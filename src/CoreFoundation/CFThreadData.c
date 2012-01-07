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

#include <CoreFoundation/CFAllocator.h>
#include <string.h>
#include "CFInternal.h"

//TODO: merge CFThreadData to to CFRuntime.c

static pthread_key_t __CFTSDKey = (pthread_key_t)NULL;

/* Called for each thread as it exits.
 */
static void __CFFinalizeThreadData(void* arg) {
    _CFThreadSpecificData* tsd = (_CFThreadSpecificData*)arg;
    if (!tsd) {
        return;
    }
    if (tsd->_allocator) {
        CFRelease(tsd->_allocator);
    }
    _CFFinalizeCurrentRunLoop();
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, tsd);
}

CF_INTERNAL _CFThreadSpecificData* _CFGetThreadSpecificData(void) {
    _CFThreadSpecificData* data = (_CFThreadSpecificData*)pthread_getspecific(__CFTSDKey);
    if (data) {
        return data;
    }
    data = (_CFThreadSpecificData*)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(_CFThreadSpecificData), 0);
    memset(data, 0, sizeof(_CFThreadSpecificData));
    pthread_setspecific(__CFTSDKey, data);
    return data;
}

CF_INTERNAL void _CFThreadDataInitialize(void) {
    pthread_key_create(&__CFTSDKey, __CFFinalizeThreadData);
}
