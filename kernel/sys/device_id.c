/*
 * Lestra OS - Device ID Randomizer
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Generates a random device ID (machine-id) at boot using the
 * kernel CSPRNG. This 32-character hex string replaces the
 * traditional /etc/machine-id and changes every boot, preventing
 * device fingerprinting across sessions.
 *
 * The machine-id is a 128-bit (16-byte) random value displayed
 * as 32 lowercase hex characters, compatible with systemd's
 * /etc/machine-id format.
 */

#include <lestra/types.h>
#include <lestra/printk.h>

#define MACHINE_ID_LEN  16   /* 16 bytes = 128 bits */
#define MACHINE_ID_STR  33   /* 32 hex chars + NUL */

/* The random machine-id, generated once at boot */
static uint8_t  machine_id_bytes[MACHINE_ID_LEN];
static char     machine_id_str[MACHINE_ID_STR];
static int      machine_id_initialized = 0;

/* Hex lookup table */
static const char hex_chars[] = "0123456789abcdef";

/* Generate a new random machine-id. Called once at boot. */
void device_id_init(void) {
    /* Generate 16 random bytes using the CSPRNG */
    get_random_bytes(machine_id_bytes, MACHINE_ID_LEN);

    /* Convert to hex string */
    for (int i = 0; i < MACHINE_ID_LEN; i++) {
        machine_id_str[i * 2]     = hex_chars[(machine_id_bytes[i] >> 4) & 0x0F];
        machine_id_str[i * 2 + 1] = hex_chars[machine_id_bytes[i] & 0x0F];
    }
    machine_id_str[32] = '\0';

    machine_id_initialized = 1;
    pr_info("device_id: machine-id = %s\n", machine_id_str);
}

/* Get the machine-id as a hex string.
 * Returns pointer to static 33-byte buffer (32 hex chars + NUL).
 * Must be called after device_id_init(). */
const char* device_id_get_string(void) {
    if (!machine_id_initialized) return "00000000000000000000000000000000";
    return machine_id_str;
}

/* Get the raw 16-byte machine-id.
 * Must be called after device_id_init(). */
const uint8_t* device_id_get_bytes(void) {
    return machine_id_bytes;
}

/* Regenerate the machine-id (for testing or manual rotation).
 * This should normally only be called at boot. */
void device_id_regenerate(void) {
    get_random_bytes(machine_id_bytes, MACHINE_ID_LEN);
    for (int i = 0; i < MACHINE_ID_LEN; i++) {
        machine_id_str[i * 2]     = hex_chars[(machine_id_bytes[i] >> 4) & 0x0F];
        machine_id_str[i * 2 + 1] = hex_chars[machine_id_bytes[i] & 0x0F];
    }
    machine_id_str[32] = '\0';
    pr_info("device_id: regenerated machine-id = %s\n", machine_id_str);
}
