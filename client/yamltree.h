#ifndef MELLAGUI_YAMLTREE_H
#define MELLAGUI_YAMLTREE_H

/*
 * YAML as a tree for the file manager's preview: the text of ONE document (the first) becomes a `struct json` tree (json.h), so the same viewer shows both.
 * What it reads is the everyday part of YAML: block mappings and sequences (also a sequence at the key's own indent and `- key: value` items), plain,
 * single and double quoted scalars (a plain one may go on over more lines), `|` and `>` block scalars, flow `[a, b]` and `{a: 1}` (over several lines),
 * comments, `---` / `...`, anchors and tags (dropped) and aliases (shown as the text `*name`). Scalars become null / true / false / numbers / strings
 * as YAML 1.2 core says. NOT understood: merge keys (shown as an ordinary key `<<`), complex keys (`? `), multi-line quoted scalars, several documents
 * (only the first is read), tabs in the indentation. The result is NULL for text that is not a mapping or a sequence at the top, or that breaks one of
 * these (then the file is simply shown as text). Free it with json_free().
 */

#include "json.h"

struct json *yaml_parse(const char *text);

#endif
