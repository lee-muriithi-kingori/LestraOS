/*
 * Lestra OS - Package Manager with Multi-Repository Support
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Repositories: core, websec, devtools, multimedia, lestra
 * Commands: repo list/add/remove, search, install, list, installed, info, remove, update
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/timer.h>
#include <lestra/mm.h>
#include <lestra/net.h>
#include <lestra/vfs.h>
#include <string.h>

#define MAX_PACKAGES   128
#define MAX_INSTALLED  32
#define MAX_DEPS       4
#define MAX_NAME_LEN   24
#define MAX_DESC_LEN   56
#define MAX_REPOS      8
#define MAX_REPO_NAME  16
#define MAX_REPO_URL   128

/* Repository definition */
struct repo {
    char name[MAX_REPO_NAME];
    char url[MAX_REPO_URL];
    char description[64];
    int enabled;
};

/* Package definition */
struct package {
    const char* name;
    const char* version;
    const char* description;
    uint32_t size_kb;
    const char* deps[MAX_DEPS];
    const char* url;
    const char* repo;       /* which repo this belongs to */
    const char* category;   /* "lang", "editor", "net", "sec", "dev", "media" */
};

struct installed_pkg {
    char name[MAX_NAME_LEN];
    char version[16];
    uint64_t install_time_ms;
};

/* ===== REPOSITORIES ===== */
static struct repo repos[MAX_REPOS] = {
    { "core",      "http://pkg.lestramk.org/core",      "Core system packages",     1 },
    { "websec",    "http://pkg.lestramk.org/websec",    "Web security tools",       1 },
    { "devtools",  "http://pkg.lestramk.org/devtools",  "Development tools",        1 },
    { "multimedia","http://pkg.lestramk.org/multimedia","Multimedia & media tools", 1 },
    { "lestra",    "http://pkg.lestramk.org/lestra",    "LestraOS-specific",        1 },
};
static int repo_count = 5;

