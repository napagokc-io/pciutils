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

enum {
    kReadIO,
    kWriteIO,
    kPrepareMap,
    kReadMSR,
    kWriteMSR,
    kReadCpuId,
    kReadMem,
    kRead,
    kWrite,
    kNumberOfMethods
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

static io_connect_t darwin3_connect = MACH_PORT_NULL;

static void
darwin3_config(struct pci_access *a UNUSED)
{
  /* config is called at launch */
  darwin3_connect = MACH_PORT_NULL;
}

static int
darwin3_detect(struct pci_access *a)
{
  /* detect is called if the user does not specify an access method */
  io_registry_entry_t    service;
  io_connect_t           connect = MACH_PORT_NULL;
  kern_return_t          status = kIOReturnSuccess;

  if (darwin3_connect != MACH_PORT_NULL)
    return 1;

  service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("DirectHWService")); // kIOMainPortDefault is only available in macOS 12.0 and later
  if (service)
  {
    status = IOServiceOpen(service, mach_task_self(), 0, &connect);
    IOObjectRelease(service);
  }

  if (!service || (kIOReturnSuccess != status))
  {
    a->debug("...cannot open DirectHW");
    return 0;
  }
  a->debug("...using DirectHW IOPCIBridge");
  darwin3_connect = connect;
  return 1;
}

static void
darwin3_init(struct pci_access *a UNUSED)
{
  /* init is called after detect or when the user specifies this access method */
  if (darwin3_connect == MACH_PORT_NULL)
    {
      darwin3_detect(a);
      a->debug("\n");
    }
  a->fd = darwin3_connect;
}

static void
darwin3_cleanup(struct pci_access *a UNUSED)
{
}

static int
darwin3_read(struct pci_dev *d, int pos, byte *buf, int len)
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
  status = MyIOConnectCallStructMethod(d->access->fd, kRead,
    &param, sizeof(param),
    &param, &outSize);
  if ((kIOReturnSuccess != status))
    d->access->error("darwin3 read failed: 0x%08x = %s", status, mach_error_string(status));

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
darwin3_write(struct pci_dev *d, int pos, byte *buf, int len)
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

  size_t outSize = sizeof(param);
  status = MyIOConnectCallStructMethod(d->access->fd, kWrite,
    &param, sizeof(param),
    &param, &outSize);
  if ((kIOReturnSuccess != status))
    d->access->error("darwin3 write failed: 0x%08x = %s", status, mach_error_string(status));

  return 1;
}

static int pciHostBridgeCount = -1;

static void
CountPciHostBridges1(io_service_t service, io_iterator_t services)
{
  while(service)
  {
    if (IOObjectConformsTo(service, "IOPCIBridge")) {
      pciHostBridgeCount++;
    }
    else {
      io_iterator_t children;
      IORegistryEntryGetChildIterator(service, kIOServicePlane, &children);
      io_service_t child = IOIteratorNext(children);
      CountPciHostBridges1(child, children);
      IOObjectRelease(children);
    }
    IOObjectRelease(service);
    service = IOIteratorNext(services);
  }
}

static void
CountPciHostBridges(void)
{
  if (pciHostBridgeCount < 0) {
    pciHostBridgeCount = 0;
    io_service_t device = IORegistryGetRootEntry(kIOMasterPortDefault);
    CountPciHostBridges1(device, 0);
  }
}

static void
darwin3_scan(struct pci_access *a)
{
  int domain;
  int bus_number;
  byte busmap[256];
  CountPciHostBridges();

  for (domain = 0; domain < pciHostBridgeCount; domain++) {
    memset(busmap, 0, sizeof(busmap));
    for (bus_number = 0; bus_number < 0x100; bus_number++) {
      if (!busmap[bus_number]) {
        pci_generic_scan_bus(a, busmap, domain, bus_number);
      }
    }
  }
}

struct pci_methods pm_darwin3 = {
  .name = "darwin3",
  .help = "Darwin3",
  .config = darwin3_config,
  .detect = darwin3_detect,
  .init = darwin3_init,
  .cleanup = darwin3_cleanup,
  .scan = darwin3_scan,
  .fill_info = pci_generic_fill_info,
  .read = darwin3_read,
  .write = darwin3_write,
};
