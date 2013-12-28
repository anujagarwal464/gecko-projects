/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CacheIndex__h__
#define CacheIndex__h__

#include "CacheLog.h"
#include "CacheFileIOManager.h"
#include "nsIRunnable.h"
#include "CacheHashUtils.h"
#include "nsICacheEntry.h"
#include "nsILoadContextInfo.h"
#include "nsTHashtable.h"
#include "mozilla/SHA1.h"
#include "mozilla/Mutex.h"
#include "mozilla/Endian.h"
#include "mozilla/TimeStamp.h"

class nsIFile;
class nsIDirectoryEnumerator;
class nsITimer;


#ifdef DEBUG
#define DEBUG_STATS 1
#endif

namespace mozilla {
namespace net {

class CacheFileMetadata;

typedef struct {
  uint32_t mVersion;
  uint32_t mTimeStamp; // for quick entry file validation
  uint32_t mIsDirty;
} CacheIndexHeader;

struct CacheIndexRecord {
  SHA1Sum::Hash mHash;
  uint32_t      mFrecency;
  uint32_t      mExpirationTime;
  uint32_t      mAppId;

  /*
   *    1000 0000 0000 0000 0000 0000 0000 0000 : initialized
   *    0100 0000 0000 0000 0000 0000 0000 0000 : anonymous
   *    0010 0000 0000 0000 0000 0000 0000 0000 : inBrowser
   *    0001 0000 0000 0000 0000 0000 0000 0000 : removed
   *    0000 1000 0000 0000 0000 0000 0000 0000 : dirty
   *    0000 0100 0000 0000 0000 0000 0000 0000 : fresh
   *    0000 0011 0000 0000 0000 0000 0000 0000 : reserved
   *    0000 0000 1111 1111 1111 1111 1111 1111 : file size
   */
  uint32_t      mFlags;

  CacheIndexRecord()
    : mFrecency(0)
    , mExpirationTime(nsICacheEntry::NO_EXPIRATION_TIME)
    , mAppId(nsILoadContextInfo::NO_APP_ID)
    , mFlags(0)
  {}
};

class CacheIndexEntry : public PLDHashEntryHdr
{
public:
  typedef const SHA1Sum::Hash& KeyType;
  typedef const SHA1Sum::Hash* KeyTypePointer;

  CacheIndexEntry(KeyTypePointer aKey)
  {
    MOZ_COUNT_CTOR(CacheIndexEntry);
    mRec = new CacheIndexRecord();
    LOG(("CacheIndexEntry::CacheIndexEntry() - Created record [rec=%p]", mRec));
    memcpy(&mRec->mHash, aKey, sizeof(SHA1Sum::Hash));
  }
  CacheIndexEntry(const CacheIndexEntry& aOther)
  {
    NS_NOTREACHED("CacheIndexEntry copy constructor is forbidden!");
  }
  ~CacheIndexEntry()
  {
    MOZ_COUNT_DTOR(CacheIndexEntry);
    LOG(("CacheIndexEntry::~CacheIndexEntry() - Deleting record [rec=%p]",
         mRec));
    delete mRec;
  }

  // KeyEquals(): does this entry match this key?
  bool KeyEquals(KeyTypePointer aKey) const
  {
    return memcmp(&mRec->mHash, aKey, sizeof(SHA1Sum::Hash)) == 0;
  }

  // KeyToPointer(): Convert KeyType to KeyTypePointer
  static KeyTypePointer KeyToPointer(KeyType aKey) { return &aKey; }

  // HashKey(): calculate the hash number
  static PLDHashNumber HashKey(KeyTypePointer aKey)
  {
    return (reinterpret_cast<const uint32_t *>(aKey))[0];
  }

  // ALLOW_MEMMOVE can we move this class with memmove(), or do we have
  // to use the copy constructor?
  enum { ALLOW_MEMMOVE = true };

  bool operator==(const CacheIndexEntry& aOther) const
  {
    return KeyEquals(&aOther.mRec->mHash);
  }

  CacheIndexEntry& operator=(const CacheIndexEntry& aOther)
  {
    MOZ_ASSERT(memcmp(&mRec->mHash, &aOther.mRec->mHash,
               sizeof(SHA1Sum::Hash)) == 0);
    mRec->mFrecency = aOther.mRec->mFrecency;
    mRec->mExpirationTime = aOther.mRec->mExpirationTime;
    mRec->mAppId = aOther.mRec->mAppId;
    mRec->mFlags = aOther.mRec->mFlags;
    return *this;
  }

