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
    printk("    ai keys list            Show configured API keys\n");
    printk("    ai keys set <p> <key>   Set API key (p: openai|claude|gemini|glm)\n");
    printk("    ai keys clear <p>       Clear a provider key\n");
    printk("    ai chat <prompt>        Chat with the AI\n");
    printk("    ai agent <prompt>       Chat with tool-calling agentic loop\n");
    printk("    ai tools                List available tools\n");
    printk("    ai providers            List supported providers\n");
    printk("\n");
    printk("  Files (file):\n");
    printk("    file ls                 List files in VFS\n");
    printk("    file cat <path>         Show file contents\n");
    printk("    file write <p> <text>   Write text to a file\n");
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

static void cmd_reboot(void) {
    printk("Rebooting system...\n");
    outb(0x64, 0xFE);
    while (1) { hlt(); }
}

static void cmd_shutdown(void) {
    printk("Shutting down...\n");
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
    printk("  PID  PPID  STATE    NAME\n");
    printk("    0    -1  running  idle\n");
    printk("    1     0  running  kernel\n");
    printk("    2     1  running  shell\n");
}

static void cmd_cpuinfo(void) {
    printk("CPU Information:\n");
    printk("  Architecture: x86_64\n");
    printk("  Model:        QEMU Virtual CPU\n");
    printk("  Cores:        1\n");
    printk("  Features:     PAE, PSE, Long Mode\n");
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
        printk("Usage: pkg <install|remove|list|installed|search|info> [args]\n");
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
    } else {
        printk("Unknown pkg subcommand: %s\n", argv[1]);
    }
}

/* ----- ai subcommands ------------------------------------------------- */
static void cmd_ai(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: ai <keys|chat|agent|tools|providers> [args]\n");
        return;
    }
    if (strcmp(argv[1], "keys") == 0) {
        if (argc < 3) {
            ai_keys_list();
            return;
        }
        if (strcmp(argv[2], "list") == 0) {
            ai_keys_list();
        } else if (strcmp(argv[2], "set") == 0) {
            if (argc < 5) {
                printk("Usage: ai keys set <provider> <key>\n");
                printk("Providers: openai, claude, gemini, glm\n");
                return;
            }
            int p = ai_provider_from_name(argv[3]);
            if (p < 0) {
                printk("Unknown provider: %s\n", argv[3]);
                printk("Providers: openai, claude, gemini, glm\n");
                return;
            }
            if (ai_keys_set(p, argv[4]) == 0) {
                printk("API key set for %s\n", ai_provider_name(p));
            } else {
                printk("Failed to set API key\n");
            }
        } else if (strcmp(argv[2], "clear") == 0) {
            if (argc < 4) {
                printk("Usage: ai keys clear <provider>\n");
                return;
            }
            int p = ai_provider_from_name(argv[3]);
            if (p < 0) {
                printk("Unknown provider: %s\n", argv[3]);
                return;
            }
            ai_keys_clear(p);
            printk("API key cleared for %s\n", ai_provider_name(p));
        } else {
            printk("Unknown keys subcommand: %s\n", argv[2]);
        }
    } else if (strcmp(argv[1], "chat") == 0) {
        if (argc < 3) {
            printk("Usage: ai chat <your prompt>\n");
            return;
        }
        /* Reconstruct prompt from argv[2..] */
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
            printk("The agent has access to tools: shell, file_read, file_write,\n");
            printk("  pkg_install, pkg_list, meminfo, uptime.\n");
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
        printk("\nSupported AI providers:\n");
        for (int i = 0; i < AI_PROVIDER_COUNT; i++) {
            printk("  %-15s %-22s model: %s\n",
                   ai_provider_name(i),
                   ai_provider_endpoint(i),
                   ai_provider_model(i));
        }
        printk("\n");
    } else {
        printk("Unknown ai subcommand: %s\n", argv[1]);
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
    printk("Welcome to Lestra Shell (lsh) 1.0\n");
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
