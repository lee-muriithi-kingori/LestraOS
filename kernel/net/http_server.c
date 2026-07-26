/*
 * Lestra OS - Built-in HTTP/1.0 server + Cloud/VPS Management API (HTTPS)
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Serves files from the VFS root over plain HTTP. Designed for
 * embedded web UIs, status pages, and local tooling.
 *
 * Cloud/VPS Management API:
 *   GET  /status    — returns JSON with uptime, memory, processes
 *   GET  /metrics   — returns detailed JSON with CPU, memory, network, disk stats
 *   POST /reboot    — triggers system reboot
 *   POST /shutdown  — triggers system shutdown
 *
 * HTTPS (TLS 1.2) Management API:
 *   Same endpoints served on port 8443 over TLS-encrypted connections.
 *   Dangerous operations (/reboot, /shutdown) are rate-limited on
 *   plaintext HTTP (30-second cooldown) but unrestricted on HTTPS.
 *
 * The management API starts automatically in cloud/VPS boot mode
 * and provides remote monitoring and control capabilities.
 * Both HTTP (8080) and HTTPS (8443) servers run simultaneously;
 * HTTPS is recommended for all management operations.
 */

#include <lestra/types.h>
#include <lestra/net.h>
#include <lestra/vfs.h>
#include <lestra/printk.h>
#include <lestra/timer.h>
#include <lestra/mm.h>
#include <lestra/sched.h>
#include <string.h>

#define snprintf ksnprintf

/* ===== Forward declarations for TLS server (kernel/net/tls_server.c) ===== */
struct tls_server_conn;  /* opaque; defined in tls_server.c */
extern void  tls_server_init(void);
extern int   tls_server_accept(struct tcp_conn* tcp, struct tls_server_conn** out);
extern int   tls_server_send(struct tls_server_conn* ctx, const void* data, uint16_t len);
extern int   tls_server_recv(struct tls_server_conn* ctx, void* buf, uint16_t bufsz, uint32_t timeout_ms);
extern void  tls_server_close(struct tls_server_conn* ctx);
extern int   tls_server_is_active(struct tls_server_conn* ctx);

/* ===== Forward declarations for power management ===== */
extern void reboot_system(void);
extern void shutdown_system(void);

/* ===== Forward declarations for disk subsystem ===== */
extern int  ahci_has_drive(void);

/* ===== Constants ===== */
#define HTTP_MAX_CONNS           4
#define HTTP_MGMT_MAX_CONNS      4
#define HTTPS_MGMT_MAX_CONNS     4
#define HTTP_MGMT_PORT           8080
#define HTTPS_MGMT_PORT          8443

/*
 * Rate-limit cooldown for dangerous operations on plaintext HTTP.
 * POST /reboot and POST /shutdown are limited to one attempt per
 * 30 seconds when accessed over unencrypted HTTP.  Over HTTPS
 * (TLS-encrypted, authenticated channel) there is no cooldown.
 */
#define HTTP_DANGER_COOLDOWN_MS  30000

/* ===== Connection state ===== */
struct http_conn {
    struct tcp_conn*          tcp;
    struct tls_server_conn*   tls;      /* non-NULL for HTTPS connections */
    int                       active;
    int                       is_tls;   /* 1 if this is a TLS (HTTPS) conn */
    char                      req[2048];
    int                       req_len;
};

/* ===== General HTTP file server state ===== */
static int http_listen_idx = -1;
static struct http_conn http_conns[HTTP_MAX_CONNS];

/* ===== HTTP Management API state (plaintext, port 8080) ===== */
static int http_mgmt_listen_idx = -1;
static struct http_conn http_mgmt_conns[HTTP_MGMT_MAX_CONNS];

/* ===== HTTPS Management API state (TLS, port 8443) ===== */
static int https_mgmt_listen_idx = -1;

/* ===== Rate-limit state for dangerous ops on plaintext HTTP ===== */
static uint64_t http_last_reboot_ms    = 0;
static uint64_t http_last_shutdown_ms  = 0;

