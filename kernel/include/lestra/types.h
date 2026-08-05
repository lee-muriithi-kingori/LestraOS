/*
 * Lestra OS - Type Definitions
 * Copyright (c) 2026 lestramk.org
 */

#ifndef LESTRA_TYPES_H
#define LESTRA_TYPES_H

#include <stdarg.h>

/* Fixed-width integer types */
#ifndef int8_t
typedef signed char        int8_t;
#endif
#ifndef uint8_t
typedef unsigned char      uint8_t;
#endif
#ifndef int16_t
typedef signed short       int16_t;
#endif
#ifndef uint16_t
typedef unsigned short     uint16_t;
#endif
#ifndef int32_t
typedef signed int         int32_t;
#endif
#ifndef uint32_t
typedef unsigned int       uint32_t;
#endif
#ifndef int64_t
typedef signed long long   int64_t;
#endif
#ifndef uint64_t
typedef unsigned long long uint64_t;
#endif

/* Size types - use uint64_t/int64_t to match stdint.h definitions */
#ifndef _SIZE_T_DEFINED
#ifndef size_t
typedef uint64_t           size_t;
#endif
#define _SIZE_T_DEFINED
#endif
#ifndef _SSIZE_T_DEFINED
#ifndef ssize_t
typedef int64_t            ssize_t;
#endif
#define _SSIZE_T_DEFINED
#endif
#ifndef _INTPTR_T_DEFINED
#ifndef intptr_t
typedef int64_t            intptr_t;
#endif
#define _INTPTR_T_DEFINED
#endif
#ifndef _UINTPTR_T_DEFINED
#ifndef uintptr_t
typedef uint64_t           uintptr_t;
#endif
#define _UINTPTR_T_DEFINED
#endif

/* Boolean */
typedef int                bool;
#define true  1
#define false 0

/* NULL */
#ifndef NULL
#define NULL ((void*)0)
#endif

/* Physical/virtual addresses */
typedef uintptr_t phys_addr_t;
typedef uintptr_t virt_addr_t;

/* Process ID */
typedef int32_t pid_t;

/* Error codes */
typedef int                errno_t;
#define EOK          0
#define ENOMEM      -1
#define EINVAL      -2
#define EACCES      -3
#define ENOTFOUND   -4
#define EIO         -5
#define ENOSYS      -6
#define EBUSY       -7
#define ENODEV      -8
#define EFAULT     -14  /* Bad address (Linux errno 14, mirrored as -14) */

/* Integer limits */
#define INT_MAX     2147483647
#define INT_MIN     (-2147483648)
#define SIZE_MAX    ((size_t)(-1))
#define SSIZE_MAX   ((ssize_t)(SIZE_MAX >> 1))

/* Memory constants */
#define KiB        (1024UL)
#define MiB        (1024UL * KiB)
#define GiB        (1024UL * MiB)

#define PAGE_SIZE   4096
#define PAGE_SHIFT  12
#define PAGE_MASK   (~(PAGE_SIZE - 1))

/* Alignment macros */
#define ALIGN_UP(x, align)    (((x) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(x, align)  ((x) & ~((align) - 1))
#define IS_ALIGNED(x, align)  (((x) & ((align) - 1)) == 0)

/* Bit manipulation */
#define BIT(n)                (1ULL << (n))
#define SET_BIT(x, n)         ((x) |= BIT(n))
#define CLEAR_BIT(x, n)       ((x) &= ~BIT(n))
#define TEST_BIT(x, n)        (((x) >> (n)) & 1)

/* Static assertions */
#define STATIC_ASSERT(expr, msg) _Static_assert(expr, msg)

/* Compiler attributes */
#define __packed       __attribute__((packed))
#define __aligned(x)   __attribute__((aligned(x)))
#define __noreturn     __attribute__((noreturn))
#define __unused       __attribute__((unused))
#define __section(x)   __attribute__((section(x)))
#define __weak         __attribute__((weak))

/* Inline assembly helpers */
#define cli() __asm__ volatile("cli")
#define sti() __asm__ volatile("sti")
#define hlt() __asm__ volatile("hlt")

#define barrier() __asm__ volatile("" ::: "memory")

