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
#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFStorage.h>
#include <CoreFoundation/CFSortFunctions.h>
#include <string.h>

#define CF_VALIDATE_ARRAY_ARG(array) \
    CF_VALIDATE_OBJECT_ARG(CFObjC, array, __kCFArrayTypeID)

#define CF_VALIDATE_MUTABLEARRAY_ARG(array) \
    CF_VALIDATE_MUTABLEOBJECT_ARG(CFObjC, array, __kCFArrayTypeID, \
	   __CFArrayGetType(array) != __kCFArrayImmutable);

//TODO replace raw bits manipulation with inline functions
//TODO rename helper structs like _releaseContext
//TOOD CF_VALIDATE_INDEX_ARG(idx, __CFArrayGetCount(array)) -> CF_VALIDATE_ARRAY_INDEX_ARG(idx, array);

typedef struct {
    const void* _item;
} __CFArrayBucket;

typedef struct {
    CFULong _leftIdx;
    CFULong _capacity;
    CFLong _bias;
    /* __CFArrayBucket buckets follow here */
} __CFArrayDeque;

typedef struct __CFArray {
    CFRuntimeBase _base;
    CFIndex _count; /* number of objects */
    CFIndex _mutations;
    void* _store;   /* can be NULL when MutableDeque */
} __CFArray;

enum {
    __CF_MAX_BUCKETS_PER_DEQUE = 262140
};

/* Flag bits */
enum {
    /* Bits 0-1 */
    __kCFArrayImmutable = 0,
    __kCFArrayDeque = 2,
    __kCFArrayStorage = 3,

    /* Bits 2-3 */
    __kCFArrayHasNullCallBacks = 0,
    __kCFArrayHasCFTypeCallBacks = 1,
    __kCFArrayHasCustomCallBacks = 3
};

struct _releaseContext {
    void (* release)(CFAllocatorRef, const void*);
    CFAllocatorRef allocator;
};

struct _acompareContext {
    CFComparatorFunction func;
    void* context;
};

static CFTypeID __kCFArrayTypeID = _kCFRuntimeNotATypeID;

///////////////////////////////////////////////////////////////////// private

static const CFArrayCallBacks __kCFNullArrayCallBacks = {
    0,
    NULL,
    NULL,
    NULL,
    NULL
};

CF_INLINE CFIndex __CFArrayDequeRoundUpCapacity(CFIndex capacity) {
    if (capacity < 4) {
        return 4;
    }
    return _CFMin((1 << _CFLastBitSet(capacity)), __CF_MAX_BUCKETS_PER_DEQUE);
}

CF_INLINE CFIndex __CFArrayGetType(CFArrayRef array) {
    return _CFBitfieldGetValue(((const CFRuntimeBase*)array)->_cfinfo[CF_INFO_BITS], 1, 0);
}

CF_INLINE CFIndex __CFArrayGetSizeOfType(CFIndex t) {
    CFIndex size = 0;
    size += sizeof(__CFArray);
    if (_CFBitfieldGetValue(t, 3, 2) == __kCFArrayHasCustomCallBacks) {
        size += sizeof(CFArrayCallBacks);
    }
    return size;
}

CF_INLINE CFIndex __CFArrayGetCount(CFArrayRef array) {
    return array->_count;
}

CF_INLINE void __CFArraySetCount(CFArrayRef array, CFIndex v) {
    ((__CFArray*)array)->_count = v;
}

/* Only applies to immutable and mutable-deque-using arrays;
 * Returns the bucket holding the left-most real value in the latter case. */
CF_INLINE __CFArrayBucket* __CFArrayGetBucketsPtr(CFArrayRef array) {
    switch (__CFArrayGetType(array)) {
        case __kCFArrayImmutable:
            return (__CFArrayBucket*)((uint8_t*)array + __CFArrayGetSizeOfType(((CFRuntimeBase*)array)->_cfinfo[CF_INFO_BITS]));
        case __kCFArrayDeque: {
            __CFArrayDeque* deque = (__CFArrayDeque*)array->_store;
            return (__CFArrayBucket*)((uint8_t*)deque + sizeof(__CFArrayDeque) + deque->_leftIdx * sizeof(__CFArrayBucket));
        }
    }
    return NULL;
}

/* This shouldn't be called if the array count is 0. */
CF_INLINE __CFArrayBucket* __CFArrayGetBucketAtIndex(CFArrayRef array, CFIndex idx) {
    switch (__CFArrayGetType(array)) {
        case __kCFArrayImmutable:
        case __kCFArrayDeque:
            return __CFArrayGetBucketsPtr(array) + idx;
        case __kCFArrayStorage: {
            CFStorageRef store = (CFStorageRef)array->_store;
            return (__CFArrayBucket*)CFStorageGetValueAtIndex(store, idx, NULL);
        }
    }
    return NULL;
}

CF_INLINE CFArrayCallBacks* __CFArrayGetCallBacks(CFArrayRef array) {
    CFArrayCallBacks* result = NULL;
    switch (_CFBitfieldGetValue(((const CFRuntimeBase*)array)->_cfinfo[CF_INFO_BITS], 3, 2)) {
        case __kCFArrayHasNullCallBacks:
            return (CFArrayCallBacks*)&__kCFNullArrayCallBacks;
        case __kCFArrayHasCFTypeCallBacks:
            return (CFArrayCallBacks*)&kCFTypeArrayCallBacks;
        case __kCFArrayHasCustomCallBacks:
            break;
    }
    switch (__CFArrayGetType(array)) {
        case __kCFArrayImmutable:
            result = (CFArrayCallBacks*)((uint8_t*)array + sizeof(__CFArray));
            break;
        case __kCFArrayDeque:
        case __kCFArrayStorage:
            result = (CFArrayCallBacks*)((uint8_t*)array + sizeof(__CFArray));
            break;
    }
    return result;
}

CF_INLINE bool __CFArrayCallBacksMatchNull(const CFArrayCallBacks* c) {
    return (!c ||
            (c->retain == __kCFNullArrayCallBacks.retain &&
             c->release == __kCFNullArrayCallBacks.release &&
             c->copyDescription == __kCFNullArrayCallBacks.copyDescription &&
             c->equal == __kCFNullArrayCallBacks.equal));
}

CF_INLINE bool __CFArrayCallBacksMatchCFType(const CFArrayCallBacks* c) {
    return (&kCFTypeArrayCallBacks == c ||
            (c->retain == kCFTypeArrayCallBacks.retain &&
             c->release == kCFTypeArrayCallBacks.release &&
             c->copyDescription == kCFTypeArrayCallBacks.copyDescription &&
             c->equal == kCFTypeArrayCallBacks.equal));
}

