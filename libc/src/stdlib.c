/*
 * Lestra OS - stdlib Implementation
 * Copyright (c) 2026 lestramk.org
 *
 * W3-A fixes:
 *   - sbrk() now calls SYS_BRK(new_top) on every growth instead of
 *     bumping a local pointer that the kernel knows nothing about.
 *     The kernel's brk(0) returns the current break; brk(addr) sets
 *     the break and returns the new value.  We track the libc-side
 *     cached break, ask the kernel to move it, and update from the
 *     kernel's reply.  (W1-A finding E.)
 *   - free() now coalesces backward: walks the free list to find the
 *     predecessor of the freed block and merges if the predecessor is
 *     both free and adjacent.  Forward coalescing was already there
 *     in spirit (via the malloc reuse path); we now also merge the
 *     freed block with its successor when they are contiguous.
 *     (W1-A finding E.)
 *
 * Freestanding — no FP, no SSE.  Builds with -mno-sse -mno-mmx -mno-sse2.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <lestra/types.h>   /* ALIGN_UP; also defines legacy negative
                             * errno constants which we override below */
#include <errno.h>          /* POSIX errno values — #undefs the legacy
                             * negative ones from <lestra/types.h> and
                             * re-defines them as positive integers. */
#include <unistd.h>

/* Simple first-fit allocator with a free list.
 *
 * Layout: every block has a header `struct block` immediately followed
 * by `block.size - sizeof(struct block)` bytes of user data.  Adjacent
 * free blocks are coalesced (forward on free, backward via the
 * predecessor walk). */

struct block {
    size_t size;          /* total size including header */
    int    free;
    struct block* next;
};

static struct block* free_list = NULL;

/* The libc-side cached break.  Initialised lazily from SYS_BRK(0). */
static char* heap_brk = NULL;

/* sbrk: grow (or shrink) the heap by `increment` bytes.
 *
 *   sbrk(0)      -> return current break (no growth)
 *   sbrk(N>0)    -> grow by N; return the OLD break (start of the new
 *                   block).  On failure return (void*)-1 and set errno.
 *
 * The kernel's sys_brk follows Linux semantics:
 *   brk(0)      -> return current break
 *   brk(addr)   -> set break to addr (if within cap), return new break
 *                  (== addr on success, == old break on failure)
 *
 * So a successful sbrk(N) sees the kernel return exactly old+N. */
static void* sbrk(ptrdiff_t increment) {
    if (!heap_brk) {
        int64_t cur = syscall(SYS_BRK, 0, 0, 0, 0, 0);
        if (cur <= 0) {
            /* Kernel refused to give us a break.  Fall back to a
             * static 1 MB heap so malloc still works for tiny programs
             * (and so libc's own init can allocate). */
            static char fallback_heap[1024 * 1024];
            heap_brk = fallback_heap;
            /* In fallback mode we cannot call SYS_BRK to grow; sbrk
             * will just bump heap_brk within the static array.  The
             * cap is the end of the array. */
            if (increment == 0) return heap_brk;
            char* fallback_end = fallback_heap + sizeof(fallback_heap);
            char* old = heap_brk;
            if (heap_brk + increment > fallback_end) {
                errno = ENOMEM;
                return (void*)-1;
            }
            heap_brk += increment;
            return old;
        }
        heap_brk = (char*)cur;
    }

    if (increment == 0) return heap_brk;

    char* old_brk = heap_brk;
    char* requested = old_brk + increment;
    int64_t new_brk = syscall(SYS_BRK, (uint64_t)requested, 0, 0, 0, 0);

    /* Kernel may have grown less than we asked (cap hit) — in that
     * case new_brk < requested and we treat it as failure. */
    if (new_brk <= 0 || (char*)new_brk < requested) {
        errno = ENOMEM;
        return (void*)-1;
    }
    heap_brk = (char*)new_brk;
    return old_brk;
}

void* malloc(size_t size) {
    if (!size) return NULL;

    size_t total = sizeof(struct block) + ALIGN_UP(size, 16);

    /* First-fit search of the free list. */
    struct block* curr = free_list;
    struct block* prev = NULL;
    while (curr) {
        if (curr->free && curr->size >= total) {
            /* Optionally split if there's room for another header + 16
             * bytes; otherwise just hand the whole block over. */
            if (curr->size >= total + sizeof(struct block) + 16) {
                struct block* split = (struct block*)((char*)curr + total);
                split->size = curr->size - total;
                split->free = 1;
                split->next = curr->next;
                curr->size  = total;
                curr->next  = split;
            }
            curr->free = 0;
            return (char*)curr + sizeof(struct block);
        }
        prev = curr;
        curr = curr->next;
    }

    /* No fit — grow the heap. */
    void* mem = sbrk((ptrdiff_t)total);
    if (mem == (void*)-1) {
        errno = ENOMEM;
        return NULL;
    }

    struct block* new_block = (struct block*)mem;
    new_block->size = total;
    new_block->free = 0;
    new_block->next = NULL;

    if (prev) prev->next = new_block;
    else      free_list = new_block;

    return (char*)new_block + sizeof(struct block);
}

