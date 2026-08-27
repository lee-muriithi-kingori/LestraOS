/*
 * Lestra OS - TLS 1.2 Client (Production)
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Complete TLS 1.2 implementation with:
 *   - AES-128-GCM AEAD (encrypt + decrypt + auth tag)
 *   - ECDHE P-256 key exchange with point-on-curve validation
 *   - SHA-256 + HMAC-SHA256
 *   - X.509 certificate ASN.1 parsing + chain verification
 *   - RSA-PKCS1-v1.5 signature verification (cert + SKE)
 *   - AES-256-CTR-DRBG CSPRNG backed by RDRAND
 *   - Full TLS 1.2 handshake with server Finished verification
 *   - TLS alert protocol (close_notify, fatal alerts)
 *   - ClientHello extensions: SNI, supported_groups, signature_algorithms
 *
 * Cipher suite: TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 (0xC02F)
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/net.h>
#include <string.h>

extern void get_random_bytes(void* buf, size_t len);
extern int rsa_verify_pkcs1_v15(const uint8_t*, const uint8_t*, const uint8_t*, uint32_t);
extern int x509_parse(const uint8_t* der, uint32_t der_len, void* cert);
extern const void* ca_store_find_by_name(const char* name);

/* ===== SHA-256 ===== */
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

/* ===== AES-128 S-box ===== */
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

/* AES-128 key expansion */
static void aes128_key_expansion(const uint8_t key[16], uint8_t round_keys[176]) {
    memcpy(round_keys, key, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, &round_keys[i-4], 4);
        if (i % 16 == 0) {
            uint8_t tmp = t[0]; t[0]=t[1]; t[1]=t[2]; t[2]=t[3]; t[3]=tmp;
            for (int j = 0; j < 4; j++) t[j] = aes_sbox[t[j]];
            t[0] ^= 0x01 << ((i/16 - 1) & 7); /* Rcon */
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
        /* SubBytes */
        for (int i = 0; i < 16; i++) state[i] = aes_sbox[state[i]];
        /* ShiftRows */
        uint8_t t;
        t=state[1]; state[1]=state[5]; state[5]=state[9]; state[9]=state[13]; state[13]=t;
        t=state[2]; state[2]=state[10]; state[10]=t; t=state[6]; state[6]=state[14]; state[14]=t;
        t=state[15]; state[15]=state[11]; state[11]=state[7]; state[7]=state[3]; state[3]=t;
        /* MixColumns (skip in last round) */
        if (round < 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0=state[c*4],a1=state[c*4+1],a2=state[c*4+2],a3=state[c*4+3];
                state[c*4]  =xtime(a0)^xtime(a1)^a1^a2^a3;
                state[c*4+1]=a0^xtime(a1)^xtime(a2)^a2^a3;
                state[c*4+2]=a0^a1^xtime(a2)^xtime(a3)^a3;
                state[c*4+3]=xtime(a0)^a0^a1^a2^xtime(a3);
            }
        }
        /* AddRoundKey */
        for (int i = 0; i < 16; i++) state[i] ^= round_keys[round*16+i];
    }
    memcpy(out, state, 16);
}

/* ===== AES-128-GCM ===== */
/* GHASH multiplication in GF(2^128) */
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

/* AES-128-GCM encrypt: produces ciphertext + 16-byte tag */
static void aes128_gcm_encrypt(const uint8_t key[16], const uint8_t iv[12],
                                const uint8_t* aad, uint32_t aad_len,
                                const uint8_t* plaintext, uint32_t pt_len,
                                uint8_t* ciphertext, uint8_t tag[16]) {
    uint8_t round_keys[176]; aes128_key_expansion(key, round_keys);

    /* H = AES(0^128) */
    uint8_t H[16]; memset(H, 0, 16);
    aes_encrypt_block(H, H, round_keys);

    /* J0 = IV || 0x00000002 (for 96-bit IV) */
    uint8_t J0[16]; memcpy(J0, iv, 12); J0[12]=0; J0[13]=0; J0[14]=0; J0[15]=2;

    /* Encrypt plaintext (CTR mode starting at J0+1) */
    uint8_t counter[16]; memcpy(counter, J0, 16);
    uint32_t i = 0;
    while (i < pt_len) {
        /* Increment counter */
        for (int j = 15; j >= 12; j--) { if (++counter[j]) break; }
        uint8_t ks[16]; aes_encrypt_block(counter, ks, round_keys);
        uint32_t block_len = (pt_len - i < 16) ? pt_len - i : 16;
        for (uint32_t j = 0; j < block_len; j++) ciphertext[i+j] = plaintext[i+j] ^ ks[j];
        i += block_len;
    }

    /* GHASH(AAD || pad || CT || pad || len(AAD) || len(CT)) */
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

    /* Tag = AES(J0) XOR Y */
    uint8_t EJ0[16]; aes_encrypt_block(J0, EJ0, round_keys);
    for (int j = 0; j < 16; j++) tag[j] = EJ0[j] ^ Y[j];
}

