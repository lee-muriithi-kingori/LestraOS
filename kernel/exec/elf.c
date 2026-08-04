/*
 * Lestra OS - ELF64 loader and userspace process execution
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * This is the foundation for running REAL programs in userspace (ring 3).
 * It parses an ELF64 binary, maps its segments into a fresh user address
 * space, sets up a user stack, and jumps to the entry point via IRETQ
 * with RPL=3 (user mode).
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <lestra/vfs.h>
#include <string.h>

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __packed elf64_hdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __packed elf64_phdr_t;

#define PT_LOAD  1
#define PF_X     1
#define PF_W     2
#define PF_R     4
/* IMPORTANT: write "\x7f" "ELF" with adjacent string literal concatenation.
 * A single "\x7fELF" is parsed by the C preprocessor as hex escape
 * \x7fE (consuming 'E' as a hex digit since A-F are valid hex), giving
 * 0xFE — the resulting string is "\xFE\x4C\x46" not "\x7F\x45\x4C\x46"
 * and the memcmp below would always fail. */
#define ELF_MAGIC "\x7f" "ELF"

#define USER_STACK_TOP   0x00007FFFFFE00000ULL
#define USER_STACK_SIZE  (256 * 1024)
#define USER_STACK_BOTTOM (USER_STACK_TOP - USER_STACK_SIZE)

/* Exposed for linux_compat.c so it can call elf_jump_to_user with the
 * same address space the regular loader uses. */
uintptr_t* user_pml4 = NULL;
static uint64_t user_entry = 0;
uint64_t user_stack_ptr = 0;

static uintptr_t* create_user_address_space(void) {
    phys_addr_t pml4_phys = pmm_alloc_page();
    if (!pml4_phys) return NULL;
    uintptr_t* pml4 = (uintptr_t*)pml4_phys;
    memset(pml4, 0, PAGE_SIZE);

    extern uint64_t boot_pml4[];
    pml4[0] = boot_pml4[0];
    pml4[1] = boot_pml4[1];
    pml4[2] = boot_pml4[2];
    pml4[3] = boot_pml4[3];

    return pml4;
}

static void user_map_page(uintptr_t* pml4, uint64_t vaddr, uint64_t phys, uint64_t flags) {
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t pd_idx   = (vaddr >> 21) & 0x1FF;
    uint64_t pt_idx   = (vaddr >> 12) & 0x1FF;

    /* NX (bit 63) is only valid on leaf PTEs.  Intermediate page table
     * entries (PML4/PDPT/PD) must NOT have it set — otherwise
     * PTE_PHYS_MASK extraction produces a non-canonical address (#GP). */
    uint64_t table_flags = flags & ~PAGE_NX;

    uintptr_t* pml4_virt = pml4;
    if (!(pml4_virt[pml4_idx] & PAGE_PRESENT)) {
        phys_addr_t pdpt_phys = pmm_alloc_page();
        if (!pdpt_phys) return;
        memset((void*)pdpt_phys, 0, PAGE_SIZE);
        pml4_virt[pml4_idx] = pdpt_phys | table_flags | PAGE_PRESENT;
    }
    uintptr_t* pdpt = (uintptr_t*)(pml4_virt[pml4_idx] & PTE_PHYS_MASK);

    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        phys_addr_t pd_phys = pmm_alloc_page();
        if (!pd_phys) return;
        memset((void*)pd_phys, 0, PAGE_SIZE);
        pdpt[pdpt_idx] = pd_phys | table_flags | PAGE_PRESENT;
    }
    uintptr_t* pd = (uintptr_t*)(pdpt[pdpt_idx] & PTE_PHYS_MASK);

    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        phys_addr_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return;
        memset((void*)pt_phys, 0, PAGE_SIZE);
        pd[pd_idx] = pt_phys | table_flags | PAGE_PRESENT;
    }
    uintptr_t* pt = (uintptr_t*)(pd[pd_idx] & PTE_PHYS_MASK);

    pt[pt_idx] = phys | flags | PAGE_PRESENT;
}

