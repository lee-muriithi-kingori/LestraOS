#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/timer.h>
#include <string.h>

static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static uint8_t xtime(uint8_t x) { return (x << 1) ^ ((x >> 7) * 0x1b); }

static void aes256_key_expansion(const uint8_t key[32], uint8_t round_keys[240]) {
    memcpy(round_keys, key, 32);
    for (int i = 32; i < 240; i += 4) {
        uint8_t t[4];
        memcpy(t, &round_keys[i-4], 4);
        int wn = i / 4;
        if (wn % 8 == 0) {
            uint8_t tmp = t[0]; t[0]=t[1]; t[1]=t[2]; t[2]=t[3]; t[3]=tmp;
            for (int j = 0; j < 4; j++) t[j] = aes_sbox[t[j]];
            t[0] ^= 0x01 << ((wn/8 - 1) & 7);
        } else if (wn % 4 == 0) {
            for (int j = 0; j < 4; j++) t[j] = aes_sbox[t[j]];
        }
        for (int j = 0; j < 4; j++)
            round_keys[i+j] = round_keys[i-32+j] ^ t[j];
    }
}

static void aes256_encrypt_block(const uint8_t in[16], uint8_t out[16], const uint8_t round_keys[240]) {
    uint8_t state[16];
    memcpy(state, in, 16);
    for (int i = 0; i < 16; i++) state[i] ^= round_keys[i];
    for (int round = 1; round <= 14; round++) {
        for (int i = 0; i < 16; i++) state[i] = aes_sbox[state[i]];
        uint8_t t;
        t=state[1]; state[1]=state[5]; state[5]=state[9]; state[9]=state[13]; state[13]=t;
        t=state[2]; state[2]=state[10]; state[10]=t; t=state[6]; state[6]=state[14]; state[14]=t;
        t=state[15]; state[15]=state[11]; state[11]=state[7]; state[7]=state[3]; state[3]=t;
        if (round < 14) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0=state[c*4],a1=state[c*4+1],a2=state[c*4+2],a3=state[c*4+3];
                state[c*4]  =xtime(a0)^xtime(a1)^a1^a2^a3;
                state[c*4+1]=a0^xtime(a1)^xtime(a2)^a2^a3;
                state[c*4+2]=a0^a1^xtime(a2)^xtime(a3)^a3;
                state[c*4+3]=xtime(a0)^a0^a1^a2^xtime(a3);
            }
        }
        for (int i = 0; i < 16; i++) state[i] ^= round_keys[round*16+i];
    }
    memcpy(out, state, 16);
}

static void increment_counter(uint8_t v[16]) {
    for (int i = 15; i >= 0; i--) {
        if (++v[i] != 0) break;
    }
}

static uint8_t drbg_key[32];
static uint8_t drbg_v[16];
static uint64_t reseed_counter = 0;
static uint64_t last_reseed_ms = 0;
static int initialized = 0;

#define RESEED_BYTES  (1024 * 1024)
#define RESEED_MS     60000

/* KE-16: Added RDSEED + interrupt-mixed fast entropy pool.
 * RDSEED (NIST SP 800-90B) provides true hardware entropy on
 * Broadwell+ / Excavator+ CPUs. XOR into existing RDRAND
 * data. CPUID-gated — no-op on CPUs that lack it.
 * Fast pool: 16-slot lock-free XOR accumulator fed by timer/keyboard/mouse
 * IRQs. Drained into DRBG reseed via entropy_drain(). */
#include <lestra/entropy.h>

