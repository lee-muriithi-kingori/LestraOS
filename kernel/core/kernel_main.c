/*
 * Lestra OS - Kernel Main Entry Point
 * Copyright (c) 2026 lestramk.org
 *
 * This is the C entry point called from boot.asm after transitioning
 * to long mode (x86_64). It initializes all kernel subsystems.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/panic.h>
#include <lestra/gdt.h>
#include <lestra/idt.h>
#include <lestra/irq.h>
#include <lestra/mm.h>
#include <lestra/vga.h>
#include <lestra/serial.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>

/* Multiboot2 info tag types */
#define MB2_TAG_END       0
#define MB2_TAG_CMDLINE   1
#define MB2_TAG_MODULE    3
#define MB2_TAG_MEMINFO   4
#define MB2_TAG_MMAP      6
#define MB2_TAG_FB        8
#define MB2_TAG_RSDP      14

struct mb2_tag {
    uint32_t type;
    uint32_t size;
} __packed;

struct mb2_meminfo {
    struct mb2_tag tag;
    uint32_t mem_lower;
    uint32_t mem_upper;
} __packed;

struct mb2_mmap {
    struct mb2_tag tag;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mmap_entry entries[];
} __packed;

/* Lestra OS boot banner */
static void print_banner(void) {
    vga_set_color(VGA_CYAN, VGA_BLACK);
    printk("\n");
    printk("  ==========================================\n");
    printk("  |                                          |\n");
    printk("  |     L e s t r a   O S                    |\n");
    printk("  |                                          |\n");
    printk("  |     by lestramk.org                      |\n");
    printk("  |     Version 1.0.0-alpha                  |\n");
    printk("  |                                          |\n");
    printk("  ==========================================\n");
    printk("\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* Parse multiboot2 info and extract memory map */
static void parse_multiboot2(void* mb2_info, struct mmap_entry** mmap, uint32_t* mmap_count) {
    uint32_t total_size = *(uint32_t*)mb2_info;
    struct mb2_tag* tag = (struct mb2_tag*)((uintptr_t)mb2_info + 8);
    
    *mmap = NULL;
    *mmap_count = 0;
    
    while (tag->type != MB2_TAG_END) {
        switch (tag->type) {
            case MB2_TAG_MEMINFO: {
                struct mb2_meminfo* mem = (struct mb2_meminfo*)tag;
                pr_info("Memory: Lower=%uKB, Upper=%uKB\n",
                        mem->mem_lower, mem->mem_upper);
                break;
            }
            case MB2_TAG_MMAP: {
                struct mb2_mmap* mmap_tag = (struct mb2_mmap*)tag;
                *mmap = mmap_tag->entries;
                *mmap_count = (mmap_tag->tag.size - 16) / mmap_tag->entry_size;
                pr_info("Memory map: %u entries\n", *mmap_count);
                break;
            }
            case MB2_TAG_MODULE: {
                pr_info("Boot module found at addr=0x%x, size=%u\n",
                        ((uint32_t*)tag)[2], ((uint32_t*)tag)[3]);
                break;
            }
        }
        
        /* Move to next tag (aligned to 8 bytes) */
        tag = (struct mb2_tag*)ALIGN_UP((uintptr_t)tag + tag->size, 8);
        
        /* Safety check - don't read past the info structure */
        if ((uintptr_t)tag >= (uintptr_t)mb2_info + total_size) {
            break;
        }
    }
}

/* Kernel main - C entry point */
void kernel_main(void* mb2_info) {
    struct mmap_entry* mmap = NULL;
    uint32_t mmap_count = 0;
    
    /* Initialize VGA first for early output */
    vga_init();
    vga_clear();
    
    /* Initialize serial port for early debugging */
    serial_default_init();
    
    /* Print boot banner */
    print_banner();
    
    pr_info("Initializing Lestra OS kernel...\n");
    
    /* Parse multiboot2 information */
    parse_multiboot2(mb2_info, &mmap, &mmap_count);
    
    /* Initialize GDT */
    pr_info("Initializing GDT...\n");
    gdt_init();
    
    /* Initialize IDT and interrupts */
    pr_info("Initializing IDT...\n");
    idt_init();
    
    /* Initialize PIC */
    pr_info("Initializing PIC...\n");
    pic_init();
    
    /* Initialize memory management */
    pr_info("Initializing memory management...\n");
    if (mmap && mmap_count > 0) {
        pmm_init(mmap, mmap_count);
    } else {
        pr_warn("No memory map from bootloader, using defaults\n");
        /* Create a basic memory map for testing */
        struct mmap_entry default_mmap[] = {
            {0x00000000, 0x0009FC00, MMAP_USABLE, 0},
            {0x00100000, 0x0FF00000, MMAP_USABLE, 0},  /* ~255MB usable */
            {0x100000000, 0x300000000, MMAP_USABLE, 0}, /* Extended memory */
        };
        pmm_init(default_mmap, 3);
    }
    
    vmm_init();
    heap_init();
    
    /* Print memory stats */
    mm_print_stats();
    
    /* Initialize timer */
    pr_info("Initializing timer (1000 Hz)...\n");
    timer_init(1000);
    
    /* Initialize keyboard */
    pr_info("Initializing keyboard...\n");
    keyboard_init();
    
    /* Enable interrupts */
    pr_info("Enabling interrupts...\n");
    sti();
    
    pr_info("\n");
    pr_info("Lestra OS kernel initialized successfully!\n");
    pr_info("Type 'help' for available commands.\n");
    pr_info("\n");
    
    /* Start the shell */
    extern void shell_run(void);
    shell_run();
    
    /* Should never reach here */
    panic("Kernel main returned unexpectedly");
}
