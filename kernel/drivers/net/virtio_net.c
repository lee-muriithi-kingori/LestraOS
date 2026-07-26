/*
 * Lestra OS - VirtIO-net Network Device Driver
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * VirtIO is the standard paravirtualized device interface for KVM/QEMU.
 * This driver supports both legacy (0.9.5, IO port) and modern (1.0, MMIO
 * via PCI capabilities) transport modes. QEMU defaults to legacy mode for
 * transitional devices (PCI ID 0x1000), so legacy is the primary path.
 *
 * Polling-based (no interrupts) — call virtio_net_recv() from net_tick()
 * to drain the RX virtqueue, consistent with the e1000 driver model.
 *
 * References:
 *   - VirtIO Specification 1.1 (https://docs.oasis-open.org/virtio/virtio/v1.1/)
 *   - VirtIO PCI Spec 0.9.5 (legacy transport)
 *   - OSdev wiki: https://wiki.osdev.org/Virtio
 */

#include <lestra/types.h>
#include <lestra/net.h>
#include <lestra/printk.h>
#include <string.h>

/* ========================================================================
 * PCI config space access (same mechanism as e1000.c / ahci.c)
 * ======================================================================== */

#define PCI_ADDR  0xCF8
#define PCI_DATA  0xCFC

static inline uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t addr = (uint32_t)0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8)
                  | (off & 0xFC);
    outl(PCI_ADDR, addr);
    return inl(PCI_DATA);
}

static inline void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v) {
    uint32_t addr = (uint32_t)0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8)
                  | (off & 0xFC);
    outl(PCI_ADDR, addr);
    outl(PCI_DATA, v);
}

/* Read a 16-bit word from PCI config space (split across 32-bit regs) */
static inline uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t word = pci_read32(bus, dev, func, off & 0xFC);
    return (uint16_t)((word >> ((off & 2) * 8)) & 0xFFFF);
}

/* Read a 8-bit byte from PCI config space */
static inline uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t word = pci_read32(bus, dev, func, off & 0xFC);
    return (uint8_t)((word >> ((off & 3) * 8)) & 0xFF);
}

/* ========================================================================
 * VirtIO PCI Legacy Register Offsets (byte offsets within BAR0 IO region)
 * These are the well-known offsets from the VirtIO PCI 0.9.5 spec and
 * match QEMU's implementation. Verified against Linux kernel virtio_pci.
 * ======================================================================== */

#define VIRTIO_PCI_HOST_FEATURES     0x00   /* 32-bit R  - device features */
#define VIRTIO_PCI_GUEST_FEATURES    0x04   /* 32-bit RW - driver features */
#define VIRTIO_PCI_QUEUE_PFN         0x08   /* 32-bit RW - queue page frame number */
#define VIRTIO_PCI_QUEUE_NUM         0x0C   /* 16-bit R  - max queue size */
#define VIRTIO_PCI_QUEUE_SEL         0x0E   /* 16-bit RW - queue selector */
#define VIRTIO_PCI_QUEUE_NOTIFY      0x10   /* 16-bit RW - queue notify (doorbell) */
#define VIRTIO_PCI_STATUS            0x12   /* 8-bit  RW - device status */
#define VIRTIO_PCI_ISR               0x13   /* 8-bit  R  - interrupt status */
#define VIRTIO_PCI_CONFIG_OFF        0x14   /* device-specific config (no MSI-X) */

/* ========================================================================
 * VirtIO Modern (1.0) PCI Capability Structure
 * Found by walking PCI capability list (cap ID 0x09 = vendor-specific).
 * ======================================================================== */

#define VIRTIO_PCI_CAP_ID            0x09   /* Vendor-specific PCI cap ID */

/* Capability types within the vendor-specific cap */
#define VIRTIO_PCI_CAP_COMMON_CFG    1      /* Common configuration */
#define VIRTIO_PCI_CAP_NOTIFY_CFG    2      /* Notifications */
#define VIRTIO_PCI_CAP_ISR_CFG       3      /* ISR status */
#define VIRTIO_PCI_CAP_DEVICE_CFG    4      /* Device-specific config */
#define VIRTIO_PCI_CAP_PCI_CFG       5      /* PCI config access */

struct virtio_pci_cap {
    uint8_t  cap_vndr;     /* 0x09 */
    uint8_t  cap_next;
    uint8_t  cap_len;
    uint8_t  cap_type;     /* 1-5 as above */
    uint8_t  bar;          /* which BAR */
    uint8_t  padding[3];
    uint32_t offset;       /* offset within BAR */
    uint32_t length;       /* length of structure */
} __packed;

struct virtio_pci_notify_cap {
    struct virtio_pci_cap cap;
    uint32_t notify_off_multiplier;
} __packed;

/* ========================================================================
 * VirtIO Common Configuration (Modern MMIO, within common_cfg BAR)
 * ======================================================================== */

#define VIRTIO_COMMON_DF_SELECT      0x00   /* 32-bit RW */
#define VIRTIO_COMMON_DF             0x04   /* 32-bit R  */
#define VIRTIO_COMMON_GF_SELECT      0x08   /* 32-bit RW */
#define VIRTIO_COMMON_GF             0x0C   /* 32-bit RW */
#define VIRTIO_COMMON_MSIX           0x10   /* 16-bit RW */
#define VIRTIO_COMMON_NUMQ           0x12   /* 16-bit R  */
#define VIRTIO_COMMON_STATUS         0x14   /* 8-bit  RW */
#define VIRTIO_COMMON_CFGGEN         0x15   /* 8-bit  R  */
#define VIRTIO_COMMON_Q_SELECT       0x16   /* 16-bit RW */
#define VIRTIO_COMMON_Q_SIZE         0x18   /* 16-bit RW */
#define VIRTIO_COMMON_Q_MSIX         0x1A   /* 16-bit RW */
#define VIRTIO_COMMON_Q_ENABLE       0x1C   /* 16-bit RW */
#define VIRTIO_COMMON_Q_NOTIFY_OFF   0x1E   /* 16-bit R  */
#define VIRTIO_COMMON_Q_DESC_LO      0x20   /* 32-bit RW */
#define VIRTIO_COMMON_Q_DESC_HI      0x24   /* 32-bit RW */
#define VIRTIO_COMMON_Q_AVAIL_LO     0x28   /* 32-bit RW */
#define VIRTIO_COMMON_Q_AVAIL_HI     0x2C   /* 32-bit RW */
#define VIRTIO_COMMON_Q_USED_LO      0x30   /* 32-bit RW */
#define VIRTIO_COMMON_Q_USED_HI      0x34   /* 32-bit RW */

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
 * VirtIO-net Feature Bits
 * ======================================================================== */

