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
#include <CoreFoundation/CFBag.h>
#include "CFInternal.h"
#include "CFRunLoop_Common.h"
#include <math.h>
#include <stdio.h>
#include <limits.h>
#include <CoreFoundation/CFSortFunctions.h>

//TODO Iterator function must return NO to stop
//TODO __CFRunLoops dictionary must retain values
//TODO decide on __CFRunLoopModeEqual __CFRunLoopModeHash - are they
//     still needed?
//TODO get rid of __CFRunLoop::_stopped array, boolean is enough

//XXX move up
extern void __CFRunLoopPortInitialize();

int _LogCFRunLoop = 1;

////////////////////////////////////////////////////////////////////////////////////////////////

/* unlock a run loop and modes before doing callouts/sleeping */
/* never try to take the run loop lock with a mode locked */
/* be very careful of common subexpression elimination and compacting code, particular across locks and unlocks! */
/* run loop mode structures should never be deallocated, even if they become empty */

struct __CFRunLoopMode {
    CFRuntimeBase _base;
    CFSpinLock_t _lock; /* must have the run loop locked before locking this */
    CFStringRef _name;
    Boolean _stopped;
    char _padding[3];
    CFMutableSetRef _sources;
    CFMutableSetRef _observers;
    CFMutableSetRef _timers;
    CFMutableArrayRef _submodes; // names of the submodes
    CFMutableArrayRef _portSet;
};

static CFTypeID __kCFRunLoopTypeID = _kCFRuntimeNotATypeID;
static CFTypeID __kCFRunLoopModeTypeID = _kCFRuntimeNotATypeID;

struct __CFRunLoop {
    CFRuntimeBase _base;
    CFSpinLock_t _lock; // locked for accessing mode list
    CFRunLoopPortRef _wakeUpPort; // used for CFRunLoopWakeUp
    volatile uint32_t* _stopped;
    CFMutableSetRef _commonModes;
    CFMutableSetRef _commonModeItems;
    CFRunLoopModeRef _currentMode;
    CFMutableDictionaryRef _modes;
};

typedef struct {
    CFMutableArrayRef results;
    int64_t cutoffTSR;
} __CFRunLoopTimersToFireContext;


/* Bit 0 of CF_INFO is used for stopped state */
/* Bit 1 of the base reserved bits is used for sleeping state */
/* Bit 2 of the base reserved bits is used for deallocating state */

/* CFRunLoop */



static CFMutableDictionaryRef __CFRunLoops = NULL;
static CFSpinLock_t __CFRunLoopsLock = CFSpinLockInit;

// If this is called on a non-main thread, and the main thread pthread_t is passed in,
// and this has not yet beed called on the main thread (since the last fork(), this will
// produce a different run loop that will probably be tossed away eventually, than the
// main thread run loop. There's nothing much we can do about that, without a call to
// fetch the main thread's pthread_t from the pthreads subsystem.

struct _findsource {
    CFRunLoopPortRef port;
    CFRunLoopSourceRef result;
};

typedef struct ___CFThreadID* __CFThreadID;
static __CFThreadID __CFMainThreadID;

///////////////////////////////////////////////////////////////////// private

CF_INLINE __CFThreadID __CFGetCurrentThreadID(void) {
    return (__CFThreadID)CFPlatformGetThreadID(pthread_self());
}

CF_INLINE void __CFRunLoopModeLock(CFRunLoopModeRef rlm) {
    CFSpinLock(&rlm->_lock);
}
CF_INLINE void __CFRunLoopModeUnlock(CFRunLoopModeRef rlm) {
    CFSpinUnlock(&rlm->_lock);
}

CF_INLINE Boolean __CFRunLoopIsStopped(CFRunLoopRef rl) {
    return rl->_stopped && rl->_stopped[2];
}
CF_INLINE void __CFRunLoopSetStopped(CFRunLoopRef rl) {
    if (rl->_stopped) {
        rl->_stopped[2] = 0x53544F50; // 'STOP'
    }
}
CF_INLINE void __CFRunLoopUnsetStopped(CFRunLoopRef rl) {
    if (rl->_stopped) {
        rl->_stopped[2] = 0x0;
    }
}

CF_INLINE Boolean __CFRunLoopIsSleeping(CFRunLoopRef rl) {
    return (Boolean)_CFBitfieldGetValue(CF_INFO(rl), 1, 1);
}
CF_INLINE void __CFRunLoopSetSleeping(CFRunLoopRef rl) {
    _CFBitfieldSetValue(CF_INFO(rl), 1, 1, 1);
}
CF_INLINE void __CFRunLoopUnsetSleeping(CFRunLoopRef rl) {
    _CFBitfieldSetValue(CF_INFO(rl), 1, 1, 0);
}

CF_INLINE Boolean __CFRunLoopIsDeallocating(CFRunLoopRef rl) {
    return (Boolean)_CFBitfieldGetValue(CF_INFO(rl), 2, 2);
}
CF_INLINE void __CFRunLoopSetDeallocating(CFRunLoopRef rl) {
    _CFBitfieldSetValue(CF_INFO(rl), 2, 2, 1);
}

CF_INLINE void __CFRunLoopLock(CFRunLoopRef rl) {
    CFSpinLock(&rl->_lock);
}
CF_INLINE void __CFRunLoopUnlock(CFRunLoopRef rl) {
    CFSpinUnlock(&rl->_lock);
}

/* call with rl locked; returns mode locked */
static CFRunLoopModeRef __CFRunLoopFindMode(CFRunLoopRef rl, CFStringRef modeName, Boolean create) {
    CFRunLoopModeRef rlm;

    rlm = (CFRunLoopModeRef)CFDictionaryGetValue(rl->_modes, modeName);
    if (rlm) {
        __CFRunLoopModeLock(rlm); /* return mode locked */
        return rlm;
    }
    if (!create) {
        return NULL;
    }
    rlm = (CFRunLoopModeRef)_CFRuntimeCreateInstance(
        CFGetAllocator(rl),
        __kCFRunLoopModeTypeID,
        sizeof(struct __CFRunLoopMode) - sizeof(CFRuntimeBase),
        NULL);
    if (!rlm) {
        return NULL;
    }
    rlm->_lock = CFSpinLockInit;
    rlm->_name = CFStringCreateCopy(CFGetAllocator(rlm), modeName);
    rlm->_stopped = false;
    rlm->_sources = NULL;
    rlm->_observers = NULL;
    rlm->_timers = NULL;
    rlm->_submodes = NULL;
    rlm->_portSet = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    CFArrayAppendValue(rlm->_portSet, rl->_wakeUpPort);
    CFDictionaryAddValue(rl->_modes, modeName, rlm);
    CFRelease(rlm);
    __CFRunLoopModeLock(rlm); /* return mode locked */
    return rlm;
}

// expects rl and rlm locked
static Boolean __CFRunLoopModeIsEmpty(CFRunLoopRef rl, CFRunLoopModeRef rlm) {
    if (!rlm) {
        return true;
    }
    if (rlm->_sources && 0 < CFSetGetCount(rlm->_sources)) {
        return false;
    }
    if (rlm->_timers && 0 < CFSetGetCount(rlm->_timers)) {
        return false;
    }
    if (rlm->_submodes) {
        CFIndex idx, cnt;
        for (idx = 0, cnt = CFArrayGetCount(rlm->_submodes); idx < cnt; idx++) {
            CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(rlm->_submodes, idx);
            CFRunLoopModeRef subrlm;
            Boolean subIsEmpty;
            subrlm = __CFRunLoopFindMode(rl, modeName, false);
            subIsEmpty = (subrlm) ? __CFRunLoopModeIsEmpty(rl, subrlm) : true;
            if (subrlm) {
                __CFRunLoopModeUnlock(subrlm);
            }
            if (!subIsEmpty) {
                return false;
            }
        }
    }
    return true;
}

