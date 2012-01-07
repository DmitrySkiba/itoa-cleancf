/*
 * Copyright (c) 2011 Dmitry Skiba
 * Copyright (c) 2008 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include <CoreFoundation/CFURL.h>
#include <CoreFoundation/CFCharacterSet.h>
#include <CoreFoundation/CFNumber.h>
#include "CFPlatform.h"
#include "CFUniChar.h"
#include "CFInternal.h"
#include "CFStringEncoding.h"
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

//TODO convert all macros to constans & functions
//TODO rename remaining functions
//TODO cleanup / refactoring
//TODO redo STRING_CHAR & constructBuffers - add freeBuffers, make
//     macro to be a function, like buffersChar.
//TODO TESTS!!!

/* __CFURL::_flags layout: [DDd m T FFFF]
 *   DDd  = 'differs' mask, 10 bits
 *   m    = 'match' flags, 2 bits
 *   T    = FS type, 4 bits
 *   FFFF = flags, 16 bits
 */
enum {
    HAS_SCHEME      = 0x0001,
    HAS_USER        = 0x0002,
    HAS_PASSWORD    = 0x0004,
    HAS_HOST        = 0x0008,
    HAS_PORT        = 0x0010,
    HAS_PATH        = 0x0020,
    HAS_PARAMETERS  = 0x0040,
    HAS_QUERY       = 0x0080,
    HAS_FRAGMENT    = 0x0100,
    HAS_HTTP_SCHEME = 0x0200,
    IS_IPV6_ENCODED = 0x0400,
    IS_OLD_UTF8_STYLE = 0x0800,
    IS_DIRECTORY    = 0x1000,
    IS_PARSED       = 0x2000,
    IS_ABSOLUTE     = 0x4000,
    IS_DECOMPOSABLE = 0x8000,
};

/* POSIX_AND_URL_PATHS_MATCH will only be true if the URL and POSIX paths are
 *  identical, character for character, except for the presence/absence of a 
 *  trailing slash on directories.
 */
#define POSIX_AND_URL_PATHS_MATCH      (0x00100000)
#define ORIGINAL_AND_URL_STRINGS_MATCH (0x00200000)

/* If ORIGINAL_AND_URL_STRINGS_MATCH is false, these bits determine where they differ */
// Scheme can actually never differ because if there were escaped characters prior to 
//  the colon, we'd interpret the string as a relative path.
// #define SCHEME_DIFFERS     (0x00400000)    unused
#define USER_DIFFERS       (0x00800000)
#define PASSWORD_DIFFERS   (0x01000000)
#define HOST_DIFFERS       (0x02000000)
// Port can actually never differ because if there were a non-digit following a colon in 
//  the net location, we'd interpret the whole net location as the host.
#define PORT_DIFFERS       (0x04000000)
// #define PATH_DIFFERS       (0x08000000)    unused
// #define PARAMETERS_DIFFER  (0x10000000)    unused
// #define QUERY_DIFFERS      (0x20000000)    unused
// #define FRAGMENT_DIFfERS   (0x40000000)    unused
#define HAS_FILE_SCHEME        (0x80000000)

// Number of bits to shift to get from HAS_FOO to FOO_DIFFERS flag
#define BIT_SHIFT_FROM_COMPONENT_TO_DIFFERS_FLAG (22)

// Other useful defines
#define NET_LOCATION_MASK (HAS_HOST | HAS_USER | HAS_PASSWORD | HAS_PORT)
#define RESOURCE_SPECIFIER_MASK  (HAS_PARAMETERS | HAS_QUERY | HAS_FRAGMENT)
#define FULL_URL_REPRESENTATION (0xF)

/* URL_PATH_TYPE(anURL) will be one of the CFURLPathStyle constants, 
 *  in which case string is a file system path, or will be FULL_URL_REPRESENTATION, 
 *  in which case the string is the full URL string.
 * One caveat - string always has a trailing path delimiter if the url 
 *  is a directory URL.  This must be stripped before returning file 
 *  system representations!
 */
#define PATH_TYPE_MASK (0x000F0000)
#define URL_PATH_TYPE(url) (((url->_flags) & PATH_TYPE_MASK) >> 16)

#define PATH_DELIM_FOR_TYPE(fsType) \
    ((fsType) == kCFURLHFSPathStyle ? \
        ':' : \
        (((fsType) == kCFURLWindowsPathStyle) ? '\\' : '/'))
#define PATH_DELIM_AS_STRING_FOR_TYPE(fsType) \
    ((fsType) == kCFURLHFSPathStyle ? \
        CFSTR(":") : \
        (((fsType) == kCFURLWindowsPathStyle) ? CFSTR("\\") : CFSTR("/")))

/* In order to get the sizeof ( __CFURL ) < 32 bytes, move these items into a 
 *  seperate structure which is only allocated when necessary.  In my tests, 
 *  it's almost never needed -- very rarely does a CFURL have either a sanitized
 *  string or a reserved pointer for URLHandle.
 */
typedef struct {
    void* _reserved; // Reserved for URLHandle's use.

    // The fully compliant RFC string.
    // This is only non-NULL if ORIGINAL_AND_URL_STRINGS_MATCH is false.
    // This should never be mutated except when the sanatized string is first computed.
    CFMutableStringRef _sanitizedString;
} __CFURLAdditionalData;

typedef struct __CFURL {
    CFRuntimeBase _cfBase;
    UInt32 _flags;
    CFStringRef _string;  // Never NULL; the meaning of _string depends on URL_PATH_TYPE(myURL) (see above)
    CFURLRef _base;
    CFRange* ranges;
    CFStringEncoding _encoding; // The encoding to use when asked to remove percent escapes; this is never consulted if IS_OLD_UTF8_STYLE is set.
    __CFURLAdditionalData* extra;
} __CFURL;

typedef enum __CFURLCharacterType {
    VALID = 1,
    UNRESERVED = 2, // unused
    PATHVALID = 4,
    SCHEME = 8,
    HEXDIGIT = 16
} __CFURLCharacterType;

static const unsigned char __CFURLValidCharacters[] = {
    /* ' '  32 */ 0,
    /* '!'  33 */ VALID | UNRESERVED | PATHVALID,
    /* '"'  34 */ 0,
    /* '#'  35 */ 0,
    /* '$'  36 */ VALID | PATHVALID,
    /* '%'  37 */ 0,
    /* '&'  38 */ VALID | PATHVALID,
    /* '''  39 */ VALID | UNRESERVED | PATHVALID,
    /* '('  40 */ VALID | UNRESERVED | PATHVALID,
    /* ')'  41 */ VALID | UNRESERVED | PATHVALID,
    /* '*'  42 */ VALID | UNRESERVED | PATHVALID,
    /* '+'  43 */ VALID | SCHEME | PATHVALID,
    /* ','  44 */ VALID | PATHVALID,
    /* '-'  45 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* '.'  46 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* '/'  47 */ VALID | PATHVALID,
    /* '0'  48 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* '1'  49 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* '2'  50 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* '3'  51 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* '4'  52 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* '5'  53 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* '6'  54 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* '7'  55 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* '8'  56 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* '9'  57 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* ':'  58 */ VALID,
    /* ';'  59 */ VALID,
    /* '<'  60 */ 0,
    /* '='  61 */ VALID | PATHVALID,
    /* '>'  62 */ 0,
    /* '?'  63 */ VALID,
    /* '@'  64 */ VALID | PATHVALID,
    /* 'A'  65 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* 'B'  66 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* 'C'  67 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* 'D'  68 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* 'E'  69 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* 'F'  70 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* 'G'  71 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'H'  72 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'I'  73 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'J'  74 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'K'  75 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'L'  76 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'M'  77 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'N'  78 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'O'  79 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'P'  80 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'Q'  81 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'R'  82 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'S'  83 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'T'  84 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'U'  85 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'V'  86 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'W'  87 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'X'  88 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'Y'  89 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'Z'  90 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* '['  91 */ 0,
    /* '\'  92 */ 0,
    /* ']'  93 */ 0,
    /* '^'  94 */ 0,
    /* '_'  95 */ VALID | UNRESERVED | PATHVALID,
    /* '`'  96 */ 0,
    /* 'a'  97 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* 'b'  98 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* 'c'  99 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* 'd' 100 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* 'e' 101 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* 'f' 102 */ VALID | UNRESERVED | SCHEME | PATHVALID | HEXDIGIT,
    /* 'g' 103 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'h' 104 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'i' 105 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'j' 106 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'k' 107 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'l' 108 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'm' 109 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'n' 110 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'o' 111 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'p' 112 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'q' 113 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'r' 114 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 's' 115 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 't' 116 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'u' 117 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'v' 118 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'w' 119 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'x' 120 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'y' 121 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* 'z' 122 */ VALID | UNRESERVED | SCHEME | PATHVALID,
    /* '{' 123 */ 0,
    /* '|' 124 */ 0,
    /* '}' 125 */ 0,
    /* '~' 126 */ VALID | UNRESERVED | PATHVALID,
    /* '' 127 */ 0
};

static CFTypeID __kCFURLTypeID = _kCFRuntimeNotATypeID;

typedef struct __CFURLEncodingTranslationParameters {
    CFStringEncoding fromEnc;
    CFStringEncoding toEnc;
    const UniChar* addlChars;
    int count;
    Boolean escapeHighBit;
    Boolean escapePercents;
    Boolean agreesOverASCII;
    Boolean encodingsMatch;
} __CFURLEncodingTranslationParameters;

// The NSURL methods do not deal with escaping escape characters at all;
//  therefore, in order to properly bridge NSURL methods, and still provide
//  the escaping behavior that we want, we need to create functions that match
//  the ObjC behavior exactly, and have the public CFURL... functions call these.

CONST_STRING_DECL(kCFURLFileScheme, "file")
CONST_STRING_DECL(kCFURLDataScheme, "data")
CONST_STRING_DECL(kCFURLHTTPScheme, "http")
CONST_STRING_DECL(kCFURLLocalhost, "localhost")

#define STRING_CHAR(x) (useCString ? cstring[(x)] : ustring[(x)])

///////////////////////////////////////////////////////////////////// private

CF_INLINE Boolean createOldUTF8StyleURLs_always_false(void) {
    return false;
}

CF_INLINE Boolean __CFURLCheckCharacter(UniChar ch, __CFURLCharacterType type) {
    return (ch >= 32 && ch <= 127) && (__CFURLValidCharacters[ch - 32] & type);
}

CF_INLINE Boolean isURLLegalCharacter(UniChar ch) {
    return ( ( 32 <= ch ) && ( ch <= 127 ) ) ? ( __CFURLValidCharacters[ ch - 32 ] & VALID ) : false;
}

CF_INLINE Boolean scheme_valid(UniChar ch) {
    return ( ( 32 <= ch ) && ( ch <= 127 ) ) ? ( __CFURLValidCharacters[ ch - 32 ] & SCHEME ) : false;
}

//// "Unreserved" as defined by RFC 2396
//CF_INLINE Boolean isUnreservedCharacter(UniChar ch) {
//    return ( ( 32 <= ch ) && ( ch <= 127 ) ) ? ( __CFURLValidCharacters[ ch - 32 ] & UNRESERVED ) : false;
//}

CF_INLINE Boolean isPathLegalCharacter(UniChar ch) {
    return ( ( 32 <= ch ) && ( ch <= 127 ) ) ? ( __CFURLValidCharacters[ ch - 32 ] & PATHVALID ) : false;
}

CF_INLINE Boolean isHexDigit(UniChar ch) {
    return ( ( 32 <= ch ) && ( ch <= 127 ) ) ? ( __CFURLValidCharacters[ ch - 32 ] & HEXDIGIT ) : false;
}

/************/

CF_INLINE __CFURL* __CFWriteableURL(CFURLRef url) {
    return (__CFURL*)url;
}

CF_INLINE void* __CFURLGetReserved(CFURLRef url) {
    if (url && url->extra) {
        return url->extra->_reserved;
    }
    return NULL;
}

CF_INLINE CFMutableStringRef __CFURLGetSanitizedString(CFURLRef url) {
    if (url && url->extra) {
        return url->extra->_sanitizedString;
    }
    return NULL;
}

static void __CFURLAllocateExtraDataspace(__CFURL* url) {
    if (url && !url->extra) {
        __CFURLAdditionalData* extra = (__CFURLAdditionalData*)CFAllocatorAllocate(
            CFGetAllocator(url),
            sizeof(__CFURLAdditionalData),
            0);
        extra->_reserved = __CFURLGetReserved(url);
        extra->_sanitizedString = __CFURLGetSanitizedString(url);

        url->extra = extra;
    }
}

static void __CFURLSetReserved(__CFURL* url, void* reserved) {
    if (url) {
        // Don't allocate extra space if we're just going to be storing NULL
        if (!url->extra && reserved) {
            __CFURLAllocateExtraDataspace(url);
        }
        if (url->extra) {
            url->extra->_reserved = reserved;
        }
    }
}

CF_INLINE void __CFURLSetSanitizedString(__CFURL* url, CFMutableStringRef sanitizedString) {
    if (url) {
        // Don't allocate extra space if we're just going to be storing NULL
        if (!url->extra && sanitizedString) {
            __CFURLAllocateExtraDataspace(url);
        }
        if (url->extra) {
            url->extra->_sanitizedString = sanitizedString;
        }
    }
}

/* Converts two hex digits to byte.
 * Returns false if ch1 or ch2 isn't properly formatted.
 */
CF_INLINE Boolean __CFMakeByte(UniChar ch1, UniChar ch2, uint8_t* result) {
    *result = 0;
    if (ch1 >= '0' && ch1 <= '9') {
        *result += (ch1 - '0');
    } else if (ch1 >= 'a' && ch1 <= 'f') {
        *result += 10 + ch1 - 'a';
    } else if (ch1 >= 'A' && ch1 <= 'F') {
        *result += 10 + ch1 - 'A';
    } else {
        return false;
    }

    *result  = (*result) << 4;
    if (ch2 >= '0' && ch2 <= '9') {
        *result += (ch2 - '0');
    } else if (ch2 >= 'a' && ch2 <= 'f') {
        *result += 10 + ch2 - 'a';
    } else if (ch2 >= 'A' && ch2 <= 'F') {
        *result += 10 + ch2 - 'A';
    } else {
        return false;
    }

    return true;
}

// Assumes the URL is already parsed
static CFRange __CFURLGetComponentRange(UInt32 flags, const CFRange* ranges, UInt32 componentFlag) {
    UInt32 idx = 0;
    if (!(flags & componentFlag)) {
        return CFRangeMake(kCFNotFound, 0);
    }
    while (!(componentFlag & 1)) {
        componentFlag = componentFlag >> 1;
        if (flags & 1) {
            idx++;
        }
        flags = flags >> 1;
    }
    return ranges[idx];
}

static CFRange __CFURLGetNetLocationRange(UInt32 flags, const CFRange* ranges) {
    CFRange netRgs[4];
    CFRange netRg = {kCFNotFound, 0};
    CFIndex i, c = 4;

    if (!(flags & NET_LOCATION_MASK)) {
        return CFRangeMake(kCFNotFound, 0);
    }

    netRgs[0] = __CFURLGetComponentRange(flags, ranges, HAS_USER);
    netRgs[1] = __CFURLGetComponentRange(flags, ranges, HAS_PASSWORD);
    netRgs[2] = __CFURLGetComponentRange(flags, ranges, HAS_HOST);
    netRgs[3] = __CFURLGetComponentRange(flags, ranges, HAS_PORT);
    for (i = 0; i < c; i++) {
        if (netRgs[i].location == kCFNotFound) {
            continue;
        }
        if (netRg.location == kCFNotFound) {
            netRg = netRgs[i];
        } else {
            netRg.length = netRgs[i].location + netRgs[i].length - netRg.location;
        }
    }
    return netRg;
}

static UInt32 __CFURLGetFirstResourceSpecifierFlag(UInt32 flags) {
    UInt32 firstRsrcSpecFlag = 0;
    UInt32 flag = HAS_FRAGMENT;
    while (flag != HAS_PATH) {
        if (flags & flag) {
            firstRsrcSpecFlag = flag;
        }
        flag = flag >> 1;
    }
    return firstRsrcSpecFlag;
}


/* Returns NULL if str cannot be converted for whatever reason, 
 *  str if str contains no characters in need of escaping, 
 *  or a newly-created string with the appropriate % escape codes in place.
 *  Caller must always release the returned string.
 */
CF_INLINE CFStringRef __CFReplacePathIllegalCharacters(CFStringRef path,
                                                       CFAllocatorRef alloc,
                                                       Boolean preserveSlashes)
{
    return CFURLCreateStringByAddingPercentEscapes(
        alloc,
        path,
        NULL,
        preserveSlashes ? CFSTR(";?") : CFSTR(";?/"),
        kCFStringEncodingUTF8);
}

/* We have 2 UniChars of a surrogate; we must convert to the correct percent-encoded UTF8 string and append to str.
 * Added so that file system URLs can always be converted from POSIX to full URL representation.  -- REW, 8/20/2001
 */
static Boolean _hackToConvertSurrogates(UniChar highChar, UniChar lowChar, CFMutableStringRef str) {
    UniChar surrogate[2];
    uint8_t bytes[6]; // Aki sez it should never take more than 6 bytes
    CFIndex len;
    uint8_t* currByte;
    surrogate[0] = highChar;
    surrogate[1] = lowChar;
    if (CFStringEncodingUnicodeToBytes(kCFStringEncodingUTF8, 0, surrogate, 2, NULL, bytes, 6, &len) != kCFStringEncodingConversionSuccess) {
        return false;
    }
    for (currByte = bytes; currByte < bytes + len; currByte++) {
        UniChar escapeSequence[3] = {'%', '\0', '\0'};
        unsigned char high, low;
        high = ((*currByte) & 0xf0) >> 4;
        low = (*currByte) & 0x0f;
        escapeSequence[1] = (high < 10) ? '0' + high : 'A' + high - 10;
        escapeSequence[2] = (low < 10) ? '0' + low : 'A' + low - 10;
        CFStringAppendCharacters(str, escapeSequence, 3);
    }
    return true;
}

static Boolean _appendPercentEscapesForCharacter(UniChar ch, CFStringEncoding encoding, CFMutableStringRef str) {
    uint8_t bytes[6]; // 6 bytes is the maximum a single character could require in UTF8 (most common case); other encodings could require more
    uint8_t* bytePtr = bytes, * currByte;
    CFIndex byteLength;
    CFAllocatorRef alloc = NULL;
    if (CFStringEncodingUnicodeToBytes(encoding, 0, &ch, 1, NULL, bytePtr, 6, &byteLength) != kCFStringEncodingConversionSuccess) {
        byteLength = CFStringEncodingByteLengthForCharacters(encoding, 0, &ch, 1);
        if (byteLength <= 6) {
            // The encoding cannot accomodate the character
            return false;
        }
        alloc = CFGetAllocator(str);
        bytePtr = (uint8_t*)CFAllocatorAllocate(alloc, byteLength, 0);
        if (!bytePtr || CFStringEncodingUnicodeToBytes(encoding, 0, &ch, 1, NULL, bytePtr, byteLength, &byteLength) != kCFStringEncodingConversionSuccess) {
            if (bytePtr) {
                CFAllocatorDeallocate(alloc, bytePtr);
            }
            return false;
        }
    }
    for (currByte = bytePtr; currByte < bytePtr + byteLength; currByte++) {
        UniChar escapeSequence[3] = {'%', '\0', '\0'};
        unsigned char high, low;
        high = ((*currByte) & 0xf0) >> 4;
        low = (*currByte) & 0x0f;
        escapeSequence[1] = (high < 10) ? '0' + high : 'A' + high - 10;
        escapeSequence[2] = (low < 10) ? '0' + low : 'A' + low - 10;
        CFStringAppendCharacters(str, escapeSequence, 3);
    }
    if (bytePtr != bytes) {
        CFAllocatorDeallocate(alloc, bytePtr);
    }
    return true;
}

