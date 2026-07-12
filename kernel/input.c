/*
 * Lestra OS - Input subsystem implementation (PS/2 mouse + event queue)
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * PS/2 mouse initialization sequence:
 *   1. Enable aux port (outb 0xA8 to 0x64)
 *   2. Read config byte (outb 0x20 to 0x64, read 0x60)
 *   3. Set bit 1 (enable IRQ12), write back (outb 0x60 to 0x64, outb config to 0x60)
 *   4. Enable packet streaming (outb 0xD4 to 0x64, outb 0xF4 to 0x60)
 *
 * IRQ12 handler reads 3 bytes from 0x60:
 *   byte 0: flags (bit 0=left, bit 1=right, bit 2=middle, bit 6=X overflow, bit 7=Y overflow, bit 5=Y sign, bit 4=X sign)
 *   byte 1: X delta (sign-extended if bit 4 set)
 *   byte 2: Y delta (sign-extended if bit 5 set, INVERTED because screen Y is downward)
 *
 * Mouse position is clamped to [0, fb_w-1] × [0, fb_h-1].
 */

#include <lestra/types.h>
#include <lestra/input.h>
#include <lestra/fb.h>
#include <lestra/irq.h>
#include <lestra/idt.h>
#include <lestra/printk.h>
#include <lestra/keyboard.h>

/* ----- event ring buffer ----- */
#define INPUT_BUF_SIZE 128
static struct event input_buf[INPUT_BUF_SIZE];
static volatile int input_head = 0;
static volatile int input_tail = 0;

static void input_push(struct event* e) {
    int next = (input_head + 1) % INPUT_BUF_SIZE;
    if (next == input_tail) return;  /* buffer full, drop event */
    input_buf[input_head] = *e;
    input_head = next;
}

int input_poll(struct event* e) {
    if (input_head == input_tail) return 0;
    *e = input_buf[input_tail];
    input_tail = (input_tail + 1) % INPUT_BUF_SIZE;
    return 1;
}

/* ----- mouse state ----- */
static volatile int mouse_x = 512;   /* start center-screen */
static volatile int mouse_y = 384;
static volatile uint8_t mouse_buttons = 0;
static uint8_t mouse_prev_buttons = 0;

/* Mouse packet parsing state */
static uint8_t mouse_packet[3];
static int mouse_packet_idx = 0;

void input_get_mouse_pos(int* x, int* y) {
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
}

uint8_t input_get_mouse_buttons(void) {
    return mouse_buttons;
}

/* ----- PS/2 controller helpers ----- */
static inline void ps2_wait_write(void) {
    /* Wait until input buffer is empty (controller can accept data) */
    int timeout = 10000;
    while ((inb(0x64) & 0x02) && timeout-- > 0);
}

static inline void ps2_wait_read(void) {
    /* Wait until output buffer is full (data available to read) */
    int timeout = 10000;
    while (!(inb(0x64) & 0x01) && timeout-- > 0);
}

static inline void ps2_mouse_write(uint8_t data) {
    ps2_wait_write();
    outb(0x64, 0xD4);       /* next byte goes to aux (mouse) port */
    ps2_wait_write();
    outb(0x60, data);
}

static inline uint8_t ps2_mouse_read(void) {
    ps2_wait_read();
    return inb(0x60);
}

/* ----- mouse IRQ handler ----- */
static void mouse_irq_handler(struct interrupt_frame* frame) {
    (void)frame;
    uint8_t data = inb(0x60);

    if (mouse_packet_idx == 0 && !(data & 0x08)) {
        /* First byte must have bit 3 set (sync bit). If not, resync. */
        return;
    }

    mouse_packet[mouse_packet_idx++] = data;
    if (mouse_packet_idx < 3) return;
    mouse_packet_idx = 0;

    /* Parse the 3-byte packet */
    uint8_t flags = mouse_packet[0];
    int dx = (int)mouse_packet[1];
    int dy = (int)mouse_packet[2];

    /* Sign-extend if negative */
    if (flags & 0x10) dx |= 0xFFFFFF00;
    if (flags & 0x20) dy |= 0xFFFFFF00;

    /* Y is inverted: mouse up = negative dy, but screen Y goes down */
    dy = -dy;

    /* Update position */
    mouse_x += dx;
    mouse_y += dy;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (fb_available) {
        if (mouse_x >= (int)fb_w) mouse_x = fb_w - 1;
        if (mouse_y >= (int)fb_h) mouse_y = fb_h - 1;
    }

    /* Update buttons */
    mouse_buttons = flags & 0x07;  /* left, right, middle */

    /* Push mouse-move event */
    struct event ev;
    ev.type = EV_MOUSE_MOVE;
    ev.mouse.x = mouse_x;
    ev.mouse.y = mouse_y;
    ev.mouse.buttons = mouse_buttons;
    input_push(&ev);

    /* Check for button press/release transitions */
    uint8_t pressed = mouse_buttons & ~mouse_prev_buttons;
    uint8_t released = ~mouse_buttons & mouse_prev_buttons;

    if (pressed & MOUSE_BTN_LEFT) {
        ev.type = EV_MOUSE_DOWN;
        ev.mouse.buttons = MOUSE_BTN_LEFT;
        input_push(&ev);
    }
    if (released & MOUSE_BTN_LEFT) {
        ev.type = EV_MOUSE_UP;
        ev.mouse.buttons = MOUSE_BTN_LEFT;
        input_push(&ev);
    }
    if (pressed & MOUSE_BTN_RIGHT) {
        ev.type = EV_MOUSE_DOWN;
        ev.mouse.buttons = MOUSE_BTN_RIGHT;
        input_push(&ev);
    }
    if (released & MOUSE_BTN_RIGHT) {
        ev.type = EV_MOUSE_UP;
        ev.mouse.buttons = MOUSE_BTN_RIGHT;
        input_push(&ev);
    }

    mouse_prev_buttons = mouse_buttons;
}

