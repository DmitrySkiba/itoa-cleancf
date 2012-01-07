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

#include <CoreFoundation/CFData.h>
#include "CFInternal.h"
#include <string.h>

// TODO __CFDataSetNumBytesUsed -> __CFDataSetLength
// TODO __CFDataSetNumBytes -> __CFDataSetCapacity

#define CF_VALIDATE_DATA_ARG(object) \
    CF_VALIDATE_OBJECT_ARG(CFObjC, object, __kCFDataTypeID)

#define CF_VALIDATE_MUTABLEDATA_ARG(data) \
    CF_VALIDATE_MUTABLEOBJECT_ARG(CFObjC, data, __kCFDataTypeID, \
        __CFMutableVariety(data) == kCFMutable || \
        __CFMutableVariety(data) == kCFFixedMutable)

struct __CFData {
    CFRuntimeBase _base;
    CFIndex _length; //number of bytes
    CFIndex _capacity; // maximum number of bytes
    CFAllocatorRef _bytesDeallocator; // used only for immutable; if NULL, no deallocation
    uint8_t* _bytes;
};

/* Bits 3-2 are used for mutability variation */
enum {
    kCFImmutable = 0x0, // unchangable and fixed capacity; default
    kCFMutable = 0x1, // changeable and variable capacity
    kCFFixedMutable = 0x3 // changeable and fixed capacity
};

static CFTypeID __kCFDataTypeID = _kCFRuntimeNotATypeID;

///////////////////////////////////////////////////////////////////// private

CF_INLINE UInt32 __CFMutableVariety(const void* cf) {
    return _CFBitfieldGetValue(CF_INFO(cf), 3, 2);
}

CF_INLINE void __CFSetMutableVariety(void* cf, UInt32 v) {
    _CFBitfieldSetValue(CF_INFO(cf), 3, 2, v);
}

CF_INLINE UInt32 __CFMutableVarietyFromFlags(UInt32 flags) {
    return _CFBitfieldGetValue(flags, 1, 0);
}

CF_INLINE CFIndex __CFDataLength(CFDataRef data) {
    return data->_length;
}

CF_INLINE void __CFDataSetLength(CFMutableDataRef data, CFIndex v) {
    /* for a CFData, _bytesUsed == _length */
}

CF_INLINE CFIndex __CFDataCapacity(CFDataRef data) {
    return data->_capacity;
}

CF_INLINE void __CFDataSetCapacity(CFMutableDataRef data, CFIndex v) {
    /* for a CFData, _bytesNum == _capacity */
}

CF_INLINE CFIndex __CFDataNumBytesUsed(CFDataRef data) {
    return data->_length;
}

CF_INLINE void __CFDataSetNumBytesUsed(CFMutableDataRef data, CFIndex v) {
    data->_length = v;
}

CF_INLINE CFIndex __CFDataNumBytes(CFDataRef data) {
    return data->_capacity;
}

CF_INLINE void __CFDataSetNumBytes(CFMutableDataRef data, CFIndex v) {
    data->_capacity = v;
}

CF_INLINE CFIndex __CFDataRoundUpCapacity(CFIndex capacity) {
    if (capacity < 16) {
        return 16;
    }
    // CF: quite probably, this doubling should slow as the data gets larger and larger; 
    //     should not use strict doubling
    return (1 << _CFLastBitSet(capacity));
}

CF_INLINE CFIndex __CFDataNumBytesForCapacity(CFIndex capacity) {
    return capacity;
}

static void __CFDataHandleOutOfMemory(CFTypeRef obj, CFIndex numBytes) {
    CFReportRuntimeError(
        kCFRuntimeErrorOutOfMemory, 
        CFSTR("Attempt to allocate %ld bytes for NS/CFData failed"), numBytes);
}

