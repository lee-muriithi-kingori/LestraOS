# LestraOS Networking Guide

LestraOS now has a real TCP/IP stack. This document explains what works, what
doesn't, and how to use the network features.

## What's implemented

| Layer | Protocol | Status |
|-------|----------|--------|
| L2 | Ethernet (IEEE 802.3) | ✅ |
| L2 | ARP | ✅ |
| L3 | IPv4 | ✅ |
| L3 | ICMP (ping) | ✅ |
| L4 | UDP | ✅ |
| L4 | TCP (one-shot, request/response) | ✅ |
| App | DHCP client (DORA) | ✅ auto-configures IP at boot |
| App | DNS resolver (A records) | ✅ |
| App | HTTP/1.0 client (GET + POST) | ✅ plain HTTP only |
| App | HTTPS/TLS | ❌ not implemented (see below) |

## Boot sequence

When the kernel boots, `net_init()` runs after all other subsystems:

1. **PCI scan** finds the E1000 NIC (vendor 0x8086, device 0x100E)
2. **E1000 driver** initializes the card: reads MAC from EEPROM, sets up
   RX/TX descriptor rings, enables the controller
3. **DHCP client** sends a DISCOVER broadcast, waits for OFFER, sends
   REQUEST, waits for ACK. On success: IP, subnet mask, gateway, and DNS
   are configured.

DHCP runs in the background (driven by the 1 kHz timer IRQ). Boot does
NOT block on it — you can use the shell while DHCP is still negotiating.
Run `network` to check status.

## Shell commands

```
network                 Show IP, MAC, gateway, DNS, link status
ping <host-or-ip>       ICMP echo (e.g. "ping 10.0.2.2")
wget <url>              HTTP GET (e.g. "wget http://example.com/")
pkg install <name>      Downloads package via HTTP if URL is in catalog
ai chat <prompt>        HTTP POST to configured AI provider
ai setkey <p> <key>     Set API key for provider p (openai|claude|gemini|glm)
ai setendpoint <p> <url> Override provider endpoint
ai setmodel <p> <model>  Override model name (e.g. "ai setmodel glm glm-5.2")
ai providers            Show all configured providers + endpoints
```

## QEMU networking

The default QEMU command (`make run`) uses `-netdev user` (SLIRP), which
gives the VM:

- **DHCP server** at 10.0.2.2 (assigns 10.0.2.15 to the VM)
- **Gateway** at 10.0.2.2
- **DNS** at 10.0.2.3
- **Outbound NAT** — the VM can open TCP/UDP connections to the internet
- **No inbound** — external hosts can't initiate connections to the VM

The `make run` target in the Makefile already includes E1000 + user
networking. If you run QEMU manually:

```bash
qemu-system-x86_64 -cdrom build/lestraos.iso -m 512M -nographic \
  -netdev user,id=net0 -device e1000,netdev=net0
```

## Using the AI client

The kernel has a pre-built HTTP client and a chat-completions request
builder that uses the OpenAI-compatible schema. This works with:

- **GLM cloud** (Z.ai) — `https://open.bigmodel.cn/api/paas/v4/chat/completions`
- **OpenAI** — `https://api.openai.com/v1/chat/completions`
- **Anthropic Claude** — `https://api.anthropic.com/v1/messages`
- **Ollama** (local) — `http://localhost:11434/v1/chat/completions`
- **llama.cpp server** (local) — `http://localhost:8080/v1/chat/completions`
- **vLLM** (local) — `http://localhost:8000/v1/chat/completions`

### The HTTPS limitation

Cloud APIs (GLM, OpenAI, Claude) are **HTTPS-only**. LestraOS does not
implement TLS, so you cannot connect to them directly. Two options:

**Option A: Local LLM (easiest, works today)**

Run Ollama on the host machine:
```bash
# On the host:
ollama serve                          # listens on localhost:11434
ollama pull llama3.2                  # download a model
```