// rl is locked, rlm is locked on entry and exit
static void __CFRunLoopModeAddPortsToPortSet(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFMutableArrayRef portSet) {
    CFIndex idx, cnt;
    const void** list, * buffer[256];

    // Version 1 sources go into the portSet currently
    if (rlm->_sources) {
        cnt = CFSetGetCount(rlm->_sources);
        list = (cnt <= 256) ? buffer : (const void**)CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void*), 0);
        CFSetGetValues(rlm->_sources, list);
        for (idx = 0; idx < cnt; idx++) {
            CFRunLoopSourceRef rls = (CFRunLoopSourceRef)list[idx];
            CFRunLoopPortRef port = __CFRunLoopSourceGetPort(rls);
            if (port) {
                CFArrayAppendValue(portSet, port);
            }
        }
        if (list != buffer) {
            CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
        }
    }
    // iterate over submodes
    for (idx = 0, cnt = rlm->_submodes ? CFArrayGetCount(rlm->_submodes) : 0; idx < cnt; idx++) {
        CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(rlm->_submodes, idx);
        CFRunLoopModeRef subrlm;
        subrlm = __CFRunLoopFindMode(rl, modeName, false);
        if (subrlm) {
            __CFRunLoopModeAddPortsToPortSet(rl, subrlm, portSet);
            __CFRunLoopModeUnlock(subrlm);
        }
    }
}

static CFArrayRef __CFRunLoopModeCollectWaitSet(CFRunLoopRef rl, CFRunLoopModeRef rlm) {
    if (rlm->_submodes) {
        CFMutableArrayRef waitSet = CFArrayCreateMutable(
                kCFAllocatorSystemDefault,
                0,
                &kCFTypeArrayCallBacks);
        __CFRunLoopModeUnlock(rlm);
        __CFRunLoopLock(rl);
        __CFRunLoopModeLock(rlm);
        __CFRunLoopModeAddPortsToPortSet(rl, rlm, waitSet);
        __CFRunLoopUnlock(rl);
        return waitSet;
    } else {
        return CFArrayCreateCopy(CFGetAllocator(rlm->_portSet), rlm->_portSet);
    }
}

static void __CFRunLoopDeallocateTimers(const void* key, const void* value, void* context) {
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)context;
    CFIndex idx, cnt;
    const void** list, * buffer[256];
    if (!rlm->_timers) {
        return;
    }
    cnt = CFSetGetCount(rlm->_timers);
    list = (cnt <= 256) ? buffer : (const void**)CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void*), 0);
    CFSetGetValues(rlm->_timers, list);
    for (idx = 0; idx < cnt; idx++) {
        CFRetain(list[idx]);
    }
    CFSetRemoveAllValues(rlm->_timers);
    for (idx = 0; idx < cnt; idx++) {
        //__CFRunLoopTimerCancel((CFRunLoopTimerRef)list[idx],rl,rlm);
        CFRelease(list[idx]);
    }
    if (list != buffer) {
        CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
    }
}

static void __CFRunLoopDeallocateSources(const void* key, const void* value, void* context) {
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)context;
    CFIndex idx, cnt;
	//TODO use _CF_ARRAY_ALLOCA instead
    const void** list, * buffer[256];
    if (!rlm->_sources) {
        return;
    }
    cnt = CFSetGetCount(rlm->_sources);
    list = (cnt <= 256) ? buffer : (const void**)CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void*), 0);
    CFSetGetValues(rlm->_sources, list);
    for (idx = 0; idx < cnt; idx++) {
        CFRetain(list[idx]);
    }
    CFSetRemoveAllValues(rlm->_sources);
    for (idx = 0; idx < cnt; idx++) {
        __CFRunLoopSourceCancel((CFRunLoopSourceRef)list[idx], rl, rlm);
        CFRelease(list[idx]);
    }
    if (list != buffer) {
        CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
    }
}

static void __CFRunLoopDeallocateObservers(const void* key, const void* value, void* context) {
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)context;
    CFIndex idx, cnt;
    const void** list, * buffer[256];
    if (!rlm->_observers) {
        return;
    }
    cnt = CFSetGetCount(rlm->_observers);
    list = (cnt <= 256) ? buffer : (const void**)CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(void*), 0);
    CFSetGetValues(rlm->_observers, list);
    for (idx = 0; idx < cnt; idx++) {
        CFRetain(list[idx]);
    }
    CFSetRemoveAllValues(rlm->_observers);
    for (idx = 0; idx < cnt; idx++) {
        __CFRunLoopObserverCancel((CFRunLoopObserverRef)list[idx], rl);
        CFRelease(list[idx]);
    }
    if (list != buffer) {
        CFAllocatorDeallocate(kCFAllocatorSystemDefault, list);
    }
}

static CFRunLoopRef __CFRunLoopCreate(void) {
    CFRunLoopRef loop = NULL;
    CFRunLoopModeRef rlm;
    uint32_t size = sizeof(struct __CFRunLoop) - sizeof(CFRuntimeBase);
    loop = (CFRunLoopRef)_CFRuntimeCreateInstance(kCFAllocatorSystemDefault, __kCFRunLoopTypeID, size, NULL);
    if (!loop) {
        return NULL;
    }
    loop->_stopped = NULL;
    loop->_lock = CFSpinLockInit;
    loop->_wakeUpPort = CFRunLoopPortCreate(CFGetAllocator(loop));
    if (!loop->_wakeUpPort) {
        CF_GENERIC_ERROR("Failed to create wakeup port.");
    }
    loop->_commonModes = CFSetCreateMutable(CFGetAllocator(loop), 0, &kCFTypeSetCallBacks);
    CFSetAddValue(loop->_commonModes, kCFRunLoopDefaultMode);
    loop->_commonModeItems = NULL;
    loop->_currentMode = NULL;
    loop->_modes = CFDictionaryCreateMutable(CFGetAllocator(loop), 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    _CFDictionarySetCapacity(loop->_modes, 10);
    rlm = __CFRunLoopFindMode(loop, kCFRunLoopDefaultMode, true);
    if (rlm) {
        __CFRunLoopModeUnlock(rlm);
    }
    return loop;
}

static void __CFRunLoopGetModeName(const void* key, const void* value, void* context) {
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)value;
    CFMutableArrayRef array = (CFMutableArrayRef)context;
    CFArrayAppendValue(array, rlm->_name);
}

static void __CFRunLoopAddItemsToCommonMode(const void* value, void* ctx) {
    CFTypeRef item = (CFTypeRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)(((CFTypeRef*)ctx)[0]);
    CFStringRef modeName = (CFStringRef)(((CFTypeRef*)ctx)[1]);
    if (CFGetTypeID(item) == CFRunLoopSourceGetTypeID()) {
        CFRunLoopAddSource(rl, (CFRunLoopSourceRef)item, modeName);
    } else if (CFGetTypeID(item) == CFRunLoopObserverGetTypeID()) {
        CFRunLoopAddObserver(rl, (CFRunLoopObserverRef)item, modeName);
    } else if (CFGetTypeID(item) == CFRunLoopTimerGetTypeID()) {
        CFRunLoopAddTimer(rl, (CFRunLoopTimerRef)item, modeName);
    }
}

