/*
 * Lestra OS - Intel 82540EM (E1000) NIC driver
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * QEMU's default NIC (`-device e1000`) is the Intel 82540EM, which is
 * well-documented and straightforward to drive. This is a polling-based
 * driver (no interrupts) — call e1000_poll() from net_tick() to drain
 * the RX ring.
 *
 * References:
 *   - Intel 8254x Gigabit Ethernet Controller Software Developer's Manual
 *   - OSdev wiki: https://wiki.osdev.org/Intel8254x
 */

#include <lestra/types.h>
#include <lestra/net.h>
#include <lestra/printk.h>
#include <lestra/panic.h>
#include <lestra/pci.h>
#include <string.h>

/* E1000 register offsets (relative to BAR0 MMIO base) */
#define E1000_CTRL      0x0000
#define E1000_STATUS    0x0008
#define E1000_EECD      0x0010
#define E1000_IMC       0x00D8   /* Interrupt Mask Clear */
#define E1000_RCTL      0x0100   /* Receive Control */
#define E1000_TCTL      0x0400   /* Transmit Control */
#define E1000_RDBAL     0x2800   /* RX Descriptor Base Low */
#define E1000_RDBAH     0x2804
#define E1000_RDLEN     0x2808
#define E1000_RDH       0x2810   /* RX Head */
#define E1000_RDT       0x2818   /* RX Tail */
#define E1000_TDBAL     0x3800
#define E1000_TDBAH     0x3804
#define E1000_TDLEN     0x3808
#define E1000_TDH       0x3810
#define E1000_TDT       0x3818
#define E1000_RA        0x5400   /* Receive Address register array */
#define E1000_MTA       0x5200   /* Multicast Table Array (128 entries) */

/* RCTL bits */
#define RCTL_EN         (1u << 1)
#define RCTL_SZ_2048    0
#define RCTL_BAM        (1u << 15)   /* Broadcast Accept Mode */
#define RCTL_SECRC      (1u << 26)   /* Strip Ethernet CRC */

/* TCTL bits */
#define TCTL_EN         (1u << 1)
#define TCTL_PSP        (1u << 3)

/* Descriptor layouts (legacy mode, 16 bytes each) */
struct rx_desc {
    uint64_t addr;        /* physical address of buffer */
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __packed;

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __packed;

#define TX_CMD_EOP    0x01   /* End of Packet */
#define TX_CMD_IFCS   0x02   /* Insert FCS */
#define TX_CMD_RS     0x08   /* Report Status */

#define NUM_RX  16
#define NUM_TX  16
#define RX_BUF_SZ 2048
#define TX_BUF_SZ 2048

/* State */
static int       e1000_present = 0;
static uintptr_t e1000_mmio = 0;
static mac_addr_t e1000_mac = MAC_ZERO;

static struct rx_desc rx_descs[NUM_TX] __aligned(16);
static struct tx_desc tx_descs[NUM_TX] __aligned(16);
static uint8_t rx_buffers[NUM_RX][RX_BUF_SZ] __aligned(16);
static uint8_t tx_buffers[NUM_TX][TX_BUF_SZ] __aligned(16);
static uint32_t rx_next = 0;
static uint32_t tx_next = 0;

/* ----- MMIO access ----- */
static inline uint32_t er32(uint32_t off) {
    return *(volatile uint32_t*)(e1000_mmio + off);
}
static inline void ew32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(e1000_mmio + off) = v;
}

/* Read the MAC from EEPROM. The 82540EM has a simple bit-banged EEPROM
 * interface. Each EEPROM read returns 16 bits. The MAC is the first 3
 * words of the EEPROM. */
#define E1000_EECD_SK   (1u << 0)
#define E1000_EECD_CS   (1u << 1)
#define E1000_EECD_DI   (1u << 2)
#define E1000_EECD_DO   (1u << 3)
#define E1000_EECD_REQ  (1u << 6)
#define E1000_EECD_GNT  (1u << 7)

