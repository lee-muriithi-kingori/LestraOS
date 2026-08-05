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

/* User stack constants — single source of truth in mm.h */

/* Exposed for linux_compat.c so it can call elf_jump_to_user with the
 * same address space the regular loader uses. */
uintptr_t* user_pml4 = NULL;
static uint64_t user_entry = 0;
uint64_t user_stack_ptr = 0;

/* KE-13: Deep-copy a boot PD into a private PD for the user address space.
 * This allows user_map_page() to split 2MB huge pages without corrupting
 * the kernel's own page tables. The new PD entries inherit the boot flags
 * (Present+Writable for identity-mapped kernel memory) but with PAGE_USER
 * cleared so the user process cannot access kernel memory through them. */
static uint64_t* deep_copy_pd(uint64_t* boot_pd) {
    phys_addr_t pd_phys = pmm_alloc_page();
    if (!pd_phys) return NULL;
    uint64_t* pd = (uint64_t*)(uintptr_t)pd_phys;
    for (int i = 0; i < 512; i++) {
        pd[i] = boot_pd[i] & ~PAGE_USER;  /* kernel-only entries */
    }
    return pd;
}

/* KE-13: Deep-copy a boot PDPT into a private PDPT, and deep-copy every
 * PD it references. This gives the user address space its own private
 * page table structures that can be modified (e.g. to split huge pages)
 * without affecting the kernel's own mappings. */
static uint64_t* deep_copy_pdpt(uint64_t* boot_pdpt) {
    phys_addr_t pdpt_phys = pmm_alloc_page();
    if (!pdpt_phys) return NULL;
    uint64_t* pdpt = (uint64_t*)(uintptr_t)pdpt_phys;
    for (int i = 0; i < 512; i++) {
        if (!(boot_pdpt[i] & PAGE_PRESENT)) {
            pdpt[i] = 0;
            continue;
        }
        /* 1GB huge page: copy as-is (no splitting needed yet) */
        if (boot_pdpt[i] & PAGE_HUGE) {
            pdpt[i] = boot_pdpt[i] & ~PAGE_USER;
            continue;
        }
        /* Regular PDPT entry pointing to a PD: deep-copy the PD */
        uint64_t* boot_pd = (uint64_t*)(uintptr_t)(boot_pdpt[i] & PTE_PHYS_MASK);
        uint64_t* new_pd = deep_copy_pd(boot_pd);
        if (!new_pd) { pdpt[i] = 0; continue; }
        pdpt[i] = ((uint64_t)(uintptr_t)new_pd) | (boot_pdpt[i] & ~PAGE_USER & ~PTE_PHYS_MASK) | PAGE_PRESENT;
    }
    return pdpt;
}

static uintptr_t* create_user_address_space(void) {
    phys_addr_t pml4_phys = pmm_alloc_page();
    if (!pml4_phys) return NULL;
    uintptr_t* pml4 = (uintptr_t*)pml4_phys;
    memset(pml4, 0, PAGE_SIZE);

    /* KE-13 FIX: Deep-copy the kernel's boot page tables instead of
     * sharing PML4[0..3] by pointer. The old code did:
     *   pml4[0] = boot_pml4[0];  // shares boot PDPT+PD
     * which meant user_map_page() encountered 2MB huge pages in the
     * shared boot PD. It would then write PTEs to the huge page's
     * physical address (misinterpreted as a PT pointer), corrupting
     * kernel BSS/data and causing a delayed #GP.
     *
     * The deep copy gives us private PDPTs and PDs. user_map_page()
     * can now safely split 2MB huge pages in the private PD copy
     * without affecting the kernel's own mappings.
     *
     * PAGE_USER is cleared on all copied entries so the user process
     * cannot access kernel memory through the identity mapping.
     * Only ELF segments and the user stack get PAGE_USER set by
     * user_map_page(). */
    extern uint64_t boot_pml4[];
    for (int i = 0; i < 4; i++) {
        if (!(boot_pml4[i] & PAGE_PRESENT)) continue;
        uint64_t* boot_pdpt = (uint64_t*)(uintptr_t)(boot_pml4[i] & PTE_PHYS_MASK);
        uint64_t* new_pdpt = deep_copy_pdpt(boot_pdpt);
        if (!new_pdpt) continue;
        pml4[i] = ((uint64_t)(uintptr_t)new_pdpt) | (boot_pml4[i] & ~PAGE_USER & ~PTE_PHYS_MASK) | PAGE_PRESENT;
    }

    return pml4;
}

