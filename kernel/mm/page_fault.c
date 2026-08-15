/*
 * Lestra OS - Page Fault Handler (COW, Stack Growth, Demand Paging)
 * Copyright (c) 2026 lestramk.org
 *
 * This module implements the page-fault handler (ISR 14) for LestraOS.
 * When a page fault occurs, the handler checks three conditions:
 *
 *  1. Copy-on-Write (COW): If the faulting page has the PAGE_COW flag
 *     set and the fault was a write to a present page, allocate a fresh
 *     physical page, copy the original contents, map the new page as
 *     writable in the faulting process's PML4, and decrement the old
 *     page's reference count. If the old page's refcount was 1 (sole
 *     owner), we just clear COW and make it writable without copying.
 *
 *  2. Stack Growth: If the fault address is in user space and below
 *     the process's current stack_bottom (but within a reasonable
 *     growth limit), map a fresh zeroed page and extend the stack
 *     downward. The new stack_bottom is recorded in the process struct.
 *
 *  3. Demand Paging: If the fault is inside the already-mapped stack
 *     region but the page is not yet present, allocate and map a zeroed
 *     page on demand.
 *
 *  Any fault that doesn't match one of these conditions causes a kernel
 *  panic with full diagnostics (fault address, error code, RIP, PID).
 */

#include <lestra/types.h>
#include <lestra/mm.h>
#include <lestra/printk.h>
#include <lestra/panic.h>
#include <lestra/sched.h>
#include <lestra/idt.h>
#include <string.h>

extern struct security_status g_security;

/* ---- Helpers ---- */

/* Walk a process's PML4 hierarchy to find the PTE for a virtual address.
 * Returns a pointer to the leaf PTE if all intermediate tables exist and
 * no huge page is encountered; NULL otherwise.
 *
 * The PML4, PDPT, PD, and PT pages are all in physical memory that is
 * identity-mapped by boot.asm (first 1 GB), so we can dereference them
 * directly via their physical addresses. */
static uint64_t* get_pte_in_pml4(uint64_t* pml4, uint64_t virt) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & PAGE_PRESENT)) return NULL;

    uint64_t* pdpt = (uint64_t*)(pml4[pml4_idx] & PTE_PHYS_MASK);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) return NULL;
    if (pdpt[pdpt_idx] & PAGE_HUGE) return NULL;   /* 1 GB huge page */

    uint64_t* pd = (uint64_t*)(pdpt[pdpt_idx] & PTE_PHYS_MASK);
    if (!(pd[pd_idx] & PAGE_PRESENT)) return NULL;
    if (pd[pd_idx] & PAGE_HUGE) return NULL;       /* 2 MB huge page */

    uint64_t* pt = (uint64_t*)(pd[pd_idx] & PTE_PHYS_MASK);
    return &pt[pt_idx];
}

/* ---- Main handler ---- */

/* page_fault_handler — called from ISR 14 dispatcher in idt.c.
 *
 * Parameters:
 *   fault_addr  — value of CR2 (linear address that caused the fault)
 *   error_code  — pushed by the CPU; bits:
 *                  0 = P  (0: not-present, 1: protection violation)
 *                  1 = R/W (0: read, 1: write)
 *                  2 = U/S (0: supervisor, 1: user)
 *                  3 = RSVD (reserved bit set in PTE)
 *                  4 = ID   (instruction fetch)
 *   frame       — saved interrupt frame (for diagnostic RIP, etc.)
 *
 * If the handler successfully resolves the fault (COW copy, stack growth,
 * demand paging), it simply returns and the faulting instruction is retried
 * via iretq. If the fault is unhandled, the kernel panics. */
