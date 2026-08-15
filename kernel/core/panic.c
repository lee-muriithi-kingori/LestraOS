/*
 * Lestra OS - Kernel Panic Handler
 * Copyright (c) 2026 lestramk.org
 */

#include <lestra/types.h>
#include <lestra/panic.h>
#include <lestra/printk.h>
#include <lestra/vga.h>
#include <stdarg.h>

__noreturn void panic(const char* msg) {
    cli();
    
    vga_set_color(VGA_WHITE, VGA_RED);
    vga_clear();
    
    printk("\n");
    printk("****************************************\n");
    printk("*        KERNEL PANIC                  *\n");
    printk("*                                      *\n");
    printk("*  Lestra OS has encountered a fatal   *\n");
    printk("*  error and cannot continue.          *\n");
    printk("*                                      *\n");
    printk("*  Error: %-28s *\n", msg);
    printk("*                                      *\n");
    printk("*  System halted.                      *\n");
    printk("****************************************\n");
    
    /* Halt the CPU */
    while (1) {
        hlt();
    }
}

__noreturn void panicf(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    
    /* Format the message */
    int i = 0;
    while (*fmt && i < 255) {
        if (*fmt == '%' && *(fmt+1)) {
            fmt++;
            switch (*fmt) {
                case 's': {
                    const char* s = va_arg(args, const char*);
                    while (*s && i < 255) buf[i++] = *s++;
                    break;
                }
                case 'd': {
                    int val = va_arg(args, int);
                    if (val < 0) { buf[i++] = '-'; val = -val; }
                    char tmp[16];
                    int ti = 0;
                    do { tmp[ti++] = '0' + (val % 10); val /= 10; } while (val);
                    while (ti > 0 && i < 255) buf[i++] = tmp[--ti];
                    break;
                }
                case 'x': {
                    unsigned val = va_arg(args, unsigned);
                    for (int n = 7; n >= 0 && i < 255; n--) {
                        int digit = (val >> (n * 4)) & 0xF;
                        if (digit || n == 0) buf[i++] = "0123456789abcdef"[digit];
                    }
                    break;
                }
                case 'p': {
                    void* p = va_arg(args, void*);
                    buf[i++] = '0'; buf[i++] = 'x';
                    uintptr_t val = (uintptr_t)p;
                    for (int n = 15; n >= 0 && i < 255; n--) {
                        int digit = (val >> (n * 4)) & 0xF;
                        buf[i++] = "0123456789abcdef"[digit];
                    }
                    break;
                }
                default:
                    buf[i++] = *fmt;
                    break;
            }
        } else {
            buf[i++] = *fmt;
        }
        fmt++;
    }
    buf[i] = '\0';
    
    va_end(args);
    panic(buf);
}

/* Global security status — populated at boot from kernel_main.c,
 * read by /proc/security and the boot-time SECURITY AUDIT print. */
struct security_status g_security = {0};

/* Stack canary support — compiled in via -fstack-protector-strong in Makefile.
 * The guard is initialized once at boot from the CSPRNG. */
uintptr_t __stack_chk_guard = 0x0BADF00DDEADBEEFull;  /* safe default before csprng */

__noreturn void __stack_chk_fail(void) {
    /* Called by GCC-generated epilogue code when the canary word has been smashed.
     * Unrecoverable — the kernel cannot trust its own stack. */
    panic("Stack smashing detected (__stack_chk_fail)");
}

void stack_canary_init(void) {
    extern uint64_t csprng_u64(void);
    __stack_chk_guard = csprng_u64();
    if ((__stack_chk_guard & 0xFF) == 0) __stack_chk_guard |= 0x5A;
    g_security.canaries = 1;
}