  void InitNew()
  {
    mRec->mFrecency = 0;
    mRec->mExpirationTime = nsICacheEntry::NO_EXPIRATION_TIME;
    mRec->mAppId = nsILoadContextInfo::NO_APP_ID;
    mRec->mFlags = 0;
  }

  void Init(uint32_t aAppId, bool aAnonymous, bool aInBrowser)
  {
    mRec->mAppId = aAppId;
    if (aAnonymous)
      mRec->mFlags |= eAnonymousMask;
    if (aInBrowser)
      mRec->mFlags |= eInBrowserMask;

    mRec->mFlags |= eInitializedMask;
  }

  const SHA1Sum::Hash * Hash() { return &mRec->mHash; }

  bool IsInitialized() { return !!(mRec->mFlags & eInitializedMask); }

  uint32_t AppId() { return mRec->mAppId; }
  bool     Anonymous() { return !!(mRec->mFlags & eAnonymousMask); }
  bool     InBrowser() { return !!(mRec->mFlags & eInBrowserMask); }

  bool IsRemoved() { return !!(mRec->mFlags & eRemovedMask); }
  void MarkRemoved() { mRec->mFlags |= eRemovedMask; }

  bool IsDirty() { return !!(mRec->mFlags & eDirtyMask); }
  void MarkDirty() { mRec->mFlags |= eDirtyMask; }
  void ClearDirty() { mRec->mFlags &= ~eDirtyMask; }

  bool IsFresh() { return !!(mRec->mFlags & eFreshMask); }
  void MarkFresh() { mRec->mFlags |= eFreshMask; }

  void     SetFrecency(uint32_t aFrecency) { mRec->mFrecency = aFrecency; }
  uint32_t GetFrecency() { return mRec->mFrecency; }

  void     SetExpirationTime(uint32_t aExpirationTime)
  {
    mRec->mExpirationTime = aExpirationTime;
  }
  uint32_t GetExpirationTime() { return mRec->mExpirationTime; }

  void     SetFileSize(uint32_t aFileSize)
  {
    MOZ_ASSERT((aFileSize & ~eFileSizeMask) == 0);
    mRec->mFlags &= ~eFileSizeMask;
    mRec->mFlags |= aFileSize;
  }
  uint32_t GetFileSize() { return mRec->mFlags & eFileSizeMask; }
  bool     IsEmpty() { return GetFileSize() == 0; }

  void WriteToBuf(void *aBuf)
  {
    CacheIndexRecord *dst = reinterpret_cast<CacheIndexRecord *>(aBuf);
    memcpy(aBuf, mRec, sizeof(CacheIndexRecord));
    // clear dirty flag
    dst->mFlags &= ~eDirtyMask;
    // clear fresh flag
    dst->mFlags &= ~eFreshMask;

#if defined(IS_LITTLE_ENDIAN)
    NetworkEndian::writeUint32(&dst->mFrecency, dst->mFrecency);
    NetworkEndian::writeUint32(&dst->mExpirationTime, dst->mExpirationTime);
    NetworkEndian::writeUint32(&dst->mAppId, dst->mAppId);
    NetworkEndian::writeUint32(&dst->mFlags, dst->mFlags);
#endif
  }

  void ReadFromBuf(void *aBuf)
  {
    CacheIndexRecord *src= reinterpret_cast<CacheIndexRecord *>(aBuf);
    MOZ_ASSERT(memcmp(&mRec->mHash, &src->mHash,
               sizeof(SHA1Sum::Hash)) == 0);

    mRec->mFrecency = NetworkEndian::readUint32(&src->mFrecency);
    mRec->mExpirationTime = NetworkEndian::readUint32(&src->mExpirationTime);
    mRec->mAppId = NetworkEndian::readUint32(&src->mAppId);
    mRec->mFlags = NetworkEndian::readUint32(&src->mFlags);
  }

  void Log() {
    LOG(("CacheIndexEntry::Log() [this=%p, hash=%08x%08x%08x%08x%08x, fresh=%u,"
         " initialized=%u, removed=%u, dirty=%u, anonymous=%u, inBrowser=%u, "
         "appId=%u, frecency=%u, expirationTime=%u, size=%u]",
         this, LOGSHA1(mRec->mHash), IsFresh(), IsInitialized(), IsRemoved(),
         IsDirty(), Anonymous(), InBrowser(), AppId(), GetFrecency(),
         GetExpirationTime(), GetFileSize()));
  }

private:
  friend class CacheIndex;
  friend class CacheIndexEntryAutoManage;