/* ===== PACKAGE CATALOG ===== */
static const struct package catalog[] = {
    /* ===== CORE REPO ===== */
    /* Languages / runtimes */
    { "python",   "3.12.0",  "Python 3 interpreter (CPython)",            28544, { NULL }, NULL, "core", "lang" },
    { "python2",  "2.7.18",  "Legacy Python 2 interpreter",               12480, { NULL }, NULL, "core", "lang" },
    { "node",     "20.5.0",  "Node.js JavaScript runtime (V8)",           23456, { NULL }, NULL, "core", "lang" },
    { "deno",     "1.35.0",  "Deno secure runtime for TS/JS",             41200, { NULL }, NULL, "core", "lang" },
    { "ruby",     "3.2.2",   "Ruby interpreter (MRI)",                    14800, { NULL }, NULL, "core", "lang" },
    { "perl",     "5.38.0",  "Perl 5 interpreter",                         9216, { NULL }, NULL, "core", "lang" },
    { "lua",      "5.4.6",   "Lua scripting language",                     1024, { NULL }, NULL, "core", "lang" },
    { "php",      "8.2.8",   "PHP interpreter",                           18200, { NULL }, NULL, "core", "lang" },
    { "gcc",      "13.2.0",  "GNU Compiler Collection (C/C++)",           88400, { "binutils", NULL }, NULL, "core", "lang" },
    { "clang",    "16.0.6",  "LLVM C/C++ compiler",                      102400, { "llvm", NULL }, NULL, "core", "lang" },
    { "rust",     "1.71.0",  "Rust toolchain (rustc + cargo)",            65200, { NULL }, NULL, "core", "lang" },
    { "go",       "1.21.0",  "Go toolchain",                              48800, { NULL }, NULL, "core", "lang" },
    { "java",     "21.0.0",  "OpenJDK 21 runtime",                        45000, { NULL }, NULL, "core", "lang" },

    /* Editors */
    { "vim",      "9.0.0",   "Vi IMproved editor",                         4200, { NULL }, NULL, "core", "editor" },
    { "emacs",    "28.2",    "GNU Emacs editor",                          22400, { NULL }, NULL, "core", "editor" },
    { "nano",     "7.2",     "Simple text editor",                         1800, { NULL }, NULL, "core", "editor" },
    { "neovim",   "0.9.1",   "Modernized Vim fork",                        6400, { NULL }, NULL, "core", "editor" },

    /* Shells */
    { "bash",     "5.2.15",  "GNU Bourne-Again Shell",                     5200, { NULL }, NULL, "core", "shell" },
    { "zsh",      "5.9",     "Z shell",                                    6800, { NULL }, NULL, "core", "shell" },
    { "fish",     "3.6.1",   "Friendly Interactive Shell",                 7400, { NULL }, NULL, "core", "shell" },

    /* Coreutils / utilities */
    { "coreutils","9.3",     "GNU core utilities (ls, cp, mv, etc.)",      8400, { NULL }, NULL, "core", "util" },
    { "findutils","4.9.0",   "GNU find, locate, xargs",                    2200, { NULL }, NULL, "core", "util" },
    { "grep",     "3.11",    "GNU grep + egrep + fgrep",                   1200, { NULL }, NULL, "core", "util" },
    { "sed",      "4.9",     "GNU stream editor",                          980, { NULL }, NULL, "core", "util" },
    { "gawk",     "5.2.2",   "GNU awk",                                   2400, { NULL }, NULL, "core", "util" },
    { "tar",      "1.34",    "GNU tar archiver",                          1800, { NULL }, NULL, "core", "util" },
    { "gzip",     "1.12",    "GNU gzip compression",                       880, { NULL }, NULL, "core", "util" },
    { "bzip2",    "1.0.8",   "bzip2 compression",                          640, { NULL }, NULL, "core", "util" },
    { "xz",       "5.4.3",   "xz / lzma compression",                     1200, { NULL }, NULL, "core", "util" },
    { "zip",      "3.0",     "Info-ZIP zip/unzip",                        1400, { NULL }, NULL, "core", "util" },

    /* Network basics */
    { "curl",     "8.2.1",   "Command-line URL transfer tool",            3200, { NULL }, NULL, "core", "net" },
    { "wget",     "1.21.4",  "GNU wget downloader",                       2800, { NULL }, NULL, "core", "net" },
    { "openssh",  "9.3p2",   "OpenSSH client + server",                   5400, { NULL }, NULL, "core", "net" },
    { "netcat",   "1.219",   "TCP/IP swiss army knife",                    420, { NULL }, NULL, "core", "net" },
    { "iproute2", "6.4.0",   "Network configuration tools",               2400, { NULL }, NULL, "core", "net" },
    { "wireguard","1.0.0",   "WireGuard VPN tools",                       1200, { NULL }, NULL, "core", "net" },

    /* Misc */
    { "htop",     "3.3.0",   "Interactive process viewer",                1400, { NULL }, NULL, "core", "util" },
    { "tmux",     "3.3a",    "Terminal multiplexer",                      1800, { NULL }, NULL, "core", "util" },
    { "screen",   "4.9.0",   "GNU screen terminal multiplexer",           1600, { NULL }, NULL, "core", "util" },
    { "tree",     "2.1.0",   "Directory tree viewer",                      220, { NULL }, NULL, "core", "util" },
    { "man",      "2.12.0",  "Manual page viewer",                        2400, { NULL }, NULL, "core", "util" },
    { "less",     "633",     "Pager",                                      880, { NULL }, NULL, "core", "util" },
    { "file",     "5.44",    "File type identification",                  1200, { NULL }, NULL, "core", "util" },
    { "diffutils","3.10",    "diff, cmp, diff3",                          1400, { NULL }, NULL, "core", "util" },
    { "patch",    "2.7.6",   "Apply diff files",                           640, { NULL }, NULL, "core", "util" },

    /* ===== WEBSEC REPO ===== */
    { "nmap",      "7.94",   "Network scanner & mapper",                  5600, { NULL }, NULL, "websec", "scanner" },
    { "masscan",   "1.3.2",  "Fast port scanner",                         2400, { NULL }, NULL, "websec", "scanner" },
    { "nikto",     "2.5.0",  "Web server scanner",                        3200, { NULL }, NULL, "websec", "scanner" },
    { "dirb",      "2.22",   "Directory bruteforcer",                     1800, { NULL }, NULL, "websec", "scanner" },
    { "gobuster",  "3.6.0",  "Directory/file/DNS brute tool",             4200, { NULL }, NULL, "websec", "scanner" },
    { "ffuf",      "2.1.0",  "Fast web fuzzer",                           2800, { NULL }, NULL, "websec", "fuzzer" },
    { "wfuzz",     "2.4.0",  "Web application fuzzer",                    3400, { NULL }, NULL, "websec", "fuzzer" },
    { "sqlmap",    "1.8.0",  "SQL injection tool",                        8200, { NULL }, NULL, "websec", "exploit" },
    { "hydra",     "9.5.0",  "Password brute-force tool",                 2400, { NULL }, NULL, "websec", "exploit" },
    { "metasploit","6.3.0",  "Metasploit framework",                     45000, { "ruby", NULL }, NULL, "websec", "exploit" },
    { "wireshark", "4.0.0",  "Network protocol analyzer",                28000, { NULL }, NULL, "websec", "sniffer" },
    { "tcpdump",   "4.99.0", "Packet capture tool",                       2800, { NULL }, NULL, "websec", "sniffer" },
    { "aircrack",  "1.7.0",  "WiFi security auditing",                    6400, { NULL }, NULL, "websec", "wireless" },
    { "reaver",    "1.6.0",  "WPS attack tool",                           1800, { NULL }, NULL, "websec", "wireless" },
    { "john",      "1.9.0",  "John the Ripper password cracker",          5200, { NULL }, NULL, "websec", "crack" },
    { "hashcat",   "6.2.0",  "GPU password recovery",                    12000, { NULL }, NULL, "websec", "crack" },
    { "burpsuite", "2023.0", "Web proxy & scanner",                      18000, { "java", NULL }, NULL, "websec", "proxy" },
    { "zaproxy",   "2.14.0", "OWASP ZAP web scanner",                    22000, { "java", NULL }, NULL, "websec", "proxy" },
    { "wpscan",    "3.8.0",  "WordPress scanner",                         6800, { "ruby", NULL }, NULL, "websec", "scanner" },
    { "amass",     "4.2.0",  "OSINT attack surface mapping",              4200, { NULL }, NULL, "websec", "recon" },
    { "subfinder", "2.6.0",  "Subdomain discovery tool",                  2800, { NULL }, NULL, "websec", "recon" },
    { "httpx",     "1.3.0",  "HTTP toolkit",                              1800, { NULL }, NULL, "websec", "recon" },
    { "nuclei",    "3.0.0",  "Vulnerability scanner",                     3400, { NULL }, NULL, "websec", "scanner" },

    /* ===== DEVTOOLS REPO ===== */
    { "git",       "2.41.0", "Distributed version control",               8800, { NULL }, NULL, "devtools", "vcs" },
    { "make",      "4.4.1",  "GNU make build tool",                       1400, { NULL }, NULL, "devtools", "build" },
    { "cmake",     "3.27.0", "Cross-platform build system",              12400, { NULL }, NULL, "devtools", "build" },
    { "autoconf",  "2.71",   "Auto-configuring source code",              2200, { NULL }, NULL, "devtools", "build" },
    { "gdb",       "13.2",   "GNU debugger",                             18400, { NULL }, NULL, "devtools", "debug" },
    { "valgrind",  "3.21.0", "Memory debugging/profiling",               14800, { NULL }, NULL, "devtools", "debug" },
    { "strace",    "6.4",    "System call tracer",                        1600, { NULL }, NULL, "devtools", "debug" },
    { "ltrace",    "0.7.3",  "Library call tracer",                        980, { NULL }, NULL, "devtools", "debug" },
    { "binutils",  "2.41",   "GNU binary utilities (ld, as, objdump)",    6800, { NULL }, NULL, "devtools", "build" },
    { "llvm",      "16.0.6", "LLVM compiler infrastructure",             88200, { NULL }, NULL, "devtools", "build" },
    { "docker",    "24.0.0", "Container runtime",                        34000, { NULL }, NULL, "devtools", "container" },
    { "podman",    "4.5.0",  "Daemonless container engine",              28000, { NULL }, NULL, "devtools", "container" },
    { "kubectl",   "1.28.0", "Kubernetes CLI",                            8400, { NULL }, NULL, "devtools", "container" },
    { "terraform", "1.5.0",  "Infrastructure as code",                   24000, { NULL }, NULL, "devtools", "infra" },
    { "ansible",   "8.0.0",  "Automation tool",                          18000, { "python", NULL }, NULL, "devtools", "infra" },
    { "pkg-config","0.29.2", "Library compile flags helper",               480, { NULL }, NULL, "devtools", "build" },
    { "pip",       "23.2.1", "Python package installer",                  1800, { "python", NULL }, NULL, "devtools", "pm" },
    { "npm",       "9.8.0",  "Node.js package manager",                   2400, { "node", NULL }, NULL, "devtools", "pm" },
    { "yarn",      "1.22.19","Fast JS package manager",                   1800, { "node", NULL }, NULL, "devtools", "pm" },
    { "cargo",     "1.71.0", "Rust package manager",                     12000, { "rust", NULL }, NULL, "devtools", "pm" },

    /* ===== MULTIMEDIA REPO ===== */
    { "ffmpeg",    "6.0.0",  "Multimedia framework (convert/encode)",    28000, { NULL }, NULL, "multimedia", "video" },
    { "ffprobe",   "6.0.0",  "Media stream analyzer",                     8000, { NULL }, NULL, "multimedia", "video" },
    { "imagemagick","7.1.0", "Image manipulation suite",                 18000, { NULL }, NULL, "multimedia", "image" },
    { "gimp",      "2.10.0", "GNU Image Manipulation Program",           34000, { NULL }, NULL, "multimedia", "image" },
    { "inkscape",  "1.3.0",  "Vector graphics editor",                   28000, { NULL }, NULL, "multimedia", "image" },
    { "vlc",       "3.0.0",  "Media player",                             24000, { "ffmpeg", NULL }, NULL, "multimedia", "player" },
    { "mpv",       "0.36.0", "Minimalist media player",                   8400, { "ffmpeg", NULL }, NULL, "multimedia", "player" },
    { "audacity",  "3.3.0",  "Audio editor",                             22000, { NULL }, NULL, "multimedia", "audio" },
    { "sox",       "14.4.2", "Sound eXchange audio tool",                 4200, { NULL }, NULL, "multimedia", "audio" },
    { "obs",       "30.0.0", "Screen recorder & streaming",              32000, { "ffmpeg", NULL }, NULL, "multimedia", "capture" },
    { "handbrake", "1.7.0",  "Video transcoder",                         18000, { NULL }, NULL, "multimedia", "video" },
    { "blender",   "4.0.0",  "3D creation suite",                       120000, { NULL }, NULL, "multimedia", "3d" },
    { "kdenlive",  "23.08.0","Video editor",                             28000, { "ffmpeg", NULL }, NULL, "multimedia", "video" },

    /* ===== LESTRA REPO ===== */
    { "lestra-ai", "1.0.0",  "LestraOS AI assistant (built-in)",           420, { NULL }, NULL, "lestra", "system" },
    { "lestra-ui", "1.0.0",  "LestraOS desktop theme pack",                280, { NULL }, NULL, "lestra", "system" },
    { "lestra-tts","1.0.0",  "LestraOS text-to-speech engine",             680, { NULL }, NULL, "lestra", "system" },
    { "lestra-net","1.0.0",  "LestraOS network tools",                    1200, { NULL }, NULL, "lestra", "system" },
    { "smollm",    "1.0.0",  "SmolLM 500M offline LLM engine",          512000, { NULL }, NULL, "lestra", "ai" },

    /* Downloadable demo packages */
    { "hello",     "1.0",    "Hello-world demo",                             1, { NULL }, "http://example.com/", "core", "demo" },
    { "rfc2616",   "1.0",    "HTTP/1.1 spec (plain-text)",                 400, { NULL }, "http://www.w3.org/Protocols/rfc2616/rfc2616.txt", "core", "doc" },
    { "rfc791",    "1.0",    "IPv4 spec (plain-text)",                     100, { NULL }, "http://www.rfc-editor.org/rfc/rfc791.txt", "core", "doc" },
    { "rfc1035",   "1.0",    "DNS spec (plain-text)",                      200, { NULL }, "http://www.rfc-editor.org/rfc/rfc1035.txt", "core", "doc" },
};

