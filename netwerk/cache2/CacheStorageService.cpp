/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CacheLog.h"
#include "CacheStorageService.h"
#include "CacheFileIOManager.h"
#include "CacheObserver.h"

#include "nsICacheStorageVisitor.h"
#include "CacheStorage.h"
#include "AppCacheStorage.h"
#include "CacheEntry.h"

#include "OldWrappers.h"

#include "nsIURI.h"
#include "nsCOMPtr.h"
#include "nsAutoPtr.h"
#include "nsNetCID.h"
#include "nsServiceManagerUtils.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/DebugOnly.h"

namespace mozilla {
namespace net {

namespace {

void LoadContextInfoMappingKey(nsAutoCString &key, nsILoadContextInfo* aInfo)
{
  /**
   * This key is used to salt file hashes.  When form of the key is changed
   * cache entries will fail to find on disk.
   */
  key.Append(aInfo->IsPrivate() ? 'P' : '-');
  key.Append(aInfo->IsAnonymous() ? 'A' : '-');
  key.Append(':');
  if (aInfo->AppId() != nsILoadContextInfo::NO_APP_ID) {
    key.AppendInt(aInfo->AppId());
  }
  if (aInfo->IsInBrowserElement()) {
    key.Append('B');
  }
}

void AppendMemoryStorageID(nsAutoCString &key)
{
  key.Append('M');
}

}

// Not defining as static or class member of CacheStorageService since
// it would otherwise need to include CacheEntry.h and that then would
// need to be exported to make nsNetModule.cpp compilable.
typedef nsClassHashtable<nsCStringHashKey, CacheEntryTable>
        GlobalEntryTables;

/**
 * Keeps tables of entries.  There is one entries table for each distinct load
 * context type.  The distinction is based on following load context info states:
 * <isPrivate|isAnon|appId|inBrowser> which builds a mapping key.
 *
 * Thread-safe to access, protected by the service mutex.
 */
static GlobalEntryTables* sGlobalEntryTables;

CacheMemoryConsumer::CacheMemoryConsumer()
: mReportedMemoryConsumption(0)
{
}

void
CacheMemoryConsumer::DoMemoryReport(uint32_t aCurrentSize)
{
  if (CacheStorageService::Self())
    CacheStorageService::Self()->OnMemoryConsumptionChange(this, aCurrentSize);
}

NS_IMPL_ISUPPORTS1(CacheStorageService, nsICacheStorageService)

CacheStorageService* CacheStorageService::sSelf = nullptr;

CacheStorageService::CacheStorageService()
: mLock("CacheStorageService")
, mShutdown(false)
, mMemorySize(0)
, mPurging(false)
{
  MOZ_COUNT_CTOR(CacheStorageService);

  CacheFileIOManager::Init();

  MOZ_ASSERT(!sSelf);

  sSelf = this;
  sGlobalEntryTables = new GlobalEntryTables();
  sGlobalEntryTables->Init();

  NS_NewNamedThread("Cache Mngmnt", getter_AddRefs(mThread));
}

CacheStorageService::~CacheStorageService()
{
  LOG(("CacheStorageService::~CacheStorageService"));
  sSelf = nullptr;

  // This assertion is not actually critical, it's here to just confirm
  // that memory occupation inc/dec code works well.  If the value here
  // is not significantly different from 0 (or overflow of ~0) then when
  // a quick solution is needed to fix this assertion failure, just disable
  // it and file a bug.
  MOZ_ASSERT(mMemorySize == 0);

  MOZ_COUNT_DTOR(CacheStorageService);
}

void CacheStorageService::Shutdown()
{
  if (mShutdown)
    return;

  LOG(("CacheStorageService::Shutdown - start"));

  mShutdown = true;

  nsCOMPtr<nsIRunnable> event =
    NS_NewRunnableMethod(this, &CacheStorageService::ShutdownBackground);

  if (mThread)
    mThread->Dispatch(event, nsIEventTarget::DISPATCH_NORMAL);

  mozilla::MutexAutoLock lock(mLock);
  sGlobalEntryTables->Clear();
  delete sGlobalEntryTables;
  sGlobalEntryTables = nullptr;

  LOG(("CacheStorageService::Shutdown - done"));
}

void CacheStorageService::ShutdownBackground()
{
  MOZ_ASSERT(IsOnManagementThread());

  mFrecencyArray.Clear();
  mExpirationArray.Clear();
}

// Internal management methods

namespace { // anon

// EvictionRunnable
// Responsible for purgin and unregistring entries (loaded) in memory

class EvictionRunnable : public nsRunnable
{
public:
  EvictionRunnable(nsCSubstring const & aContextKey, TCacheEntryTable* aEntries,
                   bool aUsingDisk,
                   nsICacheEntryDoomCallback* aCallback)
    : mContextKey(aContextKey)
    , mEntries(aEntries)
    , mCallback(aCallback)
    , mUsingDisk(aUsingDisk) { }

