/*
 * Lestra OS - Realtek RTL8139 10/100 Fast Ethernet Driver
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * First real-hardware (non-virtio) NIC driver for lestraOS.
 * Uses the shared PCI API from KE-9 (pci_find_device).
 * IO-port mapped (BAR0 = IO base), unlike E1000 which is MMIO.
 *
 * TX: 4-entry ring using TSAD (address) + TSD (status/command) registers.
 * RX: Linear ring buffer at RBSTART, wrap-around with split-copy handling.
 * Polling-based (no interrupts) — call rtl8139_recv() from net_tick().
 *
 * References:
 *   - Realtek RTL8139 Programming Guide (8139C+)
 *   - OSdev wiki: https://wiki.osdev.org/RTL8139
 *   - QEMU source: hw/net/rtl8139.c
 */

#include <lestra/types.h>
#include <lestra/net.h>
#include <lestra/printk.h>
#include <lestra/pci.h>
#include <lestra/mm.h>
#include <string.h>

/* ========================================================================
 * RTL8139 Register Offsets (from IO base)
 * ======================================================================== */

/* ID registers (read MAC) */
#define RTL_IDR0        0x00
#define RTL_IDR1        0x01
#define RTL_IDR2        0x02
#define RTL_IDR3        0x03
#define RTL_IDR4        0x04
#define RTL_IDR5        0x05

/* General */
#define RTL_CR          0x37    /* Command Register */
#define RTL_TCR         0x40    /* Transmit Configuration */
#define RTL_RCR         0x44    /* Receive Configuration */
#define RTL_TSR         0x58    /* Transmit Status */
#define RTL_RCSR        0x5C    /* Receive Count Status */
#define RTL_CMD         0x37    /* Command (alias for CR) */

/* TX descriptor registers (4 entries) */
#define RTL_TSAD0       0x20    /* TX Start Address Descriptor 0 */
#define RTL_TSAD1       0x24
#define RTL_TSAD2       0x28
#define RTL_TSAD3       0x2C
#define RTL_TSD0        0x10    /* TX Status Descriptor 0 */
#define RTL_TSD1        0x14
#define RTL_TSD2        0x18
#define RTL_TSD3        0x1C

/* RX buffer */
#define RTL_RBSTART     0x30    /* Receive Buffer Start Address (physical) */
#define RTL_CBA        0x3C    /* Current Buffer Address */
#define RTL_CAPR        0x38    /* Current Address of Packet Read */

/* Interrupts */
#define RTL_IMR         0x3C    /* Interrupt Mask */
#define RTL_ISR         0x3E    /* Interrupt Status */

/* MII / config */
#define RTL_CONFIG1     0x52
#define RTL_CONFIG2     0x53
#define RTL_CONFIG3     0x54
#define RTL_CONFIG4     0x55
#define RTL_CONFIG5     0x56

/* CR bits */
#define CR_RST          0x10    /* Software Reset */
#define CR_RX_ENABLE    0x08
#define CR_TX_ENABLE    0x04
#define CR_BUF_SIZE_64K 0x00

/* TSD bits */
#define TSD_OWN        0x20000000
#define TSD_TXOK       0x8000

/* RCR bits */
#define RCR_AB          0x08    /* Accept Broadcast */
#define RCR_AM          0x04    /* Accept Multicast */
#define RCR_APM         0x02    /* Accept Physical Match */
#define RCR_AAP         0x01    /* Accept All Packets */
#define RCR_WRAP        0x80    /* RX buffer wrap to beginning */

/* RX buffer */
#define RX_BUF_SIZE    (64 * 1024)   /* 64 KB ring buffer */
#define RX_BUF_WRAP    0xFFFF        /* 16-bit CAPR wraps */

/* TX ring */
#define TX_RING_SIZE   4

/* Status word at start of RX packet (in RX buffer) */
struct rtl_rx_status {
    uint16_t len;       /* Total packet length (including status words) */
    uint16_t flags;     /* Status flags */
};

/* ========================================================================
 * Driver state
 * ======================================================================== */

static uint16_t  rtl_io_base  = 0;
static uint8_t   rtl_mac[6];
static int       rtl_present  = 0;
static uint32_t  rtl_tx_next   = 0;    /* Next TX descriptor index (0-3) */
static uint8_t  *rx_buffer     = NULL;  /* 64 KB RX ring buffer */
static uint16_t  rx_capr       = 0;    /* Current read position in RX buffer */
static uint32_t  tx_buffers[TX_RING_SIZE] = {0}; /* TX packet buffer addrs */

