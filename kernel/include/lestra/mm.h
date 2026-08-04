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
#define PAGE_COW        0x200  /* Bit 9: software-use COW marker (x86_64 bits 9-11 are available to OS) */
#define PAGE_NX         (1ULL << 63)

/* Physical address mask for PTEs (bits 51:12).
 * MUST exclude bit 63 (PAGE_NX) and bits 52-62 (reserved/available).
 * Using ~0xFFFULL is WRONG because it preserves bit 63 (NX), producing
 * non-canonical addresses when the PTE has NX set. */
#define PTE_PHYS_MASK   0x000FFFFFFFFFF000ULL

#define PAGE_KERNEL     (PAGE_PRESENT | PAGE_WRITABLE | PAGE_GLOBAL)
#define PAGE_USER_RW    (PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER)
#define PAGE_USER_RO    (PAGE_PRESENT | PAGE_USER)

/* Virtual memory regions
 *
 * boot.asm identity-maps the first 1 GB of physical memory using 512
 * 2 MB huge pages. Anything above 0x40000000 is NOT mapped by the boot
 * stub and will page-fault on first access until vmm_map_page() is
 * called for it.
 *
 * FIX: previous code placed the kernel heap at 0x40000000..0x50000000
 * (1 GB..1.25 GB) which is the first UNMAPPED byte. The very first
 * kmalloc() write would have page-faulted. Move the heap into the
 * identity-mapped region:
 *
 *   KERNEL_HEAP_START = 0x10000000   (256 MB)
 *   KERNEL_HEAP_END   = 0x30000000   (768 MB)
 *                       -> 512 MB heap, well clear of the kernel image
 *                          at 0x100000+ and well inside the 1 GB map.
 */
#define KERNEL_VMA          0xFFFFFFFF80000000ULL  /* legacy, unused now */
#define KERNEL_HEAP_START   0x10000000ULL          /* 256 MB */
#define KERNEL_HEAP_END     0x30000000ULL          /* 768 MB (512 MB heap) */
#define KERNEL_STACK_START  0xFFFFFFFF80000000ULL  /* legacy, unused */
#define KERNEL_STACK_END    0xFFFFFFFF90000000ULL  /* legacy, unused */
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
void pmm_reserve_region(uintptr_t start, uintptr_t end);  /* mark as used */

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
uintptr_t heap_get_used(void);

/* Physical page reference counting (for COW fork) */
void pmm_refcount_init(void);
void pmm_refcount_inc(phys_addr_t addr);
uint32_t pmm_refcount_dec(phys_addr_t addr);   /* returns new refcount; 0 means last ref dropped */
uint32_t pmm_refcount_get(phys_addr_t addr);

/* Page fault handler — called from ISR 14 dispatcher in idt.c.
 * Receives faulting address (CR2), error code, and the saved interrupt
 * frame for diagnostic info (RIP etc). Returns on successful resolution
 * (COW, stack growth); panics on unhandled faults. */
struct interrupt_frame;
void page_fault_handler(uintptr_t fault_addr, uint64_t error_code,
                        struct interrupt_frame* frame);

/* Memory info */
void mm_print_stats(void);

#endif /* LESTRA_MM_H */
