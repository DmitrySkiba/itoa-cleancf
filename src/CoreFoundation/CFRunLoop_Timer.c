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

#define CF_VALIDATE_RLTIMER_ARG(rlt) \
    CF_VALIDATE_OBJECT_ARG(CF, rlt, __kCFRunLoopTimerTypeID)

#include <CoreFoundation/CFRunLoop.h>
#include <CoreFoundation/CFSet.h>
#include "CFInternal.h"
#include "CFRunLoop_Common.h"
#include <math.h>
#include <stdio.h>
#include <limits.h>

struct __CFRunLoopTimer {
    CFRuntimeBase _base;
    CFSpinLock_t _lock;
    CFRunLoopRef _runLoop;
    CFIndex _rlCount;
    CFIndex _order; // immutable
    int64_t _fireTSR; // TSR units
    int64_t _intervalTSR; // immutable; 0 means non-repeating; TSR units
    CFRunLoopTimerCallBack _callout; // immutable
    CFRunLoopTimerContext _context; // immutable, except invalidation
};

static CFTypeID __kCFRunLoopTimerTypeID = _kCFRuntimeNotATypeID;

static CFSpinLock_t __CFRLTFireTSRLock = CFSpinLockInit;

///////////////////////////////////////////////////////////////////// private

/* Bit 0 of the CF_INFO bits is used for firing state. */
CF_INLINE Boolean __CFRunLoopTimerIsFiring(CFRunLoopTimerRef rlt) {
    return (Boolean)_CFBitfieldGetValue(CF_INFO(rlt), 0, 0);
}
CF_INLINE void __CFRunLoopTimerSetFiring(CFRunLoopTimerRef rlt) {
    _CFBitfieldSetValue(CF_INFO(rlt), 0, 0, 1);
}
CF_INLINE void __CFRunLoopTimerUnsetFiring(CFRunLoopTimerRef rlt) {
    _CFBitfieldSetValue(CF_INFO(rlt), 0, 0, 0);
}

/* Bit 1 of the CF_INFO bits is used for fired-during-callout state. */
CF_INLINE Boolean __CFRunLoopTimerDidFire(CFRunLoopTimerRef rlt) {
    return (Boolean)_CFBitfieldGetValue(CF_INFO(rlt), 1, 1);
}
CF_INLINE void __CFRunLoopTimerSetDidFire(CFRunLoopTimerRef rlt) {
    _CFBitfieldSetValue(CF_INFO(rlt), 1, 1, 1);
}
CF_INLINE void __CFRunLoopTimerUnsetDidFire(CFRunLoopTimerRef rlt) {
    _CFBitfieldSetValue(CF_INFO(rlt), 1, 1, 0);
}

CF_INLINE void __CFRunLoopTimerLock(CFRunLoopTimerRef rlt) {
    CFSpinLock(&rlt->_lock);
}
CF_INLINE void __CFRunLoopTimerUnlock(CFRunLoopTimerRef rlt) {
    CFSpinUnlock(&rlt->_lock);
}

CF_INLINE void __CFRunLoopTimerFireTSRLock(void) {
    CFSpinLock(&__CFRLTFireTSRLock);
}
CF_INLINE void __CFRunLoopTimerFireTSRUnlock(void) {
    CFSpinUnlock(&__CFRLTFireTSRLock);
}

/*** CFRunLoopTimer class ***/