#define VIRTIO_NET_F_CSUM               0
#define VIRTIO_NET_F_GUEST_CSUM         1
#define VIRTIO_NET_F_MAC                5
#define VIRTIO_NET_F_GSO                6
#define VIRTIO_NET_F_GUEST_TSO4         7
#define VIRTIO_NET_F_GUEST_TSO6         8
#define VIRTIO_NET_F_GUEST_ECN          9
#define VIRTIO_NET_F_GUEST_UFO          10
#define VIRTIO_NET_F_HOST_TSO4          11
#define VIRTIO_NET_F_HOST_TSO6          12
#define VIRTIO_NET_F_HOST_ECN           13
#define VIRTIO_NET_F_HOST_UFO           14
#define VIRTIO_NET_F_MRG_RXBUF          15
#define VIRTIO_NET_F_STATUS             16
#define VIRTIO_NET_F_CTRL_VQ            17
#define VIRTIO_NET_F_CTRL_RX            18
#define VIRTIO_NET_F_CTRL_VLAN          19
#define VIRTIO_NET_F_GUEST_ANNOUNCE     21
#define VIRTIO_NET_F_MQ                 22

/* Features we accept (the driver supports) */
#define VIRTIO_NET_DRIVER_FEATURES \
    (BIT(VIRTIO_NET_F_MAC) | BIT(VIRTIO_NET_F_STATUS) | \
     BIT(VIRTIO_NET_F_CSUM) | BIT(VIRTIO_NET_F_GUEST_CSUM))

/* ========================================================================
 * VirtIO-net Header (precedes every packet in the virtqueue buffer)
 * ======================================================================== */

struct virtio_net_hdr {
    uint16_t flags;        /* VIRTIO_NET_HDR_F_NEEDS_CSUM etc. */
    uint16_t gso_type;     /* VIRTIO_NET_HDR_GSO_NONE etc. */
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;  /* only if VIRTIO_NET_F_MRG_RXBUF */
} __packed;

#define VIRTIO_NET_HDR_SIZE   12

/* ========================================================================
 * Virtqueue Structures (shared memory ring buffers)
 * ======================================================================== */

struct virtq_desc {
    uint64_t addr;         /* physical address of buffer */
    uint32_t len;          /* length of buffer */
    uint16_t flags;        /* VIRTQ_DESC_F_NEXT, VIRTQ_DESC_F_WRITE */
    uint16_t next;         /* next descriptor index (if F_NEXT) */
} __packed;

#define VIRTQ_DESC_F_NEXT      1
#define VIRTQ_DESC_F_WRITE     2
#define VIRTQ_DESC_F_INDIRECT  4

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;          /* next available ring entry (driver increments) */
    uint16_t ring[];       /* queue_size entries: head descriptor indices */
    /* uint16_t used_event; follows ring[] */
} __packed;

#define VIRTQ_AVAIL_F_NO_INTERRUPT   1

/* Used ring entry */
struct virtq_used_elem {
    uint32_t id;           /* head descriptor index of completed chain */
    uint32_t len;          /* total bytes written (for writable descriptors) */
} __packed;

struct virtq_used {
    uint16_t flags;
    uint16_t idx;          /* next used ring entry (device increments) */
    struct virtq_used_elem ring[]; /* queue_size entries */
    /* uint16_t avail_event; follows ring[] */
} __packed;

#define VIRTQ_USED_F_NO_NOTIFY       1

/* ========================================================================
 * Queue Configuration
 * ======================================================================== */

#define VNET_QUEUE_SIZE       32    /* 32 entries per virtqueue (power of 2) */
#define VNET_RX_BUF_SZ        2048  /* per RX data buffer */
#define VNET_TX_BUF_SZ        2048  /* per TX data buffer */

/* Virtqueue memory layout (legacy):
 *   Descriptor table at offset 0  (queue_size * 16 bytes)
 *   Available ring right after descriptors
 *   Used ring at next 4096-byte boundary
 * Total allocation must be at least 3 pages for safety.
 *
 * For modern mode, the three components can be at separate addresses,
 * but we keep the same layout for simplicity. */
#define VNET_VQ_ALIGN         4096
#define VNET_VQ_PAGES         3     /* 3 pages = 12288 bytes per virtqueue */

/* Compute avail ring size (including used_event) */
#define VQ_AVAIL_SZ(qs)    (2 + 2 + (qs) * 2 + 2)
/* Compute used ring size (including avail_event) */
#define VQ_USED_SZ(qs)     (2 + 2 + (qs) * sizeof(struct virtq_used_elem) + 2)

/* ========================================================================
 * Driver State
 * ======================================================================== */

static int        vnet_present = 0;
static int        vnet_is_modern = 0;

/* Legacy transport: IO port base from BAR0 */
static uint16_t   vnet_io_base = 0;

/* Modern transport: MMIO addresses from PCI capabilities */
static uintptr_t  vnet_common_cfg_addr = 0;   /* common_cfg BAR+offset */
static uintptr_t  vnet_notify_cfg_addr = 0;   /* notify BAR+offset */
static uintptr_t  vnet_isr_cfg_addr    = 0;   /* ISR BAR+offset */
static uintptr_t  vnet_device_cfg_addr = 0;   /* device-specific config */
static uint32_t   vnet_notify_off_mult = 0;   /* notify offset multiplier */
static uint8_t    vnet_common_cfg_bar  = 0;
static uint8_t    vnet_notify_cfg_bar  = 0;
static uint8_t    vnet_isr_cfg_bar     = 0;
static uint8_t    vnet_device_cfg_bar  = 0;
/* BAR base addresses for modern mode (cached) */
static uintptr_t  vnet_bars[6] = {0};

/* PCI location */
static uint8_t    vnet_pci_bus  = 0;
static uint8_t    vnet_pci_dev  = 0;
static uint8_t    vnet_pci_func = 0;

/* MAC address */
static mac_addr_t vnet_mac = MAC_ZERO;

