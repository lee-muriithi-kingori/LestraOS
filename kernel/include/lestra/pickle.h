/*
 * Lestra OS — Pickle: a from-scratch GGUF model loader and inference engine.
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Pickle is a self-contained GGUF (v3) parser + transformer forward-pass
 * engine. It does NOT use llama.cpp, ggml, or ollama. The math is done with
 * a built-in software float32 layer (pickle_softfp.c) so it runs even when
 * the host CPU's FPU/SSE is unavailable (e.g. inside the lestraOS kernel,
 * which is built with -mno-sse and has no x87 init).
 *
 * Layering:
 *   - pickle.h            (this file)  — public API, freestanding
 *   - pickle_softfp.c     — IEEE-754 single-precision soft float
 *   - pickle.c            — GGUF parse, dequantize, transformer fwd pass
 *
 * The same source compiles two ways:
 *   - PICKLE_KERNEL defined  → uses lestra kernel types & printk
 *   - PICKLE_KERNEL undefined → uses <stdint.h>/<stdio.h> (host build)
 *
 * See docs/ARCHITECTURE.md in the lestramanika repo for the host shim.
 */
#ifndef LESTRA_PICKLE_H
#define LESTRA_PICKLE_H

#ifdef PICKLE_KERNEL
#include <lestra/types.h>
#else
#include <stdint.h>
#include <stddef.h>
#endif

/* sfp_t is the IEEE-754 single-precision bit pattern stored as uint32_t.
 * Declared with may_alias so pickle.c can cast float* <-> sfp_t* safely
 * (treating float storage as IEEE-754 bit patterns). This avoids
 * strict-aliasing UB and silences -Waliasing.
 *
 * Defined here at the top because pickle_arch_t and several function
 * signatures below use sfp_t. */
#if defined(__GNUC__) || defined(__clang__)
typedef uint32_t __attribute__((may_alias)) sfp_t;
#else
typedef uint32_t sfp_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Status codes                                                        */
/* ------------------------------------------------------------------ */
#define PICKLE_OK              0
#define PICKLE_ERR_FORMAT     -1   /* not a GGUF stream / corrupt      */
#define PICKLE_ERR_VERSION    -2   /* unsupported GGUF version         */
#define PICKLE_ERR_MEMORY     -3   /* allocator returned NULL          */
#define PICKLE_ERR_TYPE       -4   /* unsupported tensor/dtype         */
#define PICKLE_ERR_IO         -5   /* I/O callback failure             */
#define PICKLE_ERR_ARG        -6   /* bad argument                     */
#define PICKLE_ERR_RANGE      -7   /* value out of range               */
#define PICKLE_ERR_ARCH       -8   /* unknown model architecture       */

/* ------------------------------------------------------------------ */
/* GGUF value types (metadata KV table)                               */
/* ------------------------------------------------------------------ */
enum gguf_value_type {
    GGUF_UINT8   = 0,
    GGUF_INT8    = 1,
    GGUF_UINT16  = 2,
    GGUF_INT16   = 3,
    GGUF_UINT32  = 4,
    GGUF_INT32   = 5,
    GGUF_FLOAT32 = 6,
    GGUF_BOOL    = 7,
    GGUF_STRING  = 8,
    GGUF_ARRAY   = 9,
    GGUF_UINT64  = 10,
    GGUF_INT64   = 11,
    GGUF_FLOAT64 = 12,
};

/* ------------------------------------------------------------------ */
/* GGML tensor types (the quantization formats pickle can dequantize) */
/* ------------------------------------------------------------------ */
enum ggml_type {
    GGML_F32   = 0,
    GGML_F16   = 1,
    GGML_Q4_0  = 2,
    GGML_Q4_1  = 3,
    GGML_Q5_0  = 6,
    GGML_Q5_1  = 7,
    GGML_Q8_0  = 8,
    GGML_Q8_1  = 9,
    GGML_Q2_K  = 10,
    GGML_Q3_K  = 11,
    GGML_Q4_K  = 12,
    GGML_Q5_K  = 13,
    GGML_Q6_K  = 14,
    GGML_Q8_K  = 15,
};

/* ------------------------------------------------------------------ */
/* I/O abstraction. The caller provides read/seek/tell callbacks.     */
/*   kernel use:  ctx = memory buffer or vfs_file*                    */
/*   host use:    ctx = FILE*                                         */
/* ------------------------------------------------------------------ */
#define PICKLE_SEEK_SET 0
#define PICKLE_SEEK_CUR 1
#define PICKLE_SEEK_END 2

typedef struct pickle_io {
    void*  ctx;
    size_t (*read)(void* ctx, void* buf, size_t len);
    int    (*seek)(void* ctx, int64_t offset, int whence);
    int64_t (*tell)(void* ctx);
    void   (*close)(void* ctx);  /* optional: called by pickle_free if io is owned */
} pickle_io_t;

