#ifndef LESTRA_UACCESS_H
#define LESTRA_UACCESS_H
/* SMAP-aware user memory access helpers.
 * All user-pointer dereferences MUST go through these helpers
 * when CR4.SMAP=1 (TIER 3 enabled). */
#include <lestra/types.h>
#include <string.h>

/* Security caps — hardening against malformed user args. */
#define LESTRA_ARG_MAX         128     /* max argc / envc */
#define LESTRA_ARG_BYTES_MAX   32768   /* max total argv+envp bytes */
#define LESTRA_POLL_MAX        1024    /* max nfds for poll/select */
#define LESTRA_PATH_MAX        4096    /* max path string length */

static inline int copy_from_user(void* kernel_dst, const void* user_src, unsigned long n) {
    if (!user_src) return -14; /* EFAULT */
    if ((unsigned long)user_src >= 0xFFFF800000000000ULL) return -14;
    stac();
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

/* strnlen_user — safely measure a NUL-terminated user string (SMAP-safe).
 * Returns length (excluding NUL) on success, or < 0 on fault/bad pointer.
 * Caps at 'max' bytes to avoid walking runaway user strings. */
static inline int strnlen_user(const char* user_str, unsigned long max) {
    if (!access_ok(user_str, 1)) return -14;
    stac();
    unsigned long i = 0;
    while (i < max) {
        char c = user_str[i];
        if (!c) { clac(); return (int)i; }
        i++;
    }
    clac();
    return (int)max;
}

/* copy_argvec_from_user — copy a NULL-terminated user pointer array
 * (argv or envp) into kernel-side buffers with full bounds checking.
 *
 * Returns 0 on success, negative errno on failure.
 * On success: k_ptrs[0..k_count-1] point into k_strings[],
 *            k_ptrs[k_count] = NULL.
 */
static inline int copy_argvec_from_user(
    const char* const* user_vec,
    char* k_ptrs[],
    char* k_strings,
    unsigned long strings_sz,
    int* out_count)
{
    int count = 0;
    unsigned long str_off = 0;

    /* Walk the user pointer array element by element.
     * We read raw pointer values directly (bypassing get_user
     * which has const-qualification issues with pointer-to-const arrays). */
    for (int i = 0; ; i++) {
        const char* ptr;
        /* Read one pointer from the user argv/envp array (SMAP-safe). */
        if (!access_ok(&user_vec[i], sizeof(const char*))) return -14;
        stac();
        ptr = user_vec[i];
        clac();

        if (!ptr) { /* NULL terminator */
            k_ptrs[count] = NULL;
            break;
        }
        if (count >= LESTRA_ARG_MAX) return -7; /* E2BIG */

        /* Measure the user string length (SMAP-safe). */
        int slen = strnlen_user(ptr, LESTRA_PATH_MAX);
        if (slen < 0) return -14;

        /* Check total byte budget. */
        if (str_off + (unsigned long)(slen + 1) > strings_sz) return -12; /* ENOMEM */

        /* Copy the string into the packed kernel buffer (SMAP-safe). */
        if (copy_from_user(&k_strings[str_off], ptr, (unsigned long)(slen + 1)) < 0)
            return -14;

        k_ptrs[count] = &k_strings[str_off];
        str_off += (unsigned long)(slen + 1);
        count++;
    }

    *out_count = count;
    return 0;
}

#endif /* LESTRA_UACCESS_H */
