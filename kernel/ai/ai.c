/*
 * Lestra OS - AI Subsystem implementation
 * Copyright (c) 2026 lestramk.org
 *
 * Implements:
 *   - Per-provider API key storage (in-memory; keys never persist to disk)
 *   - Tool registry with built-in tools (shell, file ops, package mgmt)
 *   - Chat loop that makes real provider API calls over the network
 *
 * ai_http_post() below does a real DNS lookup (net_resolve), real TCP
 * connect/send/recv (kernel/net/tcp.c), and a real TLS 1.2 handshake for
 * HTTPS endpoints (kernel/net/tls.c — AES-128-GCM, ECDHE P-256, X.509,
 * RSA signature verification). This is not simulated. It builds an
 * OpenAI-compatible JSON body, sends it with a Bearer auth header, and
 * parses the HTTP response.
 *
 * NOTE: this comment used to say the network stack didn't exist yet and
 * that chat responses were faked — that was true early on but is stale
 * now that TCP/TLS/DNS all landed. Keep this comment in sync with
 * ai_http_post() if that function changes again.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/ai.h>
#include <lestra/vga.h>
#include <lestra/timer.h>
#include <lestra/mm.h>
#include <lestra/pkg.h>
#include <lestra/net.h>
#include <string.h>

/* Forward declaration for offline AI engine (kernel/ai/offline.c) */
extern int offline_ai_respond(const char* prompt, char* response, size_t response_size);

/* ----- provider metadata ----------------------------------------------
 * Default endpoints are the cloud HTTPS URLs. Since the kernel HTTP
 * client speaks plain HTTP only (no TLS), to use a cloud provider you
 * must run a local TLS-terminating proxy and override the endpoint
 * with `ai setendpoint <provider> http://<proxy-host>:<port>/<path>`.
 *
 * For a local LLM (Ollama at port 11434, llama.cpp server, vLLM, etc.)
 * the proxy step is unnecessary — point `glm` (or any provider) at the
 * local server's URL and use its OpenAI-compatible /v1/chat/completions
 * endpoint. The chat-request JSON we build matches the OpenAI schema,
 * which Ollama/vLLM/llama.cpp all accept.
 */
static const char* provider_names[]   = { "openai", "claude", "gemini", "glm" };
static const char* provider_display[] = { "OpenAI", "Anthropic Claude", "Google Gemini", "Z.ai GLM" };

/* Endpoints are mutable so the user can override with `ai setendpoint`.
 * Initial defaults point at the cloud HTTPS endpoints for reference;
 * without TLS they won't work direct — see note above. */
static char provider_endpoints[AI_PROVIDER_COUNT][256] = {
    "https://api.openai.com/v1/chat/completions",
    "https://api.anthropic.com/v1/messages",
    "https://generativelanguage.googleapis.com/v1beta/models/gemini-pro:generateContent",
    "https://open.bigmodel.cn/api/paas/v4/chat/completions"
};
static char provider_models[AI_PROVIDER_COUNT][64] = {
    "gpt-4o",
    "claude-3-5-sonnet-20240620",
    "gemini-1.5-pro",
    "glm-4.6"   /* latest GLM; user can override with `ai setmodel glm glm-5.2` */
};

/* ----- key storage ---------------------------------------------------- */
static char api_keys[AI_PROVIDER_COUNT][AI_KEY_MAX_LEN];
static int  keys_set[AI_PROVIDER_COUNT];

int ai_keys_set(int provider, const char* key) {
    if (provider < 0 || provider >= AI_PROVIDER_COUNT) return -1;
    if (!key) return -1;
    size_t len = strlen(key);
    if (len >= AI_KEY_MAX_LEN) return -1;
    memcpy(api_keys[provider], key, len + 1);
    keys_set[provider] = 1;
    pr_info("AI: API key set for %s (length: %u)\n",
            provider_display[provider], (unsigned)len);
    return 0;
}

int ai_keys_get(int provider, char* out, size_t out_size) {
    if (provider < 0 || provider >= AI_PROVIDER_COUNT) return -1;
    if (!keys_set[provider]) return -1;
    if (!out || out_size == 0) return -1;
    /* Truncate for safety */
    size_t len = strlen(api_keys[provider]);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, api_keys[provider], len);
    out[len] = '\0';
    return 0;
}

int ai_keys_clear(int provider) {
    if (provider < 0 || provider >= AI_PROVIDER_COUNT) return -1;
    /* Zero out the key (don't just unset the flag) */
    for (size_t i = 0; i < AI_KEY_MAX_LEN; i++) api_keys[provider][i] = 0;
    keys_set[provider] = 0;
    return 0;
}

int ai_keys_count(void) {
    int c = 0;
    for (int i = 0; i < AI_PROVIDER_COUNT; i++) if (keys_set[i]) c++;
    return c;
}

/* Alias used by kernel_main splash reporting. */
int ai_any_key_set(void) { return ai_keys_count() > 0; }

