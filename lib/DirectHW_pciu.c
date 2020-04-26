/*
 * DirectHW.c - userspace part for DirectHW
 *
 * Copyright © 2008-2010 coresystems GmbH <info@coresystems.de>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "MacOSMacros.h"
#include "DirectHW.h"
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    kReadIO,
    kWriteIO,
};

typedef struct {
    UInt32 offset;
    UInt32 width;
    UInt32 data; // this field is 1 or 2 or 4 bytes starting at the lowest address
} iomem_t;

static io_connect_t darwin0_connect = MACH_PORT_NULL;
static io_service_t iokit_uc;

static int darwin0_init(void)
{
    kern_return_t err;

    /* Note the actual security happens in the kernel module.
     * This check is just candy to be able to get nicer output
     */
    if (getuid() != 0) {
        /* Fun's reserved for root */
        errno = EPERM;
        return -1;
    }

    /* Get the DirectHW driver service */
    iokit_uc = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("DirectHWService"));

    if (!iokit_uc) {
        printf("...DirectHW.kext not loaded");
        errno = ENOSYS;
        return -1;
    }

    /* Create an instance */
    err = IOServiceOpen(iokit_uc, mach_task_self(), 0, &darwin0_connect);

    /* Should not go further if error with service open */
    if (err != KERN_SUCCESS) {
        printf("...Could not create DirectHW instance");
        errno = ENOSYS;
        return -1;
    }

    return 0;
}

static void darwin0_cleanup(void)
{
    if (darwin0_connect != MACH_PORT_NULL) {
        IOServiceClose(darwin0_connect);
        darwin0_connect = MACH_PORT_NULL;
    }
}

kern_return_t MyIOConnectCallStructMethod(
    io_connect_t    connect,
    unsigned int    index,
    void *          in,
    size_t          dataInLen,
    void *          out,
    size_t *        dataOutLen
)
{
    kern_return_t err;
#if MAC_OS_X_VERSION_MAX_ALLOWED <= MAC_OS_X_VERSION_10_4 || MAC_OS_X_VERSION_SDK <= MAC_OS_X_VERSION_10_4
    err = IOConnectMethodStructureIStructureO(connect, index, dataInLen, dataOutLen, in, out);
#elif defined(__LP64__)
    err = IOConnectCallStructMethod(connect, index, in, dataInLen, out, dataOutLen);
#else
    if (IOConnectCallStructMethod != NULL) {
        /* OSX 10.5 or newer API is available */
        err = IOConnectCallStructMethod(connect, index, in, dataInLen, out, dataOutLen);
    }
    else {
        /* Use old API (not available for x86_64) */
        err = IOConnectMethodStructureIStructureO(connect, index, dataInLen, dataOutLen, in, out);
    }
#endif
    return err;
}

static int darwin0_ioread(int pos, unsigned char * buf, int len)
{
    kern_return_t err;
    size_t dataInLen = sizeof(iomem_t);
    size_t dataOutLen = sizeof(iomem_t);
    iomem_t in;
    iomem_t out;
    UInt32 tmpdata;

    in.width = len;
    in.offset = pos;

    if (len > 4)
        return 1;

    err = MyIOConnectCallStructMethod(darwin0_connect, kReadIO, &in, dataInLen, &out, &dataOutLen);
    if (err != KERN_SUCCESS)
        return 1;

    tmpdata = out.data;

    switch (len) {
        case 1: memcpy(buf, &tmpdata, 1); break;
        case 2: memcpy(buf, &tmpdata, 2); break;
        case 4: memcpy(buf, &tmpdata, 4); break;
        default: return 1;
    }

    return 0;
}

static int darwin0_iowrite(int pos, unsigned char * buf, int len)
{
    kern_return_t err;
    size_t dataInLen = sizeof(iomem_t);
    size_t dataOutLen = sizeof(iomem_t);
    iomem_t in;
    iomem_t out;

    in.width = len;
    in.offset = pos;
    memcpy(&in.data, buf, len);

    if (len > 4)
        return 1;

    err = MyIOConnectCallStructMethod(darwin0_connect, kWriteIO, &in, dataInLen, &out, &dataOutLen);
    if (err != KERN_SUCCESS)
        return 1;

    return 0;
}


/* Compatibility interface */

unsigned char inb(unsigned short addr)
{
    unsigned char ret = -1;
    darwin0_ioread(addr, &ret, 1);
    return ret;
}

unsigned short inw(unsigned short addr)
{
    unsigned short ret = -1;
    darwin0_ioread(addr, (unsigned char *)&ret, 2);
    return ret;
}

unsigned int inl(unsigned short addr)
{
    unsigned int ret = -1;
    darwin0_ioread(addr, (unsigned char *)&ret, 4);
    return ret;
}

void outb(unsigned char val, unsigned short addr)
{
    darwin0_iowrite(addr, &val, 1);
}

void outw(unsigned short val, unsigned short addr)
{
    darwin0_iowrite(addr, (unsigned char *)&val, 2);
}

void outl(unsigned int val, unsigned short addr)
{
    darwin0_iowrite(addr, (unsigned char *)&val, 4);
}

int iopl(int level)
{
    if (level) {
        if (darwin0_connect != MACH_PORT_NULL) {
            return 0;
        }
        atexit(darwin0_cleanup);
        return darwin0_init();
    }
    else {
        darwin0_cleanup();
        return 0;
    }
}
