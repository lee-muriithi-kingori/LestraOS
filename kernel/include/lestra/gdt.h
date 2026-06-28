/*
 * Lestra OS - GDT (Global Descriptor Table)
 * Copyright (c) 2026 lestramk.org
 */

#ifndef LESTRA_GDT_H
#define LESTRA_GDT_H

#include <lestra/types.h>

/* GDT entry structure */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __packed;

/* GDT pointer */
struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __packed;

/* Segment selectors */
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_CS   0x18
#define USER_DS   0x20
#define TSS_SEG   0x28

/* GDT access flags */
#define GDT_ACCESS_PRESENT     0x80
#define GDT_ACCESS_RING0       0x00
#define GDT_ACCESS_RING3       0x60
#define GDT_ACCESS_CODE        0x18
#define GDT_ACCESS_DATA        0x10
#define GDT_ACCESS_EXECUTABLE  0x08
#define GDT_ACCESS_RW          0x02

/* GDT granularity flags */
#define GDT_GRAN_4K    0x80
#define GDT_GRAN_32BIT 0x40
#define GDT_GRAN_64BIT 0x20

void gdt_init(void);
void gdt_set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran);
void gdt_reload(uint16_t cs, uint16_t ds);

#endif /* LESTRA_GDT_H */