static uint16_t eeprom_read(uint8_t addr) {
    /* Request EEPROM access */
    uint32_t eecd = er32(E1000_EECD) | E1000_EECD_REQ;
    ew32(E1000_EECD, eecd);
    /* Wait for grant (with a small bounded loop) */
    for (int i = 0; i < 1000; i++) {
        if (er32(E1000_EECD) & E1000_EECD_GNT) break;
    }
    /* Chip select */
    eecd = er32(E1000_EECD) | E1000_EECD_CS;
    ew32(E1000_EECD, eecd);
    /* Send read opcode (11) + 6-bit address (MSB first) */
    uint16_t cmd = (0b110u << 6) | (addr & 0x3F);
    for (int i = 8; i >= 0; i--) {
        uint32_t bit = (cmd >> i) & 1;
        eecd = er32(E1000_EECD);
        eecd &= ~E1000_EECD_DI;
        eecd |= bit ? E1000_EECD_DI : 0;
        ew32(E1000_EECD, eecd);
        /* clock pulse */
        eecd |= E1000_EECD_SK;  ew32(E1000_EECD, eecd);
        eecd &= ~E1000_EECD_SK; ew32(E1000_EECD, eecd);
    }
    /* Read 16 bits back */
    uint16_t out = 0;
    for (int i = 0; i < 16; i++) {
        eecd = er32(E1000_EECD);
        eecd |= E1000_EECD_SK;  ew32(E1000_EECD, eecd);
        eecd &= ~E1000_EECD_SK; ew32(E1000_EECD, eecd);
        out = (out << 1) | ((er32(E1000_EECD) & E1000_EECD_DO) ? 1 : 0);
    }
    /* Deselect */
    eecd = er32(E1000_EECD) & ~E1000_EECD_CS;
    ew32(E1000_EECD, eecd);
    return out;
}

/* ----- PCI scan -----
 * Walk bus 0 looking for vendor 0x8086 (Intel), device 0x100E (82540EM). */
static int pci_find_e1000(uint8_t* bus, uint8_t* dev, uint8_t* func, uintptr_t* bar0) {
    for (uint8_t d = 0; d < 32; d++) {
        for (uint8_t f = 0; f < 8; f++) {
            uint32_t id = pci_config_read32(0, d, f, 0);
            if (id == 0xFFFFFFFFu) continue;
            uint16_t vendor = id & 0xFFFF;
            uint16_t device = (id >> 16) & 0xFFFF;
            if (vendor == 0x8086 && (device == 0x100E || device == 0x100F
                                   || device == 0x10D3 /* 82574L */)) {
                *bus = 0; *dev = d; *func = f;
                uint32_t b0 = pci_config_read32(0, d, f, 0x10);
                *bar0 = b0 & ~0xFu;
                uint32_t cmd = pci_config_read32(0, d, f, 0x04);
                cmd |= 0x7;  /* IOSE | MSE | BME */
                pci_config_write32(0, d, f, 0x04, cmd);
                return 1;
            }
        }
    }
    return 0;
}

/* ----- public driver API (called by net.c) ----- */

int e1000_init(void);
int e1000_is_present(void);
mac_addr_t e1000_get_mac(void);
int e1000_send(const void* data, uint16_t len);
int e1000_recv(void* buf, uint16_t bufsz);

