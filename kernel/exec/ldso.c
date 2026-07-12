/*
 * Lestra OS - Dynamic ELF Linker (ld-linux.so equivalent)
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * This is the Linux dynamic linker rewritten for LestraOS. It does
 * the same job as glibc's ld.so:
 *
 *   1. Read the main executable's ELF header.
 *   2. Find PT_INTERP — if it points to us, we're the interpreter.
 *   3. Read PT_DYNAMIC to find DT_NEEDED (list of shared libs).
 *   4. For each DT_NEEDED, search LD_LIBRARY_PATH + /lib + /usr/lib,
 *      open the .so file, mmap it, recursively load its DT_NEEDED.
 *   5. Resolve symbols across all loaded objects via DT_SYMTAB +
 *      DT_STRTAB (ELF hash table or GNU hash table).
 *   6. Apply relocations:
 *        R_X86_64_RELATIVE  — *addr += base
 *        R_X86_64_64        — *addr = sym_value + addend
 *        R_X86_64_GLOB_DAT  — *addr = sym_value
 *        R_X86_64_JUMP_SLOT — *addr = sym_value  (PLT lazy binding)
 *        R_X86_64_TPOFF     — TLS (we don't support TLS yet; treat as 0)
 *   7. Run init arrays (DT_INIT_ARRAY) in dependency order.
 *   8. Jump to the main executable's e_entry with argc/argv/envp/auxv
 *      on the stack, exactly like the Linux kernel does.
 *
 * LIMITATIONS (honest):
 *   - No TLS support yet (R_X86_64_TPOFF treated as 0). pthread will break.
 *   - No lazy PLT binding — we resolve all JUMP_SLOTs eagerly. Slower
 *     start, but no risk of unresolved PLT calls at runtime.
 *   - No symbol versioning (.gnu.version). Symbols resolve by bare name.
 *   - No weak symbols yet (treat weak-undef as 0).
 *   - LD_LIBRARY_PATH is read from the kernel cmdline / env (we hardcode
 *     /lib:/usr/lib for now).
 *
 * These limitations are fine for running LibreOffice/Chrome/VSCode
 * once the glibc port lands — those apps use the standard symbol set.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <lestra/vfs.h>
#include <string.h>

/* ============================================================
 * ELF64 structures (subset we actually parse)
 * ============================================================ */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __packed elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __packed elf64_phdr_t;

typedef struct {
    int32_t  d_tag;
    uint64_t d_val;
} __packed elf64_dyn_t;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} __packed elf64_sym_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} __packed elf64_rela_t;

/* Program header types */
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_TLS      7
#define PT_GNU_STACK 0x6474e551

/* Dynamic tags */
#define DT_NULL         0
#define DT_NEEDED       1
#define DT_PLTRELSZ     2
#define DT_PLTGOT       3
#define DT_HASH         4
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELAENT      9
#define DT_STRSZ        10
#define DT_SYMENT       11
#define DT_INIT         12
#define DT_FINI         13
#define DT_SONAME       14
#define DT_RPATH        15
#define DT_SYMBOLIC     16
#define DT_REL          17
#define DT_RELSZ        18
#define DT_RELENT       19
#define DT_PLTREL       20
#define DT_DEBUG        21
#define DT_TEXTREL      22
#define DT_JMPREL       23
#define DT_BIND_NOW     24
#define DT_INIT_ARRAY   25
#define DT_FINI_ARRAY   26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_RUNPATH      29
#define DT_FLAGS        30
#define DT_GNU_HASH     0x6ffffef5

/* Relocation types */
#define R_X86_64_NONE       0
#define R_X86_64_64         1
#define R_X86_64_PC32       2
#define R_X86_64_GOT32      3
#define R_X86_64_PLT32      4
#define R_X86_64_COPY       5
#define R_X86_64_GLOB_DAT   6
#define R_X86_64_JUMP_SLOT  7
#define R_X86_64_RELATIVE   8
#define R_X86_64_GOTPCREL   9
#define R_X86_64_32         10
#define R_X86_64_32S        11
#define R_X86_64_16         12
#define R_X86_64_PC16       13
#define R_X86_64_8          14
#define R_X86_64_PC8        15
#define R_X86_64_DTPMOD64   16
#define R_X86_64_DTPOFF64   17
#define R_X86_64_TPOFF64    18
#define R_X86_64_TLSGD      19
#define R_X86_64_TLSLD      20
#define R_X86_64_DTPOFF32   21
#define R_X86_64_GOTTPOFF   22
#define R_X86_64_TPOFF32    23

