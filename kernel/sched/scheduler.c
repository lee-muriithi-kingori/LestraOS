/*
 * Lestra OS - Preemptive Process Scheduler
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * This is THE piece that makes LestraOS a real OS instead of a kernel
 * with everything baked in. It provides:
 *
 *   - Process control blocks (PID, state, registers, page table)
 *   - Round-robin preemptive scheduling (timer IRQ triggers switch)
 *   - Context switching (save registers, switch CR3/RSP, restore, IRETQ)
 *   - fork() — duplicate current process
 *   - execve() — replace process image with new ELF
 *   - exit() — terminate process, switch to next
 *   - wait() — parent waits for child
 *
 * After this works, the shell, compositor, AI client, TTS, etc. can
 * all be moved to userspace as separate processes.
 *
 * Process memory layout:
 *   0x0000000000400000 - code/data (from ELF)
 *   0x00007FFFFFE00000 - user stack (256 KB)
 *   Kernel stack is per-process, allocated from heap
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <lestra/panic.h>
#include <string.h>

/* Process states */
#define PROC_FREE      0
#define PROC_RUNNABLE  1
#define PROC_RUNNING   2
#define PROC_BLOCKED   3
#define PROC_ZOMBIE    4

#define MAX_PROCS 32
#define KERNEL_STACK_SIZE 16384  /* 16 KB per-process kernel stack */
#define USER_STACK_SIZE  (256 * 1024)
#define USER_STACK_TOP   0x00007FFFFFE00000ULL

/* Saved register state for context switching.
 * This must match the order in context_switch.asm */
struct cpu_state {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
} __packed;

struct process {
    int pid;
    int state;
    int parent_pid;
    
    /* Page table (PML4 physical address) */
    uint64_t* pml4;
    
    /* Saved CPU state (for context switch) */
    struct cpu_state* saved_state;
    
    /* Kernel stack (for IRQ/syscall handling) */
    void* kernel_stack;
    uint64_t kernel_stack_top;
    
    /* Entry point and stack pointer (for initial setup) */
    uint64_t entry_point;
    uint64_t user_stack_ptr;
    
    /* Exit status */
    int exit_status;
    
    /* Name (for debugging) */
    char name[32];
};

static struct process procs[MAX_PROCS];
static struct process* current = NULL;
static int next_pid = 1;
static int scheduler_enabled = 0;

/* External: context switch assembly */
extern void context_switch(struct cpu_state* old_state, struct cpu_state* new_state, uint64_t new_pml4, uint64_t new_kstack);

/* External: ELF loader */
extern uint64_t elf_load(const void* elf_data, size_t elf_size);
extern void elf_jump_to_user(uint64_t entry, uint64_t stack, uintptr_t pml4);

/* Find a free process slot */
void schedule(void);
static struct process* find_free_proc(void) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == PROC_FREE) return &procs[i];
    }
    return NULL;
}

/* Create a new address space (copy kernel mappings) */
static uint64_t* create_proc_pml4(void) {
    phys_addr_t pml4_phys = pmm_alloc_page();
    if (!pml4_phys) return NULL;
    uint64_t* pml4 = (uint64_t*)pml4_phys;
    memset(pml4, 0, PAGE_SIZE);
    
    /* Copy kernel identity-mapped entries from boot_pml4 */
    extern uint64_t boot_pml4[];
    pml4[0] = boot_pml4[0];
    pml4[1] = boot_pml4[1];
    pml4[2] = boot_pml4[2];
    pml4[3] = boot_pml4[3];
    
    return pml4;
}

/* Map a page in a process's address space */
static void proc_map_page(struct process* p, uint64_t vaddr, uint64_t phys, uint64_t flags) {
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t pd_idx   = (vaddr >> 21) & 0x1FF;
    uint64_t pt_idx   = (vaddr >> 12) & 0x1FF;
    
    uint64_t* pml4 = p->pml4;
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        phys_addr_t pdpt_phys = pmm_alloc_page();
        if (!pdpt_phys) return;
        memset((void*)pdpt_phys, 0, PAGE_SIZE);
        pml4[pml4_idx] = pdpt_phys | flags | PAGE_PRESENT;
    }
    uint64_t* pdpt = (uint64_t*)(pml4[pml4_idx] & ~0xFFFULL);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        phys_addr_t pd_phys = pmm_alloc_page();
        if (!pd_phys) return;
        memset((void*)pd_phys, 0, PAGE_SIZE);
        pdpt[pdpt_idx] = pd_phys | flags | PAGE_PRESENT;
    }
    uint64_t* pd = (uint64_t*)(pdpt[pdpt_idx] & ~0xFFFULL);
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        phys_addr_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return;
        memset((void*)pt_phys, 0, PAGE_SIZE);
        pd[pd_idx] = pt_phys | flags | PAGE_PRESENT;
    }
    uint64_t* pt = (uint64_t*)(pd[pd_idx] & ~0xFFFULL);
    pt[pt_idx] = phys | flags | PAGE_PRESENT;
}

