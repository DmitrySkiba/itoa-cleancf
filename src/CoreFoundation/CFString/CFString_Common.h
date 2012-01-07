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

#if !defined(__COREFOUNDATION_CFSTRING_COMMON__)
#define __COREFOUNDATION_CFSTRING_COMMON__ 1

#define CF_VALIDATE_STRING_ARG(string) \
    CF_VALIDATE_OBJECT_ARG(CF, string, _kCFStringTypeID)

#define CF_VALIDATE_STRING_INDEX_ARG(string, index) \
    CF_VALIDATE_INDEX_ARG(index, __CFStrLength(string))

#define CF_VALIDATE_STRING_RANGE_ARG(string, range) \
    CF_VALIDATE_RANGE_ARG(range, __CFStrLength(string))

#define CF_VALIDATE_STRINGENCODING_ARG(encoding) \
	CF_VALIDATE_ARG(CFStringIsEncodingAvailable(encoding), \
		"encoding %d is not available", (int)encoding)

// The only mutable variant for CFString.
struct __notInlineMutable {
    void* buffer;
    CFIndex length;
    CFIndex capacity; // Capacity in bytes
    unsigned int hasGap : 1; // Currently unused
    unsigned int isFixedCapacity : 1;
    unsigned int isExternalMutable : 1;
    unsigned int capacityProvidedExternally : 1;
    unsigned long desiredCapacity : 28;
    CFAllocatorRef contentsAllocator; // Optional
};

/* !!! Never do sizeof(CFString); the union is here just to 
 *  make it easier to access some fields.

 * !!! Assumptions:
 *  - Mutable strings are not inline;
 *  - Compile-time constant strings are not inline;
 *  - Mutable strings always have explicit length (but they might 
 *     also have length byte and null byte);
 *  - If there is an explicit length, always use that instead of the 
 *     length byte (length byte is useful for quickly returning pascal 
 *     strings);
 *  - Never look at the length byte for the length, use __CFStrLength 
 *     or __CFStrLength2.
 *
 * Note that the 'buffer' variable is in the same place 
 *  for all non-inline variants of CFString.
 */
struct __CFString {
    CFRuntimeBase base;
    union {
        // In many cases the allocated structs are smaller than these.

        // Inline; bytes follow the length.
        struct __inline1 {
            CFIndex length;
        } inline1;
        
        // Usual not-inline immutable CFString.
        struct __notInlineImmutable1 {
            void* buffer; 
            CFIndex length;
            CFAllocatorRef contentsDeallocator; // Optional; just the dealloc func is used
        } notInlineImmutable1;

        // This is the not-inline immutable CFString when 
        //  length is stored with the contents (first byte).
        struct __notInlineImmutable2 {
            void* buffer;
            CFAllocatorRef contentsDeallocator; // Optional; just the dealloc func is used
        } notInlineImmutable2;

        struct __notInlineMutable notInlineMutable;
    } variants;
};

/*
 * I = is immutable
 * E = not inline contents
 * U = is Unicode
 * N = has NULL byte
 * L = has length byte
 * D = explicit deallocator for contents (for mutable objects, allocator)
 * C = length field is CFIndex (rather than UInt32); only meaningful for 64-bit,
 *     really if needed this bit (valuable real-estate) can be given up for another 
 *     bit elsewhere, since this info is needed just for 64-bit.
 *
 * Also need (only for mutable)
 * F = is fixed
 * G = has gap
 * Cap, DesCap = capacity
 *
 * B7 B6 B5 B4 B3 B2 B1 B0
 *       U  N  L  C  I
 *
 * B6 B5
 * 0  0   inline contents
 * 0  1   E (freed with default allocator)
 * 1  0   E (not freed)
 * 1  1   E D
 *
 * !!! Note: Constant CFStrings use the bit patterns:
 * C8 (11001000 = default allocator, not inline, not freed contents; 8-bit;
 *                has NULL byte; doesn't have length; is immutable)
 * D0 (11010000 = default allocator, not inline, not freed contents;
 *                Unicode; is immutable)
 *
 * The bit usages should not be modified in a way that would effect 
 *  these bit patterns.
 */

