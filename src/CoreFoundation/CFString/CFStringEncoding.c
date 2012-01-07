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

#include "CFInternal.h"
#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFDictionary.h>
#include "CFUniChar.h"
#include "CFStringEncoding.h"
#include "CFStringEncodingConverter.h"
#include <stdlib.h>
#include <pthread.h>

// TODO: rename to _kCFUniCharXXX, replace other constants
#define kSurrogateHighStart 0xD800
#define kSurrogateHighEnd    0xDBFF
#define kSurrogateLowStart    0xDC00
#define kSurrogateLowEnd    0xDFFF


#define MAX_IANA_ALIASES (4)

typedef CFIndex (*_CFToBytesProc)(
    const void *converter, uint32_t flags,
    const UniChar *characters, CFIndex numChars,
    uint8_t *bytes, CFIndex maxByteLen, CFIndex *usedByteLen);

typedef CFIndex (*_CFToUnicodeProc)(
    const void *converter, uint32_t flags,
    const uint8_t *bytes, CFIndex numBytes,
    UniChar *characters, CFIndex maxCharLen, CFIndex *usedCharLen);

typedef struct {
    _CFToBytesProc toBytes;
    _CFToUnicodeProc toUnicode;
    _CFToUnicodeProc toCanonicalUnicode;
    void *_toBytes; // original proc
    void *_toUnicode; // original proc
    uint16_t maxLen;
    uint16_t :16;
    CFStringEncodingToBytesLenProc toBytesLen;
    CFStringEncodingToUnicodeLenProc toUnicodeLen;
    CFStringEncodingToBytesFallbackProc toBytesFallback;
    CFStringEncodingToUnicodeFallbackProc toUnicodeFallback;
    CFStringEncodingToBytesPrecomposeProc toBytesPrecompose;
    CFStringEncodingIsValidCombiningCharacterProc isValidCombiningChar;
} __CFConverter;

typedef struct {
    uint32_t encoding;
    __CFConverter *converter;
    const char *encodingName;
    const char *ianaNames[MAX_IANA_ALIASES];
    const char *loadablePath;
    CFStringEncodingToBytesFallbackProc toBytesFallback;
    CFStringEncodingToUnicodeFallbackProc toUnicodeFallback;
    uint32_t scriptCode;
} __CFConverterEntry;

/* Mapping 128..255 to lossy ASCII
 */
typedef struct {
    char chars[4];
} __CFLossyASCIITableEntry;

