/*
 * Lestra OS - VGA Text Mode Driver
 * Copyright (c) 2026 lestramk.org
 */

#ifndef LESTRA_VGA_H
#define LESTRA_VGA_H

#include <lestra/types.h>

/* VGA dimensions */
#define VGA_WIDTH   80
#define VGA_HEIGHT  25

/* VGA colors */
enum vga_color {
    VGA_BLACK = 0,
    VGA_BLUE = 1,
    VGA_GREEN = 2,
    VGA_CYAN = 3,
    VGA_RED = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN = 6,
    VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8,
    VGA_LIGHT_BLUE = 9,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW = 14,
    VGA_WHITE = 15
};

/* VGA driver functions */
void vga_init(void);
void vga_clear(void);
void vga_putchar(char c);
void vga_puts(const char* str);
void vga_set_color(uint8_t fg, uint8_t bg);
void vga_set_cursor(uint8_t x, uint8_t y);
void vga_get_cursor(uint8_t* x, uint8_t* y);
void vga_scroll(void);
void vga_backspace(void);

/* Color helper */
static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return fg | (bg << 4);
}

static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

#endif /* LESTRA_VGA_H */
