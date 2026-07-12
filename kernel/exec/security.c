/*
 * Lestra OS - Security module (malicious program detection + kill/unload)
 */
#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <string.h>

#define MAX_MONITORED_PIDS 64
#define SYSCALL_RATE_LIMIT 10000
#define FORK_RATE_LIMIT 32

struct proc_monitor {
    int pid;
    uint64_t syscall_count;
    uint64_t fork_count;
    uint64_t last_reset_ms;
    int killed;
    char kill_reason[128];
};

static struct proc_monitor monitors[MAX_MONITORED_PIDS];

static struct proc_monitor* get_monitor(int pid) {
    for (int i = 0; i < MAX_MONITORED_PIDS; i++)
        if (monitors[i].pid == pid) return &monitors[i];
    for (int i = 0; i < MAX_MONITORED_PIDS; i++)
        if (monitors[i].pid == 0) {
            monitors[i].pid = pid;
            return &monitors[i];
        }
    return NULL;
}

int security_check_elf(const void* file_data, size_t file_size) {
    if (!file_data || file_size < 64) return -1;
    const uint8_t* d = (const uint8_t*)file_data;
    if (d[0]!=0x7F||d[1]!='E'||d[2]!='L'||d[3]!='F') return -1;
    if (d[4]!=2) return -1;
    if (d[5]!=1) return -1;
    const uint16_t* machine = (const uint16_t*)(d+18);
    if (*machine != 0x3E) return -1;
    const uint16_t* type = (const uint16_t*)(d+16);
    if (*type != 2 && *type != 3) return -1;
    const uint16_t* phnum = (const uint16_t*)(d+56);
    if (*phnum > 64) return -1;
    if (file_size > 256*1024*1024) return -1;
    return 0;
}

int security_check_syscall(int pid, uint64_t syscall_num, uint64_t a1) {
    extern uint64_t timer_get_ms(void);
    uint64_t now = timer_get_ms();
    struct proc_monitor* m = get_monitor(pid);
    if (!m) return 0;
    if (now - m->last_reset_ms > 1000) {
        m->syscall_count = 0; m->fork_count = 0; m->last_reset_ms = now;
    }
    m->syscall_count++;
    if (m->syscall_count > SYSCALL_RATE_LIMIT) {
        m->killed = 1;
        return -1;
    }
    if (syscall_num == 56 || syscall_num == 57 || syscall_num == 58) {
        m->fork_count++;
        if (m->fork_count > FORK_RATE_LIMIT) { m->killed = 1; return -1; }
    }
    if (syscall_num == 9 && a1 != 0 && a1 < 0x10000) { m->killed = 1; return -1; }
    return 0;
}

void security_kill_process(int pid, const char* reason) {
    pr_warn("security: KILLING PID %d — %s\n", pid, reason ? reason : "violation");
    extern void sched_kill_and_unload(int pid, int exit_status);
    sched_kill_and_unload(pid, 137);
}

const char* security_get_kill_reason(int pid) {
    for (int i = 0; i < MAX_MONITORED_PIDS; i++)
        if (monitors[i].pid == pid && monitors[i].killed)
            return monitors[i].kill_reason;
    return NULL;
}