static __CFLossyASCIITableEntry __CFLossyASCIITable[] = {
    {{' ', 0,    0,     0}}, // NO-BREAK SPACE
    {{'!', 0,    0,     0}}, // INVERTED EXCLAMATION MARK
    {{'c', 0,    0,     0}}, // CENT SIGN
    {{'L', 0,    0,     0}}, // POUND SIGN
    {{'$', 0,    0,     0}}, // CURRENCY SIGN
    {{'Y', 0,    0,     0}}, // YEN SIGN
    {{'|', 0,    0,     0}}, // BROKEN BAR
    {{0,   0,    0,     0}}, // SECTION SIGN
    {{0,   0,    0,     0}}, // DIAERESIS
    {{'(', 'C', ')', 0}}, // COPYRIGHT SIGN
    {{'a', 0,    0,     0}}, // FEMININE ORDINAL INDICATOR
    {{'<', '<', 0,     0}}, // LEFT-POINTING DOUBLE ANGLE QUOTATION MARK
    {{0,   0,    0,     0}}, // NOT SIGN
    {{'-', 0,    0,     0}}, // SOFT HYPHEN
    {{'(', 'R', ')', 0}}, // REGISTERED SIGN
    {{0,   0,    0,     0}}, // MACRON
    {{0,   0,    0,     0}}, // DEGREE SIGN
    {{'+', '-', 0,     0}}, // PLUS-MINUS SIGN
    {{'2', 0,    0,     0}}, // SUPERSCRIPT TWO
    {{'3', 0,    0,     0}}, // SUPERSCRIPT THREE
    {{0,   0,    0,     0}}, // ACUTE ACCENT
    {{0,   0,    0,     0}}, // MICRO SIGN
    {{0,   0,    0,     0}}, // PILCROW SIGN
    {{0,   0,    0,     0}}, // MIDDLE DOT
    {{0,   0,    0,     0}}, // CEDILLA
    {{'1', 0,    0,     0}}, // SUPERSCRIPT ONE
    {{'o', 0,    0,     0}}, // MASCULINE ORDINAL INDICATOR
    {{'>', '>', 0,     0}}, // RIGHT-POINTING DOUBLE ANGLE QUOTATION MARK
    {{'1', '/', '4', 0}}, // VULGAR FRACTION ONE QUARTER
    {{'1', '/', '2', 0}}, // VULGAR FRACTION ONE HALF
    {{'3', '/', '4', 0}}, // VULGAR FRACTION THREE QUARTERS
    {{'?', 0,    0,     0}}, // INVERTED QUESTION MARK
    {{'A', 0,    0,     0}}, // LATIN CAPITAL LETTER A WITH GRAVE
    {{'A', 0,    0,     0}}, // LATIN CAPITAL LETTER A WITH ACUTE
    {{'A', 0,    0,     0}}, // LATIN CAPITAL LETTER A WITH CIRCUMFLEX
    {{'A', 0,    0,     0}}, // LATIN CAPITAL LETTER A WITH TILDE
    {{'A', 0,    0,     0}}, // LATIN CAPITAL LETTER A WITH DIAERESIS
    {{'A', 0,    0,     0}}, // LATIN CAPITAL LETTER A WITH RING ABOVE
    {{'A', 'E', 0,     0}}, // LATIN CAPITAL LETTER AE
    {{'C', 0,    0,     0}}, // LATIN CAPITAL LETTER C WITH CEDILLA
    {{'E', 0,    0,     0}}, // LATIN CAPITAL LETTER E WITH GRAVE
    {{'E', 0,    0,     0}}, // LATIN CAPITAL LETTER E WITH ACUTE
    {{'E', 0,    0,     0}}, // LATIN CAPITAL LETTER E WITH CIRCUMFLEX
    {{'E', 0,    0,     0}}, // LATIN CAPITAL LETTER E WITH DIAERESIS
    {{'I', 0,    0,     0}}, // LATIN CAPITAL LETTER I WITH GRAVE
    {{'I', 0,    0,     0}}, // LATIN CAPITAL LETTER I WITH ACUTE
    {{'I', 0,    0,     0}}, // LATIN CAPITAL LETTER I WITH CIRCUMFLEX
    {{'I', 0,    0,     0}}, // LATIN CAPITAL LETTER I WITH DIAERESIS
    {{'T', 'H', 0,     0}}, // LATIN CAPITAL LETTER ETH (Icelandic)
    {{'N', 0,    0,     0}}, // LATIN CAPITAL LETTER N WITH TILDE
    {{'O', 0,    0,     0}}, // LATIN CAPITAL LETTER O WITH GRAVE
    {{'O', 0,    0,     0}}, // LATIN CAPITAL LETTER O WITH ACUTE
    {{'O', 0,    0,     0}}, // LATIN CAPITAL LETTER O WITH CIRCUMFLEX
    {{'O', 0,    0,     0}}, // LATIN CAPITAL LETTER O WITH TILDE
    {{'O', 0,    0,     0}}, // LATIN CAPITAL LETTER O WITH DIAERESIS
    {{'X', 0,    0,     0}}, // MULTIPLICATION SIGN
    {{'O', 0,    0,     0}}, // LATIN CAPITAL LETTER O WITH STROKE
    {{'U', 0,    0,     0}}, // LATIN CAPITAL LETTER U WITH GRAVE
    {{'U', 0,    0,     0}}, // LATIN CAPITAL LETTER U WITH ACUTE
    {{'U', 0,    0,     0}}, // LATIN CAPITAL LETTER U WITH CIRCUMFLEX
    {{'U', 0,    0,     0}}, // LATIN CAPITAL LETTER U WITH DIAERESIS
    {{'Y', 0,    0,     0}}, // LATIN CAPITAL LETTER Y WITH ACUTE
    {{'t', 'h', 0,     0}}, // LATIN CAPITAL LETTER THORN (Icelandic)
    {{'s', 0,    0,     0}}, // LATIN SMALL LETTER SHARP S (German)
    {{'a', 0,    0,     0}}, // LATIN SMALL LETTER A WITH GRAVE
    {{'a', 0,    0,     0}}, // LATIN SMALL LETTER A WITH ACUTE
    {{'a', 0,    0,     0}}, // LATIN SMALL LETTER A WITH CIRCUMFLEX
    {{'a', 0,    0,     0}}, // LATIN SMALL LETTER A WITH TILDE
    {{'a', 0,    0,     0}}, // LATIN SMALL LETTER A WITH DIAERESIS
    {{'a', 0,    0,     0}}, // LATIN SMALL LETTER A WITH RING ABOVE
    {{'a', 'e', 0,     0}}, // LATIN SMALL LETTER AE
    {{'c', 0,    0,     0}}, // LATIN SMALL LETTER C WITH CEDILLA
    {{'e', 0,    0,     0}}, // LATIN SMALL LETTER E WITH GRAVE
    {{'e', 0,    0,     0}}, // LATIN SMALL LETTER E WITH ACUTE
    {{'e', 0,    0,     0}}, // LATIN SMALL LETTER E WITH CIRCUMFLEX
    {{'e', 0,    0,     0}}, // LATIN SMALL LETTER E WITH DIAERESIS
    {{'i', 0,    0,     0}}, // LATIN SMALL LETTER I WITH GRAVE
    {{'i', 0,    0,     0}}, // LATIN SMALL LETTER I WITH ACUTE
    {{'i', 0,    0,     0}}, // LATIN SMALL LETTER I WITH CIRCUMFLEX
    {{'i', 0,    0,     0}}, // LATIN SMALL LETTER I WITH DIAERESIS
    {{'T', 'H', 0,     0}}, // LATIN SMALL LETTER ETH (Icelandic)
    {{'n', 0,    0,     0}}, // LATIN SMALL LETTER N WITH TILDE
    {{'o', 0,    0,     0}}, // LATIN SMALL LETTER O WITH GRAVE
    {{'o', 0,    0,     0}}, // LATIN SMALL LETTER O WITH ACUTE
    {{'o', 0,    0,     0}}, // LATIN SMALL LETTER O WITH CIRCUMFLEX
    {{'o', 0,    0,     0}}, // LATIN SMALL LETTER O WITH TILDE
    {{'o', 0,    0,     0}}, // LATIN SMALL LETTER O WITH DIAERESIS
    {{'/', 0,    0,     0}}, // DIVISION SIGN
    {{'o', 0,    0,     0}}, // LATIN SMALL LETTER O WITH STROKE
    {{'u', 0,    0,     0}}, // LATIN SMALL LETTER U WITH GRAVE
    {{'u', 0,    0,     0}}, // LATIN SMALL LETTER U WITH ACUTE
    {{'u', 0,    0,     0}}, // LATIN SMALL LETTER U WITH CIRCUMFLEX
    {{'u', 0,    0,     0}}, // LATIN SMALL LETTER U WITH DIAERESIS
    {{'y', 0,    0,     0}}, // LATIN SMALL LETTER Y WITH ACUTE
    {{'t', 'h', 0,     0}}, // LATIN SMALL LETTER THORN (Icelandic)
    {{'y', 0,    0,     0}}, // LATIN SMALL LETTER Y WITH DIAERESIS
};

static __CFConverterEntry __CFConverterEntryASCII = {
    kCFStringEncodingASCII,
    NULL,
    "Western (ASCII)",
    {"us-ascii", "ascii", "iso-646-us", NULL},
    NULL, NULL, NULL,
    kCFStringEncodingMacRoman // We use string encoding's script range here
};

static __CFConverterEntry __CFConverterEntryISOLatin1 = {
    kCFStringEncodingISOLatin1,
    NULL,
    "Western (ISO Latin 1)",
    {"iso-8859-1", "latin1", "iso-latin-1", NULL},
    NULL, NULL, NULL,
    kCFStringEncodingMacRoman // We use string encoding's script range here
};

static __CFConverterEntry __CFConverterEntryMacRoman = {
    kCFStringEncodingMacRoman,
    NULL,
    "Western (Mac OS Roman)",
    {"macintosh", "mac",     "x-mac-roman", NULL},
    NULL, NULL, NULL,
    kCFStringEncodingMacRoman // We use string encoding's script range here
};

static __CFConverterEntry __CFConverterEntryWinLatin1 = {
    kCFStringEncodingWindowsLatin1,
    NULL,
    "Western (Windows Latin 1)",
    {"windows-1252", "cp1252", "windows latin1", NULL},
    NULL, NULL, NULL,
    kCFStringEncodingMacRoman // We use string encoding's script range here
};

static __CFConverterEntry __CFConverterEntryNextStepLatin = {
    kCFStringEncodingNextStepLatin,
    NULL,
    "Western (NextStep)",
    {"x-nextstep" ,NULL, NULL, NULL},
    NULL, NULL, NULL,
    kCFStringEncodingMacRoman // We use string encoding's script range here
};

