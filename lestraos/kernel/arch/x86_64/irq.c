/*
 * Lestra OS - PIC8259 IRQ Controller
 * Copyright (c) 2026 lestramk.org
 */

#include <lestra/types.h>
#include <lestra/irq.h>
#include <lestra/idt.h>
#include <lestra/printk.h>

/* Custom IRQ handlers */
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
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}

void pic_disable(void) {
    /* Mask all interrupts on both PICs */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void irq_enable(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t mask = inb(port);
    mask &= ~(1 << (irq & 7));
    outb(port, mask);
}

void irq_disable(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t mask = inb(port);
    mask |= (1 << (irq & 7));
    outb(port, mask);
}

void register_irq_handler(uint8_t irq, interrupt_handler_t handler) {
    if (irq < 16) {
        irq_handlers[irq] = handler;
        /* Register in IDT too */
        register_interrupt_handler(irq + 32, handler);
    }
}
