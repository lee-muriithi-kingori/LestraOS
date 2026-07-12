/*
 * Lestra OS - Framebuffer / VESA draw API
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * The linear framebuffer address, width, height, pitch, and BPP come from
 * the multiboot2 info struct (GRUB sets VESA mode 1024×768×32 and gives
 * us the LFB pointer). We double-buffer in heap memory and fb_swap()
 * does a single rep movsd to the real LFB.
 *
 * Color format is 0x00RRGGBB (32-bit XRGB8888).
 */

#ifndef LESTRA_FB_H
#define LESTRA_FB_H

#include <lestra/types.h>

/* Lestra UI design tokens — Cyan Dark theme */
#define UI_BG_BASE       0xFF0A0C12
#define UI_BG_GRAD_TOP   0xFF0E1422
#define UI_BG_GRAD_BOT   0xFF050608
#define UI_ACCENT        0xFF22D3EE   /* Lestra cyan */
#define UI_ACCENT_HOT    0xFF06B6D4
#define UI_ACCENT_SOFT   0xFF67E8F9
#define UI_TEXT_PRIMARY  0xFFE7F0F5
#define UI_TEXT_MUTED    0xFF94A3B8
#define UI_TEXT_FAINT    0xFF475569
#define UI_CARD_BG       0xC7121620
#define UI_CARD_BORDER   0x2E22D3EE
#define UI_DANGER        0xFFF87171
#define UI_SUCCESS       0xFF4ADE80
#define UI_NEUTRAL_PILL  0xD90A0C12
#define UI_FAB_CORE      0xFF22D3EE
#define UI_FAB_SPARK     0xFF67E8F9

/* Public state */
extern uint32_t *fb;          /* real linear framebuffer (MMIO) */
extern uint32_t  fb_w;
extern uint32_t  fb_h;
extern uint32_t  fb_pitch;    /* bytes per row */
extern uint32_t  fb_bpp;      /* bits per pixel */
extern int       fb_available; /* 1 if VESA mode was set */

/* Double-buffer back buffer (allocated from heap) */
extern uint32_t *fb_back;

/* API */
void     fb_init(void *mb2_info);
void     fb_swap(void);
void     fb_clear(uint32_t color);
void     fb_fill_rect(int x, int y, int w, int h, uint32_t color);
void     fb_draw_rect(int x, int y, int w, int h, uint32_t color);
void     fb_draw_rounded(int x, int y, int w, int h, int radius,
                          uint32_t fill, uint32_t border);
void     fb_draw_char(int x, int y, char c, uint32_t color);
void     fb_draw_char_scale(int x, int y, char c, uint32_t color, int scale);
void     fb_draw_string(int x, int y, const char *s, uint32_t color);
void     fb_draw_string_small(int x, int y, const char *s, uint32_t color);
int      fb_text_width(const char *s);
void     fb_set_pixel(int x, int y, uint32_t color);
uint32_t fb_get_pixel(int x, int y);
void     fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void     fb_draw_circle(int cx, int cy, int r, uint32_t color);

/* alpha blend src over dst (src has alpha channel in high byte) */
uint32_t fb_blend(uint32_t dst, uint32_t src);

#endif /* LESTRA_FB_H */
