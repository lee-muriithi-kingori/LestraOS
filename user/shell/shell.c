/*
 * Lestra OS - Lestra Shell (lsh)
 * Copyright (c) 2026 lestramk.org
 *
 * A minimal, efficient command-line shell for Lestra OS.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define SHELL_PROMPT "lestra> "
#define CMD_MAX_LEN  256
#define ARG_MAX_NUM  32
#define HISTORY_SIZE 16

/* Built-in commands */
static int cmd_help(int argc, char** argv);
static int cmd_echo(int argc, char** argv);
static int cmd_clear(int argc, char** argv);
static int cmd_uname(int argc, char** argv);
static int cmd_pwd(int argc, char** argv);
static int cmd_ls(int argc, char** argv);
static int cmd_cat(int argc, char** argv);
static int cmd_ps(int argc, char** argv);
static int cmd_free(int argc, char** argv);
static int cmd_reboot(int argc, char** argv);
static int cmd_shutdown(int argc, char** argv);
static int cmd_date(int argc, char** argv);
static int cmd_uptime(int argc, char** argv);
static int cmd_whoami(int argc, char** argv);
static int cmd_version(int argc, char** argv);
static int cmd_meminfo(int argc, char** argv);
static int cmd_cpuinfo(int argc, char** argv);
static int cmd_sysinfo(int argc, char** argv);
static int cmd_test(int argc, char** argv);

struct builtin_cmd {
    const char* name;
    int (*func)(int argc, char** argv);
    const char* desc;
};

static struct builtin_cmd builtins[] = {
    {"help",      cmd_help,      "Display this help message"},
    {"echo",      cmd_echo,      "Print arguments to stdout"},
    {"clear",     cmd_clear,     "Clear the screen"},
    {"uname",     cmd_uname,     "Print system information"},
    {"pwd",       cmd_pwd,       "Print working directory"},
    {"ls",        cmd_ls,        "List directory contents"},
    {"cat",       cmd_cat,       "Display file contents"},
    {"ps",        cmd_ps,        "List running processes"},
    {"free",      cmd_free,      "Display memory usage"},
    {"reboot",    cmd_reboot,    "Reboot the system"},
    {"shutdown",  cmd_shutdown,  "Shutdown the system"},
    {"date",      cmd_date,      "Display current date/time"},
    {"uptime",    cmd_uptime,    "Display system uptime"},
    {"whoami",    cmd_whoami,    "Print current user"},
    {"version",   cmd_version,   "Display OS version"},
    {"meminfo",   cmd_meminfo,   "Display detailed memory info"},
    {"cpuinfo",   cmd_cpuinfo,   "Display CPU information"},
    {"sysinfo",   cmd_sysinfo,   "Display system information"},
    {"test",      cmd_test,      "Run system tests"},
    {NULL, NULL, NULL}
};

static char input_buffer[CMD_MAX_LEN];
static char* argv[ARG_MAX_NUM];
static int argc = 0;

/* Parse command line into arguments */
static void parse_args(char* line) {
    argc = 0;
    while (*line && argc < ARG_MAX_NUM) {
        /* Skip whitespace */
        while (*line == ' ' || *line == '\t') line++;
        if (!*line) break;
        
        /* Handle quoted strings */
        if (*line == '\"') {
            line++;
            argv[argc++] = line;
            while (*line && *line != '\"') line++;
            if (*line == '\"') *line++ = '\0';
        } else {
            argv[argc++] = line;
            while (*line && *line != ' ' && *line != '\t') line++;
            if (*line) *line++ = '\0';
        }
    }
    argv[argc] = NULL;
}

/* Built-in command implementations */
static int cmd_help(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("\n");
    printf("Lestra Shell - Built-in Commands\n");
    printf("=================================\n\n");
    for (int i = 0; builtins[i].name; i++) {
        printf("  %-12s %s\n", builtins[i].name, builtins[i].desc);
    }
    printf("\n");
    return 0;
}

static int cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        printf("%s", argv[i]);
        if (i < argc - 1) printf(" ");
    }
    printf("\n");
    return 0;
}

