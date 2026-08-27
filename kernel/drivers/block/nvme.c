/*
 * Lestra OS - NVMe (NVM Express) Driver
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * NVMe is the modern PCIe SSD interface - dramatically faster than the
 * AHCI/SATA path and the standard drive on most current platforms.
 * This driver supports a single namespace with a 512-byte logical block
 * size (LBADS=9), which is what effectively every NVMe drive exposes.
 *
 * Single NVMe I/O queue, polling (no MSI-X / interrupts), one command
 * in flight at a time - the same synchronous model as the AHCI driver,
 * so it needs no queue concurrency or per-queue locks.
 *
 * PRPs require 4K-aligned buffers: all DMA memory is static and
 * page-aligned (BSS), matching the "kmalloc() is outside guest RAM"
 * rule used by virtio_blk/ahci. Transfers are capped at 8 sectors
 * (4096 bytes) so a single PRP entry suffices.
 *
 * References:
 *   - NVM Express Base Specification 1.4 (nvmexpress.org)
 *   - OSdev wiki: https://wiki.osdev.org/NVMe
 *   - Linux drivers/nvme/host/pci.c and nvme.h
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/pci.h>
#include <string.h>

/* ========================================================================
 * Controller registers (BAR0, offsets below 0x1000)
 * ======================================================================== */

#define NVME_CAP   0x00   /* Controller Capabilities (64-bit)        */
#define NVME_VS    0x08   /* Version                                 */
#define NVME_INTMS 0x0C   /* Interrupt Mask Set    (write 1 = mask)  */
#define NVME_INTMC 0x10   /* Interrupt Mask Clear  (write 1 = unmask)*/
#define NVME_CC    0x14   /* Controller Configuration                */
#define NVME_CSTS  0x1C   /* Controller Status                      */
#define NVME_AQA   0x24   /* Admin Queue Attributes                  */
#define NVME_ASQ   0x28   /* Admin Submission Queue base (64-bit)    */
#define NVME_ACQ   0x30   /* Admin Completion Queue base (64-bit)    */

/* Doorbells start at 0x1000, stride = 4 * (1 << CAP.DSTRD) bytes:
 *   SQ(n) Tail = 0x1000 + 2*n*stride
 *   CQ(n) Head = 0x1000 + (2*n + 1)*stride
 */
#define NVME_DBL_OFF    0x1000

/* CAP fields */
#define CAP_MQES(x)   ((x) & 0xFFFF)                  /* max queue size - 1 */
#define CAP_TO(x)     (((x) >> 32) & 0xFF)            /* timeout in 500ms   */
#define CAP_DSTRD(x)  (((x) >> 40) & 0xF)             /* doorbell stride    */
#define CAP_MPSMIN(x) (((x) >> 19) & 0x1F)            /* min page size 2^(12+x) */

/* CC fields */
#define CC_EN       BIT(0)
#define CC_MPS(x)   (((x) & 0xF) << 7)
#define CC_IOSQES(x) (((x) & 0xF) << 16)              /* default: 6 -> 64B  */
#define CC_IOCQES(x) (((x) & 0xF) << 20)              /* default: 4 -> 16B  */

/* CSTS fields */
#define CSTS_RDY    BIT(0)
#define CSTS_CFS    BIT(1)

/* AQA fields: queue size - 1 is allowed to be 0 (a 1-entry queue) */
#define AQA_ASQS(x) ((x) & 0xFFFF)
#define AQA_ACQS(x) (((x) & 0xFFFF) << 16)

/* ========================================================================
 * Commands (SQE, 64 bytes) and completions (CQE, 16 bytes)
 * ======================================================================== */

