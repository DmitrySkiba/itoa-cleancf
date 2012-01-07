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
#include <math.h>
#include <float.h>

#include "CFNumber_SInt128.inl"

#define CF_VALIDATE_NUMBER_ARG(cf) \
    CF_VALIDATE_OBJECT_ARG(CFObjC, cf, __kCFNumberTypeID)

#define CF_VALIDATE_NUMBERTYPE_ARG(type) \
    CF_VALIDATE_ARG( \
        (0 < type && type <= kCFNumberMaxType) || \
            (type == kCFNumberSInt128Type), \
        "argument %s (%d) must be valid number type", #type, type)


/* The IEEE bit patterns... Also have:
 * 0x7f800000    float +Inf
 * 0x7fc00000    float NaN
 * 0xff800000    float -Inf
 */
#define DOUBLE_NAN_BITS ((uint64_t)0x7ff8000000000000ULL)
#define DOUBLE_POSINF_BITS ((uint64_t)0x7ff0000000000000ULL)
#define DOUBLE_NEGINF_BITS ((uint64_t)0xfff0000000000000ULL)

/* CFNumber */

static const struct {
    uint16_t canonicalType: 5; // canonical fixed-width type
    uint16_t floatBit: 1;      // is float
    uint16_t storageBit: 1;    // storage size (0: (float ? 4 : 8), 1: (float ? 8 : 16) bits)
    uint16_t lgByteSize: 3;    // base-2 log byte size of public type
    uint16_t unused: 6;
}
__CFNumberTypeTable[] = {

    #define LP64ENTRY(NumberTypePrefix, CType, floatBit) \
        (sizeof(CType) == 4) ? \
            NumberTypePrefix ## 32Type : \
            NumberTypePrefix ## 64Type, \
        floatBit, \
        (floatBit && sizeof(CType) == 8) ? 1 : 0, \
        (sizeof(CType) == 4) ? 2 : 3, \
        0

    /*****/

    /* 0 */                     {0, 0, 0, 0},

    /* kCFNumberSInt8Type */    {kCFNumberSInt8Type, 0, 0, 0, 0},
    /* kCFNumberSInt16Type */   {kCFNumberSInt16Type, 0, 0, 1, 0},
    /* kCFNumberSInt32Type */   {kCFNumberSInt32Type, 0, 0, 2, 0},
    /* kCFNumberSInt64Type */   {kCFNumberSInt64Type, 0, 0, 3, 0},
    /* kCFNumberFloat32Type */  {kCFNumberFloat32Type, 1, 0, 2, 0},
    /* kCFNumberFloat64Type */  {kCFNumberFloat64Type, 1, 1, 3, 0},

    /* kCFNumberCharType */     {kCFNumberSInt8Type, 0, 0, 0, 0},
    /* kCFNumberShortType */    {kCFNumberSInt16Type, 0, 0, 1, 0},
    /* kCFNumberIntType */      {kCFNumberSInt32Type, 0, 0, 2, 0},

    /* kCFNumberLongType */     {LP64ENTRY(kCFNumberSInt, long, 0)},
    /* kCFNumberLongLongType */ {kCFNumberSInt64Type, 0, 0, 3, 0},
    /* kCFNumberFloatType */    {kCFNumberFloat32Type, 1, 0, 2, 0},
    /* kCFNumberDoubleType */   {kCFNumberFloat64Type, 1, 1, 3, 0},
    /* kCFNumberCFIndexType */  {LP64ENTRY(kCFNumberSInt, int, 0)},
    /* kCFNumberNSIntegerType */{LP64ENTRY(kCFNumberSInt, int, 0)},
    /* kCFNumberCGFloatType */  {LP64ENTRY(kCFNumberFloat, int, 1)},

    /* kCFNumberSInt128Type */  {kCFNumberSInt128Type, 0, 1, 4, 0},

    /*****/

    #undef LP64ENTRY
};

struct __CFNumber {
    CFRuntimeBase _base;
    uint64_t _pad; // need this space here for the constant objects
    /* 0 or 8 more bytes allocated here */
};

static struct __CFNumber __kCFNumberNaN = {
    INIT_CFRUNTIME_BASE(), 0ULL
};

static struct __CFNumber __kCFNumberNegativeInfinity = {
    INIT_CFRUNTIME_BASE(), 0ULL
};

static struct __CFNumber __kCFNumberPositiveInfinity = {
    INIT_CFRUNTIME_BASE(), 0ULL
};

#define MinCachedInt  (-1)
#define MaxCachedInt  (12)
#define NotToBeCached (MinCachedInt - 1)
// Storing CFNumberRefs for range MinCachedInt..MaxCachedInt
static CFNumberRef __CFNumberCache[MaxCachedInt - MinCachedInt + 1] = {NULL};

static CFTypeID __kCFNumberTypeID = _kCFRuntimeNotATypeID;

///////////////////////////////////////////////////////////////////// private

CF_INLINE CFNumberType __CFNumberGetType(CFNumberRef num) {
    return _CFBitfieldGetValue(CF_INFO(num), 4, 0);
}
CF_INLINE void __CFNumberSetType(CFNumberRef num, CFNumberType type) {
    _CFBitfieldSetValue(CF_INFO(num), 4, 0, (uint8_t)type);
}

// Returns false if the output value is not the same as the number's value, which
//  can occur due to accuracy loss and the value not being within the target range.
static Boolean __CFNumberGetValue(CFNumberRef number, CFNumberType type, void* valuePtr) {

    #define CVT(SRC_TYPE, DST_TYPE, DST_MIN, DST_MAX) \
        do { \
            SRC_TYPE sv; \
            memmove(&sv, data, sizeof(SRC_TYPE)); \
            DST_TYPE dv = (sv < DST_MIN) ? \
                (DST_TYPE)(DST_MIN) : \
                (DST_TYPE)((DST_MAX < sv) ? DST_MAX : sv); \
            memmove(valuePtr, &dv, sizeof(DST_TYPE)); \
            SRC_TYPE vv = (SRC_TYPE)dv; \
            return (vv == sv); \
        } while (0)

    #define CVT128ToInt(SRC_TYPE, DST_TYPE, DST_MIN, DST_MAX) \
        do { \
            SRC_TYPE sv; \
            memmove(&sv, data, sizeof(SRC_TYPE)); \
            DST_TYPE dv; Boolean noLoss = false; \
            if (0 < sv.high || (!sv.high && (int64_t)DST_MAX < sv.low)) { \
                dv = DST_MAX; \
            } else if (sv.high < -1 || (-1 == sv.high && sv.low < (int64_t)DST_MIN)) { \
                dv = DST_MIN; \
            } else { \
                dv = (DST_TYPE)sv.low; \
                noLoss = true; \
            } \
            memmove(valuePtr, &dv, sizeof(DST_TYPE)); \
            return noLoss; \
        } while (0)

    /*****/

    type = __CFNumberTypeTable[type].canonicalType;
    CFNumberType ntype = __CFNumberGetType(number);
    const void* data = &(number->_pad);
    switch (type) {
        case kCFNumberSInt8Type:
            if (__CFNumberTypeTable[ntype].floatBit) {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT(Float32, int8_t, INT8_MIN, INT8_MAX);
                } else {
                    CVT(Float64, int8_t, INT8_MIN, INT8_MAX);
                }
            } else {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT(int64_t, int8_t, INT8_MIN, INT8_MAX);
                } else {
                    CVT128ToInt(CFSInt128Struct, int8_t, INT8_MIN, INT8_MAX);
                }
            }
            return true;
        case kCFNumberSInt16Type:
            if (__CFNumberTypeTable[ntype].floatBit) {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT(Float32, int16_t, INT16_MIN, INT16_MAX);
                } else {
                    CVT(Float64, int16_t, INT16_MIN, INT16_MAX);
                }
            } else {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT(int64_t, int16_t, INT16_MIN, INT16_MAX);
                } else {
                    CVT128ToInt(CFSInt128Struct, int16_t, INT16_MIN, INT16_MAX);
                }
            }
            return true;
        case kCFNumberSInt32Type:
            if (__CFNumberTypeTable[ntype].floatBit) {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT(Float32, int32_t, INT32_MIN, INT32_MAX);
                } else {
                    CVT(Float64, int32_t, INT32_MIN, INT32_MAX);
                }
            } else {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT(int64_t, int32_t, INT32_MIN, INT32_MAX);
                } else {
                    CVT128ToInt(CFSInt128Struct, int32_t, INT32_MIN, INT32_MAX);
                }
            }
            return true;
        case kCFNumberSInt64Type:
            if (__CFNumberTypeTable[ntype].floatBit) {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT(Float32, int64_t, INT64_MIN, INT64_MAX);
                } else {
                    CVT(Float64, int64_t, INT64_MIN, INT64_MAX);
                }
            } else {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    memmove(valuePtr, data, 8);
                } else {
                    CVT128ToInt(CFSInt128Struct, int64_t, INT64_MIN, INT64_MAX);
                }
            }
            return true;
        case kCFNumberSInt128Type:
            if (__CFNumberTypeTable[ntype].floatBit) {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    Float32 f;
                    memmove(&f, data, 4);
                    Float64 d = f;
                    CFSInt128Struct i;
                    __CFSInt128FromFloat64(&i, &d);
                    memmove(valuePtr, &i, 16);
                    Float64 d2;
                    __CFSInt128ToFloat64(&d2, &i);
                    Float32 f2 = (Float32)d2;
                    return (f2 == f);
                } else {
                    Float64 d;
                    memmove(&d, data, 8);
                    CFSInt128Struct i;
                    __CFSInt128FromFloat64(&i, &d);
                    memmove(valuePtr, &i, 16);
                    Float64 d2;
                    __CFSInt128ToFloat64(&d2, &i);
                    return (d2 == d);
                }
            } else {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    int64_t j;
                    memmove(&j, data, 8);
                    CFSInt128Struct i;
                    i.low = j;
                    i.high = (j < 0) ? -1LL : 0LL;
                    memmove(valuePtr, &i, 16);
                } else {
                    memmove(valuePtr, data, 16);
                }
            }
            return true;
        case kCFNumberFloat32Type:
            if (__CFNumberTypeTable[ntype].floatBit) {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    memmove(valuePtr, data, 4);
                } else {
                    double d;
                    memmove(&d, data, 8);
                    if (isnan(d)) {
                        uint32_t l = 0x7fc00000;
                        memmove(valuePtr, &l, 4);
                        return true;
                    } else if (isinf(d)) {
                        uint32_t l = 0x7f800000;
                        if (d < 0.0) {
                            l += 0x80000000UL;
                        }
                        memmove(valuePtr, &l, 4);
                        return true;
                    }
                    CVT(Float64, Float32, -FLT_MAX, FLT_MAX);
                }
            } else {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT(int64_t, Float32, -FLT_MAX, FLT_MAX);
                } else {
                    CFSInt128Struct i;
                    memmove(&i, data, 16);
                    Float64 d;
                    __CFSInt128ToFloat64(&d, &i);
                    Float32 f = (Float32)d;
                    memmove(valuePtr, &f, 4);
                    d = f;
                    CFSInt128Struct i2;
                    __CFSInt128FromFloat64(&i2, &d);
                    return __CFSInt128Compare(&i2, &i) == kCFCompareEqualTo;
                }
            }
            return true;
        case kCFNumberFloat64Type:
            if (__CFNumberTypeTable[ntype].floatBit) {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    float f;
                    memmove(&f, data, 4);
                    if (isnan(f)) {
                        uint64_t l = DOUBLE_NAN_BITS;
                        memmove(valuePtr, &l, 8);
                        return true;
                    } else if (isinf(f)) {
                        uint64_t l = DOUBLE_POSINF_BITS;
                        if (f < 0.0) {
                            l += 0x8000000000000000ULL;
                        }
                        memmove(valuePtr, &l, 8);
                        return true;
                    }
                    CVT(Float32, Float64, -DBL_MAX, DBL_MAX);
                } else {
                    memmove(valuePtr, data, 8);
                }
            } else {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT(int64_t, Float64, -DBL_MAX, DBL_MAX);
                } else {
                    CFSInt128Struct i;
                    memmove(&i, data, 16);
                    Float64 d;
                    __CFSInt128ToFloat64(&d, &i);
                    memmove(valuePtr, &d, 8);
                    CFSInt128Struct i2;
                    __CFSInt128FromFloat64(&i2, &d);
                    return __CFSInt128Compare(&i2, &i) == kCFCompareEqualTo;
                }
            }
            return true;
    }
    return false;

    /*****/

    #undef CVT
    #undef CVT128ToInt
}

