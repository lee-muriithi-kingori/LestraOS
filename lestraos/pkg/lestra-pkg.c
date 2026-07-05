/*
 * Lestra OS - Package Manager (lestra-pkg)
 * Copyright (c) 2026 lestramk.org
 *
 * Simulated package manager with a prebuilt catalog of "installable"
 * software. Each package has:
 *   - name, version, description
 *   - "size" (in KB) for realistic download output
 *   - "deps" (dependencies, by name)
 *   - install_state
 *
 * The install process prints realistic progress output (download, unpack,
 * configure, done) but does NOT actually install anything — this is a
 * hobbyist OS without the runtime infrastructure to host Python/Node/etc.
 * The catalog serves as a UX scaffold for when those runtimes are ported.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/timer.h>
#include <lestra/mm.h>
#include <string.h>

#define MAX_PACKAGES   64
#define MAX_INSTALLED  32
#define MAX_DEPS       4
#define MAX_NAME_LEN   24
#define MAX_DESC_LEN   56

struct package {
    const char* name;
    const char* version;
    const char* description;
    uint32_t size_kb;          /* download size in KB */
    const char* deps[MAX_DEPS]; /* NULL-terminated */
};

struct installed_pkg {
    char name[MAX_NAME_LEN];
    char version[16];
    uint64_t install_time_ms;
};

/* Prebuilt catalog */
static const struct package catalog[] = {
    /* Languages / runtimes */
    { "python",   "3.11.4",  "Python 3 interpreter (CPython)",            28544, { NULL } },
    { "python2",  "2.7.18",  "Legacy Python 2 interpreter",               12480, { NULL } },
    { "node",     "20.5.0",  "Node.js JavaScript runtime (V8)",           23456, { NULL } },
    { "deno",     "1.35.0",  "Deno secure runtime for TS/JS",             41200, { NULL } },
    { "ruby",     "3.2.2",   "Ruby interpreter (MRI)",                    14800, { NULL } },
    { "perl",     "5.38.0",  "Perl 5 interpreter",                         9216, { NULL } },
    { "lua",      "5.4.6",   "Lua scripting language",                     1024, { NULL } },
    { "php",      "8.2.8",   "PHP interpreter",                           18200, { NULL } },
    { "gcc",      "13.2.0",  "GNU Compiler Collection (C/C++)",           88400, { "binutils", NULL } },
    { "clang",    "16.0.6",  "LLVM C/C++ compiler",                      102400, { "llvm", NULL } },
    { "rust",     "1.71.0",  "Rust toolchain (rustc + cargo)",            65200, { NULL } },
    { "go",       "1.21.0",  "Go toolchain",                              48800, { NULL } },

    /* Editors */
    { "vim",      "9.0.0",   "Vi IMproved editor",                         4200, { NULL } },
    { "emacs",    "28.2",    "GNU Emacs editor",                          22400, { NULL } },
    { "nano",     "7.2",     "Simple text editor",                         1800, { NULL } },
    { "neovim",   "0.9.1",   "Modernized Vim fork",                        6400, { NULL } },

    /* Shells */
    { "bash",     "5.2.15",  "GNU Bourne-Again Shell",                     5200, { NULL } },
    { "zsh",      "5.9",     "Z shell",                                    6800, { NULL } },
    { "fish",     "3.6.1",   "Friendly Interactive Shell",                 7400, { NULL } },
    { "tcsh",     "6.24",    "C shell with file completion",               4200, { NULL } },

    /* Coreutils / utilities */
    { "coreutils","9.3",     "GNU core utilities (ls, cp, mv, etc.)",      8400, { NULL } },
    { "findutils","4.9.0",   "GNU find, locate, xargs",                    2200, { NULL } },
    { "grep",     "3.11",    "GNU grep + egrep + fgrep",                   1200, { NULL } },
    { "sed",      "4.9",     "GNU stream editor",                          980, { NULL } },
    { "gawk",     "5.2.2",   "GNU awk",                                   2400, { NULL } },
    { "tar",      "1.34",    "GNU tar archiver",                          1800, { NULL } },
    { "gzip",     "1.12",    "GNU gzip compression",                       880, { NULL } },
    { "bzip2",    "1.0.8",   "bzip2 compression",                          640, { NULL } },
    { "xz",       "5.4.3",   "xz / lzma compression",                     1200, { NULL } },
    { "zip",      "3.0",     "Info-ZIP zip/unzip",                        1400, { NULL } },

    /* Network (would need TCP/IP stack) */
    { "curl",     "8.2.1",   "Command-line URL transfer tool",            3200, { NULL } },
    { "wget",     "1.21.4",  "GNU wget downloader",                       2800, { NULL } },
    { "openssh",  "9.3p2",   "OpenSSH client + server",                   5400, { NULL } },
    { "netcat",   "1.219",   "TCP/IP swiss army knife",                    420, { NULL } },
    { "iproute2", "6.4.0",   "Linux network configuration",               2400, { NULL } },

    /* Dev tools */
    { "git",      "2.41.0",  "Distributed version control",               8800, { NULL } },
    { "make",     "4.4.1",   "GNU make build tool",                       1400, { NULL } },
    { "cmake",    "3.27.0",  "Cross-platform build system",              12400, { NULL } },
    { "autoconf", "2.71",    "Auto-configuring source code",              2200, { NULL } },
    { "gdb",      "13.2",    "GNU debugger",                             18400, { NULL } },
    { "valgrind", "3.21.0",  "Memory debugging/profiling",               14800, { NULL } },
    { "strace",   "6.4",     "System call tracer",                        1600, { NULL } },
    { "ltrace",   "0.7.3",   "Library call tracer",                        980, { NULL } },

    /* Binutils */
    { "binutils", "2.41",    "GNU binary utilities (ld, as, objdump)",    6800, { NULL } },
    { "llvm",     "16.0.6",  "LLVM compiler infrastructure",             88200, { NULL } },

    /* Misc */
    { "htop",     "3.3.0",   "Interactive process viewer",                1400, { NULL } },
    { "tmux",     "3.3a",    "Terminal multiplexer",                      1800, { NULL } },
    { "screen",   "4.9.0",   "GNU screen terminal multiplexer",           1600, { NULL } },
    { "tree",     "2.1.0",   "Directory tree viewer",                      220, { NULL } },
    { "man",      "2.12.0",  "Manual page viewer",                        2400, { NULL } },
    { "less",     "633",     "Pager",                                      880, { NULL } },
    { "file",     "5.44",    "File type identification",                  1200, { NULL } },
    { "diffutils","3.10",    "diff, cmp, diff3",                          1400, { NULL } },
    { "patch",    "2.7.6",   "Apply diff files",                           640, { NULL } },
    { "pkg-config","0.29.2", "Library compile flags helper",               480, { NULL } },

    /* AI / ML (would need numpy, etc.) */
    { "pip",      "23.2.1",  "Python package installer",                  1800, { "python", NULL } },
    { "npm",      "9.8.0",   "Node.js package manager",                   2400, { "node", NULL } },
    { "yarn",     "1.22.19", "Fast JS package manager",                   1800, { "node", NULL } },
    { "pnpm",     "8.6.10",  "Disk-efficient JS pkg manager",             1600, { "node", NULL } },

    /* LestraOS-specific */
    { "lestra-ai","1.0.0",   "LestraOS AI assistant (built-in)",           420, { NULL } },
    { "lestra-ui","1.0.0",   "LestraOS desktop theme pack",                280, { NULL } },
};

