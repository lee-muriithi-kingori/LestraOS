/*
 * Lestra OS - Virtual Memory Manager
 * Copyright (c) 2026 lestramk.org
 *
 * x86 paging with on-demand page fault handling.
 * Identity maps first 4GB, with kernel at higher half.
 */

#include <lestra/types.h>
#include <lestra/mm.h>
#include <lestra/printk.h>
#include <lestra/panic.h>
#include <string.h>

#define PT_ENTRIES 512
#define PD_ENTRIES 512
#define PDPT_ENTRIES 512
#define PML4_ENTRIES 512

/* Page table entry type */
typedef uint64_t pml4e_t;
typedef uint64_t pdpte_t;
typedef uint64_t pde_t;
typedef uint64_t pte_t;

/* Page-table level at which a PTE was found — used by vmm_get_phys
 * to distinguish 1GB (PDPT level) from 2MB (PD level) huge pages. */
typedef enum { LEVEL_PT = 0, LEVEL_PD, LEVEL_PDPT } pt_level_t;

/* Kernel PML4 - defined in boot assembly */
extern uint64_t boot_pml4[512];

/* Get page table entry for virtual address
 * FIX: was missing huge page detection. boot.asm maps the first 1GB
 * using 2MB huge pages, so any virtual address in [0, 1GB) is backed
 * by a huge PD entry, not a PT entry. The old code dereferenced the
 * huge PD entry as if it were a PT pointer, reading garbage memory.
 *
 * Now returns a sentinel non-NULL pointer for huge-mapped addresses
 * (callers should check `*pte & PAGE_HUGE` if they care). */
static pte_t* get_pte_level(uintptr_t* pml4, uintptr_t virt, pt_level_t* out_level) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & PAGE_PRESENT)) return NULL;

    uint64_t* pdpt = (uint64_t*)(pml4[pml4_idx] & PTE_PHYS_MASK);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) return NULL;

    /* PDPT entry itself could be a 1GB huge page */
    if (pdpt[pdpt_idx] & PAGE_HUGE) {
        if (out_level) *out_level = LEVEL_PDPT;
        return &pdpt[pdpt_idx];  /* 1GB huge page */
    }

    uint64_t* pd = (uint64_t*)(pdpt[pdpt_idx] & PTE_PHYS_MASK);
    if (!(pd[pd_idx] & PAGE_PRESENT)) return NULL;

    /* PD entry can be a 2MB huge page (this is what boot.asm uses) */
    if (pd[pd_idx] & PAGE_HUGE) {
        if (out_level) *out_level = LEVEL_PD;
        return &pd[pd_idx];  /* 2MB huge page */
    }

    if (out_level) *out_level = LEVEL_PT;
    uint64_t* pt = (uint64_t*)(pd[pd_idx] & PTE_PHYS_MASK);
    return &pt[pt_idx];
}

static pte_t* get_pte(uintptr_t* pml4, uintptr_t virt) {
    return get_pte_level(pml4, virt, NULL);
}

void vmm_init(void) {
    pr_info("VMM initialized\n");
}

/* Map a single 4KB page
 * FIX: detect huge pages in the existing mapping and refuse to overwrite
 * them. Previously this would dereference a huge PD entry as a PT pointer
 * and corrupt random memory. */
