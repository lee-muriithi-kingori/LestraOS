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
 *   file ls/cat/write/rm
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
#include <string.h>

#define CMD_MAX_LEN  512
#define ARG_MAX_NUM  32

static char input_buffer[CMD_MAX_LEN];
static char* argv[ARG_MAX_NUM];
static int argc = 0;
static char cwd[64] = "/";

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
    printk("    clear        Clear screen\n");
    printk("    reboot       Reboot system\n");
    printk("    shutdown     Shutdown system\n");
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
    printk("    network                 Show network status (IP, MAC, gateway, DNS)\n");
    printk("    ping <host-or-ip>       Send ICMP echo request (e.g. 'ping 10.0.2.2')\n");
    printk("    wget <url>              HTTP GET to a URL (e.g. 'wget http://example.com/')\n");
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
    printk("    file ls                 List files in VFS\n");
    printk("    file cat <path>         Show file contents\n");
    printk("    file write <p> <text>   Write text to a file\n");
    printk("\n");
    printk("  Other:\n");
    printk("    lee          Sandbox and service manager\n");
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
static void cmd_install(void) {
    printk("\nLestra OS Installer (in-kernel stub)\n");
    printk("For real installation use the host-side tools:\n");
    printk("  Windows:  installer\\install.py --target <dev> --image build\\lestraos.img\n");
    printk("  POSIX:    installer/install.sh --target /dev/sdX --image build/lestraos.img\n");
    printk("Then boot the target device; Lestra OS loads automatically.\n\n");
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

void cmd_shutdown(void) {
    printk("Shutting down...\n");
    /* Try ACPI shutdown first (QEMU/Bochs/VirtualBox + most real HW
     * since ~2005). The 16-bit PIIX4 PM control register at I/O 0x604
     * accepts a write of 0x2000 to trigger S5 (soft-off). On older
     * boxes (no ACPI), we fall back to the 8042 keyboard-controller
     * reset — which reboots instead of powering off, but at least
     * gets the user back to the bootloader. */
    outw(0x604, 0x2000);          /* QEMU/Bochs ACPI shutdown */
    outw(0xB004, 0x2000);         /* older ACPI shutdown register */
    outw(0x4004, 0x3400);         /* Bochs/QEMU older ACPI */
    printk("ACPI shutdown failed; falling back to 8042 reset (reboot).\n");
    outb(0x64, 0xFE);
    while (1) { hlt(); }
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
        printk("  IP:         %u.%u.%u.%u\n", ip.bytes[0], ip.bytes[1], ip.bytes[2], ip.bytes[3]);
        printk("  Gateway:    %u.%u.%u.%u\n", gw.bytes[0], gw.bytes[1], gw.bytes[2], gw.bytes[3]);
        printk("  DNS:        %u.%u.%u.%u\n", dns.bytes[0], dns.bytes[1], dns.bytes[2], dns.bytes[3]);
    } else {
        printk("  Status:     DOWN (DHCP not yet complete or no NIC)\n");
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
    (void)argc; (void)argv;
    extern int ext2_mount(void);
    extern int ext2_is_mounted(void);
    if (ext2_is_mounted()) {
        printk("ext2 filesystem already mounted\n");
        return;
    }
    printk("Attempting to mount ext2 filesystem...\n");
    if (ext2_mount()) {
        printk("ext2 filesystem mounted successfully!\n");
        printk("Root directory listing:\n");
        extern void ext2_list_root(void (*callback)(const char*, uint32_t, uint8_t));
        /* Simple inline callback */
        void list_cb(const char* name, uint32_t inode, uint8_t type) {
            const char* type_str = "?";
            if (type == 1) type_str = "file";
            else if (type == 2) type_str = "dir";
            printk("  [%s] %s (inode %u)\n", type_str, name, (unsigned)inode);
        }
        ext2_list_root(list_cb);
    } else {
        printk("Failed to mount ext2 filesystem.\n");
        printk("Make sure QEMU has a disk attached with a valid ext2 filesystem.\n");
    }
}

/* ----- file subcommands ----------------------------------------------- */
static void cmd_file(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: file <ls|cat|write> [args]\n");
        return;
    }
    if (strcmp(argv[1], "ls") == 0) {
        /* List all files in VFS via vfs_readdir */
        extern int vfs_readdir(int fd, struct dirent* entry);
        printk("\nVFS files:\n");
        struct dirent entry;
        entry.inode = 0;
        int count = 0;
        while (vfs_readdir(0, &entry) == 0) {
            printk("  %s\n", entry.name);
            count++;
        }
        if (count == 0) {
            printk("  (no files in VFS - load an initrd)\n");
        }
        printk("(%d file%s total)\n", count, count == 1 ? "" : "s");
    } else if (strcmp(argv[1], "cat") == 0) {
        if (argc < 3) { printk("Usage: file cat <path>\n"); return; }
        extern ssize_t vfs_read(int fd, void* buf, size_t count);
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
        extern ssize_t vfs_write(int fd, const void* buf, size_t count);
        extern int vfs_open(const char* path, int flags);
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
        extern int vfs_close(int fd);
        vfs_close(fd);
        printk("Wrote %d bytes to %s\n", (int)n, argv[2]);
    } else {
        printk("Unknown file subcommand: %s\n", argv[1]);
    }
}