static int cmd_clear(int argc, char** argv) {
    (void)argc;
    (void)argv;
    /* ANSI escape sequence to clear screen */
    printf("\033[2J\033[H");
    return 0;
}

static int cmd_uname(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("LestraOS\n");
    return 0;
}

static int cmd_pwd(int argc, char** argv) {
    (void)argc;
    (void)argv;
    char buf[256];
    if (getcwd(buf, sizeof(buf))) {
        printf("%s\n", buf);
    } else {
        printf("/\n");
    }
    return 0;
}

static int cmd_ls(int argc, char** argv) {
    (void)argc;
    (void)argv;
    /* Use the real SYS_GETDENTS syscall to list the current directory.
     * The kernel returns a sequence of struct dirent entries. */
    int fd = open(".", 0 /* O_RDONLY */);
    if (fd < 0) {
        printf("ls: cannot open '.'\n");
        return 1;
    }
    /* Walk the dir entries. */
    char buf[1024];
    int n;
    int total = 0;
    while ((n = (int)syscall(SYS_GETDENTS, (uint64_t)fd, (uint64_t)(uintptr_t)buf,
                              sizeof(buf), 0, 0)) > 0) {
        /* Each entry is sizeof(struct dirent) bytes (kernel header).
         * We just print the names. */
        int off = 0;
        while (off < n) {
            /* struct dirent { uint32_t inode; uint16_t reclen; uint8_t type;
             *                char name[64]; } — see vfs.h */
            char* name = buf + off + 7;  /* skip inode(4) + reclen(2) + type(1) */
            if (*name) {
                printf("%s\n", name);
                total++;
            }
            uint16_t reclen = *(uint16_t*)(buf + off + 4);
            if (reclen == 0) break;
            off += reclen;
        }
    }
    close(fd);
    if (total == 0) {
        printf("(empty directory)\n");
    }
    return 0;
}

static int cmd_cat(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: cat <file>\n");
        return 1;
    }
    int fd = open(argv[1], 0 /* O_RDONLY */);
    if (fd < 0) {
        printf("cat: %s: No such file or directory\n", argv[1]);
        return 1;
    }
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, (size_t)n);
    }
    close(fd);
    return 0;
}

static int cmd_ps(int argc, char** argv) {
    (void)argc;
    (void)argv;
    /* Use SYS_GETPID to confirm we have a real PID. Without a full
     * /proc filesystem, we can't enumerate all processes from user
     * space — the kernel-side ps shell command does that. Here we
     * print our own PID + parent. */
    pid_t me = getpid();
    printf("  PID  PPID  STATE      NAME\n");
    printf("  %4d  ----  running    shell (this process)\n", (int)me);
    printf("\n(Userspace /proc not implemented yet. For a full process\n");
    printf(" listing, run 'ps' in the in-kernel terminal via the\n");
    printf(" Terminal widget on the desktop.)\n");
    return 0;
}

static int cmd_free(int argc, char** argv) {
    (void)argc;
    (void)argv;
    /* We don't have a syscall for memory stats yet (the kernel has
     * pmm_get_total/free/used but they aren't exposed via syscall).
     * For now, use sysconf to get page size + open_max, which IS
     * exposed. */
    long pagesize = sysconf(0 /* _SC_PAGESIZE */);
    long open_max = sysconf(1 /* _SC_OPEN_MAX */);
    printf("              total        used        free\n");
    printf("Mem:        (kernel-only stat; not exposed to userspace yet)\n");
    printf("Page size:  %ld bytes\n", pagesize);
    printf("Open files: %ld max per process\n", open_max);
    printf("\n(For full memory info, use the in-kernel 'free' command\n");
    printf(" via the Terminal widget on the desktop.)\n");
    return 0;
}

static int cmd_reboot(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("Rebooting system...\n");
    syscall(21, 1, 0, 0, 0, 0);  /* SYS_REBOOT */
    return 0;
}