void vmm_map_page(uintptr_t* pml4, virt_addr_t virt, phys_addr_t paddr, uint64_t flags) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    /* Allocate page tables if needed */
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        phys_addr_t pdpt_phys = pmm_alloc_page();
        if (!pdpt_phys) return;
        memset((void*)pdpt_phys, 0, PAGE_SIZE);
        pml4[pml4_idx] = pdpt_phys | PAGE_PRESENT | PAGE_WRITABLE;
    }

    uint64_t* pdpt = (uint64_t*)(pml4[pml4_idx] & PTE_PHYS_MASK);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        phys_addr_t pd_phys = pmm_alloc_page();
        if (!pd_phys) return;
        memset((void*)pd_phys, 0, PAGE_SIZE);
        pdpt[pdpt_idx] = pd_phys | PAGE_PRESENT | PAGE_WRITABLE;
    }

    /* FIX: bail if PDPT entry is a 1GB huge page */
    if (pdpt[pdpt_idx] & PAGE_HUGE) {
        pr_warn("VMM: refusing to map 0x%x (1GB huge page in the way)\n", (unsigned)virt);
        return;
    }

    uint64_t* pd = (uint64_t*)(pdpt[pdpt_idx] & PTE_PHYS_MASK);
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        phys_addr_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return;
        memset((void*)pt_phys, 0, PAGE_SIZE);
        pd[pd_idx] = pt_phys | PAGE_PRESENT | PAGE_WRITABLE;
    }

    /* FIX: bail if PD entry is a 2MB huge page (this is the case for
     * anything in the first 1GB after boot.asm sets up identity mapping). */
    if (pd[pd_idx] & PAGE_HUGE) {
        pr_warn("VMM: refusing to map 0x%x (2MB huge page in the way)\n", (unsigned)virt);
        return;
    }

    uint64_t* pt = (uint64_t*)(pd[pd_idx] & PTE_PHYS_MASK);

    /* Check if page is already mapped — use refcount-aware freeing.
     * With COW, the old physical page may be shared by other processes.
     * We only free it if this was the last PTE reference (refcount
     * drops to 0 after decrement). Otherwise, other processes still
     * reference it and we must not free it. */
    if (pt[pt_idx] & PAGE_PRESENT) {
        phys_addr_t old_phys = pt[pt_idx] & PTE_PHYS_MASK;
        /* Decrement refcount of the old physical page. Only free if
         * no other PTEs reference it (refcount drops to 0). */
        if (pmm_refcount_dec(old_phys) == 0) {
            pmm_free_page(old_phys);
        }
    }

    pt[pt_idx] = paddr | flags | PAGE_PRESENT;
    invlpg((void*)virt);
}

/* Unmap a page */
void vmm_unmap_page(uintptr_t* pml4, virt_addr_t virt) {
    pte_t* pte = get_pte(pml4, virt);
    if (pte && (*pte & PAGE_PRESENT)) {
        /* Don't unmap huge pages from under us — that would break the
         * identity mapping set up by boot.asm. Just no-op. */
        if (*pte & PAGE_HUGE) return;
        *pte = 0;
        invlpg((void*)virt);
    }
}

/* Translate virtual to physical
 * FIX: handle huge pages — for a 2MB huge page, the physical address
 * is formed from bits 51:21 of the entry plus bits 20:0 of the virt.
 * For a 1GB huge page, it's bits 51:30 of entry + bits 29:0 of virt. */
phys_addr_t vmm_get_phys(uintptr_t* pml4, virt_addr_t virt) {
    pt_level_t level;
    pte_t* pte = get_pte_level(pml4, virt, &level);
    if (pte && (*pte & PAGE_PRESENT)) {
        if (*pte & PAGE_HUGE) {
            uint64_t entry = *pte & 0x000FFFFFFFFFF000ULL;
            if (level == LEVEL_PDPT) {
                /* 1GB huge page */
                return (entry & ~0x3FFFFFFFULL) | (virt & 0x3FFFFFFF);
            } else {
                /* 2MB huge page (LEVEL_PD) */
                return (entry & ~0x1FFFFFULL) | (virt & 0x1FFFFF);
            }
        }
        return (*pte & PTE_PHYS_MASK) | (virt & 0xFFF);
    }
    return 0;
}

/* Create new address space (allocate PML4) */
uintptr_t* vmm_create_address_space(void) {
    phys_addr_t pml4_phys = pmm_alloc_page();
    if (!pml4_phys) return NULL;
    uint64_t* pml4 = (uint64_t*)pml4_phys;
    memset(pml4, 0, PAGE_SIZE);
    return (uintptr_t*)pml4;
}

/* Free every page table (and mapped leaf page) reachable from a single
 * PDPT entry. Only ever called on PML4 indices >= 4, which are private
 * to this address space (indices 0-3 are shared with boot_pml4 across
 * every process and the kernel — see create_proc_pml4() in scheduler.c
 * — and must never be freed here).
 *
 * COW-aware: leaf pages (4KB user pages) are freed only when their
 * refcount drops to 0 (meaning no other process's PTE still references
 * them). Page table structures (PT, PD, PDPT pages) are always private
 * to this address space and are freed directly. */
