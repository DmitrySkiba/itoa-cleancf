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

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFDictionary.h>
#include "CFStringEncoding.h"
#include "CFStringEncodingConverter.h"
#include "CFUniChar.h"
#include "CFStringInternal.h"
#include "CFInternal.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "CFString_Common.h"

#define CF_VALIDATE_MUTABLESTRING_ARG(string) \
    CF_VALIDATE_MUTABLEOBJECT_ARG(CF, string, _kCFStringTypeID, \
        __CFStrIsMutable(string));

typedef struct {
    CFIndex beginning;
    CFIndex length;
    CFIndex shift;
} __CFStringBlock;

typedef struct {
    CFIndex capacity;        // Capacity (if capacity == count, need to realloc to add another)
    CFIndex count;            // Number of elements actually stored
    __CFStringBlock* stack;
    Boolean stackAllocated;    // Indicates "stack" is allocated and needs to be deallocated when done
    char _padding[3];
} __CFStringBlockStack;

#define MAX_DECOMP_BUF 64

#define HANGUL_SBASE 0xAC00
#define HANGUL_LBASE 0x1100
#define HANGUL_VBASE 0x1161
#define HANGUL_TBASE 0x11A7
#define HANGUL_SCOUNT 11172
#define HANGUL_LCOUNT 19
#define HANGUL_VCOUNT 21
#define HANGUL_TCOUNT 28
#define HANGUL_NCOUNT (HANGUL_VCOUNT * HANGUL_TCOUNT)

///////////////////////////////////////////////////////////////////// private

CF_INLINE
void __CFStringBlockStackPop(__CFStringBlockStack* stack, __CFStringBlock* topBlock) {
    stack->count = stack->count - 1;
    *topBlock = stack->stack[stack->count];
}

CF_INLINE
void __CFStringBlockStackPush(__CFStringBlockStack* stack, const __CFStringBlock* newBlock) {
    if (stack->count == stack->capacity) {
        // increase size of the stack
        stack->capacity = (stack->capacity + 4) * 2;
        if (stack->stackAllocated) {
            stack->stack = (__CFStringBlock*)CFAllocatorReallocate(
                kCFAllocatorSystemDefault,
                stack->stack, stack->capacity * sizeof(__CFStringBlock), 0);
        } else {
            __CFStringBlock* newStack = (__CFStringBlock*)CFAllocatorAllocate(
                kCFAllocatorSystemDefault,
                stack->capacity * sizeof(__CFStringBlock), 0);
            memmove(newStack, stack->stack, stack->count * sizeof(__CFStringBlock));
            stack->stack = newStack;
            stack->stackAllocated = true;
        }
    }
    stack->stack[stack->count] = *newBlock;
    stack->count = stack->count + 1;
}

/* __CFStringRearrangeBlocks() rearranges the blocks of data within
 *  the buffer so that they are "evenly spaced". Buffer is assumed 
 *  to have enough room for the result.
 *
 * numBlocks is current total number of blocks within buffer.
 * blockSize is the size of each block in bytes.
 * ranges and numRanges hold the ranges that are no longer needed; 
 *  ranges are stored sorted in increasing order, and don't overlap.
 * insertLength is the final spacing between the remaining blocks.
 *
 * Example:
 *  buffer = A B C D E F G H,
 *  blockSize = 1,
 *  ranges = { (2,1) , (4,2) }  (so we want to "delete" C and E F),
 *  fromEnd = NO
 *  if insertLength = 4, result = A B ? ? ? ? D ? ? ? ? G H
 *  if insertLength = 0, result = A B D G H
 *
 * Example:
 *  buffer = A B C D E F G H I J K L M N O P Q R S T U,
 *  blockSize = 1,
 *  ranges { (1,1), (3,1), (5,11), (17,1), (19,1) },
 *  fromEnd = NO
 *  insertLength = 3,
 *  result = A ? ? ? C ? ? ? E ? ? ? Q ? ? ? S ? ? ? U
 *
 */
static
void __CFStringRearrangeBlocks(uint8_t* buffer,
                               CFIndex numBlocks, CFIndex blockSize,
                               const CFRange* ranges, CFIndex numRanges,
                               CFIndex insertLength)
{
    #define localStackSize 10
    __CFStringBlock localStack[localStackSize];
    __CFStringBlockStack stack = {localStackSize, 0, localStack, false, {0, 0, 0}};
    __CFStringBlock currentBlock = {0, 0, 0};
    CFIndex currentRange = 0;
    CFIndex amountShifted = 0;

    // must have at least 1 range left.

    while (currentRange < numRanges) {
        currentBlock.beginning = (ranges[currentRange].location + ranges[currentRange].length) * blockSize;
        if ((numRanges - currentRange) == 1) {
            // at the end.
            currentBlock.length = numBlocks * blockSize - currentBlock.beginning;
            if (currentBlock.length == 0) {
                break;
            }
        } else {
            currentBlock.length = (ranges[currentRange + 1].location * blockSize) - currentBlock.beginning;
        }

        currentBlock.shift = amountShifted + 
            (insertLength * blockSize) - 
            (ranges[currentRange].length * blockSize);
        amountShifted = currentBlock.shift;
        if (amountShifted <= 0) {
            // Process current item and rest of stack.
            if (currentBlock.shift && currentBlock.length) {
                memmove(
                    &buffer[currentBlock.beginning + currentBlock.shift],
                    &buffer[currentBlock.beginning],
                    currentBlock.length);
            }
            while (stack.count > 0) {
                __CFStringBlockStackPop(&stack, &currentBlock);
                if (currentBlock.shift && currentBlock.length) {
                    memmove(
                        &buffer[currentBlock.beginning + currentBlock.shift],
                        &buffer[currentBlock.beginning],
                        currentBlock.length);
                }
            }
        } else {
            __CFStringBlockStackPush(&stack, &currentBlock);
        }
        currentRange++;
    }

    // No more ranges. If anything is on the stack, process.

    while (stack.count > 0) {
        __CFStringBlockStackPop(&stack, &currentBlock);
        if (currentBlock.shift && currentBlock.length) {
            memmove(
                &buffer[currentBlock.beginning + currentBlock.shift],
                &buffer[currentBlock.beginning],
                currentBlock.length);
        }
    }
    if (stack.stackAllocated) {
        CFAllocatorDeallocate(kCFAllocatorSystemDefault, stack.stack);
    }
}

/* See comments for __CFStringRearrangeBlocks(); this is the same, but the 
 *  string is assembled in another buffer (dstBuffer), so the algorithm is 
 *  much easier. We also take care of the case where the source is not-Unicode
 *  but destination is. The reverse case is not supported.
 */
static
void __CFStringCopyBlocks(const uint8_t* srcBuffer,
                          uint8_t* dstBuffer, CFIndex srcLength,
                          Boolean srcIsUnicode, Boolean dstIsUnicode,
                          const CFRange* ranges, CFIndex numRanges,
                          CFIndex insertLength)
{
    CFIndex srcLocationInBytes = 0;    // in order to avoid multiplying all the time, this is in terms of bytes, not blocks
    CFIndex dstLocationInBytes = 0;    // ditto
    CFIndex srcBlockSize = srcIsUnicode ? sizeof(UniChar) : sizeof(uint8_t);
    CFIndex insertLengthInBytes = insertLength * (dstIsUnicode ? sizeof(UniChar) : sizeof(uint8_t));
    CFIndex rangeIndex = 0;
    CFIndex srcToDstMultiplier = (srcIsUnicode == dstIsUnicode) ? 1 : (sizeof(UniChar) / sizeof(uint8_t));

    // Loop over the ranges, copying the range to be preserved (right before each range)
    while (rangeIndex < numRanges) {
        CFIndex srcLengthInBytes = ranges[rangeIndex].location * srcBlockSize - srcLocationInBytes;    // srcLengthInBytes is in terms of bytes, not blocks; represents length of region to be preserved
        if (srcLengthInBytes > 0) {
            if (srcIsUnicode == dstIsUnicode) {
                memmove(dstBuffer + dstLocationInBytes, srcBuffer + srcLocationInBytes, srcLengthInBytes);
            } else {
                __CFStrConvertBytesToUnicode(srcBuffer + srcLocationInBytes, (UniChar*)(dstBuffer + dstLocationInBytes), srcLengthInBytes);
            }
        }
        srcLocationInBytes += srcLengthInBytes + ranges[rangeIndex].length * srcBlockSize;    // Skip over the just-copied and to-be-deleted stuff
        dstLocationInBytes += srcLengthInBytes * srcToDstMultiplier + insertLengthInBytes;
        rangeIndex++;
    }

    // Do last range (the one beyond last range)
    if (srcLocationInBytes < srcLength * srcBlockSize) {
        if (srcIsUnicode == dstIsUnicode) {
            memmove(dstBuffer + dstLocationInBytes, srcBuffer + srcLocationInBytes, srcLength * srcBlockSize - srcLocationInBytes);
        } else {
            __CFStrConvertBytesToUnicode(srcBuffer + srcLocationInBytes, (UniChar*)(dstBuffer + dstLocationInBytes), srcLength * srcBlockSize - srcLocationInBytes);
        }
    }
}