static CFStringRef _addPercentEscapesToString(CFAllocatorRef allocator, CFStringRef originalString, Boolean (* shouldReplaceChar)(UniChar, void*), CFIndex (* handlePercentChar)(CFIndex, CFStringRef, CFStringRef*, void*), CFStringEncoding encoding, void* context) {
    CFMutableStringRef newString = NULL;
    CFIndex idx, length;
    CFStringInlineBuffer buf;

    if (!originalString) {
        return NULL;
    }
    length = CFStringGetLength(originalString);
    if (length == 0) {
        return CFStringCreateCopy(allocator, originalString);
    }
    CFStringInitInlineBuffer(originalString, &buf, CFRangeMake(0, length));

    for (idx = 0; idx < length; idx++) {
        UniChar ch = CFStringGetCharacterFromInlineBuffer(&buf, idx);
        Boolean shouldReplace = shouldReplaceChar(ch, context);
        if (shouldReplace) {
            // Perform the replacement
            if (!newString) {
                newString = CFStringCreateMutableCopy(CFGetAllocator(originalString), 0, originalString);
                CFStringDelete(newString, CFRangeMake(idx, length - idx));
            }
            if (!_appendPercentEscapesForCharacter(ch, encoding, newString)) {
                if (encoding == kCFStringEncodingUTF8 &&
                    _CFUniCharIsSurrogateHighCharacter(ch) &&
                    idx + 1 < length &&
                    _CFUniCharIsSurrogateLowCharacter(CFStringGetCharacterFromInlineBuffer(&buf, idx + 1)))
                {
                    // Hack to guarantee we always safely convert file URLs between POSIX & full URL representation
                    if (_hackToConvertSurrogates(ch, CFStringGetCharacterFromInlineBuffer(&buf, idx + 1), newString)) {
                        idx++;  // We consumed 2 characters, not 1
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }
        } else if (ch == '%' && handlePercentChar) {
            CFStringRef replacementString = NULL;
            CFIndex newIndex = handlePercentChar(idx, originalString, &replacementString, context);
            if (newIndex < 0) {
                break;
            } else if (replacementString) {
                if (!newString) {
                    newString = CFStringCreateMutableCopy(CFGetAllocator(originalString), 0, originalString);
                    CFStringDelete(newString, CFRangeMake(idx, length - idx));
                }
                CFStringAppend(newString, replacementString);
                CFRelease(replacementString);
            }
            if (newIndex == idx) {
                if (newString) {
                    CFStringAppendCharacters(newString, &ch, 1);
                }
            } else {
                if (!replacementString && newString) {
                    CFIndex tmpIndex;
                    for (tmpIndex = idx; tmpIndex < newIndex; tmpIndex++) {
                        ch = CFStringGetCharacterAtIndex(originalString, idx);
                        CFStringAppendCharacters(newString, &ch, 1);
                    }
                }
                idx = newIndex - 1;
            }
        } else if (newString) {
            CFStringAppendCharacters(newString, &ch, 1);
        }
    }
    if (idx < length) {
        // Ran in to an encoding failure
        if (newString) {
            CFRelease(newString);
        }
        return NULL;
    } else if (newString) {
        return newString;
    } else {
        return CFStringCreateCopy(CFGetAllocator(originalString), originalString);
    }
}

static Boolean _stringContainsCharacter(CFStringRef string, UniChar ch) {
    CFIndex i, c = CFStringGetLength(string);
    CFStringInlineBuffer buf;
    CFStringInitInlineBuffer(string, &buf, CFRangeMake(0, c));
    for (i = 0; i < c; i++) {
        if (CFStringGetCharacterFromInlineBuffer(&buf, i) == ch) {
            return true;
        }
    }
    return false;
}

static Boolean _shouldPercentReplaceChar(UniChar ch, void* context) {
    CFStringRef unescape = ((CFStringRef*)context)[0];
    CFStringRef escape = ((CFStringRef*)context)[1];
    Boolean shouldReplace = (isURLLegalCharacter(ch) == false);
    if (shouldReplace) {
        if (unescape && _stringContainsCharacter(unescape, ch)) {
            shouldReplace = false;
        }
    } else if (escape && _stringContainsCharacter(escape, ch)) {
        shouldReplace = true;
    }
    return shouldReplace;
}


/* Toll-free bridging support; get the true CFURL from an NSURL */
CF_INLINE CFURLRef _CFURLFromNSURL(CFURLRef url) {
    CF_OBJC_FUNCDISPATCH(CFURLRef, url, "_cfurl");
    return url;
}

static void constructBuffers(CFAllocatorRef alloc, CFStringRef string, const char** cstring, const UniChar** ustring, Boolean* useCString, Boolean* freeCharacters) {
    CFIndex neededLength;
    CFIndex length;
    CFRange rg;

    *cstring = CFStringGetCStringPtr(string, kCFStringEncodingISOLatin1);
    if (*cstring) {
        *ustring = NULL;
        *useCString = true;
        *freeCharacters = false;
        return;
    }

    *ustring = CFStringGetCharactersPtr(string);
    if (*ustring) {
        *useCString = false;
        *freeCharacters = false;
        return;
    }

    *freeCharacters = true;
    length = CFStringGetLength(string);
    rg = CFRangeMake(0, length);
    CFStringGetBytes(string, rg, kCFStringEncodingISOLatin1, 0, false, NULL, INT_MAX, &neededLength);
    if (neededLength == length) {
        char* buf = (char*)CFAllocatorAllocate(alloc, length, 0);
        CFStringGetBytes(string, rg, kCFStringEncodingISOLatin1, 0, false, (uint8_t*)buf, length, NULL);
        *cstring = buf;
        *useCString = true;
    } else {
        UniChar* buf = (UniChar*)CFAllocatorAllocate(alloc, length * sizeof(UniChar), 0);
        CFStringGetCharacters(string, rg, buf);
        *useCString = false;
        *ustring = buf;
    }
}

static void __CFParseURLComponents(CFAllocatorRef alloc,
                                   CFStringRef string,
                                   CFURLRef baseURL,
                                   UInt32* theFlags,
                                   CFRange** range)
{
    CFRange ranges[9];
    /* index gives the URL part involved; to calculate the correct range index, 
     * use the number of the bit of the equivalent flag (i.e. the host flag is 
     * HAS_HOST, which is 0x8.  so the range index for the host is 3.)  Note 
     * that this is true in this function ONLY, since the ranges stored in 
     * (*range) are actually packed, skipping those URL components that don't 
     * exist.  This is why the indices are hard-coded in this function.
     */

    CFIndex idx, base_idx = 0;
    CFIndex string_length;
    UInt32 flags = (IS_PARSED | *theFlags);
    Boolean useCString, freeCharacters, isCompliant;
    uint8_t numRanges = 0;
    const char* cstring = NULL;
    const UniChar* ustring = NULL;

    string_length = CFStringGetLength(string);
    constructBuffers(alloc, string, &cstring, &ustring, &useCString, &freeCharacters);

    // Algorithm is as described in RFC 1808
    // 1: parse the fragment; remainder after left-most "#" is fragment
    for (idx = base_idx; idx < string_length; idx++) {
        if ('#' == STRING_CHAR(idx)) {
            flags |= HAS_FRAGMENT;
            ranges[8].location = idx + 1;
            ranges[8].length = string_length - (idx + 1);
            numRanges++;
            string_length = idx;    // remove fragment from parse string
            break;
        }
    }
    // 2: parse the scheme
    for (idx = base_idx; idx < string_length; idx++) {
        UniChar ch = STRING_CHAR(idx);
        if (':' == ch) {
            flags |= HAS_SCHEME;
            flags |= IS_ABSOLUTE;
            ranges[0].location = base_idx;
            ranges[0].length = idx;
            numRanges++;
            base_idx = idx + 1;
            // optimization for http urls
            if (idx == 4 && STRING_CHAR(0) == 'h' && STRING_CHAR(1) == 't' &&
                STRING_CHAR(2) == 't' && STRING_CHAR(3) == 'p')
            {
                flags |= HAS_HTTP_SCHEME;
            }
            // optimization for file urls
            if (idx == 4 && STRING_CHAR(0) == 'f' && STRING_CHAR(1) == 'i' &&
                STRING_CHAR(2) == 'l' && STRING_CHAR(3) == 'e')
            {
                flags |= HAS_FILE_SCHEME;
            }
            break;
        } else if (!scheme_valid(ch)) {
            break;    // invalid scheme character -- no scheme
        }
    }

    // Make sure we have an RFC-1808 compliant URL -  that's either
    //  something without a scheme, or scheme:/(stuff) or scheme://(stuff).
    // Strictly speaking, RFC 1808 & 2396 bar "scheme:" (with nothing 
    //  following the colon); however, common usage expects this to be 
    //  treated identically to "scheme://" - REW, 12/08/03
    if (!(flags & HAS_SCHEME)) {
        isCompliant = true;
    } else if (base_idx == string_length) {
        isCompliant = false;
    } else if (STRING_CHAR(base_idx) != '/') {
        isCompliant = false;
    } else {
        isCompliant = true;
    }

    if (!isCompliant) {
        // Clear the fragment flag if it's been set
        if (flags & HAS_FRAGMENT) {
            flags &= (~HAS_FRAGMENT);
            string_length = CFStringGetLength(string);
        }
        (*theFlags) = flags;
        (*range) = (CFRange*)CFAllocatorAllocate(alloc, sizeof(CFRange), 0);
        (*range)->location = ranges[0].location;
        (*range)->length = ranges[0].length;

        if (freeCharacters) {
            CFAllocatorDeallocate(alloc, useCString ? (void*)cstring : (void*)ustring);
        }
        return;
    }
    // URL is 1808-compliant
    flags |= IS_DECOMPOSABLE;

    // 3: parse the network location and login
    if (2 <= (string_length - base_idx) && '/' == STRING_CHAR(base_idx) && '/' == STRING_CHAR(base_idx + 1)) {
        CFIndex base = 2 + base_idx, extent;
        for (idx = base; idx < string_length; idx++) {
            if ('/' == STRING_CHAR(idx) || '?' == STRING_CHAR(idx)) {
                break;
            }
        }
        extent = idx;

        // net_loc parts extend from base to extent (but not including), which might be to end of string
        // net location is "<user>:<password>@<host>:<port>"
        if (extent != base) {
            for (idx = base; idx < extent; idx++) {
                if ('@' == STRING_CHAR(idx)) {   // there is a user
                    CFIndex idx2;
                    flags |= HAS_USER;
                    numRanges++;
                    ranges[1].location = base;  // base of the user
                    for (idx2 = base; idx2 < idx; idx2++) {
                        if (':' == STRING_CHAR(idx2)) {    // found a password separator
                            flags |= HAS_PASSWORD;
                            numRanges++;
                            ranges[2].location = idx2 + 1; // base of the password
                            ranges[2].length = idx - (idx2 + 1);  // password extent
                            ranges[1].length = idx2 - base; // user extent
                            break;
                        }
                    }
                    if (!(flags & HAS_PASSWORD)) {
                        // user extends to the '@'
                        ranges[1].length = idx - base; // user extent
                    }
                    base = idx + 1;
                    break;
                }
            }
            flags |= HAS_HOST;
            numRanges++;
            ranges[3].location = base; // base of host

            // base has been advanced past the user and password if they existed
            for (idx = base; idx < extent; idx++) {
                // IPV6 support (RFC 2732) DCJ June/10/2002
                if ('[' == STRING_CHAR(idx)) {    // starting IPV6 explicit address
                    //    Find the ']' terminator of the IPv6 address, leave idx pointing to ']' or end
                    for (; idx < extent; ++idx) {
                        if (']' == STRING_CHAR(idx)) {
                            flags |= IS_IPV6_ENCODED;
                            break;
                        }
                    }
                }
                // there is a port if we see a colon.  Only the last one is the port, though.
                else if (':' == STRING_CHAR(idx)) {
                    flags |= HAS_PORT;
                    numRanges++;
                    ranges[4].location = idx + 1; // base of port
                    ranges[4].length = extent - (idx + 1); // port extent
                    ranges[3].length = idx - base; // host extent
                    break;
                }
            }
            if (!(flags & HAS_PORT)) {
                ranges[3].length = extent - base;  // host extent
            }
        }
        base_idx = extent;
    }

    // 4: parse the query; remainder after left-most "?" is query
    for (idx = base_idx; idx < string_length; idx++) {
        if ('?' == STRING_CHAR(idx)) {
            flags |= HAS_QUERY;
            numRanges++;
            ranges[7].location = idx + 1;
            ranges[7].length = string_length - (idx + 1);
            string_length = idx;    // remove query from parse string
            break;
        }
    }

    // 5: parse the parameters; remainder after left-most ";" is parameters
    for (idx = base_idx; idx < string_length; idx++) {
        if (';' == STRING_CHAR(idx)) {
            flags |= HAS_PARAMETERS;
            numRanges++;
            ranges[6].location = idx + 1;
            ranges[6].length = string_length - (idx + 1);
            string_length = idx;    // remove parameters from parse string
            break;
        }
    }

    // 6: parse the path; it's whatever's left between string_length & base_idx
    if (string_length - base_idx != 0 || (flags & NET_LOCATION_MASK)) {
        // If we have a net location, we are 1808-compliant, and an empty path substring implies a path of "/"
        UniChar ch;
        Boolean isDir;
        CFRange pathRg;
        flags |= HAS_PATH;
        numRanges++;
        pathRg.location = base_idx;
        pathRg.length = string_length - base_idx;
        ranges[5] = pathRg;

        if (pathRg.length > 0) {
            Boolean sawPercent = FALSE;
            for (idx = pathRg.location; idx < string_length; idx++) {
                if ('%' == STRING_CHAR(idx)) {
                    sawPercent = TRUE;
                    break;
                }
            }
            if (!sawPercent) {
                flags |= POSIX_AND_URL_PATHS_MATCH;
            }

            ch = STRING_CHAR(pathRg.location + pathRg.length - 1);
            if (ch == '/') {
                isDir = true;
            } else if (ch == '.') {
                if (pathRg.length == 1) {
                    isDir = true;
                } else {
                    ch = STRING_CHAR(pathRg.location + pathRg.length - 2);
                    if (ch == '/') {
                        isDir = true;
                    } else if (ch != '.') {
                        isDir = false;
                    } else if (pathRg.length == 2) {
                        isDir = true;
                    } else {
                        isDir = (STRING_CHAR(pathRg.location + pathRg.length - 3) == '/');
                    }
                }
            } else {
                isDir = false;
            }
        } else {
            isDir = (baseURL != NULL) ? CFURLHasDirectoryPath(baseURL) : false;
        }
        if (isDir) {
            flags |= IS_DIRECTORY;
        }
    }

    if (freeCharacters) {
        CFAllocatorDeallocate(alloc, useCString ? (void*)cstring : (void*)ustring);
    }
    (*theFlags) = flags;
    (*range) = (CFRange*)CFAllocatorAllocate(alloc, sizeof(CFRange) * numRanges, 0);
    numRanges = 0;
    for (idx = 0, flags = 1; flags != (1 << 9); flags = (flags << 1), idx++) {
        if ((*theFlags) & flags) {
            (*range)[numRanges] = ranges[idx];
            numRanges++;
        }
    }
}

static void __CFURLEnsureComponentsParsed(CFURLRef roURL) {
    if (roURL->_flags & IS_PARSED) {
        return;
    }
    __CFURL* url = __CFWriteableURL(roURL);
    __CFParseURLComponents(CFGetAllocator(url), url->_string, url->_base, &url->_flags, &url->ranges);
}

static Boolean scanCharacters(CFAllocatorRef alloc, CFMutableStringRef* escapedString, UInt32* flags, const char* cstring, const UniChar* ustring, Boolean useCString, CFIndex base, CFIndex end, CFIndex* mark, UInt32 componentFlag, CFStringEncoding encoding) {
    CFIndex idx;
    Boolean sawIllegalChar = false;
    for (idx = base; idx < end; idx++) {
        Boolean shouldEscape;
        UniChar ch = STRING_CHAR(idx);
        if (isURLLegalCharacter(ch)) {
            if ((componentFlag == HAS_USER || componentFlag == HAS_PASSWORD) && (ch == '/' || ch == '?' || ch == '@')) {
                shouldEscape = true;
            } else {
                shouldEscape = false;
            }
        } else if (ch == '%' && idx + 2 < end && isHexDigit(STRING_CHAR(idx + 1)) && isHexDigit(STRING_CHAR(idx + 2))) {
            shouldEscape = false;
        } else if (componentFlag == HAS_HOST && ((idx == base && ch == '[') || (idx == end - 1 && ch == ']'))) {
            shouldEscape = false;
        } else {
            shouldEscape = true;
        }
        if (!shouldEscape) {
            continue;
        }

        sawIllegalChar = true;
        if (componentFlag && flags) {
            *flags |= (componentFlag << BIT_SHIFT_FROM_COMPONENT_TO_DIFFERS_FLAG);
        }
        if (!*escapedString) {
            *escapedString = CFStringCreateMutable(alloc, 0);
        }
        if (useCString) {
            CFStringRef tempString = CFStringCreateWithBytes(alloc, (uint8_t*)&(cstring[*mark]), idx - *mark, kCFStringEncodingISOLatin1, false);
            CFStringAppend(*escapedString, tempString);
            CFRelease(tempString);
        } else {
            CFStringAppendCharacters(*escapedString, &(ustring[*mark]), idx - *mark);
        }
        *mark = idx + 1;
        _appendPercentEscapesForCharacter(ch, encoding, *escapedString); // This can never fail because anURL->_string was constructed from the encoding passed in
    }
    return sawIllegalChar;
}

static void __CFURLComputeSanitizedString(__CFURL* url) {
    CFAllocatorRef alloc = CFGetAllocator(url);
    CFIndex string_length = CFStringGetLength(url->_string);
    Boolean useCString, freeCharacters;
    const char* cstring = NULL;
    const UniChar* ustring = NULL;
    CFIndex base; // where to scan from
    CFIndex mark; // first character not-yet copied to sanitized string

    __CFURLEnsureComponentsParsed(url);
    constructBuffers(alloc, url->_string, &cstring, &ustring, &useCString, &freeCharacters);
    if (!(url->_flags & IS_DECOMPOSABLE)) {
        // Impossible to have a problem character in the scheme
        CFMutableStringRef sanitizedString = NULL;
        base = __CFURLGetComponentRange(url->_flags, url->ranges, HAS_SCHEME).length + 1;
        mark = 0;
        if (!scanCharacters(alloc, &sanitizedString, &url->_flags, cstring, ustring, useCString, base, string_length, &mark, 0, url->_encoding)) {
            url->_flags |= ORIGINAL_AND_URL_STRINGS_MATCH;
        }
        if (sanitizedString) {
            __CFURLSetSanitizedString(url, sanitizedString);
        }
    } else {
        // Go component by component
        CFIndex currentComponent = HAS_USER;
        CFMutableStringRef sanitizedString = NULL;
        mark = 0;
        while (currentComponent < (HAS_FRAGMENT << 1)) {
            CFRange componentRange = __CFURLGetComponentRange(url->_flags, url->ranges, currentComponent);
            if (componentRange.location != kCFNotFound) {
                scanCharacters(alloc, &sanitizedString, &(url->_flags), cstring, ustring, useCString, componentRange.location, componentRange.location + componentRange.length, &mark, currentComponent, url->_encoding);
            }
            currentComponent = currentComponent << 1;
        }
        if (sanitizedString) {
            __CFURLSetSanitizedString(url, sanitizedString);
        } else {
            url->_flags |= ORIGINAL_AND_URL_STRINGS_MATCH;
        }
    }
    if (__CFURLGetSanitizedString(url) && mark != string_length) {
        if (useCString) {
            CFStringRef tempString = CFStringCreateWithBytes(alloc, (uint8_t*)&(cstring[mark]), string_length - mark, kCFStringEncodingISOLatin1, false);
            CFStringAppend(__CFURLGetSanitizedString(url), tempString);
            CFRelease(tempString);
        } else {
            CFStringAppendCharacters(__CFURLGetSanitizedString(url), &(ustring[mark]), string_length - mark);
        }
    }
    if (freeCharacters) {
        CFAllocatorDeallocate(alloc, useCString ? (void*)cstring : (void*)ustring);
    }
}

static void __CFURLEnsureSanitizedString(CFURLRef roURL) {
   if ((roURL->_flags & ORIGINAL_AND_URL_STRINGS_MATCH) ||
        __CFURLGetSanitizedString(roURL))
    {
        return;
    }
    __CFURLComputeSanitizedString(__CFWriteableURL(roURL));
}


static CFStringRef correctedComponent(CFStringRef comp, UInt32 compFlag, CFStringEncoding enc) {
    CFAllocatorRef alloc = CFGetAllocator(comp);
    CFIndex string_length = CFStringGetLength(comp);
    Boolean useCString, freeCharacters;
    const char* cstring = NULL;
    const UniChar* ustring = NULL;
    CFIndex mark = 0; // first character not-yet copied to sanitized string
    CFMutableStringRef result = NULL;

    constructBuffers(alloc, comp, &cstring, &ustring, &useCString, &freeCharacters);
    scanCharacters(alloc, &result, NULL, cstring, ustring, useCString, 0, string_length, &mark, compFlag, enc);
    if (result) {
        if (mark < string_length) {
            if (useCString) {
                CFStringRef tempString = CFStringCreateWithBytes(alloc, (uint8_t*)&(cstring[mark]), string_length - mark, kCFStringEncodingISOLatin1, false);
                CFStringAppend(result, tempString);
                CFRelease(tempString);
            } else {
                CFStringAppendCharacters(result, &(ustring[mark]), string_length - mark);
            }
        }
    } else {
        // This should never happen
        CFRetain(comp);
        result = (CFMutableStringRef)comp;
    }
    if (freeCharacters) {
        CFAllocatorDeallocate(alloc, useCString ? (void*)cstring : (void*)ustring);
    }
    return result;
}

static __CFURL* __CFURLAlloc(CFAllocatorRef allocator) {
    __CFURL* url = (__CFURL*)_CFRuntimeCreateInstance(
        allocator,
        __kCFURLTypeID,
        sizeof(__CFURL) - sizeof(CFRuntimeBase),
        NULL);
    if (url) {
        url->_flags = 0;
        if (createOldUTF8StyleURLs_always_false()) {
            url->_flags |= IS_OLD_UTF8_STYLE;
        }
        url->_string = NULL;
        url->_base = NULL;
        url->ranges = NULL;
        url->_encoding = kCFStringEncodingUTF8;
        url->extra = NULL;
    }
    return url;
}

/* It is the caller's responsibility to guarantee that if URLString is absolute, 
 *  base is NULL.  This is necessary to avoid duplicate processing for file 
 *  system URLs, which had to decide whether to compute the cwd for the base;
 *  we don't want to duplicate that work.  This ALSO means it's the caller's 
 *  responsibility to set the IS_ABSOLUTE bit, since we may have a degenerate 
 *  URL whose string is relative, but lacks a base.
 */
static void __CFURLInit(__CFURL* url, CFStringRef URLString, UInt32 fsType, CFURLRef base) {
    CF_VALIDATE_ARG(
        URLString && CFStringGetLength(URLString) != 0,
        "URLString must not be NULL or empty string");
    CF_VALIDATE_ARG(
        (fsType == FULL_URL_REPRESENTATION) ||
        (fsType == kCFURLPOSIXPathStyle) ||
        (fsType == kCFURLWindowsPathStyle) ||
        (fsType == kCFURLHFSPathStyle),
        "invalid fsType %d", fsType);

    // Coming in, the url has its allocator flag properly set,
    //  and its base initialized, and nothing else.
    url->_string = CFStringCreateCopy(CFGetAllocator(url), URLString);
    url->_flags |= (fsType << 16);
    url->_base = base ? CFURLCopyAbsoluteURL(base) : NULL;
}

static Boolean _CFStringIsLegalURLString(CFStringRef string) {
    // Check each character to make sure it is a legal URL char.  The valid characters are 'A'-'Z', 'a' - 'z', '0' - '9', plus the characters in "-_.!~*'()", and the set of reserved characters (these characters have special meanings in the URL syntax), which are ";/?:@&=+$,".  In addition, percent escape sequences '%' hex-digit hex-digit are permitted.
    // Plus the hash character '#' which denotes the beginning of a fragment, and can appear exactly once in the entire URL string. -- REW, 12/13/2000
    CFStringInlineBuffer stringBuffer;
    CFIndex idx = 0, length;
    Boolean sawHash = false;
    if (!string) {
        return false;
    }
    length = CFStringGetLength(string);
    CFStringInitInlineBuffer(string, &stringBuffer, CFRangeMake(0, length));
    while (idx < length) {
        UniChar ch = CFStringGetCharacterFromInlineBuffer(&stringBuffer, idx);
        idx++;

        //    Make sure that two valid hex digits follow a '%' character
        if (ch == '%') {
            if (idx + 2 > length) {
                //CF_ASSERT(false, "Detected illegal percent escape sequence at character %d when trying to create a CFURL", idx-1);
                idx = -1;  // To guarantee index < length, and our failure case is triggered
                break;
            }

            ch = CFStringGetCharacterFromInlineBuffer(&stringBuffer, idx);
            idx++;
            if (!isHexDigit(ch)) {
                //CF_ASSERT(false, "Detected illegal percent escape sequence at character %d when trying to create a CFURL", idx-2);
                idx = -1;
                break;
            }
            ch = CFStringGetCharacterFromInlineBuffer(&stringBuffer, idx);
            idx++;
            if (!isHexDigit(ch)) {
                //CF_ASSERT(false, "Detected illegal percent escape sequence at character %d when trying to create a CFURL", idx-3);
                idx = -1;
                break;
            }

            continue;
        }
        if (ch == '[' || ch == ']') {
            continue; // IPV6 support (RFC 2732) DCJ June/10/2002
        }
        if (ch == '#') {
            if (sawHash) {
                break;
            }
            sawHash = true;
            continue;
        }
        if (isURLLegalCharacter(ch)) {
            continue;
        }
        break;
    }
    if (idx < length) {
        return false;
    }
    return true;
}

static void __CFURLInitWithString(__CFURL* url, CFStringRef string, CFURLRef baseURL) {
    Boolean isAbsolute = false;
    CFRange colon = CFStringFind(string, CFSTR(":"), 0);
    if (colon.location != kCFNotFound) {
        isAbsolute = true;
        CFIndex i;
        for (i = 0; i < colon.location; i++) {
            char ch = (char)CFStringGetCharacterAtIndex(string, i);
            if (!scheme_valid(ch)) {
                isAbsolute = false;
                break;
            }
        }
    }
    __CFURLInit(url, string, FULL_URL_REPRESENTATION, isAbsolute ? NULL : baseURL);
    if (isAbsolute) {
        url->_flags |= IS_ABSOLUTE;
    }
}

static Boolean _shouldEscapeForEncodingConversion(UniChar ch, void* context) {
    __CFURLEncodingTranslationParameters* info = (__CFURLEncodingTranslationParameters*)context;
    if (info->escapeHighBit && ch > 0x7F) {
        return true;
    } else if (ch == '%' && info->escapePercents) {
        return true;
    } else if (info->addlChars) {
        const UniChar* escChar = info->addlChars;
        int i;
        for (i = 0; i < info->count; escChar++, i++) {
            if (*escChar == ch) {
                return true;
            }
        }
    }
    return false;
}

static CFIndex _convertEscapeSequence(CFIndex percentIndex, CFStringRef urlString, CFStringRef* newString, void* context) {
    __CFURLEncodingTranslationParameters* info = (__CFURLEncodingTranslationParameters*)context;
    CFMutableDataRef newData;
    Boolean sawNonASCIICharacter = false;
    CFIndex i = percentIndex;
    CFIndex length;
    *newString = NULL;
    if (info->encodingsMatch) {
        // +3 because we want the two characters of the percent 
        //  encoding to not be copied verbatim, as well
        return percentIndex + 3;
    }
    newData = CFDataCreateMutable(CFGetAllocator(urlString), 0);
    length = CFStringGetLength(urlString);

    while (i < length && CFStringGetCharacterAtIndex(urlString, i) == '%') {
        uint8_t byte;
        if (i + 2 >= length || !__CFMakeByte(CFStringGetCharacterAtIndex(urlString, i + 1), CFStringGetCharacterAtIndex(urlString, i + 2), &byte)) {
            CFRelease(newData);
            return -1;
        }
        if (byte > 0x7f) {
            sawNonASCIICharacter = true;
        }
        CFDataAppendBytes(newData, &byte, 1);
        i += 3;
    }
    if (!sawNonASCIICharacter && info->agreesOverASCII) {
        return i;
    } else {
        CFStringRef tmp = CFStringCreateWithBytes(CFGetAllocator(urlString), CFDataGetBytePtr(newData), CFDataGetLength(newData), info->fromEnc, false);
        CFIndex tmpIndex, tmpLen;
        if (!tmp) {
            CFRelease(newData);
            return -1;
        }
        tmpLen = CFStringGetLength(tmp);
        *newString = CFStringCreateMutable(CFGetAllocator(urlString), 0);
        for (tmpIndex = 0; tmpIndex < tmpLen; tmpIndex++) {
            if (!_appendPercentEscapesForCharacter(CFStringGetCharacterAtIndex(tmp, tmpIndex), info->toEnc, (CFMutableStringRef)(*newString))) {
                break;
            }
        }
        CFRelease(tmp);
        CFRelease(newData);
        if (tmpIndex < tmpLen) {
            CFRelease(*newString);
            *newString = NULL;
            return -1;
        } else {
            return i;
        }
    }
}

/* Returned string is retained for the caller; if escapePercents is true, then we do not look for any %-escape encodings in urlString */
static CFStringRef  _convertPercentEscapes(CFStringRef urlString, CFStringEncoding fromEncoding, CFStringEncoding toEncoding, Boolean escapeAllHighBitCharacters, Boolean escapePercents, const UniChar* addlCharsToEscape, int numAddlChars) {
    __CFURLEncodingTranslationParameters context;
    context.fromEnc = fromEncoding;
    context.toEnc = toEncoding;
    context.addlChars = addlCharsToEscape;
    context.count = numAddlChars;
    context.escapeHighBit = escapeAllHighBitCharacters;
    context.escapePercents = escapePercents;
    context.agreesOverASCII = (__CFStringEncodingIsSupersetOfASCII(toEncoding) && __CFStringEncodingIsSupersetOfASCII(fromEncoding)) ? true : false;
    context.encodingsMatch = (fromEncoding == toEncoding) ? true : false;
    return _addPercentEscapesToString(CFGetAllocator(urlString), urlString, _shouldEscapeForEncodingConversion, _convertEscapeSequence, toEncoding, &context);
}

static __CFURL* __CFURLCreateWithArbitraryString(CFAllocatorRef allocator, CFStringRef URLString, CFURLRef baseURL) {
    if (!URLString || CFStringGetLength(URLString) == 0) {
        return NULL;
    }
    __CFURL* url = __CFURLAlloc(allocator);
    if (url) {
        __CFURLInitWithString(url, URLString, baseURL);
    }
    return url;
}

/* This function is this way because I pulled it out of _resolvedURLPath 
 *  (so that _resolvedFileSystemPath could use it), and I didn't want to 
 *  spend a bunch of energy reworking the code.  So instead of being a bit
 *  more intelligent about inputs, it just demands a slightly perverse set
 *  of parameters, to match the old _resolvedURLPath code.  -- REW, 6/14/99
 */
static CFStringRef _resolvedPath(UniChar* pathStr, UniChar* end, UniChar pathDelimiter, Boolean stripLeadingDotDots, Boolean stripTrailingDelimiter, CFAllocatorRef alloc) {
    UniChar* idx = pathStr;
    while (idx < end) {
        if (*idx == '.') {
            if (idx + 1 == end) {
                if (idx != pathStr) {
                    *idx = '\0';
                    end = idx;
                }
                break;
            } else if (*(idx + 1) == pathDelimiter) {
                if (idx + 2 != end || idx != pathStr) {
                    memmove(idx, idx + 2, (end - (idx + 2) + 1) * sizeof(UniChar));
                    end -= 2;
                    continue;
                } else {
                    // Do not delete the sole path component
                    break;
                }
            } else if (( end - idx >= 2 ) &&  *(idx + 1) == '.' && (idx + 2 == end || (( end - idx > 2 ) && *(idx + 2) == pathDelimiter))) {
                if (idx - pathStr >= 2) {
                    // Need at least 2 characters between index and pathStr, because we know if index != newPath, then *(index-1) == pathDelimiter, and we need something before that to compact out.
                    UniChar* lastDelim = idx - 2;
                    while (lastDelim >= pathStr && *lastDelim != pathDelimiter) {
                        lastDelim--;
                    }
                    lastDelim++;
                    if (lastDelim != idx && (idx - lastDelim != 3 || *lastDelim != '.' || *(lastDelim + 1) != '.')) {
                        // We have a genuine component to compact out
                        if (idx + 2 != end) {
                            size_t numCharsToMove = end - (idx + 3) + 1; // +1 to move the '\0' as well
                            memmove(lastDelim, idx + 3, numCharsToMove * sizeof(UniChar));
                            end -= (idx + 3 - lastDelim);
                            idx = lastDelim;
                            continue;
                        } else if (lastDelim != pathStr) {
                            *lastDelim = '\0';
                            end = lastDelim;
                            break;
                        } else {
                            // Don't allow the path string to devolve to the empty string.  Fall back to "." instead. - REW
                            pathStr[0] = '.';
                            pathStr[1] = '/';
                            pathStr[2] = '\0';
                            end = &pathStr[3];
                            break;
                        }
                    }
                } else if (stripLeadingDotDots) {
                    if (idx + 3 != end) {
                        size_t numCharsToMove = end - (idx + 3) + 1;
                        memmove(idx, idx + 3, numCharsToMove * sizeof(UniChar));
                        end -= 3;
                        continue;
                    } else {
                        // Do not devolve the last path component
                        break;
                    }
                }
            }
        }
        while (idx < end && *idx != pathDelimiter) {
            idx++;
        }
        idx++;
    }
    if (stripTrailingDelimiter && end > pathStr && end - 1 != pathStr && *(end - 1) == pathDelimiter) {
        end--;
    }
    return CFStringCreateWithCharactersNoCopy(alloc, pathStr, end - pathStr, alloc);
}

static CFMutableStringRef resolveAbsoluteURLString(CFAllocatorRef alloc, CFStringRef relString, UInt32 relFlags, CFRange* relRanges, CFStringRef baseString, UInt32 baseFlags, CFRange* baseRanges) {
    CFMutableStringRef newString = CFStringCreateMutable(alloc, 0);
    CFIndex bufLen = CFStringGetLength(baseString) + CFStringGetLength(relString); // Overkill, but guarantees we never allocate again
    UniChar* buf = (UniChar*)CFAllocatorAllocate(alloc, bufLen * sizeof(UniChar), 0);
    CFRange rg;

    rg = __CFURLGetComponentRange(baseFlags, baseRanges, HAS_SCHEME);
    if (rg.location != kCFNotFound) {
        CFStringGetCharacters(baseString, rg, buf);
        CFStringAppendCharacters(newString, buf, rg.length);
        CFStringAppendCString(newString, ":", kCFStringEncodingASCII);
    }

    if (relFlags & NET_LOCATION_MASK) {
        CFStringAppend(newString, relString);
    } else {
        CFStringAppendCString(newString, "//", kCFStringEncodingASCII);
        rg = __CFURLGetNetLocationRange(baseFlags, baseRanges);
        if (rg.location != kCFNotFound) {
            CFStringGetCharacters(baseString, rg, buf);
            CFStringAppendCharacters(newString, buf, rg.length);
        }

        if (relFlags & HAS_PATH) {
            CFRange relPathRg = __CFURLGetComponentRange(relFlags, relRanges, HAS_PATH);
            CFRange basePathRg = __CFURLGetComponentRange(baseFlags, baseRanges, HAS_PATH);
            CFStringRef newPath;
            Boolean useRelPath = false;
            Boolean useBasePath = false;
            if (basePathRg.location == kCFNotFound) {
                useRelPath = true;
            } else if (relPathRg.length == 0) {
                useBasePath = true;
            } else if (CFStringGetCharacterAtIndex(relString, relPathRg.location) == '/') {
                useRelPath = true;
            } else if (basePathRg.location == kCFNotFound || basePathRg.length == 0) {
                useRelPath = true;
            }
            if (useRelPath) {
                newPath = CFStringCreateWithSubstring(alloc, relString, relPathRg);
            } else if (useBasePath) {
                newPath = CFStringCreateWithSubstring(alloc, baseString, basePathRg);
            } else {
                // #warning FIXME - Get rid of this allocation
                UniChar* newPathBuf = (UniChar*)CFAllocatorAllocate(alloc, sizeof(UniChar) * (relPathRg.length + basePathRg.length + 1), 0);
                UniChar* idx, * end;
                CFStringGetCharacters(baseString, basePathRg, newPathBuf);
                idx = newPathBuf + basePathRg.length - 1;
                while (idx != newPathBuf && *idx != '/') {
                    idx--;
                }
                if (*idx == '/') {
                    idx++;
                }
                CFStringGetCharacters(relString, relPathRg, idx);
                end = idx + relPathRg.length;
                *end = 0;
                newPath = _resolvedPath(newPathBuf, end, '/', false, false, alloc);
            }

            // If the relative URL does not begin with a slash and
            // the base does not end with a slash, add a slash.
            if ((basePathRg.location == kCFNotFound || basePathRg.length == 0) &&
                CFStringGetCharacterAtIndex(newPath, 0) != '/')
            {
                CFStringAppendCString(newString, "/", kCFStringEncodingASCII);
            }

            CFStringAppend(newString, newPath);
            CFRelease(newPath);
            rg.location = relPathRg.location + relPathRg.length;
            rg.length = CFStringGetLength(relString);
            if (rg.length > rg.location) {
                rg.length -= rg.location;
                CFStringGetCharacters(relString, rg, buf);
                CFStringAppendCharacters(newString, buf, rg.length);
            }
        } else {
            rg = __CFURLGetComponentRange(baseFlags, baseRanges, HAS_PATH);
            if (rg.location != kCFNotFound) {
                CFStringGetCharacters(baseString, rg, buf);
                CFStringAppendCharacters(newString, buf, rg.length);
            }

            if (!(relFlags & RESOURCE_SPECIFIER_MASK)) {
                // ???  Can this ever happen?
                UInt32 rsrcFlag = __CFURLGetFirstResourceSpecifierFlag(baseFlags);
                if (rsrcFlag) {
                    rg.location = __CFURLGetComponentRange(baseFlags, baseRanges, rsrcFlag).location;
                    rg.length = CFStringGetLength(baseString) - rg.location;
                    rg.location--;  // To pick up the separator
                    rg.length++;
                    CFStringGetCharacters(baseString, rg, buf);
                    CFStringAppendCharacters(newString, buf, rg.length);
                }
            } else if (relFlags & HAS_PARAMETERS) {
                rg = __CFURLGetComponentRange(relFlags, relRanges, HAS_PARAMETERS);
                rg.location--;  // To get the semicolon that starts the parameters
                rg.length = CFStringGetLength(relString) - rg.location;
                CFStringGetCharacters(relString, rg, buf);
                CFStringAppendCharacters(newString, buf, rg.length);
            } else {
                // Sigh; we have to resolve these against one another
                rg = __CFURLGetComponentRange(baseFlags, baseRanges, HAS_PARAMETERS);
                if (rg.location != kCFNotFound) {
                    CFStringAppendCString(newString, ";", kCFStringEncodingASCII);
                    CFStringGetCharacters(baseString, rg, buf);
                    CFStringAppendCharacters(newString, buf, rg.length);
                }
                rg = __CFURLGetComponentRange(relFlags, relRanges, HAS_QUERY);
                if (rg.location != kCFNotFound) {
                    CFStringAppendCString(newString, "?", kCFStringEncodingASCII);
                    CFStringGetCharacters(relString, rg, buf);
                    CFStringAppendCharacters(newString, buf, rg.length);
                } else {
                    rg = __CFURLGetComponentRange(baseFlags, baseRanges, HAS_QUERY);
                    if (rg.location != kCFNotFound) {
                        CFStringAppendCString(newString, "?", kCFStringEncodingASCII);
                        CFStringGetCharacters(baseString, rg, buf);
                        CFStringAppendCharacters(newString, buf, rg.length);
                    }
                }
                // Only the relative portion of the URL can supply the fragment; otherwise, what would be in the relativeURL?
                rg = __CFURLGetComponentRange(relFlags, relRanges, HAS_FRAGMENT);
                if (rg.location != kCFNotFound) {
                    CFStringAppendCString(newString, "#", kCFStringEncodingASCII);
                    CFStringGetCharacters(relString, rg, buf);
                    CFStringAppendCharacters(newString, buf, rg.length);
                }
            }
        }
    }
    CFAllocatorDeallocate(alloc, buf);
    return newString;
}

static CFStringRef _retainedComponentString(CFURLRef url, UInt32 compFlag, Boolean fromOriginalString, Boolean removePercentEscapes) {
    CFRange rg;
    CFStringRef comp;
    CFAllocatorRef alloc = CFGetAllocator(url);
    CF_VALIDATE_ARG(URL_PATH_TYPE(url) == FULL_URL_REPRESENTATION, "passed a file system URL");
    if (removePercentEscapes) {
        fromOriginalString = true;
    }
    __CFURLEnsureComponentsParsed(url);
    rg = __CFURLGetComponentRange(url->_flags, url->ranges, compFlag);
    if (rg.location == kCFNotFound) {
        return NULL;
    }
    if (compFlag & HAS_SCHEME && url->_flags & HAS_HTTP_SCHEME) {
        comp = kCFURLHTTPScheme;
        CFRetain(comp);
    } else if (compFlag & HAS_SCHEME && url->_flags & HAS_FILE_SCHEME) {
        comp = kCFURLFileScheme;
        CFRetain(comp);
    } else {
        comp = CFStringCreateWithSubstring(alloc, url->_string, rg);
    }
    if (!fromOriginalString) {
        __CFURLEnsureSanitizedString(url);
        if (!(url->_flags & ORIGINAL_AND_URL_STRINGS_MATCH) &&
            (url->_flags & (compFlag << BIT_SHIFT_FROM_COMPONENT_TO_DIFFERS_FLAG)))
        {
            CFStringRef newComp = correctedComponent(comp, compFlag, url->_encoding);
            CFRelease(comp);
            comp = newComp;
        }
    }
    if (removePercentEscapes) {
        CFStringRef tmp;
        if (url->_flags & IS_OLD_UTF8_STYLE || url->_encoding == kCFStringEncodingUTF8) {
            tmp = CFURLCreateStringByReplacingPercentEscapes(alloc, comp, CFSTR(""));
        } else {
            tmp = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(alloc, comp, CFSTR(""), url->_encoding);
        }
        CFRelease(comp);
        comp = tmp;
    }
    return comp;
}

static CFStringRef _unescapedParameterString(CFURLRef anURL) {
    CFStringRef str;
    if (CF_IS_OBJC(anURL)) {
        CF_OBJC_CALL(CFStringRef, str, anURL, "parameterString");
        if (str) {
            CFRetain(str);
        }
        return str;
    }
    CF_VALIDATE_OBJECT_ARG(CF, anURL, __kCFURLTypeID);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        return NULL;
    }
    str = _retainedComponentString(anURL, HAS_PARAMETERS, false, false);
    if (str) {
        return str;
    }
    if (!(anURL->_flags & IS_DECOMPOSABLE)) {
        return NULL;
    }
    if (!anURL->_base || (anURL->_flags & (NET_LOCATION_MASK | HAS_PATH | HAS_SCHEME))) {
        return NULL;
        // Parameter string definitely coming from the relative portion of the URL
    }
    return _unescapedParameterString(anURL->_base);
}

static CFStringRef _unescapedQueryString(CFURLRef anURL) {
    CFStringRef str;
    if (CF_IS_OBJC(anURL)) {
        CF_OBJC_CALL(CFStringRef, str, anURL, "query");
        if (str) {
            CFRetain(str);
        }
        return str;
    }
    CF_VALIDATE_OBJECT_ARG(CF, anURL, __kCFURLTypeID);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        return NULL;
    }
    str = _retainedComponentString(anURL, HAS_QUERY, false, false);
    if (str) {
        return str;
    }
    if (!(anURL->_flags & IS_DECOMPOSABLE)) {
        return NULL;
    }
    if (!anURL->_base || (anURL->_flags & (HAS_SCHEME | NET_LOCATION_MASK | HAS_PATH | HAS_PARAMETERS))) {
        return NULL;
    }
    return _unescapedQueryString(anURL->_base);
}

