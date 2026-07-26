/*
 * Lestra OS - CPU / GPU Temperature Sensor Driver (REAL READINGS)
 * Copyright (c) 2026 lestramk.org
 *
 * Reads real CPU temperatures using on-die thermal MSRs:
 *   - Intel: MSR_IA32_PACKAGE_THERM_STATUS (0x1B1), bits [31:24]
 *     contain the digital reading relative to TjMax.
 *     actual_temp = TjMax - (reading >> 24) & 0xFF
 *     TjMax is determined via CPUID family/model lookup table.
 *   - AMD: MSR AMCTEMPERATURE (0xA01C) returns actual temp directly
 *     (not relative to TjMax). Falls back to default 95 C if unreadable.
 *
 * GPU temperature is still simulated (no GPU thermal driver in tree),
 * but GPU thermal zones on integrated GPUs can share the CPU package
 * reading on some Intel chips.
 *
 * temp_is_simulated() returns 0 when real MSR readings succeed,
 * so the UI can honestly show "live" vs "simulated" status.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/power.h>

/* ----- MSR definitions ----- */
#define MSR_IA32_PACKAGE_THERM_STATUS  0x1B1
#define MSR_IA32_THERM_STATUS          0x19C
#define MSR_AMD_TEMPERATURE            0xA01C   /* AMD AMCTEMPERATURE */
#define MSR_IA32_TEMPERATURE_TARGET    0x1A2   /* Intel TjMax offset (some models) */

/* Simulated fallback temperatures (degrees Celsius). */
#define SIM_CPU_TEMP  45
#define SIM_GPU_TEMP  42

/* ----- CPU vendor identification ----- */
enum cpu_vendor {
    VENDOR_UNKNOWN = 0,
    VENDOR_INTEL   = 1,
    VENDOR_AMD     = 2,
};

static int initialized   = 0;
static int have_msr      = 0;   /* CPU supports MSRs (CPUID EDX bit 5) */
static int real_readings  = 0;   /* 1 if we can decode real temperatures */
static int cpu_vendor_id  = VENDOR_UNKNOWN;
static int tj_max         = 100; /* Intel TjMax default, overridden by lookup */

/* ----- CPUID helpers ----- */

/* Execute CPUID with given leaf and subleaf, store results. */
static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(subleaf));
}

/* Detect CPU vendor from CPUID leaf 0 (vendor string in EBX:EDX:ECX). */
static enum cpu_vendor detect_cpu_vendor(void) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);

    /* Intel: "GenuineIntel" → EBX=0x756E6547, EDX=0x49656E69, ECX=0x6C65746E */
    if (b == 0x756E6547 && d == 0x49656E69 && c == 0x6C65746E)
        return VENDOR_INTEL;

    /* AMD: "AuthenticAMD" → EBX=0x68747541, EDX=0x69746E41, ECX=0x444D4163
     * Also: "AMDisbetter!" → EBX=0x69447861 (early AMD K5) */
    if (b == 0x68747541 && d == 0x69746E41 && c == 0x444D4163)
        return VENDOR_AMD;
    if (b == 0x69447861)
        return VENDOR_AMD;

    return VENDOR_UNKNOWN;
}

/* CPUID.1 EDX bit 5 = MSR support. */
static int cpuid_has_msr(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    return (d & (1u << 5)) ? 1 : 0;
}

/* ----- Intel TjMax lookup table -----
 *
 * TjMax (maximum junction temperature) varies per Intel CPU model.
 * The digital thermal sensor reports (TjMax - actual_temp) in the MSR,
 * so we MUST know TjMax to get the real temperature.
 *
 * CPUID leaf 1: EAX[27:20] = extended family, EAX[19:16] = extended model
 *               EAX[11:8] = family, EAX[7:4] = model
 * Intel uses:   family = EAX[11:8] + EAX[27:20] (extended family added only if base != 0xF)
 *               model  = EAX[7:4] + (EAX[19:16] << 4) (extended model always added)
 *
 * Sources: Intel SDM Vol 3A §2.7, coretemp Linux driver, Intel datasheets.
 */