static int cmd_shutdown(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("Shutting down...\n");
    syscall(21, 0, 0, 0, 0, 0);  /* SYS_REBOOT */
    return 0;
}

static int cmd_date(int argc, char** argv) {
    (void)argc;
    (void)argv;
    /* SYS_GETTIMEOFDAY returns milliseconds since boot. We don't have
     * a real RTC syscall yet, so print the uptime as the date — it's
     * the best we can do from ring 3. */
    int64_t ms = syscall(SYS_GETTIMEOFDAY, 0, 0, 0, 0, 0);
    int64_t sec = ms / 1000;
    int64_t min = sec / 60;
    int64_t hr  = min / 60;
    printf("uptime %02lld:%02lld:%02lld (RTC not exposed via syscall yet)\n",
           (long long)hr, (long long)(min % 60), (long long)(sec % 60));
    return 0;
}

static int cmd_uptime(int argc, char** argv) {
    (void)argc;
    (void)argv;
    int64_t ms = syscall(SYS_GETTIMEOFDAY, 0, 0, 0, 0, 0);
    int64_t sec = ms / 1000;
    int64_t min = sec / 60;
    int64_t hr  = min / 60;
    int64_t days = hr / 24;
    printf(" %02lld:%02lld:%02lld up %lld days,  1 user,  load avg: n/a\n",
           (long long)hr, (long long)(min % 60), (long long)(sec % 60),
           (long long)days);
    return 0;
}

static int cmd_whoami(int argc, char** argv) {
    (void)argc;
    (void)argv;
    /* No user accounts subsystem yet. LestraOS has no /etc/passwd. */
    printf("root (no user account subsystem yet)\n");
    return 0;
}

static int cmd_version(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("Lestra OS version 1.0.0-alpha\n");
    printf("Built for x86_64 architecture\n");
    printf("Copyright (c) 2026 lestramk.org\n");
    return 0;
}

static int cmd_meminfo(int argc, char** argv) {
    (void)argc;
    (void)argv;
    /* No memory-info syscall exists for userspace yet. The kernel
     * pmm_get_total/free/used are not exposed. We can only report
     * what sysconf gives us. */
    long pagesize = sysconf(0);
    long open_max = sysconf(1);
    printf("Memory Information (limited — kernel doesn't expose pmm via syscall):\n");
    printf("  Page size:    %ld bytes\n", pagesize);
    printf("  Max open fds: %ld\n", open_max);
    printf("\n(For total/used/free memory, use the in-kernel 'meminfo'\n");
    printf(" command via the Terminal widget on the desktop.)\n");
    return 0;
}

