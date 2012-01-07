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
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFByteOrder.h>
#include "CFStringInternal.h"
#include <string.h>
#include "CFStringEncoding.h"
#include "CFStringEncodingConverter.h"
#include "CFUniChar.h"
#include "CFString_Common.h"

/* The minimum length the output buffers should be in the above functions
 */
#define kCFCharConversionBufferLength 512

enum {
    __NSNonLossyErrorMode = -1,
    __NSNonLossyASCIIMode = 0,
    __NSNonLossyBackslashMode = 1,
    __NSNonLossyHexInitialMode = __NSNonLossyBackslashMode + 1,
    __NSNonLossyHexFinalMode = __NSNonLossyHexInitialMode + 4,
    __NSNonLossyOctalInitialMode = __NSNonLossyHexFinalMode + 1,
    __NSNonLossyOctalFinalMode = __NSNonLossyHexFinalMode + 3
};

///////////////////////////////////////////////////////////////////// internal

/* Convert a byte stream to ASCII (7-bit!) or Unicode, with a CFVarWidthCharBuffer struct on the stack.
 * false return indicates an error occured during the conversion.
 * The caller needs to free the returned buffer in either ascii or unicode (indicated by isASCII),
 *  if shouldFreeChars is true.
 *
 * 9/18/98 __CFStringDecodeByteStream now avoids to allocate buffer if buffer->chars is not NULL
 *
 * Added useClientsMemoryPtr; if not-NULL, and the provided memory can be used as is,
 *  this is set to true.
 *
 * !!! converterFlags is only used for the UTF8 converter at this point
 */
