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

#include "CFStringEncoding.h"
#include "CFStringEncodingConverter.h"
#include "CFUniChar.h"
#include "CFInternal.h"

/* UTF8 */
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

static const uint32_t kReplacementCharacter = 0x0000FFFDUL;
static const uint32_t kMaximumUCS2 = 0x0000FFFFUL;
static const uint32_t kMaximumUTF16 = 0x0010FFFFUL;
static const uint32_t kMaximumUCS4 = 0x7FFFFFFFUL;

static const int halfShift = 10;
static const uint32_t halfBase = 0x0010000UL;
static const uint32_t halfMask = 0x3FFUL;
static const uint32_t kSurrogateHighStart = 0xD800UL;
static const uint32_t kSurrogateHighEnd = 0xDBFFUL;
static const uint32_t kSurrogateLowStart = 0xDC00UL;
static const uint32_t kSurrogateLowEnd = 0xDFFFUL;

/*
 * Index into the table below with the first byte of a UTF-8 sequence to
 * get the number of trailing bytes that are supposed to follow it.
 */
static const char __CFUTF8TrailingBytes[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5
};

/*
 * Magic values subtracted from a buffer value during UTF8 conversion.
 * This table contains as many values as there might be trailing bytes
 * in a UTF-8 sequence.
 */
static const UTF32Char __CFUTF8Offsets[6] = {
    0x00000000UL, 0x00003080UL, 0x000E2080UL,
    0x03C82080UL, 0xFA082080UL, 0x82082080UL};

static const uint8_t __CFUTF8FirstByteMark[7] = {
    0x00, 0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC
};

///////////////////////////////////////////////////////////////////// private

/* This code is similar in effect to making successive calls on the mbtowc and wctomb 
 * routines in FSS-UTF. However, it is considerably different in code:
 *  - it is adapted to be consistent with UTF16;
 *  - constants have been gathered;
 *  - loops & conditionals have been removed as much as possible for
 *    efficiency, in favor of drop-through switch statements.
 */

static uint16_t __CFUTF8BytesToWriteForCharacter(uint32_t ch) {
    if (ch < 0x80) {
        return 1;
    } else if (ch < 0x800) {
        return 2;
    } else if (ch < 0x10000) {
        return 3;
    } else if (ch < 0x200000) {
        return 4;
    } else if (ch < 0x4000000) {
        return 5;
    } else if (ch <= kMaximumUCS4) {
        return 6;
    } else {return 0; }
}

static uint16_t __CFToUTF8Core(uint32_t ch, uint8_t* bytes, uint32_t maxByteLen) {
    uint16_t bytesToWrite = __CFUTF8BytesToWriteForCharacter(ch);
    const uint32_t byteMask = 0xBF;
    const uint32_t byteMark = 0x80;

    if (!bytesToWrite) {
        bytesToWrite = 2;
        ch = kReplacementCharacter;
    }

    if (maxByteLen < bytesToWrite) {
        return 0;
    }

    switch (bytesToWrite) {    /* note: code falls through cases! */
        case 6: bytes[5] = (ch | byteMark) & byteMask; ch >>= 6;
        case 5: bytes[4] = (ch | byteMark) & byteMask; ch >>= 6;
        case 4: bytes[3] = (ch | byteMark) & byteMask; ch >>= 6;
        case 3: bytes[2] = (ch | byteMark) & byteMask; ch >>= 6;
        case 2: bytes[1] = (ch | byteMark) & byteMask; ch >>= 6;
        case 1: bytes[0] =  ch | __CFUTF8FirstByteMark[bytesToWrite];
    }
    return bytesToWrite;
}

static CFIndex __CFToUTF8(uint32_t flags, const UniChar* characters, CFIndex numChars, uint8_t* bytes, CFIndex maxByteLen, CFIndex* usedByteLen) {
    uint16_t bytesWritten;
    uint32_t ch;
    const UniChar* beginCharacter = characters;
    const UniChar* endCharacter = characters + numChars;
    const uint8_t* beginBytes = bytes;
    const uint8_t* endBytes = bytes + maxByteLen;
    bool isStrict = (flags & kCFStringEncodingUseHFSPlusCanonical ? false : true);

    while ((characters < endCharacter) && (!maxByteLen || (bytes < endBytes))) {
        ch = *(characters++);

        if (ch < 0x80) { // ASCII
            if (maxByteLen) {
                *bytes = ch;
            }
            ++bytes;
        } else {
            if (ch >= kSurrogateHighStart) {
                if (ch <= kSurrogateHighEnd) {
                    if ((characters < endCharacter) && ((*characters >= kSurrogateLowStart) && (*characters <= kSurrogateLowEnd))) {
                        ch = ((ch - kSurrogateHighStart) << halfShift) + (*(characters++) - kSurrogateLowStart) + halfBase;
                    } else if (isStrict) {
                        --characters;
                        break;
                    }
                } else if (isStrict && (ch <= kSurrogateLowEnd)) {
                    --characters;
                    break;
                }
            }

            if (!(bytesWritten = (maxByteLen ? __CFToUTF8Core(ch, bytes, endBytes - bytes) : __CFUTF8BytesToWriteForCharacter(ch)))) {
                characters -= (ch < 0x10000 ? 1 : 2);
                break;
            }
            bytes += bytesWritten;
        }
    }

    if (usedByteLen) {
        *usedByteLen = bytes - beginBytes;
    }
    return characters - beginCharacter;
}

