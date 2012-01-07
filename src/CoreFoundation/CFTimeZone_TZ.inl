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


///////////////////////////////////////////////////////////////////// TZ

const int32_t __TZMagic = 0x545A6966;
const uint8_t __TZVersion0 = '\0';
const uint8_t __TZVersion2 = '2';

typedef struct __TZHead {
	uint8_t magic[4];      // __TZMagic
	uint8_t version;       // __TZVersion
	uint8_t reserved[15];  // reserved -- must be zero
	uint8_t ttisgmtcnt[4]; // coded number of trans. time flags
	uint8_t ttisstdcnt[4]; // coded number of trans. time flags
	uint8_t leapcnt[4];    // coded number of leap seconds
	uint8_t timecnt[4];    // coded number of transition times
	uint8_t typecnt[4];    // coded number of local time types
	uint8_t charcnt[4];    // coded number of abbr. chars
} __TZHead;

typedef struct __TZLocalTime {
	uint8_t utcoffset[4];  // coded UTC offset in seconds
	uint8_t isdst;         // used to set tm_isdst
	uint8_t abbrindex;     // abbreviation list index
} __TZLocalTime;

typedef struct __TZTransTime {
	uint8_t value[4];      // coded transition times a la time(2)
} __TZTransTime;

CF_INLINE int32_t __TZDecodeValue(const uint8_t* bufp) {
    int32_t result = (bufp[0] & 0x80) ? ~0L : 0L;
    result = (result << 8) | (bufp[0] & 0xff);
    result = (result << 8) | (bufp[1] & 0xff);
    result = (result << 8) | (bufp[2] & 0xff);
    result = (result << 8) | (bufp[3] & 0xff);
    return result;
}

CF_INLINE void __TZEncodeValue(int32_t value, uint8_t* bufp) {
    bufp[0] = (value >> 24) & 0xff;
    bufp[1] = (value >> 16) & 0xff;
    bufp[2] = (value >> 8) & 0xff;
    bufp[3] = (value >> 0) & 0xff;
}

///////////////////////////////////////////////////////////////////// __CFTZPeriod

/* 'startSec' is the whole integer seconds from a CFAbsoluteTime,
 *  giving dates between 1933 and 2069; info outside these years 
 *  is discarded on read-in.
 * 'info' layout:
 *  - bits 31-18: unused
 *  - bit 17: is-DST state
 *  - bit 16: sign of the offset (1 == negative)
 *  - bits 15-0: abs(offset) in seconds from GMT
 */
typedef struct __CFTZPeriod {
    int32_t startSec;
    CFStringRef abbrev;
    uint32_t info;
} __CFTZPeriod;

CF_INLINE void __CFTZPeriodInit(__CFTZPeriod* period,
                                int32_t startTime,
                                CFStringRef abbrev,
                                int32_t offset,
                                Boolean isDST)
{
    period->startSec = startTime;
    period->abbrev = abbrev ? (CFStringRef)CFRetain(abbrev) : NULL;
    _CFBitfieldSetValue(period->info, 15, 0, abs(offset));
    _CFBitfieldSetValue(period->info, 16, 16, (offset < 0 ? 1 : 0));
    _CFBitfieldSetValue(period->info, 17, 17, (isDST ? 1 : 0));
}

CF_INLINE void __CFTZPeriodDestroy(__CFTZPeriod* period) {
	if (period->abbrev) {
		CFRelease(period->abbrev);
		period->abbrev = NULL;
	}
}

CF_INLINE int32_t __CFTZPeriodStartSeconds(const __CFTZPeriod* period) {
    return period->startSec;
}

CF_INLINE CFStringRef __CFTZPeriodAbbreviation(const __CFTZPeriod* period) {
    return period->abbrev;
}

CF_INLINE int32_t __CFTZPeriodGMTOffset(const __CFTZPeriod* period) {
    int32_t v = _CFBitfieldGetValue(period->info, 15, 0);
    if (_CFBitfieldGetValue(period->info, 16, 16)) {
        v = -v;
    }
    return v;
}

CF_INLINE Boolean __CFTZPeriodIsDST(const __CFTZPeriod* period) {
    return (Boolean)_CFBitfieldGetValue(period->info, 17, 17);
}

static CFComparisonResult __CFCompareTZPeriods(const void* val1, const void* val2, void* context) {
    __CFTZPeriod* tzp1 = (__CFTZPeriod*)val1;
    __CFTZPeriod* tzp2 = (__CFTZPeriod*)val2;
    // we treat equal as less than, as the code which uses the
    // result of the bsearch doesn't expect exact matches
    // (they're pretty rare, so no point in over-coding for them)
    if (__CFTZPeriodStartSeconds(tzp1) <= __CFTZPeriodStartSeconds(tzp2)) {
        return kCFCompareLessThan;
    }
    return kCFCompareGreaterThan;
}

