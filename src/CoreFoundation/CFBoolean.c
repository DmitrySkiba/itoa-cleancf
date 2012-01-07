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

#include <CoreFoundation/CFNumber.h>
#include "CFInternal.h"

struct __CFBoolean {
    CFRuntimeBase _base;
};

///////////////////////////////////////////////////////////////////// private

static struct __CFBoolean __kCFBooleanTrue = {
    INIT_CFRUNTIME_BASE()
};

static struct __CFBoolean __kCFBooleanFalse = {
    INIT_CFRUNTIME_BASE()
};

/*** CFBoolean class ***/

static CFStringRef __CFBooleanCopyDescription(CFTypeRef cf) {
    CFBooleanRef boolean = (CFBooleanRef)cf;
    return CFStringCreateWithFormat(
        kCFAllocatorSystemDefault,
        NULL, CFSTR("<CFBoolean %p [%p]>{value = %s}"), 
        cf, CFGetAllocator(cf), (boolean == kCFBooleanTrue) ? "true" : "false");
}

static CFStringRef __CFBooleanCopyFormattingDescription(CFTypeRef cf, CFDictionaryRef formatOptions) {
    CFBooleanRef boolean = (CFBooleanRef)cf;
    return (CFStringRef)CFRetain((boolean == kCFBooleanTrue) ? CFSTR("true") : CFSTR("false"));
}

static void __CFBooleanDeallocate(CFTypeRef cf) {
    CF_GENERIC_ERROR("CFBoolean objects can't be deallocated");
}

static CFTypeID __kCFBooleanTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFBooleanClass = {
    0,
    "CFBoolean",
    NULL, // init
    NULL, // copy
    __CFBooleanDeallocate,
    NULL,
    NULL,
    __CFBooleanCopyFormattingDescription,
    __CFBooleanCopyDescription
};

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL void _CFBooleanInitialize(void) {
    __kCFBooleanTypeID = _CFRuntimeRegisterClassBridge(&__CFBooleanClass, "NSCFBoolean");
    _CFRuntimeInitStaticInstance(&__kCFBooleanTrue, __kCFBooleanTypeID);
    _CFRuntimeInitStaticInstance(&__kCFBooleanFalse, __kCFBooleanTypeID);
}

///////////////////////////////////////////////////////////////////// public

const CFBooleanRef kCFBooleanTrue = &__kCFBooleanTrue;
const CFBooleanRef kCFBooleanFalse = &__kCFBooleanFalse;

CFTypeID CFBooleanGetTypeID(void) {
    return __kCFBooleanTypeID;
}

Boolean CFBooleanGetValue(CFBooleanRef boolean) {
    CF_OBJC_FUNCDISPATCH(Boolean, boolean, "boolValue");
    return (boolean == kCFBooleanTrue) ? true : false;
}