/* Negotiated features */
static uint32_t   vnet_host_features = 0;
static uint32_t   vnet_guest_features = 0;

/* ========================================================================
 * RX Virtqueue State
 * ======================================================================== */

static uint16_t   vnet_rx_qsz = 0;   /* actual queue size (from device) */

/* Virtqueue memory (page-aligned for legacy requirement) */
static uint8_t    vnet_rx_vq_mem[VNET_VQ_PAGES * VNET_VQ_ALIGN] __aligned(VNET_VQ_ALIGN);
static struct virtq_desc* vnet_rx_desc  = NULL;
static struct virtq_avail* vnet_rx_avail = NULL;
static struct virtq_used*  vnet_rx_used  = NULL;
static uint16_t*  vnet_rx_used_event = NULL;  /* used_event at end of avail ring */
static uint16_t*  vnet_rx_avail_event = NULL;  /* avail_event at end of used ring */

static uint16_t   vnet_rx_avail_idx = 0;  /* driver's next avail slot index */
static uint16_t   vnet_rx_last_used = 0;  /* driver's last processed used idx */

/* RX buffers: virtio_net_hdr + packet data, one per descriptor */
static uint8_t    vnet_rx_bufs[VNET_QUEUE_SIZE][VIRTIO_NET_HDR_SIZE + VNET_RX_BUF_SZ] __aligned(16);

/* ========================================================================
 * TX Virtqueue State
 * ======================================================================== */

static uint16_t   vnet_tx_qsz = 0;

static uint8_t    vnet_tx_vq_mem[VNET_VQ_PAGES * VNET_VQ_ALIGN] __aligned(VNET_VQ_ALIGN);
static struct virtq_desc* vnet_tx_desc  = NULL;
static struct virtq_avail* vnet_tx_avail = NULL;
static struct virtq_used*  vnet_tx_used  = NULL;
static uint16_t*  vnet_tx_used_event = NULL;
static uint16_t*  vnet_tx_avail_event = NULL;

static uint16_t   vnet_tx_avail_idx = 0;
static uint16_t   vnet_tx_last_used = 0;
static uint16_t   vnet_tx_next_desc = 0;  /* rotating descriptor index */

/* TX buffers: virtio_net_hdr + packet data */
static uint8_t    vnet_tx_bufs[VNET_QUEUE_SIZE][VIRTIO_NET_HDR_SIZE + VNET_TX_BUF_SZ] __aligned(16);

/* ========================================================================
 * IO Port Access Helpers (Legacy Transport)
 * ======================================================================== */

static inline uint32_t vnet_io_read32(uint16_t off) {
    return inl(vnet_io_base + off);
}
static inline void vnet_io_write32(uint16_t off, uint32_t val) {
    outl(vnet_io_base + off, val);
}
static inline uint16_t vnet_io_read16(uint16_t off) {
    return inw(vnet_io_base + off);
}
static inline void vnet_io_write16(uint16_t off, uint16_t val) {
    outw(vnet_io_base + off, val);
}
static inline uint8_t vnet_io_read8(uint16_t off) {
    return inb(vnet_io_base + off);
}
static inline void vnet_io_write8(uint16_t off, uint8_t val) {
    outb(vnet_io_base + off, val);
}

/* ========================================================================
 * MMIO Access Helpers (Modern Transport)
 * For modern mode, registers are at specific offsets within BAR regions.
 * We use volatile pointer access (like e1000 MMIO).
 * ======================================================================== */

static inline uint32_t vnet_mmio_read32(uintptr_t addr) {
    return *(volatile uint32_t*)addr;
}
static inline void vnet_mmio_write32(uintptr_t addr, uint32_t val) {
    *(volatile uint32_t*)addr = val;
}
static inline uint16_t vnet_mmio_read16(uintptr_t addr) {
    return *(volatile uint16_t*)addr;
}
static inline void vnet_mmio_write16(uintptr_t addr, uint16_t val) {
    *(volatile uint16_t*)addr = val;
}
static inline uint8_t vnet_mmio_read8(uintptr_t addr) {
    return *(volatile uint8_t*)addr;
}
static inline void vnet_mmio_write8(uintptr_t addr, uint8_t val) {
    *(volatile uint8_t*)addr = val;
}

/* ========================================================================
 * Abstracted Register Access (works for both legacy and modern)
 * ======================================================================== */

static inline uint32_t vnet_read_host_features(void) {
    if (vnet_is_modern) {
        vnet_mmio_write32(vnet_common_cfg_addr + VIRTIO_COMMON_DF_SELECT, 0);
        return vnet_mmio_read32(vnet_common_cfg_addr + VIRTIO_COMMON_DF);
    }
    return vnet_io_read32(VIRTIO_PCI_HOST_FEATURES);
}

static inline void vnet_write_guest_features(uint32_t val) {
    if (vnet_is_modern) {
        vnet_mmio_write32(vnet_common_cfg_addr + VIRTIO_COMMON_GF_SELECT, 0);
        vnet_mmio_write32(vnet_common_cfg_addr + VIRTIO_COMMON_GF, val);
    } else {
        vnet_io_write32(VIRTIO_PCI_GUEST_FEATURES, val);
    }
}

static inline void vnet_write_status(uint8_t val) {
    if (vnet_is_modern) {
        vnet_mmio_write8(vnet_common_cfg_addr + VIRTIO_COMMON_STATUS, val);
    } else {
        vnet_io_write8(VIRTIO_PCI_STATUS, val);
    }
}

static inline uint8_t vnet_read_status(void) {
    if (vnet_is_modern) {
        return vnet_mmio_read8(vnet_common_cfg_addr + VIRTIO_COMMON_STATUS);
    }
    return vnet_io_read8(VIRTIO_PCI_STATUS);
}

static inline void vnet_select_queue(uint16_t idx) {
    if (vnet_is_modern) {
        vnet_mmio_write16(vnet_common_cfg_addr + VIRTIO_COMMON_Q_SELECT, idx);
    } else {
        vnet_io_write16(VIRTIO_PCI_QUEUE_SEL, idx);
    }
}

static inline uint16_t vnet_read_queue_size(void) {
    if (vnet_is_modern) {
        return vnet_mmio_read16(vnet_common_cfg_addr + VIRTIO_COMMON_Q_SIZE);
    }
    return vnet_io_read16(VIRTIO_PCI_QUEUE_NUM);
}

