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

#if !defined(__COREFOUNDATION_CFUNICHAR__)
#define __COREFOUNDATION_CFUNICHAR__ 1

#include <CoreFoundation/CFByteOrder.h>
#include <CoreFoundation/CFBase.h>

CF_EXTERN_C_BEGIN

enum {
    kCFUniCharMaxDecomposedLength = 10
};

enum {
    kCFUniCharBitShiftForByte = 3,
    kCFUniCharBitShiftForMask = 7
};

enum {
    kCFUniCharUTF16Format = 0,
    kCFUniCharUTF8Format  = 2,
    kCFUniCharUTF32Format = 3
};

enum {
    kCFUniCharControlCharacterSet = 1,
    kCFUniCharWhitespaceCharacterSet,
    kCFUniCharWhitespaceAndNewlineCharacterSet,
    kCFUniCharDecimalDigitCharacterSet,
    kCFUniCharLetterCharacterSet,
    kCFUniCharLowercaseLetterCharacterSet,
    kCFUniCharUppercaseLetterCharacterSet,
    kCFUniCharNonBaseCharacterSet,
    kCFUniCharCanonicalDecomposableCharacterSet,
    kCFUniCharDecomposableCharacterSet = kCFUniCharCanonicalDecomposableCharacterSet,
    kCFUniCharAlphaNumericCharacterSet,
    kCFUniCharPunctuationCharacterSet,
    kCFUniCharIllegalCharacterSet,
    kCFUniCharTitlecaseLetterCharacterSet,
    kCFUniCharSymbolAndOperatorCharacterSet,
    kCFUniCharNewlineCharacterSet,

    kCFUniCharCompatibilityDecomposableCharacterSet = 100, // internal character sets begins here
    kCFUniCharHFSPlusDecomposableCharacterSet,
    kCFUniCharStrongRightToLeftCharacterSet,
    kCFUniCharHasNonSelfLowercaseCharacterSet,
    kCFUniCharHasNonSelfUppercaseCharacterSet,
    kCFUniCharHasNonSelfTitlecaseCharacterSet,
    kCFUniCharHasNonSelfCaseFoldingCharacterSet,
    kCFUniCharHasNonSelfMirrorMappingCharacterSet,
    kCFUniCharControlAndFormatterCharacterSet,
    kCFUniCharCaseIgnorableCharacterSet,
    kCFUniCharGraphemeExtendCharacterSet
};

enum {
    kCFUniCharBitmapFilled = (uint8_t)0,
    kCFUniCharBitmapEmpty = (uint8_t)0xFF,
    kCFUniCharBitmapAll = (uint8_t)1
};

enum {
    kCFUniCharToLowercase = 0,
    kCFUniCharToUppercase,
    kCFUniCharToTitlecase,
    kCFUniCharCaseFold,

    kCFUniCharCanonicalDecompMapping,
    kCFUniCharCanonicalPrecompMapping,
    kCFUniCharCompatibilityDecompMapping
};

enum {
    kCFUniCharCaseMapFinalSigma = (1),
    kCFUniCharCaseMapAfter_i = (1 << 1),
    kCFUniCharCaseMapMoreAbove = (1 << 2)
};

enum {
    kCFUniCharBiDiPropertyON = 0,
    kCFUniCharBiDiPropertyL,
    kCFUniCharBiDiPropertyR,
    kCFUniCharBiDiPropertyAN,
    kCFUniCharBiDiPropertyEN,
    kCFUniCharBiDiPropertyAL,
    kCFUniCharBiDiPropertyNSM,
    kCFUniCharBiDiPropertyCS,
    kCFUniCharBiDiPropertyES,
    kCFUniCharBiDiPropertyET,
    kCFUniCharBiDiPropertyBN,
    kCFUniCharBiDiPropertyS,
    kCFUniCharBiDiPropertyWS,
    kCFUniCharBiDiPropertyB,
    kCFUniCharBiDiPropertyRLO,
    kCFUniCharBiDiPropertyRLE,
    kCFUniCharBiDiPropertyLRO,
    kCFUniCharBiDiPropertyLRE,
    kCFUniCharBiDiPropertyPDF
};

enum {
    kCFUniCharCombiningProperty = 0,
    kCFUniCharBidiProperty
};

enum {
    kCFUniCharRecursiveDecompositionFlag = (1 << 30),
    kCFUniCharNonBmpFlag = (1 << 31)
};
#define _CFUniCharConvertCountToFlag(count) (((count) & 0x1F) << 24)
#define _CFUniCharConvertFlagToCount(flag) (((flag) >> 24) & 0x1F)

CF_EXPORT const void *_CFUniCharGetMappingData(uint32_t type);

CF_INLINE bool _CFUniCharIsSurrogateHighCharacter(UniChar character) {
    return ((character >= 0xD800UL) && (character <= 0xDBFFUL) ? true : false);
}

CF_INLINE bool _CFUniCharIsSurrogateLowCharacter(UniChar character) {
    return ((character >= 0xDC00UL) && (character <= 0xDFFFUL) ? true : false);
}

CF_INLINE UTF32Char _CFUniCharGetLongCharacterForSurrogatePair(UniChar surrogateHigh, UniChar surrogateLow) {
    return ((surrogateHigh - 0xD800UL) << 10) + (surrogateLow - 0xDC00UL) + 0x0010000UL;
}

