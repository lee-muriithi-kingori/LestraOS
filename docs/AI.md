# Lestra OS — AI Subsystem & Agentic Tools

LestraOS ships with a built-in AI subsystem that supports multiple
providers, API-key configuration, and an agentic tool-calling framework.

## Supported providers

| Name (CLI)  | Display name       | Default model                  | Endpoint |
|-------------|--------------------|--------------------------------|----------|
| `openai`    | OpenAI             | `gpt-4o`                       | `https://api.openai.com/v1/chat/completions` |
| `claude`    | Anthropic Claude   | `claude-3-5-sonnet-20240620`   | `https://api.anthropic.com/v1/messages` |
| `gemini`    | Google Gemini      | `gemini-1.5-pro`               | `https://generativelanguage.googleapis.com/v1beta/models/gemini-pro:generateContent` |
| `glm`       | Z.ai GLM           | `glm-4.5`                      | `https://open.bigmodel.cn/api/paas/v4/chat/completions` |

## Configuring API keys

From the Lestra shell:

```
lestra:/$ ai keys set openai sk-your-key-here
AI: API key set for OpenAI (length: 51)
API key set for openai

lestra:/$ ai keys set claude sk-ant-your-claude-key
AI: API key set for Anthropic Claude (length: 38)
API key set for claude

lestra:/$ ai keys list
AI API Keys:
-------------------------------------------
  OpenAI            [set]      endpoint: https://api.openai.com/v1/chat/completions
  Anthropic Claude  [set]      endpoint: https://api.anthropic.com/v1/messages
  Google Gemini     [empty]    endpoint: https://generativelanguage.googleapis.com/...
  Z.ai GLM          [empty]    endpoint: https://open.bigmodel.cn/api/paas/v4/...
```

Keys are stored in kernel memory only — they are never written to disk
and are cleared on reboot. This is intentional for security.

## Chatting

This used to be simulated (no network stack existed yet). It isn't
anymore — `ai chat` and `ai agent` now go over the real network:
`net_resolve()` does a DNS query, `tcp_connect`/`tls_connect` opens the
connection (TLS 1.2 for `https://` endpoints), and the response is a real
HTTP reply from whatever provider or local server you pointed it at.
Requires `ai keys set <provider> <key>` first, and DHCP to have completed
(`network` command shows status). Exact terminal output depends on the
provider's response and isn't reproduced here to avoid it going stale
again — try it against a local Ollama/llama.cpp server first if you want
a fast, free way to confirm the transport works end-to-end.

Tool-calling in `ai agent` (`meminfo`, `pkg_install`, etc.) is done
**locally** by detecting keywords in the prompt before it's sent, not via
provider-side function-calling — the JSON body sent to the provider is
just `{"model":...,"messages":[...]}`, no tools schema included.

## Available tools

```
lestra:/$ ai tools

Agentic Tools (7 registered):
-------------------------------------------
  shell              Run a shell command
  file_read          Read a file from VFS
  file_write         Write a file to VFS
  pkg_install        Install a package
  pkg_list           List installed packages
  meminfo            Get memory statistics
  uptime             Get system uptime
```

These tools can be invoked by the AI when running in agentic mode.
The framework dispatches tool calls and feeds results back into the
conversation, allowing the AI to:

- Run shell commands (`shell`)
- Read/write files (`file_read`, `file_write`)
- Install packages (`pkg_install`)
- Query system state (`meminfo`, `uptime`, `pkg_list`)

## Architecture

