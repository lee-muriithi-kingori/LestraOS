/*
 * Lestra OS - PS/2 Mouse Driver API
 * Copyright (c) 2026 lestramk.org
 *
 * Provides a standalone PS/2 mouse driver that handles aux port
 * initialization, IRQ12 processing, and 3-byte packet parsing.
 * The driver maintains its own ring buffer of mouse events and
 * optionally notifies a registered callback on each new event,
 * so the input subsystem can push events into the combined queue
 * without polling.
 */

#ifndef LESTRA_MOUSE_H
#define LESTRA_MOUSE_H

#include <lestra/types.h>

/* Mouse event structure — one per complete packet. */
struct mouse_event {
    int      dx;       /* X delta (sign-extended, positive = right) */
    int      dy;       /* Y delta (sign-extended, positive = down on screen) */
    uint8_t  buttons;  /* Button bitmask: bit 0=left, bit 1=right, bit 2=middle */
    int      x;        /* Absolute X position (clamped to framebuffer) */
    int      y;        /* Absolute Y position (clamped to framebuffer) */
};

/* Mouse button bitmask */
#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

/* Callback type: called from IRQ context whenever a complete mouse
 * packet is parsed and a new event is generated. The callback
 * receives a pointer to the event (read-only). */
typedef void (*mouse_event_callback_t)(const struct mouse_event* ev);

/* API */

/* Initialize the PS/2 mouse: enable aux port, reset mouse, set
 * sample rate / resolution / scaling, register IRQ12 handler.
 * Returns 0 on success, -1 on failure. */
int  mouse_init(void);

/* Check if a mouse event is available in the ring buffer. */
int  mouse_has_event(void);

/* Dequeue one mouse event from the ring buffer.
 * Returns 1 if an event was available, 0 if empty.
 * The event struct is filled with dx, dy, buttons, x, y. */
int  mouse_get_event(struct mouse_event* ev);

/* Register a callback to be invoked on each new mouse event.
 * The callback runs in IRQ context, so it must be minimal.
 * Pass NULL to unregister. */
void mouse_set_callback(mouse_event_callback_t cb);

/* Get current absolute mouse position (for cursor tracking). */
void mouse_get_pos(int* x, int* y);

/* Get current button state bitmask. */
uint8_t mouse_get_buttons(void);

/* Set the framebuffer dimensions for position clamping.
 * Called by input subsystem once fb is initialized. */
void mouse_set_fb_bounds(int width, int height);

#endif /* LESTRA_MOUSE_H */
