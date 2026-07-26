/*
 * Lestra OS - Service Manager
 * Copyright (c) 2026 lestramk.org
 *
 * Simple service tracking and lifecycle management.
 * Services are kernel subsystems or user processes that run in the
 * background. The service manager tracks them and provides
 * start/stop/status. Called from the shell and from the timer IRQ
 * via service_tick().
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/service.h>
#include <lestra/timer.h>
#include <string.h>

static struct service services[SERVICE_MAX];
static int initialized = 0;

/* Forward declarations for services we can start */
#ifndef SSH_DEFAULT_PORT
#define SSH_DEFAULT_PORT 2222
#endif
extern int  ssh_server_start(uint16_t port);
extern int  ssh_server_stop(void);
extern void ssh_server_init(void);
extern int  ssh_server_is_running(void);
extern void ssh_server_tick(void);

void service_init(void) {
    memset(services, 0, sizeof(services));
    initialized = 1;

    /* Register built-in services */
    service_register("net", "Network stack (E1000 + DHCP)", 1);
    service_register("shell", "Kernel shell (lsh)", 1);
    service_register("ssh", "SSH-like remote shell (port 2222)", 0);
    service_register("sandbox-server", "Sandbox HTTP server", 0);

    /* Mark net and shell as already running (they are kernel threads) */
    for (int i = 0; i < SERVICE_MAX; i++) {
        if (!services[i].in_use) continue;
        if (strcmp(services[i].name, "net") == 0 ||
            strcmp(services[i].name, "shell") == 0) {
            services[i].state = SVC_RUNNING;
            services[i].start_time = timer_get_ms();
        }
    }

    pr_info("service: initialized (%d built-in services)\n", SERVICE_MAX);
}

int service_register(const char* name, const char* desc, int auto_start) {
    if (!initialized || !name) return -1;
    if (strnlen(name, SERVICE_NAME_LEN) >= SERVICE_NAME_LEN) return -1;

    /* Check if already registered */
    for (int i = 0; i < SERVICE_MAX; i++) {
        if (services[i].in_use && strcmp(services[i].name, name) == 0) {
            return 0; /* already registered */
        }
    }

    /* Find free slot */
    for (int i = 0; i < SERVICE_MAX; i++) {
        if (!services[i].in_use) {
            memset(&services[i], 0, sizeof(struct service));
            services[i].in_use = 1;
            strncpy(services[i].name, name, SERVICE_NAME_LEN - 1);
            services[i].name[SERVICE_NAME_LEN - 1] = '\0';
            if (desc) {
                strncpy(services[i].description, desc, SERVICE_DESC_LEN - 1);
                services[i].description[SERVICE_DESC_LEN - 1] = '\0';
            }
            services[i].auto_start = auto_start;
            services[i].state = SVC_STOPPED;
            pr_info("service: registered '%s' (%s)\n", name,
                    desc ? desc : "");
            return 0;
        }
    }
    pr_warn("service: table full, cannot register '%s'\n", name);
    return -1;
}