static void __CFArrayStorageRelease(const void* itemptr, void* context) {
    struct _releaseContext* rc = (struct _releaseContext*)context;
    rc->release(rc->allocator, *(const void**)itemptr);
    *(const void**)itemptr = NULL;  // GC:  clear item to break strong reference.
}

static void __CFArrayReleaseValues(CFArrayRef array, CFRange range, bool releaseStorageIfPossible) {
    const CFArrayCallBacks* cb = __CFArrayGetCallBacks(array);
    CFAllocatorRef allocator;
    CFIndex idx;
    switch (__CFArrayGetType(array)) {
        case __kCFArrayImmutable:
            if (cb->release && 0 < range.length) {
                // if we've been finalized then we know that
                // 1) we're using the standard callback on GC memory
                // 2) the slots don't' need to be zeroed
                __CFArrayBucket* buckets = __CFArrayGetBucketsPtr(array);
                allocator = CFGetAllocator(array);
                for (idx = 0; idx < range.length; idx++) {
                    cb->release(allocator, buckets[idx + range.location]._item);
                    buckets[idx + range.location]._item = NULL; // GC:  break strong reference.
                }
            }
            break;
        case __kCFArrayDeque: {
            __CFArrayDeque* deque = (__CFArrayDeque*)array->_store;
            if (0 < range.length && deque) {
                __CFArrayBucket* buckets = __CFArrayGetBucketsPtr(array);
                if (cb->release) {
                    allocator = CFGetAllocator(array);
                    for (idx = 0; idx < range.length; idx++) {
                        cb->release(allocator, buckets[idx + range.location]._item);
                        buckets[idx + range.location]._item = NULL; // GC:  break strong reference.
                    }
                } else {
                    for (idx = 0; idx < range.length; idx++) {
                        buckets[idx + range.location]._item = NULL; // GC:  break strong reference.
                    }
                }
            }
            if (releaseStorageIfPossible && !range.location && __CFArrayGetCount(array) == range.length) {
                allocator = CFGetAllocator(array);
                if (deque) {
                    CFAllocatorDeallocate(allocator, deque);
                }
                __CFArraySetCount(array, 0); // GC: _count == 0 ==> _store == NULL.
                ((__CFArray*)array)->_store = NULL;
            }
            break;
        }
        case __kCFArrayStorage: {
            CFStorageRef store = (CFStorageRef)array->_store;
            if (cb->release && 0 < range.length) {
                struct _releaseContext context;
                allocator = CFGetAllocator(array);
                context.release = cb->release;
                context.allocator = allocator;
                CFStorageApplyFunction(store, range, __CFArrayStorageRelease, &context);
            }
            if (releaseStorageIfPossible && !range.location && __CFArrayGetCount(array) == range.length) {
                CFRelease(store);
                __CFArraySetCount(array, 0); // GC: _count == 0 ==> _store == NULL.
                ((__CFArray*)array)->_store = NULL;
                _CFBitfieldSetValue(((CFRuntimeBase*)array)->_cfinfo[CF_INFO_BITS], 1, 0, __kCFArrayDeque);
            }
            break;
        }
    }
}

static void __CFResourceRelease(CFTypeRef cf, const void* ignored) {
    kCFTypeArrayCallBacks.release(kCFAllocatorSystemDefault, cf);
}

static CFArrayRef __CFArrayInit(CFAllocatorRef allocator, UInt32 flags, CFIndex capacity, const CFArrayCallBacks* callBacks) {
    __CFArray* memory;
    CFIndex size;
    _CFBitfieldSetValue(flags, 31, 2, 0);
    if (__CFArrayCallBacksMatchNull(callBacks)) {
        _CFBitfieldSetValue(flags, 3, 2, __kCFArrayHasNullCallBacks);
    } else if (__CFArrayCallBacksMatchCFType(callBacks)) {
        _CFBitfieldSetValue(flags, 3, 2, __kCFArrayHasCFTypeCallBacks);
    } else {
        _CFBitfieldSetValue(flags, 3, 2, __kCFArrayHasCustomCallBacks);
    }
    size = __CFArrayGetSizeOfType(flags) - sizeof(CFRuntimeBase);
    switch (_CFBitfieldGetValue(flags, 1, 0)) {
        case __kCFArrayImmutable:
            size += capacity * sizeof(__CFArrayBucket);
            break;
        case __kCFArrayDeque:
        case __kCFArrayStorage:
            break;
    }
    memory = (__CFArray*)_CFRuntimeCreateInstance(allocator, __kCFArrayTypeID, size, NULL);
    if (!memory) {
        return NULL;
    }
    _CFBitfieldSetValue(memory->_base._cfinfo[CF_INFO_BITS], 6, 0, flags);
    __CFArraySetCount((CFArrayRef)memory, 0);
    switch (_CFBitfieldGetValue(flags, 1, 0)) {
        case __kCFArrayDeque:
        case __kCFArrayStorage:
            ((__CFArray*)memory)->_mutations = 1;
            ((__CFArray*)memory)->_store = NULL;
            break;
    }
    if (__kCFArrayHasCustomCallBacks == _CFBitfieldGetValue(flags, 3, 2)) {
        CFArrayCallBacks* cb = (CFArrayCallBacks*)__CFArrayGetCallBacks((CFArrayRef)memory);
        *cb = *callBacks;
    }
    return (CFArrayRef)memory;
}

static void __CFArrayConvertDequeToStore(CFMutableArrayRef array) {
    __CFArrayDeque* deque = (__CFArrayDeque*)array->_store;
    __CFArrayBucket* raw_buckets = (__CFArrayBucket*)((uint8_t*)deque + sizeof(__CFArrayDeque));
    CFStorageRef store;
    CFIndex count = CFArrayGetCount(array);
    CFAllocatorRef allocator = CFGetAllocator(array);
    store = (CFStorageRef)CFMakeCollectable(CFStorageCreate(allocator, sizeof(const void*)));
    array->_store = store;
    CFStorageInsertValues(store, CFRangeMake(0, count));
    CFStorageReplaceValues(store, CFRangeMake(0, count), raw_buckets + deque->_leftIdx);
    CFAllocatorDeallocate(CFGetAllocator(array), deque);
    _CFBitfieldSetValue(((CFRuntimeBase*)array)->_cfinfo[CF_INFO_BITS], 1, 0, __kCFArrayStorage);
}

