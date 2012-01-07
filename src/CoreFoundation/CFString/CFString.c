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

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFDictionary.h>
#include "CFStringEncoding.h"
#include "CFStringEncodingConverter.h"
#include "CFUniChar.h"
#include "CFStringInternal.h"
#include "CFInternal.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "CFString_Common.h"

typedef Boolean (*UNI_CHAR_FUNC)(UInt32 flags, UInt8 ch, UniChar* unicodeChar);

#if defined(DEBUG)
// We put this into C & Pascal strings if we can't convert
#define CONVERSIONFAILURESTR "CFString conversion failed"
#endif

// Two constant strings used by CFString; these are initialized in CFStringInitialize
CONST_STRING_DECL(kCFEmptyString, "")


/* System Encoding.
 */
static CFStringEncoding __CFDefaultSystemEncoding = kCFStringEncodingInvalidId;
static CFStringEncoding __CFDefaultFileSystemEncoding = kCFStringEncodingInvalidId;
static CFStringEncoding __CFDefaultEightBitStringEncoding = kCFStringEncodingInvalidId;

#define ALLOCATORSFREEFUNC ((CFAllocatorRef) - 1)

#define SURROGATE_START 0xD800
#define SURROGATE_END 0xDFFF

//TODO Rename, move to UniChar.h
/* Line range code */
#define CarriageReturn '\r'    /* 0x0d */
#define NewLine '\n'        /* 0x0a */
#define NextLine 0x0085
#define LineSeparator 0x2028
#define ParaSeparator 0x2029

///////////////////////////////////////////////////////////////////// private

CF_INLINE Boolean __CFStringIsLineSeparator(UniChar ch, Boolean includeLineEndings) {
    /* Quick test to cover most chars */
    if (ch > CarriageReturn && ch < NextLine) {
        return false;
    }
    return (ch == NewLine) ||
           (ch == CarriageReturn) ||
           (ch == ParaSeparator) ||
           (includeLineEndings && (ch == NextLine || ch == LineSeparator));
}

/* Returns whether the indicated encoding can be stored in 8-bit chars
 */
CF_INLINE Boolean __CFStrEncodingCanBeStoredInEightBit(CFStringEncoding encoding) {
    switch (encoding & 0xFFF) { // just use encoding base
        case kCFStringEncodingInvalidId:
        case kCFStringEncodingUnicode:
        case kCFStringEncodingNonLossyASCII:
            return false;

        case kCFStringEncodingMacRoman:
        case kCFStringEncodingWindowsLatin1:
        case kCFStringEncodingISOLatin1:
        case kCFStringEncodingNextStepLatin:
        case kCFStringEncodingASCII:
            return true;

        default: return false;
    }
}

/* Returns the encoding used in eight bit CFStrings (can't be any encoding which isn't 1-to-1 with Unicode)
 * ??? Perhaps only ASCII fits the bill due to Unicode decomposition.
 */
static CFStringEncoding __CFStringComputeEightBitStringEncoding(void) {
    if (__CFDefaultEightBitStringEncoding == kCFStringEncodingInvalidId) {
        CFStringEncoding systemEncoding = CFStringGetSystemEncoding();
        if (systemEncoding == kCFStringEncodingInvalidId) { // We're right in the middle of querying system encoding from default database. Delaying to set until system encoding is determined.
            return kCFStringEncodingASCII;
        } else if (__CFStrEncodingCanBeStoredInEightBit(systemEncoding)) {
            __CFDefaultEightBitStringEncoding = systemEncoding;
        } else {
            __CFDefaultEightBitStringEncoding = kCFStringEncodingASCII;
        }
    }

    return __CFDefaultEightBitStringEncoding;
}

/* Returns whether the provided bytes can be stored in ASCII
 */
CF_INLINE Boolean __CFBytesInASCII(const uint8_t* bytes, CFIndex len) {
    while (len--) {
        if ((uint8_t)(*bytes++) >= 128) {
            return false;
        }
    }
    return true;
}

/* Returns whether the provided 8-bit string in the specified encoding can be stored in an 8-bit CFString.
 */
CF_INLINE Boolean __CFCanUseEightBitCFStringForBytes(const uint8_t* bytes, CFIndex len, CFStringEncoding encoding) {
    if (encoding == __CFStringGetEightBitStringEncoding()) {
        return true;
    }
    if (__CFStringEncodingIsSupersetOfASCII(encoding) && __CFBytesInASCII(bytes, len)) {
        return true;
    }
    return false;
}



/**** CFString class ****/

static void __CFStringDeallocate(CFTypeRef cf) {
    CFStringRef str = (CFStringRef)cf;

    CF_ASSERT_XXX(!_CFStringIsConstantString(str),
        "deallocated CFSTR(\"%@\")", str);

    if (!__CFStrIsInline(str)) {
        uint8_t* contents;
        Boolean isMutable = __CFStrIsMutable(str);
        if (__CFStrFreeContentsWhenDone(str) && (contents = (uint8_t*)__CFStrContents(str))) {
            if (isMutable) {
                __CFStrDeallocateMutableContents(CF_CONST_CAST(CFMutableStringRef, str), contents);
            } else {
                if (__CFStrHasContentsDeallocator(str)) {
                    CFAllocatorRef contentsDeallocator = __CFStrContentsDeallocator(str);
                    CFAllocatorDeallocate(contentsDeallocator, contents);
                    CFRelease(contentsDeallocator);
                } else {
                    CFAllocatorRef alloc = CFGetAllocator(str);
                    CFAllocatorDeallocate(alloc, contents);
                }
            }
        }
        if (isMutable && __CFStrHasContentsAllocator(str)) {
            CFRelease(__CFStrContentsAllocator(CF_CONST_CAST(CFMutableStringRef, str)));
        }
    }
}

static Boolean __CFStringEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFStringRef str1 = (CFStringRef)cf1;
    CFStringRef str2 = (CFStringRef)cf2;
    const uint8_t* contents1;
    const uint8_t* contents2;
    CFIndex len1;

    /* !!! We do not need IsString assertions, as the CFBase runtime assures this */
    /* !!! We do not need == test, as the CFBase runtime assures this */

    contents1 = (uint8_t*)__CFStrContents(str1);
    contents2 = (uint8_t*)__CFStrContents(str2);
    len1 = __CFStrLength2(str1, contents1);

    if (len1 != __CFStrLength2(str2, contents2)) {
        return false;
    }

    contents1 += __CFStrSkipAnyLengthByte(str1);
    contents2 += __CFStrSkipAnyLengthByte(str2);

    if (__CFStrIsEightBit(str1) && __CFStrIsEightBit(str2)) {
        return memcmp((const char*)contents1, (const char*)contents2, len1) ? false : true;
    } else if (__CFStrIsEightBit(str1)) {    /* One string has Unicode contents */
        CFStringInlineBuffer buf;
        CFIndex buf_idx = 0;

        CFStringInitInlineBuffer(str1, &buf, CFRangeMake(0, len1));
        for (buf_idx = 0; buf_idx < len1; buf_idx++) {
            if (CFStringGetCharacterFromInlineBuffer(&buf, buf_idx) != ((UniChar*)contents2)[buf_idx]) {
                return false;
            }
        }
    } else if (__CFStrIsEightBit(str2)) {    /* One string has Unicode contents */
        CFStringInlineBuffer buf;
        CFIndex buf_idx = 0;

        CFStringInitInlineBuffer(str2, &buf, CFRangeMake(0, len1));
        for (buf_idx = 0; buf_idx < len1; buf_idx++) {
            if (CFStringGetCharacterFromInlineBuffer(&buf, buf_idx) != ((UniChar*)contents1)[buf_idx]) {
                return false;
            }
        }
    } else {                    /* Both strings have Unicode contents */
        CFIndex idx;
        for (idx = 0; idx < len1; idx++) {
            if (((UniChar*)contents1)[idx] != ((UniChar*)contents2)[idx]) {
                return false;
            }
        }
    }
    return true;
}

static CFHashCode __CFStringHash(CFTypeRef cf) {
    /* !!! We do not need an IsString assertion here, as this is called by the CFBase runtime only */
    CFStringRef str = (CFStringRef)cf;
    const uint8_t* contents = (uint8_t*)__CFStrContents(str);
    CFIndex len = __CFStrLength2(str, contents);

    if (__CFStrIsEightBit(str)) {
        contents += __CFStrSkipAnyLengthByte(str);
        return __CFStrHashEightBit(contents, len);
    } else {
        return __CFStrHashCharacters((const UniChar*)contents, len, len);
    }
}

