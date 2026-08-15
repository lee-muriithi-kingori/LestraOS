/*
 * Lestra OS - IOAPIC Driver (KE-23)
 * Copyright (c) 2026 lestramk.org
 *
 * Routes Global System Interrupts (GSIs) coming in on the IOAPIC's
 * 24 input pins (on QEMU) to CPU vectors via the LAPIC.
 *
 * Each GSI has a 64-bit Redirection Table Entry (split into two 32-bit
 * MMIO registers): the low word holds vector/delivery/polarity/trigger/mask,
 * the high word holds the destination APIC ID.
 *
 * MMIO access pattern: write the register index to IOREGSEL (offset 0),
 * then read/write IOWIN (offset 0x10, 32-bit).
 *
 * The IOAPIC MMIO base lives at 0xFEC00000 (within the boot 4GB identity
 * map), so we dereference it directly like the e1000 MMIO BAR.
 */

#include <lestra/types.h>
#include <lestra/apic.h>
#include <lestra/acpi.h>
#include <lestra/printk.h>

/* File-static IOAPIC MMIO base + GSI base. */
static volatile uint32_t* ioapic_base = NULL;
static uint32_t ioapic_gsi_base = 0;
static int ioapic_max_redir = 0;   /* Number of redirection entries (24 on QEMU). */

/* --- Low-level MMIO register access --- */

static inline volatile uint32_t* ioapic_regsel(void) {
    return (volatile uint32_t*)((uintptr_t)ioapic_base + IOAPIC_REGSEL);
}
static inline volatile uint32_t* ioapic_win(void) {
    return (volatile uint32_t*)((uintptr_t)ioapic_base + IOAPIC_WIN);
}

static uint32_t ioapic_read(uint8_t reg) {
    if (!ioapic_base) return 0;
    *ioapic_regsel() = reg;
    return *ioapic_win();
}

static void ioapic_write(uint8_t reg, uint32_t value) {
    if (!ioapic_base) return;
    *ioapic_regsel() = reg;
    *ioapic_win() = value;
}

int ioapic_init(uint32_t ioapic_mmio_base, uint32_t gsi_base) {
    ioapic_base = (volatile uint32_t*)(uintptr_t)ioapic_mmio_base;
    ioapic_gsi_base = gsi_base;

    /* Read IOAPIC ID (register 0). Bits 24-27 hold the 4-bit ID. */
    uint32_t id_reg = ioapic_read(IOAPIC_ID);
    uint8_t id = (id_reg >> 24) & 0x0F;

    /* Read IOAPIC version (register 1).
     * Bits 0-7  : version
     * Bits 8-15 : reserved
     * Bits 16-23: max redirection entry index (N-1, so +1 for count) */
    uint32_t ver = ioapic_read(IOAPIC_VER);
    ioapic_max_redir = ((ver >> 16) & 0xFF) + 1;

    pr_info("apic: IOAPIC enabled at 0x%x — id=%u, version=0x%x, %d redirection entries, GSI base=%u\n",
            ioapic_mmio_base, id, ver & 0xFF, ioapic_max_redir, ioapic_gsi_base);

    /* Mask ALL redirection entries. We write the low word with MASKED
     * set and vector 0 (the vector is irrelevant while masked). The
     * high word (destination) is cleared to 0. */
    for (int i = 0; i < ioapic_max_redir; i++) {
        ioapic_write(IOAPIC_REDTBL_HI(i), 0);
        ioapic_write(IOAPIC_REDTBL_LO(i),
                     IOAPIC_RED_DELIV_FIXED | IOAPIC_RED_DEST_PHYS |
                     IOAPIC_RED_POLAR_HIGH | IOAPIC_RED_TRIGGER_EDGE |
                     IOAPIC_RED_MASKED);
    }

    return 0;
}

int ioapic_num_redir(void) {
    return ioapic_max_redir;
}

/* Convert a GSI to a redirection-table index. */
static int gsi_to_index(uint32_t gsi) {
    if (gsi < ioapic_gsi_base) return -1;
    int idx = (int)(gsi - ioapic_gsi_base);
    if (idx >= ioapic_max_redir) return -1;
    return idx;
}

int ioapic_route_gsi(uint32_t gsi, uint8_t vector,
                     const struct ioapic_irq_policy* policy,
                     uint8_t dest_apic, int unmask) {
    int idx = gsi_to_index(gsi);
    if (idx < 0) {
        pr_warn("apic: GSI %u out of IOAPIC range (base=%u, max=%d)\n",
                gsi, ioapic_gsi_base, ioapic_max_redir);
        return -1;
    }

    /* Assemble the low 32 bits of the redirection entry. */
    uint32_t low = (uint32_t)vector & IOAPIC_RED_VECTOR_MASK;
    low |= IOAPIC_RED_DELIV_FIXED;      /* fixed delivery to one CPU */
    low |= IOAPIC_RED_DEST_PHYS;        /* physical destination mode */
    if (policy && policy->active_low)
        low |= IOAPIC_RED_POLAR_LOW;
    else
        low |= IOAPIC_RED_POLAR_HIGH;
    if (policy && policy->level_triggered)
        low |= IOAPIC_RED_TRIGGER_LEVEL;
    else
        low |= IOAPIC_RED_TRIGGER_EDGE;
    if (!unmask)
        low |= IOAPIC_RED_MASKED;

    /* High 32 bits: destination APIC ID in bits 24-31 (physical mode). */
    uint32_t high = ((uint32_t)dest_apic) << 24;

    /* Program high first, then low. Writing low with MASKED clear arms it.
     * Per Intel SDM 3.12.6, always mask before reprogramming an entry that
     * may already be in use, to avoid a spurious IRQ mid-write. */
    uint32_t cur_lo = ioapic_read(IOAPIC_REDTBL_LO(idx));
    ioapic_write(IOAPIC_REDTBL_LO(idx), cur_lo | IOAPIC_RED_MASKED);
    ioapic_write(IOAPIC_REDTBL_HI(idx), high);
    ioapic_write(IOAPIC_REDTBL_LO(idx), low);

    return 0;
}

int ioapic_mask_gsi(uint32_t gsi) {
    int idx = gsi_to_index(gsi);
    if (idx < 0) return -1;
    uint32_t low = ioapic_read(IOAPIC_REDTBL_LO(idx));
    low |= IOAPIC_RED_MASKED;
    ioapic_write(IOAPIC_REDTBL_LO(idx), low);
    return 0;
}

int ioapic_unmask_gsi(uint32_t gsi) {
    int idx = gsi_to_index(gsi);
    if (idx < 0) return -1;
    uint32_t low = ioapic_read(IOAPIC_REDTBL_LO(idx));
    low &= ~IOAPIC_RED_MASKED;
    ioapic_write(IOAPIC_REDTBL_LO(idx), low);
    return 0;
}
