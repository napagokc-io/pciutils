/*
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
  kIOPCIConfigSpace           = 0,
  kIOPCIIOSpace               = 1,
  kIOPCI32BitMemorySpace      = 2,
  kIOPCI64BitMemorySpace      = 3
};

enum
{
  kIOPCIDiagnosticsClientType = 0x99000001
};

enum {
  kIOPCIDiagnosticsMethodRead  = 0,
  kIOPCIDiagnosticsMethodWrite = 1,
  kIOPCIDiagnosticsMethodCount
};

struct IOPCIDiagnosticsParameters
{
  uint32_t   options;
  uint32_t   spaceType;
  uint32_t   bitWidth;
  uint32_t   _resv;
  uint64_t   value;
  union
  {
    uint64_t addr64;
    struct {
        unsigned int offset     :16;
        unsigned int function   :3;
        unsigned int device     :5;
        unsigned int bus        :8;
        unsigned int segment    :16;
        unsigned int reserved   :16;
    } pci;
  } address;
};
typedef struct IOPCIDiagnosticsParameters IOPCIDiagnosticsParameters;

static io_connect_t darwin2_connect = MACH_PORT_NULL;

static void
darwin2_config(struct pci_access *a UNUSED)
{
  /* config is called at launch */
  darwin2_connect = MACH_PORT_NULL;
}

static int
darwin2_detect(struct pci_access *a)
{
  /* detect is called if the user does not specify an access method */
  io_registry_entry_t    service;
  io_connect_t           connect = MACH_PORT_NULL;
  kern_return_t          status = kIOReturnSuccess;

  if (darwin2_connect != MACH_PORT_NULL)
    return 1;

  service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOPCIBridge")); // kIOMainPortDefault is only available in macOS 12.0 and later
  if (service) 
  {
    status = IOServiceOpen(service, mach_task_self(), kIOPCIDiagnosticsClientType, &connect);
    IOObjectRelease(service);
  }

  if (!service || (kIOReturnSuccess != status))
  {
    a->debug("...cannot open IOPCIBridge (add boot arg debug=0x144 & run as root)");
    return 0;
  }
  a->debug("...using IOPCIBridge");
  darwin2_connect = connect;
  return 1;
}

static void
darwin2_init(struct pci_access *a UNUSED)
{
  /* init is called after detect or when the user specifies this access method */
  if (darwin2_connect == MACH_PORT_NULL)
    {
      darwin2_detect(a);
      a->debug("\n");
    }
  a->fd = darwin2_connect;
}

static void
darwin2_cleanup(struct pci_access *a UNUSED)
{
}

static int
darwin2_read(struct pci_dev *d, int pos, byte *buf, int len)
{
  if (!(len == 1 || len == 2 || len == 4))
    return pci_generic_block_read(d, pos, buf, len);

  IOPCIDiagnosticsParameters param;
  kern_return_t              status;

  param.spaceType = kIOPCIConfigSpace;
  param.bitWidth  = len * 8;
  param.options   = 0;

  param.address.pci.offset   = pos;
  param.address.pci.function = d->func;
  param.address.pci.device   = d->dev;
  param.address.pci.bus      = d->bus;
  param.address.pci.segment  = d->domain;
  param.address.pci.reserved = 0;
  param.value                = -1ULL;

  size_t outSize = sizeof(param);
  status = MyIOConnectCallStructMethod(d->access->fd, kIOPCIDiagnosticsMethodRead,
    &param, sizeof(param),
    &param, &outSize);
  if ((kIOReturnSuccess != status))
    d->access->error("darwin2 read failed: 0x%08x = %s", status, mach_error_string(status));

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
darwin2_write(struct pci_dev *d, int pos, byte *buf, int len)
{
  if (!(len == 1 || len == 2 || len == 4))
    return pci_generic_block_write(d, pos, buf, len);

  IOPCIDiagnosticsParameters param;
  kern_return_t status;

  param.spaceType = kIOPCIConfigSpace;
  param.bitWidth  = len * 8;
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
  status = MyIOConnectCallStructMethod(d->access->fd, kIOPCIDiagnosticsMethodWrite,
    &param, sizeof(param),
    NULL, &outSize);
  if ((kIOReturnSuccess != status))
    d->access->error("darwin2 write failed: 0x%08x = %s", status, mach_error_string(status));

  return 1;
}

static void
darwin2_scan(struct pci_access *a)
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

struct pci_methods pm_darwin2 = {
  .name = "darwin2",
  .help = "Darwin2",
  .config = darwin2_config,
  .detect = darwin2_detect,
  .init = darwin2_init,
  .cleanup = darwin2_cleanup,
  .scan = darwin2_scan,
  .fill_info = pci_generic_fill_info,
  .read = darwin2_read,
  .write = darwin2_write,
};
