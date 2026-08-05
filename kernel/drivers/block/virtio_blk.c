/*
 * Lestra OS - VirtIO-blk Block Device Driver
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * VirtIO-blk is the standard paravirtualized block device for KVM/QEMU.
 * This driver supports both legacy (0.9.5, IO port) and modern (1.0, MMIO
 * via PCI capabilities) transport modes. QEMU defaults to legacy for
 * transitional devices (PCI ID 0x1001).
 *
 * Polling-based (no interrupts) — consistent with AHCI driver model.
 * Each request uses 3 chained descriptors (header + data + status byte).
 *
 * References:
 *   - VirtIO Specification 1.1 (https://docs.oasis-open.org/virtio/virtio/v1.1/)
 *   - VirtIO PCI Spec 0.9.5 (legacy transport)
 *   - OSdev wiki: https://wiki.osdev.org/Virtio
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <lestra/pci.h>
#include <string.h>

/* ========================================================================
 * VirtIO PCI Legacy Register Offsets (within BAR0 IO region)
 * ======================================================================== */

#define VIRTIO_PCI_HOST_FEATURES     0x00   /* 32-bit R */
#define VIRTIO_PCI_GUEST_FEATURES    0x04   /* 32-bit RW */
#define VIRTIO_PCI_QUEUE_PFN         0x08   /* 32-bit RW */
#define VIRTIO_PCI_QUEUE_NUM         0x0C   /* 16-bit R (max queue size) */
#define VIRTIO_PCI_QUEUE_SEL         0x0E   /* 16-bit RW */
#define VIRTIO_PCI_QUEUE_NOTIFY      0x10   /* 16-bit RW */
#define VIRTIO_PCI_STATUS            0x12   /* 8-bit RW */
#define VIRTIO_PCI_ISR               0x13   /* 8-bit R */
#define VIRTIO_PCI_CONFIG_OFF        0x14   /* device-specific config (no MSI-X) */

/* ========================================================================
 * VirtIO Modern (1.0) PCI Capability Structure
 * ======================================================================== */

#define VIRTIO_PCI_CAP_ID            0x09

#define VIRTIO_PCI_CAP_COMMON_CFG    1
#define VIRTIO_PCI_CAP_NOTIFY_CFG    2
#define VIRTIO_PCI_CAP_ISR_CFG       3
#define VIRTIO_PCI_CAP_DEVICE_CFG    4
#define VIRTIO_PCI_CAP_PCI_CFG       5

struct virtio_pci_cap {
    uint8_t  cap_vndr;
    uint8_t  cap_next;
    uint8_t  cap_len;
    uint8_t  cap_type;
    uint8_t  bar;
    uint8_t  padding[3];
    uint32_t offset;
    uint32_t length;
} __packed;

/* ========================================================================
 * VirtIO Common Configuration (Modern MMIO offsets within common_cfg)
 * ======================================================================== */

#define VIRTIO_COMMON_DF_SELECT      0x00
#define VIRTIO_COMMON_DF             0x04
#define VIRTIO_COMMON_GF_SELECT      0x08
#define VIRTIO_COMMON_GF             0x0C
#define VIRTIO_COMMON_MSIX           0x10
#define VIRTIO_COMMON_NUMQ           0x12
#define VIRTIO_COMMON_STATUS         0x14
#define VIRTIO_COMMON_CFGGEN         0x15
#define VIRTIO_COMMON_Q_SELECT       0x16
#define VIRTIO_COMMON_Q_SIZE         0x18
#define VIRTIO_COMMON_Q_MSIX         0x1A
#define VIRTIO_COMMON_Q_ENABLE       0x1C
#define VIRTIO_COMMON_Q_NOTIFY_OFF   0x1E
#define VIRTIO_COMMON_Q_DESC_LO      0x20
#define VIRTIO_COMMON_Q_DESC_HI      0x24
#define VIRTIO_COMMON_Q_AVAIL_LO     0x28
#define VIRTIO_COMMON_Q_AVAIL_HI     0x2C
#define VIRTIO_COMMON_Q_USED_LO      0x30
#define VIRTIO_COMMON_Q_USED_HI      0x34

/* ========================================================================
 * VirtIO Status Bits
 * ======================================================================== */

#define VIRTIO_CONFIG_S_ACKNOWLEDGE     0x01
#define VIRTIO_CONFIG_S_DRIVER          0x02
#define VIRTIO_CONFIG_S_DRIVER_OK       0x04
#define VIRTIO_CONFIG_S_FEATURES_OK     0x08
#define VIRTIO_CONFIG_S_NEEDS_RESET     0x40
#define VIRTIO_CONFIG_S_FAILED          0x80

/* ========================================================================
 * VirtIO-blk Feature Bits
 * ======================================================================== */