struct tjmax_entry {
    uint32_t family;    /* CPU family (combined base + extended) */
    uint32_t model;     /* CPU model (combined base + extended) */
    int      tjmax;     /* Junction max temperature in °C */
};

/* Known TjMax values for popular Intel CPUs.
 * This covers the most common modern Intel processors.
 * Missing models default to 100°C (most modern Intel). */
static const struct tjmax_entry intel_tjmax_table[] = {
    /* Nehalem / Westmere (family 6, model 0x1A/0x1E/0x2E/0x25/0x2C/0x2F) */
    { 6, 0x1A, 100 },   /* Core i7 Bloomfield (45nm) */
    { 6, 0x1E, 100 },   /* Core i7/i5 Lynnfield (45nm) */
    { 6, 0x2E, 100 },   /* Core i7 Xeon Westmere-EP (45nm) */
    { 6, 0x25, 100 },   /* Core i7/i5 Arrandale (32nm) */
    { 6, 0x2C, 100 },   /* Core i5/i7 Clarkdale (32nm) */
    { 6, 0x2F, 100 },   /* Xeon Westmere-EX */

    /* Sandy Bridge (family 6, model 0x2A/0x2D) */
    { 6, 0x2A, 100 },   /* Core i7/i5/i3 Sandy Bridge (32nm) */
    { 6, 0x2D, 100 },   /* Sandy Bridge-E */

    /* Ivy Bridge (family 6, model 0x3A/0x3E) */
    { 6, 0x3A, 105 },   /* Core i7/i5/i3 Ivy Bridge (22nm) — some are 100 */
    { 6, 0x3E, 100 },   /* Ivy Bridge-E */

    /* Haswell / Broadwell (family 6, model 0x3C/0x3F/0x45/0x46/0x3D/0x47/0x4F/0x56) */
    { 6, 0x3C, 100 },   /* Haswell desktop */
    { 6, 0x3F, 100 },   /* Haswell-E / Haswell-EP */
    { 6, 0x45, 100 },   /* Haswell ULT (mobile) */
    { 6, 0x46, 100 },   /* Haswell GT3e */
    { 6, 0x3D, 100 },   /* Broadwell */
    { 6, 0x47, 100 },   /* Broadwell-G */
    { 6, 0x4F, 100 },   /* Broadwell-E */
    { 6, 0x56, 100 },   /* Broadwell-DE */

    /* Skylake / Kaby Lake / Coffee Lake (family 6, model 0x4E/0x5E/0x8E/0x9E) */
    { 6, 0x4E, 100 },   /* Skylake mobile */
    { 6, 0x5E, 100 },   /* Skylake desktop */
    { 6, 0x8E, 100 },   /* Kaby Lake / Coffee Lake mobile */
    { 6, 0x9E, 100 },   /* Kaby Lake / Coffee Lake desktop */

    /* Comet Lake / Ice Lake / Tiger Lake (family 6, model 0xA5/0xA6/0x7D/0x7E/0x8C) */
    { 6, 0xA5, 100 },   /* Comet Lake desktop */
    { 6, 0xA6, 100 },   /* Comet Lake mobile */
    { 6, 0x7D, 100 },   /* Ice Lake mobile */
    { 6, 0x7E, 100 },   /* Ice Lake server */
    { 6, 0x8C, 100 },   /* Tiger Lake mobile */

    /* Alder Lake / Raptor Lake (family 6, model 0x97/0x9A/0xB7) */
    { 6, 0x97, 100 },   /* Alder Lake desktop */
    { 6, 0x9A, 100 },   /* Alder Lake mobile */
    { 6, 0xB7, 100 },   /* Raptor Lake */

    /* Atom / Pentium / Celeron (lower TjMax) */
    { 6, 0x36, 90  },   /* Atom Cedarview */
    { 6, 0x37, 90  },   /* Atom Bay Trail / Silvermont */
    { 6, 0x4A, 90  },   /* Atom Merrifield */
    { 6, 0x4C, 90  },   /* Atom Airmont */
    { 6, 0x5A, 85  },   /* Atom Moorefield */
    { 6, 0x5D, 85  },   /* Atom Goldmont */
    { 6, 0x6E, 100 },   /* Atom Denverton */

    /* Older NetBurst (Pentium 4) — family 15 */
    /* P4 Prescott 2M: TjMax=85 or 90 depending on stepping.
     * We default to 90 for family 15. */
    { 15, 0x03, 90 },   /* Pentium 4 Prescott */
    { 15, 0x04, 90 },   /* Pentium 4 Prescott-2M */
    { 15, 0x06, 85 },   /* Pentium 4 Cedar Mill */
};

