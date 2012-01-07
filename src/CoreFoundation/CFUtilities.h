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

#if !defined(__COREFOUNDATION_CFUTILITIES__)
#define __COREFOUNDATION_CFUTILITIES__ 1

#include <CoreFoundation/CFBase.h>

/* Bit manipulation macros.
 *
 * Bits are numbered from 31 on left to 0 on right.
 * N1 and N2 specify an inclusive range N2..N1 with N1 >= N2.
 *
 *  _CFBitfieldMask(N1, N2)
 *  _CFBitfieldGetValue(V, N1, N2)
 *  _CFBitfieldSetValue(V, N1, N2, X)
 *  _CFBitfieldMaxValue(N1, N2)
 *  _CFBitIsSet(V, N)
 *  _CFBitSet(V, N)
 *  _CFBitClear(V, N)
 * 
 * May or may not work if you use them on bitfields in types other 
 *  than UInt32, bitfields the full width of a UInt32, or anything 
 *  else for which they were not designed.
 */
#define _CFBitfieldMask(N1, N2) \
    ((((UInt32)~0UL) << (31UL - (N1) + (N2))) >> (31UL - N1))
#define _CFBitfieldGetValue(V, N1, N2) \
    (((V) & _CFBitfieldMask(N1, N2)) >> (N2))
#define _CFBitfieldSetValue(V, N1, N2, X) \
    ((V) = ((V) & ~_CFBitfieldMask(N1, N2)) | (((X) << (N2)) & _CFBitfieldMask(N1, N2)))
#define _CFBitfieldMaxValue(N1, N2) \
    _CFBitfieldGetValue(0xFFFFFFFFUL, (N1), (N2))
#define _CFBitIsSet(V, N) \
    (((V) & (1UL << (N))) != 0)
#define _CFBitSet(V, N) \
    ((V) |= (1UL << (N)))
#define _CFBitClear(V, N) \
    ((V) &= ~(1UL << (N)))


/* Finds last (most significant) bit set.
 * Least significant bit has the index of 1.
 * Returns 0 only if 'value' is 0.
 */
CF_INLINE int _CFLastBitSet(CFULong value) {
    int i;
 
    if (!value) {
        return 0;
    }
    for (i = 1; value != 1; i++) {
        value = value >> 1;
    }
    return i;
}


/* Binary searches a sorted-increasing array of some type.
 * Return value is either 
 *   * the index of the element desired,
 *      if the target value exists in the list,
 *   * greater than or equal to count,
 *      if the element is greater than all the values in the list,
 *   * the index of the element greater than the target value.
 *
 * For example, a search in the list of integers:
 *  2 3 5 7 11 13 17
 *
 * For...        Will Return...
 *  2             0
 *  5             2
 *  23            7
 *  1             0
 *  9             4
 *
 * For instance, if you just care about found/not found:
 * index = _CFBSearch(list, count, elem);
 * if (count <= index || list[index] != elem) {
 *     * Not found *
 * } else {
 *     * Found *
 * }
 *
 * Comparator is passed the address of the values.
 */
CF_EXPORT CFIndex _CFBSearch(const void *element, CFIndex elementSize,
                             const void *list, CFIndex count,
                             CFComparatorFunction comparator, void *context);


CF_EXPORT CFHashCode _CFHashBytes(UInt8 *bytes, CFIndex length);

#endif /* ! __COREFOUNDATION_CFUTILITIES__ */