#define VIRTIO_BLK_F_SIZE_MAX          1    /* max segment size */
#define VIRTIO_BLK_F_SEG_MAX           2    /* max number of segments per request */
#define VIRTIO_BLK_F_GEOMETRY          4    /* disk geometry in config */
#define VIRTIO_BLK_F_RO                5    /* read-only device */
#define VIRTIO_BLK_F_BLK_SIZE          6    /* block size in config */
#define VIRTIO_BLK_F_FLUSH             9    /* flush command supported */
#define VIRTIO_BLK_F_TOPOLOGY          10   /* topology information in config */
#define VIRTIO_BLK_F_CONFIG_WCE        11   /* writeback cache configurable */
#define VIRTIO_BLK_F_DISCARD           13   /* discard command supported */
#define VIRTIO_BLK_F_WRITE_ZEROES      14   /* write zeroes command supported */

/* Features we accept */
#define VIRTIO_BLK_DRIVER_FEATURES \
    (BIT(VIRTIO_BLK_F_SIZE_MAX) | BIT(VIRTIO_BLK_F_SEG_MAX) | \
     BIT(VIRTIO_BLK_F_BLK_SIZE) | BIT(VIRTIO_BLK_F_FLUSH) | \
     BIT(VIRTIO_BLK_F_TOPOLOGY) | BIT(VIRTIO_BLK_F_GEOMETRY))

/* ========================================================================
 * VirtIO-blk Request/Response Structures
 * ======================================================================== */

/* Request header (sent by driver to device) */
struct virtio_blk_req_hdr {
    uint32_t type;       /* VIRTIO_BLK_T_IN, VIRTIO_BLK_T_OUT, etc. */
    uint32_t reserved;   /* always 0 */
    uint64_t sector;     /* starting sector number (512-byte sectors) */
} __packed;

/* Request types */
#define VIRTIO_BLK_T_IN      0    /* read (device writes data to driver buffer) */
#define VIRTIO_BLK_T_OUT     1    /* write (driver sends data to device buffer) */
#define VIRTIO_BLK_T_FLUSH   4    /* flush device write buffer */

/* Response status byte (written by device at end of request) */
#define VIRTIO_BLK_S_OK      0
#define VIRTIO_BLK_S_IOERR   1
#define VIRTIO_BLK_S_UNSUPP  2

/* ========================================================================
 * Virtqueue Structures
 * ======================================================================== */

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __packed;

#define VIRTQ_DESC_F_NEXT      1
#define VIRTQ_DESC_F_WRITE     2
#define VIRTQ_DESC_F_INDIRECT  4

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __packed;

#define VIRTQ_AVAIL_F_NO_INTERRUPT   1

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __packed;

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
} __packed;

#define VIRTQ_USED_F_NO_NOTIFY       1

/* ========================================================================
 * Queue Configuration
 * ======================================================================== */

#define VBLK_QUEUE_SIZE       16    /* entries per virtqueue (modern mode max) */
#define VBLK_VQ_PAGES         4     /* 4 pages: enough for QueueNum up to ~250 */
#define VBLK_SECTOR_SIZE      512
#define VBLK_VQ_ALIGN         4096

/* Each request uses 3 chained descriptors:
 *   desc 0: request header (16 bytes, device reads)
 *   desc 1: data buffer (N*512 bytes, read or write depending on request type)
 *   desc 2: status byte (1 byte, device always writes)
 */
#define VBLK_REQ_DESCS        3

/* ========================================================================
 * Driver State
 * ======================================================================== */

static int        vblk_present = 0;
static int        vblk_is_modern = 0;

/* Legacy: IO port base from BAR0 */
static uint16_t   vblk_io_base = 0;

/* Modern: MMIO addresses from PCI capabilities */
static uintptr_t  vblk_common_cfg_addr = 0;
static uintptr_t  vblk_notify_cfg_addr = 0;
static uintptr_t  vblk_isr_cfg_addr    = 0;
static uintptr_t  vblk_device_cfg_addr = 0;
static uint32_t   vblk_notify_off_mult = 0;
static uintptr_t  vblk_bars[6] = {0};

/* PCI location */
static uint8_t    vblk_pci_bus  = 0;
static uint8_t    vblk_pci_dev  = 0;
static uint8_t    vblk_pci_func = 0;

/* Device info */
static uint64_t   vblk_capacity = 0;    /* number of 512-byte sectors */
static uint32_t   vblk_size_max = 0;    /* max segment size */
static uint32_t   vblk_seg_max  = 0;    /* max segments per request */
static uint32_t   vblk_blk_size = 512;  /* block size (default 512) */
static int        vblk_read_only = 0;   /* 1 if VIRTIO_BLK_F_RO */

/* Negotiated features */
static uint32_t   vblk_host_features = 0;
static uint32_t   vblk_guest_features = 0;

/* ========================================================================
 * Request Virtqueue State
 * ======================================================================== */

