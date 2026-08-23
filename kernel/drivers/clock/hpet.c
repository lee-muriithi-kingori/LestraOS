/*
 * Lestra OS - HPET High Precision Event Timer Driver (KE-37)
 * Copyright (c) 2026 lestramk.org
 *
 * The ACPI subsystem (KE-22) already locates the HPET description table
 * and caches its MMIO base in g_acpi.hpet_base. This driver turns that
 * discovery into a working high-resolution clock:
 *
 *   - Reads the General Capabilities register for the counter period
 *     (femtoseconds per tick) and the counter width (32 vs 64 bit).
 *   - Enables the timer WITHOUT touching IRQ routing or zeroing the
 *     running counter; elapsed time is measured by subtraction from a
 *     boot-time baseline.
 *   - Provides hpet_get_ns(): a monotonic nanosecond timeline anchored
 *     at init, so sys_gettimeofday() can report sub-millisecond time.
 *   - Provides hpet_udelay()/hpet_ndelay(): precise busy-waits that are
 *     immune to PIT quantization.
 *
 * Deliberately NOT done here (future work):
 *   - Comparator interrupts / legacy-replacement IRQ routing. The PIT
 *     remains the scheduler tick source; this driver only reads the
 *     main counter, so it cannot disturb IRQ0 delivery.
 *
 * MMIO access is via volatile loads/stores to the identity-mapped base
 * (HPET lives at 0xFED00000, within the boot 4GB identity map) — same
 * pattern as lapic.c and the e1000 MMIO BAR.
 */

#include <lestra/types.h>
#include <lestra/hpet.h>
#include <lestra/acpi.h>
#include <lestra/timer.h>
#include <lestra/printk.h>

/* ----- HPET MMIO register offsets (from the HPET specification) ----- */

#define HPET_REG_CAPS        0x00   /* General Capabilities and ID (RO, 64-bit) */
#define HPET_REG_CONFIG      0x10   /* General Configuration (RW) */
#define HPET_REG_INTR_STATUS 0x20   /* General Interrupt Status (RW1C) */
#define HPET_REG_MAINCTR     0xF0   /* Main Counter Value (RW) */

/* General Configuration bits */
#define HPET_CONF_ENABLE     (1ULL << 0)
#define HPET_CONF_LEGACY_RTC (1ULL << 1)

/* General Capabilities field layout */
#define HPET_CAPS_COUNTER_64BIT (1ULL << 13)
#define HPET_CAPS_PERIOD_SHIFT  32
#define HPET_CAPS_PERIOD_MASK   0xFFFFFFFFULL

/* Plausible counter periods, in femtoseconds.
 * Spec minimum is 1 fs (1 PHz), but we cap at 1 ns (1 GHz): real HPETs
 * run 10-70 fs periods (14-100 MHz+), and keeping freq_hz <= 1e9 makes
 * ticks_to_ns()'s remainder math provably overflow-free in uint64_t.
 * Reject garbage rather than divide by zero later. */
#define HPET_MIN_PERIOD_FS  1ULL
#define HPET_MAX_PERIOD_FS  1000000ULL   /* 1 ns */

static volatile uint8_t* hpet_mmio = NULL;
static uint64_t fs_per_tick = 0;    /* femtoseconds per main-counter tick */
static uint64_t freq_hz = 0;        /* derived: 1e15 / fs_per_tick */
static int counter_64bit = 0;
static int hpet_ready = 0;

/* Counter baseline: instead of zeroing the main counter at init (which
 * creates an ordering race with the PIT anchor), we snapshot its value
 * c0 and report elapsed = counter - c0. Works for any starting value. */
static uint64_t counter_base = 0;

/* 32-bit-counter wrap extension: a software high word latched on each
 * read (only used when counter_64bit == 0). Mutated under cli() to stay
 * correct across preemption — see hpet_read_counter(). */
static uint32_t last_low = 0;
static uint64_t high_words = 0;

/* gettimeofday anchor: the PIT's millisecond reading at the same instant
 * as counter_base, making wall-time
 *   us = anchor_pit_ms*1000 + elapsed_us
 * continuous across the resolution upgrade. */
static uint64_t anchor_pit_ms = 0;

