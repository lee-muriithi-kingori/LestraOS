/*
 * Lestra OS - Signal delivery (rt_sigaction, kill, rt_sigprocmask, rt_sigreturn)
 *
 * SMAP-hardened: all user-pointer access goes through uaccess.h helpers.
 */
#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/sched.h>
#include <lestra/mm.h>
#include <lestra/uaccess.h>
#include <string.h>

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGURG   23

extern void proc_exit(int);

int64_t signal_sigaction(int signum, uint64_t act, uint64_t oldact, uint64_t sigsetsize) {
    (void)sigsetsize;
    if (signum < 1 || signum >= MAX_SIGNALS) return -22;
    if (signum == SIGKILL || signum == SIGSTOP) return -22;
    struct process* p = task_current();
    if (!p) return -1;
    /* struct sigaction is 4 x uint64_t: handler, flags, restorer, mask */
    if (oldact) {
        if (!access_ok((void*)oldact, 32)) return -14; /* EFAULT */
        uint64_t kern_old[4] = {
            p->sigactions[signum].sa_handler,
            p->sigactions[signum].sa_flags,
            0, /* restorer */
            p->sigactions[signum].sa_mask
        };
        if (copy_to_user((void*)oldact, kern_old, 32) < 0) return -14;
    }
    if (act) {
        if (!access_ok((void*)act, 32)) return -14; /* EFAULT */
        uint64_t kern_act[4] = {0};
        if (copy_from_user(kern_act, (void*)act, 32) < 0) return -14;
        p->sigactions[signum].sa_handler = kern_act[0];
        p->sigactions[signum].sa_flags    = kern_act[1];
        p->sigactions[signum].sa_mask    = kern_act[3];
    }
    return 0;
}

int64_t signal_kill(int pid, int sig) {
    if (sig < 0 || sig >= MAX_SIGNALS) return -22;
    if (pid > 0) {
        struct process* p = sched_find_by_pid_impl(pid);
        if (!p) return -3;
        if (sig == 0) return 0;
        p->signal_pending |= (1ULL << sig);
        if (p->state == PROC_BLOCKED) {
            p->state = PROC_RUNNABLE;
        }
        return 0;
    }
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != PROC_FREE && procs[i].state != PROC_ZOMBIE) {
            if (sig != 0) {
                procs[i].signal_pending |= (1ULL << sig);
                if (procs[i].state == PROC_BLOCKED) {
                    procs[i].state = PROC_RUNNABLE;
                }
            }
        }
    }
    return 0;
}

int64_t signal_sigprocmask(int how, uint64_t set, uint64_t oldset, uint64_t sigsetsize) {
    (void)sigsetsize;
    struct process* p = task_current();
    if (!p) return -1;
    if (oldset) {
        if (!access_ok((void*)oldset, sizeof(uint64_t))) return -14; /* EFAULT */
        uint64_t blocked = p->signal_blocked;
        if (copy_to_user((void*)oldset, &blocked, sizeof(uint64_t)) < 0) return -14;
    }
    if (set) {
        if (!access_ok((void*)set, sizeof(uint64_t))) return -14; /* EFAULT */
        uint64_t m = 0;
        if (copy_from_user(&m, (void*)set, sizeof(uint64_t)) < 0) return -14;
        switch (how) {
            case 0: p->signal_blocked |= m; break;   /* SIG_BLOCK */
            case 1: p->signal_blocked &= ~m; break;  /* SIG_UNBLOCK */
            case 2: p->signal_blocked = m; break;     /* SIG_SETMASK */
            default: return -22;
        }
        p->signal_blocked &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));
    }
    return 0;
}

int64_t signal_sigreturn(void) {
    struct process* p = task_current();
    if (!p || !p->in_signal_handler || !p->pre_signal_state) return -22;
    memcpy(p->saved_state, p->pre_signal_state, sizeof(struct cpu_state));
    p->in_signal_handler = 0;
    extern void kfree(void*);
    kfree(p->pre_signal_state);
    p->pre_signal_state = NULL;
    return 0;
}

void signal_check_and_deliver(void) {
    struct process* p = task_current();
    if (!p || !p->saved_state) return;
    if (p->in_signal_handler) return;
    if (p->signal_pending == 0) return;
    uint64_t deliverable = p->signal_pending & ~p->signal_blocked;
    if (deliverable == 0) return;
    int signum = 1;
    for (int i = 1; i < MAX_SIGNALS; i++) {
        if (deliverable & (1ULL << i)) { signum = i; break; }
    }
    p->signal_pending &= ~(1ULL << signum);
    uint64_t handler = p->sigactions[signum].sa_handler;
    if (handler == 0) {
        if (signum == SIGCHLD || signum == SIGCONT || signum == SIGURG) return;
        proc_exit(128 + signum);
        return;
    }
    if (handler == 1) return;
    extern void* kmalloc(size_t);
    p->pre_signal_state = (struct cpu_state*)kmalloc(sizeof(struct cpu_state));
    if (!p->pre_signal_state) return;
    memcpy(p->pre_signal_state, p->saved_state, sizeof(struct cpu_state));
    p->saved_state->rip = handler;
    p->saved_state->rsp -= 128;
    p->saved_state->rflags &= ~(1ULL << 8);
    p->in_signal_handler = 1;
}

void signal_send_to_process(struct process* p, int signum) {
    if (!p || signum < 1 || signum >= MAX_SIGNALS) return;
    p->signal_pending |= (1ULL << signum);
    if (p->state == PROC_BLOCKED) {
        p->state = PROC_RUNNABLE;
    }
}
