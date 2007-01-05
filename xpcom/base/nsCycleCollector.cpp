/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

//
// This file implements a garbage-cycle collector based on the paper
// 
//   Concurrent Cycle Collection in Reference Counted Systems
//   Bacon & Rajan (2001), ECOOP 2001 / Springer LNCS vol 2072
//
// We are not using the concurrent or acyclic cases of that paper; so
// the green, red and orange colors are not used.
//
// The collector is based on tracking pointers of four colors:
//
// Black nodes are definitely live. If we ever determine a node is
// black, it's ok to forget about, drop from our records.
//
// White nodes are definitely garbage cycles. Once we finish with our
// scanning, we unlink all the white nodes and expect that by
// unlinking them they will self-destruct (since a garbage cycle is
// only keeping itself alive with internal links, by definition).
//
// Grey nodes are being scanned. Nodes that turn grey will turn
// either black if we determine that they're live, or white if we
// determine that they're a garbage cycle. After the main collection
// algorithm there should be no grey nodes.
//
// Purple nodes are *candidates* for being scanned. They are nodes we
// haven't begun scanning yet because they're not old enough, or we're
// still partway through the algorithm.
//
// XPCOM objects participating in garbage-cycle collection are obliged
// to inform us when they ought to turn purple; that is, when their
// refcount transitions from N+1 -> N, for nonzero N. Furthermore we
// require that *after* an XPCOM object has informed us of turning
// purple, they will tell us when they either transition back to being
// black (incremented refcount) or are ultimately deleted.


// Safety:
//
// An XPCOM object is either scan-safe or scan-unsafe, purple-safe or
// purple-unsafe.
//
// An object is scan-safe if:
//
//  - It can be QI'ed to |nsCycleCollectionParticipant|, though this
//    operation loses ISupports identity (like nsIClassInfo).
//  - The operation |traverse| on the resulting
//    nsCycleCollectionParticipant does not cause *any* refcount
//    adjustment to occur (no AddRef / Release calls).
//
// An object is purple-safe if it satisfies the following properties:
//
//  - The object is scan-safe.  
//  - If the object calls |nsCycleCollector::suspect(this)|, 
//    it will eventually call |nsCycleCollector::forget(this)|, 
//    exactly once per call to |suspect|, before being destroyed.
//
// When we receive a pointer |ptr| via
// |nsCycleCollector::suspect(ptr)|, we assume it is purple-safe. We
// can check the scan-safety, but have no way to ensure the
// purple-safety; objects must obey, or else the entire system falls
// apart. Don't involve an object in this scheme if you can't
// guarantee its purple-safety.
//
// When we have a scannable set of purple nodes ready, we begin
// our walks. During the walks, the nodes we |traverse| should only
// feed us more scan-safe nodes, and should not adjust the refcounts
// of those nodes. 
//
// We do not |AddRef| or |Release| any objects during scanning. We
// rely on purple-safety of the roots that call |suspect| and
// |forget| to hold, such that we will forget about a purple pointer
// before it is destroyed.  The pointers that are merely scan-safe,
// we hold only for the duration of scanning, and there should be no
// objects released from the scan-safe set during the scan (there
// should be no threads involved).
//
// We *do* call |AddRef| and |Release| on every white object, on
// either side of the calls to |Unlink|. This keeps the set of white
// objects alive during the unlinking.
// 

#ifdef WIN32
#include <crtdbg.h>
#include <errno.h>
#endif

#include "nsCycleCollectionParticipant.h"
#include "nsIProgrammingLanguage.h"
#include "nsBaseHashtable.h"
#include "nsHashKeys.h"
#include "nsDeque.h"
#include "nsCycleCollector.h"
#include "nsThreadUtils.h"
#include "prenv.h"
#include "prprf.h"
#include "plstr.h"
#include "prtime.h"

#include <stdio.h>
#ifdef WIN32
#include <io.h>
#include <process.h>
#endif


// Various parameters of this collector can be tuned using environment
// variables.

struct nsCycleCollectorParams
{
    PRBool mDoNothing;
    PRBool mReportStats;
    PRBool mHookMalloc;
    PRBool mDrawGraphs;
    PRBool mFaultIsFatal;
    PRBool mLogPointers;
    
    PRUint32 mScanDelay;
    
    nsCycleCollectorParams() :
        mDoNothing     (PR_GetEnv("XPCOM_CC_DO_NOTHING") != NULL),
        mReportStats   (PR_GetEnv("XPCOM_CC_REPORT_STATS") != NULL),
        mHookMalloc    (PR_GetEnv("XPCOM_CC_HOOK_MALLOC") != NULL),
        mDrawGraphs    (PR_GetEnv("XPCOM_CC_DRAW_GRAPHS") != NULL),
        mFaultIsFatal  (PR_GetEnv("XPCOM_CC_FAULT_IS_FATAL") != NULL),
        mLogPointers   (PR_GetEnv("XPCOM_CC_LOG_POINTERS") != NULL),

        // The default number of collections to "age" candidate
        // pointers in the purple buffer before we decide that any
        // garbage cycle they're in has stabilized and we want to
        // consider scanning it.
        //
        // Making this number smaller causes:
        //   - More time to be spent in the collector (bad)
        //   - Less delay between forming garbage and collecting it (good)

        mScanDelay(10)
    {
        char *s = PR_GetEnv("XPCOM_CC_SCAN_DELAY");
        if (s)
            PR_sscanf(s, "%d", &mScanDelay);
    }
};

// Various operations involving the collector are recorded in a
// statistics table. These are for diagnostics.

struct nsCycleCollectorStats
{
    PRUint32 mFailedQI;
    PRUint32 mSuccessfulQI;

    PRUint32 mVisitedNode;
    PRUint32 mVisitedJSNode;
    PRUint32 mWalkedGraph;
    PRUint32 mCollectedBytes;
    PRUint32 mFreeCalls;
    PRUint32 mFreedBytes;

    PRUint32 mSetColorGrey;
    PRUint32 mSetColorBlack;
    PRUint32 mSetColorWhite;

    PRUint32 mFailedUnlink;
    PRUint32 mCollectedNode;
    PRUint32 mBumpGeneration;
    PRUint32 mZeroGeneration;

