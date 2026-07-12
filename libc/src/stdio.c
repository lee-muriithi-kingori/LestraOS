/*
 * Lestra OS - stdio Implementation
 * Copyright (c) 2026 lestramk.org
 *
 * Full printf-family implementation with width, precision, flags, and
 * length modifiers. Supports:
 *
 *   %[flags][width][.precision][length]specifier
 *
 *   flags:     - + space # 0
 *   width:     decimal or "*"
 *   precision: .decimal or ".*"
 *   length:    hh h l ll z j t
 *   specifier: d i u x X o c s p % n
 *
 * The implementation is freestanding and uses only libc's own
 * string.h + unistd.h. No floats (the kernel is built with -mno-sse).
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>

/* Syscall wrapper */
extern int64_t syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

int putchar(int c) {
    char ch = (char)c;
    syscall(SYS_WRITE, STDOUT_FILENO, (uint64_t)&ch, 1, 0, 0);
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
    syscall(SYS_READ, STDIN_FILENO, (uint64_t)&c, 1, 0, 0);
    return c;
}

/* ===== printf format spec ===== */

struct fmt_spec {
    /* flags */
    int left_align;     /* '-' */
    int plus_sign;      /* '+' */
    int space_sign;     /* ' ' */
    int alt_form;       /* '#' */
    int zero_pad;       /* '0' */
    /* width / precision (-1 = not specified) */
    int width;
    int precision;
    /* length modifiers */
    int len;            /* 0=int, 1=l, 2=ll, 3=h, 4=hh, 5=z, 6=j, 7=t */
};

enum {
    LEN_NONE = 0,
    LEN_l    = 1,
    LEN_ll   = 2,
    LEN_h    = 3,
    LEN_hh   = 4,
    LEN_z    = 5,
    LEN_j    = 6,
    LEN_t    = 7,
};

static const char* parse_flags(const char* p, struct fmt_spec* s) {
    for (;;) {
        switch (*p) {
            case '-': s->left_align = 1; p++; continue;
            case '+': s->plus_sign  = 1; p++; continue;
            case ' ': s->space_sign = 1; p++; continue;
            case '#': s->alt_form   = 1; p++; continue;
            case '0': s->zero_pad   = 1; p++; continue;
        }
        break;
    }
    return p;
}

static const char* parse_width(const char* p, struct fmt_spec* s, va_list* ap) {
    if (*p == '*') {
        int w = va_arg(*ap, int);
        if (w < 0) { s->left_align = 1; w = -w; }
        s->width = w;
        return p + 1;
    }
    if (*p >= '0' && *p <= '9') {
        int w = 0;
        while (*p >= '0' && *p <= '9') {
            w = w * 10 + (*p - '0');
            p++;
        }
        s->width = w;
    }
    return p;
}

static const char* parse_precision(const char* p, struct fmt_spec* s, va_list* ap) {
    if (*p != '.') return p;
    p++;
    s->precision = 0;
    if (*p == '*') {
        int pr = va_arg(*ap, int);
        if (pr < 0) pr = -1;     /* negative precision = no precision */
        s->precision = pr;
        return p + 1;
    }
    int pr = 0;
    while (*p >= '0' && *p <= '9') {
        pr = pr * 10 + (*p - '0');
        p++;
    }
    s->precision = pr;
    return p;
}

static const char* parse_length(const char* p, struct fmt_spec* s) {
    switch (*p) {
        case 'h':
            if (p[1] == 'h') { s->len = LEN_hh; return p + 2; }
            s->len = LEN_h; return p + 1;
        case 'l':
            if (p[1] == 'l') { s->len = LEN_ll; return p + 2; }
            s->len = LEN_l; return p + 1;
        case 'z': s->len = LEN_z; return p + 1;
        case 'j': s->len = LEN_j; return p + 1;
        case 't': s->len = LEN_t; return p + 1;
    }
    return p;
}

/* Write `c` `n` times to the output. */
static void emit_padding(char c, int n,
                         char* out, size_t size, size_t* written) {
    for (int i = 0; i < n; i++) {
        if (out && *written < size - 1) out[*written] = c;
        (*written)++;
    }
}

static void emit_str(const char* s, int n,
                     char* out, size_t size, size_t* written) {
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\0') break;
        if (out && *written < size - 1) out[*written] = c;
        (*written)++;
    }
}