static uint16_t   vblk_qsz = 0;

static uint8_t    vblk_vq_mem[VBLK_VQ_PAGES * VBLK_VQ_ALIGN] __aligned(VBLK_VQ_ALIGN);
static struct virtq_desc*  vblk_desc  = NULL;
static struct virtq_avail* vblk_avail = NULL;
static struct virtq_used*  vblk_used  = NULL;

static uint16_t   vblk_avail_idx = 0;
static uint16_t   vblk_last_used = 0;

/* Request buffers (one per request slot) */
static struct virtio_blk_req_hdr vblk_req_hdrs[VBLK_QUEUE_SIZE] __aligned(16);
static uint8_t   vblk_req_status[VBLK_QUEUE_SIZE] __aligned(4);
static uint8_t   vblk_data_buf[8 * VBLK_SECTOR_SIZE] __aligned(256);  /* max 8 sectors */

/* ========================================================================
 * IO Port Access Helpers (Legacy)
 * ======================================================================== */

static inline uint32_t vblk_io_read32(uint16_t off) { return inl(vblk_io_base + off); }
static inline void vblk_io_write32(uint16_t off, uint32_t v) { outl(vblk_io_base + off, v); }
static inline uint16_t vblk_io_read16(uint16_t off) { return inw(vblk_io_base + off); }
static inline void vblk_io_write16(uint16_t off, uint16_t v) { outw(vblk_io_base + off, v); }
static inline uint8_t vblk_io_read8(uint16_t off) { return inb(vblk_io_base + off); }
static inline void vblk_io_write8(uint16_t off, uint8_t v) { outb(vblk_io_base + off, v); }

/* ========================================================================
 * MMIO Access Helpers (Modern)
 * ======================================================================== */

static inline uint32_t vblk_mmio_read32(uintptr_t addr) { return *(volatile uint32_t*)addr; }
static inline void vblk_mmio_write32(uintptr_t addr, uint32_t v) { *(volatile uint32_t*)addr = v; }
static inline uint16_t vblk_mmio_read16(uintptr_t addr) { return *(volatile uint16_t*)addr; }
static inline void vblk_mmio_write16(uintptr_t addr, uint16_t v) { *(volatile uint16_t*)addr = v; }
static inline uint8_t vblk_mmio_read8(uintptr_t addr) { return *(volatile uint8_t*)addr; }
static inline void vblk_mmio_write8(uintptr_t addr, uint8_t v) { *(volatile uint8_t*)addr = v; }

/* ========================================================================
 * Abstracted Register Access (both legacy and modern)
 * ======================================================================== */

static inline uint32_t vblk_read_host_features(void) {
    if (vblk_is_modern) {
        vblk_mmio_write32(vblk_common_cfg_addr + VIRTIO_COMMON_DF_SELECT, 0);
        return vblk_mmio_read32(vblk_common_cfg_addr + VIRTIO_COMMON_DF);
    }
    return vblk_io_read32(VIRTIO_PCI_HOST_FEATURES);
}

static inline void vblk_write_guest_features(uint32_t val) {
    if (vblk_is_modern) {
        vblk_mmio_write32(vblk_common_cfg_addr + VIRTIO_COMMON_GF_SELECT, 0);
        vblk_mmio_write32(vblk_common_cfg_addr + VIRTIO_COMMON_GF, val);
    } else {
        vblk_io_write32(VIRTIO_PCI_GUEST_FEATURES, val);
    }
}

static inline void vblk_write_status(uint8_t val) {
    if (vblk_is_modern)
        vblk_mmio_write8(vblk_common_cfg_addr + VIRTIO_COMMON_STATUS, val);
    else
        vblk_io_write8(VIRTIO_PCI_STATUS, val);
}

static inline uint8_t vblk_read_status(void) {
    if (vblk_is_modern)
        return vblk_mmio_read8(vblk_common_cfg_addr + VIRTIO_COMMON_STATUS);
    return vblk_io_read8(VIRTIO_PCI_STATUS);
}

static inline void vblk_select_queue(uint16_t idx) {
    if (vblk_is_modern)
        vblk_mmio_write16(vblk_common_cfg_addr + VIRTIO_COMMON_Q_SELECT, idx);
    else
        vblk_io_write16(VIRTIO_PCI_QUEUE_SEL, idx);
}

static inline uint16_t vblk_read_queue_size(void) {
    if (vblk_is_modern)
        return vblk_mmio_read16(vblk_common_cfg_addr + VIRTIO_COMMON_Q_SIZE);
    return vblk_io_read16(VIRTIO_PCI_QUEUE_NUM);
}

static inline void vblk_write_queue_size(uint16_t sz) {
    if (vblk_is_modern)
        vblk_mmio_write16(vblk_common_cfg_addr + VIRTIO_COMMON_Q_SIZE, sz);
}

