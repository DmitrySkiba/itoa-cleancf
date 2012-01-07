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

#if !defined(__COREFOUNDATION_CFINTERNAL__)
#define __COREFOUNDATION_CFINTERNAL__ 1

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFAllocator.h>
#include <CoreFoundation/CFRuntime.h>
#include <CoreFoundation/CFStorage.h>
#include <CoreFoundation/CFBag.h>
#include <CoreFoundation/CFSet.h>
#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFLog.h>

#include "CFBaseInternal.h"
#include "CFUtilities.h"
#include "CFObjcBridge.h"
#include "CFConstString.h"
#include "CFThreadData.h"
#include "CFObjcBridge.h"

#include <sys/time.h>
#include <sys/cdefs.h>
#include <sys/errno.h>
#include <pthread.h>

#include <libkern/OSAtomic.h>
#include <malloc/malloc.h>

#ifdef CF_ENABLE_COMPAT
    #include "compat.h"
#endif

#include "CFRuntimeInternal.h"
#include "CFNullInternal.h"
#include "CFAllocatorInternal.h"
#include "CFArrayInternal.h"
#include "CFBooleanInternal.h"
#include "CFNumberInternal.h"
#include "CFLocaleInternal.h"
#include "CFStorageInternal.h"
#include "CFCharacterSetInternal.h"
#include "CFDateInternal.h"
#include "CFRunLoopInternal.h"
#include "CFErrorInternal.h"
#include "CFTimeZoneInternal.h"
#include "CFURLInternal.h"
#include "CFNumberFormatterInternal.h"
#include "CFDateFormatterInternal.h"
#include "CFCalendarInternal.h"
#include "CFLocaleInternal.h"

#include "CFPlatform.h"

CF_EXTERN_C_BEGIN

//TODO create *Internal headers for these:
CF_EXPORT void __CFBagInitialize();
CF_EXPORT void _CFBagSetCapacity(CFMutableBagRef bag, CFIndex cap);
CF_EXPORT void __CFDictionaryInitialize();
CF_EXPORT void _CFDictionarySetCapacity(CFMutableDictionaryRef bag, CFIndex cap);
CF_EXPORT void __CFSetInitialize();
CF_EXPORT void _CFSetSetCapacity(CFMutableSetRef bag, CFIndex cap);
CF_EXPORT void _CFDataInitialize(void);

//TODO move spinlocks to CFUtilities
/* Spinlock
 *
 * Spinlocks are wrapped to allow debug checks (TODO).
 */

typedef OSSpinLock CFSpinLock_t;

#define CFSpinLockInit OS_SPINLOCK_INIT

CF_INLINE void CFSpinLock(CFSpinLock_t* lock) {
    OSSpinLockLock(lock);
}

CF_INLINE void CFSpinUnlock(CFSpinLock_t* lock) {
    OSSpinLockUnlock(lock);
}

CF_EXTERN_C_END

#endif /* ! __COREFOUNDATION_CFINTERNAL__ */
