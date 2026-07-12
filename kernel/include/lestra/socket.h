/*
 * Lestra OS - Socket layer (BSD sockets API)
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Thin kernel-side socket API that wraps the existing net/tcp + pipe
 * infrastructure. Supports:
 *
 *   AF_INET  + SOCK_STREAM  -> TCP via tcp_connect/tcp_send/tcp_recv_wait
 *   AF_UNIX  + SOCK_STREAM  -> backed by kernel pipes
 *
 * FDs live in [SOCKET_FD_BASE .. SOCKET_FD_BASE + SOCKET_MAX_OPEN).
 *
 * Not yet supported:
 *   - SOCK_DGRAM / SOCK_RAW (returns -EPROTONOSUPPORT)
 *   - listen()/accept() for AF_INET (LestraOS TCP is single-conn client)
 *   - non-blocking mode
 */
#ifndef LESTRA_SOCKET_H
#define LESTRA_SOCKET_H

#include <lestra/types.h>
#include <lestra/net.h>

#define SOCKET_FD_BASE    600
#define SOCKET_MAX_OPEN    32   /* fds 600..631 */

/* Address families we know about. */
#define AF_UNSPEC   0
#define AF_UNIX     1
#define AF_INET     2
#define AF_INET6   10

/* Socket types. */
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3

/*_sockaddr structures (mirror Linux layout so user code compiles). */
struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
} __packed;

struct sockaddr_in {
    uint16_t sin_family;     /* AF_INET */
    uint16_t sin_port;       /* network byte order */
    uint32_t sin_addr;       /* network byte order */
    uint8_t  sin_zero[8];
} __packed;

struct sockaddr_un {
    uint16_t sun_family;     /* AF_UNIX */
    char     sun_path[108];
} __packed;

/* Sol/socket levels and option names (subset). */
#define SOL_SOCKET    0xffff
#define SO_REUSEADDR  0x0002
#define SO_RCVBUF     0x0008
#define SO_SNDBUF     0x0007
#define SO_ERROR      0x1007

void    socket_init(void);

int     socket_create(int domain, int type, int protocol);
int     socket_connect(int fd, const struct sockaddr* addr, int addrlen);
int     socket_bind(int fd, const struct sockaddr* addr, int addrlen);
int     socket_listen(int fd, int backlog);
int     socket_accept(int fd, struct sockaddr* addr, int* addrlen);
ssize_t socket_sendto(int fd, const void* buf, size_t len, int flags,
                       const struct sockaddr* dest, int destlen);
ssize_t socket_recvfrom(int fd, void* buf, size_t len, int flags,
                         struct sockaddr* src, int* srclen);
int     socket_close(int fd);
int     socket_getsockopt(int fd, int level, int optname,
                            void* optval, int* optlen);
int     socket_setsockopt(int fd, int level, int optname,
                            const void* optval, int optlen);
int     socket_socketpair(int domain, int type, int protocol, int sv[2]);
int     socket_is_socket_fd(int fd);

#endif /* LESTRA_SOCKET_H */
