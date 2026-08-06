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
#include <string.h>

/* Bitmap allocator - each bit represents one 4KB page */
static uint64_t* bitmap = NULL;
static size_t bitmap_size = 0;      /* Size of bitmap in bytes */
static size_t total_pages = 0;
static size_t used_pages = 0;
static uintptr_t highest_pfn = 0;   /* Highest page frame number */

/* Physical page reference-count array (for COW fork).
 * Indexed by page frame number; each entry tracks how many PTEs
 * currently reference that physical page. When refcount drops to 0
 * via pmm_refcount_dec(), the page can be freed by pmm_free_page().
 * Set to 1 by pmm_alloc_page() for freshly allocated pages. */
static uint32_t* phys_refcount = NULL;
static size_t refcount_size = 0;   /* Size of refcount array in bytes */

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

/* Bounds checking for all bitmap operations */
static inline bool bitmap_test(size_t pfn) {
    if (pfn >= highest_pfn) return true; /* Treat as used (out of bounds) */
    return (bitmap[pfn_to_idx(pfn)] >> pfn_to_bit(pfn)) & 1;
}

static inline void bitmap_set(size_t pfn) {
    if (pfn >= highest_pfn) return;
    bitmap[pfn_to_idx(pfn)] |= (1ULL << pfn_to_bit(pfn));
}

static inline void bitmap_clear(size_t pfn) {
    if (pfn >= highest_pfn) return;
    bitmap[pfn_to_idx(pfn)] &= ~(1ULL << pfn_to_bit(pfn));
}

/* Mark a region of pages as used.
 * FIX: was ALIGN_DOWN(end) which missed the last partial page.
 * Use ALIGN_UP(end) so any page containing any byte of [start,end) is marked. */
static void mark_region(phys_addr_t start, phys_addr_t end) {
    if (end <= start) return;
    size_t start_pfn = addr_to_pfn(ALIGN_DOWN(start, PAGE_SIZE));
    size_t end_pfn = addr_to_pfn(ALIGN_UP(end, PAGE_SIZE));

    for (size_t pfn = start_pfn; pfn < end_pfn && pfn < highest_pfn; pfn++) {
        if (!bitmap_test(pfn)) {
            bitmap_set(pfn);
            used_pages++;
        }
    }
}

/* Mark a region of pages as free.
 * FIX: same ALIGN_DOWN -> ALIGN_UP correction. */
static void free_region(phys_addr_t start, phys_addr_t end) {
    if (end <= start) return;
    size_t start_pfn = addr_to_pfn(ALIGN_DOWN(start, PAGE_SIZE));
    size_t end_pfn = addr_to_pfn(ALIGN_UP(end, PAGE_SIZE));

    for (size_t pfn = start_pfn; pfn < end_pfn && pfn < highest_pfn; pfn++) {
        if (bitmap_test(pfn)) {
            bitmap_clear(pfn);
            used_pages--;
        }
    }
}

void pmm_init(struct mmap_entry* mmap, uint32_t mmap_entries) {
    /* Find highest USABLE memory address.
     *
     * FIX: previous code considered ALL mmap entries (including reserved
     * ones at very high addresses — QEMU puts a 1 TB-high 64-bit MMIO
     * reservation in the map). That made highest_pfn = 2^28, so
     * bitmap_size = ALIGN_UP(2^28 / 8, 4096) = 32 MB. The bitmap was
     * placed at __kernel_end and the "clear bitmap to all-used" loop
     * wrote 0xFFFFFFFF over 32 MB of low memory — clobbering GRUB's
     * multiboot2 info struct (which sits around 0x63A000). The kernel
     * then re-parsed the (zeroed) mmap, saw type=0xFFFFFFFF everywhere,
     * and reported 0 usable memory.
     *
     * Only considering USABLE entries gives highest_addr ~= 512 MB,
     * highest_pfn ~= 131000, bitmap_size ~= 20 KB. That fits well
     * below the GRUB data and is what we actually want — the bitmap
     * only needs to cover physical pages we might allocate. */
    uint64_t highest_addr = 0;
    for (uint32_t i = 0; i < mmap_entries; i++) {
        pr_debug("PMM: mmap[%u] base_hi=0x%x base_lo=0x%x len_hi=0x%x len_lo=0x%x type=%u\n",
                 (unsigned)i,
                 (unsigned)((mmap[i].base >> 32) & 0xFFFFFFFF),
                 (unsigned)(mmap[i].base & 0xFFFFFFFF),
                 (unsigned)((mmap[i].length >> 32) & 0xFFFFFFFF),
                 (unsigned)(mmap[i].length & 0xFFFFFFFF),
                 (unsigned)mmap[i].type);
        if (mmap[i].type == MMAP_USABLE &&
            mmap[i].base + mmap[i].length > highest_addr) {
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
            pr_debug("PMM: usable region 0x%x - 0x%x (%u MB)\n",
                     (unsigned)mmap[i].base,
                     (unsigned)(mmap[i].base + mmap[i].length),
                     (unsigned)(mmap[i].length / MiB));
        }
    }

    /* Reserve the kernel, bitmap, and refcount area */
    extern char __kernel_start[];
    phys_addr_t kernel_phys_start = (phys_addr_t)__kernel_start;

    /* Allocate refcount array right after the bitmap.
     * Each PFN needs a uint32_t (4 bytes). */
    refcount_size = ALIGN_UP(highest_pfn * sizeof(uint32_t), PAGE_SIZE);
    phys_refcount = (uint32_t*)(kernel_end + bitmap_size);
    memset(phys_refcount, 0, refcount_size);

    phys_addr_t kernel_phys_end = ALIGN_UP(kernel_end + bitmap_size + refcount_size, PAGE_SIZE);
    mark_region(kernel_phys_start, kernel_phys_end);

    /* Reserve first page (NULL pointer protection) */
    bitmap_set(0);

    pr_info("PMM: %u MB total, %u MB usable\n",
            (unsigned)(total_pages * PAGE_SIZE / MiB),
            (unsigned)(total_usable / MiB));
}

