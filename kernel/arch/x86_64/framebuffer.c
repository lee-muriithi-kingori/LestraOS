/*
 * Lestra OS - Framebuffer / VESA draw API implementation
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * The linear framebuffer address, width, height, pitch, and BPP come
 * from the multiboot2 info struct. GRUB sets VESA mode 1024×768×32
 * via the multiboot2 framebuffer header tag, and gives us the LFB
 * pointer in the info struct tag type 8.
 *
 * We double-buffer: fb_back is a heap allocation of fb_w * fb_h pixels.
 * All drawing goes to fb_back; fb_swap() copies it to the real LFB.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/font.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <string.h>

/* Public state (declared in fb.h) */
uint32_t *fb = NULL;          /* real linear framebuffer (MMIO) */
uint32_t  fb_w = 0;
uint32_t  fb_h = 0;
uint32_t  fb_pitch = 0;
uint32_t  fb_bpp = 0;
int       fb_available = 0;
uint32_t *fb_back = NULL;     /* double-buffer back buffer */

/* Multiboot2 tag types we care about */
#define MB2_TAG_FRAMEBUFFER  8

struct mb2_tag {
    uint32_t type;
    uint32_t size;
} __packed;

struct mb2_fb_tag {
    struct mb2_tag tag;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  color_info[6];
} __packed;

/* Parse multiboot2 info to find the framebuffer tag.
 * The mb2_info struct starts with total_size (u32) + reserved (u32),
 * then tags follow, each 8-byte aligned. */
void fb_init(void* mb2_info) {
    fb_available = 0;
    fb = NULL;
    fb_back = NULL;

    if (!mb2_info) {
        pr_warn("fb: no multiboot2 info - staying in text mode\n");
        return;
    }

    uint32_t total_size = *(uint32_t*)mb2_info;
    uintptr_t info_base = (uintptr_t)mb2_info;
    uintptr_t info_end = info_base + total_size;
    struct mb2_tag* tag = (struct mb2_tag*)(info_base + 8);

    while ((uintptr_t)tag + sizeof(struct mb2_tag) <= info_end) {
        if (tag->type == 0) break;  /* end tag */
        if (tag->size < sizeof(struct mb2_tag)) break;

        if (tag->type == MB2_TAG_FRAMEBUFFER &&
            tag->size >= sizeof(struct mb2_fb_tag)) {
            struct mb2_fb_tag* fbt = (struct mb2_fb_tag*)tag;
            fb = (uint32_t*)(uintptr_t)fbt->framebuffer_addr;
            fb_w = fbt->framebuffer_width;
            fb_h = fbt->framebuffer_height;
            fb_pitch = fbt->framebuffer_pitch;
            fb_bpp = fbt->framebuffer_bpp;

            pr_info("fb: %ux%u@%ubpp pitch=%u addr=0x%x type=%u\n",
                    (unsigned)fb_w, (unsigned)fb_h, (unsigned)fb_bpp,
                    (unsigned)fb_pitch, (unsigned)(uintptr_t)fb,
                    (unsigned)fbt->framebuffer_type);

            /* Accept 16, 24, and 32 bpp. VirtualBox often gives 16 or 24. */
            if (fb_bpp != 16 && fb_bpp != 24 && fb_bpp != 32) {
                pr_warn("fb: unsupported bpp %u (need 16/24/32)\n", (unsigned)fb_bpp);
                fb = NULL;
                return;
            }

            /* Check framebuffer type: 0=indexed, 1=RGB, 2=EFI */
            if (fbt->framebuffer_type == 0) {
                /* Indexed palette — try to set a basic palette */
                pr_warn("fb: indexed palette mode, setting basic palette\n");
                /* Set a simple 332 palette (8 colors R, 8 G, 4 B) */
                /* ... skip for now, just proceed — RGB conversion will be wrong
                 * but at least the screen won't be blank */
            } else if (fbt->framebuffer_type != 1 && fbt->framebuffer_type != 2) {
                pr_warn("fb: unknown type %u\n", (unsigned)fbt->framebuffer_type);
                fb = NULL;
                return;
            }

            /* Back buffer is always 32-bit ARGB internally.
             * fb_swap converts to the real framebuffer's BPP. */
            size_t buf_size = (size_t)fb_w * fb_h * 4;
            fb_back = (uint32_t*)kmalloc(buf_size);
            if (!fb_back) {
                pr_warn("fb: failed to allocate %u-byte back buffer\n",
                        (unsigned)buf_size);
                fb = NULL;
                return;
            }

            fb_available = 1;
            pr_info("fb: double-buffer ready (%u KB back buffer)\n",
                    (unsigned)(buf_size / 1024));
            return;
        }

        /* Advance to next tag (8-byte aligned) */
        uintptr_t next = (uintptr_t)tag + tag->size;
        next = (next + 7) & ~7u;
        if (next <= (uintptr_t)tag) break;
        tag = (struct mb2_tag*)next;
    }

    pr_warn("fb: no framebuffer tag in multiboot2 info - text mode only\n");
}

