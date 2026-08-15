/*
 * Lestra OS - IRQ Controller Abstraction (8259 PIC + APIC backends)
 * Copyright (c) 2026 lestramk.org
 *
 * This file is the single dispatch point for the kernel's IRQ API.
 * Drivers call register_irq_handler() / irq_enable() / irq_disable()
 * and the EOI path (pic_send_eoi, called from the IDT dispatcher)
 * regardless of which controller is active.
 *
 * KE-23: when apic_init() promoted the controller to APIC, these
 * functions transparently switch from 8259 PIC masks/EOI to IOAPIC
 * redirection+mask / LAPIC EOI. No driver needs to change.
 */

#include <lestra/types.h>
#include <lestra/irq.h>
#include <lestra/idt.h>
#include <lestra/acpi.h>
#include <lestra/apic.h>
#include <lestra/printk.h>

/* Custom IRQ handlers (indexed by ISA IRQ 0-15). */
static interrupt_handler_t irq_handlers[16];

void pic_init(void) {
    /* Save masks */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* ICW1: Start initialization, cascade mode, ICW4 needed */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);

    /* ICW2: Remap vectors - master to 32, slave to 40 */
    outb(PIC1_DATA, 32);
    outb(PIC2_DATA, 40);

    /* ICW3: Cascade identity */
    outb(PIC1_DATA, 4);   /* Slave at IRQ2 */
    outb(PIC2_DATA, 2);   /* Cascade identity */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    /* Restore masks */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);

    /* Clear handlers */
    for (int i = 0; i < 16; i++) {
        irq_handlers[i] = NULL;
    }

    pr_debug("PIC initialized, IRQs remapped to 32-47\n");
}

void pic_send_eoi(uint8_t irq) {
    /* KE-23: in APIC mode, the interrupt was delivered by the LAPIC,
     * so we ack the LAPIC (write EOI register). The 8259 PIC is masked
     * and inert — we do NOT send it an EOI (doing so on a disabled PIC
     * is harmless but pointless, and on some QEMU configs the cascade
     * PIC EOI can cause spurious IRQ7 storms). */
    if (irq_using_apic()) {
        lapic_eoi();
        return;
    }

    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}

void pic_disable(void) {
    /* Mask all interrupts on both PICs. Used by apic_init() to render
     * the 8259 inert once the IOAPIC takes over. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void irq_enable(uint8_t irq) {
    if (irq_using_apic()) {
        /* Unmask the IOAPIC redirection entry for this ISA IRQ's GSI. */
        uint32_t gsi = acpi_isa_irq_to_gsi(irq);
        ioapic_unmask_gsi(gsi);
        return;
    }

    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t mask = inb(port);
    mask &= ~(1 << (irq & 7));
    outb(port, mask);
}

void irq_disable(uint8_t irq) {
    if (irq_using_apic()) {
        /* Mask the IOAPIC redirection entry for this ISA IRQ's GSI. */
        uint32_t gsi = acpi_isa_irq_to_gsi(irq);
        ioapic_mask_gsi(gsi);
        return;
    }

    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t mask = inb(port);
    mask |= (1 << (irq & 7));
    outb(port, mask);
}

/* Convert ACPI MADT IntSrcOverride flags (polarity/trigger) into an
 * IOAPIC policy struct.
 *   polarity:   0/1 = conforming/high (treat as active-high),
 *               3 = active-low
 *   trigger:    0/1 = conforming/edge (treat as edge),
 *               3 = level
 * ISA default (no override, flags==0) is active-high edge. */
static void flags_to_policy(uint16_t flags, struct ioapic_irq_policy* p) {
    p->active_low = 0;
    p->level_triggered = 0;
    switch (flags & 0x3) {
        case 3: p->active_low = 1; break;          /* active low */
        case 0: case 1: default: break;            /* conforming/high → active high */
    }
    switch ((flags >> 2) & 0x3) {
        case 3: p->level_triggered = 1; break;     /* level */
        case 0: case 1: default: break;            /* conforming/edge → edge */
    }
}

void register_irq_handler(uint8_t irq, interrupt_handler_t handler) {
    if (irq >= 16) return;
    irq_handlers[irq] = handler;

    /* Register in the IDT dispatch table (vector = irq + 32).
     * The IDT gate itself was already installed by idt_init() for
     * vectors 32-47, so this just sets the C function pointer. */
    register_interrupt_handler(irq + 32, handler);

    if (irq_using_apic()) {
        /* Route the ISA IRQ through the IOAPIC.
         *   1. Map ISA IRQ -> GSI (uses MADT IntSrcOverride; identity if none)
         *   2. Look up the override flags for polarity/trigger
         *   3. Program the IOAPIC redirection entry: GSI -> vector (irq+32),
         *      physical destination = this CPU's LAPIC ID, unmasked
         * After this, the device's interrupt line will be delivered by the
         * IOAPIC straight to the LAPIC on this CPU, and the IDT dispatches
         * it to `handler`. */
        uint32_t gsi = acpi_isa_irq_to_gsi(irq);
        uint16_t flags = acpi_isa_irq_flags(irq);
        struct ioapic_irq_policy policy;
        flags_to_policy(flags, &policy);

        uint8_t dest = lapic_get_id();   /* deliver to BSP (self) */
        uint8_t vector = (uint8_t)(irq + 32);

        if (ioapic_route_gsi(gsi, vector, &policy, dest, 1) == 0) {
            pr_debug("apic: routed ISA IRQ %u -> GSI %u -> vector %u (dest LAPIC %u, %s/%s)\n",
                     irq, gsi, vector, dest,
                     policy.level_triggered ? "level" : "edge",
                     policy.active_low ? "low" : "high");
        }
    }
}
