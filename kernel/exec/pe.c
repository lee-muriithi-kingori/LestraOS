/*
 * Lestra OS - PE/COFF Loader (Windows EXE support)
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Loads Windows PE32+ (x86_64) executables into a user address space
 * and jumps to their entry point. This allows LestraOS to run native
 * Windows EXE files.
 *
 * Supported:
 *   - PE32+ (64-bit) executables
 *   - Static-linked binaries
 *   - Console subsystem
 *   - Basic memory mapping
 *
 * Limitations:
 *   - No DLL loading yet
 *   - No Win32 API emulation (needs Wine-like compat layer)
 *   - No thread support
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <lestra/vfs.h>
#include <lestra/sched.h>
#include <lestra/elf.h>
#include <string.h>

/* PE/COFF structures */
#define PE_MAGIC "\x4d\x5a"  /* MZ */

typedef struct {
    uint16_t e_magic;
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    uint32_t e_lfanew;
} __packed dos_header_t;

typedef struct {
    uint32_t Signature;
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} __packed coff_header_t;

typedef struct {
    uint16_t Magic;
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint64_t SizeOfStackReserve;
    uint64_t SizeOfStackCommit;
    uint64_t SizeOfHeapReserve;
    uint64_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
} __packed optional_header_t;

typedef struct {
    uint32_t VirtualAddress;
    uint32_t Size;
} __packed data_directory_t;

typedef struct {
    char     Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} __packed section_header_t;

#define IMAGE_SCN_CNT_CODE               0x00000020
#define IMAGE_SCN_CNT_INITIALIZED_DATA   0x00000040
#define IMAGE_SCN_MEM_EXECUTE            0x20000000
#define IMAGE_SCN_MEM_READ               0x40000000
#define IMAGE_SCN_MEM_WRITE              0x80000000

#define PE32_MAGIC 0x20b
#define IMAGE_SUBSYSTEM_WINDOWS_CUI     3

extern uintptr_t* user_pml4;

static uint64_t* deep_copy_pd(uint64_t* boot_pd) {
    phys_addr_t pd_phys = pmm_alloc_page();
    if (!pd_phys) return NULL;
    uint64_t* pd = (uint64_t*)(uintptr_t)pd_phys;
    for (int i = 0; i < 512; i++) {
        pd[i] = boot_pd[i] & ~PAGE_USER;
    }
    return pd;
}

static uint64_t* deep_copy_pdpt(uint64_t* boot_pdpt) {
    phys_addr_t pdpt_phys = pmm_alloc_page();
    if (!pdpt_phys) return NULL;
    uint64_t* pdpt = (uint64_t*)(uintptr_t)pdpt_phys;
    stac();
    for (int i = 0; i < 512; i++) {
        if (!(boot_pdpt[i] & PAGE_PRESENT)) {
            pdpt[i] = 0;
            continue;
        }
        if (boot_pdpt[i] & PAGE_HUGE) {
            pdpt[i] = boot_pdpt[i] & ~PAGE_USER;
            continue;
        }
        uint64_t* boot_pd = (uint64_t*)(uintptr_t)(boot_pdpt[i] & ~0xFFFULL);
        uint64_t* new_pd = deep_copy_pd(boot_pd);
        if (!new_pd) { clac(); return NULL; }
        pdpt[i] = (uintptr_t)new_pd | (boot_pdpt[i] & 0xFFFULL);
    }
    clac();
    return pdpt;
}

static uint64_t* create_user_pml4(void) {
    extern uint64_t boot_pml4[];
    uint64_t* pml4 = (uint64_t*)(uintptr_t)pmm_alloc_page();
    if (!pml4) return NULL;
    for (int i = 0; i < 512; i++) pml4[i] = 0;
    for (int i = 256; i < 512; i++) {
        if (!(boot_pml4[i] & PAGE_PRESENT)) continue;
        uint64_t* boot_pdpt = (uint64_t*)(uintptr_t)(boot_pml4[i] & ~0xFFFULL);
        uint64_t* new_pdpt = deep_copy_pdpt(boot_pdpt);
        if (!new_pdpt) return NULL;
        pml4[i] = (uintptr_t)new_pdpt | (boot_pml4[i] & 0xFFFULL);
    }
    return pml4;
}

/* Check if file is a PE executable */
int pe_is_pe(const uint8_t* data, size_t size) {
    if (size < sizeof(dos_header_t)) return 0;
    const dos_header_t* dos = (const dos_header_t*)data;
    if (dos->e_magic != 0x5A4D) return 0;  /* MZ */
    if (dos->e_lfanew + sizeof(coff_header_t) + sizeof(optional_header_t) > size) return 0;
    const uint32_t* pe_sig = (const uint32_t*)(data + dos->e_lfanew);
    if (*pe_sig != 0x00004550) return 0;  /* PE\0\0 */
    return 1;
}