static CFStringRef __CFRunLoopTimerCopyDescription(CFTypeRef cf) { /* DOES CALLOUT */
    CFRunLoopTimerRef rlt = (CFRunLoopTimerRef)cf;
    CFStringRef result;
    CFStringRef contextDesc = NULL;
    int64_t fireTime;
    __CFRunLoopTimerFireTSRLock();
    fireTime = rlt->_fireTSR;
    __CFRunLoopTimerFireTSRUnlock();
    if (rlt->_context.copyDescription) {
        contextDesc = rlt->_context.copyDescription(rlt->_context.info);
    }
    if (!contextDesc) {
        contextDesc = CFStringCreateWithFormat(
            CFGetAllocator(rlt), 
            NULL, CFSTR("<CFRunLoopTimer context %p>"), 
            rlt->_context.info);
    }
    int64_t now2 = _CFReadTSR();
    CFAbsoluteTime now1 = CFAbsoluteTimeGetCurrent();
    void* addr = (void*)rlt->_callout;
    result = CFStringCreateWithFormat(
            CFGetAllocator(rlt),
            NULL, CFSTR("<CFRunLoopTimer %p [%p]>{valid = %s, interval = %0.09g, next fire date = %0.09g, order = %d, callout = %p, context = %@}"),
            cf, CFGetAllocator(rlt),
            __CFIsValid(rlt) ? "Yes" : "No",
            _CFTSRToTimeInterval(rlt->_intervalTSR),
            now1 + _CFTSRToTimeInterval(fireTime - now2),
            rlt->_order,
            addr,
            contextDesc);
    CFRelease(contextDesc);
    return result;
}

static void __CFRunLoopTimerDeallocate(CFTypeRef cf) { /* DOES CALLOUT */
    CFRunLoopTimerRef rlt = (CFRunLoopTimerRef)cf;
    CFRunLoopTimerInvalidate(rlt); /* DOES CALLOUT */
}

static const CFRuntimeClass __CFRunLoopTimerClass = {
    0,
    "CFRunLoopTimer",
    NULL, // init
    NULL, // copy
    __CFRunLoopTimerDeallocate,
    NULL, // equal
    NULL,
    NULL,
    __CFRunLoopTimerCopyDescription
};

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL void __CFRunLoopTimerInitialize(void) {
    __kCFRunLoopTimerTypeID = _CFRuntimeRegisterClass(&__CFRunLoopTimerClass);
}

CF_INTERNAL CFRunLoopRef __CFRunLoopTimerGetLoop(CFRunLoopTimerRef rlt) {
    return rlt->_runLoop;
}

CF_INTERNAL Boolean __CFRunLoopTimerFire(CFRunLoopRef rl, CFRunLoopTimerRef rlt) { /* DOES CALLOUT */
    Boolean timerHandled = false;
    int64_t oldFireTSR = 0;

    /* Fire a timer */
    CFRetain(rlt);
    __CFRunLoopTimerLock(rlt);
    if (__CFIsValid(rlt) && !__CFRunLoopTimerIsFiring(rlt)) {
        __CFRunLoopTimerUnsetDidFire(rlt);
        __CFRunLoopTimerSetFiring(rlt);
        __CFRunLoopTimerUnlock(rlt);
        __CFRunLoopTimerFireTSRLock();
        oldFireTSR = rlt->_fireTSR;
        __CFRunLoopTimerFireTSRUnlock();
        rlt->_callout(rlt, rlt->_context.info); /* CALLOUT */
        __CFRunLoopTimerUnsetFiring(rlt);
        timerHandled = true;
    } else {
        // If the timer fires while it is firing in a higher activiation,
        // it is not allowed to fire, but we have to remember that fact.
        // Later, if the timer's fire date is being handled manually, we
        // need to re-arm the kernel timer, since it has possibly already
        // fired (this firing which is being skipped, say) and the timer
        // will permanently stop if we completely drop this firing.
        if (__CFRunLoopTimerIsFiring(rlt)) {
            __CFRunLoopTimerSetDidFire(rlt);
        }
        __CFRunLoopTimerUnlock(rlt);
    }
    if (__CFIsValid(rlt) && timerHandled) {
        if (!rlt->_intervalTSR) {
            CFRunLoopTimerInvalidate(rlt); /* DOES CALLOUT */
        } else {
            /* This is just a little bit tricky: we want to support calling
             * CFRunLoopTimerSetNextFireDate() from within the callout and
             * honor that new time here if it is a later date, otherwise
             * it is completely ignored. */
            int64_t currentFireTSR;
            __CFRunLoopTimerFireTSRLock();
            currentFireTSR = rlt->_fireTSR;
            if (oldFireTSR < currentFireTSR) {
                /* Next fire TSR was set, and set to a date after the previous
                 * fire date, so we honor it. */
                if (__CFRunLoopTimerDidFire(rlt)) {
                    __CFRunLoopTimerUnsetDidFire(rlt);
                }
            } else {
                if ((uint64_t)LLONG_MAX <= (uint64_t)oldFireTSR + (uint64_t)rlt->_intervalTSR) {
                    currentFireTSR = LLONG_MAX;
                } else {
                    int64_t currentTSR = _CFReadTSR();
                    currentFireTSR = oldFireTSR;
                    while (currentFireTSR <= currentTSR) {
                        currentFireTSR += rlt->_intervalTSR;
                    }
                }
                rlt->_fireTSR = currentFireTSR;
            }
            __CFRunLoopTimerFireTSRUnlock();
        }
    }
    CFRelease(rlt);
    return timerHandled;
}