static void __CFRunLoopAddItemToCommonModes(const void* value, void* ctx) {
    CFStringRef modeName = (CFStringRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)(((CFTypeRef*)ctx)[0]);
    CFTypeRef item = (CFTypeRef)(((CFTypeRef*)ctx)[1]);
    if (CFGetTypeID(item) == CFRunLoopSourceGetTypeID()) {
        CFRunLoopAddSource(rl, (CFRunLoopSourceRef)item, modeName);
    } else if (CFGetTypeID(item) == CFRunLoopObserverGetTypeID()) {
        CFRunLoopAddObserver(rl, (CFRunLoopObserverRef)item, modeName);
    } else if (CFGetTypeID(item) == CFRunLoopTimerGetTypeID()) {
        CFRunLoopAddTimer(rl, (CFRunLoopTimerRef)item, modeName);
    }
}

static void __CFRunLoopRemoveItemFromCommonModes(const void* value, void* ctx) {
    CFStringRef modeName = (CFStringRef)value;
    CFRunLoopRef rl = (CFRunLoopRef)(((CFTypeRef*)ctx)[0]);
    CFTypeRef item = (CFTypeRef)(((CFTypeRef*)ctx)[1]);
    if (CFGetTypeID(item) == CFRunLoopSourceGetTypeID()) {
        CFRunLoopRemoveSource(rl, (CFRunLoopSourceRef)item, modeName);
    } else if (CFGetTypeID(item) == CFRunLoopObserverGetTypeID()) {
        CFRunLoopRemoveObserver(rl, (CFRunLoopObserverRef)item, modeName);
    } else if (CFGetTypeID(item) == CFRunLoopTimerGetTypeID()) {
        CFRunLoopRemoveTimer(rl, (CFRunLoopTimerRef)item, modeName);
    }
}

static void __CFRunLoopFindSource(const void* value, void* ctx) {
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)value;
    struct _findsource* context = (struct _findsource*)ctx;
    if (context->result) {
        return;
    }
    if (__CFRunLoopSourceGetPort(rls) == context->port) {
        context->result = rls;
    }
}

// call with rl and rlm locked
static CFRunLoopSourceRef __CFRunLoopModeFindSourceForPort(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFRunLoopPortRef port) {    /* DOES CALLOUT */
    struct _findsource context = {port, NULL};
    if (rlm->_sources) {
        CFSetApplyFunction(rlm->_sources, (__CFRunLoopFindSource), &context);
    }
    if (!context.result && rlm->_submodes) {
        CFIndex idx, cnt;
        for (idx = 0, cnt = CFArrayGetCount(rlm->_submodes); idx < cnt; idx++) {
            CFRunLoopSourceRef source = NULL;
            CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(rlm->_submodes, idx);
            CFRunLoopModeRef subrlm;
            subrlm = __CFRunLoopFindMode(rl, modeName, false);
            if (subrlm) {
                source = __CFRunLoopModeFindSourceForPort(rl, subrlm, port);
                __CFRunLoopModeUnlock(subrlm);
            }
            if (source) {
                context.result = source;
                break;
            }
        }
    }
    return context.result;
}

/* rl is unlocked, rlm is locked on entrance and exit */
/* ALERT: this should collect all the candidate observers from the top level
 * and all submodes, recursively, THEN start calling them, in order to obey
 * the ordering parameter. */
static void __CFRunLoopDoObservers(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFRunLoopActivity activity) {   /* DOES CALLOUT */
    CFIndex idx, cnt;
    CFArrayRef submodes;

    /* Fire the observers */
    submodes = (rlm->_submodes && 0 < CFArrayGetCount(rlm->_submodes)) ? CFArrayCreateCopy(kCFAllocatorSystemDefault, rlm->_submodes) : NULL;
    if (rlm->_observers) {
        cnt = CFSetGetCount(rlm->_observers);
        if (0 < cnt) {
			//TODO use _CF_ARRAY_ALLOCA instead of buffer
            CFRunLoopObserverRef buffer[1024];
            CFRunLoopObserverRef* collectedObservers = (cnt <= 1024) ? buffer : (CFRunLoopObserverRef*)CFAllocatorAllocate(kCFAllocatorSystemDefault, cnt * sizeof(CFRunLoopObserverRef), 0);
            CFSetGetValues(rlm->_observers, (const void**)collectedObservers);
            for (idx = 0; idx < cnt; idx++) {
                CFRunLoopObserverRef rlo = collectedObservers[idx];
                if (_CFRunLoopObserverCanFire(rlo, activity)) {
                    CFRetain(rlo);
                } else {
                    /* We're not interested in this one - set it to NULL so we don't process it later */
                    collectedObservers[idx] = NULL;
                }
            }

            __CFRunLoopModeUnlock(rlm);
            CFIndex idx;
            CFQSortArray(collectedObservers, cnt, sizeof(CFRunLoopObserverRef), __CFRunLoopObserverQSortComparator, NULL);
            for (idx = 0; idx < cnt; idx++) {
                CFRunLoopObserverRef rlo = collectedObservers[idx];
                if (rlo) {
                    _CFRunLoopObserverFire(activity, rlo);
                    CFRelease(rlo);
                }
            }
            __CFRunLoopModeLock(rlm);

            if (collectedObservers != buffer) {
                CFAllocatorDeallocate(kCFAllocatorSystemDefault, collectedObservers);
            }
        }
    }
    if (submodes) {
        __CFRunLoopModeUnlock(rlm);
        for (idx = 0, cnt = CFArrayGetCount(submodes); idx < cnt; idx++) {
            CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(submodes, idx);
            CFRunLoopModeRef subrlm;
            __CFRunLoopLock(rl);
            subrlm = __CFRunLoopFindMode(rl, modeName, false);
            __CFRunLoopUnlock(rl);
            if (subrlm) {
                __CFRunLoopDoObservers(rl, subrlm, activity);
                __CFRunLoopModeUnlock(subrlm);
            }
        }
        CFRelease(submodes);
        __CFRunLoopModeLock(rlm);
    }
}

static void __CFRunLoopCollectSources0(const void* value, void* context) {
    CFRunLoopSourceRef rls = (CFRunLoopSourceRef)value;
    CFTypeRef* sources = (CFTypeRef*)context;
    if (_CFRunLoopSource0IsSignalled(rls)) {
        if (!*sources) {
            *sources = CFRetain(rls);
        } else if (CFGetTypeID(*sources) == CFRunLoopSourceGetTypeID()) {
            CFTypeRef oldrls = *sources;
            *sources = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
            CFArrayAppendValue((CFMutableArrayRef) * sources, oldrls);
            CFArrayAppendValue((CFMutableArrayRef) * sources, rls);
            CFRelease(oldrls);
        } else {
            CFArrayAppendValue((CFMutableArrayRef) * sources, rls);
        }
    }
}

