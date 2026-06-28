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
static struct gdt_entry gdt_entries[5];
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

void gdt_init(void) {
    /* Null descriptor */
    gdt_set_entry(0, 0, 0, 0, 0);
    
    /* Kernel code segment (64-bit, ring 0) */
    gdt_set_entry(1, 0, 0xFFFFF,
                  GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 |
                  GDT_ACCESS_EXECUTABLE | GDT_ACCESS_RW,
                  GDT_GRAN_4K | GDT_GRAN_64BIT);
    
    /* Kernel data segment (ring 0) */
    gdt_set_entry(2, 0, 0xFFFFF,
                  GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 |
                  GDT_ACCESS_RW,
                  GDT_GRAN_4K | GDT_GRAN_64BIT);
    
    /* User code segment (64-bit, ring 3) */
    gdt_set_entry(3, 0, 0xFFFFF,
                  GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 |
                  GDT_ACCESS_EXECUTABLE | GDT_ACCESS_RW,
                  GDT_GRAN_4K | GDT_GRAN_64BIT);
    
    /* User data segment (ring 3) */
    gdt_set_entry(4, 0, 0xFFFFF,
                  GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 |
                  GDT_ACCESS_RW,
                  GDT_GRAN_4K | GDT_GRAN_64BIT);
    
    gdt_pointer.limit = sizeof(gdt_entries) - 1;
    gdt_pointer.base = (uint64_t)&gdt_entries;
    
    gdt_flush((uint64_t)&gdt_pointer, KERNEL_CS, KERNEL_DS);
    pr_debug("GDT initialized with 5 entries\n");
}
