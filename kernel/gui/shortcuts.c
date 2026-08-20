/*
 * Lestra OS - Global keyboard shortcuts
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
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

/* Native widget creators */
extern struct widget* terminal_create(int x, int y);
extern void compositor_add(struct widget* w);
extern void compositor_bring_to_front(struct widget* w);

/* External hooks the compositor exposes for focus cycling. */
extern struct widget* compositor_get_focused(void);
extern void           compositor_focus_next(void);
extern void           compositor_close_focused(void);

static void focus_next_safe(void) {
    if ((void*)compositor_focus_next) compositor_focus_next();
}
static void close_focused_safe(void) {
    if ((void*)compositor_close_focused) compositor_close_focused();
}

/* Terminal singleton for quick-launch hotkey */
static struct widget* g_terminal_widget = NULL;

static void launch_terminal_hotkey(void) {
    if (!g_terminal_widget) {
        g_terminal_widget = terminal_create(100, 50);
        if (g_terminal_widget) compositor_add(g_terminal_widget);
    }
    if (g_terminal_widget) {
        g_terminal_widget->visible = 1;
        compositor_bring_to_front(g_terminal_widget);
    }
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

    /* Ctrl+Alt+T — open terminal (classic Linux shortcut). */
    if ((mods & MOD_CTRL) && (mods & MOD_ALT) && sc == KEY_T) {
        launch_terminal_hotkey();
        return 1;
    }

    /* Super+T — also open terminal (Mac-like). */
    if ((mods & MOD_SUPER) && sc == KEY_T) {
        launch_terminal_hotkey();
        return 1;
    }

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
        return 0;
    }
    /* Super (key-down) — toggle drawer on release. */
    if (sc == 0x5B /* left super */ && mods == 0) {
        return 1;
    }
    return 0;
}
