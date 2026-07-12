#ifndef LESTRA_PIPE_H
#define LESTRA_PIPE_H

#include <lestra/types.h>

#define PIPE_FD_BASE 200

int pipe_create(int fds[2]);
int pipe_create2(int fds[2], int flags);
ssize_t pipe_read(int fd, void* buf, size_t count);
ssize_t pipe_write(int fd, const void* buf, size_t count);
int pipe_close(int fd);
int pipe_is_pipe_fd(int fd);

#endif
