/*
 * Copyright (c) 2011 Dmitry Skiba
 * Copyright (c) 2008-2009 Brent Fulgham <bfulgham@gmail.org>.  All rights reserved.
 * Copyright (c) 2009 Grant Erickson <gerickson@nuovations.com>. All rights reserved.
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

#include <CoreFoundation/CFRunLoop.h>
#include <CoreFoundation/CFSet.h>
#include "CFInternal.h"
#include "CFRunLoop_Common.h"
#include <CoreFoundation/CFSortFunctions.h>
#include <math.h>
#include <stdio.h>
#include <limits.h>

#define CF_VALIDATE_RLOBSERVER_ARG(rlo) \
    CF_VALIDATE_OBJECT_ARG(CF, rlo, __kCFRunLoopObserverTypeID)

static CFTypeID __kCFRunLoopObserverTypeID = _kCFRuntimeNotATypeID;

struct __CFRunLoopObserver {
    CFRuntimeBase _base;
    CFSpinLock_t _lock;
    CFRunLoopRef _runLoop;
    CFIndex _rlCount;
    CFOptionFlags _activities;          /* immutable */
    CFIndex _order;                     /* immutable */
    CFRunLoopObserverCallBack _callout; /* immutable */
    CFRunLoopObserverContext _context;  /* immutable, except invalidation */
};

///////////////////////////////////////////////////////////////////// private

/* Bit 0 of CF_INFO is used for firing state.
 */
CF_INLINE Boolean __CFRunLoopObserverIsFiring(CFRunLoopObserverRef rlo) {
    return (Boolean)_CFBitfieldGetValue(CF_INFO(rlo), 0, 0);
}
CF_INLINE void __CFRunLoopObserverSetFiring(CFRunLoopObserverRef rlo) {
    _CFBitfieldSetValue(CF_INFO(rlo), 0, 0, 1);
}
CF_INLINE void __CFRunLoopObserverUnsetFiring(CFRunLoopObserverRef rlo) {
    _CFBitfieldSetValue(CF_INFO(rlo), 0, 0, 0);
}

/* Bit 1 of CF_INFO is used for repeats state.
 */
CF_INLINE Boolean __CFRunLoopObserverRepeats(CFRunLoopObserverRef rlo) {
    return (Boolean)_CFBitfieldGetValue(CF_INFO(rlo), 1, 1);
}
CF_INLINE void __CFRunLoopObserverSetRepeats(CFRunLoopObserverRef rlo) {
    _CFBitfieldSetValue(CF_INFO(rlo), 1, 1, 1);
}
CF_INLINE void __CFRunLoopObserverUnsetRepeats(CFRunLoopObserverRef rlo) {
    _CFBitfieldSetValue(CF_INFO(rlo), 1, 1, 0);
}

CF_INLINE void __CFRunLoopObserverLock(CFRunLoopObserverRef rlo) {
    CFSpinLock(&rlo->_lock);
}
CF_INLINE void __CFRunLoopObserverUnlock(CFRunLoopObserverRef rlo) {
    CFSpinUnlock(&rlo->_lock);
}

/*** CFRunLoopObserver class ***/

static CFStringRef __CFRunLoopObserverCopyDescription(CFTypeRef cf) { /* DOES CALLOUT */
    CFRunLoopObserverRef rlo = (CFRunLoopObserverRef)cf;
    CFStringRef result;
    CFStringRef contextDesc = NULL;
    if (rlo->_context.copyDescription) {
        contextDesc = rlo->_context.copyDescription(rlo->_context.info);
    }
    if (!contextDesc) {
        contextDesc = CFStringCreateWithFormat(
            CFGetAllocator(rlo), 
            NULL, CFSTR("<CFRunLoopObserver context %p>"), 
            rlo->_context.info);
    }
    result = CFStringCreateWithFormat(
        CFGetAllocator(rlo),
        NULL, CFSTR("<CFRunLoopObserver %p [%p]>{"
            "valid = %s, activities = 0x%x, repeats = %s, "
            "order = %d, callout = %p, context = %@}"),
        cf, CFGetAllocator(rlo),
        (__CFIsValid(rlo) ? "Yes" : "No"),
        rlo->_activities,
        (__CFRunLoopObserverRepeats(rlo) ? "Yes" : "No"),
        rlo->_order,
        rlo->_callout,
        contextDesc);
    CFRelease(contextDesc);
    return result;
}