static Boolean __CFParseTimeZoneData(CFAllocatorRef allocator,
                                     CFDataRef data,
                                     __CFTZPeriod** pPeriods, CFIndex* pPeriodCount)
{
	*pPeriods = NULL;
	*pPeriodCount = 0;

    CFIndex length = CFDataGetLength(data);

	const __TZHead* head = (const __TZHead*)CFDataGetBytePtr(data);
    if (length < sizeof(__TZHead) ||  __TZDecodeValue(head->magic) != __TZMagic) {
        return false;
    }

    int32_t timecnt = __TZDecodeValue(head->timecnt);
    int32_t typecnt = __TZDecodeValue(head->typecnt);
    int32_t charcnt = __TZDecodeValue(head->charcnt);
    
	if (typecnt <= 0 || timecnt < 0 || charcnt < 0) {
        return false;
    }
    if (1024 < timecnt || 32 < typecnt || 128 < charcnt) {
        // reject excessive timezones to avoid arithmetic overflows for
        // security reasons and to reject potentially corrupt files
        return false;
    }

    const __TZTransTime* transTime = (const __TZTransTime*)(head + 1);
    const uint8_t* ltimeType = (const uint8_t*)(transTime + timecnt);
    const __TZLocalTime* ltime = (const __TZLocalTime*)(ltimeType + timecnt);
    const char* chars = (const char*)(ltime + typecnt);

	if ((const uint8_t*)(chars + charcnt) > CFDataGetBytePtr(data) + length) {
		return false;
    }

    CFIndex periodCount = (timecnt > 0) ? timecnt : 1;
    __CFTZPeriod* periods = (__CFTZPeriod*)CFAllocatorAllocate(
		allocator,
		periodCount * sizeof(__CFTZPeriod), 0);
    memset(periods, 0, periodCount * sizeof(__CFTZPeriod));

	CFMutableDictionaryRef abbrevCache = CFDictionaryCreateMutable(
		allocator,
		0,
		NULL, &kCFTypeDictionaryValueCallBacks);

    Boolean result = true;
	CFIndex idx;
    for (idx = 0; idx < periodCount; idx++) {
		int32_t startSec = INT_MIN;
		uint8_t type = 0;

		if (timecnt) {
			CFAbsoluteTime at = __TZDecodeValue(transTime->value) -
				kCFAbsoluteTimeIntervalSince1970;
			if (at < INT_MIN) {
				startSec = INT_MIN;
			} else if (INT_MAX < at) {
				startSec = INT_MAX;
			} else {
				startSec = (int32_t)at;
			}
			transTime++;

			type = *ltimeType;
			if (typecnt <= type) {
				result = false;
				break;
	        }
			ltimeType++;
		}

		int32_t offset = __TZDecodeValue(ltime[type].utcoffset);
		
		uint8_t dst = ltime[type].isdst;
        if (dst != 0 && dst != 1) {
            result = false;
            break;
        }

		CFIndex abbrevOffset = ltime[type].abbrindex;
        if (charcnt < abbrevOffset) {
            result = false;
            break;
        }
		CFStringRef abbrev = (CFStringRef)CFDictionaryGetValue(
			abbrevCache,
			(void*)abbrevOffset);
		if (!abbrev) {
			abbrev = CFStringCreateWithCString(
                allocator,
                chars + abbrevOffset, kCFStringEncodingASCII);
			CFDictionaryAddValue(
				abbrevCache, 
				(void*)abbrevOffset, abbrev);
			CFRelease(abbrev);
		}

		__CFTZPeriodInit(periods + idx, startSec, abbrev, offset, (dst == 1));
    }
    
	CFRelease(abbrevCache);

    if (!result) {
		CFAllocatorDeallocate(allocator, periods);
		return false;
	}

    // Dump all but the last INT_MIN and the first INT_MAX.
    for (idx = 0; idx < periodCount; idx++) {
        if ((periods[idx].startSec == INT_MIN) &&
            (idx + 1 < periodCount) &&
            (periods[idx + 1].startSec == INT_MIN))
        {
			__CFTZPeriodDestroy(periods + idx);
            periodCount--;
            memmove(
				periods + idx,
				periods + idx + 1,
				sizeof(__CFTZPeriod) * (periodCount - idx));
            idx--;
        }
    }

    // Don't combine these loops!  Watch the idx decrementing...
    for (idx = 0; idx < periodCount; idx++) {
        if ((periods[idx].startSec == INT_MAX) &&
            (0 < idx) &&
            (periods[idx - 1].startSec == INT_MAX))
        {
            __CFTZPeriodDestroy(periods + idx);
            periodCount--;
            memmove(
				periods + idx,
				periods + idx + 1,
				sizeof(__CFTZPeriod) * (periodCount - idx));
            idx--;
        }
    }

    CFQSortArray(periods, periodCount, sizeof(__CFTZPeriod), __CFCompareTZPeriods, NULL);

    // if the first period is in DST and there is more than one period, drop it.
    if (periodCount > 1 && __CFTZPeriodIsDST(periods)) {
		__CFTZPeriodDestroy(periods);
        periodCount--;
        memmove(periods, periods + 1, sizeof(__CFTZPeriod) * periodCount);
    }
	
	*pPeriods = periods;
	*pPeriodCount = periodCount;
    return true;
}
