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

#if !defined(__COREFOUNDATION_CFBASE__)
#define __COREFOUNDATION_CFBASE__ 1

#if defined(_MSC_VER)
	#if defined(_M_IX86)
		#define __i386__ 1
	#else if defined(_M_X64) || defined(_M_AMD64)
		#define __x86_64__ 1
	#endif
#endif

#if (defined(__i386__) || defined(__x86_64__) || (defined (__arm__) && !defined (__ARMEB__))) && !defined(__LITTLE_ENDIAN__)
	#define __LITTLE_ENDIAN__ 1
#endif

#if ((defined (__arm__) && defined (__ARMEB__))) && !defined(__LITTLE_ENDIAN__)
	#define __BIG_ENDIAN__ 1
#endif

#if !defined(__BIG_ENDIAN__) && !defined(__LITTLE_ENDIAN__)
	#error Do not know the endianess of this architecture
#endif

#if !__BIG_ENDIAN__ && !__LITTLE_ENDIAN__
	#error Both __BIG_ENDIAN__ and __LITTLE_ENDIAN__ cannot be false
#endif

#if __BIG_ENDIAN__ && __LITTLE_ENDIAN__
	#error Both __BIG_ENDIAN__ and __LITTLE_ENDIAN__ cannot be true
#endif

#include <stdint.h>
#include <stdbool.h>

typedef unsigned char Boolean;
typedef unsigned char UInt8;
typedef signed char SInt8;
typedef unsigned short UInt16;
typedef signed short SInt16;
typedef unsigned int UInt32;
typedef signed int SInt32;
typedef uint64_t UInt64;
typedef int64_t SInt64;
typedef SInt32 OSStatus;
typedef float Float32;
typedef double Float64;
typedef unsigned short UniChar;
typedef unsigned char* StringPtr;
typedef const unsigned char* ConstStringPtr;
typedef unsigned char Str255[256];
typedef const unsigned char* ConstStr255Param;
typedef SInt16 OSErr;
typedef SInt16 RegionCode;
typedef SInt16 LangCode;
typedef UInt32 UTF32Char;
typedef UInt16 UTF16Char;
typedef UInt8 UTF8Char;

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64))
	typedef signed __int64 CFLong;
	typedef unsigned __int64 CFULong;
#else
	typedef signed long CFLong;
	typedef unsigned long CFULong;
#endif

#if !defined(NULL)
    #if defined(__GNUG__)
        #define NULL __null
    #elif defined(__cplusplus)
        #define NULL 0
    #else
        #define NULL ((void*)0)
    #endif
#endif

#if !defined(TRUE)
    #define TRUE 1
#endif

#if !defined(FALSE)
    #define FALSE 0
#endif

#if !defined(CF_EXTERN_C_BEGIN)
    #if defined(__cplusplus)
        #define CF_EXTERN_C_BEGIN extern "C" {
        #define CF_EXTERN_C_END   }
    #else
        #define CF_EXTERN_C_BEGIN
        #define CF_EXTERN_C_END
    #endif
#endif

#if defined(_MSC_VER)
    #if defined(CF_BUILDING_CF)
        #define CF_EXPORT __declspec(dllexport) extern
    #else
        #define CF_EXPORT __declspec(dllimport) extern
    #endif
#else
    #define CF_EXPORT extern
#endif

#if !defined(CF_INLINE)
    #if defined(__GNUC__) && (__GNUC__==4) && !defined(DEBUG)
        #define CF_INLINE static __inline__ __attribute__((always_inline))
    #elif defined(__GNUC__)
        #define CF_INLINE static __inline__
    #elif defined(__MWERKS__) || defined(__cplusplus)
        #define CF_INLINE static inline
    #elif defined(_MSC_VER)
        #define CF_INLINE static __inline
    #else
        #error "Can't define CF_INLINE for the platform!"
    #endif
#endif

CF_EXTERN_C_BEGIN

typedef CFULong CFTypeID;
typedef CFULong CFOptionFlags;
typedef CFULong CFHashCode;

typedef CFLong CFIndex;

/* Base "type" of all "CF objects", and polymorphic functions on them */
typedef const void* CFTypeRef;

typedef const struct __CFString* CFStringRef;
typedef struct __CFString* CFMutableStringRef;
    
typedef const struct __CFAllocator* CFAllocatorRef;
typedef const struct __CFDictionary* CFDictionaryRef;

/*
 * Type to mean any instance of a property list type;
 * currently, CFString, CFData, CFNumber, CFBoolean, CFDate,
 * CFArray, and CFDictionary.
 */
typedef CFTypeRef CFPropertyListRef;

/* Values returned from comparison functions */
enum {
    kCFCompareLessThan = -1,
    kCFCompareEqualTo = 0,
    kCFCompareGreaterThan = 1
};
typedef CFIndex CFComparisonResult;

/* A standard comparison function */
typedef CFComparisonResult (*CFComparatorFunction)(const void* val1, const void* val2, void* context);

/* Constant used by some functions to indicate failed searches. */
/* This is of type CFIndex. */
enum {
    kCFNotFound = -1
};

/* Range type */

typedef struct {
    CFIndex location;
    CFIndex length;
} CFRange;

CF_INLINE
CFRange CFRangeMake(CFIndex loc,CFIndex len) {
    CFRange range;
    range.location=loc;
    range.length=len;
    return range;
}

/* CFType */

CF_EXPORT
CFTypeID CFTypeGetTypeID(void);

/* Null representant */

typedef const struct __CFNull* CFNullRef;

CF_EXPORT
CFTypeID CFNullGetTypeID(void);

CF_EXPORT
const CFNullRef kCFNull; // the singleton null instance

/* Polymorphic CF functions */

CF_EXPORT
CFTypeID CFGetTypeID(CFTypeRef cf);

CF_EXPORT
CFStringRef CFCopyTypeIDDescription(CFTypeID type_id);

CF_EXPORT
CFTypeRef CFRetain(CFTypeRef cf);

CF_EXPORT
void CFRelease(CFTypeRef cf);

CF_EXPORT
CFIndex CFGetRetainCount(CFTypeRef cf);

CF_EXPORT
CFTypeRef CFMakeCollectable(CFTypeRef cf);

CF_EXPORT
Boolean CFEqual(CFTypeRef cf1,CFTypeRef cf2);

CF_EXPORT
CFHashCode CFHash(CFTypeRef cf);

CF_EXPORT
CFStringRef CFCopyDescription(CFTypeRef cf);

CF_EXPORT
CFStringRef CFCopyFormattingDescription(CFTypeRef cf, CFDictionaryRef formatOptions);
/*If type produces a formatting description, returns that string, otherwise NULL. */

CF_EXPORT
CFAllocatorRef CFGetAllocator(CFTypeRef cf);

CF_EXTERN_C_END

#include "CFAllocator.h"

#endif /* ! __COREFOUNDATION_CFBASE__ */
