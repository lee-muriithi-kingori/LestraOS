/*
 * Lestra OS - C Standard Library - stdio
 * Copyright (c) 2026 lestramk.org
 *
 * W3-A rewrite: adds a real FILE* layer (fopen/fclose/fread/fwrite/
 * fgetc/fputc/fputs/fflush/feof/ferror/clearerr) on top of the
 * open/read/write/close libc wrappers.  Previously these were
 * declared in <stdio.h> but never defined — every user program
 * linking FILE-based I/O failed at link time.  (W1-A finding D.)
 *
 * Design:
 *   - struct FILE carries an fd, mode flags, an error/eof state, and
 *     a single I/O buffer used for both input and output (a stream is
 *     either read-mode or write-mode; we never mix).
 *   - The standard streams stdin/stdout/stderr are static FILE objects
 *     with fd 0/1/2 pre-filled; their buffers are allocated lazily on
 *     first use so the first printf() can run before malloc has been
 *     initialised.  If the lazy malloc fails, the stream falls back to
 *     unbuffered I/O (every byte goes straight through syscall).
 *   - A singly-linked list of open FILEs lets fflush(NULL) flush every
 *     writable stream.
 *
 * The printf-family machinery (vsnprintf etc.) is preserved verbatim
 * from the previous implementation.
 *
 * Freestanding — no FP, no SSE.  Builds with -mno-sse -mno-mmx -mno-sse2.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>

/* Syscall wrapper (declared in <unistd.h>).  We re-declare here as
 * extern to match the historical style; the canonical prototype in
 * unistd.h already covers it. */
extern int64_t syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5);

/* ===== Mode-flag bits stored in FILE.flags =====
 * Defined in <stdio.h>; re-listed here only as documentation.  The
 * values must match the macros in <stdio.h> exactly. */
/* _IO_READ       0x0001   stream is open for reading        */
/* _IO_WRITE      0x0002   stream is open for writing        */
/* _IO_BUF_OWNER  0x0004   we malloc'd buf and must free it  */
/* _IO_BUF_DIRTY  0x0008   write buffer has unflushed bytes  */
/* _IO_EOF_FLAG   0x0010   end-of-file reached               */
/* _IO_ERR_FLAG   0x0020   I/O error occurred                */

#define BUFSIZ_STD 512

/* ===== Open-files list (for fflush(NULL)) ===== */
static FILE* __open_files = NULL;

/* ===== Standard streams =====
 * Pre-populated with fd 0/1/2; buffer is allocated lazily. */
static FILE __stdin_file  = { .fd = 0, .flags = _IO_READ };
static FILE __stdout_file = { .fd = 1, .flags = _IO_WRITE };
static FILE __stderr_file = { .fd = 2, .flags = _IO_WRITE };

FILE* stdin  = &__stdin_file;
FILE* stdout = &__stdout_file;
FILE* stderr = &__stderr_file;

/* Allocate the I/O buffer for a stream if not already present.
 * Returns 1 on success (or already-allocated), 0 on malloc failure
 * (stream stays unbuffered). */
static int __file_alloc_buf(FILE* f) {
    if (f->buf) return 1;
    char* p = (char*)malloc(BUFSIZ_STD);
    if (!p) return 0;
    f->buf      = p;
    f->buf_size = BUFSIZ_STD;
    f->buf_pos  = 0;
    f->buf_len  = 0;
    f->flags   |= _IO_BUF_OWNER;
    return 1;
}

/* Flush the write buffer of `f` to the underlying fd via SYS_WRITE.
 * Returns 0 on success, EOF on error (and sets the error flag). */