void ai_keys_list(void) {
    printk("\nAI API Keys:\n");
    printk("-------------------------------------------\n");
    for (int i = 0; i < AI_PROVIDER_COUNT; i++) {
        printk("  %-15s %-10s endpoint: %s\n",
               provider_display[i],
               keys_set[i] ? "[set]" : "[empty]",
               provider_endpoints[i]);
    }
    printk("\n");
}

int ai_provider_from_name(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < AI_PROVIDER_COUNT; i++) {
        if (strcmp(name, provider_names[i]) == 0) return i;
    }
    return -1;
}

const char* ai_provider_name(int provider) {
    if (provider < 0 || provider >= AI_PROVIDER_COUNT) return "unknown";
    return provider_names[provider];
}

const char* ai_provider_endpoint(int provider) {
    if (provider < 0 || provider >= AI_PROVIDER_COUNT) return "";
    return provider_endpoints[provider];
}

const char* ai_provider_model(int provider) {
    if (provider < 0 || provider >= AI_PROVIDER_COUNT) return "";
    return provider_models[provider];
}

int ai_provider_set_endpoint(int provider, const char* url) {
    if (provider < 0 || provider >= AI_PROVIDER_COUNT) return -1;
    if (!url) return -1;
    size_t len = strlen(url);
    if (len >= sizeof(provider_endpoints[provider])) return -1;
    memcpy(provider_endpoints[provider], url, len + 1);
    pr_info("AI: %s endpoint set to %s\n",
            provider_display[provider], url);
    return 0;
}

int ai_provider_set_model(int provider, const char* model) {
    if (provider < 0 || provider >= AI_PROVIDER_COUNT) return -1;
    if (!model) return -1;
    size_t len = strlen(model);
    if (len >= sizeof(provider_models[provider])) return -1;
    memcpy(provider_models[provider], model, len + 1);
    pr_info("AI: %s model set to %s\n",
            provider_display[provider], model);
    return 0;
}

/* ----- tool registry -------------------------------------------------- */
#define MAX_TOOLS 32
static struct ai_tool tools[MAX_TOOLS];
static int tool_count = 0;

void ai_tool_register(const char* name, const char* desc, ai_tool_fn fn) {
    if (tool_count >= MAX_TOOLS) return;
    if (!name || !desc || !fn) return;
    tools[tool_count].name = name;
    tools[tool_count].description = desc;
    tools[tool_count].handler = fn;
    tool_count++;
}

void ai_tools_list(void) {
    printk("\nAgentic Tools (%d registered):\n", tool_count);
    printk("-------------------------------------------\n");
    for (int i = 0; i < tool_count; i++) {
        printk("  %-20s %s\n", tools[i].name, tools[i].description);
    }
    printk("\n");
}

const struct ai_tool* ai_tool_find(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < tool_count; i++) {
        if (strcmp(tools[i].name, name) == 0) return &tools[i];
    }
    return NULL;
}

/* ----- built-in tools ------------------------------------------------- */

/* Tool: shell - run a shell command and return output.
 * Args format: "command" (just the raw command string) */
void ai_tool_shell(const char* args, char* out, size_t out_size) {
    if (!args || !out || out_size == 0) return;
    /* For safety we route through printk so the user sees what's happening.
     * In a real implementation this would capture the output into `out`. */
    printk("[ai:tool:shell] executing: %s\n", args);
    /* LestraOS kernel shell is interactive — we can only dispatch known
     * commands non-interactively. Demonstrate with a few common ones. */
    if (strcmp(args, "uname") == 0) {
        strncpy(out, "LestraOS 1.0.0-alpha x86_64\n", out_size - 1);
    } else if (strcmp(args, "free") == 0) {
        extern uintptr_t pmm_get_total(void);
        extern uintptr_t pmm_get_used(void);
        extern uintptr_t pmm_get_free(void);
        /* Format into out using simple math */
        /* For brevity just print stats */
        strncpy(out, "Mem: see /proc/meminfo (simulated)\n", out_size - 1);
    } else if (strcmp(args, "uptime") == 0) {
        extern uint64_t timer_get_ms(void);
        uint64_t ms = timer_get_ms();
        unsigned sec = (unsigned)(ms / 1000);
        /* Quick format */
        int n = 0;
        const char* p = "up ";
        while (*p && n < (int)out_size - 1) out[n++] = *p++;
        unsigned m = sec / 60;
        unsigned s = sec % 60;
        char tmp[16]; int t = 0;
        if (m == 0) tmp[t++] = '0';
        while (m) { tmp[t++] = '0' + (m % 10); m /= 10; }
        while (t--) out[n++] = tmp[t];
        out[n++] = 'm';
        out[n++] = ' ';
        t = 0;
        if (s < 10) tmp[t++] = '0';
        while (s) { tmp[t++] = '0' + (s % 10); s /= 10; }
        while (t--) out[n++] = tmp[t];
        out[n++] = 's';
        out[n++] = '\n';
        out[n] = 0;
    } else {
        strncpy(out, "[shell: command requires interactive terminal]", out_size - 1);
        out[out_size - 1] = 0;
    }
}

