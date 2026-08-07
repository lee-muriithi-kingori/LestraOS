/*
 * Lestra OS - Files / Browser / Calendar / Photos widgets
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Four real desktop app widgets that previously had icons but no
 * backing implementation. Each is a draggable window with its own
 * event handler. Together they make the desktop actually useful
 * instead of "click icon → no launcher yet".
 *
 *   Files    — lists VFS files, click to cat in-place
 *   Browser  — URL input + fetch via http_get, renders text
 *   Calendar — shows current month grid using RTC
 *   Photos   — lists VFS images, click to display (RGB888 only)
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/timer.h>
#include <lestra/printk.h>
#include <lestra/vfs.h>
#include <lestra/net.h>
#include <string.h>

/* Forward decls from compositor.c */
extern void compositor_add(struct widget* w);
extern void compositor_bring_to_front(struct widget* w);

/* ---- shared helpers ---- */
static void draw_window_frame(int x, int y, int w, int h, const char* title) {
    extern void ui_draw_card(int x, int y, int w, int h, int focused);
    ui_draw_card(x, y, w, h, 1);
    /* Title bar text */
    fb_draw_string(x + 16, y + 10, title, UI_TEXT_PRIMARY);
    /* Close button (top-right X) */
    fb_fill_rect(x + w - 24, y + 10, 16, 16, 0xFFF87171);
    fb_draw_string(x + w - 19, y + 12, "X", UI_TEXT_PRIMARY);
}

static int click_close_button(struct widget* w, int mx, int my) {
    int cx = w->x + w->w - 24;
    int cy = w->y + 10;
    return (mx >= cx && mx < cx + 16 && my >= cy && my < cy + 16);
}

/* ============================================================
 * FILES WIDGET
 * ============================================================ */
static struct widget files_widget;
static int files_inited = 0;

#define FILES_LIST_Y_OFFSET 50
#define FILES_LIST_MAX      20
static char files_names[FILES_LIST_MAX][MAX_NAME_LEN];
static int  files_count = 0;
static int  files_selected = -1;
static char files_preview[4096];
static int  files_preview_len = 0;

static void files_refresh_list(void) {
    files_count = 0;
    struct dirent entry;
    entry.inode = 0;
    while (vfs_readdir(0, &entry) == 0 && files_count < FILES_LIST_MAX) {
        strncpy(files_names[files_count], entry.name, MAX_NAME_LEN - 1);
        files_names[files_count][MAX_NAME_LEN - 1] = '\0';
        files_count++;
    }
}

static void files_show_preview(const char* path) {
    int fd = vfs_open(path, 0);
    if (fd < 0) {
        files_preview_len = ksnprintf(files_preview, sizeof(files_preview),
                                       "(cannot open %s)", path);
        return;
    }
    ssize_t n = vfs_read(fd, files_preview, sizeof(files_preview) - 1);
    vfs_close(fd);
    if (n < 0) n = 0;
    files_preview_len = (int)n;
    files_preview[n] = '\0';
}

static void files_draw(struct widget* w) {
    draw_window_frame(w->x, w->y, w->w, w->h, "Files");

    /* Left panel: file list */
    int lx = w->x + 16;
    int ly = w->y + FILES_LIST_Y_OFFSET;
    fb_fill_rect(lx, ly, 200, w->h - FILES_LIST_Y_OFFSET - 16, 0xFF1E293B);
    fb_draw_string(lx + 8, ly + 6, "Name", UI_TEXT_PRIMARY);

    for (int i = 0; i < files_count; i++) {
        int ry = ly + 24 + i * 16;
        if (ry > w->y + w->h - 24) break;
        if (i == files_selected) {
            fb_fill_rect(lx, ry - 2, 200, 16, 0xFF06B6D4);
        }
        fb_draw_string(lx + 8, ry, files_names[i], UI_TEXT_PRIMARY);
    }

    /* Right panel: preview */
    int rx = lx + 210;
    int rw = w->w - 210 - 32;
    fb_fill_rect(rx, ly, rw, w->h - FILES_LIST_Y_OFFSET - 16, 0xFF0F172A);
    fb_draw_string(rx + 8, ly + 6, "Preview", UI_TEXT_PRIMARY);

    if (files_selected >= 0 && files_preview_len > 0) {
        /* Render text wrapped at ~64 chars per line. */
        int cx = rx + 8;
        int cy = ly + 24;
        int col = 0;
        for (int i = 0; i < files_preview_len; i++) {
            char c = files_preview[i];
            if (c == '\n') {
                cx = rx + 8;
                cy += 14;
                col = 0;
                continue;
            }
            if (col >= 64) {
                cx = rx + 8;
                cy += 14;
                col = 0;
            }
            if (cy > w->y + w->h - 24) break;
            if (c >= 0x20 && c < 0x7F) {
                fb_draw_char(cx, cy, c, UI_TEXT_PRIMARY);
                cx += 8;
                col++;
            }
        }
    } else if (files_selected < 0) {
        fb_draw_string(rx + 8, ly + 24, "(select a file)", 0xFF94A3B8);
    }
}

