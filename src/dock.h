#ifndef MELLAGUI_DOCK_H
#define MELLAGUI_DOCK_H

/*
 * The dock: a row of round icon buttons without a background along the bottom of the primary output, in the spirit of macOS.
 * The icon under the pointer (and its neighbours) grows, a small dot under an icon says the program has a window, a click starts
 * the program or brings its window forward. This module owns the configuration; wm.c lays the row out and paints nothing itself,
 * render.c paints it (built-in icons: dockicon.c).
 *
 *   [dock]            enabled   = yes     show the dock at all
 *                     autohide  = no      show it only while the pointer touches the bottom edge of the screen
 *                     icon-size = 48      diameter of an icon, 24..128
 *                     zoom      = 160     the icon under the pointer grows to this many percent, 100..250 (100 = no magnifying)
 *                     gap       = 10      space between icons, 0..40
 *                     margin    = 8       distance of the icons from the bottom edge, 0..100
 *   [dock-apps]       Name = [icon] command        one button, in this order (at most LM_DOCK_MAX)
 *   [launcher-apps]   Name = [icon] command        buttons that are only in the launcher dialog (wm.h); drag one onto the dock to move it there
 *
 * `icon` is optional. It is the name of a built-in icon (terminal calculator files editor images settings code web run launcher generic), or
 * the path of a PNG / JPEG / PPM picture (~/ is the home directory; a picture without transparency is cut to a circle). Without it the
 * icon is guessed from the entry's name. `command` is a command line like in [menu] (@terminal, mellagui-* next to the compositor,
 * ~/, `a || b`); an entry whose program cannot be found is left out, MellaGUI's own programs always stay.
 * The command `@launcher` is not a program: the button opens the launcher dialog (wm.h lm_wm_launcher_set) and the dialog does not list it.
 * `@overview` is not one either: the button opens (and closes) the desktop overview, like Super+W; the launcher does not list it, it has no menu.
 * The built-in default is the same text, parsed by the same code; with no [dock-apps] section in the file the default entries are used.
 */

#include <stdbool.h>

#include "img.h"

#define LM_DOCK_MAX 16
#define LM_DOCK_NAME_MAX 64
#define LM_DOCK_ICON_MAX 256
#define LM_DOCK_CMD_MAX 1024

/* The round plate under the picture icons of installed programs (Firefox, Code ...), whose logos have no background of their own. */
enum { LM_DOCK_BG_LIGHT, LM_DOCK_BG_DARK, LM_DOCK_BG_NONE, LM_DOCK_BG_COLOUR };

struct lm_dock_settings {
    bool enabled, autohide;
    int icon_size, zoom, gap, margin;
    int icon_bg;                        /* LM_DOCK_BG_*: [dock] icon-background = light | dark | none | #rrggbb */
    uint32_t icon_bg_rgb;               /* ... for LM_DOCK_BG_COLOUR */
};
/* The plate colour 0xRRGGBB (light or dark: the built-in ones), or -1 for none. */
int lm_dock_plate_rgb(void);
/* "light" / "dark" / "none" / "#rrggbb" (what the file and the dock-opts setting say) into `out`. */
void lm_dock_bg_text(char *out, size_t n);
extern struct lm_dock_settings lm_dock;                  /* starts as the built-in values */

/* One button as the wm sees it (valid until the next lm_dock_resolve). */
struct lm_dock_item {
    const char *name;
    const char *cmd;                    /* expanded, ready for lm_server_spawn */
    const char *match;                  /* lower-case program name: a window whose app id (dots -> dashes) is this belongs to the button */
    int kind;                           /* enum lm_dockicon, when there is no picture */
    const struct lm_image *img;         /* premultiplied ARGB, at most the zoomed icon size, or NULL for a built-in icon */
    int raw;                            /* index of the entry in the file's list (lm_dock_raw_entry / lm_launchapps_raw_entry): entries that were left out leave gaps */
    bool launcher_only;                 /* from [launcher-apps] (lm_launchapps_item), not in the dock */
    bool plate;                         /* a picture whose corners are transparent (a logo): it is painted on a round plate (see icon_bg) */
};

/* True for the sections [dock], [dock-apps] and [launcher-apps]. */
bool lm_dock_section(const char *section);
/* Config-file callback (lm_conf_fn) for them. */
bool lm_dock_conf(void *ud, const char *section, const char *key, const char *value);
/* Forget what the file said (settings back to the built-ins, entries dropped) before it is read again. */
void lm_dock_reset(void);
/* After the file is read: install the default entries when there are none, expand commands, drop entries whose program is missing,
 * load pictures. */
void lm_dock_resolve(void);

int lm_dock_count(void);
const struct lm_dock_item *lm_dock_item(int i);          /* NULL out of range */

/* The entries as the file (or the built-in default) defines them, before anything is expanded or dropped; for the settings panel. */
int lm_dock_raw_count(void);
bool lm_dock_raw_entry(int i, const char **name, const char **icon, const char **cmd);
/* Is this command the Launcher button's (`@launcher`)? That button is always in the dock: lm_dock_resolve() puts it at the front when the file has none, and
 * the settings refuse to remove it or to give it another command. */
bool lm_dock_cmd_is_launcher(const char *cmd);
/* The built-in launcher buttons (the launcher shows them while the file has no [launcher-apps]; lm_settings_launcher_add_defaults() adds the missing ones to a
 * file that has its own list). */
int lm_launchapps_default_count(void);
bool lm_launchapps_default(int i, const char **name, const char **icon, const char **cmd);

/* The launcher dialog's own buttons, [launcher-apps] (the same "Name = [icon] command" lines, at most LM_DOCK_MAX): what the launcher shows
 * besides the dock's buttons; one whose name the dock has is not listed again (the dock's wins). Resolved like the dock's. */
int lm_launchapps_count(void);
const struct lm_dock_item *lm_launchapps_item(int i);
int lm_launchapps_raw_count(void);
bool lm_launchapps_raw_entry(int i, const char **name, const char **icon, const char **cmd);

/* Would `key = value` be accepted in [dock]? *builtin tells whether it is the built-in value (then the file needs no line). For the settings panel. */
bool lm_dock_check(const char *key, const char *value, bool *builtin);

/* The built-in [dock] and [dock-apps] text (what a user's file can start from). */
const char *lm_dock_default_conf(void);

/* The icon part of "[icon] command" values: splits `value` into the icon ("" when there is none) and the command. False when the
 * brackets are not closed or the command is empty. */
bool lm_dock_split(const char *value, char *icon, size_t icon_n, char *cmd, size_t cmd_n);

#endif
