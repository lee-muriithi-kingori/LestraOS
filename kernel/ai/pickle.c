/*
 * Lestra OS — Pickle GGUF engine core.
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Implements:
 *   - GGUF v3 header / metadata / tensor-info parsing (pickle_io_t callbacks)
 *   - Dequantization of all common GGML types to F32 bit-patterns (F32, F16,
 *     Q8_0, Q4_0, Q4_1, Q5_0, Q5_1, Q4_K, Q5_K, Q6_K, Q8_K, Q2_K, Q3_K)
 *   - Llama-family forward pass (RMSNorm, GQA attention w/ RoPE, SwiGLU FFN)
 *   - Greedy + temperature sampling
 *   - Boot-time self-test using an embedded tiny GGUF (pickle_demo_gguf[])
 *
 * Pure freestanding C — no libc, no ggml, no llama.cpp, no ollama. The
 * only dep is the soft-float32 layer in pickle_softfp.c. Same source
 * compiles for the kernel (PICKLE_KERNEL) and for the host (POSIX).
 *
 * ARITHMETIC DISCIPLINE
 *   The lestra kernel is built with -mno-sse and has no x87/SSE FPU init.
 *   gcc STILL emits scalar SSE instructions (movss/mulss/addss) for any
 *   C `float` arithmetic. To stay FPU-free, pickle.c NEVER does C float
 *   arithmetic. All math goes through sfp_t (uint32_t IEEE-754 bit
 *   patterns) and the sfp_*() functions in pickle_softfp.c.
 *
 *   The `float*` pointers in the public API are treated as opaque 32-bit
 *   storage — the bytes stored there are IEEE-754 binary32 bit patterns,
 *   but we never operate on them with C float operators. sfp_t is declared
 *   with __attribute__((may_alias)) so the bit-cast is well-defined.
 */
#ifdef PICKLE_KERNEL
#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <string.h>
/* Kernel string functions are memcpy/memset/strlen/strcmp (libc/include/string.h). */
#define kmemset  memset
#define kmemcpy  memcpy
#else
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>   /* munmap for mmap-backed models */
#define pr_info   printf
#define kmalloc   malloc
#define kfree     free
#define kmemset   memset
#define kmemcpy   memcpy
#endif
#ifdef PICKLE_KERNEL
#include <lestra/pickle.h>
#else
#include "pickle.h"
#endif

/* ================================================================== */
/* Default allocator                                                  */
/* ================================================================== */
static pickle_alloc_t g_alloc;

#ifdef PICKLE_KERNEL
/* Kernel bump allocator — large enough for tiny models. Real models
 * use the kernel page allocator; for the kernel demo we only ever
 * load pickle_demo_gguf[] which is < 4 KB. */
#define KERNEL_BUMP_SIZE  (1u << 22)   /* 4 MiB */
static unsigned char g_kernel_bump[KERNEL_BUMP_SIZE];
static size_t        g_kernel_bump_off = 0;
static void* kernel_bump_alloc(void* ctx, size_t size) {
    (void)ctx;
    size = (size + 15u) & ~15u;
    if (g_kernel_bump_off + size > KERNEL_BUMP_SIZE) return 0;
    void* p = &g_kernel_bump[g_kernel_bump_off];
    g_kernel_bump_off += size;
    for (size_t i = 0; i < size; i++) ((unsigned char*)p)[i] = 0;
    return p;
}
static void kernel_bump_free(void* ctx, void* ptr, size_t size) { (void)ctx; (void)ptr; (void)size; }
#endif

static void* default_alloc(void* ctx, size_t size) {
    (void)ctx;
#ifdef PICKLE_KERNEL
    return kernel_bump_alloc(0, size);
#else
    void* p = malloc(size ? size : 1);
    if (p) memset(p, 0, size);
    return p;
#endif
}
static void default_free(void* ctx, void* ptr, size_t size) {
    (void)ctx; (void)size;
#ifdef PICKLE_KERNEL
    kernel_bump_free(0, ptr, size);
#else
    free(ptr);
#endif
}

void pickle_set_alloc(const pickle_alloc_t* alloc) {
    if (alloc) g_alloc = *alloc;
    else { g_alloc.ctx = 0; g_alloc.alloc = default_alloc; g_alloc.free = default_free; }
}

static void* palloc(size_t size) {
    if (!g_alloc.alloc) pickle_set_alloc(0);
    return g_alloc.alloc(g_alloc.ctx, size);
}
static void pfree(void* p, size_t size) {
    if (!g_alloc.free) return;
    g_alloc.free(g_alloc.ctx, p, size);
}

