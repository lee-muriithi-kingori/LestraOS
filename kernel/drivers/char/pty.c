/*
 * Lestra OS - PTY (Pseudo-Terminal) Multiplexing Driver
 * Copyright (c) 2026 lestramk.org
 *
 * Complete PTY implementation with integrated line discipline for
 * SSH terminal I/O.  Each SSH session allocates a PTY pair: the
 * SSH server holds the master FD, the shell process holds the slave FD.
 *
 * Data flow:
 *   Master write  → input line discipline → slave input buffer → slave read
 *   Slave write   → output processing    → master output buffer → master read
 *
 * Line discipline (input, master→slave):
 *   - Canonical mode: line buffering, echo, backspace editing
 *   - Signal chars:  Ctrl-C → SIGINT, Ctrl-Z → SIGTSTP, Ctrl-D → EOF
 *   - Erase:  BS/DEL → backspace, Ctrl-U → kill line
 *   - CR/NL mapping: ICRNL, INLCR
 *
 * Output processing (slave→master):
 *   - ONLCR: map \n → \r\n
 *
 * FD layout:
 *   FDs [500..519],  pair index = (fd - PTY_FD_BASE) / 2
 *   Master = even FD, Slave = odd FD
 */

#include <lestra/types.h>
#include <lestra/pty.h>
#include <lestra/printk.h>
#include <lestra/sched.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Signal number definitions (match kernel/exec/signals.c)           */
/* ------------------------------------------------------------------ */
#define SIGINT    2
#define SIGQUIT   3
#define SIGTSTP   20

/* ------------------------------------------------------------------ */
/*  Static data                                                       */
/* ------------------------------------------------------------------ */

/* PTY pair instances */
static struct pty_pair pty_pairs[PTY_MAX_PAIRS];

/* FD slot table: one entry per FD in [500..519] */
#define PTY_FD_SLOT_COUNT  (PTY_FD_MAX - PTY_FD_BASE)
static struct pty_fd_slot pty_fd_slots[PTY_FD_SLOT_COUNT];

/* ------------------------------------------------------------------ */
/*  Ring-buffer helpers                                               */
/* ------------------------------------------------------------------ */

/*
 * Write `len` bytes from `data` into the ring buffer described by
 * `buf / buf_size / *write_pos / *count`.  Returns the number of
 * bytes actually written (may be less than `len` if the buffer is
 * nearly full).  Uses memcpy for contiguous segments to avoid
 * per-byte overhead.
 */
static size_t ring_buf_write(uint8_t *buf, size_t buf_size,
                              size_t *write_pos, size_t *count,
                              const uint8_t *data, size_t len)
{
    size_t available = buf_size - *count;
    size_t to_write  = (len < available) ? len : available;
    if (to_write == 0) return 0;

    /* First contiguous chunk: from write_pos to end of buffer (or to_write) */
    size_t first_chunk = buf_size - *write_pos;
    if (first_chunk > to_write) first_chunk = to_write;

    memcpy(buf + *write_pos, data, first_chunk);

    /* Second chunk wraps around to the beginning if needed */
    if (to_write > first_chunk) {
        memcpy(buf, data + first_chunk, to_write - first_chunk);
    }

    *write_pos = (*write_pos + to_write) % buf_size;
    *count    += to_write;
    return to_write;
}

/*
 * Read up to `len` bytes from the ring buffer into `data`.
 * Returns the number of bytes actually read.
 */
static size_t ring_buf_read(uint8_t *buf, size_t buf_size,
                             size_t *read_pos, size_t *count,
                             uint8_t *data, size_t len)
{
    size_t to_read = (len < *count) ? len : *count;
    if (to_read == 0) return 0;

    /* First contiguous chunk */
    size_t first_chunk = buf_size - *read_pos;
    if (first_chunk > to_read) first_chunk = to_read;

    memcpy(data, buf + *read_pos, first_chunk);

    /* Second chunk wraps around */
    if (to_read > first_chunk) {
        memcpy(data + first_chunk, buf, to_read - first_chunk);
    }

    *read_pos = (*read_pos + to_read) % buf_size;
    *count   -= to_read;
    return to_read;
}

/*
 * Find the offset of the first occurrence of `target` within the
 * readable portion of a ring buffer.  Returns the byte offset (0
 * based from the logical start of readable data) or -1 if not found.
 */
static ssize_t ring_buf_find(const uint8_t *buf, size_t buf_size,
                              size_t read_pos, size_t count,
                              uint8_t target)
{
    for (size_t i = 0; i < count; i++) {
        if (buf[(read_pos + i) % buf_size] == target) {
            return (ssize_t)i;
        }
    }
    return -1;   /* Not found */
}

