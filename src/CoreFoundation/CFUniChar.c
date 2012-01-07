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

#include <CoreFoundation/CFByteOrder.h>
#include "CFInternal.h"
#include "CFUniChar.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>

#define CF_UNICHAR_BITMAP_FILE "CFCharacterSetBitmaps.bitmap"
#include "CFUniChar_BitmapData.inl"

#if __CF_BIG_ENDIAN__
    #define CF_UNICHAR_MAPPING_FILE "CFUnicodeData-B.mapping"
    #include "CFUniChar_MappingDataB.inl"
#else
    #define CF_UNICHAR_MAPPING_FILE "CFUnicodeData-L.mapping"
    #include "CFUniChar_MappingDataL.inl"
#endif

#define CF_UNICHAR_PROPERTY_FILE "CFUniCharPropertyDatabase.data"
#include "CFUniChar_PropertyData.inl"

#if __CF_BIG_ENDIAN__
    #define TURKISH_LANG_CODE    (0x7472)    // tr
    #define LITHUANIAN_LANG_CODE (0x6C74)    // lt
    #define AZERI_LANG_CODE      (0x617A)    // az
#else
    #define TURKISH_LANG_CODE    (0x7274)    // tr
    #define LITHUANIAN_LANG_CODE (0x746C)    // lt
    #define AZERI_LANG_CODE      (0x7A61)    // az
#endif

enum {
    kCFUniCharLastExternalSet = kCFUniCharNewlineCharacterSet,
    kCFUniCharFirstInternalSet = kCFUniCharCompatibilityDecomposableCharacterSet,
    kCFUniCharLastInternalSet = kCFUniCharGraphemeExtendCharacterSet,
    kCFUniCharFirstBitmapSet = kCFUniCharDecimalDigitCharacterSet
};

#define NUM_CASE_MAP_DATA (kCFUniCharCaseFold + 1)

typedef struct {
    uint32_t _numPlanes;
    const uint8_t** _planes;
} __CFUniCharBitmapData;

typedef struct {
    uint32_t _key;
    uint32_t _value;
} __CFUniCharCaseMappings;

static char __CFUniCharUnicodeVersionString[8] = {0, 0, 0, 0, 0, 0, 0, 0};
static uint32_t __CFUniCharNumberOfBitmaps = 0;
static __CFUniCharBitmapData* __CFUniCharBitmapDataArray = NULL;
static CFSpinLock_t __CFUniCharBitmapLock = CFSpinLockInit;

static uint32_t* __CFUniCharCaseMappingTableCounts = NULL;
static uint32_t** __CFUniCharCaseMappingTable = NULL;
static const uint32_t** __CFUniCharCaseMappingExtraTable = NULL;
static const void** __CFUniCharMappingTables = NULL;
static CFSpinLock_t __CFUniCharMappingTableLock = CFSpinLockInit;

static __CFUniCharBitmapData* __CFUniCharUnicodePropertyTable = NULL;
static int __CFUniCharUnicodePropertyTableCount = 0;
static CFSpinLock_t __CFUniCharPropTableLock = CFSpinLockInit;

///////////////////////////////////////////////////////////////////// private

CF_INLINE uint32_t __CFUniCharMapExternalSetToInternalIndex(uint32_t cset) {
    return ((kCFUniCharFirstInternalSet <= cset) ? ((cset - kCFUniCharFirstInternalSet) + kCFUniCharLastExternalSet) : cset) - kCFUniCharFirstBitmapSet;
}

CF_INLINE uint32_t __CFUniCharMapCompatibilitySetID(uint32_t cset) {
    return ((cset == kCFUniCharControlCharacterSet) ?
        kCFUniCharControlAndFormatterCharacterSet :
        (((cset > kCFUniCharLastExternalSet) && (cset < kCFUniCharFirstInternalSet)) ?
            ((cset - kCFUniCharLastExternalSet) + kCFUniCharFirstInternalSet) :
            cset));
}

// Bitmap functions
CF_INLINE bool isControl(UTF32Char theChar, uint16_t charset, const void* data) { // ISO Control
    return  (theChar <= 0x001F) ||
            (theChar >= 0x007F && theChar <= 0x009F);
}

//TODO rename to __CFxxx (all others too)
CF_INLINE bool isWhitespace(UTF32Char theChar, uint16_t charset, const void* data) { // Space
    return  (theChar == 0x0020) || 
            (theChar == 0x0009) || 
            (theChar == 0x00A0) || 
            (theChar == 0x1680) || 
            (theChar >= 0x2000 && theChar <= 0x200B) || 
            (theChar == 0x202F) || 
            (theChar == 0x205F) ||
            (theChar == 0x3000);
}

CF_INLINE bool isNewline(UTF32Char theChar, uint16_t charset, const void* data) { // White space
    return  (theChar >= 0x000A && theChar <= 0x000D) ||
            (theChar == 0x0085) ||
            (theChar == 0x2028) ||
            (theChar == 0x2029);
}

CF_INLINE bool isWhitespaceAndNewline(UTF32Char theChar, uint16_t charset, const void* data) { // White space
    return  isWhitespace(theChar, charset, data) ||
            isNewline(theChar, charset, data);
}

static bool __CFUniCharLoadFile(const char* bitmapName, const uint8_t** bytes) {
    if (!strcmp(bitmapName, CF_UNICHAR_BITMAP_FILE)) {
        *bytes = __CFUniCharBitmapDataBytes;
        return true;
    }
    if (!strcmp(bitmapName, CF_UNICHAR_MAPPING_FILE)) {
        *bytes = __CFUniCharMappingDataBytes;
        return true;
    }
    if (!strcmp(bitmapName, CF_UNICHAR_PROPERTY_FILE)) {
        *bytes = __CFUniCharPropertyDataBytes;
        return true;
    }
    return false;
}

