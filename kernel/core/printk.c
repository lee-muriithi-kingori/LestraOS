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
#include <stdarg.h>

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
    vga_putchar(c);
    serial_default_putchar(c);
}

static void print_string(const char* str) {
    while (*str) {
        print_char(*str++);
    }
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
        int width = 0;
        
        if (*fmt == '#') {
            alternate = true;
            fmt++;
        }
        if (*fmt == '0') {
            zero_pad = true;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        
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
                print_string(s);
                count += strlen(s);
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

/* Simple string functions needed by printk */
size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}