/* Load PE executable into user address space */
int pe_exec(const char* path) {
    int fd = vfs_open(path, 0);
    if (fd < 0) {
        pr_warn("pe: '%s' not found\n", path);
        return -1;
    }

    static uint8_t pe_buf[65536];
    size_t total = 0;
    while (total < sizeof(pe_buf)) {
        ssize_t n = vfs_read(fd, &pe_buf[total], sizeof(pe_buf) - total);
        if (n <= 0) break;
        total += n;
    }
    vfs_close(fd);

    if (!pe_is_pe(pe_buf, total)) {
        pr_warn("pe: '%s' is not a valid PE executable\n", path);
        return -1;
    }

    const dos_header_t* dos = (const dos_header_t*)pe_buf;
    const coff_header_t* coff = (const coff_header_t*)(pe_buf + dos->e_lfanew);
    const optional_header_t* opt = (const optional_header_t*)(pe_buf + dos->e_lfanew + sizeof(coff_header_t));

    pr_info("pe: loading %s (machine=0x%x, subsystem=%d)\n", path, coff->Machine, opt->Subsystem);

    /* Create new user address space */
    user_pml4 = create_user_pml4();
    if (!user_pml4) {
        pr_warn("pe: failed to create user address space\n");
        return -1;
    }

    /* Map PE sections */
    const section_header_t* sections = (const section_header_t*)(pe_buf + dos->e_lfanew + sizeof(coff_header_t) + coff->SizeOfOptionalHeader);
    
    for (int i = 0; i < coff->NumberOfSections; i++) {
        const section_header_t* sec = &sections[i];
        if (sec->VirtualSize == 0) continue;
        
        uint64_t vaddr = opt->ImageBase + sec->VirtualAddress;
        uint64_t flags = PAGE_PRESENT | PAGE_USER;
        if (sec->Characteristics & IMAGE_SCN_MEM_WRITE) flags |= PAGE_WRITABLE;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) flags |= PAGE_NX;
        
        /* Map pages for section */
        size_t mapped = 0;
        while (mapped < sec->VirtualSize) {
            uint64_t page_vaddr = vaddr + mapped;
            phys_addr_t page_phys = pmm_alloc_page();
            if (!page_phys) {
                pr_warn("pe: out of memory mapping section %d\n", i);
                return -1;
            }
            memset((void*)(uintptr_t)page_phys, 0, 4096);
            
            /* Copy section data */
            size_t copy_size = sec->SizeOfRawData - mapped;
            if (copy_size > 4096) copy_size = 4096;
            if (sec->PointerToRawData + mapped + copy_size <= total) {
                memcpy((void*)(uintptr_t)page_phys, pe_buf + sec->PointerToRawData + mapped, copy_size);
            }
            
            user_map_page(user_pml4, page_vaddr, page_phys, flags);
            mapped += 4096;
        }
        pr_info("pe: section '%.8s' at VA 0x%x (size=%u)\n", sec->Name, (unsigned)vaddr, sec->VirtualSize);
    }

    /* Set up user stack */
    extern uint64_t user_stack_ptr;
    phys_addr_t stack_phys = pmm_alloc_page();
    if (!stack_phys) return -1;
    user_stack_ptr = 0x7FFFFFFFE000ULL;
    user_map_page(user_pml4, user_stack_ptr, stack_phys, PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);

    uint64_t entry = opt->ImageBase + opt->AddressOfEntryPoint;
    pr_info("pe: entry point at 0x%x\n", (unsigned)entry);

    /* Set TSS and jump to userspace */
    static uint8_t kstack[16384] __aligned(16);
    uint64_t kstack_top = (uint64_t)kstack + sizeof(kstack);
    extern void tss_set_rsp0(uint64_t);
    tss_set_rsp0(kstack_top);
    extern uint64_t g_syscall_kstack;
    g_syscall_kstack = kstack_top;

    struct process* cur = task_current();
    if (cur) {
        cur->pml4 = user_pml4;
        cur->entry_point = entry;
        cur->user_stack_ptr = user_stack_ptr;
    }

    extern void elf_jump_to_user(uint64_t entry, uint64_t stack, uint64_t pml4);
    elf_jump_to_user(entry, user_stack_ptr, (uintptr_t)user_pml4);

    pr_warn("pe: userspace returned (should not happen)\n");
    return -1;
}
