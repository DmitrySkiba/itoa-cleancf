/*
 * Copyright (c) 2011 Dmitry Skiba
 * Copyright (c) 2008-2009 Brent Fulgham <bfulgham@gmail.org>.  All rights reserved.
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

// SADLY, but we need to throw this away and use Bag/Set/Dictionary
//  from latest Apple's CF

//TODO expose callbacks with 'context' argument, Set/GetContext methods.

//TODO replace (caveat: extra 'context' argument)
#define INVOKE_CALLBACK1(P, A)                  (P)(A)
#define INVOKE_CALLBACK2(P, A, B)               (P)(A, B)
#define INVOKE_CALLBACK3(P, A, B, C)            (P)(A, B, C)
#define INVOKE_CALLBACK4(P, A, B, C, D)         (P)(A, B, C, D)
#define INVOKE_CALLBACK5(P, A, B, C, D, E)      (P)(A, B, C, D, E)

//TODO flatten types
typedef uintptr_t any_t;
typedef void* any_pointer_t;
typedef const void* const_any_pointer_t;

//TODO remove KVO
#if !defined(CF_OBJC_KVO_WILLCHANGE)
#define CF_OBJC_KVO_WILLCHANGE(obj, key)
#define CF_OBJC_KVO_DIDCHANGE(obj, key)
#endif

//TODO Replace all CFHash occurencies with THashName() macros (including __kCF*, CFMutableHashRef, etc)
//TODO CFHashValueCallBacks and other Value types should be used only with CFDictionary

#if CFDictionary

#define THashTag Dictionary

const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks = {0, _CFTypeCollectionRetain, _CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
const CFDictionaryKeyCallBacks kCFCopyStringDictionaryKeyCallBacks = {0, __CFStringCollectionCopy, _CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks = {0, _CFTypeCollectionRetain, _CFTypeCollectionRelease, CFCopyDescription, CFEqual};
static const CFDictionaryKeyCallBacks __kCFNullDictionaryKeyCallBacks = {0, NULL, NULL, NULL, NULL, NULL};
static const CFDictionaryValueCallBacks __kCFNullDictionaryValueCallBacks = {0, NULL, NULL, NULL, NULL};

#define CFHashKeyCallBacks CFDictionaryKeyCallBacks
#define CFHashValueCallBacks CFDictionaryValueCallBacks
#define kCFTypeHashKeyCallBacks kCFTypeDictionaryKeyCallBacks
#define kCFTypeHashValueCallBacks kCFTypeDictionaryValueCallBacks
#define __kCFNullHashKeyCallBacks __kCFNullDictionaryKeyCallBacks
#define __kCFNullHashValueCallBacks __kCFNullDictionaryValueCallBacks

#define CFHashRef CFDictionaryRef
#define CFMutableHashRef CFMutableDictionaryRef
#define __kCFHashTypeID __kCFDictionaryTypeID
#endif

#if CFBag

#define THashTag Bag

const CFBagCallBacks kCFTypeBagCallBacks = {0, _CFTypeCollectionRetain, _CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
const CFBagCallBacks kCFCopyStringBagCallBacks = {0, __CFStringCollectionCopy, _CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
static const CFBagCallBacks __kCFNullBagCallBacks = {0, NULL, NULL, NULL, NULL, NULL};

#define CFHashKeyCallBacks CFBagCallBacks
#define CFHashValueCallBacks CFBagCallBacks
#define kCFTypeHashKeyCallBacks kCFTypeBagCallBacks
#define kCFTypeHashValueCallBacks kCFTypeBagCallBacks
#define __kCFNullHashKeyCallBacks __kCFNullBagCallBacks
#define __kCFNullHashValueCallBacks __kCFNullBagCallBacks

#define CFHashRef CFBagRef
#define CFMutableHashRef CFMutableBagRef
#define __kCFHashTypeID __kCFBagTypeID
#endif

#if CFSet

#define THashTag Set

const CFSetCallBacks kCFTypeSetCallBacks = {0, _CFTypeCollectionRetain, _CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
const CFSetCallBacks kCFCopyStringSetCallBacks = {0, __CFStringCollectionCopy, _CFTypeCollectionRelease, CFCopyDescription, CFEqual, CFHash};
static const CFSetCallBacks __kCFNullSetCallBacks = {0, NULL, NULL, NULL, NULL, NULL};

#define CFHashKeyCallBacks CFSetCallBacks
#define CFHashValueCallBacks CFSetCallBacks
#define kCFTypeHashKeyCallBacks kCFTypeSetCallBacks
#define kCFTypeHashValueCallBacks kCFTypeSetCallBacks
#define __kCFNullHashKeyCallBacks __kCFNullSetCallBacks
#define __kCFNullHashValueCallBacks __kCFNullSetCallBacks

#define CFHashRef CFSetRef
#define CFMutableHashRef CFMutableSetRef
#define __kCFHashTypeID __kCFSetTypeID
#endif

/////////////////////////////////////////////////////////////////////

#define __THashStringize(Name) #Name
#define __THashMakeName(A,B) __THashMakeNameEval(A,B)
#define __THashMakeNameEval(A,B) A##B

#define __THashName(Name) __THashMakeName(__CF,__THashMakeName(THashTag,Name))
#define _THashName(Name) __THashMakeName(_CF,__THashMakeName(THashTag,Name))
#define THashName(Name) __THashMakeName(CF,__THashMakeName(THashTag,Name))

#define THashString "CF"__THashStringize(THashTag)
#define __THash __THashMakeName(__CF,THashTag)

/////////////////////////////////////////////////////////////////////

#define GETNEWKEY(newKey, oldKey) \
        any_t (*kretain)(CFAllocatorRef, any_t, any_pointer_t) = \
          !hasBeenFinalized(hc) \
            ? (any_t (*)(CFAllocatorRef,any_t,any_pointer_t))__THashName(GetKeyCallBacks)(hc)->retain \
            : (any_t (*)(CFAllocatorRef,any_t,any_pointer_t))0; \
        any_t newKey = kretain ? (any_t)INVOKE_CALLBACK3(kretain, allocator, (any_t)key, hc->_context) : (any_t)oldKey

#define RELEASEKEY(oldKey) \
        void (*krelease)(CFAllocatorRef, any_t, any_pointer_t) = \
          !hasBeenFinalized(hc) \
            ? (void (*)(CFAllocatorRef,any_t,any_pointer_t))__THashName(GetKeyCallBacks)(hc)->release \
            : (void (*)(CFAllocatorRef,any_t,any_pointer_t))0; \
        if (krelease) INVOKE_CALLBACK3(krelease, allocator, oldKey, hc->_context)
        
#if CFDictionary
#define GETNEWVALUE(newValue) \
        any_t (*vretain)(CFAllocatorRef, any_t, any_pointer_t) = \
          !hasBeenFinalized(hc) \
            ? (any_t (*)(CFAllocatorRef,any_t,any_pointer_t))__THashName(GetValueCallBacks)(hc)->retain \
            : (any_t (*)(CFAllocatorRef,any_t,any_pointer_t))0; \
        any_t newValue = vretain ? (any_t)INVOKE_CALLBACK3(vretain, allocator, (any_t)value, hc->_context) : (any_t)value

#define RELEASEVALUE(oldValue) \
    void (*vrelease)(CFAllocatorRef, any_t, any_pointer_t) = \
      !hasBeenFinalized(hc) \
        ? (void (*)(CFAllocatorRef,any_t,any_pointer_t))__THashName(GetValueCallBacks)(hc)->release \
        : (void (*)(CFAllocatorRef,any_t,any_pointer_t))0; \
    if (vrelease) INVOKE_CALLBACK3(vrelease, allocator, oldValue, hc->_context)

#endif

static void __THashName(HandleOutOfMemory)(CFTypeRef obj, CFIndex numBytes) {
    CFReportRuntimeError(
        kCFRuntimeErrorOutOfMemory, 
        CFSTR("Attempt to allocate %ld bytes for NS/"THashString" failed"), numBytes);
}


// Max load is 3/4 number of buckets
CF_INLINE CFIndex __CFHashRoundUpCapacity(CFIndex capacity) {
    return 3 * ((CFIndex)1 << (_CFLastBitSet((capacity - 1) / 3)));
}

// Returns next power of two higher than the capacity
// threshold for the given input capacity.
CF_INLINE CFIndex __CFHashNumBucketsForCapacity(CFIndex capacity) {
    return 4 * ((CFIndex)1 << (_CFLastBitSet((capacity - 1) / 3)));
}

enum { /* Bits 1-0 */
    __kCFHashImmutable = 0, /* unchangable and fixed capacity */
    __kCFHashMutable = 1, /* changeable and variable capacity */
};

