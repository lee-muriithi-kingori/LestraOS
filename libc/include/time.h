/*
 * Lestra OS - C Standard Library - time (POSIX)
 * Copyright (c) 2026 lestramk.org
 *
 * W1-A finding F: previously the libc had no <time.h>, so programs
 * that needed struct timeval / struct timespec / CLOCK_REALTIME had
 * to hand-define them.
 *
 * The kernel implements SYS_GETTIMEOFDAY(12) and SYS_CLOCK_GETTIME(40).
 * Note (W1-A finding B): the kernel's sys_gettimeofday historically
 * returned a bare int64 ms-since-boot and ignored the user pointer;
 * the libc wrapper passes the pointers through regardless, so when
 * the kernel side is fixed to fill the struct, user code starts
 * working without recompilation.
 *
 * Freestanding — no FP, no SSE.
 */
#ifndef LIBC_TIME_H
#define LIBC_TIME_H

#include <stddef.h>
#include <stdint.h>

/* Scalar time types.  Use `long` (64-bit on x86_64) to match the
 * Linux x86_64 ABI for tv_sec. */
typedef long time_t;
typedef long suseconds_t;
typedef long clock_t;

/* POSIX timeval — seconds + microseconds. */
struct timeval {
    long tv_sec;        /* seconds since the Epoch */
    long tv_usec;       /* microseconds [0, 999999] */
};

/* POSIX timespec — seconds + nanoseconds. */
struct timespec {
    long tv_sec;        /* seconds */
    long tv_nsec;       /* nanoseconds [0, 999999999] */
};

/* BSD timezone — passed to gettimeofday() by legacy callers.  Modern
 * code passes NULL for tz. */
struct timezone {
    int tz_minuteswest;   /* minutes west of GMT */
    int tz_dsttime;       /* daylight savings time in effect (obsolete) */
};

/* Clock IDs for clock_gettime(). */
#define CLOCK_REALTIME            0
#define CLOCK_MONOTONIC           1
#define CLOCK_PROCESS_CPUTIME_ID  2
#define CLOCK_THREAD_CPUTIME_ID   3
#define CLOCK_MONOTONIC_RAW       4
#define CLOCK_REALTIME_COARSE     5
#define CLOCK_MONOTONIC_COARSE    6
#define CLOCK_BOOTTIME            7
#define CLOCK_REALTIME_ALARM      8
#define CLOCK_BOOTTIME_ALARM      9

/* Wrappers (defined in libc/src/unistd.c). */
int gettimeofday(struct timeval* tv, struct timezone* tz);
int clock_gettime(int clk_id, struct timespec* tp);
int nanosleep(const struct timespec* req, struct timespec* rem);

/* sleep() is declared in <unistd.h> too; same signature, no conflict. */
unsigned int sleep(unsigned int seconds);

#endif /* LIBC_TIME_H */
