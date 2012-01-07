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

///////////////////////////////////////////////////////////////////// private

static void __CFInitHalt(CFTypeRef cf) {
    CF_GENERIC_ERROR("invalid call");
}
static CFTypeRef __CFCopyHalt(CFAllocatorRef allocator,CFTypeRef cf) {
    CF_GENERIC_ERROR("invalid call");
    return 0;
}
static void __CFFinalizeHalt(CFTypeRef cf) {
    CF_GENERIC_ERROR("invalid call");
}
static Boolean __CFEqualHalt(CFTypeRef cf1, CFTypeRef cf2) {
    CF_GENERIC_ERROR("invalid call");
    return FALSE;
}
static CFHashCode __CFHashHalt(CFTypeRef cf) {
    CF_GENERIC_ERROR("invalid call");
    return 0;
}
static CFStringRef __CFCopyFormattingDescHalt(CFTypeRef cf, CFDictionaryRef formatOptions) {
    CF_GENERIC_ERROR("invalid call");
    return 0;
}
static CFStringRef __CFCopyDebugDescHalt(CFTypeRef cf) {
    CF_GENERIC_ERROR("invalid call");
    return 0;
}

static CFTypeID __kCFNotATypeTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFNotATypeClass = {
    0,
    "Not A Type",
    __CFInitHalt,
    __CFCopyHalt,
    __CFFinalizeHalt,
    __CFEqualHalt,
    __CFHashHalt,
    __CFCopyFormattingDescHalt,
    __CFCopyDebugDescHalt
};

static CFTypeID __kCFTypeTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __CFTypeClass = {
    0,
    "CFType",
    __CFInitHalt,
    __CFCopyHalt,
    __CFFinalizeHalt,
    __CFEqualHalt,
    __CFHashHalt,
    __CFCopyFormattingDescHalt,
    __CFCopyDebugDescHalt
};

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL CFTypeID _CFNotATypeGetTypeID(void) {
    return __kCFNotATypeTypeID;
}

CF_INTERNAL void _CFTypeInitialize(void) {
    __kCFNotATypeTypeID = _CFRuntimeRegisterClassBridge(&__CFNotATypeClass, "NSCFType");
    if (__kCFNotATypeTypeID != 0) {
        CF_FATAL_ERROR("this function must be called first during CF initialization");
    }
    __kCFTypeTypeID = _CFRuntimeRegisterClass(&__CFTypeClass);
}

///////////////////////////////////////////////////////////////////// public

CFTypeID CFTypeGetTypeID() {
    return __kCFTypeTypeID;
}