static bool __CFUniCharLoadBitmapData(void) {
    __CFUniCharBitmapData* array;
    uint32_t headerSize;
    uint32_t bitmapSize;
    int numPlanes;
    uint8_t currentPlane;
    const uint8_t* bytes;
    const uint8_t* bitmapBase;
    const uint8_t* bitmap;
    int idx, bitmapIndex;

    CFSpinLock(&__CFUniCharBitmapLock);

    if (__CFUniCharBitmapDataArray || !__CFUniCharLoadFile(CF_UNICHAR_BITMAP_FILE, &bytes)) {
        CFSpinUnlock(&__CFUniCharBitmapLock);
        return false;
    }

    for (idx = 0; idx < 4 && bytes[idx]; idx++) {
        __CFUniCharUnicodeVersionString[idx * 2] = bytes[idx];
        __CFUniCharUnicodeVersionString[idx * 2 + 1] = '.';
    }
    __CFUniCharUnicodeVersionString[(idx < 4 ? idx * 2 - 1 : 7)] = '\0';

    headerSize = CFSwapInt32BigToHost(*((uint32_t*)(bytes + 4)));

    bitmapBase = bytes + headerSize;
    bytes = bytes + (sizeof(uint32_t) * 2);
    headerSize -= (sizeof(uint32_t) * 2);

    __CFUniCharNumberOfBitmaps = headerSize / (sizeof(uint32_t) * 2);

    array = (__CFUniCharBitmapData*)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(__CFUniCharBitmapData) * __CFUniCharNumberOfBitmaps, 0);

    for (idx = 0; idx < (int)__CFUniCharNumberOfBitmaps; idx++) {
        bitmap = bitmapBase + CFSwapInt32BigToHost(*((uint32_t*)bytes));
        bytes = bytes + sizeof(uint32_t);
        bitmapSize = CFSwapInt32BigToHost(*((uint32_t*)bytes));
        bytes = bytes + sizeof(uint32_t);

        numPlanes = bitmapSize / (8 * 1024);
        numPlanes = *(bitmap + (((numPlanes - 1) * ((8 * 1024) + 1)) - 1)) + 1;
        array[idx]._planes = (const uint8_t**)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(const void*) * numPlanes, 0);
        array[idx]._numPlanes = numPlanes;

        currentPlane = 0;
        for (bitmapIndex = 0; bitmapIndex < numPlanes; bitmapIndex++) {
            if (bitmapIndex == currentPlane) {
                array[idx]._planes[bitmapIndex] = bitmap;
                bitmap = bitmap + (8 * 1024);
                currentPlane = *(bitmap++);
            } else {
                array[idx]._planes[bitmapIndex] = NULL;
            }
        }
    }

    __CFUniCharBitmapDataArray = array;

    CFSpinUnlock(&__CFUniCharBitmapLock);

    return true;
}

/* Binary searches __CFUniCharCaseMappings */
static uint32_t __CFUniCharGetMappedCase(const __CFUniCharCaseMappings* theTable, uint32_t numElem, UTF32Char character) {
    const __CFUniCharCaseMappings* p, * q, * divider;

    if ((character < theTable[0]._key) || (character > theTable[numElem - 1]._key)) {
        return 0;
    }
    p = theTable;
    q = p + (numElem - 1);
    while (p <= q) {
        divider = p + ((q - p) / 2);
        if (character < divider->_key) {
            q = divider - 1;
        } else if (character > divider->_key) {
            p = divider + 1;
        } else {
            return divider->_value;
        }
    }
    return 0;
}

static bool __CFUniCharLoadCaseMappingTable(void) {
    uint32_t* countArray;
    int idx;

    if (!__CFUniCharMappingTables) {
        (void)_CFUniCharGetMappingData(kCFUniCharToLowercase);
    }
    if (!__CFUniCharMappingTables) {
        return false;
    }

    CFSpinLock(&__CFUniCharMappingTableLock);

    if (__CFUniCharCaseMappingTableCounts) {
        CFSpinUnlock(&__CFUniCharMappingTableLock);
        return true;
    }

    countArray = (uint32_t*)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(uint32_t) * NUM_CASE_MAP_DATA + sizeof(uint32_t*) * NUM_CASE_MAP_DATA * 2, 0);
    __CFUniCharCaseMappingTable = (uint32_t**)((uint8_t*)countArray + sizeof(uint32_t) * NUM_CASE_MAP_DATA);
    __CFUniCharCaseMappingExtraTable = (const uint32_t**)__CFUniCharCaseMappingTable + NUM_CASE_MAP_DATA;

    for (idx = 0; idx < NUM_CASE_MAP_DATA; idx++) {
        countArray[idx] = *((uint32_t*)__CFUniCharMappingTables[idx]) / (sizeof(uint32_t) * 2);
        __CFUniCharCaseMappingTable[idx] = ((uint32_t*)__CFUniCharMappingTables[idx]) + 1;
        __CFUniCharCaseMappingExtraTable[idx] = (const uint32_t*)((uint8_t*)__CFUniCharCaseMappingTable[idx] + *((uint32_t*)__CFUniCharMappingTables[idx]));
    }

    __CFUniCharCaseMappingTableCounts = countArray;

    CFSpinUnlock(&__CFUniCharMappingTableLock);
    return true;
}

static bool __CFUniCharIsMoreAbove(UTF16Char* buffer, CFIndex length) {
    UTF32Char currentChar;
    uint32_t property;

    while (length-- > 0) {
        currentChar = *(buffer)++;
        if (_CFUniCharIsSurrogateHighCharacter(currentChar) && (length > 0) && _CFUniCharIsSurrogateLowCharacter(*(buffer + 1))) {
            currentChar = _CFUniCharGetLongCharacterForSurrogatePair(currentChar, *(buffer++));
            --length;
        }
        if (!_CFUniCharIsMemberOf(currentChar, kCFUniCharNonBaseCharacterSet)) {
            break;
        }

        property = _CFUniCharGetCombiningPropertyForCharacter(currentChar, (const uint8_t*)_CFUniCharGetUnicodePropertyDataForPlane(kCFUniCharCombiningProperty, (currentChar >> 16) & 0xFF));

        if (property == 230) {
            return true; // Above priority
        }
    }
    return false;
}

