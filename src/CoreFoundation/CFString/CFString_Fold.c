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

#define ZERO_WIDTH_JOINER (0x200D)
#define COMBINING_GRAPHEME_JOINER (0x034F)

// Hangul ranges
#define HANGUL_CHOSEONG_START (0x1100)
#define HANGUL_CHOSEONG_END (0x115F)
#define HANGUL_JUNGSEONG_START (0x1160)
#define HANGUL_JUNGSEONG_END (0x11A2)
#define HANGUL_JONGSEONG_START (0x11A8)
#define HANGUL_JONGSEONG_END (0x11F9)
#define HANGUL_JONGSEONG_COUNT (28)

#define HANGUL_SYLLABLE_START (0xAC00)
#define HANGUL_SYLLABLE_END (0xD7AF)

#define kCFStringStackBufferLength (64)

#define MAX_TRANSCODING_LENGTH 4

static uint8_t __CFTranscodingHintLength[] = {
    2, 3, 4, 4, 4, 4, 4, 2, 2, 2, 2, 4, 0, 0, 0, 0
};

enum {
    kCFStringHangulStateL,
    kCFStringHangulStateV,
    kCFStringHangulStateT,
    kCFStringHangulStateLV,
    kCFStringHangulStateLVT,
    kCFStringHangulStateBreak
};

///////////////////////////////////////////////////////////////////// private

static
CFComparisonResult _CFCompareStringsWithLocale(CFStringInlineBuffer *str1, CFRange str1Range,
                                               CFStringInlineBuffer *str2, CFRange str2Range,
                                               CFOptionFlags options,
                                               const void *compareLocale)
{
    // TODO
    return kCFCompareEqualTo;
}

CF_INLINE bool _CFStringIsHangulLVT(UTF32Char character) {
    return ((character - HANGUL_SYLLABLE_START) % HANGUL_JONGSEONG_COUNT) != 0;
}

CF_INLINE bool _CFStringIsVirama(UTF32Char character, const uint8_t* combClassBMP) {
    return ((character == COMBINING_GRAPHEME_JOINER) ||
        (_CFUniCharGetCombiningPropertyForCharacter(
            character, 
            (const uint8_t*)((character < 0x10000) ?
                combClassBMP :
                _CFUniCharGetUnicodePropertyDataForPlane(
                    kCFUniCharCombiningProperty,
                    (character >> 16)))) == 9) ? true : false);
}

/* Returns the length of characters filled into outCharacters. 
 * If no change, returns 0. maxBufLen shoule be at least 8.
 */
static CFIndex __CFStringFoldCharacterClusterAtIndex(UTF32Char character,
                                                     CFStringInlineBuffer* buffer,
                                                     CFIndex index,
                                                     CFOptionFlags flags,
                                                     const uint8_t* langCode,
                                                     UTF32Char* outCharacters,
                                                     CFIndex maxBufferLength,
                                                     CFIndex* consumedLength)
{
    CFIndex filledLength = 0, currentIndex = index;

    if (character) {
        UTF16Char lowSurrogate;
        CFIndex planeNo = (character >> 16);
        bool isTurkikCapitalI = false;
        static const uint8_t* decompBMP = NULL;
        static const uint8_t* graphemeBMP = NULL;

        if (!decompBMP) {
            decompBMP = _CFUniCharGetBitmapPtrForPlane(kCFUniCharCanonicalDecomposableCharacterSet, 0);
            graphemeBMP = _CFUniCharGetBitmapPtrForPlane(kCFUniCharGraphemeExtendCharacterSet, 0);
        }

        ++currentIndex;

        if ((character < 0x0080) && (!langCode || (character != 'I'))) { // ASCII
            if ((flags & kCFCompareCaseInsensitive) &&
                (character >= 'A') && (character <= 'Z'))
            {
                character += ('a' - 'A');
                *outCharacters = character;
                filledLength = 1;
            }
        } else {
            // do width-insensitive mapping
            if ((flags & kCFCompareWidthInsensitive) &&
                (character >= 0xFF00) && (character <= 0xFFEF))
            {
                _CFUniCharCompatibilityDecompose(&character, 1, 1);
                *outCharacters = character;
                filledLength = 1;
            }

            // map surrogates
            if (!planeNo && _CFUniCharIsSurrogateHighCharacter(character) &&
                _CFUniCharIsSurrogateLowCharacter(
                    (lowSurrogate = CFStringGetCharacterFromInlineBuffer(buffer, currentIndex))))
            {
                character = _CFUniCharGetLongCharacterForSurrogatePair(character, lowSurrogate);
                ++currentIndex;
                planeNo = (character >> 16);
            }

            // decompose
            if (flags & (kCFCompareDiacriticsInsensitiveCompatibilityMask | kCFCompareNonliteral)) {
                if (_CFUniCharIsMemberOfBitmap(
                        character,
                        (!planeNo ?
                            decompBMP :
                            _CFUniCharGetBitmapPtrForPlane(kCFUniCharCanonicalDecomposableCharacterSet, planeNo))))
                {
                    UTF32Char original = character;

                    filledLength = _CFUniCharDecomposeCharacter(character, outCharacters, maxBufferLength);
                    character = *outCharacters;

                    if ((flags & kCFCompareDiacriticsInsensitiveCompatibilityMask) && (character < 0x0510)) {
                        filledLength = 1; // reset if Roman, Greek, Cyrillic
                    } else if (!(flags & kCFCompareNonliteral)) {
                        character = original;
                        filledLength = 0;
                    }
                }
            }

            // fold case
            if (flags & kCFCompareCaseInsensitive) {
                const uint8_t* nonBaseBitmap;

                bool filterNonBase = ((flags & kCFCompareDiacriticsInsensitiveCompatibilityMask) && (character < 0x0510));
                
                static const uint8_t* lowerBMP = NULL;
                static const uint8_t* caseFoldBMP = NULL;

                if (!lowerBMP) {
                    lowerBMP = _CFUniCharGetBitmapPtrForPlane(kCFUniCharHasNonSelfLowercaseCharacterSet, 0);
                    caseFoldBMP = _CFUniCharGetBitmapPtrForPlane(kCFUniCharHasNonSelfCaseFoldingCharacterSet, 0);
                }

                if (langCode && ('I' == character) && (
                        !strcmp((const char*)langCode, "tr") ||
                        !strcmp((const char*)langCode, "az")))
                {
                    // do Turkik special-casing
                    if (filledLength > 1) {
                        if (0x0307 == outCharacters[1]) {
                            if (--filledLength > 1) {
                                memmove(
                                    (outCharacters + 1),
                                    (outCharacters + 2),
                                    sizeof(UTF32Char) * (filledLength - 1));
                            }
                            character = (*outCharacters = 'i');
                            isTurkikCapitalI = true;
                        }
                    } else if (0x0307 == CFStringGetCharacterFromInlineBuffer(buffer, currentIndex)) {
                        character = (*outCharacters = 'i');
                        filledLength = 1;
                        ++currentIndex;
                        isTurkikCapitalI = true;
                    }
                }
                if (!isTurkikCapitalI && (
                        _CFUniCharIsMemberOfBitmap(character, (!planeNo ? lowerBMP : _CFUniCharGetBitmapPtrForPlane(kCFUniCharHasNonSelfLowercaseCharacterSet, planeNo))) ||
                        _CFUniCharIsMemberOfBitmap(character, (!planeNo ? caseFoldBMP : _CFUniCharGetBitmapPtrForPlane(kCFUniCharHasNonSelfCaseFoldingCharacterSet, planeNo)))))
                {
                    UTF16Char caseFoldBuffer[MAX_CASE_MAPPING_BUF];
                    const UTF16Char* bufferP = caseFoldBuffer, * bufferLimit;
                    UTF32Char* outCharactersP = outCharacters;
                    uint32_t bufferLength = _CFUniCharMapCaseTo(character, caseFoldBuffer, MAX_CASE_MAPPING_BUF, kCFUniCharCaseFold, 0, langCode);

                    bufferLimit = bufferP + bufferLength;

                    if (filledLength > 0) {
                        --filledLength; // decrement filledLength (will add back later)

                    }
                    // make space for casefold characters
                    if ((filledLength > 0) && (bufferLength > 1)) {
                        CFIndex totalScalerLength = 0;

                        while (bufferP < bufferLimit) {
                            if (_CFUniCharIsSurrogateHighCharacter(*(bufferP++)) && (bufferP < bufferLimit) && _CFUniCharIsSurrogateLowCharacter(*bufferP)) {
                                ++bufferP;
                            }
                            ++totalScalerLength;
                        }
                        memmove(outCharacters + totalScalerLength, outCharacters + 1, filledLength * sizeof(UTF32Char));
                        bufferP = caseFoldBuffer;
                    }

                    // fill
                    while (bufferP < bufferLimit) {
                        character = *(bufferP++);
                        if (_CFUniCharIsSurrogateHighCharacter(character) && (bufferP < bufferLimit) && _CFUniCharIsSurrogateLowCharacter(*bufferP)) {
                            character = _CFUniCharGetLongCharacterForSurrogatePair(character, *(bufferP++));
                            nonBaseBitmap = _CFUniCharGetBitmapPtrForPlane(kCFUniCharGraphemeExtendCharacterSet, (character >> 16));
                        } else {
                            nonBaseBitmap = graphemeBMP;
                        }

                        if (!filterNonBase || !_CFUniCharIsMemberOfBitmap(character, nonBaseBitmap)) {
                            *(outCharactersP++) = character;
                            ++filledLength;
                        }
                    }
                }
            }
        }

        // collect following combining marks
        if (flags & (kCFCompareDiacriticsInsensitiveCompatibilityMask | kCFCompareNonliteral)) {
            const uint8_t* nonBaseBitmap;
            const uint8_t* decompBitmap;
            bool doFill = ((flags & kCFCompareDiacriticsInsensitiveCompatibilityMask) && (character < 0x0510));

            if (!filledLength) {
                *outCharacters = character; // filledLength will be updated below on demand

                if (doFill) { // check if really needs to fill
                    UTF32Char nonBaseCharacter = CFStringGetCharacterFromInlineBuffer(buffer, currentIndex);

                    if (_CFUniCharIsSurrogateHighCharacter(nonBaseCharacter) &&
                        _CFUniCharIsSurrogateLowCharacter((lowSurrogate = CFStringGetCharacterFromInlineBuffer(buffer, currentIndex + 1))))
                    {
                        nonBaseCharacter = _CFUniCharGetLongCharacterForSurrogatePair(nonBaseCharacter, lowSurrogate);
                        nonBaseBitmap = _CFUniCharGetBitmapPtrForPlane(kCFUniCharGraphemeExtendCharacterSet, (nonBaseCharacter >> 16));
                        decompBitmap = _CFUniCharGetBitmapPtrForPlane(kCFUniCharCanonicalDecomposableCharacterSet, (nonBaseCharacter >> 16));
                    } else {
                        nonBaseBitmap = graphemeBMP;
                        decompBitmap = decompBMP;
                    }

                    if (_CFUniCharIsMemberOfBitmap(nonBaseCharacter, nonBaseBitmap)) {
                        filledLength = 1; // For the base character

                        if (!(flags & kCFCompareDiacriticsInsensitiveCompatibilityMask) ||
                            (nonBaseCharacter > 0x050F))
                        {
                            if (_CFUniCharIsMemberOfBitmap(nonBaseCharacter, decompBitmap)) {
                                filledLength += _CFUniCharDecomposeCharacter(
                                    nonBaseCharacter,
                                    &(outCharacters[filledLength]),
                                    maxBufferLength - filledLength);
                            } else {
                                outCharacters[filledLength++] = nonBaseCharacter;
                            }
                        }
                        currentIndex += ((nonBaseBitmap == graphemeBMP) ? 1 : 2);
                    } else {
                        doFill = false;
                    }
                }
            }

            while (filledLength < maxBufferLength) { // do the rest
                character = CFStringGetCharacterFromInlineBuffer(buffer, currentIndex);

                if (_CFUniCharIsSurrogateHighCharacter(character) &&
                    _CFUniCharIsSurrogateLowCharacter(
                        (lowSurrogate = CFStringGetCharacterFromInlineBuffer(buffer, currentIndex + 1))))
                {
                    character = _CFUniCharGetLongCharacterForSurrogatePair(character, lowSurrogate);
                    nonBaseBitmap = _CFUniCharGetBitmapPtrForPlane(kCFUniCharGraphemeExtendCharacterSet, (character >> 16));
                    decompBitmap = _CFUniCharGetBitmapPtrForPlane(kCFUniCharCanonicalDecomposableCharacterSet, (character >> 16));
                } else {
                    nonBaseBitmap = graphemeBMP;
                    decompBitmap = decompBMP;
                }
                if (isTurkikCapitalI) {
                    isTurkikCapitalI = false;
                } else if (_CFUniCharIsMemberOfBitmap(character, nonBaseBitmap)) {
                    if (doFill) {
                        if (_CFUniCharIsMemberOfBitmap(character, decompBitmap)) {
                            CFIndex currentLength = _CFUniCharDecomposeCharacter(
                                character, 
                                &(outCharacters[filledLength]),
                                maxBufferLength - filledLength);
                            if (!currentLength) {
                                break; // didn't fit
                            }
                            filledLength += currentLength;
                        } else {
                            outCharacters[filledLength++] = character;
                        }
                    } else if (!filledLength) {
                        filledLength = 1; // For the base character
                    }
                    currentIndex += ((nonBaseBitmap == graphemeBMP) ? 1 : 2);
                } else {
                    break;
                }
            }

            if (filledLength > 1) {
                UTF32Char* sortCharactersLimit = outCharacters + filledLength;
                UTF32Char* sortCharacters = sortCharactersLimit - 1;

                while ((outCharacters < sortCharacters) &&
                    _CFUniCharIsMemberOfBitmap(
                        *sortCharacters,
                        ((*sortCharacters < 0x10000) ? graphemeBMP : _CFUniCharGetBitmapPtrForPlane(kCFUniCharGraphemeExtendCharacterSet, (*sortCharacters >> 16)))))
                {
                    --sortCharacters;
                }

                if ((sortCharactersLimit - sortCharacters) > 1) {
                    _CFUniCharPrioritySort(sortCharacters, (sortCharactersLimit - sortCharacters));                                             // priority sort
                }
            }
        }
    }

    if ((filledLength > 0) && (consumedLength)) {
        *consumedLength = (currentIndex - index);
    }

    return filledLength;
}

