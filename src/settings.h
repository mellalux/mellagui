#ifndef LUMEN_SETTINGS_H
#define LUMEN_SETTINGS_H

/*
 * User settings: the settings panel (client/lumen-settings.c) talks to the compositor through the private
 * `lumen_settings_v1` global, and the values survive restarts in a plain `key=value` file.
 *
 *   key             value                      effect
 *   layouts         "us,ee,de" (1..4 ids)      keyboard layouts to switch between; a new keymap goes to every client
 *   layout          "ee"                       active layout (must be one of `layouts`)
 *   sticky          0 | 1                      sticky keys
 *   repeat-delay    100..2000 (ms)             key repeat delay    (wl_keyboard.repeat_info)
 *   repeat-rate     0..100 (keys/s)            key repeat rate, 0 = no repeat
 *   pointer-speed   10..500 (percent)          pointer speed multiplier
 *   natural-scroll  0 | 1                      reversed wheel / touchpad scrolling
 *   sound           0 | 1                      beeps for sticky keys
 *   gadget          0 | 1                      clipboard gadget on the desktop (Super+G)
 *   file            (read only)                path the settings are saved to, "" = not saved
 *
 * Protocol: the client sends `set(key, value)`; the compositor validates it, applies it, and answers every bound
 * client with one `value` event per key (the effective values, so a rejected change shows up as the old value)
 * followed by `done`. Binding sends the same burst. Changes made elsewhere (Super+Space, five Shifts, Super+G)
 * are announced the same way through lm_settings_changed().
 *
 * File: <--settings path>, written (atomically, ~0.3 s after the last change) after every `set`. It is read
 * before the command line is parsed, so command-line options win for that run. There is no authentication:
 * like everything else on the socket, any client of the user's session may change the settings.
 */

#include <stdbool.h>
#include <stddef.h>

struct lm_config;
struct lm_server;
struct lm_settings;

/*
 * Early (before the server exists): read `path` into cfg. The layouts list is returned as text in layouts_out
 * (empty when the file has none) because main.c parses --layouts itself and lets the command line win.
 * A missing file is fine. Returns the number of keys applied.
 */
int lm_settings_load(struct lm_config *cfg, const char *path, char *layouts_out, size_t layouts_n);

/* Registers the global. Create after input, seat and wm. */
struct lm_settings *lm_settings_create(struct lm_server *srv);
void lm_settings_destroy(struct lm_settings *s);         /* flushes a pending save */

/* The live state changed behind the panel's back: tell the bound clients. NULL-safe. */
void lm_settings_changed(struct lm_settings *s);

/* Current value of `key` as text; false for an unknown key. */
bool lm_settings_get(struct lm_settings *s, const char *key, char *out, size_t n);
/* Same as a `set` request (validates, applies, saves, announces). */
bool lm_settings_set(struct lm_settings *s, const char *key, const char *value);
/* "key=value key=value ..." for the control socket. */
void lm_settings_dump(struct lm_settings *s, char *out, size_t n);

#endif