CF_INTERNAL
Boolean __CFStringDecodeByteStream(const uint8_t* bytes, CFIndex len,
								   CFStringEncoding encoding,
								   Boolean alwaysUnicode,
								   CFVarWidthCharBuffer* buffer,
								   Boolean* useClientsMemoryPtr,
								   UInt32 converterFlags)
{

    CFIndex MaxLocalChars = sizeof(buffer->localBuffer);
    CFIndex MaxLocalUniChars = sizeof(buffer->localBuffer) / sizeof(UniChar);

    if (!len) {
        return true;
    }

    if (useClientsMemoryPtr) {
        *useClientsMemoryPtr = false;
    }

    buffer->isASCII = !alwaysUnicode;
    buffer->shouldFreeChars = false;
    buffer->numChars = 0;
    if (!buffer->allocator) {
        buffer->allocator = CFAllocatorGetDefault();
    }

    if ((encoding == kCFStringEncodingUTF16) || 
        (encoding == kCFStringEncodingUTF16BE) || 
        (encoding == kCFStringEncodingUTF16LE))
    {
        // UTF-16
        const UTF16Char* src = (const UTF16Char*)bytes;
        const UTF16Char* limit = (const UTF16Char*)(bytes + len);
        bool swap = false;

        if (kCFStringEncodingUTF16 == encoding) {
            UTF16Char bom = ((*src == 0xFFFE) || (*src == 0xFEFF) ? *(src++) : 0);

#if __CF_BIG_ENDIAN__
            if (bom == 0xFFFE) {
                swap = true;
            }
#else
            if (bom != 0xFEFF) {
                swap = true;
            }
#endif
            if (bom) {
                useClientsMemoryPtr = NULL;
            }
        } else {
#if __CF_BIG_ENDIAN__
            if (kCFStringEncodingUTF16LE == encoding) {
                swap = true;
            }
#else
            if (kCFStringEncodingUTF16BE == encoding) {
                swap = true;
            }
#endif
        }

        buffer->numChars = limit - src;

        if (useClientsMemoryPtr && !swap) { // If the caller is ready to deal with no-copy situation, and the situation is possible, indicate it...
            *useClientsMemoryPtr = true;
            buffer->chars.unicode = (UniChar*)src;
            buffer->isASCII = false;
        } else {
            if (buffer->isASCII) {    // Let's see if we can reduce the Unicode down to ASCII...
                const UTF16Char* characters = src;
                UTF16Char mask = (swap ? 0x80FF : 0xFF80);

                while (characters < limit) {
                    if (*(characters++) & mask) {
                        buffer->isASCII = false;
                        break;
                    }
                }
            }

            if (buffer->isASCII) {
                uint8_t* dst;
                if (!buffer->chars.ascii) { // we never reallocate when buffer is supplied
                    if (buffer->numChars > MaxLocalChars) {
                        buffer->chars.ascii = (UInt8*)CFAllocatorAllocate(buffer->allocator, (buffer->numChars * sizeof(uint8_t)), 0);
                        buffer->shouldFreeChars = true;
                    } else {
                        buffer->chars.ascii = (uint8_t*)buffer->localBuffer;
                    }
                }
                dst = buffer->chars.ascii;

                if (swap) {
                    while (src < limit) {
                        *(dst++) = (*(src++) >> 8);
                    }
                } else {
                    while (src < limit) {
                        *(dst++) = (uint8_t)*(src++);
                    }
                }
            } else {
                UTF16Char* dst;

                if (!buffer->chars.unicode) { // we never reallocate when buffer is supplied
                    if (buffer->numChars > MaxLocalUniChars) {
                        buffer->chars.unicode = (UniChar*)CFAllocatorAllocate(buffer->allocator, (buffer->numChars * sizeof(UTF16Char)), 0);
                        buffer->shouldFreeChars = true;
                    } else {
                        buffer->chars.unicode = (UTF16Char*)buffer->localBuffer;
                    }
                }
                dst = buffer->chars.unicode;

                if (swap) {
                    while (src < limit) {
                        *(dst++) = CFSwapInt16(*(src++));
                    }
                } else {
                    memmove(dst, src, buffer->numChars * sizeof(UTF16Char));
                }
            }
        }
    } else if (
        (encoding == kCFStringEncodingUTF32) ||
        (encoding == kCFStringEncodingUTF32BE) || 
        (encoding == kCFStringEncodingUTF32LE))
    {
        const UTF32Char* src = (const UTF32Char*)bytes;
        const UTF32Char* limit = (const UTF32Char*)(bytes + len);
        bool swap = false;
        static bool strictUTF32 = (bool) - 1;

        if ((bool) - 1 == strictUTF32) {
            strictUTF32 = true;
        }

        if (kCFStringEncodingUTF32 == encoding) {
            UTF32Char bom = ((*src == 0xFFFE0000) || (*src == 0x0000FEFF) ? *(src++) : 0);

#if __CF_BIG_ENDIAN__
            if (bom == 0xFFFE0000) {
                swap = true;
            }
#else
            if (bom != 0x0000FEFF) {
                swap = true;
            }
#endif
        } else {
#if __CF_BIG_ENDIAN__
            if (kCFStringEncodingUTF32LE == encoding) {
                swap = true;
            }
#else
            if (kCFStringEncodingUTF32BE == encoding) {
                swap = true;
            }
#endif
        }

        buffer->numChars = limit - src;

        {
            // Let's see if we have non-ASCII or non-BMP
            const UTF32Char* characters = src;
            UTF32Char asciiMask = (swap ? 0x80FFFFFF : 0xFFFFFF80);
            UTF32Char bmpMask = (swap ? 0x0000FFFF : 0xFFFF0000);

            while (characters < limit) {
                if (*characters & asciiMask) {
                    buffer->isASCII = false;
                    if (*characters & bmpMask) {
                        if (strictUTF32 && ((swap ? (UTF32Char)CFSwapInt32(*characters) : *characters) > 0x10FFFF)) {
                            return false; // outside of Unicode Scaler Value
                        }
                        ++(buffer->numChars);
                    }
                }
                ++characters;
            }
        }

        if (buffer->isASCII) {
            uint8_t* dst;
            if (!buffer->chars.ascii) {
                // We never reallocate when buffer is supplied.
                if (buffer->numChars > MaxLocalChars) {
                    buffer->chars.ascii = (UInt8*)CFAllocatorAllocate(
                        buffer->allocator,
                        buffer->numChars, 0);
                    buffer->shouldFreeChars = true;
                } else {
                    buffer->chars.ascii = (uint8_t*)buffer->localBuffer;
                }
            }
            dst = buffer->chars.ascii;

            if (swap) {
                while (src < limit) {
                    *(dst++) = (*(src++) >> 24);
                }
            } else {
                while (src < limit) {
                    *(dst++) = *(src++);
                }
            }
        } else {
            if (!buffer->chars.unicode) {
                // We never reallocate when buffer is supplied.
                if (buffer->numChars > MaxLocalUniChars) {
                    buffer->chars.unicode = (UniChar*)CFAllocatorAllocate(
                        buffer->allocator,
                        (buffer->numChars * sizeof(UniChar)), 0);
                    buffer->shouldFreeChars = true;
                } else {
                    buffer->chars.unicode = (UniChar*)buffer->localBuffer;
                }
            }
            return (_CFUniCharFromUTF32(src, limit - src, buffer->chars.unicode, !strictUTF32, __CF_BIG_ENDIAN__ ? !swap : swap) ? TRUE : FALSE);
        }
    } else {
        CFIndex idx;
        const uint8_t* chars = (const uint8_t*)bytes;
        const uint8_t* end = chars + len;

        switch (encoding) {
            case kCFStringEncodingNonLossyASCII: {
                UTF16Char currentValue = 0;
                uint8_t character;
                int8_t mode = __NSNonLossyASCIIMode;

                buffer->isASCII = false;
                buffer->shouldFreeChars = !buffer->chars.unicode && (len <= MaxLocalUniChars) ? false : true;
                buffer->chars.unicode = (buffer->chars.unicode ? buffer->chars.unicode : (len <= MaxLocalUniChars) ? (UniChar*)buffer->localBuffer : (UniChar*)CFAllocatorAllocate(buffer->allocator, len * sizeof(UniChar), 0));
                buffer->numChars = 0;

                while (chars < end) {
                    character = (*chars++);

                    switch (mode) {
                        case __NSNonLossyASCIIMode:
                            if (character == '\\') {
                                mode = __NSNonLossyBackslashMode;
                            } else if (character < 0x80) {
                                currentValue = character;
                            } else {
                                mode = __NSNonLossyErrorMode;
                            }
                            break;

                        case __NSNonLossyBackslashMode:
                            if ((character == 'U') || (character == 'u')) {
                                mode = __NSNonLossyHexInitialMode;
                                currentValue = 0;
                            } else if ((character >= '0') && (character <= '9')) {
                                mode = __NSNonLossyOctalInitialMode;
                                currentValue = character - '0';
                            } else if (character == '\\') {
                                mode = __NSNonLossyASCIIMode;
                                currentValue = character;
                            } else {
                                mode = __NSNonLossyErrorMode;
                            }
                            break;

                        default:
                            if (mode < __NSNonLossyHexFinalMode) {
                                if ((character >= '0') && (character <= '9')) {
                                    currentValue = (currentValue << 4) | (character - '0');
                                    if (++mode == __NSNonLossyHexFinalMode) {
                                        mode = __NSNonLossyASCIIMode;
                                    }
                                } else {
                                    if (character >= 'a') {
                                        character -= ('a' - 'A');
                                    }
                                    if ((character >= 'A') && (character <= 'F')) {
                                        currentValue = (currentValue << 4) | ((character - 'A') + 10);
                                        if (++mode == __NSNonLossyHexFinalMode) {
                                            mode = __NSNonLossyASCIIMode;
                                        }
                                    } else {
                                        mode = __NSNonLossyErrorMode;
                                    }
                                }
                            } else {
                                if ((character >= '0') && (character <= '9')) {
                                    currentValue = (currentValue << 3) | (character - '0');
                                    if (++mode == __NSNonLossyOctalFinalMode) {
                                        mode = __NSNonLossyASCIIMode;
                                    }
                                } else {
                                    mode = __NSNonLossyErrorMode;
                                }
                            }
                            break;
                    }

                    if (mode == __NSNonLossyASCIIMode) {
                        buffer->chars.unicode[buffer->numChars++] = currentValue;
                    } else if (mode == __NSNonLossyErrorMode) {
                        return false;
                    }
                }
                return (mode == __NSNonLossyASCIIMode);
            }

            case kCFStringEncodingUTF8:
                if ((len >= 3) && (chars[0] == 0xef) && (chars[1] == 0xbb) && (chars[2] == 0xbf)) { // If UTF8 BOM, skip
                    chars += 3;
                    len -= 3;
                    if (!len) {
                        return true;
                    }
                }
                if (buffer->isASCII) {
                    for (idx = 0; idx < len; idx++) {
                        if (128 <= chars[idx]) {
                            buffer->isASCII = false;
                            break;
                        }
                    }
                }
                if (buffer->isASCII) {
                    buffer->numChars = len;
                    buffer->shouldFreeChars = !buffer->chars.ascii && (len <= MaxLocalChars) ? false : true;
                    buffer->chars.ascii = (buffer->chars.ascii ? buffer->chars.ascii : (len <= MaxLocalChars) ? (uint8_t*)buffer->localBuffer : (UInt8*)CFAllocatorAllocate(buffer->allocator, len * sizeof(uint8_t), 0));
                    memmove(buffer->chars.ascii, chars, len * sizeof(uint8_t));
                } else {
                    CFIndex numDone;
                    static CFStringEncodingToUnicodeProc __CFFromUTF8 = NULL;

                    if (!__CFFromUTF8) {
                        const _CFStringEncodingConverter* converter = _CFStringEncodingGetConverter(kCFStringEncodingUTF8);
                        __CFFromUTF8 = (CFStringEncodingToUnicodeProc)converter->toUnicode;
                    }

                    buffer->shouldFreeChars = !buffer->chars.unicode && (len <= MaxLocalUniChars) ? false : true;
                    buffer->chars.unicode = (buffer->chars.unicode ? buffer->chars.unicode : (len <= MaxLocalUniChars) ? (UniChar*)buffer->localBuffer : (UniChar*)CFAllocatorAllocate(buffer->allocator, len * sizeof(UniChar), 0));
                    buffer->numChars = 0;
                    while (chars < end) {
                        numDone = 0;
                        chars += __CFFromUTF8(converterFlags, chars, end - chars, &(buffer->chars.unicode[buffer->numChars]), len - buffer->numChars, &numDone);

                        if (!numDone) {
                            if (buffer->shouldFreeChars) {
                                CFAllocatorDeallocate(buffer->allocator, buffer->chars.unicode);
                            }
                            buffer->isASCII = !alwaysUnicode;
                            buffer->shouldFreeChars = false;
                            buffer->chars.ascii = NULL;
                            buffer->numChars = 0;
                            return false;
                        }
                        buffer->numChars += numDone;
                    }
                }
                break;

            default:
                if (CFStringEncodingIsValidEncoding(encoding)) {
                    const _CFStringEncodingConverter* converter = _CFStringEncodingGetConverter(encoding);
                    Boolean isASCIISuperset = __CFStringEncodingIsSupersetOfASCII(encoding);

                    if (!converter) {
                        return false;
                    }

                    if (!isASCIISuperset) {
                        buffer->isASCII = false;
                    }

                    if (buffer->isASCII) {
                        for (idx = 0; idx < len; idx++) {
                            if (128 <= chars[idx]) {
                                buffer->isASCII = false;
                                break;
                            }
                        }
                    }

                    if (converter->encodingClass == kCFStringEncodingConverterCheapEightBit) {
                        if (buffer->isASCII) {
                            buffer->numChars = len;
                            buffer->shouldFreeChars = !buffer->chars.ascii && (len <= MaxLocalChars) ? false : true;
                            buffer->chars.ascii = (buffer->chars.ascii ? buffer->chars.ascii : (len <= MaxLocalChars) ? (uint8_t*)buffer->localBuffer : (UInt8*)CFAllocatorAllocate(buffer->allocator, len * sizeof(uint8_t), 0));
                            memmove(buffer->chars.ascii, chars, len * sizeof(uint8_t));
                        } else {
                            buffer->shouldFreeChars = !buffer->chars.unicode && (len <= MaxLocalUniChars) ? false : true;
                            buffer->chars.unicode = (buffer->chars.unicode ? buffer->chars.unicode : (len <= MaxLocalUniChars) ? (UniChar*)buffer->localBuffer : (UniChar*)CFAllocatorAllocate(buffer->allocator, len * sizeof(UniChar), 0));
                            buffer->numChars = len;
                            if (kCFStringEncodingASCII == encoding || kCFStringEncodingISOLatin1 == encoding) {
                                for (idx = 0; idx < len; idx++) {
                                    buffer->chars.unicode[idx] = (UniChar)chars[idx];
                                }
                            } else {
                                for (idx = 0; idx < len; idx++) {
                                    if (chars[idx] < 0x80 && isASCIISuperset) {
                                        buffer->chars.unicode[idx] = (UniChar)chars[idx];
                                    } else if (!((CFStringEncodingCheapEightBitToUnicodeProc)converter->toUnicode)(0, chars[idx], buffer->chars.unicode + idx)) {
                                        return false;
                                    }
                                }
                            }
                        }
                    } else {
                        if (buffer->isASCII) {
                            buffer->numChars = len;
                            buffer->shouldFreeChars = !buffer->chars.ascii && (len <= MaxLocalChars) ? false : true;
                            buffer->chars.ascii = (buffer->chars.ascii ? buffer->chars.ascii : (len <= MaxLocalChars) ? (uint8_t*)buffer->localBuffer : (UInt8*)CFAllocatorAllocate(buffer->allocator, len * sizeof(uint8_t), 0));
                            memmove(buffer->chars.ascii, chars, len * sizeof(uint8_t));
                        } else {
                            CFIndex guessedLength = CFStringEncodingCharLengthForBytes(encoding, 0, bytes, len);
                            static UInt32 lossyFlag = (UInt32) - 1;

                            buffer->shouldFreeChars = !buffer->chars.unicode && (guessedLength <= MaxLocalUniChars) ? false : true;
                            buffer->chars.unicode = (buffer->chars.unicode ? buffer->chars.unicode : (guessedLength <= MaxLocalUniChars) ? (UniChar*)buffer->localBuffer : (UniChar*)CFAllocatorAllocate(buffer->allocator, guessedLength * sizeof(UniChar), 0));

                            if (lossyFlag == (UInt32) - 1) {
                                lossyFlag = 0;
                            }

                            if (CFStringEncodingBytesToUnicode(encoding, lossyFlag, bytes, len, NULL, buffer->chars.unicode, (guessedLength > MaxLocalUniChars ? guessedLength : MaxLocalUniChars), &(buffer->numChars))) {
                                if (buffer->shouldFreeChars) {
                                    CFAllocatorDeallocate(buffer->allocator, buffer->chars.unicode);
                                }
                                buffer->isASCII = !alwaysUnicode;
                                buffer->shouldFreeChars = false;
                                buffer->chars.ascii = NULL;
                                buffer->numChars = 0;
                                return false;
                            }
                        }
                    }
                } else {
                    return false;
                }
        }
    }

    return true;
}