static bool __CFUniCharIsAfter_i(UTF16Char* buffer, CFIndex length) {
    UTF32Char currentChar = 0;
    uint32_t property;
    UTF32Char decomposed[kCFUniCharMaxDecomposedLength];
    CFIndex decompLength;
    CFIndex idx;

    if (length < 1) {
        return 0;
    }

    buffer += length;
    while (length-- > 1) {
        currentChar = *(--buffer);
        if (_CFUniCharIsSurrogateLowCharacter(currentChar)) {
            if ((length > 1) && _CFUniCharIsSurrogateHighCharacter(*(buffer - 1))) {
                currentChar = _CFUniCharGetLongCharacterForSurrogatePair(*(--buffer), currentChar);
                --length;
            } else {
                break;
            }
        }
        if (!_CFUniCharIsMemberOf(currentChar, kCFUniCharNonBaseCharacterSet)) {
            break;
        }

        property = _CFUniCharGetCombiningPropertyForCharacter(currentChar, (const uint8_t*)_CFUniCharGetUnicodePropertyDataForPlane(kCFUniCharCombiningProperty, (currentChar >> 16) & 0xFF));

        if (property == 230) {
            return false;                  // Above priority
        }
    }
    if (length == 0) {
        currentChar = *(--buffer);
    } else if (_CFUniCharIsSurrogateLowCharacter(currentChar) && _CFUniCharIsSurrogateHighCharacter(*(--buffer))) {
        currentChar = _CFUniCharGetLongCharacterForSurrogatePair(*buffer, currentChar);
    }

    decompLength = _CFUniCharDecomposeCharacter(currentChar, decomposed, kCFUniCharMaxDecomposedLength);
    currentChar = *decomposed;

    for (idx = 1; idx < decompLength; idx++) {
        currentChar = decomposed[idx];
        property = _CFUniCharGetCombiningPropertyForCharacter(currentChar, (const uint8_t*)_CFUniCharGetUnicodePropertyDataForPlane(kCFUniCharCombiningProperty, (currentChar >> 16) & 0xFF));

        if (property == 230) {
            return false;                  // Above priority
        }
    }
    return true;
}

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL uint8_t _CFUniCharGetBitmapForPlane(uint32_t charset, uint32_t plane, uint8_t* bitmap, bool isInverted) {
    const uint8_t* src = _CFUniCharGetBitmapPtrForPlane(charset, plane);
    int numBytes = (8 * 1024);

    if (src) {
        if (isInverted) {
            while (numBytes-- > 0) {
                *(bitmap++) = ~(*(src++));
            }
        } else {
            while (numBytes-- > 0) {
                *(bitmap++) = *(src++);
            }
        }
        return kCFUniCharBitmapFilled;
    } else if (charset == kCFUniCharIllegalCharacterSet) {
        __CFUniCharBitmapData* data = __CFUniCharBitmapDataArray + __CFUniCharMapExternalSetToInternalIndex(__CFUniCharMapCompatibilitySetID(charset));

        if (plane < data->_numPlanes && (src = data->_planes[plane])) {
            if (isInverted) {
                while (numBytes-- > 0) {
                    *(bitmap++) = *(src++);
                }
            } else {
                while (numBytes-- > 0) {
                    *(bitmap++) = ~(*(src++));
                }
            }
            return kCFUniCharBitmapFilled;
        } else if (plane == 0x0E) { // Plane 14
            int idx;
            uint8_t asciiRange = (isInverted ? (uint8_t)0xFF : (uint8_t)0);
            uint8_t otherRange = (isInverted ? (uint8_t)0 : (uint8_t)0xFF);

            *(bitmap++) = 0x02;      // UE0001 LANGUAGE TAG
            for (idx = 1; idx < numBytes; idx++) {
                *(bitmap++) = ((idx >= (0x20 / 8) && (idx < (0x80 / 8))) ? asciiRange : otherRange);
            }
            return kCFUniCharBitmapFilled;
        } else if (plane == 0x0F || plane == 0x10) { // Plane 15 & 16
            uint32_t value = (isInverted ? ~0 : 0);
            numBytes /= 4; // for 32bit

            while (numBytes-- > 0) {
                *((uint32_t*)bitmap) = value;
                bitmap += sizeof(uint32_t);
            }
            *(bitmap - 5) = (isInverted ? 0x3F : 0xC0);  // 0xFFFE & 0xFFFF
            return kCFUniCharBitmapFilled;
        }
        return (isInverted ? kCFUniCharBitmapEmpty : kCFUniCharBitmapAll);
    } else if ((charset < kCFUniCharDecimalDigitCharacterSet) || (charset == kCFUniCharNewlineCharacterSet)) {
        if (plane) {
            return (isInverted ? kCFUniCharBitmapAll : kCFUniCharBitmapEmpty);
        }

        uint8_t* bitmapBase = bitmap;
        CFIndex idx;
        uint8_t nonFillValue = (isInverted ? (uint8_t)0xFF : (uint8_t)0);

        while (numBytes-- > 0) {
            *(bitmap++) = nonFillValue;
        }

        if ((charset == kCFUniCharWhitespaceAndNewlineCharacterSet) || (charset == kCFUniCharNewlineCharacterSet)) {
            const UniChar newlines[] = {0x000A, 0x000B, 0x000C, 0x000D, 0x0085, 0x2028, 0x2029};

            for (idx = 0; idx < (int)(sizeof(newlines) / sizeof(*newlines)); idx++) {
                if (isInverted) {
                    _CFUniCharRemoveCharacterFromBitmap(newlines[idx], bitmapBase);
                } else {
                    _CFUniCharAddCharacterToBitmap(newlines[idx], bitmapBase);
                }
            }

            if (charset == kCFUniCharNewlineCharacterSet) {
                return kCFUniCharBitmapFilled;
            }
        }

        if (isInverted) {
            _CFUniCharRemoveCharacterFromBitmap(0x0009, bitmapBase);
            _CFUniCharRemoveCharacterFromBitmap(0x0020, bitmapBase);
            _CFUniCharRemoveCharacterFromBitmap(0x00A0, bitmapBase);
            _CFUniCharRemoveCharacterFromBitmap(0x1680, bitmapBase);
            _CFUniCharRemoveCharacterFromBitmap(0x202F, bitmapBase);
            _CFUniCharRemoveCharacterFromBitmap(0x205F, bitmapBase);
            _CFUniCharRemoveCharacterFromBitmap(0x3000, bitmapBase);
        } else {
            _CFUniCharAddCharacterToBitmap(0x0009, bitmapBase);
            _CFUniCharAddCharacterToBitmap(0x0020, bitmapBase);
            _CFUniCharAddCharacterToBitmap(0x00A0, bitmapBase);
            _CFUniCharAddCharacterToBitmap(0x1680, bitmapBase);
            _CFUniCharAddCharacterToBitmap(0x202F, bitmapBase);
            _CFUniCharAddCharacterToBitmap(0x205F, bitmapBase);
            _CFUniCharAddCharacterToBitmap(0x3000, bitmapBase);
        }

        for (idx = 0x2000; idx <= 0x200B; idx++) {
            if (isInverted) {
                _CFUniCharRemoveCharacterFromBitmap((UTF16Char)idx, bitmapBase);
            } else {
                _CFUniCharAddCharacterToBitmap((UTF16Char)idx, bitmapBase);
            }
        }
        return kCFUniCharBitmapFilled;
    }
    return (isInverted ? kCFUniCharBitmapAll : kCFUniCharBitmapEmpty);
}

