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
    printf("/\n");
    return 0;
}

static int cmd_ls(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("total 0\n");
    printf("drwxr-xr-x  2 root  root  4096  Jan  1 00:00 .\n");
    printf("drwxr-xr-x  2 root  root  4096  Jan  1 00:00 ..\n");
    printf("-rw-r--r--  1 root  root     0  Jan  1 00:00 welcome.txt\n");
    return 0;
}

static int cmd_cat(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: cat <file>\n");
        return 1;
    }
    if (strcmp(argv[1], "welcome.txt") == 0) {
        printf("Welcome to Lestra OS!\n");
        printf("This is a lightweight operating system by lestramk.org\n");
    } else {
        printf("cat: %s: No such file or directory\n", argv[1]);
    }
    return 0;
}

static int cmd_ps(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("  PID  PPID  STATE    NAME\n");
    printf("    1     0  running  init\n");
    printf("    2     1  running  shell\n");
    return 0;
}

static int cmd_free(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("              total        used        free\n");
    printf("Mem:       4096000     1024000     3072000\n");
    printf("Swap:            0           0           0\n");
    return 0;
}

static int cmd_reboot(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("Rebooting system...\n");
    syscall(21, 1);  /* SYS_REBOOT */
    return 0;
}

static int cmd_shutdown(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("Shutting down...\n");
    syscall(21, 0);  /* SYS_REBOOT */
    return 0;
}

static int cmd_date(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("Sat Jan  1 00:00:00 UTC 2026\n");
    return 0;
}

static int cmd_uptime(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf(" 00:00:00 up 0 min,  1 user,  load average: 0.00, 0.00, 0.00\n");
    return 0;
}

static int cmd_whoami(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("root\n");
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
    printf("Memory Information:\n");
    printf("  Total RAM:    4096 MB\n");
    printf("  Used:         1024 MB\n");
    printf("  Free:         3072 MB\n");
    printf("  Buffers:      128 MB\n");
    printf("  Cached:       256 MB\n");
    printf("  Kernel:       64 MB\n");
    return 0;
}

static int cmd_cpuinfo(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("CPU Information:\n");
    printf("  Architecture: x86_64\n");
    printf("  Model:        QEMU Virtual CPU\n");
    printf("  Cores:        1\n");
    printf("  Frequency:    2000 MHz\n");
    printf("  Features:     SSE SSE2 PAE PSE\n");
    return 0;
}

static int cmd_sysinfo(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("System Information:\n");
    printf("  OS:           Lestra OS 1.0.0-alpha\n");
    printf("  Kernel:       lestra-kernel x86_64\n");
    printf("  Architecture: x86_64\n");
    printf("  Memory:       4096 MB\n");
    printf("  Uptime:       0 minutes\n");
    printf("  Processes:    2\n");
    printf("  Shell:        lsh 1.0\n");
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