static CFStringRef __CFStringCopyDescription(CFTypeRef cf) {
    return CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("<CFString %p [%p]>{contents = \"%@\"}"), cf, CFGetAllocator(cf), cf);
}

static CFStringRef __CFStringCopyFormattingDescription(CFTypeRef cf, CFDictionaryRef formatOptions) {
    return (CFStringRef)CFStringCreateCopy(CFGetAllocator(cf), (CFStringRef)cf);
}

static const CFRuntimeClass __CFStringClass = {
    0,
    "CFString",
    NULL,      // init
    (CFRuntimeCopyCallback)CFStringCreateCopy,
    __CFStringDeallocate,
    __CFStringEqual,
    __CFStringHash,
    __CFStringCopyFormattingDescription,
    __CFStringCopyDescription
};


/**** ^^ CFString class ****/


static void __CFStringGetLineOrParagraphBounds(CFStringRef string, CFRange range, CFIndex* lineBeginIndex, CFIndex* lineEndIndex, CFIndex* contentsEndIndex, Boolean includeLineEndings) {
    CFIndex len;
    CFStringInlineBuffer buf;
    UniChar ch;

    CF_VALIDATE_STRING_ARG(string);
    CF_VALIDATE_STRING_RANGE_ARG(string, range);

    len = __CFStrLength(string);

    if (lineBeginIndex) {
        CFIndex start;
        if (range.location == 0) {
            start = 0;
        } else {
            CFStringInitInlineBuffer(string, &buf, CFRangeMake(0, len));
            CFIndex buf_idx = range.location;

            /* Take care of the special case where start happens to fall right between \r and \n */
            ch = CFStringGetCharacterFromInlineBuffer(&buf, buf_idx);
            buf_idx--;
            if ((ch == NewLine) && (CFStringGetCharacterFromInlineBuffer(&buf, buf_idx) == CarriageReturn)) {
                buf_idx--;
            }
            while (1) {
                if (buf_idx < 0) {
                    start = 0;
                    break;
                } else if (__CFStringIsLineSeparator(CFStringGetCharacterFromInlineBuffer(&buf, buf_idx), includeLineEndings)) {
                    start = buf_idx + 1;
                    break;
                } else {
                    buf_idx--;
                }
            }
        }
        *lineBeginIndex = start;
    }

    /* Now find the ending point */
    if (lineEndIndex || contentsEndIndex) {
        CFIndex endOfContents, lineSeparatorLength = 1;    /* 1 by default */
        CFStringInitInlineBuffer(string, &buf, CFRangeMake(0, len));
        CFIndex buf_idx = range.location + range.length - (range.length ? 1 : 0);
        /* First look at the last char in the range (if the range is zero length, the char after the range) to see if we're already on or within a end of line sequence... */
        ch = CFStringGetCharacterFromInlineBuffer(&buf, buf_idx);
        if (ch == NewLine) {
            endOfContents = buf_idx;
            buf_idx--;
            if (CFStringGetCharacterFromInlineBuffer(&buf, buf_idx) == CarriageReturn) {
                lineSeparatorLength = 2;
                endOfContents--;
            }
        } else {
            while (1) {
                if (__CFStringIsLineSeparator(ch, includeLineEndings)) {
                    endOfContents = buf_idx;    /* This is actually end of contentsRange */
                    buf_idx++;    /* OK for this to go past the end */
                    if ((ch == CarriageReturn) &&
                        (CFStringGetCharacterFromInlineBuffer(&buf, buf_idx) == NewLine))
                    {
                        lineSeparatorLength = 2;
                    }
                    break;
                } else if (buf_idx >= len) {
                    endOfContents = len;
                    lineSeparatorLength = 0;
                    break;
                } else {
                    buf_idx++;
                    ch = CFStringGetCharacterFromInlineBuffer(&buf, buf_idx);
                }
            }
        }
        if (contentsEndIndex) {
            *contentsEndIndex = endOfContents;
        }
        if (lineEndIndex) {
            *lineEndIndex = endOfContents + lineSeparatorLength;
        }
    }
}

static const void* __rangeRetain(CFAllocatorRef allocator, const void* ptr) {
    CFRetain(*(CFDataRef*)((uint8_t*)ptr + sizeof(CFRange)));
    return ptr;
}

static void __rangeRelease(CFAllocatorRef allocator, const void* ptr) {
    CFRelease(*(CFDataRef*)((uint8_t*)ptr + sizeof(CFRange)));
}

static CFStringRef __rangeCopyDescription(const void* ptr) {
    CFRange range = *(CFRange*)ptr;
    return CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("{%d, %d}"), range.location, range.length);
}

static Boolean    __rangeEqual(const void* ptr1, const void* ptr2) {
    CFRange range1 = *(CFRange*)ptr1;
    CFRange range2 = *(CFRange*)ptr2;
    return (range1.location == range2.location) && (range1.length == range2.length);
}

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL const char* _CFStrGetLanguageIdentifierForLocale(CFLocaleRef locale) {
    CFStringRef collatorID;
    const char* langID = NULL;
    static const void* lastLocale = NULL;
    static const char* lastLangID = NULL;
    static CFSpinLock_t lock = CFSpinLockInit;

    CFSpinLock(&lock);
    if ((lastLocale) && (lastLocale == locale)) {
        CFSpinUnlock(&lock);
        return lastLangID;
    }
    CFSpinUnlock(&lock);

    collatorID = (CFStringRef)CFLocaleGetValue(locale, _kCFLocaleCollatorID);

    // This is somewhat depending on CFLocale implementation always creating CFString for locale identifer ???
    if (__CFStrLength(collatorID) > 1) {
        const void* contents = __CFStrContents(collatorID);
        const char* string;
        char buffer[2];

        if (__CFStrIsEightBit(collatorID)) {
            string = ((const char*)contents) + __CFStrSkipAnyLengthByte(collatorID);
        } else {
            const UTF16Char* characters = (const UTF16Char*)contents;

            buffer[0] = (char)*(characters++);
            buffer[1] = (char)*characters;
            string = buffer;
        }

        if (!strncmp(string, "az", 2)) { // Azerbaijani
            langID = "az";
        } else if (!strncmp(string, "lt", 2)) { // Lithuanian
            langID = "lt";
        } else if (!strncmp(string, "tr", 2)) { // Turkish
            langID = "tr";
        }
    }

    CFSpinLock(&lock);
    lastLocale = locale;
    lastLangID = langID;
    CFSpinUnlock(&lock);

    return langID;
}

CF_INTERNAL bool _CFCanUseLocale(CFLocaleRef locale) {
    static int8_t __CFCheckLocaleCFType = -1;

    if (locale) {
        if (__CFCheckLocaleCFType < 0) {
            __CFCheckLocaleCFType = false;
        }
        if (!__CFCheckLocaleCFType || (CFGetTypeID(locale) == CFLocaleGetTypeID())) {
            return true;
        }
    }
    return false;
}

CF_INTERNAL CFStringEncoding __CFStringGetEightBitStringEncoding(void) {
    if (__CFDefaultEightBitStringEncoding == kCFStringEncodingInvalidId) {
        __CFStringComputeEightBitStringEncoding();
    }
    return __CFDefaultEightBitStringEncoding;
}

CF_INTERNAL void __CFStringInitialize(void) {
    CFTypeID typeID = _CFRuntimeRegisterClassBridge2(
        &__CFStringClass,
        "NSCFString", "NSCFMutableString");
    if (typeID != _kCFStringTypeID) {
        CF_FATAL_ERROR("CFString's type ID must be %d!", _kCFStringTypeID);
    }
}

/* contentsDeallocator indicates how to free the data if it's noCopy == true:
 *  kCFAllocatorNull: don't free
 *  ALLOCATORSFREEFUNC: free with main allocator's free func (don't pass in the real func ptr here)
 *  NULL: default allocator
 *  otherwise it's the allocator that should be used (it will be explicitly stored)
 * if noCopy == false, then freeFunc should be ALLOCATORSFREEFUNC
 * hasLengthByte, hasNullByte: refers to bytes; used only if encoding != Unicode
 * possiblyExternalFormat indicates that the bytes might have BOM and be swapped
 * tryToReduceUnicode means that the Unicode should be checked to see if it contains just ASCII (and reduce it if so)
 * numBytes contains the actual number of bytes in "bytes", including Length byte,
 *  BUT not the NULL byte at the end
 * bytes should not contain BOM characters
 * !!! Various flags should be combined to reduce number of arguments, if possible
 */
