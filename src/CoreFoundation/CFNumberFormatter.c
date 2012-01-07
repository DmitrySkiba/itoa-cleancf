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

#include <CoreFoundation/CFNumberFormatter.h>
#include "CFInternal.h"
#include <unicode/unum.h>
#include <unicode/ucurr.h>
#include <math.h>
#include <float.h>
#include "CFICUUtilities.h"

struct __CFNumberFormatter {
    CFRuntimeBase _base;
    UNumberFormat* _nf;
    CFLocaleRef _locale;
    CFNumberFormatterStyle _style;
    CFStringRef _format; // NULL for RBNFs
    CFStringRef _defformat;
    CFNumberRef _multiplier;
    CFStringRef _zeroSym;
};

static CFTypeID __kCFNumberFormatterTypeID = _kCFRuntimeNotATypeID;

///////////////////////////////////////////////////////////////////// private

static void __CFNumberFormatterApplySymbolPrefs(const void* key, const void* value, void* context) {
    if (CFGetTypeID(key) == CFStringGetTypeID() && CFGetTypeID(value) == CFStringGetTypeID()) {
        CFNumberFormatterRef formatter = (CFNumberFormatterRef)context;
        UNumberFormatSymbol sym = (UNumberFormatSymbol)CFStringGetIntValue((CFStringRef)key);
        CFStringRef item = (CFStringRef)value;
        CFIndex item_cnt = CFStringGetLength(item);
        _CF_ARRAY_ALLOCA(UChar, item_buffer, item_cnt);
        UChar* item_ustr = (UChar*)CFStringGetCharactersPtr(item);
        if (!item_ustr) {
            CFStringGetCharacters(item, CFRangeMake(0, _CFMin(ICU_BUFFER_SIZE, item_cnt)), (UniChar*)item_buffer);
            item_ustr = item_buffer;
        }
        UErrorCode status = U_ZERO_ERROR;
        unum_setSymbol(formatter->_nf, sym, item_ustr, item_cnt, &status);
    }
}

static void __CFNumberFormatterSubstituteFormatStringFromPrefsNF(CFNumberFormatterRef formatter) {
    CFIndex formatStyle = formatter->_style;
	if (formatStyle == kCFNumberFormatterSpellOutStyle ||
		formatStyle == kCFNumberFormatterNoStyle)
	{
		return;
	}

    CFStringRef prefName = CFSTR("AppleICUNumberFormatStrings");
    CFStringRef pref = NULL;
    CFDictionaryRef prefs = _CFLocaleGetPrefs(formatter->_locale);
    CFPropertyListRef metapref = prefs ? CFDictionaryGetValue(prefs, prefName) : NULL;
    if (metapref && CFGetTypeID(metapref) == CFDictionaryGetTypeID()) {
        CFStringRef key;
        switch (formatStyle) {
            case kCFNumberFormatterDecimalStyle: key = CFSTR("1"); break;
            case kCFNumberFormatterCurrencyStyle: key = CFSTR("2"); break;
            case kCFNumberFormatterPercentStyle: key = CFSTR("3"); break;
            case kCFNumberFormatterScientificStyle: key = CFSTR("4"); break;
            case kCFNumberFormatterSpellOutStyle: key = CFSTR("5"); break;
            default: key = CFSTR("0"); break;
        }
        pref = (CFStringRef)CFDictionaryGetValue((CFDictionaryRef)metapref, key);
    }
    if (!pref || CFGetTypeID(pref) != CFStringGetTypeID()) {
		return;
	}

    int32_t icustyle = UNUM_IGNORE;
    switch (formatStyle) {
        case kCFNumberFormatterDecimalStyle: icustyle = UNUM_DECIMAL; break;
        case kCFNumberFormatterCurrencyStyle: icustyle = UNUM_CURRENCY; break;
        case kCFNumberFormatterPercentStyle: icustyle = UNUM_PERCENT; break;
        case kCFNumberFormatterScientificStyle: icustyle = UNUM_SCIENTIFIC; break;
        case kCFNumberFormatterSpellOutStyle: icustyle = UNUM_SPELLOUT; break;
    }

	const char* localeName;
	CFICU_LOCALE_GET_NAME_CSTR(formatter->_locale, localeName);
	if (!localeName) {
		return;
	}

    UErrorCode status = U_ZERO_ERROR;
    UNumberFormat* nf = unum_open((UNumberFormatStyle)icustyle, NULL, 0, localeName, NULL, &status);
    if (nf) {
        UChar ubuffer[ICU_BUFFER_SIZE];
        status = U_ZERO_ERROR;
        int32_t number_len = unum_toPattern(nf, false, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && number_len <= ICU_BUFFER_SIZE) {
            CFStringRef numberString = CFStringCreateWithCharacters(kCFAllocatorSystemDefault, (const UniChar*)ubuffer, number_len);
            status = U_ZERO_ERROR;
            int32_t formatter_len = unum_toPattern(formatter->_nf, false, ubuffer, ICU_BUFFER_SIZE, &status);
            if (U_SUCCESS(status) && formatter_len <= ICU_BUFFER_SIZE) {
                CFMutableStringRef formatString = CFStringCreateMutable(kCFAllocatorSystemDefault, 0);
                CFStringAppendCharacters(formatString, (const UniChar*)ubuffer, formatter_len);
                // find numberString inside formatString, substitute the pref in that range
                CFRange result;
                if (CFStringFindWithOptions(formatString, numberString, CFRangeMake(0, formatter_len), 0, &result)) {
                    CFStringReplace(formatString, result, pref);
                    int32_t new_len = CFStringGetLength(formatString);
                    _CF_ARRAY_ALLOCA(UChar, new_buffer, new_len);
                    const UChar* new_ustr = (const UChar*)CFStringGetCharactersPtr(formatString);
                    if (!new_ustr) {
                        CFStringGetCharacters(formatString, CFRangeMake(0, new_len), (UniChar*)new_buffer);
                        new_ustr = new_buffer;
                    }
                    status = U_ZERO_ERROR;
                    unum_applyPattern(formatter->_nf, false, new_ustr, new_len, NULL, &status);
                }
                CFRelease(formatString);
            }
            CFRelease(numberString);
        }
        unum_close(nf);
    }
}
static void __CFNumberFormatterCustomize(CFNumberFormatterRef formatter) {
	__CFNumberFormatterSubstituteFormatStringFromPrefsNF(formatter);
	CFDictionaryRef prefs = _CFLocaleGetPrefs(formatter->_locale);
    CFPropertyListRef metapref = prefs ? CFDictionaryGetValue(prefs, CFSTR("AppleICUNumberSymbols")) : NULL;
    if (metapref && CFGetTypeID(metapref) == CFDictionaryGetTypeID()) {
        CFDictionaryApplyFunction((CFDictionaryRef)metapref, __CFNumberFormatterApplySymbolPrefs, formatter);
    }
}