static const int catalog_count = sizeof(catalog) / sizeof(catalog[0]);

static struct installed_pkg installed[MAX_INSTALLED];
static int installed_count = 0;

/* ----- helpers ----- */
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

static void pkg_sleep_ms(uint32_t ms) {
    extern uint64_t timer_get_ms(void);
    uint64_t target = timer_get_ms() + ms;
    while (timer_get_ms() < target) { hlt(); }
}

static void print_progress(uint32_t cur, uint32_t total, const char* label) {
    const int bar_width = 30;
    int pct = (int)((uint64_t)cur * 100 / total);
    if (pct > 100) pct = 100;
    int filled = (pct * bar_width) / 100;
    printk("\r  %s [", label);
    for (int i = 0; i < bar_width; i++) printk(i < filled ? "#" : ".");
    printk("] %d%%", pct);
    if (cur >= total) printk("\n");
}

/* ----- repository API ----- */

void pkg_repo_list(void) {
    printk("\nConfigured repositories (%d):\n", repo_count);
    printk("NAME             STATUS   URL\n");
    printk("----------------------------------------------------------\n");
    for (int i = 0; i < repo_count; i++) {
        printk("%s %s %s\n",
               repos[i].name,
               repos[i].enabled ? "enabled" : "disabled",
               repos[i].url);
    }
    printk("\n");
}