  NS_IMETHOD Run()
  {
    LOG(("EvictionRunnable::Run [this=%p, disk=%d]", this, mUsingDisk));
    if (CacheStorageService::IsOnManagementThread()) {
      if (mUsingDisk) {
        // TODO for non private entries:
        // - rename/move all files to TRASH, block shutdown
        // - start the TRASH removal process
        // - may also be invoked from the main thread...
      }

      if (mEntries) {
        // Process only a limited number of entries during a single loop to
        // prevent block of the management thread.
        mBatch = 50;
        mEntries->Enumerate(&EvictionRunnable::EvictEntry, this);
      }

      // Anything left?  Process in a separate invokation.
      if (mEntries && mEntries->Count())
        NS_DispatchToCurrentThread(this);
      else if (mCallback)
        NS_DispatchToMainThread(this); // TODO - we may want caller thread
    }
    else if (NS_IsMainThread()) {
      mCallback->OnCacheEntryDoomed(NS_OK);
    }
    else {
      MOZ_ASSERT(false, "Not main or cache management thread");
    }

    return NS_OK;
  }

private:
  static PLDHashOperator EvictEntry(const nsACString& aKey,
                                    nsRefPtr<CacheEntry>& aEntry,
                                    void* aClosure)
  {
    EvictionRunnable* evictor = static_cast<EvictionRunnable*>(aClosure);

    LOG(("  evicting entry=%p", aEntry.get()));

    // HACK ...
    // in-mem-only should only be Purge(WHOLE)'ed
    // on-disk may use the same technique I think, disk eviction runs independently
    if (!evictor->mUsingDisk) {
      // When evicting memory-only entries we have to remove them from
      // the master table as well.  PurgeAndDoom() enters the service
      // management lock.
      aEntry->PurgeAndDoom();
    }
    else {
      // Disk (+memory-only) entries are already removed from the master
      // hash table, save locking here!
      aEntry->DoomAlreadyRemoved();
    }

    if (!--evictor->mBatch)
      return PLDHashOperator(PL_DHASH_REMOVE | PL_DHASH_STOP);

    return PL_DHASH_REMOVE;
  }

  nsCString mContextKey;
  nsAutoPtr<TCacheEntryTable> mEntries;
  nsCOMPtr<nsICacheEntryDoomCallback> mCallback;
  uint32_t mBatch;
  bool mUsingDisk;
};

// WalkRunnable
// Responsible to visit the storage and walk all entries on it asynchronously

class WalkRunnable : public nsRunnable
{
public:
  WalkRunnable(nsCSubstring const & aContextKey, bool aVisitEntries,
               bool aUsingDisk,
               nsICacheStorageVisitor* aVisitor)
    : mContextKey(aContextKey)
    , mCallback(aVisitor)
    , mSize(0)
    , mNotifyStorage(true)
    , mVisitEntries(aVisitEntries)
    , mUsingDisk(aUsingDisk)
  {
  }

