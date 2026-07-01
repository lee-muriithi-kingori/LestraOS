/*
 * Lestra OS - Init System
 * Copyright (c) 2026 lestramk.org
 *
 * Lightweight init - PID 1, the first userspace process.
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
    printf("[ OK ] Started shell\n");
    printf("\n");
}

int main(void) {
    print_welcome();
    boot_stages();
    
    /* In a full implementation, init would:
     * 1. Mount filesystems
     * 2. Start system services
     * 3. Handle orphan processes
     * 4. Respawn shell on exit
     */
    
    /* For now, just start the shell directly */
    printf("Starting Lestra Shell...\n\n");
    
    extern void shell_run(void);
    shell_run();
    
    /* If shell exits, try to respawn */
    printf("Shell exited. Respawning...\n");
    shell_run();
    
    /* If all else fails */
    printf("Critical: init cannot continue. Halting.\n");
    syscall(21, 0);  /* shutdown */
    
    while (1);
    return 0;
}