```
┌────────────────────────────────────────────────────┐
│  Shell (lsh)                                       │
│  - ai chat / ai agent / ai keys / ai tools         │
├────────────────────────────────────────────────────┤
│  AI Subsystem (kernel/ai/ai.c)                     │
│  - Provider metadata + endpoint mapping            │
│  - In-memory API key store (per provider)          │
│  - Tool registry + dispatch loop                   │
│  - ai_chat() — single-shot                         │
│  - ai_chat_with_tools() — agentic loop             │
├────────────────────────────────────────────────────┤
│  HTTP(S) layer — real                              │
│  - ai_http_post() — DNS + TCP/TLS + HTTP request/  │
│    response, dispatches to tls_* for https://      │
│    endpoints, tcp_* otherwise                      │
├────────────────────────────────────────────────────┤
│  Tool implementations                              │
│  - shell       → kernel/core/shell.c (dispatch)    │
│  - file_*      → kernel/fs/vfs.c                   │
│  - pkg_*       → pkg/lestra-pkg.c                  │
│  - meminfo     → kernel/mm/pmm.c                   │
│  - uptime      → kernel/drivers/char/timer.c       │
└────────────────────────────────────────────────────┘
```

## Current limitations of real AI calls

All five of the previously-listed prerequisites are done: hand-rolled
TCP/IP (`kernel/net/net.c`), DNS (`net_resolve()`), TLS 1.2 client
(`kernel/net/tls.c`), the HTTP request/response cycle in
`ai_http_post()`, and a minimal JSON extractor (searches for `"content":`
in the response body — not a real parser, so it'll miss anything the
provider returns in a different shape).

Known rough edges, honestly:

- No `tool_calls` handling — the JSON extractor only looks for
  `"content":"..."`, so provider-side function-calling responses
  (as opposed to the local keyword-based tool dispatch in `ai agent`)
  aren't parsed.
- No response streaming — the whole body is buffered before any of it
  is shown.
- TLS is 1.2 only (cipher suite `TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256`),
  not 1.3 — some providers may reject or downgrade.
- `net_resolve()` needs DHCP to have handed out a DNS server; if that
  hasn't completed yet, `ai chat` will fail until it has.

## Provider-specific request formats

### OpenAI

```http
POST /v1/chat/completions HTTP/1.1
Host: api.openai.com
Authorization: Bearer sk-...
Content-Type: application/json

{
  "model": "gpt-4o",
  "messages": [{"role": "user", "content": "..."}],
  "tools": [
    {"type": "function", "function": {"name": "shell", "description": "...", "parameters": {...}}}
  ]
}
```

### Anthropic Claude

```http
POST /v1/messages HTTP/1.1
Host: api.anthropic.com
x-api-key: sk-ant-...
anthropic-version: 2023-06-01
Content-Type: application/json

{
  "model": "claude-3-5-sonnet-20240620",
  "max_tokens": 4096,
  "messages": [{"role": "user", "content": "..."}],
  "tools": [{"name": "shell", "description": "...", "input_schema": {...}}]
}
```

### Google Gemini

```http
POST /v1beta/models/gemini-1.5-pro:generateContent?key=AIza...
HTTP/1.1
Host: generativelanguage.googleapis.com
Content-Type: application/json

{
  "contents": [{"parts": [{"text": "..."}]}],
  "tools": [{"function_declarations": [{"name": "shell", "description": "..."}]}]
}
```

### Z.ai GLM

```http
POST /api/paas/v4/chat/completions HTTP/1.1
Host: open.bigmodel.cn
Authorization: Bearer <api-key>
Content-Type: application/json

{
  "model": "glm-4.5",
  "messages": [{"role": "user", "content": "..."}],
  "tools": [{"type": "function", "function": {"name": "shell", "description": "..."}}]
}
```

## Security notes

- API keys are kept in kernel memory only; they are never persisted to
  disk or transmitted in plaintext.
- The shell does not echo keys back when set — only the length is shown.
- Use `ai keys clear <provider>` to wipe a key from memory.
- All AI calls would (when the network stack is implemented) go directly
  to the provider over TLS — no proxy, no logging.

## Roadmap

- [ ] TCP/IP stack (lwIP port)
- [ ] DNS resolver
- [ ] TLS 1.3 client (mbedTLS port)
- [ ] HTTP/1.1 client
- [ ] JSON parser
- [ ] Real `ai_http_post()` implementation
- [ ] Tool schema generation from C structs
- [ ] Multi-turn conversation memory
- [ ] Streaming responses (SSE for OpenAI/Anthropic)
- [ ] Token usage tracking + cost display
