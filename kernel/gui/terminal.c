/*
 * Lestra OS - Terminal widget (works with real shell)
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>
#include <lestra/printk.h>
#include <string.h>

#define W 720
#define H 420
#define TITLE_H 36
#define PAD 8
#define CW 8
#define CH 16
#define COLS ((W - 2*PAD) / CW)
#define ROWS ((H - TITLE_H - 2*PAD) / CH)
#define SCROLLBACK 256

struct TermState {
    char screen[ROWS][COLS+1];
    char sb[SCROLLBACK][COLS+1];
    int sb_count, sb_head, sb_off;
    int cx, cy;
    char input[256];
    int input_len;
    int active;
};

static struct TermState ts;
static struct widget tw;

extern void shell_execute_line(const char*, void(*)(char));
extern void syscall(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static struct TermState* out_target = NULL;

static void term_scroll(void);
static void snap(void);
static void draw_prompt(void);
static void exec_cmd(void);

static void out_char(char c) {
    if (c == '\n') { ts.cx = 0; if (++ts.cy >= ROWS) { term_scroll(); ts.cy = ROWS-1; } }
    else if (c == '\r') { ts.cx = 0; }
    else if (c == '\b') { if (ts.cx > 0) { ts.cx--; ts.screen[ts.cy][ts.cx] = ' '; } }
    else if (c == '\t') { for(int i=0;i<4;i++) out_char(' '); }
    else {
        if (ts.cx >= COLS) { ts.cx = 0; if (++ts.cy >= ROWS) { term_scroll(); ts.cy = ROWS-1; } }
        ts.screen[ts.cy][ts.cx++] = c;
    }
}

static void term_scroll(void) {
    memcpy(ts.sb[ts.sb_head], ts.screen[0], COLS);
    ts.sb[ts.sb_head][COLS] = 0;
    ts.sb_head = (ts.sb_head + 1) % SCROLLBACK;
    if (ts.sb_count < SCROLLBACK) ts.sb_count++;
    ts.sb_off = 0;
    for (int r = 0; r < ROWS-1; r++) {
        memcpy(ts.screen[r], ts.screen[r+1], COLS);
        ts.screen[r][COLS] = 0;
    }
    memset(ts.screen[ROWS-1], ' ', COLS);
    ts.screen[ROWS-1][COLS] = 0;
}

static void snap(void) { ts.sb_off = 0; }

static int sb_idx(int logical) {
    return (ts.sb_head - ts.sb_count + logical + SCROLLBACK) % SCROLLBACK;
}

void terminal_scroll(int lines) {
    int max = ts.sb_count;
    int n = ts.sb_off + lines;
    if (n < 0) n = 0;
    if (n > max) n = max;
    ts.sb_off = n;
}

static void clear_all(void) {
    for (int r = 0; r < ROWS; r++) { memset(ts.screen[r], ' ', COLS); ts.screen[r][COLS] = 0; }
    ts.sb_count = ts.sb_head = ts.sb_off = ts.cx = ts.cy = 0;
}

static void draw_prompt(void) { out_char('\n'); for(const char* p="lestra:/$ "; *p; p++) out_char(*p); }

static void exec_cmd(void) {
    ts.input[ts.input_len] = 0;
    out_char('\n');
    out_target = &ts;
    shell_execute_line(ts.input, out_char);
    out_target = NULL;
    ts.input_len = 0;
    draw_prompt();
}

static void draw(struct widget* w) {
    struct TermState* s = (struct TermState*)w->state;
    extern void ui_draw_card(int,int,int,int,int);
    ui_draw_card(w->x, w->y, w->w, w->h, w->focused);
    fb_draw_string(w->x + 70, w->y + 10, "Terminal - lsh", 0xFFFFFFFF);

    int bx = w->x + PAD, by = w->y + TITLE_H + PAD, bw = w->w - 2*PAD, bh = w->h - TITLE_H - 2*PAD;
    fb_fill_rect(bx, by, bw, bh, 0xFF000000);

    int off = s->sb_off;
    for (int r = 0; r < ROWS; r++) {
        int comb = s->sb_count - off + r;
        const char* line = NULL;
        if (comb < 0) line = NULL;
        else if (comb < s->sb_count) line = s->sb[sb_idx(comb)];
        else { int sr = comb - s->sb_count; if (sr >= 0 && sr < ROWS) line = s->screen[sr]; }
        if (line) for (int c = 0; c < COLS; c++) if (line[c] && line[c] != ' ')
            fb_draw_char(bx + c*CW, by + r*CH, line[c], 0xFFFFFFFF);
    }

    uint64_t now = timer_get_ms();
    if (off == 0 && (now/500)%2==0) {
        fb_fill_rect(bx + s->cx*CW, by + s->cy*CH, CW, CH, 0xFF22D3EE);
    }
}

static void on_event(struct widget* w, struct event* e) {
    struct TermState* s = (struct TermState*)w->state;
    if (e->type == EV_MOUSE_DOWN) { s->active = 1; return; }
    if (e->type == EV_MOUSE_SCROLL) { terminal_scroll(e->mouse.scroll); return; }
    if (e->type == EV_KEY_DOWN && s->active) {
        snap();
        if (keyboard_has_key()) {
            char c = keyboard_getchar();
            if (c == '\n') exec_cmd();
            else if (c == '\b') { if (s->input_len > 0) { s->input_len--; out_char('\b'); } }
            else if (c >= 0x20 && c < 0x7F && s->input_len < 255) { s->input[s->input_len++] = c; out_char(c); }
        }
    }
}

struct widget* terminal_create(int x, int y) {
    clear_all();
    ts.input_len = 0; ts.active = 1;
    for(const char* p="Lestra Shell (lsh) - by Lee Muriithi Kingori\nType 'help' for commands.\n\n"; *p; p++) out_char(*p);
    draw_prompt();

    tw.x = x; tw.y = y; tw.w = W; tw.h = H;
    tw.visible = 1; tw.focused = 1; tw.draggable = 1; tw.resizable = 0;
    tw.draw = draw; tw.on_event = on_event; tw.state = &ts;
    memcpy(tw.title, "Terminal", 9);
    return &tw;
}
