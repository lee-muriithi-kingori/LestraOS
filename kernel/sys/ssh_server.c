/*
 * Lestra OS - SSH-like Remote Shell Server
 * Copyright (c) 2026 lestramk.org
 *
 * Simple TCP-based remote shell. Not real SSH — uses a challenge-response
 * auth protocol and executes commands via the kernel shell. Good enough
 * for VPS/cloud remote management.
 *
 * Protocol (LESTRA_SSH/1.0):
 *   S->C: LESTRA_SSH/1.0\n
 *   S->C: Welcome to LestraOS. Please authenticate.\n
 *   S->C: login:\n
 *   C->S: <username>\n
 *   S->C: CHALLENGE <32-byte-hex-random>\n
 *   S->C: password:\n
 *   C->S: AUTH <username> <response-hex>\n
 *   S->C: OK\n  or  DENIED\n
 *   S->C: LestraOS Shell (lsh) - type 'help' for commands\n> 
 *   C->S: <shell command>\n
 *   S->C: <command output>\n> 
 *   ... repeat command/output ...
 *   C->S: exit\n  (or client disconnects)
 *
 * Authentication uses a challenge-response protocol. The server sends
 * a random 32-byte challenge; the client responds with
 * djb2(challenge_hex + ":" + password). Backward-compatible with
 * old clients that send AUTH without a prior CHALLENGE.
 * Default credentials: root / lestra
 *
 * Cloud/VPS mode: when the kernel boots with "cloud" on the cmdline,
 * the SSH server auto-starts. It provides remote shell access over TCP
 * which is essential for VPS/cloud operation where there is no VGA
 * display or keyboard.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/ssh_server.h>
#include <lestra/service.h>
#include <lestra/net.h>
#include <lestra/timer.h>
#include <string.h>

/* Forward declaration for shell execution */
extern void shell_execute_line(const char* line, void (*out_cb)(char));

/* ----- credential store ----- */

#define SSH_MAX_USERS 4

struct ssh_user {
    char username[32];
    uint32_t password_hash;
};

static struct ssh_user users[SSH_MAX_USERS];
static int user_count = 0;

/* Known plaintext passwords for challenge-response verification.
 * These mirror the credential store — in a real system you'd derive
 * the response without storing plaintext, but this is a kernel. */
const char* ssh_known_passwords[] = { "lestra", "admin" };
int ssh_known_password_count = 2;

/* Salted djb2 hash — not cryptographic, but prevents plaintext comparison */
static uint32_t ssh_credential_hash(const char* password) {
    uint32_t hash = 5381;
    const char* salt = "LestraOS";
    for (const char* s = salt; *s; s++) {
        hash = ((hash << 5) + hash) + (uint8_t)*s;
    }
    for (const char* p = password; *p; p++) {
        hash = ((hash << 5) + hash) + (uint8_t)*p;
    }
    return hash;
}

/* djb2 hash of arbitrary string (used for challenge-response) */
static uint32_t ssh_hash(const char* str) {
    uint32_t hash = 5381;
    while (*str) {
        hash = ((hash << 5) + hash) + (uint8_t)*str;
        str++;
    }
    return hash;
}

/* Compute expected response for challenge-response auth:
 * djb2(challenge_hex + ":" + password) */
static uint32_t ssh_challenge_response(const char* challenge_hex,
                                       const char* password) {
    char buf[256];
    int i = 0;
    /* Copy challenge_hex */
    while (challenge_hex[i] && i < 200) {
        buf[i] = challenge_hex[i];
        i++;
    }
    buf[i++] = ':';
    /* Append password */
    const char* p = password;
    while (*p && i < 255) {
        buf[i++] = *p++;
    }
    buf[i] = '\0';
    return ssh_hash(buf);
}

/* Generate 32-byte random challenge using RDRAND */
static void generate_challenge(uint8_t* buf, int len) {
    for (int i = 0; i < len; i++) {
        uint64_t val;
        __asm__ volatile("rdrand %0" : "=r"(val));
        buf[i] = (uint8_t)(val ^ (val >> 8) ^ (val >> 16) ^ (val >> 24));
    }
}

/* Encode raw bytes as hex string. out must have room for len*2+1 chars. */
static void bytes_to_hex(const uint8_t* in, int len, char* out) {
    static const char hexdigits[] = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        out[i*2]   = hexdigits[(in[i] >> 4) & 0xf];
        out[i*2+1] = hexdigits[in[i] & 0xf];
    }
    out[len*2] = '\0';
}

