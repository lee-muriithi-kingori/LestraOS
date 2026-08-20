/*
 * Lestra OS - Wallpaper (Meet Yugi, proper fill)
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/vfs.h>
#include <lestra/mm.h>
#include <lestra/printk.h>
#include <string.h>

#define WALLPAPER_PATH "/wallpaper.rgb"
#define WP_W 640
#define WP_H 360

static uint8_t* data = NULL;
static int loaded = 0;

static void load(void) {
    if (loaded) return;
    loaded = 1;
    int fd = vfs_open(WALLPAPER_PATH, 0);
    if (fd < 0) return;
    size_t need = WP_W * WP_H * 3;
    data = kmalloc(need);
    if (!data) { vfs_close(fd); return; }
    ssize_t total = 0;
    while (total < (ssize_t)need) {
        ssize_t n = vfs_read(fd, data + total, need - total);
        if (n <= 0) break;
        total += n;
    }
    vfs_close(fd);
    if ((size_t)total != need) { kfree(data); data = NULL; }
}

void wallpaper_render(void) {
    load();
    if (data) {
        float src_ar = (float)WP_W / WP_H;
        float dst_ar = (float)fb_w / fb_h;
        int sx = 0, sy = 0, sw = WP_W, sh = WP_H;
        if (src_ar > dst_ar) { sw = (int)(WP_H * dst_ar); sx = (WP_W - sw) / 2; }
        else { sh = (int)(WP_W / dst_ar); sy = (WP_H - sh) / 2; }
        for (uint32_t y = 0; y < fb_h; y++) {
            uint32_t* row = &fb_back[y * fb_w];
            uint32_t sy_scaled = sy + (y * sh) / fb_h;
            if (sy_scaled >= WP_H) sy_scaled = WP_H - 1;
            for (uint32_t x = 0; x < fb_w; x++) {
                uint32_t sx_scaled = sx + (x * sw) / fb_w;
                if (sx_scaled >= WP_W) sx_scaled = WP_W - 1;
                size_t off = (sy_scaled * WP_W + sx_scaled) * 3;
                row[x] = 0xFF000000u | ((uint32_t)data[off] << 16) | ((uint32_t)data[off+1] << 8) | data[off+2];
            }
        }
    } else {
        for (uint32_t y = 0; y < fb_h; y++) {
            uint32_t* row = &fb_back[y * fb_w];
            uint32_t t = y * 256 / fb_h;
            uint8_t r = (0x0E * (256 - t) + 0x05 * t) / 256;
            uint8_t g = (0x14 * (256 - t) + 0x06 * t) / 256;
            uint8_t b = (0x22 * (256 - t) + 0x08 * t) / 256;
            uint32_t c = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            for (uint32_t x = 0; x < fb_w; x++) row[x] = c;
        }
    }
}