static __CFConverterEntry __CFConverterEntryUTF8 = {
    kCFStringEncodingUTF8,
    NULL,
    "UTF-8",
    {"utf-8", "unicode-1-1-utf8", NULL, NULL},
    NULL, NULL, NULL,
    kCFStringEncodingUnicode // We use string encoding's script range here
};

static const uint32_t __CFBuiltinEncodings[] = {
    kCFStringEncodingMacRoman,
    kCFStringEncodingWindowsLatin1,
    kCFStringEncodingISOLatin1,
    kCFStringEncodingNextStepLatin,
    kCFStringEncodingASCII,
    kCFStringEncodingUTF8,

    /* These seven are available only in CFString-level */
    kCFStringEncodingNonLossyASCII,
    kCFStringEncodingUTF16,
    kCFStringEncodingUTF16BE,
    kCFStringEncodingUTF16LE,
    kCFStringEncodingUTF32,
    kCFStringEncodingUTF32BE,
    kCFStringEncodingUTF32LE,

    kCFStringEncodingInvalidId,
};

///////////////////////////////////////////////////////////////////// private

CF_INLINE CFIndex __CFConvertToBytes(const __CFConverter* converter,
                                     uint32_t flags,
                                     const UniChar* chars, CFIndex numChars,
                                     uint8_t* bytes, CFIndex maxByteLen, CFIndex* usedByteLen)
{
    if (converter->toBytes) {
        return converter->toBytes(converter, 
            flags,
            chars, numChars,
            bytes, maxByteLen, usedByteLen);
    } else {
        return ((CFStringEncodingToBytesProc)converter->_toBytes)(
            flags,
            chars, numChars,
            bytes, maxByteLen, usedByteLen);
    }
}

CF_INLINE CFIndex __CFConvertToUnicode(const __CFConverter* converter,
                                       uint32_t flags,
                                       const uint8_t* bytes, CFIndex numBytes,
                                       UniChar* chars, CFIndex maxCharLen, CFIndex *usedCharLen)
{
    if (converter->toUnicode) {
        _CFToUnicodeProc toUnicode = 
            (flags & (kCFStringEncodingUseCanonical | kCFStringEncodingUseHFSPlusCanonical))?
                converter->toCanonicalUnicode:
                converter->toUnicode;
        return toUnicode(converter,
            flags,
            bytes, numBytes,
            chars, maxCharLen, usedCharLen);
    } else {
        return ((CFStringEncodingToUnicodeProc)converter->_toUnicode)(
            flags,
            bytes, numBytes,
            chars, maxCharLen, usedCharLen);
    }
}

CF_INLINE CFIndex __CFToASCIILatin1Fallback(UniChar character, uint8_t* bytes, CFIndex maxByteLen) {
    const __CFLossyASCIITableEntry* lossy = __CFLossyASCIITable + (character - 0xA0);
    CFIndex numBytes = 0;
    CFIndex idx, max = (maxByteLen && (maxByteLen < 4) ? maxByteLen : 4);

    for (idx = 0; idx < max; idx++) {
        if (lossy->chars[idx]) {
            if (maxByteLen) {
                bytes[idx] = lossy->chars[idx];
            }
            ++numBytes;
        } else {
            break;
        }
    }

    return numBytes;
}

static CFIndex __CFDefaultToBytesFallbackProc(const UniChar* characters, CFIndex numChars,
                                              uint8_t* bytes, CFIndex maxByteLen, CFIndex* usedByteLen)
{
    CFIndex processCharLen = 1, filledBytesLen = 1;
    uint8_t byte = '?';

    if (*characters < 0xA0) { // 0x80 to 0x9F maps to ASCII C0 range
        byte = (uint8_t)(*characters - 0x80);
    } else if (*characters < 0x100) {
        *usedByteLen = __CFToASCIILatin1Fallback(*characters, bytes, maxByteLen);
        return 1;
    } else if (*characters >= kSurrogateHighStart && *characters <= kSurrogateLowEnd) {
        processCharLen = (numChars > 1 && *characters <= kSurrogateLowStart && *(characters + 1) >= kSurrogateLowStart && *(characters + 1) <= kSurrogateLowEnd ? 2 : 1);
    } else if (_CFUniCharIsMemberOf(*characters, kCFUniCharWhitespaceCharacterSet)) {
        byte = ' ';
    } else if (_CFUniCharIsMemberOf(*characters, kCFUniCharWhitespaceAndNewlineCharacterSet)) {
        byte = 0x0a; // ASCIINewLine
    } else if (*characters == 0x2026) { // ellipsis
        if (0 == maxByteLen) {
            filledBytesLen = 3;
        } else if (maxByteLen > 2) {
            memset(bytes, '.', 3);
            *usedByteLen = 3;
            return processCharLen;
        }
    } else if (_CFUniCharIsMemberOf(*characters, kCFUniCharDecomposableCharacterSet)) {
        UTF32Char decomposed[kCFUniCharMaxDecomposedLength];

        (void)_CFUniCharDecomposeCharacter(*characters, decomposed, kCFUniCharMaxDecomposedLength);
        if (*decomposed < 0x80) {
            byte = (uint8_t)(*decomposed);
        } else {
            UTF16Char theChar = *decomposed;

            return __CFDefaultToBytesFallbackProc(&theChar, 1, bytes, maxByteLen, usedByteLen);
        }
    }

    if (maxByteLen) {
        *bytes = byte;
    }
    *usedByteLen = filledBytesLen;
    return processCharLen;
}

static CFIndex __CFDefaultToUnicodeFallbackProc(const uint8_t* bytes, CFIndex numBytes,
                                                UniChar* characters, CFIndex maxCharLen, CFIndex* usedCharLen)
{
    if (maxCharLen) {
        *characters = (UniChar)'?';
    }
    *usedCharLen = 1;
    return 1;
}

/* Wrapper funcs for non-standard converters
 */
static CFIndex __CFToBytesCheapEightBitWrapper(const void* converter,
                                               uint32_t flags,
                                               const UniChar* characters, CFIndex numChars,
                                               uint8_t* bytes, CFIndex maxByteLen, CFIndex* usedByteLen)
{
    CFIndex processedCharLen = 0;
    CFIndex length = (maxByteLen && (maxByteLen < numChars) ? maxByteLen : numChars);
    uint8_t byte;
    CFStringEncodingCheapEightBitToBytesProc toBytes =
        (CFStringEncodingCheapEightBitToBytesProc)((const __CFConverter*)converter)->_toBytes;

    while (processedCharLen < length) {
        if (!toBytes(flags, characters[processedCharLen], &byte)) {
            break;
        }

        if (maxByteLen) {
            bytes[processedCharLen] = byte;
        }
        processedCharLen++;
    }

    *usedByteLen = processedCharLen;
    return processedCharLen;
}

