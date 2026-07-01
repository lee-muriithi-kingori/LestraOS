/*
 * Lestra OS - Desktop Environment
 * Copyright (c) 2026 lestramk.org
 *
 * Lightweight framebuffer desktop with basic window management.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

/* Framebuffer dimensions */
#define FB_WIDTH    1024
#define FB_HEIGHT   768
#define FB_BPP      32

/* Colors (ARGB) */
#define COLOR_BLACK     0xFF000000
#define COLOR_WHITE     0xFFFFFFFF
#define COLOR_GREY      0xFF808080
#define COLOR_DKGREY    0xFF404040
#define COLOR_LTBLUE    0xFFADD8E6
#define COLOR_BLUE      0xFF0000FF
#define COLOR_CYAN      0xFF00FFFF
#define COLOR_GREEN     0xFF00FF00
#define COLOR_YELLOW    0xFFFFFF00
#define COLOR_ORANGE    0xFFFFA500
#define COLOR_RED       0xFFFF0000

/* Desktop colors */
#define WALLPAPER_COLOR     0xFF1E3A5F
#define TASKBAR_COLOR       0xFF2C2C2C
#define TASKBAR_HEIGHT      28
#define PANEL_COLOR         0xFF333333
#define WINDOW_BORDER       0xFF555555
#define WINDOW_TITLE        0xFF444444
#define WINDOW_BG           0xFFF0F0F0
#define WINDOW_ACTIVE_TITLE 0xFF0078D7

/* Framebuffer */
static uint32_t* framebuffer = NULL;
static int fb_width = FB_WIDTH;
static int fb_height = FB_HEIGHT;

/* Mouse position */
static int mouse_x = FB_WIDTH / 2;
static int mouse_y = FB_HEIGHT / 2;

/* Draw pixel */
static void draw_pixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= fb_width || y < 0 || y >= fb_height) return;
    framebuffer[y * fb_width + x] = color;
}

/* Draw filled rectangle */
static void draw_rect(int x, int y, int w, int h, uint32_t color) {
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            draw_pixel(x + dx, y + dy, color);
        }
    }
}

/* Draw horizontal line */
static void draw_hline(int x, int y, int w, uint32_t color) {
    for (int dx = 0; dx < w; dx++) {
        draw_pixel(x + dx, y, color);
    }
}

/* Draw vertical line */
static void draw_vline(int x, int y, int h, uint32_t color) {
    for (int dy = 0; dy < h; dy++) {
        draw_pixel(x, y + dy, color);
    }
}

/* Draw rectangle outline */
static void draw_rect_outline(int x, int y, int w, int h, uint32_t color) {
    draw_hline(x, y, w, color);
    draw_hline(x, y + h - 1, w, color);
    draw_vline(x, y, h, color);
    draw_vline(x + w - 1, y, h, color);
}

/* Draw circle (filled) */
static void draw_circle(int cx, int cy, int r, uint32_t color) {
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                draw_pixel(cx + dx, cy + dy, color);
            }
        }
    }
}

/* Draw simple 8x8 font character */
static void draw_char(int x, int y, char c, uint32_t color, uint32_t bg) {
    /* Simple font bitmap for ASCII 32-127 */
    static const uint8_t font[][8] = {
        ['A'] = {0x18,0x24,0x42,0x42,0x7E,0x42,0x42,0x00},
        ['B'] = {0x78,0x44,0x44,0x78,0x44,0x44,0x78,0x00},
        ['C'] = {0x3C,0x40,0x40,0x40,0x40,0x40,0x3C,0x00},
        ['D'] = {0x78,0x44,0x42,0x42,0x42,0x44,0x78,0x00},
        ['E'] = {0x7E,0x40,0x40,0x78,0x40,0x40,0x7E,0x00},
        ['L'] = {0x40,0x40,0x40,0x40,0x40,0x40,0x7E,0x00},
        ['O'] = {0x3C,0x42,0x42,0x42,0x42,0x42,0x3C,0x00},
        ['R'] = {0x78,0x44,0x44,0x78,0x50,0x48,0x44,0x00},
        ['S'] = {0x3C,0x40,0x40,0x38,0x04,0x04,0x78,0x00},
        ['T'] = {0x7E,0x10,0x10,0x10,0x10,0x10,0x10,0x00},
        ['W'] = {0x42,0x42,0x42,0x42,0x42,0x5A,0x24,0x00},
        ['a'] = {0x00,0x00,0x38,0x04,0x3C,0x44,0x3C,0x00},
        ['e'] = {0x00,0x00,0x38,0x44,0x7C,0x40,0x3C,0x00},
        ['k'] = {0x00,0x42,0x44,0x48,0x70,0x48,0x44,0x00},
        ['l'] = {0x30,0x10,0x10,0x10,0x10,0x10,0x38,0x00},
        ['m'] = {0x00,0x00,0x44,0x6C,0x54,0x44,0x44,0x00},
        ['n'] = {0x00,0x00,0x5C,0x62,0x42,0x42,0x42,0x00},
        ['o'] = {0x00,0x00,0x3C,0x42,0x42,0x42,0x3C,0x00},
        ['r'] = {0x00,0x00,0x5C,0x62,0x40,0x40,0x40,0x00},
        ['s'] = {0x00,0x00,0x3C,0x40,0x38,0x04,0x78,0x00},
        ['t'] = {0x00,0x20,0x7C,0x20,0x20,0x24,0x18,0x00},
    };
    
    uint8_t glyph = c;
    if (glyph < 32 || glyph > 127) glyph = '?';
    
    for (int row = 0; row < 8; row++) {
        uint8_t bits = font[glyph][row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                draw_pixel(x + col, y + row, color);
            } else {
                draw_pixel(x + col, y + row, bg);
            }
        }
    }
}

