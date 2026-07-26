/*
 * Lestra OS - RSA PKCS#1 v1.5 Signature Verification
 * Copyright (c) 2026 lestramk.org
 *
 * 2048-bit RSA with big number arithmetic on 256-byte
 * big-endian arrays. Schoolbook multiplication with
 * shift-and-subtract modular reduction.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <string.h>

/* ===== Big number arithmetic (256-byte, big-endian) ===== */

static void bn_zero(uint8_t r[256]) {
    memset(r, 0, 256);
}

static int bn_is_zero(const uint8_t a[256]) {
    for (int i = 0; i < 256; i++)
        if (a[i] != 0) return 0;
    return 1;
}

static int bn_cmp(const uint8_t a[256], const uint8_t b[256]) {
    for (int i = 0; i < 256; i++) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

static void bn_copy(uint8_t dst[256], const uint8_t src[256]) {
    memcpy(dst, src, 256);
}

/* r = (a * b) mod m -- schoolbook multiply then shift-and-subtract reduce */
static void bn_mod_mul(const uint8_t a[256], const uint8_t b[256],
                       const uint8_t m[256], uint8_t r[256]) {
    uint8_t prod[512];
    memset(prod, 0, 512);

    for (int i = 255; i >= 0; i--) {
        uint32_t carry = 0;
        for (int j = 255; j >= 0; j--) {
            int idx = i + j + 1;
            uint32_t v = (uint32_t)a[i] * (uint32_t)b[j] + prod[idx] + carry;
            prod[idx] = v & 0xFF;
            carry = v >> 8;
        }
        for (int k = i; k >= 0 && carry; k--) {
            uint32_t v = (uint32_t)prod[k] + carry;
            prod[k] = v & 0xFF;
            carry = v >> 8;
        }
    }

    uint8_t mshift[512];
    for (int shift = 256; shift >= 0; shift--) {
        memset(mshift, 0, 512);
        for (int i = 0; i < 256; i++) {
            if (i + shift < 512)
                mshift[i + shift] = m[i];
        }
        while (memcmp(prod, mshift, 512) >= 0) {
            int32_t borrow = 0;
            for (int idx = 511; idx >= 0; idx--) {
                int32_t diff = (int32_t)prod[idx] - (int32_t)mshift[idx] - borrow;
                if (diff < 0) { diff += 256; borrow = 1; } else { borrow = 0; }
                prod[idx] = (uint8_t)diff;
            }
        }
    }
    memcpy(r, prod + 256, 256);
}

/* r = base^exp mod mod -- left-to-right square-and-multiply */
static void bn_mod_exp(const uint8_t base[256], const uint8_t exp[256],
                       const uint8_t mod[256], uint8_t r[256]) {
    uint8_t result[256];
    uint8_t tmp[256];
    bn_zero(result);
    result[255] = 1;

    if (bn_is_zero(exp)) { bn_zero(r); r[255] = 1; return; }

    int start = -1;
    for (int i = 0; i < 256 && start < 0; i++)
        for (int bit = 7; bit >= 0; bit--)
            if ((exp[i] >> bit) & 1) { start = i * 8 + bit; goto done; }
done:
    if (start < 0) { bn_zero(r); r[255] = 1; return; }

    for (int pos = start; pos >= 0; pos--) {
        int byte_idx = pos / 8;
        int bit_idx = pos % 8;
        bn_mod_mul(result, result, mod, tmp);
        bn_copy(result, tmp);
        if ((exp[byte_idx] >> bit_idx) & 1) {
            bn_mod_mul(result, base, mod, tmp);
            bn_copy(result, tmp);
        }
    }
    bn_copy(r, result);
}

/* ===== RSA PKCS#1 v1.5 verification ===== */

int rsa_verify_pkcs1_v15(const uint8_t msg_hash[32],
                          const uint8_t sig[256],
                          const uint8_t pubkey_n[256],
                          const uint32_t pubkey_e) {
    uint8_t m[256];
    uint8_t e_bn[256];
    uint8_t expected[256];

    bn_zero(e_bn);
    e_bn[255] = (uint8_t)(pubkey_e & 0xFF);
    e_bn[254] = (uint8_t)((pubkey_e >> 8) & 0xFF);
    e_bn[253] = (uint8_t)((pubkey_e >> 16) & 0xFF);
    e_bn[252] = (uint8_t)((pubkey_e >> 24) & 0xFF);

    bn_mod_exp(sig, e_bn, pubkey_n, m);

    static const uint8_t digest_info_sha256[19] = {
        0x30, 0x31, 0x30, 0x0D, 0x06, 0x09, 0x60, 0x86,
        0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
        0x00, 0x04, 0x20
    };

    memset(expected, 0, 256);
    expected[0] = 0x00;
    expected[1] = 0x01;
    memset(&expected[2], 0xFF, 202);
    expected[204] = 0x00;
    memcpy(&expected[205], digest_info_sha256, 19);
    memcpy(&expected[224], msg_hash, 32);

    uint8_t diff = 0;
    for (int i = 0; i < 256; i++)
        diff |= m[i] ^ expected[i];
    return (diff == 0) ? 1 : 0;
}