static CFIndex __CFToUnicodeCheapEightBitWrapper(const void* converter,
                                                 uint32_t flags,
                                                 const uint8_t* bytes, CFIndex numBytes,
                                                 UniChar* characters, CFIndex maxCharLen, CFIndex* usedCharLen)
{
    CFIndex processedByteLen = 0;
    CFIndex length = (maxCharLen && (maxCharLen < numBytes) ? maxCharLen : numBytes);
    UniChar character;
    CFStringEncodingCheapEightBitToUnicodeProc toUnicode =
        (CFStringEncodingCheapEightBitToUnicodeProc)((const __CFConverter*)converter)->_toUnicode;

    while (processedByteLen < length) {
        if (!toUnicode(flags, bytes[processedByteLen], &character)) {
            break;
        }

        if (maxCharLen) {
            characters[processedByteLen] = character;
        }
        processedByteLen++;
    }

    *usedCharLen = processedByteLen;
    return processedByteLen;
}

static CFIndex __CFToCanonicalUnicodeCheapEightBitWrapper(const void* converter,
                                                          uint32_t flags,
                                                          const uint8_t* bytes, CFIndex numBytes,
                                                          UniChar* characters, CFIndex maxCharLen, CFIndex* usedCharLen)
{
    CFIndex processedByteLen = 0;
    CFIndex theUsedCharLen = 0;
    UTF32Char charBuffer[kCFUniCharMaxDecomposedLength];
    CFIndex usedLen;
    UniChar character;
    bool isHFSPlus = (flags & kCFStringEncodingUseHFSPlusCanonical ? true : false);
    CFStringEncodingCheapEightBitToUnicodeProc toUnicode =
        (CFStringEncodingCheapEightBitToUnicodeProc)((const __CFConverter*)converter)->_toUnicode;

    while ((processedByteLen < numBytes) && (!maxCharLen || (theUsedCharLen < maxCharLen))) {
        if (!toUnicode(flags, bytes[processedByteLen], &character)) {
            break;
        }

        if (_CFUniCharIsDecomposableCharacter(character)) {
            CFIndex idx;

            usedLen = _CFUniCharDecomposeCharacter(character, charBuffer, kCFUniCharMaxDecomposedLength);
            *usedCharLen = theUsedCharLen;

            for (idx = 0; idx < usedLen; idx++) {
                if (charBuffer[idx] > 0xFFFF) { // Non-BMP
                    if (theUsedCharLen + 2 > maxCharLen) {
                        return processedByteLen;
                    }
                    theUsedCharLen += 2;
                    if (maxCharLen) {
                        charBuffer[idx] = charBuffer[idx] - 0x10000;
                        *(characters++) = (UniChar)(charBuffer[idx] >> 10) + 0xD800UL;
                        *(characters++) = (UniChar)(charBuffer[idx] & 0x3FF) + 0xDC00UL;
                    }
                } else {
                    if (theUsedCharLen + 1 > maxCharLen) {
                        return processedByteLen;
                    }
                    ++theUsedCharLen;
                    *(characters++) = charBuffer[idx];
                }
            }
        } else {
            if (maxCharLen) {
                *(characters++) = character;
            }
            ++theUsedCharLen;
        }
        processedByteLen++;
    }

    *usedCharLen = theUsedCharLen;
    return processedByteLen;
}

static CFIndex __CFToBytesStandardEightBitWrapper(const void* converter,
                                                  uint32_t flags,
                                                  const UniChar* characters, CFIndex numChars,
                                                  uint8_t* bytes, CFIndex maxByteLen, CFIndex* usedByteLen)
{
    CFIndex processedCharLen = 0;
    uint8_t byte;
    CFIndex usedLen;
    CFStringEncodingStandardEightBitToBytesProc toBytes =
        (CFStringEncodingStandardEightBitToBytesProc)((const __CFConverter*)converter)->_toBytes;

    *usedByteLen = 0;

    while (numChars && (!maxByteLen || (*usedByteLen < maxByteLen))) {
        if (!(usedLen = toBytes(flags, characters, numChars, &byte))) {
            break;
        }

        if (maxByteLen) {
            bytes[*usedByteLen] = byte;
        }
        (*usedByteLen)++;
        characters += usedLen;
        numChars -= usedLen;
        processedCharLen += usedLen;
    }

    return processedCharLen;
}

static CFIndex __CFToUnicodeStandardEightBitWrapper(const void* converter,
                                                    uint32_t flags,
                                                    const uint8_t* bytes, CFIndex numBytes,
                                                    UniChar* characters, CFIndex maxCharLen, CFIndex* usedCharLen)
{
    CFIndex processedByteLen = 0;
    _CF_ARRAY_ALLOCA(UniChar, charBuffer, ((const __CFConverter*)converter)->maxLen);
    CFIndex usedLen;
    CFStringEncodingCheapEightBitToUnicodeProc toUnicode =
        (CFStringEncodingCheapEightBitToUnicodeProc)((const __CFConverter*)converter)->_toUnicode;

    *usedCharLen = 0;

    while ((processedByteLen < numBytes) && (!maxCharLen || (*usedCharLen < maxCharLen))) {
        if (!(usedLen = toUnicode(flags, bytes[processedByteLen], charBuffer))) {
            break;
        }

        if (maxCharLen) {
            CFIndex idx;

            if (*usedCharLen + usedLen > maxCharLen) {
                break;
            }

            for (idx = 0; idx < usedLen; idx++) {
                characters[*usedCharLen + idx] = charBuffer[idx];
            }
        }
        *usedCharLen += usedLen;
        processedByteLen++;
    }

    return processedByteLen;
}

