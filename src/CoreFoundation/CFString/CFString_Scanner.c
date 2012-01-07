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

#include <limits.h>
#include <stdlib.h>
#include "CFStringInternal.h"
#include "CFUniChar.h"


// Packed array of Boolean
static const unsigned char __CFNumberSet[16] = {
    0X00, // 0, 0, 0, 0, 0, 0, 0, 0, //  nul soh stx etx eot enq ack bel
    0X00, // 0, 0, 0, 0, 0, 0, 0, 0, //  bs  ht  nl  vt  np  cr  so  si
    0X00, // 0, 0, 0, 0, 0, 0, 0, 0, //  dle dc1 dc2 dc3 dc4 nak syn etb
    0X00, // 0, 0, 0, 0, 0, 0, 0, 0, //  can em  sub esc fs  gs  rs  us
    0X00, // 0, 0, 0, 0, 0, 0, 0, 0, //  sp   !   "   #   $   %   &   '
    0X28, // 0, 0, 0, 1, 0, 1, 0, 0, //  (   )   *   +   ,   -   .   /
    0xFF, // 1, 1, 1, 1, 1, 1, 1, 1, //  0   1   2   3   4   5   6   7
    0X03, // 1, 1, 0, 0, 0, 0, 0, 0, //  8   9   :   ;   <   =   >   ?
    0X20, // 0, 0, 0, 0, 0, 1, 0, 0, //  @   A   B   C   D   E   F   G
    0X00, // 0, 0, 0, 0, 0, 0, 0, 0, //  H   I   J   K   L   M   N   O
    0X00, // 0, 0, 0, 0, 0, 0, 0, 0, //  P   Q   R   S   T   U   V   W
    0X00, // 0, 0, 0, 0, 0, 0, 0, 0, //  X   Y   Z   [   \   ]   ^   _
    0X20, // 0, 0, 0, 0, 0, 1, 0, 0, //  `   a   b   c   d   e   f   g
    0X00, // 0, 0, 0, 0, 0, 0, 0, 0, //  h   i   j   k   l   m   n   o
    0X00, // 0, 0, 0, 0, 0, 0, 0, 0, //  p   q   r   s   t   u   v   w
    0X00, // 0, 0, 0, 0, 0, 0, 0, 0  //  x   y   z   {   |   }   ~  del
};

///////////////////////////////////////////////////////////////////// private

CF_INLINE Boolean __CFCharacterIsADigit(UniChar ch) {
    return (ch >= '0' && ch <= '9');
}

/* Returns -1 on illegal value */
CF_INLINE SInt32 __CFCharacterNumericValue(UniChar ch) {
    return (ch >= '0' && ch <= '9') ? (ch - '0') : -1;
}

CF_INLINE UniChar __CFStringGetFirstNonSpaceCharacter(CFStringInlineBuffer* buf, SInt32* indexPtr) {
    UniChar ch;
    while (true) {
        if (*indexPtr >= buf->rangeToBuffer.length) {
            ch = 0xFFFF;
            break;
        }
        ch = CFStringGetCharacterFromInlineBuffer(buf, *indexPtr);
        if (!_CFUniCharIsWhitespace(ch)) {
            break;
        }
        (*indexPtr)++;
    }
    return ch;
}

///////////////////////////////////////////////////////////////////// internal

/* result is int64_t or int, depending on doLonglong
 */
