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

Plain chat (no tools):

```
lestra:/$ ai chat what is the meaning of life
[AI chat] sending prompt (28 chars)...

--- AI Response ---
[simulated response from OpenAI (model: gpt-4o). Real HTTP requires a
TCP/IP stack — see docs/NETWORK.md.]

I received your prompt: "what is the meaning of life". With a real
network stack I would respond with the assistant message.
```

Agentic chat (with tools):

```
lestra:/$ ai agent show me system memory
[AI agent] running agentic loop with tools...

--- Agent Output ---
=== Agentic Chat (simulated) ===

User: show me system memory

[calling tool: meminfo]
Result: total=4095mb used=64mb free=4031mb

Assistant: I executed the requested tool and reported the result above.
```

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
│  HTTP layer (stub)                                 │
│  - ai_http_post() — currently simulated            │
│  - Replace with real HTTP client when TCP/IP stack │
│    is available (see docs/NETWORK.md, planned)     │
├────────────────────────────────────────────────────┤
│  Tool implementations                              │
│  - shell       → kernel/core/shell.c (dispatch)    │
│  - file_*      → kernel/fs/vfs.c                   │
│  - pkg_*       → pkg/lestra-pkg.c                  │
│  - meminfo     → kernel/mm/pmm.c                   │
│  - uptime      → kernel/drivers/char/timer.c       │
└────────────────────────────────────────────────────┘
```

## What's needed for real AI calls

To make the AI subsystem actually call provider APIs (instead of returning
simulated responses), the following are needed:

1. **TCP/IP stack** — implement or port lwIP into `kernel/net/`
2. **DNS resolver** — translate `api.openai.com` to an IP address
3. **TLS client** — providers require HTTPS. Port mbedTLS or similar
4. **HTTP client** — build request with `Authorization: Bearer <key>`
5. **JSON parser** — parse the response body to extract the assistant
   message and `tool_calls` array

When all five are available, only `ai_http_post()` in `kernel/ai/ai.c`
needs to be rewritten. Everything else (key management, tool framework,
dispatch loop) stays the same.

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
