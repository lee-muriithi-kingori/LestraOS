/*
 * Lestra OS - Pro UI public API
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 */
#ifndef LESTRA_UI_PRO_H
#define LESTRA_UI_PRO_H

#include <lestra/types.h>
#include <lestra/input.h>

/* Card with shadow + glassmorphism + focus glow */
void ui_draw_card(int x, int y, int w, int h, int focused);

/* Professional status bar with time/battery/temp/mem/net */
void ui_render_status_bar(void);

/* Enhanced dock with magnification */
void ui_render_dock(void);
int  ui_dock_handle_event(struct event* e);

/* Enhanced desktop icons with hover glow */
void ui_render_desktop_icons(void);
int  ui_desktop_icons_handle_click(int x, int y);
void ui_desktop_icons_handle_move(int x, int y);

#endif

/* Desktop wallpaper with LestraOS branding */
void ui_render_wallpaper(void);

/* Mini music player popup */
void music_set_playing(const char* track_name);
void music_set_stopped(void);
int music_is_playing(void);
void ui_render_mini_player(void);

/* Notification system */
void ui_notify(const char* text);
void ui_render_notifications(void);

/* Settings */
int settings_get_brightness(void);
int settings_get_volume(void);
int settings_get_adblock(void);
int settings_get_dark_mode(void);
void settings_set_brightness(int v);
void settings_set_volume(int v);
void settings_set_adblock(int v);
void settings_set_dark_mode(int v);