int pkg_repo_add(const char* name, const char* url) {
    if (repo_count >= MAX_REPOS) { pr_warn("pkg: repo limit reached\n"); return -1; }
    /* Check if already exists */
    for (int i = 0; i < repo_count; i++) {
        if (strcmp(repos[i].name, name) == 0) {
            pr_warn("pkg: repo '%s' already exists\n", name);
            return -1;
        }
    }
    strncpy(repos[repo_count].name, name, MAX_REPO_NAME - 1);
    strncpy(repos[repo_count].url, url, MAX_REPO_URL - 1);
    repos[repo_count].enabled = 1;
    repo_count++;
    pr_info("pkg: added repo '%s' -> %s\n", name, url);
    return 0;
}

int pkg_repo_remove(const char* name) {
    for (int i = 0; i < repo_count; i++) {
        if (strcmp(repos[i].name, name) == 0) {
            /* Don't allow removing core repos */
            if (strcmp(name, "core") == 0) {
                pr_warn("pkg: cannot remove 'core' repo\n");
                return -1;
            }
            for (int j = i; j < repo_count - 1; j++) {
                repos[j] = repos[j + 1];
            }
            repo_count--;
            pr_info("pkg: removed repo '%s'\n", name);
            return 0;
        }
    }
    pr_warn("pkg: repo '%s' not found\n", name);
    return -1;
}

