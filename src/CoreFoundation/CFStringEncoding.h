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

#if !defined(__COREFOUNDATION_CFSTRINGENCODING__)
#define __COREFOUNDATION_CFSTRINGENCODING__ 1

#include <CoreFoundation/CFString.h>

CF_EXTERN_C_BEGIN

/* Values for flags argument for the conversion functions below.
 * These can be combined, but the three NonSpacing behavior flags are exclusive.
 */
enum {
    // Uses fallback functions to substitutes non mappable chars.
    kCFStringEncodingAllowLossyConversion = 1,
    // Converted with original direction left-to-right.
    kCFStringEncodingBasicDirectionLeftToRight = (1 << 1),
    // Converted with original direction right-to-left.
    kCFStringEncodingBasicDirectionRightToLeft = (1 << 2),
    // Uses fallback function to combining chars.
    kCFStringEncodingSubstituteCombinings = (1 << 3),
    // Checks mappable precomposed equivalents for decomposed sequences.
    // This is the default behavior.
    kCFStringEncodingComposeCombinings = (1 << 4),
    // Ignores combining chars.
    kCFStringEncodingIgnoreCombinings = (1 << 5),
    // Always use canonical form.
    kCFStringEncodingUseCanonical = (1 << 6),
    // Always use canonical form but leaves 0x2000 ranges.
    kCFStringEncodingUseHFSPlusCanonical = (1 << 7),
    // Prepend BOM sequence (i.e. ISO2022KR).
    kCFStringEncodingPrependBOM = (1 << 8),
    // Disable the usage of 0xF8xx area for Apple proprietary chars in converting to UniChar, 
    //  resulting loosely mapping.
    kCFStringEncodingDisableCorporateArea = (1 << 9),
    // This flag forces strict ASCII compatible converion, 
    //  i.e. MacJapanese 0x5C maps to Unicode 0x5C.
    kCFStringEncodingASCIICompatibleConversion = (1 << 10),
    // 10.1 (Puma) compatible lenient UTF-8 conversion.
    kCFStringEncodingLenientUTF8Conversion = (1 << 11)
};

/* Return values for CFStringEncodingUnicodeToBytes and
 *  CFStringEncodingBytesToUnicode functions.
 */
enum {
    kCFStringEncodingConversionSuccess = 0,
    kCFStringEncodingInvalidInputStream = 1,
    kCFStringEncodingInsufficientOutputBufferLength = 2,
    kCFStringEncodingConverterUnavailable = 3
};

/* Macro to shift lossByte argument.
*/
#define CFStringEncodingLossyByteToMask(lossByte) \
    ((uint32_t)((lossByte) << 24) | kCFStringEncodingAllowLossyConversion)

#define CFStringEncodingMaskToLossyByte(flags) \
    ((uint8_t)((flags) >> 24))

/* Converts characters into the specified encoding.
 * Returns the constants defined above.
 * If maxByteLen is 0, bytes is ignored. You can pass lossyByte by passing 
 *  the value in flags argument, i.e. 
 *
 *  CFStringEncodingUnicodeToBytes(
 *      encoding,
 *      CFStringEncodingLossyByteToMask(lossByte),
 *      ....)
 */
CF_EXPORT uint32_t CFStringEncodingUnicodeToBytes(
    uint32_t encoding, uint32_t flags,
    const UniChar* characters, CFIndex numChars, CFIndex* usedCharLen,
    uint8_t* bytes, CFIndex maxByteLen, CFIndex* usedByteLen);

/* Converts bytes in the specified encoding into unicode.
 * Returns the constants defined above.
 * Both maxCharLen and usdCharLen are in UniChar length, not byte length.
 * If maxCharLen is 0, characters is ignored.
*/
CF_EXPORT uint32_t CFStringEncodingBytesToUnicode(
    uint32_t encoding, uint32_t flags,
    const uint8_t* bytes, CFIndex numBytes, CFIndex* usedByteLen,
    UniChar* characters, CFIndex maxCharLen, CFIndex* usedCharLen);

/* Returns required length of destination buffer for conversion.
 * These functions are faster than specifying 0 to maxByteLen (maxCharLen),
 *  but unnecessarily optimal length.
 */
CF_EXPORT CFIndex CFStringEncodingCharLengthForBytes(
    uint32_t encoding, uint32_t flags,
    const uint8_t* bytes, CFIndex numBytes);

CF_EXPORT CFIndex CFStringEncodingByteLengthForCharacters(
    uint32_t encoding, uint32_t flags,
    const UniChar* characters, CFIndex numChars);



CF_EXPORT bool CFStringEncodingIsValidEncoding(uint32_t encoding);

// Returns kCFStringEncodingInvalidId terminated encoding list.
CF_EXPORT const uint32_t* CFStringEncodingListOfAvailableEncodings(void);

CF_EXPORT const char* CFStringEncodingName(uint32_t encoding);

// Returns NULL-terminated list of IANA registered canonical names
CF_EXPORT const char** CFStringEncodingCanonicalCharsetNames(uint32_t encoding);

CF_EXPORT uint32_t CFStringEncodingGetScriptCodeForEncoding(uint32_t encoding);


CF_INLINE Boolean __CFStringEncodingIsSupersetOfASCII(CFStringEncoding encoding) {
    switch (encoding & 0x0000FF00) {

        case 0x100: // Unicode range
            if (encoding != kCFStringEncodingUTF8) return false;
            return true;

            
        case 0x600: // National standards range
            if (encoding != kCFStringEncodingASCII) return false;
            return true;

        case 0x800: // ISO 2022 range
            return false; // It's modal encoding


        case 0xB00:
            if (encoding == kCFStringEncodingNonLossyASCII) return false;
            return true;

        case 0xC00: // EBCDIC
            return false;

        default:
            return ((encoding & 0x0000FF00) > 0x0C00 ? false : true);
    }
}

CF_EXTERN_C_END

#endif /* ! __COREFOUNDATION_CFSTRINGENCODING__ */