// NULL bytesDeallocator to this function does not mean the default allocator, 
//  it means that there should be no deallocator, and the bytes should be copied.
static CFMutableDataRef __CFDataInit(CFAllocatorRef allocator, 
                                     CFOptionFlags flags, CFIndex capacity, 
                                     const uint8_t* bytes, CFIndex length, 
                                     CFAllocatorRef bytesDeallocator)
{
    CF_VALIDATE_ARG(__CFMutableVarietyFromFlags(flags) != 0x2, 
        "flags 0x%x do not correctly specify the mutable variety", flags);
    CF_VALIDATE_ARG(
        __CFMutableVarietyFromFlags(flags) != kCFFixedMutable || 
            length <= capacity, 
        "for kCFFixedMutable type, capacity (%d) must be greater than or equal "
            "to number of initial elements (%d)",
        capacity, length);

    CFIndex size = sizeof(struct __CFData) - sizeof(CFRuntimeBase);
    if (__CFMutableVarietyFromFlags(flags) != kCFMutable) {
        if (!bytesDeallocator) {
            size += __CFDataNumBytesForCapacity(capacity);
        }
        size += 15; // for 16-byte alignment fixup
    }
    CFMutableDataRef instance = (CFMutableDataRef)_CFRuntimeCreateInstance(
		allocator, __kCFDataTypeID, size, NULL);
    if (!instance) {
        return NULL;
    }
    if (__CFMutableVarietyFromFlags(flags) != kCFMutable) {
        instance->_bytes = (uint8_t*)(((uintptr_t)(instance + 1) + 15) & ~0xF);
    }
    __CFDataSetNumBytesUsed(instance, 0);
    __CFDataSetLength(instance, 0);
    switch (__CFMutableVarietyFromFlags(flags)) {
        case kCFMutable:
            __CFDataSetCapacity(instance, __CFDataRoundUpCapacity(1));
            __CFDataSetNumBytes(instance, __CFDataNumBytesForCapacity(__CFDataRoundUpCapacity(1)));
            instance->_bytes = (uint8_t*)CFAllocatorAllocate(
				allocator, __CFDataNumBytes(instance), 0);
            if (!instance->_bytes) {
                CFRelease(instance);
                return NULL;
            }
            instance->_bytesDeallocator = NULL;
            __CFSetMutableVariety(instance, kCFMutable);
            CFDataReplaceBytes(instance, CFRangeMake(0, 0), bytes, length);
            break;
        case kCFFixedMutable:
            /* Don't round up capacity */
            __CFDataSetCapacity(instance, capacity);
            __CFDataSetNumBytes(instance, __CFDataNumBytesForCapacity(capacity));
            instance->_bytesDeallocator = NULL;
            __CFSetMutableVariety(instance, kCFFixedMutable);
            CFDataReplaceBytes(instance, CFRangeMake(0, 0), bytes, length);
            break;
        case kCFImmutable:
            /* Don't round up capacity */
            __CFDataSetCapacity(instance, capacity);
            __CFDataSetNumBytes(instance, __CFDataNumBytesForCapacity(capacity));
            if (bytesDeallocator != NULL) {
                instance->_bytes = (uint8_t*)bytes;
                instance->_bytesDeallocator = (CFAllocatorRef)CFRetain(bytesDeallocator);
                __CFDataSetNumBytesUsed(instance, length);
                __CFDataSetLength(instance, length);
            } else {
                instance->_bytesDeallocator = NULL;
                __CFSetMutableVariety(instance, kCFFixedMutable);
                CFDataReplaceBytes(instance, CFRangeMake(0, 0), bytes, length);
            }
            break;
    }
    __CFSetMutableVariety(instance, __CFMutableVarietyFromFlags(flags));
    return instance;
}

