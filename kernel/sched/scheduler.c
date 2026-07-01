/*
 * Lestra OS - Process Scheduler
 * Copyright (c) 2026 lestramk.org
 *
 * Fair preemptive scheduler with round-robin quantum.
 * Supports 256 tasks with priority levels.
 */

#include <lestra/types.h>
#include <lestra/sched.h>
#include <lestra/mm.h>
#include <lestra/printk.h>
#include <lestra/panic.h>

#define STACK_SIZE      8192           /* 8KB kernel stack */
#define NUM_PRIORITIES  8
#define QUANTUM         10             /* Timer ticks */
#define IDLE_PID        0

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_ZOMBIE
} task_state_t;

struct task {
    uint32_t pid;
    char name[TASK_NAME_LEN];
    task_state_t state;
    int priority;           /* 0-7, lower is higher priority */
    uint32_t quantum;
    uintptr_t stack_top;
    uintptr_t stack_bottom;
    uint64_t runtime;       /* Total CPU time in ms */
    uint64_t vruntime;      /* For fair scheduling */
    struct task* parent;
    struct task* next;
    struct task* prev;
    uint32_t exit_code;
    uint64_t regs[15];      /* Saved registers */
};

/* Task lists per priority */
static struct task* ready_queues[NUM_PRIORITIES];
static struct task task_table[MAX_TASKS];
static uint32_t next_pid = 1;
static struct task* current_task = NULL;
static uint32_t task_count = 0;

static struct task* idle_task;

/* Scheduler lock */
static volatile int sched_lock = 0;

static inline void sched_lock_acquire(void) {
    while (__sync_lock_test_and_set(&sched_lock, 1));
}

static inline void sched_lock_release(void) {
    __sync_lock_release(&sched_lock);
}

void scheduler_init(void) {
    memset(task_table, 0, sizeof(task_table));

    for (int i = 0; i < NUM_PRIORITIES; i++) {
        ready_queues[i] = NULL;
    }

    /* Create idle task */
    idle_task = &task_table[IDLE_PID];
    idle_task->pid = IDLE_PID;
    strncpy(idle_task->name, "idle", TASK_NAME_LEN - 1);
    idle_task->name[TASK_NAME_LEN - 1] = '\0';
    idle_task->state = TASK_READY;
    idle_task->priority = NUM_PRIORITIES - 1;
    idle_task->quantum = QUANTUM;
    current_task = idle_task;
    task_count = 1;

    pr_info("Scheduler initialized\n");
}

uint32_t task_create(const char* name, void (*entry)(void), int priority) {
    if (!name || !entry) return 0;
    if (priority < 0 || priority >= NUM_PRIORITIES) return 0;

    sched_lock_acquire();

    /* Find free task slot */
    struct task* task = NULL;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].state == TASK_UNUSED) {
            task = &task_table[i];
            break;
        }
    }

    if (!task) {
        sched_lock_release();
        return 0;
    }

    /* Allocate kernel stack */
    task->stack_bottom = (uintptr_t)kmalloc(STACK_SIZE);
    if (!task->stack_bottom) {
        sched_lock_release();
        return 0;
    }
    task->stack_top = task->stack_bottom + STACK_SIZE;

    /* Initialize task */
    task->pid = next_pid++;
    /* FIX #11: Ensure null termination of task name */
    strncpy(task->name, name, TASK_NAME_LEN - 1);
    task->name[TASK_NAME_LEN - 1] = '\0';
    task->state = TASK_READY;
    task->priority = priority;
    task->quantum = QUANTUM;
    task->runtime = 0;
    task->vruntime = 0;
    task->parent = current_task;
    task->exit_code = 0;
    task->next = NULL;
    task->prev = NULL;

    /* Setup initial stack frame */
    uint64_t* stack = (uint64_t*)task->stack_top;
    stack[-1] = (uint64_t)entry;    /* Entry point */
    stack[-2] = 0;                   /* Dummy return address */
    task->regs[13] = (uint64_t)&stack[-2]; /* RSP */
    task->regs[14] = 0x202;          /* RFLAGS (interrupts enabled) */

    /* Add to ready queue */
    task->next = ready_queues[priority];
    if (ready_queues[priority]) {
        ready_queues[priority]->prev = task;
    }
    ready_queues[priority] = task;

    task_count++;
    sched_lock_release();

    pr_debug("Created task '%s' (pid=%u, priority=%d)\n", name, task->pid, priority);
    return task->pid;
}

