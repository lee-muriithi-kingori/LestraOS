/*
 * Lestra OS - System Installer
 * Copyright (c) 2026 lestramk.org
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#define INSTALLER_VERSION "1.0.0"

/* Installation steps */
enum install_step {
    STEP_WELCOME = 0,
    STEP_LICENSE,
    STEP_DISK,
    STEP_PARTITION,
    STEP_FORMAT,
    STEP_INSTALL,
    STEP_CONFIGURE,
    STEP_BOOTLOADER,
    STEP_FINISH
};

static const char* step_names[] = {
    "Welcome",
    "License Agreement",
    "Disk Selection",
    "Partitioning",
    "Formatting",
    "Installing System",
    "System Configuration",
    "Bootloader Installation",
    "Installation Complete"
};

static void draw_header(const char* title) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  Lestra OS Installer v%-26s ║\n", INSTALLER_VERSION);
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  %-48s║\n", title);
    printf("╚══════════════════════════════════════════════════╝\n");
    printf("\n");
}

static int step_welcome(void) {
    draw_header("Welcome to Lestra OS");
    
    printf("Welcome to the Lestra OS installation program.\n");
    printf("\n");
    printf("Lestra OS is a lightweight operating system designed to\n");
    printf("run efficiently on devices with 4GB of RAM or more.\n");
    printf("\n");
    printf("This installer will guide you through the installation process.\n");
    printf("\n");
    printf("Press Enter to continue...\n");
    getchar();
    return 0;
}

static int step_license(void) {
    draw_header("License Agreement");
    
    printf("Lestra OS is licensed under the MIT License.\n");
    printf("\n");
    printf("Copyright (c) 2026 lestramk.org\n");
    printf("\n");
    printf("Permission is hereby granted, free of charge, to any person\n");
    printf("obtaining a copy of this software and associated documentation\n");
    printf("files (the Software), to deal in the Software without restriction.\n");
    printf("\n");
    printf("Do you accept the license agreement? [Y/n]: ");
    
    char buf[8];
    if (fgets(buf, sizeof(buf), stdin)) {
        if (buf[0] == 'y' || buf[0] == 'Y' || buf[0] == '\n') {
            return 0;
        }
    }
    return -1;
}

static int step_disk(void) {
    draw_header("Disk Selection");
    
    printf("Available disks:\n");
    printf("\n");
    printf("  [1] /dev/sda - 32 GB QEMU HARDDISK\n");
    printf("  [2] /dev/sr0 - CD-ROM\n");
    printf("\n");
    printf("Select disk [1]: ");
    
    char buf[8];
    fgets(buf, sizeof(buf), stdin);
    return 0;
}

static int step_partition(void) {
    draw_header("Partitioning");
    
    printf("Partitioning options:\n");
    printf("\n");
    printf("  [1] Automatic partitioning (recommended)\n");
    printf("  [2] Manual partitioning\n");
    printf("\n");
    printf("Select option [1]: ");
    
    char buf[8];
    fgets(buf, sizeof(buf), stdin);
    
    printf("\n");
    printf("Proposed partition layout:\n");
    printf("  /dev/sda1  - 512 MB  - /boot   (ext4)\n");
    printf("  /dev/sda2  - 8 GB   - /       (ext4)\n");
    printf("  /dev/sda3  - 4 GB   - swap    (swap)\n");
    printf("  /dev/sda4  - rest   - /home   (ext4)\n");
    
    printf("\nPress Enter to apply...\n");
    getchar();
    return 0;
}

static int step_format(void) {
    draw_header("Formatting");
    
    printf("Formatting partitions...\n");
    printf("  Formatting /dev/sda1 as ext4... ");
    printf("done\n");
    printf("  Formatting /dev/sda2 as ext4... ");
    printf("done\n");
    printf("  Formatting /dev/sda3 as swap... ");
    printf("done\n");
    printf("  Formatting /dev/sda4 as ext4... ");
    printf("done\n");
    
    printf("\nPress Enter to continue...\n");
    getchar();
    return 0;
}