// Fragments are NEVER taken from a base URL
static CFStringRef _unescapedFragment(CFURLRef anURL) {
    CFStringRef str;
    if (CF_IS_OBJC(anURL)) {
        CF_OBJC_CALL(CFStringRef, str, anURL, "fragment");
        if (str) {
            CFRetain(str);
        }
        return str;
    }
    CF_VALIDATE_OBJECT_ARG(CF, anURL, __kCFURLTypeID);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        return NULL;
    }
    str = _retainedComponentString(anURL, HAS_FRAGMENT, false, false);
    return str;
}

static CFIndex insertionLocationForMask(CFURLRef url, CFOptionFlags mask) {
    CFIndex firstMaskFlag = 1;
    CFIndex lastComponentBeforeMask = 0;
    while (firstMaskFlag <= HAS_FRAGMENT) {
        if (firstMaskFlag & mask) {
            break;
        }
        if (url->_flags & firstMaskFlag) {
            lastComponentBeforeMask = firstMaskFlag;
        }
        firstMaskFlag = firstMaskFlag << 1;
    }
    if (lastComponentBeforeMask == 0) {
        // mask includes HAS_SCHEME
        return 0;
    } else if (lastComponentBeforeMask == HAS_SCHEME) {
        // Do not have to worry about the non-decomposable case here.  However, we must be prepared for the degenerate
        // case file:/path/immediately/without/host
        CFRange schemeRg = __CFURLGetComponentRange(url->_flags, url->ranges, HAS_SCHEME);
        CFRange pathRg = __CFURLGetComponentRange(url->_flags, url->ranges, HAS_PATH);
        if (schemeRg.length + 1 == pathRg.location) {
            return schemeRg.length + 1;
        } else {
            return schemeRg.length + 3;
        }
    } else {
        // For all other components, the separator precedes the component, so there's no need
        // to add extra chars to get to the next insertion point
        CFRange rg = __CFURLGetComponentRange(url->_flags, url->ranges, lastComponentBeforeMask);
        return rg.location + rg.length;
    }
}

