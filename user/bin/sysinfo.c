/**
 * sysinfo.c — LestraOS system information utility
 *
 * Prints CPU info, memory layout, boot time, and device status.
 * Works on LestraOS kernel >= 0.1.0 with basic libc and serial I/O.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/sysinfo.h>   /* LestraOS extended syscalls */

/* LestraOS-specific: read CPU vendor string via cpuid */
static void get_cpu_vendor(char out[13])
{
    /* cpuid eax=0 -> ebx:edx:ecx = vendor string (12 chars, no null) */
    unsigned int ebx, ecx, edx;
    __asm__ volatile (
        "mov $0, %%eax\n\t"
        "cpuid\n\t"
        : "=b"(ebx), "=c"(ecx), "=d"(edx)
        :
        : "eax"
    );
    memcpy(out + 0, &ebx, 4);
    memcpy(out + 4, &edx, 4);
    memcpy(out + 8, &ecx, 4);
    out[12] = '\0';
}

/* Detect CPU family/model from cpuid eax=1 */
static void get_cpu_model(char out[48])
{
    unsigned int eax, ebx, ecx, edx;
    __asm__ volatile (
        "mov $1, %%eax\n\t"
        "cpuid\n\t"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        :
        : "eax"
    );

    int family   = ((eax >> 8)  & 0x0F);
    int model    = ((eax >> 4)  & 0x0F);
    int ext_family  = ((eax >> 20) & 0x7F);
    int ext_model   = ((eax >> 16) & 0x0F);
    int stepping = (eax & 0x0F);

    /* AMD or Intel? */
    char vendor[13];
    get_cpu_vendor(vendor);

    int display_family = (ext_family == 0) ? family : family + ext_family;
    int display_model  = (ext_family == 0) ? model  : (ext_model << 4) | model;

    snprintf(out, 48, "%s Family %d Model %d Stepping %d",
             vendor, display_family, display_model, stepping);
}

/* Check for CPU features via cpuid eax=1 edx */
static void print_cpu_features(void)
{
    unsigned int edx;
    __asm__ volatile (
        "mov $1, %%eax\n\t"
        "cpuid\n\t"
        : "=d"(edx)
        :
        : "eax", "ebx", "ecx"
    );

    printf("  CPU Features:\n");
    printf("    %s %s %s %s %s\n",
           (edx & (1 << 0))  ? "FPU"    : "",
           (edx & (1 << 4))  ? "TSC"    : "",
           (edx & (1 << 5))  ? "MSR"    : "",
           (edx & (1 << 19)) ? "ACPI"   : "",
           (edx & (1 << 23)) ? "MMX"    : "");
    printf("    %s %s %s %s\n",
           (edx & (1 << 25)) ? "SSE"    : "",
           (edx & (1 << 26)) ? "SSE2"   : "",
           (edx & (1 << 28)) ? "HT"     : "",
           (edx & (1 << 29)) ? "LM"     : "");
}

/* Print memory map from multiboot info */
static void print_memory(multiboot_info_t *mbi)
{
    printf("\n[ Memory ]\n");

    if (mbi->flags & (1 << 0)) {
        printf("  Lower:  %lu KB\n",  mbi->mem_lower);
        printf("  Upper:  %lu KB\n",  mbi->mem_upper);
        printf("  Total:  %lu MB\n", (mbi->mem_lower + mbi->mem_upper) / 1024);
    } else {
        printf("  (memory info not available — boot with GRUB)\n");
    }

    /* Memory regions from mmap */
    if (mbi->flags & (1 << 6)) {
        memory_map_t *mmap = (memory_map_t *)mbi->mmap_addr;
        printf("\n  Memory regions:\n");
        int count = 0;
        while ((uint32_t)mmap < mbi->mmap_addr + mbi->mmap_length && count < 8) {
            uint64_t start = ((uint64_t)mmap->addr_high << 32) | mmap->addr_low;
            uint64_t len   = ((uint64_t)mmap->len_high   << 32) | mmap->len_low;
            const char *type_str;
            switch (mmap->type) {
                case 1: type_str = "usable";         break;
                case 2: type_str = "reserved";       break;
                case 3: type_str = "ACPI reclaim";  break;
                case 4: type_str = "NVS";           break;
                case 5: type_str = "bad";            break;
                default: type_str = "unknown";       break;
            }
            if (mmap->type == 1 && len >= 0x100000) {
                printf("    0x%016lx - 0x%016lx  %s  (%lu MB)\n",
                       start, start + len - 1, type_str, len / (1024 * 1024));
                count++;
            }
            mmap = (memory_map_t *)((uint32_t)mmap + mmap->size + sizeof(mmap->size));
        }
    }
}

