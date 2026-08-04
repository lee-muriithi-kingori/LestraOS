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
 *   - Realtek RTL8139 Programming Guide (8139D datasheet)
 *   - OSdev wiki: https://wiki.osdev.org/RTL8139
 *   - Linux 8139too driver (kernel source)
 */

#include <lestra/types.h>
#include <lestra/net.h>
#include <lestra/printk.h>
#include <lestra/pci.h>
#include <string.h>

/* ========================================================================
 * RTL8139 Register Offsets (from IO base)
 * ======================================================================== */

#define RTL_IDR0        0x00
#define RTL_IDR1        0x01
#define RTL_IDR2        0x02
#define RTL_IDR3        0x03
#define RTL_IDR4        0x04
#define RTL_IDR5        0x05

#define RTL_CR          0x37    /* Command Register */
#define RTL_TCR         0x40    /* Transmit Configuration */
#define RTL_RCR         0x44    /* Receive Configuration */

#define RTL_TSAD0       0x20    /* TX Start Address Descriptor 0 */
#define RTL_TSAD1       0x24
#define RTL_TSAD2       0x28
#define RTL_TSAD3       0x2C
#define RTL_TSD0        0x10    /* TX Status Descriptor 0 */
#define RTL_TSD1        0x14
#define RTL_TSD2        0x18
#define RTL_TSD3        0x1C

#define RTL_RBSTART     0x30    /* Receive Buffer Start Address (physical) */
#define RTL_CAPR        0x38    /* Current Address of Packet Read */
#define RTL_IMR         0x3C    /* Interrupt Mask */
#define RTL_ISR         0x3E    /* Interrupt Status */

#define RTL_CONFIG1     0x52
#define RTL_CONFIG5     0x56

/* CR bits */
#define CR_RST          0x10
#define CR_RX_ENABLE    0x08
#define CR_TX_ENABLE    0x04
#define CR_BUF_SIZE_64K 0x00

/* TSD bits — RTL8139 has no OWN bit; writing length to TSD triggers TX */
#define TSD_TXOK       0x8000

/* RCR bits (per RTL8139D datasheet, offset 0x44) */
#define RCR_AER         0x01    /* Accept Error Packets */
#define RCR_AR          0x02    /* Accept Runt Packets */
#define RCR_AB          0x04    /* Accept Broadcast */
#define RCR_AM          0x08    /* Accept Multicast */
#define RCR_APM         0x10    /* Accept Physical Match */
#define RCR_AAP         0x20    /* Accept All Packets (promiscuous) */
#define RCR_WRAP        0x80    /* RX buffer wrap to beginning */
#define RCR_MXDMA_1024  (3 << 8)  /* Max RX DMA burst = 1024 bytes */

/* RX buffer */
#define RX_BUF_SIZE    (64 * 1024)

/* TX ring */
#define TX_RING_SIZE   4

/* ========================================================================
 * Driver state — all buffers in static storage (BSS) for DMA compatibility.
 * kmalloc() returns heap addresses (0x10000000+) which are outside guest
 * physical RAM — RTL8139 DMA cannot write there.
 * ======================================================================== */

static uint16_t  rtl_io_base  = 0;
static uint8_t   rtl_mac[6];
static int       rtl_present  = 0;
static uint32_t  rtl_tx_next   = 0;

static uint8_t   rx_buffer[RX_BUF_SIZE + 16] __aligned(16);
static uint16_t  rx_capr       = 0;

