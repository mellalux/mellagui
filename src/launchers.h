#ifndef MELLAGUI_LAUNCHERS_H
#define MELLAGUI_LAUNCHERS_H

/*
 * Launchers: the "MellaGUI" app menu and the commands behind Super+Enter / Super+R / Super+I (config.apps,
 * config.terminal_cmd ...). Nothing here is hard-coded in main.c any more: the built-in defaults are just a
 * mellagui.conf text (see launchers.c) read by the same code as the user's file.
 *
 *   [commands]                        terminal = mellagui-term      Super+Enter and the panel launcher
 *   terminal / run / settings / view  run      = mellagui-run       Super+R and the panel's Run button
 *                                     settings = mellagui-settings  Super+I and the Settings entry
 *                                     view     = mellagui-view      the screenshot card's click
 *   [menu]                            Name = command                one menu entry, in this order; if the file has
 *                                     Name = a || b                 a [menu] section it replaces the default menu
 *                                                                   (--app NAME=CMD adds one more)
 *
 * A command is a shell command line. Before it is used:
 *   @terminal @run @settings @view   stand for the commands above ("Files = @terminal -e mc")
 *   mellagui-calc ...                a bare mellagui-* program is looked up next to the compositor binary first, then in PATH
 *   ~/prog                           ~ is the home directory
 *   a || b                           alternatives, the first whose program exists wins (quote-aware, outside '...' / "...");
 *                                    leading `env` and VAR=value words are skipped when looking for the program
 * A menu entry whose program cannot be found is left out, except MellaGUI's own programs (@tokens and mellagui-*),
 * which always stay so a missing build shows up as a menu entry that does nothing and not as a changed menu.
 *
 * Call order in main.c: lm_launch_conf (while the config file is read) and lm_launch_add (--app) collect, then
 * lm_launch_resolve (after the command line is parsed) fixes everything. The strings live in static storage.
 */

#include <stdbool.h>
#include <stddef.h>

struct lm_config;

/* Config-file callback (lm_conf_fn) for the sections [commands] and [menu]; `ud` is the struct lm_config. */
bool lm_launch_conf(void *ud, const char *section, const char *key, const char *value);

/* Set one of the [commands] (--terminal, --run): `which` = terminal | run | settings | view. */
bool lm_launch_set_command(const char *which, const char *cmd);

/* Forget everything the config file said (commands and [menu]); what the command line set (--terminal, --run, --app) stays.
 * For a reload: call it, read the file again, then lm_launch_resolve. */
void lm_launch_reset(void);

/* One more menu entry (--app NAME=CMD). Returns false when the menu is full or a string is too long. */
bool lm_launch_add(struct lm_config *cfg, const char *name, const char *cmd);

/* Final step: install the default menu when none was given, expand @tokens / ~ / mellagui-* / alternatives and fill
 * terminal_cmd, run_cmd, settings_cmd and view_cmd. */
void lm_launch_resolve(struct lm_config *cfg);

/* Run-time expansion of a command line for the [keys] action `exec` (same rules as the menu: @tokens, ~, mellagui-*, a || b).
 * `out` always holds something usable: the expansion, or the text as written when no alternative's program exists. */
void lm_launch_expand(const char *spec, char *out, size_t n);

/* The built-in mellagui.conf text (what a user's file can start from); `mellagui --print-config` shows it. */
const char *lm_launch_default_conf(void);

#endif
