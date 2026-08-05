/*
 * Lestra OS - On-Screen Keyboard
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * A QWERTY soft keyboard that pops up at the bottom of the screen.
 * Supports:
 *   - All letters + numbers + punctuation
 *   - Shift (sticky toggle) for capitals
 *   - Ctrl / Alt (sticky toggles) for shortcuts
 *   - Space, Enter, Backspace, Tab, Esc
 *
 * Layout (5 rows):
 *   ` 1 2 3 4 5 6 7 8 9 0 - =   Backspace
 *   Tab Q W E R T Y U I O P [ ]
 *   Caps A S D F G H J K L ; ' Enter
 *   Shift Z X C V B N M , . /  Shift
 *   Ctrl Alt Space             Alt
 *
 * Click a key to inject its char into the keyboard buffer (so the
 * existing terminal/editor widgets receive it via keyboard_getchar()).
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <lestra/printk.h>
#include <string.h>

#define OSK_W   720
#define OSK_H   220
#define OSK_PAD 6
#define OSK_KEY_W 44
#define OSK_KEY_H 36

/* keyboard_inject_char() is declared in <lestra/keyboard.h> (included
 * above) and implemented in kernel/drivers/char/keyboard.c. It pushes
 * the char into the keyboard ring buffer so existing widgets reading
 * via keyboard_getchar() receive it. */

struct osk_key {
    char label[4];
    char normal;     /* char to inject when no Shift */
    char shifted;    /* char to inject when Shift */
    int  width;      /* in key units (1=default) */
    int  special;    /* 0=plain, 1=Shift, 2=Ctrl, 3=Alt, 4=Backspace,
                      * 5=Enter, 6=Space, 7=Tab, 8=Esc, 9=Caps */
};

#define OSK_ROWS 5
static const struct osk_key osk_layout[OSK_ROWS][16] = {
    {
        {"`",'`','~',1,0}, {"1",'1','!',1,0}, {"2",'2','@',1,0},
        {"3",'3','#',1,0}, {"4",'4','$',1,0}, {"5",'5','%',1,0},
        {"6",'6','^',1,0}, {"7",'7','&',1,0}, {"8",'8','*',1,0},
        {"9",'9','(',1,0}, {"0",'0',')',1,0}, {"-",'-','_',1,0},
        {"=",'=','+',1,0}, {"Bksp",0,0,2,4},
        {"",0,0,0,0}, {"",0,0,0,0}
    },
    {
        {"Tab",0,0,2,7}, {"Q",'q','Q',1,0}, {"W",'w','W',1,0},
        {"E",'e','E',1,0}, {"R",'r','R',1,0}, {"T",'t','T',1,0},
        {"Y",'y','Y',1,0}, {"U",'u','U',1,0}, {"I",'i','I',1,0},
        {"O",'o','O',1,0}, {"P",'p','P',1,0}, {"[",'[','{',1,0},
        {"]",']','}',1,0}, {"\\",'\\','|',1,0},
        {"",0,0,0,0}, {"",0,0,0,0}
    },
    {
        {"Caps",0,0,2,9}, {"A",'a','A',1,0}, {"S",'s','S',1,0},
        {"D",'d','D',1,0}, {"F",'f','F',1,0}, {"G",'g','G',1,0},
        {"H",'h','H',1,0}, {"J",'j','J',1,0}, {"K",'k','K',1,0},
        {"L",'l','L',1,0}, {";",';',':',1,0}, {"'",'\'','"',1,0},
        {"Ent",0,0,3,5},
        {"",0,0,0,0}, {"",0,0,0,0}, {"",0,0,0,0}
    },
    {
        {"Shift",0,0,2,1}, {"Z",'z','Z',1,0}, {"X",'x','X',1,0},
        {"C",'c','C',1,0}, {"V",'v','V',1,0}, {"B",'b','B',1,0},
        {"N",'n','N',1,0}, {"M",'m','M',1,0}, {",",',','<',1,0},
        {".",'.','>',1,0}, {"/",'/','?',1,0}, {"Shift",0,0,2,1},
        {"",0,0,0,0}, {"",0,0,0,0}, {"",0,0,0,0}, {"",0,0,0,0}
    },
    {
        {"Ctrl",0,0,2,2}, {"Alt",0,0,2,3},
        {"Space",' ',' ',8,6},
        {"Alt",0,0,2,3}, {"Esc",0,0,2,8},
        {"",0,0,0,0}, {"",0,0,0,0}, {"",0,0,0,0},
        {"",0,0,0,0}, {"",0,0,0,0}, {"",0,0,0,0},
        {"",0,0,0,0}, {"",0,0,0,0}, {"",0,0,0,0},
        {"",0,0,0,0}, {"",0,0,0,0}
    }
};