void pkg_repo_update(void) {
    printk("\nRefreshing repository catalogs...\n");
    for (int i = 0; i < repo_count; i++) {
        if (!repos[i].enabled) continue;
        printk("  [%s] %s... ", repos[i].name, repos[i].url);
        /* In a real implementation, this would fetch the catalog from the URL.
         * For now, we just report the local catalog. */
        int count = 0;
        for (int j = 0; j < catalog_count; j++) {
            if (catalog[j].repo && strcmp(catalog[j].repo, repos[i].name) == 0) count++;
        }
        printk("%d packages\n", count);
    }
    printk("Done.\n\n");
}

/* ----- package API ----- */

void pkg_init(void) {
    installed_count = 0;
    pr_info("lestra-pkg: package manager initialized (%d packages, %d repos)\n",
            catalog_count, repo_count);
}

int pkg_install(const char* name) {
    const struct package* pkg = find_package(name);
    if (!pkg) {
        pr_err("pkg: package '%s' not found\n", name);
        pr_info("  Try: pkg search %s\n", name);
        return -1;
    }

    /* Check if repo is enabled */
    for (int i = 0; i < repo_count; i++) {
        if (repos[i].enabled && strcmp(repos[i].name, pkg->repo) == 0) break;
        if (i == repo_count - 1) {
            pr_warn("pkg: repo '%s' is disabled\n", pkg->repo);
            return -1;
        }
    }

    if (find_installed(name) >= 0) {
        pr_info("pkg: %s is already installed (%s)\n", name, pkg->version);
        return 0;
    }

    if (installed_count >= MAX_INSTALLED) {
        pr_err("pkg: installed package limit reached\n");
        return -1;
    }

    /* Dependencies */
    for (int i = 0; i < MAX_DEPS && pkg->deps[i]; i++) {
        if (find_installed(pkg->deps[i]) < 0) {
            pr_info("pkg: installing dependency '%s'...\n", pkg->deps[i]);
            pkg_install(pkg->deps[i]);
        }
    }

    printk("\n");
    pr_info("Installing %s (%s) from [%s]\n", pkg->name, pkg->version, pkg->repo);
    pr_info("  Category: %s\n", pkg->category);
    pr_info("  Size: %u KB\n", pkg->size_kb);

    /* Download if URL available */
    extern int net_is_up(void);
    int net_ok = net_is_up();
    if (pkg->url && net_ok) {
        pr_info("  Downloading from %s\n", pkg->url);
        struct http_response resp;
        int rc = http_get(pkg->url, &resp);
        if (rc == 0 && resp.status == 200) {
            pr_info("  Downloaded %u bytes (HTTP %u)\n",
                    (unsigned)resp.body_len, (unsigned)resp.status);
            char path[64];
            int nlen = strlen(pkg->name);
            if (nlen > 40) nlen = 40;
            memcpy(path, "/var/packages/", 14);
            memcpy(path + 14, pkg->name, nlen);
            path[14 + nlen] = '\0';
            vfs_create(path, 0644);
            int fd = vfs_open(path, 0);
            if (fd >= 0) {
                vfs_write(fd, resp.body, resp.body_len);
                vfs_close(fd);
            }
        } else {
            pr_warn("  Download failed\n");
        }
    }

    if (pkg->url && net_ok) {
        pr_info("  -> downloaded %u bytes to /var/packages/%s\n",
                (unsigned)pkg->size_kb * 1024, pkg->name);
    } else if (!pkg->url) {
        pr_info("  -> catalog entry only (no download URL)\n");
    } else if (!net_ok) {
        pr_info("  -> network down; cannot fetch URL\n");
    }

    strncpy(installed[installed_count].name, pkg->name, MAX_NAME_LEN - 1);
    installed[installed_count].name[MAX_NAME_LEN - 1] = '\0';
    strncpy(installed[installed_count].version, pkg->version, 15);
    installed[installed_count].install_time_ms = timer_get_ms();
    installed_count++;

    pr_info("Registered %s %s as installed (in-memory, lost on reboot)\n",
            pkg->name, pkg->version);
    return 0;
}

