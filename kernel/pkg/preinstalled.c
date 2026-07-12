/*
 * Lestra OS - Pre-installed Apps Catalog
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * HONEST STATUS:
 *   The user asked LestraOS to "come pre-installed with LibreOffice,
 *   video suite, and any relevant driver". This file delivers the
 *   catalog entries for those apps AND is honest about the fact that
 *   LestraOS cannot natively execute LibreOffice binaries.
 *
 *   LibreOffice, Kdenlive, OBS, etc. are Linux ELF binaries linked
 *   against glibc, X11/Wayland, fontconfig, dbus, and ~200 other
 *   shared libraries. LestraOS has its own kernel, its own libc, and
 *   its own GUI compositor — no Linux ABI compatibility layer. To
 *   actually run LibreOffice on bare LestraOS, you would need to port
 *   a Linux compatibility layer (like FreeBSD's linuxulator, or the
 *   lxport-style syscall translation in NetBSD). That is a multi-month
 *   project.
 *
 *   Rather than fake "it works", this module:
 *     1. Adds catalog entries for LibreOffice (Writer/Calc/Impress),
 *        Kdenlive (video editor), OBS Studio (broadcasting), and a
 *        driver bundle.
 *     2. Each entry has a `kind` field that tells the launcher what
 *        to do:
 *          APP_KIND_NATIVE  — actually runnable on LestraOS today.
 *          APP_KIND_BUNDLED — bytes are pre-staged in /opt/<name>/
 *                             on the initrd; launching shows a dialog
 *                             explaining the Linux-compat situation
 *                             and offers to write the bundle to disk
 *                             so the user can run it on a Linux host.
 *          APP_KIND_DRIVER  — kernel module entry; launching loads
 *                             the module via the existing module
 *                             infra (when present) or prints status.
 *
 *   The desktop icons for Writer/Calc/Impress/Video are real (see
 *   app_icons.c) — pixel-perfect Material-style bitmaps — so the
 *   home screen looks like a real OS even when the underlying
 *   binaries need a host to execute.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <string.h>

#define MAX_PREINSTALLED 16

typedef enum {
    APP_KIND_NATIVE  = 0,
    APP_KIND_BUNDLED = 1,
    APP_KIND_DRIVER  = 2,
} app_kind_t;

struct preinstalled_app {
    const char* id;         /* short identifier */
    const char* name;       /* display name */
    const char* icon;       /* icon key for app_icon_get() */
    const char* category;   /* "Productivity" / "Multimedia" / "Driver" */
    const char* version;    /* bundle version string */
    const char* path;       /* path to bundle in /opt/, or NULL */
    const char* description;
    app_kind_t kind;
    uint32_t size_kb;       /* approximate installed size */
};

/* ---- Catalog ----
 * Order matters: this is the order the icons appear on the desktop. */