static CFRange _CFURLGetCharRangeForMask(CFURLRef url, CFOptionFlags mask, CFRange* charRangeWithSeparators) {
    CFOptionFlags currentOption;
    CFOptionFlags firstMaskFlag = HAS_SCHEME;
    Boolean haveReachedMask = false;
    CFIndex beforeMask = 0;
    CFIndex afterMask = kCFNotFound;
    CFRange* currRange = url->ranges;
    CFRange maskRange = {kCFNotFound, 0};
    for (currentOption = 1; currentOption <= HAS_FRAGMENT; currentOption = currentOption << 1) {
        if (!haveReachedMask && (currentOption & mask) != 0) {
            firstMaskFlag = currentOption;
            haveReachedMask = true;
        }
        if (!(url->_flags & currentOption)) {
            continue;
        }
        if (!haveReachedMask) {
            beforeMask = currRange->location + currRange->length;
        } else if (currentOption <= mask) {
            if (maskRange.location == kCFNotFound) {
                maskRange = *currRange;
            } else {
                maskRange.length = currRange->location + currRange->length - maskRange.location;
            }
        } else {
            afterMask = currRange->location;
            break;
        }
        currRange++;
    }
    if (afterMask == kCFNotFound) {
        afterMask = maskRange.location + maskRange.length;
    }
    charRangeWithSeparators->location = beforeMask;
    charRangeWithSeparators->length = afterMask - beforeMask;
    return maskRange;
}

static CFRange _getCharRangeInDecomposableURL(CFURLRef url, CFURLComponentType component, CFRange* rangeIncludingSeparators) {
    CFOptionFlags mask;
    switch (component) {
        case kCFURLComponentScheme:
            mask = HAS_SCHEME;
            break;
        case kCFURLComponentNetLocation:
            mask = NET_LOCATION_MASK;
            break;
        case kCFURLComponentPath:
            mask = HAS_PATH;
            break;
        case kCFURLComponentResourceSpecifier:
            mask = RESOURCE_SPECIFIER_MASK;
            break;
        case kCFURLComponentUser:
            mask = HAS_USER;
            break;
        case kCFURLComponentPassword:
            mask = HAS_PASSWORD;
            break;
        case kCFURLComponentUserInfo:
            mask = HAS_USER | HAS_PASSWORD;
            break;
        case kCFURLComponentHost:
            mask = HAS_HOST;
            break;
        case kCFURLComponentPort:
            mask = HAS_PORT;
            break;
        case kCFURLComponentParameterString:
            mask = HAS_PARAMETERS;
            break;
        case kCFURLComponentQuery:
            mask = HAS_QUERY;
            break;
        case kCFURLComponentFragment:
            mask = HAS_FRAGMENT;
            break;
        default:
            rangeIncludingSeparators->location = kCFNotFound;
            rangeIncludingSeparators->length = 0;
            return CFRangeMake(kCFNotFound, 0);
    }

    if ((url->_flags & mask) == 0) {
        rangeIncludingSeparators->location = insertionLocationForMask(url, mask);
        rangeIncludingSeparators->length = 0;
        return CFRangeMake(kCFNotFound, 0);
    } else {
        return _CFURLGetCharRangeForMask(url, mask, rangeIncludingSeparators);
    }
}

static CFRange _getCharRangeInNonDecomposableURL(CFURLRef url, CFURLComponentType component, CFRange* rangeIncludingSeparators) {
    if (component == kCFURLComponentScheme) {
        CFRange schemeRg = __CFURLGetComponentRange(url->_flags, url->ranges, HAS_SCHEME);
        rangeIncludingSeparators->location = 0;
        rangeIncludingSeparators->length = schemeRg.length + 1;
        return schemeRg;
    } else if (component == kCFURLComponentResourceSpecifier) {
        CFRange schemeRg = __CFURLGetComponentRange(url->_flags, url->ranges, HAS_SCHEME);
        CFIndex stringLength = CFStringGetLength(url->_string);
        if (schemeRg.length + 1 == stringLength) {
            rangeIncludingSeparators->location = schemeRg.length + 1;
            rangeIncludingSeparators->length = 0;
            return CFRangeMake(kCFNotFound, 0);
        } else {
            rangeIncludingSeparators->location = schemeRg.length;
            rangeIncludingSeparators->length = stringLength - schemeRg.length;
            return CFRangeMake(schemeRg.length + 1, rangeIncludingSeparators->length - 1);
        }
    } else {
        rangeIncludingSeparators->location = kCFNotFound;
        rangeIncludingSeparators->length = 0;
        return CFRangeMake(kCFNotFound, 0);
    }

}

static CFArrayRef WindowsPathToURLComponents(CFStringRef path, CFAllocatorRef alloc, Boolean isDir) {
    CFArrayRef tmp;
    CFMutableArrayRef urlComponents = NULL;
    CFIndex i = 0;

    tmp = CFStringCreateArrayBySeparatingStrings(alloc, path, CFSTR("\\"));
    urlComponents = CFArrayCreateMutableCopy(alloc, 0, tmp);
    CFRelease(tmp);

    CFStringRef str = (CFStringRef)CFArrayGetValueAtIndex(urlComponents, 0);
    if (CFStringGetLength(str) == 2 && CFStringGetCharacterAtIndex(str, 1) == ':') {
        CFArrayInsertValueAtIndex(urlComponents, 0, CFSTR("")); // So we get a leading '/' below
        i = 2; // Skip over the drive letter and the empty string we just inserted
    }
    CFIndex c;
    for (c = CFArrayGetCount(urlComponents); i < c; i++) {
        CFStringRef fileComp = (CFStringRef)CFArrayGetValueAtIndex(urlComponents, i);
        CFStringRef urlComp = __CFReplacePathIllegalCharacters(fileComp, alloc, false);
        if (!urlComp) {
            // Couldn't decode fileComp
            CFRelease(urlComponents);
            return NULL;
        }
        if (urlComp != fileComp) {
            CFArraySetValueAtIndex(urlComponents, i, urlComp);
        }
        CFRelease(urlComp);
    }

    if (isDir) {
        if (CFStringGetLength((CFStringRef)CFArrayGetValueAtIndex(urlComponents, CFArrayGetCount(urlComponents) - 1)) != 0) {
            CFArrayAppendValue(urlComponents, CFSTR(""));
        }
    }
    return urlComponents;
}

static CFStringRef WindowsPathToURLPath(CFStringRef path, CFAllocatorRef alloc, Boolean isDir) {
    CFArrayRef urlComponents;
    CFStringRef str;

    if (CFStringGetLength(path) == 0) {
        return CFStringCreateWithCString(alloc, "", kCFStringEncodingASCII);
    }
    urlComponents = WindowsPathToURLComponents(path, alloc, isDir);
    if (!urlComponents) {
        return CFStringCreateWithCString(alloc, "", kCFStringEncodingASCII);
    }

    // WindowsPathToURLComponents already added percent escapes for us; no need to add them again here.
    str = CFStringCreateByCombiningStrings(alloc, urlComponents, CFSTR("/"));
    CFRelease(urlComponents);
    return str;
}

static CFStringRef POSIXPathToURLPath(CFStringRef path, CFAllocatorRef alloc, Boolean isDirectory) {
    CFStringRef pathString = __CFReplacePathIllegalCharacters(path, alloc, true);
    if (isDirectory && CFStringGetCharacterAtIndex(path, CFStringGetLength(path) - 1) != '/') {
        CFStringRef tmp = CFStringCreateWithFormat(alloc, NULL, CFSTR("%@/"), pathString);
        CFRelease(pathString);
        pathString = tmp;
    }
    return pathString;
}

static CFStringRef URLPathToPOSIXPath(CFStringRef path, CFAllocatorRef allocator, CFStringEncoding encoding) {
    // This is the easiest case; just remove the percent escape codes and we're done
    CFStringRef result = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(allocator, path, CFSTR(""), encoding);
    if (result) {
        CFIndex length = CFStringGetLength(result);
        if (length > 1 && CFStringGetCharacterAtIndex(result, length - 1) == '/') {
            CFStringRef tmp = CFStringCreateWithSubstring(allocator, result, CFRangeMake(0, length - 1));
            CFRelease(result);
            result = tmp;
        }
    }
    return result;
}

static CFStringRef URLPathToWindowsPath(CFStringRef path, CFAllocatorRef allocator, CFStringEncoding encoding) {
    // Check for a drive letter, then flip all the slashes
    CFStringRef result;
    CFArrayRef tmp = CFStringCreateArrayBySeparatingStrings(allocator, path, CFSTR("/"));
    SInt32 count = CFArrayGetCount(tmp);
    CFMutableArrayRef components = CFArrayCreateMutableCopy(allocator, count, tmp);
    CFStringRef newPath;

    CFRelease(tmp);
    if (CFStringGetLength((CFStringRef)CFArrayGetValueAtIndex(components, count - 1)) == 0) {
        CFArrayRemoveValueAtIndex(components, count - 1);
        count--;
    }

    if (count > 1 && CFStringGetLength((CFStringRef)CFArrayGetValueAtIndex(components, 0)) == 0) {
        // Absolute path; we need to check for a drive letter in the second component, and if so, remove the first component
        CFStringRef firstComponent = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(allocator, (CFStringRef)CFArrayGetValueAtIndex(components, 1), CFSTR(""), encoding);
        UniChar ch;

        {
            if (CFStringGetLength(firstComponent) == 2 && ((ch = CFStringGetCharacterAtIndex(firstComponent, 1)) == '|' || ch == ':')) {
                // Drive letter
                CFArrayRemoveValueAtIndex(components, 0);
                if (ch == '|') {
                    CFStringRef driveStr = CFStringCreateWithFormat(allocator, NULL, CFSTR("%c:"), CFStringGetCharacterAtIndex(firstComponent, 0));
                    CFArraySetValueAtIndex(components, 0, driveStr);
                    CFRelease(driveStr);
                }
            }
            CFRelease(firstComponent);
        }
    }
    newPath = CFStringCreateByCombiningStrings(allocator, components, CFSTR("\\"));
    CFRelease(components);
    result = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(allocator, newPath, CFSTR(""), encoding);
    CFRelease(newPath);
    return result;
}

// Converts url from a file system path representation to a standard representation.
static void __CFURLConvertToFullRepresentation(CFURLRef roURL) {
    __CFURL* url = __CFWriteableURL(roURL);
    CFStringRef path = NULL;
    Boolean isDir = ((url->_flags & IS_DIRECTORY) != 0);
    CFAllocatorRef alloc = CFGetAllocator(url);

    switch (URL_PATH_TYPE(url)) {
        case kCFURLPOSIXPathStyle:
            if (url->_flags & POSIX_AND_URL_PATHS_MATCH) {
                path = (CFStringRef)CFRetain(url->_string);
            } else {
                path = POSIXPathToURLPath(url->_string, alloc, isDir);
            }
            break;
        case kCFURLWindowsPathStyle:
            path = WindowsPathToURLPath(url->_string, alloc, isDir);
            break;
    }
    CF_VALIDATE_ARG(path != NULL, "Encountered malformed file system URL %@", url);
    if (!url->_base) {
        CFMutableStringRef str = CFStringCreateMutable(alloc, 0);
        CFStringAppend(str, CFSTR("file://localhost"));
        CFStringAppend(str, path);
        url->_flags = (url->_flags & IS_DIRECTORY) | (FULL_URL_REPRESENTATION << 16) | IS_DECOMPOSABLE | IS_ABSOLUTE | IS_PARSED | HAS_SCHEME | HAS_FILE_SCHEME | HAS_HOST | HAS_PATH | ORIGINAL_AND_URL_STRINGS_MATCH;
        CFRelease(url->_string);
        url->_string = str;
        url->ranges = (CFRange*)CFAllocatorAllocate(alloc, sizeof(CFRange) * 3, 0);
        url->ranges[0] = CFRangeMake(0, 4);
        url->ranges[1] = CFRangeMake(7, 9);
        url->ranges[2] = CFRangeMake(16, CFStringGetLength(path));
        CFRelease(path);
    } else {
        CFRelease(url->_string);
        url->_flags = (url->_flags & IS_DIRECTORY) | (FULL_URL_REPRESENTATION << 16) | IS_DECOMPOSABLE | IS_PARSED | HAS_PATH | ORIGINAL_AND_URL_STRINGS_MATCH;
        url->_string = path;
        url->ranges = (CFRange*)CFAllocatorAllocate(alloc, sizeof(CFRange), 0);
        url->ranges[0] = CFRangeMake(0, CFStringGetLength(path));
    }
}

// Caller must release the returned string
static CFStringRef _resolveFileSystemPaths(CFStringRef relativePath, CFStringRef basePath, Boolean baseIsDir, CFURLPathStyle fsType, CFAllocatorRef alloc) {
    CFIndex baseLen = CFStringGetLength(basePath);
    CFIndex relLen = CFStringGetLength(relativePath);
    UniChar pathDelimiter = PATH_DELIM_FOR_TYPE(fsType);
    UniChar* buf = (UniChar*)CFAllocatorAllocate(alloc, sizeof(UniChar) * (relLen + baseLen + 2), 0);
    CFStringGetCharacters(basePath, CFRangeMake(0, baseLen), buf);
    if (baseIsDir) {
        if (buf[baseLen - 1] != pathDelimiter) {
            buf[baseLen] = pathDelimiter;
            baseLen++;
        }
    } else {
        UniChar* ptr = buf + baseLen - 1;
        while (ptr > buf && *ptr != pathDelimiter) {
            ptr--;
        }
        baseLen = ptr - buf + 1;
    }
    if (fsType == kCFURLHFSPathStyle) {
        // HFS relative paths will begin with a colon, so we must remove 
        //  the trailing colon from the base path first.
        baseLen--;
    }
    CFStringGetCharacters(relativePath, CFRangeMake(0, relLen), buf + baseLen);
    *(buf + baseLen + relLen) = '\0';
    return _resolvedPath(buf, buf + baseLen + relLen, pathDelimiter, false, true, alloc);
}

// relativeURL is known to be a file system URL whose base is a matching file system URL
static CFURLRef _CFURLCopyAbsoluteFileURL(CFURLRef relativeURL) {
    CFAllocatorRef alloc = CFGetAllocator(relativeURL);
    CFURLPathStyle fsType = URL_PATH_TYPE(relativeURL);
    CFURLRef base = relativeURL->_base;
    CFStringRef newPath = _resolveFileSystemPaths(
        relativeURL->_string, base->_string, (base->_flags & IS_DIRECTORY) != 0, fsType, alloc);
    CFURLRef result =  CFURLCreateWithFileSystemPath(
        alloc, newPath, fsType, (relativeURL->_flags & IS_DIRECTORY) != 0);
    CFRelease(newPath);
    return result;
}

