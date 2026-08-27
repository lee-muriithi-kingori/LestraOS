/*
 * Lestra OS — PCI Bus Enumeration
 *
 * Provides shared PCI config space access (replaces 7 duplicated copies
 * across e1000, virtio_net, virtio_blk, ahci, ac97, ac97_capture, battery).
 * Also provides pci_scan_bus() to populate a device table and
 * pci_find_device() / pci_find_class() lookup helpers.
 *
 * Config space access supports TWO mechanisms, selected per bus:
 *   1. PCIe ECAM (MMIO) — taken from the ACPI MCFG table when present
 *      and the region falls inside the identity-mapped first 4GB.
 *      Registers are at: base + (bus<<20) + (dev<<15) + (func<<12) + off.
 *   2. Type 1 configuration mechanism (IO ports 0xCF8/0xCFC) — fallback
 *      for legacy PCI and for buses not covered by any MCFG region.
 *
 * Bus enumeration is recursive: bus 0 is scanned, then every PCI-to-PCI
 * bridge (class 06/04) found on a scanned bus has its secondary/subordinate
 * bus numbers followed, so devices behind bridges on multi-bus chipsets
 * (typical on real Intel/AMD boards) are discovered — not just bus 0.
 *
 * Single host segment assumption: segment 0 only (the common case for
 * PCs and servers; ACPI multi-segment systems are out of scope here).
 * No locking needed — all PCI config access in lestraOS happens during
 * init (single-threaded, pre-scheduler).
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/pci.h>
#include <lestra/acpi.h>
#include <string.h>

/* PCI config space IO ports (Type 1 mechanism) */
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

/* PCI-to-PCI bridge registers (header type 1) */
#define PCI_SECONDARY_BUS    0x19
#define PCI_SUBORDINATE_BUS  0x1A

/* Max bus number per segment (PCI bus numbers are 0-255). */
#define PCI_MAX_BUS          256

/* =====================================================================
 * ECAM (MMIO) config space — regions discovered from ACPI MCFG
 * ===================================================================== */

struct pci_ecam_region {
    uint64_t base_addr;      /* Identity-mapped physical base */
    uint16_t segment;
    uint8_t  start_bus;
    uint8_t  end_bus;
};

static struct pci_ecam_region pci_ecam[ACPI_MAX_ECAM_REGIONS];
static int pci_ecam_count = 0;

/* Build the ECAM region table from the ACPI MCFG database.
 * A region is usable only if its entire config window is inside the
 * identity-mapped first 4GB (boot.asm huge pages) and it belongs to
 * the segment 0 we scan. Regions outside 4GB would need ioremap. */
static void pci_ecam_init(void) {
    pci_ecam_count = 0;
    if (!g_acpi.mcfg_found) return;

    for (int i = 0; i < g_acpi.n_ecam; i++) {
        const struct acpi_mcfg_entry *e = &g_acpi.ecam[i];
        if (e->pci_segment != 0) {
            pr_warn("pci: MCFG seg %u unsupported (segment 0 only)\n",
                    e->pci_segment);
            continue;
        }
        /* Topmost byte reachable in this region. */
        uint64_t window_end = e->base_addr
                              + (((uint64_t)e->end_bus << 20)
                                 | (31u << 15) | (7u << 12)) + 0xFFFu;
        if (window_end > 0xFFFFFFFFull) {
            pr_warn("pci: MCFG region at 0x%llx ends at 0x%llx (above 4GB) "
                    "— falling back to Type 1 for bus %u-%u\n",
                    (unsigned long long)e->base_addr,
                    (unsigned long long)window_end,
                    e->start_bus, e->end_bus);
            continue;
        }
        if (pci_ecam_count >= ACPI_MAX_ECAM_REGIONS) break;
        pci_ecam[pci_ecam_count].base_addr = e->base_addr;
        pci_ecam[pci_ecam_count].segment   = e->pci_segment;
        pci_ecam[pci_ecam_count].start_bus = e->start_bus;
        pci_ecam[pci_ecam_count].end_bus   = e->end_bus;
        pci_ecam_count++;
    }

    if (pci_ecam_count > 0) {
        for (int i = 0; i < pci_ecam_count; i++)
            pr_info("pci: ECAM region %d: seg %u, bus %u-%u, base 0x%llx\n",
                    i, pci_ecam[i].segment,
                    pci_ecam[i].start_bus, pci_ecam[i].end_bus,
                    (unsigned long long)pci_ecam[i].base_addr);
    }
}

