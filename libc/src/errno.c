/*
 * Lestra OS - errno Implementation
 * Copyright (c) 2026 lestramk.org
 *
 * Provides:
 *   - the global `errno` lvalue (single-threaded; no TLS indirection)
 *   - strerror(errnum)  -> a short static description of an errno value
 *   - perror(s)         -> write "<s>: <strerror(errno)>\n" to fd 2
 *
 * The kernel syscall ABI returns negative errno on failure (e.g.
 * -ENOENT).  libc wrappers translate that to "return -1; errno = -ret"
 * per POSIX; perror/strerror then turn the resulting positive errno
 * into a human-readable string.
 *
 * Freestanding — no FP, no SSE.  Builds with -mno-sse -mno-mmx -mno-sse2.
 */

#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>

/* The global errno.  Initialised to 0 (no error) at program start. */
int errno = 0;

/* strerror: return a static string describing errnum.
 *
 * Only the common errno values are spelled out; everything else falls
 * through to "Unknown error N".  The buffer is intentionally a
 * compile-time constant so no heap allocation is needed. */
char* strerror(int errnum) {
    switch (errnum) {
        case 0:        return (char*)"Success";
        case EPERM:    return (char*)"Operation not permitted";
        case ENOENT:   return (char*)"No such file or directory";
        case ESRCH:    return (char*)"No such process";
        case EINTR:    return (char*)"Interrupted system call";
        case EIO:      return (char*)"Input/output error";
        case ENXIO:    return (char*)"No such device or address";
        case E2BIG:    return (char*)"Argument list too long";
        case ENOEXEC:  return (char*)"Exec format error";
        case EBADF:    return (char*)"Bad file descriptor";
        case ECHILD:   return (char*)"No child processes";
        case EAGAIN:   return (char*)"Resource temporarily unavailable";
        case ENOMEM:   return (char*)"Cannot allocate memory";
        case EACCES:   return (char*)"Permission denied";
        case EFAULT:   return (char*)"Bad address";
        case ENOTBLK:  return (char*)"Block device required";
        case EBUSY:    return (char*)"Device or resource busy";
        case EEXIST:   return (char*)"File exists";
        case EXDEV:    return (char*)"Invalid cross-device link";
        case ENODEV:   return (char*)"No such device";
        case ENOTDIR:  return (char*)"Not a directory";
        case EISDIR:   return (char*)"Is a directory";
        case EINVAL:   return (char*)"Invalid argument";
        case ENFILE:   return (char*)"Too many open files in system";
        case EMFILE:   return (char*)"Too many open files";
        case ENOTTY:   return (char*)"Inappropriate ioctl for device";
        case ETXTBSY:  return (char*)"Text file busy";
        case EFBIG:    return (char*)"File too large";
        case ENOSPC:   return (char*)"No space left on device";
        case ESPIPE:   return (char*)"Illegal seek";
        case EROFS:    return (char*)"Read-only file system";
        case EMLINK:   return (char*)"Too many links";
        case EPIPE:    return (char*)"Broken pipe";
        case EDOM:     return (char*)"Numerical argument out of domain";
        case ERANGE:   return (char*)"Numerical result out of range";
        case EDEADLK:  return (char*)"Resource deadlock avoided";
        case ENAMETOOLONG: return (char*)"File name too long";
        case ENOLCK:   return (char*)"No locks available";
        case ENOSYS:   return (char*)"Function not implemented";
        case ENOTEMPTY:return (char*)"Directory not empty";
        case ELOOP:    return (char*)"Too many levels of symbolic links";
        case ENOMSG:   return (char*)"No message of desired type";
        case EIDRM:    return (char*)"Identifier removed";
        case EILSEQ:   return (char*)"Invalid or incomplete multibyte/wide character";
        case EOVERFLOW:return (char*)"Value too large for defined data type";
        case ETIMEDOUT:return (char*)"Connection timed out";
        case ECONNREFUSED: return (char*)"Connection refused";
        case ECONNRESET:   return (char*)"Connection reset by peer";
        case ECONNABORTED: return (char*)"Software caused connection abort";
        case ENOTCONN: return (char*)"Transport endpoint is not connected";
        case EISCONN:  return (char*)"Transport endpoint is already connected";
        case EADDRINUSE:   return (char*)"Address already in use";
        case EADDRNOTAVAIL:return (char*)"Cannot assign requested address";
        case ENETDOWN: return (char*)"Network is down";
        case ENETUNREACH: return (char*)"Network is unreachable";
        case EHOSTUNREACH: return (char*)"No route to host";
        case ENOTSOCK: return (char*)"Socket operation on non-socket";
        case EOPNOTSUPP: return (char*)"Operation not supported";
        case EPROTONOSUPPORT: return (char*)"Protocol not supported";
        case EAFNOSUPPORT: return (char*)"Address family not supported by protocol";
        case ECANCELED:return (char*)"Operation canceled";
        default: {
            /* "Unknown error N" — render into a per-call static buffer.
             * Single-threaded so this is safe. */
            static char unknown_buf[32];
            char* p = unknown_buf + sizeof(unknown_buf) - 1;
            *p = '\0';
            int v = errnum;
            int neg = 0;
            if (v < 0) { neg = 1; v = -v; }
            do {
                *--p = (char)('0' + (v % 10));
                v /= 10;
            } while (v > 0 && p > unknown_buf + 14);
            if (neg && p > unknown_buf + 13) *--p = '-';
            /* prepend "Unknown error " */
            const char* prefix = "Unknown error ";
            const char* q = prefix;
            /* slide rendered number left to make room for prefix */
            int nlen = (int)(unknown_buf + sizeof(unknown_buf) - 1 - p);
            char* dst = unknown_buf;
            while (*q && dst < p) *dst++ = *q++;
            /* copy the rendered number after the prefix */
            for (int i = 0; i < nlen; i++) *dst++ = *p++;
            *dst = '\0';
            return unknown_buf;
        }
    }
}

/* perror: write "<s>: <strerror(errno)>\n" (or just "<strerror(errno)>\n"
 * when s is NULL or empty) to STDERR_FILENO (fd 2).
 *
 * Uses a direct SYS_WRITE syscall rather than stdio so it works even
 * when the stdio buffer state is suspect (e.g. right after a failed
 * fopen).  Does NOT clear errno — callers may inspect it afterwards. */
void perror(const char* s) {
    char sep[] = ": ";
    char nl[]  = "\n";
    char* msg = strerror(errno);

    if (s && *s) {
        /* Compute strlen(s) once. */
        size_t slen = 0;
        while (s[slen]) slen++;
        /* Write s, then ": ", then strerror, then "\n".  Best-effort:
         * ignore short-write errors because we cannot recurse into
         * perror again. */
        extern int64_t syscall(uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t);
        syscall(SYS_WRITE, STDERR_FILENO, (uint64_t)s, slen, 0, 0);
        syscall(SYS_WRITE, STDERR_FILENO, (uint64_t)sep, 2, 0, 0);
    }
    /* strerror message */
    size_t mlen = 0;
    while (msg[mlen]) mlen++;
    extern int64_t syscall(uint64_t, uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t);
    syscall(SYS_WRITE, STDERR_FILENO, (uint64_t)msg, mlen, 0, 0);
    syscall(SYS_WRITE, STDERR_FILENO, (uint64_t)nl, 1, 0, 0);
}