CF_INLINE CFIndex __CFStrNewCapacity(CFMutableStringRef str, CFIndex reqCapacity, CFIndex capacity, Boolean leaveExtraRoom, CFIndex charSize) {

	//TODO refactor this mess

    /* Basic algorithm is to shrink memory when capacity is SHRINKFACTOR
     *  times the required capacity or to allocate memory when the capacity
     *  is less than GROWFACTOR times the required capacity.
     * Additional complications are applied in the following order:
     * - desiredCapacity, which is the minimum (except initially things can be at zero)
     * - rounding up to factor of 8
     * - compressing (to fit the number if 16 bits), which effectively rounds up to factor of 256
     * - we need to make sure GROWFACTOR computation doesn't suffer from overflow issues on 32-bit,
     *    hence the casting to unsigned. Normally for required capacity of C bytes, the allocated
     *    space is (3C+1)/2. If C > ULONG_MAX/3, we instead simply return LONG_MAX
     */
    #define SHRINKFACTOR(c) (c / 2)
    #define GROWFACTOR(c) \
    (((c) >= (ULONG_MAX / 3UL)) ? \
     _CFMax(LONG_MAX - 4095, (c)) : \
     (((CFIndex)(c) * 3 + 1) / 2))

    if (capacity != 0 || reqCapacity != 0) {    /* If initially zero, and space not needed, leave it at that... */
        if ((capacity < reqCapacity) ||        /* We definitely need the room... */
            (!__CFStrCapacityProvidedExternally(str) &&     /* Assuming we control the capacity... */
             ((reqCapacity < SHRINKFACTOR(capacity)) ||   /* ...we have too much room! */
              (!leaveExtraRoom && (reqCapacity < capacity)))))         /* ...we need to eliminate the extra space... */
        {
            CFIndex newCapacity = leaveExtraRoom ? GROWFACTOR(reqCapacity) : reqCapacity;    /* Grow by 3/2 if extra room is desired */
            CFIndex desiredCapacity = __CFStrDesiredCapacity(str) * charSize;
            if (newCapacity < desiredCapacity) {    /* If less than desired, bump up to desired */
                newCapacity = desiredCapacity;
            } else if (__CFStrIsFixed(str)) {        /* Otherwise, if fixed, no need to go above the desired (fixed) capacity */
                newCapacity = _CFMax(desiredCapacity, reqCapacity);    /* !!! So, fixed is not really fixed, but "tight" */
            }
            if (__CFStrHasContentsAllocator(str)) { /* Also apply any preferred size from the allocator; should we do something for  */
                newCapacity = CFAllocatorGetPreferredSizeForSize(__CFStrContentsAllocator(str), newCapacity, 0);
            } else {
                newCapacity = malloc_good_size(newCapacity);
            }
            return newCapacity; // If packing: __CFStrUnpackNumber(__CFStrPackNumber(newCapacity));
        }
    }
    return capacity;
}

static void __CFStringHandleOutOfMemory(CFTypeRef obj) {
    CFReportRuntimeError(
        kCFRuntimeErrorOutOfMemory, 
        CFSTR("Failed to allocate memory for NS/CFString."));
}

static Boolean CFStrIsUnicode(CFStringRef str) {
    CF_OBJC_FUNCDISPATCH(Boolean, str, "_encodingCantBeStoredInEightBitCFString");
    return __CFStrIsUnicode(str);
}

// Can pass in NSString as replacement string
// Call with numRanges > 0, and incrementing ranges
static void __CFStringReplaceMultiple(CFMutableStringRef str, CFRange* ranges, CFIndex numRanges, CFStringRef replacement) {
    int cnt;
    CFStringRef copy = NULL;
    if (replacement == str) {
        copy = replacement = CFStringCreateCopy(kCFAllocatorSystemDefault, replacement);                       // Very special and hopefully rare case
    }
    CFIndex replacementLength = CFStringGetLength(replacement);

    __CFStringChangeSizeMultiple(str, ranges, numRanges, replacementLength, (replacementLength > 0) && CFStrIsUnicode(replacement));

    if (__CFStrIsUnicode(str)) {
        UniChar* contents = (UniChar*)__CFStrContents(str);
        UniChar* firstReplacement = contents + ranges[0].location;
        // Extract the replacementString into the first location, then copy from there
        CFStringGetCharacters(replacement, CFRangeMake(0, replacementLength), firstReplacement);
        for (cnt = 1; cnt < numRanges; cnt++) {
            // The ranges are in terms of the original string; so offset by the change in length due to insertion
            contents += replacementLength - ranges[cnt - 1].length;
            memmove(contents + ranges[cnt].location, firstReplacement, replacementLength * sizeof(UniChar));
        }
    } else {
        uint8_t* contents = (uint8_t*)__CFStrContents(str);
        uint8_t* firstReplacement = contents + ranges[0].location + __CFStrSkipAnyLengthByte(str);
        // Extract the replacementString into the first location, then copy from there
        CFStringGetBytes(replacement, CFRangeMake(0, replacementLength), __CFStringGetEightBitStringEncoding(), 0, false, firstReplacement, replacementLength, NULL);
        contents += __CFStrSkipAnyLengthByte(str);    // Now contents will simply track the location to insert next string into
        for (cnt = 1; cnt < numRanges; cnt++) {
            // The ranges are in terms of the original string; so offset by the change in length due to insertion
            contents += replacementLength - ranges[cnt - 1].length;
            memmove(contents + ranges[cnt].location, firstReplacement, replacementLength);
        }
    }
    if (copy) {
        CFRelease(copy);
    }
}

// Can pass in NSString as replacement string
CF_INLINE void __CFStringReplace(CFMutableStringRef str, CFRange range, CFStringRef replacement) {
    CFStringRef copy = NULL;
    if (replacement == str) {
        copy = replacement = (CFStringRef)CFStringCreateCopy(kCFAllocatorSystemDefault, replacement);                       // Very special and hopefully rare case
    }
    CFIndex replacementLength = CFStringGetLength(replacement);

    __CFStringChangeSize(str, range, replacementLength, (replacementLength > 0) && CFStrIsUnicode(replacement));

    if (__CFStrIsUnicode(str)) {
        UniChar* contents = (UniChar*)__CFStrContents(str);
        CFStringGetCharacters(replacement, CFRangeMake(0, replacementLength), contents + range.location);
    } else {
        uint8_t* contents = (uint8_t*)__CFStrContents(str);
        CFStringGetBytes(replacement, CFRangeMake(0, replacementLength), __CFStringGetEightBitStringEncoding(), 0, false, contents + range.location + __CFStrSkipAnyLengthByte(str), replacementLength, NULL);
    }

    if (copy) {
        CFRelease(copy);
    }
}


CF_INLINE uint32_t __CFGetUTF16Length(const UTF32Char* characters, uint32_t utf32Length) {
    const UTF32Char* limit = characters + utf32Length;
    uint32_t length = 0;

    while (characters < limit) {
        length += (*(characters++) > 0xFFFF ? 2 : 1);
    }

    return length;
}

CF_INLINE void __CFFillInUTF16(const UTF32Char* characters, UTF16Char* dst, uint32_t utf32Length) {
    const UTF32Char* limit = characters + utf32Length;
    UTF32Char currentChar;

    while (characters < limit) {
        currentChar = *(characters++);
        if (currentChar > 0xFFFF) {
            currentChar -= 0x10000;
            *(dst++) = (UTF16Char)((currentChar >> 10) + 0xD800UL);
            *(dst++) = (UTF16Char)((currentChar & 0x3FF) + 0xDC00UL);
        } else {
            *(dst++) = currentChar;
        }
    }
}

CF_INLINE CFMutableStringRef __CFStringCreateMutableFunnel(CFAllocatorRef alloc, CFIndex maxLength, UInt32 additionalInfoBits) {
    /* If client does not provide a minimum capacity
     */
    #define DEFAULTMINCAPACITY 32

    CFMutableStringRef str;
    Boolean hasExternalContentsAllocator = (additionalInfoBits & __kCFHasContentsAllocator) ? true : false;

    if (alloc == NULL) {
        alloc = CFAllocatorGetDefault();
    }

    // Note that if there is an externalContentsAllocator, then we also have the storage for the string allocator...
    str = (CFMutableStringRef)_CFRuntimeCreateInstance(alloc, _kCFStringTypeID, sizeof(struct __notInlineMutable) - (hasExternalContentsAllocator ? 0 : sizeof(CFAllocatorRef)), NULL);
    if (str) {

        __CFStrSetInfoBits(str, __kCFIsMutable | additionalInfoBits);
        str->variants.notInlineMutable.buffer = NULL;
        __CFStrSetExplicitLength(str, 0);
        str->variants.notInlineMutable.hasGap = str->variants.notInlineMutable.isFixedCapacity = str->variants.notInlineMutable.isExternalMutable = str->variants.notInlineMutable.capacityProvidedExternally = 0;
        if (maxLength != 0) {
            __CFStrSetIsFixed(str);
        }
        __CFStrSetDesiredCapacity(str, (maxLength == 0) ? DEFAULTMINCAPACITY : maxLength);
        __CFStrSetCapacity(str, 0);
    }
    _CFRuntimeSetMutableObjcClass(str);
    return str;
}


///////////////////////////////////////////////////////////////////// internal

/* Reallocates the backing store of the string to accomodate the new length.
 * Space is reserved or characters are deleted as indicated by insertLength 
 *  and the ranges in deleteRanges. The length is updated to reflect the new state.
 * Will also maintain a length byte and a null byte in 8-bit strings. If length 
 *  cannot fit in length byte, the space will still be reserved, but will be 0.
 *  (Hence the reason the length byte should never be looked at as length unless 
 *  there is no explicit length.)
 */
