/*
 * Lestra OS - Kernel Print Functions
 * Copyright (c) 2026 lestramk.org
 *
 * Formatted output for kernel debugging and user interaction.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/vga.h>
#include <lestra/serial.h>
#include <string.h>
#include <stdarg.h>

/* Cap for %s format specifier — matches the buffer cap below (PR #7 fix). */
#define PRINTK_MAX_STR 1024

/* KE-26: Kernel ring buffer (dmesg/kmsg equivalent).
 * Stores all printk output in a circular buffer for later retrieval via
 * /proc/kmsg. 16 KB is enough for ~200 lines of boot output. */
#define KMSG_SIZE 16384
static char kmsg_buf[KMSG_SIZE];
static size_t kmsg_write_idx = 0;  /* next write position (wraps) */
static size_t kmsg_len = 0;        /* total bytes written (caps at KMSG_SIZE) */

/* Write a character to the ring buffer. */
static void kmsg_write(char c) {
    kmsg_buf[kmsg_write_idx] = c;
    kmsg_write_idx = (kmsg_write_idx + 1) % KMSG_SIZE;
    if (kmsg_len < KMSG_SIZE) kmsg_len++;
}

/* Read the ring buffer into a user buffer. Returns bytes read.
 * If the buffer is larger than the available data, only the available
 * data is copied. The buffer is read from oldest to newest. */
size_t kmsg_read(char* buf, size_t max) {
    size_t to_copy = kmsg_len < max ? kmsg_len : max;
    size_t start;
    if (kmsg_len < KMSG_SIZE) {
        /* Buffer not yet full — start from 0 */
        start = 0;
    } else {
        /* Buffer is full — start from the oldest byte (write_idx) */
        start = kmsg_write_idx;
    }
    for (size_t i = 0; i < to_copy; i++) {
        buf[i] = kmsg_buf[(start + i) % KMSG_SIZE];
    }
    return to_copy;
}

/* Simple itoa for numbers */
static int itoa(int64_t value, char* buf, int base, bool uppercase) {
    char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[64];
    int i = 0, j = 0;
    bool negative = false;
    
    if (value < 0 && base == 10) {
        negative = true;
        value = -value;
    }
    
    if (value == 0) {
        tmp[i++] = '0';
    } else {
        while (value > 0) {
            tmp[i++] = digits[value % base];
            value /= base;
        }
    }
    
    if (negative) {
        buf[j++] = '-';
    }
    
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
    return j;
}

static int uitoa(uint64_t value, char* buf, int base, bool uppercase) {
    char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[64];
    int i = 0, j = 0;
    
    if (value == 0) {
        tmp[i++] = '0';
    } else {
        while (value > 0) {
            tmp[i++] = digits[value % base];
            value /= base;
        }
    }
    
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
    return j;
}

int print_dec(uint64_t value, char* buf, size_t bufsize) {
    (void)bufsize;
    return uitoa(value, buf, 10, false);
}

int print_hex(uint64_t value, char* buf, size_t bufsize, bool uppercase) {
    (void)bufsize;
    int len = uitoa(value, buf, 16, uppercase);
    return len;
}

int print_bin(uint64_t value, char* buf, size_t bufsize) {
    (void)bufsize;
    return uitoa(value, buf, 2, false);
}

void early_putchar(char c) {
    vga_putchar(c);
    serial_default_putchar(c);
}

void early_printk(const char* str) {
    while (*str) {
        early_putchar(*str++);
    }
}

static void print_char(char c) {
    kmsg_write(c);  /* KE-26: store in ring buffer for /proc/kmsg */
    vga_putchar(c);
    serial_default_putchar(c);
}

static void print_string(const char* str) {
    while (*str) {
        print_char(*str++);
    }
}

/* Bounded variant — writes at most `max` chars, then a '...' suffix if the
 * source was truncated. Returns the number of source characters consumed. */
static size_t print_string_n(const char* str, size_t max) {
    if (!str) return 0;
    size_t i = 0;
    while (str[i] && i < max) {
        print_char(str[i]);
        i++;
    }
    if (str[i]) {
        print_char('.');
        print_char('.');
        print_char('.');
    }
    return i + (str[i] ? 3 : 0);
}

