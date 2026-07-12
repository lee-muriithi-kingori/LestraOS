/*
 * Lestra OS - Boot Splash API
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 */
#ifndef LESTRA_SPLASH_H
#define LESTRA_SPLASH_H

/* Set a status line for the splash (index 0-15). Called by kernel_main
 * as each subsystem initializes. */
void splash_set_status(int idx, const char* line);

/* Run the 3-frame splash animation. No-op if framebuffer unavailable. */
void splash_run(void);

#endif