int pkg_remove(const char* name) {
    int idx = find_installed(name);
    if (idx < 0) { pr_err("pkg: %s is not installed\n", name); return -1; }
    /* Check reverse deps */
    for (int i = 0; i < installed_count; i++) {
        if (i == idx) continue;
        const struct package* pkg = find_package(installed[i].name);
        if (pkg) {
            for (int d = 0; d < MAX_DEPS && pkg->deps[d]; d++) {
                if (strcmp(pkg->deps[d], name) == 0) {
                    pr_warn("pkg: cannot remove %s - %s depends on it\n", name, installed[i].name);
                    return -1;
                }
            }
        }
    }
    pr_info("Removing %s (%s)...\n", name, installed[idx].version);
    pkg_sleep_ms(100);
    for (int i = idx; i < installed_count - 1; i++) installed[i] = installed[i + 1];
    installed_count--;
    pr_info("Removed %s\n", name);
    return 0;
}

void pkg_list_available(void) {
    printk("\nAvailable packages (%d total, %d repos):\n", catalog_count, repo_count);
    printk("%s %s %s %s %s %s\n", "NAME", "VERSION", "SIZE", "REPO", "CAT", "DESCRIPTION");
    printk("------------------------------------------------------------------------\n");
    for (int i = 0; i < catalog_count; i++) {
        const struct package* p = &catalog[i];
        const char* mark = (find_installed(p->name) >= 0) ? "*" : " ";
        printk("%s%s %s %5uKB %s %s %s\n",
               mark, p->name, p->version, p->size_kb, p->repo, p->category, p->description);
    }
    printk("\n(* = installed)\n");
}

void pkg_list_installed(void) {
    if (installed_count == 0) { printk("\nNo packages installed.\n"); return; }
    printk("\nInstalled packages (%d):\n", installed_count);
    printk("%s %s\n", "NAME", "VERSION");
    printk("--------------------------\n");
    for (int i = 0; i < installed_count; i++) {
        printk("%s %s\n", installed[i].name, installed[i].version);
    }
}

int pkg_search(const char* query) {
    int found = 0;
    printk("\nSearch results for '%s':\n", query);
    printk("%s %s %s %s\n", "NAME", "REPO", "CAT", "DESCRIPTION");
    printk("------------------------------------------------------\n");
    for (int i = 0; i < catalog_count; i++) {
        if (strstr(catalog[i].name, query) || strstr(catalog[i].description, query)) {
            printk("  %s %s %s %s\n",
                   catalog[i].name, catalog[i].repo, catalog[i].category, catalog[i].description);
            found++;
        }
    }
    if (found == 0) printk("  (no matches)\n");
    printk("\n%d package(s) found.\n", found);
    return found;
}

void pkg_info(const char* name) {
    const struct package* pkg = find_package(name);
    if (!pkg) { pr_err("pkg: package '%s' not found\n", name); return; }
    printk("\nPackage:  %s\n", pkg->name);
    printk("Version:  %s\n", pkg->version);
    printk("Repo:     %s\n", pkg->repo);
    printk("Category: %s\n", pkg->category);
    printk("Size:     %u KB\n", pkg->size_kb);
    printk("Desc:     %s\n", pkg->description);
    if (pkg->deps[0]) {
        printk("Deps:     ");
        for (int i = 0; i < MAX_DEPS && pkg->deps[i]; i++)
            printk("%s%s", i ? ", " : "", pkg->deps[i]);
        printk("\n");
    } else {
        printk("Deps:     (none)\n");
    }
    printk("State:    %s\n", find_installed(name) >= 0 ? "installed" : "not installed");
    if (pkg->url) printk("URL:      %s\n", pkg->url);
}

int pkg_count_installed(void) { return installed_count; }
int pkg_count_available(void) { return catalog_count; }
int pkg_repo_count(void) { return repo_count; }

/* Alias used by kernel_main splash reporting. */
int pkg_catalog_size(void) { return catalog_count; }

/* ===== DRIVER CATALOG ===== */
typedef enum {
    DRIVER_STATUS_LOADED,
    DRIVER_STATUS_STUB,
    DRIVER_STATUS_MISSING_DEP,
} driver_status_t;

struct driver_entry {
    const char* id;
    const char* name;
    const char* category;
    driver_status_t status;
    const char* description;
};