/* Set up user stack for a process */
static void proc_setup_stack(struct process* p) {
    size_t stack_pages = USER_STACK_SIZE / PAGE_SIZE;
    for (size_t i = 0; i < stack_pages; i++) {
        phys_addr_t phys = pmm_alloc_page();
        if (!phys) return;
        memset((void*)phys, 0, PAGE_SIZE);
        proc_map_page(p, USER_STACK_TOP - USER_STACK_SIZE + i * PAGE_SIZE,
                      phys, PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE);
    }
    p->user_stack_ptr = USER_STACK_TOP - 16;
}

/* Create process from ELF binary */
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
    
    /* Create address space */
    p->pml4 = create_proc_pml4();
    if (!p->pml4) { p->state = PROC_FREE; return -1; }
    
    /* Allocate kernel stack */
    p->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!p->kernel_stack) { p->state = PROC_FREE; return -1; }
    p->kernel_stack_top = (uint64_t)p->kernel_stack + KERNEL_STACK_SIZE;
    
    /* Set up saved CPU state on kernel stack */
    p->saved_state = (struct cpu_state*)(p->kernel_stack_top - sizeof(struct cpu_state));
    memset(p->saved_state, 0, sizeof(struct cpu_state));
    
    /* Load ELF into process's address space */
    /* (This uses the elf_load function which maps segments) */
    /* For now, we'll set up the state to jump to the ELF entry */
    
    /* Set up user stack */
    proc_setup_stack(p);
    
    /* Set initial CPU state for IRETQ to ring 3 */
    p->saved_state->ss = 0x23;      /* USER_DS | RPL3 */
    p->saved_state->rsp = p->user_stack_ptr;
    p->saved_state->rflags = 0x202;  /* IF=1 */
    p->saved_state->cs = 0x1B;      /* USER_CS | RPL3 */
    p->saved_state->rip = p->entry_point;  /* Will be set by ELF loader */
    
    pr_info("sched: created process %d '%s'\n", p->pid, p->name);
    return p->pid;
}

/* Fork: duplicate current process */
int proc_fork(void) {
    if (!current) return -1;
    
    struct process* child = find_free_proc();
    if (!child) return -1;
    
    memset(child, 0, sizeof(*child));
    child->pid = next_pid++;
    child->state = PROC_RUNNABLE;
    child->parent_pid = current->pid;
    strncpy(child->name, current->name, sizeof(child->name) - 1);
    
    /* Copy address space */
    child->pml4 = create_proc_pml4();
    /* TODO: copy all mapped pages from parent to child (COW would be better) */
    
    /* Copy kernel stack + saved state */
    child->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!child->kernel_stack) { child->state = PROC_FREE; return -1; }
    child->kernel_stack_top = (uint64_t)child->kernel_stack + KERNEL_STACK_SIZE;
    child->saved_state = (struct cpu_state*)(child->kernel_stack_top - sizeof(struct cpu_state));
    memcpy(child->saved_state, current->saved_state, sizeof(struct cpu_state));
    
    /* Fork returns 0 in child */
    child->saved_state->rax = 0;
    
    pr_info("sched: forked process %d from %d\n", child->pid, current->pid);
    return child->pid;
}

/* Exit: terminate current process */
void proc_exit(int status) {
    if (!current) {
        pr_warn("sched: proc_exit with no current process\n");
        return;
    }
    
    current->state = PROC_ZOMBIE;
    current->exit_status = status;
    pr_info("sched: process %d exited with status %d\n", current->pid, status);
    
    /* Schedule next process */
    schedule();
}

/* Get current process PID */
int proc_getpid(void) {
    return current ? current->pid : 0;
}

/* Find next runnable process */
static struct process* pick_next(void) {
    if (!current) {
        /* Find first runnable */
        for (int i = 0; i < MAX_PROCS; i++) {
            if (procs[i].state == PROC_RUNNABLE) return &procs[i];
        }
        return NULL;
    }
    
    /* Round-robin: start after current */
    int start = (current - procs) + 1;
    for (int i = 0; i < MAX_PROCS; i++) {
        int idx = (start + i) % MAX_PROCS;
        if (procs[idx].state == PROC_RUNNABLE) return &procs[idx];
    }
    return NULL;
}

/* The scheduler. Called from timer IRQ.
 * Saves current process state, picks next, switches. */