static void files_on_event(struct widget* w, struct event* e) {
    if (e->type == EV_MOUSE_DOWN) {
        if (click_close_button(w, e->mouse.x, e->mouse.y)) {
            w->visible = 0;
            return;
        }
        /* Click in file list? */
        int lx = w->x + 16;
        int ly = w->y + FILES_LIST_Y_OFFSET + 24;
        if (e->mouse.x >= lx && e->mouse.x < lx + 200 &&
            e->mouse.y >= ly &&
            e->mouse.y < ly + files_count * 16) {
            int idx = (e->mouse.y - ly) / 16;
            if (idx >= 0 && idx < files_count) {
                files_selected = idx;
                files_show_preview(files_names[idx]);
            }
        }
    }
}

struct widget* files_widget_create(int x, int y) {
    if (!files_inited) {
        files_refresh_list();
        files_inited = 1;
    }
    files_widget.x = x;
    files_widget.y = y;
    files_widget.w = 600;
    files_widget.h = 360;
    files_widget.visible = 1;
    files_widget.focused = 1;
    files_widget.draggable = 1;
    files_widget.resizable = 0;
    files_widget.draw = files_draw;
    files_widget.on_event = files_on_event;
    files_widget.state = NULL;
    memcpy(files_widget.title, "Files", 6);
    return &files_widget;
}

/* ============================================================
 * BROWSER WIDGET
 * ============================================================ */
static struct widget browser_widget;
static int browser_inited = 0;

#define BR_URL_MAX 256
static char br_url[BR_URL_MAX] = "http://example.com/";
static int  br_url_len = 24;
static char br_page[8192];
static int  br_page_len = 0;
static int  br_url_focused = 1;

static void br_fetch(void) {
    /* Use the real http_get from kernel/net/http.c. */
    extern int http_get(const char* url, struct http_response* resp);
    extern int net_is_up(void);

    br_page_len = ksnprintf(br_page, sizeof(br_page),
        "[fetching %s...]\n", br_url);

    if (!net_is_up()) {
        br_page_len += ksnprintf(br_page + br_page_len,
                                  sizeof(br_page) - br_page_len,
                                  "Network is down. Use 'network' in terminal to bring up DHCP.\n");
        return;
    }
    static struct http_response resp;   /* lives in BSS — big but ok */
    int rc = http_get(br_url, &resp);
    if (rc != 0) {
        br_page_len += ksnprintf(br_page + br_page_len,
                                  sizeof(br_page) - br_page_len,
                                  "Fetch failed (rc=%d).\n", rc);
        return;
    }
    br_page_len = ksnprintf(br_page, sizeof(br_page),
        "HTTP %u\n\n", (unsigned)resp.status);
    int body_copy = (int)resp.body_len;
    if (body_copy > (int)sizeof(br_page) - br_page_len - 1) {
        body_copy = (int)sizeof(br_page) - br_page_len - 1;
    }
    memcpy(br_page + br_page_len, resp.body, body_copy);
    br_page_len += body_copy;
    br_page[br_page_len] = '\0';
}

