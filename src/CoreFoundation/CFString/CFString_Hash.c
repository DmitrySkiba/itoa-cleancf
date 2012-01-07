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

static Boolean (* __CFCharToUniCharFunc)(UInt32 flags, uint8_t ch, UniChar* unicodeChar) = NULL;

// To avoid early initialization issues, we just initialize this here
// This should not be const as it is changed
CF_INTERNAL UniChar __CFCharToUniCharTable[256] = {
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,  15,
    16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,
    32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,
    48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,
    64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,
    80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,
    96,  97,  98,  99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127,
    128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
    144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
    160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
    176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
    192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
    208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
    224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
    240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255
};

/* String hashing should give the same results whatever the encoding, 
 *  so we hash UniChars.
 * If the length is less than or equal to 96, then the hash function is 
 *  simply the following (n is the nth UniChar character, starting from 0):
 *
 * hash(-1) = length
 * hash(n) = hash(n-1) * 257 + unichar(n);
 * Hash = hash(length-1) * ((length & 31) + 1)
 *
 * If the length is greater than 96, then the above algorithm applies to
 *  characters 0..31, (length/2)-16..(length/2)+15, and length-32..length-1,
 *  inclusive; thus the first, middle, and last 32 characters.
 *
 * Note that the loops below are unrolled; and:
 *  257^2 = 66049;
 *  257^3 = 16974593;
 *  257^4 = 4362470401;
 *  67503105 is 257^4 - 256^4
 * If hashcode is changed from UInt32 to something else, this last piece 
 *  needs to be readjusted.
 *
 * !!! We haven't updated for LP64 yet
 *
 * NOTE: The hash algorithm used to be duplicated in CF and Foundation; 
 *  but now it should only be in the four functions below.
 *
 * Hash function was changed between Panther and Tiger, and Tiger and Leopard.
 */
#define HashEverythingLimit 96

#define HashNextFourUniChars(accessStart, accessEnd, pointer) \
    { \
        result = result * 67503105 + \
            (accessStart 0 accessEnd) * 16974593 + \
            (accessStart 1 accessEnd) * 66049  + \
            (accessStart 2 accessEnd) * 257 + \
            (accessStart 3 accessEnd); \
        pointer += 4; \
    }

#define HashNextUniChar(accessStart, accessEnd, pointer) \
    { \
        result = result * 257 + \
            (accessStart 0 accessEnd); \
        pointer++; \
    }

///////////////////////////////////////////////////////////////////// internal

/* In this function, actualLen is the length of the original string;
 *  but len is the number of characters in buffer. The buffer is expected
 *  to contain the parts of the string relevant to hashing.
 */
CF_INTERNAL CFHashCode __CFStrHashCharacters(const UniChar* uContents, CFIndex len, CFIndex actualLen) {
    CFHashCode result = actualLen;
    if (len <= HashEverythingLimit) {
        const UniChar* end4 = uContents + (len & ~3);
        const UniChar* end = uContents + len;
        while (uContents < end4) {
            HashNextFourUniChars(uContents[, ], uContents); // First count in fours
        }
        while (uContents < end) {
            HashNextUniChar(uContents[, ], uContents); // Then for the last <4 chars, count in ones...
        }
    } else {
        const UniChar* contents, * end;
        contents = uContents;
        end = contents + 32;
        while (contents < end) {
            HashNextFourUniChars(contents[, ], contents);
        }
        contents = uContents + (len >> 1) - 16;
        end = contents + 32;
        while (contents < end) {
            HashNextFourUniChars(contents[, ], contents);
        }
        end = uContents + len;
        contents = end - 32;
        while (contents < end) {
            HashNextFourUniChars(contents[, ], contents);
        }
    }
    return result + (result << (actualLen & 31));
}

/* This hashes cString in the eight bit string encoding.
 */
CF_INTERNAL CFHashCode __CFStrHashEightBit(const uint8_t* cContents, CFIndex len) {
    CFHashCode result = len;
    if (len <= HashEverythingLimit) {
        const uint8_t* end4 = cContents + (len & ~3);
        const uint8_t* end = cContents + len;
        while (cContents < end4) {
            HashNextFourUniChars(__CFCharToUniCharTable[cContents[, ]], cContents); // First count in fours
        }
        while (cContents < end) {
            HashNextUniChar(__CFCharToUniCharTable[cContents[, ]], cContents); // Then for the last <4 chars, count in ones...
        }
    } else {
        const uint8_t* contents, * end;
        contents = cContents;
        end = contents + 32;
        while (contents < end) {
            HashNextFourUniChars(__CFCharToUniCharTable[contents[, ]], contents);
        }
        contents = cContents + (len >> 1) - 16;
        end = contents + 32;
        while (contents < end) {
            HashNextFourUniChars(__CFCharToUniCharTable[contents[, ]], contents);
        }
        end = cContents + len;
        contents = end - 32;
        while (contents < end) {
            HashNextFourUniChars(__CFCharToUniCharTable[contents[, ]], contents);
        }
    }
    return result + (result << (len & 31));
}