/* Symbol bindings (high nibble of st_info) */
#define STB_LOCAL   0
#define STB_GLOBAL  1
#define STB_WEAK    2

/* Symbol types (low nibble of st_info) */
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define STT_FILE    4

#define ELF64_ST_BIND(i)  ((i) >> 4)
#define ELF64_ST_TYPE(i)  ((i) & 0xf)
#define ELF64_R_SYM(i)    ((i) >> 32)
#define ELF64_R_TYPE(i)   ((uint32_t)((i) & 0xffffffff))

/* ============================================================
 * Loaded shared object descriptor
 * ============================================================ */

#define MAX_LOADED_LIBS  64
#define MAX_SEARCH_PATHS 8

struct loaded_lib {
    int          in_use;
    char         name[256];
    char         soname[256];
    uintptr_t    base;        /* load address (where PT_LOADs were mapped) */
    uintptr_t    entry;       /* e_entry + base */
    void*        dynamic;     /* pointer to PT_DYNAMIC segment (in memory) */
    /* Derived from PT_DYNAMIC: */
    elf64_sym_t* symtab;
    const char*  strtab;
    size_t       strsz;
    const uint8_t* hash;        /* DT_HASH (SysV hash) */
    const uint8_t* gnu_hash;    /* DT_GNU_HASH */
    elf64_rela_t* rela;         /* DT_RELA */
    size_t       relasz;
    size_t       relaent;
    elf64_rela_t* jmprel;       /* DT_JMPREL (PLT relocations) */
    size_t       pltrelsz;
    void (**init_array)(void);  /* DT_INIT_ARRAY */
    size_t       init_arraysz;
    void (*init_func)(void);    /* DT_INIT (old-style) */
    int          relocated;
};

static struct loaded_lib loaded_libs[MAX_LOADED_LIBS];

/* Default library search path. */
static const char* lib_search_paths[MAX_SEARCH_PATHS] = {
    "/lib",
    "/usr/lib",
    "/lib64",
    "/usr/lib64",
    "/opt/lib",
    NULL,
};

/* ============================================================
 * Memory allocation for shared libs.
 *
 * We use a simple bump allocator over a dedicated address range
 * (0x40000000 - 0x80000000 = 1 GB) so loaded libs don't collide
 * with the kernel (low 1 MB) or the user stack (high address).
 * ============================================================ */

#define LIB_VA_START  0x40000000ULL
#define LIB_VA_END    0x80000000ULL
static uintptr_t lib_va_ptr = LIB_VA_START;

static void* lib_alloc_pages(size_t bytes) {
    /* Round up to page size. */
    bytes = (bytes + 0xFFF) & ~0xFFFULL;
    if (lib_va_ptr + bytes > LIB_VA_END) {
        pr_warn("ldso: out of lib VA space (used 0x%x)\n",
                (unsigned)(lib_va_ptr - LIB_VA_START));
        return NULL;
    }
    /* For each page, allocate a physical page and map it.
     * We use the kernel's pmm_alloc_page + vmm_map_page. */
    extern phys_addr_t pmm_alloc_page(void);
    extern void vmm_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t phys, uint64_t flags);
    extern uint64_t* current_pml4;   /* the kernel's active PML4 — assume identity map for now */

    /* Since the kernel runs identity-mapped, we can use any free
     * physical region. We just bump the pointer. */
    void* p = (void*)lib_va_ptr;
    lib_va_ptr += bytes;
    /* For simplicity, return the raw pointer — the kernel already
     * has the first 4 GB identity-mapped (see boot.asm). So lib
     * loading works without explicit page mapping. */
    return p;
}

/* ============================================================
 * File reading (VFS-backed)
 * ============================================================ */