/* ------------------------------------------------------------------ */
/* Allocator abstraction                                              */
/* ------------------------------------------------------------------ */
typedef struct pickle_alloc {
    void* ctx;
    void* (*alloc)(void* ctx, size_t size);          /* returns zeroed mem */
    void  (*free)(void* ctx, void* ptr, size_t size);
} pickle_alloc_t;

/* Set the allocator used by pickle. NULL = use the default (kernel
 * bump allocator or host malloc). Must be set BEFORE pickle_load. */
void pickle_set_alloc(const pickle_alloc_t* alloc);

/* ------------------------------------------------------------------ */
/* Model handle                                                       */
/* ------------------------------------------------------------------ */
typedef struct pickle_model pickle_model_t;

/* Parse a GGUF stream and load all tensors into RAM. On success
 * *out_model is a new handle (free with pickle_free). */
int  pickle_load(pickle_io_t* io, pickle_model_t** out_model);

/* Parse GGUF header + metadata + tensor table ONLY — does NOT read or
 * dequantize tensor data. The io is retained in the model for on-demand
 * dequantization via pickle_dequant_tensor(). The caller must keep the
 * io (and its underlying file) alive until pickle_free().
 *
 * Use this for `info` and `dequant <tensor>` commands — loading metadata
 * from a large model is instant vs hours for full dequant. */
int  pickle_load_meta(pickle_io_t* io, pickle_model_t** out_model);

/* Dequantize a single tensor on demand. Model must be loaded via
 * pickle_load_meta(). Stores the F32 result in tensors[idx].data.
 * No-op if already dequantized. */
int  pickle_dequant_tensor(pickle_model_t* m, size_t idx);

/* Load a single tensor's raw on-disk bytes into tensors[idx].data
 * WITHOUT dequantizing. Used by the fast-path inference engine so
 * the quantized matmul kernels can read the raw Q4_K/Q6_K/... block
 * bytes directly (dequantizing block-by-block inside the dot product).
 * For F32/F16 tensors the raw bytes ARE the native format. No-op if
 * already loaded (raw or dequantized). */
int  pickle_load_tensor_raw(pickle_model_t* m, size_t idx);

#ifndef PICKLE_KERNEL
/* Attach an mmap'd file region to a meta-loaded model. Patches every
 * tensor's data pointer to point directly into the mmap (zero-copy).
 * pickle_free() will munmap() the region. After this call the model's
 * io is detached (caller owns it). This is the preferred host load
 * path — instant startup, zero copy, OS demand-paging. */
int  pickle_attach_mmap(pickle_model_t* m, void* mmap_base, size_t mmap_size);
#endif

/* Release a model and all its tensors. */
void pickle_free(pickle_model_t* model);

/* ------------------------------------------------------------------ */
/* Inspection                                                         */
/* ------------------------------------------------------------------ */
size_t      pickle_tensor_count(const pickle_model_t* m);
const char* pickle_tensor_name(const pickle_model_t* m, size_t i);

typedef struct {
    char        name[128];
    uint32_t    type;       /* enum ggml_type */
    uint32_t    n_dims;
    uint64_t    dims[8];    /* dims[0] = slowest (rows); dims[n_dims-1] = fastest */
    uint64_t    n_elements;
    size_t      data_offset;/* byte offset of tensor data within the GGUF stream */
    size_t      data_size;  /* byte size of the tensor's data block  */
    void*       data;       /* dequantized F32 buffer (rows padded to 4) */
} pickle_tensor_info_t;

int pickle_tensor_info(const pickle_model_t* m, size_t i, pickle_tensor_info_t* out);

/* Find a tensor by name. Returns index >= 0 or negative error. */
int pickle_tensor_find(const pickle_model_t* m, const char* name);

/* Metadata lookups. Return default if key not found or wrong type. */
const char* pickle_meta_string(const pickle_model_t* m, const char* key);
int64_t     pickle_meta_int   (const pickle_model_t* m, const char* key, int64_t def);
/* pickle_meta_float_bits: kernel-safe (no float in signature). Returns
 * the f32 metadata value as an IEEE-754 bit pattern in sfp_t. */
sfp_t       pickle_meta_float_bits(const pickle_model_t* m, const char* key, sfp_t def_bits);
#ifndef PICKLE_KERNEL
/* Host-only convenience wrappers (the kernel can't pass/return float). */
float       pickle_meta_float (const pickle_model_t* m, const char* key, float def);
#endif

/* ------------------------------------------------------------------ */
/* Dequantization (public — host CLI uses them for inspection tools)  */
/* ------------------------------------------------------------------ */