/*** CFLocale class ***/

static CFStringRef __CFNumberFormatterCopyDescription(CFTypeRef cf) {
    CFNumberFormatterRef formatter = (CFNumberFormatterRef)cf;
    return CFStringCreateWithFormat(
		CFGetAllocator(formatter),
		NULL, CFSTR("<CFNumberFormatter %p [%p]>"),
		cf, CFGetAllocator(formatter));
}

static void __CFNumberFormatterDeallocate(CFTypeRef cf) {
    CFNumberFormatterRef formatter = (CFNumberFormatterRef)cf;
    if (formatter->_nf) {
        unum_close(formatter->_nf);
    }
    if (formatter->_locale) {
        CFRelease(formatter->_locale);
    }
    if (formatter->_format) {
        CFRelease(formatter->_format);
    }
    if (formatter->_defformat) {
        CFRelease(formatter->_defformat);
    }
    if (formatter->_multiplier) {
        CFRelease(formatter->_multiplier);
    }
    if (formatter->_zeroSym) {
        CFRelease(formatter->_zeroSym);
    }
}

static const CFRuntimeClass __CFNumberFormatterClass = {
    0,
    "CFNumberFormatter",
    NULL, // init
    NULL, // copy
    __CFNumberFormatterDeallocate,
    NULL,
    NULL,
    NULL,
    __CFNumberFormatterCopyDescription
};

///////////////////////////////////////////////////////////////////// internal

CF_INTERNAL void _CFNumberFormatterInitialize(void) {
    __kCFNumberFormatterTypeID = _CFRuntimeRegisterClass(&__CFNumberFormatterClass);
}

///////////////////////////////////////////////////////////////////// public

CONST_STRING_DECL(kCFNumberFormatterCurrencyCode, "kCFNumberFormatterCurrencyCode")
CONST_STRING_DECL(kCFNumberFormatterDecimalSeparator, "kCFNumberFormatterDecimalSeparator")
CONST_STRING_DECL(kCFNumberFormatterCurrencyDecimalSeparator, "kCFNumberFormatterCurrencyDecimalSeparator")
CONST_STRING_DECL(kCFNumberFormatterAlwaysShowDecimalSeparator, "kCFNumberFormatterAlwaysShowDecimalSeparator")
CONST_STRING_DECL(kCFNumberFormatterGroupingSeparator, "kCFNumberFormatterGroupingSeparator")
CONST_STRING_DECL(kCFNumberFormatterUseGroupingSeparator, "kCFNumberFormatterUseGroupingSeparator")
CONST_STRING_DECL(kCFNumberFormatterPercentSymbol, "kCFNumberFormatterPercentSymbol")
CONST_STRING_DECL(kCFNumberFormatterZeroSymbol, "kCFNumberFormatterZeroSymbol")
CONST_STRING_DECL(kCFNumberFormatterNaNSymbol, "kCFNumberFormatterNaNSymbol")
CONST_STRING_DECL(kCFNumberFormatterInfinitySymbol, "kCFNumberFormatterInfinitySymbol")
CONST_STRING_DECL(kCFNumberFormatterMinusSign, "kCFNumberFormatterMinusSignSymbol")
CONST_STRING_DECL(kCFNumberFormatterPlusSign, "kCFNumberFormatterPlusSignSymbol")
CONST_STRING_DECL(kCFNumberFormatterCurrencySymbol, "kCFNumberFormatterCurrencySymbol")
CONST_STRING_DECL(kCFNumberFormatterExponentSymbol, "kCFNumberFormatterExponentSymbol")
CONST_STRING_DECL(kCFNumberFormatterMinIntegerDigits, "kCFNumberFormatterMinIntegerDigits")
CONST_STRING_DECL(kCFNumberFormatterMaxIntegerDigits, "kCFNumberFormatterMaxIntegerDigits")
CONST_STRING_DECL(kCFNumberFormatterMinFractionDigits, "kCFNumberFormatterMinFractionDigits")
CONST_STRING_DECL(kCFNumberFormatterMaxFractionDigits, "kCFNumberFormatterMaxFractionDigits")
CONST_STRING_DECL(kCFNumberFormatterGroupingSize, "kCFNumberFormatterGroupingSize")
CONST_STRING_DECL(kCFNumberFormatterSecondaryGroupingSize, "kCFNumberFormatterSecondaryGroupingSize")
CONST_STRING_DECL(kCFNumberFormatterRoundingMode, "kCFNumberFormatterRoundingMode")
CONST_STRING_DECL(kCFNumberFormatterRoundingIncrement, "kCFNumberFormatterRoundingIncrement")
CONST_STRING_DECL(kCFNumberFormatterFormatWidth, "kCFNumberFormatterFormatWidth")
CONST_STRING_DECL(kCFNumberFormatterPaddingPosition, "kCFNumberFormatterPaddingPosition")
CONST_STRING_DECL(kCFNumberFormatterPaddingCharacter, "kCFNumberFormatterPaddingCharacter")
CONST_STRING_DECL(kCFNumberFormatterDefaultFormat, "kCFNumberFormatterDefaultFormat")

