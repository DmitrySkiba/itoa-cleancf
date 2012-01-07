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

#if !defined(__COREFOUNDATION_CFRUNLOOPPORT__)
#define __COREFOUNDATION_CFRUNLOOPPORT__ 1

#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFDate.h>

CF_EXTERN_C_BEGIN

typedef struct _CFRunLoopPort* CFRunLoopPortRef;

CF_EXPORT
CFTypeID CFRunLoopPortTypeID();

CF_EXPORT
CFRunLoopPortRef CFRunLoopPortCreate(CFAllocatorRef allocator);

CF_EXPORT
Boolean CFRunLoopPortSignal(CFRunLoopPortRef port);

CF_EXPORT
Boolean CFRunLoopPortWait(CFArrayRef ports, CFTimeInterval timeout, CFIndex* singnalledIndex);

/* Implementation details.
 * CFRunLoopPortSetImpl must be called before CFRunLoop can be used.
 */

typedef struct _CFRunLoopPortImpl {
    CFIndex dataSize;
    Boolean (*create)(void* data);
    void (*destroy)(void* data);
    Boolean (*signal)(void* data);
    Boolean (*wait)(CFArrayRef ports, CFTimeInterval timeout, CFIndex* signalledIndex);
} CFRunLoopPortImpl;

CF_EXPORT
void CFRunLoopPortSetImpl(const CFRunLoopPortImpl* impl);

CF_EXPORT
CFIndex CFRunLoopPortGetImplDataSize();

CF_EXPORT
void* CFRunLoopPortGetImplData(CFRunLoopPortRef port);

CF_EXTERN_C_END

#endif /* !__COREFOUNDATION_CFRUNLOOPPORT__ */