CF_INTERNAL void __CFStringChangeSizeMultiple(CFMutableStringRef str, const CFRange* deleteRanges, CFIndex numDeleteRanges, CFIndex insertLength, Boolean makeUnicode) {
    const uint8_t* curContents = (uint8_t*)__CFStrContents(str);
    CFIndex curLength = curContents ? __CFStrLength2(str, curContents) : 0;
    CFIndex newLength;

    // Compute new length of the string
    if (numDeleteRanges == 1) {
        newLength = curLength + insertLength - deleteRanges[0].length;
    } else {
        CFIndex cnt;
        newLength = curLength + insertLength * numDeleteRanges;
        for (cnt = 0; cnt < numDeleteRanges; cnt++) {
            newLength -= deleteRanges[cnt].length;
        }
    }

    CF_VALIDATE_ARG(
        !__CFStrIsFixed(str) || (newLength <= __CFStrDesiredCapacity(str)),
        "length %d too large", newLength);

    if (newLength == 0) {
        // An somewhat optimized code-path for this special case, 
        //  with the following implicit values:
        //  newIsUnicode = false
        //  useLengthAndNullBytes = false
        //  newCharSize = sizeof(uint8_t)
        // If the newCapacity happens to be the same as the old, 
        //  we don't free the buffer; otherwise we just free it totally
        //  instead of doing a potentially useless reallocation (as the 
        //  needed capacity later might turn out to be different anyway).
        CFIndex curCapacity = __CFStrCapacity(str);
        CFIndex newCapacity = __CFStrNewCapacity(str, 0, curCapacity, true, sizeof(uint8_t));
        if (newCapacity != curCapacity) {
            // If we're reallocing anyway (larger or smaller - larger could 
            //  happen if desired capacity was changed in the meantime), 
            //  let's just free it all.
            if (curContents) {
                __CFStrDeallocateMutableContents(str, (uint8_t*)curContents);
            }
            __CFStrSetContentPtr(str, NULL);
            __CFStrSetCapacity(str, 0);
            __CFStrClearCapacityProvidedExternally(str);
            __CFStrClearHasLengthAndNullBytes(str);
            if (!__CFStrIsExternalMutable(str)) {
                __CFStrClearUnicode(str); // External mutable implies Unicode
            }
        } else {
            if (!__CFStrIsExternalMutable(str)) {
                __CFStrClearUnicode(str);
                if (curCapacity >= 2) { // If there's room
                    __CFStrSetHasLengthAndNullBytes(str);
                    ((uint8_t*)curContents)[0] = 0;
                    ((uint8_t*)curContents)[1] = 0;
                } else {
                    __CFStrClearHasLengthAndNullBytes(str);
                }
            }
        }
        __CFStrSetExplicitLength(str, 0);

    } else {
        /* This following code assumes newLength > 0 */

        Boolean oldIsUnicode = __CFStrIsUnicode(str);
        Boolean newIsUnicode = makeUnicode || oldIsUnicode || __CFStrIsExternalMutable(str);
        CFIndex newCharSize = newIsUnicode ? sizeof(UniChar) : sizeof(uint8_t);
        Boolean useLengthAndNullBytes = !newIsUnicode;

        CFIndex numExtraBytes = useLengthAndNullBytes ? 2 : 0; // 2 extra bytes to keep the length byte & null...
        CFIndex curCapacity = __CFStrCapacity(str);
        CFIndex newCapacity = __CFStrNewCapacity(str,
            newLength * newCharSize + numExtraBytes,
            curCapacity,
            true,
            newCharSize);

        uint8_t* newContents;

        // We alloc new buffer if oldIsUnicode != newIsUnicode 
        //  because the contents have to be copied.
        Boolean allocNewBuffer =
            (newCapacity != curCapacity) ||
            (curLength > 0 && !oldIsUnicode && newIsUnicode);

        if (allocNewBuffer) {
            newContents = (uint8_t*)__CFStrAllocateMutableContents(str, newCapacity);
            if (!newContents) {
                // Try allocating without extra room...
                newCapacity = __CFStrNewCapacity(
                    str,
                    newLength * newCharSize + numExtraBytes,
                    curCapacity,
                    false,
                    newCharSize);
                newContents = (uint8_t*)__CFStrAllocateMutableContents(str, newCapacity);
                if (!newContents) {
                    __CFStringHandleOutOfMemory(str);
                    // Ideally control doesn't come here at all since we expect the 
                    //  above call to raise an exception.
                    // If control comes here, there isn't much we can do.
                }
            }
        } else {
            newContents = (uint8_t*)curContents;
        }

        Boolean hasLengthAndNullBytes = __CFStrHasLengthByte(str);

        CF_ASSERT_XXX(hasLengthAndNullBytes == __CFStrHasNullByte(str), "Invalid state in 8-bit string");

        if (hasLengthAndNullBytes) {
            curContents++;
        }
        if (useLengthAndNullBytes) {
            newContents++;
        }

        if (curContents) {
            if (oldIsUnicode == newIsUnicode) {
                if (newContents == curContents) {
                    __CFStringRearrangeBlocks(newContents, curLength, newCharSize, deleteRanges, numDeleteRanges, insertLength);
                } else {
                    __CFStringCopyBlocks(curContents, newContents, curLength, oldIsUnicode, newIsUnicode, deleteRanges, numDeleteRanges, insertLength);
                }
            } else if (newIsUnicode) {    /* this implies we have a new buffer */
                __CFStringCopyBlocks(curContents, newContents, curLength, oldIsUnicode, newIsUnicode, deleteRanges, numDeleteRanges, insertLength);
            }
            if (hasLengthAndNullBytes) {
                curContents--;                           /* Undo the damage from above */
            }
            if (allocNewBuffer && __CFStrFreeContentsWhenDone(str)) {
                __CFStrDeallocateMutableContents(str, (void*)curContents);
            }
        }

        if (!newIsUnicode) {
            if (useLengthAndNullBytes) {
                newContents[newLength] = 0;    /* Always have null byte, if not unicode */
                newContents--;    /* Undo the damage from above */
                newContents[0] = __CFCanUseLengthByte(newLength) ? (uint8_t)newLength : 0;
                if (!hasLengthAndNullBytes) {
                    __CFStrSetHasLengthAndNullBytes(str);
                }
            } else {
                if (hasLengthAndNullBytes) {
                    __CFStrClearHasLengthAndNullBytes(str);
                }
            }
            if (oldIsUnicode) {
                __CFStrClearUnicode(str);
            }
        } else {    // New is unicode...
            if (!oldIsUnicode) {
                __CFStrSetUnicode(str);
            }
            if (hasLengthAndNullBytes) {
                __CFStrClearHasLengthAndNullBytes(str);
            }
        }
        __CFStrSetExplicitLength(str, newLength);

        if (allocNewBuffer) {
            __CFStrSetCapacity(str, newCapacity);
            __CFStrClearCapacityProvidedExternally(str);
            __CFStrSetContentPtr(str, newContents);
        }
    }
}

/* Same as __CFStringChangeSizeMultiple, but takes one range (very common case).
 * TODO: make inline?
 */
CF_INTERNAL void __CFStringChangeSize(CFMutableStringRef str, CFRange range, CFIndex insertLength, Boolean makeUnicode) {
    __CFStringChangeSizeMultiple(str, &range, 1, insertLength, makeUnicode);
}

CF_INTERNAL void __CFStringAppendBytes(CFMutableStringRef str, const char* cStr, CFIndex appendedLength, CFStringEncoding encoding) {
    Boolean appendedIsUnicode = false;
    Boolean freeCStrWhenDone = false;
    Boolean demoteAppendedUnicode = false;
    CFVarWidthCharBuffer vBuf;

    CF_VALIDATE_NONNEGATIVE_ARG(appendedLength);

    if (encoding == kCFStringEncodingASCII || encoding == __CFStringGetEightBitStringEncoding()) {
        // appendedLength now denotes length in UniChars
    } else if (encoding == kCFStringEncodingUnicode) {
        UniChar* chars = (UniChar*)cStr;
        CFIndex idx, length = appendedLength / sizeof(UniChar);
        bool isASCII = true;
        for (idx = 0; isASCII && idx < length; idx++) {
            isASCII = (chars[idx] < 0x80);
        }
        if (!isASCII) {
            appendedIsUnicode = true;
        } else {
            demoteAppendedUnicode = true;
        }
        appendedLength = length;
    } else {
        Boolean usingPassedInMemory = false;

        vBuf.allocator = CFAllocatorGetDefault(); // We don't want to use client's allocator for temp stuff
        vBuf.chars.unicode = NULL;    // This will cause the decode function to allocate memory if necessary

        if (!__CFStringDecodeByteStream((const uint8_t*)cStr, appendedLength, encoding, __CFStrIsUnicode(str), &vBuf, &usingPassedInMemory, 0)) {
            CF_GENERIC_ERROR("Supplied bytes could not be converted specified encoding %d", encoding);
            return;
        }

        // If not ASCII, appendedLength now denotes length in UniChars
        appendedLength = vBuf.numChars;
        appendedIsUnicode = !vBuf.isASCII;
        cStr = (const char*)vBuf.chars.ascii;
        freeCStrWhenDone = !usingPassedInMemory && vBuf.shouldFreeChars;
    }

    if (CF_IS_OBJC(str)) {
        if (!appendedIsUnicode && !demoteAppendedUnicode) {
            CF_OBJC_VOID_FUNCDISPATCH(str, "_cfAppendCString:length:", cStr, appendedLength);
        } else {
            CF_OBJC_VOID_FUNCDISPATCH(str, "appendCharacters:length:", cStr, appendedLength);
        }
    } else {
        CFIndex strLength;
        CF_VALIDATE_MUTABLESTRING_ARG(str);
        strLength = __CFStrLength(str);

        __CFStringChangeSize(str, CFRangeMake(strLength, 0), appendedLength, appendedIsUnicode || __CFStrIsUnicode(str));

        if (__CFStrIsUnicode(str)) {
            UniChar* contents = (UniChar*)__CFStrContents(str);
            if (appendedIsUnicode) {
                memmove(contents + strLength, cStr, appendedLength * sizeof(UniChar));
            } else {
                __CFStrConvertBytesToUnicode((const uint8_t*)cStr, contents + strLength, appendedLength);
            }
        } else {
            if (demoteAppendedUnicode) {
                UniChar* chars = (UniChar*)cStr;
                CFIndex idx;
                uint8_t* contents = (uint8_t*)__CFStrContents(str) + strLength + __CFStrSkipAnyLengthByte(str);
                for (idx = 0; idx < appendedLength; idx++) {
                    contents[idx] = (uint8_t)chars[idx];
                }
            } else {
                uint8_t* contents = (uint8_t*)__CFStrContents(str);
                memmove(contents + strLength + __CFStrSkipAnyLengthByte(str), cStr, appendedLength);
            }
        }
    }

    if (freeCStrWhenDone) {
        CFAllocatorDeallocate(CFAllocatorGetDefault(), (void*)cStr);
    }
}

