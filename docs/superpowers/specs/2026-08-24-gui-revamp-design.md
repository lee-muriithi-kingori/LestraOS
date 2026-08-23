# LestraOS GUI Revamp — "Lestra Aurora" Design Spec

**Date:** 2026-08-24 · **KE:** 38 · **Status:** Approved design
**Scope:** Full desktop shell rebuild — design system, window manager, shell layers, top bar, dock, notifications, overlay suite; apps inherit chrome.

---

## 1. Goals / Non-Goals

**Goals**
1. One coherent visual language ("Lestra Aurora": refined cyan-dark, glassy panels, layered depth) applied everywhere through shared code.
2. OS-drawn window chrome so all apps look identical and stay consistent by default.
3. A complete desktop shell: treated wallpaper, desktop grid, dock, floating top bar with live status island, notification toasts, lock/power/context/screenshot overlays.
4. Zero compiled-but-unreferenced GUI files after this work (fold or delete every orphan).
5. Keep the proven immediate-mode full-repaint loop (60 Hz @ 1024×768 via double buffer + `fb_swap`). No retained-mode toolkit.

**Non-Goals**
- No new framebuffer resolution or hardware acceleration.
- No per-app interior redesigns beyond removing self-drawn chrome (terminal prompt/colors etc. unchanged).
- No multi-user, no compositor effects requiring per-pixel blur of live windows.

## 2. Current State (verified)

Live today: `wallpaper_render` (raw nearest-neighbor photo, no treatment) → particles → static 12-icon `app_grid` → widgets (apps self-draw cards/titles inconsistently) → crosshair cursor. Event loop handles mouse down/up/move, key to focused widget, title-bar drag only.

Orphaned (compiled, never called): top_bar, dock, dynamic_island, notifications, animations, clock_widget, left_drawer, context_menu, task_manager, cpu_monitor, net_monitor, weather, terminal_tabs, editor_pro, app_store, app_widgets, osk, screenshot, clipboard, shortcuts, drawer, icons.c (vs app_icons.c).

Existing assets kept: `theme.c` (dark/light tokens + 6 accents persisted to `/etc/theme`), `fb.h` primitives (`fb_fill_rect_alpha`, `fb_blend`, `fb_draw_rounded`, scaled text), `app_icons.c` glyph set, `input.h` event model incl. `EV_MOUSE_SCROLL`, keyboard modifiers.

## 3. Architecture

Three new cores + slimmed compositor. Layer ownership is strict; nothing draws outside its layer.

```
kernel/gui/aurora.c   design system (tokens→primitives→motion)
kernel/gui/wm.c       window manager (chrome, hit-testing, drag/resize/minimize)
kernel/gui/shell.c    layer stack owner + frame scheduler
kernel/gui/compositor.c  event pump only (delegates paint order to shell.c)
```

### 3.1 aurora.c — Design System

Consumes theme colors; owns geometry, elevation, typography, motion, primitives.

```c
/* geometry */
#define AR_R_SM 6
#define AR_R_MD 12
#define AR_R_LG 16
#define AR_R_PILL 999
/* spacing: 4-pt grid — use literal multiples, no macro zoo */
/* control heights */
#define AR_H_TITLEBAR 36
#define AR_H_PILL 24
#define AR_H_BUTTON 28

void aurora_shadow(int x, int y, int w, int h, int focused);
void aurora_panel(int x, int y, int w, int h, int radius);        /* glass card */
void aurora_pill(int x, int y, int w, const char* label, uint32_t fg, uint32_t bg);
int  aurora_button(int x, int y, int w, int h, const char* label, int hot); /* returns pressed */
void aurora_traffic_lights(int x, int y, int focused, int hover_btn);
void aurora_progress(int x, int y, int w, int frac /*0..255*/, uint32_t color);
uint32_t aurora_scrim(uint32_t c, uint8_t alpha);                 /* blend toward black */
/* motion */
int  ar_ease_out_cubic(int t, int total);                          /* 0..total → 0..256 */
```