/* Format an integer into a temporary buffer (reversed order, then
 * flipped). Returns the number of digits written. `base` is 8/10/16.
 * `uppercase` affects hex letters. */
static int format_uint(uint64_t val, int base, int uppercase,
                       char* buf, int bufsz) {
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    if (val == 0) {
        buf[0] = '0';
        return 1;
    }
    while (val > 0 && n < bufsz) {
        buf[n++] = digits[val % (uint64_t)base];
        val /= (uint64_t)base;
    }
    /* Reverse. */
    for (int i = 0; i < n / 2; i++) {
        char t = buf[i];
        buf[i] = buf[n - 1 - i];
        buf[n - 1 - i] = t;
    }
    return n;
}

/* Emit a string field with width/precision/flag handling. */
static void emit_string_field(const char* s, struct fmt_spec* sp,
                              char* out, size_t size, size_t* written) {
    if (!s) s = "(null)";
    int slen = 0;
    while (s[slen]) slen++;
    if (sp->precision >= 0 && sp->precision < slen) slen = sp->precision;

    int pad = sp->width - slen;
    if (pad < 0) pad = 0;
    if (sp->left_align) {
        emit_str(s, slen, out, size, written);
        emit_padding(' ', pad, out, size, written);
    } else {
        emit_padding(' ', pad, out, size, written);
        emit_str(s, slen, out, size, written);
    }
}

/* Emit an integer field with width/precision/flag handling. */
static void emit_int_field(uint64_t uv, int is_negative, int base,
                           int uppercase, int alt_form, int has_sign_char,
                           char sign_char, struct fmt_spec* sp,
                           char* out, size_t size, size_t* written) {
    char digits[32];
    int ndig = format_uint(uv, base, uppercase, digits, (int)sizeof(digits));

    /* Precision: minimum number of digits (zero-pad on the left to
     * reach it). If precision is 0 and value is 0, no digits. */
    int min_digits = (sp->precision >= 0) ? sp->precision : -1;
    int zeros_for_prec = 0;
    if (min_digits >= 0) {
        if (min_digits > ndig) zeros_for_prec = min_digits - ndig;
        if (min_digits == 0 && uv == 0) ndig = 0;
    }

    /* Prefix: "0x"/"0X" for # with x/X, "0" for # with o (only when
     * the value doesn't already start with 0). */
    const char* prefix = "";
    int prefix_len = 0;
    char prefix_buf[3];
    if (alt_form && base == 16 && uv != 0) {
        prefix_buf[0] = '0';
        prefix_buf[1] = uppercase ? 'X' : 'x';
        prefix_len = 2;
    } else if (alt_form && base == 8 && uv != 0 && ndig > 0 &&
               digits[0] != '0') {
        prefix_buf[0] = '0';
        prefix_len = 1;
    }
    if (prefix_len) {
        prefix_buf[prefix_len] = '\0';
        prefix = prefix_buf;
    }

    int sign_len = 0;
    if (is_negative) { sign_len = 1; }
    else if (has_sign_char) { sign_len = 1; }

    int content_len = sign_len + prefix_len + zeros_for_prec + ndig;
    int pad = sp->width - content_len;
    if (pad < 0) pad = 0;

    /* If zero_pad is set and no explicit precision, zero-fill between
     * prefix and digits. Otherwise space-pad on the left (or right if
     * left_align). */
    int use_zero = sp->zero_pad && !sp->left_align && min_digits < 0;

    if (sp->left_align) {
        if (is_negative) emit_str("-", 1, out, size, written);
        else if (has_sign_char) emit_str(&sign_char, 1, out, size, written);
        emit_str(prefix, prefix_len, out, size, written);
        emit_padding('0', zeros_for_prec, out, size, written);
        emit_str(digits, ndig, out, size, written);
        emit_padding(' ', pad, out, size, written);
    } else if (use_zero) {
        if (is_negative) emit_str("-", 1, out, size, written);
        else if (has_sign_char) emit_str(&sign_char, 1, out, size, written);
        emit_str(prefix, prefix_len, out, size, written);
        emit_padding('0', pad + zeros_for_prec, out, size, written);
        emit_str(digits, ndig, out, size, written);
    } else {
        emit_padding(' ', pad, out, size, written);
        if (is_negative) emit_str("-", 1, out, size, written);
        else if (has_sign_char) emit_str(&sign_char, 1, out, size, written);
        emit_str(prefix, prefix_len, out, size, written);
        emit_padding('0', zeros_for_prec, out, size, written);
        emit_str(digits, ndig, out, size, written);
    }
}