static CFIndex __CFToCanonicalUnicodeStandardEightBitWrapper(const void* converter,
                                                             uint32_t flags,
                                                             const uint8_t* bytes, CFIndex numBytes,
                                                             UniChar* characters, CFIndex maxCharLen, CFIndex* usedCharLen)
{
    CFIndex processedByteLen = 0;
    _CF_ARRAY_ALLOCA(UniChar, charBuffer, ((const __CFConverter*)converter)->maxLen);
    UTF32Char decompBuffer[kCFUniCharMaxDecomposedLength];
    CFIndex usedLen;
    CFIndex decompedLen;
    CFIndex idx, decompIndex;
    bool isHFSPlus = (flags & kCFStringEncodingUseHFSPlusCanonical ? true : false);
    CFIndex theUsedCharLen = 0;
    CFStringEncodingCheapEightBitToUnicodeProc toUnicode =
        (CFStringEncodingCheapEightBitToUnicodeProc)((const __CFConverter*)converter)->_toUnicode;

    while ((processedByteLen < numBytes) && (!maxCharLen || (theUsedCharLen < maxCharLen))) {
        if (!(usedLen = toUnicode(flags, bytes[processedByteLen], charBuffer))) {
            break;
        }

        for (idx = 0; idx < usedLen; idx++) {
            if (_CFUniCharIsDecomposableCharacter(charBuffer[idx])) {
                decompedLen = _CFUniCharDecomposeCharacter(charBuffer[idx], decompBuffer, kCFUniCharMaxDecomposedLength);
                *usedCharLen = theUsedCharLen;

                for (decompIndex = 0; decompIndex < decompedLen; decompIndex++) {
                    if (decompBuffer[decompIndex] > 0xFFFF) { // Non-BMP
                        if (theUsedCharLen + 2 > maxCharLen) {
                            return processedByteLen;
                        }
                        theUsedCharLen += 2;
                        if (maxCharLen) {
                            charBuffer[idx] = charBuffer[idx] - 0x10000;
                            *(characters++) = (charBuffer[idx] >> 10) + 0xD800UL;
                            *(characters++) = (charBuffer[idx] & 0x3FF) + 0xDC00UL;
                        }
                    } else {
                        if (theUsedCharLen + 1 > maxCharLen) {
                            return processedByteLen;
                        }
                        ++theUsedCharLen;
                        *(characters++) = charBuffer[idx];
                    }
                }
            } else {
                if (maxCharLen) {
                    *(characters++) = charBuffer[idx];
                }
                ++theUsedCharLen;
            }
        }
        processedByteLen++;
    }

    *usedCharLen = theUsedCharLen;
    return processedByteLen;
}

static CFIndex __CFToBytesCheapMultiByteWrapper(const void* converter,
                                                uint32_t flags,
                                                const UniChar* characters, CFIndex numChars,
                                                uint8_t* bytes, CFIndex maxByteLen, CFIndex* usedByteLen)
{
    CFIndex processedCharLen = 0;
    _CF_ARRAY_ALLOCA(uint8_t, byteBuffer, ((const __CFConverter*)converter)->maxLen);
    CFIndex usedLen;
    CFStringEncodingCheapMultiByteToBytesProc toBytes =
        (CFStringEncodingCheapMultiByteToBytesProc)((const __CFConverter*)converter)->_toBytes;

    *usedByteLen = 0;

    while ((processedCharLen < numChars) && (!maxByteLen || (*usedByteLen < maxByteLen))) {
        if (!(usedLen = toBytes(flags, characters[processedCharLen], byteBuffer))) {
            break;
        }

        if (maxByteLen) {
            CFIndex idx;

            if (*usedByteLen + usedLen > maxByteLen) {
                break;
            }

            for (idx = 0; idx < usedLen; idx++) {
                bytes[*usedByteLen + idx] = byteBuffer[idx];
            }
        }

        *usedByteLen += usedLen;
        processedCharLen++;
    }

    return processedCharLen;
}

static CFIndex __CFToUnicodeCheapMultiByteWrapper(const void* converter,
                                                  uint32_t flags,
                                                  const uint8_t* bytes, CFIndex numBytes,
                                                  UniChar* characters, CFIndex maxCharLen, CFIndex* usedCharLen)
{
    CFIndex processedByteLen = 0;
    UniChar character;
    CFIndex usedLen;
    CFStringEncodingCheapMultiByteToUnicodeProc toUnicode =
        (CFStringEncodingCheapMultiByteToUnicodeProc)((const __CFConverter*)converter)->_toUnicode;

    *usedCharLen = 0;

    while (numBytes && (!maxCharLen || (*usedCharLen < maxCharLen))) {
        if (!(usedLen = toUnicode(flags, bytes, numBytes, &character))) {
            break;
        }

        if (maxCharLen) {
            *(characters++) = character;
        }
        (*usedCharLen)++;
        processedByteLen += usedLen;
        bytes += usedLen;
        numBytes -= usedLen;
    }

    return processedByteLen;
}

static CFIndex __CFToCanonicalUnicodeCheapMultiByteWrapper(const void* converter,
                                                           uint32_t flags,
                                                           const uint8_t* bytes, CFIndex numBytes,
                                                           UniChar* characters, CFIndex maxCharLen, CFIndex* usedCharLen)
{
    CFIndex processedByteLen = 0;
    UTF32Char charBuffer[kCFUniCharMaxDecomposedLength];
    UniChar character;
    CFIndex usedLen;
    CFIndex decomposedLen;
    CFIndex theUsedCharLen = 0;
    bool isHFSPlus = (flags & kCFStringEncodingUseHFSPlusCanonical ? true : false);
    CFStringEncodingCheapMultiByteToUnicodeProc toUnicode =
        (CFStringEncodingCheapMultiByteToUnicodeProc)((const __CFConverter*)converter)->_toUnicode;

    while (numBytes && (!maxCharLen || (theUsedCharLen < maxCharLen))) {
        if (!(usedLen = toUnicode(flags, bytes, numBytes, &character))) {
            break;
        }

        if (_CFUniCharIsDecomposableCharacter(character)) {
            CFIndex idx;

            decomposedLen = _CFUniCharDecomposeCharacter(character, charBuffer, kCFUniCharMaxDecomposedLength);
            *usedCharLen = theUsedCharLen;

            for (idx = 0; idx < decomposedLen; idx++) {
                if (charBuffer[idx] > 0xFFFF) { // Non-BMP
                    if (theUsedCharLen + 2 > maxCharLen) {
                        return processedByteLen;
                    }
                    theUsedCharLen += 2;
                    if (maxCharLen) {
                        charBuffer[idx] = charBuffer[idx] - 0x10000;
                        *(characters++) = (UniChar)(charBuffer[idx] >> 10) + 0xD800UL;
                        *(characters++) = (UniChar)(charBuffer[idx] & 0x3FF) + 0xDC00UL;
                    }
                } else {
                    if (theUsedCharLen + 1 > maxCharLen) {
                        return processedByteLen;
                    }
                    ++theUsedCharLen;
                    *(characters++) = charBuffer[idx];
                }
            }
        } else {
            if (maxCharLen) {
                *(characters++) = character;
            }
            ++theUsedCharLen;
        }

        processedByteLen += usedLen;
        bytes += usedLen;
        numBytes -= usedLen;
    }
    *usedCharLen = theUsedCharLen;
    return processedByteLen;
}

