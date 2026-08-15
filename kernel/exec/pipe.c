/*
 * Lestra OS - Pipe implementation (ring buffer + blocking I/O)
 */
#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/sched.h>
#include <lestra/mm.h>
#include <string.h>

#define MAX_PIPES 16
#define PIPE_BUF_SIZE 4096
#define PIPE_FD_BASE 200

struct pipe {
    int in_use;
    uint8_t buf[PIPE_BUF_SIZE];
    int read_pos;
    int write_pos;
    int bytes_available;
    int read_open;
    int write_open;
    int read_waiter;   /* PID of process blocked reading, 0 = none */
    int write_waiter;  /* PID of process blocked writing, 0 = none */
};

static struct pipe pipes[MAX_PIPES];

struct pipe_fd {
    int in_use;
    int pipe_idx;
    int is_write_end;
};

#define MAX_PIPE_FDS 32
static struct pipe_fd pipe_fds[MAX_PIPE_FDS];

int pipe_is_pipe_fd(int fd) {
    return (fd >= PIPE_FD_BASE && fd < PIPE_FD_BASE + MAX_PIPE_FDS && pipe_fds[fd - PIPE_FD_BASE].in_use);
}

int pipe_create(int fds[2]) {
    int pi = -1;
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].in_use) { pi = i; break; }
    }
    if (pi < 0) return -1;
    memset(&pipes[pi], 0, sizeof(pipes[pi]));
    pipes[pi].in_use = 1;
    pipes[pi].read_open = 1;
    pipes[pi].write_open = 1;

    int rfd = -1, wfd = -1;
    for (int i = 0; i < MAX_PIPE_FDS; i++) {
        if (!pipe_fds[i].in_use) {
            if (rfd < 0) {
                rfd = i + PIPE_FD_BASE;
                pipe_fds[i].in_use = 1;
                pipe_fds[i].pipe_idx = pi;
                pipe_fds[i].is_write_end = 0;
            } else if (wfd < 0) {
                wfd = i + PIPE_FD_BASE;
                pipe_fds[i].in_use = 1;
                pipe_fds[i].pipe_idx = pi;
                pipe_fds[i].is_write_end = 1;
                break;
            }
        }
    }
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) pipe_fds[rfd - PIPE_FD_BASE].in_use = 0;
        pipes[pi].in_use = 0;
        return -1;
    }
    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

int pipe_create2(int fds[2], int flags) {
    return pipe_create(fds);
}

ssize_t pipe_read(int fd, void* buf, size_t count) {
    if (!pipe_is_pipe_fd(fd)) return -1;
    struct pipe_fd* pfd = &pipe_fds[fd - PIPE_FD_BASE];
    struct pipe* p = &pipes[pfd->pipe_idx];
    if (pfd->is_write_end) return -1;
    while (p->bytes_available == 0) {
        if (!p->write_open) return 0;
        extern int proc_getpid(void);
        p->read_waiter = proc_getpid();
        task_block();
        p->read_waiter = 0;
    }
    size_t to_read = count;
    if (to_read > (size_t)p->bytes_available) to_read = p->bytes_available;
    for (size_t i = 0; i < to_read; i++) {
        ((uint8_t*)buf)[i] = p->buf[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
        p->bytes_available--;
    }
    if (p->write_waiter > 0) {
        extern void task_unblock_pid(int pid);
        task_unblock_pid(p->write_waiter);
        p->write_waiter = 0;
    }
    return (ssize_t)to_read;
}

ssize_t pipe_write(int fd, const void* buf, size_t count) {
    if (!pipe_is_pipe_fd(fd)) return -1;
    struct pipe_fd* pfd = &pipe_fds[fd - PIPE_FD_BASE];
    struct pipe* p = &pipes[pfd->pipe_idx];
    if (!pfd->is_write_end) return -1;
    if (!p->read_open) return -32; /* EPIPE */
    size_t written = 0;
    while (written < count) {
        while (p->bytes_available >= PIPE_BUF_SIZE) {
            if (!p->read_open) return -32;
            extern int proc_getpid(void);
            p->write_waiter = proc_getpid();
            task_block();
            p->write_waiter = 0;
        }
        p->buf[p->write_pos] = ((const uint8_t*)buf)[written];
        p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
        p->bytes_available++;
        written++;
        /* Wake blocked reader if any */
        if (p->read_waiter > 0) {
            extern void task_unblock_pid(int pid);
            task_unblock_pid(p->read_waiter);
            p->read_waiter = 0;
        }
    }
    return (ssize_t)written;
}

int pipe_close(int fd) {
    if (!pipe_is_pipe_fd(fd)) return -1;
    struct pipe_fd* pfd = &pipe_fds[fd - PIPE_FD_BASE];
    struct pipe* p = &pipes[pfd->pipe_idx];
    if (pfd->is_write_end) p->write_open = 0;
    else p->read_open = 0;
    pfd->in_use = 0;
    if (!p->read_open && !p->write_open) p->in_use = 0;
    return 0;
}