CF_INTERNAL void _CFStrSetDesiredCapacity(CFMutableStringRef str, CFIndex len) {
    CF_VALIDATE_MUTABLESTRING_ARG(str);
    __CFStrSetDesiredCapacity(str, len);
}

///////////////////////////////////////////////////////////////////// public

CFMutableStringRef CFStringCreateMutableCopy(CFAllocatorRef alloc, CFIndex maxLength, CFStringRef string) {
    //  CF_OBJC_FUNCDISPATCH(CFMutableStringRef, string, "mutableCopy");
    CF_VALIDATE_STRING_ARG(string);

    CFMutableStringRef mutableString = CFStringCreateMutable(alloc, maxLength);
    __CFStringReplace(mutableString, CFRangeMake(0, 0), string);

    return mutableString;
}

void CFStringSetExternalCharactersNoCopy(CFMutableStringRef string, UniChar* chars, CFIndex length, CFIndex capacity) {
    CF_VALIDATE_NONNEGATIVE_ARG(length);
    CF_VALIDATE_MUTABLESTRING_ARG(string);
    CF_VALIDATE_ARG(__CFStrIsExternalMutable(string),
        "string %p not external mutable", string)
    CF_VALIDATE_ARG((length <= capacity) && ((capacity == 0) || ((capacity > 0) && chars)), "Invalid args: characters %p length %d capacity %d", chars, length, capacity);

    __CFStrSetContentPtr(string, chars);
    __CFStrSetExplicitLength(string, length);
    __CFStrSetCapacity(string, capacity * sizeof(UniChar));
    __CFStrSetCapacityProvidedExternally(string);
}

void CFStringInsert(CFMutableStringRef str, CFIndex idx, CFStringRef insertedStr) {
    CF_OBJC_VOID_FUNCDISPATCH(str, "insertString:atIndex:", insertedStr, idx);
    
    CF_VALIDATE_MUTABLESTRING_ARG(str);
    CF_VALIDATE_STRING_INDEX_ARG(str, idx);
    
    __CFStringReplace(str, CFRangeMake(idx, 0), insertedStr);
}

void CFStringDelete(CFMutableStringRef str, CFRange range) {
    CF_OBJC_VOID_FUNCDISPATCH(str, "deleteCharactersInRange:", range);
    
    CF_VALIDATE_MUTABLESTRING_ARG(str);
    CF_VALIDATE_STRING_RANGE_ARG(str, range);

    __CFStringChangeSize(str, range, 0, false);
}

void CFStringReplace(CFMutableStringRef str, CFRange range, CFStringRef replacement) {
    CF_OBJC_VOID_FUNCDISPATCH(str, "replaceCharactersInRange:withString:", range, replacement);
    
    CF_VALIDATE_MUTABLESTRING_ARG(str);
    CF_VALIDATE_STRING_RANGE_ARG(str, range);

    __CFStringReplace(str, range, replacement);
}

void CFStringReplaceAll(CFMutableStringRef str, CFStringRef replacement) {
    CF_OBJC_VOID_FUNCDISPATCH(str, "setString:", replacement);
    CF_VALIDATE_MUTABLESTRING_ARG(str);
    __CFStringReplace(str, CFRangeMake(0, __CFStrLength(str)), replacement);
}

void CFStringAppend(CFMutableStringRef str, CFStringRef appended) {
    CF_OBJC_VOID_FUNCDISPATCH(str, "appendString:", appended);
    CF_VALIDATE_MUTABLESTRING_ARG(str);
    __CFStringReplace(str, CFRangeMake(__CFStrLength(str), 0), appended);
}

void CFStringAppendCharacters(CFMutableStringRef str, const UniChar* chars, CFIndex appendedLength) {
    CFIndex strLength, idx;

    CF_VALIDATE_NONNEGATIVE_ARG(appendedLength);

    CF_OBJC_VOID_FUNCDISPATCH(str, "appendCharacters:length:", chars, appendedLength);

    CF_VALIDATE_MUTABLESTRING_ARG(str);

    strLength = __CFStrLength(str);
    if (__CFStrIsUnicode(str)) {
        __CFStringChangeSize(str, CFRangeMake(strLength, 0), appendedLength, true);
        memmove((UniChar*)__CFStrContents(str) + strLength, chars, appendedLength * sizeof(UniChar));
    } else {
        uint8_t* contents;
        bool isASCII = true;
        for (idx = 0; isASCII && idx < appendedLength; idx++) {
            isASCII = (chars[idx] < 0x80);
        }
        __CFStringChangeSize(str, CFRangeMake(strLength, 0), appendedLength, !isASCII);
        if (!isASCII) {
            memmove((UniChar*)__CFStrContents(str) + strLength, chars, appendedLength * sizeof(UniChar));
        } else {
            contents = (uint8_t*)__CFStrContents(str) + strLength + __CFStrSkipAnyLengthByte(str);
            for (idx = 0; idx < appendedLength; idx++) {
                contents[idx] = (uint8_t)chars[idx];
            }
        }
    }
}

void CFStringAppendPascalString(CFMutableStringRef str, ConstStringPtr pStr, CFStringEncoding encoding) {
    __CFStringAppendBytes(str, (const char*)(pStr + 1), (CFIndex) * pStr, encoding);
}

void CFStringAppendCString(CFMutableStringRef str, const char* cStr, CFStringEncoding encoding) {
    __CFStringAppendBytes(str, cStr, strlen(cStr), encoding);
}

CFIndex CFStringFindAndReplace(CFMutableStringRef string, CFStringRef stringToFind, CFStringRef replacementString, CFRange rangeToSearch, CFOptionFlags compareOptions) {
    CFRange foundRange;
    Boolean backwards = ((compareOptions & kCFCompareBackwards) != 0);
    UInt32 endIndex = rangeToSearch.location + rangeToSearch.length;
#define MAX_RANGES_ON_STACK (1000 / sizeof(CFRange))
    CFRange rangeBuffer[MAX_RANGES_ON_STACK];    // Used to avoid allocating memory
    CFRange* ranges = rangeBuffer;
    CFIndex foundCount = 0;
    CFIndex capacity = MAX_RANGES_ON_STACK;

    CF_VALIDATE_MUTABLESTRING_ARG(string);
    CF_VALIDATE_STRING_RANGE_ARG(string, rangeToSearch);

    // Note: This code is very similar to the one in CFStringCreateArrayWithFindResults().
    while ((rangeToSearch.length > 0) && CFStringFindWithOptions(string, stringToFind, rangeToSearch, compareOptions, &foundRange)) {
        // Determine the next range
        if (backwards) {
            rangeToSearch.length = foundRange.location - rangeToSearch.location;
        } else {
            rangeToSearch.location = foundRange.location + foundRange.length;
            rangeToSearch.length = endIndex - rangeToSearch.location;
        }

        // If necessary, grow the array
        if (foundCount >= capacity) {
            bool firstAlloc = (ranges == rangeBuffer) ? true : false;
            capacity = (capacity + 4) * 2;
            // Note that reallocate with NULL previous pointer is same as allocate
            ranges = (CFRange*)CFAllocatorReallocate(kCFAllocatorSystemDefault, firstAlloc ? NULL : ranges, capacity * sizeof(CFRange), 0);
            if (firstAlloc) {
                memmove(ranges, rangeBuffer, MAX_RANGES_ON_STACK * sizeof(CFRange));
            }
        }
        ranges[foundCount] = foundRange;
        foundCount++;
    }

    if (foundCount > 0) {
        if (backwards) {    // Reorder the ranges to be incrementing (better to do this here, then to check other places)
            int head = 0;
            int tail = foundCount - 1;
            while (head < tail) {
                CFRange temp = ranges[head];
                ranges[head] = ranges[tail];
                ranges[tail] = temp;
                head++;
                tail--;
            }
        }
        __CFStringReplaceMultiple(string, ranges, foundCount, replacementString);
        if (ranges != rangeBuffer) {
            CFAllocatorDeallocate(kCFAllocatorSystemDefault, ranges);
        }
    }

    return foundCount;
}