/* CPU ID helpers */
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdtsc(void) {
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static inline uint8_t inb(uint16_t port) {
    uint8_t result;
    __asm__ volatile("inb %1, %0" : "=a"(result) : "dN"(port));
    return result;
}

static inline void outb(uint16_t port, uint8_t data) {
    __asm__ volatile("outb %0, %1" : : "a"(data), "dN"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t result;
    __asm__ volatile("inw %1, %0" : "=a"(result) : "dN"(port));
    return result;
}

static inline void outw(uint16_t port, uint16_t data) {
    __asm__ volatile("outw %0, %1" : : "a"(data), "dN"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t result;
    __asm__ volatile("inl %1, %0" : "=a"(result) : "dN"(port));
    return result;
}

static inline void outl(uint16_t port, uint32_t data) {
    __asm__ volatile("outl %0, %1" : : "a"(data), "dN"(port));
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

/* CPU flags */
static inline uint64_t read_flags(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    return flags;
}

static inline void invlpg(void* addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

static inline uintptr_t read_cr3(void) {
    uintptr_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(uintptr_t val) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(val));
}

static inline uintptr_t read_cr2(void) {
    uintptr_t val;
    __asm__ volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

/* CPUID-based feature detection for RDRAND/RDSEED.
 * Required: QEMU's default qemu64 CPU does NOT advertise RDRAND
 * (CPUID.01h:ECX[30]=0), so emitting `rdrand` unconditionally raises #UD.
 * Gate all rdrand/rdseed callers on these checks. */
static inline int cpu_has_rdrand(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    cached = (ecx & (1u << 30)) ? 1 : 0;
    return cached;
}

static inline int cpu_has_rdseed(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    uint32_t eax, ebx, ecx, edx;
    /* CPUID leaf 7, subleaf 0 */
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
    cached = (ebx & (1u << 18)) ? 1 : 0;
    return cached;
}

static inline int rdrand32(uint32_t* val) {
    if (!cpu_has_rdrand()) { *val = 0; return 0; }
    unsigned char ok;
    asm volatile("rdrand %1; setc %0" : "=q"(ok), "=r"(*val));
    return ok;
}

static inline int rdrand64(uint64_t* val) {
    if (!cpu_has_rdrand()) { *val = 0; return 0; }
    unsigned char ok;
    asm volatile("rdrand %1; setc %0" : "=q"(ok), "=r"(*val));
    return ok;
}

static inline int rdseed32(uint32_t* val) {
    if (!cpu_has_rdseed()) { *val = 0; return 0; }
    unsigned char ok;
    asm volatile("rdseed %1; setc %0" : "=q"(ok), "=r"(*val));
    return ok;
}

/* SMEP/SMAP CPU feature detection (CPUID leaf 7, subleaf 0).
 * SMEP = EBX bit 7, SMAP = EBX bit 20. Per Intel SDM Vol 3A.
 * NOTE: these DETECT the feature only. The CR4 bits are NOT flipped
 * here — that happens in a follow-up cycle after all syscall
 * user-pointer accesses are wrapped with stac/clac. */
static inline int cpu_has_smep(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
    cached = (ebx & (1u << 7)) ? 1 : 0;
    return cached;
}

static inline int cpu_has_smap(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
    cached = (ebx & (1u << 20)) ? 1 : 0;
    return cached;
}

static inline uint64_t read_cr4(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline void write_cr4(uint64_t val) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(val));
}

/* stac/clac: enable/disable user-memory access from ring 0 (SMAP).
 *
 * KE-25 FIX: stac/clac are #UD (Invalid Opcode) when CR4.SMAP=0 — they
 * are NOT no-ops as the old comment claimed. The syscall path uses these
 * unconditionally via the uaccess.h helpers, so on any CPU without SMAP
 * (e.g. the default `qemu64` model), the first user→kernel transition
 * that touches a user pointer would #UD and panic. This was the second
 * root cause of the "/init crashes after entering userspace" bug.
 *
 * We guard both instructions with the runtime flag g_smap_enabled, which
 * gdt_init() sets to 1 only after it successfully flips CR4.SMAP. When
 * SMAP is off, ring 0 can already access user memory, so skipping the
 * instruction is semantically correct. */
extern uint8_t g_smap_enabled;

static inline void stac(void) {
    if (g_smap_enabled) __asm__ volatile("stac" ::: "memory");
}
static inline void clac(void) {
    if (g_smap_enabled) __asm__ volatile("clac" ::: "memory");
}

/* Global security status — populated at boot, read by /proc/security. */
struct security_status {
    uint8_t smep;          /* 1 if CR4.SMEP set (currently always 0 — bit not flipped yet) */
    uint8_t smap;          /* 1 if CR4.SMAP set (currently always 0 — bit not flipped yet) */
    uint8_t nx;            /* 1 if EFER.NXE set (already on) */
    uint8_t aslr;          /* 1 if PT_LOAD randomization active (deferred) */
    uint8_t canaries;      /* 1 if -fstack-protector-strong active + guard initialized */
    uint8_t kptr_restrict; /* 0=off, 1=mask for non-root, 2=always mask */
    uint8_t kaslr_lite;    /* 1 if kernel heap base randomized (deferred) */
    uint8_t _pad;
};
extern struct security_status g_security;

#endif /* LESTRA_TYPES_H */