static const struct driver_entry drivers[] = {
    { "e1000",   "Intel E1000 NIC",     "Network", DRIVER_STATUS_LOADED, "PCI scan + MMIO + RX/TX rings" },
    { "ahci",    "AHCI SATA",           "Storage", DRIVER_STATUS_LOADED, "PCI scan + ABAR + DMA" },
    { "ac97",    "AC97 Audio (PCM-out)", "Audio",   DRIVER_STATUS_LOADED, "Playback only" },
    { "ps2-kbd", "PS/2 Keyboard",       "Input",   DRIVER_STATUS_LOADED, "Set 1 scancodes, US-QWERTY" },
    { "ps2-mouse","PS/2 Mouse",         "Input",   DRIVER_STATUS_LOADED, "3-byte packets" },
    { "rtc",     "MC146818 RTC",        "Clock",   DRIVER_STATUS_LOADED, "BCD/binary detect" },
    { "vga",     "VGA Text Mode",       "Display", DRIVER_STATUS_LOADED, "80x25 at 0xB8000" },
    { "framebuffer","VESA Framebuffer", "Display", DRIVER_STATUS_LOADED, "16/24/32 bpp, double-buffered" },
    { "ath9k",   "Atheros ath9k WiFi",  "Network", DRIVER_STATUS_STUB, "Not implemented" },
    { "iwlwifi", "Intel iwlwifi WiFi",  "Network", DRIVER_STATUS_STUB, "Not implemented" },
    { "rtw88",   "Realtek rtw88 WiFi",  "Network", DRIVER_STATUS_STUB, "Not implemented" },
    { "ac97-in", "AC97 Mic Capture",    "Audio",   DRIVER_STATUS_STUB, "Not implemented" },
    { "battery", "ACPI Battery",        "Power",   DRIVER_STATUS_STUB, "Simulated" },
    { "temp",    "CPU Thermal Sensor",  "Sensor",  DRIVER_STATUS_STUB, "Simulated" },
    { "usb",     "USB Stack (xHCI)",    "Bus",     DRIVER_STATUS_MISSING_DEP, "Not implemented" },
    { "gpu",     "GPU Driver (Intel/AMD)","Display",DRIVER_STATUS_MISSING_DEP, "Not implemented" },
    { "tls",     "TLS 1.2 (mbedTLS port)","Crypto", DRIVER_STATUS_MISSING_DEP, "Partial - no RSA/ECDH" },
};

static const int drivers_count = sizeof(drivers) / sizeof(drivers[0]);

/* ===== PREINSTALLED APP CATALOG ===== */
typedef enum {
    APP_KIND_NATIVE  = 0,
    APP_KIND_BUNDLED = 1,
} app_kind_t;

struct preinstalled_app {
    const char* id;
    const char* name;
    const char* icon;
    const char* category;
    const char* version;
    const char* path;
    const char* description;
    app_kind_t kind;
    uint32_t size_kb;
};

static const struct preinstalled_app preinstalled[] = {
    { "libreoffice-writer", "LibreOffice Writer", "writer",
      "Productivity", "7.6.5", "/opt/libreoffice/writer",
      "Requires Linux compat layer", APP_KIND_BUNDLED, 8 * 1024 },
    { "libreoffice-calc", "LibreOffice Calc", "calc",
      "Productivity", "7.6.5", "/opt/libreoffice/calc",
      "Requires Linux compat layer", APP_KIND_BUNDLED, 8 * 1024 },
    { "libreoffice-impress", "LibreOffice Impress", "impress",
      "Productivity", "7.6.5", "/opt/libreoffice/impress",
      "Requires Linux compat layer", APP_KIND_BUNDLED, 8 * 1024 },
    { "lestra-editor", "Lestra Editor", "editor",
      "Productivity", "1.0", NULL,
      "Native in-kernel editor", APP_KIND_NATIVE, 24 },

    { "kdenlive", "Kdenlive Video Editor", "video",
      "Multimedia", "23.08.4", "/opt/kdenlive",
      "Requires Linux compat layer", APP_KIND_BUNDLED, 350 * 1024 },
    { "obs-studio", "OBS Studio", "video",
      "Multimedia", "30.0.2", "/opt/obs-studio",
      "Requires Linux compat layer", APP_KIND_BUNDLED, 250 * 1024 },
    { "vlc", "VLC Media Player", "media",
      "Multimedia", "3.0.20", "/opt/vlc",
      "Requires Linux compat layer", APP_KIND_BUNDLED, 120 * 1024 },
    { "lestra-media", "Lestra Media", "media",
      "Multimedia", "1.0", NULL,
      "Native UI, no codecs", APP_KIND_NATIVE, 16 },

    { "lestra-browser", "Lestra Browser", "browser",
      "Internet", "1.0", NULL,
      "HTTP/1.0 only, no TLS", APP_KIND_NATIVE, 32 },
    { "lestra-mail", "Lestra Mail", "mail",
      "Internet", "1.0", NULL,
      "No IMAP/SMTP yet", APP_KIND_NATIVE, 20 },

    { "lestra-terminal", "Terminal", "terminal",
      "System", "1.0", NULL,
      "In-kernel shell", APP_KIND_NATIVE, 32 },
    { "lestra-files", "Files", "files",
      "System", "1.0", NULL,
      "In-memory VFS only", APP_KIND_NATIVE, 24 },
    { "lestra-ailab", "AI Lab", "ai",
      "System", "1.0", NULL,
      "Multi-provider AI chat", APP_KIND_NATIVE, 40 },
    { "lestra-settings", "Settings", "settings",
      "System", "1.0", NULL,
      "Themes, AI keys, network, packages", APP_KIND_NATIVE, 28 },
    { "lestra-calendar", "Calendar", "calendar",
      "Utilities", "1.0", NULL,
      "RTC date display", APP_KIND_NATIVE, 16 },
    { "lestra-photos", "Photos", "photos",
      "Utilities", "1.0", NULL,
      "No image library", APP_KIND_NATIVE, 16 },
};