// This has the old cast-style behavior.
static Boolean __CFNumberGetValueCompat(CFNumberRef number, CFNumberType type, void* valuePtr) {

    #define CVT_COMPAT(SRC_TYPE, DST_TYPE, FT) \
        do { \
            SRC_TYPE sv; \
            memmove(&sv, data, sizeof(SRC_TYPE)); \
            DST_TYPE dv = (DST_TYPE)(sv); \
            memmove(valuePtr, &dv, sizeof(DST_TYPE)); \
            SRC_TYPE vv = (SRC_TYPE)dv; \
            return (FT) || (vv == sv); \
        } while (0)

    #define CVT128ToInt_COMPAT(SRC_TYPE, DST_TYPE) \
        do { \
            SRC_TYPE sv; \
            memmove(&sv, data, sizeof(SRC_TYPE)); \
            DST_TYPE dv; dv = (DST_TYPE)sv.low; \
            memmove(valuePtr, &dv, sizeof(DST_TYPE)); \
            uint64_t vv = (uint64_t)dv; \
            return (vv == sv.low); \
        } while (0)

    /*****/

    type = __CFNumberTypeTable[type].canonicalType;
    CFNumberType ntype = __CFNumberGetType(number);
    const void* data = &(number->_pad);
    switch (type) {
        case kCFNumberSInt8Type:
            if (__CFNumberTypeTable[ntype].floatBit) {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT_COMPAT(Float32, int8_t, 0);
                } else {
                    CVT_COMPAT(Float64, int8_t, 0);
                }
            } else {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT_COMPAT(int64_t, int8_t, 1);
                } else {
                    CVT128ToInt_COMPAT(CFSInt128Struct, int8_t);
                }
            }
            return true;
        case kCFNumberSInt16Type:
            if (__CFNumberTypeTable[ntype].floatBit) {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT_COMPAT(Float32, int16_t, 0);
                } else {
                    CVT_COMPAT(Float64, int16_t, 0);
                }
            } else {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT_COMPAT(int64_t, int16_t, 1);
                } else {
                    CVT128ToInt_COMPAT(CFSInt128Struct, int16_t);
                }
            }
            return true;
        case kCFNumberSInt32Type:
            if (__CFNumberTypeTable[ntype].floatBit) {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT_COMPAT(Float32, int32_t, 0);
                } else {
                    CVT_COMPAT(Float64, int32_t, 0);
                }
            } else {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT_COMPAT(int64_t, int32_t, 0);
                } else {
                    CVT128ToInt_COMPAT(CFSInt128Struct, int32_t);
                }
            }
            return true;
        case kCFNumberSInt64Type:
            if (__CFNumberTypeTable[ntype].floatBit) {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT_COMPAT(Float32, int64_t, 0);
                } else {
                    CVT_COMPAT(Float64, int64_t, 0);
                }
            } else {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT_COMPAT(int64_t, int64_t, 0);
                } else {
                    CVT128ToInt_COMPAT(CFSInt128Struct, int64_t);
                }
            }
            return true;
        case kCFNumberSInt128Type:
            if (__CFNumberTypeTable[ntype].floatBit) {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    Float32 f;
                    memmove(&f, data, 4);
                    Float64 d = f;
                    CFSInt128Struct i;
                    __CFSInt128FromFloat64(&i, &d);
                    memmove(valuePtr, &i, 16);
                    Float64 d2;
                    __CFSInt128ToFloat64(&d2, &i);
                    Float32 f2 = (Float32)d2;
                    return (f2 == f);
                } else {
                    Float64 d;
                    memmove(&d, data, 8);
                    CFSInt128Struct i;
                    __CFSInt128FromFloat64(&i, &d);
                    memmove(valuePtr, &i, 16);
                    Float64 d2;
                    __CFSInt128ToFloat64(&d2, &i);
                    return (d2 == d);
                }
            } else {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    int64_t j;
                    memmove(&j, data, 8);
                    CFSInt128Struct i;
                    i.low = j;
                    i.high = (j < 0) ? -1LL : 0LL;
                    memmove(valuePtr, &i, 16);
                } else {
                    memmove(valuePtr, data, 16);
                }
            }
            return true;
        case kCFNumberFloat32Type:
            if (__CFNumberTypeTable[ntype].floatBit) {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    memmove(valuePtr, data, 4);
                } else {
                    CVT_COMPAT(Float64, Float32, 0);
                }
            } else {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT_COMPAT(int64_t, Float32, 0);
                } else {
                    CFSInt128Struct i;
                    memmove(&i, data, 16);
                    Float64 d;
                    __CFSInt128ToFloat64(&d, &i);
                    Float32 f = (Float32)d;
                    memmove(valuePtr, &f, 4);
                    d = f;
                    CFSInt128Struct i2;
                    __CFSInt128FromFloat64(&i2, &d);
                    return __CFSInt128Compare(&i2, &i) == kCFCompareEqualTo;
                }
            }
            return true;
        case kCFNumberFloat64Type:
            if (__CFNumberTypeTable[ntype].floatBit) {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT_COMPAT(Float32, Float64, 0);
                } else {
                    memmove(valuePtr, data, 8);
                }
            } else {
                if (!__CFNumberTypeTable[ntype].storageBit) {
                    CVT_COMPAT(int64_t, Float64, 0);
                } else {
                    CFSInt128Struct i;
                    memmove(&i, data, 16);
                    Float64 d;
                    __CFSInt128ToFloat64(&d, &i);
                    memmove(valuePtr, &d, 8);
                    CFSInt128Struct i2;
                    __CFSInt128FromFloat64(&i2, &d);
                    return __CFSInt128Compare(&i2, &i) == kCFCompareEqualTo;
                }
            }
            return true;
    }
    return false;

    /*****/

    #undef CVT_COMPAT
    #undef CVT128ToInt_COMPAT
}