CF_INTERNAL int64_t __CFRunLoopTimerGetFireTSR(CFRunLoopTimerRef rlt) {
    int64_t fireTSR;
    {
        __CFRunLoopTimerFireTSRLock();
        fireTSR = rlt->_fireTSR;
        __CFRunLoopTimerFireTSRUnlock();
    }
    return fireTSR;
}

///////////////////////////////////////////////////////////////////// public

CFTypeID CFRunLoopTimerGetTypeID(void) {
    return __kCFRunLoopTimerTypeID;
}

CFRunLoopTimerRef CFRunLoopTimerCreate(CFAllocatorRef allocator, 
                                       CFAbsoluteTime fireDate, 
                                       CFTimeInterval interval, 
                                       CFOptionFlags flags, 
                                       CFIndex order, 
                                       CFRunLoopTimerCallBack callout, 
                                       CFRunLoopTimerContext* context)
{
    CFRunLoopTimerRef instance = (CFRunLoopTimerRef)_CFRuntimeCreateInstance(
        allocator, 
        __kCFRunLoopTimerTypeID,
        sizeof(struct __CFRunLoopTimer) - sizeof(CFRuntimeBase), 
        NULL);
    if (!instance) {
        return NULL;
    }
    __CFSetValid(instance);
    __CFRunLoopTimerUnsetFiring(instance);
    __CFRunLoopTimerUnsetDidFire(instance);
    instance->_lock = CFSpinLockInit;
    instance->_runLoop = NULL;
    instance->_rlCount = 0;
    instance->_order = order;
    int64_t now2 = _CFReadTSR();
    CFAbsoluteTime now1 = CFAbsoluteTimeGetCurrent();
    if (fireDate > __CFRunLoopInfiniteWait) {
        fireDate = __CFRunLoopInfiniteWait;
    }
    if (fireDate < now1) {
        instance->_fireTSR = now2;
    } else if (now1 + _CFTSRToTimeInterval(LLONG_MAX) < fireDate) {
        instance->_fireTSR = LLONG_MAX;
    } else {
        instance->_fireTSR = now2 + _CFTimeIntervalToTSR(fireDate - now1);
    }
    if (interval > __CFRunLoopInfiniteWait) {
        interval = __CFRunLoopInfiniteWait;
    }
    if (interval <= 0.0) {
        instance->_intervalTSR = 0;
    } else if (_CFTSRToTimeInterval(LLONG_MAX) < interval) {
        instance->_intervalTSR = LLONG_MAX;
    } else {
        instance->_intervalTSR = _CFTimeIntervalToTSR(interval);
    }
    instance->_callout = callout;
    if (context) {
        if (context->retain) {
            instance->_context.info = (void*)context->retain(context->info);
        } else {
            instance->_context.info = context->info;
        }
        instance->_context.retain = context->retain;
        instance->_context.release = context->release;
        instance->_context.copyDescription = context->copyDescription;
    } else {
        instance->_context.info = 0;
        instance->_context.retain = 0;
        instance->_context.release = 0;
        instance->_context.copyDescription = 0;
    }
    return instance;
}