static void browser_draw(struct widget* w) {
    draw_window_frame(w->x, w->y, w->w, w->h, "Browser");

    /* URL bar */
    int ux = w->x + 16;
    int uy = w->y + 40;
    int uw = w->w - 32;
    fb_fill_rect(ux, uy, uw, 28, 0xFF1E293B);
    if (br_url_focused && ((timer_get_ms() / 500) % 2 == 0)) {
        /* Blinking cursor at end of URL. */
        fb_fill_rect(ux + 8 + br_url_len * 8, uy + 6, 2, 16, UI_ACCENT);
    }
    fb_draw_string(ux + 8, uy + 8, br_url, UI_TEXT_PRIMARY);

    /* Page area */
    int px = ux;
    int py = uy + 36;
    int pw = uw;
    int ph = w->h - 40 - 36 - 16;
    fb_fill_rect(px, py, pw, ph, 0xFFFFFFFF);  /* white page bg */
    if (br_page_len == 0) {
        fb_draw_string(px + 8, py + 8, "(no page loaded — type URL and press Enter)",
                       0xFF6B7280);
        return;
    }
    /* Render text on white bg (black ink). */
    int cx = px + 8, cy = py + 8, col = 0;
    int max_col = (pw - 16) / 8;
    for (int i = 0; i < br_page_len; i++) {
        char c = br_page[i];
        if (c == '\n') { cx = px + 8; cy += 14; col = 0; continue; }
        if (col >= max_col) { cx = px + 8; cy += 14; col = 0; }
        if (cy > py + ph - 14) break;
        if (c >= 0x20 && c < 0x7F) {
            fb_draw_char(cx, cy, c, 0xFF000000);
            cx += 8; col++;
        }
    }
}

static void browser_on_event(struct widget* w, struct event* e) {
    if (e->type == EV_MOUSE_DOWN) {
        if (click_close_button(w, e->mouse.x, e->mouse.y)) {
            w->visible = 0;
            return;
        }
        /* Click URL bar = focus. */
        int ux = w->x + 16, uy = w->y + 40, uw = w->w - 32;
        if (e->mouse.x >= ux && e->mouse.x < ux + uw &&
            e->mouse.y >= uy && e->mouse.y < uy + 28) {
            br_url_focused = 1;
        } else {
            br_url_focused = 0;
        }
    } else if (e->type == EV_KEY_DOWN && br_url_focused) {
        char c = 0;
        extern int keyboard_has_key(void);
        extern char keyboard_getchar(void);
        if (keyboard_has_key()) {
            c = keyboard_getchar();
        }
        if (c == '\n') {
            br_fetch();
        } else if (c == '\b') {
            if (br_url_len > 0) {
                br_url[--br_url_len] = '\0';
            }
        } else if (c >= 0x20 && c < 0x7F && br_url_len < BR_URL_MAX - 1) {
            br_url[br_url_len++] = c;
            br_url[br_url_len] = '\0';
        }
    }
}

struct widget* browser_widget_create(int x, int y) {
    if (!browser_inited) {
        br_page_len = ksnprintf(br_page, sizeof(br_page),
            "Lestra Browser 1.0\n"
            "==================\n\n"
            "Type a URL in the address bar and press Enter.\n"
            "HTTP and HTTPS are both supported (TLS via P-256 ECDHE).\n"
            "Pages render as plain text — no HTML parsing yet.\n");
        browser_inited = 1;
    }
    browser_widget.x = x;
    browser_widget.y = y;
    browser_widget.w = 640;
    browser_widget.h = 440;
    browser_widget.visible = 1;
    browser_widget.focused = 1;
    browser_widget.draggable = 1;
    browser_widget.resizable = 0;
    browser_widget.draw = browser_draw;
    browser_widget.on_event = browser_on_event;
    browser_widget.state = NULL;
    memcpy(browser_widget.title, "Browser", 8);
    return &browser_widget;
}

/* ============================================================
 * CALENDAR WIDGET
 * ============================================================ */
static struct widget calendar_widget;

static int calendar_month_start_dow(int year, int month) {
    /* Zeller's congruence: returns 0=Sunday..6=Saturday for day 1. */
    int q = 1;
    int m = month;
    int y = year;
    if (m < 3) { m += 12; y -= 1; }
    int k = y % 100;
    int j = y / 100;
    int h = (q + (13 * (m + 1)) / 5 + k + (k / 4) + (j / 4) + 5 * j) % 7;
    /* Zeller returns 0=Saturday..6=Friday; convert to 0=Sunday. */
    return (h + 6) % 7;
}

static int calendar_days_in_month(int year, int month) {
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 2) {
        int leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return days[month - 1];
}