  NS_IMETHODIMP Run()
  {
    if (CacheStorageService::IsOnManagementThread()) {
      LOG(("WalkRunnable::Run - collecting [this=%p, disk=%d]", this, (bool)mUsingDisk));
      // First, walk, count and grab all entries from the storage
      // TODO
      // - walk files on disk, when the storage is not private
      //    - should create representative entries only for the time
      //      of need

      mozilla::MutexAutoLock lock(CacheStorageService::Self()->Lock());

      if (!CacheStorageService::IsRunning())
        return NS_ERROR_NOT_INITIALIZED;

      CacheEntryTable* entries;
      if (sGlobalEntryTables->Get(mContextKey, &entries))
        entries->EnumerateRead(&WalkRunnable::WalkStorage, this);

      // Next, we dispatch to the main thread
    }
    else if (NS_IsMainThread()) {
      LOG(("WalkRunnable::Run - notifying [this=%p, disk=%d]", this, (bool)mUsingDisk));
      if (mNotifyStorage) {
        LOG(("  storage"));
        // Second, notify overall storage info
        mCallback->OnCacheStorageInfo(mEntryArray.Length(), mSize);
        if (!mVisitEntries)
          return NS_OK; // done

        mNotifyStorage = false;
      }
      else {
        LOG(("  entry [left=%d]", mEntryArray.Length()));
        // Third, notify each entry until depleted.
        if (!mEntryArray.Length()) {
          mCallback->OnCacheEntryVisitCompleted();
          return NS_OK; // done
        }

        mCallback->OnCacheEntryInfo(mEntryArray[0]);
        mEntryArray.RemoveElementAt(0);

        // Dispatch to the main thread again
      }
    }
    else {
      MOZ_ASSERT(false);
      return NS_ERROR_FAILURE;
    }

    NS_DispatchToMainThread(this);
    return NS_OK;
  }

private:
  static PLDHashOperator
  WalkStorage(const nsACString& aKey,
              CacheEntry* aEntry,
              void* aClosure)
  {
    WalkRunnable* walker = static_cast<WalkRunnable*>(aClosure);

    if (!walker->mUsingDisk && aEntry->UsingDisk())
      return PL_DHASH_NEXT;

    walker->mSize += aEntry->GetMetadataMemoryConsumption();

    int64_t size;
    if (NS_SUCCEEDED(aEntry->GetDataSize(&size)))
      walker->mSize += size;

    walker->mEntryArray.AppendElement(aEntry);
    return PL_DHASH_NEXT;
  }

  nsCString mContextKey;
  nsCOMPtr<nsICacheStorageVisitor> mCallback;
  nsTArray<nsRefPtr<CacheEntry> > mEntryArray;

  uint64_t mSize;

