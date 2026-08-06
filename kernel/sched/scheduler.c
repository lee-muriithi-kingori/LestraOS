/*
 * Lestra OS - Preemptive Process Scheduler
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Process control blocks, round-robin preemptive scheduling,
 * context switching, fork, exec, exit, wait, signals, blocking.
 */

#include <lestra/sched.h>
#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <lestra/panic.h>
#include <lestra/vfs.h>
#include <lestra/pipe.h>
#include <string.h>

/* Global process table and current process pointer.
 * sched.h declares `extern struct process procs[MAX_PROCS]` and
 * task_current() returns `current`. */
struct process procs[MAX_PROCS];
static struct process* current = NULL;
static int next_pid = 1;
static int scheduler_enabled = 0;
uint64_t save_kernel_cr3 = 0;

/* External: context switch assembly (KE-30: v3 — ISR frame swap + ret) */
extern void context_switch(struct cpu_state* old_state, struct cpu_state* new_state,
                           uint64_t new_pml4, uint64_t new_kstack_top);

/* External: ELF loader */
extern uint64_t elf_load(const void* elf_data, size_t elf_size);
extern void elf_jump_to_user(uint64_t entry, uint64_t stack, uintptr_t pml4);

/* elf_load populates these globals — the caller copies them into the
 * process struct before elf_load is called again for another process. */
extern uintptr_t* user_pml4;
extern uint64_t   user_stack_ptr;

/* External: page table helpers */
extern void vmm_map_page(uintptr_t* pml4, virt_addr_t vaddr, phys_addr_t paddr, uint64_t flags);
extern phys_addr_t vmm_get_phys(uintptr_t* pml4, virt_addr_t vaddr);

/* Forward declarations */
void schedule(void);

/* ---- Process slot management ---- */

static struct process* find_free_proc(void) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == PROC_FREE) return &procs[i];
    }
    return NULL;
}

struct process* sched_alloc_proc(void) {
    return find_free_proc();
}

struct process* sched_find_by_pid_impl(int pid) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != PROC_FREE && procs[i].pid == pid)
            return &procs[i];
    }
    return NULL;
}

int sched_alloc_pid(void) {
    return next_pid++;
}

struct process* task_current(void) {
    return current;
}

int proc_getpid(void) {
    return current ? current->pid : 0;
}

int proc_is_linux_process(void) {
    return current ? current->is_linux_process : 0;
}

void proc_set_linux_process(int is_linux) {
    if (current) current->is_linux_process = is_linux;
}

void sched_set_clear_child_tid(int pid, void* addr) {
    struct process* p = sched_find_by_pid_impl(pid);
    if (p) p->clear_child_tid = addr;
}

/* ---- Address space management ---- */

static uint64_t* create_proc_pml4(void) {
    /* KE-26: Use the deep-copy approach from create_user_address_space
     * (in elf.c) instead of sharing boot_pml4[0..3] by pointer. The old
     * approach had TWO bugs:
     * 1. Sharing by pointer meant any page-table modification by the
     *    forked child corrupted the KERNEL's boot page tables.
     * 2. clear_kernel_user_bits cleared PAGE_USER on leaf entries but
     *    NOT on intermediate entries, so user pages in the low 4GB were
     *    unreachable from ring 3 (same as KE-25a).
     * The deep copy creates private PDPTs and PDs with PAGE_USER cleared,
     * and user_map_page/vmm_map_page add PAGE_USER to intermediate entries
     * when mapping user pages. */
    return create_user_address_space();
}

static void proc_map_page(struct process* p, uint64_t vaddr, uint64_t phys, uint64_t flags) {
    vmm_map_page(p->pml4, vaddr, phys, flags);
}

/* ---- Deep-copy fork: safe under SMEP+SMAP ----
 *
 * KE-31: Replaced COW sharing with deep-copy. COW fork triple-faulted
 * under SMEP+SMAP because:
 *   1. cow_share_pages walks page tables via identity-mapped physical
 *      addresses in the low 4GB. If a page-table page's physical address
 *      collides with a USER-mapped virtual address in the parent's PML4
 *      (e.g. init BSS at 0x407000), SMAP blocks the supervisor access.
 *   2. Even with stac/clac, vmm_map_page() clears AC mid-loop (its own
 *      clac), so subsequent iterations fault.
 *   3. Even if SMAP were handled, COW pages in the low 4GB overlap
 *      with kernel data structures (heap, procs[], kernel stacks).
 *      After COW, the first kernel WRITE to a COW page triggers a page
 *      fault whose handler runs on the same COW'd kernel stack ->
 *      recursive fault -> triple fault.
 *
 * Solution: deep-copy user pages while running on boot_pml4.
 * boot_pml4 has NO user mappings, so SMAP is never triggered when
 * accessing page tables or page contents via the identity map.
 * Deep-copy is slower than COW but architecturally safe.
 *
 * The COW infrastructure (page_fault_handler COW path, refcounting)
 * is kept for future use once a kmap-style kernel mapping exists. */
