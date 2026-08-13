/*
 * Lestra OS - Kernel Shell (lsh) - ENHANCED
 * Copyright (c) 2026 lestramk.org
 *
 * A full-featured kernel-mode command-line shell.
 *
 * Commands:
 *   help, echo, clear, uname, free, reboot, shutdown, uptime, version, ps,
 *   cpuinfo, meminfo, neofetch, ui, theme, install, test
 *   pkg install/remove/list/search/info
 *   ai keys set/list/clear, ai chat, ai tools, ai agents
 *   file ls/cat/write/mkdir/rm/stat
 *   exec <user-binary-name>  (looks up in VFS)
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/keyboard.h>
#include <lestra/vga.h>
#include <lestra/timer.h>
#include <lestra/mm.h>
#include <lestra/panic.h>
#include <lestra/ui.h>
#include <lestra/pkg.h>
#include <lestra/ai.h>
#include <lestra/vfs.h>
#include <lestra/net.h>
#include <lestra/rtc.h>
#include <lestra/power.h>
#include <lestra/cron.h>
#include <lestra/wifi.h>
#include <lestra/sandbox.h>
#include <lestra/service.h>
#include <lestra/ssh_server.h>
#include <lestra/firewall.h>
#include <lestra/serial.h>
#include <lestra/pci.h>
#include <string.h>

#define CMD_MAX_LEN  512
#define ARG_MAX_NUM  32

static char input_buffer[CMD_MAX_LEN];
static char* argv[ARG_MAX_NUM + 1];
static int argc = 0;
static char cwd[64] = "/";

/* ----- command history (W1-F fix #4) -----------------------------------
 *
 * A 64-entry ring buffer of up-to-255-char command lines. Up arrow
 * recalls the previous entry; Down arrow moves forward. The browse
 * cursor (history_browse) is reset to "past the end" (i.e. new input)
 * whenever a command is executed. Entries are stored verbatim — no
 * expansion, no de-duplication (a user re-running the same command
 * gets two history slots, matching bash behaviour).
 *
 * The arrow keys themselves are NOT in the PS/2 keyboard's ASCII
 * buffer (kernel/drivers/char/keyboard.c drops E0-prefixed extended
 * scancodes). To make Up/Down work in the in-kernel shell we install
 * a tiny keyboard-handler hook (shell_kb_hook, below) that watches
 * for the E0 0x48 / E0 0x50 make sequences and injects two private
 * sentinel bytes (0x80 / 0x81) into the same key_buffer that
 * keyboard_getchar() drains. read_line() then recognises those
 * sentinels and triggers history navigation.
 *
 * For the serial shell (shell_run_serial), arrow keys arrive as ANSI
 * escape sequences (ESC [ A / ESC [ B) via serial_getchar(); those
 * are decoded directly in read_line_serial(). */
#define HIST_MAX     64
#define HIST_LINE    256
static char history[HIST_MAX][HIST_LINE];
static int  history_count = 0;     /* number of valid entries (<= HIST_MAX) */
static int  history_browse = 0;    /* index of the entry currently shown;
                                    * == history_count means "new input" */

/* Sentinel bytes injected by shell_kb_hook for arrow keys. These are
 * above 0x7F so they fall outside the printable ASCII range checked
 * by read_line()'s `c >= ' '` clause — they will never be inserted
 * into the input buffer as text. */
#define SHELL_KEY_UP    0x80
#define SHELL_KEY_DOWN  0x81

/* Append a command line to the history ring. Empty lines and lines
 * that are exact duplicates of the most recent entry are skipped
 * (matches the common shell convention). */
static void history_push(const char* line) {
    if (!line || !*line) return;
    /* Skip if identical to the most recent entry. */
    if (history_count > 0) {
        int last = (history_count - 1) % HIST_MAX;
        if (strcmp(history[last], line) == 0) return;
    }
    int slot = history_count % HIST_MAX;
    strncpy(history[slot], line, HIST_LINE - 1);
    history[slot][HIST_LINE - 1] = '\0';
    history_count++;
}

/* Recall entry `idx` into `out` (NUL-terminated). Returns 0 on
 * success, -1 if idx is out of range. */
static int history_get(int idx, char* out, size_t out_sz) {
    if (idx < 0 || idx >= history_count || !out || out_sz == 0) return -1;
    /* Entries wrap around the ring when history_count > HIST_MAX. */
    int base = (history_count > HIST_MAX) ? (history_count - HIST_MAX) : 0;
    int slot = (base + idx) % HIST_MAX;
    strncpy(out, history[slot], out_sz - 1);
    out[out_sz - 1] = '\0';
    return 0;
}

/* ----- arrow-key keyboard hook (PS/2 path only) -----------------------
 *
 * Installed by shell_run() before the main loop. Watches the raw
 * scancode stream for E0-prefixed Up/Down arrows and injects the
 * SHELL_KEY_UP / SHELL_KEY_DOWN sentinels into the keyboard buffer.
 *
 * This hook runs in IRQ1 context (called by keyboard.c's IRQ handler
 * before its own scancode-to-ASCII translation). It only touches the
 * E0-extended arrow keys — everything else falls through to
 * keyboard.c unchanged. */
static int shell_kb_e0_pending = 0;

static void shell_kb_hook(uint8_t scancode, char ascii) {
    (void)ascii;

    if (scancode == 0xE0) {
        shell_kb_e0_pending = 1;
        return;
    }
    if (!shell_kb_e0_pending) {
        /* Not an extended key — ignore (keyboard.c handles it). */
        return;
    }
    /* We have an E0-prefixed scancode. Only act on the make codes
     * (bit 7 clear); releases are silently absorbed. */
    shell_kb_e0_pending = 0;
    if (scancode & 0x80) return;   /* release */
    if (scancode == 0x48) {
        keyboard_inject_char((char)SHELL_KEY_UP);
    } else if (scancode == 0x50) {
        keyboard_inject_char((char)SHELL_KEY_DOWN);
    }
    /* Other extended keys (Left/Right/Home/End/PgUp/PgDn) are not
     * consumed — keyboard.c will continue to drop them as before. */
}

/* ----- prompt --------------------------------------------------------- */
static void print_prompt(void) {
    vga_set_color(VGA_CYAN, VGA_BLACK);
    printk("lestra");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    printk(":");
    vga_set_color(VGA_LIGHT_BLUE, VGA_BLACK);
    printk("%s", cwd);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    printk("$ ");
}

/* ----- arg parsing ---------------------------------------------------- */
static void parse_args(char* line) {
    argc = 0;
    while (*line && argc < ARG_MAX_NUM) {
        while (*line == ' ' || *line == '\t') line++;
        if (!*line) break;
        /* Quoted strings */
        if (*line == '"') {
            line++;
            argv[argc++] = line;
            while (*line && *line != '"') line++;
            if (*line == '"') *line++ = '\0';
        } else {
            argv[argc++] = line;
            while (*line && *line != ' ' && *line != '\t') line++;
            if (*line) *line++ = '\0';
        }
    }
    argv[argc] = NULL;
}

/* Forward declarations for AI CLI commands (defined in kernel/pkg/ai_cli.c) */
void cmd_claude(int argc, char** argv);
void cmd_glm(int argc, char** argv);
void cmd_gemini(int argc, char** argv);
void cmd_openai(int argc, char** argv);
void cmd_uai(int argc, char** argv);

/* Forward declarations for disk/mount commands */
void cmd_disk(int argc, char** argv);
void cmd_mount(int argc, char** argv);

/* ----- help ----------------------------------------------------------- */
static void cmd_help(void) {
    printk("\n");
    printk("Lestra Shell - Built-in Commands\n");
    printk("=================================\n\n");
    printk("  System:\n");
    printk("    help         Show this help\n");
    printk("    uname        Print system information\n");
    printk("    version      Show OS version\n");
    printk("    uptime       Show system uptime\n");
    printk("    free         Show memory usage\n");
    printk("    meminfo      Detailed memory info\n");
    printk("    cpuinfo      CPU information\n");
    printk("    ps           List processes\n");
    printk("    neofetch     System info with ASCII art\n");
    printk("    test         Run system tests\n");
    printk("    echo         Print arguments\n");
    printk("    cd [path]    Change current directory (default: /)\n");
    printk("    clear        Clear screen\n");
    printk("    reboot       Reboot system\n");
    printk("    shutdown     Shutdown system\n");
    printk("    mount        Show mounted filesystems / mount ext2\n");
    printk("\n");
    printk("  UI:\n");
    printk("    ui           Launch UI menu\n");
    printk("    theme <name> Switch theme (cyan|amber|green)\n");
    printk("\n");
    printk("  Package Manager (pkg):\n");
    printk("    pkg list                List available packages\n");
    printk("    pkg installed           List installed packages\n");
    printk("    pkg install <name>      Install a package\n");
    printk("    pkg remove <name>       Remove a package\n");
    printk("    pkg search <query>      Search the catalog\n");
    printk("    pkg info <name>         Show package info\n");
    printk("\n");
    printk("  AI Subsystem (ai):\n");
    printk("    ai setkey <p> <key>     Set API key (p: openai|claude|gemini|glm)\n");
    printk("    ai setendpoint <p> <url> Override endpoint (use http:// for local LLM/proxy)\n");
    printk("    ai setmodel <p> <model>  Override model name (e.g. 'ai setmodel glm glm-5.2')\n");
    printk("    ai keys list            Show configured API keys\n");
    printk("    ai keys clear <p>       Clear a provider key\n");
    printk("    ai chat <prompt>        Chat with the AI (makes real HTTP POST)\n");
    printk("    ai agent <prompt>       Chat with tool-calling agentic loop\n");
    printk("    ai tools                List available tools\n");
    printk("    ai providers            List supported providers + endpoints\n");
    printk("\n");
    printk("  Network:\n");
    printk("    network                 Show network status (IPv4 + IPv6)\n");
    printk("    ping <host-or-ip>       Send ICMPv4 echo request (e.g. 'ping 10.0.2.2')\n");
    printk("    ping6 <host-or-ip>      Send ICMPv6 echo request (e.g. 'ping6 fe80::1')\n");
    printk("    ifconfig                Show both IPv4 and IPv6 addresses\n");
    printk("    wget <url>              HTTP GET to a URL (e.g. 'wget http://example.com/')\n");
    printk("\n");
    printk("  Firewall (firewall):\n");
    printk("    firewall add <n> <accept|drop|reject> [proto src dst sport dport dir log]\n");
    printk("    firewall remove <name|id>              Remove a rule\n");
    printk("    firewall list                         List all rules\n");
    printk("    firewall flush                        Remove all rules\n");
    printk("    firewall status                       Show stats and default policies\n");
    printk("    firewall default <in|out> <accept|drop|reject>\n");
    printk("\n");
    printk("  VPS / Service Management (lee):\n");
    printk("    lee strt ssh            Start SSH remote shell (port 2222)\n");
    printk("    lee strt server         Start sandbox HTTP server\n");
    printk("    lee stop ssh            Stop SSH remote shell\n");
    printk("    lee stop server         Stop sandbox HTTP server\n");
    printk("    lee status              Show all services\n");
    printk("    lee service list        List services\n");
    printk("    lee service start <n>   Start a named service\n");
    printk("    lee service stop <n>    Stop a named service\n");
    printk("    lee net config <ip> <mask> <gw>   Set static IP\n");
    printk("    lee net dns <dns1> [dns2]         Set DNS servers\n");
    printk("    lee net dhcp                      Switch to DHCP\n");
    printk("\n");
    printk("  Files (file):\n");
    printk("    file ls [path]          List files in directory (default: /)\n");
    printk("    file cat <path>         Show file contents\n");
    printk("    file write <p> <text>   Write text to a file\n");
    printk("    file mkdir <path>       Create a directory\n");
    printk("    file rmdir <path>       Remove an empty directory\n");
    printk("    file rm <path>          Remove a file\n");
    printk("    file chmod <p> <mode>   Change file mode (octal, e.g. 755)\n");
    printk("    file stat <path>        Show file info\n");
    printk("\n");
    printk("  Hardware:\n");
    printk("    lspci        List PCI devices\n");
    printk("    sysinfo      Show full system information\n");
    printk("    netstat      Network status (IP, MAC, gateway, DNS, IPv6, firewall)\n");
    printk("    battery      Battery status\n");
    printk("    temp         Thermal sensors\n");
    printk("\n");
    printk("  System:\n");
    printk("    services     List registered services (alias: lee status)\n");
    printk("    packages     List available packages (alias: pkg list)\n");
    printk("    lee          Sandbox and service manager\n");
    printk("    firewall     Firewall management\n");
    printk("    cron         Cron daemon\n");
    printk("    whoami       Print current user (root — single-user mode)\n");
    printk("    hostname     Print system hostname\n");
    printk("\n");
    printk("  Other:\n");
    printk("    install      Show installer instructions\n");
    printk("    exit         Exit shell (halt)\n");
    printk("\n");
}

/* ----- neofetch ------------------------------------------------------- */
static void cmd_neofetch(void) {
    printk("\n");
    printk("       .______.     user@lestra\n");
    printk("       | >_<  |     --------------\n");
    printk("       | ---  |     OS:        LestraOS 1.0.0-alpha\n");
    printk("       |______|     Kernel:    lestra-build (x86_64 long-mode)\n");
    printk("                    Bootloader: GRUB/multiboot2 + boot.asm\n");
    printk("                    Shell:      lsh 1.0\n");
    printk("                    Theme:      %s\n", ui_theme_name());
    printk("                    Memory:     %u MB total, %u MB free\n",
           (unsigned)(pmm_get_total() / MiB),
           (unsigned)(pmm_get_free() / MiB));
    printk("                    Packages:   %d installed, %d available\n",
           pkg_count_installed(), pkg_count_available());
    printk("                    AI keys:    %d configured\n",
           ai_keys_count());
    printk("\n");
}

/* ----- theme ---------------------------------------------------------- */
static void cmd_theme(int argc, char** argv) {
    if (argc < 2) {
        printk("Current theme: %s\n", ui_theme_name());
        printk("Available: cyan, amber, green\n");
        return;
    }
    int t = -1;
    if (strcmp(argv[1], "cyan") == 0) t = 0;
    else if (strcmp(argv[1], "amber") == 0) t = 1;
    else if (strcmp(argv[1], "green") == 0) t = 2;
    if (t < 0) {
        printk("Unknown theme: %s (try: cyan, amber, green)\n", argv[1]);
        return;
    }
    ui_set_theme(t);
    printk("Theme switched to: %s\n", ui_theme_name());
}

/* ----- install -------------------------------------------------------- */
/* The in-OS installer (installer/install.c) is still a 17-line stub.
 * Until a real partitioner + ext2-formatter + file-copy pipeline lands
 * in the kernel, this command gives the user an honest picture of the
 * current storage situation (which disk controllers are visible, what
 * is mounted where) and points them at the working host-side installer
 * script (installer/install.sh) that writes a raw image to a device. */
