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

/* Author-specified style of one element (-1 = not specified), see dom_css.c */
struct estyle {
    int32_t fg;                     /* 0xRRGGBB or -1 */
    int32_t bg;                     /* 0xRRGGBB, -1 unspecified, -2 transparent */
    int8_t display;                 /* -1, DS_* */
    int8_t bold, italic, underline, strike, pre, align, transform, listnone, hidden;
    int16_t font_px;                /* computed font size when specified, else 0 */
    int8_t family;                  /* -1 unspecified, 0 sans, 1 serif, 2 monospace */
    int16_t mt, mb, pl, pt, pb;     /* px or -1 */
    int8_t flex_item;               /* a child of a flex row: laid out inline */
    int8_t column;                  /* flex-direction: column / row-reverse ... */
};
enum { DS_NONE, DS_BLOCK, DS_INLINE, DS_INLINE_BLOCK, DS_FLEX, DS_LIST_ITEM, DS_TABLE, DS_CONTENTS };

struct dnode {
    int type;
    int caret;                      /* focused text control: caret as a byte offset into the value */
    char *defval;                   /* form reset: the value / checked state / selected option the page started with */
    int defstate;                   /* checked (0/1), selected option index; -1 = not recorded */
    struct estyle *st, *st_before, *st_after;
    char *before_txt, *after_txt;
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
    long long load_deadline;        /* dom clock ms while the page loads (0 afterwards) */
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
    struct css_state *css;
    struct dnode *focus;            /* focused form control */
    char *focus_orig;               /* its value when it got the focus (for the change event) */
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

/* CSS (dom_css.c) */
void dom_css_apply(struct dom *d);                  /* (re)load stylesheets if they changed and cascade them onto the tree */
void dom_css_free(struct dom *d);
bool css_open_outer(struct sbuf *sb, const struct estyle *st);      /* <x-o ...>: display, margins, background, alignment */
bool css_open_inner(struct sbuf *sb, const struct estyle *st, bool is_link);   /* <x-i ...>: colour, weight, size ... */
bool css_page_bg(struct dom *d, uint32_t *rgb, uint32_t *fg);
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
/* form controls (dom.c): what kind of widget an element is */
enum { WK_NONE, WK_TEXT, WK_PASSWORD, WK_CHECKBOX, WK_RADIO, WK_BUTTON, WK_SUBMIT, WK_RESET, WK_SELECT, WK_TEXTAREA };
int widget_kind(const struct dnode *n);
struct dnode *dom_form_of(const struct dnode *n);
void widget_text(const struct dnode *n, struct sbuf *out);
const struct dnode *dom_option_next(const struct dnode *sel, const struct dnode *cur);          /* current value / label */
struct dnode *dom_node_by_numeric_id(struct dom *d, int id);
void ser_full(struct sbuf *sb, const struct dnode *n, bool inner_only);
void dom_text_content(struct sbuf *sb, const struct dnode *n);
char *style_get(const struct dnode *n, const char *prop);
void style_set(struct dnode *n, const char *prop, const char *value);
bool node_clickable(const struct dnode *n);
bool dom_matches(const struct dnode *n, const char *selector, bool *valid);
bool dom_query(const struct dnode *root, const char *selector, bool first_only, struct dnode ***out, int *n);

enum { SC_NONE, SC_DESC, SC_CHILD, SC_ADJ, SC_SIB };

struct sel_attr { char *name, *val; int op; };       /* op: 0 exists, '=', '^', '$', '*', '~', '|' */

struct sel_simple {
    char *tag;                                       /* NULL or "*" = any */
    char *id;
    char **classes;
    int nclasses;
    struct sel_attr *attrs;
    int nattrs;
    int pseudo;                                      /* bit flags: 1 first-child, 2 last-child, 4 only-child, 8 empty, 16 root, 32 checked, 64 disabled */
    int nth;                                         /* :nth-child(n) (1-based, 0 = none), -1 odd, -2 even */
    struct sel_simple *not_;                         /* :not(simple) */
    int combinator;                                  /* relation to the previous (left) compound */
    int pseudo_elem;                                 /* 1 ::before, 2 ::after (only on the last compound) */
};

struct selector { struct sel_simple *parts; int n; };
struct selector_list { struct selector *sels; int n; };


void sel_free_simple(struct sel_simple *s);
void sel_list_free(struct selector_list *l);
bool sel_parse(const char *text, struct selector_list *out);
bool list_matches(const struct selector_list *l, const struct dnode *n);
bool sel_match_one(const struct selector *sel, const struct dnode *n);   /* one selector of a list */
int sel_specificity(const struct selector *sel);

#endif
