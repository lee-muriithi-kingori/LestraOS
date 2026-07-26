/*
 * Lestra OS - SSH-2.0 Remote Shell Server
 * Copyright (c) 2026 lestramk.org
 *
 * Complete SSH-2.0 protocol server compatible with OpenSSH clients
 * (ssh root@host -p 2222). Replaces the old LESTRA_SSH/1.0 protocol.
 *
 * Supported algorithms:
 *   Key exchange:    ecdh-sha2-nistp256
 *   Host key:        ecdsa-sha2-nistp256
 *   Encryption:      aes128-gcm@openssh.com
 *   MAC:             implicit (GCM auth tag)
 *   Compression:     none
 *   Authentication:  password (ssh-userauth)
 *   Channels:        session (interactive shell + pty-req)
 *
 * Crypto primitives leverage existing LestraOS kernel code:
 *   - P-256 ECDH via p256_ecdh() extern
 *   - P-256 keygen via p256_keygen() extern
 *   - AES-128-GCM, SHA-256, HMAC-SHA256 duplicated from tls_server.c
 *   - ECDSA signing implemented here using P-256 arithmetic
 *   - CSPRNG via get_random_bytes() extern (RDRAND-backed)
 *
 * Packet protocol follows RFC 4253 binary format. After NEWKEYS,
 * packets are encrypted with aes128-gcm@openssh.com which uses
 * AES-GCM with sequence-number-derived 12-byte IVs and the
 * encrypted-length-as-AAD framing per OpenSSH convention.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/ssh_server.h>
#include <lestra/net.h>
#include <lestra/timer.h>
#include <string.h>

/* ===== Extern declarations for existing kernel crypto ===== */
extern void shell_execute_line(const char* line, void (*out_cb)(char));
extern void p256_keygen(uint8_t priv[32], uint8_t pub_x[32], uint8_t pub_y[32]);
extern int  p256_ecdh(const uint8_t priv[32], const uint8_t peer_x[32],
                      const uint8_t peer_y[32], uint8_t shared[32]);
extern void get_random_bytes(void* buf, size_t len);

/* ===== SSH-2.0 Protocol Constants ===== */
#define SSH_VERSION_STRING  "SSH-2.0-LestraOS_1.0"

/* Algorithm name-lists for KEXINIT */
#define KEX_ALGORITHMS      "ecdh-sha2-nistp256"
#define HOSTKEY_ALGORITHMS  "ecdsa-sha2-nistp256"
#define ENC_ALGORITHMS      "aes128-gcm@openssh.com"
#define MAC_ALGORITHMS      "hmac-sha2-256"
#define COMP_ALGORITHMS     "none"

/* AES-128-GCM parameters for SSH */
#define AES_GCM_KEY_LEN    16
#define AES_GCM_IV_LEN     12
#define AES_GCM_TAG_LEN    16

/* Channel defaults */
#define SSH_WINDOW_SIZE    1048576    /* 1 MB */
#define SSH_MAX_PKT_SIZE   32768

/* ===== P-256 Curve Constants (for ECDSA signing) ===== */
static const uint8_t p256_n[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xBC,0xE6,0xFA,0xAD,0xA7,0x17,0x9E,0x84,
    0xF3,0xB9,0xCA,0xC2,0xFC,0x63,0x25,0x51
};

static const uint8_t p256_Gx[32] = {
    0x6B,0x17,0xD1,0xF2,0xE1,0x2C,0x42,0x47,
    0xF8,0xBC,0xE6,0xE5,0x63,0xA4,0x40,0xF2,
    0x77,0x03,0x7D,0x81,0x2D,0xEB,0x33,0xA0,
    0xF4,0xA1,0x39,0x45,0xD8,0x98,0xC2,0x96
};

static const uint8_t p256_Gy[32] = {
    0x4F,0xE3,0x42,0xE2,0xFE,0x1A,0x7F,0x9B,
    0x8E,0xE7,0xEB,0x4A,0x7C,0x0F,0x9E,0x16,
    0x2B,0xCE,0x33,0x57,0x6B,0x31,0x5E,0xCE,
    0xCB,0xB6,0x40,0x68,0x37,0xBF,0x51,0xF5
};

/* n-2 for Fermat's little theorem modular inverse */
static const uint8_t p256_n_minus_2[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xBC,0xE6,0xFA,0xAD,0xA7,0x17,0x9E,0x84,
    0xF3,0xB9,0xCA,0xC2,0xFC,0x63,0x25,0x4F
};

/* delta_n = 2^256 - n (for binary long division modular reduction) */
static const uint8_t p256_delta_n[32] = {
    0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x43,0x19,0x05,0x52,0x58,0xE8,0x61,0x7B,
    0x0C,0x46,0x35,0x3D,0x03,0x9C,0xDA,0xAF
};

/* ===== SHA-256 Implementation ===== */
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

static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

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
        w[i] = ((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)|
               ((uint32_t)block[i*4+2]<<8)|block[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i-15],7)^rotr32(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1 = rotr32(w[i-2],17)^rotr32(w[i-2],19)^(w[i-2]>>10);
        w[i] = w[i-16]+s0+w[i-7]+s1;
    }
    uint32_t a=ctx->state[0],b=ctx->state[1],c=ctx->state[2],d=ctx->state[3];
    uint32_t e=ctx->state[4],f=ctx->state[5],g=ctx->state[6],h=ctx->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1=rotr32(e,6)^rotr32(e,11)^rotr32(e,25);
        uint32_t ch=(e&f)^(~e&g);
        uint32_t t1=h+S1+ch+sha256_k[i]+w[i];
        uint32_t S0=rotr32(a,2)^rotr32(a,13)^rotr32(a,22);
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
    if (buf_pos > 56) {
        while (buf_pos < 64) ctx->buffer[buf_pos++] = 0;
        sha256_block(ctx, ctx->buffer); buf_pos = 0;
    }
    while (buf_pos < 56) ctx->buffer[buf_pos++] = 0;
    uint64_t bc = ctx->bitcount;
    for (int i = 7; i >= 0; i--) { ctx->buffer[56+i] = bc & 0xFF; bc >>= 8; }
    sha256_block(ctx, ctx->buffer);
    for (int i = 0; i < 8; i++) {
        out[i*4]=(ctx->state[i]>>24)&0xFF; out[i*4+1]=(ctx->state[i]>>16)&0xFF;
        out[i*4+2]=(ctx->state[i]>>8)&0xFF; out[i*4+3]=ctx->state[i]&0xFF;
    }
}

static void sha256_hash(const uint8_t* data, uint32_t len, uint8_t out[32]) {
    struct sha256_ctx ctx; sha256_init(&ctx);
    sha256_update(&ctx, data, len); sha256_final(&ctx, out);
}

/* Multi-part hash: feed several buffers into one hash */
static void sha256_multi(const uint8_t* buffers[], const uint32_t lengths[],
                          int count, uint8_t out[32]) {
    struct sha256_ctx ctx; sha256_init(&ctx);
    for (int i = 0; i < count; i++) sha256_update(&ctx, buffers[i], lengths[i]);
    sha256_final(&ctx, out);
}

/* ===== HMAC-SHA256 ===== */
static void hmac_sha256(const uint8_t* key, uint32_t key_len,
                        const uint8_t* data, uint32_t data_len, uint8_t out[32]) {
    uint8_t k[64]; memset(k, 0, 64);
    if (key_len > 64) sha256_hash(key, key_len, k);
    else memcpy(k, key, key_len);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }
    struct sha256_ctx ctx; sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64); sha256_update(&ctx, data, data_len);
    uint8_t inner[32]; sha256_final(&ctx, inner);
    sha256_init(&ctx); sha256_update(&ctx, opad, 64); sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
}

/* ===== AES-128 Implementation ===== */
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

static uint8_t xt(uint8_t x) { return (x << 1) ^ ((x >> 7) * 0x1b); }

static void aes128_key_expansion(const uint8_t key[16], uint8_t round_keys[176]) {
    memcpy(round_keys, key, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4]; memcpy(t, &round_keys[i-4], 4);
        if (i % 16 == 0) {
            uint8_t tmp = t[0]; t[0]=t[1]; t[1]=t[2]; t[2]=t[3]; t[3]=tmp;
            for (int j = 0; j < 4; j++) t[j] = aes_sbox[t[j]];
            t[0] ^= (uint8_t)(0x01 << ((i/16 - 1) & 7));
        }
        for (int j = 0; j < 4; j++)
            round_keys[i+j] = round_keys[i-16+j] ^ t[j];
    }
}

static void aes_encrypt_block(const uint8_t in[16], uint8_t out[16],
                              const uint8_t rk[176]) {
    uint8_t s[16]; memcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = aes_sbox[s[i]];
        uint8_t t;
        t=s[1];s[1]=s[5];s[5]=s[9];s[9]=s[13];s[13]=t;
        t=s[2];s[2]=s[10];s[10]=t;t=s[6];s[6]=s[14];s[14]=t;
        t=s[15];s[15]=s[11];s[11]=s[7];s[7]=s[3];s[3]=t;
        if (round < 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0=s[c*4],a1=s[c*4+1],a2=s[c*4+2],a3=s[c*4+3];
                s[c*4]=xt(a0)^xt(a1)^a1^a2^a3; s[c*4+1]=a0^xt(a1)^xt(a2)^a2^a3;
                s[c*4+2]=a0^a1^xt(a2)^xt(a3)^a3; s[c*4+3]=xt(a0)^a0^a1^a2^xt(a3);
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= rk[round*16+i];
    }
    memcpy(out, s, 16);
}

/* ===== GHASH (for AES-GCM) ===== */
static void ghash_mult(const uint8_t X[16], const uint8_t H[16], uint8_t out[16]) {
    uint8_t Z[16]; memset(Z, 0, 16);
    for (int i = 0; i < 128; i++) {
        if (X[i/8] & (0x80 >> (i%8)))
            for (int j = 0; j < 16; j++) Z[j] ^= H[j];
        uint8_t lsb = Z[15] & 1;
        for (int j = 15; j > 0; j--) Z[j] = (Z[j] >> 1) | ((Z[j-1] & 1) << 7);
        Z[0] >>= 1;
        if (lsb) Z[0] ^= 0xe1;
    }
    memcpy(out, Z, 16);
}