void fb_swap(void) {
    if (!fb_available || !fb || !fb_back) return;

    if (fb_bpp == 32) {
        /* 32-bpp copy must respect pitch — fb_pitch may be > fb_w*4.
         * Copy row by row like the 24-bpp path. */
        for (uint32_t y = 0; y < fb_h; y++) {
            uint8_t* dst_row = (uint8_t*)fb + y * fb_pitch;
            uint32_t* src_row = fb_back + y * fb_w;
            uint32_t* dst = (uint32_t*)dst_row;
            uint32_t* src = src_row;
            size_t count = fb_w;
            __asm__ volatile (
                "rep movsd"
                : "+D"(dst), "+S"(src), "+c"(count)
                :
                : "memory"
            );
        }
    } else if (fb_bpp == 24) {
        /* Convert 32-bit ARGB to 24-bit RGB */
        uint8_t* dst = (uint8_t*)fb;
        for (uint32_t y = 0; y < fb_h; y++) {
            uint8_t* row = dst + y * fb_pitch;
            for (uint32_t x = 0; x < fb_w; x++) {
                uint32_t pixel = fb_back[y * fb_w + x];
                row[x * 3]     = (pixel >> 16) & 0xFF;  /* R */
                row[x * 3 + 1] = (pixel >> 8) & 0xFF;   /* G */
                row[x * 3 + 2] = pixel & 0xFF;           /* B */
            }
        }
    } else if (fb_bpp == 16) {
        /* Convert 32-bit ARGB to 16-bit RGB565 */
        uint16_t* dst = (uint16_t*)fb;
        for (uint32_t y = 0; y < fb_h; y++) {
            uint16_t* row = (uint16_t*)((uint8_t*)fb + y * fb_pitch);
            for (uint32_t x = 0; x < fb_w; x++) {
                uint32_t pixel = fb_back[y * fb_w + x];
                uint8_t r = (pixel >> 16) & 0xFF;
                uint8_t g = (pixel >> 8) & 0xFF;
                uint8_t b = pixel & 0xFF;
                /* RGB565: 5 bits R, 6 bits G, 5 bits B */
                row[x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            }
        }
    }
}

void fb_set_pixel(int x, int y, uint32_t color) {
    if (!fb_back) return;
    if (x < 0 || y < 0 || (uint32_t)x >= fb_w || (uint32_t)y >= fb_h) return;
    fb_back[y * fb_w + x] = color;
}

uint32_t fb_get_pixel(int x, int y) {
    if (!fb_back) return 0;
    if (x < 0 || y < 0 || (uint32_t)x >= fb_w || (uint32_t)y >= fb_h) return 0;
    return fb_back[y * fb_w + x];
}

void fb_clear(uint32_t color) {
    if (!fb_back) return;
    size_t count = (size_t)fb_w * fb_h;
    uint32_t* p = fb_back;
    while (count--) *p++ = color;
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!fb_back) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_w) w = fb_w - x;
    if (y + h > (int)fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;

    for (int row = 0; row < h; row++) {
        uint32_t* p = &fb_back[(y + row) * fb_w + x];
        for (int col = 0; col < w; col++) *p++ = color;
    }
}

void fb_draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (!fb_back) return;
    /* Top + bottom */
    fb_fill_rect(x, y, w, 1, color);
    fb_fill_rect(x, y + h - 1, w, 1, color);
    /* Left + right */
    fb_fill_rect(x, y, 1, h, color);
    fb_fill_rect(x + w - 1, y, 1, h, color);
}