static const int catalog_count = sizeof(catalog) / sizeof(catalog[0]);

static struct installed_pkg installed[MAX_INSTALLED];
static int installed_count = 0;

/* ----- helpers -------------------------------------------------------- */
static const struct package* find_package(const char* name) {
    for (int i = 0; i < catalog_count; i++) {
        if (strcmp(catalog[i].name, name) == 0) return &catalog[i];
    }
    return NULL;
}

static int find_installed(const char* name) {
    for (int i = 0; i < installed_count; i++) {
        if (strcmp(installed[i].name, name) == 0) return i;
    }
    return -1;
}

/* Simple sleep that yields to the timer */
static void pkg_sleep_ms(uint32_t ms) {
    extern uint64_t timer_get_ms(void);
    extern void hlt(void);
    uint64_t target = timer_get_ms() + ms;
    while (timer_get_ms() < target) {
        hlt();
    }
}

/* Print a progress bar */
static void print_progress(uint32_t cur, uint32_t total, const char* label) {
    const int bar_width = 30;
    int pct = (int)((uint64_t)cur * 100 / total);
    if (pct > 100) pct = 100;
    int filled = (pct * bar_width) / 100;

    printk("\r  %s [", label);
    for (int i = 0; i < bar_width; i++) {
        printk(i < filled ? "#" : ".");
    }
    printk("] %d%%", pct);
    if (cur >= total) printk("\n");
}

/* ----- public API ----------------------------------------------------- */

void pkg_init(void) {
    installed_count = 0;
    pr_info("lestra-pkg: package manager initialized (%d packages in catalog)\n",
            catalog_count);
}

