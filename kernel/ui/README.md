# Lestra OS — splash screen & main-loop UI

This module renders the LestraOS screen when the kernel boots. It contains:

* `ui_boot_splash()` — prints the splash screen to VGA text mode.
* `ui_menu_loop()`  — keyboard-driven top-level menu (System Tools, Install, About).
* `ui_panel()`      — draws a boxed panel with a title and a body of text.
* `ui_clear()`      — clears the screen and resets the cursor.

The rest of the kernel calls `ui_boot_splash()` from `kernel_main` so the
bootable kernel prints a recognizable banner even before the shell
prompt appears.

Beyond text VGA mode, this module is the only thing in `kernel/ui/`. A
future port can add `ui_framebuffer_*` functions without touching other
code.