void task_exit(uint32_t exit_code) {
    sched_lock_acquire();

    if (!current_task || current_task->pid == IDLE_PID) {
        sched_lock_release();
        return;
    }

    current_task->state = TASK_ZOMBIE;
    current_task->exit_code = exit_code;

    /* Reparent children to init */
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].parent == current_task) {
            task_table[i].parent = &task_table[0];
        }
    }

    task_count--;
    sched_lock_release();

    /* Yield immediately */
    sched_yield();
}

void task_sleep(uint32_t ms) {
    if (!current_task) return;

    sched_lock_acquire();
    current_task->state = TASK_BLOCKED;
    sched_lock_release();

    /* TODO: Add to sleep queue */

    sched_yield();
}

void task_wakeup(uint32_t pid) {
    sched_lock_acquire();

    if (pid < MAX_TASKS) {
        if (task_table[pid].state == TASK_BLOCKED) {
            task_table[pid].state = TASK_READY;

            int prio = task_table[pid].priority;
            task_table[pid].next = ready_queues[prio];
            if (ready_queues[prio]) {
                ready_queues[prio]->prev = &task_table[pid];
            }
            ready_queues[prio] = &task_table[pid];
        }
    }

    sched_lock_release();
}

/* Remove task from its ready queue */
static void dequeue_task(struct task* task) {
    if (task->prev) {
        task->prev->next = task->next;
    } else {
        ready_queues[task->priority] = task->next;
    }
    if (task->next) {
        task->next->prev = task->prev;
    }
    task->next = NULL;
    task->prev = NULL;
}

/* Find highest priority ready task */
static struct task* pick_next_task(void) {
    for (int i = 0; i < NUM_PRIORITIES; i++) {
        if (ready_queues[i]) {
            return ready_queues[i];
        }
    }
    return idle_task;
}

void sched_yield(void) {
    /* FIX #12: Check for NULL current_task */
    if (!current_task) return;

    sched_lock_acquire();

    struct task* prev = current_task;

    /* Decrement quantum */
    if (prev->quantum > 0) {
        prev->quantum--;
    }

    /* Check if quantum expired or task blocked */
    if (prev->quantum == 0 || prev->state != TASK_RUNNING) {
        prev->quantum = QUANTUM;

        if (prev->state == TASK_RUNNING) {
            prev->state = TASK_READY;
        }

        /* Pick next task */
        struct task* next = pick_next_task();

        if (next && next != prev) {
            dequeue_task(next);
            next->state = TASK_RUNNING;

            current_task = next;

            /* Context switch */
            __asm__ volatile(
                "pushq %%rbx\n"
                "pushq %%r12\n"
                "pushq %%r13\n"
                "pushq %%r14\n"
                "pushq %%r15\n"
                "movq %%rsp, %0\n"
                "movq %1, %%rsp\n"
                "popq %%r15\n"
                "popq %%r14\n"
                "popq %%r13\n"
                "popq %%r12\n"
                "popq %%rbx\n"
                : "=m"(prev->regs[13])
                : "m"(next->regs[13])
                : "memory"
            );
        }
    }

    sched_lock_release();
}

void scheduler_tick(void) {
    /* Called every timer tick */
    if (current_task) {
        current_task->runtime++;
    }
    sched_yield();
}

uint32_t sched_get_pid(void) {
    return current_task ? current_task->pid : 0;
}

uint32_t sched_get_ppid(void) {
    return (current_task && current_task->parent) ? current_task->parent->pid : 0;
}

uint32_t sched_get_task_count(void) {
    return task_count;
}

void sched_list_tasks(void) {
    pr_info("PID  PPID PRI State       Name\n");
    pr_info("---  ---- --- -----       ----\n");

    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].state != TASK_UNUSED) {
            const char* state_str;
            switch (task_table[i].state) {
                case TASK_READY:    state_str = "READY"; break;
                case TASK_RUNNING:  state_str = "RUNNING"; break;
                case TASK_BLOCKED:  state_str = "BLOCKED"; break;
                case TASK_ZOMBIE:   state_str = "ZOMBIE"; break;
                default:            state_str = "UNKNOWN"; break;
            }
            pr_info("%-4u %-4u %-3d %-7s     %s\n",
                    task_table[i].pid,
                    task_table[i].parent ? task_table[i].parent->pid : 0,
                    task_table[i].priority,
                    state_str,
                    task_table[i].name);
        }
    }
}
