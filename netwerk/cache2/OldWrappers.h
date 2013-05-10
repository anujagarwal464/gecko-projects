// Stuff to link the old imp to the new api - will go away!

#ifndef OLDWRAPPERS__H__
#define OLDWRAPPERS__H__

#include "nsICacheEntry.h"
#include "nsICacheListener.h"

#include "nsCOMPtr.h"
#include "nsICacheEntryOpenCallback.h"
#include "nsICacheEntryDescriptor.h"
#include "nsThreadUtils.h"

class nsIURI;
class nsICacheEntryOpenCallback;
class nsIApplicationCache;

namespace mozilla { namespace net {

class CacheStorage;

class _OldDescriptorWrapper : public nsICacheEntry /*, public nsICacheEntryDescriptor*/
{
public:
  NS_DECL_ISUPPORTS
  NS_FORWARD_NSICACHEENTRY(mOldDesc->)

  _OldDescriptorWrapper(nsICacheEntryDescriptor* desc) : mOldDesc(desc) {}

private:
  virtual ~_OldDescriptorWrapper() {}

  nsCOMPtr<nsICacheEntryDescriptor> mOldDesc;
};


class _OldGenericCacheLoad : public nsRunnable
                           , public nsICacheListener
{
public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIRUNNABLE
  NS_DECL_NSICACHELISTENER

  _OldGenericCacheLoad(nsIURI* aURI,
                       nsICacheEntryOpenCallback* aCallback,
                       CacheStorage* aCacheStorage,
                       bool aTruncate)
    : mURI(aURI)
    , mCallback(aCallback)
    , mStorage(aCacheStorage)
    , mTruncate(aTruncate)
    , mNew(true)
    , mStatus(NS_ERROR_UNEXPECTED)
    , mRunCount(0)
  {}
  virtual ~_OldGenericCacheLoad();

  nsresult Start();

protected:
  nsCOMPtr<nsIEventTarget> mCacheThread;

  nsCOMPtr<nsIURI> mURI;
  nsCOMPtr<nsICacheEntryOpenCallback> mCallback;
  nsRefPtr<CacheStorage> mStorage;
  bool mTruncate;

  bool mNew;
  nsCOMPtr<nsICacheEntry> mCacheEntry;
  nsresult mStatus;
  uint32_t mRunCount;
  nsCOMPtr<nsIApplicationCache> mAppCache;
};


class _OldApplicationCacheLoad : public _OldGenericCacheLoad
{
public:
  NS_DECL_NSIRUNNABLE

  _OldApplicationCacheLoad(nsIURI* aURI,
                           nsICacheEntryOpenCallback* aCallback,
                           nsIApplicationCache* aAppCache,
                           CacheStorage* aCacheStorage,
                           bool aTruncate)
    : _OldGenericCacheLoad(aURI, aCallback, aCacheStorage, aTruncate)
  {
    mAppCache = aAppCache;
  }
  virtual ~_OldApplicationCacheLoad() {}
};

}}

#endif