CONST_STRING_DECL(kCFNumberFormatterMultiplier, "kCFNumberFormatterMultiplier")
CONST_STRING_DECL(kCFNumberFormatterPositivePrefix, "kCFNumberFormatterPositivePrefix")
CONST_STRING_DECL(kCFNumberFormatterPositiveSuffix, "kCFNumberFormatterPositiveSuffix")
CONST_STRING_DECL(kCFNumberFormatterNegativePrefix, "kCFNumberFormatterNegativePrefix")
CONST_STRING_DECL(kCFNumberFormatterNegativeSuffix, "kCFNumberFormatterNegativeSuffix")
CONST_STRING_DECL(kCFNumberFormatterPerMillSymbol, "kCFNumberFormatterPerMillSymbol")
CONST_STRING_DECL(kCFNumberFormatterInternationalCurrencySymbol, "kCFNumberFormatterInternationalCurrencySymbol")

CONST_STRING_DECL(kCFNumberFormatterCurrencyGroupingSeparator, "kCFNumberFormatterCurrencyGroupingSeparator")
CONST_STRING_DECL(kCFNumberFormatterIsLenient, "kCFNumberFormatterIsLenient")
CONST_STRING_DECL(kCFNumberFormatterUseSignificantDigits, "kCFNumberFormatterUseSignificantDigits")
CONST_STRING_DECL(kCFNumberFormatterMinSignificantDigits, "kCFNumberFormatterMinSignificantDigits")
CONST_STRING_DECL(kCFNumberFormatterMaxSignificantDigits, "kCFNumberFormatterMaxSignificantDigits")

CFTypeID CFNumberFormatterGetTypeID(void) {
    return __kCFNumberFormatterTypeID;
}

CFNumberFormatterRef CFNumberFormatterCreate(CFAllocatorRef allocator,
											 CFLocaleRef locale,
											 CFNumberFormatterStyle style)
{
    struct __CFNumberFormatter* memory;
    uint32_t size = sizeof(struct __CFNumberFormatter) - sizeof(CFRuntimeBase);
    if (allocator == NULL) {
        allocator = CFAllocatorGetDefault();
    }
    CF_VALIDATE_OBJECT_ARG(CF, locale, CFLocaleGetTypeID());

    uint32_t ustyle;
    switch (style) {
        case kCFNumberFormatterNoStyle: ustyle = UNUM_IGNORE; break;
        case kCFNumberFormatterDecimalStyle: ustyle = UNUM_DECIMAL; break;
        case kCFNumberFormatterCurrencyStyle: ustyle = UNUM_CURRENCY; break;
        case kCFNumberFormatterPercentStyle: ustyle = UNUM_PERCENT; break;
        case kCFNumberFormatterScientificStyle: ustyle = UNUM_SCIENTIFIC; break;
        case kCFNumberFormatterSpellOutStyle: ustyle = UNUM_SPELLOUT; break;
        default:
            CF_VALIDATE_ARG(false, "unknown style %d", style);
            ustyle = UNUM_DECIMAL;
			style = kCFNumberFormatterDecimalStyle;
    }

    memory = (struct __CFNumberFormatter*)_CFRuntimeCreateInstance(allocator, CFNumberFormatterGetTypeID(), size, NULL);
    if (!memory) {
        return NULL;
    }
    memory->_nf = NULL;
    memory->_locale = NULL;
    memory->_format = NULL;
    memory->_defformat = NULL;
    memory->_multiplier = NULL;
    memory->_zeroSym = NULL;
    memory->_style = style;

	if (!locale) {
        locale = CFLocaleGetSystem();
    }

	const char* localeName = "";
	if (locale) {
		CFICU_LOCALE_GET_NAME_CSTR(locale, localeName);
		if (!localeName) {
			CFRelease(memory);
			return NULL;
		}
	}

    UErrorCode status = U_ZERO_ERROR;
    memory->_nf = unum_open((UNumberFormatStyle)ustyle, NULL, 0, localeName, NULL, &status);
    CF_ASSERT_XXX(memory->_nf, "error (%d) creating number formatter", status);
    if (!memory->_nf) {
        CFRelease(memory);
        return NULL;
    }
    UChar ubuff[4];
    if (kCFNumberFormatterNoStyle == style) {
        status = U_ZERO_ERROR;
        ubuff[0] = '#'; ubuff[1] = ';'; ubuff[2] = '#';
        unum_applyPattern(memory->_nf, false, ubuff, 3, NULL, &status);
        unum_setAttribute(memory->_nf, UNUM_MAX_INTEGER_DIGITS, 42);
        unum_setAttribute(memory->_nf, UNUM_MAX_FRACTION_DIGITS, 0);
    }
    memory->_locale = locale ? CFLocaleCreateCopy(allocator, locale) : CFLocaleGetSystem();
    __CFNumberFormatterCustomize(memory);
    if (kCFNumberFormatterSpellOutStyle != memory->_style) {
        UChar ubuffer[ICU_BUFFER_SIZE];
        status = U_ZERO_ERROR;
        int32_t ret = unum_toPattern(memory->_nf, false, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && ret <= ICU_BUFFER_SIZE) {
            memory->_format = CFStringCreateWithCharacters(allocator, (const UniChar*)ubuffer, ret);
        }
    }
    memory->_defformat = memory->_format ? (CFStringRef)CFRetain(memory->_format) : NULL;
    if (kCFNumberFormatterSpellOutStyle != memory->_style) {
        int32_t n = unum_getAttribute(memory->_nf, UNUM_MULTIPLIER);
        if (1 != n) {
            memory->_multiplier = CFNumberCreate(allocator, kCFNumberSInt32Type, &n);
            unum_setAttribute(memory->_nf, UNUM_MULTIPLIER, 1);
        }
    }
    return (CFNumberFormatterRef)memory;
}

CFLocaleRef CFNumberFormatterGetLocale(CFNumberFormatterRef formatter) {
    CF_VALIDATE_OBJECT_ARG(CF, formatter, CFNumberFormatterGetTypeID());
    return formatter->_locale;
}

