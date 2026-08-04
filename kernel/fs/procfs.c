/*
 * Lestra OS - /proc filesystem
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Synthetic process-information filesystem modeled on Linux's /proc.
 * All files are generated on demand at read time so they always
 * reflect current state.
 *
 * Files:
 *   /proc/self/exe        - symlink-style: returns the exe path string
 *   /proc/self/maps       - memory map (one line per PT_LOAD region)
 *   /proc/self/auxv       - auxiliary vector (raw bytes)
 *   /proc/self/cmdline    - NUL-separated argv
 *   /proc/meminfo         - kernel memory stats
 *   /proc/cpuinfo         - real CPU info via CPUID
 *   /proc/ps              - process listing (PID, state, name)
 *   /proc/version         - "LestraOS 1.0.0-alpha ..."
 *
 * FDs live in [300..399] so they don't collide with VFS (3..66),
 * ext2 (100..199), tarfs (200..299), pipes (also 200-ish but
 * distinct subsystem), devfs (400..499), tmpfs (500..599), or
 * sockets (600..631).
 */

#include <lestra/types.h>
#include <lestra/procfs.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <lestra/sched.h>
#include <lestra/timer.h>
#include <string.h>

/* Per-open state. We pre-render the file content into a 4 KB buffer
 * on open() so read() can just memcpy out of it. This is simpler than
 * teaching every synthetic file how to seek. */
#define PROCFS_FILE_MAX  4096

enum procfs_kind {
    PROC_NONE = 0,
    PROC_SELF_EXE,
    PROC_SELF_MAPS,
    PROC_SELF_AUXV,
    PROC_SELF_CMDLINE,
    PROC_MEMINFO,
    PROC_CPUINFO,
    PROC_VERSION,
    PROC_PS,        /* process listing (like /proc/ps) */
    PROC_SECURITY,  /* /proc/security — protection status */
};

struct procfs_open {
    int used;
    enum procfs_kind kind;
    size_t size;
    size_t pos;
    char buf[PROCFS_FILE_MAX];
};

static struct procfs_open procfs_opens[PROCFS_MAX_OPEN];

int procfs_is_procfs_fd(int fd) {
    return fd >= PROCFS_FD_BASE && fd < PROCFS_FD_BASE + PROCFS_MAX_OPEN;
}

/* ----- CPUID (real; runs on the actual CPU) ----- */
static void cpuid_raw(uint32_t leaf, uint32_t* eax, uint32_t* ebx,
                      uint32_t* ecx, uint32_t* edx) {
    __asm__ volatile(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf)
    );
}

/* Vendor string from CPUID leaf 0. */
static void cpu_vendor_string(char* out, size_t sz) {
    if (sz < 13) { if (sz) out[0] = '\0'; return; }
    uint32_t a, b, c, d;
    cpuid_raw(0, &a, &b, &c, &d);
    /* EBX, EDX, ECX in that order form the 12-byte vendor string. */
    memcpy(out + 0, &b, 4);
    memcpy(out + 4, &d, 4);
    memcpy(out + 8, &c, 4);
    out[12] = '\0';
}

/* Brand string from CPUID leaves 0x80000002..0x80000004 (48 bytes). */
static void cpu_brand_string(char* out, size_t sz) {
    if (sz < 49) {
        if (sz) out[0] = '\0';
        return;
    }
    uint32_t a, b, c, d;
    char* p = out;
    for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
        cpuid_raw(leaf, &a, &b, &c, &d);
        memcpy(p +  0, &a, 4);
        memcpy(p +  4, &b, 4);
        memcpy(p +  8, &c, 4);
        memcpy(p + 12, &d, 4);
        p += 16;
    }
    out[48] = '\0';
    /* Strip leading spaces. */
    char* s = out;
    while (*s == ' ') s++;
    if (s != out) memmove(out, s, strlen(s) + 1);
}

/* ----- File content generators -----
 * Each fills the per-open buffer with the synthetic file content and
 * returns the number of bytes written. */