/* ================================================================== */
/* I/O helpers                                                        */
/* ================================================================== */
static int io_read(pickle_io_t* io, void* buf, size_t len) {
    size_t got = io->read(io->ctx, buf, len);
    return (got == len) ? 0 : -1;
}
static uint32_t read_u32(pickle_io_t* io) {
    unsigned char b[4];
    if (io_read(io, b, 4)) return 0;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint64_t read_u64(pickle_io_t* io) {
    unsigned char b[8];
    if (io_read(io, b, 8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)b[i] << (i * 8);
    return v;
}
static int32_t read_i32(pickle_io_t* io) __attribute__((unused));
static int32_t read_i32(pickle_io_t* io) { return (int32_t)read_u32(io); }
static int64_t read_i64(pickle_io_t* io) { return (int64_t)read_u64(io); }
/* Skip an f64 — we never use the value (rare in Llama GGUFs), and
 * returning double would emit SSE returns which the kernel can't do. */
static void skip_f64(pickle_io_t* io) {
    unsigned char b[8]; (void)io_read(io, b, 8);
}

/* Read a GGUF f32 — return its IEEE-754 bit pattern stored in a sfp_t.
 * No float arithmetic happens here; we just read 4 bytes and pack. */
static sfp_t read_f32_bits(pickle_io_t* io) {
    return (sfp_t)read_u32(io);
}
/* Read a GGUF f16 — convert to f32 bit pattern (still no float arithmetic). */
static uint32_t f16_to_f32_bits(uint16_t h) {
    uint32_t sign = (h & 0x8000) << 16;
    uint32_t exp  = (h & 0x7C00) >> 10;
    uint32_t mant = (h & 0x03FF);
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) f = sign;
        else {
            /* Subnormal: normalise. The f16 subnormal exponent is
             * effectively 1 (giving 2^(1-15) = 2^(-14)). Shift the
             * mantissa left until the hidden bit (0x0400) is set,
             * decrementing the exponent for each shift. */
            exp = 1;
            while ((mant & 0x0400) == 0) { mant <<= 1; exp--; }
            mant &= 0x03FF;
            f = sign | ((exp + 127 - 15) << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        f = sign | 0x7F800000u | (mant << 13);
    } else {
        f = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    return f;
}
static sfp_t read_f16_bits(pickle_io_t* io) {
    uint16_t h = (uint16_t)((uint32_t)(unsigned char)0 | 0);
    unsigned char b[2];
    if (io_read(io, b, 2)) return 0;
    h = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return (sfp_t)f16_to_f32_bits(h);
}

static uint16_t read_u16(pickle_io_t* io) {
    unsigned char b[2]; if (io_read(io, b, 2)) return 0;
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}
static char* read_string(pickle_io_t* io) {
    uint64_t len = read_u64(io);
    if (len > (1u << 20)) return 0;
    char* s = (char*)palloc((size_t)len + 1);
    if (!s) return 0;
    if (io_read(io, s, (size_t)len)) { pfree(s, (size_t)len + 1); return 0; }
    s[len] = 0;
    return s;
}

/* ================================================================== */
/* Model & metadata storage                                           */
/* ================================================================== */
typedef struct {
    char*    key;
    uint32_t type;
    union {
        char*    str;
        int64_t  i64;
        uint64_t u64;
        sfp_t    f32_bits;
        uint8_t  b;
        struct { uint32_t type; uint64_t n; void* data; } arr;
    } v;
} pickle_kv_t;

struct pickle_model {
    uint32_t       version;
    uint64_t       tensor_count;
    uint64_t       kv_count;
    pickle_kv_t*   kv;
    pickle_tensor_info_t* tensors;
    uint32_t       alignment;
    /* On-demand dequant support: when loaded via pickle_load_meta(),
     * io and data_section_start are stored so individual tensors can
     * be dequantized later via pickle_dequant_tensor(). For full
     * pickle_load(), io remains NULL (all tensors already dequantized). */
    pickle_io_t*   io;
    int64_t        data_section_start;
#ifndef PICKLE_KERNEL
    /* mmap backend (host only): when non-NULL, every tensor's data pointer
     * points into this mmap region and must NOT be individually freed.
     * pickle_free() will munmap() the whole region instead of calling
     * pfree() on each tensor. Set by pickle_attach_mmap(). */
    void*  mmap_base;
    size_t mmap_size;
#endif
};

/* ================================================================== */
/* GGUF parsing                                                       */
/* ================================================================== */
static int parse_metadata_value(pickle_io_t* io, uint32_t type, pickle_kv_t* kv) {
    kv->type = type;
    switch (type) {
        case GGUF_UINT8:  { unsigned char b; if (io_read(io, &b, 1)) return -1; kv->v.u64 = b; return 0; }
        case GGUF_INT8:   { signed char b;   if (io_read(io, &b, 1)) return -1; kv->v.i64 = b; return 0; }
        case GGUF_UINT16: { kv->v.u64 = read_u16(io); return 0; }
        case GGUF_INT16:  { kv->v.i64 = (int16_t)read_u16(io); return 0; }
        case GGUF_UINT32: { kv->v.u64 = read_u32(io); return 0; }
        case GGUF_INT32:  { kv->v.i64 = (int32_t)read_u32(io); return 0; }
        case GGUF_UINT64: { kv->v.u64 = read_u64(io); return 0; }
        case GGUF_INT64:  { kv->v.i64 = read_i64(io); return 0; }
        case GGUF_FLOAT32:{ kv->v.f32_bits = read_f32_bits(io); return 0; }
        case GGUF_FLOAT64:{ skip_f64(io); kv->v.f32_bits = 0; return 0; }
        case GGUF_BOOL:   { unsigned char b; if (io_read(io, &b, 1)) return -1; kv->v.b = b ? 1 : 0; return 0; }
        case GGUF_STRING: { kv->v.str = read_string(io); return kv->v.str ? 0 : -1; }
        case GGUF_ARRAY:  {
            uint32_t etype = read_u32(io);
            uint64_t n = read_u64(io);
            if (n > (1u << 24)) return -1;
            size_t elem_sz = 0;
            switch (etype) {
                case GGUF_UINT8: case GGUF_INT8: case GGUF_BOOL:    elem_sz = 1; break;
                case GGUF_UINT16: case GGUF_INT16:                   elem_sz = 2; break;
                case GGUF_UINT32: case GGUF_INT32: case GGUF_FLOAT32:elem_sz = 4; break;
                case GGUF_UINT64: case GGUF_INT64: case GGUF_FLOAT64:elem_sz = 8; break;
                case GGUF_STRING: elem_sz = 0; break;
                default: return -1;
            }
            kv->v.arr.type = etype;
            kv->v.arr.n = n;
            if (etype == GGUF_STRING) {
                char** arr = (char**)palloc((size_t)n * sizeof(char*));
                if (!arr) return -1;
                for (uint64_t i = 0; i < n; i++) {
                    arr[i] = read_string(io);
                    if (!arr[i]) return -1;
                }
                kv->v.arr.data = arr;
            } else {
                void* arr = palloc((size_t)n * elem_sz);
                if (!arr) return -1;
                if (io_read(io, arr, (size_t)n * elem_sz)) return -1;
                kv->v.arr.data = arr;
            }
            return 0;
        }
        default: return -1;
    }
}

/* Parse GGUF header + metadata + tensor table. Does NOT read or
 * dequantize tensor data. Stores io + data_section_start in the model
 * for later use by pickle_dequant_tensor().
 *
 * This is the fast path for `pickle info` and `pickle dequant <tensor>`
 * — loading only metadata from a 638MB Q4_K_M model takes milliseconds
 * instead of hours (full dequant of every tensor with the software-float
 * layer is O(billions of F32 ops)). */
int pickle_load_meta(pickle_io_t* io, pickle_model_t** out_model) {
    if (!io || !out_model) return PICKLE_ERR_ARG;
    if (!g_alloc.alloc) pickle_set_alloc(0);

    uint32_t magic = read_u32(io);
    if (magic != 0x46554747u) return PICKLE_ERR_FORMAT;

    pickle_model_t* m = (pickle_model_t*)palloc(sizeof(pickle_model_t));
    if (!m) return PICKLE_ERR_MEMORY;
    m->io = NULL;
    m->data_section_start = 0;
    m->version = read_u32(io);
    if (m->version != 3 && m->version != 2) {
        pfree(m, sizeof(*m));
        return PICKLE_ERR_VERSION;
    }
    m->tensor_count = read_u64(io);
    m->kv_count     = read_u64(io);
    if (m->kv_count > 4096 || m->tensor_count > 65536) {
        pfree(m, sizeof(*m));
        return PICKLE_ERR_FORMAT;
    }

    m->kv = (pickle_kv_t*)palloc((size_t)m->kv_count * sizeof(pickle_kv_t));
    if (!m->kv) { pfree(m, sizeof(*m)); return PICKLE_ERR_MEMORY; }
    m->alignment = 32;

    for (uint64_t i = 0; i < m->kv_count; i++) {
        char* key = read_string(io);
        if (!key) return PICKLE_ERR_FORMAT;
        uint32_t type = read_u32(io);
        m->kv[i].key = key;
        if (parse_metadata_value(io, type, &m->kv[i]) != 0) return PICKLE_ERR_FORMAT;
        if (strcmp(key, "general.alignment") == 0 && type == GGUF_UINT32) {
            m->alignment = (uint32_t)m->kv[i].v.u64;
            if (m->alignment == 0) m->alignment = 32;
        }
    }

    m->tensors = (pickle_tensor_info_t*)palloc((size_t)m->tensor_count * sizeof(pickle_tensor_info_t));
    if (!m->tensors) return PICKLE_ERR_MEMORY;

    for (uint64_t i = 0; i < m->tensor_count; i++) {
        pickle_tensor_info_t* t = &m->tensors[i];
        char* name = read_string(io);
        if (!name) return PICKLE_ERR_FORMAT;
        size_t nl = strlen(name);
        if (nl >= sizeof(t->name)) nl = sizeof(t->name) - 1;
        kmemcpy(t->name, name, nl); t->name[nl] = 0;
        pfree(name, strlen(name) + 1);
        t->n_dims = read_u32(io);
        if (t->n_dims == 0 || t->n_dims > 8) return PICKLE_ERR_FORMAT;
        uint64_t n_elems = 1;
        for (uint32_t d = 0; d < t->n_dims; d++) {
            t->dims[d] = read_u64(io);
            n_elems *= t->dims[d];
        }
        t->n_elements = n_elems;
        t->type = read_u32(io);
        t->data_offset = (size_t)read_u64(io);
        t->data = 0;
        t->data_size = pickle_type_size(t->type, n_elems);
        if (t->data_size == 0) return PICKLE_ERR_TYPE;
    }

    int64_t data_section_start = io->tell(io->ctx);
    if (m->alignment > 1) {
        uint64_t mod = (uint64_t)data_section_start % m->alignment;
        if (mod != 0) {
            io->seek(io->ctx, (int64_t)(m->alignment - mod), PICKLE_SEEK_CUR);
            data_section_start = io->tell(io->ctx);
        }
    }

    /* Store io + data_section_start for on-demand dequant. The caller
     * must keep the io (and its underlying FILE*) alive until
     * pickle_free() is called. */
    m->io = io;
    m->data_section_start = data_section_start;

    *out_model = m;
    return PICKLE_OK;
}

/* Dequantize a single tensor on demand. The model must have been loaded
 * via pickle_load_meta() (which stores the io). The tensor's dequantized
 * F32 data is stored in t->data (allocated here). If already dequantized,
 * this is a no-op. */
int pickle_dequant_tensor(pickle_model_t* m, size_t idx) {
    if (!m || idx >= m->tensor_count) return PICKLE_ERR_ARG;
    pickle_tensor_info_t* t = &m->tensors[idx];
    if (t->data) return PICKLE_OK;  /* already loaded (mmap, raw, or dequant) */
    if (!m->io) return PICKLE_ERR_ARG;  /* no io to read from */

    m->io->seek(m->io->ctx, m->data_section_start + (int64_t)t->data_offset, PICKLE_SEEK_SET);
    size_t bytes = (size_t)t->n_elements * sizeof(sfp_t);
    bytes = (bytes + 15u) & ~15u;
    t->data = palloc(bytes);
    if (!t->data) return PICKLE_ERR_MEMORY;
    int rc = pickle_dequant_stream(m->io, t->type, t->n_elements, (float*)t->data);
    if (rc != PICKLE_OK) {
        pr_info("pickle: dequant failed for tensor %s (type %u): rc=%d\n",
                t->name, t->type, rc);
        pfree(t->data, bytes);
        t->data = 0;
    }
    return rc;
}

/* Load a tensor's raw on-disk bytes into t->data WITHOUT dequantizing.
 * Used by the fast-path inference engine: the quantized matmul kernels
 * (pickle_fast_matmul_q4_k etc.) read the raw Q4_K/Q6_K/... byte buffer
 * directly and dequantize block-by-block inside the dot product, which
 * is far more cache-friendly than materialising the full F32 weight
 * matrix. For F32 and F16 tensors, the raw bytes ARE the native format
 * the fast-path matmul expects.
 *
 * No-op if t->data is already non-NULL (whether from a previous raw
 * load or a full dequant). */
int pickle_load_tensor_raw(pickle_model_t* m, size_t idx) {
    if (!m || idx >= m->tensor_count) return PICKLE_ERR_ARG;
    pickle_tensor_info_t* t = &m->tensors[idx];
    if (t->data) return PICKLE_OK;  /* already loaded (mmap, raw, or dequant) */
    if (!m->io) return PICKLE_ERR_ARG;  /* no io to read from */

    m->io->seek(m->io->ctx, m->data_section_start + (int64_t)t->data_offset, PICKLE_SEEK_SET);
    size_t bytes = t->data_size;
    bytes = (bytes + 15u) & ~15u;
    t->data = palloc(bytes);
    if (!t->data) return PICKLE_ERR_MEMORY;
    if (io_read(m->io, t->data, t->data_size)) {
        pr_info("pickle: raw load failed for tensor %s: io error\n", t->name);
        pfree(t->data, bytes);
        t->data = 0;
        return PICKLE_ERR_IO;
    }
    return PICKLE_OK;
}

#ifndef PICKLE_KERNEL
/* Attach an mmap'd file region to a meta-loaded model. Patches every
 * tensor's data pointer to point directly into the mmap at the correct
 * offset (zero-copy: no malloc, no fread). Stores the mmap base/size so
 * pickle_free() can munmap() the region.
 *
 * After this call the model no longer needs its io (pickle_free won't
 * touch it — the caller is responsible for closing/freeing the io).
 *
 * This is the preferred load path for host inference: instant startup
 * (no multi-second fread of 600+ MB), zero copy, and the OS page cache
 * manages demand paging. Use pickle_load_from_file_mmap() in the host
 * shim which does the mmap + meta parse + attach in one call. */
int pickle_attach_mmap(pickle_model_t* m, void* mmap_base, size_t mmap_size) {
    if (!m || !mmap_base || mmap_size == 0) return PICKLE_ERR_ARG;
    m->mmap_base = mmap_base;
    m->mmap_size = mmap_size;
    /* Patch every tensor's data pointer into the mmap. The data_offset
     * field (set by pickle_load_meta) is the byte offset of the tensor's
     * data within the GGUF data section; data_section_start is where the
     * data section begins in the file. */
    unsigned char* base = (unsigned char*)mmap_base;
    for (uint64_t i = 0; i < m->tensor_count; i++) {
        pickle_tensor_info_t* t = &m->tensors[i];
        t->data = base + m->data_section_start + (size_t)t->data_offset;
    }
    /* Detach the io — the caller owns it and will close/free it. */
    m->io = NULL;
    return PICKLE_OK;
}
#endif

int pickle_load(pickle_io_t* io, pickle_model_t** out_model) {
    int rc = pickle_load_meta(io, out_model);
    if (rc != PICKLE_OK) return rc;
    pickle_model_t* m = *out_model;

    /* Full load: dequantize all tensors now. io is NOT retained (caller
     * can close it after this returns). */
    pickle_io_t* saved_io = m->io;
    m->io = NULL;  /* detach so pickle_free won't touch it */

    for (uint64_t i = 0; i < m->tensor_count; i++) {
        pickle_tensor_info_t* t = &m->tensors[i];
        saved_io->seek(saved_io->ctx, m->data_section_start + (int64_t)t->data_offset, PICKLE_SEEK_SET);
        size_t bytes = (size_t)t->n_elements * sizeof(sfp_t);
        bytes = (bytes + 15u) & ~15u;
        t->data = palloc(bytes);
        if (!t->data) return PICKLE_ERR_MEMORY;
        int drc = pickle_dequant_stream(saved_io, t->type, t->n_elements, (float*)t->data);
        if (drc != PICKLE_OK) {
            pr_info("pickle: dequant failed for tensor %s (type %u): rc=%d\n",
                    t->name, t->type, drc);
            return drc;
        }
    }

    return PICKLE_OK;
}

void pickle_free(pickle_model_t* m) {
    if (!m) return;
#ifndef PICKLE_KERNEL
    if (m->mmap_base) {
        /* mmap'd model: all tensor data pointers point into the mmap.
         * Don't pfree them individually — just munmap the whole region. */
        munmap(m->mmap_base, m->mmap_size);
        m->mmap_base = NULL;
    } else {
        for (uint64_t i = 0; i < m->tensor_count; i++) {
            if (m->tensors[i].data) pfree(m->tensors[i].data,
                                          (size_t)m->tensors[i].n_elements * sizeof(sfp_t));
        }
    }
#else
    for (uint64_t i = 0; i < m->tensor_count; i++) {
        if (m->tensors[i].data) pfree(m->tensors[i].data,
                                      (size_t)m->tensors[i].n_elements * sizeof(sfp_t));
    }
#endif
    pfree(m->tensors, (size_t)m->tensor_count * sizeof(pickle_tensor_info_t));
    for (uint64_t i = 0; i < m->kv_count; i++) {
        if (m->kv[i].key) pfree(m->kv[i].key, strlen(m->kv[i].key) + 1);
        if (m->kv[i].type == GGUF_STRING && m->kv[i].v.str)
            pfree(m->kv[i].v.str, strlen(m->kv[i].v.str) + 1);
    }
    pfree(m->kv, (size_t)m->kv_count * sizeof(pickle_kv_t));
    /* If the model owns an io (from pickle_load_meta), close + free it. */
    if (m->io) {
        if (m->io->close) m->io->close(m->io->ctx);
        /* The io struct itself was heap-allocated by the host shim
         * (pickle_load_from_file_meta). Free it with the C library free()
         * since it was allocated with calloc, not the pickle allocator. */
#ifndef PICKLE_KERNEL
        free(m->io);
#endif
        m->io = NULL;
    }
    pfree(m, sizeof(*m));
}

/* ================================================================== */
/* Inspection                                                         */
/* ================================================================== */
size_t pickle_tensor_count(const pickle_model_t* m) { return m ? (size_t)m->tensor_count : 0; }
const char* pickle_tensor_name(const pickle_model_t* m, size_t i) {
    if (!m || i >= m->tensor_count) return 0;
    return m->tensors[i].name;
}
int pickle_tensor_info(const pickle_model_t* m, size_t i, pickle_tensor_info_t* out) {
    if (!m || !out || i >= m->tensor_count) return PICKLE_ERR_ARG;
    *out = m->tensors[i];
    return PICKLE_OK;
}
int pickle_tensor_find(const pickle_model_t* m, const char* name) {
    if (!m || !name) return PICKLE_ERR_ARG;
    for (uint64_t i = 0; i < m->tensor_count; i++) {
        if (strcmp(m->tensors[i].name, name) == 0) return (int)i;
    }
    return PICKLE_ERR_ARG;
}

const char* pickle_meta_string(const pickle_model_t* m, const char* key) {
    if (!m || !key) return 0;
    for (uint64_t i = 0; i < m->kv_count; i++) {
        if (m->kv[i].type == GGUF_STRING && strcmp(m->kv[i].key, key) == 0)
            return m->kv[i].v.str;
    }
    return 0;
}
int64_t pickle_meta_int(const pickle_model_t* m, const char* key, int64_t def) {
    if (!m || !key) return def;
    for (uint64_t i = 0; i < m->kv_count; i++) {
        if (strcmp(m->kv[i].key, key) != 0) continue;
        switch (m->kv[i].type) {
            case GGUF_INT8:   case GGUF_INT16:  case GGUF_INT32:  case GGUF_INT64:
            case GGUF_UINT8:  case GGUF_UINT16: case GGUF_UINT32: case GGUF_UINT64:
                return (int64_t)m->kv[i].v.u64;
            default: return def;
        }
    }
    return def;
}
#ifndef PICKLE_KERNEL
float pickle_meta_float(const pickle_model_t* m, const char* key, float def) {
    if (!m || !key) return def;
    for (uint64_t i = 0; i < m->kv_count; i++) {
        if (strcmp(m->kv[i].key, key) != 0) continue;
        if (m->kv[i].type == GGUF_FLOAT32) {
            /* bit-cast to float — no arithmetic, just reinterpret */
            sfp_t u = m->kv[i].v.f32_bits;
            return sfp_to_float(u);
        }
    }
    return def;
}
#endif

sfp_t pickle_meta_float_bits(const pickle_model_t* m, const char* key, sfp_t def_bits) {
    if (!m || !key) return def_bits;
    for (uint64_t i = 0; i < m->kv_count; i++) {
        if (strcmp(m->kv[i].key, key) != 0) continue;
        if (m->kv[i].type == GGUF_FLOAT32) return m->kv[i].v.f32_bits;
    }
    return def_bits;
}

/* Expose a GGUF_ARRAY metadata value as a read-only view. Returns
 * PICKLE_OK and fills *out, or PICKLE_ERR_ARG if the key is absent or
 * not an array. Used by the host BPE tokenizer (pickle_tokenizer.c)
 * to read tokenizer.ggml.tokens / .scores / .token_type without
 * needing access to pickle_kv_t (which is private to this file).
 *
 * Layout conventions:
 *   - For numeric element types (UINT8/INT32/FLOAT32/etc.): out->data
 *     points at the n*elem_sz raw bytes, out->n is the element count.
 *   - For GGUF_STRING: out->data points at a (char**) array of n
 *     NUL-terminated string pointers (each separately allocated by
 *     the parser). out->n is the element count. Callers iterate
 *     ((char**)out->data)[i].
 */
typedef struct {
    uint32_t type;     /* GGUF value type of element */
    uint64_t n;        /* element count */
    const void* data;  /* raw bytes (numeric) or char** (string) */
} pickle_array_view_t;

int pickle_meta_array(const pickle_model_t* m, const char* key, pickle_array_view_t* out) {
    if (!m || !key || !out) return PICKLE_ERR_ARG;
    for (uint64_t i = 0; i < m->kv_count; i++) {
        if (strcmp(m->kv[i].key, key) != 0) continue;
        if (m->kv[i].type != GGUF_ARRAY) return PICKLE_ERR_ARG;
        out->type = m->kv[i].v.arr.type;
        out->n    = m->kv[i].v.arr.n;
        out->data = m->kv[i].v.arr.data;
        return PICKLE_OK;
    }
    return PICKLE_ERR_ARG;
}

/* ================================================================== */
/* Type sizes & dequantization                                        */
/* All dequant paths write sfp_t bit patterns into the (sfp_t*)out    */
/* buffer. NO C float arithmetic anywhere.                            */
/* ================================================================== */
size_t pickle_type_size(uint32_t type, uint64_t n) {
    switch (type) {
        case GGML_F32:  return (size_t)(n * 4);
        case GGML_F16:  return (size_t)(n * 2);
        case GGML_Q8_0: return (size_t)((n / 32) * 34);
        case GGML_Q8_1: return (size_t)((n / 32) * 36);
        case GGML_Q4_0: return (size_t)((n / 32) * 18);
        case GGML_Q4_1: return (size_t)((n / 32) * 20);
        case GGML_Q5_0: return (size_t)((n / 32) * 22);
        case GGML_Q5_1: return (size_t)((n / 32) * 24);
        case GGML_Q6_K: return (size_t)((n / 256) * 210);
        case GGML_Q5_K: return (size_t)((n / 256) * 176);
        case GGML_Q4_K: return (size_t)((n / 256) * 144);
        case GGML_Q3_K: return (size_t)((n / 256) * 110);
        case GGML_Q2_K: return (size_t)((n / 256) * 84);
        case GGML_Q8_K: return (size_t)((n / 256) * 292);
        default: return 0;
    }
}

static int read_block(pickle_io_t* io, void* dst, size_t n) {
    return io_read(io, dst, n) ? PICKLE_ERR_IO : PICKLE_OK;
}

int pickle_dequant_stream(pickle_io_t* io, uint32_t type, uint64_t n, float* out_f) {
    if (!io || !out_f) return PICKLE_ERR_ARG;
    sfp_t* out = (sfp_t*)out_f;   /* bit-pattern storage; no aliasing issue (sfp_t may_alias) */

    switch (type) {
        case GGML_F32: {
            for (uint64_t i = 0; i < n; i++) out[i] = read_f32_bits(io);
            return PICKLE_OK;
        }
        case GGML_F16: {
            for (uint64_t i = 0; i < n; i++) out[i] = read_f16_bits(io);
            return PICKLE_OK;
        }
        case GGML_Q8_0: {
            uint64_t blocks = n / 32;
            for (uint64_t b = 0; b < blocks; b++) {
                sfp_t d = read_f16_bits(io);
                signed char q[32];
                if (read_block(io, q, 32)) return PICKLE_ERR_IO;
                for (int i = 0; i < 32; i++)
                    out[b*32 + i] = sfp_mul(d, sfp_from_int((int32_t)q[i]));
            }
            return PICKLE_OK;
        }
        case GGML_Q8_1: {
            uint64_t blocks = n / 32;
            for (uint64_t b = 0; b < blocks; b++) {
                sfp_t d = read_f32_bits(io);
                sfp_t m = read_f32_bits(io);
                signed char q[32];
                if (read_block(io, q, 32)) return PICKLE_ERR_IO;
                for (int i = 0; i < 32; i++)
                    out[b*32 + i] = sfp_add(sfp_mul(d, sfp_from_int((int32_t)q[i])), m);
            }
            return PICKLE_OK;
        }
        case GGML_Q4_0: {
            uint64_t blocks = n / 32;
            for (uint64_t b = 0; b < blocks; b++) {
                sfp_t d = read_f16_bits(io);
                unsigned char q[16];
                if (read_block(io, q, 16)) return PICKLE_ERR_IO;
                for (int i = 0; i < 16; i++) {
                    int lo = q[i] & 0x0F;
                    int hi = (q[i] >> 4) & 0x0F;
                    out[b*32 + i]      = sfp_mul(d, sfp_from_int(lo - 8));
                    out[b*32 + 16 + i] = sfp_mul(d, sfp_from_int(hi - 8));
                }
            }
            return PICKLE_OK;
        }
        case GGML_Q4_1: {
            uint64_t blocks = n / 32;
            for (uint64_t b = 0; b < blocks; b++) {
                sfp_t d = read_f32_bits(io);
                sfp_t m = read_f32_bits(io);
                unsigned char q[16];
                if (read_block(io, q, 16)) return PICKLE_ERR_IO;
                for (int i = 0; i < 16; i++) {
                    int lo = q[i] & 0x0F;
                    int hi = (q[i] >> 4) & 0x0F;
                    out[b*32 + i]      = sfp_add(sfp_mul(d, sfp_from_int(lo)), m);
                    out[b*32 + 16 + i] = sfp_add(sfp_mul(d, sfp_from_int(hi)), m);
                }
            }
            return PICKLE_OK;
        }
        case GGML_Q5_0: {
            uint64_t blocks = n / 32;
            for (uint64_t b = 0; b < blocks; b++) {
                sfp_t d = read_f16_bits(io);
                unsigned char qh[4], ql[16];
                if (read_block(io, qh, 4))  return PICKLE_ERR_IO;
                if (read_block(io, ql, 16)) return PICKLE_ERR_IO;
                uint32_t q5 = (uint32_t)qh[0] | ((uint32_t)qh[1] << 8) |
                              ((uint32_t)qh[2] << 16) | ((uint32_t)qh[3] << 24);
                sfp_t offset = sfp_mul(d, sfp_from_int(-16));
                for (int i = 0; i < 16; i++) {
                    int lo = ql[i] & 0x0F;
                    int hi = (ql[i] >> 4) & 0x0F;
                    int b0 = ((q5 >> i)      & 1) << 4;
                    int b1 = ((q5 >> (16+i)) & 1) << 4;
                    out[b*32 + i]      = sfp_add(sfp_mul(d, sfp_from_int(lo | b0)), offset);
                    out[b*32 + 16 + i] = sfp_add(sfp_mul(d, sfp_from_int(hi | b1)), offset);
                }
            }
            return PICKLE_OK;
        }
        case GGML_Q5_1: {
            uint64_t blocks = n / 32;
            for (uint64_t b = 0; b < blocks; b++) {
                sfp_t d = read_f32_bits(io);
                sfp_t m = read_f32_bits(io);
                unsigned char qh[4], ql[16];
                if (read_block(io, qh, 4))  return PICKLE_ERR_IO;
                if (read_block(io, ql, 16)) return PICKLE_ERR_IO;
                uint32_t q5 = (uint32_t)qh[0] | ((uint32_t)qh[1] << 8) |
                              ((uint32_t)qh[2] << 16) | ((uint32_t)qh[3] << 24);
                for (int i = 0; i < 16; i++) {
                    int lo = ql[i] & 0x0F;
                    int hi = (ql[i] >> 4) & 0x0F;
                    int b0 = ((q5 >> i)      & 1) << 4;
                    int b1 = ((q5 >> (16+i)) & 1) << 4;
                    out[b*32 + i]      = sfp_add(sfp_mul(d, sfp_from_int(lo | b0)), m);
                    out[b*32 + 16 + i] = sfp_add(sfp_mul(d, sfp_from_int(hi | b1)), m);
                }
            }
            return PICKLE_OK;
        }
        /* K-quants: correct GGML block layouts matching ggml/ggjt spec.
 * Block sizes: Q2_K=84, Q3_K=110, Q4_K=144, Q5_K=176, Q6_K=210, Q8_K=292.
 * For the kernel self-test we always use F32. */
        case GGML_Q6_K: {
            /* 210 bytes: ql[128] + qh[64] + scales[16] (int8) + d(f16,2)
             * GGML non-interleaved packing (matches ggml-quants.c):
             *   2 chunks of 128 elements. Within a chunk, 4 sub-blocks of 32 (sub=0..3):
             *   ql[chunk*64 + (sub%2)*32 + l]: low nibble if sub in {0,1}, high if sub in {2,3}
             *   qh[chunk*32 + l] bits [2*sub, 2*sub+1]: the high 2 bits
             *   Scale: scales[i/16]
             *   Formula: dq = d * scales[i/16] * (q - 32) */
            uint64_t blocks = n / 256;
            for (uint64_t b = 0; b < blocks; b++) {
                unsigned char ql[128], qh[64];
                signed char scales[16];
                if (read_block(io, ql, 128))                    return PICKLE_ERR_IO;
                if (read_block(io, qh, 64))                     return PICKLE_ERR_IO;
                if (read_block(io, (unsigned char*)scales, 16)) return PICKLE_ERR_IO;
                sfp_t d = read_f16_bits(io);
                for (int i = 0; i < 256; i++) {
                    int chunk  = i >> 7;
                    int within = i & 127;
                    int sub    = within >> 5;
                    int l      = within & 31;
                    int ql_byte = chunk*64 + (sub & 1)*32 + l;
                    int q_lo = (ql[ql_byte] >> (4 * (sub >> 1))) & 0x0F;
                    int qh_byte = chunk*32 + l;
                    int q_hi = (qh[qh_byte] >> (2 * sub)) & 0x03;
                    int q = q_lo | (q_hi << 4);
                    q -= 32;
                    out[b*256 + i] = sfp_mul(d, sfp_mul(
                        sfp_from_int((int32_t)scales[i / 16]),
                        sfp_from_int((int32_t)q)));
                }
            }
            return PICKLE_OK;
        }
        case GGML_Q5_K: {
            /* 176 bytes: d(f16,2) + dmin(f16,2) + scales[12] + qh[32] + qs[128]
             * GGML non-interleaved: 4 chunks of 64. qs advances 32/chunk.
             * qh is REUSED (not advanced) — each byte covers 8 elements at
             * bit positions 2*chunk+half. */
            uint64_t blocks = n / 256;
            for (uint64_t b = 0; b < blocks; b++) {
                sfp_t d    = read_f16_bits(io);
                sfp_t dmin = read_f16_bits(io);
                unsigned char sc[12], qh[32], qs[128];
                if (read_block(io, sc, 12))     return PICKLE_ERR_IO;
                if (read_block(io, qh, 32))     return PICKLE_ERR_IO;
                if (read_block(io, qs, 128))    return PICKLE_ERR_IO;
                int is = 0;
                const unsigned char* ql_p = qs;
                uint8_t u1 = 1, u2 = 2;
                for (int j = 0; j < 256; j += 64) {
                    int sc0, m0, sc1, m1;
                    if (is + 0 < 4) {
                        sc0 = sc[is+0] & 0x3F; m0 = sc[is+0 + 4] & 0x3F;
                    } else {
                        sc0 = ((sc[is+0+4] & 0x0F) | ((sc[is+0-4] >> 6) << 4)) & 0x3F;
                        m0 = ((sc[is+0+4] >>  4) | ((sc[is+0-0] >> 6) << 4)) & 0x3F;
                    }
                    if (is + 1 < 4) {
                        sc1 = sc[is+1] & 0x3F; m1 = sc[is+1 + 4] & 0x3F;
                    } else {
                        sc1 = ((sc[is+1+4] & 0x0F) | ((sc[is+1-4] >> 6) << 4)) & 0x3F;
                        m1 = ((sc[is+1+4] >>  4) | ((sc[is+1-0] >> 6) << 4)) & 0x3F;
                    }
                    sfp_t d1 = sfp_mul(d, sfp_from_int(sc0));
                    sfp_t m1f = sfp_mul(dmin, sfp_from_int(m0));
                    sfp_t d2 = sfp_mul(d, sfp_from_int(sc1));
                    sfp_t m2f = sfp_mul(dmin, sfp_from_int(m1));
                    for (int l = 0; l < 32; l++) {
                        int q0 = (ql_p[l] & 0x0F) + ((qh[l] & u1) ? 16 : 0);
                        int q1 = (ql_p[l] >> 4)   + ((qh[l] & u2) ? 16 : 0);
                        out[b*256 + j + l]      = sfp_sub(sfp_mul(d1, sfp_from_int(q0)), m1f);
                        out[b*256 + j + l + 32] = sfp_sub(sfp_mul(d2, sfp_from_int(q1)), m2f);
                    }
                    ql_p += 32; is += 2;
                    u1 <<= 2; u2 <<= 2;
                }
            }
            return PICKLE_OK;
        }
        case GGML_Q4_K: {
            /* 144 bytes: d(f16,2) + dmin(f16,2) + scales[12] + qs[128]
             * GGML non-interleaved: 4 chunks of 64. qs advances 32/chunk.
             * Within a chunk, qs[l] low nibble = elem chunk*64+l,
             * high nibble = elem chunk*64+l+32. 8 scales via get_scale_min_k4. */
            uint64_t blocks = n / 256;
            for (uint64_t b = 0; b < blocks; b++) {
                sfp_t d    = read_f16_bits(io);
                sfp_t dmin = read_f16_bits(io);
                unsigned char sc[12], qs[128];
                if (read_block(io, sc, 12))     return PICKLE_ERR_IO;
                if (read_block(io, qs, 128))    return PICKLE_ERR_IO;
                int is = 0;
                const unsigned char* q = qs;
                for (int j = 0; j < 256; j += 64) {
                    int sc0, m0, sc1, m1;
                    if (is + 0 < 4) {
                        sc0 = sc[is+0] & 0x3F; m0 = sc[is+0 + 4] & 0x3F;
                    } else {
                        sc0 = ((sc[is+0+4] & 0x0F) | ((sc[is+0-4] >> 6) << 4)) & 0x3F;
                        m0 = ((sc[is+0+4] >>  4) | ((sc[is+0-0] >> 6) << 4)) & 0x3F;
                    }
                    if (is + 1 < 4) {
                        sc1 = sc[is+1] & 0x3F; m1 = sc[is+1 + 4] & 0x3F;
                    } else {
                        sc1 = ((sc[is+1+4] & 0x0F) | ((sc[is+1-4] >> 6) << 4)) & 0x3F;
                        m1 = ((sc[is+1+4] >>  4) | ((sc[is+1-0] >> 6) << 4)) & 0x3F;
                    }
                    sfp_t d1 = sfp_mul(d, sfp_from_int(sc0));
                    sfp_t m1f = sfp_mul(dmin, sfp_from_int(m0));
                    sfp_t d2 = sfp_mul(d, sfp_from_int(sc1));
                    sfp_t m2f = sfp_mul(dmin, sfp_from_int(m1));
                    for (int l = 0; l < 32; l++) {
                        int q0 = q[l] & 0x0F;
                        int q1 = q[l] >> 4;
                        out[b*256 + j + l]      = sfp_sub(sfp_mul(d1, sfp_from_int(q0)), m1f);
                        out[b*256 + j + l + 32] = sfp_sub(sfp_mul(d2, sfp_from_int(q1)), m2f);
                    }
                    q += 32; is += 2;
                }
            }
            return PICKLE_OK;
        }
        case GGML_Q3_K: {
            /* 110 bytes: hmask(32) + qs(64) + sc(12) + pad(2) */
            uint64_t blocks = n / 256;
            for (uint64_t b = 0; b < blocks; b++) {
                unsigned char hmask[32], qs[64], sc[12], pad[2];
                if (read_block(io, hmask, 32))   return PICKLE_ERR_IO;
                if (read_block(io, qs, 64))      return PICKLE_ERR_IO;
                if (read_block(io, sc, 12))      return PICKLE_ERR_IO;
                if (read_block(io, pad, 2))      return PICKLE_ERR_IO;
                uint16_t du = (uint16_t)sc[0] | ((uint16_t)sc[1] << 8);
                sfp_t d = f16_to_f32_bits(du);
                uint16_t dmin_u = (uint16_t)sc[2] | ((uint16_t)sc[3] << 8);
                sfp_t dmin = f16_to_f32_bits(dmin_u);
                int ks[8];
                for (int i = 0; i < 8; i++) {
                    int v = sc[4 + i] & 0x3F;
                    if (v & 0x20) v -= 64;
                    ks[i] = v;
                }
                for (int i = 0; i < 256; i++) {
                    int byte_idx = i / 4;
                    int bit_off  = 2 * (i & 3);
                    int q = (qs[byte_idx] >> bit_off) & 0x03;
                    if (q & 0x02) q -= 4;
                    if ((hmask[i / 8] >> (i % 8)) & 1) q += 4;
                    int sub = i / 32;
                    sfp_t sv = sfp_mul(d, sfp_mul(sfp_from_int(ks[sub]), sfp_from_int(q)));
                    out[b*256 + i] = sfp_sub(sv, dmin);
                }
            }
            return PICKLE_OK;
        }
        case GGML_Q2_K: {
            /* 84 bytes: d(f16,2) + dmin(f16,2) + scales(16) + qs(64) */
            uint64_t blocks = n / 256;
            for (uint64_t b = 0; b < blocks; b++) {
                sfp_t d    = read_f16_bits(io);
                sfp_t dmin = read_f16_bits(io);
                unsigned char sc_raw[16], qs[64];
                if (read_block(io, sc_raw, 16))  return PICKLE_ERR_IO;
                if (read_block(io, qs, 64))      return PICKLE_ERR_IO;
                int scales[32];
                for (int g = 0; g < 4; g++) {
                    uint32_t sw = (uint32_t)sc_raw[g*4]       |
                                  ((uint32_t)sc_raw[g*4+1] << 8) |
                                  ((uint32_t)sc_raw[g*4+2] << 16) |
                                  ((uint32_t)sc_raw[g*4+3] << 24);
                    for (int j = 0; j < 8; j++) {
                        int v = (sw >> (4*j)) & 0x0F;
                        if (v & 0x08) v -= 16;
                        scales[g*8 + j] = v;
                    }
                }
                for (int i = 0; i < 256; i++) {
                    int byte_idx = i / 4;
                    int bit_off  = 2 * (i & 3);
                    int q = (qs[byte_idx] >> bit_off) & 0x03;
                    int sub = i / 16;
                    sfp_t sv = sfp_mul(d, sfp_mul(sfp_from_int(scales[sub]), sfp_from_int(q)));
                    out[b*256 + i] = sfp_sub(sv, dmin);
                }
            }
            return PICKLE_OK;
        }
        case GGML_Q8_K: {
            /* 292 bytes: d(f32,4) + qs(256) + dsubs(16) + scales(16) */
            uint64_t blocks = n / 256;
            for (uint64_t b = 0; b < blocks; b++) {
                sfp_t d = read_f32_bits(io);
                signed char qs[256];
                if (read_block(io, qs, 256)) return PICKLE_ERR_IO;
                sfp_t dsubs[4], scales_v[4];
                for (int s = 0; s < 4; s++) dsubs[s] = read_f32_bits(io);
                for (int s = 0; s < 4; s++) scales_v[s] = read_f32_bits(io);
                for (int i = 0; i < 256; i++) {
                    int sub = i / 64;
                    sfp_t sub_d = sfp_mul(d, scales_v[sub]);
                    sfp_t sub_off = dsubs[sub];
                    out[b*256 + i] = sfp_add(sfp_mul(sub_d, sfp_from_int(qs[i])), sub_off);
                }
            }
            return PICKLE_OK;
        }

        default:
            return PICKLE_ERR_TYPE;
    }
}

/* ================================================================== */
/* Architecture detection                                             */
/* ================================================================== */
int pickle_arch_detect(const pickle_model_t* m, pickle_arch_t* arch) {
    if (!m || !arch) return PICKLE_ERR_ARG;
    kmemset(arch, 0, sizeof(*arch));
    const char* a = pickle_meta_string(m, "general.architecture");
    if (!a) return PICKLE_ERR_ARCH;
    size_t al = strlen(a);
    if (al >= sizeof(arch->arch_name)) al = sizeof(arch->arch_name) - 1;
    kmemcpy(arch->arch_name, a, al); arch->arch_name[al] = 0;

    arch->n_layers         = (int)pickle_meta_int(m, "llama.block_count",                  -1);
    arch->hidden_dim       = (int)pickle_meta_int(m, "llama.embedding_length",             -1);
    arch->intermediate_dim = (int)pickle_meta_int(m, "llama.feed_forward_length",          -1);
    arch->n_heads          = (int)pickle_meta_int(m, "llama.attention.head_count",         -1);
    arch->n_kv_heads       = (int)pickle_meta_int(m, "llama.attention.head_count_kv", arch->n_heads);
    arch->max_seq_len      = (int)pickle_meta_int(m, "llama.context_length",               2048);
    arch->rms_eps_bits         = pickle_meta_float_bits(m, "llama.attention.layer_norm_rms_epsilon", SFP_1E5);
    arch->rope_theta_bits      = pickle_meta_float_bits(m, "llama.rope.freq_base",                  SFP_10000);
    arch->rope_freq_scale_bits = pickle_meta_float_bits(m, "llama.rope.scaling.factor",             SFP_ONE);
    int rope_dim           = (int)pickle_meta_int(m, "llama.rope.dimension_count",          0);
    arch->head_dim         = rope_dim ? rope_dim : arch->hidden_dim / arch->n_heads;
    arch->tie_word_embeddings = (int)pickle_meta_int(m, "llama.tie_word_embeddings",        0);

    int64_t vs = pickle_meta_int(m, "llama.vocab_size", -1);
    if (vs <= 0) {
        /* Fallback: derive from token_embd.weight shape. In GGUF the
         * embedding tensor is stored as [hidden_dim, vocab_size] with
         * hidden_dim being ne[0] (the fast/contiguous dimension). So
         * vocab_size is the LAST dim, not dims[0]. */
        int ti = pickle_tensor_find(m, "token_embd.weight");
        if (ti < 0) return PICKLE_ERR_ARCH;
        uint32_t nd = m->tensors[ti].n_dims;
        if (nd == 0) return PICKLE_ERR_ARCH;
        vs = (int64_t)m->tensors[ti].dims[nd - 1];
    }
    arch->vocab_size = (int)vs;

    arch->norm_type = PICKLE_NORM_RMS;
    arch->act_type  = PICKLE_ACT_SILU;
    arch->rope_type = PICKLE_ROPE_NEOX;
    arch->attn_logit_softcapping_bits  = SFP_ZERO;
    arch->final_logit_softcapping_bits = SFP_ZERO;

    if (arch->n_layers <= 0 || arch->hidden_dim <= 0 ||
        arch->n_heads <= 0 || arch->intermediate_dim <= 0 ||
        arch->vocab_size <= 0) {
        return PICKLE_ERR_ARCH;
    }
    return PICKLE_OK;
}

/* ================================================================== */
/* KV cache                                                           */
/* ================================================================== */
int pickle_kv_alloc(const pickle_arch_t* arch, pickle_kv_cache_t* kv) {
    if (!arch || !kv) return PICKLE_ERR_ARG;
    kv->n_layers   = arch->n_layers;
    kv->n_kv_heads = arch->n_kv_heads;
    kv->head_dim   = arch->head_dim;
    kv->max_seq    = arch->max_seq_len;
    size_t per_slot = (size_t)(arch->n_kv_heads * arch->head_dim) * sizeof(sfp_t);
    size_t total_slots = (size_t)arch->n_layers * (size_t)arch->max_seq_len;
    char* base = (char*)palloc(total_slots * sizeof(pickle_kv_slot_t));
    if (!base) return PICKLE_ERR_MEMORY;
    kv->slots = (pickle_kv_slot_t*)base;
    for (size_t i = 0; i < total_slots; i++) {
        kv->slots[i].k = (float*)palloc(per_slot);
        kv->slots[i].v = (float*)palloc(per_slot);
        if (!kv->slots[i].k || !kv->slots[i].v) return PICKLE_ERR_MEMORY;
    }
    return PICKLE_OK;
}
void pickle_kv_free(pickle_kv_cache_t* kv) {
    if (!kv || !kv->slots) return;
    size_t total = (size_t)kv->n_layers * (size_t)kv->max_seq;
    for (size_t i = 0; i < total; i++) {
        if (kv->slots[i].k) pfree(kv->slots[i].k, (size_t)kv->n_kv_heads * kv->head_dim * sizeof(sfp_t));
        if (kv->slots[i].v) pfree(kv->slots[i].v, (size_t)kv->n_kv_heads * kv->head_dim * sizeof(sfp_t));
    }
    pfree(kv->slots, total * sizeof(pickle_kv_slot_t));
    kv->slots = 0;
}

/* ================================================================== */
/* Math helpers (all in sfp_t — NO C float arithmetic)               */
/* ================================================================== */
static void rms_norm(sfp_t* x, const sfp_t* w, int n, sfp_t eps) {
    sfp_t sum = SFP_ZERO;
    for (int i = 0; i < n; i++) sum = sfp_add(sum, sfp_mul(x[i], x[i]));
    sfp_t mean = sfp_div(sum, sfp_from_int(n));
    sfp_t denom = sfp_add(mean, eps);
    sfp_t rsq = sfp_rsqrt(denom);
    for (int i = 0; i < n; i++) x[i] = sfp_mul(x[i], sfp_mul(w[i], rsq));
}

static void matmul(sfp_t* y, const sfp_t* W, const sfp_t* x, int out_n, int in_n) {
    for (int o = 0; o < out_n; o++) {
        const sfp_t* wrow = W + (size_t)o * in_n;
        sfp_t acc = SFP_ZERO;
        for (int i = 0; i < in_n; i++) acc = sfp_add(acc, sfp_mul(wrow[i], x[i]));
        y[o] = acc;
    }
}

/* Apply RoPE (NeoX half-rotate). q is [n_heads*head_dim], k is [n_kv_heads*head_dim].
 * Computes freq_i = 1/theta^(2i/D), then rotates (q[i], q[i+half]) by angle=freq*pos. */
static void apply_rope(sfp_t* q, sfp_t* k, int n_heads, int n_kv_heads, int head_dim,
                       int pos, sfp_t theta) {
    int half = head_dim / 2;
    /* Precompute freq[i] = 1 / theta^(2i/D) for i in [0, half).
     * freq[0] = 1, freq[i] = freq[i-1] / sqrt(theta^2/D) ... simpler: freq[i] = exp(-ln(theta) * 2i/D).
     * For simplicity, compute via repeated division by theta. */
    sfp_t freqs[64];   /* max head_dim 128 */
    if (half > 64) half = 64;
    freqs[0] = SFP_ONE;
    for (int i = 1; i < half; i++) freqs[i] = sfp_div(freqs[i-1], theta);

    sfp_t pos_s = sfp_from_int(pos);
    for (int h = 0; h < n_heads; h++) {
        sfp_t* qh = q + h * head_dim;
        for (int i = 0; i < half; i++) {
            sfp_t angle = sfp_mul(freqs[i], pos_s);
            sfp_t s = sfp_sin(angle);
            sfp_t c = sfp_cos(angle);
            sfp_t q0 = qh[i];
            sfp_t q1 = qh[i + half];
            qh[i]        = sfp_sub(sfp_mul(c, q0), sfp_mul(s, q1));
            qh[i + half] = sfp_add(sfp_mul(s, q0), sfp_mul(c, q1));
        }
    }
    for (int h = 0; h < n_kv_heads; h++) {
        sfp_t* kh = k + h * head_dim;
        for (int i = 0; i < half; i++) {
            sfp_t angle = sfp_mul(freqs[i], pos_s);
            sfp_t s = sfp_sin(angle);
            sfp_t c = sfp_cos(angle);
            sfp_t k0 = kh[i];
            sfp_t k1 = kh[i + half];
            kh[i]        = sfp_sub(sfp_mul(c, k0), sfp_mul(s, k1));
            kh[i + half] = sfp_add(sfp_mul(s, k0), sfp_mul(c, k1));
        }
    }
}

/* Causal multi-head attention. Q [n_heads*head_dim]; K/V cached. */
static void attention(sfp_t* out, const sfp_t* Q,
                      const pickle_kv_cache_t* kv, int layer, size_t pos_start, size_t pos_end,
                      int n_heads, int n_kv_heads, int head_dim) {
    sfp_t scale = sfp_div(SFP_ONE, sfp_sqrt(sfp_from_int(head_dim)));
    sfp_t scores[512];
    size_t n = pos_end - pos_start;
    if (n > 512) n = 512;
    for (int h = 0; h < n_heads; h++) {
        const sfp_t* qh = Q + h * head_dim;
        int kv_head = h * n_kv_heads / n_heads;
        for (size_t p = 0; p < n; p++) {
            const sfp_t* k = (sfp_t*)kv->slots[layer * kv->max_seq + pos_start + p].k
                             + kv_head * head_dim;
            sfp_t dot = SFP_ZERO;
            for (int d = 0; d < head_dim; d++) dot = sfp_add(dot, sfp_mul(qh[d], k[d]));
            scores[p] = sfp_mul(dot, scale);
        }
        /* softmax */
        sfp_t maxv = scores[0];
        for (size_t p = 1; p < n; p++) if (sfp_gt(scores[p], maxv)) maxv = scores[p];
        sfp_t sume = SFP_ZERO;
        for (size_t p = 0; p < n; p++) {
            scores[p] = sfp_exp(sfp_sub(scores[p], maxv));
            sume = sfp_add(sume, scores[p]);
        }
        if (sfp_eq(sume, SFP_ZERO)) sume = SFP_ONE;
        for (size_t p = 0; p < n; p++) scores[p] = sfp_div(scores[p], sume);
        /* weighted sum of V */
        sfp_t* oh = out + h * head_dim;
        for (int d = 0; d < head_dim; d++) oh[d] = SFP_ZERO;
        for (size_t p = 0; p < n; p++) {
            const sfp_t* v = (sfp_t*)kv->slots[layer * kv->max_seq + pos_start + p].v
                             + kv_head * head_dim;
            sfp_t w = scores[p];
            for (int d = 0; d < head_dim; d++) oh[d] = sfp_add(oh[d], sfp_mul(w, v[d]));
        }
    }
}

/* Build a tensor name like "blk.5.attn_q.weight" without using sprintf. */
static void make_blk_name(char* buf, size_t cap, int L, const char* suffix) {
    size_t n = 0;
    const char* p = "blk.";
    while (*p && n < cap-1) buf[n++] = *p++;
    char num[16]; int nl = 0;
    int v = L;
    if (v == 0) num[nl++] = '0';
    else { char t[16]; int tl=0; while (v) { t[tl++] = '0' + (v % 10); v /= 10; } while (tl) num[nl++] = t[--tl]; }
    for (int i = 0; i < nl && n < cap-1; i++) buf[n++] = num[i];
    p = suffix;
    while (*p && n < cap-1) buf[n++] = *p++;
    buf[n] = 0;
}

/* ================================================================== */
/* Forward pass                                                       */
/* ================================================================== */
int pickle_forward(pickle_model_t* model, const pickle_arch_t* arch,
                   const int32_t* tokens, size_t n_tokens,
                   float* out_logits_f, pickle_kv_cache_t* kv, size_t* kv_pos) {
    if (!model || !arch || !tokens || !out_logits_f) return PICKLE_ERR_ARG;

    int H  = arch->n_heads;
    int HK = arch->n_kv_heads;
    int D  = arch->head_dim;
    int HD = arch->hidden_dim;
    int ID = arch->intermediate_dim;
    int VS = arch->vocab_size;
    int NL = arch->n_layers;
    sfp_t eps   = arch->rms_eps_bits;
    sfp_t theta = arch->rope_theta_bits;

    int t_emb  = pickle_tensor_find(model, "token_embd.weight");
    int t_norm = pickle_tensor_find(model, "output_norm.weight");
    int t_out  = pickle_tensor_find(model, "output.weight");
    if (t_emb < 0) return PICKLE_ERR_ARCH;

    sfp_t* emb_w = (sfp_t*)model->tensors[t_emb].data;
    sfp_t* out_logits = (sfp_t*)out_logits_f;

    sfp_t* x    = (sfp_t*)palloc((size_t)HD * sizeof(sfp_t));
    sfp_t* xn   = (sfp_t*)palloc((size_t)HD * sizeof(sfp_t));
    sfp_t* q    = (sfp_t*)palloc((size_t)(H  * D) * sizeof(sfp_t));
    sfp_t* k    = (sfp_t*)palloc((size_t)(HK * D) * sizeof(sfp_t));
    sfp_t* v    = (sfp_t*)palloc((size_t)(HK * D) * sizeof(sfp_t));
    sfp_t* ao   = (sfp_t*)palloc((size_t)(H  * D) * sizeof(sfp_t));
    sfp_t* aproj= (sfp_t*)palloc((size_t)HD * sizeof(sfp_t));
    sfp_t* xn2  = (sfp_t*)palloc((size_t)HD * sizeof(sfp_t));
    sfp_t* gate = (sfp_t*)palloc((size_t)ID * sizeof(sfp_t));
    sfp_t* up   = (sfp_t*)palloc((size_t)ID * sizeof(sfp_t));
    sfp_t* act  = (sfp_t*)palloc((size_t)ID * sizeof(sfp_t));
    sfp_t* down = (sfp_t*)palloc((size_t)HD * sizeof(sfp_t));
    if (!x || !xn || !q || !k || !v || !ao || !aproj || !xn2 ||
        !gate || !up || !act || !down) return PICKLE_ERR_MEMORY;

    size_t pos_start = kv_pos ? *kv_pos : 0;
    if (!kv) pos_start = 0;
    int has_kv = (kv != 0);

    for (size_t ti = 0; ti < n_tokens; ti++) {
        int32_t tok = tokens[ti];
        if (tok < 0 || tok >= VS) return PICKLE_ERR_RANGE;
        const sfp_t* emb_row = emb_w + (size_t)tok * HD;
        for (int i = 0; i < HD; i++) x[i] = emb_row[i];

        size_t pos = pos_start + ti;

        for (int L = 0; L < NL; L++) {
            char nm[128];

            make_blk_name(nm, sizeof(nm), L, ".attn_norm.weight");
            int t_an = pickle_tensor_find(model, nm);
            if (t_an < 0) return PICKLE_ERR_ARCH;

            for (int i = 0; i < HD; i++) xn[i] = x[i];
            rms_norm(xn, (sfp_t*)model->tensors[t_an].data, HD, eps);

            make_blk_name(nm, sizeof(nm), L, ".attn_q.weight");
            int t_q = pickle_tensor_find(model, nm);
            if (t_q < 0) return PICKLE_ERR_ARCH;
            matmul(q, (sfp_t*)model->tensors[t_q].data, xn, H * D, HD);

            make_blk_name(nm, sizeof(nm), L, ".attn_k.weight");
            int t_k = pickle_tensor_find(model, nm);
            if (t_k < 0) return PICKLE_ERR_ARCH;
            matmul(k, (sfp_t*)model->tensors[t_k].data, xn, HK * D, HD);

            make_blk_name(nm, sizeof(nm), L, ".attn_v.weight");
            int t_v = pickle_tensor_find(model, nm);
            if (t_v < 0) return PICKLE_ERR_ARCH;
            matmul(v, (sfp_t*)model->tensors[t_v].data, xn, HK * D, HD);

            apply_rope(q, k, H, HK, D, (int)pos, theta);

            if (has_kv && pos < (size_t)kv->max_seq) {
                size_t slot = (size_t)L * kv->max_seq + pos;
                sfp_t* kdst = (sfp_t*)kv->slots[slot].k;
                sfp_t* vdst = (sfp_t*)kv->slots[slot].v;
                for (int i = 0; i < HK * D; i++) { kdst[i] = k[i]; vdst[i] = v[i]; }
                attention(ao, q, kv, L, 0, pos + 1, H, HK, D);
            } else if (has_kv) {
                /* pos >= max_seq — can't write to KV; just attend to existing. */
                size_t attend_end = (size_t)kv->max_seq;
                attention(ao, q, kv, L, 0, attend_end, H, HK, D);
            } else {
                /* Stateless single-token: write to slot 0 and self-attend. */
                size_t slot = (size_t)L * kv->max_seq + 0;
                sfp_t* kdst = (sfp_t*)kv->slots[slot].k;
                sfp_t* vdst = (sfp_t*)kv->slots[slot].v;
                for (int i = 0; i < HK * D; i++) { kdst[i] = k[i]; vdst[i] = v[i]; }
                attention(ao, q, kv, L, 0, 1, H, HK, D);
            }

            make_blk_name(nm, sizeof(nm), L, ".attn_output.weight");
            int t_ao = pickle_tensor_find(model, nm);
            if (t_ao < 0) return PICKLE_ERR_ARCH;
            matmul(aproj, (sfp_t*)model->tensors[t_ao].data, ao, HD, H * D);

            for (int i = 0; i < HD; i++) x[i] = sfp_add(x[i], aproj[i]);

            make_blk_name(nm, sizeof(nm), L, ".ffn_norm.weight");
            int t_fn = pickle_tensor_find(model, nm);
            if (t_fn < 0) return PICKLE_ERR_ARCH;
            for (int i = 0; i < HD; i++) xn2[i] = x[i];
            rms_norm(xn2, (sfp_t*)model->tensors[t_fn].data, HD, eps);

            make_blk_name(nm, sizeof(nm), L, ".ffn_gate.weight");
            int t_g = pickle_tensor_find(model, nm);
            if (t_g < 0) return PICKLE_ERR_ARCH;
            matmul(gate, (sfp_t*)model->tensors[t_g].data, xn2, ID, HD);

            make_blk_name(nm, sizeof(nm), L, ".ffn_up.weight");
            int t_u = pickle_tensor_find(model, nm);
            if (t_u < 0) return PICKLE_ERR_ARCH;
            matmul(up, (sfp_t*)model->tensors[t_u].data, xn2, ID, HD);

            for (int i = 0; i < ID; i++)
                act[i] = sfp_mul(sfp_silu(gate[i]), up[i]);

            make_blk_name(nm, sizeof(nm), L, ".ffn_down.weight");
            int t_d = pickle_tensor_find(model, nm);
            if (t_d < 0) return PICKLE_ERR_ARCH;
            matmul(down, (sfp_t*)model->tensors[t_d].data, act, HD, ID);

            for (int i = 0; i < HD; i++) x[i] = sfp_add(x[i], down[i]);
        }

        if (ti == n_tokens - 1) {
            if (t_norm >= 0) rms_norm(x, (sfp_t*)model->tensors[t_norm].data, HD, eps);
            sfp_t* lm_w;
            if (arch->tie_word_embeddings || t_out < 0) lm_w = emb_w;
            else lm_w = (sfp_t*)model->tensors[t_out].data;
            matmul(out_logits, lm_w, x, VS, HD);
        }
    }

    if (kv_pos) *kv_pos = pos_start + n_tokens;

    pfree(x,    HD * sizeof(sfp_t));
    pfree(xn,   HD * sizeof(sfp_t));
    pfree(q,    H * D * sizeof(sfp_t));
    pfree(k,    HK * D * sizeof(sfp_t));
    pfree(v,    HK * D * sizeof(sfp_t));
    pfree(ao,   H * D * sizeof(sfp_t));
    pfree(aproj,HD * sizeof(sfp_t));
    pfree(xn2,  HD * sizeof(sfp_t));
    pfree(gate, ID * sizeof(sfp_t));
    pfree(up,   ID * sizeof(sfp_t));
    pfree(act,  ID * sizeof(sfp_t));
    pfree(down, HD * sizeof(sfp_t));

    return PICKLE_OK;
}

/* ================================================================== */
/* Sampling                                                           */
/* ================================================================== */
int32_t pickle_argmax(const float* logits_f, size_t n) {
    if (!logits_f || n == 0) return -1;
    const sfp_t* logits = (const sfp_t*)logits_f;
    int32_t best = 0;
    sfp_t bestv = logits[0];
    for (size_t i = 1; i < n; i++) {
        if (sfp_gt(logits[i], bestv)) { bestv = logits[i]; best = (int32_t)i; }
    }
    return best;
}
int32_t pickle_sample_greedy(const float* logits, size_t n) {
    return pickle_argmax(logits, n);
}

static uint32_t xs32_state = 0x12345678u;
static uint32_t xs32(void) {
    uint32_t x = xs32_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    xs32_state = x;
    return x;
}

int32_t pickle_sample_temperature_bits(float* logits_f, size_t n, sfp_t temp, sfp_t top_p, uint32_t seed) {
    if (!logits_f || n == 0) return -1;
    sfp_t* logits = (sfp_t*)logits_f;
    xs32_state = seed ? seed : 0x12345678u;
    if (sfp_le(temp, SFP_ZERO)) return pickle_argmax(logits_f, n);
    for (size_t i = 0; i < n; i++) logits[i] = sfp_div(logits[i], temp);
    sfp_t maxv = logits[0];
    for (size_t i = 1; i < n; i++) if (sfp_gt(logits[i], maxv)) maxv = logits[i];
    sfp_t sum = SFP_ZERO;
    for (size_t i = 0; i < n; i++) {
        logits[i] = sfp_exp(sfp_sub(logits[i], maxv));
        sum = sfp_add(sum, logits[i]);
    }
    if (sfp_eq(sum, SFP_ZERO)) return 0;
    for (size_t i = 0; i < n; i++) logits[i] = sfp_div(logits[i], sum);
    (void)top_p;
    uint32_t r = xs32();
    sfp_t thr = sfp_div(sfp_from_int((int32_t)(r & 0x7FFFFFFF)), sfp_from_int(0x7FFFFFFF));
    sfp_t acc = SFP_ZERO;
    for (size_t i = 0; i < n; i++) {
        acc = sfp_add(acc, logits[i]);
        if (sfp_ge(acc, thr)) return (int32_t)i;
    }
    return (int32_t)(n - 1);
}
#ifndef PICKLE_KERNEL
int32_t pickle_sample_temperature(float* logits, size_t n, float temp, float top_p, uint32_t seed) {
    return pickle_sample_temperature_bits(logits, n, sfp_from_float(temp), sfp_from_float(top_p), seed);
}
#endif

/* ================================================================== */
/* Self-test                                                          */
/* ================================================================== */
typedef struct { const unsigned char* p; size_t len; size_t off; } mem_ctx_t;
static size_t mem_read(void* ctx, void* buf, size_t len) {
    mem_ctx_t* m = (mem_ctx_t*)ctx;
    if (m->off + len > m->len) len = m->len - m->off;
    kmemcpy(buf, m->p + m->off, len);
    m->off += len;
    return len;
}
static int mem_seek(void* ctx, int64_t offset, int whence) {
    mem_ctx_t* m = (mem_ctx_t*)ctx;
    int64_t no;
    if (whence == PICKLE_SEEK_SET) no = offset;
    else if (whence == PICKLE_SEEK_CUR) no = (int64_t)m->off + offset;
    else if (whence == PICKLE_SEEK_END) no = (int64_t)m->len + offset;
    else return -1;
    if (no < 0) no = 0;
    if ((size_t)no > m->len) no = (int64_t)m->len;
    m->off = (size_t)no;
    return 0;
}
static int64_t mem_tell(void* ctx) { return (int64_t)((mem_ctx_t*)ctx)->off; }

int pickle_selftest(int32_t* out_token) {
    if (out_token) *out_token = -1;
    if (!g_alloc.alloc) pickle_set_alloc(0);

    mem_ctx_t ctx;
    ctx.p = pickle_demo_gguf;
    ctx.len = pickle_demo_gguf_len;
    ctx.off = 0;

    pickle_io_t io;
    io.ctx = &ctx;
    io.read = mem_read;
    io.seek = mem_seek;
    io.tell = mem_tell;

    pickle_model_t* m = 0;
    int rc = pickle_load(&io, &m);
    if (rc != PICKLE_OK) {
        pr_info("pickle: selftest load failed rc=%d\n", rc);
        return rc;
    }
    pickle_arch_t arch;
    rc = pickle_arch_detect(m, &arch);
    if (rc != PICKLE_OK) {
        pr_info("pickle: selftest arch_detect failed rc=%d\n", rc);
        pickle_free(m);
        return rc;
    }
    pr_info("pickle: selftest arch=%s L=%d H=%d HK=%d D=%d HD=%d VS=%d\n",
            arch.arch_name, arch.n_layers, arch.n_heads, arch.n_kv_heads,
            arch.head_dim, arch.hidden_dim, arch.vocab_size);

    pickle_kv_cache_t kv;
    rc = pickle_kv_alloc(&arch, &kv);
    if (rc != PICKLE_OK) { pickle_free(m); return rc; }

    int32_t prompt[3] = { 1, 5, 3 };
    float* logits = (float*)palloc((size_t)arch.vocab_size * sizeof(sfp_t));
    if (!logits) { pickle_kv_free(&kv); pickle_free(m); return PICKLE_ERR_MEMORY; }

    size_t kv_pos = 0;
    rc = pickle_forward(m, &arch, prompt, 3, logits, &kv, &kv_pos);
    if (rc != PICKLE_OK) {
        pr_info("pickle: selftest forward failed rc=%d\n", rc);
        pfree(logits, (size_t)arch.vocab_size * sizeof(sfp_t));
        pickle_kv_free(&kv);
        pickle_free(m);
        return rc;
    }

    int32_t tok = pickle_argmax(logits, (size_t)arch.vocab_size);
    if (out_token) *out_token = tok;
    pr_info("pickle: selftest OK, next token = %d\n", (int)tok);

    pfree(logits, (size_t)arch.vocab_size * sizeof(sfp_t));
    pickle_kv_free(&kv);
    pickle_free(m);
    return PICKLE_OK;
}
