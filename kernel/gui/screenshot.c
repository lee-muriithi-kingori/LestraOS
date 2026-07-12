/*
 * Lestra OS - Screenshot Tool
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Win+Shift+S enters "region select" mode: the screen dims and the
 * user drags a rectangle. On mouse-up, the pixels inside that
 * rectangle are written to /tmp/screenshot.rgb as raw RGB888.
 *
 * State machine:
 *   IDLE  -> Win+Shift+S -> ARMED
 *   ARMED -> mouse-down  -> DRAGGING (anchor = mouse pos)
 *   DRAGGING -> mouse-up -> CAPTURE -> IDLE
 *
 * The rectangle preview is drawn in cyan on top of the dimmed screen.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>
#include <lestra/vfs.h>
#include <lestra/mm.h>
#include <lestra/printk.h>
#include <string.h>

enum {
    SS_IDLE = 0,
    SS_ARMED,
    SS_DRAGGING,
};

struct ss_state {
    int mode;
    int anchor_x, anchor_y;
    int cur_x, cur_y;
    uint64_t last_capture_ms;
    char last_path[64];
};

static struct ss_state ss_state;

/* ---------- helpers ---------- */
static void ss_rect(int* x, int* y, int* w, int* h) {
    int x0 = ss_state.anchor_x, y0 = ss_state.anchor_y;
    int x1 = ss_state.cur_x,    y1 = ss_state.cur_y;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    *x = x0; *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
}

static void ss_capture(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    /* Allocate a buffer in the kernel heap. */
    size_t bytes = (size_t)w * h * 3;
    uint8_t* buf = (uint8_t*)kmalloc(bytes);
    if (!buf) {
        pr_warn("screenshot: out of memory for %u bytes\n", (unsigned)bytes);
        return;
    }
    /* Copy pixels from the framebuffer back buffer. */
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int px = x + col;
            int py = y + row;
            if (px < 0 || px >= (int)fb_w || py < 0 || py >= (int)fb_h) {
                buf[(row * w + col) * 3 + 0] = 0;
                buf[(row * w + col) * 3 + 1] = 0;
                buf[(row * w + col) * 3 + 2] = 0;
                continue;
            }
            uint32_t c = fb_get_pixel(px, py);
            buf[(row * w + col) * 3 + 0] = (uint8_t)((c >> 16) & 0xFF); /* R */
            buf[(row * w + col) * 3 + 1] = (uint8_t)((c >> 8)  & 0xFF); /* G */
            buf[(row * w + col) * 3 + 2] = (uint8_t)(c & 0xFF);         /* B */
        }
    }
    /* Write to /tmp/screenshot.rgb via VFS. */
    const char* path = "/tmp/screenshot.rgb";
    int fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        pr_warn("screenshot: cannot open %s for write\n", path);
        kfree(buf);
        return;
    }
    /* Write a small header: "LSCS" + width(4) + height(4) */
    vfs_write(fd, "LSCS", 4);
    uint8_t hdr[8];
    hdr[0] = (uint8_t)(w & 0xFF); hdr[1] = (uint8_t)((w >> 8) & 0xFF);
    hdr[2] = (uint8_t)((w >> 16) & 0xFF); hdr[3] = (uint8_t)((w >> 24) & 0xFF);
    hdr[4] = (uint8_t)(h & 0xFF); hdr[5] = (uint8_t)((h >> 8) & 0xFF);
    hdr[6] = (uint8_t)((h >> 16) & 0xFF); hdr[7] = (uint8_t)((h >> 24) & 0xFF);
    vfs_write(fd, hdr, 8);
    /* Write RGB data in chunks of 4 KB. */
    size_t off = 0;
    while (off < bytes) {
        size_t chunk = bytes - off;
        if (chunk > 4096) chunk = 4096;
        ssize_t wrote = vfs_write(fd, buf + off, chunk);
        if (wrote <= 0) break;
        off += (size_t)wrote;
    }
    vfs_close(fd);
    kfree(buf);

    ss_state.last_capture_ms = timer_get_ms();
    strncpy(ss_state.last_path, path, sizeof(ss_state.last_path) - 1);
    ss_state.last_path[sizeof(ss_state.last_path) - 1] = '\0';
    pr_info("screenshot: captured %dx%d -> %s\n", w, h, path);
}

