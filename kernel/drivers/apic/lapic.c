/*
 * Lestra OS - Local APIC Driver (KE-23)
 * Copyright (c) 2026 lestramk.org
 *
 * Per-CPU interrupt controller. On the BSP (CPU 0) it:
 *   - Enables xAPIC mode via IA32_APIC_BASE MSR (bit 11)
 *   - Programs the Spurious Vector Register: vector 0xFF, APIC enabled
 *   - Masks all LVT entries (timer/LINT0/LINT1/error/CMCI/thermal/perf)
 *   - Clears ESR and any pending EOI
 *
 * MMIO access is via volatile 32-bit loads/stores to the LAPIC base.
 * The base lives at 0xFEE00000 (within the boot 4GB identity map), so
 * no ioremap is needed — same pattern as the e1000 MMIO BAR.
 */

#include <lestra/types.h>
#include <lestra/apic.h>
#include <lestra/acpi.h>
#include <lestra/idt.h>
#include <lestra/printk.h>

/* IA32_APIC_BASE MSR is in types.h via rdmsr/wrmsr helpers. */

/* File-static MMIO base. Set once by lapic_init, read by everyone else.
 * 0 means "not initialized yet" (LAPIC calls become no-ops). */
static volatile uint32_t* lapic_base = NULL;

/* Helper: volatile pointer to a 32-bit LAPIC register. */
static inline volatile uint32_t* lapic_reg(uint32_t reg) {
    return (volatile uint32_t*)((uintptr_t)lapic_base + reg);
}

uint32_t lapic_read(uint32_t reg) {
    if (!lapic_base) return 0;
    return *lapic_reg(reg);
}

void lapic_write(uint32_t reg, uint32_t value) {
    if (!lapic_base) return;
    *lapic_reg(reg) = value;
}

/* CPUID: does this CPU have an APIC on chip?
 * Feature bit EDX[9] (CPUID leaf 1). */
static int cpu_has_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    return (edx >> 9) & 1;
}

/* Spurious interrupt handler. Per Intel SDM 3.10.9, a spurious APIC
 * interrupt (vector 0xFF) MUST NOT be EOI'd — doing so could EOI a
 * real interrupt that is currently in-service. This handler does
 * nothing and returns. */
static void spurious_handler(struct interrupt_frame* frame) {
    (void)frame;
    /* Intentionally empty. No EOI. */
}

