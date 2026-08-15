/*
 * Lestra OS - TLS 1.2 Server (for sandbox HTTP server)
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Self-contained TLS 1.2 server with:
 *   - Duplicated crypto from tls.c (AES-128-GCM, SHA-256, HMAC, P-256 ECDHE)
 *   - Self-signed X.509 certificate generation (DER)
 *   - Full TLS 1.2 server handshake
 *   - Encrypted application data send/recv
 *
 * Cipher: TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 (0xC02F)
 *
 * Crypto primitives duplicated from tls.c to avoid making them non-static.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/net.h>
#include <string.h>

extern void get_random_bytes(void* buf, size_t len);
extern void p256_keygen(uint8_t priv[32], uint8_t pub_x[32], uint8_t pub_y[32]);
extern int  p256_ecdh(const uint8_t priv[32], const uint8_t peer_x[32],
                       const uint8_t peer_y[32], uint8_t shared[32]);
extern int  p256_selftest(void);

/* ===== SHA-256 (duplicated from tls.c) ===== */
struct sha256_ctx {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t buffer[64];
};

static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_init(struct sha256_ctx* ctx) {
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85;
    ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c;
    ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
    ctx->bitcount = 0;
}

static void sha256_block(struct sha256_ctx* ctx, const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)|((uint32_t)block[i*4+2]<<8)|block[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1 = rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
        w[i] = w[i-16]+s0+w[i-7]+s1;
    }
    uint32_t a=ctx->state[0],b=ctx->state[1],c=ctx->state[2],d=ctx->state[3];
    uint32_t e=ctx->state[4],f=ctx->state[5],g=ctx->state[6],h=ctx->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1=rotr(e,6)^rotr(e,11)^rotr(e,25);
        uint32_t ch=(e&f)^(~e&g);
        uint32_t t1=h+S1+ch+sha256_k[i]+w[i];
        uint32_t S0=rotr(a,2)^rotr(a,13)^rotr(a,22);
        uint32_t maj=(a&b)^(a&c)^(b&c);
        uint32_t t2=S0+maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
    ctx->bitcount += 512;
}

static void sha256_update(struct sha256_ctx* ctx, const uint8_t* data, uint32_t len) {
    uint32_t buf_pos = ctx->bitcount / 8 % 64;
    while (len > 0) {
        uint32_t to_copy = 64 - buf_pos;
        if (to_copy > len) to_copy = len;
        memcpy(&ctx->buffer[buf_pos], data, to_copy);
        buf_pos += to_copy; data += to_copy; len -= to_copy;
        if (buf_pos == 64) { sha256_block(ctx, ctx->buffer); buf_pos = 0; }
    }
}

static void sha256_final(struct sha256_ctx* ctx, uint8_t out[32]) {
    uint32_t buf_pos = ctx->bitcount / 8 % 64;
    ctx->buffer[buf_pos++] = 0x80;
    if (buf_pos > 56) { while (buf_pos < 64) ctx->buffer[buf_pos++] = 0; sha256_block(ctx, ctx->buffer); buf_pos = 0; }
    while (buf_pos < 56) ctx->buffer[buf_pos++] = 0;
    uint64_t bc = ctx->bitcount;
    for (int i = 7; i >= 0; i--) { ctx->buffer[56+i] = bc & 0xFF; bc >>= 8; }
    sha256_block(ctx, ctx->buffer);
    for (int i = 0; i < 8; i++) {
        out[i*4]  =(ctx->state[i]>>24)&0xFF; out[i*4+1]=(ctx->state[i]>>16)&0xFF;
        out[i*4+2]=(ctx->state[i]>>8)&0xFF;  out[i*4+3]=ctx->state[i]&0xFF;
    }
}

static void sha256_hash(const uint8_t* data, uint32_t len, uint8_t out[32]) {
    struct sha256_ctx ctx; sha256_init(&ctx); sha256_update(&ctx, data, len); sha256_final(&ctx, out);
}

/* ===== HMAC-SHA256 ===== */
static void hmac_sha256(const uint8_t* key, uint32_t key_len, const uint8_t* data, uint32_t data_len, uint8_t out[32]) {
    uint8_t k[64]; memset(k, 0, 64);
    if (key_len > 64) { sha256_hash(key, key_len, k); } else { memcpy(k, key, key_len); }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }
    struct sha256_ctx ctx; sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64); sha256_update(&ctx, data, data_len);
    uint8_t inner[32]; sha256_final(&ctx, inner);
    sha256_init(&ctx); sha256_update(&ctx, opad, 64); sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
}

/* ===== AES-128 ===== */
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

static void aes128_key_expansion(const uint8_t key[16], uint8_t round_keys[176]) {
    memcpy(round_keys, key, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, &round_keys[i-4], 4);
        if (i % 16 == 0) {
            uint8_t tmp = t[0]; t[0]=t[1]; t[1]=t[2]; t[2]=t[3]; t[3]=tmp;
            for (int j = 0; j < 4; j++) t[j] = aes_sbox[t[j]];
            t[0] ^= 0x01 << ((i/16 - 1) & 7);
        }
        for (int j = 0; j < 4; j++)
            round_keys[i+j] = round_keys[i-16+j] ^ t[j];
    }
}

static uint8_t xtime(uint8_t x) { return (x << 1) ^ ((x >> 7) * 0x1b); }