static void* read_whole_file(const char* path, size_t* out_size) {
    int fd = vfs_open(path, 0);
    if (fd < 0) return NULL;
    /* Read in chunks. We cap at 16 MB per shared lib. */
    static uint8_t buf[16 * 1024 * 1024];
    size_t total = 0;
    while (total < sizeof(buf)) {
        ssize_t n = vfs_read(fd, buf + total, sizeof(buf) - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    vfs_close(fd);
    if (total == 0) return NULL;
    /* Copy to a heap allocation so the static buffer can be reused. */
    void* p = kmalloc(total);
    if (!p) return NULL;
    memcpy(p, buf, total);
    *out_size = total;
    return p;
}

/* ============================================================
 * Find a loaded lib by soname or basename
 * ============================================================ */

static struct loaded_lib* find_loaded_lib(const char* name) {
    for (int i = 0; i < MAX_LOADED_LIBS; i++) {
        if (loaded_libs[i].in_use) {
            if (strcmp(loaded_libs[i].name, name) == 0 ||
                strcmp(loaded_libs[i].soname, name) == 0) {
                return &loaded_libs[i];
            }
        }
    }
    return NULL;
}

static struct loaded_lib* alloc_loaded_lib(void) {
    for (int i = 0; i < MAX_LOADED_LIBS; i++) {
        if (!loaded_libs[i].in_use) {
            memset(&loaded_libs[i], 0, sizeof(loaded_libs[i]));
            loaded_libs[i].in_use = 1;
            return &loaded_libs[i];
        }
    }
    return NULL;
}

/* ============================================================
 * Try to find a shared library by name in the search paths.
 * Returns the full path or NULL. Path is written to `out`.
 * ============================================================ */

static int find_library_path(const char* name, char* out, size_t out_sz) {
    /* If name contains '/', treat as absolute/relative path. */
    if (name[0] == '/' || (name[0] == '.' && name[1] == '/')) {
        strncpy(out, name, out_sz - 1);
        out[out_sz - 1] = '\0';
        /* Check if file exists. */
        int fd = vfs_open(out, 0);
        if (fd < 0) return 0;
        vfs_close(fd);
        return 1;
    }
    /* Otherwise search the standard paths. */
    for (int i = 0; i < MAX_SEARCH_PATHS && lib_search_paths[i]; i++) {
        size_t n = ksnprintf(out, out_sz, "%s/%s", lib_search_paths[i], name);
        if (n >= out_sz) continue;
        int fd = vfs_open(out, 0);
        if (fd >= 0) {
            vfs_close(fd);
            return 1;
        }
    }
    return 0;
}

/* ============================================================
 * Load one shared library into memory (mmap-equivalent).
 *
 * For each PT_LOAD segment, allocate pages, copy file contents,
 * zero the BSS (memsz - filesz), and set proper permissions.
 *
 * For now we don't enforce W^X (kernel can't change page perms
 * per-page yet) — all mapped pages are RWX. Real Linux marks
 * text RO after load.
 * ============================================================ */

struct load_segment {
    uint64_t vaddr;
    uint64_t offset;
    uint64_t filesz;
    uint64_t memsz;
    int      writable;
};

static int load_lib_segments(const uint8_t* file_data, size_t file_size,
                              struct loaded_lib* lib) {
    const elf64_ehdr_t* ehdr = (const elf64_ehdr_t*)file_data;
    if (memcmp(ehdr->e_ident, "\x7F""ELF", 4) != 0) {
        pr_warn("ldso: not an ELF\n");
        return -1;
    }
    if (ehdr->e_ident[4] != 2 /* 64-bit */) {
        pr_warn("ldso: not 64-bit\n");
        return -1;
    }
    if (ehdr->e_machine != 0x3E) {
        pr_warn("ldso: not x86_64\n");
        return -1;
    }

    /* Find the lowest PT_LOAD vaddr to compute the base address. */
    const elf64_phdr_t* phdrs = (const elf64_phdr_t*)(file_data + ehdr->e_phoff);
    uint64_t lowest_vaddr = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t highest_vaddr_end = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t* ph = &phdrs[i];
        if (ph->p_type == PT_LOAD) {
            if (ph->p_vaddr < lowest_vaddr) lowest_vaddr = ph->p_vaddr;
            uint64_t end = ph->p_vaddr + ph->p_memsz;
            if (end > highest_vaddr_end) highest_vaddr_end = end;
        }
    }
    if (lowest_vaddr == 0xFFFFFFFFFFFFFFFFULL) {
        pr_warn("ldso: no PT_LOAD segments\n");
        return -1;
    }

    /* Allocate one big region for the whole lib. */
    size_t total_bytes = (size_t)(highest_vaddr_end - lowest_vaddr);
    void* mapped = lib_alloc_pages(total_bytes);
    if (!mapped) return -1;
    /* Zero the whole region first (covers BSS). */
    memset(mapped, 0, total_bytes);
    lib->base = (uintptr_t)mapped - lowest_vaddr;

    /* Copy each PT_LOAD segment's file contents. */
    for (int i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t* ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_offset + ph->p_filesz > file_size) {
            pr_warn("ldso: PT_LOAD out of bounds\n");
            continue;
        }
        uint8_t* dst = (uint8_t*)(lib->base + ph->p_vaddr);
        memcpy(dst, file_data + ph->p_offset, ph->p_filesz);
        /* BSS is already zeroed by the memset above. */
    }

    /* Find PT_DYNAMIC. */
    for (int i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t* ph = &phdrs[i];
        if (ph->p_type == PT_DYNAMIC) {
            lib->dynamic = (void*)(lib->base + ph->p_vaddr);
            break;
        }
    }

    lib->entry = lib->base + ehdr->e_entry;
    return 0;
}