// Assumes url is a CFURL (not an Obj-C NSURL)
static CFRange _rangeOfLastPathComponent(CFURLRef url) {
    UInt32 pathType = URL_PATH_TYPE(url);
    CFRange pathRg, componentRg;

    if (pathType ==  FULL_URL_REPRESENTATION) {
        __CFURLEnsureComponentsParsed(url);
        pathRg = __CFURLGetComponentRange(url->_flags, url->ranges, HAS_PATH);
    } else {
        pathRg = CFRangeMake(0, CFStringGetLength(url->_string));
    }

    if (pathRg.location == kCFNotFound || pathRg.length == 0) {
        // No path
        return pathRg;
    }
    if (CFStringGetCharacterAtIndex(url->_string, pathRg.location + pathRg.length - 1) == PATH_DELIM_FOR_TYPE(pathType)) {
        pathRg.length--;
        if (pathRg.length == 0) {
            pathRg.length++;
            return pathRg;
        }
    }
    if (CFStringFindWithOptions(url->_string, PATH_DELIM_AS_STRING_FOR_TYPE(pathType), pathRg, kCFCompareBackwards, &componentRg)) {
        componentRg.location++;
        componentRg.length = pathRg.location + pathRg.length - componentRg.location;
    } else {
        componentRg = pathRg;
    }
    return componentRg;
}

// There is no matching ObjC method for this functionality;
//  because this function sits on top of the CFURL primitives,
//  it's o.k. not to check for the need to dispatch an ObjC method instead,
//  but this means care must be taken that this function never call anything
//  that will result in dereferencing anURL without first checking for
//  an ObjC dispatch.  -- REW, 10/29/98
static CFStringRef __CFURLCreateStringWithFileSystemPath(CFAllocatorRef allocator, CFURLRef anURL, CFURLPathStyle fsType, Boolean resolveAgainstBase) {
    CFURLRef base = resolveAgainstBase ? CFURLGetBaseURL(anURL) : NULL;
    CFStringRef basePath = base ? __CFURLCreateStringWithFileSystemPath(allocator, base, fsType, false) : NULL;
    CFStringRef relPath = NULL;

    if (!CF_IS_OBJC(anURL)) {
        // We can grope the ivars
        CFURLPathStyle myType = URL_PATH_TYPE(anURL);
        if (myType == fsType) {
            relPath = (CFStringRef)CFRetain(anURL->_string);
        } else if (fsType == kCFURLPOSIXPathStyle && myType == FULL_URL_REPRESENTATION) {
            __CFURLEnsureComponentsParsed(anURL);
            if (anURL->_flags & POSIX_AND_URL_PATHS_MATCH) {
                relPath = _retainedComponentString(anURL, HAS_PATH, true, true);
            }
        }
    }

    if (relPath == NULL) {
        CFStringRef urlPath = CFURLCopyPath(anURL);
        CFStringEncoding enc = (anURL->_flags & IS_OLD_UTF8_STYLE) ? kCFStringEncodingUTF8 : anURL->_encoding;
        if (urlPath) {
            switch (fsType) {
                case kCFURLPOSIXPathStyle:
                    relPath = URLPathToPOSIXPath(urlPath, allocator, enc);
                    break;
                case kCFURLHFSPathStyle:
                    relPath = NULL;
                    break;
                case kCFURLWindowsPathStyle:
                    relPath = URLPathToWindowsPath(urlPath, allocator, enc);
                    break;
                default:
                    CF_VALIDATE_ARG(false, "Received unknown path type %d", fsType);
            }
            CFRelease(urlPath);
        }
    }

    //    For Tiger, leave this behavior in for all path types.  For Leopard, it would be nice to remove this entirely
    //    and do a linked-on-or-later check so we don't break third parties.
    //    See <rdar://problem/4003028> Converting volume name from POSIX to HFS form fails and
    //    <rdar://problem/4018895> CF needs to back out 4003028 for icky details.
    if (relPath && CFURLHasDirectoryPath(anURL) && CFStringGetLength(relPath) > 1 && CFStringGetCharacterAtIndex(relPath, CFStringGetLength(relPath) - 1) == PATH_DELIM_FOR_TYPE(fsType)) {
        CFStringRef tmp = CFStringCreateWithSubstring(allocator, relPath, CFRangeMake(0, CFStringGetLength(relPath) - 1));
        CFRelease(relPath);
        relPath = tmp;
    }

    // Note that !resolveAgainstBase implies !base
    if (!basePath || !relPath) {
        return relPath;
    } else {
        CFStringRef result = _resolveFileSystemPaths(relPath, basePath, CFURLHasDirectoryPath(base), fsType, allocator);
        CFRelease(basePath);
        CFRelease(relPath);
        return result;
    }
}


/*** CFURL class ***/

static void __CFURLDeallocate(CFTypeRef cf) {
    CFURLRef url = (CFURLRef)cf;
    CFAllocatorRef alloc;
    CF_VALIDATE_OBJECT_ARG(CF, cf, CFURLGetTypeID());
    alloc = CFGetAllocator(url);
    if (url->_string) {
        CFRelease(url->_string);
    }
    if (url->_base) {
        CFRelease(url->_base);
    }
    if (url->ranges) {
        CFAllocatorDeallocate(alloc, url->ranges);
    }
    if (__CFURLGetSanitizedString(url)) {
        CFRelease(__CFURLGetSanitizedString(url));
    }

    if (url->extra != NULL) {
        CFAllocatorDeallocate(alloc, url->extra);
    }
}

static Boolean __CFURLEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFURLRef url1 = (CFURLRef)cf1;
    CFURLRef url2 = (CFURLRef)cf2;
    UInt32 pathType1, pathType2;

    if (url1 == url2) {
        return true;
    }
    if ((url1->_flags & IS_PARSED) && (url2->_flags & IS_PARSED) && (url1->_flags & IS_DIRECTORY) != (url2->_flags & IS_DIRECTORY)) {
        return false;
    }
    if (url1->_base) {
        if (!url2->_base) {
            return false;
        }
        if (!CFEqual(url1->_base, url2->_base)) {
            return false;
        }
    } else if (url2->_base) {
        return false;
    }

    pathType1 = URL_PATH_TYPE(url1);
    pathType2 = URL_PATH_TYPE(url2);
    if (pathType1 == pathType2) {
        if (pathType1 != FULL_URL_REPRESENTATION) {
            return CFEqual(url1->_string, url2->_string);
        } else {
            // Do not compare the original strings; compare the sanatized strings.
            return CFEqual(CFURLGetString(url1), CFURLGetString(url2));
        }
    } else {
        // Try hard to avoid the expensive conversion from a file system representation to the canonical form
        CFStringRef scheme1 = CFURLCopyScheme(url1);
        CFStringRef scheme2 = CFURLCopyScheme(url2);
        Boolean eq;
        if (scheme1 && scheme2) {
            eq = CFEqual(scheme1, scheme2);
            CFRelease(scheme1);
            CFRelease(scheme2);
        } else if (!scheme1 && !scheme2) {
            eq = TRUE;
        } else {
            eq = FALSE;
            if (scheme1) {
                CFRelease(scheme1);
            } else {CFRelease(scheme2); }
        }
        if (!eq) {
            return false;
        }

        if (pathType1 == FULL_URL_REPRESENTATION) {
            __CFURLEnsureComponentsParsed(url1);
            if (url1->_flags & (HAS_USER | HAS_PORT | HAS_PASSWORD | HAS_QUERY | HAS_PARAMETERS | HAS_FRAGMENT )) {
                return false;
            }
        }

        if (pathType2 == FULL_URL_REPRESENTATION) {
            __CFURLEnsureComponentsParsed(url2);
            if (url2->_flags & (HAS_USER | HAS_PORT | HAS_PASSWORD | HAS_QUERY | HAS_PARAMETERS | HAS_FRAGMENT )) {
                return false;
            }
        }

        // No help for it; we now must convert to the canonical representation and compare.
        return CFEqual(CFURLGetString(url1), CFURLGetString(url2));
    }
}

static CFHashCode __CFURLHash(CFTypeRef cf) {
    /* This is tricky, because we do not want the hash value to 
     *  change as a file system URL is changed to its canonical 
     *  representation, nor do we wish to force the conversion 
     *  to the canonical representation. We choose instead to 
     *  take the last path component (or "/" in the unlikely 
     *  case that the path is empty), then hash on that.
     */
    CFURLRef url = (CFURLRef)cf;
    UInt32 result;
    if (CFURLCanBeDecomposed(url)) {
        CFStringRef lastComp = CFURLCopyLastPathComponent(url);
        CFStringRef hostNameRef = CFURLCopyHostName(url);

        result = 0;

        if (lastComp) {
            result = CFHash(lastComp);
            CFRelease(lastComp);
        }

        if (hostNameRef) {
            result ^= CFHash(hostNameRef);
            CFRelease(hostNameRef);
        }
    } else {
        result = CFHash(CFURLGetString(url));
    }
    return result;
}

static CFStringRef __CFURLCopyFormattingDescription(CFTypeRef cf, CFDictionaryRef formatOptions) {
    CFURLRef url = (CFURLRef)cf;
    if (!url->_base) {
        CFRetain(url->_string);
        return url->_string;
    } else {
        // Do not dereference url->_base; it may be an ObjC object
        return CFStringCreateWithFormat(
            CFGetAllocator(url),
            NULL, CFSTR("%@ -- %@"), url->_string, url->_base);
    }
}

static CFStringRef __CFURLCopyDescription(CFTypeRef cf) {
    CFURLRef url = (CFURLRef)cf;
    CFStringRef result;
    CFAllocatorRef alloc = CFGetAllocator(url);
    if (url->_base) {
        CFStringRef baseString = CFCopyDescription(url->_base);
        result = CFStringCreateWithFormat(
            alloc,
            NULL, CFSTR("<CFURL %p [%p]>{type = %d, string = %@,\n\tbase = %@}"),
            cf, alloc, URL_PATH_TYPE(url), url->_string, baseString);
        CFRelease(baseString);
    } else {
        result = CFStringCreateWithFormat(
            alloc,
            NULL, CFSTR("<CFURL %p [%p]>{type = %d, string = %@, base = (null)}"),
            cf, alloc, URL_PATH_TYPE(url), url->_string);
    }
    return result;
}

static const CFRuntimeClass __CFURLClass = {
    0,
    "CFURL",
    NULL, // init
    NULL, // copy
    __CFURLDeallocate,
    __CFURLEqual,
    __CFURLHash,
    __CFURLCopyFormattingDescription,
    __CFURLCopyDescription
};

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL CFStringEncoding _CFURLGetEncoding(CFURLRef url) {
    return url->_encoding;
}


CF_INTERNAL void _CFURLInitialize(void) {
    __kCFURLTypeID = _CFRuntimeRegisterClass(&__CFURLClass);
}


///////////////////////////////////////////////////////////////////// public

/* Uses UTF-8 to translate all percent escape sequences.
 * Returns NULL if it encounters a format failure.
 * May return the original string.
 */
CFStringRef  CFURLCreateStringByReplacingPercentEscapes(CFAllocatorRef alloc,
                                                        CFStringRef originalString,
                                                        CFStringRef charactersToLeaveEscaped)
{
    CFMutableStringRef newStr = NULL;
    CFIndex length;
    CFIndex mark = 0;
    CFRange percentRange, searchRange;
    CFStringRef escapedStr = NULL;
    CFMutableStringRef strForEscapedChar = NULL;
    UniChar escapedChar;
    Boolean escapeAll = (charactersToLeaveEscaped && CFStringGetLength(charactersToLeaveEscaped) == 0);
    Boolean failed = false;

    if (!originalString) {
        return NULL;
    }

    if (charactersToLeaveEscaped == NULL) {
        return CFStringCreateCopy(alloc, originalString);
    }

    length = CFStringGetLength(originalString);
    searchRange = CFRangeMake(0, length);

    while (!failed && CFStringFindWithOptions(originalString, CFSTR("%"), searchRange, 0, &percentRange)) {
        uint8_t bytes[4]; // Single UTF-8 character could require up to 4 bytes.
        uint8_t numBytesExpected;
        UniChar ch1, ch2;

        escapedStr = NULL;
        // Make sure we have at least 2 more characters
        if (length - percentRange.location < 3) {
            failed = true;
            break;
        }

        // if we don't have at least 2 more characters, we can't interpret the percent escape code,
        // so we assume the percent character is legit, and let it pass into the string
        ch1 = CFStringGetCharacterAtIndex(originalString, percentRange.location + 1);
        ch2 = CFStringGetCharacterAtIndex(originalString, percentRange.location + 2);
        if (!__CFMakeByte(ch1, ch2, bytes)) {
            failed = true;
            break;
        }
        if (!(bytes[0] & 0x80)) {
            numBytesExpected = 1;
        } else if (!(bytes[0] & 0x20)) {
            numBytesExpected = 2;
        } else if (!(bytes[0] & 0x10)) {
            numBytesExpected = 3;
        } else {
            numBytesExpected = 4;
        }
        if (numBytesExpected == 1) {
            // one byte sequence (most common case); handle this specially
            escapedChar = bytes[0];
            if (!strForEscapedChar) {
                strForEscapedChar = CFStringCreateMutableWithExternalCharactersNoCopy(
                    alloc,
                    &escapedChar, 1, 1, kCFAllocatorNull);
            }
            escapedStr = strForEscapedChar;
        } else {
            CFIndex j;
            // Make sure up front that we have enough characters
            if (length < percentRange.location + numBytesExpected * 3) {
                failed = true;
                break;
            }
            for (j = 1; j < numBytesExpected; j++) {
                if (CFStringGetCharacterAtIndex(originalString, percentRange.location + 3 * j) != '%') {
                    failed = true;
                    break;
                }
                ch1 = CFStringGetCharacterAtIndex(originalString, percentRange.location + 3 * j + 1);
                ch2 = CFStringGetCharacterAtIndex(originalString, percentRange.location + 3 * j + 2);
                if (!__CFMakeByte(ch1, ch2, bytes + j)) {
                    failed = true;
                    break;
                }
            }

            // !!! We should do the low-level bit-twiddling ourselves; this is expensive!  REW, 6/10/99
            escapedStr = CFStringCreateWithBytes(alloc, bytes, numBytesExpected, kCFStringEncodingUTF8, false);
            if (!escapedStr) {
                failed = true;
                break;
            }
            if (CFStringGetLength(escapedStr) == 0 && numBytesExpected == 3 &&
                bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf)
            {
                // Somehow, the UCS-2 BOM got translated in to a UTF8 string
                escapedChar = 0xfeff;
                if (!strForEscapedChar) {
                    strForEscapedChar = CFStringCreateMutableWithExternalCharactersNoCopy(
                        alloc,
                        &escapedChar, 1, 1, kCFAllocatorNull);
                }
                CFRelease(escapedStr);
                escapedStr = strForEscapedChar;
            }
        }

        // The new character is in escapedChar;
        //  the number of percent escapes it took is in numBytesExpected.
        searchRange.location = percentRange.location + 3 * numBytesExpected;
        searchRange.length = length - searchRange.location;

        if (!escapeAll) {
            if (CFStringFind(charactersToLeaveEscaped, escapedStr, 0).location != kCFNotFound) {
                if (escapedStr != strForEscapedChar) {
                    CFRelease(escapedStr);
                    escapedStr = NULL;
                }
                continue;
            }
        }

        if (!newStr) {
            newStr = CFStringCreateMutable(alloc, length);
        }
        if (percentRange.location - mark > 0) {
            // The creation of this temporary string is unfortunate.
            CFStringRef substring = CFStringCreateWithSubstring(
                alloc,
                originalString, CFRangeMake(mark, percentRange.location - mark));
            CFStringAppend(newStr, substring);
            CFRelease(substring);
        }
        CFStringAppend(newStr, escapedStr);
        if (escapedStr != strForEscapedChar) {
            CFRelease(escapedStr);
            escapedStr = NULL;
        }

        // We need mark to be the index of the first character 
        //  beyond the escape sequence.
        mark = searchRange.location;
    }

    if (escapedStr && escapedStr != strForEscapedChar) {
        CFRelease(escapedStr);
    }
    if (strForEscapedChar) {
        CFRelease(strForEscapedChar);
    }
    if (failed) {
        if (newStr) {
            CFRelease(newStr);
        }
        return NULL;
    } else if (newStr) {
        if (mark < length) {
            // Need to cat on the remainder of the string
            CFStringRef substring = CFStringCreateWithSubstring(
                alloc,
                originalString, CFRangeMake(mark, length - mark));
            CFStringAppend(newStr, substring);
            CFRelease(substring);
        }
        return newStr;
    } else {
        return CFStringCreateCopy(alloc, originalString);
    }
}

CFStringRef CFURLCreateStringByReplacingPercentEscapesUsingEncoding(CFAllocatorRef alloc,
                                                                    CFStringRef originalString,
                                                                    CFStringRef charactersToLeaveEscaped,
                                                                    CFStringEncoding enc)
{
    if (enc == kCFStringEncodingUTF8) {
        return CFURLCreateStringByReplacingPercentEscapes(alloc, originalString, charactersToLeaveEscaped);
    } else {
        CFMutableStringRef newStr = NULL;
        CFMutableStringRef escapedStr = NULL;
        CFIndex length;
        CFIndex mark = 0;
        CFRange percentRange, searchRange;
        Boolean escapeAll = (charactersToLeaveEscaped && CFStringGetLength(charactersToLeaveEscaped) == 0);
        Boolean failed = false;
        uint8_t byteBuffer[8];
        uint8_t* bytes = byteBuffer;
        int capacityOfBytes = 8;

        if (!originalString) {
            return NULL;
        }

        if (charactersToLeaveEscaped == NULL) {
            return CFStringCreateCopy(alloc, originalString);
        }

        length = CFStringGetLength(originalString);
        searchRange = CFRangeMake(0, length);

        while (!failed && CFStringFindWithOptions(originalString, CFSTR("%"), searchRange, 0, &percentRange)) {
            UniChar ch1, ch2;
            CFIndex percentLoc = percentRange.location;
            CFStringRef convertedString;
            int numBytesUsed = 0;
            do {
                // Make sure we have at least 2 more characters
                if (length - percentLoc < 3) {
                    failed = true; break;
                }

                if (numBytesUsed == capacityOfBytes) {
                    if (bytes == byteBuffer) {
                        bytes = (uint8_t*)CFAllocatorAllocate(alloc, 16, 0);
                        memmove(bytes, byteBuffer, capacityOfBytes);
                        capacityOfBytes = 16;
                    } else {
                        void* oldbytes = bytes;
                        int oldcap = capacityOfBytes;
                        capacityOfBytes = 2 * capacityOfBytes;
                        bytes = (uint8_t*)CFAllocatorAllocate(alloc, capacityOfBytes, 0);
                        memmove(bytes, oldbytes, oldcap);
                        CFAllocatorDeallocate(alloc, oldbytes);
                    }
                }
                percentLoc++;
                ch1 = CFStringGetCharacterAtIndex(originalString, percentLoc);
                percentLoc++;
                ch2 = CFStringGetCharacterAtIndex(originalString, percentLoc);
                percentLoc++;
                if (!__CFMakeByte(ch1, ch2, bytes + numBytesUsed)) {
                    failed = true;
                    break;
                }
                numBytesUsed++;
            } while (CFStringGetCharacterAtIndex(originalString, percentLoc) == '%');
            searchRange.location = percentLoc;
            searchRange.length = length - searchRange.location;

            if (failed) {
                break;
            }
            convertedString = CFStringCreateWithBytes(alloc, bytes, numBytesUsed, enc, false);
            if (!convertedString) {
                failed = true;
                break;
            }

            if (!newStr) {
                newStr = CFStringCreateMutable(alloc, length);
            }
            if (percentRange.location - mark > 0) {
                // The creation of this temporary string is unfortunate.
                CFStringRef substring = CFStringCreateWithSubstring(
                    alloc,
                    originalString, CFRangeMake(mark, percentRange.location - mark));
                CFStringAppend(newStr, substring);
                CFRelease(substring);
            }

            if (escapeAll) {
                CFStringAppend(newStr, convertedString);
                CFRelease(convertedString);
            } else {
                CFIndex i, c = CFStringGetLength(convertedString);
                if (!escapedStr) {
                    escapedStr = CFStringCreateMutableWithExternalCharactersNoCopy(
                        alloc,
                        &ch1, 1, 1, kCFAllocatorNull);
                }
                for (i = 0; i < c; i++) {
                    ch1 = CFStringGetCharacterAtIndex(convertedString, i);
                    if (CFStringFind(charactersToLeaveEscaped, escapedStr, 0).location == kCFNotFound) {
                        CFStringAppendCharacters(newStr, &ch1, 1);
                    } else {
                        // Must regenerate the escape sequence for this character;
                        //  because we started with percent escapes, we know this call cannot fail.
                        _appendPercentEscapesForCharacter(ch1, enc, newStr);
                    }
                }
            }

            // We need mark to be the index of the first character beyond the escape sequence.
            mark = searchRange.location;
        }

        if (escapedStr) {
            CFRelease(escapedStr);
        }
        if (bytes != byteBuffer) {
            CFAllocatorDeallocate(alloc, bytes);
        }
        if (failed) {
            if (newStr) {
                CFRelease(newStr);
            }
            return NULL;
        } else if (newStr) {
            if (mark < length) {
                // Need to cat on the remainder of the string
                CFStringRef substring = CFStringCreateWithSubstring(
                    alloc,
                    originalString, CFRangeMake(mark, length - mark));
                CFStringAppend(newStr, substring);
                CFRelease(substring);
            }
            return newStr;
        } else {
            return CFStringCreateCopy(alloc, originalString);
        }
    }
}

