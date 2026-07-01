/*
 * Lestra OS - Kernel Heap Allocator
 * Copyright (c) 2026 lestramk.org
 *
 * Simple buddy allocator for kernel heap.
 * Efficient for allocations from 16 bytes to 1MB.
 */

#include <lestra/types.h>
#include <lestra/mm.h>
#include <lestra/printk.h>
#include <lestra/panic.h>

/* Heap block header */
struct heap_block {
    size_t size;           /* Total size including header */
    bool used;
    struct heap_block* next;
    struct heap_block* prev;
    uint32_t magic;        /* For corruption detection */
};

#define HEAP_MAGIC  0x4B4E4C48  /* "HLNK" */

static struct heap_block* heap_start = NULL;
static struct heap_block* heap_end = NULL;
static size_t heap_total = 0;
static size_t heap_used = 0;

void heap_init(void) {
    /* Allocate initial heap region */
    phys_addr_t phys = pmm_alloc_pages(256);  /* 1MB initial heap */
    if (!phys) {
        panic("Heap: Failed to allocate initial heap");
    }

    heap_start = (struct heap_block*)(phys + KERNEL_VMA);
    heap_start->size = 256 * PAGE_SIZE;
    heap_start->used = false;
    heap_start->next = NULL;
    heap_start->prev = NULL;
    heap_start->magic = HEAP_MAGIC;

    heap_end = heap_start;
    heap_total = 256 * PAGE_SIZE;
    heap_used = 0;

    pr_debug("Heap initialized: 1MB at %p\n", (void*)heap_start);
}

static void split_block(struct heap_block* block, size_t size) {
    /* FIX: Validate size won't overflow pointer arithmetic */
    if (block->size < size + sizeof(struct heap_block) + 16) {
        return; /* Too small to split */
    }

    /* Safety check: ensure new block is within heap bounds */
    uintptr_t new_addr = (uintptr_t)block + size;
    if (new_addr < (uintptr_t)block || new_addr >= (uintptr_t)heap_start + heap_total) {
        return; /* Overflow or out of bounds */
    }

    struct heap_block* new_block = (struct heap_block*)new_addr;
    new_block->size = block->size - size;
    new_block->used = false;
    new_block->magic = HEAP_MAGIC;
    new_block->next = block->next;
    new_block->prev = block;

    if (block->next) {
        block->next->prev = new_block;
    } else {
        heap_end = new_block;
    }

    block->next = new_block;
    block->size = size;
}

static void coalesce(struct heap_block* block) {
    /* Coalesce with next block */
    if (block->next && !block->next->used) {
        block->size += block->next->size;
        block->next = block->next->next;
        if (block->next) {
            block->next->prev = block;
        } else {
            heap_end = block;
        }
    }

    /* Coalesce with previous block */
    if (block->prev && !block->prev->used) {
        block->prev->size += block->size;
        block->prev->next = block->next;
        if (block->next) {
            block->next->prev = block->prev;
        } else {
            heap_end = block->prev;
        }
    }
}

void* kmalloc(size_t size) {
    if (size == 0) return NULL;

    size_t total_size = ALIGN_UP(size + sizeof(struct heap_block), 16);

    /* Find first fit */
    struct heap_block* current = heap_start;
    while (current) {
        if (!current->used && current->size >= total_size) {
            if (current->magic != HEAP_MAGIC) {
                panic("Heap corruption detected!");
            }

            split_block(current, total_size);
            current->used = true;
            heap_used += current->size;

            return (void*)((uintptr_t)current + sizeof(struct heap_block));
        }
        current = current->next;
    }

    /* No fit found - try to extend heap */
    phys_addr_t phys = pmm_alloc_pages(64);  /* 256KB extension */
    if (!phys) return NULL;

    struct heap_block* new_block = (struct heap_block*)(phys + KERNEL_VMA);
    new_block->size = 64 * PAGE_SIZE;
    new_block->used = false;
    new_block->magic = HEAP_MAGIC;
    new_block->next = NULL;
    new_block->prev = heap_end;

    heap_end->next = new_block;
    heap_end = new_block;
    heap_total += 64 * PAGE_SIZE;

    /* Try allocation again */
    return kmalloc(size);
}

void* kcalloc(size_t count, size_t size) {
    /* FIX #6: Integer overflow check (CRITICAL) */
    if (count == 0 || size == 0) return NULL;
    if (size > SIZE_MAX / count) {
        pr_warn("kcalloc: size overflow (%zu * %zu)\n", count, size);
        return NULL;
    }

    size_t total = count * size;
    void* ptr = kmalloc(total);
    if (ptr) {
        uint8_t* p = (uint8_t*)ptr;
        for (size_t i = 0; i < total; i++) {
            p[i] = 0;
        }
    }
    return ptr;
}

void* krealloc(void* ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return NULL; }

    struct heap_block* block = (struct heap_block*)((uintptr_t)ptr - sizeof(struct heap_block));
    if (block->magic != HEAP_MAGIC) {
        panic("krealloc: Invalid block");
    }

    size_t old_size = block->size - sizeof(struct heap_block);
    if (old_size >= size) return ptr;

    void* new_ptr = kmalloc(size);
    if (new_ptr) {
        uint8_t* src = (uint8_t*)ptr;
        uint8_t* dst = (uint8_t*)new_ptr;
        for (size_t i = 0; i < old_size; i++) {
            dst[i] = src[i];
        }
        kfree(ptr);
    }
    return new_ptr;
}

void kfree(void* ptr) {
    if (!ptr) return;

    struct heap_block* block = (struct heap_block*)((uintptr_t)ptr - sizeof(struct heap_block));
    if (block->magic != HEAP_MAGIC) {
        panic("kfree: Invalid block or double free");
    }

    if (!block->used) {
        panic("kfree: Double free detected");
    }

    block->used = false;
    heap_used -= block->size;
    coalesce(block);
}

size_t ksize(void* ptr) {
    if (!ptr) return 0;
    struct heap_block* block = (struct heap_block*)((uintptr_t)ptr - sizeof(struct heap_block));
    if (block->magic != HEAP_MAGIC) return 0;
    return block->size - sizeof(struct heap_block);
}

void mm_print_stats(void) {
    pr_info("Memory Statistics:\n");
    pr_info("  Physical: %lu MB total, %lu MB used, %lu MB free\n",
            pmm_get_total() / MiB, pmm_get_used() / MiB, pmm_get_free() / MiB);
    pr_info("  Heap: %lu KB total, %lu KB used\n",
            heap_total / KiB, heap_used / KiB);
}