enum {
    __kCFFreeContentsWhenDoneMask = 0x020,
    __kCFFreeContentsWhenDone = 0x020,
    __kCFContentsMask = 0x060,
    __kCFHasInlineContents = 0x000,
    __kCFNotInlineContentsNoFree = 0x040, // Don't free
    __kCFNotInlineContentsDefaultFree = 0x020, // Use allocator's free function
    __kCFNotInlineContentsCustomFree = 0x060, // Use a specially provided free function
    __kCFHasContentsAllocatorMask = 0x060,
    __kCFHasContentsAllocator = 0x060, // (For mutable strings) use a specially provided allocator
    __kCFHasContentsDeallocatorMask = 0x060,
    __kCFHasContentsDeallocator = 0x060,
    __kCFIsMutableMask = 0x01,
    __kCFIsMutable = 0x01,
    __kCFIsUnicodeMask = 0x10,
    __kCFIsUnicode = 0x10,
    __kCFHasNullByteMask = 0x08,
    __kCFHasNullByte = 0x08,
    __kCFHasLengthByteMask = 0x04,
    __kCFHasLengthByte = 0x04,
    // !!! Bit 0x02 has been freed up
};

///////////////////////////////////////////////////////////////////// private

/* The following set of functions and macros need to be updated on change to the bit configuration
 */
CF_INLINE Boolean __CFStrIsMutable(CFStringRef str) {
    return (CF_INFO(str) & __kCFIsMutableMask) == __kCFIsMutable;
}

CF_INLINE Boolean __CFStrIsInline(CFStringRef str) {
    return (CF_INFO(str) & __kCFContentsMask) == __kCFHasInlineContents;
}

CF_INLINE Boolean __CFStrFreeContentsWhenDone(CFStringRef str) {
    return (CF_INFO(str) & __kCFFreeContentsWhenDoneMask) == __kCFFreeContentsWhenDone;
}

CF_INLINE Boolean __CFStrHasContentsDeallocator(CFStringRef str) {
    return (CF_INFO(str) & __kCFHasContentsDeallocatorMask) == __kCFHasContentsDeallocator;
}

CF_INLINE Boolean __CFStrIsUnicode(CFStringRef str) {
    return (CF_INFO(str) & __kCFIsUnicodeMask) == __kCFIsUnicode;
}

CF_INLINE Boolean __CFStrIsEightBit(CFStringRef str) {
    return (CF_INFO(str) & __kCFIsUnicodeMask) != __kCFIsUnicode;
}

CF_INLINE Boolean __CFStrHasNullByte(CFStringRef str) {
    return (CF_INFO(str) & __kCFHasNullByteMask) == __kCFHasNullByte;
}

CF_INLINE Boolean __CFStrHasLengthByte(CFStringRef str) {
    return (CF_INFO(str) & __kCFHasLengthByteMask) == __kCFHasLengthByte;
}

// Has explicit length if (1) mutable or (2) not mutable and no length byte.
CF_INLINE Boolean __CFStrHasExplicitLength(CFStringRef str) {
    return (CF_INFO(str) & (__kCFIsMutableMask | __kCFHasLengthByteMask)) != __kCFHasLengthByte;
}

CF_INLINE Boolean __CFStrIsConstant(CFStringRef str) {
    //TODO Rename to _CFRuntimeIsConst, put together with 
    //  _CFRuntimeMakeConst (see CFString_Const.c).
    return CF_BASE(str)->_rc == 0;
}

// Number of bytes to skip over the length byte in the contents
CF_INLINE SInt32 __CFStrSkipAnyLengthByte(CFStringRef str) {
    return ((CF_INFO(str) & __kCFHasLengthByteMask) == __kCFHasLengthByte) ? 1 : 0;
}

