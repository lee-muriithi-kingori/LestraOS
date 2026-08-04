#ifndef LESTRA_PCI_H
#define LESTRA_PCI_H
/*
 * Lestra OS — Shared PCI Config Space Access API
 *
 * Replaces the per-driver copy-pasted pci_read32/pci_write32/pci_read16/pci_read8
 * that were duplicated across e1000.c, virtio_net.c, virtio_blk.c, ahci.c,
 * ac97.c, ac97_capture.c, and battery.c (7 files, 7 copies).
 *
 * Usage:  #include <lestra/pci.h>
 *        uint32_t id = pci_config_read32(0, d, f, 0x00);
 *
 * All access is via Type 1 configuration mechanism (IO ports 0xCF8/0xCFC).
 * No locking needed — all PCI config access in lestraOS happens during init
 * (single-threaded, pre-scheduler).
 */

#include <lestra/types.h>

/* PCI config space IO ports (Type 1 mechanism) */
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

/* PCI configuration space offsets (common) */
#define PCI_VENDOR_ID    0x00  /* 16-bit */
#define PCI_DEVICE_ID    0x02  /* 16-bit */
#define PCI_COMMAND      0x04  /* 16-bit */
#define PCI_STATUS       0x06  /* 16-bit */
#define PCI_REVISION_ID  0x08  /*  8-bit */
#define PCI_CLASS_CODE   0x09  /*  8-bit: programming interface */
#define PCI_SUBCLASS     0x0A  /*  8-bit */
#define PCI_CLASS        0x0B  /*  8-bit: base class */
#define PCI_HEADER_TYPE  0x0E  /*  8-bit */
#define PCI_IRQ_LINE     0x3C  /*  8-bit */
#define PCI_CAP_PTR      0x34  /*  8-bit */

/* PCI command register bits */
#define PCI_CMD_IO_SPACE      0x0001
#define PCI_CMD_MEM_SPACE     0x0002
#define PCI_CMD_BUS_MASTER    0x0004
#define PCI_CMD_INTX_DISABLE  0x0400

/* BAR offsets */
#define PCI_BAR0  0x10
#define PCI_BAR1  0x14
#define PCI_BAR2  0x18
#define PCI_BAR3  0x1C
#define PCI_BAR4  0x20
#define PCI_BAR5  0x24

/* PCI class codes (base class) */
#define PCI_CLASS_OLD         0x00
#define PCI_CLASS_STORAGE     0x01
#define PCI_CLASS_NETWORK     0x02
#define PCI_CLASS_DISPLAY     0x03
#define PCI_CLASS_MULTIMEDIA  0x04
#define PCI_CLASS_MEMORY      0x05
#define PCI_CLASS_BRIDGE      0x06
#define PCI_CLASS_COMM        0x07
#define PCI_CLASS_SYSTEM      0x08
#define PCI_CLASS_INPUT       0x09
#define PCI_CLASS_DOCKING     0x0A
#define PCI_CLASS_PROCESSOR   0x0B
#define PCI_CLASS_SERIAL      0x0C
#define PCI_CLASS_WIRELESS    0x0D
#define PCI_CLASS_I2O         0x0E
#define PCI_CLASS_SATELLITE   0x0F
#define PCI_CLASS_ENCRYPTION  0x10
#define PCI_CLASS_SIGNAL      0x11

/* PCI device info — filled by pci_scan_bus() */
#define PCI_MAX_DEVICES  64
#define PCI_MAX_BARS     6

struct pci_device {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;    /* base class */
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision_id;
    uint8_t  header_type;
    uint8_t  irq_line;
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint32_t bar[PCI_MAX_BARS];
};

/* Core config space read/write (Type 1 mechanism) */
uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint8_t  pci_config_read8 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void     pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val);

/* Bus scanning */
int  pci_scan_bus(uint8_t bus, struct pci_device *table, int max_entries);
void pci_device_enable(struct pci_device *dev);

/* Lookup helpers */
struct pci_device *pci_find_device(uint16_t vendor, uint16_t device);
struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass);

/* Class name strings (for lspci) */
const char *pci_class_name(uint8_t class_code);

/* Global access for lspci and driver lookup. */
int             pci_get_device_count(void);
struct pci_device *pci_get_device(int index);

#endif /* LESTRA_PCI_H */