static void cmd_install(void) {
    printk("\n=== Lestra OS Installer ===\n");
    printk("(in-kernel installer is a stub — uses host-side tools for now)\n\n");

    /* ---- In-OS storage picture ---- */
    extern int ahci_has_drive(void);
    extern int virtio_blk_is_present(void);
    extern int ext2_is_mounted(void);
    extern int fat32_is_mounted(void);

    printk("Storage detected:\n");
    printk("  AHCI/SATA disk:   %s\n",
           ahci_has_drive() ? "present" : "absent");
    printk("  VirtIO-blk disk:  %s\n",
           virtio_blk_is_present() ? "present" : "absent");
    printk("  ext2 mounted:     %s\n",
           ext2_is_mounted() ? "yes" : "no");
    printk("  FAT32 mounted:    %s\n",
           fat32_is_mounted() ? "yes" : "no");

    /* List the VFS mount table. */
    printk("\nMounts:\n");
    int nm = vfs_get_mount_count();
    if (nm == 0) {
        printk("  (none)\n");
    } else {
        for (int i = 0; i < nm; i++) {
            struct mount* m = vfs_get_mount(i);
            if (!m) continue;
            const char* fstype = "?";
            switch (m->fs_type) {
                case FS_TYPE_MEMFS: fstype = "memfs"; break;
                case FS_TYPE_EXT2:  fstype = "ext2";  break;
                case FS_TYPE_FAT32: fstype = "fat32"; break;
            }
            printk("  %-12s %s\n", m->path, fstype);
        }
    }

    /* ---- Host-side installer ---- */
    printk("\nTo install Lestra OS onto a real disk, run the host-side\n");
    printk("installer from a Linux host with the build outputs present:\n");
    printk("  Windows:  installer\\install.py --target <dev> --image build\\lestraos.img\n");
    printk("  POSIX:    installer/install.sh --target /dev/sdX --image build/lestraos.img\n");
    printk("Then boot the target device; Lestra OS loads automatically.\n");
    printk("\nIn-OS, you can also use the shell to prepare a disk manually:\n");
    printk("  disk                 - show AHCI disk info\n");
    printk("  mount ext2 <dev> <t> - mount an ext2 partition\n");
    printk("  save <path> <text>   - write a file to the mounted ext2 fs\n");
    printk("\n");
}


/* ----- echo ----------------------------------------------------------- */
static void cmd_echo(void) {
    for (int i = 1; i < argc; i++) {
        printk("%s", argv[i]);
        if (i < argc - 1) printk(" ");
    }
    printk("\n");
}

static void cmd_clear(void) { vga_clear(); }
static void cmd_uname(void) { printk("LestraOS\n"); }

static void cmd_free(void) {
    printk("              total        used        free\n");
    printk("Mem:      %8u    %8u    %8u\n",
           (unsigned)(pmm_get_total() / KiB),
           (unsigned)(pmm_get_used() / KiB),
           (unsigned)(pmm_get_free() / KiB));
}

void cmd_reboot(void) {
    printk("Rebooting system...\n");
    /* 8042 keyboard-controller reset = warm reboot. Works on QEMU
     * and on most real PC hardware. This is the right call for reboot. */
    outb(0x64, 0xFE);
    while (1) { hlt(); }
}

/* Global reboot/shutdown functions callable from other modules
 * (HTTP management API, SSH server, etc.) */
void reboot_system(void) {
    pr_info("System reboot requested (via management API)\n");
    outb(0x64, 0xFE);
    while (1) { hlt(); }
}

/* ----- ACPI shutdown helpers ------------------------------------------- */

/*
 * Real ACPI shutdown: scan PCI bus for PIIX4 PM device, search memory
 * for the ACPI RSDP, walk RSDT/XSDT → FACP → extract PM1a_CNT_BLK
 * port address, then write SLP_EN + SLP_TYP(S5) to trigger soft-off.
 *
 * If ACPI tables are not found (e.g. very old hardware), fall back to
 * the QEMU/Bochs-specific port writes that the previous code used.
 */

/* PCI config space access (type 1 mechanism) */
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

static uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (reg & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write_config(uint8_t bus, uint8_t dev, uint8_t func,
                             uint8_t reg, uint32_t val) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (reg & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

/* Scan PCI bus 0 for ACPI PM device (Intel PIIX4/ICH).
 * Vendor 8086, device 7000 (PIIX4) or 7113 (PIIX4E/ICH0 ACPI).
 * Returns: PM base I/O address from config register, or 0 if not found. */
static uint16_t pci_find_acpi_pm_base(void) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        for (uint8_t func = 0; func < 8; func++) {
            uint32_t id = pci_read_config(0, dev, func, 0);
            uint16_t vendor = id & 0xFFFF;
            uint16_t device = (id >> 16) & 0xFFFF;

            /* Intel PIIX4 / ICH ACPI PM device */
            if (vendor == 0x8086 && (device == 0x7000 || device == 0x7113)) {
                /* PM base is in PCI config register 0x40 (PIIX4) or 0x48 (ICH).
                 * Bits [15:7] = base address, bits [6:0] = reserved/zero.
                 * Mask out the low bits to get the I/O port base. */
                uint32_t pm_reg = pci_read_config(0, dev, func, 0x40);
                uint16_t pm_base = (uint16_t)((pm_reg & 0xFF80) >> 0);
                /* PIIX4 PM1a_CNT_BLK = pm_base + 0x04 */
                pr_info("acpi: found Intel PIIX4 ACPI (bus=0,dev=%d,func=%d,dev_id=0x%x)\n",
                        dev, func, device);
                pr_info("acpi: PM base = 0x%x, PM1a_CNT = 0x%x\n",
                        pm_base, pm_base + 4);
                return pm_base;
            }
        }
    }
    return 0;
}

/* ----- ACPI table structures ----- */