int e1000_init(void) {
    uint8_t bus, dev, func;
    uintptr_t bar0;
    if (!pci_find_e1000(&bus, &dev, &func, &bar0)) {
        pr_warn("e1000: no Intel NIC found in PCI bus 0\n");
        return 0;
    }
    e1000_mmio = bar0;
    pr_info("e1000: found at PCI 00:%u.%u, MMIO=0x%x\n",
            (unsigned)dev, (unsigned)func, (unsigned)bar0);

    /* Read MAC from EEPROM */
    uint16_t w0 = eeprom_read(0);
    uint16_t w1 = eeprom_read(1);
    uint16_t w2 = eeprom_read(2);
    e1000_mac.bytes[0] = w0 & 0xFF;
    e1000_mac.bytes[1] = (w0 >> 8) & 0xFF;
    e1000_mac.bytes[2] = w1 & 0xFF;
    e1000_mac.bytes[3] = (w1 >> 8) & 0xFF;
    e1000_mac.bytes[4] = w2 & 0xFF;
    e1000_mac.bytes[5] = (w2 >> 8) & 0xFF;
    pr_info("e1000: MAC %x:%x:%x:%x:%x:%x\n",
            e1000_mac.bytes[0], e1000_mac.bytes[1], e1000_mac.bytes[2],
            e1000_mac.bytes[3], e1000_mac.bytes[4], e1000_mac.bytes[5]);

    /* Disable interrupts (we poll) */
    ew32(E1000_IMC, 0xFFFFFFFFu);

    /* ----- RX setup ----- */
    for (int i = 0; i < NUM_RX; i++) {
        rx_descs[i].addr = (uint64_t)(uintptr_t)rx_buffers[i];
        rx_descs[i].length = 0;
        rx_descs[i].status = 0;
    }
    ew32(E1000_RDBAL, (uint32_t)(uintptr_t)rx_descs);
    ew32(E1000_RDBAH, 0);
    ew32(E1000_RDLEN, NUM_RX * sizeof(struct rx_desc));
    ew32(E1000_RDH, 0);
    ew32(E1000_RDT, NUM_RX - 1);
    rx_next = 0;

    ew32(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_SZ_2048);

    /* ----- TX setup ----- */
    for (int i = 0; i < NUM_TX; i++) {
        tx_descs[i].addr = (uint64_t)(uintptr_t)tx_buffers[i];
        tx_descs[i].length = 0;
        tx_descs[i].cmd = 0;
        tx_descs[i].status = 0;
    }
    ew32(E1000_TDBAL, (uint32_t)(uintptr_t)tx_descs);
    ew32(E1000_TDBAH, 0);
    ew32(E1000_TDLEN, NUM_TX * sizeof(struct tx_desc));
    ew32(E1000_TDH, 0);
    ew32(E1000_TDT, 0);
    tx_next = 0;

    ew32(E1000_TCTL, TCTL_EN | TCTL_PSP);

    /* Program Receive Address register 0 with our MAC */
    uint32_t ral = (e1000_mac.bytes[0])
                 | ((uint32_t)e1000_mac.bytes[1] << 8)
                 | ((uint32_t)e1000_mac.bytes[2] << 16)
                 | ((uint32_t)e1000_mac.bytes[3] << 24);
    uint32_t rah = ((uint32_t)e1000_mac.bytes[4])
                 | ((uint32_t)e1000_mac.bytes[5] << 8)
                 | (1u << 31);  /* AV bit - Address Valid */
    ew32(E1000_RA + 0, ral);
    ew32(E1000_RA + 4, rah);

    /* Clear multicast table */
    for (int i = 0; i < 128; i++) {
        ew32(E1000_MTA + i*4, 0);
    }

    e1000_present = 1;
    /* Check link status (STATUS bit 1 = LU, Link Up) */
    uint32_t status = er32(E1000_STATUS);
    pr_info("e1000: link up (driver initialized, RX/TX rings %u/%u entries, STATUS=0x%x)\n",
            (unsigned)NUM_RX, (unsigned)NUM_TX, (unsigned)status);
    return 1;
}

int e1000_is_present(void) {
    return e1000_present;
}

mac_addr_t e1000_get_mac(void) {
    return e1000_mac;
}

int e1000_send(const void* data, uint16_t len) {
    if (!e1000_present) return 0;
    if (len > TX_BUF_SZ) len = TX_BUF_SZ;

    uint32_t idx = tx_next;
    memcpy(tx_buffers[idx], data, len);
    /* Set length FIRST, then cmd, then status=0. The hardware reads the
     * descriptor when TDT advances, so order matters. */
    tx_descs[idx].length = len;
    tx_descs[idx].cso = 0;
    tx_descs[idx].cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    tx_descs[idx].status = 0;
    tx_descs[idx].css = 0;
    tx_descs[idx].special = 0;

    /* Memory barrier: ensure descriptor + buffer writes are visible
     * before we ring the doorbell. */
    __asm__ __volatile__("" ::: "memory");

    tx_next = (tx_next + 1) % NUM_TX;
    ew32(E1000_TDT, tx_next);

    /* Wait for the DD (Descriptor Done) status bit. The hardware sets
     * this after the packet has been DMA'd out. */
    int timed_out = 1;
    for (int i = 0; i < 1000000; i++) {
        if (tx_descs[idx].status & 0x01) { timed_out = 0; break; }
    }
    if (timed_out) {
        pr_warn("e1000: TX timeout (desc %u, len=%u, TDT=%u, TDH=%u)\n",
                (unsigned)idx, (unsigned)len,
                (unsigned)er32(E1000_TDT), (unsigned)er32(E1000_TDH));
        return 0;
    }
    return len;
}

int e1000_recv(void* buf, uint16_t bufsz) {
    if (!e1000_present) return 0;

    uint32_t idx = rx_next;
    if (!(rx_descs[idx].status & 0x01)) {
        return 0;
    }

    uint16_t len = rx_descs[idx].length;
    if (len > bufsz) len = bufsz;
    memcpy(buf, rx_buffers[idx], len);

    rx_descs[idx].status = 0;
    rx_next = (rx_next + 1) % NUM_RX;
    ew32(E1000_RDT, (rx_next + NUM_RX - 1) % NUM_RX);

    return len;
}
