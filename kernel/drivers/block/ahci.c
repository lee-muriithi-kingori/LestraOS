/*
 * Lestra OS - AHCI (SATA) Host Bus Adapter driver
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * A minimal AHCI driver that can detect SATA drives and read sectors.
 * This is enough to support an ext2 filesystem on a virtual disk.
 *
 * Only reads are supported (sufficient for a read-only ext2 fs).
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <lestra/pci.h>
#include <string.h>

/* AHCI register offsets */
#define AHCI_GHC        0x04
#define AHCI_PI         0x0C
#define AHCI_VS         0x10
#define AHCI_PORTS      0x100
#define AHCI_PORT_SIZE  0x80

#define PORT_CLB        0x00
#define PORT_CLBU       0x04
#define PORT_FB         0x08
#define PORT_FBU        0x0C
#define PORT_CMD        0x18
#define PORT_TFD        0x20
#define PORT_SIG        0x24
#define PORT_SSTS       0x28
#define PORT_CI         0x38

#define GHC_AE          (1u << 31)
#define GHC_HR          (1u)
#define CMD_ST          (1u << 0)
#define CMD_FRE         (1u << 4)
#define CMD_CR          (1u << 15)
#define CMD_FR          (1u << 14)

struct cmd_header {
    uint8_t  config;
    uint8_t  status;
    uint32_t prdtl;
    uint32_t prdbc;
    uint64_t ctba;
    uint32_t reserved[4];
} __packed;

struct prd_entry {
    uint64_t dba;
    uint32_t reserved;
    uint32_t dbc;
} __packed;

struct cmd_table {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  reserved[48];
    struct prd_entry prd;
} __packed;

static uintptr_t ahci_abar = 0;
static int ahci_present = 0;
static int ahci_port = -1;

static struct cmd_header cmd_list[32] __aligned(1024);
static struct cmd_table cmd_tbl __aligned(128);
static uint8_t ahci_dma_buf[4096] __aligned(256);

static inline uint32_t ahci_read(uint32_t off) {
    return *(volatile uint32_t*)(ahci_abar + off);
}
static inline void ahci_write(uint32_t off, uint32_t val) {
    *(volatile uint32_t*)(ahci_abar + off) = val;
}
static inline uint32_t port_read(int port, uint32_t off) {
    return ahci_read(AHCI_PORTS + port * AHCI_PORT_SIZE + off);
}
static inline void port_write(int port, uint32_t off, uint32_t val) {
    ahci_write(AHCI_PORTS + port * AHCI_PORT_SIZE + off, val);
}

static int ahci_find_hba(uint8_t* bus, uint8_t* dev, uint8_t* func) {
    int ndevs = pci_get_device_count();
    for (int i = 0; i < ndevs; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) continue;
        if (d->class_code == 0x01 && d->subclass == 0x06) {
            *bus = d->bus; *dev = d->dev; *func = d->func;
            return 1;
        }
    }
    return 0;
}

int ahci_init(void);
int ahci_is_present(void);
int ahci_has_drive(void);
int ahci_read_sectors(uint64_t lba, uint32_t count, void* buf);