/* ----- Low-level MMIO helpers ----- */

static inline uint64_t hpet_read64(uint32_t reg) {
    return *(volatile uint64_t*)(hpet_mmio + reg);
}

static inline void hpet_write64(uint32_t reg, uint64_t val) {
    *(volatile uint64_t*)(hpet_mmio + reg) = val;
}

static inline uint32_t hpet_read32(uint32_t reg) {
    return *(volatile uint32_t*)(hpet_mmio + reg);
}

/* Read the main counter as a monotonic 64-bit value.
 *
 * 64-bit counters: a single volatile load.
 *
 * 32-bit counters: two consecutive raw reads that agree did not straddle
 * a rollover, then the software high word is extended whenever the low
 * word goes backwards. The latch update runs with interrupts off — if
 * we were preempted between reading the low word and updating last_low,
 * another task's read would advance the latch and our stale value would
 * wrongly bump high_words (a one-time +2^32-tick time jump). */
static uint64_t hpet_read_counter(void) {
    if (counter_64bit)
        return hpet_read64(HPET_REG_MAINCTR);

    uint32_t flags = (uint32_t)read_flags();
    cli();
    uint32_t a, b;
    do {
        a = hpet_read32(HPET_REG_MAINCTR);
        b = hpet_read32(HPET_REG_MAINCTR);
    } while (a != b);
    if (a < last_low) high_words++;
    last_low = a;
    if (flags & (1u << 9)) sti();   /* restore IF only if it was set */
    return ((uint64_t)high_words << 32) | a;
}

/* Convert elapsed main-counter ticks to nanoseconds without 128-bit
 * math (the kernel does not link libgcc's __udivti3). Split into a
 * whole-seconds term plus a remainder term:
 *   ns = (t / f) * 1e9 + (t % f) * 1e9 / f
 * HPET_MAX_PERIOD_FS (1 ns) caps freq_hz at 1e9, so remainder*1e9
 * <= 1e18 fits comfortably in uint64_t (max ~1.8e19). */
static uint64_t ticks_to_ns(uint64_t ticks) {
    if (!freq_hz) return 0;
    return (ticks / freq_hz) * 1000000000ULL
         + (ticks % freq_hz) * 1000000000ULL / freq_hz;
}

/* ----- Public API ----- */

