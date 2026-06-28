/*
 * Lestra OS - Virtual Memory Manager
 * Copyright (c) 2026 lestramk.org
 *
 * x86_64 4-level paging with on-demand allocation.
 */

#include <lestra/types.h>
#include <lestra/mm.h>
#include <lestra/printk.h>
#include <lestra/panic.h>

/* Page table entry manipulation */
static inline uint64_t pte_addr(uint64_t pte) {
    return pte & 0x000FFFFFFFFFF000ULL;
}

static inline bool pte_present(uint64_t pte) {
    return pte & PAGE_PRESENT;
}

static inline void tlb_flush(void* vaddr) {
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

/* Get or create page table at given level */
static uintptr_t* get_or_create_table(uintptr_t* parent, size_t index, uint64_t flags) {
    uint64_t entry = parent[index];
    if (pte_present(entry)) {
        return (uintptr_t*)(pte_addr(entry) + KERNEL_VMA);
    }
    
    /* Allocate new page table */
    phys_addr_t phys = pmm_alloc_page();
    if (!phys) {
        panic("VMM: Out of physical memory for page table");
    }
    
    /* Zero the page */
    uintptr_t* virt = (uintptr_t*)(phys + KERNEL_VMA);
    for (int i = 0; i < 512; i++) {
        virt[i] = 0;
    }
    
    parent[index] = phys | flags;
    return virt;
}

void vmm_init(void) {
    /* The bootloader set up initial page tables.
     * We just need to ensure the kernel page tables are properly set up.
     */
    pr_debug("VMM initialized (4-level paging)\n");
}

void vmm_map_page(uintptr_t* pml4, virt_addr_t vaddr, phys_addr_t paddr, uint64_t flags) {
    size_t pml4_idx = (vaddr >> 39) & 0x1FF;
    size_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    size_t pd_idx   = (vaddr >> 21) & 0x1FF;
    size_t pt_idx   = (vaddr >> 12) & 0x1FF;
    
    uintptr_t* pdpt = get_or_create_table(pml4, pml4_idx, PAGE_KERNEL);
    uintptr_t* pd   = get_or_create_table(pdpt, pdpt_idx, PAGE_KERNEL);
    uintptr_t* pt   = get_or_create_table(pd, pd_idx, PAGE_KERNEL);
    
    pt[pt_idx] = (paddr & ~0xFFF) | flags;
    tlb_flush((void*)vaddr);
}

void vmm_unmap_page(uintptr_t* pml4, virt_addr_t vaddr) {
    size_t pml4_idx = (vaddr >> 39) & 0x1FF;
    size_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    size_t pd_idx   = (vaddr >> 21) & 0x1FF;
    size_t pt_idx   = (vaddr >> 12) & 0x1FF;
    
    uint64_t pml4e = pml4[pml4_idx];
    if (!pte_present(pml4e)) return;
    
    uintptr_t* pdpt = (uintptr_t*)(pte_addr(pml4e) + KERNEL_VMA);
    uint64_t pdpte = pdpt[pdpt_idx];
    if (!pte_present(pdpte)) return;
    
    uintptr_t* pd = (uintptr_t*)(pte_addr(pdpte) + KERNEL_VMA);
    uint64_t pde = pd[pd_idx];
    if (!pte_present(pde)) return;
    
    uintptr_t* pt = (uintptr_t*)(pte_addr(pde) + KERNEL_VMA);
    pt[pt_idx] = 0;
    tlb_flush((void*)vaddr);
}

phys_addr_t vmm_get_phys(uintptr_t* pml4, virt_addr_t vaddr) {
    size_t pml4_idx = (vaddr >> 39) & 0x1FF;
    size_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    size_t pd_idx   = (vaddr >> 21) & 0x1FF;
    size_t pt_idx   = (vaddr >> 12) & 0x1FF;
    
    uint64_t pml4e = pml4[pml4_idx];
    if (!pte_present(pml4e)) return 0;
    
    uintptr_t* pdpt = (uintptr_t*)(pte_addr(pml4e) + KERNEL_VMA);
    uint64_t pdpte = pdpt[pdpt_idx];
    if (!pte_present(pdpte)) return 0;
    
    uintptr_t* pd = (uintptr_t*)(pte_addr(pdpte) + KERNEL_VMA);
    uint64_t pde = pd[pd_idx];
    if (!pte_present(pde)) return 0;
    
    uintptr_t* pt = (uintptr_t*)(pte_addr(pde) + KERNEL_VMA);
    return pte_addr(pt[pt_idx]) | (vaddr & 0xFFF);
}

uintptr_t* vmm_create_address_space(void) {
    phys_addr_t phys = pmm_alloc_page();
    if (!phys) return NULL;
    
    uintptr_t* pml4 = (uintptr_t*)(phys + KERNEL_VMA);
    
    /* Clear all entries */
    for (int i = 0; i < 512; i++) {
        pml4[i] = 0;
    }
    
    /* Copy kernel higher-half mapping */
    uintptr_t* current_pml4 = (uintptr_t*)(read_cr3() + KERNEL_VMA);
    for (int i = 256; i < 512; i++) {
        pml4[i] = current_pml4[i];
    }
    
    return pml4;
}

void vmm_destroy_address_space(uintptr_t* pml4) {
    /* Free user-space page tables (indices 0-255) */
    for (int i = 0; i < 256; i++) {
        if (pte_present(pml4[i])) {
            uintptr_t* pdpt = (uintptr_t*)(pte_addr(pml4[i]) + KERNEL_VMA);
            for (int j = 0; j < 512; j++) {
                if (pte_present(pdpt[j])) {
                    uintptr_t* pd = (uintptr_t*)(pte_addr(pdpt[j]) + KERNEL_VMA);
                    for (int k = 0; k < 512; k++) {
                        if (pte_present(pd[k]) && !(pd[k] & PAGE_HUGE)) {
                            pmm_free_page(pte_addr(pd[k]));
                        }
                    }
                    pmm_free_page(pte_addr(pdpt[j]));
                }
            }
            pmm_free_page(pte_addr(pml4[i]));
        }
    }
    
    pmm_free_page((phys_addr_t)pml4 - KERNEL_VMA);
}

void vmm_switch_address_space(uintptr_t* pml4) {
    write_cr3((phys_addr_t)pml4 - KERNEL_VMA);
}