/* Return the ECAM base for a bus, or 0 if the bus is only Type-1. */
static uintptr_t pci_ecam_base_for(uint8_t bus) {
    for (int i = 0; i < pci_ecam_count; i++) {
        if (bus >= pci_ecam[i].start_bus && bus <= pci_ecam[i].end_bus)
            return (uintptr_t)pci_ecam[i].base_addr;
    }
    return 0;
}

/* ECAM slot base for (bus, dev, func). */
static uintptr_t pci_ecam_slot(uint8_t bus, uint8_t dev, uint8_t func) {
    uintptr_t base = pci_ecam_base_for(bus);
    return base + ((uintptr_t)bus << 20)
                + ((uintptr_t)dev << 15)
                + ((uintptr_t)func << 12);
}

/* =====================================================================
 * Config space read/write
 * ===================================================================== */

static uint32_t pci_config_read32_typ1(uint8_t bus, uint8_t dev,
                                       uint8_t func, uint8_t off) {
    uint32_t addr = (uint32_t)0x80000000u
                    | ((uint32_t)bus << 16)
                    | ((uint32_t)dev << 11)
                    | ((uint32_t)func << 8)
                    | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (pci_ecam_count > 0 && pci_ecam_base_for(bus))
        return *(volatile uint32_t *)(pci_ecam_slot(bus, dev, func) + (off & 0xFC));
    return pci_config_read32_typ1(bus, dev, func, off);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t word;
    if (pci_ecam_count > 0 && pci_ecam_base_for(bus))
        word = *(volatile uint32_t *)(pci_ecam_slot(bus, dev, func) + (off & 0xFC));
    else
        word = pci_config_read32_typ1(bus, dev, func, off & 0xFC);
    return (uint16_t)((word >> ((off & 2) * 8)) & 0xFFFF);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t word;
    if (pci_ecam_count > 0 && pci_ecam_base_for(bus))
        word = *(volatile uint32_t *)(pci_ecam_slot(bus, dev, func) + (off & 0xFC));
    else
        word = pci_config_read32_typ1(bus, dev, func, off & 0xFC);
    return (uint8_t)((word >> ((off & 3) * 8)) & 0xFF);
}

