/*
 * Lestra OS - stdlib Implementation
 * Copyright (c) 2026 lestramk.org
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <lestra/types.h>

/* Simple heap using sbrk */
static char* heap_start = NULL;
static char* heap_end = NULL;
static char* heap_max = NULL;

/* Simple block header */
struct block {
    size_t size;
    int free;
    struct block* next;
};

static struct block* free_list = NULL;

/* Internal heap sbrk - tracks heap end for user space */
static void* sbrk(size_t increment) {
    if (!heap_start) {
        /* First allocation - request initial heap from kernel */
        extern int64_t syscall(uint64_t, ...);
        void* base = (void*)syscall(9, 0);  /* SYS_BRK with 0 = get current break */
        if (!base) {
            /* Fallback: use a static heap area */
            static char fallback_heap[1024 * 1024];  /* 1MB static fallback */
            heap_start = fallback_heap;
            heap_end = fallback_heap;
            heap_max = fallback_heap + sizeof(fallback_heap);
        } else {
            heap_start = base;
            heap_end = base;
            heap_max = base + (1024 * 1024);  /* 1MB user heap */
        }
    }

    if (increment == 0) return heap_end;

    if (heap_end + increment > heap_max) return (void*)-1;

    void* old_end = heap_end;
    heap_end += increment;
    return old_end;
}

void* malloc(size_t size) {
    if (!size) return NULL;

    size_t total = sizeof(struct block) + ALIGN_UP(size, 16);
    struct block* curr = free_list;
    struct block* prev = NULL;

    while (curr) {
        if (curr->free && curr->size >= total) {
            curr->free = 0;
            return (char*)curr + sizeof(struct block);
        }
        prev = curr;
        curr = curr->next;
    }

    /* Allocate new memory from heap */
    void* mem = sbrk(total);
    if (mem == (void*)-1) return NULL;

    struct block* new_block = (struct block*)mem;
    new_block->size = total;
    new_block->free = 0;
    new_block->next = NULL;

    if (prev) prev->next = new_block;
    else free_list = new_block;

    return (char*)new_block + sizeof(struct block);
}

void* calloc(size_t nmemb, size_t size) {
    /* FIX: Integer overflow check (Issue #1 - CRITICAL) */
    if (nmemb && size > SIZE_MAX / nmemb) return NULL;

    size_t total = nmemb * size;
    void* ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void free(void* ptr) {
    if (!ptr) return;
    struct block* block = (struct block*)((char*)ptr - sizeof(struct block));
    block->free = 1;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (!size) { free(ptr); return NULL; }

    struct block* block = (struct block*)((char*)ptr - sizeof(struct block));
    if (block->size >= size) return ptr;

    void* new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, block->size - sizeof(struct block));
        free(ptr);
    }
    return new_ptr;
}

int atoi(const char* nptr) {
    /* FIX: Integer overflow prevention (Issue #2 - MEDIUM) */
    int result = 0;
    int sign = 1;
    while (*nptr == ' ' || *nptr == '\t') nptr++;
    if (*nptr == '-') { sign = -1; nptr++; }
    else if (*nptr == '+') nptr++;
    while (*nptr >= '0' && *nptr <= '9') {
        int digit = *nptr - '0';
        /* Check for overflow before multiplication */
        if (result > (INT_MAX - digit) / 10) {
            return (sign == 1) ? INT_MAX : INT_MIN;
        }
        result = result * 10 + digit;
        nptr++;
    }
    return sign * result;
}

void exit(int status) {
    extern int64_t syscall(uint64_t, ...);
    syscall(0, status);  /* SYS_EXIT */
    while (1);
}

void abort(void) {
    exit(1);
}

static unsigned int rand_seed = 1;

int rand(void) {
    rand_seed = rand_seed * 1103515245 + 12345;
    return (rand_seed >> 16) & 0x7FFF;
}

void srand(unsigned int seed) {
    rand_seed = seed;
}

/* atexit handlers */
#define MAX_ATEXIT 32
static void (*atexit_handlers[MAX_ATEXIT])(void);
static int atexit_count = 0;

int atexit(void (*func)(void)) {
    if (!func || atexit_count >= MAX_ATEXIT) return -1;
    atexit_handlers[atexit_count++] = func;
    return 0;
}

void __call_atexit(void) {
    while (atexit_count > 0) {
        atexit_count--;
        if (atexit_handlers[atexit_count])
            atexit_handlers[atexit_count]();
    }
}
