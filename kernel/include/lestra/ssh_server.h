/*
 * Lestra OS - SSH-like Remote Shell Server
 * Copyright (c) 2026 lestramk.org
 */

#ifndef LESTRA_SSH_SERVER_H
#define LESTRA_SSH_SERVER_H

#include <lestra/types.h>

#define SSH_DEFAULT_PORT 2222
#define SSH_MAX_SESSIONS 4
#define SSH_MAX_USERS    4
#define SSH_BUF_SIZE     1024
#define SSH_TIMEOUT_MS   300000  /* 5 minute idle timeout */

struct ssh_session {
    int in_use;
    int conn_idx;       /* index into tcp_get_conn() array */
    int authenticated;
    int pid;            /* shell process PID */
    char username[32];
    uint64_t last_activity;
    /* Challenge-response fields */
    uint8_t challenge[32];  /* raw challenge bytes */
    char challenge_hex[65]; /* hex-encoded challenge (64 hex chars + NUL) */
    int challenge_sent;     /* 1 if CHALLENGE has been sent to client */
};

void ssh_server_init(void);
int  ssh_server_start(uint16_t port);
int  ssh_server_stop(void);
void ssh_server_tick(void);
int  ssh_server_is_running(void);

#endif /* LESTRA_SSH_SERVER_H */
