/*
 * Lestra OS - Kernel Main Entry Point
 * Copyright (c) 2026 lestramk.org
 * C entry point from boot.asm; initializes all kernel subsystems
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
#include <lestra/fat32.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/splash.h>
#include <lestra/ssh_server.h>
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
    printk("  |     by Lee Muriithi Kingori               |\n");
    printk("  |     lestramk.org  (c) 2026               |\n");
    printk("  |     Version 1.0.0-alpha                  |\n");
    printk("  |                                          |\n");
    printk("  ==========================================\n");
    printk("\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* Forward declarations */
extern void shell_run(void);
extern void shell_run_serial(void);  /* serial-only shell (no VGA needed) */

/* Global cloud boot flag — set when cmdline contains "cloud".
 * Other subsystems (SSH server, HTTP management API) can check this
 * to decide whether to auto-start. */
int boot_cloud = 0;
/* KE-26: Full boot cmdline for /proc/cmdline */
char g_boot_cmdline[256] = {0};
int boot_serial = 0;

/* Return cloud boot flag for other subsystems */
int cloud_mode_active(void) {
    return boot_cloud;
}
int serial_mode_active(void) {
    return boot_serial;
}

/* Parse multiboot2 info: extract mmap + initrd + rootfs.tar modules in one pass.
 * Hardened against malformed multiboot2 info. */
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

    /* Security feature detection.
     * CR4.SMEP and CR4.SMAP are now flipped in gdt_init() (TIER 3).
     * g_security.smep/smap are already set there. */
    extern struct security_status g_security;
    g_security.nx = 1;  /* EFER.NXE already set in boot.asm */
    g_security.kptr_restrict = 1;
    g_security.aslr = 1;  /* stack/brk ASLR active (TIER 5) */
    pr_info("security: SMEP=%s SMAP=%s %s\n",
            g_security.smep ? "ENABLED" : "unavailable",
            g_security.smap ? "ENABLED" : "unavailable",
            (g_security.smep || g_security.smap) ? "(CR4 bits flipped)" : "(CPU lacks feature)");

    /* Initialize IDT and interrupts */
    pr_info("Initializing IDT...\n");
    idt_init();

    /* Initialize PIC */
    pr_info("Initializing PIC...\n");
    pic_init();

    /* Initialize ACPI table discovery early (before IRQ handlers register) */
    pr_info("Initializing ACPI table discovery (early)...\n");
    {
        extern int acpi_init(void);
        acpi_init();
    }

    /* Initialize Local APIC + IOAPIC; falls back to PIC if no MADT */
    pr_info("Initializing APIC subsystem...\n");
    {
        extern int apic_init(void);
        extern int irq_using_apic(void);
        if (apic_init() == 0) {
            pr_info("APIC active — interrupts routed via IOAPIC+LAPIC\n");
        } else {
            pr_info("APIC unavailable — using legacy 8259 PIC\n");
        }
        (void)irq_using_apic;
    }

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
            {0x10000000, 0x70000000, MMAP_USABLE, 0},   /* 1.75GB more (above heap) */
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

    /* Load initrd if present */
    if (initrd_addr && initrd_size > 0) {
        pr_info("Loading initrd from 0x%p (size=%u bytes)\n",
                initrd_addr, initrd_size);
        initrd_init(initrd_addr, initrd_size);
    } else {
        pr_warn("No initrd module from bootloader\n");
    }

    /* Load tar rootfs if GRUB provided one */
    if (rootfs_addr && rootfs_size > 0) {
        pr_info("Loading rootfs.tar from 0x%p (size=%u bytes)\n",
                rootfs_addr, rootfs_size);
        tarfs_init(rootfs_addr, rootfs_size);
    } else {
        pr_info("No rootfs.tar module — running with initrd only\n");
    }

    /* Run VFS self-test */
    vfs_selftest();

    /* Print memory stats */
    mm_print_stats();

    /* Initialize timer */
    pr_info("Initializing timer (1000 Hz)...\n");
    timer_init(1000);

    /* Initialize CSPRNG early for ASLR + canaries (needs timer_init first) */
    extern void csprng_init(void);
    csprng_init();
    extern void stack_canary_init(void);
    stack_canary_init();

    /* Initialize keyboard */
    pr_info("Initializing keyboard...\n");
    keyboard_init();

    /* Initialize package manager */
    pr_info("Initializing package manager...\n");
    pkg_init();

    /* Initialize AI subsystem (registers tools, clears key store) */
    pr_info("Initializing AI subsystem...\n");
    ai_init();

    /* Scan PCI bus — populates device table used by NIC, AHCI, audio, etc. */
    pr_info("Scanning PCI bus...\n");
    extern void pci_init(void);
    pci_init();

    /* Initialize networking (E1000 + DHCP + TCP/IP + HTTP) — non-fatal if no NIC */
    pr_info("Initializing network stack...\n");
    net_init();

    /* Initialize AHCI/SATA driver — non-fatal if no drive */
    pr_info("Initializing disk subsystem...\n");
    extern int ahci_init(void);
    ahci_init();

    /* Initialize VirtIO-blk for KVM/QEMU — non-fatal if no device */
    extern int virtio_blk_init(void);
    if (virtio_blk_init()) {
        extern int virtio_blk_is_present(void);
        if (virtio_blk_is_present()) {
            extern int virtio_blk_read_sectors(uint64_t, uint32_t, void*);
            extern int virtio_blk_write_sectors(uint64_t, uint32_t, const void*);
            if (fat32_init((fat32_read_fn)virtio_blk_read_sectors) == 0) {
                fat32_set_write_fn((fat32_write_fn)virtio_blk_write_sectors);

                /* Mount FAT32 at /fat32 via VFS */
                extern int vfs_mount(const char*, const char*, const char*);
                vfs_mount("virtio0", "/fat32", "fat32");

                /* List root directory as a boot-time demo. */
                struct fat32_dirent entries[16];
                int n = fat32_list_root(entries, 16);
                if (n > 0) {
                    pr_info("fat32: root directory (%d entries):\n", n);
                    for (int i = 0; i < n; i++) {
                        pr_info("  %s  cluster=%u  size=%u  %s\n",
                                entries[i].name, entries[i].first_cluster,
                                entries[i].file_size,
                                entries[i].is_dir ? "<DIR>" : "");
                    }
                    /* Read and print first file as a demo. */
                    struct fat32_dirent *f = NULL;
                    for (int i = 0; i < n; i++) {
                        if (!entries[i].is_dir && entries[i].file_size > 0 && entries[i].file_size < 512) {
                            f = &entries[i]; break;
                        }
                    }
                    if (f) {
                        char buf[256];
                        int rd = fat32_read_file(f->first_cluster, f->file_size, buf, sizeof(buf)-1);
                        if (rd > 0) {
                            buf[rd] = '\0';
                            pr_info("fat32: %s -> \"%s\"\n", f->name, buf);
                        }
                    }
                    /* KE-24 demo: write a test file to prove persistence. */
                    if (fat32_is_writable()) {
                        struct fat32_dirent test_de;
                        const char *test_name = "KE24TEST.TXT";
                        if (fat32_lookup(test_name, NULL) != 0) {
                            /* Create and write the test file */
                            if (fat32_create_file(test_name, &test_de) == 0) {
                                const char *msg = "lestraOS KE-24: FAT32 write works!";
                                uint32_t wc = 0, ws = 0;
                                fat32_write_file(test_de.first_cluster, 0,
                                                 msg, strlen(msg), 0, &wc, &ws);
                                /* Update both cluster and size in the dir entry */
                                fat32_update_entry(test_name, wc, ws);
                                pr_info("fat32: KE-24 test file written (%u bytes, cluster %u)\n", ws, wc);
                            }
                        } else {
                            pr_info("fat32: KE-24 test file already exists (persistence confirmed!)\n");
                        }
                    }
                }
            }
        }
    }

    /* Initialize audio (AC97) — non-fatal if no controller */
    pr_info("Initializing audio subsystem...\n");
    extern int ac97_init(void);
    ac97_init();

    /* Initialize RTC */
    pr_info("Initializing RTC...\n");
    extern void rtc_init(void);
    rtc_init();

    /* Initialize power management (battery) */
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

    pr_info("\n=== SECURITY AUDIT ===\n");
    pr_info("  SMEP:           %s\n", g_security.smep ? "ENABLED (CR4.SMEP)" : "unavailable");
    pr_info("  SMAP:           %s\n", g_security.smap ? "ENABLED (CR4.SMAP)" : "unavailable");
    pr_info("  NX:             ENABLED (EFER.NXE)\n");
    pr_info("  ASLR:           %s (stack+%d bits, brk+%d bits, TSC-CSPRNG)\n",
            "ENABLED", ASLR_STACK_BITS, ASLR_BRK_BITS);
    pr_info("  Stack canaries: %s (-fstack-protector-strong)\n",
            g_security.canaries ? "ENABLED" : "DISABLED");
    pr_info("  kptr_restrict:  %d\n", g_security.kptr_restrict);
    pr_info("  KASLR-lite:     ENABLED (heap+%d bits, TSC-early)\n", 8);
    pr_info("  Entropy pool:   ACTIVE (16 slots, IRQ-mixed: timer/KB/mouse)\n");
    pr_info("======================\n");

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
     * GRUB passes the kernel cmdline (e.g. "gui" or "legacy" or "recovery"
     * or "cloud serial"). Default is "gui" if framebuffer is available,
     * else "legacy". */
    int boot_gui = 0;
    int boot_legacy = 0;
    int boot_recovery = 0;
    boot_cloud = 0;
    boot_serial = 0;
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
                /* KE-26: Save the full cmdline for /proc/cmdline */
                extern char g_boot_cmdline[256];
                size_t cl = 0;
                while (cmdline[cl] && cl < 255) {
                    g_boot_cmdline[cl] = cmdline[cl];
                    cl++;
                }
                g_boot_cmdline[cl] = '\0';
                if (strstr(cmdline, "gui")) boot_gui = 1;
                if (strstr(cmdline, "legacy")) { boot_gui = 0; boot_legacy = 1; }
                if (strstr(cmdline, "recovery")) boot_recovery = 1;
                if (strstr(cmdline, "cloud")) { boot_cloud = 1; boot_gui = 0; boot_legacy = 0; boot_recovery = 0; }
                if (strstr(cmdline, "serial")) boot_serial = 1;
                pr_info("Boot cmdline: '%s'\n", cmdline);
                break;
            }
            uintptr_t next = (uintptr_t)tag + tag->size;
            next = (next + 7) & ~7u;
            if (next <= (uintptr_t)tag) break;
            tag = (struct mb2_tag*)next;
        }
    }

    /* Default to GUI if no explicit choice */
    extern int fb_available;
    if (!boot_recovery && !boot_gui && !boot_legacy && !boot_cloud) {
        boot_gui = 1;
    }

    /* Cloud/VPS boot mode — headless server with SSH + HTTP */
    if (boot_cloud) {
        pr_info("\n=== CLOUD/VPS SERVER MODE ===\n");
        pr_info("LestraOS Cloud/VPS Mode activated\n");
        pr_info("Serial console (COM1) is primary I/O\n");
        pr_info("Skipping GUI, framebuffer, and compositor\n");

        /* Start SSH server for remote access */
        pr_info("Starting SSH remote shell server...\n");
        extern void ssh_server_init(void);
        extern int  ssh_server_start(uint16_t);
        ssh_server_init();
        if (ssh_server_start(SSH_DEFAULT_PORT) == 0) {
            pr_info("LestraOS Cloud/VPS Mode - SSH server starting on port %u\n",
                    (unsigned)SSH_DEFAULT_PORT);
            printk("SSH server listening on port %u\n", (unsigned)SSH_DEFAULT_PORT);
        } else {
            pr_warn("SSH server failed to start (network not up?)\n");
        }

        /* Start HTTP management API (plaintext, for simple monitoring) */
        pr_info("Starting HTTP management API on port 8080...\n");
        extern void http_mgmt_start(uint16_t);
        http_mgmt_start(8080);
        pr_info("HTTP management API listening on port 8080\n");
        printk("HTTP management API on port 8080\n");

        /* Start HTTPS (TLS 1.2) management API for secure management.
         * The TLS server must be initialized first to generate the
         * self-signed certificate and key pair.  All management
         * operations (/reboot, /shutdown) are fully functional on
         * HTTPS without rate-limiting. */
        pr_info("Initializing TLS server (self-signed cert + key pair)...\n");
        extern void tls_server_init(void);
        tls_server_init();

        pr_info("Starting HTTPS management API on port 8443...\n");
        extern void http_mgmt_tls_start(uint16_t);
        http_mgmt_tls_start(8443);
        pr_info("HTTPS (TLS 1.2) management API listening on port 8443\n");
        printk("HTTPS management API on port 8443 (TLS 1.2)\n");
        printk("Use HTTPS for secure management operations\n");

        /* Enter serial-only shell — no VGA needed */
        pr_info("Entering serial shell...\n");
        shell_run_serial();
    } else if (boot_recovery) {
        pr_info("\n=== RECOVERY MODE ===\n");
        shell_run();
} else if (boot_gui) {
        pr_info("Initializing GUI...\n");
        fb_init(mb2_info);
        if (fb_available) {
            pr_info("GUI: framebuffer OK, starting compositor\n");
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

            splash_run();

            input_init();
            compositor_init();
            compositor_run();
            if (!fb_available) {
                shell_run();
            }
        } else {
            pr_warn("GUI NOT AVAILABLE - No VESA framebuffer\n");
            pr_info("VirtualBox: select 'Legacy Text Shell' or enable 3D Acceleration\n\n");
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
