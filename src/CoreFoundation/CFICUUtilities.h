/*
 * Copyright (C) 2011 Dmitry Skiba
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if !defined(__COREFOUNDATION_CFICUUTILITIESINTERNAL__)
#define __COREFOUNDATION_CFICUUTILITIESINTERNAL__  1

#include <unicode/ucal.h>

#define CFICU_GET_STRING_CSTR(string, cstr) \
	char cstr ## Buffer[512]; \
	cstr = CFStringGetCStringPtr(string, kCFStringEncodingASCII); \
	if (!cstr) { \
		if (CFStringGetCString( \
				string, \
				cstr ## Buffer, sizeof(cstr ## Buffer), \
				kCFStringEncodingASCII)) \
		{ \
			cstr = cstr ## Buffer; \
		} \
	}

#define CFICU_LOCALE_GET_NAME_CSTR(locale, cstr) \
	CFStringRef cstr ## Name = CFLocaleGetIdentifier(locale); \
	CFICU_GET_STRING_CSTR(cstr ## Name, cstr)

#define ICU_BUFFER_SIZE 768

CF_EXTERN_C_BEGIN

//TODO _CFCalendarCreateUCalendar must be non-inline function.
CF_INLINE
UCalendar* _CFCalendarCreateUCalendar(CFStringRef calendarID, CFStringRef localeID, CFTimeZoneRef tz) {
    if (calendarID) {
        CFDictionaryRef components = CFLocaleCreateComponentsFromLocaleIdentifier(kCFAllocatorSystemDefault, localeID);
        CFMutableDictionaryRef mcomponents = CFDictionaryCreateMutableCopy(kCFAllocatorSystemDefault, 0, components);
        CFDictionarySetValue(mcomponents, kCFLocaleCalendarIdentifier, calendarID);
        localeID = CFLocaleCreateLocaleIdentifierFromComponents(kCFAllocatorSystemDefault, mcomponents);
        CFRelease(mcomponents);
        CFRelease(components);
    }

	const char* cstr;
	CFICU_GET_STRING_CSTR(localeID, cstr);
    if (!cstr) {
        if (calendarID) {
            CFRelease(localeID);
        }
        return NULL;
    }

    UChar ubuffer[ICU_BUFFER_SIZE];
    CFStringRef tznam = CFTimeZoneGetName(tz);
    CFIndex cnt = CFStringGetLength(tznam);
    if (cnt > ICU_BUFFER_SIZE) {
        cnt = ICU_BUFFER_SIZE;
    }
    CFStringGetCharacters(tznam, CFRangeMake(0, cnt), (UniChar*)ubuffer);

    UErrorCode status = U_ZERO_ERROR;
    UCalendar* cal = ucal_open(ubuffer, (int32_t)cnt, cstr, UCAL_TRADITIONAL, &status);
    if (calendarID) {
        CFRelease(localeID);
    }
    return cal;
}

CF_EXTERN_C_END

#endif /* !__COREFOUNDATION_CFICUUTILITIESINTERNAL__ */