/* Global for deep_copy_user_pages to communicate page count to proc_fork */
static int pages_copied_last = 0;

static void deep_copy_user_pages(struct process* parent, struct process* child) {
    extern uint64_t boot_pml4[];
    uint64_t saved_cr3 = read_cr3();

    /* Switch to boot_pml4 for the entire copy operation.
     * boot_pml4 maps the low 4GB as supervisor-only (no USER pages),
     * so SMAP never triggers when we dereference page table entries
     * or copy page contents via their identity-mapped addresses. */
    static int pages_copied = 0;
    pages_copied = 0;
    write_cr3((uintptr_t)boot_pml4);

    /* KE-33 FIX: Walk the child's page-table structure and mark ALL
     * page-table pages (PML4, PDPTs, PDs, PTs) as used in the PMM
     * bitmap. This prevents pmm_alloc_page from returning them during
     * the deep-copy loop below.
     *
     * Without this, pmm_alloc_page (called for user page copies AND
     * by vmm_map_page for new PT pages) can return a page that's
     * already in use as a page-table page, causing memcpy to overwrite
     * the page-table structure. This manifests as pd[0] (kernel text
     * mapping) being zeroed, which triple-faults on context_switch to
     * the child.
     *
     * The root cause is a PMM bitmap corruption cascade during the
     * copy loop — re-marking the page-table pages as used before the
     * loop is a targeted fix that prevents the cascade from affecting
     * the child's own page tables. */
    extern void pmm_mark_used(phys_addr_t);
    {
        uint64_t* pml4 = child->pml4;
        pmm_mark_used((phys_addr_t)(uintptr_t)pml4);
        for (int p4 = 0; p4 < 512; p4++) {
            if (!(pml4[p4] & PAGE_PRESENT)) continue;
            uint64_t* pdpt = (uint64_t*)(pml4[p4] & PTE_PHYS_MASK);
            pmm_mark_used((phys_addr_t)(uintptr_t)pdpt);
            for (int p3 = 0; p3 < 512; p3++) {
                if (!(pdpt[p3] & PAGE_PRESENT)) continue;
                if (pdpt[p3] & PAGE_HUGE) continue;
                uint64_t* pd = (uint64_t*)(pdpt[p3] & PTE_PHYS_MASK);
                pmm_mark_used((phys_addr_t)(uintptr_t)pd);
                for (int p2 = 0; p2 < 512; p2++) {
                    if (!(pd[p2] & PAGE_PRESENT)) continue;
                    if (pd[p2] & PAGE_HUGE) continue;
                    uint64_t* pt = (uint64_t*)(pd[p2] & PTE_PHYS_MASK);
                    pmm_mark_used((phys_addr_t)(uintptr_t)pt);
                }
            }
        }
    }

    /* KE-31: Scan ALL PML4 indices (0-511), not just 4-511.
     * User ELF segments (e.g. /init at 0x400000) live in PML4[0]
     * because 0x400000 >> 39 = 0. The old cow_share_pages also
     * started at index 4 and missed these pages — but COW's
     * crash masked the bug. With deep-copy, the child's PML4[0]
     * is a fresh deep copy of boot_pml4[0] (no user pages), so
     * we MUST copy user PTEs from the parent's PML4[0-3] too.
     * The PAGE_USER check on each leaf PTE filters out kernel
     * pages in these ranges automatically. */
    for (int p4 = 0; p4 < 512; p4++) {
        if (!(parent->pml4[p4] & PAGE_PRESENT)) continue;
        uint64_t* pdpt = (uint64_t*)(parent->pml4[p4] & PTE_PHYS_MASK);
        for (int p3 = 0; p3 < 512; p3++) {
            if (!(pdpt[p3] & PAGE_PRESENT)) continue;
            if (pdpt[p3] & PAGE_HUGE) continue;
            uint64_t* pd = (uint64_t*)(pdpt[p3] & PTE_PHYS_MASK);
            for (int p2 = 0; p2 < 512; p2++) {
                if (!(pd[p2] & PAGE_PRESENT)) continue;
                if (pd[p2] & PAGE_HUGE) continue;
                uint64_t* pt = (uint64_t*)(pd[p2] & PTE_PHYS_MASK);
                for (int p1 = 0; p1 < 512; p1++) {
                    if (!(pt[p1] & PAGE_PRESENT)) continue;
                    if (!(pt[p1] & PAGE_USER)) continue;

                    uint64_t vaddr = ((uint64_t)p4 << 39) | ((uint64_t)p3 << 30) |
                                     ((uint64_t)p2 << 21) | ((uint64_t)p1 << 12);
                    phys_addr_t old_phys = pt[p1] & 0x000FFFFFFFFFF000ULL;
                    uint64_t flags = (pt[p1] & 0xFFFULL) | (pt[p1] & 0xFF00000000000000ULL);

                    /* Allocate a fresh physical page for the child.
                     * KE-33: Retry if pmm returns a page in the kernel
                     * region (0x100000-0x382000 = kernel+bitmap+refcount).
                     * The PMM bitmap can get corrupted during this loop
                     * (by vmm_map_page's PT-page memsets cascading), so
                     * we defensively reject any allocation that would
                     * overwrite kernel/BMM data. */
                    phys_addr_t new_phys = 0;
                    for (int retry = 0; retry < 8; retry++) {
                        new_phys = pmm_alloc_page();
                        if (!new_phys) break;
                        if (new_phys >= 0x100000 && new_phys < 0x400000) {
                            /* kernel/bitmap/refcount region — reject */
                            pmm_mark_used(new_phys);
                            new_phys = 0;
                            continue;
                        }
                        break;
                    }
                    if (!new_phys) {
                        pr_warn("fork: OOM deep-copying page 0x%x\n",
                                (unsigned)vaddr);
                        continue;
                    }


                    /* Copy contents from parent's page (identity-mapped
                     * by boot_pml4, accessible without stac since there
                     * are no USER mappings in boot_pml4). */
                    memcpy((void*)(uintptr_t)new_phys,
                           (void*)(uintptr_t)old_phys, PAGE_SIZE);

                    /* Map in child's PML4 with same flags (writable,
                     * no COW). vmm_map_page's internal stac/clac is
                     * harmless under boot_pml4 (no USER pages to
                     * trigger SMAP regardless of AC state). */
                    vmm_map_page(child->pml4, vaddr, new_phys, flags);
                    pages_copied++;
                }
            }
        }
    }

    pr_info("fork: copied %d user pages\n", pages_copied);
    pages_copied_last = pages_copied;


    /* Switch back to parent's CR3 and flush TLB */
    write_cr3(saved_cr3);
}

