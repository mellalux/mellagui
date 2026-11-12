#ifndef MELLAGUI_JSON_H
#define MELLAGUI_JSON_H

/*
 * A small JSON reader for the clients (the smart home program reads the cloud's answers with it): the text becomes a tree of nodes. Numbers keep
 * their text as well (`str`), because ids may be longer than a double is exact. Nesting is limited to 64 levels.
 */

#include <stdbool.h>
#include <stddef.h>

enum json_type { JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ };

struct json {
    enum json_type type;
    char *key;                          /* the member's name when the parent is an object */
    char *str;                          /* JSON_STR: the text; JSON_NUM: the number as written */
    double num;
    bool b;
    struct json *child, *next;          /* the first member / element, the next sibling */
};

/* NULL when the text is not one JSON value (trailing white space is fine). */
struct json *json_parse(const char *text);
void json_free(struct json *j);

/* The member of an object (NULL when `obj` is NULL, no object or has no such member). */
struct json *json_get(const struct json *obj, const char *key);
/* Takes the member out of the object: the caller frees it. */
struct json *json_detach(struct json *obj, const char *key);
/* Members by name with a default: a string (numbers give their text), a number, a truth value. */
const char *json_str(const struct json *obj, const char *key, const char *def);
double json_num(const struct json *obj, const char *key, double def);
bool json_bool(const struct json *obj, const char *key, bool def);

/* Writes `s` as a JSON string with its quotes into out (cut when it does not fit; always terminated). Returns out. */
char *json_quote(const char *s, char *out, size_t n);

#endif
