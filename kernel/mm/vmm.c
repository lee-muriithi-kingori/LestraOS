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

#define PT_ENTRIES 512
#define PD_ENTRIES 512
#define PDPT_ENTRIES 512
#define PML4_ENTRIES 512

#define PAGE_PRESENT    0x001
#define PAGE_WRITABLE   0x002
#define PAGE_USER       0x004
#define PAGE_WRITETHROUGH 0x008
#define PAGE_NOCACHE    0x010
#define PAGE_ACCESSED   0x020
#define PAGE_DIRTY      0x040
#define PAGE_HUGE       0x080
#define PAGE_GLOBAL     0x100

#define PAGE_SIZE_4KB   0x1000
#define PAGE_SIZE_2MB   0x200000

/* Page table entry type */
typedef uint64_t pml4e_t;
typedef uint64_t pdpte_t;
typedef uint64_t pde_t;
typedef uint64_t pte_t;

/* Kernel PML4 - defined in assembly */
extern uint64_t kernel_pml4[512];

static inline void invlpg(uintptr_t addr) {
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

void vmm_init(void) {
    /* Paging already enabled by bootloader with identity mapping */
    /* Set up kernel heap mapping at KERNEL_HEAP_START */

    pr_info("VMM initialized\n");
}

/* Get page table entry for virtual address */
static pte_t* get_pte(uintptr_t virt) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    if (!(kernel_pml4[pml4_idx] & PAGE_PRESENT)) return NULL;

    uint64_t* pdpt = (uint64_t*)(kernel_pml4[pml4_idx] & ~0xFFF);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) return NULL;

    uint64_t* pd = (uint64_t*)(pdpt[pdpt_idx] & ~0xFFF);
    if (!(pd[pd_idx] & PAGE_PRESENT)) return NULL;

    uint64_t* pt = (uint64_t*)(pd[pd_idx] & ~0xFFF);
    return &pt[pt_idx];
}

/* Map a single 4KB page */
void vmm_map_page(uintptr_t virt, phys_addr_t phys, uint64_t flags) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    /* Allocate page tables if needed */
    if (!(kernel_pml4[pml4_idx] & PAGE_PRESENT)) {
        phys_addr_t pdpt_phys = pmm_alloc_page();
        if (!pdpt_phys) return;
        memset((void*)(pdpt_phys + KERNEL_VMA), 0, PAGE_SIZE);
        kernel_pml4[pml4_idx] = pdpt_phys | PAGE_PRESENT | PAGE_WRITABLE;
    }

    uint64_t* pdpt = (uint64_t*)(kernel_pml4[pml4_idx] & ~0xFFF);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        phys_addr_t pd_phys = pmm_alloc_page();
        if (!pd_phys) return;
        memset((void*)(pd_phys + KERNEL_VMA), 0, PAGE_SIZE);
        pdpt[pdpt_idx] = pd_phys | PAGE_PRESENT | PAGE_WRITABLE;
    }

    uint64_t* pd = (uint64_t*)(pdpt[pdpt_idx] & ~0xFFF);
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        phys_addr_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return;
        memset((void*)(pt_phys + KERNEL_VMA), 0, PAGE_SIZE);
        pd[pd_idx] = pt_phys | PAGE_PRESENT | PAGE_WRITABLE;
    }

    uint64_t* pt = (uint64_t*)(pd[pd_idx] & ~0xFFF);

    /* FIX #10: Check if page is already mapped - report overwrite */
    if (pt[pt_idx] & PAGE_PRESENT) {
        pr_warn("VMM: Remapping already-mapped page at virt=0x%lx (old: 0x%lx)\n",
                virt, pt[pt_idx] & ~0xFFF);
        /* Unmap old page to avoid memory leak */
        phys_addr_t old_phys = pt[pt_idx] & ~0xFFF;
        pmm_free_page(old_phys);
    }

    pt[pt_idx] = phys | flags | PAGE_PRESENT;
    invlpg(virt);
}

/* Unmap a page */
void vmm_unmap_page(uintptr_t virt) {
    pte_t* pte = get_pte(virt);
    if (pte && (*pte & PAGE_PRESENT)) {
        *pte = 0;
        invlpg(virt);
    }
}

/* Translate virtual to physical */
phys_addr_t vmm_virt_to_phys(uintptr_t virt) {
    pte_t* pte = get_pte(virt);
    if (pte && (*pte & PAGE_PRESENT)) {
        return (*pte & ~0xFFF) | (virt & 0xFFF);
    }
    return 0;
}

/* Allocate and map a virtual page */
void* vmm_alloc_page(uint64_t flags) {
    phys_addr_t phys = pmm_alloc_page();
    if (!phys) return NULL;

    /* Find free virtual address */
    static uintptr_t next_virt = KERNEL_HEAP_START;
    uintptr_t virt = next_virt;
    next_virt += PAGE_SIZE;

    vmm_map_page(virt, phys, flags);
    return (void*)virt;
}

void vmm_free_page_ptr(void* ptr) {
    if (!ptr) return;
    uintptr_t virt = (uintptr_t)ptr;
    phys_addr_t phys = vmm_virt_to_phys(virt);
    if (phys) {
        pmm_free_page(phys);
        vmm_unmap_page(virt);
    }
}

/* Page fault handler */
void vmm_page_fault_handler(uintptr_t fault_addr, uint64_t error_code) {
    pr_err("Page fault at 0x%lx (error: 0x%lx)\n", fault_addr, error_code);

    /* Check if it's a protection violation */
    if (error_code & 0x1) {
        pr_err("  -> Page protection violation\n");
    } else {
        pr_err("  -> Page not present\n");
    }

    /* Check if user or supervisor mode */
    if (error_code & 0x4) {
        pr_err("  -> User-mode access\n");
    } else {
        pr_err("  -> Supervisor-mode access\n");
    }

    /* Check if write or read */
    if (error_code & 0x2) {
        pr_err("  -> Write access\n");
    } else {
        pr_err("  -> Read access\n");
    }

    /* Try to handle stack growth */
    if (fault_addr >= KERNEL_STACK_START && fault_addr < KERNEL_STACK_END) {
        phys_addr_t phys = pmm_alloc_page();
        if (phys) {
            vmm_map_page(ALIGN_DOWN(fault_addr, PAGE_SIZE), phys,
                         PAGE_WRITABLE | PAGE_GLOBAL);
            pr_info("  -> Handled stack growth\n");
            return;
        }
    }

    panic("Unhandled page fault at 0x%lx", fault_addr);
}

/* Map a contiguous region */
void vmm_map_region(uintptr_t virt, phys_addr_t phys, size_t pages, uint64_t flags) {
    for (size_t i = 0; i < pages; i++) {
        vmm_map_page(virt + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags);
    }
}
