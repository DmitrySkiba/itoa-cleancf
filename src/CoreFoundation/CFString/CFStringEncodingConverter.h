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

#if !defined(__COREFOUNDATION_CFSTRINGENCODINGCONVERTER__)
#define __COREFOUNDATION_CFSTRINGENCODINGCONVERTER__ 1

#include <CoreFoundation/CFString.h>

CF_EXTERN_C_BEGIN

enum {
    kCFStringEncodingConverterStandard = 0,
    kCFStringEncodingConverterCheapEightBit = 1,
    kCFStringEncodingConverterStandardEightBit = 2,
    kCFStringEncodingConverterCheapMultiByte = 3,
    kCFStringEncodingConverterPlatformSpecific = 4 // Other fields are ignored
};

/* kCFStringEncodingConverterStandard */
typedef CFIndex (*CFStringEncodingToBytesProc)(
    uint32_t flags,
    const UniChar* characters, CFIndex numChars,
    uint8_t* bytes, CFIndex maxByteLen, CFIndex* usedByteLen);

typedef CFIndex (*CFStringEncodingToUnicodeProc)(
    uint32_t flags,
    const uint8_t* bytes, CFIndex numBytes,
    UniChar* characters, CFIndex maxCharLen, CFIndex* usedCharLen);


/* kCFStringEncodingConverterCheapEightBit */
typedef bool (*CFStringEncodingCheapEightBitToBytesProc)(
    uint32_t flags, UniChar character, uint8_t *byte);

typedef bool (*CFStringEncodingCheapEightBitToUnicodeProc)(
    uint32_t flags, uint8_t byte, UniChar *character);


/* kCFStringEncodingConverterStandardEightBit */
typedef uint16_t (*CFStringEncodingStandardEightBitToBytesProc)(
    uint32_t flags, const UniChar *characters, CFIndex numChars, uint8_t *byte);

typedef uint16_t (*CFStringEncodingStandardEightBitToUnicodeProc)(
    uint32_t flags, uint8_t byte, UniChar *characters);


/* kCFStringEncodingConverterCheapMultiByte */
typedef uint16_t (*CFStringEncodingCheapMultiByteToBytesProc)(
    uint32_t flags, UniChar character, uint8_t *bytes);

typedef uint16_t (*CFStringEncodingCheapMultiByteToUnicodeProc)(
    uint32_t flags, const uint8_t *bytes, CFIndex numBytes, UniChar *character);


typedef CFIndex (*CFStringEncodingToBytesLenProc)(
    uint32_t flags, const UniChar *characters, CFIndex numChars);

typedef CFIndex (*CFStringEncodingToUnicodeLenProc)(
    uint32_t flags, const uint8_t *bytes, CFIndex numBytes);

typedef CFIndex (*CFStringEncodingToBytesPrecomposeProc)(
    uint32_t flags,
    const UniChar *character, CFIndex numChars,
    uint8_t *bytes, CFIndex maxByteLen, CFIndex *usedByteLen);

typedef bool (*CFStringEncodingIsValidCombiningCharacterProc)(
    UniChar character);


/* Fallback functions used when allowLossy
 */
typedef CFIndex (*CFStringEncodingToBytesFallbackProc)(
    const UniChar *characters, CFIndex numChars,
    uint8_t *bytes, CFIndex maxByteLen, CFIndex *usedByteLen);

typedef CFIndex (*CFStringEncodingToUnicodeFallbackProc)(
    const uint8_t *bytes, CFIndex numBytes,
    UniChar *characters, CFIndex maxCharLen, CFIndex *usedCharLen);


typedef struct {
    void *toBytes;
    void *toUnicode;
    uint16_t maxBytesPerChar;
    uint16_t maxDecomposedCharLen;
    uint8_t encodingClass;
    uint32_t :24;
    CFStringEncodingToBytesLenProc toBytesLen;
    CFStringEncodingToUnicodeLenProc toUnicodeLen;
    CFStringEncodingToBytesFallbackProc toBytesFallback;
    CFStringEncodingToUnicodeFallbackProc toUnicodeFallback;
    CFStringEncodingToBytesPrecomposeProc toBytesPrecompose;
    CFStringEncodingIsValidCombiningCharacterProc isValidCombiningChar;
} _CFStringEncodingConverter;


CF_EXPORT const _CFStringEncodingConverter* _CFStringEncodingGetConverter(uint32_t encoding);

CF_EXPORT const _CFStringEncodingConverter _CFEncodingConverterASCII;
CF_EXPORT const _CFStringEncodingConverter _CFEncodingConverterISOLatin1;
CF_EXPORT const _CFStringEncodingConverter _CFEncodingConverterMacRoman;
CF_EXPORT const _CFStringEncodingConverter _CFEncodingConverterWinLatin1;
CF_EXPORT const _CFStringEncodingConverter _CFEncodingConverterNextStepLatin;
CF_EXPORT const _CFStringEncodingConverter _CFEncodingConverterUTF8;

CF_EXTERN_C_END

#endif /* ! __COREFOUNDATION_CFSTRINGENCODINGCONVERTER__ */