static void __CFArrayConvertStoreToDeque(CFMutableArrayRef array) {
    CFStorageRef store = (CFStorageRef)array->_store;
    __CFArrayDeque* deque;
    __CFArrayBucket* raw_buckets;
    CFIndex count = CFStorageGetCount(store); // storage, not array, has correct count at this point
    // do not resize down to a completely tight deque
    CFIndex capacity = __CFArrayDequeRoundUpCapacity(count + 6);
    CFIndex size = sizeof(__CFArrayDeque) + capacity * sizeof(__CFArrayBucket);
    CFAllocatorRef allocator = CFGetAllocator(array);
    deque = (__CFArrayDeque*)CFAllocatorAllocate(allocator, size, 0);
    deque->_leftIdx = (capacity - count) / 2;
    deque->_capacity = capacity;
    deque->_bias = 0;
    array->_store = deque;
    raw_buckets = (__CFArrayBucket*)((uint8_t*)deque + sizeof(__CFArrayDeque));
    CFStorageGetValues(store, CFRangeMake(0, count), raw_buckets + deque->_leftIdx);
    CFRelease(store); // GC:  balances CFMakeCollectable() above.
    _CFBitfieldSetValue(((CFRuntimeBase*)array)->_cfinfo[CF_INFO_BITS], 1, 0, __kCFArrayDeque);
}

// may move deque storage, as it may need to grow deque
static void __CFArrayRepositionDequeRegions(CFMutableArrayRef array, CFRange range, CFIndex newCount) {
    // newCount elements are going to replace the range, and the result will fit in the deque
    __CFArrayDeque* deque = (__CFArrayDeque*)array->_store;
    __CFArrayBucket* buckets;
    CFIndex cnt, futureCnt, numNewElems;
    CFIndex L, A, B, C, R;

    buckets = (__CFArrayBucket*)((uint8_t*)deque + sizeof(__CFArrayDeque));
    cnt = __CFArrayGetCount(array);
    futureCnt = cnt - range.length + newCount;

    L = deque->_leftIdx;               // length of region to left of deque
    A = range.location;                // length of region in deque to left of replaced range
    B = range.length;                  // length of replaced range
    C = cnt - B - A;                   // length of region in deque to right of replaced range
    R = deque->_capacity - cnt - L;    // length of region to right of deque
    numNewElems = newCount - B;

    CFIndex wiggle = deque->_capacity >> 17;
    if (wiggle < 4) {
        wiggle = 4;
    }
    if (deque->_capacity < (uint32_t)futureCnt || (cnt < futureCnt && L + R < wiggle)) {
        // must be inserting or space is tight, reallocate and re-center everything
        CFIndex capacity = __CFArrayDequeRoundUpCapacity(futureCnt + wiggle);
        CFIndex size = sizeof(__CFArrayDeque) + capacity * sizeof(__CFArrayBucket);
        CFAllocatorRef allocator = CFGetAllocator(array);
        __CFArrayDeque* newDeque = (__CFArrayDeque*)CFAllocatorAllocate(allocator, size, 0);
        __CFArrayBucket* newBuckets = (__CFArrayBucket*)((uint8_t*)newDeque + sizeof(__CFArrayDeque));
        CFIndex oldL = L;
        CFIndex newL = (capacity - futureCnt) / 2;
        CFIndex oldC0 = oldL + A + B;
        CFIndex newC0 = newL + A + newCount;
        newDeque->_leftIdx = newL;
        newDeque->_capacity = capacity;
        newDeque->_bias = 0;
        if (0 < A) {
            memmove(newBuckets + newL, buckets + oldL, A * sizeof(__CFArrayBucket));
        }
        if (0 < C) {
            memmove(newBuckets + newC0, buckets + oldC0, C * sizeof(__CFArrayBucket));
        }
        if (deque) {
            CFAllocatorDeallocate(allocator, deque);
        }
        array->_store = newDeque;
        return;
    }

    if ((numNewElems < 0 && C < A) || (numNewElems <= R && C < A)) {    // move C
        // deleting: C is smaller
        // inserting: C is smaller and R has room
        CFIndex oldC0 = L + A + B;
        CFIndex newC0 = L + A + newCount;
        if (0 < C) {
            memmove(buckets + newC0, buckets + oldC0, C * sizeof(__CFArrayBucket));
        }
        // GrP GC: zero-out newly exposed space on the right, if any
        if (oldC0 > newC0) {
            memset(buckets + newC0 + C, 0x00, (oldC0 - newC0) * sizeof(__CFArrayBucket));
        }
    } else if ((numNewElems < 0) || (numNewElems <= L && A <= C)) {    // move A
        // deleting: A is smaller or equal (covers remaining delete cases)
        // inserting: A is smaller and L has room
        CFIndex oldL = L;
        CFIndex newL = L - numNewElems;
        deque->_leftIdx = newL;
        if (0 < A) {
            memmove(buckets + newL, buckets + oldL, A * sizeof(__CFArrayBucket));
        }
        // GrP GC: zero-out newly exposed space on the left, if any
        if (newL > oldL) {
            memset(buckets + oldL, 0x00, (newL - oldL) * sizeof(__CFArrayBucket));
        }
    } else {
        // now, must be inserting, and either:
        // A<=C, but L doesn't have room (R might have, but don't care)
        // C<A, but R doesn't have room (L might have, but don't care)
        // re-center everything
        CFIndex oldL = L;
        CFIndex newL = (L + R - numNewElems) / 2;
        CFIndex oldBias = deque->_bias;
        deque->_bias = (newL < oldL) ? -1 : 1;
        if (oldBias < 0) {
            newL = newL - newL / 2;
        } else if (0 < oldBias) {
            newL = newL + newL / 2;
        }
        CFIndex oldC0 = oldL + A + B;
        CFIndex newC0 = newL + A + newCount;
        deque->_leftIdx = newL;
        if (newL < oldL) {
            if (0 < A) {
                memmove(buckets + newL, buckets + oldL, A * sizeof(__CFArrayBucket));
            }
            if (0 < C) {
                memmove(buckets + newC0, buckets + oldC0, C * sizeof(__CFArrayBucket));
            }
            // GrP GC: zero-out newly exposed space on the right, if any
            if (oldC0 > newC0) {
                memset(buckets + newC0 + C, 0x00, (oldC0 - newC0) * sizeof(__CFArrayBucket));
            }
        } else {
            if (0 < C) {
                memmove(buckets + newC0, buckets + oldC0, C * sizeof(__CFArrayBucket));
            }
            if (0 < A) {
                memmove(buckets + newL, buckets + oldL, A * sizeof(__CFArrayBucket));
            }
            // GrP GC: zero-out newly exposed space on the left, if any
            if (newL > oldL) {
                memset(buckets + oldL, 0x00, (newL - oldL) * sizeof(__CFArrayBucket));
            }
        }
    }
}

static void __CFArrayHandleOutOfMemory(CFTypeRef obj, CFIndex numBytes) {
    CFReportRuntimeError(
        kCFRuntimeErrorOutOfMemory, 
        CFSTR("Attempt to allocate %ld bytes for CFArray failed"), numBytes);
}