static void ghash_update(uint8_t Y[16], const uint8_t* data, uint32_t len,
                          const uint8_t H[16]) {
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

/* ===== AES-128-GCM for SSH (NIST SP 800-38D standard) =====
 * This implementation uses the correct GCM counter construction:
 * J0 = IV || 0x00000001 (standard, per NIST SP 800-38D)
 * Counter for keystream starts at inc32(J0) = IV || 0x00000002
 * Tag = GHASH(AAD, CT) XOR E(J0)
 *
 * For SSH's aes128-gcm@openssh.com:
 * - The 4-byte packet_length is encrypted using E(J0) as CTR keystream
 *   (XOR first 4 bytes of E(J0) with the length bytes)
 * - The payload+padding is encrypted with standard GCM
 * - The encrypted_length (4 bytes) is used as AAD for the GCM operation
 * - IV is derived from the packet sequence number:
 *   iv = {0,0,0,0,0,0,0,0} || seq(4 bytes big-endian) */

/* Increment only the last 4 bytes of a 16-byte counter (GCM spec) */
static void gcm_inc32(uint8_t v[16]) {
    uint32_t c = ((uint32_t)v[12]<<24)|((uint32_t)v[13]<<16)|
                 ((uint32_t)v[14]<<8)|v[15];
    c++;
    v[12]=(c>>24)&0xFF; v[13]=(c>>16)&0xFF; v[14]=(c>>8)&0xFF; v[15]=c&0xFF;
}

/* Build 12-byte IV from packet sequence number */
static void gcm_iv_from_seq(uint32_t seq, uint8_t iv[12]) {
    memset(iv, 0, 8);
    iv[8]  = (seq >> 24) & 0xFF;
    iv[9]  = (seq >> 16) & 0xFF;
    iv[10] = (seq >> 8)  & 0xFF;
    iv[11] = seq & 0xFF;
}

/* Encrypt the 4-byte SSH packet length using AES-CTR with J0 keystream */
static void ssh_encrypt_length(const uint8_t key[16], const uint8_t iv[12],
                               const uint8_t plain_len[4], uint8_t enc_len[4]) {
    uint8_t rk[176]; aes128_key_expansion(key, rk);
    uint8_t J0[16]; memcpy(J0, iv, 12);
    J0[12]=0; J0[13]=0; J0[14]=0; J0[15]=1;
    uint8_t ks[16]; aes_encrypt_block(J0, ks, rk);
    enc_len[0] = plain_len[0] ^ ks[0];
    enc_len[1] = plain_len[1] ^ ks[1];
    enc_len[2] = plain_len[2] ^ ks[2];
    enc_len[3] = plain_len[3] ^ ks[3];
}

/* Decrypt the 4-byte SSH packet length */
static void ssh_decrypt_length(const uint8_t key[16], const uint8_t iv[12],
                               const uint8_t enc_len[4], uint8_t plain_len[4]) {
    /* CTR mode encryption and decryption are the same operation */
    ssh_encrypt_length(key, iv, enc_len, plain_len);
}

/* AES-128-GCM encrypt (NIST standard, for SSH payload) */
static void ssh_gcm_encrypt(const uint8_t key[16], const uint8_t iv[12],
                             const uint8_t* aad, uint32_t aad_len,
                             const uint8_t* pt, uint32_t pt_len,
                             uint8_t* ct, uint8_t tag[16]) {
    uint8_t rk[176]; aes128_key_expansion(key, rk);
    uint8_t H[16]; memset(H, 0, 16); aes_encrypt_block(H, H, rk);

    /* J0 = IV || 0x00000001 */
    uint8_t J0[16]; memcpy(J0, iv, 12);
    J0[12]=0; J0[13]=0; J0[14]=0; J0[15]=1;

    /* Counter starts at inc32(J0) = IV || 0x00000002 */
    uint8_t counter[16]; memcpy(counter, J0, 16); gcm_inc32(counter);
    uint32_t i = 0;
    while (i < pt_len) {
        uint8_t ks[16]; aes_encrypt_block(counter, ks, rk);
        uint32_t blen = (pt_len - i < 16) ? pt_len - i : 16;
        for (uint32_t j = 0; j < blen; j++) ct[i+j] = pt[i+j] ^ ks[j];
        i += blen; gcm_inc32(counter);
    }

    /* GHASH */
    uint8_t Y[16]; memset(Y, 0, 16);
    if (aad_len > 0) ghash_update(Y, aad, aad_len, H);
    if (pt_len > 0) ghash_update(Y, ct, pt_len, H);
    uint8_t lb[16];
    uint64_t ab = (uint64_t)aad_len*8, cb = (uint64_t)pt_len*8;
    for (int j=0;j<8;j++) lb[j]=(ab>>(56-8*j))&0xFF;
    for (int j=0;j<8;j++) lb[8+j]=(cb>>(56-8*j))&0xFF;
    for (int j=0;j<16;j++) Y[j]^=lb[j];
    uint8_t tmp[16]; ghash_mult(Y, H, tmp); memcpy(Y, tmp, 16);

    /* Tag = GHASH XOR E(J0) */
    uint8_t EJ0[16]; aes_encrypt_block(J0, EJ0, rk);
    for (int j = 0; j < 16; j++) tag[j] = EJ0[j] ^ Y[j];
}

/* AES-128-GCM decrypt (NIST standard, for SSH payload) */
static int ssh_gcm_decrypt(const uint8_t key[16], const uint8_t iv[12],
                            const uint8_t* aad, uint32_t aad_len,
                            const uint8_t* ct, uint32_t ct_len,
                            const uint8_t tag[16], uint8_t* pt) {
    uint8_t rk[176]; aes128_key_expansion(key, rk);
    uint8_t H[16]; memset(H, 0, 16); aes_encrypt_block(H, H, rk);

    uint8_t J0[16]; memcpy(J0, iv, 12);
    J0[12]=0; J0[13]=0; J0[14]=0; J0[15]=1;

    /* Compute expected tag first (verify before decrypting) */
    uint8_t Y[16]; memset(Y, 0, 16);
    if (aad_len > 0) ghash_update(Y, aad, aad_len, H);
    if (ct_len > 0) ghash_update(Y, ct, ct_len, H);
    uint8_t lb[16];
    uint64_t ab=(uint64_t)aad_len*8, cb=(uint64_t)ct_len*8;
    for (int j=0;j<8;j++) lb[j]=(ab>>(56-8*j))&0xFF;
    for (int j=0;j<8;j++) lb[8+j]=(cb>>(56-8*j))&0xFF;
    for (int j=0;j<16;j++) Y[j]^=lb[j];
    uint8_t tmp[16]; ghash_mult(Y, H, tmp); memcpy(Y, tmp, 16);
    uint8_t EJ0[16]; aes_encrypt_block(J0, EJ0, rk);
    uint8_t computed[16];
    for (int j=0;j<16;j++) computed[j]=EJ0[j]^Y[j];

    /* Constant-time tag comparison */
    uint8_t diff = 0;
    for (int j = 0; j < 16; j++) diff |= computed[j] ^ tag[j];
    if (diff != 0) return 0; /* authentication failure */

    /* Decrypt using CTR mode */
    uint8_t counter[16]; memcpy(counter, J0, 16); gcm_inc32(counter);
    uint32_t i = 0;
    while (i < ct_len) {
        uint8_t ks[16]; aes_encrypt_block(counter, ks, rk);
        uint32_t blen = (ct_len-i<16)?ct_len-i:16;
        for (uint32_t j=0;j<blen;j++) pt[i+j]=ct[i+j]^ks[j];
        i += blen; gcm_inc32(counter);
    }
    return 1;
}

/* ===== Big Integer Multiplication (for modular arithmetic over n) ===== */

/* Multiply two 32-byte big-endian numbers → 64-byte big-endian result */
static void big_mul256(const uint8_t a[32], const uint8_t b[32], uint8_t r[64]) {
    /* Work in little-endian for easier carry propagation */
    uint8_t a_le[32], b_le[32];
    for (int i = 0; i < 32; i++) { a_le[i] = a[31-i]; b_le[i] = b[31-i]; }

    uint32_t acc[64]; memset(acc, 0, sizeof(acc));
    for (int i = 0; i < 32; i++) {
        uint16_t ai = a_le[i];
        for (int j = 0; j < 32; j++) acc[i+j] += ai * b_le[j];
    }
    /* Carry propagation */
    for (int i = 0; i < 63; i++) {
        acc[i+1] += acc[i] >> 8; acc[i] &= 0xFF;
    }
    acc[63] &= 0xFF;
    /* Convert back to big-endian */
    for (int i = 0; i < 64; i++) r[i] = (uint8_t)acc[63-i];
}

/* Reduce 512-bit big-endian number mod 256-bit modulus using binary long division */
static void mod_reduce(const uint8_t val[64], const uint8_t mod[32], uint8_t result[32]) {
    uint8_t rem[32]; memset(rem, 0, 32);
    for (int i = 0; i < 512; i++) {
        /* Shift rem left by 1 bit */
        uint8_t carry = 0;
        for (int j = 0; j < 32; j++) {
            uint8_t nc = (rem[j] >> 7) & 1;
            rem[j] = (rem[j] << 1) | carry;
            carry = nc;
        }
        /* Add bit i of val (MSB first) */
        rem[31] |= (val[i/8] >> (7-(i%8))) & 1;
        /* If overflow or rem >= mod, reduce */
        if (carry || memcmp(rem, mod, 32) >= 0) {
            if (carry) {
                /* result = delta_n + rem where delta_n = 2^256 - mod */
                uint16_t c = 0;
                for (int j = 31; j >= 0; j--) {
                    uint16_t s = (uint16_t)rem[j] + p256_delta_n[j] + c;
                    rem[j] = s & 0xFF; c = s >> 8;
                }
            } else {
                int32_t bw = 0;
                for (int j = 31; j >= 0; j--) {
                    int32_t d = (int32_t)rem[j] - (int32_t)mod[j] - bw;
                    if (d<0) { d+=256; bw=1; } else bw=0;
                    rem[j] = (uint8_t)d;
                }
            }
        }
    }
    memcpy(result, rem, 32);
}

/* ===== Modular Arithmetic over P-256 Order n ===== */

static void fe_add_n(const uint8_t a[32], const uint8_t b[32], uint8_t r[32]) {
    uint16_t carry = 0;
    for (int i = 31; i >= 0; i--) {
        uint16_t sum = (uint16_t)a[i] + b[i] + carry;
        r[i] = sum & 0xFF; carry = sum >> 8;
    }
    if (carry || memcmp(r, p256_n, 32) >= 0) {
        int32_t bw = 0;
        for (int i = 31; i >= 0; i--) {
            int32_t d = (int32_t)r[i] - (int32_t)p256_n[i] - bw;
            if (d<0) { d+=256; bw=1; } else bw=0;
            r[i] = (uint8_t)d;
        }
    }
}

static void fe_sub_n(const uint8_t a[32], const uint8_t b[32], uint8_t r[32]) {
    int32_t borrow = 0;
    for (int i = 31; i >= 0; i--) {
        int32_t d = (int32_t)a[i] - (int32_t)b[i] - borrow;
        if (d<0) { d+=256; borrow=1; } else borrow=0;
        r[i] = (uint8_t)d;
    }
    if (borrow) {
        uint16_t carry = 0;
        for (int i = 31; i >= 0; i--) {
            uint16_t sum = (uint16_t)r[i] + p256_n[i] + carry;
            r[i] = sum & 0xFF; carry = sum >> 8;
        }
    }
}

static void fe_mul_n(const uint8_t a[32], const uint8_t b[32], uint8_t r[32]) {
    uint8_t product[64]; big_mul256(a, b, product);
    mod_reduce(product, p256_n, r);
}

/* Modular inverse mod n using Fermat's little theorem: a^{-1} = a^{n-2} mod n */
static void fe_inv_n(const uint8_t a[32], uint8_t r[32]) {
    uint8_t base[32], result[32], tmp[32];
    memcpy(base, a, 32);
    memset(result, 0, 32); result[31] = 1; /* result = 1 */

    for (int i = 0; i < 256; i++) {
        fe_mul_n(result, result, tmp); memcpy(result, tmp, 32);
        uint8_t bit = (p256_n_minus_2[i/8] >> (7-(i%8))) & 1;
        if (bit) { fe_mul_n(result, base, tmp); memcpy(result, tmp, 32); }
    }
    memcpy(r, result, 32);
}

/* ===== ECDSA-SHA2-NISTP256 Signing ===== */
static int ecdsa_sign(const uint8_t priv[32], const uint8_t hash[32],
                      uint8_t r_out[32], uint8_t s_out[32]) {
    uint8_t zero32[32]; memset(zero32, 0, 32);

    for (int retry = 0; retry < 10; retry++) {
        /* Generate random nonce k, reduce mod n */
        uint8_t k[32]; get_random_bytes(k, 32);
        if (memcmp(k, p256_n, 32) >= 0) {
            int32_t bw = 0;
            for (int i = 31; i >= 0; i--) {
                int32_t d = (int32_t)k[i]-(int32_t)p256_n[i]-bw;
                if (d<0){d+=256;bw=1;}else bw=0;
                k[i]=(uint8_t)d;
            }
        }
        if (memcmp(k, zero32, 32) == 0) continue;

        /* Compute R = k*G using p256_ecdh with base point as "peer" */
        uint8_t Rx[32];
        if (p256_ecdh(k, p256_Gx, p256_Gy, Rx) != 1) continue;

        /* r = Rx mod n */
        memcpy(r_out, Rx, 32);
        if (memcmp(r_out, p256_n, 32) >= 0) {
            int32_t bw=0;
            for (int i=31;i>=0;i--) {
                int32_t d=(int32_t)r_out[i]-(int32_t)p256_n[i]-bw;
                if(d<0){d+=256;bw=1;}else bw=0;
                r_out[i]=(uint8_t)d;
            }
        }
        if (memcmp(r_out, zero32, 32) == 0) continue;

        /* z = hash mod n */
        uint8_t z[32]; memcpy(z, hash, 32);
        if (memcmp(z, p256_n, 32) >= 0) {
            int32_t bw=0;
            for(int i=31;i>=0;i--){
                int32_t d=(int32_t)z[i]-(int32_t)p256_n[i]-bw;
                if(d<0){d+=256;bw=1;}else bw=0;
                z[i]=(uint8_t)d;
            }
        }

        /* s = k^{-1} * (z + r*d) mod n */
        uint8_t k_inv[32]; fe_inv_n(k, k_inv);
        uint8_t rd[32]; fe_mul_n(r_out, priv, rd);
        uint8_t zrd[32]; fe_add_n(z, rd, zrd);
        fe_mul_n(k_inv, zrd, s_out);

        if (memcmp(s_out, zero32, 32) == 0) continue;
        return 1;
    }
    return 0;
}

/* ===== SSH Data Type Helpers =====
 * SSH uses a specific binary encoding for protocol data (RFC 4251 Section 5):
 *   uint32:   4 bytes big-endian
 *   string:   uint32(length) || byte[length]
 *   mpint:    uint32(length) || byte[length] (big-endian, minimal, positive)
 *   boolean:  1 byte (0=false, nonzero=true)
 *   name-list: uint32(length) || comma-separated ASCII string */

/* Buffer writer: accumulates SSH data types into a byte buffer */
struct ssh_buf {
    uint8_t* data;
    uint32_t len;
    uint32_t cap;
};

static void buf_init(struct ssh_buf* b, uint8_t* data, uint32_t cap) {
    b->data = data; b->len = 0; b->cap = cap;
}

static void buf_put_u8(struct ssh_buf* b, uint8_t v) {
    if (b->len < b->cap) b->data[b->len++] = v;
}

static void buf_put_u32(struct ssh_buf* b, uint32_t v) {
    if (b->len + 4 <= b->cap) {
        b->data[b->len++]=(v>>24)&0xFF; b->data[b->len++]=(v>>16)&0xFF;
        b->data[b->len++]=(v>>8)&0xFF;  b->data[b->len++]=v&0xFF;
    }
}

static void buf_put_bytes(struct ssh_buf* b, const uint8_t* d, uint32_t len) {
    if (b->len + len <= b->cap) { memcpy(b->data+b->len, d, len); b->len += len; }
}

static void buf_put_string(struct ssh_buf* b, const uint8_t* d, uint32_t len) {
    buf_put_u32(b, len); buf_put_bytes(b, d, len);
}

static void buf_put_cstring(struct ssh_buf* b, const char* s) {
    buf_put_string(b, (const uint8_t*)s, (uint32_t)strlen(s));
}

/* Put mpint: SSH big-endian positive integer with minimal representation.
 * If MSB of first byte >= 0x80, prepend a zero byte for sign. */
static void buf_put_mpint(struct ssh_buf* b, const uint8_t* val, uint32_t vlen) {
    /* Strip leading zero bytes */
    uint32_t start = 0;
    while (start < vlen && val[start] == 0) start++;
    uint32_t slen = vlen - start;
    if (slen == 0) { buf_put_u32(b, 0); return; }
    /* If MSB has high bit, prepend zero for sign */
    if (val[start] & 0x80) {
        buf_put_u32(b, slen + 1);
        buf_put_u8(b, 0);
        buf_put_bytes(b, val + start, slen);
    } else {
        buf_put_u32(b, slen);
        buf_put_bytes(b, val + start, slen);
    }
}

static void buf_put_bool(struct ssh_buf* b, int v) {
    buf_put_u8(b, v ? 1 : 0);
}

/* Buffer reader: extracts SSH data types from a byte buffer */
struct ssh_rbuf {
    const uint8_t* data;
    uint32_t pos;
    uint32_t len;
};

static void rbuf_init(struct ssh_rbuf* r, const uint8_t* data, uint32_t len) {
    r->data = data; r->pos = 0; r->len = len;
}

static int rbuf_remaining(struct ssh_rbuf* r) { return r->len - r->pos; }

static uint8_t rbuf_get_u8(struct ssh_rbuf* r) {
    if (r->pos >= r->len) return 0;
    return r->data[r->pos++];
}

static uint32_t rbuf_get_u32(struct ssh_rbuf* r) {
    if (r->pos + 4 > r->len) return 0;
    uint32_t v = ((uint32_t)r->data[r->pos]<<24)|
                 ((uint32_t)r->data[r->pos+1]<<16)|
                 ((uint32_t)r->data[r->pos+2]<<8)|
                 r->data[r->pos+3];
    r->pos += 4; return v;
}

static int rbuf_get_bytes(struct ssh_rbuf* r, uint8_t* out, uint32_t len) {
    if (r->pos + len > r->len) return 0;
    if (out) memcpy(out, r->data + r->pos, len);
    r->pos += len; return 1;
}

/* Get SSH string: returns pointer to string data and length. Does NOT copy. */
static const uint8_t* rbuf_get_string(struct ssh_rbuf* r, uint32_t* out_len) {
    *out_len = rbuf_get_u32(r);
    if (r->pos + *out_len > r->len) { *out_len = 0; return NULL; }
    const uint8_t* ptr = r->data + r->pos;
    r->pos += *out_len;
    return ptr;
}

/* Get SSH string and copy into buffer with null terminator */
static int rbuf_get_cstring(struct ssh_rbuf* r, char* out, uint32_t out_cap) {
    uint32_t slen;
    const uint8_t* s = rbuf_get_string(r, &slen);
    if (!s || slen >= out_cap) return 0;
    memcpy(out, s, slen); out[slen] = '\0'; return 1;
}

/* Get mpint: extract big-endian integer into fixed-size buffer */
static int rbuf_get_mpint(struct ssh_rbuf* r, uint8_t* out, uint32_t out_len) {
    uint32_t mpint_len = rbuf_get_u32(r);
    if (mpint_len == 0) { memset(out, 0, out_len); return 1; }
    if (r->pos + mpint_len > r->len) return 0;
    /* Skip leading zero (sign byte) if present */
    uint32_t start = 0;
    if (r->data[r->pos] == 0 && mpint_len > 1) { start = 1; }
    uint32_t actual_len = mpint_len - start;
    if (actual_len > out_len) return 0;
    memset(out, 0, out_len);
    memcpy(out + out_len - actual_len, r->data + r->pos + start, actual_len);
    r->pos += mpint_len;
    return 1;
}

/* Forward declarations */
static void ssh_execute_and_send(struct ssh_session* s, const char* line);

/* ===== Host Key Management ===== */
struct ssh_host_key {
    uint8_t priv[32];     /* ECDSA private key */
    uint8_t pub_x[32];    /* ECDSA public key x-coordinate */
    uint8_t pub_y[32];    /* ECDSA public key y-coordinate */
    int initialized;
};

static struct ssh_host_key host_key;

static void host_key_generate(void) {
    if (host_key.initialized) return;
    p256_keygen(host_key.priv, host_key.pub_x, host_key.pub_y);
    host_key.initialized = 1;
    pr_info("ssh: host key generated (ecdsa-sha2-nistp256)\n");
}

/* Build the host key blob (K_S) for SSH protocol:
 * string "ecdsa-sha2-nistp256"
 * string "nistp256"
 * string Q (uncompressed point: 04 || x || y) */
static uint32_t host_key_blob(uint8_t* buf, uint32_t cap) {
    struct ssh_buf b; buf_init(&b, buf, cap);
    buf_put_cstring(&b, "ecdsa-sha2-nistp256");
    buf_put_cstring(&b, "nistp256");
    /* Uncompressed point: 04 || pub_x || pub_y */
    uint8_t point[65];
    point[0] = 0x04;
    memcpy(point+1, host_key.pub_x, 32);
    memcpy(point+1+32, host_key.pub_y, 32);
    buf_put_string(&b, point, 65);
    return b.len;
}

/* Build the ECDSA signature blob for SSH:
 * string "ecdsa-sha2-nistp256"
 * string sig_inner (mpint r || mpint s) */
static uint32_t ecdsa_sig_blob(const uint8_t hash[32], uint8_t* buf, uint32_t cap) {
    uint8_t r[32], s[32];
    if (!ecdsa_sign(host_key.priv, hash, r, s)) {
        pr_err("ssh: ECDSA signing failed\n");
        return 0;
    }
    struct ssh_buf b; buf_init(&b, buf, cap);
    buf_put_cstring(&b, "ecdsa-sha2-nistp256");
    /* Inner signature blob: mpint r || mpint s */
    uint8_t inner[128]; struct ssh_buf ib; buf_init(&ib, inner, 128);
    buf_put_mpint(&ib, r, 32);
    buf_put_mpint(&ib, s, 32);
    buf_put_string(&b, inner, ib.len);
    return b.len;
}

/* ===== SSH Key Derivation (RFC 4253 Section 7.2) =====
 * Derive key material from shared secret K, exchange hash H, and session_id.
 * Key = HASH(K || H || letter || session_id) where letter is 'A'-'F'.
 * For AES-128-GCM we need: 16-byte keys for each direction. */
static void derive_key(const uint8_t K[], uint32_t K_len,
                       const uint8_t H[32], const uint8_t session_id[32],
                       char letter, uint8_t* out, uint32_t out_len) {
    /* Build: K(mpint) || H || letter || session_id */
    /* K is encoded as mpint (SSH big-endian positive integer) */
    uint8_t kbuf[128]; struct ssh_buf kb; buf_init(&kb, kbuf, 128);
    buf_put_mpint(&kb, K, K_len);

    uint8_t hash_input[256]; struct ssh_buf hi; buf_init(&hi, hash_input, 256);
    buf_put_bytes(&hi, kbuf, kb.len);
    buf_put_bytes(&hi, H, 32);
    buf_put_u8(&hi, (uint8_t)letter);
    buf_put_bytes(&hi, session_id, 32);

    uint8_t hash_out[32]; sha256_hash(hash_input, hi.len, hash_out);
    /* If more bytes needed than 32, extend: HASH(K || H || letter || session_id || hash_out) */
    if (out_len <= 32) {
        memcpy(out, hash_out, out_len);
    } else {
        memcpy(out, hash_out, 32);
        /* Derive more: hash again with previous hash appended */
        struct ssh_buf hi2; buf_init(&hi2, hash_input, 256);
        buf_put_bytes(&hi2, kbuf, kb.len);
        buf_put_bytes(&hi2, H, 32);
        buf_put_u8(&hi2, (uint8_t)letter);
        buf_put_bytes(&hi2, session_id, 32);
        buf_put_bytes(&hi2, hash_out, 32);
        sha256_hash(hash_input, hi2.len, hash_out);
        memcpy(out + 32, hash_out, out_len - 32);
    }
}

/* ===== SSH Packet Building and Parsing ===== */

/* Build an unencrypted SSH binary packet.
 * Format: packet_length(4) | padding_length(1) | payload(N) | random_padding(P)
 * packet_length = 1 + N + P, must be multiple of 8, >= 8.
 * Returns total bytes written, or 0 on error. */
static uint32_t ssh_build_packet(uint8_t* buf, uint32_t cap,
                                 const uint8_t* payload, uint32_t payload_len) {
    uint32_t block_size = 8;
    uint32_t min_padding = 4;
    uint32_t pkt_len = 1 + payload_len + min_padding;
    /* Round up to multiple of block_size */
    pkt_len = ((pkt_len + block_size - 1) / block_size) * block_size;
    uint32_t pad_len = pkt_len - 1 - payload_len;
    if (pad_len > 255) pad_len = 255; /* SSH max padding */
    pkt_len = 1 + payload_len + pad_len;

    uint32_t total = 4 + pkt_len;
    if (total > cap) return 0;

    /* packet_length */
    buf[0]=(pkt_len>>24)&0xFF; buf[1]=(pkt_len>>16)&0xFF;
    buf[2]=(pkt_len>>8)&0xFF;  buf[3]=pkt_len&0xFF;
    /* padding_length */
    buf[4] = (uint8_t)pad_len;
    /* payload */
    memcpy(buf + 5, payload, payload_len);
    /* random padding */
    get_random_bytes(buf + 5 + payload_len, pad_len);
    return total;
}

/* Build an encrypted SSH binary packet (AES-128-GCM).
 * Format: encrypted_length(4) | encrypted_payload(pkt_len) | auth_tag(16)
 * The length is encrypted with CTR(J0). The payload is GCM-encrypted
 * with encrypted_length as AAD. IV is derived from send sequence number. */
static uint32_t ssh_build_enc_packet(struct ssh_session* s,
                                     uint8_t* buf, uint32_t cap,
                                     const uint8_t* payload, uint32_t payload_len) {
    uint32_t block_size = 8;
    uint32_t min_padding = 4;
    uint32_t pkt_len = 1 + payload_len + min_padding;
    pkt_len = ((pkt_len + block_size - 1) / block_size) * block_size;
    uint32_t pad_len = pkt_len - 1 - payload_len;
    if (pad_len > 255) pad_len = 255;
    pkt_len = 1 + payload_len + pad_len;

    uint32_t total = 4 + pkt_len + AES_GCM_TAG_LEN;
    if (total > cap) return 0;

    /* Build plaintext: padding_length(1) | payload | random_padding */
    uint8_t plaintext[SSH_BUF_SIZE];
    plaintext[0] = (uint8_t)pad_len;
    memcpy(plaintext + 1, payload, payload_len);
    get_random_bytes(plaintext + 1 + payload_len, pad_len);

    /* Build IV from sequence number */
    uint8_t iv[12]; gcm_iv_from_seq(s->enc_send.seq, iv);

    /* Encrypt packet_length using CTR with J0 */
    uint8_t plain_len[4], enc_len[4];
    plain_len[0]=(pkt_len>>24)&0xFF; plain_len[1]=(pkt_len>>16)&0xFF;
    plain_len[2]=(pkt_len>>8)&0xFF;  plain_len[3]=pkt_len&0xFF;
    ssh_encrypt_length(s->enc_send.key, iv, plain_len, enc_len);

    /* Encrypt payload+padding using GCM, AAD=encrypted_length */
    uint8_t ciphertext[SSH_BUF_SIZE], tag[16];
    ssh_gcm_encrypt(s->enc_send.key, iv, enc_len, 4,
                    plaintext, pkt_len, ciphertext, tag);

    /* Assemble: enc_len(4) | ciphertext(pkt_len) | tag(16) */
    memcpy(buf, enc_len, 4);
    memcpy(buf + 4, ciphertext, pkt_len);
    memcpy(buf + 4 + pkt_len, tag, AES_GCM_TAG_LEN);

    s->enc_send.seq++;
    return total;
}

/* Try to extract one complete unencrypted SSH packet from rx_buf.
 * Returns total bytes consumed (including the packet), or 0 if
 * incomplete, or -1 on error. payload/payload_len get the
 * decoded payload (without padding). */
static int ssh_recv_packet(struct ssh_session* s,
                           uint8_t* payload, uint32_t* payload_len) {
    if (s->rx_len < 5) return 0; /* need at least packet_length + padding_length */

    /* Peek at packet_length */
    uint32_t pkt_len = ((uint32_t)s->rx_buf[0]<<24)|
                       ((uint32_t)s->rx_buf[1]<<16)|
                       ((uint32_t)s->rx_buf[2]<<8)|
                       s->rx_buf[3];
    if (pkt_len < 8 || pkt_len > SSH_BUF_SIZE - 4) return -1;
    uint32_t total = 4 + pkt_len;
    if (s->rx_len < total) return 0; /* incomplete */

    uint8_t pad_len = s->rx_buf[4];
    *payload_len = pkt_len - 1 - pad_len;
    if (*payload_len > pkt_len) return -1;
    memcpy(payload, s->rx_buf + 5, *payload_len);
    return total;
}

/* Try to extract one complete encrypted SSH packet (AES-GCM) from rx_buf.
 * Returns bytes consumed, 0 if incomplete, -1 on auth failure or error. */
static int ssh_recv_enc_packet(struct ssh_session* s,
                               uint8_t* payload, uint32_t* payload_len) {
    if (s->rx_len < 4) return 0;

    /* Decrypt packet_length from first 4 bytes */
    uint8_t iv[12]; gcm_iv_from_seq(s->enc_recv.seq, iv);
    uint8_t enc_len[4], plain_len[4];
    memcpy(enc_len, s->rx_buf, 4);
    ssh_decrypt_length(s->enc_recv.key, iv, enc_len, plain_len);

    uint32_t pkt_len = ((uint32_t)plain_len[0]<<24)|
                       ((uint32_t)plain_len[1]<<16)|
                       ((uint32_t)plain_len[2]<<8)|
                       plain_len[3];
    if (pkt_len < 8 || pkt_len > SSH_BUF_SIZE - 4 - AES_GCM_TAG_LEN) {
        pr_err("ssh: invalid encrypted packet length %u\n", pkt_len);
        return -1;
    }

    uint32_t total = 4 + pkt_len + AES_GCM_TAG_LEN;
    if (s->rx_len < total) return 0; /* incomplete */

    /* Decrypt and verify payload using GCM, AAD=encrypted_length(4 bytes) */
    uint8_t plaintext[SSH_BUF_SIZE];
    if (!ssh_gcm_decrypt(s->enc_recv.key, iv, s->rx_buf, 4,
                         s->rx_buf + 4, pkt_len,
                         s->rx_buf + 4 + pkt_len, plaintext)) {
        pr_err("ssh: GCM authentication failure (seq %u)\n", s->enc_recv.seq);
        return -1;
    }

    /* Extract padding_length and payload */
    uint8_t pad_len = plaintext[0];
    *payload_len = pkt_len - 1 - pad_len;
    if (*payload_len > pkt_len) return -1;
    memcpy(payload, plaintext + 1, *payload_len);

    s->enc_recv.seq++;
    return total;
}

/* ===== Credential Store ===== */
#define SSH_MAX_USERS 4

struct ssh_user {
    char username[32];
    char password[64];
};

static struct ssh_user users[SSH_MAX_USERS];
static int user_count = 0;

static void ssh_init_users(void) {
    user_count = 0;
    strncpy(users[0].username, "root", 31); strncpy(users[0].password, "lestra", 63);
    strncpy(users[1].username, "admin", 31); strncpy(users[1].password, "admin", 63);
    user_count = 2;
}

static int ssh_check_password(const char* username, const char* password) {
    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].username, username) == 0 &&
            strcmp(users[i].password, password) == 0)
            return 1;
    }
    return 0;
}