Visual rules:
- **Glass panel:** `theme_card_bg()` fill (alpha carried in token, e.g. 0xC7), 1 px border `theme_card_border()`, inner 1 px top highlight white@0x18 inset radius.
- **Shadow:** 3 expanding rects below-right, black @ 0x38/0x20/0x10, offsets 2/4/6 px; focused windows get an extra accent glow ring (accent @ 0x30, offset −2 expand 2).
- **Traffic lights:** ⌀12 circles, gap 8: close `theme_danger()`, minimize 0xFFFBBF24, maximize `theme_success()`; glyphs (×, −, +) drawn only while cursor hovers the title bar; unfocused windows render all three desaturated gray.
- **Typography:** Display = `fb_draw_char_scale(...,2)`; Body = plain; Caption = `fb_draw_string_small` + `TEXT_MUTED`. All text colors from theme tokens — no raw whites outside aurora.

### 3.2 wm.c — Window Manager

```c
enum { WM_CHROME_NONE = 0, WM_CHROME_WINDOW = 1 };
enum { WM_HIT_MISS=0, WM_HIT_CONTENT, WM_HIT_TITLEBAR, WM_HIT_CLOSE,
       WM_HIT_MIN, WM_HIT_MAX, WM_HIT_RESIZE };

struct widget {            /* extended, existing fields untouched */
    /* ...existing... */
    int chrome;            /* default 0 = NONE; window apps set 1 */
    int minimized;
};
```

- `wm_decorate(w)` — called by shell before `w->draw`: shadow → panel → 36 px title bar (glyph+title left, lights right) → content sunken rect behind `w->draw`. Apps draw ONLY into `wm_content_rect(w)` = `(x, y+36, w−0, h−36)`.
- `wm_hit(w, mx, my)` — classifies: lights zone (right 72 px of title bar), titlebar strip, resize grip = bottom-right 10×10, else content.
- Drag: existing compositor drag logic moves into wm via TITLEBAR hits; clamped to screen.
- Resize: grip drag adjusts `w,h`; clamped to `min_w/min_h` (per-app values set at create; default 320×200).
- Maximize: toggle stores pre-max rect in widget state, sets rect to full work area (below top bar, above dock).
- Minimize: `visible=0, minimized=1`; dock dot stays lit; dock click restores + focuses.
- Focus: click-to-focus brings to front; focused = glow ring + bright lights.

### 3.3 shell.c — Layer Stack

```c
void shell_init(void);     /* wallpaper, grid, dock, topbar, overlays */
void shell_frame(void);    /* one full paint: layers 1..7 below */
void shell_dispatch(struct event* e);  /* routed from compositor */
```

Paint order per frame:
1. **Wallpaper:** Yugi photo loaded once, **pre-scrimmed at load time** (per-pixel ×(1−α) toward black, α≈0.33) + vignette strips baked into the same pass; fallback = vertical aurora gradient (BG_GRAD_TOP→ACCENT-tinted void). Particles die (moved to deleted animations.c ideas; not part of Aurora).
2. **Desktop grid:** restyled tiles — glass rounded square (AR_R_MD), icon glyph centered, label Caption below tile; hover (tracked from EV_MOUSE_MOVE position) lifts tile 4 px with accent glow; click launches/focuses as today.
3. **Windows:** z-ordered `wm_decorate` + draw for visible, non-minimized widgets.
4. **Dock:** bottom-center glass pill (AR_R_PILL ends), flat row of 12 icons at 44 px with 8 px gaps (~620 px wide, fits 1024); running dot = 4 px accent circle under icon; hover lift + glow; click = launch/focus/minimize-toggle. Dock height 56 + 8 margin.
5. **Top bar:** floating glass bar, x=12,y=8,w=fb_w−24,h=40,r=AR_R_MD.
   - Left: `Lestra` wordmark (Display, accent) + focused app name (Body muted).
   - Center: **island** pill — compact: `CPU 12%  ↓ 42 KB/s`; hover expands width to show battery %, temp, uptime; click toggles Task Manager window.
   - Right: volume, wifi, battery pills + clock `HH:MM` (updates from `timer_get_ms()`).