/* Print boot device info */
static void print_boot_device(multiboot_info_t *mbi)
{
    printf("\n[ Boot Device ]\n");
    if (mbi->flags & (1 << 1)) {
        printf("  Drive:  0x%02x%02x%02x%02x\n",
               (mbi->boot_device >> 24) & 0xFF,
               (mbi->boot_device >> 16) & 0xFF,
               (mbi->boot_device >> 8)  & 0xFF,
               mbi->boot_device & 0xFF);
    } else {
        printf("  (not available)\n");
    }
}

/* Print cmdline if present */
static void print_cmdline(multiboot_info_t *mbi)
{
    if (mbi->flags & (1 << 2) && mbi->cmdline) {
        printf("\n[ Kernel Command Line ]\n");
        printf("  %s\n", (char *)mbi->cmdline);
    }
}

/* Print multiboot modules (loaded by bootloader) */
static void print_modules(multiboot_info_t *mbi)
{
    if (mbi->flags & (1 << 3) && mbi->mods_count > 0) {
        module_t *mod = (module_t *)mbi->mods_addr;
        printf("\n[ Modules ] (%d loaded)\n", mbi->mods_count);
        for (unsigned int i = 0; i < mbi->mods_count; i++) {
            printf("  [%u] %s\n", i + 1, (char *)mod[i].string);
        }
    }
}

/* LestraOS version — read from kernel info page if available */
static void print_kernel_info(void)
{
    printf("\n[ LestraOS ]\n");
    printf("  Version: 0.1.0-unstable\n");
    printf("  Build:   %s %s\n", __DATE__, __TIME__);
    printf("  Kernel:  LestraOS x86_64\n");
    printf("  Compiler: GCC %d.%d.%d\n",
           __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
}

/* Separator */
static void divider(void)
{
    printf("\n");
    for (int i = 0; i < 60; i++) printf("─");
    printf("\n");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("\n");
    printf("  ███████╗██╗  ██╗██╗   ██╗\n");
    printf("  ╚══███╔╝╚██╗██╔╝╚██╗ ██╔╝\n");
    printf("    ███╔╝  ╚███╔╝  ╚████╔╝ \n");
    printf("   ███╔╝   ██╔██╗   ╚██╔╝  \n");
    printf("  ███████╗██╔╝ ██╗   ██║   \n");
    printf("  ╚══════╝╚═╝  ╚═╝   ╚═╝   \n");
    printf("  LestraOS System Information\n");
    divider();

    /* Kernel info */
    print_kernel_info();

    /* CPU info */
    printf("\n[ CPU ]\n");
    char model[48];
    get_cpu_model(model);
    printf("  %s\n", model);

    /* Check嵩 cpuid availability */
    int has_cpuid = 0;
    __asm__ volatile (
        "pushfl\n\t"
        "pop %%eax\n\t"
        "mov %%eax, %%ecx\n\t"
        "xor $0x200000, %%eax\n\t"
        "push %%eax\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "pop %%eax\n\t"
        "xor %%eax, %%ecx\n\t"
        "and $0x200000, %%eax\n\t"
        "mov %%eax, %0\n\t"
        : "=r"(has_cpuid)
        :
        : "eax", "ecx"
    );

    if (has_cpuid) {
        print_cpu_features();
    } else {
        printf("  (cpuid not available)\n");
    }

    /* Serial ports (LestraOS COM1 driver) */
    printf("\n[ Serial Ports ]\n");
    printf("  COM1:  0x3F8  IRQ4   %s\n",
           is_serial_present(0x3F8) ? "present" : "not detected");
    printf("  COM2:  0x2F8  IRQ3   %s\n",
           is_serial_present(0x2F8) ? "present" : "not detected");

    /* VESA / display info */
    printf("\n[ Display ]\n");
    if (vga_get_mode() >= 0x0013) {
        printf("  Mode:   0x%04X (VESA)\n", vga_get_mode());
        printf("  Type:   graphics\n");
    } else {
        printf("  Mode:   0x%04X (VGA text)\n", vga_get_mode());
        printf("  Type:   text 80x25\n");
    }

    /* Keyboard state */
    printf("\n[ Keyboard ]\n");
    printf("  Driver: PS/2 scancode set 1+2\n");
    printf("  Status: %s\n", is_keyboard_present() ? "connected" : "not found");

    /* If multiboot info is available (GRUB), print it */
    extern multiboot_info_t *lestra_get_mbi(void);
    multiboot_info_t *mbi = lestra_get_mbi();
    if (mbi) {
        print_memory(mbi);
        print_boot_device(mbi);
        print_cmdline(mbi);
        print_modules(mbi);
    }

    divider();
    printf("  Run 'help' in Lestra Shell for more commands.\n");
    divider();
    printf("\n");

    return 0;
}