enum { /* Bits 5-4 (value), 3-2 (key) */
    __kCFHashHasNullCallBacks = 0,
    __kCFHashHasCFTypeCallBacks = 1,
    __kCFHashHasCustomCallBacks = 3 /* callbacks are at end of header */
};

struct __THash {
    CFRuntimeBase _base;
    CFIndex _count;             /* number of values */
    CFIndex _bucketsNum;        /* number of buckets */
    CFIndex _bucketsUsed;       /* number of used buckets */
    CFIndex _bucketsCap;        /* maximum number of used buckets */
    CFIndex _mutations;
    CFIndex _deletes;
    any_pointer_t _context;     /* private */
    CFOptionFlags _xflags;
    any_t _marker;
    any_t *_keys;     /* can be NULL if not allocated yet */
    any_t *_values;   /* can be NULL if not allocated yet */
};

/* Bits 1-0 of the _xflags are used for mutability variety */
/* Bits 3-2 of the _xflags are used for key callback indicator bits */
/* Bits 5-4 of the _xflags are used for value callback indicator bits */
/* Bit 6 of the _xflags is special KVO actions bit */
/* Bits 7,8,9 are GC use */

CF_INLINE bool hasBeenFinalized(CFTypeRef collection) {
    return _CFBitfieldGetValue(((const struct __THash *)collection)->_xflags, 7, 7) != 0;
}

CF_INLINE void markFinalized(CFTypeRef collection) {
    _CFBitfieldSetValue(((struct __THash *)collection)->_xflags, 7, 7, 1);
}


CF_INLINE CFIndex __CFHashGetType(CFHashRef hc) {
    return _CFBitfieldGetValue(hc->_xflags, 1, 0);
}

CF_INLINE CFIndex __THashName(GetSizeOfType)(CFIndex t) {
    CFIndex size = sizeof(struct __THash);
    if (_CFBitfieldGetValue(t, 3, 2) == __kCFHashHasCustomCallBacks) {
        size += sizeof(CFHashKeyCallBacks);
    }
    if (_CFBitfieldGetValue(t, 5, 4) == __kCFHashHasCustomCallBacks) {
        size += sizeof(CFHashValueCallBacks);
    }
    return size;
}

CF_INLINE const CFHashKeyCallBacks *__THashName(GetKeyCallBacks)(CFHashRef hc) {
    CFHashKeyCallBacks *result = NULL;
    switch (_CFBitfieldGetValue(hc->_xflags, 3, 2)) {
    case __kCFHashHasNullCallBacks:
        return &__kCFNullHashKeyCallBacks;
    case __kCFHashHasCFTypeCallBacks:
        return &kCFTypeHashKeyCallBacks;
    case __kCFHashHasCustomCallBacks:
        break;
    }
    result = (CFHashKeyCallBacks *)((uint8_t *)hc + sizeof(struct __THash));
    return result;
}

CF_INLINE Boolean __THashName(KeyCallBacksMatchNull)(const CFHashKeyCallBacks *c) {
    return (NULL == c ||
        (c->retain == __kCFNullHashKeyCallBacks.retain &&
         c->release == __kCFNullHashKeyCallBacks.release &&
         c->copyDescription == __kCFNullHashKeyCallBacks.copyDescription &&
         c->equal == __kCFNullHashKeyCallBacks.equal &&
         c->hash == __kCFNullHashKeyCallBacks.hash));
}

CF_INLINE Boolean __THashName(KeyCallBacksMatchCFType)(const CFHashKeyCallBacks *c) {
    return (&kCFTypeHashKeyCallBacks == c ||
        (c->retain == kCFTypeHashKeyCallBacks.retain &&
         c->release == kCFTypeHashKeyCallBacks.release &&
         c->copyDescription == kCFTypeHashKeyCallBacks.copyDescription &&
         c->equal == kCFTypeHashKeyCallBacks.equal &&
         c->hash == kCFTypeHashKeyCallBacks.hash));
}

CF_INLINE const CFHashValueCallBacks *__THashName(GetValueCallBacks)(CFHashRef hc) {
    CFHashValueCallBacks *result = NULL;
    switch (_CFBitfieldGetValue(hc->_xflags, 5, 4)) {
    case __kCFHashHasNullCallBacks:
        return &__kCFNullHashValueCallBacks;
    case __kCFHashHasCFTypeCallBacks:
        return &kCFTypeHashValueCallBacks;
    case __kCFHashHasCustomCallBacks:
        break;
    }
    if (_CFBitfieldGetValue(hc->_xflags, 3, 2) == __kCFHashHasCustomCallBacks) {
        result = (CFHashValueCallBacks *)((uint8_t *)hc + sizeof(struct __THash) + sizeof(CFHashKeyCallBacks));
    } else {
        result = (CFHashValueCallBacks *)((uint8_t *)hc + sizeof(struct __THash));
    }
    return result;
}

CF_INLINE Boolean __THashName(ValueCallBacksMatchNull)(const CFHashValueCallBacks *c) {
    return (NULL == c ||
        (c->retain == __kCFNullHashValueCallBacks.retain &&
         c->release == __kCFNullHashValueCallBacks.release &&
         c->copyDescription == __kCFNullHashValueCallBacks.copyDescription &&
         c->equal == __kCFNullHashValueCallBacks.equal));
}

CF_INLINE Boolean __THashName(ValueCallBacksMatchCFType)(const CFHashValueCallBacks *c) {
    return (&kCFTypeHashValueCallBacks == c ||
        (c->retain == kCFTypeHashValueCallBacks.retain &&
         c->release == kCFTypeHashValueCallBacks.release &&
         c->copyDescription == kCFTypeHashValueCallBacks.copyDescription &&
         c->equal == kCFTypeHashValueCallBacks.equal));
}

CFIndex _THashName(GetKVOBit)(CFHashRef hc) {
    return _CFBitfieldGetValue(hc->_xflags, 6, 6);
}

void _THashName(SetKVOBit)(CFHashRef hc, CFIndex bit) {
    _CFBitfieldSetValue(((CFMutableHashRef)hc)->_xflags, 6, 6, ((uintptr_t)bit & 0x1));
}

CF_INLINE Boolean __THashName(ShouldShrink)(CFHashRef hc) {
    return (__kCFHashMutable == __CFHashGetType(hc)) &&
           (hc->_bucketsNum < 4 * hc->_deletes || (256 <= hc->_bucketsCap && hc-> _bucketsUsed < 3 * hc->_bucketsCap / 16));
}

CF_INLINE CFIndex __CFHashGetOccurrenceCount(CFHashRef hc, CFIndex idx) {
#if CFBag
    return (CFIndex)hc->_values[idx];
#endif
    return 1;
}

CF_INLINE Boolean __CFHashKeyIsValue(CFHashRef hc, any_t key) {
    return (hc->_marker != key && ~hc->_marker != key) ? true : false;
}

CF_INLINE Boolean __CFHashKeyIsMagic(CFHashRef hc, any_t key) {
    return (hc->_marker == key || ~hc->_marker == key) ? true : false;
}