/* rl is unlocked, rlm is locked on entrance and exit */
static Boolean __CFRunLoopDoSources0(CFRunLoopRef rl, CFRunLoopModeRef rlm, Boolean stopAfterHandle) {    /* DOES CALLOUT */
    CFTypeRef sources = NULL;
    Boolean sourceHandled = false;
    CFIndex idx, cnt;

    __CFRunLoopModeUnlock(rlm); // locks have to be taken in order
    __CFRunLoopLock(rl);
    __CFRunLoopModeLock(rlm);
    /* Fire the version 0 sources */
    if (rlm->_sources && 0 < CFSetGetCount(rlm->_sources)) {
        CFSetApplyFunction(rlm->_sources, (__CFRunLoopCollectSources0), &sources);
    }
    for (idx = 0, cnt = (rlm->_submodes) ? CFArrayGetCount(rlm->_submodes) : 0; idx < cnt; idx++) {
        CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(rlm->_submodes, idx);
        CFRunLoopModeRef subrlm;
        subrlm = __CFRunLoopFindMode(rl, modeName, false);
        if (subrlm) {
            if (subrlm->_sources && 0 < CFSetGetCount(subrlm->_sources)) {
                CFSetApplyFunction(subrlm->_sources, (__CFRunLoopCollectSources0), &sources);
            }
            __CFRunLoopModeUnlock(subrlm);
        }
    }
    __CFRunLoopUnlock(rl);
    if (sources) {
        // sources is either a single (retained) CFRunLoopSourceRef or an array of (retained) CFRunLoopSourceRef
        __CFRunLoopModeUnlock(rlm);
        if (CFGetTypeID(sources) == CFRunLoopSourceGetTypeID()) {
            CFRunLoopSourceRef rls = (CFRunLoopSourceRef)sources;
            sourceHandled = _CFRunLoopSource0Perform(rls);
        } else {
            cnt = CFArrayGetCount((CFArrayRef)sources);
            CFArraySortValues((CFMutableArrayRef)sources, CFRangeMake(0, cnt), (__CFRunLoopSourceComparator), NULL);
            for (idx = 0; idx < cnt; idx++) {
                CFRunLoopSourceRef rls = (CFRunLoopSourceRef)CFArrayGetValueAtIndex((CFArrayRef)sources, idx);
                sourceHandled = _CFRunLoopSource0Perform(rls);
                if (stopAfterHandle && sourceHandled) {
                    break;
                }
            }
        }
        CFRelease(sources);
        __CFRunLoopModeLock(rlm);
    }
    return sourceHandled;
}

static Boolean __CFRunLoopDoSource1(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFRunLoopSourceRef rls) {    /* DOES CALLOUT */
    Boolean sourceHandled = false;

    /* Fire a version 1 source */
    CFRetain(rls);
    __CFRunLoopModeUnlock(rlm);

    sourceHandled = _CFRunLoopSource1Perform(rls);

    CFRelease(rls);
    __CFRunLoopModeLock(rlm);
    return sourceHandled;
}

static void __CFRunLoopGetNextTimerFireTSRIterator(CFRunLoopTimerRef rlt, void* context) {
    int64_t* fireTSR = (int64_t*)context;
    if (__CFIsValid(rlt)) {
        int64_t timerFireTSR = __CFRunLoopTimerGetFireTSR(rlt);
        if (!*fireTSR || *fireTSR > timerFireTSR) {
            *fireTSR = timerFireTSR;
        }
    }
}

static int64_t __CFRunLoopGetNextTimerFireTSR(CFRunLoopRef rl, CFRunLoopModeRef rlm) {
    int64_t fireTime = 0;
    __CFRunLoopModeIterateTimers(rl, rlm, __CFRunLoopGetNextTimerFireTSRIterator, &fireTime);
    return fireTime;
}


static void __CFRunLoopTimersToFireIterator(CFRunLoopTimerRef rlt, void* ctx) {
    __CFRunLoopTimersToFireContext* context = (__CFRunLoopTimersToFireContext*)ctx;
    int64_t fireTSR = __CFRunLoopTimerGetFireTSR(rlt);
    if (fireTSR <= context->cutoffTSR) {
        if (!context->results) {
            context->results = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
        }
        CFArrayAppendValue(context->results, rlt);
    }
}

// rl and rlm must be locked
static CFArrayRef __CFRunLoopTimersToFire(CFRunLoopRef rl, CFRunLoopModeRef rlm) {
    __CFRunLoopTimersToFireContext ctxt = {NULL, _CFReadTSR()};
    __CFRunLoopModeIterateTimers(rl, rlm, __CFRunLoopTimersToFireIterator, &ctxt);
    return ctxt.results;
}


static CFRunLoopPortRef __CFRunLoopWait(CFRunLoopRef rl, CFRunLoopModeRef rlm, int64_t termTSR) {
    CFArrayRef waitSet = __CFRunLoopModeCollectWaitSet(rl, rlm);
    CFTimeInterval timeout = 0;
    if (termTSR) {
        int64_t nextStop = __CFRunLoopGetNextTimerFireTSR(rl, rlm);
        if (nextStop <= 0) {
            nextStop = termTSR;
        } else if (nextStop > termTSR) {
            nextStop = termTSR;
        }
        int64_t tsr = _CFReadTSR();
        int64_t timeoutTSR = nextStop - tsr;
        printf("Next stop: %I64d, TSR: %I64d, timeout: %I64d\n", nextStop, tsr, timeoutTSR);
        if (timeoutTSR < 0) {
            timeout = 0;
        } else {
            timeout = _CFTSRToTimeInterval(timeoutTSR);
        }
    }
    CFRunLoopPortRef signalledPort = NULL;
    CFIndex signalledIndex = 0;
    if (CFRunLoopPortWait(waitSet, timeout, &signalledIndex) && signalledIndex != -1) {
        signalledPort = (CFRunLoopPortRef)CFArrayGetValueAtIndex(waitSet, signalledIndex);
    }
    CFRelease(waitSet);
    return signalledPort;
}