static CFComparisonResult __CFArrayCompareValues(const void* v1, const void* v2, struct _acompareContext* context) {
    const void** val1 = (const void**)v1;
    const void** val2 = (const void**)v2;
    return (CFComparisonResult)(context->func(*val1, *val2, context->context));
}

/***** CFArray class *****/

static void __CFArrayDeallocate(CFTypeRef cf) {
    CFArrayRef array = (CFArrayRef)cf;
    __CFArrayReleaseValues(array, CFRangeMake(0, __CFArrayGetCount(array)), true);
}

static Boolean __CFArrayEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFArrayRef array1 = (CFArrayRef)cf1;
    CFArrayRef array2 = (CFArrayRef)cf2;
    const CFArrayCallBacks* cb1, * cb2;
    CFIndex idx, cnt;
    if (array1 == array2) {
        return true;
    }
    cnt = __CFArrayGetCount(array1);
    if (cnt != __CFArrayGetCount(array2)) {
        return false;
    }
    cb1 = __CFArrayGetCallBacks(array1);
    cb2 = __CFArrayGetCallBacks(array2);
    if (cb1->equal != cb2->equal) {
        return false;
    }
    if (!cnt) {
        return true;              /* after function comparison! */
    }
    for (idx = 0; idx < cnt; idx++) {
        const void* val1 = __CFArrayGetBucketAtIndex(array1, idx)->_item;
        const void* val2 = __CFArrayGetBucketAtIndex(array2, idx)->_item;
        if (val1 != val2) {
            if (!cb1->equal) {
                return false;
            }
            if (!cb1->equal(val1, val2)) {
                return false;
            }
        }
    }
    return true;
}

static CFHashCode __CFArrayHash(CFTypeRef cf) {
    CFArrayRef array = (CFArrayRef)cf;
    return __CFArrayGetCount(array);
}

static CFStringRef __CFArrayCopyDescription(CFTypeRef cf) {
    CFArrayRef array = (CFArrayRef)cf;
    CFMutableStringRef result;
    const CFArrayCallBacks* cb;
    CFAllocatorRef allocator;
    CFIndex idx, cnt;
    cnt = __CFArrayGetCount(array);
    allocator = CFGetAllocator(array);
    result = CFStringCreateMutable(allocator, 0);
    switch (__CFArrayGetType(array)) {
        case __kCFArrayImmutable:
            CFStringAppendFormat(result, NULL, CFSTR("<CFArray %p [%p]>{type = immutable, count = %u, values = (\n"), cf, allocator, cnt);
            break;
        case __kCFArrayDeque:
            CFStringAppendFormat(result, NULL, CFSTR("<CFArray %p [%p]>{type = mutable-small, count = %u, values = (\n"), cf, allocator, cnt);
            break;
        case __kCFArrayStorage:
            CFStringAppendFormat(result, NULL, CFSTR("<CFArray %p [%p]>{type = mutable-large, count = %u, values = (\n"), cf, allocator, cnt);
            break;
    }
    cb = __CFArrayGetCallBacks(array);
    for (idx = 0; idx < cnt; idx++) {
        CFStringRef desc = NULL;
        const void* val = __CFArrayGetBucketAtIndex(array, idx)->_item;
        if (cb->copyDescription) {
            desc = (CFStringRef)cb->copyDescription(val);
        }
        if (desc) {
            CFStringAppendFormat(result, NULL, CFSTR("\t%u : %@\n"), idx, desc);
            CFRelease(desc);
        } else {
            CFStringAppendFormat(result, NULL, CFSTR("\t%u : <%p>\n"), idx, val);
        }
    }
    CFStringAppend(result, CFSTR(")}"));
    return result;
}

static const CFRuntimeClass __CFArrayClass = {
    0,
    "CFArray",
    NULL,               // init
    NULL,               // copy
    __CFArrayDeallocate,
    __CFArrayEqual,
    __CFArrayHash,
    NULL,               //
    __CFArrayCopyDescription
};

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL void _CFArrayInitialize(void) {
    __kCFArrayTypeID = _CFRuntimeRegisterClass(&__CFArrayClass);
}

// This creates an array which is for CFTypes or NSObjects, with an ownership transfer --
// the array does not take a retain, and the caller does not need to release the inserted objects.
// The incoming objects must also be collectable if allocated out of a collectable allocator.
CF_INTERNAL CFArrayRef _CFArrayCreate_ex(CFAllocatorRef allocator, Boolean isMutable, const void** values, CFIndex numValues) {
    CFArrayRef result;
    result = __CFArrayInit(allocator, isMutable ? __kCFArrayDeque : __kCFArrayImmutable, numValues, &kCFTypeArrayCallBacks);
    if (!isMutable) {
        __CFArrayBucket* buckets = __CFArrayGetBucketsPtr(result);
        memmove(buckets, values, numValues * sizeof(__CFArrayBucket));
    } else {
        if (__CF_MAX_BUCKETS_PER_DEQUE <= numValues) {
            CFStorageRef store = (CFStorageRef)CFMakeCollectable(CFStorageCreate(allocator, sizeof(const void*)));
            ((CFMutableArrayRef)result)->_store = store;
            CFStorageInsertValues(store, CFRangeMake(0, numValues));
            CFStorageReplaceValues(store, CFRangeMake(0, numValues), values);
            _CFBitfieldSetValue(((CFRuntimeBase*)result)->_cfinfo[CF_INFO_BITS], 1, 0, __kCFArrayStorage);
        } else if (0 <= numValues) {
            __CFArrayDeque* deque;
            __CFArrayBucket* raw_buckets;
            CFIndex capacity = __CFArrayDequeRoundUpCapacity(numValues);
            CFIndex size = sizeof(__CFArrayDeque) + capacity * sizeof(__CFArrayBucket);
            deque = (__CFArrayDeque*)CFAllocatorAllocate(allocator, size, 0);
            deque->_leftIdx = (capacity - numValues) / 2;
            deque->_capacity = capacity;
            deque->_bias = 0;
            ((CFMutableArrayRef)result)->_store = deque;
            raw_buckets = (__CFArrayBucket*)((uint8_t*)deque + sizeof(__CFArrayDeque));
            memmove(raw_buckets + deque->_leftIdx + 0, values, numValues * sizeof(__CFArrayBucket));
            _CFBitfieldSetValue(((CFRuntimeBase*)result)->_cfinfo[CF_INFO_BITS], 1, 0, __kCFArrayDeque);
        }
    }
    __CFArraySetCount(result, numValues);
    return result;
}

///////////////////////////////////////////////////////////////////// public