static int handle_specifier(const char** pp, va_list* ap,
                            char* out, size_t size, size_t* written) {
    const char* p = *pp;
    struct fmt_spec sp;
    memset(&sp, 0, sizeof(sp));
    sp.width = -1;
    sp.precision = -1;

    p = parse_flags(p, &sp);
    p = parse_width(p, &sp, ap);
    p = parse_precision(p, &sp, ap);
    p = parse_length(p, &sp);

    char spec = *p;
    if (spec == '\0') {
        /* malformed; stop */
        *pp = p;
        return 0;
    }
    p++;

    switch (spec) {
        case 'd':
        case 'i': {
            long long val;
            switch (sp.len) {
                case LEN_l:  val = (long long)va_arg(*ap, long); break;
                case LEN_ll: val = va_arg(*ap, long long); break;
                case LEN_h:  val = (short)va_arg(*ap, int); break;
                case LEN_hh: val = (signed char)va_arg(*ap, int); break;
                case LEN_z:  val = (long long)va_arg(*ap, long); break; /* ssize_t */
                case LEN_j:  val = (long long)va_arg(*ap, long long); break;
                case LEN_t:  val = (long long)va_arg(*ap, long); break;
                default:     val = (long long)va_arg(*ap, int); break;
            }
            int neg = val < 0;
            uint64_t uv = neg ? (uint64_t)(-(val + 1)) + 1ULL : (uint64_t)val;
            char sign_char = neg ? '-' : (sp.plus_sign ? '+' :
                                          (sp.space_sign ? ' ' : '\0'));
            int has_sign = (sign_char != '\0');
            emit_int_field(uv, neg, 10, 0, 0, has_sign && !neg,
                           sp.plus_sign ? '+' : ' ', &sp,
                           out, size, written);
            break;
        }
        case 'u': {
            uint64_t val;
            switch (sp.len) {
                case LEN_l:  val = (uint64_t)va_arg(*ap, unsigned long); break;
                case LEN_ll: val = va_arg(*ap, unsigned long long); break;
                case LEN_h:  val = (unsigned short)va_arg(*ap, unsigned int); break;
                case LEN_hh: val = (unsigned char)va_arg(*ap, unsigned int); break;
                case LEN_z:  val = (uint64_t)va_arg(*ap, size_t); break;
                case LEN_j:  val = (uint64_t)va_arg(*ap, uintmax_t); break;
                case LEN_t:  val = (uint64_t)va_arg(*ap, long); break;
                default:     val = (uint64_t)va_arg(*ap, unsigned int); break;
            }
            emit_int_field(val, 0, 10, 0, 0, 0, '\0', &sp,
                           out, size, written);
            break;
        }
        case 'x':
        case 'X': {
            uint64_t val;
            switch (sp.len) {
                case LEN_l:  val = (uint64_t)va_arg(*ap, unsigned long); break;
                case LEN_ll: val = va_arg(*ap, unsigned long long); break;
                case LEN_h:  val = (unsigned short)va_arg(*ap, unsigned int); break;
                case LEN_hh: val = (unsigned char)va_arg(*ap, unsigned int); break;
                case LEN_z:  val = (uint64_t)va_arg(*ap, size_t); break;
                case LEN_j:  val = (uint64_t)va_arg(*ap, uintmax_t); break;
                case LEN_t:  val = (uint64_t)va_arg(*ap, long); break;
                default:     val = (uint64_t)va_arg(*ap, unsigned int); break;
            }
            int upper = (spec == 'X');
            emit_int_field(val, 0, 16, upper, sp.alt_form, 0, '\0', &sp,
                           out, size, written);
            break;
        }
        case 'o': {
            uint64_t val;
            switch (sp.len) {
                case LEN_l:  val = (uint64_t)va_arg(*ap, unsigned long); break;
                case LEN_ll: val = va_arg(*ap, unsigned long long); break;
                case LEN_h:  val = (unsigned short)va_arg(*ap, unsigned int); break;
                case LEN_hh: val = (unsigned char)va_arg(*ap, unsigned int); break;
                case LEN_z:  val = (uint64_t)va_arg(*ap, size_t); break;
                case LEN_j:  val = (uint64_t)va_arg(*ap, uintmax_t); break;
                case LEN_t:  val = (uint64_t)va_arg(*ap, long); break;
                default:     val = (uint64_t)va_arg(*ap, unsigned int); break;
            }
            emit_int_field(val, 0, 8, 0, sp.alt_form, 0, '\0', &sp,
                           out, size, written);
            break;
        }
        case 'c': {
            char c = (char)va_arg(*ap, int);
            int pad = sp.width - 1;
            if (pad < 0) pad = 0;
            if (sp.left_align) {
                emit_str(&c, 1, out, size, written);
                emit_padding(' ', pad, out, size, written);
            } else {
                emit_padding(' ', pad, out, size, written);
                emit_str(&c, 1, out, size, written);
            }
            break;
        }
        case 's': {
            const char* s = va_arg(*ap, const char*);
            emit_string_field(s, &sp, out, size, written);
            break;
        }
        case 'p': {
            void* ptr = va_arg(*ap, void*);
            uintptr_t val = (uintptr_t)ptr;
            /* %p is "0x%" PRIxPTR */
            sp.alt_form = 1;
            emit_int_field((uint64_t)val, 0, 16, 0, 1, 0, '\0', &sp,
                           out, size, written);
            break;
        }
        case '%': {
            emit_str("%", 1, out, size, written);
            break;
        }
        case 'n': {
            /* Write the number of chars written so far into the
             * pointer arg. Length modifier selects the pointed type. */
            switch (sp.len) {
                case LEN_l:  *(long*)va_arg(*ap, void*) = (long)*written; break;
                case LEN_ll: *(long long*)va_arg(*ap, void*) = (long long)*written; break;
                case LEN_h:  *(short*)va_arg(*ap, void*) = (short)*written; break;
                case LEN_hh: *(signed char*)va_arg(*ap, void*) = (signed char)*written; break;
                case LEN_z:  *(size_t*)va_arg(*ap, void*) = (size_t)*written; break;
                case LEN_j:  *(uintmax_t*)va_arg(*ap, void*) = (uintmax_t)*written; break;
                case LEN_t:  *(long*)va_arg(*ap, void*) = (long)*written; break;
                default:     *(int*)va_arg(*ap, void*) = (int)*written; break;
            }
            break;
        }
        default:
            /* Unknown specifier: emit the '%' and the char verbatim. */
            emit_str("%", 1, out, size, written);
            emit_str(&spec, 1, out, size, written);
            break;
    }

    *pp = p;
    return 1;
}