static inline void vblk_notify_queue(uint16_t queue_idx) {
    if (vblk_is_modern) {
        uintptr_t addr = vblk_notify_cfg_addr + queue_idx * vblk_notify_off_mult;
        vblk_mmio_write16(addr, queue_idx);
    } else {
        vblk_io_write16(VIRTIO_PCI_QUEUE_NOTIFY, queue_idx);
    }
}

/* ========================================================================
 * Device-Specific Config Access (VirtIO-blk: capacity, size_max, etc.)
 * ======================================================================== */

static uint8_t vblk_read_config8(uint8_t off) {
    if (vblk_is_modern)
        return vblk_mmio_read8(vblk_device_cfg_addr + off);
    return inb(vblk_io_base + VIRTIO_PCI_CONFIG_OFF + off);
}

static uint16_t vblk_read_config16(uint8_t off) {
    if (vblk_is_modern)
        return vblk_mmio_read16(vblk_device_cfg_addr + off);
    return inb(vblk_io_base + VIRTIO_PCI_CONFIG_OFF + off) |
           ((uint16_t)inb(vblk_io_base + VIRTIO_PCI_CONFIG_OFF + off + 1) << 8);
}

static uint32_t vblk_read_config32(uint8_t off) {
    if (vblk_is_modern)
        return vblk_mmio_read32(vblk_device_cfg_addr + off);
    uint32_t val = 0;
    for (int i = 0; i < 4; i++) {
        val |= (uint32_t)inb(vblk_io_base + VIRTIO_PCI_CONFIG_OFF + off + i) << (i * 8);
    }
    return val;
}

static uint64_t vblk_read_config64(uint8_t off) {
    if (vblk_is_modern) {
        /* Read as two 32-bit values */
        uint32_t lo = vblk_mmio_read32(vblk_device_cfg_addr + off);
        uint32_t hi = vblk_mmio_read32(vblk_device_cfg_addr + off + 4);
        return (uint64_t)lo | ((uint64_t)hi << 32);
    }
    uint64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val |= (uint64_t)inb(vblk_io_base + VIRTIO_PCI_CONFIG_OFF + off + i) << (i * 8);
    }
    return val;
}

/* ========================================================================
 * PCI Device Scanning
 * Find VirtIO-blk: vendor 0x1AF4, device 0x1001 (transitional)
 * or 0x1042 (modern-only).
 * ======================================================================== */

static int pci_find_virtio_blk(uint8_t* bus, uint8_t* dev, uint8_t* func) {
    for (uint8_t d = 0; d < 32; d++) {
        for (uint8_t f = 0; f < 8; f++) {
            uint32_t id = pci_config_read32(0, d, f, 0);
            if (id == 0xFFFFFFFFu) continue;
            uint16_t vendor = id & 0xFFFF;
            uint16_t device = (id >> 16) & 0xFFFF;

            if (vendor != 0x1AF4) continue;

            int is_blk = 0;
            if (device == 0x1001) {
                uint16_t subsystem_id = pci_config_read16(0, d, f, 0x2E);
                if (subsystem_id == 2) is_blk = 1;
            } else if (device == 0x1042) {
                is_blk = 1;
            } else if (device >= 0x1040 && device <= 0x107F) {
                uint16_t subsystem_id = pci_config_read16(0, d, f, 0x2E);
                if (subsystem_id == 2) is_blk = 1;
            }

            if (!is_blk) continue;

            *bus = 0; *dev = d; *func = f;

            /* Enable device */
            uint32_t cmd = pci_config_read32(0, d, f, 0x04);
            cmd |= 0x7;
            pci_config_write32(0, d, f, 0x04, cmd);

            pr_info("virtio_blk: found at PCI 00:%u.%u, device=0x%x\n",
                    (unsigned)d, (unsigned)f, (unsigned)device);
            return 1;
        }
    }
    return 0;
}

/* ========================================================================
 * Modern Transport Detection (same logic as virtio_net.c)
 * ======================================================================== */

