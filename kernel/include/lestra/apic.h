/*
 * Lestra OS - Local APIC + IOAPIC Subsystem (KE-23)
 * Copyright (c) 2026 lestramk.org
 *
 * Replaces the legacy 8259 PIC as the system interrupt controller when
 * ACPI MADT tables are available. Falls back to PIC otherwise.
 *
 * Architecture:
 *
 *   ISA device (PIT/keyboard/mouse/...)  --ISA IRQ line--+
 *                                                         |
 *   PCI device (MSI/MSI-X) -----------------------------+ |
 *                                                       | |
 *                                                       v v
 *                                                   +-------+
 *                                                   | IOAPIC|  (MMIO @ 0xFEC00000)
 *                                                   +-------+
 *                                                        | GSI (redirection entry -> vector)
 *                                                        v
 *                                                   +-------+
 *                                                   | LAPIC |  (MMIO @ 0xFEE00000)
 *                                                   +-------+
 *                                                        | interrupt on this CPU
 *                                                        v
 *                                                   +-------+
 *                                                   |  CPU  |
 *                                                   +-------+
 *
 * The existing driver API (register_irq_handler / irq_enable / irq_disable /
 * pic_send_eoi) is preserved. The IRQ backend transparently switches from
 * PIC mask/EOI to IOAPIC redirection+mask / LAPIC EOI when APIC mode is on.
 *
 * Memory map: both LAPIC and IOAPIC MMIO bases live in the 0xFExx_xxxx
 * range, which is within the first 4GB of physical memory. boot.asm
 * identity-maps the first 4GB with 2MB huge pages, and vmm.c refuses to
 * tear down those huge pages — so both controllers are directly
 * dereferencable via volatile pointers (no ioremap needed), exactly like
 * the e1000 MMIO BAR.
 *
 * SMP: only the BSP (CPU 0) is initialized here. AP wakeup (Send INIT-SIPI-SIPI
 * via LAPIC IPI) is deferred to a future KE.
 */

#ifndef LESTRA_APIC_H
#define LESTRA_APIC_H

#include <lestra/types.h>

/* ============================================================ */
/* Interrupt controller mode                                    */
/* ============================================================ */

enum irq_controller {
    IRQ_CONTROLLER_PIC  = 0,   /* Legacy 8259 PIC (default before apic_init) */
    IRQ_CONTROLLER_APIC = 1,   /* Local APIC + IOAPIC (after successful apic_init) */
};

/* Returns the active interrupt controller. PIC until apic_init() promotes to APIC. */
enum irq_controller irq_controller_get(void);

/* True iff the LAPIC + IOAPIC are driving interrupts (not the 8259 PIC). */
int irq_using_apic(void);

/* ============================================================ */
/* Local APIC (LAPIC) — per-CPU MMIO controller                 */
/* ============================================================ */

/* LAPIC register offsets (relative to the LAPIC MMIO base). */
#define LAPIC_ID              0x020   /* LAPIC ID register */
#define LAPIC_VERSION         0x030   /* Version register */
#define LAPIC_TPR             0x080   /* Task Priority */
#define LAPIC_EOI             0x0B0   /* End Of Interrupt — write 0 to ack */
#define LAPIC_LDR             0x0D0   /* Logical Destination */
#define LAPIC_DFR             0x0E0   /* Destination Format (legacy, read 0xF) */
#define LAPIC_SVR             0x0F0   /* Spurious Vector Register */
#define LAPIC_ISR             0x100   /* In-Service Register (256 bits / 0x10 stride) */
#define LAPIC_TMR             0x180   /* Trigger Mode Register */
#define LAPIC_IRR             0x200   /* Interrupt Request Register */
#define LAPIC_ESR             0x280   /* Error Status Register */
#define LAPIC_LVT_CMCI        0x2F0   /* LVT CMCI */
#define LAPIC_LVT_TIMER       0x320   /* LVT Timer */
#define LAPIC_LVT_THERMAL     0x330   /* LVT Thermal Monitor */
#define LAPIC_LVT_PERF        0x340   /* LVT Performance Counter */
#define LAPIC_LVT_LINT0       0x350   /* LVT LINT0 */
#define LAPIC_LVT_LINT1       0x360   /* LVT LINT1 */
#define LAPIC_LVT_ERROR       0x370   /* LVT Error */
#define LAPIC_INIT_COUNT      0x380   /* Timer Initial Count */
#define LAPIC_CURR_COUNT      0x390   /* Timer Current Count */
#define LAPIC_DIV_CONF        0x3E0   /* Timer Divide Configuration */

