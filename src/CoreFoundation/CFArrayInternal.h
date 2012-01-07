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

#if !defined(__COREFOUNDATION_CFARRAYINTERNAL__)
#define __COREFOUNDATION_CFARRAYINTERNAL__  1

CF_EXTERN_C_BEGIN

CF_EXPORT
void _CFArrayInitialize(void);

/* This creates an array which is for CFTypes or NSObjects, with an
 *  ownership transfer -- the array does not take a retain, and the
 *  caller does not need to release the inserted objects.
 */
CF_EXPORT
CFArrayRef _CFArrayCreate_ex(CFAllocatorRef allocator, Boolean isMutable, const void** values, CFIndex numValues);

CF_EXTERN_C_END

#endif /* !__COREFOUNDATION_CFARRAYINTERNAL__ */