static int vblk_detect_modern(uint8_t bus, uint8_t dev, uint8_t func) {
    /* Cache BAR base addresses */
    for (int i = 0; i < 6; i++) {
        uint32_t bar_val = pci_config_read32(bus, dev, func, 0x10 + i * 4);
        if (bar_val == 0 || bar_val == 0xFFFFFFFFu) {
            vblk_bars[i] = 0;
            continue;
        }
        if (bar_val & 1) {
            vblk_bars[i] = bar_val & ~0x3u;
        } else {
            uint8_t bar_type = (bar_val >> 1) & 3;
            if (bar_type == 0) {
                vblk_bars[i] = bar_val & ~0xFu;
            } else if (bar_type == 2 && i + 1 < 6) {
                uint32_t bar_hi = pci_config_read32(bus, dev, func, 0x10 + (i + 1) * 4);
                vblk_bars[i] = (bar_val & ~0xFu) | ((uint64_t)bar_hi << 32);
                vblk_bars[i + 1] = 0;
            } else {
                vblk_bars[i] = bar_val & ~0xFu;
            }
        }
    }

    /* Walk PCI capability list */
    uint8_t cap_off = pci_config_read8(bus, dev, func, 0x34);
    int found = 0;

    while (cap_off != 0 && cap_off != 0xFF) {
        uint8_t cap_id = pci_config_read8(bus, dev, func, cap_off);
        if (cap_id != VIRTIO_PCI_CAP_ID) {
            cap_off = pci_config_read8(bus, dev, func, cap_off + 1);
            continue;
        }

        struct virtio_pci_cap cap;
        uint32_t w0 = pci_config_read32(bus, dev, func, cap_off);
        uint32_t w1 = pci_config_read32(bus, dev, func, cap_off + 4);
        uint32_t w2 = pci_config_read32(bus, dev, func, cap_off + 8);
        uint32_t w3 = pci_config_read32(bus, dev, func, cap_off + 12);

        cap.cap_vndr  = (uint8_t)(w0 & 0xFF);
        cap.cap_next  = (uint8_t)((w0 >> 8) & 0xFF);
        cap.cap_len   = (uint8_t)((w0 >> 16) & 0xFF);
        cap.cap_type  = (uint8_t)((w0 >> 24) & 0xFF);
        cap.bar       = (uint8_t)(w1 & 0xFF);
        cap.offset    = w2;
        cap.length    = w3;

        if (cap.bar >= 6 || vblk_bars[cap.bar] == 0) {
            cap_off = cap.cap_next;
            continue;
        }

        uintptr_t struct_addr = vblk_bars[cap.bar] + cap.offset;

        switch (cap.cap_type) {
        case VIRTIO_PCI_CAP_COMMON_CFG:
            vblk_common_cfg_addr = struct_addr;
            found++;
            break;
        case VIRTIO_PCI_CAP_NOTIFY_CFG:
            vblk_notify_cfg_addr = struct_addr;
            vblk_notify_off_mult = pci_config_read32(bus, dev, func, cap_off + 16);
            found++;
            break;
        case VIRTIO_PCI_CAP_ISR_CFG:
            vblk_isr_cfg_addr = struct_addr;
            found++;
            break;
        case VIRTIO_PCI_CAP_DEVICE_CFG:
            vblk_device_cfg_addr = struct_addr;
            found++;
            break;
        }

        cap_off = cap.cap_next;
    }

    if (vblk_common_cfg_addr != 0 && vblk_device_cfg_addr != 0) {
        pr_info("virtio_blk: modern transport detected (%u caps found)\n", found);
        return 1;
    }

    return 0;
}

/* ========================================================================
 * Virtqueue Layout Setup
 * ======================================================================== */

static void vblk_setup_vq_layout(uint8_t* vq_mem, uint16_t qsz) {
    uintptr_t base = (uintptr_t)vq_mem;

    vblk_desc  = (struct virtq_desc*)base;

    uintptr_t avail_offset = qsz * sizeof(struct virtq_desc);
    vblk_avail = (struct virtq_avail*)(base + avail_offset);

    uintptr_t used_offset = ALIGN_UP(avail_offset + 2 + 2 + qsz * 2 + 2, VBLK_VQ_ALIGN);
    vblk_used  = (struct virtq_used*)(base + used_offset);
}

/* ========================================================================
 * Virtqueue Registration
 * ======================================================================== */

