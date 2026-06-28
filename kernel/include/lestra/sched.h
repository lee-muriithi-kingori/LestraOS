/*
 * Lestra OS - Process Scheduler
 * Copyright (c) 2026 lestramk.org
 */

#ifndef LESTRA_SCHED_H
#define LESTRA_SCHED_H

#include <lestra/types.h>

#define MAX_TASKS       256
#define TASK_NAME_LEN   32
#define DEFAULT_STACK   (16 * KiB)

/* Task states */
enum task_state {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_ZOMBIE,
    TASK_SLEEPING
};

/* Task structure */
struct task {
    pid_t pid;
    pid_t ppid;
    char name[TASK_NAME_LEN];
    enum task_state state;
    
    /* CPU context */
    uint64_t rsp;
    uint64_t rip;
    uint64_t rbp;
    uint64_t cr3;
    
    /* Memory */
    uintptr_t* page_table;
    uintptr_t stack_base;
    uintptr_t stack_size;
    
    /* Timing */
    uint64_t ticks;
    uint64_t sleep_until;
    uint64_t cpu_time;
    
    /* Priority (0-31, lower = higher priority) */
    int priority;
    
    /* Links */
    struct task* next;
    struct task* prev;
};

/* Scheduler functions */
void sched_init(void);
void sched_yield(void);
void sched_tick(void);
struct task* task_create(const char* name, void (*entry)(void));
void task_exit(int code);
void task_block(enum task_state state);
void task_unblock(struct task* task);
void task_sleep(uint64_t ms);
struct task* task_current(void);
void task_set_priority(struct task* task, int priority);

/* Context switch */
void context_switch(struct task* prev, struct task* next);

#endif /* LESTRA_SCHED_H */