int hpet_init(void) {
    if (hpet_ready) return 0;

    /* Need the ACPI table's MMIO base (identity-mapped like the LAPIC). */
    if (!g_acpi.hpet_found || !g_acpi.hpet_base) {
        pr_info("hpet: no ACPI HPET table — keeping PIT as sole clock\n");
        return -1;
    }
    hpet_mmio = (volatile uint8_t*)(uintptr_t)g_acpi.hpet_base;

    /* Read capabilities: counter period and width. */
    uint64_t caps = hpet_read64(HPET_REG_CAPS);
    fs_per_tick = (caps >> HPET_CAPS_PERIOD_SHIFT) & HPET_CAPS_PERIOD_MASK;
    counter_64bit = (caps & HPET_CAPS_COUNTER_64BIT) ? 1 : 0;

    if (fs_per_tick < HPET_MIN_PERIOD_FS || fs_per_tick > HPET_MAX_PERIOD_FS) {
        pr_warn("hpet: implausible counter period %llu fs — disabling\n",
                (unsigned long long)fs_per_tick);
        hpet_mmio = NULL;
        return -1;
    }
    freq_hz = 1000000000000000ULL / fs_per_tick;   /* 1e15 / fs */

    /* Bring-up sequence:
     * 1. Halt the counter (clear ENABLE).
     * 2. Keep legacy replacement OFF — the PIT keeps IRQ0, so scheduler
     *    tick delivery is untouched no matter what happens here.
     * 3. Start it (set ENABLE). We deliberately do NOT zero the main
     *    counter: it may hold firmware's arbitrary value, and elapsed
     *    time is computed by subtraction from a snapshot taken below.
     *    This avoids an ordering race between "reset to zero" and
     *    "anchor the PIT timeline". */
    uint64_t cfg = hpet_read64(HPET_REG_CONFIG);
    cfg &= ~(HPET_CONF_ENABLE | HPET_CONF_LEGACY_RTC);
    hpet_write64(HPET_REG_CONFIG, cfg);

    /* Prime the 32-bit wrap latch with the current low word BEFORE
     * enabling, so the first post-enable read can never see a value
     * that looks like a wrap relative to garbage state. */
    last_low = counter_64bit ? 0 : hpet_read32(HPET_REG_MAINCTR);
    high_words = 0;

    hpet_write64(HPET_REG_CONFIG, cfg | HPET_CONF_ENABLE);

    /* Verify the counter actually advances. A dead HPET (some QEMU
     * configs, broken firmware) must fall back cleanly to the PIT.
     * The first read also becomes our elapsed-time baseline. */
    uint64_t t0 = hpet_read_counter();
    for (volatile int i = 0; i < 10000; i++) { /* short spin */ }
    uint64_t t1 = hpet_read_counter();
    if (t1 == t0 && !counter_64bit) {
        /* 32-bit counters at very low frequencies could legitimately
         * not tick in this window; retry with a longer spin before
         * declaring failure. */
        for (volatile int i = 0; i < 5000000; i++) { }
        t1 = hpet_read_counter();
    }
    if (t1 == t0) {
        pr_warn("hpet: main counter frozen at 0x%llx — disabling\n",
                (unsigned long long)t1);
        hpet_write64(HPET_REG_CONFIG, cfg);   /* halt again */
        hpet_mmio = NULL;
        return -1;
    }

    /* Anchor both timelines at the same instant: baseline counter
     * reading + PIT milliseconds, taken back-to-back. */
    counter_base = hpet_read_counter();
    anchor_pit_ms = timer_get_ms();
    hpet_ready = 1;

    pr_info("hpet: ACTIVE at 0x%llx — freq %llu Hz (%llu-bit counter), "
            "%llu comparators\n",
            (unsigned long long)g_acpi.hpet_base,
            (unsigned long long)freq_hz,
            (unsigned long long)(counter_64bit ? 64 : 32),
            (unsigned long long)g_acpi.hpet_comparators);
    return 0;
}

int hpet_available(void) {
    return hpet_ready;
}

uint64_t hpet_get_freq_hz(void) {
    return hpet_ready ? freq_hz : 0;
}

uint64_t hpet_get_ns(void) {
    if (!hpet_ready) {
        /* Graceful degradation: PIT-granularity monotonic timeline. */
        return timer_get_ms() * 1000000ULL;
    }
    return ticks_to_ns(hpet_read_counter() - counter_base);
}

/* Microsecond-resolution boot clock for sys_gettimeofday()/clock_gettime:
 * PIT milliseconds up to the init instant, HPET microseconds after it.
 * Monotonic (both components only move forward) and continuous (the
 * anchor was captured at the switch). Falls back to plain PIT ms when
 * no HPET is present, preserving the pre-KE-37 behaviour exactly. */
uint64_t hpet_wall_us(void) {
    if (!hpet_ready)
        return timer_get_ms() * 1000ULL;
    return anchor_pit_ms * 1000ULL
         + ticks_to_ns(hpet_read_counter() - counter_base) / 1000ULL;
}

void hpet_udelay(uint64_t us) {
    if (!hpet_ready || us == 0) {
        /* No HPET: PIT busy-wait, chunked so the uint32_t ms argument
         * cannot truncate a long delay (timer_wait_ms takes ~49 days
         * per call at most). */
        while (us > 0) {
            uint64_t chunk = us > 4000000000ULL ? 4000000000ULL : us;
            timer_wait_ms((uint32_t)((chunk + 999) / 1000));
            us -= chunk;
        }
        return;
    }
    uint64_t deadline = hpet_get_ns() + us * 1000ULL;
    while (hpet_get_ns() < deadline) {
        /* busy spin — callers are pre-scheduler or need precision */
    }
}

void hpet_ndelay(uint64_t ns) {
    if (!hpet_ready || ns < 1000) {
        /* Sub-microsecond without an HPET: a short TSC spin. */
        if (ns >= 100) {
            for (volatile int i = 0; i < 100; i++) { }
        }
        return;
    }
    uint64_t deadline = hpet_get_ns() + ns;
    while (hpet_get_ns() < deadline) {
        /* busy spin */
    }
}
