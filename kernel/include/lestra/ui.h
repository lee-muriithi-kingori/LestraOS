/*
 * Lestra OS - UI module header.
 * Copyright (c) 2026 lestramk.org
 */
#ifndef LESTRA_UI_H
#define LESTRA_UI_H

#include <lestra/types.h>

/* Core primitives */
void ui_clear(void);
void ui_box(uint8_t row, uint8_t col, uint8_t height, uint8_t width, const char* title);
void ui_panel(uint8_t row, uint8_t col, uint8_t height, uint8_t width,
              const char* title, const char* body);

/* Title bar / status bar */
void ui_titlebar(const char* title);
void ui_statusbar(const char* msg);

/* Splash + menu */
void ui_boot_splash(void);
void ui_menu_loop(void);

/* System tools panel */
void ui_system_tools(void);

/* Theme control - 0=cyan-neon, 1=amber-phosphor, 2=green-phosphor */
int  ui_get_theme(void);
void ui_set_theme(int t);
const char* ui_theme_name(void);

/* Per-theme attribute accessors for shell/other modules */
uint8_t ui_attr_border(void);
uint8_t ui_attr_body(void);
uint8_t ui_attr_accent(void);
uint8_t ui_attr_prompt(void);
uint8_t ui_attr_error(void);
uint8_t ui_attr_success(void);

#endif /* LESTRA_UI_H */
