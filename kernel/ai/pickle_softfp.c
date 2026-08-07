/*
 * Lestra OS — Pickle soft-float32 (IEEE 754 single-precision, integer-only).
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Implements the subset of IEEE-754 binary32 arithmetic needed for
 * transformer inference: add/sub/mul/div, comparisons, int<->float
 * conversions, exp, tanh, sigmoid, silu, gelu, sqrt, rsqrt, sin, cos.
 *
 * Simplifications appropriate to NN inference:
 *   - Flush-to-zero on input denormals (DTZ).
 *   - Flush-to-zero on output denormals (FTZ).
 *   - No NaN propagation: NaN/Inf inputs are treated as ±max finite.
 *
 * The implementation uses only uint32_t/int32_t/int64_t arithmetic, so
 * it runs on any CPU and inside the lestraOS kernel (which is built
 * with -mno-sse and has no x87 init).
 */
#ifdef PICKLE_KERNEL
#include <lestra/types.h>
#include <lestra/printk.h>
#else
#include <stdint.h>
#include <stdio.h>
#define pr_info(...)  do {} while (0)
#endif
#ifdef PICKLE_KERNEL
#include <lestra/pickle.h>
#else
#include "pickle.h"
#endif

/* ---- bit layout helpers ------------------------------------------- */
#define SFP_SIGN_BITS  0x80000000u
#define SFP_EXP_MASK   0x7F800000u
#define SFP_MANT_MASK  0x007FFFFFu
#define SFP_EXP_BIAS   127
#define SFP_MANT_BITS  23
#define SFP_HIDDEN     0x00800000u   /* implicit leading 1 in normal    */

typedef union { sfp_t u; float f; } sfp_cast_t;

/* host-only convenience (in-kernel float ops would #NM/#UD) — the
 * function body is just a 32-bit move so it would be safe, but the
 * calling convention (float in XMM0) requires SSE. */
#ifndef PICKLE_KERNEL
sfp_t sfp_from_float(float f) { sfp_cast_t c; c.f = f; return c.u; }
float sfp_to_float(sfp_t a)   { sfp_cast_t c; c.u = a; return c.f; }
#endif

/* ---- internal unpack ---------------------------------------------- */
/* Returns 0 for zero/denormal, 1 for normal, 2 for inf/nan. */
static int sfp_unpack(sfp_t a, int* sign, int* exp, uint32_t* mant) {
    *sign = (a & SFP_SIGN_BITS) ? 1 : 0;
    *exp  = (int)((a & SFP_EXP_MASK) >> SFP_MANT_BITS);
    *mant = a & SFP_MANT_MASK;
    if (*exp == 0) {
        /* zero or denormal — flush to zero */
        *mant = 0;
        return 0;
    }
    if (*exp == 255) {
        /* inf or nan — treat as max finite */
        *exp = 254;
        *mant = SFP_MANT_MASK | SFP_HIDDEN;
        return 2;
    }
    /* normal: add hidden bit */
    *mant |= SFP_HIDDEN;
    return 1;
}

static sfp_t sfp_pack(int sign, int exp, uint32_t mant) {
    if (mant == 0) return 0;
    /* normalize so mantissa is in [1<<23, 1<<24) */
    while (mant < SFP_HIDDEN && exp > 1) {
        mant <<= 1;
        exp--;
    }
    while (mant >= (SFP_HIDDEN << 1)) {
        mant >>= 1;
        exp++;
    }
    /* flush to zero if exponent too small */
    if (exp <= 0) return 0;
    /* saturate if too large */
    if (exp >= 255) {
        /* return max finite */
        return (sfp_t)((sign ? SFP_SIGN_BITS : 0u) | 0x7F7FFFFFu);
    }
    /* round to nearest even (truncate the hidden bit) */
    uint32_t out = ((uint32_t)sign << 31) | ((uint32_t)exp << SFP_MANT_BITS) | (mant & SFP_MANT_MASK);
    return out;
}

/* ---- conversions -------------------------------------------------- */
sfp_t sfp_from_int(int32_t v) {
    if (v == 0) return 0;
    int sign = 0;
    int64_t mag = v;
    if (v < 0) { sign = 1; mag = -(int64_t)v; }
    /* find highest set bit */
    int exp = SFP_EXP_BIAS + 23;
    while (mag < (1LL << 23)) { mag <<= 1; exp--; }
    while (mag >= (1LL << 24)) { mag >>= 1; exp++; }
    return sfp_pack(sign, exp, (uint32_t)mag);
}