static size_t gen_self_exe(struct procfs_open* o) {
    struct process* p = task_current();
    if (!p) { o->buf[0] = '\0'; return 1; }
    /* exe_path is set by the ELF loader when the process starts. */
    size_t n = strlen(p->exe_path);
    if (n >= sizeof(o->buf)) n = sizeof(o->buf) - 1;
    memcpy(o->buf, p->exe_path, n);
    o->buf[n] = '\0';
    return n + 1;  /* include trailing NUL like Linux readlink does */
}

static size_t gen_self_maps(struct procfs_open* o) {
    struct process* p = task_current();
    if (!p) { o->buf[0] = '\0'; return 1; }
    /* The current scheduler only tracks the user PML4 and the entry
     * point — there's no per-region VMA list. Emit the canonical
     * regions so tools like /proc/$$/maps at least produce sensible
     * output. */
    int n = ksnprintf(o->buf, sizeof(o->buf),
        "%016llx-%016llx r-xp 00000000 00:00 0  [text]\n"
        "%016llx-%016llx rw-p 00000000 00:00 0  [data]\n"
        "%016llx-%016llx rw-p 00000000 00:00 0  [stack]\n"
        "%016llx-%016llx r--p 00000000 00:00 0  [vdso]\n",
        (unsigned long long)0x400000ULL,
        (unsigned long long)0x500000ULL,
        (unsigned long long)0x600000ULL,
        (unsigned long long)0x700000ULL,
        (unsigned long long)0x7FFFFFE00000ULL - 0x40000ULL,
        (unsigned long long)0x7FFFFFE00000ULL,
        (unsigned long long)0x7FFFFFE00000ULL,
        (unsigned long long)0x7FFFFFE01000ULL);
    (void)p;
    return (size_t)n;
}

static size_t gen_self_auxv(struct procfs_open* o) {
    /* Minimal AT_NULL-terminated auxv. Real Linux fills ~20 entries
     * (AT_BASE, AT_PHDR, AT_PAGESZ, AT_ENTRY, ...). We emit the bare
     * minimum so glibc startup that dereferences auxv doesn't crash. */
    uint64_t* a = (uint64_t*)o->buf;
    int i = 0;
    a[i++] = 6;   a[i++] = 4096;            /* AT_PAGESZ = 4096 */
    a[i++] = 9;   a[i++] = 100;             /* AT_CLKTCK = 100 */
    a[i++] = 0;   a[i++] = 0;               /* AT_NULL */
    return (size_t)(i * sizeof(uint64_t));
}

static size_t gen_self_cmdline(struct procfs_open* o) {
    struct process* p = task_current();
    if (!p || !p->exe_path[0]) { o->buf[0] = '\0'; return 1; }
    /* Single-arg cmdline: the exe path itself, NUL-terminated. */
    size_t n = strlen(p->exe_path);
    if (n >= sizeof(o->buf) - 1) n = sizeof(o->buf) - 2;
    memcpy(o->buf, p->exe_path, n);
    o->buf[n] = '\0';
    return n + 1;
}

static size_t gen_meminfo(struct procfs_open* o) {
    uintptr_t total = pmm_get_total();
    uintptr_t used  = pmm_get_used();
    uintptr_t freeb = pmm_get_free();
    /* heap_get_used returns bytes; we report in kB like Linux. */
    extern uintptr_t heap_get_used(void);
    uintptr_t heap_used = heap_get_used();
    int n = ksnprintf(o->buf, sizeof(o->buf),
        "MemTotal:       %8llu kB\n"
        "MemFree:        %8llu kB\n"
        "MemAvailable:   %8llu kB\n"
        "Buffers:        %8llu kB\n"
        "Cached:         %8llu kB\n"
        "SwapCached:     %8llu kB\n"
        "Active:         %8llu kB\n"
        "Inactive:       %8llu kB\n"
        "SwapTotal:      %8llu kB\n"
        "SwapFree:       %8llu kB\n"
        "KernelStack:    %8llu kB\n"
        "Mlocked:        %8llu kB\n",
        (unsigned long long)(total / 1024),
        (unsigned long long)(freeb / 1024),
        (unsigned long long)(freeb / 1024),
        0ULL,
        (unsigned long long)(heap_used / 1024),
        0ULL,
        (unsigned long long)(used / 1024),
        0ULL,
        0ULL,
        0ULL,
        0ULL,
        0ULL);
    return (size_t)n;
}

