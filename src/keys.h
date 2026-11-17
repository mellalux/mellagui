#ifndef MELLAGUI_KEYS_H
#define MELLAGUI_KEYS_H

/*
 * Keyboard shortcuts of the window manager (lm_wm_key in wm.c): a table of  modifiers + key -> action, filled from
 * the built-in defaults and then from the [keys] section of mellagui.conf.
 *
 *   [keys]
 *   Super+Return        = terminal
 *   Super+Shift+1       = move-to-workspace 1
 *   Super+T             = exec @terminal -e htop        # any shell command; @terminal @run @settings @view as in [dock-apps]
 *   Alt+F4              = close
 *   Super+Q             = none                          # removes a default
 *
 * A line replaces the binding of the same combination; the rest of the defaults stay. `mellagui --print-config` lists them all.
 *
 * Combination: modifiers Super (Win, Mod4, Meta), Ctrl (Control), Alt, Shift, joined with '+', then one key: A..Z, 0..9, F1..F24,
 * Enter (Return), Tab, Space, Escape, Backspace, Delete, Insert, Home, End, PageUp, PageDown, Left, Right, Up, Down, Minus, Equal,
 * Comma, Period, Slash, Semicolon, Apostrophe, Grave, Backslash, LeftBrace, RightBrace, and the extra keys of a multimedia keyboard:
 * VolumeUp, VolumeDown, VolumeMute, MicMute, PlayPause, Play, NextTrack, PrevTrack, StopMedia, Eject, Calculator, Mail, Browser (HomePage),
 * WWW, Search, Files, Favorites (Bookmarks), Config, Sleep, Phone, Player, BrightnessUp, BrightnessDown. Case does not matter. The modifiers
 * must match exactly (Super+Q does not fire for Super+Shift+Q).
 *
 * Actions (ARG = number / command):
 *   terminal run settings            the [commands] programs          exec CMD             a shell command
 *   close fullscreen maximize-toggle  the focused window               maximize / restore   (Super+Up / Super+Down)
 *   tile-left tile-right             half of the screen                workspace N          switch (1..4)
 *   tile-top-left tile-top-right tile-bottom-left tile-bottom-right   a quarter of the screen (Super+Ctrl+Left / Right, with Shift: the lower ones)
 *   tile-third 1|2|3   the left, middle or right third (full height);  tile-twothirds 1|2   the left or the right two thirds
 *   tile-grid-move left|right|up|down   into the cell of the window grid ([windows] grid-columns x grid-rows) the window is in, then one cell on
 *   move-to-workspace N              move the focused window           focus-next focus-prev  cycle the windows
 *   layout-next layout-prev          keyboard layout                   lock                 lock the screen
 *   gadget-clipboard gadget-mixer gadget-system gadget-smart   show / hide a gadget   clipboard-pick     choose from the history with the arrows
 *   quit                             leave MellaGUI (asks for the lock secret first)       none  remove the binding
 *   launcher                         the launcher dialog: big icons of the dock, add / remove (Super+A)
 *   osk                              the on-screen keyboard: show / hide (Super+K, the panel button)
 *   overview                         the desktop overview: all windows of this workspace as pictures (Super+W)
 *   reload-config                    read mellagui.conf again (theme, sizes, shortcuts, dock; see config.reload)
 *   volume-up volume-down volume-mute  the default output's volume (5 % a step, repeats while held) and mute
 *   media play-pause|play|pause|next|previous|stop   the music / video player that speaks MPRIS (a browser, Spotify, mpv ...), through busctl
 *
 * Not configurable (modal or hardware-level): Ctrl+Alt+F1..F12, Alt+Tab, Print / Alt+Print / Ctrl+Print, Esc in dialogs.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum lm_action {
    LM_ACT_NONE,
    LM_ACT_TERMINAL, LM_ACT_RUN, LM_ACT_SETTINGS, LM_ACT_EXEC,
    LM_ACT_LAYOUT_NEXT, LM_ACT_LAYOUT_PREV,
    LM_ACT_GADGET_CLIPBOARD, LM_ACT_CLIPBOARD_PICK, LM_ACT_GADGET_MIXER, LM_ACT_GADGET_SYSTEM, LM_ACT_GADGET_SMART,
    LM_ACT_CLOSE, LM_ACT_LOCK, LM_ACT_QUIT,
    LM_ACT_WORKSPACE, LM_ACT_MOVE_TO_WORKSPACE, LM_ACT_FOCUS_NEXT, LM_ACT_FOCUS_PREV,
    LM_ACT_FULLSCREEN, LM_ACT_MAXIMIZE_TOGGLE, LM_ACT_MAXIMIZE, LM_ACT_RESTORE, LM_ACT_TILE_LEFT, LM_ACT_TILE_RIGHT,
    LM_ACT_TILE_TOP_LEFT, LM_ACT_TILE_TOP_RIGHT, LM_ACT_TILE_BOTTOM_LEFT, LM_ACT_TILE_BOTTOM_RIGHT,
    LM_ACT_RELOAD_CONFIG, LM_ACT_LAUNCHER, LM_ACT_OSK, LM_ACT_QUICK, LM_ACT_MAGNIFY_IN, LM_ACT_MAGNIFY_OUT, LM_ACT_OVERVIEW, LM_ACT_NIGHT_LIGHT, LM_ACT_DND,
    LM_ACT_GADGET_TOGGLE,
    LM_ACT_TILE_THIRD, LM_ACT_TILE_TWOTHIRDS, LM_ACT_TILE_GRID_MOVE,
    LM_ACT_VOLUME_UP, LM_ACT_VOLUME_DOWN, LM_ACT_VOLUME_MUTE, LM_ACT_MEDIA,
};

#define LM_KEYBIND_ARG 256
#define LM_MAX_KEYBINDS 128
#define LM_KEY_MODS_MASK 0x4du      /* LM_MOD_SHIFT | CTRL | ALT | SUPER (keymap.h) */