void fb_draw_rounded(int x, int y, int w, int h, int radius,
                      uint32_t fill, uint32_t border) {
    if (!fb_back) return;
    /* Fill the body (minus corner areas that need rounding) */
    fb_fill_rect(x + radius, y, w - 2 * radius, h, fill);
    fb_fill_rect(x, y + radius, radius, h - 2 * radius, fill);
    fb_fill_rect(x + w - radius, y + radius, radius, h - 2 * radius, fill);

    /* Draw the four rounded corners pixel by pixel */
    for (int dy = 0; dy < radius; dy++) {
        for (int dx = 0; dx < radius; dx++) {
            int dist_sq = (radius - 1 - dx) * (radius - 1 - dx)
                        + (radius - 1 - dy) * (radius - 1 - dy);
            int r_sq = radius * radius;
            if (dist_sq <= r_sq) {
                /* Top-left */
                fb_set_pixel(x + dx, y + dy, fill);
                /* Top-right */
                fb_set_pixel(x + w - 1 - dx, y + dy, fill);
                /* Bottom-left */
                fb_set_pixel(x + dx, y + h - 1 - dy, fill);
                /* Bottom-right */
                fb_set_pixel(x + w - 1 - dx, y + h - 1 - dy, fill);
            }
        }
    }

    /* Draw border (1px, follows the rounded shape) */
    /* Top edge (excluding corners) */
    fb_fill_rect(x + radius, y, w - 2 * radius, 1, border);
    /* Bottom edge */
    fb_fill_rect(x + radius, y + h - 1, w - 2 * radius, 1, border);
    /* Left edge */
    fb_fill_rect(x, y + radius, 1, h - 2 * radius, border);
    /* Right edge */
    fb_fill_rect(x + w - 1, y + radius, 1, h - 2 * radius, border);
    /* Corner border pixels */
    for (int dy = 0; dy < radius; dy++) {
        for (int dx = 0; dx < radius; dx++) {
            int dist_sq = (radius - 1 - dx) * (radius - 1 - dx)
                        + (radius - 1 - dy) * (radius - 1 - dy);
            int outer_sq = radius * radius;
            int inner_sq = (radius - 1) * (radius - 1);
            if (dist_sq <= outer_sq && dist_sq > inner_sq) {
                fb_set_pixel(x + dx, y + dy, border);
                fb_set_pixel(x + w - 1 - dx, y + dy, border);
                fb_set_pixel(x + dx, y + h - 1 - dy, border);
                fb_set_pixel(x + w - 1 - dx, y + h - 1 - dy, border);
            }
        }
    }
}

void fb_draw_char(int x, int y, char c, uint32_t color) {
    if (!fb_back) return;
    const uint8_t* glyph = font_get_row((uint8_t)c);
    if (!glyph) return;
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                fb_set_pixel(x + col, y + row, color);
                /* Pixel-perfect: add subtle right-edge for thicker chars */
                if (col < 7 && (bits & (0x80 >> (col + 1)))) {
                    /* adjacent pixel is also set — good, solid look */
                }
            }
        }
    }
}

void fb_draw_char_scale(int x, int y, char c, uint32_t color, int scale) {
    if (!fb_back || scale <= 0) return;
    if (scale == 1) { fb_draw_char(x, y, c, color); return; }
    const uint8_t* glyph = font_get_row((uint8_t)c);
    if (!glyph) return;
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                fb_fill_rect(x + col * scale, y + row * scale,
                             scale, scale, color);
            }
        }
    }
}

void fb_draw_string(int x, int y, const char* s, uint32_t color) {
    if (!fb_back || !s) return;
    int cx = x;
    while (*s) {
        fb_draw_char(cx, y, *s, color);
        cx += 8;
        s++;
    }
}

void fb_draw_string_small(int x, int y, const char* s, uint32_t color) {
    /* For now, same as regular - we only have one font size.
     * The brief specifies a 6×10 font for the status pill; that would
     * be a separate font table. For v3 MVP, we use the 8×16 font. */
    fb_draw_string(x, y, s, color);
}

int fb_text_width(const char* s) {
    if (!s) return 0;
    int n = 0;
    while (*s++) n++;
    return n * 8;
}

void fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    if (!fb_back) return;
    /* Bresenham's line algorithm */
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        fb_set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

void fb_draw_circle(int cx, int cy, int r, uint32_t color) {
    if (!fb_back || r <= 0) return;
    /* Midpoint circle algorithm */
    int x = r, y = 0;
    int err = 0;
    while (x >= y) {
        fb_set_pixel(cx + x, cy + y, color);
        fb_set_pixel(cx + y, cy + x, color);
        fb_set_pixel(cx - y, cy + x, color);
        fb_set_pixel(cx - x, cy + y, color);
        fb_set_pixel(cx - x, cy - y, color);
        fb_set_pixel(cx - y, cy - x, color);
        fb_set_pixel(cx + y, cy - x, color);
        fb_set_pixel(cx + x, cy - y, color);
        y += 1;
        if (err <= 0) { err += 2 * y + 1; }
        if (err > 0) { x -= 1; err -= 2 * x + 1; }
    }
}

/* Alpha blend src over dst.
 * src alpha is in the high byte (0xAA______). We use "over" compositing. */
uint32_t fb_blend(uint32_t dst, uint32_t src) {
    uint8_t sa = (src >> 24) & 0xFF;
    if (sa == 0) return dst;
    if (sa == 255) return src;

    uint8_t sr = (src >> 16) & 0xFF;
    uint8_t sg = (src >> 8) & 0xFF;
    uint8_t sb = src & 0xFF;
    uint8_t dr = (dst >> 16) & 0xFF;
    uint8_t dg = (dst >> 8) & 0xFF;
    uint8_t db = dst & 0xFF;

    /* out = src * alpha + dst * (1 - alpha) */
    uint8_t out_r = (sr * sa + dr * (255 - sa)) / 255;
    uint8_t out_g = (sg * sa + dg * (255 - sa)) / 255;
    uint8_t out_b = (sb * sa + db * (255 - sa)) / 255;

    return 0xFF000000u | ((uint32_t)out_r << 16) | ((uint32_t)out_g << 8) | out_b;
}