static size_t gen_cpuinfo(struct procfs_open* o) {
    char vendor[16];
    char brand[64];
    cpu_vendor_string(vendor, sizeof(vendor));
    cpu_brand_string(brand, sizeof(brand));

    uint32_t a, b, c, d;
    cpuid_raw(1, &a, &b, &c, &d);
    uint32_t family   = (a >> 8) & 0xF;
    uint32_t model    = (a >> 4) & 0xF;
    uint32_t stepping = a & 0xF;
    uint32_t extfam   = (a >> 20) & 0xFF;
    if (family == 0xF) family += extfam;
    uint32_t cores = (b >> 16) & 0xFF;
    if (cores == 0) cores = 1;
    int has_mmx  = (d & (1 << 23)) ? 1 : 0;
    int has_sse  = (d & (1 << 25)) ? 1 : 0;
    int has_sse2 = (d & (1 << 26)) ? 1 : 0;
    int has_avx  = (c & (1 << 28)) ? 1 : 0;

    int n = ksnprintf(o->buf, sizeof(o->buf),
        "processor\t: 0\n"
        "vendor_id\t: %s\n"
        "cpu family\t: %u\n"
        "model\t\t: %u\n"
        "model name\t: %s\n"
        "stepping\t: %u\n"
        "cpu MHz\t\t: %u\n"
        "cache size\t: %u KB\n"
        "physical id\t: 0\n"
        "siblings\t: %u\n"
        "core id\t\t: 0\n"
        "cpu cores\t: %u\n"
        "flags\t\t: fpu mmx%s%s%s%s\n"
        "bogomips\t: %u.%02u\n",
        vendor,
        (unsigned)family,
        (unsigned)model,
        brand[0] ? brand : "(unknown)",
        (unsigned)stepping,
        2400u,
        256u,
        (unsigned)cores,
        (unsigned)cores,
        has_mmx  ? " mmx"  : "",
        has_sse  ? " sse"  : "",
        has_sse2 ? " sse2" : "",
        has_avx  ? " avx"  : "",
        4000u, 25u);
    return (size_t)n;
}

static size_t gen_version(struct procfs_open* o) {
    int n = ksnprintf(o->buf, sizeof(o->buf),
        "LestraOS 1.0.0-alpha #1 (gcc) %s\n"
        "  Lee Muriihi Kingori\n"
        "  lestramk.org (c) 2026\n"
        "  Built on a freestanding x86_64 kernel.\n",
        __DATE__);
    return (size_t)n;
}

/* Generate a process listing in ps-style tabular format. */
static size_t gen_ps(struct procfs_open* o) {
    /* State names for display. */
    static const char* state_names[] = {
        "free", "runnable", "running", "blocked", "zombie"
    };
    int off = 0;
    off += ksnprintf(o->buf + off, sizeof(o->buf) - off,
                     "  PID  PPID  STATE        NAME\n");
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == PROC_FREE) continue;
        const char* sname = (procs[i].state >= 0 && procs[i].state <= 4)
                            ? state_names[procs[i].state] : "???";
        off += ksnprintf(o->buf + off, sizeof(o->buf) - off,
                         " %4d  %4d  %-12s %s\n",
                         procs[i].pid,
                         procs[i].parent_pid,
                         sname,
                         procs[i].name);
        if (off >= (int)sizeof(o->buf) - 64) break;
    }
    return (size_t)off;
}

/* Generate /proc/security — machine-parseable protection status. */
static size_t gen_security(struct procfs_open* o) {
    extern struct security_status g_security;
    int off = 0;
    off += ksnprintf(o->buf + off, sizeof(o->buf) - off,
                     "protection           status    notes\n");
    off += ksnprintf(o->buf + off, sizeof(o->buf) - off,
                     "SMEP                 %s    CR4 bit 20 (CPU %s)\n",
                     g_security.smep ? "on " : "off",
                     g_security.smep ? "supports" : "lacks");
    off += ksnprintf(o->buf + off, sizeof(o->buf) - off,
                     "SMAP                 %s    CR4 bit 21 (CPU %s)\n",
                     g_security.smap ? "on " : "off",
                     g_security.smap ? "supports" : "lacks");
    off += ksnprintf(o->buf + off, sizeof(o->buf) - off,
                     "NX                   on    EFER.NXE\n");
    off += ksnprintf(o->buf + off, sizeof(o->buf) - off,
                     "ASLR                 %s  stack+%d brk+%d (TSC-CSPRNG)\n",
                     g_security.aslr ? "on " : "off",
                     ASLR_STACK_BITS, ASLR_BRK_BITS);
    off += ksnprintf(o->buf + off, sizeof(o->buf) - off,
                     "StackCanaries        %s  -fstack-protector-strong\n",
                     g_security.canaries ? "on " : "off");
    off += ksnprintf(o->buf + off, sizeof(o->buf) - off,
                     "kptr_restrict        %d\n", g_security.kptr_restrict);
    off += ksnprintf(o->buf + off, sizeof(o->buf) - off,
                     "KASLR-lite           off   pending\n");
    return (size_t)off;
}