/* ===== Helper: send data over either plain TCP or TLS ===== */
static int http_conn_send(struct http_conn* hc, const void* data, uint16_t len) {
    if (hc->is_tls && hc->tls) {
        return tls_server_send(hc->tls, data, len);
    }
    return tcp_send_conn(hc->tcp, data, len);
}

/* ===== Content type guessing ===== */
static const char* guess_content_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    ext++;
    if (strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0) return "text/html";
    if (strcmp(ext, "css") == 0) return "text/css";
    if (strcmp(ext, "js") == 0) return "application/javascript";
    if (strcmp(ext, "json") == 0) return "application/json";
    if (strcmp(ext, "txt") == 0 || strcmp(ext, "log") == 0) return "text/plain";
    if (strcmp(ext, "png") == 0) return "image/png";
    if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, "gif") == 0) return "image/gif";
    if (strcmp(ext, "svg") == 0) return "image/svg+xml";
    if (strcmp(ext, "ico") == 0) return "image/x-icon";
    if (strcmp(ext, "xml") == 0) return "text/xml";
    if (strcmp(ext, "pdf") == 0) return "application/pdf";
    if (strcmp(ext, "wasm") == 0) return "application/wasm";
    return "application/octet-stream";
}

/* ===== File serving (used by general HTTP server and management API) ===== */
static void http_serve_file(struct http_conn* hc, const char* path) {
    struct stat st;
    if (vfs_stat(path, &st) < 0 || st.size == 0) {
        const char* resp =
            "HTTP/1.0 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 14\r\n"
            "Connection: close\r\n"
            "\r\n"
            "404 Not Found\n";
        http_conn_send(hc, resp, (uint16_t)strlen(resp));
        return;
    }

    const char* ct = guess_content_type(path);
    char header[256];
    int hlen = ksnprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "\r\n",
        ct, (unsigned)st.size);

    http_conn_send(hc, header, (uint16_t)hlen);

    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) return;

    uint8_t chunk[1400];
    uint64_t offset = 0;
    while (offset < st.size) {
        size_t to_read = st.size - offset;
        if (to_read > sizeof(chunk)) to_read = sizeof(chunk);
        ssize_t n = vfs_read(fd, chunk, to_read);
        if (n <= 0) break;
        http_conn_send(hc, chunk, (uint16_t)n);
        offset += n;
    }
    vfs_close(fd);
}