const CFArrayCallBacks kCFTypeArrayCallBacks = {
    0,
    _CFTypeCollectionRetain,
    _CFTypeCollectionRelease,
    CFCopyDescription,
    CFEqual
};

CFTypeID CFArrayGetTypeID(void) {
    return __kCFArrayTypeID;
}

CFArrayRef CFArrayCreate(CFAllocatorRef allocator, const void** values, CFIndex numValues, const CFArrayCallBacks* callBacks) {
    CF_VALIDATE_NONNEGATIVE_ARG(numValues);
    if (numValues) {
    	CF_VALIDATE_PTR_ARG(values);
    }

    CFIndex idx;
    CFArrayRef result = __CFArrayInit(allocator, __kCFArrayImmutable, numValues, callBacks);
    const CFArrayCallBacks* cb = __CFArrayGetCallBacks(result);
    __CFArrayBucket* buckets = __CFArrayGetBucketsPtr(result);
    if (cb->retain) {
        for (idx = 0; idx < numValues; idx++) {
            buckets->_item = cb->retain(allocator, *values);
            values++;
            buckets++;
        }
    } else {
        for (idx = 0; idx < numValues; idx++) {
            buckets->_item = *values;
            values++;
            buckets++;
        }
    }
    __CFArraySetCount(result, numValues);
    return result;
}

CFMutableArrayRef CFArrayCreateMutable(CFAllocatorRef allocator, CFIndex capacity, const CFArrayCallBacks* callBacks) {
    CF_VALIDATE_NONNEGATIVE_ARG(capacity);
    CF_VALIDATE_ARG(capacity <= (CFIndex)(LONG_MAX / sizeof(void*)), 
        "capacity (%d) is too large for this architecture", capacity);

    return (CFMutableArrayRef)__CFArrayInit(allocator, __kCFArrayDeque, capacity, callBacks);
}

CFArrayRef CFArrayCreateCopy(CFAllocatorRef allocator, CFArrayRef array) {
    CF_VALIDATE_ARRAY_ARG(array);

    CFArrayRef result;
    const CFArrayCallBacks* cb;
    __CFArrayBucket* buckets;
    CFIndex numValues = CFArrayGetCount(array);
    CFIndex idx;
    if (CF_IS_OBJC(array)) {
        cb = &kCFTypeArrayCallBacks;
    } else {
        cb = __CFArrayGetCallBacks(array);
    }
    result = __CFArrayInit(allocator, __kCFArrayImmutable, numValues, cb);
    cb = __CFArrayGetCallBacks(result); // GC: use the new array's callbacks so we don't leak.
    buckets = __CFArrayGetBucketsPtr(result);
    for (idx = 0; idx < numValues; idx++) {
        const void* value = CFArrayGetValueAtIndex(array, idx);
        if (cb->retain) {
            value = (void*)cb->retain(allocator, value);
        }
        buckets->_item = value;
        buckets++;
    }
    __CFArraySetCount(result, numValues);
    return result;
}

CFMutableArrayRef CFArrayCreateMutableCopy(CFAllocatorRef allocator, CFIndex capacity, CFArrayRef array) {
    CF_VALIDATE_ARRAY_ARG(array);

    CFMutableArrayRef result;
    const CFArrayCallBacks* cb;
    CFIndex idx, numValues = CFArrayGetCount(array);
    UInt32 flags;
    if (CF_IS_OBJC(array)) {
        cb = &kCFTypeArrayCallBacks;
    } else {
        cb = __CFArrayGetCallBacks(array);
    }
    flags = __kCFArrayDeque;
    result = (CFMutableArrayRef)__CFArrayInit(allocator, flags, capacity, cb);
    if (!capacity) {
        _CFArraySetCapacity(result, numValues);
    }
    for (idx = 0; idx < numValues; idx++) {
        const void* value = CFArrayGetValueAtIndex(array, idx);
        CFArrayAppendValue(result, value);
    }
    return result;
}

CFIndex CFArrayGetCount(CFArrayRef array) {
    CF_VALIDATE_ARRAY_ARG(array);

    CF_OBJC_FUNCDISPATCH(CFIndex, array, "count");

    return __CFArrayGetCount(array);
}

CFIndex CFArrayGetCountOfValue(CFArrayRef array, CFRange range, const void* value) {
    CF_VALIDATE_ARRAY_ARG(array);
    CF_VALIDATE_RANGE_ARG(range, CFArrayGetCount(array));

    // TODO CF: this ignores range
    CF_OBJC_FUNCDISPATCH(CFIndex, array, 
        "countOccurrences:", value);

    const CFArrayCallBacks* cb;
    CFIndex idx, count = 0;
    cb = __CFArrayGetCallBacks(array);
    for (idx = 0; idx < range.length; idx++) {
        const void* item = __CFArrayGetBucketAtIndex(array, range.location + idx)->_item;
        if (value == item || (cb->equal && cb->equal(value, item))) {
            count++;
        }
    }
    return count;
}

Boolean CFArrayContainsValue(CFArrayRef array, CFRange range, const void* value) {
    CF_VALIDATE_ARRAY_ARG(array);
    CF_VALIDATE_RANGE_ARG(range, CFArrayGetCount(array));

    CF_OBJC_FUNCDISPATCH(char, array, 
        "containsObject:inRange:", value, range);

    CFIndex idx;
    for (idx = 0; idx < range.length; idx++) {
        const void* item = __CFArrayGetBucketAtIndex(array, range.location + idx)->_item;
        if (value == item) {
            return true;
        }
    }
    const CFArrayCallBacks* cb = __CFArrayGetCallBacks(array);
    if (cb->equal) {
        for (idx = 0; idx < range.length; idx++) {
            const void* item = __CFArrayGetBucketAtIndex(array, range.location + idx)->_item;
            if (cb->equal(value, item)) {
                return true;
            }
        }
    }
    return false;
}

const void* CFArrayGetValueAtIndex(CFArrayRef array, CFIndex idx) {
    CF_VALIDATE_ARRAY_ARG(array);
    CF_VALIDATE_INDEX_ARG(idx, __CFArrayGetCount(array));

    CF_OBJC_FUNCDISPATCH(void*, array, "objectAtIndex:", idx);

    return __CFArrayGetBucketAtIndex(array, idx)->_item;
}

void CFArrayGetValues(CFArrayRef array, CFRange range, const void** values) {
    CF_VALIDATE_ARRAY_ARG(array);
    CF_VALIDATE_RANGE_ARG(range, CFArrayGetCount(array));
    CF_VALIDATE_NONZERO_ARG(values);

    CF_OBJC_VOID_FUNCDISPATCH(array, 
        "getObjects:range:", values, range);

    if (0 < range.length) {
        switch (__CFArrayGetType(array)) {
            case __kCFArrayImmutable:
            case __kCFArrayDeque:
                memmove(
                    values,
                    __CFArrayGetBucketsPtr(array) + range.location,
                    range.length * sizeof(__CFArrayBucket));
                break;
            case __kCFArrayStorage: {
                CFStorageRef store = (CFStorageRef)array->_store;
                CFStorageGetValues(store, range, values);
                break;
            }
        }
    }
}

