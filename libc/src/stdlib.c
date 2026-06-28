/*
 * Lestra OS - stdlib Implementation
 * Copyright (c) 2026 lestramk.org
 */

#include <stdlib.h>
#include <string.h>

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
    
    /* Allocate new memory from kernel */
    extern int64_t syscall(uint64_t, ...);
    void* mem = (void*)syscall(9, total);  /* SYS_BRK */
    if (!mem) return NULL;
    
    struct block* new_block = (struct block*)mem;
    new_block->size = total;
    new_block->free = 0;
    new_block->next = NULL;
    
    if (prev) prev->next = new_block;
    else free_list = new_block;
    
    return (char*)new_block + sizeof(struct block);
}

void* calloc(size_t nmemb, size_t size) {
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
    int result = 0;
    int sign = 1;
    while (*nptr == ' ' || *nptr == '\t') nptr++;
    if (*nptr == '-') { sign = -1; nptr++; }
    else if (*nptr == '+') nptr++;
    while (*nptr >= '0' && *nptr <= '9') {
        result = result * 10 + (*nptr - '0');
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