/* rl is unlocked, rlm locked on entrance and exit */
static int32_t __CFRunLoopRun(CFRunLoopRef rl, CFRunLoopModeRef rlm, CFTimeInterval seconds, Boolean stopAfterHandle, Boolean waitIfEmpty) {      /* DOES CALLOUT */
    int64_t termTSR;
    Boolean poll = false;
    Boolean firstPass = true;

    if (__CFRunLoopIsStopped(rl)) {
        return kCFRunLoopRunStopped;
    } else if (rlm->_stopped) {
        rlm->_stopped = false;
        return kCFRunLoopRunStopped;
    }
    if (seconds <= 0.0) {
        termTSR = 0;
    } else if (seconds > __CFRunLoopInfiniteWait) {
        termTSR = LLONG_MAX;
    } else {
        termTSR = (int64_t)_CFReadTSR() + _CFTimeIntervalToTSR(seconds);
    }
    if (seconds <= 0.0) {
        poll = true;
    }
    for (;; ) {
        CFArrayRef timersToCall = NULL;
        CFRunLoopPortRef livePort = NULL;
        int32_t returnValue = 0;
        Boolean sourceHandledThisLoop = false;

        __CFRunLoopDoObservers(rl, rlm, kCFRunLoopBeforeTimers);
        __CFRunLoopDoObservers(rl, rlm, kCFRunLoopBeforeSources);

        sourceHandledThisLoop = __CFRunLoopDoSources0(rl, rlm, stopAfterHandle);

        if (sourceHandledThisLoop) {
            poll = true;
        }

        if (!poll) {
            __CFRunLoopDoObservers(rl, rlm, kCFRunLoopBeforeWaiting);
            __CFRunLoopSetSleeping(rl);
        }
        __CFRunLoopModeUnlock(rlm);

        livePort = __CFRunLoopWait(rl, rlm, poll ? 0 : termTSR);

        __CFRunLoopLock(rl);
        __CFRunLoopModeLock(rlm);
        __CFRunLoopUnlock(rl);
        if (!poll) {
            __CFRunLoopUnsetSleeping(rl);
            __CFRunLoopDoObservers(rl, rlm, kCFRunLoopAfterWaiting);
        }
        poll = false;
        __CFRunLoopModeUnlock(rlm);
        __CFRunLoopLock(rl);
        __CFRunLoopModeLock(rlm);

        timersToCall = __CFRunLoopTimersToFire(rl, rlm);

        if (!livePort) {
            __CFRunLoopUnlock(rl);
        } else if (livePort == rl->_wakeUpPort) {
            __CFRunLoopUnlock(rl);
            if (_LogCFRunLoop) {
                CFLog(kCFLogLevelDebug, CFSTR("wakeupPort was signalled"));
            }
        } else {
            CFRunLoopSourceRef rls = __CFRunLoopModeFindSourceForPort(rl, rlm, livePort);
            __CFRunLoopUnlock(rl);
            if (rls) {
                if (_LogCFRunLoop) {
                    CFLog(kCFLogLevelDebug, CFSTR("Source %@ was signalled"), rls);
                }
                if (__CFRunLoopDoSource1(rl, rlm, rls)) {
                    sourceHandledThisLoop = true;
                }
            }
        }

        if (timersToCall) {
            CFIndex i;
            for (i = CFArrayGetCount(timersToCall) - 1; i >= 0; i--) {
                CFRunLoopTimerRef rlt = (CFRunLoopTimerRef)CFArrayGetValueAtIndex(timersToCall, i);
                if (rlt) {
                    __CFRunLoopModeUnlock(rlm);
                    __CFRunLoopTimerFire(rl, rlt);
                    __CFRunLoopModeLock(rlm);
                }
            }
            CFRelease(timersToCall);
        }

        __CFRunLoopModeUnlock(rlm);     // locks must be taken in order
        __CFRunLoopLock(rl);
        __CFRunLoopModeLock(rlm);
        if (sourceHandledThisLoop && stopAfterHandle) {
            returnValue = kCFRunLoopRunHandledSource;
            // If we're about to timeout, but we just did a zero-timeout poll that only found our own
            // internal wakeup signal on the first look at the portset, we'll go around the loop one
            // more time, so as not to starve a v1 source that was just added along with a runloop wakeup.
        } else if (returnValue || termTSR <= _CFReadTSR()) {
            returnValue = kCFRunLoopRunTimedOut;
        } else if (__CFRunLoopIsStopped(rl)) {
            returnValue = kCFRunLoopRunStopped;
        } else if (rlm->_stopped) {
            rlm->_stopped = false;
            returnValue = kCFRunLoopRunStopped;
        } else if (!waitIfEmpty && __CFRunLoopModeIsEmpty(rl, rlm)) {
            returnValue = kCFRunLoopRunFinished;
        }
        __CFRunLoopUnlock(rl);
        if (returnValue) {
            return returnValue;
        }
        firstPass = false;
    }
}

static CFRunLoopRef __CFRunLoopGetForThread(__CFThreadID thread) {
    CFRunLoopRef loop = NULL;
    CFSpinLock(&__CFRunLoopsLock);
    if (!__CFRunLoops) {
        CFSpinUnlock(&__CFRunLoopsLock);
        CFMutableDictionaryRef loops = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, NULL, NULL);
        CFRunLoopRef mainLoop = __CFRunLoopCreate();
        CFDictionarySetValue(loops, __CFMainThreadID, mainLoop);
        if (!OSAtomicCompareAndSwapPtrBarrier(NULL, loops, (void* volatile*)&__CFRunLoops)) {
            CFRelease(loops);
            CFRelease(mainLoop);
        }
        CFSpinLock(&__CFRunLoopsLock);
    }
    loop = (CFRunLoopRef)CFDictionaryGetValue(__CFRunLoops, thread);
    if (!loop) {
        CFSpinUnlock(&__CFRunLoopsLock);
        CFRunLoopRef newLoop = __CFRunLoopCreate();
        CFSpinLock(&__CFRunLoopsLock);
        loop = (CFRunLoopRef)CFDictionaryGetValue(__CFRunLoops, thread);
        if (loop) {
            CFRelease(newLoop);
        } else {
            CFDictionarySetValue(__CFRunLoops, thread, newLoop);
            loop = newLoop;
        }
    }
    if (thread == __CFGetCurrentThreadID()) {
        // Make sure run loop finalizer is registered.
        _CFGetThreadSpecificData();
    }
    CFSpinUnlock(&__CFRunLoopsLock);
    return loop;
}

/*** CFRunLoopMode class ***/

static Boolean __CFRunLoopModeEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFRunLoopModeRef rlm1 = (CFRunLoopModeRef)cf1;
    CFRunLoopModeRef rlm2 = (CFRunLoopModeRef)cf2;
    return CFEqual(rlm1->_name, rlm2->_name);
}
static CFHashCode __CFRunLoopModeHash(CFTypeRef cf) {
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)cf;
    return CFHash(rlm->_name);
}

static CFStringRef __CFRunLoopModeCopyDescription(CFTypeRef cf) {
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)cf;
    CFMutableStringRef result;
    result = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
    CFStringAppendFormat(
        result,
        NULL, CFSTR("<CFRunLoopMode %p [%p]>{name = %@, "),
        rlm, CFGetAllocator(rlm), rlm->_name);
    CFStringAppendFormat(
        result,
        NULL, CFSTR("\n\tsources = %@,\n\tobservers = %@,\n\ttimers = %@\n},\n"),
        rlm->_sources, rlm->_observers, rlm->_timers);
    return result;
}

static void __CFRunLoopModeDeallocate(CFTypeRef cf) {
    CFRunLoopModeRef rlm = (CFRunLoopModeRef)cf;
    if (rlm->_sources) {
        CFRelease(rlm->_sources);
    }
    if (rlm->_observers) {
        CFRelease(rlm->_observers);
    }
    if (rlm->_timers) {
        CFRelease(rlm->_timers);
    }
    if (rlm->_submodes) {
        CFRelease(rlm->_submodes);
    }
    CFRelease(rlm->_name);
    CFRelease(rlm->_portSet);
}

static const CFRuntimeClass __CFRunLoopModeClass = {
    0,
    "CFRunLoopMode",
    NULL, // init
    NULL, // copy
    __CFRunLoopModeDeallocate,
    __CFRunLoopModeEqual,
    __CFRunLoopModeHash,
    NULL,
    __CFRunLoopModeCopyDescription
};

/*** CFRunLoop class ***/

static void __CFRunLoopDeallocate(CFTypeRef cf) {
    CFRunLoopRef rl = (CFRunLoopRef)cf;
    /* We try to keep the run loop in a valid state as long as possible,
     * since sources may have non-retained references to the run loop.
     * Another reason is that we don't want to lock the run loop for
     * callback reasons, if we can get away without that.  We start by
     * eliminating the sources, since they are the most likely to call
     * back into the run loop during their "cancellation". Common mode
     * items will be removed from the mode indirectly by the following
     * three lines. */
    __CFRunLoopSetDeallocating(rl);
    if (rl->_modes) {
        CFDictionaryApplyFunction(rl->_modes, __CFRunLoopDeallocateSources, rl);
        CFDictionaryApplyFunction(rl->_modes, __CFRunLoopDeallocateObservers, rl);
        CFDictionaryApplyFunction(rl->_modes, __CFRunLoopDeallocateTimers, rl);
    }
    __CFRunLoopLock(rl);
    if (rl->_commonModeItems) {
        CFRelease(rl->_commonModeItems);
    }
    if (rl->_commonModes) {
        CFRelease(rl->_commonModes);
    }
    if (rl->_modes) {
        CFRelease(rl->_modes);
    }
    CFRelease(rl->_wakeUpPort);
    __CFRunLoopUnlock(rl);
}