void CFStringPad(CFMutableStringRef string, CFStringRef padString, CFIndex length, CFIndex indexIntoPad) {
    CFIndex originalLength;

    CF_VALIDATE_NONNEGATIVE_ARG(length);
    CF_VALIDATE_NONNEGATIVE_ARG(indexIntoPad);

    CF_OBJC_VOID_FUNCDISPATCH(string, "_cfPad:length:padIndex:", padString, length, indexIntoPad);

    CF_VALIDATE_MUTABLESTRING_ARG(string);

    originalLength = __CFStrLength(string);
    if (length < originalLength) {
        __CFStringChangeSize(string, CFRangeMake(length, originalLength - length), 0, false);
    } else if (originalLength < length) {
        uint8_t* contents;
        Boolean isUnicode;
        CFIndex charSize;
        CFIndex padStringLength;
        CFIndex padLength;
        CFIndex padRemaining = length - originalLength;

        if (CF_IS_OBJC(padString)) {
            padStringLength = CFStringGetLength(padString);
            isUnicode = true;    /* !!! Bad for now */
        } else {
            CF_VALIDATE_STRING_ARG(padString);
            padStringLength = __CFStrLength(padString);
            isUnicode = __CFStrIsUnicode(string) || __CFStrIsUnicode(padString);
        }

        charSize = isUnicode ? sizeof(UniChar) : sizeof(uint8_t);

        __CFStringChangeSize(string, CFRangeMake(originalLength, 0), padRemaining, isUnicode);

        contents = (uint8_t*)__CFStrContents(string) + charSize * originalLength + __CFStrSkipAnyLengthByte(string);
        padLength = padStringLength - indexIntoPad;
        padLength = padRemaining < padLength ? padRemaining : padLength;

        while (padRemaining > 0) {
            if (isUnicode) {
                CFStringGetCharacters(padString, CFRangeMake(indexIntoPad, padLength), (UniChar*)contents);
            } else {
                CFStringGetBytes(padString, CFRangeMake(indexIntoPad, padLength), __CFStringGetEightBitStringEncoding(), 0, false, contents, padRemaining * charSize, NULL);
            }
            contents += padLength * charSize;
            padRemaining -= padLength;
            indexIntoPad = 0;
            padLength = padRemaining < padLength ? padRemaining : padStringLength;
        }
    }
}

void CFStringTrim(CFMutableStringRef string, CFStringRef trimString) {
    CFRange range;
    CFIndex newStartIndex;
    CFIndex length;

    CF_OBJC_VOID_FUNCDISPATCH(string, "_cfTrim:", trimString);

    CF_VALIDATE_MUTABLESTRING_ARG(string);
    CF_VALIDATE_STRING_ARG(trimString);

    newStartIndex = 0;
    length = __CFStrLength(string);

    while (CFStringFindWithOptions(string, trimString, CFRangeMake(newStartIndex, length - newStartIndex), kCFCompareAnchored, &range)) {
        newStartIndex = range.location + range.length;
    }

    if (newStartIndex < length) {
        CFIndex charSize = __CFStrIsUnicode(string) ? sizeof(UniChar) : sizeof(uint8_t);
        uint8_t* contents = (uint8_t*)__CFStrContents(string) + __CFStrSkipAnyLengthByte(string);

        length -= newStartIndex;
        if (__CFStrLength(trimString) < length) {
            while (CFStringFindWithOptions(string, trimString, CFRangeMake(newStartIndex, length), kCFCompareAnchored | kCFCompareBackwards, &range)) {
                length = range.location - newStartIndex;
            }
        }
        memmove(contents, contents + newStartIndex * charSize, length * charSize);
        __CFStringChangeSize(string, CFRangeMake(length, __CFStrLength(string) - length), 0, false);
    } else { // Only trimString in string, trim all
        __CFStringChangeSize(string, CFRangeMake(0, length), 0, false);
    }
}

void CFStringTrimWhitespace(CFMutableStringRef string) {
    CFIndex newStartIndex;
    CFIndex length;
    CFStringInlineBuffer buffer;

    CF_OBJC_VOID_FUNCDISPATCH(string, "_cfTrimWS");

    CF_VALIDATE_MUTABLESTRING_ARG(string);

    newStartIndex = 0;
    length = __CFStrLength(string);

    CFStringInitInlineBuffer(string, &buffer, CFRangeMake(0, length));
    CFIndex buffer_idx = 0;

    while (buffer_idx < length && _CFUniCharIsMemberOf(CFStringGetCharacterFromInlineBuffer(&buffer, buffer_idx), kCFUniCharWhitespaceAndNewlineCharacterSet)) {
        buffer_idx++;
    }
    newStartIndex = buffer_idx;

    if (newStartIndex < length) {
        uint8_t* contents = (uint8_t*)__CFStrContents(string) + __CFStrSkipAnyLengthByte(string);
        CFIndex charSize = (__CFStrIsUnicode(string) ? sizeof(UniChar) : sizeof(uint8_t));

        buffer_idx = length - 1;
        while (0 <= buffer_idx && _CFUniCharIsMemberOf(CFStringGetCharacterFromInlineBuffer(&buffer, buffer_idx), kCFUniCharWhitespaceAndNewlineCharacterSet)) {
            buffer_idx--;
        }
        length = buffer_idx - newStartIndex + 1;

        memmove(contents, contents + newStartIndex * charSize, length * charSize);
        __CFStringChangeSize(string, CFRangeMake(length, __CFStrLength(string) - length), 0, false);
    } else { // Whitespace only string
        __CFStringChangeSize(string, CFRangeMake(0, length), 0, false);
    }
}

void CFStringLowercase(CFMutableStringRef string, CFLocaleRef locale) {
    CFIndex currentIndex = 0;
    CFIndex length;
    const uint8_t* langCode;
    Boolean isEightBit = __CFStrIsEightBit(string);

    CF_OBJC_VOID_FUNCDISPATCH(string, "_cfLowercase:", locale);

    CF_VALIDATE_MUTABLESTRING_ARG(string);

    length = __CFStrLength(string);

    langCode = (const uint8_t*)(_CFCanUseLocale(locale) ? _CFStrGetLanguageIdentifierForLocale(locale) : NULL);

    if (!langCode && isEightBit) {
        uint8_t* contents = (uint8_t*)__CFStrContents(string) + __CFStrSkipAnyLengthByte(string);
        for (; currentIndex < length; currentIndex++) {
            if (contents[currentIndex] >= 'A' && contents[currentIndex] <= 'Z') {
                contents[currentIndex] += 'a' - 'A';
            } else if (contents[currentIndex] > 127) {
                break;
            }
        }
    }

    if (currentIndex < length) {
        UTF16Char* contents;
        UniChar mappedCharacters[MAX_CASE_MAPPING_BUF];
        CFIndex mappedLength;
        UTF32Char currentChar;
        UInt32 flags = 0;

        if (isEightBit) {
            __CFStringChangeSize(string, CFRangeMake(0, 0), 0, true);
        }

        contents = (UniChar*)__CFStrContents(string);

        for (; currentIndex < length; currentIndex++) {

            if (_CFUniCharIsSurrogateHighCharacter(contents[currentIndex]) && (currentIndex + 1 < length) && _CFUniCharIsSurrogateLowCharacter(contents[currentIndex + 1])) {
                currentChar = _CFUniCharGetLongCharacterForSurrogatePair(contents[currentIndex], contents[currentIndex + 1]);
            } else {
                currentChar = contents[currentIndex];
            }
            flags = ((langCode || (currentChar == 0x03A3)) ? _CFUniCharGetConditionalCaseMappingFlags(currentChar, contents, currentIndex, length, kCFUniCharToLowercase, langCode, flags) : 0);

            mappedLength = _CFUniCharMapCaseTo(currentChar, mappedCharacters, MAX_CASE_MAPPING_BUF, kCFUniCharToLowercase, flags, langCode);
            if (mappedLength > 0) {
                contents[currentIndex] = *mappedCharacters;
            }

            if (currentChar > 0xFFFF) { // Non-BMP char
                switch (mappedLength) {
                    case 0:
                        __CFStringChangeSize(string, CFRangeMake(currentIndex, 2), 0, true);
                        contents = (UniChar*)__CFStrContents(string);
                        length -= 2;
                        break;

                    case 1:
                        __CFStringChangeSize(string, CFRangeMake(currentIndex + 1, 1), 0, true);
                        contents = (UniChar*)__CFStrContents(string);
                        --length;
                        break;

                    case 2:
                        contents[++currentIndex] = mappedCharacters[1];
                        break;

                    default:
                        --mappedLength; // Skip the current char
                        __CFStringChangeSize(string, CFRangeMake(currentIndex + 1, 0), mappedLength - 1, true);
                        contents = (UniChar*)__CFStrContents(string);
                        memmove(contents + currentIndex + 1, mappedCharacters + 1, mappedLength * sizeof(UniChar));
                        length += (mappedLength - 1);
                        currentIndex += mappedLength;
                        break;
                }
            } else if (mappedLength == 0) {
                __CFStringChangeSize(string, CFRangeMake(currentIndex, 1), 0, true);
                contents = (UniChar*)__CFStrContents(string);
                --length;
            } else if (mappedLength > 1) {
                --mappedLength; // Skip the current char
                __CFStringChangeSize(string, CFRangeMake(currentIndex + 1, 0), mappedLength, true);
                contents = (UniChar*)__CFStrContents(string);
                memmove(contents + currentIndex + 1, mappedCharacters + 1, mappedLength * sizeof(UniChar));
                length += mappedLength;
                currentIndex += mappedLength;
            }
        }
    }
}

