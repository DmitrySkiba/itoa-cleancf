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
#include <math.h>
#include <stdio.h>
#include <limits.h>

#define CF_VALIDATE_RLSOURCE_ARG(rls) \
    CF_VALIDATE_OBJECT_ARG(CF, rls, __kCFRunLoopSourceTypeID)

struct __CFRunLoopSource {
    CFRuntimeBase _base;
    uint32_t _bits;
    CFSpinLock_t _lock;
    CFIndex _order; // immutable
    CFMutableBagRef _runLoops;
    union {
        CFRunLoopSourceContext version0; // immutable, except invalidation
        CFRunLoopSourceContext1 version1; // immutable, except invalidation
    } _context;
};

static CFTypeID __kCFRunLoopSourceTypeID = _kCFRuntimeNotATypeID;

///////////////////////////////////////////////////////////////////// private

CF_INLINE Boolean __CFRunLoopSourceIsSignaled(CFRunLoopSourceRef rls) {
    return (Boolean)_CFBitfieldGetValue(rls->_bits, 1, 1);
}
CF_INLINE void __CFRunLoopSourceSetSignaled(CFRunLoopSourceRef rls) {
    _CFBitfieldSetValue(rls->_bits, 1, 1, 1);
}
CF_INLINE void __CFRunLoopSourceUnsetSignaled(CFRunLoopSourceRef rls) {
    _CFBitfieldSetValue(rls->_bits, 1, 1, 0);
}

CF_INLINE void __CFRunLoopSourceLock(CFRunLoopSourceRef rls) {
    CFSpinLock(&rls->_lock);
}

CF_INLINE void __CFRunLoopSourceUnlock(CFRunLoopSourceRef rls) {
    CFSpinUnlock(&rls->_lock);
}

static void __CFRunLoopSourceRemoveFromRunLoop(const void* value, void* context) {
    CFRunLoopRef rl = (CFRunLoopRef)value;
    CFTypeRef* params = (CFTypeRef*)context;
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)params[0];
    CFArrayRef array;
    CFIndex idx;
    if (rl == params[1]) {
        return;
    }
    array = CFRunLoopCopyAllModes(rl);
    for (idx = CFArrayGetCount(array); idx--; ) {
        CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(array, idx);
        CFRunLoopRemoveSource(rl, rls, modeName);
    }
    CFRunLoopRemoveSource(rl, rls, kCFRunLoopCommonModes);
    CFRelease(array);
    params[1] = rl;
}

/*** CFRunLoopSource class ***/

static Boolean __CFRunLoopSourceEqual(CFTypeRef cf1, CFTypeRef cf2) { /* DOES CALLOUT */
    CFRunLoopSourceRef rls1 = (CFRunLoopSourceRef)cf1;
    CFRunLoopSourceRef rls2 = (CFRunLoopSourceRef)cf2;
    if (rls1 == rls2) {
        return true;
    }
    if (rls1->_order != rls2->_order) {
        return false;
    }
    if (rls1->_context.version0.version != rls2->_context.version0.version) {
        return false;
    }
    if (rls1->_context.version0.hash != rls2->_context.version0.hash) {
        return false;
    }
    if (rls1->_context.version0.equal != rls2->_context.version0.equal) {
        return false;
    }
    if (rls1->_context.version0.version == 0 &&
        rls1->_context.version0.perform != rls2->_context.version0.perform)
    {
        return false;
    }
    if (rls1->_context.version1.version == 1 &&
        rls1->_context.version1.perform != rls2->_context.version1.perform)
    {
        return false;
    }
    if (rls1->_context.version0.equal) {
        return rls1->_context.version0.equal(
            rls1->_context.version0.info,
            rls2->_context.version0.info);
    }
    return (rls1->_context.version0.info == rls2->_context.version0.info);
}

static CFHashCode __CFRunLoopSourceHash(CFTypeRef cf) { /* DOES CALLOUT */
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)cf;
    if (rls->_context.version0.hash) {
        return rls->_context.version0.hash(rls->_context.version0.info);
    }
    return (CFHashCode)rls->_context.version0.info;
}

static CFStringRef __CFRunLoopSourceCopyDescription(CFTypeRef cf) { /* DOES CALLOUT */
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)cf;
    CFStringRef result;
    CFStringRef contextDesc = NULL;
    if (rls->_context.version0.copyDescription) {
        contextDesc = rls->_context.version0.copyDescription(rls->_context.version0.info);
    }
    if (!contextDesc) {
        void* addr = NULL;
        if (rls->_context.version0.version == 0) {
            addr = (void*)rls->_context.version0.perform;
        } else if (rls->_context.version1.version == 1) {
            addr = (void*)rls->_context.version1.perform;
        }
        contextDesc = CFStringCreateWithFormat(
            CFGetAllocator(rls), 
            NULL, CFSTR("<CFRunLoopSource context>{version = %ld, info = %p, callout = %p}"), 
            rls->_context.version0.version, 
            rls->_context.version0.info, 
            addr);
    }
    result = CFStringCreateWithFormat(
        CFGetAllocator(rls), 
        NULL, CFSTR("<CFRunLoopSource %p [%p]>{signalled = %s, valid = %s, order = %d, context = %@}"), 
        cf, CFGetAllocator(rls), 
        (__CFRunLoopSourceIsSignaled(rls) ? "Yes" : "No"), 
        (__CFIsValid(rls) ? "Yes" : "No"), 
        rls->_order, 
        contextDesc);
    CFRelease(contextDesc);
    return result;
}