/* Draw string */
static void draw_string(int x, int y, const char* str, uint32_t color, uint32_t bg) {
    while (*str) {
        draw_char(x, y, *str++, color, bg);
        x += 8;
    }
}

/* Draw desktop background */
static void draw_wallpaper(void) {
    draw_rect(0, 0, fb_width, fb_height, WALLPAPER_COLOR);
    
    /* Draw simple "Lestra OS" text */
    draw_string(fb_width/2 - 80, fb_height/2 - 40,
                "Lestra OS", COLOR_WHITE, WALLPAPER_COLOR);
    draw_string(fb_width/2 - 60, fb_height/2 - 20,
                "Desktop", COLOR_LTBLUE, WALLPAPER_COLOR);
}

/* Draw taskbar */
static void draw_taskbar(void) {
    int y = fb_height - TASKBAR_HEIGHT;
    
    /* Taskbar background */
    draw_rect(0, y, fb_width, TASKBAR_HEIGHT, TASKBAR_COLOR);
    
    /* Start button */
    draw_rect(4, y + 2, 60, TASKBAR_HEIGHT - 4, COLOR_BLUE);
    draw_string(8, y + 6, "Lestra", COLOR_WHITE, COLOR_BLUE);
    
    /* Time display */
    draw_string(fb_width - 80, y + 6, "00:00", COLOR_WHITE, TASKBAR_COLOR);
    
    /* Separator line */
    draw_hline(0, y, fb_width, WINDOW_BORDER);
}

/* Draw sample window */
static void draw_window(int x, int y, int w, int h, const char* title, bool active) {
    /* Shadow */
    draw_rect(x + 4, y + 4, w, h, 0x88000000);
    
    /* Window body */
    draw_rect(x, y, w, h, WINDOW_BG);
    
    /* Title bar */
    uint32_t title_color = active ? WINDOW_ACTIVE_TITLE : WINDOW_TITLE;
    draw_rect(x, y, w, 24, title_color);
    
    /* Title text */
    draw_string(x + 8, y + 6, title, COLOR_WHITE, title_color);
    
    /* Border */
    draw_rect_outline(x, y, w, h, WINDOW_BORDER);
    
    /* Close button */
    draw_rect(x + w - 28, y + 4, 20, 16, COLOR_RED);
    draw_string(x + w - 22, y + 6, "x", COLOR_WHITE, COLOR_RED);
    
    /* Content area */
    draw_rect(x + 4, y + 28, w - 8, h - 32, COLOR_WHITE);
}

/* Draw mouse cursor */
static void draw_mouse(void) {
    /* Simple arrow cursor */
    uint32_t color = COLOR_WHITE;
    uint32_t shadow = COLOR_BLACK;
    
    for (int i = 0; i < 12; i++) {
        draw_pixel(mouse_x + i, mouse_y + i, shadow);
        draw_pixel(mouse_x + i, mouse_y + i + 1, color);
    }
    draw_vline(mouse_x, mouse_y, 16, shadow);
    draw_vline(mouse_x + 1, mouse_y, 16, color);
    draw_pixel(mouse_x + 8, mouse_y + 12, shadow);
    draw_pixel(mouse_x + 4, mouse_y + 12, shadow);
}

/* Render entire desktop */
static void render_desktop(void) {
    draw_wallpaper();
    draw_window(100, 80, 400, 300, "Welcome to Lestra OS", true);
    draw_window(550, 150, 300, 200, "System Info", false);
    draw_taskbar();
    draw_mouse();
}

/* Main desktop loop */
int main(void) {
    printf("Starting Lestra Desktop Environment...\n");
    
    /* In a real implementation, this would:
     * 1. Set up the framebuffer via VBE/multiboot
     * 2. Initialize mouse driver
     * 3. Create window manager
     * 4. Handle events
     */
    
    printf("Desktop environment initialized.\n");
    printf("(Framebuffer mode not available in text mode)\n");
    
    return 0;
}