/* ===== Session State ===== */
static struct ssh_session sessions[SSH_MAX_SESSIONS];
static int listen_idx = -1;
static int ssh_running = 0;
static int ssh_port = SSH_DEFAULT_PORT;

void ssh_server_init(void) {
    memset(sessions, 0, sizeof(sessions));
    ssh_running = 0; listen_idx = -1;
    ssh_init_users();
    host_key_generate();
    pr_info("ssh: SSH-2.0 server initialized (%d user(s), host key ready)\n", user_count);
}

int ssh_server_start(uint16_t port) {
    if (ssh_running) { pr_warn("ssh: already running\n"); return 0; }
    ssh_port = port;
    listen_idx = tcp_listen(port, SSH_MAX_SESSIONS);
    if (listen_idx < 0) { pr_err("ssh: failed to listen on port %u\n", (unsigned)port); return -1; }
    ssh_running = 1;
    pr_info("ssh: SSH-2.0 server listening on port %u\n", (unsigned)port);
    printk("SSH-2.0 server listening on port %u\n", (unsigned)port);
    return 0;
}

int ssh_server_stop(void) {
    if (!ssh_running) return 0;
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        if (sessions[i].in_use && sessions[i].conn_idx >= 0) {
            struct tcp_conn* c = tcp_get_conn(sessions[i].conn_idx);
            if (c) tcp_close_conn(c);
            memset(&sessions[i], 0, sizeof(sessions[i]));
        }
    }
    if (listen_idx >= 0) {
        struct tcp_conn* lc = tcp_get_conn(listen_idx);
        if (lc) { lc->in_use = 0; lc->state = TCP_CLOSED; }
        listen_idx = -1;
    }
    ssh_running = 0;
    pr_info("ssh: stopped\n");
    printk("SSH-2.0 server stopped.\n");
    return 0;
}