static CFRange _CFStringInlineBufferGetComposedRange(CFStringInlineBuffer* buffer,
                                                     CFIndex start,
                                                     CFStringCharacterClusterType type,
                                                     const uint8_t* bmpBitmap,
                                                     CFIndex csetType)
{
    CFIndex end = start + 1;
    const uint8_t* bitmap = bmpBitmap;
    UTF32Char character;
    UTF16Char otherSurrogate;
    uint8_t step;

    character = CFStringGetCharacterFromInlineBuffer(buffer, start);

    // We don't combine characters in Armenian ~ Limbu range for backward deletion
    if ((type != kCFStringBackwardDeletionCluster) || (character < 0x0530) || (character > 0x194F)) {
        // Check if the current is surrogate
        if (_CFUniCharIsSurrogateHighCharacter(character) && _CFUniCharIsSurrogateLowCharacter((otherSurrogate = CFStringGetCharacterFromInlineBuffer(buffer, start + 1)))) {
            ++end;
            character = _CFUniCharGetLongCharacterForSurrogatePair(character, otherSurrogate);
            bitmap = _CFUniCharGetBitmapPtrForPlane(csetType, (character >> 16));
        }

        // Extend backward
        while (start > 0) {
            if ((type == kCFStringBackwardDeletionCluster) && (character >= 0x0530) && (character < 0x1950)) {
                break;
            }

            if (character < 0x10000) { // the first round could be already be non-BMP
                if (_CFUniCharIsSurrogateLowCharacter(character) &&
                    _CFUniCharIsSurrogateHighCharacter((otherSurrogate = CFStringGetCharacterFromInlineBuffer(buffer, start - 1)))) {
                    character = _CFUniCharGetLongCharacterForSurrogatePair(otherSurrogate, character);
                    bitmap = _CFUniCharGetBitmapPtrForPlane(csetType, (character >> 16));
                    --start;
                } else {
                    bitmap = bmpBitmap;
                }
            }

            if (!_CFUniCharIsMemberOfBitmap(character, bitmap) &&
                (character != 0xFF9E) &&
                (character != 0xFF9F) &&
                ((character & 0x1FFFF0) != 0xF870))
            {
                break;
            }

            --start;

            character = CFStringGetCharacterFromInlineBuffer(buffer, start);
        }
    }

    // Hangul
    if (((character >= HANGUL_CHOSEONG_START) && (character <= HANGUL_JONGSEONG_END)) ||
        ((character >= HANGUL_SYLLABLE_START) && (character <= HANGUL_SYLLABLE_END)))
    {
        uint8_t state;
        uint8_t initialState;

        if (character < HANGUL_JUNGSEONG_START) {
            state = kCFStringHangulStateL;
        } else if (character < HANGUL_JONGSEONG_START) {
            state = kCFStringHangulStateV;
        } else if (character < HANGUL_SYLLABLE_START) {
            state = kCFStringHangulStateT;
        } else {
            state = (_CFStringIsHangulLVT(character) ? kCFStringHangulStateLVT : kCFStringHangulStateLV);
        }
        initialState = state;

        // Extend backward
        while (
            ((character = CFStringGetCharacterFromInlineBuffer(buffer, start - 1)) >= HANGUL_CHOSEONG_START) &&
            (character <= HANGUL_SYLLABLE_END) &&
            ((character <= HANGUL_JONGSEONG_END) || (character >= HANGUL_SYLLABLE_START)))
        {
            switch (state) {
                case kCFStringHangulStateV:
                    if (character <= HANGUL_CHOSEONG_END) {
                        state = kCFStringHangulStateL;
                    } else if (
                        (character >= HANGUL_SYLLABLE_START) && (character <= HANGUL_SYLLABLE_END) &&
                        !_CFStringIsHangulLVT(character))
                    {
                        state = kCFStringHangulStateLV;
                    } else if (character > HANGUL_JUNGSEONG_END) {
                        state = kCFStringHangulStateBreak;
                    }
                    break;

                case kCFStringHangulStateT:
                    if ((character >= HANGUL_JUNGSEONG_START) && (character <= HANGUL_JUNGSEONG_END)) {
                        state = kCFStringHangulStateV;
                    } else if ((character >= HANGUL_SYLLABLE_START) && (character <= HANGUL_SYLLABLE_END)) {
                        state = (_CFStringIsHangulLVT(character) ? kCFStringHangulStateLVT : kCFStringHangulStateLV);
                    } else if (character < HANGUL_JUNGSEONG_START) {
                        state = kCFStringHangulStateBreak;
                    }
                    break;

                default:
                    state = ((character < HANGUL_JUNGSEONG_START) ? kCFStringHangulStateL : kCFStringHangulStateBreak);
                    break;
            }

            if (state == kCFStringHangulStateBreak) {
                break;
            }
            --start;
        }

        // Extend forward
        state = initialState;
        while (
            ((character = CFStringGetCharacterFromInlineBuffer(buffer, end)) > 0) && (
                ((character >= HANGUL_CHOSEONG_START) && (character <= HANGUL_JONGSEONG_END)) ||
                ((character >= HANGUL_SYLLABLE_START) && (character <= HANGUL_SYLLABLE_END))))
        {
            switch (state) {
                case kCFStringHangulStateLV:
                case kCFStringHangulStateV:
                    if ((character >= HANGUL_JUNGSEONG_START) && (character <= HANGUL_JONGSEONG_END)) {
                        state = ((character < HANGUL_JONGSEONG_START) ? kCFStringHangulStateV : kCFStringHangulStateT);
                    } else {
                        state = kCFStringHangulStateBreak;
                    }
                    break;

                case kCFStringHangulStateLVT:
                case kCFStringHangulStateT:
                    state = (((character >= HANGUL_JONGSEONG_START) && (character <= HANGUL_JONGSEONG_END)) ? kCFStringHangulStateT : kCFStringHangulStateBreak);
                    break;

                default:
                    if (character < HANGUL_JUNGSEONG_START) {
                        state = kCFStringHangulStateL;
                    } else if (character < HANGUL_JONGSEONG_START) {
                        state = kCFStringHangulStateV;
                    } else if (character >= HANGUL_SYLLABLE_START) {
                        state = (_CFStringIsHangulLVT(character) ? kCFStringHangulStateLVT : kCFStringHangulStateLV);
                    } else {
                        state = kCFStringHangulStateBreak;
                    }
                    break;
            }

            if (state == kCFStringHangulStateBreak) {
                break;
            }
            ++end;
        }
    }

    // Extend forward
    while ((character = CFStringGetCharacterFromInlineBuffer(buffer, end)) > 0) {
        if ((type == kCFStringBackwardDeletionCluster) && (character >= 0x0530) && (character < 0x1950)) {
            break;
        }

        if (_CFUniCharIsSurrogateHighCharacter(character) &&
            _CFUniCharIsSurrogateLowCharacter(
                (otherSurrogate = CFStringGetCharacterFromInlineBuffer(buffer, end + 1))))
        {
            character = _CFUniCharGetLongCharacterForSurrogatePair(character, otherSurrogate);
            bitmap = _CFUniCharGetBitmapPtrForPlane(csetType, (character >> 16));
            step = 2;
        } else {
            bitmap = bmpBitmap;
            step  = 1;
        }

        if (!_CFUniCharIsMemberOfBitmap(character, bitmap) &&
            (character != 0xFF9E) &&
            (character != 0xFF9F) &&
            ((character & 0x1FFFF0) != 0xF870))
        {
            break;
        }

        end += step;
    }

    return CFRangeMake(start, end - start);
}

