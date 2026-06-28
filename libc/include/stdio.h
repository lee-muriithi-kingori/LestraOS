/*
 * Lestra OS - C Standard Library - stdio
 * Copyright (c) 2026 lestramk.org
 */
#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct {
    int fd;
    int flags;
    int unget;
    int eof;
    int err;
    char* buf;
    size_t buf_size;
    size_t buf_pos;
} FILE;

#define EOF (-1)

#define stdin  ((FILE*)1)
#define stdout ((FILE*)2)
#define stderr ((FILE*)3)

/* File operations */
FILE* fopen(const char* path, const char* mode);
int fclose(FILE* stream);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
int fgetc(FILE* stream);
int fputc(int c, FILE* stream);
int fputs(const char* s, FILE* stream);
int fflush(FILE* stream);

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
