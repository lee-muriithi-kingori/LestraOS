/*
 * Lestra OS - App Grid (clean, working)
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/gui.h>
#include <lestra/printk.h>
#include <string.h>

extern void compositor_add(struct widget* w);
extern void compositor_bring_to_front(struct widget* w);

extern struct widget* terminal_create(int, int);
extern struct widget* editor_create(int, int);
extern struct widget* files_widget_create(int, int);
extern struct widget* browser_widget_create(int, int);
extern struct widget* calendar_widget_create(int, int);
extern struct widget* photos_widget_create(int, int);
extern struct widget* mail_widget_create(int, int);
extern struct widget* media_create(int, int);
extern struct widget* ailab_create(int, int);
extern struct widget* settings_app_create(int, int);
extern struct widget* about_create(int, int);
extern struct widget* help_create(int, int);

#define COLS 6
#define ROWS 2
#define ISIZE 64
#define LABEL_H 18
#define GAP_X 40
#define GAP_Y 70
#define START_X 60
#define START_Y 70

typedef enum { L_TERM, L_EDITOR, L_FILES, L_BROWSER, L_CAL, L_PHOTOS,
               L_MAIL, L_MEDIA, L_AILAB, L_SETTINGS, L_ABOUT, L_HELP } LKind;

struct Entry { const char* icon; const char* label; LKind kind; };

static const struct Entry grid[ROWS][COLS] = {
    { {"terminal","Terminal",L_TERM}, {"editor","Editor",L_EDITOR}, {"files","Files",L_FILES}, {"browser","Browser",L_BROWSER}, {"calendar","Calendar",L_CAL}, {"photos","Photos",L_PHOTOS} },
    { {"mail","Mail",L_MAIL}, {"media","Media",L_MEDIA}, {"ai","AI Lab",L_AILAB}, {"settings","Settings",L_SETTINGS}, {"about","About",L_ABOUT}, {"help","Help",L_HELP} }
};

extern void app_icon_draw(int, int, const uint8_t[32][32], int);
extern const uint8_t (*app_icon_get(const char*))[32];

static struct widget* singletons[12] = {0};

static void launch(LKind k) {
    struct widget* w = NULL; struct widget** slot = NULL;
    switch (k) {
        case L_TERM: slot=&singletons[0]; if(!*slot){*slot=terminal_create(100,50);if(*slot)compositor_add(*slot);} w=*slot; break;
        case L_EDITOR: slot=&singletons[1]; if(!*slot){*slot=editor_create(120,60);if(*slot)compositor_add(*slot);} w=*slot; break;
        case L_FILES: slot=&singletons[2]; if(!*slot){*slot=files_widget_create(140,70);if(*slot)compositor_add(*slot);} w=*slot; break;
        case L_BROWSER: slot=&singletons[3]; if(!*slot){*slot=browser_widget_create(160,80);if(*slot)compositor_add(*slot);} w=*slot; break;
        case L_CAL: slot=&singletons[4]; if(!*slot){*slot=calendar_widget_create(fb_w/2-140,100);if(*slot)compositor_add(*slot);} w=*slot; break;
        case L_PHOTOS: slot=&singletons[5]; if(!*slot){*slot=photos_widget_create(200,90);if(*slot)compositor_add(*slot);} w=*slot; break;
        case L_MAIL: slot=&singletons[6]; if(!*slot){*slot=mail_widget_create(220,100);if(*slot)compositor_add(*slot);} w=*slot; break;
        case L_MEDIA: slot=&singletons[7]; if(!*slot){*slot=media_create(250,110);if(*slot)compositor_add(*slot);} w=*slot; break;
        case L_AILAB: slot=&singletons[8]; if(!*slot){*slot=ailab_create(200,60);if(*slot)compositor_add(*slot);} w=*slot; break;
        case L_SETTINGS: slot=&singletons[9]; if(!*slot){*slot=settings_app_create(220,120);if(*slot)compositor_add(*slot);} w=*slot; break;
        case L_ABOUT: slot=&singletons[10]; if(!*slot){*slot=about_create(fb_w/2-190,200);if(*slot)compositor_add(*slot);} w=*slot; break;
        case L_HELP: slot=&singletons[11]; if(!*slot){*slot=help_create(fb_w/2-200,180);if(*slot)compositor_add(*slot);} w=*slot; break;
    }
    if (w) { w->visible = 1; compositor_bring_to_front(w); }
}

static int inited = 0;
void app_grid_init(void) { inited = 1; pr_info("app_grid: %d apps ready\n", COLS*ROWS); }

static void cell_pos(int r, int c, int* x, int* y) {
    *x = START_X + c * (ISIZE + GAP_X);
    *y = START_Y + r * (ISIZE + LABEL_H + GAP_Y);
}

void app_grid_render(void) {
    if (!inited) app_grid_init();
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) {
        const struct Entry* e = &grid[r][c];
        int x, y; cell_pos(r, c, &x, &y);
        fb_fill_rect(x+2, y+2, ISIZE, ISIZE, 0x40000000);
        const uint8_t (*icon)[32] = app_icon_get(e->icon);
        if (icon) app_icon_draw(x, y, icon, ISIZE/32);
        int lw = fb_text_width(e->label);
        int lx = x + (ISIZE - lw) / 2, ly = y + ISIZE + 4;
        fb_fill_rect(lx-4, ly-1, lw+8, LABEL_H, 0xA00E1422);
        fb_draw_string(lx, ly, e->label, 0xFFFFFFFF);
    }
}

int app_grid_handle_click(int x, int y) {
    if (!inited) app_grid_init();
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) {
        int ix, iy; cell_pos(r, c, &ix, &iy);
        if (x >= ix && x < ix + ISIZE && y >= iy && y < iy + ISIZE + LABEL_H) {
            pr_info("app_grid: launch %s\n", grid[r][c].label);
            launch(grid[r][c].kind);
            return 1;
        }
    }
    return 0;
}
