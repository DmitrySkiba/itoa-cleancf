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

#include <CoreFoundation/CFLocale.h>
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFDictionary.h>
//TODO uncomment when CFPreferences is available
//#include <CoreFoundation/CFPreferences.h>
#include <CoreFoundation/CFCalendar.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFNumberFormatter.h>

#include "CFInternal.h"
#include <unicode/uloc.h>     // ICU locales
#include <unicode/ulocdata.h> // ICU locale data
#include <unicode/ucurr.h>    // ICU currency functions
#include <unicode/uset.h>     // ICU Unicode sets
#include <unicode/putil.h>    // ICU low-level utilities
#include <unicode/umsg.h>     // ICU message formatting

#include <stdlib.h>
#include <stdio.h>
#include <unicode/ucol.h>

//TODO remove?
#define FALLBACK_LOCALE_NAME CFSTR("")

static const char* kCalendarKeyword = "calendar";
static const char* kCollationKeyword = "collation";

enum {
	__kCFMaxICUNameSize = 1024
};

typedef struct __CFLocale {
    CFRuntimeBase _base;
    CFStringRef _identifier; // canonical identifier, never NULL
    CFMutableDictionaryRef _cache;
    CFMutableDictionaryRef _overrides;
    CFDictionaryRef _prefs;
    CFSpinLock_t _lock;
} __CFLocale;
static CFTypeID __kCFLocaleTypeID = _kCFRuntimeNotATypeID;

// Flag bits (0-1)
enum {
    __kCFLocaleOrdinary = 0,
    __kCFLocaleSystem = 1,
    __kCFLocaleUser = 2,
    __kCFLocaleCustom = 3
};

typedef int32_t (*__CFICUFunction)(const char*, const char*, UChar*, int32_t, UErrorCode*);

struct __CFLocaleKeyTableEntry {
    CFStringRef key;
    bool (*get)(CFLocaleRef, bool user, CFTypeRef*, CFStringRef context);  // returns an immutable copy & reference
    bool (*name)(const char*, const char*, CFStringRef*);
    CFStringRef context;
};

static CFSpinLock_t __CFLocaleGlobalLock = CFSpinLockInit;
static CFLocaleRef __CFLocaleSystem = NULL;
static CFLocaleRef __CFLocaleCurrent = NULL;
static CFMutableDictionaryRef __CFLocaleCache = NULL;

///////////////////////////////////////////////////////////////////// private

CF_INLINE CFIndex __CFLocaleGetType(CFLocaleRef locale) {
    return _CFBitfieldGetValue(CF_INFO(locale), 1, 0);
}
CF_INLINE void __CFLocaleSetType(CFLocaleRef locale, CFIndex type) {
    _CFBitfieldSetValue(CF_INFO(locale), 1, 0, (uint8_t)type);
}

CF_INLINE void __CFLocaleLockGlobal(void) {
    CFSpinLock(&__CFLocaleGlobalLock);
}
CF_INLINE void __CFLocaleUnlockGlobal(void) {
    CFSpinUnlock(&__CFLocaleGlobalLock);
}

CF_INLINE void __CFLocaleLock(CFLocaleRef locale) {
    CFSpinLock(CF_CONST_CAST(CFSpinLock_t*, &locale->_lock));
}
CF_INLINE void __CFLocaleUnlock(CFLocaleRef locale) {
    CFSpinUnlock(CF_CONST_CAST(CFSpinLock_t*, &locale->_lock));
}

static CFArrayRef __CFLocaleCopyCStringsAsArray(const char* const* p) {
    CFMutableArrayRef working = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    for (;*p; ++p) {
        CFStringRef string = CFStringCreateWithCString(kCFAllocatorSystemDefault, *p, kCFStringEncodingASCII);
        CFArrayAppendValue(working, string);
        CFRelease(string);
    }
    CFArrayRef result = CFArrayCreateCopy(kCFAllocatorSystemDefault, working);
    CFRelease(working);
    return result;
}

static CFArrayRef __CFLocaleCopyUEnumerationAsArray(UEnumeration* enumer, UErrorCode* icuErr) {
    const UChar* next = NULL;
    int32_t len = 0;
    CFMutableArrayRef working = NULL;
    if (U_SUCCESS(*icuErr)) {
        working = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);
    }
    while ((next = uenum_unext(enumer, &len, icuErr)) && U_SUCCESS(*icuErr)) {
        CFStringRef string = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, (const UniChar*)next, (CFIndex)len);
        CFArrayAppendValue(working, string);
        CFRelease(string);
    }
    if (*icuErr == U_INDEX_OUTOFBOUNDS_ERROR) {
        *icuErr = U_ZERO_ERROR;      // Temp: Work around bug (ICU 5220) in ucurr enumerator
    }
    CFArrayRef result = NULL;
    if (U_SUCCESS(*icuErr)) {
        result = CFArrayCreateCopy(kCFAllocatorSystemDefault, working);
    }
    if (working != NULL) {
        CFRelease(working);
    }
    return result;
}

/*
 * These functions return true or false depending on the success or failure of the function.
 * In the Copy case, this is failure to fill the *cf out parameter, and that out parameter is
 * returned by reference WITH a retain on it.
 */

static bool __CFLocaleCopyLocaleID(CFLocaleRef locale, bool user, CFTypeRef* cf, CFStringRef context) {
    *cf = CFRetain(locale->_identifier);
    return true;
}