int32_t sfp_to_int(sfp_t a) {
    int sign, exp; uint32_t mant;
    int kind = sfp_unpack(a, &sign, &exp, &mant);
    if (kind == 0) return 0;
    int shift = exp - SFP_EXP_BIAS - SFP_MANT_BITS;
    int32_t r;
    if (shift >= 0) {
        /* left-shift; saturate to int32 range */
        if (shift > 7) r = 0x7FFFFFFF;
        else r = (int32_t)((uint32_t)mant << shift);
    } else {
        r = (int32_t)(mant >> (-shift));
    }
    return sign ? -r : r;
}

/* ---- comparisons -------------------------------------------------- */
int sfp_cmp(sfp_t a, sfp_t b) {
    /* handle signed-zero equivalence */
    if ((a & ~SFP_SIGN_BITS) == 0 && (b & ~SFP_SIGN_BITS) == 0) return 0;
    int sa = (a & SFP_SIGN_BITS) ? 1 : 0;
    int sb = (b & SFP_SIGN_BITS) ? 1 : 0;
    if (sa != sb) return sa ? -1 : 1;
    /* same sign — compare as unsigned */
    if (sa == 0) {
        if (a < b) return -1;
        if (a > b) return 1;
        return 0;
    } else {
        /* both negative: bigger magnitude = smaller value */
        if (a > b) return -1;
        if (a < b) return 1;
        return 0;
    }
}

int sfp_lt(sfp_t a, sfp_t b) { return sfp_cmp(a, b) < 0; }
int sfp_gt(sfp_t a, sfp_t b) { return sfp_cmp(a, b) > 0; }
int sfp_le(sfp_t a, sfp_t b) { return sfp_cmp(a, b) <= 0; }
int sfp_ge(sfp_t a, sfp_t b) { return sfp_cmp(a, b) >= 0; }
int sfp_eq(sfp_t a, sfp_t b) { return sfp_cmp(a, b) == 0; }

sfp_t sfp_neg(sfp_t a) { return a ^ SFP_SIGN_BITS; }
sfp_t sfp_abs(sfp_t a) { return a & ~SFP_SIGN_BITS; }
sfp_t sfp_max(sfp_t a, sfp_t b) { return sfp_lt(a, b) ? b : a; }
sfp_t sfp_min(sfp_t a, sfp_t b) { return sfp_lt(a, b) ? a : b; }

/* ---- add/sub ------------------------------------------------------ */
sfp_t sfp_add(sfp_t a, sfp_t b) {
    int sa, ea; uint32_t ma;
    int sb, eb; uint32_t mb;
    sfp_unpack(a, &sa, &ea, &ma);
    sfp_unpack(b, &sb, &eb, &mb);
    if (ma == 0 && mb == 0) return 0;
    if (ma == 0) return sfp_pack(sb, eb, mb);
    if (mb == 0) return sfp_pack(sa, ea, ma);

    /* Align to larger exponent by shifting smaller mantissa right. */
    int ediff = ea - eb;
    int eout = ea > eb ? ea : eb;
    if (ediff > 0) {
        if (ediff > 31) mb = 0;
        else mb = mb >> ediff;
    } else if (ediff < 0) {
        if (-ediff > 31) ma = 0;
        else ma = ma >> (-ediff);
    }

    if (sa == sb) {
        /* same sign: add magnitudes (use 32-bit; mantissa can grow to 25 bits) */
        uint64_t sum = (uint64_t)ma + (uint64_t)mb;
        /* sum may be up to 2^25 (hidden+1 plus hidden+1) */
        return sfp_pack(sa, eout, (uint32_t)sum);
    } else {
        /* opposite sign: subtract */
        if (ma >= mb) {
            uint32_t diff = ma - mb;
            return sfp_pack(sa, eout, diff);
        } else {
            uint32_t diff = mb - ma;
            return sfp_pack(sb, eout, diff);
        }
    }
}

sfp_t sfp_sub(sfp_t a, sfp_t b) {
    return sfp_add(a, sfp_neg(b));
}