static void calendar_draw(struct widget* w) {
    draw_window_frame(w->x, w->y, w->w, w->h, "Calendar");

    extern void rtc_get_time(uint8_t*, uint8_t*, uint8_t*);
    extern void rtc_get_date(uint16_t*, uint8_t*, uint8_t*);
    uint8_t hh, mm, ss, day, month;
    uint16_t year;
    rtc_get_time(&hh, &mm, &ss);
    rtc_get_date(&year, &month, &day);

    /* Month + year header */
    static const char* month_names[] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    char hdr[32];
    ksnprintf(hdr, sizeof(hdr), "%s %u", month_names[month - 1], (unsigned)year);
    fb_draw_string(w->x + (w->w - fb_text_width(hdr)) / 2, w->y + 40, hdr,
                   UI_TEXT_PRIMARY);

    /* Day-of-week header row */
    static const char* dow[] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
    int grid_x = w->x + 16;
    int grid_y = w->y + 70;
    int cell_w = (w->w - 32) / 7;
    for (int i = 0; i < 7; i++) {
        fb_draw_string(grid_x + i * cell_w + (cell_w - 16) / 2,
                       grid_y, dow[i], 0xFF94A3B8);
    }

    /* Day cells */
    int first_dow = calendar_month_start_dow(year, month);
    int days = calendar_days_in_month(year, month);
    int row = 1, col = first_dow;
    for (int d = 1; d <= days; d++) {
        int cx = grid_x + col * cell_w + (cell_w - 16) / 2;
        int cy = grid_y + row * 24;
        if (d == day) {
            /* Highlight today. */
            fb_fill_rect(cx - 4, cy - 2, 24, 18, 0xFF22D3EE);
            fb_draw_string(cx, cy, (char[3]){(char)('0' + d / 10), (char)('0' + d % 10), 0},
                           0xFF000000);
        } else {
            char buf[3] = {(char)('0' + d / 10), (char)('0' + d % 10), 0};
            fb_draw_string(cx, cy, buf, UI_TEXT_PRIMARY);
        }
        col++;
        if (col >= 7) { col = 0; row++; }
    }

    /* Time at the bottom. */
    char time_buf[16];
    ksnprintf(time_buf, sizeof(time_buf), "%02u:%02u:%02u",
              (unsigned)hh, (unsigned)mm, (unsigned)ss);
    fb_draw_string(w->x + (w->w - fb_text_width(time_buf)) / 2,
                   w->y + w->h - 24, time_buf, UI_ACCENT);
}

static void calendar_on_event(struct widget* w, struct event* e) {
    if (e->type == EV_MOUSE_DOWN) {
        if (click_close_button(w, e->mouse.x, e->mouse.y)) {
            w->visible = 0;
            return;
        }
    }
}

struct widget* calendar_widget_create(int x, int y) {
    calendar_widget.x = x;
    calendar_widget.y = y;
    calendar_widget.w = 280;
    calendar_widget.h = 280;
    calendar_widget.visible = 1;
    calendar_widget.focused = 1;
    calendar_widget.draggable = 1;
    calendar_widget.resizable = 0;
    calendar_widget.draw = calendar_draw;
    calendar_widget.on_event = calendar_on_event;
    calendar_widget.state = NULL;
    memcpy(calendar_widget.title, "Calendar", 9);
    return &calendar_widget;
}

/* ============================================================
 * PHOTOS WIDGET
 * ============================================================ */
static struct widget photos_widget;
static int photos_inited = 0;
static int photos_selected = -1;

static void photos_draw(struct widget* w) {
    draw_window_frame(w->x, w->y, w->w, w->h, "Photos");
    int bx = w->x + 16;
    int by = w->y + 50;

    if (!photos_inited) {
        /* Refresh file list. */
        photos_inited = 1;
    }
    /* List VFS files (we don't filter by extension since VFS has no
     * metadata; the user clicks each to see what it is). */
    extern void desktop_icons_render(void);   /* unused, just to silence */
    (void)desktop_icons_render;

    static char photo_names[20][MAX_NAME_LEN];
    static int  photo_count = 0;
    photo_count = 0;
    struct dirent entry;
    entry.inode = 0;
    while (vfs_readdir(0, &entry) == 0 && photo_count < 20) {
        strncpy(photo_names[photo_count], entry.name, MAX_NAME_LEN - 1);
        photo_names[photo_count][MAX_NAME_LEN - 1] = '\0';
        photo_count++;
    }

    fb_draw_string(bx, by, "Click a file to view:", UI_TEXT_PRIMARY);
    for (int i = 0; i < photo_count; i++) {
        int ry = by + 20 + i * 16;
        if (ry > w->y + w->h - 100) break;
        if (i == photos_selected) {
            fb_fill_rect(bx, ry - 2, w->w - 32, 16, 0xFF06B6D4);
        }
        fb_draw_string(bx + 8, ry, photo_names[i], UI_TEXT_PRIMARY);
    }

    /* Preview area at bottom. */
    int py = w->y + w->h - 80;
    fb_fill_rect(bx, py, w->w - 32, 64, 0xFF0F172A);
    if (photos_selected >= 0 && photos_selected < photo_count) {
        /* Read first 64 bytes and try to interpret as RGB888 image. */
        int fd = vfs_open(photo_names[photos_selected], 0);
        if (fd >= 0) {
            static uint8_t buf[64 * 64 * 3];
            ssize_t n = vfs_read(fd, buf, sizeof(buf));
            vfs_close(fd);
            if (n > 100) {
                /* Render as 64x64 thumbnail. */
                for (int y = 0; y < 64; y++) {
                    for (int x = 0; x < 64; x++) {
                        int idx = (y * 64 + x) * 3;
                        if (idx + 2 >= n) break;
                        uint32_t color = 0xFF000000u |
                                          ((uint32_t)buf[idx] << 16) |
                                          ((uint32_t)buf[idx + 1] << 8) |
                                          buf[idx + 2];
                        fb_set_pixel(bx + x, py + y, color);
                    }
                }
            } else {
                fb_draw_string(bx + 8, py + 24, "(file too small or not an image)",
                               0xFF94A3B8);
            }
        }
    } else {
        fb_draw_string(bx + 8, py + 24, "(no file selected)", 0xFF94A3B8);
    }
}