CF_INTERNAL
CFStringRef __CFStringCreateImmutableFunnel(CFAllocatorRef alloc,
                                            const void* bytes, CFIndex numBytes,
                                            CFStringEncoding encoding,
                                            Boolean possiblyExternalFormat,
                                            Boolean tryToReduceUnicode,
                                            Boolean hasLengthByte,
                                            Boolean hasNullByte,
                                            Boolean noCopy,
                                            CFAllocatorRef contentsDeallocator,
                                            UInt32 converterFlags)
{
    CF_VALIDATE_STRINGENCODING_ARG(encoding);
    
    CFMutableStringRef str;
    CFVarWidthCharBuffer vBuf;
    CFIndex size;
    Boolean useLengthByte = false;
    Boolean useNullByte = false;
    Boolean useInlineData = false;

    if (!alloc) {
        alloc = CFAllocatorGetDefault();
    }

    if (contentsDeallocator == ALLOCATORSFREEFUNC) {
        contentsDeallocator = alloc;
    } else if (contentsDeallocator == NULL) {
        contentsDeallocator = CFAllocatorGetDefault();
    }

    if (kCFEmptyString && !numBytes && (alloc == kCFAllocatorSystemDefault)) {
        // If we are using the system default allocator,
        //  and the string is empty, then use the empty string!

        if (noCopy && (contentsDeallocator != kCFAllocatorNull)) {
            CFAllocatorDeallocate(contentsDeallocator, (void*)bytes);
        }
        return (CFStringRef)CFRetain(kCFEmptyString);
    }

    // At this point, contentsDeallocator is either same as alloc, or kCFAllocatorNull,
    //  or something else, but not NULL

    vBuf.shouldFreeChars = false; // We use this to remember to free the buffer possibly allocated by decode

    // Record whether we're starting out with an ASCII-superset string,
    //  because we need to know this later for the string ROM; this may get changed later if
    //  we successfully convert down from Unicode.  We only record this once because
    //  __CFCanUseEightBitCFStringForBytes() can be expensive.
    Boolean stringSupportsEightBitCFRepresentation =
        (encoding != kCFStringEncodingUnicode) &&
        __CFCanUseEightBitCFStringForBytes((const uint8_t*)bytes, numBytes, encoding);

    // First check to see if the data needs to be converted...
    // ??? We could be more efficient here and in some cases (Unicode data) eliminate a copy

    if ((encoding == kCFStringEncodingUnicode && possiblyExternalFormat) ||
        encoding != kCFStringEncodingUnicode && !stringSupportsEightBitCFRepresentation)
    {
        const void* realBytes = (uint8_t*)bytes + (hasLengthByte ? 1 : 0);
        CFIndex realNumBytes = numBytes - (hasLengthByte ? 1 : 0);
        Boolean usingPassedInMemory = false;

        vBuf.allocator = CFAllocatorGetDefault(); // We don't want to use client's allocator for temp stuff
        vBuf.chars.unicode = NULL;    // This will cause the decode function to allocate memory if necessary

        if (!__CFStringDecodeByteStream((const uint8_t*)realBytes, realNumBytes, encoding, false, &vBuf, &usingPassedInMemory, converterFlags)) {
            // Note that if the string can't be created, we don't free the buffer, even if there is a contents deallocator. This is on purpose.
            return NULL;
        }

        encoding = vBuf.isASCII ? kCFStringEncodingASCII : kCFStringEncodingUnicode;

        // Update our flag according to whether the decoded buffer is ASCII
        stringSupportsEightBitCFRepresentation = vBuf.isASCII;

        if (!usingPassedInMemory) {
            // Make the parameters fit the new situation
            numBytes = vBuf.isASCII ? vBuf.numChars : (vBuf.numChars * sizeof(UniChar));
            hasLengthByte = hasNullByte = false;

            // Get rid of the original buffer if its not being used
            if (noCopy && (contentsDeallocator != kCFAllocatorNull)) {
                CFAllocatorDeallocate(contentsDeallocator, (void*)bytes);
            }
            contentsDeallocator = alloc;    // At this point we are using the string's allocator, as the original buffer is gone...

            // See if we can reuse any storage the decode func might have allocated
            // We do this only for Unicode, as otherwise we would not have NULL and Length bytes

            if (vBuf.shouldFreeChars && (alloc == vBuf.allocator) && encoding == kCFStringEncodingUnicode) {
                vBuf.shouldFreeChars = false; // Transferring ownership to the CFString
                bytes = CFAllocatorReallocate(vBuf.allocator, (void*)vBuf.chars.unicode, numBytes, 0); // Tighten up the storage
                noCopy = true;
            } else {
                bytes = vBuf.chars.unicode;
                noCopy = false; // Can't do noCopy anymore
                // If vBuf.shouldFreeChars is true, the buffer will be freed as intended near the end of this func
            }
        }

        // At this point, all necessary input arguments have been changed to reflect the new state

    } else if (encoding == kCFStringEncodingUnicode && tryToReduceUnicode) { // Check to see if we can reduce Unicode to ASCII
        CFIndex cnt;
        CFIndex len = numBytes / sizeof(UniChar);
        Boolean allASCII = true;

        for (cnt = 0; cnt < len; cnt++) {
            if (((const UniChar*)bytes)[cnt] > 127) {
                allASCII = false;
                break;
            }
        }

        if (allASCII) {    // Yes we can!
            uint8_t* ptr, * mem;
            Boolean newHasLengthByte = __CFCanUseLengthByte(len);
            numBytes = (len + 1 + (newHasLengthByte ? 1 : 0)) * sizeof(uint8_t);    // NULL and possible length byte
            // See if we can use that temporary local buffer in vBuf...
            if (numBytes >= sizeof(vBuf.localBuffer)) {
                mem = ptr = (uint8_t*)CFAllocatorAllocate(alloc, numBytes, 0);
            } else {
                mem = ptr = (uint8_t*)(vBuf.localBuffer);
            }
            if (mem) { // If we can't allocate memory for some reason, use what we had (that is, as if we didn't have all ASCII)
                // Copy the Unicode bytes into the new ASCII buffer
                hasLengthByte = newHasLengthByte;
                hasNullByte = true;
                if (hasLengthByte) {
                    *ptr++ = (uint8_t)len;
                }
                for (cnt = 0; cnt < len; cnt++) {
                    ptr[cnt] = (uint8_t)(((const UniChar*)bytes)[cnt]);
                }
                ptr[len] = 0;
                if (noCopy && (contentsDeallocator != kCFAllocatorNull)) {
                    CFAllocatorDeallocate(contentsDeallocator, (void*)bytes);
                }
                // Now make everything look like we had an ASCII buffer to start with
                bytes = mem;
                encoding = kCFStringEncodingASCII;
                contentsDeallocator = alloc; // At this point we are using the string's allocator, as the original buffer is gone...
                noCopy = (numBytes >= sizeof(vBuf.localBuffer)); // If we had to allocate it, make sure it's kept around
                numBytes--; // Should not contain the NULL byte at end...
                stringSupportsEightBitCFRepresentation = true; // We're ASCII now!
            }
        }

        // At this point, all necessary input arguments have been changed to reflect the new state
    }

    // Now determine the necessary size
    if (noCopy) {

        size = sizeof(void*);                 // Pointer to the buffer
        if (contentsDeallocator != alloc && contentsDeallocator != kCFAllocatorNull) {
            size += sizeof(void*); // The contentsDeallocator
        }
        if (!hasLengthByte) {
            size += sizeof(CFIndex);                    // Explicit length
        }
        useLengthByte = hasLengthByte;
        useNullByte = hasNullByte;

    } else {    // Inline data; reserve space for it

        useInlineData = true;
        size = numBytes;

        if (hasLengthByte || (encoding != kCFStringEncodingUnicode && __CFCanUseLengthByte(numBytes))) {
            useLengthByte = true;
            if (!hasLengthByte) {
                size += 1;
            }
        } else {
            size += sizeof(CFIndex); // Explicit length
        }
        if (hasNullByte || encoding != kCFStringEncodingUnicode) {
            useNullByte = true;
            size += 1;
        }
    }

    // Finally, allocate!

    str = (CFMutableStringRef)_CFRuntimeCreateInstance(alloc, _kCFStringTypeID, size, NULL);
    if (str) {

        __CFStrSetInfoBits(str,
            (useInlineData ? __kCFHasInlineContents :
             (contentsDeallocator == alloc ? __kCFNotInlineContentsDefaultFree :
              (contentsDeallocator == kCFAllocatorNull ? __kCFNotInlineContentsNoFree :
                  __kCFNotInlineContentsCustomFree))) |
            ((encoding == kCFStringEncodingUnicode) ? __kCFIsUnicode : 0) |
            (useNullByte ? __kCFHasNullByte : 0) |
            (useLengthByte ? __kCFHasLengthByte : 0));

        if (!useLengthByte) {
            CFIndex length = numBytes - (hasLengthByte ? 1 : 0);
            if (encoding == kCFStringEncodingUnicode) {
                length /= sizeof(UniChar);
            }
            __CFStrSetExplicitLength(str, length);
        }

        if (useInlineData) {
            uint8_t* contents = (uint8_t*)__CFStrContents(str);
            if (useLengthByte && !hasLengthByte) {
                *contents++ = (uint8_t)numBytes;
            }
            memmove(contents, bytes, numBytes);
            if (useNullByte) {
                contents[numBytes] = 0;
            }
        } else {
            __CFStrSetContentPtr(str, bytes);
            if (contentsDeallocator != alloc && contentsDeallocator != kCFAllocatorNull) {
                __CFStrSetContentsDeallocator(str, (CFAllocatorRef)CFRetain(contentsDeallocator));
            }
        }
    } else {
        if (noCopy && (contentsDeallocator != kCFAllocatorNull)) {
            CFAllocatorDeallocate(contentsDeallocator, (void*)bytes);
        }
    }
    if (vBuf.shouldFreeChars) {
        CFAllocatorDeallocate(vBuf.allocator, (void*)bytes);
    }

    return str;
}