static int vblk_register_vq(uint16_t queue_idx, uint16_t qsz) {
    uintptr_t desc_addr  = (uintptr_t)vblk_desc;
    uintptr_t avail_addr = (uintptr_t)vblk_avail;
    uintptr_t used_addr  = (uintptr_t)vblk_used;

    vblk_select_queue(queue_idx);

    if (vblk_is_modern) {
        uint16_t max_sz = vblk_mmio_read16(vblk_common_cfg_addr + VIRTIO_COMMON_Q_SIZE);
        if (qsz > max_sz) qsz = max_sz;
        vblk_mmio_write16(vblk_common_cfg_addr + VIRTIO_COMMON_Q_SIZE, qsz);

        vblk_mmio_write32(vblk_common_cfg_addr + VIRTIO_COMMON_Q_DESC_LO,
                          (uint32_t)(desc_addr & 0xFFFFFFFF));
        vblk_mmio_write32(vblk_common_cfg_addr + VIRTIO_COMMON_Q_DESC_HI,
                          (uint32_t)(desc_addr >> 32));
        vblk_mmio_write32(vblk_common_cfg_addr + VIRTIO_COMMON_Q_AVAIL_LO,
                          (uint32_t)(avail_addr & 0xFFFFFFFF));
        vblk_mmio_write32(vblk_common_cfg_addr + VIRTIO_COMMON_Q_AVAIL_HI,
                          (uint32_t)(avail_addr >> 32));
        vblk_mmio_write32(vblk_common_cfg_addr + VIRTIO_COMMON_Q_USED_LO,
                          (uint32_t)(used_addr & 0xFFFFFFFF));
        vblk_mmio_write32(vblk_common_cfg_addr + VIRTIO_COMMON_Q_USED_HI,
                          (uint32_t)(used_addr >> 32));

        vblk_mmio_write16(vblk_common_cfg_addr + VIRTIO_COMMON_Q_ENABLE, 1);

        if (vblk_mmio_read16(vblk_common_cfg_addr + VIRTIO_COMMON_Q_ENABLE) != 1) {
            pr_err("virtio_blk: failed to enable queue %u\n", (unsigned)queue_idx);
            return 0;
        }
    } else {
        /* Legacy: write PFN */
        /* The PFN should reference the physical address of the virtqueue page.
         * Since we're identity-mapped, vblk_vq_mem virtual = vblk_vq_mem physical.
         * PFN = physical_address / page_size */
        uint32_t pfn = (uint32_t)((uintptr_t)vblk_vq_mem / VBLK_VQ_ALIGN);
        vblk_io_write32(VIRTIO_PCI_QUEUE_PFN, pfn);
    }

    return 1;
}

/* ========================================================================
 * Public Driver API (same contract as ahci.c)
 * ======================================================================== */

int virtio_blk_init(void);
int virtio_blk_is_present(void);
int virtio_blk_read_sector(uint64_t lba, void* buf);
int virtio_blk_write_sector(uint64_t lba, const void* buf);
int virtio_blk_read_sectors(uint64_t lba, uint32_t count, void* buf);
int virtio_blk_write_sectors(uint64_t lba, uint32_t count, const void* buf);
uint64_t virtio_blk_get_capacity(void);

/* ========================================================================
 * Initialization
 * ======================================================================== */