int ahci_init(void) {
    uint8_t bus, dev, func;
    if (!ahci_find_hba(&bus, &dev, &func)) {
        pr_info("ahci: no AHCI HBA found\n");
        return 0;
    }

    uint32_t bar5 = pci_config_read32(bus, dev, func, 0x24);
    ahci_abar = bar5 & ~0xFu;
    pr_info("ahci: ABAR = 0x%x\n", (unsigned)ahci_abar);

    uint32_t cmd_reg = pci_config_read32(bus, dev, func, 0x04);
    pci_config_write32(bus, dev, func, 0x04, cmd_reg | 0x7);

    uint32_t ghc = ahci_read(AHCI_GHC);
    ahci_write(AHCI_GHC, ghc | GHC_AE);
    ahci_write(AHCI_GHC, GHC_AE | GHC_HR);
    for (int i = 0; i < 100000; i++) {
        if (!(ahci_read(AHCI_GHC) & GHC_HR)) break;
    }
    ahci_write(AHCI_GHC, GHC_AE);

    uint32_t pi = ahci_read(AHCI_PI);
    uint32_t version = ahci_read(AHCI_VS);
    pr_info("ahci: version 0x%x, PI=0x%x\n",
            (unsigned)version, (unsigned)pi);

    ahci_port = -1;
    for (int p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;
        uint32_t ssts = port_read(p, PORT_SSTS);
        if ((ssts & 0x0F) == 0x03) {
            uint32_t sig = port_read(p, PORT_SIG);
            pr_info("ahci: port %d has device (SSTS=0x%x, SIG=0x%x)\n",
                    p, (unsigned)ssts, (unsigned)sig);
            if (sig == 0x00000101) {
                ahci_port = p;
                break;
            }
        }
    }

    if (ahci_port < 0) {
        pr_info("ahci: no SATA drive found\n");
        ahci_present = 1;
        return 1;
    }

    int p = ahci_port;
    uint32_t cmd = port_read(p, PORT_CMD);
    port_write(p, PORT_CMD, cmd & ~(CMD_ST | CMD_FRE));
    for (int i = 0; i < 100000; i++) {
        cmd = port_read(p, PORT_CMD);
        if (!(cmd & (CMD_CR | CMD_FR))) break;
    }

    port_write(p, PORT_CLB, (uint32_t)(uintptr_t)cmd_list);
    port_write(p, PORT_CLBU, 0);
    static uint8_t fis_buf[256] __aligned(256);
    port_write(p, PORT_FB, (uint32_t)(uintptr_t)fis_buf);
    port_write(p, PORT_FBU, 0);
    port_write(p, PORT_CMD, CMD_FRE);
    port_write(p, PORT_CMD, CMD_FRE | CMD_ST);

    ahci_present = 1;
    pr_info("ahci: drive ready on port %d\n", p);
    return 1;
}

int ahci_is_present(void) { return ahci_present; }
int ahci_has_drive(void) { return ahci_port >= 0; }

int ahci_read_sectors(uint64_t lba, uint32_t count, void* buf) {
    if (ahci_port < 0 || count == 0) return 0;
    if (count > 8) count = 8;
    int p = ahci_port;
    uint32_t byte_count = count * 512;

    struct cmd_header* hdr = &cmd_list[0];
    memset(hdr, 0, sizeof(*hdr));
    hdr->config = 5 | 0x80;
    hdr->prdtl = 1;
    hdr->ctba = (uint64_t)(uintptr_t)&cmd_tbl;

    memset(&cmd_tbl, 0, sizeof(cmd_tbl));
    cmd_tbl.prd.dba = (uint64_t)(uintptr_t)ahci_dma_buf;
    cmd_tbl.prd.dbc = (byte_count - 1) | (1u << 31);

    cmd_tbl.cfis[0] = 0x27;  /* FIS type: Register H2D */
    cmd_tbl.cfis[1] = 0x80;  /* C bit */
    cmd_tbl.cfis[2] = 0x25;  /* READ DMA EXT */
    cmd_tbl.cfis[4] = lba & 0xFF;
    cmd_tbl.cfis[5] = (lba >> 8) & 0xFF;
    cmd_tbl.cfis[6] = (lba >> 16) & 0xFF;
    cmd_tbl.cfis[7] = 0x40 | ((lba >> 24) & 0x0F);
    cmd_tbl.cfis[8] = (lba >> 32) & 0xFF;
    cmd_tbl.cfis[9] = (lba >> 40) & 0xFF;
    cmd_tbl.cfis[10] = (lba >> 48) & 0xFF;
    cmd_tbl.cfis[11] = (lba >> 56) & 0xFF;
    cmd_tbl.cfis[12] = count & 0xFF;
    cmd_tbl.cfis[13] = (count >> 8) & 0xFF;

    port_write(p, 0x10, 0xFFFFFFFFu);  /* PORT_IS clear */
    port_write(p, PORT_CI, 1);

    int timeout = 10000000;
    while (timeout-- > 0) {
        uint32_t ci = port_read(p, PORT_CI);
        if (!(ci & 1)) break;
    }
    if (timeout <= 0) {
        pr_warn("ahci: read timeout at LBA %u\n", (unsigned)lba);
        return 0;
    }

    uint32_t tfd = port_read(p, PORT_TFD);
    if (tfd & 0x01) {
        pr_warn("ahci: read error at LBA %u (TFD=0x%x)\n",
                (unsigned)lba, (unsigned)tfd);
        return 0;
    }

    memcpy(buf, ahci_dma_buf, byte_count);
    return (int)count;
}