static void proc_setup_stack(struct process* p) {
    size_t stack_pages = (256 * 1024) / PAGE_SIZE;
    uint64_t stack_top = 0x00007FFFFFE00000ULL;
    uint64_t stack_start = stack_top - (256 * 1024);
    for (size_t i = 0; i < stack_pages; i++) {
        phys_addr_t phys = pmm_alloc_page();
        if (!phys) return;
        memset((void*)phys, 0, PAGE_SIZE);
        proc_map_page(p, stack_start + i * PAGE_SIZE,
                      phys, PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE | PAGE_NX);
    }
    p->user_stack_ptr = stack_top - 16;
    p->stack_bottom = stack_start;   /* Track stack bottom for growth */
}

/* ---- Per-process FD table management ---- */

/* Initialize a process's fd table with the standard special fds:
 * fd 0 = stdin,  fd 1 = stdout,  fd 2 = stderr. */
void fd_table_init(struct process* p) {
    for (int i = 0; i < MAX_FD_PER_PROC; i++) {
        p->fds[i].type = FD_UNUSED;
        p->fds[i].resource = 0;
        p->fds[i].offset = 0;
        p->fds[i].flags = 0;
    }
    /* stdin (fd 0) — reads from keyboard */
    p->fds[0].type = FD_SPECIAL;
    p->fds[0].resource = 0;
    p->fds[0].offset = 0;
    p->fds[0].flags = O_RDONLY;

    /* stdout (fd 1) — writes to VGA + serial */
    p->fds[1].type = FD_SPECIAL;
    p->fds[1].resource = 1;
    p->fds[1].offset = 0;
    p->fds[1].flags = O_WRONLY;

    /* stderr (fd 2) — writes to VGA + serial */
    p->fds[2].type = FD_SPECIAL;
    p->fds[2].resource = 2;
    p->fds[2].offset = 0;
    p->fds[2].flags = O_WRONLY;
}

/* Copy parent's fd table to child (for fork).
 * Both processes share the same underlying VFS/pipe resources
 * (the 'resource' field points to the same global fd). Offsets
 * are copied but thereafter independent — each process tracks
 * its own position in the file. */
void fd_table_copy(struct process* parent, struct process* child) {
    for (int i = 0; i < MAX_FD_PER_PROC; i++) {
        child->fds[i] = parent->fds[i];
        /* For VFS fds, both parent and child share the same global
         * VFS fd. No additional refcounting is needed at the VFS
         * level because VFS fds are global integers. */
    }
}

