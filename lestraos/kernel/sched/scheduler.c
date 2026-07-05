/*
 * Lestra OS - Scheduler (stub)
 *
 * Real round-robin scheduler + context switch lives on the roadmap.
 * For now we stub every API to satisfy the linker; the kernel reaches
 * kernel_main() and runs shell_run() in a single-task configuration.
 */
#include <lestra/types.h>
#include <lestra/sched.h>
#include <lestra/printk.h>

void sched_init(void) {
    pr_info("Scheduler initialized (single-task mode stub)\n");
}

void sched_yield(void) {
    /* single task: nothing to schedule */
}

void sched_tick(void) {
    /* no preemption in single-task mode */
}

struct task* task_create(const char* name, void (*entry)(void)) {
    (void)name; (void)entry;
    return NULL; /* not supported in stub */
}

void task_exit(int code) {
    (void)code;
    pr_info("task_exit: stub\n");
}

void task_block(enum task_state state) {
    (void)state;
}

void task_unblock(struct task* task) {
    (void)task;
}

void task_sleep(uint64_t ms) {
    /* busy-wait placeholder; real impl uses timer queue */
    for (volatile uint64_t i = 0; i < (ms * 100000ULL); i++) { (void)i; }
}

struct task* task_current(void) {
    return NULL;
}

void task_set_priority(struct task* task, int priority) {
    (void)task; (void)priority;
}

void context_switch(struct task* prev, struct task* next) {
    (void)prev; (void)next;
}

/* schedule(frame) is called by the IRQ0 timer handler with the saved
 * interrupt context. The real implementation performs a full register
 * save/restore and task switch; in single-task mode we just drop it. */
void schedule(void* frame) {
    (void)frame;
}