/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CacheLog.h"
#include "CacheFileUtils.h"
#include "LoadContextInfo.h"
#include "nsCOMPtr.h"
#include "nsAutoPtr.h"
#include "nsString.h"


namespace mozilla {
namespace net {
namespace CacheFileUtils {

namespace { // anon

/**
 * A simple recursive descent parser for the mapping key.
 */
class KeyParser
{
public:
  KeyParser(nsACString::const_iterator aCaret, nsACString::const_iterator aEnd)
    : caret(aCaret)
    , end(aEnd)
    // Initialize attributes to their default values
    , appId(nsILoadContextInfo::NO_APP_ID)
    , isPrivate(false)
    , isInBrowser(false)
    , isAnonymous(false)
    // Initialize the cache key to a zero length by default
    , cacheKey(aEnd)
  {
  }

private:
  // Current character being parsed
  nsACString::const_iterator caret;
  // The end of the buffer
  nsACString::const_iterator const end;

  // Results
  uint32_t appId;
  bool isPrivate;
  bool isInBrowser;
  bool isAnonymous;
  // Position of the cache key, if present
  nsACString::const_iterator cacheKey;

  bool ParseTags()
  {
    // Expects to be at the tag name or at the end
    if (caret == end)
      return true;

    // 'Read' the tag name and move to the next char
    switch (*caret++) {
    case ':':
      // optional terminator, followed by optional cache key
      cacheKey = caret;
      caret = end;
      return true;
    case 'p':
      isPrivate = true;
      break;
    case 'b':
      isInBrowser = true;
      break;
    case 'a':
      isAnonymous = true;
      break;
    case 'i': {
      nsAutoCString appIdString;
      if (!ParseValue(&appIdString))
        return false;

      nsresult rv;
      appId = appIdString.ToInteger(&rv);
      if (NS_FAILED(rv))
        return false; // appid value is mandatory

      break;
    }
    default:
      if (!ParseValue()) // skip any tag values, optional
        return false;
      break;
    }

    // Recurse to the next tag
    return ParseNextTagOrEnd();
  }

  bool ParseNextTagOrEnd()
  {
    // We expect a comma after every tag
    if (caret == end || *caret++ != ',')
      return false;

    // Go to another tag
    return ParseTags();
  }

  bool ParseValue(nsAutoCString * result = nullptr)
  {
    // If at the end, fail since we expect a comma ; value may be empty tho
    if (caret == end)
      return false;

    // Remeber where the value starts
    nsACString::const_iterator val = caret;
    nsACString::const_iterator comma = end;
    bool escape = false;
    while (caret != end) {
      nsACString::const_iterator at = caret;
      ++caret; // we can safely break/continue the loop now

      if (*at == ',') {
        if (comma != end) {
          // another comma (we have found ",," -> escape)
          comma = end;
          escape = true;
        } else {
          comma = at;
        }
        continue;
      }

      if (comma != end) {
        // after a single comma
        break;
      }
    }

    // At this point |comma| points to the last and lone ',' we've hit.
    // If a lone comma was not found, |comma| is at the end of the buffer,
    // that is not expected and we return failure.

    caret = comma;
    if (result) {
      result->Assign(Substring(val, caret));
      if (escape)
        result->ReplaceSubstring(NS_LITERAL_CSTRING(",,"), NS_LITERAL_CSTRING(","));
    }

    return caret != end;
  }

public:
  already_AddRefed<LoadContextInfo> Parse()
  {
    nsRefPtr<LoadContextInfo> info;
    if (ParseTags())
      info = GetLoadContextInfo(isPrivate, appId, isInBrowser, isAnonymous);

    return info.forget();
  }

  void CacheKey(nsACString &result)
  {
    // cacheKey is either pointing to end or the position where the cache key is.
    result.Assign(Substring(cacheKey, end));
  }
};

} // anon

already_AddRefed<nsILoadContextInfo>
ParseKey(const nsACString &aKey, nsACString *aCacheKey)
{
  nsACString::const_iterator caret, end;
  aKey.BeginReading(caret);
  aKey.EndReading(end);

  KeyParser parser(caret, end);
  nsRefPtr<LoadContextInfo> info = parser.Parse();

  if (info && aCacheKey)
    parser.CacheKey(*aCacheKey);

  return info.forget();
}

void
CreateKeyPrefix(nsILoadContextInfo* aInfo, nsACString &_retval)
{
  /**
   * This key is used to salt file hashes.  When form of the key is changed
   * cache entries will fail to find on disk.
   *
   * IMPORTANT NOTE:
   * Keep the attributes list sorted according their ASCII code.
   */

  if (aInfo->IsAnonymous()) {
    _retval.Append(NS_LITERAL_CSTRING("a,"));
  }

  if (aInfo->IsInBrowserElement()) {
    _retval.Append(NS_LITERAL_CSTRING("b,"));
  }

  if (aInfo->AppId() != nsILoadContextInfo::NO_APP_ID) {
    _retval.Append('i');
    _retval.AppendInt(aInfo->AppId());
    _retval.Append(',');
  }

  if (aInfo->IsPrivate()) {
    _retval.Append(NS_LITERAL_CSTRING("p,"));
  }
}

} // CacheFileUtils
} // net
} // mozilla