struct nvme_command {
    uint8_t  opcode;
    uint8_t  flags;             /* SGL/PSDT flags */
    uint16_t cid;
    uint32_t nsid;
    uint32_t cdw2;
    uint32_t cdw3;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __packed;
STATIC_ASSERT(sizeof(struct nvme_command) == 64, "nvme sqe must be 64 bytes");

struct nvme_completion {
    uint32_t result;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;            /* bit 0 = phase tag, bits 15:1 = status code */
} __packed;
STATIC_ASSERT(sizeof(struct nvme_completion) == 16, "nvme cqe must be 16 bytes");

/* Admin command opcodes */
#define NVME_OPC_CREATE_IOSQ   0x01
#define NVME_OPC_DELETE_IOCQ   0x04
#define NVME_OPC_CREATE_IOCQ   0x05
#define NVME_OPC_IDENTIFY      0x06
#define NVME_OPC_SET_FEATURES  0x09

/* I/O command opcodes */
#define NVME_OPC_WRITE         0x01
#define NVME_OPC_READ          0x02

/* Identify CNS values */
#define NVME_CNS_CONTROLLER    0x01
#define NVME_CNS_NAMESPACE     0x00

/* Set Features FIDs */
#define NVME_FID_NUM_QUEUES    0x07

/* Admin queue / IO queue sizes (queue-size-1 is stored in registers) */
#define NVME_ADMIN_Q   8
#define NVME_IO_Q      8

/* ========================================================================
 * Driver state - all DMA memory static + page-aligned for PRP/queue use.
 * ======================================================================== */

static int        nvme_present = 0;
static uintptr_t  nvme_regs = 0;           /* BAR0 mapped (< 4 GB) */
static uint32_t   nvme_dbl_stride = 4;     /* in bytes */

/* Admin queue pair (QID 0) */
static struct nvme_command     admin_sq[NVME_ADMIN_Q] __aligned(4096);
static struct nvme_completion  admin_cq[NVME_ADMIN_Q] __aligned(4096);
static uint16_t  admin_sq_tail = 0;
static uint16_t  admin_cq_head = 0;
static int       admin_cq_phase = 1;       /* controller starts at phase 1 */
static uint16_t  admin_cid = 0;

/* I/O queue pair (QID 1) */
static struct nvme_command     io_sq[NVME_IO_Q] __aligned(4096);
static struct nvme_completion  io_cq[NVME_IO_Q] __aligned(4096);
static uint16_t  io_sq_tail = 0;
static uint16_t  io_cq_head = 0;
static int       io_cq_phase = 1;
static uint16_t  io_cid = 0;

/* Data transfer bounce buffer (single PRP, max 8 sectors = 1 page) */
static uint8_t   nvme_io_buf[4096] __aligned(4096);

/* Identify buffers (4096-byte transfers, PRP1 must be page-aligned) */
static uint8_t   nvme_id_ctrl[4096] __aligned(4096);
static uint8_t   nvme_id_ns[4096] __aligned(4096);

/* Disk info */
static uint64_t  nvme_capacity = 0;        /* in 512-byte sectors */
static uint32_t  nvme_blk_size = 512;
static char      nvme_model[40] = "?";
static char      nvme_fw[8] = "?";

/* ========================================================================
 * MMIO + doorbell helpers
 * ======================================================================== */

static inline uint32_t nvme_rd32(uint32_t off) {
    return *(volatile uint32_t*)(nvme_regs + off);
}
static inline void nvme_wr32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(nvme_regs + off) = v;
}
/* Store a 64-bit value to a register pair, low dword first. */
static inline void nvme_wr64(uint32_t off, uint64_t v) {
    nvme_wr32(off, (uint32_t)v);
    nvme_wr32(off + 4, (uint32_t)(v >> 32));
}

static inline void nvme_dbl_sq_tail(uint16_t qid, uint16_t tail) {
    nvme_wr32(NVME_DBL_OFF + 2 * qid * nvme_dbl_stride, tail);
}
static inline void nvme_dbl_cq_head(uint16_t qid, uint16_t head) {
    nvme_wr32(NVME_DBL_OFF + (2 * qid + 1) * nvme_dbl_stride, head);
}

/* Memory barrier before publishing an SQ entry with its doorbell, and
 * a full fence after ringing it so the completion read sees posted DMA. */
static inline void nvme_fence(void) {
    __asm__ volatile("mfence" ::: "memory");
}

/* ========================================================================
 * Completion polling
 * ======================================================================== */

/* Wait for the next completion on cq, verify it belongs to expect_cid,
 * release the entry via the head doorbell and report any error status. */
