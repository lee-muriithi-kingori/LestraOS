/*
 * Lestra OS - Socket layer (BSD sockets API)
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Thin kernel-side socket API that wraps the existing net/tcp + pipe
 * infrastructure.
 *
 *   AF_INET  + SOCK_STREAM  -> TCP via tcp_connect/tcp_send/tcp_recv_wait
 *   AF_UNIX  + SOCK_STREAM  -> backed by kernel pipes (pipe_create)
 *
 * FDs live in [600..631] so they don't collide with any other FD
 * range in the kernel (VFS, ext2, tarfs, procfs, devfs, tmpfs).
 *
 * Not yet supported:
 *   - SOCK_DGRAM / SOCK_RAW (returns -EPROTONOSUPPORT)
 *   - listen()/accept() for AF_INET (LestraOS TCP is single-conn client)
 *   - non-blocking mode
 */

#include <lestra/types.h>
#include <lestra/socket.h>
#include <lestra/pipe.h>
#include <lestra/printk.h>
#include <lestra/timer.h>
#include <string.h>

/* errno constants (mirror libc/include/errno.h). */
#define EPROTONOSUPPORT  93
#define EAFNOSUPPORT     97
#define EBADF             9
#define EFAULT           14
#define EINVAL           22
#define ENOTSOCK         88
#define EOPNOTSUPP       102
#define EISCONN          106
#define ENOTCONN         107
#define EADDRINUSE        98
#define ECONNREFUSED     111
#define ENOPROTOOPT      92

enum sock_kind {
    SOCK_NONE = 0,
    SOCK_INET_TCP,
    SOCK_UNIX_STREAM,
};

enum sock_state {
    SS_UNCONNECTED = 0,
    SS_CONNECTING,
    SS_CONNECTED,
    SS_CLOSED,
};

struct socket {
    int used;
    int domain;          /* AF_INET / AF_UNIX */
    int type;            /* SOCK_STREAM / SOCK_DGRAM */
    int protocol;
    enum sock_kind  kind;
    enum sock_state state;

    /* AF_INET: peer info */
    ipv4_addr_t peer_ip;
    uint16_t    peer_port;

    /* AF_UNIX: read/write ends of an underlying pipe */
    int pipe_read_fd;
    int pipe_write_fd;

    /* Cached options for getsockopt. */
    int so_error;
    int so_rcvbuf;
    int so_sndbuf;
    int so_reuseaddr;
};

static struct socket sockets[SOCKET_MAX_OPEN];

int socket_is_socket_fd(int fd) {
    return fd >= SOCKET_FD_BASE && fd < SOCKET_FD_BASE + SOCKET_MAX_OPEN &&
           sockets[fd - SOCKET_FD_BASE].used;
}

static struct socket* sock_alloc(void) {
    for (int i = 0; i < SOCKET_MAX_OPEN; i++) {
        if (!sockets[i].used) {
            memset(&sockets[i], 0, sizeof(sockets[i]));
            sockets[i].used = 1;
            sockets[i].state = SS_UNCONNECTED;
            sockets[i].pipe_read_fd = -1;
            sockets[i].pipe_write_fd = -1;
            sockets[i].so_rcvbuf = 8192;
            sockets[i].so_sndbuf = 8192;
            return &sockets[i];
        }
    }
    return NULL;
}

void socket_init(void) {
    memset(sockets, 0, sizeof(sockets));
    pr_info("socket: initialized (FD range %d..%d)\n",
            SOCKET_FD_BASE, SOCKET_FD_BASE + SOCKET_MAX_OPEN - 1);
}

int socket_create(int domain, int type, int protocol) {
    (void)protocol;
    if (domain != AF_INET && domain != AF_UNIX) return -EAFNOSUPPORT;
    if (type != SOCK_STREAM) return -EPROTONOSUPPORT;

    struct socket* s = sock_alloc();
    if (!s) return -EBADF;   /* table full */
    s->domain = domain;
    s->type   = type;
    if (domain == AF_INET)  s->kind = SOCK_INET_TCP;
    else                     s->kind = SOCK_UNIX_STREAM;

    int fd = (int)(s - &sockets[0]);
    return fd + SOCKET_FD_BASE;
}