// Returns ptr to the buffer (which might include the length byte).
CF_INLINE const void* __CFStrContents(CFStringRef str) {
    if (__CFStrIsInline(str)) {
        return CF_CAST(uint8_t*, &str->variants) + 
            (__CFStrHasExplicitLength(str) ? sizeof(CFIndex) : 0);
    } else {
        // Not inline; pointer is always word 2
        return str->variants.notInlineImmutable1.buffer;
    }
}

/* Sets the content pointer for immutable or mutable strings.
 */
CF_INLINE void __CFStrSetContentPtr(CFStringRef str, const void* p) {
    // TODO catch all writes for mutable string case.
    ((CFMutableStringRef)str)->variants.notInlineImmutable1.buffer = (void*)p;
}

/* Returns length; use __CFStrLength2 if contents buffer pointer has 
 *  already been computed.
 */
CF_INLINE CFIndex __CFStrLength(CFStringRef str) {
    if (__CFStrHasExplicitLength(str)) {
        if (__CFStrIsInline(str)) {
            return str->variants.inline1.length;
        } else {
            return str->variants.notInlineImmutable1.length;
        }
    } else {
        return *(uint8_t*)__CFStrContents(str);
    }
}
CF_INLINE CFIndex __CFStrLength2(CFStringRef str, const void* buffer) {
    if (__CFStrHasExplicitLength(str)) {
        if (__CFStrIsInline(str)) {
            return str->variants.inline1.length;
        } else {
            return str->variants.notInlineImmutable1.length;
        }
    } else {
        return *(uint8_t*)buffer;
    }
}

CF_INLINE void __CFStrSetExplicitLength(CFStringRef str, CFIndex v) {
    if (__CFStrIsInline(str)) {
        ((CFMutableStringRef)str)->variants.inline1.length = v;
    } else {
        ((CFMutableStringRef)str)->variants.notInlineImmutable1.length = v;
    }
}

/* "Capacity" is stored in number of bytes, not characters.
 *  It indicates the total number of bytes in the contents buffer.
 */
CF_INLINE CFIndex __CFStrCapacity(CFStringRef str) {
    return str->variants.notInlineMutable.capacity;
}
CF_INLINE void __CFStrSetCapacity(CFMutableStringRef str, CFIndex cap) {
    str->variants.notInlineMutable.capacity = cap;
}

// Assumption: The following set of inlines (using str->variants.notInlineMutable) 
//  are called with mutable strings only.
CF_INLINE Boolean __CFStrIsFixed(CFStringRef str) {
    return str->variants.notInlineMutable.isFixedCapacity;
}
CF_INLINE void __CFStrSetIsFixed(CFMutableStringRef str) {
    str->variants.notInlineMutable.isFixedCapacity = 1;
}

CF_INLINE Boolean __CFStrIsExternalMutable(CFStringRef str) {
    return str->variants.notInlineMutable.isExternalMutable;
}
CF_INLINE void __CFStrSetIsExternalMutable(CFMutableStringRef str) {
    str->variants.notInlineMutable.isExternalMutable = 1;
}

CF_INLINE void __CFStrSetUnicode(CFMutableStringRef str) {
    str->base._cfinfo[CF_INFO_BITS] |= __kCFIsUnicode;
}
CF_INLINE void __CFStrClearUnicode(CFMutableStringRef str) {
    str->base._cfinfo[CF_INFO_BITS] &= ~__kCFIsUnicode;
}

CF_INLINE void __CFStrSetHasLengthAndNullBytes(CFMutableStringRef str) {
    str->base._cfinfo[CF_INFO_BITS] |= (__kCFHasLengthByte | __kCFHasNullByte);
}
CF_INLINE void __CFStrClearHasLengthAndNullBytes(CFMutableStringRef str) {
    str->base._cfinfo[CF_INFO_BITS] &= ~(__kCFHasLengthByte | __kCFHasNullByte);
}