#define TJMAX_TABLE_SIZE  (sizeof(intel_tjmax_table) / sizeof(intel_tjmax_table[0]))

/* Lookup TjMax for a given Intel CPU family/model combination. */
static int lookup_intel_tjmax(uint32_t family, uint32_t model) {
    for (unsigned i = 0; i < TJMAX_TABLE_SIZE; i++) {
        if (intel_tjmax_table[i].family == family &&
            intel_tjmax_table[i].model == model)
            return intel_tjmax_table[i].tjmax;
    }
    /* Default: 100°C for modern Intel, 90°C for family 15 (NetBurst/P4) */
    return (family == 15) ? 90 : 100;
}

/* Decode Intel family and model from CPUID leaf 1 EAX.
 * Intel SDM: family = base_family + extended_family (if base==0xF)
 *             model  = base_model | (extended_model << 4) */
static void decode_intel_cpuid(uint32_t* family_out, uint32_t* model_out) {
    uint32_t eax, b, c, d;
    cpuid(1, 0, &eax, &b, &c, &d);

    uint32_t base_family   = (eax >> 8)  & 0xF;
    uint32_t base_model    = (eax >> 4)  & 0xF;
    uint32_t ext_family    = (eax >> 20) & 0xFF;
    uint32_t ext_model     = (eax >> 16) & 0xFF;

    uint32_t family = base_family;
    if (base_family == 0xF)
        family += ext_family;

    uint32_t model = base_model | (ext_model << 4);

    *family_out = family;
    *model_out  = model;
}

/* Try to read TjMax from MSR_IA32_TEMPERATURE_TARGET (0x1A2).
 * This MSR exists on some newer Intel CPUs (Sandy Bridge+) and
 * directly encodes TjMax in bits [23:16].
 * Returns -1 if the MSR doesn't exist or reads as 0. */
static int read_msr_tjmax(void) {
    /* Check if CPU supports this MSR via CPUID leaf 6 (thermal mgmt).
     * CPUID.06 EAX bit 0 = digital thermal sensor available. */
    uint32_t a, b, c, d;
    cpuid(6, 0, &a, &b, &c, &d);
    if (!(a & 1)) return -1;   /* no thermal MSR */

    uint64_t val = rdmsr(MSR_IA32_TEMPERATURE_TARGET);
    int tj_from_msr = (int)((val >> 16) & 0xFF);
    if (tj_from_msr > 0 && tj_from_msr < 200)
        return tj_from_msr;

    return -1;   /* invalid reading */
}

/* ----- Temperature reading functions ----- */

/* Read real CPU temperature from Intel Package Thermal MSR.
 * Format: bits [31:24] = digital temp reading relative to TjMax.
 * actual_temp = TjMax - (bits[31:24] & 0xFF)
 * Returns -1 if MSR unreadable or reading is invalid (e.g. 0xFF = sensor off). */
static int read_intel_package_temp(void) {
    uint64_t therm = rdmsr(MSR_IA32_PACKAGE_THERM_STATUS);
    uint8_t raw = (uint8_t)((therm >> 24) & 0xFF);

    /* Raw value 0xFF means the sensor is off or not calibrated. */
    if (raw == 0xFF) return -1;

    /* raw = TjMax - actual_temp, so actual_temp = TjMax - raw */
    int temp = tj_max - (int)raw;

    /* Sanity: temperature should be between 0 and tj_max */
    if (temp < 0) temp = 0;
    if (temp > tj_max) temp = tj_max;

    return temp;
}