/* CFNumber class */

static Boolean __CFNumberEqual(CFTypeRef cf1, CFTypeRef cf2) {
    return CFNumberCompare((CFNumberRef)cf1, (CFNumberRef)cf2, 0) == kCFCompareEqualTo;
}

static CFHashCode __CFNumberHash(CFTypeRef cf) {
    CFHashCode h;
    CFNumberRef number = (CFNumberRef)cf;
    switch (__CFNumberGetType(number)) {
        case kCFNumberSInt8Type:
        case kCFNumberSInt16Type:
        case kCFNumberSInt32Type: {
            SInt32 i;
            __CFNumberGetValue(number, kCFNumberSInt32Type, &i);
            h = _CFNumberHashInt(i);
            break;
        }
        default: {
            Float64 d;
            __CFNumberGetValue(number, kCFNumberFloat64Type, &d);
            h = _CFNumberHashDouble((double)d);
            break;
        }
    }
    return h;
}

static CFStringRef __CFNumberCopyFormattingDescription(CFTypeRef cf, CFDictionaryRef formatOptions) {
    CFNumberRef number = (CFNumberRef)cf;
    CFNumberType type = __CFNumberGetType(number);
    if (__CFNumberTypeTable[type].floatBit) {
        return _CFNumberCopyFormattingDescriptionAsFloat64(number);
    }
    CFSInt128Struct i;
    __CFNumberGetValue(number, kCFNumberSInt128Type, &i);
    char buffer[128];
    __CFSInt128Format(buffer, &i, false);
    return CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("%s"), buffer);
}