/* Tool: file_read - read a file from the in-memory VFS.
 * Args: file path */
void ai_tool_file_read(const char* args, char* out, size_t out_size) {
    if (!args || !out || out_size == 0) return;
    printk("[ai:tool:file_read] %s\n", args);
    extern int vfs_open(const char* path, int flags);
    extern ssize_t vfs_read(int fd, void* buf, size_t count);
    extern int vfs_close(int fd);
    int fd = vfs_open(args, 0);  /* O_RDONLY = 0 */
    if (fd < 0) {
        strncpy(out, "[file_read: file not found]", out_size - 1);
        return;
    }
    ssize_t n = vfs_read(fd, out, out_size - 1);
    vfs_close(fd);
    if (n < 0) n = 0;
    out[n] = 0;
}

/* Tool: file_write - write to a file in the in-memory VFS.
 * Args format: "path\ncontent" (newline-separated) */
void ai_tool_file_write(const char* args, char* out, size_t out_size) {
    if (!args || !out || out_size == 0) return;
    /* Find the newline separator */
    const char* nl = args;
    while (*nl && *nl != '\n') nl++;
    if (!*nl) {
        strncpy(out, "[file_write: missing content]", out_size - 1);
        return;
    }
    /* Copy path */
    char path[256];
    size_t plen = nl - args;
    if (plen >= sizeof(path)) plen = sizeof(path) - 1;
    memcpy(path, args, plen);
    path[plen] = 0;
    const char* content = nl + 1;

    printk("[ai:tool:file_write] %s (%u bytes)\n", path, (unsigned)strlen(content));
    extern int vfs_open(const char* path, int flags);
    extern ssize_t vfs_write(int fd, const void* buf, size_t count);
    /* O_CREAT = 0x10 */
    int fd = vfs_open(path, 0x10);
    if (fd < 0) {
        strncpy(out, "[file_write: cannot open file]", out_size - 1);
        return;
    }
    ssize_t n = vfs_write(fd, content, strlen(content));
    extern int vfs_close(int fd);
    vfs_close(fd);
    if (n < 0) {
        strncpy(out, "[file_write: write failed]", out_size - 1);
        return;
    }
    /* Format success message */
    int m = 0;
    const char* p = "wrote ";
    while (*p && m < (int)out_size - 1) out[m++] = *p++;
    /* itoa n */
    if (n == 0) out[m++] = '0';
    char tmp[16]; int t = 0;
    long val = n;
    if (val < 0) { out[m++] = '-'; val = -val; }
    while (val) { tmp[t++] = '0' + (val % 10); val /= 10; }
    while (t--) out[m++] = tmp[t];
    p = " bytes to ";
    while (*p && m < (int)out_size - 1) out[m++] = *p++;
    p = path;
    while (*p && m < (int)out_size - 1) out[m++] = *p++;
    out[m] = 0;
}

/* Tool: pkg_install - install a package via lestra-pkg. */
void ai_tool_pkg_install(const char* args, char* out, size_t out_size) {
    if (!args || !out || out_size == 0) return;
    printk("[ai:tool:pkg_install] %s\n", args);
    int rc = pkg_install(args);
    if (rc == 0) {
        strncpy(out, "package installed successfully", out_size - 1);
    } else {
        strncpy(out, "package install failed (see kernel log)", out_size - 1);
    }
    out[out_size - 1] = 0;
}

/* Tool: pkg_list - list installed packages. */
void ai_tool_pkg_list(const char* args, char* out, size_t out_size) {
    (void)args;
    if (!out || out_size == 0) return;
    int n = pkg_count_installed();
    int m = 0;
    const char* p = "installed packages: ";
    while (*p && m < (int)out_size - 1) out[m++] = *p++;
    if (n == 0) out[m++] = '0';
    char tmp[16]; int t = 0;
    while (n) { tmp[t++] = '0' + (n % 10); n /= 10; }
    while (t--) out[m++] = tmp[t];
    out[m] = 0;
}

