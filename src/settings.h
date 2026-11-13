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

 *   saver           0 | 1                      start the screensaver (mellagui-matrix, [commands] saver) when nothing was touched for saver-minutes
 *   saver-minutes   0 .. 240                   that many minutes of idle time, 0 = never (default 10; saved)
 *   osk-avoid       0 | 1                      windows keep out of the on-screen keyboard's way: maximized ones get smaller, others move up (default 1; saved)
 *   layout-badges   0 | 1                      the panel shows a badge for every enabled keyboard layout and a click picks that one (default 0 = one badge, a click switches to the next; saved)
 *   saver-blank     0 .. 240                   with the screensaver on: the monitors go to sleep (fading out first) that many minutes after the last input, 0 = never (default 15; saved)
 *   lock-wallpaper  0 | 1                      the lock screen shows the wallpaper, blurred and dimmed, behind the clock (default 1; saved; no wallpaper = plain dark)
 *   layout-label    none | code | name         with layout-badges: written next to each flag - nothing, the short code (US, EE) or the language (default none; saved)
 *   saver-now       (write only)               1 = start it now (a preview), 0 = end it (saver.h)
 *   resolution      auto | preferred | WxH[@Hz] video mode of the outputs. Applied at once; unless "kept" (resolution-confirm=1)
 *                                              it is undone after 15 s (env MELLAGUI_REVERT_MS), a black screen cannot confirm
 *                                              anything. Only a kept value is saved. auto = the preferred mode corrected to the
 *                                              monitor's native shape (edid.h), preferred = the monitor's own preferred mode
 *   wallpaper       "" | file name             desktop background picture (PNG, JPEG or PPM; "~/" = home; at most 200 bytes); "" = the gradient
 *   wallpaper-mode  fill | fit | stretch | center   how it is placed: cover and crop, whole picture with bars, stretched, unscaled in the middle
 *   wallpaper-status (read only)               "" (none set) | ok | error: <why> for the last wallpaper that was set
 *   output-order    "HDMI-A-1,DP-2" | ""       connector names, leftmost monitor first; outputs not named follow in connection order (empty = connection
 *                                              order). Windows move with their monitor
 *   outputs         (read only)               "name|WxH|x,y;..." the outputs, left to right
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
 *   notify          (write only)               "title<TAB>body<TAB>milliseconds": a notification card at the top right (0 ms = until clicked,
 *                                              default 6000; mellagui-notify sends it)
 *   config-file     (read only)                the mellagui.conf that is read ("" = none; dock, shortcuts, colours, sizes: conf.h)
 *   config-status  (read only)                 ok | missing (set but not readable) | none | ignored:N (N lines rejected by the last reload)
 *   config-reload   (write only)               1 = read mellagui.conf again (same as Super+Shift+R / SIGUSR2)
 *   config-create   (write only)               1 = write the built-in defaults to config-file when it does not exist yet
 *   dock-0 .. dock-15 (read only)              "name<US>icon<US>command" of the buttons of [dock-apps] in mellagui.conf (<US> = 0x1f; raw text, before
 *                                              @tokens are expanded; the built-in default dock while the file has none), "" past the end
 *   dock-opts       (read only)                "enabled=1 autohide=0 icon-size=48 zoom=160 gap=10 margin=8": the [dock] options
 *   dock-put        (write only)               "index<US>name<US>icon<US>command": replace button `index` or, with -1, add one (at most 16; a name is
 *                                              unique, no '=' and no leading '#;[' or spaces at the ends; icon may be empty, has no brackets)
 *   dock-del        (write only)               "index": remove a button
 *   dock-move       (write only)               "index<US>-1|1": swap a button with its neighbour
 *   dock-set        (write only)               "option<US>value": one line of [dock] (the built-in value removes the line)
 *   dock-reset      (write only)               1 = remove [dock] and [dock-apps] from the file, the built-in dock returns
 *                                              These rewrite only those sections of the file (confedit.c: comments, other sections
 *                                              and order stay), create the file when missing, and read it again at once. Refused without a file.
 *   keys-count      (read only)                number of shortcuts in the table (keys.h)
 *   keys-page       0..                        which ten of them keys-row-N shows (clamped to the last page)
 *   keys-row-0 .. keys-row-9 (read only)       "Super+Q<US>close" the shortcut at keys-page * 10 + N, "" past the end
 *   keys-actions    (read only)                the action names, space separated
 *   keys-capture    0 | 1                      1 = the next key combination is only recorded (wm.h lm_wm_key_capture); back to 0 when it was
 *   keys-captured   (read only)                the recorded combination ("" = cancelled with Escape or none yet)
 *   keys-set        (write only)               "combo<US>action [arg]": bind (written to [keys] of the file; dropped from it when it equals
 *                                              the built-in binding); refused for an unknown key or action
 *   keys-del        (write only)               "combo": switch a built-in shortcut off ("combo = none") or drop an own one
 *   keys-reset      (write only)               1 = remove [keys] from the file, the built-in shortcuts return
 *                                              The three rewrite only the [keys] lines concerned (confedit.c), read the file again and need a file.
 *   theme-colors    (read only)                "name=#rrggbb name=#rrggbb* ...": every colour of the palette (theme.h), * = differs from the built-in one
 *   theme-set       (write only)               "name<US>#rrggbb": set a colour in [theme] of the file (the line is dropped when it equals the built-in one)
 *   theme-del       (write only)               "name": back to the built-in colour (drops its line)
 *   theme-reset     (write only)               1 = remove [theme] from the file
 *                                              All three rewrite only the [theme] lines concerned, read the file again (the compositor repaints at once;
 *                                              programs already running keep their colours, the panel follows theme-colors itself) and need a file.
 *   metrics         (read only)                "section.name=value:min:max:builtin ...": every size of metrics.h ([windows], [panel], [gadgets], [notifications])
 *   metrics-set     (write only)               "section.name<US>value": set a size in its section of the file (no line when it equals the built-in one)
 *   metrics-reset   (write only)               1 = remove the four size sections from the file
 *                                              Both rewrite only the lines concerned, read the file again (the wm refits windows at once), need a file.
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
struct lm_client;
struct lm_settings;