/* RSDP (Root System Description Pointer) — signature "RSD PTR " */
struct acpi_rsdp {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;          /* 0 = ACPI 1.0 (RSDT), 2+ = ACPI 2.0+ (XSDT) */
    uint32_t rsdt_address;      /* 32-bit physical address of RSDT */
    uint32_t length;            /* XSDT length (ACPI 2.0+) */
    uint64_t xsdt_address;      /* 64-bit physical address of XSDT (ACPI 2.0+) */
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __packed;

/* Common ACPI table header (first 36 bytes of every ACPI table) */
struct acpi_header {
    char     signature[4];      /* e.g. "FACP" */
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    char     creator_id[4];
    uint32_t creator_revision;
} __packed;

/* FACP (Fixed ACPI Description Table) — signature "FACP"
 *
 * Byte layout must match the ACPI spec exactly (see ACPI spec §5.2.9).
 * All offsets are relative to the start of the table:
 *   0-35:   Standard ACPI header (36 bytes)
 *   36-39:  FIRMWARE_CTRL (uint32_t)
 *   40-43:  DSDT (uint32_t)
 *   44:     PREFERRED_PM_PROFILE (uint8_t, was "model" in ACPI 1.0)
 *   45:     Reserved (uint8_t, must be 0)
 *   46-47:  SCI_INT (uint16_t)
 *   48-51:  SMI_CMD (uint32_t)
 *   52:     ACPI_ENABLE (uint8_t)
 *   53:     ACPI_DISABLE (uint8_t)
 *   54:     S4BIOS_REQ (uint8_t)
 *   55:     PSTATE_CNT (uint8_t)
 *   56-59:  PM1a_EVT_BLK (uint32_t)
 *   60-63:  PM1b_EVT_BLK (uint32_t)
 *   64-67:  PM1a_CNT_BLK (uint32_t)
 *   ... (rest of FACP fields follow)
 */
struct acpi_fadt {
    struct acpi_header header;
    uint32_t firmware_ctrl;      /* FACS address (32-bit) */
    uint32_t dsdt;               /* DSDT address (32-bit) */
    uint8_t  preferred_pm_profile; /* ACPI 1.0 model / 2.0+ preferred profile */
    uint8_t  reserved1;          /* Reserved (must be 0) */
    uint16_t sci_int;            /* SCI interrupt vector */
    uint32_t smi_cmd;            /* SMI command port */
    uint8_t  acpi_enable;        /* Value to write to SMI_CMD to enable ACPI */
    uint8_t  acpi_disable;       /* Value to write to SMI_CMD to disable ACPI */
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;       /* PM1a Event Block I/O port */
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;       /* PM1a Control Block I/O port — THIS IS WHAT WE NEED */
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;        /* Width of PM1_CNT register (usually 2) */
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
    uint8_t  gpe1_base;
    uint8_t  cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alrm;
    uint8_t  mon_alrm;
    uint8_t  century;
    /* ACPI 2.0+ extensions */
    uint16_t iapc_boot_arch;
    uint8_t  reserved2;
    uint32_t flags;
    /* More fields for ACPI 2.0+ (RESET_REG, RESET_VALUE, etc.) that we don't need */
} __packed;

/* Calculate simple ACPI checksum (sum of all bytes must be 0 mod 256) */
static int acpi_checksum_valid(const void* table, uint32_t len) {
    const uint8_t* p = (const uint8_t*)table;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
        sum += p[i];
    return (sum == 0);
}

/* Search for RSDP in memory.
 * Standard locations: (1) first 1KB of EBDA, (2) BIOS ROM 0xE0000–0xFFFFF.
 * The EBDA base address is stored as a 16-bit segment at real-mode address 0x40E.
 * In our flat memory model, that segment << 4 gives the physical address. */
static const struct acpi_rsdp* find_rsdp(void) {
    /* Method 1: Search EBDA (Extended BIOS Data Area) */
    /* Address 0x40E contains the EBDA segment (real-mode). Convert to physical:
     * physical = segment << 4. We read the 16-bit value at 0x40E. */
    uint16_t ebda_seg = *(volatile uint16_t*)(uintptr_t)0x40E;
    uintptr_t ebda_base = (uintptr_t)ebda_seg << 4;
    if (ebda_base >= 0x400 && ebda_base < 0xA0000) {
        /* Search first 1KB of EBDA for "RSD PTR " signature */
        for (uintptr_t addr = ebda_base; addr < ebda_base + 1024; addr += 16) {
            const struct acpi_rsdp* rsdp = (const struct acpi_rsdp*)addr;
            if (rsdp->signature[0] == 'R' && rsdp->signature[1] == 'S' &&
                rsdp->signature[2] == 'D' && rsdp->signature[3] == ' ' &&
                rsdp->signature[4] == 'P' && rsdp->signature[5] == 'T' &&
                rsdp->signature[6] == 'R' && rsdp->signature[7] == ' ') {
                /* Verify checksum (first 20 bytes for ACPI 1.0) */
                if (acpi_checksum_valid(rsdp, 20)) {
                    pr_info("acpi: RSDP found in EBDA at 0x%x (rev=%d)\n",
                            (unsigned)addr, rsdp->revision);
                    return rsdp;
                }
            }
        }
    }

    /* Method 2: Search BIOS ROM area (0xE0000 – 0xFFFFF) */
    for (uintptr_t addr = 0xE0000; addr < 0xFFFFF; addr += 16) {
        const struct acpi_rsdp* rsdp = (const struct acpi_rsdp*)addr;
        if (rsdp->signature[0] == 'R' && rsdp->signature[1] == 'S' &&
            rsdp->signature[2] == 'D' && rsdp->signature[3] == ' ' &&
            rsdp->signature[4] == 'P' && rsdp->signature[5] == 'T' &&
            rsdp->signature[6] == 'R' && rsdp->signature[7] == ' ') {
            if (acpi_checksum_valid(rsdp, 20)) {
                pr_info("acpi: RSDP found in BIOS ROM at 0x%x (rev=%d)\n",
                        (unsigned)addr, rsdp->revision);
                return rsdp;
            }
        }
    }

    pr_info("acpi: RSDP not found in memory\n");
    return NULL;
}

/* Walk RSDT/XSDT to find the FACP table.
 * Returns pointer to FACP (in physical memory), or NULL. */
static const struct acpi_fadt* find_fadt(const struct acpi_rsdp* rsdp) {
    if (!rsdp) return NULL;

    const struct acpi_header* table = NULL;

    if (rsdp->revision >= 2 && rsdp->xsdt_address) {
        /* ACPI 2.0+: use XSDT (64-bit address) */
        table = (const struct acpi_header*)(uintptr_t)rsdp->xsdt_address;
        if (table->signature[0] != 'X' || table->signature[1] != 'S' ||
            table->signature[2] != 'D' || table->signature[3] != 'T') {
            pr_warn("acpi: XSDT signature mismatch, trying RSDT\n");
            table = NULL;
        }
    }

    if (!table && rsdp->rsdt_address) {
        /* ACPI 1.0 or fallback: use RSDT (32-bit address) */
        table = (const struct acpi_header*)(uintptr_t)rsdp->rsdt_address;
        if (table->signature[0] != 'R' || table->signature[1] != 'S' ||
            table->signature[2] != 'D' || table->signature[3] != 'T') {
            pr_warn("acpi: RSDT signature mismatch\n");
            return NULL;
        }
    }

    if (!table) {
        pr_warn("acpi: no RSDT/XSDT address in RSDP\n");
        return NULL;
    }

    /* Verify table checksum */
    if (!acpi_checksum_valid(table, table->length)) {
        pr_warn("acpi: RSDT/XSDT checksum invalid\n");
        return NULL;
    }

    /* Walk entries: RSDT has 32-bit entries, XSDT has 64-bit entries */
    uint32_t entry_size = (rsdp->revision >= 2 && rsdp->xsdt_address) ? 8 : 4;
    uint32_t entry_count = (table->length - sizeof(struct acpi_header)) / entry_size;

    pr_info("acpi: walking %u entries in %s (len=%u)\n",
            entry_count,
            (entry_size == 8) ? "XSDT" : "RSDT",
            table->length);

    for (uint32_t i = 0; i < entry_count; i++) {
        uintptr_t entry_addr;
        if (entry_size == 8) {
            /* XSDT: 64-bit entries (we only use low 32 bits for safety) */
            const uint64_t* entries = (const uint64_t*)(table + 1);
            entry_addr = (uintptr_t)entries[i];
        } else {
            /* RSDT: 32-bit entries */
            const uint32_t* entries = (const uint32_t*)(table + 1);
            entry_addr = (uintptr_t)entries[i];
        }

        const struct acpi_header* hdr = (const struct acpi_header*)entry_addr;
        if (hdr->signature[0] == 'F' && hdr->signature[1] == 'A' &&
            hdr->signature[2] == 'C' && hdr->signature[3] == 'P') {
            pr_info("acpi: FACP found at 0x%x (len=%u)\n",
                    (unsigned)entry_addr, hdr->length);
            return (const struct acpi_fadt*)hdr;
        }
    }

    pr_warn("acpi: FACP table not found in RSDT/XSDT\n");
    return NULL;
}

/* Attempt to extract S5 sleep type from DSDT by searching for the _S5
 * package in the raw AML bytecode. This is a heuristic approach that
 * avoids needing a full AML interpreter.
 *
 * The _S5 package typically looks like:
 *   Name (_S5, Package () { <sleep_type>, <sleep_type>, 0, 0 })
 * In AML bytecode this appears as:
 *   08 5F 53 35 5F 12 ... (NameOp "_S5_" PackageOp ...)
 *
 * Returns: SLP_TYP value for S5 (typically 0-7), or -1 if not found. */
static int find_s5_slp_type(const struct acpi_fadt* fadt) {
    if (!fadt || !fadt->dsdt) return -1;

    const struct acpi_header* dsdt = (const struct acpi_header*)(uintptr_t)fadt->dsdt;
    if (dsdt->signature[0] != 'D' || dsdt->signature[1] != 'S' ||
        dsdt->signature[2] != 'D' || dsdt->signature[3] != 'T') {
        pr_info("acpi: DSDT signature mismatch\n");
        return -1;
    }

    uint32_t dsdt_len = dsdt->length;
    const uint8_t* aml = (const uint8_t*)((uintptr_t)dsdt + sizeof(struct acpi_header));
    uint32_t aml_len = dsdt_len - sizeof(struct acpi_header);

    /* Search for "_S5_" name in AML: 08 5F 53 35 5F (NameOp + "_S5_") */
    for (uint32_t i = 0; i < aml_len - 5; i++) {
        if (aml[i]   == 0x08 &&  /* NameOp */
            aml[i+1] == 0x5F &&  /* '_' */
            aml[i+2] == 0x53 &&  /* 'S' */
            aml[i+3] == 0x35 &&  /* '5' */
            aml[i+4] == 0x5F) {  /* '_' */
            /* Found "_S5_" name. Next should be a Package:
             * 0x12 (PackageOp) followed by encoded elements.
             * The first element is the SLP_TYP value for S5. */
            uint32_t j = i + 5;
            if (j < aml_len && aml[j] == 0x12) {
                /* PackageOp: skip package length encoding, find first data element.
                 * AML package length encoding varies:
                 *   If high 2 bits of byte P are 00: length = P & 0x3F, 1 byte total
                 *   If 01: length = (P & 0x0F) | next_byte << 4, 2 bytes total
                 *   If 10: length = next 2 bytes as little-endian + (P & 0x0F) << 16, 3 bytes
                 *   If 11: length = next 4 bytes as little-endian, 5 bytes total
                 */
                j++;
                uint8_t pkg_lead = aml[j];
                uint8_t num_elements = aml[j+1];  /* element count */
                uint32_t pkg_hdr_size = 2;  /* lead byte + element count byte */

                /* Determine package length encoding */
                uint8_t lead_high = (pkg_lead >> 6) & 3;
                if (lead_high == 0) {
                    /* 1-byte length */
                    pkg_hdr_size = 2;
                } else if (lead_high == 1) {
                    /* 2-byte length */
                    pkg_hdr_size = 3;
                } else if (lead_high == 2) {
                    /* 3-byte length */
                    pkg_hdr_size = 4;
                } else {
                    /* 5-byte length */
                    pkg_hdr_size = 6;
                }

                /* Skip past the package header to the first element */
                j += pkg_hdr_size;

                /* The first element is the SLP_TYP for S5.
                 * AML encoding for a small integer (0-63): 0x00 | value
                 * For a byte value (0-255): 0x0A then byte
                 * The common values are 0, 1, 2, 5, 7. */
                if (j < aml_len) {
                    uint8_t elem = aml[j];
                    if (elem <= 0x3F) {
                        /* Small integer: value = elem itself */
                        int slp_type = (int)elem;
                        pr_info("acpi: _S5 SLP_TYP = %d (from DSDT AML)\n", slp_type);
                        return slp_type;
                    } else if (elem == 0x0A && j + 1 < aml_len) {
                        /* Byte prefix: value is next byte */
                        int slp_type = (int)aml[j+1];
                        pr_info("acpi: _S5 SLP_TYP = %d (from DSDT AML byte)\n", slp_type);
                        return slp_type;
                    }
                }
            }
        }
    }

    pr_info("acpi: _S5 package not found in DSDT\n");
    return -1;
}

/* ----- ACPI shutdown implementation ----- */

void cmd_shutdown(void) {
    printk("Shutting down...\n");

    /* Strategy 1: Find ACPI tables and use proper PM1a_CNT_BLK shutdown.
     * This works on real hardware with proper ACPI implementation. */

    /* First try PCI scan for Intel PIIX4/ICH PM device */
    uint16_t pci_pm_base = pci_find_acpi_pm_base();

    /* Then try full ACPI RSDP → FACP path */
    const struct acpi_rsdp* rsdp = find_rsdp();
    const struct acpi_fadt* fadt = find_fadt(rsdp);

    if (fadt && fadt->pm1a_cnt_blk) {
        /* We found the real PM1a_CNT port from the ACPI FACP table! */
        uint16_t pm1a_cnt = (uint16_t)fadt->pm1a_cnt_blk;
        uint8_t  pm1_cnt_len = fadt->pm1_cnt_len;

        /* Determine S5 sleep type.
         * Try DSDT _S5 package first, then fall back to common defaults. */
        int slp_type_s5 = find_s5_slp_type(fadt);
        if (slp_type_s5 < 0) {
            /* Common defaults per chipset:
             * - Intel PIIX4/ICH: S5 SLP_TYP = 0
             * - Most modern Intel: S5 SLP_TYP = 0 or 5
             * - Some chipsets: S5 SLP_TYP = 1, 2, 7
             * We'll try 0 first (covers most Intel + QEMU). */
            slp_type_s5 = 0;
            pr_info("acpi: using default S5 SLP_TYP = 0\n");
        }

        /* ACPI shutdown formula:
         * PM1a_CNT value = (SLP_TYP << SLP_TYP_BIT_POSITION) | SLP_EN_BIT
         * SLP_TYP_BIT_POSITION = 10 (bits [12:10] in PM1_CNT register)
         * SLP_EN_BIT = bit 13
         *
         * For 16-bit PM1_CNT: value = (slp_type << 10) | (1 << 13)
         * For 32-bit PM1_CNT (pm1_cnt_len == 4): same formula but write 32-bit.
         */
        uint16_t sleep_val = (uint16_t)((slp_type_s5 << 10) | (1 << 13));
        pr_info("acpi: PM1a_CNT_BLK = 0x%x, writing 0x%x (SLP_TYP=%d, SLP_EN)\n",
                pm1a_cnt, sleep_val, slp_type_s5);

        /* Write to PM1a_CNT_BLK to trigger S5 shutdown */
        if (pm1_cnt_len == 4) {
            outl(pm1a_cnt, (uint32_t)sleep_val);
        } else {
            outw(pm1a_cnt, sleep_val);
        }

        /* Also write to PM1b_CNT_BLK if present */
        if (fadt->pm1b_cnt_blk) {
            uint16_t pm1b_cnt = (uint16_t)fadt->pm1b_cnt_blk;
            if (pm1_cnt_len == 4)
                outl(pm1b_cnt, (uint32_t)sleep_val);
            else
                outw(pm1b_cnt, sleep_val);
        }

        /* Wait a moment for the power-off to take effect */
        for (volatile int i = 0; i < 1000000; i++);
    }

    if (pci_pm_base) {
        /* PCI-found PM base: PIIX4 PM1a_CNT = pm_base + 4.
         * Try S5 shutdown via the PCI-discovered port. */
        uint16_t pm1a_cnt = pci_pm_base + 4;
        uint16_t sleep_val = (0 << 10) | (1 << 13);  /* SLP_TYP=0, SLP_EN */
        pr_info("acpi: PCI PIIX4 PM1a_CNT = 0x%x, writing 0x%x\n",
                pm1a_cnt, sleep_val);
        outw(pm1a_cnt, sleep_val);
        for (volatile int i = 0; i < 1000000; i++);
    }

    /* Strategy 2: QEMU/Bochs/VirtualBox-specific port writes.
     * These are the old hardcoded ports that work in emulators. */
    outw(0x604, 0x2000);          /* QEMU/Bochs PIIX4 ACPI shutdown */
    outw(0xB004, 0x2000);         /* older VirtualBox ACPI shutdown register */
    outw(0x4004, 0x3400);         /* Bochs/QEMU older ACPI */

    printk("ACPI shutdown failed; falling back to 8042 reset (reboot).\n");
    outb(0x64, 0xFE);
    while (1) { hlt(); }
}

/* Global shutdown function callable from other modules */
void shutdown_system(void) {
    pr_info("System shutdown requested (via management API)\n");
    cmd_shutdown();
}

static void cmd_uptime(void) {
    uint64_t ms = timer_get_ms();
    uint64_t sec = ms / 1000;
    uint64_t min = sec / 60;
    uint64_t hr = min / 60;
    printk("Uptime: %02lu:%02lu:%02lu\n", hr, min % 60, sec % 60);
}

static void cmd_version(void) {
    printk("Lestra OS version 1.0.0-alpha\n");
    printk("Built for x86_64 architecture\n");
    printk("Copyright (c) 2026 lestramk.org\n");
}

static void cmd_ps(void) {
    /* Query the real scheduler process table instead of printing
     * hardcoded fake rows. If the scheduler has no tasks (we're in
     * single-kernel-context fallback mode), say so honestly. */
    extern int sched_get_proc_info(int idx, int* pid, char* name, int* state);
    printk("  PID  PPID  STATE      NAME\n");
    int any = 0;
    for (int i = 0; i < 32; i++) {
        int pid, state;
        char name[32];
        if (sched_get_proc_info(i, &pid, name, &state)) {
            const char* s = (state == 2) ? "running " :
                            (state == 1) ? "runnable" :
                            (state == 3) ? "blocked " :
                            (state == 4) ? "zombie  " : "free    ";
            printk("  %4d  ----  %s  %s\n", pid, s, name);
            any = 1;
        }
    }
    if (!any) {
        printk("  (no userspace processes — running in kernel-context mode)\n");
    }
}

static void cmd_cpuinfo(void) {
    /* Use real CPUID instead of hardcoding "QEMU Virtual CPU". */
    char vendor[13] = {0};
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    memcpy(vendor + 0, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = '\0';

    uint32_t max_leaf = eax;
    printk("CPU Information:\n");
    printk("  Architecture: x86_64\n");
    printk("  Vendor:       %s\n", vendor);
    printk("  Max leaf:     0x%x\n", max_leaf);

    /* Leaf 1: family, model, stepping, features */
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    uint32_t stepping = eax & 0xF;
    uint32_t model    = (eax >> 4) & 0xF;
    uint32_t family   = (eax >> 8) & 0xF;
    uint32_t ext_model = (eax >> 16) & 0xF;
    uint32_t ext_family = (eax >> 20) & 0xFF;
    uint32_t disp_family = family + (family == 0xF ? ext_family : 0);
    uint32_t disp_model  = model | (family == 0xF ? (ext_model << 4) : 0);
    printk("  Family:       0x%x  Model: 0x%x  Stepping: 0x%x\n",
           disp_family, disp_model, stepping);

    /* Logical CPU count from EBX[23:16] */
    uint32_t logical = (ebx >> 16) & 0xFF;
    printk("  Logical CPUS: %u\n", logical);

    /* Feature flags */
    printk("  Features:     ");
    if (edx & (1 << 0))  printk("fpu ");
    if (edx & (1 << 1))  printk("vme ");
    if (edx & (1 << 2))  printk("de ");
    if (edx & (1 << 3))  printk("pse ");
    if (edx & (1 << 4))  printk("tsc ");
    if (edx & (1 << 5))  printk("msr ");
    if (edx & (1 << 6))  printk("pae ");
    if (edx & (1 << 8))  printk("cx8 ");
    if (edx & (1 << 9))  printk("apic ");
    if (edx & (1 << 15)) printk("cmov ");
    if (edx & (1 << 23)) printk("mmx ");
    if (edx & (1 << 24)) printk("fxsr ");
    if (edx & (1 << 25)) printk("sse ");
    if (edx & (1 << 26)) printk("sse2 ");
    if (edx & (1 << 28)) printk("ht ");
    if (edx & (1 << 29)) printk("lm ");
    if (ecx & (1 << 0))  printk("sse3 ");
    if (ecx & (1 << 9))  printk("ssse3 ");
    if (ecx & (1 << 20)) printk("sse4.2 ");
    if (ecx & (1 << 28)) printk("avx ");
    printk("\n");
}

static void cmd_meminfo(void) {
    printk("Memory Information:\n");
    printk("  Total RAM:    %u MB\n", (unsigned)(pmm_get_total() / MiB));
    printk("  Used:         %u MB\n", (unsigned)(pmm_get_used() / MiB));
    printk("  Free:         %u MB\n", (unsigned)(pmm_get_free() / MiB));
    printk("  Kernel Heap:  %u KB\n", (unsigned)(heap_get_used() / KiB));
}

static void cmd_test(void) {
    printk("Running system tests...\n\n");

    printk("[1/5] Memory allocation... ");
    void* p = kmalloc(1024);
    if (p) {
        kfree(p);
        printk("PASS\n");
    } else {
        printk("FAIL\n");
    }

    printk("[2/5] String compare... ");
    if (strcmp("hello", "hello") == 0) printk("PASS\n");
    else printk("FAIL\n");

    printk("[3/5] String length... ");
    if (strlen("hello") == 5) printk("PASS\n");
    else printk("FAIL\n");

    printk("[4/5] Timer ticks... ");
    uint64_t t1 = timer_get_ticks();
    if (t1 > 0) printk("PASS (ticks=%u)\n", (unsigned)t1);
    else printk("FAIL\n");

    printk("[5/5] Page allocator... ");
    phys_addr_t page = pmm_alloc_page();
    if (page) {
        pmm_free_page(page);
        printk("PASS\n");
    } else {
        printk("FAIL\n");
    }

    printk("\nAll tests completed.\n");
}

/* ----- pkg subcommands ------------------------------------------------ */
static void cmd_pkg(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: pkg <install|remove|list|installed|search|info|repo|update> [args]\n");
        printk("  pkg list                   List all available packages\n");
        printk("  pkg installed              List installed packages\n");
        printk("  pkg install <name>         Install a package\n");
        printk("  pkg remove <name>          Remove a package\n");
        printk("  pkg search <query>         Search all repos\n");
        printk("  pkg info <name>            Show package details\n");
        printk("  pkg update                 Refresh repo catalogs\n");
        printk("  pkg repo list              List configured repos\n");
        printk("  pkg repo add <name> <url>  Add a repository\n");
        printk("  pkg repo remove <name>     Remove a repository\n");
        printk("\nRepos: core, websec, devtools, multimedia, lestra\n");
        return;
    }
    if (strcmp(argv[1], "list") == 0) {
        pkg_list_available();
    } else if (strcmp(argv[1], "installed") == 0) {
        pkg_list_installed();
    } else if (strcmp(argv[1], "install") == 0) {
        if (argc < 3) { printk("Usage: pkg install <name>\n"); return; }
        pkg_install(argv[2]);
    } else if (strcmp(argv[1], "remove") == 0) {
        if (argc < 3) { printk("Usage: pkg remove <name>\n"); return; }
        pkg_remove(argv[2]);
    } else if (strcmp(argv[1], "search") == 0) {
        if (argc < 3) { printk("Usage: pkg search <query>\n"); return; }
        pkg_search(argv[2]);
    } else if (strcmp(argv[1], "info") == 0) {
        if (argc < 3) { printk("Usage: pkg info <name>\n"); return; }
        pkg_info(argv[2]);
    } else if (strcmp(argv[1], "update") == 0) {
        extern void pkg_repo_update(void);
        pkg_repo_update();
    } else if (strcmp(argv[1], "repo") == 0) {
        if (argc < 3) {
            printk("Usage: pkg repo <list|add|remove> [args]\n");
            return;
        }
        if (strcmp(argv[2], "list") == 0) {
            extern void pkg_repo_list(void);
            pkg_repo_list();
        } else if (strcmp(argv[2], "add") == 0) {
            if (argc < 5) { printk("Usage: pkg repo add <name> <url>\n"); return; }
            extern int pkg_repo_add(const char*, const char*);
            pkg_repo_add(argv[3], argv[4]);
        } else if (strcmp(argv[2], "remove") == 0) {
            if (argc < 4) { printk("Usage: pkg repo remove <name>\n"); return; }
            extern int pkg_repo_remove(const char*);
            pkg_repo_remove(argv[3]);
        } else {
            printk("Unknown repo subcommand: %s\n", argv[2]);
        }
    } else {
        printk("Unknown pkg subcommand: %s\n", argv[1]);
    }
}

/* ----- ai subcommands ------------------------------------------------- */
static void cmd_ai(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: ai <setkey|setendpoint|setmodel|chat|agent|tools|providers|keys>\n");
        printk("  ai setkey <provider> <key>     Set API key (e.g. 'ai setkey glm mykey123')\n");
        printk("  ai setendpoint <provider> <url> Override endpoint (use http:// for local LLM/proxy)\n");
        printk("  ai setmodel <provider> <model> Override model name (e.g. 'ai setmodel glm glm-5.2')\n");
        printk("  ai chat <prompt>               Send a chat message to the configured provider\n");
        printk("  ai agent <prompt>              Run with tools (shell, file_read, pkg_install, etc.)\n");
        printk("  ai providers                   List configured providers + endpoints\n");
        printk("  ai tools                       List registered tools\n");
        printk("  ai keys list                   Show which providers have keys set\n");
        printk("\nProviders: openai, claude, gemini, glm\n");
        printk("Default model for GLM is glm-4.6; override with 'ai setmodel glm glm-5.2'\n");
        return;
    }
    if (strcmp(argv[1], "setkey") == 0) {
        if (argc < 4) {
            printk("Usage: ai setkey <provider> <key>\n");
            printk("Providers: openai, claude, gemini, glm\n");
            return;
        }
        int p = ai_provider_from_name(argv[2]);
        if (p < 0) {
            printk("Unknown provider: %s\n", argv[2]);
            return;
        }
        if (ai_keys_set(p, argv[3]) == 0) {
            printk("API key set for %s\n", ai_provider_name(p));
        }
    } else if (strcmp(argv[1], "setendpoint") == 0) {
        if (argc < 4) {
            printk("Usage: ai setendpoint <provider> <url>\n");
            printk("Example: ai setendpoint glm http://10.0.2.2:11434/v1/chat/completions\n");
            printk("(points at a local Ollama server running on the host at port 11434)\n");
            return;
        }
        int p = ai_provider_from_name(argv[2]);
        if (p < 0) {
            printk("Unknown provider: %s\n", argv[2]);
            return;
        }
        if (ai_provider_set_endpoint(p, argv[3]) == 0) {
            printk("Endpoint set for %s\n", ai_provider_name(p));
        }
    } else if (strcmp(argv[1], "setmodel") == 0) {
        if (argc < 4) {
            printk("Usage: ai setmodel <provider> <model>\n");
            printk("Example: ai setmodel glm glm-5.2\n");
            return;
        }
        int p = ai_provider_from_name(argv[2]);
        if (p < 0) {
            printk("Unknown provider: %s\n", argv[2]);
            return;
        }
        if (ai_provider_set_model(p, argv[3]) == 0) {
            printk("Model set for %s\n", ai_provider_name(p));
        }
    } else if (strcmp(argv[1], "keys") == 0) {
        if (argc < 3 || strcmp(argv[2], "list") == 0) {
            ai_keys_list();
        } else if (strcmp(argv[2], "clear") == 0) {
            if (argc < 4) {
                printk("Usage: ai keys clear <provider>\n");
                return;
            }
            int p = ai_provider_from_name(argv[3]);
            if (p < 0) { printk("Unknown provider\n"); return; }
            ai_keys_clear(p);
            printk("Cleared key for %s\n", ai_provider_name(p));
        } else {
            printk("Unknown keys subcommand: %s\n", argv[2]);
        }
    } else if (strcmp(argv[1], "chat") == 0) {
        if (argc < 3) {
            printk("Usage: ai chat <your prompt>\n");
            return;
        }
        char prompt[AI_PROMPT_MAX];
        prompt[0] = 0;
        int len = 0;
        for (int i = 2; i < argc; i++) {
            int wlen = strlen(argv[i]);
            if (len + wlen + 2 >= AI_PROMPT_MAX) break;
            if (len > 0) prompt[len++] = ' ';
            memcpy(prompt + len, argv[i], wlen);
            len += wlen;
        }
        prompt[len] = 0;

        char response[AI_RESPONSE_MAX];
        printk("\n[AI chat] sending prompt (%u chars)...\n", (unsigned)len);
        int rc = ai_chat(prompt, response, sizeof(response));
        printk("\n--- AI Response ---\n%s\n", response);
        if (rc != 0) {
            printk("(return code: %d)\n", rc);
        }
    } else if (strcmp(argv[1], "agent") == 0) {
        if (argc < 3) {
            printk("Usage: ai agent <your prompt>\n");
            return;
        }
        char prompt[AI_PROMPT_MAX];
        prompt[0] = 0;
        int len = 0;
        for (int i = 2; i < argc; i++) {
            int wlen = strlen(argv[i]);
            if (len + wlen + 2 >= AI_PROMPT_MAX) break;
            if (len > 0) prompt[len++] = ' ';
            memcpy(prompt + len, argv[i], wlen);
            len += wlen;
        }
        prompt[len] = 0;

        char response[AI_RESPONSE_MAX];
        printk("\n[AI agent] running agentic loop with tools...\n");
        int rc = ai_chat_with_tools(prompt, response, sizeof(response), 5);
        printk("\n--- Agent Output ---\n%s\n", response);
        if (rc != 0) {
            printk("(return code: %d)\n", rc);
        }
    } else if (strcmp(argv[1], "tools") == 0) {
        ai_tools_list();
    } else if (strcmp(argv[1], "providers") == 0) {
        printk("\nSupported AI providers (defaults shown - override with ai setendpoint/setmodel):\n");
        for (int i = 0; i < AI_PROVIDER_COUNT; i++) {
            printk("  %s endpoint: %s\n", ai_provider_name(i), ai_provider_endpoint(i));
            printk("         model:    %s\n", ai_provider_model(i));
        }
        printk("\nTo use a cloud HTTPS API directly, run a local TLS proxy:\n");
        printk("  socat TCP-LISTEN:8443,reuseaddr,fork OPENSSL:open.bigmodel.cn:443\n");
        printk("Then: ai setendpoint glm http://10.0.2.2:8443/api/paas/v4/chat/completions\n");
        printk("\nFor local LLM (Ollama): ai setendpoint glm http://10.0.2.2:11434/v1/chat/completions\n");
    } else {
        printk("Unknown ai subcommand: %s\n", argv[1]);
    }
}