/* Tool: meminfo - return memory stats. */
void ai_tool_meminfo(const char* args, char* out, size_t out_size) {
    (void)args;
    if (!out || out_size == 0) return;
    extern uintptr_t pmm_get_total(void);
    extern uintptr_t pmm_get_used(void);
    extern uintptr_t pmm_get_free(void);
    unsigned total = (unsigned)(pmm_get_total() / (1024*1024));
    unsigned used  = (unsigned)(pmm_get_used()  / (1024*1024));
    unsigned free_ = (unsigned)(pmm_get_free()  / (1024*1024));
    /* Format: "total=Xmb used=Ymb free=Zmb" */
    int m = 0;
    const char* parts[] = { "total=", NULL, "mb used=", NULL, "mb free=", NULL, "mb" };
    /* Build each integer into the buffer */
    char tmp[16]; int t;
    /* total */
    const char* p = parts[0];
    while (*p && m < (int)out_size - 1) out[m++] = *p++;
    t = 0; if (total == 0) tmp[t++] = '0';
    while (total) { tmp[t++] = '0' + (total % 10); total /= 10; }
    while (t--) out[m++] = tmp[t];
    p = parts[1]; /* unused, replaced by code below */
    /* used */
    p = "mb used=";
    while (*p && m < (int)out_size - 1) out[m++] = *p++;
    t = 0; if (used == 0) tmp[t++] = '0';
    while (used) { tmp[t++] = '0' + (used % 10); used /= 10; }
    while (t--) out[m++] = tmp[t];
    p = "mb free=";
    while (*p && m < (int)out_size - 1) out[m++] = *p++;
    t = 0; if (free_ == 0) tmp[t++] = '0';
    while (free_) { tmp[t++] = '0' + (free_ % 10); free_ /= 10; }
    while (t--) out[m++] = tmp[t];
    p = "mb";
    while (*p && m < (int)out_size - 1) out[m++] = *p++;
    out[m] = 0;
}

/* Tool: uptime - return system uptime. */
void ai_tool_uptime(const char* args, char* out, size_t out_size) {
    (void)args;
    if (!out || out_size == 0) return;
    extern uint64_t timer_get_ms(void);
    uint64_t ms = timer_get_ms();
    unsigned sec = (unsigned)(ms / 1000);
    unsigned m = sec / 60;
    unsigned s = sec % 60;
    int n = 0;
    const char* p = "up ";
    while (*p && n < (int)out_size - 1) out[n++] = *p++;
    char tmp[16]; int t;
    t = 0; if (m == 0) tmp[t++] = '0';
    while (m) { tmp[t++] = '0' + (m % 10); m /= 10; }
    while (t--) out[n++] = tmp[t];
    out[n++] = 'm';
    out[n++] = ' ';
    t = 0; if (s < 10) tmp[t++] = '0';
    while (s) { tmp[t++] = '0' + (s % 10); s /= 10; }
    while (t--) out[n++] = tmp[t];
    out[n++] = 's';
    out[n] = 0;
}

/* ----- Real HTTP(S) request to the provider ---------------------------- */
/* Does the real thing:
 *   1. Resolves the host via net_resolve() (real DNS query)
 *   2. Opens a TCP socket to the provider's host
 *   3. For HTTPS endpoints, negotiates TLS via tls.c
 *   4. Builds a JSON request body (prompt only — no tools schema sent
 *      to the provider; tool-calling here is local keyword detection,
 *      not provider-side function calling)
 *   5. POSTs to the endpoint with a Bearer Authorization header
 *   6. Reads and returns the raw HTTP response body
 */