static CFStringRef __CFNumberCopyDescription(CFTypeRef cf) {
    CFNumberRef number = (CFNumberRef)cf;
    CFNumberType type = __CFNumberGetType(number);
    CFMutableStringRef mstr = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
    CFStringAppendFormat(mstr, NULL, CFSTR("<CFNumber %p [%p]>{value = "), cf, CFGetAllocator(cf));
    if (__CFNumberTypeTable[type].floatBit) {
        Float64 d;
        __CFNumberGetValue(number, kCFNumberFloat64Type, &d);
        if (isnan(d)) {
            CFStringAppend(mstr, CFSTR("nan"));
        } else if (isinf(d)) {
            CFStringAppend(mstr, (0.0 < d) ? CFSTR("+infinity") : CFSTR("-infinity"));
        } else if (0.0 == d) {
            CFStringAppend(mstr, (copysign(1.0, d) < 0.0) ? CFSTR("-0.0") : CFSTR("+0.0"));
        } else {
            CFStringAppendFormat(mstr, NULL, CFSTR("%+.*f"), (__CFNumberTypeTable[type].storageBit ? 20 : 10), d);
        }
        const char* typeName = "unknown float";
        switch (type) {
            case kCFNumberFloat32Type: typeName = "kCFNumberFloat32Type"; break;
            case kCFNumberFloat64Type: typeName = "kCFNumberFloat64Type"; break;
        }
        CFStringAppendFormat(mstr, NULL, CFSTR(", type = %s}"), typeName);
    } else {
        CFSInt128Struct i;
        __CFNumberGetValue(number, kCFNumberSInt128Type, &i);
        char buffer[128];
        __CFSInt128Format(buffer, &i, true);
        const char* typeName = "unknown integer";
        switch (type) {
            case kCFNumberSInt8Type: typeName = "kCFNumberSInt8Type"; break;
            case kCFNumberSInt16Type: typeName = "kCFNumberSInt16Type"; break;
            case kCFNumberSInt32Type: typeName = "kCFNumberSInt32Type"; break;
            case kCFNumberSInt64Type: typeName = "kCFNumberSInt64Type"; break;
            case kCFNumberSInt128Type: typeName = "kCFNumberSInt128Type"; break;
        }
        CFStringAppendFormat(mstr, NULL, CFSTR("%s, type = %s}"), buffer, typeName);
    }
    return mstr;
}

