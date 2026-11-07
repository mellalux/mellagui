#ifndef MELLAGUI_SETTINGS_H
#define MELLAGUI_SETTINGS_H

/*
 * User settings: the settings panel (client/mellagui-settings.c) talks to the compositor through the private
 * `mellagui_settings_v1` global, and the values survive restarts in a plain `key=value` file.
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
 *   mixer           0 | 1                      volume mixer gadget on the desktop (Super+V)
 *   sysinfo         0 | 1                      system monitor gadget: CPU, memory, temperature (Super+S)
 *   resolution      auto | preferred | WxH[@Hz] video mode of the outputs. Applied at once; unless "kept" (resolution-confirm=1)
 *                                              it is undone after 15 s (env MELLAGUI_REVERT_MS), a black screen cannot confirm
 *                                              anything. Only a kept value is saved. auto = the preferred mode corrected to the
 *                                              monitor's native shape (edid.h), preferred = the monitor's own preferred mode
 *   wallpaper       "" | file name             desktop background picture (PNG or PPM; "~/" = home; at most 200 bytes); "" = the gradient
 *   wallpaper-mode  fill | fit | stretch | center   how it is placed: cover and crop, whole picture with bars, stretched, unscaled in the middle
 *   wallpaper-status (read only)               "" (none set) | ok | error: <why> for the last wallpaper that was set
 *   audio-out       ALSA device name           where MellaGUI's own beeps are played (aplay -D); "default" or e.g. plughw:CARD=PCH,DEV=3
 *   file            (read only)                path the settings are saved to, "" = not saved
 *   -- live state, never saved: --
 *   resolutions     "WxH@Hz|note;..."          modes the first output can show (read only)
 *   resolution-now  WxH@Hz                     the mode the first output shows now (read only)
 *   monitor         "make|model|connector|mm_w|mm_h"  the first output's monitor (EDID: manufacturer and name; sizes 0 = unknown) (read only)
 *   resolution-confirm  ""|seconds             non-empty while a resolution change waits to be kept; set 1 = keep, 0 = go back
 *   audio-outs      "name|description;..."     ALSA playback devices from /proc/asound, HDMI/DisplayPort ones marked (read only)
 *   sinks, sink     list / name                PulseAudio/PipeWire outputs and the default one (pactl, else wpctl; `sink` is
 *                                              settable); empty until audio-rescan was set and when there is no sound server
 *   audio-profiles  "cardid:index|Card - Profile;..."  PipeWire card outputs (analog / HDMI / S/PDIF; pw-cli), read only
 *   audio-profile   cardid:index               the active entry of audio-profiles; setting one switches the card to that output
 *                                              (this is how HDMI audio is chosen); the sink list is refreshed 1-3 s later
 *   sinks-status    ""|ok|missing-tools|no-server  why `sinks` is empty: no pactl/wpctl installed, or no server answers (read only)
 *   audio-rescan    (write only)               any value: ask pactl again (the panel does when its Sound page opens)
 *   lock-kind       none | password | pin      screen lock (lock.h); its own file lock.conf, not this one (read only)
 *   lock-timeout    minutes, 0 = never         lock after this much idle time (read only; change with lock-set)
 *   lock-start      0 | 1                      lock when MellaGUI starts (read only; change with lock-set)
 *   lock-blank      minutes, 0 = never         monitors go to standby this long after the last input while locked (read only)
 *   lock-vt         0 | 1                      Ctrl+Alt+F1..F12 switches the console while the screen is locked (read only)
 *   lock-status     ""|ok|wrong|wait:N|weak|invalid|error   result of the last lock-set (read only)
 *   lock-set        (write only)               "current<TAB>kind<TAB>new<TAB>minutes<TAB>start<TAB>blank<TAB>vt": change the lock; `current` must be
 *                                              the existing secret (ignored while none is set), kind = password|pin|keep|none,
 *                                              empty minutes / start / blank / vt = keep. Never echoed, never logged
 *   lock-now        (write only)               1 = lock the screen now (refused when no secret is set)
 *   locked          0 | 1                      the screen is locked now (read only)
 *
 * Protocol: the client sends `set(key, value)`; the compositor validates it, applies it, and answers every bound
 * client with one `value` event per key (the effective values, so a rejected change shows up as the old value)
 * followed by `done`. Binding sends the same burst. Changes made elsewhere (Super+Space, five Shifts, Super+G)
 * are announced the same way through lm_settings_changed().
 *
 * File: <--settings path>, written (atomically, ~0.3 s after the last change) after every `set`. It is read
 * before the command line is parsed, so command-line options win for that run. Access: the socket is 0600 and only the
 * compositor's own user (and root) may connect; on the drm backend only MellaGUI's own programs (executables called
 * mellagui-* in the compositor's directory) get the settings, any other client binds an inert object (config.settings_access,
 * `--settings-access any|own`; the headless backend allows everyone, the tests need it). The lock is the exception to "any
 * client may change it": `lock-set` needs the current secret.
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