CFNumberFormatterStyle CFNumberFormatterGetStyle(CFNumberFormatterRef formatter) {
    CF_VALIDATE_OBJECT_ARG(CF, formatter, CFNumberFormatterGetTypeID());
    return formatter->_style;
}

CFStringRef CFNumberFormatterGetFormat(CFNumberFormatterRef formatter) {
    CF_VALIDATE_OBJECT_ARG(CF, formatter, CFNumberFormatterGetTypeID());
    if (kCFNumberFormatterSpellOutStyle == formatter->_style) {
        return NULL;
    }
    UChar ubuffer[ICU_BUFFER_SIZE];
    CFStringRef newString = NULL;
    UErrorCode status = U_ZERO_ERROR;
    int32_t ret = unum_toPattern(formatter->_nf, false, ubuffer, ICU_BUFFER_SIZE, &status);
    if (U_SUCCESS(status) && ret <= ICU_BUFFER_SIZE) {
        newString = CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, ret);
    }
    if (newString && !formatter->_format) {
        formatter->_format = newString;
    } else if (newString && !CFEqual(newString, formatter->_format)) {
        CFRelease(formatter->_format);
        formatter->_format = newString;
    } else if (newString) {
        CFRelease(newString);
    }
    return formatter->_format;
}

void CFNumberFormatterSetFormat(CFNumberFormatterRef formatter, CFStringRef formatString) {
    CF_VALIDATE_OBJECT_ARG(CF, formatter, CFNumberFormatterGetTypeID());
    CF_VALIDATE_OBJECT_ARG(CF, formatString, CFStringGetTypeID());
    if (kCFNumberFormatterSpellOutStyle == formatter->_style) {
        return;
    }
    CFIndex cnt = CFStringGetLength(formatString);
    CF_VALIDATE_ARG(cnt <= 1024, "format string too long");
    if ((!formatter->_format || !CFEqual(formatter->_format, formatString)) && cnt <= 1024) {
        _CF_ARRAY_ALLOCA(UChar, ubuffer, cnt);
        const UChar* ustr = (const UChar*)CFStringGetCharactersPtr(formatString);
        if (!ustr) {
            CFStringGetCharacters(formatString, CFRangeMake(0, cnt), (UniChar*)ubuffer);
            ustr = ubuffer;
        }
        UErrorCode status = U_ZERO_ERROR;
        unum_applyPattern(formatter->_nf, false, ustr, cnt, NULL, &status);
        if (U_SUCCESS(status)) {
            if (formatter->_format) {
                CFRelease(formatter->_format);
            }
            UChar ubuffer2[ICU_BUFFER_SIZE];
            status = U_ZERO_ERROR;
            int32_t ret = unum_toPattern(formatter->_nf, false, ubuffer2, ICU_BUFFER_SIZE, &status);
            if (U_SUCCESS(status) && ret <= ICU_BUFFER_SIZE) {
                formatter->_format = CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer2, ret);
            }
        }
    }
}

CFStringRef CFNumberFormatterCreateStringWithNumber(CFAllocatorRef allocator, CFNumberFormatterRef formatter, CFNumberRef number) {
    if (allocator == NULL) {
        allocator = CFAllocatorGetDefault();
    }
    CF_VALIDATE_OBJECT_ARG(CF, allocator, CFAllocatorGetTypeID());
    CF_VALIDATE_OBJECT_ARG(CF, formatter, CFNumberFormatterGetTypeID());
    CF_VALIDATE_OBJECT_ARG(CF, number, CFNumberGetTypeID());
    CFNumberType type = CFNumberGetType(number);
    char buffer[64];
    CFNumberGetValue(number, type, buffer);
    return CFNumberFormatterCreateStringWithValue(allocator, formatter, type, buffer);
}

CFStringRef CFNumberFormatterCreateStringWithValue(CFAllocatorRef allocator, CFNumberFormatterRef formatter, CFNumberType numberType, const void* valuePtr) {

	#define FORMAT(T, FUNC) \
		T value = *(T*)valuePtr; \
		if (!value && formatter->_zeroSym) { \
			return (CFStringRef)CFRetain(formatter->_zeroSym); \
		} \
		if (1.0 != multiplier) { \
			value = (T)(value * multiplier); \
		} \
		status = U_ZERO_ERROR; \
		used = FUNC(formatter->_nf, value, ubuffer, cnt, NULL, &status); \
		if (status == U_BUFFER_OVERFLOW_ERROR || cnt < used) { \
			cnt = used + 1; \
			ustr = (UChar*)CFAllocatorAllocate(kCFAllocatorSystemDefault, sizeof(UChar) * cnt, 0); \
			status = U_ZERO_ERROR; \
			used = FUNC(formatter->_nf, value, ustr, cnt, NULL, &status); \
		}

	/*****/

    if (allocator == NULL) {
        allocator = CFAllocatorGetDefault();
    }
    CF_VALIDATE_OBJECT_ARG(CF, allocator, CFAllocatorGetTypeID());
    CF_VALIDATE_OBJECT_ARG(CF, formatter, CFNumberFormatterGetTypeID());
    double multiplier = 1.0;
    if (formatter->_multiplier) {
        if (!CFNumberGetValue(formatter->_multiplier, kCFNumberFloat64Type, &multiplier)) {
            multiplier = 1.0;
        }
    }
    UChar* ustr = NULL, ubuffer[ICU_BUFFER_SIZE];
    UErrorCode status = U_ZERO_ERROR;
    CFIndex used, cnt = ICU_BUFFER_SIZE;
    if (numberType == kCFNumberFloat64Type || numberType == kCFNumberDoubleType) {
        FORMAT(double, unum_formatDouble)
    } else if (numberType == kCFNumberFloat32Type || numberType == kCFNumberFloatType) {
        FORMAT(float, unum_formatDouble)
    } else if (numberType == kCFNumberSInt64Type || numberType == kCFNumberLongLongType) {
        FORMAT(int64_t, unum_formatInt64)
    } else if (numberType == kCFNumberLongType || numberType == kCFNumberCFIndexType) {
#if __LP64__
        FORMAT(int64_t, unum_formatInt64)
#else
        FORMAT(int32_t, unum_formatInt64)
#endif
    } else if (numberType == kCFNumberSInt32Type || numberType == kCFNumberIntType) {
        FORMAT(int32_t, unum_formatInt64)
    } else if (numberType == kCFNumberSInt16Type || numberType == kCFNumberShortType) {
        FORMAT(int16_t, unum_formatInt64)
    } else if (numberType == kCFNumberSInt8Type || numberType == kCFNumberCharType) {
        FORMAT(int8_t, unum_formatInt64)
    } else {
        CF_VALIDATE_ARG(false, "unknown CFNumberType (%d)", numberType);
        return NULL;
    }
    CFStringRef string = NULL;
    if (U_SUCCESS(status)) {
        string = CFStringCreateWithCharacters(allocator, ustr ? (const UniChar*)ustr : (const UniChar*)ubuffer, used);
    }
    if (ustr) {
        CFAllocatorDeallocate(kCFAllocatorSystemDefault, ustr);
    }
    return string;

	/*****/

	#undef FORMAT
}

