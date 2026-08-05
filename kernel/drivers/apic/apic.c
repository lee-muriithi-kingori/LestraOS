/*
 * Lestra OS - APIC Subsystem Orchestrator (KE-23)
 * Copyright (c) 2026 lestramk.org
 *
 * Top-level entry point. Decides at boot whether to drive interrupts
 * with the Local APIC + IOAPIC (preferred) or fall back to the legacy
 * 8259 PIC (when ACPI MADT is unavailable).
 *
 * Must be called AFTER acpi_init() (which populates g_acpi) and BEFORE
 * any driver calls register_irq_handler() (so the very first registration
 * can program an IOAPIC redirection entry instead of a PIC mask).
 */

#include <lestra/types.h>
#include <lestra/apic.h>
#include <lestra/acpi.h>
#include <lestra/irq.h>
#include <lestra/printk.h>

/* The active controller. PIC until apic_init() promotes it. */
static enum irq_controller g_irq_controller = IRQ_CONTROLLER_PIC;

enum irq_controller irq_controller_get(void) {
    return g_irq_controller;
}

int irq_using_apic(void) {
    return g_irq_controller == IRQ_CONTROLLER_APIC;
}

int apic_init(void) {
    /* We need the MADT to know where the LAPIC and IOAPIC live. */
    if (!g_acpi.madt_found) {
        pr_warn("apic: no ACPI MADT — staying on legacy 8259 PIC\n");
        return -1;
    }

    uint32_t lapic_base  = g_acpi.lapic_addr  ? g_acpi.lapic_addr  : LAPIC_DEFAULT_BASE;
    uint32_t ioapic_base = g_acpi.ioapic_addr ? g_acpi.ioapic_addr : IOAPIC_DEFAULT_BASE;
    uint32_t gsi_base    = g_acpi.ioapic_gsi_base;

    pr_info("apic: initializing APIC subsystem (LAPIC=0x%x, IOAPIC=0x%x, gsi_base=%u)\n",
            lapic_base, ioapic_base, gsi_base);

    /* Bring up the Local APIC first (it owns EOI / spurious vector). */
    if (lapic_init(lapic_base) != 0) {
        pr_warn("apic: LAPIC init failed — staying on PIC\n");
        return -1;
    }

    /* Bring up the IOAPIC (masks all redirection entries on init). */
    if (ioapic_init(ioapic_base, gsi_base) != 0) {
        pr_warn("apic: IOAPIC init failed — staying on PIC\n");
        return -1;
    }

    /* Disable the legacy 8259 PIC. All 16 IRQ lines are masked so the
     * PIC can't fire into the LAPIC's LINT0/ExtINT path. We keep the
     * PIC initialized (so pic_send_eoi is still safe to call as a no-op
     * fallback path), but it is now inert. */
    pic_disable();

    /* Promote the controller. From this point, register_irq_handler /
     * irq_enable / irq_disable / pic_send_eoi all branch on this flag
     * and talk to the IOAPIC+LAPIC instead of the PIC. */
    g_irq_controller = IRQ_CONTROLLER_APIC;

    pr_info("apic: APIC subsystem active — PIC disabled, interrupts routed via IOAPIC+LAPIC\n");
    return 0;
}