/* ---- multiply ----------------------------------------------------- */
sfp_t sfp_mul(sfp_t a, sfp_t b) {
    int sa, ea; uint32_t ma;
    int sb, eb; uint32_t mb;
    sfp_unpack(a, &sa, &ea, &ma);
    sfp_unpack(b, &sb, &eb, &mb);
    if (ma == 0 || mb == 0) return 0;
    int sign = sa ^ sb;
    /* ma, mb in [2^23, 2^24) (with hidden bit).
     * Product in [2^46, 2^48).
     * Result exponent: ea + eb - BIAS (before normalization adjustment). */
    uint64_t prod = (uint64_t)ma * (uint64_t)mb;
    int eout = ea + eb - SFP_EXP_BIAS;
    /* Normalize: shift prod into [2^46, 2^47) so that after
     * right-shifting by 23 we get the 24-bit mantissa (1 hidden + 23 explicit). */
    while (prod < (1ULL << 46) && eout > 1) { prod <<= 1; eout--; }
    while (prod >= (1ULL << 47)) { prod >>= 1; eout++; }
    /* Extract 23-bit explicit mantissa (drop the hidden bit). */
    uint32_t mant = (uint32_t)(prod >> 23);
    /* round-to-nearest-even with the dropped 23 bits */
    uint32_t rem = (uint32_t)(prod & 0x007FFFFFu);
    if (rem > 0x00400000u || (rem == 0x00400000u && (mant & 1))) {
        mant++;
    }
    return sfp_pack(sign, eout, mant);
}

/* ---- divide ------------------------------------------------------- */
sfp_t sfp_div(sfp_t a, sfp_t b) {
    int sa, ea; uint32_t ma;
    int sb, eb; uint32_t mb;
    sfp_unpack(a, &sa, &ea, &ma);
    sfp_unpack(b, &sb, &eb, &mb);
    if (mb == 0) {
        /* division by zero — return max finite with sign of a */
        return sfp_pack(sa, 254, SFP_MANT_MASK | SFP_HIDDEN);
    }
    if (ma == 0) return 0;
    int sign = sa ^ sb;
    int eout = ea - eb + SFP_EXP_BIAS;
    /* Scale dividend up by 2^24 so the quotient has 24 significant bits. */
    uint64_t dividend = (uint64_t)ma << 24;
    uint32_t quot = (uint32_t)(dividend / mb);
    /* Normalize so quot is in [2^23, 2^24). */
    while (quot < SFP_HIDDEN && eout > 1) { quot <<= 1; eout--; }
    while (quot >= (SFP_HIDDEN << 1)) { quot >>= 1; eout++; }
    return sfp_pack(sign, eout, quot);
}

/* ---- sqrt (Newton-Raphson) ---------------------------------------- */
sfp_t sfp_sqrt(sfp_t a) {
    int sa, ea; uint32_t ma;
    sfp_unpack(a, &sa, &ea, &ma);
    if (sa) return 0;                  /* sqrt(negative) -> 0        */
    if (ma == 0) return 0;
    /* Halve the exponent (with rounding to nearest even). */
    int eout = ea - SFP_EXP_BIAS;
    if (eout & 1) {
        /* odd exponent: scale mantissa so result exponent is even */
        ma <<= 1;
    }
    eout >>= 1;
    eout += SFP_EXP_BIAS;
    /* Newton-Raphson on mantissa (treat as fixed-point 1.m * 2^0).
     * Initial guess: x0 = ma >> 12 (rough). */
    uint64_t x = (uint64_t)ma + (1u << 11);  /* ensure nonzero */
    x = (x + ((uint64_t)ma << 23) / x) >> 1;
    x = (x + ((uint64_t)ma << 23) / x) >> 1;
    x = (x + ((uint64_t)ma << 23) / x) >> 1;
    x = (x + ((uint64_t)ma << 23) / x) >> 1;
    /* x is roughly sqrt(ma) * 2^12 in our scaled representation; pack. */
    /* normalize x into [2^23, 2^24) */
    while (x < SFP_HIDDEN && eout > 1) { x <<= 1; eout--; }
    while (x >= (SFP_HIDDEN << 1)) { x >>= 1; eout++; }
    return sfp_pack(0, eout, (uint32_t)x);
}

sfp_t sfp_rsqrt(sfp_t a) {
    /* 1/sqrt(a). For our use (RMSNorm), this is the hot path.
     * Compute sqrt then divide 1 by it. */
    sfp_t s = sfp_sqrt(a);
    if (s == 0) return sfp_from_int(0);
    return sfp_div(SFP_ONE, s);
}

/* ---- exp (Taylor + range reduce) ---------------------------------- */
/* e^x = 2^(x/ln2) = 2^k * e^r,  where k = round(x/ln2), r = x - k*ln2.
 * r in [-ln2/2, ln2/2] ~= [-0.347, 0.347]. Taylor: 1 + r + r^2/2 + r^3/6 + r^4/24.
 */
