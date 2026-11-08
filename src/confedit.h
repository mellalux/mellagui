#ifndef MELLAGUI_CONFEDIT_H
#define MELLAGUI_CONFEDIT_H

/*
 * Editing mellagui.conf from the settings panel without destroying what the user wrote by hand: only the section that is
 * being changed is rewritten, every other byte (comments, order, spacing, other sections) is kept as it was.
 */

#include <stdbool.h>

/* Replace the body of [section] by `body` (text of "key = value\n" lines, may be empty). Comment lines right under the header
 * stay. A section that does not exist is appended; with body == NULL the whole section (header included) is removed.
 * The file and its directories are created when missing; the write is atomic (temp file + rename). False on an I/O error. */
bool lm_confedit_set_section(const char *path, const char *section, const char *body);

/* Set (value != NULL) or delete (value == NULL) the line of [section] whose key `same(existing_key, ud)` accepts, keeping every other line
 * and comment. The first matching line is rewritten as "key = value" (with the new spelling of `key`), further matches are removed.
 * With no match a new line goes at the end of the section (after its last non-blank line); a missing section is appended.
 * Same file rules as lm_confedit_set_section. */
bool lm_confedit_set_key(const char *path, const char *section, bool (*same)(const char *existing_key, void *ud), void *ud,
                         const char *key, const char *value);

#endif