    PRUint32 mSuspectNode;
    PRUint32 mSpills;    
    PRUint32 mForgetNode;
    PRUint32 mFreedWhilePurple;
  
    PRUint32 mCollection;

    nsCycleCollectorStats()
    {
        memset(this, 0, sizeof(nsCycleCollectorStats));
    }
  
    void Dump()
    {
        fprintf(stderr, "\f\n");
#define DUMP(entry) fprintf(stderr, "%30.30s: %-20.20d\n", #entry, entry)
        DUMP(mFailedQI);
        DUMP(mSuccessfulQI);
    
        DUMP(mVisitedNode);
        DUMP(mVisitedJSNode);
        DUMP(mWalkedGraph);
        DUMP(mCollectedBytes);
        DUMP(mFreeCalls);
        DUMP(mFreedBytes);
    
        DUMP(mSetColorGrey);
        DUMP(mSetColorBlack);
        DUMP(mSetColorWhite);
    
        DUMP(mFailedUnlink);
        DUMP(mCollectedNode);
        DUMP(mBumpGeneration);
        DUMP(mZeroGeneration);
    
        DUMP(mSuspectNode);
        DUMP(mSpills);
        DUMP(mForgetNode);
        DUMP(mFreedWhilePurple);
    
        DUMP(mCollection);
#undef DUMP
    }
};

static PRBool
nsCycleCollector_shouldSuppress(nsISupports *s);

////////////////////////////////////////////////////////////////////////
// Base types
////////////////////////////////////////////////////////////////////////

enum NodeColor { black, white, grey };

struct PtrInfo
{
    NodeColor mColor;
    PRUint32 mLang;
    size_t mInternalRefs;
    size_t mRefCount;
    size_t mBytes;
    const char *mName;

    PtrInfo() 
        : mColor(black),
          mLang(nsIProgrammingLanguage::CPLUSPLUS),
          mInternalRefs(0), 
          mRefCount(0),
          mBytes(0), 
          mName(nsnull)
    {}
    
    PtrInfo(PRUint32 gen, NodeColor col) 
        : mColor(col),
          mLang(nsIProgrammingLanguage::CPLUSPLUS),
          mInternalRefs(0), 
          mRefCount(0),
          mBytes(0), 
          mName(nsnull)
    {}
};


typedef nsBaseHashtable<nsClearingVoidPtrHashKey, PRUint32, PRUint32> PointerSet;
typedef nsBaseHashtable<nsClearingVoidPtrHashKey, PtrInfo, PtrInfo> GCTable;

static void
Fault(const char *msg, const void *ptr);

struct nsPurpleBuffer
{

#define ASSOCIATIVITY 2
#define INDEX_LOW_BIT 6
#define N_INDEX_BITS 13

#define N_ENTRIES (1 << N_INDEX_BITS)
#define N_POINTERS (N_ENTRIES * ASSOCIATIVITY)
#define TOTAL_BYTES (N_POINTERS * PR_BYTES_PER_WORD)
#define INDEX_MASK PR_BITMASK(N_INDEX_BITS)
#define POINTER_INDEX(P) ((((PRUword)P) >> INDEX_LOW_BIT) & (INDEX_MASK))

#if (INDEX_LOW_BIT + N_INDEX_BITS > (8 * PR_BYTES_PER_WORD))
#error "index bit overflow"
#endif

    // This class serves as a generational wrapper around a pldhash
    // table: a subset of generation zero lives in mCache, the
    // remainder spill into the mBackingStore hashtable. The idea is
    // to get a higher hit rate and greater locality of reference for
    // generation zero, in which the vast majority of suspect/forget
    // calls annaihilate one another.

    nsCycleCollectorParams &mParams;
    nsCycleCollectorStats &mStats;
    void* mCache[N_POINTERS];
    PRUint32 mCurrGen;    
    PointerSet mBackingStore;
    nsDeque *mTransferBuffer;
    
    nsPurpleBuffer(nsCycleCollectorParams &params,
                   nsCycleCollectorStats &stats) 
        : mParams(params),
          mStats(stats),
          mTransferBuffer(nsnull)
    {
        memset(mCache, 0, sizeof(mCache));
        mBackingStore.Init();
    }

    ~nsPurpleBuffer()
    {
        memset(mCache, 0, sizeof(mCache));
        mBackingStore.Clear();
    }

    void BumpGeneration();
    void SelectAgedPointers(nsDeque *transferBuffer);

    PRBool Exists(void *p)
    {
        PRUint32 idx = POINTER_INDEX(p);
        for (PRUint32 i = 0; i < ASSOCIATIVITY; ++i) {
            if (mCache[idx+i] == p)
                return PR_TRUE;
        }
        PRUint32 gen;
        return mBackingStore.Get(p, &gen);
    }

    void Put(void *p)
    {
        PRUint32 idx = POINTER_INDEX(p);
        for (PRUint32 i = 0; i < ASSOCIATIVITY; ++i) {
            if (!mCache[idx+i]) {
                mCache[idx+i] = p;
                return;
            }
        }
        mStats.mSpills++;
        SpillOne(p);
    }

    void Remove(void *p)     
    {
        PRUint32 idx = POINTER_INDEX(p);
        for (PRUint32 i = 0; i < ASSOCIATIVITY; ++i) {
            if (mCache[idx+i] == p) {
                mCache[idx+i] = (void*)0;
                return;
            }
        }
        mBackingStore.Remove(p);
    }

    void SpillOne(void* &p)
    {
        mBackingStore.Put(p, mCurrGen);
        p = (void*)0;
    }

    void SpillAll()
    {
        for (PRUint32 i = 0; i < N_POINTERS; ++i) {
            if (mCache[i]) {
                SpillOne(mCache[i]);
            }
        }
    }
};

static PR_CALLBACK PLDHashOperator
zeroGenerationCallback(const void*  ptr,
                       PRUint32&    generation,
                       void*        userArg)
{
    nsPurpleBuffer *purp = NS_STATIC_CAST(nsPurpleBuffer*, userArg);
    purp->mStats.mZeroGeneration++;
    generation = 0;
    return PL_DHASH_NEXT;
}