  enum {
    eInitializedMask = 0x80000000,
    eAnonymousMask   = 0x40000000,
    eInBrowserMask   = 0x20000000,
    eRemovedMask     = 0x10000000,
    eDirtyMask       = 0x08000000,
    eFreshMask       = 0x04000000,
    eReservedMask    = 0x03000000,
    eFileSizeMask    = 0x00FFFFFF
  };

  CacheIndexRecord *mRec;
};

class CacheIndexStats
{
public:
  CacheIndexStats()
    : mStateLogged(false)
    , mDisableLogging(false)
    , mCount(0)
    , mNotInitialized(0)
    , mRemoved(0)
    , mDirty(0)
    , mFresh(0)
    , mEmpty(0)
    , mSize(0)
  {
  }

  bool operator==(const CacheIndexStats& aOther) const
  {
    return aOther.mStateLogged == mStateLogged &&
           aOther.mCount == mCount &&
           aOther.mNotInitialized == mNotInitialized &&
           aOther.mRemoved == mRemoved &&
           aOther.mDirty == mDirty &&
           aOther.mFresh == mFresh &&
           aOther.mEmpty == mEmpty &&
           aOther.mSize == mSize;
  }

  void DisableLogging() {
    mDisableLogging = true;
  }

  void Log() {
    LOG(("CacheIndexStats::Log() [count=%u, notInitialized=%u, removed=%u, "
         "dirty=%u, fresh=%u, empty=%u, size=%lld]", mCount, mNotInitialized,
         mRemoved, mDirty, mFresh, mEmpty, mSize));
  }

  bool StateLogged() {
    return mStateLogged;
  }

  uint32_t Count() {
    MOZ_ASSERT(!mStateLogged, "CacheIndexStats::Count() - state logged!");
    return mCount;
  }

  uint32_t Dirty() {
    MOZ_ASSERT(!mStateLogged, "CacheIndexStats::Dirty() - state logged!");
    return mDirty;
  }

  uint32_t Fresh() {
    MOZ_ASSERT(!mStateLogged, "CacheIndexStats::Fresh() - state logged!");
    return mFresh;
  }

  uint32_t ActiveEntriesCount() {
    MOZ_ASSERT(!mStateLogged, "CacheIndexStats::ActiveEntriesCount() - state "
               "logged!");
    return mCount - mRemoved - mNotInitialized - mEmpty;
  }

  int64_t Size() {
    MOZ_ASSERT(!mStateLogged, "CacheIndexStats::Size() - state logged!");
    return mSize;
  }

  void BeforeChange(CacheIndexEntry *aEntry) {
    if (!mDisableLogging) {
#ifdef DEBUG_STATS
      LOG(("CacheIndexStats::BeforeChange()"));
      Log();
#endif
    }

    MOZ_ASSERT(!mStateLogged, "CacheIndexStats::BeforeChange() - state "
               "logged!");
    mStateLogged = !mStateLogged;
    if (aEntry) {
      MOZ_ASSERT(mCount);
      mCount--;
      if (aEntry->IsDirty()) {
        MOZ_ASSERT(mDirty);
        mDirty--;
      }
      if (aEntry->IsFresh()) {
        MOZ_ASSERT(mFresh);
        mFresh--;
      }
      if (aEntry->IsRemoved()) {
        MOZ_ASSERT(mRemoved);
        mRemoved--;
      }
      else {
        if (!aEntry->IsInitialized()) {
          MOZ_ASSERT(mNotInitialized);
          mNotInitialized--;
        }
        else {
          if (aEntry->IsEmpty()) {
            MOZ_ASSERT(mEmpty);
            mEmpty--;
          }
          else {
            MOZ_ASSERT(mSize);
            mSize -= aEntry->GetFileSize();
          }
        }
      }
    }
  }

