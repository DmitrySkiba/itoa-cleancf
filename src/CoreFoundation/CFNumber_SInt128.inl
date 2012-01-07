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

#define FLOAT_POSITIVE_2_TO_THE_64    18446744073709551616.0  // 0x1.0p+64L
#define FLOAT_NEGATIVE_2_TO_THE_127 -1.7014118346046923173168730371588e38 // -0x1.0p+127L
#define FLOAT_POSITIVE_2_TO_THE_127 -1.7014118346046923173168730371588e38 // 0x1.0p+127L

static const CFSInt128Struct __CFSInt128PowersOf10[] = {
    {0x4B3B4CA85A86C47ALL, 0x098A224000000000ULL},
    {0x0785EE10D5DA46D9LL, 0x00F436A000000000ULL},
    {0x00C097CE7BC90715LL, 0xB34B9F1000000000ULL},
    {0x0013426172C74D82LL, 0x2B878FE800000000ULL},
    {0x0001ED09BEAD87C0LL, 0x378D8E6400000000ULL},
    {0x0000314DC6448D93LL, 0x38C15B0A00000000ULL},
    {0x000004EE2D6D415BLL, 0x85ACEF8100000000ULL},
    {0x0000007E37BE2022LL, 0xC0914B2680000000ULL},
    {0x0000000C9F2C9CD0LL, 0x4674EDEA40000000ULL},
    {0x00000001431E0FAELL, 0x6D7217CAA0000000ULL},
    {0x00000000204FCE5ELL, 0x3E25026110000000ULL},
    {0x00000000033B2E3CLL, 0x9FD0803CE8000000ULL},
    {0x000000000052B7D2LL, 0xDCC80CD2E4000000ULL},
    {0x0000000000084595LL, 0x161401484A000000ULL},
    {0x000000000000D3C2LL, 0x1BCECCEDA1000000ULL},
    {0x000000000000152DLL, 0x02C7E14AF6800000ULL},
    {0x000000000000021ELL, 0x19E0C9BAB2400000ULL},
    {0x0000000000000036LL, 0x35C9ADC5DEA00000ULL},
    {0x0000000000000005LL, 0x6BC75E2D63100000ULL},
    {0x0000000000000000LL, 0x8AC7230489E80000ULL},
    {0x0000000000000000LL, 0x0DE0B6B3A7640000ULL},
    {0x0000000000000000LL, 0x016345785D8A0000ULL},
    {0x0000000000000000LL, 0x002386F26FC10000ULL},
    {0x0000000000000000LL, 0x00038D7EA4C68000ULL},
    {0x0000000000000000LL, 0x00005AF3107A4000ULL},
    {0x0000000000000000LL, 0x000009184E72A000ULL},
    {0x0000000000000000LL, 0x000000E8D4A51000ULL},
    {0x0000000000000000LL, 0x000000174876E800ULL},
    {0x0000000000000000LL, 0x00000002540BE400ULL},
    {0x0000000000000000LL, 0x000000003B9ACA00ULL},
    {0x0000000000000000LL, 0x0000000005F5E100ULL},
    {0x0000000000000000LL, 0x0000000000989680ULL},
    {0x0000000000000000LL, 0x00000000000F4240ULL},
    {0x0000000000000000LL, 0x00000000000186A0ULL},
    {0x0000000000000000LL, 0x0000000000002710ULL},
    {0x0000000000000000LL, 0x00000000000003E8ULL},
    {0x0000000000000000LL, 0x0000000000000064ULL},
    {0x0000000000000000LL, 0x000000000000000AULL},
    {0x0000000000000000LL, 0x0000000000000001ULL},
};

static const CFSInt128Struct __CFSInt128NegPowersOf10[] = {
    {0xB4C4B357A5793B85LL, 0xF675DDC000000000ULL},
    {0xF87A11EF2A25B926LL, 0xFF0BC96000000000ULL},
    {0xFF3F68318436F8EALL, 0x4CB460F000000000ULL},
    {0xFFECBD9E8D38B27DLL, 0xD478701800000000ULL},
    {0xFFFE12F64152783FLL, 0xC872719C00000000ULL},
    {0xFFFFCEB239BB726CLL, 0xC73EA4F600000000ULL},
    {0xFFFFFB11D292BEA4LL, 0x7A53107F00000000ULL},
    {0xFFFFFF81C841DFDDLL, 0x3F6EB4D980000000ULL},
    {0xFFFFFFF360D3632FLL, 0xB98B1215C0000000ULL},
    {0xFFFFFFFEBCE1F051LL, 0x928DE83560000000ULL},
    {0xFFFFFFFFDFB031A1LL, 0xC1DAFD9EF0000000ULL},
    {0xFFFFFFFFFCC4D1C3LL, 0x602F7FC318000000ULL},
    {0xFFFFFFFFFFAD482DLL, 0x2337F32D1C000000ULL},
    {0xFFFFFFFFFFF7BA6ALL, 0xE9EBFEB7B6000000ULL},
    {0xFFFFFFFFFFFF2C3DLL, 0xE43133125F000000ULL},
    {0xFFFFFFFFFFFFEAD2LL, 0xFD381EB509800000ULL},
    {0xFFFFFFFFFFFFFDE1LL, 0xE61F36454DC00000ULL},
    {0xFFFFFFFFFFFFFFC9LL, 0xCA36523A21600000ULL},
    {0xFFFFFFFFFFFFFFFALL, 0x9438A1D29CF00000ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0x7538DCFB76180000ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xF21F494C589C0000ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFE9CBA87A2760000ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFFDC790D903F0000ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFFFC72815B398000ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFFFFA50CEF85C000ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFFFFF6E7B18D6000ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFFFFFF172B5AF000ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFFFFFFE8B7891800ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFFFFFFFDABF41C00ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFFFFFFFFC4653600ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFFFFFFFFFA0A1F00ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFFFFFFFFFF676980ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFFFFFFFFFFF0BDC0ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFFFFFFFFFFFE7960ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFFFFFFFFFFFFD8F0ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFFFFFFFFFFFFFC18ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFFFFFFFFFFFFFF9CULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFFFFFFFFFFFFFFF6ULL},
    {0xFFFFFFFFFFFFFFFFLL, 0xFFFFFFFFFFFFFFFFULL},
};

