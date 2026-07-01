/*
 * Lestra OS - IRQ (Interrupt Request) Handling
 * Copyright (c) 2026 lestramk.org
 */

#ifndef LESTRA_IRQ_H
#define LESTRA_IRQ_H

#include <lestra/types.h>
#include <lestra/idt.h>

/* PIC definitions */
#define PIC1_CMD    0x20
#define PIC1_DATA   0x21
#define PIC2_CMD    0xA0
#define PIC2_DATA   0xA1

#define PIC_EOI     0x20

#define ICW1_INIT   0x10
#define ICW1_ICW4   0x01
#define ICW4_8086   0x01

/* IRQ numbers (remapped to 32-47) */
#define IRQ0   32   /* Programmable Interval Timer */
#define IRQ1   33   /* Keyboard */
#define IRQ2   34   /* Cascade (used internally) */
#define IRQ3   35   /* COM2 */
#define IRQ4   36   /* COM1 */
#define IRQ5   37   /* LPT2/Sound */
#define IRQ6   38   /* Floppy disk */
#define IRQ7   39   /* LPT1 */
#define IRQ8   40   /* CMOS RTC */
#define IRQ9   41   /* ACPI/SCI */
#define IRQ10  42   /* Available */
#define IRQ11  43   /* Available */
#define IRQ12  44   /* PS/2 Mouse */
#define IRQ13  45   /* FPU/Coprocessor */
#define IRQ14  46   /* Primary ATA */
#define IRQ15  47   /* Secondary ATA */

void pic_init(void);
void pic_send_eoi(uint8_t irq);
void pic_disable(void);
void irq_enable(uint8_t irq);
void irq_disable(uint8_t irq);
void register_irq_handler(uint8_t irq, interrupt_handler_t handler);

#endif /* LESTRA_IRQ_H */