/* Character class functions UnicodeData-2_1_5.txt
*/
CF_INLINE Boolean _CFUniCharIsWhitespace(UniChar theChar) {
    return (theChar < 0x21) ||
        (theChar > 0x7E && theChar < 0xA1) ||
        (theChar >= 0x2000 && theChar <= 0x200B) ||
        (theChar == 0x3000);
}

CF_INLINE bool _CFUniCharIsMemberOfBitmap(UTF16Char theChar, const uint8_t* bitmap) {
    return (bitmap && (bitmap[(theChar) >> kCFUniCharBitShiftForByte] & (((uint32_t)1) << (theChar & kCFUniCharBitShiftForMask))) ? true : false);
}

CF_INLINE void _CFUniCharAddCharacterToBitmap(UTF16Char theChar, uint8_t* bitmap) {
    bitmap[(theChar) >> kCFUniCharBitShiftForByte] |= (((uint32_t)1) << (theChar & kCFUniCharBitShiftForMask));
}

CF_INLINE void _CFUniCharRemoveCharacterFromBitmap(UTF16Char theChar, uint8_t* bitmap) {
    bitmap[(theChar) >> kCFUniCharBitShiftForByte] &= ~(((uint32_t)1) << (theChar & kCFUniCharBitShiftForMask));
}

CF_EXPORT bool _CFUniCharIsMemberOf(UTF32Char theChar, uint32_t charset);

/* This function returns NULL for 
 *      kCFUniCharControlCharacterSet,
 *      kCFUniCharWhitespaceCharacterSet,
 *      kCFUniCharWhitespaceAndNewlineCharacterSet,
 *      kCFUniCharIllegalCharacterSet
 */
CF_EXPORT const uint8_t* _CFUniCharGetBitmapPtrForPlane(uint32_t charset, uint32_t plane);

CF_EXPORT uint8_t _CFUniCharGetBitmapForPlane(uint32_t charset, uint32_t plane, uint8_t* bitmap, bool isInverted);
CF_EXPORT uint32_t _CFUniCharGetNumberOfPlanes(uint32_t charset);

CF_EXPORT CFIndex _CFUniCharMapCaseTo(UTF32Char theChar, UTF16Char* convertedChar, CFIndex maxLength, uint32_t ctype, uint32_t flags, const uint8_t* langCode);
CF_EXPORT uint32_t _CFUniCharGetConditionalCaseMappingFlags(UTF32Char theChar, UTF16Char* buffer, CFIndex currentIndex, CFIndex length, uint32_t type, const uint8_t* langCode, uint32_t lastFlags);

// The second arg 'bitmap' has to be the pointer to a specific plane
CF_EXPORT uint8_t _CFUniCharGetBidiPropertyForCharacter(UTF16Char character, const uint8_t* bitmap);
CF_EXPORT uint8_t _CFUniCharGetCombiningPropertyForCharacter(UTF16Char character, const uint8_t* bitmap);

CF_EXPORT const void* _CFUniCharGetUnicodePropertyDataForPlane(uint32_t propertyType, uint32_t plane);
CF_EXPORT uint32_t _CFUniCharGetNumberOfPlanesForUnicodePropertyData(uint32_t propertyType);
CF_EXPORT uint32_t _CFUniCharGetUnicodeProperty(UTF32Char character, uint32_t propertyType);

CF_EXPORT bool _CFUniCharFillDestinationBuffer(const UTF32Char* src, CFIndex srcLength, void** dst, CFIndex dstLength, CFIndex* filledLength, uint32_t dstFormat);

// UTF32 support
CF_EXPORT bool _CFUniCharToUTF32(const UTF16Char* src, CFIndex length, UTF32Char* dst, bool allowLossy, bool isBigEndien);
CF_EXPORT bool _CFUniCharFromUTF32(const UTF32Char* src, CFIndex length, UTF16Char* dst, bool allowLossy, bool isBigEndien);

/* Decomposition */

CF_INLINE bool _CFUniCharIsDecomposableCharacter(UTF32Char character) {
    if (character < 0x80) {
        return false;
    }
    return _CFUniCharIsMemberOf(character, kCFUniCharHFSPlusDecomposableCharacterSet);
}

CF_EXPORT CFIndex _CFUniCharDecomposeCharacter(UTF32Char character, UTF32Char* convertedChars, CFIndex maxBufferLength);
CF_EXPORT CFIndex _CFUniCharCompatibilityDecompose(UTF32Char* convertedChars, CFIndex length, CFIndex maxBufferLength);

CF_EXPORT bool _CFUniCharDecompose(const UTF16Char* src, CFIndex length, CFIndex* consumedLength, void* dst, CFIndex maxLength, CFIndex* filledLength, bool needToReorder, uint32_t dstFormat, bool isHFSPlus);

//TODO Move to CFUniChar.c
CF_EXPORT void _CFUniCharPrioritySort(UTF32Char* characters, CFIndex length);

/* Precomposition */

// This function cannot precompose Hangul Jamo.
CF_EXPORT UTF32Char _CFUniCharPrecomposeCharacter(UTF32Char base, UTF32Char combining);

CF_EXTERN_C_END

#endif /* ! __COREFOUNDATION_CFUNICHAR__ */