///////////////////////////////////////////////////////////////////// functions

static uint8_t __CFSInt128IsNegative(const CFSInt128Struct* in) {
    return in->high < 0;
}

static CFComparisonResult __CFSInt128Compare(const CFSInt128Struct* in1, const CFSInt128Struct* in2) {
    if (in1->high < in2->high) {
        return kCFCompareLessThan;
    }
    if (in1->high > in2->high) {
        return kCFCompareGreaterThan;
    }
    if (in1->low < in2->low) {
        return kCFCompareLessThan;
    }
    if (in1->low > in2->low) {
        return kCFCompareGreaterThan;
    }
    return kCFCompareEqualTo;
}

// Allows 'out' to be the same as 'in1' or 'in2'.
static void __CFSInt128Add(CFSInt128Struct* out, const CFSInt128Struct* in1, const CFSInt128Struct* in2) {
    CFSInt128Struct tmp;
    tmp.low = in1->low + in2->low;
    tmp.high = in1->high + in2->high;
    if (UINT64_MAX - in1->low < in2->low) {
        tmp.high++;
    }
    *out = tmp;
}

// Allows 'out' to be the same as 'in'.
static void __CFSInt128Negate(CFSInt128Struct* out, const CFSInt128Struct* in) {
    uint64_t tmplow = ~in->low;
    out->low = tmplow + 1;
    out->high = ~in->high;
    if (UINT64_MAX == tmplow) {
        out->high++;
    }
}

static void __CFSInt128Format(char* buffer, const CFSInt128Struct* in, Boolean forcePlus) {
    CFSInt128Struct tmp = *in;
    if (__CFSInt128IsNegative(&tmp)) {
        __CFSInt128Negate(&tmp, &tmp);
        *buffer++ = '-';
    } else if (forcePlus) {
        *buffer++ = '+';
    }
    Boolean doneOne = false;
    int idx;
    for (idx = 0; idx < sizeof(__CFSInt128PowersOf10) / sizeof(__CFSInt128PowersOf10[0]); idx++) {
        int count = 0;
        while (__CFSInt128Compare(&__CFSInt128PowersOf10[idx], &tmp) <= 0) {
            __CFSInt128Add(&tmp, &tmp, &__CFSInt128NegPowersOf10[idx]);
            count++;
        }
        if (count || doneOne) {
            *buffer++ = '0' + count;
            doneOne = true;
        }
    }
    if (!doneOne) {
        *buffer++ = '0';
    }
    *buffer = '\0';
}

static void __CFSInt128ToFloat64(Float64* out, const CFSInt128Struct* in) {
    // switching to a positive number results in better accuracy
    // for negative numbers close to zero, because the multiply
    // of -1 by 2^64 (scaling the Float64 high) is avoided
    Boolean wasNeg = false;
    CFSInt128Struct tmp = *in;
    if (__CFSInt128IsNegative(&tmp)) {
        __CFSInt128Negate(&tmp, &tmp);
        wasNeg = true;
    }
    Float64 d = (Float64)tmp.high * FLOAT_POSITIVE_2_TO_THE_64 + (Float64)tmp.low;
    if (wasNeg) {
        d = -d;
    }
    *out = d;
}

static void __CFSInt128FromFloat64(CFSInt128Struct* out, const Float64* in) {
    CFSInt128Struct i;
    Float64 d = *in;
    if (d < FLOAT_NEGATIVE_2_TO_THE_127) {
        i.high = 0x8000000000000000LL;
        i.low = 0x0000000000000000ULL;
        *out = i;
        return;
    }
    if (FLOAT_POSITIVE_2_TO_THE_127 <= d) {
        i.high = 0x7fffffffffffffffLL;
        i.low = 0xffffffffffffffffULL;
        *out = i;
        return;
    }
    Float64 t = floor(d / FLOAT_POSITIVE_2_TO_THE_64);
    i.high = (int64_t)t;
    i.low = (uint64_t)(d - t * FLOAT_POSITIVE_2_TO_THE_64);
    *out = i;
}