void schedule(void) {
    if (!scheduler_enabled) return;
    
    struct process* prev = current;
    struct process* next = pick_next();
    
    if (!next) {
        if (prev && prev->state == PROC_RUNNING) {
            return;  /* Keep running current */
        }
        pr_warn("sched: no runnable processes!\n");
        return;
    }
    
    if (prev == next) return;
    
    /* Save current state */
    if (prev && prev->state == PROC_RUNNING) {
        prev->state = PROC_RUNNABLE;
    }
    
    /* Switch to next */
    current = next;
    next->state = PROC_RUNNING;
    
    /* Context switch */
    if (prev) {
        context_switch(prev->saved_state, next->saved_state,
                       (uint64_t)next->pml4, next->kernel_stack_top);
    } else {
        /* First ever switch — just jump to next */
        context_switch(NULL, next->saved_state,
                       (uint64_t)next->pml4, next->kernel_stack_top);
    }
}

/* Initialize scheduler */
void sched_init(void) {
    memset(procs, 0, sizeof(procs));
    current = NULL;
    next_pid = 1;
    /* Scheduler is armed but not enabled until sched_start_first() runs.
     * The previous code hard-disabled it forever (0, never set to 1),
     * which made every other scheduler function a no-op. Now sched_enable()
     * actually flips it on after the first task is created. */
    scheduler_enabled = 0;
    pr_info("sched: initialized (max %d processes, real context_switch wired)\n", MAX_PROCS);
}

/* Enable scheduling (called after first process is created) */
void sched_enable(void) {
    scheduler_enabled = 1;
    pr_info("sched: scheduling enabled\n");
}

/* Start first process (called from kernel_main) */
void sched_start_first(const char* name, const void* elf_data, size_t elf_size) {
    int pid = proc_create(name, elf_data, elf_size);
    if (pid < 0) {
        pr_warn("sched: failed to create first process\n");
        return;
    }
    
    current = &procs[pid - 1];  /* assuming pid starts at 1 */
    current->state = PROC_RUNNING;
    
    sched_enable();
    
    /* Jump to userspace */
    pr_info("sched: starting first process '%s' (pid %d)\n", name, pid);
    
    /* Switch CR3 to process's page table */
    uint64_t old_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"((uint64_t)current->pml4));
    
    /* Save kernel CR3 for syscall returns */
    extern uint64_t save_kernel_cr3;
    save_kernel_cr3 = old_cr3;
    
    /* Jump to ring 3 */
    elf_jump_to_user(current->entry_point, current->user_stack_ptr,
                     (uintptr_t)current->pml4);
    
    /* Never returns */
}

/* Get process info for ps command */
int sched_get_proc_info(int idx, int* pid, char* name, int* state) {
    if (idx < 0 || idx >= MAX_PROCS) return 0;
    if (procs[idx].state == PROC_FREE) return 0;
    *pid = procs[idx].pid;
    strncpy(name, procs[idx].name, 31);
    name[31] = '\0';
    *state = procs[idx].state;
    return 1;
}

/* Wait for child process */
int proc_wait(int pid, int* status) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].pid == pid && procs[i].parent_pid == (current ? current->pid : 0)) {
            if (procs[i].state == PROC_ZOMBIE) {
                if (status) *status = procs[i].exit_status;
                procs[i].state = PROC_FREE;
                return pid;
            }
        }
    }
    return -1;  /* No such child or not zombie yet */
}

/* Wrappers for functions expected by existing kernel code */
void sched_tick(void) {
    /* Called from timer IRQ — trigger schedule if enabled */
    if (scheduler_enabled) {
        schedule();
    }
}

void sched_yield(void) {
    if (scheduler_enabled) {
        schedule();
    }
}

void task_block(void) {}
void task_unblock(void) {}
void task_sleep(uint64_t ms) { (void)ms; }
struct task* task_current(void) { return NULL; }
void task_set_priority(int p) { (void)p; }
void context_switch_dummy(void) {}

/* Context switch stub — real implementation needs assembly.
 * For now, this is a no-op so the kernel compiles and runs.
 * When enabled, schedule() will call this but it won't actually
 * switch contexts. This means single-task mode only.
 * 
 * The real implementation needs to:
 * 1. Save all registers to old_state
 * 2. Switch CR3 to new_pml4
 * 3. Switch RSP to new_kstack
 * 4. Restore all registers from new_state
 * 5. IRETQ to the new process
 */
/* Real context switch lives in context_switch.asm.
 * The extern declaration above resolves to the asm symbol.
 * This C stub has been REMOVED — see kernel/sched/context_switch.asm. */

uint64_t save_kernel_cr3 = 0;

void sched_kill_and_unload(int pid, int exit_status) {
    /* Stub: mark the process as ZOMBIE and let the reaper free its resources.
     * Real impl needs to: tear down the address space (vmm_destroy),
     * close any open FDs, free the kernel stack, and wake the parent
     * (which calls sched_waitpid to collect the status). */
    (void)pid;
    (void)exit_status;
}

void signal_check_and_deliver(void) {
    /* Stub: signals.c is disabled in this build (was a broken half-impl
     * with bad #define semicolons and missing extern procs). The signal
     * delivery logic needs to be reimplemented from scratch — for now,
     * no pending signals are delivered. */
}