struct lm_keybind {
    uint32_t mods;                  /* LM_MOD_SHIFT | LM_MOD_CTRL | LM_MOD_ALT | LM_MOD_SUPER */
    uint32_t code;                  /* evdev KEY_* */
    enum lm_action action;
    int num;                        /* workspace actions: 0-based; tile-third / tile-twothirds: 0-based place; tile-grid-move: 0 left, 1 right, 2 up, 3 down */
    char arg[LM_KEYBIND_ARG];       /* exec: the command */
};

struct lm_keybinds {
    struct lm_keybind b[LM_MAX_KEYBINDS];
    int n;
};

/* The MPRIS method ("PlayPause") of a `media` action's word ("play-pause"), NULL when the word is not one. */
const char *lm_media_method(const char *arg);

/* "Super+Shift+1" for a combination (canonical spelling); false when the key has no name in the table above. */
bool lm_keys_combo_text(uint32_t mods, uint32_t code, char *out, size_t n);
/* "move-to-workspace 1" / "exec firefox": the action part of a binding. */
void lm_keys_action_text(const struct lm_keybind *b, char *out, size_t n);
/* Parse "Super+Shift+1" (case does not matter). */
bool lm_keys_parse_combo(const char *text, uint32_t *mods, uint32_t *code);
/* Would the line `combo = value` of [keys] be accepted (and is it not "none")? For the settings panel. */
bool lm_keys_valid(const char *combo, const char *value);
/* True when two spellings name the same combination ("alt+f4", "Alt+F4"). */
bool lm_keys_same_combo(const char *a, const char *b);
/* The built-in binding of a combination, or NULL. */
const struct lm_keybind *lm_keys_builtin(uint32_t code, uint32_t mods);
/* All action names, space separated ("terminal run settings exec ..."). */
const char *lm_keys_action_names(void);

/* Fill with the built-in bindings (the same text lm_keys_default_conf() returns). */
void lm_keys_defaults(struct lm_keybinds *k);

/* Config-file callback (lm_conf_fn) for the section [keys]; `ud` is the struct lm_keybinds. */
bool lm_keys_conf(void *ud, const char *section, const char *key, const char *value);

/* The binding for this key and modifier state (`mods` may hold any LM_MOD_* bits, only the four above count); NULL if none. */
const struct lm_keybind *lm_keys_find(const struct lm_keybinds *k, uint32_t code, uint32_t mods);

/* "Super+Shift+1 = move-to-workspace 1" for entry i of the table. */
void lm_keys_describe(const struct lm_keybind *b, char *out, size_t n);

/* The [keys] section with the built-in bindings, as mellagui.conf text. */
const char *lm_keys_default_conf(void);

#endif