int virtio_blk_init(void) {
    uint8_t bus, dev, func;

    /* Step 1: Find VirtIO-blk PCI device */
    if (!pci_find_virtio_blk(&bus, &dev, &func)) {
        pr_info("virtio_blk: no VirtIO-blk device found\n");
        return 0;
    }

    vblk_pci_bus  = bus;
    vblk_pci_dev  = dev;
    vblk_pci_func = func;

    /* Step 2: Detect transport mode */
    if (vblk_detect_modern(bus, dev, func)) {
        vblk_is_modern = 1;
    } else {
        vblk_is_modern = 0;
        /* Read BAR0 for legacy IO base */
        uint32_t bar0 = pci_config_read32(bus, dev, func, 0x10);
        vblk_io_base = (uint16_t)(bar0 & ~0x3u);
        pr_info("virtio_blk: legacy transport, IO base=0x%x\n", (unsigned)vblk_io_base);
    }

    /* Step 3: Reset device */
    vblk_write_status(0);
    for (int i = 0; i < 1000; i++) {
        if (vblk_read_status() == 0) break;
    }
    if (vblk_read_status() != 0) {
        pr_err("virtio_blk: device reset failed\n");
        return 0;
    }

    /* Step 4: Acknowledge */
    vblk_write_status(VIRTIO_CONFIG_S_ACKNOWLEDGE);

    /* Step 5: Read host features */
    vblk_host_features = vblk_read_host_features();
    pr_info("virtio_blk: host features=0x%x\n", (unsigned)vblk_host_features);

    /* Step 6: Negotiate features */
    vblk_guest_features = vblk_host_features & VIRTIO_BLK_DRIVER_FEATURES;
    vblk_write_guest_features(vblk_guest_features);
    pr_info("virtio_blk: negotiated features=0x%x\n", (unsigned)vblk_guest_features);

    /* Check if read-only */
    vblk_read_only = (vblk_guest_features & BIT(VIRTIO_BLK_F_RO)) ? 1 : 0;

    /* Step 7: Driver status */
    vblk_write_status(VIRTIO_CONFIG_S_DRIVER);

    if (vblk_is_modern) {
        vblk_write_status(vblk_read_status() | VIRTIO_CONFIG_S_FEATURES_OK);
        if (!(vblk_read_status() & VIRTIO_CONFIG_S_FEATURES_OK)) {
            pr_err("virtio_blk: feature negotiation failed\n");
            vblk_write_status(VIRTIO_CONFIG_S_FAILED);
            return 0;
        }
    }

    /* Step 8: Read device configuration */
    /* Capacity is always present in config space */
    vblk_capacity = vblk_read_config64(0);
    pr_info("virtio_blk: capacity=%u sectors (%u MB)\n",
            (unsigned)vblk_capacity,
            (unsigned)(vblk_capacity * VBLK_SECTOR_SIZE / (1024 * 1024)));

    if (vblk_guest_features & BIT(VIRTIO_BLK_F_SIZE_MAX)) {
        vblk_size_max = vblk_read_config32(8);
        pr_info("virtio_blk: size_max=%u\n", (unsigned)vblk_size_max);
    } else {
        vblk_size_max = VBLK_SECTOR_SIZE;
    }

    if (vblk_guest_features & BIT(VIRTIO_BLK_F_SEG_MAX)) {
        vblk_seg_max = vblk_read_config32(12);
        pr_info("virtio_blk: seg_max=%u\n", (unsigned)vblk_seg_max);
    } else {
        vblk_seg_max = 1;
    }

    if (vblk_guest_features & BIT(VIRTIO_BLK_F_BLK_SIZE)) {
        vblk_blk_size = vblk_read_config32(20);
        pr_info("virtio_blk: blk_size=%u\n", (unsigned)vblk_blk_size);
    }

    /* Step 9: Set up the request virtqueue (queue 0) */
    vblk_select_queue(0);
    vblk_qsz = vblk_read_queue_size();
    if (vblk_qsz == 0) vblk_qsz = VBLK_QUEUE_SIZE;
    /* CRITICAL: In legacy mode, do NOT clamp queue size.
     * The device computes virtqueue layout from its own QueueNum.
     * If our layout uses a different (smaller) QueueNum, the
     * avail/used rings end up at different offsets and the
     * device never sees our descriptors. Clamp only for modern
     * transport where we write the size back to the device. */
    if (vblk_is_modern && vblk_qsz > VBLK_QUEUE_SIZE) {
        vblk_qsz = VBLK_QUEUE_SIZE;
        vblk_write_queue_size(vblk_qsz);
    }

    /* Layout the virtqueue structures */
    vblk_setup_vq_layout(vblk_vq_mem, vblk_qsz);

    /* Initialize virtqueue memory */
    memset(vblk_vq_mem, 0, sizeof(vblk_vq_mem));

    vblk_avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;
    vblk_avail->idx   = 0;
    vblk_avail_idx    = 0;
    vblk_last_used    = 0;

    /* Initialize descriptors (will be filled per request) */
    for (uint16_t i = 0; i < vblk_qsz; i++) {
        vblk_desc[i].addr  = 0;
        vblk_desc[i].len   = 0;
        vblk_desc[i].flags = 0;
        vblk_desc[i].next  = 0;
    }

    /* Register virtqueue */
    if (!vblk_register_vq(0, vblk_qsz)) {
        pr_err("virtio_blk: failed to register request queue\n");
        vblk_write_status(VIRTIO_CONFIG_S_FAILED);
        return 0;
    }

    /* Step 10: Signal DRIVER_OK */
    vblk_write_status(vblk_read_status() | VIRTIO_CONFIG_S_DRIVER_OK);

    vblk_present = 1;
    pr_info("virtio_blk: initialized (mode=%s, qsz=%u, %s)\n",
            vblk_is_modern ? "modern" : "legacy",
            (unsigned)vblk_qsz,
            vblk_read_only ? "read-only" : "read-write");
    return 1;
}

/* ========================================================================
 * Query Functions
 * ======================================================================== */

int virtio_blk_is_present(void) {
    return vblk_present;
}

uint64_t virtio_blk_get_capacity(void) {
    return vblk_capacity;
}

/* ========================================================================
 * Execute a block request (read or write)
 * Uses 3 chained descriptors:
 *   desc 0: request header (device reads)
 *   desc 1: data buffer (device reads for write, device writes for read)
 *   desc 2: status byte (device always writes)
 *
 * Returns: number of sectors transferred, or 0 on error.
 * ======================================================================== */

