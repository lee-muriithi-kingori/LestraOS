/*
 * Lestra OS - Kernel Heap Allocator
 * Copyright (c) 2026 lestramk.org
 *
 * Simple linked-list allocator over the kernel heap region.
 */

#include <lestra/types.h>
#include <lestra/mm.h>
#include <lestra/printk.h>
#include <lestra/panic.h>
#include <string.h>

/* Page table root, defined by the boot stub. The kernel runs identity-
 * mapped, so heap addresses in [KERNEL_HEAP_START, KERNEL_HEAP_END) are
 * already backed by physical memory of the same address (boot.asm maps
 * the first 1GB using 2MB huge pages). */
extern uint64_t boot_pml4[512];

/* Heap block header */
struct heap_block {
    size_t size;                /* Total size including header */
    struct heap_block* next;    /* Next free block */
    int free;                   /* 1 = free, 0 = used */
};

static struct heap_block* free_list = NULL;
static uintptr_t heap_next = KERNEL_HEAP_START;

void heap_init(void) {
    free_list = NULL;
    heap_next = KERNEL_HEAP_START;

    /* FIX: reserve the heap region in PMM so it doesn't get allocated
     * for other uses (page tables, etc.). The heap uses identity-mapped
     * memory at [KERNEL_HEAP_START, KERNEL_HEAP_END). */
    extern void pmm_reserve_region(uintptr_t start, uintptr_t end);
    pmm_reserve_region(KERNEL_HEAP_START, KERNEL_HEAP_END);

    pr_info("Heap initialized at 0x%x (size: %u MB)\n",
            (unsigned)KERNEL_HEAP_START,
            (unsigned)((KERNEL_HEAP_END - KERNEL_HEAP_START) / MiB));
}

void* kmalloc(size_t size) {
    if (!size) return NULL;

    size_t total = sizeof(struct heap_block) + ALIGN_UP(size, 16);

    /* Try to find a free block */
    struct heap_block* prev = NULL;
    struct heap_block* curr = free_list;
    while (curr) {
        if (curr->free && curr->size >= total) {
            curr->free = 0;
            /* Split if block is significantly larger */
            if (curr->size >= total + sizeof(struct heap_block) + 16) {
                struct heap_block* new_block = (struct heap_block*)((char*)curr + total);
                new_block->size = curr->size - total;
                new_block->free = 1;
                new_block->next = curr->next;
                curr->size = total;
                curr->next = new_block;
            }
            return (char*)curr + sizeof(struct heap_block);
        }
        prev = curr;
        curr = curr->next;
    }

    /* Allocate from heap */
    if (heap_next + total > KERNEL_HEAP_END) {
        pr_warn("kmalloc: out of heap memory\n");
        return NULL;
    }

    /* The heap region is identity-mapped via 2MB huge pages in boot.asm,
     * so we don't need to call vmm_map_page here. Just bump-allocate. */

    struct heap_block* block = (struct heap_block*)heap_next;
    block->size = total;
    block->free = 0;
    block->next = NULL;

    heap_next += total;
    return (char*)block + sizeof(struct heap_block);
}

void* kcalloc(size_t count, size_t size) {
    if (count && size > SIZE_MAX / count) return NULL;
    size_t total = count * size;
    void* ptr = kmalloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void* krealloc(void* ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (!size) { kfree(ptr); return NULL; }

    struct heap_block* block = (struct heap_block*)((char*)ptr - sizeof(struct heap_block));
    if (block->size >= sizeof(struct heap_block) + size) return ptr;

    void* new_ptr = kmalloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, block->size - sizeof(struct heap_block));
        kfree(ptr);
    }
    return new_ptr;
}

void kfree(void* ptr) {
    if (!ptr) return;
    struct heap_block* block = (struct heap_block*)((char*)ptr - sizeof(struct heap_block));
    block->free = 1;

    /* Coalesce with next block if free */
    if (block->next && block->next->free) {
        block->size += block->next->size;
        block->next = block->next->next;
    }

    /* Add to free list */
    block->next = free_list;
    free_list = block;
}

size_t ksize(void* ptr) {
    if (!ptr) return 0;
    struct heap_block* block = (struct heap_block*)((char*)ptr - sizeof(struct heap_block));
    return block->size - sizeof(struct heap_block);
}

uintptr_t heap_get_used(void) {
    return heap_next - KERNEL_HEAP_START;
}

void mm_print_stats(void) {
    pr_info("Memory: Total=%u MB, Used=%u MB, Free=%u MB\n",
            (unsigned)(pmm_get_total() / MiB),
            (unsigned)(pmm_get_used() / MiB),
            (unsigned)(pmm_get_free() / MiB));
    pr_info("Heap: Used=%u KB\n",
            (unsigned)(heap_get_used() / KiB));
}
