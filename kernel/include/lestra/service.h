/*
 * Lestra OS - Service Manager
 * Copyright (c) 2026 lestramk.org
 */

#ifndef LESTRA_SERVICE_H
#define LESTRA_SERVICE_H

#include <lestra/types.h>

#define SERVICE_MAX        16
#define SERVICE_NAME_LEN   32
#define SERVICE_DESC_LEN   64
#define SERVICE_MAX_RESTART 5

enum service_state {
    SVC_STOPPED = 0,
    SVC_STARTING,
    SVC_RUNNING,
    SVC_FAILED,
};

struct service {
    int in_use;
    char name[SERVICE_NAME_LEN];
    enum service_state state;
    int pid;                    /* process PID (0 if kernel thread) */
    int auto_start;             /* start on boot */
    int restart_count;          /* number of restarts */
    uint64_t start_time;        /* when started (ms) */
    char description[SERVICE_DESC_LEN];
};

struct service_info {
    char name[SERVICE_NAME_LEN];
    char description[SERVICE_DESC_LEN];
    enum service_state state;
    int pid;
    int auto_start;
    int restart_count;
};

void service_init(void);
void service_tick(void);
int  service_register(const char* name, const char* desc, int auto_start);
int  service_start(const char* name);
int  service_stop(const char* name);
int  service_status(const char* name);
int  service_list(struct service_info* list, int max);
int  service_find(const char* name);

#endif /* LESTRA_SERVICE_H */