static const CFRuntimeClass __CFNumberClass = {
    0,
    "CFNumber",
    NULL, // init
    NULL, // copy
    NULL,
    __CFNumberEqual,
    __CFNumberHash,
    __CFNumberCopyFormattingDescription,
    __CFNumberCopyDescription
};

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL CFStringRef _CFNumberCopyFormattingDescriptionAsFloat64(CFTypeRef cf) {
    Float64 d;
    CFNumberGetValue((CFNumberRef)cf, kCFNumberFloat64Type, &d);
    if (isnan(d)) {
        return (CFStringRef)CFRetain(CFSTR("nan"));
    }
    if (isinf(d)) {
        return (CFStringRef)CFRetain((0.0 < d) ? CFSTR("+infinity") : CFSTR("-infinity"));
    }
    if (0.0 == d) {
        return (CFStringRef)CFRetain(CFSTR("0.0"));
    }
    // if %g is used here, need to use DBL_DIG + 2 on Mac OS X, but %f needs +1
    return CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("%.*g"), DBL_DIG + 2, d);
}

CF_INTERNAL void _CFNumberInitialize(void) {
    __kCFNumberTypeID = _CFRuntimeRegisterClassBridge(&__CFNumberClass, "NSCFNumber");

    _CFRuntimeInitStaticInstance(&__kCFNumberNaN, __kCFNumberTypeID);
    __CFNumberSetType(&__kCFNumberNaN, kCFNumberFloat64Type);
    __kCFNumberNaN._pad = DOUBLE_NAN_BITS;

    _CFRuntimeInitStaticInstance(&__kCFNumberNegativeInfinity, __kCFNumberTypeID);
    __CFNumberSetType(&__kCFNumberNegativeInfinity, kCFNumberFloat64Type);
    __kCFNumberNegativeInfinity._pad = DOUBLE_NEGINF_BITS;

    _CFRuntimeInitStaticInstance(&__kCFNumberPositiveInfinity, __kCFNumberTypeID);
    __CFNumberSetType(&__kCFNumberPositiveInfinity, kCFNumberFloat64Type);
    __kCFNumberPositiveInfinity._pad = DOUBLE_POSINF_BITS;
}

