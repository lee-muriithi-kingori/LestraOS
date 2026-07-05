/*
 * Lestra OS - Kernel Linker Script (FIXED)
 * Copyright (c) 2026 lestramk.org
 *
 * Kernel is loaded at physical 0x100000 (1MB) by GRUB/multiboot2.
 * Identity-mapped by boot.asm (first 8MB via 4 huge pages).
 *
 * FIX: Previously placed at 0x10000 (64KB) which conflicts with BIOS
 * data areas, real-mode IVT, and bootloader structures. The standard
 * multiboot2 load address is 1MB.
 */

ENTRY(_start)

SECTIONS
{
    /* FIX: 0x100000 (1MB) - standard multiboot2 kernel load address */
    . = 0x100000;

    __kernel_start = .;
    __kernel_start_phys = .;

    /* Multiboot2 header - MUST be in first 32K of file, 8-byte aligned */
    .multiboot2 :
    {
        KEEP(*(.multiboot2))
    }

    /* Boot code (32-bit -> 64-bit transition) */
    .boot :
    {
        *(.boot)
        . = ALIGN(4096);
    }

    /* Kernel code */
    .text :
    {
        __text_start = .;
        *(.text)
        *(.text.*)
        . = ALIGN(4096);
        __text_end = .;
    }

    /* Read-only data */
    .rodata :
    {
        __rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4096);
        __rodata_end = .;
    }

    /* Initialized data */
    .data :
    {
        __data_start = .;
        *(.data)
        *(.data.*)
        . = ALIGN(4096);
        __data_end = .;
    }

    /* Block descriptor table (GDT/IDT) - needs to be page-aligned */
    .bss :
    {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4096);
        __bss_end = .;
    }

    __kernel_end = .;
    __kernel_end_phys = .;
    __kernel_size = __kernel_end - __kernel_start;
    __bss_size = __bss_end - __bss_start;

    /* Discard sections we don't need in freestanding kernel */
    /DISCARD/ :
    {
        *(.comment)
        *(.note)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.gnu.*)
        *(.debug*)
        *(.stab)
        *(.stabstr)
    }
}
