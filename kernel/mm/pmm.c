/*
 * Lestra OS - Physical Memory Manager
 * Copyright (c) 2026 lestramk.org
 *
 * Bitmap-based physical page allocator.
 * Optimized for systems with 4GB+ RAM.
 */

#include <lestra/types.h>
#include <lestra/mm.h>
#include <lestra/printk.h>
#include <lestra/panic.h>

/* Bitmap allocator - each bit represents one 4KB page */
static uint64_t* bitmap = NULL;
static size_t bitmap_size = 0;      /* Size of bitmap in bytes */
static size_t total_pages = 0;
static size_t used_pages = 0;
static uintptr_t highest_pfn = 0;   /* Highest page frame number */

/* Convert physical address to page frame number */
static inline size_t addr_to_pfn(phys_addr_t addr) {
    return addr / PAGE_SIZE;
}

/* Convert page frame number to bitmap index and bit */
static inline size_t pfn_to_idx(size_t pfn) {
    return pfn / 64;
}

static inline size_t pfn_to_bit(size_t pfn) {
    return pfn % 64;
}

static inline bool bitmap_test(size_t pfn) {
    return (bitmap[pfn_to_idx(pfn)] >> pfn_to_bit(pfn)) & 1;
}

static inline void bitmap_set(size_t pfn) {
    bitmap[pfn_to_idx(pfn)] |= (1ULL << pfn_to_bit(pfn));
}

static inline void bitmap_clear(size_t pfn) {
    bitmap[pfn_to_idx(pfn)] &= ~(1ULL << pfn_to_bit(pfn));
}

/* Mark a region of pages as used */
static void mark_region(phys_addr_t start, phys_addr_t end) {
    size_t start_pfn = addr_to_pfn(ALIGN_UP(start, PAGE_SIZE));
    size_t end_pfn = addr_to_pfn(ALIGN_DOWN(end, PAGE_SIZE));
    
    for (size_t pfn = start_pfn; pfn < end_pfn && pfn < highest_pfn; pfn++) {
        if (!bitmap_test(pfn)) {
            bitmap_set(pfn);
            used_pages++;
        }
    }
}

/* Mark a region of pages as free */
static void free_region(phys_addr_t start, phys_addr_t end) {
    size_t start_pfn = addr_to_pfn(ALIGN_UP(start, PAGE_SIZE));
    size_t end_pfn = addr_to_pfn(ALIGN_DOWN(end, PAGE_SIZE));
    
    for (size_t pfn = start_pfn; pfn < end_pfn && pfn < highest_pfn; pfn++) {
        if (bitmap_test(pfn)) {
            bitmap_clear(pfn);
            used_pages--;
        }
    }
}

void pmm_init(struct mmap_entry* mmap, uint32_t mmap_entries) {
    /* Find highest memory address */
    uint64_t highest_addr = 0;
    for (uint32_t i = 0; i < mmap_entries; i++) {
        if (mmap[i].base + mmap[i].length > highest_addr) {
            highest_addr = mmap[i].base + mmap[i].length;
        }
    }
    
    highest_pfn = highest_addr / PAGE_SIZE;
    total_pages = highest_pfn;
    bitmap_size = ALIGN_UP(highest_pfn / 8, PAGE_SIZE);
    
    /* Place bitmap just after the kernel (at 1MB + something) */
    extern char __kernel_end[];
    uintptr_t kernel_end = (uintptr_t)__kernel_end;
    bitmap = (uint64_t*)kernel_end;
    
    /* Clear bitmap (mark all as used by default) */
    for (size_t i = 0; i < bitmap_size / 8; i++) {
        bitmap[i] = ~0ULL;
    }
    used_pages = total_pages;
    
    /* Mark usable regions as free */
    uint64_t total_usable = 0;
    for (uint32_t i = 0; i < mmap_entries; i++) {
        if (mmap[i].type == MMAP_USABLE && mmap[i].length > 0) {
            free_region(mmap[i].base, mmap[i].base + mmap[i].length);
            total_usable += mmap[i].length;
            pr_debug("PMM: Usable region 0x%llx - 0x%llx (%llu MB)\n",
                     mmap[i].base, mmap[i].base + mmap[i].length,
                     mmap[i].length / MiB);
        }
    }
    
    /* Reserve the kernel and bitmap area */
    extern char __kernel_start[];
    phys_addr_t kernel_phys_start = (phys_addr_t)__kernel_start - KERNEL_VMA;
    phys_addr_t kernel_phys_end = ALIGN_UP(kernel_end + bitmap_size - KERNEL_VMA, PAGE_SIZE);
    mark_region(kernel_phys_start, kernel_phys_end);
    
    /* Reserve first page (NULL pointer protection) */
    bitmap_set(0);
    
    pr_info("PMM: %lu MB total, %lu MB usable\n",
            total_pages * PAGE_SIZE / MiB, total_usable / MiB);
}

phys_addr_t pmm_alloc_page(void) {
    for (size_t idx = 0; idx < bitmap_size / 8; idx++) {
        if (bitmap[idx] != ~0ULL) {
            for (size_t bit = 0; bit < 64; bit++) {
                size_t pfn = idx * 64 + bit;
                if (pfn >= highest_pfn) return 0;
                if (!bitmap_test(pfn)) {
                    bitmap_set(pfn);
                    used_pages++;
                    return pfn * PAGE_SIZE;
                }
            }
        }
    }
    return 0; /* Out of memory */
}

phys_addr_t pmm_alloc_pages(size_t count) {
    if (count == 1) return pmm_alloc_page();
    
    /* Simple first-fit for contiguous allocation */
    size_t found = 0;
    size_t start_pfn = 0;
    
    for (size_t pfn = 1; pfn < highest_pfn; pfn++) {
        if (!bitmap_test(pfn)) {
            if (found == 0) start_pfn = pfn;
            found++;
            if (found >= count) {
                for (size_t i = 0; i < count; i++) {
                    bitmap_set(start_pfn + i);
                }
                used_pages += count;
                return start_pfn * PAGE_SIZE;
            }
        } else {
            found = 0;
        }
    }
    return 0;
}

void pmm_free_page(phys_addr_t addr) {
    if (addr == 0) return;
    size_t pfn = addr_to_pfn(addr);
    if (pfn < highest_pfn && bitmap_test(pfn)) {
        bitmap_clear(pfn);
        used_pages--;
    }
}

void pmm_free_pages(phys_addr_t addr, size_t count) {
    for (size_t i = 0; i < count; i++) {
        pmm_free_page(addr + i * PAGE_SIZE);
    }
}

uintptr_t pmm_get_total(void) {
    return total_pages * PAGE_SIZE;
}

uintptr_t pmm_get_used(void) {
    return used_pages * PAGE_SIZE;
}

uintptr_t pmm_get_free(void) {
    return (total_pages - used_pages) * PAGE_SIZE;
}
