/*
 * Lestra OS - Input subsystem (mouse + keyboard event queue)
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * The PS/2 mouse driver is in drivers/char/mouse.c (handles aux port,
 * IRQ12, packet parsing). The keyboard is handled by keyboard.c.
 * This module wires both drivers into a unified event queue:
 *   - Mouse events (move/down/up) via callback from mouse.c
 *   - Keyboard events (key down/up) via hook from keyboard.c
 *   - Mouse cursor position tracking (delegates to mouse.c)
 *
 * The compositor polls events via input_poll().
 */

#ifndef LESTRA_INPUT_H
#define LESTRA_INPUT_H

#include <lestra/types.h>

/* Event types */
enum event_type {
    EV_MOUSE_MOVE = 0,
    EV_MOUSE_DOWN,
    EV_MOUSE_UP,
    EV_MOUSE_SCROLL,
    EV_KEY_DOWN,
    EV_KEY_UP,
};

/* Input event */
struct event {
    enum event_type type;
    union {
        struct { int x, y; int scroll; uint8_t buttons; } mouse;
        struct { uint8_t scancode; uint8_t ascii; uint8_t mods; } key;
    };
};

/* Mouse button bitmask */
#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

/* Modifier key bitmask (for key events) */
#define MOD_SHIFT  0x01
#define MOD_CTRL   0x02
#define MOD_ALT    0x04
#define MOD_SUPER  0x08

/* API */
void input_init(void);
int  input_poll(struct event* e);      /* returns 1 if event available, 0 if empty */
void input_get_mouse_pos(int* x, int* y);
uint8_t input_get_mouse_buttons(void);

#endif /* LESTRA_INPUT_H */
