/*
 * Lestra OS - Lestra Shell (lsh)
 * Copyright (c) 2026 lestramk.org
 *
 * A minimal, functional POSIX-ish shell that uses real syscalls
 * to interact with the filesystem and /proc. All hardcoded outputs
 * have been replaced with actual open/read/getdents/getcwd/etc.
 * calls so the shell works with the real VFS.
 *
 * Supported commands:
 *   help, echo, clear, uname, pwd, cd, ls, cat, ps, free, meminfo,
 *   cpuinfo, date, uptime, whoami, version, sysinfo, touch, rm, mkdir,
 *   reboot, shutdown, test
 *
 * Pipes (|) and background (&) are supported.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

/* O_CREAT and O_APPEND flags — these come from the kernel's vfs.h
 * but we mirror them here since the libc doesn't have fcntl.h yet. */
#define O_CREAT     0x0010
#define O_APPEND    0x0040
#define O_RDONLY    0x0001

#define SHELL_PROMPT "lestra> "
#define CMD_MAX_LEN  256
#define ARG_MAX_NUM  32
#define MAX_PIPELINE 8

/* ---- Built-in command declarations ---- */
static int cmd_help(int argc, char** argv);
static int cmd_echo(int argc, char** argv);
static int cmd_clear(int argc, char** argv);
static int cmd_uname(int argc, char** argv);
static int cmd_pwd(int argc, char** argv);
static int cmd_cd(int argc, char** argv);
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
static int cmd_touch(int argc, char** argv);
static int cmd_rm(int argc, char** argv);
static int cmd_mkdir(int argc, char** argv);

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
    {"cd",        cmd_cd,        "Change working directory"},
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
    {"touch",     cmd_touch,     "Create an empty file"},
    {"rm",        cmd_rm,        "Remove a file"},
    {"mkdir",     cmd_mkdir,     "Create a directory"},
    {NULL, NULL, NULL}
};

static char input_buffer[CMD_MAX_LEN];
static char* argv[ARG_MAX_NUM];
static int argc = 0;
static int last_bg_pid = 0;

/* Current working directory cache — updated by cd and used by
 * the prompt. We keep our own copy so getcwd isn't called every
 * time we print the prompt. */
static char cwd_buf[256] = "/";

