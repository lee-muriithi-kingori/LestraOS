/*
 * Lestra OS - Memory Management
 * Copyright (c) 2026 lestramk.org
 */

#ifndef LESTRA_MM_H
#define LESTRA_MM_H

#include <lestra/types.h>

/* Page flags */
#define PAGE_PRESENT    0x001
#define PAGE_WRITABLE   0x002
#define PAGE_USER       0x004
#define PAGE_WRITETHRU  0x008
#define PAGE_NOCACHE    0x010
#define PAGE_ACCESSED   0x020
#define PAGE_DIRTY      0x040
#define PAGE_HUGE       0x080
#define PAGE_GLOBAL     0x100
#define PAGE_NX         (1ULL << 63)

#define PAGE_KERNEL     (PAGE_PRESENT | PAGE_WRITABLE | PAGE_GLOBAL)
#define PAGE_USER_RW    (PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER)
#define PAGE_USER_RO    (PAGE_PRESENT | PAGE_USER)

/* Virtual memory regions */
#define KERNEL_VMA          0xFFFFFFFF80000000ULL
#define KERNEL_HEAP_START   0xFFFFFFFF90000000ULL
#define KERNEL_HEAP_END     0xFFFFFFFFA0000000ULL
#define USER_SPACE_START    0x0000000000400000ULL
#define USER_SPACE_END      0x00007FFFFFFFFFFFULL

/* Physical memory map entry types */
#define MMAP_USABLE       1
#define MMAP_RESERVED     2
#define MMAP_ACPI_RECLAIM 3
#define MMAP_ACPI_NVS     4
#define MMAP_BAD          5

struct mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} __packed;

/* Physical page allocator */
void pmm_init(struct mmap_entry* mmap, uint32_t mmap_entries);
phys_addr_t pmm_alloc_page(void);
phys_addr_t pmm_alloc_pages(size_t count);
void pmm_free_page(phys_addr_t addr);
void pmm_free_pages(phys_addr_t addr, size_t count);
uintptr_t pmm_get_total(void);
uintptr_t pmm_get_used(void);
uintptr_t pmm_get_free(void);

/* Virtual memory / paging */
void vmm_init(void);
void vmm_map_page(uintptr_t* pml4, virt_addr_t vaddr, phys_addr_t paddr, uint64_t flags);
void vmm_unmap_page(uintptr_t* pml4, virt_addr_t vaddr);
phys_addr_t vmm_get_phys(uintptr_t* pml4, virt_addr_t vaddr);
uintptr_t* vmm_create_address_space(void);
void vmm_destroy_address_space(uintptr_t* pml4);
void vmm_switch_address_space(uintptr_t* pml4);

/* Kernel heap */
void heap_init(void);
void* kmalloc(size_t size);
void* kcalloc(size_t count, size_t size);
void* krealloc(void* ptr, size_t size);
void kfree(void* ptr);
size_t ksize(void* ptr);

/* Memory info */
void mm_print_stats(void);

#endif /* LESTRA_MM_H */
