/*
 * Lestra OS - IDT Setup (x86_64)
 * Copyright (c) 2026 lestramk.org
 */

#include <lestra/types.h>
#include <lestra/gdt.h>
#include <lestra/idt.h>
#include <lestra/irq.h>
#include <lestra/printk.h>
#include <lestra/panic.h>
#include <lestra/vga.h>
#include <lestra/mm.h>

/* IDT entries and pointer */
static struct idt_entry idt_entries[IDT_ENTRIES];
static struct idt_ptr idt_pointer;

/* Custom interrupt handlers */
static interrupt_handler_t interrupt_handlers[IDT_ENTRIES];

/* ISR stubs from assembly */
extern void* isr_stubs[];

/* Exception names */
static const char* exception_names[] = {
    "Divide by Zero",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 FPU Error",
    "Alignment Check",
    "Machine Check",
    "SIMD Exception",
    "Virtualization Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Security Exception",
    "Reserved"
};

void idt_set_gate(uint8_t vector, void* handler, uint8_t type, uint8_t dpl) {
    uint64_t addr = (uint64_t)handler;
    
    idt_entries[vector].offset_low = addr & 0xFFFF;
    idt_entries[vector].offset_mid = (addr >> 16) & 0xFFFF;
    idt_entries[vector].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt_entries[vector].selector = KERNEL_CS;
    idt_entries[vector].ist = 0;
    idt_entries[vector].type_attr = IDT_ATTR_PRESENT | type | dpl;
    idt_entries[vector].reserved = 0;
}

void idt_reload(void) {
    idt_pointer.limit = sizeof(idt_entries) - 1;
    idt_pointer.base = (uint64_t)&idt_entries;
    __asm__ volatile("lidt %0" : : "m"(idt_pointer));
}

void register_interrupt_handler(uint8_t vector, interrupt_handler_t handler) {
    interrupt_handlers[vector] = handler;
}

/* Default exception handler */
static void default_exception_handler(struct interrupt_frame* frame) {
    uint8_t vector = frame->int_no & 0xFF;
    
    vga_set_color(VGA_WHITE, VGA_RED);
    printk("\n!!! EXCEPTION: %s (vector %d) !!!\n",
           vector < 32 ? exception_names[vector] : "Unknown", vector);
    printk("  RIP: 0x%p  CS: 0x%x  RFLAGS: 0x%x\n",
           (void*)frame->rip, frame->cs, frame->rflags);
    printk("  RAX: 0x%p  RBX: 0x%p  RCX: 0x%p\n",
           (void*)frame->rax, (void*)frame->rbx, (void*)frame->rcx);
    printk("  RDX: 0x%p  RSI: 0x%p  RDI: 0x%p\n",
           (void*)frame->rdx, (void*)frame->rsi, (void*)frame->rdi);
    printk("  RBP: 0x%p  RSP: 0x%p  ERR: 0x%x\n",
           (void*)frame->rbp, (void*)frame->rsp, frame->err_code);

    /* KE-36 DIAG: print the captured return address before/after CR3
     * switch (set by context_switch.asm ISR-swap mode). This helps
     * diagnose the #GP where context_switch's ret pops a corrupted
     * return address. */
    {
        extern uint64_t g_ke36_ret_before_cr3, g_ke36_ret_after_cr3, g_ke36_ret_at_ret;
        printk("  KE-36: ret_before_cr3=0x%p ret_after_cr3=0x%p ret_at_ret=0x%p\n",
               (void*)g_ke36_ret_before_cr3, (void*)g_ke36_ret_after_cr3,
               (void*)g_ke36_ret_at_ret);
    }

    if (vector == ISR_PAGE_FAULT) {
        /* Read faulting address from CR2 before any other operation
         * can clobber it. Then delegate to the comprehensive
         * page_fault_handler in kernel/mm/page_fault.c which
         * handles COW resolution, stack growth, and demand paging.
         * If the handler returns, iretq retries the faulting
         * instruction with the newly-mapped/resolved page. */
        uint64_t fault_addr = read_cr2();
        page_fault_handler((uintptr_t)fault_addr, frame->err_code, frame);
        return;
    }
    
    panic("Unhandled exception");
}

/* Default IRQ handler */
static void default_irq_handler(struct interrupt_frame* frame) {
    uint8_t irq = frame->int_no - 32;
    pic_send_eoi(irq);
    
    /* Call registered handler if any */
    if (interrupt_handlers[frame->int_no]) {
        interrupt_handlers[frame->int_no](frame);
    }
}

/* Main interrupt dispatcher (called from assembly stubs) */
/* KE-26: IRQ/interrupt counter for /proc/interrupts.
 * Counts how many times each vector (0-255) has fired. */
uint64_t irq_counts[256] = {0};

void interrupt_dispatch(struct interrupt_frame* frame) {
    uint8_t vector = frame->int_no;

    /* KE-26: Count this interrupt */
    if (vector < 256) irq_counts[vector]++;

    if (vector < 32) {
        /* CPU exception */
        if (interrupt_handlers[vector]) {
            interrupt_handlers[vector](frame);
        } else {
            default_exception_handler(frame);
        }
    } else if (vector < 48) {
        /* IRQ */
        if (interrupt_handlers[vector]) {
            interrupt_handlers[vector](frame);
        }
        pic_send_eoi(vector - 32);
    } else {
        /* Custom interrupt */
        if (interrupt_handlers[vector]) {
            interrupt_handlers[vector](frame);
        }
    }
}

void idt_init(void) {
    /* Clear handler table */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        interrupt_handlers[i] = NULL;
    }
    
    /* Set up exception handlers (vectors 0-31) */
    for (int i = 0; i < 32; i++) {
        idt_set_gate(i, isr_stubs[i], IDT_TYPE_INTERRUPT, IDT_ATTR_RING0);
    }
    /* FIX: #DF (vector 8) must use IST1 - otherwise kernel stack overflow triple faults */
    idt_entries[ISR_DOUBLE_FAULT].ist = 1;
    
    /* Set up IRQ handlers (vectors 32-47) */
    for (int i = 32; i < 48; i++) {
        idt_set_gate(i, isr_stubs[i], IDT_TYPE_INTERRUPT, IDT_ATTR_RING0);
    }
    
    idt_reload();
    pr_debug("IDT initialized with %d entries\n", IDT_ENTRIES);
}