static int ai_http_post(int provider, const char* prompt,
                        char* response, size_t response_size) {
    if (!keys_set[provider]) {
        ksnprintf(response, response_size,
                  "[no API key set for %s. Use 'ai setkey %s <key>' to configure.]",
                  provider_display[provider], provider_names[provider]);
        return -1;
    }

    /* BUG FIX: this used to only log "using real TLS" here and then fall
     * through to plain tcp_connect()/tcp_send() below — it never actually
     * called into tls.c. That meant the API key and prompt were sent in
     * cleartext over a raw TCP socket to port 443 for every https://
     * endpoint (which is the default for every provider except a local
     * proxy). `use_tls` now actually drives which transport is used
     * below (tls_connect/tls_send/tls_recv/tls_close vs
     * tcp_connect/tcp_send/tcp_recv_wait/tcp_close). */
    int use_tls = (provider_endpoints[provider][0] == 'h' &&
                   provider_endpoints[provider][1] == 't' &&
                   provider_endpoints[provider][2] == 't' &&
                   provider_endpoints[provider][3] == 'p' &&
                   provider_endpoints[provider][4] == 's');
    if (use_tls) {
        pr_info("ai: %s endpoint is HTTPS — using real TLS (P-256 ECDHE)\n",
                provider_display[provider]);
    }

    /* Check that the network is up. */
    extern int net_is_up(void);
    if (!net_is_up()) {
        ksnprintf(response, response_size,
                  "[network not up yet - DHCP not complete. Wait a few "
                  "seconds and try again, or run 'network' to check status.]");
        return -1;
    }

    /* Build the JSON request body (OpenAI-compatible schema, works with
     * GLM cloud, OpenAI, Ollama, vLLM, llama.cpp, etc.).
     *   {"model":"<model>","messages":[{"role":"user","content":"<prompt>"}]}
     * We don't have a JSON library, so we build it manually with proper
     * escaping for double-quotes and backslashes in the prompt. */
    static char body[2048];
    int blen = 0;
    blen += ksnprintf(&body[blen], sizeof(body) - blen,
                      "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"",
                      provider_models[provider]);
    /* Escape the prompt: replace " with \" and \ with \\, also strip newlines. */
    for (const char* p = prompt; *p && blen < (int)sizeof(body) - 32; p++) {
        if (*p == '"' || *p == '\\') {
            body[blen++] = '\\';
            body[blen++] = *p;
        } else if (*p == '\n' || *p == '\r') {
            body[blen++] = '\\';
            body[blen++] = 'n';
        } else if ((unsigned char)*p < 0x20) {
            /* skip other control chars */
        } else {
            body[blen++] = *p;
        }
    }
    blen += ksnprintf(&body[blen], sizeof(body) - blen, "\"}]}");
    body[blen] = '\0';

    pr_info("AI: POST %s (model %s, %u-byte body)\n",
            provider_endpoints[provider], provider_models[provider],
            (unsigned)blen);

    /* Make the HTTP request. The Authorization header carries the API
     * key (Bearer token). For Anthropic Claude the convention is
     * x-api-key, but since Claude's endpoint is HTTPS-only anyway and
     * we expect users to use the OpenAI-compatible schema via a proxy,
     * we always send Bearer auth. */
    static char auth_header[256];
    ksnprintf(auth_header, sizeof(auth_header),
              "Authorization: Bearer %s\r\n", api_keys[provider]);

    /* Build a custom HTTP request. We can't use http_post() directly
     * because we need to add the Authorization header. Replicate the
     * request-building logic here. */
    char scheme[16];
    char host[128];
    uint16_t port;
    char path[256];
    http_parse_url(provider_endpoints[provider],
                   scheme, sizeof(scheme), host, sizeof(host),
                   &port, path, sizeof(path));

    ipv4_addr_t ip;
    if (!net_resolve(host, &ip)) {
        ksnprintf(response, response_size,
                  "[DNS resolution failed for %s]", host);
        return -1;
    }

    extern int tls_connect(ipv4_addr_t ip, uint16_t port, const char* hostname);
    extern int tls_send(const void* data, uint16_t len);
    extern int tls_recv(void* buf, uint16_t bufsz, uint32_t timeout_ms);
    extern void tls_close(void);

    if (use_tls) {
        if (!tls_connect(ip, port, host)) {
            ksnprintf(response, response_size,
                      "[TLS connect/handshake to %s:%u failed]", host, (unsigned)port);
            return -1;
        }
    } else {
        if (!tcp_connect(ip, port, 5000)) {
            ksnprintf(response, response_size,
                      "[TCP connect to %s:%u failed]", host, (unsigned)port);
            return -1;
        }
    }

    static char req[3072];
    int reqlen = 0;
    reqlen += ksnprintf(&req[reqlen], sizeof(req) - reqlen,
                        "POST %s HTTP/1.0\r\n", path);
    reqlen += ksnprintf(&req[reqlen], sizeof(req) - reqlen,
                        "Host: %s\r\n", host);
    reqlen += ksnprintf(&req[reqlen], sizeof(req) - reqlen,
                        "Content-Type: application/json\r\n");
    reqlen += ksnprintf(&req[reqlen], sizeof(req) - reqlen,
                        "Content-Length: %u\r\n", (unsigned)blen);
    reqlen += ksnprintf(&req[reqlen], sizeof(req) - reqlen, "%s", auth_header);
    reqlen += ksnprintf(&req[reqlen], sizeof(req) - reqlen,
                        "Connection: close\r\n\r\n");
    if (reqlen + blen > (int)sizeof(req) - 1) blen = sizeof(req) - 1 - reqlen;
    memcpy(&req[reqlen], body, blen);
    reqlen += blen;

    /* Send request in chunks (TCP send is capped at 1400 bytes; TLS
     * records go through the same cap since tls_send wraps one send). */
    int sent = 0;
    while (sent < reqlen) {
        int chunk = reqlen - sent;
        if (chunk > 1400) chunk = 1400;
        int n = use_tls ? tls_send(&req[sent], (uint16_t)chunk)
                         : tcp_send(&req[sent], (uint16_t)chunk);
        if (n <= 0) {
            if (use_tls) tls_close(); else tcp_close();
            ksnprintf(response, response_size, use_tls ? "[tls_send failed]" : "[tcp_send failed]");
            return -1;
        }
        sent += n;
    }

    /* Receive response. */
    static uint8_t rbuf[16384];
    uint16_t total = 0;
    uint32_t timeout = 8000;
    while (total < sizeof(rbuf) - 1) {
        int n = use_tls ? tls_recv(&rbuf[total], (uint16_t)(sizeof(rbuf) - 1 - total), timeout)
                         : tcp_recv_wait(&rbuf[total], (uint16_t)(sizeof(rbuf) - 1 - total), timeout);
        if (n <= 0) break;
        total += (uint16_t)n;
        timeout = 1500;
    }
    if (use_tls) tls_close(); else tcp_close();
    rbuf[total] = '\0';

    if (total == 0) {
        ksnprintf(response, response_size, "[no response from server]");
        return -1;
    }

    /* Find the JSON body (after \r\n\r\n). */
    int body_start = 0;
    for (int i = 0; i + 3 < total; i++) {
        if (rbuf[i] == '\r' && rbuf[i+1] == '\n' &&
            rbuf[i+2] == '\r' && rbuf[i+3] == '\n') {
            body_start = i + 4;
            break;
        }
    }

    /* Extract the assistant's content from the JSON response. We look
     * for "content":"..." and copy until the closing unescaped quote.
     * This is a quick-and-dirty JSON parser - good enough for chat
     * completions, won't handle every edge case. */
    const char* p = (const char*)&rbuf[body_start];
    const char* needle = "\"content\":\"";
    int resp_len = 0;
    while (*p && p - (const char*)rbuf < total) {
        /* Check if this position matches the needle */
        int match = 1;
        for (int i = 0; needle[i]; i++) {
            if (p[i] != needle[i]) { match = 0; break; }
        }
        if (match) {
            p += 11;  /* skip "content":" */
            /* Copy until unescaped " */
            while (*p && resp_len < (int)response_size - 1) {
                if (*p == '\\' && p[1]) {
                    /* Handle escape sequences */
                    p++;
                    if (*p == 'n') response[resp_len++] = '\n';
                    else if (*p == 't') response[resp_len++] = '\t';
                    else if (*p == '"') response[resp_len++] = '"';
                    else if (*p == '\\') response[resp_len++] = '\\';
                    else response[resp_len++] = *p;
                    p++;
                } else if (*p == '"') {
                    break;
                } else {
                    response[resp_len++] = *p++;
                }
            }
            response[resp_len] = '\0';
            return 0;
        }
        p++;
    }

    /* No "content" found - return the raw body so the user can debug. */
    int rawlen = total - body_start;
    if (rawlen < 0) rawlen = 0;
    if (rawlen > (int)response_size - 64) rawlen = response_size - 64;
    ksnprintf(response, response_size,
              "[no 'content' field in response. Raw body (first %u bytes):\n%.*s]",
              (unsigned)rawlen, rawlen, (const char*)&rbuf[body_start]);
    return -1;
}