/* ------------------------------------------------------------------ */
/*  Signal delivery to a process group                                */
/* ------------------------------------------------------------------ */

/*
 * Send a signal to every process in the foreground process group
 * associated with a PTY's slave side.  In LestraOS we don't have a
 * formal pgid field in struct process, so we deliver to the process
 * whose PID equals slave_pgid and also to any of its children
 * (processes whose parent_pid == slave_pgid).
 */
static void pty_send_signal_group(struct pty_pair *pty, int signum)
{
    if (pty->slave_pgid <= 0) return;

    /* Deliver to the group leader */
    extern int64_t signal_kill(int pid, int sig);
    signal_kill(pty->slave_pgid, signum);

    /* Deliver to children of the group leader */
    extern struct process procs[];
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != PROC_FREE &&
            procs[i].state != PROC_ZOMBIE &&
            procs[i].parent_pid == pty->slave_pgid &&
            procs[i].pid != pty->slave_pgid) {
            signal_kill(procs[i].pid, signum);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Line discipline: echo helper                                      */
/* ------------------------------------------------------------------ */

/*
 * Echo a single character to the master output buffer.
 * Control characters (0..31, 127) are rendered as ^X notation
 * when ECHOCTL is enabled.  DEL (127) renders as ^?.
 */
static void pty_echo_char(struct pty_pair *pty, uint8_t ch)
{
    if (!(pty->term_modes & PTY_ECHO)) return;

    if (pty->term_modes & PTY_ECHOCTL) {
        /* POSIX: control chars other than TAB, NL, CR, BS are echoed
         * as caret notation (^X).  BS(0x08) TAB(0x09) NL(0x0A) and
         * CR(0x0D) are excluded because they have their own terminal
         * behavior (cursor movement, line advancement). */
        if (ch < 32 && ch != '\b' && ch != '\t' && ch != '\n' && ch != '\r') {
            /* Render as ^X: caret + letter offset */
            uint8_t caret = '^';
            uint8_t letter = ch + 64;   /* 0x03 → 'C', 0x1A → 'Z' */
            ring_buf_write(pty->master_output_buf, PTY_BUFFER_SIZE,
                           &pty->master_output_write_pos,
                           &pty->master_output_count,
                           &caret, 1);
            ring_buf_write(pty->master_output_buf, PTY_BUFFER_SIZE,
                           &pty->master_output_write_pos,
                           &pty->master_output_count,
                           &letter, 1);
            return;
        }
        if (ch == 127) {
            /* DEL renders as ^? */
            uint8_t caret = '^';
            uint8_t question = '?';
            ring_buf_write(pty->master_output_buf, PTY_BUFFER_SIZE,
                           &pty->master_output_write_pos,
                           &pty->master_output_count,
                           &caret, 1);
            ring_buf_write(pty->master_output_buf, PTY_BUFFER_SIZE,
                           &pty->master_output_write_pos,
                           &pty->master_output_count,
                           &question, 1);
            return;
        }
    }

    /* Normal printable character or space */
    ring_buf_write(pty->master_output_buf, PTY_BUFFER_SIZE,
                   &pty->master_output_write_pos,
                   &pty->master_output_count,
                   &ch, 1);
}

/*
 * Echo a caret-letter pair for signal characters (^C, ^Z, ^\)
 * followed by \r\n so the terminal shows the signal on its own line.
 */
static void pty_echo_signal(struct pty_pair *pty, uint8_t ctrl_ch)
{
    if (!(pty->term_modes & PTY_ECHO)) return;

    uint8_t seq[4];
    seq[0] = '^';
    seq[1] = ctrl_ch + 64;       /* e.g. 0x03 → 'C' */
    seq[2] = '\r';
    seq[3] = '\n';
    ring_buf_write(pty->master_output_buf, PTY_BUFFER_SIZE,
                   &pty->master_output_write_pos,
                   &pty->master_output_count,
                   seq, 4);
}

/*
 * Echo the visual erase sequence "\b \b" for one character.
 * Called when backspace/delete erases a character from the line buffer.
 */
static void pty_echo_erase(struct pty_pair *pty)
{
    if (!(pty->term_modes & PTY_ECHO)) return;
    if (!(pty->term_modes & PTY_ECHOE)) {
        /* Without ECHOE, just echo the erase character itself */
        uint8_t erase_ch = PTY_VERASE;
        ring_buf_write(pty->master_output_buf, PTY_BUFFER_SIZE,
                       &pty->master_output_write_pos,
                       &pty->master_output_count,
                       &erase_ch, 1);
        return;
    }
    /* ECHOE: visual erase as BS + space + BS */
    uint8_t erase_seq[3] = { 0x08, ' ', 0x08 };
    ring_buf_write(pty->master_output_buf, PTY_BUFFER_SIZE,
                   &pty->master_output_write_pos,
                   &pty->master_output_count,
                   erase_seq, 3);
}

/* ------------------------------------------------------------------ */
/*  Line discipline: input processing (master → slave)                */
/* ------------------------------------------------------------------ */

/*
 * Process one character arriving from the master (SSH client) side.
 * Depending on terminal modes:
 *   - ISIG:   check for Ctrl-C, Ctrl-Z, Ctrl-\  → generate signals
 *   - ICANON: buffer until \n, support Ctrl-D (EOF), backspace, kill
 *   - ECHO:   echo characters back through the master output buffer
 *   - ICRNL:  map CR → NL
 *   - INLCR:  map NL → CR
 *
 * In non-canonical mode, characters are delivered directly to the
 * slave input buffer with no line editing.
 */
static void pty_input_process(struct pty_pair *pty, uint8_t ch)
{
    /* ---- Signal characters (ISIG) ---- */
    if (pty->term_modes & PTY_ISIG) {
        if (ch == PTY_VINTR) {
            /* Ctrl-C → SIGINT */
            pty_echo_signal(pty, ch);
            pty_send_signal_group(pty, SIGINT);
            /* Discard any partial line */
            pty->line_len = 0;
            pty->eof_pending = 0;
            /* Wake anyone blocked reading from slave */
            if (pty->slave_read_waiter > 0) {
                task_unblock_pid(pty->slave_read_waiter);
                pty->slave_read_waiter = 0;
            }
            return;
        }
        if (ch == PTY_VSUSP) {
            /* Ctrl-Z → SIGTSTP */
            pty_echo_signal(pty, ch);
            pty_send_signal_group(pty, SIGTSTP);
            pty->line_len = 0;
            pty->eof_pending = 0;
            if (pty->slave_read_waiter > 0) {
                task_unblock_pid(pty->slave_read_waiter);
                pty->slave_read_waiter = 0;
            }
            return;
        }
        if (ch == PTY_VQUIT) {
            /* Ctrl-\ → SIGQUIT */
            pty_echo_signal(pty, ch);
            pty_send_signal_group(pty, SIGQUIT);
            pty->line_len = 0;
            pty->eof_pending = 0;
            if (pty->slave_read_waiter > 0) {
                task_unblock_pid(pty->slave_read_waiter);
                pty->slave_read_waiter = 0;
            }
            return;
        }
    }

    /* ---- EOF character (Ctrl-D) ---- */
    if (ch == PTY_VEOF && (pty->term_modes & PTY_ICANON)) {
        if (pty->line_len > 0) {
            /* Flush current line buffer as a partial line (no trailing \n).
             * This is the "EOF pushes the current line" behavior. */
            ring_buf_write(pty->slave_input_buf, PTY_BUFFER_SIZE,
                           &pty->slave_input_write_pos,
                           &pty->slave_input_count,
                           pty->line_buf, pty->line_len);
            pty->line_len = 0;

            /* Wake blocked slave reader */
            if (pty->slave_read_waiter > 0) {
                task_unblock_pid(pty->slave_read_waiter);
                pty->slave_read_waiter = 0;
            }
        } else {
            /* Empty line buffer + Ctrl-D → true EOF.
             * Next slave_read() should return 0. */
            pty->eof_pending = 1;

            /* Wake blocked slave reader so it can see the EOF */
            if (pty->slave_read_waiter > 0) {
                task_unblock_pid(pty->slave_read_waiter);
                pty->slave_read_waiter = 0;
            }
        }
        /* Ctrl-D is not echoed */
        return;
    }

    /* ---- Erase character (BS / DEL) ---- */
    if ((ch == PTY_VERASE || ch == PTY_VBS) &&
        (pty->term_modes & PTY_ICANON)) {
        if (pty->line_len > 0) {
            pty->line_len--;
            pty_echo_erase(pty);
        }
        return;
    }

    /* ---- Word erase (Ctrl-W) ---- */
    if (ch == PTY_VWERASE && (pty->term_modes & PTY_ICANON)) {
        /* Erase back to the previous word boundary */
        while (pty->line_len > 0 &&
               pty->line_buf[pty->line_len - 1] == ' ') {
            pty->line_len--;
            pty_echo_erase(pty);
        }
        while (pty->line_len > 0 &&
               pty->line_buf[pty->line_len - 1] != ' ') {
            pty->line_len--;
            pty_echo_erase(pty);
        }
        return;
    }

    /* ---- Kill line (Ctrl-U) ---- */
    if (ch == PTY_VKILL && (pty->term_modes & PTY_ICANON)) {
        if (pty->term_modes & PTY_ECHOK) {
            /* Echo a newline after the kill */
            uint8_t nl_seq[2] = { '\r', '\n' };
            ring_buf_write(pty->master_output_buf, PTY_BUFFER_SIZE,
                           &pty->master_output_write_pos,
                           &pty->master_output_count,
                           nl_seq, 2);
        } else if (pty->term_modes & PTY_ECHO) {
            /* Echo erase for each character */
            for (size_t i = 0; i < pty->line_len; i++) {
                pty_echo_erase(pty);
            }
        }
        pty->line_len = 0;
        return;
    }

    /* ---- CR / NL mapping (before further processing) ---- */
    if (pty->term_modes & PTY_ICRNL) {
        if (ch == '\r') ch = '\n';
    }
    if (pty->term_modes & PTY_INLCR) {
        if (ch == '\n') ch = '\r';
    }

    /* ---- Canonical mode: line buffering ---- */
    if (pty->term_modes & PTY_ICANON) {
        if (ch == '\n') {
            /* Line complete: append \n and flush to slave input buffer */
            if (pty->line_len < PTY_BUFFER_SIZE - 1) {
                pty->line_buf[pty->line_len++] = '\n';
            }
            size_t written = ring_buf_write(pty->slave_input_buf,
                                            PTY_BUFFER_SIZE,
                                            &pty->slave_input_write_pos,
                                            &pty->slave_input_count,
                                            pty->line_buf,
                                            pty->line_len);
            if (written > 0) {
                /* Wake blocked slave reader */
                if (pty->slave_read_waiter > 0) {
                    task_unblock_pid(pty->slave_read_waiter);
                    pty->slave_read_waiter = 0;
                }
            }

            /* Echo the newline as \r\n */
            if (pty->term_modes & PTY_ECHO) {
                uint8_t nl_echo[2] = { '\r', '\n' };
                ring_buf_write(pty->master_output_buf, PTY_BUFFER_SIZE,
                               &pty->master_output_write_pos,
                               &pty->master_output_count,
                               nl_echo, 2);
                /* Wake blocked master reader so SSH client sees the echo */
                if (pty->master_read_waiter > 0) {
                    task_unblock_pid(pty->master_read_waiter);
                    pty->master_read_waiter = 0;
                }
            }

            pty->line_len = 0;
            pty->eof_pending = 0;
            return;
        }

        /* If line buffer is full, force-flush what we have */
        if (pty->line_len >= PTY_BUFFER_SIZE - 1) {
            ring_buf_write(pty->slave_input_buf, PTY_BUFFER_SIZE,
                           &pty->slave_input_write_pos,
                           &pty->slave_input_count,
                           pty->line_buf, pty->line_len);
            if (pty->slave_read_waiter > 0) {
                task_unblock_pid(pty->slave_read_waiter);
                pty->slave_read_waiter = 0;
            }
            pty->line_len = 0;
        }

        /* Append character to line buffer */
        pty->line_buf[pty->line_len++] = ch;

        /* Echo the character */
        pty_echo_char(pty, ch);

        /* Wake master reader for echo data */
        if ((pty->term_modes & PTY_ECHO) && pty->master_read_waiter > 0) {
            task_unblock_pid(pty->master_read_waiter);
            pty->master_read_waiter = 0;
        }
        return;
    }

    /* ---- Non-canonical mode: deliver directly ---- */
    ring_buf_write(pty->slave_input_buf, PTY_BUFFER_SIZE,
                   &pty->slave_input_write_pos,
                   &pty->slave_input_count,
                   &ch, 1);

    /* Echo in non-canonical mode */
    if (pty->term_modes & PTY_ECHO) {
        pty_echo_char(pty, ch);
        if (pty->master_read_waiter > 0) {
            task_unblock_pid(pty->master_read_waiter);
            pty->master_read_waiter = 0;
        }
    }

    /* Wake blocked slave reader */
    if (pty->slave_read_waiter > 0) {
        task_unblock_pid(pty->slave_read_waiter);
        pty->slave_read_waiter = 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Output processing (slave → master)                                */
/* ------------------------------------------------------------------ */

/*
 * Process data written by the slave (shell) side before placing it
 * into the master output buffer.  Applies output transformations
 * controlled by term_modes:
 *   - OPOST | ONLCR: map each \n to \r\n for proper terminal display
 *
 * Returns the total number of source bytes consumed (always equals
 * `count` unless the master output buffer fills up partway).
 */
static size_t pty_output_process(struct pty_pair *pty,
                                  const uint8_t *data, size_t count)
{
    size_t src_consumed = 0;

    if ((pty->term_modes & PTY_OPOST) && (pty->term_modes & PTY_ONLCR)) {
        /* ONLCR: expand each \n to \r\n */
        for (size_t i = 0; i < count; i++) {
            uint8_t ch = data[i];
            if (ch == '\n') {
                /* Check that we have room for 2 bytes */
                size_t avail = PTY_BUFFER_SIZE - pty->master_output_count;
                if (avail < 2) break;
                uint8_t crnl[2] = { '\r', '\n' };
                ring_buf_write(pty->master_output_buf, PTY_BUFFER_SIZE,
                               &pty->master_output_write_pos,
                               &pty->master_output_count,
                               crnl, 2);
            } else if (ch == '\r') {
                /* Pass \r through unchanged (it may already precede \n) */
                size_t avail = PTY_BUFFER_SIZE - pty->master_output_count;
                if (avail < 1) break;
                ring_buf_write(pty->master_output_buf, PTY_BUFFER_SIZE,
                               &pty->master_output_write_pos,
                               &pty->master_output_count,
                               &ch, 1);
            } else {
                size_t avail = PTY_BUFFER_SIZE - pty->master_output_count;
                if (avail < 1) break;
                ring_buf_write(pty->master_output_buf, PTY_BUFFER_SIZE,
                               &pty->master_output_write_pos,
                               &pty->master_output_count,
                               &ch, 1);
            }
            src_consumed++;
        }
    } else if (pty->term_modes & PTY_OPOST) {
        /* OPOST without ONLCR: just copy, but could apply other
         * transforms in the future (OCRNL, ONOCR, etc.). */
        src_consumed = ring_buf_write(pty->master_output_buf,
                                      PTY_BUFFER_SIZE,
                                      &pty->master_output_write_pos,
                                      &pty->master_output_count,
                                      data, count);
    } else {
        /* No output processing: raw copy */
        src_consumed = ring_buf_write(pty->master_output_buf,
                                      PTY_BUFFER_SIZE,
                                      &pty->master_output_write_pos,
                                      &pty->master_output_count,
                                      data, count);
    }

    /* Wake any process blocked reading from the master side */
    if (src_consumed > 0 && pty->master_read_waiter > 0) {
        task_unblock_pid(pty->master_read_waiter);
        pty->master_read_waiter = 0;
    }

    return src_consumed;
}

/* ------------------------------------------------------------------ */
/*  FD-to-pair resolution helpers                                     */
/* ------------------------------------------------------------------ */

/*
 * Map a global PTY FD to its fd_slot structure.
 * Returns NULL if the FD is out of range or not in use.
 */
static struct pty_fd_slot *pty_fd_to_slot(int fd)
{
    if (fd < PTY_FD_BASE || fd >= PTY_FD_MAX) return NULL;
    int idx = fd - PTY_FD_BASE;
    if (!pty_fd_slots[idx].in_use) return NULL;
    return &pty_fd_slots[idx];
}

/*
 * Map a global PTY FD to its pty_pair.
 * Returns NULL if the FD is invalid.
 */
struct pty_pair *pty_get_pair(int fd)
{
    struct pty_fd_slot *slot = pty_fd_to_slot(fd);
    if (!slot) return NULL;
    if (slot->pair_idx < 0 || slot->pair_idx >= PTY_MAX_PAIRS) return NULL;
    return &pty_pairs[slot->pair_idx];
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void pty_init(void)
{
    memset(pty_pairs,   0, sizeof(pty_pairs));
    memset(pty_fd_slots, 0, sizeof(pty_fd_slots));
    pr_info("pty: initialized (%d pairs, FD range %d-%d)\n",
            PTY_MAX_PAIRS, PTY_FD_BASE, PTY_FD_MAX - 1);
}

int pty_create(int *master_fd, int *slave_fd)
{
    if (!master_fd || !slave_fd) return EINVAL;

    /* Find a free PTY pair slot */
    int pair_idx = -1;
    for (int i = 0; i < PTY_MAX_PAIRS; i++) {
        if (!pty_pairs[i].in_use) { pair_idx = i; break; }
    }
    if (pair_idx < 0) {
        pr_warn("pty: no free pair slots\n");
        return EBUSY;
    }

    /* Find two free FD slots: one for master, one for slave */
    int master_slot = -1;
    int slave_slot  = -1;
    for (int i = 0; i < PTY_FD_SLOT_COUNT; i++) {
        if (!pty_fd_slots[i].in_use) {
            if (master_slot < 0) {
                master_slot = i;
            } else if (slave_slot < 0) {
                slave_slot = i;
                break;
            }
        }
    }

    if (master_slot < 0 || slave_slot < 0) {
        pr_warn("pty: no free FD slots\n");
        return EBUSY;
    }

    /* Initialize the pair */
    struct pty_pair *p = &pty_pairs[pair_idx];
    memset(p, 0, sizeof(*p));
    p->in_use       = 1;
    p->master_open   = 1;
    p->slave_open    = 1;
    p->term_modes    = PTY_DEFAULT_MODES;
    p->master_fd     = PTY_FD_BASE + master_slot;
    p->slave_fd      = PTY_FD_BASE + slave_slot;
    p->slave_pgid    = 0;        /* Set later via pty_set_slave_pgid */
    p->eof_pending   = 0;
    p->line_len      = 0;

    /* Default window size: 80 × 24 (classic terminal) */
    p->winsize.ws_row    = 24;
    p->winsize.ws_col    = 80;
    p->winsize.ws_xpixel = 0;
    p->winsize.ws_ypixel = 0;

    /* Initialize FD slots */
    pty_fd_slots[master_slot].in_use    = 1;
    pty_fd_slots[master_slot].pair_idx  = pair_idx;
    pty_fd_slots[master_slot].is_master = 1;

    pty_fd_slots[slave_slot].in_use    = 1;
    pty_fd_slots[slave_slot].pair_idx  = pair_idx;
    pty_fd_slots[slave_slot].is_master = 0;

    *master_fd = p->master_fd;
    *slave_fd  = p->slave_fd;

    pr_info("pty: pair %d created (master=%d, slave=%d)\n",
            pair_idx, *master_fd, *slave_fd);
    return EOK;
}

int pty_master_write(int fd, const void *buf, size_t count)
{
    struct pty_fd_slot *slot = pty_fd_to_slot(fd);
    if (!slot || !slot->is_master) return EINVAL;

    struct pty_pair *pty = &pty_pairs[slot->pair_idx];
    if (!pty->in_use || !pty->slave_open) return EIO;

    const uint8_t *data = (const uint8_t *)buf;
    size_t processed = 0;

    /* Feed each byte through the input line discipline.
     * This may block if intermediate buffers fill, but in practice
     * the line discipline writes small chunks into the slave input
     * buffer and echo buffer. */
    while (processed < count) {
        pty_input_process(pty, data[processed]);
        processed++;

        /* If the slave input buffer is getting full, we need to
         * slow down.  In canonical mode this is unlikely because
         * the line buffer absorbs characters first.  In non-canonical
         * mode we check directly. */
        if (!(pty->term_modes & PTY_ICANON)) {
            if (pty->slave_input_count >= PTY_BUFFER_SIZE - 1) {
                /* Slave input buffer nearly full — stop accepting.
                 * The caller should retry after the slave reads some data. */
                break;
            }
        }
    }

    return (int)processed;
}

ssize_t pty_master_read(int fd, void *buf, size_t count)
{
    struct pty_fd_slot *slot = pty_fd_to_slot(fd);
    if (!slot || !slot->is_master) return EINVAL;

    struct pty_pair *pty = &pty_pairs[slot->pair_idx];
    if (!pty->in_use) return EIO;

    /* Block until data is available or slave side closes */
    while (pty->master_output_count == 0) {
        if (!pty->slave_open) {
            /* Slave closed and no more data → EOF for master reader */
            return 0;
        }
        /* No data yet; block the calling task */
        extern int proc_getpid(void);
        pty->master_read_waiter = proc_getpid();
        task_block();
        pty->master_read_waiter = 0;
    }

    /* Read available data from the master output ring buffer */
    size_t to_read = (count < pty->master_output_count)
                     ? count : pty->master_output_count;
    size_t actually_read = ring_buf_read(pty->master_output_buf,
                                          PTY_BUFFER_SIZE,
                                          &pty->master_output_read_pos,
                                          &pty->master_output_count,
                                          (uint8_t *)buf, to_read);

    /* If we freed space, wake any task blocked writing from slave side */
    if (actually_read > 0 && pty->slave_write_waiter > 0) {
        task_unblock_pid(pty->slave_write_waiter);
        pty->slave_write_waiter = 0;
    }

    return (ssize_t)actually_read;
}

int pty_slave_write(int fd, const void *buf, size_t count)
{
    struct pty_fd_slot *slot = pty_fd_to_slot(fd);
    if (!slot || slot->is_master) return EINVAL;

    struct pty_pair *pty = &pty_pairs[slot->pair_idx];
    if (!pty->in_use || !pty->master_open) return EIO;

    const uint8_t *data = (const uint8_t *)buf;
    size_t total_consumed = 0;

    /* Process the data through output discipline and write into
     * the master output buffer.  If the buffer fills up, we may
     * not be able to process all bytes in one call. */
    while (total_consumed < count) {
        size_t remaining = count - total_consumed;
        size_t consumed = pty_output_process(pty,
                                              data + total_consumed,
                                              remaining);
        if (consumed == 0) {
            /* Master output buffer is full — block until master
             * reads some data and frees space. */
            if (!pty->master_open) return EIO;   /* Broken: master closed */

            extern int proc_getpid(void);
            pty->slave_write_waiter = proc_getpid();
            task_block();
            pty->slave_write_waiter = 0;
            /* After unblocking, retry writing the remaining data */
            continue;
        }
        total_consumed += consumed;
    }

    return (int)total_consumed;
}

ssize_t pty_slave_read(int fd, void *buf, size_t count)
{
    struct pty_fd_slot *slot = pty_fd_to_slot(fd);
    if (!slot || slot->is_master) return EINVAL;

    struct pty_pair *pty = &pty_pairs[slot->pair_idx];
    if (!pty->in_use) return EIO;

    /* ---- Canonical mode: deliver one complete line ---- */
    if (pty->term_modes & PTY_ICANON) {
        /* Wait until there is at least one complete line or EOF */
        while (pty->slave_input_count == 0 && !pty->eof_pending) {
            if (!pty->master_open) {
                /* Master closed — deliver EOF */
                return 0;
            }
            /* No data yet; block */
            extern int proc_getpid(void);
            pty->slave_read_waiter = proc_getpid();
            task_block();
            pty->slave_read_waiter = 0;
        }

        /* Check for EOF condition */
        if (pty->eof_pending && pty->slave_input_count == 0) {
            /* Ctrl-D on empty line → true EOF */
            pty->eof_pending = 0;
            return 0;
        }

        /* Find the first newline in the slave input buffer.
         * In canonical mode, we always deliver up to and including
         * the first \n (one line per read call).  If no \n is found,
         * the data must be from a Ctrl-D flush — deliver it all. */
        ssize_t nl_offset = ring_buf_find(pty->slave_input_buf,
                                           PTY_BUFFER_SIZE,
                                           pty->slave_input_read_pos,
                                           pty->slave_input_count,
                                           '\n');

        size_t to_deliver;
        if (nl_offset >= 0) {
            /* Deliver up to and including the \n */
            to_deliver = (size_t)nl_offset + 1;
        } else {
            /* No newline found — must be a Ctrl-D partial line.
             * Deliver all available data. */
            to_deliver = pty->slave_input_count;
        }

        /* Limit to the caller's requested count */
        if (to_deliver > count) to_deliver = count;

        size_t actually_read = ring_buf_read(pty->slave_input_buf,
                                              PTY_BUFFER_SIZE,
                                              &pty->slave_input_read_pos,
                                              &pty->slave_input_count,
                                              (uint8_t *)buf, to_deliver);

        /* Wake anyone blocked writing from master side */
        if (actually_read > 0 && pty->master_write_waiter > 0) {
            task_unblock_pid(pty->master_write_waiter);
            pty->master_write_waiter = 0;
        }

        return (ssize_t)actually_read;
    }

    /* ---- Non-canonical mode: deliver whatever is available ---- */
    while (pty->slave_input_count == 0) {
        if (!pty->master_open) {
            return 0;   /* Master closed → EOF */
        }
        /* No data; block */
        extern int proc_getpid(void);
        pty->slave_read_waiter = proc_getpid();
        task_block();
        pty->slave_read_waiter = 0;
    }

    size_t to_read = (count < pty->slave_input_count)
                     ? count : pty->slave_input_count;
    size_t actually_read = ring_buf_read(pty->slave_input_buf,
                                          PTY_BUFFER_SIZE,
                                          &pty->slave_input_read_pos,
                                          &pty->slave_input_count,
                                          (uint8_t *)buf, to_read);

    if (actually_read > 0 && pty->master_write_waiter > 0) {
        task_unblock_pid(pty->master_write_waiter);
        pty->master_write_waiter = 0;
    }

    return (ssize_t)actually_read;
}

int pty_set_winsize(int fd, const struct winsize *ws)
{
    struct pty_pair *pty = pty_get_pair(fd);
    if (!pty || !ws) return EINVAL;

    pty->winsize.ws_row    = ws->ws_row;
    pty->winsize.ws_col    = ws->ws_col;
    pty->winsize.ws_xpixel = ws->ws_xpixel;
    pty->winsize.ws_ypixel = ws->ws_ypixel;

    pr_debug("pty: winsize set to %u×%u\n",
             (unsigned)ws->ws_col, (unsigned)ws->ws_row);
    return EOK;
}

int pty_close(int fd)
{
    struct pty_fd_slot *slot = pty_fd_to_slot(fd);
    if (!slot) return EINVAL;

    struct pty_pair *pty = &pty_pairs[slot->pair_idx];
    if (!pty->in_use) return EINVAL;

    /* Mark the appropriate side as closed */
    if (slot->is_master) {
        pty->master_open = 0;
        /* Wake blocked slave reader (it will see EOF) */
        if (pty->slave_read_waiter > 0) {
            task_unblock_pid(pty->slave_read_waiter);
            pty->slave_read_waiter = 0;
        }
        /* Wake blocked slave writer (it will get EIO) */
        if (pty->slave_write_waiter > 0) {
            task_unblock_pid(pty->slave_write_waiter);
            pty->slave_write_waiter = 0;
        }
    } else {
        pty->slave_open = 0;
        /* Wake blocked master reader (it will see EOF when buffer drains) */
        if (pty->master_read_waiter > 0) {
            task_unblock_pid(pty->master_read_waiter);
            pty->master_read_waiter = 0;
        }
        /* Wake blocked master writer (it will get EIO) */
        if (pty->master_write_waiter > 0) {
            task_unblock_pid(pty->master_write_waiter);
            pty->master_write_waiter = 0;
        }
        /* Discard any remaining line buffer (shell closed — no consumer) */
        pty->line_len    = 0;
        pty->eof_pending = 0;
    }

    /* Release the FD slot */
    slot->in_use = 0;

    /* If both sides are closed, release the pair */
    if (!pty->master_open && !pty->slave_open) {
        pty->in_use     = 0;
        pty->master_fd  = 0;
        pty->slave_fd   = 0;
        pty->slave_pgid = 0;
        pr_info("pty: pair %d released (both sides closed)\n",
                slot->pair_idx);
    }

    return EOK;
}

int pty_is_pty_fd(int fd)
{
    if (fd < PTY_FD_BASE || fd >= PTY_FD_MAX) return 0;
    return pty_fd_slots[fd - PTY_FD_BASE].in_use;
}

int pty_set_slave_pgid(int fd, int pgid)
{
    struct pty_pair *pty = pty_get_pair(fd);
    if (!pty) return EINVAL;
    pty->slave_pgid = pgid;
    return EOK;
}

int pty_set_modes(int fd, uint32_t modes)
{
    struct pty_pair *pty = pty_get_pair(fd);
    if (!pty) return EINVAL;

    /* When switching from canonical to non-canonical mode, flush
     * any partial line buffer to the slave input buffer so the
     * reader can consume it. */
    if ((pty->term_modes & PTY_ICANON) && !(modes & PTY_ICANON)) {
        if (pty->line_len > 0) {
            ring_buf_write(pty->slave_input_buf, PTY_BUFFER_SIZE,
                           &pty->slave_input_write_pos,
                           &pty->slave_input_count,
                           pty->line_buf, pty->line_len);
            pty->line_len = 0;
            /* Wake blocked slave reader */
            if (pty->slave_read_waiter > 0) {
                task_unblock_pid(pty->slave_read_waiter);
                pty->slave_read_waiter = 0;
            }
        }
        pty->eof_pending = 0;
    }

    /* When switching to canonical mode from non-canonical, discard
     * any unprocessed raw input that doesn't form a complete line.
     * (This matches POSIX behavior: existing raw data is not
     * re-processed through canonical rules.) */

    pty->term_modes = modes;
    return EOK;
}

uint32_t pty_get_modes(int fd)
{
    struct pty_pair *pty = pty_get_pair(fd);
    if (!pty) return 0;
    return pty->term_modes;
}