/* Register access helpers (IO-port mapped) */
static inline void rtl_write8(uint16_t off, uint8_t v)  { outb(rtl_io_base + off, v); }
static inline void rtl_write16(uint16_t off, uint16_t v) { outw(rtl_io_base + off, v); }
static inline void rtl_write32(uint16_t off, uint32_t v) { outl(rtl_io_base + off, v); }
static inline uint8_t  rtl_read8(uint16_t off)  { return inb(rtl_io_base + off); }
static inline uint16_t rtl_read16(uint16_t off) { return inw(rtl_io_base + off); }
static inline uint32_t rtl_read32(uint16_t off) { return inl(rtl_io_base + off); }

/* ========================================================================
 * Init
 * ======================================================================== */

int rtl8139_init(void) {
    struct pci_device *dev = pci_find_device(0x10EC, 0x8139);
    /* Also check common variant IDs */
    if (!dev) dev = pci_find_device(0x10EC, 0x8139 + 1); /* 0x8138 is a revision */
    if (!dev) return 0;

    /* RTL8139 uses BAR0 as IO base. Bit 0 = IO space indicator. */
    rtl_io_base = (uint16_t)(dev->bar[0] & ~0x3u);
    if (!rtl_io_base) {
        pr_warn("rtl8139: BAR0 is zero\n");
        return 0;
    }

    pci_device_enable(dev);

    pr_info("rtl8139: found at IO base 0x%x (PCI %02x:%02x.%x)\n",
            (unsigned)rtl_io_base, dev->bus, dev->dev, dev->func);

    /* --- Software reset --- */
    rtl_write8(RTL_CR, CR_RST);
    /* Poll until reset bit clears (spec says < 10 us, use generous timeout) */
    for (int i = 0; i < 100000; i++) {
        if (!(rtl_read8(RTL_CR) & CR_RST)) break;
    }
    if (rtl_read8(RTL_CR) & CR_RST) {
        pr_warn("rtl8139: reset timed out\n");
        return 0;
    }

    /* --- Read MAC from IDR0-IDR5 --- */
    rtl_mac[0] = rtl_read8(RTL_IDR0);
    rtl_mac[1] = rtl_read8(RTL_IDR1);
    rtl_mac[2] = rtl_read8(RTL_IDR2);
    rtl_mac[3] = rtl_read8(RTL_IDR3);
    rtl_mac[4] = rtl_read8(RTL_IDR4);
    rtl_mac[5] = rtl_read8(RTL_IDR5);
    pr_info("rtl8139: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            rtl_mac[0], rtl_mac[1], rtl_mac[2],
            rtl_mac[3], rtl_mac[4], rtl_mac[5]);

    /* --- Allocate TX buffers (max frame size) --- */
    for (int i = 0; i < TX_RING_SIZE; i++) {
        tx_buffers[i] = 0;
    }

    /* --- Allocate RX ring buffer (64 KB, must be 16-byte aligned) --- */
    rx_buffer = (uint8_t*)kmalloc(RX_BUF_SIZE + 16);
    if (!rx_buffer) {
        pr_warn("rtl8139: OOM for RX buffer\n");
        return 0;
    }
    memset(rx_buffer, 0, RX_BUF_SIZE + 16);
    /* Identity-mapped kernel: virt addr = phys addr. Align to 16 bytes. */
    uint32_t rx_phys = ((uint32_t)(uintptr_t)rx_buffer + 15) & ~15u;
    rx_buffer = (uint8_t*)(uintptr_t)rx_phys;
    rtl_write32(RTL_RBSTART, rx_phys);

    /* --- Configure RX: accept broadcast + physical match + wrap --- */
    rtl_write32(RTL_RCR, RCR_AB | RCR_APM | RCR_WRAP);

    /* --- Configure TX --- */
    rtl_write32(RTL_TCR, 0x03000700);  /* IFG=3, MXDMA=7 (2048 bytes), CRC append */

    /* --- Clear pending interrupts --- */
    rtl_write16(RTL_ISR, 0xFFFF);

    /* --- Enable RX and TX --- */
    rtl_write8(RTL_CR, CR_RX_ENABLE | CR_TX_ENABLE | CR_BUF_SIZE_64K);

    /* Set CAPR to the start of the RX buffer */
    rx_capr = 16; /* Offset into RX buffer (skip nothing at start) */
    rtl_write16(RTL_CAPR, rx_phys + rx_capr);

    rtl_present = 1;
    pr_info("rtl8139: initialized (IO base 0x%x, %d KB RX buffer)\n",
            (unsigned)rtl_io_base, RX_BUF_SIZE / 1024);
    return 1;
}

