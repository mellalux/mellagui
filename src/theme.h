#ifndef MELLAGUI_THEME_H
#define MELLAGUI_THEME_H

/*
 * The colour theme: one palette of named colours shared by the compositor (panel, frames, gadgets, wallpaper) and the
 * bundled programs (calculator, editor, files, settings ...). The defaults are the built-in look; mellagui.conf can change
 * any of them:
 *
 *   [theme]
 *   accent = #6fa8ff          # #rrggbb, rrggbb, 0xrrggbb or #rgb; every name below, unnamed ones keep the default
 *   frame  = 3b4a6b
 *
 * The compositor reads it at start; every client reads the same file at its own start (the compositor hands the path on in
 * the environment variable MELLAGUI_CONFIG, "none" = defaults), so a changed colour shows in programs started afterwards.
 * Values are 0xRRGGBB; `lm_theme` is a plain global so a client can write `#define COL_BG (lm_theme.background)`.
 */

#include <stdbool.h>
#include <stdint.h>

/* X(config name, struct field, default 0xRRGGBB) */
#define LM_THEME_COLORS(X) \
    X("wallpaper-top",        wallpaper_top,     0x1b2a49) /* desktop gradient, upper colour */ \
    X("wallpaper-bottom",     wallpaper_bottom,  0x3a6b72) /* ... lower colour */ \
    X("frame",                frame,             0x3b4a6b) /* window frame (title bar) of the focused window */ \
    X("frame-idle",           frame_idle,        0x2a2f3a) /* ... of the others */ \
    X("content",              content,           0x101216) /* window area before the client has drawn */ \
    X("title-text",           title_text,        0xffffff) \
    X("title-text-idle",      title_text_idle,   0x9aa0ae) \
    X("title-button-hover",   title_button_hover,   0x5a6c96) \
    X("title-button-pressed", title_button_pressed, 0x2c3856) \
    X("title-button-close",   title_button_close,   0xc0392b) \
    X("background",           background,        0x14171f) /* panel, menu and the window background of the bundled programs */ \
    X("field",                field,             0x0d0f14) /* input fields, displays, side bars */ \
    X("card",                 card,              0x161a24) /* gadget / notification cards */ \
    X("border",               border,            0x3a4256) \
    X("button",               button,            0x2d3a5a) \
    X("button-hover",         button_hover,      0x41527e) \
    X("button-down",          button_down,       0x56699c) \
    X("button-alt",           button_alt,        0x3a4a72) /* operator keys of the calculator */ \
    X("bar",                  bar,               0x2b303b) /* tool bars of the editor, files, terminal */ \
    X("accent",               accent,            0x6fa8ff) \
    X("text",                 text,              0xe6e9ef) \
    X("text-soft",            text_soft,         0xd8dee9) /* body text of the editor / files / terminal */ \
    X("text-dim",             text_dim,          0x7b8294) \
    X("muted",                muted,             0xa0a6b4) \
    X("danger",               danger,            0xc0392b) /* the power button, the close button */ \
    X("alert",                alert,             0xe05a5a) /* muted / critical in the gadgets */ \
    X("warn",                 warn,              0xe0a040) \
    X("ok",                   ok,                0x5ac88c) \
    X("caution",              caution,           0xe0a458) /* "modified" marks in the editor / files */ \
    X("error",                error,             0xff7070)

struct lm_theme {
#define X(name, field, def) uint32_t field;
    LM_THEME_COLORS(X)
#undef X
};

extern struct lm_theme lm_theme;      /* starts as the built-in defaults */

/* Back to the built-in colours (a reload reads the file again on top of them). */
void lm_theme_reset(void);

/* Set one colour from its config name and text. False when the name or the text is unknown / malformed. */
bool lm_theme_set(const char *name, const char *value);

/* Config-file callback (lm_conf_fn) for the section [theme]. */
bool lm_theme_conf(void *ud, const char *section, const char *key, const char *value);

/* For programs: read [theme] from $MELLAGUI_CONFIG (else the default per-user file; "none" = defaults). */
void lm_theme_load_env(void);

/* The [theme] section with the built-in values, as mellagui.conf text. */
const char *lm_theme_default_conf(void);

#endif
