/*
 * Lestra OS - GUI public API
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 */

#ifndef LESTRA_GUI_H
#define LESTRA_GUI_H

#include <lestra/types.h>
#include <lestra/input.h>

/* Widget structure — shared between compositor and all GUI components. */
struct widget;

typedef void (*widget_draw_fn)(struct widget* w);
typedef void (*widget_event_fn)(struct widget* w, struct event* e);

struct widget {
    int x, y, w, h;
    int z;
    int visible;
    int focused;
    int draggable;
    int resizable;
    widget_draw_fn  draw;
    widget_event_fn on_event;
    void* state;
    char title[64];
};

/* Compositor API */
void compositor_init(void);
void compositor_run(void);
void compositor_add(struct widget* w);
void compositor_remove(struct widget* w);
void compositor_bring_to_front(struct widget* w);
void compositor_quit(void);

/* Terminal widget creator (defined in gui/terminal.c) */
struct widget* terminal_create(int x, int y);

/* AI Lab card (defined in gui/ai_lab.c) */
struct widget* ailab_create(int x, int y);

/* About dialog (defined in gui/dialogs.c) */
struct widget* about_create(int x, int y);

/* Help dialog (defined in gui/dialogs.c) */
struct widget* help_create(int x, int y);

/* Wallpaper (defined in gui/wallpaper.c) */
void wallpaper_render(void);
int wallpaper_get(void);
void wallpaper_set(int idx);

/* Drawer (app launcher) - defined in gui/drawer.c */
void drawer_toggle(void);

#endif /* LESTRA_GUI_H */