static CFStringRef __CFRunLoopCopyDescription(CFTypeRef cf) {
    CFRunLoopRef rl = (CFRunLoopRef)cf;
    CFMutableStringRef result;
    result = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
    CFStringAppendFormat(
        result, NULL, 
        CFSTR("<CFRunLoop %p [%p]>{wait port = %@, stopped = %s,\ncurrent mode = %@,\n"), 
        cf, CFGetAllocator(cf), 
        rl->_wakeUpPort, 
        (rl->_stopped && (rl->_stopped[2] == 0x53544F50)) ? "Yes" : "No", 
        rl->_currentMode ? rl->_currentMode->_name : CFSTR("(none)"));
    CFStringAppendFormat(result, 
        NULL, CFSTR("common modes = %@,\ncommon mode items = %@,\nmodes = %@}\n"), 
        rl->_commonModes, rl->_commonModeItems, rl->_modes);
    return result;
}

static const CFRuntimeClass __CFRunLoopClass = {
    0,
    "CFRunLoop",
    NULL, // init
    NULL, // copy
    __CFRunLoopDeallocate,
    NULL,
    NULL,
    NULL,
    __CFRunLoopCopyDescription
};

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL void _CFRunLoopInitialize(void) {
    __kCFRunLoopTypeID = _CFRuntimeRegisterClass(&__CFRunLoopClass);
    __kCFRunLoopModeTypeID = _CFRuntimeRegisterClass(&__CFRunLoopModeClass);
    __CFMainThreadID = __CFGetCurrentThreadID();
    __CFRunLoopPortInitialize();
}

CF_INTERNAL void _CFFinalizeCurrentRunLoop(void) {
    CFSpinLock(&__CFRunLoopsLock);
    if (__CFRunLoops) {
        __CFThreadID threadID = __CFGetCurrentThreadID();
        if (threadID != __CFMainThreadID) {
            CFRunLoopRef currentLoop = (CFRunLoopRef)CFDictionaryGetValue(__CFRunLoops, threadID);
            if (currentLoop) {
                CFDictionaryRemoveValue(__CFRunLoops, threadID);
                CFRelease(currentLoop);
            }        
        }
    }
    CFSpinUnlock(&__CFRunLoopsLock);
}

CF_INTERNAL void __CFRunLoopModeIterateTimers(CFRunLoopRef rl, CFRunLoopModeRef rlm, __CFRunLoopModeTimerIterator iterator, void* context) {
    if (rlm->_timers && CFSetGetCount(rlm->_timers)) {
        CFSetApplyFunction(rlm->_timers, (CFSetApplierFunction)iterator, context);
    }
    if (rlm->_submodes) {
        CFIndex idx, cnt;
        for (idx = 0, cnt = CFArrayGetCount(rlm->_submodes); idx < cnt; idx++) {
            CFStringRef modeName = (CFStringRef)CFArrayGetValueAtIndex(rlm->_submodes, idx);
            CFRunLoopModeRef subrlm = __CFRunLoopFindMode(rl, modeName, false);
            if (subrlm) {
                __CFRunLoopModeIterateTimers(rl, subrlm, iterator, context);
                __CFRunLoopModeUnlock(subrlm);
            }
        }
    }
}

CF_INTERNAL void __CFRunLoopFindModeIterateTimers(CFRunLoopRef rl, CFStringRef modeName, __CFRunLoopModeTimerIterator iterator, void* context) {
    CFRunLoopModeRef rlm;
    __CFRunLoopLock(rl);
    rlm = __CFRunLoopFindMode(rl, modeName, false);
    __CFRunLoopUnlock(rl);
    if (rlm) {
        __CFRunLoopModeIterateTimers(rl, rlm, iterator, context);
        __CFRunLoopModeUnlock(rlm);
    }
}

CF_INTERNAL CFStringRef _CFRunLoopModeGetName(CFRunLoopModeRef rlm) {
    return rlm->_name;
}

CF_INTERNAL void _CFRunLoopModeAddPort(CFRunLoopModeRef rlm, CFRunLoopPortRef port) {
    CFArrayAppendValue(rlm->_portSet, port);
}

CF_INTERNAL void _CFRunLoopModeRemovePort(CFRunLoopModeRef rlm, CFRunLoopPortRef port) {
    CFIndex index = CFArrayGetFirstIndexOfValue(
            rlm->_portSet,
            CFRangeMake(0, CFArrayGetCount(rlm->_portSet)),
            port);
    if (index != kCFNotFound) {
        CFArrayRemoveValueAtIndex(rlm->_portSet, index);
    }
}

///////////////////////////////////////////////////////////////////// public

CONST_STRING_DECL(kCFRunLoopDefaultMode, "kCFRunLoopDefaultMode")
CONST_STRING_DECL(kCFRunLoopCommonModes, "kCFRunLoopCommonModes")

CFTypeID CFRunLoopGetTypeID(void) {
    return __kCFRunLoopTypeID;
}

CFRunLoopRef CFRunLoopGetMain(void) {
    return __CFRunLoopGetForThread(__CFMainThreadID);
}

CFRunLoopRef CFRunLoopGetCurrent(void) {
    return __CFRunLoopGetForThread(__CFGetCurrentThreadID());
}

CFStringRef CFRunLoopCopyCurrentMode(CFRunLoopRef rl) {
    CFStringRef result = NULL;
    __CFRunLoopLock(rl);
    if (rl->_currentMode) {
        result = (CFStringRef)CFRetain(rl->_currentMode->_name);
    }
    __CFRunLoopUnlock(rl);
    return result;
}

CFArrayRef CFRunLoopCopyAllModes(CFRunLoopRef rl) {
    CFMutableArrayRef array;
    __CFRunLoopLock(rl);
    array = CFArrayCreateMutable(kCFAllocatorSystemDefault, CFDictionaryGetCount(rl->_modes), &kCFTypeArrayCallBacks);
    CFDictionaryApplyFunction(rl->_modes, __CFRunLoopGetModeName, array);
    __CFRunLoopUnlock(rl);
    return array;
}

void CFRunLoopAddCommonMode(CFRunLoopRef rl, CFStringRef modeName) {
    if (__CFRunLoopIsDeallocating(rl)) {
        return;
    }
    __CFRunLoopLock(rl);
    if (!CFSetContainsValue(rl->_commonModes, modeName)) {
        CFSetRef set = rl->_commonModeItems ? 
            CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModeItems) :
            NULL;
        CFSetAddValue(rl->_commonModes, modeName);
        __CFRunLoopUnlock(rl);
        if (set) {
            CFTypeRef context[2] = {rl, modeName};
            /* add all common-modes items to new mode */
            CFSetApplyFunction(set, __CFRunLoopAddItemsToCommonMode, context);
            CFRelease(set);
        }
    } else {
        __CFRunLoopUnlock(rl);
    }
}