CF_INLINE __CFConverterEntry* __CFConverterGetEntry(uint32_t encoding) {
    switch (encoding) {
        case kCFStringEncodingInvalidId:
        case kCFStringEncodingASCII:
            return &__CFConverterEntryASCII;

        case kCFStringEncodingISOLatin1:
            return &__CFConverterEntryISOLatin1;

        case kCFStringEncodingMacRoman:
            return &__CFConverterEntryMacRoman;

        case kCFStringEncodingWindowsLatin1:
            return &__CFConverterEntryWinLatin1;

        case kCFStringEncodingNextStepLatin:
            return &__CFConverterEntryNextStepLatin;

        case kCFStringEncodingUTF8:
            return &__CFConverterEntryUTF8;

        default:
            return NULL;
    }
}

CF_INLINE __CFConverter* __CFConverterFromDefinition(const _CFStringEncodingConverter* definition) {
#define NUM_OF_ENTRIES_CYCLE (10)
    static CFSpinLock_t _indexLock = CFSpinLockInit;
    static uint32_t _currentIndex = 0;
    static uint32_t _allocatedSize = 0;
    static __CFConverter* _allocatedEntries = NULL;
    __CFConverter* converter;

    CFSpinLock(&_indexLock);
    if ((_currentIndex + 1) >= _allocatedSize) {
        _currentIndex = 0;
        _allocatedSize = 0;
        _allocatedEntries = NULL;
    }
    if (_allocatedEntries == NULL) { // Not allocated yet
        _allocatedEntries = (__CFConverter*)CFAllocatorAllocate(
            kCFAllocatorSystemDefault,
            sizeof(__CFConverter) * NUM_OF_ENTRIES_CYCLE, 0);
        _allocatedSize = NUM_OF_ENTRIES_CYCLE;
        converter = &(_allocatedEntries[_currentIndex]);
    } else {
        converter = &(_allocatedEntries[++_currentIndex]);
    }
    CFSpinUnlock(&_indexLock);

    switch (definition->encodingClass) {
        case kCFStringEncodingConverterStandard:
            converter->toBytes = (_CFToBytesProc)definition->toBytes;
            converter->toUnicode = (_CFToUnicodeProc)definition->toUnicode;
            converter->toCanonicalUnicode = (_CFToUnicodeProc)definition->toUnicode;
            converter->_toBytes = NULL;
            converter->_toUnicode = NULL;
            converter->maxLen = 2;
            break;

        case kCFStringEncodingConverterCheapEightBit:
            converter->toBytes = __CFToBytesCheapEightBitWrapper;
            converter->toUnicode = __CFToUnicodeCheapEightBitWrapper;
            converter->toCanonicalUnicode = __CFToCanonicalUnicodeCheapEightBitWrapper;
            converter->_toBytes = definition->toBytes;
            converter->_toUnicode = definition->toUnicode;
            converter->maxLen = 1;
            break;

        case kCFStringEncodingConverterStandardEightBit:
            converter->toBytes = __CFToBytesStandardEightBitWrapper;
            converter->toUnicode = __CFToUnicodeStandardEightBitWrapper;
            converter->toCanonicalUnicode = __CFToCanonicalUnicodeStandardEightBitWrapper;
            converter->_toBytes = definition->toBytes;
            converter->_toUnicode = definition->toUnicode;
            converter->maxLen = definition->maxDecomposedCharLen;
            break;

        case kCFStringEncodingConverterCheapMultiByte:
            converter->toBytes = __CFToBytesCheapMultiByteWrapper;
            converter->toUnicode = __CFToUnicodeCheapMultiByteWrapper;
            converter->toCanonicalUnicode = __CFToCanonicalUnicodeCheapMultiByteWrapper;
            converter->_toBytes = definition->toBytes;
            converter->_toUnicode = definition->toUnicode;
            converter->maxLen = definition->maxBytesPerChar;
            break;

        case kCFStringEncodingConverterPlatformSpecific:
            converter->toBytes = NULL;
            converter->toUnicode = NULL;
            converter->toCanonicalUnicode = NULL;
            converter->_toBytes = NULL;
            converter->_toUnicode = NULL;
            converter->maxLen = 0;
            converter->toBytesLen = NULL;
            converter->toUnicodeLen = NULL;
            converter->toBytesFallback = NULL;
            converter->toUnicodeFallback = NULL;
            converter->toBytesPrecompose = NULL;
            converter->isValidCombiningChar = NULL;
            return converter;

        default: // Shouln't be here
            return NULL;
    }

    converter->toBytesLen = (definition->toBytesLen ? definition->toBytesLen : (CFStringEncodingToBytesLenProc)(uintptr_t)definition->maxBytesPerChar);
    converter->toUnicodeLen = (definition->toUnicodeLen ? definition->toUnicodeLen : (CFStringEncodingToUnicodeLenProc)(uintptr_t)definition->maxDecomposedCharLen);
    converter->toBytesFallback = (definition->toBytesFallback ? definition->toBytesFallback : __CFDefaultToBytesFallbackProc);
    converter->toUnicodeFallback = (definition->toUnicodeFallback ? definition->toUnicodeFallback : __CFDefaultToUnicodeFallbackProc);
    converter->toBytesPrecompose = (definition->toBytesPrecompose ? definition->toBytesPrecompose : NULL);
    converter->isValidCombiningChar = (definition->isValidCombiningChar ? definition->isValidCombiningChar : NULL);

    return converter;
}

static const __CFConverter* __CFGetConverter(uint32_t encoding) {
    __CFConverterEntry* entry = __CFConverterGetEntry(encoding);
    if (!entry) {
        return NULL;
    }
    if (!entry->converter) {
        const _CFStringEncodingConverter* definition = _CFStringEncodingGetConverter(entry->encoding);

        if (definition) {
            entry->converter = __CFConverterFromDefinition(definition);
            entry->toBytesFallback = definition->toBytesFallback;
            entry->toUnicodeFallback = definition->toUnicodeFallback;
        }
    }
    return (__CFConverter*)entry->converter;
}


///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL bool CFStringEncodingIsValidEncoding(uint32_t encoding) {
    return (_CFStringEncodingGetConverter(encoding) ? true : false);
}

CF_INTERNAL const char* CFStringEncodingName(uint32_t encoding) {
    __CFConverterEntry* entry = __CFConverterGetEntry(encoding);
    if (entry) {
        return entry->encodingName;
    }
    return NULL;
}

CF_INTERNAL const char** CFStringEncodingCanonicalCharsetNames(uint32_t encoding) {
    __CFConverterEntry* entry = __CFConverterGetEntry(encoding);
    if (entry) {
        return entry->ianaNames;
    }
    return NULL;
}

