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

#if !defined(__COREFOUNDATION_CFOBJCBRIDGE__)
#define __COREFOUNDATION_CFOBJCBRIDGE__ 1

#ifdef CF_ENABLE_OBJC_BRIDGE

#include <objc/message.h>

#define CF_IS_OBJC(cf) \
    !_CFRuntimeIsCFInstance(cf)

#define CF_OBJC_VOID_CALL(cf, selectorName, ...) \
    { \
        static SEL selector = NULL; \
        if (!selector) { \
            selector = sel_getUid(selectorName); \
        } \
        objc_msgSend(CF_CONST_CAST(id, cf), selector, ##__VA_ARGS__); \
    }
                
#define CF_OBJC_CALL(ResultType, result, cf, selectorName, ...) \
    { \
        static SEL selector = NULL; \
        if (!selector) { \
            selector = sel_getUid(selectorName); \
        } \
		typedef char ResultType_must_not_be_larger_than_id[ \
			2 * (sizeof(ResultType) <= sizeof(id)) - 1]; \
        result = CF_CAST(ResultType, \
			objc_msgSend(CF_CONST_CAST(id, cf), selector, ##__VA_ARGS__) \
		); \
    }

#define CF_OBJC_VOID_FUNCDISPATCH(cf, selectorName, ...) \
    if (CF_IS_OBJC(cf)) { \
		CF_OBJC_VOID_CALL(cf, selectorName, ##__VA_ARGS__); \
		return; \
	}

#define CF_OBJC_FUNCDISPATCH(ResultType, cf, selectorName, ...) \
    if (CF_IS_OBJC(cf)) { \
		ResultType result; \
		CF_OBJC_CALL(ResultType, result, cf, selectorName, ##__VA_ARGS__); \
		return result; \
	}

#else // !CF_ENABLE_OBJC_BRIDGE

#define CF_IS_OBJC(cf) \
    (0)

#define CF_OBJC_VOID_CALL(cf, selectorName, ...) \
    do {} while(0)
                
#define CF_OBJC_CALL(ResultType, result, cf, selectorName, ...) \
    do {} while(0)

#define CF_OBJC_VOID_FUNCDISPATCH(cf, selectorName, ...) \
    do {} while(0)

#define CF_OBJC_FUNCDISPATCH(ResultType, cf, selectorName, ...) \
    do {} while(0)

#endif // CF_ENABLE_OBJC_BRIDGE

#endif /* ! __COREFOUNDATION_CFOBJCBRIDGE__ */
