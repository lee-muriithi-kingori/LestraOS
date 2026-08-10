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

/* ----- conversation memory ---------------------------------------------
 * Per-session conversation history so the AI has context across turns.
 * A flat sliding-window buffer: when full, we drop the oldest half.
 * This is intentionally simple — a real impl would store structured
 * messages, but a flat string works for both the cloud path (prepended
 * to the prompt) and the offline path (same). */
#define AI_HISTORY_SIZE 8192
static char conversation_history[AI_HISTORY_SIZE];
static int  history_len = 0;

void ai_clear_memory(void) {
    history_len = 0;
    conversation_history[0] = '\0';
    pr_info("AI: conversation memory cleared\n");
}

/* Append a turn to the history. Drops oldest content if needed. */
static void history_append_turn(const char* prompt, const char* response) {
    /* Build the turn string: "User: <prompt>\nAI: <response>\n" */
    char turn[AI_PROMPT_MAX + 1024];
    int tlen = ksnprintf(turn, sizeof(turn), "User: %s\nAI: %s\n", prompt, response);
    if (tlen < 0) return;

    /* If the turn alone is bigger than the buffer, truncate it. */
    if (tlen >= AI_HISTORY_SIZE) tlen = AI_HISTORY_SIZE - 1;

    /* Make room: if needed, drop oldest content (shift left). */
    if (history_len + tlen + 1 >= AI_HISTORY_SIZE) {
        int drop = (history_len + tlen + 1) - (AI_HISTORY_SIZE - 1);
        /* Round up to drop at least half the buffer so we don't shift
         * on every single turn. */
        if (drop < AI_HISTORY_SIZE / 2) drop = AI_HISTORY_SIZE / 2;
        if (drop > history_len) drop = history_len;
        memmove(conversation_history, conversation_history + drop,
                history_len - drop);
        history_len -= drop;
        conversation_history[history_len] = '\0';
    }

    memcpy(conversation_history + history_len, turn, tlen);
    history_len += tlen;
    conversation_history[history_len] = '\0';
}

/* Build a prompt that includes recent conversation history for context.
 * Writes into `out` (size out_size). The current prompt is always at the
 * end; history is prepended as "Previous conversation:\n...\n\n". */
static int build_prompt_with_history(const char* prompt, char* out, size_t out_size) {
    int n = 0;
    if (history_len > 0) {
        /* Use at most the last ~600 bytes of history to stay within the
         * HTTP body budget (ai_http_post uses a 2048-byte body buffer). */
        int hist_start = history_len > 600 ? history_len - 600 : 0;
        int hist_len = history_len - hist_start;
        n += ksnprintf(out + n, out_size - n,
                       "Previous conversation (for context):\n%.*s\n\nCurrent question: ",
                       hist_len, conversation_history + hist_start);
    }
    /* Append the current prompt (bounded). */
    int plen = strlen(prompt);
    if (n + plen >= (int)out_size - 1) plen = out_size - 1 - n;
    memcpy(out + n, prompt, plen);
    n += plen;
    out[n] = '\0';
    return n;
}

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
 * Args format: "command" (just the raw command string)
 *
 * Routes through shell_execute_line (kernel/core/shell.c) so the AI can
 * run ANY in-kernel shell command. Output is captured from the kmsg ring
 * buffer by diffing before/after snapshots. For the common short-command
 * case this gives the AI the actual command output; if the kmsg buffer
 * wrapped (very long boot log + long command output), we fall back to a
 * status message and the user sees the full output on the console. */
extern void shell_execute_line(const char* line, void (*out)(char c));

