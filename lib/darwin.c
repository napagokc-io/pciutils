/*
 *	The PCI Library -- Darwin kIOACPI access
 *
 *	Copyright (c) 2013 Apple, Inc.
 *
 *	Can be freely distributed and used under the terms of the GNU GPL v2+.
 *
 *	SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "internal.h"

#include <mach/mach_error.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOKitKeys.h>
#include "DirectHW.h"

enum {
    kACPIMethodAddressSpaceRead		= 0,
    kACPIMethodAddressSpaceWrite	= 1,
    kACPIMethodDebuggerCommand		= 2,
    kACPIMethodCount
};

#pragma pack(1)

typedef UInt32 IOACPIAddressSpaceID;

enum {
    kIOACPIAddressSpaceIDSystemMemory       = 0,
    kIOACPIAddressSpaceIDSystemIO           = 1,
    kIOACPIAddressSpaceIDPCIConfiguration   = 2,
    kIOACPIAddressSpaceIDEmbeddedController = 3,
    kIOACPIAddressSpaceIDSMBus              = 4
};

/*
 * 64-bit ACPI address
 */
union IOACPIAddress {
    UInt64 addr64;
    struct {
	unsigned int offset     :16;
	unsigned int function   :3;
	unsigned int device     :5;
	unsigned int bus        :8;
	unsigned int segment    :16;
	unsigned int reserved   :16;
    } pci;
};
typedef union IOACPIAddress IOACPIAddress;

#pragma pack()

struct AddressSpaceParam {
    UInt64			value;
    UInt32			spaceID;
    IOACPIAddress	address;
    UInt32			bitWidth;
    UInt32			bitOffset;
    UInt32			options;
};
typedef struct AddressSpaceParam AddressSpaceParam;

static io_connect_t darwin1_connect = MACH_PORT_NULL;

static void
darwin1_config(struct pci_access *a UNUSED)
{
  /* config is called at launch */
  darwin1_connect = MACH_PORT_NULL;
}

static int
darwin1_detect(struct pci_access *a)
{
  /* detect is called if the user does not specify an access method */
  io_registry_entry_t    service;
  io_connect_t           connect = MACH_PORT_NULL;
  kern_return_t          status = kIOReturnSuccess;

  if (darwin1_connect != MACH_PORT_NULL)
    return 1;

  service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("AppleACPIPlatformExpert"));
  if (service)
    {
      status = IOServiceOpen(service, mach_task_self(), 0, &connect);
      IOObjectRelease(service);
    }

  if (!service || (kIOReturnSuccess != status))
    {
      a->debug("...cannot open AppleACPIPlatformExpert (add boot arg debug=0x144 & run as root)");
      return 0;
    }
  a->debug("...using AppleACPIPlatformExpert");
  darwin1_connect = connect;
  return 1;
}

static void
darwin1_init(struct pci_access *a UNUSED)
{
  /* init is called after detect or when the user specifies this access method */
  if (darwin1_connect == MACH_PORT_NULL)
    {
      darwin1_detect(a);
      a->debug("\n");
    }
  a->fd = darwin1_connect;
}

static void
darwin1_cleanup(struct pci_access *a UNUSED)
{
}

static int
darwin1_read(struct pci_dev *d, int pos, byte *buf, int len)
{
  if (!(len == 1 || len == 2 || len == 4))
    return pci_generic_block_read(d, pos, buf, len);

  AddressSpaceParam param;
  kern_return_t     status;

  param.spaceID   = kIOACPIAddressSpaceIDPCIConfiguration;
  param.bitWidth  = len * 8;
  param.bitOffset = 0;
  param.options   = 0;

  param.address.pci.offset   = pos;
  param.address.pci.function = d->func;
  param.address.pci.device   = d->dev;
  param.address.pci.bus      = d->bus;
  param.address.pci.segment  = d->domain;
  param.address.pci.reserved = 0;
  param.value                = -1ULL;

  size_t outSize = sizeof(param);
  status = MyIOConnectCallStructMethod(d->access->fd, kACPIMethodAddressSpaceRead,
    &param, sizeof(param),
    &param, &outSize);
  if ((kIOReturnSuccess != status))
    d->access->error("darwin read failed: 0x%08x = %s", status, mach_error_string(status));

  switch (len)
    {
    case 1:
      buf[0] = (u8) param.value;
      break;
    case 2:
      ((u16 *) buf)[0] = cpu_to_le16((u16) param.value);
      break;
    case 4:
      ((u32 *) buf)[0] = cpu_to_le32((u32) param.value);
      break;
    }
  return 1;
}

static int
darwin1_write(struct pci_dev *d, int pos, byte *buf, int len)
{
  if (!(len == 1 || len == 2 || len == 4))
    return pci_generic_block_write(d, pos, buf, len);

  AddressSpaceParam param;
  kern_return_t     status;

  param.spaceID   = kIOACPIAddressSpaceIDPCIConfiguration;
  param.bitWidth  = len * 8;
  param.bitOffset = 0;
  param.options   = 0;

  param.address.pci.offset   = pos;
  param.address.pci.function = d->func;
  param.address.pci.device   = d->dev;
  param.address.pci.bus      = d->bus;
  param.address.pci.segment  = d->domain;
  param.address.pci.reserved = 0;

  switch (len)
    {
    case 1:
      param.value = buf[0];
      break;
    case 2:
      param.value = le16_to_cpu(((u16 *) buf)[0]);
      break;
    case 4:
      param.value = le32_to_cpu(((u32 *) buf)[0]);
      break;
    }

  size_t outSize = 0;
  status = MyIOConnectCallStructMethod(d->access->fd, kACPIMethodAddressSpaceWrite,
    &param, sizeof(param),
    NULL, &outSize);
  if ((kIOReturnSuccess != status))
    d->access->error("darwin write failed: 0x%08x = %s", status, mach_error_string(status));

  return 1;
}

static void
darwin1_scan(struct pci_access *a)
{
  int domain;
  int bus_number;
  byte busmap[256];

  /* setting segment for kIOACPIAddressSpaceIDPCIConfiguration doesn't seem to do anything so we'll just do one domain */
  for (domain = 0; domain < 1; domain++) {
    memset(busmap, 0, sizeof(busmap));
    for (bus_number = 0; bus_number < 0x100; bus_number++) {
      if (!busmap[bus_number]) {
        pci_generic_scan_bus(a, busmap, domain, bus_number);
      }
    }
  }
}

struct pci_methods pm_darwin = {
    .name = "darwin",
    .help = "Darwin",
    .config = darwin1_config,
    .detect = darwin1_detect,
    .init = darwin1_init,
    .cleanup = darwin1_cleanup,
    .scan = darwin1_scan,
    .fill_info = pci_generic_fill_info,
    .read = darwin1_read,
    .write = darwin1_write,
};
