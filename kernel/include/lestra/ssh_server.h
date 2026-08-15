/*
 * Lestra OS - SSH-2.0 Remote Shell Server
 * Copyright (c) 2026 lestramk.org
 *
 * Complete SSH-2.0 protocol server compatible with OpenSSH clients.
 * Supported algorithms:
 *   Key exchange: ecdh-sha2-nistp256
 *   Host key:     ecdsa-sha2-nistp256
 *   Encryption:   aes128-gcm@openssh.com
 *   MAC:          implicit (GCM auth tag)
 *   Compression:  none
 *   Authentication: password (ssh-userauth)
 *   Channels:     session (interactive shell + pty-req)
 */

#ifndef LESTRA_SSH_SERVER_H
#define LESTRA_SSH_SERVER_H

#include <lestra/types.h>
#include <lestra/net.h>

#define SSH_DEFAULT_PORT    2222
#define SSH_MAX_SESSIONS    4
#define SSH_BUF_SIZE        8192
#define SSH_TIMEOUT_MS      300000   /* 5 minute idle timeout */

/* SSH-2.0 message types */
#define SSH_MSG_DISCONNECT       1
#define SSH_MSG_IGNORE           2
#define SSH_MSG_UNIMPLEMENTED    3
#define SSH_MSG_DEBUG            4
#define SSH_MSG_SERVICE_REQUEST  5
#define SSH_MSG_SERVICE_ACCEPT   6
#define SSH_MSG_KEX_INIT         20
#define SSH_MSG_NEWKEYS          21
#define SSH_MSG_KEX_ECDH_INIT    30
#define SSH_MSG_KEX_ECDH_REPLY   31
#define SSH_MSG_USERAUTH_REQUEST 50
#define SSH_MSG_USERAUTH_FAILURE 51
#define SSH_MSG_USERAUTH_SUCCESS 52
#define SSH_MSG_USERAUTH_BANNER  53
#define SSH_MSG_GLOBAL_REQUEST   80
#define SSH_MSG_REQUEST_SUCCESS  81
#define SSH_MSG_REQUEST_FAILURE  82
#define SSH_MSG_CHANNEL_OPEN     90
#define SSH_MSG_CHANNEL_OPEN_CONFIRM 91
#define SSH_MSG_CHANNEL_OPEN_FAILURE  92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST 93
#define SSH_MSG_CHANNEL_DATA     94
#define SSH_MSG_CHANNEL_EXTENDED_DATA 95
#define SSH_MSG_CHANNEL_EOF      96
#define SSH_MSG_CHANNEL_CLOSE    97
#define SSH_MSG_CHANNEL_REQUEST  98
#define SSH_MSG_CHANNEL_SUCCESS  99
#define SSH_MSG_CHANNEL_FAILURE  100

/* Alternate names used in ssh_server.c implementation */
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION SSH_MSG_CHANNEL_OPEN_CONFIRM

/* SSH disconnect reason codes */
#define SSH_DISCONNECT_HOST_NOT_ALLOWED           1
#define SSH_DISCONNECT_PROTOCOL_ERROR             2
#define SSH_DISCONNECT_KEY_EXCHANGE_FAILED        3
#define SSH_DISCONNECT_RESERVED                  4
#define SSH_DISCONNECT_MAC_ERROR                 5
#define SSH_DISCONNECT_COMPRESSION_ERROR         6
#define SSH_DISCONNECT_SERVICE_NOT_AVAILABLE     7
#define SSH_DISCONNECT_PROTOCOL_VERSION_NOT_SUPPORTED 8
#define SSH_DISCONNECT_HOST_KEY_NOT_VERIFIABLE   9
#define SSH_DISCONNECT_CONNECTION_LOST           10
#define SSH_DISCONNECT_BY_APPLICATION            11
#define SSH_DISCONNECT_TOO_MANY_CONNECTIONS      12
#define SSH_DISCONNECT_AUTH_CANCELLED_BY_USER    13
#define SSH_DISCONNECT_NO_MORE_AUTH_METHODS_AVAILABLE 14
#define SSH_DISCONNECT_ILLEGAL_USER_NAME         15

/* Session state aliases used in ssh_server.c */
#define SSH_STATE_INIT           0
#define SSH_STATE_VERSION_SENT   1
#define SSH_STATE_KEX_INIT_SENT  2
#define SSH_STATE_NEWKEYS_PENDING 3
#define SSH_STATE_ENCRYPTED      4
#define SSH_STATE_AUTH_PENDING   5
#define SSH_STATE_CHANNEL_OPEN   6
#define SSH_STATE_SHELL_RUNNING  7

/* Encryption state for AES-128-GCM */
struct ssh_enc_state {
    uint8_t key[16];     /* AES-128 key */
    uint32_t seq;        /* packet sequence number (used as IV counter) */
};

/* SSH session state machine */
enum ssh_session_state {
    SSH_ST_VERSION = 0,
    SSH_ST_KEX_INIT,
    SSH_ST_KEX_ECDH,
    SSH_ST_NEWKEYS_SENT,
    SSH_ST_SERVICE,
    SSH_ST_USERAUTH,
    SSH_ST_CHANNEL,
    SSH_ST_RUNNING,
    SSH_ST_CLOSING,
};

/* SSH session */
struct ssh_session {
    int in_use;
    int conn_idx;           /* TCP connection slot index */
    enum ssh_session_state state;
    int encryption_active;
    char username[32];
    uint64_t last_activity;

    /* Raw TCP receive buffer */
    uint8_t rx_buf[SSH_BUF_SIZE];
    int rx_len;

    /* Line buffer for shell command processing */
    char line_buf[SSH_BUF_SIZE];
    int line_len;

    /* Version exchange */
    char client_version[256];

    /* Key exchange */
    uint8_t server_kex_cookie[16];
    uint8_t client_kex_cookie[16];
    uint8_t server_kexinit[SSH_BUF_SIZE];
    int server_kexinit_len;
    uint8_t client_kexinit[SSH_BUF_SIZE];
    int client_kexinit_len;
    uint8_t ecdh_priv[32];      /* server ECDH private key */
    uint8_t ecdh_pub[64];       /* server ECDH public key (x||y, 64 bytes) */
    uint8_t client_ecdh_pub[64]; /* client ECDH public key (x||y, 64 bytes) */
    uint8_t shared_secret[32];   /* ECDH shared secret K */
    uint8_t session_id[32];      /* exchange hash H */
    int session_id_set;

    /* Encryption */
    struct ssh_enc_state enc_send;  /* server->client encryption */
    struct ssh_enc_state enc_recv;  /* client->server encryption */

    /* Channel */
    uint32_t our_channel;
    uint32_t peer_channel;
    uint32_t window_local;
    uint32_t window_remote;
    uint32_t max_pkt_local;
    uint32_t max_pkt_remote;

    /* PTY */
    int pty_fd;          /* PTY master fd */
    char pty_term[32];   /* terminal type (e.g. "xterm-256color") */
    uint16_t pty_rows;
    uint16_t pty_cols;
    uint16_t pty_width_px;
    uint16_t pty_height_px;
};

/* Public API (same interface as before, compatible with service.c) */
void ssh_server_init(void);
int  ssh_server_start(uint16_t port);
int  ssh_server_stop(void);
void ssh_server_tick(void);
int  ssh_server_is_running(void);

#endif /* LESTRA_SSH_SERVER_H */