/* Utility routine to tell whether a sequence of bytes is legal UTF-8.
 * This must be called with the length pre-determined by the first byte.
 * If not calling this from ConvertUTF8to*, then the length can be set by:
 *    length = __CFUTF8TrailingBytes[*source]+1;
 * and the sequence is illegal right away if there aren't that many bytes
 * available.
 * If presented with a length > 4, this returns false.  The Unicode
 * definition of UTF-8 goes up to 4-byte sequences.
 */
static bool __CFIsLegalUTF8(const uint8_t* source, CFIndex length) {
    if (length > 4) {
        return false;
    }

    const uint8_t* srcptr = source + length;
    uint8_t head = *source;

    while (--srcptr > source) {
        if ((*srcptr & 0xC0) != 0x80) {
            return false;
        }
    }

    if (((head >= 0x80) && (head < 0xC2)) || (head > 0xF4)) {
        return false;
    }

    if (((head == 0xE0) && (*(source + 1) < 0xA0)) || ((head == 0xED) && (*(source + 1) > 0x9F)) || ((head == 0xF0) && (*(source + 1) < 0x90)) || ((head == 0xF4) && (*(source + 1) > 0x8F))) {
        return false;
    }
    return true;
}

static CFIndex __CFFromUTF8(uint32_t flags, const uint8_t* bytes, CFIndex numBytes, UniChar* characters, CFIndex maxCharLen, CFIndex* usedCharLen) {
    const uint8_t* source = bytes;
    uint16_t extraBytesToRead;
    CFIndex theUsedCharLen = 0;
    uint32_t ch;
    bool isHFSPlus = (flags & kCFStringEncodingUseHFSPlusCanonical ? true : false);
    bool needsToDecompose = (flags & kCFStringEncodingUseCanonical || isHFSPlus ? true : false);
    bool strictUTF8 = (flags & kCFStringEncodingLenientUTF8Conversion ? false : true);
    UTF32Char decomposed[kCFUniCharMaxDecomposedLength];
    CFIndex decompLength;
    bool isStrict = !isHFSPlus;

    while (numBytes && (!maxCharLen || (theUsedCharLen < maxCharLen))) {
        extraBytesToRead = __CFUTF8TrailingBytes[*source];

        if (extraBytesToRead > --numBytes) {
            break;
        }
        numBytes -= extraBytesToRead;

        /* Do this check whether lenient or strict */
        // We need to allow 0xA9 (copyright in MacRoman and Unicode) not to break existing apps
        // Will use a flag passed in from upper layers to switch restriction mode for this case in the next release
        if ((extraBytesToRead > 3) || (strictUTF8 && !__CFIsLegalUTF8(source, extraBytesToRead + 1))) {
            if ((*source == 0xA9) || (flags & kCFStringEncodingAllowLossyConversion)) {
                numBytes += extraBytesToRead;
                ++source;
                if (maxCharLen) {
                    *(characters++) = (UTF16Char)kReplacementCharacter;
                }
                ++theUsedCharLen;
                continue;
            } else {
                break;
            }
        }

        ch = 0;
        /*
         * The cases all fall through. See "Note A" below.
         */
        switch (extraBytesToRead) {
            case 3:    ch += *source++; ch <<= 6;
            case 2:    ch += *source++; ch <<= 6;
            case 1:    ch += *source++; ch <<= 6;
            case 0:    ch += *source++;
        }
        ch -= __CFUTF8Offsets[extraBytesToRead];

        if (ch <= kMaximumUCS2) {
            if (isStrict && (ch >= kSurrogateHighStart && ch <= kSurrogateLowEnd)) {
                source -= (extraBytesToRead + 1);
                break;
            }
            if (needsToDecompose && _CFUniCharIsDecomposableCharacter(ch)) {
                decompLength = _CFUniCharDecomposeCharacter(ch, decomposed, kCFUniCharMaxDecomposedLength);

                if (maxCharLen) {
                    if (!_CFUniCharFillDestinationBuffer(decomposed, decompLength, (void**)&characters, maxCharLen, &theUsedCharLen, kCFUniCharUTF16Format)) {
                        break;
                    }
                } else {
                    theUsedCharLen += decompLength;
                }
            } else {
                if (maxCharLen) {
                    *(characters++) = (UTF16Char)ch;
                }
                ++theUsedCharLen;
            }
        } else if (ch > kMaximumUTF16) {
            if (isStrict) {
                source -= (extraBytesToRead + 1);
                break;
            }
            if (maxCharLen) {
                *(characters++) = (UTF16Char)kReplacementCharacter;
            }
            ++theUsedCharLen;
        } else {
            if (needsToDecompose && _CFUniCharIsDecomposableCharacter(ch)) {
                decompLength = _CFUniCharDecomposeCharacter(ch, decomposed, kCFUniCharMaxDecomposedLength);

                if (maxCharLen) {
                    if (!_CFUniCharFillDestinationBuffer(decomposed, decompLength, (void**)&characters, maxCharLen, &theUsedCharLen, kCFUniCharUTF16Format)) {
                        break;
                    }
                } else {
                    while (--decompLength >= 0) {
                        theUsedCharLen += (decomposed[decompLength] < 0x10000 ? 1 : 2);
                    }
                }
            } else {
                if (maxCharLen) {
                    if ((theUsedCharLen + 2) > maxCharLen) {
                        break;
                    }
                    ch -= halfBase;
                    *(characters++) = (ch >> halfShift) + kSurrogateHighStart;
                    *(characters++) = (ch & halfMask) + kSurrogateLowStart;
                }
                theUsedCharLen += 2;
            }
        }
    }

    if (usedCharLen) {
        *usedCharLen = theUsedCharLen;
    }

    return source - bytes;
}

