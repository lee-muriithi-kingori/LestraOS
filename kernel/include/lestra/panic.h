/*
 * Lestra OS - Kernel Panic
 * Copyright (c) 2026 lestramk.org
 */

#ifndef LESTRA_PANIC_H
#define LESTRA_PANIC_H

#include <lestra/types.h>

/* Panic - halt the system with an error message */
__noreturn void panic(const char* msg);
__noreturn void panicf(const char* fmt, ...);

/* Assertion */
#define kassert(expr) \
    do { \
        if (!(expr)) { \
            panicf("Assertion failed: %s at %s:%d", \
                   #expr, __FILE__, __LINE__); \
        } \
    } while(0)

#define kassert_msg(expr, msg) \
    do { \
        if (!(expr)) { \
            panicf("Assertion failed: %s - %s at %s:%d", \
                   #expr, msg, __FILE__, __LINE__); \
        } \
    } while(0)

#endif /* LESTRA_PANIC_H */
