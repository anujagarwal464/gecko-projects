/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CacheIndex.h"

#include "CacheLog.h"
#include "CacheFileIOManager.h"
#include "CacheFileMetadata.h"
#include "nsThreadUtils.h"
#include "nsISimpleEnumerator.h"
#include "nsIDirectoryEnumerator.h"
#include "nsPrintfCString.h"
#include "mozilla/DebugOnly.h"
#include "prinrval.h"
#include "nsIFile.h"
#include <algorithm>


#define kMinUnwrittenChanges 30 // 300
#define kMinDumpInterval     3000 // 20000  // in milliseconds
#define kMaxBufSize          16384
#define kIndexVersion        0x00000001

const char kIndexName[]     = "index";
const char kTempIndexName[] = "index.tmp";
const char kJournalName[]   = "index.log";

namespace mozilla {
namespace net {

CacheIndex * CacheIndex::gInstance = nullptr;


NS_IMPL_ADDREF(CacheIndex)
NS_IMPL_RELEASE(CacheIndex)

NS_INTERFACE_MAP_BEGIN(CacheIndex)
  NS_INTERFACE_MAP_ENTRY(mozilla::net::CacheFileIOListener)
  NS_INTERFACE_MAP_ENTRY(nsIRunnable)
NS_INTERFACE_MAP_END_THREADSAFE


CacheIndex::CacheIndex()
  : mLock("CacheFile.mLock")
  , mState(INITIAL)
  , mShuttingDown(false)
  , mIndexNeedsUpdate(false)
  , mIndexOnDiskIsValid(false)
  , mDontMarkIndexClean(false)
  , mIndexTimeStamp(0)
  , mLastDumpTime(0)
  , mSkipEntries(0)
  , mProcessEntries(0)
  , mRWBuf(nullptr)
  , mRWBufSize(0)
  , mRWBufPos(0)
  , mReadOpenCount(0)
  , mReadFailed(false)
  , mJournalReadSuccessfully(false)
{
  LOG(("CacheIndex::CacheIndex [this=%p]", this));
  MOZ_COUNT_CTOR(CacheIndex);
  MOZ_ASSERT(!gInstance, "multiple CacheIndex instances!");
}

CacheIndex::~CacheIndex()
{
  LOG(("CacheIndex::~CacheIndex [this=%p]", this));
  MOZ_COUNT_DTOR(CacheIndex);

  ReleaseBuffer();
}

nsresult
CacheIndex::Init(nsIFile *aCacheDirectory)
{
  LOG(("CacheIndex::Init()"));

  MOZ_ASSERT(NS_IsMainThread());

  if (gInstance)
    return NS_ERROR_ALREADY_INITIALIZED;

  nsRefPtr<CacheIndex> idx = new CacheIndex();

  nsresult rv = idx->InitInternal(aCacheDirectory);
  NS_ENSURE_SUCCESS(rv, rv);

  idx.swap(gInstance);
  return NS_OK;
}

nsresult
CacheIndex::InitInternal(nsIFile *aCacheDirectory)
{
  nsresult rv;

  rv = aCacheDirectory->Clone(getter_AddRefs(mCacheDirectory));
  NS_ENSURE_SUCCESS(rv, rv);

  ChangeState(READING);

  // dispatch an event since IO manager's path is not initialized yet
  nsCOMPtr<nsIRunnable> event;
  event = NS_NewRunnableMethod(this, &CacheIndex::ReadIndexFromDisk);

  rv = NS_DispatchToCurrentThread(event);
  if (NS_FAILED(rv)) {
    ChangeState(INITIAL);
    LOG(("CacheIndex::InitInternal() - Cannot dispatch event"));
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}

nsresult
CacheIndex::PreShutdown()
{
  LOG(("CacheIndex::PreShutdown() [gInstance=%p]", gInstance));

  MOZ_ASSERT(NS_IsMainThread());

  nsresult rv;
  nsRefPtr<CacheIndex> index = gInstance;

  if (!index)
    return NS_ERROR_NOT_INITIALIZED;

  MutexAutoLock lock(index->mLock);

  LOG(("CacheIndex::PreShutdown() - [state=%d, indexOnDiskIsValid=%d, "
       "dontMarkIndexClean=%d]", index->mState, index->mIndexOnDiskIsValid,
       index->mDontMarkIndexClean));

  index->mShuttingDown = true;

  if (index->mState == READY)
    return NS_OK; // nothing to do

  nsCOMPtr<nsIRunnable> event;
  event = NS_NewRunnableMethod(index, &CacheIndex::PreShutdownInternal);

  nsCOMPtr<nsIEventTarget> ioTarget = CacheFileIOManager::IOTarget();
  MOZ_ASSERT(ioTarget);

  rv = ioTarget->Dispatch(event, nsIEventTarget::DISPATCH_NORMAL);
  if (NS_FAILED(rv)) {
    NS_WARNING("CacheIndex::PreShutdown() - Can't dispatch event");
    LOG(("CacheIndex::PreShutdown() - Can't dispatch event" ));
    return rv;
  }

  return NS_OK;
}

void
CacheIndex::PreShutdownInternal()
{
  LOG(("CacheIndex::PreShutdownInternal()"));

  MutexAutoLock lock(mLock);

  LOG(("CacheIndex::PreShutdownInternal() - [state=%d, indexOnDiskIsValid=%d, "
       "dontMarkIndexClean=%d]", mState, mIndexOnDiskIsValid,
       mDontMarkIndexClean));

  MOZ_ASSERT(mShuttingDown);

  switch (mState) {
    case WRITING:
      FinishWrite(false);
      break;
    case READY:
      // nothing to do, write the journal in Shutdown()
      break;
    case READING:
      FinishRead(false);
      break;
    case BUILDING:
      FinishBuild(false);
      break;
    case UPDATING:
      FinishUpdate(false);
      break;
    default:
      MOZ_ASSERT(false, "Implement me!");
  }

  // We should end up in READY state
  MOZ_ASSERT(mState == READY);
}

nsresult
CacheIndex::Shutdown()
{
  LOG(("CacheIndex::Shutdown() [gInstance=%p]", gInstance));

  MOZ_ASSERT(NS_IsMainThread());

  nsRefPtr<CacheIndex> index;
  index.swap(gInstance);

  if (!index)
    return NS_ERROR_NOT_INITIALIZED;

  MutexAutoLock lock(index->mLock);

  LOG(("CacheIndex::Shutdown() - [state=%d, indexOnDiskIsValid=%d, "
       "dontMarkIndexClean=%d]", index->mState, index->mIndexOnDiskIsValid,
       index->mDontMarkIndexClean));

  MOZ_ASSERT(index->mShuttingDown);

  EState oldState = index->mState;
  index->ChangeState(SHUTDOWN);

  if (oldState != READY) {
    LOG(("CacheIndex::Shutdown() - Unexpected state. Did posting of "
         "PreShutdownInternal() fail?"));
  }

  switch (oldState) {
    case WRITING:
      index->FinishWrite(false);
      // no break
    case READY:
      if (index->mIndexOnDiskIsValid && !index->mDontMarkIndexClean) {
        if (NS_FAILED(index->WriteLogToDisk()))
          index->RemoveIndexFromDisk();
      }
      else
        index->RemoveIndexFromDisk();
      break;
    case READING:
      index->FinishRead(false);
      break;
    case BUILDING:
      index->FinishBuild(false);
      break;
    case UPDATING:
      index->FinishUpdate(false);
      break;
    default:
      MOZ_ASSERT(false, "Implement me!");
  }

  return NS_OK;
}

nsresult
CacheIndex::AddEntry(const SHA1Sum::Hash *aHash)
{
  LOG(("CacheIndex::AddEntry() [hash=%08x%08x%08x%08x%08x]", LOGSHA1(aHash)));

  nsresult rv;
  nsRefPtr<CacheIndex> index = gInstance;

  if (!index)
    return NS_ERROR_NOT_INITIALIZED;

  MutexAutoLock lock(index->mLock);

  rv = index->EnsureIndexUsable();
  if (NS_FAILED(rv)) return rv;

  bool doUpdateIfHasEffect = false;

  {
    CacheIndexEntryAutoManage entryMng(aHash, index);

    CacheIndexEntry *entry = index->mIndex.GetEntry(*aHash);
    bool entryRemoved = entry && entry->IsRemoved();

    if (index->mState == READY || index->mState == UPDATING ||
        index->mState == BUILDING) {
      MOZ_ASSERT(index->mPendingUpdates.Count() == 0);

      if (entry && !entryRemoved) {
        if (entry->IsFresh()) {
          // Someone removed the file on disk while FF is running. Update
          // process can fix only non-fresh entries (i.e. entries that were not
          // added within this session). Start update only if we have such
          // entries.
          //
          // TODO: This should be very rare problem. If it turns out not to be
          // true, change the update process so that it also iterates all
          // initialized non-empty entries and checks whether the file exists.

          LOG(("CacheIndex::AddEntry() - Cache file was removed outside FF "
               "process!"));

          doUpdateIfHasEffect = true;
        }
        else if (index->mState == READY) {
          LOG(("CacheIndex::AddEntry() - Found entry that shouldn't exist, "
               "update is needed"));
          index->mIndexNeedsUpdate = true;
        }
      }

      if (!entry) {
        entry = index->mIndex.PutEntry(*aHash);
      }
    }
    else { // WRITING, READING
      CacheIndexEntry *updated = index->mPendingUpdates.GetEntry(*aHash);
      bool updatedRemoved = updated && updated->IsRemoved();

      if ((updated && !updatedRemoved) ||
          (!updated && entry && !entryRemoved && entry->IsFresh())) {
        // Fresh entry found, so the file was removed outside FF
        LOG(("CacheIndex::AddEntry() - Cache file was removed outside FF "
             "process!"));

        doUpdateIfHasEffect = true;
      }
      else if (!updated && entry && !entryRemoved) {
        if (index->mState == WRITING) {
          LOG(("CacheIndex::AddEntry() - Found entry that shouldn't exist, "
               "update is needed"));
          index->mIndexNeedsUpdate = true;
        }
        // Ignore if state is READING since the index information is partial
      }

      updated = index->mPendingUpdates.PutEntry(*aHash);
      entry = updated;
    }

    entry->InitNew();
    entry->MarkDirty();
    entry->MarkFresh();
  }

  if (doUpdateIfHasEffect &&
      index->mIndexStats.Count() != index->mIndexStats.Fresh()) {
    index->mIndexNeedsUpdate = true;
  }

  index->StartUpdatingIndexIfNeeded();
  index->WriteIndexToDiskIfNeeded();

  return NS_OK;
}

nsresult
CacheIndex::EnsureEntryExists(const SHA1Sum::Hash *aHash)
{
  LOG(("CacheIndex::EnsureEntryExists() [hash=%08x%08x%08x%08x%08x]",
       LOGSHA1(aHash)));

  nsresult rv;
  nsRefPtr<CacheIndex> index = gInstance;

  if (!index)
    return NS_ERROR_NOT_INITIALIZED;

  MutexAutoLock lock(index->mLock);

  rv = index->EnsureIndexUsable();
  if (NS_FAILED(rv)) return rv;

  {
    CacheIndexEntryAutoManage entryMng(aHash, index);

    CacheIndexEntry *entry = index->mIndex.GetEntry(*aHash);
    bool entryRemoved = entry && entry->IsRemoved();

    if (index->mState == READY || index->mState == UPDATING ||
        index->mState == BUILDING) {
      MOZ_ASSERT(index->mPendingUpdates.Count() == 0);

      if (!entry || entryRemoved) {
        if (entryRemoved && entry->IsFresh()) {
          // This could happen only if somebody copies files to the entries
          // directory while FF is running.
          LOG(("CacheIndex::EnsureEntryExists() - Cache file was added outside "
               "FF process! Update is needed."));
          index->mIndexNeedsUpdate = true;
        }
        else if (index->mState == READY ||
                 (entryRemoved && !entry->IsFresh())) {
          // Removed non-fresh entries can be present as a result of
          // ProcessJournalEntry()
          LOG(("CacheIndex::EnsureEntryExists() - Didn't find entry that should"
               " exist, update is needed"));
          index->mIndexNeedsUpdate = true;
        }

        if (!entry) {
          entry = index->mIndex.PutEntry(*aHash);
        }
        entry->InitNew();
        entry->MarkDirty();
      }
      entry->MarkFresh();
    }
    else { // WRITING, READING
      CacheIndexEntry *updated = index->mPendingUpdates.GetEntry(*aHash);
      bool updatedRemoved = updated && updated->IsRemoved();

      if (updatedRemoved ||
          (!updated && entryRemoved && entry->IsFresh())) {
        // Fresh information about missing entry found. This could happen only
        // if somebody copies files to the entries directory while FF is running.
        LOG(("CacheIndex::EnsureEntryExists() - Cache file was added outside "
             "FF process! Update is needed."));
        index->mIndexNeedsUpdate = true;
      }
      else if (!updated && (!entry || entryRemoved)) {
        if (index->mState == WRITING) {
          LOG(("CacheIndex::EnsureEntryExists() - Didn't find entry that should"
               " exist, update is needed"));
          index->mIndexNeedsUpdate = true;
        }
        // Ignore if state is READING since the index information is partial
      }

      // We don't need entryRemoved and updatedRemoved info anymore
      if (entryRemoved)   entry = nullptr;
      if (updatedRemoved) updated = nullptr;

      if (updated) {
        updated->MarkFresh();
      }
      else {
        if (!entry) {
          // Create a new entry
          updated = index->mPendingUpdates.PutEntry(*aHash);
          updated->InitNew();
          updated->MarkFresh();
          updated->MarkDirty();
        }
        else {
          if (!entry->IsFresh()) {
            // To mark the entry fresh we must make a copy of index entry
            // since the index is read-only.
            updated = index->mPendingUpdates.PutEntry(*aHash);
            *updated = *entry;
            updated->MarkFresh();
          }
        }
      }
    }
  }

  index->StartUpdatingIndexIfNeeded();
  index->WriteIndexToDiskIfNeeded();

  return NS_OK;
}

nsresult
CacheIndex::InitEntry(const SHA1Sum::Hash *aHash,
                      uint32_t             aAppId,
                      bool                 aAnonymous,
                      bool                 aInBrowser)
{
  LOG(("CacheIndex::InitEntry() [hash=%08x%08x%08x%08x%08x, appId=%u, "
       "anonymous=%d, inBrowser=%d]", LOGSHA1(aHash), aAppId, aAnonymous,
       aInBrowser));

  nsresult rv;
  nsRefPtr<CacheIndex> index = gInstance;

  if (!index)
    return NS_ERROR_NOT_INITIALIZED;

  MutexAutoLock lock(index->mLock);

  rv = index->EnsureIndexUsable();
  if (NS_FAILED(rv)) return rv;

  {
    CacheIndexEntryAutoManage entryMng(aHash, index);

    CacheIndexEntry *entry = index->mIndex.GetEntry(*aHash);

    if (entry && entry->IsRemoved())
      entry = nullptr;

    if (index->mState == READY || index->mState == UPDATING ||
        index->mState == BUILDING) {
      MOZ_ASSERT(index->mPendingUpdates.Count() == 0);
      MOZ_ASSERT(entry);
      MOZ_ASSERT(entry->IsFresh());

      if (CheckCollision(entry, aAppId, aAnonymous, aInBrowser)) {
        index->mIndexNeedsUpdate = true; // TODO Does this really help in case of collision?
      }
      else {
        if (entry->IsInitialized())
          return NS_OK;
      }
    }
    else {
      CacheIndexEntry *updated = index->mPendingUpdates.GetEntry(*aHash);
      DebugOnly<bool> removed = updated && updated->IsRemoved();

      MOZ_ASSERT(updated || !removed);
      MOZ_ASSERT(updated || entry);

      if (updated) {
        MOZ_ASSERT(updated->IsFresh());

        if (CheckCollision(updated, aAppId, aAnonymous, aInBrowser)) {
          index->mIndexNeedsUpdate = true;
        } else {
          if (updated->IsInitialized())
            return NS_OK;
        }
        entry = updated;
      } else {
        MOZ_ASSERT(entry->IsFresh());

        if (CheckCollision(entry, aAppId, aAnonymous, aInBrowser)) {
          index->mIndexNeedsUpdate = true;
        } else {
          if (entry->IsInitialized())
            return NS_OK;
        }

        // make a copy of a read-only entry
        updated = index->mPendingUpdates.PutEntry(*aHash);
        *updated = *entry;
        entry = updated;
      }
    }

    entry->Init(aAppId, aAnonymous, aInBrowser);
    entry->MarkDirty();
  }

  index->StartUpdatingIndexIfNeeded();
  index->WriteIndexToDiskIfNeeded();

  return NS_OK;
}

nsresult
CacheIndex::RemoveEntry(const SHA1Sum::Hash *aHash)
{
  LOG(("CacheIndex::RemoveEntry() [hash=%08x%08x%08x%08x%08x]",
       LOGSHA1(aHash)));

  nsresult rv;
  nsRefPtr<CacheIndex> index = gInstance;

  if (!index)
    return NS_ERROR_NOT_INITIALIZED;

  MutexAutoLock lock(index->mLock);

  rv = index->EnsureIndexUsable();
  if (NS_FAILED(rv)) return rv;

  {
    CacheIndexEntryAutoManage entryMng(aHash, index);

    CacheIndexEntry *entry = index->mIndex.GetEntry(*aHash);
    bool entryRemoved = entry && entry->IsRemoved();

    if (index->mState == READY || index->mState == UPDATING ||
        index->mState == BUILDING) {
      MOZ_ASSERT(index->mPendingUpdates.Count() == 0);

      if (!entry || entryRemoved) {
        if (entryRemoved && entry->IsFresh()) {
          // This could happen only if somebody copies files to the entries
          // directory while FF is running.
          LOG(("CacheIndex::RemoveEntry() - Cache file was added outside FF "
               "process! Update is needed."));
          index->mIndexNeedsUpdate = true;
        }
        else if (index->mState == READY ||
                 (entryRemoved && !entry->IsFresh())) {
          // Removed non-fresh entries can be present as a result of
          // ProcessJournalEntry()
          LOG(("CacheIndex::RemoveEntry() - Didn't find entry that should exist"
               ", update is needed"));
          index->mIndexNeedsUpdate = true;
        }
      }
      else {
        if (entry) {
          if (!entry->IsDirty() && entry->IsEmpty()) {
            index->mIndex.RemoveEntry(*aHash);
            entry = nullptr;
          } else {
            entry->MarkRemoved();
            entry->MarkDirty();
            entry->MarkFresh();
          }
        }
      }
    }
    else { // WRITING, READING
      CacheIndexEntry *updated = index->mPendingUpdates.GetEntry(*aHash);
      bool updatedRemoved = updated && updated->IsRemoved();

      if (updatedRemoved ||
          (!updated && entryRemoved && entry->IsFresh())) {
        // Fresh information about missing entry found. This could happen only
        // if somebody copies files to the entries directory while FF is running.
        LOG(("CacheIndex::RemoveEntry() - Cache file was added outside FF "
             "process! Update is needed."));
        index->mIndexNeedsUpdate = true;
      }
      else if (!updated && (!entry || entryRemoved)) {
        if (index->mState == WRITING) {
          LOG(("CacheIndex::RemoveEntry() - Didn't find entry that should exist"
               ", update is needed"));
          index->mIndexNeedsUpdate = true;
        }
        // Ignore if state is READING since the index information is partial
      }

      if (!updated) {
        updated = index->mPendingUpdates.PutEntry(*aHash);
        updated->InitNew();
      }

      updated->MarkRemoved();
      updated->MarkDirty();
      updated->MarkFresh();
    }
  }

  index->StartUpdatingIndexIfNeeded();
  index->WriteIndexToDiskIfNeeded();

  return NS_OK;
}

nsresult
CacheIndex::UpdateEntry(const SHA1Sum::Hash *aHash,
                        const uint32_t      *aFrecency,
                        const uint32_t      *aExpirationTime,
                        const uint32_t      *aSize)
{
  LOG(("CacheIndex::UpdateEntry() [hash=%08x%08x%08x%08x%08x, "
       "frecency=%s, expirationTime=%s, size=%s]", LOGSHA1(aHash),
       aFrecency ? nsPrintfCString("%u", *aFrecency).get() : "",
       aExpirationTime ? nsPrintfCString("%u", *aExpirationTime).get() : "",
       aSize ? nsPrintfCString("%u", *aSize).get() : ""));

  nsresult rv;
  nsRefPtr<CacheIndex> index = gInstance;

  if (!index)
    return NS_ERROR_NOT_INITIALIZED;

  MutexAutoLock lock(index->mLock);

  rv = index->EnsureIndexUsable();
  if (NS_FAILED(rv)) return rv;

  {
    CacheIndexEntryAutoManage entryMng(aHash, index);

    CacheIndexEntry *entry = index->mIndex.GetEntry(*aHash);

    if (entry && entry->IsRemoved())
      entry = nullptr;

    if (index->mState == READY || index->mState == UPDATING ||
        index->mState == BUILDING) {
      MOZ_ASSERT(index->mPendingUpdates.Count() == 0);
      MOZ_ASSERT(entry);

      if (!EntryChanged(entry, aFrecency, aExpirationTime, aSize)) {
        return NS_OK;
      }
    }
    else {
      CacheIndexEntry *updated = index->mPendingUpdates.GetEntry(*aHash);
      DebugOnly<bool> removed = updated && updated->IsRemoved();

      MOZ_ASSERT(updated || !removed);
      MOZ_ASSERT(updated || entry);

      if (!updated) {
        if (entry && EntryChanged(entry, aFrecency, aExpirationTime, aSize)) {
          // make a copy of a read-only entry
          updated = index->mPendingUpdates.PutEntry(*aHash);
          *updated = *entry;
          entry = updated;
        }
        else
          return NS_ERROR_NOT_AVAILABLE;
      }
      else {
        entry = updated;
      }
    }

    MOZ_ASSERT(entry->IsFresh());
    entry->MarkDirty();

    if (aFrecency)
      entry->SetFrecency(*aFrecency);

    if (aExpirationTime)
      entry->SetExpirationTime(*aExpirationTime);

    if (aSize) {
      uint32_t size = *aSize;
      if (size & ~CacheIndexEntry::eFileSizeMask) {
        LOG(("FileSize is too big, truncating to %u",
             CacheIndexEntry::eFileSizeMask));
        size = CacheIndexEntry::eFileSizeMask;
      }
      entry->SetFileSize(size);
    }
  }

  index->WriteIndexToDiskIfNeeded();

  return NS_OK;
}

nsresult
CacheIndex::EnsureIndexUsable()
{
  MOZ_ASSERT(mState != INITIAL);

  switch (mState) {
    case INITIAL:
    case SHUTDOWN:
      return NS_ERROR_NOT_AVAILABLE;

    case READING:
    case WRITING:
    case BUILDING:
    case UPDATING:
    case READY:
      break;
  }

  return NS_OK;
}

bool
CacheIndex::CheckCollision(CacheIndexEntry *aEntry,
                           uint32_t         aAppId,
                           bool             aAnonymous,
                           bool             aInBrowser)
{
  if (!aEntry->IsInitialized())
    return false;

  if (aEntry->AppId() != aAppId || aEntry->Anonymous() != aAnonymous ||
      aEntry->InBrowser() != aInBrowser) {
    LOG(("CacheIndex::CheckCollision() - Collision detected for entry hash=%08x"
         "%08x%08x%08x%08x, expected values: appId=%u, anonymous=%d, "
         "inBrowser=%d; actual values: appId=%u, anonymous=%d, inBrowser=%d]",
         LOGSHA1(aEntry->Hash()), aAppId, aAnonymous, aInBrowser,
         aEntry->AppId(), aEntry->Anonymous(), aEntry->InBrowser()));
    return true;
  }

  return false;
}

bool
CacheIndex::EntryChanged(CacheIndexEntry *aEntry,
                         const uint32_t  *aFrecency,
                         const uint32_t  *aExpirationTime,
                         const uint32_t  *aSize)
{
  if (aFrecency && *aFrecency != aEntry->GetFrecency())
    return true;
  if (aExpirationTime && *aExpirationTime != aEntry->GetExpirationTime())
    return true;
  if (aSize &&
      (*aSize & CacheIndexEntry::eFileSizeMask) != aEntry->GetFileSize())
    return true;

  return false;
}

void
CacheIndex::ProcessPendingOperations()
{
  LOG(("CacheIndex::ProcessPendingOperations()"));

  mLock.AssertCurrentThreadOwns();

  mPendingUpdates.EnumerateEntries(&CacheIndex::UpdateEntryInIndex, this);

  MOZ_ASSERT(mPendingUpdates.Count() == 0);

  EnsureCorrectStats();
}

PLDHashOperator
CacheIndex::UpdateEntryInIndex(CacheIndexEntry *aEntry, void* aClosure)
{
  CacheIndex *index = static_cast<CacheIndex *>(aClosure);

  LOG(("CacheFile::UpdateEntryInIndex() [hash=%08x%08x%08x%08x%08x]",
       LOGSHA1(aEntry->Hash())));

  MOZ_ASSERT(aEntry->IsFresh());
  MOZ_ASSERT(aEntry->IsDirty());

  CacheIndexEntry *entry = index->mIndex.GetEntry(*aEntry->Hash());

  CacheIndexEntryAutoManage emng(aEntry->Hash(), index);
  emng.DoNotSearchInUpdates();

  if (aEntry->IsRemoved()) {
    if (entry) {
      if (entry->IsRemoved()) {
        MOZ_ASSERT(entry->IsFresh());
        MOZ_ASSERT(entry->IsDirty());
      }
      else if (!entry->IsRemoved() && !entry->IsDirty() && entry->IsEmpty()) {
        // Empty entries are not stored in index. Just remove the entry, but
        // only in case the entry is not dirty, i.e. the entry was empty when
        // we wrote the index.
        index->mIndex.RemoveEntry(*aEntry->Hash());
        entry = nullptr;
      }
      else {
        entry->MarkRemoved();
        entry->MarkDirty();
        entry->MarkFresh();
      }
    }

    return PL_DHASH_REMOVE;
  }

  entry = index->mIndex.PutEntry(*aEntry->Hash());
  *entry = *aEntry;

  return PL_DHASH_REMOVE;
}

bool
CacheIndex::WriteIndexToDiskIfNeeded()
{
  if (mState != READY || mShuttingDown)
    return false;

  if (!((PR_IntervalNow() - mLastDumpTime) >
      PR_MillisecondsToInterval(kMinDumpInterval)))
    return false;

  if (mIndexStats.Dirty() < kMinUnwrittenChanges)
    return false;

  WriteIndexToDisk();
  return true;
}

void
CacheIndex::WriteIndexToDisk()
{
  LOG(("CacheIndex::WriteIndexToDisk()"));
  mIndexStats.Log();

  nsresult rv;

  mLock.AssertCurrentThreadOwns();
  MOZ_ASSERT(mState == READY);

  ChangeState(WRITING);

  mProcessEntries = mIndexStats.ActiveEntriesCount();

  rv = CacheFileIOManager::OpenFile(NS_LITERAL_CSTRING(kTempIndexName),
                                    CacheFileIOManager::SPECIAL_FILE |
                                    CacheFileIOManager::CREATE,
                                    this);
  if (NS_FAILED(rv)) {
    LOG(("CacheIndex::WriteIndexToDisk() - Can't open file [rv=0x%08x]", rv));
    FinishWrite(false);
  }
}

struct WriteRecordsHelper
{
  char    *mBuf;
  uint32_t mSkip;
  uint32_t mProcessMax;
  uint32_t mProcessed;
#ifdef DEBUG
  bool     mHasMore;
#endif
};

void
CacheIndex::WriteIndexHeader(CacheFileHandle *aHandle, nsresult aResult)
{
  LOG(("CacheIndex::WriteIndexHeader() [handle=%p, result=0x%08x]", aHandle,
       aResult));

  mLock.AssertCurrentThreadOwns();
  MOZ_ASSERT(!mRWBuf);
  MOZ_ASSERT(!mHash);

  if (NS_FAILED(aResult)) {
    LOG(("CacheIndex::WriteIndexHeader() - Can't open file [rv=0x%08x]",
         aResult));
    FinishWrite(false);
    return;
  }

  AllocBuffer();
  mHandle = aHandle;
  mHash = new CacheHash();

  CacheIndexHeader *hdr = reinterpret_cast<CacheIndexHeader *>(mRWBuf);
  hdr->mVersion = PR_htonl(kIndexVersion);
  hdr->mTimeStamp = PR_htonl(static_cast<uint32_t>(PR_Now() / PR_USEC_PER_SEC));
  hdr->mIsDirty = PR_htonl(1);

  mRWBufPos = sizeof(CacheIndexHeader);
  mSkipEntries = 0;
  WriteRecords();
}

void
CacheIndex::WriteRecords()
{
  LOG(("CacheIndex::WriteRecords()"));

  nsresult rv;

  mLock.AssertCurrentThreadOwns();

  int64_t fileOffset;

  if (mSkipEntries) {
    MOZ_ASSERT(mRWBufPos == 0);
    fileOffset = sizeof(CacheIndexHeader);
    fileOffset += sizeof(CacheIndexRecord) * mSkipEntries;
  }
  else {
    MOZ_ASSERT(mRWBufPos == sizeof(CacheIndexHeader));
    fileOffset = 0;
  }
  uint32_t hashOffset = mRWBufPos;

  WriteRecordsHelper data;
  data.mBuf = mRWBuf + mRWBufPos;
  data.mSkip = mSkipEntries;
  data.mProcessMax = (mRWBufSize - mRWBufPos) / sizeof(CacheIndexRecord);
  MOZ_ASSERT(data.mProcessMax != 0 || mProcessEntries == 0); // TODO make sure we can write an empty index
  data.mProcessed = 0;
#ifdef DEBUG
  data.mHasMore = false;
#endif

  mIndex.EnumerateEntries(&CacheIndex::CopyRecordsToRWBuf, &data);
  MOZ_ASSERT(mRWBufPos != static_cast<uint32_t>(data.mBuf - mRWBuf) ||
             mProcessEntries == 0);
  mRWBufPos = data.mBuf - mRWBuf;
  mSkipEntries += data.mProcessed;
  MOZ_ASSERT(mSkipEntries <= mProcessEntries);

  mHash->Update(mRWBuf + hashOffset, mRWBufPos - hashOffset);

  if (mSkipEntries == mProcessEntries) {
    MOZ_ASSERT(!data.mHasMore);

    // We've processed all records
    if (mRWBufPos + sizeof(CacheHash::Hash32_t) > mRWBufSize) {
      // realloc buffer to spare another write cycle
      mRWBufSize = mRWBufPos + sizeof(CacheHash::Hash32_t);
      mRWBuf = static_cast<char *>(moz_xrealloc(mRWBuf, mRWBufSize));
    }

    *(reinterpret_cast<uint32_t *>(mRWBuf + mRWBufPos)) =
      PR_htonl(mHash->GetHash());

    mRWBufPos += sizeof(CacheHash::Hash32_t);
    mSkipEntries = 0;
  }
  else {
    MOZ_ASSERT(data.mHasMore);
  }

  rv = CacheFileIOManager::Write(mHandle, fileOffset, mRWBuf, mRWBufPos,
                                 mSkipEntries ? false : true, this);
  if (NS_FAILED(rv)) {
    LOG(("CacheIndex::WriteRecords() - CacheFileIOManager::Write() failed "
         "synchronously [rv=0x%08x]", rv));
    FinishWrite(false);
  }

  mRWBufPos = 0;
}

void
CacheIndex::FinishWrite(bool aSucceeded)
{
  LOG(("CacheIndex::FinishWrite() [succeeded=%d]", aSucceeded));

  MOZ_ASSERT((!aSucceeded && mState == SHUTDOWN) || mState == WRITING);

  mLock.AssertCurrentThreadOwns();

  mHandle = nullptr;
  mHash = nullptr;
  ReleaseBuffer();

  if (aSucceeded) {
    mIndex.EnumerateEntries(&CacheIndex::ApplyIndexChanges, this);
    mIndexOnDiskIsValid = true;
  }

  ProcessPendingOperations();
  mIndexStats.Log();

  if (mState == WRITING) {
    ChangeState(READY);
    mLastDumpTime = PR_IntervalNow();
  }
}

PLDHashOperator
CacheIndex::CopyRecordsToRWBuf(CacheIndexEntry *aEntry, void* aClosure)
{
  if (aEntry->IsRemoved())
    return PL_DHASH_NEXT;

  if (!aEntry->IsInitialized()) {
    return PL_DHASH_NEXT;
  }

  if (aEntry->IsEmpty()) {
    return PL_DHASH_NEXT;
  }

  WriteRecordsHelper *data = static_cast<WriteRecordsHelper *>(aClosure);
  if (data->mSkip) {
    data->mSkip--;
    return PL_DHASH_NEXT;
  }

  if (data->mProcessed == data->mProcessMax) {
#ifdef DEBUG
    data->mHasMore = true;
#endif
    return PL_DHASH_STOP;
  }

  aEntry->WriteToBuf(data->mBuf);
  data->mBuf += sizeof(CacheIndexRecord);
  data->mProcessed++;

  return PL_DHASH_NEXT;
}

PLDHashOperator
CacheIndex::ApplyIndexChanges(CacheIndexEntry *aEntry, void* aClosure)
{
  CacheIndex *index = static_cast<CacheIndex *>(aClosure);

  CacheIndexEntryAutoManage emng(aEntry->Hash(), index);

  if (aEntry->IsRemoved()) {
    emng.DoNotSearchInIndex();
    return PL_DHASH_REMOVE;
  }

  if (aEntry->IsDirty())
    aEntry->ClearDirty();

  return PL_DHASH_NEXT;
}

nsresult
CacheIndex::GetFile(const nsACString &aName, nsIFile **_retval)
{
  nsresult rv;

  nsCOMPtr<nsIFile> file;
  rv = mCacheDirectory->Clone(getter_AddRefs(file));
  NS_ENSURE_SUCCESS(rv, rv);

  rv = file->AppendNative(aName);
  NS_ENSURE_SUCCESS(rv, rv);

  file.swap(*_retval);
  return NS_OK;
}

nsresult
CacheIndex::RemoveFile(const nsACString &aName)
{
  nsresult rv;

  nsCOMPtr<nsIFile> file;
  rv = GetFile(aName, getter_AddRefs(file));
  NS_ENSURE_SUCCESS(rv, rv);

  bool exists;
  rv = file->Exists(&exists);
  NS_ENSURE_SUCCESS(rv, rv);

  if (exists) {
    rv = file->Remove(false);
    if (NS_FAILED(rv)) {
      LOG(("CacheIndex::RemoveFile() - Cannot remove old entry file from disk."
           "[name=%s]", PromiseFlatCString(aName).get()));
      NS_WARNING("Cannot remove old entry file from the disk");
      return rv;
    }
  }

  return NS_OK;
}

void
CacheIndex::RemoveIndexFromDisk()
{
  LOG(("CacheIndex::RemoveIndexFromDisk()"));

  RemoveFile(NS_LITERAL_CSTRING(kIndexName));
  RemoveFile(NS_LITERAL_CSTRING(kTempIndexName));
  RemoveFile(NS_LITERAL_CSTRING(kJournalName));
}

class WriteLogHelper
{
public:
  WriteLogHelper(PRFileDesc *aFD)
    : mStatus(NS_OK)
    , mFD(aFD)
    , mBufSize(kMaxBufSize)
    , mBufPos(0)
  {
    mHash = new CacheHash();
    mBuf = static_cast<char *>(moz_xmalloc(mBufSize));
  }

  ~WriteLogHelper() {
    free(mBuf);
  }

  nsresult AddEntry(CacheIndexEntry *aEntry);
  nsresult Finish();

private:

  nsresult FlushBuffer();

  nsresult            mStatus;
  PRFileDesc         *mFD;
  char               *mBuf;
  uint32_t            mBufSize;
  int32_t             mBufPos;
  nsCOMPtr<CacheHash> mHash;
};

nsresult
WriteLogHelper::AddEntry(CacheIndexEntry *aEntry)
{
  nsresult rv;

  if (NS_FAILED(mStatus))
    return mStatus;

  if (mBufPos + sizeof(CacheIndexRecord) > mBufSize) {
    mHash->Update(mBuf, mBufPos);

    rv = FlushBuffer();
    if (NS_FAILED(rv)) {
      mStatus = rv;
      return rv;
    }
    MOZ_ASSERT(mBufPos + sizeof(CacheIndexRecord) <= mBufSize);
  }

  aEntry->WriteToBuf(mBuf + mBufPos);
  mBufPos += sizeof(CacheIndexRecord);

  return NS_OK;
}

nsresult
WriteLogHelper::Finish()
{
  nsresult rv;

  if (NS_FAILED(mStatus))
    return mStatus;

  mHash->Update(mBuf, mBufPos);
  if (mBufPos + sizeof(CacheHash::Hash32_t) > mBufSize) {
    rv = FlushBuffer();
    if (NS_FAILED(rv)) {
      mStatus = rv;
      return rv;
    }
    MOZ_ASSERT(mBufPos + sizeof(CacheHash::Hash32_t) <= mBufSize);
  }

  *(reinterpret_cast<uint32_t *>(mBuf + mBufPos)) = PR_htonl(mHash->GetHash());
  mBufPos += sizeof(CacheHash::Hash32_t);

  rv = FlushBuffer();
  NS_ENSURE_SUCCESS(rv, rv);

  mStatus = NS_ERROR_UNEXPECTED; // Don't allow any other operation
  return NS_OK;
}

nsresult
WriteLogHelper::FlushBuffer()
{
  MOZ_ASSERT(NS_SUCCEEDED(mStatus));

  int32_t bytesWritten = PR_Write(mFD, mBuf, mBufPos);

  if (bytesWritten != mBufPos)
    return NS_ERROR_FAILURE;

  mBufPos = 0;
  return NS_OK;
}

nsresult
CacheIndex::WriteLogToDisk()
{
  LOG(("CacheIndex::WriteLogToDisk()"));

  nsresult rv;

  MOZ_ASSERT(mPendingUpdates.Count() == 0);

  RemoveFile(NS_LITERAL_CSTRING(kTempIndexName));

  nsCOMPtr<nsIFile> indexFile;
  rv = GetFile(NS_LITERAL_CSTRING(kIndexName), getter_AddRefs(indexFile));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIFile> logFile;
  rv = GetFile(NS_LITERAL_CSTRING(kJournalName), getter_AddRefs(logFile));
  NS_ENSURE_SUCCESS(rv, rv);

  mIndexStats.Log();

  PRFileDesc *fd = nullptr;
  rv = logFile->OpenNSPRFileDesc(PR_RDWR | PR_CREATE_FILE | PR_TRUNCATE,
                                 0600, &fd);
  NS_ENSURE_SUCCESS(rv, rv);

  WriteLogHelper wlh(fd);
  mIndex.EnumerateEntries(&CacheIndex::WriteEntryToLog, &wlh);

  rv = wlh.Finish();
  PR_Close(fd);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = indexFile->OpenNSPRFileDesc(PR_RDWR, 0600, &fd);
  NS_ENSURE_SUCCESS(rv, rv);

  CacheIndexHeader header;
  int32_t bytesRead = PR_Read(fd, &header, sizeof(CacheIndexHeader));
  if (bytesRead != sizeof(CacheIndexHeader)) {
    PR_Close(fd);
    return NS_ERROR_FAILURE;
  }

  header.mIsDirty = PR_htonl(0);

  int64_t offset = PR_Seek64(fd, 0, PR_SEEK_SET);
  if (offset == -1) {
    PR_Close(fd);
    return NS_ERROR_FAILURE;
  }

  int32_t bytesWritten = PR_Write(fd, &header, sizeof(CacheIndexHeader));
  PR_Close(fd);
  if (bytesWritten != sizeof(CacheIndexHeader)) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

PLDHashOperator
CacheIndex::WriteEntryToLog(CacheIndexEntry *aEntry, void* aClosure)
{
  WriteLogHelper *wlh = static_cast<WriteLogHelper *>(aClosure);

  if (aEntry->IsRemoved() || aEntry->IsDirty()) {
    wlh->AddEntry(aEntry);
  }

  return PL_DHASH_REMOVE;
}

void
CacheIndex::ReadIndexFromDisk()
{
  LOG(("CacheIndex::ReadIndexFromDisk()"));

  nsresult rv;

  MutexAutoLock lock(mLock);

  MOZ_ASSERT(mState == READING);

  mReadFailed = false;
  mReadOpenCount = 0;

  rv = CacheFileIOManager::OpenFile(NS_LITERAL_CSTRING(kIndexName),
                                    CacheFileIOManager::SPECIAL_FILE |
                                    CacheFileIOManager::OPEN,
                                    this);
  if (NS_FAILED(rv)) {
    LOG(("CacheIndex::ReadIndexFromDisk() - CacheFileIOManager::OpenFile() "
         "failed [rv=0x%08x, file=%s]", rv, kIndexName));
    mReadFailed = true;
  }
  else
    mReadOpenCount++;

  rv = CacheFileIOManager::OpenFile(NS_LITERAL_CSTRING(kJournalName),
                                    CacheFileIOManager::SPECIAL_FILE |
                                    CacheFileIOManager::OPEN,
                                    this);
  if (NS_FAILED(rv)) {
    LOG(("CacheIndex::ReadIndexFromDisk() - CacheFileIOManager::OpenFile() "
         "failed [rv=0x%08x, file=%s]", rv, kJournalName));
    mReadFailed = true;
  }
  else
    mReadOpenCount++;

  rv = CacheFileIOManager::OpenFile(NS_LITERAL_CSTRING(kTempIndexName),
                                    CacheFileIOManager::SPECIAL_FILE |
                                    CacheFileIOManager::OPEN,
                                    this);
  if (NS_FAILED(rv)) {
    LOG(("CacheIndex::ReadIndexFromDisk() - CacheFileIOManager::OpenFile() "
         "failed [rv=0x%08x, file=%s]", rv, kTempIndexName));
    mReadFailed = true;
  }
  else
    mReadOpenCount++;

  if (mReadOpenCount == 0)
    FinishRead(false);
}

void
CacheIndex::StartReadingIndex()
{
  LOG(("CacheIndex::StartReadingIndex()"));

  nsresult rv;

  mLock.AssertCurrentThreadOwns();

  MOZ_ASSERT(mHandle);
  MOZ_ASSERT(mState == READING);
  MOZ_ASSERT(!mIndexOnDiskIsValid);
  MOZ_ASSERT(!mDontMarkIndexClean);
  MOZ_ASSERT(!mJournalReadSuccessfully);
  MOZ_ASSERT(mHandle->FileSize() >= 0);

  int64_t entriesSize = mHandle->FileSize() - sizeof(CacheIndexHeader) -
                        sizeof(CacheHash::Hash32_t);

  if (entriesSize < 0 || entriesSize % sizeof(CacheIndexRecord)) {
    LOG(("CacheIndex::StartReadingIndex() - Index is corrupted"));
    FinishRead(false);
    return;
  }

  AllocBuffer();
  mSkipEntries = 0;
  mHash = new CacheHash();

  mRWBufPos = std::min(mRWBufSize, static_cast<uint32_t>(mHandle->FileSize()));

  rv = CacheFileIOManager::Read(mHandle, 0, mRWBuf, mRWBufPos, this);
  if (NS_FAILED(rv)) {
    LOG(("CacheIndex::StartReadingIndex() - CacheFileIOManager::Read() failed "
         "synchronously [rv=0x%08x]", rv));
    FinishRead(false);
  }
}

void
CacheIndex::ParseRecords()
{
  LOG(("CacheIndex::ParseRecords()"));

  nsresult rv;

  mLock.AssertCurrentThreadOwns();

  uint32_t entryCnt = (mHandle->FileSize() - sizeof(CacheIndexHeader) -
                     sizeof(CacheHash::Hash32_t)) / sizeof(CacheIndexRecord);
  uint32_t pos = 0;

  if (!mSkipEntries) {
    CacheIndexHeader *hdr = reinterpret_cast<CacheIndexHeader *>(
                              moz_xmalloc(sizeof(CacheIndexHeader)));
    memcpy(hdr, mRWBuf, sizeof(CacheIndexHeader));

    if (PR_ntohl(hdr->mVersion) != kIndexVersion) {
      free(hdr);
      FinishRead(false);
      return;
    }

    mIndexTimeStamp = PR_ntohl(hdr->mTimeStamp);

    if (PR_ntohl(hdr->mIsDirty)) {
      if (mHandle2) {
        CacheFileIOManager::DoomFile(mHandle2, nullptr);
        mHandle2 = nullptr;
      }
      free(hdr);
    }
    else {
      hdr->mIsDirty = PR_htonl(1);
      // Mark index dirty
      rv = CacheFileIOManager::Write(mHandle, 0, reinterpret_cast<char *>(hdr),
                                     sizeof(CacheIndexHeader), true, nullptr);
      if (NS_FAILED(rv)) {
        // This is not fatal, just free the memory
        free(hdr);
      }
    }

    pos += sizeof(CacheIndexHeader);
  }

  uint32_t hashOffset = pos;

  while (pos + sizeof(CacheIndexRecord) <= mRWBufPos &&
         mSkipEntries != entryCnt) {
    CacheIndexRecord *rec = reinterpret_cast<CacheIndexRecord *>(mRWBuf + pos);
    CacheIndexEntry tmpEntry(&rec->mHash);
    tmpEntry.ReadFromBuf(mRWBuf + pos);

    if (tmpEntry.IsDirty() || !tmpEntry.IsInitialized() || tmpEntry.IsEmpty() ||
        tmpEntry.IsFresh() || tmpEntry.IsRemoved()) {
      LOG(("CacheIndex::ParseRecords() - Invalid entry found in index, removing"
           " whole index [dirty=%d, initialized=%d, empty=%d, fresh=%d, "
           "removed=%d]", tmpEntry.IsDirty(), tmpEntry.IsInitialized(),
           tmpEntry.IsEmpty(), tmpEntry.IsFresh(), tmpEntry.IsRemoved()));
      FinishRead(false);
      return;
    }

    CacheIndexEntryAutoManage emng(tmpEntry.Hash(), this);

    CacheIndexEntry *entry = mIndex.PutEntry(*tmpEntry.Hash());
    *entry = tmpEntry;

    pos += sizeof(CacheIndexRecord);
    mSkipEntries++;
  }

  mHash->Update(mRWBuf + hashOffset, pos - hashOffset);

  if (pos != mRWBufPos) {
    memmove(mRWBuf, mRWBuf + pos, mRWBufPos - pos);
    mRWBufPos -= pos;
    pos = 0;
  }

  int64_t fileOffset = sizeof(CacheIndexHeader) +
                       mSkipEntries * sizeof(CacheIndexRecord) + mRWBufPos;

  MOZ_ASSERT(fileOffset <= mHandle->FileSize());
  if (fileOffset == mHandle->FileSize()) {
    if (mHash->GetHash() != PR_ntohl(*(reinterpret_cast<uint32_t *>(mRWBuf)))) {
      LOG(("CacheIndex::ParseRecords() - Hash mismatch, [is %x, should be %x]",
           mHash->GetHash(),
           PR_ntohl(*(reinterpret_cast<uint32_t *>(mRWBuf)))));
      FinishRead(false);
      return;
    }

    mIndexOnDiskIsValid = true;
    mJournalReadSuccessfully = false;

    if (mHandle2)
      StartReadingJournal();
    else
      FinishRead(false);

    return;
  }

  pos = mRWBufPos;
  uint32_t toRead = std::min(mRWBufSize - pos,
                             static_cast<uint32_t>(mHandle->FileSize() -
                                                   fileOffset));
  mRWBufPos = pos + toRead;

  rv = CacheFileIOManager::Read(mHandle, fileOffset, mRWBuf + pos, toRead,
                                this);
  if (NS_FAILED(rv)) {
    LOG(("CacheIndex::ParseRecords() - CacheFileIOManager::Read() failed "
         "synchronously [rv=0x%08x]", rv));
    FinishRead(false);
    return;
  }
}

void
CacheIndex::StartReadingJournal()
{
  LOG(("CacheIndex::StartReadingJournal()"));

  nsresult rv;

  mLock.AssertCurrentThreadOwns();

  MOZ_ASSERT(mHandle2);
  MOZ_ASSERT(mIndexOnDiskIsValid);
  MOZ_ASSERT(mTmpJournal.Count() == 0);
  MOZ_ASSERT(mHandle2->FileSize() >= 0);

  int64_t entriesSize = mHandle2->FileSize() - sizeof(CacheHash::Hash32_t);

  if (entriesSize < 0 || entriesSize % sizeof(CacheIndexRecord)) {
    LOG(("CacheIndex::StartReadingJournal() - Journal is corrupted"));
    FinishRead(false);
    return;
  }

  mSkipEntries = 0;
  mHash = new CacheHash();

  mRWBufPos = std::min(mRWBufSize, static_cast<uint32_t>(mHandle2->FileSize()));

  rv = CacheFileIOManager::Read(mHandle2, 0, mRWBuf, mRWBufPos, this);
  if (NS_FAILED(rv)) {
    LOG(("CacheIndex::StartReadingJournal() - CacheFileIOManager::Read() failed"
         " synchronously [rv=0x%08x]", rv));
    FinishRead(false);
  }
}

void
CacheIndex::ParseJournal()
{
  LOG(("CacheIndex::ParseRecords()"));

  nsresult rv;

  mLock.AssertCurrentThreadOwns();

  uint32_t entryCnt = (mHandle2->FileSize() - sizeof(CacheHash::Hash32_t)) /
                      sizeof(CacheIndexRecord);

  uint32_t pos = 0;

  while (pos + sizeof(CacheIndexRecord) <= mRWBufPos &&
         mSkipEntries != entryCnt) {
    CacheIndexRecord *rec = reinterpret_cast<CacheIndexRecord *>(mRWBuf + pos);
    CacheIndexEntry tmpEntry(&rec->mHash);
    tmpEntry.ReadFromBuf(mRWBuf + pos);

    CacheIndexEntry *entry = mTmpJournal.PutEntry(*tmpEntry.Hash());
    *entry = tmpEntry;

    if (entry->IsDirty() || entry->IsFresh()) {
      LOG(("CacheIndex::ParseJournal() - Invalid entry found in journal, "
           "ignoring whole journal [dirty=%d, fresh=%d]", entry->IsDirty(),
           entry->IsFresh()));
      FinishRead(false);
      return;
    }

    pos += sizeof(CacheIndexRecord);
    mSkipEntries++;
  }

  mHash->Update(mRWBuf, pos);

  if (pos != mRWBufPos) {
    memmove(mRWBuf, mRWBuf + pos, mRWBufPos - pos);
    mRWBufPos -= pos;
    pos = 0;
  }

  int64_t fileOffset = mSkipEntries * sizeof(CacheIndexRecord) + mRWBufPos;

  MOZ_ASSERT(fileOffset <= mHandle2->FileSize());
  if (fileOffset == mHandle2->FileSize()) {
    if (mHash->GetHash() != PR_ntohl(*(reinterpret_cast<uint32_t *>(mRWBuf)))) {
      LOG(("CacheIndex::ParseJournal() - Hash mismatch, [is %x, should be %x]",
           mHash->GetHash(),
           PR_ntohl(*(reinterpret_cast<uint32_t *>(mRWBuf)))));
      FinishRead(false);
      return;
    }

    mJournalReadSuccessfully = true;
    FinishRead(true);
    return;
  }

  pos = mRWBufPos;
  uint32_t toRead = std::min(mRWBufSize - pos,
                             static_cast<uint32_t>(mHandle2->FileSize() -
                                                   fileOffset));
  mRWBufPos = pos + toRead;

  rv = CacheFileIOManager::Read(mHandle2, fileOffset, mRWBuf + pos, toRead,
                                this);
  if (NS_FAILED(rv)) {
    LOG(("CacheIndex::ParseJournal() - CacheFileIOManager::Read() failed "
         "synchronously [rv=0x%08x]", rv));
    FinishRead(false);
    return;
  }
}

void
CacheIndex::MergeJournal()
{
  LOG(("CacheIndex::MergeJournal()"));

  mLock.AssertCurrentThreadOwns();

  mTmpJournal.EnumerateEntries(&CacheIndex::ProcessJournalEntry, this);

  MOZ_ASSERT(mTmpJournal.Count() == 0);
}

PLDHashOperator
CacheIndex::ProcessJournalEntry(CacheIndexEntry *aEntry, void* aClosure)
{
  CacheIndex *index = static_cast<CacheIndex *>(aClosure);

  LOG(("CacheFile::ProcessJournalEntry() [hash=%08x%08x%08x%08x%08x]",
       LOGSHA1(aEntry->Hash())));

  CacheIndexEntry *entry = index->mIndex.GetEntry(*aEntry->Hash());

  CacheIndexEntryAutoManage emng(aEntry->Hash(), index);

  if (aEntry->IsRemoved()) {
    if (entry) {
      entry->MarkRemoved();
      entry->MarkDirty();
    }
  }
  else {
    if (!entry)
      entry = index->mIndex.PutEntry(*aEntry->Hash());

    *entry = *aEntry;
    entry->MarkDirty();
  }

  return PL_DHASH_REMOVE;
}

void
CacheIndex::EnsureNoFreshEntry()
{
#ifdef DEBUG_STATS
  CacheIndexStats debugStats;
  debugStats.DisableLogging();
  mIndex.EnumerateEntries(&CacheIndex::SumIndexStats, &debugStats);
  MOZ_ASSERT(debugStats.Fresh() == 0);
#endif
}

void
CacheIndex::EnsureCorrectStats()
{
#ifdef DEBUG_STATS
  MOZ_ASSERT(mPendingUpdates.Count() == 0);
  CacheIndexStats debugStats;
  debugStats.DisableLogging();
  mIndex.EnumerateEntries(&CacheIndex::SumIndexStats, &debugStats);
  MOZ_ASSERT(debugStats == mIndexStats);
#endif
}

PLDHashOperator
CacheIndex::SumIndexStats(CacheIndexEntry *aEntry, void* aClosure)
{
  CacheIndexStats *stats = static_cast<CacheIndexStats *>(aClosure);
  stats->BeforeChange(nullptr);
  stats->AfterChange(aEntry);
  return PL_DHASH_NEXT;
}

void
CacheIndex::FinishRead(bool aSucceeded)
{
  LOG(("CacheIndex::FinishRead() [succeeded=%d]", aSucceeded));
  mLock.AssertCurrentThreadOwns();

  MOZ_ASSERT((!aSucceeded && mState == SHUTDOWN) || mState == READING);

  MOZ_ASSERT(
    // -> rebuild
    (!aSucceeded && !mIndexOnDiskIsValid && !mJournalReadSuccessfully) ||
    // -> update
    (!aSucceeded && mIndexOnDiskIsValid && !mJournalReadSuccessfully) ||
    // -> ready
    (aSucceeded && mIndexOnDiskIsValid && mJournalReadSuccessfully));

  if (mState == SHUTDOWN) {
    RemoveFile(NS_LITERAL_CSTRING(kTempIndexName));
    RemoveFile(NS_LITERAL_CSTRING(kJournalName));
  }
  else {
    if (mHandle && !mIndexOnDiskIsValid)
      CacheFileIOManager::DoomFile(mHandle, nullptr);
    if (mHandle2)
      CacheFileIOManager::DoomFile(mHandle2, nullptr);
  }

  mHandle = nullptr;
  mHandle2 = nullptr;
  mHash = nullptr;
  ReleaseBuffer();

  if (mState == SHUTDOWN)
    return;

  if (!mIndexOnDiskIsValid) {
    MOZ_ASSERT(mTmpJournal.Count() == 0);
    EnsureNoFreshEntry();
    ProcessPendingOperations();
    // Remove all entres that we haven't seen during this session
    mIndex.EnumerateEntries(&CacheIndex::RemoveNonFreshEntries, this);
    StartBuildingIndex();
    return;
  }

  if (!mJournalReadSuccessfully) {
    mTmpJournal.Clear();
    EnsureNoFreshEntry();
    ProcessPendingOperations();
    StartUpdatingIndex();
    return;
  }

  MergeJournal();
  EnsureNoFreshEntry();
  ProcessPendingOperations();
  mIndexStats.Log();

  ChangeState(READY);
  mLastDumpTime = PR_IntervalNow(); // Do not dump new index immediately
}

nsresult
CacheIndex::SetupDirectoryEnumerator()
{
  MOZ_ASSERT(!NS_IsMainThread());
  MOZ_ASSERT(!mDirEnumerator);

  nsresult rv;
  nsCOMPtr<nsIFile> file;

  rv = mCacheDirectory->Clone(getter_AddRefs(file));
  NS_ENSURE_SUCCESS(rv, rv);

  rv = file->AppendNative(NS_LITERAL_CSTRING(kEntriesDir));
  NS_ENSURE_SUCCESS(rv, rv);

  bool exists;
  rv = file->Exists(&exists);
  NS_ENSURE_SUCCESS(rv, rv);

  if (!exists) {
    NS_WARNING("CacheIndex::SetupDirectoryEnumerator() - Entries directory "
               "doesn't exist!");
    LOG(("CacheIndex::SetupDirectoryEnumerator() - Entries directory doesn't "
          "exist!" ));
    return NS_ERROR_UNEXPECTED;
  }

  nsCOMPtr<nsISimpleEnumerator> enumerator;
  rv = file->GetDirectoryEntries(getter_AddRefs(enumerator));
  NS_ENSURE_SUCCESS(rv, rv);

  mDirEnumerator = do_QueryInterface(enumerator, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

void
CacheIndex::BuildUpdateInitEntry(CacheIndexEntry *aEntry,
                                 CacheFileMetadata *aMetaData,
                                 nsIFile *aFile,
                                 const nsACString &aLeafName)
{
  nsresult rv;

  aEntry->Init(aMetaData->AppId(), aMetaData->IsAnonymous(),
               aMetaData->IsInBrowser());
  aEntry->MarkDirty();
  aEntry->MarkFresh();

  uint32_t expirationTime;
  aMetaData->GetExpirationTime(&expirationTime);
  aEntry->SetExpirationTime(expirationTime);

  uint32_t frecency;
  aMetaData->GetFrecency(&frecency);
  aEntry->SetFrecency(frecency);

  int64_t size64 = 0;
  rv = aFile->GetFileSize(&size64);
  if (NS_FAILED(rv)) {
    LOG(("CacheIndex::BuildUpdateInitEntry() - Cannot get filesize of file that"
         " was successfully parsed. [name=%s]",
         PromiseFlatCString(aLeafName).get()));
  }

  if (size64 & 0x3FF)
    size64 += 0x400;

  size64 >>= 10;

  uint32_t size;
  if (size64 >> 32) {
    NS_WARNING("CacheIndex::BuildUpdateInitEntry() - FileSize is too large, "
               "truncating to PR_UINT32_MAX");
    size = PR_UINT32_MAX;
  }
  else {
    size = static_cast<uint32_t>(size64);
  }

  if (size & ~CacheIndexEntry::eFileSizeMask) {
    LOG(("CacheIndex::BuildUpdateInitEntry() - FileSize is too big, truncating "
         "to %u", CacheIndexEntry::eFileSizeMask));
    size = CacheIndexEntry::eFileSizeMask;
  }
  aEntry->SetFileSize(size);
}

void
CacheIndex::StartBuildingIndex()
{
  LOG(("CacheIndex::StartBuildingIndex()"));

  nsresult rv;

  MOZ_ASSERT(mPendingUpdates.Count() == 0);

  ChangeState(BUILDING);
  mDontMarkIndexClean = false;

  if (mShuttingDown) {
    FinishBuild(false);
    return;
  }

  nsRefPtr<CacheIOThread> ioThread = CacheFileIOManager::IOThread();
  MOZ_ASSERT(ioThread);

  if (ioThread->IsCurrentThread()) {
    BuildIndex();
  }
  else {
    rv = ioThread->Dispatch(this, CacheIOThread::BUILD_OR_UPDATE_INDEX);
    if (NS_FAILED(rv)) {
      NS_WARNING("CacheIndex::StartBuildingIndex() - Can't dispatch event");
      LOG(("CacheIndex::StartBuildingIndex() - Can't dispatch event" ));
      FinishBuild(false);
    }
  }
}

void
CacheIndex::BuildIndex()
{
  mLock.AssertCurrentThreadOwns();

  MOZ_ASSERT(mPendingUpdates.Count() == 0);

  nsresult rv;

  if (!mDirEnumerator) {
    rv = SetupDirectoryEnumerator();
    if (NS_FAILED(rv)) {
      FinishBuild(false);
      return;
    }
  }

  while (true) {
    nsCOMPtr<nsIFile> file;
    rv = mDirEnumerator->GetNextFile(getter_AddRefs(file));
    if (!file) {
      FinishBuild(NS_SUCCEEDED(rv) ? true : false);
      return;
    }

    nsAutoCString leaf;
    rv = file->GetNativeLeafName(leaf);
    if (NS_FAILED(rv)) {
      LOG(("CacheIndex::BuildIndex() - GetNativeLeafName() failed! Skipping "
           "file."));
      mDontMarkIndexClean = true;
      continue;
    }

    SHA1Sum::Hash hash;
    rv = CacheFileIOManager::StrToHash(leaf, &hash);
    if (NS_FAILED(rv)) {
      LOG(("CacheIndex::BuildIndex() - Filename is not a hash, removing file. "
           "[name=%s]", leaf.get()));
      file->Remove(false);
      continue;
    }

    CacheIndexEntry *entry = mIndex.GetEntry(hash);
    if (entry && entry->IsRemoved()) {
      LOG(("CacheIndex::BuildIndex() - Found file that should not exist. "
           "[name=%s]", leaf.get()));
      entry->Log();
      MOZ_ASSERT(entry->IsFresh());
      entry = nullptr;
    }

#ifdef DEBUG
    nsRefPtr<CacheFileHandle> handle;
    CacheFileIOManager::gInstance->mHandles.GetHandle(&hash,
                                                      getter_AddRefs(handle));
#endif

    if (entry) {
      // the entry is up to date
      LOG(("CacheIndex::BuildIndex() - Skipping file because the entry is up to"
           " date. [name=%s]", leaf.get()));
      entry->Log();
      MOZ_ASSERT(entry->IsFresh()); // The entry must be from this session
      // there must be an active CacheFile if the entry is not initialized
      MOZ_ASSERT(entry->IsInitialized() || handle);
      continue;
    }

    MOZ_ASSERT(!handle);

    nsRefPtr<CacheFileMetadata> meta = new CacheFileMetadata();

    {
      MutexAutoUnlock unlock(mLock);
      rv = meta->SyncReadMetadata(file);
    }

    // Nobody should add the entry while the lock was released
    entry = mIndex.GetEntry(hash);
    MOZ_ASSERT(!entry || entry->IsRemoved());

    if (NS_FAILED(rv)) {
      LOG(("CacheIndex::BuildIndex() - CacheFileMetadata::SyncReadMetadata() "
           "failed, removing file. [name=%s]", leaf.get()));
      file->Remove(false);
    } else {
      CacheIndexEntryAutoManage entryMng(&hash, this);
      entry = mIndex.PutEntry(hash);
      BuildUpdateInitEntry(entry, meta, file, leaf);
      LOG(("CacheIndex::BuildIndex() - Added entry to index. [hash=%s]",
           leaf.get()));
      entry->Log();
    }

    break;
  }

  nsRefPtr<CacheIOThread> ioThread = CacheFileIOManager::IOThread();
  MOZ_ASSERT(ioThread);

  rv = ioThread->Dispatch(this, CacheIOThread::BUILD_OR_UPDATE_INDEX);
  if (NS_FAILED(rv)) {
    NS_WARNING("CacheIndex::BuildIndex() - Can't dispatch event");
    LOG(("CacheIndex::BuildIndex() - Can't dispatch event" ));
    FinishBuild(false);
    return;
  }
}

void
CacheIndex::FinishBuild(bool aSucceeded)
{
  LOG(("CacheIndex::FinishBuild() [succeeded=%d]", aSucceeded));

  MOZ_ASSERT((!aSucceeded && mState == SHUTDOWN) || mState == BUILDING);

  mLock.AssertCurrentThreadOwns();

  if (mDirEnumerator) {
    if (NS_IsMainThread()) {
      LOG(("CacheIndex::FinishBuild() - posting of PreShutdownInternal failed? "
           "Cannot safely release mDirEnumerator, leaking it!"));
      // This can happen only in case dispatching event to IO thread failed in
      // CacheIndex::PreShutdown().
      mDirEnumerator.forget(); // Leak it!
    }
    else {
      mDirEnumerator->Close();
      mDirEnumerator = nullptr;
    }
  }

  mDontMarkIndexClean = (!aSucceeded || mDontMarkIndexClean) ? true : false;

  if (mState == BUILDING) {
    ChangeState(READY);
    mLastDumpTime = PR_IntervalNow(); // Do not dump new index immediately
  }
}

bool
CacheIndex::StartUpdatingIndexIfNeeded(bool aSwitchingToReadyState)
{
  // Start updating process when we are in or we are switching to READY state
  // and index needs update, but not during shutdown.
  if ((mState == READY || aSwitchingToReadyState) && mIndexNeedsUpdate &&
      !mShuttingDown) {
    LOG(("CacheIndex::StartUpdatingIndexIfNeeded() - starting update process"));
    mIndexNeedsUpdate = false;
    StartUpdatingIndex();
    return true;
  }

  return false;
}

void
CacheIndex::StartUpdatingIndex()
{
  LOG(("CacheIndex::StartUpdatingIndex()"));

  nsresult rv;

  mIndexStats.Log();

  MOZ_ASSERT(mPendingUpdates.Count() == 0);
  MOZ_ASSERT(mIndexOnDiskIsValid);
  ChangeState(UPDATING);
  mDontMarkIndexClean = false;

  if (mShuttingDown) {
    FinishUpdate(false);
    return;
  }

  nsRefPtr<CacheIOThread> ioThread = CacheFileIOManager::IOThread();
  MOZ_ASSERT(ioThread);

  if (ioThread->IsCurrentThread()) {
    UpdateIndex();
  }
  else {
    rv = ioThread->Dispatch(this, CacheIOThread::BUILD_OR_UPDATE_INDEX);
    if (NS_FAILED(rv)) {
      NS_WARNING("CacheIndex::StartUpdatingIndex() - Can't dispatch event");
      LOG(("CacheIndex::StartUpdatingIndex() - Can't dispatch event" ));
      FinishUpdate(false);
    }
  }
}

void
CacheIndex::UpdateIndex()
{
  mLock.AssertCurrentThreadOwns();

  MOZ_ASSERT(mPendingUpdates.Count() == 0);

  nsresult rv;

  if (!mDirEnumerator) {
    rv = SetupDirectoryEnumerator();
    if (NS_FAILED(rv)) {
      FinishUpdate(false);
      return;
    }
  }

  while (true) {
    nsCOMPtr<nsIFile> file;
    rv = mDirEnumerator->GetNextFile(getter_AddRefs(file));
    if (!file) {
      FinishUpdate(NS_SUCCEEDED(rv) ? true : false);
      return;
    }

    nsAutoCString leaf;
    rv = file->GetNativeLeafName(leaf);
    if (NS_FAILED(rv)) {
      LOG(("CacheIndex::UpdateIndex() - GetNativeLeafName() failed! Skipping "
           "file."));
      mDontMarkIndexClean = true;
      continue;
    }

    SHA1Sum::Hash hash;
    rv = CacheFileIOManager::StrToHash(leaf, &hash);
    if (NS_FAILED(rv)) {
      LOG(("CacheIndex::UpdateIndex() - Filename is not a hash, removing file. "
           "[name=%s]", leaf.get()));
      file->Remove(false);
      continue;
    }

    CacheIndexEntry *entry = mIndex.GetEntry(hash);
    if (entry && entry->IsRemoved()) {
      if (entry->IsFresh()) {
        LOG(("CacheIndex::UpdateIndex() - Found file that should not exist. "
             "[name=%s]", leaf.get()));
        entry->Log();
      }
      entry = nullptr;
    }

#ifdef DEBUG
    nsRefPtr<CacheFileHandle> handle;
    CacheFileIOManager::gInstance->mHandles.GetHandle(&hash,
                                                      getter_AddRefs(handle));
#endif

    if (entry && entry->IsFresh()) {
      // the entry is up to date
      LOG(("CacheIndex::UpdateIndex() - Skipping file because the entry is up "
           " to date. [name=%s]", leaf.get()));
      entry->Log();
      // there must be an active CacheFile if the entry is not initialized
      MOZ_ASSERT(entry->IsInitialized() || handle);
      continue;
    }

    MOZ_ASSERT(!handle);

    if (entry) {
      PRTime lastModifiedTime;
      rv = file->GetLastModifiedTime(&lastModifiedTime);
      if (NS_FAILED(rv)) {
        LOG(("CacheIndex::UpdateIndex() - Cannot get lastModifiedTime. "
             "[name=%s]", leaf.get()));
        // Assume the file is newer than index
      }
      else {
        if (mIndexTimeStamp > (lastModifiedTime / PR_MSEC_PER_SEC)) {
          LOG(("CacheIndex::UpdateIndex() - Skipping file because of last "
               "modified time. [name=%s, indexTimeStamp=%u, "
               "lastModifiedTime=%u]", leaf.get(), mIndexTimeStamp,
               lastModifiedTime / PR_MSEC_PER_SEC));

          CacheIndexEntryAutoManage entryMng(&hash, this);
          entry->MarkFresh();
          continue;
        }
      }
    }

    nsRefPtr<CacheFileMetadata> meta = new CacheFileMetadata();

    {
      MutexAutoUnlock unlock(mLock);
      rv = meta->SyncReadMetadata(file);
    }

    // Nobody should add the entry while the lock was released
    entry = mIndex.GetEntry(hash);
    MOZ_ASSERT(!entry || !entry->IsFresh());

    CacheIndexEntryAutoManage entryMng(&hash, this);

    if (NS_FAILED(rv)) {
      LOG(("CacheIndex::UpdateIndex() - CacheFileMetadata::SyncReadMetadata() "
           "failed, removing file. [name=%s]", leaf.get()));
      file->Remove(false);
      if (entry) {
        entry->MarkRemoved();
        entry->MarkFresh();
        entry->MarkDirty();
      }
    } else {
      entry = mIndex.PutEntry(hash);
      BuildUpdateInitEntry(entry, meta, file, leaf);
      LOG(("CacheIndex::UpdateIndex() - Added/updated entry to/in index. "
           "[hash=%s]", leaf.get()));
      entry->Log();
    }

    break;
  }

  nsRefPtr<CacheIOThread> ioThread = CacheFileIOManager::IOThread();
  MOZ_ASSERT(ioThread);

  rv = ioThread->Dispatch(this, CacheIOThread::BUILD_OR_UPDATE_INDEX);
  if (NS_FAILED(rv)) {
    NS_WARNING("CacheIndex::UpdateIndex() - Can't dispatch event");
    LOG(("CacheIndex::UpdateIndex() - Can't dispatch event" ));
    FinishUpdate(false);
    return;
  }
}

void
CacheIndex::FinishUpdate(bool aSucceeded)
{
  LOG(("CacheIndex::FinishUpdate() [succeeded=%d]", aSucceeded));

  MOZ_ASSERT((!aSucceeded && mState == SHUTDOWN) || mState == UPDATING);

  mLock.AssertCurrentThreadOwns();

  if (mDirEnumerator) {
    if (NS_IsMainThread()) {
      LOG(("CacheIndex::FinishUpdate() - posting of PreShutdownInternal failed?"
           " Cannot safely release mDirEnumerator, leaking it!"));
      // This can happen only in case dispatching event to IO thread failed in
      // CacheIndex::PreShutdown().
      mDirEnumerator.forget(); // Leak it!
    }
    else {
      mDirEnumerator->Close();
      mDirEnumerator = nullptr;
    }
  }

  mDontMarkIndexClean = (!aSucceeded || mDontMarkIndexClean) ? true : false;

  if (mState == UPDATING) {
    if (aSucceeded) {
      // Delete all non-fresh entries only if we iterated all entries
      // successfully.
      mIndex.EnumerateEntries(&CacheIndex::RemoveNonFreshEntries, this);
    }

    ChangeState(READY);
    mLastDumpTime = PR_IntervalNow(); // Do not dump new index immediately
  }
}

PLDHashOperator
CacheIndex::RemoveNonFreshEntries(CacheIndexEntry *aEntry, void* aClosure)
{
  if (aEntry->IsFresh())
    return PL_DHASH_NEXT;

  LOG(("CacheFile::RemoveNonFreshEntries() - Removing entry. "
       "[hash=%08x%08x%08x%08x%08x]", LOGSHA1(aEntry->Hash())));

  CacheIndex *index = static_cast<CacheIndex *>(aClosure);

  CacheIndexEntryAutoManage emng(aEntry->Hash(), index);
  emng.DoNotSearchInIndex();

  return PL_DHASH_REMOVE;
}

#ifdef MOZ_LOGGING
char const *
CacheIndex::StateString(EState aState)
{
  switch (aState) {
    case INITIAL:  return "INITIAL";
    case READING:  return "READING";
    case WRITING:  return "WRITING";
    case BUILDING: return "BUILDING";
    case UPDATING: return "UPDATING";
    case READY:    return "READY";
    case SHUTDOWN: return "SHUTDOWN";
  }

  MOZ_ASSERT(false, "Unexpected state");
  return "?";
}
#endif

void
CacheIndex::ChangeState(EState aNewState)
{
#ifdef MOZ_LOGGING
  LOG(("CacheIndex::ChangeState() changing state %s -> %s", StateString(mState),
       StateString(aNewState)));
#endif

  // All pending updates should be processed before changing state
  MOZ_ASSERT(mPendingUpdates.Count() == 0);

  // PreShutdownInternal() should change the state to READY from every state. It
  // may go through different states, but once we are in READY state the only
  // possible transition is to SHUTDOWN state.
  MOZ_ASSERT(!mShuttingDown || mState != READY || aNewState == SHUTDOWN);

  // Start updating process when switching to READY state if needed
  if (aNewState == READY && StartUpdatingIndexIfNeeded(true))
    return;

  mState = aNewState;
}

void
CacheIndex::AllocBuffer()
{
  if (mState == WRITING) {
    mRWBufSize = sizeof(CacheIndexHeader) + sizeof(CacheHash::Hash32_t) +
                 mProcessEntries * sizeof(CacheIndexRecord);
    if (mRWBufSize > kMaxBufSize)
      mRWBufSize = kMaxBufSize;
  }
  else {
    mRWBufSize = kMaxBufSize;
  }
  mRWBuf = static_cast<char *>(moz_xmalloc(mRWBufSize));
}

void
CacheIndex::ReleaseBuffer()
{
  if (!mRWBuf)
    return;

  free(mRWBuf);
  mRWBuf = nullptr;
  mRWBufSize = 0;
  mRWBufPos = 0;
}

namespace { // anon

class FrecencyComparator
{
public:
  bool Equals(CacheIndexRecord* a, CacheIndexRecord* b) const {
    return a->mFrecency == b->mFrecency;
  }
  bool LessThan(CacheIndexRecord* a, CacheIndexRecord* b) const {
    return a->mFrecency < b->mFrecency;
  }
};

class ExpirationComparator
{
public:
  bool Equals(CacheIndexRecord* a, CacheIndexRecord* b) const {
    return a->mExpirationTime == b->mExpirationTime;
  }
  bool LessThan(CacheIndexRecord* a, CacheIndexRecord* b) const {
    return a->mExpirationTime < b->mExpirationTime;
  }
};

} // anon

void
CacheIndex::RemoveEntryFromArrays(CacheIndexEntry *aEntry)
{
  if (!aEntry || !aEntry->IsInitialized() || aEntry->IsRemoved())
    return;

  RemoveRecordFromFrecencyArray(aEntry->mRec);
  RemoveRecordFromExpirationArray(aEntry->mRec);
}

void
CacheIndex::InsertEntryToArrays(CacheIndexEntry *aEntry)
{
  if (!aEntry || !aEntry->IsInitialized() || aEntry->IsRemoved())
    return;

  InsertRecordToFrecencyArray(aEntry->mRec);
  InsertRecordToExpirationArray(aEntry->mRec);
}

void
CacheIndex::InsertRecordToFrecencyArray(CacheIndexRecord *aRecord)
{
  LOG(("CacheIndex::InsertRecordToFrecencyArray() [record=%p, hash=%08x%08x%08x"
       "%08x%08x]", aRecord, LOGSHA1(aRecord->mHash)));

  MOZ_ASSERT(!mFrecencyArray.Contains(aRecord));
  mFrecencyArray.InsertElementSorted(aRecord, FrecencyComparator());
}

void
CacheIndex::InsertRecordToExpirationArray(CacheIndexRecord *aRecord)
{
  LOG(("CacheIndex::InsertRecordToExpirationArray() [record=%p, hash=%08x%08x"
       "%08x%08x%08x]", aRecord, LOGSHA1(aRecord->mHash)));

  MOZ_ASSERT(!mExpirationArray.Contains(aRecord));
  mExpirationArray.InsertElementSorted(aRecord, ExpirationComparator());
}

void
CacheIndex::RemoveRecordFromFrecencyArray(CacheIndexRecord *aRecord)
{
  LOG(("CacheIndex::RemoveRecordFromFrecencyArray() [record=%p, hash=%08x%08x"
       "%08x%08x%08x]", aRecord, LOGSHA1(aRecord->mHash)));

  DebugOnly<bool> removed;
  removed = mFrecencyArray.RemoveElement(aRecord);
  MOZ_ASSERT(removed);
}

void
CacheIndex::RemoveRecordFromExpirationArray(CacheIndexRecord *aRecord)
{
  LOG(("CacheIndex::RemoveRecordFromExpirationArray() [record=%p, hash=%08x%08x"
       "%08x%08x%08x]", aRecord, LOGSHA1(aRecord->mHash)));

  DebugOnly<bool> removed;
  removed = mExpirationArray.RemoveElement(aRecord);
  MOZ_ASSERT(removed);
}

nsresult
CacheIndex::Run()
{
  LOG(("CacheIndex::Run()"));

  nsresult rv;

  MutexAutoLock lock(mLock);

  rv = EnsureIndexUsable();
  if (NS_FAILED(rv)) return rv;

  if (mState == READY && mShuttingDown)
    return NS_OK;

  switch (mState) {
    case BUILDING:
      BuildIndex();
      break;
    case UPDATING:
      UpdateIndex();
      break;
    default:
      MOZ_ASSERT(false, "Unknown state");
  }

  return NS_OK;
}

nsresult
CacheIndex::OnFileOpened(CacheFileHandle *aHandle, nsresult aResult)
{
  LOG(("CacheIndex::OnFileOpened() [handle=%p, result=0x%08x]", aHandle,
       aResult));

  nsresult rv;

  MutexAutoLock lock(mLock);

  rv = EnsureIndexUsable();
  if (NS_FAILED(rv)) return rv;

  if (mState == READY && mShuttingDown)
    return NS_OK;

  switch (mState) {
    case WRITING:
      WriteIndexHeader(aHandle, aResult);
      break;
    case READING:
      mReadOpenCount--;

      if (mReadFailed) {
        if (NS_SUCCEEDED(aResult))
          CacheFileIOManager::DoomFile(aHandle, nullptr);

        if (mReadOpenCount == 0)
          FinishRead(false);

        return NS_OK;
      }

      switch (mReadOpenCount) {
        case 2: // kIndexName
          if (NS_FAILED(aResult)) {
            mReadFailed = true;
          }
          else {
            MOZ_ASSERT(aHandle->Key() == kIndexName);
            mHandle = aHandle;
          }
          break;
        case 1: // kJournalName
          if (NS_SUCCEEDED(aResult)) {
            MOZ_ASSERT(aHandle->Key() == kJournalName);
            mHandle2 = aHandle;
          }
          break;
        case 0: // kTempIndexName
          if (NS_SUCCEEDED(aResult)) {
            MOZ_ASSERT(aHandle->Key() == kTempIndexName);
            CacheFileIOManager::DoomFile(aHandle, nullptr);

            if (mHandle2) { // this should never happen
              LOG(("CacheIndex::OnFileOpened() - Unexpected state, all files "
                   "[%s, %s, %s] should never exist. Removing whole index.",
                   kIndexName, kJournalName, kTempIndexName));
              FinishRead(false);
              break;
            }
          }

          if (mHandle2) {
            // Rename journal to make sure we update index on next start in case
            // firefox crashes
            rv = CacheFileIOManager::RenameFile(
              mHandle2, NS_LITERAL_CSTRING(kTempIndexName), this);
            if (NS_FAILED(rv)) {
              LOG(("CacheIndex::OnFileOpened() - CacheFileIOManager::RenameFile"
                   "() failed synchronously [rv=0x%08x]", rv));
              FinishRead(false);
              break;
            }
            break;
          }

          StartReadingIndex();
      }
      break;
    default:
      MOZ_ASSERT(false, "Unknown state");
  }

  return NS_OK;
}

nsresult
CacheIndex::OnDataWritten(CacheFileHandle *aHandle, const char *aBuf,
                          nsresult aResult)
{
  LOG(("CacheIndex::OnDataWritten() [handle=%p, result=0x%08x]", aHandle,
       aResult));

  nsresult rv;

  MutexAutoLock lock(mLock);

  rv = EnsureIndexUsable();
  if (NS_FAILED(rv)) return rv;

  if (mState == READY && mShuttingDown)
    return NS_OK;

  switch (mState) {
    case WRITING:
      if (NS_FAILED(aResult)) {
        FinishWrite(false);
      }
      else {
        if (mSkipEntries == 0) {
          rv = CacheFileIOManager::RenameFile(mHandle,
                                              NS_LITERAL_CSTRING(kIndexName),
                                              this);
          if (NS_FAILED(rv)) {
            LOG(("CacheIndex::OnDataWritten() - CacheFileIOManager::"
                 "RenameFile() failed synchronously [rv=0x%08x]", rv));
            FinishWrite(false);
          }
        }
        else
          WriteRecords();
      }
      break;
    default:
      MOZ_ASSERT(false, "Unknown state");
  }

  return NS_OK;
}

nsresult
CacheIndex::OnDataRead(CacheFileHandle *aHandle, char *aBuf, nsresult aResult)
{
  LOG(("CacheIndex::OnDataRead() [handle=%p, result=0x%08x]", aHandle,
       aResult));

  nsresult rv;

  MutexAutoLock lock(mLock);

  rv = EnsureIndexUsable();
  if (NS_FAILED(rv)) return rv;

  if (mState == READY && mShuttingDown)
    return NS_OK;

  switch (mState) {
    case READING:
      if (NS_FAILED(aResult)) {
        FinishRead(false);
      }
      else {
        if (!mIndexOnDiskIsValid)
          ParseRecords();
        else
          ParseJournal();
      }
      break;
    default:
      MOZ_ASSERT(false, "Unknown state");
  }

  return NS_OK;
}

nsresult
CacheIndex::OnFileDoomed(CacheFileHandle *aHandle, nsresult aResult)
{
  MOZ_CRASH("CacheIndex::OnFileDoomed should not be called!");
  return NS_ERROR_UNEXPECTED;
}

nsresult
CacheIndex::OnEOFSet(CacheFileHandle *aHandle, nsresult aResult)
{
  MOZ_CRASH("CacheIndex::OnEOFSet should not be called!");
  return NS_ERROR_UNEXPECTED;
}

nsresult
CacheIndex::OnFileRenamed(CacheFileHandle *aHandle, nsresult aResult)
{
  LOG(("CacheIndex::OnFileRenamed() [handle=%p, result=0x%08x]", aHandle,
       aResult));

  nsresult rv;

  MutexAutoLock lock(mLock);

  rv = EnsureIndexUsable();
  if (NS_FAILED(rv)) return rv;

  if (mState == READY && mShuttingDown)
    return NS_OK;

  switch (mState) {
    case WRITING:
      FinishWrite(NS_FAILED(aResult) ? false : true);
      break;
    case READING:
      if (NS_FAILED(aResult))
        FinishRead(false);
      else
        StartReadingIndex();
      break;
    default:
      MOZ_ASSERT(false, "Unknown state");
  }

  return NS_OK;
}

} // net
} // mozilla
