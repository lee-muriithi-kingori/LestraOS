/*
 * Lestra OS - Material App Grid (clickable)
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Renders the 16 Material-Design app icons from app_icons.c in a 8x2
 * grid on the desktop. Each icon is hit-tested on mouse-down; the
 * click is routed to:
 *
 *   - The native in-kernel widget creator (terminal_create, etc.)
 *     for apps that have one.
 *   - preinstalled_launch() for bundled apps (LibreOffice, Kdenlive,
 *     OBS, VLC) — which prints an honest "this requires Linux compat"
 *     message to the kernel log and returns a status string the UI
 *     can show in a dialog.
 *
 * This is the missing wiring that makes "every icon clickable" true
 * for real, instead of just visually.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/printk.h>
#include <string.h>

/* From app_icons.c */
extern void app_icon_draw(int x, int y, const uint8_t icon[32][32], int scale);
extern const uint8_t (*app_icon_get(const char* name))[32];

/* From preinstalled.c */
extern const char* preinstalled_launch(const char* id);
extern const struct preinstalled_app* preinstalled_get(int idx);
extern int preinstalled_count(void);

/* From compositor.c */
extern void compositor_add(struct widget* w);
extern void compositor_bring_to_front(struct widget* w);

/* Native widget creators (in-kernel, ring 0) */
extern struct widget* terminal_create(int x, int y);
extern struct widget* ailab_create(int x, int y);
extern struct widget* about_create(int x, int y);
extern struct widget* help_create(int x, int y);
extern struct widget* editor_create(int x, int y);
extern struct widget* media_create(int x, int y);
/* New widget creators (kernel/gui/app_widgets.c) */
extern struct widget* files_widget_create(int x, int y);
extern struct widget* browser_widget_create(int x, int y);
extern struct widget* calendar_widget_create(int x, int y);
extern struct widget* photos_widget_create(int x, int y);
extern struct widget* mail_widget_create(int x, int y);

#define GRID_COLS    8
#define GRID_ROWS    2
#define ICON_SIZE    56
#define ICON_LABEL_H 18
#define COL_GAP      60
#define ROW_GAP      90
#define GRID_X0      80
#define GRID_Y0      90

/* Each entry maps a desktop cell to (a) an icon key, (b) a display
 * label, (c) a launch handler kind. */
typedef enum {
    LAUNCH_NATIVE_TERMINAL,
    LAUNCH_NATIVE_AILAB,
    LAUNCH_NATIVE_EDITOR,
    LAUNCH_NATIVE_MEDIA,
    LAUNCH_NATIVE_ABOUT,
    LAUNCH_NATIVE_HELP,
    LAUNCH_NATIVE_FILES,
    LAUNCH_NATIVE_BROWSER,
    LAUNCH_NATIVE_CALENDAR,
    LAUNCH_NATIVE_PHOTOS,
    LAUNCH_NATIVE_MAIL,
    LAUNCH_BUNDLE,        /* preinstalled_launch(id) */
    LAUNCH_NONE,          /* no-op (e.g. settings not built yet) */
} launch_kind_t;

struct grid_entry {
    const char* icon_key;
    const char* label;
    const char* bundle_id;   /* only used for LAUNCH_BUNDLE */
    launch_kind_t kind;
};

