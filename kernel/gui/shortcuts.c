/*
 * Lestra OS - Global keyboard shortcuts
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Handled before the compositor dispatches key events to the focused
 * widget. Returns 1 if the shortcut was consumed (the focused widget
 * should not see the event), 0 otherwise.
 *
 * Shortcuts:
 *   Alt+Tab          cycle focus to the next window
 *   Alt+F4           close (hide) the focused window
 *   Super (release)  toggle the app drawer
 *   Escape           close any open menu / dialog
 *
 * Modifier bits come from input.h (MOD_ALT, MOD_CTRL, MOD_SUPER, MOD_SHIFT).
 */

#include <lestra/types.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/keyboard.h>
#include <lestra/printk.h>

/* Forward decls from the rest of the GUI. */
extern void menu_close(void);
extern int  menu_is_open(void);
extern void drawer_toggle(void);
extern void compositor_bring_to_front(struct widget* w);

/* External hooks the compositor exposes for focus cycling. These are
 * weak in the sense that if no compositor symbols match, the shortcut
 * just no-ops. */
extern struct widget* compositor_get_focused(void);  /* may be NULL */
extern void           compositor_focus_next(void);   /* may be NULL */
extern void           compositor_close_focused(void); /* may be NULL */

/* Fallback no-op wrappers if the compositor doesn't export the above.
 * We use weak symbol semantics by checking for NULL before calling. */
static void focus_next_safe(void) {
    if ((void*)compositor_focus_next) compositor_focus_next();
}
static void close_focused_safe(void) {
    if ((void*)compositor_close_focused) compositor_close_focused();
}

int shortcuts_handle_event(struct event* e) {
    if (!e) return 0;
    if (e->type != EV_KEY_DOWN) {
        /* Super release toggles the drawer. */
        if (e->type == EV_KEY_UP && e->key.scancode == 0x5B /* left super */) {
            drawer_toggle();
            return 1;
        }
        return 0;
    }

    uint8_t mods = e->key.mods;
    uint8_t sc  = e->key.scancode;

    /* Alt+Tab — cycle focus. */
    if ((mods & MOD_ALT) && sc == KEY_TAB) {
        focus_next_safe();
        return 1;
    }
    /* Alt+F4 — close focused window. */
    if ((mods & MOD_ALT) && sc == KEY_F4) {
        close_focused_safe();
        return 1;
    }
    /* Escape — close any open context menu. */
    if (sc == KEY_ESC) {
        if (menu_is_open()) {
            menu_close();
            return 1;
        }
        /* Also close any modal overlays by sending them an event through
         * the compositor's normal path. The compositor will route Esc to
         * the focused widget if it didn't get consumed here. */
        return 0;
    }
    /* Super (key-down) — toggle drawer on release, not press. */
    if (sc == 0x5B /* left super */ && mods == 0) {
        /* We'll act on key-up so users can hold Super for a launcher
         * overview in the future. */
        return 1;
    }
    return 0;
}
