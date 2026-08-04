/*
 * Lestra OS - GDT Setup (x86_64)
 * Copyright (c) 2026 lestramk.org
 */

#include <lestra/types.h>
#include <lestra/gdt.h>
#include <lestra/printk.h>
#include <lestra/panic.h>

/* GDT entries:
 * 0: Null descriptor
 * 1: Kernel code segment (64-bit)
 * 2: Kernel data segment
 * 3: User code segment (64-bit)
 * 4: User data segment
 * 5+6: TSS descriptor (system segment in long mode is 16 bytes / 2 slots)
 *
 * FIX: array was [6] (slots 0..5) but TSS descriptor occupies slots 5 AND 6.
 * Writing the upper half of the TSS descriptor overflowed past the array
 * end, and the GDT limit was too small for `ltr 0x28` to read the full
 * 16-byte descriptor, causing a #GP. Bumping to [7] makes the GDT limit
 * 0x37 = 55, covering all 56 bytes (7 * 8) the CPU may read.
 */
static struct gdt_entry gdt_entries[7];
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
#define IST_STACK_SIZE 8192
static struct tss_entry tss __aligned(16);
static uint8_t ist1_stack[IST_STACK_SIZE] __aligned(16);

static inline void tss_set_entry(int index, struct tss_entry* t) {
    /* System segment descriptor in long mode is 16 bytes spanning two
     * consecutive 8-byte GDT slots. Byte layout of the 16-byte descriptor:
     *
     *   bytes 0..1   limit_low      (bits 15..0 of limit)
     *   bytes 2..3   base_low       (bits 15..0 of base)
     *   byte  4      base_mid       (bits 23..16 of base)
     *   byte  5      access         (P/DPL/S/type)
     *   byte  6      granularity    (limit 19..16 + flags)
     *   byte  7      base_high      (bits 31..24 of base)
     *   bytes 8..11  base_upper     (bits 63..32 of base)
     *   bytes 12..15 reserved_zero  (must be 0)
     *
     * struct gdt_entry covers bytes 0..7. slot (index+1) covers bytes 8..15.
     *
     * FIX: previous code had three encoding bugs:
     *  - base_low masked with 0xFFFFFF (only 16 bits fit in struct)
     *  - base_mid got bits 31..24 instead of 23..16
     *  - base_high got bits 39..32 instead of 31..24
     *  - base_upper written to bytes 12..15 instead of bytes 8..11
     * Result: TSS base was corrupted; ltr would #GP. With a 64-bit address
     * like 0x0000000000100000 the previous code would record base as
     * 0x000010000000xxxx — totally bogus.
     */
    uint64_t base = (uint64_t)t;
    uint32_t limit = sizeof(struct tss_entry) - 1;
    /* access byte: P=1, DPL=0 (ring 0), S=0 (system), type=0x9 (64-bit avail TSS) */
    uint8_t access = 0x80 | 0x00 | 0x00 | TSS_TYPE_AVAILABLE;
    uint8_t gran   = 0x00;  /* byte-granular for TSS */

    gdt_entries[index].limit_low   = limit & 0xFFFF;
    gdt_entries[index].base_low    = base & 0xFFFF;
    gdt_entries[index].base_mid    = (base >> 16) & 0xFF;
    gdt_entries[index].access      = access;
    gdt_entries[index].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt_entries[index].base_high   = (base >> 24) & 0xFF;

    /* Upper 32 bits of base go into bytes 8..11 of the 16-byte descriptor,
     * which is bytes 0..3 of the next GDT slot. Bytes 12..15 (4..7 of the
     * next slot) must be zero. */
    uint32_t upper = (uint32_t)(base >> 32);
    *((volatile uint32_t*)&gdt_entries[index + 1])                       = upper;
    *((volatile uint32_t*)(((uintptr_t)&gdt_entries[index + 1]) + 4))    = 0;
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