static void __CFRunLoopSourceDeallocate(CFTypeRef cf) { /* DOES CALLOUT */
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)cf;
    CFRunLoopSourceInvalidate(rls);
    if (rls->_context.version0.release) {
        rls->_context.version0.release(rls->_context.version0.info);
    }
}

static const CFRuntimeClass __CFRunLoopSourceClass = {
    0,
    "CFRunLoopSource",
    NULL, // init
    NULL, // copy
    __CFRunLoopSourceDeallocate,
    __CFRunLoopSourceEqual,
    __CFRunLoopSourceHash,
    NULL,
    __CFRunLoopSourceCopyDescription
};

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL CFRunLoopPortRef __CFRunLoopSourceGetPort(CFRunLoopSourceRef rls) { /* DOES CALLOUT */
    if (rls->_context.version1.version == 1) {
        return rls->_context.version1.getPort(rls->_context.version1.info); /* CALLOUT */
    } else {
        return NULL;
    }
}

/* rlm is not locked */
CF_INTERNAL void __CFRunLoopSourceSchedule(CFRunLoopSourceRef rls, CFRunLoopRef rl, CFRunLoopModeRef rlm) {    /* DOES CALLOUT */
    __CFRunLoopSourceLock(rls);
    if (!rls->_runLoops) {
        rls->_runLoops = CFBagCreateMutable(CFGetAllocator(rls), 0, NULL);
    }
    CFBagAddValue(rls->_runLoops, rl);
    __CFRunLoopSourceUnlock(rls);
    // Have to unlock before the callout -- cannot help clients with safety.

    if (!rls->_context.version0.version) {
        if (rls->_context.version0.schedule) {
            rls->_context.version0.schedule(rls->_context.version0.info, rl, _CFRunLoopModeGetName(rlm));
        }
    } else if (1 == rls->_context.version0.version) {
        CFRunLoopPortRef port = rls->_context.version1.getPort(rls->_context.version1.info); /* CALLOUT */
        if (port) {
            _CFRunLoopModeAddPort(rlm, port);
        }
    }
}

/* rlm is not locked */
CF_INTERNAL void __CFRunLoopSourceCancel(CFRunLoopSourceRef rls, CFRunLoopRef rl, CFRunLoopModeRef rlm) {      /* DOES CALLOUT */
    if (!rls->_context.version0.version) {
        if (rls->_context.version0.cancel) {
            rls->_context.version0.cancel(rls->_context.version0.info, rl, _CFRunLoopModeGetName(rlm)); /* CALLOUT */
        }
    } else if (1 == rls->_context.version0.version) {
        CFRunLoopPortRef port = rls->_context.version1.getPort(rls->_context.version1.info); /* CALLOUT */
        if (port) {
            _CFRunLoopModeRemovePort(rlm, port);
        }
    }
    __CFRunLoopSourceLock(rls);
    if (rls->_runLoops) {
        CFBagRemoveValue(rls->_runLoops, rl);
    }
    __CFRunLoopSourceUnlock(rls);
}

CF_INTERNAL CFComparisonResult __CFRunLoopSourceComparator(const void* val1, const void* val2, void* context) {
    CFRunLoopSourceRef o1 = (CFRunLoopSourceRef)val1;
    CFRunLoopSourceRef o2 = (CFRunLoopSourceRef)val2;
    if (o1->_order < o2->_order) {
        return kCFCompareLessThan;
    }
    if (o2->_order < o1->_order) {
        return kCFCompareGreaterThan;
    }
    return kCFCompareEqualTo;
}

CF_INTERNAL Boolean _CFRunLoopSource0Perform(CFRunLoopSourceRef rls) {
    __CFRunLoopSourceLock(rls);
    __CFRunLoopSourceUnsetSignaled(rls);
    if (__CFIsValid(rls)) {
        __CFRunLoopSourceUnlock(rls);
        if (rls->_context.version0.perform) {
            rls->_context.version0.perform(rls->_context.version0.info); /* CALLOUT */
        }
        return true;
    } else {
        __CFRunLoopSourceUnlock(rls);
        return false;
    }
}

CF_INTERNAL Boolean _CFRunLoopSource0IsSignalled(CFRunLoopSourceRef rls) {
    return __CFIsValid(rls) &&
        rls->_context.version0.version == 0 &&
        __CFRunLoopSourceIsSignaled(rls);
}