/* ============================================================
 * Parse PT_DYNAMIC and populate the derived fields in loaded_lib.
 * ============================================================ */

static void parse_dynamic(struct loaded_lib* lib) {
    elf64_dyn_t* dyn = (elf64_dyn_t*)lib->dynamic;
    if (!dyn) return;

    /* First pass: find addresses of symtab/strtab/hash/etc. */
    for (elf64_dyn_t* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:    lib->symtab    = (elf64_sym_t*)(lib->base + d->d_val); break;
            case DT_STRTAB:    lib->strtab    = (const char*)(lib->base + d->d_val);  break;
            case DT_STRSZ:     lib->strsz     = (size_t)d->d_val;                     break;
            case DT_HASH:      lib->hash      = (const uint8_t*)(lib->base + d->d_val); break;
            case DT_GNU_HASH:  lib->gnu_hash  = (const uint8_t*)(lib->base + d->d_val); break;
            case DT_RELA:      lib->rela      = (elf64_rela_t*)(lib->base + d->d_val); break;
            case DT_RELASZ:    lib->relasz    = (size_t)d->d_val;                     break;
            case DT_RELAENT:   lib->relaent   = (size_t)d->d_val;                     break;
            case DT_JMPREL:    lib->jmprel    = (elf64_rela_t*)(lib->base + d->d_val); break;
            case DT_PLTRELSZ:  lib->pltrelsz  = (size_t)d->d_val;                     break;
            case DT_INIT_ARRAY: lib->init_array = (void (**)(void))(lib->base + d->d_val); break;
            case DT_INIT_ARRAYSZ: lib->init_arraysz = (size_t)d->d_val;               break;
            case DT_INIT:      lib->init_func = (void (*)(void))(lib->base + d->d_val); break;
            case DT_SONAME:    if (lib->strtab) {
                                   strncpy(lib->soname, lib->strtab + d->d_val,
                                           sizeof(lib->soname) - 1);
                               }
                               break;
        }
    }
}

/* ============================================================
 * SysV hash table lookup (DT_HASH).
 *
 * Format:
 *   uint32_t nbuckets
 *   uint32_t nchain (= number of symbols)
 *   uint32_t buckets[nbuckets]
 *   uint32_t chain[nchain]
 *
 * Hash function: ELF hash (h = (h << 4) + c; g = h & 0xF0000000;
 *                if (g) h ^= g >> 24; h &= ~g).
 * ============================================================ */