static bool __CFLocaleCopyCodes(CFLocaleRef locale, bool user, CFTypeRef* cf, CFStringRef context) {
    CFDictionaryRef codes = NULL;
    // this access of _cache is protected by the lock in CFLocaleGetValue()
    if (!CFDictionaryGetValueIfPresent(locale->_cache, CFSTR("__kCFLocaleCodes"), (const void**)&codes)) {
        codes = CFLocaleCreateComponentsFromLocaleIdentifier(kCFAllocatorSystemDefault, locale->_identifier);
        if (codes) {
            CFDictionarySetValue(locale->_cache, CFSTR("__kCFLocaleCodes"), codes);
        }
        if (codes) {
            CFRelease(codes);
        }
    }
    if (codes) {
        CFStringRef value = (CFStringRef)CFDictionaryGetValue(codes, context); // context is one of kCFLocale*Code constants
        if (value) {
            CFRetain(value);
        }
        *cf = value;
        return true;
    }
    return false;
}

static CFCharacterSetRef __CFCreateCharacterSetFromUSet(USet* set) {
    UErrorCode icuErr = U_ZERO_ERROR;
    CFMutableCharacterSetRef working = CFCharacterSetCreateMutable(NULL);
    UChar buffer[2048];     // Suitable for most small sets
    int32_t stringLen;

    if (working == NULL) {
        return NULL;
    }

    int32_t itemCount = uset_getItemCount(set);
    int32_t i;
    for (i = 0; i < itemCount; ++i) {
        UChar32 start, end;
        UChar* string;

        string = buffer;
        stringLen = uset_getItem(set, i, &start, &end, buffer, sizeof(buffer) / sizeof(UChar), &icuErr);
        if (icuErr == U_BUFFER_OVERFLOW_ERROR) {
            string = (UChar*)malloc(sizeof(UChar) * (stringLen + 1));
            if (!string) {
                CFRelease(working);
                return NULL;
            }
            icuErr = U_ZERO_ERROR;
            (void)uset_getItem(set, i, &start, &end, string, stringLen + 1, &icuErr);
        }
        if (U_FAILURE(icuErr)) {
            if (string != buffer) {
                free(string);
            }
            CFRelease(working);
            return NULL;
        }
        if (stringLen <= 0) {
            CFCharacterSetAddCharactersInRange(working, CFRangeMake(start, end - start + 1));
        } else {
            CFStringRef cfString = CFStringCreateWithCharactersNoCopy(kCFAllocatorSystemDefault, (UniChar*)string, stringLen, kCFAllocatorNull);
            CFCharacterSetAddCharactersInString(working, cfString);
            CFRelease(cfString);
        }
        if (string != buffer) {
            free(string);
        }
    }

    CFCharacterSetRef result = CFCharacterSetCreateCopy(kCFAllocatorSystemDefault, working);
    CFRelease(working);
    return result;
}

static bool __CFLocaleCopyExemplarCharSet(CFLocaleRef locale, bool user, CFTypeRef* cf, CFStringRef context) {
    char localeID[ULOC_FULLNAME_CAPACITY + ULOC_KEYWORD_AND_VALUES_CAPACITY];
    if (CFStringGetCString(locale->_identifier, localeID, sizeof(localeID) / sizeof(char), kCFStringEncodingASCII)) {
        UErrorCode icuStatus = U_ZERO_ERROR;
        ULocaleData* uld = ulocdata_open(localeID, &icuStatus);
        USet* set = ulocdata_getExemplarSet(uld, NULL, USET_ADD_CASE_MAPPINGS, ULOCDATA_ES_STANDARD, &icuStatus);
        ulocdata_close(uld);
        if (U_FAILURE(icuStatus)) {
            return false;
        }
        if (icuStatus == U_USING_DEFAULT_WARNING) { // If default locale used, force to empty set
            uset_clear(set);
        }
        *cf = __CFCreateCharacterSetFromUSet(set);
        uset_close(set);
        return (*cf != NULL);
    }
    return false;
}

static bool __CFLocaleCopyICUKeyword(CFLocaleRef locale, bool user, CFTypeRef* cf, CFStringRef context, const char* keyword) {
    char localeID[ULOC_FULLNAME_CAPACITY + ULOC_KEYWORD_AND_VALUES_CAPACITY];
    if (CFStringGetCString(locale->_identifier, localeID, sizeof(localeID) / sizeof(char), kCFStringEncodingASCII)) {
        char value[ULOC_KEYWORD_AND_VALUES_CAPACITY];
        UErrorCode icuStatus = U_ZERO_ERROR;
        if (uloc_getKeywordValue(localeID, keyword, value, sizeof(value) / sizeof(char), &icuStatus) > 0 && U_SUCCESS(icuStatus)) {
            *cf = (CFTypeRef)CFStringCreateWithCString(kCFAllocatorSystemDefault, value, kCFStringEncodingASCII);
            return true;
        }
    }
    *cf = NULL;
    return false;
}

static bool __CFLocaleCopyCalendarID(CFLocaleRef locale, bool user, CFTypeRef* cf, CFStringRef context) {
    bool succeeded = __CFLocaleCopyICUKeyword(locale, user, cf, context, kCalendarKeyword);
    if (succeeded) {
        if (CFEqual(*cf, kCFGregorianCalendar)) {
            CFRelease(*cf);
            *cf = CFRetain(kCFGregorianCalendar);
        } else if (CFEqual(*cf, kCFBuddhistCalendar)) {
            CFRelease(*cf);
            *cf = CFRetain(kCFBuddhistCalendar);
        } else if (CFEqual(*cf, kCFJapaneseCalendar)) {
            CFRelease(*cf);
            *cf = CFRetain(kCFJapaneseCalendar);
        } else if (CFEqual(*cf, kCFIslamicCalendar)) {
            CFRelease(*cf);
            *cf = CFRetain(kCFIslamicCalendar);
        } else if (CFEqual(*cf, kCFIslamicCivilCalendar)) {
            CFRelease(*cf);
            *cf = CFRetain(kCFIslamicCivilCalendar);
        } else if (CFEqual(*cf, kCFHebrewCalendar)) {
            CFRelease(*cf);
            *cf = CFRetain(kCFHebrewCalendar);
        } else if (CFEqual(*cf, kCFChineseCalendar)) {
            CFRelease(*cf);
            *cf = CFRetain(kCFChineseCalendar);
        }
    } else {
        *cf = CFRetain(kCFGregorianCalendar);
    }
    return true;
}

