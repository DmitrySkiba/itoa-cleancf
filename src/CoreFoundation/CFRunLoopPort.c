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

#include "CFInternal.h"
#include <CoreFoundation/CFRunLoopPort.h>

//TODO reformat CFRunLoopPort.c to conform codestyle

///////////////////////////////////////////////// impl

static Boolean DefaultCreate(void* data) {
    return true;
}
static void DefaultDestroy(void* data) {
}
static Boolean DefaultSignal(void* data) {
    return false;
}
static Boolean DefaultWait(CFArrayRef ports,CFTimeInterval timeout,CFIndex* signalledIndex) {
     return false;
}

static CFSpinLock_t g_implLock=CFSpinLockInit;
static Boolean g_implUsed=false;
static Boolean g_implSet=false;
static CFRunLoopPortImpl g_impl={
    0,
    DefaultCreate,
    DefaultDestroy,
    DefaultSignal,
    DefaultWait
};

void CFRunLoopPortSetImpl(const CFRunLoopPortImpl* impl) {
    CFSpinLock(&g_implLock);
    if (g_implUsed) {
        CFSpinUnlock(&g_implLock);
        CF_GENERIC_ERROR("Current CFRunLoopPortImpl object is in use and can't be changed.");
    }
    memcpy(&g_impl,impl,sizeof(CFRunLoopPortImpl));
    g_implSet=true;
    CFSpinUnlock(&g_implLock);
}

///////////////////////////////////////////////// port

typedef struct _CFRunLoopPort {
    CFRuntimeBase runtime;
    UInt8 data[1];
} CFRunLoopPort;

static CFTypeID g_typeID=_kCFRuntimeNotATypeID;

CFTypeID CFRunLoopPortTypeID() {
    return g_typeID;
}

CFRunLoopPortRef CFRunLoopPortCreate(CFAllocatorRef allocator) {
    {
        CFSpinLock(&g_implLock);
        if (!g_implSet) {
            CFLog(kCFLogLevelWarning,CFSTR("CFRunLoopPortImpl was not set. CFRunLoop may not behave as expected."));
        }
        g_implUsed=g_implSet;
        CFSpinUnlock(&g_implLock);
    }
    CFRunLoopPort* port=(CFRunLoopPort*)_CFRuntimeCreateInstance(
        allocator,
        CFRunLoopPortTypeID(),
        (CFIndex)g_impl.dataSize - sizeof(port->data),
        NULL);
    if (!port) {
        return port;
    }
    if (!g_impl.create(port->data)) {
        CFRelease(port);
        return NULL;
    }
    return port;        
}

Boolean CFRunLoopPortSignal(CFRunLoopPortRef port) {
    return g_impl.signal(port->data);
}

Boolean CFRunLoopPortWait(CFArrayRef ports,CFTimeInterval timeout,CFIndex* signalledIndex) {
    return g_impl.wait(ports,timeout,signalledIndex);
}

CFIndex CFRunLoopPortGetImplDataSize() {
    return g_impl.dataSize;
}

void* CFRunLoopPortGetImplData(CFRunLoopPortRef port) {
    return port->data;
}

///////////////////////////////////////////////// class

static void Deallocate(CFTypeRef cf) {
    g_impl.destroy(CF_CONST_CAST(CFRunLoopPort*,cf)->data);
}

static Boolean Equal(CFTypeRef cf1,CFTypeRef cf2) {
    if (cf1==cf2) {
        return true;
    }
    if (!cf1 || !cf2) {
        return false;
    }
    return 0==memcmp(
        CF_CONST_CAST(CFRunLoopPort*,cf1)->data,
        CF_CONST_CAST(CFRunLoopPort*,cf2)->data,
        g_impl.dataSize);
}

static const CFRuntimeClass g_class={
    0,
    "CFRunLoopPort",
    NULL,        // init
    NULL,        // copy
    Deallocate,
    Equal,
    NULL,        // hash
    NULL,        // formatting description
    NULL,        // description
};

CF_INTERNAL void __CFRunLoopPortInitialize() {
    g_typeID=_CFRuntimeRegisterClass(&g_class);
}

/////////////////////////////////////////////////