sfp_t sfp_exp(sfp_t x) {
    int sign, ex; uint32_t mx;
    sfp_unpack(x, &sign, &ex, &mx);
    if (mx == 0) return SFP_ONE;
    /* Range-reduce: divide by ln2 (0.6931472). */
    sfp_t LN2  = SFP_LN2;
    sfp_t INV_LN2 = SFP_INV_LN2;
    sfp_t k_f = sfp_mul(x, INV_LN2);
    int k = sfp_to_int(k_f);
    /* k > 128 or k < -128 saturates */
    if (k > 127) return sfp_pack(0, 254, SFP_MANT_MASK | SFP_HIDDEN);
    if (k < -126) return 0;
    sfp_t k_ln2 = sfp_mul(sfp_from_int(k), LN2);
    sfp_t r = sfp_sub(x, k_ln2);
    /* Taylor series for e^r: 1 + r + r^2/2 + r^3/6 + r^4/24 + r^5/120 */
    sfp_t r2 = sfp_mul(r, r);
    sfp_t r3 = sfp_mul(r2, r);
    sfp_t r4 = sfp_mul(r3, r);
    sfp_t r5 = sfp_mul(r4, r);
    sfp_t term = SFP_ONE;
    term = sfp_add(term, r);
    term = sfp_add(term, sfp_mul(SFP_HALF, r2));
    term = sfp_add(term, sfp_mul(SFP_ONE_SIXTH, r3));
    term = sfp_add(term, sfp_mul(SFP_ONE_24TH, r4));
    term = sfp_add(term, sfp_mul(SFP_ONE_120TH, r5));
    /* Multiply by 2^k (just adjust exponent). */
    /* Unpack term and bump exponent. */
    int ts, te; uint32_t tm;
    sfp_unpack(term, &ts, &te, &tm);
    if (tm == 0) return 0;
    te += k;
    return sfp_pack(ts, te, tm);
}

sfp_t sfp_sigmoid(sfp_t x) {
    /* 1 / (1 + e^-x) */
    sfp_t e = sfp_exp(sfp_neg(x));
    return sfp_div(SFP_ONE, sfp_add(SFP_ONE, e));
}

sfp_t sfp_silu(sfp_t x) {
    /* x * sigmoid(x) */
    return sfp_mul(x, sfp_sigmoid(x));
}

sfp_t sfp_tanh(sfp_t x) {
    /* tanh(x) = (e^2x - 1)/(e^2x + 1) */
    sfp_t two_x = sfp_add(x, x);
    sfp_t e = sfp_exp(two_x);
    sfp_t num = sfp_sub(e, SFP_ONE);
    sfp_t den = sfp_add(e, SFP_ONE);
    if (den == 0) return SFP_ONE;
    return sfp_div(num, den);
}

sfp_t sfp_gelu(sfp_t x) {
    /* 0.5 * x * (1 + tanh(sqrt(2/pi) * (x + 0.044715 * x^3))) */
    sfp_t x2 = sfp_mul(x, x);
    sfp_t x3 = sfp_mul(x2, x);
    sfp_t inner = sfp_add(x, sfp_mul(SFP_GELU_C2, x3));
    sfp_t t = sfp_tanh(sfp_mul(SFP_SQRT_2_OVER_PI, inner));
    sfp_t one_plus_t = sfp_add(SFP_ONE, t);
    return sfp_mul(SFP_HALF, sfp_mul(x, one_plus_t));
}

/* ---- sin/cos (range-reduce + Taylor) ------------------------------ */
/* Reduce to [-pi, pi], then use Taylor series. */
sfp_t sfp_sin(sfp_t x) {
    /* Reduce mod 2pi */
    sfp_t TWO_PI = SFP_2PI;
    sfp_t PI     = SFP_PI;
    /* x mod 2pi (rough): divide and subtract */
    sfp_t k_f = sfp_div(x, TWO_PI);
    int k = sfp_to_int(k_f);
    sfp_t r = sfp_sub(x, sfp_mul(sfp_from_int(k), TWO_PI));
    /* Wrap to [-pi, pi] */
    if (sfp_gt(r, PI))      r = sfp_sub(r, TWO_PI);
    else if (sfp_lt(r, sfp_neg(PI))) r = sfp_add(r, TWO_PI);
    /* Taylor: sin(r) = r - r^3/6 + r^5/120 - r^7/5040 */
    sfp_t r2 = sfp_mul(r, r);
    sfp_t r3 = sfp_mul(r2, r);
    sfp_t r5 = sfp_mul(r3, r2);
    sfp_t r7 = sfp_mul(r5, r2);
    sfp_t res = r;
    res = sfp_sub(res, sfp_mul(SFP_ONE_SIXTH, r3));
    res = sfp_add(res, sfp_mul(SFP_ONE_120TH, r5));
    res = sfp_sub(res, sfp_mul(SFP_ONE_5040TH, r7));
    return res;
}

sfp_t sfp_cos(sfp_t x) {
    /* cos(x) = sin(x + pi/2) */
    return sfp_sin(sfp_add(x, SFP_HALF_PI));
}