/* May this client use MellaGUI's private globals (settings, capture)? The rule of `--settings-access` above. */
bool lm_settings_client_allowed(struct lm_server *srv, const struct lm_client *c);

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

/* The dock's and the launcher dialog's button lists in mellagui.conf ([dock-apps] / [launcher-apps]); every call rewrites its section(s) and reads
 * the file again. Indexes are lm_dock_item.raw. False when there is no config file, the values are not acceptable (name in use, list full) or an
 * index is wrong.
 *   dock_put / launcher_put      a new button at the end (icon "" = guessed from the name)
 *   dock_del / launcher_del      remove one
 *   dock_reorder                 move button `from` so that it ends up at `to`
 *   launcher_to_dock             a launcher button joins the dock, before dock raw index `at` (-1 = at the end), and leaves the launcher list
 *   dock_to_launcher             a dock button leaves the dock and goes to the launcher list */
bool lm_settings_dock_put(struct lm_server *srv, const char *name, const char *icon, const char *cmd);
bool lm_settings_dock_del(struct lm_server *srv, int raw_index);
/* Change one entry of [dock-apps] / [launcher-apps] in place (the launcher dialog's edit form); false when the text is not allowed, another entry
 * has that name or there is no config file. */
bool lm_settings_dock_replace(struct lm_server *srv, int raw_index, const char *name, const char *icon, const char *cmd);
bool lm_settings_launcher_replace(struct lm_server *srv, int raw_index, const char *name, const char *icon, const char *cmd);
bool lm_settings_launcher_put(struct lm_server *srv, const char *name, const char *icon, const char *cmd);
int lm_settings_launcher_add_defaults(struct lm_server *srv);   /* the built-in launcher buttons the file lacks are written to [launcher-apps]; returns how many, -1 = no file */
bool lm_settings_launcher_del(struct lm_server *srv, int raw_index);
bool lm_settings_dock_reorder(struct lm_server *srv, int from, int to);
bool lm_settings_launcher_to_dock(struct lm_server *srv, int raw_index, int at);
bool lm_settings_dock_to_launcher(struct lm_server *srv, int raw_index);

/* Current value of `key` as text; false for an unknown key. */
bool lm_settings_get(struct lm_settings *s, const char *key, char *out, size_t n);
/* Same as a `set` request (validates, applies, saves, announces). */
bool lm_settings_set(struct lm_settings *s, const char *key, const char *value);
/* Applies config.scale_pct to every output (output.h) and tells the wm and the clients. */
void lm_settings_apply_scale(struct lm_server *srv);
/* "key=value key=value ..." for the control socket. */
void lm_settings_dump(struct lm_settings *s, char *out, size_t n);

#endif