static const int preinstalled_count = sizeof(preinstalled) / sizeof(preinstalled[0]);

/* ----- driver API ----- */
void pkg_driver_list(void) {
    printk("\nDriver catalog (%d):\n", drivers_count);
    printk("%s %s %s %s\n", "ID", "NAME", "CATEGORY", "STATUS");
    printk("----------------------------------------------------------\n");
    for (int i = 0; i < drivers_count; i++) {
        const char* status_str = "unknown";
        switch (drivers[i].status) {
            case DRIVER_STATUS_LOADED: status_str = "loaded"; break;
            case DRIVER_STATUS_STUB: status_str = "stub"; break;
            case DRIVER_STATUS_MISSING_DEP: status_str = "missing-dep"; break;
        }
        printk("%s %s %s %s\n", drivers[i].id, drivers[i].name, drivers[i].category, status_str);
    }
    printk("\n");
}

const struct driver_entry* pkg_driver_get(int idx) {
    if (idx < 0 || idx >= drivers_count) return NULL;
    return &drivers[idx];
}

int pkg_driver_count(void) { return drivers_count; }

/* ----- preinstalled app API ----- */
void pkg_preinstalled_list(void) {
    printk("\nPre-installed apps (%d):\n", preinstalled_count);
    printk("%s %s %s %s %s\n", "ID", "NAME", "CATEGORY", "KIND", "VERSION");
    printk("----------------------------------------------------------------\n");
    for (int i = 0; i < preinstalled_count; i++) {
        const char* kind_str = preinstalled[i].kind == APP_KIND_NATIVE ? "native" : "bundled";
        printk("%s %s %s %s %s\n",
               preinstalled[i].id, preinstalled[i].name,
               preinstalled[i].category, kind_str, preinstalled[i].version);
    }
    printk("\n");
}

const struct preinstalled_app* pkg_preinstalled_get(int idx) {
    if (idx < 0 || idx >= preinstalled_count) return NULL;
    return &preinstalled[idx];
}

const struct preinstalled_app* pkg_preinstalled_find(const char* id) {
    if (!id) return NULL;
    for (int i = 0; i < preinstalled_count; i++) {
        if (preinstalled[i].id && strcmp(preinstalled[i].id, id) == 0) {
            return &preinstalled[i];
        }
    }
    return NULL;
}

int pkg_preinstalled_count(void) { return preinstalled_count; }

/* Launcher for preinstalled apps - called by GUI when icon clicked */
const char* pkg_preinstalled_launch(const char* id) {
    const struct preinstalled_app* app = pkg_preinstalled_find(id);
    if (!app) return "App not found.";

    switch (app->kind) {
        case APP_KIND_NATIVE:
            printk("pkg: launching native app '%s'\n", app->name);
            return "Launched (native in-kernel widget).";

        case APP_KIND_BUNDLED:
            printk("pkg: bundle '%s' at %s - requires Linux compat\n",
                   app->name, app->path ? app->path : "(no path)");
            return "BUNDLE PRE-STAGED. LestraOS has no Linux ABI compat layer yet, so this app cannot run on bare metal. The bundle is in /opt/ and can be copied to a Linux host. To enable native execution, port a Linux compatibility layer.";

        default:
            return "Unknown app kind.";
    }
}
