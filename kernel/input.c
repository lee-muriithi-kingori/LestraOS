/*
 * Lestra OS - Input subsystem implementation (mouse + keyboard event queue)
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * The PS/2 mouse driver is now a separate module (drivers/char/mouse.c)
 * that handles aux port initialization, IRQ12 processing, and packet
 * parsing. This module wires the mouse driver into the unified event
 * queue via a callback that converts mouse_event → input event.
 *
 * The keyboard is still handled by keyboard.c, and we hook into its
 * scancode handler to also push key events for the GUI compositor.
 *
 * The compositor polls events via input_poll().
 */

#include <lestra/types.h>
#include <lestra/input.h>
#include <lestra/mouse.h>
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

/* ----- mouse state (for button transition detection) ----- */
static uint8_t prev_mouse_buttons = 0;

/* ----- mouse callback (called from IRQ12 context by mouse.c) -----
 *
 * Converts a mouse_event from the PS/2 mouse driver into one or more
 * input events in the unified queue:
 *   1. Always push EV_MOUSE_MOVE with the new position and current buttons
 *   2. Push EV_MOUSE_DOWN / EV_MOUSE_UP for button press/release transitions
 */
static void input_mouse_callback(const struct mouse_event* mev) {
    /* Push mouse-move event */
    struct event ev;
    ev.type = EV_MOUSE_MOVE;
    ev.mouse.x = mev->x;
    ev.mouse.y = mev->y;
    ev.mouse.buttons = mev->buttons;
    input_push(&ev);

    /* Check for button press/release transitions */
    uint8_t pressed  = mev->buttons & ~prev_mouse_buttons;
    uint8_t released = ~mev->buttons & prev_mouse_buttons;

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
    if (pressed & MOUSE_BTN_MIDDLE) {
        ev.type = EV_MOUSE_DOWN;
        ev.mouse.buttons = MOUSE_BTN_MIDDLE;
        input_push(&ev);
    }
    if (released & MOUSE_BTN_MIDDLE) {
        ev.type = EV_MOUSE_UP;
        ev.mouse.buttons = MOUSE_BTN_MIDDLE;
        input_push(&ev);
    }

    /* Push scroll wheel event if non-zero scroll delta */
    if (mev->scroll != 0) {
        ev.type = EV_MOUSE_SCROLL;
        ev.mouse.x = mev->x;
        ev.mouse.y = mev->y;
        ev.mouse.scroll = mev->scroll;
        ev.mouse.buttons = mev->buttons;
        input_push(&ev);
    }

    prev_mouse_buttons = mev->buttons;
}

/* ----- mouse position / button getters (for external consumers) ----- */

void input_get_mouse_pos(int* x, int* y) {
    mouse_get_pos(x, y);
}

uint8_t input_get_mouse_buttons(void) {
    return mouse_get_buttons();
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
    /* Initialize the PS/2 mouse driver (handles aux port, IRQ12, packets) */
    mouse_init();

    /* Set framebuffer bounds for mouse position clamping */
    if (fb_available) {
        mouse_set_fb_bounds((int)fb_w, (int)fb_h);
    }

    /* Register our callback so we receive mouse events from the driver.
     * The callback runs in IRQ context and pushes events into our
     * unified input queue — no polling needed. */
    mouse_set_callback(input_mouse_callback);

    /* Hook into the keyboard handler to push key events for the GUI.
     * keyboard.c's set_handler lets us register a callback. */
    prev_kb_handler = NULL;  /* keyboard.c doesn't chain; we capture directly */
    keyboard_set_handler(input_kb_hook);

    pr_info("input: subsystem initialized (mouse callback + keyboard hook)\n");
}