/* Spurious vector: high vector, low priority. Must NOT be EOI'd. */
#define LAPIC_SPURIOUS_VECTOR 0xFF

/* SVR bit 8 = APIC Software Enable (1 = enabled). */
#define LAPIC_SVR_ENABLE      (1u << 8)

/* IA32_APIC_BASE MSR (0x1B) bits. */
#define APIC_BASE_MSR         0x1B
#define APIC_BASE_BSP         (1u << 8)    /* Bootstrap CPU */
#define APIC_BASE_X2APIC      (1u << 10)   /* x2APIC mode enable */
#define APIC_BASE_ENABLE      (1u << 11)   /* xAPIC global enable */
#define APIC_BASE_ADDR_MASK   0x000FFFFFFFFFF000ULL

/* Default LAPIC MMIO base (used when ACPI MADT didn't provide one). */
#define LAPIC_DEFAULT_BASE    0xFEE00000ULL

/* LVT mask bit. */
#define LAPIC_LVT_MASKED      (1u << 16)

/* Timer mode bits in LVT Timer. */
#define LAPIC_LVT_TIMER_ONESHOT  (0u << 17)
#define LAPIC_LVT_TIMER_PERIODIC (1u << 17)
#define LAPIC_LVT_TIMER_TSC      (2u << 17)

/* Initialize the Local APIC for the BSP.
 * - Enables the APIC via the IA32_APIC_BASE MSR
 * - Programs the Spurious Vector Register (vector 0xFF, APIC enabled)
 * - Masks all LVT entries (timer/LINT0/LINT1/error/...)
 * - Clears any pending EOI
 * Returns 0 on success, -1 if the CPU has no APIC (CPUID check). */
int lapic_init(uint32_t lapic_mmio_base);

/* Acknowledge an interrupt on this CPU's LAPIC.
 * Safe to call from IRQ context. Writes 0 to the EOI register.
 * No-op when in PIC mode (defers to pic_send_eoi). */
void lapic_eoi(void);

/* Read this CPU's LAPIC ID (physical, bits 24-31 of LAPIC_ID). */
uint8_t lapic_get_id(void);

/* Read a 32-bit LAPIC register. */
uint32_t lapic_read(uint32_t reg);
/* Write a 32-bit LAPIC register. */
void lapic_write(uint32_t reg, uint32_t value);

/* Configure the LAPIC timer (for future use as system tick).
 *   vector      — IDT vector to fire
 *   one_shot    — 0 = periodic, 1 = one-shot
 *   initial     — initial count
 *   divide      — divide config raw value (0..7 encoding)
 * Leaves the timer UNMASKED and running. Use lapic_timer_mask() to stop. */
void lapic_timer_setup(uint8_t vector, int one_shot, uint32_t initial, uint8_t divide);
void lapic_timer_mask(void);

/* Send an Inter-Processor Interrupt (for future SMP AP wakeup).
 *   dest      — target LAPIC ID (physical mode)
 *   vector    — vector / shorthand payload
 *   flags     — delivery mode + assert/deassert + level + dest mode raw ICR low bits
 *               (caller assembles per SDM; this helper writes ICR_HIGH then ICR_LOW). */
void lapic_send_ipi(uint8_t dest, uint8_t vector, uint32_t flags);

/* ============================================================ */
/* IOAPIC — routes GSIs (Global System Interrupts) to vectors   */
/* ============================================================ */

/* IOAPIC register access window. */
#define IOAPIC_REGSEL          0x00   /* Register select (write index here) */
#define IOAPIC_WIN             0x10   /* 32-bit data window (read/write here) */