SInt32 CFRunLoopRunSpecific(CFRunLoopRef rl, CFStringRef modeName, CFTimeInterval seconds, Boolean returnAfterSourceHandled) {        /* DOES CALLOUT */
    if (__CFRunLoopIsDeallocating(rl)) {
        return kCFRunLoopRunFinished;
    }
    __CFRunLoopLock(rl);
    CFRunLoopModeRef currentMode = __CFRunLoopFindMode(rl, modeName, false);
    if (!currentMode || __CFRunLoopModeIsEmpty(rl, currentMode)) {
        if (currentMode) {
            __CFRunLoopModeUnlock(currentMode);
        }
        __CFRunLoopUnlock(rl);
        return kCFRunLoopRunFinished;
    }
    uint32_t* previousStopped = (uint32_t*)rl->_stopped;
    rl->_stopped = (volatile uint32_t*)CFAllocatorAllocate(kCFAllocatorSystemDefault, 4 * sizeof(uint32_t), 0);
    //XXX what is that for? why not use flag?
    rl->_stopped[0] = 0x4346524C;
    rl->_stopped[1] = 0x4346524C;   // 'CFRL'
    rl->_stopped[2] = 0x00000000;   // here the value is stored
    rl->_stopped[3] = 0x4346524C;
    CFRunLoopModeRef previousMode = rl->_currentMode;
    rl->_currentMode = currentMode;
    __CFRunLoopUnlock(rl);
    int32_t result;
    __CFRunLoopDoObservers(rl, currentMode, kCFRunLoopEntry);
    result = __CFRunLoopRun(rl, currentMode, seconds, returnAfterSourceHandled, false);
    __CFRunLoopDoObservers(rl, currentMode, kCFRunLoopExit);
    __CFRunLoopModeUnlock(currentMode);
    __CFRunLoopLock(rl);
    CFAllocatorDeallocate(kCFAllocatorSystemDefault, (uint32_t*)rl->_stopped);
    rl->_stopped = previousStopped;
    rl->_currentMode = previousMode;
    __CFRunLoopUnlock(rl);
    return result;
}

void CFRunLoopRun(void) { /* DOES CALLOUT */
    int32_t result;
    do {
        result = CFRunLoopRunSpecific(CFRunLoopGetCurrent(), kCFRunLoopDefaultMode, __CFRunLoopInfiniteWait, false);
    } while (kCFRunLoopRunStopped != result && kCFRunLoopRunFinished != result);
}

SInt32 CFRunLoopRunInMode(CFStringRef modeName, CFTimeInterval seconds, Boolean returnAfterSourceHandled) { /* DOES CALLOUT */
    return CFRunLoopRunSpecific(CFRunLoopGetCurrent(), modeName, seconds, returnAfterSourceHandled);
}

Boolean CFRunLoopIsWaiting(CFRunLoopRef rl) {
    return __CFRunLoopIsSleeping(rl);
}

void CFRunLoopWakeUp(CFRunLoopRef rl) {
    CFRunLoopPortSignal(rl->_wakeUpPort);
}

void CFRunLoopStop(CFRunLoopRef rl) {
    __CFRunLoopLock(rl);
    __CFRunLoopSetStopped(rl);
    __CFRunLoopUnlock(rl);
    CFRunLoopWakeUp(rl);
}

Boolean CFRunLoopContainsSource(CFRunLoopRef rl, CFRunLoopSourceRef rls, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    Boolean hasValue = false;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
        if (rl->_commonModeItems) {
            hasValue = CFSetContainsValue(rl->_commonModeItems, rls);
        }
        __CFRunLoopUnlock(rl);
    } else {
        rlm = __CFRunLoopFindMode(rl, modeName, false);
        __CFRunLoopUnlock(rl);
        if (rlm && rlm->_sources) {
            hasValue = CFSetContainsValue(rlm->_sources, rls);
            __CFRunLoopModeUnlock(rlm);
        } else if (rlm) {
            __CFRunLoopModeUnlock(rlm);
        }
    }
    return hasValue;
}

void CFRunLoopAddSource(CFRunLoopRef rl, CFRunLoopSourceRef rls, CFStringRef modeName) { /* DOES CALLOUT */
    CFRunLoopModeRef rlm;
    if (__CFRunLoopIsDeallocating(rl)) {
        return;
    }
    if (!__CFIsValid(rls)) {
        return;
    }
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
        CFSetRef set = rl->_commonModes ? 
            CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) :
            NULL;
        if (!rl->_commonModeItems) {
            rl->_commonModeItems = CFSetCreateMutable(CFGetAllocator(rl), 0, &kCFTypeSetCallBacks);
            _CFSetSetCapacity(rl->_commonModeItems, 20);
        }
        CFSetAddValue(rl->_commonModeItems, rls);
        __CFRunLoopUnlock(rl);
        if (set) {
            CFTypeRef context[2] = {rl, rls};
            /* add new item to all common-modes */
            CFSetApplyFunction(set, (__CFRunLoopAddItemToCommonModes), (void*)context);
            CFRelease(set);
        }
    } else {
        rlm = __CFRunLoopFindMode(rl, modeName, true);
        __CFRunLoopUnlock(rl);
        if (rlm && !rlm->_sources) {
            rlm->_sources = CFSetCreateMutable(CFGetAllocator(rlm), 0, &kCFTypeSetCallBacks);
            _CFSetSetCapacity(rlm->_sources, 10);
        }
        if (rlm && !CFSetContainsValue(rlm->_sources, rls)) {
            CFSetAddValue(rlm->_sources, rls);
            __CFRunLoopModeUnlock(rlm);
            __CFRunLoopSourceSchedule(rls, rl, rlm); /* DOES CALLOUT */
        } else if (rlm) {
            __CFRunLoopModeUnlock(rlm);
        }
    }
}

void CFRunLoopRemoveSource(CFRunLoopRef rl, CFRunLoopSourceRef rls, CFStringRef modeName) { /* DOES CALLOUT */
    CFRunLoopModeRef rlm;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
        if (rl->_commonModeItems && CFSetContainsValue(rl->_commonModeItems, rls)) {
            CFSetRef set = rl->_commonModes ?
                CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) :
                NULL;
            CFSetRemoveValue(rl->_commonModeItems, rls);
            __CFRunLoopUnlock(rl);
            if (set) {
                CFTypeRef context[2] = {rl, rls};
                /* remove new item from all common-modes */
                CFSetApplyFunction(set, __CFRunLoopRemoveItemFromCommonModes, context);
                CFRelease(set);
            }
        } else {
            __CFRunLoopUnlock(rl);
        }
    } else {
        rlm = __CFRunLoopFindMode(rl, modeName, false);
        __CFRunLoopUnlock(rl);
        if (rlm && rlm->_sources && CFSetContainsValue(rlm->_sources, rls)) {
            CFRetain(rls);
            CFSetRemoveValue(rlm->_sources, rls);
            __CFRunLoopModeUnlock(rlm);
            __CFRunLoopSourceCancel(rls, rl, rlm); /* DOES CALLOUT */
            CFRelease(rls);
        } else if (rlm) {
            __CFRunLoopModeUnlock(rlm);
        }
    }
}

Boolean CFRunLoopContainsObserver(CFRunLoopRef rl, CFRunLoopObserverRef rlo, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    Boolean hasValue = false;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
        if (rl->_commonModeItems) {
            hasValue = CFSetContainsValue(rl->_commonModeItems, rlo);
        }
        __CFRunLoopUnlock(rl);
    } else {
        rlm = __CFRunLoopFindMode(rl, modeName, false);
        __CFRunLoopUnlock(rl);
        if (rlm && rlm->_observers) {
            hasValue = CFSetContainsValue(rlm->_observers, rlo);
            __CFRunLoopModeUnlock(rlm);
        } else if (rlm) {
            __CFRunLoopModeUnlock(rlm);
        }
    }
    return hasValue;
}