static void photos_on_event(struct widget* w, struct event* e) {
    if (e->type == EV_MOUSE_DOWN) {
        if (click_close_button(w, e->mouse.x, e->mouse.y)) {
            w->visible = 0;
            return;
        }
        /* Click in file list. */
        int bx = w->x + 16;
        int by = w->y + 70;
        if (e->mouse.x >= bx && e->mouse.x < bx + w->w - 32 &&
            e->mouse.y >= by) {
            int idx = (e->mouse.y - by) / 16;
            if (idx >= 0 && idx < 20) {
                photos_selected = idx;
            }
        }
    }
}

struct widget* photos_widget_create(int x, int y) {
    photos_widget.x = x;
    photos_widget.y = y;
    photos_widget.w = 400;
    photos_widget.h = 360;
    photos_widget.visible = 1;
    photos_widget.focused = 1;
    photos_widget.draggable = 1;
    photos_widget.resizable = 0;
    photos_widget.draw = photos_draw;
    photos_widget.on_event = photos_on_event;
    photos_widget.state = NULL;
    memcpy(photos_widget.title, "Photos", 7);
    return &photos_widget;
}

/* ============================================================
 * Mail widget — UI only (IMAP/SMTP need real protocols)
 * ============================================================ */
static struct widget mail_widget;

static void mail_draw(struct widget* w) {
    draw_window_frame(w->x, w->y, w->w, w->h, "Mail");
    int bx = w->x + 16;
    int by = w->y + 50;

    fb_draw_string(bx, by,
        "Lestra Mail 1.0", UI_TEXT_PRIMARY);
    fb_draw_string(bx, by + 20,
        "============", UI_TEXT_PRIMARY);
    fb_draw_string(bx, by + 40,
        "IMAP/SMTP not implemented yet.", 0xFF94A3B8);
    fb_draw_string(bx, by + 56,
        "To enable email:", 0xFF94A3B8);
    fb_draw_string(bx, by + 72,
        "  1. Port an IMAP client (e.g. libetpan)", 0xFF94A3B8);
    fb_draw_string(bx, by + 88,
        "  2. Use TLS for IMAPS/SMTPS (already implemented)", 0xFF94A3B8);
    fb_draw_string(bx, by + 104,
        "  3. Add a 'mail' shell command that fetches inbox", 0xFF94A3B8);
    fb_draw_string(bx, by + 132,
        "For now, use the Browser widget to access webmail.", UI_TEXT_PRIMARY);
}

static void mail_on_event(struct widget* w, struct event* e) {
    if (e->type == EV_MOUSE_DOWN) {
        if (click_close_button(w, e->mouse.x, e->mouse.y)) {
            w->visible = 0;
            return;
        }
    }
}

struct widget* mail_widget_create(int x, int y) {
    mail_widget.x = x;
    mail_widget.y = y;
    mail_widget.w = 480;
    mail_widget.h = 240;
    mail_widget.visible = 1;
    mail_widget.focused = 1;
    mail_widget.draggable = 1;
    mail_widget.resizable = 0;
    mail_widget.draw = mail_draw;
    mail_widget.on_event = mail_on_event;
    mail_widget.state = NULL;
    memcpy(mail_widget.title, "Mail", 5);
    return &mail_widget;
}
