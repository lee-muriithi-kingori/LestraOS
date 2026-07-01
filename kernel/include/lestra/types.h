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

/* Size types */
#ifndef size_t
typedef unsigned long      size_t;
#endif
#ifndef ssize_t
typedef signed long        ssize_t;
#endif
#ifndef intptr_t
typedef long               intptr_t;
#endif
#ifndef uintptr_t
typedef unsigned long      uintptr_t;
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

#endif /* LESTRA_TYPES_H */