6. **Overlays (z asc):** notification toasts (top-right stack under bar, slide-in via ease-out-cubic, auto-dismiss 4000 ms, max 3) → context menu (right-click desktop: New Terminal, Change Theme, Lock, Power…) → screenshot dim → power menu → lock screen (full-screen, clock huge, any key/click dismisses).
7. **Cursor:** replaced crosshair with proper arrow pointer sprite (blit, 12×18) + resize-arrow over grips + hand over buttons.

Event routing (`shell_dispatch`): overlays consume first (topmost active wins), then wm hits on windows (lights/titlebar/grip/content), then dock/topbar/grid hit zones, then desktop. Keys: global shortcuts table first (Alt+Tab cycle, Alt+F4 close, Ctrl+Alt+T terminal, Ctrl+Alt+L lock, Ctrl+Alt+P power, Esc closes menus), else focused widget.

### 3.4 Fold-in rewrites

| File | Fate |
|---|---|
| notifications.c | rewrite → toast API `notify_push(title, body)`, wired: app launch, theme change, DHCP acquire |
| task_manager.c | rewrite → absorbs cpu_monitor/net_monitor polling; island opens it |
| dynamic_island.c | folded into top_bar.c as the center pill component; file deleted |
| lock_screen.c, power_menu.c, context_menu.c, screenshot.c, clipboard.c, shortcuts.c | restyle w/ aurora_panel + wire through shell_dispatch |
| wallpaper.c | keep loader; add load-time scrim+vignette bake & gradient fallback |

### 3.5 Deletions (verify zero references, then `git rm`)

dock.c(old), animations.c, clock_widget.c, left_drawer.c, drawer.c, osk.c,
editor_pro.c, terminal_tabs.c, app_store.c, app_widgets.c, weather.c,
sys_info_widget.c, desktop_icons.c, icons.c, cpu_monitor.c, net_monitor.c,
dynamic_island.c.

### 3.6 App migration (12 windows)

Each of terminal, editor, files, browser, calendar, photos, mail, media,
ai_lab, settings_app, dialogs(about/help):
1. Set `chrome=WM_CHROME_WINDOW`, sensible `min_w/min_h` in create().
2. Delete its card/title/close-button drawing; offset content by title bar via `wm_content_rect()`.
3. Keep ALL input/logic handlers unchanged.

## 4. Data Flow

Input ISR → `input_poll` queue → compositor loop → `shell_dispatch` (overlays→wm→shell surfaces) → widget `on_event`. Paint: shell_frame reads theme tokens via aurora primitives → fb_back → `fb_swap`. Time: `timer_get_ms()` drives clock/island/animations. Kernel stats for island/task manager: direct extern reads of sched/net counters (same sources cpu_monitor used).

## 5. Error Handling

- Missing `/wallpaper.rgb` → gradient fallback (existing behavior, restyled).
- kmalloc failures at shell_init → skip component, log, continue boot (GUI must never panic the kernel).
- Dock/topbar hit arrays sized fixed (12/…); overflow guarded.
- All new files compile under `-Wall -Wextra -ffreestanding -mno-red-zone -mcmodel=large`; no libc beyond existing `string.h` kernel subset; no 128-bit math (no libgcc div helpers).

## 6. Testing Plan (post-WSL-reboot)

1. `make clean && make all` — zero new warnings.
2. QEMU boot (qemu64,+smep,+smap): serial log shows `shell:` init lines, HPET line (KE-37 regression check), zero page faults.
3. Visual smoke via screenshot dump path: desktop renders wallpaper+dock+topbar; launch each of the 12 apps; verify chrome uniformity, focus ring, minimize→dock restore, resize clamp, maximize toggle.
4. Overlay sweep: right-click menu items, toast appears on launch, lock/power screens, Win+Shift+S capture writes /tmp/screenshot.rgb.
5. 5-minute stability run at 60 fps (no leak: toasts freed, no per-frame allocs).

## 7. Risks / Mitigations

- **Big-bang breakage:** implement in plan-order (aurora→wm→shell→bar/dock→folds→app migration→deletions last), building after each stage.
- **Perf regressions:** shadows/glows are O(w+h) ring fills, not per-pixel blurs; measured against 16 ms budget; drop glow if frame >14 ms.
- **Reference breakage on deletes:** grep-verified per file before removal; Makefile wildcards make deletion automatic.