CF_INTERNAL uint32_t _CFUniCharGetNumberOfPlanes(uint32_t charset) {
    if ((charset == kCFUniCharControlCharacterSet) || (charset == kCFUniCharControlAndFormatterCharacterSet)) {
        return 15; // 0 to 14
    } else if (charset < kCFUniCharDecimalDigitCharacterSet) {
        return 1;
    } else if (charset == kCFUniCharIllegalCharacterSet) {
        return 17;
    } else {
        uint32_t numPlanes;

        if (!__CFUniCharBitmapDataArray) {
            __CFUniCharLoadBitmapData();
        }

        numPlanes = __CFUniCharBitmapDataArray[__CFUniCharMapExternalSetToInternalIndex(__CFUniCharMapCompatibilitySetID(charset))]._numPlanes;

        return numPlanes;
    }
}

CF_INTERNAL const void* _CFUniCharGetMappingData(uint32_t type) {

    CFSpinLock(&__CFUniCharMappingTableLock);

    if (!__CFUniCharMappingTables) {
        const uint8_t* bytes;
        const uint8_t* bodyBase;
        int headerSize;
        int idx, count;

        if (!__CFUniCharLoadFile(CF_UNICHAR_MAPPING_FILE, &bytes)) {
            CFSpinUnlock(&__CFUniCharMappingTableLock);
            return NULL;
        }

        bytes += 4; // Skip Unicode version
        headerSize = *((uint32_t*)bytes);
        bytes += sizeof(uint32_t);
        headerSize -= (sizeof(uint32_t) * 2);
        bodyBase = bytes + headerSize;

        count = headerSize / sizeof(uint32_t);

        __CFUniCharMappingTables = (const void**)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(const void*) * count, 0);

        for (idx = 0; idx < count; idx++) {
            __CFUniCharMappingTables[idx] = bodyBase + *((uint32_t*)bytes);
            bytes += sizeof(uint32_t);
        }
    }

    CFSpinUnlock(&__CFUniCharMappingTableLock);

    return __CFUniCharMappingTables[type];
}

CF_INTERNAL uint32_t _CFUniCharGetConditionalCaseMappingFlags(UTF32Char theChar, UTF16Char* buffer, CFIndex currentIndex, CFIndex length, uint32_t type, const uint8_t* langCode, uint32_t lastFlags) {
    if (theChar == 0x03A3) { // GREEK CAPITAL LETTER SIGMA
        if ((type == kCFUniCharToLowercase) && (currentIndex > 0)) {
            UTF16Char* start = buffer;
            UTF16Char* end = buffer + length;
            UTF32Char otherChar;

            // First check if we're after a cased character
            buffer += (currentIndex - 1);
            while (start <= buffer) {
                otherChar = *(buffer--);
                if (_CFUniCharIsSurrogateLowCharacter(otherChar) && (start <= buffer) && _CFUniCharIsSurrogateHighCharacter(*buffer)) {
                    otherChar = _CFUniCharGetLongCharacterForSurrogatePair(*(buffer--), otherChar);
                }
                if (!_CFUniCharIsMemberOf(otherChar, kCFUniCharCaseIgnorableCharacterSet)) {
                    if (!_CFUniCharIsMemberOf(otherChar, kCFUniCharUppercaseLetterCharacterSet) && !_CFUniCharIsMemberOf(otherChar, kCFUniCharLowercaseLetterCharacterSet)) {
                        return 0;                                                                                                                                                   // Uppercase set contains titlecase
                    }
                    break;
                }
            }

            // Next check if we're before a cased character
            buffer = start + currentIndex + 1;
            while (buffer < end) {
                otherChar = *(buffer++);
                if (_CFUniCharIsSurrogateHighCharacter(otherChar) && (buffer < end) && _CFUniCharIsSurrogateLowCharacter(*buffer)) {
                    otherChar = _CFUniCharGetLongCharacterForSurrogatePair(otherChar, *(buffer++));
                }
                if (!_CFUniCharIsMemberOf(otherChar, kCFUniCharCaseIgnorableCharacterSet)) {
                    if (_CFUniCharIsMemberOf(otherChar, kCFUniCharUppercaseLetterCharacterSet) || _CFUniCharIsMemberOf(otherChar, kCFUniCharLowercaseLetterCharacterSet)) {
                        return 0;                                                                                                                                                 // Uppercase set contains titlecase
                    }
                    break;
                }
            }
            return kCFUniCharCaseMapFinalSigma;
        }
    } else if (langCode) {
        if (*((const uint16_t*)langCode) == LITHUANIAN_LANG_CODE) {
            if ((theChar == 0x0307) && ((kCFUniCharCaseMapAfter_i | kCFUniCharCaseMapMoreAbove) & lastFlags) == (kCFUniCharCaseMapAfter_i | kCFUniCharCaseMapMoreAbove)) {
                return (__CFUniCharIsAfter_i(buffer, currentIndex) ? kCFUniCharCaseMapAfter_i : 0);
            } else if (type == kCFUniCharToLowercase) {
                if ((theChar == 0x0049) || (theChar == 0x004A) || (theChar == 0x012E)) {
                    return (__CFUniCharIsMoreAbove(buffer + (++currentIndex), length - currentIndex) ? kCFUniCharCaseMapMoreAbove : 0);
                }
            } else if ((theChar == 'i') || (theChar == 'j')) {
                return (__CFUniCharIsMoreAbove(buffer + (++currentIndex), length - currentIndex) ? (kCFUniCharCaseMapAfter_i | kCFUniCharCaseMapMoreAbove) : 0);
            }
        } else if ((*((const uint16_t*)langCode) == TURKISH_LANG_CODE) || (*((const uint16_t*)langCode) == AZERI_LANG_CODE)) {
            if (type == kCFUniCharToLowercase) {
                if (theChar == 0x0307) {
                    return (kCFUniCharCaseMapMoreAbove & lastFlags ? kCFUniCharCaseMapAfter_i : 0);
                } else if (theChar == 0x0049) {
                    return (((++currentIndex < length) && (buffer[currentIndex] == 0x0307)) ? kCFUniCharCaseMapMoreAbove : 0);
                }
            }
        }
    }
    return 0;
}