static inline void vnet_write_queue_size(uint16_t sz) {
    if (vnet_is_modern) {
        vnet_mmio_write16(vnet_common_cfg_addr + VIRTIO_COMMON_Q_SIZE, sz);
    } else {
        /* Legacy: queue size is implicit, device uses max size */
    }
}

/* Ring the doorbell to notify the device about available buffers */
static inline void vnet_notify_queue(uint16_t queue_idx) {
    if (vnet_is_modern) {
        /* Modern: notify at notify_cfg_addr + queue_notify_offset * multiplier */
        uintptr_t addr = vnet_notify_cfg_addr + queue_idx * vnet_notify_off_mult;
        vnet_mmio_write16(addr, queue_idx);
    } else {
        vnet_io_write16(VIRTIO_PCI_QUEUE_NOTIFY, queue_idx);
    }
}

/* Read ISR status */
static inline uint8_t vnet_read_isr(void) {
    if (vnet_is_modern) {
        return vnet_mmio_read8(vnet_isr_cfg_addr);
    }
    return vnet_io_read8(VIRTIO_PCI_ISR);
}

/* ========================================================================
 * Device-Specific Config Access (VirtIO-net: MAC, status, etc.)
 * ======================================================================== */

static uint8_t vnet_read_config_byte(uint8_t off) {
    if (vnet_is_modern) {
        return vnet_mmio_read8(vnet_device_cfg_addr + off);
    }
    return inb(vnet_io_base + VIRTIO_PCI_CONFIG_OFF + off);
}

static uint16_t vnet_read_config16(uint8_t off) {
    if (vnet_is_modern) {
        return vnet_mmio_read16(vnet_device_cfg_addr + off);
    }
    /* Legacy: read two bytes individually (IO ports are byte-granular) */
    uint16_t lo = inb(vnet_io_base + VIRTIO_PCI_CONFIG_OFF + off);
    uint16_t hi = inb(vnet_io_base + VIRTIO_PCI_CONFIG_OFF + off + 1);
    return lo | (hi << 8);
}

static uint32_t vnet_read_config32(uint8_t off) {
    if (vnet_is_modern) {
        return vnet_mmio_read32(vnet_device_cfg_addr + off);
    }
    /* Legacy: read 4 bytes individually */
    uint32_t val = 0;
    for (int i = 0; i < 4; i++) {
        val |= (uint32_t)inb(vnet_io_base + VIRTIO_PCI_CONFIG_OFF + off + i) << (i * 8);
    }
    return val;
}

/* ========================================================================
 * PCI Device Scanning
 * Scan bus 0 for VirtIO-net: vendor 0x1AF4, device 0x1000 (transitional)
 * or 0x1041 (modern-only). Also check subsystem device ID for net type.
 * ======================================================================== */

static int pci_find_virtio_net(uint8_t* bus, uint8_t* dev, uint8_t* func,
                                uintptr_t* bar0_out) {
    for (uint8_t d = 0; d < 32; d++) {
        for (uint8_t f = 0; f < 8; f++) {
            uint32_t id = pci_read32(0, d, f, 0);
            if (id == 0xFFFFFFFFu) continue;
            uint16_t vendor = id & 0xFFFF;
            uint16_t device = (id >> 16) & 0xFFFF;

            if (vendor != 0x1AF4) continue;

            /* Transitional VirtIO-net: device 0x1000, subsystem ID 1 */
            /* Modern-only VirtIO-net: device 0x1041 */
            int is_net = 0;
            if (device == 0x1000) {
                uint16_t subsystem_id = pci_read16(0, d, f, 0x2E);
                if (subsystem_id == 1) is_net = 1;
            } else if (device == 0x1041) {
                is_net = 1;
            } else if (device >= 0x1040 && device <= 0x107F) {
                /* Modern device ID range; check subsystem */
                uint16_t subsystem_id = pci_read16(0, d, f, 0x2E);
                if (subsystem_id == 1) is_net = 1;
            }

            if (!is_net) continue;

            *bus = 0; *dev = d; *func = f;

            /* Read BAR0 (legacy IO port base) */
            uint32_t b0 = pci_read32(0, d, f, 0x10);
            *bar0_out = b0;

            /* Enable the device: IOSE | MSE | BME */
            uint32_t cmd = pci_read32(0, d, f, 0x04);
            cmd |= 0x7;
            pci_write32(0, d, f, 0x04, cmd);

            pr_info("virtio_net: found at PCI 00:%u.%u, device=0x%x, BAR0=0x%x\n",
                    (unsigned)d, (unsigned)f, (unsigned)device, (unsigned)b0);
            return 1;
        }
    }
    return 0;
}

/* ========================================================================
 * Modern Transport Detection
 * Walk the PCI capability linked list looking for VirtIO vendor-specific
 * capabilities (cap ID 0x09). Parse common_cfg, notify, ISR, and
 * device_cfg capability structures.
 * ======================================================================== */