static void __CFDataGrow(CFMutableDataRef data, CFIndex numNewValues) {
    CFIndex oldLength = __CFDataLength(data);
    CFIndex capacity = __CFDataRoundUpCapacity(oldLength + numNewValues);
    CFAllocatorRef allocator = CFGetAllocator(data);
    __CFDataSetCapacity(data, capacity);
    __CFDataSetNumBytes(data, __CFDataNumBytesForCapacity(capacity));
    void* bytes = CFAllocatorReallocate(allocator, data->_bytes, __CFDataNumBytes(data), 0);
    if (!bytes) {
        __CFDataHandleOutOfMemory(data, __CFDataNumBytes(data));
    }
    data->_bytes = (uint8_t*)bytes;
}

static CFIndex __CFObjCDataGetLength(CFDataRef data) {
    CF_OBJC_FUNCDISPATCH(CFIndex, data, "length");
    return __CFDataLength(data);
}

/*** CFData class ***/

static void __CFDataDeallocate(CFTypeRef cf) {
    CFMutableDataRef data = (CFMutableDataRef)cf;
    CFAllocatorRef allocator = CFGetAllocator(data);
    switch (__CFMutableVariety(data)) {
        case kCFMutable:
            CFAllocatorDeallocate(allocator, data->_bytes);
            data->_bytes = NULL;
            break;
        case kCFFixedMutable:
            break;
        case kCFImmutable:
            if (data->_bytesDeallocator) {
                CFAllocatorDeallocate(data->_bytesDeallocator, data->_bytes);
                CFRelease(data->_bytesDeallocator);
                data->_bytes = NULL;
            }
            break;
    }
}

static Boolean __CFDataEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFDataRef data1 = (CFDataRef)cf1;
    CFDataRef data2 = (CFDataRef)cf2;
    CFIndex length;
    length = __CFDataLength(data1);
    if (length != __CFDataLength(data2)) {
        return false;
    }
    return !memcmp(data1->_bytes, data2->_bytes, length);
}

static CFHashCode __CFDataHash(CFTypeRef cf) {
    CFDataRef data = (CFDataRef)cf;
    return _CFHashBytes(data->_bytes, _CFMin(__CFDataLength(data), 80));
}

static CFStringRef __CFDataCopyDescription(CFTypeRef cf) {
    CFDataRef data = (CFDataRef)cf;
    CFMutableStringRef result;
    CFIndex idx;
    CFIndex len;
    const uint8_t* bytes;
    len = __CFDataLength(data);
    bytes = data->_bytes;
    result = CFStringCreateMutable(CFGetAllocator(data), 0);
    CFStringAppendFormat(result,
        NULL, CFSTR("<CFData %p [%p]>{length = %u, capacity = %u, bytes = 0x"), 
        cf, CFGetAllocator(data), len, __CFDataCapacity(data));
    if (24 < len) {
        for (idx = 0; idx < 16; idx += 4) {
            CFStringAppendFormat(result,
                NULL, CFSTR("%02x%02x%02x%02x"),
                bytes[idx], bytes[idx + 1], bytes[idx + 2], bytes[idx + 3]);
        }
        CFStringAppend(result, CFSTR(" ... "));
        for (idx = len - 8; idx < len; idx += 4) {
            CFStringAppendFormat(result,
                NULL, CFSTR("%02x%02x%02x%02x"),
                bytes[idx], bytes[idx + 1], bytes[idx + 2], bytes[idx + 3]);
        }
    } else {
        for (idx = 0; idx < len; idx++) {
            CFStringAppendFormat(result, NULL, CFSTR("%02x"), bytes[idx]);
        }
    }
    CFStringAppend(result, CFSTR("}"));
    return result;
}

static const CFRuntimeClass __CFDataClass = {
    0,
    "CFData",
    NULL, // init
    NULL, // copy
    __CFDataDeallocate,
    __CFDataEqual,
    __CFDataHash,
    NULL, //
    __CFDataCopyDescription
};

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL void _CFDataInitialize(void) {
    __kCFDataTypeID = _CFRuntimeRegisterClassBridge2(
        &__CFDataClass, 
        "NSCFData", "NSCFMutableData");
}