static int nvme_poll_cq(struct nvme_completion *cq, uint16_t qsz,
                        uint16_t *head, int *phase, uint16_t qid,
                        uint16_t expect_cid, uint32_t *result) {
    for (int i = 0; i < 100000000; i++) {
        struct nvme_completion *c = &cq[*head];
        if ((c->status & 1u) != (uint16_t)*phase)
            continue;

        uint16_t sc = c->status >> 1;            /* status code (0 = OK) */
        if (result) *result = c->result;

        /* Release the completion entry before the controller reuses it */
        *head = (*head + 1) % qsz;
        nvme_dbl_cq_head(qid, *head);
        if (*head == 0)
            *phase = !*phase;

        if (sc) {
            pr_warn("nvme: queue %u command %u failed (status=0x%x, cid=%u)\n",
                    (unsigned)qid, (unsigned)expect_cid,
                    (unsigned)sc, (unsigned)c->cid);
            return -1;
        }
        if (expect_cid != 0 && c->cid != expect_cid) {
            pr_warn("nvme: unexpected cid %u (wanted %u)\n",
                    (unsigned)c->cid, (unsigned)expect_cid);
            return -1;
        }
        return 0;
    }
    pr_warn("nvme: queue %u command %u timed out waiting for completion\n",
            (unsigned)qid, (unsigned)expect_cid);
    return -1;
}

/* Submit one admin command synchronously. Returns 0 on success. */
static int nvme_submit_admin(const struct nvme_command *cmd, uint32_t *result) {
    struct nvme_command *slot = &admin_sq[admin_sq_tail];

    admin_cid++;
    slot->opcode  = cmd->opcode;
    slot->flags   = 0;
    slot->cid     = admin_cid;
    slot->nsid    = cmd->nsid;
    slot->cdw2    = cmd->cdw2;
    slot->cdw3    = cmd->cdw3;
    slot->mptr    = cmd->mptr;
    slot->prp1    = cmd->prp1;
    slot->prp2    = cmd->prp2;
    slot->cdw10   = cmd->cdw10;
    slot->cdw11   = cmd->cdw11;
    slot->cdw12   = cmd->cdw12;
    slot->cdw13   = cmd->cdw13;
    slot->cdw14   = cmd->cdw14;
    slot->cdw15   = cmd->cdw15;

    nvme_fence();                              /* publish the SQE */
    admin_sq_tail = (admin_sq_tail + 1) % NVME_ADMIN_Q;
    nvme_dbl_sq_tail(0, admin_sq_tail);
    nvme_fence();                              /* fence doorbell writes */

    return nvme_poll_cq(admin_cq, NVME_ADMIN_Q, &admin_cq_head,
                        &admin_cq_phase, 0, admin_cid, result);
}

/* Submit one I/O command synchronously (single outstanding at a time). */
static int nvme_submit_io(const struct nvme_command *cmd, uint32_t *result) {
    struct nvme_command *slot = &io_sq[io_sq_tail];

    io_cid++;
    *slot = *cmd;
    slot->cid = io_cid;

    nvme_fence();
    io_sq_tail = (io_sq_tail + 1) % NVME_IO_Q;
    nvme_dbl_sq_tail(1, io_sq_tail);
    nvme_fence();

    return nvme_poll_cq(io_cq, NVME_IO_Q, &io_cq_head,
                        &io_cq_phase, 1, io_cid, result);
}

/* ========================================================================
 * PCI discovery
 * ======================================================================== */

/* NVMe controller: PCI class 01 (storage), subclass 08 (NVM). */
static struct pci_device *nvme_find_pci(void) {
    int ndevs = pci_get_device_count();
    for (int i = 0; i < ndevs; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) continue;
        if (d->class_code == 0x01 && d->subclass == 0x08)
            return d;
    }
    return NULL;
}

/* ========================================================================
 * Controller init
 * ======================================================================== */

int nvme_init(void);
int nvme_is_present(void);
int nvme_has_drive(void);
int nvme_read_sectors(uint64_t lba, uint32_t count, void* buf);
int nvme_write_sectors(uint64_t lba, uint32_t count, const void* buf);

