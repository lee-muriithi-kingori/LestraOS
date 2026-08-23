/*
 * Lestra OS - HPET High Precision Event Timer (KE-37)
 * Copyright (c) 2026 lestramk.org
 *
 * Monotonic nanosecond clock + microsecond busy-delays, driven by the
 * ACPI-discovered HPET at g_acpi.hpet_base. The PIT stays the scheduler
 * tick source — the HPET only provides high-resolution *time*, it does
 * not generate interrupts.
 */

#ifndef LESTRA_HPET_H
#define LESTRA_HPET_H

#include <lestra/types.h>

/* Bring up the HPET using the ACPI table's base address.
 * Non-fatal: returns -1 and leaves hpet_available()==0 if there is no
 * HPET table, the MMIO block looks wrong, or the counter cannot start.
 * Must be called AFTER acpi_init() and timer_init(). */
int hpet_init(void);

/* 1 if the HPET is running (hpet_get_ns / delays are usable). */
int hpet_available(void);

/* Monotonic nanoseconds since hpet_init() succeeded.
 * Returns PIT-derived milliseconds*1e6 if the HPET is unavailable,
 * so callers always get a valid monotonic timeline. */
uint64_t hpet_get_ns(void);

/* Microsecond-resolution boot clock for sys_gettimeofday():
 * PIT milliseconds up to hpet_init(), HPET microseconds after it.
 * Falls back to PIT milliseconds*1000 without an HPET. */
uint64_t hpet_wall_us(void);

/* Raw main-counter frequency in Hz (0 = no HPET). */
uint64_t hpet_get_freq_hz(void);

/* Precise busy-wait delays (spin on the main counter).
 * Fall back to timer_wait_ms()/hlt() when no HPET is present. */
void hpet_udelay(uint64_t us);
void hpet_ndelay(uint64_t ns);

#endif /* LESTRA_HPET_H */
