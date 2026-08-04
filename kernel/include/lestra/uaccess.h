#ifndef LESTRA_UACCESS_H
#define LESTRA_UACCESS_H
/* SMAP-aware user memory access helpers.
 * Currently ADDITIVE ONLY — stac/clac compile to no-ops when CR4.SMAP=0.
 * Once CR4.SMAP is enabled (next cycle, after syscall wrappers land),
 * these become required for correctness. */
#include <lestra/types.h>
#include <string.h>

static inline int copy_from_user(void* kernel_dst, const void* user_src, unsigned long n) {
    if (!user_src) return -14; /* EFAULT */
    if ((unsigned long)user_src >= 0xFFFF800000000000ULL) return -14;
    stac();
    /* rely on page-fault handler for bounds; VMA check TODO */
    memcpy(kernel_dst, user_src, n);
    clac();
    return 0;
}

static inline int copy_to_user(void* user_dst, const void* kernel_src, unsigned long n) {
    if (!user_dst) return -14;
    if ((unsigned long)user_dst >= 0xFFFF800000000000ULL) return -14;
    stac();
    memcpy(user_dst, kernel_src, n);
    clac();
    return 0;
}

static inline int strncpy_from_user(char* dst, const char* user_src, unsigned long max) {
    if (!user_src) return -14;
    stac();
    unsigned long i = 0;
    while (i < max - 1) {
        char c = ((const char*)user_src)[i];
        dst[i] = c;
        if (!c) { i++; break; }
        i++;
    }
    dst[i < max ? i : max - 1] = '\0';
    clac();
    return (int)i;
}

static inline int access_ok(const void* addr, unsigned long size) {
    if (!addr) return 0;
    unsigned long a = (unsigned long)addr;
    if (a >= 0xFFFF800000000000ULL) return 0;
    if (a + size < a) return 0; /* overflow */
    if (a + size > 0x00007FFFFFFFFFFFULL) return 0;
    return 1;
}

/* get_user / put_user — single-word accessors.
 * Return 0 on success, -EFAULT on bad pointer. */
#define get_user(kp, up) ({ \
    int __gu_ret = 0; \
    __typeof__(*(up)) __gu_tmp = 0; \
    if (access_ok((const void*)(up), sizeof(*(up)))) { \
        stac(); \
        __gu_tmp = *(up); \
        clac(); \
        *(kp) = __gu_tmp; \
    } else { \
        __gu_ret = -14; \
    } \
    __gu_ret; \
})

#define put_user(kv, up) ({ \
    int __pu_ret = 0; \
    if (access_ok((const void*)(up), sizeof(*(up)))) { \
        __typeof__(*(up)) __pu_tmp = (kv); \
        stac(); \
        *(up) = __pu_tmp; \
        clac(); \
    } else { \
        __pu_ret = -14; \
    } \
    __pu_ret; \
})

/* clear_user — zero-fill a user buffer (SMAP-safe memset). */
static inline int clear_user(void* user_dst, unsigned long n) {
    if (!user_dst) return -14;
    if ((unsigned long)user_dst >= 0xFFFF800000000000ULL) return -14;
    stac();
    memset(user_dst, 0, n);
    clac();
    return 0;
}

/* Security caps — hardening against malformed user args. */
#define LESTRA_ARG_MAX         128     /* max argc / envc */
#define LESTRA_ARG_BYTES_MAX   32768   /* max total argv+envp bytes */
#define LESTRA_POLL_MAX        1024    /* max nfds for poll/select */
#define LESTRA_PATH_MAX        4096    /* max path string length */
#endif /* LESTRA_UACCESS_H */