///////////////////////////////////////////////////////////////////// public

CFTypeID CFDataGetTypeID(void) {
    return __kCFDataTypeID;
}

CFDataRef CFDataCreate(CFAllocatorRef allocator, const uint8_t* bytes, CFIndex length) {
    CF_VALIDATE_LENGTH_PTR_ARGS(length, bytes);

    return __CFDataInit(allocator, kCFImmutable, length, bytes, length, NULL);
}

CFDataRef CFDataCreateWithBytesNoCopy(CFAllocatorRef allocator,
                                      const uint8_t* bytes, CFIndex length,
                                      CFAllocatorRef bytesDeallocator)
{
    CF_VALIDATE_LENGTH_PTR_ARGS(length, bytes);
    CF_VALIDATE_ALLOCATOR_ARG(bytesDeallocator);
    
    if (!bytesDeallocator) {
        bytesDeallocator = CFAllocatorGetDefault();
    }
    return __CFDataInit(allocator, kCFImmutable, length, bytes, length, bytesDeallocator);
}

CFDataRef CFDataCreateCopy(CFAllocatorRef allocator, CFDataRef data) {
    const uint8_t* bytes = CFDataGetBytePtr(data);
    CFIndex length = CFDataGetLength(data);
    
    return __CFDataInit(allocator, kCFImmutable, length, bytes, length, NULL);
}

CFDataRef CFDataCreateWithSubdata(CFAllocatorRef allocator, CFDataRef data, CFRange range) {
    CF_VALIDATE_RANGE_ARG(range, CFDataGetLength(data));
    
    const uint8_t* bytes = CFDataGetBytePtr(data) + range.location;
    CFIndex length = range.length;
    
    return __CFDataInit(allocator, kCFImmutable, length, bytes, length, NULL);
}

CFMutableDataRef CFDataCreateMutable(CFAllocatorRef allocator, CFIndex capacity) {
    CF_VALIDATE_NONNEGATIVE_ARG(capacity);

    CFMutableDataRef result = __CFDataInit(
        allocator,
        (capacity ? kCFFixedMutable : kCFMutable), capacity,
        NULL, 0, NULL);
    _CFRuntimeSetMutableObjcClass(result);
    return result;
}

CFMutableDataRef CFDataCreateMutableCopy(CFAllocatorRef allocator, CFIndex capacity, CFDataRef data) {
    CF_VALIDATE_NONNEGATIVE_ARG(capacity);

    const uint8_t* bytes = CFDataGetBytePtr(data);
    CFIndex length = CFDataGetLength(data);
    
    CFMutableDataRef result = __CFDataInit(
        allocator,
        (capacity ? kCFFixedMutable : kCFMutable), capacity,
        bytes, length, NULL);
    _CFRuntimeSetMutableObjcClass(result);
    return result;
}

CFIndex CFDataGetLength(CFDataRef data) {
    CF_VALIDATE_DATA_ARG(data);

    CF_OBJC_FUNCDISPATCH(CFIndex, data, "length");
    
    return __CFDataLength(data);
}

const uint8_t* CFDataGetBytePtr(CFDataRef data) {
    CF_VALIDATE_DATA_ARG(data);

    CF_OBJC_FUNCDISPATCH(const uint8_t*, data, "bytes");
    
    return data->_bytes;
}

uint8_t* CFDataGetMutableBytePtr(CFMutableDataRef data) {
    CF_VALIDATE_MUTABLEDATA_ARG(data);

    CF_OBJC_FUNCDISPATCH(uint8_t*, data, "mutableBytes");
    
    return data->_bytes;
}

void CFDataGetBytes(CFDataRef data, CFRange range, uint8_t* buffer) {
    CF_VALIDATE_RANGE_ARG(range, CFDataGetLength(data));
    CF_VALIDATE_PTR_ARG(buffer);

    CF_OBJC_VOID_FUNCDISPATCH(data, "getBytes:range:", buffer, range);
    
    memmove(buffer, data->_bytes + range.location, range.length);
}