CF_INTERNAL CFStringRef _CFStringCreateWithFormatAndArgumentsAux(CFAllocatorRef alloc, CFStringRef (* copyDescFunc)(void*, const void*), CFDictionaryRef formatOptions, CFStringRef format, va_list arguments) {
    CFStringRef str;
    CFMutableStringRef outputString = CFStringCreateMutable(CFAllocatorGetDefault(), 0); //should use alloc if no copy/release
    __CFStrSetDesiredCapacity(outputString, 120);    // Given this will be tightened later, choosing a larger working string is fine
    _CFStringAppendFormatAndArgumentsAux(outputString, copyDescFunc, formatOptions, format, arguments);
    // ??? copy/release should not be necessary here -- just make immutable, compress if possible
    // (However, this does make the string inline, and cause the supplied allocator to be used...)
    str = (CFStringRef)CFStringCreateCopy(alloc, outputString);
    CFRelease(outputString);
    return str;
}

/* This one is for NSCFString; it does not ObjC dispatch or assertion check
 */
CF_INTERNAL CFIndex _CFStringGetLength2(CFStringRef str) {
    return __CFStrLength(str);
}

CF_INTERNAL CFStringEncoding CFStringFileSystemEncoding(void) {
    if (__CFDefaultFileSystemEncoding == kCFStringEncodingInvalidId) {
        __CFDefaultFileSystemEncoding = CFPlatformGetFileSystemEncoding();
    }
    return __CFDefaultFileSystemEncoding;
}

CF_INTERNAL const void* __CFStringCollectionCopy(CFAllocatorRef allocator, const void* ptr) {
    CFStringRef string = (CFStringRef)ptr;
    return CFStringCreateCopy(allocator, string);
}

///////////////////////////////////////////////////////////////////// public


CFTypeID CFStringGetTypeID(void) {
    return _kCFStringTypeID;
}

CFStringRef CFStringCreateCopy(CFAllocatorRef alloc, CFStringRef str) {
    CF_VALIDATE_STRING_ARG(str);

    // If the string is not mutable
    //  and it has the same allocator as the one we're using
    //  and the characters are inline, or are owned by the string, 
    //   or the string is constant ...
    if (!__CFStrIsMutable(str) &&
        ((alloc ? alloc : CFAllocatorGetDefault()) == CFGetAllocator(str)) &&
        (__CFStrIsInline(str) || __CFStrFreeContentsWhenDone(str) || __CFStrIsConstant(str)))
    {
        // ... then just retain instead of making a true copy.
        CFRetain(str);
        return str;
    }
    if (__CFStrIsEightBit(str)) {
        const uint8_t* contents = (const uint8_t*)__CFStrContents(str);
        return __CFStringCreateImmutableFunnel(
            alloc, 
            contents + __CFStrSkipAnyLengthByte(str), __CFStrLength2(str, contents), 
            __CFStringGetEightBitStringEncoding(), 
            false, false, false, false, false, ALLOCATORSFREEFUNC, 0);
    } else {
        const UniChar* contents = (const UniChar*)__CFStrContents(str);
        return __CFStringCreateImmutableFunnel(
            alloc, 
            contents, __CFStrLength2(str, contents) * sizeof(UniChar), 
            kCFStringEncodingUnicode, 
            false, true, false, false, false, ALLOCATORSFREEFUNC, 0);
    }
}

CFStringRef  CFStringCreateWithPascalString(CFAllocatorRef alloc, ConstStringPtr pStr, CFStringEncoding encoding) {
    CFIndex len = (CFIndex)(*(uint8_t*)pStr);
    return __CFStringCreateImmutableFunnel(alloc, pStr, len + 1, encoding, false, false, true, false, false, ALLOCATORSFREEFUNC, 0);
}

CFStringRef  CFStringCreateWithCString(CFAllocatorRef alloc, const char* cStr, CFStringEncoding encoding) {
    CF_VALIDATE_PTR_ARG(cStr);
    CFIndex len = strlen(cStr);
    return __CFStringCreateImmutableFunnel(alloc, cStr, len, encoding, false, false, false, true, false, ALLOCATORSFREEFUNC, 0);
}

CFStringRef  CFStringCreateWithPascalStringNoCopy(CFAllocatorRef alloc, ConstStringPtr pStr, CFStringEncoding encoding, CFAllocatorRef contentsDeallocator) {
    CFIndex len = (CFIndex)(*(uint8_t*)pStr);
    return __CFStringCreateImmutableFunnel(alloc, pStr, len + 1, encoding, false, false, true, false, true, contentsDeallocator, 0);
}

CFStringRef  CFStringCreateWithCStringNoCopy(CFAllocatorRef alloc, const char* cStr, CFStringEncoding encoding, CFAllocatorRef contentsDeallocator) {
    CF_VALIDATE_PTR_ARG(cStr);
    CFIndex len = strlen(cStr);
    return __CFStringCreateImmutableFunnel(alloc, cStr, len, encoding, false, false, false, true, true, contentsDeallocator, 0);
}

CFStringRef  CFStringCreateWithCharacters(CFAllocatorRef alloc, const UniChar* chars, CFIndex numChars) {
    return __CFStringCreateImmutableFunnel(alloc, chars, numChars * sizeof(UniChar), kCFStringEncodingUnicode, false, true, false, false, false, ALLOCATORSFREEFUNC, 0);
}

CFStringRef  CFStringCreateWithCharactersNoCopy(CFAllocatorRef alloc, const UniChar* chars, CFIndex numChars, CFAllocatorRef contentsDeallocator) {
    return __CFStringCreateImmutableFunnel(alloc, chars, numChars * sizeof(UniChar), kCFStringEncodingUnicode, false, false, false, false, true, contentsDeallocator, 0);
}

CFStringRef  CFStringCreateWithBytes(CFAllocatorRef alloc, const uint8_t* bytes, CFIndex numBytes, CFStringEncoding encoding, Boolean externalFormat) {
    return __CFStringCreateImmutableFunnel(alloc, bytes, numBytes, encoding, externalFormat, true, false, false, false, ALLOCATORSFREEFUNC, 0);
}

CFStringRef  CFStringCreateWithBytesNoCopy(CFAllocatorRef alloc, const uint8_t* bytes, CFIndex numBytes, CFStringEncoding encoding, Boolean externalFormat, CFAllocatorRef contentsDeallocator) {
    return __CFStringCreateImmutableFunnel(alloc, bytes, numBytes, encoding, externalFormat, true, false, false, true, contentsDeallocator, 0);
}

CFStringRef  CFStringCreateWithFormatAndArguments(CFAllocatorRef alloc, CFDictionaryRef formatOptions, CFStringRef format, va_list arguments) {
    return _CFStringCreateWithFormatAndArgumentsAux(alloc, NULL, formatOptions, format, arguments);
}