  bool mNotifyStorage : 1;
  bool mVisitEntries : 1;
  bool mUsingDisk : 1;
};

PLDHashOperator CollectPrivateContexts(const nsACString& aKey,
                                       CacheEntryTable* aTable,
                                       void* aClosure)
{
  if (aKey[0] == 'P') {
    nsTArray<nsCString>* keys = static_cast<nsTArray<nsCString>*>(aClosure);
    keys->AppendElement(aKey);
  }

  return PL_DHASH_NEXT;
}

PLDHashOperator CollectContexts(const nsACString& aKey,
                                       CacheEntryTable* aTable,
                                       void* aClosure)
{
  nsTArray<nsCString>* keys = static_cast<nsTArray<nsCString>*>(aClosure);
  keys->AppendElement(aKey);

  return PL_DHASH_NEXT;
}

} // anon

void CacheStorageService::DropPrivateBrowsingEntries()
{
  mozilla::MutexAutoLock lock(mLock);

  if (mShutdown)
    return;

  nsTArray<nsCString> keys;
  sGlobalEntryTables->EnumerateRead(&CollectPrivateContexts, &keys);

  for (uint32_t i = 0; i < keys.Length(); ++i)
    DoomStorageEntries(keys[i], true, nullptr);
}

// Helper methods

nsresult CacheStorageService::Dispatch(nsIRunnable* aEvent)
{
  if (!mThread)
    return NS_ERROR_NOT_AVAILABLE;

  return mThread->Dispatch(aEvent, nsIThread::DISPATCH_NORMAL);
}

// nsICacheStorageService

NS_IMETHODIMP CacheStorageService::MemoryCacheStorage(nsILoadContextInfo *aLoadContextInfo,
                                                      nsICacheStorage * *_retval)
{
  NS_ENSURE_ARG(aLoadContextInfo);
  NS_ENSURE_ARG(_retval);

  nsRefPtr<CacheStorage> storage =
    new CacheStorage(aLoadContextInfo, false, false);

  storage.forget(_retval);
  return NS_OK;
}

NS_IMETHODIMP CacheStorageService::DiskCacheStorage(nsILoadContextInfo *aLoadContextInfo,
                                                    bool aLookupAppCache,
                                                    nsICacheStorage * *_retval)
{
  NS_ENSURE_ARG(aLoadContextInfo);
  NS_ENSURE_ARG(_retval);

  // TODO save some heap granularity - cache commonly used storages.

  nsRefPtr<CacheStorage> storage =
    new CacheStorage(aLoadContextInfo, true, aLookupAppCache);

  storage.forget(_retval);
  return NS_OK;
}

NS_IMETHODIMP CacheStorageService::AppCacheStorage(nsILoadContextInfo *aLoadContextInfo,
                                                   nsIApplicationCache *aApplicationCache,
                                                   nsICacheStorage * *_retval)
{
  NS_ENSURE_ARG(aLoadContextInfo);
  NS_ENSURE_ARG(_retval);

  // Using classification since cl believes we want to instantiate this method
  // having the same name as the desired class...
  nsRefPtr<mozilla::net::AppCacheStorage> storage =
    new mozilla::net::AppCacheStorage(aLoadContextInfo, aApplicationCache);

  storage.forget(_retval);
  return NS_OK;
}

NS_IMETHODIMP CacheStorageService::Clear()
{
  mozilla::MutexAutoLock lock(mLock);

  NS_ENSURE_TRUE(!mShutdown, NS_ERROR_NOT_INITIALIZED);

  // TODO
  // - tell the file manager to doom the current cache (all files)

  // (Could be optimized by just throwing all away / however, should doom entries
  // that could be used)

  nsTArray<nsCString> keys;
  sGlobalEntryTables->EnumerateRead(&CollectContexts, &keys);

  for (uint32_t i = 0; i < keys.Length(); ++i)
    DoomStorageEntries(keys[i], true, nullptr);

  return NS_OK;
}

NS_IMETHODIMP CacheStorageService::GetIoTarget(nsIEventTarget** aEventTarget)
{
  NS_ENSURE_ARG(aEventTarget);

  nsCOMPtr<nsIEventTarget> ioTarget = CacheFileIOManager::IOTarget();
  ioTarget.forget(aEventTarget);
  return NS_OK;
}

// Methods used by CacheEntry for management of in-memory structures.

namespace { // anon

class FrecencyComparator
{
public:
  bool Equals(CacheEntry* a, CacheEntry* b) const {
    return a->GetFrecency() == b->GetFrecency();
  }
  bool LessThan(CacheEntry* a, CacheEntry* b) const {
    return a->GetFrecency() < b->GetFrecency();
  }
};

class ExpirationComparator
{
public:
  bool Equals(CacheEntry* a, CacheEntry* b) const {
    return a->GetExpirationTime() == b->GetExpirationTime();
  }
  bool LessThan(CacheEntry* a, CacheEntry* b) const {
    return a->GetExpirationTime() < b->GetExpirationTime();
  }
};

} // anon

void
CacheStorageService::RegisterEntry(CacheEntry* aEntry)
{
  MOZ_ASSERT(IsOnManagementThread());

  if (mShutdown || !aEntry->CanRegister())
    return;

  LOG(("CacheStorageService::RegisterEntry [entry=%p]", aEntry));

  mFrecencyArray.InsertElementSorted(aEntry, FrecencyComparator());
  mExpirationArray.InsertElementSorted(aEntry, ExpirationComparator());

  aEntry->SetRegistered(true);
}

void
CacheStorageService::UnregisterEntry(CacheEntry* aEntry)
{
  MOZ_ASSERT(IsOnManagementThread());

  if (!aEntry->IsRegistered())
    return;

  LOG(("CacheStorageService::UnregisterEntry [entry=%p]", aEntry));

  mozilla::DebugOnly<bool> removedFrecency = mFrecencyArray.RemoveElement(aEntry);
  mozilla::DebugOnly<bool> removedExpiration = mExpirationArray.RemoveElement(aEntry);

  MOZ_ASSERT(mShutdown || (removedFrecency && removedExpiration));

  // Note: aEntry->CanRegister() since now returns false
  aEntry->SetRegistered(false);
}

static bool
AddExactEntry(CacheEntryTable* aEntries,
              nsCString const& aKey,
              CacheEntry* aEntry,
              bool aOverwrite)
{
  nsRefPtr<CacheEntry> existingEntry;
  if (!aOverwrite && aEntries->Get(aKey, getter_AddRefs(existingEntry))) {
    bool equals = existingEntry == aEntry;
    LOG(("AddExactEntry [entry=%p equals=%d]", aEntry, equals));
    return equals; // Already there...
  }

  LOG(("AddExactEntry [entry=%p put]", aEntry));
  aEntries->Put(aKey, aEntry);
  return true;
}

static bool
RemoveExactEntry(CacheEntryTable* aEntries,
                 nsCString const& aKey,
                 CacheEntry* aEntry,
                 bool aOverwrite)
{
  nsRefPtr<CacheEntry> existingEntry;
  if (!aEntries->Get(aKey, getter_AddRefs(existingEntry))) {
    LOG(("RemoveExactEntry [entry=%p already gone]"));
    return false; // Already removed...
  }

  if (!aOverwrite && existingEntry != aEntry) {
    LOG(("RemoveExactEntry [entry=%p already replaced]"));
    return false; // Already replaced...
  }

  LOG(("RemoveExactEntry [entry=%p removed]"));
  aEntries->Remove(aKey);
  return true;
}

void
CacheStorageService::RemoveEntry(CacheEntry* aEntry)
{
  LOG(("CacheStorageService::RemoveEntry [entry=%p]", aEntry));

  nsAutoCString entryKey;
  nsresult rv = aEntry->HashingKey(entryKey);
  if (NS_FAILED(rv)) {
    NS_ERROR("aEntry->HashingKey() failed?");
    return;
  }

  mozilla::MutexAutoLock lock(mLock);

  if (mShutdown) {
    LOG(("  after shutdown"));
    return;
  }

  CacheEntryTable* entries;
  if (sGlobalEntryTables->Get(aEntry->GetStorageID(), &entries))
    RemoveExactEntry(entries, entryKey, aEntry, false /* don't overwrite */);

  nsAutoCString memoryStorageID(aEntry->GetStorageID());
  AppendMemoryStorageID(memoryStorageID);

  if (sGlobalEntryTables->Get(memoryStorageID, &entries))
    RemoveExactEntry(entries, entryKey, aEntry, false /* don't overwrite */);
}

void
CacheStorageService::RecordMemoryOnlyEntry(CacheEntry* aEntry,
                                           bool aOnlyInMemory,
                                           bool aOverwrite)
{
  LOG(("CacheStorageService::RecordMemoryOnlyEntry [entry=%p, memory=%d, overwrite=%d]",
    aEntry, aOnlyInMemory, aOverwrite));
  // This method is responsible to put this entry to a special record hashtable
  // that contains only entries that are stored in memory.
  // Keep in mind that every entry, regardless of whether is in-memory-only or not
  // is always recorded in the storage master hash table, the one identified by
  // CacheEntry.StorageID().

  mLock.AssertCurrentThreadOwns();

  if (mShutdown) {
    LOG(("  after shutdown"));
    return;
  }

  nsresult rv;

  nsAutoCString entryKey;
  rv = aEntry->HashingKey(entryKey);
  if (NS_FAILED(rv)) {
    NS_ERROR("aEntry->HashingKey() failed?");
    return;
  }

  CacheEntryTable* entries = nullptr;
  nsAutoCString memoryStorageID(aEntry->GetStorageID());
  AppendMemoryStorageID(memoryStorageID);

  if (!sGlobalEntryTables->Get(memoryStorageID, &entries)) {
    if (!aOnlyInMemory) {
      LOG(("  not recorded as memory only"));
      return;
    }

    entries = new CacheEntryTable();
    entries->Init();
    sGlobalEntryTables->Put(memoryStorageID, entries);
    LOG(("  new memory-only storage table for %s", memoryStorageID.get()));
  }

  if (aOnlyInMemory) {
    AddExactEntry(entries, entryKey, aEntry, aOverwrite);
  }
  else {
    RemoveExactEntry(entries, entryKey, aEntry, aOverwrite);
  }
}

void
CacheStorageService::OnMemoryConsumptionChange(CacheMemoryConsumer* aConsumer,
                                               uint32_t aCurrentMemoryConsumption)
{
  LOG(("CacheStorageService::OnMemoryConsumptionChange [consumer=%p, size=%u]",
    aConsumer, aCurrentMemoryConsumption));

  uint32_t savedMemorySize = aConsumer->mReportedMemoryConsumption;
  if (savedMemorySize == aCurrentMemoryConsumption)
    return;

  // Exchange saved size with current one.
  aConsumer->mReportedMemoryConsumption = aCurrentMemoryConsumption;

  mMemorySize -= savedMemorySize;
  mMemorySize += aCurrentMemoryConsumption;

  LOG(("  mMemorySize=%u (+%u,-%u)", uint32_t(mMemorySize), aCurrentMemoryConsumption, savedMemorySize));

  // Bypass purging when memory has not grew up significantly
  if (aCurrentMemoryConsumption <= savedMemorySize)
    return;

  if (mPurging) {
    LOG(("  already purging"));
    return;
  }

  if (mMemorySize <= CacheObserver::MemoryLimit())
    return;

  // Throw the oldest data or whole entries away when over certain limits
  mPurging = true;

  // Must always dipatch, since this can be called under e.g. a CacheFile's lock.
  nsCOMPtr<nsIRunnable> event =
    NS_NewRunnableMethod(this, &CacheStorageService::PurgeOverMemoryLimit);

  Dispatch(event);
}

void
CacheStorageService::PurgeOverMemoryLimit()
{
  MOZ_ASSERT(IsOnManagementThread());

  LOG(("CacheStorageService::PurgeOverMemoryLimit"));

#ifdef MOZ_LOGGING
  TimeStamp start(TimeStamp::Now());
#endif

  uint32_t const memoryLimit = CacheObserver::MemoryLimit();

  if (mMemorySize > memoryLimit) {
    LOG(("  memory data consumption over the limit, abandon expired entries"));
    PurgeExpired();
  }

  bool frecencyNeedsSort = true;
  if (mMemorySize > memoryLimit) {
    LOG(("  memory data consumption over the limit, abandon disk backed data"));
    PurgeByFrecency(frecencyNeedsSort, CacheEntry::PURGE_DATA_ONLY_DISK_BACKED);
  }

  if (mMemorySize > memoryLimit) {
    LOG(("  metadata consumtion over the limit, abandon disk backed entries"));
    PurgeByFrecency(frecencyNeedsSort, CacheEntry::PURGE_WHOLE_ONLY_DISK_BACKED);
  }

  if (mMemorySize > memoryLimit) {
    LOG(("  memory data consumption over the limit, abandon any entry"));
    PurgeByFrecency(frecencyNeedsSort, CacheEntry::PURGE_WHOLE);
  }

  LOG(("  purging took %1.2fms", (TimeStamp::Now() - start).ToMilliseconds()));

  mPurging = false;
}

void
CacheStorageService::PurgeExpired()
{
  MOZ_ASSERT(IsOnManagementThread());

  mExpirationArray.Sort(ExpirationComparator());
  uint32_t now = NowInSeconds();

  uint32_t const memoryLimit = CacheObserver::MemoryLimit();

  for (uint32_t i = 0; mMemorySize > memoryLimit && i < mExpirationArray.Length();) {
    nsRefPtr<CacheEntry> entry = mExpirationArray[i];

    uint32_t expirationTime = entry->GetExpirationTime();
    if (expirationTime > 0 && expirationTime <= now) {
      LOG(("  dooming expired entry=%p, exptime=%u (now=%u)",
        entry.get(), entry->GetExpirationTime(), now));

      entry->PurgeAndDoom();
      continue;
    }

    // not purged, move to the next one
    ++i;
  }
}

void
CacheStorageService::PurgeByFrecency(bool &aFrecencyNeedsSort, uint32_t aWhat)
{
  MOZ_ASSERT(IsOnManagementThread());

  if (aFrecencyNeedsSort) {
    mFrecencyArray.Sort(FrecencyComparator());
    aFrecencyNeedsSort = false;
  }

  uint32_t const memoryLimit = CacheObserver::MemoryLimit();

  for (uint32_t i = 0; mMemorySize > memoryLimit && i < mFrecencyArray.Length();) {
    nsRefPtr<CacheEntry> entry = mFrecencyArray[i];

    if (entry->Purge(aWhat)) {
      LOG(("  abandoned (%d), entry=%p, frecency=%1.10f",
        aWhat, entry.get(), entry->GetFrecency()));
      continue;
    }

    // not purged, move to the next one
    ++i;
  }
}

// Methods exposed to and used by CacheStorage.

nsresult
CacheStorageService::AddStorageEntry(CacheStorage const* aStorage,
                                     nsIURI* aURI,
                                     const nsACString & aIdExtension,
                                     bool aCreateIfNotExist,
                                     bool aReplace,
                                     CacheEntry** aResult)
{
  NS_ENSURE_FALSE(mShutdown, NS_ERROR_NOT_INITIALIZED);

  NS_ENSURE_ARG(aStorage);

  nsAutoCString contextKey;
  LoadContextInfoMappingKey(contextKey, aStorage->LoadInfo());

  return AddStorageEntry(contextKey, aURI, aIdExtension,
                         aStorage->WriteToDisk(), aCreateIfNotExist, aReplace,
                         aResult);
}

nsresult
CacheStorageService::AddStorageEntry(nsCSubstring const& aContextKey,
                                     nsIURI* aURI,
                                     const nsACString & aIdExtension,
                                     bool aWriteToDisk,
                                     bool aCreateIfNotExist,
                                     bool aReplace,
                                     CacheEntry** aResult)
{
  NS_ENSURE_ARG(aURI);

  nsresult rv;

  nsAutoCString entryKey;
  rv = CacheEntry::HashingKey(EmptyCString(), aIdExtension, aURI, entryKey);
  NS_ENSURE_SUCCESS(rv, rv);

  LOG(("CacheStorageService::AddStorageEntry [entryKey=%s, contextKey=%s]",
    entryKey.get(), aContextKey.BeginReading()));

  nsRefPtr<CacheEntry> entry;

  {
    mozilla::MutexAutoLock lock(mLock);

    NS_ENSURE_FALSE(mShutdown, NS_ERROR_NOT_INITIALIZED);

    // Ensure storage table
    CacheEntryTable* entries;
    if (!sGlobalEntryTables->Get(aContextKey, &entries)) {
      entries = new CacheEntryTable();
      entries->Init();
      sGlobalEntryTables->Put(aContextKey, entries);
      LOG(("  new storage entries table for context %s", aContextKey.BeginReading()));
    }

    bool entryExists = entries->Get(entryKey, getter_AddRefs(entry));

    // Check entry that is memory-only is also in related memory-only hashtable.
    // If not, it has been evicted and we will truncate it ; doom is pending for it,
    // this consumer just made it sooner then the entry has actually been removed
    // from the master hash table.
    // (This can be bypassed when entry is about to be replaced anyway.)
    if (entryExists && !entry->UsingDisk() && !aReplace) {
      nsAutoCString memoryStorageID(aContextKey);
      AppendMemoryStorageID(memoryStorageID);
      CacheEntryTable* memoryEntries;
      aReplace = sGlobalEntryTables->Get(memoryStorageID, &memoryEntries) &&
                 memoryEntries->GetWeak(entryKey) != entry;

#ifdef MOZ_LOGGING
      if (aReplace) {
        LOG(("  memory-only entry %p for %s already doomed, replacing", entry.get(), entryKey.get()));
      }
#endif
    }

    // If truncate is demanded, delete and doom the current entry
    if (entryExists && aReplace) {
      entries->Remove(entryKey);

      LOG(("  dooming entry %p for %s because of OPEN_TRUNCATE", entry.get(), entryKey.get()));
      // On purpose called under the lock to prevent races of doom and open on I/O thread
      entry->DoomAlreadyRemoved();

      entry = nullptr;
      entryExists = false;
    }

    if (entryExists && entry->SetUsingDisk(aWriteToDisk)) {
      RecordMemoryOnlyEntry(entry, !aWriteToDisk, true /* overwrite */);
    }

    // Ensure entry for the particular URL, if not read/only
    if (!entryExists && (aCreateIfNotExist || aReplace)) {
      // Entry is not in the hashtable or has just been truncated...
      entry = new CacheEntry(aContextKey, aURI, aIdExtension, aWriteToDisk);
      entries->Put(entryKey, entry);
      LOG(("  new entry %p for %s", entry.get(), entryKey.get()));
    }
  }

  entry.forget(aResult);
  return NS_OK;
}

nsresult
CacheStorageService::DoomStorageEntry(CacheStorage const* aStorage,
                                      nsIURI *aURI,
                                      const nsACString & aIdExtension,
                                      nsICacheEntryDoomCallback* aCallback)
{
  LOG(("CacheStorageService::DoomStorageEntry"));

  NS_ENSURE_ARG(aStorage);
  NS_ENSURE_ARG(aURI);

  nsAutoCString contextKey;
  LoadContextInfoMappingKey(contextKey, aStorage->LoadInfo());

  nsAutoCString entryKey;
  nsresult rv = CacheEntry::HashingKey(EmptyCString(), aIdExtension, aURI, entryKey);
  NS_ENSURE_SUCCESS(rv, rv);

  nsRefPtr<CacheEntry> entry;
  {
    mozilla::MutexAutoLock lock(mLock);

    NS_ENSURE_FALSE(mShutdown, NS_ERROR_NOT_INITIALIZED);

    CacheEntryTable* entries;
    if (sGlobalEntryTables->Get(contextKey, &entries)) {
      if (entries->Get(entryKey, getter_AddRefs(entry))) {
        if (aStorage->WriteToDisk() || !entry->UsingDisk()) {
          // When evicting from disk storage, purge
          // When evicting from memory storage and the entry is memory-only, purge
          LOG(("  purging entry %p for %s [storage use disk=%d, entry use disk=%d]",
            entry.get(), entryKey.get(), aStorage->WriteToDisk(), entry->UsingDisk()));
          entries->Remove(entryKey);
        }
        else {
          // Otherwise, leave it
          LOG(("  leaving entry %p for %s [storage use disk=%d, entry use disk=%d]",
            entry.get(), entryKey.get(), aStorage->WriteToDisk(), entry->UsingDisk()));
          entry = nullptr;
        }
      }
    }
  }

  if (entry) {
    LOG(("  dooming entry %p for %s", entry.get(), entryKey.get()));
    return entry->AsyncDoom(aCallback);
  }

  LOG(("  no entry loaded for %s", entryKey.get()));

  if (aStorage->WriteToDisk()) {
    LOG(("  dooming file only for %s", entryKey.get()));
    // TODO - go to the disk and doom the entry...
    // ? should record the entry is doomed and don't let open the file
    //   but the file io thread queue should ensure we first remove the
    //   existing file before anyone else would try to open it

    // HACK...
    if (aCallback)
      aCallback->OnCacheEntryDoomed(NS_ERROR_NOT_AVAILABLE);

    return NS_OK;
  }

  if (aCallback)
    aCallback->OnCacheEntryDoomed(NS_ERROR_NOT_AVAILABLE);

  return NS_OK;
}

nsresult
CacheStorageService::DoomStorageEntries(CacheStorage const* aStorage,
                                        nsICacheEntryDoomCallback* aCallback)
{
  LOG(("CacheStorageService::DoomStorageEntries"));

  NS_ENSURE_FALSE(mShutdown, NS_ERROR_NOT_INITIALIZED);
  NS_ENSURE_ARG(aStorage);

  nsAutoCString contextKey;
  LoadContextInfoMappingKey(contextKey, aStorage->LoadInfo());

  mozilla::MutexAutoLock lock(mLock);

  return DoomStorageEntries(contextKey, aStorage->WriteToDisk(), aCallback);
}

nsresult
CacheStorageService::DoomStorageEntries(nsCSubstring const& aContextKey,
                                        bool aDiskStorage,
                                        nsICacheEntryDoomCallback* aCallback)
{
  mLock.AssertCurrentThreadOwns();

  NS_ENSURE_TRUE(!mShutdown, NS_ERROR_NOT_INITIALIZED);

  nsAutoCString memoryStorageID(aContextKey);
  AppendMemoryStorageID(memoryStorageID);

  nsAutoPtr<CacheEntryTable> entries;
  if (aDiskStorage) {
    LOG(("  dooming disk+memory storage of %s", aContextKey.BeginReading()));
    // Grab all entries in this storage
    sGlobalEntryTables->RemoveAndForget(aContextKey, entries);
    // Just remove the memory-only records table
    sGlobalEntryTables->Remove(memoryStorageID);
  }
  else {
    LOG(("  dooming memory-only storage of %s", aContextKey.BeginReading()));
    // Grab the memory-only records table, EvictionRunnable will safely remove
    // entries one by one from the master hashtable on the background management
    // thread.  Code at AddStorageEntry ensures a new entry will always replace
    // memory only entries that EvictionRunnable yet didn't manage to remove.
    sGlobalEntryTables->RemoveAndForget(memoryStorageID, entries);
  }

  nsRefPtr<EvictionRunnable> evict = new EvictionRunnable(
    aContextKey, entries.forget(), aDiskStorage, aCallback);

  return Dispatch(evict);
}

nsresult
CacheStorageService::WalkStorageEntries(CacheStorage const* aStorage,
                                        bool aVisitEntries,
                                        nsICacheStorageVisitor* aVisitor)
{
  LOG(("CacheStorageService::WalkStorageEntries [cb=%p, visitentries=%d]", aVisitor, aVisitEntries));
  NS_ENSURE_FALSE(mShutdown, NS_ERROR_NOT_INITIALIZED);

  NS_ENSURE_ARG(aStorage);

  nsAutoCString contextKey;
  LoadContextInfoMappingKey(contextKey, aStorage->LoadInfo());

  nsRefPtr<WalkRunnable> event = new WalkRunnable(
    contextKey, aVisitEntries, aStorage->WriteToDisk(), aVisitor);
  return Dispatch(event);
}

} // net
} // mozilla
