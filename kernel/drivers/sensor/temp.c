/*
 * Lestra OS - CPU / GPU Temperature Sensor Driver
 * Copyright (c) 2026 lestramk.org
 *
 * Reads CPU and GPU temperatures. On real hardware we'd walk the ACPI
 * thermal zones (_TZ._TMP) or read the on-die CPU thermal MSRs, but
 * LestraOS doesn't yet ship an AML interpreter and TjMax calibration
 * is CPU-model-specific, so we report a simulated 45 C — enough for
 * the system monitor applet to render a sensible bar.
 *
 * We do probe CPUID for MSR support and log what we find, so the
 * groundwork for a real thermal driver is in place.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/power.h>

/* Simulated temperatures in degrees Celsius. */
#define SIM_CPU_TEMP  45
#define SIM_GPU_TEMP  42

/* Intel Package Thermal Status MSR (readable on modern Core CPUs).
 * The reading format is: bits 22:16 = digital thermal sensor output,
 * reported as (TjMax - reading). TjMax varies per model (typically
 * 100 C), so decoding it requires per-family tables which we don't
 * ship yet. */
#define MSR_IA32_PACKAGE_THERM_STATUS  0x1B1

static int initialized = 0;
static int have_msr    = 0;   /* CPU supports MSRs (CPUID EDX bit 5) */

/* CPUID.1 EDX bit 5 = MSR support. */
static int cpuid_has_msr(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(1), "c"(0));
    return (d & (1u << 5)) ? 1 : 0;
}

int temp_init(void) {
    pr_info("temp: initialising thermal sensors\n");

    have_msr = cpuid_has_msr();
    if (have_msr) {
        /* Probe the package thermal MSR to confirm it's readable. We
         * don't yet decode the reading (TjMax is CPU-model-specific),
         * so we still report simulated values for now. */
        uint64_t therm = rdmsr(MSR_IA32_PACKAGE_THERM_STATUS);
        pr_info("temp: CPU MSR thermal sensor available (raw=%x)\n",
                (unsigned)(therm & 0xFFFFFFFFu));
    } else {
        pr_info("temp: no CPU MSR support detected\n");
    }

    pr_info("temp: using simulated readings (CPU=%dC, GPU=%dC)\n",
            SIM_CPU_TEMP, SIM_GPU_TEMP);

    initialized = 1;
    return 0;
}

int temp_get_cpu(void) {
    if (!initialized) return -1;
    /* HONEST STATUS: We do read MSR_IA32_PACKAGE_THERM_STATUS during
     * init (see temp_init above), but we do NOT decode the reading
     * because TjMax is CPU-model-specific and we don't ship a TjMax
     * table yet. The returned value is therefore the hardcoded SIM
     * constant. On real silicon this number is WRONG. */
    return SIM_CPU_TEMP;
}

int temp_get_gpu(void) {
    if (!initialized) return -1;
    /* No GPU thermal driver in tree. Simulated. */
    return SIM_GPU_TEMP;
}

/* New: lets the UI honestly flag these as simulated. */
int temp_is_simulated(void) { return 1; }