int rtl8139_is_present(void) { return rtl_present; }

mac_addr_t rtl8139_get_mac(void) {
    mac_addr_t m;
    memcpy(m.bytes, rtl_mac, 6);
    return m;
}

/* ========================================================================
 * TX: Send a packet
 * ======================================================================== */

int rtl8139_send(const void* data, uint16_t len) {
    if (!rtl_present) return -1;
    if (len < 64) len = 64;   /* RTL8139 pads to 64 bytes min */
    if (len > 1792) return -1; /* Max TX packet size */

    uint32_t idx = rtl_tx_next % TX_RING_SIZE;
    uint16_t tsad_off = RTL_TSAD0 + idx * 4;
    uint16_t tsd_off  = RTL_TSD0  + idx * 4;

    /* Allocate TX buffer if needed */
    if (!tx_buffers[idx]) {
        tx_buffers[idx] = (uint32_t)(uintptr_t)kmalloc(2048);
        if (!tx_buffers[idx]) return -1;
    }

    /* Copy packet data to TX buffer */
    memcpy((void*)(uintptr_t)tx_buffers[idx], data, len);

    /* Set TX address */
    rtl_write32(tsad_off, tx_buffers[idx]);

    /* Set TX descriptor: length + OWN bit */
    rtl_write32(tsd_off, (uint32_t)len | TSD_OWN);

    /* Poll for completion (TXOK bit in TSD) */
    int timeout = 1000000;
    while (timeout-- > 0) {
        uint32_t tsd = rtl_read32(tsd_off);
        if (tsd & TSD_TXOK) break;
    }

    /* Clear TXOK in ISR so next send works */
    rtl_write16(RTL_ISR, RTL_ISR);

    rtl_tx_next++;
    return (int)len;
}

/* ========================================================================
 * RX: Receive a packet (called from net_tick)
 * ======================================================================== */

int rtl8139_recv(void* buf, uint16_t bufsz) {
    if (!rtl_present) return 0;

    /* Each packet in the RX buffer is preceded by a 4-byte status header:
     *   word 0: flags (bit 0 = IOR, bit 13 = RUNT, bit 15 = ROK)
     *   word 1: total length including this header and CRC (max 16-bit)
     *   word 2-3: multicast filter match (optional, present if IOR set)
     * We track rx_capr as our read position within the RX buffer. */
    struct rtl_rx_status *status = (struct rtl_rx_status*)(rx_buffer + rx_capr);
    uint16_t pkt_len = status->len;
    uint16_t flags = status->flags;

    /* ROK (Received OK) is bit 15. If not set, the packet has an error. */
    if (!(flags & 0x0001)) return 0;  /* No valid packet */
    /* pkt_len includes 4-byte header + 4-byte CRC.
     * Actual data = pkt_len - 8, but we subtract 4 (just CRC) and let
     * the net stack handle the 4-byte CRC. */
    pkt_len -= 4;
    if (pkt_len < 14 || pkt_len > 1514) return 0;  /* Invalid Ethernet frame */
    if (pkt_len > bufsz) pkt_len = bufsz;

    uint16_t data_start = rx_capr + 4; /* Skip 4-byte status header */
    uint16_t data_end = data_start + pkt_len;

    /* Advance rx_capr to the next packet (pkt_len + 4, aligned to 4 bytes) */
    rx_capr = (rx_capr + pkt_len + 4 + 3) & ~3u;
    if (rx_capr >= RX_BUF_SIZE) rx_capr -= RX_BUF_SIZE; /* Wrap */

    /* Tell hardware where we've read to */
    uint32_t rx_phys = (uint32_t)(uintptr_t)rx_buffer;
    rtl_write16(RTL_CAPR, rx_phys + rx_capr);

    /* Handle wrap-around copy */
    if (data_end <= RX_BUF_SIZE) {
        /* No wrap needed */
        memcpy(buf, rx_buffer + data_start, pkt_len);
    } else {
        /* Packet wraps around the end of the buffer */
        uint16_t first_part = RX_BUF_SIZE - data_start;
        memcpy(buf, rx_buffer + data_start, first_part);
        memcpy((uint8_t*)buf + first_part, rx_buffer, pkt_len - first_part);
    }

    return (int)pkt_len;
}