int socket_connect(int fd, const struct sockaddr* addr, int addrlen) {
    if (!socket_is_socket_fd(fd)) return -ENOTSOCK;
    if (!addr || addrlen < (int)sizeof(uint16_t)) return -EFAULT;
    struct socket* s = &sockets[fd - SOCKET_FD_BASE];

    if (s->kind == SOCK_INET_TCP) {
        if (addrlen < (int)sizeof(struct sockaddr_in)) return -EFAULT;
        const struct sockaddr_in* in = (const struct sockaddr_in*)addr;
        if (in->sin_family != AF_INET) return -EAFNOSUPPORT;

        /* Convert network-byte-order fields to host order. */
        uint16_t port = ntohs16(in->sin_port);
        ipv4_addr_t ip;
        memcpy(ip.bytes, &in->sin_addr, 4);

        s->peer_ip   = ip;
        s->peer_port = port;
        s->state     = SS_CONNECTING;

        /* tcp_connect blocks until SYN-ACK or timeout. */
        int rc = tcp_connect(ip, port, 5000);
        if (!rc) {
            s->state = SS_CLOSED;
            s->so_error = ECONNREFUSED;
            return -ECONNREFUSED;
        }
        s->state = SS_CONNECTED;
        return 0;
    }
    /* AF_UNIX connect is not supported — clients should use socketpair
     * or pipes directly. */
    return -EOPNOTSUPP;
}

int socket_bind(int fd, const struct sockaddr* addr, int addrlen) {
    if (!socket_is_socket_fd(fd)) return -ENOTSOCK;
    if (!addr || addrlen < (int)sizeof(uint16_t)) return -EFAULT;
    /* LestraOS TCP is single-connection client-only; bind() is a no-op
     * that succeeds so user code that calls it doesn't crash. */
    (void)addr; (void)addrlen;
    return 0;
}

int socket_listen(int fd, int backlog) {
    if (!socket_is_socket_fd(fd)) return -ENOTSOCK;
    /* Same caveat as bind() — we don't have a TCP server. Accept success. */
    (void)backlog;
    return 0;
}

int socket_accept(int fd, struct sockaddr* addr, int* addrlen) {
    if (!socket_is_socket_fd(fd)) return -ENOTSOCK;
    /* No server-side TCP yet. */
    (void)addr; (void)addrlen;
    return -EOPNOTSUPP;
}

ssize_t socket_sendto(int fd, const void* buf, size_t len, int flags,
                       const struct sockaddr* dest, int destlen) {
    if (!socket_is_socket_fd(fd)) return -ENOTSOCK;
    if (!buf) return -EFAULT;
    (void)flags;
    (void)dest; (void)destlen;
    struct socket* s = &sockets[fd - SOCKET_FD_BASE];

    if (s->kind == SOCK_INET_TCP) {
        if (s->state != SS_CONNECTED) return -ENOTCONN;
        /* tcp_send accepts up to 64 KB per call. */
        if (len > 0xFFFF) len = 0xFFFF;
        int rc = tcp_send(buf, (uint16_t)len);
        return (rc > 0) ? (ssize_t)len : -1;
    }
    if (s->kind == SOCK_UNIX_STREAM) {
        if (s->pipe_write_fd < 0) return -ENOTCONN;
        extern ssize_t pipe_write(int fd, const void* buf, size_t count);
        ssize_t n = pipe_write(s->pipe_write_fd, buf, len);
        if (n < 0) return -1;
        return n;
    }
    return -EOPNOTSUPP;
}

ssize_t socket_recvfrom(int fd, void* buf, size_t len, int flags,
                         struct sockaddr* src, int* srclen) {
    if (!socket_is_socket_fd(fd)) return -ENOTSOCK;
    if (!buf) return -EFAULT;
    (void)flags;
    (void)src; (void)srclen;
    struct socket* s = &sockets[fd - SOCKET_FD_BASE];

    if (s->kind == SOCK_INET_TCP) {
        if (s->state != SS_CONNECTED) return -ENOTCONN;
        if (len > 0xFFFF) len = 0xFFFF;
        int rc = tcp_recv_wait(buf, (uint16_t)len, 30000);
        return (rc >= 0) ? (ssize_t)rc : -1;
    }
    if (s->kind == SOCK_UNIX_STREAM) {
        if (s->pipe_read_fd < 0) return -ENOTCONN;
        extern ssize_t pipe_read(int fd, void* buf, size_t count);
        ssize_t n = pipe_read(s->pipe_read_fd, buf, len);
        return (n >= 0) ? n : -1;
    }
    return -EOPNOTSUPP;
}