static bool __CFLocaleCopyCalendar(CFLocaleRef locale, bool user, CFTypeRef* cf, CFStringRef context) {
    if (__CFLocaleCopyCalendarID(locale, user, cf, context)) {
        CFCalendarRef calendar = CFCalendarCreateWithIdentifier(kCFAllocatorSystemDefault, (CFStringRef) * cf);
        CFCalendarSetLocale(calendar, locale);
        CFRelease(*cf);
        *cf = calendar;
        return true;
    }
    return false;
}

static bool __CFLocaleCopyCollationID(CFLocaleRef locale, bool user, CFTypeRef* cf, CFStringRef context) {
    return __CFLocaleCopyICUKeyword(locale, user, cf, context, kCollationKeyword);
}

static bool __CFLocaleCopyCollatorID(CFLocaleRef locale, bool user, CFTypeRef* cf, CFStringRef context) {
    CFStringRef canonLocaleCFStr = NULL;
    if (!canonLocaleCFStr) {
        canonLocaleCFStr = CFLocaleGetIdentifier(locale);
        CFRetain(canonLocaleCFStr);
    }
    *cf = canonLocaleCFStr;
    return canonLocaleCFStr ? true : false;
}

static bool __CFLocaleCopyUsesMetric(CFLocaleRef locale, bool user, CFTypeRef* cf, CFStringRef context) {
    bool us = false;    // Default is Metric
    bool done = false;

    if (!done) {
        char localeID[ULOC_FULLNAME_CAPACITY + ULOC_KEYWORD_AND_VALUES_CAPACITY];
        if (CFStringGetCString(locale->_identifier, localeID, sizeof(localeID) / sizeof(char), kCFStringEncodingASCII)) {
            UErrorCode icuStatus = U_ZERO_ERROR;
            UMeasurementSystem ms = UMS_SI;
            ms = ulocdata_getMeasurementSystem(localeID, &icuStatus);
            if (U_SUCCESS(icuStatus)) {
                us = (ms == UMS_US);
                done = true;
            }
        }
    }
    if (!done) {
        us = false;
    }
    *cf = us ? CFRetain(kCFBooleanFalse) : CFRetain(kCFBooleanTrue);
    return true;
}

static bool __CFLocaleCopyMeasurementSystem(CFLocaleRef locale, bool user, CFTypeRef* cf, CFStringRef context) {
    if (__CFLocaleCopyUsesMetric(locale, user, cf, context)) {
        bool us = (*cf == kCFBooleanFalse);
        CFRelease(*cf);
        *cf = us ? CFRetain(CFSTR("U.S.")) : CFRetain(CFSTR("Metric"));
        return true;
    }
    return false;
}

static bool __CFLocaleCopyNumberFormat(CFLocaleRef locale, bool user, CFTypeRef* cf, CFStringRef context) {
    CFTypeRef prop = NULL;
    CFNumberFormatterRef nf = CFNumberFormatterCreate(kCFAllocatorSystemDefault, locale, kCFNumberFormatterDecimalStyle);
    prop = nf ? CFNumberFormatterCopyProperty(nf, context) : NULL;
    if (nf) {
        CFRelease(nf);
    }
    if (prop) {
        *cf = prop;
        return true;
    }
    return false;
}

// ICU does not reliably set up currency info for other than Currency-type formatters,
// so we have to have another routine here which creates a Currency number formatter.
static bool __CFLocaleCopyNumberFormat2(CFLocaleRef locale, bool user, CFTypeRef* cf, CFStringRef context) {
    CFTypeRef prop = NULL;
    CFNumberFormatterRef nf = CFNumberFormatterCreate(kCFAllocatorSystemDefault, locale, kCFNumberFormatterCurrencyStyle);
    prop = nf ? CFNumberFormatterCopyProperty(nf, context) : NULL;
    if (nf) {
        CFRelease(nf);
    }
    if (prop) {
        *cf = prop;
        return true;
    }
    return false;
}

static bool __CFLocaleICUName(const char* locale, const char* valLocale, CFStringRef* out, __CFICUFunction icu) {
    UErrorCode icuStatus = U_ZERO_ERROR;
    int32_t size;
    UChar name[__kCFMaxICUNameSize];

    size = (*icu)(valLocale, locale, name, __kCFMaxICUNameSize, &icuStatus);
    if (U_SUCCESS(icuStatus) && size > 0 && icuStatus != U_USING_DEFAULT_WARNING) {
        *out = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, (UniChar*)name, size);
        return (*out != NULL);
    }
    return false;
}

static bool __CFLocaleICUKeywordValueName(const char* locale, const char* value, const char* keyword, CFStringRef* out) {
    UErrorCode icuStatus = U_ZERO_ERROR;
    int32_t size = 0;
    UChar name[__kCFMaxICUNameSize];
    // Need to make a fake locale ID
    char lid[ULOC_FULLNAME_CAPACITY + ULOC_KEYWORD_AND_VALUES_CAPACITY];
    if (strlen(value) < ULOC_KEYWORD_AND_VALUES_CAPACITY) {
        snprintf(lid, sizeof(lid), "en_US@%s=%s", keyword, value);
        size = uloc_getDisplayKeywordValue(lid, keyword, locale, name, __kCFMaxICUNameSize, &icuStatus);
        if (U_SUCCESS(icuStatus) && size > 0 && icuStatus != U_USING_DEFAULT_WARNING) {
            *out = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, (UniChar*)name, size);
            return (*out != NULL);
        }
    }
    return false;
}