void nsPurpleBuffer::BumpGeneration()
{
    SpillAll();
    if (mCurrGen == 0xffffffff) {
        mBackingStore.Enumerate(zeroGenerationCallback, this);
        mCurrGen = 0;
    } else {
        ++mCurrGen;
    }
    mStats.mBumpGeneration++;
}

static inline PRBool
SufficientlyAged(PRUint32 generation, nsPurpleBuffer *p)
{
    return generation + p->mParams.mScanDelay < p->mCurrGen;
}

static PR_CALLBACK PLDHashOperator
ageSelectionCallback(const void*  ptr,
                     PRUint32&    generation,
                     void*        userArg)
{
    nsPurpleBuffer *purp = NS_STATIC_CAST(nsPurpleBuffer*, userArg);
    if (SufficientlyAged(generation, purp)) {
        nsISupports *root = NS_STATIC_CAST(nsISupports *, 
                                           NS_CONST_CAST(void*, ptr));
        purp->mTransferBuffer->Push(root);
    }
    return PL_DHASH_NEXT;
}

void
nsPurpleBuffer::SelectAgedPointers(nsDeque *transferBuffer)
{
    mTransferBuffer = transferBuffer;
    mBackingStore.Enumerate(ageSelectionCallback, this);
    mTransferBuffer = nsnull;
}



struct nsCycleCollector
{
    PRUint32 mTick;
    PRTime mLastAging;
    PRBool mCollectionInProgress;
    PRBool mScanInProgress;

    GCTable mGraph;
    nsCycleCollectionLanguageRuntime *mRuntimes[nsIProgrammingLanguage::MAX+1];

    // The set of buffers |mBufs| serves a variety of purposes; mostly
    // involving the transfer of pointers from a hashtable iterator
    // routine to some outer logic that might also need to mutate the
    // hashtable. In some contexts, only buffer 0 is used (as a
    // set-of-all-pointers); in other contexts, one buffer is used
    // per-language (as a set-of-pointers-in-language-N).

    nsDeque *mBufs[nsIProgrammingLanguage::MAX+1];
    
    nsCycleCollectorParams mParams;
    nsCycleCollectorStats mStats;    

    nsPurpleBuffer mPurpleBuf;
    FILE *mPtrLog;
    
    void MaybeDrawGraphs();
    void RegisterRuntime(PRUint32 langID, 
                         nsCycleCollectionLanguageRuntime *rt);
    void ForgetRuntime(PRUint32 langID);

    void CollectPurple();
    void MarkRoots();
    void ScanRoots();
    void CollectWhite();
    void ForgetAll();

    nsCycleCollector();
    ~nsCycleCollector();

    void Suspect(nsISupports *n);
    void Forget(nsISupports *n);
    void Allocated(void *n, size_t sz);
    void Freed(void *n);
    void Collect();
};


class GraphWalker :
    public nsCycleCollectionTraversalCallback
{
    nsDeque mQueue;
    void *mCurrPtr;
    PtrInfo mCurrPi;

protected:
    GCTable &mGraph;
    nsCycleCollectionLanguageRuntime **mRuntimes;

public:
    
    GraphWalker(GCTable & tab,
                nsCycleCollectionLanguageRuntime **runtimes) : 
        mQueue(nsnull),
        mCurrPtr(nsnull),
        mGraph(tab),
        mRuntimes(runtimes)
    {}

    virtual ~GraphWalker() 
    {}
   
    void Walk(void *s0);

    // nsCycleCollectionTraversalCallback methods.
    void DescribeNode(size_t refCount, size_t objSz, const char *objName);
    void NoteXPCOMChild(nsISupports *child);
    void NoteScriptChild(PRUint32 langID, void *child);

    // Provided by concrete walker subtypes.
    virtual PRBool ShouldVisitNode(void *p, PtrInfo const & pi) = 0;
    virtual void VisitNode(void *p, PtrInfo & pi, size_t refcount) = 0;
    virtual void NoteChild(void *c, PtrInfo & childpi) = 0;
};


////////////////////////////////////////////////////////////////////////
// The static collector object
////////////////////////////////////////////////////////////////////////


static int sCollectorConstructed = 0;
static nsCycleCollector sCollector;


////////////////////////////////////////////////////////////////////////
// Utility functions
////////////////////////////////////////////////////////////////////////

struct safetyCallback :     
    public nsCycleCollectionTraversalCallback
{
    // This is just a dummy interface to feed to children when we're
    // called, to force potential segfaults to happen early, so gdb
    // can give us an informative stack trace. If we don't use it, the
    // collector runs faster but segfaults happen after pointers have
    // been queued and dequeued, at which point their owner is
    // obscure.
    void DescribeNode(size_t refCount, size_t objSz, const char *objName) {}
    void NoteXPCOMChild(nsISupports *child) {}
    void NoteScriptChild(PRUint32 langID, void *child) {}
};

static safetyCallback sSafetyCallback;


static inline void
EnsurePtrInfo(GCTable & tab, void *n, PtrInfo & pi)
{
    if (!tab.Get(n, &pi))
        tab.Put(n, pi);
}


static void
Fault(const char *msg, const void *ptr=nsnull)
{
    // This should be nearly impossible, but just in case.
    if (!sCollectorConstructed)
        return;

    if (sCollector.mParams.mFaultIsFatal) {

        if (ptr)
            printf("Fatal fault in cycle collector: %s (ptr: %p)\n", msg, ptr);
        else
            printf("Fatal fault in cycle collector: %s\n", msg);

        exit(1);

    } 

    // When faults are not fatal, we assume we're running in a
    // production environment and we therefore want to disable the
    // collector on a fault. This will unfortunately cause the browser
    // to leak pretty fast wherever creates cyclical garbage, but it's
    // probably a better user experience than crashing. Besides, we
    // *should* never hit a fault.

    sCollector.mParams.mDoNothing = PR_TRUE;
}