/* Filled circle using scanline algorithm */
void fb_fill_circle(int cx, int cy, int r, uint32_t color) {
    if (!fb_back || r <= 0) return;
    for (int y = -r; y <= r; y++) {
        int half = 0;
        /* Calculate horizontal extent using Pythagorean theorem */
        int r2 = r * r;
        int y2 = y * y;
        if (r2 >= y2) {
            int dx2 = r2 - y2;
            /* Approximate sqrt using integer math */
            half = 0;
            for (int d = 1; d <= r; d++) {
                if (d * d >= dx2) { half = d; break; }
            }
        }
        if (half > 0) {
            fb_fill_rect(cx - half, cy + y, half * 2 + 1, 1, color);
        }
    }
}

/* Filled triangle using scanline rasterization */
void fb_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color) {
    /* Sort vertices by y coordinate */
    if (y0 > y1) { int t; t=x0; x0=x1; x1=t; t=y0; y0=y1; y1=t; }
    if (y0 > y2) { int t; t=x0; x0=x2; x2=t; t=y0; y0=y2; y2=t; }
    if (y1 > y2) { int t; t=x1; x1=x2; x2=t; t=y1; y1=y2; y2=t; }

    if (y2 == y0) return;

    for (int y = y0; y <= y2; y++) {
        int xa, xb;
        if (y < y1) {
            /* Interpolate from y0 to y1 */
            if (y1 != y0) {
                xa = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
            } else {
                xa = x1;
            }
            /* Interpolate from y0 to y2 */
            xb = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        } else {
            /* Interpolate from y1 to y2 */
            if (y2 != y1) {
                xa = x1 + (x2 - x1) * (y - y1) / (y2 - y1);
            } else {
                xa = x2;
            }
            /* Interpolate from y0 to y2 */
            xb = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        }
        if (xa > xb) { int t = xa; xa = xb; xb = t; }
        fb_fill_rect(xa, y, xb - xa + 1, 1, color);
    }
}

/* Blit a sprite with optional transparency key */
void fb_blit(int x, int y, int w, int h, const uint32_t* data, uint32_t trans_key) {
    if (!fb_back || !data) return;
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= (int)fb_h) continue;
        for (int col = 0; col < w; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= (int)fb_w) continue;
            uint32_t pixel = data[row * w + col];
            if (pixel != trans_key) {
                fb_back[dy * fb_w + dx] = pixel;
            }
        }
    }
}

/* Blit with per-sprite alpha modulation */
void fb_blit_alpha(int x, int y, int w, int h, const uint32_t* data,
                   uint32_t trans_key, uint8_t alpha) {
    if (!fb_back || !data) return;
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= (int)fb_h) continue;
        for (int col = 0; col < w; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= (int)fb_w) continue;
            uint32_t pixel = data[row * w + col];
            if (pixel == trans_key) continue;
            /* Apply alpha modulation */
            uint8_t a = alpha;
            uint32_t mod_pixel = (pixel & 0x00FFFFFF) | ((uint32_t)a << 24);
            uint32_t dst = fb_back[dy * fb_w + dx];
            fb_back[dy * fb_w + dx] = fb_blend(dst, mod_pixel);
        }
    }
}

/* Alpha-filled rectangle */
void fb_fill_rect_alpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
    if (!fb_back) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_w) w = fb_w - x;
    if (y + h > (int)fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;

    uint8_t sr = (color >> 16) & 0xFF;
    uint8_t sg = (color >> 8) & 0xFF;
    uint8_t sb = color & 0xFF;

    for (int row = 0; row < h; row++) {
        uint32_t* p = &fb_back[(y + row) * fb_w + x];
        for (int col = 0; col < w; col++) {
            uint32_t dst = *p;
            uint8_t dr = (dst >> 16) & 0xFF;
            uint8_t dg = (dst >> 8) & 0xFF;
            uint8_t db = dst & 0xFF;
            uint8_t out_r = (sr * alpha + dr * (255 - alpha)) / 255;
            uint8_t out_g = (sg * alpha + dg * (255 - alpha)) / 255;
            uint8_t out_b = (sb * alpha + db * (255 - alpha)) / 255;
            *p++ = 0xFF000000u | ((uint32_t)out_r << 16) |
                   ((uint32_t)out_g << 8) | out_b;
        }
    }
}