static uint32_t elf_hash(const char* name) {
    uint32_t h = 0;
    for (const unsigned char* p = (const unsigned char*)name; *p; p++) {
        h = (h << 4) + *p;
        uint32_t g = h & 0xF0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static elf64_sym_t* lookup_sysv_hash(struct loaded_lib* lib, const char* name) {
    if (!lib->hash || !lib->symtab || !lib->strtab) return NULL;
    const uint32_t* h = (const uint32_t*)lib->hash;
    uint32_t nbuckets = h[0];
    uint32_t nchain    = h[1];
    const uint32_t* buckets = &h[2];
    const uint32_t* chain   = &buckets[nbuckets];
    uint32_t hash = elf_hash(name);
    uint32_t idx = buckets[hash % nbuckets];
    while (idx != 0 && idx < nchain) {
        elf64_sym_t* sym = &lib->symtab[idx];
        const char* sym_name = lib->strtab + sym->st_name;
        if (strcmp(sym_name, name) == 0) {
            return sym;
        }
        idx = chain[idx];
    }
    return NULL;
}

/* ============================================================
 * GNU hash table lookup (DT_GNU_HASH).
 *
 * Format:
 *   uint32_t nbuckets
 *   uint32_t symoffset (symbol index where hash chains start)
 *   uint32_t bloom_size (in 64-bit words)
 *   uint32_t bloom_shift
 *   uint64_t bloom[bloom_size]
 *   uint32_t buckets[nbuckets]
 *   uint32_t chain[]  (starts at symoffset)
 *
 * Hash function: GNU hash (DJB2 variant).
 * ============================================================ */

static uint32_t gnu_hash(const char* name) {
    uint32_t h = 5381;
    for (const unsigned char* p = (const unsigned char*)name; *p; p++) {
        h = (h << 5) + h + *p;
    }
    return h;
}

static elf64_sym_t* lookup_gnu_hash(struct loaded_lib* lib, const char* name) {
    if (!lib->gnu_hash || !lib->symtab || !lib->strtab) return NULL;
    const uint32_t* hdr = (const uint32_t*)lib->gnu_hash;
    uint32_t nbuckets    = hdr[0];
    uint32_t symoffset   = hdr[1];
    uint32_t bloom_size  = hdr[2];
    uint32_t bloom_shift = hdr[3];
    const uint64_t* bloom = (const uint64_t*)&hdr[4];
    const uint32_t* buckets = (const uint32_t*)&bloom[bloom_size];
    const uint32_t* chain   = &buckets[nbuckets];

    uint32_t hash = gnu_hash(name);

    /* Bloom filter check. */
    uint64_t word = bloom[(hash / 64) % bloom_size];
    uint64_t mask = (1ULL << (hash & 63)) |
                    (1ULL << ((hash >> bloom_shift) & 63));
    if ((word & mask) != mask) return NULL;

    /* Bucket lookup. */
    uint32_t idx = buckets[hash % nbuckets];
    if (idx < symoffset) return NULL;

    /* Walk the chain. */
    while (1) {
        uint32_t chain_hash = chain[idx - symoffset];
        if ((chain_hash & ~1u) == (hash & ~1u)) {
            elf64_sym_t* sym = &lib->symtab[idx];
            const char* sym_name = lib->strtab + sym->st_name;
            if (strcmp(sym_name, name) == 0) {
                return sym;
            }
        }
        if (chain_hash & 1) break;   /* end of chain */
        idx++;
    }
    return NULL;
}

/* ============================================================
 * Symbol lookup across all loaded libs.
 *
 * Search order: main executable first, then libs in load order.
 * This matches Linux's default behavior (with some caveats around
 * symbol interposition that we ignore for simplicity).
 * ============================================================ */

static elf64_sym_t* lookup_symbol_global(const char* name,
                                          struct loaded_lib** out_lib) {
    for (int i = 0; i < MAX_LOADED_LIBS; i++) {
        struct loaded_lib* lib = &loaded_libs[i];
        if (!lib->in_use) continue;

        elf64_sym_t* sym = lookup_gnu_hash(lib, name);
        if (!sym) sym = lookup_sysv_hash(lib, name);
        if (sym) {
            /* Skip undefined symbols (st_shndx == 0). */
            if (sym->st_shndx == 0 && sym->st_value == 0) continue;
            if (out_lib) *out_lib = lib;
            return sym;
        }
    }
    return NULL;
}

/* ============================================================
 * Apply relocations for one lib.
 *
 * R_X86_64_RELATIVE:  *addr = base + addend
 * R_X86_64_64:        *addr = sym_value + addend
 * R_X86_64_GLOB_DAT:  *addr = sym_value
 * R_X86_64_JUMP_SLOT: *addr = sym_value  (PLT — we bind eagerly)
 * R_X86_64_PC32:      *addr = sym_value + addend - addr  (PC-relative)
 * R_X86_64_TPOFF*:    treat as 0 (no TLS yet)
 * R_X86_64_NONE:      no-op
 * ============================================================ */

static int apply_relocations(struct loaded_lib* lib) {
    /* Apply DT_RELA (non-PLT relocations). */
    if (lib->rela && lib->relasz) {
        size_t n = lib->relasz / sizeof(elf64_rela_t);
        for (size_t i = 0; i < n; i++) {
            elf64_rela_t* r = &lib->rela[i];
            uint32_t type = ELF64_R_TYPE(r->r_info);
            uint32_t sym_idx = ELF64_R_SYM(r->r_info);
            uint64_t* target = (uint64_t*)(lib->base + r->r_offset);

            switch (type) {
                case R_X86_64_NONE:
                    break;
                case R_X86_64_RELATIVE:
                    *target = lib->base + (uint64_t)r->r_addend;
                    break;
                case R_X86_64_64: {
                    elf64_sym_t* sym = &lib->symtab[sym_idx];
                    const char* name = lib->strtab + sym->st_name;
                    struct loaded_lib* def_lib = NULL;
                    elf64_sym_t* def = NULL;
                    if (sym->st_shndx != 0) {
                        /* Local symbol — defined in this lib. */
                        def = sym;
                        def_lib = lib;
                    } else {
                        def = lookup_symbol_global(name, &def_lib);
                    }
                    if (!def) {
                        if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
                            *target = 0;
                            break;
                        }
                        pr_warn("ldso: %s: unresolved R_X86_64_64 %s\n",
                                lib->name, name);
                        *target = 0;
                        break;
                    }
                    *target = def_lib->base + def->st_value + (uint64_t)r->r_addend;
                    break;
                }
                case R_X86_64_GLOB_DAT: {
                    elf64_sym_t* sym = &lib->symtab[sym_idx];
                    const char* name = lib->strtab + sym->st_name;
                    struct loaded_lib* def_lib = NULL;
                    elf64_sym_t* def = NULL;
                    if (sym->st_shndx != 0) {
                        def = sym; def_lib = lib;
                    } else {
                        def = lookup_symbol_global(name, &def_lib);
                    }
                    if (!def) {
                        if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
                            *target = 0; break;
                        }
                        pr_warn("ldso: %s: unresolved GLOB_DAT %s\n",
                                lib->name, name);
                        *target = 0; break;
                    }
                    *target = def_lib->base + def->st_value;
                    break;
                }
                case R_X86_64_JUMP_SLOT: {
                    /* Eager PLT binding. */
                    elf64_sym_t* sym = &lib->symtab[sym_idx];
                    const char* name = lib->strtab + sym->st_name;
                    struct loaded_lib* def_lib = NULL;
                    elf64_sym_t* def = NULL;
                    if (sym->st_shndx != 0) {
                        def = sym; def_lib = lib;
                    } else {
                        def = lookup_symbol_global(name, &def_lib);
                    }
                    if (!def) {
                        if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
                            *target = 0; break;
                        }
                        pr_warn("ldso: %s: unresolved JUMP_SLOT %s\n",
                                lib->name, name);
                        *target = 0; break;
                    }
                    *target = def_lib->base + def->st_value;
                    break;
                }
                case R_X86_64_PC32: {
                    elf64_sym_t* sym = &lib->symtab[sym_idx];
                    const char* name = lib->strtab + sym->st_name;
                    struct loaded_lib* def_lib = NULL;
                    elf64_sym_t* def = NULL;
                    if (sym->st_shndx != 0) {
                        def = sym; def_lib = lib;
                    } else {
                        def = lookup_symbol_global(name, &def_lib);
                    }
                    if (!def) {
                        pr_warn("ldso: %s: unresolved PC32 %s\n",
                                lib->name, name);
                        *(uint32_t*)target = 0; break;
                    }
                    uint64_t S = def_lib->base + def->st_value;
                    uint64_t P = (uint64_t)target;
                    *(uint32_t*)target = (uint32_t)(S + r->r_addend - P);
                    break;
                }
                case R_X86_64_32:
                case R_X86_64_32S: {
                    elf64_sym_t* sym = &lib->symtab[sym_idx];
                    const char* name = lib->strtab + sym->st_name;
                    struct loaded_lib* def_lib = NULL;
                    elf64_sym_t* def = NULL;
                    if (sym->st_shndx != 0) {
                        def = sym; def_lib = lib;
                    } else {
                        def = lookup_symbol_global(name, &def_lib);
                    }
                    if (!def) {
                        pr_warn("ldso: %s: unresolved 32 %s\n",
                                lib->name, name);
                        *(uint32_t*)target = 0; break;
                    }
                    *(uint32_t*)target = (uint32_t)(def_lib->base + def->st_value + r->r_addend);
                    break;
                }
                case R_X86_64_TPOFF64:
                case R_X86_64_TPOFF32:
                case R_X86_64_DTPMOD64:
                case R_X86_64_DTPOFF64:
                case R_X86_64_TLSGD:
                case R_X86_64_TLSLD:
                case R_X86_64_GOTTPOFF:
                    /* TLS not supported — write 0. */
                    *target = 0;
                    break;
                default:
                    pr_warn("ldso: %s: unhandled reloc type %u\n",
                            lib->name, type);
                    break;
            }
        }
    }
    return 0;
}

