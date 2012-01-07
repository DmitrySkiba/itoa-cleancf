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
#include "CFUtilities.h"

CF_INTERNAL
CFIndex _CFBSearch(const void* element, CFIndex elementSize,
				   const void* list, CFIndex count,
				   CFComparatorFunction comparator, void* context)
{
    const char* ptr = (const char*)list;
    while (0 < count) {
        CFIndex half = count / 2;
        const char* probe = ptr + elementSize * half;
        CFComparisonResult cr = comparator(element, probe, context);
        if (!cr) {
            return (probe - (const char*)list) / elementSize;
        }
        ptr = (cr < 0) ? ptr : probe + elementSize;
        count = (cr < 0) ? half : (half + (count & 1) - 1);
    }
    return (ptr - (const char*)list) / elementSize;
}

CF_INTERNAL
CFHashCode _CFHashBytes(uint8_t* bytes, CFIndex length) {
    /* The ELF hash algorithm, used in the ELF object file format */

    #define ELF_STEP(B) \
        T1 = (H << 4) + B; \
        T2 = T1 & 0xF0000000; \
        if (T2) {T1 ^= (T2 >> 24); } \
        T1 &= (~T2); \
        H = T1;

	/*****/

    UInt32 H = 0, T1, T2;
    SInt32 rem = length;
    while (3 < rem) {
        ELF_STEP(bytes[length - rem]);
        ELF_STEP(bytes[length - rem + 1]);
        ELF_STEP(bytes[length - rem + 2]);
        ELF_STEP(bytes[length - rem + 3]);
        rem -= 4;
    }
    switch (rem) {
        case 3:  ELF_STEP(bytes[length - 3]);
        case 2:  ELF_STEP(bytes[length - 2]);
        case 1:  ELF_STEP(bytes[length - 1]);
        case 0:;
    }
    return H;

	/*****/

    #undef ELF_STEP
}