void CFArrayApplyFunction(CFArrayRef array, CFRange range, CFArrayApplierFunction applier, void* context) {
    CF_VALIDATE_ARRAY_ARG(array);
    CF_VALIDATE_RANGE_ARG(range, CFArrayGetCount(array));
    CF_VALIDATE_NONZERO_ARG(applier);

    CF_OBJC_VOID_FUNCDISPATCH(array,
        "apply:context:", applier, context);

    CFIndex idx;
    for (idx = 0; idx < range.length; idx++) {
        const void* item = __CFArrayGetBucketAtIndex(array, range.location + idx)->_item;
        applier(item, context);
    }
}

CFIndex CFArrayGetFirstIndexOfValue(CFArrayRef array, CFRange range, const void* value) {
    const CFArrayCallBacks* cb;
    CFIndex idx;
    CF_OBJC_FUNCDISPATCH(CFIndex, array, "_cfindexOfObject:inRange:", value, range);
    CF_VALIDATE_ARRAY_ARG(array);
    CF_VALIDATE_RANGE_ARG(range, CFArrayGetCount(array));
    cb = __CFArrayGetCallBacks(array);
    for (idx = 0; idx < range.length; idx++) {
        const void* item = __CFArrayGetBucketAtIndex(array, range.location + idx)->_item;
        if (value == item || (cb->equal && cb->equal(value, item))) {
            return idx + range.location;
        }
    }
    return kCFNotFound;
}

CFIndex CFArrayGetLastIndexOfValue(CFArrayRef array, CFRange range, const void* value) {
    const CFArrayCallBacks* cb;
    CFIndex idx;
    CF_OBJC_FUNCDISPATCH(CFIndex, array, "_cflastIndexOfObject:inRange:", value, range);
    CF_VALIDATE_ARRAY_ARG(array);
    CF_VALIDATE_RANGE_ARG(range, CFArrayGetCount(array));
    cb = __CFArrayGetCallBacks(array);
    for (idx = range.length; idx--; ) {
        const void* item = __CFArrayGetBucketAtIndex(array, range.location + idx)->_item;
        if (value == item || (cb->equal && cb->equal(value, item))) {
            return idx + range.location;
        }
    }
    return kCFNotFound;
}

void CFArrayAppendValue(CFMutableArrayRef array, const void* value) {
    CF_OBJC_VOID_FUNCDISPATCH(array, "addObject:", value);
    CF_VALIDATE_MUTABLEARRAY_ARG(array);
    _CFArrayReplaceValues(array, CFRangeMake(__CFArrayGetCount(array), 0), &value, 1);
}

void CFArraySetValueAtIndex(CFMutableArrayRef array, CFIndex idx, const void* value) {
    CF_OBJC_VOID_FUNCDISPATCH(array, "setObject:atIndex:", value, idx);
    CF_VALIDATE_MUTABLEARRAY_ARG(array);
    CF_VALIDATE_INDEX_ARG(idx, __CFArrayGetCount(array));
    if (idx == __CFArrayGetCount(array)) {
        _CFArrayReplaceValues(array, CFRangeMake(idx, 0), &value, 1);
    } else {
        const void* old_value;
        const CFArrayCallBacks* cb = __CFArrayGetCallBacks(array);
        CFAllocatorRef allocator = CFGetAllocator(array);
        __CFArrayBucket* bucket = __CFArrayGetBucketAtIndex(array, idx);
        if (cb->retain) {
            value = (void*)cb->retain(allocator, value);
        }
        old_value = bucket->_item;
        bucket->_item = value; // GC: handles deque/CFStorage cases.
        if (cb->release) {
            cb->release(allocator, old_value);
        }
        array->_mutations++;
    }
}

void CFArrayInsertValueAtIndex(CFMutableArrayRef array, CFIndex idx, const void* value) {
    CF_OBJC_VOID_FUNCDISPATCH(array, "insertObject:atIndex:", value, idx);
    CF_VALIDATE_MUTABLEARRAY_ARG(array);
    CF_VALIDATE_INDEX_ARG(idx, __CFArrayGetCount(array));
    _CFArrayReplaceValues(array, CFRangeMake(idx, 0), &value, 1);
}

void CFArrayExchangeValuesAtIndices(CFMutableArrayRef array, CFIndex idx1, CFIndex idx2) {
    const void* tmp;
    __CFArrayBucket* bucket1, * bucket2;
    CFAllocatorRef bucketsAllocator;
    CF_OBJC_VOID_FUNCDISPATCH(array, "exchange::", idx1, idx2);
    CF_VALIDATE_MUTABLEARRAY_ARG(array);
    CF_VALIDATE_INDEX_ARG(idx1, __CFArrayGetCount(array));
    CF_VALIDATE_INDEX_ARG(idx2, __CFArrayGetCount(array));
    bucket1 = __CFArrayGetBucketAtIndex(array, idx1);
    bucket2 = __CFArrayGetBucketAtIndex(array, idx2);
    tmp = bucket1->_item;
    bucketsAllocator = CFGetAllocator(array);
    // XXX these aren't needed.
    bucket1->_item = bucket2->_item;
    bucket2->_item = tmp;
    array->_mutations++;

}

void CFArrayRemoveValueAtIndex(CFMutableArrayRef array, CFIndex idx) {
    CF_OBJC_VOID_FUNCDISPATCH(array, "removeObjectAtIndex:", idx);
    CF_VALIDATE_MUTABLEARRAY_ARG(array);
    CF_VALIDATE_INDEX_ARG(idx, __CFArrayGetCount(array));
    _CFArrayReplaceValues(array, CFRangeMake(idx, 1), NULL, 0);
}

void CFArrayRemoveAllValues(CFMutableArrayRef array) {
    CF_OBJC_VOID_FUNCDISPATCH(array, "removeAllObjects");
    CF_VALIDATE_MUTABLEARRAY_ARG(array);
    __CFArrayReleaseValues(array, CFRangeMake(0, __CFArrayGetCount(array)), true);
    __CFArraySetCount(array, 0);
    array->_mutations++;
}