static CFIndex __CFToUTF8Len(uint32_t flags, const UniChar* characters, CFIndex numChars) {
    uint32_t bytesToWrite = 0;
    uint32_t ch;

    while (numChars) {
        ch = *characters++;
        numChars--;
        if ((ch >= kSurrogateHighStart && ch <= kSurrogateHighEnd) && numChars && (*characters >= kSurrogateLowStart && *characters <= kSurrogateLowEnd)) {
            ch = ((ch - kSurrogateHighStart) << halfShift) + (*characters++ - kSurrogateLowStart) + halfBase;
            numChars--;
        }
        bytesToWrite += __CFUTF8BytesToWriteForCharacter(ch);
    }

    return bytesToWrite;
}

static CFIndex __CFFromUTF8Len(uint32_t flags, const uint8_t* source, CFIndex numBytes) {
    uint16_t extraBytesToRead;
    CFIndex theUsedCharLen = 0;
    uint32_t ch;
    bool isHFSPlus = (flags & kCFStringEncodingUseHFSPlusCanonical ? true : false);
    bool needsToDecompose = (flags & kCFStringEncodingUseCanonical || isHFSPlus ? true : false);
    bool strictUTF8 = (flags & kCFStringEncodingLenientUTF8Conversion ? false : true);
    UTF32Char decomposed[kCFUniCharMaxDecomposedLength];
    CFIndex decompLength;
    bool isStrict = !isHFSPlus;

    while (numBytes) {
        extraBytesToRead = __CFUTF8TrailingBytes[*source];

        if (extraBytesToRead > --numBytes) {
            break;
        }
        numBytes -= extraBytesToRead;

        /* Do this check whether lenient or strict */
        // We need to allow 0xA9 (copyright in MacRoman and Unicode) not to break existing apps
        // Will use a flag passed in from upper layers to switch restriction mode for this case in the next release
        if ((extraBytesToRead > 3) || (strictUTF8 && !__CFIsLegalUTF8(source, extraBytesToRead + 1))) {
            if ((*source == 0xA9) || (flags & kCFStringEncodingAllowLossyConversion)) {
                numBytes += extraBytesToRead;
                ++source;
                ++theUsedCharLen;
                continue;
            } else {
                break;
            }
        }

        ch = 0;
        /*
         * The cases all fall through. See "Note A" below.
         */
        switch (extraBytesToRead) {
            case 3:    ch += *source++; ch <<= 6;
            case 2:    ch += *source++; ch <<= 6;
            case 1:    ch += *source++; ch <<= 6;
            case 0:    ch += *source++;
        }
        ch -= __CFUTF8Offsets[extraBytesToRead];

        if (ch <= kMaximumUCS2) {
            if (isStrict && (ch >= kSurrogateHighStart && ch <= kSurrogateLowEnd)) {
                break;
            }
            if (needsToDecompose && _CFUniCharIsDecomposableCharacter(ch)) {
                decompLength = _CFUniCharDecomposeCharacter(ch, decomposed, kCFUniCharMaxDecomposedLength);
                theUsedCharLen += decompLength;
            } else {
                ++theUsedCharLen;
            }
        } else if (ch > kMaximumUTF16) {
            ++theUsedCharLen;
        } else {
            if (needsToDecompose && _CFUniCharIsDecomposableCharacter(ch)) {
                decompLength = _CFUniCharDecomposeCharacter(ch, decomposed, kCFUniCharMaxDecomposedLength);
                while (--decompLength >= 0) {
                    theUsedCharLen += (decomposed[decompLength] < 0x10000 ? 1 : 2);
                }
            } else {
                theUsedCharLen += 2;
            }
        }
    }

    return theUsedCharLen;
}

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL const _CFStringEncodingConverter _CFEncodingConverterUTF8 = {
    (void*)__CFToUTF8,
    (void*)__CFFromUTF8,
    3,
    2,
    kCFStringEncodingConverterStandard,
    __CFToUTF8Len,
    __CFFromUTF8Len,
    NULL,
    NULL,
    NULL,
    NULL,
};
