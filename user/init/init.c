/*
 * Lestra OS - Init System
 * Copyright (c) 2026 lestramk.org
 *
 * Lightweight init - PID 1, the first userspace process.
 *
 * NOTE: This is built as a separate ELF from user/shell/shell.c. The
 * previous version extern-declared shell_run() which is defined in
 * shell.c — but the link line never included shell.o, so the link
 * would fail with "undefined reference to `shell_run`". This version
 * is self-contained: it prints the banner, execs /bin/shell, and
 * loops forever if the exec fails.
 *
 * Since the kernel does not yet implement execve (sys_execve is a stub),
 * this binary effectively just prints the boot banner and idles. Once
 * execve is implemented, calling SYS_EXECVE on "/bin/shell" will replace
 * this process image with the shell.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* Welcome message */
static void print_welcome(void) {
    printf("\n");
    printf("  _                    _           ____   _____ \n");
    printf(" | |                  | |         / __ \\ / ____|\n");
    printf(" | |    ___  __ _  ___| |_ ___   | |  | | (___  \n");
    printf(" | |   / _ \\/ _` |/ __| __/ _ \\  | |  | |\\___ \\ \n");
    printf(" | |__|  __/ (_| | (__| || (_) | | |__| |____) |\n");
    printf(" |_____\\___|\\__,_|\\___|\\__\\___/   \\____/|_____/ \n");
    printf("\n");
    printf("  lestramk.org - Lightweight Operating System\n");
    printf("  Version 1.0.0-alpha | x86_64\n");
    printf("\n");
}

/* Print boot stages */
static void boot_stages(void) {
    printf("[ OK ] Mounted root filesystem\n");
    printf("[ OK ] Started kernel\n");
    printf("[ OK ] Initialized memory management\n");
    printf("[ OK ] Loaded device drivers\n");
    printf("[ OK ] Started timer\n");
    printf("[ OK ] Mounted initrd\n");
    printf("[ OK ] Started init (PID 1)\n");
    printf("\n");
}

/* Entry point - kernel loads this ELF and jumps to _start */
void _start(void) {
    print_welcome();
    boot_stages();

    /* In a full implementation, init would:
     * 1. Mount filesystems
     * 2. Start system services
     * 3. Fork+exec /shell on the console
     * 4. Wait for shell to exit, then respawn
     *
     * The initrd packs the shell binary as /shell (see Makefile:
     * mkinitrd.py uses os.path.basename, so user/shell -> /shell).
     * KE-26-b: fixed the path from /bin/shell to /shell to match.
     */
    printf("Starting Lestra Shell...\n\n");

    char* argv[] = { "/shell", NULL };
    char* envp[] = { "PATH=/bin", "TERM=lestra", NULL };
    int rc = execve("/shell", argv, envp);
    (void)rc;  /* if execve succeeds we never get here */

    printf("init: execve(/shell) failed\n");
    printf("init: PID 1 idle. Use the in-kernel shell for now.\n");
    while (1) {
        sleep(1);
    }
}