/* Number of bytes the given quantized type occupies for n elements. */
size_t pickle_type_size(uint32_t type, uint64_t n);

/* Dequantize a stream of n elements of `type` into out[] (F32).
 * Reads bytes from io as needed. */
int pickle_dequant_stream(pickle_io_t* io, uint32_t type, uint64_t n, float* out);

/* ------------------------------------------------------------------ */
/* Architecture descriptor (Llama-style family)                       */
/* ------------------------------------------------------------------ */
enum pickle_norm_type { PICKLE_NORM_RMS = 0, PICKLE_NORM_LAYER = 1 };
enum pickle_act_type  { PICKLE_ACT_SILU = 0, PICKLE_ACT_GELU = 1, PICKLE_ACT_GELU_TANH = 2 };
enum pickle_rope_type { PICKLE_ROPE_STANDARD = 0, PICKLE_ROPE_NEOX = 1 };

typedef struct {
    /* detected from metadata */
    char     arch_name[64];        /* "llama", "qwen2", "phi3", ...    */
    int      n_layers;
    int      n_heads;              /* query heads                       */
    int      n_kv_heads;           /* key/value heads (GQA); ==n_heads for MHA */
    int      head_dim;             /* usually hidden/n_heads            */
    int      hidden_dim;           /* model embedding dimension         */
    int      intermediate_dim;     /* FFN hidden (feed_forward_length)  */
    int      vocab_size;
    int      max_seq_len;          /* context length                    */
    /* Float-valued fields are stored as sfp_t (IEEE-754 bit patterns)
     * so the struct can be moved/copied without ever emitting SSE
     * instructions. Host callers can recover the float value with
     * sfp_to_float(field); kernel callers use the sfp_*() math API. */
    sfp_t    rope_theta_bits;
    sfp_t    rope_freq_scale_bits;
    sfp_t    rms_eps_bits;
    int      tie_word_embeddings;
    int      norm_type;
    int      act_type;
    int      rope_type;
    sfp_t    attn_logit_softcapping_bits;
    sfp_t    final_logit_softcapping_bits;
} pickle_arch_t;

int pickle_arch_detect(const pickle_model_t* m, pickle_arch_t* arch);

/* ------------------------------------------------------------------ */
/* Inference                                                          */
/* ------------------------------------------------------------------ */

/* KV-cache slot, one per layer per position. */
typedef struct {
    float* k;   /* [n_kv_heads * head_dim] */
    float* v;   /* [n_kv_heads * head_dim] */
} pickle_kv_slot_t;

typedef struct {
    int              n_layers;
    int              n_kv_heads;
    int              head_dim;
    int              max_seq;
    pickle_kv_slot_t* slots;  /* [n_layers * max_seq] */
} pickle_kv_cache_t;

/* Allocate a KV cache big enough for arch->max_seq_len. */
int  pickle_kv_alloc(const pickle_arch_t* arch, pickle_kv_cache_t* out);
void pickle_kv_free(pickle_kv_cache_t* kv);

/* Run a forward pass over n_tokens tokens. Writes logits for the LAST
 * token into out_logits (must be arch->vocab_size floats). Uses kv
 * cache starting at *kv_pos (in: where to start writing; out: new pos).
 *
 * Pass kv=NULL / kv_pos=NULL for stateless single-token inference.
 */
int pickle_forward(
    pickle_model_t*        model,
    const pickle_arch_t*   arch,
    const int32_t*         tokens,
    size_t                 n_tokens,
    float*                 out_logits,
    pickle_kv_cache_t*     kv,
    size_t*                kv_pos
);

/* ------------------------------------------------------------------ */
/* Sampling                                                           */
/* ------------------------------------------------------------------ */
int32_t pickle_argmax          (const float* logits, size_t n);
int32_t pickle_sample_greedy   (const float* logits, size_t n);
/* Kernel-safe sampler: temp/top_p passed as sfp_t bit patterns. */
int32_t pickle_sample_temperature_bits(float* logits, size_t n, sfp_t temp_bits, sfp_t top_p_bits, uint32_t seed);
#ifndef PICKLE_KERNEL
int32_t pickle_sample_temperature(float* logits, size_t n, float temp, float top_p, uint32_t seed);
#endif

/* ------------------------------------------------------------------ */
/* Soft-float API (exposed for host test harnesses)                   */
/* ------------------------------------------------------------------ */
/* (sfp_t is typedef'd at the top of this file.) */

#define SFP_ZERO   ((sfp_t)0x00000000u)
#define SFP_ONE    ((sfp_t)0x3F800000u)
#define SFP_NEG_ONE ((sfp_t)0xBF800000u)