CF_INLINE uintptr_t __THashName(ScrambleHash)(uintptr_t k) {
    uintptr_t a = 0x4B616E65UL;
    uintptr_t b = 0x4B616E65UL;
    uintptr_t c = 1;
    a += k;
    a -= b; a -= c; a ^= (c >> 13);
    b -= c; b -= a; b ^= (a << 8);
    c -= a; c -= b; c ^= (b >> 13);
    a -= b; a -= c; a ^= (c >> 12);
    b -= c; b -= a; b ^= (a << 16);
    c -= a; c -= b; c ^= (b >> 5);
    a -= b; a -= c; a ^= (c >> 3);
    b -= c; b -= a; b ^= (a << 10);
    c -= a; c -= b; c ^= (b >> 15);
    return c;
}

static CFIndex __THashName(FindBuckets1a)(CFHashRef hc, any_t key) {
    CFHashCode keyHash = (CFHashCode)key;
    keyHash = (CFHashCode)__THashName(ScrambleHash)(keyHash);
    any_t *keys = hc->_keys;
    any_t marker = hc->_marker;
    CFIndex probe = keyHash & (hc->_bucketsNum - 1);
    CFIndex probeskip = 1;        // See RemoveValue() for notes before changing this value
    CFIndex start = probe;
    for (;;) {
        any_t currKey = keys[probe];
        if (marker == currKey) {                /* empty */
            return kCFNotFound;
        } else if (~marker == currKey) {        /* deleted */
            /* do nothing */
        } else if (currKey == key) {
            return probe;
        }
        probe = probe + probeskip;
        // This alternative to probe % buckets assumes that
        // probeskip is always positive and less than the
        // number of buckets.
        if (hc->_bucketsNum <= probe) {
            probe -= hc->_bucketsNum;
        }
        if (start == probe) {
            return kCFNotFound;
        }
    }
}

static CFIndex __THashName(FindBuckets1b)(CFHashRef hc, any_t key) {
    const CFHashKeyCallBacks *cb = __THashName(GetKeyCallBacks)(hc);
    CFHashCode keyHash = cb->hash ? (CFHashCode)INVOKE_CALLBACK2(((CFHashCode (*)(any_t, any_pointer_t))cb->hash), key, hc->_context) : (CFHashCode)key;
    keyHash = (CFHashCode)__THashName(ScrambleHash)(keyHash);
    any_t *keys = hc->_keys;
    any_t marker = hc->_marker;
    CFIndex probe = keyHash & (hc->_bucketsNum - 1);
    CFIndex probeskip = 1;        // See RemoveValue() for notes before changing this value
    CFIndex start = probe;
    for (;;) {
        any_t currKey = keys[probe];
        if (marker == currKey) {                /* empty */
            return kCFNotFound;
        } else if (~marker == currKey) {        /* deleted */
            /* do nothing */
        } else if (currKey == key || (cb->equal && INVOKE_CALLBACK3((Boolean (*)(any_t, any_t, any_pointer_t))cb->equal, currKey, key, hc->_context))) {
            return probe;
        }
        probe = probe + probeskip;
        // This alternative to probe % buckets assumes that
        // probeskip is always positive and less than the
        // number of buckets.
        if (hc->_bucketsNum <= probe) {
            probe -= hc->_bucketsNum;
        }
        if (start == probe) {
            return kCFNotFound;
        }
    }
}

CF_INLINE CFIndex __THashName(FindBuckets1)(CFHashRef hc, any_t key) {
    if (__kCFHashHasNullCallBacks == _CFBitfieldGetValue(hc->_xflags, 3, 2)) {
        return __THashName(FindBuckets1a)(hc, key);
    }
    return __THashName(FindBuckets1b)(hc, key);
}

static void __THashName(FindBuckets2)(CFHashRef hc, any_t key, CFIndex *match, CFIndex *nomatch) {
    const CFHashKeyCallBacks *cb = __THashName(GetKeyCallBacks)(hc);
    CFHashCode keyHash = cb->hash ? (CFHashCode)INVOKE_CALLBACK2(((CFHashCode (*)(any_t, any_pointer_t))cb->hash), key, hc->_context) : (CFHashCode)key;
    keyHash = (CFHashCode)__THashName(ScrambleHash)(keyHash);
    any_t *keys = hc->_keys;
    any_t marker = hc->_marker;
    CFIndex probe = keyHash & (hc->_bucketsNum - 1);
    CFIndex probeskip = 1;        // See RemoveValue() for notes before changing this value
    CFIndex start = probe;
    *match = kCFNotFound;
    *nomatch = kCFNotFound;
    for (;;) {
        any_t currKey = keys[probe];
        if (marker == currKey) {                /* empty */
            if (nomatch) *nomatch = probe;
            return;
        } else if (~marker == currKey) {        /* deleted */
            if (nomatch) {
                *nomatch = probe;
                nomatch = NULL;
            }
        } else if (currKey == key || (cb->equal && INVOKE_CALLBACK3((Boolean (*)(any_t, any_t, any_pointer_t))cb->equal, currKey, key, hc->_context))) {
            *match = probe;
            return;
        }
        probe = probe + probeskip;
        // This alternative to probe % buckets assumes that
        // probeskip is always positive and less than the
        // number of buckets.
        if (hc->_bucketsNum <= probe) {
            probe -= hc->_bucketsNum;
        }
        if (start == probe) {
            return;
        }
    }
}

static void __THashName(FindNewMarker)(CFHashRef hc) {
    any_t *keys = hc->_keys;
    any_t newMarker;
    CFIndex idx, nbuckets;
    Boolean hit;

    nbuckets = hc->_bucketsNum;
    newMarker = hc->_marker;
    do {
        newMarker--;
        hit = false;
        for (idx = 0; idx < nbuckets; idx++) {
            if (newMarker == keys[idx] || ~newMarker == keys[idx]) {
                hit = true;
                break;
            }
        }
    } while (hit);
    for (idx = 0; idx < nbuckets; idx++) {
        if (hc->_marker == keys[idx]) {
            keys[idx] = newMarker;
        } else if (~hc->_marker == keys[idx]) {
            keys[idx] = ~newMarker;
        }
    }
    ((struct __THash *)hc)->_marker = newMarker;
}

static Boolean __THashName(Equal)(CFTypeRef cf1, CFTypeRef cf2) {
    CFHashRef hc1 = (CFHashRef)cf1;
    CFHashRef hc2 = (CFHashRef)cf2;
    const CFHashKeyCallBacks *cb1, *cb2;
    const CFHashValueCallBacks *vcb1, *vcb2;
    any_t *keys;
    CFIndex idx, nbuckets;
    if (hc1 == hc2) return true;
    if (hc1->_count != hc2->_count) return false;
    cb1 = __THashName(GetKeyCallBacks)(hc1);
    cb2 = __THashName(GetKeyCallBacks)(hc2);
    if (cb1->equal != cb2->equal) return false;
    vcb1 = __THashName(GetValueCallBacks)(hc1);
    vcb2 = __THashName(GetValueCallBacks)(hc2);
    if (vcb1->equal != vcb2->equal) return false;
    if (0 == hc1->_bucketsUsed) return true; /* after function comparison! */
    keys = hc1->_keys;
    nbuckets = hc1->_bucketsNum;
    for (idx = 0; idx < nbuckets; idx++) {
        if (hc1->_marker != keys[idx] && ~hc1->_marker != keys[idx]) {
#if CFDictionary
            const_any_pointer_t value;
            if (!THashName(GetValueIfPresent)(hc2, (any_pointer_t)keys[idx], &value)) return false;
        if (hc1->_values[idx] != (any_t)value) {
        if (NULL == vcb1->equal) return false;
            if (!INVOKE_CALLBACK3((Boolean (*)(any_t, any_t, any_pointer_t))vcb1->equal, hc1->_values[idx], (any_t)value, hc1->_context)) return false;
            }
#endif
#if  CFSet
            const_any_pointer_t value;
            if (!THashName(GetValueIfPresent)(hc2, (any_pointer_t)keys[idx], &value)) return false;
#endif
#if CFBag
            if (hc1->_values[idx] != THashName(GetCountOfValue)(hc2, (any_pointer_t)keys[idx])) return false;
#endif
        }
    }
    return true;
}

static CFHashCode __THashName(Hash)(CFTypeRef cf) {
    CFHashRef hc = (CFHashRef)cf;
    return hc->_count;
}