static bool __CFLocaleICUCurrencyName(const char* locale, const char* value, UCurrNameStyle style, CFStringRef* out) {
    size_t valLen = strlen(value);
    if (valLen != 3) { // not a valid ISO code
        return false;
    }
    UChar curr[4];
    UBool isChoice = FALSE;
    int32_t size = 0;
    UErrorCode icuStatus = U_ZERO_ERROR;
    u_charsToUChars(value, curr, (int32_t)valLen);
    curr[valLen] = '\0';
    const UChar* name;
    name = ucurr_getName(curr, locale, style, &isChoice, &size, &icuStatus);
    if (U_FAILURE(icuStatus) || icuStatus == U_USING_DEFAULT_WARNING) {
        return false;
    }
    UChar result[__kCFMaxICUNameSize];
    if (isChoice) {
        UChar pattern[__kCFMaxICUNameSize];
        CFStringRef patternRef = CFStringCreateWithFormat(kCFAllocatorSystemDefault, NULL, CFSTR("{0,choice,%S}"), name);
        CFIndex pattlen = CFStringGetLength(patternRef);
        CFStringGetCharacters(patternRef, CFRangeMake(0, pattlen), (UniChar*)pattern);
        CFRelease(patternRef);
        pattern[pattlen] = '\0';        // null terminate the pattern
        // Format the message assuming a large amount of the currency
        size = u_formatMessage("en_US", pattern, pattlen, result, __kCFMaxICUNameSize, &icuStatus, 10.0);
        if (U_FAILURE(icuStatus)) {
            return false;
        }
        name = result;

    }
    *out = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, (UniChar*)name, size);
    return (*out != NULL);
}

static bool __CFLocaleFullName(const char* locale, const char* value, CFStringRef* out) {
    UErrorCode icuStatus = U_ZERO_ERROR;
    int32_t size;
    UChar name[__kCFMaxICUNameSize];

    // First, try to get the full locale.
    size = uloc_getDisplayName(value, locale, name, __kCFMaxICUNameSize, &icuStatus);
    if (U_FAILURE(icuStatus) || size <= 0) {
        return false;
    }

    // Did we wind up using a default somewhere?
    if (icuStatus == U_USING_DEFAULT_WARNING) {
        // For some locale IDs, there may be no language which has a translation for every
        // piece. Rather than return nothing, see if we can at least handle
        // the language part of the locale.
        UErrorCode localStatus = U_ZERO_ERROR;
        int32_t localSize;
        UChar localName[__kCFMaxICUNameSize];
        localSize = uloc_getDisplayLanguage(value, locale, localName, __kCFMaxICUNameSize, &localStatus);
        if (U_FAILURE(localStatus) || size <= 0 || localStatus == U_USING_DEFAULT_WARNING) {
            return false;
        }
    }

    // This locale is OK, so use the result.
    *out = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, (UniChar*)name, size);
    return (*out != NULL);
}

static bool __CFLocaleLanguageName(const char* locale, const char* value, CFStringRef* out) {
    size_t len = strlen(value);
    if (len >= 2 && len <= 3) {
        return __CFLocaleICUName(locale, value, out, uloc_getDisplayLanguage);
    }
    return false;
}

static bool __CFLocaleCountryName(const char* locale, const char* value, CFStringRef* out) {
    // Need to make a fake locale ID
    char lid[ULOC_FULLNAME_CAPACITY];
    if (strlen(value) == 2) {
        snprintf(lid, sizeof(lid), "en_%s", value);
        return __CFLocaleICUName(locale, lid, out, uloc_getDisplayCountry);
    }
    return false;
}

static bool __CFLocaleScriptName(const char* locale, const char* value, CFStringRef* out) {
    // Need to make a fake locale ID
    char lid[ULOC_FULLNAME_CAPACITY];
    if (strlen(value) == 4) {
        snprintf(lid, sizeof(lid), "en_%s_US", value);
        return __CFLocaleICUName(locale, lid, out, uloc_getDisplayScript);
    }
    return false;
}

static bool __CFLocaleVariantName(const char* locale, const char* value, CFStringRef* out) {
    // Need to make a fake locale ID
    char lid[ULOC_FULLNAME_CAPACITY + ULOC_KEYWORD_AND_VALUES_CAPACITY];
    if (strlen(value) < ULOC_FULLNAME_CAPACITY) {
        snprintf(lid, sizeof(lid), "en_US_%s", value);
        return __CFLocaleICUName(locale, lid, out, uloc_getDisplayVariant);
    }
    return false;
}

static bool __CFLocaleCalendarName(const char* locale, const char* value, CFStringRef* out) {
    return __CFLocaleICUKeywordValueName(locale, value, kCalendarKeyword, out);
}

static bool __CFLocaleCollationName(const char* locale, const char* value, CFStringRef* out) {
    return __CFLocaleICUKeywordValueName(locale, value, kCollationKeyword, out);
}

static bool __CFLocaleCurrencyShortName(const char* locale, const char* value, CFStringRef* out) {
    return __CFLocaleICUCurrencyName(locale, value, UCURR_SYMBOL_NAME, out);
}

static bool __CFLocaleCurrencyFullName(const char* locale, const char* value, CFStringRef* out) {
    return __CFLocaleICUCurrencyName(locale, value, UCURR_LONG_NAME, out);
}

static bool __CFLocaleNoName(const char* locale, const char* value, CFStringRef* out) {
    return false;
}

