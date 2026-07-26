/*
 * Lestra OS - Pseudo-Terminal (PTY) Interface
 * Copyright (c) 2026 lestramk.org
 *
 * PTY multiplexing for SSH terminal I/O. Each SSH session gets a PTY
 * pair so the remote shell has proper terminal emulation with line
 * discipline, signal generation, and window-size tracking.
 *
 * FD layout:  PTY FDs occupy [500..519] (PTY_FD_BASE + up to PTY_MAX)
 *             Each pair consumes 2 FDs: even = master, odd = slave.
 *             Pair index = (fd - PTY_FD_BASE) / 2
 */

#ifndef LESTRA_PTY_H
#define LESTRA_PTY_H

#include <lestra/types.h>

/* ---- FD range ---- */
#define PTY_FD_BASE      500     /* First PTY FD number */
#define PTY_FD_MAX       520     /* One past last PTY FD (exclusive) */
#define PTY_MAX_PAIRS    4       /* Maximum concurrent PTY pairs */
#define PTY_BUFFER_SIZE  4096    /* Bytes per direction per pair */

/* ---- Terminal mode flags (termios-like) ---- */
#define PTY_ICANON    0x0001     /* Canonical mode (line buffering) */
#define PTY_ECHO      0x0002     /* Echo input characters back to master */
#define PTY_ISIG      0x0004     /* Generate signals from special chars */
#define PTY_OPOST     0x0008     /* Enable output processing */
#define PTY_ONLCR     0x0010     /* Output: map NL to CR+NL */
#define PTY_ICRNL     0x0020     /* Input: map CR to NL */
#define PTY_INLCR     0x0040     /* Input: map NL to CR */
#define PTY_ECHOE     0x0080     /* Echo erase chars visually (BS-SP-BS) */
#define PTY_ECHOK     0x0100     /* Echo kill char with newline */
#define PTY_ECHOCTL   0x0200     /* Echo control chars as ^X notation */

/* ---- Default terminal mode mask ---- */
#define PTY_DEFAULT_MODES  (PTY_ICANON | PTY_ECHO | PTY_ISIG | \
                             PTY_OPOST  | PTY_ONLCR | PTY_ICRNL | \
                             PTY_ECHOE  | PTY_ECHOCTL)

/* ---- Special / control characters ---- */
#define PTY_VINTR     0x03       /* Ctrl-C  → SIGINT */
#define PTY_VQUIT     0x1C       /* Ctrl-\  → SIGQUIT */
#define PTY_VERASE    0x7F       /* DEL     → erase last char */
#define PTY_VBS       0x08       /* BS      → erase last char (alternative) */
#define PTY_VWERASE   0x17       /* Ctrl-W  → erase last word */
#define PTY_VKILL     0x15       /* Ctrl-U  → erase entire line */
#define PTY_VEOF      0x04       /* Ctrl-D  → end-of-file / flush */
#define PTY_VSUSP     0x1A       /* Ctrl-Z  → SIGTSTP */
#define PTY_VSTART    0x11       /* Ctrl-Q  → resume output (XON) */
#define PTY_VSTOP     0x13       /* Ctrl-S  → pause output (XOFF) */

/* ---- Window size structure (matches POSIX struct winsize) ---- */
struct winsize {
    uint16_t ws_row;             /* Terminal rows (lines) */
    uint16_t ws_col;             /* Terminal columns (characters) */
    uint16_t ws_xpixel;         /* Horizontal pixels (unused, set to 0) */
    uint16_t ws_ypixel;         /* Vertical pixels (unused, set to 0) */
};

/* ---- PTY pair state ---- */
struct pty_pair {
    int in_use;                  /* 1 if this pair is allocated */

    /* File descriptors for each side */
    int master_fd;               /* Master FD (SSH / controlling side) */
    int slave_fd;                /* Slave FD  (shell / terminal side) */

    /* Master-side open count (multiple readers allowed) */
    int master_open;             /* 1 while master side is open */
    int slave_open;              /* 1 while slave side is open */

