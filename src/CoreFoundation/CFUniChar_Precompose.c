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

#include <string.h>
#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFCharacterSet.h>
#include "CFUniChar.h"
#include "CFInternal.h"

typedef struct {
    UTF16Char _key;
    UTF16Char _value;
} __CFUniCharPrecomposeBMPMappings;

typedef struct {
    UTF32Char _key;
    uint32_t _value;
} __CFUniCharPrecomposeMappings;

// Canonical Precomposition
static __CFUniCharPrecomposeMappings* __CFUniCharPrecompSourceTable = NULL;
static uint32_t __CFUniCharPrecompositionTableLength = 0;
static uint16_t* __CFUniCharBMPPrecompDestinationTable = NULL;
static uint32_t* __CFUniCharNonBMPPrecompDestinationTable = NULL;

static CFSpinLock_t __CFUniCharPrecompositionTableLock = CFSpinLockInit;

///////////////////////////////////////////////////////////////////// private

static void __CFUniCharLoadPrecompositionTable(void) {

    CFSpinLock(&__CFUniCharPrecompositionTableLock);

    if (!__CFUniCharPrecompSourceTable) {
        const uint32_t* bytes = (const uint32_t*)_CFUniCharGetMappingData(kCFUniCharCanonicalPrecompMapping);
        uint32_t bmpMappingLength;

        if (!bytes) {
            CFSpinUnlock(&__CFUniCharPrecompositionTableLock);
            return;
        }

        __CFUniCharPrecompositionTableLength = *(bytes++);
        bmpMappingLength = *(bytes++);
        __CFUniCharPrecompSourceTable = (__CFUniCharPrecomposeMappings*)bytes;
        __CFUniCharBMPPrecompDestinationTable = (uint16_t*)((intptr_t)bytes + (__CFUniCharPrecompositionTableLength * sizeof(UTF32Char) * 2));
        __CFUniCharNonBMPPrecompDestinationTable = (uint32_t*)(((intptr_t)__CFUniCharBMPPrecompDestinationTable) + bmpMappingLength);
    }

    CFSpinUnlock(&__CFUniCharPrecompositionTableLock);
}

static UTF16Char __CFUniCharGetMappedBMPValue(const __CFUniCharPrecomposeBMPMappings* theTable, uint32_t numElem, UTF16Char character) {
    const __CFUniCharPrecomposeBMPMappings* p, * q, * divider;

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
        } else {return divider->_value; }
    }
    return 0;
}

static uint32_t __CFUniCharGetMappedValue(const __CFUniCharPrecomposeMappings* theTable, uint32_t numElem, UTF32Char character) {
    const __CFUniCharPrecomposeMappings* p, * q, * divider;

    if ((character < theTable[0]._key) || (character > theTable[numElem - 1]._key)) {
        return 0;
    }
    p = theTable;
    q = p + (numElem - 1);
    while (p <= q) {
        divider = p + ((q - p) >> 1);    /* divide by 2 */
        if (character < divider->_key) {
            q = divider - 1;
        } else if (character > divider->_key) {
            p = divider + 1;
        } else {return divider->_value; }
    }
    return 0;
}

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL UTF32Char _CFUniCharPrecomposeCharacter(UTF32Char base, UTF32Char combining) {
    uint32_t value;

    if (!__CFUniCharPrecompSourceTable) {
        __CFUniCharLoadPrecompositionTable();
    }

    value = __CFUniCharGetMappedValue(__CFUniCharPrecompSourceTable, __CFUniCharPrecompositionTableLength, combining);
    if (!value) {
        return 0xFFFD;
    }

    // We don't have precomposition in non-BMP
    if (value & kCFUniCharNonBmpFlag) {
        value = __CFUniCharGetMappedValue((const __CFUniCharPrecomposeMappings*)((uint32_t*)__CFUniCharNonBMPPrecompDestinationTable + (value & 0xFFFF)), (value >> 16) & 0x7FFF, base);
    } else {
        value = __CFUniCharGetMappedBMPValue((const __CFUniCharPrecomposeBMPMappings*)((uint32_t*)__CFUniCharBMPPrecompDestinationTable + (value & 0xFFFF)), (value >> 16), base);
    }
    return (value ? value : 0xFFFD);
}
