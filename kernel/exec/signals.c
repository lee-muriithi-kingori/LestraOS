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
    /* W3-B / W1-C #4: after restoring, re-check for additional pending
     * signals so a multi-signal burst doesn't lose any. Without this,
     * sigreturn would return to user code and the next signal would
     * only be delivered on the next syscall. */
    extern void signal_check_and_deliver(void);
    signal_check_and_deliver();
    return 0;
}

/* W3-B: tiny in-user-space sigreturn trampoline.
 *
 * We can't point the handler's return address at the kernel's
 * signal_sigreturn (SMEP would #PF). Instead we write a 16-byte
 * machine-code stub onto the user stack that does:
 *     mov rax, 27   ; LESTRA_SYS_RT_SIGRETURN
 *     syscall       ; re-enter the kernel, never returns
 *     int3          ; pad / trap if fallthrough
 *
 * The handler's return address is set to point at this stub, so
 * `ret` from the handler lands here and re-enters the kernel via
 * the syscall ABI. signal_sigreturn then restores the pre-signal
 * saved state from pre_signal_state.
 *
 * Machine code (verified against the AMD64 ISA manual):
 *   48 c7 c0 1b 00 00 00   mov rax, 27
 *   0f 05                  syscall
 *   cc cc cc cc cc cc cc   int3 pad (7 bytes) -> total 16
 */
static const uint8_t sig_trampoline_code[16] = {
    0x48, 0xc7, 0xc0, 0x1b, 0x00, 0x00, 0x00,  /* mov rax, 27 */
    0x0f, 0x05,                                  /* syscall */
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,         /* int3 pad */
};

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

    /* W3-B / W1-C #4: set up the user stack so the handler sees:
     *   - rdi = signum (System V AMD64 first arg)
     *   - rip = handler
     *   - the address just below the handler's RSP points at a
     *     tiny `mov rax, 27; syscall` trampoline we copy onto the
     *     user stack, so `ret` from the handler invokes sigreturn.
     *
     * Layout (low to high):
     *   tramp_addr       : [16 bytes of sig_trampoline_code]
     *   ret_addr_slot    : [8 bytes = tramp_addr]    <- new RSP
     *
     * We also leave 128 bytes of redzone below the trampoline for
     * leaf-function scratch. SysV AMD64 requires RSP%16 == 8 at
     * function entry (the `call` pushed the 8-byte return address).
     */
    uint64_t old_rsp = p->saved_state->rsp;
    /* Leave 128 bytes of redzone, then 16 bytes for the trampoline,
     * then 8 bytes for the return address. */
    uint64_t tramp_addr = (old_rsp - 128 - 16) & ~0xFULL;   /* 16-byte aligned */
    uint64_t ret_addr_slot = tramp_addr - 8;                  /* RSP after `ret` push */

    /* Copy the trampoline code into user space (SMAP-safe). */
    if (copy_to_user((void*)tramp_addr, sig_trampoline_code,
                     sizeof(sig_trampoline_code)) < 0) {
        /* Stack pointer is broken — can't deliver the signal safely.
         * Free the pre_signal_state we just allocated and bail. */
        extern void kfree(void*);
        kfree(p->pre_signal_state);
        p->pre_signal_state = NULL;
        return;
    }
    /* Write the return address (points at the trampoline). */
    if (put_user((uint64_t)tramp_addr, (uint64_t*)ret_addr_slot) < 0) {
        extern void kfree(void*);
        kfree(p->pre_signal_state);
        p->pre_signal_state = NULL;
        return;
    }

    /* Set up the user-return state: handler entry point, signum in
     * RDI, new RSP pointing at the return-address slot. */
    p->saved_state->rip = handler;
    p->saved_state->rdi = (uint64_t)signum;
    p->saved_state->rsp = ret_addr_slot;
    p->saved_state->rflags &= ~(1ULL << 8);  /* clear TF (single-step) */
    p->in_signal_handler = 1;
}

void signal_send_to_process(struct process* p, int signum) {
    if (!p || signum < 1 || signum >= MAX_SIGNALS) return;
    p->signal_pending |= (1ULL << signum);
    if (p->state == PROC_BLOCKED) {
        p->state = PROC_RUNNABLE;
    }
}