CF_INTERNAL CFNumberType _CFNumberGetType2(CFNumberRef number) {
    CF_VALIDATE_NUMBER_ARG(number);
    return __CFNumberGetType(number);
}

///////////////////////////////////////////////////////////////////// public

const CFNumberRef kCFNumberNaN = &__kCFNumberNaN;
const CFNumberRef kCFNumberNegativeInfinity = &__kCFNumberNegativeInfinity;
const CFNumberRef kCFNumberPositiveInfinity = &__kCFNumberPositiveInfinity;

CFTypeID CFNumberGetTypeID(void) {
    return __kCFNumberTypeID;
}

CFNumberRef CFNumberCreate(CFAllocatorRef allocator, CFNumberType type, const void* valuePtr) {
    CF_VALIDATE_NUMBERTYPE_ARG(type);

    // Look for cases where we can return a cached instance.
    // We only use cached objects if the allocator is the system
    // default allocator, except for the special floating point
    // constant objects, where we return the cached object
    // regardless of allocator, since that is what has always
    // been done (and now must for compatibility).
    if (!allocator) {
        allocator = CFAllocatorGetDefault();
    }
    int64_t valToBeCached = NotToBeCached;

    if (__CFNumberTypeTable[type].floatBit) {
        CFNumberRef cached = NULL;
        if (!__CFNumberTypeTable[type].storageBit) {
            Float32 f = *(Float32*)valuePtr;
            if (isnan(f)) {
                cached = kCFNumberNaN;
            }
            if (isinf(f)) {
                cached = (f < 0.0) ? kCFNumberNegativeInfinity : kCFNumberPositiveInfinity;
            }
        } else {
            Float64 d = *(Float64*)valuePtr;
            if (isnan(d)) {
                cached = kCFNumberNaN;
            }
            if (isinf(d)) {
                cached = (d < 0.0) ? kCFNumberNegativeInfinity : kCFNumberPositiveInfinity;
            }
        }
        if (cached) {
            return (CFNumberRef)CFRetain(cached);
        }
    } else if (kCFAllocatorSystemDefault == allocator) {
        switch (__CFNumberTypeTable[type].canonicalType) {
            case kCFNumberSInt8Type:
            {
                int8_t val = *(int8_t*)valuePtr;
                if (MinCachedInt <= val && val <= MaxCachedInt) {
                    valToBeCached = (int64_t)val;
                }
                break;
            }
            case kCFNumberSInt16Type:
            {
                int16_t val = *(int16_t*)valuePtr;
                if (MinCachedInt <= val && val <= MaxCachedInt) {
                    valToBeCached = (int64_t)val;
                }
                break;
            }
            case kCFNumberSInt32Type:
            {
                int32_t val = *(int32_t*)valuePtr;
                if (MinCachedInt <= val && val <= MaxCachedInt) {
                    valToBeCached = (int64_t)val;
                }
                break;
            }
            case kCFNumberSInt64Type:
            {
                int64_t val = *(int64_t*)valuePtr;
                if (MinCachedInt <= val && val <= MaxCachedInt) {
                    valToBeCached = (int64_t)val;
                }
                break;
            }
        }
        if (NotToBeCached != valToBeCached) {
            CFNumberRef cached = __CFNumberCache[valToBeCached - MinCachedInt]; // Atomic to access the value in the cache
            if (cached) {
                return (CFNumberRef)CFRetain(cached);
            }
        }
    }

    CFIndex size = 8 + ((!__CFNumberTypeTable[type].floatBit && __CFNumberTypeTable[type].storageBit) ? 8 : 0);
    CFNumberRef result = (CFNumberRef)_CFRuntimeCreateInstance(allocator, __kCFNumberTypeID, size, NULL);
    if (!result) {
        return NULL;
    }
    __CFNumberSetType(result, __CFNumberTypeTable[type].canonicalType);

    // for a value to be cached, we already have the value handy
    if (NotToBeCached != valToBeCached) {
        memmove((void*)&result->_pad, &valToBeCached, 8);
        // Put this in the cache unless the cache is already filled (by another thread).  If we do put it in the cache, retain it an extra time for the cache.
        // Note that we don't bother freeing this result and returning the cached value if the cache was filled, since cached CFNumbers are not guaranteed unique.
        // Barrier assures that the number that is placed in the cache is properly formed.
        CFNumberType origType = __CFNumberGetType(result);
        // Force all cached numbers to have the same type, so that the type does not
        // depend on the order and original type in/with which the numbers are created.
        // Forcing the type AFTER it was cached would cause a race condition with other
        // threads pulling the number object out of the cache and using it.
        __CFNumberSetType(result, kCFNumberSInt32Type);
        if (OSAtomicCompareAndSwapPtrBarrier(NULL, CF_CONST_CAST(void*, result), (void* volatile*)&__CFNumberCache[valToBeCached - MinCachedInt])) {
            CFRetain(result);
        } else {
            // Did not cache the number object, put original type back.
            __CFNumberSetType(result, origType);
        }
        return result;
    }

    uint64_t value;
    switch (__CFNumberTypeTable[type].canonicalType) {
        case kCFNumberSInt8Type:   value = (uint64_t)(int64_t)*(int8_t*)valuePtr; goto smallVal;
        case kCFNumberSInt16Type:  value = (uint64_t)(int64_t)*(int16_t*)valuePtr; goto smallVal;
        case kCFNumberSInt32Type:  value = (uint64_t)(int64_t)*(int32_t*)valuePtr; goto smallVal;
        smallVal: memmove((void*)&result->_pad, &value, 8); break;
        case kCFNumberSInt64Type:  memmove((void*)&result->_pad, valuePtr, 8); break;
        case kCFNumberSInt128Type: memmove((void*)&result->_pad, valuePtr, 16); break;
        case kCFNumberFloat32Type: memmove((void*)&result->_pad, valuePtr, 4); break;
        case kCFNumberFloat64Type: memmove((void*)&result->_pad, valuePtr, 8); break;
    }
    return result;
}