CFStringRef  CFStringCreateWithFormat(CFAllocatorRef alloc, CFDictionaryRef formatOptions, CFStringRef format, ...) {
    CFStringRef result;
    va_list argList;

    va_start(argList, format);
    result = CFStringCreateWithFormatAndArguments(alloc, formatOptions, format, argList);
    va_end(argList);

    return result;
}

CFStringRef CFStringCreateWithSubstring(CFAllocatorRef alloc, CFStringRef str, CFRange range) {
    CF_VALIDATE_STRING_ARG(str);
    CF_VALIDATE_STRING_RANGE_ARG(str, range);

    if (range.location == 0 && range.length == __CFStrLength(str)) {
        // The substring is the whole string.
        return (CFStringRef)CFStringCreateCopy(alloc, str);
    } else if (__CFStrIsEightBit(str)) {
        const uint8_t* contents = (const uint8_t*)__CFStrContents(str);
        return __CFStringCreateImmutableFunnel(
            alloc,
            contents + range.location + __CFStrSkipAnyLengthByte(str),
            range.length,
            __CFStringGetEightBitStringEncoding(),
            false, false, false, false, false, ALLOCATORSFREEFUNC, 0);
    } else {
        const UniChar* contents = (const UniChar*)__CFStrContents(str);
        return __CFStringCreateImmutableFunnel(
            alloc,
            contents + range.location,
            range.length * sizeof(UniChar),
            kCFStringEncodingUnicode,
            false, true, false, false, false, ALLOCATORSFREEFUNC, 0);
    }
}

CFStringRef CFStringCreateWithFileSystemRepresentation(CFAllocatorRef alloc, const char* buffer) {
    return CFStringCreateWithCString(alloc, buffer, CFStringFileSystemEncoding());
}

CFIndex CFStringGetLength(CFStringRef str) {
    CF_OBJC_FUNCDISPATCH(CFIndex, str, "length");

    CF_VALIDATE_STRING_ARG(str);
    return __CFStrLength(str);
}

UniChar CFStringGetCharacterAtIndex(CFStringRef str, CFIndex idx) {
    CF_OBJC_FUNCDISPATCH(UniChar, str, "characterAtIndex:", idx);

    CF_VALIDATE_STRING_ARG(str);
    CF_VALIDATE_STRING_INDEX_ARG(str, idx);

    const uint8_t* contents = (const uint8_t*)__CFStrContents(str);
    if (__CFStrIsEightBit(str)) {
        contents += __CFStrSkipAnyLengthByte(str);
        return __CFCharToUniCharTable[contents[idx]];
    } else {
        return ((UniChar*)contents)[idx];
    }
}

void CFStringGetCharacters(CFStringRef str, CFRange range, UniChar* buffer) {
    CF_OBJC_VOID_FUNCDISPATCH(str, "getCharacters:range:", buffer, range);

    CF_VALIDATE_STRING_ARG(str);
    CF_VALIDATE_STRING_RANGE_ARG(str, range);

    const uint8_t* contents = (const uint8_t*)__CFStrContents(str);
    if (__CFStrIsEightBit(str)) {
        __CFStrConvertBytesToUnicode(contents + (range.location + __CFStrSkipAnyLengthByte(str)), buffer, range.length);
    } else {
        const UniChar* uContents = ((UniChar*)contents) + range.location;
        memmove(buffer, uContents, range.length * sizeof(UniChar));
    }
}

CFIndex CFStringGetBytes(CFStringRef str, CFRange range, CFStringEncoding encoding, uint8_t lossByte, Boolean isExternalRepresentation, uint8_t* buffer, CFIndex maxBufLen, CFIndex* usedBufLen) {
    // No objc dispatch needed here since __CFStringEncodeByteStream works 
    //  with both CFString and NSString.

    CF_VALIDATE_NONNEGATIVE_ARG(maxBufLen);

    if (!CF_IS_OBJC(str)) {    // If we can grope the ivars, let's do it...
        CF_VALIDATE_STRING_ARG(str);
        CF_VALIDATE_STRING_RANGE_ARG(str, range);

        if (__CFStrIsEightBit(str) && ((__CFStringGetEightBitStringEncoding() == encoding) || (__CFStringGetEightBitStringEncoding() == kCFStringEncodingASCII && __CFStringEncodingIsSupersetOfASCII(encoding)))) {    // Requested encoding is equal to the encoding in string
            const unsigned char* contents = (const unsigned char*)__CFStrContents(str);
            CFIndex cLength = range.length;

            if (buffer) {
                if (cLength > maxBufLen) {
                    cLength = maxBufLen;
                }
                memmove(buffer, contents + __CFStrSkipAnyLengthByte(str) + range.location, cLength);
            }
            if (usedBufLen) {
                *usedBufLen = cLength;
            }

            return cLength;
        }
    }

    return __CFStringEncodeByteStream(str, range.location, range.length, isExternalRepresentation, encoding, lossByte, buffer, maxBufLen, usedBufLen);
}

ConstStringPtr CFStringGetPascalStringPtr(CFStringRef str, CFStringEncoding encoding) {
    if (!CF_IS_OBJC(str)) {
        CF_VALIDATE_STRING_ARG(str);
        if (__CFStrHasLengthByte(str) && __CFStrIsEightBit(str) && ((__CFStringGetEightBitStringEncoding() == encoding) || (__CFStringGetEightBitStringEncoding() == kCFStringEncodingASCII && __CFStringEncodingIsSupersetOfASCII(encoding)))) {    // Requested encoding is equal to the encoding in string || the contents is in ASCII
            const uint8_t* contents = (const uint8_t*)__CFStrContents(str);
            if (__CFStrHasExplicitLength(str) && (__CFStrLength2(str, contents) != (SInt32)(*contents))) {
                //TODO WTF is length check for?
                return NULL;                                                                                         // Invalid length byte
            }
            return (ConstStringPtr)contents;
        }
    }
    return NULL;
}

const char* CFStringGetCStringPtr(CFStringRef str, CFStringEncoding encoding) {
    if (encoding != __CFStringGetEightBitStringEncoding() && (
            kCFStringEncodingASCII != __CFStringGetEightBitStringEncoding() ||
            !__CFStringEncodingIsSupersetOfASCII(encoding)))
    {
        return NULL;
    }

    CF_OBJC_FUNCDISPATCH(const char*, str, "_fastCStringContents:", true);

    CF_VALIDATE_STRING_ARG(str);

    if (__CFStrHasNullByte(str)) {
        // Note: this is called a lot, 27000 times to open a small xcode project with one file open.
        // Of these uses about 1500 are for cStrings/utf8strings.
        return (const char*)__CFStrContents(str) + __CFStrSkipAnyLengthByte(str);
    } else {
        return NULL;
    }
}

const UniChar* CFStringGetCharactersPtr(CFStringRef str) {
    CF_OBJC_FUNCDISPATCH(const UniChar *, str, "_fastCharacterContents");

    CF_VALIDATE_STRING_ARG(str);
    if (__CFStrIsUnicode(str)) {
        return (const UniChar*)__CFStrContents(str);
    }
    return NULL;
}

Boolean CFStringGetPascalString(CFStringRef str, Str255 buffer, CFIndex bufferSize, CFStringEncoding encoding) {
    CFIndex length;
    CFIndex usedLen;

    CF_VALIDATE_NONNEGATIVE_ARG(bufferSize);
    if (bufferSize < 1) {
        return false;
    }

    if (CF_IS_OBJC(str)) {
        length = CFStringGetLength(str);
        if (!__CFCanUseLengthByte(length)) {
            // Can't fit into pstring
            return false;
        }
    } else {
        const uint8_t* contents;

        CF_VALIDATE_STRING_ARG(str);

        contents = (const uint8_t*)__CFStrContents(str);
        length = __CFStrLength2(str, contents);

        if (!__CFCanUseLengthByte(length)) {
            // Can't fit into pstring
            return false;
        }
        if (__CFStrIsEightBit(str) && (
                (__CFStringGetEightBitStringEncoding() == encoding) ||
                (__CFStringGetEightBitStringEncoding() == kCFStringEncodingASCII && 
                    __CFStringEncodingIsSupersetOfASCII(encoding))))
        {   
            // Requested encoding is equal to the encoding in string
            if (length >= bufferSize) {
                return false;
            }
            memmove((void*)(1 + (const char*)buffer), (__CFStrSkipAnyLengthByte(str) + contents), length);
            *buffer = (unsigned char)length;
            return true;
        }
    }

    if (__CFStringEncodeByteStream(str, 0, length, false, encoding, false, (UInt8*)(1 + (uint8_t*)buffer), bufferSize - 1, &usedLen) != length) {

#if defined(DEBUG)
        if (bufferSize > 0) {
            strlcpy((char*)buffer + 1, CONVERSIONFAILURESTR, bufferSize - 1);
            buffer[0] = (unsigned char)((CFIndex)sizeof(CONVERSIONFAILURESTR) < (bufferSize - 1) ? (CFIndex)sizeof(CONVERSIONFAILURESTR) : (bufferSize - 1));
        }
#else
        if (bufferSize > 0) {
            buffer[0] = 0;
        }
#endif
        return false;
    }
    *buffer = (unsigned char)usedLen;
    return true;
}

