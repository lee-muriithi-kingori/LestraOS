/*
 * Lestra OS — PCI Bus Enumeration
 *
 * Provides shared PCI config space access (replaces 7 duplicated copies
 * across e1000, virtio_net, virtio_blk, ahci, ac97, ac97_capture, battery).
 * Also provides pci_scan_bus() to populate a device table and
 * pci_find_device() / pci_find_class() lookup helpers.
 *
 * Uses Type 1 configuration mechanism (IO ports 0xCF8/0xCFC).
 * Single-host-bridge assumption: scans bus 0 only. Multi-host-bridge
 * (PCI-to-PCI bridges) can be added later.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/pci.h>
#include <string.h>

/* =====================================================================
 * Config space read/write — Type 1 mechanism
 * ===================================================================== */

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t addr = (uint32_t)0x80000000u
                    | ((uint32_t)bus << 16)
                    | ((uint32_t)dev << 11)
                    | ((uint32_t)func << 8)
                    | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t word = pci_config_read32(bus, dev, func, off & 0xFC);
    return (uint16_t)((word >> ((off & 2) * 8)) & 0xFFFF);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t word = pci_config_read32(bus, dev, func, off & 0xFC);
    return (uint8_t)((word >> ((off & 3) * 8)) & 0xFF);
}

void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val) {
    uint32_t addr = (uint32_t)0x80000000u
                    | ((uint32_t)bus << 16)
                    | ((uint32_t)dev << 11)
                    | ((uint32_t)func << 8)
                    | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

/* =====================================================================
 * Device table (static, filled by pci_scan_bus)
 * ===================================================================== */

static struct pci_device pci_table[PCI_MAX_DEVICES];
static int pci_table_count = 0;

/* =====================================================================
 * BAR decoding helpers
 * ===================================================================== */

static void pci_read_bars(struct pci_device *d) {
    /* For type 0 headers: BARs 0-5 at offsets 0x10-0x27.
     * Type 1 (bridge) headers only have BARs 0-1. We read up to 6
     * and let the caller decide what's valid. */
    int bar_count = (d->header_type & 0x7F) == 0x01 ? 2 : PCI_MAX_BARS;
    for (int i = 0; i < bar_count; i++) {
        d->bar[i] = pci_config_read32(d->bus, d->dev, d->func,
                                        PCI_BAR0 + i * 4);
    }
}

/* =====================================================================
 * Bus scanning
 * ===================================================================== */

int pci_scan_bus(uint8_t bus, struct pci_device *table, int max_entries) {
    int count = 0;
    for (int dev = 0; dev < 32; dev++) {
        /* Read vendor/device ID (offset 0x00) — 32-bit read gives
         * vendor in low 16 bits, device in high 16 bits. */
        uint32_t id = pci_config_read32(bus, (uint8_t)dev, 0, 0x00);
        uint16_t vendor = id & 0xFFFF;
        if (vendor == 0xFFFF || vendor == 0x0000) continue;

        /* Device present — probe function 0. */
        struct pci_device *d = &table[count];
        memset(d, 0, sizeof(*d));
        d->vendor_id = vendor;
        d->device_id = (id >> 16) & 0xFFFF;
        d->bus = bus;
        d->dev = (uint8_t)dev;
        d->func = 0;

        uint32_t class_rev = pci_config_read32(bus, (uint8_t)dev, 0, 0x08);
        d->revision_id = class_rev & 0xFF;
        d->prog_if      = (class_rev >> 8) & 0xFF;
        d->subclass     = (class_rev >> 16) & 0xFF;
        d->class_code   = (class_rev >> 24) & 0xFF;

        d->header_type = pci_config_read8(bus, (uint8_t)dev, 0, PCI_HEADER_TYPE);
        d->irq_line    = pci_config_read8(bus, (uint8_t)dev, 0, PCI_IRQ_LINE);

        pci_read_bars(d);
        count++;
        if (count >= max_entries) break;

        /* If bit 7 of header_type is set, this is a multi-function device.
         * Probe functions 1-7. */
        if (d->header_type & 0x80) {
            for (int func = 1; func < 8; func++) {
                uint32_t fid = pci_config_read32(bus, (uint8_t)dev, (uint8_t)func, 0x00);
                uint16_t fvendor = fid & 0xFFFF;
                if (fvendor == 0xFFFF || fvendor == 0x0000) continue;

                d = &table[count];
                memset(d, 0, sizeof(*d));
                d->vendor_id = fvendor;
                d->device_id = (fid >> 16) & 0xFFFF;
                d->bus = bus;
                d->dev = (uint8_t)dev;
                d->func = (uint8_t)func;

                class_rev = pci_config_read32(bus, (uint8_t)dev, (uint8_t)func, 0x08);
                d->revision_id = class_rev & 0xFF;
                d->prog_if      = (class_rev >> 8) & 0xFF;
                d->subclass     = (class_rev >> 16) & 0xFF;
                d->class_code   = (class_rev >> 24) & 0xFF;

                d->header_type = pci_config_read8(bus, (uint8_t)dev, (uint8_t)func, PCI_HEADER_TYPE);
                d->irq_line    = pci_config_read8(bus, (uint8_t)dev, (uint8_t)func, PCI_IRQ_LINE);

                pci_read_bars(d);
                count++;
                if (count >= max_entries) goto done;
            }
        }
    }
done:
    return count;
}

/* Enable IO space, memory space, and bus mastering on a device. */
void pci_device_enable(struct pci_device *dev) {
    uint16_t cmd = pci_config_read16(dev->bus, dev->dev, dev->func, PCI_COMMAND);
    cmd |= PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER;
    pci_config_write32(dev->bus, dev->dev, dev->func, PCI_COMMAND, cmd);
}

/* =====================================================================
 * Lookup helpers
 * ===================================================================== */

struct pci_device *pci_find_device(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < pci_table_count; i++) {
        if (pci_table[i].vendor_id == vendor &&
            pci_table[i].device_id == device)
            return &pci_table[i];
    }
    return NULL;
}

struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass) {
    for (int i = 0; i < pci_table_count; i++) {
        if (pci_table[i].class_code == class_code &&
            pci_table[i].subclass == subclass)
            return &pci_table[i];
    }
    return NULL;
}

/* =====================================================================
 * Class name strings
 * ===================================================================== */

const char *pci_class_name(uint8_t cc) {
    switch (cc) {
    case PCI_CLASS_NETWORK:    return "Network controller";
    case PCI_CLASS_DISPLAY:    return "Display controller";
    case PCI_CLASS_STORAGE:    return "Storage controller";
    case PCI_CLASS_MULTIMEDIA: return "Multimedia controller";
    case PCI_CLASS_BRIDGE:     return "Bridge";
    case PCI_CLASS_COMM:       return "Communication controller";
    case PCI_CLASS_SYSTEM:     return "System peripheral";
    case PCI_CLASS_INPUT:      return "Input device controller";
    case PCI_CLASS_SERIAL:     return "Serial bus controller";
    case PCI_CLASS_WIRELESS:   return "Wireless controller";
    case PCI_CLASS_PROCESSOR:  return "Processor";
    case PCI_CLASS_ENCRYPTION: return "Encryption controller";
    case PCI_CLASS_SIGNAL:     return "Signal processing controller";
    case PCI_CLASS_MEMORY:     return "Memory controller";
    default:                   return "Unknown";
    }
}

/* =====================================================================
 * Init — called from kernel_main before driver init
 * ===================================================================== */

void pci_init(void) {
    pci_table_count = pci_scan_bus(0, pci_table, PCI_MAX_DEVICES);
    pr_info("pci: bus 0 scan found %d device(s)\n", pci_table_count);
    for (int i = 0; i < pci_table_count; i++) {
        struct pci_device *d = &pci_table[i];
        pr_info("pci: %02x:%02x.%x %04x:%04x [%s] %s\n",
                d->bus, d->dev, d->func,
                d->vendor_id, d->device_id,
                pci_class_name(d->class_code),
                (d->header_type & 0x7F) == 1 ? "(bridge)" : "");
    }
}

/* Global access for lspci and driver lookup. */
int pci_get_device_count(void) { return pci_table_count; }
struct pci_device *pci_get_device(int index) {
    if (index < 0 || index >= pci_table_count) return NULL;
    return &pci_table[index];
}