void 
GraphWalker::DescribeNode(size_t refCount, size_t objSz, const char *objName)
{
    if (refCount == 0)
        Fault("zero refcount", mCurrPtr);

    mCurrPi.mBytes = objSz;
    mCurrPi.mName = objName;
    this->VisitNode(mCurrPtr, mCurrPi, refCount);
    sCollector.mStats.mVisitedNode++;
    if (mCurrPi.mLang == nsIProgrammingLanguage::JAVASCRIPT)
        sCollector.mStats.mVisitedJSNode++;
}


void 
GraphWalker::NoteXPCOMChild(nsISupports *child) 
{
    if (!child)
        Fault("null XPCOM pointer returned"); 
   
    if (nsCycleCollector_isScanSafe(child) && 
        !nsCycleCollector_shouldSuppress(child)) {
        PtrInfo childPi;
        EnsurePtrInfo(mGraph, child, childPi);
        this->NoteChild(child, childPi);
        mRuntimes[nsIProgrammingLanguage::CPLUSPLUS]->Traverse(child, sSafetyCallback);
        mQueue.Push(child);
    }
}


void
GraphWalker::NoteScriptChild(PRUint32 langID, void *child) 
{
    if (!child)
        Fault("null script language pointer returned");
    
    if (!mRuntimes[langID])
        Fault("traversing pointer for unregistered language", child);
   
    PtrInfo childPi;
    childPi.mLang = langID;
    EnsurePtrInfo(mGraph, child, childPi);
    this->NoteChild(child, childPi);
    mRuntimes[langID]->Traverse(child, sSafetyCallback);
    mQueue.Push(child);
}


void
GraphWalker::Walk(void *s0)
{
    mQueue.Empty();
    mQueue.Push(s0);

    while (mQueue.GetSize() > 0) {

        mCurrPtr = mQueue.Pop();
        nsresult rv;

        if (!mGraph.Get(mCurrPtr, &mCurrPi)) {
            Fault("unknown pointer", mCurrPtr);
            continue;
        }

        if (mCurrPi.mLang >nsIProgrammingLanguage::MAX ) {
            Fault("unknown language during walk");
            continue;
        }

        if (!mRuntimes[mCurrPi.mLang]) {
            Fault("script pointer for unregistered language");
            continue;
        }
        
        if (this->ShouldVisitNode(mCurrPtr, mCurrPi)) {

            rv = mRuntimes[mCurrPi.mLang]->Traverse(mCurrPtr, *this);
            if (NS_FAILED(rv)) {
                Fault("script pointer traversal failed", mCurrPtr);
            }
        }
    }
    sCollector.mStats.mWalkedGraph++;
}


////////////////////////////////////////////////////////////////////////
// Bacon & Rajan's |MarkRoots| routine.
////////////////////////////////////////////////////////////////////////


struct MarkGreyWalker : public GraphWalker
{
    MarkGreyWalker(GCTable &tab,
                   nsCycleCollectionLanguageRuntime **runtimes)
        : GraphWalker(tab, runtimes) 
    {}

    PRBool ShouldVisitNode(void *p, PtrInfo const & pi)  
    { 
        return pi.mColor != grey; 
    }

    void VisitNode(void *p, PtrInfo & pi, size_t refcount) 
    { 
        pi.mColor = grey; 
        pi.mRefCount = refcount;
        sCollector.mStats.mSetColorGrey++;
        mGraph.Put(p, pi);
    }

    void NoteChild(void *c, PtrInfo & childpi)
    { 
        childpi.mInternalRefs++; 
        mGraph.Put(c, childpi);
    }
};

void 
nsCycleCollector::CollectPurple()
{
    mBufs[0]->Empty();
    mPurpleBuf.SelectAgedPointers(mBufs[0]);
}

void
nsCycleCollector::MarkRoots()
{
    int i;
    for (i = 0; i < mBufs[0]->GetSize(); ++i) {
        PtrInfo pi;
        nsISupports *s = NS_STATIC_CAST(nsISupports *, mBufs[0]->ObjectAt(i));
        EnsurePtrInfo(mGraph, s, pi);
        MarkGreyWalker(mGraph, mRuntimes).Walk(s);
    }
}


////////////////////////////////////////////////////////////////////////
// Bacon & Rajan's |ScanRoots| routine.
////////////////////////////////////////////////////////////////////////


struct ScanBlackWalker : public GraphWalker
{
    ScanBlackWalker(GCTable &tab,
                   nsCycleCollectionLanguageRuntime **runtimes)
        : GraphWalker(tab, runtimes) 
    {}

    PRBool ShouldVisitNode(void *p, PtrInfo const & pi) 
    { 
        return pi.mColor != black; 
    }

    void VisitNode(void *p, PtrInfo & pi, size_t refcount) 
    { 
        pi.mColor = black; 
        sCollector.mStats.mSetColorBlack++;
        mGraph.Put(p, pi);
    }

    void NoteChild(void *c, PtrInfo & childpi) {}
};


struct scanWalker : public GraphWalker
{
    scanWalker(GCTable &tab,
               nsCycleCollectionLanguageRuntime **runtimes)
        : GraphWalker(tab, runtimes) 
    {}

    PRBool ShouldVisitNode(void *p, PtrInfo const & pi) 
    { 
        return pi.mColor == grey; 
    }

    void VisitNode(void *p, PtrInfo & pi, size_t refcount) 
    {
        if (pi.mColor != grey)
            Fault("scanning non-grey node", p);

        if (pi.mInternalRefs > refcount)
            Fault("traversed refs exceed refcount", p);

        if (pi.mInternalRefs == refcount) {
            pi.mColor = white;
            sCollector.mStats.mSetColorWhite++;
        } else {
            ScanBlackWalker(mGraph, mRuntimes).Walk(p);
            pi.mColor = black;
            sCollector.mStats.mSetColorBlack++;
        }
        mGraph.Put(p, pi);
    }
    void NoteChild(void *c, PtrInfo & childpi) {}
};


static PR_CALLBACK PLDHashOperator
NoGreyCallback(const void*  ptr,
               PtrInfo&     pinfo,
               void*        userArg)
{
    if (pinfo.mColor == grey)
        Fault("valid grey node after scanning", ptr);
    return PL_DHASH_NEXT;
}


