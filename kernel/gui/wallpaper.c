/*
 * Lestra OS - Wallpaper
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Displays the Meet Yugi family photo as the OS wallpaper.
 * The image is loaded from /wallpaper.rgb in the initrd at boot.
 * Format: raw RGB (3 bytes per pixel, no header), 640x360.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/vfs.h>
#include <lestra/mm.h>
#include <lestra/printk.h>
#include <string.h>

#define WALLPAPER_PATH "/wallpaper.rgb"
#define WP_IMG_W  640
#define WP_IMG_H  360

/* Embedded fallback: solid dark background if image fails to load */
#define FALLBACK_TOP    0xFF0E1422
#define FALLBACK_BOTTOM 0xFF050608

static uint8_t* wallpaper_data = NULL;
static int wallpaper_loaded = 0;
static int wallpaper_w = WP_IMG_W;
static int wallpaper_h = WP_IMG_H;

static void wp_load(void) {
    if (wallpaper_loaded) return;
    wallpaper_loaded = 1;

    int fd = vfs_open(WALLPAPER_PATH, O_RDONLY);
    if (fd < 0) {
        pr_warn("wallpaper: failed to open %s, using fallback\n", WALLPAPER_PATH);
        return;
    }

    /* Read the entire image into a heap buffer */
    size_t expected = (size_t)WP_IMG_W * WP_IMG_H * 3;
    wallpaper_data = (uint8_t*)kmalloc(expected);
    if (!wallpaper_data) {
        pr_warn("wallpaper: out of memory for image\n");
        vfs_close(fd);
        return;
    }

    ssize_t total = 0;
    while ((size_t)total < expected) {
        ssize_t n = vfs_read(fd, wallpaper_data + total, expected - total);
        if (n <= 0) break;
        total += n;
    }
    vfs_close(fd);

    if ((size_t)total == expected) {
        pr_info("wallpaper: loaded Meet Yugi (%dx%d, %u bytes)\n",
                WP_IMG_W, WP_IMG_H, (unsigned)total);
    } else {
        pr_warn("wallpaper: incomplete read (%zd/%zu), using fallback\n",
                total, expected);
        kfree(wallpaper_data);
        wallpaper_data = NULL;
    }
}

/* ---------- public API ---------- */
void wallpaper_set(int idx) {
    (void)idx;
    /* Single wallpaper - no-op */
}

int wallpaper_get(void) {
    return 0;
}

void wallpaper_render(void) {
    wp_load();

    if (wallpaper_data) {
        /* Scale the 640x360 image to fill the framebuffer.
         * Uses nearest-neighbor scaling for speed. */
        for (uint32_t y = 0; y < fb_h; y++) {
            uint32_t* row = &fb_back[y * fb_w];
            uint32_t src_y = (y * WP_IMG_H) / fb_h;
            if (src_y >= WP_IMG_H) src_y = WP_IMG_H - 1;

            for (uint32_t x = 0; x < fb_w; x++) {
                uint32_t src_x = (x * WP_IMG_W) / fb_w;
                if (src_x >= WP_IMG_W) src_x = WP_IMG_W - 1;

                /* Source pixel offset (3 bytes per pixel: R, G, B) */
                size_t offset = ((size_t)src_y * WP_IMG_W + src_x) * 3;
                uint8_t r = wallpaper_data[offset];
                uint8_t g = wallpaper_data[offset + 1];
                uint8_t b = wallpaper_data[offset + 2];

                row[x] = 0xFF000000u | ((uint32_t)r << 16) |
                         ((uint32_t)g << 8) | b;
            }
        }
    } else {
        /* Fallback: vertical gradient */
        uint8_t tr = (uint8_t)((FALLBACK_TOP >> 16) & 0xFF);
        uint8_t tg = (uint8_t)((FALLBACK_TOP >> 8) & 0xFF);
        uint8_t tb = (uint8_t)(FALLBACK_TOP & 0xFF);
        uint8_t br = (uint8_t)((FALLBACK_BOTTOM >> 16) & 0xFF);
        uint8_t bg = (uint8_t)((FALLBACK_BOTTOM >> 8) & 0xFF);
        uint8_t bb = (uint8_t)(FALLBACK_BOTTOM & 0xFF);

        for (uint32_t y = 0; y < fb_h; y++) {
            uint32_t t = y * 256 / fb_h;
            uint8_t r = (uint8_t)((tr * (256 - t) + br * t) / 256);
            uint8_t g = (uint8_t)((tg * (256 - t) + bg * t) / 256);
            uint8_t b = (uint8_t)((tb * (256 - t) + bb * t) / 256);
            uint32_t* row = &fb_back[y * fb_w];
            uint32_t color = 0xFF000000u |
                             ((uint32_t)r << 16) |
                             ((uint32_t)g << 8) | b;
            for (uint32_t x = 0; x < fb_w; x++) row[x] = color;
        }
    }
}

/* Wallpaper picker - disabled (single wallpaper OS) */
struct widget* wallpaper_picker_create(int x, int y) {
    (void)x; (void)y;
    return NULL;
}