int pkg_install(const char* name) {
    const struct package* pkg = find_package(name);
    if (!pkg) {
        pr_err("pkg: package '%s' not found in catalog\n", name);
        return -1;
    }

    if (find_installed(name) >= 0) {
        pr_info("pkg: %s is already installed (version %s)\n", name, pkg->version);
        return 0;
    }

    if (installed_count >= MAX_INSTALLED) {
        pr_err("pkg: cannot install %s - installed package limit reached\n", name);
        return -1;
    }

    /* Check dependencies */
    for (int i = 0; i < MAX_DEPS && pkg->deps[i]; i++) {
        if (find_installed(pkg->deps[i]) < 0) {
            pr_info("pkg: installing dependency '%s'...\n", pkg->deps[i]);
            pkg_install(pkg->deps[i]);
        }
    }

    printk("\n");
    pr_info("Installing %s (%s)...\n", pkg->name, pkg->version);
    pr_info("  Size: %u KB\n", pkg->size_kb);

    /* Simulate download progress */
    uint32_t total = pkg->size_kb;
    uint32_t step = total / 10;
    if (step == 0) step = 1;
    for (uint32_t d = 0; d <= total; d += step) {
        print_progress(d > total ? total : d, total, "Downloading");
        pkg_sleep_ms(80);
    }
    print_progress(total, total, "Downloading");

    /* Simulate unpacking */
    printk("  Unpacking %s...\n", pkg->name);
    pkg_sleep_ms(150);

    printk("  Configuring %s...\n", pkg->name);
    pkg_sleep_ms(100);

    /* Mark as installed */
    strncpy(installed[installed_count].name, pkg->name, MAX_NAME_LEN - 1);
    installed[installed_count].name[MAX_NAME_LEN - 1] = '\0';
    strncpy(installed[installed_count].version, pkg->version, 15);
    installed[installed_count].version[15] = '\0';
    installed[installed_count].install_time_ms = timer_get_ms();
    installed_count++;

    pr_info("Successfully installed %s %s\n", pkg->name, pkg->version);
    return 0;
}

int pkg_remove(const char* name) {
    int idx = find_installed(name);
    if (idx < 0) {
        pr_err("pkg: %s is not installed\n", name);
        return -1;
    }

    /* Check if any other installed package depends on this one */
    for (int i = 0; i < installed_count; i++) {
        if (i == idx) continue;
        const struct package* pkg = find_package(installed[i].name);
        if (pkg) {
            for (int d = 0; d < MAX_DEPS && pkg->deps[d]; d++) {
                if (strcmp(pkg->deps[d], name) == 0) {
                    pr_warn("pkg: cannot remove %s - %s depends on it\n",
                            name, installed[i].name);
                    return -1;
                }
            }
        }
    }

    pr_info("Removing %s (%s)...\n", name, installed[idx].version);
    pkg_sleep_ms(100);

    /* Shift down */
    for (int i = idx; i < installed_count - 1; i++) {
        installed[i] = installed[i + 1];
    }
    installed_count--;

    pr_info("Removed %s\n", name);
    return 0;
}

void pkg_list_available(void) {
    printk("\nAvailable packages (%d total):\n", catalog_count);
    printk("%-16s %-12s %-12s %s\n", "NAME", "VERSION", "SIZE", "DESCRIPTION");
    printk("---------------------------------------------------------------\n");
    for (int i = 0; i < catalog_count; i++) {
        const struct package* p = &catalog[i];
        const char* mark = (find_installed(p->name) >= 0) ? "*" : " ";
        printk("%s%-15s %-12s %5u KB    %s\n",
               mark, p->name, p->version, p->size_kb, p->description);
    }
    printk("\n(* = installed)\n");
}

void pkg_list_installed(void) {
    if (installed_count == 0) {
        printk("\nNo packages installed.\n");
        return;
    }
    printk("\nInstalled packages (%d):\n", installed_count);
    printk("%-16s %-12s\n", "NAME", "VERSION");
    printk("--------------------------\n");
    for (int i = 0; i < installed_count; i++) {
        printk("%-16s %s\n", installed[i].name, installed[i].version);
    }
}

int pkg_search(const char* query) {
    int found = 0;
    printk("\nSearch results for '%s':\n", query);
    for (int i = 0; i < catalog_count; i++) {
        if (strstr(catalog[i].name, query) ||
            strstr(catalog[i].description, query)) {
            printk("  %-16s %s\n", catalog[i].name, catalog[i].description);
            found++;
        }
    }
    if (found == 0) {
        printk("  (no matches)\n");
    }
    return found;
}

void pkg_info(const char* name) {
    const struct package* pkg = find_package(name);
    if (!pkg) {
        pr_err("pkg: package '%s' not found\n", name);
        return;
    }
    printk("\nPackage: %s\n", pkg->name);
    printk("Version: %s\n", pkg->version);
    printk("Size:    %u KB\n", pkg->size_kb);
    printk("Desc:    %s\n", pkg->description);
    if (pkg->deps[0]) {
        printk("Deps:    ");
        for (int i = 0; i < MAX_DEPS && pkg->deps[i]; i++) {
            printk("%s%s", i ? ", " : "", pkg->deps[i]);
        }
        printk("\n");
    } else {
        printk("Deps:    (none)\n");
    }
    printk("State:   %s\n", find_installed(name) >= 0 ? "installed" : "not installed");
}

int pkg_count_installed(void) { return installed_count; }
int pkg_count_available(void) { return catalog_count; }
