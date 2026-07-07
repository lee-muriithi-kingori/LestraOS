/*
 * Lestra OS - GDT Setup (x86_64)
 * Copyright (c) 2026 lestramk.org
 */

#include <lestra/types.h>
#include <lestra/gdt.h>
#include <lestra/printk.h>
#include <lestra/panic.h>

/* GDT entries - 5 entries:
 * 0: Null descriptor
 * 1: Kernel code segment (64-bit)
 * 2: Kernel data segment
 * 3: User code segment (64-bit)
 * 4: User data segment
 */
static struct gdt_entry gdt_entries[6];  /* +1 for TSS (PR #6 fix) */
static struct gdt_ptr gdt_pointer;

extern void gdt_flush(uint64_t gdt_ptr, uint16_t cs, uint16_t ds);

void gdt_set_entry(int index, uint32_t base, uint32_t limit,
                    uint8_t access, uint8_t gran) {
    gdt_entries[index].base_low = base & 0xFFFF;
    gdt_entries[index].base_mid = (base >> 16) & 0xFF;
    gdt_entries[index].base_high = (base >> 24) & 0xFF;
    gdt_entries[index].limit_low = limit & 0xFFFF;
    gdt_entries[index].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt_entries[index].access = access;
}

/* PR #6 fix: TSS state + dedicated stack for IST1 (#DF handler).
 * Sized so that a #DF (e.g., kernel stack overflow on syscall)
 lands on its own stack instead of dying as a triple fault. */
#define IST_STACK_SIZE 4096
static struct tss_entry tss __aligned(16);
static uint8_t ist1_stack[IST_STACK_SIZE] __aligned(16);

static inline void tss_set_entry(int index, struct tss_entry* t) {
    /* System segment descriptor in long mode is 16 bytes:
     * bytes  0..7  : limit_low(16) | base_low(24) | type(4) | S(1)=0 | DPL(2) | P(1)
     * bytes  8..15 : base_mid(8) | granularity(4) | base_high(8) | reserved(8) | base_upper(32)
     * The descriptor takes 2 GDT slots. */
    uint64_t base = (uint64_t)t;
    uint32_t limit = sizeof(struct tss_entry) - 1;
    /* access byte: P=1, DPL=0 (ring 0), S=0 (system), type=0x9 (64-bit avail TSS) */
    uint8_t access = 0x80 | 0x00 | 0x00 | TSS_TYPE_AVAILABLE;
    uint8_t gran   = 0x00;  /* byte-granular for TSS */

    gdt_entries[index].limit_low   = limit & 0xFFFF;
    gdt_entries[index].base_low    = base & 0xFFFFFF;
    gdt_entries[index].base_mid    = (base >> 24) & 0xFF;
    gdt_entries[index].access      = access;
    gdt_entries[index].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt_entries[index].base_high   = (base >> 32) & 0xFF;

    /* Upper 32 bits of base go into the next GDT slot (index+1).
     * The 'gdt_entry' struct is only 8 bytes, so we need to
     * write the upper 32 bits into the upper half of slot+1
     * using a packed write. Easiest: cast and write a uint64_t. */
    uint64_t upper = (base >> 32) & 0xFFFFFFFF;
    /* The 'upper' half lives at offset 12..16 of the 16-byte descriptor.
     * struct gdt_entry is 8 bytes; we place the upper in slot (index+1)
     * as a uint64_t overlay starting at limit_low+4 = base_high byte. */
    *((volatile uint32_t*)(((uintptr_t)&gdt_entries[index + 1]) + 4)) = (uint32_t)upper;
}

/* LTR: load task register. After this, IST stacks are active
 * and ring transitions load rsp0 from the TSS. */
static inline void ltr_load(uint16_t sel) {
    __asm__ volatile("ltr %0" : : "r"(sel) : "memory");
}

void gdt_init(void) {
    /* Null descriptor */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* Kernel code segment (64-bit, ring 0)
     * FIX: added GDT_ACCESS_S (0x10) - without S bit, CPU treats this
     * as a system segment (TSS/gate) and the long-mode far jump will #GP. */
    gdt_set_entry(1, 0, 0xFFFFF,
                  GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 |
                  GDT_ACCESS_S | GDT_ACCESS_EXECUTABLE | GDT_ACCESS_RW,
                  GDT_GRAN_4K | GDT_GRAN_64BIT);

    /* Kernel data segment (ring 0) */
    gdt_set_entry(2, 0, 0xFFFFF,
                  GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 |
                  GDT_ACCESS_S | GDT_ACCESS_RW,
                  GDT_GRAN_4K | GDT_GRAN_64BIT);

    /* User code segment (64-bit, ring 3) */
    gdt_set_entry(3, 0, 0xFFFFF,
                  GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 |
                  GDT_ACCESS_S | GDT_ACCESS_EXECUTABLE | GDT_ACCESS_RW,
                  GDT_GRAN_4K | GDT_GRAN_64BIT);

    /* User data segment (ring 3) */
    gdt_set_entry(4, 0, 0xFFFFF,
                  GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 |
                  GDT_ACCESS_S | GDT_ACCESS_RW,
                  GDT_GRAN_4K | GDT_GRAN_64BIT);

    gdt_pointer.limit = sizeof(gdt_entries) - 1;
    gdt_pointer.base = (uint64_t)&gdt_entries;

    gdt_flush((uint64_t)&gdt_pointer, KERNEL_CS, KERNEL_DS);

    /* PR #6 fix: load TSS descriptor and tell the CPU to use it. */
    uintptr_t ist_top = (uintptr_t)&ist1_stack[IST_STACK_SIZE];
    tss.rsp0 = 0;  /* updated on each ring transition (see syscall_entry) */
    tss.ist1 = ist_top;
    tss_set_entry(5, &tss);
    ltr_load(TSS_SEG);

    pr_debug("GDT initialized with 6 entries (5 code/data + TSS, IST1 #DF stack)\n");
}