static void ssh_init_users(void) {
    user_count = 0;
    strncpy(users[0].username, "root", 31);
    users[0].password_hash = ssh_credential_hash("lestra");
    user_count = 1;
    /* Add a second user for flexibility */
    strncpy(users[1].username, "admin", 31);
    users[1].password_hash = ssh_credential_hash("admin");
    user_count = 2;
}

static int ssh_auth_check(const char* username, uint32_t hash) {
    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].username, username) == 0 &&
            users[i].password_hash == hash) {
            return 1;
        }
    }
    return 0;
}

/* Find user entry, return pointer or NULL */
static struct ssh_user* ssh_find_user(const char* username) {
    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].username, username) == 0)
            return &users[i];
    }
    return NULL;
}

/* ----- session state ----- */

static struct ssh_session sessions[SSH_MAX_SESSIONS];
static int listen_idx = -1;        /* TCP listen slot index */
static int ssh_running = 0;
static int ssh_port = SSH_DEFAULT_PORT;

void ssh_server_init(void) {
    memset(sessions, 0, sizeof(sessions));
    ssh_running = 0;
    listen_idx = -1;
    ssh_init_users();
    pr_info("ssh: initialized (%d user(s) configured)\n", user_count);
}

int ssh_server_start(uint16_t port) {
    if (ssh_running) {
        pr_warn("ssh: already running\n");
        return 0;
    }

    ssh_port = port;
    listen_idx = tcp_listen(port, SSH_MAX_SESSIONS);
    if (listen_idx < 0) {
        pr_err("ssh: failed to listen on port %u\n", (unsigned)port);
        return -1;
    }

    ssh_running = 1;
    pr_info("ssh: listening on port %u\n", (unsigned)port);
    printk("SSH server listening on port %u\n", (unsigned)port);
    return 0;
}

int ssh_server_stop(void) {
    if (!ssh_running) return 0;

    /* Close all active sessions */
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        if (sessions[i].in_use && sessions[i].conn_idx >= 0) {
            struct tcp_conn* sc = tcp_get_conn(sessions[i].conn_idx);
            if (sc) tcp_close_conn(sc);
            memset(&sessions[i], 0, sizeof(sessions[i]));
        }
    }

    /* Stop listening */
    if (listen_idx >= 0) {
        struct tcp_conn* lc = tcp_get_conn(listen_idx);
        if (lc) {
            lc->in_use = 0;
            lc->state = TCP_CLOSED;
        }
        listen_idx = -1;
    }

    ssh_running = 0;
    pr_info("ssh: stopped\n");
    printk("SSH server stopped.\n");
    return 0;
}

int ssh_server_is_running(void) {
    return ssh_running;
}

/* Find a free session slot */
static int ssh_alloc_session(void) {
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        if (!sessions[i].in_use) return i;
    }
    return -1;
}

/* Find session by connection pointer */
static int ssh_find_session(struct tcp_conn* conn) {
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        if (sessions[i].in_use && sessions[i].conn_idx == (int)(conn - tcp_get_conn(0))) {
            return i;
        }
    }
    return -1;
}

/* Parse a hex string to uint32_t */
static uint32_t parse_hex32(const char* s) {
    uint32_t val = 0;
    for (int i = 0; i < 8 && *s; i++) {
        char c = *s++;
        uint32_t nibble;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = 10 + c - 'a';
        else if (c >= 'A' && c <= 'F') nibble = 10 + c - 'A';
        else break;
        val = (val << 4) | nibble;
    }
    return val;
}

/* Output buffer for capturing shell output */
#define SSH_OUTPUT_BUF 4096
static char ssh_out_buf[SSH_OUTPUT_BUF];
static int  ssh_out_len;

static void ssh_output_cb(char c) {
    if (ssh_out_len < SSH_OUTPUT_BUF - 1) {
        ssh_out_buf[ssh_out_len++] = c;
    }
}