void
nsCycleCollector::ScanRoots()
{
    int i;

    for (i = 0; i < mBufs[0]->GetSize(); ++i) {
        nsISupports *s = NS_STATIC_CAST(nsISupports *, mBufs[0]->ObjectAt(i));
        scanWalker(mGraph, mRuntimes).Walk(s); 
    }

    // Sanity check: scan should have colored all grey nodes black or
    // white. So we ensure we have no grey nodes at this point.
    mGraph.Enumerate(NoGreyCallback, this);
}


////////////////////////////////////////////////////////////////////////
// Bacon & Rajan's |CollectWhite| routine, somewhat modified.
////////////////////////////////////////////////////////////////////////


static PR_CALLBACK PLDHashOperator
FindWhiteCallback(const void*  ptr,
                  PtrInfo&     pinfo,
                  void*        userArg)
{
    nsCycleCollector *collector = NS_STATIC_CAST(nsCycleCollector*, 
                                                 userArg);
    if (pinfo.mColor == white) {
        nsISupports *s = NS_STATIC_CAST(nsISupports *, 
                                        NS_CONST_CAST(void*, ptr));
        if (pinfo.mLang  > nsIProgrammingLanguage::MAX)
            Fault("White node has bad language ID", s);
        else
            collector->mBufs[pinfo.mLang]->Push(s);
    }
    return PL_DHASH_NEXT;
}


void
nsCycleCollector::CollectWhite()
{
    // Explanation of "somewhat modified": we have no way to collect the
    // set of whites "all at once", we have to ask each of them to drop
    // their outgoing links and assume this will cause the garbage cycle
    // to *mostly* self-destruct (except for the reference we continue
    // to hold). 
    // 
    // To do this "safely" we must make sure that the white nodes we're
    // operating on are stable for the duration of our operation. So we
    // make 3 sets of calls to language runtimes:
    //
    //   - Root(whites), which should pin the whites in memory.
    //   - Unlink(whites), which drops outgoing links on each white.
    //   - Unroot(whites), which returns the whites to normal GC.

    PRUint32 i;
    nsresult rv;

    for (i = 0; i < nsIProgrammingLanguage::MAX+1; ++i)
        mBufs[i]->Empty();

#ifdef WIN32
    struct _CrtMemState ms1, ms2;
    _CrtMemCheckpoint(&ms1);
#endif // WIN32

    mGraph.Enumerate(FindWhiteCallback, this);

    for (i = 0; i < nsIProgrammingLanguage::MAX+1; ++i) {
        if (mRuntimes[i] &&
            mBufs[i]->GetSize() > 0) {
            rv = mRuntimes[i]->Root(*mBufs[i]);
            if (NS_FAILED(rv))
                Fault("Failed root call while unlinking");
        }
    }

    for (i = 0; i < nsIProgrammingLanguage::MAX+1; ++i) {
        if (mRuntimes[i] &&
            mBufs[i]->GetSize() > 0) {
            rv = mRuntimes[i]->Unlink(*mBufs[i]);
            if (NS_FAILED(rv)) {
                Fault("Failed unlink call while unlinking");
                mStats.mFailedUnlink++;
            } else {
                mStats.mCollectedNode += mBufs[i]->GetSize();
            }
        }
    }

    for (i = 0; i < nsIProgrammingLanguage::MAX+1; ++i) {
        if (mRuntimes[i] &&
            mBufs[i]->GetSize() > 0) {
            rv = mRuntimes[i]->Unroot(*mBufs[i]);
            if (NS_FAILED(rv))
                Fault("Failed unroot call while unlinking");
        }
    }

    for (i = 0; i < nsIProgrammingLanguage::MAX+1; ++i)
        mBufs[i]->Empty();

#ifdef WIN32
    _CrtMemCheckpoint(&ms2);
    if (ms2.lTotalCount < ms1.lTotalCount)
        mStats.mFreedBytes += (ms1.lTotalCount - ms2.lTotalCount);
#endif // WIN32
}


////////////////////////////////////////////////////////////////////////
// Implement the LanguageRuntime interface for C++/XPCOM 
////////////////////////////////////////////////////////////////////////


struct nsCycleCollectionXPCOMRuntime : 
    public nsCycleCollectionLanguageRuntime 
{
    nsresult BeginCycleCollection() 
    {
        return NS_OK;
    }

    nsresult Traverse(void *p, nsCycleCollectionTraversalCallback &cb) 
    {
        nsresult rv;

        // We use QI to move from an nsISupports to an
        // nsCycleCollectionParticipant, which is a per-class
        // singleton helper object that implements traversal and
        // unlinking logic for the nsISupports in question.

        nsISupports *s = NS_STATIC_CAST(nsISupports *, p);        
        nsCOMPtr<nsCycleCollectionParticipant> cp = do_QueryInterface(s, &rv);
        if (NS_FAILED(rv)) {
            Fault("walking wrong type of pointer", s);
            return NS_ERROR_FAILURE;
        }

        sCollector.mStats.mSuccessfulQI++;

        rv = cp->Traverse(s, cb);
        if (NS_FAILED(rv)) {
            Fault("XPCOM pointer traversal failed", s);
            return NS_ERROR_FAILURE;
        }
        return NS_OK;
    }

    nsresult Root(const nsDeque &nodes)
    {
        for (PRInt32 i = 0; i < nodes.GetSize(); ++i) {
            void *p = nodes.ObjectAt(i);
            nsISupports *s = NS_STATIC_CAST(nsISupports *, p);
            NS_ADDREF(s);
        }
        return NS_OK;
    }

    nsresult Unlink(const nsDeque &nodes)
    {
        nsresult rv;

        for (PRInt32 i = 0; i < nodes.GetSize(); ++i) {
            void *p = nodes.ObjectAt(i);

            nsISupports *s = NS_STATIC_CAST(nsISupports *, p);
            nsCOMPtr<nsCycleCollectionParticipant> cp 
                = do_QueryInterface(s, &rv);

            if (NS_FAILED(rv)) {
                Fault("unlinking wrong kind of pointer", s);
                return NS_ERROR_FAILURE;
            }

            sCollector.mStats.mSuccessfulQI++;
            rv = cp->Unlink(s);

            if (NS_FAILED(rv)) {
                Fault("failed unlink", s);
                return NS_ERROR_FAILURE;
            }
        }
        return NS_OK;
    }

    nsresult Unroot(const nsDeque &nodes)
    {
        for (PRInt32 i = 0; i < nodes.GetSize(); ++i) {
            void *p = nodes.ObjectAt(i);
            nsISupports *s = NS_STATIC_CAST(nsISupports *, p);
            NS_RELEASE(s);
        }
        return NS_OK;
    }

    nsresult FinishCycleCollection() 
    {
        return NS_OK;
    }
};