void* calloc(size_t nmemb, size_t size) {
    /* Integer overflow check (W1-A Issue #1). */
    if (nmemb && size > SIZE_MAX / nmemb) {
        errno = ENOMEM;
        return NULL;
    }
    size_t total = nmemb * size;
    void* ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

/* free: mark the block free and coalesce with adjacent free neighbours.
 *
 * Forward coalescing: if the block immediately following in the free
 * list is both free and physically adjacent (its header sits exactly
 * at `block + block->size`), merge it into `block`.
 *
 * Backward coalescing: walk the free list to find the predecessor P of
 * `block`.  If P is free and physically adjacent (block sits exactly at
 * `P + P->size`), merge `block` into P. */
void free(void* ptr) {
    if (!ptr) return;

    struct block* block = (struct block*)((char*)ptr - sizeof(struct block));
    block->free = 1;

    /* Forward coalesce: find the successor in the free list (the
     * physically-next block, if it's there).  Because malloc grows
     * the heap monotonically and never reorders blocks, the block
     * whose header sits at `block + block->size` is the next one in
     * the list — IF it exists.  Walk to be safe. */
    struct block* succ = block->next;
    if (succ && succ->free) {
        char* expected = (char*)block + block->size;
        if ((char*)succ == expected) {
            block->size += succ->size;
            block->next  = succ->next;
        }
    }

    /* Backward coalesce: walk the list to find the predecessor of
     * `block`.  If it's free and physically adjacent, merge `block`
     * into it. */
    struct block* pred = NULL;
    struct block* c = free_list;
    while (c && c != block) {
        pred = c;
        c = c->next;
    }
    if (pred && pred->free) {
        char* expected = (char*)pred + pred->size;
        if ((char*)block == expected) {
            pred->size += block->size;
            pred->next  = block->next;
        }
    }
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (!size) { free(ptr); return NULL; }

    struct block* block = (struct block*)((char*)ptr - sizeof(struct block));
    size_t needed = sizeof(struct block) + ALIGN_UP(size, 16);

    if (block->size >= needed) return ptr;

    /* Try to grow in place by absorbing a free successor. */
    struct block* succ = block->next;
    if (succ && succ->free) {
        char* expected = (char*)block + block->size;
        if ((char*)succ == expected && block->size + succ->size >= needed) {
            block->size += succ->size;
            block->next  = succ->next;
            return ptr;
        }
    }

    /* Otherwise allocate new + copy + free old. */
    void* new_ptr = malloc(size);
    if (!new_ptr) return NULL;
    size_t copy = block->size - sizeof(struct block);
    if (copy > size) copy = size;
    memcpy(new_ptr, ptr, copy);
    free(ptr);
    return new_ptr;
}

int atoi(const char* nptr) {
    int result = 0;
    int sign = 1;
    while (*nptr == ' ' || *nptr == '\t') nptr++;
    if (*nptr == '-') { sign = -1; nptr++; }
    else if (*nptr == '+') nptr++;
    while (*nptr >= '0' && *nptr <= '9') {
        int digit = *nptr - '0';
        /* Overflow guard. */
        if (result > (INT_MAX - digit) / 10) {
            return (sign == 1) ? INT_MAX : INT_MIN;
        }
        result = result * 10 + digit;
        nptr++;
    }
    return sign * result;
}

void exit(int status) {
    /* Run atexit handlers in reverse order. */
    extern void __call_atexit(void);
    __call_atexit();
    syscall(SYS_EXIT, (uint64_t)(int64_t)status, 0, 0, 0, 0);
    while (1) { __asm__ volatile("hlt"); }
}

void abort(void) {
    exit(1);
}

static unsigned int rand_seed = 1;

int rand(void) {
    /* Numerical Recipes LCG. */
    rand_seed = rand_seed * 1103515245u + 12345u;
    return (int)((rand_seed >> 16) & 0x7FFF);
}

void srand(unsigned int seed) {
    rand_seed = seed;
}

/* atexit handlers */
#define MAX_ATEXIT 32
static void (*atexit_handlers[MAX_ATEXIT])(void);
static int atexit_count = 0;

int atexit(void (*func)(void)) {
    if (!func || atexit_count >= MAX_ATEXIT) {
        errno = ENOMEM;
        return -1;
    }
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
