/*
 * Lestra OS - Realtek RTL8168/8111 (and RTL8169) Gigabit Ethernet Driver
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * The RTL8168/8111 is the single most common wired NIC on consumer
 * desktop/mainboard hardware (PCIe GbE Family Controller). This driver
 * covers that family, which shares one register interface with the
 * older RTL8169. Unlike the RTL8139 (linear legacy RX buffer), these
 * chips are descriptor-based ("C+ mode"): TX and RX traffic flows
 * through 16-byte DMA descriptor rings.
 *
 * Polling-based (no interrupts) - call rtl8168_poll_rx() from
 * net_tick() to drain the RX ring. TPPoll pokes the TX engine.
 *
 * References:
 *   - Realtek RTL8111B/RTL8168B Registers Datasheet v1.0
 *   - FreeBSD if_re.c (BAR mapping for 8168/8101E = memory BAR2)
 *   - iPXE realtek.c (descriptor ring + register protocol)
 *   - OSDev wiki: https://wiki.osdev.org/RTL8169
 */

#include <lestra/types.h>
#include <lestra/net.h>
#include <lestra/nic.h>
#include <lestra/printk.h>
#include <lestra/pci.h>
#include <string.h>

/* ========================================================================
 * Register offsets (relative to the MMIO register window)
 * ======================================================================== */

#define RTL_IDR0        0x00    /* ID Register 0 (MAC byte 0) */
#define RTL_MAR0        0x08    /* Multicast Address Register 0 */
#define RTL_MAR4        0x0C    /* Multicast Address Register 4 */

#define RTL_TNPDS       0x20    /* TX Normal-Priority Desc Start (64-bit) */
#define RTL_CR          0x37    /* Command Register */
#define RTL_TPPOLL      0x38    /* TX Priority Polling (NPQ bit) */
#define RTL_IMR         0x3C    /* Interrupt Mask (word) */
#define RTL_ISR         0x3E    /* Interrupt Status (word) */
#define RTL_TCR         0x40    /* Transmit Configuration */
#define RTL_RCR         0x44    /* Receive Configuration */

#define RTL_PHYAR       0x60    /* PHY (MII) Access Register */
#define RTL_PHYSTATUS   0x6C    /* PHY Status Register */
#define RTL_RMS         0xDA    /* RX Packet Maximum Size (word) */
#define RTL_CPCR        0xE0    /* C+ Command Register (word) */
#define RTL_RDSAR       0xE4    /* RX Descriptor Start Address (64-bit) */

/* CR bits */
#define CR_RST          0x10
#define CR_RE           0x08    /* Receiver Enable */
#define CR_TE           0x04    /* Transmitter Enable */

/* TPPoll bits */
#define TPPOLL_NPQ      0x40

/* TCR bits */
#define TCR_MXDMA_MASK  0x0700
#define TCR_MXDMA_UNLTD 0x0700  /* Max DMA burst = unlimited */

/* RCR bits (per RTL8111B/8168B datasheet, offset 0x44) */
#define RCR_STOP_WORK   0x01000000UL
#define RCR_RXFTH(x)    ((x) << 13)   /* RX FIFO threshold */
#define RCR_RBLEN(x)    ((x) << 11)   /* RX buffer length */
#define RCR_MXDMA(x)    ((x) << 8)    /* RX DMA burst */
#define RCR_WRAP        0x80
#define RCR_AB          0x08    /* Accept Broadcast */
#define RCR_AM          0x04    /* Accept Multicast */
#define RCR_APM         0x02    /* Accept Physical Match */
#define RCR_AAP         0x01    /* Accept All Packets */

#define RCR_DEFAULT (RCR_RXFTH(0x7) | RCR_RBLEN(0) | RCR_MXDMA(0x7) | \
                     RCR_WRAP | RCR_AB | RCR_AM | RCR_APM | RCR_AAP)

/* C+ Command Register bits */
#define CPCR_VLAN       0x40
#define CPCR_DAC        0x10    /* PCI Dual Address Cycle (64-bit DMA) */
#define CPCR_MULRW      0x08
#define CPCR_CPRX       0x02    /* C+ Receive Engine Enable */
#define CPCR_CPTX       0x01    /* C+ Transmit Engine Enable */

/* PHY status bits */
#define PHYSTATUS_LINK  0x02

/* ========================================================================
 * Descriptor format (16 bytes, same layout for RX and TX)
 *
 * dword 0: length[15:0] | flags[31:16]
 * dword 1: VLAN tag / reserved
 * dword 2: buffer physical address low
 * dword 3: buffer physical address high
 * ======================================================================== */