int vprintk(const char* fmt, va_list args) {
    char buf[128];
    int count = 0;
    
    while (*fmt) {
        if (*fmt != '%') {
            if (*fmt == '\n') {
                print_char('\r');
                count++;
            }
            print_char(*fmt);
            fmt++;
            count++;
            continue;
        }
        
        fmt++;
        
        /* Parse format specifier */
        bool alternate = false;
        bool zero_pad = false;
        bool left_align = false;
        int width = 0;
        int length = 0; /* 0=none, 1=l, 2=ll */

        /* Flags: '-' (left-align), '#' (alternate), '0' (zero-pad) */
        while (*fmt == '-' || *fmt == '#' || *fmt == '0' || *fmt == ' ') {
            if (*fmt == '-') left_align = true;
            else if (*fmt == '#') alternate = true;
            else if (*fmt == '0') zero_pad = true;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if (*fmt == 'l') {
            fmt++;
            length = 1;
            if (*fmt == 'l') {
                fmt++;
                length = 2;
            }
        }

        (void)alternate; (void)zero_pad; (void)left_align; (void)length; /* width/flags noted but padding not implemented */
        
        switch (*fmt) {
            case 'c': {
                char c = (char)va_arg(args, int);
                print_char(c);
                count++;
                break;
            }
            case 's': {
                const char* s = va_arg(args, const char*);
                if (!s) s = "(null)";
                /* PR #7 fix: bound to PRINTK_MAX_STR to avoid runaway output
                 * when callers pass a non-null-terminated / very long string. */
                count += print_string_n(s, PRINTK_MAX_STR);
                break;
            }
            case 'd':
            case 'i': {
                int64_t val = va_arg(args, int64_t);
                int len = itoa(val, buf, 10, false);
                print_string(buf);
                count += len;
                break;
            }
            case 'u': {
                uint64_t val = va_arg(args, uint64_t);
                int len = uitoa(val, buf, 10, false);
                print_string(buf);
                count += len;
                break;
            }
            case 'x': {
                uint64_t val = va_arg(args, uint64_t);
                if (alternate) {
                    print_string("0x");
                    count += 2;
                }
                int len = uitoa(val, buf, 16, false);
                print_string(buf);
                count += len;
                break;
            }
            case 'X': {
                uint64_t val = va_arg(args, uint64_t);
                if (alternate) {
                    print_string("0x");
                    count += 2;
                }
                int len = uitoa(val, buf, 16, true);
                print_string(buf);
                count += len;
                break;
            }
            case 'p': {
                void* ptr = va_arg(args, void*);
                print_string("0x");
                int len = uitoa((uintptr_t)ptr, buf, 16, false);
                print_string(buf);
                count += len + 2;
                break;
            }
            case '%':
                print_char('%');
                count++;
                break;
            default:
                print_char('%');
                print_char(*fmt);
                count += 2;
                break;
        }
        fmt++;
    }
    
    return count;
}

int printk(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    /* Skip log level prefix if present */
    if (fmt[0] == '<' && fmt[1] >= '0' && fmt[1] <= '7' && fmt[2] == '>') {
        fmt += 3;
    }
    
    int ret = vprintk(fmt, args);
    va_end(args);
    return ret;
}

/* strlen is provided by libc/string.c — no local copy needed */

/* ----- in-kernel snprintf -----
 * Minimal but correct: supports %s, %d, %u, %x, %c, %%, and the
 * 'l' / 'll' length modifiers. Width/precision are ignored. Returns
 * the number of chars that WOULD have been written (excluding NUL). */
int ksnprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    size_t i = 0;   /* chars written so far (excluding NUL) */
    char tmp[32];

    auto_flush:;
    while (*fmt) {
        if (*fmt != '%') {
            if (i + 1 < size) buf[i] = *fmt;
            i++;
            fmt++;
            continue;
        }
        fmt++;
        /* Skip flags / width / precision (we ignore them but must consume) */
        while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#'
            || *fmt == '0' || (*fmt >= '0' && *fmt <= '9') || *fmt == '.') {
            fmt++;
        }
        /* Length modifiers */
        int is_long = 0;   /* 0=int, 1=long, 2=long long */
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') { is_long = 2; fmt++; } }
        else if (*fmt == 'z') { is_long = 1; fmt++; }

        char spec = *fmt++;
        switch (spec) {
            case '%':
                if (i + 1 < size) buf[i] = '%';
                i++;
                break;
            case 'c': {
                char c = (char)va_arg(args, int);
                if (i + 1 < size) buf[i] = c;
                i++;
                break;
            }
            case 's': {
                const char* s = va_arg(args, const char*);
                if (!s) s = "(null)";
                while (*s) {
                    if (i + 1 < size) buf[i] = *s;
                    i++;
                    s++;
                }
                break;
            }
            case 'd':
            case 'i': {
                int64_t v;
                if (is_long == 2) v = va_arg(args, long long);
                else if (is_long == 1) v = va_arg(args, long);
                else v = va_arg(args, int);
                /* itoa-style: write to tmp then copy */
                int neg = 0;
                uint64_t u;
                if (v < 0) { neg = 1; u = (uint64_t)(-(v + 1)) + 1u; }
                else u = (uint64_t)v;
                int tlen = 0;
                if (u == 0) tmp[tlen++] = '0';
                while (u) { tmp[tlen++] = '0' + (u % 10); u /= 10; }
                if (neg) tmp[tlen++] = '-';
                /* reverse */
                for (int k = 0; k < tlen / 2; k++) {
                    char x = tmp[k]; tmp[k] = tmp[tlen-1-k]; tmp[tlen-1-k] = x;
                }
                for (int k = 0; k < tlen; k++) {
                    if (i + 1 < size) buf[i] = tmp[k];
                    i++;
                }
                break;
            }
            case 'u': {
                uint64_t v;
                if (is_long == 2) v = va_arg(args, unsigned long long);
                else if (is_long == 1) v = va_arg(args, unsigned long);
                else v = va_arg(args, unsigned int);
                int tlen = 0;
                if (v == 0) tmp[tlen++] = '0';
                while (v) { tmp[tlen++] = '0' + (v % 10); v /= 10; }
                for (int k = 0; k < tlen / 2; k++) {
                    char x = tmp[k]; tmp[k] = tmp[tlen-1-k]; tmp[tlen-1-k] = x;
                }
                for (int k = 0; k < tlen; k++) {
                    if (i + 1 < size) buf[i] = tmp[k];
                    i++;
                }
                break;
            }
            case 'x':
            case 'X': {
                uint64_t v;
                if (is_long == 2) v = va_arg(args, unsigned long long);
                else if (is_long == 1) v = va_arg(args, unsigned long);
                else v = va_arg(args, unsigned int);
                const char* digits = (spec == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
                int tlen = 0;
                if (v == 0) tmp[tlen++] = '0';
                while (v) { tmp[tlen++] = digits[v & 0xF]; v >>= 4; }
                for (int k = 0; k < tlen / 2; k++) {
                    char x = tmp[k]; tmp[k] = tmp[tlen-1-k]; tmp[tlen-1-k] = x;
                }
                for (int k = 0; k < tlen; k++) {
                    if (i + 1 < size) buf[i] = tmp[k];
                    i++;
                }
                break;
            }
            case 'p': {
                uint64_t v = (uint64_t)va_arg(args, void*);
                if (i + 1 < size) buf[i] = '0'; i++;
                if (i + 1 < size) buf[i] = 'x'; i++;
                const char* digits = "0123456789abcdef";
                int tlen = 0;
                if (v == 0) tmp[tlen++] = '0';
                while (v) { tmp[tlen++] = digits[v & 0xF]; v >>= 4; }
                for (int k = 0; k < tlen / 2; k++) {
                    char x = tmp[k]; tmp[k] = tmp[tlen-1-k]; tmp[tlen-1-k] = x;
                }
                for (int k = 0; k < tlen; k++) {
                    if (i + 1 < size) buf[i] = tmp[k];
                    i++;
                }
                break;
            }
            default:
                /* unknown specifier - emit literally */
                if (i + 1 < size) buf[i] = '%';
                i++;
                if (i + 1 < size) buf[i] = spec;
                i++;
                break;
        }
    }
    if (size > 0) {
        buf[i < size ? i : size - 1] = '\0';
    }
    va_end(args);
    return (int)i;
}
