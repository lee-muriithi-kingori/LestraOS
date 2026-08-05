/*
 * Lestra OS — Fast Interrupt-Driven Entropy Pool
 *
 * KE-16: Collects timing jitter from IRQ handlers into a lock-free
 * XOR accumulator. Periodically drained into the AES-CTR DRBG on reseed
 * to improve entropy quality beyond the RDRAND/TSC-only source.
 *
 * Architecture:
 *   16 uint64_t slots (1024 bits total). Each IRQ XORs a TSC delta
 *   plus an IRQ number and a per-slot counter into one rotating slot.
 *   On DRBG reseed, all 16 slots are folded into a 48-byte
 *   seed and mixed with RDRAND/RDSEED data by collect_entropy().
 *
 * Zero-allocation: the pool is a static BSS array. IRQ handlers
 *   do one pointer write + one XOR + one pointer read per invocation (~25 cycles).
 * No locks, no atomic ops, no AES in the hot path.
 */

#include <lestra/entropy.h>
#include <lestra/printk.h>
#include <string.h>

/* BSS pool — zero-allocation, lock-free. Non-static so the inline
 * entropy_mix_irq() in entropy.h can reference it via extern. */
uint64_t entropy_pool[ENTROPY_SLOTS];

void entropy_drain(uint8_t* out, int len) {
    /* Fold all 16 uint64_t slots (128 bytes) into the output buffer.
     * Strategy: XOR-accumulate into 6 uint64_t accumulators (48 bytes),
     * each accumulator mixing different byte-shifts of the pool slots
     * for better diffusion.
     *
     * acc[0..5] = 6 × uint64_t = 48 bytes total */
    uint64_t acc[6] = {0};
    for (int i = 0; i < ENTROPY_SLOTS; i++) {
        uint64_t v = entropy_pool[i];
        acc[0] ^= v;
        acc[1] ^= (v >> 3)  ^ (v << 5);
        acc[2] ^= (v >> 11) ^ (v << 13);
        acc[3] ^= (v >> 17) ^ (v << 15);
        acc[4] ^= (v >> 23) ^ (v << 9);
        acc[5] ^= (v >> 29) ^ (v << 19);
    }

    /* Zero-fill first, then XOR the accumulators in */
    memset(out, 0, len);
    int bytes = len;
    if (bytes > 0) { int n = bytes < 8 ? bytes : 8; memcpy(out, acc, n); bytes -= n; }
    if (bytes > 0) { int n = bytes < 8 ? bytes : 8; memcpy(out + 8, acc + 1, n); bytes -= n; }
    if (bytes > 0) { int n = bytes < 8 ? bytes : 8; memcpy(out + 16, acc + 2, n); bytes -= n; }
    if (bytes > 0) { int n = bytes < 8 ? bytes : 8; memcpy(out + 24, acc + 3, n); bytes -= n; }
    if (bytes > 0) { int n = bytes < 8 ? bytes : 8; memcpy(out + 32, acc + 4, n); bytes -= n; }
    if (bytes > 0) { int n = bytes < 8 ? bytes : 8; memcpy(out + 40, acc + 5, n); bytes -= n; }
}