CFStringRef CFURLCreateStringByAddingPercentEscapes(CFAllocatorRef allocator, CFStringRef originalString, CFStringRef charactersToLeaveUnescaped, CFStringRef legalURLCharactersToBeEscaped, CFStringEncoding encoding) {
    CFStringRef strings[2];
    strings[0] = charactersToLeaveUnescaped;
    strings[1] = legalURLCharactersToBeEscaped;
    return _addPercentEscapesToString(allocator, originalString, _shouldPercentReplaceChar, NULL, encoding, strings);
}

CFTypeID CFURLGetTypeID(void) {
    return __kCFURLTypeID;
}

// encoding will be used both to interpret the bytes of URLBytes, and to interpret any percent-escapes within the bytes.
CFURLRef CFURLCreateWithBytes(CFAllocatorRef allocator, const uint8_t* URLBytes, CFIndex length, CFStringEncoding encoding, CFURLRef baseURL) {
    CFStringRef urlString = CFStringCreateWithBytes(allocator, URLBytes, length, encoding, false);
    if (!urlString || CFStringGetLength(urlString) == 0) {
        if (urlString) {
            CFRelease(urlString);
        }
        return NULL;
    }
    if (createOldUTF8StyleURLs_always_false()) {
        if (encoding != kCFStringEncodingUTF8) {
            CFStringRef tmp = _convertPercentEscapes(urlString, encoding, kCFStringEncodingUTF8, false, false, NULL, 0);
            CFRelease(urlString);
            urlString = tmp;
            if (!urlString) {
                return NULL;
            }
        }
    }

    __CFURL* result = __CFURLAlloc(allocator);
    if (result) {
        __CFURLInitWithString(result, urlString, baseURL);
        if (encoding != kCFStringEncodingUTF8 && !createOldUTF8StyleURLs_always_false()) {
            result->_encoding = encoding;
        }
    }
    CFRelease(urlString); // it's retained by result, now.
    return result;
}

CFDataRef CFURLCreateData(CFAllocatorRef allocator, CFURLRef url, CFStringEncoding encoding, Boolean escapeWhitespace) {
    static const UniChar whitespaceChars[4] = {' ', '\n', '\r', '\t'};
    CFStringRef myStr = CFURLGetString(url);
    CFStringRef newStr;
    CFDataRef result;
    if (url->_flags & IS_OLD_UTF8_STYLE) {
        newStr = (encoding == kCFStringEncodingUTF8) ? (CFStringRef)CFRetain(myStr) : _convertPercentEscapes(myStr, kCFStringEncodingUTF8, encoding, true, false, escapeWhitespace ? whitespaceChars : NULL, escapeWhitespace ? 4 : 0);
    } else {
        newStr = myStr;
        CFRetain(newStr);
    }
    result = CFStringCreateExternalRepresentation(allocator, newStr, encoding, 0);
    CFRelease(newStr);
    return result;
}

// Any escape sequences in URLString will be interpreted via UTF-8.
CFURLRef CFURLCreateWithString(CFAllocatorRef allocator, CFStringRef URLString, CFURLRef baseURL) {
    __CFURL* url;
    if (!URLString || CFStringGetLength(URLString) == 0) {
        return NULL;
    }
    if (!_CFStringIsLegalURLString(URLString)) {
        return NULL;
    }
    url = __CFURLAlloc(allocator);
    if (url) {
        __CFURLInitWithString(url, URLString, baseURL);
    }
    return url;
}

CFURLRef CFURLCreateAbsoluteURLWithBytes(CFAllocatorRef alloc, const UInt8* relativeURLBytes, CFIndex length, CFStringEncoding encoding, CFURLRef baseURL, Boolean useCompatibilityMode) {
    CFStringRef relativeString = CFStringCreateWithBytes(alloc, relativeURLBytes, length, encoding, false);
    if (!relativeString) {
        return NULL;
    }
    if (!useCompatibilityMode) {
        __CFURL* url = __CFURLCreateWithArbitraryString(alloc, relativeString, baseURL);
        CFRelease(relativeString);
        if (url) {
            url->_encoding = encoding;
            CFURLRef absURL = CFURLCopyAbsoluteURL(url);
            CFRelease(url);
            return absURL;
        } else {
            return NULL;
        }
    } else {
        UInt32 absFlags = 0;
        CFRange* absRanges;
        CFStringRef absString = NULL;
        Boolean absStringIsMutable = false;
        __CFURL* absURL;
        if (!baseURL) {
            absString = relativeString;
        } else {
            UniChar ch = CFStringGetCharacterAtIndex(relativeString, 0);
            if (ch == '?' || ch == ';' || ch == '#') {
                // Nothing but parameter + query + fragment; append to the baseURL string
                CFStringRef baseString;
                if (CF_IS_OBJC(baseURL)) {
                    baseString = CFURLGetString(baseURL);
                } else {
                    baseString = baseURL->_string;
                }
                absString = CFStringCreateMutable(alloc, CFStringGetLength(baseString) + CFStringGetLength(relativeString));
                CFStringAppend((CFMutableStringRef)absString, baseString);
                CFStringAppend((CFMutableStringRef)absString, relativeString);
                absStringIsMutable = true;
            } else {
                UInt32 relFlags = 0;
                CFRange* relRanges;
                CFStringRef relString = NULL;
                __CFParseURLComponents(alloc, relativeString, baseURL, &relFlags, &relRanges);
                if (relFlags & HAS_SCHEME) {
                    CFStringRef baseScheme = CFURLCopyScheme(baseURL);
                    CFRange relSchemeRange = __CFURLGetComponentRange(relFlags, relRanges, HAS_SCHEME);
                    if (baseScheme && CFStringGetLength(baseScheme) == relSchemeRange.length && CFStringHasPrefix(relativeString, baseScheme)) {
                        relString = CFStringCreateWithSubstring(alloc, relativeString, CFRangeMake(relSchemeRange.length + 1, CFStringGetLength(relativeString) - relSchemeRange.length - 1));
                        CFAllocatorDeallocate(alloc, relRanges);
                        relFlags = 0;
                        __CFParseURLComponents(alloc, relString, baseURL, &relFlags, &relRanges);
                    } else {
                        // Discard the base string; the relative string is absolute and we're not in the funky edge case where the schemes match
                        CFRetain(relativeString);
                        absString = relativeString;
                    }
                    if (baseScheme) {
                        CFRelease(baseScheme);
                    }
                } else {
                    CFRetain(relativeString);
                    relString = relativeString;
                }
                if (!absString) {
                    if (!CF_IS_OBJC(baseURL)) {
                        __CFURLEnsureComponentsParsed(baseURL);
                        absString = resolveAbsoluteURLString(alloc, relString, relFlags, relRanges, baseURL->_string, baseURL->_flags, baseURL->ranges);
                    } else {
                        CFStringRef baseString;
                        UInt32 baseFlags = 0;
                        CFRange* baseRanges;
                        if (CF_IS_OBJC(baseURL)) {
                            baseString = CFURLGetString(baseURL);
                        } else {
                            baseString = baseURL->_string;
                        }
                        __CFParseURLComponents(alloc, baseString, NULL, &baseFlags, &baseRanges);
                        absString = resolveAbsoluteURLString(alloc, relString, relFlags, relRanges, baseString, baseFlags, baseRanges);
                        CFAllocatorDeallocate(alloc, baseRanges);
                    }
                    absStringIsMutable = true;
                }
                if (relString) {
                    CFRelease(relString);
                }
                CFAllocatorDeallocate(alloc, relRanges);
            }
            CFRelease(relativeString);
        }
        __CFParseURLComponents(alloc, absString, NULL, &absFlags, &absRanges);
        if (absFlags & HAS_PATH) {
            CFRange pathRg = __CFURLGetComponentRange(absFlags, absRanges, HAS_PATH);
            // This is expensive, but it allows us to reuse _resolvedPath.  It should be cleaned up to get this allocation removed at some point. - REW
            UniChar* buf = (UniChar*)CFAllocatorAllocate(alloc, sizeof(UniChar) * (pathRg.length + 1), 0);
            CFStringRef newPath;
            CFStringGetCharacters(absString, pathRg, buf);
            buf[pathRg.length] = '\0';
            newPath = _resolvedPath(buf, buf + pathRg.length, '/', true, false, alloc);
            if (CFStringGetLength(newPath) != pathRg.length) {
                if (!absStringIsMutable) {
                    CFStringRef tmp = CFStringCreateMutableCopy(alloc, CFStringGetLength(absString), absString);
                    CFRelease(absString);
                    absString = tmp;
                }
                CFStringReplace((CFMutableStringRef)absString, pathRg, newPath);
            }
            CFRelease(newPath);
            // Do not deallocate buf; newPath took ownership of it.
        }
        CFAllocatorDeallocate(alloc, absRanges);
        absURL = __CFURLCreateWithArbitraryString(alloc, absString, NULL);
        CFRelease(absString);
        if (absURL) {
            absURL->_encoding = encoding;
        }
        return absURL;
    }
}

CFURLRef CFURLCopyAbsoluteURL(CFURLRef relativeURL) {
    CFURLRef base;
    __CFURL* anURL;
    CFURLPathStyle fsType;
    CFAllocatorRef alloc = CFGetAllocator(relativeURL);
    CFStringRef baseString, newString;
    UInt32 baseFlags;
    CFRange* baseRanges;
    Boolean baseIsObjC;

    CF_VALIDATE_PTR_ARG(relativeURL);
    if (CF_IS_OBJC(relativeURL)) {
        CFURLRef absoluteURL;
        CF_OBJC_CALL(CFURLRef, absoluteURL, relativeURL, "absoluteURL");
        if (absoluteURL) {
            CFRetain(absoluteURL);
        }
        return absoluteURL;
    }

    CF_VALIDATE_OBJECT_ARG(CF, relativeURL, __kCFURLTypeID);

    base = relativeURL->_base;
    if (!base) {
        return (CFURLRef)CFRetain(relativeURL);
    }
    baseIsObjC = CF_IS_OBJC(base);
    fsType = URL_PATH_TYPE(relativeURL);

    if (!baseIsObjC && fsType != FULL_URL_REPRESENTATION && fsType == URL_PATH_TYPE(base)) {
        return _CFURLCopyAbsoluteFileURL(relativeURL);
    }
    if (fsType != FULL_URL_REPRESENTATION) {
        __CFURLConvertToFullRepresentation(relativeURL);
        fsType = FULL_URL_REPRESENTATION;
    }
    __CFURLEnsureComponentsParsed(relativeURL);
    if ((relativeURL->_flags & POSIX_AND_URL_PATHS_MATCH) && !(relativeURL->_flags & (RESOURCE_SPECIFIER_MASK | NET_LOCATION_MASK)) && !baseIsObjC && (URL_PATH_TYPE(base) == kCFURLPOSIXPathStyle)) {
        // There's nothing to relativeURL's string except the path
        CFStringRef newPath = _resolveFileSystemPaths(relativeURL->_string, base->_string, CFURLHasDirectoryPath(base), kCFURLPOSIXPathStyle, alloc);
        CFURLRef result = CFURLCreateWithFileSystemPath(alloc, newPath, kCFURLPOSIXPathStyle, CFURLHasDirectoryPath(relativeURL));
        CFRelease(newPath);
        return result;
    }

    if (!baseIsObjC) {
        CFURLPathStyle baseType = URL_PATH_TYPE(base);
        if (baseType != FULL_URL_REPRESENTATION) {
            __CFURLConvertToFullRepresentation(base);
        } else {
            __CFURLEnsureComponentsParsed(base);
        }
        baseString = base->_string;
        baseFlags = base->_flags;
        baseRanges = base->ranges;
    } else {
        baseString = CFURLGetString(base);
        baseFlags = 0;
        baseRanges = NULL;
        __CFParseURLComponents(alloc, baseString, NULL, &baseFlags, &baseRanges);
    }

    newString = resolveAbsoluteURLString(alloc, relativeURL->_string, relativeURL->_flags, relativeURL->ranges, baseString, baseFlags, baseRanges);
    if (baseIsObjC) {
        CFAllocatorDeallocate(alloc, baseRanges);
    }
    anURL = __CFURLCreateWithArbitraryString(alloc, newString, NULL);
    CFRelease(newString);
    anURL->_encoding = relativeURL->_encoding;
    return anURL;
}

Boolean CFURLCanBeDecomposed(CFURLRef anURL) {
    anURL = _CFURLFromNSURL(anURL);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        return true;
    }
    __CFURLEnsureComponentsParsed(anURL);
    return ((anURL->_flags & IS_DECOMPOSABLE) != 0);
}

CFStringRef CFURLGetString(CFURLRef url) {
    CF_OBJC_FUNCDISPATCH(CFStringRef, url, "relativeString");
    if (URL_PATH_TYPE(url) != FULL_URL_REPRESENTATION) {
        if (url->_base && (url->_flags & POSIX_AND_URL_PATHS_MATCH)) {
            return url->_string;
        }
        __CFURLConvertToFullRepresentation(url);
    }
    __CFURLEnsureSanitizedString(url);
    if (url->_flags & ORIGINAL_AND_URL_STRINGS_MATCH) {
        return url->_string;
    } else {
        return __CFURLGetSanitizedString(url);
    }
}

CFIndex CFURLGetBytes(CFURLRef url, UInt8* buffer, CFIndex bufferLength) {
    CFIndex length, charsConverted, usedLength;
    CFStringRef string;
    CFStringEncoding enc;
    if (CF_IS_OBJC(url)) {
        string = CFURLGetString(url);
        enc = kCFStringEncodingUTF8;
    } else {
        if (URL_PATH_TYPE(url) != FULL_URL_REPRESENTATION) {
            __CFURLConvertToFullRepresentation(url);
        }
        string = url->_string;
        enc = url->_encoding;
    }
    length = CFStringGetLength(string);
    charsConverted = CFStringGetBytes(string, CFRangeMake(0, length), enc, 0, false, buffer, bufferLength, &usedLength);
    if (charsConverted != length) {
        return -1;
    } else {
        return usedLength;
    }
}

CFURLRef  CFURLGetBaseURL(CFURLRef anURL) {
    CF_OBJC_FUNCDISPATCH(CFURLRef, anURL, "baseURL");
    return anURL->_base;
}

CFStringRef  CFURLCopyScheme(CFURLRef anURL) {
    CFStringRef scheme;
    if (CF_IS_OBJC(anURL)) {
        CF_OBJC_CALL(CFStringRef, scheme, anURL, "scheme");
        if (scheme) {
            CFRetain(scheme);
        }
        return scheme;
    }
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        if (anURL->_base) {
            return CFURLCopyScheme(anURL->_base);
        } else {
            CFRetain(kCFURLFileScheme); // because caller will release it
            return kCFURLFileScheme;
        }
    }
    if (anURL->_flags & IS_PARSED && anURL->_flags & HAS_HTTP_SCHEME) {
        CFRetain(kCFURLHTTPScheme);
        return kCFURLHTTPScheme;
    }
    if (anURL->_flags & IS_PARSED && anURL->_flags & HAS_FILE_SCHEME) {
        CFRetain(kCFURLFileScheme);
        return kCFURLFileScheme;
    }
    scheme = _retainedComponentString(anURL, HAS_SCHEME, true, false);
    if (scheme) {
        return scheme;
    } else if (anURL->_base) {
        return CFURLCopyScheme(anURL->_base);
    } else {
        return NULL;
    }
}

CFStringRef CFURLCopyNetLocation(CFURLRef anURL) {
    anURL = _CFURLFromNSURL(anURL);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        // !!! This won't work if we go to putting the vol ref num in the net location for HFS
        if (anURL->_base) {
            return CFURLCopyNetLocation(anURL->_base);
        } else {
            CFRetain(kCFURLLocalhost);
            return kCFURLLocalhost;
        }
    }
    __CFURLEnsureComponentsParsed(anURL);
    if (anURL->_flags & NET_LOCATION_MASK) {
        // We provide the net location
        CFRange netRg = __CFURLGetNetLocationRange(anURL->_flags, anURL->ranges);
        CFStringRef netLoc;
        __CFURLEnsureSanitizedString(anURL);
        if (!(anURL->_flags & ORIGINAL_AND_URL_STRINGS_MATCH) &&
            (anURL->_flags & (USER_DIFFERS | PASSWORD_DIFFERS | HOST_DIFFERS | PORT_DIFFERS)))
        {
            // Only thing that can come before the net location is the scheme.
            // It's impossible for the scheme to contain percent escapes.
            // Therefore, we can use the location of netRg in _sanatizedString,
            //  just not the length.
            CFRange netLocEnd;
            netRg.length = CFStringGetLength(__CFURLGetSanitizedString(anURL)) - netRg.location;
            if (CFStringFindWithOptions(__CFURLGetSanitizedString(anURL), CFSTR("/"), netRg, 0, &netLocEnd)) {
                netRg.length = netLocEnd.location - netRg.location;
            }
            netLoc = CFStringCreateWithSubstring(CFGetAllocator(anURL), __CFURLGetSanitizedString(anURL), netRg);
        } else {
            netLoc = CFStringCreateWithSubstring(CFGetAllocator(anURL), anURL->_string, netRg);
        }
        return netLoc;
    } else if (anURL->_base) {
        return CFURLCopyNetLocation(anURL->_base);
    } else {
        return NULL;
    }
}

// NOTE - if you want an absolute path, you must first get the absolute URL.
// If you want a file system path, use the file system methods above.
CFStringRef  CFURLCopyPath(CFURLRef anURL) {
    anURL = _CFURLFromNSURL(anURL);
    if (URL_PATH_TYPE(anURL) == kCFURLPOSIXPathStyle &&
        (anURL->_flags & POSIX_AND_URL_PATHS_MATCH))
    {
        CFRetain(anURL->_string);
        return anURL->_string;
    }
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        __CFURLConvertToFullRepresentation(anURL);
    }
    return _retainedComponentString(anURL, HAS_PATH, false, false);
}

/* NULL if CFURLCanBeDecomposed(anURL) is false; also does not resolve the URL against its base.  See also CFCreateAbsoluteURL().  Note that, strictly speaking, any leading '/' is not considered part of the URL's path, although its presence or absence determines whether the path is absolute.  CFURLCopyPath()'s return value includes any leading slash (giving the path the normal POSIX appearance); CFURLCopyStrictPath()'s return value omits any leading slash, and uses isAbsolute to report whether the URL's path is absolute.
 *
 * CFURLCopyFileSystemPath() returns the URL's path as a file system path for the given path style.  All percent escape sequences are replaced.  The URL is not resolved against its base before computing the path.
 */
CFStringRef CFURLCopyStrictPath(CFURLRef anURL, Boolean* isAbsolute) {
    CFStringRef path = CFURLCopyPath(anURL);
    if (!path || CFStringGetLength(path) == 0) {
        if (path) {
            CFRelease(path);
        }
        if (isAbsolute) {
            *isAbsolute = false;
        }
        return NULL;
    }
    if (CFStringGetCharacterAtIndex(path, 0) == '/') {
        CFStringRef tmp;
        if (isAbsolute) {
            *isAbsolute = true;
        }
        tmp = CFStringCreateWithSubstring(CFGetAllocator(path), path, CFRangeMake(1, CFStringGetLength(path) - 1));
        CFRelease(path);
        path = tmp;
    } else {
        if (isAbsolute) {
            *isAbsolute = false;
        }
    }
    return path;
}

