/*
 * Lestra OS - /dev filesystem
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Minimal /dev with the four device files every Unix user expects:
 *
 *   /dev/null      read returns 0 bytes; write discards everything
 *   /dev/zero      read returns count zero bytes; write discards
 *   /dev/urandom   read returns count pseudo-random bytes
 *   /dev/tty       open returns -1 (we don't have a controlling tty)
 *
 * FDs live in [400..499] so they don't collide with VFS (3..66),
 * ext2 (100..199), tarfs (200..299), procfs (300..399), tmpfs
 * (500..599), or sockets (600..631).
 */

#include <lestra/types.h>
#include <lestra/devfs.h>
#include <lestra/mouse.h>
#include <lestra/printk.h>
#include <lestra/timer.h>
#include <string.h>

enum devfs_kind {
    DEV_NONE = 0,
    DEV_NULL,
    DEV_ZERO,
    DEV_URANDOM,
    DEV_TTY,
    DEV_MOUSE,
};

struct devfs_open {
    int used;
    enum devfs_kind kind;
};

static struct devfs_open devfs_opens[DEVFS_MAX_OPEN];

int devfs_is_devfs_fd(int fd) {
    return fd >= DEVFS_FD_BASE && fd < DEVFS_FD_BASE + DEVFS_MAX_OPEN;
}

/* ---- PRNG: xorshift128 seeded from rdtsc + timer ---- */
static uint64_t prng_state[2] = { 0x123456789abcdef0ULL, 0xfedcba9876543210ULL };

static void prng_seed(void) {
    /* Mix in TSC and the millisecond counter for entropy. */
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t tsc = ((uint64_t)hi << 32) | lo;
    prng_state[0] ^= tsc;
    prng_state[1] ^= timer_get_ms();
    if (prng_state[0] == 0 && prng_state[1] == 0) {
        prng_state[0] = 0x123456789abcdef0ULL;
        prng_state[1] = 0xfedcba9876543210ULL;
    }
}

static uint64_t prng_next(void) {
    /* xorshift128 (Marsaglia). */
    uint64_t s1 = prng_state[0];
    uint64_t s0 = prng_state[1];
    uint64_t r  = s0 + s1;
    prng_state[0] = s0;
    s1 ^= s1 << 23;
    s1 ^= s1 >> 17;
    s1 ^= s0;
    s1 ^= s0 >> 26;
    prng_state[1] = s1;
    return r;
}

static enum devfs_kind classify(const char* path) {
    if (!path) return DEV_NONE;
    if (strcmp(path, "/dev/null")      == 0) return DEV_NULL;
    if (strcmp(path, "/dev/zero")      == 0) return DEV_ZERO;
    if (strcmp(path, "/dev/urandom")   == 0) return DEV_URANDOM;
    if (strcmp(path, "/dev/random")    == 0) return DEV_URANDOM; /* same impl */
    if (strcmp(path, "/dev/tty")       == 0) return DEV_TTY;
    if (strcmp(path, "/dev/mouse")     == 0) return DEV_MOUSE;
    if (strcmp(path, "/dev/input/mouse0") == 0) return DEV_MOUSE;
    return DEV_NONE;
}

int devfs_open(const char* path) {
    enum devfs_kind k = classify(path);
    if (k == DEV_NONE) return -1;
    /* /dev/tty requires a controlling terminal, which we don't have.
     * Linux returns ENXIO; we just return -1 so the caller sees
     * "no such device". */
    if (k == DEV_TTY) return -1;
    for (int i = 0; i < DEVFS_MAX_OPEN; i++) {
        if (!devfs_opens[i].used) {
            devfs_opens[i].used = 1;
            devfs_opens[i].kind = k;
            return i + DEVFS_FD_BASE;
        }
    }
    return -1;  /* table full */
}

int devfs_close(int fd) {
    fd -= DEVFS_FD_BASE;
    if (fd < 0 || fd >= DEVFS_MAX_OPEN) return -1;
    devfs_opens[fd].used = 0;
    devfs_opens[fd].kind = DEV_NONE;
    return 0;
}

ssize_t devfs_read(int fd, void* buf, size_t count) {
    fd -= DEVFS_FD_BASE;
    if (fd < 0 || fd >= DEVFS_MAX_OPEN || !devfs_opens[fd].used) return -1;
    if (!buf) return -EFAULT;
    enum devfs_kind k = devfs_opens[fd].kind;
    switch (k) {
        case DEV_NULL:
            /* /dev/null always returns EOF. */
            return 0;
        case DEV_ZERO:
            memset(buf, 0, count);
            return (ssize_t)count;
        case DEV_URANDOM: {
            /* Fill buf with pseudo-random bytes 8 at a time. */
            uint8_t* p = (uint8_t*)buf;
            size_t i = 0;
            while (i + 8 <= count) {
                uint64_t r = prng_next();
                memcpy(p + i, &r, 8);
                i += 8;
            }
            if (i < count) {
                uint64_t r = prng_next();
                size_t left = count - i;
                memcpy(p + i, &r, left);
            }
            return (ssize_t)count;
        }
        case DEV_TTY:
            /* open() already refused, but be defensive. */
            return -1;
        case DEV_MOUSE: {
            /* Read one mouse_event struct at a time.
             * If no event available, return 0 (EAGAIN semantics). */
            struct mouse_event mev;
            if (!mouse_get_event(&mev)) return 0;
            size_t copy = sizeof(mev);
            if (copy > count) copy = count;
            memcpy(buf, &mev, copy);
            return (ssize_t)copy;
        }
        default:
            return -1;
    }
}

ssize_t devfs_write(int fd, const void* buf, size_t count) {
    fd -= DEVFS_FD_BASE;
    if (fd < 0 || fd >= DEVFS_MAX_OPEN || !devfs_opens[fd].used) return -1;
    enum devfs_kind k = devfs_opens[fd].kind;
    switch (k) {
        case DEV_NULL:
        case DEV_ZERO:
        case DEV_URANDOM:
            /* All three silently discard writes — matches Linux. */
            return (ssize_t)count;
        case DEV_TTY:
        case DEV_MOUSE:
            return -1;
        default:
            return -1;
    }
    (void)buf;
}

void devfs_init(void) {
    memset(devfs_opens, 0, sizeof(devfs_opens));
    prng_seed();
    pr_info("devfs: initialized (FD range %d..%d, devices: null zero urandom tty mouse)\n",
            DEVFS_FD_BASE, DEVFS_FD_BASE + DEVFS_MAX_OPEN - 1);
}
