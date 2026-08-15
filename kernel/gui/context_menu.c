/*
 * Lestra OS - Right-click context menus
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * A small floating menu that appears at the cursor when the user
 * right-clicks on the desktop or on an icon. Items are arranged
 * vertically with a hover highlight; disabled items are greyed out.
 * Optional separator_after draws a thin divider below an item.
 *
 * Visual style: dark glass panel with cyan accent border, matching
 * the rest of the Lestra UI (UI_CARD_BG / UI_ACCENT / UI_TEXT_*).
 *
 * Usage:
 *   struct menu_item items[] = {
 *       { "Open",   on_open,   0, 0 },
 *       { "Rename", on_rename, 0, 0 },
 *       { "",       NULL,      1, 0 },      // separator
 *       { "Delete", on_delete, 0, 1 },
 *   };
 *   menu_show(x, y, items, 4);
 *   ... // compositor polls menu_handle_event() each frame
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/font.h>
#include <lestra/timer.h>
#include <lestra/printk.h>
#include <string.h>

/* Forward-declared menu callback type. Callbacks receive no args and
 * return void — they're simple "fire and forget" actions. */
typedef void (*menu_callback_t)(void);

struct menu_item {
    const char*     label;             /* "" => separator */
    menu_callback_t callback;
    int             disabled;
    int             separator_after;   /* draw divider below this item */
};

#define MENU_MAX_ITEMS    16
#define MENU_ITEM_H       28
#define MENU_PAD_X        12
#define MENU_PAD_Y         6
#define MENU_MIN_W       140

static int menu_open_flag = 0;
static int menu_x = 0;
static int menu_y = 0;
static int menu_w = MENU_MIN_W;
static int menu_h = 0;
static int menu_hover = -1;
static struct menu_item menu_items[MENU_MAX_ITEMS];
static int menu_n_items = 0;

int menu_is_open(void) {
    return menu_open_flag;
}

void menu_close(void) {
    menu_open_flag = 0;
    menu_n_items = 0;
    menu_hover = -1;
}

void menu_show(int x, int y, const struct menu_item* items, int n) {
    if (!items || n <= 0) return;
    if (n > MENU_MAX_ITEMS) n = MENU_MAX_ITEMS;
    memcpy(menu_items, items, sizeof(struct menu_item) * (size_t)n);
    menu_n_items = n;
    /* Compute width from the widest label. */
    int max_w = MENU_MIN_W;
    for (int i = 0; i < n; i++) {
        if (menu_items[i].label && menu_items[i].label[0]) {
            int tw = fb_text_width(menu_items[i].label);
            if (tw + MENU_PAD_X * 2 > max_w) max_w = tw + MENU_PAD_X * 2;
        }
    }
    menu_w = max_w;
    menu_h = MENU_PAD_Y * 2 + n * MENU_ITEM_H;
    /* Clamp to screen so the menu doesn't overflow the framebuffer. */
    if (x + menu_w > (int)fb_w) x = (int)fb_w - menu_w - 4;
    if (y + menu_h > (int)fb_h) y = (int)fb_h - menu_h - 4;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    menu_x = x;
    menu_y = y;
    menu_hover = -1;
    menu_open_flag = 1;
}

void menu_render(void) {
    if (!menu_open_flag) return;
    /* Drop shadow. */
    fb_draw_rounded(menu_x + 2, menu_y + 4, menu_w, menu_h, 8,
                    0x66000000u, 0x66000000u);
    /* Glass panel body. */
    fb_draw_rounded(menu_x, menu_y, menu_w, menu_h, 8,
                    UI_CARD_BG, UI_ACCENT);
    /* Items. */
    int iy = menu_y + MENU_PAD_Y;
    for (int i = 0; i < menu_n_items; i++) {
        struct menu_item* it = &menu_items[i];
        int item_x = menu_x + 2;
        int item_w = menu_w - 4;
        if (it->label == NULL || it->label[0] == '\0') {
            /* Separator. */
            fb_fill_rect(menu_x + MENU_PAD_X, iy + MENU_ITEM_H / 2 - 1,
                         menu_w - MENU_PAD_X * 2, 1, UI_CARD_BORDER);
            iy += MENU_ITEM_H;
            continue;
        }
        if (i == menu_hover && !it->disabled) {
            /* Cyan hover highlight. */
            fb_draw_rounded(item_x, iy, item_w, MENU_ITEM_H - 2, 4,
                            0x3322D3EEu, 0x5522D3EEu);
        }
        uint32_t color = it->disabled ? UI_TEXT_FAINT : UI_TEXT_PRIMARY;
        fb_draw_string(item_x + MENU_PAD_X, iy + (MENU_ITEM_H - 16) / 2,
                       it->label, color);
        if (it->separator_after) {
            fb_fill_rect(menu_x + MENU_PAD_X, iy + MENU_ITEM_H - 3,
                         menu_w - MENU_PAD_X * 2, 1, UI_CARD_BORDER);
        }
        iy += MENU_ITEM_H;
    }
}