static void aes_encrypt_block(const uint8_t in[16], uint8_t out[16], const uint8_t round_keys[176]) {
    uint8_t state[16];
    memcpy(state, in, 16);
    for (int i = 0; i < 16; i++) state[i] ^= round_keys[i];
    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) state[i] = aes_sbox[state[i]];
        uint8_t t;
        t=state[1]; state[1]=state[5]; state[5]=state[9]; state[9]=state[13]; state[13]=t;
        t=state[2]; state[2]=state[10]; state[10]=t; t=state[6]; state[6]=state[14]; state[14]=t;
        t=state[15]; state[15]=state[11]; state[11]=state[7]; state[7]=state[3]; state[3]=t;
        if (round < 10) {
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

/* ===== AES-128-GCM ===== */
static void ghash_mult(const uint8_t X[16], const uint8_t H[16], uint8_t out[16]) {
    uint8_t Z[16]; memset(Z, 0, 16);
    for (int i = 0; i < 128; i++) {
        if (X[i/8] & (0x80 >> (i%8))) {
            for (int j = 0; j < 16; j++) Z[j] ^= H[j];
        }
        uint8_t lsb = Z[15] & 1;
        for (int j = 15; j > 0; j--) Z[j] = (Z[j] >> 1) | ((Z[j-1] & 1) << 7);
        Z[0] >>= 1;
        if (lsb) Z[0] ^= 0xe1;
    }
    memcpy(out, Z, 16);
}

static void ghash_update(uint8_t Y[16], const uint8_t* data, uint32_t len, const uint8_t H[16]) {
    uint32_t i = 0;
    while (i + 16 <= len) {
        for (int j = 0; j < 16; j++) Y[j] ^= data[i+j];
        uint8_t tmp[16]; ghash_mult(Y, H, tmp); memcpy(Y, tmp, 16);
        i += 16;
    }
    if (i < len) {
        uint8_t block[16]; memset(block, 0, 16);
        memcpy(block, data+i, len-i);
        for (int j = 0; j < 16; j++) Y[j] ^= block[j];
        uint8_t tmp[16]; ghash_mult(Y, H, tmp); memcpy(Y, tmp, 16);
    }
}

static void aes128_gcm_encrypt(const uint8_t key[16], const uint8_t iv[12],
                                const uint8_t* aad, uint32_t aad_len,
                                const uint8_t* plaintext, uint32_t pt_len,
                                uint8_t* ciphertext, uint8_t tag[16]) {
    uint8_t round_keys[176]; aes128_key_expansion(key, round_keys);
    uint8_t H[16]; memset(H, 0, 16);
    aes_encrypt_block(H, H, round_keys);
    uint8_t J0[16]; memcpy(J0, iv, 12); J0[12]=0; J0[13]=0; J0[14]=0; J0[15]=2;

    uint8_t counter[16]; memcpy(counter, J0, 16);
    uint32_t i = 0;
    while (i < pt_len) {
        for (int j = 15; j >= 12; j--) { if (++counter[j]) break; }
        uint8_t ks[16]; aes_encrypt_block(counter, ks, round_keys);
        uint32_t block_len = (pt_len - i < 16) ? pt_len - i : 16;
        for (uint32_t j = 0; j < block_len; j++) ciphertext[i+j] = plaintext[i+j] ^ ks[j];
        i += block_len;
    }

    uint8_t Y[16]; memset(Y, 0, 16);
    if (aad_len > 0) ghash_update(Y, aad, aad_len, H);
    if (pt_len > 0) ghash_update(Y, ciphertext, pt_len, H);
    uint8_t len_block[16];
    uint64_t aad_bits = (uint64_t)aad_len * 8;
    uint64_t ct_bits = (uint64_t)pt_len * 8;
    for (int j = 0; j < 8; j++) len_block[j] = (aad_bits >> (56-8*j)) & 0xFF;
    for (int j = 0; j < 8; j++) len_block[8+j] = (ct_bits >> (56-8*j)) & 0xFF;
    for (int j = 0; j < 16; j++) Y[j] ^= len_block[j];
    uint8_t tmp[16]; ghash_mult(Y, H, tmp); memcpy(Y, tmp, 16);

    uint8_t EJ0[16]; aes_encrypt_block(J0, EJ0, round_keys);
    for (int j = 0; j < 16; j++) tag[j] = EJ0[j] ^ Y[j];
}

static int aes128_gcm_decrypt(const uint8_t key[16], const uint8_t iv[12],
                               const uint8_t* aad, uint32_t aad_len,
                               const uint8_t* ciphertext, uint32_t ct_len,
                               const uint8_t tag[16], uint8_t* plaintext) {
    uint8_t round_keys[176]; aes128_key_expansion(key, round_keys);
    uint8_t H[16]; memset(H, 0, 16);
    aes_encrypt_block(H, H, round_keys);
    uint8_t J0[16]; memcpy(J0, iv, 12); J0[12]=0; J0[13]=0; J0[14]=0; J0[15]=2;

    uint8_t Y[16]; memset(Y, 0, 16);
    if (aad_len > 0) ghash_update(Y, aad, aad_len, H);
    if (ct_len > 0) ghash_update(Y, ciphertext, ct_len, H);
    uint8_t len_block[16];
    uint64_t aad_bits = (uint64_t)aad_len * 8;
    uint64_t ct_bits = (uint64_t)ct_len * 8;
    for (int j = 0; j < 8; j++) len_block[j] = (aad_bits >> (56-8*j)) & 0xFF;
    for (int j = 0; j < 8; j++) len_block[8+j] = (ct_bits >> (56-8*j)) & 0xFF;
    for (int j = 0; j < 16; j++) Y[j] ^= len_block[j];
    uint8_t tmp[16]; ghash_mult(Y, H, tmp); memcpy(Y, tmp, 16);
    uint8_t EJ0[16]; aes_encrypt_block(J0, EJ0, round_keys);
    uint8_t computed_tag[16];
    for (int j = 0; j < 16; j++) computed_tag[j] = EJ0[j] ^ Y[j];

    uint8_t diff = 0;
    for (int j = 0; j < 16; j++) diff |= computed_tag[j] ^ tag[j];
    if (diff != 0) return 0;

    uint8_t counter[16]; memcpy(counter, J0, 16);
    uint32_t i = 0;
    while (i < ct_len) {
        for (int j = 15; j >= 12; j--) { if (++counter[j]) break; }
        uint8_t ks[16]; aes_encrypt_block(counter, ks, round_keys);
        uint32_t block_len = (ct_len - i < 16) ? ct_len - i : 16;
        for (uint32_t j = 0; j < block_len; j++) plaintext[i+j] = ciphertext[i+j] ^ ks[j];
        i += block_len;
    }
    return 1;
}

/* ===== TLS PRF (RFC 5246) ===== */
static void p_hash(const uint8_t* secret, uint32_t secret_len,
                   const uint8_t* seed, uint32_t seed_len,
                   uint8_t* out, uint32_t out_len) {
    uint8_t A[32];
    memcpy(A, seed, seed_len < 32 ? seed_len : 32);
    hmac_sha256(secret, secret_len, seed, seed_len, A);

    uint32_t offset = 0;
    while (offset < out_len) {
        uint8_t combined[32 + 128];
        memcpy(combined, A, 32);
        memcpy(combined + 32, seed, seed_len < 128 ? seed_len : 128);
        uint8_t block[32];
        hmac_sha256(secret, secret_len, combined, 32 + seed_len, block);

        uint32_t to_copy = out_len - offset;
        if (to_copy > 32) to_copy = 32;
        memcpy(out + offset, block, to_copy);
        offset += to_copy;

        hmac_sha256(secret, secret_len, A, 32, A);
    }
}

static void tls_prf(const uint8_t* secret, uint32_t secret_len,
                    const char* label, const uint8_t* seed, uint32_t seed_len,
                    uint8_t* out, uint32_t out_len) {
    uint32_t label_len = strlen(label);
    uint8_t combined[256];
    memcpy(combined, label, label_len);
    memcpy(combined + label_len, seed, seed_len);
    p_hash(secret, secret_len, combined, label_len + seed_len, out, out_len);
}

/* ===== TLS Record Layer Constants ===== */
#define TLS_RECORD_HANDSHAKE     22
#define TLS_RECORD_CHANGE_CIPHER 20
#define TLS_RECORD_ALERT         21
#define TLS_RECORD_APPLICATION   23

#define TLS_HS_CLIENT_HELLO        1
#define TLS_HS_SERVER_HELLO        2
#define TLS_HS_CERTIFICATE        11
#define TLS_HS_SERVER_KEY_EXCH    12
#define TLS_HS_SERVER_HELLO_DONE  14
#define TLS_HS_CLIENT_KEY_EXCH    16
#define TLS_HS_FINISHED           20

#define TLS_ALERT_WARNING  1
#define TLS_ALERT_FATAL    2
#define TLS_ALERT_CLOSE_NOTIFY           0
#define TLS_ALERT_BAD_RECORD_MAC        20
#define TLS_ALERT_HANDSHAKE_FAILURE     40

/* ===== DER/ASN.1 helpers for X.509 cert ===== */
static int der_write_tag_len(uint8_t* buf, uint8_t tag, uint16_t len) {
    int off = 0;
    buf[off++] = tag;
    if (len < 0x80) {
        buf[off++] = (uint8_t)len;
    } else if (len < 0x100) {
        buf[off++] = 0x81;
        buf[off++] = (uint8_t)len;
    } else {
        buf[off++] = 0x82;
        buf[off++] = (uint8_t)(len >> 8);
        buf[off++] = (uint8_t)(len & 0xFF);
    }
    return off;
}

static int der_write_int(uint8_t* buf, uint32_t val) {
    uint8_t tmp[5];
    int len = 0;
    if (val == 0) { tmp[len++] = 0; }
    else {
        while (val > 0) { tmp[len++] = val & 0xFF; val >>= 8; }
        for (int i = 0; i < len/2; i++) { uint8_t t = tmp[i]; tmp[i] = tmp[len-1-i]; tmp[len-1-i] = t; }
    }
    int off = der_write_tag_len(buf, 0x02, (uint16_t)len);
    memcpy(buf + off, tmp, len);
    return off + len;
}

/* ===== Server Certificate ===== */
struct tls_server_cert {
    uint8_t private_key[32];
    uint8_t public_key[64];      /* x || y */
    uint8_t cert_der[1024];
    uint16_t cert_len;
};

static struct tls_server_cert server_cert;
static int cert_initialized = 0;

static void server_cert_generate(void) {
    if (cert_initialized) return;

    if (!p256_selftest()) {
        pr_warn("tls_server: P-256 self-test FAILED\n");
        return;
    }

    p256_keygen(server_cert.private_key,
                server_cert.public_key,
                server_cert.public_key + 32);
    pr_info("tls_server: generated P-256 key pair\n");

    /* Build self-signed X.509 cert in DER.
     * We build a minimal cert that's structurally valid.
     * Since we don't have ECDSA signing, we'll use a zero signature
     * (self-signed anyway, client skips verification). */
    uint8_t* d = server_cert.cert_der;
    int off = 0;

    /* We need to build the TBS first to know its length,
     * then wrap it in Certificate ::= SEQUENCE { tbs, sigAlg, sig } */
    uint8_t tbs[512];
    int tbs_off = 0;

    /* version [0] EXPLICIT INTEGER { v1(0) } */
    tbs_off += der_write_tag_len(tbs + tbs_off, 0xA0, 3);
    tbs_off += der_write_int(tbs + tbs_off, 0);

    /* serialNumber INTEGER 1 */
    tbs_off += der_write_int(tbs + tbs_off, 1);

    /* signatureAlgorithm: id-ecdsaWithSHA256 (1.2.840.10045.4.3.2) */
    {
        uint8_t alg[] = { 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };
        memcpy(tbs + tbs_off, alg, sizeof(alg));
        tbs_off += sizeof(alg);
    }

    /* issuer = CN = "LestraOS Self-Signed CA" */
    {
        const char* cn = "LestraOS Self-Signed CA";
        uint8_t cn_len = (uint8_t)strlen(cn);
        /* SEQUENCE { SET { SEQUENCE { OID(2.5.4.3), UTF8String } } } */
        uint8_t cn_der[64];
        int cn_off = 0;
        /* UTF8String */
        cn_off += der_write_tag_len(cn_der + cn_off, 0x0C, cn_len);
        memcpy(cn_der + cn_off, cn, cn_len);
        cn_off += cn_len;
        /* SEQUENCE { OID(2.5.4.3), UTF8String } */
        uint8_t seq[80];
        int seq_off = 0;
        uint8_t oid[] = { 0x06, 0x03, 0x55, 0x04, 0x03 };
        memcpy(seq + seq_off, oid, sizeof(oid));
        seq_off += sizeof(oid);
        memcpy(seq + seq_off, cn_der, cn_off);
        seq_off += cn_off;
        tbs_off += der_write_tag_len(tbs + tbs_off, 0x30, (uint16_t)seq_off);
        memcpy(tbs + tbs_off, seq, seq_off);
        tbs_off += seq_off;
        /* SET */
        int set_total = seq_off;
        uint8_t set_buf[96];
        int set_off = der_write_tag_len(set_buf, 0x31, (uint16_t)set_total);
        memcpy(set_buf + set_off, seq, seq_off);
        set_off += seq_off;
        tbs_off += der_write_tag_len(tbs + tbs_off, 0x30, (uint16_t)set_off);
        memcpy(tbs + tbs_off, set_buf, set_off);
        tbs_off += set_off;
    }

    /* validity: SEQUENCE { notBefore, notAfter } */
    {
        /* UTCTime: 20260101000000Z and 20360101000000Z */
        uint8_t nb[] = { 0x17, 0x0d, '2', '6', '0', '1', '0', '1', '0', '0', '0', '0', '0', '0', 'Z' };
        uint8_t na[] = { 0x17, 0x0d, '3', '6', '0', '1', '0', '1', '0', '0', '0', '0', '0', '0', 'Z' };
        int validity_len = sizeof(nb) + sizeof(na);
        tbs_off += der_write_tag_len(tbs + tbs_off, 0x30, (uint16_t)validity_len);
        memcpy(tbs + tbs_off, nb, sizeof(nb));
        tbs_off += sizeof(nb);
        memcpy(tbs + tbs_off, na, sizeof(na));
        tbs_off += sizeof(na);
    }

    /* subject = same as issuer */
    {
        const char* cn = "LestraOS Self-Signed CA";
        uint8_t cn_len = (uint8_t)strlen(cn);
        uint8_t cn_der[64];
        int cn_off = 0;
        cn_off += der_write_tag_len(cn_der + cn_off, 0x0C, cn_len);
        memcpy(cn_der + cn_off, cn, cn_len);
        cn_off += cn_len;
        uint8_t seq[80];
        int seq_off = 0;
        uint8_t oid[] = { 0x06, 0x03, 0x55, 0x04, 0x03 };
        memcpy(seq + seq_off, oid, sizeof(oid));
        seq_off += sizeof(oid);
        memcpy(seq + seq_off, cn_der, cn_off);
        seq_off += cn_off;
        tbs_off += der_write_tag_len(tbs + tbs_off, 0x30, (uint16_t)seq_off);
        memcpy(tbs + tbs_off, seq, seq_off);
        tbs_off += seq_off;
        int set_total = seq_off;
        uint8_t set_buf[96];
        int set_off = der_write_tag_len(set_buf, 0x31, (uint16_t)set_total);
        memcpy(set_buf + set_off, seq, seq_off);
        set_off += seq_off;
        tbs_off += der_write_tag_len(tbs + tbs_off, 0x30, (uint16_t)set_off);
        memcpy(tbs + tbs_off, set_buf, set_off);
        tbs_off += set_off;
    }

    /* subjectPublicKeyInfo: EC public key */
    {
        /* AlgorithmIdentifier: ecPublicKey (1.2.840.10045.2.1) + P-256 (1.2.840.10045.3.1.7) */
        uint8_t alg[] = { 0x30, 0x13,
            0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
            0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07
        };
        /* BIT STRING with 65-byte uncompressed point */
        uint8_t pubkey_point[66];
        pubkey_point[0] = 0x00; /* no unused bits */
        pubkey_point[1] = 0x04; /* uncompressed */
        memcpy(pubkey_point + 2, server_cert.public_key, 64);

        int spki_body = sizeof(alg) + 2 + 1 + 65; /* alg + BIT STRING tag+len + unused bits + point */
        tbs_off += der_write_tag_len(tbs + tbs_off, 0x30, (uint16_t)spki_body);
        memcpy(tbs + tbs_off, alg, sizeof(alg));
        tbs_off += sizeof(alg);
        tbs_off += der_write_tag_len(tbs + tbs_off, 0x03, 66);
        tbs_off += der_write_tag_len(tbs + tbs_off, 0x00, 65);
        memcpy(tbs + tbs_off, pubkey_point + 1, 65);
        tbs_off += 65;
    }

    /* Wrap TBS in SEQUENCE */
    int tbs_total = tbs_off;
    off += der_write_tag_len(d + off, 0x30, (uint16_t)tbs_total);
    memcpy(d + off, tbs, tbs_off);
    off += tbs_off;

    /* signatureAlgorithm (same as in TBS) */
    {
        uint8_t alg[] = { 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };
        memcpy(d + off, alg, sizeof(alg));
        off += sizeof(alg);
    }

    /* signatureValue: BIT STRING with 64-byte zeroed ECDSA signature (self-signed, skip verification) */
    off += der_write_tag_len(d + off, 0x03, 66);
    d[off++] = 0x00; /* 0 unused bits */
    /* ECDSA sig is SEQUENCE { INTEGER r, INTEGER s } */
    /* Build minimal: SEQUENCE { INTEGER(0x20 zeros), INTEGER(0x20 zeros) } */
    uint8_t sig_inner[66];
    int si = 0;
    sig_inner[si++] = 0x30; /* SEQUENCE */
    sig_inner[si++] = 0x44; /* length = 68 bytes (32+32 + tags) */
    /* INTEGER 0 (r = 0) */
    sig_inner[si++] = 0x02;
    sig_inner[si++] = 0x20; /* 32 bytes */
    memset(sig_inner + si, 0, 32);
    si += 32;
    /* INTEGER 0 (s = 0) */
    sig_inner[si++] = 0x02;
    sig_inner[si++] = 0x20; /* 32 bytes */
    memset(sig_inner + si, 0, 32);
    si += 32;
    memcpy(d + off, sig_inner, si);
    off += si;

    server_cert.cert_len = (uint16_t)off;
    cert_initialized = 1;
    pr_info("tls_server: self-signed cert generated (%u bytes DER)\n", (unsigned)off);
}

/* ===== Server Connection State ===== */
struct tls_server_conn {
    struct tcp_conn* tcp;
    int handshake_done;
    int handshake_step;         /* tracks where we are in the handshake */
    uint8_t client_random[32];
    uint8_t server_random[32];
    uint8_t private_key[32];
    uint8_t public_key[64];
    uint8_t client_pubkey_x[32];
    uint8_t client_pubkey_y[32];
    uint8_t premaster[32];
    uint8_t master_secret[48];
    uint8_t client_key[16];
    uint8_t server_key[16];
    uint8_t client_iv[12];
    uint8_t server_iv[12];
    uint64_t write_seq;
    uint64_t read_seq;
    struct sha256_ctx transcript;
    struct tls_server_cert* cert;
};

/* ===== Server record send/recv (connection-specific TCP) ===== */
static int tls_server_send_record(struct tls_server_conn* ctx,
                                  uint8_t type, const uint8_t* data, uint16_t len) {
    static uint8_t record[8192];
    record[0] = type;
    record[1] = 0x03; record[2] = 0x03; /* TLS 1.2 */
    record[3] = (len >> 8) & 0xFF;
    record[4] = len & 0xFF;
    memcpy(&record[5], data, len);
    int total = 5 + len;
    int sent = 0;
    while (sent < total) {
        int chunk = total - sent; if (chunk > 1400) chunk = 1400;
        int n = tcp_send_conn(ctx->tcp, &record[sent], (uint16_t)chunk);
        if (n <= 0) return 0;
        sent += n;
    }
    return 1;
}

static int tls_server_recv_record(struct tls_server_conn* ctx,
                                  uint8_t* type, uint8_t* data, uint16_t* len,
                                  uint32_t timeout_ms) {
    uint8_t header[5];
    int n = tcp_recv_conn(ctx->tcp, header, 5, timeout_ms);
    if (n < 5) return 0;
    *type = header[0];
    *len = ((uint16_t)header[3] << 8) | header[4];
    if (*len > 16384) return 0;
    n = tcp_recv_conn(ctx->tcp, data, *len, timeout_ms);
    if (n < *len) return 0;
    return 1;
}

static void tls_server_send_alert(struct tls_server_conn* ctx,
                                  uint8_t level, uint8_t desc) {
    uint8_t alert[2];
    alert[0] = level;
    alert[1] = desc;
    tls_server_send_record(ctx, TLS_RECORD_ALERT, alert, 2);
}

/* ===== Server Handshake ===== */

static int tls_server_do_handshake(struct tls_server_conn* ctx) {
    uint8_t rtype, rdata[8192];
    uint16_t rlen;

    /* 1. Receive ClientHello */
    if (!tls_server_recv_record(ctx, &rtype, rdata, &rlen, 10000)) {
        pr_warn("tls_server: no ClientHello\n");
        return 0;
    }
    if (rtype != TLS_RECORD_HANDSHAKE || rdata[0] != TLS_HS_CLIENT_HELLO) {
        pr_warn("tls_server: expected ClientHello, got type=%u hs=%u\n", rtype, rdata[0]);
        return 0;
    }
    pr_info("tls_server: received ClientHello\n");

    /* Parse client random (bytes 4..35 of handshake body) */
    if (rlen >= 38) {
        memcpy(ctx->client_random, &rdata[6], 32);
    }

    /* Check offered cipher suites for 0xC02F */
    if (rlen >= 42) {
        uint16_t cs_len = ((uint16_t)rdata[38] << 8) | rdata[39];
        int found = 0;
        for (uint16_t i = 0; i + 1 < cs_len; i += 2) {
            uint16_t cs = ((uint16_t)rdata[40+i] << 8) | rdata[40+i+1];
            if (cs == 0xC02F) { found = 1; break; }
        }
        if (!found) {
            pr_warn("tls_server: client doesn't support 0xC02F\n");
            tls_server_send_alert(ctx, TLS_ALERT_FATAL, TLS_ALERT_HANDSHAKE_FAILURE);
            return 0;
        }
    }

    sha256_update(&ctx->transcript, rdata, rlen);

    /* 2. Send ServerHello */
    {
        static uint8_t sh[256];
        int len = 0;
        sh[0] = TLS_HS_SERVER_HELLO;
        int body_start = 4; len = body_start;
        sh[len++] = 0x03; sh[len++] = 0x03; /* TLS 1.2 */
        get_random_bytes(ctx->server_random, 32);
        memcpy(&sh[len], ctx->server_random, 32); len += 32;
        sh[len++] = 0; /* session ID length = 0 */
        sh[len++] = 0xC0; sh[len++] = 0x2F; /* selected cipher */
        sh[len++] = 0x00; sh[len++] = 0x00; /* no compression */
        int body_len = len - body_start;
        sh[1] = (body_len >> 16) & 0xFF;
        sh[2] = (body_len >> 8) & 0xFF;
        sh[3] = body_len & 0xFF;
        sha256_update(&ctx->transcript, sh, len);
        if (!tls_server_send_record(ctx, TLS_RECORD_HANDSHAKE, sh, len)) return 0;
        pr_info("tls_server: ServerHello sent\n");
    }

    /* 3. Send Certificate */
    {
        server_cert_generate();
        ctx->cert = &server_cert;

        static uint8_t cert_msg[1200];
        int len = 0;
        cert_msg[0] = TLS_HS_CERTIFICATE;
        int body_start = 4; len = body_start;
        uint16_t cl = server_cert.cert_len;
        cert_msg[len++] = (cl >> 16) & 0xFF;
        cert_msg[len++] = (cl >> 8) & 0xFF;
        cert_msg[len++] = cl & 0xFF;
        cert_msg[len++] = (cl >> 16) & 0xFF;
        cert_msg[len++] = (cl >> 8) & 0xFF;
        cert_msg[len++] = cl & 0xFF;
        memcpy(&cert_msg[len], server_cert.cert_der, cl);
        len += cl;
        int body_len = len - body_start;
        cert_msg[1] = (body_len >> 16) & 0xFF;
        cert_msg[2] = (body_len >> 8) & 0xFF;
        cert_msg[3] = body_len & 0xFF;
        sha256_update(&ctx->transcript, cert_msg, len);
        if (!tls_server_send_record(ctx, TLS_RECORD_HANDSHAKE, cert_msg, len)) return 0;
        pr_info("tls_server: Certificate sent (%u bytes DER)\n", (unsigned)cl);
    }

    /* Generate ECDHE key pair */
    p256_keygen(ctx->private_key, ctx->public_key, ctx->public_key + 32);
    pr_info("tls_server: generated ECDHE key pair\n");

    /* 4. Send ServerKeyExchange */
    {
        static uint8_t ske[256];
        int len = 0;
        ske[0] = TLS_HS_SERVER_KEY_EXCH;
        int body_start = 4; len = body_start;
        /* curve type 3 (named curve), curve 0x0017 (secp256r1) */
        ske[len++] = 3;          /* ECCurveType named_curve */
        ske[len++] = 0x00; ske[len++] = 0x17; /* secp256r1 */
        ske[len++] = 65;         /* point length */
        ske[len++] = 0x04;       /* uncompressed */
        memcpy(&ske[len], ctx->public_key, 64); len += 64;

        int body_len = len - body_start;
        ske[1] = (body_len >> 16) & 0xFF;
        ske[2] = (body_len >> 8) & 0xFF;
        ske[3] = body_len & 0xFF;
        sha256_update(&ctx->transcript, ske, len);
        if (!tls_server_send_record(ctx, TLS_RECORD_HANDSHAKE, ske, len)) return 0;
        pr_info("tls_server: ServerKeyExchange sent\n");
    }

    /* 5. Send ServerHelloDone */
    {
        uint8_t done[4];
        done[0] = TLS_HS_SERVER_HELLO_DONE;
        done[1] = 0; done[2] = 0; done[3] = 0;
        sha256_update(&ctx->transcript, done, 4);
        if (!tls_server_send_record(ctx, TLS_RECORD_HANDSHAKE, done, 4)) return 0;
        pr_info("tls_server: ServerHelloDone sent\n");
    }

    /* 6. Receive ClientKeyExchange */
    if (!tls_server_recv_record(ctx, &rtype, rdata, &rlen, 10000)) {
        pr_warn("tls_server: no ClientKeyExchange\n");
        return 0;
    }
    if (rtype != TLS_RECORD_HANDSHAKE || rdata[0] != TLS_HS_CLIENT_KEY_EXCH) {
        pr_warn("tls_server: expected ClientKeyExchange\n");
        return 0;
    }
    pr_info("tls_server: received ClientKeyExchange\n");

    /* Parse client's ECDHE public key */
    {
        uint8_t* body = rdata + 4;
        uint8_t point_len = body[0];
        if (point_len == 65 && body[1] == 0x04) {
            memcpy(ctx->client_pubkey_x, &body[2], 32);
            memcpy(ctx->client_pubkey_y, &body[34], 32);
            pr_info("tls_server: parsed client P-256 pubkey\n");
        } else {
            pr_warn("tls_server: unexpected client key format\n");
            return 0;
        }
    }
    sha256_update(&ctx->transcript, rdata, rlen);

    /* 7. Compute premaster via ECDH */
    if (p256_ecdh(ctx->private_key, ctx->client_pubkey_x, ctx->client_pubkey_y, ctx->premaster) != 1) {
        pr_warn("tls_server: ECDH failed\n");
        tls_server_send_alert(ctx, TLS_ALERT_FATAL, TLS_ALERT_HANDSHAKE_FAILURE);
        return 0;
    }
    pr_info("tls_server: premaster secret computed\n");

    /* 8. Derive keys */
    {
        uint8_t master_seed[64];
        memcpy(master_seed, ctx->client_random, 32);
        memcpy(master_seed + 32, ctx->server_random, 32);
        tls_prf(ctx->premaster, 32, "master secret", master_seed, 64, ctx->master_secret, 48);
        pr_info("tls_server: master secret derived\n");

        uint8_t key_block[128];
        tls_prf(ctx->master_secret, 48, "key expansion", master_seed, 64, key_block, 72);
        /* Server is the "other side" from client's perspective:
         * client_write_key = what the client uses to encrypt to us = our read key
         * server_write_key = what we use to encrypt to client = our write key
         * In key_block: client_key(16) || server_key(16) || ... || client_iv(4) || server_iv(4)
         * For the server: our "write" key = key_block[16..31], our "read" key = key_block[0..15] */
        memcpy(ctx->client_key, key_block, 16);       /* what client sends (we read) */
        memcpy(ctx->server_key, key_block + 16, 16);  /* what we send (client reads) */
        memcpy(ctx->client_iv, key_block + 40, 12);   /* client's fixed IV */
        memcpy(ctx->server_iv, key_block + 52, 12);   /* server's fixed IV */
        pr_info("tls_server: key material derived\n");
    }

    /* 9. Receive ChangeCipherSpec from client */
    if (!tls_server_recv_record(ctx, &rtype, rdata, &rlen, 10000)) {
        pr_warn("tls_server: no client ChangeCipherSpec\n");
        return 0;
    }
    if (rtype != TLS_RECORD_CHANGE_CIPHER) {
        pr_warn("tls_server: expected ChangeCipherSpec\n");
        return 0;
    }
    pr_info("tls_server: received client ChangeCipherSpec\n");
    /* CCS is not included in the transcript hash */

    /* 10. Receive client Finished (encrypted) */
    if (!tls_server_recv_record(ctx, &rtype, rdata, &rlen, 10000)) {
        pr_warn("tls_server: no client Finished\n");
        return 0;
    }
    pr_info("tls_server: received client Finished (%u bytes)\n", rlen);

    if (rlen < 16) {
        pr_warn("tls_server: client Finished too short\n");
        tls_server_send_alert(ctx, TLS_ALERT_FATAL, TLS_ALERT_HANDSHAKE_FAILURE);
        return 0;
    }

    /* Decrypt client Finished */
    {
        uint8_t iv[12];
        memcpy(iv, ctx->client_iv, 12);
        iv[11] ^= (uint8_t)(ctx->read_seq & 0xFF);
        iv[10] ^= (uint8_t)((ctx->read_seq >> 8) & 0xFF);

        uint8_t decrypted[16];
        if (!aes128_gcm_decrypt(ctx->client_key, iv, NULL, 0,
                                rdata, rlen - 16, &rdata[rlen - 16], decrypted)) {
            pr_warn("tls_server: client Finished GCM verification FAILED\n");
            tls_server_send_alert(ctx, TLS_ALERT_FATAL, TLS_ALERT_BAD_RECORD_MAC);
            return 0;
        }
        ctx->read_seq++;

        /* Compute expected verify_data */
        uint8_t transcript_hash[32];
        struct sha256_ctx saved = ctx->transcript;
        sha256_final(&ctx->transcript, transcript_hash);
        ctx->transcript = saved;

        uint8_t expected[12];
        tls_prf(ctx->master_secret, 48, "client finished", transcript_hash, 32, expected, 12);

        uint8_t diff = 0;
        for (int i = 0; i < 12; i++) diff |= decrypted[i] ^ expected[i];
        if (diff != 0) {
            pr_warn("tls_server: client Finished verify_data mismatch!\n");
            tls_server_send_alert(ctx, TLS_ALERT_FATAL, TLS_ALERT_HANDSHAKE_FAILURE);
            return 0;
        }
        pr_info("tls_server: client Finished verified\n");

        /* Update transcript with the decrypted Finished message */
        static uint8_t fin_hs[16];
        fin_hs[0] = TLS_HS_FINISHED;
        fin_hs[1] = 0; fin_hs[2] = 0; fin_hs[3] = 12;
        memcpy(&fin_hs[4], decrypted, 12);
        sha256_update(&ctx->transcript, fin_hs, 16);
    }

    /* 11. Send ChangeCipherSpec */
    {
        uint8_t ccs = 1;
        if (!tls_server_send_record(ctx, TLS_RECORD_CHANGE_CIPHER, &ccs, 1)) return 0;
        pr_info("tls_server: ChangeCipherSpec sent\n");
    }

    /* 12. Send Finished */
    {
        uint8_t transcript_hash[32];
        struct sha256_ctx saved = ctx->transcript;
        sha256_final(&ctx->transcript, transcript_hash);
        ctx->transcript = saved;

        uint8_t finished_data[12];
        tls_prf(ctx->master_secret, 48, "server finished", transcript_hash, 32, finished_data, 12);

        static uint8_t finished_msg[16];
        finished_msg[0] = TLS_HS_FINISHED;
        finished_msg[1] = 0; finished_msg[2] = 0; finished_msg[3] = 12;
        memcpy(&finished_msg[4], finished_data, 12);

        uint8_t iv[12];
        memcpy(iv, ctx->server_iv, 12);
        iv[11] ^= (uint8_t)(ctx->write_seq & 0xFF);
        iv[10] ^= (uint8_t)((ctx->write_seq >> 8) & 0xFF);

        uint8_t encrypted[64];
        uint8_t tag[16];
        aes128_gcm_encrypt(ctx->server_key, iv, NULL, 0,
                           finished_msg, 16, encrypted, tag);

        static uint8_t enc_record[32];
        memcpy(enc_record, encrypted, 16);
        memcpy(enc_record + 16, tag, 16);
        if (!tls_server_send_record(ctx, TLS_RECORD_HANDSHAKE, enc_record, 32)) return 0;
        ctx->write_seq++;
        pr_info("tls_server: Finished sent (encrypted)\n");
    }

    ctx->handshake_done = 1;
    pr_info("tls_server: handshake complete!\n");
    return 1;
}

/* ===== Public API ===== */

void tls_server_init(void) {
    server_cert_generate();
}

int tls_server_accept(struct tcp_conn* tcp, struct tls_server_conn** out) {
    static struct tls_server_conn conns[4];
    static int inited = 0;
    if (!inited) {
        memset(conns, 0, sizeof(conns));
        inited = 1;
    }

    int slot = -1;
    for (int i = 0; i < 4; i++) {
        if (conns[i].tcp == NULL) { slot = i; break; }
    }
    if (slot < 0) {
        pr_warn("tls_server: no free TLS connection slots\n");
        return -1;
    }

    struct tls_server_conn* ctx = &conns[slot];
    memset(ctx, 0, sizeof(*ctx));
    ctx->tcp = tcp;
    sha256_init(&ctx->transcript);

    pr_info("tls_server: starting TLS handshake...\n");
    if (!tls_server_do_handshake(ctx)) {
        pr_warn("tls_server: handshake failed\n");
        ctx->tcp = NULL;
        return -1;
    }

    *out = ctx;
    return 0;
}

int tls_server_send(struct tls_server_conn* ctx, const void* data, uint16_t len) {
    if (!ctx || !ctx->handshake_done) return 0;

    uint8_t encrypted[8192];
    uint8_t tag[16];
    uint8_t iv[12];
    memcpy(iv, ctx->server_iv, 12);
    iv[11] ^= (uint8_t)(ctx->write_seq & 0xFF);
    iv[10] ^= (uint8_t)((ctx->write_seq >> 8) & 0xFF);

    aes128_gcm_encrypt(ctx->server_key, iv, NULL, 0,
                       data, len, encrypted, tag);

    static uint8_t record[8192];
    memcpy(record, encrypted, len);
    memcpy(record + len, tag, 16);

    ctx->write_seq++;
    return tls_server_send_record(ctx, TLS_RECORD_APPLICATION, record, len + 16);
}

int tls_server_recv(struct tls_server_conn* ctx, void* buf, uint16_t bufsz, uint32_t timeout_ms) {
    if (!ctx || !ctx->handshake_done) return 0;

    uint8_t rtype, rdata[8192];
    uint16_t rlen;
    if (!tls_server_recv_record(ctx, &rtype, rdata, &rlen, timeout_ms)) return 0;
    if (rtype == TLS_RECORD_ALERT) return -2; /* connection closed */
    if (rtype != TLS_RECORD_APPLICATION) return 0;
    if (rlen < 16) return 0;

    uint16_t ct_len = rlen - 16;
    uint8_t iv[12];
    memcpy(iv, ctx->client_iv, 12);
    iv[11] ^= (uint8_t)(ctx->read_seq & 0xFF);
    iv[10] ^= (uint8_t)((ctx->read_seq >> 8) & 0xFF);

    if (!aes128_gcm_decrypt(ctx->client_key, iv, NULL, 0,
                            rdata, ct_len, &rdata[ct_len], (uint8_t*)buf)) {
        pr_warn("tls_server: GCM tag verification failed\n");
        return 0;
    }

    ctx->read_seq++;
    return ct_len;
}

void tls_server_close(struct tls_server_conn* ctx) {
    if (!ctx) return;
    if (ctx->handshake_done) {
        tls_server_send_alert(ctx, TLS_ALERT_WARNING, TLS_ALERT_CLOSE_NOTIFY);
    }
    if (ctx->tcp) {
        tcp_close_conn(ctx->tcp);
        ctx->tcp = NULL;
    }
    ctx->handshake_done = 0;
}

int tls_server_cert_pem(struct tls_server_conn* ctx, char* buf, int bufsz) {
    server_cert_generate();
    if (!cert_initialized || server_cert.cert_len == 0) return 0;

    static const char* begin = "-----BEGIN CERTIFICATE-----\n";
    static const char* end = "-----END CERTIFICATE-----\n";
    int begin_len = (int)strlen(begin);
    int end_len = (int)strlen(end);

    /* Simple base64 encode */
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t* der = server_cert.cert_der;
    int der_len = server_cert.cert_len;

    int off = 0;
    if (off + begin_len < bufsz) memcpy(buf + off, begin, begin_len);
    off += begin_len;

    for (int i = 0; i < der_len; i += 3) {
        uint32_t val = (uint32_t)der[i] << 16;
        if (i + 1 < der_len) val |= (uint32_t)der[i+1] << 8;
        if (i + 2 < der_len) val |= (uint32_t)der[i+2];

        char triple[4];
        triple[0] = b64[(val >> 18) & 0x3F];
        triple[1] = b64[(val >> 12) & 0x3F];
        triple[2] = (i + 1 < der_len) ? b64[(val >> 6) & 0x3F] : '=';
        triple[3] = (i + 2 < der_len) ? b64[val & 0x3F] : '=';

        if (off + 4 < bufsz) memcpy(buf + off, triple, 4);
        off += 4;

        if (((i / 3) + 1) % 19 == 0 && i + 3 < der_len) {
            if (off + 1 < bufsz) buf[off] = '\n';
            off++;
        }
    }
    if (off + 1 < bufsz) buf[off] = '\n';
    off++;

    if (off + end_len < bufsz) memcpy(buf + off, end, end_len);
    off += end_len;

    if (off < bufsz) buf[off] = '\0';
    return off;
}

int tls_server_is_active(struct tls_server_conn* ctx) {
    return ctx && ctx->tcp && ctx->handshake_done;
}