CFAbsoluteTime CFRunLoopTimerGetNextFireDate(CFRunLoopTimerRef rlt) {
    int64_t fireTime, result = 0;
    //TODO _cffireTime:(CFAbsoluteTime*)
    //CF_OBJC_FUNCDISPATCH(CFAbsoluteTime, rlt, "_cffireTime");
    CF_VALIDATE_RLTIMER_ARG(rlt);
    __CFRunLoopTimerFireTSRLock();
    fireTime = rlt->_fireTSR;
    __CFRunLoopTimerFireTSRUnlock();
    __CFRunLoopTimerLock(rlt);
    if (__CFIsValid(rlt)) {
        result = fireTime;
    }
    __CFRunLoopTimerUnlock(rlt);
    int64_t now2 = _CFReadTSR();
    CFAbsoluteTime now1 = CFAbsoluteTimeGetCurrent();
    return (!result) ? 0.0 : now1 + _CFTSRToTimeInterval(result - now2);
}

void CFRunLoopTimerSetNextFireDate(CFRunLoopTimerRef rlt, CFAbsoluteTime fireDate) {
    __CFRunLoopTimerFireTSRLock();
    int64_t now2 = _CFReadTSR();
    CFAbsoluteTime now1 = CFAbsoluteTimeGetCurrent();
    if (fireDate > __CFRunLoopInfiniteWait) {
        fireDate = __CFRunLoopInfiniteWait;
    }
    if (fireDate < now1) {
        rlt->_fireTSR = now2;
    } else if (now1 + _CFTSRToTimeInterval(LLONG_MAX) < fireDate) {
        rlt->_fireTSR = LLONG_MAX;
    } else {
        rlt->_fireTSR = now2 + _CFTimeIntervalToTSR(fireDate - now1);
    }
    __CFRunLoopTimerFireTSRUnlock();
}

CFTimeInterval CFRunLoopTimerGetInterval(CFRunLoopTimerRef rlt) {
    //TODO _cfTimeInterval:(CFTimeInterval*)
    //CF_OBJC_FUNCDISPATCH(CFTimeInterval, rlt, "timeInterval");
    CF_VALIDATE_RLTIMER_ARG(rlt);
    return _CFTSRToTimeInterval(rlt->_intervalTSR);
}

Boolean CFRunLoopTimerDoesRepeat(CFRunLoopTimerRef rlt) {
    CF_VALIDATE_RLTIMER_ARG(rlt);
    return (rlt->_intervalTSR != 0);
}

CFIndex CFRunLoopTimerGetOrder(CFRunLoopTimerRef rlt) {
    CF_OBJC_FUNCDISPATCH(CFIndex, rlt, "order");
    CF_VALIDATE_RLTIMER_ARG(rlt);
    return rlt->_order;
}

void CFRunLoopTimerInvalidate(CFRunLoopTimerRef rlt) {  /* DOES CALLOUT */
    CF_OBJC_VOID_FUNCDISPATCH(rlt, "invalidate");
    CF_VALIDATE_RLTIMER_ARG(rlt);
    CFRetain(rlt);
    __CFRunLoopTimerLock(rlt);
    if (__CFIsValid(rlt)) {
        CFRunLoopRef rl = rlt->_runLoop;
        void* info = rlt->_context.info;
        __CFUnsetValid(rlt);
        rlt->_context.info = NULL;
        __CFRunLoopTimerUnlock(rlt);
        if (rl) {
            CFArrayRef array;
            CFIndex idx;
            array = CFRunLoopCopyAllModes(rl);
            for (idx = CFArrayGetCount(array); idx--; ) {
                CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(array, idx);
                CFRunLoopRemoveTimer(rl, rlt, modeName);
            }
            CFRunLoopRemoveTimer(rl, rlt, kCFRunLoopCommonModes);
            CFRelease(array);
        }
        if (rlt->_context.release) {
            rlt->_context.release(info); /* CALLOUT */
        }
    } else {
        __CFRunLoopTimerUnlock(rlt);
    }
    CFRelease(rlt);
}

Boolean CFRunLoopTimerIsValid(CFRunLoopTimerRef rlt) {
    CF_OBJC_FUNCDISPATCH(Boolean, rlt, "isValid");
    CF_VALIDATE_RLTIMER_ARG(rlt);
    return __CFIsValid(rlt);
}

void CFRunLoopTimerGetContext(CFRunLoopTimerRef rlt, CFRunLoopTimerContext* context) {
    CF_VALIDATE_RLTIMER_ARG(rlt);
    CF_VALIDATE_ARG(context->version == 0,
        "context version not initialized to 0");

    *context = rlt->_context;
}
