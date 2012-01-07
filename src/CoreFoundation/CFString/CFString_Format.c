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

#if defined(__GNUC__)
#define LONG_DOUBLE_SUPPORT 1
#else
#define LONG_DOUBLE_SUPPORT 0
#endif

enum {
    __kCFFormatZeroFlag = (1 << 0), // if not, padding is space char
    __kCFFormatMinusFlag = (1 << 1),// if not, no flag implied
    __kCFFormatPlusFlag = (1 << 2), // if not, no flag implied, overrides space
    __kCFFormatSpaceFlag = (1 << 3) // if not, no flag implied
};

typedef struct {
    int16_t size;
    int16_t type;
    SInt32 loc;
    SInt32 len;
    SInt32 widthArg;
    SInt32 precArg;
    uint32_t flags;
    int8_t mainArgNum;
    int8_t precArgNum;
    int8_t widthArgNum;
    int8_t unused1;
} __CFFormatSpec;

typedef struct {
    int16_t type;
    int16_t size;
    union {
        int64_t int64Value;
        double doubleValue;
#if LONG_DOUBLE_SUPPORT
        long double longDoubleValue;
#endif
        void* pointerValue;
    } value;
} __CFPrintValue;

enum {
    __kCFFormatDefaultSize = 0,
    __kCFFormatSize1 = 1,
    __kCFFormatSize2 = 2,
    __kCFFormatSize4 = 3,
    __kCFFormatSize8 = 4,
    __kCFFormatSize16 = 5,
    __kCFFormatSizeLong = __kCFFormatSize4,
    __kCFFormatSizePointer = __kCFFormatSize4,

    __kCFFormatLiteralType = 32,
    __kCFFormatLongType = 33,
    __kCFFormatDoubleType = 34,
    __kCFFormatPointerType = 35,
    __kCFFormatCFType = 37,            /* handled specially */
    __kCFFormatUnicharsType = 38,      /* handled specially */
    __kCFFormatCharsType = 39,         /* handled specially */
    __kCFFormatPascalCharsType = 40,   /* handled specially */
    __kCFFormatSingleUnicharType = 41, /* handled specially */
    __kCFFormatDummyPointerType = 42   /* special case for %n */
};

#define SNPRINTF(TYPE, WHAT) \
    { \
        TYPE value = (TYPE)WHAT; \
        if (-1 != specs[curSpec].widthArgNum) { \
            if (-1 != specs[curSpec].precArgNum) { \
                snprintf(buffer, 255, formatBuffer, width, precision, value); \
            } else { \
                snprintf(buffer, 255, formatBuffer, width, value); \
            } \
        } else { \
            if (-1 != specs[curSpec].precArgNum) { \
                snprintf(buffer, 255, formatBuffer, precision, value); \
            } else { \
                snprintf(buffer, 255, formatBuffer, value); \
            } \
        } \
    }

///////////////////////////////////////////////////////////////////// private