static void user_map_data(uintptr_t* pml4, uint64_t vaddr, const void* data,
                          size_t data_len, int writable, int executable) {
    uint64_t flags = PAGE_PRESENT | PAGE_USER;
    if (writable) flags |= PAGE_WRITABLE;
    if (!executable) flags |= PAGE_NX;

    size_t num_pages = (data_len + PAGE_SIZE - 1) / PAGE_SIZE;
    for (size_t i = 0; i < num_pages; i++) {
        phys_addr_t phys = pmm_alloc_page();
        if (!phys) return;
        memset((void*)(uintptr_t)phys, 0, PAGE_SIZE);
        size_t offset = i * PAGE_SIZE;
        size_t to_copy = data_len - offset;
        if (to_copy > PAGE_SIZE) to_copy = PAGE_SIZE;
        memcpy((void*)(uintptr_t)phys, (const char*)data + offset, to_copy);
        user_map_page(pml4, vaddr + offset, phys, flags);
    }
}

uint64_t elf_load(const void* elf_data, size_t elf_size) {
    if (!elf_data || elf_size < sizeof(elf64_hdr_t)) return 0;

    const elf64_hdr_t* hdr = (const elf64_hdr_t*)elf_data;

    if (memcmp(hdr->e_ident, ELF_MAGIC, 4) != 0) return 0;
    if (hdr->e_ident[4] != 2) return 0;
    if (hdr->e_machine != 0x3E) return 0;
    if (hdr->e_type != 2 && hdr->e_type != 3) return 0;

    pr_info("elf: entry=0x%x, phnum=%u\n",
            (unsigned)hdr->e_entry, (unsigned)hdr->e_phnum);

    user_pml4 = create_user_address_space();
    if (!user_pml4) return 0;

    const elf64_phdr_t* phdrs = (const elf64_phdr_t*)
        ((const char*)elf_data + hdr->e_phoff);

    for (int i = 0; i < hdr->e_phnum; i++) {
        const elf64_phdr_t* ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;

        pr_info("elf: PT_LOAD vaddr=0x%x filesz=%u memsz=%u\n",
                (unsigned)ph->p_vaddr, (unsigned)ph->p_filesz,
                (unsigned)ph->p_memsz);

        int writable   = (ph->p_flags & PF_W) != 0;
        int executable = (ph->p_flags & PF_X) != 0;
        const void* segment_data = (const char*)elf_data + ph->p_offset;

        user_map_data(user_pml4, ph->p_vaddr, segment_data, ph->p_filesz, writable, executable);

        if (ph->p_memsz > ph->p_filesz) {
            uint64_t bss_start = ph->p_vaddr + ph->p_filesz;
            bss_start = (bss_start + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
            uint64_t bss_end = ph->p_vaddr + ph->p_memsz;
            size_t bss_pages = (bss_end - bss_start + PAGE_SIZE - 1) / PAGE_SIZE;
            uint64_t bss_flags = PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE;
            if (!executable) bss_flags |= PAGE_NX;
            for (size_t p = 0; p < bss_pages; p++) {
                phys_addr_t phys = pmm_alloc_page();
                if (phys) {
                    memset((void*)(uintptr_t)phys, 0, PAGE_SIZE);
                    user_map_page(user_pml4, bss_start + p * PAGE_SIZE, phys, bss_flags);
                }
            }
        }
    }

    user_stack_ptr = USER_STACK_TOP;
    size_t stack_pages = USER_STACK_SIZE / PAGE_SIZE;
    for (size_t i = 0; i < stack_pages; i++) {
        phys_addr_t phys = pmm_alloc_page();
        if (phys) {
            memset((void*)(uintptr_t)phys, 0, PAGE_SIZE);
            user_map_page(user_pml4, USER_STACK_BOTTOM + i * PAGE_SIZE,
                          phys, PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE | PAGE_NX);
        }
    }
    user_stack_ptr = USER_STACK_TOP - 16;

    user_entry = hdr->e_entry;
    return user_entry;
}

extern void elf_jump_to_user(uint64_t entry, uint64_t stack, uintptr_t pml4);

int elf_exec(const char* path) {
    int fd = vfs_open(path, 0);
    if (fd < 0) {
        pr_warn("elf: '%s' not found\n", path);
        return -1;
    }

    static uint8_t elf_buf[65536];
    size_t total = 0;
    while (total < sizeof(elf_buf)) {
        ssize_t n = vfs_read(fd, &elf_buf[total], sizeof(elf_buf) - total);
        if (n <= 0) break;
        total += n;
    }
    vfs_close(fd);

    pr_info("elf: loaded %u bytes from %s\n", (unsigned)total, path);

    uint64_t entry = elf_load(elf_buf, total);
    if (!entry) return -1;

    pr_info("elf: jumping to userspace\n");
    elf_jump_to_user(entry, user_stack_ptr, (uintptr_t)user_pml4);

    pr_warn("elf: userspace returned (should not happen)\n");
    return -1;
}