///////////////////////////////////////////////////////////////////// public

CFComparisonResult CFStringCompareWithOptionsAndLocale(CFStringRef string,
                                                       CFStringRef string2, 
                                                       CFRange rangeToCompare, 
                                                       CFOptionFlags compareOptions, 
                                                       CFLocaleRef locale)
{
    CF_VALIDATE_RANGE_ARG(rangeToCompare, CFStringGetLength(string));
    CF_VALIDATE_STRING_ARG(string2);
    
    // No objc dispatch needed here since CFStringInlineBuffer 
    //  works with both CFString and NSString.

    // TODO refactor this bunch of variables into struct
    UTF32Char strBuf1[kCFStringStackBufferLength];
    CFStringInlineBuffer inlineBuf1;
    UTF32Char str1Char;
    CFIndex str1UsedLen;
    CFIndex str1Index = 0;
    CFIndex strBuf1Index = 0;
    CFIndex strBuf1Len = 0;
    
    UTF32Char strBuf2[kCFStringStackBufferLength];
    CFStringInlineBuffer inlineBuf2;
    UTF32Char str2Char;
    CFIndex str2UsedLen;
    CFIndex str2Index = 0;
    CFIndex strBuf2Index = 0;
    CFIndex strBuf2Len = 0;
    
    bool caseInsensitive = !!(compareOptions &
		kCFCompareCaseInsensitive);
    
    bool diacriticsInsensitive = !!(compareOptions &
		kCFCompareDiacriticsInsensitiveCompatibilityMask);
    
    bool equalityOptions = !!(compareOptions &
		(kCFCompareCaseInsensitive |
			kCFCompareNonliteral |
         	kCFCompareDiacriticsInsensitiveCompatibilityMask |
         	kCFCompareWidthInsensitive));
    
    bool numerically = !!(compareOptions &
		kCFCompareNumerically);

    CFIndex str2Len = CFStringGetLength(string2);
    
    locale = NULL;
    Boolean freeLocale = false;
    if ((compareOptions & kCFCompareLocalized) && !locale) {
        locale = CFLocaleCopyCurrent();
        freeLocale = true;
    }

    if (!locale && !numerically) {
        // Could do binary comp (be careful when adding new flags).

        CFStringEncoding eightBitEncoding = __CFStringGetEightBitStringEncoding();
        const uint8_t* str1Bytes = (const uint8_t*)CFStringGetCStringPtr(string, eightBitEncoding);
        const uint8_t* str2Bytes = (const uint8_t*)CFStringGetCStringPtr(string2, eightBitEncoding);
        CFIndex factor = sizeof(uint8_t);

        if (str1Bytes && str2Bytes) {
            compareOptions &= ~kCFCompareNonliteral; // remove non-literal

            if (kCFStringEncodingASCII == eightBitEncoding) {
                if (caseInsensitive) {
                    int cmpResult = strncasecmp(
                        (const char*)str1Bytes + rangeToCompare.location,
                        (const char*)str2Bytes,
                        _CFMin(rangeToCompare.length, str2Len));
                    if (!cmpResult) {
                        cmpResult = rangeToCompare.length - str2Len;
                    }
                    if (!cmpResult) {
                        return kCFCompareEqualTo;
                    } else {
                        return (cmpResult < 0) ? kCFCompareLessThan : kCFCompareGreaterThan;
                    }
                }
            } else if (caseInsensitive || diacriticsInsensitive) {
                CFIndex limitLength = _CFMin(rangeToCompare.length, str2Len);

                str1Bytes += rangeToCompare.location;

                while (str1Index < limitLength) {
                    str1Char = str1Bytes[str1Index];
                    str2Char = str2Bytes[str1Index];

                    if (str1Char != str2Char) {
                        if ((str1Char < 0x80) && (str2Char < 0x80)) {
                            if ((str1Char >= 'A') && (str1Char <= 'Z')) {
                                str1Char += ('a' - 'A');
                            }
                            if ((str2Char >= 'A') && (str2Char <= 'Z')) {
                                str2Char += ('a' - 'A');
                            }
                            if (str1Char != str2Char) {
                                return ((str1Char < str2Char) ? kCFCompareLessThan : kCFCompareGreaterThan);
                            }
                        } else {
                            str1Bytes = NULL;
                            break;
                        }
                    }
                    ++str1Index;
                }

                str2Index = str1Index;

                if (str1Index == limitLength) {
                    int cmpResult = rangeToCompare.length - str2Len;
                    return ((!cmpResult) ? kCFCompareEqualTo : ((cmpResult < 0) ? kCFCompareLessThan : kCFCompareGreaterThan));
                }
            }
        } else if (!equalityOptions && !str1Bytes && !str2Bytes) {
            str1Bytes = (const uint8_t*)CFStringGetCharactersPtr(string);
            str2Bytes = (const uint8_t*)CFStringGetCharactersPtr(string2);
            factor = sizeof(UTF16Char);
#if __LITTLE_ENDIAN__
            if (str1Bytes && str2Bytes) { // we cannot use memcmp
                const UTF16Char* str1 = ((const UTF16Char*)str1Bytes) + rangeToCompare.location;
                const UTF16Char* str1Limit = str1 + _CFMin(rangeToCompare.length, str2Len);
                const UTF16Char* str2 = (const UTF16Char*)str2Bytes;
                CFIndex cmpResult = 0;

                while ((!cmpResult) && (str1 < str1Limit)) {
                    cmpResult = (CFIndex) * (str1++) - (CFIndex) * (str2++);
                }
                if (!cmpResult) {
                    cmpResult = rangeToCompare.length - str2Len;
                }
                if (!cmpResult) {
                    return kCFCompareEqualTo;
                } else {
                    return (cmpResult < 0) ? kCFCompareLessThan : kCFCompareGreaterThan;
                }
            }
#endif /* __LITTLE_ENDIAN__ */
        }
        if (str1Bytes && str2Bytes) {
            int cmpResult = memcmp(
                str1Bytes + (rangeToCompare.location * factor),
                str2Bytes,
                _CFMin(rangeToCompare.length, str2Len) * factor);
            if (!cmpResult) {
                cmpResult = rangeToCompare.length - str2Len;
            }
            return ((!cmpResult) ? kCFCompareEqualTo : ((cmpResult < 0) ? kCFCompareLessThan : kCFCompareGreaterThan));
        }
    }

 
    const uint8_t* graphemeBMP = _CFUniCharGetBitmapPtrForPlane(
		kCFUniCharGraphemeExtendCharacterSet, 0);
    
    CFComparisonResult compareResult = kCFCompareEqualTo;
    UTF16Char otherChar;
    
    const uint8_t* langCode = locale ?
	    (const uint8_t*)_CFStrGetLanguageIdentifierForLocale(locale) :
    	NULL;
    
    CFStringInitInlineBuffer(string, &inlineBuf1, rangeToCompare);
    CFStringInitInlineBuffer(string2, &inlineBuf2, CFRangeMake(0, str2Len));

    while ((str1Index < rangeToCompare.length) && (str2Index < str2Len)) {
        if (strBuf1Len == 0) {
            str1Char = CFStringGetCharacterFromInlineBuffer(&inlineBuf1, str1Index);
            if (caseInsensitive && (str1Char >= 'A') && (str1Char <= 'Z') && (!langCode || (str1Char != 'I'))) {
                str1Char += ('a' - 'A');
            }
            str1UsedLen = 1;
        } else {
            str1Char = strBuf1[strBuf1Index++];
        }
        if (strBuf2Len == 0) {
            str2Char = CFStringGetCharacterFromInlineBuffer(&inlineBuf2, str2Index);
            if (caseInsensitive && (str2Char >= 'A') && (str2Char <= 'Z') && (!langCode || (str2Char != 'I'))) {
                str2Char += ('a' - 'A');
            }
            str2UsedLen = 1;
        } else {
            str2Char = strBuf2[strBuf2Index++];
        }

        if (numerically &&
            (!strBuf1Len && (str1Char <= '9') && (str1Char >= '0')) &&
            (!strBuf2Len && (str2Char <= '9') && (str2Char >= '0')))
        {
            // If both are not ASCII digits, then don't do numerical comparison here.
            uint64_t intValue1 = 0, intValue2 = 0; // !!! Doesn't work if numbers are > max uint64_t

            do {
                intValue1 = (intValue1 * 10) + (str1Char - '0');
                str1Char = CFStringGetCharacterFromInlineBuffer(&inlineBuf1, ++str1Index);
            } while ((str1Char <= '9') && (str1Char >= '0'));

            do {
                intValue2 = intValue2 * 10 + (str2Char - '0');
                str2Char = CFStringGetCharacterFromInlineBuffer(&inlineBuf2, ++str2Index);
            } while ((str2Char <= '9') && (str2Char >= '0'));

            if (intValue1 == intValue2) {
                continue;
            } else if (intValue1 < intValue2) {
                if (freeLocale && locale) {
                    CFRelease(locale);
                }
                return kCFCompareLessThan;
            } else {
                if (freeLocale && locale) {
                    CFRelease(locale);
                }
                return kCFCompareGreaterThan;
            }
        }

        if (str1Char != str2Char) {
            if (!equalityOptions) {
                CFComparisonResult res = (!locale ?
                    ((str1Char < str2Char) ? kCFCompareLessThan : kCFCompareGreaterThan) :
                    _CFCompareStringsWithLocale(
                        &inlineBuf1, CFRangeMake(strBuf1Index, rangeToCompare.length - strBuf1Index),
                        &inlineBuf2, CFRangeMake(strBuf2Index, str2Len - strBuf2Index),
                        compareOptions,
                        locale));
                if (freeLocale && locale) {
                    CFRelease(locale);
                }
                return res;
            }

            if ((compareOptions & kCFCompareForcedOrdering) && (kCFCompareEqualTo == compareResult)) {
                compareResult = ((str1Char < str2Char) ? kCFCompareLessThan : kCFCompareGreaterThan);
            }

            if ((str1Char < 0x80) && (str2Char < 0x80)) {
                if (locale) {
                    CFComparisonResult res = _CFCompareStringsWithLocale(
                        &inlineBuf1, CFRangeMake(strBuf1Index, rangeToCompare.length - strBuf1Index),
                        &inlineBuf2, CFRangeMake(strBuf2Index, str2Len - strBuf2Index),
                        compareOptions,
                        locale);
                    if (freeLocale && locale) {
                        CFRelease(locale);
                    }
                    return res;
                } else if (!caseInsensitive) {
                    if (freeLocale && locale) {
                        CFRelease(locale);
                    }
                    return ((str1Char < str2Char) ? kCFCompareLessThan : kCFCompareGreaterThan);
                }
            }

            if (_CFUniCharIsSurrogateHighCharacter(str1Char) &&
                _CFUniCharIsSurrogateLowCharacter(
                    (otherChar = CFStringGetCharacterFromInlineBuffer(&inlineBuf1, str1Index + 1))))
            {
                str1Char = _CFUniCharGetLongCharacterForSurrogatePair(str1Char, otherChar);
                str1UsedLen = 2;
            }

            if (_CFUniCharIsSurrogateHighCharacter(str2Char) &&
                _CFUniCharIsSurrogateLowCharacter(
                    (otherChar = CFStringGetCharacterFromInlineBuffer(&inlineBuf2, str2Index + 1))))
            {
                str2Char = _CFUniCharGetLongCharacterForSurrogatePair(str2Char, otherChar);
                str2UsedLen = 2;
            }

            if (diacriticsInsensitive && (str1Index > 0)) {
                bool str1Skip = false;
                bool str2Skip = false;

                if (!strBuf1Len &&
                    _CFUniCharIsMemberOfBitmap(
                        str1Char,
                        ((str1Char < 0x10000) ?
                            graphemeBMP :
                            _CFUniCharGetBitmapPtrForPlane(kCFUniCharGraphemeExtendCharacterSet, (str1Char >> 16)))))
                {
                    str1Char = str2Char;
                    str1Skip = true;
                }
                if (!strBuf2Len &&
                    _CFUniCharIsMemberOfBitmap(
                        str2Char,
                        ((str2Char < 0x10000) ?
                            graphemeBMP :
                            _CFUniCharGetBitmapPtrForPlane(kCFUniCharGraphemeExtendCharacterSet, (str2Char >> 16)))))
                {
                    str2Char = str1Char;
                    str2Skip = true;
                }

                if (str1Skip != str2Skip) {
                    if (str1Skip) {
                        str2Index -= str2UsedLen;
                    }
                    if (str2Skip) {
                        str1Index -= str1UsedLen;
                    }
                }
            }

            if (str1Char != str2Char) {
                if (!strBuf1Len) {
                    strBuf1Len = __CFStringFoldCharacterClusterAtIndex(
                        str1Char, &inlineBuf1, str1Index, 
                        compareOptions, 
                        langCode, 
                        strBuf1, kCFStringStackBufferLength, &str1UsedLen);
                    if (strBuf1Len > 0) {
                        str1Char = *strBuf1;
                        strBuf1Index = 1;
                    }
                }

                if (!strBuf1Len && (0 < strBuf2Len)) {
                    CFComparisonResult res =  (!locale ?
                        ((str1Char < str2Char) ? kCFCompareLessThan : kCFCompareGreaterThan) :
                        _CFCompareStringsWithLocale(
                            &inlineBuf1, CFRangeMake(strBuf1Index, rangeToCompare.length - strBuf1Index),
                            &inlineBuf2, CFRangeMake(strBuf2Index, str2Len - strBuf2Index),
                            compareOptions,
                            locale));
                    if (freeLocale && locale) {
                        CFRelease(locale);
                    }
                    return res;
                }

                if (!strBuf2Len && (!strBuf1Len || (str1Char != str2Char))) {
                    strBuf2Len = __CFStringFoldCharacterClusterAtIndex(
                        str2Char, &inlineBuf2, str2Index, 
                        compareOptions, 
                        langCode, 
                        strBuf2, kCFStringStackBufferLength, &str2UsedLen);
                    if (strBuf2Len > 0) {
                        str2Char = *strBuf2;
                        strBuf2Index = 1;
                    }
                    if ((!strBuf2Len) || (str1Char != str2Char)) {
                        CFComparisonResult res = (!locale ?
                            ((str1Char < str2Char) ? kCFCompareLessThan : kCFCompareGreaterThan) :
                            _CFCompareStringsWithLocale(
                                &inlineBuf1, CFRangeMake(strBuf1Index, rangeToCompare.length - strBuf1Index),
                                &inlineBuf2, CFRangeMake(strBuf2Index, str2Len - strBuf2Index),
                                compareOptions,
                                locale));
                        if (freeLocale && locale) {
                            CFRelease(locale);
                        }
                        return res;
                    }
                }
            }

            if ((strBuf1Len > 0) && (strBuf2Len > 0)) {
                while ((strBuf1Index < strBuf1Len) && (strBuf2Index < strBuf2Len)) {
                    if (strBuf1[strBuf1Index] != strBuf2[strBuf2Index]) {
                        break;
                    }
                    ++strBuf1Index; ++strBuf2Index;
                }
                if ((strBuf1Index < strBuf1Len) && (strBuf2Index < strBuf2Len)) {
                    CFComparisonResult res = (!locale ?
                        ((str1Char < str2Char) ? kCFCompareLessThan : kCFCompareGreaterThan) :
                        _CFCompareStringsWithLocale(
                            &inlineBuf1, CFRangeMake(strBuf1Index, rangeToCompare.length - strBuf1Index),
                            &inlineBuf2, CFRangeMake(strBuf2Index, str2Len - strBuf2Index),
                            compareOptions,
                            locale));
                    if (freeLocale && locale) {
                        CFRelease(locale);
                    }
                    return res;
                }
            }
        }

        if ((strBuf1Len > 0) && (strBuf1Index == strBuf1Len)) {
            strBuf1Len = 0;
        }
        if ((strBuf2Len > 0) && (strBuf2Index == strBuf2Len)) {
            strBuf2Len = 0;
        }

        if (strBuf1Len == 0) {
            str1Index += str1UsedLen;
        }
        if (strBuf2Len == 0) {
            str2Index += str2UsedLen;
        }
    }

    if (diacriticsInsensitive) {
        while (str1Index < rangeToCompare.length) {
            str1Char = CFStringGetCharacterFromInlineBuffer(&inlineBuf1, str1Index);
            if (str1Char < 0x80) {
                break; // found ASCII

            }
            if (_CFUniCharIsSurrogateHighCharacter(str1Char) &&
                _CFUniCharIsSurrogateLowCharacter(
                    (otherChar = CFStringGetCharacterFromInlineBuffer(&inlineBuf1, str1Index + 1))))
            {
                str1Char = _CFUniCharGetLongCharacterForSurrogatePair(str1Char, otherChar);
            }

            if (!_CFUniCharIsMemberOfBitmap(
                    str1Char,
                    ((str1Char < 0x10000) ?
                        graphemeBMP :
                        _CFUniCharGetBitmapPtrForPlane(kCFUniCharGraphemeExtendCharacterSet, (str1Char >> 16)))))
            {
                break;
            }

            str1Index += ((str1Char < 0x10000) ? 1 : 2);
        }

        while (str2Index < str2Len) {
            str2Char = CFStringGetCharacterFromInlineBuffer(&inlineBuf2, str2Index);
            if (str2Char < 0x80) {
                break; // found ASCII

            }
            if (_CFUniCharIsSurrogateHighCharacter(str2Char) &&
                _CFUniCharIsSurrogateLowCharacter(
                    (otherChar = CFStringGetCharacterFromInlineBuffer(&inlineBuf2, str2Index + 1))))
            {
                str2Char = _CFUniCharGetLongCharacterForSurrogatePair(str2Char, otherChar);
            }

            if (!_CFUniCharIsMemberOfBitmap(
                str2Char,
                ((str2Char < 0x10000) ?
                    graphemeBMP :
                    _CFUniCharGetBitmapPtrForPlane(kCFUniCharGraphemeExtendCharacterSet, (str2Char >> 16)))))
            {
                break;
            }

            str2Index += ((str2Char < 0x10000) ? 1 : 2);
        }
    }

    if (freeLocale && locale) {
        CFRelease(locale);
    }

    return ((str1Index < rangeToCompare.length) ?
        kCFCompareGreaterThan :
        ((str2Index < str2Len) ? kCFCompareLessThan : compareResult));
}