CF_INLINE void __CFParseFormatSpec(const UniChar* uformat, const uint8_t* cformat,
                                   SInt32* fmtIdx, SInt32 fmtLen,
                                   __CFFormatSpec* spec)
{
    Boolean seenDot = false;
    for (;;) {
        UniChar ch;
        if (fmtLen <= *fmtIdx) {
            return;                   /* no type */
        }
        if (cformat) {
            ch = (UniChar)cformat[(*fmtIdx)++];
        } else {
            ch = uformat[(*fmtIdx)++];
        }
 reswtch:
        switch (ch) {
            case '#': // ignored for now
                break;
            case 0x20:
                if (!(spec->flags & __kCFFormatPlusFlag)) {
                    spec->flags |= __kCFFormatSpaceFlag;
                }
                break;
            case '-':
                spec->flags |= __kCFFormatMinusFlag;
                spec->flags &= ~__kCFFormatZeroFlag; // remove zero flag
                break;
            case '+':
                spec->flags |= __kCFFormatPlusFlag;
                spec->flags &= ~__kCFFormatSpaceFlag; // remove space flag
                break;
            case '0':
                if (!(spec->flags & __kCFFormatMinusFlag)) {
                    spec->flags |= __kCFFormatZeroFlag;
                }
                break;
            case 'h':
                spec->size = __kCFFormatSize2;
                break;
            case 'l':
                if (*fmtIdx < fmtLen) {
                    // fetch next character, don't increment fmtIdx
                    if (cformat) {
                        ch = (UniChar)cformat[(*fmtIdx)];
                    } else {ch = uformat[(*fmtIdx)]; }
                    if ('l' == ch) { // 'll' for long long, like 'q'
                        (*fmtIdx)++;
                        spec->size = __kCFFormatSize8;
                        break;
                    }
                }
                spec->size = __kCFFormatSizeLong;
                break;
#if LONG_DOUBLE_SUPPORT
            case 'L':
                spec->size = __kCFFormatSize16;
                break;
#endif
            case 'q':
                spec->size = __kCFFormatSize8;
                break;
            case 't': case 'z':
                spec->size = __kCFFormatSizeLong;
                break;
            case 'j':
                spec->size = __kCFFormatSize8;
                break;
            case 'c':
                spec->type = __kCFFormatLongType;
                spec->size = __kCFFormatSize1;
                return;
            case 'O': case 'o': case 'D': case 'd': case 'i': case 'U': case 'u': case 'x': case 'X':
                spec->type = __kCFFormatLongType;
                // Seems like if spec->size == 0, we should spec->size = __kCFFormatSize4.
                // However, 0 is handled correctly.
                return;
            case 'a': case 'A': case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':
                spec->type = __kCFFormatDoubleType;
                if (spec->size != __kCFFormatSize16) {
                    spec->size = __kCFFormatSize8;
                }
                return;
            case 'n':
                // %n is not handled correctly; for Leopard or newer apps, 
                //  we disable it further.
                spec->type = __kCFFormatDummyPointerType;
                spec->size = __kCFFormatSizePointer;
                return;
            case 'p':
                spec->type = __kCFFormatPointerType;
                spec->size = __kCFFormatSizePointer;
                return;
            case 's':
                spec->type = __kCFFormatCharsType;
                spec->size = __kCFFormatSizePointer;
                return;
            case 'S':
                spec->type = __kCFFormatUnicharsType;
                spec->size = __kCFFormatSizePointer;
                return;
            case 'C':
                spec->type = __kCFFormatSingleUnicharType;
                spec->size = __kCFFormatSize2;
                return;
            case 'P':
                spec->type = __kCFFormatPascalCharsType;
                spec->size = __kCFFormatSizePointer;
                return;
            case '@':
                spec->type = __kCFFormatCFType;
                spec->size = __kCFFormatSizePointer;
                return;
            case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': {
                int64_t number = 0;
                do {
                    number = 10 * number + (ch - '0');
                    if (cformat) {
                        ch = (UniChar)cformat[(*fmtIdx)++];
                    } else {
                        ch = uformat[(*fmtIdx)++];
                    }
                } while ((UInt32)(ch - '0') <= 9);
                if ('$' == ch) {
                    // Arg numbers start from 1.
                    if (-2 == spec->precArgNum) {
                        spec->precArgNum = (int8_t)number - 1;
                    } else if (-2 == spec->widthArgNum) {
                        spec->widthArgNum = (int8_t)number - 1;
                    } else {
                        spec->mainArgNum = (int8_t)number - 1;
                    }
                    break;
                } else if (seenDot) {
                    // Else it's either precision or width.
                    spec->precArg = (SInt32)number;
                } else {
                    spec->widthArg = (SInt32)number;
                }
                goto reswtch;
            }
            case '*':
                spec->widthArgNum = -2;
                break;
            case '.':
                seenDot = true;
                if (cformat) {
                    ch = (UniChar)cformat[(*fmtIdx)++];
                } else {
                    ch = uformat[(*fmtIdx)++];
                }
                if ('*' == ch) {
                    spec->precArgNum = -2;
                    break;
                }
                goto reswtch;
            default:
                spec->type = __kCFFormatLiteralType;
                return;
        }
    }
}


