/*
 * Lestra OS - Sandbox Implementation
 * Copyright (c) 2026 lestramk.org
 *
 * Isolated execution environments for running untrusted code.
 * Each sandbox gets its own PML4, resource limits, and filesystem root.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <lestra/sched.h>
#include <lestra/sandbox.h>
#include <string.h>

static struct sandbox sandboxes[SANDBOX_MAX];
static int initialized = 0;

void sandbox_init(void) {
    pr_info("sandbox: initialising (max %d sandboxes)\n", SANDBOX_MAX);
    memset(sandboxes, 0, sizeof(sandboxes));
    for (int i = 0; i < SANDBOX_MAX; i++) {
        sandboxes[i].id = i + 1;
    }
    initialized = 1;
    pr_info("sandbox: ready\n");
}

static struct sandbox* find_free_slot(void) {
    for (int i = 0; i < SANDBOX_MAX; i++) {
        if (!sandboxes[i].in_use) return &sandboxes[i];
    }
    return NULL;
}

static struct sandbox* find_by_id(int id) {
    if (id < 1 || id > SANDBOX_MAX) return NULL;
    struct sandbox* s = &sandboxes[id - 1];
    if (!s->in_use) return NULL;
    return s;
}

static void setup_rootfs(struct sandbox* s) {
    /* Default rootfs: /sandbox/<id>/ */
    ksnprintf(s->rootfs, SANDBOX_ROOTFS_LEN, "/sandbox/%d/", s->id);
}

int sandbox_create(const char* name, int port) {
    if (!initialized) return -1;

    struct sandbox* s = find_free_slot();
    if (!s) {
        pr_warn("sandbox: max sandboxes (%d) reached\n", SANDBOX_MAX);
        return -1;
    }

    memset(s->name, 0, SANDBOX_NAME_LEN);
    if (name && name[0]) {
        strncpy(s->name, name, SANDBOX_NAME_LEN - 1);
    } else {
        ksnprintf(s->name, SANDBOX_NAME_LEN, "sb%d", s->id);
    }

    s->in_use         = 1;
    s->pid            = -1;
    s->active         = 0;
    s->memory_limit   = SANDBOX_DEFAULT_MEM_LIMIT;
    s->memory_used    = 0;
    s->network_disabled = 1;
    s->max_open_fds   = SANDBOX_DEFAULT_MAX_FDS;
    s->port           = port > 0 ? port : 0;

    setup_rootfs(s);

    /* Create isolated address space */
    s->pml4 = (uint64_t*)vmm_create_address_space();
    if (!s->pml4) {
        pr_err("sandbox: failed to create address space for sandbox %d\n", s->id);
        s->in_use = 0;
        return -1;
    }

    pr_info("sandbox: created sandbox %d \"%s\" (mem=%luMB, fds=%d, net=%s)\n",
            s->id, s->name,
            (unsigned long)(s->memory_limit / (1024 * 1024)),
            s->max_open_fds,
            s->network_disabled ? "off" : "on");
    return s->id;
}

int sandbox_destroy(int id) {
    if (!initialized) return -1;
    struct sandbox* s = find_by_id(id);
    if (!s) {
        pr_warn("sandbox: sandbox %d not found\n", id);
        return -1;
    }

    /* Stop if still running */
    if (s->active && s->pid > 0) {
        sandbox_stop(id);
    }

    /* Tear down address space */
    if (s->pml4) {
        vmm_destroy_address_space(s->pml4);
        s->pml4 = NULL;
    }

    pr_info("sandbox: destroyed sandbox %d \"%s\"\n", s->id, s->name);
    memset(s, 0, sizeof(*s));
    s->id = id;  /* preserve id slot marker */
    return 0;
}

int sandbox_start(int id, const char* cmd) {
    if (!initialized) return -1;
    struct sandbox* s = find_by_id(id);
    if (!s) {
        pr_warn("sandbox: sandbox %d not found\n", id);
        return -1;
    }

    if (s->active) {
        pr_warn("sandbox: sandbox %d already running (pid %d)\n", id, s->pid);
        return -1;
    }

    if (!cmd || !cmd[0]) {
        pr_warn("sandbox: no command specified\n");
        return -1;
    }

    /* Find a free process slot */
    struct process* proc = sched_alloc_proc();
    if (!proc) {
        pr_err("sandbox: no free process slots\n");
        return -1;
    }

    /* Set up the process in the sandbox's isolated address space */
    memset(proc->name, 0, sizeof(proc->name));
    strncpy(proc->name, cmd, sizeof(proc->name) - 1);
    if (proc->pml4) {
        vmm_destroy_address_space(proc->pml4);
    }
    proc->pml4 = s->pml4;

    s->pid = proc->pid;
    s->active = 1;

    /* Mark process as runnable — it will execute when scheduled */
    proc->state = PROC_RUNNABLE;

    pr_info("sandbox: started sandbox %d, pid %d, cmd \"%s\"\n",
            s->id, s->pid, cmd);
    return 0;
}

int sandbox_stop(int id) {
    if (!initialized) return -1;
    struct sandbox* s = find_by_id(id);
    if (!s) {
        pr_warn("sandbox: sandbox %d not found\n", id);
        return -1;
    }

    if (!s->active) {
        pr_info("sandbox: sandbox %d not running\n", id);
        return 0;
    }

    /* Kill the sandbox process */
    if (s->pid > 0) {
        sched_kill_and_unload(s->pid, 0);
        pr_info("sandbox: killed process %d in sandbox %d\n", s->pid, s->id);
    }

    s->pid    = -1;
    s->active = 0;

    return 0;
}

int sandbox_status(int id, struct sandbox_info* info) {
    if (!initialized) return -1;
    struct sandbox* s = find_by_id(id);
    if (!s || !info) return -1;

    info->id              = s->id;
    info->pid             = s->pid;
    info->active          = s->active;
    info->network_disabled = s->network_disabled;
    info->memory_limit    = s->memory_limit;
    info->memory_used     = s->memory_used;
    info->max_open_fds    = s->max_open_fds;
    info->port            = s->port;
    strncpy(info->name, s->name, SANDBOX_NAME_LEN);
    strncpy(info->rootfs, s->rootfs, SANDBOX_ROOTFS_LEN);
    return 0;
}

int sandbox_is_sandboxed(int pid) {
    if (!initialized) return 0;
    for (int i = 0; i < SANDBOX_MAX; i++) {
        if (sandboxes[i].in_use && sandboxes[i].active &&
            sandboxes[i].pid == pid) {
            return 1;
        }
    }
    return 0;
}

int sandbox_count(void) {
    if (!initialized) return 0;
    int n = 0;
    for (int i = 0; i < SANDBOX_MAX; i++) {
        if (sandboxes[i].in_use) n++;
    }
    return n;
}

void sandbox_list(void) {
    if (!initialized) {
        printk("sandbox: not initialised\n");
        return;
    }
    printk("Sandboxes (%d/%d used):\n", sandbox_count(), SANDBOX_MAX);
    int any = 0;
    for (int i = 0; i < SANDBOX_MAX; i++) {
        struct sandbox* s = &sandboxes[i];
        if (!s->in_use) continue;
        printk("  [%d] %-16s  %s  pid=%-4d  net=%-3s  mem=%luMB  port=%d\n",
               s->id, s->name,
               s->active ? "RUNNING" : "STOPPED",
               s->pid,
               s->network_disabled ? "off" : "on",
               (unsigned long)(s->memory_limit / (1024 * 1024)),
               s->port);
        any = 1;
    }
    if (!any) {
        printk("  (none)\n");
    }
}