////////////////////////////////////////////////////////////////////////
// Extra book-keeping functions.
////////////////////////////////////////////////////////////////////////

static PR_CALLBACK PLDHashOperator
ForgetAllCallback(const void*  ptr,
                  PtrInfo&     pinfo,
                  void*        userArg)
{
    nsCycleCollector *collector = NS_STATIC_CAST(nsCycleCollector*, 
                                                 userArg);
    
    nsISupports *root = NS_STATIC_CAST(nsISupports *, 
                                       NS_CONST_CAST(void*, ptr));
    collector->mBufs[0]->Push(root);
    return PL_DHASH_NEXT;
}


void
nsCycleCollector::ForgetAll()
{  
    mBufs[0]->Empty();
    mGraph.Enumerate(ForgetAllCallback, this);
    while (mBufs[0]->GetSize() > 0) {
        nsISupports *s = NS_STATIC_CAST(nsISupports *, mBufs[0]->Pop());
        Forget(s);
    }
    mBufs[0]->Empty();
}


struct graphVizWalker : public GraphWalker
{
    // We can't just use _popen here because graphviz-for-windows
    // doesn't set up its stdin stream properly, sigh.
    PointerSet mVisited;
    void *mParent;
    FILE *mStream;

    graphVizWalker(GCTable &tab,
                   nsCycleCollectionLanguageRuntime **runtimes)
        : GraphWalker(tab, runtimes), 
          mParent(nsnull), 
          mStream(nsnull)        
    {
#ifdef WIN32
        mStream = fopen("c:\\cycle-graph.dot", "w+");
#else
        mStream = popen("dotty -", "w");
#endif
        mVisited.Init();
        fprintf(mStream, 
                "digraph collection {\n"
                "rankdir=LR\n"
                "node [fontname=fixed, fontsize=10, style=filled, shape=box]\n"
                );
    }

    ~graphVizWalker()
    {
        fprintf(mStream, "\n}\n");
#ifdef WIN32
        fclose(mStream);
        // Even dotty doesn't work terribly well on windows, since
        // they execute lefty asynchronously. So we'll just run 
        // lefty ourselves.
        _spawnlp(_P_WAIT, 
                 "lefty", 
                 "lefty",
                 "-e",
                 "\"load('dotty.lefty');"
                 "dotty.simple('c:\\cycle-graph.dot');\"",
                 NULL);
        unlink("c:\\cycle-graph.dot");
#else
        pclose(mStream);
#endif
    }

    PRBool ShouldVisitNode(void *p, PtrInfo const & pi)  
    { 
        PRUint32 dummy;
        return ! mVisited.Get(p, &dummy);
    }

    void VisitNode(void *p, PtrInfo & pi, size_t refcount) 
    {
        PRUint32 dummy = 0;
        mVisited.Put(p, dummy);
        mParent = p;
        fprintf(mStream, 
                "n%p [label=\"%s\\n%p\\n%u/%u refs found\", "
                "fillcolor=%s, fontcolor=%s]\n", 
                p, pi.mName, 
                p,
                pi.mInternalRefs, pi.mRefCount, 
                (pi.mColor == black ? "black" : "white"),
                (pi.mColor == black ? "white" : "black"));
    }

    void NoteChild(void *c, PtrInfo & childpi)
    { 
        fprintf(mStream, "n%p -> n%p\n", mParent, c);
    }
};


////////////////////////////////////////////////////////////////////////
// Memory-hooking stuff
// When debugging wild pointers, it sometimes helps to hook malloc and
// free. This stuff is disabled unless you set an environment variable.
////////////////////////////////////////////////////////////////////////

static PRBool hookedMalloc = PR_FALSE;

#ifdef __GLIBC__
#include <malloc.h>

static void* (*old_memalign_hook)(size_t, size_t, const void *);
static void* (*old_realloc_hook)(void *, size_t, const void *);
static void* (*old_malloc_hook)(size_t, const void *);
static void (*old_free_hook)(void *, const void *);

static void* my_memalign_hook(size_t, size_t, const void *);
static void* my_realloc_hook(void *, size_t, const void *);
static void* my_malloc_hook(size_t, const void *);
static void my_free_hook(void *, const void *);

static inline void 
install_old_hooks()
{
    __memalign_hook = old_memalign_hook;
    __realloc_hook = old_realloc_hook;
    __malloc_hook = old_malloc_hook;
    __free_hook = old_free_hook;
}

static inline void 
save_old_hooks()
{
    // Glibc docs recommend re-saving old hooks on
    // return from recursive calls. Strangely when 
    // we do this, we find ourselves in infinite
    // recursion.

    //     old_memalign_hook = __memalign_hook;
    //     old_realloc_hook = __realloc_hook;
    //     old_malloc_hook = __malloc_hook;
    //     old_free_hook = __free_hook;
}

static inline void 
install_new_hooks()
{
    __memalign_hook = my_memalign_hook;
    __realloc_hook = my_realloc_hook;
    __malloc_hook = my_malloc_hook;
    __free_hook = my_free_hook;
}

static void*
my_realloc_hook(void *ptr, size_t size, const void *caller)
{
    void *result;    

    install_old_hooks();
    result = realloc(ptr, size);
    save_old_hooks();

    if (sCollectorConstructed)
        sCollector.Freed(ptr);

    if (sCollectorConstructed)
        sCollector.Allocated(result, size);

    install_new_hooks();

    return result;
}