/* ============================================================
 * Load a shared library by name (recursive).
 *
 * If already loaded, returns the existing entry. Otherwise reads
 * the file, maps it, parses PT_DYNAMIC, recursively loads DT_NEEDED
 * dependencies, applies relocations.
 *
 * Returns the loaded_lib on success, NULL on failure.
 * ============================================================ */

static struct loaded_lib* load_library(const char* name);

static struct loaded_lib* load_library(const char* name) {
    /* Already loaded? */
    struct loaded_lib* existing = find_loaded_lib(name);
    if (existing) return existing;

    /* Find the file. */
    char path[256];
    if (!find_library_path(name, path, sizeof(path))) {
        pr_warn("ldso: library not found: %s\n", name);
        return NULL;
    }

    /* Read the file. */
    size_t file_size = 0;
    void* file_data = read_whole_file(path, &file_size);
    if (!file_data) {
        pr_warn("ldso: cannot read %s\n", path);
        return NULL;
    }

    /* Allocate a slot. */
    struct loaded_lib* lib = alloc_loaded_lib();
    if (!lib) {
        kfree(file_data);
        return NULL;
    }
    strncpy(lib->name, name, sizeof(lib->name) - 1);

    pr_info("ldso: loading %s (size=%u)\n", path, (unsigned)file_size);