Then in LestraOS (QEMU's 10.0.2.2 = host):
```
lestra:/$ ai setendpoint glm http://10.0.2.2:11434/v1/chat/completions
lestra:/$ ai setmodel glm llama3.2
lestra:/$ ai setkey glm dummy         # Ollama doesn't need a key, but our client requires one
lestra:/$ ai chat Hello, who are you?
```

**Option B: TLS-terminating proxy (for cloud APIs)**

Run a local proxy that accepts plain HTTP and forwards over HTTPS:
```bash
# On the host, using socat (one-shot forwarder):
socat TCP-LISTEN:8443,reuseaddr,fork \
  OPENSSL:open.bigmodel.cn:443,verify=0

# Or using nginx as a reverse proxy:
# nginx.conf:
#   server {
#     listen 8080;
#     location / { proxy_pass https://open.bigmodel.cn; }
#   }
```

Then in LestraOS:
```
lestra:/$ ai setendpoint glm http://10.0.2.2:8443/api/paas/v4/chat/completions
lestra:/$ ai setmodel glm glm-4.6
lestra:/$ ai setkey glm YOUR_GLM_API_KEY
lestra:/$ ai chat Explain how TCP works in one sentence
```

### Pre-configured defaults

The AI subsystem ships with these defaults:

| Provider | Default endpoint | Default model |
|----------|-----------------|---------------|
| openai | https://api.openai.com/v1/chat/completions | gpt-4o |
| claude | https://api.anthropic.com/v1/messages | claude-3-5-sonnet-20240620 |
| gemini | https://generativelanguage.googleapis.com/v1beta/models/gemini-pro:generateContent | gemini-1.5-pro |
| glm | https://open.bigmodel.cn/api/paas/v4/chat/completions | glm-4.6 |

Override any of these with `ai setendpoint` and `ai setmodel`. To use GLM 5.2:
```
ai setmodel glm glm-5.2
```

## Package downloads

The package catalog includes 60+ packages. Most have `url = NULL` (no
download available — they're catalog entries for future use). Four demo
packages have real HTTP URLs:

```
pkg install hello      # downloads http://example.com/
pkg install rfc2616    # downloads the HTTP/1.1 spec
pkg install rfc791     # downloads the IPv4 spec
pkg install rfc1035    # downloads the DNS spec
```

When a package has a URL and the network is up, `pkg install` does a real
HTTP GET and stores the downloaded bytes in the in-memory VFS at
`/var/packages/<name>`. Verify with:
```
file ls
file cat /var/packages/hello
```

## Architecture

```
User shell
    |
    v
HTTP client (net/http.c) ----- POST/GET
    |
    v
TCP state machine (net/tcp.c) - SYN/SYN-ACK/ACK/FIN
    |
    v
IP layer + ICMP + ARP (net/net.c)
    |
    v
E1000 NIC driver (drivers/net/e1000.c) - PCI + MMIO + DMA rings
    |
    v
QEMU virtual NIC -> SLIRP user-mode network -> host network
```

All network processing happens in `net_tick()`, called from the 1 kHz timer
IRQ. The stack is single-threaded (no preemption), with a re-entrancy guard
so that synchronous wait loops (e.g. `tcp_connect`) can also call
`net_tick()` without corrupting state.

## Limitations

- **No TLS/HTTPS.** Cloud AI APIs need a local proxy. See above.
- **One TCP connection at a time.** The shell is request/response; this is
  fine for HTTP GET/POST and AI chat.
- **No TCP retransmit timer.** If a packet is lost, the connection stalls.
  For LAN use this is fine; for lossy WAN, add a retransmit timer in
  `tcp_tick()`.
- **DNS: A records only.** No AAAA, no CNAME chase, no caching beyond the
  ARP cache.
- **No inbound connections.** QEMU SLIRP doesn't support them (without
  `-hostfwd`). The kernel also has no `listen()` syscall yet.
- **8 KB max HTTP response body.** Larger responses are truncated.