/* Close all open fds in a process (called during proc_reap).
 * For each active fd, close the underlying resource via
 * vfs_close or pipe_close as appropriate. FD_SPECIAL fds
 * (stdin/stdout/stderr) have no underlying resource to close. */
void fd_table_close_all(struct process* p) {
    for (int i = 0; i < MAX_FD_PER_PROC; i++) {
        if (p->fds[i].type == FD_UNUSED) continue;

        if (p->fds[i].type == FD_VFS) {
            vfs_close(p->fds[i].resource);
        } else if (p->fds[i].type == FD_PIPE) {
            pipe_close(p->fds[i].resource);
        }
        /* FD_SPECIAL has no resource to close */

        p->fds[i].type = FD_UNUSED;
        p->fds[i].resource = 0;
        p->fds[i].offset = 0;
        p->fds[i].flags = 0;
    }
}

/* Reap a zombie process — free all its resources.
 * Called by proc_wait / proc_wait_blocking when the parent
 * collects the child's exit status and the child slot is
 * about to be marked PROC_FREE. This closes all open fds,
 * destroys the address space (COW-aware), and frees the
 * kernel stack. */
void proc_reap(struct process* p) {
    if (!p) return;

    /* Close all open file descriptors */
    fd_table_close_all(p);

    /* Destroy address space (COW-aware: shared pages are only freed
     * when their refcount drops to 0, not unconditionally) */
    if (p->pml4) {
        vmm_destroy_address_space(p->pml4);
        p->pml4 = NULL;
    }

    /* Free kernel stack */
    if (p->kernel_stack) {
        kfree(p->kernel_stack);
        p->kernel_stack = NULL;
    }
}

/* ---- Process lifecycle ---- */

int proc_create(const char* name, const void* elf_data, size_t elf_size) {
    struct process* p = find_free_proc();
    if (!p) {
        pr_warn("sched: no free process slots\n");
        return -1;
    }

    memset(p, 0, sizeof(*p));
    p->pid = next_pid++;
    p->state = PROC_RUNNABLE;
    p->parent_pid = current ? current->pid : 0;
    strncpy(p->name, name, sizeof(p->name) - 1);

    /* Load the ELF binary — this sets up the user address space,
     * maps segments, allocates user stack, and returns entry point.
     * elf_load populates globals user_pml4/user_stack_ptr which we
     * copy into the process struct. */
    uint64_t entry = elf_load(elf_data, elf_size);
    if (!entry) {
        pr_warn("sched: elf_load failed for '%s'\n", name);
        p->state = PROC_FREE;
        return -1;
    }
    p->entry_point = entry;
    p->pml4 = (uint64_t*)user_pml4;
    p->user_stack_ptr = user_stack_ptr;
    p->stack_bottom = 0x00007FFFFFE00000ULL - (256 * 1024);

    p->kernel_stack = kmalloc(16384);
    if (!p->kernel_stack) {
        vmm_destroy_address_space(p->pml4);
        p->state = PROC_FREE;
        return -1;
    }
    p->kernel_stack_top = (uint64_t)p->kernel_stack + 16384;

    p->saved_state = (struct cpu_state*)(p->kernel_stack_top - sizeof(struct cpu_state));
    memset(p->saved_state, 0, sizeof(struct cpu_state));

    /* Initialize per-process fd table with stdin/stdout/stderr */
    fd_table_init(p);

    /* KE-26: GDT was rearranged — USER_DS=0x18 (slot 3), USER_CS=0x20 (slot 4).
     * Ring-3 selectors: CS = 0x23 (USER_CS|RPL3), SS = 0x1B (USER_DS|RPL3).
     * These must match what sysretq loads (see syscall_init STAR setup) so
     * that a context switch via iretq and a return-from-syscall via sysretq
     * land in the same privilege/segment state. */
    p->saved_state->ss = 0x1B;
    p->saved_state->rsp = p->user_stack_ptr;
    p->saved_state->rflags = 0x202;
    p->saved_state->cs = 0x23;
    p->saved_state->rip = p->entry_point;

    pr_info("sched: created process %d '%s' entry=0x%x\n",
            p->pid, p->name, (unsigned)p->entry_point);
    return p->pid;
}