Boolean CFURLHasDirectoryPath(CFURLRef anURL) {
    CF_VALIDATE_OBJECT_ARG(CF, anURL, __kCFURLTypeID);
    if (URL_PATH_TYPE(anURL) == FULL_URL_REPRESENTATION) {
        __CFURLEnsureComponentsParsed(anURL);
        if (!anURL->_base || (anURL->_flags & (HAS_PATH | NET_LOCATION_MASK))) {
            return ((anURL->_flags & IS_DIRECTORY) != 0);
        }
        return CFURLHasDirectoryPath(anURL->_base);
    }
    return ((anURL->_flags & IS_DIRECTORY) != 0);
}

CFStringRef CFURLCopyResourceSpecifier(CFURLRef anURL) {
    anURL = _CFURLFromNSURL(anURL);
    CF_VALIDATE_OBJECT_ARG(CF, anURL, __kCFURLTypeID);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        return NULL;
    }
    __CFURLEnsureComponentsParsed(anURL);
    if (!(anURL->_flags & IS_DECOMPOSABLE)) {
        CFRange schemeRg = __CFURLGetComponentRange(anURL->_flags, anURL->ranges, HAS_SCHEME);
        CFIndex base = schemeRg.location + schemeRg.length + 1;
        __CFURLEnsureSanitizedString(anURL);
        if (__CFURLGetSanitizedString(anURL)) {
            // It is impossible to have a percent escape in the scheme (if there were one, we would have considered the URL a relativeURL with a  colon in the path instead), so this range computation is always safe.
            return CFStringCreateWithSubstring(CFGetAllocator(anURL), __CFURLGetSanitizedString(anURL), CFRangeMake(base, CFStringGetLength(__CFURLGetSanitizedString(anURL)) - base));
        } else {
            return CFStringCreateWithSubstring(CFGetAllocator(anURL), anURL->_string, CFRangeMake(base, CFStringGetLength(anURL->_string) - base));
        }
    } else {
        UInt32 firstRsrcSpecFlag = __CFURLGetFirstResourceSpecifierFlag(anURL->_flags);
        UInt32 flag;
        if (firstRsrcSpecFlag) {
            Boolean canUseOriginalString = true;
            Boolean canUseSanitizedString = true;
            CFAllocatorRef alloc = CFGetAllocator(anURL);
            __CFURLEnsureSanitizedString(anURL);
            if (!(anURL->_flags & ORIGINAL_AND_URL_STRINGS_MATCH)) {
                // See if any pieces in the resource specifier differ between sanitized string and original string
                for (flag = firstRsrcSpecFlag; flag != (HAS_FRAGMENT << 1); flag = flag << 1) {
                    if (anURL->_flags & (flag << BIT_SHIFT_FROM_COMPONENT_TO_DIFFERS_FLAG)) {
                        canUseOriginalString = false;
                        break;
                    }
                }
            }
            if (!canUseOriginalString) {
                // If none of the pieces prior to the first resource specifier flag differ, then we can use the offset from the original string as the offset in the sanitized string.
                for (flag = firstRsrcSpecFlag >> 1; flag != 0; flag = flag >> 1) {
                    if (anURL->_flags & (flag << BIT_SHIFT_FROM_COMPONENT_TO_DIFFERS_FLAG)) {
                        canUseSanitizedString = false;
                        break;
                    }
                }
            }
            if (canUseOriginalString) {
                CFRange rg = __CFURLGetComponentRange(anURL->_flags, anURL->ranges, firstRsrcSpecFlag);
                rg.location--;  // Include the character that demarcates the component
                rg.length = CFStringGetLength(anURL->_string) - rg.location;
                return CFStringCreateWithSubstring(alloc, anURL->_string, rg);
            } else if (canUseSanitizedString) {
                CFRange rg = __CFURLGetComponentRange(anURL->_flags, anURL->ranges, firstRsrcSpecFlag);
                rg.location--;  // Include the character that demarcates the component
                rg.length = CFStringGetLength(__CFURLGetSanitizedString(anURL)) - rg.location;
                return CFStringCreateWithSubstring(alloc, __CFURLGetSanitizedString(anURL), rg);
            } else {
                // Must compute the correct string to return; just reparse....
                UInt32 sanFlags = 0;
                CFRange* sanRanges = NULL;
                CFRange rg;
                __CFParseURLComponents(alloc, __CFURLGetSanitizedString(anURL), anURL->_base, &sanFlags, &sanRanges);
                rg = __CFURLGetComponentRange(sanFlags, sanRanges, firstRsrcSpecFlag);
                CFAllocatorDeallocate(alloc, sanRanges);
                rg.location--;  // Include the character that demarcates the component
                rg.length = CFStringGetLength(__CFURLGetSanitizedString(anURL)) - rg.location;
                return CFStringCreateWithSubstring(CFGetAllocator(anURL), __CFURLGetSanitizedString(anURL), rg);
            }
        } else {
            // The resource specifier cannot possibly come from the base.
            return NULL;
        }
    }
}

// For the next four methods, it is important to realize that, if a URL supplies any part of the net location (host, user, port, or password), it must supply all of the net location (i.e. none of it comes from its base URL).  Also, it is impossible for a URL to be relative, supply none of the net location, and still have its (empty) net location take precedence over its base URL (because there's nothing that precedes the net location except the scheme, and if the URL supplied the scheme, it would be absolute, and there would be no base).
CFStringRef  CFURLCopyHostName(CFURLRef anURL) {
    CFStringRef tmp;
    if (CF_IS_OBJC(anURL)) {
        CF_OBJC_CALL(CFStringRef, tmp, anURL, "host");
        if (tmp) {
            CFRetain(tmp);
        }
        return tmp;
    }
    CF_VALIDATE_OBJECT_ARG(CF, anURL, __kCFURLTypeID);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        if (anURL->_base) {
            return CFURLCopyHostName(anURL->_base);
        } else {
            CFRetain(kCFURLLocalhost);
            return kCFURLLocalhost;
        }
    }
    tmp = _retainedComponentString(anURL, HAS_HOST, true, true);
    if (tmp) {
        if (anURL->_flags & IS_IPV6_ENCODED) {
            // Have to strip off the brackets to get the true hostname.
            // Assume that to be legal the first and last characters are brackets!
            CFStringRef strippedHost = CFStringCreateWithSubstring(CFGetAllocator(anURL), tmp, CFRangeMake(1, CFStringGetLength(tmp) - 2));
            CFRelease(tmp);
            tmp = strippedHost;
        }
        return tmp;
    } else if (anURL->_base && !(anURL->_flags & NET_LOCATION_MASK) && !(anURL->_flags & HAS_SCHEME)) {
        return CFURLCopyHostName(anURL->_base);
    } else {
        return NULL;
    }
}

// Return -1 to indicate no port is specified
SInt32 CFURLGetPortNumber(CFURLRef anURL) {
    CFStringRef port;
    if (CF_IS_OBJC(anURL)) {
        CFNumberRef cfPort;
        CF_OBJC_CALL(CFNumberRef, cfPort, anURL, "port");
        SInt32 num;
        if (cfPort && CFNumberGetValue(cfPort, kCFNumberSInt32Type, &num)) {
            return num;
        }
        return -1;
    }
    CF_VALIDATE_OBJECT_ARG(CF, anURL, __kCFURLTypeID);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        if (anURL->_base) {
            return CFURLGetPortNumber(anURL->_base);
        }
        return -1;
    }
    port = _retainedComponentString(anURL, HAS_PORT, true, false);
    if (port) {
        SInt32 portNum, idx, length = CFStringGetLength(port);
        CFStringInlineBuffer buf;
        CFStringInitInlineBuffer(port, &buf, CFRangeMake(0, length));
        idx = 0;
        if (!_CFStringScanInteger(&buf, NULL, &idx, false, &portNum) || (idx != length)) {
            portNum = -1;
        }
        CFRelease(port);
        return portNum;
    } else if (anURL->_base && !(anURL->_flags & NET_LOCATION_MASK) && !(anURL->_flags & HAS_SCHEME)) {
        return CFURLGetPortNumber(anURL->_base);
    } else {
        return -1;
    }
}

CFStringRef  CFURLCopyUserName(CFURLRef anURL) {
    CFStringRef user;
    if (CF_IS_OBJC(anURL)) {
        CF_OBJC_CALL(CFStringRef, user, anURL, "user");
        if (user) {
            CFRetain(user);
        }
        return user;
    }
    CF_VALIDATE_OBJECT_ARG(CF, anURL, __kCFURLTypeID);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        if (anURL->_base) {
            return CFURLCopyUserName(anURL->_base);
        }
        return NULL;
    }
    user = _retainedComponentString(anURL, HAS_USER, true, true);
    if (user) {
        return user;
    } else if (anURL->_base && !(anURL->_flags & NET_LOCATION_MASK) && !(anURL->_flags & HAS_SCHEME)) {
        return CFURLCopyUserName(anURL->_base);
    } else {
        return NULL;
    }
}

CFStringRef  CFURLCopyPassword(CFURLRef anURL) {
    CFStringRef passwd;
    if (CF_IS_OBJC(anURL)) {
        CF_OBJC_CALL(CFStringRef, passwd, anURL, "password");
        if (passwd) {
            CFRetain(passwd);
        }
        return passwd;
    }
    CF_VALIDATE_OBJECT_ARG(CF, anURL, __kCFURLTypeID);
    if (URL_PATH_TYPE(anURL) != FULL_URL_REPRESENTATION) {
        if (anURL->_base) {
            return CFURLCopyPassword(anURL->_base);
        }
        return NULL;
    }
    passwd = _retainedComponentString(anURL, HAS_PASSWORD, true, true);
    if (passwd) {
        return passwd;
    } else if (anURL->_base && !(anURL->_flags & NET_LOCATION_MASK) && !(anURL->_flags & HAS_SCHEME)) {
        return CFURLCopyPassword(anURL->_base);
    } else {
        return NULL;
    }
}

CFStringRef  CFURLCopyParameterString(CFURLRef anURL, CFStringRef charactersToLeaveEscaped) {
    CFStringRef param = _unescapedParameterString(anURL);
    if (param) {
        CFStringRef result;
        if (anURL->_flags & IS_OLD_UTF8_STYLE || anURL->_encoding == kCFStringEncodingUTF8) {
            result = CFURLCreateStringByReplacingPercentEscapes(CFGetAllocator(anURL), param, charactersToLeaveEscaped);
        } else {
            result = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(CFGetAllocator(anURL), param, charactersToLeaveEscaped, anURL->_encoding);
        }
        CFRelease(param);
        return result;
    }
    return NULL;
}

CFStringRef  CFURLCopyQueryString(CFURLRef anURL, CFStringRef charactersToLeaveEscaped) {
    CFStringRef query = _unescapedQueryString(anURL);
    if (query) {
        CFStringRef tmp;
        if (anURL->_flags & IS_OLD_UTF8_STYLE || anURL->_encoding == kCFStringEncodingUTF8) {
            tmp = CFURLCreateStringByReplacingPercentEscapes(CFGetAllocator(anURL), query, charactersToLeaveEscaped);
        } else {
            tmp = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(CFGetAllocator(anURL), query, charactersToLeaveEscaped, anURL->_encoding);
        }
        CFRelease(query);
        return tmp;
    }
    return NULL;
}

CFStringRef  CFURLCopyFragment(CFURLRef anURL, CFStringRef charactersToLeaveEscaped) {
    CFStringRef fragment = _unescapedFragment(anURL);
    if (fragment) {
        CFStringRef tmp;
        if (anURL->_flags & IS_OLD_UTF8_STYLE || anURL->_encoding == kCFStringEncodingUTF8) {
            tmp = CFURLCreateStringByReplacingPercentEscapes(CFGetAllocator(anURL), fragment, charactersToLeaveEscaped);
        } else {
            tmp = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(CFGetAllocator(anURL), fragment, charactersToLeaveEscaped, anURL->_encoding);
        }
        CFRelease(fragment);
        return tmp;
    }
    return NULL;
}

CFRange CFURLGetByteRangeForComponent(CFURLRef url, CFURLComponentType component, CFRange* rangeIncludingSeparators) {
    CFRange charRange, charRangeWithSeparators;
    CFRange byteRange;
    CF_VALIDATE_ARG(component > 0 && component < 13, "passed invalid component %d", component);
    url = _CFURLFromNSURL(url);
    if (URL_PATH_TYPE(url) != FULL_URL_REPRESENTATION) {
        __CFURLConvertToFullRepresentation(url);
    }

    __CFURLEnsureComponentsParsed(url);

    if (!(url->_flags & IS_DECOMPOSABLE)) {
        // Special-case this because non-decomposable URLs have a slightly strange flags setup
        charRange = _getCharRangeInNonDecomposableURL(url, component, &charRangeWithSeparators);
    } else {
        charRange = _getCharRangeInDecomposableURL(url, component, &charRangeWithSeparators);
    }

    if (charRangeWithSeparators.location == kCFNotFound) {
        if (rangeIncludingSeparators) {
            rangeIncludingSeparators->location = kCFNotFound;
            rangeIncludingSeparators->length = 0;
        }
        return CFRangeMake(kCFNotFound, 0);
    } else if (rangeIncludingSeparators) {
        CFStringGetBytes(url->_string, CFRangeMake(0, charRangeWithSeparators.location), url->_encoding, 0, false, NULL, 0, &(rangeIncludingSeparators->location));

        if (charRange.location == kCFNotFound) {
            byteRange = charRange;
            CFStringGetBytes(url->_string, charRangeWithSeparators, url->_encoding, 0, false, NULL, 0, &(rangeIncludingSeparators->length));
        } else {
            CFIndex maxCharRange = charRange.location + charRange.length;
            CFIndex maxCharRangeWithSeparators = charRangeWithSeparators.location + charRangeWithSeparators.length;

            if (charRangeWithSeparators.location == charRange.location) {
                byteRange.location = rangeIncludingSeparators->location;
            } else {
                CFIndex numBytes;
                CFStringGetBytes(url->_string, CFRangeMake(charRangeWithSeparators.location, charRange.location - charRangeWithSeparators.location), url->_encoding, 0, false, NULL, 0, &numBytes);
                byteRange.location = charRangeWithSeparators.location + numBytes;
            }
            CFStringGetBytes(url->_string, charRange, url->_encoding, 0, false, NULL, 0, &(byteRange.length));
            if (maxCharRangeWithSeparators == maxCharRange) {
                rangeIncludingSeparators->length = byteRange.location + byteRange.length - rangeIncludingSeparators->location;
            } else {
                CFIndex numBytes;
                CFRange rg;
                rg.location = maxCharRange;
                rg.length = maxCharRangeWithSeparators - rg.location;
                CFStringGetBytes(url->_string, rg, url->_encoding, 0, false, NULL, 0, &numBytes);
                rangeIncludingSeparators->length = byteRange.location + byteRange.length + numBytes - rangeIncludingSeparators->location;
            }
        }
    } else if (charRange.location == kCFNotFound) {
        byteRange = charRange;
    } else {
        CFStringGetBytes(url->_string, CFRangeMake(0, charRange.location), url->_encoding, 0, false, NULL, 0, &(byteRange.location));
        CFStringGetBytes(url->_string, charRange, url->_encoding, 0, false, NULL, 0, &(byteRange.length));
    }
    return byteRange;
}

CFURLRef CFURLCreateWithFileSystemPath(CFAllocatorRef allocator, CFStringRef filePath, CFURLPathStyle fsType, Boolean isDirectory) {
    Boolean isAbsolute = true;
    CFIndex len;
    CFURLRef baseURL, result;

    CF_VALIDATE_ARG(
        fsType == kCFURLPOSIXPathStyle ||
        fsType == kCFURLHFSPathStyle ||
        fsType == kCFURLWindowsPathStyle,
        "invalid fsType %d", fsType);
    CF_VALIDATE_PTR_ARG(filePath);

    len = CFStringGetLength(filePath);

    switch (fsType) {
        case kCFURLPOSIXPathStyle:
            isAbsolute = (len > 0 && CFStringGetCharacterAtIndex(filePath, 0) == '/');
            break;
        case kCFURLWindowsPathStyle:
            isAbsolute = (len >= 3 && CFStringGetCharacterAtIndex(filePath, 1) == ':' && CFStringGetCharacterAtIndex(filePath, 2) == '\\');
            /* Absolute path under Win32 can begin with "\\"
             * (Sergey Zubarev)
             */
            if (!isAbsolute) {
                isAbsolute = (len > 2 && CFStringGetCharacterAtIndex(filePath, 0) == '\\' && CFStringGetCharacterAtIndex(filePath, 1) == '\\');
            }
            break;
        case kCFURLHFSPathStyle:
            isAbsolute = (len > 0 && CFStringGetCharacterAtIndex(filePath, 0) != ':');
            break;
    }
    if (isAbsolute) {
        baseURL = NULL;
    } else {
        baseURL = CFURLCreateWithFileSystemPath(
            allocator,
            CFPlatformGetCurrentDirectory(),
            CFPlatformGetURLPathStyle(),
            true);
    }
    result = CFURLCreateWithFileSystemPathRelativeToBase(allocator, filePath, fsType, isDirectory, baseURL);
    if (baseURL) {
        CFRelease(baseURL);
    }
    return result;
}