static int vnet_detect_modern(uint8_t bus, uint8_t dev, uint8_t func) {
    /* Cache all BAR base addresses */
    for (int i = 0; i < 6; i++) {
        uint32_t bar_val = pci_read32(bus, dev, func, 0x10 + i * 4);
        if (bar_val == 0 || bar_val == 0xFFFFFFFFu) {
            vnet_bars[i] = 0;
            continue;
        }
        if (bar_val & 1) {
            /* IO BAR */
            vnet_bars[i] = bar_val & ~0x3u;
        } else {
            /* MMIO BAR */
            uint8_t bar_type = (bar_val >> 1) & 3;
            if (bar_type == 0) {
                /* 32-bit MMIO */
                vnet_bars[i] = bar_val & ~0xFu;
            } else if (bar_type == 2) {
                /* 64-bit MMIO - need upper half */
                if (i + 1 < 6) {
                    uint32_t bar_hi = pci_read32(bus, dev, func, 0x10 + (i + 1) * 4);
                    vnet_bars[i] = (bar_val & ~0xFu) | ((uint64_t)bar_hi << 32);
                    vnet_bars[i + 1] = 0; /* upper half consumed */
                } else {
                    vnet_bars[i] = bar_val & ~0xFu;
                }
            } else {
                vnet_bars[i] = bar_val & ~0xFu;
            }
        }
    }

    /* Walk PCI capability list starting at offset 0x34 */
    uint8_t cap_off = pci_read8(bus, dev, func, 0x34);
    int found_caps = 0;

    while (cap_off != 0 && cap_off != 0xFF) {
        uint8_t cap_id = pci_read8(bus, dev, func, cap_off);
        if (cap_id != VIRTIO_PCI_CAP_ID) {
            /* Not a VirtIO capability, skip */
            uint8_t next = pci_read8(bus, dev, func, cap_off + 1);
            cap_off = next;
            continue;
        }

        /* Read the VirtIO PCI capability structure */
        struct virtio_pci_cap cap;
        uint32_t cap_word0 = pci_read32(bus, dev, func, cap_off);
        uint32_t cap_word1 = pci_read32(bus, dev, func, cap_off + 4);
        uint32_t cap_word2 = pci_read32(bus, dev, func, cap_off + 8);
        uint32_t cap_word3 = pci_read32(bus, dev, func, cap_off + 12);

        /* Parse the capability (little-endian layout) */
        cap.cap_vndr  = (uint8_t)(cap_word0 & 0xFF);
        cap.cap_next  = (uint8_t)((cap_word0 >> 8) & 0xFF);
        cap.cap_len   = (uint8_t)((cap_word0 >> 16) & 0xFF);
        cap.cap_type  = (uint8_t)((cap_word0 >> 24) & 0xFF);
        cap.bar       = (uint8_t)(cap_word1 & 0xFF);
        /* padding[3] in bytes 1-3 of word1 */
        cap.offset    = cap_word2;
        cap.length    = cap_word3;

        pr_info("virtio_net: modern cap type=%u, bar=%u, offset=0x%x, length=0x%x\n",
                (unsigned)cap.cap_type, (unsigned)cap.bar,
                (unsigned)cap.offset, (unsigned)cap.length);

        if (cap.bar >= 6 || vnet_bars[cap.bar] == 0) {
            pr_warn("virtio_net: modern cap references invalid BAR%u\n", (unsigned)cap.bar);
            cap_off = cap.cap_next;
            continue;
        }

        uintptr_t bar_base = vnet_bars[cap.bar];
        uintptr_t struct_addr = bar_base + cap.offset;

        switch (cap.cap_type) {
        case VIRTIO_PCI_CAP_COMMON_CFG:
            vnet_common_cfg_addr = struct_addr;
            vnet_common_cfg_bar  = cap.bar;
            found_caps++;
            break;
        case VIRTIO_PCI_CAP_NOTIFY_CFG:
            vnet_notify_cfg_addr = struct_addr;
            vnet_notify_cfg_bar  = cap.bar;
            /* Read notify_off_multiplier (additional 32-bit field) */
            vnet_notify_off_mult = pci_read32(bus, dev, func, cap_off + 16);
            found_caps++;
            break;
        case VIRTIO_PCI_CAP_ISR_CFG:
            vnet_isr_cfg_addr = struct_addr;
            vnet_isr_cfg_bar  = cap.bar;
            found_caps++;
            break;
        case VIRTIO_PCI_CAP_DEVICE_CFG:
            vnet_device_cfg_addr = struct_addr;
            vnet_device_cfg_bar  = cap.bar;
            found_caps++;
            break;
        case VIRTIO_PCI_CAP_PCI_CFG:
            /* Alternative config via PCI config space - not used here */
            break;
        }

        cap_off = cap.cap_next;
    }

    /* We need at least common_cfg and device_cfg for basic operation */
    if (vnet_common_cfg_addr != 0 && vnet_device_cfg_addr != 0) {
        pr_info("virtio_net: modern transport detected (found %u VirtIO caps)\n",
                found_caps);
        return 1;
    }

    pr_info("virtio_net: no modern transport caps found, using legacy\n");
    return 0;
}

/* ========================================================================
 * Virtqueue Layout
 * Given a base address and queue size, compute the addresses of the
 * descriptor table, available ring, and used ring.
 *
 * Legacy: all three are contiguous, used ring starts at next 4096 boundary.
 * Modern: each component is registered separately via common_cfg registers,
 * but we still lay them out in our contiguous allocation for simplicity.
 * ======================================================================== */

static void vnet_setup_vq_layout(struct virtq_desc** desc,
                                  struct virtq_avail** avail,
                                  struct virtq_used** used,
                                  uint16_t** used_event_ptr,
                                  uint16_t** avail_event_ptr,
                                  uint8_t* vq_mem, uint16_t qsz) {
    uintptr_t base = (uintptr_t)vq_mem;

    /* Descriptor table starts at offset 0 */
    *desc = (struct virtq_desc*)base;

    /* Available ring starts right after descriptor table */
    uintptr_t avail_offset = qsz * sizeof(struct virtq_desc);
    *avail = (struct virtq_avail*)(base + avail_offset);

    /* used_event is at the end of the avail ring */
    *used_event_ptr = (uint16_t*)(base + avail_offset + 2 + 2 + qsz * 2);

    /* Used ring starts at next 4096-byte boundary (legacy requirement) */
    uintptr_t used_offset = ALIGN_UP(avail_offset + VQ_AVAIL_SZ(qsz), VNET_VQ_ALIGN);
    *used = (struct virtq_used*)(base + used_offset);

    /* avail_event is at the end of the used ring */
    *avail_event_ptr = (uint16_t*)(base + used_offset + 2 + 2 + qsz * sizeof(struct virtq_used_elem));
}

/* ========================================================================
 * Virtqueue Registration
 * After setting up the memory layout, register the virtqueue with the
 * device via the appropriate transport (legacy or modern).
 * ======================================================================== */

