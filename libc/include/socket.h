/*
 * Lestra OS - C Standard Library - socket (POSIX)
 * Copyright (c) 2026 lestramk.org
 *
 * W1-A finding F: previously the libc had no <socket.h>, so programs
 * that needed struct sockaddr / AF_INET / SOCK_STREAM had to hand-
 * define them.  The kernel implements sys_socket(44)/sys_bind(45)/
 * sys_connect(46)/sys_listen(47)/sys_accept(48)/sys_send(49)/
 * sys_recv(50); sendto/recvfrom fall back to send/recv when the
 * caller doesn't supply a destination/source address.
 *
 * Freestanding — no FP, no SSE.
 */
#ifndef LIBC_SOCKET_H
#define LIBC_SOCKET_H

#include <stddef.h>
#include <stdint.h>

/* POSIX uses socklen_t for address lengths.  The kernel takes plain
 * int; we typedef socklen_t as unsigned int to match Linux. */
typedef unsigned int socklen_t;

/* Address families. */
#define AF_UNSPEC     0
#define AF_UNIX       1     /* local to host (pipes, portals) */
#define AF_LOCAL      1     /* POSIX name for AF_UNIX */
#define AF_INET       2     /* IPv4 Internet protocols */
#define AF_AX25       3
#define AF_IPX        4
#define AF_INET6     10     /* IPv6 Internet protocols */
#define AF_NETLINK   16
#define AF_PACKET    17
#define AF_BLUETOOTH 31

/* Socket types. */
#define SOCK_STREAM    1    /* connection-oriented byte stream (TCP) */
#define SOCK_DGRAM     2    /* connectionless datagram (UDP) */
#define SOCK_RAW       3    /* raw protocol access */
#define SOCK_RDM       4    /* reliably-delivered message */
#define SOCK_SEQPACKET 5    /* sequenced reliable packet stream */
#define SOCK_PACKET   10    /* Linux packet socket (legacy) */

/* Socket option levels. */
#define SOL_SOCKET     1    /* options at the socket API level */

/* Socket options for SOL_SOCKET. */
#define SO_DEBUG        1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_DONTROUTE    5
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_OOBINLINE   10
#define SO_NO_CHECK    11
#define SO_PRIORITY    12
#define SO_LINGER      13
#define SO_BSDCOMPAT   14
#define SO_REUSEPORT   15

/* send()/recv() flags. */
#define MSG_OOB         0x0001
#define MSG_PEEK        0x0002
#define MSG_DONTROUTE   0x0004
#define MSG_CTRUNC      0x0008
#define MSG_PROBE       0x0010
#define MSG_TRUNC       0x0020
#define MSG_DONTWAIT    0x0040
#define MSG_EOR         0x0080
#define MSG_WAITALL     0x0100
#define MSG_FIN         0x0200
#define MSG_SYN         0x0400
#define MSG_CONFIRM     0x0800
#define MSG_RST         0x1000
#define MSG_ERRQUEUE    0x2000
#define MSG_NOSIGNAL    0x4000
#define MSG_MORE        0x8000

/* shutdown() how. */
#define SHUT_RD          0
#define SHUT_WR          1
#define SHUT_RDWR        2

/* Generic socket address.  All address-family-specific structs start
 * with the same sa_family field; the rest is family-specific data. */
struct sockaddr {
    unsigned short sa_family;     /* address family (AF_*) */
    char           sa_data[14];   /* family-specific address bytes */
};

/* IPv4 socket address.  sin_port and sin_addr are in network byte
 * order (big-endian) — callers must htons()/htonl() before assigning. */
struct sockaddr_in {
    unsigned short sin_family;    /* AF_INET */
    unsigned short sin_port;      /* port, network byte order */
    unsigned int   sin_addr;      /* IPv4 address, network byte order */
    unsigned char  sin_zero[8];   /* pad to sizeof(struct sockaddr) */
};

/* IPv6 socket address (compact form — kernel doesn't use IPv6 yet). */
struct sockaddr_in6 {
    unsigned short sin6_family;   /* AF_INET6 */
    unsigned short sin6_port;     /* port, network byte order */
    unsigned int   sin6_flowinfo; /* IPv6 flow label */
    unsigned char  sin6_addr[16]; /* IPv6 address */
    unsigned int   sin6_scope_id; /* scope ID */
};

/* Unix-domain socket address. */
struct sockaddr_un {
    unsigned short sun_family;    /* AF_UNIX */
    char           sun_path[108]; /* filesystem path */
};

/* Wrappers (defined in libc/src/unistd.c).  The kernel implements
 * sys_socket through sys_recv (44..50).  sendto() and recvfrom()
 * fall back to send()/recv() when the caller passes NULL for the
 * destination/source address; otherwise they return -1/ENOSYS until
 * the kernel grows dedicated syscalls. */
int     socket(int domain, int type, int protocol);
int     bind(int fd, const struct sockaddr* addr, socklen_t addrlen);
int     connect(int fd, const struct sockaddr* addr, socklen_t addrlen);
int     listen(int fd, int backlog);
int     accept(int fd, struct sockaddr* addr, socklen_t* addrlen);
ssize_t send(int fd, const void* buf, size_t len, int flags);
ssize_t recv(int fd, void* buf, size_t len, int flags);
ssize_t sendto(int fd, const void* buf, size_t len, int flags,
               const struct sockaddr* dest_addr, socklen_t dest_len);
ssize_t recvfrom(int fd, void* buf, size_t len, int flags,
                 struct sockaddr* src_addr, socklen_t* src_len);

#endif /* LIBC_SOCKET_H */
