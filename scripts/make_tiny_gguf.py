#!/usr/bin/env python3
"""
Generate pickle_demo.gguf — a tiny Llama-architecture GGUF v3 file with
F32 weights — and emit pickle_demo_gguf.c with the binary embedded as a
const unsigned char array. Used by pickle_selftest() so the kernel can
verify the GGUF parse + forward pass at boot without any FS access.

Tensor shapes match what pickle.c expects:
    token_embd.weight        [vocab, hidden]
    blk.N.attn_norm.weight   [hidden]
    blk.N.attn_q.weight      [n_heads*head_dim,    hidden]
    blk.N.attn_k.weight      [n_kv_heads*head_dim, hidden]
    blk.N.attn_v.weight      [n_kv_heads*head_dim, hidden]
    blk.N.attn_output.weight [hidden, n_heads*head_dim]
    blk.N.ffn_norm.weight    [hidden]
    blk.N.ffn_gate.weight    [intermediate, hidden]
    blk.N.ffn_up.weight      [intermediate, hidden]
    blk.N.ffn_down.weight    [hidden, intermediate]
    output_norm.weight       [hidden]
    output.weight            [vocab, hidden]

Run: python3 scripts/make_tiny_gguf.py
Outputs:
    /tmp/pickle_demo.gguf                      (raw GGUF v3)
    kernel/ai/pickle_demo_gguf.c               (embedded C array)
"""
import os
import struct
import sys

# ---- model config ---------------------------------------------------
HIDDEN        = 8
N_LAYERS      = 1
N_HEADS       = 2
N_KV_HEADS    = 1
HEAD_DIM      = HIDDEN // N_HEADS          # 4
INTERMEDIATE  = 16                          # 2x hidden
VOCAB         = 16
CTX           = 16
ALIGN         = 32

# GGML type id
F32 = 0

# ---- deterministic LCG for reproducible "weights" -------------------
def lcg(seed=0xCAFEBABE):
    state = seed
    while True:
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        # Map to [-0.5, 0.5]
        yield (state / float(0x7FFFFFFF)) - 0.5

gen = lcg()

# ---- tensor table ---------------------------------------------------
tensors = [
    ("token_embd.weight",        [VOCAB, HIDDEN],                F32),
    ("blk.0.attn_norm.weight",   [HIDDEN],                       F32),
    ("blk.0.attn_q.weight",      [N_HEADS*HEAD_DIM,    HIDDEN],  F32),
    ("blk.0.attn_k.weight",      [N_KV_HEADS*HEAD_DIM, HIDDEN],  F32),
    ("blk.0.attn_v.weight",      [N_KV_HEADS*HEAD_DIM, HIDDEN],  F32),
    ("blk.0.attn_output.weight", [HIDDEN, N_HEADS*HEAD_DIM],     F32),
    ("blk.0.ffn_norm.weight",    [HIDDEN],                       F32),
    ("blk.0.ffn_gate.weight",    [INTERMEDIATE, HIDDEN],         F32),
    ("blk.0.ffn_up.weight",      [INTERMEDIATE, HIDDEN],         F32),
    ("blk.0.ffn_down.weight",    [HIDDEN, INTERMEDIATE],         F32),
    ("output_norm.weight",       [HIDDEN],                       F32),
    ("output.weight",            [VOCAB, HIDDEN],                F32),
]

# Pre-generate all weights
all_weights = []
for name, dims, typ in tensors:
    n = 1
    for d in dims:
        n *= d
    vals = [next(gen) for _ in range(n)]
    all_weights.append((name, dims, typ, vals))

# ---- writer helpers -------------------------------------------------
def write_str(f, s):
    b = s.encode('utf-8')
    f.write(struct.pack('<Q', len(b)))
    f.write(b)

def write_kv_str(f, key, val):
    write_str(f, key)
    f.write(struct.pack('<I', 8))   # STRING
    write_str(f, val)

def write_kv_u32(f, key, val):
    write_str(f, key)
    f.write(struct.pack('<I', 4))   # UINT32
    f.write(struct.pack('<I', val))

def write_kv_f32(f, key, val):
    write_str(f, key)
    f.write(struct.pack('<I', 6))   # FLOAT32
    f.write(struct.pack('<f', val))