struct rtl_desc {
    uint16_t length;            /* buffer size (RX arm) / frame len (done) */
    uint16_t flags;
    uint32_t vlan;
    uint64_t addr;
} __packed;

/* Descriptor flag bits */
#define DESC_OWN    0x8000      /* NIC owns this descriptor */
#define DESC_EOR    0x4000      /* End of Ring */
#define DESC_FS     0x2000      /* First Segment of frame */
#define DESC_LS     0x1000      /* Last Segment of frame */
#define DESC_RES    0x0020      /* Receive error summary */

#define DESC_LEN_MASK 0x3FFF    /* RX length field is 14 bits */

/* ========================================================================
 * Configuration
 * ======================================================================== */

#define NUM_RX  16
#define NUM_TX  8
#define RX_BUF_SZ  2048
#define TX_BUF_SZ  2048

/* ========================================================================
 * State - static storage (BSS) because the NIC DMA-reads the descriptor
 * rings and buffers directly from physical RAM (same rule as RTL8139:
 * kmalloc() returns heap addresses outside guest physical RAM).
 * ======================================================================== */

static int       rtl_present = 0;
static uintptr_t rtl_mmio = 0;
static mac_addr_t rtl_mac = MAC_ZERO;

static struct rtl_desc rx_ring[NUM_RX]   __aligned(256);
static struct rtl_desc tx_ring[NUM_TX]   __aligned(256);
static uint8_t rx_bufs[NUM_RX][RX_BUF_SZ] __aligned(16);
static uint8_t tx_bufs[NUM_TX][TX_BUF_SZ] __aligned(16);
static uint32_t rx_head = 0;   /* next descriptor to drain */
static uint32_t tx_head = 0;   /* next descriptor to submit */

/* ----- MMIO access helpers ----- */
static inline uint8_t  rtl_read8(uint32_t off)  { return *(volatile uint8_t*)(rtl_mmio + off); }
static inline uint16_t rtl_read16(uint32_t off) { return *(volatile uint16_t*)(rtl_mmio + off); }
static inline uint32_t rtl_read32(uint32_t off) { return *(volatile uint32_t*)(rtl_mmio + off); }
static inline void rtl_write8(uint32_t off, uint8_t v)  { *(volatile uint8_t*)(rtl_mmio + off) = v; }
static inline void rtl_write16(uint32_t off, uint16_t v){ *(volatile uint16_t*)(rtl_mmio + off) = v; }
static inline void rtl_write32(uint32_t off, uint32_t v){ *(volatile uint32_t*)(rtl_mmio + off) = v; }

/* ========================================================================
 * Device detection
 * ======================================================================== */

/* Vendor 0x10EC. FreeBSD re(4) covers this exact set; the RTL8168/8111
 * register interface is identical to the RTL8169. */
static const uint16_t rtl_device_ids[] = {
    0x8168,   /* RTL8111/8168B (PCIe GbE Family Controller) */
    0x8161,   /* RTL8168                       */
    0x8162,   /* RTL8111                       */
    0x8167,   /* RTL-8110SC/8169SC             */
    0x8169,   /* RTL-8169                      */
    0,
};

static struct pci_device *rtl8168_find_pci(void) {
    for (int i = 0; rtl_device_ids[i]; i++) {
        struct pci_device *dev = pci_find_device(0x10EC, rtl_device_ids[i]);
        if (dev) return dev;
    }
    return NULL;
}

/*
 * Find the register window. FreeBSD if_re.c: prefer memory mapping;
 * RTL8168/8101E use memory BAR2, the rest of the family use BAR1, and
 * BAR0 (I/O ports) is the fallback. Our pci table recorded the raw BAR
 * values at scan time, so we pick the memory window by type bit.
 */
static uintptr_t rtl8168_reg_base(struct pci_device *dev) {
    /* 8168/8169 family: try memory BAR2, then BAR1, then BAR0 */
    static const int try_order[] = { 2, 1, 0 };
    for (int i = 0; i < 3; i++) {
        int idx = try_order[i];
        uint32_t raw = dev->bar[idx];
        if (!raw) continue;
        if ((raw & 0x1) == 0) {          /* memory BAR */
            return (uintptr_t)(raw & ~0xF);
        }
    }
    return 0;
}

/* ========================================================================
 * Reset
 * ======================================================================== */

static int rtl8168_reset(void) {
    rtl_write8(RTL_CR, CR_RST);
    for (int i = 0; i < 100000; i++) {
        if (!(rtl_read8(RTL_CR) & CR_RST)) return 0;
    }
    return -1;
}

/* ========================================================================
 * Init
 * ======================================================================== */