// Note string members start with an extra &, and are fixed up at init time.
static struct __CFLocaleKeyTableEntry __CFLocaleKeyTable[] = {
    {(CFStringRef)&kCFLocaleIdentifier, __CFLocaleCopyLocaleID, __CFLocaleFullName, NULL},
    {(CFStringRef)&kCFLocaleLanguageCode, __CFLocaleCopyCodes, __CFLocaleLanguageName, (CFStringRef)&kCFLocaleLanguageCode},
    {(CFStringRef)&kCFLocaleCountryCode, __CFLocaleCopyCodes, __CFLocaleCountryName, (CFStringRef)&kCFLocaleCountryCode},
    {(CFStringRef)&kCFLocaleScriptCode, __CFLocaleCopyCodes, __CFLocaleScriptName, (CFStringRef)&kCFLocaleScriptCode},
    {(CFStringRef)&kCFLocaleVariantCode, __CFLocaleCopyCodes, __CFLocaleVariantName, (CFStringRef)&kCFLocaleVariantCode},
    {(CFStringRef)&kCFLocaleExemplarCharacterSet, __CFLocaleCopyExemplarCharSet, __CFLocaleNoName, NULL},
    {(CFStringRef)&kCFLocaleCalendarIdentifier, __CFLocaleCopyCalendarID, __CFLocaleCalendarName, NULL},
    {(CFStringRef)&kCFLocaleCalendar, __CFLocaleCopyCalendar, __CFLocaleNoName, NULL},
    {(CFStringRef)&kCFLocaleCollationIdentifier, __CFLocaleCopyCollationID, __CFLocaleCollationName, NULL},
    {(CFStringRef)&kCFLocaleUsesMetricSystem, __CFLocaleCopyUsesMetric, __CFLocaleNoName, NULL},
    {(CFStringRef)&kCFLocaleMeasurementSystem, __CFLocaleCopyMeasurementSystem, __CFLocaleNoName, NULL},
    {(CFStringRef)&kCFLocaleDecimalSeparator, __CFLocaleCopyNumberFormat, __CFLocaleNoName, (CFStringRef)&kCFNumberFormatterDecimalSeparator},
    {(CFStringRef)&kCFLocaleGroupingSeparator, __CFLocaleCopyNumberFormat, __CFLocaleNoName, (CFStringRef)&kCFNumberFormatterGroupingSeparator},
    {(CFStringRef)&kCFLocaleCurrencySymbol, __CFLocaleCopyNumberFormat2, __CFLocaleCurrencyShortName, (CFStringRef)&kCFNumberFormatterCurrencySymbol},
    {(CFStringRef)&kCFLocaleCurrencyCode, __CFLocaleCopyNumberFormat2, __CFLocaleCurrencyFullName, (CFStringRef)&kCFNumberFormatterCurrencyCode},
    {(CFStringRef)&_kCFLocaleCollatorID, __CFLocaleCopyCollatorID, __CFLocaleNoName, NULL},
};
enum {
    __kCFLocaleKeyTableCount = CF_COUNTOF(__CFLocaleKeyTable)
};

/*** CFLocale class ***/

static void __CFLocaleDeallocate(CFTypeRef cf) {
    CFLocaleRef locale = (CFLocaleRef)cf;
    CFRelease(locale->_identifier);
    if (locale->_cache) {
        CFRelease(locale->_cache);
    }
    if (locale->_overrides) {
        CFRelease(locale->_overrides);
    }
    if (locale->_prefs) {
        CFRelease(locale->_prefs);
    }
}

static Boolean __CFLocaleEqual(CFTypeRef cf1, CFTypeRef cf2) {
    CFLocaleRef locale1 = (CFLocaleRef)cf1;
    CFLocaleRef locale2 = (CFLocaleRef)cf2;
    // a user locale and a locale created with an ident are not the same even if their contents are
    if (__CFLocaleGetType(locale1) != __CFLocaleGetType(locale2)) {
        return false;
    }
    if (!CFEqual(locale1->_identifier, locale2->_identifier)) {
        return false;
    }
    if (!locale1->_overrides && locale2->_overrides) {
        return false;
    }
    if (locale1->_overrides && !locale2->_overrides) {
        return false;
    }
    if (locale1->_overrides && !CFEqual(locale1->_overrides, locale2->_overrides)) {
        return false;
    }
    if (__kCFLocaleUser == __CFLocaleGetType(locale1)) {
        return CFEqual(locale1->_prefs, locale2->_prefs);
    }
    return true;
}

static CFHashCode __CFLocaleHash(CFTypeRef cf) {
    CFLocaleRef locale = (CFLocaleRef)cf;
    return CFHash(locale->_identifier);
}

static CFStringRef __CFLocaleCopyDescription(CFTypeRef cf) {
    CFLocaleRef locale = (CFLocaleRef)cf;
    const char* type = NULL;
    switch (__CFLocaleGetType(locale)) {
        case __kCFLocaleOrdinary: type = "ordinary"; break;
        case __kCFLocaleSystem: type = "system"; break;
        case __kCFLocaleUser: type = "user"; break;
        case __kCFLocaleCustom: type = "custom"; break;
    }
    return CFStringCreateWithFormat(CFGetAllocator(locale), NULL, CFSTR("<CFLocale %p [%p]>{type = %s, identifier = '%@'}"), cf, CFGetAllocator(locale), type, locale->_identifier);
}

static const CFRuntimeClass __CFLocaleClass = {
    0,
    "CFLocale",
    NULL, // init
    NULL, // copy
    __CFLocaleDeallocate,
    __CFLocaleEqual,
    __CFLocaleHash,
    NULL,
    __CFLocaleCopyDescription
};

///////////////////////////////////////////////////////////////////// internal

CONST_STRING_DECL(_kCFLocaleCollatorID, "locale:collator id")