/* Handle a complete line received from a client */
static void ssh_handle_line(int session_idx, struct tcp_conn* c,
                            const char* line) {
    struct ssh_session* s = &sessions[session_idx];

    /* Phase 1: awaiting username input from the login prompt.
     * The user typed their username — store it and prompt for password. */
    if (s->awaiting_username && !s->authenticated) {
        /* Store the username the client just entered */
        strncpy(s->username, line, 31);
        s->username[31] = '\0';
        s->awaiting_username = 0;

        /* Prompt for AUTH response (password hash) */
        const char* pw_prompt = "password: ";
        tcp_send_conn(c, pw_prompt, (uint16_t)strlen(pw_prompt));
        return;
    }

    if (!s->authenticated) {
        /* Expect: AUTH <username> <response_hex> */
        if (strncmp(line, "AUTH ", 5) != 0) {
            const char* denied = "DENIED\n";
            tcp_send_conn(c, denied, (uint16_t)strlen(denied));
            return;
        }
        line += 5;

        /* Parse username (up to first space) */
        char username[32];
        int uidx = 0;
        while (*line && *line != ' ' && uidx < 31) {
            username[uidx++] = *line++;
        }
        username[uidx] = '\0';
        if (*line == ' ') line++;

        /* Parse response (hex string) */
        uint32_t response = parse_hex32(line);

        int auth_ok = 0;

        if (s->challenge_sent) {
            /* Challenge-response: verify djb2(challenge_hex + ":" + password) */
            struct ssh_user* user = ssh_find_user(username);
            if (user) {
                /* We need the plaintext password to compute the expected
                 * response. Since we only store hashes, we must re-hash
                 * with each known password. This is a kernel — we know
                 * the credential list. */
                for (int p = 0; p < ssh_known_password_count; p++) {
                    uint32_t expected = ssh_challenge_response(
                        s->challenge_hex, ssh_known_passwords[p]);
                    if (expected == response &&
                        strcmp(user->username, username) == 0) {
                        /* Also verify the password hash matches the user */
                        if (user->password_hash ==
                            ssh_credential_hash(ssh_known_passwords[p])) {
                            auth_ok = 1;
                            break;
                        }
                    }
                }
            }
        } else {
            /* Backward compat: old client sent AUTH without prior CHALLENGE */
            auth_ok = ssh_auth_check(username, response);
        }

        if (auth_ok) {
            s->authenticated = 1;
            strncpy(s->username, username, 31);
            s->username[31] = '\0';
            s->awaiting_username = 0;
            const char* ok = "OK\n";
            tcp_send_conn(c, ok, 3);
            /* Send shell welcome and first prompt */
            const char* shell_welcome = "\nLestraOS Shell (lsh) - type 'help' for commands\n";
            tcp_send_conn(c, shell_welcome, (uint16_t)strlen(shell_welcome));
            const char* first_prompt = "> ";
            tcp_send_conn(c, first_prompt, 2);
            pr_info("ssh: user '%s' authenticated (session %d)\n",
                    username, session_idx);
        } else {
            const char* denied = "DENIED\n";
            tcp_send_conn(c, denied, 7);
            /* Re-prompt for login */
            const char* retry = "login: ";
            tcp_send_conn(c, retry, (uint16_t)strlen(retry));
            s->awaiting_username = 1;
            pr_info("ssh: auth failed for '%s'\n", username);
        }
        return;
    }

    /* Authenticated — execute shell command */
    if (strcmp(line, "exit") == 0) {
        const char* bye = "Goodbye.\n";
        tcp_send_conn(c, bye, (uint16_t)strlen(bye));
        tcp_close_conn(c);
        s->in_use = 0;
        pr_info("ssh: session %d closed (user '%s')\n",
                session_idx, s->username);
        return;
    }

    /* Execute command through kernel shell */
    ssh_out_len = 0;
    shell_execute_line(line, ssh_output_cb);

    /* Send output back to client */
    if (ssh_out_len > 0) {
        ssh_out_buf[ssh_out_len] = '\0';
        tcp_send_conn(c, ssh_out_buf, (uint16_t)ssh_out_len);
    }
    /* Send a prompt-like terminator so client knows output is done */
    const char* prompt = "\n> ";
    tcp_send_conn(c, prompt, 3);
}

/* Parse one line from a receive buffer. Returns pointer past the \n,
 * or NULL if no complete line is available. Copies the line (without
 * trailing \n/\r) into `line_buf`. */
static const char* ssh_extract_line(const char* buf, int buflen,
                                    char* line_buf, int line_buf_sz) {
    int i = 0;
    while (i < buflen && buf[i] != '\n') i++;
    if (i >= buflen) return NULL;  /* no complete line yet */

    int copy_len = i;
    if (copy_len > 0 && buf[copy_len - 1] == '\r') copy_len--;
    if (copy_len >= line_buf_sz) copy_len = line_buf_sz - 1;
    memcpy(line_buf, buf, copy_len);
    line_buf[copy_len] = '\0';
    return buf + i + 1;  /* past the \n */
}

/* Per-session receive buffers */
static char session_rx_buf[SSH_MAX_SESSIONS][SSH_BUF_SIZE];
static int  session_rx_len[SSH_MAX_SESSIONS];