static void rtl8168_arm_rx_desc(int idx) {
    struct rtl_desc *d = &rx_ring[idx];
    int is_last = (idx == NUM_RX - 1);
    d->addr = (uint64_t)(uintptr_t)rx_bufs[idx];
    d->vlan = 0;
    /* length must be written before OWN so the card sees a valid buffer */
    d->length = RX_BUF_SZ;
    barrier();
    d->flags = DESC_OWN | (is_last ? DESC_EOR : 0);
    barrier();
}

int rtl8168_init(void) {
    struct pci_device *dev = rtl8168_find_pci();
    if (!dev) return 0;

    rtl_mmio = rtl8168_reg_base(dev);
    if (!rtl_mmio) {
        pr_warn("rtl8168: no usable register BAR\n");
        return 0;
    }

    pci_device_enable(dev);
    pr_info("rtl8168: found %04x:%04x at PCI %02x:%02x.%x, MMIO=0x%x\n",
            (unsigned)dev->vendor_id, (unsigned)dev->device_id,
            dev->bus, dev->dev, dev->func, (unsigned)rtl_mmio);

    if (rtl8168_reset() < 0) {
        pr_warn("rtl8168: reset timed out\n");
        return 0;
    }

    /* Read MAC from the ID registers (chip autoloads from EEPROM) */
    for (int i = 0; i < 6; i++)
        rtl_mac.bytes[i] = rtl_read8(RTL_IDR0 + i);
    pr_info("rtl8168: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            rtl_mac.bytes[0], rtl_mac.bytes[1], rtl_mac.bytes[2],
            rtl_mac.bytes[3], rtl_mac.bytes[4], rtl_mac.bytes[5]);

    /* Disable interrupts (we poll) */
    rtl_write16(RTL_IMR, 0);

    /* Ring start addresses: low dword first, then high — the RTL8168
     * latches the 64-bit address when the high dword is written. */
    rtl_write32(RTL_TNPDS,     (uint32_t)(uintptr_t)tx_ring);
    rtl_write32(RTL_TNPDS + 4, (uint32_t)((uint64_t)(uintptr_t)tx_ring >> 32));
    rtl_write32(RTL_RDSAR,     (uint32_t)(uintptr_t)rx_ring);
    rtl_write32(RTL_RDSAR + 4, (uint32_t)((uint64_t)(uintptr_t)rx_ring >> 32));

    /* Enable C+ descriptor mode (disable VLAN handling, keep DAC off:
     * our buffers live in the low 4 GB so 32-bit addressing suffices,
     * and DAC is known to misbehave on some 8168 revisions). */
    uint16_t cpcr = rtl_read16(RTL_CPCR);
    cpcr &= ~(CPCR_VLAN | CPCR_DAC);
    cpcr |= CPCR_MULRW | CPCR_CPRX | CPCR_CPTX;
    rtl_write16(RTL_CPCR, cpcr);

    /* RX maximum packet size: any frame larger than this is dropped.
     * Linux/u-boot rtl8169 set 0x800 (2048) to match their 2KB RX
     * buffers - we do the same (RX_BUF_SZ). Max Ethernet frame is
     * 1522 (incl. VLAN), well inside this. */
    rtl_write16(RTL_RMS, RX_BUF_SZ);

    /* Accept all packets (multicast table all 1s) */
    rtl_write32(RTL_MAR0, 0xFFFFFFFF);
    rtl_write32(RTL_MAR4, 0xFFFFFFFF);

    /* Enable RX/TX engines */
    rtl_write8(RTL_CR, CR_RE | CR_TE);

    /* TX config: unlimited DMA burst, keep any revision bits intact */
    uint32_t tcr = rtl_read32(RTL_TCR);
    tcr &= ~TCR_MXDMA_MASK;
    tcr |= TCR_MXDMA_UNLTD;
    rtl_write32(RTL_TCR, tcr);

    /* RX config: FIFO whole-packet threshold, unlimited DMA, accept all */
    uint32_t rcr = rtl_read32(RTL_RCR);
    rcr &= ~(RCR_STOP_WORK | RCR_RXFTH(0x7) | RCR_RBLEN(0x3) |
             RCR_MXDMA(0x7) | 0x80 | 0x0F);
    rcr |= RCR_DEFAULT;
    rtl_write32(RTL_RCR, rcr);

    /* Arm the entire RX ring before traffic flows */
    for (int i = 0; i < NUM_RX; i++)
        rtl8168_arm_rx_desc(i);
    rx_head = 0;
    tx_head = 0;

    rtl_present = 1;

    uint8_t physt = rtl_read8(RTL_PHYSTATUS);
    pr_info("rtl8168: initialized (RX ring %u, TX ring %u, link %s, PHYSTAT=0x%02x)\n",
            (unsigned)NUM_RX, (unsigned)NUM_TX,
            (physt & PHYSTATUS_LINK) ? "up" : "down",
            (unsigned)physt);
    return 1;
}