/* ===== Request handler (shared by HTTP and HTTPS management) ===== */
static void http_handle_request(struct http_conn* hc) {
    char* req = hc->req;
    int len = hc->req_len;

    if (len < 4) return;
    req[len] = '\0';

    char method[8] = {0};
    char path[256] = {0};
    int mi = 0, pi = 0;

    int i = 0;
    while (i < len && req[i] != ' ' && mi < 7) method[mi++] = req[i++];
    i++;
    while (i < len && req[i] != ' ' && req[i] != '\r' && pi < 255) path[pi++] = req[i++];

    /* ----- Cloud/VPS Management API endpoints ----- */

    /* GET /status — returns system status as JSON */
    if (strcmp(path, "/status") == 0) {
        char json_body[1024];
        uint64_t uptime_ms = timer_get_ms();
        uint64_t uptime_s = uptime_ms / 1000;
        uintptr_t total_mem = pmm_get_total();
        uintptr_t free_mem = pmm_get_free();
        uintptr_t used_mem = pmm_get_used();
        int proc_count = 0;
        for (int p = 0; p < MAX_PROCS; p++) {
            if (procs[p].state != PROC_FREE) proc_count++;
        }
        ipv4_addr_t ip = net_get_ip();
        int net_up = net_is_up();

        int jlen = ksnprintf(json_body, sizeof(json_body),
            "{\"os\":\"LestraOS\",\"version\":\"1.0.0-alpha\","
            "\"uptime_ms\":%llu,\"uptime_s\":%llu,"
            "\"memory\":{\"total_kb\":%llu,\"free_kb\":%llu,\"used_kb\":%llu},"
            "\"processes\":%d,"
            "\"network\":{\"up\":%d,\"ip\":\"%u.%u.%u.%u\"}"
            "}\n",
            (unsigned long long)uptime_ms, (unsigned long long)uptime_s,
            (unsigned long long)(total_mem / 1024),
            (unsigned long long)(free_mem / 1024),
            (unsigned long long)(used_mem / 1024),
            proc_count,
            net_up,
            ip.bytes[0], ip.bytes[1], ip.bytes[2], ip.bytes[3]);

        char header[256];
        int hlen = ksnprintf(header, sizeof(header),
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n", jlen);
        http_conn_send(hc, header, (uint16_t)hlen);
        http_conn_send(hc, json_body, (uint16_t)jlen);
        pr_info("http_mgmt: GET /status -> %d bytes (tls=%d)\n", jlen, hc->is_tls);
        return;
    }

    /* GET /metrics — returns detailed system stats for VPS monitoring tools.
     * Provides CPU, memory, network, and disk metrics in JSON format. */
    if (strcmp(path, "/metrics") == 0) {
        char json_body[2048];
        uint64_t uptime_ms = timer_get_ms();
        uint64_t uptime_s = uptime_ms / 1000;
        uintptr_t total_mem = pmm_get_total();
        uintptr_t free_mem = pmm_get_free();
        uintptr_t used_mem = pmm_get_used();
        uintptr_t mem_percent = 0;
        if (total_mem > 0) {
            mem_percent = (used_mem * 100) / total_mem;
        }

        int proc_count = 0;
        int proc_running = 0;
        int proc_runnable = 0;
        for (int p = 0; p < MAX_PROCS; p++) {
            if (procs[p].state != PROC_FREE) proc_count++;
            if (procs[p].state == PROC_RUNNING) proc_running++;
            if (procs[p].state == PROC_RUNNABLE) proc_runnable++;
        }
        int load_avg = proc_running + proc_runnable;

        ipv4_addr_t ip = net_get_ip();
        ipv4_addr_t gw = net_get_gateway();
        ipv4_addr_t dns = net_get_dns();
        int net_up = net_is_up();

        int disk_present = ahci_has_drive();

        int jlen = ksnprintf(json_body, sizeof(json_body),
            "{\"os\":\"LestraOS\",\"version\":\"1.0.0-alpha\","
            "\"timestamp_ms\":%llu,"
            "\"cpu\":{"
            "\"uptime_ms\":%llu,\"uptime_s\":%llu,"
            "\"processes_total\":%d,\"processes_running\":%d,\"processes_runnable\":%d,"
            "\"load_avg\":%d"
            "},"
            "\"memory\":{"
            "\"total_kb\":%llu,\"free_kb\":%llu,\"used_kb\":%llu,\"used_percent\":%llu"
            "},"
            "\"network\":{"
            "\"up\":%d,"
            "\"ip\":\"%u.%u.%u.%u\","
            "\"gateway\":\"%u.%u.%u.%u\","
            "\"dns\":\"%u.%u.%u.%u\""
            "},"
            "\"disk\":{"
            "\"present\":%d"
            "}"
            "}\n",
            (unsigned long long)uptime_ms,
            (unsigned long long)uptime_ms, (unsigned long long)uptime_s,
            proc_count, proc_running, proc_runnable,
            load_avg,
            (unsigned long long)(total_mem / 1024),
            (unsigned long long)(free_mem / 1024),
            (unsigned long long)(used_mem / 1024),
            (unsigned long long)mem_percent,
            net_up,
            ip.bytes[0], ip.bytes[1], ip.bytes[2], ip.bytes[3],
            gw.bytes[0], gw.bytes[1], gw.bytes[2], gw.bytes[3],
            dns.bytes[0], dns.bytes[1], dns.bytes[2], dns.bytes[3],
            disk_present);

        char header[256];
        int hlen = ksnprintf(header, sizeof(header),
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n", jlen);
        http_conn_send(hc, header, (uint16_t)hlen);
        http_conn_send(hc, json_body, (uint16_t)jlen);
        pr_info("http_mgmt: GET /metrics -> %d bytes (tls=%d)\n", jlen, hc->is_tls);
        return;
    }

    /* POST /reboot — trigger system reboot.
     * Rate-limited on plaintext HTTP (30-second cooldown between attempts).
     * No cooldown on HTTPS (TLS provides authenticated/encrypted channel). */
    if (strcmp(path, "/reboot") == 0 && strcmp(method, "POST") == 0) {
        /* Rate-limit check for plaintext HTTP */
        if (!hc->is_tls) {
            uint64_t now = timer_get_ms();
            uint64_t elapsed = now - http_last_reboot_ms;
            if (elapsed < HTTP_DANGER_COOLDOWN_MS) {
                uint64_t remaining = HTTP_DANGER_COOLDOWN_MS - elapsed;
                char body[128];
                int blen = ksnprintf(body, sizeof(body),
                    "{\"error\":\"rate_limited\",\"endpoint\":\"/reboot\","
                    "\"cooldown_ms\":%llu,\"remaining_ms\":%llu,"
                    "\"message\":\"Use HTTPS (port 8443) for unrestricted access\"}\n",
                    (unsigned long long)HTTP_DANGER_COOLDOWN_MS,
                    (unsigned long long)remaining);
                char header[256];
                int hlen = ksnprintf(header, sizeof(header),
                    "HTTP/1.0 429 Too Many Requests\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %d\r\n"
                    "Connection: close\r\n"
                    "Retry-After: %llu\r\n"
                    "\r\n", blen, (unsigned long long)(remaining / 1000));
                http_conn_send(hc, header, (uint16_t)hlen);
                http_conn_send(hc, body, (uint16_t)blen);
                pr_warn("http_mgmt: POST /reboot rate-limited on HTTP (remaining %llu ms)\n",
                        (unsigned long long)remaining);
                return;
            }
            http_last_reboot_ms = now;
        }

        const char* resp =
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 22\r\n"
            "Connection: close\r\n"
            "\r\n"
            "{\"action\":\"reboot\"}\n";
        http_conn_send(hc, resp, (uint16_t)strlen(resp));
        pr_info("http_mgmt: POST /reboot — rebooting system (tls=%d)\n", hc->is_tls);
        /* Reboot after a short delay so the HTTP response is sent */
        timer_wait_ms(100);
        reboot_system();
        return;
    }

    /* POST /shutdown — trigger system shutdown.
     * Rate-limited on plaintext HTTP (30-second cooldown between attempts).
     * No cooldown on HTTPS (TLS provides authenticated/encrypted channel). */
    if (strcmp(path, "/shutdown") == 0 && strcmp(method, "POST") == 0) {
        /* Rate-limit check for plaintext HTTP */
        if (!hc->is_tls) {
            uint64_t now = timer_get_ms();
            uint64_t elapsed = now - http_last_shutdown_ms;
            if (elapsed < HTTP_DANGER_COOLDOWN_MS) {
                uint64_t remaining = HTTP_DANGER_COOLDOWN_MS - elapsed;
                char body[128];
                int blen = ksnprintf(body, sizeof(body),
                    "{\"error\":\"rate_limited\",\"endpoint\":\"/shutdown\","
                    "\"cooldown_ms\":%llu,\"remaining_ms\":%llu,"
                    "\"message\":\"Use HTTPS (port 8443) for unrestricted access\"}\n",
                    (unsigned long long)HTTP_DANGER_COOLDOWN_MS,
                    (unsigned long long)remaining);
                char header[256];
                int hlen = ksnprintf(header, sizeof(header),
                    "HTTP/1.0 429 Too Many Requests\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %d\r\n"
                    "Connection: close\r\n"
                    "Retry-After: %llu\r\n"
                    "\r\n", blen, (unsigned long long)(remaining / 1000));
                http_conn_send(hc, header, (uint16_t)hlen);
                http_conn_send(hc, body, (uint16_t)blen);
                pr_warn("http_mgmt: POST /shutdown rate-limited on HTTP (remaining %llu ms)\n",
                        (unsigned long long)remaining);
                return;
            }
            http_last_shutdown_ms = now;
        }

        const char* resp =
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 26\r\n"
            "Connection: close\r\n"
            "\r\n"
            "{\"action\":\"shutdown\"}\n";
        http_conn_send(hc, resp, (uint16_t)strlen(resp));
        pr_info("http_mgmt: POST /shutdown — shutting down system (tls=%d)\n", hc->is_tls);
        timer_wait_ms(100);
        shutdown_system();
        return;
    }

    /* /reboot and /shutdown also accept GET for simple browser testing.
     * Show an informational page explaining how to use these endpoints. */
    if (strcmp(path, "/reboot") == 0) {
        const char* resp =
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 158\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<html><body><h2>Reboot</h2>"
            "<p>POST to /reboot to reboot the system.</p>"
            "<p>Plaintext HTTP is rate-limited. Use HTTPS (port 8443) for unrestricted access.</p>"
            "</body></html>\n";
        http_conn_send(hc, resp, (uint16_t)strlen(resp));
        return;
    }
    if (strcmp(path, "/shutdown") == 0) {
        const char* resp =
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 164\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<html><body><h2>Shutdown</h2>"
            "<p>POST to /shutdown to halt the system.</p>"
            "<p>Plaintext HTTP is rate-limited. Use HTTPS (port 8443) for unrestricted access.</p>"
            "</body></html>\n";
        http_conn_send(hc, resp, (uint16_t)strlen(resp));
        return;
    }

    /* ----- Regular file-serving (original behavior) ----- */

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        const char* resp =
            "HTTP/1.0 405 Method Not Allowed\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 22\r\n"
            "Connection: close\r\n"
            "\r\n"
            "405 Method Not Allowed\n";
        http_conn_send(hc, resp, (uint16_t)strlen(resp));
        return;
    }

    if (pi == 0 || path[0] != '/') {
        path[0] = '/';
        path[1] = '\0';
    }

    pr_info("http_server: %s %s\n", method, path);

    char vfs_path[256];
    ksnprintf(vfs_path, sizeof(vfs_path), "%s", path);

    http_serve_file(hc, vfs_path);
}

/* ============================================================
 * General-purpose HTTP file server (port 80 or user-specified)
 * ============================================================ */

void http_server_start(uint16_t port) {
    http_listen_idx = tcp_listen(port, HTTP_MAX_CONNS);
    memset(http_conns, 0, sizeof(http_conns));
    if (http_listen_idx >= 0)
        pr_info("http_server: listening on port %u\n", (unsigned)port);
    else
        pr_warn("http_server: failed to listen on port %u\n", (unsigned)port);
}

void http_server_tick(void) {
    if (http_listen_idx < 0) return;

    struct tcp_conn* lc = tcp_get_conn(http_listen_idx);
    if (!lc) return;

    if (lc->pending_accept && lc->accepted) {
        struct tcp_conn* ac = lc->accepted;
        lc->pending_accept = 0;
        lc->accepted = NULL;

        int slot = -1;
        for (int i = 0; i < HTTP_MAX_CONNS; i++) {
            if (!http_conns[i].active) { slot = i; break; }
        }
        if (slot >= 0) {
            http_conns[slot].tcp = ac;
            http_conns[slot].tls = NULL;
            http_conns[slot].is_tls = 0;
            http_conns[slot].active = 1;
            http_conns[slot].req_len = 0;
            pr_info("http_server: new client on slot %d\n", slot);
        } else {
            pr_warn("http_server: connection slots full, dropping\n");
            tcp_close_conn(ac);
        }
    }

    for (int i = 0; i < HTTP_MAX_CONNS; i++) {
        if (!http_conns[i].active) continue;
        struct http_conn* hc = &http_conns[i];
        struct tcp_conn* tc = hc->tcp;

        if (!tc->in_use || tc->state == TCP_CLOSED) {
            hc->active = 0;
            continue;
        }

        if (tc->rx_len > 0) {
            int space = (int)sizeof(hc->req) - hc->req_len - 1;
            if (space > 0 && tc->rx_len > 0) {
                int n = tc->rx_len;
                if (n > space) n = space;
                memcpy(&hc->req[hc->req_len], tc->rx_buf, n);
                hc->req_len += n;
                memmove(tc->rx_buf, &tc->rx_buf[n], tc->rx_len - n);
                tc->rx_len -= n;
            }

            int complete = 0;
            for (int j = 0; j + 3 < hc->req_len; j++) {
                if (hc->req[j] == '\r' && hc->req[j+1] == '\n' &&
                    hc->req[j+2] == '\r' && hc->req[j+3] == '\n') {
                    complete = 1;
                    break;
                }
            }

            if (complete) {
                http_handle_request(hc);
                tcp_close_conn(tc);
                hc->active = 0;
            }
        }

        if (tc->rx_closed && !hc->active) {
            hc->active = 0;
        }
    }
}

/* ============================================================
 * Cloud/VPS HTTP Management API (plaintext, port 8080)
 * ============================================================
 *
 * A dedicated HTTP server for cloud/VPS management. Starts
 * automatically in cloud boot mode. Provides endpoints for
 * system monitoring and remote control.
 *
 * Endpoints:
 *   GET  /status    — JSON with uptime, memory, process count, IP
 *   GET  /metrics   — JSON with detailed CPU, memory, network, disk stats
 *   POST /reboot    — reboot the system (rate-limited on plaintext HTTP)
 *   POST /shutdown  — halt the system (rate-limited on plaintext HTTP)
 *
 * This is separate from the general-purpose HTTP file server above.
 * It has its own listen socket and connection slots.
 * ============================================================ */

void http_mgmt_start(uint16_t port) {
    http_mgmt_listen_idx = tcp_listen(port, HTTP_MGMT_MAX_CONNS);
    memset(http_mgmt_conns, 0, sizeof(http_mgmt_conns));
    http_last_reboot_ms = 0;
    http_last_shutdown_ms = 0;
    if (http_mgmt_listen_idx >= 0)
        pr_info("http_mgmt: Cloud/VPS management API listening on port %u (plaintext HTTP)\n", (unsigned)port);
    else
        pr_warn("http_mgmt: failed to listen on port %u\n", (unsigned)port);
}

/* ============================================================
 * Cloud/VPS HTTPS Management API (TLS 1.2, port 8443)
 * ============================================================
 *
 * Starts a TLS-encrypted management API alongside the plaintext
 * HTTP management API.  Connections are handled synchronously:
 * accept → TLS handshake → read request → process → send
 * response → close.
 *
 * All management endpoints are available over HTTPS without
 * rate-limiting, since TLS provides an encrypted channel.
 *
 * Must call tls_server_init() BEFORE http_mgmt_tls_start()
 * so that the server's key pair and self-signed certificate
 * are generated before any TLS handshake is attempted.
 * ============================================================ */

void http_mgmt_tls_start(uint16_t port) {
    https_mgmt_listen_idx = tcp_listen(port, HTTPS_MGMT_MAX_CONNS);
    if (https_mgmt_listen_idx >= 0)
        pr_info("http_mgmt: HTTPS (TLS 1.2) management API listening on port %u\n", (unsigned)port);
    else
        pr_warn("http_mgmt: failed to listen on HTTPS port %u\n", (unsigned)port);
}

/* ============================================================
 * Combined management API tick
 * ============================================================
 *
 * Polls both the plaintext HTTP (8080) and HTTPS (8443)
 * management servers.  HTTP connections use the existing
 * non-blocking model.  HTTPS connections are handled
 * synchronously: TLS handshake → recv request → process →
 * send response → close.
 *
 * Called from the kernel main loop / timer tick.
 * ============================================================ */

void http_mgmt_tick(void) {
    /* ----- HTTPS (TLS) connection handling ----- */
    if (https_mgmt_listen_idx >= 0) {
        struct tcp_conn* tls_lc = tcp_get_conn(https_mgmt_listen_idx);
        if (tls_lc && tls_lc->pending_accept && tls_lc->accepted) {
            struct tcp_conn* ac = tls_lc->accepted;
            tls_lc->pending_accept = 0;
            tls_lc->accepted = NULL;

            pr_info("http_mgmt: new HTTPS connection, starting TLS handshake...\n");

            /* Perform TLS 1.2 handshake on the accepted TCP connection */
            struct tls_server_conn* tls_ctx = NULL;
            int hs_result = tls_server_accept(ac, &tls_ctx);

            if (hs_result < 0 || tls_ctx == NULL) {
                pr_warn("http_mgmt: TLS handshake failed, closing connection\n");
                tcp_close_conn(ac);
                /* Continue to HTTP handling below — don't return */
            } else {
                pr_info("http_mgmt: TLS handshake succeeded\n");

                /* Read the encrypted HTTP request.
                 * Use a 5-second timeout so we don't block forever
                 * on a slow or malicious client. */
                char tls_req_buf[2048];
                int tls_req_len = tls_server_recv(tls_ctx, tls_req_buf,
                                                  sizeof(tls_req_buf), 5000);

                if (tls_req_len <= 0) {
                    pr_warn("http_mgmt: TLS recv failed or timed out (result=%d)\n", tls_req_len);
                    tls_server_close(tls_ctx);
                } else {
                    /* Build an http_conn for the request handler.
                     * The handler will use tls_server_send via
                     * http_conn_send() for all responses. */
                    struct http_conn hc;
                    memset(&hc, 0, sizeof(hc));
                    hc.tcp = ac;
                    hc.tls = tls_ctx;
                    hc.is_tls = 1;
                    hc.active = 1;
                    memcpy(hc.req, tls_req_buf, tls_req_len);
                    hc.req_len = tls_req_len;

                    /* Process the HTTPS request using the same handler */
                    http_handle_request(&hc);

                    /* Close the TLS connection after the response is sent */
                    pr_info("http_mgmt: HTTPS request processed, closing TLS connection\n");
                    tls_server_close(tls_ctx);
                }
            }
        }
    }

    /* ----- Plaintext HTTP connection handling ----- */
    if (http_mgmt_listen_idx < 0) return;

    struct tcp_conn* lc = tcp_get_conn(http_mgmt_listen_idx);
    if (!lc) return;

    /* Accept new connections */
    if (lc->pending_accept && lc->accepted) {
        struct tcp_conn* ac = lc->accepted;
        lc->pending_accept = 0;
        lc->accepted = NULL;

        int slot = -1;
        for (int i = 0; i < HTTP_MGMT_MAX_CONNS; i++) {
            if (!http_mgmt_conns[i].active) { slot = i; break; }
        }
        if (slot >= 0) {
            http_mgmt_conns[slot].tcp = ac;
            http_mgmt_conns[slot].tls = NULL;
            http_mgmt_conns[slot].is_tls = 0;
            http_mgmt_conns[slot].active = 1;
            http_mgmt_conns[slot].req_len = 0;
            pr_info("http_mgmt: new plaintext management client on slot %d\n", slot);
        } else {
            pr_warn("http_mgmt: connection slots full, dropping\n");
            tcp_close_conn(ac);
        }
    }

    /* Service each active plaintext management connection */
    for (int i = 0; i < HTTP_MGMT_MAX_CONNS; i++) {
        if (!http_mgmt_conns[i].active) continue;
        struct http_conn* hc = &http_mgmt_conns[i];
        struct tcp_conn* tc = hc->tcp;

        if (!tc->in_use || tc->state == TCP_CLOSED) {
            hc->active = 0;
            continue;
        }

        if (tc->rx_len > 0) {
            int space = (int)sizeof(hc->req) - hc->req_len - 1;
            if (space > 0 && tc->rx_len > 0) {
                int n = tc->rx_len;
                if (n > space) n = space;
                memcpy(&hc->req[hc->req_len], tc->rx_buf, n);
                hc->req_len += n;
                memmove(tc->rx_buf, &tc->rx_buf[n], tc->rx_len - n);
                tc->rx_len -= n;
            }

            int complete = 0;
            for (int j = 0; j + 3 < hc->req_len; j++) {
                if (hc->req[j] == '\r' && hc->req[j+1] == '\n' &&
                    hc->req[j+2] == '\r' && hc->req[j+3] == '\n') {
                    complete = 1;
                    break;
                }
            }

            if (complete) {
                http_handle_request(hc);
                tcp_close_conn(tc);
                hc->active = 0;
            }
        }

        if (tc->rx_closed && !hc->active) {
            hc->active = 0;
        }
    }
}