/* Split a 2MB huge page into 512 individual 4KB page table entries.
 * This operates on the USER's private PD (created by deep_copy_pd), so
 * the kernel's own page tables are never modified.
 *
 * The new 4KB entries are mapped WITHOUT PAGE_USER so the user process
 * cannot access the kernel memory they cover. Only the specific PTE that
 * user_map_page() installs will have PAGE_USER set.
 *
 * Returns the pointer to the new page table, or NULL on failure. */
static uint64_t* split_2mb_huge_page(uint64_t* pd, int pd_idx, uint64_t old_pd_entry) {
    phys_addr_t pt_phys = pmm_alloc_page();
    if (!pt_phys) return NULL;
    uint64_t* pt = (uint64_t*)(uintptr_t)pt_phys;
    memset(pt, 0, PAGE_SIZE);

    /* Extract the 2MB huge page base and flags */
    uint64_t huge_base = old_pd_entry & PTE_PHYS_MASK;
    uint64_t huge_flags = old_pd_entry & ~PTE_PHYS_MASK & ~PAGE_HUGE;
    /* Ensure PAGE_USER is clear — kernel memory, not user-accessible */
    huge_flags &= ~PAGE_USER;

    /* Fill 512 PTEs covering the 2MB region */
    for (int i = 0; i < 512; i++) {
        pt[i] = huge_base + (uint64_t)i * PAGE_SIZE | huge_flags;
    }

    /* Replace the PD entry: point to our new PT instead of the huge page */
    pd[pd_idx] = pt_phys | (old_pd_entry & ~PAGE_HUGE & ~PTE_PHYS_MASK) | PAGE_PRESENT;

    return pt;
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

    /* KE-25 FIX: On x86 long mode, a user-mode (CPL=3) access requires the
     * U/S bit (PAGE_USER) to be set at EVERY level of the page walk — not
     * just the leaf PTE. If any intermediate entry (PML4/PDPT/PD) is
     * supervisor-only (U/S=0), the CPU treats the whole subtree as
     * supervisor and a user access faults with err=0x15 (P=1, U=1, I/D=1
     * under NXE) even when the leaf PTE has PAGE_USER set.
     *
     * The boot page tables (and thus the deep-copied user page tables) map
     * the low 4 GB with U/S=0 (supervisor) huge pages. Without this fix,
     * every user page the ELF loader maps in the low 4 GB — including
     * /init's .text at 0x401000 — is unreachable from ring 3, and the very
     * first instruction fetch in userspace page-faults. This was the root
     * cause of the long-standing "/init crashes immediately after jumping
     * to userspace" bug.
     *
     * Setting PAGE_USER on an intermediate entry only makes the walk
     * traversable by user mode; it does NOT grant access to the kernel
     * pages it contains — those still have U/S=0 on their LEAF PTEs, so
     * user access to them still faults. */
    int is_user = (flags & PAGE_USER) != 0;

    uintptr_t* pml4_virt = pml4;
    if (!(pml4_virt[pml4_idx] & PAGE_PRESENT)) {
        phys_addr_t pdpt_phys = pmm_alloc_page();
        if (!pdpt_phys) return;
        memset((void*)pdpt_phys, 0, PAGE_SIZE);
        pml4_virt[pml4_idx] = pdpt_phys | table_flags | PAGE_PRESENT;
    }
    if (is_user) pml4_virt[pml4_idx] |= PAGE_USER;
    uintptr_t* pdpt = (uintptr_t*)(pml4_virt[pml4_idx] & PTE_PHYS_MASK);

    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        phys_addr_t pd_phys = pmm_alloc_page();
        if (!pd_phys) return;
        memset((void*)pd_phys, 0, PAGE_SIZE);
        pdpt[pdpt_idx] = pd_phys | table_flags | PAGE_PRESENT;
    }
    if (is_user) pdpt[pdpt_idx] |= PAGE_USER;

    /* Check for 1GB huge page at PDPT level — cannot split, bail */
    if (pdpt[pdpt_idx] & PAGE_HUGE) {
        pr_warn("elf: cannot map 0x%lx — 1GB huge page in the way\n", (unsigned long)vaddr);
        return;
    }

    uintptr_t* pd = (uintptr_t*)(pdpt[pdpt_idx] & PTE_PHYS_MASK);

    /* KE-13 FIX: If the PD entry is a 2MB huge page (copied from boot
     * page tables by deep_copy_pd), split it into 4KB pages before
     * installing the user PTE. This is safe because we're modifying
     * the user's PRIVATE PD copy, not the kernel's boot PD. */
    if (pd[pd_idx] & PAGE_HUGE) {
        uint64_t* pt = split_2mb_huge_page(pd, pd_idx, pd[pd_idx]);
        if (!pt) return;
        if (is_user) pd[pd_idx] |= PAGE_USER;   /* KE-25: make PD entry user-traversable */
        pt[pt_idx] = phys | flags | PAGE_PRESENT;
        return;
    }

    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        phys_addr_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return;
        memset((void*)pt_phys, 0, PAGE_SIZE);
        pd[pd_idx] = pt_phys | table_flags | PAGE_PRESENT;
    }
    if (is_user) pd[pd_idx] |= PAGE_USER;       /* KE-25: make PD entry user-traversable */
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

    /* ASLR: randomize stack top by ASLR_STACK_BITS (12 bits = 16 MB).
     * Slide downward from USER_STACK_TOP_DEFAULT, stay page-aligned. */
    uint64_t stack_slide = (csprng_u64() & ((1ULL << ASLR_STACK_BITS) - 1)) << PAGE_SHIFT;
    uint64_t stack_top = USER_STACK_TOP_DEFAULT - stack_slide;
    uint64_t stack_size = USER_STACK_SIZE_DEFAULT;
    uint64_t stack_bottom = stack_top - stack_size;

    user_stack_ptr = stack_top;
    size_t stack_pages = stack_size / PAGE_SIZE;
    for (size_t i = 0; i < stack_pages; i++) {
        phys_addr_t phys = pmm_alloc_page();
        if (phys) {
            memset((void*)(uintptr_t)phys, 0, PAGE_SIZE);
            user_map_page(user_pml4, stack_bottom + i * PAGE_SIZE,
                          phys, PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE | PAGE_NX);
        }
    }
    user_stack_ptr = stack_top - 16;

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

    /* KE-24 FIX: Set TSS.RSP0 to a valid kernel stack BEFORE jumping to
     * ring 3. Without this, the first interrupt in userspace (the 1000 Hz
     * timer IRQ) loads RSP from tss.rsp0=0, the CPU cannot push the
     * interrupt frame to address 0, and triple-faults → silent reboot.
     * This was the root cause of the long-standing "/init reboots
     * immediately after jumping to userspace" bug.
     *
     * We use a static 16 KB stack buffer because elf_exec never returns
     * (it iretq's to userspace), so dynamic allocation would leak. The
     * buffer lives in kernel BSS within the 4 GB identity map, which is
     * deep-copied into every user PML4 as supervisor-present — so it is
     * reachable from ring-0 interrupt context regardless of CR3. */
    static uint8_t kstack[16384] __aligned(16);
    uint64_t kstack_top = (uint64_t)kstack + sizeof(kstack);
    extern void tss_set_rsp0(uint64_t);
    tss_set_rsp0(kstack_top);

    pr_info("elf: jumping to userspace (tss.rsp0=0x%x)\n", (unsigned)kstack_top);
    elf_jump_to_user(entry, user_stack_ptr, (uintptr_t)user_pml4);

    pr_warn("elf: userspace returned (should not happen)\n");
    return -1;
}