static void __CFRunLoopObserverDeallocate(CFTypeRef cf) { /* DOES CALLOUT */
    CFRunLoopObserverRef rlo = (CFRunLoopObserverRef)cf;
    CFRunLoopObserverInvalidate(rlo);
}

static const CFRuntimeClass __CFRunLoopObserverClass = {
    0,
    "CFRunLoopObserver",
    NULL, // init
    NULL, // copy
    __CFRunLoopObserverDeallocate,
    NULL,
    NULL,
    NULL,
    __CFRunLoopObserverCopyDescription
};

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL void __CFRunLoopObserverInitialize(void) {
    __kCFRunLoopObserverTypeID = _CFRuntimeRegisterClass(&__CFRunLoopObserverClass);
}

CF_INTERNAL CFRunLoopRef __CFRunLoopObserverGetLoop(CFRunLoopObserverRef rlo) {
    return rlo->_runLoop;
}

CF_INTERNAL void __CFRunLoopObserverSchedule(CFRunLoopObserverRef rlo, CFRunLoopRef rl) {
    __CFRunLoopObserverLock(rlo);
    if (!rlo->_rlCount) {
        rlo->_runLoop = rl;
    }
    rlo->_rlCount++;
    __CFRunLoopObserverUnlock(rlo);
}

CF_INTERNAL void __CFRunLoopObserverCancel(CFRunLoopObserverRef rlo, CFRunLoopRef rl) {
    __CFRunLoopObserverLock(rlo);
    rlo->_rlCount--;
    if (!rlo->_rlCount) {
        rlo->_runLoop = NULL;
    }
    __CFRunLoopObserverUnlock(rlo);
}

CF_INTERNAL Boolean _CFRunLoopObserverCanFire(CFRunLoopObserverRef rlo, CFRunLoopActivity activity) {
    return (rlo->_activities & activity) &&
           __CFIsValid(rlo) &&
           !__CFRunLoopObserverIsFiring(rlo);
}

CF_INTERNAL void _CFRunLoopObserversFire(CFRunLoopActivity activity, CFRunLoopObserverRef* collectedObservers, CFIndex cnt) {
    CFIndex idx;
    CFQSortArray(collectedObservers, cnt, sizeof(CFRunLoopObserverRef), __CFRunLoopObserverQSortComparator, NULL);
    for (idx = 0; idx < cnt; idx++) {
        CFRunLoopObserverRef rlo = collectedObservers[idx];
        if (rlo) {
            __CFRunLoopObserverLock(rlo);
            if (__CFIsValid(rlo)) {
                __CFRunLoopObserverUnlock(rlo);
                __CFRunLoopObserverSetFiring(rlo);
                rlo->_callout(rlo, activity, rlo->_context.info); /* CALLOUT */
                __CFRunLoopObserverUnsetFiring(rlo);
                if (!__CFRunLoopObserverRepeats(rlo)) {
                    CFRunLoopObserverInvalidate(rlo);
                }
            } else {
                __CFRunLoopObserverUnlock(rlo);
            }
            CFRelease(rlo);
        }
    }
}

CF_INTERNAL void _CFRunLoopObserverFire(CFRunLoopActivity activity, CFRunLoopObserverRef rlo) {
    __CFRunLoopObserverLock(rlo);
    if (__CFIsValid(rlo)) {
        __CFRunLoopObserverUnlock(rlo);
        __CFRunLoopObserverSetFiring(rlo);
        rlo->_callout(rlo, activity, rlo->_context.info); /* CALLOUT */
        __CFRunLoopObserverUnsetFiring(rlo);
        if (!__CFRunLoopObserverRepeats(rlo)) {
            CFRunLoopObserverInvalidate(rlo);
        }
    } else {
        __CFRunLoopObserverUnlock(rlo);
    }
}

CF_INTERNAL CFComparisonResult __CFRunLoopObserverQSortComparator(const void* val1, const void* val2, void* context) {
    CFRunLoopObserverRef o1 = *((CFRunLoopObserverRef*)val1);
    CFRunLoopObserverRef o2 = *((CFRunLoopObserverRef*)val2);
    if (!o1) {
        return (!o2) ? kCFCompareEqualTo : kCFCompareLessThan;
    }
    if (!o2) {
        return kCFCompareGreaterThan;
    }
    if (o1->_order < o2->_order) {
        return kCFCompareLessThan;
    }
    if (o2->_order < o1->_order) {
        return kCFCompareGreaterThan;
    }
    return kCFCompareEqualTo;
}