int proc_fork(void) {
    if (!current) return -1;

    struct process* child = find_free_proc();
    if (!child) return -1;

    memset(child, 0, sizeof(*child));
    child->pid = next_pid++;
    child->state = PROC_RUNNABLE;
    child->parent_pid = current->pid;
    strncpy(child->name, current->name, sizeof(child->name) - 1);

    child->pml4 = create_proc_pml4();
    if (!child->pml4) { child->state = PROC_FREE; return -1; }


    /* KE-31: Deep-copy parent's user pages into child.
     * Uses boot_pml4 internally to avoid SMAP faults on identity-mapped
     * page table pages that collide with user-mapped virtual addresses.
     * See deep_copy_user_pages() comment for full rationale. */
    deep_copy_user_pages(current, child);

    /* Copy stack_bottom from parent (both start with same stack range) */
    child->stack_bottom = current->stack_bottom;

    /* KE-31: Use a static kernel stack for the child.
     * TODO(KE-34): switch to kmalloc — current kmalloc causes hang
     * after deep_copy_user_pages returns (investigate heap allocator
     * interaction with deep-copy page allocations). The static buffer
     * limits us to ONE forked child at a time, which is fine for the
     * current fork test but must be fixed before multi-process use. */
    static uint8_t child_kstack[16384] __aligned(16);
    child->kernel_stack = child_kstack;
    child->kernel_stack_top = (uint64_t)child_kstack + sizeof(child_kstack);
    child->saved_state = (struct cpu_state*)(child->kernel_stack_top - sizeof(struct cpu_state));

    /* KE-31: Build child's return state from SYSCALL-saved values.
     *
     * We CANNOT use current->saved_state because it's stale — it was
     * set by proc_create and never updated by context_switch (the
     * parent may never have been preempted, so no ISR-swap ever wrote
     * the current register state to saved_state).
     *
     * Instead, syscall_entry.asm saves the user's RIP (in RCX),
     * RFLAGS (in R11), and RSP (in g_saved_user_rsp) on every syscall.
     * These give us the exact user state needed for the child to resume
     * at the instruction after the fork() call.
     *
     * The child gets: rax=0 (fork return value in child), all other
     * GPRs=0 (safe for C code — the compiler only relies on RSP),
     * rip=user_return_rip, rsp=user's current stack pointer,
     * rflags=user's RFLAGS, cs=0x23 (user CS), ss=0x1B (user SS). */
    extern uint64_t g_syscall_user_rip;
    extern uint64_t g_syscall_user_rflags;
    extern uint64_t g_saved_user_rsp;

    memset(child->saved_state, 0, sizeof(struct cpu_state));
    child->saved_state->rax    = 0;  /* fork return value for child */
    child->saved_state->rip    = g_syscall_user_rip;
    child->saved_state->cs     = 0x23;  /* USER_CS | RPL3 */
    /* KE-31: Ensure IF is set in child's RFLAGS.
     * g_syscall_user_rflags has IF=0 because SFMASK=0x200 clears it
     * on syscall entry. The child MUST run with interrupts enabled
     * so the timer IRQ can fire for preemption. */
    child->saved_state->rflags = (g_syscall_user_rflags | 0x200) & ~0x3000;
    child->saved_state->rsp    = g_saved_user_rsp;
    child->saved_state->ss     = 0x1B;  /* USER_DS | RPL3 */

    /* KE-33: Copy the parent's USER callee-saved registers (rbx, rbp,
     * r12-r15) into the child's saved_state.
     *
     * These registers were saved by syscall_entry.asm to globals
     * (g_syscall_user_rbx etc.) immediately on syscall entry, BEFORE
     * any C code ran. We cannot read them from the current register
     * file via inline asm because the C compiler (having compiled
     * syscall_dispatch -> ... -> sys_fork -> proc_fork) is free to
     * spill and reuse any callee-saved register for its own
     * temporaries — the values in rbx/rbp/r12-r15 here may be the
     * compiler's, not the user's.
     *
     * The caller-saved registers (rdi, rsi, rdx, rcx, r8, r9, r10,
     * r11) do NOT need to be preserved across the fork() syscall:
     * the System V ABI allows the kernel to clobber them, and the
     * user's libc fork() wrapper assumes only rax (return value) is
     * meaningful. rcx/r11 are special (they hold user RIP/RFLAGS on
     * syscall entry) and are already reflected in child->saved_state
     * ->rip / ->rflags via g_syscall_user_rip / g_syscall_user_rflags.
     *
     * Without this copy, the child resumes at user RIP with rbp=0 /
     * r12-r15=0 (from the memset above) and crashes on the first
     * stack-frame access (e.g. `mov [rbp-8], rax` with rbp=0 writes
     * to address 0xFFFFFFFFFFFFFFF8 → #GP → triple fault). */
    extern uint64_t g_syscall_user_rbx;
    extern uint64_t g_syscall_user_rbp;
    extern uint64_t g_syscall_user_r12;
    extern uint64_t g_syscall_user_r13;
    extern uint64_t g_syscall_user_r14;
    extern uint64_t g_syscall_user_r15;

    child->saved_state->rbx = g_syscall_user_rbx;
    child->saved_state->rbp = g_syscall_user_rbp;
    child->saved_state->r12 = g_syscall_user_r12;
    child->saved_state->r13 = g_syscall_user_r13;
    child->saved_state->r14 = g_syscall_user_r14;
    child->saved_state->r15 = g_syscall_user_r15;

    pr_info("fork: child state rip=0x%x rsp=0x%x\n",
            (unsigned)child->saved_state->rip,
            (unsigned)child->saved_state->rsp);

    /* Copy parent's fd table to child (shared VFS resources,
     * independent offsets) */
    fd_table_copy(current, child);

    pr_info("sched: forked process %d from %d (deep-copy, %d pages)\n",
            child->pid, current->pid, pages_copied_last);

    return child->pid;
}