///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL
void _CFStringAppendFormatAndArgumentsAux(CFMutableStringRef outputString,
                                          CFStringRef (*copyDescFunc)(void*, const void*),
                                          CFDictionaryRef formatOptions,
                                          CFStringRef formatString,
                                          va_list args)
{
    SInt32 numSpecs, sizeSpecs, sizeArgNum, formatIdx, curSpec, argNum;
    CFIndex formatLen;
    
    #define FORMAT_BUFFER_LEN 400
    const uint8_t* cformat = NULL;
    const UniChar* uformat = NULL;
    UniChar* formatChars = NULL;
    UniChar localFormatBuffer[FORMAT_BUFFER_LEN];

    #define VPRINTF_BUFFER_LEN 61
    __CFFormatSpec localSpecsBuffer[VPRINTF_BUFFER_LEN];
    __CFFormatSpec* specs;
    __CFPrintValue localValuesBuffer[VPRINTF_BUFFER_LEN];
    __CFPrintValue* values;
    CFAllocatorRef tmpAlloc = NULL;

    intmax_t dummyLocation; // A place for %n to do its thing in; should be the widest possible int value

    numSpecs = 0;
    sizeSpecs = 0;
    sizeArgNum = 0;
    specs = NULL;
    values = NULL;

    formatLen = CFStringGetLength(formatString);
    if (!CF_IS_OBJC(formatString)) {
        CF_VALIDATE_STRING_ARG(formatString);
        if (!__CFStrIsUnicode(formatString)) {
            cformat = (const uint8_t*)__CFStrContents(formatString);
            if (cformat) {
                cformat += __CFStrSkipAnyLengthByte(formatString);
            }
        } else {
            uformat = (const UniChar*)__CFStrContents(formatString);
        }
    }
    if (!cformat && !uformat) {
        formatChars = (formatLen > FORMAT_BUFFER_LEN) ? (UniChar*)CFAllocatorAllocate(tmpAlloc = CFAllocatorGetDefault(), formatLen * sizeof(UniChar), 0) : localFormatBuffer;

        CFStringGetCharacters(formatString, CFRangeMake(0, formatLen), formatChars);
        uformat = formatChars;
    }

    /* Compute an upper bound for the number of format specifications */
    if (cformat) {
        for (formatIdx = 0; formatIdx < formatLen; formatIdx++) {
            if ('%' == cformat[formatIdx]) {
                sizeSpecs++;
            }
        }
    } else {
        for (formatIdx = 0; formatIdx < formatLen; formatIdx++) {
            if ('%' == uformat[formatIdx]) {
                sizeSpecs++;
            }
        }
    }
    tmpAlloc = CFAllocatorGetDefault();
    specs = ((2 * sizeSpecs + 1) > VPRINTF_BUFFER_LEN) ? (__CFFormatSpec*)CFAllocatorAllocate(tmpAlloc, (2 * sizeSpecs + 1) * sizeof(__CFFormatSpec), 0) : localSpecsBuffer;

    /* Collect format specification information from the format string */
    for (curSpec = 0, formatIdx = 0; formatIdx < formatLen; curSpec++) {
        SInt32 newFmtIdx;
        specs[curSpec].loc = formatIdx;
        specs[curSpec].len = 0;
        specs[curSpec].size = 0;
        specs[curSpec].type = 0;
        specs[curSpec].flags = 0;
        specs[curSpec].widthArg = -1;
        specs[curSpec].precArg = -1;
        specs[curSpec].mainArgNum = -1;
        specs[curSpec].precArgNum = -1;
        specs[curSpec].widthArgNum = -1;
        if (cformat) {
            for (newFmtIdx = formatIdx; newFmtIdx < formatLen && '%' != cformat[newFmtIdx]; newFmtIdx++) {
                ;
            }
        } else {
            for (newFmtIdx = formatIdx; newFmtIdx < formatLen && '%' != uformat[newFmtIdx]; newFmtIdx++) {
                ;
            }
        }
        if (newFmtIdx != formatIdx) { /* Literal chunk */
            specs[curSpec].type = __kCFFormatLiteralType;
            specs[curSpec].len = newFmtIdx - formatIdx;
        } else {
            newFmtIdx++; /* Skip % */
            __CFParseFormatSpec(uformat, cformat, &newFmtIdx, formatLen, &(specs[curSpec]));
            if (__kCFFormatLiteralType == specs[curSpec].type) {
                specs[curSpec].loc = formatIdx + 1;
                specs[curSpec].len = 1;
            } else {
                specs[curSpec].len = newFmtIdx - formatIdx;
            }
        }
        formatIdx = newFmtIdx;
    }
    numSpecs = curSpec;
    // Max of three args per spec, reasoning thus: 1 width, 1 prec, 1 value
    values = ((3 * sizeSpecs + 1) > VPRINTF_BUFFER_LEN) ?
        (__CFPrintValue*)CFAllocatorAllocate(tmpAlloc, (3 * sizeSpecs + 1) * sizeof(__CFPrintValue), 0) :
        localValuesBuffer;
    memset(values, 0, (3 * sizeSpecs + 1) * sizeof(__CFPrintValue));
    sizeArgNum = (3 * sizeSpecs + 1);

    /* Compute values array */
    argNum = 0;
    for (curSpec = 0; curSpec < numSpecs; curSpec++) {
        SInt32 newMaxArgNum;
        if (0 == specs[curSpec].type) {
            continue;
        }
        if (__kCFFormatLiteralType == specs[curSpec].type) {
            continue;
        }
        newMaxArgNum = sizeArgNum;
        if (newMaxArgNum < specs[curSpec].mainArgNum) {
            newMaxArgNum = specs[curSpec].mainArgNum;
        }
        if (newMaxArgNum < specs[curSpec].precArgNum) {
            newMaxArgNum = specs[curSpec].precArgNum;
        }
        if (newMaxArgNum < specs[curSpec].widthArgNum) {
            newMaxArgNum = specs[curSpec].widthArgNum;
        }
        if (sizeArgNum < newMaxArgNum) {
            if (specs != localSpecsBuffer) {
                CFAllocatorDeallocate(tmpAlloc, specs);
            }
            if (values != localValuesBuffer) {
                CFAllocatorDeallocate(tmpAlloc, values);
            }
            if (formatChars && (formatChars != localFormatBuffer)) {
                CFAllocatorDeallocate(tmpAlloc, formatChars);
            }
            return; // more args than we expected!
        }
        // It is actually incorrect to reorder some specs and not all;
        //  we just do some random garbage here.
        if (-2 == specs[curSpec].widthArgNum) {
            specs[curSpec].widthArgNum = argNum++;
        }
        if (-2 == specs[curSpec].precArgNum) {
            specs[curSpec].precArgNum = argNum++;
        }
        if (-1 == specs[curSpec].mainArgNum) {
            specs[curSpec].mainArgNum = argNum++;
        }
        values[specs[curSpec].mainArgNum].size = specs[curSpec].size;
        values[specs[curSpec].mainArgNum].type = specs[curSpec].type;
        if (-1 != specs[curSpec].widthArgNum) {
            values[specs[curSpec].widthArgNum].size = 0;
            values[specs[curSpec].widthArgNum].type = __kCFFormatLongType;
        }
        if (-1 != specs[curSpec].precArgNum) {
            values[specs[curSpec].precArgNum].size = 0;
            values[specs[curSpec].precArgNum].type = __kCFFormatLongType;
        }
    }

    /* Collect the arguments in correct type from vararg list */
    for (argNum = 0; argNum < sizeArgNum; argNum++) {
        switch (values[argNum].type) {
            case 0:
            case __kCFFormatLiteralType:
                break;
            case __kCFFormatLongType:
            case __kCFFormatSingleUnicharType:
                if (__kCFFormatSize1 == values[argNum].size) {
                    values[argNum].value.int64Value = (int64_t)(int8_t)va_arg(args, int);
                } else if (__kCFFormatSize2 == values[argNum].size) {
                    values[argNum].value.int64Value = (int64_t)(int16_t)va_arg(args, int);
                } else if (__kCFFormatSize4 == values[argNum].size) {
                    values[argNum].value.int64Value = (int64_t)va_arg(args, int32_t);
                } else if (__kCFFormatSize8 == values[argNum].size) {
                    values[argNum].value.int64Value = (int64_t)va_arg(args, int64_t);
                } else {
                    values[argNum].value.int64Value = (int64_t)va_arg(args, int);
                }
                break;
            case __kCFFormatDoubleType:
#if LONG_DOUBLE_SUPPORT
                if (__kCFFormatSize16 == values[argNum].size) {
                    values[argNum].value.longDoubleValue = va_arg(args, long double);
                } else
#endif
                {
                    values[argNum].value.doubleValue = va_arg(args, double);
                }
                break;
            case __kCFFormatPointerType:
            case __kCFFormatCFType:
            case __kCFFormatUnicharsType:
            case __kCFFormatCharsType:
            case __kCFFormatPascalCharsType:
                values[argNum].value.pointerValue = va_arg(args, void*);
                break;
            case __kCFFormatDummyPointerType:
                (void)va_arg(args, void*); // Skip the provided argument
                values[argNum].value.pointerValue = &dummyLocation;
                break;
        }
    }
    va_end(args);

    /* Format the pieces together */
    for (curSpec = 0; curSpec < numSpecs; curSpec++) {
        SInt32 width = 0, precision = 0;
        UniChar* up, ch;
        Boolean hasWidth = false, hasPrecision = false;

        // widthArgNum and widthArg are never set at the same time; same for precArg*
        if (-1 != specs[curSpec].widthArgNum) {
            width = (SInt32)values[specs[curSpec].widthArgNum].value.int64Value;
            hasWidth = true;
        }
        if (-1 != specs[curSpec].precArgNum) {
            precision = (SInt32)values[specs[curSpec].precArgNum].value.int64Value;
            hasPrecision = true;
        }
        if (-1 != specs[curSpec].widthArg) {
            width = specs[curSpec].widthArg;
            hasWidth = true;
        }
        if (-1 != specs[curSpec].precArg) {
            precision = specs[curSpec].precArg;
            hasPrecision = true;
        }

        switch (specs[curSpec].type) {
            case __kCFFormatLongType:
            case __kCFFormatDoubleType:
            case __kCFFormatPointerType: {
                char formatBuffer[128];
#if defined(__GNUC__)
                char buffer[256 + width + precision];
#else
                char stackBuffer[512];
                char* dynamicBuffer = NULL;
                char* buffer = stackBuffer;
                if (256 + width + precision > 512) {
                    dynamicBuffer = (char*)CFAllocatorAllocate(kCFAllocatorSystemDefault, 256 + width + precision, 0);
                    buffer = dynamicBuffer;
                }
#endif
                SInt32 cidx, idx, loc;
                Boolean appended = false;
                loc = specs[curSpec].loc;
                // In preparation to call snprintf(), copy the format string out
                if (cformat) {
                    for (idx = 0, cidx = 0; cidx < specs[curSpec].len; idx++, cidx++) {
                        if ('$' == cformat[loc + cidx]) {
                            for (idx--; '0' <= formatBuffer[idx] && formatBuffer[idx] <= '9'; idx--) {
                                ;
                            }
                        } else {
                            formatBuffer[idx] = cformat[loc + cidx];
                        }
                    }
                } else {
                    for (idx = 0, cidx = 0; cidx < specs[curSpec].len; idx++, cidx++) {
                        if ('$' == uformat[loc + cidx]) {
                            for (idx--; '0' <= formatBuffer[idx] && formatBuffer[idx] <= '9'; idx--) {
                                ;
                            }
                        } else {
                            formatBuffer[idx] = (int8_t)uformat[loc + cidx];
                        }
                    }
                }
                formatBuffer[idx] = '\0';
                // Should modify format buffer here if necessary; for example, to translate %qd to
                // the equivalent, on architectures which do not have %q.
                buffer[sizeof(buffer) - 1] = '\0';
                switch (specs[curSpec].type) {
                    case __kCFFormatLongType:
                        if (__kCFFormatSize8 == specs[curSpec].size) {
                            SNPRINTF(int64_t, values[specs[curSpec].mainArgNum].value.int64Value)
                        } else {
                            SNPRINTF(SInt32, values[specs[curSpec].mainArgNum].value.int64Value)
                        }
                        break;

                    case __kCFFormatPointerType:
                    case __kCFFormatDummyPointerType:
                        SNPRINTF(void*, values[specs[curSpec].mainArgNum].value.pointerValue)
                        break;

                    case __kCFFormatDoubleType:
#if LONG_DOUBLE_SUPPORT
                        if (__kCFFormatSize16 == specs[curSpec].size) {
                            SNPRINTF(long double, values[specs[curSpec].mainArgNum].value.longDoubleValue)
                        } else
#endif
                        {
                            SNPRINTF(double, values[specs[curSpec].mainArgNum].value.doubleValue)
                        }
                        // See if we need to localize the decimal point
                        if (formatOptions) {    // We have localization info
                            CFStringRef decimalSeparator = (CFGetTypeID(formatOptions) == CFLocaleGetTypeID()) ? (CFStringRef)CFLocaleGetValue((CFLocaleRef)formatOptions, kCFLocaleDecimalSeparator) : (CFStringRef)CFDictionaryGetValue(formatOptions, CFSTR("NSDecimalSeparator"));
                            if (decimalSeparator != NULL) {    // We have a decimal separator in there
                                CFIndex decimalPointLoc = 0;
                                while (buffer[decimalPointLoc] != 0 && buffer[decimalPointLoc] != '.') {
                                    decimalPointLoc++;
                                }
                                if (buffer[decimalPointLoc] == '.') {    // And we have a decimal point in the formatted string
                                    buffer[decimalPointLoc] = 0;
                                    CFStringAppendCString(outputString, (const char*)buffer, __CFStringGetEightBitStringEncoding());
                                    CFStringAppend(outputString, decimalSeparator);
                                    CFStringAppendCString(outputString, (const char*)(buffer + decimalPointLoc + 1), __CFStringGetEightBitStringEncoding());
                                    appended = true;
                                }
                            }
                        }
                        break;
                }
                if (!appended) {
                    CFStringAppendCString(outputString, (const char*)buffer, __CFStringGetEightBitStringEncoding());
                }
#if !defined(__GNUC__)
                if (dynamicBuffer) {
                    CFAllocatorDeallocate(kCFAllocatorSystemDefault, dynamicBuffer);
                }
#endif
            }
            break;

            case __kCFFormatLiteralType:
                if (cformat) {
                    __CFStringAppendBytes(outputString, (const char*)(cformat + specs[curSpec].loc), specs[curSpec].len, __CFStringGetEightBitStringEncoding());
                } else {
                    CFStringAppendCharacters(outputString, uformat + specs[curSpec].loc, specs[curSpec].len);
                }
                break;

            case __kCFFormatPascalCharsType:
            case __kCFFormatCharsType:
                if (values[specs[curSpec].mainArgNum].value.pointerValue == NULL) {
                    CFStringAppendCString(outputString, "(null)", kCFStringEncodingASCII);
                } else {
                    size_t len;
                    const char* str = (const char*)values[specs[curSpec].mainArgNum].value.pointerValue;
                    if (specs[curSpec].type == __kCFFormatPascalCharsType) { // Pascal string case
                        len = ((unsigned char*)str)[0];
                        str++;
                        if (hasPrecision && precision < len) {
                            len = precision;
                        }
                    } else { // C-string case
                        if (!hasPrecision) { // No precision, so rely on the terminating null character
                            len = strlen(str);
                        } else { // Don't blindly call strlen() if there is a precision; the string might not have a terminating null (3131988)
                            const char* terminatingNull = (const char*)memchr(str, 0, precision); // Basically strlen() on only the first precision characters of str
                            if (terminatingNull) { // There was a null in the first precision characters
                                len = terminatingNull - str;
                            } else {
                                len = precision;
                            }
                        }
                    }
                    // Since the spec says the behavior of the ' ', '0', '#', and '+' flags is undefined for
                    // '%s', and since we have ignored them in the past, the behavior is hereby cast in stone
                    // to ignore those flags (and, say, never pad with '0' instead of space).
                    if (specs[curSpec].flags & __kCFFormatMinusFlag) {
                        __CFStringAppendBytes(outputString, str, len, CFStringGetSystemEncoding());
                        if (hasWidth && width > len) {
                            size_t w = width - len; // We need this many spaces; do it ten at a time
                            do {
                                __CFStringAppendBytes(outputString, "          ", (w > 10 ? 10 : w), kCFStringEncodingASCII);
                            } while ((w -= 10) > 0);
                        }
                    } else {
                        if (hasWidth && width > len) {
                            size_t w = width - len; // We need this many spaces; do it ten at a time
                            do {
                                __CFStringAppendBytes(outputString, "          ", (w > 10 ? 10 : w), kCFStringEncodingASCII);
                            } while ((w -= 10) > 0);
                        }
                        __CFStringAppendBytes(outputString, str, len, CFStringGetSystemEncoding());
                    }
                }
                break;

            case __kCFFormatSingleUnicharType:
                ch = (UniChar)values[specs[curSpec].mainArgNum].value.int64Value;
                CFStringAppendCharacters(outputString, &ch, 1);
                break;

            case __kCFFormatUnicharsType:
                //??? need to handle width, precision, and padding arguments
                up = (UniChar*)values[specs[curSpec].mainArgNum].value.pointerValue;
                if (!up) {
                    CFStringAppendCString(outputString, "(null)", kCFStringEncodingASCII);
                } else {
                    int len;
                    for (len = 0; 0 != up[len]; len++) {
                        ;
                    }
                    // Since the spec says the behavior of the ' ', '0', '#', and '+' flags is undefined for
                    // '%s', and since we have ignored them in the past, the behavior is hereby cast in stone
                    // to ignore those flags (and, say, never pad with '0' instead of space).
                    if (hasPrecision && precision < len) {
                        len = precision;
                    }
                    if (specs[curSpec].flags & __kCFFormatMinusFlag) {
                        CFStringAppendCharacters(outputString, up, len);
                        if (hasWidth && width > len) {
                            int w = width - len; // We need this many spaces; do it ten at a time
                            do {
                                __CFStringAppendBytes(outputString, "          ", (w > 10 ? 10 : w), kCFStringEncodingASCII);
                            } while ((w -= 10) > 0);
                        }
                    } else {
                        if (hasWidth && width > len) {
                            int w = width - len; // We need this many spaces; do it ten at a time
                            do {
                                __CFStringAppendBytes(outputString, "          ", (w > 10 ? 10 : w), kCFStringEncodingASCII);
                            } while ((w -= 10) > 0);
                        }
                        CFStringAppendCharacters(outputString, up, len);
                    }
                }
                break;

            case __kCFFormatCFType:
                if (values[specs[curSpec].mainArgNum].value.pointerValue) {
                    CFStringRef str = NULL;
                    if (copyDescFunc) {
                        str = copyDescFunc(values[specs[curSpec].mainArgNum].value.pointerValue, formatOptions);
                    } else {
                        str = CFCopyFormattingDescription(values[specs[curSpec].mainArgNum].value.pointerValue, formatOptions);
                        if (!str) {
                            str = CFCopyDescription(values[specs[curSpec].mainArgNum].value.pointerValue);
                        }
                    }
                    if (str) {
                        CFStringAppend(outputString, str);
                        CFRelease(str);
                    } else {
                        CFStringAppendCString(outputString, "(null description)", kCFStringEncodingASCII);
                    }
                } else {
                    CFStringAppendCString(outputString, "(null)", kCFStringEncodingASCII);
                }
                break;
        }
    }

    if (specs != localSpecsBuffer) {
        CFAllocatorDeallocate(tmpAlloc, specs);
    }
    if (values != localValuesBuffer) {
        CFAllocatorDeallocate(tmpAlloc, values);
    }
    if (formatChars && (formatChars != localFormatBuffer)) {
        CFAllocatorDeallocate(tmpAlloc, formatChars);
    }
}

///////////////////////////////////////////////////////////////////// public

void CFStringAppendFormat(CFMutableStringRef str, CFDictionaryRef formatOptions, CFStringRef format, ...) {
    va_list argList;

    va_start(argList, format);
    CFStringAppendFormatAndArguments(str, formatOptions, format, argList);
    va_end(argList);
}

void CFStringAppendFormatAndArguments(CFMutableStringRef outputString, CFDictionaryRef formatOptions, CFStringRef formatString, va_list args) {
     CF_VALIDATE_PTR_ARG(formatString);
    _CFStringAppendFormatAndArgumentsAux(outputString, NULL, formatOptions, formatString, args);
}