Boolean CFStringGetCString(CFStringRef str, char* buffer, CFIndex bufferSize, CFStringEncoding encoding) {
    const uint8_t* contents;
    CFIndex len;

    CF_VALIDATE_NONNEGATIVE_ARG(bufferSize);
    if (bufferSize < 1) {
        return false;
    }

    CF_OBJC_FUNCDISPATCH(Boolean, str, "_getCString:maxLength:encoding:", buffer, bufferSize - 1, encoding);

    CF_VALIDATE_STRING_ARG(str);

    contents = (const uint8_t*)__CFStrContents(str);
    len = __CFStrLength2(str, contents);

    if (__CFStrIsEightBit(str) && ((__CFStringGetEightBitStringEncoding() == encoding) || (__CFStringGetEightBitStringEncoding() == kCFStringEncodingASCII && __CFStringEncodingIsSupersetOfASCII(encoding)))) {    // Requested encoding is equal to the encoding in string
        if (len >= bufferSize) {
            return false;
        }
        memmove(buffer, contents + __CFStrSkipAnyLengthByte(str), len);
        buffer[len] = 0;
        return true;
    } else {
        CFIndex usedLen;

        if (__CFStringEncodeByteStream(str, 0, len, false, encoding, false, (unsigned char*)buffer, bufferSize - 1, &usedLen) == len) {
            buffer[usedLen] = '\0';
            return true;
        } else {
#if defined(DEBUG)
            strlcpy(buffer, CONVERSIONFAILURESTR, bufferSize);
#else
            if (bufferSize > 0) {
                buffer[0] = 0;
            }
#endif
            return false;
        }
    }
}

//TODO !! Should support kCFCompareNonliteral
Boolean CFStringFindCharacterFromSet(CFStringRef theString, CFCharacterSetRef theSet, CFRange rangeToSearch, CFOptionFlags searchOptions, CFRange* result) {
    CFStringInlineBuffer stringBuffer;
    CFCharacterSetInlineBuffer csetBuffer;
    UniChar ch;
    CFIndex step;
    CFIndex fromLoc, toLoc, cnt;    // fromLoc and toLoc are inclusive
    Boolean found = false;
    Boolean done = false;

    if ((rangeToSearch.location + rangeToSearch.length > CFStringGetLength(theString)) ||
        (rangeToSearch.length == 0))
    {
        return false;
    }

    if (searchOptions & kCFCompareBackwards) {
        fromLoc = rangeToSearch.location + rangeToSearch.length - 1;
        toLoc = rangeToSearch.location;
    } else {
        fromLoc = rangeToSearch.location;
        toLoc = rangeToSearch.location + rangeToSearch.length - 1;
    }
    if (searchOptions & kCFCompareAnchored) {
        toLoc = fromLoc;
    }

    step = (fromLoc <= toLoc) ? 1 : -1;
    cnt = fromLoc;

    CFStringInitInlineBuffer(theString, &stringBuffer, rangeToSearch);
    CFCharacterSetInitInlineBuffer(theSet, &csetBuffer);

    do {
        ch = CFStringGetCharacterFromInlineBuffer(&stringBuffer, cnt - rangeToSearch.location);
        if ((ch >= SURROGATE_START) && (ch <= SURROGATE_END)) {
            int otherCharIndex = cnt + step;

            if (((step < 0) && (otherCharIndex < toLoc)) ||
                ((step > 0) && (otherCharIndex > toLoc)))
            {
                done = true;
            } else {
                UniChar highChar;
                UniChar lowChar = CFStringGetCharacterFromInlineBuffer(&stringBuffer, otherCharIndex - rangeToSearch.location);

                if (cnt < otherCharIndex) {
                    highChar = ch;
                } else {
                    highChar = lowChar;
                    lowChar = ch;
                }

                if (_CFUniCharIsSurrogateHighCharacter(highChar) &&
                    _CFUniCharIsSurrogateLowCharacter(lowChar) &&
                    CFCharacterSetInlineBufferIsLongCharacterMember(
                        &csetBuffer,
                        _CFUniCharGetLongCharacterForSurrogatePair(highChar, lowChar)))
                {
                    if (result) {
                        *result = CFRangeMake((cnt < otherCharIndex ? cnt : otherCharIndex), 2);
                    }
                    return true;
                } else if (otherCharIndex == toLoc) {
                    done = true;
                } else {
                    cnt = otherCharIndex + step;
                }
            }
        } else if (CFCharacterSetInlineBufferIsLongCharacterMember(&csetBuffer, ch)) {
            done = found = true;
        } else if (cnt == toLoc) {
            done = true;
        } else {
            cnt += step;
        }
    } while (!done);

    if (found && result) {
        *result = CFRangeMake(cnt, 1);
    }
    return found;
}

void CFStringGetLineBounds(CFStringRef string, CFRange range, CFIndex* lineBeginIndex, CFIndex* lineEndIndex, CFIndex* contentsEndIndex) {
    CF_OBJC_VOID_FUNCDISPATCH(string, "getLineStart:end:contentsEnd:forRange:", lineBeginIndex, lineEndIndex, contentsEndIndex, CFRangeMake(range.location, range.length));
    __CFStringGetLineOrParagraphBounds(string, range, lineBeginIndex, lineEndIndex, contentsEndIndex, true);
}

void CFStringGetParagraphBounds(CFStringRef string, CFRange range, CFIndex* parBeginIndex, CFIndex* parEndIndex, CFIndex* contentsEndIndex) {
    CF_OBJC_VOID_FUNCDISPATCH(string, "getParagraphStart:end:contentsEnd:forRange:", parBeginIndex, parEndIndex, contentsEndIndex, CFRangeMake(range.location, range.length));
    __CFStringGetLineOrParagraphBounds(string, range, parBeginIndex, parEndIndex, contentsEndIndex, false);
}

