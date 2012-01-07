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

#include <CoreFoundation/CFTimeZone.h>
#include <CoreFoundation/CFURL.h>
#include <CoreFoundation/CFSortFunctions.h>
#include <CoreFoundation/CFDictionary.h>

#include "CFInternal.h"
#include <math.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include "CFICUUtilities.h"

#include "CFTimeZone_TZ.inl"

struct __CFTimeZone {
    CFRuntimeBase _base;
    CFStringRef _name;
    CFDataRef _data;
    __CFTZPeriod* _periods;
    int32_t _periodCnt;
};

static CFTypeID __kCFTimeZoneTypeID = _kCFRuntimeNotATypeID;

enum {
    kCFTimeZoneNameStyleGeneric = 4,
    kCFTimeZoneNameStyleShortGeneric = 5
};

static CFSpinLock_t __CFTimeZoneGlobalLock = CFSpinLockInit;
static CFTimeZoneRef __CFTimeZoneSystem = NULL;
static CFTimeZoneRef __CFTimeZoneDefault = NULL;
static CFDictionaryRef __CFTimeZoneAbbreviationDict = NULL;
static CFSpinLock_t __CFTimeZoneAbbreviationLock = CFSpinLockInit;
static CFArrayRef __CFKnownTimeZoneList = NULL;
static CFMutableDictionaryRef __CFTimeZoneCache = NULL;

/* The criteria here are sort of: coverage for the U.S. and Europe,
 * large cities, abbreviation uniqueness, and perhaps a few others.
 * But do not make the list too large with obscure information.
 */
static const char* __CFTimeZoneAbbreviationDefaults[][2] = {
    {"ADT", "America/Halifax"},
    {"AKDT", "America/Juneau"},
    {"AKST", "America/Juneau"},
    {"ART", "America/Argentina/Buenos_Aires"},
    {"AST", "America/Halifax"},
    {"BDT", "Asia/Dhaka"},
    {"BRST", "America/Sao_Paulo"},
    {"BRT", "America/Sao_Paulo"},
    {"BST", "Europe/London"},
    {"CAT", "Africa/Harare"},
    {"CDT", "America/Chicago"},
    {"CEST", "Europe/Paris"},
    {"CET", "Europe/Paris"},
    {"CLST", "America/Santiago"},
    {"CLT", "America/Santiago"},
    {"COT", "America/Bogota"},
    {"CST", "America/Chicago"},
    {"EAT", "Africa/Addis_Ababa"},
    {"EDT", "America/New_York"},
    {"EEST", "Europe/Istanbul"},
    {"EET", "Europe/Istanbul"},
    {"EST", "America/New_York"},
    {"GMT", "GMT"},
    {"GST", "Asia/Dubai"},
    {"HKT", "Asia/Hong_Kong"},
    {"HST", "Pacific/Honolulu"},
    {"ICT", "Asia/Bangkok"},
    {"IRST", "Asia/Tehran"},
    {"IST", "Asia/Calcutta"},
    {"JST", "Asia/Tokyo"},
    {"KST", "Asia/Seoul"},
    {"MDT", "America/Denver"},
    {"MSD", "Europe/Moscow"},
    {"MSK", "Europe/Moscow"},
    {"MST", "America/Denver"},
    {"NZDT", "Pacific/Auckland"},
    {"NZST", "Pacific/Auckland"},
    {"PDT", "America/Los_Angeles"},
    {"PET", "America/Lima"},
    {"PHT", "Asia/Manila"},
    {"PKT", "Asia/Karachi"},
    {"PST", "America/Los_Angeles"},
    {"SGT", "Asia/Singapore"},
    {"UTC", "UTC"},
    {"WAT", "Africa/Lagos"},
    {"WEST", "Europe/Lisbon"},
    {"WET", "Europe/Lisbon"},
    {"WIT", "Asia/Jakarta"}
};

///////////////////////////////////////////////////////////////////// private

CF_INLINE void __CFTimeZoneLockGlobal(void) {
    CFSpinLock(&__CFTimeZoneGlobalLock);
}

CF_INLINE void __CFTimeZoneUnlockGlobal(void) {
    CFSpinUnlock(&__CFTimeZoneGlobalLock);
}