CFNumberRef CFNumberFormatterCreateNumberFromString(CFAllocatorRef allocator, CFNumberFormatterRef formatter, CFStringRef string, CFRange* rangep, CFOptionFlags options) {
    if (allocator == NULL) {
        allocator = CFAllocatorGetDefault();
    }
    CF_VALIDATE_OBJECT_ARG(CF, allocator, CFAllocatorGetTypeID());
    CF_VALIDATE_OBJECT_ARG(CF, formatter, CFNumberFormatterGetTypeID());
    CF_VALIDATE_OBJECT_ARG(CF, string, CFStringGetTypeID());
    CFNumberType type = (options & kCFNumberFormatterParseIntegersOnly) ? kCFNumberSInt64Type : kCFNumberFloat64Type;
    char buffer[16];
    if (CFNumberFormatterGetValueFromString(formatter, string, rangep, type, buffer)) {
        return CFNumberCreate(allocator, type, buffer);
    }
    return NULL;
}

Boolean CFNumberFormatterGetValueFromString(CFNumberFormatterRef formatter, CFStringRef string, CFRange* rangep, CFNumberType numberType, void* valuePtr) {
    CF_VALIDATE_OBJECT_ARG(CF, formatter, CFNumberFormatterGetTypeID());
    CF_VALIDATE_OBJECT_ARG(CF, string, CFStringGetTypeID());
    Boolean isZero = false;
    CFRange range = {0, 0};
    if (rangep) {
        range = *rangep;
    } else {
        range.length = CFStringGetLength(string);
    }
    if (formatter->_zeroSym && kCFCompareEqualTo == CFStringCompareWithOptions(string, formatter->_zeroSym, range, 0)) {
        isZero = true;
    }
    if (1024 < range.length) {
        range.length = 1024;
    }
    const UChar* ustr = (const UChar*)CFStringGetCharactersPtr(string);
    _CF_ARRAY_ALLOCA(UChar, ubuffer, (!ustr) ? range.length : 1);
    if (!ustr) {
        CFStringGetCharacters(string, range, (UniChar*)ubuffer);
        ustr = ubuffer;
    } else {
        ustr += range.location;
    }
    Boolean integerOnly = 1;
    switch (numberType) {
        case kCFNumberSInt8Type: case kCFNumberCharType:
        case kCFNumberSInt16Type: case kCFNumberShortType:
        case kCFNumberSInt32Type: case kCFNumberIntType:
        case kCFNumberLongType: case kCFNumberCFIndexType:
        case kCFNumberSInt64Type: case kCFNumberLongLongType:
            unum_setAttribute(formatter->_nf, UNUM_PARSE_INT_ONLY, 1);
            break;
        default:
            unum_setAttribute(formatter->_nf, UNUM_PARSE_INT_ONLY, 0);
            integerOnly = 0;
            break;
    }
    int32_t dpos = 0;
    UErrorCode status = U_ZERO_ERROR;
    int64_t dreti = 0;
    double dretd = 0.0;
    if (isZero) {
        dpos = rangep ? rangep->length : 0;
    } else {
        if (integerOnly) {
            dreti = unum_parseInt64(formatter->_nf, ustr, range.length, &dpos, &status);
        } else {
            dretd = unum_parseDouble(formatter->_nf, ustr, range.length, &dpos, &status);
        }
    }
    if (rangep) {
        rangep->length = dpos;
    }
    if (U_FAILURE(status)) {
        return false;
    }
    if (formatter->_multiplier) {
        double multiplier = 1.0;
        if (!CFNumberGetValue(formatter->_multiplier, kCFNumberFloat64Type, &multiplier)) {
            multiplier = 1.0;
        }
        dreti = (int64_t)((double)dreti / multiplier); // integer truncation
        dretd = dretd / multiplier;
    }
    switch (numberType) {
        case kCFNumberSInt8Type: case kCFNumberCharType:
            if (INT8_MIN <= dreti && dreti <= INT8_MAX) {
                *(int8_t*)valuePtr = (int8_t)dreti;
                return true;
            }
            break;
        case kCFNumberSInt16Type: case kCFNumberShortType:
            if (INT16_MIN <= dreti && dreti <= INT16_MAX) {
                *(int16_t*)valuePtr = (int16_t)dreti;
                return true;
            }
            break;
        case kCFNumberSInt32Type: case kCFNumberIntType:
#if !__LP64__
        case kCFNumberLongType: case kCFNumberCFIndexType:
#endif
            if (INT32_MIN <= dreti && dreti <= INT32_MAX) {
                *(int32_t*)valuePtr = (int32_t)dreti;
                return true;
            }
            break;
        case kCFNumberSInt64Type: case kCFNumberLongLongType:
#if __LP64__
        case kCFNumberLongType: case kCFNumberCFIndexType:
#endif
            if (INT64_MIN <= dreti && dreti <= INT64_MAX) {
                *(int64_t*)valuePtr = (int64_t)dreti;
                return true;
            }
            break;
        case kCFNumberFloat32Type: case kCFNumberFloatType:
            if (-FLT_MAX <= dretd && dretd <= FLT_MAX) {
                *(float*)valuePtr = (float)dretd;
                return true;
            }
            break;
        case kCFNumberFloat64Type: case kCFNumberDoubleType:
            if (-DBL_MAX <= dretd && dretd <= DBL_MAX) {
                *(double*)valuePtr = (double)dretd;
                return true;
            }
            break;
    }
    return false;
}