/* Row 1: productivity + multimedia + internet */
/* Row 2: utilities + system */
static const struct grid_entry grid[GRID_ROWS][GRID_COLS] = {
    {
        { "writer",   "LibreOffice Writer", "libreoffice-writer", LAUNCH_BUNDLE },
        { "calc",     "LibreOffice Calc",   "libreoffice-calc",   LAUNCH_BUNDLE },
        { "impress",  "LibreOffice Impress","libreoffice-impress",LAUNCH_BUNDLE },
        { "video",    "Kdenlive",           "kdenlive",           LAUNCH_BUNDLE },
        { "video",    "OBS Studio",         "obs-studio",         LAUNCH_BUNDLE },
        { "media",    "VLC",                "vlc",                LAUNCH_BUNDLE },
        { "browser",  "Browser",            NULL,                 LAUNCH_NATIVE_BROWSER },
        { "mail",     "Mail",               NULL,                 LAUNCH_NATIVE_MAIL },
    },
    {
        { "calendar", "Calendar",           NULL,                 LAUNCH_NATIVE_CALENDAR },
        { "photos",   "Photos",             NULL,                 LAUNCH_NATIVE_PHOTOS },
        { "music",    "Music",              NULL,                 LAUNCH_NATIVE_MEDIA },
        { "terminal", "Terminal",           NULL,                 LAUNCH_NATIVE_TERMINAL },
        { "ai",       "AI Lab",             NULL,                 LAUNCH_NATIVE_AILAB },
        { "editor",   "Editor",             NULL,                 LAUNCH_NATIVE_EDITOR },
        { "files",    "Files",              NULL,                 LAUNCH_NATIVE_FILES },
        { "settings", "Settings",           NULL,                 LAUNCH_NATIVE_ABOUT },
    },
};

static int app_grid_inited = 0;

/* Track created native widgets so we don't open duplicates. */
static struct widget* w_terminal = NULL;
static struct widget* w_ailab    = NULL;
static struct widget* w_editor   = NULL;
static struct widget* w_media    = NULL;
static struct widget* w_about    = NULL;
static struct widget* w_help     = NULL;
static struct widget* w_files    = NULL;
static struct widget* w_browser  = NULL;
static struct widget* w_calendar = NULL;
static struct widget* w_photos   = NULL;
static struct widget* w_mail     = NULL;

void app_grid_init(void) {
    app_grid_inited = 1;
    pr_info("app_grid: initialized (%d icons, all clickable)\n",
            GRID_COLS * GRID_ROWS);
}

/* Compute (x, y) for a grid cell. */
static void grid_cell_pos(int row, int col, int* x, int* y) {
    *x = GRID_X0 + col * (ICON_SIZE + COL_GAP);
    *y = GRID_Y0 + row * (ICON_SIZE + ICON_LABEL_H + ROW_GAP);
}

void app_grid_render(void) {
    if (!app_grid_inited) app_grid_init();

    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            const struct grid_entry* e = &grid[r][c];
            int x, y;
            grid_cell_pos(r, c, &x, &y);

            /* Soft drop shadow under the icon */
            fb_fill_rect(x + 2, y + 2, ICON_SIZE, ICON_SIZE, 0x40000000);

            /* Icon (2x scale of 32x32 source = 64x64; we draw at ICON_SIZE) */
            const uint8_t (*icon)[32] = app_icon_get(e->icon_key);
            if (icon) {
                int scale = ICON_SIZE / 32;   /* 1 = 32x32, 2 = 64x64 */
                app_icon_draw(x, y, icon, scale);
            }

            /* Label below */
            int lw = fb_text_width(e->label);
            int lx = x + (ICON_SIZE - lw) / 2;
            int ly = y + ICON_SIZE + 4;
            /* Label background pill for readability over particles */
            fb_fill_rect(lx - 4, ly - 1, lw + 8, ICON_LABEL_H, 0xA00E1422);
            fb_draw_string(lx, ly, e->label, UI_TEXT_PRIMARY);
        }
    }
}

/* Launch a native widget. Avoid duplicates — if the widget already
 * exists, just bring it to front. */