CF_INTERNAL void __CFSetCharToUniCharFunc(Boolean (*func)(UInt32 flags, UInt8 ch, UniChar* unicodeChar)) {
    if (__CFCharToUniCharFunc != func) {
        int ch;
        __CFCharToUniCharFunc = func;
        if (func) {
            for (ch = 128; ch < 256; ch++) {
                UniChar uch;
                __CFCharToUniCharTable[ch] = (__CFCharToUniCharFunc(0, ch, &uch) ? uch : 0xFFFD);
            }
        } else {
            // If we have no __CFCharToUniCharFunc, assume 128..255 return the value as-is.
            for (ch = 128; ch < 256; ch++) {
                __CFCharToUniCharTable[ch] = ch;
            }
        }
    }
}

CF_INTERNAL void __CFStrConvertBytesToUnicode(const uint8_t* bytes, UniChar* buffer, CFIndex numChars) {
    CFIndex idx;
    for (idx = 0; idx < numChars; idx++) {
        buffer[idx] = __CFCharToUniCharTable[bytes[idx]];
    }
}

///////////////////////////////////////////////////////////////////// public

CFHashCode CFStringHashISOLatin1CString(const uint8_t* bytes, CFIndex len) {
    CFHashCode result = len;
    if (len <= HashEverythingLimit) {
        const uint8_t* end4 = bytes + (len & ~3);
        const uint8_t* end = bytes + len;
        while (bytes < end4) {
            HashNextFourUniChars(bytes[, ], bytes); // First count in fours
        }
        while (bytes < end) {
            HashNextUniChar(bytes[, ], bytes); // Then for the last <4 chars, count in ones...
        }
    } else {
        const uint8_t* contents, * end;
        contents = bytes;
        end = contents + 32;
        while (contents < end) {
            HashNextFourUniChars(contents[, ], contents);
        }
        contents = bytes + (len >> 1) - 16;
        end = contents + 32;
        while (contents < end) {
            HashNextFourUniChars(contents[, ], contents);
        }
        end = bytes + len;
        contents = end - 32;
        while (contents < end) {
            HashNextFourUniChars(contents[, ], contents);
        }
    }
    return result + (result << (len & 31));
}

CFHashCode CFStringHashCString(const uint8_t* bytes, CFIndex len) {
    return __CFStrHashEightBit(bytes, len);
}

CFHashCode CFStringHashCharacters(const UniChar* characters, CFIndex len) {
    return __CFStrHashCharacters(characters, len, len);
}

/* This is meant to be called from NSString or subclassers only.
 * It is an error for this to be called without the ObjC runtime or an 
 *  argument which is not an NSString or subclass.
 * It can be called with NSCFString, although that would be inefficient 
 *  (causing indirection) and won't normally happen anyway, as NSCFString 
 *  overrides hash.
 */
CFHashCode CFStringHashNSString(CFStringRef str) {
    UniChar buffer[HashEverythingLimit];
    CFIndex bufLen; // Number of characters in the buffer for hashing
    CFIndex len = 0; // Actual length of the string

    CF_OBJC_CALL(CFIndex, len, str, "length");
    if (len <= HashEverythingLimit) {
        CF_OBJC_VOID_CALL(str, "getCharacters:range:", buffer, CFRangeMake(0, len));
        bufLen = len;
    } else {
        CF_OBJC_VOID_CALL(str, "getCharacters:range:", buffer, CFRangeMake(0, 32));
        CF_OBJC_VOID_CALL(str, "getCharacters:range:", buffer + 32, CFRangeMake((len >> 1) - 16, 32));
        CF_OBJC_VOID_CALL(str, "getCharacters:range:", buffer + 64, CFRangeMake(len - 32, 32));
        bufLen = HashEverythingLimit;
    }
    return __CFStrHashCharacters(buffer, bufLen, len);
}
