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

/* Segment selectors
 *
 * KE-26: GDT layout follows the AMD64 syscall/sysret convention so that
 * SYSRET can load CS/SS correctly. The ABI requires:
 *   - syscall:  CS = STAR[47:32],            SS = STAR[47:32] + 8
 *   - sysret:   CS = (STAR[63:48] + 16) | 3,  SS = (STAR[63:48] + 8) | 3
 * For sysret CS to land on USER_CS and SS on USER_DS, the GDT MUST have
 *   KERNEL_CS, KERNEL_DS = KERNEL_CS+8, USER_DS = KERNEL_CS+16, USER_CS = KERNEL_CS+24
 * i.e. USER_DS immediately precedes USER_CS. The previous layout had
 * USER_CS before USER_DS, which made sysret load SS = KERNEL_DS|RPL3
 * (a ring-0 data selector with RPL=3) — the CPU then ran /init's .text
 * in ring 0, triggering SMEP #PF / SMAP #PF / triple-fault.
 *
 * With STAR[63:48] = KERNEL_DS (0x10):
 *   sysret CS = (0x10 + 16) | 3 = 0x23 (USER_CS | RPL3)
 *   sysret SS = (0x10 + 8)  | 3 = 0x1B (USER_DS | RPL3)
 */
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_DS   0x18   /* slot 3 — MUST precede USER_CS for sysret */
#define USER_CS   0x20   /* slot 4 — USER_DS + 8 */
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

/* Set the RSP0 field in the TSS.
 *
 * RSP0 is the stack pointer the CPU loads when transitioning from ring 3
 * to ring 0 via an interrupt gate with IST=0 (which is all of IRQ vectors
 * 32-47). If RSP0 is 0 or points to unmapped memory when an interrupt
 * fires in ring 3, the CPU cannot push the interrupt frame → #DF →
 * triple fault → silent reboot (no handler prints anything).
 *
 * This MUST be set to a valid kernel stack top before ANY code runs in
 * ring 3 — i.e. before elf_jump_to_user() / context_switch to a user
 * process — and re-set on every context switch to a user process. */
void tss_set_rsp0(uint64_t rsp0);

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