CFComparisonResult CFStringCompareWithOptions(CFStringRef string, CFStringRef string2, CFRange rangeToCompare, CFOptionFlags compareOptions) {
    return CFStringCompareWithOptionsAndLocale(string, string2, rangeToCompare, compareOptions, NULL);
}

CFComparisonResult CFStringCompare(CFStringRef string, CFStringRef str2, CFOptionFlags options) {
    return CFStringCompareWithOptions(string, str2, CFRangeMake(0, CFStringGetLength(string)), options);
}

Boolean CFStringFindWithOptionsAndLocale(CFStringRef string,
                                         CFStringRef stringToFind,
                                         CFRange rangeToSearch,
                                         CFOptionFlags compareOptions,
                                         CFLocaleRef locale,
                                         CFRange* result)
{
    /* No objc dispatch needed here since CFStringInlineBuffer works with both CFString and NSString */
    CFIndex findStrLen = CFStringGetLength(stringToFind);
    Boolean didFind = false;
    bool lengthVariants = (0 != (compareOptions & (
        kCFCompareCaseInsensitive |
        kCFCompareNonliteral |
        kCFCompareDiacriticsInsensitiveCompatibilityMask)));

    if ((findStrLen > 0) && (rangeToSearch.length > 0) &&
        ((findStrLen <= rangeToSearch.length) || lengthVariants))
    {
        UTF32Char strBuf1[kCFStringStackBufferLength];
        UTF32Char strBuf2[kCFStringStackBufferLength];
        CFStringInlineBuffer inlineBuf1, inlineBuf2;
        UTF32Char str1Char, str2Char;
        CFStringEncoding eightBitEncoding = __CFStringGetEightBitStringEncoding();
        const uint8_t* str1Bytes = (const uint8_t*)CFStringGetCStringPtr(string, eightBitEncoding);
        const uint8_t* str2Bytes = (const uint8_t*)CFStringGetCStringPtr(stringToFind, eightBitEncoding);
        const UTF32Char* characters, * charactersLimit;
        const uint8_t* langCode = NULL;
        CFIndex fromLoc, toLoc;
        CFIndex str1Index, str2Index;
        CFIndex strBuf1Len, strBuf2Len;
        bool equalityOptions = ((lengthVariants || (compareOptions & kCFCompareWidthInsensitive)) ? true : false);
        bool caseInsensitive = ((compareOptions & kCFCompareCaseInsensitive) ? true : false);
        int8_t delta;

        if (!locale) {
            if (compareOptions & kCFCompareLocalized) {
                CFLocaleRef currentLocale = CFLocaleCopyCurrent();
                langCode = (const uint8_t*)_CFStrGetLanguageIdentifierForLocale(currentLocale);
                CFRelease(currentLocale);
            }
        } else {
            langCode = (const uint8_t*)_CFStrGetLanguageIdentifierForLocale(locale);
        }

        CFStringInitInlineBuffer(string, &inlineBuf1, CFRangeMake(0, rangeToSearch.location + rangeToSearch.length));
        CFStringInitInlineBuffer(stringToFind, &inlineBuf2, CFRangeMake(0, findStrLen));

        if (compareOptions & kCFCompareBackwards) {
            fromLoc = rangeToSearch.location + rangeToSearch.length - (lengthVariants ? 1 : findStrLen);
            toLoc = (((compareOptions & kCFCompareAnchored) && !lengthVariants) ? fromLoc : rangeToSearch.location);
        } else {
            fromLoc = rangeToSearch.location;
            toLoc = ((compareOptions & kCFCompareAnchored) ? fromLoc : rangeToSearch.location + rangeToSearch.length - (lengthVariants ? 1 : findStrLen));
        }

        delta = ((fromLoc <= toLoc) ? 1 : -1);

        if (str1Bytes && str2Bytes) {
            CFIndex maxStr1Index = (rangeToSearch.location + rangeToSearch.length);
            uint8_t str1Byte, str2Byte;

            while (1) {
                str1Index = fromLoc;
                str2Index = 0;

                while ((str1Index < maxStr1Index) && (str2Index < findStrLen)) {
                    str1Byte = str1Bytes[str1Index];
                    str2Byte = str2Bytes[str2Index];

                    if (str1Byte != str2Byte) {
                        if (equalityOptions) {
                            if ((str1Byte < 0x80) && ((!langCode) || ('I' != str1Byte))) {
                                if (caseInsensitive && (str1Byte >= 'A') && (str1Byte <= 'Z')) {
                                    str1Byte += ('a' - 'A');
                                }
                                *strBuf1 = str1Byte;
                                strBuf1Len = 1;
                            } else {
                                str1Char = CFStringGetCharacterFromInlineBuffer(&inlineBuf1, str1Index);
                                strBuf1Len = __CFStringFoldCharacterClusterAtIndex(str1Char, &inlineBuf1, str1Index, compareOptions, langCode, strBuf1, kCFStringStackBufferLength, NULL);
                                if (1 > strBuf1Len) {
                                    *strBuf1 = str1Char;
                                    strBuf1Len = 1;
                                }
                            }
                            if ((str2Byte < 0x80) && ((!langCode) || ('I' != str2Byte))) {
                                if (caseInsensitive && (str2Byte >= 'A') && (str2Byte <= 'Z')) {
                                    str2Byte += ('a' - 'A');
                                }
                                *strBuf2 = str2Byte;
                                strBuf2Len = 1;
                            } else {
                                str2Char = CFStringGetCharacterFromInlineBuffer(&inlineBuf2, str2Index);
                                strBuf2Len = __CFStringFoldCharacterClusterAtIndex(str2Char, &inlineBuf2, str2Index, compareOptions, langCode, strBuf2, kCFStringStackBufferLength, NULL);
                                if (1 > strBuf2Len) {
                                    *strBuf2 = str2Char;
                                    strBuf2Len = 1;
                                }
                            }

                            if ((1 == strBuf1Len) && (1 == strBuf2Len)) { // normal case
                                if (*strBuf1 != *strBuf2) {
                                    break;
                                }
                            } else {
                                CFIndex delta;

                                if (!caseInsensitive && (strBuf1Len != strBuf2Len)) {
                                    break;
                                }
                                if (memcmp(strBuf1, strBuf2, sizeof(UTF32Char) * _CFMin(strBuf1Len, strBuf2Len))) {
                                    break;
                                }

                                if (strBuf1Len < strBuf2Len) {
                                    delta = strBuf2Len - strBuf1Len;

                                    if ((str1Index + strBuf1Len + delta) > (rangeToSearch.location + rangeToSearch.length)) {
                                        break;
                                    }

                                    characters = &(strBuf2[strBuf1Len]);
                                    charactersLimit = characters + delta;

                                    while (characters < charactersLimit) {
                                        strBuf1Len = __CFStringFoldCharacterClusterAtIndex(CFStringGetCharacterFromInlineBuffer(&inlineBuf1, str1Index + 1), &inlineBuf1, str1Index + 1, compareOptions, langCode, strBuf1, kCFStringStackBufferLength, NULL);
                                        if ((strBuf1Len > 0) || (*characters != *strBuf1)) {
                                            break;
                                        }
                                        ++characters; ++str1Index;
                                    }
                                    if (characters < charactersLimit) {
                                        break;
                                    }
                                } else if (strBuf2Len < strBuf1Len) {
                                    delta = strBuf1Len - strBuf2Len;

                                    if ((str2Index + strBuf2Len + delta) > findStrLen) {
                                        break;
                                    }

                                    characters = &(strBuf1[strBuf2Len]);
                                    charactersLimit = characters + delta;

                                    while (characters < charactersLimit) {
                                        strBuf2Len = __CFStringFoldCharacterClusterAtIndex(CFStringGetCharacterFromInlineBuffer(&inlineBuf2, str1Index + 1), &inlineBuf2, str2Index + 1, compareOptions, langCode, strBuf2, kCFStringStackBufferLength, NULL);
                                        if ((strBuf2Len > 0) || (*characters != *strBuf2)) {
                                            break;
                                        }
                                        ++characters; ++str2Index;
                                    }
                                    if (characters < charactersLimit) {
                                        break;
                                    }
                                }
                            }
                        } else {
                            break;
                        }
                    }
                    ++str1Index; ++str2Index;
                }

                if (str2Index == findStrLen) {
                    if (((kCFCompareBackwards | kCFCompareAnchored) != (compareOptions & (kCFCompareBackwards | kCFCompareAnchored))) || (str1Index == (rangeToSearch.location + rangeToSearch.length))) {
                        didFind = true;
                        if (result) {
                            *result = CFRangeMake(fromLoc, str1Index - fromLoc);
                        }
                    }
                    break;
                }

                if (fromLoc == toLoc) {
                    break;
                }
                fromLoc += delta;
            }
        } else if (equalityOptions) {
            UTF16Char otherChar;
            CFIndex str1UsedLen, str2UsedLen, strBuf1Index = 0, strBuf2Index = 0;
            bool diacriticsInsensitive = ((compareOptions & kCFCompareDiacriticsInsensitiveCompatibilityMask) ? true : false);
            const uint8_t* graphemeBMP = _CFUniCharGetBitmapPtrForPlane(kCFUniCharGraphemeExtendCharacterSet, 0);
            const uint8_t* combClassBMP = (const uint8_t*)_CFUniCharGetUnicodePropertyDataForPlane(kCFUniCharCombiningProperty, 0);

            while (1) {
                str1Index = fromLoc;
                str2Index = 0;

                strBuf1Len = strBuf2Len = 0;

                while (str2Index < findStrLen) {
                    if (strBuf1Len == 0) {
                        str1Char = CFStringGetCharacterFromInlineBuffer(&inlineBuf1, str1Index);
                        if (caseInsensitive && (str1Char >= 'A') && (str1Char <= 'Z') && ((!langCode) || (str1Char != 'I'))) {
                            str1Char += ('a' - 'A');
                        }
                        str1UsedLen = 1;
                    } else {
                        str1Char = strBuf1[strBuf1Index++];
                    }
                    if (strBuf2Len == 0) {
                        str2Char = CFStringGetCharacterFromInlineBuffer(&inlineBuf2, str2Index);
                        if (caseInsensitive && (str2Char >= 'A') && (str2Char <= 'Z') && ((!langCode) || (str2Char != 'I'))) {
                            str2Char += ('a' - 'A');
                        }
                        str2UsedLen = 1;
                    } else {
                        str2Char = strBuf2[strBuf2Index++];
                    }

                    if (str1Char != str2Char) {
                        if ((str1Char < 0x80) && (str2Char < 0x80) && ((!langCode) || !caseInsensitive)) {
                            break;
                        }

                        if (_CFUniCharIsSurrogateHighCharacter(str1Char) && _CFUniCharIsSurrogateLowCharacter((otherChar = CFStringGetCharacterFromInlineBuffer(&inlineBuf1, str1Index + 1)))) {
                            str1Char = _CFUniCharGetLongCharacterForSurrogatePair(str1Char, otherChar);
                            str1UsedLen = 2;
                        }

                        if (_CFUniCharIsSurrogateHighCharacter(str2Char) && _CFUniCharIsSurrogateLowCharacter((otherChar = CFStringGetCharacterFromInlineBuffer(&inlineBuf2, str2Index + 1)))) {
                            str2Char = _CFUniCharGetLongCharacterForSurrogatePair(str2Char, otherChar);
                            str2UsedLen = 2;
                        }

                        if (diacriticsInsensitive && (str1Index > fromLoc)) {
                            bool str1Skip = false;
                            bool str2Skip = false;

                            if ((!strBuf1Len) && _CFUniCharIsMemberOfBitmap(str1Char, ((str1Char < 0x10000) ? graphemeBMP : _CFUniCharGetBitmapPtrForPlane(kCFUniCharGraphemeExtendCharacterSet, (str1Char >> 16))))) {
                                str1Char = str2Char;
                                str1Skip = true;
                            }
                            if ((!strBuf2Len) && _CFUniCharIsMemberOfBitmap(str2Char, ((str2Char < 0x10000) ? graphemeBMP : _CFUniCharGetBitmapPtrForPlane(kCFUniCharGraphemeExtendCharacterSet, (str2Char >> 16))))) {
                                str2Char = str1Char;
                                str2Skip = true;
                            }

                            if (str1Skip != str2Skip) {
                                if (str1Skip) {
                                    str2Index -= str2UsedLen;
                                }
                                if (str2Skip) {
                                    str1Index -= str1UsedLen;
                                }
                            }
                        }

                        if (str1Char != str2Char) {
                            if (!strBuf1Len) {
                                strBuf1Len = __CFStringFoldCharacterClusterAtIndex(str1Char, &inlineBuf1, str1Index, compareOptions, langCode, strBuf1, kCFStringStackBufferLength, &str1UsedLen);
                                if (strBuf1Len > 0) {
                                    str1Char = *strBuf1;
                                    strBuf1Index = 1;
                                }
                            }

                            if ((!strBuf1Len) && (0 < strBuf2Len)) {
                                break;
                            }

                            if ((!strBuf2Len) && ((!strBuf1Len) || (str1Char != str2Char))) {
                                strBuf2Len = __CFStringFoldCharacterClusterAtIndex(str2Char, &inlineBuf2, str2Index, compareOptions, langCode, strBuf2, kCFStringStackBufferLength, &str2UsedLen);
                                if ((!strBuf2Len) || (str1Char != *strBuf2)) {
                                    break;
                                }
                                strBuf2Index = 1;
                            }
                        }

                        if ((strBuf1Len > 0) && (strBuf2Len > 0)) {
                            while ((strBuf1Index < strBuf1Len) && (strBuf2Index < strBuf2Len)) {
                                if (strBuf1[strBuf1Index] != strBuf2[strBuf2Index]) {
                                    break;
                                }
                                ++strBuf1Index; ++strBuf2Index;
                            }
                            if ((strBuf1Index < strBuf1Len) && (strBuf2Index < strBuf2Len)) {
                                break;
                            }
                        }
                    }

                    if ((strBuf1Len > 0) && (strBuf1Index == strBuf1Len)) {
                        strBuf1Len = 0;
                    }
                    if ((strBuf2Len > 0) && (strBuf2Index == strBuf2Len)) {
                        strBuf2Len = 0;
                    }

                    if (strBuf1Len == 0) {
                        str1Index += str1UsedLen;
                    }
                    if (strBuf2Len == 0) {
                        str2Index += str2UsedLen;
                    }
                }

                if (str2Index == findStrLen) {
                    bool match = true;

                    if (strBuf1Len > 0) {
                        match = false;

                        if ((compareOptions & kCFCompareDiacriticsInsensitiveCompatibilityMask) && (strBuf1[0] < 0x0510)) {
                            while (strBuf1Index < strBuf1Len) {
                                if (!_CFUniCharIsMemberOfBitmap(strBuf1[strBuf1Index], ((strBuf1[strBuf1Index] < 0x10000) ? graphemeBMP : _CFUniCharGetBitmapPtrForPlane(kCFUniCharCanonicalDecomposableCharacterSet, (strBuf1[strBuf1Index] >> 16))))) {
                                    break;
                                }
                                ++strBuf1Index;
                            }

                            if (strBuf1Index == strBuf1Len) {
                                str1Index += str1UsedLen;
                                match = true;
                            }
                        }
                    }

                    if (match && (compareOptions & (kCFCompareDiacriticsInsensitiveCompatibilityMask | kCFCompareNonliteral)) && (str1Index < (rangeToSearch.location + rangeToSearch.length))) {
                        const uint8_t* nonBaseBitmap;

                        str1Char = CFStringGetCharacterFromInlineBuffer(&inlineBuf1, str1Index);

                        if (_CFUniCharIsSurrogateHighCharacter(str1Char) && _CFUniCharIsSurrogateLowCharacter((otherChar = CFStringGetCharacterFromInlineBuffer(&inlineBuf1, str1Index + 1)))) {
                            str1Char = _CFUniCharGetLongCharacterForSurrogatePair(str1Char, otherChar);
                            nonBaseBitmap = _CFUniCharGetBitmapPtrForPlane(kCFUniCharGraphemeExtendCharacterSet, (str1Char >> 16));
                        } else {
                            nonBaseBitmap = graphemeBMP;
                        }

                        if (_CFUniCharIsMemberOfBitmap(str1Char, nonBaseBitmap)) {
                            if (diacriticsInsensitive) {
                                if (str1Char < 0x10000) {
                                    CFIndex index = str1Index;

                                    do {
                                        str1Char = CFStringGetCharacterFromInlineBuffer(&inlineBuf1, --index);
                                    } while (_CFUniCharIsMemberOfBitmap(str1Char, graphemeBMP), (rangeToSearch.location < index));

                                    if (str1Char < 0x0510) {
                                        CFIndex maxIndex = (rangeToSearch.location + rangeToSearch.length);

                                        while (++str1Index < maxIndex) {
                                            if (!_CFUniCharIsMemberOfBitmap(CFStringGetCharacterFromInlineBuffer(&inlineBuf1, str1Index), graphemeBMP)) {
                                                break;
                                            }
                                        }
                                    }
                                }
                            } else {
                                match = false;
                            }
                        } else if (!diacriticsInsensitive) {
                            otherChar = CFStringGetCharacterFromInlineBuffer(&inlineBuf1, str1Index - 1);

                            // this is assuming viramas are only in BMP ???
                            if ((str1Char == COMBINING_GRAPHEME_JOINER) || (otherChar == COMBINING_GRAPHEME_JOINER) || (otherChar == ZERO_WIDTH_JOINER) || ((otherChar >= HANGUL_CHOSEONG_START) && (otherChar <= HANGUL_JONGSEONG_END)) || (_CFUniCharGetCombiningPropertyForCharacter(otherChar, combClassBMP) == 9)) {
                                CFRange clusterRange = CFStringGetRangeOfCharacterClusterAtIndex(string, str1Index - 1, kCFStringGraphemeCluster);

                                if (str1Index < (clusterRange.location + clusterRange.length)) {
                                    match = false;
                                }
                            }
                        }
                    }

                    if (match) {
                        if (((kCFCompareBackwards | kCFCompareAnchored) != (compareOptions & (kCFCompareBackwards | kCFCompareAnchored))) || (str1Index == (rangeToSearch.location + rangeToSearch.length))) {
                            didFind = true;
                            if (result) {
                                *result = CFRangeMake(fromLoc, str1Index - fromLoc);
                            }
                        }
                        break;
                    }
                }

                if (fromLoc == toLoc) {
                    break;
                }
                fromLoc += delta;
            }
        } else {
            while (1) {
                str1Index = fromLoc;
                str2Index = 0;

                while (str2Index < findStrLen) {
                    if (CFStringGetCharacterFromInlineBuffer(&inlineBuf1, str1Index) != CFStringGetCharacterFromInlineBuffer(&inlineBuf2, str2Index)) {
                        break;
                    }

                    ++str1Index; ++str2Index;
                }

                if (str2Index == findStrLen) {
                    didFind = true;
                    if (result) {
                        *result = CFRangeMake(fromLoc, findStrLen);
                    }
                    break;
                }

                if (fromLoc == toLoc) {
                    break;
                }
                fromLoc += delta;
            }
        }
    }

    return didFind;
}