///////////////////////////////////////////////////////////////////// public

CFTypeID CFRunLoopObserverGetTypeID(void) {
    return __kCFRunLoopObserverTypeID;
}

CFRunLoopObserverRef CFRunLoopObserverCreate(CFAllocatorRef allocator,
                                             CFOptionFlags activities,
                                             Boolean repeats,
                                             CFIndex order,
                                             CFRunLoopObserverCallBack callout,
                                             CFRunLoopObserverContext* context)
{
    CFRunLoopObserverRef memory;
    UInt32 size;
    size = sizeof(struct __CFRunLoopObserver) - sizeof(CFRuntimeBase);
    memory = (CFRunLoopObserverRef)_CFRuntimeCreateInstance(allocator, __kCFRunLoopObserverTypeID, size, NULL);
    if (!memory) {
        return NULL;
    }
    __CFSetValid(memory);
    __CFRunLoopObserverUnsetFiring(memory);
    if (repeats) {
        __CFRunLoopObserverSetRepeats(memory);
    } else {
        __CFRunLoopObserverUnsetRepeats(memory);
    }
    memory->_lock = CFSpinLockInit;
    memory->_runLoop = NULL;
    memory->_rlCount = 0;
    memory->_activities = activities;
    memory->_order = order;
    memory->_callout = callout;
    if (context) {
        if (context->retain) {
            memory->_context.info = (void*)context->retain(context->info);
        } else {
            memory->_context.info = context->info;
        }
        memory->_context.retain = context->retain;
        memory->_context.release = context->release;
        memory->_context.copyDescription = context->copyDescription;
    } else {
        memory->_context.info = 0;
        memory->_context.retain = 0;
        memory->_context.release = 0;
        memory->_context.copyDescription = 0;
    }
    return memory;
}

CFOptionFlags CFRunLoopObserverGetActivities(CFRunLoopObserverRef rlo) {
    CF_VALIDATE_RLOBSERVER_ARG(rlo);
    return rlo->_activities;
}

CFIndex CFRunLoopObserverGetOrder(CFRunLoopObserverRef rlo) {
    CF_VALIDATE_RLOBSERVER_ARG(rlo);
    return rlo->_order;
}

Boolean CFRunLoopObserverDoesRepeat(CFRunLoopObserverRef rlo) {
    CF_VALIDATE_RLOBSERVER_ARG(rlo);
    return __CFRunLoopObserverRepeats(rlo);
}

void CFRunLoopObserverInvalidate(CFRunLoopObserverRef rlo) { /* DOES CALLOUT */
    CF_VALIDATE_RLOBSERVER_ARG(rlo);
    CFRetain(rlo);
    __CFRunLoopObserverLock(rlo);
    if (__CFIsValid(rlo)) {
        CFRunLoopRef rl = rlo->_runLoop;
        __CFUnsetValid(rlo);
        __CFRunLoopObserverUnlock(rlo);
        if (rl) {
            CFArrayRef array;
            CFIndex idx;
            array = CFRunLoopCopyAllModes(rl);
            for (idx = CFArrayGetCount(array); idx--; ) {
                CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(array, idx);
                CFRunLoopRemoveObserver(rl, rlo, modeName);
            }
            CFRunLoopRemoveObserver(rl, rlo, kCFRunLoopCommonModes);
            CFRelease(array);
        }
        if (rlo->_context.release) {
            rlo->_context.release(rlo->_context.info); /* CALLOUT */
        }
        rlo->_context.info = NULL;
    } else {
        __CFRunLoopObserverUnlock(rlo);
    }
    CFRelease(rlo);
}

Boolean CFRunLoopObserverIsValid(CFRunLoopObserverRef rlo) {
    return __CFIsValid(rlo);
}

void CFRunLoopObserverGetContext(CFRunLoopObserverRef rlo, CFRunLoopObserverContext* context) {
    CF_VALIDATE_RLOBSERVER_ARG(rlo);
    CF_VALIDATE_ARG(context->version == 0,
        "context version not initialized to 0");

    *context = rlo->_context;
}