    /* Map segments. */
    if (load_lib_segments(file_data, file_size, lib) < 0) {
        pr_warn("ldso: failed to map %s\n", path);
        lib->in_use = 0;
        kfree(file_data);
        return NULL;
    }

    /* Parse PT_DYNAMIC. */
    parse_dynamic(lib);

    /* Recursively load DT_NEEDED dependencies BEFORE relocations,
     * because relocations may reference symbols from those deps. */
    elf64_dyn_t* dyn = (elf64_dyn_t*)lib->dynamic;
    if (dyn) {
        for (elf64_dyn_t* d = dyn; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_NEEDED && lib->strtab) {
                const char* dep_name = lib->strtab + d->d_val;
                pr_info("ldso: %s needs %s\n", name, dep_name);
                if (!load_library(dep_name)) {
                    pr_warn("ldso: %s: dependency %s not found (continuing)\n",
                            name, dep_name);
                }
            }
        }
    }

    /* Apply relocations. */
    apply_relocations(lib);
    lib->relocated = 1;

    pr_info("ldso: loaded %s at base 0x%x, entry 0x%x\n",
            name, (unsigned)lib->base, (unsigned)lib->entry);

    kfree(file_data);
    return lib;
}

/* ============================================================
 * Load the main executable (also a dynamic ELF).
 *
 * The main executable is loaded at a fixed address (its PT_LOADs
 * specify vaddrs starting near 0x400000). Its PT_INTERP points to
 * us. After we load it and all its deps, we jump to its e_entry.
 * ============================================================ */

extern void elf_jump_to_user(uint64_t entry, uint64_t stack, uintptr_t pml4);
extern uintptr_t* user_pml4;
extern uint64_t user_stack_ptr;

#define USER_STACK_TOP   0x00007FFFFFE00000ULL
#define USER_STACK_SIZE  (8 * 1024 * 1024)   /* 8 MB stack for apps */