CF_INTERNAL uint32_t _CFUniCharGetNumberOfPlanesForUnicodePropertyData(uint32_t propertyType) {
    (void)_CFUniCharGetUnicodePropertyDataForPlane(propertyType, 0);
    return __CFUniCharUnicodePropertyTable[propertyType]._numPlanes;
}

CF_INTERNAL uint32_t _CFUniCharGetUnicodeProperty(UTF32Char character, uint32_t propertyType) {
    if (propertyType == kCFUniCharCombiningProperty) {
        return _CFUniCharGetCombiningPropertyForCharacter(character, (const uint8_t*)_CFUniCharGetUnicodePropertyDataForPlane(propertyType, (character >> 16) & 0xFF));
    } else if (propertyType == kCFUniCharBidiProperty) {
        return _CFUniCharGetBidiPropertyForCharacter(character, (const uint8_t*)_CFUniCharGetUnicodePropertyDataForPlane(propertyType, (character >> 16) & 0xFF));
    } else {
        return 0;
    }
}

CF_INTERNAL const char* __CFUniCharGetUnicodeVersionString(void) {
    if (!__CFUniCharBitmapDataArray) {
        __CFUniCharLoadBitmapData();
    }
    return __CFUniCharUnicodeVersionString;
}

CF_INTERNAL bool _CFUniCharIsMemberOf(UTF32Char theChar, uint32_t charset) {
    charset = __CFUniCharMapCompatibilitySetID(charset);

    switch (charset) {
        case kCFUniCharWhitespaceCharacterSet:
            return isWhitespace(theChar, charset, NULL);

        case kCFUniCharWhitespaceAndNewlineCharacterSet:
            return isWhitespaceAndNewline(theChar, charset, NULL);

        case kCFUniCharNewlineCharacterSet:
            return isNewline(theChar, charset, NULL);

        default: {
            uint32_t tableIndex = __CFUniCharMapExternalSetToInternalIndex(charset);

            if (!__CFUniCharBitmapDataArray) {
                __CFUniCharLoadBitmapData();
            }

            if (tableIndex < __CFUniCharNumberOfBitmaps) {
                __CFUniCharBitmapData* data = __CFUniCharBitmapDataArray + tableIndex;
                uint8_t planeNo = (theChar >> 16) & 0xFF;

                // The bitmap data for kCFUniCharIllegalCharacterSet is actually LEGAL set less Plane 14 ~ 16
                if (charset == kCFUniCharIllegalCharacterSet) {
                    if (planeNo == 0x0E) {                           // Plane 14
                        theChar &= 0xFF;
                        return (((theChar == 0x01) || ((theChar > 0x1F) && (theChar < 0x80))) ? false : true);
                    } else if (planeNo == 0x0F || planeNo == 0x10) { // Plane 15 & 16
                        return ((theChar & 0xFF) > 0xFFFD ? true : false);
                    } else {
                        return (planeNo < data->_numPlanes && data->_planes[planeNo] ? !_CFUniCharIsMemberOfBitmap(theChar, data->_planes[planeNo]) : true);
                    }
                } else if (charset == kCFUniCharControlAndFormatterCharacterSet) {
                    if (planeNo == 0x0E) { // Plane 14
                        theChar &= 0xFF;
                        return (((theChar == 0x01) || ((theChar > 0x1F) && (theChar < 0x80))) ? true : false);
                    } else {
                        return (planeNo < data->_numPlanes && data->_planes[planeNo] ? _CFUniCharIsMemberOfBitmap(theChar, data->_planes[planeNo]) : false);
                    }
                } else {
                    return (planeNo < data->_numPlanes && data->_planes[planeNo] ? _CFUniCharIsMemberOfBitmap(theChar, data->_planes[planeNo]) : false);
                }
            }
            return false;
        }
    }
}