CFStringRef CFStringCreateByCombiningStrings(CFAllocatorRef alloc, CFArrayRef array, CFStringRef separatorString) {
    CFIndex numChars;
    CFIndex separatorNumByte;
    CFIndex stringCount = CFArrayGetCount(array);
    Boolean isSepCFString = !CF_IS_OBJC(separatorString);
    Boolean canBeEightbit = isSepCFString && __CFStrIsEightBit(separatorString);
    CFIndex idx;
    CFStringRef otherString;
    void* buffer;
    uint8_t* bufPtr;
    const void* separatorContents = NULL;

    if (stringCount == 0) {
        return CFStringCreateWithCharacters(alloc, NULL, 0);
    } else if (stringCount == 1) {
        return (CFStringRef)CFStringCreateCopy(alloc, (CFStringRef)CFArrayGetValueAtIndex(array, 0));
    }

    if (alloc == NULL) {
        alloc = CFAllocatorGetDefault();
    }

    numChars = CFStringGetLength(separatorString) * (stringCount - 1);
    for (idx = 0; idx < stringCount; idx++) {
        otherString = (CFStringRef)CFArrayGetValueAtIndex(array, idx);
        numChars += CFStringGetLength(otherString);
        // canBeEightbit is already false if the separator is an NSString...
        if (!CF_IS_OBJC(otherString) && __CFStrIsUnicode(otherString)) {
            canBeEightbit = false;
        }
    }

    buffer = (uint8_t*)CFAllocatorAllocate(alloc, canBeEightbit ? ((numChars + 1) * sizeof(uint8_t)) : (numChars * sizeof(UniChar)), 0);
    bufPtr = (uint8_t*)buffer;
    separatorNumByte = CFStringGetLength(separatorString) * (canBeEightbit ? sizeof(uint8_t) : sizeof(UniChar));

    for (idx = 0; idx < stringCount; idx++) {
        if (idx) { // add separator here unless first string
            if (separatorContents) {
                memmove(bufPtr, separatorContents, separatorNumByte);
            } else {
                if (!isSepCFString) { // NSString
                    CFStringGetCharacters(separatorString, CFRangeMake(0, CFStringGetLength(separatorString)), (UniChar*)bufPtr);
                } else if (canBeEightbit || __CFStrIsUnicode(separatorString)) {
                    memmove(bufPtr, (const uint8_t*)__CFStrContents(separatorString) + __CFStrSkipAnyLengthByte(separatorString), separatorNumByte);
                } else {
                    __CFStrConvertBytesToUnicode((uint8_t*)__CFStrContents(separatorString) + __CFStrSkipAnyLengthByte(separatorString), (UniChar*)bufPtr, __CFStrLength(separatorString));
                }
                separatorContents = bufPtr;
            }
            bufPtr += separatorNumByte;
        }

        otherString = (CFStringRef )CFArrayGetValueAtIndex(array, idx);
        if (CF_IS_OBJC(otherString)) {
            CFIndex otherLength = CFStringGetLength(otherString);
            CFStringGetCharacters(otherString, CFRangeMake(0, otherLength), (UniChar*)bufPtr);
            bufPtr += otherLength * sizeof(UniChar);
        } else {
            const uint8_t* otherContents = (const uint8_t*)__CFStrContents(otherString);
            CFIndex otherNumByte = __CFStrLength2(otherString, otherContents) * (canBeEightbit ? sizeof(uint8_t) : sizeof(UniChar));

            if (canBeEightbit || __CFStrIsUnicode(otherString)) {
                memmove(bufPtr, otherContents + __CFStrSkipAnyLengthByte(otherString), otherNumByte);
            } else {
                __CFStrConvertBytesToUnicode(otherContents + __CFStrSkipAnyLengthByte(otherString), (UniChar*)bufPtr, __CFStrLength2(otherString, otherContents));
            }
            bufPtr += otherNumByte;
        }
    }
    if (canBeEightbit) {
        *bufPtr = 0;
    }
    return canBeEightbit ?
           CFStringCreateWithCStringNoCopy(alloc, (const char*)buffer, __CFStringGetEightBitStringEncoding(), alloc) :
           CFStringCreateWithCharactersNoCopy(alloc, (UniChar*)buffer, numChars, alloc);
}

CFArrayRef CFStringCreateArrayBySeparatingStrings(CFAllocatorRef alloc, CFStringRef string, CFStringRef separatorString) {
    // No objc dispatch needed here since CFStringCreateArrayWithFindResults() 
    //  works with both CFString and NSString.

    CFArrayRef separatorRanges;
    CFIndex length = CFStringGetLength(string);
    if (!(separatorRanges = CFStringCreateArrayWithFindResults(alloc, string, separatorString, CFRangeMake(0, length), 0))) {
        return CFArrayCreate(alloc, (const void**)&string, 1, &kCFTypeArrayCallBacks);
    } else {
        CFIndex idx;
        CFIndex count = CFArrayGetCount(separatorRanges);
        CFIndex startIndex = 0;
        CFIndex numChars;
        CFMutableArrayRef array = CFArrayCreateMutable(alloc, count + 2, &kCFTypeArrayCallBacks);
        const CFRange* currentRange;
        CFStringRef substring;

        for (idx = 0; idx < count; idx++) {
            currentRange = (const CFRange*)CFArrayGetValueAtIndex(separatorRanges, idx);
            numChars = currentRange->location - startIndex;
            substring = CFStringCreateWithSubstring(alloc, string, CFRangeMake(startIndex, numChars));
            CFArrayAppendValue(array, substring);
            CFRelease(substring);
            startIndex = currentRange->location + currentRange->length;
        }
        substring = CFStringCreateWithSubstring(alloc, string, CFRangeMake(startIndex, length - startIndex));
        CFArrayAppendValue(array, substring);
        CFRelease(substring);

        CFRelease(separatorRanges);

        return array;
    }
}

CFStringRef CFStringCreateFromExternalRepresentation(CFAllocatorRef alloc, CFDataRef data, CFStringEncoding encoding) {
    return CFStringCreateWithBytes(alloc, CFDataGetBytePtr(data), CFDataGetLength(data), encoding, true);
}

CFDataRef CFStringCreateExternalRepresentation(CFAllocatorRef alloc, CFStringRef string, CFStringEncoding encoding, uint8_t lossByte) {
    CFIndex length;
    CFIndex guessedByteLength;
    uint8_t* bytes;
    CFIndex usedLength;
    SInt32 result;

    if (CF_IS_OBJC(string)) {
        length = CFStringGetLength(string);
    } else {
        CF_VALIDATE_STRING_ARG(string);
        length = __CFStrLength(string);

        if (__CFStrIsEightBit(string) && (
            (__CFStringGetEightBitStringEncoding() == encoding) || 
            (__CFStringGetEightBitStringEncoding() == kCFStringEncodingASCII && __CFStringEncodingIsSupersetOfASCII(encoding))))
        {   
            // Requested encoding is equal to the encoding in string
            return CFDataCreate(alloc, ((uint8_t*)__CFStrContents(string) + __CFStrSkipAnyLengthByte(string)), __CFStrLength(string));
        }
    }

    if (alloc == NULL) {
        alloc = CFAllocatorGetDefault();
    }

    if (((encoding & 0x0FFF) == kCFStringEncodingUnicode) && ((encoding == kCFStringEncodingUnicode) || ((encoding > kCFStringEncodingUTF8) && (encoding <= kCFStringEncodingUTF32LE)))) {
        //TODO name the magic 'UTF32 format' bit.
        guessedByteLength = (length + 1) * ((((encoding >> 26)  & 2) == 0) ? sizeof(UTF16Char) : sizeof(UTF32Char)); // UTF32 format has the bit set
    } else if (((guessedByteLength = CFStringGetMaximumSizeForEncoding(length, encoding)) > length) && !CF_IS_OBJC(string)) { // Multi byte encoding
        if (__CFStrIsUnicode(string)) {
            CFIndex aLength = CFStringEncodingByteLengthForCharacters(encoding, kCFStringEncodingPrependBOM, (const UniChar*)__CFStrContents(string), __CFStrLength(string));
            if (aLength > 0) {
                guessedByteLength = aLength;
            }
        } else {
            result = __CFStringEncodeByteStream(string, 0, length, true, encoding, lossByte, NULL, LONG_MAX, &guessedByteLength);
            // if result == length, we always succeed
            //   otherwise, if result == 0, we fail
            //   otherwise, if there was a lossByte but still result != length, we fail
            if ((result != length) && (!result || !lossByte)) {
                return NULL;
            }
            if (guessedByteLength == length && __CFStrIsEightBit(string) && __CFStringEncodingIsSupersetOfASCII(encoding)) { // It's all ASCII !!
                return CFDataCreate(alloc, ((uint8_t*)__CFStrContents(string) + __CFStrSkipAnyLengthByte(string)), __CFStrLength(string));
            }
        }
    }
    bytes = (uint8_t*)CFAllocatorAllocate(alloc, guessedByteLength, 0);

    result = __CFStringEncodeByteStream(string, 0, length, true, encoding, lossByte, bytes, guessedByteLength, &usedLength);

    if ((result != length) && (!result || !lossByte)) {        // see comment above about what this means
        CFAllocatorDeallocate(alloc, bytes);
        return NULL;
    }

    return CFDataCreateWithBytesNoCopy(alloc, (uint8_t*)bytes, usedLength, alloc);
}

CFStringEncoding CFStringGetSmallestEncoding(CFStringRef str) {
    CFIndex len;
    CF_OBJC_FUNCDISPATCH(CFStringEncoding, str, "_smallestEncodingInCFStringEncoding");
    CF_VALIDATE_STRING_ARG(str);

    if (__CFStrIsEightBit(str)) {
        return __CFStringGetEightBitStringEncoding();
    }
    len = __CFStrLength(str);
    if (__CFStringEncodeByteStream(str, 0, len, false, __CFStringGetEightBitStringEncoding(), 0, NULL, LONG_MAX, NULL) == len) {
        return __CFStringGetEightBitStringEncoding();
    }
    if ((__CFStringGetEightBitStringEncoding() != CFStringGetSystemEncoding()) && (__CFStringEncodeByteStream(str, 0, len, false, CFStringGetSystemEncoding(), 0, NULL, LONG_MAX, NULL) == len)) {
        return CFStringGetSystemEncoding();
    }
    return kCFStringEncodingUnicode;
}