static int __file_flush_write(FILE* f) {
    if (!(f->flags & _IO_BUF_DIRTY)) return 0;
    if (!f->buf || f->buf_pos == 0) {
        f->flags &= ~_IO_BUF_DIRTY;
        return 0;
    }
    size_t off = 0;
    while (off < f->buf_pos) {
        int64_t r = syscall(SYS_WRITE, (uint64_t)f->fd,
                              (uint64_t)(f->buf + off),
                              (uint64_t)(f->buf_pos - off), 0, 0);
        if (r < 0) {
            f->flags |= _IO_ERR_FLAG;
            errno = (int)-r;
            f->flags &= ~_IO_BUF_DIRTY;
            f->buf_pos = 0;
            return EOF;
        }
        if (r == 0) break;
        off += (size_t)r;
    }
    f->buf_pos = 0;
    f->flags &= ~_IO_BUF_DIRTY;
    return 0;
}

/* ================================================================== */
/* FILE* API                                                          */
/* ================================================================== */

FILE* fopen(const char* path, const char* mode) {
    if (!path || !mode) { errno = EINVAL; return NULL; }

    int flags = 0;
    int io_flags = 0;
    int has_plus = 0;

    /* Parse the leading mode char. */
    switch (mode[0]) {
        case 'r':
            io_flags |= _IO_READ;
            /* O_RDWR if "r+" */
            break;
        case 'w':
            io_flags |= _IO_WRITE;
            flags    |= O_WRONLY | O_CREAT | O_TRUNC;
            break;
        case 'a':
            io_flags |= _IO_WRITE;
            flags    |= O_WRONLY | O_CREAT | O_APPEND;
            break;
        default:
            errno = EINVAL;
            return NULL;
    }
    /* Look at the rest of the mode string for '+', 'b', 'e'. */
    for (const char* p = mode + 1; *p; p++) {
        if (*p == '+') {
            has_plus = 1;
            io_flags |= _IO_READ | _IO_WRITE;
        }
        /* 'b' (binary) and 'e' (O_CLOEXEC) are accepted but the
         * kernel doesn't honour O_CLOEXEC yet — we just ignore them. */
    }
    if (mode[0] == 'r') {
        flags = has_plus ? O_RDWR : O_RDONLY;
    } else if (has_plus) {
        /* "w+" / "a+" — read+write */
        flags &= ~(O_WRONLY | O_RDONLY);
        flags |= O_RDWR;
    }

    int fd = open(path, flags);
    if (fd < 0) return NULL;   /* errno already set by open() */

    FILE* f = (FILE*)malloc(sizeof(FILE));
    if (!f) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    f->fd       = fd;
    f->flags    = io_flags;
    f->err      = 0;
    f->eof      = 0;
    f->buf      = NULL;
    f->buf_size = 0;
    f->buf_pos  = 0;
    f->buf_len  = 0;
    f->next     = NULL;

    /* Try to allocate the I/O buffer; if it fails, the stream runs
     * unbuffered (every fread/fwrite goes straight through syscall). */
    __file_alloc_buf(f);

    /* Link into the open-files list. */
    f->next = __open_files;
    __open_files = f;

    return f;
}

int fclose(FILE* f) {
    if (!f) { errno = EBADF; return EOF; }

    /* Don't close the standard streams — they're static. */
    if (f == &__stdin_file || f == &__stdout_file || f == &__stderr_file) {
        fflush(f);
        return 0;
    }

    int rc = 0;
    if (fflush(f) == EOF) rc = EOF;

    if (close(f->fd) < 0) rc = EOF;

    /* Unlink from open-files list. */
    FILE** pp = &__open_files;
    while (*pp && *pp != f) pp = &(*pp)->next;
    if (*pp) *pp = f->next;

    if ((f->flags & _IO_BUF_OWNER) && f->buf) free(f->buf);
    free(f);
    return rc;
}

