/*
 * Lestra OS - About & Help dialogs
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Simple informational dialogs used by the desktop, dock, drawer, and app grid.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/printk.h>
#include <string.h>

/* ---- About dialog ---- */
#define ABOUT_W  520
#define ABOUT_H  380

struct about_state {
    struct widget widget;
};

static struct about_state about_state;

static void about_draw(struct widget* w) {
    extern void ui_draw_card(int x, int y, int w, int h, int focused);
    ui_draw_card(w->x, w->y, w->w, w->h, w->focused);

    int cx = w->x + w->w / 2;
    int y = w->y + 40;

    /* Logo / title */
    fb_draw_string(cx - fb_text_width("LestraOS") / 2, y, "LestraOS", UI_TEXT_PRIMARY);
    y += 24;
    fb_draw_string(cx - fb_text_width("Version 1.0.0-alpha") / 2, y, "Version 1.0.0-alpha", UI_TEXT_MUTED);
    y += 24;
    fb_draw_string(cx - fb_text_width("by Lee Muriithi Kingori") / 2, y, "by Lee Muriithi Kingori", UI_TEXT_MUTED);
    y += 24;
    fb_draw_string(cx - fb_text_width("lestramk.org") / 2, y, "lestramk.org", UI_ACCENT);

    y += 40;
    const char* info[] = {
        "A custom x86_64 operating system built from scratch.",
        "Kernel, drivers, libc, userspace, networking, TLS, AI.",
        "All layers written from scratch - no vendored kernel code.",
    };
    for (int i = 0; i < 3; i++) {
        fb_draw_string(cx - fb_text_width(info[i]) / 2, y + i * 20, info[i], UI_TEXT_PRIMARY);
    }

    y += 80;
    fb_draw_string(cx - fb_text_width("Press ESC or click X to close") / 2, y, "Press ESC or click X to close", UI_TEXT_MUTED);
}

static void about_on_event(struct widget* w, struct event* e) {
    if (e->type == EV_KEY_DOWN && e->key.scancode == KEY_ESC) {
        w->visible = 0;
        w->focused = 0;
    }
    /* Close button handled by compositor */
}

struct widget* about_create(int x, int y) {
    about_state.widget.x = x;
    about_state.widget.y = y;
    about_state.widget.w = ABOUT_W;
    about_state.widget.h = ABOUT_H;
    about_state.widget.visible = 1;
    about_state.widget.focused = 1;
    about_state.widget.draggable = 1;
    about_state.widget.resizable = 0;
    about_state.widget.draw = about_draw;
    about_state.widget.on_event = about_on_event;
    about_state.widget.state = NULL;
    memcpy(about_state.widget.title, "About LestraOS", 15);
    return &about_state.widget;
}

/* ---- Help dialog ---- */
#define HELP_W  560
#define HELP_H  420

struct help_state {
    struct widget widget;
};

static struct help_state help_state;

static void help_draw(struct widget* w) {
    extern void ui_draw_card(int x, int y, int w, int h, int focused);
    ui_draw_card(w->x, w->y, w->w, w->h, w->focused);

    int x0 = w->x + 24;
    int y = w->y + 40;

    fb_draw_string(x0, y, "LestraOS - Quick Reference", UI_TEXT_PRIMARY);
    y += 32;

    const char* sections[][2] = {
        { "System",       "help, sysinfo, neofetch, uname, version, uptime, whoami, hostname, exit" },
        { "Process",      "ps, free, cpuinfo, meminfo, reboot, shutdown" },
        { "Network",      "netstat, ifconfig, network, ping, ping6, wget, firewall" },
        { "Files",        "file (ls/mkdir/cat/cp/mv/rmdir/rm/chmod/stat), mount, save, exec" },
        { "Hardware",     "lspci, battery, temp, wifi, disk" },
        { "Services",     "services (alias: lee status), lee (service manager), cron" },
        { "Packages",     "packages (alias: pkg list), pkg (install/list/search)" },
        { "AI",           "ai, claude, glm, gemini, openai, uai" },
        { "UI",           "ui, theme, clear, date, time, play, speak" },
        { "Shortcuts",    "Super: open drawer, Alt+Tab: switch windows, Alt+F4: close, Ctrl+Alt+L: lock, Ctrl+Alt+P: power menu" },
    };

    for (int i = 0; i < 10; i++) {
        fb_draw_string(x0, y, sections[i][0], UI_ACCENT);
        y += 18;
        fb_draw_string(x0, y, sections[i][1], UI_TEXT_PRIMARY);
        y += 24;
    }

    fb_draw_string(x0, y + 10, "Press ESC or click X to close", UI_TEXT_MUTED);
}

static void help_on_event(struct widget* w, struct event* e) {
    if (e->type == EV_KEY_DOWN && e->key.scancode == KEY_ESC) {
        w->visible = 0;
        w->focused = 0;
    }
}

struct widget* help_create(int x, int y) {
    help_state.widget.x = x;
    help_state.widget.y = y;
    help_state.widget.w = HELP_W;
    help_state.widget.h = HELP_H;
    help_state.widget.visible = 1;
    help_state.widget.focused = 1;
    help_state.widget.draggable = 1;
    help_state.widget.resizable = 0;
    help_state.widget.draw = help_draw;
    help_state.widget.on_event = help_on_event;
    help_state.widget.state = NULL;
    memcpy(help_state.widget.title, "Help", 5);
    return &help_state.widget;
}