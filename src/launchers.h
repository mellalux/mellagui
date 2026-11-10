#ifndef MELLAGUI_LAUNCHERS_H
#define MELLAGUI_LAUNCHERS_H

/*
 * Launchers: the commands behind Super+Enter / Super+R / Super+I and the screenshot card (config.terminal_cmd ...), and the expansion of a
 * command line that the dock ([dock-apps]) and the [keys] action `exec` share. Nothing here is hard-coded in main.c: the built-in defaults are just
 * a mellagui.conf text (see launchers.c) read by the same code as the user's file.
 *
 *   [commands]                        terminal = mellagui-term      Super+Enter
 *   terminal / run / settings / view  run      = mellagui-run       Super+R and the panel's Run button
 *                                     settings = mellagui-settings  Super+I
 *                                     view     = mellagui-view      the screenshot card's click
 *                                     saver    = mellagui-matrix    the screensaver (a full screen client; saver.h)
 *                                     calendar = mellagui-calendar  the calendar a click on the panel's clock shows (kept running, hidden)
 *                                     suspend / reboot / poweroff = systemctl suspend ...   the session dialog's Suspend, Restart and Shutdown buttons
 *
 * A command is a shell command line. Before it is used:
 *   @terminal @run @settings @view @saver @calendar   stand for the commands above ("Files = @terminal -e mc")
 *   mellagui-calc ...                a bare mellagui-* program is looked up next to the compositor binary first, then in PATH
 *   ~/prog                           ~ is the home directory
 *   a || b                           alternatives, the first whose program exists wins (quote-aware, outside '...' / "...");
 *                                    leading `env` and VAR=value words are skipped when looking for the program
 * (The MellaGUI menu that used the same rules is gone; the dock replaced it. A [menu] section in an old file is ignored.)
 *
 * Call order in main.c: lm_launch_conf (while the config file is read) and lm_launch_set_command (--terminal, --run) collect, then
 * lm_launch_resolve (after the command line is parsed) fixes everything. The strings live in static storage.
 */

#include <stdbool.h>
#include <stddef.h>

struct lm_config;

/* Pieces of the built-in Code / Firefox commands of the default dock. VS Code: ~/VSCode/code (tarball), else the Electron binary inside the snap;
 * /snap/bin/code is deliberately not used: its launcher appends --ozone-platform=x11 after our flags and there is no X11. Firefox: native Wayland,
 * WebRender on the GPU; the snap is confined: it may only connect to sockets named wayland-N, which is what MellaGUI picks by default. */
#define LM_CODE_FLAGS "--ozone-platform=wayland --disable-gpu --no-sandbox --password-store=basic"
#define LM_FF_ENV "env MOZ_ENABLE_WAYLAND=1 GDK_BACKEND=wayland MOZ_WEBRENDER=1"

/* Config-file callback (lm_conf_fn) for the section [commands]; `ud` is the struct lm_config. */
bool lm_launch_conf(void *ud, const char *section, const char *key, const char *value);

/* Set one of the [commands] (--terminal, --run): `which` = terminal | run | settings | view | saver. */
bool lm_launch_set_command(const char *which, const char *cmd);

/* Forget everything the config file said; what the command line set (--terminal, --run) stays.
 * For a reload: call it, read the file again, then lm_launch_resolve. */
void lm_launch_reset(void);

/* Final step: expand the commands and fill terminal_cmd, run_cmd, settings_cmd, view_cmd and saver_cmd. */
void lm_launch_resolve(struct lm_config *cfg);

/* Run-time expansion of a command line for the [keys] action `exec` (the rules above: @tokens, ~, mellagui-*, a || b).
 * `out` always holds something usable: the expansion, or the text as written when no alternative's program exists. */
void lm_launch_expand(const char *spec, char *out, size_t n);

/* Like lm_launch_expand, and tells whether a program of `spec` exists (MellaGUI's own programs count as existing): the dock leaves out
 * entries for which it is false. `out` is filled as lm_launch_expand does. */
bool lm_launch_exists(const char *spec, char *out, size_t n);

/* The built-in mellagui.conf text (what a user's file can start from); `mellagui --print-config` shows it. */
const char *lm_launch_default_conf(void);

#endif