CF_INTERNAL void _CFLocaleInitialize(void) {
    CFIndex idx;
    __kCFLocaleTypeID = _CFRuntimeRegisterClass(&__CFLocaleClass);
    for (idx = 0; idx < __kCFLocaleKeyTableCount; idx++) {
        // table fixup to workaround compiler/language limitations
        __CFLocaleKeyTable[idx].key = *((CFStringRef*)__CFLocaleKeyTable[idx].key);
        if (__CFLocaleKeyTable[idx].context) {
            __CFLocaleKeyTable[idx].context = *((CFStringRef*)__CFLocaleKeyTable[idx].context);
        }
    }
}

CF_INTERNAL CFDictionaryRef _CFLocaleGetPrefs(CFLocaleRef locale) {
    return locale->_prefs;
}

///////////////////////////////////////////////////////////////////// public

CONST_STRING_DECL(kCFLocaleCurrentLocaleDidChangeNotification,
				  "kCFLocaleCurrentLocaleDidChangeNotification")

// Remember to keep the names such that they would make sense for the user locale,
// in addition to the others; for example, it is "Currency", not "DefaultCurrency".
// (And besides, "Default" is almost always implied.)  Words like "Default" and
// "Preferred" and so on should be left out of the names.
CONST_STRING_DECL(kCFLocaleIdentifier, "locale:id")
CONST_STRING_DECL(kCFLocaleLanguageCode, "locale:language code")
CONST_STRING_DECL(kCFLocaleCountryCode, "locale:country code")
CONST_STRING_DECL(kCFLocaleScriptCode, "locale:script code")
CONST_STRING_DECL(kCFLocaleVariantCode, "locale:variant code")
CONST_STRING_DECL(kCFLocaleExemplarCharacterSet, "locale:exemplar characters")
CONST_STRING_DECL(kCFLocaleCalendarIdentifier, "calendar")
CONST_STRING_DECL(kCFLocaleCalendar, "locale:calendarref")
CONST_STRING_DECL(kCFLocaleCollationIdentifier, "collation")
CONST_STRING_DECL(kCFLocaleUsesMetricSystem, "locale:uses metric")
CONST_STRING_DECL(kCFLocaleMeasurementSystem, "locale:measurement system")
CONST_STRING_DECL(kCFLocaleDecimalSeparator, "locale:decimal separator")
CONST_STRING_DECL(kCFLocaleGroupingSeparator, "locale:grouping separator")
CONST_STRING_DECL(kCFLocaleCurrencySymbol, "locale:currency symbol")
CONST_STRING_DECL(kCFLocaleCurrencyCode, "currency")

CONST_STRING_DECL(kCFGregorianCalendar, "gregorian")
CONST_STRING_DECL(kCFBuddhistCalendar, "buddhist")
CONST_STRING_DECL(kCFJapaneseCalendar, "japanese")
CONST_STRING_DECL(kCFIslamicCalendar, "islamic")
CONST_STRING_DECL(kCFIslamicCivilCalendar, "islamic-civil")
CONST_STRING_DECL(kCFHebrewCalendar, "hebrew")
CONST_STRING_DECL(kCFChineseCalendar, "chinese")

CFTypeID CFLocaleGetTypeID(void) {
    return __kCFLocaleTypeID;
}

CFLocaleRef CFLocaleGetSystem(void) {
    CFLocaleRef locale;
    __CFLocaleLockGlobal();
    if (!__CFLocaleSystem) {
        __CFLocaleUnlockGlobal();
        locale = CFLocaleCreate(kCFAllocatorSystemDefault, CFSTR(""));
        if (!locale) {
            return NULL;
        }
        __CFLocaleSetType(locale, __kCFLocaleSystem);
        __CFLocaleLockGlobal();
        if (!__CFLocaleSystem) {
            __CFLocaleSystem = locale;
        } else {
            if (locale) {
                CFRelease(locale);
            }
        }
    }
    locale = __CFLocaleSystem ? (CFLocaleRef)CFRetain(__CFLocaleSystem) : NULL;
    __CFLocaleUnlockGlobal();
    return locale;
}

CFLocaleRef CFLocaleCopyCurrent(void) {

    __CFLocaleLockGlobal();
    if (__CFLocaleCurrent) {
        CFRetain(__CFLocaleCurrent);
        __CFLocaleUnlockGlobal();
        return __CFLocaleCurrent;
    }
    __CFLocaleUnlockGlobal();

    CFDictionaryRef prefs = NULL;
    CFStringRef identifier = NULL;

    __CFLocale* locale;
    uint32_t size = sizeof(__CFLocale) - sizeof(CFRuntimeBase);
    locale = (__CFLocale*)_CFRuntimeCreateInstance(kCFAllocatorSystemDefault, CFLocaleGetTypeID(), size, NULL);
    if (!locale) {
        return NULL;
    }
    __CFLocaleSetType(locale, __kCFLocaleUser);
    if (!identifier) {
        identifier = (CFStringRef)CFRetain(FALLBACK_LOCALE_NAME);
    }
    locale->_identifier = identifier;
    locale->_cache = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, NULL, &kCFTypeDictionaryValueCallBacks);
    locale->_overrides = NULL;
    locale->_prefs = prefs;
    locale->_lock = CFSpinLockInit;

    __CFLocaleLockGlobal();
    if (!__CFLocaleCurrent) {
        __CFLocaleCurrent = locale;
    } else {
        CFRelease(locale);
    }
    locale = (__CFLocale*)CFRetain(__CFLocaleCurrent);
    __CFLocaleUnlockGlobal();
    return locale;
}

