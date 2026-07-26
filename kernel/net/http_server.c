/*
 * Lestra OS - Built-in HTTP/1.0 server + Cloud/VPS Management API
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Serves files from the VFS root over plain HTTP. Designed for
 * embedded web UIs, status pages, and local tooling.
 *
 * Cloud/VPS Management API:
 *   GET  /status    — returns JSON with uptime, memory, processes
 *   POST /reboot    — triggers system reboot
 *   POST /shutdown  — triggers system shutdown
 *
 * The management API starts automatically in cloud/VPS boot mode
 * and provides remote monitoring and control capabilities.
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

#define HTTP_MAX_CONNS 4

struct http_conn {
    struct tcp_conn* tcp;
    int active;
    char req[2048];
    int req_len;
};

static int http_listen_idx = -1;
static struct http_conn http_conns[HTTP_MAX_CONNS];

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

static void http_serve_file(struct tcp_conn* c, const char* path) {
    struct stat st;
    if (vfs_stat(path, &st) < 0 || st.size == 0) {
        const char* resp =
            "HTTP/1.0 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 14\r\n"
            "Connection: close\r\n"
            "\r\n"
            "404 Not Found\n";
        tcp_send_conn(c, resp, (uint16_t)strlen(resp));
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

    tcp_send_conn(c, header, (uint16_t)hlen);

    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) return;

    uint8_t chunk[1400];
    uint64_t offset = 0;
    while (offset < st.size) {
        size_t to_read = st.size - offset;
        if (to_read > sizeof(chunk)) to_read = sizeof(chunk);
        ssize_t n = vfs_read(fd, chunk, to_read);
        if (n <= 0) break;
        tcp_send_conn(c, chunk, (uint16_t)n);
        offset += n;
    }
    vfs_close(fd);
}

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
    if (strcmp(path, "/status") == 0) {
        /* GET /status — returns system status as JSON */
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
        tcp_send_conn(hc->tcp, header, (uint16_t)hlen);
        tcp_send_conn(hc->tcp, json_body, (uint16_t)jlen);
        pr_info("http_mgmt: GET /status -> %d bytes\n", jlen);
        return;
    }

    if (strcmp(path, "/reboot") == 0 && strcmp(method, "POST") == 0) {
        /* POST /reboot — trigger system reboot */
        const char* resp =
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 22\r\n"
            "Connection: close\r\n"
            "\r\n"
            "{\"action\":\"reboot\"}\n";
        tcp_send_conn(hc->tcp, resp, (uint16_t)strlen(resp));
        pr_info("http_mgmt: POST /reboot — rebooting system\n");
        /* Reboot after a short delay so the HTTP response is sent */
        extern void reboot_system(void);
        timer_wait_ms(100);
        reboot_system();
        return;
    }

    if (strcmp(path, "/shutdown") == 0 && strcmp(method, "POST") == 0) {
        /* POST /shutdown — trigger system shutdown */
        const char* resp =
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 26\r\n"
            "Connection: close\r\n"
            "\r\n"
            "{\"action\":\"shutdown\"}\n";
        tcp_send_conn(hc->tcp, resp, (uint16_t)strlen(resp));
        pr_info("http_mgmt: POST /shutdown — shutting down system\n");
        extern void shutdown_system(void);
        timer_wait_ms(100);
        shutdown_system();
        return;
    }

    /* /reboot and /shutdown also accept GET for simple browser testing */
    if (strcmp(path, "/reboot") == 0) {
        const char* resp =
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 90\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<html><body><h2>Reboot</h2><p>POST to /reboot to reboot the system.</p></body></html>\n";
        tcp_send_conn(hc->tcp, resp, (uint16_t)strlen(resp));
        return;
    }
    if (strcmp(path, "/shutdown") == 0) {
        const char* resp =
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 96\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<html><body><h2>Shutdown</h2><p>POST to /shutdown to halt the system.</p></body></html>\n";
        tcp_send_conn(hc->tcp, resp, (uint16_t)strlen(resp));
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
        tcp_send_conn(hc->tcp, resp, (uint16_t)strlen(resp));
        return;
    }

    if (pi == 0 || path[0] != '/') {
        path[0] = '/';
        path[1] = '\0';
    }

    pr_info("http_server: %s %s\n", method, path);

    char vfs_path[256];
    ksnprintf(vfs_path, sizeof(vfs_path), "%s", path);

    http_serve_file(hc->tcp, vfs_path);
}

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
 * Cloud/VPS HTTP Management API
 * ============================================================
 *
 * A dedicated HTTP server for cloud/VPS management. Starts
 * automatically in cloud boot mode. Provides endpoints for
 * system monitoring and remote control.
 *
 * Endpoints:
 *   GET  /status    — JSON with uptime, memory, process count, IP
 *   POST /reboot    — reboot the system (returns JSON confirmation)
 *   POST /shutdown  — halt the system (returns JSON confirmation)
 *
 * This is separate from the general-purpose HTTP file server above.
 * It has its own listen socket and connection slots.
 * ============================================================ */

#define HTTP_MGMT_MAX_CONNS 4
#define HTTP_MGMT_PORT      8080

static int http_mgmt_listen_idx = -1;
static struct http_conn http_mgmt_conns[HTTP_MGMT_MAX_CONNS];

/* Forward declaration for reboot/shutdown from power module */
extern void reboot_system(void);
extern void shutdown_system(void);

void http_mgmt_start(uint16_t port) {
    http_mgmt_listen_idx = tcp_listen(port, HTTP_MGMT_MAX_CONNS);
    memset(http_mgmt_conns, 0, sizeof(http_mgmt_conns));
    if (http_mgmt_listen_idx >= 0)
        pr_info("http_mgmt: Cloud/VPS management API listening on port %u\n", (unsigned)port);
    else
        pr_warn("http_mgmt: failed to listen on port %u\n", (unsigned)port);
}

void http_mgmt_tick(void) {
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
            http_mgmt_conns[slot].active = 1;
            http_mgmt_conns[slot].req_len = 0;
            pr_info("http_mgmt: new management client on slot %d\n", slot);
        } else {
            pr_warn("http_mgmt: connection slots full, dropping\n");
            tcp_close_conn(ac);
        }
    }

    /* Service each active management connection */
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
