/*
 * Lestra OS - PS/2 Mouse Driver
 * Copyright (c) 2026 lestramk.org
 *
 * Standalone PS/2 mouse driver that handles:
 *   - Aux port initialization on the 8042 PS/2 controller
 *   - Mouse reset, sample rate, resolution, and scaling setup
 *   - IRQ12 handler for mouse data packets
 *   - 3-byte packet parsing (flags + X/Y deltas)
 *   - Ring buffer of mouse_event structs
 *   - Optional callback notification for real-time event routing
 *
 * The driver is wired into the input subsystem via mouse_set_callback(),
 * so input.c receives each event immediately from the IRQ handler
 * without needing to poll.
 *
 * PS/2 mouse protocol (3-byte packet):
 *   Byte 0: Bit 7 = Y overflow, Bit 6 = X overflow,
 *           Bit 5 = Y sign (9th bit of Y delta),
 *           Bit 4 = X sign (9th bit of X delta),
 *           Bit 3 = always 1 (sync bit for packet alignment),
 *           Bit 2 = middle button, Bit 1 = right button,
 *           Bit 0 = left button
 *   Byte 1: X delta (8-bit, sign-extended using bit 4 of byte 0)
 *   Byte 2: Y delta (8-bit, sign-extended using bit 5 of byte 0)
 *           NOTE: screen Y goes downward, so we negate dy.
 */

#include <lestra/types.h>
#include <lestra/mouse.h>
#include <lestra/irq.h>
#include <lestra/idt.h>
#include <lestra/printk.h>
#include <lestra/fb.h>
#include <lestra/entropy.h>

/* ----- PS/2 controller I/O ports ----- */
#define PS2_DATA_PORT    0x60   /* Data port (read/write) */
#define PS2_STATUS_PORT  0x64   /* Status register (read) */
#define PS2_CMD_PORT     0x64   /* Command port (write) */

/* PS/2 controller commands */
#define PS2_CMD_READ_CONFIG   0x20   /* Read controller config byte */
#define PS2_CMD_WRITE_CONFIG  0x60   /* Write controller config byte */
#define PS2_CMD_DISABLE_AUX   0xA7   /* Disable aux (mouse) port */
#define PS2_CMD_ENABLE_AUX    0xA8   /* Enable aux (mouse) port */
#define PS2_CMD_SEND_TO_AUX   0xD4   /* Next byte goes to aux device */
#define PS2_CMD_SELF_TEST     0xAA   /* Controller self-test */
#define PS2_CMD_KB_TEST       0xAB   /* Keyboard port test */
#define PS2_CMD_AUX_TEST      0xA9   /* Aux port test */

/* PS/2 mouse commands (sent via 0xD4 prefix to cmd port, then data to 0x60) */
#define MOUSE_CMD_RESET       0xFF   /* Reset mouse */
#define MOUSE_CMD_SET_RATE    0xF3   /* Set sample rate */
#define MOUSE_CMD_ENABLE      0xF4   /* Enable packet streaming */
#define MOUSE_CMD_DISABLE     0xF5   /* Disable packet streaming */
#define MOUSE_CMD_SET_DEFAULT 0xF6   /* Set default settings */
#define MOUSE_CMD_SET_RES     0xE8   /* Set resolution */
#define MOUSE_CMD_SET_SCALE21 0xE7   /* Set scaling 2:1 */
#define MOUSE_CMD_SET_SCALE11 0xE6   /* Set scaling 1:1 */

/* Mouse response bytes */
#define MOUSE_ACK             0xFA   /* Command acknowledged */
#define MOUSE_SELF_TEST_OK    0xAA   /* Self-test passed */
#define MOUSE_ID_STANDARD     0x00   /* Standard 3-byte packet mouse */

/* ----- Ring buffer for mouse events ----- */
#define MOUSE_BUF_SIZE  128

static struct mouse_event mouse_buf[MOUSE_BUF_SIZE];
static volatile int mouse_buf_head = 0;
static volatile int mouse_buf_tail = 0;

/* ----- Mouse state ----- */
static volatile int mouse_x = 0;
static volatile int mouse_y = 0;
static volatile uint8_t mouse_buttons = 0;
static int mouse_initialized = 0;

/* Framebuffer bounds for position clamping */
static int fb_width  = 1024;
static int fb_height = 768;

/* Callback for event notification (set by input subsystem).
 * Invoked directly from the IRQ handler — any operation in the callback
 * must be safe in interrupt context (no spinlocks, no blocking I/O).
 * If the callback needs to do heavy work, it should queue a deferred
 * task and return immediately. */
static mouse_event_callback_t event_callback = NULL;

/* ----- PS/2 controller helpers ----- */

/* Wait until the PS/2 controller input buffer is empty (ready to send). */
static inline void ps2_wait_write(void) {
    int timeout = 100000;
    while ((inb(PS2_STATUS_PORT) & 0x02) && timeout-- > 0);
}