int nvme_init(void) {
    struct pci_device *d = nvme_find_pci();
    if (!d) {
        pr_info("nvme: no NVMe controller found\n");
        return 0;
    }

    /* BAR0 is a 64-bit BAR. If the high dword is non-zero the register
     * window sits above 4 GB and this kernel (no ioremap, low-4GB-only
     * identity map, see pci.c) cannot reach it. */
    if (d->bar[1]) {
        pr_warn("nvme: BAR0 above 4 GB (0x%x%08x), not accessible\n",
                (unsigned)d->bar[1], (unsigned)d->bar[0]);
        return 0;
    }
    nvme_regs = (uintptr_t)(d->bar[0] & ~0xFu);

    /* Memory + bus master */
    uint32_t cmd_reg = pci_config_read32(d->bus, d->dev, d->func, 0x04);
    pci_config_write32(d->bus, d->dev, d->func, 0x04, cmd_reg | 0x6);

    uint64_t cap = (uint64_t)nvme_rd32(NVME_CAP + 4) << 32 | nvme_rd32(NVME_CAP);
    uint8_t  mpsmin = CAP_MPSMIN(cap);
    uint8_t  to     = CAP_TO(cap);
    nvme_dbl_stride = (uint32_t)(1u << (CAP_DSTRD(cap) + 2));   /* in bytes */

    if (CAP_MQES(cap) < NVME_IO_Q - 1) {
        pr_warn("nvme: controller max queue size %u < %u entries required\n",
                (unsigned)(CAP_MQES(cap) + 1), (unsigned)NVME_IO_Q);
        return 0;
    }

    pr_info("nvme: found at PCI %02x:%02x.%x, BAR0=0x%x, MPSMIN=%u, TO=%u\n",
            d->bus, d->dev, d->func, (unsigned)nvme_regs,
            (unsigned)mpsmin, (unsigned)to);

    /* PRP memory page size. We build 4K-aligned buffers only, so the
     * controller must run with 4096-byte pages. Nearly every NVMe SSD
     * reports MPSMIN=0; anything else we cannot drive honestly. */
    if (mpsmin != 0) {
        pr_warn("nvme: unsupported page size 2^(%u+12) (%u-aligned buffers only)\n",
                (unsigned)mpsmin, 4096);
        return 0;
    }

    /* Mask all interrupts - we poll. */
    nvme_wr32(NVME_INTMS, 0xFFFFFFFFu);

    /* Disable controller if a previous firmware/driver enabled it */
    if (nvme_rd32(NVME_CC) & CC_EN) {
        nvme_wr32(NVME_CC, 0);
        for (int i = 0; i < 10000000; i++) {
            if (!(nvme_rd32(NVME_CSTS) & CSTS_RDY)) break;
        }
        if (nvme_rd32(NVME_CSTS) & CSTS_RDY) {
            pr_warn("nvme: controller failed to become disabled\n");
            return 0;
        }
    }

    /* Queues: size-1 in AQA (we allocate NVME_ADMIN_Q entries) */
    nvme_wr32(NVME_AQA, AQA_ASQS(NVME_ADMIN_Q - 1) | AQA_ACQS(NVME_ADMIN_Q - 1));
    nvme_wr64(NVME_ASQ, (uintptr_t)admin_sq);
    nvme_wr64(NVME_ACQ, (uintptr_t)admin_cq);

    /* Enable: NVM command set (CSS=0), 4K pages, 64-byte SQE / 16-byte CQE */
    nvme_wr32(NVME_CC, CC_EN | CC_IOSQES(6) | CC_IOCQES(4));
    for (int i = 0; i < 10000000; i++) {
        if (nvme_rd32(NVME_CSTS) & CSTS_RDY) break;
    }
    if (!(nvme_rd32(NVME_CSTS) & CSTS_RDY)) {
        pr_warn("nvme: controller failed to become ready\n");
        return 0;
    }
    if (nvme_rd32(NVME_CSTS) & CSTS_CFS) {
        pr_warn("nvme: controller fatal status\n");
        return 0;
    }

    /* Request exactly one I/O submission + one completion queue. */
    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_OPC_SET_FEATURES;
    cmd.cdw10  = NVME_FID_NUM_QUEUES;
    cmd.cdw11  = 0;                    /* (0+1) SQ, (0+1) CQ */
    if (nvme_submit_admin(&cmd, NULL) < 0)
        return 0;

    /* Create I/O completion queue (QID 1, 16-byte entries).
     * Per NVMe 1.4: DW10 = CQID | (QSIZE-1)<<16, DW11 = PC|IEN|IV<<16.
     * Polling, so IEN=0. DW12 is reserved. */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode  = NVME_OPC_CREATE_IOCQ;
    cmd.cdw10   = 1 | ((NVME_IO_Q - 1) << 16);   /* CQID | QSIZE - 1 */
    cmd.cdw11   = 1;                             /* PC = physically contiguous */
    cmd.prp1    = (uintptr_t)io_cq;
    if (nvme_submit_admin(&cmd, NULL) < 0)
        return 0;

    /* Create I/O submission queue (QID 1, 64-byte entries).
     * Per NVMe 1.4: DW10 = SQID | (QSIZE-1)<<16, DW11 = PC|QPRIO|CQID<<16. */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode  = NVME_OPC_CREATE_IOSQ;
    cmd.cdw10   = 1 | ((NVME_IO_Q - 1) << 16);   /* SQID | QSIZE - 1 */
    cmd.cdw11   = (1 | (1 << 1)) | (1 << 16);    /* PC | QPRIO=high | CQID=1 */
    cmd.prp1    = (uintptr_t)io_sq;
    if (nvme_submit_admin(&cmd, NULL) < 0)
        return 0;

    /* Identify Controller (CNS=1) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_OPC_IDENTIFY;
    cmd.cdw10  = NVME_CNS_CONTROLLER;
    cmd.prp1   = (uintptr_t)nvme_id_ctrl;
    if (nvme_submit_admin(&cmd, NULL) < 0)
        return 0;
    memcpy(nvme_model, nvme_id_ctrl + 24, 40);       /* MN: 40 bytes */
    nvme_model[39] = '\0';
    memcpy(nvme_fw,   nvme_id_ctrl + 64, 8);          /* FR: 8 bytes */
    nvme_fw[7] = '\0';

    /* Identify Namespace 1 (CNS=0) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_OPC_IDENTIFY;
    cmd.nsid   = 1;
    cmd.cdw10  = NVME_CNS_NAMESPACE;
    cmd.prp1   = (uintptr_t)nvme_id_ns;
    if (nvme_submit_admin(&cmd, NULL) < 0)
        return 0;

    /* namespace size lives at dword 0 (8 bytes) */
    uint64_t nsze    = *(volatile uint64_t*)nvme_id_ns;
    uint32_t flbas   = *(volatile uint32_t*)(nvme_id_ns + 24) & 0xF;
    uint8_t  lbads   = nvme_id_ns[128 + flbas * 4 + 2];   /* lbaf: ms(2) lbads(1) */

    if (lbads != 9) {           /* we require 512-byte logical blocks */
        pr_warn("nvme: namespace block size 2^%u unsupported (512 only)\n",
                (unsigned)lbads);
        return 0;
    }

    nvme_capacity = nsze;
    nvme_blk_size = 512;
    nvme_present  = 1;

    pr_info("nvme: %s (FW %s), %u sectors (%u MB), 512-byte blocks\n",
            nvme_model, nvme_fw,
            (unsigned)(nsze > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)nsze),
            (unsigned)(nsze * 512u / 1024u / 1024u));
    return 1;
}