void CFRunLoopAddObserver(CFRunLoopRef rl, CFRunLoopObserverRef rlo, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    if (__CFRunLoopIsDeallocating(rl)) {
        return;
    }
    if (!__CFIsValid(rlo) || (__CFRunLoopObserverGetLoop(rlo) && __CFRunLoopObserverGetLoop(rlo) != rl)) {
        return;
    }
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
        CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
        if (!rl->_commonModeItems) {
            rl->_commonModeItems = CFSetCreateMutable(CFGetAllocator(rl), 0, &kCFTypeSetCallBacks);
        }
        CFSetAddValue(rl->_commonModeItems, rlo);
        __CFRunLoopUnlock(rl);
        if (set) {
            CFTypeRef context[2] = {rl, rlo};
            /* add new item to all common-modes */
            CFSetApplyFunction(set, (__CFRunLoopAddItemToCommonModes), (void*)context);
            CFRelease(set);
        }
    } else {
        rlm = __CFRunLoopFindMode(rl, modeName, true);
        __CFRunLoopUnlock(rl);
        if (rlm && !rlm->_observers) {
            rlm->_observers = CFSetCreateMutable(CFGetAllocator(rlm), 0, &kCFTypeSetCallBacks);
        }
        if (rlm && !CFSetContainsValue(rlm->_observers, rlo)) {
            CFSetAddValue(rlm->_observers, rlo);
            __CFRunLoopModeUnlock(rlm);
            __CFRunLoopObserverSchedule(rlo, rl);
        } else if (rlm) {
            __CFRunLoopModeUnlock(rlm);
        }
    }
}

void CFRunLoopRemoveObserver(CFRunLoopRef rl, CFRunLoopObserverRef rlo, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
        if (rl->_commonModeItems && CFSetContainsValue(rl->_commonModeItems, rlo)) {
            CFSetRef set = rl->_commonModes ? 
                CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) :
                NULL;
            CFSetRemoveValue(rl->_commonModeItems, rlo);
            __CFRunLoopUnlock(rl);
            if (set) {
                CFTypeRef context[2] = {rl, rlo};
                /* remove new item from all common-modes */
                CFSetApplyFunction(set, __CFRunLoopRemoveItemFromCommonModes, context);
                CFRelease(set);
            }
        } else {
            __CFRunLoopUnlock(rl);
        }
    } else {
        rlm = __CFRunLoopFindMode(rl, modeName, false);
        __CFRunLoopUnlock(rl);
        if (rlm && rlm->_observers && CFSetContainsValue(rlm->_observers, rlo)) {
            CFRetain(rlo);
            CFSetRemoveValue(rlm->_observers, rlo);
            __CFRunLoopModeUnlock(rlm);
            __CFRunLoopObserverCancel(rlo, rl);
            CFRelease(rlo);
        } else if (rlm) {
            __CFRunLoopModeUnlock(rlm);
        }
    }
}

Boolean CFRunLoopContainsTimer(CFRunLoopRef rl, CFRunLoopTimerRef rlt, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    Boolean hasValue = false;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
        if (rl->_commonModeItems) {
            hasValue = CFSetContainsValue(rl->_commonModeItems, rlt);
        }
        __CFRunLoopUnlock(rl);
    } else {
        rlm = __CFRunLoopFindMode(rl, modeName, false);
        __CFRunLoopUnlock(rl);
        if (rlm && rlm->_timers) {
            hasValue = CFSetContainsValue(rlm->_timers, rlt);
            __CFRunLoopModeUnlock(rlm);
        } else if (rlm) {
            __CFRunLoopModeUnlock(rlm);
        }
    }
    return hasValue;
}

void CFRunLoopAddTimer(CFRunLoopRef rl, CFRunLoopTimerRef rlt, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    if (__CFRunLoopIsDeallocating(rl)) {
        return;
    }
    if (!__CFIsValid(rlt) || (__CFRunLoopTimerGetLoop(rlt) && __CFRunLoopTimerGetLoop(rlt) != rl)) {
        return;
    }
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
        CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
        if (!rl->_commonModeItems) {
            rl->_commonModeItems = CFSetCreateMutable(CFGetAllocator(rl), 0, &kCFTypeSetCallBacks);
        }
        CFSetAddValue(rl->_commonModeItems, rlt);
        __CFRunLoopUnlock(rl);
        if (set) {
            CFTypeRef context[2] = {rl, rlt};
            /* add new item to all common-modes */
            CFSetApplyFunction(set, (__CFRunLoopAddItemToCommonModes), (void*)context);
            CFRelease(set);
        }
    } else {
        rlm = __CFRunLoopFindMode(rl, modeName, true);
        __CFRunLoopUnlock(rl);
        if (rlm && !rlm->_timers) {
            rlm->_timers = CFSetCreateMutable(CFGetAllocator(rlm), 0, &kCFTypeSetCallBacks);
        }
        if (rlm && !CFSetContainsValue(rlm->_timers, rlt)) {
            CFSetAddValue(rlm->_timers, rlt);
            __CFRunLoopModeUnlock(rlm);
        } else if (rlm) {
            __CFRunLoopModeUnlock(rlm);
        }
    }
}

void CFRunLoopRemoveTimer(CFRunLoopRef rl, CFRunLoopTimerRef rlt, CFStringRef modeName) {
    CFRunLoopModeRef rlm;
    __CFRunLoopLock(rl);
    if (modeName == kCFRunLoopCommonModes) {
        if (rl->_commonModeItems && CFSetContainsValue(rl->_commonModeItems, rlt)) {
            CFSetRef set = rl->_commonModes ? CFSetCreateCopy(kCFAllocatorSystemDefault, rl->_commonModes) : NULL;
            CFSetRemoveValue(rl->_commonModeItems, rlt);
            __CFRunLoopUnlock(rl);
            if (set) {
                CFTypeRef context[2] = {rl, rlt};
                /* remove new item from all common-modes */
                CFSetApplyFunction(set, __CFRunLoopRemoveItemFromCommonModes, (void*)context);
                CFRelease(set);
            }
        } else {
            __CFRunLoopUnlock(rl);
        }
    } else {
        rlm = __CFRunLoopFindMode(rl, modeName, false);
        __CFRunLoopUnlock(rl);
        if (rlm && rlm->_timers && CFSetContainsValue(rlm->_timers, rlt)) {
            CFRetain(rlt);
            CFSetRemoveValue(rlm->_timers, rlt);
            __CFRunLoopModeUnlock(rlm);
            CFRelease(rlt);
        } else if (rlm) {
            __CFRunLoopModeUnlock(rlm);
        }
    }
}

CFAbsoluteTime CFRunLoopGetNextTimerFireDate(CFRunLoopRef rl, CFStringRef modeName) {
    int64_t fireTSR = 0;
    __CFRunLoopFindModeIterateTimers(rl, modeName, __CFRunLoopGetNextTimerFireTSRIterator, &fireTSR);
    int64_t nowTSR = _CFReadTSR();
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    return !fireTSR ? 0.0 : (now + _CFTSRToTimeInterval(fireTSR - nowTSR));
}