  void AfterChange(CacheIndexEntry *aEntry) {
    MOZ_ASSERT(mStateLogged, "CacheIndexStats::AfterChange() - state not "
               "logged!");
    mStateLogged = !mStateLogged;
    if (aEntry) {
      mCount++;
      if (aEntry->IsDirty()) mDirty++;
      if (aEntry->IsFresh()) mFresh++;
      if (aEntry->IsRemoved()) mRemoved++;
      else {
        if (!aEntry->IsInitialized()) mNotInitialized++;
        else {
          if (aEntry->IsEmpty()) mEmpty++;
          else mSize += aEntry->GetFileSize();
        }
      }
    }

    if (!mDisableLogging) {
#ifdef DEBUG_STATS
      LOG(("CacheIndexStats::AfterChange()"));
      Log();
#endif
    }
  }

private:
  bool     mStateLogged;
  bool     mDisableLogging;
  uint32_t mCount;
  uint32_t mNotInitialized;
  uint32_t mRemoved;
  uint32_t mDirty;
  uint32_t mFresh;
  uint32_t mEmpty;
  int64_t  mSize;
};

class CacheIndex : public CacheFileIOListener
                 , public nsIRunnable
{
public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIRUNNABLE

  CacheIndex();

  static nsresult Init(nsIFile *aCacheDirectory);
  static nsresult PreShutdown();
  static nsresult Shutdown();

  static nsresult AddEntry(const SHA1Sum::Hash *aHash);
  static nsresult EnsureEntryExists(const SHA1Sum::Hash *aHash);
  static nsresult InitEntry(const SHA1Sum::Hash *aHash,
                            uint32_t             aAppId,
                            bool                 aAnonymous,
                            bool                 aInBrowser);
  static nsresult RemoveEntry(const SHA1Sum::Hash *aHash);
  static nsresult UpdateEntry(const SHA1Sum::Hash *aHash,
                              const uint32_t      *aFrecency,
                              const uint32_t      *aExpirationTime,
                              const uint32_t      *aSize);

  enum EntryStatus {
    EXISTS         = 0,
    DOES_NOT_EXIST = 1,
    DOES_NOT_KNOW  = 2
  };

  static nsresult HasEntry(const nsACString &aKey, EntryStatus *_retval);

  NS_IMETHOD OnFileOpened(CacheFileHandle *aHandle, nsresult aResult);
  NS_IMETHOD OnDataWritten(CacheFileHandle *aHandle, const char *aBuf,
                           nsresult aResult);
  NS_IMETHOD OnDataRead(CacheFileHandle *aHandle, char *aBuf, nsresult aResult);
  NS_IMETHOD OnFileDoomed(CacheFileHandle *aHandle, nsresult aResult);
  NS_IMETHOD OnEOFSet(CacheFileHandle *aHandle, nsresult aResult);
  NS_IMETHOD OnFileRenamed(CacheFileHandle *aHandle, nsresult aResult);

private:
  friend class CacheIndexEntryAutoManage;
  friend class CacheIndexAutoLock;
  friend class CacheIndexAutoUnlock;

  virtual ~CacheIndex();

  void     Lock();
  void     Unlock();
  void     AssertOwnsLock();

  nsresult InitInternal(nsIFile *aCacheDirectory);
  void     PreShutdownInternal();

  nsresult EnsureIndexUsable();

  static bool CheckCollision(CacheIndexEntry *aEntry,
                             uint32_t         aAppId,
                             bool             aAnonymous,
                             bool             aInBrowser);

  static bool EntryChanged(CacheIndexEntry *aEntry,
                           const uint32_t  *aFrecency,
                           const uint32_t  *aExpirationTime,
                           const uint32_t  *aSize);

  void ProcessPendingOperations();
  static PLDHashOperator UpdateEntryInIndex(CacheIndexEntry *aEntry,
                                            void* aClosure);

  bool WriteIndexToDiskIfNeeded();
  void WriteIndexToDisk();
  void WriteIndexHeader(CacheFileHandle *aHandle, nsresult aResult);
  void WriteRecords();
  void FinishWrite(bool aSucceeded);

  static PLDHashOperator CopyRecordsToRWBuf(CacheIndexEntry *aEntry,
                                            void* aClosure);
  static PLDHashOperator ApplyIndexChanges(CacheIndexEntry *aEntry,
                                           void* aClosure);

  nsresult GetFile(const nsACString &aName, nsIFile **_retval);
  nsresult RemoveFile(const nsACString &aName);
  void     RemoveIndexFromDisk();
  nsresult WriteLogToDisk();

  static PLDHashOperator WriteEntryToLog(CacheIndexEntry *aEntry,
                                         void* aClosure);

  void ReadIndexFromDisk();
  void StartReadingIndex();
  void ParseRecords();
  void StartReadingJournal();
  void ParseJournal();
  void MergeJournal();
  void EnsureNoFreshEntry();
  void EnsureCorrectStats();
  static PLDHashOperator SumIndexStats(CacheIndexEntry *aEntry, void* aClosure);
  void FinishRead(bool aSucceeded);

  static PLDHashOperator ProcessJournalEntry(CacheIndexEntry *aEntry,
                                             void* aClosure);

  static void DelayedBuildUpdate(nsITimer *aTimer, void *aClosure);
  nsresult PostBuildUpdateTimer(uint32_t aDelay);
  nsresult SetupDirectoryEnumerator();
  void BuildUpdateInitEntry(CacheIndexEntry *aEntry,
                            CacheFileMetadata *aMetaData,
                            nsIFile *aFile,
                            const nsACString &aLeafName);
  void StartBuildingIndex();
  void BuildIndex();
  void FinishBuild(bool aSucceeded);

  bool StartUpdatingIndexIfNeeded(bool aSwitchingToReadyState = false);
  void StartUpdatingIndex();
  void UpdateIndex();
  void FinishUpdate(bool aSucceeded);

  static PLDHashOperator RemoveNonFreshEntries(CacheIndexEntry *aEntry,
                                               void* aClosure);

  enum EState {
    INITIAL  = 0,
    READING  = 1,
    WRITING  = 2,
    BUILDING = 3,
    UPDATING = 4,
    READY    = 5,
    SHUTDOWN = 6
  };

#ifdef MOZ_LOGGING
  static char const * StateString(EState aState);
#endif
  void ChangeState(EState aNewState);
  void AllocBuffer();
  void ReleaseBuffer();

  void RemoveEntryFromArrays(CacheIndexEntry *aEntry);
  void InsertEntryToArrays(CacheIndexEntry *aEntry);

  void InsertRecordToFrecencyArray(CacheIndexRecord *aRecord);
  void InsertRecordToExpirationArray(CacheIndexRecord *aRecord);
  void RemoveRecordFromFrecencyArray(CacheIndexRecord *aRecord);
  void RemoveRecordFromExpirationArray(CacheIndexRecord *aRecord);

  static CacheIndex *gInstance;

  nsCOMPtr<nsIFile> mCacheDirectory;

  mozilla::Mutex mLock;
  EState         mState;
  TimeStamp      mStartTime;
  bool           mShuttingDown;
  bool           mIndexNeedsUpdate;
  bool           mIndexOnDiskIsValid;
  bool           mDontMarkIndexClean;
  uint32_t       mIndexTimeStamp;
  TimeStamp      mLastDumpTime;

  uint32_t                  mSkipEntries;
  uint32_t                  mProcessEntries;
  char                     *mRWBuf;
  uint32_t                  mRWBufSize;
  uint32_t                  mRWBufPos;
  nsRefPtr<CacheHash>       mHash;

  uint32_t                  mReadOpenCount;
  bool                      mReadFailed;
  bool                      mJournalReadSuccessfully;

  nsRefPtr<CacheFileHandle> mHandle;
  nsRefPtr<CacheFileHandle> mHandle2;

  nsCOMPtr<nsIDirectoryEnumerator> mDirEnumerator;

  nsTHashtable<CacheIndexEntry> mIndex;
  CacheIndexStats               mIndexStats;
  nsTHashtable<CacheIndexEntry> mTmpJournal;
  nsTHashtable<CacheIndexEntry> mPendingUpdates;
  nsTArray<CacheIndexRecord *>  mFrecencyArray;
  nsTArray<CacheIndexRecord *>  mExpirationArray;
};

class CacheIndexEntryAutoManage
{
public:
  CacheIndexEntryAutoManage(const SHA1Sum::Hash *aHash, CacheIndex *aIndex)
    : mIndex(aIndex)
    , mOldRecord(nullptr)
    , mOldFrecency(0)
    , mOldExpirationTime(nsICacheEntry::NO_EXPIRATION_TIME)
    , mDoNotSearchInIndex(false)
    , mDoNotSearchInUpdates(false)
  {
    mHash = aHash;
    CacheIndexEntry *entry = FindEntry();
    mIndex->mIndexStats.BeforeChange(entry);
    if (entry && entry->IsInitialized() && !entry->IsRemoved()) {
      mOldRecord = entry->mRec;
      mOldFrecency = entry->mRec->mFrecency;
      mOldExpirationTime = entry->mRec->mExpirationTime;
    }
  }