int nvme_is_present(void)  { return nvme_present; }
int nvme_has_drive(void)   { return nvme_present && nvme_capacity > 0; }

/* ========================================================================
 * I/O: read / write up to 8 sectors (4096 bytes) into the page-aligned
 * bounce buffer - single PRP covers the whole transfer.
 * ======================================================================== */

static int nvme_io_request(int write, uint64_t lba, uint32_t count) {
    if (!nvme_present || count == 0) return 0;
    if (count > 8) count = 8;

    struct nvme_command cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = write ? NVME_OPC_WRITE : NVME_OPC_READ;
    cmd.nsid   = 1;
    cmd.cdw10  = (uint32_t)lba;                 /* SLBA low  */
    cmd.cdw11  = (uint32_t)(lba >> 32);         /* SLBA high */
    cmd.cdw12  = (count - 1) & 0xFFFF;          /* NLB (0-based) */
    cmd.prp1   = (uintptr_t)nvme_io_buf;

    if (nvme_submit_io(&cmd, NULL) < 0)
        return 0;
    return (int)count;
}

int nvme_read_sectors(uint64_t lba, uint32_t count, void* buf) {
    int n = nvme_io_request(0, lba, count);
    if (n <= 0) return 0;
    memcpy(buf, nvme_io_buf, (uint32_t)n * 512);
    return n;
}

int nvme_write_sectors(uint64_t lba, uint32_t count, const void* buf) {
    if (!nvme_present || count == 0) return 0;
    if (count > 8) count = 8;
    memcpy(nvme_io_buf, buf, (uint32_t)count * 512);
    return nvme_io_request(1, lba, count);
}