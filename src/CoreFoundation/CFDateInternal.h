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

#if !defined(__COREFOUNDATION_CFDATEINTERNAL__)
#define __COREFOUNDATION_CFDATEINTERNAL__  1

#include <CoreFoundation/CFDate.h>
#include "CFPlatform.h"

CF_EXTERN_C_BEGIN

CF_EXPORT
void _CFDateInitialize(void);

CF_INLINE SInt64 _CFReadTSR(void) {
    return CFPlatformReadTSR();
}

CF_EXPORT
int64_t _CFTimeIntervalToTSR(CFTimeInterval interval);

CF_EXPORT
CFTimeInterval _CFTSRToTimeInterval(int64_t tsr);

CF_EXTERN_C_END

#endif /* !__COREFOUNDATION_CFDATEINTERNAL__ */
