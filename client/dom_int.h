/* Internals of the DOM (dom.c: tree/parser/serializer/selectors, dom_js.c: JavaScript bindings and page lifecycle). */
#ifndef LUMEN_DOM_INT_H
#define LUMEN_DOM_INT_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dom.h"
#include "js.h"

enum { DN_ELEMENT = 1, DN_TEXT = 3, DN_COMMENT = 8, DN_DOCUMENT = 9, DN_FRAGMENT = 11 };

struct dattr { char *name, *value; };

struct listener {
    char *type;
    jsval fn;
    bool is_prop;                   /* element.onclick = fn (one per type) */
    bool once;
    struct listener *next;
};

struct dnode {
    int type;
    char *tag;                      /* lowercase, elements only */
    char *text;                     /* text / comment data */
    struct dattr *attrs;
    int nattr;
    struct dnode *parent, *first, *last, *prev, *next;
    struct listener *listeners;
    jsobj *wrapper;
    int id;
    struct dom *dom;
    struct dnode *all_next;
    bool script_done;
};

struct timer {
    int id;
    jsval fn;
    jsval *args;
    int nargs;
    long long due;                  /* ms on the dom clock */
    long long interval;             /* 0 = timeout */
    struct timer *next;
};

struct dom {
    struct dom_host host;
    char url[2048];
    js *J;
    struct dnode *doc, *html, *head, *body;
    struct dnode *all_nodes;
    int next_id;
    bool dirty;
    char *title_cache;
    struct timer *timers;
    int next_timer;
    unsigned long long steps_total;
    bool budget_reported;
    long long clock_offset;         /* virtual time added to the monotonic clock (dom_run_timers_virtual) */
    struct dnode *current_script;
    struct listener *win_listeners;
    int ready;                      /* 0 loading, 1 interactive, 2 complete */
    char cookie[1024];
    jsobj *node_proto, *style_proto, *classlist_proto, *event_proto, *storage_proto;
    jsobj *local_storage, *session_storage;
    jsobj *location_obj, *history_obj, *document_obj;
    struct dnode *running_click;
};

struct sbuf { char *p; size_t n, cap; };
void sbuf_add(struct sbuf *s, const char *d, size_t n);
void sbuf_addz(struct sbuf *s, const char *z);
void sbuf_putc(struct sbuf *s, char c);

char *dstrdup(const char *s);
char *dstrndup(const char *s, size_t n);
char *dom_decode(const char *s, size_t n);

struct dnode *dnode_new(struct dom *d, int type, const char *tag);
struct dnode *dtext_new(struct dom *d, const char *text);
void dom_free_nodes(struct dom *d);
const char *dattr_get(const struct dnode *n, const char *name);
void dattr_set(struct dnode *n, const char *name, const char *value);
bool dattr_remove(struct dnode *n, const char *name);
void dnode_detach(struct dnode *n);
bool dnode_insert(struct dnode *parent, struct dnode *n, struct dnode *ref);
bool dnode_contains(const struct dnode *a, const struct dnode *b);
bool tag_is_void(const char *t);
void dom_parse_into(struct dom *d, struct dnode *root, const char *src, size_t len);
void dom_normalize(struct dom *d);
struct dnode *dom_find_tag(struct dnode *root, const char *tag);
struct dnode *dom_by_id(struct dom *d, const char *id);
struct dnode *dom_node_by_numeric_id(struct dom *d, int id);
void ser_full(struct sbuf *sb, const struct dnode *n, bool inner_only);
void dom_text_content(struct sbuf *sb, const struct dnode *n);
char *style_get(const struct dnode *n, const char *prop);
void style_set(struct dnode *n, const char *prop, const char *value);
bool node_clickable(const struct dnode *n);
bool dom_matches(const struct dnode *n, const char *selector, bool *valid);
bool dom_query(const struct dnode *root, const char *selector, bool first_only, struct dnode ***out, int *n);

#endif