CFStringEncoding CFStringGetFastestEncoding(CFStringRef str) {
    CF_OBJC_FUNCDISPATCH(CFStringEncoding, str, "_fastestEncodingInCFStringEncoding");
    CF_VALIDATE_STRING_ARG(str);
    return __CFStrIsEightBit(str) ? __CFStringGetEightBitStringEncoding() : kCFStringEncodingUnicode;    /* ??? */
}

CFArrayRef CFStringCreateArrayWithFindResults(CFAllocatorRef alloc, CFStringRef string, CFStringRef stringToFind, CFRange rangeToSearch, CFOptionFlags compareOptions) {
    CFRange foundRange;
    Boolean backwards = ((compareOptions & kCFCompareBackwards) != 0);
    UInt32 endIndex = rangeToSearch.location + rangeToSearch.length;
    CFMutableDataRef rangeStorage = NULL;    // Basically an array of CFRange, CFDataRef (packed)
    uint8_t* rangeStorageBytes = NULL;
    CFIndex foundCount = 0;
    CFIndex capacity = 0;        // Number of CFRange, CFDataRef element slots in rangeStorage

    if (alloc == NULL) {
        alloc = CFAllocatorGetDefault();
    }

    while ((rangeToSearch.length > 0) && CFStringFindWithOptions(string, stringToFind, rangeToSearch, compareOptions, &foundRange)) {
        // Determine the next range
        if (backwards) {
            rangeToSearch.length = foundRange.location - rangeToSearch.location;
        } else {
            rangeToSearch.location = foundRange.location + foundRange.length;
            rangeToSearch.length = endIndex - rangeToSearch.location;
        }

        // If necessary, grow the data and squirrel away the found range
        if (foundCount >= capacity) {
            if (rangeStorage == NULL) {
                rangeStorage = CFDataCreateMutable(alloc, 0);
            }
            capacity = (capacity + 4) * 2;
            CFDataSetLength(rangeStorage, capacity * (sizeof(CFRange) + sizeof(CFDataRef)));
            rangeStorageBytes = (uint8_t*)CFDataGetMutableBytePtr(rangeStorage) + foundCount * (sizeof(CFRange) + sizeof(CFDataRef));
        }
        memmove(rangeStorageBytes, &foundRange, sizeof(CFRange)); // The range
        memmove(rangeStorageBytes + sizeof(CFRange), &rangeStorage, sizeof(CFDataRef)); // The data
        rangeStorageBytes += (sizeof(CFRange) + sizeof(CFDataRef));
        foundCount++;
    }

    if (foundCount > 0) {
        CFIndex cnt;
        CFMutableArrayRef array;
        const CFArrayCallBacks callbacks = {0, __rangeRetain, __rangeRelease, __rangeCopyDescription, __rangeEqual};

        CFDataSetLength(rangeStorage, foundCount * (sizeof(CFRange) + sizeof(CFDataRef))); // Tighten storage up
        rangeStorageBytes = (uint8_t*)CFDataGetMutableBytePtr(rangeStorage);

        array = CFArrayCreateMutable(alloc, foundCount * sizeof(CFRange*), &callbacks);
        for (cnt = 0; cnt < foundCount; cnt++) {
            // Each element points to the appropriate CFRange in the CFData
            CFArrayAppendValue(array, rangeStorageBytes + cnt * (sizeof(CFRange) + sizeof(CFDataRef)));
        }
        CFRelease(rangeStorage);        // We want the data to go away when all CFRanges inside it are released...
        return array;
    } else {
        return NULL;
    }
}



CFIndex CFStringGetMaximumSizeOfFileSystemRepresentation(CFStringRef string) {
    CFIndex len = CFStringGetLength(string);
    CFStringEncoding enc = CFStringGetFastestEncoding(string);
    switch (enc) {
        case kCFStringEncodingASCII:
        case kCFStringEncodingMacRoman:
            return len * 3 + 1;
        default:
            return len * 9 + 1;
    }
}

Boolean CFStringGetFileSystemRepresentation(CFStringRef string, char* buffer, CFIndex maxBufLen) {
    return CFStringGetCString(string, buffer, maxBufLen, CFStringFileSystemEncoding());
}

CFStringEncoding CFStringGetSystemEncoding(void) {

    if (__CFDefaultSystemEncoding == kCFStringEncodingInvalidId) {
        const _CFStringEncodingConverter* converter = NULL;
        __CFDefaultSystemEncoding = CFPlatformGetSystemEncoding();
        converter = _CFStringEncodingGetConverter(__CFDefaultSystemEncoding);

        __CFSetCharToUniCharFunc(converter->encodingClass == kCFStringEncodingConverterCheapEightBit ?
            (UNI_CHAR_FUNC)converter->toUnicode :
            NULL);
    }

    return __CFDefaultSystemEncoding;
}

/* ??? Is returning length when no other answer is available the right thing?
 */
CFIndex CFStringGetMaximumSizeForEncoding(CFIndex length, CFStringEncoding encoding) {
    if (encoding == kCFStringEncodingUTF8) {
        return length * 3;
    } else if ((encoding == kCFStringEncodingUTF32) || (encoding == kCFStringEncodingUTF32BE) || (encoding == kCFStringEncodingUTF32LE)) { // UTF-32
        return length * sizeof(UTF32Char);
    } else {
        encoding &= 0xFFF; // Mask off non-base part
    }
    switch (encoding) {
        case kCFStringEncodingUnicode:
            return length * sizeof(UniChar);

        case kCFStringEncodingNonLossyASCII:
            return length * 6; // 1 Unichar could expand to 6 bytes

        case kCFStringEncodingMacRoman:
        case kCFStringEncodingWindowsLatin1:
        case kCFStringEncodingISOLatin1:
        case kCFStringEncodingNextStepLatin:
        case kCFStringEncodingASCII:
            return length / sizeof(uint8_t);

        default:
            return length / sizeof(uint8_t);
    }
}

void CFShow(CFTypeRef obj) {
    CFStringRef str;
    if (obj) {
        if (CFGetTypeID(obj) == CFStringGetTypeID()) {
            str = CFCopyFormattingDescription(obj, NULL);
            if (!str) {
                str = CFCopyDescription(obj);
            }
        } else {
            str = CFCopyDescription(obj);
        }
    } else {
        str = (CFStringRef)CFRetain(CFSTR("(null)"));
    }
    CFPlatformLog("CoreFoundation", kCFLogLevelDebug, str);
    CFRelease(str);
}

void CFShowStr(CFStringRef str) {
    CFAllocatorRef alloc;

    if (!str) {
        fprintf(stdout, "(null)\n");
        return;
    }

    if (CF_IS_OBJC(str)) {
        fprintf(stdout, "%p is an NSString, not CFString\n", str);
        return;
    }

    alloc = CFGetAllocator(str);

    fprintf(stdout, "\nLength %d\nIsEightBit %d\n",
        (int)__CFStrLength(str), __CFStrIsEightBit(str));
    fprintf(stdout, "HasLengthByte %d\nHasNullByte %d\nInlineContents %d\n",
        __CFStrHasLengthByte(str), __CFStrHasNullByte(str), __CFStrIsInline(str));

    fprintf(stdout, "Allocator ");
    if (alloc != kCFAllocatorSystemDefault) {
        fprintf(stdout, "%p\n", alloc);
    } else {
        fprintf(stdout, "SystemDefault\n");
    }

    fprintf(stdout, "Mutable %d\n", __CFStrIsMutable(str));
    if (__CFStrIsMutable(str)) {
        if (__CFStrHasContentsAllocator(str)) {
            fprintf(stdout, "ExternalContentsAllocator %p\n", 
                __CFStrContentsAllocator(CF_CONST_CAST(CFMutableStringRef, str)));
        }
        fprintf(stdout, "CurrentCapacity %d\n%sCapacity %d\n", 
            (int)__CFStrCapacity(str), 
            __CFStrIsFixed(str) ? "Fixed" : "Desired", 
            (int)__CFStrDesiredCapacity(str));
    } else {
        if (__CFStrHasContentsDeallocator(str)) {
            if (__CFStrContentsDeallocator(str)) {
                fprintf(stdout, "ContentsDeallocatorFunc %p\n", 
                    __CFStrContentsDeallocator(str));
            } else {
                fprintf(stdout, "ContentsDeallocatorFunc None\n");
            }
        }
    }

    fprintf(stdout, "Contents %p\n", __CFStrContents(str));
}