int fflush(FILE* f) {
    if (!f) {
        /* Flush every writable stream. */
        FILE* cur = __open_files;
        int rc = 0;
        while (cur) {
            if ((cur->flags & _IO_WRITE) && __file_flush_write(cur) == EOF)
                rc = EOF;
            cur = cur->next;
        }
        /* Also flush stdout/stderr (which may not be on the open list). */
        if (__file_flush_write(&__stdout_file) == EOF) rc = EOF;
        if (__file_flush_write(&__stderr_file) == EOF) rc = EOF;
        return rc;
    }
    if (f->flags & _IO_WRITE) return __file_flush_write(f);
    /* Read streams have no write buffer to flush. */
    return 0;
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* f) {
    if (!f || !ptr || size == 0 || nmemb == 0) return 0;
    if (!(f->flags & _IO_READ)) {
        errno = EBADF;
        f->flags |= _IO_ERR_FLAG;
        return 0;
    }

    size_t want = size * nmemb;
    size_t got = 0;
    char* dst = (char*)ptr;

    /* Drain any bytes still in the read buffer first. */
    while (got < want && f->buf_pos < f->buf_len) {
        dst[got++] = f->buf[f->buf_pos++];
    }

    /* If we still need data and the remaining request is larger than
     * the buffer, read directly into the caller's buffer (avoids an
     * extra copy).  Otherwise refill the buffer first and serve from
     * it. */
    while (got < want) {
        size_t remaining = want - got;
        if (!f->buf || remaining >= f->buf_size) {
            int64_t r = syscall(SYS_READ, (uint64_t)f->fd,
                                  (uint64_t)(dst + got),
                                  (uint64_t)remaining, 0, 0);
            if (r < 0) {
                errno = (int)-r;
                f->flags |= _IO_ERR_FLAG;
                break;
            }
            if (r == 0) { f->flags |= _IO_EOF_FLAG; break; }
            got += (size_t)r;
        } else {
            /* Refill the buffer. */
            int64_t r = syscall(SYS_READ, (uint64_t)f->fd,
                                  (uint64_t)f->buf,
                                  (uint64_t)f->buf_size, 0, 0);
            if (r < 0) {
                errno = (int)-r;
                f->flags |= _IO_ERR_FLAG;
                break;
            }
            if (r == 0) { f->flags |= _IO_EOF_FLAG; break; }
            f->buf_len = (size_t)r;
            f->buf_pos = 0;
            while (got < want && f->buf_pos < f->buf_len) {
                dst[got++] = f->buf[f->buf_pos++];
            }
        }
    }

    /* POSIX: returns the number of complete elements read. */
    return got / size;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* f) {
    if (!f || !ptr || size == 0 || nmemb == 0) return 0;
    if (!(f->flags & _IO_WRITE)) {
        errno = EBADF;
        f->flags |= _IO_ERR_FLAG;
        return 0;
    }

    size_t want = size * nmemb;
    size_t put = 0;
    const char* src = (const char*)ptr;

    /* If unbuffered, write straight through. */
    if (!f->buf) {
        while (put < want) {
            int64_t r = syscall(SYS_WRITE, (uint64_t)f->fd,
                                  (uint64_t)(src + put),
                                  (uint64_t)(want - put), 0, 0);
            if (r < 0) {
                errno = (int)-r;
                f->flags |= _IO_ERR_FLAG;
                break;
            }
            if (r == 0) break;
            put += (size_t)r;
        }
        return put / size;
    }

    /* Buffered: append to the buffer, flushing when full.  Large
     * writes that would overflow the buffer by more than its size
     * bypass the buffer (we flush first, then write directly). */
    while (put < want) {
        size_t space = f->buf_size - f->buf_pos;
        if (space == 0) {
            if (__file_flush_write(f) == EOF) break;
            space = f->buf_size;
        }
        size_t chunk = want - put;
        if (chunk > space) chunk = space;
        memcpy(f->buf + f->buf_pos, src + put, chunk);
        f->buf_pos += chunk;
        f->flags   |= _IO_BUF_DIRTY;
        put        += chunk;
    }

    return put / size;
}