int socket_close(int fd) {
    if (!socket_is_socket_fd(fd)) return -ENOTSOCK;
    struct socket* s = &sockets[fd - SOCKET_FD_BASE];
    if (s->kind == SOCK_INET_TCP && s->state == SS_CONNECTED) {
        tcp_close();
    }
    if (s->kind == SOCK_UNIX_STREAM) {
        extern int pipe_close(int fd);
        if (s->pipe_read_fd  >= 0) pipe_close(s->pipe_read_fd);
        if (s->pipe_write_fd >= 0) pipe_close(s->pipe_write_fd);
    }
    s->used = 0;
    s->state = SS_CLOSED;
    return 0;
}

int socket_getsockopt(int fd, int level, int optname,
                       void* optval, int* optlen) {
    if (!socket_is_socket_fd(fd)) return -ENOTSOCK;
    if (!optval || !optlen) return -EFAULT;
    struct socket* s = &sockets[fd - SOCKET_FD_BASE];
    if (level != SOL_SOCKET) return -EOPNOTSUPP;
    int val = 0;
    switch (optname) {
        case SO_ERROR:    val = s->so_error;    break;
        case SO_RCVBUF:   val = s->so_rcvbuf;   break;
        case SO_SNDBUF:   val = s->so_sndbuf;   break;
        case SO_REUSEADDR: val = s->so_reuseaddr; break;
        default: return -ENOPROTOOPT;
    }
    if (*optlen >= (int)sizeof(int)) {
        *(int*)optval = val;
        *optlen = (int)sizeof(int);
    } else {
        return -EINVAL;
    }
    return 0;
}

int socket_setsockopt(int fd, int level, int optname,
                       const void* optval, int optlen) {
    if (!socket_is_socket_fd(fd)) return -ENOTSOCK;
    if (!optval || optlen < (int)sizeof(int)) return -EFAULT;
    struct socket* s = &sockets[fd - SOCKET_FD_BASE];
    int val = *(const int*)optval;
    if (level != SOL_SOCKET) return -EOPNOTSUPP;
    switch (optname) {
        case SO_RCVBUF:    s->so_rcvbuf    = val; break;
        case SO_SNDBUF:    s->so_sndbuf    = val; break;
        case SO_REUSEADDR: s->so_reuseaddr = val; break;
        case SO_ERROR:     s->so_error     = val; break;
        default: return -ENOPROTOOPT;
    }
    return 0;
}

int socket_socketpair(int domain, int type, int protocol, int sv[2]) {
    (void)protocol;
    if (domain != AF_UNIX) return -EAFNOSUPPORT;
    if (type != SOCK_STREAM) return -EPROTONOSUPPORT;
    if (!sv) return -EFAULT;

    /* Create two AF_UNIX sockets and a kernel pipe to back them. */
    struct socket* a = sock_alloc();
    struct socket* b = sock_alloc();
    if (!a || !b) {
        if (a) a->used = 0;
        if (b) b->used = 0;
        return -EBADF;
    }
    int pipe_fds[2];
    extern int pipe_create(int fds[2]);
    if (pipe_create(pipe_fds) < 0) {
        a->used = 0;
        b->used = 0;
        return -EBADF;
    }
    /* Socket A reads from pipe_fds[0], writes to pipe_fds[1].
     * Socket B does the inverse — but a single pipe is unidirectional,
     * so for true bidirectional behavior we'd need two pipes. Use two
     * pipe_create() calls. */
    int pipe_fds2[2];
    if (pipe_create(pipe_fds2) < 0) {
        extern int pipe_close(int);
        pipe_close(pipe_fds[0]);
        pipe_close(pipe_fds[1]);
        a->used = 0;
        b->used = 0;
        return -EBADF;
    }
    /* A: read from pipe_fds[0], write to pipe_fds2[1].
     * B: read from pipe_fds2[0], write to pipe_fds[1]. */
    a->domain = AF_UNIX; a->type = type; a->kind = SOCK_UNIX_STREAM;
    a->state = SS_CONNECTED;
    a->pipe_read_fd  = pipe_fds[0];
    a->pipe_write_fd = pipe_fds2[1];

    b->domain = AF_UNIX; b->type = type; b->kind = SOCK_UNIX_STREAM;
    b->state = SS_CONNECTED;
    b->pipe_read_fd  = pipe_fds2[0];
    b->pipe_write_fd = pipe_fds[1];

    int afd = (int)(a - &sockets[0]) + SOCKET_FD_BASE;
    int bfd = (int)(b - &sockets[0]) + SOCKET_FD_BASE;
    sv[0] = afd;
    sv[1] = bfd;
    return 0;
}
