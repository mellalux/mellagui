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
    X("background",           background,        0x14171f) /* panel, About dialog and the window background of the bundled programs */ \
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
    X("error",                error,             0xff7070) \
    /* ---- the colourful parts: they used to be literals in render.c, dockicon.c and the programs ---- */ \
    X("on-colour",            on_colour,         0xffffff) /* text and marks on the coloured fills (buttons, keys, workspace cells) */ \
    X("text-bright",          text_bright,       0xffffff) /* emphasised text on the normal background (the active pane's path) */ \
    X("ink",                  ink,               0x0b0d12) /* dark text on bright fills, the lock screen's backdrop */ \
    X("text-shadow",          text_shadow,       0x10141c) /* the 1 px shadow under white letters on the keyboard's keys */ \
    X("task-text",            task_text,         0xc4c9d4) /* entries of the Alt+Tab list and of menus */ \
    X("title-button-close-pressed", title_button_close_pressed, 0x8e2a20) \
    X("lock-box",             lock_box,          0x1c212b) /* the lock screen's secret box */ \
    X("lock-box-blocked",     lock_box_blocked,  0x2a1a1a) /* ... while the wrong-attempt delay runs */ \
    X("panel-line",           panel_line,        0x262c3b) /* thin lines in the panel */ \
    X("sticky-hot",           sticky_hot,        0x8fbcff) \
    X("help-card",            help_card,         0x182030) /* the launcher's small help card */ \
    X("veil",                 veil,              0xffffff) /* the light touch on dark cards (hairlines, hovered rows); a light theme makes it dark */ \
    X("glass",                glass,             0xffffff) /* the launcher's and the keyboard's translucent glass: veil, rim, icons' discs */ \
    X("glass-text",           glass_text,        0xffffff) \
    X("glass-hint",           glass_hint,        0xe6edf7) \
    X("glass-error",          glass_error,       0xffb4a8) \
    X("glass-shadow",         glass_shadow,      0x0b1220) /* the 1 px shadow under the glass texts */ \
    X("key-modifier",         key_modifier,      0x6b7a99) /* Shift, Ctrl ... on the on-screen keyboard */ \
    X("bar-deep",             bar_deep,          0x0e1118) /* the viewer's bottom bar */ \
    X("bar-text",             bar_text,          0xd8dde8) \
    X("bar-text-bright",      bar_text_bright,   0xeceff4) \
    X("viewer-button",        viewer_button,     0x1d2433) \
    X("viewer-button-hover",  viewer_button_hover, 0x2d3a56) \
    X("viewer-button-down",   viewer_button_down,  0x3d4a6a) \
    X("divider",              divider,           0x2a3040) \
    X("control",              control,           0x353b49) /* scroll bar thumbs, small buttons */ \
    X("control-hot",          control_hot,       0x4a5266) \
    X("control-idle",         control_idle,      0x4a4f5c) \
    X("gutter",               gutter,            0x10131a) /* the editor's line numbers */ \
    X("current-line",         current_line,      0x1a1e27) \
    X("terminal-button-on",   terminal_button_on, 0x5e6a82) \
    X("menu-border",          menu_border,       0x4c566a) \
    X("menu-current",         menu_current,      0x88c0d0) \
    X("progress",             progress,          0x2f6b46) /* the file manager's progress bar */ \
    X("picture",              picture,           0xc792ea) /* picture files in the file manager */ \
    X("calc-key",             calc_key,          0x4a5368) \
    X("calc-key-lit",         calc_key_lit,      0x646f88) \
    X("plate-light",          plate_light,       0xf3f5f9) /* the plate behind logo icons in the dock: light ... */ \
    X("plate-dark",           plate_dark,        0x2a2f3b) /* ... and dark */ \
    X("hue-1",                hue_1,             0x4c8dff) /* the round panel buttons (workspaces, keyboard layouts) and the keyboard's rows */ \
    X("hue-2",                hue_2,             0x3fb950) \
    X("hue-3",                hue_3,             0xf0861c) \
    X("hue-4",                hue_4,             0xa371f7) \
    X("hue-5",                hue_5,             0x2cb5a0) \
    X("hue-6",                hue_6,             0xe5534b) \
    X("hue-7",                hue_7,             0xe0b020) \
    X("hue-8",                hue_8,             0xd16bb8) \
    X("icon-generic",         icon_generic,      0x6e7681) /* the dock's built-in icons */ \
    X("icon-terminal",        icon_terminal,     0x2f353d) \
    X("icon-calculator",      icon_calculator,   0xf0861c) \
    X("icon-files",           icon_files,        0x2f81f7) \
    X("icon-editor",          icon_editor,       0x2fa84f) \
    X("icon-images",          icon_images,       0x8957e5) \
    X("icon-settings",        icon_settings,     0x768390) \
    X("icon-code",            icon_code,         0x1f6feb) \
    X("icon-web",             icon_web,          0xe8590c) \
    X("icon-run",             icon_run,          0x238636) \
    X("icon-launcher",        icon_launcher,     0x5b5fc7) \
    X("icon-camera",          icon_camera,       0xd6336c) \
    X("icon-record",          icon_record,       0xc92a2a) \
    X("icon-monitor",         icon_monitor,      0x0c8599) \
    X("icon-overview",        icon_overview,     0x3b82f6) \
    X("btn-green",            btn_green,         0x3f9a5e) /* the programs' round coloured buttons: a colour and the lighter one for hover */ \
    X("btn-green-lit",        btn_green_lit,     0x56b878) \
    X("btn-blue",             btn_blue,          0x3d6fb4) \
    X("btn-blue-lit",         btn_blue_lit,      0x5588d0) \
    X("btn-amber",            btn_amber,         0xb8803a) \
    X("btn-amber-lit",        btn_amber_lit,     0xd49a50) \
    X("btn-teal",             btn_teal,          0x2f8f9a) \
    X("btn-teal-lit",         btn_teal_lit,      0x45aab5) \
    X("btn-purple",           btn_purple,        0x8a6fb8) \
    X("btn-purple-lit",       btn_purple_lit,    0xa088d0) \
    X("btn-red",              btn_red,           0xb84545) \
    X("btn-red-lit",          btn_red_lit,       0xd65f5f) \
    X("btn-slate",            btn_slate,         0x5a6a8a) \
    X("btn-slate-lit",        btn_slate_lit,     0x7488ad) \
    X("btn-grey",             btn_grey,          0x6b6f7a) \
    X("btn-grey-lit",         btn_grey_lit,      0x868b98) \
    X("btn-orange",           btn_orange,        0xc0742f) \
    X("btn-orange-lit",       btn_orange_lit,    0xdf8f45) \
    X("btn-deep-teal",        btn_deep_teal,     0x2f7f8f) \
    X("btn-deep-teal-lit",    btn_deep_teal_lit, 0x45a0b2) \
    X("btn-rust",             btn_rust,          0xb5603c) \
    X("btn-rust-lit",         btn_rust_lit,      0xd07a52) \
    X("btn-sky",              btn_sky,           0x3c82b5) \
    X("btn-sky-lit",          btn_sky_lit,       0x569ccf) \
    X("btn-lime",             btn_lime,          0x6a9a3c) \
    X("btn-lime-lit",         btn_lime_lit,      0x84b552) \
    X("btn-rose",             btn_rose,          0xb84a6a) \
    X("btn-rose-lit",         btn_rose_lit,      0xd6638a) \
    X("btn-olive",            btn_olive,         0x7a9a2f) \
    X("btn-olive-lit",        btn_olive_lit,     0x98b84a) \
    X("btn-maroon",           btn_maroon,        0x8a3434) \
    X("btn-maroon-lit",       btn_maroon_lit,    0xad4a4a) \
    X("btn-steel",            btn_steel,         0x4a7fa8) /* the disk usage map */ \
    X("tile-other",           tile_other,        0x555b66)

