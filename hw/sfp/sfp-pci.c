/*
 * symbolic + fuzz test generic PCI device
 * 2020-2021 Tong Zhang<ztong0001@gmail.com>
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "hw/block/block.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/pci/pci.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "sysemu/sysemu.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "sysemu/hostmem.h"
#include "sysemu/block-backend.h"
#include "exec/memory.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/cutils.h"
#include "hw/irq.h"

#include "aplib.h"

#define SFPBARCNT 6

typedef struct SFPMBS {
  void *parent;
  int baridx;
  MemoryRegion iomem;
  uint8_t *bar;
} SFPMBS;

typedef struct SFPCtrl {
  PCIDevice parent_obj;
  SFPMBS bars[SFPBARCNT];
  qemu_irq irq;
} SFPCtrl;

SFPCtrl* sfpctrl;

static const VMStateDescription sfp_vmstate = {
    .name = "sfp",
    .unmigratable = 1,
};


#define TYPE_SFP "sfp"
#define SFP(obj) OBJECT_CHECK(SFPCtrl, (obj), TYPE_SFP)

static void sfp_assert_irq(SFPCtrl *n) {
#if 0
    if (msix_enabled(&(n->parent_obj))) {
      // printf("msix_notify\n");
      msix_notify(&(n->parent_obj), 0);
    } else {
      // printf("pci_irq_assert\n");
      pci_irq_assert(&n->parent_obj);
    }
#else
  qemu_set_irq(n->irq, 1);
#endif
}

static void sfp_deassert_irq(SFPCtrl *n) {
#if 0
    if (!msix_enabled(&(n->parent_obj)))
      pci_irq_deassert(&n->parent_obj);
#else
  qemu_set_irq(n->irq, 0);
#endif
}

#if 0
static void sfp_gen_irq(void *opaque) {
  SFPCtrl *n = (SFPCtrl *)opaque;
  if (ap_get_irq_status()) {
    sfp_assert_irq(n);
  } else {
    sfp_deassert_irq(n);
  }
}
#else
void sfp_set_irq(int isset);
void sfp_set_irq(int isset) {
  static int sfp_irq_is_triggered;
  if ((sfp_irq_is_triggered==1) && (isset==0)) {
    sfp_deassert_irq(sfpctrl);
    sfp_irq_is_triggered = 0;
  } else if ((sfp_irq_is_triggered==0) && (isset==1)) {
    sfp_irq_is_triggered = 1;
    sfp_assert_irq(sfpctrl);
  }
}
#endif

#if 0
static int sfp_vector_unmask(PCIDevice *dev, unsigned vector, MSIMessage msg) {
  return 0;
}

static void sfp_vector_mask(PCIDevice *dev, unsigned vector) {}

static void sfp_vector_poll(PCIDevice *dev, unsigned int vector_start,
                            unsigned int vector_end) {}
#endif

static uint64_t sfp_mmio_read(void *opaque, hwaddr addr, unsigned size) {
  // any bar access will reset IRQ
  sfp_set_irq(0);
  // SFPMBS *n = (SFPMBS *)opaque;
  uint64_t val = 0;
  ap_get_fuzz_data((uint8_t *)&val, addr, size);
  // TODO: read from fuzz file @ offset addr and size
  return val;
}


static void sfp_mmio_write(void *opaque, hwaddr addr, uint64_t data,
                           unsigned size) {
  // any bar access will reset IRQ
  sfp_set_irq(0);
  ap_set_fuzz_data(data, addr, size);
  //SFPMBS *n = (SFPMBS *)opaque;
  //SFPCtrl *ctrl = (SFPCtrl *)n->parent;
  // TODO - write the fuzz file or discard?
  // printf("sfp_mmio_write: addr %#lx size %d val=%#lx\n", addr, size, data);
  // uint8_t* val = (uint8_t*)&data;
  // memcpy(&(n->bar[addr]), val, size);
  //
}

static const MemoryRegionOps sfp_mmio_ops = {
    .read = sfp_mmio_read,
    .write = sfp_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl =
        {
            .min_access_size = 1,
            .max_access_size = 8,
        },
};

static void sfp_realize(PCIDevice *pci_dev, Error **errp) {
  SFPCtrl *n = SFP(pci_dev);
  sfpctrl = n;
  for (int i = 0; i < ap_get_pci_bar_cnt(); i++) {
    if (i == 4)
      continue;
    SFPMBS *sfpmbs = &(n->bars[i]);
    // FIXME: fix bar size
    int bar_size = ap_get_pci_bar_size(i);
    uint8_t *bar = (uint8_t *)calloc(bar_size, 1);
    if (!bar) {
      printf("bar %d allocation failed!\n", i);
      exit(-1);
    }
    sfpmbs->parent = n;
    sfpmbs->baridx = i;
    sfpmbs->bar = bar;

    int bartype = ap_get_pci_bar_type(i);

    printf("sfp allocated %s bar[%d] %d bytes\n", bartype == 0 ? "PIO" : "MMIO",
           i, bar_size);

    char name[16];
    sprintf(name, "sfp-%d", i);
    memory_region_init_io(&sfpmbs->iomem, OBJECT(n), &sfp_mmio_ops, sfpmbs,
                          name, bar_size);
    // register bar
    if (bartype != 0) {
      // - mmio
      pci_register_bar(pci_dev, i, PCI_BASE_ADDRESS_SPACE_MEMORY,
                       &sfpmbs->iomem);
    } else {
      // - pio
      pci_register_bar(pci_dev, i, PCI_BASE_ADDRESS_SPACE_IO, &sfpmbs->iomem);
    }
  }
  printf("Config PCI Class\n");
  // FIXME: IRQ
  uint8_t *pci_conf = pci_dev->config;

  // pcie_endpoint_cap_init(pci_dev, 0x80);
  // specify class id here
  const char *spciclass = getenv("PCI_CLASS");
  uint32_t udata;
  if (spciclass == NULL) {
    udata = ap_get_pci_class();
  } else {
    sscanf(spciclass, "%x", &udata);
  }
  uint16_t pciclass;
  uint8_t progif;
  printf("SFP PCI CLASS=%#x\n", udata);
  progif = udata & 0xff;
  pciclass = udata >> 8;
  pci_config_set_class(pci_conf, pciclass);
  pci_config_set_prog_interface(pci_conf, progif);

  printf("Create MSIX bar\n");
  pci_conf[PCI_INTERRUPT_PIN] = 1;
  n->irq = pci_allocate_irq(pci_dev);
#if 0
  //TODO make this configurable
  if (msix_init_exclusive_bar(pci_dev, 32, 4, errp)) {
    printf("SFP:cannot init MSIX ");
    exit(-1);
    return;
  }
  for (int i = 0; i < 32; i++) {
    if (msix_vector_use(pci_dev, i)) {
      printf("SFP:cannot use MSIX %d\n", i);
      exit(-1);
    }
  }
  if (msix_set_vector_notifiers(pci_dev, sfp_vector_unmask, sfp_vector_mask,
                                sfp_vector_poll)) {
    printf("SFP: cannot set msix notifier\n");
    exit(-1);
  }
#endif

  printf("Create PCI-Express Setup\n");

  if (pci_bus_is_express(pci_get_bus(pci_dev))) {
    pci_dev->cap_present |= QEMU_PCI_CAP_EXPRESS;
    // pci_dev->cap_present |= QEMU_PCI_CAP_MSI;
    // pci_dev->cap_present |= QEMU_PCI_CAP_MSIX;
    assert(pcie_endpoint_cap_init(pci_dev, 0x80) > 0);
  } else {
    printf("SFP is not connected to PCI Express bus, capability is limited\n");
  }
  printf("Done\n");
}

static void sfp_exit(PCIDevice *pci_dev) { msix_uninit_exclusive_bar(pci_dev); }

#define PCI_PRODUCT_ID_HAPS_HSOTG 0xabc0

static void sfp_class_init(ObjectClass *oc, void *data) {
  // initialize AFL client here
  ap_init();

  DeviceClass *dc = DEVICE_CLASS(oc);
  PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
  uint16_t sfpvid = 0x8086;
  uint16_t sfppid = 0x8086;
  const char *svid = getenv("SFPVID");
  const char *spid = getenv("SFPPID");
  if (svid != NULL)
    sscanf(svid, "%hx", &sfpvid);
  else
    sfpvid = ap_get_pci_vid();
  if (spid != NULL)
    sscanf(spid, "%hx", &sfppid);
  else
    sfppid = ap_get_pci_pid();

  printf("SFP vid=%#x, pid=%#x\n", sfpvid, sfppid);

  pc->realize = sfp_realize;
  pc->exit = sfp_exit;
  pc->romfile = ap_get_rom_path();

  // need to get this from driver
  pc->vendor_id = sfpvid;
  // need to get this from driver
  pc->device_id = sfppid;
  const char *sfprevision = getenv("SFP_REVISION");
  if (sfprevision != NULL) {
    uint16_t revision;
    sscanf(sfprevision, "%hx", &revision);
    printf("SFP REVISION=%#x\n", revision);
    pc->revision = revision;
  } else {
    pc->revision = ap_get_pci_rev();
  }
  // sub vid
  const char *sfpsubvid = getenv("SFP_SUBVID");
  if (sfpsubvid != NULL) {
    uint16_t subvid;
    sscanf(sfpsubvid, "%hx", &subvid);
    printf("SFP SUBVID=%#x\n", subvid);
    pc->subsystem_vendor_id = subvid;
  } else {
    pc->subsystem_vendor_id = ap_get_pci_subvid();
  }
  // sub pid
  const char *sfpsubpid = getenv("SFP_SUBPID");
  if (sfpsubpid != NULL) {
    uint16_t subpid;
    sscanf(sfpsubpid, "%hx", &subpid);
    printf("SFP SUBPID=%#x\n", subpid);
    pc->subsystem_id = subpid;
  } else {
    pc->subsystem_id = ap_get_pci_subpid();
  }
  // does not really matter
  set_bit(DEVICE_CATEGORY_MISC, dc->categories);
  dc->desc = ap_get_dev_name();
  dc->vmsd = &sfp_vmstate;

}

static void sfp_instance_init(Object *obj) {}

static const TypeInfo sfp_info = {
    .name = TYPE_SFP,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(SFPCtrl),
    .class_init = sfp_class_init,
    .instance_init = sfp_instance_init,
    .interfaces = (InterfaceInfo[]){{INTERFACE_PCIE_DEVICE}, {}},
};

static void sfp_register_types(void) { type_register_static(&sfp_info); }

type_init(sfp_register_types)