CF_INTERNAL uint32_t CFStringEncodingGetScriptCodeForEncoding(uint32_t encoding) {
    __CFConverterEntry* entry = __CFConverterGetEntry(encoding);
    if (entry) {
        return entry->scriptCode;
    }
    if ((encoding & 0x0FFF) == kCFStringEncodingUnicode) {
        return kCFStringEncodingUnicode;
    }
    if (encoding < 0xFF) {
        return encoding;
    }
    return kCFStringEncodingInvalidId;
}

CF_INTERNAL const uint32_t* CFStringEncodingListOfAvailableEncodings(void) {
    return __CFBuiltinEncodings;
}


/***** Conversion functions *****/

CF_INTERNAL CFIndex CFStringEncodingCharLengthForBytes(
    uint32_t encoding, uint32_t flags,
    const uint8_t* bytes, CFIndex numBytes)
{
    const __CFConverter* converter = __CFGetConverter(encoding);

    if (converter) {
        uintptr_t switchVal = (uintptr_t)(converter->toUnicodeLen);

        if (switchVal < 0xFFFF) {
            return switchVal * numBytes;
        } else {
            return converter->toUnicodeLen(flags, bytes, numBytes);
        }
    }

    return 0;
}

CF_INTERNAL CFIndex CFStringEncodingByteLengthForCharacters(
    uint32_t encoding, uint32_t flags,
    const UniChar* characters, CFIndex numChars)
{
    const __CFConverter* converter = __CFGetConverter(encoding);

    if (converter) {
        uintptr_t switchVal = (uintptr_t)(converter->toBytesLen);

        if (switchVal < 0xFFFF) {
            return switchVal * numChars;
        } else {
            return converter->toBytesLen(flags, characters, numChars);
        }
    }

    return 0;
}

CF_INTERNAL uint32_t CFStringEncodingUnicodeToBytes(
    uint32_t encoding, uint32_t flags,
    const UniChar* characters, CFIndex numChars, CFIndex* usedCharLen,
    uint8_t* bytes, CFIndex maxByteLen, CFIndex* usedByteLen)
{
    if (encoding == kCFStringEncodingUTF8) {
        static CFStringEncodingToBytesProc __CFToUTF8 = NULL;
        CFIndex convertedCharLen;
        CFIndex usedLen;

        if ((flags & kCFStringEncodingUseCanonical) || (flags & kCFStringEncodingUseHFSPlusCanonical)) {
            _CFUniCharDecompose(characters, numChars, &convertedCharLen, (void*)bytes, maxByteLen, &usedLen, true, kCFUniCharUTF8Format, (flags & kCFStringEncodingUseHFSPlusCanonical ? true : false));
        } else {
            if (!__CFToUTF8) {
                const _CFStringEncodingConverter* utf8Converter = _CFStringEncodingGetConverter(kCFStringEncodingUTF8);
                __CFToUTF8 = (CFStringEncodingToBytesProc)utf8Converter->toBytes;
            }
            convertedCharLen = __CFToUTF8(0, characters, numChars, bytes, maxByteLen, &usedLen);
        }
        if (usedCharLen) {
            *usedCharLen = convertedCharLen;
        }
        if (usedByteLen) {
            *usedByteLen = usedLen;
        }

        if (convertedCharLen == numChars) {
            return kCFStringEncodingConversionSuccess;
        } else if (maxByteLen && (maxByteLen == usedLen)) {
            return kCFStringEncodingInsufficientOutputBufferLength;
        } else {
            return kCFStringEncodingInvalidInputStream;
        }
    } else {
        const __CFConverter* converter = __CFGetConverter(encoding);
        CFIndex usedLen = 0;
        CFIndex localUsedByteLen;
        CFIndex theUsedByteLen = 0;
        uint32_t theResult = kCFStringEncodingConversionSuccess;
        CFStringEncodingToBytesPrecomposeProc toBytesPrecompose = NULL;
        CFStringEncodingIsValidCombiningCharacterProc isValidCombiningChar = NULL;

        if (!converter) {
            return kCFStringEncodingConverterUnavailable;
        }

        if (flags & kCFStringEncodingSubstituteCombinings) {
            if (!(flags & kCFStringEncodingAllowLossyConversion)) {
                isValidCombiningChar = converter->isValidCombiningChar;
            }
        } else {
            isValidCombiningChar = converter->isValidCombiningChar;
            if (!(flags & kCFStringEncodingIgnoreCombinings)) {
                toBytesPrecompose = converter->toBytesPrecompose;
                flags |= kCFStringEncodingComposeCombinings;
            }
        }

        while ((usedLen < numChars) && (!maxByteLen || (theUsedByteLen < maxByteLen))) {
            if ((usedLen += __CFConvertToBytes(converter, flags, characters + usedLen, numChars - usedLen, bytes + theUsedByteLen, (maxByteLen ? maxByteLen - theUsedByteLen : 0), &localUsedByteLen)) < numChars) {
                CFIndex dummy;

                if (isValidCombiningChar && (usedLen > 0) && isValidCombiningChar(characters[usedLen])) {
                    if (toBytesPrecompose) {
                        CFIndex localUsedLen = usedLen;

                        while (isValidCombiningChar(characters[--usedLen])) {
                            ;
                        }
                        theUsedByteLen += localUsedByteLen;
                        if (converter->maxLen > 1) {
                            __CFConvertToBytes(converter, flags, characters + usedLen, localUsedLen - usedLen, NULL, 0, &localUsedByteLen);
                            theUsedByteLen -= localUsedByteLen;
                        } else {
                            theUsedByteLen--;
                        }
                        if ((localUsedLen = toBytesPrecompose(flags, characters + usedLen, numChars - usedLen, bytes + theUsedByteLen, (maxByteLen ? maxByteLen - theUsedByteLen : 0), &localUsedByteLen)) > 0) {
                            usedLen += localUsedLen;
                            if ((usedLen < numChars) && isValidCombiningChar(characters[usedLen])) { // There is a non-base char not combined remaining
                                theUsedByteLen += localUsedByteLen;
                                theResult = kCFStringEncodingInvalidInputStream;
                                break;
                            }
                        } else if (flags & kCFStringEncodingAllowLossyConversion) {
                            uint8_t lossyByte = CFStringEncodingMaskToLossyByte(flags);

                            if (lossyByte) {
                                while (isValidCombiningChar(characters[++usedLen])) {
                                    ;
                                }
                                localUsedByteLen = 1;
                                if (maxByteLen) {
                                    *(bytes + theUsedByteLen) = lossyByte;
                                }
                            } else {
                                ++usedLen;
                                usedLen += converter->toBytesFallback(
                                    characters + usedLen, numChars - usedLen,
                                    bytes + theUsedByteLen, (maxByteLen ? maxByteLen - theUsedByteLen : 0),
                                    &localUsedByteLen);
                            }
                        } else {
                            theResult = kCFStringEncodingInvalidInputStream;
                            break;
                        }
                    } else if (maxByteLen && ((maxByteLen == theUsedByteLen + localUsedByteLen) || __CFConvertToBytes(converter, flags, characters + usedLen, numChars - usedLen, NULL, 0, &dummy))) { // buffer was filled up
                        theUsedByteLen += localUsedByteLen;
                        theResult = kCFStringEncodingInsufficientOutputBufferLength;
                        break;
                    } else if (flags & kCFStringEncodingIgnoreCombinings) {
                        while ((++usedLen < numChars) && isValidCombiningChar(characters[usedLen])) {
                            ;
                        }
                    } else {
                        uint8_t lossyByte = CFStringEncodingMaskToLossyByte(flags);

                        theUsedByteLen += localUsedByteLen;
                        if (lossyByte) {
                            ++usedLen;
                            localUsedByteLen = 1;
                            if (maxByteLen) {
                                *(bytes + theUsedByteLen) = lossyByte;
                            }
                        } else {
                            usedLen += converter->toBytesFallback(characters + usedLen, numChars - usedLen, bytes + theUsedByteLen, (maxByteLen ? maxByteLen - theUsedByteLen : 0), &localUsedByteLen);
                        }
                    }
                } else if (maxByteLen && ((maxByteLen == theUsedByteLen + localUsedByteLen) || __CFConvertToBytes(converter, flags, characters + usedLen, numChars - usedLen, NULL, 0, &dummy))) { // buffer was filled up
                    theUsedByteLen += localUsedByteLen;

                    if (flags & kCFStringEncodingAllowLossyConversion && !CFStringEncodingMaskToLossyByte(flags)) {
                        CFIndex localUsedLen;

                        localUsedByteLen = 0;
                        while ((usedLen < numChars) && !localUsedByteLen && (localUsedLen = converter->toBytesFallback(characters + usedLen, numChars - usedLen, NULL, 0, &localUsedByteLen))) {
                            usedLen += localUsedLen;
                        }
                    }
                    if (usedLen < numChars) {
                        theResult = kCFStringEncodingInsufficientOutputBufferLength;
                    }
                    break;
                } else if (flags & kCFStringEncodingAllowLossyConversion) {
                    uint8_t lossyByte = CFStringEncodingMaskToLossyByte(flags);

                    theUsedByteLen += localUsedByteLen;
                    if (lossyByte) {
                        ++usedLen;
                        localUsedByteLen = 1;
                        if (maxByteLen) {
                            *(bytes + theUsedByteLen) = lossyByte;
                        }
                    } else {
                        usedLen += converter->toBytesFallback(characters + usedLen, numChars - usedLen, bytes + theUsedByteLen, (maxByteLen ? maxByteLen - theUsedByteLen : 0), &localUsedByteLen);
                    }
                } else {
                    theUsedByteLen += localUsedByteLen;
                    theResult = kCFStringEncodingInvalidInputStream;
                    break;
                }
            }
            theUsedByteLen += localUsedByteLen;
        }

        if (usedLen < numChars && maxByteLen && theResult == kCFStringEncodingConversionSuccess) {
            if (flags & kCFStringEncodingAllowLossyConversion && !CFStringEncodingMaskToLossyByte(flags)) {
                CFIndex localUsedLen;

                localUsedByteLen = 0;
                while ((usedLen < numChars) && !localUsedByteLen && (localUsedLen = converter->toBytesFallback(characters + usedLen, numChars - usedLen, NULL, 0, &localUsedByteLen))) {
                    usedLen += localUsedLen;
                }
            }
            if (usedLen < numChars) {
                theResult = kCFStringEncodingInsufficientOutputBufferLength;
            }
        }
        if (usedByteLen) {
            *usedByteLen = theUsedByteLen;
        }
        if (usedCharLen) {
            *usedCharLen = usedLen;
        }

        return theResult;
    }
}

