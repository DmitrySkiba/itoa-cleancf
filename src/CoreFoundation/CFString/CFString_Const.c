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
#include "CFString_Common.h"

/* Table which holds constant strings created with CFSTR, 
 *  when -fconstant-cfstrings option is not used. These dynamically
 *  created constant strings are stored in __CFCStrTable. The keys
 *  are the 8-bit constant C-strings from the compiler; the values
 *  are the CFStrings created for them.
 */
static CFMutableDictionaryRef __CFCStrTable = NULL;
static CFSpinLock_t __CFCStrTableLock = CFSpinLockInit;

///////////////////////////////////////////////////////////////////// private

static CFStringRef __CFCStrCopyDescription(const void* ptr) {
    return CFStringCreateWithCStringNoCopy(
        kCFAllocatorSystemDefault,
        (const char*)ptr,
        __CFStringGetEightBitStringEncoding(),
        kCFAllocatorNull);
}

static Boolean __CFCStrEqual(const void* ptr1, const void* ptr2) {
    return (strcmp((const char*)ptr1, (const char*)ptr2) == 0);
}

static CFHashCode __CFCStrHash(const void* ptr) {
    // It doesn't quite matter if we convert to Unicode correctly, 
    //  as long as we do it consistently.

    const char* cStr = (const char*)ptr;
    CFIndex len = strlen(cStr);
    CFHashCode result = 0;
    if (len <= 4) {    // All chars
        CFIndex cnt = len;
        while (cnt--) {
            result += (result << 8) + *cStr++;
        }
    } else {        // First and last 2 chars
        result += (result << 8) + cStr[0];
        result += (result << 8) + cStr[1];
        result += (result << 8) + cStr[len - 2];
        result += (result << 8) + cStr[len - 1];
    }
    result += (result << (len & 31));
    return result;
}

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL Boolean _CFStringIsConstantString(CFStringRef str) {
    Boolean found = false;
    if (__CFCStrTable) {
        CFSpinLock(&__CFCStrTableLock);
        found = CFDictionaryContainsValue(__CFCStrTable, str);
        CFSpinUnlock(&__CFCStrTableLock);
    }
    return found;
}

///////////////////////////////////////////////////////////////////// public

int __CFConstantStringClassReference[12] = {0};

CFStringRef __CFStringMakeConstantString(const char* cStr) {
    CFStringRef result;
    if (__CFCStrTable == NULL) {
        CFDictionaryKeyCallBacks constantStringCallBacks = {
            0,
            NULL,
            NULL,
            __CFCStrCopyDescription,
            __CFCStrEqual,
            __CFCStrHash
        };
        CFDictionaryValueCallBacks constantStringValueCallBacks = kCFTypeDictionaryValueCallBacks;
        constantStringValueCallBacks.equal = NULL; // So that we only find strings that are ==
        CFMutableDictionaryRef table = CFDictionaryCreateMutable(
            kCFAllocatorSystemDefault,
            0,
            &constantStringCallBacks, &constantStringValueCallBacks);
        _CFDictionarySetCapacity(table, 2500); // avoid lots of rehashing
        CFSpinLock(&__CFCStrTableLock);
        if (__CFCStrTable == NULL) {
            __CFCStrTable = table;
        }
        CFSpinUnlock(&__CFCStrTableLock);
        if (__CFCStrTable != table) {
            CFRelease(table);
        }
    }

    CFSpinLock(&__CFCStrTableLock);
    if ((result = (CFStringRef)CFDictionaryGetValue(__CFCStrTable, cStr))) {
        CFSpinUnlock(&__CFCStrTableLock);
    } else {
        CFSpinUnlock(&__CFCStrTableLock);
        {
            char* key;
            Boolean isASCII = true;
            // Given this code path is rarer these days, OK to do this 
            //  extra work to verify the strings.
            const char* tmp = cStr;
            while (*tmp) {
                if (*(tmp++) & 0x80) {
                    isASCII = false;
                    break;
                }
            }
            if (!isASCII) {
                CFMutableStringRef ms = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
                tmp = cStr;
                while (*tmp) {
                    CFStringAppendFormat(ms, NULL, (*tmp & 0x80) ? CFSTR("\\%3o") : CFSTR("%1c"), *tmp);
                    tmp++;
                }
                CFLog(
                    kCFLogLevelWarning, 
                    CFSTR("WARNING: CFSTR(\"%@\") has non-7 bit chars, interpreting "
                        "using MacOS Roman encoding for now, but this will change. "
                        "Please eliminate usages of non-7 bit chars (including escaped "
                        "characters above \\177 octal) in CFSTR()."),
                    ms);
                CFRelease(ms);
            }

            // Treat non-7 bit chars in CFSTR() as MacOSRoman, for compatibility
            result = CFStringCreateWithCString(kCFAllocatorSystemDefault, cStr, kCFStringEncodingMacRoman);
            if (result == NULL) {
                CF_FATAL_ERROR("Can't interpret CFSTR() as MacOS Roman, crashing...");
            }
            if (__CFStrIsEightBit(result)) {
                key = (char*)__CFStrContents(result) + __CFStrSkipAnyLengthByte(result);
            } else {
                // For some reason the string is not 8-bit!
                CFIndex length = strlen(cStr) + 1;
                key = (char*)CFAllocatorAllocate(kCFAllocatorSystemDefault, length, 0);
                strlcpy(key, cStr, length);
                // !!! We will leak key if the string is removed from the 
                //  table (or table is freed).
            }

            {
                CFStringRef resultToBeReleased = result;
                CFIndex count;
                CFSpinLock(&__CFCStrTableLock);
                count = CFDictionaryGetCount(__CFCStrTable);
                CFDictionaryAddValue(__CFCStrTable, key, result);
                if (CFDictionaryGetCount(__CFCStrTable) == count) {
                    // Add did nothing, someone already put it there.
                    result = (CFStringRef)CFDictionaryGetValue(__CFCStrTable, key);
                } else {
                    // TODO Generalize (_CFRuntimeMakeConst?).
                    CF_BASE(result)->_rc = 0;
                }
                CFSpinUnlock(&__CFCStrTableLock);

                // This either eliminates the extra retain on the freshly created string, 
                //  or frees it, if it was actually not inserted into the table.
                CFRelease(resultToBeReleased);
            }
        }
    }
    return result;
}