/* KE-33: Explicitly mark a physical page as used in the bitmap.
 * Used to protect page-table pages from being re-allocated by
 * pmm_alloc_page during deep_copy_user_pages. */
void pmm_mark_used(phys_addr_t phys) {
    size_t pfn = addr_to_pfn(phys);
    if (pfn >= highest_pfn) return;
    if (!bitmap_test(pfn)) {
        bitmap_set(pfn);
        used_pages++;
    }
    if (phys_refcount) phys_refcount[pfn] = 1;
}

phys_addr_t pmm_alloc_page(void) {
    for (size_t idx = 0; idx < bitmap_size / 8; idx++) {
        if (bitmap[idx] != ~0ULL) {
            for (size_t bit = 0; bit < 64; bit++) {
                size_t pfn = idx * 64 + bit;
                if (pfn >= highest_pfn) return 0;
                if (!bitmap_test(pfn)) {
                    phys_addr_t candidate = pfn * PAGE_SIZE;
                    /* KE-33: Safety net — never return a page in the
                     * kernel/bitmap/refcount region (0x100000-0x382000),
                     * even if the bitmap says it's free. The bitmap can
                     * get corrupted during fork's deep_copy loop (by
                     * vmm_map_page's PT-page memsets cascading), and
                     * returning a kernel page would let callers
                     * overwrite kernel data. Mark it used and skip. */
                    if (candidate >= 0x100000 && candidate < 0x382000) {
                        bitmap_set(pfn);
                        continue;
                    }
                    bitmap_set(pfn);
                    used_pages++;
                    if (phys_refcount) phys_refcount[pfn] = 1;
                    return candidate;
                }
            }
        }
    }
    return 0; /* Out of memory */
}

phys_addr_t pmm_alloc_pages(size_t count) {
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_page();

    if (count > highest_pfn) return 0;

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
        /* Clear refcount when freeing: ensures consistency if the page
         * is later re-allocated (pmm_alloc_page will set it to 1). */
        if (phys_refcount) phys_refcount[pfn] = 0;
    }
}

void pmm_free_pages(phys_addr_t addr, size_t count) {
    if (addr == 0 || count == 0) return;
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

/* Reserve a region of physical memory so PMM won't allocate from it.
 * Used by heap_init() to claim the heap region. */
void pmm_reserve_region(uintptr_t start, uintptr_t end) {
    mark_region(start, end);
    pr_info("PMM: reserved region 0x%x - 0x%x (%u MB)\n",
            (unsigned)start, (unsigned)end,
            (unsigned)((end - start) / MiB));
}

/* ---- Physical page reference-count functions (for COW fork) ---- */

void pmm_refcount_init(void) {
    /* The array was already placed and zeroed during pmm_init().
     * This function exists for explicit initialization if needed,
     * but is currently a no-op since pmm_init handles it. */
    pr_info("PMM: refcount array at 0x%x, size %u KB (%u entries)\n",
            (unsigned)(uintptr_t)phys_refcount,
            (unsigned)(refcount_size / 1024),
            (unsigned)highest_pfn);
}

void pmm_refcount_inc(phys_addr_t addr) {
    if (!phys_refcount || addr == 0) return;
    size_t pfn = addr_to_pfn(addr);
    if (pfn < highest_pfn) {
        phys_refcount[pfn]++;
    }
}

/* Decrement refcount for a physical page. Returns the new refcount.
 * If it returns 0, the caller should free the page via pmm_free_page()
 * (unless they are the sole owner just clearing COW, in which case
 * the page stays allocated with refcount implicitly reset to 1). */
uint32_t pmm_refcount_dec(phys_addr_t addr) {
    if (!phys_refcount || addr == 0) return 0;
    size_t pfn = addr_to_pfn(addr);
    if (pfn < highest_pfn && phys_refcount[pfn] > 0) {
        phys_refcount[pfn]--;
        return phys_refcount[pfn];
    }
    return 0;
}

uint32_t pmm_refcount_get(phys_addr_t addr) {
    if (!phys_refcount || addr == 0) return 0;
    size_t pfn = addr_to_pfn(addr);
    if (pfn < highest_pfn) return phys_refcount[pfn];
    return 0;
}
