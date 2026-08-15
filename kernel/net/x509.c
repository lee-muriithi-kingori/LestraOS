/*
 * Lestra OS - X.509 Certificate Parser
 * Copyright (c) 2026 lestramk.org
 *
 * DER-encoded X.509 v3 certificate parsing with SHA-256
 * hashing for signature verification. ASN.1 helpers for
 * SEQUENCE, INTEGER, OID, SET, and UTF8STRING tags.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <string.h>

/* ===== SHA-256 (duplicated from tls.c for self-containment) ===== */

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

/* ===== ASN.1 DER parsing helpers ===== */

struct asn1_cursor {
    const uint8_t* buf;
    uint32_t len;
    uint32_t pos;
};

static int asn1_read_tag(struct asn1_cursor* c, uint8_t* tag) {
    if (c->pos >= c->len) return -1;
    *tag = c->buf[c->pos++];
    return 0;
}

static int asn1_read_length(struct asn1_cursor* c, uint32_t* out_len) {
    if (c->pos >= c->len) return -1;
    uint8_t b0 = c->buf[c->pos++];
    if (b0 < 0x80) {
        *out_len = b0;
    } else if (b0 == 0x81) {
        if (c->pos >= c->len) return -1;
        *out_len = c->buf[c->pos++];
    } else if (b0 == 0x82) {
        if (c->pos + 2 > c->len) return -1;
        *out_len = ((uint32_t)c->buf[c->pos] << 8) | c->buf[c->pos+1];
        c->pos += 2;
    } else {
        return -1;
    }
    if (c->pos + *out_len > c->len) return -1;
    return 0;
}

static int asn1_skip(struct asn1_cursor* c) {
    uint8_t tag;
    uint32_t len;
    if (asn1_read_tag(c, &tag) < 0) return -1;
    if (asn1_read_length(c, &len) < 0) return -1;
    c->pos += len;
    return 0;
}

static int asn1_enter(struct asn1_cursor* c, uint8_t expected_tag) {
    uint8_t tag;
    uint32_t len;
    if (asn1_read_tag(c, &tag) < 0) return -1;
    if (tag != expected_tag) return -1;
    if (asn1_read_length(c, &len) < 0) return -1;
    c->buf += (c->pos - (len + 2));
    c->len = len;
    c->pos = 0;
    return 0;
}

/* ===== X.509 certificate structures ===== */

#define X509_MAX_NAME_LEN   128
#define X509_MAX_CERT_SIZE  4096

struct x509_name {
    char cn[X509_MAX_NAME_LEN];
};

struct x509_cert {
    struct x509_name subject;
    struct x509_name issuer;
    uint8_t serial[20];
    uint8_t tbs_hash[32];
    uint8_t signature[256];
    uint8_t pubkey_n[256];
    uint32_t pubkey_e;
};

static void x509_parse_printable_string(struct asn1_cursor* c, char* out, uint32_t max) {
    uint8_t tag;
    uint32_t len;
    if (asn1_read_tag(c, &tag) < 0) return;
    if (asn1_read_length(c, &len) < 0) return;
    if (len >= max) len = max - 1;
    memcpy(out, &c->buf[c->pos], len);
    out[len] = '\0';
    c->pos += len;
}

static void x509_parse_utf8_string(struct asn1_cursor* c, char* out, uint32_t max) {
    uint8_t tag;
    uint32_t len;
    if (asn1_read_tag(c, &tag) < 0) return;
    if (asn1_read_length(c, &len) < 0) return;
    if (len >= max) len = max - 1;
    memcpy(out, &c->buf[c->pos], len);
    out[len] = '\0';
    c->pos += len;
}

static int x509_parse_rdn(struct asn1_cursor* c, struct x509_name* name) {
    uint8_t tag;
    uint32_t len;
    if (asn1_read_tag(c, &tag) < 0) return -1;
    if (tag != 0x31) return -1;
    if (asn1_read_length(c, &len) < 0) return -1;

    uint32_t rdn_end = c->pos + len;
    while (c->pos < rdn_end) {
        if (asn1_read_tag(c, &tag) < 0) return -1;
        if (tag != 0x30) return -1;
        if (asn1_read_length(c, &len) < 0) return -1;

        uint32_t attr_end = c->pos + len;

        /* OID */
        if (asn1_read_tag(c, &tag) < 0) return -1;
        if (tag != 0x06) return -1;
        if (asn1_read_length(c, &len) < 0) return -1;

        int is_cn = (len == 3 && c->buf[c->pos] == 0x55 &&
                     c->buf[c->pos+1] == 0x04 && c->buf[c->pos+2] == 0x03);
        c->pos += len;

        /* Value */
        if (c->pos >= attr_end) continue;
        if (asn1_read_tag(c, &tag) < 0) return -1;
        if (asn1_read_length(c, &len) < 0) return -1;

        if (is_cn) {
            if (tag == 0x13)
                x509_parse_printable_string(c, name->cn, X509_MAX_NAME_LEN);
            else if (tag == 0x0C)
                x509_parse_utf8_string(c, name->cn, X509_MAX_NAME_LEN);
            else
                c->pos += len;
        } else {
            c->pos += len;
        }

        c->pos = attr_end;
    }
    return 0;
}

