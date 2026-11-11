#ifndef MELLAGUI_SEARCH_H
#define MELLAGUI_SEARCH_H

/*
 * The launcher's search (Spotlight style): what is typed in the Launchpad finds, besides the buttons of the dock and the launcher,
 *   - a calculation: "12*3+4", "sqrt(2)", "2^10" ... shows "= 40" (Enter copies the result to the clipboard),
 *   - the installed programs (the .desktop files of $XDG_DATA_DIRS, ~/.local/share, snap and flatpak: Name, GenericName, Keywords; NoDisplay and Hidden ones
 *     are left out), started with their Exec line (in a terminal when they say Terminal=true),
 *   - files and folders of the home folder by name (an index that is made in the background, a few hundred entries every 15 ms, depth 6, no dot folders),
 *     opened by the program of the suite that fits (pictures and videos: the viewer, text: the editor, folders: the file manager, else xdg-open).
 * The results are `struct lm_dock_item`s like the buttons, so the launcher paints and starts them in the same way; their `raw` is -1000 - i.
 *
 * Tests: $MELLAGUI_APPS_DIRS (colon separated, replaces the application folders), $MELLAGUI_SEARCH_ROOT (colon separated, replaces the home folder).
 */

#include <stdbool.h>
#include <stddef.h>

#include "dock.h"

struct lm_loop;

#define LM_SEARCH_MAX 24                /* results at most: the calculation, up to 12 programs, up to 8 files */

/* Starts the file index in the background (once; it is made again when it is older than 10 minutes) and reads the programs on the first call. */
void lm_search_begin(struct lm_loop *loop);
void lm_search_shutdown(void);

/* The results for `query` (one word or more, case does not matter); `known(name)` says that a button of that name exists already (it is not listed twice).
 * Returns how many there are. The items stay valid until the next call. */
int lm_search_query(const char *query, bool (*known)(const char *name));
const struct lm_dock_item *lm_search_item(int i);
/* One line about result i for the hint ("/home/me/notes.txt", the program's command): valid until the next query. */
const char *lm_search_detail(int i);
/* Is this one of the results (and not a button of the file)? */
bool lm_search_owns(const struct lm_dock_item *it);
/* Entries in the file index so far, and whether it is complete (tests). */
int lm_search_indexed(bool *done);

/* The calculator: false when `text` is no calculation (it needs an operator, a function or a bracket, so that a plain number or word is not one). */
bool lm_calc_eval(const char *text, double *out);
void lm_calc_format(double v, char *out, size_t n);

#endif