    /* Terminal mode bitmap */
    uint32_t term_modes;

    /* ---- Ring buffer: master → slave (user typed input) ---- */
    uint8_t  slave_input_buf[PTY_BUFFER_SIZE];
    size_t   slave_input_read_pos;
    size_t   slave_input_write_pos;
    size_t   slave_input_count;  /* Occupied bytes */

    /* ---- Ring buffer: slave → master (shell output) ---- */
    uint8_t  master_output_buf[PTY_BUFFER_SIZE];
    size_t   master_output_read_pos;
    size_t   master_output_write_pos;
    size_t   master_output_count; /* Occupied bytes */

    /* ---- Canonical line-editing buffer (input direction only) ---- */
    uint8_t  line_buf[PTY_BUFFER_SIZE];
    size_t   line_len;            /* Current characters in line buffer */

    /* ---- EOF pending flag (Ctrl-D on empty line → next slave_read returns 0) ---- */
    int eof_pending;

    /* ---- Window size (for SIGWINCH and TIOCSWINSZ) ---- */
    struct winsize winsize;

    /* ---- Slave foreground process group for signal delivery ---- */
    int slave_pgid;               /* Process group ID receiving signals */

    /* ---- Blocked-task waiters (for blocking I/O) ---- */
    int master_read_waiter;       /* PID blocked on master_read, 0 = none */
    int slave_read_waiter;        /* PID blocked on slave_read, 0 = none */
    int master_write_waiter;      /* PID blocked on master_write, 0 = none */
    int slave_write_waiter;       /* PID blocked on slave_write, 0 = none */
};

/* ---- PTY FD slot (maps global FD to a pair + side) ---- */
struct pty_fd_slot {
    int in_use;                   /* 1 if this FD slot is occupied */
    int pair_idx;                 /* Index into pty_pairs[] */
    int is_master;                /* 1 = master side, 0 = slave side */
};

/* ---- Public API ---- */

/* Initialize the PTY subsystem (call once at boot) */
void pty_init(void);

/* Allocate a new PTY pair.  Returns 0 on success, <0 on error.
 * master_fd and slave_fd are filled with the allocated FD numbers. */
int  pty_create(int *master_fd, int *slave_fd);

/* Write data from the master (SSH) side into the PTY.
 * Data flows master → slave after input line-discipline processing.
 * Returns number of bytes consumed, or <0 on error. */
int  pty_master_write(int fd, const void *buf, size_t count);

/* Read shell output from the master side.
 * Returns bytes read (0 if slave closed and buffer empty), <0 on error. */
ssize_t pty_master_read(int fd, void *buf, size_t count);

/* Write data from the slave (shell) side into the PTY.
 * Data flows slave → master after output processing.
 * Returns number of bytes consumed, or <0 on error. */
int  pty_slave_write(int fd, const void *buf, size_t count);

/* Read user input from the slave side.
 * In canonical mode returns one complete line.
 * Returns bytes read (0 = EOF), <0 on error. */
ssize_t pty_slave_read(int fd, void *buf, size_t count);

/* Set the terminal window size for the PTY identified by fd. */
int  pty_set_winsize(int fd, const struct winsize *ws);

/* Close one side of a PTY.  When both sides close, the pair is freed. */
int  pty_close(int fd);

/* Check whether a given fd falls in the PTY range and is active. */
int  pty_is_pty_fd(int fd);

/* Retrieve the pty_pair for a given fd (NULL if not a PTY). */
struct pty_pair *pty_get_pair(int fd);

/* Set the slave foreground process group (for SIGINT/SIGTSTP delivery). */
int  pty_set_slave_pgid(int fd, int pgid);

/* Set terminal modes for a PTY pair. */
int  pty_set_modes(int fd, uint32_t modes);

/* Get current terminal modes for a PTY pair. */
uint32_t pty_get_modes(int fd);

#endif /* LESTRA_PTY_H */