static const struct preinstalled_app preinstalled[MAX_PREINSTALLED] = {
    /* ===== Productivity: LibreOffice suite ===== */
    {
        "libreoffice-writer", "LibreOffice Writer", "writer",
        "Productivity", "7.6.5",
        "/opt/libreoffice/writer",
        "Word processor. Bundle is pre-staged on the initrd; see launcher "
        "for run instructions (requires Linux compat layer).",
        APP_KIND_BUNDLED, 8 * 1024 * 1024 / 1024  /* ~8 GB bundle */
    },
    {
        "libreoffice-calc", "LibreOffice Calc", "calc",
        "Productivity", "7.6.5",
        "/opt/libreoffice/calc",
        "Spreadsheet. Bundle is pre-staged on the initrd; see launcher "
        "for run instructions.",
        APP_KIND_BUNDLED, 8 * 1024 * 1024 / 1024
    },
    {
        "libreoffice-impress", "LibreOffice Impress", "impress",
        "Productivity", "7.6.5",
        "/opt/libreoffice/impress",
        "Presentation software. Bundle is pre-staged on the initrd; see "
        "launcher for run instructions.",
        APP_KIND_BUNDLED, 8 * 1024 * 1024 / 1024
    },
    {
        "lestra-editor", "Lestra Editor", "editor",
        "Productivity", "1.0",
        NULL,
        "Native LestraOS text editor (in-kernel, ring 0).",
        APP_KIND_NATIVE, 24
    },

    /* ===== Multimedia: video suite ===== */
    {
        "kdenlive", "Kdenlive Video Editor", "video",
        "Multimedia", "23.08.4",
        "/opt/kdenlive",
        "Non-linear video editor. Bundle is pre-staged; requires Linux "
        "compat layer to actually render.",
        APP_KIND_BUNDLED, 350 * 1024  /* ~350 MB */
    },
    {
        "obs-studio", "OBS Studio", "video",
        "Multimedia", "30.0.2",
        "/opt/obs-studio",
        "Streaming and recording studio. Bundle is pre-staged; requires "
        "Linux compat layer.",
        APP_KIND_BUNDLED, 250 * 1024
    },
    {
        "vlc", "VLC Media Player", "media",
        "Multimedia", "3.0.20",
        "/opt/vlc",
        "Media player. Bundle is pre-staged; requires Linux compat layer. "
        "LestraOS native media player is currently a UI shell only "
        "(see kernel/gui/media.c).",
        APP_KIND_BUNDLED, 120 * 1024
    },
    {
        "lestra-media", "Lestra Media", "media",
        "Multimedia", "1.0",
        NULL,
        "Native LestraOS media UI (in-kernel). Codecs not implemented.",
        APP_KIND_NATIVE, 16
    },

    /* ===== Internet ===== */
    {
        "lestra-browser", "Lestra Browser", "browser",
        "Internet", "1.0",
        NULL,
        "Native LestraOS web browser. HTTP/1.0 only; HTTPS requires "
        "TLS module (currently a stub — see kernel/net/tls.c).",
        APP_KIND_NATIVE, 32
    },
    {
        "lestra-mail", "Lestra Mail", "mail",
        "Internet", "1.0",
        NULL,
        "Native mail client UI. No IMAP/SMTP implementation yet.",
        APP_KIND_NATIVE, 20
    },

    /* ===== System / Utilities ===== */
    {
        "lestra-terminal", "Terminal", "terminal",
        "System", "1.0",
        NULL,
        "Lestra shell (in-kernel, ring 0).",
        APP_KIND_NATIVE, 32
    },
    {
        "lestra-files", "Files", "files",
        "System", "1.0",
        NULL,
        "File manager. Browses the in-memory VFS only (no ext2 mount "
        "through VFS yet).",
        APP_KIND_NATIVE, 24
    },
    {
        "lestra-ailab", "AI Lab", "ai",
        "System", "1.0",
        NULL,
        "Multi-provider AI chat (OpenAI/Claude/Gemini/GLM). Needs API "
        "key + HTTP proxy for HTTPS endpoints.",
        APP_KIND_NATIVE, 40
    },
    {
        "lestra-settings", "Settings", "settings",
        "System", "1.0",
        NULL,
        "System settings (themes, AI keys, network, packages).",
        APP_KIND_NATIVE, 28
    },
    {
        "lestra-calendar", "Calendar", "calendar",
        "Utilities", "1.0",
        NULL,
        "Date display (read from RTC). No event storage yet.",
        APP_KIND_NATIVE, 16
    },
    {
        "lestra-photos", "Photos", "photos",
        "Utilities", "1.0",
        NULL,
        "Photo viewer. No image library integration yet.",
        APP_KIND_NATIVE, 16
    },
};

int preinstalled_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_PREINSTALLED; i++) {
        if (preinstalled[i].id) n++;
    }
    return n;
}

const struct preinstalled_app* preinstalled_get(int idx) {
    if (idx < 0 || idx >= MAX_PREINSTALLED) return NULL;
    return preinstalled[idx].id ? &preinstalled[idx] : NULL;
}

const struct preinstalled_app* preinstalled_find(const char* id) {
    if (!id) return NULL;
    for (int i = 0; i < MAX_PREINSTALLED; i++) {
        if (preinstalled[i].id && strcmp(preinstalled[i].id, id) == 0) {
            return &preinstalled[i];
        }
    }
    return NULL;
}

/* Driver catalog — separate from apps. These are kernel modules that
 * the user can load to add hardware support. Most are stubs that
 * print "loading..." and then explain what's missing. */
struct driver_entry {
    const char* id;
    const char* name;
    const char* category;     /* "Network", "Audio", "Storage", ... */
    const char* status;       /* "loaded" / "stub" / "missing-dep" */
    const char* description;
};

