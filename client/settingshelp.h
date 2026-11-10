#ifndef MELLAGUI_SETTINGSHELP_H
#define MELLAGUI_SETTINGSHELP_H

/*
 * The help texts of the settings panel: the small "?" circle at the end of a setting opens one of these in a modal card.
 *
 * Every entry is found by the setting's key (the widget's `key`; the radios of one group share it, and `arg` tells a group's members apart where
 * their meanings differ), a heading by "@" and its text. The text may hold '\n' (a new paragraph); the panel wraps the lines itself.
 */

#include <stdbool.h>

/* The help for key / arg ("" = none): its title and text, or false when there is none. */
bool settings_help(const char *key, const char *arg, const char **title, const char **text);

/* The help of a key that comes with a number (the Sizes page's "met-3"): false when `key` is not one of those. */
bool settings_help_known(const char *key, const char *arg);

#endif