/* Called periodically by service_tick() to service SSH sessions */
void ssh_server_tick(void) {
    if (!ssh_running || listen_idx < 0) return;

    /* Check for new incoming connections */
    struct tcp_conn* new_conn = NULL;
    if (tcp_accept(listen_idx, &new_conn) == 0 && new_conn) {
        int slot = ssh_alloc_session();
        if (slot < 0) {
            pr_warn("ssh: no free session slots, rejecting\n");
            tcp_close_conn(new_conn);
        } else {
            memset(&sessions[slot], 0, sizeof(sessions[slot]));
            sessions[slot].in_use = 1;
            sessions[slot].conn_idx = (int)(new_conn - tcp_get_conn(0));
            sessions[slot].authenticated = 0;
            sessions[slot].pid = 0;
            sessions[slot].last_activity = timer_get_ms();
            sessions[slot].challenge_sent = 0;
            session_rx_len[slot] = 0;

            pr_info("ssh: new connection from %u.%u.%u.%u:%u (session %d)\n",
                    new_conn->peer_ip.bytes[0], new_conn->peer_ip.bytes[1],
                    new_conn->peer_ip.bytes[2], new_conn->peer_ip.bytes[3],
                    (unsigned)new_conn->peer_port, slot);

            /* Generate and send challenge */
            generate_challenge(sessions[slot].challenge, 32);
            bytes_to_hex(sessions[slot].challenge, 32,
                         sessions[slot].challenge_hex);

            /* Send welcome banner and login prompt */
            const char* welcome1 = "LESTRA_SSH/1.0\n";
            tcp_send_conn(new_conn, welcome1, (uint16_t)strlen(welcome1));
            const char* welcome2 = "Welcome to LestraOS. Please authenticate.\n";
            tcp_send_conn(new_conn, welcome2, (uint16_t)strlen(welcome2));

            /* Send challenge */
            char challenge_msg[136]; /* "CHALLENGE " + 64 hex + "\n" + NUL */
            int pos = 0;
            const char* prefix = "CHALLENGE ";
            while (*prefix) challenge_msg[pos++] = *prefix++;
            for (int j = 0; j < 64; j++)
                challenge_msg[pos++] = sessions[slot].challenge_hex[j];
            challenge_msg[pos++] = '\n';
            challenge_msg[pos] = '\0';
            tcp_send_conn(new_conn, challenge_msg, (uint16_t)pos);

            /* Send login prompt */
            const char* login_prompt = "login: ";
            tcp_send_conn(new_conn, login_prompt, (uint16_t)strlen(login_prompt));

            sessions[slot].challenge_sent = 1;
            sessions[slot].awaiting_username = 1;
        }
    }

    /* Service each active session */
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        if (!sessions[i].in_use) continue;

        struct tcp_conn* c = tcp_get_conn(sessions[i].conn_idx);
        if (!c || c->state != TCP_ESTABLISHED) {
            pr_info("ssh: session %d connection lost\n", i);
            sessions[i].in_use = 0;
            continue;
        }

        /* Check idle timeout */
        if (timer_get_ms() - sessions[i].last_activity > SSH_TIMEOUT_MS) {
            pr_info("ssh: session %d idle timeout\n", i);
            const char* msg = "Connection timed out.\n";
            tcp_send_conn(c, msg, (uint16_t)strlen(msg));
            tcp_close_conn(c);
            sessions[i].in_use = 0;
            continue;
        }

        /* Read available data */
        if (c->rx_len > 0) {
            uint16_t space = SSH_BUF_SIZE - session_rx_len[i];
            uint16_t to_read = c->rx_len;
            if (to_read > space) to_read = space;
            if (to_read > 0) {
                memcpy(&session_rx_buf[i][session_rx_len[i]], c->rx_buf, to_read);
                /* Shift the connection's rx buffer */
                memmove(c->rx_buf, &c->rx_buf[to_read], c->rx_len - to_read);
                c->rx_len -= to_read;
                session_rx_len[i] += to_read;
            }
            sessions[i].last_activity = timer_get_ms();
        }

        /* Process complete lines */
        char line[SSH_BUF_SIZE];
        const char* remaining = session_rx_buf[i];
        int remaining_len = session_rx_len[i];

        while (remaining_len > 0) {
            const char* next = ssh_extract_line(remaining, remaining_len,
                                                line, sizeof(line));
            if (!next) break;
            int consumed = (int)(next - remaining);
            remaining += consumed;
            remaining_len -= consumed;

            if (strlen(line) > 0) {
                ssh_handle_line(i, c, line);
            }
            /* After handle_line, connection may be closed */
            if (!sessions[i].in_use) break;
        }

        /* Compact the receive buffer */
        if (sessions[i].in_use && remaining_len >= 0) {
            int leftover = (int)(remaining - session_rx_buf[i]);
            if (leftover > 0 && leftover < session_rx_len[i]) {
                memmove(session_rx_buf[i], remaining, remaining_len);
            }
            session_rx_len[i] = remaining_len;
        }
    }
}