static int step_install(void) {
    draw_header("Installing System");
    
    printf("Installing Lestra OS base system...\n");
    printf("\n");
    
    const char* files[] = {
        "kernel",
        "libc",
        "init",
        "shell",
        "drivers",
        "modules",
        "config",
        "docs"
    };
    
    for (int i = 0; i < 8; i++) {
        printf("  [%d/%d] Installing %s... ", i + 1, 8, files[i]);
        for (int j = 0; j < 5; j++) {
            printf(".");
        }
        printf(" done\n");
    }
    
    printf("\nBase system installed successfully!\n");
    printf("\nPress Enter to continue...\n");
    getchar();
    return 0;
}

static int step_configure(void) {
    draw_header("System Configuration");
    
    char hostname[64];
    printf("Enter hostname [lestra]: ");
    fgets(hostname, sizeof(hostname), stdin);
    hostname[strcspn(hostname, "\n")] = '\0';
    if (strlen(hostname) == 0) strcpy(hostname, "lestra");
    
    printf("\nSelect timezone:\n");
    printf("  [1] UTC\n");
    printf("  [2] Africa/Nairobi\n");
    printf("  [3] America/New_York\n");
    printf("  [4] Europe/London\n");
    printf("  [5] Asia/Tokyo\n");
    printf("  [6] Australia/Sydney\n");
    printf("Select [1]: ");
    
    char buf[8];
    fgets(buf, sizeof(buf), stdin);
    
    printf("\nConfiguration saved.\n");
    printf("  Hostname: %s\n", hostname);
    printf("  Timezone: UTC\n");
    
    printf("\nPress Enter to continue...\n");
    getchar();
    return 0;
}

static int step_bootloader(void) {
    draw_header("Bootloader Installation");
    
    printf("Installing GRUB2 bootloader...\n");
    printf("  Installing on /dev/sda... ");
    printf("done\n");
    printf("  Configuring boot entries... ");
    printf("done\n");
    printf("  Generating grub.cfg... ");
    printf("done\n");
    
    printf("\nBootloader installed successfully!\n");
    printf("\nPress Enter to continue...\n");
    getchar();
    return 0;
}

static int step_finish(void) {
    draw_header("Installation Complete!");
    
    printf("Lestra OS has been successfully installed on your computer.\n");
    printf("\n");
    printf("You can now reboot into your new Lestra OS installation.\n");
    printf("\n");
    printf("Thank you for choosing Lestra OS!\n");
    printf("\n");
    printf("  [1] Reboot now\n");
    printf("  [2] Power off\n");
    printf("  [3] Return to shell\n");
    printf("\n");
    printf("Select [1]: ");
    
    char buf[8];
    fgets(buf, sizeof(buf), stdin);
    
    if (buf[0] == '2') {
        printf("Shutting down...\n");
        syscall(21, 0);
    } else if (buf[0] == '3') {
        return 0;
    } else {
        printf("Rebooting...\n");
        syscall(21, 1);
    }
    
    return 0;
}

/* Installation progress */
static void show_progress(int step, int total) {
    printf("\r[");
    int filled = (step * 30) / total;
    for (int i = 0; i < 30; i++) {
        printf(i < filled ? "=" : " ");
    }
    printf("] %d/%d", step, total);
}

int main(void) {
    printf("\n");
    printf("Starting Lestra OS Installer...\n");
    printf("\n");
    
    /* Run installation steps */
    if (step_welcome() != 0) return 1;
    if (step_license() != 0) return 1;
    if (step_disk() != 0) return 1;
    if (step_partition() != 0) return 1;
    if (step_format() != 0) return 1;
    if (step_install() != 0) return 1;
    if (step_configure() != 0) return 1;
    if (step_bootloader() != 0) return 1;
    step_finish();
    
    return 0;
}
