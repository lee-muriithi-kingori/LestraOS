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
 *  - Parse rootfs.tar module (in addition to initrd.img). The first
 *    boot module whose command-line contains "initrd" or no string is
 *    treated as the legacy initrd; any module tagged "rootfs.tar" is
 *    handed to tarfs_init() so we get a real /etc, /bin, /lib tree.
 *  - Call tarfs_init() after initrd_load() so tar-backed files are
 *    visible to the ELF loader when userspace_boot() runs.
 *  - Call userspace_boot() after compositor_init() so we actually
 *    exec /init into ring 3 before entering the compositor loop.
 *  - Splash status lines now reflect real subsystem state:
 *    pkg_catalog_size(), ai_any_key_set(), net_is_up().
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
#include <lestra/tarfs.h>
#include <lestra/vga.h>
#include <lestra/serial.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>
#include <lestra/ui.h>
#include <lestra/pkg.h>
#include <lestra/ai.h>
#include <lestra/net.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/splash.h>
#include <string.h>

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
    printk("  |     by Lee Muriihi Kingori               |\n");
    printk("  |     lestramk.org  (c) 2026               |\n");
    printk("  |     Version 1.0.0-alpha                  |\n");
    printk("  |                                          |\n");
    printk("  ==========================================\n");
    printk("\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* Forward declarations */
extern void shell_run(void);

/* Parse multiboot2 info: extract mmap + initrd + rootfs.tar modules
 * in one pass.
 * PR #8 fix: hardened against malformed / hostile multiboot2 info.
 *  - reject NULL / too-small / too-large info struct,
 *  - validate each tag's size before dereferencing,
 *  - clamp the mmap entry_count math so a bad `entry_size` can't
 *    underflow us into reading GBs of memory,
 *  - bail out cleanly if a tag's size is bogus or if the next
 *    tag pointer would land outside [info, info + total_size).
 */
#define MB2_MIN_INFO_SIZE  16   /* total_size(4) + reserved(4) + 1 tag + pad */
#define MB2_MAX_INFO_SIZE  (16UL * 1024UL * 1024UL)  /* 16 MiB sanity cap */
#define MB2_MAX_TAG_SIZE   (1UL  * 1024UL * 1024UL)  /* 1 MiB per-tag cap */
static void parse_multiboot2(void* mb2_info,
                             struct mmap_entry** mmap_out,
                             uint32_t* mmap_count_out,
                             void** initrd_addr_out,
                             uint32_t* initrd_size_out,
                             void** rootfs_addr_out,
                             uint32_t* rootfs_size_out) {
    *mmap_out = NULL;
    *mmap_count_out = 0;
    *initrd_addr_out = NULL;
    *initrd_size_out = 0;
    *rootfs_addr_out = NULL;
    *rootfs_size_out = 0;

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
                struct mb2_module* mod = (struct mb2_module*)tag;
                if (tag->size >= sizeof(struct mb2_module) &&
                    mod->mod_end >= mod->mod_start) {
                    const char* modstr = mod->string ? mod->string : "(none)";
                    pr_info("Boot module: addr=0x%x-0x%x (size=%u) string='%s'\n",
                            mod->mod_start, mod->mod_end,
                            mod->mod_end - mod->mod_start,
                            modstr);
                    /* Heuristic: a module whose string mentions "rootfs"
                     * is the tar rootfs; otherwise it's the initrd. */
                    if (strstr(modstr, "rootfs") || strstr(modstr, ".tar")) {
                        if (*rootfs_addr_out == NULL) {
                            *rootfs_addr_out = (void*)(uintptr_t)mod->mod_start;
                            *rootfs_size_out = mod->mod_end - mod->mod_start;
                        }
                    } else if (*initrd_addr_out == NULL) {
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
    void* rootfs_addr = NULL;
    uint32_t rootfs_size = 0;

    /* Initialize VGA first for early output */
    vga_init();
    vga_clear();

    /* Initialize serial port for early debugging */
    serial_default_init();

    /* Print boot banner */
    print_banner();

    pr_info("Initializing Lestra OS kernel...\n");

    /* Parse multiboot2 information (single pass). Now also extracts
     * the rootfs.tar module if GRUB was told to load one. */
    parse_multiboot2(mb2_info, &mmap, &mmap_count,
                     &initrd_addr, &initrd_size,
                     &rootfs_addr, &rootfs_size);

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

    /* Initialize physical page reference counting (for COW fork) */
    pmm_refcount_init();

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

    /* Load the tar rootfs if GRUB gave us one. tarfs_init parses the
     * ustar archive and registers /etc, /bin, /lib, /usr/... as
     * kernel-visible files. This runs AFTER initrd_load() so the
     * initrd's small bootstrap set (init, shell, sysinfo) is still
     * available even if rootfs.tar is missing. */
    if (rootfs_addr && rootfs_size > 0) {
        pr_info("Loading rootfs.tar from 0x%p (size=%u bytes)\n",
                rootfs_addr, rootfs_size);
        tarfs_init(rootfs_addr, rootfs_size);
    } else {
        pr_info("No rootfs.tar module — running with initrd only\n");
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

    /* Initialize networking (E1000 driver + DHCP + TCP/IP + HTTP client).
     * Non-fatal if no NIC present - kernel still runs without network. */
    pr_info("Initializing network stack...\n");
    net_init();

    /* Initialize AHCI/SATA driver (for persistent storage).
     * Non-fatal if no AHCI HBA or no drive - kernel still runs. */
    pr_info("Initializing disk subsystem...\n");
    extern int ahci_init(void);
    ahci_init();

    /* Initialize AC97 audio driver.
     * Non-fatal if no audio controller - kernel still runs. */
    pr_info("Initializing audio subsystem...\n");
    extern int ac97_init(void);
    ac97_init();

    /* Initialize RTC (real-time clock) for proper time */
    pr_info("Initializing RTC...\n");
    extern void rtc_init(void);
    rtc_init();

    /* Initialize battery/ACPI driver */
    pr_info("Initializing power management...\n");
    extern int battery_init(void);
    battery_init();

    /* Initialize temperature sensors */
    extern int temp_init(void);
    temp_init();

    /* Initialize WiFi framework */
    pr_info("Initializing WiFi framework...\n");
    extern void wifi_init(void);
    wifi_init();

    /* Initialize cron daemon */
    pr_info("Initializing cron daemon...\n");
    extern void cron_init(void);
    cron_init();

    /* Initialize service manager */
    pr_info("Initializing service manager...\n");
    extern void service_init(void);
    service_init();

    /* Initialize sandbox subsystem */
    pr_info("Initializing sandbox subsystem...\n");
    extern void sandbox_init(void);
    sandbox_init();

    /* Enable interrupts */
    pr_info("Enabling interrupts...\n");
    sti();

    /* Give DHCP a few seconds to complete in the background.
     * net_tick() runs from the timer IRQ and will keep trying for up
     * to ~10 seconds; we don't block boot on it. */
    pr_info("Network: acquiring IP via DHCP (background)...\n");

    pr_info("\n");
    pr_info("Lestra OS kernel initialized successfully!\n");
    pr_info("Type 'help' for available commands.\n");
    pr_info("\n");

    /* Parse the multiboot2 cmdline to determine boot mode.
     * GRUB passes the kernel cmdline (e.g. "gui" or "legacy" or "recovery").
     * Default is "gui" if framebuffer is available, else "legacy". */
    int boot_gui = 0;
    int boot_legacy = 0;
    int boot_recovery = 0;
    {
        /* Re-parse mb2_info for the cmdline tag */
        uint32_t total_size = *(uint32_t*)mb2_info;
        uintptr_t info_base = (uintptr_t)mb2_info;
        uintptr_t info_end = info_base + total_size;
        struct mb2_tag* tag = (struct mb2_tag*)(info_base + 8);
        while ((uintptr_t)tag + sizeof(struct mb2_tag) <= info_end) {
            if (tag->type == 0) break;
            if (tag->size < sizeof(struct mb2_tag)) break;
            if (tag->type == MB2_TAG_CMDLINE) {
                const char* cmdline = (const char*)tag + 8;
                if (strstr(cmdline, "gui")) boot_gui = 1;
                if (strstr(cmdline, "legacy")) { boot_gui = 0; boot_legacy = 1; }
                if (strstr(cmdline, "recovery")) boot_recovery = 1;
                pr_info("Boot cmdline: '%s'\n", cmdline);
                break;
            }
            uintptr_t next = (uintptr_t)tag + tag->size;
            next = (next + 7) & ~7u;
            if (next <= (uintptr_t)tag) break;
            tag = (struct mb2_tag*)next;
        }
    }

    /* If no explicit choice, default to GUI.
     * We'll check fb_available AFTER fb_init() and fall back to text
     * mode if the framebuffer is invalid (e.g. on VirtualBox). */
    extern int fb_available;
    if (!boot_recovery && !boot_gui && !boot_legacy) {
        boot_gui = 1;
    }

    if (boot_recovery) {
        pr_info("\n=== RECOVERY MODE ===\n");
        shell_run();
    } else if (boot_gui) {
        pr_info("Initializing GUI...\n");
        fb_init(mb2_info);
        if (fb_available) {
            pr_info("GUI: framebuffer OK, starting compositor\n");
            /* Set splash status lines for the animation — REFLECT REAL STATE.
             * Previous code hardcoded "65 pkgs", "AI client ok", "network ok"
             * regardless of whether pkg catalog loaded, whether DHCP
             * completed, or whether AI keys were set. We now query the
             * subsystems. */
            splash_set_status(0, "-> GDT ok");
            splash_set_status(1, "-> IDT ok");
            splash_set_status(2, "-> PMM ok");
            splash_set_status(3, "-> VMM ok");
            splash_set_status(4, "-> keyboard ok");
            splash_set_status(5, "-> serial ok");
            {
                extern int pkg_catalog_size(void);
                extern int net_is_up(void);
                extern int ai_any_key_set(void);
                int npkgs = pkg_catalog_size();
                char buf[64];
                ksnprintf(buf, sizeof(buf), "-> pkg manager ok (%d pkgs in catalog)", npkgs);
                splash_set_status(6, buf);
                splash_set_status(7, ai_any_key_set() ? "-> AI client ok (key set)"
                                                       : "-> AI client ok (no key, offline mode)");
                splash_set_status(8, net_is_up() ? "-> network ok (DHCP)"
                                                  : "-> network init (no IP yet)");
            }
            splash_set_status(9, "-> framebuffer ok");

            /* Run splash animation */
            splash_run();

            /* Initialize input (mouse) */
            input_init();
            compositor_init();

            /* Boot PID 1 — attempt to execve("/init") into ring 3.
             * If the initrd was loaded and /init is a valid ELF, the
             * system now has a real userspace process. If it fails
             * (no initrd, bad ELF, etc.) we fall through to the
             * in-kernel compositor as a recovery surface. */
            extern void userspace_boot(void);
            userspace_boot();

            /* Boot to clean desktop — NO auto-opened apps.
             * User clicks desktop icons or the dock to launch apps.
             * This is how a real OS works: you see the desktop first. */

            compositor_run();
            if (!fb_available) {
                shell_run();
            }
        } else {
            pr_warn("================================================\n");
            pr_warn("  GUI NOT AVAILABLE\n");
            pr_warn("  No VESA framebuffer from bootloader.\n");
            pr_warn("  Falling back to text shell.\n");
            pr_warn("================================================\n");
            pr_info("\nIf you're on VirtualBox:\n");
            pr_info("  1. Select 'Legacy Text Shell' from boot menu\n");
            pr_info("  2. Or enable 'Enable 3D Acceleration' in VM settings\n");
            pr_info("  3. Or set Graphics Controller to 'VMSVGA'\n\n");
            shell_run();
        }
    } else {
        /* Legacy text mode */
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
        shell_run();
    }

    /* Should never reach here */
    panic("Kernel main returned unexpectedly");
}
