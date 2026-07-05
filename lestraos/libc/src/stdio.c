/*
 * Lestra OS - stdio Implementation
 * Copyright (c) 2026 lestramk.org
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>

/* Syscall wrapper */
extern int64_t syscall(uint64_t num, ...);

int putchar(int c) {
    char ch = (char)c;
    syscall(SYS_WRITE, STDOUT_FILENO, &ch, 1);
    return c;
}

int puts(const char* s) {
    int n = 0;
    while (s[n]) {
        putchar(s[n]);
        n++;
    }
    putchar('\n');
    return n + 1;
}

int getchar(void) {
    char c;
    syscall(SYS_READ, STDIN_FILENO, &c, 1);
    return c;
}

/* Simple vsnprintf */
int vsnprintf(char* str, size_t size, const char* fmt, va_list ap) {
    size_t i = 0;
    
    while (*fmt && i < size - 1) {
        if (*fmt != '%') {
            str[i++] = *fmt++;
            continue;
        }
        fmt++;
        
        switch (*fmt) {
            case 'c': {
                str[i++] = (char)va_arg(ap, int);
                break;
            }
            case 's': {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                while (*s && i < size - 1) str[i++] = *s++;
                break;
            }
            case 'd': {
                int val = va_arg(ap, int);
                char buf[32];
                int neg = val < 0;
                if (neg) val = -val;
                int n = 0;
                do { buf[n++] = '0' + (val % 10); val /= 10; } while (val);
                if (neg) buf[n++] = '-';
                while (n-- && i < size - 1) str[i++] = buf[n];
                break;
            }
            case 'u': {
                unsigned val = va_arg(ap, unsigned);
                char buf[32];
                int n = 0;
                do { buf[n++] = '0' + (val % 10); val /= 10; } while (val);
                while (n-- && i < size - 1) str[i++] = buf[n];
                break;
            }
            case 'x': {
                unsigned val = va_arg(ap, unsigned);
                char buf[32];
                int n = 0;
                do {
                    int digit = val & 0xF;
                    buf[n++] = "0123456789abcdef"[digit];
                    val >>= 4;
                } while (val);
                while (n-- && i < size - 1) str[i++] = buf[n];
                break;
            }
            case 'p': {
                void* p = va_arg(ap, void*);
                uintptr_t val = (uintptr_t)p;
                str[i++] = '0';
                if (i < size - 1) str[i++] = 'x';
                char buf[32];
                int n = 0;
                do {
                    int digit = val & 0xF;
                    buf[n++] = "0123456789abcdef"[digit];
                    val >>= 4;
                } while (val);
                while (n-- && i < size - 1) str[i++] = buf[n];
                break;
            }
            case '%':
                str[i++] = '%';
                break;
            default:
                str[i++] = *fmt;
                break;
        }
        fmt++;
    }
    
    str[i] = '\0';
    return i;
}

int snprintf(char* str, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char* str, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(str, 0xFFFFFFFF, fmt, ap);
    va_end(ap);
    return ret;
}

int vprintf(const char* fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    syscall(SYS_WRITE, STDOUT_FILENO, buf, n);
    return n;
}

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vprintf(fmt, ap);
    va_end(ap);
    return ret;
}

int fprintf(FILE* stream, const char* fmt, ...) {
    (void)stream;
    va_list ap;
    va_start(ap, fmt);
    int ret = vprintf(fmt, ap);
    va_end(ap);
    return ret;
}
