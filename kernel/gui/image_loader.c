/*
 * Lestra OS - Image Loader for Embedded Assets
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Loads raw RGB image data (wallpapers, icons) that are embedded in
 * the kernel binary as C arrays. The images are generated at build
 * time by a Python script and converted to C header files.
 *
 * Format: raw RGB888 (3 bytes per pixel, no header).
 * The caller provides width/height since there's no header.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/printk.h>
#include <string.h>

/* Blit a raw RGB image to the framebuffer at (x, y).
 * Handles transparency: if a pixel matches the transparent_color,
 * it is skipped (not drawn). Set transparent_color to 0x01000000
 * to disable transparency (draw all pixels). */
void fb_blit_rgb(int x, int y, int img_w, int img_h,
                 const uint8_t* rgb_data, uint32_t transparent_color) {
    if (!fb_back || !rgb_data) return;

    for (int row = 0; row < img_h; row++) {
        for (int col = 0; col < img_w; col++) {
            int px = x + col;
            int py = y + row;
            if (px < 0 || px >= (int)fb_w || py < 0 || py >= (int)fb_h)
                continue;

            const uint8_t* src = &rgb_data[(row * img_w + col) * 3];
            uint8_t r = src[0];
            uint8_t g = src[1];
            uint8_t b = src[2];

            /* Skip transparent pixels (for icons with green-screen bg) */
            if (transparent_color != 0x01000000) {
                uint32_t pixel_color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                if (pixel_color == transparent_color) continue;
            }

            fb_set_pixel(px, py, 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
        }
    }
}

/* Blit a raw RGB image with alpha blending (for semi-transparent images).
 * The alpha is provided as a separate channel or as a global alpha value. */
void fb_blit_rgb_alpha(int x, int y, int img_w, int img_h,
                        const uint8_t* rgb_data, uint8_t alpha) {
    if (!fb_back || !rgb_data) return;
    uint32_t src_alpha = alpha;

    for (int row = 0; row < img_h; row++) {
        for (int col = 0; col < img_w; col++) {
            int px = x + col;
            int py = y + row;
            if (px < 0 || px >= (int)fb_w || py < 0 || py >= (int)fb_h)
                continue;

            const uint8_t* src = &rgb_data[(row * img_w + col) * 3];
            uint8_t r = src[0];
            uint8_t g = src[1];
            uint8_t b = src[2];

            uint32_t src_pixel = (src_alpha << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            uint32_t dst_pixel = fb_get_pixel(px, py);
            fb_set_pixel(px, py, fb_blend(dst_pixel, src_pixel));
        }
    }
}

/* Blit a raw RGB image scaled by an integer factor.
 * e.g. scale=2 doubles the image size (48x48 -> 96x96). */
void fb_blit_rgb_scaled(int x, int y, int img_w, int img_h,
                         const uint8_t* rgb_data, int scale,
                         uint32_t transparent_color) {
    if (!fb_back || !rgb_data || scale <= 0) return;

    for (int row = 0; row < img_h; row++) {
        for (int col = 0; col < img_w; col++) {
            const uint8_t* src = &rgb_data[(row * img_w + col) * 3];
            uint8_t r = src[0];
            uint8_t g = src[1];
            uint8_t b = src[2];

            if (transparent_color != 0x01000000) {
                uint32_t pixel_color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                if (pixel_color == transparent_color) continue;
            }

            uint32_t color = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            fb_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}