void CFArrayReplaceValues(CFMutableArrayRef array, CFRange range, const void** newValues, CFIndex newCount) {
    CF_OBJC_VOID_FUNCDISPATCH(array, "replaceObjectsInRange:withObjects:count:", range, (void**)newValues, newCount);
    CF_VALIDATE_MUTABLEARRAY_ARG(array);
    CF_VALIDATE_RANGE_ARG(range, CFArrayGetCount(array));
    CF_VALIDATE_NONNEGATIVE_ARG(newCount);
    return _CFArrayReplaceValues(array, range, newValues, newCount);
}

void CFArraySortValues(CFMutableArrayRef array, CFRange range, CFComparatorFunction comparator, void* context) {
    CF_OBJC_VOID_FUNCDISPATCH(array, "sortUsingFunction:context:range:", comparator, context, range);
    CF_VALIDATE_MUTABLEARRAY_ARG(array);
    CF_VALIDATE_RANGE_ARG(range, CFArrayGetCount(array));
    CF_VALIDATE_PTR_ARG(comparator);
    
    array->_mutations++;

    if (1 < range.length) {
        struct _acompareContext ctx;
        __CFArrayBucket* bucket;
        ctx.func = comparator;
        ctx.context = context;
        switch (__CFArrayGetType(array)) {
            case __kCFArrayDeque:
                bucket = __CFArrayGetBucketsPtr(array) + range.location;
                CFQSortArray(bucket, range.length, sizeof(void*), (CFComparatorFunction)__CFArrayCompareValues, &ctx);
                break;
            case __kCFArrayStorage: {
                CFStorageRef store = (CFStorageRef)array->_store;
                CFAllocatorRef allocator = CFGetAllocator(array);
                const void** values, * buffer[256];
                values = (range.length <= 256) ? (const void**)buffer : (const void**)CFAllocatorAllocate(allocator, range.length * sizeof(void*), 0); // GC OK
                CFStorageGetValues(store, range, values);
                CFQSortArray(values, range.length, sizeof(void*), (CFComparatorFunction)__CFArrayCompareValues, &ctx);
                CFStorageReplaceValues(store, range, values);
                if (values != buffer) {
                    CFAllocatorDeallocate(allocator, values);            // GC OK
                }
                break;
            }
        }
    }
}

CFIndex CFArrayBSearchValues(CFArrayRef array, CFRange range, const void* value, CFComparatorFunction comparator, void* context) {
    CF_VALIDATE_ARRAY_ARG(array);
    CF_VALIDATE_RANGE_ARG(range, CFArrayGetCount(array));
    CF_VALIDATE_PTR_ARG(comparator);
    
    bool isObjC = CF_IS_OBJC(array);
    CFIndex idx = 0;
    if (range.length <= 0) {
        return range.location;
    }
    if (isObjC || __kCFArrayStorage == __CFArrayGetType(array)) {
        const void* item;
        item = CFArrayGetValueAtIndex(array, range.location + range.length - 1);
        if (comparator(item, value, context) < 0) {
            return range.location + range.length;
        }
        item = CFArrayGetValueAtIndex(array, range.location);
        if (comparator(value, item, context) < 0) {
            return range.location;
        }
		CFIndex one = 1;
        int lg = _CFLastBitSet(range.length) - 1; // lg2(range.length)
        item = CFArrayGetValueAtIndex(array, range.location - 1 + (one << lg));
        idx = range.location + (comparator(item, value, context) < 0) ?
			range.length - (one << lg) :
			-1;
        while (lg--) {
            item = CFArrayGetValueAtIndex(array, range.location + idx + (one << lg));
            if (comparator(item, value, context) < 0) {
                idx += (one << lg);
            }
        }
        idx++;
    } else {
        struct _acompareContext ctx;
        ctx.func = comparator;
        ctx.context = context;
        idx = _CFBSearch(
			&value, sizeof(void*),
			__CFArrayGetBucketsPtr(array) + range.location, range.length,
			(CFComparatorFunction)__CFArrayCompareValues, &ctx);
    }
    return idx + range.location;
}

void CFArrayAppendArray(CFMutableArrayRef array, CFArrayRef otherArray, CFRange otherRange) {
    CFIndex idx;
    CF_VALIDATE_ARRAY_ARG(array);
    CF_VALIDATE_ARRAY_ARG(otherArray);
    CF_VALIDATE_ARG(__CFArrayGetType(array) != __kCFArrayImmutable, "array is immutable");
    CF_VALIDATE_RANGE_ARG(otherRange, CFArrayGetCount(otherArray));
    
    for (idx = otherRange.location; idx < otherRange.location + otherRange.length; idx++) {
        CFArrayAppendValue(array, CFArrayGetValueAtIndex(otherArray, idx));
    }
}

///////////////////////////////////////////////////////////////////// hidden

// This function is for Foundation's benefit; no one else should use it.
void _CFArraySetCapacity(CFMutableArrayRef array, CFIndex cap) {
    if (CF_IS_OBJC(array)) {
        return;
    }
    CF_VALIDATE_ARRAY_ARG(array);
    CF_VALIDATE_ARG(__CFArrayGetType(array) != __kCFArrayImmutable, 
		"array is immutable");
    CF_VALIDATE_ARG(__CFArrayGetCount(array) <= cap,
		"desired capacity (%d) is less than count (%d)", cap, __CFArrayGetCount(array));
    // Currently, attempting to set the capacity of an array which is the CFStorage
    // variant, or set the capacity larger than __CF_MAX_BUCKETS_PER_DEQUE, has no
    // effect.  The primary purpose of this API is to help avoid a bunch of the
    // resizes at the small capacities 4, 8, 16, etc.
    if (__CFArrayGetType(array) == __kCFArrayDeque) {
        __CFArrayDeque* deque = (__CFArrayDeque*)array->_store;
        CFIndex capacity = __CFArrayDequeRoundUpCapacity(cap);
        CFIndex size = sizeof(__CFArrayDeque) + capacity * sizeof(__CFArrayBucket);
        CFAllocatorRef allocator = CFGetAllocator(array);
        if (!deque) {
            deque = (__CFArrayDeque*)CFAllocatorAllocate(allocator, size, 0);
            if (!deque) {
                __CFArrayHandleOutOfMemory(array, size);
            }
            deque->_leftIdx = capacity / 2;
        } else {
            __CFArrayDeque* olddeque = deque;
            CFIndex oldcap = deque->_capacity;
            deque = (__CFArrayDeque*)CFAllocatorAllocate(allocator, size, 0);
            if (!deque) {
                __CFArrayHandleOutOfMemory(array, size);
            }
            memmove(deque, olddeque, sizeof(__CFArrayDeque) + oldcap * sizeof(__CFArrayBucket));
            CFAllocatorDeallocate(allocator, olddeque);
        }
        deque->_capacity = capacity;
        deque->_bias = 0;
        array->_store = deque;
    }
}

