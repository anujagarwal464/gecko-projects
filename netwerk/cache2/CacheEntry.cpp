/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CacheEntry.h"
#include "CacheStorageService.h"
#include "CacheLog.h"

#include "nsIInputStream.h"
#include "nsIOutputStream.h"
#include "nsISeekableStream.h"
#include "nsIURI.h"
#include "nsICacheEntryOpenCallback.h"
#include "nsICacheStorage.h"
#include "nsISerializable.h"

#include "nsComponentManagerUtils.h"
#include "nsString.h"
#include "nsProxyRelease.h"
#include "nsSerializationHelper.h"
#include <math.h>

namespace mozilla {
namespace net {

NS_IMPL_THREADSAFE_ISUPPORTS1(CacheEntry::Handle, nsICacheEntry)

// CacheEntry::Handle

CacheEntry::Handle::Handle(CacheEntry* aEntry)
: mEntry(aEntry)
{
  MOZ_COUNT_CTOR(CacheEntry::Handle);

  LOG(("New CacheEntry::Handle %p for entry %p", this, aEntry));
}

CacheEntry::Handle::~Handle()
{
  mEntry->OnWriterClosed(this);

  MOZ_COUNT_DTOR(CacheEntry::Handle);
}

// CacheEntry

NS_IMPL_THREADSAFE_ISUPPORTS2(CacheEntry, nsICacheEntry, nsIRunnable)

CacheEntry::CacheEntry(const nsACString& aStorageID,
                       nsIURI* aURI,
                       const nsACString& aEnhanceID,
                       bool aUseDisk)
: mReportedMemorySize(0)
, mFrecency(0)
, mSortingExpirationTime(uint32_t(-1))
, mLock("CacheEntry")
, mFileLoadResult(NS_OK)
, mURI(aURI)
, mEnhanceID(aEnhanceID)
, mStorageID(aStorageID)
, mUseDisk(aUseDisk)
, mIsLoading(false)
, mIsLoaded(false)
, mIsReady(false)
, mIsWriting(false)
, mIsDoomed(false)
, mIsRegistered(false)
, mIsRegistrationAllowed(true)
, mSecurityInfoLoaded(false)
, mPredictedDataSize(0)
, mDataSize(0)
, mMetadataMemoryOccupation(0)
{
  MOZ_COUNT_CTOR(CacheEntry);

  mService = CacheStorageService::Self();

  CacheStorageService::Self()->RecordMemoryOnlyEntry(
    this, !aUseDisk, true /* overwrite */);
}

CacheEntry::~CacheEntry()
{
  ProxyReleaseMainThread(mURI);

  LOG(("CacheEntry::~CacheEntry [this=%p]", this));
  MOZ_COUNT_DTOR(CacheEntry);
}

nsresult CacheEntry::HashingKeyWithStorage(nsACString &aResult)
{
  return HashingKey(mStorageID, mEnhanceID, mURI, aResult);
}

nsresult CacheEntry::HashingKey(nsACString &aResult)
{
  return HashingKey(EmptyCString(), mEnhanceID, mURI, aResult);
}

// static
nsresult CacheEntry::HashingKey(nsCSubstring const& aStorageID,
                                nsCSubstring const& aEnhanceID,
                                nsIURI* aURI,
                                nsACString &aResult)
{
  /**
   * This key is used to salt hash that is a base for disk file name.
   * Changing it will cause we will not be able to find files on disk.
   */

  if (aStorageID.Length()) {
    aResult.Append(aStorageID);
    aResult.Append(':');
  }

  if (aEnhanceID.Length()) {
    aResult.Append(aEnhanceID);
    aResult.Append(':');
  }

  nsAutoCString spec;
  nsresult rv = aURI->GetAsciiSpec(spec);
  NS_ENSURE_SUCCESS(rv, rv);

  aResult.Append(spec);

  return NS_OK;
}

void CacheEntry::Load(bool aTruncate)
{
  LOG(("CacheEntry::Load [this=%p]", this));

  {
    mozilla::MutexAutoLock lock(mLock);

    MOZ_ASSERT(!mFile);
    MOZ_ASSERT(!mIsReady);

    if (aTruncate || !mUseDisk) {
      mIsLoading = false;
      mIsLoaded = true;
    }
    else {
      mIsLoading = true;
      mIsLoaded = false;
    }

    mFile = new CacheFile();
  }

  nsresult rv;

  nsAutoCString fileKey;
  rv = HashingKeyWithStorage(fileKey);

  // TODO tell the file to store on disk or not from the very start
  if (NS_SUCCEEDED(rv))
    rv = mFile->Init(fileKey, aTruncate, this);

  if (NS_FAILED(rv))
    OnFileReady(rv, false);
}

NS_IMETHODIMP CacheEntry::OnFileReady(nsresult aResult, bool aIsNew)
{
  LOG(("CacheEntry::OnFileReady [this=%p, rv=0x%08x, new=%d]",
      this, aResult, aIsNew));

  {
    mozilla::MutexAutoLock lock(mLock);
    mIsLoaded = true;
    mIsLoading = false;
    mIsReady = !aIsNew;

    if (NS_FAILED(aResult))
      mFile = nullptr;
  }

  if (NS_FAILED(aResult))
    AsyncDoom(nullptr);

  InvokeCallbacks();

  return NS_OK;
}

NS_IMETHODIMP CacheEntry::OnFileDoomed(nsresult aResult)
{
  nsCOMPtr<nsICacheEntryDoomCallback> callback;
  {
    mozilla::MutexAutoLock lock(mLock);
    mDoomCallback.swap(callback);
  }

  if (callback) {
    nsRefPtr<DoomCallbackRunnable> event =
      new DoomCallbackRunnable(callback, NS_OK);
    NS_DispatchToMainThread(event);
  }

  return NS_OK;
}

void CacheEntry::AsyncOpen(nsICacheEntryOpenCallback* aCallback, uint32_t aFlags)
{
  LOG(("CacheEntry::AsyncOpen [this=%p, callback=%p]", this, aCallback));
  LOG(("  ready=%d, loaded=%d, writing=%d", (bool)mIsReady, (bool)mIsLoaded, (bool)mIsWriting));

  // Call to this methods means a demand to access this entry by a single consumer.
  // Thus, update frecency.

  bool readonly = aFlags & nsICacheStorage::OPEN_READONLY;
  bool truncate = aFlags & nsICacheStorage::OPEN_TRUNCATE;
  MOZ_ASSERT(!readonly || !truncate);

  bool loading, loaded;
  {
    mozilla::MutexAutoLock lock(mLock);
    loaded = mIsLoaded;
    loading = mIsLoading;
  }

  // Must not call truncate on already loaded entry
  MOZ_ASSERT(!(truncate && loaded));

  if (!loaded) {
    RememberCallback(aCallback, readonly);
    if (!loading) {
      BackgroundOp(Ops::REGISTER);
      Load(truncate);
    }

    return;
  }

  bool called = InvokeCallback(aCallback, readonly);
  if (!called) {
    RememberCallback(aCallback, readonly);
  }
}

void CacheEntry::RememberCallback(nsICacheEntryOpenCallback* aCallback,
                                  bool aReadOnly)
{
  LOG(("CacheEntry::RememberCallback [this=%p, cb=%p]", this, aCallback));

  mozilla::MutexAutoLock lock(mLock);
  if (!aReadOnly)
    mCallbacks.AppendObject(aCallback);
  else
    mReadOnlyCallbacks.AppendObject(aCallback);
}

void CacheEntry::InvokeCallbacks()
{
  LOG(("CacheEntry::InvokeCallbacks START [this=%p]", this));

  mozilla::MutexAutoLock lock(mLock);

  while (mCallbacks.Count()) {
    nsCOMPtr<nsICacheEntryOpenCallback> callback = mCallbacks[0];
    mCallbacks.RemoveElementAt(0);

    {
      mozilla::MutexAutoUnlock unlock(mLock);
      InvokeCallback(callback, false);
    }

    if (!mIsReady) {
      // Stop invoking other callbacks since one of them
      // is now writing to the entry.  No consumers should
      // get this entry until metadata are filled with
      // values downloaded from the server.
      LOG(("CacheEntry::InvokeCallbacks DONE (not ready) [this=%p]", this));
      return;
    }
  }

  while (mReadOnlyCallbacks.Count()) {
    nsCOMPtr<nsICacheEntryOpenCallback> callback = mReadOnlyCallbacks[0];

    {
      mozilla::MutexAutoUnlock unlock(mLock);
      if (!InvokeCallback(callback, true)) {
        // Didn't trigger, so we must stop
        break;
      }
    }

    mReadOnlyCallbacks.RemoveElement(callback);
  }

  LOG(("CacheEntry::InvokeCallbacks DONE [this=%p]", this));
}

bool CacheEntry::InvokeCallback(nsICacheEntryOpenCallback* aCallback,
                                bool aReadOnly)
{
  LOG(("CacheEntry::InvokeCallback [this=%p, cb=%p]", this, aCallback));
  // When we are here, the entry must be loaded from disk
  MOZ_ASSERT(mIsLoaded);
  MOZ_ASSERT(!mIsLoading);

  bool ready, writing, doomed;
  {
    mozilla::MutexAutoLock lock(mLock);
    LOG(("  ready=%d, loaded=%d, writing=%d, doomed=%d",
      (bool)mIsReady, (bool)mIsLoaded, (bool)mIsWriting, (bool)mIsDoomed));

    ready = mIsReady;
    writing = mIsWriting;
    doomed = mIsDoomed;
  }

  if (ready && !doomed) {
    // Metadata present, validate the entry
    bool isValid;
    nsresult rv = aCallback->OnCacheEntryCheck(this, nullptr, &isValid);
    LOG(("  OnCacheEntryCheck result: rv=0x%08x, valid=%d", rv, isValid));

    if ((NS_FAILED(rv) || !isValid) && !aReadOnly) {
      LOG(("  replacing entry %p", this));
      nsRefPtr<CacheEntry> newEntry;

      // Grab all callbacks first, it makes this code reentrant
      nsCOMArray<nsICacheEntryOpenCallback> callbacks, readOnlyCallbacks;
      {
        mozilla::MutexAutoLock lock(mLock);
        mCallbacks.SwapElements(callbacks);
        mReadOnlyCallbacks.SwapElements(readOnlyCallbacks);
      }

      // The following call dooms this (current) entry and creates a new one for the
      // URL in the same storage.  We then transfer callbacks to that new entry.
      // NOTE: dooming _posts_ InvokeCallbacks() on this entry so that this code
      // is not reentered.
      rv = CacheStorageService::Self()->AddStorageEntry(
        GetStorageID(), GetURI(), GetEnhanceID(),
        mUseDisk,
        true, // always create
        true, // truncate existing (this one)
        getter_AddRefs(newEntry));

      if (NS_SUCCEEDED(rv)) {
        newEntry->AsyncOpen(aCallback, nsICacheStorage::OPEN_TRUNCATE);
        newEntry->TransferCallbacks(callbacks, readOnlyCallbacks);

        // aCallback transfered to the new entry
        return true;
      }

      mozilla::MutexAutoLock lock(mLock);
      mCallbacks.AppendElements(callbacks);
      mReadOnlyCallbacks.AppendElements(readOnlyCallbacks);
    }
  }
  else if (writing) {
    // Not ready and writing, don't let others interfer.
    LOG(("  entry is being written, callback bypassed"));
    return false;
  }

  InvokeAvailableCallback(aCallback, aReadOnly);
  return true;
}

void CacheEntry::InvokeAvailableCallback(nsICacheEntryOpenCallback* aCallback,
                                         bool aReadOnly)
{
  // ifdef log and debug
  {
    mozilla::MutexAutoLock lock(mLock);
    LOG(("CacheEntry::InvokeAvailableCallback [this=%p, cb=%p, ready=%d, r/o=%d]",
      this, aCallback, (bool)mIsReady, aReadOnly));

    // When we are here, the entry must be loaded from disk
    MOZ_ASSERT(mIsLoaded);
  }

  if (!NS_IsMainThread()) {
    // Must happen on the main thread :(
    nsRefPtr<AvailableCallbackRunnable> event =
      new AvailableCallbackRunnable(this, aCallback, aReadOnly);
    NS_DispatchToMainThread(event);
    return;
  }


  bool ready, doomed;
  {
    mozilla::MutexAutoLock lock(mLock);

    ready = mIsReady;
    doomed = mIsDoomed;
  }

  if (doomed) {
    LOG(("  doomed, notifying OCEA with NS_ERROR_CACHE_KEY_NOT_FOUND"));
    aCallback->OnCacheEntryAvailable(nullptr, false, nullptr, NS_ERROR_CACHE_KEY_NOT_FOUND);
    return;
  }

  if (ready) {
    LOG(("  ready, notifying OCEA with entry and NS_OK"));
    BackgroundOp(Ops::FRECENCYUPDATE);
    aCallback->OnCacheEntryAvailable(this, false, nullptr, NS_OK);
    return;
  }

  if (aReadOnly) {
    LOG(("  r/o and not ready, notifying OCEA with NS_ERROR_CACHE_KEY_NOT_FOUND"));
    aCallback->OnCacheEntryAvailable(nullptr, false, nullptr, NS_ERROR_CACHE_KEY_NOT_FOUND);
    return;
  }

  // This is a new entry and needs to be fetched first.
  // The Handle blocks other consumers until the channel
  // either releases the entry or marks metadata as filled.

  // Consumer will be responsible to fill the entry metadata and data.
  {
    mozilla::MutexAutoLock lock(mLock);
    mIsWriting = true;
  }

  BackgroundOp(Ops::FRECENCYUPDATE);
  nsRefPtr<Handle> handle = new Handle(this);
  nsresult rv = aCallback->OnCacheEntryAvailable(handle, true, nullptr, NS_OK);

  if (NS_FAILED(rv)) {
    LOG(("  writing failed (0x%08x)", rv));

    // Consumer given a new entry failed to take care of the entry.
    OnWriterClosed(handle);
    return;
  }

  LOG(("  writing"));
}

void CacheEntry::OnWriterClosed(Handle const* aHandle)
{
  LOG(("CacheEntry::OnWriterClosed [this=%p, handle=%p]", this, aHandle));

  {
    BackgroundOp(Ops::REPORTUSAGE);

    mozilla::MutexAutoLock lock(mLock);

    LOG(("  ready=%d, loaded=%d, writing=%d", (bool)mIsReady, (bool)mIsLoaded, (bool)mIsWriting));
    if (!mIsWriting)
      return;

    mIsWriting = false;
  }

  InvokeCallbacks();
}

void CacheEntry::TransferCallbacks(nsCOMArray<nsICacheEntryOpenCallback> const &aCallbacks,
                                   nsCOMArray<nsICacheEntryOpenCallback> const &aReadOnlyCallbacks)
{
  bool invoke;
  {
    mozilla::MutexAutoLock lock(mLock);
    LOG(("CacheEntry::TransferCallbacks [entry=%p, %d, %d, %d, %d]",
      this, mCallbacks.Length(), mReadOnlyCallbacks.Length(), aCallbacks.Length(), aReadOnlyCallbacks.Length()));

    mCallbacks.AppendObjects(aCallbacks);
    mReadOnlyCallbacks.AppendObjects(aReadOnlyCallbacks);

    invoke = aCallbacks.Length() || aReadOnlyCallbacks.Length();
  }

  if (invoke)
    BackgroundOp(Ops::CALLBACKS, true);
}

already_AddRefed<CacheFile> CacheEntry::File()
{
  mozilla::MutexAutoLock lock(mLock);
  nsRefPtr<CacheFile> file(mFile);
  return file.forget();
}

bool CacheEntry::UsingDisk() const
{
  CacheStorageService::Self()->Lock().AssertCurrentThreadOwns();

  return mUseDisk;
}

bool CacheEntry::SetUsingDisk(bool aUsingDisk)
{
  // Called by the service when this entry is reopen to reflect
  // demanded storage target.
  CacheStorageService::Self()->Lock().AssertCurrentThreadOwns();

  bool changed = mUseDisk != aUsingDisk;
  mUseDisk = aUsingDisk;
  return changed;
}

uint32_t CacheEntry::GetMetadataMemoryOccupation() const
{
  CacheEntry* this_non_const = const_cast<CacheEntry*>(this);
  mozilla::MutexAutoLock lock(this_non_const->mLock);
  return mMetadataMemoryOccupation;
}

uint32_t CacheEntry::GetDataMemoryOccupation() const
{
  uint32_t size;
  CacheEntry* this_non_const = const_cast<CacheEntry*>(this);

  // TODO
  nsresult rv = this_non_const->GetDataSize(&size);
  if (NS_FAILED(rv))
    return 0;

  return size;
}

// nsICacheEntry

NS_IMETHODIMP CacheEntry::GetPersistToDisk(bool *aPersistToDisk)
{
  // No need to sync when only reading.
  // When consumer needs to be consistent with state of the memory storage entries
  // table, then let it use GetUseDisk getter that must be called under the service lock.
  *aPersistToDisk = mUseDisk;
  return NS_OK;
}
NS_IMETHODIMP CacheEntry::SetPersistToDisk(bool aPersistToDisk)
{
  if (mUseDisk == aPersistToDisk)
    return NS_OK;

  mozilla::MutexAutoLock lock(CacheStorageService::Self()->Lock());

  mUseDisk = aPersistToDisk;
  CacheStorageService::Self()->RecordMemoryOnlyEntry(
    this, !aPersistToDisk, false /* don't overwrite */);

  // File persistence is setup just before we open output stream on it.

  return NS_OK;
}

NS_IMETHODIMP CacheEntry::GetKey(nsACString & aKey)
{
  return mURI->GetAsciiSpec(aKey);
}

NS_IMETHODIMP CacheEntry::GetFetchCount(int32_t *aFetchCount)
{
  nsRefPtr<CacheFile> file(File());
  if (!file)
    return NS_ERROR_NOT_AVAILABLE;

  mozilla::MutexAutoLock lock(*file->GetLock());

  CacheFileMetadata* metadata = file->Metadata();
  MOZ_ASSERT(metadata);
  NS_ENSURE_TRUE(metadata, NS_ERROR_UNEXPECTED);

  return metadata->GetFetchCount(reinterpret_cast<uint32_t*>(aFetchCount));
}

NS_IMETHODIMP CacheEntry::GetLastFetched(uint32_t *aLastFetched)
{
  nsRefPtr<CacheFile> file(File());
  if (!file)
    return NS_ERROR_NOT_AVAILABLE;

  mozilla::MutexAutoLock lock(*file->GetLock());

  CacheFileMetadata* metadata = file->Metadata();
  MOZ_ASSERT(metadata);
  NS_ENSURE_TRUE(metadata, NS_ERROR_UNEXPECTED);

  return metadata->GetLastFetched(aLastFetched);
}

NS_IMETHODIMP CacheEntry::GetLastModified(uint32_t *aLastModified)
{
  nsRefPtr<CacheFile> file(File());
  if (!file)
    return NS_ERROR_NOT_AVAILABLE;

  mozilla::MutexAutoLock lock(*file->GetLock());

  CacheFileMetadata* metadata = file->Metadata();
  MOZ_ASSERT(metadata);
  NS_ENSURE_TRUE(metadata, NS_ERROR_UNEXPECTED);

  return metadata->GetLastModified(aLastModified);
  return NS_OK;
}

NS_IMETHODIMP CacheEntry::GetExpirationTime(uint32_t *aExpirationTime)
{
  nsRefPtr<CacheFile> file(File());
  if (!file)
    return NS_ERROR_NOT_AVAILABLE;

  mozilla::MutexAutoLock lock(*file->GetLock());

  CacheFileMetadata* metadata = file->Metadata();
  MOZ_ASSERT(metadata);
  NS_ENSURE_TRUE(metadata, NS_ERROR_UNEXPECTED);

  return metadata->GetExpirationTime(aExpirationTime);
}

NS_IMETHODIMP CacheEntry::SetExpirationTime(uint32_t aExpirationTime)
{
  nsRefPtr<CacheFile> file(File());
  if (!file)
    return NS_ERROR_NOT_AVAILABLE;

  {
    mozilla::MutexAutoLock lock(*file->GetLock());

    CacheFileMetadata* metadata = file->Metadata();
    MOZ_ASSERT(metadata);
    NS_ENSURE_TRUE(metadata, NS_ERROR_UNEXPECTED);

    nsresult rv = metadata->SetExpirationTime(aExpirationTime);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  // Aligned assignment, thus atomic.
  mSortingExpirationTime = aExpirationTime;
  return NS_OK;
}

NS_IMETHODIMP CacheEntry::OpenInputStream(uint32_t offset, nsIInputStream * *_retval)
{
  LOG(("CacheEntry::OpenInputStream [this=%p]", this));

  nsRefPtr<CacheFile> file;
  {
    mozilla::MutexAutoLock lock(mLock);

    if (mIsDoomed) {
      LOG(("  doomed..."));
      return NS_ERROR_NOT_AVAILABLE;
    }

    file = mFile;
  }

  if (!file)
    return NS_ERROR_NOT_AVAILABLE;

  nsresult rv;

  nsCOMPtr<nsIInputStream> stream;
  rv = file->OpenInputStream(getter_AddRefs(stream));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsISeekableStream> seekable =
    do_QueryInterface(stream, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = seekable->Seek(nsISeekableStream::NS_SEEK_SET, offset);
  NS_ENSURE_SUCCESS(rv, rv);

  stream.forget(_retval);
  return NS_OK;
}

NS_IMETHODIMP CacheEntry::OpenOutputStream(uint32_t offset, nsIOutputStream * *_retval)
{
  LOG(("CacheEntry::OpenOutputStream [this=%p]", this));

  nsRefPtr<CacheFile> file;
  {
    mozilla::MutexAutoLock lock(mLock);

    if (mIsDoomed) {
      LOG(("  doomed..."));
      return NS_ERROR_NOT_AVAILABLE;
    }

    file = mFile;
  }

  if (!file)
    return NS_ERROR_NOT_AVAILABLE;

  nsresult rv;

  // No need to sync on mUseDisk here, we don't need to be consistent
  // with content of memory storage entries hash table.
  // TODO test no-store response followed by normal response ; maybe
  // reopen the file here?  what to do with the metadata?  probably reopen
  // the file in CacheFile internally
  rv = file->SetMemoryOnly(!mUseDisk);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIOutputStream> stream;
  rv = file->OpenOutputStream(getter_AddRefs(stream));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsISeekableStream> seekable =
    do_QueryInterface(stream, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = seekable->Seek(nsISeekableStream::NS_SEEK_SET, offset);
  NS_ENSURE_SUCCESS(rv, rv);

  stream.forget(_retval);
  return NS_OK;
}

NS_IMETHODIMP CacheEntry::GetPredictedDataSize(int64_t *aPredictedDataSize)
{
  *aPredictedDataSize = mPredictedDataSize;
  return NS_OK;
}
NS_IMETHODIMP CacheEntry::SetPredictedDataSize(int64_t aPredictedDataSize)
{
  mPredictedDataSize = aPredictedDataSize;
  return NS_OK;
}

NS_IMETHODIMP CacheEntry::GetSecurityInfo(nsISupports * *aSecurityInfo)
{
  nsRefPtr<CacheFile> file;
  {
    mozilla::MutexAutoLock lock(mLock);

    if (mSecurityInfoLoaded) {
      NS_IF_ADDREF(*aSecurityInfo = mSecurityInfo);
      return NS_OK;
    }

    if (!mFile)
      return NS_ERROR_NOT_AVAILABLE;

    file = mFile;
  }

  char const* info;
  nsCOMPtr<nsISupports> secInfo;
  {
    mozilla::MutexAutoLock lock(*file->GetLock());

    CacheFileMetadata* metadata = file->Metadata();
    MOZ_ASSERT(metadata);
    NS_ENSURE_TRUE(metadata, NS_ERROR_UNEXPECTED);

    info = metadata->GetElement("security-info");
  }

  if (info) {
    nsresult rv = NS_DeserializeObject(nsDependentCString(info),
                                       getter_AddRefs(secInfo));
    NS_ENSURE_SUCCESS(rv, rv);
  }

  {
    mozilla::MutexAutoLock lock(mLock);

    mSecurityInfoLoaded = true;
    mSecurityInfo.swap(secInfo);

    NS_IF_ADDREF(*aSecurityInfo = mSecurityInfo);
  }

  return NS_OK;
}
NS_IMETHODIMP CacheEntry::SetSecurityInfo(nsISupports *aSecurityInfo)
{
  nsresult rv;

  nsRefPtr<CacheFile> file;
  {
    mozilla::MutexAutoLock lock(mLock);

    mSecurityInfo = aSecurityInfo;
    mSecurityInfoLoaded = true;

    if (!mFile)
      return NS_ERROR_NOT_AVAILABLE;

    file = mFile;
  }

  nsCOMPtr<nsISerializable> serializable =
    do_QueryInterface(aSecurityInfo);
  if (aSecurityInfo && !serializable)
    return NS_ERROR_UNEXPECTED;

  nsCString info;
  if (serializable) {
    rv = NS_SerializeToString(serializable, info);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  {
    mozilla::MutexAutoLock lock(*file->GetLock());

    CacheFileMetadata* metadata = file->Metadata();
    MOZ_ASSERT(metadata);
    NS_ENSURE_TRUE(metadata, NS_ERROR_UNEXPECTED);

    rv = metadata->SetElement("security-info", info.Length() ? info.get() : nullptr);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}

NS_IMETHODIMP CacheEntry::GetStorageDataSize(uint32_t *aStorageDataSize)
{
  mozilla::MutexAutoLock lock(mLock);

  *aStorageDataSize = 0; // will be reported in callback from file or a getter on file will be exposed
  return NS_OK;
}

NS_IMETHODIMP CacheEntry::AsyncDoom(nsICacheEntryDoomCallback *aCallback)
{
  {
    mozilla::MutexAutoLock lock(mLock);

    if (mDoomCallback || mIsDoomed)
      return NS_ERROR_IN_PROGRESS;

    mIsDoomed = true;
    mDoomCallback = aCallback;
  }

  // Immediately remove the entry from the storage hash table
  CacheStorageService::Self()->RemoveEntry(this);
  BackgroundOp(Ops::DOOM);

  return NS_OK;
}

NS_IMETHODIMP CacheEntry::GetMetaDataElement(const char * aKey, char * *aRetval)
{
  nsRefPtr<CacheFile> file(File());
  if (!file)
    return NS_ERROR_NOT_AVAILABLE;

  mozilla::MutexAutoLock lock(*file->GetLock());

  CacheFileMetadata* metadata = file->Metadata();
  MOZ_ASSERT(metadata);
  NS_ENSURE_TRUE(metadata, NS_ERROR_UNEXPECTED);

  char const *value = metadata->GetElement(aKey);
  if (!value)
    return NS_ERROR_NOT_AVAILABLE;

  *aRetval = NS_strdup(value);
  return NS_OK;
}

NS_IMETHODIMP CacheEntry::SetMetaDataElement(const char * aKey, const char * aValue)
{
  nsRefPtr<CacheFile> file(File());
  if (!file)
    return NS_ERROR_NOT_AVAILABLE;

  mozilla::MutexAutoLock lock(*file->GetLock());

  CacheFileMetadata* metadata = file->Metadata();
  MOZ_ASSERT(metadata);
  NS_ENSURE_TRUE(metadata, NS_ERROR_UNEXPECTED);

  return metadata->SetElement(aKey, aValue);
}

NS_IMETHODIMP CacheEntry::MetaDataReady()
{
  LOG(("CacheEntry::MetaDataReady [this=%p, ready=%d]", this, (bool)mIsReady));

  {
    mozilla::MutexAutoLock lock(mLock);
    if (mIsReady)
      return NS_OK;

    mIsReady = true;
    mIsWriting = false;
  }

  BackgroundOp(Ops::REPORTUSAGE);
  InvokeCallbacks();
  return NS_OK;
}

NS_IMETHODIMP CacheEntry::SetDataSize(uint32_t size)
{
  // ?
  mDataSize = size;
  return NS_OK;
}

NS_IMETHODIMP CacheEntry::GetDataSize(uint32_t *aDataSize)
{
  *aDataSize = 0;

  nsRefPtr<CacheFile> file;
  {
    mozilla::MutexAutoLock lock(mLock);
    if (mIsWriting)
      return NS_ERROR_IN_PROGRESS;

    mFile = file;
  }

  // mayhemer: TODO Problem with compression
  if (!file)
    return NS_OK; // really OK?

  *aDataSize = file->DataSize();
  return NS_OK;
}

NS_IMETHODIMP CacheEntry::MarkValid()
{
  // NOT IMPLEMENTED ACTUALLY
  return NS_OK;
}

NS_IMETHODIMP CacheEntry::Close()
{
  // NOT IMPLEMENTED ACTUALLY
  return NS_OK;
}

NS_IMETHODIMP CacheEntry::GetStoragePolicy(nsCacheStoragePolicy *aStoragePolicy)
{
  // NOT IMPLEMENTED ACTUALLY
  return NS_OK;
}
NS_IMETHODIMP CacheEntry::SetStoragePolicy(nsCacheStoragePolicy aStoragePolicy)
{
  // NOT IMPLEMENTED ACTUALLY
  return NS_OK;
}

// nsIRunnable

NS_IMETHODIMP CacheEntry::Run()
{
  MOZ_ASSERT(CacheStorageService::IsOnManagementThread());

  uint32_t ops;
  {
    mozilla::MutexAutoLock lock(mLock);
    ops = mBackgroundOperations.Grab();
  }

  BackgroundOp(ops);
  return NS_OK;
}

// Management methods

double CacheEntry::GetFrecency() const
{
  MOZ_ASSERT(CacheStorageService::IsOnManagementThread());
  return mFrecency;
}

uint32_t CacheEntry::GetExpirationTime() const
{
  MOZ_ASSERT(CacheStorageService::IsOnManagementThread());
  return mSortingExpirationTime;
}

uint32_t& CacheEntry::ReportedMemorySize()
{
  MOZ_ASSERT(CacheStorageService::IsOnManagementThread());
  return mReportedMemorySize;
}

bool CacheEntry::IsRegistered() const
{
  MOZ_ASSERT(CacheStorageService::IsOnManagementThread());
  return mIsRegistered;
}

bool CacheEntry::CanRegister() const
{
  MOZ_ASSERT(CacheStorageService::IsOnManagementThread());
  return !mIsRegistered && mIsRegistrationAllowed;
}

void CacheEntry::SetRegistered(bool aRegistered)
{
  MOZ_ASSERT(CacheStorageService::IsOnManagementThread());
  MOZ_ASSERT(mIsRegistrationAllowed);

  mIsRegistered = aRegistered;

  if (!aRegistered) // Never allow registration again
    mIsRegistrationAllowed = false;
}

bool CacheEntry::Purge(uint32_t aWhat)
{
  LOG(("CacheEntry::Purge [this=%p, what=%d]", this, aWhat));

  MOZ_ASSERT(CacheStorageService::IsOnManagementThread());

  {
    switch (aWhat) {
    case PURGE_DATA_ONLY_DISK_BACKED:
    case PURGE_WHOLE_ONLY_DISK_BACKED:
      // This is an in-memory only entry, don't purge it
      if (!mUseDisk) {
        LOG(("  not using disk"));
        return false;
      }
    }

    mozilla::MutexAutoLock lock(mLock);

    if (mIsWriting || mIsLoading || mFrecency == 0) {
      // In-progress (write or load) entries should (at least for consistency and from
      // the logical point of view) stay in memory.
      // Zero-frecency entries are those which have never been given to any consumer, those
      // are actually very fresh and should not go just because frecency had not been set
      // so far.
      LOG(("  is writing=%d, loading=%d, frecency=%1.10f", mIsWriting, mIsLoading, mFrecency));
      return false;
    }
  }

  switch (aWhat) {
  case PURGE_WHOLE_ONLY_DISK_BACKED:
  case PURGE_WHOLE:
    {
      CacheStorageService::Self()->UnregisterEntry(this);
      CacheStorageService::Self()->RemoveEntry(this);

      CacheStorageService::Self()->OnMemoryConsumptionChange(this, 0);

      // Entry removed it self from control arrays, return true
      return true;
    }

  case PURGE_DATA_ONLY_DISK_BACKED:
    {
      uint32_t metadataSize;

      nsRefPtr<CacheFile> file;
      {
        mozilla::MutexAutoLock lock(mLock);
        file = mFile;
        metadataSize = mMetadataMemoryOccupation;
      }

      if (file) {
        // TODO
        // file->ThrowMemoryCachedData();
      }

      CacheStorageService::Self()->OnMemoryConsumptionChange(this, metadataSize);

      // Entry has been left in control arrays, return false (not purged)
      return false;
    }
  }

  LOG(("  ?"));
  return false;
}

void CacheEntry::PurgeAndDoom()
{
  LOG(("CacheEntry::PurgeAndDoom [this=%p]", this));

  MOZ_ASSERT(CacheStorageService::IsOnManagementThread());

  CacheStorageService::Self()->RemoveEntry(this);
  DoomAlreadyRemoved();
}

void CacheEntry::DoomAlreadyRemoved()
{
  LOG(("CacheEntry::DoomAlreadyRemoved [this=%p]", this));

  {
    mozilla::MutexAutoLock lock(mLock);
    mIsDoomed = true;
  }

  if (!CacheStorageService::IsOnManagementThread()) {
    BackgroundOp(Ops::DOOM);
    return;
  }

  CacheStorageService::Self()->UnregisterEntry(this);
  CacheStorageService::Self()->OnMemoryConsumptionChange(this, 0);

  nsCOMPtr<nsICacheEntryDoomCallback> callback;
  nsRefPtr<CacheFile> file;
  bool invokeCallbacks;
  {
    mozilla::MutexAutoLock lock(mLock);

    invokeCallbacks = mCallbacks.Length() || mReadOnlyCallbacks.Length();

    // Otherwise wait for the file to be doomed
    if (!mFile)
      mDoomCallback.swap(callback);
    else
      file = mFile;
  }

  if (invokeCallbacks) {
    // Must force post here since may be indirectly called from
    // InvokeCallback of this entry and we don't want reentrancy here.
    BackgroundOp(Ops::CALLBACKS, true);
  }

  if (file) {
    file->Doom(this);
  }
  else if (callback) {
    nsRefPtr<DoomCallbackRunnable> event =
      new DoomCallbackRunnable(callback, NS_OK);
    NS_DispatchToMainThread(event);
  }
}

void CacheEntry::BackgroundOp(uint32_t aOperations, bool aForceAsync)
{
  if (!CacheStorageService::IsOnManagementThread() || aForceAsync) {
    mozilla::MutexAutoLock lock(mLock);

    if (mBackgroundOperations.Set(aOperations))
      CacheStorageService::Self()->Dispatch(this);

    LOG(("CacheEntry::BackgroundOp this=%p dipatch of %x", this, aOperations));
    return;
  }

  MOZ_ASSERT(CacheStorageService::IsOnManagementThread());

  if (aOperations & Ops::FRECENCYUPDATE) {
    #ifndef M_LN2
    #define M_LN2 0.69314718055994530942
    #endif

    // Half-life is 90 days.
    static double const half_life = 90.0 * (24 * 60 * 60);
    // Must convert from seconds to milliseconds since PR_Now() gives usecs.
    static double const decay = (M_LN2 / half_life) / static_cast<double>(PR_USEC_PER_SEC);

    double now_decay = static_cast<double>(PR_Now()) * decay;

    if (mFrecency == 0) {
      mFrecency = now_decay;
    }
    else {
      // TODO: when C++11 enabled, use std::log1p(n) which is equal to log(n + 1) but
      // more precise.
      mFrecency = log(exp(mFrecency - now_decay) + 1) + now_decay;
    }
    LOG(("CacheEntry FRECENCYUPDATE [this=%p, frecency=%1.10f]", this, mFrecency));
  }

  if (aOperations & Ops::REGISTER) {
    LOG(("CacheEntry REGISTER [this=%p]", this));

    CacheStorageService::Self()->RegisterEntry(this);
  }

  if (aOperations & Ops::REPORTUSAGE) {
    LOG(("CacheEntry REPORTUSAGE [this=%p]", this));

    uint32_t memorySize;

    // Should get only what is actually consumed in memory..
    // TODO - definitely more here to do
    nsresult rv = GetStorageDataSize(&memorySize);
    if (NS_FAILED(rv))
      memorySize = 0;

    {
      mozilla::MutexAutoLock lock(mLock);
      memorySize += mMetadataMemoryOccupation;
    }

    CacheStorageService::Self()->OnMemoryConsumptionChange(this, memorySize);
  }

  if (aOperations & Ops::DOOM) {
    LOG(("CacheEntry DOOM [this=%p]", this));

    DoomAlreadyRemoved();
  }

  if (aOperations & Ops::CALLBACKS) {
    LOG(("CacheEntry CALLBACKS (invoke) [this=%p]", this));

    InvokeCallbacks();
  }
}

} // net
} // mozilla