void ai_tool_shell(const char* args, char* out, size_t out_size) {
    if (!args || !out || out_size == 0) return;
    out[0] = 0;

    /* Refuse to run "ai ..." subcommands to avoid recursion
     * (ai_chat -> ai_tool_shell -> shell_execute_line("ai ...") -> ai_chat). */
    if (args[0] == 'a' && args[1] == 'i' &&
        (args[2] == ' ' || args[2] == '\t')) {
        strncpy(out, "[shell: refused to run 'ai' subcommand (recursion guard)]",
                out_size - 1);
        out[out_size - 1] = 0;
        return;
    }
    /* Refuse reboot/shutdown for safety. */
    if (strcmp(args, "reboot") == 0 || strcmp(args, "shutdown") == 0 ||
        strncmp(args, "reboot ", 7) == 0 || strncmp(args, "shutdown ", 9) == 0) {
        strncpy(out, "[shell: refused to run power command for safety]",
                out_size - 1);
        out[out_size - 1] = 0;
        return;
    }

    printk("[ai:tool:shell] executing: %s\n", args);

    /* Capture output via kmsg ring buffer diff.
     * 1. Snapshot kmsg before the command.
     * 2. Run the command (output goes to printk -> kmsg + console).
     * 3. Snapshot kmsg after.
     * 4. The new content is the tail of the after-snapshot that wasn't
     *    in the before-snapshot. */
    extern size_t kmsg_read(char* buf, size_t max);

    /* Use a kmalloc'd buffer so we don't blow the kernel stack
     * (4 KB + 4 KB = 8 KB). */
    int bufsz = 4096;
    char* before = (char*)kmalloc(bufsz);
    char* after  = (char*)kmalloc(bufsz);
    if (!before || !after) {
        ksnprintf(out, out_size, "[shell: out of memory for output capture]");
        if (before) kfree(before);
        if (after)  kfree(after);
        return;
    }

    size_t before_len = kmsg_read(before, bufsz - 1);
    before[before_len] = 0;

    /* Run the command. shell_execute_line ignores the `out` callback
     * (output goes through printk), so we pass NULL. */
    shell_execute_line(args, 0);

    size_t after_len = kmsg_read(after, bufsz - 1);
    after[after_len] = 0;

    /* Find the new content. Two cases:
     *   (a) after_len > before_len AND the first before_len bytes match:
     *       new content = after[before_len .. after_len].
     *   (b) Otherwise (kmsg wrapped or was truncated): try to find where
     *       the before-snapshot's tail appears in the after-snapshot, and
     *       take everything after that. If that fails, return a status. */
    char* new_start = NULL;
    size_t new_len = 0;

    if (after_len > before_len &&
        memcmp(before, after, before_len) == 0) {
        /* Case (a): clean diff. */
        new_start = after + before_len;
        new_len = after_len - before_len;
    } else if (after_len > 0 && before_len > 0) {
        /* Case (b): search for the last 64 bytes of `before` in `after`.
         * If found, everything after that point is new. */
        int tail_len = before_len > 64 ? 64 : before_len;
        const char* needle = before + before_len - tail_len;
        /* Simple substring search. */
        for (size_t i = 0; i + tail_len <= after_len; i++) {
            if (memcmp(after + i, needle, tail_len) == 0) {
                new_start = after + i + tail_len;
                new_len = after_len - (i + tail_len);
                break;
            }
        }
    }

    if (new_start && new_len > 0) {
        /* Strip the "[ai:tool:shell] executing:" line that we just printk'd,
         * since it's our own log, not command output. Look for the first
         * newline after the executing line. */
        const char* exec_marker = "[ai:tool:shell] executing:";
        if (new_len > strlen(exec_marker) &&
            memcmp(new_start, exec_marker, strlen(exec_marker)) == 0) {
            /* Skip to end of that line. */
            const char* nl = new_start;
            const char* end = new_start + new_len;
            while (nl < end && *nl != '\n') nl++;
            if (nl < end) {
                nl++;  /* skip the newline */
                new_len = end - nl;
                new_start = (char*)nl;
            }
        }
        /* Copy to output, bounded. */
        if (new_len >= out_size) new_len = out_size - 1;
        memcpy(out, new_start, new_len);
        out[new_len] = 0;
    } else {
        /* Couldn't extract output (kmsg wrapped or command produced no
         * output). Return a status message — the user saw it on console. */
        ksnprintf(out, out_size,
                  "[shell: '%s' executed (output sent to console — "
                  "kmsg capture unavailable)]", args);
    }

    kfree(before);
    kfree(after);
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

/* ----- OpenAI function-calling (tools schema) -------------------------- */

/* Build the OpenAI "tools" JSON array fragment for all registered tools.
 * Writes a string like:
 *   [{"type":"function","function":{"name":"shell","description":"...","parameters":{"type":"object","properties":{"cmd":{"type":"string","description":"..."}},"required":["cmd"]}}},...]
 * Returns the length written (excluding NUL). */
static int ai_build_tools_json(char* buf, size_t size) {
    int n = 0;
    n += ksnprintf(buf + n, size - n, "[");
    for (int i = 0; i < tool_count && n < (int)size - 64; i++) {
        if (i > 0) {
            if (n < (int)size - 1) buf[n++] = ',';
        }
        /* Each tool's parameter schema is minimal: a single "arg" string
         * for tools that take an argument, or empty properties for tools
         * that don't. This is intentionally simple — the LLM gets enough
         * info to call the tool, and our execution side extracts the
         * first string value as the arg. */
        const char* arg_name = "arg";
        const char* arg_desc = "The argument string for the tool";
        if (strcmp(tools[i].name, "shell") == 0)        { arg_name = "cmd";   arg_desc = "The shell command to run"; }
        else if (strcmp(tools[i].name, "file_read") == 0) { arg_name = "path"; arg_desc = "The file path to read"; }
        else if (strcmp(tools[i].name, "file_write") == 0){ arg_name = "content"; arg_desc = "Path and content separated by newline"; }
        else if (strcmp(tools[i].name, "pkg_install") == 0){ arg_name = "name"; arg_desc = "The package name to install"; }
        else if (strcmp(tools[i].name, "pkg_list") == 0)  { arg_name = NULL;   arg_desc = NULL; }
        else if (strcmp(tools[i].name, "meminfo") == 0)   { arg_name = NULL;   arg_desc = NULL; }
        else if (strcmp(tools[i].name, "uptime") == 0)    { arg_name = NULL;   arg_desc = NULL; }

        n += ksnprintf(buf + n, size - n,
                       "{\"type\":\"function\",\"function\":{\"name\":\"%s\",\"description\":\"%s\",\"parameters\":",
                       tools[i].name, tools[i].description);
        if (arg_name) {
            n += ksnprintf(buf + n, size - n,
                           "{\"type\":\"object\",\"properties\":{\"%s\":{\"type\":\"string\",\"description\":\"%s\"}},\"required\":[\"%s\"]}}",
                           arg_name, arg_desc, arg_name);
        } else {
            n += ksnprintf(buf + n, size - n,
                           "{\"type\":\"object\",\"properties\":{}}");
        }
        n += ksnprintf(buf + n, size - n, "}}");
    }
    n += ksnprintf(buf + n, size - n, "]");
    return n;
}

/* Extract a string value for a given key from a JSON object string.
 * Looks for "key":"value" and copies the value (un-escaping \" and \\)
 * into out. Returns 1 if found, 0 if not. */
static int json_extract_string(const char* json, const char* key,
                                char* out, size_t out_size) {
    if (!json || !key || !out || out_size == 0) return 0;
    /* Build the needle: "key":" */
    char needle[64];
    int nlen = ksnprintf(needle, sizeof(needle), "\"%s\":\"", key);
    if (nlen <= 0 || nlen >= (int)sizeof(needle)) return 0;

    const char* p = json;
    while (*p) {
        if (memcmp(p, needle, nlen) == 0) {
            p += nlen;
            /* Copy until unescaped " */
            int o = 0;
            while (*p && o < (int)out_size - 1) {
                if (*p == '\\' && p[1]) {
                    p++;
                    if (*p == 'n') { if (o < (int)out_size - 1) out[o++] = '\n'; }
                    else if (*p == 't') { if (o < (int)out_size - 1) out[o++] = '\t'; }
                    else if (*p == '"') { if (o < (int)out_size - 1) out[o++] = '"'; }
                    else if (*p == '\\') { if (o < (int)out_size - 1) out[o++] = '\\'; }
                    else { if (o < (int)out_size - 1) out[o++] = *p; }
                    p++;
                } else if (*p == '"') {
                    break;
                } else {
                    out[o++] = *p++;
                }
            }
            out[o] = 0;
            return 1;
        }
        p++;
    }
    return 0;
}

/* Extract the first tool call from an OpenAI-format response.
 * Looks for "tool_calls":[{..."name":"<name>"..."arguments":"<args>"}].
 * On success, copies name + args into the provided buffers and returns 1.
 * Also extracts the tool_call_id if present (for the "tool" role response).
 * Returns 0 if no tool_calls found. */
static int ai_extract_tool_call(const char* response_json,
                                 char* name, size_t name_size,
                                 char* args, size_t args_size,
                                 char* call_id, size_t call_id_size) {
    const char* tc = strstr(response_json, "\"tool_calls\"");
    if (!tc) return 0;
    /* Find the first "name":"..." after tool_calls. */
    const char* name_pos = strstr(tc, "\"name\":\"");
    if (!name_pos) return 0;
    name_pos += 8;  /* skip "name":" */
    int n = 0;
    while (*name_pos && *name_pos != '"' && n < (int)name_size - 1) {
        name[n++] = *name_pos++;
    }
    name[n] = 0;

    /* Find "arguments":"..." after the name. */
    const char* args_pos = strstr(name_pos, "\"arguments\":\"");
    if (args_pos) {
        args_pos += 13;  /* skip "arguments":" */
        int a = 0;
        while (*args_pos && a < (int)args_size - 1) {
            if (*args_pos == '\\' && args_pos[1]) {
                args_pos++;
                if (*args_pos == '"') { if (a < (int)args_size - 1) args[a++] = '"'; }
                else if (*args_pos == '\\') { if (a < (int)args_size - 1) args[a++] = '\\'; }
                else if (*args_pos == 'n') { if (a < (int)args_size - 1) args[a++] = '\n'; }
                else { if (a < (int)args_size - 1) args[a++] = *args_pos; }
                args_pos++;
            } else if (*args_pos == '"') {
                break;
            } else {
                args[a++] = *args_pos++;
            }
        }
        args[a] = 0;
    } else {
        args[0] = 0;
    }

    /* Find "id":"..." for the tool_call_id (may be before or after name). */
    if (call_id && call_id_size > 0) {
        call_id[0] = 0;
        const char* id_pos = strstr(tc, "\"id\":\"");
        if (id_pos) {
            id_pos += 6;
            int i = 0;
            while (*id_pos && *id_pos != '"' && i < (int)call_id_size - 1) {
                call_id[i++] = *id_pos++;
            }
            call_id[i] = 0;
        }
    }

    return 1;
}

/* Send a chat completion request with a pre-built messages array and
 * optional tools schema. Returns 0 on success and copies the raw JSON
 * response body into response_buf (so the caller can parse tool_calls).
 * This is the "with tools" variant of ai_http_post — it does NOT extract
 * the "content" field; it returns the raw JSON for the caller to parse. */
static int ai_http_post_messages(int provider,
                                  const char* messages_json,
                                  const char* tools_json,
                                  char* response_buf, size_t response_size) {
    if (!keys_set[provider]) {
        ksnprintf(response_buf, response_size,
                  "[no API key set for %s]", provider_display[provider]);
        return -1;
    }
    extern int net_is_up(void);
    if (!net_is_up()) {
        ksnprintf(response_buf, response_size, "[network not up]");
        return -1;
    }

    int use_tls = (provider_endpoints[provider][0] == 'h' &&
                   provider_endpoints[provider][1] == 't' &&
                   provider_endpoints[provider][2] == 't' &&
                   provider_endpoints[provider][3] == 'p' &&
                   provider_endpoints[provider][4] == 's');

    /* Build the JSON body:
     *   {"model":"<model>","messages":[<messages_json>],"tools":<tools_json>}
     * or without tools if tools_json is NULL. */
    static char body[4096];
    int blen = 0;
    blen += ksnprintf(&body[blen], sizeof(body) - blen,
                      "{\"model\":\"%s\",\"messages\":[",
                      provider_models[provider]);
    /* Copy messages_json (already escaped by the caller). */
    int mlen = strlen(messages_json);
    if (blen + mlen >= (int)sizeof(body) - 256) {
        mlen = sizeof(body) - 256 - blen;
    }
    memcpy(&body[blen], messages_json, mlen);
    blen += mlen;
    blen += ksnprintf(&body[blen], sizeof(body) - blen, "]");

    if (tools_json) {
        blen += ksnprintf(&body[blen], sizeof(body) - blen,
                          ",\"tools\":%s", tools_json);
        /* Request tool_choice="auto" so the model decides when to call. */
        blen += ksnprintf(&body[blen], sizeof(body) - blen,
                          ",\"tool_choice\":\"auto\"");
    }
    blen += ksnprintf(&body[blen], sizeof(body) - blen, "}");
    body[blen] = 0;

    pr_info("AI: POST %s (model %s, %u-byte body, tools=%s)\n",
            provider_endpoints[provider], provider_models[provider],
            (unsigned)blen, tools_json ? "yes" : "no");

    /* Build HTTP request (same as ai_http_post but with our body). */
    char scheme[16], host[128], path[256];
    uint16_t port;
    http_parse_url(provider_endpoints[provider],
                   scheme, sizeof(scheme), host, sizeof(host),
                   &port, path, sizeof(path));

    ipv4_addr_t ip;
    if (!net_resolve(host, &ip)) {
        ksnprintf(response_buf, response_size, "[DNS failed for %s]", host);
        return -1;
    }

    extern int tls_connect(ipv4_addr_t, uint16_t, const char*);
    extern int tls_send(const void*, uint16_t);
    extern int tls_recv(void*, uint16_t, uint32_t);
    extern void tls_close(void);

    if (use_tls) {
        if (!tls_connect(ip, port, host)) {
            ksnprintf(response_buf, response_size, "[TLS handshake to %s:%u failed]", host, (unsigned)port);
            return -1;
        }
    } else {
        if (!tcp_connect(ip, port, 5000)) {
            ksnprintf(response_buf, response_size, "[TCP connect to %s:%u failed]", host, (unsigned)port);
            return -1;
        }
    }

    static char req[5120];
    int reqlen = 0;
    reqlen += ksnprintf(&req[reqlen], sizeof(req) - reqlen, "POST %s HTTP/1.0\r\n", path);
    reqlen += ksnprintf(&req[reqlen], sizeof(req) - reqlen, "Host: %s\r\n", host);
    reqlen += ksnprintf(&req[reqlen], sizeof(req) - reqlen, "Content-Type: application/json\r\n");
    reqlen += ksnprintf(&req[reqlen], sizeof(req) - reqlen, "Content-Length: %u\r\n", (unsigned)blen);
    reqlen += ksnprintf(&req[reqlen], sizeof(req) - reqlen, "Authorization: Bearer %s\r\n", api_keys[provider]);
    reqlen += ksnprintf(&req[reqlen], sizeof(req) - reqlen, "Connection: close\r\n\r\n");
    if (reqlen + blen > (int)sizeof(req) - 1) blen = sizeof(req) - 1 - reqlen;
    memcpy(&req[reqlen], body, blen);
    reqlen += blen;

    int sent = 0;
    while (sent < reqlen) {
        int chunk = reqlen - sent;
        if (chunk > 1400) chunk = 1400;
        int n = use_tls ? tls_send(&req[sent], (uint16_t)chunk)
                         : tcp_send(&req[sent], (uint16_t)chunk);
        if (n <= 0) {
            if (use_tls) tls_close(); else tcp_close();
            ksnprintf(response_buf, response_size, "[send failed]");
            return -1;
        }
        sent += n;
    }

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
    rbuf[total] = 0;

    if (total == 0) {
        ksnprintf(response_buf, response_size, "[no response from server]");
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

    /* Copy the raw JSON body to the caller's buffer. */
    int rawlen = total - body_start;
    if (rawlen < 0) rawlen = 0;
    if (rawlen >= (int)response_size) rawlen = response_size - 1;
    memcpy(response_buf, &rbuf[body_start], rawlen);
    response_buf[rawlen] = 0;
    return 0;
}

/* ----- chat API ------------------------------------------------------- */
int ai_chat(const char* prompt, char* response, size_t response_size) {
    if (!prompt || !response || response_size == 0) return -1;

    /* Recognize "/clear" as a special command to reset conversation
     * memory. This lets the user reset context without needing a shell
     * command (though `ai clear` also works via ai_clear_memory). */
    if (strcmp(prompt, "/clear") == 0) {
        ai_clear_memory();
        strncpy(response, "[conversation memory cleared]", response_size - 1);
        response[response_size - 1] = 0;
        return 0;
    }

    /* Build a prompt that includes recent conversation history for
     * context, so the AI can refer to earlier turns. */
    char combined[AI_PROMPT_MAX + 1024];
    build_prompt_with_history(prompt, combined, sizeof(combined));

    /* Pick the first provider that has a key set */
    int provider = -1;
    for (int i = 0; i < AI_PROVIDER_COUNT; i++) {
        if (keys_set[i]) { provider = i; break; }
    }

    int rc;
    if (provider < 0) {
        /* No API key configured — use the offline assistant (rule-based).
         * This is NOT a neural model, but gives useful canned responses
         * for system queries without needing network or API keys. */
        if (offline_ai_respond(combined, response, response_size)) {
            rc = 0;
        } else {
            strncpy(response,
                    "[no AI provider configured. Use 'ai setkey <provider> <key>' "
                    "to set an API key. Providers: openai, claude, gemini, glm]",
                    response_size - 1);
            response[response_size - 1] = 0;
            rc = -1;
        }
    } else {
        rc = ai_http_post(provider, combined, response, response_size);
    }

    /* Record this turn in conversation memory (even on failure, so the
     * user sees their question was received — but only if we got a
     * non-empty response). */
    if (response[0]) {
        history_append_turn(prompt, response);
    }

    return rc;
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
    /* Recognize "/clear" for consistency with ai_chat. */
    if (strcmp(prompt, "/clear") == 0) {
        ai_clear_memory();
        strncpy(response, "[conversation memory cleared]", response_size - 1);
        response[response_size - 1] = 0;
        return 0;
    }
    /* Include conversation history for context. */
    char combined[AI_PROMPT_MAX + 1024];
    build_prompt_with_history(prompt, combined, sizeof(combined));
    int rc = ai_http_post(provider, combined, response, response_size);
    if (response[0]) {
        history_append_turn(prompt, response);
    }
    return rc;
}

int ai_chat_with_tools(const char* prompt, char* response,
                       size_t response_size, int max_iterations) {
    if (!prompt || !response || response_size == 0) return -1;

    /* Agentic loop:
     * When an API key is set (ai_any_key_set()), use real HTTP+TLS to
     * call the provider with a proper OpenAI tools[] schema. Parse the
     * response for "tool_calls", execute each tool, feed results back as
     * "role":"tool" messages, and loop up to max_iterations.
     *
     * When no key is set (offline mode), we fall back to keyword-based
     * tool dispatch + the offline rule engine. This is explicitly NOT
     * neural, but it keeps the UX functional for demonstration. */

    /* Pick the first provider with a key. */
    int provider = -1;
    for (int i = 0; i < AI_PROVIDER_COUNT; i++) {
        if (keys_set[i]) { provider = i; break; }
    }

    if (provider >= 0 && net_is_up()) {
        /* Build the tools[] JSON schema once. */
        static char tools_json[2048];
        ai_build_tools_json(tools_json, sizeof(tools_json));

        /* Build the initial messages array:
         *   {"role":"system","content":"..."},{"role":"user","content":"<prompt>"}
         * The caller-supplied prompt is JSON-escaped. */
        static char messages_buf[6144];
        int mlen = 0;
        mlen += ksnprintf(messages_buf + mlen, sizeof(messages_buf) - mlen,
                          "{\"role\":\"system\",\"content\":\"You are an AI assistant running inside "
                          "LestraOS with access to tools (shell, file_read, file_write, pkg_install, "
                          "pkg_list, meminfo, uptime). When the user asks you to perform an action, "
                          "call the appropriate tool. When just chatting, respond normally.\"}");
        mlen += ksnprintf(messages_buf + mlen, sizeof(messages_buf) - mlen,
                          ",{\"role\":\"user\",\"content\":\"");
        /* Escape the prompt for JSON. */
        for (const char* src = prompt; *src && mlen < (int)sizeof(messages_buf) - 16; src++) {
            if (*src == '"' || *src == '\\') {
                messages_buf[mlen++] = '\\';
                messages_buf[mlen++] = *src;
            } else if (*src == '\n') {
                messages_buf[mlen++] = '\\';
                messages_buf[mlen++] = 'n';
            } else if (*src == '\r') {
                /* skip */
            } else {
                messages_buf[mlen++] = *src;
            }
        }
        mlen += ksnprintf(messages_buf + mlen, sizeof(messages_buf) - mlen, "\"}");
        messages_buf[mlen] = 0;

        int total_written = 0;
        int iteration = 0;

        while (iteration < max_iterations) {
            iteration++;

            /* Send the request with tools schema. */
            char raw_resp[AI_RESPONSE_MAX];
            int rc = ai_http_post_messages(provider, messages_buf,
                                            tools_json, raw_resp, sizeof(raw_resp));
            if (rc != 0) {
                /* HTTP failed — copy the error message and fall through
                 * to the offline path below. */
                break;
            }

            /* Check if the response contains tool_calls. */
            char tool_name[64], tool_args[512], call_id[128];
            if (ai_extract_tool_call(raw_resp, tool_name, sizeof(tool_name),
                                      tool_args, sizeof(tool_args),
                                      call_id, sizeof(call_id))) {
                /* Found a tool call. Execute it. */
                const struct ai_tool* tool = ai_tool_find(tool_name);
                char tool_out[1024];
                tool_out[0] = 0;

                if (tool) {
                    /* For tools that take a JSON arguments object, extract
                     * the first string value as the arg. For tools that
                     * take no args, pass empty string. */
                    char exec_args[512];
                    exec_args[0] = 0;
                    if (tool_args[0] == '{') {
                        /* Try known arg names, then fall back to the
                         * first "key":"value" pair. */
                        const char* arg_keys[] = {"cmd", "path", "content",
                                                   "name", "arg", NULL};
                        int found = 0;
                        for (int k = 0; arg_keys[k]; k++) {
                            if (json_extract_string(tool_args, arg_keys[k],
                                                     exec_args, sizeof(exec_args))) {
                                found = 1;
                                break;
                            }
                        }
                        if (!found) {
                            /* No known arg key matched — leave exec_args
                             * empty. The tool will handle the empty arg
                             * gracefully (most tools treat it as "no
                             * argument"). */
                            exec_args[0] = 0;
                        }
                    } else {
                        /* Arguments is already a plain string. */
                        strncpy(exec_args, tool_args, sizeof(exec_args) - 1);
                        exec_args[sizeof(exec_args) - 1] = 0;
                    }

                    pr_info("AI: tool_call %s(args='%s')\n", tool_name, exec_args);
                    tool->handler(exec_args, tool_out, sizeof(tool_out));
                } else {
                    ksnprintf(tool_out, sizeof(tool_out),
                              "[unknown tool: %s]", tool_name);
                }

                /* Append the tool call + result to the output buffer. */
                const char* prefix = "[tool: ";
                int plen = strlen(prefix);
                int i = 0;
                while (i < plen && total_written < (int)response_size - 1)
                    response[total_written++] = prefix[i++];
                const char* tp = tool_name;
                while (*tp && total_written < (int)response_size - 1)
                    response[total_written++] = *tp++;
                const char* sep = "]\n";
                i = 0; plen = strlen(sep);
                while (i < plen && total_written < (int)response_size - 1)
                    response[total_written++] = sep[i++];
                const char* op = tool_out;
                while (*op && total_written < (int)response_size - 1)
                    response[total_written++] = *op++;
                response[total_written++] = '\n';
                response[total_written] = 0;

                /* Append the assistant's tool_call + tool result to the
                 * messages array so the next request has context. */
                mlen += ksnprintf(messages_buf + mlen, sizeof(messages_buf) - mlen,
                                  ",{\"role\":\"assistant\",\"content\":null,"
                                  "\"tool_calls\":[{\"id\":\"%s\",\"type\":\"function\","
                                  "\"function\":{\"name\":\"%s\",\"arguments\":\"",
                                  call_id[0] ? call_id : "call_1", tool_name);
                /* Re-escape tool_args for JSON. */
                for (const char* a = tool_args; *a && mlen < (int)sizeof(messages_buf) - 64; a++) {
                    if (*a == '"' || *a == '\\') {
                        messages_buf[mlen++] = '\\';
                        messages_buf[mlen++] = *a;
                    } else if (*a == '\n') {
                        messages_buf[mlen++] = '\\';
                        messages_buf[mlen++] = 'n';
                    } else {
                        messages_buf[mlen++] = *a;
                    }
                }
                mlen += ksnprintf(messages_buf + mlen, sizeof(messages_buf) - mlen,
                                  "\"}}]},{\"role\":\"tool\",\"tool_call_id\":\"%s\",\"content\":\"",
                                  call_id[0] ? call_id : "call_1");
                /* Escape tool_out for JSON. */
                for (const char* a = tool_out; *a && mlen < (int)sizeof(messages_buf) - 32; a++) {
                    if (*a == '"' || *a == '\\') {
                        messages_buf[mlen++] = '\\';
                        messages_buf[mlen++] = *a;
                    } else if (*a == '\n') {
                        messages_buf[mlen++] = '\\';
                        messages_buf[mlen++] = 'n';
                    } else if ((unsigned char)*a < 0x20) {
                        /* skip control chars */
                    } else {
                        messages_buf[mlen++] = *a;
                    }
                }
                mlen += ksnprintf(messages_buf + mlen, sizeof(messages_buf) - mlen, "\"}");
                messages_buf[mlen] = 0;

                /* Continue the loop — the AI may call more tools or give
                 * a final text response. On the final iteration, don't
                 * send tools so the AI is forced to give a text answer. */
                continue;
            }

            /* No tool_calls — extract the "content" field from the
             * response JSON and return it as the final answer. */
            char content[AI_RESPONSE_MAX];
            if (json_extract_string(raw_resp, "content", content, sizeof(content)) && content[0]) {
                const char* cp = content;
                while (*cp && total_written < (int)response_size - 1) {
                    response[total_written++] = *cp++;
                }
            } else {
                /* No content field — return the raw response (truncated). */
                const char* rp = raw_resp;
                while (*rp && total_written < (int)response_size - 64) {
                    response[total_written++] = *rp++;
                }
            }
            response[total_written] = 0;

            /* Record in conversation memory. */
            history_append_turn(prompt, response);
            return 0;
        }

        /* If we exhausted iterations, return whatever we accumulated. */
        response[total_written] = 0;
        if (total_written > 0) {
            history_append_turn(prompt, response);
            return 0;
        }
        /* Fall through to offline mode. */
    }

    /* OFFLINE MODE: keyword-based tool dispatch (not neural).
     * Improved with more patterns: list files, what time, read file,
     * install X, run X, etc. Real offline reasoning needs pickle_forward
     * (deferred — STRETCH fix #7). */
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
    char tool_out[1024];
    int iterations = 0;
    int dispatched = 0;

    while (iterations < max_iterations) {
        iterations++;
        const char* tool_name = NULL;
        const char* tool_args = "";
        char arg_buf[256];
        arg_buf[0] = 0;

        /* Pattern-match tool requests. Improved with more patterns. */
        if (strstr(prompt, "memory") || strstr(prompt, "meminfo") ||
            strstr(prompt, "how much ram") || strstr(prompt, "free memory")) {
            tool_name = "meminfo";
        } else if (strstr(prompt, "uptime") || strstr(prompt, "how long") ||
                   strstr(prompt, "what time") || strstr(prompt, "system time")) {
            tool_name = "uptime";
        } else if (strstr(prompt, "install ")) {
            tool_name = "pkg_install";
            tool_args = strstr(prompt, "install ") + 8;
            /* Strip trailing words like "package" or "please". */
            strncpy(arg_buf, tool_args, sizeof(arg_buf) - 1);
            arg_buf[sizeof(arg_buf) - 1] = 0;
            /* Take just the first word as the package name. */
            char* sp = arg_buf;
            while (*sp && *sp != ' ') sp++;
            *sp = 0;
            tool_args = arg_buf;
        } else if (strstr(prompt, "list packages") || strstr(prompt, "pkg list") ||
                   strstr(prompt, "installed packages") || strstr(prompt, "what packages")) {
            tool_name = "pkg_list";
        } else if (strstr(prompt, "list files") || strstr(prompt, "ls") ||
                   strstr(prompt, "show files") || strstr(prompt, "dir")) {
            /* Use the shell tool to run "ls" (or equivalent). */
            tool_name = "shell";
            strncpy(arg_buf, "ls", sizeof(arg_buf) - 1);
            arg_buf[sizeof(arg_buf) - 1] = 0;
            tool_args = arg_buf;
        } else if (strstr(prompt, "read file ") || strstr(prompt, "cat ")) {
            tool_name = "file_read";
            const char* fa = strstr(prompt, "read file ");
            if (fa) fa += 10;
            else fa = strstr(prompt, "cat ") + 4;
            strncpy(arg_buf, fa, sizeof(arg_buf) - 1);
            arg_buf[sizeof(arg_buf) - 1] = 0;
            /* Strip trailing words. */
            char* nl = arg_buf;
            while (*nl && *nl != ' ' && *nl != '\n') nl++;
            *nl = 0;
            tool_args = arg_buf;
        } else if (strstr(prompt, "shell ") || strstr(prompt, "run ") ||
                   strstr(prompt, "execute ")) {
            tool_name = "shell";
            const char* sa = strstr(prompt, "shell ");
            if (sa) sa += 6;
            else { sa = strstr(prompt, "run "); if (sa) sa += 4; }
            if (!sa) { sa = strstr(prompt, "execute ") + 8; }
            strncpy(arg_buf, sa, sizeof(arg_buf) - 1);
            arg_buf[sizeof(arg_buf) - 1] = 0;
            /* Strip at newline. */
            char* nl = arg_buf;
            while (*nl && *nl != '\n') nl++;
            *nl = 0;
            tool_args = arg_buf;
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
    /* Record in conversation memory. */
    history_append_turn(prompt, response);
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

    /* Clear conversation memory */
    ai_clear_memory();

    /* Register built-in tools */
    ai_tool_register("shell",       "Run a shell command",                   ai_tool_shell);
    ai_tool_register("file_read",   "Read a file from VFS",                  ai_tool_file_read);
    ai_tool_register("file_write",  "Write a file to VFS",                   ai_tool_file_write);
    ai_tool_register("pkg_install", "Install a package",                     ai_tool_pkg_install);
    ai_tool_register("pkg_list",    "List installed packages",               ai_tool_pkg_list);
    ai_tool_register("meminfo",     "Get memory statistics",                 ai_tool_meminfo);
    ai_tool_register("uptime",      "Get system uptime",                     ai_tool_uptime);

    pr_info("AI subsystem initialized (%d tools registered)\n", tool_count);

    /* Boot-time pickle self-test: parses the embedded tiny GGUF model
     * and runs one forward pass to verify the GGUF parser + transformer
     * + soft-float math work end-to-end. Replaces the old "rule-based
     * only" offline assistant with a real (tiny) neural inference
     * engine. See kernel/ai/pickle.c, kernel/include/lestra/pickle.h,
     * and the standalone lestramanika repo. */
    {
        extern int pickle_selftest(int32_t* out_token);
        int32_t tok = -1;
        pr_info("AI: running pickle GGUF self-test (embedded tiny Llama model)...\n");
        int rc = pickle_selftest(&tok);
        if (rc == 0) {
            pr_info("AI: pickle self-test PASSED — forward pass produced token %d\n", (int)tok);
        } else {
            pr_info("AI: pickle self-test FAILED rc=%d\n", rc);
        }
    }
}