static void free_pdpt_subtree(phys_addr_t pdpt_phys) {
    uint64_t* pdpt = (uint64_t*)pdpt_phys;
    for (int p3 = 0; p3 < PDPT_ENTRIES; p3++) {
        if (!(pdpt[p3] & PAGE_PRESENT)) continue;
        if (pdpt[p3] & PAGE_HUGE) continue; /* 1GB huge page: nothing to recurse into */

        phys_addr_t pd_phys = pdpt[p3] & PTE_PHYS_MASK;
        uint64_t* pd = (uint64_t*)pd_phys;
        for (int p2 = 0; p2 < PD_ENTRIES; p2++) {
            if (!(pd[p2] & PAGE_PRESENT)) continue;
            if (pd[p2] & PAGE_HUGE) continue; /* 2MB huge page: nothing to recurse into */

            phys_addr_t pt_phys = pd[p2] & PTE_PHYS_MASK;
            uint64_t* pt = (uint64_t*)pt_phys;
            for (int p1 = 0; p1 < PT_ENTRIES; p1++) {
                if (!(pt[p1] & PAGE_PRESENT)) continue;
                phys_addr_t page_phys = pt[p1] & PTE_PHYS_MASK;
                /* COW-aware: decrement refcount. Only free the physical
                 * page if no other PTE still references it. Private
                 * pages (refcount=1) will be freed; COW-shared pages
                 * (refcount>1) will remain for other processes. */
                if (pmm_refcount_dec(page_phys) == 0) {
                    pmm_free_page(page_phys);
                }
            }
            /* Page table page: private to this address space, free directly */
            pmm_free_page(pt_phys);
        }
        pmm_free_page(pd_phys);
    }
    pmm_free_page(pdpt_phys);
}

void vmm_destroy_address_space(uintptr_t* pml4) {
    if (!pml4) return;

    /* PML4 indices 0-3 are shared kernel mappings copied by pointer in
     * create_proc_pml4() — freeing them would corrupt the kernel and
     * every other process's address space. Only free indices 4-511,
     * which are private to this process (e.g. the user stack at
     * PML4[255], and any ELF segments mapped outside the low range). */
    for (int p4 = 4; p4 < PML4_ENTRIES; p4++) {
        if (!(pml4[p4] & PAGE_PRESENT)) continue;
        free_pdpt_subtree(pml4[p4] & PTE_PHYS_MASK);
    }

    pmm_free_page((phys_addr_t)(uintptr_t)pml4);
}

void vmm_switch_address_space(uintptr_t* pml4) {
    if (pml4) {
        write_cr3((uintptr_t)pml4);
    }
}

/* Allocate and map a virtual page
 * FIX: was using KERNEL_HEAP_START which conflicts with the bump heap.
 * Also, the first 1GB is identity-mapped via 2MB huge pages, so any
 * address there will be refused by vmm_map_page. Use a region at 2GB
 * (PML4[0] PDPT[2]) — boot.asm only sets PDPT[0], so PDPT[2] is free
 * for vmm_map_page to set up 4KB page table entries. */
#define VMM_ALLOC_REGION_START  0x80000000ULL   /* 2GB */
#define VMM_ALLOC_REGION_END    0xC0000000ULL   /* 3GB - 1GB of space */

void* vmm_alloc_page(uint64_t flags) {
    phys_addr_t phys = pmm_alloc_page();
    if (!phys) return NULL;

    static uintptr_t next_virt = VMM_ALLOC_REGION_START;
    if (next_virt >= VMM_ALLOC_REGION_END) return NULL;
    uintptr_t virt = next_virt;
    next_virt += PAGE_SIZE;

    vmm_map_page(boot_pml4, virt, phys, flags);
    return (void*)virt;
}

void vmm_free_page_ptr(void* ptr) {
    if (!ptr) return;
    uintptr_t virt = (uintptr_t)ptr;
    phys_addr_t phys = vmm_get_phys(boot_pml4, virt);
    if (phys) {
        pmm_free_page(phys);
        vmm_unmap_page(boot_pml4, virt);
    }
}

/* Legacy page-fault handler — kept as a fallback for kernel-context
 * faults that don't go through the per-process ISR 14 handler.
 * The real handler is page_fault_handler() in kernel/mm/page_fault.c,
 * called from idt.c. This stub remains for any direct calls from
 * kernel code that still reference vmm_page_fault_handler. */
void vmm_page_fault_handler(uintptr_t fault_addr, uint64_t error_code) {
    /* Delegate to the new handler with a NULL frame (kernel context) */
    page_fault_handler(fault_addr, error_code, NULL);
}

/* Map a contiguous region */
void vmm_map_region(uintptr_t virt, phys_addr_t phys, size_t pages, uint64_t flags) {
    for (size_t i = 0; i < pages; i++) {
        vmm_map_page(boot_pml4, virt + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags);
    }
}