static CFTimeZoneRef __CFTimeZoneCacheGetCopy(CFStringRef name) {
    CFTimeZoneRef tz;
    __CFTimeZoneLockGlobal();
    if (__CFTimeZoneCache &&
        CFDictionaryGetValueIfPresent(__CFTimeZoneCache, name, (const void**)&tz))
    {
        __CFTimeZoneUnlockGlobal();
        return (CFTimeZoneRef)CFRetain(tz);
    }
    __CFTimeZoneUnlockGlobal();
    return NULL;
}

static void __CFTimeZoneCachePut(CFTimeZoneRef tz) {
    __CFTimeZoneLockGlobal();
    if (!__CFTimeZoneCache) {
        __CFTimeZoneCache = CFDictionaryCreateMutable(
            kCFAllocatorSystemDefault,
            0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    CFDictionaryAddValue(__CFTimeZoneCache, tz->_name, tz);
    __CFTimeZoneUnlockGlobal();
}

static void __CFTimeZoneCacheRemoveApplier(const void* key, const void* value, void* context) {
    CFDictionaryRemoveValue(__CFTimeZoneCache, (CFStringRef)key);
}

static CFIndex __CFBSearchTZPeriods(CFTimeZoneRef tz, CFAbsoluteTime at) {
    __CFTZPeriod elem;
    __CFTZPeriodInit(&elem, (int32_t)floor(at), NULL, 0, false);
    CFIndex idx = _CFBSearch(&elem, sizeof(__CFTZPeriod), tz->_periods, tz->_periodCnt, __CFCompareTZPeriods, NULL);
    if (tz->_periodCnt <= idx) {
        idx = tz->_periodCnt;
    } else if (!idx) {
        idx = 1;
    }
    return idx - 1;
}

static CFTimeZoneRef __CFTimeZoneCreateFixed(CFAllocatorRef allocator, int32_t seconds, CFStringRef name, Boolean isDST) {
    CFDataRef data;
    {
        int32_t nameSize = CFStringGetLength(name) + 1;
        int32_t dataSize = sizeof(__TZHead) + 6 + nameSize;
        _CF_ARRAY_ALLOCA(uint8_t, dataBytes, dataSize);
        memset(dataBytes, 0, dataSize);

        // Setup head
        __TZHead* head = (__TZHead*)dataBytes;
        __TZEncodeValue(__TZMagic, head->magic);
        __TZEncodeValue(1, head->ttisgmtcnt);
        __TZEncodeValue(1, head->ttisstdcnt);
        __TZEncodeValue(1, head->typecnt);
        __TZEncodeValue(nameSize, head->charcnt);

        // Put local time
        __TZLocalTime* ltime = (__TZLocalTime*)(head + 1);
		__TZEncodeValue(seconds, ltime->utcoffset);
		ltime->isdst = isDST ? 1 : 0;
        
        // Put abbr. chars
        char* chars = (char*)(ltime + 1);
        CFStringGetCString(name, chars, nameSize, kCFStringEncodingASCII);

        data = CFDataCreate(allocator, dataBytes, dataSize);
    }
    CFTimeZoneRef tz = CFTimeZoneCreate(allocator, name, data);
    CFRelease(data);
    return tz;
}

/*** CFTimeZone class ***/

static void __CFTimeZoneDeallocate(CFTypeRef cf) {
    CFTimeZoneRef tz = (CFTimeZoneRef)cf;
    CFAllocatorRef allocator = CFGetAllocator(tz);
    CFIndex idx;
    if (tz->_name) {
        CFRelease(tz->_name);
    }
    if (tz->_data) {
        CFRelease(tz->_data);
    }
    for (idx = 0; idx < tz->_periodCnt; idx++) {
		__CFTZPeriodDestroy(tz->_periods + idx);
    }
    if (tz->_periods) {
        CFAllocatorDeallocate(allocator, tz->_periods);
    }
}

static Boolean __CFTimeZoneEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFTimeZoneRef tz1 = (CFTimeZoneRef)cf1;
    CFTimeZoneRef tz2 = (CFTimeZoneRef)cf2;
    if (!CFEqual(CFTimeZoneGetName(tz1), CFTimeZoneGetName(tz2))) {
        return false;
    }
    if (!CFEqual(CFTimeZoneGetData(tz1), CFTimeZoneGetData(tz2))) {
        return false;
    }
    return true;
}

static CFHashCode __CFTimeZoneHash(CFTypeRef cf) {
    CFTimeZoneRef tz = (CFTimeZoneRef)cf;
    return CFHash(CFTimeZoneGetName(tz));
}

static CFStringRef __CFTimeZoneCopyDescription(CFTypeRef cf) {
    CFTimeZoneRef tz = (CFTimeZoneRef)cf;
    CFStringRef result, abbrev;
    CFAbsoluteTime at;
    at = CFAbsoluteTimeGetCurrent();
    abbrev = CFTimeZoneCopyAbbreviation(tz, at);
    result = CFStringCreateWithFormat(
        kCFAllocatorSystemDefault,
        NULL,
        CFSTR("<CFTimeZone %p [%p]>{"
            "name = %@; abbreviation = %@; GMT offset = %g; is DST = %s"
        "}"),
        cf, CFGetAllocator(tz),
        tz->_name, abbrev,
        CFTimeZoneGetSecondsFromGMT(tz, at),
        CFTimeZoneIsDaylightSavingTime(tz, at) ? "true" : "false");
    CFRelease(abbrev);
    return result;
}

static const CFRuntimeClass __CFTimeZoneClass = {
    0,
    "CFTimeZone",
    NULL, // init
    NULL, // copy
    __CFTimeZoneDeallocate,
    __CFTimeZoneEqual,
    __CFTimeZoneHash,
    NULL, //
    __CFTimeZoneCopyDescription
};

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL void __CFTimeZoneInitialize(void) {
    __kCFTimeZoneTypeID = _CFRuntimeRegisterClass(&__CFTimeZoneClass);
}

///////////////////////////////////////////////////////////////////// public

CONST_STRING_DECL(
    kCFTimeZoneSystemTimeZoneDidChangeNotification,
    "kCFTimeZoneSystemTimeZoneDidChangeNotification")

CFTypeID CFTimeZoneGetTypeID(void) {
    return __kCFTimeZoneTypeID;
}

CFTimeZoneRef CFTimeZoneCopySystem(void) {
    CFTimeZoneRef tz;
    __CFTimeZoneLockGlobal();
    if (!__CFTimeZoneSystem) {
        __CFTimeZoneUnlockGlobal();
        tz = CFPlatformCreateSystemTimeZone();
        __CFTimeZoneLockGlobal();
        if (!__CFTimeZoneSystem) {
            __CFTimeZoneSystem = tz;
        } else {
            if (tz) {
                CFRelease(tz);
            }
        }
    }
    tz = __CFTimeZoneSystem;
    if (tz) {
        CFRetain(tz);
    }
    __CFTimeZoneUnlockGlobal();
    return tz;
}

void CFTimeZoneResetSystem(void) {
    __CFTimeZoneLockGlobal();
    if (__CFTimeZoneDefault == __CFTimeZoneSystem) {
        if (__CFTimeZoneDefault) {
            CFRelease(__CFTimeZoneDefault);
        }
        __CFTimeZoneDefault = NULL;
    }
    CFTimeZoneRef tz = __CFTimeZoneSystem;
    __CFTimeZoneSystem = NULL;
    __CFTimeZoneUnlockGlobal();
    if (tz) {
        CFRelease(tz);
    }
}

CFTimeZoneRef CFTimeZoneCopyDefault(void) {
    CFTimeZoneRef tz;
    __CFTimeZoneLockGlobal();
    if (!__CFTimeZoneDefault) {
        __CFTimeZoneUnlockGlobal();
        tz = CFTimeZoneCopySystem();
        __CFTimeZoneLockGlobal();
        if (!__CFTimeZoneDefault) {
            __CFTimeZoneDefault = tz;
        } else {
            if (tz) {
                CFRelease(tz);
            }
        }
    }
    tz = __CFTimeZoneDefault;
    if (tz) {
        CFRetain(tz);
    }
    __CFTimeZoneUnlockGlobal();
    return tz;
}

void CFTimeZoneSetDefault(CFTimeZoneRef tz) {
    if (tz) {
        CF_VALIDATE_OBJECT_ARG(CF, tz, CFTimeZoneGetTypeID());
    }
    __CFTimeZoneLockGlobal();
    if (tz != __CFTimeZoneDefault) {
        if (tz) {
            CFRetain(tz);
        }
        if (__CFTimeZoneDefault) {
            CFRelease(__CFTimeZoneDefault);
        }
        __CFTimeZoneDefault = tz;
    }
    __CFTimeZoneUnlockGlobal();
}

CFArrayRef CFTimeZoneCopyKnownNames(void) {
    CFArrayRef tzs;
    __CFTimeZoneLockGlobal();
    if (!__CFKnownTimeZoneList) {
        __CFKnownTimeZoneList = CFPlatformLoadKnownTimeZones();
    }
    tzs = __CFKnownTimeZoneList;
    if (tzs) {
        CFRetain(tzs);
    }
    __CFTimeZoneUnlockGlobal();
    return tzs;
}

CFDictionaryRef CFTimeZoneCopyAbbreviationDictionary(void) {
    CFSpinLock(&__CFTimeZoneAbbreviationLock);
    if (!__CFTimeZoneAbbreviationDict) {
        CFIndex abbrCount = CF_COUNTOF(__CFTimeZoneAbbreviationDefaults);
        CFMutableDictionaryRef abbrs = CFDictionaryCreateMutable(
                kCFAllocatorSystemDefault, abbrCount,
                &kCFTypeDictionaryKeyCallBacks,
                &kCFTypeDictionaryValueCallBacks);
        if (abbrs) {
            for (CFIndex i = 0; i != abbrCount; ++i) {
                CFStringRef abbr = CFStringCreateWithCString(
                        kCFAllocatorSystemDefault,
                        __CFTimeZoneAbbreviationDefaults[i][0],
                        kCFStringEncodingASCII);

                CFStringRef name = CFStringCreateWithCString(
                        kCFAllocatorSystemDefault,
                        __CFTimeZoneAbbreviationDefaults[i][1],
                        kCFStringEncodingASCII);

                CFDictionaryAddValue(abbrs, abbr, name);
            }
        }
        __CFTimeZoneAbbreviationDict = abbrs;
    }
    CFSpinUnlock(&__CFTimeZoneAbbreviationLock);

    if (__CFTimeZoneAbbreviationDict) {
        CFRetain(__CFTimeZoneAbbreviationDict);
    }
    return __CFTimeZoneAbbreviationDict;
}

void CFTimeZoneSetAbbreviationDictionary(CFDictionaryRef dict) {
    CF_VALIDATE_OBJECT_ARG(CF, dict, CFDictionaryGetTypeID());
    __CFTimeZoneLockGlobal();
    if (dict != __CFTimeZoneAbbreviationDict) {
        if (dict) {
            CFRetain(dict);
        }
        if (__CFTimeZoneAbbreviationDict) {
            CFDictionaryApplyFunction(
                __CFTimeZoneAbbreviationDict,
                __CFTimeZoneCacheRemoveApplier, NULL);
            CFRelease(__CFTimeZoneAbbreviationDict);
        }
        __CFTimeZoneAbbreviationDict = dict;
    }
    __CFTimeZoneUnlockGlobal();
}

CFTimeZoneRef CFTimeZoneCreate(CFAllocatorRef allocator, CFStringRef name, CFDataRef data) {
    if (allocator == NULL) {
        allocator = CFAllocatorGetDefault();
    }
    CF_VALIDATE_OBJECT_ARG(CF, allocator, CFAllocatorGetTypeID());
    CF_VALIDATE_OBJECT_ARG(CF, name, CFStringGetTypeID());
    CF_VALIDATE_OBJECT_ARG(CF, data, CFDataGetTypeID());

    {
        CFTimeZoneRef tz = __CFTimeZoneCacheGetCopy(name);
        if (tz) {
            return tz;
        }
    }

    __CFTZPeriod* tzp = NULL;
    CFIndex cnt = 0;
    if (!__CFParseTimeZoneData(allocator, data, &tzp, &cnt)) {
        return NULL;
    }

    struct __CFTimeZone* instance = (struct __CFTimeZone*)_CFRuntimeCreateInstance(
        allocator, 
        CFTimeZoneGetTypeID(), 
        sizeof(struct __CFTimeZone) - sizeof(CFRuntimeBase), 
        NULL);
    if (!instance) {
		if (tzp) {
			CFIndex idx;
			for (idx = 0; idx < cnt; idx++) {
				__CFTZPeriodDestroy(tzp + idx);
			}
            CFAllocatorDeallocate(allocator, tzp);
        }
        return NULL;
    }
    instance->_name = CFStringCreateCopy(allocator, name);
    instance->_data = CFDataCreateCopy(allocator, data);
    instance->_periods = tzp;
    instance->_periodCnt = cnt;

    __CFTimeZoneCachePut(instance);

    return instance;
}

// rounds offset to nearest minute
CFTimeZoneRef CFTimeZoneCreateWithTimeIntervalFromGMT(CFAllocatorRef allocator, CFTimeInterval ti) {
    CFTimeZoneRef result;
    CFStringRef name;
    int32_t seconds, minute, hour;
    if (allocator == NULL) {
        allocator = CFAllocatorGetDefault();
    }
    CF_VALIDATE_OBJECT_ARG(CF, allocator, CFAllocatorGetTypeID());

	//TODO rework this mess!

    if (ti < -18.0 * 3600 || 18.0 * 3600 < ti) {
        return NULL;
    }
    ti = (ti < 0) ?
        ceil((ti / 60) - 0.5) * 60 :
        floor((ti / 60) + 0.5) * 60;
    seconds = (int32_t)ti;
    hour = (ti < 0) ? (-seconds / 3600) : (seconds / 3600);
    seconds -= ((ti < 0) ? -hour : hour) * 3600;
    minute = (ti < 0) ? (-seconds / 60) : (seconds / 60);
    if (fabs(ti) < 1.0) {
        name = (CFStringRef)CFRetain(CFSTR("GMT"));
    } else {
        name = CFStringCreateWithFormat(
            allocator,
            NULL, CFSTR("GMT%c%02d%02d"),
            (ti < 0.0 ? '-' : '+'), hour, minute);
    }
    result = __CFTimeZoneCreateFixed(allocator, (int32_t)ti, name, 0);
    CFRelease(name);
    return result;
}

CFTimeZoneRef CFTimeZoneCreateWithName(CFAllocatorRef allocator, CFStringRef name, Boolean tryAbbrev) {
    if (allocator == NULL) {
        allocator = CFAllocatorGetDefault();
    }
    CF_VALIDATE_OBJECT_ARG(CF, allocator, CFAllocatorGetTypeID());
    CF_VALIDATE_OBJECT_ARG(CF, name, CFStringGetTypeID());

    if (CFEqual(CFSTR(""), name)) {
        // Empty string is not a time zone name, just abort now,
        //  following stuff will fail anyway.
        return NULL;
    }

    CFDataRef data = NULL;
    if (tryAbbrev) {
        CFDictionaryRef abbrevs = CFTimeZoneCopyAbbreviationDictionary();
        CFStringRef fullName = (CFStringRef)CFDictionaryGetValue(abbrevs, name);
        if (fullName) {
            CFTimeZoneRef tz = __CFTimeZoneCacheGetCopy(fullName);
            if (tz) {
                CFRelease(abbrevs);
                return tz;
            }
            data = CFPlatformLoadTimeZoneData(fullName);
        }
        CFRelease(abbrevs);
    }
    if (!data) {
        CFTimeZoneRef tz = __CFTimeZoneCacheGetCopy(name);
        if (tz) {
            return tz;
        }
        data = CFPlatformLoadTimeZoneData(name);
    }
    if (!data) {
        return NULL;
    }
    CFTimeZoneRef tz = CFTimeZoneCreate(allocator, name, data);
    CFRelease(data);
    return tz;
}

CFStringRef CFTimeZoneGetName(CFTimeZoneRef tz) {
    CF_OBJC_FUNCDISPATCH(CFStringRef, tz, "name");
    CF_VALIDATE_OBJECT_ARG(CF, tz, CFTimeZoneGetTypeID());
    return tz->_name;
}

CFDataRef CFTimeZoneGetData(CFTimeZoneRef tz) {
    CF_OBJC_FUNCDISPATCH(CFDataRef, tz, "data");
    CF_VALIDATE_OBJECT_ARG(CF, tz, CFTimeZoneGetTypeID());
    return tz->_data;
}

CFTimeInterval CFTimeZoneGetSecondsFromGMT(CFTimeZoneRef tz, CFAbsoluteTime at) {
    CFIndex idx;
    //TODO _secondsFromGMTForAbsoluteTime:result:
    //CF_OBJC_FUNCDISPATCH(CFTimeInterval, tz, "_secondsFromGMTForAbsoluteTime:", at);
    CF_VALIDATE_OBJECT_ARG(CF, tz, CFTimeZoneGetTypeID());
    idx = __CFBSearchTZPeriods(tz, at);
    return __CFTZPeriodGMTOffset(&(tz->_periods[idx]));
}

CFStringRef CFTimeZoneCopyAbbreviation(CFTimeZoneRef tz, CFAbsoluteTime at) {
    CFStringRef result;
    CFIndex idx;
    CF_OBJC_FUNCDISPATCH(CFStringRef, tz, "_abbreviationForAbsoluteTime:", at);
    CF_VALIDATE_OBJECT_ARG(CF, tz, CFTimeZoneGetTypeID());
    idx = __CFBSearchTZPeriods(tz, at);
    result = __CFTZPeriodAbbreviation(&(tz->_periods[idx]));
    return result ? (CFStringRef)CFRetain(result) : NULL;
}

Boolean CFTimeZoneIsDaylightSavingTime(CFTimeZoneRef tz, CFAbsoluteTime at) {
    CFIndex idx;
    CF_OBJC_FUNCDISPATCH(Boolean, tz, "_isDaylightSavingTimeForAbsoluteTime:", at);
    CF_VALIDATE_OBJECT_ARG(CF, tz, CFTimeZoneGetTypeID());
    idx = __CFBSearchTZPeriods(tz, at);
    return __CFTZPeriodIsDST(&(tz->_periods[idx]));
}

CFTimeInterval CFTimeZoneGetDaylightSavingTimeOffset(CFTimeZoneRef tz, CFAbsoluteTime at) {
    //TODO _daylightSavingTimeOffsetForAbsoluteTime:result:
    //CF_OBJC_FUNCDISPATCH(CFTimeInterval, tz, "_daylightSavingTimeOffsetForAbsoluteTime:", at);
    CF_VALIDATE_OBJECT_ARG(CF, tz, CFTimeZoneGetTypeID());
    CFIndex idx = __CFBSearchTZPeriods(tz, at);
    if (__CFTZPeriodIsDST(&(tz->_periods[idx]))) {
        CFTimeInterval offset = __CFTZPeriodGMTOffset(&(tz->_periods[idx]));
        if (idx + 1 < tz->_periodCnt) {
            return offset - __CFTZPeriodGMTOffset(&(tz->_periods[idx + 1]));
        } else if (0 < idx) {
            return offset - __CFTZPeriodGMTOffset(&(tz->_periods[idx - 1]));
        }
    }
    return 0.0;
}

CFAbsoluteTime CFTimeZoneGetNextDaylightSavingTimeTransition(CFTimeZoneRef tz, CFAbsoluteTime at) {
    //TODO _nextDaylightSavingTimeTransitionAfterAbsoluteTime:result:
    //CF_OBJC_FUNCDISPATCH(CFTimeInterval, tz, "_nextDaylightSavingTimeTransitionAfterAbsoluteTime:", at);
    CF_VALIDATE_OBJECT_ARG(CF, tz, CFTimeZoneGetTypeID());
    CFIndex idx = __CFBSearchTZPeriods(tz, at);
    if (tz->_periodCnt <= idx + 1) {
        return 0.0;
    }
    return (CFAbsoluteTime)__CFTZPeriodStartSeconds(&(tz->_periods[idx + 1]));
}

CFStringRef CFTimeZoneCopyLocalizedName(CFTimeZoneRef tz, CFTimeZoneNameStyle style, CFLocaleRef locale) {
    CF_OBJC_FUNCDISPATCH(CFStringRef, tz, "localizedName:locale:", style, locale);
    CF_VALIDATE_OBJECT_ARG(CF, tz, CFTimeZoneGetTypeID());
    CF_VALIDATE_OBJECT_ARG(CF, locale, CFLocaleGetTypeID());

	const char* localeName;
	CFICU_LOCALE_GET_NAME_CSTR(locale, localeName);
    if (!localeName) {
        return NULL;
    }

    CFStringRef localeID = CFLocaleGetIdentifier(locale);
    UCalendar *cal = _CFCalendarCreateUCalendar(NULL, localeID, tz);
    if (!cal) {
        return NULL;
    }

    UChar ubuffer[ICU_BUFFER_SIZE];
    UErrorCode status = U_ZERO_ERROR;
    int32_t cnt = ucal_getTimeZoneDisplayName(
		cal,
		(UCalendarDisplayNameType)style,
		localeName,
		ubuffer, ICU_BUFFER_SIZE,
		&status);
    ucal_close(cal);
    if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
        return CFStringCreateWithCharacters(CFGetAllocator(tz), (const UniChar*)ubuffer, cnt);
    }
    return NULL;
}

CFStringRef CFTimeZoneGetId(CFTimeZoneRef tz) {
    CFMutableStringRef id = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, tz->_name);
    int32_t i = 0;
    for (;i != tz->_periodCnt; ++i) {
        __CFTZPeriod period = tz->_periods[i];
        CFStringAppendFormat(
            id,
            NULL, CFSTR("%d%@%d"),
            period.startSec, period.abbrev, period.info);
    }
    return id;
}