static void launch_native(launch_kind_t kind) {
    struct widget* w = NULL;
    switch (kind) {
        case LAUNCH_NATIVE_TERMINAL:
            if (!w_terminal) {
                w_terminal = terminal_create(180, 40);
                if (w_terminal) compositor_add(w_terminal);
            }
            w = w_terminal;
            break;
        case LAUNCH_NATIVE_AILAB:
            if (!w_ailab) {
                w_ailab = ailab_create(200, 60);
                if (w_ailab) compositor_add(w_ailab);
            }
            w = w_ailab;
            break;
        case LAUNCH_NATIVE_EDITOR:
            if (!w_editor) {
                w_editor = editor_create(200, 60);
                if (w_editor) compositor_add(w_editor);
            }
            w = w_editor;
            break;
        case LAUNCH_NATIVE_MEDIA:
            if (!w_media) {
                w_media = media_create(250, 100);
                if (w_media) compositor_add(w_media);
            }
            w = w_media;
            break;
        case LAUNCH_NATIVE_ABOUT:
            if (!w_about) {
                w_about = about_create((int)fb_w / 2 - 190, 200);
                if (w_about) compositor_add(w_about);
            }
            w = w_about;
            break;
        case LAUNCH_NATIVE_HELP:
            if (!w_help) {
                w_help = help_create((int)fb_w / 2 - 200, 180);
                if (w_help) compositor_add(w_help);
            }
            w = w_help;
            break;
        case LAUNCH_NATIVE_FILES:
            if (!w_files) {
                w_files = files_widget_create(200, 80);
                if (w_files) compositor_add(w_files);
            }
            w = w_files;
            break;
        case LAUNCH_NATIVE_BROWSER:
            if (!w_browser) {
                w_browser = browser_widget_create(160, 60);
                if (w_browser) compositor_add(w_browser);
            }
            w = w_browser;
            break;
        case LAUNCH_NATIVE_CALENDAR:
            if (!w_calendar) {
                w_calendar = calendar_widget_create(
                    (int)fb_w / 2 - 140, 100);
                if (w_calendar) compositor_add(w_calendar);
            }
            w = w_calendar;
            break;
        case LAUNCH_NATIVE_PHOTOS:
            if (!w_photos) {
                w_photos = photos_widget_create(300, 80);
                if (w_photos) compositor_add(w_photos);
            }
            w = w_photos;
            break;
        case LAUNCH_NATIVE_MAIL:
            if (!w_mail) {
                w_mail = mail_widget_create(220, 120);
                if (w_mail) compositor_add(w_mail);
            }
            w = w_mail;
            break;
        default:
            return;
    }
    if (w) {
        w->visible = 1;
        compositor_bring_to_front(w);
    }
}

int app_grid_handle_click(int x, int y) {
    if (!app_grid_inited) app_grid_init();

    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            int ix, iy;
            grid_cell_pos(r, c, &ix, &iy);
            if (x >= ix && x < ix + ICON_SIZE &&
                y >= iy && y < iy + ICON_SIZE + ICON_LABEL_H) {
                const struct grid_entry* e = &grid[r][c];
                switch (e->kind) {
                    case LAUNCH_NATIVE_TERMINAL:
                    case LAUNCH_NATIVE_AILAB:
                    case LAUNCH_NATIVE_EDITOR:
                    case LAUNCH_NATIVE_MEDIA:
                    case LAUNCH_NATIVE_ABOUT:
                    case LAUNCH_NATIVE_HELP:
                    case LAUNCH_NATIVE_FILES:
                    case LAUNCH_NATIVE_BROWSER:
                    case LAUNCH_NATIVE_CALENDAR:
                    case LAUNCH_NATIVE_PHOTOS:
                    case LAUNCH_NATIVE_MAIL:
                        pr_info("app_grid: launching native '%s'\n", e->label);
                        launch_native(e->kind);
                        return 1;
                    case LAUNCH_BUNDLE:
                        /* LibreOffice / Kdenlive / OBS / VLC — pre-staged
                         * in /opt/ but need Linux compat to actually run.
                         * The launcher returns an honest status string. */
                        pr_info("app_grid: launching bundle '%s' (%s)\n",
                                e->label, e->bundle_id);
                        preinstalled_launch(e->bundle_id);
                        /* Open the About widget as the dialog surface so
                         * the user sees *something* happen. In a real
                         * port this would be a dedicated "bundle status"
                         * dialog. */
                        launch_native(LAUNCH_NATIVE_ABOUT);
                        return 1;
                    case LAUNCH_NONE:
                        pr_info("app_grid: '%s' has no launcher yet\n",
                                e->label);
                        return 1;  /* consume the click so it doesn't
                                    * fall through to legacy icons */
                }
            }
        }
    }
    return 0;
}