Boolean CFStringFindWithOptions(CFStringRef string, CFStringRef stringToFind, CFRange rangeToSearch, CFOptionFlags compareOptions, CFRange* result) {
    return CFStringFindWithOptionsAndLocale(string, stringToFind, rangeToSearch, compareOptions, NULL, result);
}

CFRange CFStringFind(CFStringRef string, CFStringRef stringToFind, CFOptionFlags compareOptions) {
    CFRange foundRange;

    if (CFStringFindWithOptions(string, stringToFind, CFRangeMake(0, CFStringGetLength(string)), compareOptions, &foundRange)) {
        return foundRange;
    } else {
        return CFRangeMake(kCFNotFound, 0);
    }
}

Boolean CFStringHasPrefix(CFStringRef string, CFStringRef prefix) {
    return CFStringFindWithOptions(string, prefix, CFRangeMake(0, CFStringGetLength(string)), kCFCompareAnchored, NULL);
}

Boolean CFStringHasSuffix(CFStringRef string, CFStringRef suffix) {
    return CFStringFindWithOptions(string, suffix, CFRangeMake(0, CFStringGetLength(string)), kCFCompareAnchored | kCFCompareBackwards, NULL);
}

void CFStringFold(CFMutableStringRef theString, CFStringCompareFlags theFlags, CFLocaleRef locale) {
    CFStringInlineBuffer stringBuffer;
    CFIndex length = CFStringGetLength(theString);
    CFIndex currentIndex = 0;
    CFIndex bufferLength = 0;
    UTF32Char buffer[kCFStringStackBufferLength];
    const uint8_t* cString;
    const uint8_t* langCode;
    CFStringEncoding eightBitEncoding;
    bool caseInsensitive = ((theFlags & kCFCompareCaseInsensitive) ? true : false);
    bool isObjc = CF_IS_OBJC(theString);
    CFLocaleRef theLocale = locale;

    if ((theFlags & kCFCompareLocalized) && (!locale)) {
        theLocale = CFLocaleCopyCurrent();
    }

    theFlags &= (kCFCompareCaseInsensitive | kCFCompareDiacriticInsensitive | kCFCompareWidthInsensitive);

    if (!theFlags || !length) {
        goto bail; // nothing to do

    }
    langCode = (!theLocale ? NULL : (const uint8_t*)_CFStrGetLanguageIdentifierForLocale(theLocale));

    eightBitEncoding = __CFStringGetEightBitStringEncoding();
    cString = (const uint8_t*)CFStringGetCStringPtr(theString, eightBitEncoding);

    if ((cString) && !caseInsensitive && (kCFStringEncodingASCII == eightBitEncoding)) {
        goto bail; // All ASCII
    }

    CFStringInitInlineBuffer(theString, &stringBuffer, CFRangeMake(0, length));

    if (cString && (theFlags & (kCFCompareCaseInsensitive | kCFCompareDiacriticInsensitive))) {
        const uint8_t* cStringPtr = cString;
        const uint8_t* cStringLimit = cString + length;
        uint8_t* cStringContents = (isObjc ? NULL : (uint8_t*)__CFStrContents(theString) + __CFStrSkipAnyLengthByte(theString));

        while (cStringPtr < cStringLimit) {
            if ((*cStringPtr < 0x80) && !langCode) {
                if (caseInsensitive && (*cStringPtr >= 'A') && (*cStringPtr <= 'Z')) {
                    if (!cStringContents) {
                        break;
                    } else {
                        cStringContents[cStringPtr - cString] += ('a' - 'A');
                    }
                }
            } else {
                bufferLength = __CFStringFoldCharacterClusterAtIndex(
                    (UTF32Char)__CFCharToUniCharTable[*cStringPtr],
                    &stringBuffer, cStringPtr - cString,
                    theFlags,
                    langCode,
                    buffer, kCFStringStackBufferLength, NULL);
                if (bufferLength > 0) {
                    if (*buffer > 0x7F || bufferLength > 1 || !cStringContents) {
                        break;
                    }
                    cStringContents[cStringPtr - cString] = *buffer;
                }
            }
            ++cStringPtr;
        }

        currentIndex = cStringPtr - cString;
    }

    if (currentIndex < length) {
        UTF16Char* contents;

        if (isObjc) {
            CFMutableStringRef cfString;
            CFRange range = CFRangeMake(currentIndex, length - currentIndex);

            contents = (UTF16Char*)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(UTF16Char) * range.length, 0);

            CFStringGetCharacters(theString, range, contents);

            cfString = CFStringCreateMutableWithExternalCharactersNoCopy(kCFAllocatorSystemDefault, contents, range.length, range.length, NULL);

            CFStringFold(cfString, theFlags, theLocale);

            CFStringReplace(theString, range, cfString);

            CFRelease(cfString);
        } else {
            const UTF32Char* characters;
            const UTF32Char* charactersLimit;
            UTF32Char character;
            CFIndex consumedLength;

            contents = NULL;

            if (bufferLength > 0) {
                __CFStringChangeSize(theString, CFRangeMake(currentIndex + 1, 0), bufferLength - 1, true);
                length = __CFStrLength(theString);
                CFStringInitInlineBuffer(theString, &stringBuffer, CFRangeMake(0, length));

                contents = (UTF16Char*)__CFStrContents(theString) + currentIndex;
                characters = buffer;
                charactersLimit = characters + bufferLength;
                while (characters < charactersLimit) {
                    *(contents++) = (UTF16Char) * (characters++);
                }
                ++currentIndex;
            }

            while (currentIndex < length) {
                character = CFStringGetCharacterFromInlineBuffer(&stringBuffer, currentIndex);

                consumedLength = 0;

                if (!langCode && (character < 0x80) && !(theFlags & kCFCompareDiacriticInsensitive)) {
                    if (caseInsensitive && (character >= 'A') && (character <= 'Z')) {
                        consumedLength = 1;
                        bufferLength = 1;
                        *buffer = character + ('a' - 'A');
                    }
                } else {
                    if (_CFUniCharIsSurrogateHighCharacter(character) && (currentIndex + 1) < length) {
                        UTF16Char lowSurrogate = CFStringGetCharacterFromInlineBuffer(&stringBuffer, currentIndex + 1);
                        if (_CFUniCharIsSurrogateLowCharacter(lowSurrogate)) {
                            character = _CFUniCharGetLongCharacterForSurrogatePair(character, lowSurrogate);
                        }
                    }

                    bufferLength = __CFStringFoldCharacterClusterAtIndex(
                        character,
                        &stringBuffer, currentIndex,
                        theFlags,
                        langCode,
                        buffer, kCFStringStackBufferLength, &consumedLength);
                }

                if (consumedLength > 0) {
                    CFIndex utf16Length = bufferLength;

                    characters = buffer;
                    charactersLimit = characters + bufferLength;

                    while (characters < charactersLimit) {
                        if (*(characters++) > 0xFFFF) {
                            ++utf16Length; // Extend bufferLength to the UTF-16 length
                        }
                    }
                    if ((utf16Length != consumedLength) || __CFStrIsEightBit(theString)) {
                        CFRange range;
                        CFIndex insertLength;

                        if (consumedLength < utf16Length) { // Need to expand
                            range = CFRangeMake(currentIndex + consumedLength, 0);
                            insertLength = utf16Length - consumedLength;
                        } else {
                            range = CFRangeMake(currentIndex + utf16Length, consumedLength - utf16Length);
                            insertLength = 0;
                        }
                        __CFStringChangeSize(theString, range, insertLength, true);
                        length = __CFStrLength(theString);
                        CFStringInitInlineBuffer(theString, &stringBuffer, CFRangeMake(0, length));
                    }

                    _CFUniCharFromUTF32(
                        buffer, bufferLength,
                        (UTF16Char*)__CFStrContents(theString) + currentIndex,
                        true,
                        __CF_BIG_ENDIAN__);

                    currentIndex += utf16Length;
                } else {
                    ++currentIndex;
                }
            }
        }
    }

