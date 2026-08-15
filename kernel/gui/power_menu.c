/*
 * Lestra OS - Power Menu
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * A full-screen overlay with three big options: Shut Down, Restart,
 * Sleep. Clicking one of them opens a confirmation prompt ("Are you
 * sure? [Yes] [Cancel]"). The actual power actions are wired through
 * cmd_shutdown() / cmd_reboot() which the existing shell already
 * provides (kernel/core/shell.c). Sleep is a no-op stub because the
 * kernel doesn't yet have ACPI S3 support.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>
#include <lestra/printk.h>
#include <string.h>

extern void cmd_reboot(void);
extern void cmd_shutdown(void);

#define PM_BTN_W   180
#define PM_BTN_H   90
#define PM_BTN_GAP 24

enum {
    PM_NONE = -1,
    PM_SHUTDOWN = 0,
    PM_RESTART,
    PM_SLEEP
};

struct pm_state {
    int visible;
    int confirming;     /* which action (PM_SHUTDOWN/PM_RESTART/PM_SLEEP) */
};

static struct pm_state pm_state;

/* ---------- helpers ---------- */
static int pm_hit_button(int action, int mx, int my) {
    int total_w = 3 * PM_BTN_W + 2 * PM_BTN_GAP;
    int bx = (int)fb_w / 2 - total_w / 2;
    int by = (int)fb_h / 2 - PM_BTN_H / 2 - 20;
    int x = bx + action * (PM_BTN_W + PM_BTN_GAP);
    return (mx >= x && mx < x + PM_BTN_W &&
            my >= by && my < by + PM_BTN_H);
}

static int pm_hit_confirm_yes(int mx, int my) {
    int bw = 100, bh = 32;
    int bx = (int)fb_w / 2 - bw - 12;
    int by = (int)fb_h / 2 + 30;
    return (mx >= bx && mx < bx + bw && my >= by && my < by + bh);
}

static int pm_hit_confirm_cancel(int mx, int my) {
    int bw = 100, bh = 32;
    int bx = (int)fb_w / 2 + 12;
    int by = (int)fb_h / 2 + 30;
    return (mx >= bx && mx < bx + bw && my >= by && my < by + bh);
}

static void pm_draw_button(int action, const char* glyph,
                            const char* label, uint32_t color) {
    int total_w = 3 * PM_BTN_W + 2 * PM_BTN_GAP;
    int bx = (int)fb_w / 2 - total_w / 2;
    int by = (int)fb_h / 2 - PM_BTN_H / 2 - 20;
    int x = bx + action * (PM_BTN_W + PM_BTN_GAP);
    fb_draw_rounded(x, by, PM_BTN_W, PM_BTN_H, 12,
                    0xFF0E1422, color);
    /* Glyph */
    int gw = fb_text_width(glyph);
    fb_draw_string(x + (PM_BTN_W - gw) / 2, by + 12, glyph, color);
    /* Label */
    int lw = fb_text_width(label);
    fb_draw_string(x + (PM_BTN_W - lw) / 2, by + 56, label, UI_TEXT_PRIMARY);
}

/* ---------- public ---------- */
void power_menu_show(void) {
    pm_state.visible = 1;
    pm_state.confirming = PM_NONE;
}

void power_menu_hide(void) {
    pm_state.visible = 0;
    pm_state.confirming = PM_NONE;
}

/* ---------- render ---------- */
void power_menu_render(void) {
    if (!pm_state.visible) return;

    /* Dim backdrop. */
    fb_fill_rect(0, 0, (int)fb_w, (int)fb_h, 0xCC050608u);

    /* Title. */
    const char* title = "Power";
    int tw = fb_text_width(title);
    fb_draw_string(((int)fb_w - tw) / 2,
                   (int)fb_h / 2 - PM_BTN_H / 2 - 70,
                   title, UI_TEXT_PRIMARY);

    if (pm_state.confirming == PM_NONE) {
        pm_draw_button(PM_SHUTDOWN, "X", "Shut Down", UI_DANGER);
        pm_draw_button(PM_RESTART,  "R", "Restart",   UI_ACCENT);
        pm_draw_button(PM_SLEEP,    "Z", "Sleep",     UI_TEXT_MUTED);
        const char* hint = "Click an option. Esc to cancel.";
        int hw = fb_text_width(hint);
        fb_draw_string(((int)fb_w - hw) / 2,
                       (int)fb_h / 2 + PM_BTN_H / 2 + 24,
                       hint, UI_TEXT_MUTED);
    } else {
        /* Confirmation prompt. */
        const char* verb = (pm_state.confirming == PM_SHUTDOWN)
                           ? "shut down" : (pm_state.confirming == PM_RESTART)
                           ? "restart"   : "sleep";
        char msg[64];
        ksnprintf(msg, sizeof(msg), "Are you sure you want to %s?", verb);
        int mw = fb_text_width(msg);
        fb_draw_string(((int)fb_w - mw) / 2,
                       (int)fb_h / 2 - 8, msg, UI_TEXT_PRIMARY);
        /* Yes button */
        int bw = 100, bh = 32;
        int yx = (int)fb_w / 2 - bw - 12;
        int yy = (int)fb_h / 2 + 30;
        fb_draw_rounded(yx, yy, bw, bh, 6, UI_DANGER, UI_DANGER);
        fb_draw_string(yx + 24, yy + 8, "Yes", 0xFF000000);
        /* Cancel button */
        int cx = (int)fb_w / 2 + 12;
        fb_draw_rounded(cx, yy, bw, bh, 6, 0xFF1E293B, UI_ACCENT);
        fb_draw_string(cx + 16, yy + 8, "Cancel", UI_TEXT_PRIMARY);
    }
}

/* ---------- events ---------- */
int power_menu_handle_event(struct event* e) {
    if (!pm_state.visible) return 0;
    if (e->type == EV_KEY_DOWN && e->key.scancode == KEY_ESC) {
        if (pm_state.confirming != PM_NONE) {
            pm_state.confirming = PM_NONE;
        } else {
            power_menu_hide();
        }
        return 1;
    }
    if (e->type != EV_MOUSE_DOWN) {
        /* Swallow everything while we're up. */
        return (e->type == EV_MOUSE_MOVE || e->type == EV_MOUSE_UP ||
                e->type == EV_KEY_UP) ? 1 : 0;
    }
    int mx = e->mouse.x, my = e->mouse.y;

    if (pm_state.confirming != PM_NONE) {
        if (pm_hit_confirm_yes(mx, my)) {
            int action = pm_state.confirming;
            pm_state.confirming = PM_NONE;
            pm_state.visible = 0;
            switch (action) {
                case PM_SHUTDOWN: cmd_shutdown(); break;
                case PM_RESTART:  cmd_reboot();   break;
                case PM_SLEEP:    pr_info("power_menu: sleep (no ACPI S3)\n");
                                  break;
            }
            return 1;
        }
        if (pm_hit_confirm_cancel(mx, my)) {
            pm_state.confirming = PM_NONE;
            return 1;
        }
        return 1;  /* consume */
    }

    /* Top-level menu. */
    if (pm_hit_button(PM_SHUTDOWN, mx, my)) {
        pm_state.confirming = PM_SHUTDOWN;
        return 1;
    }
    if (pm_hit_button(PM_RESTART, mx, my)) {
        pm_state.confirming = PM_RESTART;
        return 1;
    }
    if (pm_hit_button(PM_SLEEP, mx, my)) {
        pm_state.confirming = PM_SLEEP;
        return 1;
    }
    /* Click outside = dismiss. */
    power_menu_hide();
    return 1;
}