/* IOAPIC register indices (written to REGSEL). */
#define IOAPIC_ID              0x00
#define IOAPIC_VER             0x01
#define IOAPIC_ARB             0x02
/* Redirection table: 24 entries (typical), each is 64-bit = 2x 32-bit regs. */
#define IOAPIC_REDTBL          0x10
#define IOAPIC_REDTBL_LO(n)    (IOAPIC_REDTBL + 2*(n))
#define IOAPIC_REDTBL_HI(n)    (IOAPIC_REDTBL + 2*(n) + 1)

/* Default IOAPIC MMIO base (used when ACPI MADT didn't provide one). */
#define IOAPIC_DEFAULT_BASE    0xFEC00000ULL

/* Redirection-entry low-32-bit field encoding (SDM / IOAPIC spec). */
#define IOAPIC_RED_VECTOR_MASK  0x000000FFu
#define IOAPIC_RED_DELIV_FIXED  (0u << 8)    /* Fixed delivery */
#define IOAPIC_RED_DELIV_LOWEST (1u << 8)
#define IOAPIC_RED_DELIV_SMI    (2u << 8)
#define IOAPIC_RED_DELIV_NMI    (4u << 8)
#define IOAPIC_RED_DELIV_INIT   (5u << 8)
#define IOAPIC_RED_DELIV_EXTINT (7u << 8)
#define IOAPIC_RED_DEST_PHYS    (0u << 11)   /* Physical destination mode */
#define IOAPIC_RED_DEST_LOGICAL (1u << 11)
#define IOAPIC_RED_POLAR_HIGH   (0u << 13)   /* Active high (ISA default) */
#define IOAPIC_RED_POLAR_LOW    (1u << 13)
#define IOAPIC_RED_TRIGGER_EDGE (0u << 15)   /* Edge-triggered (ISA default) */
#define IOAPIC_RED_TRIGGER_LEVEL (1u << 15)
#define IOAPIC_RED_MASKED       (1u << 16)

/* Trigger/polarity policy for a GSI. */
struct ioapic_irq_policy {
    int active_low;     /* 0 = active high, 1 = active low */
    int level_triggered;/* 0 = edge, 1 = level */
};

/* Initialize the IOAPIC.
 * - Selects the MMIO base (ACPI-provided or default 0xFEC00000)
 * - Reads ID, version, max redirection entries
 * - Masks ALL redirection entries (so no stray interrupts fire before drivers register)
 * Returns 0 on success, -1 if IOAPIC not reachable. */
int ioapic_init(uint32_t ioapic_mmio_base, uint32_t gsi_base);

/* Number of GSI redirection entries this IOAPIC supports (24 on QEMU). */
int ioapic_num_redir(void);

/* Route a GSI to a CPU vector and (optionally) unmask it.
 *   gsi       — Global System Interrupt (e.g. ISA IRQ0 -> GSI 2 on QEMU)
 *   vector    — IDT vector (typically irq + 32)
 *   policy    — trigger/polarity (from ACPI MADT ISA override flags, or {0,0})
 *   dest_apic — physical LAPIC ID to deliver to (use lapic_get_id() for self)
 *   unmask    — 1 = enable immediately, 0 = leave masked (caller unmask later)
 * Returns 0 on success, -1 if GSI is out of range. */
int ioapic_route_gsi(uint32_t gsi, uint8_t vector,
                     const struct ioapic_irq_policy* policy,
                     uint8_t dest_apic, int unmask);

/* Mask / unmask a GSI at the IOAPIC redirection entry. */
int ioapic_mask_gsi(uint32_t gsi);
int ioapic_unmask_gsi(uint32_t gsi);

/* ============================================================ */
/* Top-level APIC subsystem init                                */
/* ============================================================ */

/* Initialize the APIC subsystem (LAPIC + IOAPIC).
 * Called once during boot, AFTER acpi_init() and BEFORE any driver
 * registers an IRQ handler (so the first register_irq_handler() can
 * program the IOAPIC redirection entry).
 *
 * If ACPI MADT was found and provides LAPIC + IOAPIC addresses:
 *   - lapic_init(), ioapic_init(), pic_disable()
 *   - promote irq_controller to APIC
 *   - register the spurious-vector no-op handler
 *   - return 0
 * Otherwise:
 *   - leave the PIC in charge (legacy mode)
 *   - return -1 (non-fatal; kernel continues with 8259 PIC) */
int apic_init(void);

#endif /* LESTRA_APIC_H */