static CFStringRef __THashName(CopyDescription)(CFTypeRef cf) {
    CFHashRef hc = (CFHashRef)cf;
    CFAllocatorRef allocator;
    const CFHashKeyCallBacks *cb;
    const CFHashValueCallBacks *vcb;
    any_t *keys;
    CFIndex idx, nbuckets;
    CFMutableStringRef result;
    cb = __THashName(GetKeyCallBacks)(hc);
    vcb = __THashName(GetValueCallBacks)(hc);
    keys = hc->_keys;
    nbuckets = hc->_bucketsNum;
    allocator = CFGetAllocator(hc);
    result = CFStringCreateMutable(allocator, 0);
    const char *type = "?";
    switch (__CFHashGetType(hc)) {
    case __kCFHashImmutable: type = "immutable"; break;
    case __kCFHashMutable: type = "mutable"; break;
    }
    CFStringAppendFormat(result, NULL, CFSTR("<"THashString" %p [%p]>{type = %s, count = %u, capacity = %u, pairs = (\n"), cf, allocator, type, hc->_count, hc->_bucketsCap);
    for (idx = 0; idx < nbuckets; idx++) {
        if (__CFHashKeyIsValue(hc, keys[idx])) {
            CFStringRef kDesc = NULL, vDesc = NULL;
            if (NULL != cb->copyDescription) {
                kDesc = (CFStringRef)INVOKE_CALLBACK2(((CFStringRef (*)(any_t, any_pointer_t))cb->copyDescription), keys[idx], hc->_context);
            }
            if (NULL != vcb->copyDescription) {
                vDesc = (CFStringRef)INVOKE_CALLBACK2(((CFStringRef (*)(any_t, any_pointer_t))vcb->copyDescription), hc->_values[idx], hc->_context);
            }
#if CFDictionary
            if (NULL != kDesc && NULL != vDesc) {
                CFStringAppendFormat(result, NULL, CFSTR("\t%u : %@ = %@\n"), idx, kDesc, vDesc);
                CFRelease(kDesc);
                CFRelease(vDesc);
            } else if (NULL != kDesc) {
                CFStringAppendFormat(result, NULL, CFSTR("\t%u : %@ = <%p>\n"), idx, kDesc, hc->_values[idx]);
                CFRelease(kDesc);
            } else if (NULL != vDesc) {
                CFStringAppendFormat(result, NULL, CFSTR("\t%u : <%p> = %@\n"), idx, keys[idx], vDesc);
                CFRelease(vDesc);
            } else {
                CFStringAppendFormat(result, NULL, CFSTR("\t%u : <%p> = <%p>\n"), idx, keys[idx], hc->_values[idx]);
            }
#endif
#if CFSet
            if (NULL != kDesc) {
                CFStringAppendFormat(result, NULL, CFSTR("\t%u : %@\n"), idx, kDesc);
                CFRelease(kDesc);
            } else {
                CFStringAppendFormat(result, NULL, CFSTR("\t%u : <%p>\n"), idx, keys[idx]);
            }
#endif
#if CFBag
            if (NULL != kDesc) {
                CFStringAppendFormat(result, NULL, CFSTR("\t%u : %@ (%ld)\n"), idx, kDesc, hc->_values[idx]);
                CFRelease(kDesc);
            } else {
                CFStringAppendFormat(result, NULL, CFSTR("\t%u : <%p> (%ld)\n"), idx, keys[idx], hc->_values[idx]);
            }
#endif
        }
    }
    CFStringAppend(result, CFSTR(")}"));
    return result;
}

static void __THashName(Deallocate)(CFTypeRef cf) {
    CFMutableHashRef hc = (CFMutableHashRef)cf;
    CFAllocatorRef allocator = CFGetAllocator(hc);
    const CFHashKeyCallBacks *cb = __THashName(GetKeyCallBacks)(hc);
    const CFHashValueCallBacks *vcb = __THashName(GetValueCallBacks)(hc);

    // mark now in case any callout somehow tries to add an entry back in
    markFinalized(cf);
    if (vcb->release || cb->release) {
        any_t *keys = hc->_keys;
        CFIndex idx, nbuckets = hc->_bucketsNum;
        for (idx = 0; idx < nbuckets; idx++) {
            any_t oldkey = keys[idx];
            if (hc->_marker != oldkey && ~hc->_marker != oldkey) {
                if (vcb->release) {
                    INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, any_t, any_pointer_t))vcb->release), allocator, hc->_values[idx], hc->_context);
                }
                if (cb->release) {
                    INVOKE_CALLBACK3(((void (*)(CFAllocatorRef, any_t, any_pointer_t))cb->release), allocator, oldkey, hc->_context);
                }
            }
        }
    }

    CFAllocatorDeallocate(allocator, hc->_keys);
#if CFDictionary || CFBag
    CFAllocatorDeallocate(allocator, hc->_values);
#endif
    hc->_keys = NULL;
    hc->_values = NULL;
    hc->_count = 0;  // GC: also zero count, so the hc will appear empty.
    hc->_bucketsUsed = 0;
    hc->_bucketsNum = 0;
}

static CFTypeID __kCFHashTypeID = _kCFRuntimeNotATypeID;

static const CFRuntimeClass __THashName(Class) = {
    0,
    THashString,
    NULL,        // init
    NULL,        // copy
    __THashName(Deallocate),
    __THashName(Equal),
    __THashName(Hash),
    NULL,        //
    __THashName(CopyDescription)
};

CF_INTERNAL void __THashName(Initialize)(void) {
    __kCFHashTypeID = _CFRuntimeRegisterClass(&__THashName(Class));
}

CFTypeID THashName(GetTypeID)(void) {
    return __kCFHashTypeID;
}

static CFMutableHashRef __THashName(Init)(CFAllocatorRef allocator, CFOptionFlags flags, CFIndex capacity, const CFHashKeyCallBacks *keyCallBacks
#if CFDictionary
, const CFHashValueCallBacks *valueCallBacks
#endif
) {
    struct __THash *hc;
    CFIndex size;
    _CFBitfieldSetValue(flags, 31, 2, 0);
    CFOptionFlags xflags = 0;
    if (__THashName(KeyCallBacksMatchNull)(keyCallBacks)) {
        _CFBitfieldSetValue(flags, 3, 2, __kCFHashHasNullCallBacks);
    } else if (__THashName(KeyCallBacksMatchCFType)(keyCallBacks)) {
        _CFBitfieldSetValue(flags, 3, 2, __kCFHashHasCFTypeCallBacks);
    } else {
        _CFBitfieldSetValue(flags, 3, 2, __kCFHashHasCustomCallBacks);
    }
#if CFDictionary
    if (__THashName(ValueCallBacksMatchNull)(valueCallBacks)) {
        _CFBitfieldSetValue(flags, 5, 4, __kCFHashHasNullCallBacks);
    } else if (__THashName(ValueCallBacksMatchCFType)(valueCallBacks)) {
        _CFBitfieldSetValue(flags, 5, 4, __kCFHashHasCFTypeCallBacks);
    } else {
        _CFBitfieldSetValue(flags, 5, 4, __kCFHashHasCustomCallBacks);
    }
#endif
    size = __THashName(GetSizeOfType)(flags) - sizeof(CFRuntimeBase);
    hc = (struct __THash *)_CFRuntimeCreateInstance(allocator, __kCFHashTypeID, size, NULL);
    if (NULL == hc) {
        return NULL;
    }
    hc->_count = 0;
    hc->_bucketsUsed = 0;
    hc->_marker = (any_t)0xa1b1c1d3;
    hc->_context = NULL;
    hc->_deletes = 0;
    hc->_mutations = 1;
    hc->_xflags = xflags | flags;
    hc->_bucketsCap = __CFHashRoundUpCapacity(1);
    hc->_bucketsNum = 0;
    hc->_keys = NULL;
    hc->_values = NULL;
    if (__kCFHashHasCustomCallBacks == _CFBitfieldGetValue(flags, 3, 2)) {
        CFHashKeyCallBacks *cb = (CFHashKeyCallBacks *)__THashName(GetKeyCallBacks)((CFHashRef)hc);
        *cb = *keyCallBacks;
    }
#if CFDictionary
    if (__kCFHashHasCustomCallBacks == _CFBitfieldGetValue(flags, 5, 4)) {
        CFHashValueCallBacks *vcb = (CFHashValueCallBacks *)__THashName(GetValueCallBacks)((CFHashRef)hc);
        *vcb = *valueCallBacks;
    }
#endif
    return hc;
}