CF_INTERNAL Boolean _CFStringScanInteger(CFStringInlineBuffer* buf, CFTypeRef locale, SInt32* indexPtr, Boolean doLonglong, void* result) {
    Boolean doingLonglong = false; /* Set to true if doLonglong, and we overflow an int... */
    Boolean neg = false;
    int intResult = 0;
    int64_t longlongResult = 0;
    UniChar ch;

    ch = __CFStringGetFirstNonSpaceCharacter(buf, indexPtr);

    if (ch == '-' || ch == '+') {
        neg = (ch == '-');
        (*indexPtr)++;
        ch = __CFStringGetFirstNonSpaceCharacter(buf, indexPtr);
    }

    if (!__CFCharacterIsADigit(ch)) {
        return false; /* No digits, bail out... */
    }
    do {
        if (doingLonglong) {
            if ((longlongResult >= LLONG_MAX / 10) && 
                ((longlongResult > LLONG_MAX / 10) || 
                    (__CFCharacterNumericValue(ch) - (neg ? 1 : 0) >= LLONG_MAX - longlongResult * 10)))
            {
                /* ??? This might not handle LLONG_MIN correctly... */
                longlongResult = neg ? LLONG_MIN : LLONG_MAX;
                neg = false;
                while (__CFCharacterIsADigit(ch = CFStringGetCharacterFromInlineBuffer(buf, ++(*indexPtr)))) {
                    ;                                                                                                   /* Skip remaining digits */
                }
            } else {
                longlongResult = longlongResult * 10 + __CFCharacterNumericValue(ch);
                ch = CFStringGetCharacterFromInlineBuffer(buf, ++(*indexPtr));
            }
        } else {
            if ((intResult >= INT_MAX / 10) &&
                ((intResult > INT_MAX / 10) ||
                    (__CFCharacterNumericValue(ch) - (neg ? 1 : 0) >= INT_MAX - intResult * 10)))
            {
                // Overflow, check for int64_t...
                if (doLonglong) {
                    longlongResult = intResult;
                    doingLonglong = true;
                } else {
                    /* ??? This might not handle INT_MIN correctly... */
                    intResult = neg ? INT_MIN : INT_MAX;
                    neg = false;
                    while (__CFCharacterIsADigit(ch = CFStringGetCharacterFromInlineBuffer(buf, ++(*indexPtr)))) {
                        ;                                                                                               /* Skip remaining digits */
                    }
                }
            } else {
                intResult = intResult * 10 + __CFCharacterNumericValue(ch);
                ch = CFStringGetCharacterFromInlineBuffer(buf, ++(*indexPtr));
            }
        }
    } while (__CFCharacterIsADigit(ch));

    if (result) {
        if (doLonglong) {
            if (!doingLonglong) {
                longlongResult = intResult;
            }
            *(int64_t*)result = neg ? -longlongResult : longlongResult;
        } else {
            *(int*)result = neg ? -intResult : intResult;
        }
    }

    return true;
}

CF_INTERNAL Boolean _CFStringScanDouble(CFStringInlineBuffer* buf, CFTypeRef locale, SInt32* indexPtr, double* resultPtr) {
    #define STACK_BUFFER_SIZE 256
    #define ALLOC_CHUNK_SIZE  256 // first and subsequent malloc size.  Should be greater than STACK_BUFFER_SIZE
    char localCharBuffer[STACK_BUFFER_SIZE];
    char* charPtr = localCharBuffer;
    char* endCharPtr;
    UniChar decimalChar = '.';
    SInt32 numChars = 0;
    SInt32 capacity = STACK_BUFFER_SIZE; // in chars
    double result;
    UniChar ch;
    CFAllocatorRef tmpAlloc = NULL;

    ch = __CFStringGetFirstNonSpaceCharacter(buf, indexPtr);
    // At this point indexPtr points at the first non-space char
    do {
        if (ch >= 128 || (__CFNumberSet[ch >> 3] & (1 << (ch & 7))) == 0) {
            // Not in __CFNumberSet
            if (ch != decimalChar) {
                break;
            }
            ch = '.'; // Replace the decimal character with something strtod will understand
        }
        if (numChars >= capacity - 1) {
            capacity += ALLOC_CHUNK_SIZE;
            if (tmpAlloc == NULL) {
                tmpAlloc = CFAllocatorGetDefault();
            }
            if (charPtr == localCharBuffer) {
                charPtr = (char*)CFAllocatorAllocate(tmpAlloc, capacity * sizeof(char), 0);
                memmove(charPtr, localCharBuffer, numChars * sizeof(char));
            } else {
                charPtr = (char*)CFAllocatorReallocate(tmpAlloc, charPtr, capacity * sizeof(char), 0);
            }
        }
        charPtr[numChars++] = (char)ch;
        ch = CFStringGetCharacterFromInlineBuffer(buf, *indexPtr + numChars);
    } while (true);

    charPtr[numChars] = 0; // Null byte for strtod
    result = strtod(charPtr, &endCharPtr);

    if (tmpAlloc) {
        CFAllocatorDeallocate(tmpAlloc, charPtr);
    }
    if (charPtr == endCharPtr) {
        return false;
    }
    *indexPtr += (endCharPtr - charPtr);
    if (resultPtr) {
        *resultPtr = result;            // only store result if we succeed

    }
    return true;
}

SInt32 CFStringGetIntValue(CFStringRef str) {
    Boolean success;
    SInt32 result;
    SInt32 idx = 0;
    CFStringInlineBuffer buf;
    CFStringInitInlineBuffer(str, &buf, CFRangeMake(0, CFStringGetLength(str)));
    success = _CFStringScanInteger(&buf, NULL, &idx, false, &result);
    return success ? result : 0;
}

double CFStringGetDoubleValue(CFStringRef str) {
    Boolean success;
    double result;
    SInt32 idx = 0;
    CFStringInlineBuffer buf;
    CFStringInitInlineBuffer(str, &buf, CFRangeMake(0, CFStringGetLength(str)));
    success = _CFStringScanDouble(&buf, NULL, &idx, &result);
    return success ? result : 0.0;
}