/* Precomputed IEEE-754 bit patterns for math constants used inside the
 * soft-float layer. Defined as macros so the kernel build (which can't
 * use sfp_from_float() because that requires SSE for arg passing) can
 * still construct these constants. */
#define SFP_HALF                 ((sfp_t)0x3F000000u)   /* 0.5              */
#define SFP_ONE_SIXTH            ((sfp_t)0x3E2AAAABu)   /* 1/6              */
#define SFP_ONE_24TH             ((sfp_t)0x3D2AAAABu)   /* 1/24             */
#define SFP_ONE_120TH            ((sfp_t)0x3C088889u)   /* 1/120            */
#define SFP_ONE_5040TH           ((sfp_t)0x39500D01u)   /* 1/5040           */
#define SFP_LN2                  ((sfp_t)0x3F317218u)   /* ln(2)            */
#define SFP_INV_LN2              ((sfp_t)0x3FB8AA3Bu)   /* 1/ln(2)          */
#define SFP_SQRT_2_OVER_PI       ((sfp_t)0x3F4C422Au)   /* sqrt(2/pi)       */
#define SFP_GELU_C2              ((sfp_t)0x3D372713u)   /* 0.044715         */
#define SFP_2PI                  ((sfp_t)0x40C90FDBu)   /* 2*pi             */
#define SFP_PI                   ((sfp_t)0x40490FDBu)   /* pi               */
#define SFP_HALF_PI              ((sfp_t)0x3FC90FDBu)   /* pi/2             */
#define SFP_1E5                  ((sfp_t)0x3727C5ACu)   /* 1e-5             */
#define SFP_10000                ((sfp_t)0x461C4000u)   /* 10000.0          */

sfp_t sfp_from_int(int32_t v);
int32_t sfp_to_int(sfp_t a);
#ifndef PICKLE_KERNEL
/* Host-only: bit-cast between C float and sfp_t. The kernel can't use
 * these because passing/returning float requires SSE. */
sfp_t sfp_from_float(float f);
float sfp_to_float  (sfp_t a);
#else
/* In kernel builds, callers must construct sfp_t literals directly
 * (e.g. SFP_ONE, SFP_ZERO, sfp_from_int(42)) — never via float constants. */
#endif

sfp_t sfp_add(sfp_t a, sfp_t b);
sfp_t sfp_sub(sfp_t a, sfp_t b);
sfp_t sfp_mul(sfp_t a, sfp_t b);
sfp_t sfp_div(sfp_t a, sfp_t b);
sfp_t sfp_neg(sfp_t a);
sfp_t sfp_abs(sfp_t a);

int   sfp_cmp(sfp_t a, sfp_t b);     /* -1, 0, 1 */
int   sfp_lt (sfp_t a, sfp_t b);
int   sfp_gt (sfp_t a, sfp_t b);
int   sfp_le (sfp_t a, sfp_t b);
int   sfp_ge (sfp_t a, sfp_t b);
int   sfp_eq (sfp_t a, sfp_t b);

sfp_t sfp_max(sfp_t a, sfp_t b);
sfp_t sfp_min(sfp_t a, sfp_t b);

sfp_t sfp_exp (sfp_t x);    /* e^x,  Taylor + range-reduce   */
sfp_t sfp_tanh(sfp_t x);    /* via (e^2x - 1)/(e^2x + 1)     */
sfp_t sfp_sigmoid(sfp_t x); /* 1 / (1 + e^-x)                */
sfp_t sfp_silu(sfp_t x);    /* x * sigmoid(x)                */
sfp_t sfp_gelu(sfp_t x);    /* 0.5x(1+tanh(sqrt(2/pi)(x+0.044715x^3))) */
sfp_t sfp_sqrt(sfp_t x);
sfp_t sfp_rsqrt(sfp_t x);   /* 1/sqrt(x)                     */
sfp_t sfp_sin(sfp_t x);
sfp_t sfp_cos(sfp_t x);

/* ------------------------------------------------------------------ */
/* Self-test                                                          */
/* ------------------------------------------------------------------ */

/* Embedded tiny GGUF model — generated by scripts/make_tiny_gguf.py.
 * A 1-layer, 8-dim, 16-vocab character model with F32 weights so the
 * kernel can self-verify pickle's forward pass without any FS access. */
extern const unsigned char pickle_demo_gguf[];
extern const unsigned int  pickle_demo_gguf_len;

/* Parse the embedded demo model, run one forward pass with a fixed
 * prompt, write the resulting token id to *out_token. Returns PICKLE_OK
 * on success. Used as a boot-time sanity check. */
int pickle_selftest(int32_t* out_token);

#ifdef __cplusplus
}
#endif

#endif /* LESTRA_PICKLE_H */
