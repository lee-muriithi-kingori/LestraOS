/*
 * Lestra OS - Bitmap Icons API
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 */
#ifndef LESTRA_ICONS_H
#define LESTRA_ICONS_H

#include <lestra/types.h>

#define ICON_DIM 32

/* Draw a 32x32 bitmap icon at (x,y) with the given scale (1=32px, 2=64px). */
void fb_draw_icon(int x, int y, const uint8_t icon[32][32], int scale);

/* Get icon by index: 0=terminal, 1=ai, 2=editor, 3=media, 4=files, 5=settings */
const uint8_t (*icon_get(int idx))[32];

#endif