void proc_exit(int status) {
    if (!current) {
        pr_warn("sched: proc_exit with no current process\n");
        return;
    }

    current->state = PROC_ZOMBIE;
    current->exit_status = status;
    pr_info("sched: process %d exited with status %d\n", current->pid, status);

    /* Wake parent if it's blocked in waitpid */
    struct process* parent = sched_find_by_pid_impl(current->parent_pid);
    if (parent && parent->state == PROC_BLOCKED) {
        parent->state = PROC_RUNNABLE;
    }

    schedule();
}

/* ---- Blocking primitives ---- */

/* Track what PID each process is waiting for (for waitpid blocking) */
static int wait_target[MAX_PROCS];

void task_block(void) {
    if (!current) return;
    current->state = PROC_BLOCKED;
    schedule();
    /* We've been resumed — either because another task called
     * task_unblock() on us and the scheduler picked us again, or
     * because no other task was runnable and schedule() returned
     * immediately without context-switching. In the latter case our
     * state is still PROC_BLOCKED; restore PROC_RUNNING so the next
     * schedule() doesn't permanently skip us (a stuck-in-BLOCKED
     * bug that would otherwise hang a single-process caller). */
    if (current->state == PROC_BLOCKED) {
        current->state = PROC_RUNNING;
    }
}

void task_unblock(void* task) {
    struct process* p = (struct process*)task;
    if (!p) return;
    if (p->state == PROC_BLOCKED) {
        p->state = PROC_RUNNABLE;
    }
}

void task_unblock_pid(int pid) {
    struct process* p = sched_find_by_pid_impl(pid);
    if (p && p->state == PROC_BLOCKED) {
        p->state = PROC_RUNNABLE;
    }
}

void task_sleep(uint64_t ms) {
    if (!current) return;
    if (ms == 0) {
        /* Zero sleep = plain yield. */
        if (scheduler_enabled) schedule();
        return;
    }

    /* Honor the requested sleep duration by recording a wake deadline
     * (in timer_get_ms() units) and blocking until it passes.
     *
     * The wake is driven from two places:
     *   1. sched_check_wakeups() — runs on every timer IRQ tick; if our
     *      deadline has passed it transitions us PROC_BLOCKED -> PROC_RUNNABLE
     *      so a subsequent schedule() can pick us.
     *   2. This loop's own deadline check — handles the case where no
     *      other task is runnable and schedule() returns without switching;
     *      we then halt the CPU until the next IRQ0 tick.
     *
     * Together these guarantee the sleep duration is honored AND the
     * CPU is yielded (either to another task or to the hlt instruction)
     * instead of the previous behaviour where task_sleep() ignored `ms`
     * entirely and just yielded once, causing poll()/select() callers
     * to busy-loop at 100% CPU.
     *
     * NOTE on interrupt enablement: SYSCALL clears RFLAGS.IF (SFMASK is
     * set to 0x200 in syscall_init), so syscall handlers run with
     * interrupts disabled. A plain hlt() would therefore halt forever
     * (no IRQ could wake us). We use the standard "sti; hlt" sequence:
     * sti re-enables interrupts, and the CPU guarantees no interrupt is
     * taken until after the following hlt begins — so there is no race
     * between sti and hlt. The timer IRQ then fires, its handler runs
     * sched_check_wakeups() (which may wake us), and we resume. */
    extern uint64_t timer_get_ms(void);
    uint64_t deadline = timer_get_ms() + ms;
    current->wake_tick = deadline;

    while (1) {
        /* Re-check deadline first — sched_check_wakeups may have already
         * advanced us past it during a prior schedule(). */
        if ((int64_t)(timer_get_ms() - deadline) >= 0) break;

        current->state = PROC_BLOCKED;
        schedule();

        /* If schedule() returned without switching (no other task
         * runnable), halt the CPU until the next IRQ0 tick to avoid
         * a busy-loop. The timer IRQ's sched_check_wakeups() will
         * wake us when the deadline passes. We use sti;hlt (not a
         * bare hlt) because SYSCALL cleared IF on entry. */
        if (current->state == PROC_BLOCKED) {
            __asm__ volatile("sti; hlt" ::: "memory");
        }
    }

    current->wake_tick = 0;
    current->state = PROC_RUNNING;
}

