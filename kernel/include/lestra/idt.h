/*
 * Lestra OS - IDT (Interrupt Descriptor Table)
 * Copyright (c) 2026 lestramk.org
 */

#ifndef LESTRA_IDT_H
#define LESTRA_IDT_H

#include <lestra/types.h>

/* IDT entry (64-bit interrupt gate) */
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __packed;

/* IDT pointer */
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __packed;

/* IDT types */
#define IDT_TYPE_INTERRUPT  0x0E  /* Interrupt gate */
#define IDT_TYPE_TRAP       0x0F  /* Trap gate */
#define IDT_TYPE_TASK       0x05  /* Task gate */

#define IDT_ATTR_PRESENT    0x80
#define IDT_ATTR_RING0      0x00
#define IDT_ATTR_RING3      0x60

/* Exception numbers */
#define ISR_DIV_ZERO        0
#define ISR_DEBUG           1
#define ISR_NMI             2
#define ISR_BREAKPOINT      3
#define ISR_OVERFLOW        4
#define ISR_BOUND_RANGE     5
#define ISR_INVALID_OPCODE  6
#define ISR_DEVICE_NA       7
#define ISR_DOUBLE_FAULT    8
#define ISR_INVALID_TSS     10
#define ISR_SEG_NOT_PRESENT 11
#define ISR_STACK_FAULT     12
#define ISR_GP_FAULT        13
#define ISR_PAGE_FAULT      14
#define ISR_FPE             16
#define ISR_ALIGNMENT       17
#define ISR_MACHINE_CHECK   18
#define ISR_SIMD_FPE        19
#define ISR_VIRTUALIZATION  20
#define ISR_SECURITY        30

#define IDT_ENTRIES         256

/* Interrupt handler function type */
typedef void (*interrupt_handler_t)(struct interrupt_frame* frame);

/* Interrupt frame passed to handlers */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __packed;

void idt_init(void);
void idt_set_gate(uint8_t vector, void* handler, uint8_t type, uint8_t dpl);
void idt_reload(void);
void register_interrupt_handler(uint8_t vector, interrupt_handler_t handler);

/* ISR assembly stubs */
extern void* isr_stubs[];

#endif /* LESTRA_IDT_H */