/* ----- network subcommands -------------------------------------------- */
static void cmd_network(int argc, char** argv) {
    (void)argc; (void)argv;
    printk("\nNetwork status:\n");
    printk("  Interface:  %s\n", net_get_iface_name());
    mac_addr_t mac = net_get_mac();
    printk("  MAC:        %x:%x:%x:%x:%x:%x\n",
           mac.bytes[0], mac.bytes[1], mac.bytes[2],
           mac.bytes[3], mac.bytes[4], mac.bytes[5]);
    if (net_is_up()) {
        ipv4_addr_t ip = net_get_ip();
        ipv4_addr_t gw = net_get_gateway();
        ipv4_addr_t dns = net_get_dns();
        printk("  Status:     UP (DHCP complete)\n");
        printk("  IPv4:       %u.%u.%u.%u\n", ip.bytes[0], ip.bytes[1], ip.bytes[2], ip.bytes[3]);
        printk("  Gateway:    %u.%u.%u.%u\n", gw.bytes[0], gw.bytes[1], gw.bytes[2], gw.bytes[3]);
        printk("  DNS:        %u.%u.%u.%u\n", dns.bytes[0], dns.bytes[1], dns.bytes[2], dns.bytes[3]);
    } else {
        printk("  Status:     DOWN (DHCP not yet complete or no NIC)\n");
    }
    if (net_ipv6_is_valid()) {
        ipv6_addr_t ip6 = net_get_ipv6();
        char addr_str[40];
        ipv6_addr_to_str(ip6, addr_str, sizeof(addr_str));
        printk("  IPv6:       %s\n", addr_str);
    } else {
        printk("  IPv6:       not configured\n");
    }
    printk("\n");
}

static void cmd_ifconfig(int argc, char** argv) {
    (void)argc; (void)argv;
    printk("\nNetwork interfaces:\n");
    printk("  %s:\n", net_get_iface_name());
    mac_addr_t mac = net_get_mac();
    printk("    MAC:      %x:%x:%x:%x:%x:%x\n",
           mac.bytes[0], mac.bytes[1], mac.bytes[2],
           mac.bytes[3], mac.bytes[4], mac.bytes[5]);
    if (net_is_up()) {
        ipv4_addr_t ip = net_get_ip();
        ipv4_addr_t mask = net_get_gateway();  /* approximate */
        ipv4_addr_t gw = net_get_gateway();
        ipv4_addr_t dns = net_get_dns();
        printk("    IPv4:     %u.%u.%u.%u\n", ip.bytes[0], ip.bytes[1], ip.bytes[2], ip.bytes[3]);
        printk("    Gateway:  %u.%u.%u.%u\n", gw.bytes[0], gw.bytes[1], gw.bytes[2], gw.bytes[3]);
        printk("    DNS:      %u.%u.%u.%u\n", dns.bytes[0], dns.bytes[1], dns.bytes[2], dns.bytes[3]);
    } else {
        printk("    IPv4:     (DHCP pending)\n");
    }
    if (net_ipv6_is_valid()) {
        ipv6_addr_t ip6 = net_get_ipv6();
        char addr_str[40];
        ipv6_addr_to_str(ip6, addr_str, sizeof(addr_str));
        printk("    IPv6:     %s\n", addr_str);
    } else {
        printk("    IPv6:     (auto-config pending)\n");
    }
    printk("\n");
}

static void cmd_ping(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: ping <host-or-ip>\n");
        printk("Example: ping 10.0.2.2\n");
        return;
    }
    ipv4_addr_t ip;
    /* Try to parse as IP first; if that fails, DNS resolve */
    if (net_resolve(argv[1], &ip)) {
        printk("PING %s (%u.%u.%u.%u)...\n", argv[1],
               ip.bytes[0], ip.bytes[1], ip.bytes[2], ip.bytes[3]);
        uint32_t start = (uint32_t)timer_get_ms();
        int ok = net_ping(ip, 1, 3000);
        uint32_t elapsed = (uint32_t)timer_get_ms() - start;
        if (ok) {
            printk("Reply from %u.%u.%u.%u: time=%u ms\n",
                   ip.bytes[0], ip.bytes[1], ip.bytes[2], ip.bytes[3],
                   (unsigned)elapsed);
        } else {
            printk("Request timed out (waited %u ms)\n", (unsigned)elapsed);
        }
    } else {
        printk("Could not resolve %s\n", argv[1]);
    }
}

static void cmd_ping6(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: ping6 <host-or-ipv6>\n");
        printk("Example: ping6 fe80::1\n");
        printk("         ping6 google.com  (AAAA lookup)\n");
        return;
    }
    if (!net_ipv6_is_valid()) {
        printk("IPv6 not configured yet\n");
        return;
    }
    ipv6_addr_t ip6;
    ipv4_addr_t ip4_unused;
    /* Check if it's an IPv6 literal (contains ':') */
    int has_colon = 0;
    for (const char* p = argv[1]; *p; p++) if (*p == ':') { has_colon = 1; break; }
    if (has_colon) {
        /* Parse literal IPv6 - use the same parser as net_resolve_dual */
        memset(&ip6, 0, sizeof(ip6));
        ipv4_addr_t dummy4;
        net_resolve_dual(argv[1], &dummy4, &ip6);
    } else {
        /* DNS resolve AAAA */
        if (!net_resolve_dual(argv[1], &ip4_unused, &ip6)) {
            printk("Could not resolve %s (no AAAA record)\n", argv[1]);
            return;
        }
    }
    char addr_str[40];
    ipv6_addr_to_str(ip6, addr_str, sizeof(addr_str));
    printk("PING6 %s (%s)...\n", argv[1], addr_str);
    uint32_t start = (uint32_t)timer_get_ms();
    int ok = net_ping6(ip6, 1, 3000);
    uint32_t elapsed = (uint32_t)timer_get_ms() - start;
    if (ok) {
        printk("Reply from %s: time=%u ms\n", addr_str, (unsigned)elapsed);
    } else {
        printk("Request timed out (waited %u ms)\n", (unsigned)elapsed);
    }
}

static void cmd_wget(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: wget <url>\n");
        printk("Example: wget http://example.com/\n");
        return;
    }
    if (!net_is_up()) {
        printk("Network not up - wait for DHCP or check 'network' status\n");
        return;
    }
    printk("Fetching %s...\n", argv[1]);
    struct http_response resp;
    int rc = http_get(argv[1], &resp);
    if (rc != 0) {
        printk("HTTP request failed\n");
        return;
    }
    printk("HTTP %u, %u bytes body\n", (unsigned)resp.status, (unsigned)resp.body_len);
    printk("--- body (first 1024 bytes) ---\n");
    int show = resp.body_len > 1024 ? 1024 : (int)resp.body_len;
    for (int i = 0; i < show; i++) {
        char c = resp.body[i];
        if (c == '\r') continue;
        extern void vga_putchar(char c);
        vga_putchar(c);
    }
    if (resp.body_len > 1024) {
        printk("\n... (%u more bytes truncated)\n",
               (unsigned)(resp.body_len - 1024));
    }
    printk("\n");
}

/* ----- disk/mount commands (AHCI + ext2) ----- */
void cmd_disk(int argc, char** argv) {
    (void)argc; (void)argv;
    extern int ahci_is_present(void);
    extern int ahci_has_drive(void);
    printk("\nDisk status:\n");
    if (ahci_is_present()) {
        printk("  AHCI HBA: present\n");
        if (ahci_has_drive()) {
            printk("  SATA drive: detected\n");
            /* Try reading sector 0 (MBR) */
            static uint8_t buf[512];
            extern int ahci_read_sectors(uint64_t lba, uint32_t count, void* buf);
            int n = ahci_read_sectors(0, 1, buf);
            if (n > 0) {
                printk("  Sector 0 read: OK (%u bytes)\n", (unsigned)(n * 512));
                /* Check for MBR signature */
                if (buf[510] == 0x55 && buf[511] == 0xAA) {
                    printk("  MBR signature: valid\n");
                } else {
                    printk("  MBR signature: not found (raw disk?)\n");
                }
            } else {
                printk("  Sector 0 read: failed\n");
            }
        } else {
            printk("  SATA drive: not detected\n");
            printk("  (attach a disk with: qemu -drive id=disk,file=disk.img,if=none -device ide-hd,drive=disk)\n");
        }
    } else {
        printk("  AHCI HBA: not found\n");
    }
    extern int ext2_is_mounted(void);
    if (ext2_is_mounted()) {
        printk("  ext2 filesystem: mounted\n");
    } else {
        printk("  ext2 filesystem: not mounted (use 'mount')\n");
    }
    printk("\n");
}

void cmd_mount(int argc, char** argv) {
    /* Show mount info or mount a filesystem. */
    if (argc >= 3 && strcmp(argv[1], "ext2") == 0) {
        /* Mount ext2 at the specified target path. */
        int rc = vfs_mount("sda1", argv[2], "ext2");
        if (rc == 0) {
            printk("ext2 filesystem mounted at %s\n", argv[2]);
            /* Show the directory listing. */
            int dirfd = vfs_open(argv[2], O_DIRECTORY);
            if (dirfd < 0) dirfd = vfs_open(argv[2], O_RDONLY);
            if (dirfd >= 0) {
                struct dirent entry;
                entry.inode = 0;
                printk("Contents:\n");
                while (vfs_readdir(dirfd, &entry) == 0) {
                    const char* type_str = (entry.type == FT_DIRECTORY) ? "dir" : "file";
                    printk("  [%s] %s\n", type_str, entry.name);
                }
                vfs_close(dirfd);
            }
        } else {
            printk("Failed to mount ext2 filesystem.\n");
        }
        return;
    }

    /* Default: mount ext2 at root. */
    if (argc >= 2 && strcmp(argv[1], "ext2") == 0) {
        int rc = vfs_mount("sda1", "/", "ext2");
        if (rc == 0) {
            printk("ext2 filesystem mounted at root (/)\n");
            /* Show root directory listing. */
            int dirfd = vfs_open("/", O_DIRECTORY);
            if (dirfd < 0) dirfd = vfs_open("/", O_RDONLY);
            if (dirfd >= 0) {
                struct dirent entry;
                entry.inode = 0;
                printk("Root contents:\n");
                while (vfs_readdir(dirfd, &entry) == 0) {
                    const char* type_str = (entry.type == FT_DIRECTORY) ? "dir" : "file";
                    printk("  [%s] %s\n", type_str, entry.name);
                }
                vfs_close(dirfd);
            }
        } else {
            printk("ext2 filesystem already mounted or mount failed.\n");
        }
        return;
    }

    /* No arguments: show current mount status. */
    if (argc == 1) {
        int mount_count = vfs_get_mount_count();
        if (mount_count == 0) {
            printk("No filesystems mounted (use 'mount ext2' to mount)\n");
            return;
        }
        printk("Mounted filesystems:\n");
        for (int i = 0; i < mount_count; i++) {
            struct mount* m = vfs_get_mount(i);
            if (m) {
                const char* type_str = (m->fs_type == FS_TYPE_EXT2) ? "ext2" : "memfs";
                printk("  %s at %s (%s)\n", type_str, m->path, type_str);
            }
        }
    }
}

