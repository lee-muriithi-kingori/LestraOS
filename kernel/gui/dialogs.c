/*
 * Lestra OS - About + Help dialogs
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 */
#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <string.h>

/* ----- About dialog ----- */
static void about_draw(struct widget* w) {
    fb_draw_rounded(w->x, w->y, w->w, w->h, 14,
                    UI_CARD_BG, UI_ACCENT);
    fb_fill_rect(w->x + 1, w->y + 1, w->w - 2, 35, 0xE00E1422);
    fb_draw_string(w->x + 12, w->y + 10, "About Lestra OS", UI_TEXT_PRIMARY);
    fb_draw_string(w->x + w->w - 20, w->y + 10, "x", UI_TEXT_MUTED);

    int y = w->y + 50;
    fb_draw_string(w->x + 16, y, "Lestra OS 1.0.0-alpha", UI_ACCENT_SOFT); y += 20;
    fb_draw_string(w->x + 16, y, "by Lee Muriithi Kingori", UI_TEXT_PRIMARY); y += 18;
    fb_draw_string(w->x + 16, y, "lestramk.org (c) 2026", UI_TEXT_MUTED); y += 24;

    fb_draw_string(w->x + 16, y, "Architecture: x86_64 long-mode", UI_TEXT_PRIMARY); y += 18;
    fb_draw_string(w->x + 16, y, "Bootloader: GRUB/multiboot2", UI_TEXT_PRIMARY); y += 18;
    fb_draw_string(w->x + 16, y, "Framebuffer: VESA 1024x768x32", UI_TEXT_PRIMARY); y += 18;
    fb_draw_string(w->x + 16, y, "Network: E1000 + TCP/IP + HTTP", UI_TEXT_PRIMARY); y += 18;
    fb_draw_string(w->x + 16, y, "AI: GLM 5.2 / Claude / Gemini / OpenAI", UI_TEXT_PRIMARY); y += 24;

    fb_draw_string(w->x + 16, y, "Press any key to close", UI_TEXT_FAINT);
}

static void about_on_event(struct widget* w, struct event* e) {
    (void)w;
    if (e->type == EV_KEY_DOWN || e->type == EV_MOUSE_DOWN) {
        w->visible = 0;
    }
}

static struct widget about_widget;

struct widget* about_create(int x, int y) {
    about_widget.x = x;
    about_widget.y = y;
    about_widget.w = 380;
    about_widget.h = 280;
    about_widget.visible = 1;
    about_widget.focused = 0;
    about_widget.draggable = 1;
    about_widget.resizable = 0;
    about_widget.draw = about_draw;
    about_widget.on_event = about_on_event;
    about_widget.state = NULL;
    memcpy(about_widget.title, "About", 6);
    return &about_widget;
}

/* ----- Help dialog ----- */
static void help_draw(struct widget* w) {
    fb_draw_rounded(w->x, w->y, w->w, w->h, 14,
                    UI_CARD_BG, UI_ACCENT);
    fb_fill_rect(w->x + 1, w->y + 1, w->w - 2, 35, 0xE00E1422);
    fb_draw_string(w->x + 12, w->y + 10, "Help - Lestra OS", UI_TEXT_PRIMARY);
    fb_draw_string(w->x + w->w - 20, w->y + 10, "x", UI_TEXT_MUTED);

    int y = w->y + 48;
    const char* lines[] = {
        "GUI Commands:",
        "  Click terminal to focus, type commands",
        "  Drag title bar to move cards",
        "  Click FAB (bottom-right) for app drawer",
        "",
        "Shell Commands:",
        "  help, neofetch, network, ping <host>",
        "  wget <url>, pkg install <name>",
        "  ai setkey <provider> <key>",
        "  ai setmodel glm glm-5.2",
        "  ai chat <prompt>",
        "  claude / glm / gemini / openai / uai",
        "",
        "Press any key to close",
    };
    for (int i = 0; i < (int)(sizeof(lines)/sizeof(lines[0])); i++) {
        uint32_t color = (lines[i][0] && lines[i][0] != ' ') ? UI_ACCENT_SOFT : UI_TEXT_PRIMARY;
        if (lines[i][0] == 0) { y += 8; continue; }
        fb_draw_string(w->x + 16, y, lines[i], color);
        y += 18;
    }
}

static void help_on_event(struct widget* w, struct event* e) {
    (void)w;
    if (e->type == EV_KEY_DOWN || e->type == EV_MOUSE_DOWN) {
        w->visible = 0;
    }
}

static struct widget help_widget;

struct widget* help_create(int x, int y) {
    help_widget.x = x;
    help_widget.y = y;
    help_widget.w = 400;
    help_widget.h = 320;
    help_widget.visible = 1;
    help_widget.focused = 0;
    help_widget.draggable = 1;
    help_widget.resizable = 0;
    help_widget.draw = help_draw;
    help_widget.on_event = help_on_event;
    help_widget.state = NULL;
    memcpy(help_widget.title, "Help", 5);
    return &help_widget;
}