CF_INTERNAL const uint8_t* _CFUniCharGetBitmapPtrForPlane(uint32_t charset, uint32_t plane) {
    if (!__CFUniCharBitmapDataArray) {
        __CFUniCharLoadBitmapData();
    }

    charset = __CFUniCharMapCompatibilitySetID(charset);

    if ((charset > kCFUniCharWhitespaceAndNewlineCharacterSet) && (charset != kCFUniCharIllegalCharacterSet) && (charset != kCFUniCharNewlineCharacterSet)) {
        uint32_t tableIndex = __CFUniCharMapExternalSetToInternalIndex(charset);

        if (tableIndex < __CFUniCharNumberOfBitmaps) {
            __CFUniCharBitmapData* data = __CFUniCharBitmapDataArray + tableIndex;

            return (plane < data->_numPlanes ? data->_planes[plane] : NULL);
        }
    }
    return NULL;
}

CF_INTERNAL CFIndex _CFUniCharMapCaseTo(UTF32Char theChar, UTF16Char* convertedChar, CFIndex maxLength, uint32_t ctype, uint32_t flags, const uint8_t* langCode) {
    __CFUniCharBitmapData* data;
    uint8_t planeNo = (theChar >> 16) & 0xFF;

caseFoldRetry:

    if (flags & kCFUniCharCaseMapFinalSigma) {
        if (theChar == 0x03A3) { // Final sigma
            *convertedChar = (ctype == kCFUniCharToLowercase ? 0x03C2 : 0x03A3);
            return 1;
        }
    }

    if (langCode) {
        switch (*(uint16_t*)langCode) {
            case LITHUANIAN_LANG_CODE:
                if (theChar == 0x0307 && (flags & kCFUniCharCaseMapAfter_i)) {
                    return 0;
                } else if (ctype == kCFUniCharToLowercase) {
                    if (flags & kCFUniCharCaseMapMoreAbove) {
                        switch (theChar) {
                            case 0x0049: // LATIN CAPITAL LETTER I
                                *(convertedChar++) = 0x0069;
                                *(convertedChar++) = 0x0307;
                                return 2;

                            case 0x004A: // LATIN CAPITAL LETTER J
                                *(convertedChar++) = 0x006A;
                                *(convertedChar++) = 0x0307;
                                return 2;

                            case 0x012E: // LATIN CAPITAL LETTER I WITH OGONEK
                                *(convertedChar++) = 0x012F;
                                *(convertedChar++) = 0x0307;
                                return 2;

                            default: break;
                        }
                    }
                    switch (theChar) {
                        case 0x00CC: // LATIN CAPITAL LETTER I WITH GRAVE
                            *(convertedChar++) = 0x0069;
                            *(convertedChar++) = 0x0307;
                            *(convertedChar++) = 0x0300;
                            return 3;

                        case 0x00CD: // LATIN CAPITAL LETTER I WITH ACUTE
                            *(convertedChar++) = 0x0069;
                            *(convertedChar++) = 0x0307;
                            *(convertedChar++) = 0x0301;
                            return 3;

                        case 0x0128: // LATIN CAPITAL LETTER I WITH TILDE
                            *(convertedChar++) = 0x0069;
                            *(convertedChar++) = 0x0307;
                            *(convertedChar++) = 0x0303;
                            return 3;

                        default: break;
                    }
                }
                break;

            case TURKISH_LANG_CODE:
            case AZERI_LANG_CODE:
                if ((theChar == 0x0049) || (theChar == 0x0131)) {                     // LATIN CAPITAL LETTER I & LATIN SMALL LETTER DOTLESS I
                    *convertedChar = (((ctype == kCFUniCharToLowercase) || (ctype == kCFUniCharCaseFold))  ? ((kCFUniCharCaseMapMoreAbove & flags) ? 0x0069 : 0x0131) : 0x0049);
                    return 1;
                } else if ((theChar == 0x0069) || (theChar == 0x0130)) {              // LATIN SMALL LETTER I & LATIN CAPITAL LETTER I WITH DOT ABOVE
                    *convertedChar = (((ctype == kCFUniCharToLowercase) || (ctype == kCFUniCharCaseFold)) ? 0x0069 : 0x0130);
                    return 1;
                } else if (theChar == 0x0307 && (kCFUniCharCaseMapAfter_i & flags)) { // COMBINING DOT ABOVE AFTER_i
                    if (ctype == kCFUniCharToLowercase) {
                        return 0;
                    } else {
                        *convertedChar = 0x0307;
                        return 1;
                    }
                }
                break;

            default: break;
        }
    }

    if (!__CFUniCharBitmapDataArray) {
        __CFUniCharLoadBitmapData();
    }

    data = __CFUniCharBitmapDataArray + __CFUniCharMapExternalSetToInternalIndex(__CFUniCharMapCompatibilitySetID(ctype + kCFUniCharHasNonSelfLowercaseCharacterSet));

    if (planeNo < data->_numPlanes && data->_planes[planeNo] && _CFUniCharIsMemberOfBitmap(theChar, data->_planes[planeNo]) && (__CFUniCharCaseMappingTableCounts || __CFUniCharLoadCaseMappingTable())) {
        uint32_t value = __CFUniCharGetMappedCase((const __CFUniCharCaseMappings*)__CFUniCharCaseMappingTable[ctype], __CFUniCharCaseMappingTableCounts[ctype], theChar);

        if (!value && ctype == kCFUniCharToTitlecase) {
            value = __CFUniCharGetMappedCase((const __CFUniCharCaseMappings*)__CFUniCharCaseMappingTable[kCFUniCharToUppercase], __CFUniCharCaseMappingTableCounts[kCFUniCharToUppercase], theChar);
            if (value) {
                ctype = kCFUniCharToUppercase;
            }
        }

        if (value) {
            CFIndex count = _CFUniCharConvertFlagToCount(value);

            if (count == 1) {
                if (value & kCFUniCharNonBmpFlag) {
                    if (maxLength > 1) {
                        value = (value & 0xFFFFFF) - 0x10000;
                        *(convertedChar++) = (UTF16Char)(value >> 10) + 0xD800UL;
                        *(convertedChar++) = (UTF16Char)(value & 0x3FF) + 0xDC00UL;
                        return 2;
                    }
                } else {
                    *convertedChar = (UTF16Char)value;
                    return 1;
                }
            } else if (count < maxLength) {
                const uint32_t* extraMapping = __CFUniCharCaseMappingExtraTable[ctype] + (value & 0xFFFFFF);

                if (value & kCFUniCharNonBmpFlag) {
                    CFIndex copiedLen = 0;

                    while (count-- > 0) {
                        value = *(extraMapping++);
                        if (value > 0xFFFF) {
                            if (copiedLen + 2 >= maxLength) {
                                break;
                            }
                            value = (value & 0xFFFFFF) - 0x10000;
                            convertedChar[copiedLen++] = (UTF16Char)(value >> 10) + 0xD800UL;
                            convertedChar[copiedLen++] = (UTF16Char)(value & 0x3FF) + 0xDC00UL;
                        } else {
                            if (copiedLen + 1 >= maxLength) {
                                break;
                            }
                            convertedChar[copiedLen++] = value;
                        }
                    }
                    if (!count) {
                        return copiedLen;
                    }
                } else {
                    CFIndex idx;

                    for (idx = 0; idx < count; idx++) {
                        *(convertedChar++) = (UTF16Char) * (extraMapping++);
                    }
                    return count;
                }
            }
        }
    } else if (ctype == kCFUniCharCaseFold) {
        ctype = kCFUniCharToLowercase;
        goto caseFoldRetry;
    }

    if (theChar > 0xFFFF) { // non-BMP
        theChar = (theChar & 0xFFFFFF) - 0x10000;
        *(convertedChar++) = (UTF16Char)(theChar >> 10) + 0xD800UL;
        *(convertedChar++) = (UTF16Char)(theChar & 0x3FF) + 0xDC00UL;
        return 2;
    } else {
        *convertedChar = theChar;
        return 1;
    }
}