/* ----- file subcommands ----------------------------------------------- */
static void cmd_file(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: file <ls|cat|write|mkdir|rmdir|rm|chmod|stat> [args]\n");
        return;
    }
    if (strcmp(argv[1], "ls") == 0) {
        /* List files in a directory via vfs_readdir.
         * If a path argument is given, list that directory;
         * otherwise list the root directory. */
        const char* dir_path = "/";
        if (argc >= 3) dir_path = argv[2];

        /* Open the directory for reading. */
        int dirfd = vfs_open(dir_path, O_DIRECTORY);
        if (dirfd < 0) {
            /* vfs_open on a directory may not work with O_DIRECTORY
             * for memfs directories (they just return the idx+3 fd).
             * Try without O_DIRECTORY as fallback. */
            dirfd = vfs_open(dir_path, O_RDONLY);
        }
        if (dirfd < 0) {
            printk("file: %s: not found or not a directory\n", dir_path);
            return;
        }

        printk("\nContents of %s:\n", dir_path);
        struct dirent entry;
        entry.inode = 0;
        int count = 0;
        while (vfs_readdir(dirfd, &entry) == 0) {
            const char* type_str = (entry.type == FT_DIRECTORY) ? "dir" : "file";
            printk("  [%s] %s\n", type_str, entry.name);
            count++;
        }
        if (count == 0) {
            printk("  (empty directory)\n");
        }
        printk("(%d entry%s total)\n", count, count == 1 ? "" : "s");
        vfs_close(dirfd);
    } else if (strcmp(argv[1], "cat") == 0) {
        if (argc < 3) { printk("Usage: file cat <path>\n"); return; }
        int fd = vfs_open(argv[2], 0);
        if (fd < 0) {
            printk("file: %s: not found\n", argv[2]);
            return;
        }
        char buf[512];
        ssize_t n = vfs_read(fd, buf, sizeof(buf) - 1);
        vfs_close(fd);
        if (n > 0) {
            buf[n] = 0;
            printk("%s\n", buf);
        } else {
            printk("(empty file)\n");
        }
    } else if (strcmp(argv[1], "write") == 0) {
        if (argc < 4) {
            printk("Usage: file write <path> <text...>\n");
            return;
        }
        int fd = vfs_open(argv[2], 0x10);  /* O_CREAT */
        if (fd < 0) {
            printk("file: cannot create %s\n", argv[2]);
            return;
        }
        /* Reconstruct text from argv[3..] */
        char buf[512];
        int len = 0;
        for (int i = 3; i < argc; i++) {
            int wlen = strlen(argv[i]);
            if (len + wlen + 2 >= (int)sizeof(buf)) break;
            if (len > 0) buf[len++] = ' ';
            memcpy(buf + len, argv[i], wlen);
            len += wlen;
        }
        buf[len] = 0;
        ssize_t n = vfs_write(fd, buf, len);
        vfs_close(fd);
        printk("Wrote %d bytes to %s\n", (int)n, argv[2]);
    } else if (strcmp(argv[1], "mkdir") == 0) {
        if (argc < 3) { printk("Usage: file mkdir <path>\n"); return; }
        int rc = vfs_mkdir(argv[2], 0755);
        if (rc < 0) {
            printk("file: mkdir %s: failed (path exists or parent missing)\n", argv[2]);
        } else {
            printk("file: created directory %s\n", argv[2]);
        }
    } else if (strcmp(argv[1], "rmdir") == 0) {
        if (argc < 3) { printk("Usage: file rmdir <path>\n"); return; }
        int rc = vfs_rmdir(argv[2]);
        if (rc < 0) {
            printk("file: rmdir %s: failed (not a dir, not empty, or not found)\n", argv[2]);
        } else {
            printk("file: removed directory %s\n", argv[2]);
        }
    } else if (strcmp(argv[1], "rm") == 0) {
        if (argc < 3) { printk("Usage: file rm <path>\n"); return; }
        int rc = vfs_unlink(argv[2]);
        if (rc < 0) {
            printk("file: rm %s: failed\n", argv[2]);
        } else {
            printk("file: removed %s\n", argv[2]);
        }
    } else if (strcmp(argv[1], "chmod") == 0) {
        if (argc < 4) { printk("Usage: file chmod <path> <mode>\n"); return; }
        /* Parse octal mode string (e.g. "755", "644"). */
        uint32_t mode = 0;
        const char* s = argv[3];
        while (*s >= '0' && *s <= '7') {
            mode = mode * 8 + (uint32_t)(*s - '0');
            s++;
        }
        if (*s != '\0' || argv[3][0] == '\0') {
            printk("file: chmod: invalid mode '%s' (use octal, e.g. 755)\n", argv[3]);
            return;
        }
        int rc = vfs_chmod(argv[2], mode);
        if (rc < 0) {
            printk("file: chmod %s: failed (not found)\n", argv[2]);
        } else {
            printk("file: chmod %s: mode set to 0%o\n", argv[2], mode);
        }
    } else if (strcmp(argv[1], "stat") == 0) {
        if (argc < 3) { printk("Usage: file stat <path>\n"); return; }
        struct stat st;
        int rc = vfs_stat(argv[2], &st);
        if (rc < 0) {
            printk("file: stat %s: not found\n", argv[2]);
        } else {
            const char* type_str = S_ISDIR(st.mode) ? "directory" : "regular file";
            printk("file: %s: %s, size=%u, mode=0%o\n",
                   argv[2], type_str, (unsigned)st.size, st.mode);
        }
    } else {
        printk("Unknown file subcommand: %s\n", argv[1]);
    }
}

/* ----- lee subcommands ------------------------------------------------- */
static void cmd_lee(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: lee <strt|stop|status|sandbox|service|net|help>\n");
        printk("  lee strt server [port] [--tls]  Start sandbox HTTP(S) server\n");
        printk("  lee strt ssh [port]       Start SSH-like remote shell (default 2222)\n");
        printk("  lee strt sandbox [id] [size_mb]  Start sandbox (id=1|2, default 16MB storage)\n");
        printk("  lee stop server           Stop the HTTP server\n");
        printk("  lee stop ssh              Stop the SSH server\n");
        printk("  lee stop sandbox [id]     Stop a specific sandbox\n");
        printk("  lee status                Show all running services\n");
        printk("  lee service list          List all services\n");
        printk("  lee service start <name>  Start a named service\n");
        printk("  lee service stop <name>   Stop a named service\n");
        printk("  lee net config <ip> <mask> <gw>  Set static IP\n");
        printk("  lee net dns <dns1> [dns2] Set DNS servers\n");
        printk("  lee net dhcp              Switch to DHCP mode\n");
        printk("  lee sandbox list          List active sandboxes\n");
        printk("  lee help                  Show this help\n");
        return;
    }

    if (strcmp(argv[1], "help") == 0) {
        cmd_lee(1, NULL);
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        printk("\nLestraOS Services:\n");
        /* Use the service manager for all services */
        struct service_info list[SERVICE_MAX];
        int count = service_list(list, SERVICE_MAX);
        printk("  %-18s  %-10s  %s\n", "SERVICE", "STATE", "DESCRIPTION");
        printk("  %-18s  %-10s  %s\n", "-------", "-----", "-----------");
        for (int i = 0; i < count; i++) {
            const char* state_str;
            switch (list[i].state) {
                case SVC_STOPPED:  state_str = "stopped";  break;
                case SVC_STARTING: state_str = "starting"; break;
                case SVC_RUNNING:  state_str = "running";  break;
                case SVC_FAILED:   state_str = "failed";   break;
                default:           state_str = "unknown";   break;
            }
            printk("  %-18s  %-10s  %s\n",
                   list[i].name, state_str, list[i].description);
        }
        /* Sandboxes */
        int sb_count = sandbox_count();
        printk("\n  Sandboxes: %d/%d\n", sb_count, SANDBOX_MAX);
        for (int i = 1; i <= SANDBOX_MAX; i++) {
            struct sandbox_info info;
            if (sandbox_status(i, &info) == 0) {
                printk("    [%d] %-16s  %s\n",
                       info.id, info.name,
                       info.active ? "RUNNING" : "STOPPED");
            }
        }
        printk("\n");
        return;
    }

    /* ----- lee service ... ----- */
    if (strcmp(argv[1], "service") == 0) {
        if (argc < 3) {
            printk("Usage: lee service <list|start|stop> [name]\n");
            return;
        }
        if (strcmp(argv[2], "list") == 0) {
            struct service_info list[SERVICE_MAX];
            int count = service_list(list, SERVICE_MAX);
            printk("\n  %-18s  %-10s  %s\n", "SERVICE", "STATE", "DESCRIPTION");
            printk("  %-18s  %-10s  %s\n", "-------", "-----", "-----------");
            for (int i = 0; i < count; i++) {
                const char* state_str;
                switch (list[i].state) {
                    case SVC_STOPPED:  state_str = "stopped";  break;
                    case SVC_STARTING: state_str = "starting"; break;
                    case SVC_RUNNING:  state_str = "running";  break;
                    case SVC_FAILED:   state_str = "failed";   break;
                    default:           state_str = "unknown";   break;
                }
                printk("  %-18s  %-10s  %s\n",
                       list[i].name, state_str, list[i].description);
            }
            printk("\n");
        } else if (strcmp(argv[2], "start") == 0) {
            if (argc < 4) {
                printk("Usage: lee service start <name>\n");
                return;
            }
            service_start(argv[3]);
        } else if (strcmp(argv[2], "stop") == 0) {
            if (argc < 4) {
                printk("Usage: lee service stop <name>\n");
                return;
            }
            service_stop(argv[3]);
        } else {
            printk("Unknown service subcommand: %s\n", argv[2]);
        }
        return;
    }

    /* ----- lee net config ... ----- */
    if (strcmp(argv[1], "net") == 0) {
        if (argc < 3) {
            printk("Usage: lee net <config|dns|dhcp>\n");
            printk("  lee net config <ip> <mask> <gw>  Set static IP\n");
            printk("  lee net dns <dns1> [dns2]        Set DNS servers\n");
            printk("  lee net dhcp                     Switch to DHCP mode\n");
            return;
        }
        if (strcmp(argv[2], "config") == 0) {
            if (argc < 6) {
                printk("Usage: lee net config <ip> <mask> <gw>\n");
                printk("Example: lee net config 192.168.1.100 255.255.255.0 192.168.1.1\n");
                return;
            }
            extern int net_config_set_ip(const char*, const char*, const char*);
            if (net_config_set_ip(argv[3], argv[4], argv[5]) == 0) {
                printk("Static IP configured (reboot or reinit net to apply)\n");
            }
        } else if (strcmp(argv[2], "dns") == 0) {
            if (argc < 4) {
                printk("Usage: lee net dns <dns1> [dns2]\n");
                printk("Example: lee net dns 8.8.8.8 8.8.4.4\n");
                return;
            }
            extern int net_config_set_dns(const char*, const char*);
            const char* d2 = (argc >= 5) ? argv[4] : "";
            if (net_config_set_dns(argv[3], d2) == 0) {
                printk("DNS configured\n");
            }
        } else if (strcmp(argv[2], "dhcp") == 0) {
            extern void net_config_dhcp(void);
            net_config_dhcp();
            printk("Switched to DHCP mode\n");
        } else {
            printk("Unknown net subcommand: %s\n", argv[2]);
        }
        return;
    }

    if (strcmp(argv[1], "strt") == 0) {
        if (argc < 3) {
            printk("Usage: lee strt <server|ssh|sandbox> [args]\n");
            return;
        }
        if (strcmp(argv[2], "server") == 0) {
            int port = 0;
            int tls = 0;
            for (int i = 3; i < argc; i++) {
                if (strcmp(argv[i], "--tls") == 0) {
                    tls = 1;
                } else {
                    port = 0;
                    for (char* p = argv[i]; *p >= '0' && *p <= '9'; p++)
                        port = port * 10 + (*p - '0');
                    if (port <= 0 || port > 65535) port = 0;
                }
            }
            if (port <= 0) port = tls ? 8443 : 8080;
            if (sandbox_server_is_running()) {
                printk("HTTP server already running on port %d\n",
                       sandbox_server_port());
            } else {
                if (tls) {
                    sandbox_server_start_tls(port);
                    printk("HTTPS sandbox server started on port %d (TLS)\n", port);
                } else {
                    sandbox_server_start(port);
                    printk("HTTP sandbox server started on port %d\n", port);
                }
            }
        } else if (strcmp(argv[2], "ssh") == 0) {
            service_start("ssh");
        } else if (strcmp(argv[2], "sandbox") == 0) {
            int id = 0;
            int size_mb = 16; /* default 16MB */
            if (argc >= 4) {
                id = 0;
                for (char* p = argv[3]; *p >= '0' && *p <= '9'; p++)
                    id = id * 10 + (*p - '0');
            }
            if (argc >= 5) {
                size_mb = 0;
                for (char* p = argv[4]; *p >= '0' && *p <= '9'; p++)
                    size_mb = size_mb * 10 + (*p - '0');
                if (size_mb < 1) size_mb = 1;
                if (size_mb > 256) size_mb = 256;
            }
            if (id == 0) {
                uint64_t storage = (uint64_t)size_mb * 1024 * 1024;
                id = sandbox_create(NULL, 0, storage);
                if (id > 0) {
                    printk("Created sandbox %d (%dMB storage)\n", id, size_mb);
                } else {
                    printk("Failed to create sandbox (max %d reached)\n",
                           SANDBOX_MAX);
                }
            } else {
                if (id < 1 || id > SANDBOX_MAX) {
                    printk("Invalid sandbox id: %d (max %d)\n",
                           id, SANDBOX_MAX);
                } else {
                    struct sandbox_info info;
                    if (sandbox_status(id, &info) != 0 || !info.active) {
                        printk("Sandbox %d is not running\n", id);
                    } else {
                        printk("Sandbox %d already active (pid %d)\n",
                               id, info.pid);
                    }
                }
            }
        } else {
            printk("Unknown strt target: %s\n", argv[2]);
        }
        return;
    }

    if (strcmp(argv[1], "stop") == 0) {
        if (argc < 3) {
            printk("Usage: lee stop <server|ssh|sandbox> [args]\n");
            return;
        }
        if (strcmp(argv[2], "server") == 0) {
            if (!sandbox_server_is_running()) {
                printk("HTTP server not running\n");
            } else {
                sandbox_server_stop();
                printk("HTTP server stopped\n");
            }
        } else if (strcmp(argv[2], "ssh") == 0) {
            service_stop("ssh");
        } else if (strcmp(argv[2], "sandbox") == 0) {
            if (argc < 4) {
                printk("Usage: lee stop sandbox <id>\n");
                return;
            }
            int id = 0;
            for (char* p = argv[3]; *p >= '0' && *p <= '9'; p++)
                id = id * 10 + (*p - '0');
            if (id < 1 || id > SANDBOX_MAX) {
                printk("Invalid sandbox id: %d\n", id);
                return;
            }
            if (sandbox_stop(id) == 0) {
                printk("Sandbox %d stopped\n", id);
            } else {
                printk("Failed to stop sandbox %d\n", id);
            }
        } else {
            printk("Unknown stop target: %s\n", argv[2]);
        }
        return;
    }

    if (strcmp(argv[1], "sandbox") == 0) {
        if (argc >= 3 && strcmp(argv[2], "list") == 0) {
            sandbox_list();
            return;
        }
        printk("Usage: lee sandbox list\n");
        return;
    }

    printk("Unknown lee subcommand: %s\n", argv[1]);
    printk("Type 'lee help' for available commands.\n");
}