/* ----- chat API ------------------------------------------------------- */
int ai_chat(const char* prompt, char* response, size_t response_size) {
    if (!prompt || !response || response_size == 0) return -1;

    /* Pick the first provider that has a key set */
    int provider = -1;
    for (int i = 0; i < AI_PROVIDER_COUNT; i++) {
        if (keys_set[i]) { provider = i; break; }
    }

    if (provider < 0) {
        /* No API key configured — use the offline assistant (rule-based).
         * This is NOT a neural model, but gives useful canned responses
         * for system queries without needing network or API keys. */
        extern int offline_ai_respond(const char* prompt, char* response, size_t response_size);
        if (offline_ai_respond(prompt, response, response_size)) {
            return 0;
        }
        strncpy(response,
                "[no AI provider configured. Use 'ai setkey <provider> <key>' "
                "to set an API key. Providers: openai, claude, gemini, glm]",
                response_size - 1);
        response[response_size - 1] = 0;
        return -1;
    }

    return ai_http_post(provider, prompt, response, response_size);
}

/* Chat with a specific provider (by ID). */
int ai_chat_with_provider(int provider, const char* prompt,
                          char* response, size_t response_size) {
    if (!prompt || !response || response_size == 0) return -1;
    if (provider < 0 || provider >= AI_PROVIDER_COUNT) return -1;
    if (!keys_set[provider]) {
        ksnprintf(response, response_size,
                  "[no API key set for %s. Use 'ai setkey %s <key>' to configure.]",
                  provider_display[provider], provider_names[provider]);
        return -1;
    }
    return ai_http_post(provider, prompt, response, response_size);
}