CF_INTERNAL const void* _CFUniCharGetUnicodePropertyDataForPlane(uint32_t propertyType, uint32_t plane) {

    CFSpinLock(&__CFUniCharPropTableLock);

    if (!__CFUniCharUnicodePropertyTable) {
        __CFUniCharBitmapData* table;
        const uint8_t* bytes;
        const uint8_t* bodyBase;
        const uint8_t* planeBase;
        int headerSize;
        int idx, count;
        int planeIndex, planeCount;
        int planeSize;

        if (!__CFUniCharLoadFile(CF_UNICHAR_PROPERTY_FILE, &bytes)) {
            CFSpinUnlock(&__CFUniCharPropTableLock);
            return NULL;
        }

        bytes += 4; // Skip Unicode version
        headerSize = CFSwapInt32BigToHost(*((uint32_t*)bytes));
        bytes += sizeof(uint32_t);

        headerSize -= (sizeof(uint32_t) * 2);
        bodyBase = bytes + headerSize;

        count = headerSize / sizeof(uint32_t);
        __CFUniCharUnicodePropertyTableCount = count;

        table = (__CFUniCharBitmapData*)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(__CFUniCharBitmapData) * count, 0);

        for (idx = 0; idx < count; idx++) {
            planeCount = *bodyBase;
            planeBase = bodyBase + planeCount + (planeCount % 4 ? 4 - (planeCount % 4) : 0);
            table[idx]._planes = (const uint8_t**)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(const void*) * planeCount, 0);

            for (planeIndex = 0; planeIndex < planeCount; planeIndex++) {
                if ((planeSize = bodyBase[planeIndex + 1])) {
                    table[idx]._planes[planeIndex] = planeBase;
                    planeBase += (planeSize * 256);
                } else {
                    table[idx]._planes[planeIndex] = NULL;
                }
            }

            table[idx]._numPlanes = planeCount;
            bodyBase += (CFSwapInt32BigToHost(*((uint32_t*)bytes++)));
        }

        __CFUniCharUnicodePropertyTable = table;
    }

    CFSpinUnlock(&__CFUniCharPropTableLock);

    return (plane < __CFUniCharUnicodePropertyTable[propertyType]._numPlanes ? __CFUniCharUnicodePropertyTable[propertyType]._planes[plane] : NULL);
}

