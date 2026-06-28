/*
 * Lestra OS - Kernel Linker Script
 * Copyright (c) 2026 lestramk.org
 *
 * Defines the memory layout for the x86_64 kernel.
 * The kernel is loaded at 0x100000 (1MB) by the bootloader.
 */

ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_VMA + 0x100000;

    __kernel_start = .;

    /* Text section - executable code */
    .text : AT(ADDR(.text) - KERNEL_VMA)
    {
        __text_start = .;
        *(.multiboot)
        *(.multiboot2)
        *(.text)
        *(.text.*)
        . = ALIGN(4096);
        __text_end = .;
    }

    /* Read-only data */
    .rodata : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        __rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4096);
        __rodata_end = .;
    }

    /* Initialized data */
    .data : AT(ADDR(.data) - KERNEL_VMA)
    {
        __data_start = .;
        *(.data)
        *(.data.*)
        . = ALIGN(4096);
        __data_end = .;
    }

    /* BSS - uninitialized data */
    .bss : AT(ADDR(.bss) - KERNEL_VMA)
    {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4096);
        __bss_end = .;
    }

    __kernel_end = .;

    /* Page tables (allocated in BSS, referenced here for sizing) */
    __kernel_size = __kernel_end - __kernel_start;
    __bss_size = __bss_end - __bss_start;

    /* Discard unnecessary sections */
    /DISCARD/ :
    {
        *(.comment)
        *(.note)
        *(.note.*)
        *(.eh_frame)
        *(.gnu.*)
    }
}