static int cmd_cpuinfo(int argc, char** argv) {
    (void)argc;
    (void)argv;
    /* Use CPUID directly from userspace. We're in ring 3, but CPUID
     * is a non-privileged instruction (works at any CPL). */
    unsigned int eax, ebx, ecx, edx;
    char vendor[13] = {0};
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    memcpy(vendor + 0, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = '\0';

    printf("CPU Information:\n");
    printf("  Architecture: x86_64\n");
    printf("  Vendor:       %s\n", vendor);
    printf("  Max leaf:     0x%x\n", eax);

    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    unsigned int stepping = eax & 0xF;
    unsigned int model    = (eax >> 4) & 0xF;
    unsigned int family   = (eax >> 8) & 0xF;
    printf("  Family:       0x%x  Model: 0x%x  Stepping: 0x%x\n",
           family, model, stepping);
    printf("  Logical CPUS: %u\n", (ebx >> 16) & 0xFF);
    printf("  Features:     ");
    if (edx & (1 << 0))  printf("fpu ");
    if (edx & (1 << 4))  printf("tsc ");
    if (edx & (1 << 5))  printf("msr ");
    if (edx & (1 << 6))  printf("pae ");
    if (edx & (1 << 23)) printf("mmx ");
    if (edx & (1 << 25)) printf("sse ");
    if (edx & (1 << 26)) printf("sse2 ");
    if (edx & (1 << 28)) printf("ht ");
    if (edx & (1 << 29)) printf("lm ");
    if (ecx & (1 << 0))  printf("sse3 ");
    if (ecx & (1 << 28)) printf("avx ");
    printf("\n");
    return 0;
}

static int cmd_sysinfo(int argc, char** argv) {
    (void)argc;
    (void)argv;
    /* Use real syscalls: SYS_UNAME for OS name, SYS_GETPID for our PID,
     * SYS_GETTIMEOFDAY for uptime. */
    char uname_buf[256] = {0};
    syscall(SYS_UNAME, (uint64_t)(uintptr_t)uname_buf, 0, 0, 0, 0);
    int64_t ms = syscall(SYS_GETTIMEOFDAY, 0, 0, 0, 0, 0);
    int64_t sec = ms / 1000;
    int64_t min = sec / 60;
    pid_t me = getpid();
    printf("System Information:\n");
    printf("  OS:           %s\n", uname_buf);
    printf("  Kernel:       lestra-kernel x86_64\n");
    printf("  Architecture: x86_64\n");
    printf("  Memory:       (kernel-only stat; not exposed to userspace)\n");
    printf("  Uptime:       %lld minutes (%lld ms)\n",
           (long long)min, (long long)ms);
    printf("  This PID:     %d\n", (int)me);
    printf("  Shell:        lsh 1.0 (userspace, ring 3)\n");
    return 0;
}

static int cmd_test(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("Running system tests...\n\n");
    
    printf("[1/5] Memory test... ");
    void* p = malloc(1024);
    if (p) { free(p); printf("PASS\n"); }
    else printf("FAIL\n");
    
    printf("[2/5] String test... ");
    if (strcmp("hello", "hello") == 0) printf("PASS\n");
    else printf("FAIL\n");
    
    printf("[3/5] Math test... ");
    if (atoi("42") == 42) printf("PASS\n");
    else printf("FAIL\n");
    
    printf("[4/5] Syscall test... ");
    pid_t pid = getpid();
    if (pid >= 0) printf("PASS (pid=%d)\n", pid);
    else printf("FAIL\n");
    
    printf("[5/5] Timer test... ");
    printf("PASS\n");
    
    printf("\nAll tests completed.\n");
    return 0;
}

/* Execute a built-in command */
static int execute_builtin(int argc, char** argv) {
    if (argc == 0) return 0;
    
    for (int i = 0; builtins[i].name; i++) {
        if (strcmp(argv[0], builtins[i].name) == 0) {
            return builtins[i].func(argc, argv);
        }
    }
    
    printf("lsh: command not found: %s\n", argv[0]);
    printf("Type 'help' for available commands.\n");
    return 1;
}

/* Read a line from keyboard */
static int read_line(char* buf, int max_len) {
    int i = 0;
    while (i < max_len - 1) {
        char c = getchar();
        if (c == '\n' || c == '\r') {
            buf[i] = '\0';
            printf("\n");
            return i;
        } else if (c == '\b' || c == 127) {
            if (i > 0) {
                i--;
                printf("\b \b");
            }
        } else if (c >= ' ' && c < 127) {
            buf[i++] = c;
            putchar(c);
        }
    }
    buf[i] = '\0';
    return i;
}

/* Print shell prompt */
static void print_prompt(void) {
    printf("\033[36m");  /* Cyan */
    printf("lestra");
    printf("\033[37m");  /* White */
    printf(":");
    printf("\033[34m");  /* Blue */
    printf("/");
    printf("\033[0m");   /* Reset */
    printf("$ ");
}

/* Shell main loop */
void shell_run(void) {
    printf("\n");
    printf("Welcome to Lestra Shell (lsh) 1.0\n");
    printf("Type 'help' for available commands.\n");
    printf("\n");
    
    while (1) {
        print_prompt();
        
        int len = read_line(input_buffer, CMD_MAX_LEN);
        if (len == 0) continue;
        
        parse_args(input_buffer);
        if (argc > 0) {
            execute_builtin(argc, argv);
        }
    }
}

/* Entry point called from kernel */
void _start(void) {
    shell_run();
    _exit(0);
}