CF_INTERNAL uint32_t CFStringEncodingBytesToUnicode(
    uint32_t encoding, uint32_t flags,
    const uint8_t* bytes, CFIndex numBytes, CFIndex* usedByteLen,
    UniChar* characters, CFIndex maxCharLen, CFIndex* usedCharLen)
{
    const __CFConverter* converter = __CFGetConverter(encoding);
    CFIndex usedLen = 0;
    CFIndex theUsedCharLen = 0;
    CFIndex localUsedCharLen;
    uint32_t theResult = kCFStringEncodingConversionSuccess;

    if (!converter) {
        return kCFStringEncodingConverterUnavailable;
    }

    while ((usedLen < numBytes) && (!maxCharLen || (theUsedCharLen < maxCharLen))) {
        if ((usedLen += __CFConvertToUnicode(converter, flags, bytes + usedLen, numBytes - usedLen, characters + theUsedCharLen, (maxCharLen ? maxCharLen - theUsedCharLen : 0), &localUsedCharLen)) < numBytes) {
            CFIndex tempUsedCharLen;

            if (maxCharLen && (
                    (maxCharLen == theUsedCharLen + localUsedCharLen) || (
                        ((flags & (kCFStringEncodingUseCanonical | kCFStringEncodingUseHFSPlusCanonical)) ||
                            (maxCharLen == theUsedCharLen + localUsedCharLen + 1)) &&
                        __CFConvertToUnicode(converter, flags, bytes + usedLen, numBytes - usedLen, NULL, 0, &tempUsedCharLen))
                    )
                )
            {
                // buffer was filled up
                theUsedCharLen += localUsedCharLen;
                theResult = kCFStringEncodingInsufficientOutputBufferLength;
                break;
            } else if (flags & kCFStringEncodingAllowLossyConversion) {
                theUsedCharLen += localUsedCharLen;
                usedLen += converter->toUnicodeFallback(
                    bytes + usedLen, numBytes - usedLen,
                    characters + theUsedCharLen, (maxCharLen ? maxCharLen - theUsedCharLen : 0),
                    &localUsedCharLen);
            } else {
                theUsedCharLen += localUsedCharLen;
                theResult = kCFStringEncodingInvalidInputStream;
                break;
            }
        }
        theUsedCharLen += localUsedCharLen;
    }

    if (usedLen < numBytes && maxCharLen && theResult == kCFStringEncodingConversionSuccess) {
        theResult = kCFStringEncodingInsufficientOutputBufferLength;
    }
    if (usedCharLen) {
        *usedCharLen = theUsedCharLen;
    }
    if (usedByteLen) {
        *usedByteLen = usedLen;
    }

    return theResult;
}
