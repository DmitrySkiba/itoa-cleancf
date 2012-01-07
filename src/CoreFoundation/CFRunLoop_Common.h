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

#if !defined(__COREFOUNDATION_CFRUNLOOPCOMMON__)
#define __COREFOUNDATION_CFRUNLOOPCOMMON__ 1

#include <CoreFoundation/CFRunLoop.h>

typedef struct __CFRunLoopMode* CFRunLoopModeRef;

CF_EXPORT int _LogCFRunLoop;

static const double __CFRunLoopInfiniteWait = 3.1556952e+9;

/////////////////////////////////////////////////

/* Bit 3 in the base reserved bits is used for invalid 
 * state in run loop objects
 */
CF_INLINE Boolean __CFIsValid(const void* cf) {
    return (Boolean)_CFBitfieldGetValue(CF_INFO(cf),3,3);
}
CF_INLINE void __CFSetValid(void* cf) {
    _CFBitfieldSetValue(CF_INFO(cf),3,3,1);
}
CF_INLINE void __CFUnsetValid(void* cf) {
    _CFBitfieldSetValue(CF_INFO(cf),3,3,0);
}

/////////////////////////////////////////////////

typedef void(*__CFRunLoopModeTimerIterator)(CFRunLoopTimerRef rlt,void* context);
CF_EXPORT void __CFRunLoopModeIterateTimers(CFRunLoopRef rl,CFRunLoopModeRef rlm,__CFRunLoopModeTimerIterator iterator,void* context);
CF_EXPORT void __CFRunLoopFindModeIterateTimers(CFRunLoopRef rl,CFStringRef modeName,__CFRunLoopModeTimerIterator iterator,void* context);

CF_EXPORT CFStringRef _CFRunLoopModeGetName(CFRunLoopModeRef rlm);
CF_EXPORT void _CFRunLoopModeAddPort(CFRunLoopModeRef rlm, CFRunLoopPortRef port);
CF_EXPORT void _CFRunLoopModeRemovePort(CFRunLoopModeRef rlm, CFRunLoopPortRef port);

//////////////////////////////////////////////////////////////////////////////////////////////////

CF_EXPORT CFRunLoopRef __CFRunLoopTimerGetLoop(CFRunLoopTimerRef rlt);
CF_EXPORT Boolean __CFRunLoopTimerFire(CFRunLoopRef rl,CFRunLoopTimerRef rlt);
CF_EXPORT int64_t __CFRunLoopTimerGetFireTSR(CFRunLoopTimerRef rlt);

//////////////////////////////////////////////////////////////////////////////////////////////////

CF_EXPORT CFRunLoopPortRef __CFRunLoopSourceGetPort(CFRunLoopSourceRef rls);
CF_EXPORT void __CFRunLoopSourceSchedule(CFRunLoopSourceRef rls,CFRunLoopRef rl,CFRunLoopModeRef rlm);
CF_EXPORT void __CFRunLoopSourceCancel(CFRunLoopSourceRef rls,CFRunLoopRef rl,CFRunLoopModeRef rlm);
CF_EXPORT Boolean _CFRunLoopSource0Perform(CFRunLoopSourceRef rls);
CF_EXPORT Boolean _CFRunLoopSource0IsSignalled(CFRunLoopSourceRef rls);
CF_EXPORT Boolean _CFRunLoopSource1Perform(CFRunLoopSourceRef rls);

CF_EXPORT CFComparisonResult __CFRunLoopSourceComparator(const void* x, const void* y, void* context);

//////////////////////////////////////////////////////////////////////////////////////////////////

CF_EXPORT CFRunLoopRef __CFRunLoopObserverGetLoop(CFRunLoopObserverRef rlo);
CF_EXPORT void __CFRunLoopObserverCancel(CFRunLoopObserverRef rlo,CFRunLoopRef rl);
CF_EXPORT void __CFRunLoopObserverSchedule(CFRunLoopObserverRef rlo,CFRunLoopRef rl);
CF_EXPORT Boolean _CFRunLoopObserverCanFire(CFRunLoopObserverRef rlo, CFRunLoopActivity activity);
CF_EXPORT void _CFRunLoopObserverFire(CFRunLoopActivity activity, CFRunLoopObserverRef rlo);

CF_EXPORT CFComparisonResult __CFRunLoopObserverQSortComparator(const void* val1, const void* val2, void* context);

//////////////////////////////////////////////////////////////////////////////////////////////////

#endif /* ! __COREFOUNDATION_CFRUNLOOPCOMMON__ */
