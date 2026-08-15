/*
 * Lestra OS - sysinfo utility
 * Copyright (c) 2026 lestramk.org
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* LestraOS-specific: read CPU vendor string via cpuid.
 * FIX: under -fPIC / -fpie on x86_64, rbx is reserved for the GOT
 * pointer and the compiler refuses to use it as an asm output ("b").
 * Save rbx manually around the cpuid. Also, do not list a register
 * as both output and clobber. */
static void get_cpu_vendor(char out[13])
{
    unsigned int eax, ebx, ecx, edx;
    __asm__ volatile (
        "mov %%rbx, %%rdi\n\t"   /* save PIC rbx */
        "mov $0, %%eax\n\t"
        "cpuid\n\t"
        "mov %%ebx, %%esi\n\t"   /* stash ebx value */
        "mov %%rdi, %%rbx\n\t"   /* restore PIC rbx */
        : "=a"(eax), "=S"(ebx), "=c"(ecx), "=d"(edx)
        :
        : "rdi"
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
        "mov %%rbx, %%rdi\n\t"   /* save PIC rbx */
        "mov $1, %%eax\n\t"
        "cpuid\n\t"
        "mov %%ebx, %%esi\n\t"   /* stash ebx value */
        "mov %%rdi, %%rbx\n\t"   /* restore PIC rbx */
        : "=a"(eax), "=S"(ebx), "=c"(ecx), "=d"(edx)
        :
        : "rdi"
    );

    int family   = ((eax >> 8)  & 0x0F);
    int model    = ((eax >> 4)  & 0x0F);
    int ext_family  = ((eax >> 20) & 0x7F);
    int ext_model   = ((eax >> 16) & 0x0F);
    int stepping = (eax & 0x0F);

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

/* Separator */
static void divider(void)
{
    printf("\n");
    for (int i = 0; i < 60; i++) printf("-");
    printf("\n");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("\n");
    printf("  LestraOS System Information\n");
    divider();

    /* Kernel info */
    printf("\n[ LestraOS ]\n");
    printf("  Version: 1.0.0-alpha\n");
    printf("  Build:   %s %s\n", __DATE__, __TIME__);
    printf("  Kernel:  LestraOS x86_64\n");
    printf("  Compiler: GCC %d.%d.%d\n",
           __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);

    /* CPU info */
    printf("\n[ CPU ]\n");
    char model[48];
    get_cpu_model(model);
    printf("  %s\n", model);

    /* Check cpuid availability.
     * FIX: in long mode the flags register is RFLAGS (64-bit), so the
     * 32-bit-only pushfl/popfl mnemonics don't assemble. Use pushfq/popfq. */
    int has_cpuid = 0;
    __asm__ volatile (
        "pushfq\n\t"
        "pop %%rax\n\t"
        "mov %%rax, %%rcx\n\t"
        "xor $0x200000, %%rax\n\t"
        "push %%rax\n\t"
        "popfq\n\t"
        "pushfq\n\t"
        "pop %%rax\n\t"
        "xor %%rax, %%rcx\n\t"
        "and $0x200000, %%rax\n\t"
        "mov %%eax, %0\n\t"
        : "=r"(has_cpuid)
        :
        : "rax", "rcx"
    );

    if (has_cpuid) {
        print_cpu_features();
    } else {
        printf("  (cpuid not available)\n");
    }

    divider();
    printf("  Run 'help' in Lestra Shell for more commands.\n");
    divider();
    printf("\n");

    return 0;
}

/* Entry point - kernel loads this ELF and jumps to _start.
 * Without this, ld's default entry would land at the first byte of
 * .text (which may be any helper function), not at main(). */
void _start(void) {
    int rc = main(0, (char**)0);
    _exit(rc);
}
