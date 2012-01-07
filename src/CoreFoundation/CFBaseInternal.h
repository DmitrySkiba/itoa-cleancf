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

#if !defined(__COREFOUNDATION_CFBASEINTERNAL__)
#define __COREFOUNDATION_CFBASEINTERNAL__ 1

#include <CoreFoundation/CFBase.h>
#include <limits.h>

#if defined(_MSC_VER)
    #define __PRETTY_FUNCTION__ __FUNCTION__
#elif !defined(__GNUC__)
    #define __PRETTY_FUNCTION__ "<unknown>"
#endif

#if defined(__BIG_ENDIAN__)
    #define __CF_BIG_ENDIAN__ 1
    #define __CF_LITTLE_ENDIAN__ 0
#endif

#if defined(__LITTLE_ENDIAN__)
    #define __CF_LITTLE_ENDIAN__ 1
    #define __CF_BIG_ENDIAN__ 0
#endif

#if !defined(LLONG_MAX)
    #if defined(_I64_MAX)
        #define LLONG_MAX _I64_MAX
    #else
        #error "Can't define LLONG_MAX!"
    #endif
#endif

#if !defined(LLONG_MIN)
    #if defined(_I64_MIN)
        #define LLONG_MIN _I64_MIN
    #else
        #error "Can't define LLONG_MIN!"
    #endif
#endif

#if defined(__GNUC__) && !defined(__STRICT_ANSI__)
    #define _CFMin(A,B) ({__typeof__(A) __a = (A); __typeof__(B) __b = (B); __a < __b ? __a : __b; })
    #define _CFMax(A,B) ({__typeof__(A) __a = (A); __typeof__(B) __b = (B); __a < __b ? __b : __a; })
#else
    #define _CFMin(A,B) ((A) < (B) ? (A) : (B))
    #define _CFMax(A,B) ((A) > (B) ? (A) : (B))
#endif

#if defined(__GNUC__)
    #define _CF_ARRAY_ALLOCA(Type, variable, size) \
		Type* variable = (Type*)__builtin_alloca(size * sizeof(Type));
#elif defined(_MSC_VER)
    #include <malloc.h>
    #define _CF_ARRAY_ALLOCA(Type, variable, size) \
		Type* variable = (Type*)_alloca(size * sizeof(Type));
#endif

#if defined(__HAS_PRIVATE_EXTERN__)
	#define CF_INTERNAL __private_extern__
#elif defined(__GNUC__)
	#define CF_INTERNAL __attribute__((visibility("hidden")))
#else
	#define CF_INTERNAL
#endif

// Use in situations where plain cast can cause warnings,
//  e.g. when casting away const or casting small integers
//  to pointers.
#define CF_CAST(Type, value) ((Type)(uintptr_t)(value))

//TODO Deprecated, must be replaced with CF_CAST
#define CF_CONST_CAST(Type, pointer) CF_CAST(Type, pointer)

#define CF_COUNTOF(array) (sizeof(array)/sizeof(*(array)))

// CFRuntimeBase manipulation
#define CF_BASE(cf) CF_CAST(CFRuntimeBase*, cf)
#define CF_INFO(cf) (CF_BASE(cf)->_cfinfo[CF_INFO_BITS])
#define CF_TYPEID(cf) ((CF_FULLINFO(cf) >> 8) & 0xFFFF)
#define CF_FULLINFO(cf) (*(uint32_t*)(CF_BASE(cf)->_cfinfo))
#define CF_MAKE_FULLINFO(typeID, info) ((uint32_t)(((typeID) & 0xFFFF) << 8) | ((info) & 0xFF))

//TODO this thing is broken - find out what type itemsPtr should be
//     (easiest way is to actually use CF' fast enumerators from NS)
typedef struct {
    CFULong state;
    void** itemsPtr;
    void* mutationsPtr;
    CFULong extra[5];
} _CFObjcFastEnumerationState;

CF_EXTERN_C_BEGIN

CF_EXPORT
void _CFTypeInitialize(void);

CF_EXPORT
CFTypeID _CFNotATypeGetTypeID(void);

CF_EXPORT
const void* _CFTypeCollectionRetain(CFAllocatorRef allocator, const void* ptr);
CF_EXPORT
void _CFTypeCollectionRelease(CFAllocatorRef allocator, const void* ptr);

//TODO _CFRangeIsValid is not descriptive, rename.
CF_INLINE Boolean _CFRangeIsValid(CFRange range, CFIndex length) {
#ifndef __LP64__
    int64_t maxLocation = (int64_t)(uint32_t)range.location + (uint32_t)range.length;
#else
    #error Fix _CFRangeIsValid for 64-bit
#endif
    return maxLocation <= length;
}

CF_EXTERN_C_END

#endif /* !__COREFOUNDATION_CFBASEINTERNAL__ */