#if CFDictionary
CFHashRef THashName(Create)(CFAllocatorRef allocator, const_any_pointer_t *keys, const_any_pointer_t *values, CFIndex numValues, const CFHashKeyCallBacks *keyCallBacks, const CFHashValueCallBacks *valueCallBacks) {
#endif
#if CFSet || CFBag
CFHashRef THashName(Create)(CFAllocatorRef allocator, const_any_pointer_t *keys, CFIndex numValues, const CFHashKeyCallBacks *keyCallBacks) {
#endif
    CF_VALIDATE_NONNEGATIVE_ARG(numValues);
#if CFDictionary
    CFMutableHashRef hc = __THashName(Init)(allocator, __kCFHashImmutable, numValues, keyCallBacks, valueCallBacks);
#endif
#if CFSet || CFBag
    CFMutableHashRef hc = __THashName(Init)(allocator, __kCFHashImmutable, numValues, keyCallBacks);
#endif
    _CFBitfieldSetValue(hc->_xflags, 1, 0, __kCFHashMutable);
    for (CFIndex idx = 0; idx < numValues; idx++) {
#if CFDictionary
        THashName(AddValue)(hc, keys[idx], values[idx]);
#endif
#if CFSet || CFBag
        THashName(AddValue)(hc, keys[idx]);
#endif
    }
    _CFBitfieldSetValue(hc->_xflags, 1, 0, __kCFHashImmutable);
    return (CFHashRef)hc;
}

#if CFDictionary
CFMutableHashRef THashName(CreateMutable)(CFAllocatorRef allocator, CFIndex capacity, const CFHashKeyCallBacks *keyCallBacks, const CFHashValueCallBacks *valueCallBacks) {
#endif
#if CFSet || CFBag
CFMutableHashRef THashName(CreateMutable)(CFAllocatorRef allocator, CFIndex capacity, const CFHashKeyCallBacks *keyCallBacks) {
#endif
    CF_VALIDATE_NONNEGATIVE_ARG(capacity);
#if CFDictionary
    CFMutableHashRef hc = __THashName(Init)(allocator, __kCFHashMutable, capacity, keyCallBacks, valueCallBacks);
#endif
#if CFSet || CFBag
    CFMutableHashRef hc = __THashName(Init)(allocator, __kCFHashMutable, capacity, keyCallBacks);
#endif
    return hc;
}

#if CFDictionary || CFSet
// does not have Add semantics for Bag; it has Set semantics ... is that best?
static void __THashName(Grow)(CFMutableHashRef hc, CFIndex numNewValues);

// This creates a hc which is for CFTypes or NSObjects, with a CFRetain style ownership transfer;
// the hc does not take a retain (since it claims 1), and the caller does not need to release the inserted objects (since we do it).
// The incoming objects must also be collectable if allocated out of a collectable allocator - and are neither released nor retained.
#if CFDictionary
CFHashRef _THashName(Create_ex)(CFAllocatorRef allocator, Boolean isMutable, const_any_pointer_t *keys, const_any_pointer_t *values, CFIndex numValues) {
#endif
#if CFSet || CFBag
CFHashRef _THashName(Create_ex)(CFAllocatorRef allocator, Boolean isMutable, const_any_pointer_t *keys, CFIndex numValues) {
#endif
    CF_VALIDATE_NONNEGATIVE_ARG(numValues);
#if CFDictionary
    CFMutableHashRef hc = __THashName(Init)(allocator, __kCFHashMutable, numValues, &kCFTypeHashKeyCallBacks, &kCFTypeHashValueCallBacks);
#endif
#if CFSet || CFBag
    CFMutableHashRef hc = __THashName(Init)(allocator, __kCFHashMutable, numValues, &kCFTypeHashKeyCallBacks);
#endif
    __THashName(Grow)(hc, numValues);
    for (CFIndex idx = 0; idx < numValues; idx++) {
        CFIndex match, nomatch;
        __THashName(FindBuckets2)(hc, (any_t)keys[idx], &match, &nomatch);
        if (kCFNotFound == match) {
            CFAllocatorRef allocator = CFGetAllocator(hc);
            any_t newKey = (any_t)keys[idx];
            if (__CFHashKeyIsMagic(hc, newKey)) {
                __THashName(FindNewMarker)(hc);
            }
            if (hc->_keys[nomatch] == ~hc->_marker) {
                hc->_deletes--;
            }
            hc->_keys[nomatch] = newKey;
#if CFDictionary
            any_t newValue = (any_t)values[idx];
            hc->_values[nomatch] = newValue;
#endif
#if CFBag
            hc->_values[nomatch] = 1;
#endif
            hc->_bucketsUsed++;
            hc->_count++;
        } else {
            CFAllocatorRef allocator = CFGetAllocator(hc);
#if CFSet || CFBag
            any_t oldKey = hc->_keys[match];
            any_t newKey = (any_t)keys[idx];
            hc->_keys[match] = ~hc->_marker;
            if (__CFHashKeyIsMagic(hc, newKey)) {
                __THashName(FindNewMarker)(hc);
            }
            hc->_keys[match] = newKey;
            RELEASEKEY(oldKey);
#endif
#if CFDictionary
            any_t oldValue = hc->_values[match];
            any_t newValue = (any_t)values[idx];
            hc->_values[match] = newValue;
            RELEASEVALUE(oldValue);
#endif
        }
    }
    if (!isMutable) _CFBitfieldSetValue(hc->_xflags, 1, 0, __kCFHashImmutable);
    return (CFHashRef)hc;
}
#endif

CFHashRef THashName(CreateCopy)(CFAllocatorRef allocator, CFHashRef other) {
    CFMutableHashRef hc = THashName(CreateMutableCopy)(allocator, THashName(GetCount)(other), other);
    _CFBitfieldSetValue(hc->_xflags, 1, 0, __kCFHashImmutable);
    return hc;
}

CFMutableHashRef THashName(CreateMutableCopy)(CFAllocatorRef allocator, CFIndex capacity, CFHashRef other) {
    CFIndex numValues = THashName(GetCount)(other);
    const_any_pointer_t *list, buffer[256];
    list = (numValues <= 256) ? buffer : (const_any_pointer_t *)CFAllocatorAllocate(allocator, numValues * sizeof(const_any_pointer_t), 0);
#if CFDictionary
    const_any_pointer_t *vlist, vbuffer[256];
    vlist = (numValues <= 256) ? vbuffer : (const_any_pointer_t *)CFAllocatorAllocate(allocator, numValues * sizeof(const_any_pointer_t), 0);
#endif
#if CFSet || CFBag
    THashName(GetValues)(other, list);
#endif
#if CFDictionary
    THashName(GetKeysAndValues)(other, list, vlist);
#endif
    const CFHashKeyCallBacks *kcb;
    const CFHashValueCallBacks *vcb;
    if (CF_IS_OBJC(other)) {
        kcb = &kCFTypeHashKeyCallBacks;
        vcb = &kCFTypeHashValueCallBacks;
    } else {
        kcb = __THashName(GetKeyCallBacks)(other);
        vcb = __THashName(GetValueCallBacks)(other);
    }
#if CFDictionary
    CFMutableHashRef hc = __THashName(Init)(allocator, __kCFHashMutable, capacity, kcb, vcb);
#endif
#if CFSet || CFBag
    CFMutableHashRef hc = __THashName(Init)(allocator, __kCFHashMutable, capacity, kcb);
#endif
    if (0 == capacity) _THashName(SetCapacity)(hc, numValues);
    for (CFIndex idx = 0; idx < numValues; idx++) {
#if CFDictionary
        THashName(AddValue)(hc, list[idx], vlist[idx]);
#endif
#if CFSet || CFBag
        THashName(AddValue)(hc, list[idx]);
#endif
    }
    if (list != buffer) CFAllocatorDeallocate(allocator, list);
#if CFDictionary
    if (vlist != vbuffer) CFAllocatorDeallocate(allocator, vlist);
#endif
    return hc;
}

// Used by NSHashTables/NSMapTables and KVO
void _THashName(SetContext)(CFHashRef hc, any_pointer_t context) {
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    ((CFMutableHashRef)hc)->_context = context;
}

any_pointer_t _THashName(GetContext)(CFHashRef hc) {
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    return hc->_context;
}

CFIndex THashName(GetCount)(CFHashRef hc) {
    if (CFDictionary || CFSet) CF_OBJC_FUNCDISPATCH(CFIndex, hc, "count");
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    return hc->_count;
}

#if CFDictionary
CFIndex THashName(GetCountOfKey)(CFHashRef hc, const_any_pointer_t key) {
#endif
#if CFSet || CFBag
CFIndex THashName(GetCountOfValue)(CFHashRef hc, const_any_pointer_t key) {
#endif
    if (CFDictionary) CF_OBJC_FUNCDISPATCH(CFIndex, hc, "countForKey:", key);
    if (CFSet) CF_OBJC_FUNCDISPATCH(CFIndex, hc, "countForObject:", key);
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    if (0 == hc->_bucketsUsed) return 0;
    CFIndex match = __THashName(FindBuckets1)(hc, (any_t)key);
    return (kCFNotFound != match ? __CFHashGetOccurrenceCount(hc, match) : 0);
}

#if CFDictionary
Boolean THashName(ContainsKey)(CFHashRef hc, const_any_pointer_t key) {
#endif
#if CFSet || CFBag
Boolean THashName(ContainsValue)(CFHashRef hc, const_any_pointer_t key) {
#endif
    if (CFDictionary) CF_OBJC_FUNCDISPATCH(char, hc, "containsKey:", key);
    if (CFSet) CF_OBJC_FUNCDISPATCH(char, hc, "containsObject:", key);
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    if (0 == hc->_bucketsUsed) return false;
    CFIndex match = __THashName(FindBuckets1)(hc, (any_t)key);
    return (kCFNotFound != match ? true : false);
}

#if CFDictionary
CFIndex THashName(GetCountOfValue)(CFHashRef hc, const_any_pointer_t value) {
    CF_OBJC_FUNCDISPATCH(CFIndex, hc, "countForObject:", value);
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    if (0 == hc->_bucketsUsed) return 0;
    any_t *keys = hc->_keys;
    Boolean (*equal)(any_t, any_t, any_pointer_t) = (Boolean (*)(any_t, any_t, any_pointer_t))__THashName(GetValueCallBacks)(hc)->equal;
    CFIndex cnt = 0;
    for (CFIndex idx = 0, nbuckets = hc->_bucketsNum; idx < nbuckets; idx++) {
        if (__CFHashKeyIsValue(hc, keys[idx])) {
            if ((hc->_values[idx] == (any_t)value) || (equal && INVOKE_CALLBACK3(equal, hc->_values[idx], (any_t)value, hc->_context))) {
                cnt++;
            }
        }
    }
    return cnt;
}

Boolean THashName(ContainsValue)(CFHashRef hc, const_any_pointer_t value) {
    CF_OBJC_FUNCDISPATCH(char, hc, "containsObject:", value);
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    if (0 == hc->_bucketsUsed) return false;
    any_t *keys = hc->_keys;
    Boolean (*equal)(any_t, any_t, any_pointer_t) = (Boolean (*)(any_t, any_t, any_pointer_t))__THashName(GetValueCallBacks)(hc)->equal;
    for (CFIndex idx = 0, nbuckets = hc->_bucketsNum; idx < nbuckets; idx++) {
        if (__CFHashKeyIsValue(hc, keys[idx])) {
            if ((hc->_values[idx] == (any_t)value) || (equal && INVOKE_CALLBACK3(equal, hc->_values[idx], (any_t)value, hc->_context))) {
                return true;
            }
        }
    }
    return false;
}
#endif

const_any_pointer_t THashName(GetValue)(CFHashRef hc, const_any_pointer_t key) {
    if (CFDictionary) CF_OBJC_FUNCDISPATCH(const_any_pointer_t, hc, "objectForKey:", key);
    if (CFSet) CF_OBJC_FUNCDISPATCH(const_any_pointer_t, hc, "member:", key);
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    if (0 == hc->_bucketsUsed) return 0;
    CFIndex match = __THashName(FindBuckets1)(hc, (any_t)key);
    return (kCFNotFound != match ? (const_any_pointer_t)(CFDictionary ? hc->_values[match] : hc->_keys[match]) : 0);
}

Boolean THashName(GetValueIfPresent)(CFHashRef hc, const_any_pointer_t key, const_any_pointer_t *value) {
    if (CFDictionary) CF_OBJC_FUNCDISPATCH(Boolean, hc, "_getValue:forKey:", (any_t *)value, key);
    if (CFSet) CF_OBJC_FUNCDISPATCH(Boolean, hc, "_getValue:forObj:", (any_t *)value, key);
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    if (0 == hc->_bucketsUsed) return false;
    CFIndex match = __THashName(FindBuckets1)(hc, (any_t)key);
    return (kCFNotFound != match ? ((value ? (*value = (const_any_pointer_t)(CFDictionary ? hc->_values[match] : hc->_keys[match])) : 0), true): false);
}

#if CFDictionary
Boolean THashName(GetKeyIfPresent)(CFHashRef hc, const_any_pointer_t key, const_any_pointer_t *actualkey) {
    CF_OBJC_FUNCDISPATCH(Boolean, hc, "getActualKey:forKey:", actualkey, key);
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    if (0 == hc->_bucketsUsed) return false;
    CFIndex match = __THashName(FindBuckets1)(hc, (any_t)key);
    return (kCFNotFound != match ? ((actualkey ? (*actualkey=(const_any_pointer_t)hc->_keys[match]) : NULL), true) : false);
}
#endif

#if CFDictionary
void THashName(GetKeysAndValues)(CFHashRef hc, const_any_pointer_t *keybuf, const_any_pointer_t *valuebuf) {
#endif
#if CFSet || CFBag
void THashName(GetValues)(CFHashRef hc, const_any_pointer_t *keybuf) {
    const_any_pointer_t *valuebuf = 0;
#endif
    if (CFDictionary) CF_OBJC_VOID_FUNCDISPATCH(hc, "getObjects:andKeys:", (any_t *)valuebuf, (any_t *)keybuf);
    if (CFSet) CF_OBJC_VOID_FUNCDISPATCH(hc, "getObjects:", (any_t *)keybuf);
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    any_t *keys = hc->_keys;
    for (CFIndex idx = 0, nbuckets = hc->_bucketsNum; idx < nbuckets; idx++) {
        if (__CFHashKeyIsValue(hc, keys[idx])) {
            for (CFIndex cnt = __CFHashGetOccurrenceCount(hc, idx); cnt--;) {
                if (keybuf) *keybuf++ = (const_any_pointer_t)keys[idx];
                if (valuebuf) *valuebuf++ = (const_any_pointer_t)hc->_values[idx];
            }
        }
    }
}

#if CFDictionary || CFSet
CFIndex _THashName(FastEnumeration)(CFHashRef hc, _CFObjcFastEnumerationState *state, void *stackbuffer, CFIndex count) {
    /* copy as many as count items over */
    if (0 == state->state) {        /* first time */
        state->mutationsPtr = (void*)&hc->_mutations;
    }
    state->itemsPtr = (void**)stackbuffer;
    CFIndex cnt = 0;
    any_t *keys = hc->_keys;
    for (CFIndex idx = (CFIndex)state->state, nbuckets = hc->_bucketsNum; idx < nbuckets && cnt < (CFIndex)count; idx++) {
        if (__CFHashKeyIsValue(hc, keys[idx])) {
            state->itemsPtr[cnt++] = keys[idx];
        }
        state->state++;
    }
    return cnt;
}
#endif

void THashName(ApplyFunction)(CFHashRef hc, THashName(ApplierFunction) applier, any_pointer_t context) {
    if (CFDictionary) CF_OBJC_VOID_FUNCDISPATCH(hc, "_apply:context:", applier, context);
    if (CFSet) CF_OBJC_VOID_FUNCDISPATCH(hc, "_applyValues:context:", applier, context);
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    any_t *keys = hc->_keys;
    for (CFIndex idx = 0, nbuckets = hc->_bucketsNum; idx < nbuckets; idx++) {
        if (__CFHashKeyIsValue(hc, keys[idx])) {
            for (CFIndex cnt = __CFHashGetOccurrenceCount(hc, idx); cnt--;) {
#if CFDictionary
                INVOKE_CALLBACK3(applier, (const_any_pointer_t)keys[idx], (const_any_pointer_t)hc->_values[idx], context);
#endif
#if CFSet || CFBag
                INVOKE_CALLBACK2(applier, (const_any_pointer_t)keys[idx], context);
#endif
            }
        }
    }
}

static void __THashName(Grow)(CFMutableHashRef hc, CFIndex numNewValues) {
    any_t *oldkeys = hc->_keys;
    any_t *oldvalues = hc->_values;
    CFIndex nbuckets = hc->_bucketsNum;
    hc->_bucketsCap = __CFHashRoundUpCapacity(hc->_bucketsUsed + numNewValues);
    hc->_bucketsNum = __CFHashNumBucketsForCapacity(hc->_bucketsCap);
    hc->_deletes = 0;
    CFAllocatorRef allocator = CFGetAllocator(hc);
    any_t *mem = (any_t *)CFAllocatorAllocate(allocator, hc->_bucketsNum * sizeof(any_t), 0);
    if (NULL == mem) __THashName(HandleOutOfMemory)(hc, hc->_bucketsNum * sizeof(any_t));
    hc->_keys = mem;
    any_t *keysBase = mem;
#if CFDictionary || CFBag
    mem = (any_t *)CFAllocatorAllocate(allocator, hc->_bucketsNum * sizeof(any_t), 0);
    if (NULL == mem) __THashName(HandleOutOfMemory)(hc, hc->_bucketsNum * sizeof(any_t));
    hc->_values = mem;
#endif
#if CFDictionary
    any_t *valuesBase = mem;
#endif
    for (CFIndex idx = 0, nbuckets = hc->_bucketsNum; idx < nbuckets; idx++) {
        hc->_keys[idx] = hc->_marker;
#if CFDictionary || CFBag
        hc->_values[idx] = 0;
#endif
    }
    if (NULL == oldkeys) return;
    for (CFIndex idx = 0; idx < nbuckets; idx++) {
        if (__CFHashKeyIsValue(hc, oldkeys[idx])) {
            CFIndex match, nomatch;
            __THashName(FindBuckets2)(hc, oldkeys[idx], &match, &nomatch);
            CF_VALIDATE(kCFRuntimeErrorGeneric,
                kCFNotFound == match, 
                "two values (%p, %p) now hash to the same slot; "
                    "mutable value changed while in table or hash value is not immutable",
                oldkeys[idx], hc->_keys[match]);
            if (kCFNotFound != nomatch) {
                hc->_keys[nomatch] = oldkeys[idx];
#if CFDictionary
                hc->_values[nomatch] = oldvalues[idx];
#endif
#if CFBag
                hc->_values[nomatch] = oldvalues[idx];
#endif
            }
        }
    }
    CFAllocatorDeallocate(allocator, oldkeys);
    CFAllocatorDeallocate(allocator, oldvalues);
}

// This function is for Foundation's benefit; no one else should use it.
void _THashName(SetCapacity)(CFMutableHashRef hc, CFIndex cap) {
    if (CF_IS_OBJC(hc)) return;
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    CF_VALIDATE_ARG(__CFHashGetType(hc) != __kCFHashImmutable, "collection is immutable");
    CF_VALIDATE_ARG(hc->_bucketsUsed <= cap, "desired capacity (%ld) is less than bucket count (%ld)", cap, hc->_bucketsUsed);
    __THashName(Grow)(hc, cap - hc->_bucketsUsed);
}


#if CFDictionary
void THashName(AddValue)(CFMutableHashRef hc, const_any_pointer_t key, const_any_pointer_t value) {
#endif
#if CFSet || CFBag
void THashName(AddValue)(CFMutableHashRef hc, const_any_pointer_t key) {
    #define value 0
#endif
    if (CFDictionary) CF_OBJC_VOID_FUNCDISPATCH(hc, "_addObject:forKey:", value, key);
    if (CFSet) CF_OBJC_VOID_FUNCDISPATCH(hc, "addObject:", key);
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    switch (__CFHashGetType(hc)) {
    case __kCFHashMutable:
        if (hc->_bucketsUsed == hc->_bucketsCap || NULL == hc->_keys) {
            __THashName(Grow)(hc, 1);
        }
        break;
    default:
        CF_VALIDATE_ARG(__CFHashGetType(hc) != __kCFHashImmutable, "collection is immutable");
        break;
    }
    hc->_mutations++;
    CFIndex match, nomatch;
    __THashName(FindBuckets2)(hc, (any_t)key, &match, &nomatch);
    if (kCFNotFound != match) {
#if CFBag
        CF_OBJC_KVO_WILLCHANGE(hc, hc->_keys[match]);
        hc->_values[match]++;
        hc->_count++;
        CF_OBJC_KVO_DIDCHANGE(hc, hc->_keys[match]);
#endif
    } else {
        CFAllocatorRef allocator = CFGetAllocator(hc);
        GETNEWKEY(newKey, key);
#if CFDictionary
        GETNEWVALUE(newValue);
#endif
        if (__CFHashKeyIsMagic(hc, newKey)) {
            __THashName(FindNewMarker)(hc);
        }
        if (hc->_keys[nomatch] == ~hc->_marker) {
            hc->_deletes--;
        }
        CF_OBJC_KVO_WILLCHANGE(hc, key);
        hc->_keys[nomatch] = newKey;
#if CFDictionary
        hc->_values[nomatch] = newValue;
#endif
#if CFBag
        hc->_values[nomatch] = 1;
#endif
        hc->_bucketsUsed++;
        hc->_count++;
        CF_OBJC_KVO_DIDCHANGE(hc, key);
    }
}

#if CFDictionary
void THashName(ReplaceValue)(CFMutableHashRef hc, const_any_pointer_t key, const_any_pointer_t value) {
#endif
#if CFSet || CFBag
void THashName(ReplaceValue)(CFMutableHashRef hc, const_any_pointer_t key) {
    #define value 0
#endif
    if (CFDictionary) CF_OBJC_VOID_FUNCDISPATCH(hc, "_replaceObject:forKey:", value, key);
    if (CFSet) CF_OBJC_VOID_FUNCDISPATCH(hc, "_replaceObject:", key);
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    switch (__CFHashGetType(hc)) {
    case __kCFHashMutable:
        break;
    default:
        CF_VALIDATE_ARG(__CFHashGetType(hc) != __kCFHashImmutable, "collection is immutable");
        break;
    }
    hc->_mutations++;
    if (0 == hc->_bucketsUsed) return;
    CFIndex match = __THashName(FindBuckets1)(hc, (any_t)key);
    if (kCFNotFound == match) return;
    CFAllocatorRef allocator = CFGetAllocator(hc);
#if CFSet || CFBag
    GETNEWKEY(newKey, key);
#endif
#if CFDictionary
    GETNEWVALUE(newValue);
#endif
    any_t oldKey = hc->_keys[match];
    CF_OBJC_KVO_WILLCHANGE(hc, oldKey);
#if CFSet || CFBag
    hc->_keys[match] = ~hc->_marker;
    if (__CFHashKeyIsMagic(hc, newKey)) {
        __THashName(FindNewMarker)(hc);
    }
    hc->_keys[match] = newKey;
#endif
#if CFDictionary
    any_t oldValue = hc->_values[match];
    hc->_values[match] = newValue;
#endif
    CF_OBJC_KVO_DIDCHANGE(hc, oldKey);
#if CFSet || CFBag
    RELEASEKEY(oldKey);
#endif
#if CFDictionary
    RELEASEVALUE(oldValue);
#endif
}

#if CFDictionary
void THashName(SetValue)(CFMutableHashRef hc, const_any_pointer_t key, const_any_pointer_t value) {
#endif
#if CFSet || CFBag
void THashName(SetValue)(CFMutableHashRef hc, const_any_pointer_t key) {
    #define value 0
#endif
    if (CFDictionary) CF_OBJC_VOID_FUNCDISPATCH(hc, "setObject:forKey:", value, key);
    if (CFSet) CF_OBJC_VOID_FUNCDISPATCH(hc, "_setObject:", key);
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    switch (__CFHashGetType(hc)) {
    case __kCFHashMutable:
        if (hc->_bucketsUsed == hc->_bucketsCap || NULL == hc->_keys) {
            __THashName(Grow)(hc, 1);
        }
        break;
    default:
        CF_VALIDATE_ARG(__CFHashGetType(hc) != __kCFHashImmutable, "collection is immutable");
        break;
    }
    hc->_mutations++;
    CFIndex match, nomatch;
    __THashName(FindBuckets2)(hc, (any_t)key, &match, &nomatch);
    if (kCFNotFound == match) {
        CFAllocatorRef allocator = CFGetAllocator(hc);
        GETNEWKEY(newKey, key);
#if CFDictionary
        GETNEWVALUE(newValue);
#endif
        if (__CFHashKeyIsMagic(hc, newKey)) {
            __THashName(FindNewMarker)(hc);
        }
        if (hc->_keys[nomatch] == ~hc->_marker) {
            hc->_deletes--;
        }
        CF_OBJC_KVO_WILLCHANGE(hc, key);
        hc->_keys[nomatch] = newKey;
#if CFDictionary
        hc->_values[nomatch] = newValue;
#endif
#if CFBag
        hc->_values[nomatch] = 1;
#endif
        hc->_bucketsUsed++;
        hc->_count++;
        CF_OBJC_KVO_DIDCHANGE(hc, key);
    } else {
        CFAllocatorRef allocator = CFGetAllocator(hc);
#if CFSet || CFBag
        GETNEWKEY(newKey, key);
#endif
#if CFDictionary
        GETNEWVALUE(newValue);
#endif
        any_t oldKey = hc->_keys[match];
        CF_OBJC_KVO_WILLCHANGE(hc, oldKey);
#if CFSet || CFBag
        hc->_keys[match] = ~hc->_marker;
        if (__CFHashKeyIsMagic(hc, newKey)) {
            __THashName(FindNewMarker)(hc);
        }
        hc->_keys[match] = newKey;
#endif
#if CFDictionary
        any_t oldValue = hc->_values[match];
        hc->_values[match] = newValue;
#endif
        CF_OBJC_KVO_DIDCHANGE(hc, oldKey);
#if CFSet || CFBag
        RELEASEKEY(oldKey);
#endif
#if CFDictionary
        RELEASEVALUE(oldValue);
#endif
    }
}

void THashName(RemoveValue)(CFMutableHashRef hc, const_any_pointer_t key) {
    if (CFDictionary) CF_OBJC_VOID_FUNCDISPATCH(hc, "removeObjectForKey:", key);
    if (CFSet) CF_OBJC_VOID_FUNCDISPATCH(hc, "removeObject:", key);
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    switch (__CFHashGetType(hc)) {
    case __kCFHashMutable:
        break;
    default:
        CF_VALIDATE_ARG(__CFHashGetType(hc) != __kCFHashImmutable, "collection is immutable");
        break;
    }
    hc->_mutations++;
    if (0 == hc->_bucketsUsed) return;
    CFIndex match = __THashName(FindBuckets1)(hc, (any_t)key);
    if (kCFNotFound == match) return;
    if (1 < __CFHashGetOccurrenceCount(hc, match)) {
#if CFBag
        CF_OBJC_KVO_WILLCHANGE(hc, hc->_keys[match]);
        hc->_values[match]--;
        hc->_count--;
        CF_OBJC_KVO_DIDCHANGE(hc, hc->_keys[match]);
#endif
    } else {
        CFAllocatorRef allocator = CFGetAllocator(hc);
        any_t oldKey = hc->_keys[match];
        CF_OBJC_KVO_WILLCHANGE(hc, oldKey);
        hc->_keys[match] = ~hc->_marker;
#if CFDictionary
        any_t oldValue = hc->_values[match];
        hc->_values[match] = 0;
#endif
#if CFBag
        hc->_values[match] = 0;
#endif
        hc->_count--;
        hc->_bucketsUsed--;
        hc->_deletes++;
        CF_OBJC_KVO_DIDCHANGE(hc, oldKey);
        RELEASEKEY(oldKey);
#if CFDictionary
        RELEASEVALUE(oldValue);
#endif
        if (__THashName(ShouldShrink)(hc)) {
            __THashName(Grow)(hc, 0);
        } else {
            // When the probeskip == 1 always and only, a DELETED slot followed by an EMPTY slot
            // can be converted to an EMPTY slot.  By extension, a chain of DELETED slots followed
            // by an EMPTY slot can be converted to EMPTY slots, which is what we do here.
            if (match < hc->_bucketsNum - 1 && hc->_keys[match + 1] == hc->_marker) {
                while (0 <= match && hc->_keys[match] == ~hc->_marker) {
                    hc->_keys[match] = hc->_marker;
                    hc->_deletes--;
                    match--;
                }
            }
        }
    }
}

void THashName(RemoveAllValues)(CFMutableHashRef hc) {
    if (CFDictionary) CF_OBJC_VOID_FUNCDISPATCH(hc, "removeAllObjects");
    if (CFSet) CF_OBJC_VOID_FUNCDISPATCH(hc, "removeAllObjects");
    CF_VALIDATE_OBJECT_ARG(CF, hc, __kCFHashTypeID);
    switch (__CFHashGetType(hc)) {
    case __kCFHashMutable:
        break;
    default:
        CF_VALIDATE_ARG(__CFHashGetType(hc) != __kCFHashImmutable, "collection is immutable");
        break;
    }
    hc->_mutations++;
    if (0 == hc->_bucketsUsed) return;
    CFAllocatorRef allocator = CFGetAllocator(hc);
    any_t *keys = hc->_keys;
    for (CFIndex idx = 0, nbuckets = hc->_bucketsNum; idx < nbuckets; idx++) {
        if (__CFHashKeyIsValue(hc, keys[idx])) {
            any_t oldKey = keys[idx];
            CF_OBJC_KVO_WILLCHANGE(hc, oldKey);
#if CFDictionary || CFSet
            hc->_count--;
#endif
#if CFBag
            hc->_count -= (CFIndex)hc->_values[idx];
#endif
            hc->_keys[idx] = ~hc->_marker;
#if CFDictionary
            any_t oldValue = hc->_values[idx];
            hc->_values[idx] = 0;
#endif
#if CFBag
            hc->_values[idx] = 0;
#endif
            hc->_bucketsUsed--;
            hc->_deletes++;
            CF_OBJC_KVO_DIDCHANGE(hc, oldKey);
            RELEASEKEY(oldKey);
#if CFDictionary
            RELEASEVALUE(oldValue);
#endif
        }
    }
    for (CFIndex idx = 0, nbuckets = hc->_bucketsNum; idx < nbuckets; idx++) {
        keys[idx] = hc->_marker;
    }
    hc->_deletes = 0;
    hc->_bucketsUsed = 0;
    hc->_count = 0;
    if (__THashName(ShouldShrink)(hc) && (256 <= hc->_bucketsCap)) {
        __THashName(Grow)(hc, 128);
    }
}