void CFStringUppercase(CFMutableStringRef string, CFLocaleRef locale) {
    CFIndex currentIndex = 0;
    CFIndex length;
    const uint8_t* langCode;
    Boolean isEightBit = __CFStrIsEightBit(string);

    CF_OBJC_VOID_FUNCDISPATCH(string, "_cfUppercase:", locale);

    CF_VALIDATE_MUTABLESTRING_ARG(string);

    length = __CFStrLength(string);

    langCode = (const uint8_t*)(_CFCanUseLocale(locale) ? _CFStrGetLanguageIdentifierForLocale(locale) : NULL);

    if (!langCode && isEightBit) {
        uint8_t* contents = (uint8_t*)__CFStrContents(string) + __CFStrSkipAnyLengthByte(string);
        for (; currentIndex < length; currentIndex++) {
            if (contents[currentIndex] >= 'a' && contents[currentIndex] <= 'z') {
                contents[currentIndex] -= 'a' - 'A';
            } else if (contents[currentIndex] > 127) {
                break;
            }
        }
    }

    if (currentIndex < length) {
        UniChar* contents;
        UniChar mappedCharacters[MAX_CASE_MAPPING_BUF];
        CFIndex mappedLength;
        UTF32Char currentChar;
        UInt32 flags = 0;

        if (isEightBit) {
            __CFStringChangeSize(string, CFRangeMake(0, 0), 0, true);
        }

        contents = (UniChar*)__CFStrContents(string);

        for (; currentIndex < length; currentIndex++) {
            if (_CFUniCharIsSurrogateHighCharacter(contents[currentIndex]) &&
                (currentIndex + 1 < length) &&
                _CFUniCharIsSurrogateLowCharacter(contents[currentIndex + 1]))
            {
                currentChar = _CFUniCharGetLongCharacterForSurrogatePair(
                    contents[currentIndex],
                    contents[currentIndex + 1]);
            } else {
                currentChar = contents[currentIndex];
            }

            flags = (langCode ? _CFUniCharGetConditionalCaseMappingFlags(currentChar, contents, currentIndex, length, kCFUniCharToUppercase, langCode, flags) : 0);

            mappedLength = _CFUniCharMapCaseTo(currentChar, mappedCharacters, MAX_CASE_MAPPING_BUF, kCFUniCharToUppercase, flags, langCode);
            if (mappedLength > 0) {
                contents[currentIndex] = *mappedCharacters;
            }

            if (currentChar > 0xFFFF) { // Non-BMP char
                switch (mappedLength) {
                    case 0:
                        __CFStringChangeSize(string, CFRangeMake(currentIndex, 2), 0, true);
                        contents = (UniChar*)__CFStrContents(string);
                        length -= 2;
                        break;

                    case 1:
                        __CFStringChangeSize(string, CFRangeMake(currentIndex + 1, 1), 0, true);
                        contents = (UniChar*)__CFStrContents(string);
                        --length;
                        break;

                    case 2:
                        contents[++currentIndex] = mappedCharacters[1];
                        break;

                    default:
                        --mappedLength; // Skip the current char
                        __CFStringChangeSize(string, CFRangeMake(currentIndex + 1, 0), mappedLength - 1, true);
                        contents = (UniChar*)__CFStrContents(string);
                        memmove(contents + currentIndex + 1, mappedCharacters + 1, mappedLength * sizeof(UniChar));
                        length += (mappedLength - 1);
                        currentIndex += mappedLength;
                        break;
                }
            } else if (mappedLength == 0) {
                __CFStringChangeSize(string, CFRangeMake(currentIndex, 1), 0, true);
                contents = (UniChar*)__CFStrContents(string);
                --length;
            } else if (mappedLength > 1) {
                --mappedLength; // Skip the current char
                __CFStringChangeSize(string, CFRangeMake(currentIndex + 1, 0), mappedLength, true);
                contents = (UniChar*)__CFStrContents(string);
                memmove(contents + currentIndex + 1, mappedCharacters + 1, mappedLength * sizeof(UniChar));
                length += mappedLength;
                currentIndex += mappedLength;
            }
        }
    }
}

void CFStringCapitalize(CFMutableStringRef string, CFLocaleRef locale) {
    CFIndex currentIndex = 0;
    CFIndex length;
    const uint8_t* langCode;
    Boolean isEightBit = __CFStrIsEightBit(string);
    Boolean isLastCased = false;
    const uint8_t* caseIgnorableForBMP;

    CF_OBJC_VOID_FUNCDISPATCH(string, "_cfCapitalize:", locale);

    CF_VALIDATE_MUTABLESTRING_ARG(string);

    length = __CFStrLength(string);

    caseIgnorableForBMP = _CFUniCharGetBitmapPtrForPlane(kCFUniCharCaseIgnorableCharacterSet, 0);

    langCode = (const uint8_t*)(_CFCanUseLocale(locale) ? _CFStrGetLanguageIdentifierForLocale(locale) : NULL);

    if (!langCode && isEightBit) {
        uint8_t* contents = (uint8_t*)__CFStrContents(string) + __CFStrSkipAnyLengthByte(string);
        for (; currentIndex < length; currentIndex++) {
            if (contents[currentIndex] > 127) {
                break;
            } else if (contents[currentIndex] >= 'A' && contents[currentIndex] <= 'Z') {
                contents[currentIndex] += (isLastCased ? 'a' - 'A' : 0);
                isLastCased = true;
            } else if (contents[currentIndex] >= 'a' && contents[currentIndex] <= 'z') {
                contents[currentIndex] -= (!isLastCased ? 'a' - 'A' : 0);
                isLastCased = true;
            } else if (!_CFUniCharIsMemberOfBitmap(contents[currentIndex], caseIgnorableForBMP)) {
                isLastCased = false;
            }
        }
    }

    if (currentIndex < length) {
        UniChar* contents;
        UniChar mappedCharacters[MAX_CASE_MAPPING_BUF];
        CFIndex mappedLength;
        UTF32Char currentChar;
        UInt32 flags = 0;

        if (isEightBit) {
            __CFStringChangeSize(string, CFRangeMake(0, 0), 0, true);
        }

        contents = (UniChar*)__CFStrContents(string);

        for (; currentIndex < length; currentIndex++) {
            if (_CFUniCharIsSurrogateHighCharacter(contents[currentIndex]) &&
                (currentIndex + 1 < length) &&
                _CFUniCharIsSurrogateLowCharacter(contents[currentIndex + 1]))
            {
                currentChar = _CFUniCharGetLongCharacterForSurrogatePair(
                    contents[currentIndex],
                    contents[currentIndex + 1]);
            } else {
                currentChar = contents[currentIndex];
            }
            flags = ((langCode || ((currentChar == 0x03A3) && isLastCased)) ? _CFUniCharGetConditionalCaseMappingFlags(currentChar, contents, currentIndex, length, (isLastCased ? kCFUniCharToLowercase : kCFUniCharToTitlecase), langCode, flags) : 0);

            mappedLength = _CFUniCharMapCaseTo(currentChar, mappedCharacters, MAX_CASE_MAPPING_BUF, (isLastCased ? kCFUniCharToLowercase : kCFUniCharToTitlecase), flags, langCode);
            if (mappedLength > 0) {
                contents[currentIndex] = *mappedCharacters;
            }

            if (currentChar > 0xFFFF) { // Non-BMP char
                switch (mappedLength) {
                    case 0:
                        __CFStringChangeSize(string, CFRangeMake(currentIndex, 2), 0, true);
                        contents = (UniChar*)__CFStrContents(string);
                        length -= 2;
                        break;

                    case 1:
                        __CFStringChangeSize(string, CFRangeMake(currentIndex + 1, 1), 0, true);
                        contents = (UniChar*)__CFStrContents(string);
                        --length;
                        break;

                    case 2:
                        contents[++currentIndex] = mappedCharacters[1];
                        break;

                    default:
                        --mappedLength; // Skip the current char
                        __CFStringChangeSize(string, CFRangeMake(currentIndex + 1, 0), mappedLength - 1, true);
                        contents = (UniChar*)__CFStrContents(string);
                        memmove(contents + currentIndex + 1, mappedCharacters + 1, mappedLength * sizeof(UniChar));
                        length += (mappedLength - 1);
                        currentIndex += mappedLength;
                        break;
                }
            } else if (mappedLength == 0) {
                __CFStringChangeSize(string, CFRangeMake(currentIndex, 1), 0, true);
                contents = (UniChar*)__CFStrContents(string);
                --length;
            } else if (mappedLength > 1) {
                --mappedLength; // Skip the current char
                __CFStringChangeSize(string, CFRangeMake(currentIndex + 1, 0), mappedLength, true);
                contents = (UniChar*)__CFStrContents(string);
                memmove(contents + currentIndex + 1, mappedCharacters + 1, mappedLength * sizeof(UniChar));
                length += mappedLength;
                currentIndex += mappedLength;
            }

            if (!((currentChar > 0xFFFF) ? _CFUniCharIsMemberOf(currentChar, kCFUniCharCaseIgnorableCharacterSet) : _CFUniCharIsMemberOfBitmap(currentChar, caseIgnorableForBMP))) { // We have non-caseignorable here
                isLastCased = ((_CFUniCharIsMemberOf(currentChar, kCFUniCharUppercaseLetterCharacterSet) || _CFUniCharIsMemberOf(currentChar, kCFUniCharLowercaseLetterCharacterSet)) ? true : false);
            }
        }
    }
}