static int vnet_register_vq(uint16_t queue_idx, uint16_t qsz,
                             uintptr_t desc_addr, uintptr_t avail_addr,
                             uintptr_t used_addr) {
    vnet_select_queue(queue_idx);

    if (vnet_is_modern) {
        /* Check max queue size */
        uint16_t max_sz = vnet_mmio_read16(vnet_common_cfg_addr + VIRTIO_COMMON_Q_SIZE);
        /* Modern: QueueNum is max, we write our desired size */
        if (qsz > max_sz) qsz = max_sz;
        vnet_mmio_write16(vnet_common_cfg_addr + VIRTIO_COMMON_Q_SIZE, qsz);

        /* Write descriptor, available, and used ring addresses */
        vnet_mmio_write32(vnet_common_cfg_addr + VIRTIO_COMMON_Q_DESC_LO,
                          (uint32_t)(desc_addr & 0xFFFFFFFF));
        vnet_mmio_write32(vnet_common_cfg_addr + VIRTIO_COMMON_Q_DESC_HI,
                          (uint32_t)(desc_addr >> 32));
        vnet_mmio_write32(vnet_common_cfg_addr + VIRTIO_COMMON_Q_AVAIL_LO,
                          (uint32_t)(avail_addr & 0xFFFFFFFF));
        vnet_mmio_write32(vnet_common_cfg_addr + VIRTIO_COMMON_Q_AVAIL_HI,
                          (uint32_t)(avail_addr >> 32));
        vnet_mmio_write32(vnet_common_cfg_addr + VIRTIO_COMMON_Q_USED_LO,
                          (uint32_t)(used_addr & 0xFFFFFFFF));
        vnet_mmio_write32(vnet_common_cfg_addr + VIRTIO_COMMON_Q_USED_HI,
                          (uint32_t)(used_addr >> 32));

        /* Enable the queue */
        vnet_mmio_write16(vnet_common_cfg_addr + VIRTIO_COMMON_Q_ENABLE, 1);

        /* Verify queue is enabled */
        if (vnet_mmio_read16(vnet_common_cfg_addr + VIRTIO_COMMON_Q_ENABLE) != 1) {
            pr_err("virtio_net: failed to enable queue %u (modern)\n", (unsigned)queue_idx);
            return 0;
        }
    } else {
        /* Legacy: write page frame number of the virtqueue allocation */
        /* The PFN is the physical page number (address / 4096) */
        /* Since LestraOS uses identity mapping, virt addr = phys addr */
        uintptr_t vq_phys = (uintptr_t)vnet_rx_vq_mem; /* base of the contiguous allocation */
        /* Actually, the PFN should be the start of the page containing the
         * virtqueue data. Since our allocation is page-aligned, this is
         * simply vq_phys / 4096. */
        uint32_t pfn = (uint32_t)(desc_addr / VNET_VQ_ALIGN);
        vnet_io_write32(VIRTIO_PCI_QUEUE_PFN, pfn);
    }

    return 1;
}

/* ========================================================================
 * Public Driver API (same contract as e1000.c for net.c integration)
 * ======================================================================== */

int virtio_net_init(void);
int virtio_net_is_present(void);
mac_addr_t virtio_net_get_mac(void);
int virtio_net_send(const void* data, uint16_t len);
int virtio_net_recv(void* buf, uint16_t bufsz);

/* ========================================================================
 * Initialization
 * ======================================================================== */