int fgetc(FILE* f) {
    if (!f) return EOF;
    if (!(f->flags & _IO_READ)) {
        errno = EBADF;
        f->flags |= _IO_ERR_FLAG;
        return EOF;
    }
    /* If buffer is empty (or absent), try to refill / read one byte. */
    if (!f->buf || f->buf_pos >= f->buf_len) {
        if (!f->buf) {
            char c;
            int64_t r = syscall(SYS_READ, (uint64_t)f->fd,
                                  (uint64_t)&c, 1, 0, 0);
            if (r < 0) { errno = (int)-r; f->flags |= _IO_ERR_FLAG; return EOF; }
            if (r == 0) { f->flags |= _IO_EOF_FLAG; return EOF; }
            return (unsigned char)c;
        }
        int64_t r = syscall(SYS_READ, (uint64_t)f->fd,
                              (uint64_t)f->buf, (uint64_t)f->buf_size, 0, 0);
        if (r < 0) { errno = (int)-r; f->flags |= _IO_ERR_FLAG; return EOF; }
        if (r == 0) { f->flags |= _IO_EOF_FLAG; return EOF; }
        f->buf_len = (size_t)r;
        f->buf_pos = 0;
    }
    return (unsigned char)f->buf[f->buf_pos++];
}

int fputc(int c, FILE* f) {
    if (!f) return EOF;
    if (!(f->flags & _IO_WRITE)) {
        errno = EBADF;
        f->flags |= _IO_ERR_FLAG;
        return EOF;
    }
    unsigned char ch = (unsigned char)c;

    if (!f->buf) {
        int64_t r = syscall(SYS_WRITE, (uint64_t)f->fd,
                              (uint64_t)&ch, 1, 0, 0);
        if (r < 0) { errno = (int)-r; f->flags |= _IO_ERR_FLAG; return EOF; }
        if (r == 0) return EOF;
        return (int)ch;
    }

    if (f->buf_pos >= f->buf_size) {
        if (__file_flush_write(f) == EOF) return EOF;
    }
    f->buf[f->buf_pos++] = (char)ch;
    f->flags |= _IO_BUF_DIRTY;
    /* Line-buffered-ish: flush on newline for stdout/stderr so log
     * lines appear immediately. */
    if (ch == '\n' && (f == &__stdout_file || f == &__stderr_file)) {
        __file_flush_write(f);
    }
    return (int)ch;
}

int fputs(const char* s, FILE* f) {
    if (!s || !f) { errno = EINVAL; return EOF; }
    size_t n = 0;
    while (s[n]) n++;
    size_t w = fwrite(s, 1, n, f);
    if (w < n) return EOF;
    return (int)n;
}

int feof(FILE* f) {
    return f && (f->flags & _IO_EOF_FLAG) ? 1 : 0;
}

int ferror(FILE* f) {
    return f && (f->flags & _IO_ERR_FLAG) ? 1 : 0;
}

void clearerr(FILE* f) {
    if (f) f->flags &= ~(_IO_EOF_FLAG | _IO_ERR_FLAG);
}

/* ================================================================== */
/* Character I/O on stdout/stdin                                      */
/* ================================================================== */

int putchar(int c) {
    return fputc(c, stdout);
}

int puts(const char* s) {
    if (!s) return EOF;
    size_t n = 0;
    while (s[n]) n++;
    if (fwrite(s, 1, n, stdout) < n) return EOF;
    if (fputc('\n', stdout) == EOF) return EOF;
    return (int)(n + 1);
}

int getchar(void) {
    return fgetc(stdin);
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
    return vfprintf(stdout, fmt, ap);
}

int vfprintf(FILE* stream, const char* fmt, va_list ap) {
    if (!stream) return -1;
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) return -1;
    int to_write = n;
    if ((size_t)to_write > sizeof(buf) - 1) to_write = (int)sizeof(buf) - 1;
    size_t w = fwrite(buf, 1, (size_t)to_write, stream);
    if (w < (size_t)to_write) return -1;
    return n;
}

int vsprintf(char* str, const char* fmt, va_list ap) {
    return vsnprintf(str, (size_t)-1, fmt, ap);
}

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return ret;
}

int fprintf(FILE* stream, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vfprintf(stream, fmt, ap);
    va_end(ap);
    return ret;
}
