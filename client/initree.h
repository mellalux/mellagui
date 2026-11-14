#ifndef MELLAGUI_INITREE_H
#define MELLAGUI_INITREE_H

/*
 * INI files (.ini .conf .cfg .desktop .service .properties ...) as a tree for the file manager's preview: a `[section]` is an object, `key = value`
 * (or `key: value`) a member of it, keys before the first section belong to the root. A key may repeat (every line is a member). Values are
 * trimmed, a pair of quotes round the whole value is taken off, a ` #` / ` ;` comment at the end of an unquoted value goes, `true` / `false` and
 * numbers are typed, the rest is text. Lines that start with `#` or `;` are comments. The result is NULL when a line is neither a comment, a section
 * nor a `key = value` pair (most `.conf` files of other programs are no INI: they are shown as text), or when there is nothing in the file.
 * Free it with json_free().
 */

#include "json.h"

struct json *ini_parse(const char *text);

#endif