void page_fault_handler(uintptr_t fault_addr, uint64_t error_code,
                        struct interrupt_frame* frame) {
    struct process* cur = task_current();

    /* Decode error-code bits for logging and logic */
    int present = (error_code & 0x01);    /* P bit */
    int write   = (error_code & 0x02);    /* R/W bit */
    int user    = (error_code & 0x04);    /* U/S bit */

    pr_info("PF: addr=0x%x err=0x%x%s%s%s%s proc=%s(%d)\n",
            (unsigned)fault_addr, (unsigned)error_code,
            present ? " P" : " NP",
            write   ? " W" : " R",
            user    ? " U" : " K",
            g_security.smap ? " SMAP" : "",
            cur ? cur->name : "kernel",
            cur ? cur->pid  : 0);

    /* ================================================================
     * 1. Copy-on-Write (COW) resolution
     *
     * A COW fault occurs when: the page IS present (protection
     * violation, P=1), the access was a write (R/W=1), and the PTE
     * has our software PAGE_COW flag set. The PAGE_WRITABLE bit has
     * been cleared so the write triggers a fault.
     *
     * Resolution:
     *  - If refcount == 1 (sole owner): just clear PAGE_COW and
     *    set PAGE_WRITABLE — no copy needed.
     *  - If refcount > 1: allocate a new physical page, memcpy the
     *    old contents, map the new page writable in the faulting
     *    process's PML4, and decrement the old page's refcount.
     * ================================================================ */
    if (present && write && cur && cur->pml4) {
        uint64_t* pte = get_pte_in_pml4(cur->pml4, fault_addr);
        if (pte && (*pte & PAGE_COW)) {
            /* Extract physical address (bits 12-51 only) and all flags
             * (low bits 0-11 + high bits 52-63 including PAGE_NX). */
            phys_addr_t old_phys = *pte & 0x000FFFFFFFFFF000ULL;
            uint64_t low_flags  = *pte & 0xFFFULL;
            uint64_t high_flags = *pte & 0xFF00000000000000ULL;  /* includes PAGE_NX at bit 63 */
            uint64_t old_flags  = low_flags | high_flags;

            uint32_t ref = pmm_refcount_get(old_phys);

            if (ref <= 1) {
                /* Sole owner (or stale refcount): just make it writable.
                 * No need to copy — we are the only process referencing
                 * this physical page. Preserve all flag bits including NX. */
                *pte = old_phys | (old_flags & ~PAGE_COW) | PAGE_WRITABLE;
                invlpg((void*)fault_addr);
                pr_info("PF: COW sole-owner, remapped 0x%x writable\n",
                        (unsigned)ALIGN_DOWN(fault_addr, PAGE_SIZE));
                return;
            }

            /* Multiple references: allocate a private copy */
            phys_addr_t new_phys = pmm_alloc_page();
            if (!new_phys) {
                panicf("PF: COW — out of memory for private copy at 0x%x",
                       (unsigned)fault_addr);
            }

            /* Copy old contents to new page */
            memcpy((void*)new_phys, (void*)(uintptr_t)old_phys, PAGE_SIZE);

            /* Decrement old page's refcount (we no longer reference it) */
            pmm_refcount_dec(old_phys);

            /* Map the new private page: writable, no COW.
             * Preserve all flag bits (low + high, including NX).
             * We directly write the PTE instead of going through
             * vmm_map_page to avoid double-decrementing the old
             * page's refcount (we already decremented it manually). */
            uint64_t new_flags = (old_flags & ~PAGE_COW) | PAGE_WRITABLE;
            /* We already decremented old_phys refcount manually, so
             * tell vmm_map_page NOT to double-decrement by directly
             * writing the PTE instead of going through vmm_map_page
             * (which would see the old present entry and try to
             * decrement its refcount again). */
            uint64_t page_addr = ALIGN_DOWN(fault_addr, PAGE_SIZE);
            uint64_t pml4_idx = (page_addr >> 39) & 0x1FF;
            uint64_t pdpt_idx = (page_addr >> 30) & 0x1FF;
            uint64_t pd_idx   = (page_addr >> 21) & 0x1FF;
            uint64_t pt_idx   = (page_addr >> 12) & 0x1FF;

            /* Walk to PT (all intermediate tables exist since the old
             * mapping was present) */
            uint64_t* pdpt = (uint64_t*)(cur->pml4[pml4_idx] & PTE_PHYS_MASK);
            uint64_t* pd   = (uint64_t*)(pdpt[pdpt_idx] & PTE_PHYS_MASK);
            uint64_t* pt   = (uint64_t*)(pd[pd_idx] & PTE_PHYS_MASK);

            /* Directly write the new PTE (avoids vmm_map_page's
             * double-decrement of old_phys refcount) */
            pt[pt_idx] = new_phys | new_flags | PAGE_PRESENT;
            invlpg((void*)fault_addr);

            pr_info("PF: COW resolved — new page at phys 0x%x for virt 0x%x (old ref=%u)\n",
                    (unsigned)new_phys, (unsigned)page_addr, ref);
            return;
        }
    }

    /* ================================================================
     * 2. Stack Growth
     *
     * If a user-mode fault occurs at an address below the current
     * stack_bottom but above a reasonable growth limit (8 MB below
     * stack_bottom), allocate a fresh zeroed page and extend the
     * stack downward. Update process->stack_bottom.
     * ================================================================ */
    if (user && cur && fault_addr >= USER_SPACE_START && fault_addr <= USER_SPACE_END) {
        uint64_t stack_top    = USER_STACK_TOP_DEFAULT;  /* may be ASLR-shifted */
        uint64_t stack_bottom = cur->stack_bottom;

        /* Growth allowed: fault is below current bottom but above
         * stack_bottom - 8 MB (prevent unbounded growth). */
        if (fault_addr < stack_bottom &&
            (stack_bottom - fault_addr) <= (8UL * 1024 * 1024)) {
            phys_addr_t phys = pmm_alloc_page();
            if (!phys) {
                panicf("PF: stack growth — OOM at 0x%x", (unsigned)fault_addr);
            }
            memset((void*)(uintptr_t)phys, 0, PAGE_SIZE);

            uint64_t page_addr = ALIGN_DOWN(fault_addr, PAGE_SIZE);
            vmm_map_page(cur->pml4, page_addr, phys,
                         PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE | PAGE_NX);
            cur->stack_bottom = page_addr;

            pr_info("PF: stack grown down to 0x%x (PID %d)\n",
                    (unsigned)page_addr, cur->pid);
            return;
        }

        /* ================================================================
         * 3. Demand Paging (within mapped stack region)
         *
         * If the fault is within the stack's nominal range but the
         * specific page is not yet present (e.g. a guard page that
         * was never allocated), map a zeroed page on demand.
         * ================================================================ */
        if (fault_addr >= stack_bottom && fault_addr < stack_top) {
            uint64_t* pte = get_pte_in_pml4(cur->pml4, fault_addr);
            if (!pte || !(*pte & PAGE_PRESENT)) {
                phys_addr_t phys = pmm_alloc_page();
                if (!phys) {
                    panicf("PF: demand paging — OOM at 0x%x", (unsigned)fault_addr);
                }
                memset((void*)(uintptr_t)phys, 0, PAGE_SIZE);

                uint64_t page_addr = ALIGN_DOWN(fault_addr, PAGE_SIZE);
                vmm_map_page(cur->pml4, page_addr, phys,
                             PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE | PAGE_NX);

                pr_info("PF: demand paging at 0x%x (PID %d)\n",
                        (unsigned)page_addr, cur->pid);
                return;
            }
        }
    }

    /* ================================================================
     * 4. brk-heap demand paging (W3-B / W1-D #1)
     *
     * libc malloc() calls sbrk() which calls SYS_BRK. sys_brk now
     * eagerly maps the new pages between old_brk and new_brk, but
     * for safety we ALSO handle a fault anywhere in [brk_base, brk)
     * that wasn't mapped (e.g. if brk shrank and re-grew past an
     * unmapped page, or if a race left a hole). Map a zeroed
     * user-RW-NX page and resume.
     * ================================================================ */
    if (user && write && cur && cur->brk_base != 0 &&
        fault_addr >= cur->brk_base && fault_addr < cur->brk) {
        /* Make sure the page isn't already present (avoid masking
         * a protection violation on an existing brk page). */
        uint64_t* pte = get_pte_in_pml4(cur->pml4, fault_addr);
        if (!pte || !(*pte & PAGE_PRESENT)) {
            phys_addr_t phys = pmm_alloc_page();
            if (phys) {
                memset((void*)(uintptr_t)phys, 0, PAGE_SIZE);
                uint64_t page_addr = ALIGN_DOWN(fault_addr, PAGE_SIZE);
                vmm_map_page(cur->pml4, page_addr, phys,
                             PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE | PAGE_NX);
                pr_info("PF: brk demand-page at 0x%x (PID %d, brk 0x%x..0x%x)\n",
                        (unsigned)page_addr, cur->pid,
                        (unsigned)cur->brk_base, (unsigned)cur->brk);
                return;
            }
            /* OOM: fall through to SIGSEGV below — don't panic. */
            pr_err("PF: brk demand-page OOM at 0x%x (PID %d)\n",
                   (unsigned)fault_addr, cur->pid);
        }
    }

    /* ================================================================
     * Unhandled fault → kernel panic (kernel-mode) OR SIGSEGV
     * delivery to the offending process (user-mode).
     *
     * W1-C #1 / W1-D #6: A user-mode fault we can't resolve (NULL
     * deref, write to read-only page, NX-execute attempt, unmapped
     * address) must NOT bring down the whole kernel. We deliver
     * SIGSEGV (signal 11) to the current process; if no handler is
     * installed, the default SIG_DFL action terminates the process
     * via proc_exit(128 + 11). Only KERNEL-mode faults panic.
     * ================================================================ */
    /* SMAP diagnostic: if CR4.SMAP is enabled and the faulting address
     * is in user space (below 0x8000_0000_0000) while the fault came from
     * supervisor mode (U/S=0), this is almost certainly a kernel code path
     * that forgot stac() before accessing user memory.  Bit 5 (SMAP) in
     * the error code is also set by the CPU when SMAP is the cause.
     * This hint makes missed stac/clac sites trivial to diagnose once
     * CR4.SMAP is flipped in TIER 3. */
    int smap_bit = (error_code >> 5) & 1;
    if (g_security.smap && !user && fault_addr < 0x800000000000ULL) {
        pr_err("PF: *** POSSIBLE SMAP VIOLATION ***\n");
        pr_err("  Kernel accessed user address 0x%x without AC flag\n",
               (unsigned)fault_addr);
        pr_err("  SMAP error-code bit: %s  CR4.SMAP: ON\n",
               smap_bit ? "SET (confirmed SMAP)" : "clear (check stac)");
        pr_err("  Add stac()/clac() or use copy_from_user/copy_to_user around\n");
        pr_err("  the access at RIP 0x%p.\n",
               frame ? (void*)frame->rip : (void*)0);
    }

    pr_err("PF: UNHANDLED page fault at 0x%x (error 0x%x)\n",
           (unsigned)fault_addr, (unsigned)error_code);
    pr_err("  Present: %s  Access: %s  Mode: %s\n",
           present ? "yes" : "no",
           write   ? "write" : "read",
           user    ? "user" : "supervisor");
    if (cur) {
        pr_err("  Process: %s (PID %d)\n", cur->name, cur->pid);
    }
    if (frame) {
        pr_err("  RIP: 0x%p  RSP: 0x%p\n",
               (void*)frame->rip, (void*)frame->rsp);
    }

    /* USER-mode unhandled fault → deliver SIGSEGV to the process.
     * signal_kill is in signals.c; SIGSEGV is 11. If the process
     * has no SIGSEGV handler installed, signal_check_and_deliver()
     * will run the SIG_DFL action which calls proc_exit(128+11).
     * We can't call signal_check_and_deliver() directly from here
     * because we're in IRQ context, not syscall context — the
     * saved_state pointer on the process struct may not be the
     * user-return state. Instead we just set the pending bit; the
     * next syscall return OR timer-tick preemption path will
     * deliver it. For a fault that occurred OUTSIDE a syscall,
     * we need to terminate immediately — call proc_exit directly
     * if no handler is installed. */
    if (user && cur) {
        extern int64_t signal_kill(int pid, int sig);
        /* Set SIGSEGV pending. */
        signal_kill(cur->pid, 11 /* SIGSEGV */);

        /* If the process has no SIGSEGV handler installed, terminate
         * it now (SIG_DFL for SIGSEGV is "terminate + core"). The
         * 0-th sa_handler means "default" (no user handler). */
        uint64_t segv_handler = cur->sigactions[11].sa_handler;
        if (segv_handler == 0) {
            pr_err("PF: terminating PID %d (SIGSEGV, no handler)\n",
                   cur->pid);
            /* proc_exit will schedule() the next task and never return. */
            extern void proc_exit(int);
            proc_exit(128 + 11);   /* 139 = 128 + SIGSEGV */
            /* proc_exit doesn't return; if it did, halt. */
            while (1) { __asm__ volatile("hlt"); }
        }
        /* A handler IS installed — the pending SIGSEGV will be
         * delivered when we return through the IRQ path (the next
         * sched_tick or syscall return calls signal_check_and_deliver).
         * For now, return so the faulting instruction is retried,
         * which will re-fault if the handler didn't fix the mapping.
         * NOTE: this is a known limitation — without wiring
         * signal_check_and_deliver into the IRQ return path, the
         * signal may not be delivered until the next syscall. */
        return;
    }

    /* KERNEL-mode unhandled fault → real panic. */
    panicf("Unhandled page fault at 0x%x (err 0x%x, RIP 0x%p)",
            (unsigned)fault_addr, (unsigned)error_code,
            frame ? (void*)frame->rip : NULL);
}