/* ----- keyboard hook (pushes key events to input queue) -----
 * The existing keyboard.c reads scancodes from the PS/2 keyboard and
 * puts ASCII chars into a ring buffer for the text shell. We hook into
 * the same IRQ to also push structured events for the GUI compositor.
 * This avoids needing a separate IRQ handler for the keyboard. */
static void (*prev_kb_handler)(uint8_t scancode, char ascii) = NULL;

static uint8_t key_mods = 0;   /* current modifier state */

static void input_kb_hook(uint8_t scancode, char ascii) {
    /* Track modifier keys */
    /* Scancode set 1: 0x2A=LShift, 0x36=RShift, 0x1D=Ctrl, 0x38=Alt, 0x5B=LWin */
    uint8_t make = !(scancode & 0x80);  /* bit 7 set = break (release) */
    uint8_t code = scancode & 0x7F;

    if (code == 0x2A || code == 0x36) {
        if (make) key_mods |= MOD_SHIFT; else key_mods &= ~MOD_SHIFT;
    } else if (code == 0x1D) {
        if (make) key_mods |= MOD_CTRL; else key_mods &= ~MOD_CTRL;
    } else if (code == 0x38) {
        if (make) key_mods |= MOD_ALT; else key_mods &= ~MOD_ALT;
    }

    /* Push key event for GUI */
    struct event ev;
    ev.type = make ? EV_KEY_DOWN : EV_KEY_UP;
    ev.key.scancode = scancode;
    ev.key.ascii = ascii;
    ev.key.mods = key_mods;
    input_push(&ev);

    /* Chain to previous handler if any */
    if (prev_kb_handler) prev_kb_handler(scancode, ascii);
}

/* ----- public API ----- */
void input_init(void) {
    /* Enable the aux (mouse) port on the PS/2 controller */
    ps2_wait_write();
    outb(0x64, 0xA8);   /* enable aux port */

    /* Read current config, enable IRQ12 (bit 1), write back */
    ps2_wait_write();
    outb(0x64, 0x20);   /* read config */
    ps2_wait_read();
    uint8_t config = inb(0x60);
    config |= 0x02;     /* enable IRQ12 */
    config &= ~0x20;    /* disable mouse clock gating */
    ps2_wait_write();
    outb(0x64, 0x60);   /* write config */
    ps2_wait_write();
    outb(0x60, config);

    /* Reset the mouse */
    ps2_mouse_write(0xFF);  /* reset */
    (void)ps2_mouse_read(); /* ACK */
    (void)ps2_mouse_read(); /* self-test pass */
    (void)ps2_mouse_read(); /* mouse ID */

    /* Enable packet streaming */
    ps2_mouse_write(0xF4);  /* enable streaming */
    (void)ps2_mouse_read(); /* ACK */

    /* Register IRQ12 handler */
    register_irq_handler(12, mouse_irq_handler);
    irq_enable(12);

    /* Hook into the keyboard handler to push key events for the GUI.
     * keyboard.c's set_handler lets us register a callback. */
    extern void keyboard_set_handler(void (*handler)(uint8_t, char));
    prev_kb_handler = NULL;  /* keyboard.c doesn't chain; we capture directly */
    keyboard_set_handler(input_kb_hook);

    /* Set initial mouse position to center of screen */
    if (fb_available) {
        mouse_x = fb_w / 2;
        mouse_y = fb_h / 2;
    }

    pr_info("input: PS/2 mouse initialized (IRQ12), cursor at (%u,%u)\n",
            (unsigned)mouse_x, (unsigned)mouse_y);
}