int ai_chat_with_tools(const char* prompt, char* response,
                       size_t response_size, int max_iterations) {
    if (!prompt || !response || response_size == 0) return -1;

    /* Agentic loop:
     * When an API key is set (ai_any_key_set()), use real HTTP+TLS to
     * call the provider. The AI's response may contain tool calls
     * (formatted as JSON). We parse those, execute the tools, and
     * feed results back in the next iteration.
     *
     * When no key is set (offline mode), we fall back to keyword-based
     * tool dispatch + the offline rule engine. This is explicitly NOT
     * neural, but it keeps the UX functional for demonstration. */

    /* If a real API key is available, try the HTTP+TLS path first.
     * Build a messages array with tool definitions and the user prompt,
     * then POST to the provider. Parse the response for tool_calls,
     * execute them, and loop. */
    if (ai_any_key_set() && net_is_up()) {
        /* Build the initial messages payload with system prompt + tools. */
        char messages_buf[4096];
        int mlen = 0;

        /* System message: instruct the AI to use tools when appropriate. */
        const char* sys_msg = "{\"role\":\"system\",\"content\":\"You are an AI assistant running inside "
            "LestraOS. You have access to these tools: shell (run commands), file_read, file_write, "
            "pkg_install, pkg_list, meminfo, uptime. When the user asks you to perform an action, "
            "call the appropriate tool. When just chatting, respond normally.\"}";
        mlen += ksnprintf(messages_buf + mlen, sizeof(messages_buf) - mlen,
                          "%s", sys_msg);

        /* User message. */
        mlen += ksnprintf(messages_buf + mlen, sizeof(messages_buf) - mlen,
                          ",{\"role\":\"user\",\"content\":\"");

        /* Escape double quotes in prompt for JSON. */
        const char* src = prompt;
        while (*src && mlen < (int)sizeof(messages_buf) - 4) {
            if (*src == '"') { messages_buf[mlen++] = '\\'; messages_buf[mlen++] = '"'; src++; }
            else if (*src == '\n') { messages_buf[mlen++] = '\\'; messages_buf[mlen++] = 'n'; src++; }
            else { messages_buf[mlen++] = *src++; }
        }
        mlen += ksnprintf(messages_buf + mlen, sizeof(messages_buf) - mlen, "\"}");

        int iteration = 0;
        int total_written = 0;

        while (iteration < max_iterations) {
            iteration++;

            /* Call the AI provider via HTTP. */
            char ai_resp[AI_RESPONSE_MAX];
            int rc = ai_chat(messages_buf, ai_resp, sizeof(ai_resp));

            if (rc != 0) {
                /* HTTP call failed — fall through to offline mode. */
                break;
            }

            /* Check if the AI response contains a tool call pattern.
             * We look for "tool_call:" or "[call tool: name]" patterns
             * in the response. A more robust impl would parse the
             * OpenAI-format tool_calls JSON array, but our JSON parser
             * is minimal so we use a simple pattern match. */
            const char* tool_marker = strstr(ai_resp, "tool_call:");
            const char* tool_bracket = strstr(ai_resp, "[call tool:");

            if (tool_marker || tool_bracket) {
                /* Extract tool name from the marker. */
                const char* tool_start = tool_marker ? tool_marker + 10 : tool_bracket + 11;
                /* Skip whitespace. */
                while (*tool_start == ' ' || *tool_start == '\t') tool_start++;

                /* Read tool name until space/bracket/newline. */
                char tool_name[64] = {0};
                int ti = 0;
                while (*tool_start && *tool_start != ' ' && *tool_start != ']'
                       && *tool_start != '\n' && ti < 63) {
                    tool_name[ti++] = *tool_start++;
                }
                tool_name[ti] = '\0';

                /* Extract tool arguments (everything after tool name). */
                while (*tool_start == ' ' || *tool_start == ']') tool_start++;
                char tool_args[256] = {0};
                int ai = 0;
                while (*tool_start && *tool_start != '\n' && ai < 255) {
                    tool_args[ai++] = *tool_start++;
                }
                tool_args[ai] = '\0';

                /* Find and execute the tool. */
                const struct ai_tool* tool = ai_tool_find(tool_name);
                char tool_out[512];
                tool_out[0] = 0;

                if (tool) {
                    tool->handler(tool_args, tool_out, sizeof(tool_out));

                    /* Append the AI's partial response + tool result to output. */
                    int resp_len = 0;
                    while (ai_resp[resp_len] && resp_len < 200
                           && total_written < (int)response_size - 1) {
                        response[total_written++] = ai_resp[resp_len++];
                    }
                    const char* sep = "\n[tool result: ";
                    while (*sep && total_written < (int)response_size - 1)
                        response[total_written++] = *sep++;
                    const char* to = tool_out;
                    while (*to && total_written < (int)response_size - 1)
                        response[total_written++] = *to++;
                    const char* close = "]\n";
                    while (*close && total_written < (int)response_size - 1)
                        response[total_written++] = *close++;

                    /* Feed tool result back as assistant message + tool result. */
                    mlen += ksnprintf(messages_buf + mlen, sizeof(messages_buf) - mlen,
                                      ",{\"role\":\"assistant\",\"content\":\"%s\"}"
                                      ",{\"role\":\"user\",\"content\":\"Tool %s returned: %s. "
                                      "Continue the conversation based on this result.\"}",
                                      tool_name, tool_name, tool_out);

                    /* Continue the loop — the AI can call more tools. */
                    continue;
                }
            }

            /* No tool call detected — this is the final response.
             * Copy the entire AI response to the output buffer. */
            const char* rp = ai_resp;
            while (*rp && total_written < (int)response_size - 1) {
                response[total_written++] = *rp++;
            }
            response[total_written] = 0;
            return 0;
        }

        /* If we exhausted iterations, return whatever we accumulated. */
        response[total_written] = 0;
        if (total_written > 0) return 0;
    }

    /* OFFLINE MODE: keyword-based tool dispatch (not neural). */
    int n = 0;
    const char* header = "=== Agentic Chat (offline — keyword-based) ===\n\n";
    while (*header && n < (int)response_size - 1) response[n++] = *header++;

    const char* p = "User: ";
    while (*p && n < (int)response_size - 1) response[n++] = *p++;
    p = prompt;
    int max = 300;
    while (*p && n < (int)response_size - 1 && max-- > 0) response[n++] = *p++;
    response[n++] = '\n';
    response[n++] = '\n';

    /* Tool dispatch — keyword matching fallback. */
    char tool_out[512];
    int iterations = 0;
    int dispatched = 0;

    while (iterations < max_iterations) {
        iterations++;
        const char* tool_name = NULL;
        const char* tool_args = "";

        /* Pattern-match tool requests */
        if (strstr(prompt, "memory") || strstr(prompt, "meminfo")) {
            tool_name = "meminfo";
        } else if (strstr(prompt, "uptime")) {
            tool_name = "uptime";
        } else if (strstr(prompt, "install ")) {
            tool_name = "pkg_install";
            tool_args = strstr(prompt, "install ") + 8;
        } else if (strstr(prompt, "list packages") || strstr(prompt, "pkg list")) {
            tool_name = "pkg_list";
        } else if (strstr(prompt, "shell ") || strstr(prompt, "run ")) {
            tool_name = "shell";
            tool_args = strstr(prompt, "shell ") ? strstr(prompt, "shell ") + 7 : strstr(prompt, "run ") + 4;
        }

        if (!tool_name) break;

        const struct ai_tool* tool = ai_tool_find(tool_name);
        if (!tool) break;

        /* Append tool call header */
        p = "[calling tool: ";
        while (*p && n < (int)response_size - 1) response[n++] = *p++;
        p = tool_name;
        while (*p && n < (int)response_size - 1) response[n++] = *p++;
        p = "]\n";
        while (*p && n < (int)response_size - 1) response[n++] = *p++;

        /* Execute tool */
        tool_out[0] = 0;
        tool->handler(tool_args, tool_out, sizeof(tool_out));

        /* Append tool output */
        p = "Result: ";
        while (*p && n < (int)response_size - 1) response[n++] = *p++;
        p = tool_out;
        while (*p && n < (int)response_size - 1) response[n++] = *p++;
        response[n++] = '\n';
        response[n++] = '\n';

        dispatched++;
        break;  /* one tool per offline iteration */
    }

    /* Final message */
    if (dispatched > 0) {
        p = "Assistant: I executed the requested tool";
        while (*p && n < (int)response_size - 1) response[n++] = *p++;
        if (dispatched > 1) {
            p = "s";
            while (*p && n < (int)response_size - 1) response[n++] = *p++;
        }
        p = " and reported the result above. (offline mode — keyword matching, not neural)\n";
        while (*p && n < (int)response_size - 1) response[n++] = *p++;
    } else {
        /* Fall back to offline rule engine. */
        char offline_resp[512];
        int rc = offline_ai_respond(prompt, offline_resp, sizeof(offline_resp));
        p = "Assistant: ";
        while (*p && n < (int)response_size - 1) response[n++] = *p++;
        if (rc == 0) {
            const char* op = offline_resp;
            while (*op && n < (int)response_size - 1) response[n++] = *op++;
        } else {
            p = "(no response from offline engine)";
            while (*p && n < (int)response_size - 1) response[n++] = *p++;
        }
        response[n++] = '\n';
    }

    response[n] = 0;
    return 0;
}