static int x509_parse_name(struct asn1_cursor* c, struct x509_name* name) {
    uint8_t tag;
    uint32_t len;
    if (asn1_read_tag(c, &tag) < 0) return -1;
    if (tag != 0x30) return -1;
    if (asn1_read_length(c, &len) < 0) return -1;

    uint32_t seq_end = c->pos + len;
    while (c->pos < seq_end) {
        if (x509_parse_rdn(c, name) < 0) return -1;
    }
    return 0;
}

static int x509_parse_algorithm(struct asn1_cursor* c) {
    uint8_t tag;
    uint32_t len;
    if (asn1_read_tag(c, &tag) < 0) return -1;
    if (tag != 0x30) return -1;
    if (asn1_read_length(c, &len) < 0) return -1;
    c->pos += len;
    return 0;
}

static int x509_parse_bit_string(struct asn1_cursor* c, uint8_t* out, uint32_t max, uint32_t* out_len) {
    uint8_t tag;
    uint32_t len;
    if (asn1_read_tag(c, &tag) < 0) return -1;
    if (tag != 0x03) return -1;
    if (asn1_read_length(c, &len) < 0) return -1;
    if (len < 1) return -1;
    uint8_t unused = c->buf[c->pos++];
    len--;
    (void)unused;
    if (len > max) len = max;
    memcpy(out, &c->buf[c->pos], len);
    *out_len = len;
    c->pos += len;
    return 0;
}

static int x509_parse_integer(struct asn1_cursor* c, uint8_t* out, uint32_t max) {
    uint8_t tag;
    uint32_t len;
    if (asn1_read_tag(c, &tag) < 0) return -1;
    if (tag != 0x02) return -1;
    if (asn1_read_length(c, &len) < 0) return -1;
    /* Skip leading zero byte (two's complement) */
    if (len > 0 && c->buf[c->pos] == 0x00) { c->pos++; len--; }
    if (len > max) len = max;
    memcpy(out, &c->buf[c->pos], len);
    c->pos += len;
    return 0;
}

/* Parse RSA public key from SubjectPublicKeyInfo */
static int x509_parse_rsa_pubkey(struct asn1_cursor* c, uint8_t n[256], uint32_t* e) {
    uint8_t tag;
    uint32_t len;

    /* SubjectPublicKeyInfo ::= SEQUENCE { algorithm AlgorithmIdentifier, subjectPublicKey BIT STRING } */
    if (asn1_read_tag(c, &tag) < 0) return -1;
    if (tag != 0x30) return -1;
    if (asn1_read_length(c, &len) < 0) return -1;

    uint32_t spki_end = c->pos + len;

    /* AlgorithmIdentifier */
    if (x509_parse_algorithm(c) < 0) return -1;

    /* BIT STRING containing RSAPublicKey */
    uint8_t bs[512];
    uint32_t bs_len;
    if (x509_parse_bit_string(c, bs, 512, &bs_len) < 0) return -1;

    /* RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER } */
    struct asn1_cursor pk = { bs, bs_len, 0 };
    if (asn1_read_tag(&pk, &tag) < 0) return -1;
    if (tag != 0x30) return -1;
    if (asn1_read_length(&pk, &len) < 0) return -1;

    /* modulus */
    uint8_t modulus[256];
    memset(modulus, 0, 256);
    uint8_t mod_raw[256];
    uint32_t mod_len = 0;

    if (asn1_read_tag(&pk, &tag) < 0) return -1;
    if (tag != 0x02) return -1;
    if (asn1_read_length(&pk, &len) < 0) return -1;
    /* Handle leading zero (two's complement positive) */
    uint32_t start = 0;
    if (len > 0 && pk.buf[pk.pos] == 0x00) { start = 1; len--; }
    if (len > 256) len = 256;
    memcpy(mod_raw, &pk.buf[pk.pos + start], len);
    mod_len = len;
    /* Copy into 256-byte big-endian (right-aligned) */
    memset(n, 0, 256);
    if (mod_len <= 256)
        memcpy(n + (256 - mod_len), mod_raw, mod_len);
    pk.pos += start + len;

    /* publicExponent */
    uint8_t exp_raw[8];
    uint32_t exp_len = 0;
    if (asn1_read_tag(&pk, &tag) < 0) return -1;
    if (tag != 0x02) return -1;
    if (asn1_read_length(&pk, &len) < 0) return -1;
    if (len > 8) len = 8;
    memcpy(exp_raw, &pk.buf[pk.pos], len);
    exp_len = len;
    pk.pos += len;

    *e = 0;
    for (uint32_t i = 0; i < exp_len; i++)
        *e = (*e << 8) | exp_raw[i];

    c->pos = spki_end;
    return 0;
}