bail:
    if (!locale && theLocale) {
        CFRelease(theLocale);
    }
}

CFRange CFStringGetRangeOfCharacterClusterAtIndex(CFStringRef string, CFIndex charIndex, CFStringCharacterClusterType type) {
    CFRange range;
    CFIndex currentIndex;
    CFIndex length = CFStringGetLength(string);
    CFIndex csetType = ((kCFStringGraphemeCluster == type) ? kCFUniCharGraphemeExtendCharacterSet : kCFUniCharNonBaseCharacterSet);
    CFStringInlineBuffer stringBuffer;
    const uint8_t* bmpBitmap;
    const uint8_t* letterBMP;
    const uint8_t* combClassBMP;
    UTF32Char character;
    UTF16Char otherSurrogate;

    if (charIndex >= length) {
        return CFRangeMake(kCFNotFound, 0);
    }

    /* Fast case.  If we're eight-bit, it's either the default encoding is cheap or the content is all ASCII.
     * Watch out when (or if) adding more 8bit Mac-scripts in CFStringEncodingConverters
     */
    if (!CF_IS_OBJC(string) && __CFStrIsEightBit(string)) {
        return CFRangeMake(charIndex, 1);
    }

    bmpBitmap = _CFUniCharGetBitmapPtrForPlane(csetType, 0);
    letterBMP = _CFUniCharGetBitmapPtrForPlane(kCFUniCharLetterCharacterSet, 0);
    combClassBMP = (const uint8_t*)_CFUniCharGetUnicodePropertyDataForPlane(kCFUniCharCombiningProperty, 0);

    CFStringInitInlineBuffer(string, &stringBuffer, CFRangeMake(0, length));

    // Get composed character sequence first
    range = _CFStringInlineBufferGetComposedRange(&stringBuffer, charIndex, type, bmpBitmap, csetType);

    // Do grapheme joiners
    if (type < kCFStringCursorMovementCluster) {
        const uint8_t* letter = letterBMP;

        // Check to see if we have a letter at the beginning of initial cluster
        character = CFStringGetCharacterFromInlineBuffer(&stringBuffer, range.location);

        if ((range.length > 1) && _CFUniCharIsSurrogateHighCharacter(character) &&
            _CFUniCharIsSurrogateLowCharacter(
                (otherSurrogate = CFStringGetCharacterFromInlineBuffer(&stringBuffer, range.location + 1))))
        {
            character = _CFUniCharGetLongCharacterForSurrogatePair(character, otherSurrogate);
            letter = _CFUniCharGetBitmapPtrForPlane(kCFUniCharLetterCharacterSet, (character >> 16));
        }

        if ((character == ZERO_WIDTH_JOINER) || _CFUniCharIsMemberOfBitmap(character, letter)) {
            CFRange otherRange;

            // Check if preceded by grapheme joiners (U034F and viramas)
            otherRange.location = currentIndex = range.location;

            while (currentIndex > 1) {
                character = CFStringGetCharacterFromInlineBuffer(&stringBuffer, --currentIndex);

                // ??? We're assuming viramas only in BMP
                if ((_CFStringIsVirama(character, combClassBMP) || 
                        ((character == ZERO_WIDTH_JOINER) && 
                            _CFStringIsVirama(
                                CFStringGetCharacterFromInlineBuffer(&stringBuffer, --currentIndex), combClassBMP)))
                    && (currentIndex > 0))
                {
                    --currentIndex;
                } else {
                    break;
                }

                currentIndex = _CFStringInlineBufferGetComposedRange(&stringBuffer, currentIndex, type, bmpBitmap, csetType).location;

                character = CFStringGetCharacterFromInlineBuffer(&stringBuffer, currentIndex);

                if (_CFUniCharIsSurrogateLowCharacter(character) &&
                    _CFUniCharIsSurrogateHighCharacter(
                        (otherSurrogate = CFStringGetCharacterFromInlineBuffer(&stringBuffer, currentIndex - 1))))
                {
                    character = _CFUniCharGetLongCharacterForSurrogatePair(character, otherSurrogate);
                    letter = _CFUniCharGetBitmapPtrForPlane(kCFUniCharLetterCharacterSet, (character >> 16));
                    --currentIndex;
                } else {
                    letter = letterBMP;
                }

                if (!_CFUniCharIsMemberOfBitmap(character, letter)) {
                    break;
                }
                range.location = currentIndex;
            }

            range.length += otherRange.location - range.location;

            // Check if followed by grapheme joiners
            if ((range.length > 1) && ((range.location + range.length) < length)) {
                otherRange = range;
                currentIndex = otherRange.location + otherRange.length;

                do {
                    character = CFStringGetCharacterFromInlineBuffer(&stringBuffer, currentIndex - 1);

                    // ??? We're assuming viramas only in BMP
                    if ((character != ZERO_WIDTH_JOINER) && !_CFStringIsVirama(character, combClassBMP)) {
                        break;
                    }

                    character = CFStringGetCharacterFromInlineBuffer(&stringBuffer, currentIndex);

                    if (character == ZERO_WIDTH_JOINER) {
                        character = CFStringGetCharacterFromInlineBuffer(&stringBuffer, ++currentIndex);
                    }

                    if (_CFUniCharIsSurrogateHighCharacter(character) &&
                        _CFUniCharIsSurrogateLowCharacter(
                            (otherSurrogate = CFStringGetCharacterFromInlineBuffer(&stringBuffer, currentIndex + 1))))
                    {
                        character = _CFUniCharGetLongCharacterForSurrogatePair(character, otherSurrogate);
                        letter = _CFUniCharGetBitmapPtrForPlane(kCFUniCharLetterCharacterSet, (character >> 16));
                    } else {
                        letter = letterBMP;
                    }

                    // We only conjoin letters
                    if (!_CFUniCharIsMemberOfBitmap(character, letter)) {
                        break;
                    }
                    otherRange = _CFStringInlineBufferGetComposedRange(&stringBuffer, currentIndex, type, bmpBitmap, csetType);
                    currentIndex = otherRange.location + otherRange.length;
                } while ((otherRange.location + otherRange.length) < length);
                range.length = currentIndex - range.location;
            }
        }
    }

    // Check if we're part of prefix transcoding hints
    CFIndex otherIndex;

    currentIndex = (range.location + range.length) - (MAX_TRANSCODING_LENGTH + 1);
    if (currentIndex < 0) {
        currentIndex = 0;
    }

    while (currentIndex <= range.location) {
        character = CFStringGetCharacterFromInlineBuffer(&stringBuffer, currentIndex);

        if ((character & 0x1FFFF0) == 0xF860) { // transcoding hint
            otherIndex = currentIndex + __CFTranscodingHintLength[(character - 0xF860)] + 1;
            if (otherIndex >= (range.location + range.length)) {
                if (otherIndex <= length) {
                    range.location = currentIndex;
                    range.length = otherIndex - currentIndex;
                }
                break;
            }
        }
        ++currentIndex;
    }

    return range;
}

CFRange CFStringGetRangeOfComposedCharactersAtIndex(CFStringRef theString, CFIndex theIndex) {
    return CFStringGetRangeOfCharacterClusterAtIndex(theString, theIndex, kCFStringComposedCharacterCluster);
}