CFNumberType CFNumberGetType(CFNumberRef number) {
    CF_OBJC_FUNCDISPATCH(CFNumberType, number, "_cfNumberType");
    CF_VALIDATE_NUMBER_ARG(number);
    CFNumberType type = __CFNumberGetType(number);
    if (type == kCFNumberSInt128Type) {
        // Must hide this type, since it is not public.
        type = kCFNumberSInt64Type;
    }
    return type;
}

CFIndex CFNumberGetByteSize(CFNumberRef number) {
    CF_VALIDATE_NUMBER_ARG(number);
    return 1 << __CFNumberTypeTable[CFNumberGetType(number)].lgByteSize;
}

Boolean CFNumberIsFloatType(CFNumberRef number) {
    CF_VALIDATE_NUMBER_ARG(number);
    return __CFNumberTypeTable[CFNumberGetType(number)].floatBit;
}

Boolean CFNumberGetValue(CFNumberRef number, CFNumberType type, void* valuePtr) {
    CF_OBJC_FUNCDISPATCH(Boolean, number, "_getValue:forType:", valuePtr, __CFNumberTypeTable[type].canonicalType);
    CF_VALIDATE_NUMBER_ARG(number);
    CF_VALIDATE_NUMBERTYPE_ARG(type);
    uint8_t localMemory[128];
    return __CFNumberGetValueCompat(number, type, valuePtr ? valuePtr : localMemory);
}