/* Wait until the PS/2 controller output buffer is full (data available). */
static inline void ps2_wait_read(void) {
    int timeout = 100000;
    while (!(inb(PS2_STATUS_PORT) & 0x01) && timeout-- > 0);
}

/* Send a command byte to the PS/2 controller command port (0x64). */
static inline void ps2_send_cmd(uint8_t cmd) {
    ps2_wait_write();
    outb(PS2_CMD_PORT, cmd);
}

/* Write a data byte to the PS/2 data port (0x60). */
static inline void ps2_write_data(uint8_t data) {
    ps2_wait_write();
    outb(PS2_DATA_PORT, data);
}

/* Read a data byte from the PS/2 data port (0x60). */
static inline uint8_t ps2_read_data(void) {
    ps2_wait_read();
    return inb(PS2_DATA_PORT);
}

/* Send a command to the aux (mouse) device:
 * 1. Tell controller next byte goes to aux (0xD4 to 0x64)
 * 2. Send command byte to data port (0x60)
 * 3. Read ACK response (0xFA) */
static void mouse_send_cmd(uint8_t cmd) {
    ps2_send_cmd(PS2_CMD_SEND_TO_AUX);
    ps2_write_data(cmd);
    /* Read ACK — mouse always ACKs valid commands */
    uint8_t ack = ps2_read_data();
    if (ack != MOUSE_ACK) {
        pr_warn("mouse: command 0x%x ACK mismatch (got 0x%x)\n", cmd, ack);
    }
}

/* Send a command with an argument byte (e.g. set sample rate, set resolution).
 * The mouse ACKs the command byte, then ACKs the argument byte. */
static void mouse_send_cmd_arg(uint8_t cmd, uint8_t arg) {
    ps2_send_cmd(PS2_CMD_SEND_TO_AUX);
    ps2_write_data(cmd);
    (void)ps2_read_data();  /* ACK for command */

    ps2_send_cmd(PS2_CMD_SEND_TO_AUX);
    ps2_write_data(arg);
    (void)ps2_read_data();  /* ACK for argument */
}

/* ----- Packet parsing state ----- */
static uint8_t  mouse_packet[4];   /* Buffer for incoming packet bytes */
static int      mouse_pkt_idx = 0;  /* Current byte index in packet */

/* ----- IRQ12 handler ----- */

static void mouse_irq_handler(struct interrupt_frame* frame) {
    (void)frame;

    /* KE-16: Feed mouse packet event timing into entropy pool */
    uint8_t data = inb(PS2_DATA_PORT);
    uint64_t now = rdtsc();
    entropy_mix_irq(12, now);

    /* Synchronization: the first byte of a 3-byte packet must have
     * bit 3 set. If we're at byte 0 and bit 3 is clear, we lost
     * sync — discard and resynchronize. */
    if (mouse_pkt_idx == 0 && !(data & 0x08)) {
        return;
    }

    mouse_packet[mouse_pkt_idx++] = data;
    if (mouse_pkt_idx < 3) return;

    /* Complete 3-byte packet received. Reset index for next packet. */
    mouse_pkt_idx = 0;

    /* Parse the packet:
     * byte 0: flags
     * byte 1: X delta
     * byte 2: Y delta */
    uint8_t flags = mouse_packet[0];
    int dx = (int)mouse_packet[1];
    int dy = (int)mouse_packet[2];

    /* Sign-extend X delta if bit 4 (X sign) is set */
    if (flags & 0x10) dx |= (int)0xFFFFFF00;

    /* Sign-extend Y delta if bit 5 (Y sign) is set */
    if (flags & 0x20) dy |= (int)0xFFFFFF00;

    /* Y is inverted: PS/2 reports positive Y as "up" (away from user),
     * but screen Y increases downward. Negate to match screen coords. */
    dy = -dy;

    /* Ignore overflow packets (X or Y overflow bits set) */
    if (flags & 0x40 || flags & 0x80) {
        return;
    }

    /* Update absolute position (clamped to framebuffer bounds).
     * Use 64-bit intermediates to avoid signed integer overflow UB. */
    {
        long new_x = (long)mouse_x + dx;
        long new_y = (long)mouse_y + dy;
        if (new_x < 0)           new_x = 0;
        if (new_y < 0)           new_y = 0;
        if (new_x >= fb_width)   new_x = fb_width - 1;
        if (new_y >= fb_height)  new_y = fb_height - 1;
        mouse_x = (int)new_x;
        mouse_y = (int)new_y;
    }

    /* Update button state */
    mouse_buttons = flags & 0x07;  /* Left=bit0, Right=bit1, Middle=bit2 */

    /* Create the event */
    struct mouse_event ev;
    ev.dx      = dx;
    ev.dy      = dy;
    ev.buttons = mouse_buttons;
    ev.x       = mouse_x;
    ev.y       = mouse_y;

    /* Push into ring buffer */
    int next = (mouse_buf_head + 1) % MOUSE_BUF_SIZE;
    if (next != mouse_buf_tail) {
        mouse_buf[mouse_buf_head] = ev;
        mouse_buf_head = next;
    }

    /* Notify callback (if registered) — runs in IRQ context */
    if (event_callback) {
        event_callback(&ev);
    }
}

