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

#if !defined(__COREFOUNDATION_CFCONSTSTRING__)
#define __COREFOUNDATION_CFCONSTSTRING__ 1

#include <CoreFoundation/CFString.h>
#include "CFStringInternal.h"

//TODO STATIC_CONST_STRING_DECL, use it (CFURL, etc.)

///////////////////////////////////////////////////////////////////// with compiler support

#ifdef __CONSTANT_CFSTRINGS__

#define CONST_STRING_DECL(Name, chars) \
    const CFStringRef Name = CFSTR(chars);

#endif // __CONSTANT_CFSTRINGS__

///////////////////////////////////////////////////////////////////// without compiler support

#ifndef __CONSTANT_CFSTRINGS__

struct CF_CONST_STRING {
    CFRuntimeBase _base;
    const char* _ptr;
    uint32_t _length;
};

extern int __CFConstantStringClassReference[];

/* For the description of 0xc8 flag see CFString_Common.h */

#if __CF_BIG_ENDIAN__

#define CONST_STRING_DECL(Name, chars) \
    static struct CF_CONST_STRING __ ## Name ## __ = { \
        {&__CFConstantStringClassReference, {0x00, 0x00, _kCFStringTypeID, 0xc8}, 0}, \
        chars, sizeof(chars) - 1 \
    }; \
    const CFStringRef Name = (CFStringRef) & __ ## Name ## __;

#else

#define CONST_STRING_DECL(Name, chars) \
    static struct CF_CONST_STRING __ ## Name ## __ = { \
        {&__CFConstantStringClassReference, {0xc8, _kCFStringTypeID, 0x00, 0x00}, 0}, \
        chars, sizeof(chars) - 1 \
    }; \
    const CFStringRef Name = (CFStringRef) & __ ## Name ## __;

#endif // __CF_BIG_ENDIAN__

#endif // !__CONSTANT_CFSTRINGS__

/////////////////////////////////////////////////////////////////////

#endif /* ! __COREFOUNDATION_CFCONSTSTRING__ */