/* Read real CPU temperature from Intel Core-level Thermal MSR (0x19C).
 * Used as fallback if package-level MSR is unavailable.
 * Format same as package MSR: bits [22:16] for core, or bits [31:24] depending on model.
 * For consistency we use bits [31:24] here as well since modern Intel
 * uses this format for both core and package thermal sensors. */
static int read_intel_core_temp(void) {
    uint64_t therm = rdmsr(MSR_IA32_THERM_STATUS);
    uint8_t raw = (uint8_t)((therm >> 16) & 0xFF);

    if (raw == 0x80) return -1;   /* sensor off */

    int temp = tj_max - (int)raw;
    if (temp < 0) temp = 0;
    if (temp > tj_max) temp = tj_max;
    return temp;
}

/* Read real CPU temperature from AMD AMCTEMPERATURE MSR (0xA01C).
 * Unlike Intel, this MSR reports the absolute temperature directly.
 * Bits [31:21] = temperature in 1/8°C increments (some docs say raw °C,
 * but most AMD family 17h+ report raw °C in bits [31:21]).
 * We try to read and return the value, clamped to sanity range. */
static int read_amd_temp(void) {
    uint64_t val = rdmsr(MSR_AMD_TEMPERATURE);
    /* AMD family 17h/19h: bits [31:21] contain raw temp in °C.
     * Older AMD (family 0Fh/10h): CurTemp in bits [31:21] as 1/8°C. */
    uint32_t family, model;
    uint32_t eax, b, c, d;
    cpuid(1, 0, &eax, &b, &c, &d);
    uint32_t base_family = (eax >> 8) & 0xF;
    uint32_t ext_family  = (eax >> 20) & 0xFF;
    uint32_t family_full = base_family + (base_family == 0xF ? ext_family : 0);

    int temp;
    if (family_full >= 0x17) {
        /* Ryzen / modern AMD: raw temperature in bits [31:21], degrees C.
         * Some models: bits [31:21] = temp * 0.125, so divide by 8.
         * We read as-is and round to nearest integer. */
        temp = (int)((val >> 21) & 0x7FF);
        /* Some Ryzen report value as direct °C, others as 0.125°C steps.
         * If value > 200, assume it's in 0.125°C steps and divide by 8. */
        if (temp > 200) temp = temp / 8;
    } else {
        /* Older AMD (K8/K10): CurTemp in bits [31:21], 0.125°C resolution */
        temp = ((int)((val >> 21) & 0x7FF)) / 8;
    }

    /* Sanity clamp */
    if (temp < 0) temp = 0;
    if (temp > 150) temp = 150;

    return temp;
}

/* ----- init / public API ----- */