int vsnprintf(char* str, size_t size, const char* fmt, va_list ap) {
    size_t written = 0;
    const char* p = fmt;

    /* Make a writable copy of the va_list since we need to pass it by
     * pointer to helpers. */
    va_list ap_local;
    va_copy(ap_local, ap);

    while (*p) {
        if (*p != '%') {
            if (str && written < size - 1) str[written] = *p;
            written++;
            p++;
            continue;
        }
        p++;  /* skip '%' */
        if (!handle_specifier(&p, &ap_local, str, size, &written)) {
            break;
        }
    }
    if (str) {
        if (size > 0) {
            size_t term = (written < size - 1) ? written : size - 1;
            str[term] = '\0';
        }
    }
    va_end(ap_local);
    return (int)written;
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
    int ret = vsnprintf(str, (size_t)-1, fmt, ap);
    va_end(ap);
    return ret;
}

int vprintf(const char* fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) return -1;
    int to_write = n;
    if ((size_t)to_write > sizeof(buf) - 1) to_write = (int)sizeof(buf) - 1;
    syscall(SYS_WRITE, STDOUT_FILENO, (uint64_t)buf, (uint64_t)to_write, 0, 0);
    return n;
}

int vfprintf(FILE* stream, const char* fmt, va_list ap) {
    (void)stream;
    return vprintf(fmt, ap);
}

int vsprintf(char* str, const char* fmt, va_list ap) {
    return vsnprintf(str, (size_t)-1, fmt, ap);
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