/* ----- open / read / close ----- */

static enum procfs_kind classify_path(const char* path) {
    if (!path) return PROC_NONE;
    if (strcmp(path, "/proc/self/exe")     == 0) return PROC_SELF_EXE;
    if (strcmp(path, "/proc/self/maps")    == 0) return PROC_SELF_MAPS;
    if (strcmp(path, "/proc/self/auxv")    == 0) return PROC_SELF_AUXV;
    if (strcmp(path, "/proc/self/cmdline") == 0) return PROC_SELF_CMDLINE;
    if (strcmp(path, "/proc/meminfo")      == 0) return PROC_MEMINFO;
    if (strcmp(path, "/proc/cpuinfo")      == 0) return PROC_CPUINFO;
    if (strcmp(path, "/proc/version")      == 0) return PROC_VERSION;
    if (strcmp(path, "/proc/ps")           == 0) return PROC_PS;
    if (strcmp(path, "/proc/security")     == 0) return PROC_SECURITY;
    return PROC_NONE;
}

int procfs_open(const char* path) {
    enum procfs_kind kind = classify_path(path);
    if (kind == PROC_NONE) return -1;

    for (int i = 0; i < PROCFS_MAX_OPEN; i++) {
        if (!procfs_opens[i].used) {
            struct procfs_open* o = &procfs_opens[i];
            o->used = 1;
            o->kind = kind;
            o->pos  = 0;
            switch (kind) {
                case PROC_SELF_EXE:     o->size = gen_self_exe(o);     break;
                case PROC_SELF_MAPS:    o->size = gen_self_maps(o);    break;
                case PROC_SELF_AUXV:    o->size = gen_self_auxv(o);    break;
                case PROC_SELF_CMDLINE: o->size = gen_self_cmdline(o); break;
                case PROC_MEMINFO:      o->size = gen_meminfo(o);      break;
                case PROC_CPUINFO:      o->size = gen_cpuinfo(o);      break;
                case PROC_VERSION:      o->size = gen_version(o);      break;
                case PROC_PS:           o->size = gen_ps(o);           break;
                case PROC_SECURITY:     o->size = gen_security(o);     break;
                default: o->used = 0; return -1;
            }
            return i + PROCFS_FD_BASE;
        }
    }
    return -1;  /* too many open procfs files */
}

int procfs_close(int fd) {
    fd -= PROCFS_FD_BASE;
    if (fd < 0 || fd >= PROCFS_MAX_OPEN) return -1;
    procfs_opens[fd].used = 0;
    procfs_opens[fd].kind = PROC_NONE;
    return 0;
}

ssize_t procfs_read(int fd, void* buf, size_t count) {
    fd -= PROCFS_FD_BASE;
    if (fd < 0 || fd >= PROCFS_MAX_OPEN || !procfs_opens[fd].used) return -1;
    struct procfs_open* o = &procfs_opens[fd];
    if (o->pos >= o->size) return 0;
    size_t avail = o->size - o->pos;
    if (count > avail) count = avail;
    memcpy(buf, o->buf + o->pos, count);
    o->pos += count;
    return (ssize_t)count;
}

void procfs_init(void) {
    memset(procfs_opens, 0, sizeof(procfs_opens));
    pr_info("procfs: initialized (FD range %d..%d)\n",
            PROCFS_FD_BASE, PROCFS_FD_BASE + PROCFS_MAX_OPEN - 1);
}