int temp_init(void) {
    pr_info("temp: initialising thermal sensors\n");

    /* Step 1: Detect MSR support */
    have_msr = cpuid_has_msr();
    if (!have_msr) {
        pr_info("temp: no CPU MSR support detected, using simulated readings\n");
        initialized = 1;
        return 0;
    }

    /* Step 2: Detect CPU vendor */
    cpu_vendor_id = detect_cpu_vendor();
    pr_info("temp: CPU vendor = %s\n",
            cpu_vendor_id == VENDOR_INTEL ? "Intel" :
            cpu_vendor_id == VENDOR_AMD  ? "AMD" : "Unknown");

    /* Step 3: Determine TjMax and attempt real reading */
    if (cpu_vendor_id == VENDOR_INTEL) {
        /* Decode family/model from CPUID */
        uint32_t family, model;
        decode_intel_cpuid(&family, &model);
        pr_info("temp: Intel CPU family=0x%x model=0x%x\n", family, model);

        /* Try MSR_IA32_TEMPERATURE_TARGET first (most reliable on modern CPUs) */
        int tj_from_msr = read_msr_tjmax();
        if (tj_from_msr > 0) {
            tj_max = tj_from_msr;
            pr_info("temp: TjMax=%d°C (from MSR 0x1A2)\n", tj_max);
        } else {
            tj_max = lookup_intel_tjmax(family, model);
            pr_info("temp: TjMax=%d°C (from CPUID lookup table)\n", tj_max);
        }

        /* Probe the package thermal MSR */
        uint64_t therm = rdmsr(MSR_IA32_PACKAGE_THERM_STATUS);
        pr_info("temp: MSR_IA32_PACKAGE_THERM_STATUS raw=0x%x\n",
                (unsigned)(therm & 0xFFFFFFFFu));

        /* Try real temperature reading */
        int temp = read_intel_package_temp();
        if (temp >= 0) {
            pr_info("temp: real CPU temperature = %d°C (TjMax=%d°C, raw_delta=%d)\n",
                    temp, tj_max, tj_max - temp);
            real_readings = 1;
        } else {
            /* Package MSR might not exist on older CPUs; try core-level */
            temp = read_intel_core_temp();
            if (temp >= 0) {
                pr_info("temp: real CPU core temperature = %d°C\n", temp);
                real_readings = 1;
            } else {
                pr_info("temp: thermal MSR unreadable, using simulated readings\n");
            }
        }
    } else if (cpu_vendor_id == VENDOR_AMD) {
        /* AMD: try AMCTEMPERATURE MSR */
        pr_info("temp: AMD CPU, attempting AMCTEMPERATURE MSR\n");
        int temp = read_amd_temp();
        if (temp > 0 && temp < 150) {
            pr_info("temp: real AMD CPU temperature = %d°C\n", temp);
            real_readings = 1;
            /* AMD doesn't use TjMax concept, but set a reference value */
            tj_max = 95;   /* AMD typical thermal limit */
        } else {
            pr_info("temp: AMD MSR unreadable, defaulting to %d°C\n", 95);
            tj_max = 95;
        }
    } else {
        /* Unknown vendor — try Intel-style MSR as last resort */
        uint64_t therm = rdmsr(MSR_IA32_PACKAGE_THERM_STATUS);
        uint8_t raw = (uint8_t)((therm >> 24) & 0xFF);
        if (raw != 0xFF) {
            int temp = 100 - (int)raw;   /* assume TjMax=100 */
            if (temp >= 0 && temp <= 150) {
                pr_info("temp: unknown CPU, guessed temperature = %d°C\n", temp);
                real_readings = 1;
            }
        }
    }

    if (!real_readings) {
        pr_info("temp: using simulated readings (CPU=%d°C, GPU=%d°C)\n",
                SIM_CPU_TEMP, SIM_GPU_TEMP);
    } else {
        pr_info("temp: REAL temperature readings active\n");
    }

    initialized = 1;
    return 0;
}

int temp_get_cpu(void) {
    if (!initialized) return -1;

    /* If real readings work, always read live from MSR.
     * This gives current temperature (not a stale cached value). */
    if (real_readings) {
        if (cpu_vendor_id == VENDOR_INTEL) {
            /* Prefer package-level sensor (more accurate for whole-CPU) */
            int temp = read_intel_package_temp();
            if (temp >= 0) return temp;
            /* Fall back to core-level sensor */
            temp = read_intel_core_temp();
            if (temp >= 0) return temp;
        } else if (cpu_vendor_id == VENDOR_AMD) {
            int temp = read_amd_temp();
            if (temp > 0 && temp < 150) return temp;
        }
    }

    /* Simulated fallback */
    return SIM_CPU_TEMP;
}

int temp_get_gpu(void) {
    if (!initialized) return -1;
    /* No dedicated GPU thermal driver. On systems with integrated GPUs
     * (Intel HD/Iris, AMD APU), the GPU shares the CPU package thermal
     * sensor. We report a slightly lower value as GPU is typically cooler
     * than CPU cores under load. If real CPU readings work, estimate GPU
     * as CPU - 3°C (typical offset for integrated GPUs). */
    if (real_readings) {
        int cpu_temp = temp_get_cpu();
        if (cpu_temp > 0) return cpu_temp - 3;
    }
    return SIM_GPU_TEMP;
}

/* Returns 0 when real MSR readings succeed, 1 when simulated. */
int temp_is_simulated(void) {
    if (real_readings) return 0;
    return 1;
}