CFLocaleRef CFLocaleCreate(CFAllocatorRef allocator, CFStringRef identifier) {
    if (allocator == NULL) {
        allocator = CFAllocatorGetDefault();
    }
    CF_VALIDATE_OBJECT_ARG(CF, allocator, CFAllocatorGetTypeID());
    CF_VALIDATE_OBJECT_ARG(CF, identifier, CFStringGetTypeID());
    CFStringRef localeIdentifier = NULL;
    if (identifier) {
        localeIdentifier = CFLocaleCreateCanonicalLocaleIdentifierFromString(allocator, identifier);
    }
    if (!localeIdentifier) {
        return NULL;
    }
    CFStringRef old = localeIdentifier;
    localeIdentifier = (CFStringRef)CFStringCreateCopy(allocator, localeIdentifier);
    CFRelease(old);
    __CFLocaleLockGlobal();
    // Look for cases where we can return a cached instance.
    // We only use cached objects if the allocator is the system
    // default allocator.
    if (!allocator) {
        allocator = CFAllocatorGetDefault();
    }
    Boolean canCache = (kCFAllocatorSystemDefault == allocator);
    if (canCache && __CFLocaleCache) {
        CFLocaleRef locale = (CFLocaleRef)CFDictionaryGetValue(__CFLocaleCache, localeIdentifier);
        if (locale) {
            CFRetain(locale);
            __CFLocaleUnlockGlobal();
            CFRelease(localeIdentifier);
            return locale;
        }
    }
    __CFLocale* locale = NULL;
    uint32_t size = sizeof(__CFLocale) - sizeof(CFRuntimeBase);
    locale = (__CFLocale*)_CFRuntimeCreateInstance(allocator, CFLocaleGetTypeID(), size, NULL);
    if (!locale) {
        return NULL;
    }
    __CFLocaleSetType(locale, __kCFLocaleOrdinary);
    locale->_identifier = localeIdentifier;
    locale->_cache = CFDictionaryCreateMutable(allocator, 0, NULL, &kCFTypeDictionaryValueCallBacks);
    locale->_overrides = NULL;
    locale->_prefs = NULL;

    locale->_lock = CFSpinLockInit;

    if (canCache) {
        if (!__CFLocaleCache) {
            __CFLocaleCache = CFDictionaryCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        }
        CFDictionarySetValue(__CFLocaleCache, localeIdentifier, locale);
    }
    __CFLocaleUnlockGlobal();
    return (CFLocaleRef)locale;
}

CFLocaleRef CFLocaleCreateCopy(CFAllocatorRef allocator, CFLocaleRef locale) {
    return (CFLocaleRef)CFRetain(locale);
}

CFStringRef CFLocaleGetIdentifier(CFLocaleRef locale) {
    CF_OBJC_FUNCDISPATCH(CFStringRef, locale, "localeIdentifier");
    return locale->_identifier;
}

CFTypeRef CFLocaleGetValue(CFLocaleRef locale, CFStringRef key) {
    CF_OBJC_FUNCDISPATCH(CFTypeRef, locale, "objectForKey:", key);
    CFIndex idx, slot = -1;
    for (idx = 0; idx < __kCFLocaleKeyTableCount; idx++) {
        if (__CFLocaleKeyTable[idx].key == key) {
            slot = idx;
            break;
        }
    }
    if (-1 == slot && key) {
        for (idx = 0; idx < __kCFLocaleKeyTableCount; idx++) {
            if (CFEqual(__CFLocaleKeyTable[idx].key, key)) {
                slot = idx;
                break;
            }
        }
    }
    if (-1 == slot) {
        return NULL;
    }
    CFTypeRef value;
    if (locale->_overrides && CFDictionaryGetValueIfPresent(locale->_overrides, __CFLocaleKeyTable[slot].key, &value)) {
        return value;
    }
    __CFLocaleLock(locale);
    if (CFDictionaryGetValueIfPresent(locale->_cache, __CFLocaleKeyTable[slot].key, &value)) {
        __CFLocaleUnlock(locale);
        return value;
    }
    if (__kCFLocaleUser == __CFLocaleGetType(locale) && __CFLocaleKeyTable[slot].get(locale, true, &value, __CFLocaleKeyTable[slot].context)) {
        if (value) {
            CFDictionarySetValue(locale->_cache, __CFLocaleKeyTable[idx].key, value);
        }
        if (value) {
            CFRelease(value);
        }
        __CFLocaleUnlock(locale);
        return value;
    }
    if (__CFLocaleKeyTable[slot].get(locale, false, &value, __CFLocaleKeyTable[slot].context)) {
        if (value) {
            CFDictionarySetValue(locale->_cache, __CFLocaleKeyTable[idx].key, value);
        }
        if (value) {
            CFRelease(value);
        }
        __CFLocaleUnlock(locale);
        return value;
    }
    __CFLocaleUnlock(locale);
    return NULL;
}

