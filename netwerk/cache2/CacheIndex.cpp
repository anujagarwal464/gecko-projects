/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CacheIndex.h"

#include "CacheLog.h"
#include "CacheFileIOManager.h"
#include "nsThreadUtils.h"
#include "nsPrintfCString.h"
#include "mozilla/DebugOnly.h"
#include "prinrval.h"
#include "nsIFile.h"
#include <algorithm>


#define kMinUnwrittenChanges 300
#define kMinDumpInterval     20000  // in milliseconds
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
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END_THREADSAFE


CacheIndex::CacheIndex()
  : mLock("CacheFile.mLock")
  , mState(INITIAL)
  , mIndexNeedsUpdate(false)
  , mIndexOnDiskIsValid(false)
  , mIndexTimeStamp(0)
  , mLastDumpTime(0)
  , mSkipEntries(0)
  , mRWBuf(nullptr)
  , mRWBufSize(0)
  , mRWBufPos(0)
  , mReadOpenCount(0)
  , mReadFailed(false)
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
CacheIndex::Shutdown()
{
  LOG(("CacheIndex::Shutdown() [gInstance=%p]", gInstance));

  MOZ_ASSERT(NS_IsMainThread());

  nsRefPtr<CacheIndex> index;
  index.swap(gInstance);

  if (!index)
    return NS_ERROR_NOT_INITIALIZED;

  MutexAutoLock lock(index->mLock);

  LOG(("CacheIndex::Shutdown() - [state=%d, indexOnDiskIsValid=%d]",
       index->mState, index->mIndexOnDiskIsValid));

  EState oldState = index->mState;
  index->ChangeState(SHUTDOWN);

  switch (oldState) {
    case WRITING:
      index->FinishWrite(false);
    case READY:
      if (index->mIndexOnDiskIsValid) {
        if (NS_FAILED(index->WriteLogToDisk()))
          index->RemoveIndexFromDisk();
      }
      else
        index->RemoveIndexFromDisk();
      break;
    case READING:
      index->FinishRead(false);
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

  CacheIndexEntry *entry = index->mIndex.GetEntry(*aHash);

  if (index->mState == READY)
    index->mIndexStats.BeforeChange(entry);

  if (entry && entry->IsRemoved())
    entry = nullptr;

  if (index->mState == READY) {
    MOZ_ASSERT(index->mPendingUpdates.Count() == 0);
    MOZ_ASSERT(index->mPendingRemovals.Count() == 0);

    if (entry) {
      LOG(("CacheIndex::AddEntry() - Found entry that shouldn't exist, update "
           "is needed"));
      index->mIndexNeedsUpdate = true;
    }
    else {
      entry = index->mIndex.PutEntry(*aHash);
    }
  }
  else {
    CacheIndexEntry *updated = index->mPendingUpdates.GetEntry(*aHash);
    bool removed = index->mPendingRemovals.Contains(*aHash);

    MOZ_ASSERT(!updated);

    if (index->mState == WRITING) {
      if (entry && !removed) {
        LOG(("CacheIndex::AddEntry() - Found entry that shouldn't exist, "
             "update is needed"));
        index->mIndexNeedsUpdate = true;
      }
    }
    else if (index->mState == BUILDING) {
      MOZ_ASSERT(!entry || removed);
    }

    updated = index->mPendingUpdates.PutEntry(*aHash);
    entry = updated;
  }

  entry->InitNew();
  entry->MarkDirty();
  entry->MarkFresh();

  if (index->mState == READY)
    index->mIndexStats.AfterChange(entry);

  if (index->ShouldWriteIndexToDisk()) {
    index->WriteIndexToDisk();
  }

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

  CacheIndexEntry *entry = index->mIndex.GetEntry(*aHash);

  if (index->mState == READY)
    index->mIndexStats.BeforeChange(entry);

  if (entry && entry->IsRemoved())
    entry = nullptr;

  if (index->mState == READY) {
    MOZ_ASSERT(index->mPendingUpdates.Count() == 0);
    MOZ_ASSERT(index->mPendingRemovals.Count() == 0);

    if (!entry) {
      LOG(("CacheIndex::EnsureEntryExists() - Didn't find entry that should "
           "exist, update is needed"));
      index->mIndexNeedsUpdate = true;
      entry = index->mIndex.PutEntry(*aHash);
      entry->InitNew();
      entry->MarkDirty();
      entry->MarkFresh();
    }
  }
  else {
    CacheIndexEntry *updated = index->mPendingUpdates.GetEntry(*aHash);
    DebugOnly<bool> removed = index->mPendingRemovals.Contains(*aHash);

    if (index->mState == WRITING) {
      MOZ_ASSERT(updated || !removed);
      if (!updated && !entry) {
        LOG(("CacheIndex::EnsureEntryExists() - Didn't find entry that should "
             "exist, update is needed"));
        index->mIndexNeedsUpdate = true;
      }
    }

    if (updated) {
      updated->MarkFresh();
    }
    else {
      if (!entry) {
        // the entry is missing in index since it is beeing read, rebuilt, etc.
        updated = index->mPendingUpdates.PutEntry(*aHash);
        updated->InitNew();
        updated->MarkFresh();
      }
      else {
        if (!entry->IsFresh()) {
          updated = index->mPendingUpdates.PutEntry(*aHash);
          *updated = *entry;
          updated->MarkFresh();
        }
      }
    }
  }

  if (index->mState == READY)
    index->mIndexStats.AfterChange(entry);

  if (index->ShouldWriteIndexToDisk()) {
    index->WriteIndexToDisk();
  }

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

  CacheIndexEntry *entry = index->mIndex.GetEntry(*aHash);

  if (index->mState == READY)
    index->mIndexStats.BeforeChange(entry);

  if (entry && entry->IsRemoved())
    entry = nullptr;

  if (index->mState == READY) {
    MOZ_ASSERT(index->mPendingUpdates.Count() == 0);
    MOZ_ASSERT(index->mPendingRemovals.Count() == 0);
    MOZ_ASSERT(entry);
    if (CheckCollision(entry, aAppId, aAnonymous, aInBrowser)) {
      index->mIndexNeedsUpdate = true;
    }
  }
  else {
    CacheIndexEntry *updated = index->mPendingUpdates.GetEntry(*aHash);
    DebugOnly<bool> removed = index->mPendingRemovals.Contains(*aHash);

    MOZ_ASSERT(updated || !removed);
    MOZ_ASSERT(updated || entry);

    if (updated) {
      if (CheckCollision(updated, aAppId, aAnonymous, aInBrowser)) {
        index->mIndexNeedsUpdate = true;
      } else {
        if (updated->IsInitialized())
          return NS_OK;
      }
      entry = updated;
    } else {
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

  if (index->mState == READY)
    index->mIndexStats.AfterChange(entry);

  if (index->ShouldWriteIndexToDisk()) {
    index->WriteIndexToDisk();
  }

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

  if (index->mState == READY) {
    MOZ_ASSERT(index->mPendingUpdates.Count() == 0);
    MOZ_ASSERT(index->mPendingRemovals.Count() == 0);

    CacheIndexEntry *entry = index->mIndex.GetEntry(*aHash);
    index->mIndexStats.BeforeChange(entry);

    if (!entry || entry->IsRemoved()) {
      LOG(("CacheIndex::RemoveEntry() - Didn't find entry that should exist, "
           "update is needed"));
      index->mIndexNeedsUpdate = true;
    }
    else {
      if (!entry->IsDirty() && entry->IsEmpty()) {
        index->mIndex.RemoveEntry(*aHash);
        entry = nullptr;
      } else {
        entry->MarkRemoved();
        entry->MarkDirty();
      }
    }

    index->mIndexStats.AfterChange(entry);
  }
  else {
    CacheIndexEntry *updated = index->mPendingUpdates.GetEntry(*aHash);
    bool removed = index->mPendingRemovals.Contains(*aHash);

    if (index->mState == WRITING) {
      MOZ_ASSERT(updated || !removed);

      CacheIndexEntry *entry = index->mIndex.GetEntry(*aHash);
      if (entry && entry->IsRemoved())
        entry = nullptr;

      if (!updated && !entry) {
        LOG(("CacheIndex::RemoveEntry() - Didn't find entry that should exist, "
             "update is needed"));
        index->mIndexNeedsUpdate = true;
      }
    }

    // make sure we have information about entry removal
    if (!removed) {
      index->mPendingRemovals.PutEntry(*aHash);
    }

    // clear any pending update of such entry
    if (updated) {
      index->mPendingUpdates.RemoveEntry(*aHash);
    }
  }

  if (index->ShouldWriteIndexToDisk()) {
    index->WriteIndexToDisk();
  }

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

  CacheIndexEntry *entry = index->mIndex.GetEntry(*aHash);

  if (index->mState == READY)
    index->mIndexStats.BeforeChange(entry);

  if (entry && entry->IsRemoved())
    entry = nullptr;

  if (index->mState == READY) {
    MOZ_ASSERT(index->mPendingUpdates.Count() == 0);
    MOZ_ASSERT(index->mPendingRemovals.Count() == 0);
    MOZ_ASSERT(entry);

    if (!EntryChanged(entry, aFrecency, aExpirationTime, aSize)) {
      index->mIndexStats.AfterChange(entry);
      return NS_OK;
    }

    if (!entry->IsDirty()) {
      entry->MarkDirty();
    }
  }
  else {
    CacheIndexEntry *updated = index->mPendingUpdates.GetEntry(*aHash);
    DebugOnly<bool> removed = index->mPendingRemovals.Contains(*aHash);

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

  if (index->mState == READY)
    index->mIndexStats.AfterChange(entry);

  if (index->ShouldWriteIndexToDisk()) {
    index->WriteIndexToDisk();
  }

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

  mPendingRemovals.EnumerateEntries(&CacheIndex::RemoveEntryFromIndex, this);
  mPendingUpdates.EnumerateEntries(&CacheIndex::UpdateEntryInIndex, this);

  MOZ_ASSERT(mPendingUpdates.Count() == 0);
  MOZ_ASSERT(mPendingRemovals.Count() == 0);
}

PLDHashOperator
CacheIndex::RemoveEntryFromIndex(CacheIndexEmptyEntry *aEntry, void* aClosure)
{
  CacheIndex *index = static_cast<CacheIndex *>(aClosure);

  LOG(("CacheFile::RemoveEntryFromIndex() [hash=%08x%08x%08x%08x%08x]",
       LOGSHA1(aEntry->Hash())));

  CacheIndexEntry *entry = index->mIndex.GetEntry(*aEntry->Hash());

  index->mIndexStats.BeforeChange(entry);

  if (entry && !entry->IsRemoved()) {
    if (!entry->IsDirty() && entry->IsEmpty()) {
      index->mIndex.RemoveEntry(*aEntry->Hash());
      entry = nullptr;
    } else {
      entry->MarkRemoved();
      entry->MarkDirty();
    }
  }

  index->mIndexStats.AfterChange(entry);

  return PL_DHASH_REMOVE;
}

PLDHashOperator
CacheIndex::UpdateEntryInIndex(CacheIndexEntry *aEntry, void* aClosure)
{
  CacheIndex *index = static_cast<CacheIndex *>(aClosure);

  LOG(("CacheFile::UpdateEntryInIndex() [hash=%08x%08x%08x%08x%08x]",
       LOGSHA1(aEntry->Hash())));

  CacheIndexEntry *entry = index->mIndex.GetEntry(*aEntry->Hash());

  index->mIndexStats.BeforeChange(entry);

  entry = index->mIndex.PutEntry(*aEntry->Hash());
  *entry = *aEntry;
  entry->MarkDirty();

  index->mIndexStats.AfterChange(entry);

  return PL_DHASH_REMOVE;
}

bool
CacheIndex::ShouldWriteIndexToDisk()
{
  if (mState != READY)
    return false;

  if (!((PR_IntervalNow() - mLastDumpTime) >
      PR_MillisecondsToInterval(kMinDumpInterval)))
    return false;

  if (mIndexStats.Dirty() < kMinUnwrittenChanges)
    return false;

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
};

void
CacheIndex::WriteIndexHeader(CacheFileHandle *aHandle, nsresult aResult)
{
  LOG(("CacheIndex::WriteIndexHeader() [handle=%p, result=0x%08x]", aHandle,
       aResult));

  mLock.AssertCurrentThreadOwns();
  MOZ_ASSERT(!mRWBuf);
  MOZ_ASSERT(!mHash);
  MOZ_ASSERT(mIndex.Count() == mIndexStats.Count());

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
  hdr->mVersion = htonl(kIndexVersion);
  hdr->mTimeStamp = htonl(static_cast<uint32_t>(PR_Now() / PR_USEC_PER_SEC));
  hdr->mIsDirty = htonl(1);

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
  MOZ_ASSERT(data.mProcessMax != 0 || mIndexStats.ActiveEntriesCount() == 0);
  data.mProcessed = 0;

  mIndex.EnumerateEntries(&CacheIndex::CopyRecordsToRWBuf, &data);
  MOZ_ASSERT(mRWBufPos != static_cast<uint32_t>(data.mBuf - mRWBuf) ||
             mIndexStats.ActiveEntriesCount() == 0);
  mRWBufPos = data.mBuf - mRWBuf;
  mSkipEntries += data.mProcessed;
  MOZ_ASSERT(mSkipEntries <= mIndexStats.ActiveEntriesCount());

  mHash->Update(mRWBuf + hashOffset, mRWBufPos - hashOffset);

  if (mSkipEntries == mIndexStats.ActiveEntriesCount()) {
    // We've processed all records
    if (mRWBufPos + sizeof(CacheHash::Hash32_t) > mRWBufSize) {
      // realloc buffer to spare another write cycle
      mRWBufSize = mRWBufPos + sizeof(CacheHash::Hash32_t);
      mRWBuf = static_cast<char *>(moz_xrealloc(mRWBuf, mRWBufSize));
    }

    *(reinterpret_cast<uint32_t *>(mRWBuf + mRWBufPos)) =
      htonl(mHash->GetHash());

    mRWBufPos += sizeof(CacheHash::Hash32_t);
    mSkipEntries = 0;
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

struct ApplyIndexChangesHelper
{
  CacheIndexStats *mIndexStats;
  uint32_t         mCacheSize;
};

void
CacheIndex::FinishWrite(bool aSucceeded)
{
  LOG(("CacheIndex::FinishWrite() [succeeded=%d]", aSucceeded));

  MOZ_ASSERT((!aSucceeded && mState == SHUTDOWN) || mState == WRITING);

  mLock.AssertCurrentThreadOwns();

  mHandle = nullptr;
  mHash = nullptr;
  ReleaseBuffer();

  ApplyIndexChangesHelper data;
  data.mIndexStats = &mIndexStats;
  data.mCacheSize = 0;

  if (aSucceeded) {
    mIndex.EnumerateEntries(&CacheIndex::ApplyIndexChanges, &data);
    mIndexOnDiskIsValid = true;
  }

  MOZ_ASSERT(data.mCacheSize == mIndexStats.Size());

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
//    MOZ_ASSERT(aEntry->IsEmpty()); // TODO enable this check once REBUILD and UPDATE are implemented
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

  aEntry->WriteToBuf(data->mBuf);
  data->mBuf += sizeof(CacheIndexRecord);
  data->mProcessed++;
  if (data->mProcessed == data->mProcessMax)
    return PL_DHASH_STOP;

  return PL_DHASH_NEXT;
}

PLDHashOperator
CacheIndex::ApplyIndexChanges(CacheIndexEntry *aEntry, void* aClosure)
{
  ApplyIndexChangesHelper *data = static_cast<ApplyIndexChangesHelper *>(
                                    aClosure);

  data->mIndexStats->BeforeChange(aEntry);

  if (aEntry->IsRemoved()) {
    data->mIndexStats->AfterChange(nullptr);

    return PL_DHASH_REMOVE;
  }

  if (aEntry->IsDirty())
    aEntry->ClearDirty();

  data->mIndexStats->AfterChange(aEntry);

#ifdef DEBUG
  if (aEntry->IsInitialized())
    data->mCacheSize += aEntry->GetFileSize();
#endif

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
      NS_WARNING("Cannot remove old entry from the disk");
      // TODO log
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

  *(reinterpret_cast<uint32_t *>(mBuf + mBufPos)) = htonl(mHash->GetHash());
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

  RemoveFile(NS_LITERAL_CSTRING(kTempIndexName));

  nsCOMPtr<nsIFile> indexFile;
  rv = GetFile(NS_LITERAL_CSTRING(kIndexName), getter_AddRefs(indexFile));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIFile> logFile;
  rv = GetFile(NS_LITERAL_CSTRING(kJournalName), getter_AddRefs(logFile));
  NS_ENSURE_SUCCESS(rv, rv);

  ProcessPendingOperations();
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

  header.mIsDirty = htonl(0);

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
  MOZ_ASSERT(!mIndexNeedsUpdate);
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

    if (ntohl(hdr->mVersion) != kIndexVersion) {
      free(hdr);
      FinishRead(false);
      return;
    }

    mIndexTimeStamp = ntohl(hdr->mTimeStamp);

    if (ntohl(hdr->mIsDirty)) {
      if (mHandle2) {
        CacheFileIOManager::DoomFile(mHandle2, nullptr);
        mHandle2 = nullptr;
      }
      free(hdr);
    }
    else {
      hdr->mIsDirty = htonl(1);
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

    CacheIndexEntry *entry = mIndex.PutEntry(*tmpEntry.Hash());
    *entry = tmpEntry;

    if (entry->IsDirty() || !entry->IsInitialized() || entry->IsEmpty() ||
        entry->IsFresh() || entry->IsRemoved()) {
      LOG(("CacheIndex::ParseRecords() - Invalid entry found in index, removing"
           " whole index [dirty=%d, initialized=%d, empty=%d, fresh=%d, "
           "removed=%d]", entry->IsDirty(), entry->IsInitialized(),
           entry->IsEmpty(), entry->IsFresh(), entry->IsRemoved()));
      FinishRead(false);
      return;
    }

    mIndexStats.BeforeChange(nullptr);
    mIndexStats.AfterChange(entry);

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
    if (mHash->GetHash() != ntohl(*(reinterpret_cast<uint32_t *>(mRWBuf)))) {
      LOG(("CacheIndex::ParseRecords() - Hash mismatch, [is %x, should be %x]",
           mHash->GetHash(), ntohl(*(reinterpret_cast<uint32_t *>(mRWBuf)))));
      FinishRead(false);
      return;
    }

    mIndexOnDiskIsValid = true;
    mIndexNeedsUpdate = true;

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
    if (mHash->GetHash() != ntohl(*(reinterpret_cast<uint32_t *>(mRWBuf)))) {
      LOG(("CacheIndex::ParseJournal() - Hash mismatch, [is %x, should be %x]",
           mHash->GetHash(), ntohl(*(reinterpret_cast<uint32_t *>(mRWBuf)))));
           FinishRead(false);
           return;
    }

    mIndexNeedsUpdate = false;
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

  index->mIndexStats.BeforeChange(entry);

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

  index->mIndexStats.AfterChange(entry);

  return PL_DHASH_REMOVE;
}

void
CacheIndex::FinishRead(bool aSucceeded)
{
  LOG(("CacheIndex::FinishRead() [succeeded=%d]", aSucceeded));
  mLock.AssertCurrentThreadOwns();

  MOZ_ASSERT((!aSucceeded && mState == SHUTDOWN) || mState == READING);

  MOZ_ASSERT(
    (!aSucceeded && !mIndexOnDiskIsValid && !mIndexNeedsUpdate) || // -> rebuild
    (!aSucceeded && mIndexOnDiskIsValid && mIndexNeedsUpdate) ||   // -> update
    (aSucceeded && mIndexOnDiskIsValid && !mIndexNeedsUpdate));    // -> ready

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
    mIndexStats.Reset();
    StartRebuildingIndex();
    return;
  }

  if (mIndexNeedsUpdate) {
    mTmpJournal.Clear();
    StartUpdatingIndex();
    return;
  }

  MergeJournal();
  ProcessPendingOperations();
  mIndexStats.Log();
  ChangeState(READY);
  mLastDumpTime = PR_IntervalNow(); // Do not dump new index immediately
}

void
CacheIndex::StartRebuildingIndex()
{
  LOG(("CacheIndex::StartRebuildingIndex()"));

  // TODO implement me !
  ProcessPendingOperations();
  ChangeState(READY);
  mLastDumpTime = PR_IntervalNow(); // Do not dump new index immediately
}

void
CacheIndex::StartUpdatingIndex()
{
  LOG(("CacheIndex::StartUpdatingIndex()"));

  mIndexStats.Log();

  // TODO implement me !
  ProcessPendingOperations();
  ChangeState(READY);
  mLastDumpTime = PR_IntervalNow(); // Do not dump new index immediately
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
  mState = aNewState;
}

void
CacheIndex::AllocBuffer()
{
  if (mState == WRITING) {
    mRWBufSize = sizeof(CacheIndexHeader) + sizeof(CacheHash::Hash32_t) +
                 mIndexStats.ActiveEntriesCount() * sizeof(CacheIndexRecord);
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

nsresult
CacheIndex::OnFileOpened(CacheFileHandle *aHandle, nsresult aResult)
{
  LOG(("CacheIndex::OnFileOpened() [handle=%p, result=0x%08x]", aHandle,
       aResult));

  nsresult rv;

  MutexAutoLock lock(mLock);

  rv = EnsureIndexUsable();
  if (NS_FAILED(rv)) return rv;

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