/* ===== Public API ===== */

/* Parse a DER-encoded X.509 certificate.
 * Returns 0 on success, -1 on error. */
int x509_parse(const uint8_t* der, uint32_t der_len, struct x509_cert* cert) {
    memset(cert, 0, sizeof(*cert));
    cert->pubkey_e = 65537;

    struct asn1_cursor c = { der, der_len, 0 };
    uint8_t tag;
    uint32_t len;

    /* Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signatureValue } */
    if (asn1_read_tag(&c, &tag) < 0) return -1;
    if (tag != 0x30) return -1;
    if (asn1_read_length(&c, &len) < 0) return -1;

    uint32_t cert_end = c.pos + len;

    /* TBSCertificate */
    uint32_t tbs_start = c.pos;
    if (asn1_read_tag(&c, &tag) < 0) return -1;
    if (tag != 0x30) return -1;
    if (asn1_read_length(&c, &len) < 0) return -1;
    uint32_t tbs_end = c.pos + len;

    /* TBS: version [0] EXPLICIT, serialNumber, signature, issuer, validity, subject, subjectPublicKeyInfo */
    /* version */
    if (asn1_read_tag(&c, &tag) < 0) return -1;
    if (tag == 0xA0) {
        if (asn1_read_length(&c, &len) < 0) return -1;
        c.pos += len;
    }

    /* serialNumber */
    x509_parse_integer(&c, cert->serial, 20);

    /* signature algorithm (skip) */
    x509_parse_algorithm(&c);

    /* issuer */
    x509_parse_name(&c, &cert->issuer);

    /* validity (skip) */
    if (asn1_read_tag(&c, &tag) < 0) return -1;
    if (tag == 0x30) {
        if (asn1_read_length(&c, &len) < 0) return -1;
        c.pos += len;
    }

    /* subject */
    x509_parse_name(&c, &cert->subject);

    /* subjectPublicKeyInfo */
    x509_parse_rsa_pubkey(&c, cert->pubkey_n, &cert->pubkey_e);

    /* Hash TBS for signature verification */
    sha256_hash(&der[tbs_start], tbs_end - tbs_start, cert->tbs_hash);

    c.pos = tbs_end;

    /* signatureAlgorithm (skip) */
    x509_parse_algorithm(&c);

    /* signatureValue BIT STRING */
    uint8_t sig_bits[256];
    uint32_t sig_len;
    if (x509_parse_bit_string(&c, sig_bits, 256, &sig_len) < 0) return -1;
    memset(cert->signature, 0, 256);
    if (sig_len <= 256)
        memcpy(cert->signature + (256 - sig_len), sig_bits, sig_len);

    return 0;
}

/* Find the issuer of child in certs[].
 * Returns index or -1 if not found. */
int x509_find_issuer(const struct x509_cert* child,
                     const struct x509_cert* certs, int count) {
    for (int i = 0; i < count; i++) {
        if (strcmp(child->issuer.cn, certs[i].subject.cn) == 0)
            return i;
    }
    return -1;
}

/* Verify a certificate chain. certs[0] is the end-entity,
 * certs[1] is its issuer, etc. The last cert must be a
 * self-signed root. Returns 0 on success.
 * verify_sig(child_hash, child_sig, issuer_n, issuer_e) is
 * provided externally (calls into rsa.c). */
int x509_verify_cert(const struct x509_cert* certs, int count,
                     int (*verify_sig)(const uint8_t*, const uint8_t*,
                                       const uint8_t*, uint32_t)) {
    if (count < 1) return -1;

    for (int i = 0; i < count - 1; i++) {
        if (strcmp(certs[i].issuer.cn, certs[i+1].subject.cn) != 0) {
            pr_warn("x509: chain break at depth %d: issuer='%s' != subject='%s'\n",
                    i, certs[i].issuer.cn, certs[i+1].subject.cn);
            return -1;
        }
        if (verify_sig(certs[i].tbs_hash, certs[i].signature,
                       certs[i+1].pubkey_n, certs[i+1].pubkey_e) != 1) {
            pr_warn("x509: signature verification failed at depth %d\n", i);
            return -1;
        }
    }

    /* Self-signed root: issuer == subject, verify self-signature */
    const struct x509_cert* root = &certs[count - 1];
    if (strcmp(root->issuer.cn, root->subject.cn) != 0) {
        pr_warn("x509: top cert not self-signed\n");
        return -1;
    }
    if (verify_sig(root->tbs_hash, root->signature,
                   root->pubkey_n, root->pubkey_e) != 1) {
        pr_warn("x509: root self-signature failed\n");
        return -1;
    }

    return 0;
}
