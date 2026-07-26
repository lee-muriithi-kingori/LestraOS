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
        uint64_t* pdpt = (uint64_t*)(pml4[p4] & ~0xFFFULL);
        for (int p3 = 0; p3 < 512; p3++) {
            if (!(pdpt[p3] & PAGE_PRESENT)) continue;
            if (pdpt[p3] & PAGE_HUGE) { pdpt[p3] &= ~PAGE_USER; continue; }
            uint64_t* pd = (uint64_t*)(pdpt[p3] & ~0xFFFULL);
            for (int p2 = 0; p2 < 512; p2++) {
                if (!(pd[p2] & PAGE_PRESENT)) continue;
                if (pd[p2] & PAGE_HUGE) { pd[p2] &= ~PAGE_USER; continue; }
                uint64_t* pt = (uint64_t*)(pd[p2] & ~0xFFFULL);
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

/* Copy all user pages (PAGE_USER set) from parent's address space to child's.
 * FIX: was only walking PML4 indices 0-3 (the shared kernel range copied
 * by pointer in create_proc_pml4()), which never contains the user stack —
 * proc_setup_stack() maps the stack at 0x00007FFFFFE00000, which falls in
 * PML4 index 255. That meant fork() silently never copied the child's
 * stack; the child would page-fault (or worse) the instant it touched it.
 * Now walks the full PML4 range so every private mapping — stack, ELF
 * segments, anything else proc_map_page put outside indices 0-3 — actually
 * gets copied. vmm_map_page()/proc_map_page() creates any missing
 * intermediate page tables in the child on demand, so no extra setup is
 * needed here. */
static void copy_user_pages(struct process* parent, struct process* child) {
    for (int p4 = 0; p4 < 512; p4++) {
        if (!(parent->pml4[p4] & PAGE_PRESENT)) continue;
        uint64_t* pdpt = (uint64_t*)(parent->pml4[p4] & ~0xFFFULL);
        for (int p3 = 0; p3 < 512; p3++) {
            if (!(pdpt[p3] & PAGE_PRESENT)) continue;
            if (pdpt[p3] & PAGE_HUGE) continue;
            uint64_t* pd = (uint64_t*)(pdpt[p3] & ~0xFFFULL);
            for (int p2 = 0; p2 < 512; p2++) {
                if (!(pd[p2] & PAGE_PRESENT)) continue;
                if (pd[p2] & PAGE_HUGE) continue;
                uint64_t* pt = (uint64_t*)(pd[p2] & ~0xFFFULL);
                for (int p1 = 0; p1 < 512; p1++) {
                    if (!(pt[p1] & PAGE_PRESENT)) continue;
                    if (!(pt[p1] & PAGE_USER)) continue;

                    uint64_t vaddr = ((uint64_t)p4 << 39) | ((uint64_t)p3 << 30) |
                                     ((uint64_t)p2 << 21) | ((uint64_t)p1 << 12);
                    uint64_t phys_addr = pt[p1] & ~0xFFFULL;
                    uint64_t flags = pt[p1] & 0xFFFULL;

                    phys_addr_t new_page = pmm_alloc_page();
                    if (!new_page) continue;
                    memcpy((void*)new_page, (void*)phys_addr, PAGE_SIZE);
                    proc_map_page(child, vaddr, new_page, flags);
                }
            }
        }
    }
}

static void proc_setup_stack(struct process* p) {
    size_t stack_pages = (256 * 1024) / PAGE_SIZE;
    uint64_t stack_top = 0x00007FFFFFE00000ULL;
    for (size_t i = 0; i < stack_pages; i++) {
        phys_addr_t phys = pmm_alloc_page();
        if (!phys) return;
        memset((void*)phys, 0, PAGE_SIZE);
        proc_map_page(p, stack_top - (256 * 1024) + i * PAGE_SIZE,
                      phys, PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE);
    }
    p->user_stack_ptr = stack_top - 16;
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

    p->pml4 = create_proc_pml4();
    if (!p->pml4) { p->state = PROC_FREE; return -1; }

    p->kernel_stack = kmalloc(16384);
    if (!p->kernel_stack) { p->state = PROC_FREE; return -1; }
    p->kernel_stack_top = (uint64_t)p->kernel_stack + 16384;

    p->saved_state = (struct cpu_state*)(p->kernel_stack_top - sizeof(struct cpu_state));
    memset(p->saved_state, 0, sizeof(struct cpu_state));

    proc_setup_stack(p);

    p->saved_state->ss = 0x23;
    p->saved_state->rsp = p->user_stack_ptr;
    p->saved_state->rflags = 0x202;
    p->saved_state->cs = 0x1B;
    p->saved_state->rip = p->entry_point;

    pr_info("sched: created process %d '%s'\n", p->pid, p->name);
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
    copy_user_pages(current, child);

    child->kernel_stack = kmalloc(16384);
    if (!child->kernel_stack) { child->state = PROC_FREE; return -1; }
    child->kernel_stack_top = (uint64_t)child->kernel_stack + 16384;
    child->saved_state = (struct cpu_state*)(child->kernel_stack_top - sizeof(struct cpu_state));
    memcpy(child->saved_state, current->saved_state, sizeof(struct cpu_state));

    child->saved_state->rax = 0;

    pr_info("sched: forked process %d from %d\n", child->pid, current->pid);
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
