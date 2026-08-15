/*
 * Lestra OS - C Standard Library - stdio
 * Copyright (c) 2026 lestramk.org
 *
 * W3-A rewrite:
 *   - struct FILE is now a real buffered-stream descriptor (fd, mode
 *     flags, eof/err state, I/O buffer, and a `next` pointer used by
 *     fflush(NULL) to walk all open streams).
 *   - stdin/stdout/stderr are real FILE* objects (defined in
 *     libc/src/stdio.c as static FILEs pre-populated with fds 0/1/2),
 *     not the bare ((FILE*)N) sentinels they used to be.  This makes
 *     fprintf(stdout, ...) and fputc('x', stderr) actually route to
 *     the right fd.
 *   - fopen/fclose/fread/fwrite/fgetc/fputc/fputs/fflush/feof/ferror/
 *     clearerr are now declared AND defined (previously declared only
 *     — W1-A finding D).
 */
#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

/* Mode-flag bits stored in FILE.flags (also referenced from stdio.c). */
#define _IO_READ       0x0001
#define _IO_WRITE      0x0002
#define _IO_BUF_OWNER  0x0004   /* buffer was malloc'd; free on close */
#define _IO_BUF_DIRTY  0x0008   /* write buffer has unflushed bytes */
#define _IO_EOF_FLAG   0x0010
#define _IO_ERR_FLAG   0x0020

struct FILE {
    int    fd;          /* underlying file descriptor */
    int    flags;       /* _IO_* mode + state bits */
    int    err;         /* nonzero if I/O error occurred (legacy field) */
    int    eof;         /* nonzero if end-of-file reached (legacy field) */
    char*  buf;         /* I/O buffer (NULL = unbuffered) */
    size_t buf_size;    /* capacity of buf */
    size_t buf_pos;     /* read cursor OR write cursor */
    size_t buf_len;     /* valid bytes in buf (for reads) */
    struct FILE* next;  /* next entry in the open-files list */
};
typedef struct FILE FILE;

#define EOF (-1)

/* Standard streams — real FILE* objects defined in libc/src/stdio.c.
 * Their buffers are allocated lazily on first use. */
extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

/* File operations — now both declared and defined in libc/src/stdio.c. */
FILE*  fopen(const char* path, const char* mode);
int    fclose(FILE* stream);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
int    fgetc(FILE* stream);
int    fputc(int c, FILE* stream);
int    fputs(const char* s, FILE* stream);
int    fflush(FILE* stream);
int    feof(FILE* stream);
int    ferror(FILE* stream);
void   clearerr(FILE* stream);

/* Formatted I/O */
int printf(const char* fmt, ...);
int fprintf(FILE* stream, const char* fmt, ...);
int sprintf(char* str, const char* fmt, ...);
int snprintf(char* str, size_t size, const char* fmt, ...);
int vprintf(const char* fmt, va_list ap);
int vfprintf(FILE* stream, const char* fmt, va_list ap);
int vsprintf(char* str, const char* fmt, va_list ap);
int vsnprintf(char* str, size_t size, const char* fmt, va_list ap);

/* Character I/O */
int putchar(int c);
int puts(const char* s);
int getchar(void);

#endif /* LIBC_STDIO_H */
