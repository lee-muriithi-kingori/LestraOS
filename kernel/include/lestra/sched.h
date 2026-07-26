#ifndef LESTRA_SCHED_H
#define LESTRA_SCHED_H

#include <lestra/types.h>

#define MAX_PROCS       32
#define MAX_SIGNALS     32
#define MAX_FD_PER_PROC 128

/* Per-process file descriptor entry.
 * Maps a process-local fd number to an underlying resource.
 * FD types: FD_UNUSED (free slot), FD_SPECIAL (stdin/stdout/stderr),
 * FD_VFS (VFS file — memfs or ext2), FD_PIPE (pipe endpoint). */
#define FD_UNUSED   0
#define FD_SPECIAL  1
#define FD_VFS      2
#define FD_PIPE     3

struct fd_entry {
    int type;               /* FD_UNUSED / FD_SPECIAL / FD_VFS / FD_PIPE */
    int resource;           /* global VFS fd or pipe fd (unused for FD_SPECIAL) */
    int64_t offset;         /* per-process read/write offset */
    int flags;              /* open flags (O_RDONLY, O_WRONLY, O_RDWR, …) */
};

struct cpu_state {
    uint64_t r15, r14, r13, r12;
    uint64_t rbp, rbx;
    uint64_t rax;
    uint64_t rip, cs, rflags, rsp, ss;
    uint64_t fs_base;
} __packed;

struct sigaction_entry {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_mask;
};

struct process {
    int pid;
    int state;
    int parent_pid;
    uint64_t* pml4;
    int pml4_refcount;
    struct cpu_state* saved_state;
    void* kernel_stack;
    uint64_t kernel_stack_top;
    uint64_t entry_point;
    uint64_t user_stack_ptr;
    int exit_status;
    char name[32];
    void* clear_child_tid;
    struct sigaction_entry sigactions[MAX_SIGNALS];
    uint64_t signal_pending;
    uint64_t signal_blocked;
    struct cpu_state* pre_signal_state;
    int in_signal_handler;
    uint64_t fs_base;
    int is_linux_process;
    char exe_path[256];
    uint64_t brk;
    uint64_t stack_bottom;               /* bottom of mapped stack region (grows downward) */
    struct fd_entry fds[MAX_FD_PER_PROC]; /* per-process file descriptor table */
};

#define PROC_FREE      0
#define PROC_RUNNABLE  1
#define PROC_RUNNING   2
#define PROC_BLOCKED   3
#define PROC_ZOMBIE    4

void sched_init(void);
void sched_enable(void);
void sched_yield(void);
void sched_tick(void);
void sched_check_wakeups(void);
void schedule(void);

int proc_getpid(void);
void proc_exit(int status);
struct process* task_current(void);
struct process* sched_alloc_proc(void);
struct process* sched_find_by_pid_impl(int pid);
int sched_alloc_pid(void);
int sched_clone_thread(uint64_t child_stack, uint64_t ptid, uint64_t ctid, uint64_t newtls);
void sched_kill_and_unload(int pid, int exit_status);
void sched_set_clear_child_tid(int pid, void* addr);
void proc_set_linux_process(int is_linux);
int proc_is_linux_process(void);
void fd_table_init(struct process* p);
void fd_table_copy(struct process* parent, struct process* child);
void fd_table_close_all(struct process* p);
void proc_reap(struct process* p);

void task_block(void);
void task_unblock(void* task);
void task_unblock_pid(int pid);
void task_sleep(uint64_t ms);
void task_set_priority(int p);

void signal_check_and_deliver(void);

extern struct process procs[MAX_PROCS];

#endif