static int collect_entropy(uint8_t out[48]) {
    uint32_t buf[12];
    int have_hw = 0;

    /* Primary: RDRAND (12 × 32 bits = 384 bits hardware entropy) */
    for (int i = 0; i < 12; i++) {
        if (rdrand32(&buf[i])) have_hw++;
    }

    /* Secondary: RDSEED — true hardware entropy per NIST SP 800-90B.
     * XOR into first 6 slots (192 bits) to supplement RDRAND.
     * CPUID-gated: no-op on CPUs that lack it. */
    if (cpu_has_rdseed()) {
        uint32_t seed;
        for (int i = 0; i < 6; i++) {
            if (rdseed32(&seed)) buf[i] ^= seed;
        }
    }

    /* Tertiary: interrupt-mixed fast entropy pool.
     * Timer IRQ (1 kHz) provides TSC jitter between fires — real hardware
     * timing entropy. Keyboard/mouse IRQs provide event timing.
     * Folded into 48 bytes via entropy_drain() and XOR-mixed with
     * RDRAND/RDSEED data. */
    uint8_t pool_bytes[48];
    entropy_drain(pool_bytes, 48);
    for (int i = 0; i < 12; i++)
        buf[i] ^= ((uint32_t*)pool_bytes)[i];

    if (have_hw < 6) {
        /* INSECURE: TSC-only entropy is predictable in deterministic VMs.
         * Do NOT use cloud/SSH mode as production without real RDRAND
         * or an interrupt-mixed entropy pool. */
        static int warned = 0;
        if (!warned) {
            printk("csprng: WARNING — RDRAND unavailable, using INSECURE TSC fallback\n");
            warned = 1;
        }
        uint64_t tsc = rdtsc();
        uint64_t ms = timer_get_ms();
        uint32_t sp = (uint32_t)(uintptr_t)&tsc;
        for (int i = 0; i < 12; i++)
            buf[i] ^= (uint32_t)(tsc >> (i*5)) ^ (uint32_t)(ms << i) ^ sp ^ (i * 0x61C88647);
    }
    memcpy(out, buf, 48);
    return have_hw >= 6;
}

static void drbg_generate_block(uint8_t out[16]) {
    increment_counter(drbg_v);
    uint8_t rk[240];
    aes256_key_expansion(drbg_key, rk);
    aes256_encrypt_block(drbg_v, out, rk);
}

static void drbg_update(const uint8_t provided_data[48]) {
    uint8_t temp[48];
    uint8_t block[16];
    for (int i = 0; i < 3; i++) {
        increment_counter(drbg_v);
        uint8_t rk[240];
        aes256_key_expansion(drbg_key, rk);
        aes256_encrypt_block(drbg_v, block, rk);
        memcpy(temp + i * 16, block, 16);
    }
    for (int i = 0; i < 32; i++)
        drbg_key[i] ^= provided_data[i];
    for (int i = 0; i < 16; i++)
        drbg_v[i] ^= provided_data[32 + i];
    for (int i = 0; i < 32; i++)
        drbg_key[i] ^= temp[i];
    for (int i = 0; i < 16; i++)
        drbg_v[i] ^= temp[32 + i];
}

static void drbg_instantiate(const uint8_t entropy[48]) {
    memset(drbg_key, 0, 32);
    memset(drbg_v, 0, 16);
    uint8_t seed[48];
    memcpy(seed, entropy, 48);
    drbg_update(seed);
}

static void drbg_reseed_internal(const uint8_t entropy[48]) {
    uint8_t seed[48];
    memcpy(seed, entropy, 48);
    drbg_update(seed);
    reseed_counter = 0;
    last_reseed_ms = timer_get_ms();
}

void csprng_init(void) {
    uint8_t entropy[48];
    int hw = collect_entropy(entropy);
    drbg_instantiate(entropy);
    reseed_counter = 0;
    last_reseed_ms = timer_get_ms();
    initialized = 1;
    if (hw)
        pr_info("csprng: initialized (RDRAND)\n");
    else
        pr_info("csprng: initialized (TSC fallback)\n");
}

void csprng_reseed(void) {
    uint8_t entropy[48];
    collect_entropy(entropy);
    drbg_reseed_internal(entropy);
}

int csprng_generate(void* buf, size_t len) {
    if (!initialized) csprng_init();

    uint64_t now = timer_get_ms();
    if (reseed_counter >= RESEED_BYTES || (now - last_reseed_ms) >= RESEED_MS)
        csprng_reseed();

    uint8_t* out = (uint8_t*)buf;
    uint8_t block[16];
    size_t remaining = len;
    while (remaining > 0) {
        drbg_generate_block(block);
        size_t chunk = remaining < 16 ? remaining : 16;
        memcpy(out, block, chunk);
        out += chunk;
        remaining -= chunk;
        reseed_counter += chunk;
    }

    uint8_t update_data[48];
    for (int i = 0; i < 3; i++)
        drbg_generate_block(update_data + i * 16);
    drbg_update(update_data);

    return 0;
}

uint64_t csprng_u64(void) {
    uint64_t val;
    csprng_generate(&val, sizeof(val));
    return val;
}

void get_random_bytes(void* buf, size_t len) {
    csprng_generate(buf, len);
}