int virtio_net_init(void) {
    uint8_t bus, dev, func;
    uintptr_t bar0_raw;

    /* Step 1: Find VirtIO-net PCI device */
    if (!pci_find_virtio_net(&bus, &dev, &func, &bar0_raw)) {
        pr_info("virtio_net: no VirtIO-net device found\n");
        return 0;
    }

    vnet_pci_bus  = bus;
    vnet_pci_dev  = dev;
    vnet_pci_func = func;

    /* Step 2: Detect transport mode (modern vs legacy) */
    if (vnet_detect_modern(bus, dev, func)) {
        vnet_is_modern = 1;
    } else {
        vnet_is_modern = 0;
        /* Legacy: extract IO port base from BAR0 */
        /* BAR0 bottom bit is 1 for IO space */
        vnet_io_base = (uint16_t)(bar0_raw & ~0x3u);
        pr_info("virtio_net: legacy transport, IO base=0x%x\n", (unsigned)vnet_io_base);
    }

    /* Step 3: Reset device */
    vnet_write_status(0);
    /* Wait for reset to take effect (read status back, should be 0) */
    for (int i = 0; i < 1000; i++) {
        if (vnet_read_status() == 0) break;
    }
    if (vnet_read_status() != 0) {
        pr_err("virtio_net: device reset failed (status=0x%x)\n", (unsigned)vnet_read_status());
        return 0;
    }

    /* Step 4: Acknowledge device */
    vnet_write_status(VIRTIO_CONFIG_S_ACKNOWLEDGE);

    /* Step 5: Read device (host) features */
    vnet_host_features = vnet_read_host_features();
    pr_info("virtio_net: host features=0x%x\n", (unsigned)vnet_host_features);

    /* Step 6: Negotiate features - accept only what we support */
    vnet_guest_features = vnet_host_features & VIRTIO_NET_DRIVER_FEATURES;
    vnet_write_guest_features(vnet_guest_features);
    pr_info("virtio_net: negotiated features=0x%x\n", (unsigned)vnet_guest_features);

    /* Step 7: Indicate driver is ready */
    vnet_write_status(VIRTIO_CONFIG_S_DRIVER);

    /* For modern transport: validate FEATURES_OK */
    if (vnet_is_modern) {
        vnet_write_status(vnet_read_status() | VIRTIO_CONFIG_S_FEATURES_OK);
        uint8_t status = vnet_read_status();
        if (!(status & VIRTIO_CONFIG_S_FEATURES_OK)) {
            pr_err("virtio_net: FEATURES_OK not set, feature negotiation failed\n");
            vnet_write_status(VIRTIO_CONFIG_S_FAILED);
            return 0;
        }
    }

    /* Step 8: Read MAC address from device config */
    if (vnet_guest_features & BIT(VIRTIO_NET_F_MAC)) {
        for (int i = 0; i < 6; i++) {
            vnet_mac.bytes[i] = vnet_read_config_byte(i);
        }
        pr_info("virtio_net: MAC from device: %x:%x:%x:%x:%x:%x\n",
                vnet_mac.bytes[0], vnet_mac.bytes[1], vnet_mac.bytes[2],
                vnet_mac.bytes[3], vnet_mac.bytes[4], vnet_mac.bytes[5]);
    } else {
        /* Device doesn't provide MAC; generate a random local one */
        pr_warn("virtio_net: device does not provide MAC, generating local MAC\n");
        vnet_mac.bytes[0] = 0x02;  /* locally administered */
        vnet_mac.bytes[1] = 0x00;
        vnet_mac.bytes[2] = 0x00;
        /* Use TSC for randomness */
        uint64_t tsc = rdtsc();
        vnet_mac.bytes[3] = (uint8_t)(tsc >> 16);
        vnet_mac.bytes[4] = (uint8_t)(tsc >> 8);
        vnet_mac.bytes[5] = (uint8_t)(tsc);
    }

    /* Step 9: Set up RX virtqueue (queue 0) */
    vnet_select_queue(0);
    vnet_rx_qsz = vnet_read_queue_size();
    if (vnet_rx_qsz == 0 || vnet_rx_qsz > VNET_QUEUE_SIZE) {
        pr_warn("virtio_net: RX queue size %u, using %u\n",
                (unsigned)vnet_rx_qsz, (unsigned)VNET_QUEUE_SIZE);
        vnet_rx_qsz = VNET_QUEUE_SIZE;
    }
    /* For legacy, vnet_rx_qsz is the max; we can use it directly.
     * For modern, we need to write our chosen size back. */
    if (vnet_is_modern) {
        vnet_write_queue_size(vnet_rx_qsz);
    }

    /* Layout the RX virtqueue structures */
    vnet_setup_vq_layout(&vnet_rx_desc, &vnet_rx_avail, &vnet_rx_used,
                          &vnet_rx_used_event, &vnet_rx_avail_event,
                          vnet_rx_vq_mem, vnet_rx_qsz);

    /* Initialize RX descriptor table and available ring */
    memset(vnet_rx_vq_mem, 0, sizeof(vnet_rx_vq_mem));

    /* Fill all RX descriptors with empty buffers and add to available ring */
    for (uint16_t i = 0; i < vnet_rx_qsz; i++) {
        vnet_rx_desc[i].addr  = (uint64_t)(uintptr_t)vnet_rx_bufs[i];
        vnet_rx_desc[i].len   = VIRTIO_NET_HDR_SIZE + VNET_RX_BUF_SZ;
        vnet_rx_desc[i].flags = VIRTQ_DESC_F_WRITE;  /* device writes into buffer */
        vnet_rx_desc[i].next  = 0;
        vnet_rx_avail->ring[i] = i;
    }
    vnet_rx_avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;
    vnet_rx_avail->idx   = vnet_rx_qsz;  /* all descriptors are available */
    vnet_rx_avail_idx    = vnet_rx_qsz;   /* track driver's next avail index */
    vnet_rx_last_used    = 0;

    /* Register RX virtqueue with device */
    uintptr_t rx_desc_addr  = (uintptr_t)vnet_rx_desc;
    uintptr_t rx_avail_addr = (uintptr_t)vnet_rx_avail;
    uintptr_t rx_used_addr  = (uintptr_t)vnet_rx_used;

    if (!vnet_register_vq(0, vnet_rx_qsz, rx_desc_addr, rx_avail_addr, rx_used_addr)) {
        pr_err("virtio_net: failed to register RX queue\n");
        vnet_write_status(VIRTIO_CONFIG_S_FAILED);
        return 0;
    }

    /* Step 10: Set up TX virtqueue (queue 1) */
    vnet_select_queue(1);
    vnet_tx_qsz = vnet_read_queue_size();
    if (vnet_tx_qsz == 0 || vnet_tx_qsz > VNET_QUEUE_SIZE) {
        pr_warn("virtio_net: TX queue size %u, using %u\n",
                (unsigned)vnet_tx_qsz, (unsigned)VNET_QUEUE_SIZE);
        vnet_tx_qsz = VNET_QUEUE_SIZE;
    }
    if (vnet_is_modern) {
        vnet_write_queue_size(vnet_tx_qsz);
    }

    /* Layout the TX virtqueue structures */
    vnet_setup_vq_layout(&vnet_tx_desc, &vnet_tx_avail, &vnet_tx_used,
                          &vnet_tx_used_event, &vnet_tx_avail_event,
                          vnet_tx_vq_mem, vnet_tx_qsz);

    /* Initialize TX virtqueue */
    memset(vnet_tx_vq_mem, 0, sizeof(vnet_tx_vq_mem));

    vnet_tx_avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;
    vnet_tx_avail->idx   = 0;  /* no descriptors available initially */
    vnet_tx_avail_idx    = 0;
    vnet_tx_last_used    = 0;
    vnet_tx_next_desc    = 0;

    /* TX descriptors don't need pre-population; we fill them on send */
    for (uint16_t i = 0; i < vnet_tx_qsz; i++) {
        vnet_tx_desc[i].addr  = (uint64_t)(uintptr_t)vnet_tx_bufs[i];
        vnet_tx_desc[i].len   = 0;
        vnet_tx_desc[i].flags = 0;  /* device reads from buffer */
        vnet_tx_desc[i].next  = 0;
    }

    /* Register TX virtqueue with device */
    uintptr_t tx_desc_addr  = (uintptr_t)vnet_tx_desc;
    uintptr_t tx_avail_addr = (uintptr_t)vnet_tx_avail;
    uintptr_t tx_used_addr  = (uintptr_t)vnet_tx_used;

    if (!vnet_register_vq(1, vnet_tx_qsz, tx_desc_addr, tx_avail_addr, tx_used_addr)) {
        pr_err("virtio_net: failed to register TX queue\n");
        vnet_write_status(VIRTIO_CONFIG_S_FAILED);
        return 0;
    }

    /* Step 11: Signal DRIVER_OK - device can now operate */
    vnet_write_status(vnet_read_status() | VIRTIO_CONFIG_S_DRIVER_OK);

    /* Step 12: Notify device that RX buffers are available */
    barrier();
    vnet_notify_queue(0);

    /* Check link status if VIRTIO_NET_F_STATUS is negotiated */
    if (vnet_guest_features & BIT(VIRTIO_NET_F_STATUS)) {
        uint16_t status = vnet_read_config16(6);  /* status field at offset 6 in config */
        pr_info("virtio_net: link status=0x%x (%s)\n",
                (unsigned)status, (status & 1) ? "up" : "down");
    } else {
        pr_info("virtio_net: link status feature not available, assuming up\n");
    }

    vnet_present = 1;
    pr_info("virtio_net: initialized successfully (mode=%s, RX/TX qsz=%u/%u)\n",
            vnet_is_modern ? "modern" : "legacy",
            (unsigned)vnet_rx_qsz, (unsigned)vnet_tx_qsz);
    return 1;
}

