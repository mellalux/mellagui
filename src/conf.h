#ifndef MELLAGUI_CONF_H
#define MELLAGUI_CONF_H

/*
 * Generic reader for MellaGUI's user configuration (mellagui.conf): an INI-like text file.
 *
 *   # comment            (also ';' at the start of a line)
 *   [section]            sections group the keys: menu, commands, keys, theme, panel ...
 *   key = value          key: everything before the first '=', trimmed (it may contain spaces, "Text Editor = ...");
 *                        value: the rest of the line, trimmed (a '=' inside it is fine: "env A=1 prog")
 *
 * The reader only splits lines; what a section means is decided by the callback, so every module owns its section
 * (the menu: launchers.c, ...). Unknown sections / keys are the callback's call: return false and the line is
 * reported as a warning with its file and number, the rest of the file is still read.
 *
 * Shared by the compositor and (later) the clients, so it depends on nothing but log.h.
 */

#include <stdbool.h>

/* Called for every `key = value` line. `section` is "" before the first [section] header, lower-cased otherwise. */
typedef bool (*lm_conf_fn)(void *ud, const char *section, const char *key, const char *value);

/* Parse `text` (NUL terminated). `origin` is only used in messages ("file:line: ..."). Returns the number of lines that were
 * rejected (syntax error or callback returned false). */
int lm_conf_parse(const char *text, const char *origin, lm_conf_fn fn, void *ud);

/* Read and parse a file. Returns -1 when it cannot be opened (a missing file is not an error for the caller), else as lm_conf_parse. */
int lm_conf_load(const char *path, lm_conf_fn fn, void *ud);

#endif