// This function does no ObjC dispatch or argument checking;
// It should only be called from places where that dispatch and check has already been done, or NSCFArray
void _CFArrayReplaceValues(CFMutableArrayRef array, CFRange range, const void** newValues, CFIndex newCount) {
    const CFArrayCallBacks* cb;
    CFAllocatorRef allocator;
    CFIndex idx, cnt, futureCnt;
    const void** newv, * buffer[256];
    cnt = __CFArrayGetCount(array);
    futureCnt = cnt - range.length + newCount;
    CF_VALIDATE_ARG(newCount <= futureCnt, "internal error 1");
    cb = __CFArrayGetCallBacks(array);
    allocator = CFGetAllocator(array);
    /* Retain new values if needed, possibly allocating a temporary buffer for them */
    if (cb->retain) {
        newv = (newCount <= 256) ? (const void**)buffer : (const void**)CFAllocatorAllocate(allocator, newCount * sizeof(void*), 0); // GC OK
        for (idx = 0; idx < newCount; idx++) {
            newv[idx] = (void*)cb->retain(allocator, (void*)newValues[idx]);
        }
    } else {
        newv = newValues;
    }
    array->_mutations++;

    /* Now, there are three regions of interest, each of which may be empty:
     *   A: the region from index 0 to one less than the range.location
     *   B: the region of the range
     *   C: the region from range.location + range.length to the end
     * Note that index 0 is not necessarily at the lowest-address edge
     * of the available storage. The values in region B need to get
     * released, and the values in regions A and C (depending) need
     * to get shifted if the number of new values is different from
     * the length of the range being replaced.
     */
    if (0 < range.length) {
        __CFArrayReleaseValues(array, range, false);
    }
    // region B elements are now "dead"
    if (__kCFArrayStorage == __CFArrayGetType(array)) {
        CFStorageRef store = (CFStorageRef)array->_store;
        // reposition regions A and C for new region B elements in gap
        if (range.length < newCount) {
            CFStorageInsertValues(store, CFRangeMake(range.location + range.length, newCount - range.length));
        } else if (newCount < range.length) {
            CFStorageDeleteValues(store, CFRangeMake(range.location + newCount, range.length - newCount));
        }
        if (futureCnt <= __CF_MAX_BUCKETS_PER_DEQUE / 2) {
            __CFArrayConvertStoreToDeque(array);
        }
    } else if (!array->_store) {
        if (__CF_MAX_BUCKETS_PER_DEQUE <= futureCnt) {
            CFStorageRef store = (CFStorageRef)CFMakeCollectable(CFStorageCreate(allocator, sizeof(const void*)));
            array->_store = store;
            CFStorageInsertValues(store, CFRangeMake(0, newCount));
            _CFBitfieldSetValue(((CFRuntimeBase*)array)->_cfinfo[CF_INFO_BITS], 1, 0, __kCFArrayStorage);
        } else if (0 <= futureCnt) {
            __CFArrayDeque* deque;
            CFIndex capacity = __CFArrayDequeRoundUpCapacity(futureCnt);
            CFIndex size = sizeof(__CFArrayDeque) + capacity * sizeof(__CFArrayBucket);
            deque = (__CFArrayDeque*)CFAllocatorAllocate(allocator, size, 0);
            deque->_leftIdx = (capacity - newCount) / 2;
            deque->_capacity = capacity;
            deque->_bias = 0;
            array->_store = deque;
        }
    } else {        // Deque
        // reposition regions A and C for new region B elements in gap
        if (__CF_MAX_BUCKETS_PER_DEQUE <= futureCnt) {
            CFStorageRef store;
            __CFArrayConvertDequeToStore(array);
            store = (CFStorageRef)array->_store;
            if (range.length < newCount) {
                CFStorageInsertValues(store, CFRangeMake(range.location + range.length, newCount - range.length));
            } else if (newCount < range.length) { // this won't happen, but is here for completeness
                CFStorageDeleteValues(store, CFRangeMake(range.location + newCount, range.length - newCount));
            }
        } else if (range.length != newCount) {
            __CFArrayRepositionDequeRegions(array, range, newCount);
        }
    }
    // copy in new region B elements
    if (0 < newCount) {
        if (__kCFArrayStorage == __CFArrayGetType(array)) {
            CFStorageRef store = (CFStorageRef)array->_store;
            CFStorageReplaceValues(store, CFRangeMake(range.location, newCount), newv);
        } else { // Deque
            __CFArrayDeque* deque = (__CFArrayDeque*)array->_store;
            __CFArrayBucket* raw_buckets = (__CFArrayBucket*)((uint8_t*)deque + sizeof(__CFArrayDeque));
            if (newCount == 1) {
                *((const void**)raw_buckets + deque->_leftIdx + range.location) = newv[0];
            } else {
                memmove(raw_buckets + deque->_leftIdx + range.location, newv, newCount * sizeof(__CFArrayBucket));
            }
        }
    }
    __CFArraySetCount(array, futureCnt);
    if (newv != buffer && newv != newValues) {
        CFAllocatorDeallocate(allocator, newv);
    }
}

// This is for use by NSCFArray; it avoids ObjC dispatch, and checks for out of bounds
const void* _CFArrayCheckAndGetValueAtIndex(CFArrayRef array, CFIndex idx) {
    if (0 <= idx && idx < __CFArrayGetCount(array)) {
        return __CFArrayGetBucketAtIndex(array, idx)->_item;
    }
    return (void*)(-1);
}

CFIndex _CFArrayFastEnumeration(CFArrayRef array, void* pstate, void* stackbuffer, CFIndex count) {
    _CFObjcFastEnumerationState* state = (_CFObjcFastEnumerationState*)pstate;
    if (array->_count == 0) {
        return 0;
    }
    enum {ATSTART = 0, ATEND = 1};
    switch (__CFArrayGetType(array)) {
        case __kCFArrayImmutable:
            if (state->state == ATSTART) { /* first time */
                static const unsigned long const_mu = 1;
                state->state = ATEND;
                state->mutationsPtr = (unsigned long*)&const_mu;
                state->itemsPtr = (unsigned long*)__CFArrayGetBucketsPtr(array);
                return array->_count;
            }
            return 0;
        case __kCFArrayDeque:
            if (state->state == ATSTART) { /* first time */
                state->state = ATEND;
                state->mutationsPtr = (unsigned long*)&array->_mutations;
                state->itemsPtr = (unsigned long*)__CFArrayGetBucketsPtr(array);
                return array->_count;
            }
            return 0;
        case __kCFArrayStorage:
            state->mutationsPtr = (unsigned long*)&array->_mutations;
            return _CFStorageFastEnumeration((CFStorageRef)array->_store, state, stackbuffer, count);
    }
    return 0;
}