/* ----- Public API ----- */

int mouse_init(void) {
    pr_info("mouse: initializing PS/2 mouse driver\n");

    /* Step 1: Enable aux (mouse) port on the PS/2 controller */
    ps2_send_cmd(PS2_CMD_ENABLE_AUX);
    io_wait();

    /* Step 2: Read current controller config byte, enable IRQ12 (bit 1),
     * and disable mouse clock gating (clear bit 5), then write back. */
    ps2_send_cmd(PS2_CMD_READ_CONFIG);
    uint8_t config = ps2_read_data();
    config |= 0x02;    /* Enable IRQ12 (aux device interrupt) */
    config &= ~0x20;   /* Disable mouse clock gating (ensure clock runs) */
    config |= 0x01;    /* Ensure IRQ1 (keyboard) is also enabled */
    ps2_send_cmd(PS2_CMD_WRITE_CONFIG);
    ps2_write_data(config);
    io_wait();

    /* Step 3: Reset the mouse.
     * Response sequence: ACK (0xFA), Self-test OK (0xAA), Mouse ID (0x00). */
    ps2_send_cmd(PS2_CMD_SEND_TO_AUX);
    ps2_write_data(MOUSE_CMD_RESET);
    io_wait();

    /* Read reset response: ACK, self-test pass, mouse ID */
    uint8_t ack     = ps2_read_data();   /* Should be 0xFA (ACK) */
    uint8_t self_ok = ps2_read_data();   /* Should be 0xAA (self-test OK) */
    uint8_t mouse_id = ps2_read_data();  /* Should be 0x00 (standard 3-byte mouse) */

    if (ack != MOUSE_ACK) {
        pr_warn("mouse: reset ACK missing (got 0x%x)\n", ack);
    }
    if (self_ok != MOUSE_SELF_TEST_OK) {
        pr_warn("mouse: self-test failed (got 0x%x)\n", self_ok);
    }
    pr_info("mouse: reset response: ACK=0x%x, self-test=0x%x, ID=0x%x\n",
            ack, self_ok, mouse_id);

    /* Step 4: Set sample rate to 100 samples/second.
     * Command: 0xF3 (set rate), arg: 100 (decimal 100 = 0x64). */
    mouse_send_cmd_arg(MOUSE_CMD_SET_RATE, 100);
    io_wait();

    /* Step 5: Set resolution to 4 counts/mm (highest standard setting).
     * Command: 0xE8 (set resolution), arg: 3 (0=1, 1=2, 2=4, 3=8 counts/mm). */
    mouse_send_cmd_arg(MOUSE_CMD_SET_RES, 3);
    io_wait();

    /* Step 6: Set scaling to 1:1 (no scaling).
     * Command: 0xE6 (set scaling 1:1). */
    mouse_send_cmd(MOUSE_CMD_SET_SCALE11);
    io_wait();

    /* Step 7: Enable packet streaming (mouse starts sending data).
     * Command: 0xF4 (enable streaming). */
    mouse_send_cmd(MOUSE_CMD_ENABLE);
    io_wait();

    /* Step 8: Register IRQ12 handler and enable the interrupt */
    register_irq_handler(12, mouse_irq_handler);
    irq_enable(12);

    /* Set initial position to center of framebuffer */
    if (fb_available) {
        mouse_x = fb_w / 2;
        mouse_y = fb_h / 2;
        fb_width  = (int)fb_w;
        fb_height = (int)fb_h;
    } else {
        mouse_x = fb_width / 2;
        mouse_y = fb_height / 2;
    }

    mouse_buf_head = 0;
    mouse_buf_tail = 0;
    mouse_pkt_idx  = 0;
    mouse_buttons  = 0;
    mouse_initialized = 1;

    pr_info("mouse: PS/2 mouse initialized (IRQ12), cursor at (%d,%d)\n",
            mouse_x, mouse_y);
    return 0;
}

int mouse_has_event(void) {
    return (mouse_buf_head != mouse_buf_tail);
}

int mouse_get_event(struct mouse_event* ev) {
    if (mouse_buf_head == mouse_buf_tail) return 0;
    *ev = mouse_buf[mouse_buf_tail];
    mouse_buf_tail = (mouse_buf_tail + 1) % MOUSE_BUF_SIZE;
    return 1;
}

void mouse_set_callback(mouse_event_callback_t cb) {
    event_callback = cb;
}

void mouse_get_pos(int* x, int* y) {
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
}

uint8_t mouse_get_buttons(void) {
    return mouse_buttons;
}

void mouse_set_fb_bounds(int width, int height) {
    fb_width  = width;
    fb_height = height;
    /* Recenter cursor if it's outside the new bounds */
    if (mouse_x >= width)  mouse_x = width - 1;
    if (mouse_y >= height) mouse_y = height - 1;
}