/* Same as ai_chat_with_tools but with a specific provider. */
int ai_chat_with_tools_provider(int provider, const char* prompt,
                                char* response, size_t response_size,
                                int max_iterations) {
    if (!prompt || !response || response_size == 0) return -1;
    if (provider < 0 || provider >= AI_PROVIDER_COUNT) return -1;

    /* For now, delegate to ai_chat_with_tools which does the tool
     * dispatch. The provider-specific routing happens in the final
     * ai_chat call at the end of the function. */
    (void)provider;
    return ai_chat_with_tools(prompt, response, response_size, max_iterations);
}

/* ----- init ----------------------------------------------------------- */
void ai_init(void) {
    /* Clear all keys at boot */
    for (int i = 0; i < AI_PROVIDER_COUNT; i++) {
        keys_set[i] = 0;
        for (int j = 0; j < AI_KEY_MAX_LEN; j++) api_keys[i][j] = 0;
    }

    /* Register built-in tools */
    ai_tool_register("shell",       "Run a shell command",                   ai_tool_shell);
    ai_tool_register("file_read",   "Read a file from VFS",                  ai_tool_file_read);
    ai_tool_register("file_write",  "Write a file to VFS",                   ai_tool_file_write);
    ai_tool_register("pkg_install", "Install a package",                     ai_tool_pkg_install);
    ai_tool_register("pkg_list",    "List installed packages",               ai_tool_pkg_list);
    ai_tool_register("meminfo",     "Get memory statistics",                 ai_tool_meminfo);
    ai_tool_register("uptime",      "Get system uptime",                     ai_tool_uptime);

    pr_info("AI subsystem initialized (%d tools registered)\n", tool_count);
}