/* ---------- public API ---------- */
void screenshot_enter_mode(void) {
    ss_state.mode = SS_ARMED;
    ss_state.anchor_x = ss_state.cur_x = 0;
    ss_state.anchor_y = ss_state.cur_y = 0;
    pr_info("screenshot: enter region-select mode (drag a box)\n");
}

int screenshot_is_active(void) {
    return ss_state.mode != SS_IDLE;
}

void screenshot_render(void) {
    if (ss_state.mode == SS_IDLE) return;

    /* Dim the whole screen. */
    fb_fill_rect(0, 0, (int)fb_w, (int)fb_h, 0x80050608u);

    if (ss_state.mode == SS_DRAGGING) {
        int rx, ry, rw, rh;
        ss_rect(&rx, &ry, &rw, &rh);
        if (rw > 0 && rh > 0) {
            /* Re-brighten the selection so it stands out. */
            for (int y = ry; y < ry + rh; y++) {
                for (int x = rx; x < rx + rw; x++) {
                    if (x < 0 || x >= (int)fb_w ||
                        y < 0 || y >= (int)fb_h) continue;
                    /* Sample original from fb_back. fb_get_pixel reads
                     * the back buffer (post-blit state). */
                    uint32_t c = fb_get_pixel(x, y);
                    fb_set_pixel(x, y, c);
                }
            }
            /* Cyan border around the selection. */
            fb_draw_rect(rx, ry, rw, rh, UI_ACCENT);
            /* Cross-hair lines through the cursor. */
            fb_draw_line(ss_state.cur_x, ry,
                         ss_state.cur_x, ry + rh, 0x4022D3EEu);
            fb_draw_line(rx, ss_state.cur_y,
                         rx + rw, ss_state.cur_y, 0x4022D3EEu);
            /* Size label. */
            char sz[32];
            ksnprintf(sz, sizeof(sz), "%dx%d", rw, rh);
            fb_draw_string(rx + 4, ry + rh + 4, sz, UI_ACCENT);
        }
    }

    /* Hint at the top. */
    const char* hint = (ss_state.mode == SS_ARMED)
                       ? "Drag to select region. Esc to cancel."
                       : "Release to capture. Esc to cancel.";
    int hw = fb_text_width(hint);
    fb_fill_rect(((int)fb_w - hw) / 2 - 8, 12, hw + 16, 22, 0xE00E1422u);
    fb_draw_string(((int)fb_w - hw) / 2, 16, hint, UI_TEXT_PRIMARY);
}

int screenshot_handle_event(struct event* e) {
    if (ss_state.mode == SS_IDLE) {
        /* Win+Shift+S arms us. */
        if (e->type == EV_KEY_DOWN &&
            (e->key.mods & MOD_SUPER) &&
            (e->key.mods & MOD_SHIFT) &&
            e->key.scancode == 0x1F /* 'S' */) {
            screenshot_enter_mode();
            return 1;
        }
        return 0;
    }
    /* Active: consume all events so the user can't accidentally interact
     * with widgets behind the dim overlay. */
    if (e->type == EV_KEY_DOWN && e->key.scancode == KEY_ESC) {
        ss_state.mode = SS_IDLE;
        return 1;
    }
    if (e->type == EV_MOUSE_MOVE) {
        ss_state.cur_x = e->mouse.x;
        ss_state.cur_y = e->mouse.y;
        return 1;
    }
    if (e->type == EV_MOUSE_DOWN) {
        ss_state.mode = SS_DRAGGING;
        ss_state.anchor_x = e->mouse.x;
        ss_state.anchor_y = e->mouse.y;
        ss_state.cur_x = e->mouse.x;
        ss_state.cur_y = e->mouse.y;
        return 1;
    }
    if (e->type == EV_MOUSE_UP) {
        int rx, ry, rw, rh;
        ss_rect(&rx, &ry, &rw, &rh);
        ss_state.mode = SS_IDLE;
        if (rw >= 2 && rh >= 2) {
            ss_capture(rx, ry, rw, rh);
        }
        return 1;
    }
    return 1;  /* swallow */
}