static uint8_t   tx_buf_0[2048] __aligned(16);
static uint8_t   tx_buf_1[2048] __aligned(16);
static uint8_t   tx_buf_2[2048] __aligned(16);
static uint8_t   tx_buf_3[2048] __aligned(16);
static uint8_t  *tx_ring[TX_RING_SIZE];

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
    if (!dev) return 0;

    rtl_io_base = (uint16_t)(dev->bar[0] & ~0x3u);
    if (!rtl_io_base) {
        pr_warn("rtl8139: BAR0 is zero\n");
        return 0;
    }

    pci_device_enable(dev);
    pr_info("rtl8139: found at IO base 0x%x (PCI %02x:%02x.%x)\n",
            (unsigned)rtl_io_base, dev->bus, dev->dev, dev->func);

    /* Software reset */
    rtl_write8(RTL_CR, CR_RST);
    for (int i = 0; i < 100000; i++) {
        if (!(rtl_read8(RTL_CR) & CR_RST)) break;
    }
    if (rtl_read8(RTL_CR) & CR_RST) {
        pr_warn("rtl8139: reset timed out\n");
        return 0;
    }

    /* Read MAC from IDR0-IDR5 */
    for (int i = 0; i < 6; i++)
        rtl_mac[i] = rtl_read8(RTL_IDR0 + i);
    pr_info("rtl8139: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            rtl_mac[0], rtl_mac[1], rtl_mac[2],
            rtl_mac[3], rtl_mac[4], rtl_mac[5]);

    /* Set up TX ring pointers (static buffers in real physical RAM) */
    tx_ring[0] = tx_buf_0;
    tx_ring[1] = tx_buf_1;
    tx_ring[2] = tx_buf_2;
    tx_ring[3] = tx_buf_3;

    /* Set up RX ring buffer (static, 64 KB, 16-byte aligned, real RAM) */
    memset(rx_buffer, 0, sizeof(rx_buffer));
    uint32_t rx_phys = ((uint32_t)(uintptr_t)rx_buffer + 15) & ~15u;
    rtl_write32(RTL_RBSTART, rx_phys);

    /* Clear Config1 (LAN wake bits can prevent RX) */
    rtl_write8(RTL_CONFIG1, 0x00);

    /* Configure RX: accept all packet types + wrap + 1024-byte DMA burst */
    rtl_write32(RTL_RCR, RCR_AB | RCR_APM | RCR_AM | RCR_AAP | RCR_WRAP | RCR_MXDMA_1024);

    /* Configure TX: IFG=3, MXDMA=2048, CRC append */
    rtl_write32(RTL_TCR, 0x03000700);

    /* Clear pending interrupts */
    rtl_write16(RTL_ISR, 0xFFFF);

    /* Enable RX and TX */
    rtl_write8(RTL_CR, CR_RX_ENABLE | CR_TX_ENABLE | CR_BUF_SIZE_64K);

    rx_capr = 0;
    rtl_present = 1;
    pr_info("rtl8139: initialized (IO base 0x%x, RBSTART=0x%x)\n",
            (unsigned)rtl_io_base, (unsigned)rx_phys);
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
    if (len < 64) len = 64;
    if (len > 1792) return -1;

    uint32_t idx = rtl_tx_next % TX_RING_SIZE;
    uint16_t tsad_off = RTL_TSAD0 + idx * 4;
    uint16_t tsd_off  = RTL_TSD0  + idx * 4;

    memcpy(tx_ring[idx], data, len);
    rtl_write32(tsad_off, (uint32_t)(uintptr_t)tx_ring[idx]);

    /* Writing length to TSD triggers transmit (no OWN bit in RTL8139) */
    rtl_write32(tsd_off, (uint32_t)len);

    /* Poll for TXOK */
    int tx_ok = 0;
    for (int i = 0; i < 1000000; i++) {
        if (rtl_read32(tsd_off) & TSD_TXOK) { tx_ok = 1; break; }
    }
    if (!tx_ok && rtl_tx_next > 0) {
        pr_warn("rtl8139: TX%d timed out! TSD=%08x\n", (int)idx, rtl_read32(tsd_off));
    }

    /* NOTE(KE-12): Do NOT clear ISR here. */
    rtl_tx_next++;
    return (int)len;
}

/* ========================================================================
 * RX: Receive a packet (called from net_tick at 1 kHz)
 *
 * QEMU RTL8139 model notes:
 *   - Hardware writes packets sequentially into the RX buffer at RxBufAddr.
 *   - Each packet starts with a 4-byte status header: {flags, length}.
 *   - flags bit 0 (ROK) = packet received OK.
 *   - length = total bytes including this 4-byte header + Ethernet frame.
 *   - QEMU's can_receive() blocks when CAPR == RxBufAddr (buffer full).
 *
 * NAPI-style deferred CAPR update (KE-12):
 *   We do NOT write CAPR at all. The register stays at its hardware
 *   default (0xFFF0), giving ~64KB of buffer headroom. Our software
 *   rx_capr tracks the actual read position independently.
 *   Writing CAPR forward (even with a gap) reduces QEMU's computed
 *   free space and can trigger the can_receive() deadlock. Leaving
 *   CAPR at 0xFFF0 means can_receive() always sees ~64KB free.
 *   Trade-off: we must drain faster than hardware fills — trivial at
 *   1kHz polling with a 64KB buffer (~190 packets before wrap).
 * ======================================================================== */

int rtl8139_recv(void* buf, uint16_t bufsz) {
    if (!rtl_present) return 0;

    uint16_t flags   = *(uint16_t*)(rx_buffer + rx_capr);
    uint16_t pkt_len = *(uint16_t*)(rx_buffer + rx_capr + 2);

    /* ROK (bit 0) must be set for a valid packet */
    if (!(flags & 0x0001))
        return 0;

    /* Extract Ethernet frame length.
     * pkt_len includes the 4-byte status header + Ethernet frame.
     * QEMU's RTL8139 model does NOT include CRC in the length field.
     * So: frame_len = pkt_len - 4 (status header only). */
    uint16_t frame_len = pkt_len - 4;
    if (frame_len < 14 || frame_len > 1514) return 0;
    if (frame_len > bufsz) frame_len = bufsz;

    uint16_t data_start = rx_capr + 4;

    /* Advance read pointer (4-byte aligned, wrapping at 64 KB) */
    rx_capr = (rx_capr + ((pkt_len + 3) & ~3u)) & (RX_BUF_SIZE - 1);

    /* Copy Ethernet frame, handling wrap-around */
    uint32_t end = (uint32_t)data_start + frame_len;
    if (end <= RX_BUF_SIZE) {
        memcpy(buf, rx_buffer + data_start, frame_len);
    } else {
        uint16_t first = RX_BUF_SIZE - data_start;
        memcpy(buf, rx_buffer + data_start, first);
        memcpy((uint8_t*)buf + first, rx_buffer, frame_len - first);
    }

    return (int)frame_len;
}

/* Flush: called after draining a batch of packets in net_tick().
 * With KE-12, we do NOT write CAPR (it stays at 0xFFF0 hardware default).
 * This function exists as a hook for future interrupt-driven RX where
 * we might need to re-arm the RX interrupt after draining.
 */
void rtl8139_recv_flush(void) {
    /* Intentionally empty: CAPR stays at hardware default (0xFFF0).
     * Writing CAPR forward causes QEMU can_receive() to see less free
     * space, potentially deadlocking RX. The software rx_capr tracks
     * the actual read position independently. */
}
