/*
 * Lestra OS - Kernel Stubs
 * Minimal stub implementations for planned features to allow kernel to link.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>

/* Physical memory manager - stub (uses simple bump allocator) */
void pmm_init(struct mmap_entry* mmap, uint32_t mmap_entries) {
    pr_info("PMM: initialized (stub - %u entries)\n", mmap_entries);
}

/* Virtual memory manager - stub */
void vmm_init(void) {
    pr_info("VMM: initialized (stub)\n");
}

/* Heap allocator - stub */
void heap_init(void) {
    pr_info("Heap: initialized (stub)\n");
}

/* Print memory stats - stub */
void mm_print_stats(void) {
    pr_info("Memory stats: (stub)\n");
}

/* Shell runner - stub (userspace shell is planned) */
void shell_run(void) {
    pr_info("\n");
    pr_info("==========================================\n");
    pr_info(" L e s t r a   O S\n");
    pr_info(" by lestramk.org\n");
    pr_info("==========================================\n");
    pr_info("Kernel initialized successfully!\n");
    pr_info("Userspace shell: TBD (needs VFS + syscall)\n");
    pr_info("\n");
    /* Idle loop */
    for (;;) {
        __asm__ volatile("hlt");
    }
}