void CFStringNormalize(CFMutableStringRef string, CFStringNormalizationForm theForm) {
    CFIndex currentIndex = 0;
    CFIndex length;
    bool needToReorder = true;

    CF_OBJC_VOID_FUNCDISPATCH(string, "_cfNormalize:", theForm);

    CF_VALIDATE_MUTABLESTRING_ARG(string);

    length = __CFStrLength(string);

    if (__CFStrIsEightBit(string)) {
        uint8_t* contents;

        if (theForm == kCFStringNormalizationFormC) {
            return;                                         // 8bit form has no decomposition

        }
        contents = (uint8_t*)__CFStrContents(string) + __CFStrSkipAnyLengthByte(string);

        for (; currentIndex < length; currentIndex++) {
            if (contents[currentIndex] > 127) {
                __CFStringChangeSize(string, CFRangeMake(0, 0), 0, true); // need to do harm way
                needToReorder = false;
                break;
            }
        }
    }

    if (currentIndex < length) {
        UTF16Char* limit = (UTF16Char*)__CFStrContents(string) + length;
        UTF16Char* contents = (UTF16Char*)__CFStrContents(string) + currentIndex;
        UTF32Char buffer[MAX_DECOMP_BUF];
        UTF32Char* mappedCharacters = buffer;
        CFIndex allocatedLength = MAX_DECOMP_BUF;
        CFIndex mappedLength;
        CFIndex currentLength;
        UTF32Char currentChar;
        const uint8_t* decompBMP = _CFUniCharGetBitmapPtrForPlane(kCFUniCharCanonicalDecomposableCharacterSet, 0);
        const uint8_t* nonBaseBMP = _CFUniCharGetBitmapPtrForPlane(kCFUniCharNonBaseCharacterSet, 0);
        const uint8_t* combiningBMP = (const uint8_t*)_CFUniCharGetUnicodePropertyDataForPlane(kCFUniCharCombiningProperty, 0);

        while (contents < limit) {
            if (_CFUniCharIsSurrogateHighCharacter(*contents) && (contents + 1 < limit) && _CFUniCharIsSurrogateLowCharacter(*(contents + 1))) {
                currentChar = _CFUniCharGetLongCharacterForSurrogatePair(*contents, *(contents + 1));
                currentLength = 2;
                contents += 2;
            } else {
                currentChar = *(contents++);
                currentLength = 1;
            }

            mappedLength = 0;

            if (_CFUniCharIsMemberOfBitmap(currentChar, ((currentChar < 0x10000) ? decompBMP : _CFUniCharGetBitmapPtrForPlane(kCFUniCharCanonicalDecomposableCharacterSet, (currentChar >> 16)))) && (!_CFUniCharGetCombiningPropertyForCharacter(currentChar, ((currentChar < 0x10000) ? combiningBMP : (const uint8_t*)_CFUniCharGetUnicodePropertyDataForPlane(kCFUniCharCombiningProperty, (currentChar >> 16)))))) {
                if ((theForm & kCFStringNormalizationFormC) == 0 || currentChar < HANGUL_SBASE || currentChar > (HANGUL_SBASE + HANGUL_SCOUNT)) { // We don't have to decompose Hangul Syllables if we're precomposing again
                    mappedLength = _CFUniCharDecomposeCharacter(currentChar, mappedCharacters, MAX_DECOMP_BUF);
                }
            }

            if ((needToReorder || (theForm & kCFStringNormalizationFormC)) && ((contents < limit) || (mappedLength == 0))) {
                if (mappedLength > 0) {
                    if (_CFUniCharIsSurrogateHighCharacter(*contents) && (contents + 1 < limit) && _CFUniCharIsSurrogateLowCharacter(*(contents + 1))) {
                        currentChar = _CFUniCharGetLongCharacterForSurrogatePair(*contents, *(contents + 1));
                    } else {
                        currentChar = *contents;
                    }
                }

                if (_CFUniCharGetCombiningPropertyForCharacter(currentChar, (const uint8_t*)((currentChar < 0x10000) ? combiningBMP : _CFUniCharGetUnicodePropertyDataForPlane(kCFUniCharCombiningProperty, (currentChar >> 16))))) {
                    uint32_t decompLength;

                    if (mappedLength == 0) {
                        contents -= (currentChar & 0xFFFF0000 ? 2 : 1);
                        if (currentIndex > 0) {
                            if (_CFUniCharIsSurrogateLowCharacter(*(contents - 1)) && (currentIndex > 1) && _CFUniCharIsSurrogateHighCharacter(*(contents - 2))) {
                                *mappedCharacters = _CFUniCharGetLongCharacterForSurrogatePair(*(contents - 2), *(contents - 1));
                                currentIndex -= 2;
                                currentLength += 2;
                            } else {
                                *mappedCharacters = *(contents - 1);
                                --currentIndex;
                                ++currentLength;
                            }
                            mappedLength = 1;
                        }
                    } else {
                        currentLength += (currentChar & 0xFFFF0000 ? 2 : 1);
                    }
                    contents += (currentChar & 0xFFFF0000 ? 2 : 1);

                    if (_CFUniCharIsMemberOfBitmap(currentChar, ((currentChar < 0x10000) ? decompBMP : _CFUniCharGetBitmapPtrForPlane(kCFUniCharCanonicalDecomposableCharacterSet, (currentChar >> 16))))) { // Vietnamese accent, etc.
                        decompLength = _CFUniCharDecomposeCharacter(currentChar, mappedCharacters + mappedLength, MAX_DECOMP_BUF - mappedLength);
                        mappedLength += decompLength;
                    } else {
                        mappedCharacters[mappedLength++] = currentChar;
                    }

                    while (contents < limit) {
                        if (_CFUniCharIsSurrogateHighCharacter(*contents) && (contents + 1 < limit) && _CFUniCharIsSurrogateLowCharacter(*(contents + 1))) {
                            currentChar = _CFUniCharGetLongCharacterForSurrogatePair(*contents, *(contents + 1));
                        } else {
                            currentChar = *contents;
                        }
                        if (!_CFUniCharGetCombiningPropertyForCharacter(currentChar, (const uint8_t*)((currentChar < 0x10000) ? combiningBMP : _CFUniCharGetUnicodePropertyDataForPlane(kCFUniCharCombiningProperty, (currentChar >> 16))))) {
                            break;
                        }
                        if (currentChar & 0xFFFF0000) {
                            contents += 2;
                            currentLength += 2;
                        } else {
                            ++contents;
                            ++currentLength;
                        }
                        if (mappedLength == allocatedLength) {
                            allocatedLength += MAX_DECOMP_BUF;
                            if (mappedCharacters == buffer) {
                                mappedCharacters = (UTF32Char*)CFAllocatorAllocate(kCFAllocatorSystemDefault, allocatedLength * sizeof(UTF32Char), 0);
                                memmove(mappedCharacters, buffer, MAX_DECOMP_BUF * sizeof(UTF32Char));
                            } else {
                                mappedCharacters = (UTF32Char*)CFAllocatorReallocate(kCFAllocatorSystemDefault, mappedCharacters, allocatedLength * sizeof(UTF32Char), 0);
                            }
                        }
                        if (_CFUniCharIsMemberOfBitmap(currentChar, ((currentChar < 0x10000) ? decompBMP : _CFUniCharGetBitmapPtrForPlane(kCFUniCharCanonicalDecomposableCharacterSet, (currentChar >> 16))))) { // Vietnamese accent, etc.
                            decompLength = _CFUniCharDecomposeCharacter(currentChar, mappedCharacters + mappedLength, MAX_DECOMP_BUF - mappedLength);
                            mappedLength += decompLength;
                        } else {
                            mappedCharacters[mappedLength++] = currentChar;
                        }
                    }
                }
                if (needToReorder && mappedLength > 1) {
                    _CFUniCharPrioritySort(mappedCharacters, mappedLength);
                }
            }

            if (theForm & kCFStringNormalizationFormKD) {
                CFIndex newLength = 0;

                if (mappedLength == 0 && _CFUniCharIsMemberOf(currentChar, kCFUniCharCompatibilityDecomposableCharacterSet)) {
                    mappedCharacters[mappedLength++] = currentChar;
                }
                while (newLength < mappedLength) {
                    newLength = _CFUniCharCompatibilityDecompose(mappedCharacters, mappedLength, allocatedLength);
                    if (newLength == 0) {
                        allocatedLength += MAX_DECOMP_BUF;
                        if (mappedCharacters == buffer) {
                            mappedCharacters = (UTF32Char*)CFAllocatorAllocate(kCFAllocatorSystemDefault, allocatedLength * sizeof(UTF32Char), 0);
                            memmove(mappedCharacters, buffer, MAX_DECOMP_BUF * sizeof(UTF32Char));
                        } else {
                            mappedCharacters = (UTF32Char*)CFAllocatorReallocate(kCFAllocatorSystemDefault, mappedCharacters, allocatedLength * sizeof(UTF32Char), 0);
                        }
                    }
                }
                mappedLength = newLength;
            }

            if (theForm & kCFStringNormalizationFormC) {
                UTF32Char nextChar;

                if (mappedLength > 1) {
                    CFIndex consumedLength = 1;
                    UTF32Char* currentBase = mappedCharacters;
                    uint8_t currentClass, lastClass = 0;
                    bool didCombine = false;

                    currentChar = *mappedCharacters;

                    while (consumedLength < mappedLength) {
                        nextChar = mappedCharacters[consumedLength];
                        currentClass = _CFUniCharGetCombiningPropertyForCharacter(nextChar, (const uint8_t*)((nextChar < 0x10000) ? combiningBMP : _CFUniCharGetUnicodePropertyDataForPlane(kCFUniCharCombiningProperty, (nextChar >> 16))));

                        if (theForm & kCFStringNormalizationFormKD) {
                            if ((currentChar >= HANGUL_LBASE) && (currentChar < (HANGUL_LBASE + 0xFF))) {
                                SInt8 lIndex = currentChar - HANGUL_LBASE;

                                if ((0 <= lIndex) && (lIndex <= HANGUL_LCOUNT)) {
                                    SInt16 vIndex = nextChar - HANGUL_VBASE;

                                    if ((vIndex >= 0) && (vIndex <= HANGUL_VCOUNT)) {
                                        SInt16 tIndex = 0;
                                        CFIndex usedLength = mappedLength;

                                        mappedCharacters[consumedLength++] = 0xFFFD;

                                        if (consumedLength < mappedLength) {
                                            tIndex = mappedCharacters[consumedLength] - HANGUL_TBASE;
                                            if ((tIndex < 0) || (tIndex > HANGUL_TCOUNT)) {
                                                tIndex = 0;
                                            } else {
                                                mappedCharacters[consumedLength++] = 0xFFFD;
                                            }
                                        }
                                        *currentBase = (lIndex * HANGUL_VCOUNT + vIndex) * HANGUL_TCOUNT + tIndex + HANGUL_SBASE;

                                        while (--usedLength > 0) {
                                            if (mappedCharacters[usedLength] == 0xFFFD) {
                                                --mappedLength;
                                                --consumedLength;
                                                memmove(mappedCharacters + usedLength, mappedCharacters + usedLength + 1, (mappedLength - usedLength) * sizeof(UTF32Char));
                                            }
                                        }
                                        currentBase = mappedCharacters + consumedLength;
                                        currentChar = *currentBase;
                                        ++consumedLength;

                                        continue;
                                    }
                                }
                            }
                            if (!_CFUniCharIsMemberOfBitmap(nextChar, ((nextChar < 0x10000) ? nonBaseBMP : _CFUniCharGetBitmapPtrForPlane(kCFUniCharNonBaseCharacterSet, (nextChar >> 16))))) {
                                *currentBase = currentChar;
                                currentBase = mappedCharacters + consumedLength;
                                currentChar = nextChar;
                                ++consumedLength;
                                continue;
                            }
                        }

                        if ((lastClass == 0) || (currentClass > lastClass)) {
                            nextChar = _CFUniCharPrecomposeCharacter(currentChar, nextChar);
                            if (nextChar == 0xFFFD) {
                                lastClass = currentClass;
                            } else {
                                mappedCharacters[consumedLength] = 0xFFFD;
                                didCombine = true;
                                currentChar = nextChar;
                            }
                        }
                        ++consumedLength;
                    }

                    *currentBase = currentChar;
                    if (didCombine) {
                        consumedLength = mappedLength;
                        while (--consumedLength > 0) {
                            if (mappedCharacters[consumedLength] == 0xFFFD) {
                                --mappedLength;
                                memmove(mappedCharacters + consumedLength, mappedCharacters + consumedLength + 1, (mappedLength - consumedLength) * sizeof(UTF32Char));
                            }
                        }
                    }
                } else if ((currentChar >= HANGUL_LBASE) && (currentChar < (HANGUL_LBASE + 0xFF))) { // Hangul Jamo
                    SInt8 lIndex = currentChar - HANGUL_LBASE;

                    if ((contents < limit) && (0 <= lIndex) && (lIndex <= HANGUL_LCOUNT)) {
                        SInt16 vIndex = *contents - HANGUL_VBASE;

                        if ((vIndex >= 0) && (vIndex <= HANGUL_VCOUNT)) {
                            SInt16 tIndex = 0;

                            ++contents; ++currentLength;

                            if (contents < limit) {
                                tIndex = *contents - HANGUL_TBASE;
                                if ((tIndex < 0) || (tIndex > HANGUL_TCOUNT)) {
                                    tIndex = 0;
                                } else {
                                    ++contents; ++currentLength;
                                }
                            }
                            *mappedCharacters = (lIndex * HANGUL_VCOUNT + vIndex) * HANGUL_TCOUNT + tIndex + HANGUL_SBASE;
                            mappedLength = 1;
                        }
                    }
                } else { // collect class 0 non-base characters
                    while (contents < limit) {
                        nextChar = *contents;
                        if (_CFUniCharIsSurrogateHighCharacter(nextChar) && ((contents + 1) < limit) && _CFUniCharIsSurrogateLowCharacter(*(contents + 1))) {
                            nextChar = _CFUniCharGetLongCharacterForSurrogatePair(nextChar, *(contents + 1));
                            if (!_CFUniCharIsMemberOfBitmap(nextChar, (const uint8_t*)_CFUniCharGetBitmapPtrForPlane(kCFUniCharNonBaseCharacterSet, (nextChar >> 16))) || (_CFUniCharGetCombiningPropertyForCharacter(nextChar, (const uint8_t*)_CFUniCharGetUnicodePropertyDataForPlane(kCFUniCharCombiningProperty, (nextChar >> 16))))) {
                                break;
                            }
                        } else {
                            if (!_CFUniCharIsMemberOfBitmap(nextChar, nonBaseBMP) || (_CFUniCharGetCombiningPropertyForCharacter(nextChar, combiningBMP))) {
                                break;
                            }
                        }
                        currentChar = _CFUniCharPrecomposeCharacter(currentChar, nextChar);
                        if (0xFFFD == currentChar) {
                            break;
                        }

                        if (nextChar < 0x10000) {
                            ++contents; ++currentLength;
                        } else {
                            contents += 2;
                            currentLength += 2;
                        }

                        *mappedCharacters = currentChar;
                        mappedLength = 1;
                    }
                }
            }

            if (mappedLength > 0) {
                CFIndex utf16Length = __CFGetUTF16Length(mappedCharacters, mappedLength);

                if (utf16Length != currentLength) {
                    __CFStringChangeSize(string, CFRangeMake(currentIndex, currentLength), utf16Length, true);
                    currentLength = utf16Length;
                }
                contents = (UTF16Char*)__CFStrContents(string);
                limit = contents + __CFStrLength(string);
                contents += currentIndex;
                __CFFillInUTF16(mappedCharacters, contents, mappedLength);
                contents += utf16Length;
            }
            currentIndex += currentLength;
        }

        if (mappedCharacters != buffer) {
            CFAllocatorDeallocate(kCFAllocatorSystemDefault, mappedCharacters);
        }
    }
}