/* ========================================================================
 * Query Functions
 * ======================================================================== */

int virtio_net_is_present(void) {
    return vnet_present;
}

mac_addr_t virtio_net_get_mac(void) {
    return vnet_mac;
}

/* ========================================================================
 * Transmit
 * Send a raw Ethernet frame (no VirtIO header in the caller's data).
 * We prepend a zeroed virtio_net_hdr internally.
 *
 * Returns len on success, 0 on failure/timeout.
 * ======================================================================== */

int virtio_net_send(const void* data, uint16_t len) {
    if (!vnet_present) return 0;
    if (len > VNET_TX_BUF_SZ) len = VNET_TX_BUF_SZ;

    /* Pick the next descriptor index (rotating) */
    uint16_t desc_idx = vnet_tx_next_desc;

    /* Prepare the buffer: virtio_net_hdr (zeroed) + packet data */
    memset(vnet_tx_bufs[desc_idx], 0, VIRTIO_NET_HDR_SIZE);
    memcpy(vnet_tx_bufs[desc_idx] + VIRTIO_NET_HDR_SIZE, data, len);

    /* Fill in the descriptor */
    vnet_tx_desc[desc_idx].addr  = (uint64_t)(uintptr_t)vnet_tx_bufs[desc_idx];
    vnet_tx_desc[desc_idx].len   = VIRTIO_NET_HDR_SIZE + len;
    vnet_tx_desc[desc_idx].flags = 0;  /* read-only for device */
    vnet_tx_desc[desc_idx].next  = 0;

    /* Memory barrier: ensure descriptor + buffer writes are visible
     * before we update the available ring and ring the doorbell. */
    barrier();

    /* Add descriptor to available ring */
    uint16_t avail_slot = vnet_tx_avail_idx % vnet_tx_qsz;
    vnet_tx_avail->ring[avail_slot] = desc_idx;
    vnet_tx_avail_idx++;
    barrier();
    vnet_tx_avail->idx = vnet_tx_avail_idx;

    /* Ring the doorbell */
    barrier();
    vnet_notify_queue(1);  /* queue 1 = TX */

    /* Wait for the device to process the descriptor (polling) */
    int timed_out = 1;
    for (int i = 0; i < 10000000; i++) {
        if (vnet_tx_used->idx != vnet_tx_last_used) {
            timed_out = 0;
            break;
        }
    }

    if (timed_out) {
        pr_warn("virtio_net: TX timeout (desc %u, len=%u, used.idx=%u, last_used=%u)\n",
                (unsigned)desc_idx, (unsigned)len,
                (unsigned)vnet_tx_used->idx, (unsigned)vnet_tx_last_used);
        /* Don't advance the descriptor index on timeout to avoid corruption */
        return 0;
    }

    /* Process the used ring entry */
    uint16_t used_slot = vnet_tx_last_used % vnet_tx_qsz;
    struct virtq_used_elem* ue = &vnet_tx_used->ring[used_slot];
    /* Verify the used entry matches our descriptor (sanity check) */
    if (ue->id != desc_idx) {
        pr_warn("virtio_net: TX used entry mismatch (expected %u, got %u)\n",
                (unsigned)desc_idx, (unsigned)ue->id);
    }
    vnet_tx_last_used++;

    /* Advance the rotating descriptor index */
    vnet_tx_next_desc = (vnet_tx_next_desc + 1) % vnet_tx_qsz;

    return len;
}

/* ========================================================================
 * Receive
 * Poll the RX virtqueue for completed packets. Strip the virtio_net_hdr
 * and return just the Ethernet frame data to the caller.
 *
 * Returns packet length on success, 0 if no packet available.
 * ======================================================================== */

int virtio_net_recv(void* buf, uint16_t bufsz) {
    if (!vnet_present) return 0;

    /* Check if the device has placed any completed descriptors in the used ring */
    if (vnet_rx_used->idx == vnet_rx_last_used) {
        return 0;  /* no new packets */
    }

    /* Process the next used ring entry */
    uint16_t used_slot = vnet_rx_last_used % vnet_rx_qsz;
    struct virtq_used_elem* ue = &vnet_rx_used->ring[used_slot];
    uint16_t desc_idx = (uint16_t)ue->id;

    /* The device wrote data into vnet_rx_bufs[desc_idx]:
     *   first VIRTIO_NET_HDR_SIZE bytes = virtio_net_hdr
     *   remaining bytes = actual packet data
     * ue->len = total bytes written (including the header). */
    uint32_t total_len = ue->len;
    if (total_len < VIRTIO_NET_HDR_SIZE) {
        pr_warn("virtio_net: RX packet too short (%u bytes)\n", (unsigned)total_len);
        /* Still need to recycle the descriptor */
    }

    /* Packet data starts after the VirtIO header */
    uint32_t pkt_len = total_len - VIRTIO_NET_HDR_SIZE;
    if (pkt_len > bufsz) pkt_len = bufsz;

    /* Copy packet data (excluding the VirtIO header) to caller's buffer */
    if (pkt_len > 0) {
        memcpy(buf, vnet_rx_bufs[desc_idx] + VIRTIO_NET_HDR_SIZE, pkt_len);
    }

    /* Advance our tracking of the used ring */
    vnet_rx_last_used++;

    /* Recycle the descriptor: re-add it to the available ring */
    vnet_rx_desc[desc_idx].addr  = (uint64_t)(uintptr_t)vnet_rx_bufs[desc_idx];
    vnet_rx_desc[desc_idx].len   = VIRTIO_NET_HDR_SIZE + VNET_RX_BUF_SZ;
    vnet_rx_desc[desc_idx].flags = VIRTQ_DESC_F_WRITE;
    vnet_rx_desc[desc_idx].next  = 0;

    uint16_t avail_slot = vnet_rx_avail_idx % vnet_rx_qsz;
    vnet_rx_avail->ring[avail_slot] = desc_idx;
    vnet_rx_avail_idx++;
    barrier();
    vnet_rx_avail->idx = vnet_rx_avail_idx;

    /* Notify the device about the recycled buffer */
    barrier();
    vnet_notify_queue(0);  /* queue 0 = RX */

    return (int)pkt_len;
}