CF_INTERNAL bool _CFUniCharFillDestinationBuffer(const UTF32Char* src, CFIndex srcLength, void** dst, CFIndex dstLength, CFIndex* filledLength, uint32_t dstFormat) {
    /*
     *  The UTF8 conversion in the following function is derived from ConvertUTF.c
     */
    /*
     * Copyright 2001 Unicode, Inc.
     *
     * Disclaimer
     *
     * This source code is provided as is by Unicode, Inc. No claims are
     * made as to fitness for any particular purpose. No warranties of any
     * kind are expressed or implied. The recipient agrees to determine
     * applicability of information provided. If this file has been
     * purchased on magnetic or optical media from Unicode, Inc., the
     * sole remedy for any claim will be exchange of defective media
     * within 90 days of receipt.
     *
     * Limitations on Rights to Redistribute This Code
     *
     * Unicode, Inc. hereby grants the right to freely use the information
     * supplied in this file in the creation of products supporting the
     * Unicode Standard, and to make copies of this file in any form
     * for internal or external distribution as long as this notice
     * remains attached.
     */
    #define UNI_REPLACEMENT_CHAR (0x0000FFFDUL)

    UTF32Char currentChar;
    CFIndex usedLength = *filledLength;

    if (dstFormat == kCFUniCharUTF16Format) {
        UTF16Char* dstBuffer = (UTF16Char*)*dst;

        while (srcLength-- > 0) {
            currentChar = *(src++);

            if (currentChar > 0xFFFF) { // Non-BMP
                usedLength += 2;
                if (dstLength) {
                    if (usedLength > dstLength) {
                        return false;
                    }
                    currentChar -= 0x10000;
                    *(dstBuffer++) = (UTF16Char)((currentChar >> 10) + 0xD800UL);
                    *(dstBuffer++) = (UTF16Char)((currentChar & 0x3FF) + 0xDC00UL);
                }
            } else {
                ++usedLength;
                if (dstLength) {
                    if (usedLength > dstLength) {
                        return false;
                    }
                    *(dstBuffer++) = (UTF16Char)currentChar;
                }
            }
        }

        *dst = dstBuffer;
    } else if (dstFormat == kCFUniCharUTF8Format) {
        uint8_t* dstBuffer = (uint8_t*)*dst;
        uint16_t bytesToWrite = 0;
        const UTF32Char byteMask = 0xBF;
        const UTF32Char byteMark = 0x80;
        static const uint8_t firstByteMark[7] = {0x00, 0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC};

        while (srcLength-- > 0) {
            currentChar = *(src++);

            /* Figure out how many bytes the result will require */
            if (currentChar < (UTF32Char)0x80) {
                bytesToWrite = 1;
            } else if (currentChar < (UTF32Char)0x800) {
                bytesToWrite = 2;
            } else if (currentChar < (UTF32Char)0x10000) {
                bytesToWrite = 3;
            } else if (currentChar < (UTF32Char)0x200000) {
                bytesToWrite = 4;
            } else {
                bytesToWrite = 2;
                currentChar = UNI_REPLACEMENT_CHAR;
            }

            usedLength += bytesToWrite;

            if (dstLength) {
                if (usedLength > dstLength) {
                    return false;
                }

                dstBuffer += bytesToWrite;
                switch (bytesToWrite) {    /* note: everything falls through. */
                    case 4:    *--dstBuffer = (currentChar | byteMark) & byteMask; currentChar >>= 6;
                    case 3:    *--dstBuffer = (currentChar | byteMark) & byteMask; currentChar >>= 6;
                    case 2:    *--dstBuffer = (currentChar | byteMark) & byteMask; currentChar >>= 6;
                    case 1:    *--dstBuffer =  currentChar | firstByteMark[bytesToWrite];
                }
                dstBuffer += bytesToWrite;
            }
        }

        *dst = dstBuffer;
    } else {
        UTF32Char* dstBuffer = (UTF32Char*)*dst;

        while (srcLength-- > 0) {
            currentChar = *(src++);

            ++usedLength;
            if (dstLength) {
                if (usedLength > dstLength) {
                    return false;
                }
                *(dstBuffer++) = currentChar;
            }
        }

        *dst = dstBuffer;
    }

    *filledLength = usedLength;

    return true;
}

// The second arg 'bitmap' has to be the pointer to a specific plane
CF_INTERNAL uint8_t _CFUniCharGetBidiPropertyForCharacter(UTF16Char character, const uint8_t* bitmap) {
    if (bitmap) {
        uint8_t value = bitmap[(character >> 8)];

        if (value > kCFUniCharBiDiPropertyPDF) {
            bitmap = bitmap + 256 + ((value - kCFUniCharBiDiPropertyPDF - 1) * 256);
            return bitmap[character % 256];
        } else {
            return value;
        }
    }
    return kCFUniCharBiDiPropertyL;
}

CF_INTERNAL uint8_t _CFUniCharGetCombiningPropertyForCharacter(UTF16Char character, const uint8_t* bitmap) {
    if (bitmap) {
        uint8_t value = bitmap[(character >> 8)];

        if (value) {
            bitmap = bitmap + 256 + ((value - 1) * 256);
            return bitmap[character % 256];
        }
    }
    return 0;
}

CF_INTERNAL bool _CFUniCharToUTF32(const UTF16Char* src, CFIndex length, UTF32Char* dst, bool allowLossy, bool isBigEndien) {
    const UTF16Char* limit = src + length;
    UTF32Char character;

    while (src < limit) {
        character = *(src++);

        if (_CFUniCharIsSurrogateHighCharacter(character)) {
            if ((src < limit) && _CFUniCharIsSurrogateLowCharacter(*src)) {
                character = _CFUniCharGetLongCharacterForSurrogatePair(character, *(src++));
            } else {
                if (!allowLossy) {
                    return false;
                }
                character = 0xFFFD; // replacement character
            }
        } else if (_CFUniCharIsSurrogateLowCharacter(character)) {
            if (!allowLossy) {
                return false;
            }
            character = 0xFFFD; // replacement character
        }

        *(dst++) = (isBigEndien ? CFSwapInt32HostToBig(character) : CFSwapInt32HostToLittle(character));
    }

    return true;
}

CF_INTERNAL bool _CFUniCharFromUTF32(const UTF32Char* src, CFIndex length, UTF16Char* dst, bool allowLossy, bool isBigEndien) {
    const UTF32Char* limit = src + length;
    UTF32Char character;

    while (src < limit) {
        character = (isBigEndien ? CFSwapInt32BigToHost(*(src++)) : CFSwapInt32LittleToHost(*(src++)));

        if (character < 0xFFFF) {                      // BMP
            if (allowLossy) {
                if (_CFUniCharIsSurrogateHighCharacter(character)) {
                    UTF32Char otherCharacter = 0xFFFD; // replacement character

                    if (src < limit) {
                        otherCharacter = (isBigEndien ? CFSwapInt32BigToHost(*src) : CFSwapInt32LittleToHost(*src));

                        if ((otherCharacter < 0x10000) && _CFUniCharIsSurrogateLowCharacter(otherCharacter)) {
                            *(dst++) = character; ++src;
                        } else {
                            otherCharacter = 0xFFFD; // replacement character
                        }
                    }

                    character = otherCharacter;
                } else if (_CFUniCharIsSurrogateLowCharacter(character)) {
                    character = 0xFFFD; // replacement character
                }
            } else {
                if (_CFUniCharIsSurrogateHighCharacter(character) || _CFUniCharIsSurrogateLowCharacter(character)) {
                    return false;
                }
            }
        } else if (character < 0x110000) { // non-BMP
            character -= 0x10000;
            *(dst++) = (UTF16Char)((character >> 10) + 0xD800UL);
            character = (UTF16Char)((character & 0x3FF) + 0xDC00UL);
        } else {
            if (!allowLossy) {
                return false;
            }
            character = 0xFFFD; // replacement character
        }

        *(dst++) = character;
    }
    return true;
}