int menu_handle_event(struct event* e) {
    if (!menu_open_flag || !e) return 0;
    switch (e->type) {
        case EV_MOUSE_MOVE: {
            int mx = e->mouse.x;
            int my = e->mouse.y;
            if (mx < menu_x || mx >= menu_x + menu_w ||
                my < menu_y || my >= menu_y + menu_h) {
                menu_hover = -1;
                return 0;
            }
            int rel = (my - menu_y - MENU_PAD_Y) / MENU_ITEM_H;
            if (rel < 0 || rel >= menu_n_items) {
                menu_hover = -1;
            } else if (menu_items[rel].label == NULL ||
                       menu_items[rel].label[0] == '\0' ||
                       menu_items[rel].disabled) {
                menu_hover = -1;
            } else {
                menu_hover = rel;
            }
            return 1;  /* consumed the move so the desktop doesn't hover */
        }
        case EV_MOUSE_DOWN: {
            int mx = e->mouse.x;
            int my = e->mouse.y;
            /* Click outside the menu dismisses it. */
            if (mx < menu_x || mx >= menu_x + menu_w ||
                my < menu_y || my >= menu_y + menu_h) {
                menu_close();
                return 0;
            }
            if (menu_hover >= 0 && menu_hover < menu_n_items &&
                menu_items[menu_hover].callback &&
                !menu_items[menu_hover].disabled) {
                menu_callback_t cb = menu_items[menu_hover].callback;
                menu_close();
                cb();
                return 1;
            }
            return 1;
        }
        case EV_KEY_DOWN:
            /* Any key dismisses the menu (Esc handled by shortcuts.c). */
            menu_close();
            return 1;
        default:
            return 0;
    }
}

/* ============================================================
 * Desktop right-click default menu
 *
 * The compositor calls this when the user right-clicks on the
 * desktop (or anywhere not consumed by a widget). It builds a
 * small menu that wires into the other overlay subsystems:
 * lock screen, power menu, screenshot, brightness, volume.
 * ============================================================ */

/* Forward decls for the other overlay subsystems we dispatch to. */
extern void lock_screen_show(void);
extern void power_menu_show(void);
extern void screenshot_enter_mode(void);
extern void brightness_show_at(int x, int y);
extern void brightness_hide(void);
extern void volume_slider_show_at(int x, int y);
extern void volume_slider_hide(void);

/* Anchors for the flyouts (top-right, just below the top bar). */
static void desktop_menu_lock(void)    { lock_screen_show(); }
static void desktop_menu_power(void)   { power_menu_show(); }
static void desktop_menu_screenshot(void) { screenshot_enter_mode(); }

static void desktop_menu_brightness(void) {
    /* Only one flyout at a time — dismiss the sibling. */
    volume_slider_hide();
    brightness_show_at((int)fb_w - 280, 60);
}

static void desktop_menu_volume(void) {
    brightness_hide();
    volume_slider_show_at((int)fb_w - 56, 60);
}

void menu_show_desktop_default(int x, int y) {
    static struct menu_item items[] = {
        { "Lock Screen",   desktop_menu_lock,      0, 0 },
        { "Power Menu",    desktop_menu_power,     0, 0 },
        { "Screenshot",    desktop_menu_screenshot,0, 0 },
        { "",              NULL,                   0, 0 },  /* separator */
        { "Brightness",    desktop_menu_brightness,0, 0 },
        { "Volume",        desktop_menu_volume,    0, 0 },
    };
    menu_show(x, y, items, (int)(sizeof(items) / sizeof(items[0])));
}