void task_set_priority(int p) { (void)p; }

/* Called on timer tick to check for processes that need waking */
void sched_check_wakeups(void) {
    extern uint64_t timer_get_ms(void);
    uint64_t now = timer_get_ms();
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != PROC_BLOCKED) continue;

        /* waitpid wake: blocked because we're waiting for a child */
        if (wait_target[i] > 0) {
            struct process* target = sched_find_by_pid_impl(wait_target[i]);
            if (target && target->state == PROC_ZOMBIE) {
                procs[i].state = PROC_RUNNABLE;
                wait_target[i] = 0;
            }
            continue;  /* a waitpid waiter can't also be a sleeper */
        }

        /* task_sleep wake: blocked because we asked to sleep until wake_tick */
        if (procs[i].wake_tick != 0 &&
            (int64_t)(now - procs[i].wake_tick) >= 0) {
            procs[i].state = PROC_RUNNABLE;
            /* Leave wake_tick non-zero so task_sleep()'s loop can see we
             * were woken by the timer and clear it on exit. */
        }
    }
}

/* ---- Wait / scheduling ---- */

int proc_wait(int pid, int* status) {
    if (!current) return -1;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].pid == pid && procs[i].parent_pid == current->pid) {
            if (procs[i].state == PROC_ZOMBIE) {
                if (status) *status = procs[i].exit_status;
                int ret_pid = procs[i].pid;
                /* Reap the zombie: close fds, free address space & kernel stack */
                proc_reap(&procs[i]);
                procs[i].state = PROC_FREE;
                return ret_pid;
            }
            return 0;  /* exists but not zombie yet */
        }
    }
    return -1;
}

/* Blocking waitpid: block the calling process until the target child exits */
int proc_wait_blocking(int pid, int* status) {
    if (!current) return -1;
    /* Find the child */
    int found = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].parent_pid == current->pid) {
            if (pid > 0 && procs[i].pid != pid) continue;
            if (procs[i].state == PROC_ZOMBIE) {
                if (status) *status = procs[i].exit_status;
                int ret_pid = procs[i].pid;
                /* Reap the zombie: close fds, free address space & kernel stack */
                proc_reap(&procs[i]);
                procs[i].state = PROC_FREE;
                return ret_pid;
            }
            if (procs[i].state != PROC_FREE) found = 1;
        }
    }
    if (!found) return -1;

    /* Block until child exits */
    current->state = PROC_BLOCKED;
    /* Store which PID we're waiting for (use current's slot index) */
    int idx = (int)(current - procs);
    wait_target[idx] = pid > 0 ? pid : -1;
    schedule();
    wait_target[idx] = 0;

    /* Re-check after waking up */
    return proc_wait(pid, status);
}

/* ---- Scheduling core ---- */

static struct process* pick_next(void) {
    if (!current) {
        for (int i = 0; i < MAX_PROCS; i++) {
            if (procs[i].state == PROC_RUNNABLE) return &procs[i];
        }
        return NULL;
    }
    int start = (current - procs) + 1;
    for (int i = 0; i < MAX_PROCS; i++) {
        int idx = (start + i) % MAX_PROCS;
        if (procs[idx].state == PROC_RUNNABLE) return &procs[idx];
    }
    return NULL;
}

void schedule(void) {
    if (!scheduler_enabled) return;

    struct process* prev = current;
    struct process* next = pick_next();

    if (!next) {
        if (prev && prev->state == PROC_RUNNING) return;
        return;
    }
    if (prev == next) return;

    if (prev && prev->state == PROC_RUNNING) {
        prev->state = PROC_RUNNABLE;
    }

    current = next;
    next->state = PROC_RUNNING;


    if (prev) {
        context_switch(prev->saved_state, next->saved_state,
                       (uint64_t)next->pml4, next->kernel_stack_top);
    } else {
        /* First process: no old state to save, no ISR frame to swap.
         * context_switch(NULL, ...) will skip the save and ISR frame
         * write, but still switch CR3 and restore callee-saved regs. */
        context_switch(NULL, next->saved_state,
                       (uint64_t)next->pml4, next->kernel_stack_top);
    }
}

/* ---- Initialization ---- */