void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func,
                        uint8_t off, uint32_t val) {
    if (pci_ecam_count > 0 && pci_ecam_base_for(bus)) {
        *(volatile uint32_t *)(pci_ecam_slot(bus, dev, func) + (off & 0xFC)) = val;
        return;
    }
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

/* Bitmap of buses already queued/scanned (dedupes bridge fan-out). */
static uint32_t pci_bus_queued[PCI_MAX_BUS / 32];

static int bus_was_queued(uint8_t bus) {
    return (pci_bus_queued[bus >> 5] >> (bus & 31)) & 1;
}

static void bus_queue(uint8_t bus) {
    pci_bus_queued[bus >> 5] |= (1u << (bus & 31));
}

/* Scan every device on `bus`, appending to the table.
 * Returns the number of fresh devices added. */
static int pci_scan_devices_on_bus(uint8_t bus, struct pci_device *table,
                                   int max_entries, int *count) {
    int added = 0;
    for (int dev = 0; dev < 32; dev++) {
        /* Read vendor/device ID (offset 0x00) — 32-bit read gives
         * vendor in low 16 bits, device in high 16 bits. */
        uint32_t id = pci_config_read32(bus, (uint8_t)dev, 0, 0x00);
        uint16_t vendor = id & 0xFFFF;
        if (vendor == 0xFFFF || vendor == 0x0000) continue;

        /* Device present — probe function 0. */
        struct pci_device *d = &table[*count];
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
        (*count)++;
        added++;
        if (*count >= max_entries) return added;

        /* If bit 7 of header_type is set, this is a multi-function device.
         * Probe functions 1-7. */
        if (d->header_type & 0x80) {
            for (int func = 1; func < 8; func++) {
                uint32_t fid = pci_config_read32(bus, (uint8_t)dev, (uint8_t)func, 0x00);
                uint16_t fvendor = fid & 0xFFFF;
                if (fvendor == 0xFFFF || fvendor == 0x0000) continue;

                d = &table[*count];
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
                (*count)++;
                added++;
                if (*count >= max_entries) return added;
            }
        }
    }
    return added;
}

/* Enqueue the child buses behind every PCI-to-PCI bridge on `bus`.
 * A bridge (class 06 / subclass 04, header type 1) reports the first
 * bus behind it in the Secondary Bus Number register and the last
 * (inclusive) in the Subordinate Bus Number register. We queue every
 * bus in that range so a bridge behind a bridge is discovered too. */
static void pci_queue_bridge_buses(uint8_t bus) {
    for (int dev = 0; dev < 32; dev++) {
        uint32_t id = pci_config_read32(bus, (uint8_t)dev, 0, 0x00);
        uint16_t vendor = id & 0xFFFF;
        if (vendor == 0xFFFF || vendor == 0x0000) continue;

        /* Functions: bridges are almost always function 0, but honor
         * the multi-function bit to be correct on weird hardware. */
        int max_funcs = (pci_config_read8(bus, (uint8_t)dev, 0,
                                          PCI_HEADER_TYPE) & 0x80) ? 8 : 1;

        for (int func = 0; func < max_funcs; func++) {
            uint32_t fid = pci_config_read32(bus, (uint8_t)dev, (uint8_t)func, 0x00);
            if ((fid & 0xFFFF) == 0xFFFF || (fid & 0xFFFF) == 0x0000) continue;

            uint32_t class_rev = pci_config_read32(bus, (uint8_t)dev,
                                                    (uint8_t)func, 0x08);
            uint8_t class_code = (class_rev >> 24) & 0xFF;
            uint8_t subclass   = (class_rev >> 16) & 0xFF;
            uint8_t htype      = pci_config_read8(bus, (uint8_t)dev,
                                                    (uint8_t)func, PCI_HEADER_TYPE);

            /* PCI-PCI (or PCIe upstream/downstream port) bridge. */
            if ((htype & 0x7F) == 0x01 &&
                class_code == PCI_CLASS_BRIDGE && subclass == 0x04) {
                uint8_t sec = pci_config_read8(bus, (uint8_t)dev, (uint8_t)func,
                                               PCI_SECONDARY_BUS);
                uint8_t sub = pci_config_read8(bus, (uint8_t)dev, (uint8_t)func,
                                               PCI_SUBORDINATE_BUS);
                /* Guard: a sane bridge always points forward (sec > bus).
                 * Clamp to the last valid bus and skip loops from
                 * firmware that reports sec==bus. */
                if (sec <= bus) continue;
                for (uint32_t b = sec; b <= sub && b < PCI_MAX_BUS; b++) {
                    if (!bus_was_queued((uint8_t)b))
                        bus_queue((uint8_t)b);
                }
            }
        }
    }
}

int pci_scan_bus(uint8_t bus, struct pci_device *table, int max_entries) {
    int count = 0;

    /* Reset the visited-bus bitmap: each top-level call starts fresh. */
    memset(pci_bus_queued, 0, sizeof(pci_bus_queued));
    bus_queue(bus);

    /* Breadth-first walk of the bus tree. pci_scan_devices_on_bus appends
     * devices; pci_queue_bridge_buses finds every bridge on the current
     * bus and queues its child buses. Loop until the queue is empty. */
    for (uint32_t q = 0; q < 256; q++) {
        if (bus_was_queued((uint8_t)q)) {
            pci_scan_devices_on_bus((uint8_t)q, table, max_entries, &count);
            pci_queue_bridge_buses((uint8_t)q);
            if (count >= max_entries) break;
        }
    }
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
    pci_ecam_init();

    pci_table_count = pci_scan_bus(0, pci_table, PCI_MAX_DEVICES);
    if (pci_ecam_count == 0)
        pr_info("pci: no ECAM/MCFG — using Type 1 config mechanism\n");
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
