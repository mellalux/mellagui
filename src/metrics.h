#ifndef MELLAGUI_METRICS_H
#define MELLAGUI_METRICS_H

/*
 * Sizes of the shell: window frames, the panel, the app menu, the desktop gadgets and the notification cards, in pixels.
 * The defaults are the built-in look; mellagui.conf can change any of them (a value outside its range is rejected with a
 * warning and the default stays):
 *
 *   [windows]        titlebar-height = 28    border-width = 4     button-size = 24   scrollbar-width = 14   ...
 *   [panel]          height = 32             task-max-width = 160   ...
 *   [gadgets]        width = 250             margin = 12          ...
 *   [notifications]  width = 320             gap = 6
 *
 * `mellagui --print-config` lists every key with its range. The values are read once at start (wm.c and render.c use them
 * through `lm_metrics`; `#define PANEL_H (lm_metrics.panel_height)` replaces what used to be a constant). MellaGUI's own programs read [windows] too (lm_theme_load_env),
 * so scrollbar-width reaches them when they start.
 */

#include <stdbool.h>

/* X(section, config name, field, default, min, max, what) */
#define LM_METRICS(X) \
    X("windows", "titlebar-height", titlebar_height, 28, 20, 64, "height of a window's title bar") \
    X("windows", "border-width", border_width, 4, 1, 16, "frame width at the sides and the bottom") \
    X("windows", "button-size", button_size, 24, 12, 40, "close / maximize / minimize squares (kept 2 px smaller than the title bar)") \
    X("windows", "button-gap", button_gap, 4, 0, 16, "space between those buttons") \
    X("windows", "resize-corner", resize_corner, 14, 6, 40, "size of the corner zone that resizes diagonally") \
    X("windows", "double-click-ms", double_click_ms, 400, 100, 1000, "double click time in ms (title bar, gadget title, on-screen keyboard margin, files, viewer, editor; running programs keep theirs)") \
    X("windows", "wobble", wobble, 60, 0, 100, "how much a dragged window bends and wobbles (springs; 0 = it stays rigid; needs animation-ms above 0)") \
    X("windows", "min-width", min_width, 160, 50, 1000, "smallest window width a user can resize to") \
    X("windows", "min-height", min_height, 80, 40, 1000, "smallest window height") \
    X("windows", "scrollbar-width", scrollbar_width, 14, 4, 40, "width of the scroll bars of MellaGUI's own programs (the file browser)") \
    X("windows", "button-roundness", button_roundness, 100, 0, 100, "how round the dock and launcher icons and the programs' coloured buttons are (100 = circles and pills, 0 = squares; running programs keep theirs)") \
    X("windows", "animation-ms", animation_ms, 300, 0, 1000, "how long a window takes to open, close, minimize and come back, and how slowly a dragged window catches up with the pointer, in ms (0 = no animations)") \
    X("panel", "height", panel_height, 32, 20, 64, "height of the top panel") \
    X("panel", "workspace-width", panel_ws_width, 24, 16, 80, "one workspace cell") \
    X("panel", "layout-width", panel_layout_width, 36, 24, 100, "the keyboard layout indicator") \
    X("panel", "sticky-width", panel_sticky_width, 84, 50, 160, "the Sticky indicator") \
    X("panel", "caps-width", panel_caps_width, 48, 30, 120, "the Caps indicator") \
    X("panel", "task-max-width", panel_task_max_width, 160, 60, 400, "widest taskbar entry") \
    X("panel", "task-gap", panel_task_gap, 2, 0, 16, "space between taskbar entries") \
    X("gadgets", "width", gadget_width, 250, 180, 640, "clipboard gadget, starting width") \
    X("gadgets", "mixer-width", mixer_width, 250, 210, 640, "volume mixer gadget, starting width") \
    X("gadgets", "mixer-height", mixer_height, 196, 120, 560, "... starting height") \
    X("gadgets", "sysinfo-width", sys_width, 250, 210, 640, "system monitor gadget, starting width") \
    X("gadgets", "sysinfo-height", sys_height, 320, 130, 560, "... starting height") \
    X("gadgets", "margin", gadget_margin, 12, 0, 100, "distance from the screen edge when docked") \
    X("gadgets", "snap", gadget_snap, 14, 0, 60, "a dragged gadget sticks to an edge within this many pixels") \
    X("notifications", "width", note_width, 320, 200, 800, "width of a notification card") \
    X("notifications", "gap", note_gap, 6, 0, 40, "space between cards")

struct lm_metrics {
#define X(sec, name, field, def, lo, hi, what) int field;
    LM_METRICS(X)
#undef X
};

extern struct lm_metrics lm_metrics;      /* starts as the built-in defaults */

/* Config-file callback (lm_conf_fn) for the sections [windows], [panel], [gadgets], [notifications]. False for an unknown key
 * or a value that is not a whole number inside the key's range. */
bool lm_metrics_conf(void *ud, const char *section, const char *key, const char *value);

/* Walk the table: entry i (0..lm_metrics_count()-1) with its current, built-in, smallest and largest value. */
int lm_metrics_count(void);
bool lm_metrics_entry(int i, const char **section, const char **name, int *value, int *builtin, int *lo, int *hi);
/* What a size is for ("height of the top panel"), by section and name; NULL when unknown. */
const char *lm_metrics_what(const char *section, const char *name);
/* Set one value by section and name; false when unknown or out of range. */
bool lm_metrics_set(const char *section, const char *name, int value);

/* Back to the built-in sizes. */
void lm_metrics_reset(void);

/* True when `section` is one of the sections above. */
bool lm_metrics_section(const char *section);

/* After the whole file is read: bring values that depend on each other into line (button size vs title bar height). */
void lm_metrics_fixup(void);

/* The four sections with the built-in values and a comment line per key, as mellagui.conf text. */
const char *lm_metrics_default_conf(void);

#endif