void sched_init(void) {
    memset(procs, 0, sizeof(procs));
    current = NULL;
    next_pid = 1;
    scheduler_enabled = 0;
    memset(wait_target, 0, sizeof(wait_target));
    pr_info("sched: initialized (max %d processes, real context_switch)\n", MAX_PROCS);
}

void sched_enable(void) {
    scheduler_enabled = 1;
    pr_info("sched: scheduling enabled\n");
}

/* KE-30: sched_disable kept as API but no longer needed as a stopgap.
 * The context_switch triple-fault (bug #3) is now fixed — context_switch
 * v3 uses ISR frame swap + ret instead of direct iretq, so preemption
 * works correctly even during elf_exec. The scheduler stays enabled. */
void sched_disable(void) {
    scheduler_enabled = 0;
}

void sched_start_first(const char* name, const void* elf_data, size_t elf_size) {
    int pid = proc_create(name, elf_data, elf_size);
    if (pid < 0) {
        pr_warn("sched: failed to create first process\n");
        return;
    }

    current = &procs[pid - 1];
    current->state = PROC_RUNNING;

    /* KE-26: Re-enabled preemption. The KE-26 fixes (GDT swap, iretq
     * syscall return, vmm_map_page intermediate USER bits, fork deep-copy)
     * should make the context_switch path stable. With only PID 1 running,
     * schedule() is a no-op (no other runnable process), so this is safe
     * to test. Once fork() creates child processes, real preemption will
     * kick in.
     *
     * KE-32: The ISR-frame GPR offsets in context_switch.asm were reversed
     * (g_isr_frame points at r15 = last push, not rax = first push), and
     * the .isr_swap path had new_kstack_top/new_pml4 swapped on the stack.
     * Both are now fixed, so the preemptive context-switch path is correct
     * even when a second process exists (e.g. after fork()). */
    sched_enable();

    pr_info("sched: starting first process '%s' (pid %d) [preemptive, KE-32]\n", name, pid);

    /* KE-25: Do NOT pre-switch CR3 here. elf_jump_to_user() saves the
     * CURRENT cr3 into save_kernel_cr3 and then switches to the user
     * PML4. If we switch CR3 first, save_kernel_cr3 ends up = the user
     * PML4 (wrong) instead of the boot kernel CR3. Let elf_jump_to_user
     * do the save+switch, exactly like elf_exec. */

    /* KE-25: arm TSS.RSP0 with PID 1's kernel stack BEFORE jumping to
     * ring 3. The first timer IRQ in userspace loads RSP from tss.rsp0;
     * if it's 0/stale the CPU can't push the interrupt frame and
     * triple-faults. elf_exec did this; sched_start_first must too.
     *
     * We use a static 16 KB BSS stack (like elf_exec) for PID 1: it is
     * 16-byte aligned and in the identity-mapped BSS, so both interrupt
     * delivery (TSS.RSP0) and syscall entry (g_syscall_kstack) can
     * reliably push frames here. */
    static uint8_t pid1_kstack[16384] __aligned(16);
    current->kernel_stack = pid1_kstack;
    current->kernel_stack_top = (uint64_t)pid1_kstack + sizeof(pid1_kstack);
    extern void tss_set_rsp0(uint64_t);
    tss_set_rsp0(current->kernel_stack_top);

    /* KE-25: publish the kernel stack top for syscall_entry.asm, which
     * switches RSP to this value on every syscall (syscall does NOT
     * load RSP from TSS, so without this the kernel would push onto the
     * user stack → SMAP triple-fault). */
    extern uint64_t g_syscall_kstack;
    g_syscall_kstack = current->kernel_stack_top;

    elf_jump_to_user(current->entry_point, current->user_stack_ptr,
                     (uintptr_t)current->pml4);
}

/* ---- Misc wrappers ---- */

int sched_get_proc_info(int idx, int* pid, char* name, int* state) {
    if (idx < 0 || idx >= MAX_PROCS) return 0;
    if (procs[idx].state == PROC_FREE) return 0;
    *pid = procs[idx].pid;
    strncpy(name, procs[idx].name, 31);
    name[31] = '\0';
    *state = procs[idx].state;
    return 1;
}

void sched_tick(void) {
    if (scheduler_enabled) {
        sched_check_wakeups();
        schedule();
    }
}

void sched_yield(void) {
    if (scheduler_enabled) {
        schedule();
    }
}

void sched_kill_and_unload(int pid, int exit_status) {
    struct process* p = sched_find_by_pid_impl(pid);
    if (p) {
        p->state = PROC_ZOMBIE;
        p->exit_status = exit_status;
    }
}

int sched_clone_thread(uint64_t child_stack, uint64_t ptid, uint64_t ctid, uint64_t newtls) {
    (void)child_stack; (void)ptid; (void)ctid; (void)newtls;
    return -1;
}