CF_INTERNAL Boolean _CFRunLoopSource1Perform(CFRunLoopSourceRef rls) {
    __CFRunLoopSourceLock(rls);
    if (__CFIsValid(rls)) {
        __CFRunLoopSourceUnsetSignaled(rls);
        __CFRunLoopSourceUnlock(rls);
        if (rls->_context.version1.perform) {
            if (_LogCFRunLoop) {
                CFLog(kCFLogLevelDebug, CFSTR("%p __CFRunLoopDoSource1 performing rls %p"), CFRunLoopGetCurrent(), rls);
            }
            rls->_context.version1.perform(rls->_context.version1.info); /* CALLOUT */
        } else {
            if (_LogCFRunLoop) {
                CFLog(kCFLogLevelDebug, CFSTR("%p __CFRunLoopDoSource1 perform is NULL"), CFRunLoopGetCurrent());
            }
        }
        return true;
    } else {
        if (_LogCFRunLoop) {
            CFLog(kCFLogLevelDebug, CFSTR("%p __CFRunLoopDoSource1 rls %p is invalid"), CFRunLoopGetCurrent(), rls);
        }
        __CFRunLoopSourceUnlock(rls);
        return false;
    }
}

CF_INTERNAL void __CFRunLoopSourceInitialize(void) {
    __kCFRunLoopSourceTypeID = _CFRuntimeRegisterClass(&__CFRunLoopSourceClass);
}

///////////////////////////////////////////////////////////////////// public

CFTypeID CFRunLoopSourceGetTypeID(void) {
    return __kCFRunLoopSourceTypeID;
}

CFRunLoopSourceRef CFRunLoopSourceCreate(CFAllocatorRef allocator, CFIndex order, CFRunLoopSourceContext* context) {
    CF_VALIDATE_PTR_ARG(context);

    CFRunLoopSourceRef memory;
    uint32_t size;
    size = sizeof(struct __CFRunLoopSource) - sizeof(CFRuntimeBase);
    memory = (CFRunLoopSourceRef)_CFRuntimeCreateInstance(allocator, __kCFRunLoopSourceTypeID, size, NULL);
    if (!memory) {
        return NULL;
    }
    __CFSetValid(memory);
    __CFRunLoopSourceUnsetSignaled(memory);
    memory->_lock = CFSpinLockInit;
    memory->_bits = 0;
    memory->_order = order;
    memory->_runLoops = NULL;
    size = 0;
    switch (context->version) {
        case 0:
            size = sizeof(CFRunLoopSourceContext);
            break;
        case 1:
            size = sizeof(CFRunLoopSourceContext1);
            break;
    }
    memmove(&memory->_context, context, size);
    if (context->retain) {
        memory->_context.version0.info = (void*)context->retain(context->info);
    }
    return memory;
}

CFIndex CFRunLoopSourceGetOrder(CFRunLoopSourceRef rls) {
    CF_VALIDATE_RLSOURCE_ARG(rls);
    return rls->_order;
}

void CFRunLoopSourceInvalidate(CFRunLoopSourceRef rls) {
    CF_VALIDATE_RLSOURCE_ARG(rls);
    CFRetain(rls);
    __CFRunLoopSourceLock(rls);
    if (__CFIsValid(rls)) {
        __CFUnsetValid(rls);
        __CFRunLoopSourceUnsetSignaled(rls);
        if (rls->_runLoops) {
            CFTypeRef params[2] = {rls, NULL};
            CFBagRef bag = CFBagCreateCopy(kCFAllocatorSystemDefault, rls->_runLoops);
            CFRelease(rls->_runLoops);
            rls->_runLoops = NULL;
            __CFRunLoopSourceUnlock(rls);
            CFBagApplyFunction(bag, (__CFRunLoopSourceRemoveFromRunLoop), params);
            CFRelease(bag);
        } else {
            __CFRunLoopSourceUnlock(rls);
        }
        /* for hashing- and equality-use purposes, can't actually release the context here */
    } else {
        __CFRunLoopSourceUnlock(rls);
    }
    CFRelease(rls);
}

Boolean CFRunLoopSourceIsValid(CFRunLoopSourceRef rls) {
    CF_VALIDATE_RLSOURCE_ARG(rls);
    return __CFIsValid(rls);
}

void CFRunLoopSourceGetContext(CFRunLoopSourceRef rls, CFRunLoopSourceContext* context) {
    CF_VALIDATE_RLSOURCE_ARG(rls);
    CF_VALIDATE_ARG(context->version == 0 || context->version == 1,
        "context version not initialized to 0 or 1");

    CFIndex size = 0;
    switch (context->version) {
        case 0:
            size = sizeof(CFRunLoopSourceContext);
            break;
        case 1:
            size = sizeof(CFRunLoopSourceContext1);
            break;
    }
    memmove(context, &rls->_context, size);
}

void CFRunLoopSourceSignal(CFRunLoopSourceRef rls) {
    __CFRunLoopSourceLock(rls);
    if (__CFIsValid(rls)) {
        __CFRunLoopSourceSetSignaled(rls);
    }
    __CFRunLoopSourceUnlock(rls);
}

Boolean CFRunLoopSourceIsSignalled(CFRunLoopSourceRef rls) {
    __CFRunLoopSourceLock(rls);
    Boolean result = __CFRunLoopSourceIsSignaled(rls);
    __CFRunLoopSourceUnlock(rls);
    return result;
}