void CFDataSetLength(CFMutableDataRef data, CFIndex length) {
    CF_VALIDATE_MUTABLEDATA_ARG(data);
    CF_VALIDATE_LENGTH_ARG(length);

    CF_OBJC_VOID_FUNCDISPATCH(data, "setLength:", length);

    CFIndex len = __CFDataLength(data);
    switch (__CFMutableVariety(data)) {
        case kCFMutable:
            if (len < length) {
                // CF: should only grow when new length exceeds current capacity, 
                //  not whenever it exceeds the current length
                __CFDataGrow(data, length - len);
            }
            break;
        case kCFFixedMutable:
            CF_VALIDATE_ARG(length <= __CFDataCapacity(data),
                "fixed-capacity data is full");
            break;
    }
    if (len < length) {
        memset(data->_bytes + len, 0, length - len);
    }
    __CFDataSetLength(data, length);
    __CFDataSetNumBytesUsed(data, length);
}

void CFDataIncreaseLength(CFMutableDataRef data, CFIndex extraLength) {
    CF_VALIDATE_MUTABLEDATA_ARG(data);
    CF_VALIDATE_ARG(
		(extraLength + CFDataGetLength(data)) >= 0,
        "extraLength is invalid");

    CF_OBJC_VOID_FUNCDISPATCH(data, "increaseLengthBy:", extraLength);

    CFDataSetLength(data, __CFDataLength(data) + extraLength);
}

void CFDataAppendBytes(CFMutableDataRef data, const uint8_t* bytes, CFIndex length) {
    CF_VALIDATE_MUTABLEDATA_ARG(data);
    CF_VALIDATE_LENGTH_PTR_ARGS(length, bytes);

    CF_OBJC_VOID_FUNCDISPATCH(data, "appendBytes:length:", bytes, length);

    CFDataReplaceBytes(data, CFRangeMake(__CFDataLength(data), 0), bytes, length);
}

void CFDataDeleteBytes(CFMutableDataRef data, CFRange range) {
    CF_VALIDATE_MUTABLEDATA_ARG(data);
    CF_VALIDATE_RANGE_ARG(range, CFDataGetLength(data));

    CF_OBJC_VOID_FUNCDISPATCH(data, "replaceBytesInRange:withBytes:length:", range, NULL, 0);

    CFDataReplaceBytes(data, range, NULL, 0);
}

void CFDataReplaceBytes(CFMutableDataRef data,
                        CFRange range,
                        const uint8_t* newBytes, CFIndex newLength)
{
    CF_VALIDATE_MUTABLEDATA_ARG(data);
    CF_VALIDATE_RANGE_ARG(range, CFDataGetLength(data));
    CF_VALIDATE_LENGTH_PTR_ARGS(newLength, newBytes);

    CF_OBJC_VOID_FUNCDISPATCH(data, "replaceBytesInRange:withBytes:length:", 
		range, newBytes, newLength);

    CFIndex len = __CFDataLength(data);
    CFIndex newCount = len - range.length + newLength;

    switch (__CFMutableVariety(data)) {
        case kCFMutable:
            if (__CFDataNumBytes(data) < newCount) {
                __CFDataGrow(data, newLength - range.length);
            }
            break;
        case kCFFixedMutable:
            CF_VALIDATE_ARG(newCount <= __CFDataCapacity(data),
                "fixed-capacity data is full");
            break;
    }
    if (newLength != range.length && range.location + range.length < len) {
        memmove(
            data->_bytes + range.location + newLength,
            data->_bytes + range.location + range.length,
            len - range.location - range.length);
    }
    if (0 < newLength) {
        memmove(data->_bytes + range.location, newBytes, newLength);
    }
    __CFDataSetNumBytesUsed(data, newCount);
    __CFDataSetLength(data, newCount);
}