__attribute__((unused))
static void parse_args(char* line) {
    argc = 0;
    while (*line && argc < ARG_MAX_NUM) {
        while (*line == ' ' || *line == '\t') line++;
        if (!*line) break;
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

static int is_builtin(const char* name) {
    for (int i = 0; builtins[i].name; i++) {
        if (strcmp(name, builtins[i].name) == 0) return 1;
    }
    return 0;
}

static int run_builtin(int argc, char** argv) {
    for (int i = 0; builtins[i].name; i++) {
        if (strcmp(argv[0], builtins[i].name) == 0) {
            return builtins[i].func(argc, argv);
        }
    }
    return 1;
}

/* ---- Helper: read a file from /proc and print its contents ---- */
static int read_proc_file(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("Error: cannot open %s\n", path);
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

/* ---- Built-in command implementations ---- */

static int cmd_help(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("\n");
    printf("Lestra Shell - Built-in Commands\n");
    printf("=================================\n\n");
    for (int i = 0; builtins[i].name; i++) {
        printf("  %-12s %s\n", builtins[i].name, builtins[i].desc);
    }
    printf("\n  Pipes:       cmd1 | cmd2\n");
    printf("  Background:  cmd &\n");
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
    (void)argc; (void)argv;
    printf("\033[2J\033[H");
    return 0;
}

static int cmd_uname(int argc, char** argv) {
    (void)argc; (void)argv;
    char buf[256] = {0};
    syscall(SYS_UNAME, (uint64_t)(uintptr_t)buf, 0, 0, 0, 0);
    printf("%s\n", buf);
    return 0;
}

static int cmd_pwd(int argc, char** argv) {
    (void)argc; (void)argv;
    /* Use getcwd syscall to show the real working directory. */
    char buf[256];
    if (getcwd(buf, sizeof(buf))) {
        printf("%s\n", buf);
        /* Also update our cached cwd for the prompt. */
        strncpy(cwd_buf, buf, sizeof(cwd_buf) - 1);
    } else {
        printf("/\n");
    }
    return 0;
}

static int cmd_cd(int argc, char** argv) {
    if (argc < 2) {
        /* cd with no args → go to root (like home dir). */
        if (chdir("/") == 0) {
            strncpy(cwd_buf, "/", sizeof(cwd_buf) - 1);
        } else {
            printf("cd: cannot change to /\n");
        }
        return 0;
    }
    if (chdir(argv[1]) == 0) {
        /* Update our cached cwd for the prompt. */
        char buf[256];
        if (getcwd(buf, sizeof(buf))) {
            strncpy(cwd_buf, buf, sizeof(cwd_buf) - 1);
            cwd_buf[sizeof(cwd_buf) - 1] = '\0';
        }
    } else {
        printf("cd: %s: No such directory\n", argv[1]);
        return 1;
    }
    return 0;
}

static int cmd_ls(int argc, char** argv) {
    const char* path = (argc >= 2) ? argv[1] : "/";
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("ls: cannot open '%s'\n", path);
        return 1;
    }
    char buf[2048];
    int total = 0;
    int n;
    while ((n = (int)syscall(SYS_GETDENTS, (uint64_t)fd,
                              (uint64_t)(uintptr_t)buf,
                              sizeof(buf), 0, 0)) > 0) {
        int off = 0;
        while (off < n) {
            /* struct dirent layout: inode(4) + reclen(2) + type(1) + name(64) */
            char* name = buf + off + 7;
            if (*name) {
                /* Skip "." and ".." entries for cleaner output. */
                if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
                    printf("%s\n", name);
                    total++;
                }
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
    int fd = open(argv[1], O_RDONLY);
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

/* ps: read /proc/ps which generates a process listing */
static int cmd_ps(int argc, char** argv) {
    (void)argc; (void)argv;
    return read_proc_file("/proc/ps");
}

/* free: read /proc/meminfo for real memory stats */
static int cmd_free(int argc, char** argv) {
    (void)argc; (void)argv;
    return read_proc_file("/proc/meminfo");
}

static int cmd_reboot(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("Rebooting system...\n");
    syscall(SYS_REBOOT, 1, 0, 0, 0, 0);
    return 0;
}

static int cmd_shutdown(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("Shutting down...\n");
    syscall(SYS_REBOOT, 0, 0, 0, 0, 0);
    return 0;
}

/* date: show time since boot formatted as HH:MM:SS.
 * The kernel has no real-time clock (no RTC driver), so
 * gettimeofday() returns ms since boot. We format it as
 * a relative timestamp. */
static int cmd_date(int argc, char** argv) {
    (void)argc; (void)argv;
    int64_t ms = syscall(SYS_GETTIMEOFDAY, 0, 0, 0, 0, 0);
    int64_t sec = ms / 1000;
    int64_t min = sec / 60;
    int64_t hr  = min / 60;
    printf("Boot time: %02lld:%02lld:%02lld (no RTC; relative to boot)\n",
           (long long)hr, (long long)(min % 60), (long long)(sec % 60));
    return 0;
}

static int cmd_uptime(int argc, char** argv) {
    (void)argc; (void)argv;
    int64_t ms = syscall(SYS_GETTIMEOFDAY, 0, 0, 0, 0, 0);
    int64_t sec = ms / 1000;
    int64_t min = sec / 60;
    int64_t hr  = min / 60;
    int64_t days = hr / 24;
    printf(" %02lld:%02lld:%02lld up %lld days\n",
           (long long)(hr % 24), (long long)(min % 60), (long long)(sec % 60),
           (long long)days);
    return 0;
}

static int cmd_whoami(int argc, char** argv) {
    (void)argc; (void)argv;
    /* No user management yet; always root. */
    printf("root\n");
    return 0;
}

static int cmd_version(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("Lestra OS version 1.0.0-alpha\n");
    printf("Built for x86_64 architecture\n");
    printf("Copyright (c) 2026 lestramk.org\n");
    return 0;
}

/* meminfo: read /proc/meminfo for detailed memory stats */
static int cmd_meminfo(int argc, char** argv) {
    (void)argc; (void)argv;
    return read_proc_file("/proc/meminfo");
}

/* cpuinfo: read /proc/cpuinfo for real CPU info */
static int cmd_cpuinfo(int argc, char** argv) {
    (void)argc; (void)argv;
    return read_proc_file("/proc/cpuinfo");
}

static int cmd_sysinfo(int argc, char** argv) {
    (void)argc; (void)argv;
    char uname_buf[256] = {0};
    syscall(SYS_UNAME, (uint64_t)(uintptr_t)uname_buf, 0, 0, 0, 0);
    int64_t ms = syscall(SYS_GETTIMEOFDAY, 0, 0, 0, 0, 0);
    int64_t sec = ms / 1000;
    pid_t me = getpid();
    printf("System Information:\n");
    printf("  OS:           %s\n", uname_buf);
    printf("  Kernel:       lestra-kernel x86_64\n");
    printf("  Architecture: x86_64\n");
    printf("  Uptime:       %lld seconds\n", (long long)sec);
    printf("  This PID:     %d\n", (int)me);
    printf("  Shell:        lsh 2.0 (userspace, ring 3)\n");
    return 0;
}

static int cmd_test(int argc, char** argv) {
    (void)argc; (void)argv;
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
    printf("[5/5] Pipe test... ");
    int pfds[2];
    if (pipe(pfds) == 0) {
        write(pfds[1], "ok", 2);
        char rbuf[4];
        int n = (int)read(pfds[0], rbuf, sizeof(rbuf));
        close(pfds[0]);
        close(pfds[1]);
        if (n == 2 && rbuf[0] == 'o' && rbuf[1] == 'k') printf("PASS\n");
        else printf("FAIL (read returned %d)\n", n);
    } else {
        printf("FAIL (pipe returned -1)\n");
    }
    printf("\nAll tests completed.\n");
    return 0;
}

/* touch: create an empty file using open with O_CREAT */
static int cmd_touch(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: touch <file>\n");
        return 1;
    }
    int fd = open(argv[1], O_CREAT);
    if (fd < 0) {
        printf("touch: cannot create %s\n", argv[1]);
        return 1;
    }
    close(fd);
    return 0;
}

/* rm: remove a file using the unlink syscall */
static int cmd_rm(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: rm <file>\n");
        return 1;
    }
    if (unlink(argv[1]) != 0) {
        printf("rm: cannot remove %s\n", argv[1]);
        return 1;
    }
    return 0;
}

/* mkdir: create a directory using the mkdir syscall */
static int cmd_mkdir(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: mkdir <dir>\n");
        return 1;
    }
    if (mkdir(argv[1], 0755) != 0) {
        printf("mkdir: cannot create %s\n", argv[1]);
        return 1;
    }
    return 0;
}

/* ---- Line input and prompt ---- */

static void read_line(char* buf, int max_len) {
    int i = 0;
    while (i < max_len - 1) {
        char c = getchar();
        if (c == '\n' || c == '\r') {
            buf[i] = '\0';
            printf("\n");
            return;
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
}

static void print_prompt(void) {
    /* Show the current working directory in the prompt,
     * truncated if too long (like bash's \W prompt). */
    printf("\033[36m");
    printf("lestra");
    printf("\033[37m");
    printf(":");
    printf("\033[34m");
    /* Simplify display: show just the last component of cwd,
     * or "/" if at root. */
    const char* display = cwd_buf;
    if (strcmp(display, "/") != 0) {
        /* Find the last '/' and show what follows it. */
        const char* last_slash = strrchr(display, '/');
        if (last_slash && last_slash[1]) {
            display = last_slash + 1;
        }
    }
    printf("%s", display);
    printf("\033[0m");
    printf("$ ");
}

/* Execute a single command (not a pipeline stage).
 * Returns the PID of the child process, or -1 on error. */
__attribute__((unused))
static int exec_single(char** cmd_argv) {
    if (!cmd_argv || !cmd_argv[0]) return -1;
    if (is_builtin(cmd_argv[0])) {
        run_builtin(argc, cmd_argv);
        return -1;  /* builtins run in-process, no child */
    }
    pid_t pid = fork();
    if (pid == 0) {
        execvp(cmd_argv[0], cmd_argv);
        printf("lsh: command not found: %s\n", cmd_argv[0]);
        _exit(1);
    }
    return (int)pid;
}

/* Parse a single command string into argv array. Returns argc. */
static int parse_single_cmd(char* cmd_str, char** out_argv) {
    int ac = 0;
    while (*cmd_str && ac < ARG_MAX_NUM - 1) {
        while (*cmd_str == ' ' || *cmd_str == '\t') cmd_str++;
        if (!*cmd_str) break;
        out_argv[ac++] = cmd_str;
        while (*cmd_str && *cmd_str != ' ' && *cmd_str != '\t') cmd_str++;
        if (*cmd_str) *cmd_str++ = '\0';
    }
    out_argv[ac] = NULL;
    return ac;
}

/* Execute a pipeline: cmd1 | cmd2 | ... | cmdN */
static void exec_pipeline(char* line, int background) {
    char* cmds[MAX_PIPELINE];
    int ncmds = 0;

    cmds[ncmds++] = line;
    char* p = line;
    while (*p && ncmds < MAX_PIPELINE) {
        if (*p == '|') {
            *p = '\0';
            p++;
            while (*p == ' ') p++;
            cmds[ncmds++] = p;
        } else {
            p++;
        }
    }

    if (ncmds == 1) {
        char* single_argv[ARG_MAX_NUM];
        parse_single_cmd(cmds[0], single_argv);
        if (!single_argv[0]) return;
        if (is_builtin(single_argv[0])) {
            run_builtin(parse_single_cmd(cmds[0], single_argv), single_argv);
            return;
        }
        pid_t pid = fork();
        if (pid == 0) {
            execvp(single_argv[0], single_argv);
            printf("lsh: command not found: %s\n", single_argv[0]);
            _exit(1);
        }
        if (!background) {
            int status;
            waitpid(pid, &status, 0);
        } else {
            last_bg_pid = (int)pid;
            printf("[bg] %d\n", (int)pid);
        }
        return;
    }

    /* Multi-stage pipeline */
    int prev_read_fd = -1;
    int last_pid = -1;

    for (int i = 0; i < ncmds; i++) {
        int pfd[2];
        if (i < ncmds - 1) {
            if (pipe(pfd) < 0) {
                printf("lsh: pipe failed\n");
                return;
            }
        }

        char* cmd_argv[ARG_MAX_NUM];
        parse_single_cmd(cmds[i], cmd_argv);
        if (!cmd_argv[0]) continue;

        pid_t pid = fork();
        if (pid == 0) {
            if (prev_read_fd >= 0) {
                dup2(prev_read_fd, STDIN_FILENO);
                close(prev_read_fd);
            }
            if (i < ncmds - 1) {
                close(pfd[0]);
                dup2(pfd[1], STDOUT_FILENO);
                close(pfd[1]);
            }
            execvp(cmd_argv[0], cmd_argv);
            printf("lsh: command not found: %s\n", cmd_argv[0]);
            _exit(1);
        }

        if (prev_read_fd >= 0) close(prev_read_fd);
        if (i < ncmds - 1) {
            close(pfd[1]);
            prev_read_fd = pfd[0];
        }
        last_pid = (int)pid;
    }

    if (!background && last_pid > 0) {
        for (int i = 0; i < ncmds; i++) {
            int status;
            waitpid(-1, &status, 0);
        }
    } else if (background) {
        last_bg_pid = last_pid;
        printf("[bg] %d\n", last_pid);
    }
}

/* Reap any finished background children (non-blocking) */
static void reap_background(void) {
    int status;
    while (waitpid(-1, &status, 1) > 0) {
        /* child reaped */
    }
}

void shell_run(void) {
    /* Initialize cwd cache from the kernel. */
    char buf[256];
    if (getcwd(buf, sizeof(buf))) {
        strncpy(cwd_buf, buf, sizeof(cwd_buf) - 1);
        cwd_buf[sizeof(cwd_buf) - 1] = '\0';
    }

    printf("\n");
    printf("Welcome to Lestra Shell (lsh) 2.0\n");
    printf("Type 'help' for available commands.\n");
    printf("\n");

    while (1) {
        reap_background();
        print_prompt();

        read_line(input_buffer, CMD_MAX_LEN);
        if (input_buffer[0] == '\0') continue;

        /* Strip trailing background marker */
        int bg = 0;
        size_t len = strlen(input_buffer);
        while (len > 0 && input_buffer[len - 1] == ' ') {
            input_buffer[--len] = '\0';
        }
        if (len > 0 && input_buffer[len - 1] == '&') {
            input_buffer[--len] = '\0';
            bg = 1;
        }

        exec_pipeline(input_buffer, bg);
    }
}

void _start(void) {
    shell_run();
    _exit(0);
}