def write_kv_bool(f, key, val):
    write_str(f, key)
    f.write(struct.pack('<I', 7))   # BOOL
    f.write(struct.pack('<B', 1 if val else 0))

# ---- compute data offsets ------------------------------------------
data_offsets = []
cur = 0
for name, dims, typ, vals in all_weights:
    data_offsets.append(cur)
    sz = len(vals) * 4  # F32
    cur += sz
    mod = cur % ALIGN
    if mod != 0:
        cur += ALIGN - mod

# ---- write GGUF -----------------------------------------------------
out_gguf = '/tmp/pickle_demo.gguf'
with open(out_gguf, 'wb') as f:
    # Header
    f.write(struct.pack('<I', 0x46554747))   # magic
    f.write(struct.pack('<I', 3))             # version 3
    f.write(struct.pack('<Q', len(tensors)))  # tensor_count
    f.write(struct.pack('<Q', 13))            # kv_count

    # Metadata
    write_kv_str (f, "general.architecture",                       "llama")
    write_kv_str (f, "general.name",                               "pickle-selftest-tiny")
    write_kv_u32 (f, "general.file_type",                          0)
    write_kv_u32 (f, "llama.block_count",                          N_LAYERS)
    write_kv_u32 (f, "llama.embedding_length",                     HIDDEN)
    write_kv_u32 (f, "llama.feed_forward_length",                  INTERMEDIATE)
    write_kv_u32 (f, "llama.attention.head_count",                 N_HEADS)
    write_kv_u32 (f, "llama.attention.head_count_kv",              N_KV_HEADS)
    write_kv_u32 (f, "llama.context_length",                       CTX)
    write_kv_u32 (f, "llama.rope.dimension_count",                 HEAD_DIM)
    write_kv_f32 (f, "llama.attention.layer_norm_rms_epsilon",     1e-5)
    write_kv_f32 (f, "llama.rope.freq_base",                       10000.0)
    write_kv_bool(f, "llama.tie_word_embeddings",                  False)

    # Tensor info
    for i, (name, dims, typ, vals) in enumerate(all_weights):
        write_str(f, name)
        f.write(struct.pack('<I', len(dims)))
        for d in dims:
            f.write(struct.pack('<Q', d))
        f.write(struct.pack('<I', typ))
        f.write(struct.pack('<Q', data_offsets[i]))

    # Align tensor data section
    pos = f.tell()
    mod = pos % ALIGN
    if mod != 0:
        f.write(b'\x00' * (ALIGN - mod))

    # Tensor data
    for i, (name, dims, typ, vals) in enumerate(all_weights):
        # If there's a gap from the previous tensor's padding, write zeros
        for v in vals:
            f.write(struct.pack('<f', v))
        # Pad to ALIGN
        pos = f.tell()
        mod = pos % ALIGN
        if mod != 0:
            f.write(b'\x00' * (ALIGN - mod))

size = os.path.getsize(out_gguf)
print(f"Wrote {out_gguf} ({size} bytes)")

# ---- emit C array ---------------------------------------------------
out_c = sys.argv[1] if len(sys.argv) > 1 else 'kernel/ai/pickle_demo_gguf.c'
with open(out_gguf, 'rb') as g:
    blob = g.read()

with open(out_c, 'w') as c:
    c.write("/*\n")
    c.write(" * Auto-generated by scripts/make_tiny_gguf.py — DO NOT EDIT.\n")
    c.write(" * Tiny Llama-arch GGUF v3 (F32, 1 layer, 8-dim, 16-vocab) for pickle_selftest.\n")
    c.write(" */\n")
    c.write("#ifdef PICKLE_KERNEL\n")
    c.write("#include <lestra/types.h>\n")
    c.write("#else\n")
    c.write("#include <stdint.h>\n")
    c.write("#endif\n")
    c.write("#include <lestra/pickle.h>\n\n")
    c.write("const unsigned char pickle_demo_gguf[] = {\n")
    for i in range(0, len(blob), 12):
        chunk = blob[i:i+12]
        c.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
    c.write("};\n")
    c.write(f"const unsigned int pickle_demo_gguf_len = {len(blob)};\n")
print(f"Wrote {out_c} ({len(blob)} bytes embedded)")
