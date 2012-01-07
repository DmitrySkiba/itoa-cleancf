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

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL const void* _CFTypeCollectionRetain(CFAllocatorRef allocator, const void* ptr) {
    CFTypeRef cf = (CFTypeRef)ptr;
    return CFRetain(cf);
}

CF_INTERNAL void _CFTypeCollectionRelease(CFAllocatorRef allocator, const void* ptr) {
    CFTypeRef cf = (CFTypeRef)ptr;
    CFRelease(cf);
}

///////////////////////////////////////////////////////////////////// public

CFTypeID CFGetTypeID(CFTypeRef cf) {
    CF_VALIDATE_PTR_ARG(cf);
    if (_CFIsMallocZone(cf)) {
        return _CFNotATypeGetTypeID();
    }

    CF_OBJC_FUNCDISPATCH(CFTypeID, cf, "_cfTypeID");
    
    return CF_TYPEID(cf);
}

CFStringRef CFCopyTypeIDDescription(CFTypeID typeID) {
    const CFRuntimeClass* typeClass = _CFRuntimeGetClassWithTypeID(typeID);
    CF_VALIDATE_ARG(typeClass &&
        typeID != _CFNotATypeGetTypeID() && typeID != CFTypeGetTypeID(),
        "type ID %d is invalid", typeID);
    return CFStringCreateWithCString(
        kCFAllocatorSystemDefault,
        typeClass->className, kCFStringEncodingASCII);
}

CFTypeRef CFRetain(CFTypeRef cf) {
    CF_VALIDATE_PTR_ARG(cf);
    if (_CFIsMallocZone(cf)) {
        return cf;
    }

    CF_OBJC_FUNCDISPATCH(CFTypeRef, cf, "retain");
    
    int32_t rc;
    do {
        rc = CF_BASE(cf)->_rc;
        if (!rc) {
            // Static object.
            break;
        }
    } while (!OSAtomicCompareAndSwap32Barrier(rc, rc + 1, &CF_BASE(cf)->_rc));
    return cf;
}

void CFRelease(CFTypeRef cf) {
    CF_VALIDATE_PTR_ARG(cf);
    if (_CFIsMallocZone(cf)) {
        return;
    }

    CF_OBJC_VOID_FUNCDISPATCH(cf, "release");

    int32_t rc;
    do {
        rc = CF_BASE(cf)->_rc;
        if (!rc) {
            // Static object.
            break;
        }
        if (rc == 1) {
            // CANNOT WRITE ANY NEW VALUE INTO _RC UNTIL AFTER FINALIZATION
            CFTypeID typeID = CF_TYPEID(cf);
            const CFRuntimeClass* typeClass = _CFRuntimeGetClassWithTypeID(typeID);
            if (typeClass->finalize) {
                typeClass->finalize(cf);
            }
            if (typeID == CFAllocatorGetTypeID()) {
                break;
            }
            // We recheck _rc to see if the object has been retained again during
            // the finalization process.  This allows for the finalizer to resurrect,
            // but the main point is to allow finalizers to be able to manage the
            // removal of objects from uniquing caches, which may race with other threads
            // which are allocating (looking up and finding) objects from those caches,
            // which (that thread) would be the thing doing the extra retain in that case.
            if (OSAtomicCompareAndSwap32Barrier(1, 0, &CF_BASE(cf)->_rc)) {
                _CFRuntimeDestroyInstance(cf);
                break;
            }
        }
    } while (!OSAtomicCompareAndSwap32Barrier(rc, rc - 1, &CF_BASE(cf)->_rc));
}

CFIndex CFGetRetainCount(CFTypeRef cf) {
    CF_VALIDATE_PTR_ARG(cf);
    if (_CFIsMallocZone(cf)) {
        return (CFIndex)LONG_MAX;
    }

    CF_OBJC_FUNCDISPATCH(CFIndex, cf, "retainCount");
    
    int32_t rc = CF_BASE(cf)->_rc;
    return rc ? (CFIndex)rc : (CFIndex)LONG_MAX;
}


Boolean CFEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CF_VALIDATE_PTR_ARG(cf1);
    CF_VALIDATE_PTR_ARG(cf2);

    if (cf1 == cf2) {
        return true;
    }
    if (_CFIsMallocZone(cf1) || _CFIsMallocZone(cf2)) {
        return false;
    }

    CF_OBJC_FUNCDISPATCH(Boolean, cf1, "isEqual:", cf2);
    CF_OBJC_FUNCDISPATCH(Boolean, cf2, "isEqual:", cf1);

    if (CF_TYPEID(cf1) != CF_TYPEID(cf2)) {
        return false;
    }
    const CFRuntimeClass* typeClass = _CFRuntimeGetClassWithTypeID(CF_TYPEID(cf1));
    if (typeClass->equal) {
        return typeClass->equal(cf1, cf2);
    }
    return false;
}

CFHashCode CFHash(CFTypeRef cf) {
    CF_VALIDATE_PTR_ARG(cf);
    if (_CFIsMallocZone(cf)) {
        return (CFHashCode)cf;
    }

    CF_OBJC_FUNCDISPATCH(CFHashCode, cf, "hash");
    
    const CFRuntimeClass* typeClass = _CFRuntimeGetClassWithTypeID(CF_TYPEID(cf));
    if (typeClass->hash) {
        return typeClass->hash(cf);
    }
    return (CFHashCode)cf;
}

CFStringRef CFCopyDescription(CFTypeRef cf) {
    CF_VALIDATE_PTR_ARG(cf);
    if (_CFIsMallocZone(cf)) {
        return NULL;
    }

    CF_OBJC_FUNCDISPATCH(CFStringRef, cf, "_cfCopyDescription");
    
    const CFRuntimeClass* typeClass = _CFRuntimeGetClassWithTypeID(CF_TYPEID(cf));
    if (typeClass->copyDebugDesc) {
        CFStringRef result = typeClass->copyDebugDesc(cf);
        if (result) {
            return result;
        }
    }
    return CFStringCreateWithFormat(
        kCFAllocatorSystemDefault,
        NULL, CFSTR("<%s %p [%p]>"), 
        typeClass->className, cf, CFGetAllocator(cf));
}

CFStringRef CFCopyFormattingDescription(CFTypeRef cf, CFDictionaryRef formatOptions) {
    CF_VALIDATE_PTR_ARG(cf);
    if (_CFIsMallocZone(cf)) {
        return NULL;
    }
    
    if (_CFRuntimeIsInstanceOf(cf, CF_TYPEID(cf))) {
	    const CFRuntimeClass* typeClass = _CFRuntimeGetClassWithTypeID(CF_TYPEID(cf));
    	if (typeClass->copyFormattingDesc) {
        	return typeClass->copyFormattingDesc(cf, formatOptions);
	    }
    }
    return NULL;
}

CFAllocatorRef CFGetAllocator(CFTypeRef cf) {
    if (!cf) {
        return kCFAllocatorSystemDefault;
    }
    if (CF_IS_OBJC(cf)) {
        return (CFAllocatorRef)malloc_zone_from_ptr(cf);
    }
    if (_CFIsMallocZone(cf)) {
        return NULL;
    }
    if (CF_TYPEID(cf) == CFAllocatorGetTypeID()) {
        return _CFAllocatorGetAllocator(cf);
    }
    return _CFRuntimeGetInstanceAllocator(cf);
}

CFTypeRef CFMakeCollectable(CFTypeRef cf) {
    return cf;
}