int rtl8168_is_present(void) { return rtl_present; }

mac_addr_t rtl8168_get_mac(void) { return rtl_mac; }

/* ========================================================================
 * TX
 * ======================================================================== */

int rtl8168_send(const void* data, uint16_t len) {
    if (!rtl_present) return -1;
    if (len > TX_BUF_SZ) return -1;

    if (tx_ring[tx_head].flags & DESC_OWN) {
        pr_warn("rtl8168: TX ring full (desc %u)\n", (unsigned)tx_head);
        return -1;
    }

    uint32_t idx = tx_head;
    memcpy(tx_bufs[idx], data, len);

    struct rtl_desc *d = &tx_ring[idx];
    int is_last = (idx == NUM_TX - 1);
    d->addr = (uint64_t)(uintptr_t)tx_bufs[idx];
    d->vlan = 0;
    d->length = len;
    barrier();
    /* OWN|FS|LS must be the last thing written so the card never sees
     * a partially-prepared descriptor. */
    d->flags = DESC_OWN | DESC_FS | DESC_LS | (is_last ? DESC_EOR : 0);
    barrier();

    /* Poke the TX engine */
    rtl_write8(RTL_TPPOLL, TPPOLL_NPQ);

    tx_head = (tx_head + 1) % NUM_TX;

    /* Wait for the descriptor to be consumed (OWN cleared by hardware) */
    int timed_out = 1;
    for (int i = 0; i < 1000000; i++) {
        if (!(d->flags & DESC_OWN)) { timed_out = 0; break; }
    }
    if (timed_out) {
        pr_warn("rtl8168: TX timeout (desc %u, len=%u)\n", (unsigned)idx, (unsigned)len);
        return -1;
    }
    return len;
}

/* ========================================================================
 * RX
 * ======================================================================== */

int rtl8168_recv(void* buf, uint16_t bufsz) {
    if (!rtl_present) return 0;

    struct rtl_desc *d = &rx_ring[rx_head];
    if (d->flags & DESC_OWN)
        return 0;                              /* card still owns it */

    uint16_t pkt_len = d->length & DESC_LEN_MASK;
    if (d->flags & DESC_RES) {
        pr_warn("rtl8168: RX error on desc %u (flags=0x%04x)\n",
                (unsigned)rx_head, (unsigned)d->flags);
        rtl8168_arm_rx_desc(rx_head);          /* re-arm and drop */
        rx_head = (rx_head + 1) % NUM_RX;
        return 0;
    }

    /* Descriptor length includes the 4-byte FCS; strip it */
    if (pkt_len >= 4) pkt_len -= 4;
    if (pkt_len > bufsz) pkt_len = bufsz;

    memcpy(buf, rx_bufs[rx_head], pkt_len);

    rtl8168_arm_rx_desc(rx_head);
    rx_head = (rx_head + 1) % NUM_RX;
    return pkt_len;
}

/* Polling driver: the RX loop in net_tick() calls recv() until empty,
 * so a flush hook is a no-op (ring stays armed continuously). */
static void rtl8168_flush(void) { }

/* ========================================================================
 * MAC control
 * ======================================================================== */

static int rtl8168_set_mac(mac_addr_t mac) {
    if (!rtl_present) return -1;
    rtl_mac = mac;
    /* Individual address registers are read-only after EEPROM autoload
     * on most 8168 revisions; runtime MAC change needs C+ mode patterns
     * we don't support. Update the software copy only (like RTL8139). */
    return 0;
}

/* ========================================================================
 * KE-14: NIC driver vtable
 * ======================================================================== */

static int rtl8168_nic_init(void)     { return rtl8168_init(); }
static int rtl8168_nic_send(const void *data, uint16_t len) { return rtl8168_send(data, len); }
static int rtl8168_nic_recv(void *buf, uint16_t bufsz)     { return rtl8168_recv(buf, bufsz); }
static mac_addr_t rtl8168_nic_get_mac(void) { return rtl8168_get_mac(); }
static void rtl8168_nic_flush(void)   { rtl8168_flush(); }
static int rtl8168_nic_set_mac(mac_addr_t mac) { return rtl8168_set_mac(mac); }

const struct nic_ops rtl8168_ops = {
    .name    = "rtl8168",
    .init    = rtl8168_nic_init,
    .send    = rtl8168_nic_send,
    .recv    = rtl8168_nic_recv,
    .get_mac = rtl8168_nic_get_mac,
    .set_mac = rtl8168_nic_set_mac,
    .flush   = rtl8168_nic_flush,
};