/* ----- lee subcommands ------------------------------------------------- */
static void cmd_lee(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: lee <strt|stop|status|sandbox|service|net|help>\n");
        printk("  lee strt server [port]    Start sandbox HTTP server (default 8080)\n");
        printk("  lee strt ssh [port]       Start SSH-like remote shell (default 2222)\n");
        printk("  lee strt sandbox [id]     Start sandboxed environment (id=1 or 2)\n");
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
            int port = 8080;
            if (argc >= 4) {
                port = 0;
                for (char* p = argv[3]; *p >= '0' && *p <= '9'; p++)
                    port = port * 10 + (*p - '0');
                if (port <= 0 || port > 65535) port = 8080;
            }
            if (sandbox_server_is_running()) {
                printk("HTTP server already running on port %d\n",
                       sandbox_server_port());
            } else {
                sandbox_server_start(port);
                printk("HTTP sandbox server started on port %d\n", port);
            }
        } else if (strcmp(argv[2], "ssh") == 0) {
            service_start("ssh");
        } else if (strcmp(argv[2], "sandbox") == 0) {
            int id = 0;
            if (argc >= 4) {
                id = 0;
                for (char* p = argv[3]; *p >= '0' && *p <= '9'; p++)
                    id = id * 10 + (*p - '0');
            }
            if (id == 0) {
                id = sandbox_create(NULL, 0);
                if (id > 0) {
                    printk("Created sandbox %d\n", id);
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

/* ----- command dispatch ----------------------------------------------- */
static void execute_command(void) {
    if (argc == 0) return;

    char* cmd = argv[0];

    if (strcmp(cmd, "help") == 0) cmd_help();
    else if (strcmp(cmd, "echo") == 0) cmd_echo();
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
    else if (strcmp(cmd, "ping") == 0) cmd_ping(argc, argv);
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
    else if (strcmp(cmd, "sysinfo") == 0) {
        printk("\n=== System Information ===\n");
        printk("OS:        Lestra OS 1.0.0-alpha\n");
        printk("Author:    Lee Muriihi Kingori\n");
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
    else if (strcmp(cmd, "exit") == 0) {
        printk("Shell exiting... (system will halt)\n");
        cli();
        while (1) hlt();
    }
    else {
        printk("Unknown command: %s\n", cmd);
        printk("Type 'help' for available commands.\n");
    }
}

/* ----- input ---------------------------------------------------------- */
static int read_line(void) {
    int i = 0;
    while (i < CMD_MAX_LEN - 1) {
        char c = keyboard_getchar();
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
    }
    input_buffer[i] = '\0';
    return i;
}

/* ----- shell entry ---------------------------------------------------- */
void shell_run(void) {
    printk("\n");
    printk("Lestra Shell (lsh) 1.0 - by Lee Muriihi Kingori\n");
    printk("Type 'help' for available commands.\n");
    printk("\n");

    while (1) {
        print_prompt();
        int len = read_line();
        if (len == 0) continue;
        parse_args(input_buffer);
        if (argc > 0) {
            execute_command();
        }
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