#define LM_THEME_NAME_MAX 32

struct lm_theme {
#define X(name, field, def) uint32_t field;
    LM_THEME_COLORS(X)
#undef X
};

extern struct lm_theme lm_theme;      /* starts as the built-in defaults */

/* Back to the built-in colours (a reload reads the file again on top of them). */
void lm_theme_reset(void);

/*
 * Themes: `[theme] name = NAME` picks a whole palette (built-in presets "colorful" - the default, the look MellaGUI always had -,
 * "graphite", "light", "contrast"; any other NAME is the file themes/NAME.conf next to mellagui.conf, lines `colour = #rrggbb`
 * and optionally `base = PRESET`, # comments, a [theme] header is allowed). Single colours written in [theme] stay on top of the
 * theme whatever the order of the lines. lm_theme_entry's "built-in" value is the one the active theme gives.
 */
const char *lm_theme_name(void);
/* Changes with every change of the palette: a cache of something painted with it is stale when this differs. */
uint32_t lm_theme_generation(void);
/* Switches to theme `name` (colours set one by one stay). False when it is unknown or its file cannot be read; nothing changes then. */
bool lm_theme_use(const char *name);
/* Where theme files are looked for: `themes/` beside this mellagui.conf (MELLAGUI_THEMES overrides, for tests). Call before reading the file. */
void lm_theme_set_conf_path(const char *conf_path);
/* The themes there are: presets first, then the files (sorted); fills names (and descs when not NULL), returns how many. */
int lm_theme_list(char names[][LM_THEME_NAME_MAX], char descs[][80], int max);

/* Walk the palette: entry i (0..lm_theme_count()-1) with its current and the active theme's own value (0xRRGGBB). */
int lm_theme_count(void);
bool lm_theme_entry(int i, const char **name, uint32_t *value, uint32_t *builtin);
/* "#rrggbb", "rrggbb", "0xrrggbb" or "#rgb" -> 0xRRGGBB. */
bool lm_theme_parse_color(const char *text, uint32_t *out);

/* Set one colour from its config name and text. False when the name or the text is unknown / malformed. */
bool lm_theme_set(const char *name, const char *value);

/* Config-file callback (lm_conf_fn) for the section [theme]. */
bool lm_theme_conf(void *ud, const char *section, const char *key, const char *value);

/* For programs: read [theme] from $MELLAGUI_CONFIG (else the default per-user file; "none" = defaults). */
void lm_theme_load_env(void);

/* The [theme] section with the built-in values, as mellagui.conf text. */
const char *lm_theme_default_conf(void);

#endif