CFMutableStringRef CFStringCreateMutable(CFAllocatorRef alloc, CFIndex maxLength) {
	CF_VALIDATE_LENGTH_ARG(maxLength);

    return __CFStringCreateMutableFunnel(alloc, maxLength, __kCFNotInlineContentsDefaultFree);
}

CFMutableStringRef CFStringCreateMutableWithExternalCharactersNoCopy(CFAllocatorRef alloc,
																	 UniChar* chars,
																	 CFIndex numChars,
																	 CFIndex capacity,
																	 CFAllocatorRef externalCharactersAllocator)
{
    CFOptionFlags contentsAllocationBits = externalCharactersAllocator ?
        (externalCharactersAllocator == kCFAllocatorNull ?
			__kCFNotInlineContentsNoFree :
			__kCFHasContentsAllocator) :
        __kCFNotInlineContentsDefaultFree;

    CFMutableStringRef string = __CFStringCreateMutableFunnel(
		alloc,
		0,
		contentsAllocationBits | __kCFIsUnicode);
    if (!string) {
		return NULL;
	}

    __CFStrSetIsExternalMutable(string);
    if (contentsAllocationBits == __kCFHasContentsAllocator) {
        __CFStrSetContentsAllocator(string, (CFAllocatorRef)CFRetain(externalCharactersAllocator));
    }
    CFStringSetExternalCharactersNoCopy(string, chars, numChars, capacity);
    return string;
}

CFMutableStringRef CFStringCreateMutableWithCharacters(CFAllocatorRef allocator,
                                                       const UniChar* chars,
                                                       CFIndex length)
{
    CF_VALIDATE_LENGTH_PTR_ARGS(length, chars);
    
	CFMutableStringRef instance = CFStringCreateMutable(allocator, 0);
    CFStringAppendCharacters(instance, chars, length);
	return instance;
}

CFMutableStringRef CFStringCreateMutableWithCString(CFAllocatorRef allocator,
                                                    const char* chars,
                                                    CFStringEncoding encoding)

{
    CF_VALIDATE_PTR_ARG(chars);
    
    CFMutableStringRef instance = CFStringCreateMutable(allocator, 0);
    CFStringAppendCString(instance, chars, encoding);
	return instance;
}

CFMutableStringRef CFStringCreateMutableWithBytes(CFAllocatorRef allocator,
                                                  const void* bytes,
                                                  CFIndex length,
                                                  CFStringEncoding encoding)
{
 	CF_VALIDATE_LENGTH_PTR_ARGS(length, bytes);
    
    CFMutableStringRef instance = CFStringCreateMutable(allocator, 0);
    __CFStringAppendBytes(instance, (const char*)bytes, length, encoding);
	return instance;
}

CFMutableStringRef CFStringCreateMutableWithFormatAndArguments(CFAllocatorRef allocator,
                                                               CFDictionaryRef formatOptions,
                                                               CFStringRef format,
                                                               va_list arguments)
{
    CF_VALIDATE_PTR_ARG(format);
    
    CFMutableStringRef instance = CFStringCreateMutable(allocator, 0);
    CFStringAppendFormatAndArguments(instance, formatOptions, format, arguments);
    return instance;
}

CFMutableStringRef CFStringCreateMutableWithFormat(CFAllocatorRef allocator,
                                                   CFDictionaryRef formatOptions,
                                                   CFStringRef format,
                                                   ...)
{
    va_list arguments;
    va_start(arguments, format);
    CFMutableStringRef instance = CFStringCreateMutableWithFormatAndArguments(
    	allocator,
		formatOptions,
		format,
		arguments);
    va_end(arguments);
    return instance;
}
