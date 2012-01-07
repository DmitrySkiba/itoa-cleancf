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

#if !defined(__COREFOUNDATION_CFSTRINGINTERNAL__)
#define __COREFOUNDATION_CFSTRINGINTERNAL__ 1

#include "CFInternal.h"

//TODO cleanup CFStringInternal

enum {
    _kCFStringTypeID = 7
};

CF_EXTERN_C_BEGIN

CF_EXPORT void __CFStringInitialize();
CF_EXPORT const void* __CFStringCollectionCopy(CFAllocatorRef allocator, const void* ptr);

CF_EXPORT CFStringEncoding CFStringFileSystemEncoding(void);

CF_EXPORT CFStringRef __CFStringCreateImmutableFunnel(CFAllocatorRef alloc, const void *bytes, CFIndex numBytes, CFStringEncoding encoding, Boolean possiblyExternalFormat, Boolean tryToReduceUnicode, Boolean hasLengthByte, Boolean hasNullByte, Boolean noCopy, CFAllocatorRef contentsDeallocator, UInt32 converterFlags);


// TODO, add functions (to CFString.h) CFStringTryGet{Int,Double}Value() and use them instead
CF_EXPORT Boolean _CFStringScanDouble(CFStringInlineBuffer* buf, CFTypeRef locale, SInt32* indexPtr, double* resultPtr);
CF_EXPORT Boolean _CFStringScanInteger(CFStringInlineBuffer* buf, CFTypeRef locale, SInt32* indexPtr, Boolean doLonglong, void* result);

/////////////////////////////////////////////////////////////////////////////////////////
// NS INTERFACE

/* These two allow specifying an alternate description function (instead of CFCopyDescription); 
 * Used by NSString.
*/
CF_EXPORT void _CFStringAppendFormatAndArgumentsAux(
    CFMutableStringRef outputString,
    CFStringRef (*copyDescFunc)(void *, const void *loc),
    CFDictionaryRef formatOptions, CFStringRef formatString, va_list args);
CF_EXPORT CFStringRef  _CFStringCreateWithFormatAndArgumentsAux(
    CFAllocatorRef alloc,
    CFStringRef (*copyDescFunc)(void *, const void *loc),
    CFDictionaryRef formatOptions, CFStringRef format, va_list arguments);


/* For NSString (and NSAttributedString) usage, mutate with isMutable check
 * Zzzz what is comment is for?
 */

/* For NSString usage, guarantees that the contents can be extracted as
 *  8-bit bytes in the  __CFStringGetEightBitStringEncoding().
*/
CF_EXPORT Boolean __CFStringIsEightBit(CFStringRef str);

/* For NSCFString usage, these do range check (where applicable) but don't check for ObjC dispatch
*/
CF_EXPORT CFIndex _CFStringGetLength2(CFStringRef str);

CF_EXPORT CFHashCode CFStringHashISOLatin1CString(const uint8_t *bytes, CFIndex len);
CF_EXPORT CFHashCode CFStringHashCString(const uint8_t *bytes, CFIndex len);
CF_EXPORT CFHashCode CFStringHashCharacters(const UniChar *characters, CFIndex len);
CF_EXPORT CFHashCode CFStringHashNSString(CFStringRef str);


// Make private (Foundation should use public CFStringGetRangeOfComposedCharactersAtIndex)
enum {
	/* Unicode Grapheme Cluster */
    kCFStringGraphemeCluster = 1,

	/* Compose all non-base (including spacing marks) */
    kCFStringComposedCharacterCluster = 2,

	/* Cluster suitable for cursor movements */
    kCFStringCursorMovementCluster = 3,

	/* Cluster suitable for backward deletion */
    kCFStringBackwardDeletionCluster = 4
};
typedef CFIndex CFStringCharacterClusterType;
CF_EXPORT CFRange CFStringGetRangeOfCharacterClusterAtIndex(CFStringRef string, CFIndex charIndex, CFStringCharacterClusterType type);


//TODO REVIEW
// Compatibility kCFCompare flags. Use the new public kCFCompareDiacriticInsensitive
enum {
    kCFCompareDiacriticsInsensitive = 128, /* kCFCompareDiacriticInsensitive */
    kCFCompareDiacriticsInsensitiveCompatibilityMask = ((1 << 28)|kCFCompareDiacriticsInsensitive),
};

/*
CF_EXPORT char* _CFStrGetLanguageIdentifierForLocale(CFLocaleRef locale) {
CF_EXPORT bool _CFCanUseLocale(CFLocaleRef locale) {
CF_EXPORT CFStringEncoding __CFStringGetEightBitStringEncoding(void) {
CF_EXPORT void __CFStringInitialize(void) {
CF_EXPORT CFStringRef __CFStringCreateImmutableFunnel(
CF_EXPORT CFStringRef _CFStringCreateWithBytesNoCopy(CFAllocatorRef alloc, const uint8_t* bytes, CFIndex numBytes, CFStringEncoding encoding, Boolean externalFormat, CFAllocatorRef contentsDeallocator) {
CF_EXPORT CFStringRef _CFStringCreateWithFormatAndArgumentsAux(CFAllocatorRef alloc, CFStringRef (* copyDescFunc)(void*, const void*), CFDictionaryRef formatOptions, CFStringRef format, va_list arguments) {
CF_EXPORT void _CFStrSetDesiredCapacity(CFMutableStringRef str, CFIndex len) {
CF_EXPORT CFIndex _CFStringGetLength2(CFStringRef str) {
CF_EXPORT CFStringEncoding CFStringFileSystemEncoding(void) {
*/

/////////////////////////////////////////////////////////////////////////////////////////

CF_EXTERN_C_END

#endif /* !__COREFOUNDATION_CFSTRINGINTERNAL__ */