CFStringRef CFLocaleCopyDisplayNameForPropertyValue(CFLocaleRef displayLocale, CFStringRef key, CFStringRef value) {
    CF_OBJC_FUNCDISPATCH(CFStringRef, displayLocale, "_copyDisplayNameForKey:value:", key, value);
    CFIndex idx, slot = -1;
    for (idx = 0; idx < __kCFLocaleKeyTableCount; idx++) {
        if (__CFLocaleKeyTable[idx].key == key) {
            slot = idx;
            break;
        }
    }
    if (-1 == slot && key) {
        for (idx = 0; idx < __kCFLocaleKeyTableCount; idx++) {
            if (CFEqual(__CFLocaleKeyTable[idx].key, key)) {
                slot = idx;
                break;
            }
        }
    }
    if (-1 == slot || !value) {
        return NULL;
    }
    // Get the locale ID as a C string
    char localeID[ULOC_FULLNAME_CAPACITY + ULOC_KEYWORD_AND_VALUES_CAPACITY];
    char cValue[ULOC_FULLNAME_CAPACITY + ULOC_KEYWORD_AND_VALUES_CAPACITY];
    if (CFStringGetCString(displayLocale->_identifier, localeID, sizeof(localeID) / sizeof(localeID[0]), kCFStringEncodingASCII) && CFStringGetCString(value, cValue, sizeof(cValue) / sizeof(char), kCFStringEncodingASCII)) {
        CFStringRef result;
        if ((!displayLocale->_prefs) && __CFLocaleKeyTable[slot].name(localeID, cValue, &result)) {
            return result;
        }

        // We could not find a result using the requested language. Fall back through all preferred languages.
        CFArrayRef langPref;
        if (displayLocale->_prefs) {
            langPref = (CFArrayRef)CFDictionaryGetValue(displayLocale->_prefs, CFSTR("AppleLanguages"));
            if (langPref) {
                CFRetain(langPref);
            }
        } else {
            //TODO CFPreferencesCopyAppValue in CFLocaleCopyDisplayNameForPropertyValue
            langPref = NULL; //(CFArrayRef)CFPreferencesCopyAppValue(CFSTR("AppleLanguages"), kCFPreferencesCurrentApplication);
        }
        if (langPref != NULL) {
            CFIndex count = CFArrayGetCount(langPref);
            CFIndex i;
            bool success = false;
            for (i = 0; i < count && !success; ++i) {
                CFStringRef language = (CFStringRef)CFArrayGetValueAtIndex(langPref, i);
                CFStringRef cleanLanguage = CFLocaleCreateCanonicalLanguageIdentifierFromString(kCFAllocatorSystemDefault, language);
                if (CFStringGetCString(cleanLanguage, localeID, sizeof(localeID) / sizeof(localeID[0]), kCFStringEncodingASCII)) {
                    success = __CFLocaleKeyTable[slot].name(localeID, cValue, &result);
                }
                CFRelease(cleanLanguage);
            }
            CFRelease(langPref);
            if (success) {
                return result;
            }
        }
    }
    return NULL;
}

CFArrayRef CFLocaleCopyAvailableLocaleIdentifiers(void) {
    int32_t locale, localeCount = uloc_countAvailable();
    CFMutableSetRef working = CFSetCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeSetCallBacks);
    for (locale = 0; locale < localeCount; ++locale) {
        const char* localeID = uloc_getAvailable(locale);
        CFStringRef string1 = CFStringCreateWithCString(kCFAllocatorSystemDefault, localeID, kCFStringEncodingASCII);
        CFStringRef string2 = CFLocaleCreateCanonicalLocaleIdentifierFromString(kCFAllocatorSystemDefault, string1);
        CFSetAddValue(working, string1);
        // do not include canonicalized version as IntlFormats cannot cope with that in its popup
        CFRelease(string1);
        CFRelease(string2);
    }
    CFIndex cnt = CFSetGetCount(working);
    _CF_ARRAY_ALLOCA(const void*, buffer, cnt);
    CFSetGetValues(working, buffer);
    CFArrayRef result = CFArrayCreate(kCFAllocatorSystemDefault, buffer, cnt, &kCFTypeArrayCallBacks);
    CFRelease(working);
    return result;
}

CFArrayRef CFLocaleCopyISOLanguageCodes(void) {
    const char* const* p = uloc_getISOLanguages();
    return __CFLocaleCopyCStringsAsArray(p);
}

CFArrayRef CFLocaleCopyISOCountryCodes(void) {
    const char* const* p = uloc_getISOCountries();
    return __CFLocaleCopyCStringsAsArray(p);
}

CFArrayRef CFLocaleCopyISOCurrencyCodes(void) {
    UErrorCode icuStatus = U_ZERO_ERROR;
    UEnumeration* enumer = ucurr_openISOCurrencies(UCURR_ALL, &icuStatus);
    CFArrayRef result = __CFLocaleCopyUEnumerationAsArray(enumer, &icuStatus);
    uenum_close(enumer);
    return result;
}

CFArrayRef CFLocaleCopyCommonISOCurrencyCodes(void) {
    UErrorCode icuStatus = U_ZERO_ERROR;
    UEnumeration* enumer = ucurr_openISOCurrencies(UCURR_COMMON | UCURR_NON_DEPRECATED, &icuStatus);
    CFArrayRef result = __CFLocaleCopyUEnumerationAsArray(enumer, &icuStatus);
    uenum_close(enumer);
    return result;
}

CFArrayRef CFLocaleCopyPreferredLanguages(void) {
    CFMutableArrayRef newArray = CFArrayCreateMutable(kCFAllocatorSystemDefault, 0, &kCFTypeArrayCallBacks);

    //TODO CFPreferencesCopyAppValue in CFLocaleCopyPreferredLanguages
    CFArrayRef languagesArray = NULL; //(CFArrayRef)CFPreferencesCopyAppValue(CFSTR("AppleLanguages"), kCFPreferencesCurrentApplication);
    if (languagesArray && (CFArrayGetTypeID() == CFGetTypeID(languagesArray))) {
        for (CFIndex idx = 0, cnt = CFArrayGetCount(languagesArray); idx < cnt; idx++) {
            CFStringRef str = (CFStringRef)CFArrayGetValueAtIndex(languagesArray, idx);
            if (str && (CFStringGetTypeID() == CFGetTypeID(str))) {
                CFStringRef ident = CFLocaleCreateCanonicalLanguageIdentifierFromString(kCFAllocatorSystemDefault, str);
                CFArrayAppendValue(newArray, ident);
                CFRelease(ident);
            }
        }
    }
    if (languagesArray) {
        CFRelease(languagesArray);
    }
    return newArray;
}


