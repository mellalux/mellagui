/*
 * A small DOM for lumen-web, driven by the JavaScript interpreter (js.h).
 *
 * The page is parsed into a tree (dom_load), its <script>s run in document order, DOMContentLoaded / load fire, and the
 * tree is serialized back to markup (dom_serialize) for lumen-web's own HTML renderer. After timers or clicks the tree may
 * have changed (dom_take_dirty) and is serialized again. Elements that have click listeners (or an onclick attribute) are
 * rendered as links with the pseudo address "js:<id>"; dom_click() dispatches the event to them.
 *
 * Supported: document/element/text nodes, getElementById / querySelector(All) (tag, #id, .class, [attr], :first-child ...
 * with descendant/child/sibling combinators), createElement, appendChild/insertBefore/removeChild/..., innerHTML,
 * textContent, attributes, classList, dataset, style (display:none hides), events (addEventListener, onclick, bubbling,
 * CustomEvent, dispatchEvent), setTimeout/setInterval, location, history, localStorage, console, document.write.
 * Not supported: layout geometry (all sizes are 0), CSS, fetch/XMLHttpRequest, Promises, forms.
 */
#ifndef LUMEN_DOM_H
#define LUMEN_DOM_H

#include <stdbool.h>
#include <stddef.h>

struct dom;

struct dom_host {
    void *data;
    /* load a script: `src` as written in the page; false + message on failure */
    bool (*fetch_script)(void *data, const char *src, char **body, size_t *len, char *err, size_t errn);
    /* script asked to go to another page (location.href = ..., location.assign/replace, history.back/forward) */
    void (*navigate)(void *data, const char *url, int history_delta);
    void (*message)(void *data, const char *level, const char *text);   /* console output and script errors */
    void (*alert)(void *data, const char *text);
};

struct dom *dom_create(const char *url, const struct dom_host *host);
void dom_free(struct dom *d);

/* Parse `html`, run the scripts, fire DOMContentLoaded and load. */
void dom_load(struct dom *d, const char *html, size_t len);

/* Markup of the current tree for the renderer (malloc'ed, NUL terminated). */
char *dom_serialize(struct dom *d, size_t *len);
const char *dom_title(struct dom *d);

/* True once after the tree (or the title) changed. */
bool dom_take_dirty(struct dom *d);

/* Timers: milliseconds until the next one is due (-1 none), and run the due ones (true if the tree changed). */
long long dom_next_timer_ms(struct dom *d);
bool dom_run_timers(struct dom *d);
/* For --dump and tests: run timers in order without waiting, up to `max_ms` of virtual time. */
void dom_run_timers_virtual(struct dom *d, long long max_ms);

/* A rendered "js:<id>" link was clicked: run the listeners. Returns true if the browser should follow `href`
 * (the element is an <a href> and no listener called preventDefault); `href` is then the link target as written. */
bool dom_click(struct dom *d, int id, char *href, size_t hrefn);

#endif