void CFNumberFormatterSetProperty(CFNumberFormatterRef formatter, CFStringRef key, CFTypeRef value) {
	CF_VALIDATE_PTR_ARG(key);
	CF_VALIDATE_PTR_ARG(value);

    int32_t n;
    double d;
    UErrorCode status = U_ZERO_ERROR;
    UChar ubuffer[ICU_BUFFER_SIZE];
    CFIndex cnt;
    CF_VALIDATE_OBJECT_ARG(CF, formatter, CFNumberFormatterGetTypeID());
    CF_VALIDATE_OBJECT_ARG(CF, key, CFStringGetTypeID());
    if (kCFNumberFormatterCurrencyCode == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setTextAttribute(formatter->_nf, UNUM_CURRENCY_CODE, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterDecimalSeparator == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setSymbol(formatter->_nf, UNUM_DECIMAL_SEPARATOR_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterCurrencyDecimalSeparator == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setSymbol(formatter->_nf, UNUM_MONETARY_SEPARATOR_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterAlwaysShowDecimalSeparator == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFBooleanGetTypeID());
        unum_setAttribute(formatter->_nf, UNUM_DECIMAL_ALWAYS_SHOWN, (kCFBooleanTrue == value));
    } else if (kCFNumberFormatterGroupingSeparator == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setSymbol(formatter->_nf, UNUM_GROUPING_SEPARATOR_SYMBOL, (const UChar*)ubuffer, cnt, &status);
    } else if (kCFNumberFormatterUseGroupingSeparator == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFBooleanGetTypeID());
        unum_setAttribute(formatter->_nf, UNUM_GROUPING_USED, (kCFBooleanTrue == value));
    } else if (kCFNumberFormatterPercentSymbol == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setSymbol(formatter->_nf, UNUM_PERCENT_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterZeroSymbol == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        CFStringRef old = formatter->_zeroSym;
        formatter->_zeroSym = value ? (CFStringRef)CFRetain(value) : NULL;
        if (old) {
            CFRelease(old);
        }
    } else if (kCFNumberFormatterNaNSymbol == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setSymbol(formatter->_nf, UNUM_NAN_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterInfinitySymbol == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setSymbol(formatter->_nf, UNUM_INFINITY_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterMinusSign == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setSymbol(formatter->_nf, UNUM_MINUS_SIGN_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterPlusSign == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setSymbol(formatter->_nf, UNUM_PLUS_SIGN_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterCurrencySymbol == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setSymbol(formatter->_nf, UNUM_CURRENCY_SYMBOL, (const UChar*)ubuffer, cnt, &status);
    } else if (kCFNumberFormatterExponentSymbol == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setSymbol(formatter->_nf, UNUM_EXPONENTIAL_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterMinIntegerDigits == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFNumberGetTypeID());
        CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
        unum_setAttribute(formatter->_nf, UNUM_MIN_INTEGER_DIGITS, n);
    } else if (kCFNumberFormatterMaxIntegerDigits == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFNumberGetTypeID());
        CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
        unum_setAttribute(formatter->_nf, UNUM_MAX_INTEGER_DIGITS, n);
    } else if (kCFNumberFormatterMinFractionDigits == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFNumberGetTypeID());
        CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
        unum_setAttribute(formatter->_nf, UNUM_MIN_FRACTION_DIGITS, n);
    } else if (kCFNumberFormatterMaxFractionDigits == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFNumberGetTypeID());
        CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
        unum_setAttribute(formatter->_nf, UNUM_MAX_FRACTION_DIGITS, n);
    } else if (kCFNumberFormatterGroupingSize == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFNumberGetTypeID());
        CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
        unum_setAttribute(formatter->_nf, UNUM_GROUPING_SIZE, n);
    } else if (kCFNumberFormatterSecondaryGroupingSize == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFNumberGetTypeID());
        CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
        unum_setAttribute(formatter->_nf, UNUM_SECONDARY_GROUPING_SIZE, n);
    } else if (kCFNumberFormatterRoundingMode == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFNumberGetTypeID());
        CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
        unum_setAttribute(formatter->_nf, UNUM_ROUNDING_MODE, n);
    } else if (kCFNumberFormatterRoundingIncrement == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFNumberGetTypeID());
        CFNumberGetValue((CFNumberRef)value, kCFNumberDoubleType, &d);
        unum_setDoubleAttribute(formatter->_nf, UNUM_ROUNDING_INCREMENT, d);
    } else if (kCFNumberFormatterFormatWidth == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFNumberGetTypeID());
        CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
        unum_setAttribute(formatter->_nf, UNUM_FORMAT_WIDTH, n);
    } else if (kCFNumberFormatterPaddingPosition == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFNumberGetTypeID());
        CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
        unum_setAttribute(formatter->_nf, UNUM_PADDING_POSITION, n);
    } else if (kCFNumberFormatterPaddingCharacter == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setTextAttribute(formatter->_nf, UNUM_PADDING_CHARACTER, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterDefaultFormat == key) {
        // read-only attribute
    } else if (kCFNumberFormatterMultiplier == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFNumberGetTypeID());
        CFNumberRef old = formatter->_multiplier;
        formatter->_multiplier = value ? (CFNumberRef)CFRetain(value) : NULL;
        if (old) {
            CFRelease(old);
        }
    } else if (kCFNumberFormatterPositivePrefix == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setTextAttribute(formatter->_nf, UNUM_POSITIVE_PREFIX, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterPositiveSuffix == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setTextAttribute(formatter->_nf, UNUM_POSITIVE_SUFFIX, (const UChar*)ubuffer, cnt, &status);
    } else if (kCFNumberFormatterNegativePrefix == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setTextAttribute(formatter->_nf, UNUM_NEGATIVE_PREFIX, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterNegativeSuffix == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setTextAttribute(formatter->_nf, UNUM_NEGATIVE_SUFFIX, (const UChar*)ubuffer, cnt, &status);
    } else if (kCFNumberFormatterPerMillSymbol == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setSymbol(formatter->_nf, UNUM_PERMILL_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterInternationalCurrencySymbol == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setSymbol(formatter->_nf, UNUM_INTL_CURRENCY_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterCurrencyGroupingSeparator == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFStringGetTypeID());
        cnt = CFStringGetLength((CFStringRef)value);
        if (ICU_BUFFER_SIZE < cnt) {
            cnt = ICU_BUFFER_SIZE;
        }
        CFStringGetCharacters((CFStringRef)value, CFRangeMake(0, cnt), (UniChar*)ubuffer);
        unum_setSymbol(formatter->_nf, UNUM_MONETARY_GROUPING_SEPARATOR_SYMBOL, ubuffer, cnt, &status);
    } else if (kCFNumberFormatterIsLenient == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFBooleanGetTypeID());
        unum_setAttribute(formatter->_nf, UNUM_LENIENT_PARSE, (kCFBooleanTrue == value));
    } else if (kCFNumberFormatterUseSignificantDigits == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFBooleanGetTypeID());
        unum_setAttribute(formatter->_nf, UNUM_SIGNIFICANT_DIGITS_USED, (kCFBooleanTrue == value));
    } else if (kCFNumberFormatterMinSignificantDigits == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFNumberGetTypeID());
        CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
        unum_setAttribute(formatter->_nf, UNUM_MIN_SIGNIFICANT_DIGITS, n);
    } else if (kCFNumberFormatterMaxSignificantDigits == key) {
        CF_VALIDATE_OBJECT_ARG(CF, value, CFNumberGetTypeID());
        CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
        unum_setAttribute(formatter->_nf, UNUM_MAX_SIGNIFICANT_DIGITS, n);
    } else {
		CF_VALIDATE_ARG(false, "unknown key %p (%@)", key, key);
    }
}

