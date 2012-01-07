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

struct __CFNull {
    CFRuntimeBase _base;
};

///////////////////////////////////////////////////////////////////// private

static struct __CFNull __kCFNull = {
    INIT_CFRUNTIME_BASE()
};

/*** CFNull class ***/

static CFStringRef __CFNullCopyDescription(CFTypeRef cf) {
    return CFStringCreateWithFormat(
        kCFAllocatorSystemDefault,
        NULL, CFSTR("<CFNull %p [%p]>"), cf, CFGetAllocator(cf));
}

static CFStringRef __CFNullCopyFormattingDescription(CFTypeRef cf, CFDictionaryRef formatOptions) {
    return (CFStringRef)CFRetain(CFSTR("null"));
}

static void __CFNullDeallocate(CFTypeRef cf) {
    CF_GENERIC_ERROR("CFNull objects can't be deallocated");
}

static CFTypeID __kCFNullTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFNullClass = {
    0,
    "CFNull",
    NULL, // init
    NULL, // copy
    __CFNullDeallocate,
    NULL, // equal
    NULL, // hash
    __CFNullCopyFormattingDescription,
    __CFNullCopyDescription
};

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL void _CFNullInitialize(void) {
    __kCFNullTypeID = _CFRuntimeRegisterClassBridge(&__CFNullClass, "NSNull");
    _CFRuntimeInitStaticInstance(&__kCFNull, __kCFNullTypeID);
}

///////////////////////////////////////////////////////////////////// public

const CFNullRef kCFNull = &__kCFNull;

CFTypeID CFNullGetTypeID(void) {
    return __kCFNullTypeID;
}