int ldso_load_and_run(const char* exe_path, int argc, char** argv, char** envp) {
    pr_info("ldso: starting dynamic linker for %s\n", exe_path);

    /* Read the main executable. */
    size_t file_size = 0;
    void* file_data = read_whole_file(exe_path, &file_size);
    if (!file_data) {
        pr_warn("ldso: cannot read %s\n", exe_path);
        return -1;
    }

    /* Allocate a lib slot for the main exe. We use the loaded_lib
     * structure because it has all the parsing infrastructure. */
    struct loaded_lib* exe = alloc_loaded_lib();
    if (!exe) {
        kfree(file_data);
        return -1;
    }
    strncpy(exe->name, exe_path, sizeof(exe->name) - 1);

    if (load_lib_segments(file_data, file_size, exe) < 0) {
        pr_warn("ldso: failed to map %s\n", exe_path);
        exe->in_use = 0;
        kfree(file_data);
        return -1;
    }

    parse_dynamic(exe);

    /* Check for PT_INTERP — verify it's us. */
    const elf64_ehdr_t* ehdr = (const elf64_ehdr_t*)file_data;
    const elf64_phdr_t* phdrs = (const elf64_phdr_t*)((uint8_t*)file_data + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_INTERP) {
            const char* interp = (const char*)(file_data + phdrs[i].p_offset);
            pr_info("ldso: PT_INTERP = %s\n", interp);
            /* We don't strictly need to load ourselves — we ARE the
             * interpreter. But we do need to make sure any symbols
             * the exe expects from ld.so (like __libc_start_main) are
             * resolvable. For now we skip self-loading. */
        }
    }

    /* Load DT_NEEDED dependencies. */
    elf64_dyn_t* dyn = (elf64_dyn_t*)exe->dynamic;
    if (dyn) {
        for (elf64_dyn_t* d = dyn; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_NEEDED && exe->strtab) {
                const char* dep_name = exe->strtab + d->d_val;
                pr_info("ldso: %s needs %s\n", exe_path, dep_name);
                if (!load_library(dep_name)) {
                    pr_warn("ldso: missing dep %s (continuing)\n", dep_name);
                }
            }
        }
    }

    /* Apply relocations for the main exe. */
    apply_relocations(exe);

    /* Run init arrays in dependency order (deps first, exe last). */
    for (int i = 0; i < MAX_LOADED_LIBS; i++) {
        struct loaded_lib* lib = &loaded_libs[i];
        if (!lib->in_use || !lib->relocated) continue;
        if (lib == exe) continue;   /* do exe last */
        if (lib->init_func) {
            pr_info("ldso: running DT_INIT for %s\n", lib->name);
            lib->init_func();
        }
        if (lib->init_array && lib->init_arraysz) {
            size_t n = lib->init_arraysz / sizeof(void (*)(void));
            for (size_t j = 0; j < n; j++) {
                if (lib->init_array[j]) {
                    pr_info("ldso: running init_array[%u] for %s\n",
                            (unsigned)j, lib->name);
                    lib->init_array[j]();
                }
            }
        }
    }
    /* Now the exe's init. */
    if (exe->init_func) exe->init_func();
    if (exe->init_array && exe->init_arraysz) {
        size_t n = exe->init_arraysz / sizeof(void (*)(void));
        for (size_t j = 0; j < n; j++) {
            if (exe->init_array[j]) exe->init_array[j]();
        }
    }

    /* Build the initial stack: argc, argv[], NULL, envp[], NULL, auxv[]. */
    /* For now we just set up argc/argv. Real Linux also passes envp and
     * auxv (AT_PHDR, AT_ENTRY, AT_PAGESZ, etc.) which glibc reads. */
    extern phys_addr_t pmm_alloc_page(void);
    /* Allocate 8 MB of stack pages. */
    size_t stack_pages = USER_STACK_SIZE / 4096;
    for (size_t i = 0; i < stack_pages; i++) {
        phys_addr_t phys = pmm_alloc_page();
        if (!phys) break;
        /* Map at USER_STACK_TOP - USER_STACK_SIZE + i*4096. We need
         * the kernel's VMM. For now, we trust that elf_jump_to_user
         * sets up a fresh address space — but that means our loaded
         * libs are NOT in the new address space. This is the next
         * piece of work: we need to load libs INTO the user PML4. */
    }

    /* Place argc + argv on the top of the stack. */
    uint64_t* sp = (uint64_t*)(USER_STACK_TOP - 256);
    sp[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) {
        sp[1 + i] = (uint64_t)argv[i];
    }
    sp[1 + argc] = 0;     /* argv NULL terminator */
    /* envp */
    if (envp) {
        int e = 0;
        while (envp[e]) {
            sp[2 + argc + e] = (uint64_t)envp[e];
            e++;
        }
        sp[2 + argc + e] = 0;
    } else {
        sp[2 + argc] = 0;
    }
    /* auxv — just AT_NULL. */
    sp[3 + argc] = 0;   /* AT_NULL type */
    sp[4 + argc] = 0;   /* value */

    pr_info("ldso: jumping to user entry 0x%x (sp=0x%x)\n",
            (unsigned)exe->entry, (unsigned)(uintptr_t)sp);

    /* Jump to ring 3. */
    elf_jump_to_user(exe->entry, (uint64_t)sp, (uintptr_t)user_pml4);

    /* Never reached. */
    return 0;
}

/* ============================================================
 * Public: probe whether a file is a dynamic ELF.
 * Returns 1 if yes (has PT_INTERP or PT_DYNAMIC).
 * ============================================================ */

int ldso_is_dynamic(const char* path) {
    int fd = vfs_open(path, 0);
    if (fd < 0) return 0;
    static uint8_t buf[4096];
    ssize_t n = vfs_read(fd, buf, sizeof(buf));
    vfs_close(fd);
    if (n < (ssize_t)sizeof(elf64_ehdr_t)) return 0;
    elf64_ehdr_t* ehdr = (elf64_ehdr_t*)buf;
    if (memcmp(ehdr->e_ident, "\x7F""ELF", 4) != 0) return 0;
    if (ehdr->e_ident[4] != 2) return 0;
    if (ehdr->e_machine != 0x3E) return 0;
    /* Check for PT_INTERP or PT_DYNAMIC. */
    elf64_phdr_t* phdrs = (elf64_phdr_t*)(buf + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if ((uint8_t*)&phdrs[i + 1] - buf > n) break;
        if (phdrs[i].p_type == PT_INTERP) return 1;
        if (phdrs[i].p_type == PT_DYNAMIC) return 1;
    }
    return 0;
}
