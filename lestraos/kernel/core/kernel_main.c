/*
 * Lestra OS - Kernel Main Entry Point (FIXED)
 * Copyright (c) 2026 lestramk.org
 *
 * This is the C entry point called from boot.asm after transitioning
 * to long mode (x86_64). It initializes all kernel subsystems.
 *
 * FIXES:
 *  - Removed shadow initrd_init() that masked the real one in fs/vfs.c.
 *    Now GRUB-loaded initrd modules are actually parsed.
 *  - Use the multiboot2 module tag's 64-bit safe address decoding.
 *  - Cleaned up double mb2 parse (do it once).
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/panic.h>
#include <lestra/gdt.h>
#include <lestra/idt.h>
#include <lestra/irq.h>
#include <lestra/mm.h>
#include <lestra/sched.h>
#include <lestra/syscall.h>
#include <lestra/vfs.h>
#include <lestra/vga.h>
#include <lestra/serial.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>
#include <lestra/ui.h>
#include <lestra/pkg.h>
#include <lestra/ai.h>

/* Real initrd_init is provided by fs/vfs.c */
extern void initrd_init(void* addr, uint32_t size);

/* Multiboot2 info tag types */
#define MB2_TAG_END       0
#define MB2_TAG_CMDLINE   1
#define MB2_TAG_MODULE    3
#define MB2_TAG_MEMINFO   4
#define MB2_TAG_MMAP      6
#define MB2_TAG_FB        8
#define MB2_TAG_RSDP      14

struct mb2_tag {
    uint32_t type;
    uint32_t size;
} __packed;

struct mb2_meminfo {
    struct mb2_tag tag;
    uint32_t mem_lower;
    uint32_t mem_upper;
} __packed;

struct mb2_mmap {
    struct mb2_tag tag;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mmap_entry entries[];
} __packed;

struct mb2_module {
    struct mb2_tag tag;
    uint32_t mod_start;
    uint32_t mod_end;
    char string[];
} __packed;