CFURLRef CFURLCreateWithFileSystemPathRelativeToBase(CFAllocatorRef allocator, CFStringRef filePath, CFURLPathStyle fsType, Boolean isDirectory, CFURLRef baseURL) {
    __CFURL* url;
    Boolean isAbsolute = true, releaseFilePath = false;
    UniChar pathDelim = '\0';
    CFIndex len;

    CF_VALIDATE_PTR_ARG(filePath);
    CF_VALIDATE_ARG(
        fsType == kCFURLPOSIXPathStyle ||
        fsType == kCFURLHFSPathStyle ||
        fsType == kCFURLWindowsPathStyle,
        "encountered unknown path style %d", fsType);

    len = CFStringGetLength(filePath);

    switch (fsType) {
        case kCFURLPOSIXPathStyle:
            isAbsolute = (len > 0 && CFStringGetCharacterAtIndex(filePath, 0) == '/');

            pathDelim = '/';
            break;
        case kCFURLWindowsPathStyle:
            isAbsolute = (len >= 3 && CFStringGetCharacterAtIndex(filePath, 1) == ':' && CFStringGetCharacterAtIndex(filePath, 2) == '\\');
            /* Absolute path under Win32 can begin with "\\"
             * (Sergey Zubarev)
             */
            if (!isAbsolute) {
                isAbsolute = (len > 2 && CFStringGetCharacterAtIndex(filePath, 0) == '\\' && CFStringGetCharacterAtIndex(filePath, 1) == '\\');
            }
            pathDelim = '\\';
            break;
        case kCFURLHFSPathStyle:
        {CFRange fullStrRange = CFRangeMake(0, CFStringGetLength(filePath) );

         isAbsolute = (len > 0 && CFStringGetCharacterAtIndex(filePath, 0) != ':');
         pathDelim = ':';

         if (filePath && CFStringFindWithOptions(filePath, CFSTR("::"), fullStrRange, 0, NULL)) {
             UniChar* chars = (UniChar*)malloc(fullStrRange.length * sizeof( UniChar ) );
             CFIndex index, writeIndex, firstColonOffset = -1;

             CFStringGetCharacters(filePath, fullStrRange, chars);

             for ( index = 0, writeIndex = 0; index < fullStrRange.length; index++) {
                 if (chars[ index ] == ':') {
                     if (index + 1 < fullStrRange.length && chars[ index + 1 ] == ':') {

                         // Don't let :: go off the 'top' of the path -- which means that there always has to be at
                         //    least one ':' to the left of the current write position to go back to.
                         if (writeIndex > 0 && firstColonOffset >= 0) {
                             writeIndex--;
                             while ( writeIndex > 0 && writeIndex >= firstColonOffset && chars[ writeIndex ] != ':') {
                                 writeIndex--;
                             }
                         }
                         index++;        // skip over the first ':', so we replace the ':' which is there with a new one
                     }

                     if (firstColonOffset == -1) {
                         firstColonOffset = writeIndex;
                     }
                 }

                 chars[ writeIndex++ ] = chars[ index ];
             }

             if (releaseFilePath && filePath) {
                 CFRelease(filePath);
             }

             filePath = CFStringCreateWithCharacters(allocator, chars, writeIndex);
             // reset len because a canonical HFS path can be a different length than the original CFString
             len = CFStringGetLength(filePath);
             releaseFilePath = true;

             free(chars);
         }

         break; }
    }
    if (isAbsolute) {
        baseURL = NULL;
    }

    if (isDirectory && len > 0 && CFStringGetCharacterAtIndex(filePath, len - 1) != pathDelim) {
        CFMutableStringRef tempRef = CFStringCreateMutable(allocator, 0);
        CFStringAppend(tempRef, filePath);
        CFStringAppendCharacters(tempRef, &pathDelim, 1);
        if (releaseFilePath && filePath) {
            CFRelease(filePath);
        }
        filePath = tempRef;
        releaseFilePath = true;
    } else if (!isDirectory && len > 0 && CFStringGetCharacterAtIndex(filePath, len - 1) == pathDelim) {
        if (len == 1 || CFStringGetCharacterAtIndex(filePath, len - 2) == pathDelim) {
            // Override isDirectory
            isDirectory = true;
        } else {
            CFStringRef tempRef = CFStringCreateWithSubstring(allocator, filePath, CFRangeMake(0, len - 1));
            if (releaseFilePath && filePath) {
                CFRelease(filePath);
            }
            filePath = tempRef;
            releaseFilePath = true;
        }
    }
    if (!filePath || CFStringGetLength(filePath) == 0) {
        if (releaseFilePath && filePath) {
            CFRelease(filePath);
        }
        return NULL;
    }
    url = __CFURLAlloc(allocator);
    __CFURLInit(url, filePath, fsType, baseURL);
    if (releaseFilePath) {
        CFRelease(filePath);
    }
    if (isDirectory) {
        url->_flags |= IS_DIRECTORY;
    }
    if (fsType == kCFURLPOSIXPathStyle) {
        // Check if relative path is equivalent to URL representation; this will be true if url->_string contains only characters from the unreserved character set, plus '/' to delimit the path, plus ';', '@', '&', '=', '+', '$', ',' (according to RFC 2396) -- REW, 12/1/2000
        // Per Section 5 of RFC 2396, there's a special problem if a colon apears in the first path segment - in this position, it can be mistaken for the scheme name.  Otherwise, it's o.k., and can be safely identified as part of the path.  In this one case, we need to prepend "./" to make it clear what's going on.... -- REW, 8/24/2001
        CFStringInlineBuffer buf;
        Boolean sawSlash = FALSE;
        Boolean mustPrependDotSlash = FALSE;
        CFIndex idx, length = CFStringGetLength(url->_string);
        CFStringInitInlineBuffer(url->_string, &buf, CFRangeMake(0, length));
        for (idx = 0; idx < length; idx++) {
            UniChar ch = CFStringGetCharacterFromInlineBuffer(&buf, idx);
            if (!isPathLegalCharacter(ch)) {
                break;
            }
            if (!sawSlash) {
                if (ch == '/') {
                    sawSlash = TRUE;
                } else if (ch == ':') {
                    mustPrependDotSlash = TRUE;
                }
            }
        }
        if (idx == length) {
            url->_flags |= POSIX_AND_URL_PATHS_MATCH;
        }
        if (mustPrependDotSlash) {
            CFMutableStringRef newString = CFStringCreateMutable(allocator, 0);
            CFStringAppend(newString, CFSTR("./"));
            CFStringAppend(newString, url->_string);
            CFRelease(url->_string);
            url->_string = newString;
        }
    }
    return url;
}

CFStringRef CFURLCopyFileSystemPath(CFURLRef anURL, CFURLPathStyle pathStyle) {
    CF_VALIDATE_ARG(
        pathStyle == kCFURLPOSIXPathStyle ||
        pathStyle == kCFURLHFSPathStyle ||
        pathStyle == kCFURLWindowsPathStyle,
        "Encountered unknown path style %d", pathStyle);
    return __CFURLCreateStringWithFileSystemPath(CFGetAllocator(anURL), anURL, pathStyle, false);
}

Boolean CFURLGetFileSystemRepresentation(CFURLRef url, Boolean resolveAgainstBase, uint8_t* buffer, CFIndex bufLen) {
    CFStringRef path;
    CFAllocatorRef alloc = CFGetAllocator(url);

    if (!url) {
        return false;
    }
    path = __CFURLCreateStringWithFileSystemPath(alloc, url, CFPlatformGetURLPathStyle(), resolveAgainstBase);
    if (path) {
        Boolean convResult = CFStringGetFileSystemRepresentation(path, (char*)buffer, bufLen);
        CFRelease(path);
        return convResult;
    }
    return false;
}

CFURLRef CFURLCreateFromFileSystemRepresentation(CFAllocatorRef allocator, const uint8_t* buffer, CFIndex bufLen, Boolean isDirectory) {
    CFStringRef path = CFStringCreateWithBytes(allocator, buffer, bufLen, CFStringFileSystemEncoding(), false);
    CFURLRef newURL;
    if (!path) {
        return NULL;
    }
    newURL = CFURLCreateWithFileSystemPath(allocator, path, CFPlatformGetURLPathStyle(), isDirectory);
    CFRelease(path);
    return newURL;
}

CFURLRef CFURLCreateFromFileSystemRepresentationRelativeToBase(CFAllocatorRef allocator, const uint8_t* buffer, CFIndex bufLen, Boolean isDirectory, CFURLRef baseURL) {
    CFStringRef path = CFStringCreateWithBytes(allocator, buffer, bufLen, CFStringFileSystemEncoding(), false);
    CFURLRef newURL;
    if (!path) {
        return NULL;
    }
    newURL = CFURLCreateWithFileSystemPathRelativeToBase(allocator, path, CFPlatformGetURLPathStyle(), isDirectory, baseURL);
    CFRelease(path);
    return newURL;
}

CFStringRef CFURLCopyLastPathComponent(CFURLRef url) {
    CFStringRef result;

    if (CF_IS_OBJC(url)) {
        CFStringRef path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
        CFIndex length;
        CFRange rg, compRg;
        if (!path) {
            return NULL;
        }
        rg = CFRangeMake(0, CFStringGetLength(path));
        length = rg.length; // Remember this for comparison later
        if (CFStringGetCharacterAtIndex(path, rg.length - 1) == '/') {
            rg.length--;
        }
        if (CFStringFindWithOptions(path, CFSTR("/"), rg, kCFCompareBackwards, &compRg)) {
            rg.length = rg.location + rg.length - (compRg.location + 1);
            rg.location = compRg.location + 1;
        }
        if (rg.location == 0 && rg.length == length) {
            result = path;
        } else {
            result = CFStringCreateWithSubstring(CFGetAllocator(url), path, rg);
            CFRelease(path);
        }
    } else {
        CFRange rg = _rangeOfLastPathComponent(url);
        if (rg.location == kCFNotFound || rg.length == 0) {
            // No path
            return (CFStringRef)CFRetain(CFSTR(""));
        }
        if (rg.length == 1 && CFStringGetCharacterAtIndex(url->_string, rg.location) == PATH_DELIM_FOR_TYPE(URL_PATH_TYPE(url))) {
            return (CFStringRef)CFRetain(CFSTR("/"));
        }
        result = CFStringCreateWithSubstring(CFGetAllocator(url), url->_string, rg);
        if (URL_PATH_TYPE(url) == FULL_URL_REPRESENTATION && !(url->_flags & POSIX_AND_URL_PATHS_MATCH)) {
            CFStringRef tmp;
            if (url->_flags & IS_OLD_UTF8_STYLE || url->_encoding == kCFStringEncodingUTF8) {
                tmp = CFURLCreateStringByReplacingPercentEscapes(CFGetAllocator(url), result, CFSTR(""));
            } else {
                tmp = CFURLCreateStringByReplacingPercentEscapesUsingEncoding(CFGetAllocator(url), result, CFSTR(""), url->_encoding);
            }
            CFRelease(result);
            result = tmp;
        }
    }
    return result;
}

CFStringRef CFURLCopyPathExtension(CFURLRef url) {
    CFStringRef lastPathComp = CFURLCopyLastPathComponent(url);
    CFStringRef ext = NULL;

    if (lastPathComp) {
        CFRange rg = CFStringFind(lastPathComp, CFSTR("."), kCFCompareBackwards);
        if (rg.location != kCFNotFound) {
            rg.location++;
            rg.length = CFStringGetLength(lastPathComp) - rg.location;
            if (rg.length > 0) {
                ext = CFStringCreateWithSubstring(CFGetAllocator(url), lastPathComp, rg);
            } else {
                ext = (CFStringRef)CFRetain(CFSTR(""));
            }
        }
        CFRelease(lastPathComp);
    }
    return ext;
}

CFURLRef CFURLCreateCopyAppendingPathComponent(CFAllocatorRef allocator, CFURLRef url, CFStringRef pathComponent, Boolean isDirectory) {
    UInt32 fsType;
    CFURLRef result;
    url = _CFURLFromNSURL(url);
    CF_VALIDATE_OBJECT_ARG(CF, url, __kCFURLTypeID);
    CF_VALIDATE_PTR_ARG(pathComponent);

    fsType = URL_PATH_TYPE(url);
    if (fsType != FULL_URL_REPRESENTATION && CFStringFindWithOptions(pathComponent, PATH_DELIM_AS_STRING_FOR_TYPE(fsType), CFRangeMake(0, CFStringGetLength(pathComponent)), 0, NULL)) {
        // Must convert to full representation, and then work with it
        fsType = FULL_URL_REPRESENTATION;
        __CFURLConvertToFullRepresentation(url);
    }

    if (fsType == FULL_URL_REPRESENTATION) {
        CFMutableStringRef newString;
        CFStringRef newComp;
        CFRange pathRg;
        __CFURLEnsureComponentsParsed(url);
        if (!(url->_flags & HAS_PATH)) {
            return NULL;
        }

        newString = CFStringCreateMutableCopy(allocator, 0, url->_string);
        newComp = CFURLCreateStringByAddingPercentEscapes(allocator, pathComponent, NULL, CFSTR(";?"),  (url->_flags & IS_OLD_UTF8_STYLE) ? kCFStringEncodingUTF8 : url->_encoding);
        pathRg = __CFURLGetComponentRange(url->_flags, url->ranges, HAS_PATH);
        if (!pathRg.length || CFStringGetCharacterAtIndex(url->_string, pathRg.location + pathRg.length - 1) != '/') {
            CFStringInsert(newString, pathRg.location + pathRg.length, CFSTR("/"));
            pathRg.length++;
        }
        CFStringInsert(newString, pathRg.location + pathRg.length, newComp);
        if (isDirectory) {
            CFStringInsert(newString, pathRg.location + pathRg.length + CFStringGetLength(newComp), CFSTR("/"));
        }
        CFRelease(newComp);
        result = __CFURLCreateWithArbitraryString(allocator, newString, url->_base);
        CFRelease(newString);
    } else {
        UniChar pathDelim = PATH_DELIM_FOR_TYPE(fsType);
        CFStringRef newString;
        if (CFStringGetCharacterAtIndex(url->_string, CFStringGetLength(url->_string) - 1) != pathDelim) {
            if (isDirectory) {
                newString = CFStringCreateWithFormat(allocator, NULL, CFSTR("%@%c%@%c"), url->_string, pathDelim, pathComponent, pathDelim);
            } else {
                newString = CFStringCreateWithFormat(allocator, NULL, CFSTR("%@%c%@"), url->_string, pathDelim, pathComponent);
            }
        } else {
            if (isDirectory) {
                newString = CFStringCreateWithFormat(allocator, NULL, CFSTR("%@%@%c"), url->_string, pathComponent, pathDelim);
            } else {
                newString = CFStringCreateWithFormat(allocator, NULL, CFSTR("%@%@"), url->_string, pathComponent);
            }
        }
        result = CFURLCreateWithFileSystemPathRelativeToBase(allocator, newString, fsType, isDirectory, url->_base);
        CFRelease(newString);
    }
    return result;
}

CFURLRef CFURLCreateCopyDeletingLastPathComponent(CFAllocatorRef allocator, CFURLRef url) {
    CFURLRef result;
    CFMutableStringRef newString;
    CFRange lastCompRg, pathRg;
    Boolean appendDotDot = false;
    UInt32 fsType;

    url = _CFURLFromNSURL(url);
    CF_VALIDATE_PTR_ARG(url);
    CF_VALIDATE_OBJECT_ARG(CF, url, __kCFURLTypeID);

    fsType = URL_PATH_TYPE(url);
    if (fsType == FULL_URL_REPRESENTATION) {
        __CFURLEnsureComponentsParsed(url);
        if (!(url->_flags & HAS_PATH)) {
            return NULL;
        }
        pathRg = __CFURLGetComponentRange(url->_flags, url->ranges, HAS_PATH);
    } else {
        pathRg = CFRangeMake(0, CFStringGetLength(url->_string));
    }
    lastCompRg = _rangeOfLastPathComponent(url);
    if (lastCompRg.length == 0) {
        appendDotDot = true;
    } else if (lastCompRg.length == 1) {
        UniChar ch = CFStringGetCharacterAtIndex(url->_string, lastCompRg.location);
        if (ch == '.' || ch == PATH_DELIM_FOR_TYPE(fsType)) {
            appendDotDot = true;
        }
    } else if (lastCompRg.length == 2 && 
        CFStringGetCharacterAtIndex(url->_string, lastCompRg.location) == '.' &&
        CFStringGetCharacterAtIndex(url->_string, lastCompRg.location + 1) == '.')
    {
        appendDotDot = true;
    }

    newString = CFStringCreateMutableCopy(allocator, 0, url->_string);
    if (appendDotDot) {
        CFIndex delta = 0;
        if (pathRg.length > 0 && CFStringGetCharacterAtIndex(url->_string, pathRg.location + pathRg.length - 1) != PATH_DELIM_FOR_TYPE(fsType)) {
            CFStringInsert(newString, pathRg.location + pathRg.length, PATH_DELIM_AS_STRING_FOR_TYPE(fsType));
            delta++;
        }
        CFStringInsert(newString, pathRg.location + pathRg.length + delta, CFSTR(".."));
        delta += 2;
        CFStringInsert(newString, pathRg.location + pathRg.length + delta, PATH_DELIM_AS_STRING_FOR_TYPE(fsType));
        delta++;
        // We know we have "/../" at the end of the path; we wish to know if that's immediately preceded by "/." (but that "/." doesn't start the string), in which case we want to delete the "/.".
        if (pathRg.length + delta > 4 && CFStringGetCharacterAtIndex(newString, pathRg.location + pathRg.length + delta - 5) == '.') {
            if (pathRg.length + delta > 7 && CFStringGetCharacterAtIndex(newString, pathRg.location + pathRg.length + delta - 6) == PATH_DELIM_FOR_TYPE(fsType)) {
                CFStringDelete(newString, CFRangeMake(pathRg.location + pathRg.length + delta - 6, 2));
            } else if (pathRg.length + delta == 5) {
                CFStringDelete(newString, CFRangeMake(pathRg.location + pathRg.length + delta - 5, 2));
            }
        }
    } else if (lastCompRg.location == pathRg.location) {
        CFStringReplace(newString, pathRg, CFSTR("."));
        CFStringInsert(newString, 1, PATH_DELIM_AS_STRING_FOR_TYPE(fsType));
    } else {
        CFStringDelete(newString, CFRangeMake(lastCompRg.location, pathRg.location + pathRg.length - lastCompRg.location));
    }
    if (fsType == FULL_URL_REPRESENTATION) {
        result = __CFURLCreateWithArbitraryString(allocator, newString, url->_base);
    } else {
        result = CFURLCreateWithFileSystemPathRelativeToBase(allocator, newString, fsType, true, url->_base);
    }
    CFRelease(newString);
    return result;
}

CFURLRef CFURLCreateCopyAppendingPathExtension(CFAllocatorRef allocator, CFURLRef url, CFStringRef extension) {
    CFMutableStringRef newString;
    CFURLRef result;
    CFRange rg;
    CFURLPathStyle fsType;

    CF_VALIDATE_PTR_ARG(url);
    url = _CFURLFromNSURL(url);
    CF_VALIDATE_OBJECT_ARG(CF, url, __kCFURLTypeID);
    CF_VALIDATE_OBJECT_ARG(CF, extension, CFStringGetTypeID());

    rg = _rangeOfLastPathComponent(url);
    if (rg.location < 0) {
        return NULL; // No path
    }
    fsType = URL_PATH_TYPE(url);
    if (fsType != FULL_URL_REPRESENTATION &&
        CFStringFindWithOptions(
            extension,
            PATH_DELIM_AS_STRING_FOR_TYPE(fsType),
            CFRangeMake(0, CFStringGetLength(extension)), 0, NULL))
    {
        __CFURLConvertToFullRepresentation(url);
        fsType = FULL_URL_REPRESENTATION;
        rg = _rangeOfLastPathComponent(url);
    }

    newString = CFStringCreateMutableCopy(allocator, 0, url->_string);
    CFStringInsert(newString, rg.location + rg.length, CFSTR("."));
    if (fsType == FULL_URL_REPRESENTATION) {
        CFStringRef newExt = CFURLCreateStringByAddingPercentEscapes(
            allocator,
            extension,
            NULL,
            CFSTR(";?/"),
            (url->_flags & IS_OLD_UTF8_STYLE) ? kCFStringEncodingUTF8 : url->_encoding);
        CFStringInsert(newString, rg.location + rg.length + 1, newExt);
        CFRelease(newExt);
        result = __CFURLCreateWithArbitraryString(allocator, newString, url->_base);
    } else {
        CFStringInsert(newString, rg.location + rg.length + 1, extension);
        result = CFURLCreateWithFileSystemPathRelativeToBase(
            allocator,
            newString,
            fsType,
            (url->_flags & IS_DIRECTORY) != 0,
            url->_base);
    }
    CFRelease(newString);
    return result;
}

CFURLRef CFURLCreateCopyDeletingPathExtension(CFAllocatorRef allocator, CFURLRef url) {
    CFRange rg, dotRg;
    CFURLRef result;

    CF_VALIDATE_PTR_ARG(url);
    url = _CFURLFromNSURL(url);
    CF_VALIDATE_OBJECT_ARG(CF, url, __kCFURLTypeID);
    rg = _rangeOfLastPathComponent(url);
    if (rg.location < 0) {
        result = NULL;
    } else if (rg.length && CFStringFindWithOptions(url->_string, CFSTR("."), rg, kCFCompareBackwards, &dotRg)) {
        CFMutableStringRef newString = CFStringCreateMutableCopy(allocator, 0, url->_string);
        dotRg.length = rg.location + rg.length - dotRg.location;
        CFStringDelete(newString, dotRg);
        if (URL_PATH_TYPE(url) == FULL_URL_REPRESENTATION) {
            result = __CFURLCreateWithArbitraryString(allocator, newString, url->_base);
        } else {
            result = CFURLCreateWithFileSystemPathRelativeToBase(
                allocator,
                newString,
                URL_PATH_TYPE(url),
                (url->_flags & IS_DIRECTORY) != 0,
                url->_base);
        }
        CFRelease(newString);
    } else {
        result = (CFURLRef)CFRetain(url);
    }
    return result;
}

void CFShowURL(CFURLRef url) {
    if (!url) {
        fprintf(stdout, "(null)\n");
        return;
    }
    fprintf(stdout, "<CFURL %p>{", (const void*)url);
    if (CF_IS_OBJC(url)) {
        fprintf(stdout, "ObjC bridged object}\n");
        return;
    }
    fprintf(stdout, "\n\tPath type: ");
    switch (URL_PATH_TYPE(url)) {
        case kCFURLPOSIXPathStyle:
            fprintf(stdout, "POSIX");
            break;
        case kCFURLHFSPathStyle:
            fprintf(stdout, "HFS");
            break;
        case kCFURLWindowsPathStyle:
            fprintf(stdout, "NTFS");
            break;
        case FULL_URL_REPRESENTATION:
            fprintf(stdout, "Native URL");
            break;
        default:
            fprintf(stdout, "UNRECOGNIZED PATH TYPE %d", (char)URL_PATH_TYPE(url));
    }
    fprintf(stdout, "\n\tRelative string: ");
    CFShow(url->_string);
    fprintf(stdout, "\tBase URL: ");
    if (url->_base) {
        fprintf(stdout, "<%p> ", (const void*)url->_base);
        CFShow(url->_base);
    } else {
        fprintf(stdout, "(null)\n");
    }
    fprintf(stdout, "\tFlags: 0x%x\n}\n", (unsigned int)url->_flags);
}

