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

#if !defined(__COREFOUNDATION_CFNUMBERINTERNAL__)
#define __COREFOUNDATION_CFNUMBERINTERNAL__  1

// Fields in this struct may switch position someday, 
//  do not use '= {high, low}' -style initialization.
typedef struct {
    int64_t high;
    uint64_t low;
} CFSInt128Struct;

enum {
    kCFNumberSInt128Type = 17
};

CF_EXTERN_C_BEGIN

CF_EXPORT
void _CFNumberInitialize(void);

CF_EXPORT
CFStringRef _CFNumberCopyFormattingDescriptionAsFloat64(CFTypeRef cf);

//TODO WTF 2 in _CFNumberGetType2 ? Rename to something like _CFNumberGetTrueType.
/* Function supports kCFNumberSInt128Type (CFNumberGetType converts 
 *  it to kCFNumberSInt64Type).
 */
CF_EXPORT
CFNumberType _CFNumberGetType2(CFNumberRef number);

CF_EXTERN_C_END

#endif /* !__COREFOUNDATION_CFNUMBERINTERNAL__ */
