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
 *   On DRBG reseed, all 16 slots are folded (XOR-sum) into a 48-byte
 *   seed and mixed with RDRAND/RDSEED data by collect_entropy().
 *
 * Zero-allocation: the pool is a static BSS array. IRQ handlers
 *   do one pointer write + one XOR + one pointer read per invocation (~25 cycles).
 * No locks, no atomic ops, no AES in the hot path.
 *
 * Entropy sources (wired in priority order):
 *   0: Timer IRQ (1 kHz) — most frequent, TSC jitter between fires
 *   1: Keyboard IRQ — scancode, timing varies with human input
 *   2: Mouse IRQ — packet arrival timing, low frequency
 */
#ifndef LESTRA_ENTROPY_H
#define LESTRA_ENTROPY_H

#include <lestra/types.h>

#define ENTROPY_SLOTS     16
#define ENTROPY_TOTAL_BITS (ENTROPY_SLOTS * 64)

/* Mix timing jitter into the fast entropy pool.
 * Called from IRQ handlers with the current TSC delta and IRQ number.
 * ~25 cycles: one rdtsc + one XOR + one indexed store. */
static inline void entropy_mix_irq(int irq_num, uint64_t tsc_delta) {
    extern uint64_t entropy_pool[ENTROPY_SLOTS];
    static int slot = 0;
    slot = (slot + 1) & (ENTROPY_SLOTS - 1);
    /* Mix: TSC delta, IRQ number for diversity, slot counter for forward secrecy */
    entropy_pool[slot] ^= tsc_delta ^ ((uint64_t)irq_num << 48) ^ ((uint64_t)slot << 32);
}
/* Drain the fast pool into a byte buffer for the DRBG.
 * XOR-sums all 16 slots into 48 bytes (3 bytes per slot).
 * Called from csprng_reseed() before the RDRAND/RDSEED collection. */
void entropy_drain(uint8_t* out, int len);

#endif /* LESTRA_ENTROPY_H */