int service_find(const char* name) {
    for (int i = 0; i < SERVICE_MAX; i++) {
        if (services[i].in_use && strcmp(services[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int start_service_by_index(int idx) {
    struct service* s = &services[idx];

    if (s->state == SVC_RUNNING) {
        printk("service: '%s' is already running\n", s->name);
        return 0;
    }

    s->state = SVC_STARTING;
    s->start_time = timer_get_ms();

    int rc = -1;

    if (strcmp(s->name, "net") == 0) {
        /* Network is already running as a kernel thread */
        rc = 0;
    } else if (strcmp(s->name, "shell") == 0) {
        /* Shell is already running */
        rc = 0;
    } else if (strcmp(s->name, "ssh") == 0) {
        ssh_server_init();
        rc = ssh_server_start(SSH_DEFAULT_PORT);
    } else if (strcmp(s->name, "sandbox-server") == 0) {
        extern void sandbox_server_start(int port);
        sandbox_server_start(8080);
        rc = 0;
    } else {
        pr_warn("service: unknown service '%s'\n", s->name);
        rc = -1;
    }

    if (rc == 0) {
        s->state = SVC_RUNNING;
        s->restart_count = 0;
        pr_info("service: '%s' started\n", s->name);
        printk("Service '%s' started.\n", s->name);
    } else {
        s->state = SVC_FAILED;
        pr_warn("service: '%s' failed to start\n", s->name);
        printk("Service '%s' failed to start.\n", s->name);
    }
    return rc;
}

static int stop_service_by_index(int idx) {
    struct service* s = &services[idx];

    if (s->state == SVC_STOPPED) {
        printk("service: '%s' is not running\n", s->name);
        return 0;
    }

    int rc = 0;

    if (strcmp(s->name, "net") == 0) {
        pr_warn("service: cannot stop kernel network service\n");
        printk("Cannot stop kernel service '%s'.\n", s->name);
        return -1;
    } else if (strcmp(s->name, "shell") == 0) {
        pr_warn("service: cannot stop kernel shell service\n");
        printk("Cannot stop kernel service '%s'.\n", s->name);
        return -1;
    } else if (strcmp(s->name, "ssh") == 0) {
        rc = ssh_server_stop();
    } else if (strcmp(s->name, "sandbox-server") == 0) {
        extern void sandbox_server_stop(void);
        sandbox_server_stop();
        rc = 0;
    }

    if (rc == 0) {
        s->state = SVC_STOPPED;
        s->pid = 0;
        pr_info("service: '%s' stopped\n", s->name);
        printk("Service '%s' stopped.\n", s->name);
    } else {
        pr_warn("service: '%s' failed to stop\n", s->name);
        printk("Service '%s' failed to stop.\n", s->name);
    }
    return rc;
}

int service_start(const char* name) {
    if (!initialized || !name) return -1;
    int idx = service_find(name);
    if (idx < 0) {
        printk("Unknown service: %s\n", name);
        return -1;
    }
    return start_service_by_index(idx);
}

int service_stop(const char* name) {
    if (!initialized || !name) return -1;
    int idx = service_find(name);
    if (idx < 0) {
        printk("Unknown service: %s\n", name);
        return -1;
    }
    return stop_service_by_index(idx);
}

int service_status(const char* name) {
    if (!initialized || !name) return -1;
    int idx = service_find(name);
    if (idx < 0) {
        printk("Unknown service: %s\n", name);
        return -1;
    }

    struct service* s = &services[idx];
    const char* state_str;
    switch (s->state) {
        case SVC_STOPPED:  state_str = "stopped";  break;
        case SVC_STARTING: state_str = "starting"; break;
        case SVC_RUNNING:  state_str = "running";  break;
        case SVC_FAILED:   state_str = "failed";   break;
        default:           state_str = "unknown";   break;
    }

    printk("  %-18s  %-10s  %s\n", s->name, state_str, s->description);
    if (s->state == SVC_RUNNING && s->start_time > 0) {
        uint64_t uptime_ms = timer_get_ms() - s->start_time;
        uint64_t uptime_s = uptime_ms / 1000;
        printk("                     uptime: %lus  restarts: %d\n",
               (unsigned)uptime_s, s->restart_count);
    }
    return s->state;
}

int service_list(struct service_info* list, int max) {
    if (!initialized || !list || max <= 0) return 0;
    int count = 0;
    for (int i = 0; i < SERVICE_MAX && count < max; i++) {
        if (!services[i].in_use) continue;
        strncpy(list[count].name, services[i].name, SERVICE_NAME_LEN - 1);
        list[count].name[SERVICE_NAME_LEN - 1] = '\0';
        strncpy(list[count].description, services[i].description,
                SERVICE_DESC_LEN - 1);
        list[count].description[SERVICE_DESC_LEN - 1] = '\0';
        list[count].state = services[i].state;
        list[count].pid = services[i].pid;
        list[count].auto_start = services[i].auto_start;
        list[count].restart_count = services[i].restart_count;
        count++;
    }
    return count;
}

/* Called periodically from timer IRQ or from shell tick.
 * Restarts failed services that have auto_start set and haven't
 * exceeded the restart limit. */
void service_tick(void) {
    if (!initialized) return;

    for (int i = 0; i < SERVICE_MAX; i++) {
        struct service* s = &services[i];
        if (!s->in_use) continue;
        if (s->state == SVC_FAILED && s->auto_start) {
            if (s->restart_count < SERVICE_MAX_RESTART) {
                s->restart_count++;
                pr_info("service: auto-restarting '%s' (attempt %d)\n",
                        s->name, s->restart_count);
                start_service_by_index(i);
            } else {
                pr_warn("service: '%s' exceeded max restarts, giving up\n",
                        s->name);
                s->state = SVC_STOPPED;
            }
        }
    }

    /* Check SSH idle timeouts */
    if (ssh_server_is_running()) {
        ssh_server_tick();
    }

    /* Poll HTTP management API (active in cloud/VPS mode) */
    extern void http_mgmt_tick(void);
    http_mgmt_tick();
}