  ~CacheIndexEntryAutoManage()
  {
    CacheIndexEntry *entry = FindEntry();
    mIndex->mIndexStats.AfterChange(entry);
    if (!entry || !entry->IsInitialized() || entry->IsRemoved()) {
      entry = nullptr;
    }

    if (entry && !mOldRecord) {
      mIndex->InsertRecordToFrecencyArray(entry->mRec);
      mIndex->InsertRecordToExpirationArray(entry->mRec);
    }
    else if (!entry && mOldRecord) {
      mIndex->RemoveRecordFromFrecencyArray(mOldRecord);
      mIndex->RemoveRecordFromExpirationArray(mOldRecord);
    }
    else if (entry && mOldRecord) {
      bool replaceFrecency = false;
      bool replaceExpiration = false;

      if (entry->mRec != mOldRecord) {
        // record has a different address, we have to replace it
        replaceFrecency = replaceExpiration = true;
      }
      else {
        if (entry->mRec->mFrecency != mOldFrecency) {
          replaceFrecency = true;
        }
        if (entry->mRec->mExpirationTime != mOldExpirationTime) {
          replaceExpiration = true;
        }
      }

      if (replaceFrecency) {
        mIndex->RemoveRecordFromFrecencyArray(mOldRecord);
        mIndex->InsertRecordToFrecencyArray(entry->mRec);
      }
      if (replaceExpiration) {
        mIndex->RemoveRecordFromExpirationArray(mOldRecord);
        mIndex->InsertRecordToExpirationArray(entry->mRec);
      }
    }
    else {
      // both entries were removed or not initialized, do nothing
    }
  }