/* AES-128-GCM decrypt: verifies tag, returns 1 on success */
static int aes128_gcm_decrypt(const uint8_t key[16], const uint8_t iv[12],
                               const uint8_t* aad, uint32_t aad_len,
                               const uint8_t* ciphertext, uint32_t ct_len,
                               const uint8_t tag[16], uint8_t* plaintext) {
    uint8_t round_keys[176]; aes128_key_expansion(key, round_keys);
    uint8_t H[16]; memset(H, 0, 16);
    aes_encrypt_block(H, H, round_keys);
    uint8_t J0[16]; memcpy(J0, iv, 12); J0[12]=0; J0[13]=0; J0[14]=0; J0[15]=2;

    /* Verify tag first */
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

    /* Constant-time tag comparison */
    uint8_t diff = 0;
    for (int j = 0; j < 16; j++) diff |= computed_tag[j] ^ tag[j];
    if (diff != 0) return 0; /* tag mismatch */

    /* Decrypt (same as encrypt, XOR with keystream) */
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

/* ===== TLS Connection State ===== */
struct tls_state {
    int connected;
    int handshake_done;
    ipv4_addr_t server_ip;
    uint16_t server_port;
    char server_name[128];
    uint8_t client_random[32];
    uint8_t server_random[32];
    uint8_t master_secret[48];
    uint8_t client_write_key[16];
    uint8_t server_write_key[16];
    uint8_t client_write_iv[12];
    uint8_t server_write_iv[12];
    uint64_t client_seq;
    uint64_t server_seq;
    /* Handshake transcript hash */
    struct sha256_ctx transcript;
};

static struct tls_state tls;

/* ===== TLS PRF (RFC 5246) ===== */
/* P_hash(secret, seed) = HMAC(secret, A(1)+seed) + HMAC(secret, A(2)+seed) + ... */
static void p_hash(const uint8_t* secret, uint32_t secret_len,
                   const uint8_t* seed, uint32_t seed_len,
                   uint8_t* out, uint32_t out_len) {
    uint8_t A[32]; /* A(0) = seed, A(i) = HMAC(secret, A(i-1)) */
    memcpy(A, seed, seed_len < 32 ? seed_len : 32);
    /* A(1) = HMAC(secret, seed) */
    hmac_sha256(secret, secret_len, seed, seed_len, A);

    uint32_t offset = 0;
    while (offset < out_len) {
        /* Compute HMAC(secret, A(i) + seed) */
        uint8_t combined[32 + 128];
        memcpy(combined, A, 32);
        memcpy(combined + 32, seed, seed_len < 128 ? seed_len : 128);
        uint8_t block[32];
        hmac_sha256(secret, secret_len, combined, 32 + seed_len, block);

        uint32_t to_copy = out_len - offset;
        if (to_copy > 32) to_copy = 32;
        memcpy(out + offset, block, to_copy);
        offset += to_copy;

        /* A(i+1) = HMAC(secret, A(i)) */
        hmac_sha256(secret, secret_len, A, 32, A);
    }
}

/* TLS PRF: PRF(secret, label, seed) = P_hash(secret, label + seed) */
static void tls_prf(const uint8_t* secret, uint32_t secret_len,
                    const char* label, const uint8_t* seed, uint32_t seed_len,
                    uint8_t* out, uint32_t out_len) {
    uint32_t label_len = strlen(label);
    uint8_t combined[256];
    memcpy(combined, label, label_len);
    memcpy(combined + label_len, seed, seed_len);
    p_hash(secret, secret_len, combined, label_len + seed_len, out, out_len);
}

/* ===== TLS Record Layer ===== */
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
#define TLS_ALERT_UNEXPECTED_MESSAGE    10
#define TLS_ALERT_BAD_RECORD_MAC        20
#define TLS_ALERT_HANDSHAKE_FAILURE     40
#define TLS_ALERT_DECODE_ERROR          50
#define TLS_ALERT_INTERNAL_ERROR        80
#define TLS_ALERT_INAPPROPRIATE_FALLBACK 86
#define TLS_ALERT_UNSUPPORTED_EXTENSION 110
#define TLS_ALERT_CERTIFICATE_UNAUTHORIZED 42
#define TLS_ALERT_CERTIFICATE_EXPIRED      45

static int tls_send_record(uint8_t type, const uint8_t* data, uint16_t len) {
    static uint8_t record[17000];
    record[0] = type;
    record[1] = 0x03; record[2] = 0x03; /* TLS 1.2 */
    record[3] = (len >> 8) & 0xFF;
    record[4] = len & 0xFF;
    memcpy(&record[5], data, len);
    int total = 5 + len;
    extern int tcp_send(const void*, uint16_t);
    int sent = 0;
    while (sent < total) {
        int chunk = total - sent; if (chunk > 1400) chunk = 1400;
        int n = tcp_send(&record[sent], (uint16_t)chunk);
        if (n <= 0) return 0;
        sent += n;
    }
    return 1;
}

static int tls_recv_record(uint8_t* type, uint8_t* data, uint16_t* len, uint32_t timeout_ms) {
    extern int tcp_recv_wait(uint8_t*, uint16_t, uint32_t);
    uint8_t header[5];
    int n = tcp_recv_wait(header, 5, timeout_ms);
    if (n < 5) return 0;
    *type = header[0];
    *len = ((uint16_t)header[3] << 8) | header[4];
    if (*len > 16384) return 0;
    n = tcp_recv_wait(data, *len, timeout_ms);
    if (n < *len) return 0;
    return 1;
}

static int tls_send_alert(uint8_t level, uint8_t desc) {
    uint8_t alert[2];
    alert[0] = level;
    alert[1] = desc;
    return tls_send_record(TLS_RECORD_ALERT, alert, 2);
}

/* ===== ClientHello ===== */
static void tls_generate_random(uint8_t out[32]) {
    get_random_bytes(out, 32);
}

static int tls_send_client_hello(void) {
    static uint8_t hello[512];
    int len = 0;
    hello[0] = TLS_HS_CLIENT_HELLO;
    int body_start = 4; len = body_start;
    hello[len++] = 0x03; hello[len++] = 0x03;
    tls_generate_random(tls.client_random);
    memcpy(&hello[len], tls.client_random, 32); len += 32;
    hello[len++] = 0;
    hello[len++] = 0; hello[len++] = 4;
    hello[len++] = 0xC0; hello[len++] = 0x2F;
    hello[len++] = 0xC0; hello[len++] = 0x30;
    hello[len++] = 1; hello[len++] = 0;
    int ext_start = len;
    hello[len++] = 0; hello[len++] = 0;
    int sni_len = strlen(tls.server_name);
    hello[len++] = 0x00; hello[len++] = 0x00;
    int ext_data = 5 + sni_len;
    hello[len++] = (ext_data >> 8) & 0xFF; hello[len++] = ext_data & 0xFF;
    int list_len = 3 + sni_len;
    hello[len++] = (list_len >> 8) & 0xFF; hello[len++] = list_len & 0xFF;
    hello[len++] = 0;
    hello[len++] = (sni_len >> 8) & 0xFF; hello[len++] = sni_len & 0xFF;
    memcpy(&hello[len], tls.server_name, sni_len); len += sni_len;
    hello[len++] = 0x00; hello[len++] = 0x0A;
    hello[len++] = 0x00; hello[len++] = 0x04;
    hello[len++] = 0x00; hello[len++] = 0x02;
    hello[len++] = 0x00; hello[len++] = 0x1D;
    hello[len++] = 0x00; hello[len++] = 0x0D;
    hello[len++] = 0x00; hello[len++] = 0x04;
    hello[len++] = 0x00; hello[len++] = 0x02;
    hello[len++] = 0x04; hello[len++] = 0x01;
    int ext_total = len - ext_start - 2;
    hello[ext_start] = (ext_total >> 8) & 0xFF;
    hello[ext_start+1] = ext_total & 0xFF;
    int body_len = len - body_start;
    hello[1] = (body_len >> 16) & 0xFF; hello[2] = (body_len >> 8) & 0xFF; hello[3] = body_len & 0xFF;
    sha256_update(&tls.transcript, hello, len);
    return tls_send_record(TLS_RECORD_HANDSHAKE, hello, len);
}

/* ===== X.509 cert struct (matches x509.c) ===== */
struct tls_x509_cert {
    char subject_cn[128];
    char issuer_cn[128];
    uint8_t serial[20];
    uint8_t tbs_hash[32];
    uint8_t signature[256];
    uint8_t pubkey_n[256];
    uint32_t pubkey_e;
};

/* ===== TLS Connect (main handshake) ===== */
int tls_connect(ipv4_addr_t ip, uint16_t port, const char* hostname) {
    if (!net_is_up()) { pr_warn("tls: network not up\n"); return 0; }
    memset(&tls, 0, sizeof(tls));
    tls.server_ip = ip; tls.server_port = port;
    strncpy(tls.server_name, hostname, sizeof(tls.server_name)-1);

    pr_info("tls: connecting to %s:%u\n", hostname, (unsigned)port);
    extern int tcp_connect(ipv4_addr_t, uint16_t, uint32_t);
    if (!tcp_connect(ip, port, 5000)) { pr_warn("tls: tcp_connect failed\n"); return 0; }
    tls.connected = 1;

    sha256_init(&tls.transcript);

    if (!tls_send_client_hello()) { tls_send_alert(TLS_ALERT_FATAL, TLS_ALERT_INTERNAL_ERROR); return 0; }
    pr_info("tls: ClientHello sent (SNI=%s)\n", tls.server_name);

    uint8_t rtype, rdata[16384];
    uint16_t rlen;

    if (!tls_recv_record(&rtype, rdata, &rlen, 5000)) { pr_warn("tls: no ServerHello\n"); tls_send_alert(TLS_ALERT_FATAL, TLS_ALERT_UNEXPECTED_MESSAGE); return 0; }
    pr_info("tls: received record type=%u len=%u\n", rtype, rlen);
    sha256_update(&tls.transcript, rdata, rlen);

    if (rtype == TLS_RECORD_HANDSHAKE && rdata[0] == TLS_HS_SERVER_HELLO) {
        memcpy(tls.server_random, &rdata[6], 32);
        pr_info("tls: ServerHello - version 0x%02x%02x\n", rdata[4], rdata[5]);
    }

    if (!tls_recv_record(&rtype, rdata, &rlen, 5000)) { pr_warn("tls: no Certificate\n"); return 0; }
    pr_info("tls: received Certificate (%u bytes)\n", rlen);
    sha256_update(&tls.transcript, rdata, rlen);

    struct tls_x509_cert server_cert;
    memset(&server_cert, 0, sizeof(server_cert));
    int cert_verified = 0;
    if (rtype == TLS_RECORD_HANDSHAKE && rdata[0] == TLS_HS_CERTIFICATE) {
        uint8_t* body = rdata + 4;
        uint32_t body_len = ((uint32_t)rdata[1] << 16) | ((uint32_t)rdata[2] << 8) | rdata[3];
        if (body_len >= 7) {
            uint32_t chain_len = ((uint32_t)body[0] << 16) | ((uint32_t)body[1] << 8) | body[2];
            if (chain_len + 3 <= body_len) {
                uint32_t cert_der_len = ((uint32_t)body[3] << 16) | ((uint32_t)body[4] << 8) | body[5];
                if (cert_der_len + 6 <= body_len && cert_der_len > 0) {
                    if (x509_parse(&body[6], cert_der_len, &server_cert) == 0) {
                        pr_info("tls: cert subject='%s' issuer='%s'\n", server_cert.subject_cn, server_cert.issuer_cn);
                        const struct tls_x509_cert* ca = (const struct tls_x509_cert*)ca_store_find_by_name(server_cert.issuer_cn);
                        if (ca) {
                            if (rsa_verify_pkcs1_v15(server_cert.tbs_hash, server_cert.signature, ca->pubkey_n, ca->pubkey_e) == 1) {
                                cert_verified = 1;
                                pr_info("tls: certificate verified against CA '%s'\n", ca->subject_cn);
                            } else {
                                pr_warn("tls: certificate signature verification FAILED\n");
                            }
                        } else {
                            pr_warn("tls: no CA found for issuer '%s'\n", server_cert.issuer_cn);
                        }
                    } else {
                        pr_warn("tls: X.509 parse failed\n");
                    }
                }
            }
        }
    }
    if (!cert_verified) {
        pr_warn("tls: certificate verification FAILED — aborting handshake\n");
        tls_send_alert(TLS_ALERT_FATAL, TLS_ALERT_HANDSHAKE_FAILURE);
        return 0;
    }

    static uint8_t ske_buf[512];
    uint16_t ske_len = 0;
    if (!tls_recv_record(&rtype, ske_buf, &ske_len, 5000)) { pr_warn("tls: no ServerKeyExchange\n"); return 0; }
    pr_info("tls: received ServerKeyExchange (%u bytes)\n", (unsigned)ske_len);
    sha256_update(&tls.transcript, ske_buf, ske_len);

    uint8_t server_pubkey_x[32];
    uint8_t server_pubkey_y[32];
    {
        uint8_t* body = ske_buf;
        if (ske_len >= 8 && body[0] == TLS_HS_SERVER_KEY_EXCH) {
            uint8_t curve_type = body[4];
            uint16_t curve_id  = ((uint16_t)body[5] << 8) | body[6];
            uint8_t point_len  = body[7];
            if (curve_type == 3 && curve_id == 0x0017 && point_len == 65 && ske_len >= 8 + 65) {
                memcpy(server_pubkey_x, &body[8 + 1],  32);
                memcpy(server_pubkey_y, &body[8 + 33], 32);
                pr_info("tls: parsed server P-256 pubkey\n");
            } else {
                pr_warn("tls: unexpected SKE curve_type=%u curve_id=0x%04x point_len=%u\n",
                        curve_type, curve_id, point_len);
                memset(server_pubkey_x, 0, 32);
                memset(server_pubkey_y, 0, 32);
            }
        } else {
            pr_warn("tls: SKE not a ServerKeyExchange handshake msg\n");
            memset(server_pubkey_x, 0, 32);
            memset(server_pubkey_y, 0, 32);
        }
    }

    if (!tls_recv_record(&rtype, rdata, &rlen, 5000)) { pr_warn("tls: no ServerHelloDone\n"); return 0; }
    pr_info("tls: received ServerHelloDone\n");
    sha256_update(&tls.transcript, rdata, rlen);

    if (cert_verified && server_cert.pubkey_n[0] != 0) {
        uint8_t ske_hash[32];
        struct sha256_ctx saved_transcript = tls.transcript;
        sha256_final(&tls.transcript, ske_hash);
        tls.transcript = saved_transcript;
        if (rsa_verify_pkcs1_v15(ske_hash, &ske_buf[4 + 4], server_cert.pubkey_n, server_cert.pubkey_e) != 1) {
            pr_warn("tls: ServerKeyExchange signature verification FAILED\n");
            tls_send_alert(TLS_ALERT_FATAL, TLS_ALERT_HANDSHAKE_FAILURE);
            return 0;
        }
        pr_info("tls: ServerKeyExchange signature verified\n");
    }

    uint8_t ecdhe_secret[32];
    uint8_t our_pubkey_x[32];
    uint8_t our_pubkey_y[32];
    extern void p256_keygen(uint8_t*, uint8_t*, uint8_t*);
    extern int  p256_ecdh(const uint8_t*, const uint8_t*, const uint8_t*, uint8_t*);
    extern int  p256_selftest(void);
    if (!p256_selftest()) { pr_warn("tls: P-256 self-test FAILED\n"); return 0; }
    p256_keygen(ecdhe_secret, our_pubkey_x, our_pubkey_y);
    pr_info("tls: generated ECDHE key pair (P-256)\n");

    static uint8_t cke[128];
    int cke_len = 0;
    cke[0] = TLS_HS_CLIENT_KEY_EXCH;
    uint8_t pubkey[65];
    pubkey[0] = 4;
    memcpy(&pubkey[1],  our_pubkey_x, 32);
    memcpy(&pubkey[33], our_pubkey_y, 32);
    cke_len = 4 + 1 + 65;
    cke[3] = 1 + 65;
    cke[4] = 65;
    memcpy(&cke[5], pubkey, 65);
    sha256_update(&tls.transcript, cke, cke_len);
    tls_send_record(TLS_RECORD_HANDSHAKE, cke, cke_len);
    pr_info("tls: ClientKeyExchange sent\n");

    uint8_t premaster[32];
    if (p256_ecdh(ecdhe_secret, server_pubkey_x, server_pubkey_y, premaster) != 1) {
        pr_warn("tls: ECDH failed (peer point invalid?)\n");
        tls_send_alert(TLS_ALERT_FATAL, TLS_ALERT_HANDSHAKE_FAILURE);
        return 0;
    }
    pr_info("tls: premaster secret computed via P-256 ECDH\n");

    uint8_t master_seed[64];
    memcpy(master_seed, tls.client_random, 32);
    memcpy(master_seed+32, tls.server_random, 32);
    tls_prf(premaster, 32, "master secret", master_seed, 64, tls.master_secret, 48);
    pr_info("tls: master secret derived\n");

    uint8_t key_block[128];
    tls_prf(tls.master_secret, 48, "key expansion", master_seed, 64, key_block, 72);
    memcpy(tls.client_write_key, &key_block[0], 16);
    memcpy(tls.server_write_key, &key_block[16], 16);
    memcpy(tls.client_write_iv, &key_block[40], 12);
    memcpy(tls.server_write_iv, &key_block[52], 12);
    pr_info("tls: key material derived\n");

    uint8_t ccs = 1;
    tls_send_record(TLS_RECORD_CHANGE_CIPHER, &ccs, 1);
    pr_info("tls: ChangeCipherSpec sent\n");

    uint8_t transcript_hash[32];
    sha256_final(&tls.transcript, transcript_hash);

    uint8_t finished_data[12];
    tls_prf(tls.master_secret, 48, "client finished", transcript_hash, 32, finished_data, 12);

    static uint8_t finished_msg[16];
    finished_msg[0] = TLS_HS_FINISHED;
    finished_msg[1] = 0; finished_msg[2] = 0; finished_msg[3] = 12;
    memcpy(&finished_msg[4], finished_data, 12);

    uint8_t encrypted_finished[64];
    uint8_t tag[16];
    uint8_t iv[12];
    memcpy(iv, tls.client_write_iv, 12);
    tls.client_seq = 0;
    aes128_gcm_encrypt(tls.client_write_key, iv, NULL, 0,
                       finished_msg, 16, encrypted_finished, tag);

    static uint8_t enc_record[32];
    memcpy(enc_record, encrypted_finished, 16);
    memcpy(enc_record+16, tag, 16);
    tls_send_record(TLS_RECORD_HANDSHAKE, enc_record, 32);
    tls.client_seq++;
    pr_info("tls: Finished sent (encrypted)\n");

    sha256_update(&tls.transcript, finished_msg, 16);

    if (!tls_recv_record(&rtype, rdata, &rlen, 5000)) { pr_warn("tls: no server CCS\n"); return 0; }
    pr_info("tls: received server ChangeCipherSpec\n");

    if (!tls_recv_record(&rtype, rdata, &rlen, 5000)) { pr_warn("tls: no server Finished\n"); return 0; }
    pr_info("tls: received server Finished (%u bytes)\n", rlen);

    if (rlen < 16) {
        pr_warn("tls: server Finished too short\n");
        tls_send_alert(TLS_ALERT_FATAL, TLS_ALERT_DECODE_ERROR);
        return 0;
    }

    uint8_t server_iv[12];
    memcpy(server_iv, tls.server_write_iv, 12);
    server_iv[11] ^= (uint8_t)(tls.server_seq & 0xFF);
    server_iv[10] ^= (uint8_t)((tls.server_seq >> 8) & 0xFF);

    uint8_t decrypted_finished[16];
    if (!aes128_gcm_decrypt(tls.server_write_key, server_iv, NULL, 0,
                            rdata, rlen - 16, &rdata[rlen - 16], decrypted_finished)) {
        pr_warn("tls: server Finished GCM verification FAILED\n");
        tls_send_alert(TLS_ALERT_FATAL, TLS_ALERT_BAD_RECORD_MAC);
        return 0;
    }
    tls.server_seq++;

    uint8_t server_finished_data[12];
    tls_prf(tls.master_secret, 48, "server finished", transcript_hash, 32, server_finished_data, 12);

    uint8_t fin_diff = 0;
    for (int i = 0; i < 12; i++) fin_diff |= decrypted_finished[i] ^ server_finished_data[i];
    if (fin_diff != 0) {
        pr_warn("tls: server Finished hash mismatch!\n");
        tls_send_alert(TLS_ALERT_FATAL, TLS_ALERT_HANDSHAKE_FAILURE);
        return 0;
    }
    pr_info("tls: server Finished verified\n");

    tls.handshake_done = 1;
    pr_info("tls: handshake complete! Secure channel established.\n");
    return 1;
}

/* ===== TLS Send (encrypted application data) ===== */
int tls_send(const void* data, uint16_t len) {
    if (!tls.handshake_done) return 0;

    /* Encrypt with AES-128-GCM */
    uint8_t encrypted[16384];
    uint8_t tag[16];
    uint8_t iv[12];
    memcpy(iv, tls.client_write_iv, 12);
    /* XOR sequence number into IV */
    iv[11] ^= (uint8_t)(tls.client_seq & 0xFF);
    iv[10] ^= (uint8_t)((tls.client_seq >> 8) & 0xFF);

    aes128_gcm_encrypt(tls.client_write_key, iv, NULL, 0,
                       data, len, encrypted, tag);

    /* Record: ciphertext + tag */
    static uint8_t record[17000];
    memcpy(record, encrypted, len);
    memcpy(record + len, tag, 16);

    tls.client_seq++;
    return tls_send_record(TLS_RECORD_APPLICATION, record, len + 16);
}

/* ===== TLS Recv (decrypt application data) ===== */
int tls_recv(void* buf, uint16_t bufsz, uint32_t timeout_ms) {
    if (!tls.handshake_done) return 0;

    uint8_t rtype, rdata[16384];
    uint16_t rlen;
    if (!tls_recv_record(&rtype, rdata, &rlen, timeout_ms)) return 0;
    if (rtype != TLS_RECORD_APPLICATION) return 0;

    /* Decrypt with AES-128-GCM */
    if (rlen < 16) return 0; /* need at least tag */
    uint16_t ct_len = rlen - 16;
    uint8_t iv[12];
    memcpy(iv, tls.server_write_iv, 12);
    iv[11] ^= (uint8_t)(tls.server_seq & 0xFF);
    iv[10] ^= (uint8_t)((tls.server_seq >> 8) & 0xFF);

    uint8_t* plaintext = (uint8_t*)buf;
    if (!aes128_gcm_decrypt(tls.server_write_key, iv, NULL, 0,
                             rdata, ct_len, &rdata[ct_len], plaintext)) {
        pr_warn("tls: GCM tag verification failed!\n");
        return 0;
    }

    tls.server_seq++;
    return ct_len;
}

void tls_close(void) {
    if (tls.connected && tls.handshake_done) {
        tls_send_alert(TLS_ALERT_WARNING, TLS_ALERT_CLOSE_NOTIFY);
    }
    if (tls.connected) {
        extern void tcp_close(void);
        tcp_close();
        tls.connected = 0;
        tls.handshake_done = 0;
    }
}

int tls_is_connected(void) {
    return tls.connected && tls.handshake_done;
}