int lapic_init(uint32_t lapic_mmio_base) {
    if (!cpu_has_apic()) {
        pr_warn("apic: CPU reports no on-chip APIC (CPUID.1:EDX[9]=0)\n");
        return -1;
    }

    /* Set the MMIO base pointer (within the 4GB identity map). */
    lapic_base = (volatile uint32_t*)(uintptr_t)lapic_mmio_base;

    /* --- Enable xAPIC via IA32_APIC_BASE MSR (0x1B) ---
     * Preserve the BSP bit and the base address; set the ENABLE bit (11).
     * Do NOT set x2APIC bit (10) — we use MMIO (xAPIC), not MSR (x2APIC). */
    uint64_t base_msr = rdmsr(APIC_BASE_MSR);
    /* Sanity: the MSR's base address should match the ACPI-provided base.
     * If not, trust the MSR (it's what the CPU actually decodes). */
    uint64_t msr_phys = base_msr & APIC_BASE_ADDR_MASK;
    if (msr_phys && (uint32_t)msr_phys != lapic_mmio_base) {
        pr_info("apic: LAPIC MMIO base mismatch (MSR=0x%x, MADT=0x%x) — using MSR\n",
                (unsigned)msr_phys, lapic_mmio_base);
        lapic_base = (volatile uint32_t*)msr_phys;
    }
    base_msr |= APIC_BASE_ENABLE;
    /* Clear x2APIC bit defensively (in case firmware set it). */
    base_msr &= ~APIC_BASE_X2APIC;
    wrmsr(APIC_BASE_MSR, base_msr);

    /* --- Mask all LVT entries first (so nothing fires before we're ready) ---
     * Write MASKED bit into every LVT. */
    lapic_write(LAPIC_LVT_CMCI,    LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_TIMER,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PERF,    LAPIC_LVT_MASKED);
    /* LINT0/LINT1: in legacy-pic-compatible systems these route ExtINT/NMI.
     * We mask them — we are NOT using the PIC as a master, so external
     * interrupts come straight through the IOAPIC. */
    lapic_write(LAPIC_LVT_LINT0,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR,   LAPIC_LVT_MASKED);

    /* --- Clear Error Status Register (write-then-read pattern) --- */
    lapic_write(LAPIC_ESR, 0);
    (void)lapic_read(LAPIC_ESR);

    /* --- Clear any pending EOI (defensive) --- */
    lapic_write(LAPIC_EOI, 0);

    /* --- Program the Spurious Vector Register ---
     * Bits 0-7: spurious vector (0xFF)
     * Bit 8   : APIC Software Enable (1 = enabled)
     * Bit 9   : Focus Processor Check (0 = disable, recommended on modern CPUs)
     * Vector 0xFF is the lowest-priority vector and sits at the top of the
     * IDT. We register a no-op handler so spurious IRQs are silently
     * discarded WITHOUT EOI (EOIing a spurious IRQ would corrupt state). */
    lapic_write(LAPIC_SVR, LAPIC_SPURIOUS_VECTOR | LAPIC_SVR_ENABLE);

    /* Register the spurious no-op handler in the IDT dispatch table.
     * The IDT gate for vector 0xFF must be installed too — isr.asm
     * provides stubs for all 256 vectors, but idt_init() only loads
     * gates 0-47. Install the gate here. */
    idt_set_gate(LAPIC_SPURIOUS_VECTOR, isr_stubs[LAPIC_SPURIOUS_VECTOR],
                 IDT_TYPE_INTERRUPT, IDT_ATTR_RING0);
    idt_reload();  /* Flush IDTR so the CPU sees the new gate for vector 0xFF */
    register_interrupt_handler(LAPIC_SPURIOUS_VECTOR, spurious_handler);

    /* --- Task Priority: allow all interrupts (TPR=0 means no priority class
     * is masked). Set to 0 so IRQs are never blocked by task priority. --- */
    lapic_write(LAPIC_TPR, 0);

    uint8_t id = lapic_get_id();
    uint32_t ver = lapic_read(LAPIC_VERSION);
    pr_info("apic: LAPIC enabled at 0x%x — id=%u, version=0x%x, max-lvt=%u\n",
            lapic_mmio_base, id, ver & 0xFF, (ver >> 16) & 0xFF);

    return 0;
}

void lapic_eoi(void) {
    /* Writes 0 to the EOI register. The LAPIC then clears the highest-
     * priority bit in the ISR (In-Service Register), allowing lower or
     * equal priority interrupts to be delivered.
     *
     * No-op if LAPIC isn't initialized (PIC mode handles EOI itself). */
    lapic_write(LAPIC_EOI, 0);
}

uint8_t lapic_get_id(void) {
    /* Physical LAPIC ID is in bits 24-31 of the ID register (xAPIC layout). */
    return (uint8_t)(lapic_read(LAPIC_ID) >> 24);
}

void lapic_timer_setup(uint8_t vector, int one_shot, uint32_t initial, uint8_t divide) {
    if (!lapic_base) return;
    /* Divide config (low 4 bits encode the divisor; we accept raw encoding). */
    lapic_write(LAPIC_DIV_CONF, divide & 0x0F);
    /* Initial count. */
    lapic_write(LAPIC_INIT_COUNT, initial);
    /* LVT Timer: vector + mode + unmasked. */
    uint32_t lvt = vector;
    if (one_shot)
        lvt |= LAPIC_LVT_TIMER_ONESHOT;
    else
        lvt |= LAPIC_LVT_TIMER_PERIODIC;
    lapic_write(LAPIC_LVT_TIMER, lvt);
}

void lapic_timer_mask(void) {
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    /* Stop the counter too. */
    lapic_write(LAPIC_INIT_COUNT, 0);
}

void lapic_send_ipi(uint8_t dest, uint8_t vector, uint32_t flags) {
    if (!lapic_base) return;
    /* Delivery Status bit (12) must poll to 0 (idle) before sending.
     * For simplicity (and per common practice on QEMU), we don't poll —
     * the IPI completes synchronously fast on a single-CPU system.
     * Future SMP KE should poll ICR[12]. */
    /* High 32 bits: destination field (bits 24-31 = physical APIC ID). */
    lapic_write(0x310, ((uint32_t)dest) << 24);
    /* Low 32 bits: vector + flags. Writing the low word triggers the IPI. */
    lapic_write(0x300, (uint32_t)vector | flags);
}