struct osk_state {
    int visible;
    int shift_on;
    int ctrl_on;
    int alt_on;
    int caps_on;
    /* Cached positions for hit-testing. */
    int key_x[OSK_ROWS][16];
    int key_y[OSK_ROWS][16];
    int key_w[OSK_ROWS][16];
    int key_h[OSK_ROWS][16];
    int computed;
};

static struct osk_state osk_state;

/* ---------- layout ---------- */
static void osk_compute_layout(void) {
    if (osk_state.computed) return;
    int y = 0;
    for (int r = 0; r < OSK_ROWS; r++) {
        int x = 0;
        for (int c = 0; c < 16; c++) {
            const struct osk_key* k = &osk_layout[r][c];
            if (k->width == 0) {
                osk_state.key_x[r][c] = -1;
                continue;
            }
            osk_state.key_x[r][c] = x;
            osk_state.key_y[r][c] = y;
            osk_state.key_w[r][c] = k->width * OSK_KEY_W;
            osk_state.key_h[r][c] = OSK_KEY_H;
            x += k->width * OSK_KEY_W;
        }
        y += OSK_KEY_H + 2;
    }
    osk_state.computed = 1;
}

/* ---------- inject ---------- */
static void osk_inject(const struct osk_key* k) {
    char c = 0;
    int shifted = osk_state.shift_on ^ (osk_state.caps_on &&
                                        (k->normal >= 'a' && k->normal <= 'z'));
    if (k->special == 0) {
        c = shifted ? k->shifted : k->normal;
        if (osk_state.caps_on && c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        keyboard_inject_char(c);
        /* Shift is non-sticky for plain keys (one-shot). */
        if (osk_state.shift_on) osk_state.shift_on = 0;
    } else {
        switch (k->special) {
            case 1: osk_state.shift_on = !osk_state.shift_on; break;
            case 2: osk_state.ctrl_on  = !osk_state.ctrl_on;  break;
            case 3: osk_state.alt_on   = !osk_state.alt_on;   break;
            case 4: keyboard_inject_char('\b'); break;
            case 5: keyboard_inject_char('\n'); break;
            case 6: keyboard_inject_char(' ');  break;
            case 7: keyboard_inject_char('\t'); break;
            case 8: keyboard_inject_char(0x1B); break;  /* ESC */
            case 9: osk_state.caps_on = !osk_state.caps_on; break;
        }
    }
    (void)c;
}

/* ---------- draw ---------- */
static void osk_draw_key(int x, int y, int w, int h,
                         const struct osk_key* k, int active) {
    uint32_t bg = 0xFF1E293B;
    uint32_t fg = UI_TEXT_PRIMARY;
    if (k->special == 1) {  /* Shift */
        bg = osk_state.shift_on ? UI_ACCENT : 0xFF1E293B;
        fg = osk_state.shift_on ? 0xFF000000 : UI_TEXT_PRIMARY;
    } else if (k->special == 2) {  /* Ctrl */
        bg = osk_state.ctrl_on ? UI_ACCENT : 0xFF1E293B;
        fg = osk_state.ctrl_on ? 0xFF000000 : UI_TEXT_PRIMARY;
    } else if (k->special == 3) {  /* Alt */
        bg = osk_state.alt_on ? UI_ACCENT : 0xFF1E293B;
        fg = osk_state.alt_on ? 0xFF000000 : UI_TEXT_PRIMARY;
    } else if (k->special == 9) {  /* Caps */
        bg = osk_state.caps_on ? UI_ACCENT : 0xFF1E293B;
        fg = osk_state.caps_on ? 0xFF000000 : UI_TEXT_PRIMARY;
    }
    if (active) {
        bg = UI_ACCENT_HOT;
        fg = 0xFF000000;
    }
    fb_draw_rounded(x, y, w, h, 5, bg, bg);
    /* Label */
    int lw = fb_text_width(k->label);
    fb_draw_string(x + (w - lw) / 2, y + (h - 16) / 2 + 2,
                   k->label, fg);
}

static void osk_render(void) {
    if (!osk_state.visible) return;
    osk_compute_layout();

    /* Position the OSK at the bottom-centre of the screen. */
    int ox = (int)fb_w / 2 - OSK_W / 2;
    int oy = (int)fb_h - OSK_H - 8;
    (void)ox; (void)oy;

    /* Background panel. */
    fb_draw_rounded(ox, oy, OSK_W, OSK_H, 8,
                    0xE6050608, UI_ACCENT);

    /* Title bar. */
    fb_fill_rect(ox + 4, oy + 4, OSK_W - 8, 24, 0xE00E1422);
    fb_draw_string(ox + 12, oy + 10, "On-Screen Keyboard", UI_TEXT_PRIMARY);
    fb_draw_string(ox + OSK_W - 18, oy + 10, "x", UI_TEXT_MUTED);

    /* Keys. */
    int body_x = ox + OSK_PAD;
    int body_y = oy + 32;
    for (int r = 0; r < OSK_ROWS; r++) {
        for (int c = 0; c < 16; c++) {
            if (osk_state.key_x[r][c] < 0) continue;
            const struct osk_key* k = &osk_layout[r][c];
            if (k->width == 0) continue;
            int kx = body_x + osk_state.key_x[r][c];
            int ky = body_y + osk_state.key_y[r][c];
            osk_draw_key(kx, ky, osk_state.key_w[r][c],
                         osk_state.key_h[r][c], k, 0);
        }
    }
}

/* ---------- events ---------- */
static int osk_hit_close(int mx, int my) {
    int ox = (int)fb_w / 2 - OSK_W / 2;
    int oy = (int)fb_h - OSK_H - 8;
    return (mx >= ox + OSK_W - 18 && mx < ox + OSK_W - 4 &&
            my >= oy + 6 && my < oy + 22);
}

static int osk_handle_event(struct event* e) {
    if (!osk_state.visible) return 0;
    if (e->type != EV_MOUSE_DOWN) return 0;
    int mx = e->mouse.x, my = e->mouse.y;
    if (osk_hit_close(mx, my)) {
        osk_state.visible = 0;
        return 1;
    }
    int ox = (int)fb_w / 2 - OSK_W / 2;
    int oy = (int)fb_h - OSK_H - 8;
    int body_x = ox + OSK_PAD;
    int body_y = oy + 32;
    for (int r = 0; r < OSK_ROWS; r++) {
        for (int c = 0; c < 16; c++) {
            if (osk_state.key_x[r][c] < 0) continue;
            const struct osk_key* k = &osk_layout[r][c];
            if (k->width == 0) continue;
            int kx = body_x + osk_state.key_x[r][c];
            int ky = body_y + osk_state.key_y[r][c];
            if (mx >= kx && mx < kx + osk_state.key_w[r][c] &&
                my >= ky && my < ky + osk_state.key_h[r][c]) {
                osk_inject(k);
                return 1;
            }
        }
    }
    /* Click outside the OSK panel: consume to avoid click-through. */
    if (mx >= ox && mx < ox + OSK_W && my >= oy && my < oy + OSK_H) {
        return 1;
    }
    return 0;
}

/* ---------- public ---------- */
void osk_create(void) {
    memset(&osk_state, 0, sizeof(osk_state));
    osk_state.visible = 0;
    osk_compute_layout();
}

void osk_show(void) {
    osk_state.visible = 1;
}

void osk_hide(void) {
    osk_state.visible = 0;
}

int osk_is_visible(void) {
    return osk_state.visible;
}