static void* 
my_memalign_hook(size_t size, size_t alignment, const void *caller)
{
    void *result;    

    install_old_hooks();
    result = memalign(size, alignment);
    save_old_hooks();

    if (sCollectorConstructed)
        sCollector.Allocated(result, size);

    install_new_hooks();

    return result;
}


static void 
my_free_hook (void *ptr, const void *caller)
{
    install_old_hooks();
    free(ptr);
    save_old_hooks();

    if (sCollectorConstructed)
        sCollector.Freed(ptr);

    install_new_hooks();
}      


static void*
my_malloc_hook (size_t size, const void *caller)
{
    void *result;    

    install_old_hooks();
    result = malloc (size);
    save_old_hooks();

    if (sCollectorConstructed)
        sCollector.Allocated(result, size);

    install_new_hooks();

    return result;
}


static void 
InitMemHook(void)
{
    if (!hookedMalloc) {
        save_old_hooks();
        install_new_hooks();
        hookedMalloc = PR_TRUE;        
    }
}

void (*__malloc_initialize_hook) (void) = InitMemHook;


#elif defined(WIN32)

static int 
AllocHook(int allocType, void *userData, size_t size, int 
          blockType, long requestNumber, const unsigned char *filename, int 
          lineNumber)
{
    if (allocType == _HOOK_FREE)
        sCollector.Freed(userData);
    return 1;
}


static void InitMemHook(void)
{
    if (!hookedMalloc) {
        _CrtSetAllocHook (AllocHook);
        hookedMalloc = PR_TRUE;        
    }
}


#elif 0 // defined(XP_MACOSX)

#include <malloc/malloc.h>

static void (*old_free)(struct _malloc_zone_t *zone, void *ptr);

static void
freehook(struct _malloc_zone_t *zone, void *ptr)
{
    sCollector.Freed(ptr);
    old_free(zone, ptr);
}


static void
InitMemHook(void)
{
    if (!hookedMalloc) {
        malloc_zone_t *default_zone = malloc_default_zone();
        old_free = default_zone->free;
        default_zone->free = freehook;
        hookedMalloc = PR_TRUE;
    }
}


#else

static void
InitMemHook(void)
{
}

#endif // GLIBC / WIN32 / OSX


////////////////////////////////////////////////////////////////////////
// Collector implementation
////////////////////////////////////////////////////////////////////////

nsCycleCollector::nsCycleCollector() : 
    mTick(0),
    mLastAging(0),
    mCollectionInProgress(PR_FALSE),
    mScanInProgress(PR_FALSE),
    mPurpleBuf(mParams, mStats),
    mPtrLog(nsnull)
{
    mGraph.Init();

    for (PRUint32 i = 0; i <= nsIProgrammingLanguage::MAX; ++i) {
        mRuntimes[i] = nsnull;
        mBufs[i] = new nsDeque(nsnull);
    }

    mRuntimes[nsIProgrammingLanguage::CPLUSPLUS] 
        = new nsCycleCollectionXPCOMRuntime();

    sCollectorConstructed = 1;
}


nsCycleCollector::~nsCycleCollector()
{
    sCollectorConstructed = 0;

    mGraph.Clear();    

    for (PRUint32 i = 0; i < nsIProgrammingLanguage::MAX+1; ++i) {
        delete mBufs[i];
        mBufs[i] = NULL;
    }

    delete mRuntimes[nsIProgrammingLanguage::CPLUSPLUS];
    mRuntimes[nsIProgrammingLanguage::CPLUSPLUS] = NULL;

    for (PRUint32 i = 0; i < nsIProgrammingLanguage::MAX+1; ++i) {
        mRuntimes[i] = NULL;
    }
}


void 
nsCycleCollector::RegisterRuntime(PRUint32 langID, 
                                  nsCycleCollectionLanguageRuntime *rt)
{
    if (mParams.mDoNothing)
        return;

    if (langID > nsIProgrammingLanguage::MAX)
        Fault("unknown language runtime in registration");

    if (mRuntimes[langID])
        Fault("multiple registrations of langauge runtime", rt);

    mRuntimes[langID] = rt;
}


void 
nsCycleCollector::ForgetRuntime(PRUint32 langID)
{
    if (mParams.mDoNothing)
        return;

    if (langID > nsIProgrammingLanguage::MAX)
        Fault("unknown language runtime in deregistration");

    if (! mRuntimes[langID])
        Fault("forgetting non-registered language runtime");

    mRuntimes[langID] = nsnull;
}


void 
nsCycleCollector::MaybeDrawGraphs()
{
    if (mParams.mDrawGraphs) {

        PRUint32 i;
        nsDeque roots(nsnull);

        while (mBufs[0]->GetSize() > 0)
            roots.Push(mBufs[0]->Pop());

        for (i = 0; i < nsIProgrammingLanguage::MAX+1; ++i)
            mBufs[i]->Empty();

        mGraph.Enumerate(FindWhiteCallback, this);

        // We draw graphs only if there were any white nodes.
        PRBool anyWhites = PR_FALSE;

        for (i = 0; i < nsIProgrammingLanguage::MAX+1; ++i) {
            if (mBufs[i]->GetSize() > 0) {
                anyWhites = PR_TRUE;
                break;
            }
        }

        if (anyWhites) {
            graphVizWalker gw(mGraph, mRuntimes);
            while (roots.GetSize() > 0) {
                nsISupports *s = NS_STATIC_CAST(nsISupports *, roots.Pop());
                gw.Walk(s);
            }
        }

        for (i = 0; i < nsIProgrammingLanguage::MAX+1; ++i)
            mBufs[i]->Empty();
    }
}

class Suppressor :
    public nsCycleCollectionTraversalCallback
{
protected:
    static char *sSuppressionList;
    static PRBool sInitialized;
    PRBool mSuppressThisNode;
public:
    Suppressor()
    {
    }

    PRBool shouldSuppress(nsISupports *s)
    {
        if (!sInitialized) {
            sSuppressionList = PR_GetEnv("XPCOM_CC_SUPPRESS");
            sInitialized = PR_TRUE;
        }
        if (sSuppressionList == nsnull) {
            mSuppressThisNode = PR_FALSE;
        } else {
            nsresult rv;
            nsCOMPtr<nsCycleCollectionParticipant> cp = do_QueryInterface(s, &rv);
            if (NS_FAILED(rv)) {
                Fault("checking suppression on wrong type of pointer", s);
                return PR_TRUE;
            }
            cp->Traverse(s, *this);
        }
        return mSuppressThisNode;
    }

    void DescribeNode(size_t refCount, size_t objSz, const char *objName)
    {
        mSuppressThisNode = (PL_strstr(sSuppressionList, objName) != nsnull);
    }

    void NoteXPCOMChild(nsISupports *child) {}
    void NoteScriptChild(PRUint32 langID, void *child) {}
};

