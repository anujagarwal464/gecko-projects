/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CacheIndex__h__
#define CacheIndex__h__

#include "CacheLog.h"
#include "CacheFileIOManager.h"
#include "CacheHashUtils.h"
#include "nsICacheEntry.h"
#include "nsILoadContextInfo.h"
#include "nsTHashtable.h"
#include "mozilla/SHA1.h"
#include "mozilla/Mutex.h"
#include "prnetdb.h"

class nsIFile;

namespace mozilla {
namespace net {

typedef struct {
  uint32_t            mVersion;
  uint32_t            mTimeStamp; // for quick entry file validation
  uint32_t            mIsDirty;
} CacheIndexHeader;

struct CacheIndexRecord {
  SHA1Sum::Hash mHash;
  uint32_t      mFrecency;
  uint32_t      mExpirationTime;
  uint32_t      mAppId;

  /*
   *    1000 0000 0000 0000 0000 0000 0000 0000 : anonymous
   *    0100 0000 0000 0000 0000 0000 0000 0000 : inBrowser
   *    0010 0000 0000 0000 0000 0000 0000 0000 : removed
   *    0001 0000 0000 0000 0000 0000 0000 0000 : dirty
   *    0000 1111 0000 0000 0000 0000 0000 0000 : reserved
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
    memcpy(&mRec.mHash, aKey, sizeof(SHA1Sum::Hash));
  }
  CacheIndexEntry(const CacheIndexEntry& aOther)
  {
    NS_NOTREACHED("CacheIndexEntry copy constructor is forbidden!");
  }
  ~CacheIndexEntry()
  {
    MOZ_COUNT_DTOR(CacheIndexEntry);
  }

  // KeyEquals(): does this entry match this key?
  bool KeyEquals(KeyTypePointer aKey) const
  {
    return memcmp(&mRec.mHash, aKey, sizeof(SHA1Sum::Hash)) == 0;
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
    return KeyEquals(&aOther.mRec.mHash);
  }

  CacheIndexEntry& operator=(const CacheIndexEntry& aOther)
  {
    MOZ_ASSERT(memcmp(&mRec.mHash, &aOther.mRec.mHash,
               sizeof(SHA1Sum::Hash)) == 0);
    mRec.mFrecency = aOther.mRec.mFrecency;
    mRec.mExpirationTime = aOther.mRec.mExpirationTime;
    mRec.mAppId = aOther.mRec.mAppId;
    mRec.mFlags = aOther.mRec.mFlags;
    return *this;
  }

  void InitNew()
  {
    mRec.mFrecency = 0;
    mRec.mExpirationTime = nsICacheEntry::NO_EXPIRATION_TIME;
    mRec.mAppId = nsILoadContextInfo::NO_APP_ID;
    mRec.mFlags = 0;
  }

  void Init(uint32_t aAppId, bool aAnonymous, bool aInBrowser)
  {
    mRec.mAppId = aAppId;
    if (aAnonymous)
      mRec.mFlags |= eAnonymousMask;
    if (aInBrowser)
      mRec.mFlags |= eInBrowserMask;

    mRec.mFlags |= eInitializedMask;
  }

  const SHA1Sum::Hash * Hash() { return &mRec.mHash; }

  bool IsInitialized() { return !!(mRec.mFlags & eInitializedMask); }

  uint32_t AppId() { return mRec.mAppId; }
  bool     Anonymous() { return !!(mRec.mFlags & eAnonymousMask); }
  bool     InBrowser() { return !!(mRec.mFlags & eInBrowserMask); }

  bool IsRemoved() { return !!(mRec.mFlags & eRemovedMask); }
  void MarkRemoved() { mRec.mFlags |= eRemovedMask; }

  bool IsDirty() { return !!(mRec.mFlags & eDirtyMask); }
  void MarkDirty() { mRec.mFlags |= eDirtyMask; }
  void ClearDirty() { mRec.mFlags &= ~eDirtyMask; }

  bool IsFresh() { return !!(mRec.mFlags & eFreshMask); }
  void MarkFresh() { mRec.mFlags |= eFreshMask; }

  void     SetFrecency(uint32_t aFrecency) { mRec.mFrecency = aFrecency; }
  uint32_t GetFrecency() { return mRec.mFrecency; }

  void     SetExpirationTime(uint32_t aExpirationTime)
  {
    mRec.mExpirationTime = aExpirationTime;
  }
  uint32_t GetExpirationTime() { return mRec.mExpirationTime; }

  void     SetFileSize(uint32_t aFileSize)
  {
    MOZ_ASSERT((aFileSize & ~eFileSizeMask) == 0);
    mRec.mFlags &= ~eFileSizeMask;
    mRec.mFlags |= aFileSize;
  }
  uint32_t GetFileSize() { return mRec.mFlags & eFileSizeMask; }
  bool     IsEmpty() { return GetFileSize() == 0; }

  void WriteToBuf(void *aBuf)
  {
    CacheIndexRecord *dst = reinterpret_cast<CacheIndexRecord *>(aBuf);
    memcpy(aBuf, &mRec, sizeof(CacheIndexRecord));
    // clear dirty flag
    dst->mFlags &= ~eDirtyMask;
    // clear fresh flag
    dst->mFlags &= ~eFreshMask;

#if defined(IS_LITTLE_ENDIAN)
    dst->mFrecency = PR_htonl(dst->mFrecency);
    dst->mExpirationTime = PR_htonl(dst->mExpirationTime);
    dst->mAppId = PR_htonl(dst->mAppId);
    dst->mFlags = PR_htonl(dst->mFlags);
#endif
  }

  void ReadFromBuf(void *aBuf)
  {
    CacheIndexRecord *src= reinterpret_cast<CacheIndexRecord *>(aBuf);
    MOZ_ASSERT(memcmp(&mRec.mHash, &src->mHash,
               sizeof(SHA1Sum::Hash)) == 0);

    mRec.mFrecency = PR_ntohl(src->mFrecency);
    mRec.mExpirationTime = PR_ntohl(src->mExpirationTime);
    mRec.mAppId = PR_ntohl(src->mAppId);
    mRec.mFlags = PR_ntohl(src->mFlags);
  }

private:
  friend class CacheIndex;

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

  CacheIndexRecord mRec;
};

class CacheIndexEmptyEntry : public PLDHashEntryHdr
{
public:
  typedef const SHA1Sum::Hash& KeyType;
  typedef const SHA1Sum::Hash* KeyTypePointer;

  CacheIndexEmptyEntry(KeyTypePointer aKey)
  {
    MOZ_COUNT_CTOR(CacheIndexEmptyEntry);
    memcpy(&mHash, aKey, sizeof(SHA1Sum::Hash));
  }
  CacheIndexEmptyEntry(const CacheIndexEmptyEntry& aOther)
  {
    NS_ERROR("ALLOW_MEMMOVE == true, should never be called");
    memcpy(&mHash, &aOther.mHash, sizeof(SHA1Sum::Hash));
  }
  ~CacheIndexEmptyEntry()
  {
    MOZ_COUNT_DTOR(CacheIndexEmptyEntry);
  }

  // KeyEquals(): does this entry match this key?
  bool KeyEquals(KeyTypePointer aKey) const
  {
    return memcmp(&mHash, aKey, sizeof(SHA1Sum::Hash)) == 0;
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

  bool operator==(const CacheIndexEmptyEntry& aOther) const
  {
    return KeyEquals(&aOther.mHash);
  }

  const SHA1Sum::Hash * Hash() { return &mHash; }

private:
  SHA1Sum::Hash mHash;
};

class CacheIndexStats
{
public:
  CacheIndexStats()
    : mStateLogged(false)
    , mCount(0)
    , mNotInitialized(0)
    , mRemoved(0)
    , mDirty(0)
    , mEmpty(0)
    , mSize(0)
  {}

  void Reset() {
    mCount = 0;
    mNotInitialized = 0;
    mRemoved = 0;
    mDirty = 0;
    mEmpty = 0;
    mSize = 0;
  }

  void Log() {
    LOG(("CacheIndexStats::Log() [count=%u, notInitialized=%u, removed=%u, "
         "dirty=%u, empty=%u, size=%lld]", mCount, mNotInitialized, mRemoved,
         mDirty, mEmpty, mSize));
  }

  void BeforeChange(CacheIndexEntry *aEntry) {
    MOZ_ASSERT(!mStateLogged);
    mStateLogged = !mStateLogged;
    if (aEntry) {
      MOZ_ASSERT(mCount);
      mCount--;
      if (aEntry->IsDirty()) {
        MOZ_ASSERT(mDirty);
        mDirty--;
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
    MOZ_ASSERT(mStateLogged);
    mStateLogged = !mStateLogged;
    if (aEntry) {
      mCount++;
      if (aEntry->IsDirty()) mDirty++;
      if (aEntry->IsRemoved()) mRemoved++;
      else {
        if (!aEntry->IsInitialized()) mNotInitialized++;
        else {
          if (aEntry->IsEmpty()) mEmpty++;
          else mSize += aEntry->GetFileSize();
        }
      }
    }
  }

  uint32_t Count() {
    MOZ_ASSERT(!mStateLogged);
    return mCount;
  }

  uint32_t Dirty() {
    MOZ_ASSERT(!mStateLogged);
    return mDirty;
  }

  uint32_t ActiveEntriesCount() {
    MOZ_ASSERT(!mStateLogged);
    return mCount - mRemoved - mNotInitialized - mEmpty;
  }

  int64_t Size() {
    MOZ_ASSERT(!mStateLogged);
    return mSize;
  }

private:
  bool     mStateLogged;
  uint32_t mCount;
  uint32_t mNotInitialized;
  uint32_t mRemoved;
  uint32_t mDirty;
  uint32_t mEmpty;
  int64_t  mSize;
};


class CacheIndex : public CacheFileIOListener
{
public:
  NS_DECL_THREADSAFE_ISUPPORTS

  CacheIndex();

  static nsresult Init(nsIFile *aCacheDirectory);
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

  NS_IMETHOD OnFileOpened(CacheFileHandle *aHandle, nsresult aResult);
  NS_IMETHOD OnDataWritten(CacheFileHandle *aHandle, const char *aBuf,
                           nsresult aResult);
  NS_IMETHOD OnDataRead(CacheFileHandle *aHandle, char *aBuf, nsresult aResult);
  NS_IMETHOD OnFileDoomed(CacheFileHandle *aHandle, nsresult aResult);
  NS_IMETHOD OnEOFSet(CacheFileHandle *aHandle, nsresult aResult);
  NS_IMETHOD OnFileRenamed(CacheFileHandle *aHandle, nsresult aResult);

private:
  virtual ~CacheIndex();

  nsresult InitInternal(nsIFile *aCacheDirectory);

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
  static PLDHashOperator RemoveEntryFromIndex(CacheIndexEmptyEntry *aEntry,
                                              void* aClosure);
  static PLDHashOperator UpdateEntryInIndex(CacheIndexEntry *aEntry,
                                            void* aClosure);

  bool ShouldWriteIndexToDisk();
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
  void FinishRead(bool aSucceeded);

  static PLDHashOperator ProcessJournalEntry(CacheIndexEntry *aEntry,
                                             void* aClosure);

  void StartRebuildingIndex();
  void StartUpdatingIndex();

  enum EState {
    INITIAL  = 0,
    READING  = 1,
    WRITING  = 2,
    BUILDING = 3,
    UPDATING = 4,
    READY    = 5,
    SHUTDOWN = 7
  };

#ifdef MOZ_LOGGING
  static char const * StateString(EState aState);
#endif
  void ChangeState(EState aNewState);
  void AllocBuffer();
  void ReleaseBuffer();

  static CacheIndex *gInstance;

  nsCOMPtr<nsIFile> mCacheDirectory;

  mozilla::Mutex mLock;
  EState         mState;
  bool           mIndexNeedsUpdate;
  bool           mIndexOnDiskIsValid;
  uint32_t       mIndexTimeStamp;
  PRIntervalTime mLastDumpTime;

  uint32_t                  mSkipEntries;
  char                     *mRWBuf;
  uint32_t                  mRWBufSize;
  uint32_t                  mRWBufPos;
  nsRefPtr<CacheHash>       mHash;

  uint32_t                  mReadOpenCount;
  bool                      mReadFailed;

  nsRefPtr<CacheFileHandle> mHandle;
  nsRefPtr<CacheFileHandle> mHandle2;

  nsTHashtable<CacheIndexEntry> mIndex;
  CacheIndexStats               mIndexStats;
  nsTHashtable<CacheIndexEntry> mTmpJournal;

  nsTHashtable<CacheIndexEntry>      mPendingUpdates;
  nsTHashtable<CacheIndexEmptyEntry> mPendingRemovals;
};


} // net
} // mozilla

#endif