// If capacity is provided externally, we only change it when we need to grow beyond it
CF_INLINE Boolean __CFStrCapacityProvidedExternally(CFStringRef str) {
    return str->variants.notInlineMutable.capacityProvidedExternally;
}
CF_INLINE void __CFStrSetCapacityProvidedExternally(CFMutableStringRef str) {
    str->variants.notInlineMutable.capacityProvidedExternally = 1;
}
CF_INLINE void __CFStrClearCapacityProvidedExternally(CFMutableStringRef str) {
    str->variants.notInlineMutable.capacityProvidedExternally = 0;
}

/* "Desired capacity" is in number of characters; it is the client
 *  requested capacity; if fixed, it is the upper bound on the mutable 
 *  string backing store.
 */
CF_INLINE CFIndex __CFStrDesiredCapacity(CFStringRef str) {
    return str->variants.notInlineMutable.desiredCapacity;
}
CF_INLINE void __CFStrSetDesiredCapacity(CFMutableStringRef str, CFIndex size) {
    str->variants.notInlineMutable.desiredCapacity = size;
}

CF_INLINE void __CFStrSetInfoBits(CFStringRef str, UInt32 v) {
    _CFBitfieldSetValue(CF_INFO(str), 6, 0, v);
}

/* Returns whether a length byte can be tacked on to a string 
 *  of the indicated length.
 */
CF_INLINE Boolean __CFCanUseLengthByte(CFIndex len) {
    #define __kCFMaxPascalStrLen 255
    return (len <= __kCFMaxPascalStrLen);
}

///////////////////////////////////////////////////

CF_INLINE Boolean __CFStrHasContentsAllocator(CFStringRef str) {
    return (CF_INFO(str) & __kCFHasContentsAllocatorMask) == __kCFHasContentsAllocator;
}

CF_INLINE CFAllocatorRef* __CFStrContentsDeallocatorPtr(CFStringRef str) {
    return __CFStrHasExplicitLength(str) ?
           (CFAllocatorRef*)&str->variants.notInlineImmutable1.contentsDeallocator :
           (CFAllocatorRef*)&str->variants.notInlineImmutable2.contentsDeallocator;
}

// Assumption: Called with immutable strings only, and on strings that are known to have a contentsDeallocator
CF_INLINE CFAllocatorRef __CFStrContentsDeallocator(CFStringRef str) {
    return *__CFStrContentsDeallocatorPtr(str);
}

// Assumption: Called with immutable strings only, and on strings that are known to have a contentsDeallocator
CF_INLINE void __CFStrSetContentsDeallocator(CFStringRef str, CFAllocatorRef contentsAllocator) {
    *__CFStrContentsDeallocatorPtr(str) = contentsAllocator;
}

CF_INLINE CFAllocatorRef* __CFStrContentsAllocatorPtr(CFStringRef str) {
    return (CFAllocatorRef*)&str->variants.notInlineMutable.contentsAllocator;
}

// Assumption: Called with strings that have a contents allocator; also, contents allocator follows custom
CF_INLINE void __CFStrSetContentsAllocator(CFMutableStringRef str, CFAllocatorRef alloc) {
    *(__CFStrContentsAllocatorPtr(str)) = alloc;
}

// Assumption: Called with strings that have a contents allocator; also, contents allocator follows custom
CF_INLINE CFAllocatorRef __CFStrContentsAllocator(CFMutableStringRef str) {
    return *(__CFStrContentsAllocatorPtr(str));
}

CF_INLINE void* __CFStrAllocateMutableContents(CFMutableStringRef str, CFIndex size) {
    CFAllocatorRef alloc = __CFStrHasContentsAllocator(str) ?
        __CFStrContentsAllocator(str) :
        CFGetAllocator(str);
    return CFAllocatorAllocate(alloc, size, 0);
}

CF_INLINE void __CFStrDeallocateMutableContents(CFMutableStringRef str, void* buffer) {
    CFAllocatorRef alloc = (__CFStrHasContentsAllocator(str)) ?
        __CFStrContentsAllocator(str) :
        CFGetAllocator(str);
    CFAllocatorDeallocate(alloc, buffer);
}

////////////////////////////////////////////////////////////////////////////////////

