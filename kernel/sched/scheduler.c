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

/* External: context switch assembly */
extern void context_switch(struct cpu_state* old_state, struct cpu_state* new_state,
                           uint64_t new_pml4, uint64_t new_kstack);

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

/* Clear PAGE_USER bit on all page table entries copied from the kernel,
 * so user processes cannot access kernel memory. User-mapped pages
 * (stack, ELF segments) are mapped separately with PAGE_USER set. */
static void clear_kernel_user_bits(uint64_t* pml4) {
    for (int p4 = 0; p4 < 4; p4++) {
        if (!(pml4[p4] & PAGE_PRESENT)) continue;
        uint64_t* pdpt = (uint64_t*)(pml4[p4] & PTE_PHYS_MASK);
        for (int p3 = 0; p3 < 512; p3++) {
            if (!(pdpt[p3] & PAGE_PRESENT)) continue;
            if (pdpt[p3] & PAGE_HUGE) { pdpt[p3] &= ~PAGE_USER; continue; }
            uint64_t* pd = (uint64_t*)(pdpt[p3] & PTE_PHYS_MASK);
            for (int p2 = 0; p2 < 512; p2++) {
                if (!(pd[p2] & PAGE_PRESENT)) continue;
                if (pd[p2] & PAGE_HUGE) { pd[p2] &= ~PAGE_USER; continue; }
                uint64_t* pt = (uint64_t*)(pd[p2] & PTE_PHYS_MASK);
                for (int p1 = 0; p1 < 512; p1++) {
                    if (!(pt[p1] & PAGE_PRESENT)) continue;
                    pt[p1] &= ~PAGE_USER;
                }
            }
        }
    }
}

static uint64_t* create_proc_pml4(void) {
    phys_addr_t pml4_phys = pmm_alloc_page();
    if (!pml4_phys) return NULL;
    uint64_t* pml4 = (uint64_t*)pml4_phys;
    memset(pml4, 0, PAGE_SIZE);

    extern uint64_t boot_pml4[];
    pml4[0] = boot_pml4[0];
    pml4[1] = boot_pml4[1];
    pml4[2] = boot_pml4[2];
    pml4[3] = boot_pml4[3];

    clear_kernel_user_bits(pml4);

    return pml4;
}

static void proc_map_page(struct process* p, uint64_t vaddr, uint64_t phys, uint64_t flags) {
    vmm_map_page(p->pml4, vaddr, phys, flags);
}

/* ---- COW fork: share user pages instead of deep-copying ----
 *
 * Instead of allocating new physical pages and memcpy-ing the parent's
 * contents (the old copy_user_pages approach), we now share the same
 * physical pages between parent and child. Both processes get their
 * PTEs marked read-only + PAGE_COW. The first write to any shared
 * page triggers a page fault, which the page_fault_handler in
 * kernel/mm/page_fault.c resolves by allocating a private copy
 * (or simply making the page writable if refcount == 1, meaning
 * the other process already exited or created its own copy).
 *
 * For each user page in the parent's PML4:
 *   1. Mark the parent's PTE as read-only + PAGE_COW
 *      (clear PAGE_WRITABLE, add PAGE_COW).
 *   2. Map the same physical page in the child's PML4 with the same
 *      read-only + PAGE_COW flags.
 *   3. Increment the physical page's reference count (pmm_refcount_inc)
 *      because the child now also references it.
 *
 * After sharing, flush the parent's TLB by reloading CR3 so that
 * subsequent writes by the parent will correctly fault (the TLB
 * still has old writable entries from before the fork). */
static void cow_share_pages(struct process* parent, struct process* child) {
    for (int p4 = 4; p4 < 512; p4++) { /* PML4 indices 0-3 are kernel */
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
                    /* Extract physical address (bits 12-51) and flags
                     * (low bits 0-11 + high bits 52-63 including NX).
                     * We must preserve PAGE_NX and other high flag bits
                     * when computing COW flags for the child mapping. */
                    phys_addr_t phys_addr = pt[p1] & 0x000FFFFFFFFFF000ULL;
                    uint64_t low_flags    = pt[p1] & 0xFFFULL;
                    uint64_t high_flags   = pt[p1] & 0xFF00000000000000ULL;
                    uint64_t flags        = low_flags | high_flags;

                    /* Step 1: mark parent's PTE as read-only + COW */
                    pt[p1] = (pt[p1] & ~PAGE_WRITABLE) | PAGE_COW;

                    /* Step 2: map same physical page in child (read-only + COW) */
                    uint64_t cow_flags = (flags & ~PAGE_WRITABLE) | PAGE_COW;
                    vmm_map_page(child->pml4, vaddr, phys_addr, cow_flags);

                    /* Step 3: increment refcount (child now also references it) */
                    pmm_refcount_inc(phys_addr);
                }
            }
        }
    }

    /* Flush parent's TLB: the PTE modifications above changed several
     * entries from writable to read-only. Without a TLB flush, the CPU
     * would still use the stale writable entries and writes would NOT
     * trigger the COW page fault. Reloading CR3 flushes all non-global
     * TLB entries (our user pages don't have PAGE_GLOBAL set). */
    write_cr3((uintptr_t)parent->pml4);
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

    p->saved_state->ss = 0x23;
    p->saved_state->rsp = p->user_stack_ptr;
    p->saved_state->rflags = 0x202;
    p->saved_state->cs = 0x1B;
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

    /* COW fork: share parent's user pages instead of deep-copying.
     * Both parent and child PTEs get PAGE_COW + read-only.
     * First write triggers page fault → private copy allocated. */
    cow_share_pages(current, child);

    /* Copy stack_bottom from parent (both start with same stack range) */
    child->stack_bottom = current->stack_bottom;

    child->kernel_stack = kmalloc(16384);
    if (!child->kernel_stack) { child->state = PROC_FREE; return -1; }
    child->kernel_stack_top = (uint64_t)child->kernel_stack + 16384;
    child->saved_state = (struct cpu_state*)(child->kernel_stack_top - sizeof(struct cpu_state));
    memcpy(child->saved_state, current->saved_state, sizeof(struct cpu_state));

    child->saved_state->rax = 0;

    /* Copy parent's fd table to child (shared VFS resources,
     * independent offsets) */
    fd_table_copy(current, child);

    pr_info("sched: forked process %d from %d (COW)\n", child->pid, current->pid);
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
    (void)ms;
    /* Simplified: yield the CPU. A real impl would set a timer wakeup. */
    if (scheduler_enabled) schedule();
}

void task_set_priority(int p) { (void)p; }

/* Called on timer tick to check for processes that need waking */
void sched_check_wakeups(void) {
    /* Check if any blocked process is waiting for a zombie child */
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == PROC_BLOCKED && wait_target[i] > 0) {
            struct process* target = sched_find_by_pid_impl(wait_target[i]);
            if (target && target->state == PROC_ZOMBIE) {
                procs[i].state = PROC_RUNNABLE;
                wait_target[i] = 0;
            }
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

void sched_start_first(const char* name, const void* elf_data, size_t elf_size) {
    int pid = proc_create(name, elf_data, elf_size);
    if (pid < 0) {
        pr_warn("sched: failed to create first process\n");
        return;
    }

    current = &procs[pid - 1];
    current->state = PROC_RUNNING;

    sched_enable();

    pr_info("sched: starting first process '%s' (pid %d)\n", name, pid);

    uint64_t old_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"((uint64_t)current->pml4));
    save_kernel_cr3 = old_cr3;

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