CFComparisonResult CFNumberCompare(CFNumberRef number1, CFNumberRef number2, void* context) {
    CF_OBJC_FUNCDISPATCH(CFComparisonResult, number1, "compare:", number2);
    CF_OBJC_FUNCDISPATCH(CFComparisonResult, number2, "_reverseCompare:", number1);
    CF_VALIDATE_NUMBER_ARG(number1);
    CF_VALIDATE_NUMBER_ARG(number2);

    CFNumberType type1 = __CFNumberGetType(number1);
    CFNumberType type2 = __CFNumberGetType(number2);
    // Both numbers are integers
    if (!__CFNumberTypeTable[type1].floatBit && !__CFNumberTypeTable[type2].floatBit) {
        CFSInt128Struct i1, i2;
        __CFNumberGetValue(number1, kCFNumberSInt128Type, &i1);
        __CFNumberGetValue(number2, kCFNumberSInt128Type, &i2);
        return __CFSInt128Compare(&i1, &i2);
    }
    // Both numbers are floats
    if (__CFNumberTypeTable[type1].floatBit && __CFNumberTypeTable[type2].floatBit) {
        Float64 d1, d2;
        __CFNumberGetValue(number1, kCFNumberFloat64Type, &d1);
        __CFNumberGetValue(number2, kCFNumberFloat64Type, &d2);
        double s1 = copysign(1.0, d1);
        double s2 = copysign(1.0, d2);
        if (isnan(d1) && isnan(d2)) {
            return kCFCompareEqualTo;
        }
        if (isnan(d1)) {
            return (s2 < 0.0) ? kCFCompareGreaterThan : kCFCompareLessThan;
        }
        if (isnan(d2)) {
            return (s1 < 0.0) ? kCFCompareLessThan : kCFCompareGreaterThan;
        }
        // at this point, we know we don't have any NaNs
        if (s1 < s2) {
            return kCFCompareLessThan;
        }
        if (s2 < s1) {
            return kCFCompareGreaterThan;
        }
        // at this point, we know the signs are the same; do not combine these tests
        if (d1 < d2) {
            return kCFCompareLessThan;
        }
        if (d2 < d1) {
            return kCFCompareGreaterThan;
        }
        return kCFCompareEqualTo;
    }
    // One float, one integer; swap if necessary so number1 is the float
    Boolean swapResult = false;
    if (__CFNumberTypeTable[type2].floatBit) {
        CFNumberRef tmp = number1;
        number1 = number2;
        number2 = tmp;
        swapResult = true;
    }
    // At large integer values, the precision of double is quite low
    // e.g. all values roughly 2^127 +- 2^73 are represented by 1 double, 2^127.
    // If we just used double compare, that would make the 2^73 largest 128-bit
    // integers look equal, so we have to use integer comparison when possible.
    Float64 d1, d2;
    __CFNumberGetValue(number1, kCFNumberFloat64Type, &d1);
    // if the double value is really big, cannot be equal to integer
    // nan d1 will not compare true here
    if (d1 < FLOAT_NEGATIVE_2_TO_THE_127) {
        return !swapResult ? kCFCompareLessThan : kCFCompareGreaterThan;
    }
    if (FLOAT_POSITIVE_2_TO_THE_127 <= d1) {
        return !swapResult ? kCFCompareGreaterThan : kCFCompareLessThan;
    }
    CFSInt128Struct i1, i2;
    __CFNumberGetValue(number1, kCFNumberSInt128Type, &i1);
    __CFNumberGetValue(number2, kCFNumberSInt128Type, &i2);
    CFComparisonResult res = __CFSInt128Compare(&i1, &i2);
    if (kCFCompareEqualTo != res) {
        return !swapResult ? res : -res;
    }
    // now things are equal, but perhaps due to rounding or nan
    if (isnan(d1)) {
        if (__CFSInt128IsNegative(&i2)) {
            return !swapResult ? kCFCompareGreaterThan : kCFCompareLessThan;
        }
        // nan compares less than positive 0 too
        return !swapResult ? kCFCompareLessThan : kCFCompareGreaterThan;
    }
    // at this point, we know we don't have NaN
    double s1 = copysign(1.0, d1);
    double s2 = __CFSInt128IsNegative(&i2) ? -1.0 : 1.0;
    if (s1 < s2) {
        return !swapResult ? kCFCompareLessThan : kCFCompareGreaterThan;
    }
    if (s2 < s1) {
        return !swapResult ? kCFCompareGreaterThan : kCFCompareLessThan;
    }
    // at this point, we know the signs are the same; do not combine these tests
    __CFNumberGetValue(number2, kCFNumberFloat64Type, &d2);
    if (d1 < d2) {
        return !swapResult ? kCFCompareLessThan : kCFCompareGreaterThan;
    }
    if (d2 < d1) {
        return !swapResult ? kCFCompareGreaterThan : kCFCompareLessThan;
    }
    return kCFCompareEqualTo;
}

CFHashCode _CFNumberHashInt(SInt32 i) {
    return ((i > 0) ? (CFHashCode)(i) : (CFHashCode)(-i)) * 2654435761U;
}

CFHashCode _CFNumberHashDouble(double d) {
    double dInt;
    if (d < 0) {
        d = -d;
    }
    dInt = rint(d);
    return (CFHashCode)((2654435761U * (CFHashCode)fmod(dInt, (double)ULONG_MAX)) + ((d - dInt) * ULONG_MAX));
}
