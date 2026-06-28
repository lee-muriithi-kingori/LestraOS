/*
 * Lestra OS - Round-Robin Process Scheduler
 * Copyright (c) 2026 lestramk.org
 */

#include <lestra/types.h>
#include <lestra/sched.h>
#include <lestra/mm.h>
#include <lestra/timer.h>
#include <lestra/printk.h>
#include <lestra/panic.h>

/* Task table */
static struct task task_table[MAX_TASKS];
static struct task* current_task = NULL;
static struct task* ready_queue_head = NULL;
static struct task* ready_queue_tail = NULL;
static pid_t next_pid = 1;
static uint32_t task_count = 0;

/* Idle task */
static void idle_task(void) {
    while (1) {
        sti();
        hlt();
        cli();
    }
}

/* Get next available PID */
static pid_t alloc_pid(void) {
    return next_pid++;
}

/* Find unused task slot */
static struct task* alloc_task_slot(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].state == TASK_UNUSED) {
            return &task_table[i];
        }
    }
    return NULL;
}

/* Add task to ready queue */
static void enqueue_ready(struct task* task) {
    task->next = NULL;
    task->prev = ready_queue_tail;
    
    if (ready_queue_tail) {
        ready_queue_tail->next = task;
    } else {
        ready_queue_head = task;
    }
    ready_queue_tail = task;
}

/* Remove task from ready queue */
static void dequeue_ready(struct task* task) {
    if (task->prev) {
        task->prev->next = task->next;
    } else {
        ready_queue_head = task->next;
    }
    
    if (task->next) {
        task->next->prev = task->prev;
    } else {
        ready_queue_tail = task->prev;
    }
    
    task->next = NULL;
    task->prev = NULL;
}

void sched_init(void) {
    /* Clear task table */
    for (int i = 0; i < MAX_TASKS; i++) {
        task_table[i].state = TASK_UNUSED;
        task_table[i].next = NULL;
        task_table[i].prev = NULL;
    }
    
    task_count = 0;
    next_pid = 1;
    
    /* Create idle task */
    struct task* idle = alloc_task_slot();
    idle->pid = alloc_pid();
    strncpy(idle->name, "idle", TASK_NAME_LEN);
    idle->state = TASK_READY;
    idle->priority = 31;
    idle->page_table = (uintptr_t*)(read_cr3() + KERNEL_VMA);
    
    /* Allocate stack */
    phys_addr_t stack_phys = pmm_alloc_pages(4);
    idle->stack_base = stack_phys + KERNEL_VMA;
    idle->stack_size = 4 * PAGE_SIZE;
    idle->rsp = idle->stack_base + idle->stack_size;
    idle->rip = (uint64_t)idle_task;
    
    enqueue_ready(idle);
    current_task = idle;
    
    /* Install timer tick handler */
    timer_set_handler(sched_tick);
    
    pr_info("Scheduler initialized (round-robin)\n");
}

struct task* task_create(const char* name, void (*entry)(void)) {
    struct task* task = alloc_task_slot();
    if (!task) {
        pr_err("Task create: No free slots\n");
        return NULL;
    }
    
    task->pid = alloc_pid();
    task->ppid = current_task ? current_task->pid : 0;
    strncpy(task->name, name, TASK_NAME_LEN);
    task->state = TASK_READY;
    task->priority = 16;  /* Default priority */
    task->ticks = 0;
    task->cpu_time = 0;
    
    /* Create address space */
    task->page_table = vmm_create_address_space();
    if (!task->page_table) {
        task->state = TASK_UNUSED;
        return NULL;
    }
    
    /* Allocate kernel stack */
    phys_addr_t stack_phys = pmm_alloc_pages(DEFAULT_STACK / PAGE_SIZE);
    if (!stack_phys) {
        vmm_destroy_address_space(task->page_table);
        task->state = TASK_UNUSED;
        return NULL;
    }
    
    task->stack_base = stack_phys + KERNEL_VMA;
    task->stack_size = DEFAULT_STACK;
    
    /* Set up initial stack */
    task->rsp = task->stack_base + task->stack_size;
    task->rip = (uint64_t)entry;
    
    enqueue_ready(task);
    task_count++;
    
    pr_debug("Created task '%s' (pid=%d)\n", name, task->pid);
    return task;
}

void task_exit(int code) {
    (void)code;
    
    if (!current_task || current_task->pid == 0) {
        panic("Idle task tried to exit");
    }
    
    dequeue_ready(current_task);
    current_task->state = TASK_ZOMBIE;
    task_count--;
    
    pr_debug("Task '%s' (pid=%d) exited with code %d\n",
             current_task->name, current_task->pid, code);
    
    /* Schedule next task */
    sched_yield();
}

void task_block(enum task_state state) {
    if (!current_task) return;
    
    current_task->state = state;
    dequeue_ready(current_task);
    sched_yield();
}

void task_unblock(struct task* task) {
    if (task->state == TASK_BLOCKED || task->state == TASK_SLEEPING) {
        task->state = TASK_READY;
        enqueue_ready(task);
    }
}

void task_sleep(uint64_t ms) {
    if (!current_task) return;
    
    current_task->sleep_until = timer_get_ms() + ms;
    task_block(TASK_SLEEPING);
}

struct task* task_current(void) {
    return current_task;
}

void task_set_priority(struct task* task, int priority) {
    if (priority < 0) priority = 0;
    if (priority > 31) priority = 31;
    task->priority = priority;
}

void sched_tick(void) {
    if (!current_task) return;
    
    current_task->ticks++;
    current_task->cpu_time++;
    
    /* Check sleeping tasks */
    uint64_t now = timer_get_ms();
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].state == TASK_SLEEPING &&
            now >= task_table[i].sleep_until) {
            task_unblock(&task_table[i]);
        }
    }
    
    /* Time slice: switch every 10 ticks */
    if (current_task->ticks >= (10 - current_task->priority / 4)) {
        current_task->ticks = 0;
        
        if (ready_queue_head && ready_queue_head != current_task) {
            sched_yield();
        }
    }
}

void sched_yield(void) {
    if (!ready_queue_head) return;
    
    struct task* prev = current_task;
    struct task* next = ready_queue_head;
    
    /* Move current to end of queue if still ready */
    if (prev->state == TASK_RUNNING || prev->state == TASK_READY) {
        prev->state = TASK_READY;
        dequeue_ready(prev);
        enqueue_ready(prev);
    }
    
    next->state = TASK_RUNNING;
    current_task = next;
    
    if (prev != next) {
        /* Update CR3 if different address space */
        if (prev->cr3 != next->cr3) {
            write_cr3((uint64_t)next->page_table - KERNEL_VMA);
        }
        
        context_switch(prev, next);
    }
}

/* Context switch assembly */
__asm__(
    ".global context_switch\n"
    "context_switch:\n"
    "    push rbx\n"
    "    push rbp\n"
    "    push r12\n"
    "    push r13\n"
    "    push r14\n"
    "    push r15\n"
    "    mov [rdi + 8], rsp\n"   /* Save rsp to prev->rsp */
    "    mov rsp, [rsi + 8]\n"   /* Load rsp from next->rsp */
    "    pop r15\n"
    "    pop r14\n"
    "    pop r13\n"
    "    pop r12\n"
    "    pop rbp\n"
    "    pop rbx\n"
    "    ret\n"
);