/* ----- firewall subcommands ------------------------------------------- */
static void cmd_firewall(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: firewall <add|remove|list|flush|status|default> [args]\n");
        printk("  firewall add <name> <accept|drop|reject> [options]\n");
        printk("    Options:\n");
        printk("      proto <tcp|udp|icmp|any>    Protocol to match\n");
        printk("      src <ip>                    Source IP address\n");
        printk("      dst <ip>                    Destination IP address\n");
        printk("      srcmask <mask>              Source mask (default 255.255.255.255)\n");
        printk("      dstmask <mask>              Destination mask (default 255.255.255.255)\n");
        printk("      sport <port>                Source port (0 = any)\n");
        printk("      dport <port>                Destination port (0 = any)\n");
        printk("      dir <in|out|both>           Direction (default both)\n");
        printk("      log                         Log matches\n");
        printk("  firewall remove <name>         Remove a rule by name\n");
        printk("  firewall remove <id>           Remove a rule by numeric ID\n");
        printk("  firewall list                  List all rules\n");
        printk("  firewall flush                 Remove all rules\n");
        printk("  firewall status                Show stats and default policies\n");
        printk("  firewall default <in|out> <accept|drop|reject>\n");
        printk("\nExamples:\n");
        printk("  firewall add block-telnet drop proto tcp dport 23 dir both\n");
        printk("  firewall add allow-http accept proto tcp dport 80 dir in\n");
        printk("  firewall default in drop\n");
        return;
    }

    if (strcmp(argv[1], "list") == 0) {
        fw_list_rules();
    } else if (strcmp(argv[1], "flush") == 0) {
        fw_flush();
    } else if (strcmp(argv[1], "status") == 0) {
        fw_status();
    } else if (strcmp(argv[1], "remove") == 0) {
        if (argc < 3) {
            printk("Usage: firewall remove <name|id>\n");
            return;
        }
        /* Try numeric ID first */
        int is_num = 1;
        for (char* p = argv[2]; *p; p++) {
            if (*p < '0' || *p > '9') { is_num = 0; break; }
        }
        if (is_num && argv[2][0]) {
            int id = 0;
            for (char* p = argv[2]; *p >= '0' && *p <= '9'; p++)
                id = id * 10 + (*p - '0');
            if (fw_remove_rule_by_id(id) == 0) {
                printk("Removed rule %d\n", id);
            } else {
                printk("Rule %d not found\n", id);
            }
        } else {
            if (fw_remove_rule(argv[2]) == 0) {
                printk("Removed rule: %s\n", argv[2]);
            } else {
                printk("Rule not found: %s\n", argv[2]);
            }
        }
    } else if (strcmp(argv[1], "default") == 0) {
        if (argc < 4) {
            printk("Usage: firewall default <in|out> <accept|drop|reject>\n");
            return;
        }
        int dir = -1;
        if (strcmp(argv[2], "in") == 0) dir = 0;
        else if (strcmp(argv[2], "out") == 0) dir = 1;
        if (dir < 0) {
            printk("Direction must be 'in' or 'out'\n");
            return;
        }
        enum fw_action act;
        if (strcmp(argv[3], "accept") == 0) act = FW_ACCEPT;
        else if (strcmp(argv[3], "drop") == 0) act = FW_DROP;
        else if (strcmp(argv[3], "reject") == 0) act = FW_REJECT;
        else {
            printk("Action must be 'accept', 'drop', or 'reject'\n");
            return;
        }
        fw_set_default(dir, act);
        printk("Default %s policy set to %s\n", argv[2], argv[3]);
    } else if (strcmp(argv[1], "add") == 0) {
        if (argc < 4) {
            printk("Usage: firewall add <name> <accept|drop|reject> [options]\n");
            return;
        }
        const char* name = argv[2];
        enum fw_action action;
        if (strcmp(argv[3], "accept") == 0) action = FW_ACCEPT;
        else if (strcmp(argv[3], "drop") == 0) action = FW_DROP;
        else if (strcmp(argv[3], "reject") == 0) action = FW_REJECT;
        else {
            printk("Action must be 'accept', 'drop', or 'reject'\n");
            return;
        }

        /* Parse optional arguments */
        enum fw_proto proto = FW_PROTO_ANY;
        ipv4_addr_t src_ip = IP_ZERO, dst_ip = IP_ZERO;
        ipv4_addr_t src_mask = ipv4(255,255,255,255);
        ipv4_addr_t dst_mask = ipv4(255,255,255,255);
        uint16_t src_port = 0, dst_port = 0;
        int direction = 2; /* both */
        int logged = 0;

        int i = 4;
        while (i < argc) {
            if (strcmp(argv[i], "proto") == 0 && i + 1 < argc) {
                i++;
                if (strcmp(argv[i], "tcp") == 0) proto = FW_PROTO_TCP;
                else if (strcmp(argv[i], "udp") == 0) proto = FW_PROTO_UDP;
                else if (strcmp(argv[i], "icmp") == 0) proto = FW_PROTO_ICMP;
                else if (strcmp(argv[i], "any") == 0) proto = FW_PROTO_ANY;
                else printk("Unknown protocol: %s (use tcp|udp|icmp|any)\n", argv[i]);
            } else if (strcmp(argv[i], "src") == 0 && i + 1 < argc) {
                i++;
                /* Parse IP: split on '/' for optional mask */
                char* slash = NULL;
                for (char* p = argv[i]; *p; p++) {
                    if (*p == '/') { *p = '\0'; slash = p + 1; break; }
                }
                /* Parse dotted quad */
                int vals[4] = {0,0,0,0}, vi = 0;
                const char* s = argv[i];
                while (*s && vi < 4) {
                    if (*s >= '0' && *s <= '9') vals[vi] = vals[vi]*10 + (*s-'0');
                    else if (*s == '.') vi++;
                    s++;
                }
                src_ip = ipv4(vals[0], vals[1], vals[2], vals[3]);
                if (slash) {
                    vals[0] = vals[1] = vals[2] = vals[3] = 0;
                    vi = 0; s = slash;
                    while (*s && vi < 4) {
                        if (*s >= '0' && *s <= '9') vals[vi] = vals[vi]*10 + (*s-'0');
                        else if (*s == '.') vi++;
                        s++;
                    }
                    src_mask = ipv4(vals[0], vals[1], vals[2], vals[3]);
                }
            } else if (strcmp(argv[i], "dst") == 0 && i + 1 < argc) {
                i++;
                char* slash = NULL;
                for (char* p = argv[i]; *p; p++) {
                    if (*p == '/') { *p = '\0'; slash = p + 1; break; }
                }
                int vals[4] = {0,0,0,0}, vi = 0;
                const char* s = argv[i];
                while (*s && vi < 4) {
                    if (*s >= '0' && *s <= '9') vals[vi] = vals[vi]*10 + (*s-'0');
                    else if (*s == '.') vi++;
                    s++;
                }
                dst_ip = ipv4(vals[0], vals[1], vals[2], vals[3]);
                if (slash) {
                    vals[0] = vals[1] = vals[2] = vals[3] = 0;
                    vi = 0; s = slash;
                    while (*s && vi < 4) {
                        if (*s >= '0' && *s <= '9') vals[vi] = vals[vi]*10 + (*s-'0');
                        else if (*s == '.') vi++;
                        s++;
                    }
                    dst_mask = ipv4(vals[0], vals[1], vals[2], vals[3]);
                }
            } else if (strcmp(argv[i], "srcmask") == 0 && i + 1 < argc) {
                i++;
                int vals[4] = {0,0,0,0}, vi = 0;
                const char* s = argv[i];
                while (*s && vi < 4) {
                    if (*s >= '0' && *s <= '9') vals[vi] = vals[vi]*10 + (*s-'0');
                    else if (*s == '.') vi++;
                    s++;
                }
                src_mask = ipv4(vals[0], vals[1], vals[2], vals[3]);
            } else if (strcmp(argv[i], "dstmask") == 0 && i + 1 < argc) {
                i++;
                int vals[4] = {0,0,0,0}, vi = 0;
                const char* s = argv[i];
                while (*s && vi < 4) {
                    if (*s >= '0' && *s <= '9') vals[vi] = vals[vi]*10 + (*s-'0');
                    else if (*s == '.') vi++;
                    s++;
                }
                dst_mask = ipv4(vals[0], vals[1], vals[2], vals[3]);
            } else if (strcmp(argv[i], "sport") == 0 && i + 1 < argc) {
                i++;
                src_port = 0;
                for (char* p = argv[i]; *p >= '0' && *p <= '9'; p++)
                    src_port = src_port * 10 + (*p - '0');
            } else if (strcmp(argv[i], "dport") == 0 && i + 1 < argc) {
                i++;
                dst_port = 0;
                for (char* p = argv[i]; *p >= '0' && *p <= '9'; p++)
                    dst_port = dst_port * 10 + (*p - '0');
            } else if (strcmp(argv[i], "dir") == 0 && i + 1 < argc) {
                i++;
                if (strcmp(argv[i], "in") == 0) direction = 0;
                else if (strcmp(argv[i], "out") == 0) direction = 1;
                else if (strcmp(argv[i], "both") == 0) direction = 2;
                else printk("Unknown direction: %s (use in|out|both)\n", argv[i]);
            } else if (strcmp(argv[i], "log") == 0) {
                logged = 1;
            } else {
                printk("Unknown option: %s\n", argv[i]);
            }
            i++;
        }

        int id = fw_add_rule(name, action, proto,
                             src_ip, src_mask, dst_ip, dst_mask,
                             src_port, dst_port, direction, logged);
        if (id >= 0) {
            printk("Added rule %d: %s %s\n", id, name,
                   action == FW_ACCEPT ? "ACCEPT" : action == FW_DROP ? "DROP" : "REJECT");
        } else {
            printk("Failed to add rule (table full, max %d)\n", FW_MAX_RULES);
        }
    } else {
        printk("Unknown firewall subcommand: %s\n", argv[1]);
    }
}

