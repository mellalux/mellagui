#ifndef MELLAGUI_TOMLTREE_H
#define MELLAGUI_TOMLTREE_H

/*
 * TOML as a tree for the file manager's preview: the document becomes a `struct json` tree (json.h), so the viewer of JSON, YAML and XML shows it too.
 * Tables (`[a.b]`, dotted keys, inline `{}`) are objects, arrays of tables (`[[a]]`) and arrays arrays, strings (basic, literal, and their multi-line
 * forms), integers (`_`, hex / octal / binary), floats (inf, nan), booleans and the date / time values (shown as text) are values.
 * The result is NULL for text that breaks TOML's grammar or has nothing in it (the file is then shown as text). Free it with json_free().
 */

#include "json.h"

struct json *toml_parse(const char *text);

#endif