char *Suppressor::sSuppressionList = nsnull;
PRBool Suppressor::sInitialized = PR_FALSE;

static PRBool
nsCycleCollector_shouldSuppress(nsISupports *s)
{
    Suppressor supp;
    return supp.shouldSuppress(s);
}

void 
nsCycleCollector::Suspect(nsISupports *n)
{
    mStats.mSuspectNode++;

    if (mParams.mDoNothing)
        return;

    if (!NS_IsMainThread())
        Fault("trying to suspect from non-main thread");

    if (mScanInProgress)
        Fault("tried to suspect a node during scanning", n);

    if (!nsCycleCollector_isScanSafe(n))
        Fault("suspected a non-scansafe pointer", n);    

    if (nsCycleCollector_shouldSuppress(n))
        return;

    if (mParams.mHookMalloc)
        InitMemHook();

    mPurpleBuf.Put(n);

    if (mParams.mLogPointers) {
        if (!mPtrLog)
            mPtrLog = fopen("pointer_log", "w");
        fprintf(mPtrLog, "S %p\n", NS_STATIC_CAST(void*, n));
    }
}


void 
nsCycleCollector::Forget(nsISupports *n)
{
    mStats.mForgetNode++;

    if (mParams.mDoNothing)
        return;

    if (!NS_IsMainThread())
        Fault("trying to forget from non-main thread");

    if (mScanInProgress)
        Fault("tried to forget a node during scanning", n);

    if (mParams.mHookMalloc)
        InitMemHook();

    mPurpleBuf.Remove(n);
    
    if (mParams.mLogPointers) {
        if (!mPtrLog)
            mPtrLog = fopen("pointer_log", "w");
        fprintf(mPtrLog, "F %p\n", NS_STATIC_CAST(void*, n));
    }
}

void 
nsCycleCollector::Allocated(void *n, size_t sz)
{
}

void 
nsCycleCollector::Freed(void *n)
{
    mStats.mFreeCalls++;

    if (mPurpleBuf.Exists(n)) {
        mStats.mForgetNode++;
        mStats.mFreedWhilePurple++;
        Fault("freed while purple", n);
        mPurpleBuf.Remove(n);
        
        if (mParams.mLogPointers) {
            if (!mPtrLog)
                mPtrLog = fopen("pointer_log", "w");
            fprintf(mPtrLog, "R %p\n", n);
        }
    }
}


void
nsCycleCollector::Collect()
{
    // This triggers a JS GC. Our caller assumes we always trigger at
    // least one JS GC -- they rely on this fact to avoid redundant JS
    // GC calls -- so it's essential that we actually execute this
    // step!
    
    for (PRUint32 i = 0; i <= nsIProgrammingLanguage::MAX; ++i) {
        if (mRuntimes[i])
            mRuntimes[i]->BeginCycleCollection();
    }

    if (! mParams.mDoNothing) {

        if (mParams.mHookMalloc)
            InitMemHook();
        
        CollectPurple();

        if (mBufs[0]->GetSize() == 0) {
            mPurpleBuf.BumpGeneration();
            mStats.mCollection++;
            if (mParams.mReportStats)
                mStats.Dump();

        } else {
            
            if (mCollectionInProgress)
                Fault("re-entered collection");
            
            mCollectionInProgress = PR_TRUE;
            mScanInProgress = PR_TRUE;
            
            mGraph.Clear();
            
            // The main Bacon & Rajan collection algorithm.
            
            MarkRoots();  
            ScanRoots();
            
            mScanInProgress = PR_FALSE;
            MaybeDrawGraphs();
            CollectWhite();
            ForgetAll();
            
            // Some additional book-keeping.
            
            mGraph.Clear();
            mPurpleBuf.BumpGeneration();
            mStats.mCollection++;
            if (mParams.mReportStats)
                mStats.Dump();
            mCollectionInProgress = PR_FALSE;
        }
    }

    for (PRUint32 i = 0; i <= nsIProgrammingLanguage::MAX; ++i) {
        if (mRuntimes[i])
            mRuntimes[i]->FinishCycleCollection();
    }    
}


////////////////////////////////////////////////////////////////////////
// Module public API (exported in nsCycleCollector.h)
// Just functions that redirect into the singleton, once it's built.
////////////////////////////////////////////////////////////////////////

void 
nsCycleCollector_registerRuntime(PRUint32 langID, 
                                 nsCycleCollectionLanguageRuntime *rt)
{
    if (sCollectorConstructed == 0)
        return;

    sCollector.RegisterRuntime(langID, rt);
}


void 
nsCycleCollector_forgetRuntime(PRUint32 langID)
{
    if (sCollectorConstructed == 0)
        return;

    sCollector.ForgetRuntime(langID);
}


void 
nsCycleCollector_suspect(nsISupports *n)
{
    if (sCollectorConstructed == 0)
        return;
    
    sCollector.Suspect(n);
}


void 
nsCycleCollector_forget(nsISupports *n)
{
    if (sCollectorConstructed == 0)
        return;

    sCollector.Forget(n);
}


void 
nsCycleCollector_collect()
{
    if (sCollectorConstructed == 0)
        return;

    sCollector.Collect();
}


PRBool
nsCycleCollector_isScanSafe(nsISupports *s)
{
    nsresult rv;

    if (!s)
        return PR_FALSE;

    nsCOMPtr<nsCycleCollectionParticipant> cp = do_QueryInterface(s, &rv);
    if (NS_FAILED(rv)) {
        sCollector.mStats.mFailedQI++;
        return PR_FALSE;
    }

    return PR_TRUE;
}