/* Write `count` sectors (512 bytes each) starting at LBA `lba` from `buf`.
 * Returns number of sectors written, or 0 on error. */
int ahci_write_sectors(uint64_t lba, uint32_t count, const void* buf) {
    if (ahci_port < 0 || count == 0) return 0;
    if (count > 8) count = 8;
    int p = ahci_port;
    uint32_t byte_count = count * 512;

    /* Copy data to DMA buffer */
    memcpy(ahci_dma_buf, buf, byte_count);

    /* Set up the command header in slot 0 */
    struct cmd_header* hdr = &cmd_list[0];
    memset(hdr, 0, sizeof(*hdr));
    hdr->config = 5 | 0x80;  /* CFL=5, C=clear BUSY on completion */
    hdr->config |= 0x40;     /* W bit = write */
    hdr->prdtl = 1;
    hdr->ctba = (uint64_t)(uintptr_t)&cmd_tbl;

    /* Set up the PRD entry */
    memset(&cmd_tbl, 0, sizeof(cmd_tbl));
    cmd_tbl.prd.dba = (uint64_t)(uintptr_t)ahci_dma_buf;
    cmd_tbl.prd.dbc = (byte_count - 1) | (1u << 31);

    /* Build the FIS_REG_H2D for WRITE DMA EXT (0x35) */
    cmd_tbl.cfis[0] = 0x27;  /* FIS type: Register H2D */
    cmd_tbl.cfis[1] = 0x80;  /* C bit */
    cmd_tbl.cfis[2] = 0x35;  /* WRITE DMA EXT */
    cmd_tbl.cfis[4] = lba & 0xFF;
    cmd_tbl.cfis[5] = (lba >> 8) & 0xFF;
    cmd_tbl.cfis[6] = (lba >> 16) & 0xFF;
    cmd_tbl.cfis[7] = 0x40 | ((lba >> 24) & 0x0F);
    cmd_tbl.cfis[8] = (lba >> 32) & 0xFF;
    cmd_tbl.cfis[9] = (lba >> 40) & 0xFF;
    cmd_tbl.cfis[10] = (lba >> 48) & 0xFF;
    cmd_tbl.cfis[11] = (lba >> 56) & 0xFF;
    cmd_tbl.cfis[12] = count & 0xFF;
    cmd_tbl.cfis[13] = (count >> 8) & 0xFF;

    port_write(p, 0x10, 0xFFFFFFFFu);  /* PORT_IS clear */
    port_write(p, PORT_CI, 1);

    int timeout = 10000000;
    while (timeout-- > 0) {
        uint32_t ci = port_read(p, PORT_CI);
        if (!(ci & 1)) break;
    }
    if (timeout <= 0) {
        pr_warn("ahci: write timeout at LBA %u\n", (unsigned)lba);
        return 0;
    }

    uint32_t tfd = port_read(p, PORT_TFD);
    if (tfd & 0x01) {
        pr_warn("ahci: write error at LBA %u (TFD=0x%x)\n",
                (unsigned)lba, (unsigned)tfd);
        return 0;
    }

    return (int)count;
}