///////////////////////////////////////////////////////////////////// public

/* Create a byte stream from a CFString backing. Can convert a string piece at a time
 *  into a fixed size buffer. Returns number of characters converted.
 * Characters that cannot be converted to the specified encoding are represented
 *  with the char specified by lossByte; if 0, then lossy conversion is not allowed
 *  and conversion stops, returning partial results.
 * Pass buffer==NULL if you don't care about the converted string (but just the 
 *  convertability, or number of bytes required, indicated by usedBufLen).
 * Does not zero-terminate. If you want to create Pascal or C string, allow one extra 
 *  byte at start or end.
 *
 * Note: This function is intended to work through CFString functions, so it should work
 * with NSStrings as well as CFStrings.
 */
CFIndex __CFStringEncodeByteStream(CFStringRef string, CFIndex rangeLoc, CFIndex rangeLen,
                                   Boolean generatingExternalFile,
                                   CFStringEncoding encoding,
                                   char lossByte,
                                   uint8_t* buffer, CFIndex max, CFIndex* usedBufLen)
{
    CFIndex totalBytesWritten = 0;    /* Number of written bytes */
    CFIndex numCharsProcessed = 0;    /* Number of processed chars */
    const UniChar* unichars;

    if (encoding == kCFStringEncodingUTF8 && (unichars = CFStringGetCharactersPtr(string))) {
        static CFStringEncodingToBytesProc __CFToUTF8 = NULL;

        if (!__CFToUTF8) {
            const _CFStringEncodingConverter* utf8Converter = _CFStringEncodingGetConverter(kCFStringEncodingUTF8);
            __CFToUTF8 = (CFStringEncodingToBytesProc)utf8Converter->toBytes;
        }
        numCharsProcessed = __CFToUTF8(
            (generatingExternalFile ? kCFStringEncodingPrependBOM : 0),
            unichars + rangeLoc, rangeLen,
            buffer, (buffer ? max : 0), &totalBytesWritten);

    } else if (encoding == kCFStringEncodingNonLossyASCII) {
        const char* hex = "0123456789abcdef";
        UniChar ch;
        CFStringInlineBuffer buf;
        CFStringInitInlineBuffer(string, &buf, CFRangeMake(rangeLoc, rangeLen));
        while (numCharsProcessed < rangeLen) {
            CFIndex reqLength; /* Required number of chars to encode this UniChar */
            CFIndex cnt;
            char tmp[6];
            ch = CFStringGetCharacterFromInlineBuffer(&buf, numCharsProcessed);
            if ((ch >= ' ' && ch <= '~' && ch != '\\') || (ch == '\n' || ch == '\r' || ch == '\t')) {
                reqLength = 1;
                tmp[0] = (char)ch;
            } else {
                if (ch == '\\') {
                    tmp[1] = '\\';
                    reqLength = 2;
                } else if (ch < 256) { /* \nnn; note that this is not NEXTSTEP encoding but a (small) UniChar */
                    tmp[1] = '0' + (ch >> 6);
                    tmp[2] = '0' + ((ch >> 3) & 7);
                    tmp[3] = '0' + (ch & 7);
                    reqLength = 4;
                } else { /* \Unnnn */
                    tmp[1] = 'u'; // Changed to small+u in order to be aligned with Java
                    tmp[2] = hex[(ch >> 12) & 0x0f];
                    tmp[3] = hex[(ch >> 8) & 0x0f];
                    tmp[4] = hex[(ch >> 4) & 0x0f];
                    tmp[5] = hex[ch & 0x0f];
                    reqLength = 6;
                }
                tmp[0] = '\\';
            }
            if (buffer) {
                if (totalBytesWritten + reqLength > max) {
                    break; // Doesn't fit...
                }
                for (cnt = 0; cnt < reqLength; cnt++) {
                    buffer[totalBytesWritten + cnt] = tmp[cnt];
                }
            }
            totalBytesWritten += reqLength;
            numCharsProcessed++;
        }
    } else if (
        (encoding == kCFStringEncodingUTF16) || 
        (encoding == kCFStringEncodingUTF16BE) || 
        (encoding == kCFStringEncodingUTF16LE))
    {
        CFIndex extraForBOM = (generatingExternalFile && (encoding == kCFStringEncodingUTF16) ? sizeof(UniChar) : 0);
        numCharsProcessed = rangeLen;
        if (buffer && (numCharsProcessed * (CFIndex)sizeof(UniChar) + extraForBOM > max)) {
            numCharsProcessed = (max > extraForBOM) ? ((max - extraForBOM) / sizeof(UniChar)) : 0;
        }
        totalBytesWritten = (numCharsProcessed * sizeof(UniChar)) + extraForBOM;
        if (buffer) {
            if (extraForBOM) { /* Generate BOM */
#if __CF_BIG_ENDIAN__
                *buffer++ = 0xfe; *buffer++ = 0xff;
#else
                *buffer++ = 0xff; *buffer++ = 0xfe;
#endif
            }
            CFStringGetCharacters(string, CFRangeMake(rangeLoc, numCharsProcessed), (UniChar*)buffer);
            if ((__CF_BIG_ENDIAN__ ?  kCFStringEncodingUTF16LE : kCFStringEncodingUTF16BE) == encoding) { // Need to swap
                UTF16Char* characters = (UTF16Char*)buffer;
                const UTF16Char* limit = characters + numCharsProcessed;

                while (characters < limit) {
                    *characters = CFSwapInt16(*characters);
                    ++characters;
                }
            }
        }
    } else if (
        (encoding == kCFStringEncodingUTF32) || 
        (encoding == kCFStringEncodingUTF32BE) || 
        (encoding == kCFStringEncodingUTF32LE))
    {
        UTF32Char character;
        CFStringInlineBuffer buf;
        UTF32Char* characters = (UTF32Char*)buffer;

        bool swap = (encoding == (__CF_BIG_ENDIAN__ ? kCFStringEncodingUTF32LE : kCFStringEncodingUTF32BE));
        if (generatingExternalFile && (encoding == kCFStringEncodingUTF32)) {
            totalBytesWritten += sizeof(UTF32Char);
            if (characters) {
                if (totalBytesWritten > max) { // insufficient buffer
                    totalBytesWritten = 0;
                } else {
                    *(characters++) = 0x0000FEFF;
                }
            }
        }

        CFStringInitInlineBuffer(string, &buf, CFRangeMake(rangeLoc, rangeLen));
        while (numCharsProcessed < rangeLen) {
            character = CFStringGetCharacterFromInlineBuffer(&buf, numCharsProcessed);

            if (_CFUniCharIsSurrogateHighCharacter(character)) {
                UTF16Char otherCharacter;

                if (((numCharsProcessed + 1) < rangeLen) &&
                    _CFUniCharIsSurrogateLowCharacter(
                        (otherCharacter = CFStringGetCharacterFromInlineBuffer(&buf, numCharsProcessed + 1))))
                {
                    character = _CFUniCharGetLongCharacterForSurrogatePair(character, otherCharacter);
                } else if (lossByte) {
                    character = lossByte;
                } else {
                    break;
                }
            } else if (_CFUniCharIsSurrogateLowCharacter(character)) {
                if (lossByte) {
                    character = lossByte;
                } else {
                    break;
                }
            }

            totalBytesWritten += sizeof(UTF32Char);

            if (characters) {
                if (totalBytesWritten > max) {
                    totalBytesWritten -= sizeof(UTF32Char);
                    break;
                }
                *(characters++) = (swap ? CFSwapInt32(character) : character);
            }

            numCharsProcessed += (character > 0xFFFF ? 2 : 1);
        }
    } else {
        CFIndex numChars;
        UInt32 flags;
        const unsigned char* cString = NULL;
        Boolean isASCIISuperset = __CFStringEncodingIsSupersetOfASCII(encoding);

        if (!CF_IS_OBJC(string) && isASCIISuperset) { // Checking for NSString to avoid infinite recursion
            const unsigned char* ptr;
            if ((cString = (const unsigned char*)CFStringGetCStringPtr(string, __CFStringGetEightBitStringEncoding()))) {
                ptr = (cString += rangeLoc);
                if (__CFStringGetEightBitStringEncoding() == encoding) {
                    numCharsProcessed = (rangeLen < max || buffer == NULL ? rangeLen : max);
                    if (buffer) {
                        memmove(buffer, cString, numCharsProcessed);
                    }
                    if (usedBufLen) {
                        *usedBufLen = numCharsProcessed;
                    }
                    return numCharsProcessed;
                }
                while (*ptr < 0x80 && rangeLen > 0) {
                    ++ptr;
                    --rangeLen;
                }
                numCharsProcessed = ptr - cString;
                if (buffer) {
                    numCharsProcessed = (numCharsProcessed < max ? numCharsProcessed : max);
                    memmove(buffer, cString, numCharsProcessed);
                    buffer += numCharsProcessed;
                    max -= numCharsProcessed;
                }
                if (!rangeLen || (buffer && (max == 0))) {
                    if (usedBufLen) {
                        *usedBufLen = numCharsProcessed;
                    }
                    return numCharsProcessed;
                }
                rangeLoc += numCharsProcessed;
                totalBytesWritten += numCharsProcessed;
            }
            if (!cString && (cString = CFStringGetPascalStringPtr(string, __CFStringGetEightBitStringEncoding()))) {
                ptr = (cString += (rangeLoc + 1));
                if (__CFStringGetEightBitStringEncoding() == encoding) {
                    numCharsProcessed = (rangeLen < max || buffer == NULL ? rangeLen : max);
                    if (buffer) {
                        memmove(buffer, cString, numCharsProcessed);
                    }
                    if (usedBufLen) {
                        *usedBufLen = numCharsProcessed;
                    }
                    return numCharsProcessed;
                }
                while (*ptr < 0x80 && rangeLen > 0) {
                    ++ptr;
                    --rangeLen;
                }
                numCharsProcessed = ptr - cString;
                if (buffer) {
                    numCharsProcessed = (numCharsProcessed < max ? numCharsProcessed : max);
                    memmove(buffer, cString, numCharsProcessed);
                    buffer += numCharsProcessed;
                    max -= numCharsProcessed;
                }
                if (!rangeLen || (buffer && (max == 0))) {
                    if (usedBufLen) {
                        *usedBufLen = numCharsProcessed;
                    }
                    return numCharsProcessed;
                }
                rangeLoc += numCharsProcessed;
                totalBytesWritten += numCharsProcessed;
            }
        }

        if (!buffer) {
            max = 0;
        }

        // Special case for Foundation. When lossByte == 0xFF && encoding kCFStringEncodingASCII,
        //  we do the default ASCII fallback conversion.
        if (lossByte) {
            flags = (unsigned char)lossByte == 0xFF && encoding == kCFStringEncodingASCII ?
                kCFStringEncodingAllowLossyConversion :
                CFStringEncodingLossyByteToMask(lossByte);
        } else {
            flags = 0;
        }
        flags |= (generatingExternalFile ? kCFStringEncodingPrependBOM : 0);

        if (!cString && (cString = (const unsigned char*)CFStringGetCharactersPtr(string))) {  // Must be Unicode string
            if (CFStringEncodingIsValidEncoding(encoding)) { // Converter available in CF
                CFStringEncodingUnicodeToBytes(encoding, flags, (const UniChar*)cString + rangeLoc, rangeLen, &numCharsProcessed, buffer, max, &totalBytesWritten);
            } else {
                return 0;
            }
        } else {
            UniChar charBuf[kCFCharConversionBufferLength];
            CFIndex currentLength;
            CFIndex usedLen;
            CFIndex lastUsedLen = 0, lastNumChars = 0;
            uint32_t result;
            Boolean isCFBuiltin = CFStringEncodingIsValidEncoding(encoding);
#define MAX_DECOMP_LEN (6)

            while (rangeLen > 0) {
                currentLength = (rangeLen > kCFCharConversionBufferLength ? kCFCharConversionBufferLength : rangeLen);
                CFStringGetCharacters(string, CFRangeMake(rangeLoc, currentLength), charBuf);

                // could be in the middle of surrogate pair; back up.
                if ((rangeLen > kCFCharConversionBufferLength) && _CFUniCharIsSurrogateHighCharacter(charBuf[kCFCharConversionBufferLength - 1])) {
                    --currentLength;
                }

                if (isCFBuiltin) { // Converter available in CF
                    if ((result = CFStringEncodingUnicodeToBytes(encoding, flags, charBuf, currentLength, &numChars, buffer, max, &usedLen)) != kCFStringEncodingConversionSuccess) {
                        if (kCFStringEncodingInvalidInputStream == result) {
                            CFRange composedRange;
                            // Check the tail
                            if ((rangeLen > kCFCharConversionBufferLength) && ((currentLength - numChars) < MAX_DECOMP_LEN)) {
                                composedRange = CFStringGetRangeOfComposedCharactersAtIndex(string, rangeLoc + currentLength);

                                if ((composedRange.length <= MAX_DECOMP_LEN) && (composedRange.location < (rangeLoc + numChars))) {
                                    result = CFStringEncodingUnicodeToBytes(encoding, flags, charBuf, composedRange.location - rangeLoc, &numChars, buffer, max, &usedLen);
                                }
                            }

                            // Check the head
                            if ((kCFStringEncodingConversionSuccess != result) && (lastNumChars > 0) && (numChars < MAX_DECOMP_LEN)) {
                                composedRange = CFStringGetRangeOfComposedCharactersAtIndex(string, rangeLoc);

                                if ((composedRange.length <= MAX_DECOMP_LEN) && (composedRange.location < rangeLoc)) {
                                    // Try if the composed range can be converted
                                    CFStringGetCharacters(string, composedRange, charBuf);

                                    if (CFStringEncodingUnicodeToBytes(encoding, flags, charBuf, composedRange.length, &numChars, NULL, 0, &usedLen) == kCFStringEncodingConversionSuccess) { // OK let's try the last run
                                        CFIndex lastRangeLoc = rangeLoc - lastNumChars;

                                        currentLength = composedRange.location - lastRangeLoc;
                                        CFStringGetCharacters(string, CFRangeMake(lastRangeLoc, currentLength), charBuf);

                                        if ((result = CFStringEncodingUnicodeToBytes(encoding, flags, charBuf, currentLength, &numChars, (max ? buffer - lastUsedLen : NULL), (max ? max + lastUsedLen : 0), &usedLen)) == kCFStringEncodingConversionSuccess) { // OK let's try the last run
                                            // Looks good. back up
                                            totalBytesWritten -= lastUsedLen;
                                            numCharsProcessed -= lastNumChars;

                                            rangeLoc = lastRangeLoc;
                                            rangeLen += lastNumChars;

                                            if (max) {
                                                buffer -= lastUsedLen;
                                                max += lastUsedLen;
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        if (kCFStringEncodingConversionSuccess != result) { // really failed
                            totalBytesWritten += usedLen;
                            numCharsProcessed += numChars;
                            break;
                        }
                    }
                } else {
                    return 0;
                }

                totalBytesWritten += usedLen;
                numCharsProcessed += numChars;

                rangeLoc += numChars;
                rangeLen -= numChars;
                if (max) {
                    buffer += usedLen;
                    max -= usedLen;
                    if (max <= 0) {
                        break;
                    }
                }
                lastUsedLen = usedLen; lastNumChars = numChars;
                flags &= ~kCFStringEncodingPrependBOM;
            }
        }
    }
    if (usedBufLen) {
        *usedBufLen = totalBytesWritten;
    }
    return numCharsProcessed;
}