/* Lestra OS boot banner */
static void print_banner(void) {
    vga_set_color(VGA_CYAN, VGA_BLACK);
    printk("\n");
    printk("  ==========================================\n");
    printk("  |                                          |\n");
    printk("  |     L e s t r a   O S                    |\n");
    printk("  |                                          |\n");
    printk("  |     by lestramk.org                      |\n");
    printk("  |     Version 1.0.0-alpha                  |\n");
    printk("  |                                          |\n");
    printk("  ==========================================\n");
    printk("\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* Forward declarations */
extern void shell_run(void);

/* Parse multiboot2 info: extract mmap + initrd modules in one pass.
 * PR #8 fix: hardened against malformed / hostile multiboot2 info.
 *  - reject NULL / too-small / too-large info struct,
  - validate each tag's size before dereferencing,
  - clamp the mmap entry_count math so a bad `entry_size` can't
    underflow us into reading GBs of memory,
  - bail out cleanly if a tag's size is bogus or if the next
    tag pointer would land outside [info, info + total_size).
 */
#define MB2_MIN_INFO_SIZE  16   /* total_size(4) + reserved(4) + 1 tag + pad */
#define MB2_MAX_INFO_SIZE  (16UL * 1024UL * 1024UL)  /* 16 MiB sanity cap */
#define MB2_MAX_TAG_SIZE   (1UL  * 1024UL * 1024UL)  /* 1 MiB per-tag cap */
static void parse_multiboot2(void* mb2_info,
                             struct mmap_entry** mmap_out,
                             uint32_t* mmap_count_out,
                             void** initrd_addr_out,
                             uint32_t* initrd_size_out) {
    *mmap_out = NULL;
    *mmap_count_out = 0;
    *initrd_addr_out = NULL;
    *initrd_size_out = 0;

    if (!mb2_info) {
        pr_warn("parse_multiboot2: mb2_info is NULL\n");
        return;
    }

    uint32_t total_size = *(volatile uint32_t*)mb2_info;
    if (total_size < MB2_MIN_INFO_SIZE || total_size > MB2_MAX_INFO_SIZE) {
        pr_warn("parse_multiboot2: implausible total_size=%u (refusing)\n", total_size);
        return;
    }

    uintptr_t info_base = (uintptr_t)mb2_info;
    uintptr_t info_end  = info_base + total_size;
    struct mb2_tag* tag = (struct mb2_tag*)(info_base + 8);

    while ((uintptr_t)tag >= info_base + 8 &&
           (uintptr_t)tag + sizeof(struct mb2_tag) <= info_end) {
        if (tag->type == MB2_TAG_END) break;

        /* Reject tags with bogus sizes (0 or implausibly large)
         * BEFORE dereferencing their payload. */
        if (tag->size < sizeof(struct mb2_tag) || tag->size > MB2_MAX_TAG_SIZE) {
            pr_warn("parse_multiboot2: bogus tag size=%u type=0x%x, stopping\n",
                    tag->size, tag->type);
            break;
        }
        /* And ensure the tag's payload fits inside info before we read it. */
        if ((uintptr_t)tag + tag->size > info_end) {
            pr_warn("parse_multiboot2: tag overflows info, stopping\n");
            break;
        }

        switch (tag->type) {
            case MB2_TAG_MEMINFO: {
                if (tag->size >= sizeof(struct mb2_meminfo)) {
                    struct mb2_meminfo* mem = (struct mb2_meminfo*)tag;
                    pr_info("Memory: Lower=%uKB, Upper=%uKB\n",
                            mem->mem_lower, mem->mem_upper);
                }
                break;
            }
            case MB2_TAG_MMAP: {
                if (tag->size >= sizeof(struct mb2_mmap)) {
                    struct mb2_mmap* mmap_tag = (struct mb2_mmap*)tag;
                    /* entry_size must be >= sizeof(struct mmap_entry)
                     * and non-zero, or the count math is meaningless. */
                    if (mmap_tag->entry_size >= sizeof(struct mmap_entry)) {
                        uint32_t count = (mmap_tag->tag.size - sizeof(struct mb2_mmap))
                                          / mmap_tag->entry_size;
                        *mmap_out = mmap_tag->entries;
                        *mmap_count_out = count;
                        pr_info("Memory map: %u entries\n", count);
                    } else {
                        pr_warn("parse_multiboot2: mmap entry_size=%u too small\n",
                                mmap_tag->entry_size);
                    }
                }
                break;
            }
            case MB2_TAG_MODULE: {
                if (tag->size >= sizeof(struct mb2_module) &&
                    mod->mod_end >= mod->mod_start) {
                    struct mb2_module* mod = (struct mb2_module*)tag;
                    pr_info("Boot module: addr=0x%x-0x%x (size=%u) string='%s'\n",
                            mod->mod_start, mod->mod_end,
                            mod->mod_end - mod->mod_start,
                            mod->string ? mod->string : "(none)");
                    /* Take the first module as initrd */
                    if (*initrd_addr_out == NULL) {
                        *initrd_addr_out = (void*)(uintptr_t)mod->mod_start;
                        *initrd_size_out = mod->mod_end - mod->mod_start;
                    }
                }
                break;
            }
            default:
                /* unknown tag — ignore but still advance */
                break;
        }

        /* Move to next tag (aligned to 8 bytes) */
        uintptr_t next = ALIGN_UP((uintptr_t)tag + tag->size, 8);
        if (next <= (uintptr_t)tag) break;  /* size 0 or overflow */
        tag = (struct mb2_tag*)next;
    }
}

/* Kernel main - C entry point */
void kernel_main(void* mb2_info) {
    struct mmap_entry* mmap = NULL;
    uint32_t mmap_count = 0;
    void* initrd_addr = NULL;
    uint32_t initrd_size = 0;

    /* Initialize VGA first for early output */
    vga_init();
    vga_clear();

    /* Initialize serial port for early debugging */
    serial_default_init();

    /* Print boot banner */
    print_banner();

    pr_info("Initializing Lestra OS kernel...\n");

    /* Parse multiboot2 information (single pass) */
    parse_multiboot2(mb2_info, &mmap, &mmap_count, &initrd_addr, &initrd_size);

    /* Initialize GDT */
    pr_info("Initializing GDT...\n");
    gdt_init();

    /* Initialize IDT and interrupts */
    pr_info("Initializing IDT...\n");
    idt_init();

    /* Initialize PIC */
    pr_info("Initializing PIC...\n");
    pic_init();

    /* Initialize memory management */
    pr_info("Initializing memory management...\n");
    if (mmap && mmap_count > 0) {
        pmm_init(mmap, mmap_count);
    } else {
        pr_warn("No memory map from bootloader, using defaults\n");
        /* Default memory map for 4GB QEMU VM */
        struct mmap_entry default_mmap[] = {
            {0x00000000, 0x0009FC00, MMAP_USABLE, 0},
            {0x00100000, 0x0FF00000, MMAP_USABLE, 0},   /* ~255MB */
            {0x10000000, 0x70000000, MMAP_USABLE, 0},   /* 1.75GB more */
            {0x100000000, 0x00000000, MMAP_RESERVED, 0}, /* >4GB reserved */
        };
        pmm_init(default_mmap, 4);
    }

    vmm_init();
    heap_init();

    /* Initialize scheduler */
    sched_init();

    /* Initialize syscall interface */
    syscall_init();

    /* Initialize VFS */
    vfs_init();

    /* Load initrd if present (FIX: previously this never ran) */
    if (initrd_addr && initrd_size > 0) {
        pr_info("Loading initrd from 0x%p (size=%u bytes)\n",
                initrd_addr, initrd_size);
        initrd_init(initrd_addr, initrd_size);
    } else {
        pr_warn("No initrd module from bootloader\n");
    }

    /* Print memory stats */
    mm_print_stats();

    /* Initialize timer */
    pr_info("Initializing timer (1000 Hz)...\n");
    timer_init(1000);

    /* Initialize keyboard */
    pr_info("Initializing keyboard...\n");
    keyboard_init();

    /* Initialize package manager */
    pr_info("Initializing package manager...\n");
    pkg_init();

    /* Initialize AI subsystem (registers tools, clears key store) */
    pr_info("Initializing AI subsystem...\n");
    ai_init();

    /* Enable interrupts */
    pr_info("Enabling interrupts...\n");
    sti();

    pr_info("\n");
    pr_info("Lestra OS kernel initialized successfully!\n");
    pr_info("Type 'help' for available commands.\n");
    pr_info("\n");

    /* Boot splash + menu before dropping into the shell. */
    ui_boot_splash();
    pr_info("Press a key within 5s, or 0 to skip to shell: ");
    uint64_t deadline = timer_get_ms() + 5000;
    int choice = -1;
    while (timer_get_ms() < deadline) {
        if (keyboard_has_key()) {
            choice = keyboard_getchar();
            break;
        }
    }
    if (choice != '0' && choice != -1) {
        ui_menu_loop();
    } else {
        ui_clear();
    }

    /* Start the kernel shell */
    shell_run();

    /* Should never reach here */
    panic("Kernel main returned unexpectedly");
}