static int vblk_do_request(uint32_t type, uint64_t sector, uint32_t count,
                            void* data_buf) {
    if (!vblk_present) return 0;
    if (count == 0) return 0;
    if (count > 8) count = 8;  /* limit to 8 sectors per request (4KB max) */
    if (type == VIRTIO_BLK_T_OUT && vblk_read_only) {
        pr_warn("virtio_blk: write attempted on read-only device\n");
        return 0;
    }

    uint32_t byte_count = count * VBLK_SECTOR_SIZE;

    /* For writes, copy data to our DMA buffer first */
    if (type == VIRTIO_BLK_T_OUT) {
        memcpy(vblk_data_buf, data_buf, byte_count);
    }

    /* Fill the request header */
    vblk_req_hdrs[0].type     = type;
    vblk_req_hdrs[0].reserved = 0;
    vblk_req_hdrs[0].sector   = sector;

    /* Reset status byte */
    vblk_req_status[0] = 0xFF;  /* device will write VIRTIO_BLK_S_OK or error */

    /* Set up 3 chained descriptors */

    /* Descriptor 0: request header (device reads, not writable) */
    vblk_desc[0].addr  = (uint64_t)(uintptr_t)&vblk_req_hdrs[0];
    vblk_desc[0].len   = sizeof(struct virtio_blk_req_hdr);
    vblk_desc[0].flags = VIRTQ_DESC_F_NEXT;
    vblk_desc[0].next  = 1;

    /* Descriptor 1: data buffer */
    vblk_desc[1].addr  = (uint64_t)(uintptr_t)vblk_data_buf;
    vblk_desc[1].len   = byte_count;
    vblk_desc[1].flags = VIRTQ_DESC_F_NEXT;
    if (type == VIRTIO_BLK_T_IN) {
        vblk_desc[1].flags |= VIRTQ_DESC_F_WRITE;  /* device writes data */
    }
    vblk_desc[1].next  = 2;

    /* Descriptor 2: status byte (device always writes) */
    vblk_desc[2].addr  = (uint64_t)(uintptr_t)&vblk_req_status[0];
    vblk_desc[2].len   = 1;
    vblk_desc[2].flags = VIRTQ_DESC_F_WRITE;
    vblk_desc[2].next  = 0;  /* end of chain */

    /* Memory barrier: ensure all descriptor + buffer writes are visible */
    barrier();

    /* Add descriptor chain head (desc 0) to available ring */
    uint16_t avail_slot = vblk_avail_idx % vblk_qsz;
    vblk_avail->ring[avail_slot] = 0;  /* head descriptor index */
    vblk_avail_idx++;
    barrier();
    vblk_avail->idx = vblk_avail_idx;

    /* Ring the doorbell */
    barrier();
    vblk_notify_queue(0);

    /* Wait for the device to process the request (poll used ring).
     * CRITICAL: The used ring is written by the device via DMA.
     * GCC -O2 hoists non-volatile reads out of tight loops, so we
     * MUST read through a volatile-qualified pointer to force a
     * real load from memory on every iteration. */
    volatile struct virtq_used* vblk_used_v = (volatile struct virtq_used*)vblk_used;
    int timed_out = 1;
    for (int i = 0; i < 2000000; i++) {
        uint16_t used_idx = vblk_used_v->idx;
        if (used_idx != vblk_last_used) {
            timed_out = 0;
            break;
        }
    }

    if (timed_out) {
        /* Diagnostic: re-read once with volatile to check if device
         * completed after our loop exited (confirms hoisting bug). */
        uint16_t final_idx = vblk_used_v->idx;
        pr_warn("virtio_blk: request timeout (type=%u, sector=%u, count=%u) "
                "used_idx=%u last_used=%u\n",
                (unsigned)type, (unsigned)sector, (unsigned)count,
                (unsigned)final_idx, (unsigned)vblk_last_used);
        return 0;
    }

    /* Process the used ring entry (read through volatile to get
     * device-written len, then use non-volatile for status check). */
    uint16_t used_slot = vblk_last_used % vblk_qsz;
    struct virtq_used_elem* ue = &((volatile struct virtq_used*)vblk_used)->ring[used_slot];
    vblk_last_used++;

    /* Check status byte */
    if (vblk_req_status[0] != VIRTIO_BLK_S_OK) {
        pr_warn("virtio_blk: request failed (type=%u, sector=%u, status=%u)\n",
                (unsigned)type, (unsigned)sector, (unsigned)vblk_req_status[0]);
        return 0;
    }

    /* For reads, copy data from DMA buffer to caller's buffer */
    if (type == VIRTIO_BLK_T_IN) {
        memcpy(data_buf, vblk_data_buf, byte_count);
    }

    return (int)count;
}

/* ========================================================================
 * Single-sector operations (same API as AHCI driver's ahci_read/write)
 * ======================================================================== */

int virtio_blk_read_sector(uint64_t lba, void* buf) {
    return vblk_do_request(VIRTIO_BLK_T_IN, lba, 1, buf) ? 1 : 0;
}

int virtio_blk_write_sector(uint64_t lba, const void* buf) {
    return vblk_do_request(VIRTIO_BLK_T_OUT, lba, 1, (void*)buf) ? 1 : 0;
}

/* ========================================================================
 * Multi-sector operations (same API as AHCI driver's ahci_read/write_sectors)
 * ======================================================================== */

int virtio_blk_read_sectors(uint64_t lba, uint32_t count, void* buf) {
    if (!vblk_present || count == 0) return 0;
    return vblk_do_request(VIRTIO_BLK_T_IN, lba, count, buf);
}

int virtio_blk_write_sectors(uint64_t lba, uint32_t count, const void* buf) {
    if (!vblk_present || count == 0) return 0;
    if (vblk_read_only) return 0;
    return vblk_do_request(VIRTIO_BLK_T_OUT, lba, count, (void*)buf);
}