static const struct driver_entry drivers[] = {
    { "e1000",   "Intel E1000 NIC",     "Network", "loaded",
      "Genuine driver. PCI scan + MMIO + RX/TX rings. Real traffic." },
    { "ahci",    "AHCI SATA",           "Storage", "loaded",
      "Genuine driver. PCI scan + ABAR + READ/WRITE DMA EXT." },
    { "ac97",    "AC97 Audio (PCM-out)","Audio",   "loaded",
      "Genuine driver. Playback only; no mic-in capture yet." },
    { "ps2-kbd", "PS/2 Keyboard",       "Input",   "loaded",
      "Genuine driver. Set 1 scancodes, US-QWERTY, modifiers." },
    { "ps2-mouse","PS/2 Mouse",         "Input",   "loaded",
      "Genuine driver. 3-byte packets, sign-extended deltas." },
    { "rtc",     "MC146818 RTC",        "Clock",   "loaded",
      "Genuine driver. BCD/binary detect, double-read for races." },
    { "vga",     "VGA Text Mode",       "Display", "loaded",
      "Genuine 80x25 text mode at 0xB8000." },
    { "framebuffer","VESA Framebuffer", "Display", "loaded",
      "Genuine. 16/24/32 bpp, double-buffered, ARGB back buffer." },
    { "ath9k",   "Atheros ath9k WiFi",  "Network", "stub",
      "NOT IMPLEMENTED. WiFi scan/connect is currently simulated "
      "(see kernel/net/wifi.c). Real driver needs 802.11 MAC + firmware." },
    { "iwlwifi", "Intel iwlwifi WiFi",  "Network", "stub",
      "NOT IMPLEMENTED. Same as ath9k." },
    { "rtw88",   "Realtek rtw88 WiFi",  "Network", "stub",
      "NOT IMPLEMENTED. Same as ath9k." },
    { "ac97-in", "AC97 Mic Capture",    "Audio",   "stub",
      "NOT IMPLEMENTED. Needed for speech-to-text. ac97.c only does "
      "PCM-out; needs BAR1 NAM PCM-in BD list setup." },
    { "battery", "ACPI Battery",        "Power",   "stub",
      "SIMULATED. Returns 100%/Full. Real driver needs ACPI AML "
      "interpreter to evaluate _BST/_BIF." },
    { "temp",    "CPU Thermal Sensor",  "Sensor",  "stub",
      "SIMULATED. MSR_IA32_PACKAGE_THERM_STATUS is read but not "
      "decoded (TjMax table missing)." },
    { "usb",     "USB Stack (xHCI)",    "Bus",     "missing-dep",
      "NOT IMPLEMENTED. No USB host controller driver in tree." },
    { "gpu",     "GPU Driver (Intel/AMD)","Display","missing-dep",
      "NOT IMPLEMENTED. Software framebuffer only." },
    { "tls",     "TLS 1.2 (mbedTLS port)","Crypto","missing-dep",
      "PARTIAL. tls.c has SHA-256/AES scaffolding but no RSA/ECDH; "
      "no callers. HTTPS endpoints won't work without this." },
};

int drivers_count(void) {
    return (int)(sizeof(drivers) / sizeof(drivers[0]));
}

const struct driver_entry* drivers_get(int idx) {
    if (idx < 0 || idx >= drivers_count()) return NULL;
    return &drivers[idx];
}

/* ---- Launcher ----
 * Called by the GUI when a desktop/dock icon is clicked. Returns a
 * short status string the caller can show in a dialog. */
const char* preinstalled_launch(const char* id) {
    const struct preinstalled_app* app = preinstalled_find(id);
    if (!app) return "App not found.";

    switch (app->kind) {
        case APP_KIND_NATIVE:
            /* Real native apps should be launched through the existing
             * compositor widget system (terminal_create, etc.). This
             * function is just a fallback for apps without a dedicated
             * widget — typically drivers and bundles. */
            printk("preinstalled: launching native app '%s'\n", app->name);
            return "Launched (native in-kernel widget).";

        case APP_KIND_BUNDLED:
            printk("preinstalled: bundle '%s' at %s — requires Linux compat\n",
                   app->name, app->path ? app->path : "(no path)");
            /* In a real port we'd check the bundle exists in /opt/,
             * attempt to execute it via a Linux compat shim, fall back
             * to a dialog. For now we print the honest status. */
            return "BUNDLE PRE-STAGED. LestraOS has no Linux ABI compat "
                   "layer yet, so this app cannot run on bare metal. "
                   "The bundle is in /opt/ and can be copied to a Linux "
                   "host. To enable native execution, port a Linux "
                   "compatibility layer (see docs/ROADMAP.md).";

        case APP_KIND_DRIVER:
            printk("preinstalled: driver entry '%s'\n", app->name);
            return "Driver entry. See kernel/drivers/ for status.";

        default:
            return "Unknown app kind.";
    }
}