CFTypeRef CFNumberFormatterCopyProperty(CFNumberFormatterRef formatter, CFStringRef key) {
	CF_VALIDATE_PTR_ARG(key);

    int32_t n;
    double d;
    UErrorCode status = U_ZERO_ERROR;
    UChar ubuffer[ICU_BUFFER_SIZE];
    CFIndex cnt;
    CF_VALIDATE_OBJECT_ARG(CF, formatter, CFNumberFormatterGetTypeID());
    CF_VALIDATE_OBJECT_ARG(CF, key, CFStringGetTypeID());
    if (kCFNumberFormatterCurrencyCode == key) {
        cnt = unum_getTextAttribute(formatter->_nf, UNUM_CURRENCY_CODE, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt == 0) {
			const char* localeName;
			CFICU_LOCALE_GET_NAME_CSTR(formatter->_locale, localeName);
			if (!localeName) {
				return NULL;
			}
            UErrorCode status = U_ZERO_ERROR;
            UNumberFormat* nf = unum_open(UNUM_CURRENCY, NULL, 0, localeName, NULL, &status);
            if (nf) {
                cnt = unum_getTextAttribute(nf, UNUM_CURRENCY_CODE, ubuffer, ICU_BUFFER_SIZE, &status);
                unum_close(nf);
            }
        }
        if (U_SUCCESS(status) && 0 < cnt && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterDecimalSeparator == key) {
        cnt = unum_getSymbol(formatter->_nf, UNUM_DECIMAL_SEPARATOR_SYMBOL, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterCurrencyDecimalSeparator == key) {
        cnt = unum_getSymbol(formatter->_nf, UNUM_MONETARY_SEPARATOR_SYMBOL, (UChar*)ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterAlwaysShowDecimalSeparator == key) {
        n = unum_getAttribute(formatter->_nf, UNUM_DECIMAL_ALWAYS_SHOWN);
        if (1) {
            return CFRetain(n ? kCFBooleanTrue : kCFBooleanFalse);
        }
    } else if (kCFNumberFormatterGroupingSeparator == key) {
        cnt = unum_getSymbol(formatter->_nf, UNUM_GROUPING_SEPARATOR_SYMBOL, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterUseGroupingSeparator == key) {
        n = unum_getAttribute(formatter->_nf, UNUM_GROUPING_USED);
        if (1) {
            return CFRetain(n ? kCFBooleanTrue : kCFBooleanFalse);
        }
    } else if (kCFNumberFormatterPercentSymbol == key) {
        cnt = unum_getSymbol(formatter->_nf, UNUM_PERCENT_SYMBOL, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterZeroSymbol == key) {
        return formatter->_zeroSym ? CFRetain(formatter->_zeroSym) : NULL;
    } else if (kCFNumberFormatterNaNSymbol == key) {
        cnt = unum_getSymbol(formatter->_nf, UNUM_NAN_SYMBOL, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterInfinitySymbol == key) {
        cnt = unum_getSymbol(formatter->_nf, UNUM_INFINITY_SYMBOL, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterMinusSign == key) {
        cnt = unum_getSymbol(formatter->_nf, UNUM_MINUS_SIGN_SYMBOL, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterPlusSign == key) {
        cnt = unum_getSymbol(formatter->_nf, UNUM_PLUS_SIGN_SYMBOL, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterCurrencySymbol == key) {
        cnt = unum_getSymbol(formatter->_nf, UNUM_CURRENCY_SYMBOL, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterExponentSymbol == key) {
        cnt = unum_getSymbol(formatter->_nf, UNUM_EXPONENTIAL_SYMBOL, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterMinIntegerDigits == key) {
        n = unum_getAttribute(formatter->_nf, UNUM_MIN_INTEGER_DIGITS);
        if (1) {
            return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
        }
    } else if (kCFNumberFormatterMaxIntegerDigits == key) {
        n = unum_getAttribute(formatter->_nf, UNUM_MAX_INTEGER_DIGITS);
        if (1) {
            return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
        }
    } else if (kCFNumberFormatterMinFractionDigits == key) {
        n = unum_getAttribute(formatter->_nf, UNUM_MIN_FRACTION_DIGITS);
        if (1) {
            return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
        }
    } else if (kCFNumberFormatterMaxFractionDigits == key) {
        n = unum_getAttribute(formatter->_nf, UNUM_MAX_FRACTION_DIGITS);
        if (1) {
            return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
        }
    } else if (kCFNumberFormatterGroupingSize == key) {
        n = unum_getAttribute(formatter->_nf, UNUM_GROUPING_SIZE);
        if (1) {
            return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
        }
    } else if (kCFNumberFormatterSecondaryGroupingSize == key) {
        n = unum_getAttribute(formatter->_nf, UNUM_SECONDARY_GROUPING_SIZE);
        if (1) {
            return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
        }
    } else if (kCFNumberFormatterRoundingMode == key) {
        n = unum_getAttribute(formatter->_nf, UNUM_ROUNDING_MODE);
        if (1) {
            return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
        }
    } else if (kCFNumberFormatterRoundingIncrement == key) {
        d = unum_getDoubleAttribute(formatter->_nf, UNUM_ROUNDING_INCREMENT);
        if (1) {
            return CFNumberCreate(CFGetAllocator(formatter), kCFNumberDoubleType, &d);
        }
    } else if (kCFNumberFormatterFormatWidth == key) {
        n = unum_getAttribute(formatter->_nf, UNUM_FORMAT_WIDTH);
        if (1) {
            return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
        }
    } else if (kCFNumberFormatterPaddingPosition == key) {
        n = unum_getAttribute(formatter->_nf, UNUM_PADDING_POSITION);
        if (1) {
            return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
        }
    } else if (kCFNumberFormatterPaddingCharacter == key) {
        cnt = unum_getTextAttribute(formatter->_nf, UNUM_PADDING_CHARACTER, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterDefaultFormat == key) {
        return formatter->_defformat ? CFRetain(formatter->_defformat) : NULL;
    } else if (kCFNumberFormatterMultiplier == key) {
        return formatter->_multiplier ? CFRetain(formatter->_multiplier) : NULL;
    } else if (kCFNumberFormatterPositivePrefix == key) {
        cnt = unum_getTextAttribute(formatter->_nf, UNUM_POSITIVE_PREFIX, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterPositiveSuffix == key) {
        cnt = unum_getTextAttribute(formatter->_nf, UNUM_POSITIVE_SUFFIX, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterNegativePrefix == key) {
        cnt = unum_getTextAttribute(formatter->_nf, UNUM_NEGATIVE_PREFIX, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterNegativeSuffix == key) {
        cnt = unum_getTextAttribute(formatter->_nf, UNUM_NEGATIVE_SUFFIX, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterPerMillSymbol == key) {
        cnt = unum_getSymbol(formatter->_nf, UNUM_PERMILL_SYMBOL, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterInternationalCurrencySymbol == key) {
        cnt = unum_getSymbol(formatter->_nf, UNUM_INTL_CURRENCY_SYMBOL, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterCurrencyGroupingSeparator == key) {
        cnt = unum_getSymbol(formatter->_nf, UNUM_MONETARY_GROUPING_SEPARATOR_SYMBOL, ubuffer, ICU_BUFFER_SIZE, &status);
        if (U_SUCCESS(status) && cnt <= ICU_BUFFER_SIZE) {
            return CFStringCreateWithCharacters(CFGetAllocator(formatter), (const UniChar*)ubuffer, cnt);
        }
    } else if (kCFNumberFormatterIsLenient == key) {
        n = unum_getAttribute(formatter->_nf, UNUM_LENIENT_PARSE);
        if (1) {
            return CFRetain(n ? kCFBooleanTrue : kCFBooleanFalse);
        }
    } else if (kCFNumberFormatterUseSignificantDigits == key) {
        n = unum_getAttribute(formatter->_nf, UNUM_SIGNIFICANT_DIGITS_USED);
        if (1) {
            return CFRetain(n ? kCFBooleanTrue : kCFBooleanFalse);
        }
    } else if (kCFNumberFormatterMinSignificantDigits == key) {
        n = unum_getAttribute(formatter->_nf, UNUM_MIN_SIGNIFICANT_DIGITS);
        if (1) {
            return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
        }
    } else if (kCFNumberFormatterMaxSignificantDigits == key) {
        n = unum_getAttribute(formatter->_nf, UNUM_MAX_SIGNIFICANT_DIGITS);
        if (1) {
            return CFNumberCreate(CFGetAllocator(formatter), kCFNumberSInt32Type, &n);
        }
    } else {
        CF_VALIDATE_ARG(false, "unknown key %p (%@)", key, key);
    }
    return NULL;
}

Boolean CFNumberFormatterGetDecimalInfoForCurrencyCode(CFStringRef currencyCode, int32_t* defaultFractionDigits, double* roundingIncrement) {
	CF_VALIDATE_ARG(CFStringGetLength(currencyCode) == 3,
		"currencyCode is not 3 characters");

    UChar ubuffer[4];
    CFStringGetCharacters(currencyCode, CFRangeMake(0, 3), (UniChar*)ubuffer);
    ubuffer[3] = 0;

	UErrorCode icuStatus = U_ZERO_ERROR;
    if (defaultFractionDigits) {
        *defaultFractionDigits = ucurr_getDefaultFractionDigits(ubuffer, &icuStatus);
    }
    if (U_FAILURE(icuStatus)) {
        return false;
    }

    if (roundingIncrement) {
        *roundingIncrement = ucurr_getRoundingIncrement(ubuffer, &icuStatus);
    }
    if (U_FAILURE(icuStatus)) {
        return false;
    }

    return	(!defaultFractionDigits || 0 <= *defaultFractionDigits) &&
			(!roundingIncrement || 0.0 <= *roundingIncrement);
}