int ssh_server_is_running(void) { return ssh_running; }

static int ssh_alloc_session(void) {
    for (int i = 0; i < SSH_MAX_SESSIONS; i++)
        if (!sessions[i].in_use) return i;
    return -1;
}

/* ===== Send helpers: send raw bytes or SSH packets over TCP ===== */

static int ssh_send_raw(struct ssh_session* s, const uint8_t* data, uint32_t len) {
    struct tcp_conn* c = tcp_get_conn(s->conn_idx);
    if (!c || c->state != TCP_ESTABLISHED) return -1;
    uint32_t sent = 0;
    while (sent < len) {
        uint32_t chunk = len - sent; if (chunk > 1400) chunk = 1400;
        int n = tcp_send_conn(c, (const uint8_t*)(data + sent), (uint16_t)chunk);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* Send an SSH message as a packet (encrypted if active) */
static int ssh_send_msg(struct ssh_session* s, const uint8_t* payload, uint32_t len) {
    uint8_t pkt_buf[SSH_BUF_SIZE];
    uint32_t pkt_len;
    if (s->encryption_active) {
        pkt_len = ssh_build_enc_packet(s, pkt_buf, SSH_BUF_SIZE, payload, len);
    } else {
        pkt_len = ssh_build_packet(pkt_buf, SSH_BUF_SIZE, payload, len);
    }
    if (pkt_len == 0) return -1;
    return ssh_send_raw(s, pkt_buf, pkt_len);
}

/* Send SSH_MSG_DISCONNECT with reason code and description */
static void ssh_disconnect(struct ssh_session* s, uint32_t reason, const char* desc) {
    uint8_t payload[256]; struct ssh_buf b; buf_init(&b, payload, 256);
    buf_put_u8(&b, SSH_MSG_DISCONNECT);
    buf_put_u32(&b, reason);
    buf_put_cstring(&b, desc);
    ssh_send_msg(s, payload, b.len);
    struct tcp_conn* c = tcp_get_conn(s->conn_idx);
    if (c) tcp_close_conn(c);
    s->in_use = 0;
}

/* ===== Protocol Handlers ===== */

/* Process client version string. Find the first line starting with "SSH-2.0-".
 * Returns bytes consumed, 0 if no complete line, -1 on error. */
static int process_version(struct ssh_session* s) {
    /* Look for \r\n or \n terminator */
    int line_end = -1;
    for (int i = 0; i < s->rx_len; i++) {
        if (s->rx_buf[i] == '\n') { line_end = i; break; }
    }
    if (line_end < 0) return 0;

    /* Skip any leading lines not starting with SSH- */
    int start = 0;
    while (start < line_end) {
        /* Find start of current line */
        int line_start = start;
        while (line_start < line_end && s->rx_buf[line_start] != '\n') line_start++;
        /* Actually, look backwards for the line containing SSH- */
        break;
    }

    /* Extract version line (strip \r if present) */
    int ver_end = line_end;
    if (ver_end > 0 && s->rx_buf[ver_end-1] == '\r') ver_end--;
    int ver_start = 0;
    /* Skip any preceding lines that don't start with "SSH-" */
    for (int i = 0; i < ver_end; i++) {
        if (s->rx_buf[i] == '\n') {
            int next_start = i + 1;
            if (next_start < ver_end &&
                s->rx_buf[next_start] == 'S' &&
                next_start + 3 < ver_end &&
                s->rx_buf[next_start+1] == 'S' &&
                s->rx_buf[next_start+2] == 'H' &&
                s->rx_buf[next_start+3] == '-') {
                ver_start = next_start;
                break;
            }
        }
    }
    /* If we didn't find SSH- prefix, try the whole line */
    if (s->rx_buf[ver_start] != 'S') {
        /* Check if the entire extracted line starts with SSH- */
        if (s->rx_buf[0] == 'S' && s->rx_buf[1] == 'S' && s->rx_buf[2] == 'H' && s->rx_buf[3] == '-')
            ver_start = 0;
        else
            return -1; /* No valid SSH version string */
    }

    int ver_len = ver_end - ver_start;
    if (ver_len >= 256) ver_len = 255;
    memcpy(s->client_version, s->rx_buf + ver_start, ver_len);
    s->client_version[ver_len] = '\0';

    /* Check it starts with "SSH-2.0-" */
    if (strncmp(s->client_version, "SSH-2.0-", 8) != 0 &&
        strncmp(s->client_version, "SSH-1.99-", 9) != 0) {
        pr_warn("ssh: client version not SSH-2.0: '%s'\n", s->client_version);
        return -1;
    }

    pr_info("ssh: client version: %s\n", s->client_version);

    /* Consume the line (including \n and optional \r) */
    int consumed = line_end + 1;
    /* Also consume any preceding non-SSH lines */
    if (ver_start > 0) consumed = line_end + 1; /* consume everything up to \n */

    return consumed;
}

/* Build and send our KEXINIT message. Also store it for hash computation. */
static void send_kexinit(struct ssh_session* s) {
    uint8_t payload[512]; struct ssh_buf b; buf_init(&b, payload, 512);
    buf_put_u8(&b, SSH_MSG_KEX_INIT);
    /* Cookie: 16 random bytes */
    uint8_t cookie[16]; get_random_bytes(cookie, 16);
    buf_put_bytes(&b, cookie, 16);
    /* Algorithm name-lists */
    buf_put_cstring(&b, KEX_ALGORITHMS);
    buf_put_cstring(&b, HOSTKEY_ALGORITHMS);
    buf_put_cstring(&b, ENC_ALGORITHMS);      /* enc c->s */
    buf_put_cstring(&b, ENC_ALGORITHMS);      /* enc s->c */
    buf_put_cstring(&b, MAC_ALGORITHMS);       /* mac c->s */
    buf_put_cstring(&b, MAC_ALGORITHMS);       /* mac s->c */
    buf_put_cstring(&b, COMP_ALGORITHMS);      /* comp c->s */
    buf_put_cstring(&b, COMP_ALGORITHMS);      /* comp s->c */
    buf_put_cstring(&b, "");                   /* lang c->s */
    buf_put_cstring(&b, "");                   /* lang s->c */
    buf_put_bool(&b, 0);                       /* first_kex_follows */
    buf_put_u32(&b, 0);                        /* reserved */

    /* Store raw KEXINIT payload (without message type byte) for hash computation */
    s->server_kexinit_len = b.len - 1; /* skip the SSH_MSG_KEX_INIT byte */
    memcpy(s->server_kexinit, payload + 1, s->server_kexinit_len);

    ssh_send_msg(s, payload, b.len);
    pr_debug("ssh: KEXINIT sent\n");
}

/* Parse client KEXINIT, store for hash, and verify algorithm compatibility.
 * Returns 1 on success, 0 on failure. */
static int process_kexinit(struct ssh_session* s, const uint8_t* payload, uint32_t len) {
    if (len < 17 || payload[0] != SSH_MSG_KEX_INIT) return 0;

    /* Store raw KEXINIT payload (without message type byte) */
    s->client_kexinit_len = len - 1;
    if (s->client_kexinit_len > sizeof(s->client_kexinit)) return 0;
    memcpy(s->client_kexinit, payload + 1, s->client_kexinit_len);

    /* Parse and verify algorithm negotiation */
    struct ssh_rbuf r; rbuf_init(&r, payload, len);
    rbuf_get_u8(&r); /* skip message type */
    rbuf_get_bytes(&r, NULL, 16); /* skip cookie */

    /* Check each name-list for compatibility */
    const char* our_algos[] = {
        KEX_ALGORITHMS, HOSTKEY_ALGORITHMS, ENC_ALGORITHMS, ENC_ALGORITHMS,
        MAC_ALGORITHMS, MAC_ALGORITHMS, COMP_ALGORITHMS, COMP_ALGORITHMS, "", ""
    };

    for (int i = 0; i < 10; i++) {
        uint32_t slen; const uint8_t* s = rbuf_get_string(&r, &slen);
        if (!s) return 0;
        /* Check that our algorithm appears in client's list */
        const char* our = our_algos[i];
        uint32_t our_len = (uint32_t)strlen(our);
        /* Search for our algo in client's comma-separated list */
        int found = 0;
        uint32_t pos = 0;
        while (pos < slen) {
            uint32_t end = pos;
            while (end < slen && s[end] != ',') end++;
            if (end - pos == our_len && memcmp(s + pos, our, our_len) == 0) {
                found = 1; break;
            }
            pos = end + 1;
        }
        if (!found && our_len > 0) {
            pr_warn("ssh: algorithm negotiation failed for category %d\n", i);
            return 0;
        }
    }
    /* Skip first_kex_follows and reserved */
    rbuf_get_u8(&r); rbuf_get_u32(&r);

    pr_debug("ssh: KEXINIT from client parsed, algorithms compatible\n");
    return 1;
}

/* Process ECDH_INIT from client, compute shared secret and exchange hash,
 * then send ECDH_REPLY + NEWKEYS. Returns 1 on success, 0 on error. */
static int process_ecdh_init(struct ssh_session* s, const uint8_t* payload, uint32_t len) {
    if (len < 69 || payload[0] != SSH_MSG_KEX_ECDH_INIT) {
        pr_warn("ssh: invalid ECDH_INIT message\n");
        return 0;
    }

    struct ssh_rbuf r; rbuf_init(&r, payload, len);
    rbuf_get_u8(&r); /* skip message type */

    /* Client's ECDH public key: SSH string containing 04 || x || y */
    uint32_t q_c_len; const uint8_t* q_c = rbuf_get_string(&r, &q_c_len);
    if (!q_c || q_c_len != 65 || q_c[0] != 0x04) {
        pr_warn("ssh: invalid client ECDH public key\n");
        return 0;
    }
    memcpy(s->client_ecdh_pub, q_c + 1, 64); /* x || y */

    /* Generate our ephemeral ECDH key pair */
    p256_keygen(s->ecdh_priv, s->ecdh_pub, s->ecdh_pub + 32);

    /* Compute shared secret */
    if (p256_ecdh(s->ecdh_priv, s->client_ecdh_pub, s->client_ecdh_pub + 32,
                  s->shared_secret) != 1) {
        pr_err("ssh: ECDH computation failed\n");
        return 0;
    }

    /* Build host key blob (K_S) */
    uint8_t ks_buf[128]; uint32_t ks_len = host_key_blob(ks_buf, 128);

    /* Build our ECDH public key string (Q_S): 04 || pub_x || pub_y */
    uint8_t q_s[65]; q_s[0] = 0x04;
    memcpy(q_s+1, s->ecdh_pub, 64);

    /* Compute exchange hash H = SHA256(V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || K)
     * Each field is encoded as an SSH string (uint32 length || data), except K which is mpint. */

    /* Encode all fields as SSH strings/mpint */
    uint8_t vc_buf[256], vs_buf[256], ic_buf[1024+4], is_buf[1024+4];
    uint8_t ks_str[128+4], qc_str[65+4], qs_str[65+4], k_buf[128];
    struct ssh_buf vc, vs, ic, is, ks_s, qc_s, qs_s, kb;

    /* V_C: client version string */
    buf_init(&vc, vc_buf, 256);
    buf_put_cstring(&vc, s->client_version);

    /* V_S: server version string */
    buf_init(&vs, vs_buf, 256);
    buf_put_cstring(&vs, SSH_VERSION_STRING);

    /* I_C: client KEXINIT payload */
    buf_init(&ic, ic_buf, sizeof(ic_buf));
    buf_put_string(&ic, s->client_kexinit, s->client_kexinit_len);

    /* I_S: server KEXINIT payload */
    buf_init(&is, is_buf, sizeof(is_buf));
    buf_put_string(&is, s->server_kexinit, s->server_kexinit_len);

    /* K_S: host key blob */
    buf_init(&ks_s, ks_str, sizeof(ks_str));
    buf_put_string(&ks_s, ks_buf, ks_len);

    /* Q_C: client ECDH public key */
    buf_init(&qc_s, qc_str, sizeof(qc_str));
    buf_put_string(&qc_s, q_c, q_c_len);

    /* Q_S: server ECDH public key */
    buf_init(&qs_s, qs_str, sizeof(qs_str));
    buf_put_string(&qs_s, q_s, 65);

    /* K: shared secret as mpint */
    buf_init(&kb, k_buf, sizeof(k_buf));
    buf_put_mpint(&kb, s->shared_secret, 32);

    /* Compute H = SHA256(all fields concatenated) */
    const uint8_t* parts[] = {vc_buf, vs_buf, ic_buf, is_buf, ks_str, qc_str, qs_str, k_buf};
    const uint32_t lens[] = {vc.len, vs.len, ic.len, is.len, ks_s.len, qc_s.len, qs_s.len, kb.len};
    uint8_t H[32];
    sha256_multi(parts, lens, 8, H);

    /* Set session_id (first exchange hash becomes session_id) */
    if (!s->session_id_set) {
        memcpy(s->session_id, H, 32);
        s->session_id_set = 1;
    }

    /* Sign H with host ECDSA key */
    uint8_t sig_blob_buf[256];
    uint32_t sig_blob_len = ecdsa_sig_blob(H, sig_blob_buf, 256);
    if (sig_blob_len == 0) return 0;

    /* Build ECDH_REPLY message:
     * byte SSH_MSG_KEX_ECDH_REPLY
     * string K_S (host key blob)
     * string Q_S (server ECDH public key)
     * string H_sig (signature of H) */
    uint8_t reply[SSH_BUF_SIZE]; struct ssh_buf rb; buf_init(&rb, reply, SSH_BUF_SIZE);
    buf_put_u8(&rb, SSH_MSG_KEX_ECDH_REPLY);
    buf_put_string(&rb, ks_buf, ks_len);
    buf_put_string(&rb, q_s, 65);
    buf_put_string(&rb, sig_blob_buf, sig_blob_len);

    /* Send ECDH_REPLY */
    ssh_send_msg(s, reply, rb.len);

    /* Derive encryption keys */
    derive_key(s->shared_secret, 32, H, s->session_id, 'C', s->enc_recv.key, 16);
    derive_key(s->shared_secret, 32, H, s->session_id, 'D', s->enc_send.key, 16);
    /* Note: IVs for GCM are derived from sequence numbers, not from 'A'/'B' keys.
     * We still derive them per spec but don't use them for GCM. */

    /* Send NEWKEYS */
    uint8_t newkeys_payload[1]; newkeys_payload[0] = SSH_MSG_NEWKEYS;
    ssh_send_msg(s, newkeys_payload, 1);

    /* Activate encryption for sending (we now send encrypted packets).
     * Receiving will be activated when we get client's NEWKEYS. */
    s->enc_send.seq = 0;
    s->state = SSH_STATE_NEWKEYS_PENDING;

    pr_info("ssh: ECDH key exchange complete, NEWKEYS sent\n");
    return 1;
}

/* Process client's NEWKEYS: activate decryption. */
static void process_newkeys(struct ssh_session* s) {
    s->enc_recv.seq = 0;
    s->encryption_active = 1;
    s->state = SSH_STATE_ENCRYPTED;
    pr_info("ssh: encryption active (aes128-gcm@openssh.com)\n");
}

/* Process SERVICE_REQUEST (for "ssh-userauth") */
static void process_service_request(struct ssh_session* s,
                                    const uint8_t* payload, uint32_t len) {
    if (len < 5 || payload[0] != SSH_MSG_SERVICE_REQUEST) return;
    struct ssh_rbuf r; rbuf_init(&r, payload, len);
    rbuf_get_u8(&r); /* skip type */
    char svc[64];
    if (!rbuf_get_cstring(&r, svc, 64)) return;

    if (strcmp(svc, "ssh-userauth") == 0) {
        uint8_t resp[64]; struct ssh_buf b; buf_init(&b, resp, 64);
        buf_put_u8(&b, SSH_MSG_SERVICE_ACCEPT);
        buf_put_cstring(&b, "ssh-userauth");
        ssh_send_msg(s, resp, b.len);
        s->state = SSH_STATE_AUTH_PENDING;
        pr_debug("ssh: service 'ssh-userauth' accepted\n");
    } else {
        ssh_disconnect(s, SSH_DISCONNECT_SERVICE_NOT_AVAILABLE, "Service not available");
    }
}

/* Process USERAUTH_REQUEST for password authentication */
static void process_userauth(struct ssh_session* s,
                             const uint8_t* payload, uint32_t len) {
    if (len < 5 || payload[0] != SSH_MSG_USERAUTH_REQUEST) return;
    struct ssh_rbuf r; rbuf_init(&r, payload, len);
    rbuf_get_u8(&r);

    char username[64], svc[64], method[64];
    if (!rbuf_get_cstring(&r, username, 64)) return;
    if (!rbuf_get_cstring(&r, svc, 64)) return;
    if (!rbuf_get_cstring(&r, method, 64)) return;

    if (strcmp(svc, "ssh-userauth") != 0) {
        /* Wrong service */
        uint8_t fail[64]; struct ssh_buf b; buf_init(&b, fail, 64);
        buf_put_u8(&b, SSH_MSG_USERAUTH_FAILURE);
        buf_put_cstring(&b, "password");
        buf_put_bool(&b, 0); /* no partial success */
        ssh_send_msg(s, fail, b.len);
        return;
    }

    if (strcmp(method, "password") == 0) {
        /* Password auth: boolean(FALSE=change) || string(password) */
        uint8_t change_pw = rbuf_get_u8(&r);
        if (change_pw) {
            /* Password change request - not supported */
            uint8_t fail[64]; struct ssh_buf b; buf_init(&b, fail, 64);
            buf_put_u8(&b, SSH_MSG_USERAUTH_FAILURE);
            buf_put_cstring(&b, "password");
            buf_put_bool(&b, 0);
            ssh_send_msg(s, fail, b.len);
            return;
        }
        char password[128];
        if (!rbuf_get_cstring(&r, password, 128)) return;

        if (ssh_check_password(username, password)) {
            strncpy(s->username, username, 31); s->username[31] = '\0';
            uint8_t ok[16]; ok[0] = SSH_MSG_USERAUTH_SUCCESS;
            ssh_send_msg(s, ok, 1);
            s->state = SSH_STATE_CHANNEL_OPEN;
            pr_info("ssh: user '%s' authenticated\n", username);
        } else {
            uint8_t fail[64]; struct ssh_buf b; buf_init(&b, fail, 64);
            buf_put_u8(&b, SSH_MSG_USERAUTH_FAILURE);
            buf_put_cstring(&b, "password");
            buf_put_bool(&b, 0);
            ssh_send_msg(s, fail, b.len);
            pr_info("ssh: auth failed for '%s'\n", username);
        }
    } else if (strcmp(method, "publickey") == 0) {
        /* Publickey not supported, offer password only */
        uint8_t fail[64]; struct ssh_buf b; buf_init(&b, fail, 64);
        buf_put_u8(&b, SSH_MSG_USERAUTH_FAILURE);
        buf_put_cstring(&b, "password");
        buf_put_bool(&b, 0);
        ssh_send_msg(s, fail, b.len);
    } else {
        /* Unknown method */
        uint8_t fail[64]; struct ssh_buf b; buf_init(&b, fail, 64);
        buf_put_u8(&b, SSH_MSG_USERAUTH_FAILURE);
        buf_put_cstring(&b, "password");
        buf_put_bool(&b, 0);
        ssh_send_msg(s, fail, b.len);
    }
}

/* Process CHANNEL_OPEN for "session" channel */
static void process_channel_open(struct ssh_session* s,
                                 const uint8_t* payload, uint32_t len) {
    if (len < 5 || payload[0] != SSH_MSG_CHANNEL_OPEN) return;
    struct ssh_rbuf r; rbuf_init(&r, payload, len);
    rbuf_get_u8(&r);

    char ch_type[64];
    if (!rbuf_get_cstring(&r, ch_type, 64)) return;
    uint32_t peer_chan = rbuf_get_u32(&r);
    uint32_t init_win = rbuf_get_u32(&r);
    uint32_t max_pkt  = rbuf_get_u32(&r);

    if (strcmp(ch_type, "session") != 0) {
        uint8_t fail[64]; struct ssh_buf b; buf_init(&b, fail, 64);
        buf_put_u8(&b, SSH_MSG_CHANNEL_OPEN_FAILURE);
        buf_put_u32(&b, peer_chan);
        buf_put_u32(&b, SSH_DISCONNECT_SERVICE_NOT_AVAILABLE);
        buf_put_cstring(&b, "Unknown channel type");
        buf_put_cstring(&b, "");
        ssh_send_msg(s, fail, b.len);
        return;
    }

    s->peer_channel = peer_chan;
    s->our_channel = 0; /* We use channel 0 */
    s->window_remote = init_win;
    s->max_pkt_remote = max_pkt;
    s->window_local = SSH_WINDOW_SIZE;
    s->max_pkt_local = SSH_MAX_PKT_SIZE;

    uint8_t confirm[32]; struct ssh_buf b; buf_init(&b, confirm, 32);
    buf_put_u8(&b, SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
    buf_put_u32(&b, s->our_channel);
    buf_put_u32(&b, peer_chan);
    buf_put_u32(&b, SSH_WINDOW_SIZE);
    buf_put_u32(&b, SSH_MAX_PKT_SIZE);
    ssh_send_msg(s, confirm, b.len);
    pr_debug("ssh: session channel opened\n");
}

/* Process CHANNEL_REQUEST (pty-req, shell, exec, etc.) */
static void process_channel_request(struct ssh_session* s,
                                    const uint8_t* payload, uint32_t len) {
    if (len < 5 || payload[0] != SSH_MSG_CHANNEL_REQUEST) return;
    struct ssh_rbuf r; rbuf_init(&r, payload, len);
    rbuf_get_u8(&r);
    uint32_t chan = rbuf_get_u32(&r);
    char req_type[64];
    if (!rbuf_get_cstring(&r, req_type, 64)) return;
    uint8_t want_reply = rbuf_get_u8(&r);

    if (strcmp(req_type, "pty-req") == 0) {
        /* PTY request: string(term), uint32(cols), uint32(rows),
         * uint32(width_px), uint32(height_px), string(mode) */
        if (!rbuf_get_cstring(&r, s->pty_term, sizeof(s->pty_term))) return;
        s->pty_cols = rbuf_get_u32(&r);
        s->pty_rows = rbuf_get_u32(&r);
        s->pty_width_px = rbuf_get_u32(&r);
        s->pty_height_px = rbuf_get_u32(&r);
        /* Skip terminal modes string */
        uint32_t modes_len; rbuf_get_string(&r, &modes_len);
        s->pty_fd = -1; /* No real PTY in kernel, but store params */

        if (want_reply) {
            uint8_t ok[16]; ok[0] = SSH_MSG_CHANNEL_SUCCESS;
            ok[1]=(chan>>24)&0xFF; ok[2]=(chan>>16)&0xFF;
            ok[3]=(chan>>8)&0xFF;  ok[4]=chan&0xFF;
            /* Actually, CHANNEL_SUCCESS uses recipient channel, not sender */
            uint8_t resp[16]; struct ssh_buf b2; buf_init(&b2, resp, 16);
            buf_put_u8(&b2, SSH_MSG_CHANNEL_SUCCESS);
            buf_put_u32(&b2, s->our_channel);
            ssh_send_msg(s, resp, b2.len);
        }
        pr_info("ssh: pty-req: term=%s cols=%u rows=%u\n",
                s->pty_term, s->pty_cols, s->pty_rows);

    } else if (strcmp(req_type, "shell") == 0) {
        /* Shell request: start interactive shell */
        s->state = SSH_STATE_SHELL_RUNNING;

        if (want_reply) {
            uint8_t resp[16]; struct ssh_buf b2; buf_init(&b2, resp, 16);
            buf_put_u8(&b2, SSH_MSG_CHANNEL_SUCCESS);
            buf_put_u32(&b2, s->our_channel);
            ssh_send_msg(s, resp, b2.len);
        }

        /* Send welcome banner and first prompt via channel data */
        const char* welcome = "\r\nLestraOS Shell (lsh) - type 'help' for commands\r\n> ";
        uint8_t data_pkt[256]; struct ssh_buf db; buf_init(&db, data_pkt, 256);
        buf_put_u8(&db, SSH_MSG_CHANNEL_DATA);
        buf_put_u32(&db, s->our_channel);
        buf_put_cstring(&db, welcome);
        ssh_send_msg(s, data_pkt, db.len);
        pr_info("ssh: shell started for '%s'\n", s->username);

    } else if (strcmp(req_type, "exec") == 0) {
        /* Exec request: string(command) */
        char command[512];
        if (!rbuf_get_cstring(&r, command, 512)) return;
        s->state = SSH_STATE_SHELL_RUNNING;

        if (want_reply) {
            uint8_t resp[16]; struct ssh_buf b2; buf_init(&b2, resp, 16);
            buf_put_u8(&b2, SSH_MSG_CHANNEL_SUCCESS);
            buf_put_u32(&b2, s->our_channel);
            ssh_send_msg(s, resp, b2.len);
        }

        /* Execute command and send output */
        ssh_execute_and_send(s, command);
        /* Send EOF and close */
        uint8_t eof_pkt[16]; eof_pkt[0] = SSH_MSG_CHANNEL_EOF;
        eof_pkt[1]=(s->our_channel>>24)&0xFF; eof_pkt[2]=(s->our_channel>>16)&0xFF;
        eof_pkt[3]=(s->our_channel>>8)&0xFF;  eof_pkt[4]=s->our_channel&0xFF;
        ssh_send_msg(s, eof_pkt, 5);
        /* Send exit-status */
        uint8_t exit_pkt[16]; struct ssh_buf xb; buf_init(&xb, exit_pkt, 16);
        buf_put_u8(&xb, SSH_MSG_CHANNEL_REQUEST);
        buf_put_u32(&xb, s->our_channel);
        buf_put_cstring(&xb, "exit-status");
        buf_put_bool(&xb, 0);
        buf_put_u32(&xb, 0); /* exit code 0 */
        ssh_send_msg(s, exit_pkt, xb.len);
        /* Close channel */
        uint8_t close_pkt[16]; close_pkt[0]=SSH_MSG_CHANNEL_CLOSE;
        close_pkt[1]=(s->our_channel>>24)&0xFF; close_pkt[2]=(s->our_channel>>16)&0xFF;
        close_pkt[3]=(s->our_channel>>8)&0xFF;  close_pkt[4]=s->our_channel&0xFF;
        ssh_send_msg(s, close_pkt, 5);
        s->state = SSH_STATE_CHANNEL_OPEN;
        pr_info("ssh: exec '%s' completed\n", command);

    } else if (strcmp(req_type, "window-change") == 0) {
        /* Window change: uint32(cols), uint32(rows), uint32(width_px), uint32(height_px) */
        s->pty_cols = rbuf_get_u32(&r);
        s->pty_rows = rbuf_get_u32(&r);
        s->pty_width_px = rbuf_get_u32(&r);
        s->pty_height_px = rbuf_get_u32(&r);
        /* No reply for window-change */
        pr_debug("ssh: window-change: cols=%u rows=%u\n", s->pty_cols, s->pty_rows);

    } else if (strcmp(req_type, "env") == 0) {
        /* Environment variable: string(name), string(value) - ignore */
        char env_name[128], env_val[256];
        rbuf_get_cstring(&r, env_name, 128);
        rbuf_get_cstring(&r, env_val, 256);
        if (want_reply) {
            uint8_t resp[16]; struct ssh_buf b2; buf_init(&b2, resp, 16);
            buf_put_u8(&b2, SSH_MSG_CHANNEL_SUCCESS);
            buf_put_u32(&b2, s->our_channel);
            ssh_send_msg(s, resp, b2.len);
        }

    } else if (strcmp(req_type, "signal") == 0) {
        /* Signal: string(signal_name) - ignore */
        char sig[32]; rbuf_get_cstring(&r, sig, 32);
        /* No reply expected for signal */

    } else {
        /* Unknown request */
        pr_debug("ssh: unknown channel request: '%s'\n", req_type);
        if (want_reply) {
            uint8_t resp[16]; struct ssh_buf b2; buf_init(&b2, resp, 16);
            buf_put_u8(&b2, SSH_MSG_CHANNEL_FAILURE);
            buf_put_u32(&b2, s->our_channel);
            ssh_send_msg(s, resp, b2.len);
        }
    }
}

/* Process CHANNEL_DATA: shell input from client */
static void process_channel_data(struct ssh_session* s,
                                 const uint8_t* payload, uint32_t len) {
    if (len < 9 || payload[0] != SSH_MSG_CHANNEL_DATA) return;
    struct ssh_rbuf r; rbuf_init(&r, payload, len);
    rbuf_get_u8(&r); /* type */
    uint32_t chan = rbuf_get_u32(&r);
    uint32_t data_len; const uint8_t* data = rbuf_get_string(&r, &data_len);
    if (!data) return;

    /* We only handle data on our channel */
    if (chan != s->our_channel) return;

    /* Process input data line by line */
    for (uint32_t i = 0; i < data_len; i++) {
        uint8_t ch = data[i];

        /* Handle special characters */
        if (ch == '\n' || ch == '\r') {
            /* Execute the accumulated command line */
            if (s->line_len > 0) {
                s->line_buf[s->line_len] = '\0';
                if (strcmp(s->line_buf, "exit") == 0) {
                    /* Close the session gracefully */
                    uint8_t eof_pkt[16];
                    eof_pkt[0]=SSH_MSG_CHANNEL_EOF;
                    eof_pkt[1]=(s->our_channel>>24)&0xFF;
                    eof_pkt[2]=(s->our_channel>>16)&0xFF;
                    eof_pkt[3]=(s->our_channel>>8)&0xFF;
                    eof_pkt[4]=s->our_channel&0xFF;
                    ssh_send_msg(s, eof_pkt, 5);
                    uint8_t close_pkt[16];
                    close_pkt[0]=SSH_MSG_CHANNEL_CLOSE;
                    close_pkt[1]=(s->our_channel>>24)&0xFF;
                    close_pkt[2]=(s->our_channel>>16)&0xFF;
                    close_pkt[3]=(s->our_channel>>8)&0xFF;
                    close_pkt[4]=s->our_channel&0xFF;
                    ssh_send_msg(s, close_pkt, 5);
                    s->in_use = 0;
                    pr_info("ssh: session closed (user '%s' exit)\n", s->username);
                    return;
                }
                ssh_execute_and_send(s, s->line_buf);
                s->line_len = 0;
                /* Send prompt */
                const char* prompt = "\r\n> ";
                uint8_t data_pkt[64]; struct ssh_buf db; buf_init(&db, data_pkt, 64);
                buf_put_u8(&db, SSH_MSG_CHANNEL_DATA);
                buf_put_u32(&db, s->our_channel);
                buf_put_cstring(&db, prompt);
                ssh_send_msg(s, data_pkt, db.len);
            }
        } else if (ch == 127 || ch == 8) {
            /* Backspace */
            if (s->line_len > 0) s->line_len--;
        } else if (ch == 3) {
            /* Ctrl-C: clear line */
            s->line_len = 0;
            const char* ctrl_c_resp = "\r\n> ";
            uint8_t data_pkt[64]; struct ssh_buf db; buf_init(&db, data_pkt, 64);
            buf_put_u8(&db, SSH_MSG_CHANNEL_DATA);
            buf_put_u32(&db, s->our_channel);
            buf_put_cstring(&db, ctrl_c_resp);
            ssh_send_msg(s, data_pkt, db.len);
        } else if (ch >= 32 && ch < 127) {
            /* Printable character */
            if (s->line_len < sizeof(s->line_buf) - 1)
                s->line_buf[s->line_len++] = ch;
        }
        /* Ignore other control characters */
    }
}

/* Handle CHANNEL_WINDOW_ADJUST */
static void process_window_adjust(struct ssh_session* s,
                                  const uint8_t* payload, uint32_t len) {
    if (len < 9 || payload[0] != SSH_MSG_CHANNEL_WINDOW_ADJUST) return;
    struct ssh_rbuf r; rbuf_init(&r, payload, len);
    rbuf_get_u8(&r);
    uint32_t chan = rbuf_get_u32(&r);
    uint32_t add = rbuf_get_u32(&r);
    if (chan == s->our_channel) s->window_remote += add;
}

/* Handle CHANNEL_CLOSE */
static void process_channel_close(struct ssh_session* s,
                                  const uint8_t* payload, uint32_t len) {
    if (len < 5 || payload[0] != SSH_MSG_CHANNEL_CLOSE) return;
    struct ssh_rbuf r; rbuf_init(&r, payload, len);
    rbuf_get_u8(&r);
    uint32_t chan = rbuf_get_u32(&r);
    if (chan == s->our_channel) {
        /* Send our CLOSE back */
        uint8_t close_pkt[16];
        close_pkt[0]=SSH_MSG_CHANNEL_CLOSE;
        close_pkt[1]=(s->our_channel>>24)&0xFF;
        close_pkt[2]=(s->our_channel>>16)&0xFF;
        close_pkt[3]=(s->our_channel>>8)&0xFF;
        close_pkt[4]=s->our_channel&0xFF;
        ssh_send_msg(s, close_pkt, 5);
        s->in_use = 0;
        pr_info("ssh: channel closed, session ending\n");
    }
}

/* Handle CHANNEL_EOF */
static void process_channel_eof(struct ssh_session* s,
                                const uint8_t* payload, uint32_t len) {
    if (len < 5 || payload[0] != SSH_MSG_CHANNEL_EOF) return;
    struct ssh_rbuf r; rbuf_init(&r, payload, len);
    rbuf_get_u8(&r);
    uint32_t chan = rbuf_get_u32(&r);
    if (chan == s->our_channel) {
        /* Client's side is done sending, we can still send data */
    }
}

/* Handle GLOBAL_REQUEST (e.g. no-more-sessions) */
static void process_global_request(struct ssh_session* s,
                                   const uint8_t* payload, uint32_t len) {
    if (len < 5 || payload[0] != SSH_MSG_GLOBAL_REQUEST) return;
    struct ssh_rbuf r; rbuf_init(&r, payload, len);
    rbuf_get_u8(&r);
    char req[64]; rbuf_get_cstring(&r, req, 64);
    uint8_t want_reply = rbuf_get_u8(&r);

    /* We deny all global requests */
    if (want_reply) {
        uint8_t deny[16]; deny[0] = SSH_MSG_REQUEST_FAILURE;
        ssh_send_msg(s, deny, 1);
    }
}

/* ===== Shell Output ===== */
#define SSH_OUTPUT_BUF 4096
static char ssh_out_buf[SSH_OUTPUT_BUF];
static int ssh_out_len;

static void ssh_output_cb(char c) {
    if (ssh_out_len < SSH_OUTPUT_BUF - 1) ssh_out_buf[ssh_out_len++] = c;
}

static void ssh_execute_and_send(struct ssh_session* s, const char* line) {
    ssh_out_len = 0;
    shell_execute_line(line, ssh_output_cb);

    if (ssh_out_len > 0) {
        /* Send output as channel data, chunked if large */
        uint32_t offset = 0;
        while (offset < ssh_out_len) {
            uint32_t chunk = ssh_out_len - offset;
            /* Limit chunk size to respect client's max packet and our window */
            if (chunk > s->max_pkt_remote - 32) chunk = s->max_pkt_remote - 32;
            if (chunk > s->window_remote) chunk = s->window_remote;
            if (chunk > 4096) chunk = 4096;

            uint8_t data_pkt[4100]; struct ssh_buf db; buf_init(&db, data_pkt, sizeof(data_pkt));
            buf_put_u8(&db, SSH_MSG_CHANNEL_DATA);
            buf_put_u32(&db, s->our_channel);
            buf_put_string(&db, (const uint8_t*)(ssh_out_buf + offset), chunk);
            ssh_send_msg(s, data_pkt, db.len);
            offset += chunk;
            s->window_remote -= chunk;
        }
    }
}

/* ===== Main Packet Dispatch ===== */
static void ssh_process_packet(struct ssh_session* s,
                               const uint8_t* payload, uint32_t len) {
    if (len < 1) return;
    uint8_t msg_type = payload[0];

    switch (msg_type) {
    case SSH_MSG_DISCONNECT:
        pr_info("ssh: client disconnected\n");
        s->in_use = 0;
        break;
    case SSH_MSG_IGNORE:
        break; /* no-op */
    case SSH_MSG_UNIMPLEMENTED:
        break; /* client says we sent an unimplemented msg */
    case SSH_MSG_DEBUG:
        break; /* ignore debug messages */
    case SSH_MSG_KEX_INIT:
        if (s->state == SSH_STATE_KEX_INIT_SENT ||
            s->state == SSH_STATE_VERSION_SENT) {
            if (!process_kexinit(s, payload, len)) {
                ssh_disconnect(s, SSH_DISCONNECT_KEY_EXCHANGE_FAILED,
                              "Algorithm negotiation failed");
            }
        }
        break;
    case SSH_MSG_KEX_ECDH_INIT:
        if (s->state == SSH_STATE_KEX_INIT_SENT) {
            if (!process_ecdh_init(s, payload, len)) {
                ssh_disconnect(s, SSH_DISCONNECT_KEY_EXCHANGE_FAILED,
                              "ECDH key exchange failed");
            }
        }
        break;
    case SSH_MSG_NEWKEYS:
        if (s->state == SSH_STATE_NEWKEYS_PENDING) {
            process_newkeys(s);
        }
        break;
    case SSH_MSG_SERVICE_REQUEST:
        if (s->state == SSH_STATE_ENCRYPTED) {
            process_service_request(s, payload, len);
        }
        break;
    case SSH_MSG_USERAUTH_REQUEST:
        if (s->state == SSH_STATE_ENCRYPTED ||
            s->state == SSH_STATE_AUTH_PENDING) {
            process_userauth(s, payload, len);
        }
        break;
    case SSH_MSG_GLOBAL_REQUEST:
        process_global_request(s, payload, len);
        break;
    case SSH_MSG_CHANNEL_OPEN:
        if (s->state == SSH_STATE_CHANNEL_OPEN) {
            process_channel_open(s, payload, len);
        }
        break;
    case SSH_MSG_CHANNEL_WINDOW_ADJUST:
        process_window_adjust(s, payload, len);
        break;
    case SSH_MSG_CHANNEL_DATA:
        if (s->state == SSH_STATE_SHELL_RUNNING) {
            process_channel_data(s, payload, len);
        }
        break;
    case SSH_MSG_CHANNEL_EXTENDED_DATA:
        /* Extended data (stderr) - ignore for now */
        break;
    case SSH_MSG_CHANNEL_EOF:
        process_channel_eof(s, payload, len);
        break;
    case SSH_MSG_CHANNEL_CLOSE:
        process_channel_close(s, payload, len);
        break;
    case SSH_MSG_CHANNEL_REQUEST:
        if (s->state == SSH_STATE_CHANNEL_OPEN ||
            s->state == SSH_STATE_SHELL_RUNNING) {
            process_channel_request(s, payload, len);
        }
        break;
    default:
        pr_debug("ssh: unhandled message type %u\n", msg_type);
        /* Send SSH_MSG_UNIMPLEMENTED */
        {
            uint8_t unimp[12]; struct ssh_buf b; buf_init(&b, unimp, 12);
            buf_put_u8(&b, SSH_MSG_UNIMPLEMENTED);
            buf_put_u32(&b, s->enc_recv.seq - 1); /* seq of the received packet */
            ssh_send_msg(s, unimp, b.len);
        }
        break;
    }
}

/* ===== Server Tick ===== */
void ssh_server_tick(void) {
    if (!ssh_running || listen_idx < 0) return;

    /* Check for new incoming connections */
    struct tcp_conn* new_conn = NULL;
    if (tcp_accept(listen_idx, &new_conn) == 0 && new_conn) {
        int slot = ssh_alloc_session();
        if (slot < 0) {
            pr_warn("ssh: no free session slots, rejecting\n");
            tcp_close_conn(new_conn);
        } else {
            memset(&sessions[slot], 0, sizeof(sessions[slot]));
            sessions[slot].in_use = 1;
            sessions[slot].conn_idx = (int)(new_conn - tcp_get_conn(0));
            sessions[slot].last_activity = timer_get_ms();
            sessions[slot].state = SSH_STATE_INIT;
            sessions[slot].window_local = SSH_WINDOW_SIZE;
            sessions[slot].max_pkt_local = SSH_MAX_PKT_SIZE;
            sessions[slot].encryption_active = 0;
            sessions[slot].session_id_set = 0;
            sessions[slot].line_len = 0;

            pr_info("ssh: new connection from %u.%u.%u.%u:%u (session %d)\n",
                    new_conn->peer_ip.bytes[0], new_conn->peer_ip.bytes[1],
                    new_conn->peer_ip.bytes[2], new_conn->peer_ip.bytes[3],
                    (unsigned)new_conn->peer_port, slot);

            /* Send our version string */
            const char* ver = SSH_VERSION_STRING "\r\n";
            ssh_send_raw(&sessions[slot], (const uint8_t*)ver, (uint32_t)strlen(ver));
            sessions[slot].state = SSH_STATE_VERSION_SENT;
        }
    }

    /* Service each active session */
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        if (!sessions[i].in_use) continue;
        struct ssh_session* s = &sessions[i];

        struct tcp_conn* c = tcp_get_conn(s->conn_idx);
        if (!c || c->state != TCP_ESTABLISHED) {
            pr_info("ssh: session %d connection lost\n", i);
            s->in_use = 0;
            continue;
        }

        /* Check idle timeout */
        if (timer_get_ms() - s->last_activity > SSH_TIMEOUT_MS) {
            pr_info("ssh: session %d idle timeout\n", i);
            ssh_disconnect(s, SSH_DISCONNECT_BY_APPLICATION, "Idle timeout");
            continue;
        }

        /* Read available data from TCP into session rx buffer */
        if (c->rx_len > 0) {
            uint32_t space = SSH_BUF_SIZE - s->rx_len;
            uint32_t to_read = c->rx_len;
            if (to_read > space) to_read = space;
            if (to_read > 0) {
                memcpy(s->rx_buf + s->rx_len, c->rx_buf, to_read);
                memmove(c->rx_buf, c->rx_buf + to_read, c->rx_len - to_read);
                c->rx_len -= (uint16_t)to_read;
                s->rx_len += (uint16_t)to_read;
            }
            s->last_activity = timer_get_ms();
        }

        /* Process data based on current state */
        if (s->state == SSH_STATE_VERSION_SENT) {
            /* Version exchange phase: look for client version string line */
            int consumed = process_version(s);
            if (consumed < 0) {
                ssh_disconnect(s, SSH_DISCONNECT_PROTOCOL_VERSION_NOT_SUPPORTED,
                              "Invalid client version");
                continue;
            }
            if (consumed > 0) {
                /* Remove consumed bytes from rx_buf */
                memmove(s->rx_buf, s->rx_buf + consumed, s->rx_len - consumed);
                s->rx_len -= consumed;
                /* Version exchange complete, send KEXINIT */
                s->state = SSH_STATE_KEX_INIT_SENT;
                send_kexinit(s);
            }
        }

        /* After version exchange, process binary packets */
        if (s->state >= SSH_STATE_KEX_INIT_SENT) {
            /* Try to extract and process one or more SSH packets */
            while (s->in_use && s->rx_len > 0) {
                uint8_t payload[SSH_BUF_SIZE];
                uint32_t payload_len;
                int consumed;

                if (s->encryption_active) {
                    consumed = ssh_recv_enc_packet(s, payload, &payload_len);
                } else {
                    consumed = ssh_recv_packet(s, payload, &payload_len);
                }

                if (consumed == 0) break; /* incomplete, wait for more data */
                if (consumed < 0) {
                    /* Error (auth failure, invalid packet) */
                    ssh_disconnect(s, SSH_DISCONNECT_MAC_ERROR,
                                  "Packet authentication failure");
                    break;
                }

                /* Remove consumed bytes from rx_buf */
                memmove(s->rx_buf, s->rx_buf + consumed, s->rx_len - consumed);
                s->rx_len -= consumed;

                /* Dispatch the packet */
                ssh_process_packet(s, payload, payload_len);
            }
        }
    }
}
