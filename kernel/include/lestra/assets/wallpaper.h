/*
 * Lestra OS - Desktop Wallpaper (1024x768 RGB)
 * Copyright (c) 2026 lestramk.org
 *
 * Procedurally generated gradient wallpaper so we don't need to ship
 * a 2.4 MB binary blob. The kernel's ui_render_wallpaper() blits this
 * directly to the framebuffer.
 */
#ifndef LESTRA_ASSETS_WALLPAPER_H
#define LESTRA_ASSETS_WALLPAPER_H

#include <lestra/types.h>

/* 1024x768 RGB888 = 2,359,296 bytes. Stored as a flat array.
 * Generated at runtime by ui_render_wallpaper() if this array is empty,
 * but here we provide a 1x1 placeholder that the renderer detects. */
static const uint8_t wallpaper_data[3] = {
    0x0A, 0x0C, 0x12   /* single pixel: deep navy */
};

/* Wallpaper dimensions */
#define WALLPAPER_W 1
#define WALLPAPER_H 1

#endif /* LESTRA_ASSETS_WALLPAPER_H */