CF_EXPORT void __CFStringAppendBytes(CFMutableStringRef str, const char* cStr, CFIndex appendedLength, CFStringEncoding encoding);
CF_EXPORT void __CFStrConvertBytesToUnicode(const uint8_t* bytes, UniChar* buffer, CFIndex numChars);

/* result is long long or int, depending on doLonglong*/
CF_EXPORT Boolean _CFStringScanInteger(CFStringInlineBuffer* buf, CFTypeRef locale, SInt32* indexPtr, Boolean doLonglong, void* result);
CF_EXPORT Boolean _CFStringScanDouble(CFStringInlineBuffer* buf, CFTypeRef locale, SInt32* indexPtr, double* resultPtr);

CF_EXPORT void __CFSetCharToUniCharFunc(Boolean (* func)(UInt32 flags, UInt8 ch, UniChar* unicodeChar));
CF_EXPORT UniChar __CFCharToUniCharTable[256];
CF_EXPORT void __CFStrConvertBytesToUnicode(const uint8_t* bytes, UniChar* buffer, CFIndex numChars);

CF_EXPORT CFStringEncoding __CFStringGetEightBitStringEncoding(void);

typedef struct {      /* A simple struct to maintain ASCII/Unicode versions of the same buffer. */
    union {
        UInt8* ascii;
        UniChar* unicode;
    } chars;
    Boolean isASCII;    /* This really does mean 7-bit ASCII, not _NSDefaultCStringEncoding() */
    Boolean shouldFreeChars;    /* If the number of bytes exceeds __kCFVarWidthLocalBufferSize, bytes are allocated */
    Boolean _unused1;
    Boolean _unused2;
    CFAllocatorRef allocator;    /* Use this allocator to allocate, reallocate, and deallocate the bytes */
    CFIndex numChars;    /* This is in terms of ascii or unicode; that is, if isASCII, it is number of 7-bit chars; otherwise it is number of UniChars; note that the actual allocated space might be larger */
    UInt8 localBuffer[1008];    /* private; 168 ISO2022JP chars, 504 Unicode chars, 1008 ASCII chars */
} CFVarWidthCharBuffer;

/* Convert a byte stream to ASCII (7-bit!) or Unicode, with a CFVarWidthCharBuffer struct on the stack. false return indicates an error occured during the conversion. Depending on .isASCII, follow .chars.ascii or .chars.unicode.  If .shouldFreeChars is returned as true, free the returned buffer when done with it.  If useClientsMemoryPtr is provided as non-NULL, and the provided memory can be used as is, this is set to true, and the .ascii or .unicode buffer in CFVarWidthCharBuffer is set to bytes.
 * !!! If the stream is Unicode and has no BOM, the data is assumed to be big endian! Could be trouble on Intel if someone didn't follow that assumption.
 */
CF_EXPORT Boolean __CFStringDecodeByteStream(const UInt8* bytes, CFIndex len, CFStringEncoding encoding, Boolean alwaysUnicode, CFVarWidthCharBuffer* buffer, Boolean* useClientsMemoryPtr, UInt32 converterFlags);

CF_EXPORT const char* _CFStrGetLanguageIdentifierForLocale(CFLocaleRef locale);

CFHashCode __CFStrHashEightBit(const uint8_t* cContents, CFIndex len);
CFHashCode __CFStrHashCharacters(const UniChar* uContents, CFIndex len, CFIndex actualLen);
void __CFStringChangeSizeMultiple(CFMutableStringRef str, const CFRange* deleteRanges, CFIndex numDeleteRanges, CFIndex insertLength, Boolean makeUnicode);
void __CFStringChangeSize(CFMutableStringRef str, CFRange range, CFIndex insertLength, Boolean makeUnicode);
Boolean _CFStringIsConstantString(CFStringRef str);

bool _CFCanUseLocale(CFLocaleRef locale);


#define MAX_CASE_MAPPING_BUF (8)

#endif /* !__COREFOUNDATION_CFSTRING_COMMON__ */