  // We cannot rely on nsTHashtable::GetEntry() in case we are enumerating the
  // entries and returning PL_DHASH_REMOVE. Destructor is called before the
  // entry is removed. Caller must call one of following methods to skip
  // lookup in the hashtable.
  void DoNotSearchInIndex()   { mDoNotSearchInIndex = true; }
  void DoNotSearchInUpdates() { mDoNotSearchInUpdates = true; }

private:
  CacheIndexEntry * FindEntry()
  {
    CacheIndexEntry *entry = nullptr;

    switch (mIndex->mState) {
      case CacheIndex::READING:
      case CacheIndex::WRITING:
        if (!mDoNotSearchInUpdates)
          entry = mIndex->mPendingUpdates.GetEntry(*mHash);
        // no break
      case CacheIndex::BUILDING:
      case CacheIndex::UPDATING:
      case CacheIndex::READY:
        if (!entry && !mDoNotSearchInIndex)
          entry = mIndex->mIndex.GetEntry(*mHash);
        break;
      case CacheIndex::INITIAL:
      case CacheIndex::SHUTDOWN:
      default:
        MOZ_ASSERT(false, "Unexpected state!");
    }

    return entry;
  }

  const SHA1Sum::Hash *mHash;
  nsRefPtr<CacheIndex> mIndex;
  CacheIndexRecord    *mOldRecord;
  uint32_t             mOldFrecency;
  uint32_t             mOldExpirationTime;
  bool                 mDoNotSearchInIndex;
  bool                 mDoNotSearchInUpdates;
};

class CacheIndexAutoLock {
public:
  CacheIndexAutoLock(CacheIndex *aIndex)
    : mIndex(aIndex)
    , mLocked(true)
  {
    mIndex->Lock();
  }
  ~CacheIndexAutoLock()
  {
    if (mLocked)
      mIndex->Unlock();
  }
  void Lock()
  {
    MOZ_ASSERT(!mLocked);
    mIndex->Lock();
    mLocked = true;
  }
  void Unlock()
  {
    MOZ_ASSERT(mLocked);
    mIndex->Unlock();
    mLocked = false;
  }

private:
  nsRefPtr<CacheIndex> mIndex;
  bool mLocked;
};

class CacheIndexAutoUnlock {
public:
  CacheIndexAutoUnlock(CacheIndex *aIndex)
    : mIndex(aIndex)
    , mLocked(false)
  {
    mIndex->Unlock();
  }
  ~CacheIndexAutoUnlock()
  {
    if (!mLocked)
      mIndex->Lock();
  }
  void Lock()
  {
    MOZ_ASSERT(!mLocked);
    mIndex->Lock();
    mLocked = true;
  }
  void Unlock()
  {
    MOZ_ASSERT(mLocked);
    mIndex->Unlock();
    mLocked = false;
  }

private:
  nsRefPtr<CacheIndex> mIndex;
  bool mLocked;
};

} // net
} // mozilla

#endif