/* ----- command dispatch ----------------------------------------------- */
static void execute_command(void) {
    if (argc == 0) return;

    char* cmd = argv[0];

    if (strcmp(cmd, "help") == 0) cmd_help();
    else if (strcmp(cmd, "echo") == 0) cmd_echo();
    else if (strcmp(cmd, "cd") == 0) {
        if (argc < 2) {
            /* No arg: go to root */
            strcpy(cwd, "/");
        } else {
            if (argv[1][0] == '/') {
                strncpy(cwd, argv[1], sizeof(cwd) - 1);
            } else {
                strncat(cwd, "/", sizeof(cwd) - strlen(cwd) - 1);
                strncat(cwd, argv[1], sizeof(cwd) - strlen(cwd) - 1);
            }
            cwd[sizeof(cwd) - 1] = '\0';
        }
    }
    else if (strcmp(cmd, "clear") == 0) cmd_clear();
    else if (strcmp(cmd, "uname") == 0) cmd_uname();
    else if (strcmp(cmd, "free") == 0) cmd_free();
    else if (strcmp(cmd, "reboot") == 0) cmd_reboot();
    else if (strcmp(cmd, "shutdown") == 0) cmd_shutdown();
    else if (strcmp(cmd, "uptime") == 0) cmd_uptime();
    else if (strcmp(cmd, "version") == 0) cmd_version();
    else if (strcmp(cmd, "ps") == 0) cmd_ps();
    else if (strcmp(cmd, "cpuinfo") == 0) cmd_cpuinfo();
    else if (strcmp(cmd, "meminfo") == 0) cmd_meminfo();
    else if (strcmp(cmd, "test") == 0) cmd_test();
    else if (strcmp(cmd, "neofetch") == 0) cmd_neofetch();
    else if (strcmp(cmd, "install") == 0) cmd_install();
    else if (strcmp(cmd, "ui") == 0) {
        ui_boot_splash();
        ui_menu_loop();
    }
    else if (strcmp(cmd, "theme") == 0) cmd_theme(argc, argv);
    else if (strcmp(cmd, "pkg") == 0) cmd_pkg(argc, argv);
    else if (strcmp(cmd, "ai") == 0) cmd_ai(argc, argv);
    else if (strcmp(cmd, "file") == 0) cmd_file(argc, argv);
    else if (strcmp(cmd, "network") == 0) cmd_network(argc, argv);
    else if (strcmp(cmd, "ifconfig") == 0) cmd_ifconfig(argc, argv);
    else if (strcmp(cmd, "ping") == 0) cmd_ping(argc, argv);
    else if (strcmp(cmd, "ping6") == 0) cmd_ping6(argc, argv);
    else if (strcmp(cmd, "wget") == 0) cmd_wget(argc, argv);
    else if (strcmp(cmd, "claude") == 0) cmd_claude(argc, argv);
    else if (strcmp(cmd, "glm") == 0) cmd_glm(argc, argv);
    else if (strcmp(cmd, "gemini") == 0) cmd_gemini(argc, argv);
    else if (strcmp(cmd, "openai") == 0) cmd_openai(argc, argv);
    else if (strcmp(cmd, "uai") == 0) cmd_uai(argc, argv);
    else if (strcmp(cmd, "disk") == 0) cmd_disk(argc, argv);
    else if (strcmp(cmd, "mount") == 0) cmd_mount(argc, argv);
    else if (strcmp(cmd, "exec") == 0) {
        if (argc < 2) {
            printk("Usage: exec <elf-path>\n");
            printk("Example: exec /hello.elf\n");
            printk("         exec /opt/libreoffice/program/soffice.bin\n");
            return;
        }
        extern int elf_exec(const char* path);
        extern int ldso_is_dynamic(const char* path);
        extern int ldso_load_and_run(const char* exe_path, int argc,
                                      char** argv, char** envp);
        printk("Executing %s...\n", argv[1]);
        if (ldso_is_dynamic(argv[1])) {
            printk("  (dynamic ELF — using ldso dynamic linker)\n");
            int rc = ldso_load_and_run(argv[1], argc - 1, &argv[1], NULL);
            printk("ldso returned: %d\n", rc);
        } else {
            printk("  (static ELF — using elf_exec)\n");
            int rc = elf_exec(argv[1]);
            printk("exec returned: %d\n", rc);
        }
    }
    else if (strcmp(cmd, "save") == 0) {
        if (argc < 3) {
            printk("Usage: save <path> <text...>\n");
            printk("Saves text to a file on the ext2 disk.\n");
            printk("Example: save /test.txt Hello World\n");
            return;
        }
        extern int ext2_write_file(const char* path, const void* buf, uint32_t len);
        extern int ext2_is_mounted(void);
        if (!ext2_is_mounted()) {
            printk("ext2 not mounted. Run 'mount' first.\n");
            return;
        }
        /* Reconstruct text from argv[2..] */
        char buf[512];
        int len = 0;
        for (int i = 2; i < argc && len < (int)sizeof(buf) - 2; i++) {
            if (len > 0) buf[len++] = ' ';
            int wlen = strlen(argv[i]);
            if (len + wlen >= (int)sizeof(buf) - 1) break;
            memcpy(buf + len, argv[i], wlen);
            len += wlen;
        }
        buf[len] = '\n';
        len++;
        int written = ext2_write_file(argv[1], buf, len);
        printk("Saved %d bytes to %s\n", written, argv[1]);
    }
    else if (strcmp(cmd, "play") == 0) {
        extern int ac97_is_present(void);
        if (!ac97_is_present()) {
            printk("No AC97 audio controller found.\n");
            return;
        }
        printk("Playing test tone (440 Hz, 1 second)...\n");
        extern int ac97_play(const void* buf, uint32_t len);
        static int16_t tone[44100 * 2];
        for (int i = 0; i < 44100; i++) {
            int phase = (i * 440 * 256) / 44100;
            extern int isin(uint32_t phase_milli);
            int val = (isin(phase * 1000 / 256) * 16000) / 1000;
            tone[i * 2] = val;
            tone[i * 2 + 1] = val;
        }
        ac97_play(tone, sizeof(tone));
        printk("Done.\n");
    }
    else if (strcmp(cmd, "speak") == 0) {
        if (argc < 2) {
            printk("Usage: speak <text to say>\n");
            printk("Example: speak Hello world\n");
            printk("The OS will speak the text out loud through the audio driver.\n");
            return;
        }
        /* Reconstruct text from argv */
        char text[512];
        int len = 0;
        for (int i = 1; i < argc && len < (int)sizeof(text) - 2; i++) {
            if (len > 0) text[len++] = ' ';
            int wlen = strlen(argv[i]);
            if (len + wlen >= (int)sizeof(text) - 1) break;
            memcpy(text + len, argv[i], wlen);
            len += wlen;
        }
        text[len] = '\0';
        printk("Speaking: \"%s\"\n", text);
        extern int tts_speak(const char* text);
        int rc = tts_speak(text);
        if (rc == 0) printk("Done.\n");
        else printk("TTS failed (no audio device?)\n");
    }
    else if (strcmp(cmd, "date") == 0 || strcmp(cmd, "time") == 0) {
        uint8_t h, m, s;
        uint16_t year;
        uint8_t month, day;
        rtc_get_time(&h, &m, &s);
        rtc_get_date(&year, &month, &day);
        const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
        const char* mname = (month >= 1 && month <= 12) ? months[month-1] : "???";
        printk("%s %u %u  %02u:%02u:%02u UTC\n",
               mname, (unsigned)day, (unsigned)year,
               (unsigned)h, (unsigned)m, (unsigned)s);
    }
    else if (strcmp(cmd, "battery") == 0) {
        int pct = battery_get_percent();
        const char* status = battery_get_status_str();
        printk("Battery: %d%% (%s)\n", pct, status);
        if (battery_is_charging()) printk("  Status: charging\n");
    }
    else if (strcmp(cmd, "temp") == 0) {
        int cpu = temp_get_cpu();
        int gpu = temp_get_gpu();
        printk("CPU temperature: %d°C\n", cpu);
        printk("GPU temperature: %d°C\n", gpu);
    }
    else if (strcmp(cmd, "wifi") == 0) {
        if (argc < 2) {
            printk("Usage: wifi <scan|connect|disconnect|status>\n");
            printk("  wifi scan              - scan for networks\n");
            printk("  wifi connect <ssid>    - connect to a network\n");
            printk("  wifi disconnect        - disconnect\n");
            printk("  wifi status            - show connection status\n");
            return;
        }
        if (strcmp(argv[1], "scan") == 0) {
            int n = wifi_scan();
            printk("Found %d networks:\n", n);
            for (int i = 0; i < n; i++) {
                printk("  %-20s signal: %d%%\n",
                       wifi_get_ssid(i), wifi_get_signal(i));
            }
        } else if (strcmp(argv[1], "connect") == 0) {
            if (argc < 3) {
                printk("Usage: wifi connect <ssid> [password]\n");
                return;
            }
            const char* pass = (argc >= 4) ? argv[3] : "";
            if (wifi_connect(argv[2], pass)) {
                printk("Connected to %s\n", argv[2]);
            } else {
                printk("Failed to connect to %s\n", argv[2]);
            }
        } else if (strcmp(argv[1], "disconnect") == 0) {
            wifi_disconnect();
            printk("Disconnected.\n");
        } else if (strcmp(argv[1], "status") == 0) {
            if (wifi_is_connected()) {
                printk("Connected to: %s\n", wifi_get_connected_ssid());
            } else {
                printk("Not connected.\n");
            }
        } else {
            printk("Unknown wifi subcommand: %s\n", argv[1]);
        }
    }
    else if (strcmp(cmd, "cron") == 0) {
        if (argc < 2) {
            printk("Usage: cron <list|add|remove|count>\n");
            printk("  cron list                       - show all tasks\n");
            printk("  cron add \"* * * * *\" <cmd>     - add a task\n");
            printk("  cron remove <id>                - remove a task\n");
            printk("  cron count                      - show task count\n");
            printk("Schedule format: minute hour day month weekday (* = wildcard)\n");
            printk("Example: cron add \"*/5 * * * *\" neofetch\n");
            return;
        }
        if (strcmp(argv[1], "list") == 0) {
            cron_list();
        } else if (strcmp(argv[1], "add") == 0) {
            if (argc < 4) {
                printk("Usage: cron add \"<schedule>\" <command>\n");
                return;
            }
            int id = cron_add(argv[2], argv[3]);
            if (id >= 0) {
                printk("Added cron task %d: %s -> %s\n", id, argv[2], argv[3]);
            } else {
                printk("Failed to add cron task (table full?)\n");
            }
        } else if (strcmp(argv[1], "remove") == 0) {
            if (argc < 3) {
                printk("Usage: cron remove <id>\n");
                return;
            }
            int id = 0; for (char* p = argv[2]; *p >= '0' && *p <= '9'; p++) id = id * 10 + (*p - '0');
            if (cron_remove(id)) {
                printk("Removed cron task %d\n", id);
            } else {
                printk("Task %d not found\n", id);
            }
        } else if (strcmp(argv[1], "count") == 0) {
            printk("%d cron tasks scheduled\n", cron_count());
        } else {
            printk("Unknown cron subcommand: %s\n", argv[1]);
        }
    }
    else if (strcmp(cmd, "lee") == 0) cmd_lee(argc, argv);
    else if (strcmp(cmd, "firewall") == 0) cmd_firewall(argc, argv);
    else if (strcmp(cmd, "sysinfo") == 0) {
        printk("\n=== System Information ===\n");
        printk("OS:        Lestra OS 1.0.0-alpha\n");
        printk("Author:    Lee Muriithi Kingori\n");
        printk("Kernel:    x86_64 long-mode\n");
        printk("Uptime:    %u seconds\n", (unsigned)(timer_get_ms() / 1000));
        /* Time */
        uint8_t h, m, s;
        uint16_t yr;
        uint8_t mo, dy;
        rtc_get_time(&h, &m, &s);
        rtc_get_date(&yr, &mo, &dy);
        printk("Time:      %02u:%02u:%02u %u-%02u-%02u\n",
               (unsigned)h, (unsigned)m, (unsigned)s,
               (unsigned)yr, (unsigned)mo, (unsigned)dy);
        /* Memory */
        printk("Memory:    %u MB total, %u MB free, %u KB heap\n",
               (unsigned)(pmm_get_total() / (1024*1024)),
               (unsigned)(pmm_get_free() / (1024*1024)),
               (unsigned)(heap_get_used() / 1024));
        /* Network */
        if (net_is_up()) {
            ipv4_addr_t ip = net_get_ip();
            printk("Network:   UP  IP %u.%u.%u.%u\n",
                   ip.bytes[0], ip.bytes[1], ip.bytes[2], ip.bytes[3]);
            if (wifi_is_connected()) {
                printk("WiFi:      %s\n", wifi_get_connected_ssid());
            }
        } else {
            printk("Network:   DOWN\n");
        }
        if (net_ipv6_is_valid()) {
            ipv6_addr_t ip6 = net_get_ipv6();
            char addr_str[40];
            ipv6_addr_to_str(ip6, addr_str, sizeof(addr_str));
            printk("IPv6:      %s\n", addr_str);
        }
        /* Battery */
        printk("Battery:   %d%% (%s)\n", battery_get_percent(),
               battery_get_status_str());
        /* Temperature */
        printk("CPU temp:  %d°C\n", temp_get_cpu());
        /* Disk */
        extern int ahci_has_drive(void);
        extern int ext2_is_mounted(void);
        printk("Disk:      %s\n", ahci_has_drive() ? "present" : "none");
        printk("Filesystem: %s\n", ext2_is_mounted() ? "ext2 mounted" : "not mounted");
        /* Cron */
        printk("Cron:      %d tasks\n", cron_count());
        printk("==========================\n\n");
    }
    else if (strcmp(cmd, "lspci") == 0) {
        printk("\n=== PCI Device List ===\n");
        int count = pci_get_device_count();
        if (count == 0) {
            printk("No PCI devices found.\n");
        } else {
            for (int i = 0; i < count; i++) {
                struct pci_device *d = pci_get_device(i);
                printk("%02x:%02x.%x %04x:%04x  %s [%02x:%02x.%x]  irq=%d",
                       d->bus, d->dev, d->func,
                       d->vendor_id, d->device_id,
                       pci_class_name(d->class_code),
                       d->class_code, d->subclass, d->prog_if,
                       d->irq_line);
                if (d->bar[0])
                    printk("  bar0=0x%x", d->bar[0]);
                printk("\n");
            }
        }
        printk("========================\n\n");
    }
    else if (strcmp(cmd, "exit") == 0) {
        printk("Shell exiting... (system will halt)\n");
        cli();
        while (1) hlt();
    }
    else if (strcmp(cmd, "netstat") == 0) {
        /* Concise network status — IP, MAC, gateway, DNS, IPv6, link, firewall. */
        printk("\n=== Network Status ===\n");
        if (!net_is_up()) {
            printk("Link:      DOWN\n");
            printk("======================\n\n");
            return;
        }
        printk("Link:      UP (e1000)\n");
        ipv4_addr_t ip = net_get_ip();
        printk("IP:        %u.%u.%u.%u\n", ip.bytes[0], ip.bytes[1], ip.bytes[2], ip.bytes[3]);
        ipv4_addr_t gw = net_get_gateway();
        ipv4_addr_t dns = net_get_dns();
        mac_addr_t mac = net_get_mac();
        printk("Gateway:   %u.%u.%u.%u\n", gw.bytes[0], gw.bytes[1], gw.bytes[2], gw.bytes[3]);
        printk("DNS:       %u.%u.%u.%u\n", dns.bytes[0], dns.bytes[1], dns.bytes[2], dns.bytes[3]);
        printk("MAC:       %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac.bytes[0], mac.bytes[1], mac.bytes[2], mac.bytes[3], mac.bytes[4], mac.bytes[5]);
        if (net_ipv6_is_valid()) {
            ipv6_addr_t ip6 = net_get_ipv6();
            char addr_str[40];
            ipv6_addr_to_str(ip6, addr_str, sizeof(addr_str));
            printk("IPv6:      %s\n", addr_str);
        }
        if (wifi_is_connected()) {
            printk("WiFi:      connected to %s\n", wifi_get_connected_ssid());
        } else {
            printk("WiFi:      not connected\n");
        }
        printk("Firewall:  (run 'firewall status' for details)\n");
        printk("======================\n\n");
    }
    else if (strcmp(cmd, "services") == 0) {
        /* Alias: 'services' → 'lee status' (shows all registered services) */
        char* new_argv[3] = { "lee", "status", NULL };
        cmd_lee(2, new_argv);
    }
    else if (strcmp(cmd, "packages") == 0) {
        /* Alias: 'packages' → 'pkg list' (lists 110 packages across 5 repos) */
        char* new_argv[3] = { "pkg", "list", NULL };
        cmd_pkg(2, new_argv);
    }
    else if (strcmp(cmd, "whoami") == 0) {
        /* LestraOS is single-user — the kernel shell always runs as root. */
        printk("root\n");
    }
    else if (strcmp(cmd, "hostname") == 0) {
        printk("lestraos\n");
    }
    else {
        printk("Unknown command: %s\n", cmd);
        printk("Type 'help' for available commands.\n");
    }
}

/* ----- tab completion (W1-F fix #5) ------------------------------------
 *
 * When the user presses Tab, we try to complete the token currently
 * being typed (the substring of input_buffer from the last
 * whitespace to the cursor position `pos`).
 *
 *   1. If the token contains no '/', we attempt COMMAND completion
 *      against the static builtin command table. If exactly one
 *      builtin starts with the token, the token is replaced with the
 *      full command name + a trailing space. If multiple match, they
 *      are listed (like bash) and the input is left unchanged.
 *
 *   2. We also attempt FILE completion: the token is split into a
 *      directory part (before the last '/', or cwd if none) and a
 *      filename prefix. vfs_readdir enumerates the directory; any
 *      entry whose name starts with the prefix is a candidate. Same
 *      single-match-completes / multi-match-lists semantics.
 *
 * Command completion takes precedence (most commands are typed
 * bare), so `fil<Tab>` completes to `file ` rather than listing
 * /files. File completion kicks in once the token contains a '/'.
 *
 * Returns the new cursor position (the index into input_buffer at
 * which the next typed char will land). The buffer is NUL-terminated
 * by the caller. */
static const char* const shell_builtins[] = {
    "help", "echo", "cd", "clear", "uname", "free", "reboot", "shutdown",
    "uptime", "version", "ps", "cpuinfo", "meminfo", "test", "neofetch",
    "install", "ui", "theme", "pkg", "ai", "file", "network", "ifconfig",
    "ping", "ping6", "wget", "claude", "glm", "gemini", "openai", "uai",
    "disk", "mount", "exec", "save", "play", "speak", "date", "time",
    "battery", "temp", "wifi", "cron", "lee", "firewall", "sysinfo",
    "lspci", "exit", "netstat", "services", "packages", "whoami", "hostname",
    NULL
};

/* Erase `old_len` chars from the line, then print `buf` (which is
 * NUL-terminated). Used to redraw the input line on history recall
 * or tab completion. */
static void shell_redraw_line(const char* buf, int old_len) {
    /* Erase old chars one by one with "\b \b". */
    for (int k = 0; k < old_len; k++) printk("\b \b");
    /* Print the new content. */
    printk("%s", buf);
}

/* Try to complete the token at the end of input_buffer (which is
 * currently `pos` chars long). On success, mutates input_buffer in
 * place and returns the new cursor position. On no match, returns
 * `pos` unchanged. Multi-match: prints the candidates on a new line
 * (the caller is responsible for re-printing the prompt+buffer
 * afterwards — for simplicity we just leave the input unchanged and
 * let the user keep typing). */
static int shell_tab_complete(int pos) {
    if (pos <= 0) return pos;

    /* Find the start of the current token (last whitespace). */
    int tok_start = pos;
    while (tok_start > 0 && input_buffer[tok_start - 1] != ' ' &&
           input_buffer[tok_start - 1] != '\t') {
        tok_start--;
    }
    int tok_len = pos - tok_start;
    if (tok_len <= 0) return pos;

    char token[64];
    if (tok_len >= (int)sizeof(token)) tok_len = sizeof(token) - 1;
    memcpy(token, input_buffer + tok_start, tok_len);
    token[tok_len] = '\0';

    /* ---- Step 1: command completion (only if no '/' in token) ---- */
    if (strchr(token, '/') == NULL) {
        const char* single_match = NULL;
        int n_matches = 0;
        for (int i = 0; shell_builtins[i] != NULL; i++) {
            if (strncmp(shell_builtins[i], token, tok_len) == 0) {
                single_match = shell_builtins[i];
                n_matches++;
            }
        }
        if (n_matches == 1 && single_match) {
            /* Replace token with the full command + trailing space. */
            int full_len = (int)strlen(single_match);
            /* Make room: shift everything after `pos` by the delta. */
            int delta = full_len - tok_len + 1;  /* +1 for trailing space */
            if (pos + delta >= CMD_MAX_LEN) return pos;
            /* Move the tail (if any). */
            for (int k = pos; k >= tok_start + tok_len; k--) {
                input_buffer[k + delta] = input_buffer[k];
            }
            /* Write the completed command + space. */
            memcpy(input_buffer + tok_start, single_match, full_len);
            input_buffer[tok_start + full_len] = ' ';
            int new_pos = pos + delta;
            input_buffer[new_pos] = '\0';
            /* Redraw from tok_start onward. */
            int old_visible = pos - tok_start;
            for (int k = 0; k < old_visible; k++) printk("\b \b");
            printk("%s", input_buffer + tok_start);
            return new_pos;
        }
        if (n_matches > 1) {
            /* List matches on a new line, then re-print the prompt
             * and current input so the user can keep typing. */
            printk("\n");
            for (int i = 0; shell_builtins[i] != NULL; i++) {
                if (strncmp(shell_builtins[i], token, tok_len) == 0) {
                    printk("%s  ", shell_builtins[i]);
                }
            }
            printk("\n");
            return pos;  /* input unchanged */
        }
        /* No command match — fall through to file completion. */
    }

    /* ---- Step 2: file completion ---- */
    /* Split token into dir + prefix at the last '/'. */
    char dir[MAX_PATH_LEN];
    char prefix[MAX_NAME_LEN];
    const char* last_slash = strrchr(token, '/');
    if (last_slash) {
        int dlen = (int)(last_slash - token);
        if (dlen >= (int)sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, token, dlen);
        dir[dlen] = '\0';
        /* If the dir part is empty (token started with '/'), use "/". */
        if (dlen == 0) { dir[0] = '/'; dir[1] = '\0'; }
        /* Resolve relative dirs against cwd. */
        if (dir[0] != '/') {
            char tmp[MAX_PATH_LEN];
            ksnprintf(tmp, sizeof(tmp), "%s/%s", cwd, dir);
            strncpy(dir, tmp, sizeof(dir) - 1);
            dir[sizeof(dir) - 1] = '\0';
        }
        strncpy(prefix, last_slash + 1, sizeof(prefix) - 1);
        prefix[sizeof(prefix) - 1] = '\0';
    } else {
        /* No '/' — complete against cwd. */
        strncpy(dir, cwd, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        strncpy(prefix, token, sizeof(prefix) - 1);
        prefix[sizeof(prefix) - 1] = '\0';
    }
    int prefix_len = (int)strlen(prefix);

    /* Enumerate the directory. */
    int fd = vfs_open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return pos;
    char single_name[MAX_NAME_LEN] = {0};
    int single_is_dir = 0;
    int n_matches = 0;
    struct dirent de;
    while (1) {
        int rc = vfs_readdir(fd, &de);
        if (rc != 0 || de.name[0] == '\0') break;
        if (prefix_len == 0 ||
            strncmp(de.name, prefix, prefix_len) == 0) {
            if (n_matches == 0) {
                strncpy(single_name, de.name, sizeof(single_name) - 1);
                single_name[sizeof(single_name) - 1] = '\0';
                single_is_dir = (de.type == FT_DIRECTORY);
            } else if (n_matches == 1) {
                /* Second match — print the first one + this one. */
                printk("\n%s  %s", single_name, de.name);
            } else {
                printk("  %s", de.name);
            }
            n_matches++;
        }
    }
    vfs_close(fd);

    if (n_matches == 1) {
        /* Replace the prefix portion of the token with the full name.
         * Append a '/' if it's a directory so the user can keep
         * tab-completing subdirectories. */
        int name_len = (int)strlen(single_name);
        int extra = single_is_dir ? 1 : 0;  /* trailing '/' */
        int delta = name_len + extra - prefix_len;
        if (pos + delta >= CMD_MAX_LEN) return pos;
        for (int k = pos; k >= tok_start + tok_len; k--) {
            input_buffer[k + delta] = input_buffer[k];
        }
        memcpy(input_buffer + tok_start + (tok_len - prefix_len),
               single_name, name_len);
        if (single_is_dir) {
            input_buffer[tok_start + (tok_len - prefix_len) + name_len] = '/';
        }
        int new_pos = pos + delta;
        input_buffer[new_pos] = '\0';
        int old_visible = pos - (tok_start + (tok_len - prefix_len));
        for (int k = 0; k < old_visible; k++) printk("\b \b");
        printk("%s", input_buffer + tok_start + (tok_len - prefix_len));
        return new_pos;
    }
    if (n_matches > 1) {
        printk("\n");
        return pos;
    }
    /* No file match either — silently do nothing. */
    return pos;
}

/* ----- input ---------------------------------------------------------- */
/* Forward decl: defined further down, but read_line_serial references
 * it for the Tab-completion redraw path. */
static void print_prompt_serial(void);

static int read_line(void) {
    int i = 0;
    /* Save the in-progress input when the user first presses Up, so
     * Down-arrow can restore it after browsing past the newest entry. */
    char saved_input[CMD_MAX_LEN];
    int  saved_len = 0;
    saved_input[0] = '\0';
    history_browse = history_count;  /* start "after" the last entry */

    while (i < CMD_MAX_LEN - 1) {
        char c = keyboard_getchar();

        if (c == (char)SHELL_KEY_UP) {
            /* Up arrow — recall the previous history entry. */
            if (history_count == 0) continue;
            if (history_browse == history_count) {
                /* First Up press: save the current in-progress input. */
                input_buffer[i] = '\0';
                strncpy(saved_input, input_buffer, sizeof(saved_input) - 1);
                saved_input[sizeof(saved_input) - 1] = '\0';
                saved_len = i;
            }
            if (history_browse > 0) {
                history_browse--;
                char entry[HIST_LINE];
                if (history_get(history_browse, entry, sizeof(entry)) == 0) {
                    int old_i = i;
                    strncpy(input_buffer, entry, CMD_MAX_LEN - 1);
                    input_buffer[CMD_MAX_LEN - 1] = '\0';
                    i = (int)strlen(input_buffer);
                    shell_redraw_line(input_buffer, old_i);
                }
            }
            continue;
        }
        if (c == (char)SHELL_KEY_DOWN) {
            /* Down arrow — move forward in history. */
            if (history_count == 0) continue;
            if (history_browse < history_count) {
                history_browse++;
                int old_i = i;
                if (history_browse == history_count) {
                    /* Past the end — restore the saved in-progress input. */
                    strncpy(input_buffer, saved_input, CMD_MAX_LEN - 1);
                    input_buffer[CMD_MAX_LEN - 1] = '\0';
                    i = saved_len;
                } else {
                    char entry[HIST_LINE];
                    if (history_get(history_browse, entry, sizeof(entry)) == 0) {
                        strncpy(input_buffer, entry, CMD_MAX_LEN - 1);
                        input_buffer[CMD_MAX_LEN - 1] = '\0';
                        i = (int)strlen(input_buffer);
                    }
                }
                shell_redraw_line(input_buffer, old_i);
            }
            continue;
        }
        if (c == '\t') {
            /* Tab — attempt completion. */
            input_buffer[i] = '\0';
            int new_i = shell_tab_complete(i);
            if (new_i != i) {
                i = new_i;
            }
            /* If no completion happened, re-print the prompt+input so
             * the user sees where they are (matches the multi-match
             * listing case). */
            if (new_i == i && i > 0) {
                printk("\n");
                print_prompt();
                printk("%s", input_buffer);
            }
            continue;
        }

        if (c == '\n' || c == '\r') {
            input_buffer[i] = '\0';
            printk("\n");
            return i;
        } else if (c == '\b' || c == 127) {
            if (i > 0) {
                i--;
                printk("\b \b");
            }
        } else if (c >= ' ' && c < 127) {
            input_buffer[i++] = c;
            printk("%c", c);
        }
        /* Any byte ≥ 0x80 (other arrow sentinels we don't handle here)
         * is silently dropped by the c >= ' ' filter above. */
    }
    input_buffer[i] = '\0';
    return i;
}

/* ----- serial-only input (cloud/VPS mode) ----------------------------- */
/* Reads a line from the serial port (COM1). No VGA or keyboard needed.
 * This is the primary I/O path in cloud/VPS mode where there is no
 * monitor or keyboard attached. All stdin/stdout/stderr goes through
 * COM1 (0x3F8).
 *
 * Supports the same Up/Down history and Tab completion as the PS/2
 * read_line(). Arrow keys arrive as ANSI X3.64 / VT100 escape
 * sequences: ESC [ A (Up), ESC [ B (Down). Tab is ASCII 0x09. */
static int read_line_serial(void) {
    int i = 0;
    char saved_input[CMD_MAX_LEN];
    int  saved_len = 0;
    saved_input[0] = '\0';
    history_browse = history_count;

    while (i < CMD_MAX_LEN - 1) {
        char c = serial_getchar(COM1);

        /* ESC starts a possible ANSI escape sequence. */
        if (c == 0x1B) {
            /* Peek: if the next two bytes are '[' + 'A'/'B', it's an
             * arrow key. Otherwise treat ESC as a no-op. serial_getchar
             * blocks, so we can safely read the next byte. */
            char c2 = serial_getchar(COM1);
            if (c2 == '[') {
                char c3 = serial_getchar(COM1);
                if (c3 == 'A') {
                    /* Up arrow. */
                    if (history_count == 0) continue;
                    if (history_browse == history_count) {
                        input_buffer[i] = '\0';
                        strncpy(saved_input, input_buffer,
                                sizeof(saved_input) - 1);
                        saved_input[sizeof(saved_input) - 1] = '\0';
                        saved_len = i;
                    }
                    if (history_browse > 0) {
                        history_browse--;
                        char entry[HIST_LINE];
                        if (history_get(history_browse, entry,
                                        sizeof(entry)) == 0) {
                            int old_i = i;
                            strncpy(input_buffer, entry, CMD_MAX_LEN - 1);
                            input_buffer[CMD_MAX_LEN - 1] = '\0';
                            i = (int)strlen(input_buffer);
                            /* Erase old line, draw new. */
                            for (int k = 0; k < old_i; k++)
                                serial_puts(COM1, "\b \b");
                            serial_puts(COM1, input_buffer);
                        }
                    }
                    continue;
                } else if (c3 == 'B') {
                    /* Down arrow. */
                    if (history_count == 0) continue;
                    if (history_browse < history_count) {
                        history_browse++;
                        int old_i = i;
                        if (history_browse == history_count) {
                            strncpy(input_buffer, saved_input,
                                    CMD_MAX_LEN - 1);
                            input_buffer[CMD_MAX_LEN - 1] = '\0';
                            i = saved_len;
                        } else {
                            char entry[HIST_LINE];
                            if (history_get(history_browse, entry,
                                            sizeof(entry)) == 0) {
                                strncpy(input_buffer, entry, CMD_MAX_LEN - 1);
                                input_buffer[CMD_MAX_LEN - 1] = '\0';
                                i = (int)strlen(input_buffer);
                            }
                        }
                        for (int k = 0; k < old_i; k++)
                            serial_puts(COM1, "\b \b");
                        serial_puts(COM1, input_buffer);
                    }
                    continue;
                }
                /* Other ANSI sequences (C/D = Left/Right, etc.) —
                 * consume c3 and continue. */
                continue;
            }
            /* ESC alone (no '[') — ignore. */
            continue;
        }

        if (c == '\t') {
            input_buffer[i] = '\0';
            int new_i = shell_tab_complete(i);
            if (new_i != i) {
                i = new_i;
            }
            if (new_i == i && i > 0) {
                serial_puts(COM1, "\r\n");
                print_prompt_serial();
                serial_puts(COM1, input_buffer);
            }
            continue;
        }
        if (c == '\n' || c == '\r') {
            input_buffer[i] = '\0';
            serial_puts(COM1, "\r\n");
            return i;
        } else if (c == '\b' || c == 127) {
            if (i > 0) {
                i--;
                serial_puts(COM1, "\b \b");
            }
        } else if (c >= ' ' && c < 127) {
            input_buffer[i++] = c;
            serial_putchar(COM1, c);
        }
    }
    input_buffer[i] = '\0';
    return i;
}

/* Print prompt over serial only (no VGA color codes) */
static void print_prompt_serial(void) {
    serial_puts(COM1, "lestra:");
    serial_puts(COM1, cwd);
    serial_puts(COM1, "$ ");
}

/* ----- shell entry ---------------------------------------------------- */
void shell_run(void) {
    printk("\n");
    printk("Lestra Shell (lsh) 1.0 - by Lee Muriithi Kingori\n");
    printk("Type 'help' for available commands.  (Up/Down: history, Tab: complete)\n");
    printk("\n");

    /* Install the arrow-key hook so Up/Down recall history (W1-F #4).
     * The hook injects sentinel bytes into the PS/2 keyboard buffer
     * that read_line() recognises. Safe to install here because in
     * the text/recovery paths input_init() has NOT been called, so
     * we're not clobbering input.c's hook. */
    keyboard_set_handler(shell_kb_hook);

    while (1) {
        print_prompt();
        int len = read_line();
        if (len == 0) continue;
        /* Record the command in the history ring before execution
         * (matches bash, which stores the line regardless of exit
         * status). Empty / duplicate-of-last entries are skipped by
         * history_push itself. */
        history_push(input_buffer);
        parse_args(input_buffer);
        if (argc > 0) {
            execute_command();
        }
    }
}

/* ----- serial-only shell entry (cloud/VPS mode) ----------------------- */
/* Runs the shell entirely through the serial port (COM1, 0x3F8).
 * No VGA, no keyboard, no framebuffer needed. This is the primary
 * interactive shell in cloud/VPS mode where the system is headless.
 *
 * stdin:  COM1 serial input
 * stdout: COM1 serial output (also goes through printk to serial)
 * stderr: COM1 serial output
 *
 * The shell also runs a tick loop that services SSH sessions and
 * the HTTP management API, since in cloud mode these are essential
 * background services that need regular polling. */
void shell_run_serial(void) {
    serial_puts(COM1, "\r\n");
    serial_puts(COM1, "Lestra Shell (lsh) 1.0 - Cloud/VPS Serial Mode\r\n");
    serial_puts(COM1, "Type 'help' for available commands.  (Up/Down: history, Tab: complete)\r\n");
    serial_puts(COM1, "\r\n");

    while (1) {
        print_prompt_serial();
        int len = read_line_serial();
        if (len == 0) continue;
        history_push(input_buffer);
        parse_args(input_buffer);
        if (argc > 0) {
            /* Execute the command — output goes through printk which
             * writes to both VGA (if available) and serial. In cloud
             * mode, serial is the primary output. */
            execute_command();
        }

        /* Service background tasks needed for cloud mode:
         * SSH server tick (processes remote sessions)
         * HTTP management API tick (processes management requests)
         * Network tick (keeps TCP/IP stack alive)
         * Service manager tick (restarts failed services) */
        extern void ssh_server_tick(void);
        extern void http_mgmt_tick(void);
        extern void net_tick(void);
        extern void service_tick(void);

        if (ssh_server_is_running()) ssh_server_tick();
        http_mgmt_tick();
        net_tick();
        service_tick();
    }
}

/* Public API for the GUI terminal: execute a single command line.
 * Output goes to the standard printk (VGA + serial). The GUI terminal
 * intercepts by hooking the printk output path. For v3 MVP, the GUI
 * terminal reads from the keyboard buffer and calls this function. */
void shell_execute_line(const char* line, void (*out)(char c)) {
    (void)out;  /* output goes through printk; GUI intercepts at a lower level */
    if (!line || !*line) return;
    strncpy(input_buffer, line, CMD_MAX_LEN - 1);
    input_buffer[CMD_MAX_LEN - 1] = '\0';
    parse_args(input_buffer);
    if (argc > 0) {
        execute_command();
    }
}
