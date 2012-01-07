#
# Copyright (C) 2011 Dmitry Skiba
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

MODULE_PATH := $(call my-dir)

include $(CLEAR_VARS)

MODULE_NAME := cf

$(call itoa-sysroot-copy-files,\
    $(MODULE_PATH)/include/CoreFoundation,usr/include/CoreFoundation)

$(eval $(call itoa-copy-file,\
    $(MODULE_PATH)/APPLE_LICENSE,$(MODULE_NAME).license.txt))
$(eval $(call itoa-copy-file,\
    $(MODULE_PATH)/android/icu4c.license.txt,icu4c.license.txt))

MODULE_CFLAGS := \
    -I$(MODULE_PATH)/include \
    -I$(MODULE_PATH)/src/CoreFoundation \
    -I$(MODULE_PATH)/android/src/icu4c \
    \
    -DCF_ENABLE_OBJC_BRIDGE \
    \
    -std=gnu99 \
    -fgnu89-inline \

MODULE_SRC_FILES += \
    src/CoreFoundation/CFAllocator.c \
    src/CoreFoundation/CFArray.c \
    src/CoreFoundation/CFBag.c \
    src/CoreFoundation/CFBase.c \
    src/CoreFoundation/CFBoolean.c \
    src/CoreFoundation/CFCharacterSet.c \
    src/CoreFoundation/CFData.c \
    src/CoreFoundation/CFDate.c \
    src/CoreFoundation/CFDictionary.c \
    src/CoreFoundation/CFLog.c \
    src/CoreFoundation/CFNull.c \
    src/CoreFoundation/CFNumber.c \
    src/CoreFoundation/CFNumberFormatter.c \
    src/CoreFoundation/CFPlatformLinux.c \
    src/CoreFoundation/CFRunLoop.c \
    src/CoreFoundation/CFRunLoop_Observer.c \
    src/CoreFoundation/CFRunLoop_Source.c \
    src/CoreFoundation/CFRunLoop_Timer.c \
    src/CoreFoundation/CFRunLoopPort.c \
    src/CoreFoundation/CFRuntime.c \
    src/CoreFoundation/CFSet.c \
    src/CoreFoundation/CFSortFunctions.c \
    src/CoreFoundation/CFStorage.c \
    src/CoreFoundation/CFString/CFString.c \
    src/CoreFoundation/CFString/CFString_Const.c \
    src/CoreFoundation/CFString/CFString_Encode.c \
    src/CoreFoundation/CFString/CFString_Encoding.c \
    src/CoreFoundation/CFString/CFString_Fold.c \
    src/CoreFoundation/CFString/CFString_Format.c \
    src/CoreFoundation/CFString/CFString_Hash.c \
    src/CoreFoundation/CFString/CFString_Mutable.c \
    src/CoreFoundation/CFString/CFString_Scanner.c \
    src/CoreFoundation/CFString/CFStringEncoding.c \
    src/CoreFoundation/CFString/CFStringEncodingConverters.c \
    src/CoreFoundation/CFString/CFStringEncodingConverterUTF8.c \
    src/CoreFoundation/CFThreadData.c \
    src/CoreFoundation/CFType.c \
    src/CoreFoundation/CFUniChar.c \
    src/CoreFoundation/CFUniChar_Decompose.c \
    src/CoreFoundation/CFUniChar_Precompose.c \
    src/CoreFoundation/CFUtilities.c \
    src/CoreFoundation/CFError.c \
    src/CoreFoundation/CFURL.c \
    src/CoreFoundation/CFTimeZone.c \
    src/CoreFoundation/CFDateFormatter.c \
    src/CoreFoundation/CFLocale.c \
    src/CoreFoundation/CFLocale_Identifier.c \
    src/CoreFoundation/CFCalendar.c \

MODULE_LDLIBS := -llog -ldl
MODULE_LDLIBS += -L$(MODULE_PATH)/android/src/icu4c/libs -licui18n -licuuc

MODULE_SHARED_LIBRARIES := macemu objc

include $(BUILD_SHARED_LIBRARY)

