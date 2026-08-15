/*
 * Lestra OS - C Standard Library - stdlib
 * Copyright (c) 2026 lestramk.org
 */
#ifndef LIBC_STDLIB_H
#define LIBC_STDLIB_H

#include <stddef.h>
#include <stdint.h>

/* Memory */
void* malloc(size_t size);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);
void free(void* ptr);

/* String conversion */
int atoi(const char* nptr);
long atol(const char* nptr);
long long atoll(const char* nptr);
int itoa(int value, char* str, int base);

/* Random */
int rand(void);
void srand(unsigned int seed);

/* Exit */
void exit(int status);
void abort(void);
int atexit(void (*func)(void));

/* Environment */
char* getenv(const char* name);

/* System */
int system(const char* command);

#endif /* LIBC_STDLIB_H */
