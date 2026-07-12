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

/* GDT access flags
 * Bit layout of access byte:
 *   7  P  (Present)
 *   6-5 DPL (Descriptor Privilege Level: 00=ring0, 11=ring3)
 *   4  S  (0=system, 1=code/data)
 *   3  E  (Executable: 1=code, 0=data)
 *   2  DC (Direction/Conforming)
 *   1  RW (Readable/Writable)
 *   0  A  (Accessed - set by CPU)
 * FIX: S bit (0x10) must be set for code/data segments, otherwise
 * the CPU treats the descriptor as a system segment (TSS, LDT, gate). */
#define GDT_ACCESS_PRESENT     0x80
#define GDT_ACCESS_RING0       0x00
#define GDT_ACCESS_RING3       0x60
#define GDT_ACCESS_S           0x10  /* Code/data (not system) */
#define GDT_ACCESS_CODE        0x18  /* S + E */
#define GDT_ACCESS_DATA        0x10  /* S only */
#define GDT_ACCESS_EXECUTABLE  0x08
#define GDT_ACCESS_RW          0x02
#define GDT_ACCESS_ACCESSED    0x01

/* GDT granularity flags */
#define GDT_GRAN_4K    0x80
#define GDT_GRAN_32BIT 0x40
#define GDT_GRAN_64BIT 0x20

void gdt_init(void);
void gdt_set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran);

/* --- Task State Segment (long mode) ---
 * PR #6 fix: GDT was missing a TSS descriptor. Without one:
 *  - syscall entry does NOT swap to a kernel stack (no rsp0 load),
    so user-mode ROP can clobber the kernel stack during syscall.
 *  - #DF (double fault), #NMI, #DB, etc. cannot use a separate
    stack via IST, so a kernel stack overflow turns into a
    triple fault and immediate reboot.
 */
struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __packed;

/* TSS descriptor flags (system segment, S bit = 0) */
#define TSS_TYPE_AVAILABLE  0x9  /* 64-bit TSS (available) */
#define TSS_TYPE_BUSY       0xB  /* 64-bit TSS (busy) */

/* Update gdt_entries array size: 5 (code/data) + 1 (TSS) = 6 */
#endif /* LESTRA_GDT_H */
