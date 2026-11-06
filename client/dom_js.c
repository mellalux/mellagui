/* DOM JavaScript bindings and the page lifecycle (scripts, events, timers). See dom.h. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "dom_int.h"

#define DNATIVE(name) static jsval name(js *J __attribute__((unused)), jsval thisv __attribute__((unused)), \
                                        int argc __attribute__((unused)), jsval *argv __attribute__((unused)), \
                                        jsobj *callee __attribute__((unused)))
#define ARG(i) ((i) < argc ? argv[i] : JS_UNDEFINED)
#define SCRIPT_BUDGET 25000000ULL           /* instruction budget of one entry point (script, timer, event) */
#define PAGE_BUDGET 150000000ULL            /* ... and of the whole page; after that its scripts stop running */

static jsval wrap(struct dom *d, struct dnode *n);
static void flush_scripts(struct dom *d);
static bool run_allowed(struct dom *d);

/* ---- host classes --------------------------------------------------------------------------------------------- */

static bool node_get(js *J, jsobj *o, jsstr *key, jsval *out);
static bool node_set(js *J, jsobj *o, jsstr *key, jsval v);
static void node_keys(js *J, jsobj *o, jsobj *arr);
static const struct js_hostclass node_class = { "HTMLElement", node_get, node_set, node_keys };

static bool style_get_hook(js *J, jsobj *o, jsstr *key, jsval *out);
static bool style_set_hook(js *J, jsobj *o, jsstr *key, jsval v);
static const struct js_hostclass style_class = { "CSSStyleDeclaration", style_get_hook, style_set_hook, NULL };

static bool cl_get(js *J, jsobj *o, jsstr *key, jsval *out);
static const struct js_hostclass classlist_class = { "DOMTokenList", cl_get, NULL, NULL };

static bool ds_get(js *J, jsobj *o, jsstr *key, jsval *out);
static bool ds_set(js *J, jsobj *o, jsstr *key, jsval v);
static void ds_keys(js *J, jsobj *o, jsobj *arr);
static const struct js_hostclass dataset_class = { "DOMStringMap", ds_get, ds_set, ds_keys };

static bool loc_get(js *J, jsobj *o, jsstr *key, jsval *out);
static bool loc_set(js *J, jsobj *o, jsstr *key, jsval v);
static const struct js_hostclass location_class = { "Location", loc_get, loc_set, NULL };

static struct dnode *unwrap(jsval v) { return js_host_ptr(v, &node_class); }

static struct dnode *this_node(js *J, jsval thisv)
{
    struct dnode *n = unwrap(thisv);
    if (!n)
        js_throw_error(J, "TypeError", "Illegal invocation (not a DOM node)");
    return n;
}

static jsval wrap(struct dom *d, struct dnode *n)
{
    if (!n)
        return JS_NULLV;
    if (!n->wrapper)
        n->wrapper = js_host_new(d->J, &node_class, n, d->node_proto);
    return js_objv(n->wrapper);
}

static jsval jstr(js *J, const char *z) { return js_cstr(J, z ? z : ""); }

static const char *sarg(js *J, jsval v)
{
    size_t l;
    const char *s = js_tostr(J, v, &l);
    return s ? s : "";
}

static void upper(char *s) { for (; *s; s++) *s = (char)toupper((unsigned char)*s); }

static jsval nodes_to_array(struct dom *d, struct dnode **list, int n)
{
    jsobj *a = js_array_new(d->J);
    for (int i = 0; i < n; i++)
        js_array_push(d->J, a, wrap(d, list[i]));
    return js_objv(a);
}

static struct dnode *next_el(struct dnode *n) { for (n = n->next; n && n->type != DN_ELEMENT; n = n->next); return n; }
static struct dnode *prev_el(struct dnode *n) { for (n = n->prev; n && n->type != DN_ELEMENT; n = n->prev); return n; }

static void kebab(const char *camel, char *out, size_t n)
{
    size_t o = 0;
    for (const char *p = camel; *p && o + 2 < n; p++) {
        if (isupper((unsigned char)*p)) {
            out[o++] = '-';
            out[o++] = (char)tolower((unsigned char)*p);
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

static void camel(const char *kebab_, char *out, size_t n)
{
    size_t o = 0;
    for (const char *p = kebab_; *p && o + 1 < n; p++) {
        if (*p == '-' && p[1]) {
            out[o++] = (char)toupper((unsigned char)p[1]);
            p++;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

/* ---- events ----------------------------------------------------------------------------------------------------- */

/* Give the next script / handler its own budget; false once the page has used up its total or its memory. */
static bool run_allowed(struct dom *d)
{
    d->steps_total += js_steps_used(d->J);
    if (js_memory_exhausted(d->J) || d->steps_total > PAGE_BUDGET) {
        if (!d->budget_reported) {
            d->budget_reported = true;
            d->host.message ? d->host.message(d->host.data, "error", "this page used up its JavaScript budget; its scripts are stopped")
                            : (void)0;
        }
        return false;
    }
    js_set_budget(d->J, SCRIPT_BUDGET);
    return true;
}

static void report(struct dom *d, const char *level, const char *msg)
{
    if (d->host.message)
        d->host.message(d->host.data, level, msg);
    else
        fprintf(stderr, "[js %s] %s\n", level, msg);
}

static void report_exception(struct dom *d)
{
    js *J = d->J;
    jsval ex = js_exception(J);
    char b[500];
    js_describe_exception(J, ex, b, sizeof(b));
    report(d, "error", b);
}

static struct listener *add_listener(struct listener **head, const char *type, jsval fn, bool is_prop, bool once)
{
    struct listener *l;
    if (is_prop) {
        for (l = *head; l; l = l->next)
            if (l->is_prop && !strcmp(l->type, type)) {
                l->fn = fn;
                return l;
            }
    } else {
        for (l = *head; l; l = l->next)
            if (!l->is_prop && !strcmp(l->type, type) && js_strict_equals(l->fn, fn))
                return l;                                   /* adding the same listener twice is a no-op */
    }
    l = calloc(1, sizeof(*l));
    if (!l)
        exit(1);
    l->type = dstrdup(type);
    l->fn = fn;
    l->is_prop = is_prop;
    l->once = once;
    struct listener **p = head;
    while (*p)
        p = &(*p)->next;
    *p = l;
    return l;
}

static void remove_listener(struct listener **head, const char *type, jsval fn)
{
    for (struct listener **p = head; *p; p = &(*p)->next) {
        struct listener *l = *p;
        if (!l->is_prop && !strcmp(l->type, type) && js_strict_equals(l->fn, fn)) {
            *p = l->next;
            free(l->type);
            free(l);
            return;
        }
    }
}

static jsval make_event(struct dom *d, const char *type, bool bubbles, bool cancelable, jsval detail)
{
    js *J = d->J;
    jsobj *e = js_object_new(J);
    js_set_proto(e, d->event_proto);
    jsval ev = js_objv(e);
    js_set(J, ev, "type", jstr(J, type));
    js_set(J, ev, "bubbles", js_bool(bubbles));
    js_set(J, ev, "cancelable", js_bool(cancelable));
    js_set(J, ev, "defaultPrevented", js_bool(false));
    js_set(J, ev, "target", JS_NULLV);
    js_set(J, ev, "currentTarget", JS_NULLV);
    js_set(J, ev, "eventPhase", js_num(0));
    js_set(J, ev, "isTrusted", js_bool(false));
    js_set(J, ev, "timeStamp", js_num((double)clock() * 1000.0 / CLOCKS_PER_SEC));
    if (detail.t != JS_UNDEF)
        js_set(J, ev, "detail", detail);
    return ev;
}

/* Compile `onclick="..."` style attributes on first use. */
static void ensure_inline_handler(struct dom *d, struct dnode *n, const char *type)
{
    char attr[48];
    snprintf(attr, sizeof(attr), "on%s", type);
    const char *code = dattr_get(n, attr);
    if (!code)
        return;
    for (struct listener *l = n->listeners; l; l = l->next)
        if (l->is_prop && !strcmp(l->type, type))
            return;
    struct sbuf sb = { 0 };
    sbuf_addz(&sb, "(function(event) {");
    sbuf_addz(&sb, code);
    sbuf_addz(&sb, "\n})");
    jsval fn;
    char err[300];
    if (js_eval_value(d->J, sb.p, sb.n, &fn, err, sizeof(err)))
        add_listener(&n->listeners, type, fn, true, false);
    else
        report(d, "error", err);
    free(sb.p);
}

/* Run the listeners of `type` on one target; returns false when stopImmediatePropagation was called. A listener may add or
 * remove listeners while it runs, so the list is copied first (never touch a listener struct after calling it). */
static bool run_listeners(struct dom *d, struct listener **headp, const char *type, jsval this_obj, jsval ev)
{
    js *J = d->J;
    struct snap { jsval fn; bool is_prop, once; } snap[64];
    int n = 0;
    for (struct listener *l = *headp; l && n < 64; l = l->next)
        if (!strcmp(l->type, type)) {
            snap[n].fn = l->fn;
            snap[n].is_prop = l->is_prop;
            snap[n].once = l->once;
            n++;
        }
    for (int i = 0; i < n; i++) {
        jsval fn = snap[i].fn;
        if (snap[i].once && !snap[i].is_prop)
            remove_listener(headp, type, fn);                /* `once` listeners go before they run */
        if (!js_is_function(fn))
            continue;
        js_set(J, ev, "currentTarget", this_obj);
        bool threw;
        jsval r = js_call(J, fn, this_obj, 1, &ev, &threw);
        if (threw) {
            report_exception(d);
        } else if (snap[i].is_prop && r.t == JS_BOOL && !r.u.b) {
            if (js_truthy(js_get(J, ev, "cancelable")))
                js_set(J, ev, "defaultPrevented", js_bool(true));
        }
        if (js_truthy(js_get(J, ev, "__stopImmediate")))
            return false;
    }
    return true;
}

static void dispatch_on_window(struct dom *d, jsval ev, const char *type)
{
    jsval win = js_objv(js_global(d->J));
    js_set(d->J, ev, "target", win);
    run_listeners(d, &d->win_listeners, type, win, ev);
}

/* Dispatch to `target` (a node) and bubble to its ancestors, the document and the window. */
static bool dispatch_node_event(struct dom *d, struct dnode *target, jsval ev)
{
    js *J = d->J;
    const char *type = sarg(J, js_get(J, ev, "type"));
    bool bubbles = js_truthy(js_get(J, ev, "bubbles"));
    js_set(J, ev, "target", wrap(d, target));
    js_set(J, ev, "eventPhase", js_num(2));
    for (struct dnode *n = target; n; n = n->parent) {
        ensure_inline_handler(d, n, type);
        if (J && js_thrown(J))
            report_exception(d);
        run_listeners(d, &n->listeners, type, wrap(d, n), ev);
        if (n != target)
            js_set(J, ev, "eventPhase", js_num(3));
        if (!bubbles || js_truthy(js_get(J, ev, "cancelBubble")) || js_truthy(js_get(J, ev, "__stopImmediate")))
            goto done;
    }
    if (!js_truthy(js_get(J, ev, "cancelBubble"))) {
        jsval win = js_objv(js_global(J));
        run_listeners(d, &d->win_listeners, type, win, ev);
    }
done:
    js_set(J, ev, "eventPhase", js_num(0));
    return !js_truthy(js_get(J, ev, "defaultPrevented"));
}

DNATIVE(ev_preventDefault)
{
    if (thisv.t == JS_OBJ && js_truthy(js_get(J, thisv, "cancelable")))
        js_set(J, thisv, "defaultPrevented", js_bool(true));
    return JS_UNDEFINED;
}
DNATIVE(ev_stopPropagation) { if (thisv.t == JS_OBJ) js_set(J, thisv, "cancelBubble", js_bool(true)); return JS_UNDEFINED; }
DNATIVE(ev_stopImmediate)
{
    if (thisv.t == JS_OBJ) {
        js_set(J, thisv, "cancelBubble", js_bool(true));
        js_set(J, thisv, "__stopImmediate", js_bool(true));
    }
    return JS_UNDEFINED;
}

static jsval event_ctor_impl(js *J, jsval thisv, int argc, jsval *argv, jsobj *callee, bool custom)
{
    struct dom *d = js_fn_data(callee);
    const char *type = sarg(J, ARG(0));
    jsval opts = ARG(1);
    bool bubbles = false, cancelable = false;
    jsval detail = JS_UNDEFINED;
    if (opts.t == JS_OBJ) {
        bubbles = js_truthy(js_get(J, opts, "bubbles"));
        cancelable = js_truthy(js_get(J, opts, "cancelable"));
        if (custom)
            detail = js_get(J, opts, "detail");
    }
    jsval ev = make_event(d, type, bubbles, cancelable, custom && detail.t == JS_UNDEF ? JS_NULLV : detail);
    (void)thisv;
    return ev;
}
DNATIVE(Event_ctor) { return event_ctor_impl(J, thisv, argc, argv, callee, false); }
DNATIVE(CustomEvent_ctor) { return event_ctor_impl(J, thisv, argc, argv, callee, true); }

static struct listener **listeners_of(struct dom *d, jsval thisv, struct dnode **node_out)
{
    struct dnode *n = unwrap(thisv);
    if (node_out)
        *node_out = n;
    if (n)
        return &n->listeners;
    return &d->win_listeners;                               /* window (the global object) */
}

DNATIVE(addEventListener)
{
    struct dom *d = js_fn_data(callee);
    const char *type = sarg(J, ARG(0));
    if (!js_is_function(ARG(1)))
        return JS_UNDEFINED;
    bool once = ARG(2).t == JS_OBJ && js_truthy(js_get(J, ARG(2), "once"));
    add_listener(listeners_of(d, thisv, NULL), type, ARG(1), false, once);
    d->dirty = true;                                        /* clickable elements are rendered differently */
    return JS_UNDEFINED;
}
DNATIVE(removeEventListener)
{
    struct dom *d = js_fn_data(callee);
    remove_listener(listeners_of(d, thisv, NULL), sarg(J, ARG(0)), ARG(1));
    d->dirty = true;
    return JS_UNDEFINED;
}
DNATIVE(dispatchEvent)
{
    struct dom *d = js_fn_data(callee);
    jsval ev = ARG(0);
    if (ev.t != JS_OBJ)
        return js_throw_error(J, "TypeError", "dispatchEvent needs an Event");
    struct dnode *n = unwrap(thisv);
    bool ok;
    if (n) {
        ok = dispatch_node_event(d, n, ev);
    } else {
        const char *type = sarg(J, js_get(J, ev, "type"));
        dispatch_on_window(d, ev, type);
        ok = !js_truthy(js_get(J, ev, "defaultPrevented"));
    }
    flush_scripts(d);
    return js_bool(ok);
}

DNATIVE(el_click)
{
    struct dnode *n = this_node(J, thisv);
    if (!n)
        return JS_UNDEFINED;
    struct dom *d = n->dom;
    jsval ev = make_event(d, "click", true, true, JS_UNDEFINED);
    dispatch_node_event(d, n, ev);
    flush_scripts(d);
    return JS_UNDEFINED;
}

/* ---- node properties --------------------------------------------------------------------------------------------- */

static bool is_reflected(const char *k)
{
    static const char *const list[] = { "href", "src", "type", "name", "title", "alt", "placeholder", "rel", "target",
                                        "action", "method", "lang", "dir", "role", "content", "charset", "width", "height",
                                        "media", "htmlFor", NULL };
    for (int i = 0; list[i]; i++)
        if (!strcmp(list[i], k))
            return true;
    return false;
}

static bool is_boolean_attr(const char *k)
{
    return !strcmp(k, "checked") || !strcmp(k, "disabled") || !strcmp(k, "hidden") || !strcmp(k, "selected") ||
           !strcmp(k, "readOnly") || !strcmp(k, "required") || !strcmp(k, "multiple") || !strcmp(k, "autofocus") ||
           !strcmp(k, "async") || !strcmp(k, "defer");
}

static void children_of(struct dnode *n, bool only_elements, struct dnode ***out, int *cnt)
{
    int c = 0;
    for (struct dnode *k = n->first; k; k = k->next)
        if (!only_elements || k->type == DN_ELEMENT)
            c++;
    *out = malloc(sizeof(**out) * (size_t)(c + 1));
    *cnt = 0;
    for (struct dnode *k = n->first; k; k = k->next)
        if (!only_elements || k->type == DN_ELEMENT)
            (*out)[(*cnt)++] = k;
}

static jsval children_array(struct dom *d, struct dnode *n, bool only_elements)
{
    struct dnode **l;
    int c;
    children_of(n, only_elements, &l, &c);
    jsval r = nodes_to_array(d, l, c);
    free(l);
    return r;
}

static jsval make_sub(struct dom *d, const struct js_hostclass *hc, struct dnode *n, jsobj *proto)
{
    return js_objv(js_host_new(d->J, hc, n, proto));
}

static void set_text_content(struct dom *d, struct dnode *n, const char *text)
{
    if (n->type == DN_TEXT || n->type == DN_COMMENT) {
        free(n->text);
        n->text = dstrdup(text);
        d->dirty = true;
        return;
    }
    while (n->first)
        dnode_detach(n->first);
    if (*text)
        dnode_insert(n, dtext_new(d, text), NULL);
    d->dirty = true;
}

static void set_inner_html(struct dom *d, struct dnode *n, const char *html)
{
    while (n->first)
        dnode_detach(n->first);
    struct dnode *frag = dnode_new(d, DN_FRAGMENT, NULL);
    dom_parse_into(d, frag, html, strlen(html));
    /* scripts inserted through innerHTML never run */
    for (struct dnode *c = frag->first; c; c = c->next)
        if (c->type == DN_ELEMENT && !strcmp(c->tag, "script"))
            c->script_done = true;
    dnode_insert(n, frag, NULL);
    d->dirty = true;
}

/* html / head / body of any document node (the main one or one made by document.implementation) */
static struct dnode *doc_part(struct dnode *doc, const char *tag)
{
    struct dnode *html = NULL;
    for (struct dnode *c = doc->first; c && !html; c = c->next)
        if (c->type == DN_ELEMENT)
            html = c;
    if (!html)
        return NULL;
    if (!strcmp(tag, "html"))
        return html;
    for (struct dnode *c = html->first; c; c = c->next)
        if (c->type == DN_ELEMENT && !strcmp(c->tag, tag))
            return c;
    return NULL;
}

static jsval get_title(struct dom *d)
{
    return jstr(d->J, dom_title(d));
}

static void set_title(struct dom *d, const char *t)
{
    struct dnode *el = dom_find_tag(d->head, "title");
    if (!el) {
        el = dnode_new(d, DN_ELEMENT, "title");
        dnode_insert(d->head, el, NULL);
    }
    set_text_content(d, el, t);
}

static bool node_get(js *J, jsobj *o, jsstr *key, jsval *out)
{
    struct dnode *n = js_host_data(o);
    struct dom *d = n->dom;
    const char *k = key->s;
    switch (k[0]) {
    case 'a':
        if (!strcmp(k, "attributes")) {
            jsobj *arr = js_array_new(J);
            for (int i = 0; i < n->nattr; i++) {
                jsobj *a = js_object_new(J);
                js_set(J, js_objv(a), "name", jstr(J, n->attrs[i].name));
                js_set(J, js_objv(a), "value", jstr(J, n->attrs[i].value));
                js_array_push(J, arr, js_objv(a));
            }
            *out = js_objv(arr);
            return true;
        }
        if (!strcmp(k, "activeElement")) { *out = wrap(d, d->body); return true; }
        break;
    case 'b':
        if (!strcmp(k, "body") && n->type == DN_DOCUMENT) { *out = wrap(d, doc_part(n, "body")); return true; }
        break;
    case 'c':
        if (!strcmp(k, "className")) { *out = jstr(J, dattr_get(n, "class")); return true; }
        if (!strcmp(k, "classList")) { *out = make_sub(d, &classlist_class, n, d->classlist_proto); return true; }
        if (!strcmp(k, "childNodes")) { *out = children_array(d, n, false); return true; }
        if (!strcmp(k, "children")) { *out = children_array(d, n, true); return true; }
        if (!strcmp(k, "childElementCount")) {
            int c = 0;
            for (struct dnode *x = n->first; x; x = x->next) c += x->type == DN_ELEMENT;
            *out = js_num(c);
            return true;
        }
        if (!strcmp(k, "currentScript")) { *out = wrap(d, d->current_script); return true; }
        if (!strcmp(k, "cookie") && n->type == DN_DOCUMENT) { *out = jstr(J, d->cookie); return true; }
        if (!strcmp(k, "characterSet") && n->type == DN_DOCUMENT) { *out = jstr(J, "UTF-8"); return true; }
        if (!strcmp(k, "compatMode") && n->type == DN_DOCUMENT) { *out = jstr(J, "CSS1Compat"); return true; }
        if (!strcmp(k, "contentType") && n->type == DN_DOCUMENT) { *out = jstr(J, "text/html"); return true; }
        if (!strncmp(k, "client", 6) && (!strcmp(k + 6, "Width") || !strcmp(k + 6, "Height") || !strcmp(k + 6, "Top") ||
                                          !strcmp(k + 6, "Left"))) { *out = js_num(0); return true; }
        if (!strcmp(k, "checked")) { *out = js_bool(dattr_get(n, "checked") != NULL); return true; }
        break;
    case 'd':
        if (!strcmp(k, "dataset")) { *out = make_sub(d, &dataset_class, n, NULL); return true; }
        if (!strcmp(k, "documentElement")) { *out = wrap(d, doc_part(n->type == DN_DOCUMENT ? n : d->doc, "html")); return true; }
        if (!strcmp(k, "defaultView")) { *out = js_objv(js_global(J)); return true; }
        if (!strcmp(k, "data") && (n->type == DN_TEXT || n->type == DN_COMMENT)) { *out = jstr(J, n->text); return true; }
        if (!strcmp(k, "doctype")) { *out = JS_NULLV; return true; }
        if (!strcmp(k, "domain")) { *out = jstr(J, "localhost"); return true; }
        break;
    case 'f':
        if (!strcmp(k, "firstChild")) { *out = wrap(d, n->first); return true; }
        if (!strcmp(k, "firstElementChild")) {
            struct dnode *c = n->first;
            while (c && c->type != DN_ELEMENT) c = c->next;
            *out = wrap(d, c);
            return true;
        }
        if (!strcmp(k, "forms") && n->type == DN_DOCUMENT) {
            struct dnode **l; int c;
            dom_query(n, "form", false, &l, &c);
            *out = nodes_to_array(d, l, c);
            free(l);
            return true;
        }
        break;
    case 'h':
        if (!strcmp(k, "head") && n->type == DN_DOCUMENT) { *out = wrap(d, doc_part(n, "head")); return true; }
        if (!strcmp(k, "hidden")) { *out = js_bool(dattr_get(n, "hidden") != NULL); return true; }
        break;
    case 'i':
        if (!strcmp(k, "id")) { *out = jstr(J, dattr_get(n, "id")); return true; }
        if (!strcmp(k, "innerHTML")) {
            struct sbuf sb = { 0 };
            ser_full(&sb, n, true);
            *out = jstr(J, sb.p);
            free(sb.p);
            return true;
        }
        if (!strcmp(k, "innerText")) goto text_content;
        if (!strcmp(k, "isConnected")) { *out = js_bool(dnode_contains(d->doc, n)); return true; }
        if (!strcmp(k, "images") && n->type == DN_DOCUMENT) {
            struct dnode **l; int c;
            dom_query(n, "img", false, &l, &c);
            *out = nodes_to_array(d, l, c);
            free(l);
            return true;
        }
        break;
    case 'l':
        if (!strcmp(k, "lastChild")) { *out = wrap(d, n->last); return true; }
        if (!strcmp(k, "lastElementChild")) {
            struct dnode *c = n->last;
            while (c && c->type != DN_ELEMENT) c = c->prev;
            *out = wrap(d, c);
            return true;
        }
        if (!strcmp(k, "localName")) { *out = jstr(J, n->tag); return true; }
        if (!strcmp(k, "location") && n->type == DN_DOCUMENT) { *out = js_objv(d->location_obj); return true; }
        if (!strcmp(k, "links") && n->type == DN_DOCUMENT) {
            struct dnode **l; int c;
            dom_query(n, "a[href]", false, &l, &c);
            *out = nodes_to_array(d, l, c);
            free(l);
            return true;
        }
        break;
    case 'n':
        if (!strcmp(k, "nodeType")) { *out = js_num(n->type); return true; }
        if (!strcmp(k, "nodeName")) {
            char b[64];
            if (n->type == DN_ELEMENT) { snprintf(b, sizeof(b), "%s", n->tag); upper(b); }
            else snprintf(b, sizeof(b), "%s", n->type == DN_TEXT ? "#text" : n->type == DN_COMMENT ? "#comment" : n->type == DN_DOCUMENT ? "#document" : "#document-fragment");
            *out = jstr(J, b);
            return true;
        }
        if (!strcmp(k, "nodeValue")) { *out = n->text ? jstr(J, n->text) : JS_NULLV; return true; }
        if (!strcmp(k, "nextSibling")) { *out = wrap(d, n->next); return true; }
        if (!strcmp(k, "nextElementSibling")) { *out = wrap(d, next_el(n)); return true; }
        if (!strcmp(k, "name") && n->type == DN_ELEMENT) { *out = jstr(J, dattr_get(n, "name")); return true; }
        break;
    case 'o':
        if (!strcmp(k, "outerHTML")) {
            struct sbuf sb = { 0 };
            ser_full(&sb, n, false);
            *out = jstr(J, sb.p);
            free(sb.p);
            return true;
        }
        if (!strcmp(k, "ownerDocument")) { *out = n->type == DN_DOCUMENT ? JS_NULLV : wrap(d, d->doc); return true; }
        if (!strcmp(k, "offsetHeight")) { *out = js_num(0); return true; }
        if (!strncmp(k, "offset", 6) && (!strcmp(k + 6, "Width") || !strcmp(k + 6, "Height") || !strcmp(k + 6, "Top") ||
                                          !strcmp(k + 6, "Left"))) { *out = js_num(0); return true; }
        if (!strcmp(k, "offsetParent")) { *out = JS_NULLV; return true; }
        if (k[1] == 'n' && n->type != DN_TEXT) {                           /* onclick etc. */
            for (struct listener *l = n->listeners; l; l = l->next)
                if (l->is_prop && !strcmp(l->type, k + 2)) { *out = l->fn; return true; }
            if (dattr_get(n, k)) {
                ensure_inline_handler(d, n, k + 2);
                for (struct listener *l = n->listeners; l; l = l->next)
                    if (l->is_prop && !strcmp(l->type, k + 2)) { *out = l->fn; return true; }
            }
            if (!strncmp(k, "on", 2) && !strchr(k, '_') && strlen(k) > 3 && islower((unsigned char)k[2])) {
                *out = JS_NULLV;
                return true;
            }
        }
        break;
    case 'p':
        if (!strcmp(k, "parentNode")) { *out = wrap(d, n->parent); return true; }
        if (!strcmp(k, "parentElement")) { *out = n->parent && n->parent->type == DN_ELEMENT ? wrap(d, n->parent) : JS_NULLV; return true; }
        if (!strcmp(k, "previousSibling")) { *out = wrap(d, n->prev); return true; }
        if (!strcmp(k, "previousElementSibling")) { *out = wrap(d, prev_el(n)); return true; }
        break;
    case 'r':
        if (!strcmp(k, "readyState") && n->type == DN_DOCUMENT) {
            *out = jstr(J, d->ready == 0 ? "loading" : d->ready == 1 ? "interactive" : "complete");
            return true;
        }
        if (!strcmp(k, "referrer") && n->type == DN_DOCUMENT) { *out = jstr(J, ""); return true; }
        break;
    case 's':
        if (!strcmp(k, "style") && n->type == DN_ELEMENT) { *out = make_sub(d, &style_class, n, d->style_proto); return true; }
        if (!strncmp(k, "scroll", 6) && (!strcmp(k + 6, "Width") || !strcmp(k + 6, "Height") || !strcmp(k + 6, "Top") ||
                                          !strcmp(k + 6, "Left"))) { *out = js_num(0); return true; }
        if (!strcmp(k, "selected")) { *out = js_bool(dattr_get(n, "selected") != NULL); return true; }
        break;
    case 't':
        if (!strcmp(k, "tagName")) {
            if (n->type != DN_ELEMENT) { *out = JS_UNDEFINED; return true; }
            char b[64];
            snprintf(b, sizeof(b), "%s", n->tag);
            upper(b);
            *out = jstr(J, b);
            return true;
        }
        if (!strcmp(k, "textContent")) {
        text_content:;
            if (n->type == DN_DOCUMENT) { *out = JS_NULLV; return true; }
            struct sbuf sb = { 0 };
            dom_text_content(&sb, n);
            *out = jstr(J, sb.p);
            free(sb.p);
            return true;
        }
        if (!strcmp(k, "title") && n->type == DN_DOCUMENT) { *out = get_title(d); return true; }
        break;
    case 'v':
        if (!strcmp(k, "value") && n->type == DN_ELEMENT) {
            if (!strcmp(n->tag, "textarea")) {
                struct sbuf sb = { 0 };
                dom_text_content(&sb, n);
                *out = jstr(J, sb.p);
                free(sb.p);
            } else {
                *out = jstr(J, dattr_get(n, "value"));
            }
            return true;
        }
        break;
    case 'U':
        if (!strcmp(k, "URL") && n->type == DN_DOCUMENT) { *out = jstr(J, d->url); return true; }
        break;
    }
    if (n->type == DN_ELEMENT) {
        if (!strcmp(k, "disabled") || !strcmp(k, "readOnly") || !strcmp(k, "required") || !strcmp(k, "multiple") ||
            !strcmp(k, "autofocus") || !strcmp(k, "async") || !strcmp(k, "defer")) {
            char lower[32];
            snprintf(lower, sizeof(lower), "%s", k);
            for (char *p = lower; *p; p++) *p = (char)tolower((unsigned char)*p);
            *out = js_bool(dattr_get(n, lower) != NULL);
            return true;
        }
        if (!strcmp(k, "htmlFor")) { *out = jstr(J, dattr_get(n, "for")); return true; }
        if (is_reflected(k)) {
            const char *v = dattr_get(n, k);
            *out = jstr(J, v);
            return true;
        }
    }
    return false;
}

static bool node_set(js *J, jsobj *o, jsstr *key, jsval v)
{
    struct dnode *n = js_host_data(o);
    struct dom *d = n->dom;
    const char *k = key->s;
    if (!strcmp(k, "textContent") || !strcmp(k, "innerText")) {
        if (n->type == DN_DOCUMENT)
            return true;
        set_text_content(d, n, v.t == JS_NULL ? "" : sarg(J, v));
        return true;
    }
    if (!strcmp(k, "innerHTML") && n->type == DN_ELEMENT) {
        set_inner_html(d, n, sarg(J, v));
        return true;
    }
    if (!strcmp(k, "outerHTML") && n->type == DN_ELEMENT && n->parent) {
        struct dnode *frag = dnode_new(d, DN_FRAGMENT, NULL);
        const char *html = sarg(J, v);
        dom_parse_into(d, frag, html, strlen(html));
        dnode_insert(n->parent, frag, n);
        dnode_detach(n);
        return true;
    }
    if ((!strcmp(k, "nodeValue") || !strcmp(k, "data")) && (n->type == DN_TEXT || n->type == DN_COMMENT)) {
        set_text_content(d, n, sarg(J, v));
        return true;
    }
    if (n->type == DN_DOCUMENT) {
        if (!strcmp(k, "title")) { set_title(d, sarg(J, v)); return true; }
        if (!strcmp(k, "cookie")) {
            const char *c = sarg(J, v);
            size_t l = strcspn(c, ";");                       /* keep just "name=value" pairs, no expiry handling */
            if (strlen(d->cookie) + l + 3 < sizeof(d->cookie)) {
                if (d->cookie[0]) strcat(d->cookie, "; ");
                strncat(d->cookie, c, l);
            }
            return true;
        }
        if (!strcmp(k, "location")) {
            if (d->host.navigate)
                d->host.navigate(d->host.data, sarg(J, v), 0);
            return true;
        }
        return false;
    }
    if (n->type != DN_ELEMENT)
        return false;
    if (!strcmp(k, "id")) { dattr_set(n, "id", sarg(J, v)); return true; }
    if (!strcmp(k, "className")) { dattr_set(n, "class", sarg(J, v)); return true; }
    if (!strcmp(k, "htmlFor")) { dattr_set(n, "for", sarg(J, v)); return true; }
    if (!strcmp(k, "style")) { dattr_set(n, "style", sarg(J, v)); return true; }
    if (!strcmp(k, "value")) {
        if (!strcmp(n->tag, "textarea")) set_text_content(d, n, sarg(J, v));
        else dattr_set(n, "value", sarg(J, v));
        return true;
    }
    if (is_boolean_attr(k)) {
        char lower[32];
        snprintf(lower, sizeof(lower), "%s", k);
        for (char *p = lower; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (js_truthy(v)) dattr_set(n, lower, "");
        else dattr_remove(n, lower);
        return true;
    }
    if (is_reflected(k)) { dattr_set(n, k, sarg(J, v)); return true; }
    if (k[0] == 'o' && k[1] == 'n' && strlen(k) > 3) {                      /* element.onclick = fn */
        if (js_is_function(v)) {
            add_listener(&n->listeners, k + 2, v, true, false);
        } else {
            for (struct listener **p = &n->listeners; *p; p = &(*p)->next)
                if ((*p)->is_prop && !strcmp((*p)->type, k + 2)) {
                    struct listener *l = *p;
                    *p = l->next;
                    free(l->type);
                    free(l);
                    break;
                }
            dattr_remove(n, k);
        }
        d->dirty = true;
        return true;
    }
    return false;
}

static void node_keys(js *J, jsobj *o, jsobj *arr)
{
    (void)J; (void)o; (void)arr;
}

/* ---- style / classList / dataset / location ------------------------------------------------------------------ */

static bool style_get_hook(js *J, jsobj *o, jsstr *key, jsval *out)
{
    struct dnode *n = js_host_data(o);
    const char *k = key->s;
    if (!strcmp(k, "getPropertyValue") || !strcmp(k, "setProperty") || !strcmp(k, "removeProperty") || !strcmp(k, "item") ||
        !strcmp(k, "constructor") || !strcmp(k, "toString"))
        return false;
    if (!strcmp(k, "cssText")) { *out = jstr(J, dattr_get(n, "style")); return true; }
    if (!strcmp(k, "length")) {
        const char *st = dattr_get(n, "style");
        int c = 0;
        for (const char *p = st ? st : ""; *p; p++) c += *p == ':';
        *out = js_num(c);
        return true;
    }
    char kb[128];
    kebab(k, kb, sizeof(kb));
    char *v = style_get(n, kb);
    *out = jstr(J, v);
    free(v);
    return true;
}

static bool style_set_hook(js *J, jsobj *o, jsstr *key, jsval v)
{
    struct dnode *n = js_host_data(o);
    const char *k = key->s;
    if (!strcmp(k, "cssText")) {
        dattr_set(n, "style", sarg(J, v));
        return true;
    }
    char kb[128];
    kebab(k, kb, sizeof(kb));
    style_set(n, kb, v.t == JS_NULL ? "" : sarg(J, v));
    return true;
}

DNATIVE(style_getPropertyValue)
{
    struct dnode *n = js_host_ptr(thisv, &style_class);
    if (!n) return jstr(J, "");
    char *v = style_get(n, sarg(J, ARG(0)));
    jsval r = jstr(J, v);
    free(v);
    return r;
}
DNATIVE(style_setProperty)
{
    struct dnode *n = js_host_ptr(thisv, &style_class);
    if (n) style_set(n, sarg(J, ARG(0)), sarg(J, ARG(1)));
    return JS_UNDEFINED;
}
DNATIVE(style_removeProperty)
{
    struct dnode *n = js_host_ptr(thisv, &style_class);
    if (!n) return jstr(J, "");
    char *v = style_get(n, sarg(J, ARG(0)));
    jsval r = jstr(J, v);
    free(v);
    style_set(n, sarg(J, ARG(0)), "");
    return r;
}

/* class attribute as a token list */
static bool cl_has(const char *cls, const char *tok)
{
    size_t tl = strlen(tok);
    for (const char *p = cls ? cls : ""; *p;) {
        while (isspace((unsigned char)*p)) p++;
        const char *s = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if ((size_t)(p - s) == tl && !strncmp(s, tok, tl))
            return true;
    }
    return false;
}

static void cl_modify(struct dnode *n, const char *tok, int mode)           /* 1 add, 0 remove */
{
    const char *cls = dattr_get(n, "class");
    struct sbuf sb = { 0 };
    bool had = cl_has(cls, tok);
    for (const char *p = cls ? cls : ""; *p;) {
        while (isspace((unsigned char)*p)) p++;
        const char *s = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (p == s)
            break;
        if (!mode && (size_t)(p - s) == strlen(tok) && !strncmp(s, tok, strlen(tok)))
            continue;
        if (sb.n) sbuf_putc(&sb, ' ');
        sbuf_add(&sb, s, (size_t)(p - s));
    }
    if (mode && !had) {
        if (sb.n) sbuf_putc(&sb, ' ');
        sbuf_addz(&sb, tok);
    }
    if (mode || had)
        dattr_set(n, "class", sb.p ? sb.p : "");
    free(sb.p);
}

static bool cl_get(js *J, jsobj *o, jsstr *key, jsval *out)
{
    struct dnode *n = js_host_data(o);
    if (!strcmp(key->s, "length")) {
        int c = 0;
        for (const char *p = dattr_get(n, "class") ? dattr_get(n, "class") : ""; *p;) {
            while (isspace((unsigned char)*p)) p++;
            if (!*p) break;
            c++;
            while (*p && !isspace((unsigned char)*p)) p++;
        }
        *out = js_num(c);
        return true;
    }
    if (!strcmp(key->s, "value")) { *out = jstr(J, dattr_get(n, "class")); return true; }
    return false;
}

DNATIVE(cl_add)
{
    struct dnode *n = js_host_ptr(thisv, &classlist_class);
    for (int i = 0; n && i < argc; i++) cl_modify(n, sarg(J, argv[i]), 1);
    return JS_UNDEFINED;
}
DNATIVE(cl_remove)
{
    struct dnode *n = js_host_ptr(thisv, &classlist_class);
    for (int i = 0; n && i < argc; i++) cl_modify(n, sarg(J, argv[i]), 0);
    return JS_UNDEFINED;
}
DNATIVE(cl_contains)
{
    struct dnode *n = js_host_ptr(thisv, &classlist_class);
    return js_bool(n && cl_has(dattr_get(n, "class"), sarg(J, ARG(0))));
}
DNATIVE(cl_toggle)
{
    struct dnode *n = js_host_ptr(thisv, &classlist_class);
    if (!n) return js_bool(false);
    const char *t = sarg(J, ARG(0));
    bool has = cl_has(dattr_get(n, "class"), t);
    bool want = ARG(1).t == JS_UNDEF ? !has : js_truthy(ARG(1));
    if (want != has)
        cl_modify(n, t, want);
    return js_bool(want);
}
DNATIVE(cl_replace)
{
    struct dnode *n = js_host_ptr(thisv, &classlist_class);
    if (!n) return js_bool(false);
    const char *a = sarg(J, ARG(0)), *b = sarg(J, ARG(1));
    if (!cl_has(dattr_get(n, "class"), a))
        return js_bool(false);
    cl_modify(n, a, 0);
    cl_modify(n, b, 1);
    return js_bool(true);
}
DNATIVE(cl_item)
{
    struct dnode *n = js_host_ptr(thisv, &classlist_class);
    if (!n) return JS_NULLV;
    int want = (int)js_tonum(J, ARG(0)), i = 0;
    for (const char *p = dattr_get(n, "class") ? dattr_get(n, "class") : ""; *p;) {
        while (isspace((unsigned char)*p)) p++;
        const char *s = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (p == s) break;
        if (i++ == want)
            return js_strn(J, s, (size_t)(p - s));
    }
    return JS_NULLV;
}

static bool ds_get(js *J, jsobj *o, jsstr *key, jsval *out)
{
    struct dnode *n = js_host_data(o);
    char kb[160], an[176];
    kebab(key->s, kb, sizeof(kb));
    snprintf(an, sizeof(an), "data-%s", kb);
    const char *v = dattr_get(n, an);
    *out = v ? jstr(J, v) : JS_UNDEFINED;
    return true;
}
static bool ds_set(js *J, jsobj *o, jsstr *key, jsval v)
{
    struct dnode *n = js_host_data(o);
    char kb[160], an[176];
    kebab(key->s, kb, sizeof(kb));
    snprintf(an, sizeof(an), "data-%s", kb);
    dattr_set(n, an, sarg(J, v));
    return true;
}
static void ds_keys(js *J, jsobj *o, jsobj *arr)
{
    struct dnode *n = js_host_data(o);
    for (int i = 0; i < n->nattr; i++)
        if (!strncmp(n->attrs[i].name, "data-", 5)) {
            char c[160];
            camel(n->attrs[i].name + 5, c, sizeof(c));
            js_array_push(J, arr, jstr(J, c));
        }
}

/* location: components of d->url */
static void url_parts(const char *url, char *proto, char *host, char *port, char *path, char *search, char *hash, size_t n)
{
    proto[0] = host[0] = port[0] = path[0] = search[0] = hash[0] = 0;
    const char *p = url;
    const char *c = strchr(p, ':');
    if (c) {
        snprintf(proto, n, "%.*s:", (int)(c - p), p);
        p = c + 1;
    }
    if (!strncmp(p, "//", 2)) {
        p += 2;
        size_t al = strcspn(p, "/?#");
        char auth[256];
        snprintf(auth, sizeof(auth), "%.*s", (int)al, p);
        char *at = strrchr(auth, '@');
        char *hp = at ? at + 1 : auth;
        char *colon = strrchr(hp, ':');
        if (colon && !strchr(colon, ']')) {
            snprintf(port, n, "%s", colon + 1);
            *colon = 0;
        }
        snprintf(host, n, "%s", hp);
        p += al;
    }
    size_t pl = strcspn(p, "?#");
    snprintf(path, n, "%.*s", (int)pl, p);
    p += pl;
    if (*p == '?') {
        size_t sl = strcspn(p, "#");
        snprintf(search, n, "%.*s", (int)sl, p);
        p += sl;
    }
    if (*p == '#')
        snprintf(hash, n, "%s", p);
    if (!path[0] && host[0])
        snprintf(path, n, "/");
}

static bool loc_get(js *J, jsobj *o, jsstr *key, jsval *out)
{
    struct dom *d = js_host_data(o);
    char proto[40], host[256], port[16], path[1024], search[1024], hash[512];
    url_parts(d->url, proto, host, port, path, search, hash, sizeof(path) < 256 ? sizeof(path) : 256);
    const char *k = key->s;
    char b[2200];
    if (!strcmp(k, "href")) { *out = jstr(J, d->url); return true; }
    if (!strcmp(k, "protocol")) { *out = jstr(J, proto); return true; }
    if (!strcmp(k, "hostname")) { *out = jstr(J, host); return true; }
    if (!strcmp(k, "port")) { *out = jstr(J, port); return true; }
    if (!strcmp(k, "host")) { snprintf(b, sizeof(b), "%s%s%s", host, port[0] ? ":" : "", port); *out = jstr(J, b); return true; }
    if (!strcmp(k, "pathname")) { *out = jstr(J, path); return true; }
    if (!strcmp(k, "search")) { *out = jstr(J, search[1] ? search : ""); return true; }
    if (!strcmp(k, "hash")) { *out = jstr(J, hash[1] ? hash : ""); return true; }
    if (!strcmp(k, "origin")) {
        snprintf(b, sizeof(b), "%s//%s%s%s", proto, host, port[0] ? ":" : "", port);
        *out = jstr(J, host[0] ? b : "null");
        return true;
    }
    return false;
}

static bool loc_set(js *J, jsobj *o, jsstr *key, jsval v)
{
    struct dom *d = js_host_data(o);
    const char *k = key->s;
    if (!strcmp(k, "href") || !strcmp(k, "pathname") || !strcmp(k, "search")) {
        if (d->host.navigate)
            d->host.navigate(d->host.data, sarg(J, v), 0);
        return true;
    }
    if (!strcmp(k, "hash")) {
        char *p = strchr(d->url, '#');
        if (p) *p = 0;
        const char *h = sarg(J, v);
        if (strlen(d->url) + strlen(h) + 2 < sizeof(d->url)) {
            if (*h != '#') strcat(d->url, "#");
            strcat(d->url, h);
        }
        return true;
    }
    return false;
}

DNATIVE(loc_assign)
{
    struct dom *d = js_fn_data(callee);
    if (d->host.navigate)
        d->host.navigate(d->host.data, sarg(J, ARG(0)), 0);
    return JS_UNDEFINED;
}
DNATIVE(loc_reload)
{
    struct dom *d = js_fn_data(callee);
    if (d->host.navigate)
        d->host.navigate(d->host.data, d->url, 0);
    return JS_UNDEFINED;
}
DNATIVE(loc_toString) { struct dom *d = js_fn_data(callee); return jstr(J, d->url); }
DNATIVE(hist_go)
{
    struct dom *d = js_fn_data(callee);
    const char *name = sarg(J, js_get(J, js_objv(callee), "name"));
    int delta = !strcmp(name, "back") ? -1 : !strcmp(name, "forward") ? 1 : (int)js_tonum(J, ARG(0));
    if (delta && d->host.navigate)
        d->host.navigate(d->host.data, NULL, delta);
    return JS_UNDEFINED;
}
DNATIVE(hist_pushState)
{
    struct dom *d = js_fn_data(callee);
    if (ARG(2).t == JS_STR || ARG(2).t == JS_NUM) {
        const char *u = sarg(J, ARG(2));
        char proto[40], host[256], port[16], path[1024], search[1024], hash[512];
        url_parts(d->url, proto, host, port, path, search, hash, 256);
        char nu[2048];
        if (strstr(u, "://"))
            snprintf(nu, sizeof(nu), "%s", u);
        else if (u[0] == '/')
            snprintf(nu, sizeof(nu), "%s//%s%s%s%s", proto, host, port[0] ? ":" : "", port[0] ? port : "", u);
        else if (u[0] == '?' || u[0] == '#')
            snprintf(nu, sizeof(nu), "%s//%s%s%s%s%s", proto, host, port[0] ? ":" : "", port[0] ? port : "", path, u);
        else {
            char dir[1024];
            snprintf(dir, sizeof(dir), "%s", path);
            char *sl = strrchr(dir, '/');
            if (sl) sl[1] = 0;
            snprintf(nu, sizeof(nu), "%s//%s%s%s%s%s", proto, host, port[0] ? ":" : "", port[0] ? port : "", dir, u);
        }
        if (!strncmp(nu, "http", 4) || !strncmp(nu, "file", 4))
            snprintf(d->url, sizeof(d->url), "%s", nu);
    }
    return JS_UNDEFINED;
}

/* ---- node methods ------------------------------------------------------------------------------------------------ */

static char *lower_dup(const char *s)
{
    char *r = dstrdup(s);
    for (char *p = r; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    return r;
}

DNATIVE(el_getAttribute)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    char *k = lower_dup(sarg(J, ARG(0)));
    const char *v = dattr_get(n, k);
    free(k);
    return v ? jstr(J, v) : JS_NULLV;
}
DNATIVE(el_setAttribute)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    char *k = lower_dup(sarg(J, ARG(0)));
    dattr_set(n, k, sarg(J, ARG(1)));
    free(k);
    return JS_UNDEFINED;
}
DNATIVE(el_removeAttribute)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    char *k = lower_dup(sarg(J, ARG(0)));
    dattr_remove(n, k);
    free(k);
    return JS_UNDEFINED;
}
DNATIVE(el_hasAttribute)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    char *k = lower_dup(sarg(J, ARG(0)));
    bool r = dattr_get(n, k) != NULL;
    free(k);
    return js_bool(r);
}
DNATIVE(el_hasAttributes) { struct dnode *n = this_node(J, thisv); return n ? js_bool(n->nattr > 0) : JS_UNDEFINED; }
DNATIVE(el_getAttributeNames)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    jsobj *a = js_array_new(J);
    for (int i = 0; i < n->nattr; i++)
        js_array_push(J, a, jstr(J, n->attrs[i].name));
    return js_objv(a);
}

static struct dnode *arg_node(js *J, jsval v)
{
    struct dnode *n = unwrap(v);
    if (!n)
        js_throw_error(J, "TypeError", "argument is not a DOM node");
    return n;
}

DNATIVE(el_appendChild)
{
    struct dnode *p = this_node(J, thisv);
    if (!p) return JS_UNDEFINED;
    struct dnode *c = arg_node(J, ARG(0));
    if (!c) return JS_UNDEFINED;
    if (!dnode_insert(p, c, NULL))
        return js_throw_error(J, "Error", "HierarchyRequestError: the new child contains the parent");
    return ARG(0);
}
DNATIVE(el_insertBefore)
{
    struct dnode *p = this_node(J, thisv);
    if (!p) return JS_UNDEFINED;
    struct dnode *c = arg_node(J, ARG(0));
    if (!c) return JS_UNDEFINED;
    struct dnode *ref = ARG(1).t == JS_NULL || ARG(1).t == JS_UNDEF ? NULL : arg_node(J, ARG(1));
    if (J && js_thrown(J)) return JS_UNDEFINED;
    if (ref && ref->parent != p)
        return js_throw_error(J, "Error", "NotFoundError: the reference node is not a child of this node");
    if (!dnode_insert(p, c, ref))
        return js_throw_error(J, "Error", "HierarchyRequestError: the new child contains the parent");
    return ARG(0);
}
DNATIVE(el_removeChild)
{
    struct dnode *p = this_node(J, thisv);
    if (!p) return JS_UNDEFINED;
    struct dnode *c = arg_node(J, ARG(0));
    if (!c) return JS_UNDEFINED;
    if (c->parent != p)
        return js_throw_error(J, "Error", "NotFoundError: the node is not a child of this node");
    dnode_detach(c);
    return ARG(0);
}
DNATIVE(el_replaceChild)
{
    struct dnode *p = this_node(J, thisv);
    struct dnode *nw = p ? arg_node(J, ARG(0)) : NULL, *old = nw ? arg_node(J, ARG(1)) : NULL;
    if (!p || !nw || !old) return JS_UNDEFINED;
    if (old->parent != p)
        return js_throw_error(J, "Error", "NotFoundError: the node to replace is not a child of this node");
    dnode_insert(p, nw, old);
    dnode_detach(old);
    return ARG(1);
}
DNATIVE(el_remove)
{
    struct dnode *n = this_node(J, thisv);
    if (n) dnode_detach(n);
    return JS_UNDEFINED;
}

/* node arguments that may also be strings (append/prepend/before/after) */
static struct dnode *node_or_text(js *J, struct dom *d, jsval v)
{
    struct dnode *n = unwrap(v);
    return n ? n : dtext_new(d, sarg(J, v));
}
DNATIVE(el_append)
{
    struct dnode *p = this_node(J, thisv);
    for (int i = 0; p && i < argc; i++)
        dnode_insert(p, node_or_text(J, p->dom, argv[i]), NULL);
    return JS_UNDEFINED;
}
DNATIVE(el_prepend)
{
    struct dnode *p = this_node(J, thisv);
    if (!p) return JS_UNDEFINED;
    struct dnode *first = p->first;
    for (int i = 0; i < argc; i++)
        dnode_insert(p, node_or_text(J, p->dom, argv[i]), first);
    return JS_UNDEFINED;
}
DNATIVE(el_before)
{
    struct dnode *n = this_node(J, thisv);
    if (!n || !n->parent) return JS_UNDEFINED;
    for (int i = 0; i < argc; i++)
        dnode_insert(n->parent, node_or_text(J, n->dom, argv[i]), n);
    return JS_UNDEFINED;
}
DNATIVE(el_after)
{
    struct dnode *n = this_node(J, thisv);
    if (!n || !n->parent) return JS_UNDEFINED;
    struct dnode *ref = n->next;
    for (int i = 0; i < argc; i++)
        dnode_insert(n->parent, node_or_text(J, n->dom, argv[i]), ref);
    return JS_UNDEFINED;
}
DNATIVE(el_replaceWith)
{
    struct dnode *n = this_node(J, thisv);
    if (!n || !n->parent) return JS_UNDEFINED;
    struct dnode *ref = n->next, *par = n->parent;
    dnode_detach(n);
    for (int i = 0; i < argc; i++)
        dnode_insert(par, node_or_text(J, n->dom, argv[i]), ref);
    return JS_UNDEFINED;
}

static struct dnode *clone_node(struct dom *d, struct dnode *n, bool deep)
{
    struct dnode *c = dnode_new(d, n->type, n->tag);
    if (n->text)
        c->text = dstrdup(n->text);
    for (int i = 0; i < n->nattr; i++) {
        c->attrs = realloc(c->attrs, sizeof(*c->attrs) * (size_t)(c->nattr + 1));
        c->attrs[c->nattr].name = dstrdup(n->attrs[i].name);
        c->attrs[c->nattr].value = dstrdup(n->attrs[i].value);
        c->nattr++;
    }
    if (deep)
        for (struct dnode *k = n->first; k; k = k->next)
            dnode_insert(c, clone_node(d, k, true), NULL);
    return c;
}
DNATIVE(el_cloneNode)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    return wrap(n->dom, clone_node(n->dom, n, js_truthy(ARG(0))));
}
DNATIVE(el_contains)
{
    struct dnode *n = this_node(J, thisv), *o = unwrap(ARG(0));
    return js_bool(n && o && dnode_contains(n, o));
}
DNATIVE(el_hasChildNodes) { struct dnode *n = this_node(J, thisv); return n ? js_bool(n->first != NULL) : JS_UNDEFINED; }
DNATIVE(el_noop) { return JS_UNDEFINED; }
DNATIVE(el_matches)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    bool valid;
    bool r = dom_matches(n, sarg(J, ARG(0)), &valid);
    if (!valid)
        return js_throw_error(J, "SyntaxError", "'%s' is not a valid selector", sarg(J, ARG(0)));
    return js_bool(r);
}
DNATIVE(el_closest)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    const char *sel = sarg(J, ARG(0));
    for (struct dnode *p = n; p; p = p->parent) {
        bool valid;
        if (p->type == DN_ELEMENT && dom_matches(p, sel, &valid))
            return wrap(n->dom, p);
        if (!valid)
            return js_throw_error(J, "SyntaxError", "'%s' is not a valid selector", sel);
    }
    return JS_NULLV;
}
DNATIVE(el_querySelector)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    struct dnode **l;
    int c;
    if (!dom_query(n, sarg(J, ARG(0)), true, &l, &c))
        return js_throw_error(J, "SyntaxError", "'%s' is not a valid selector", sarg(J, ARG(0)));
    jsval r = c ? wrap(n->dom, l[0]) : JS_NULLV;
    free(l);
    return r;
}
DNATIVE(el_querySelectorAll)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    struct dnode **l;
    int c;
    if (!dom_query(n, sarg(J, ARG(0)), false, &l, &c))
        return js_throw_error(J, "SyntaxError", "'%s' is not a valid selector", sarg(J, ARG(0)));
    jsval r = nodes_to_array(n->dom, l, c);
    free(l);
    return r;
}
DNATIVE(el_getElementsByTagName)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    char *t = lower_dup(sarg(J, ARG(0)));
    struct dnode **l;
    int c;
    bool ok = dom_query(n, t, false, &l, &c);
    free(t);
    jsval r = ok ? nodes_to_array(n->dom, l, c) : js_objv(js_array_new(J));
    free(l);
    return r;
}
DNATIVE(el_getElementsByClassName)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    struct sbuf sb = { 0 };
    for (const char *p = sarg(J, ARG(0)); *p;) {
        while (isspace((unsigned char)*p)) p++;
        const char *s = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (p > s) {
            sbuf_putc(&sb, '.');
            sbuf_add(&sb, s, (size_t)(p - s));
        }
    }
    struct dnode **l = NULL;
    int c = 0;
    jsval r;
    if (sb.n && dom_query(n, sb.p, false, &l, &c))
        r = nodes_to_array(n->dom, l, c);
    else
        r = js_objv(js_array_new(J));
    free(l);
    free(sb.p);
    return r;
}
DNATIVE(el_getElementsByName)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    char sel[300];
    snprintf(sel, sizeof(sel), "[name=\"%.250s\"]", sarg(J, ARG(0)));
    struct dnode **l;
    int c;
    jsval r = dom_query(n->dom->doc, sel, false, &l, &c) ? nodes_to_array(n->dom, l, c) : js_objv(js_array_new(J));
    free(l);
    return r;
}
DNATIVE(el_insertAdjacentHTML)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    char *pos = lower_dup(sarg(J, ARG(0)));
    const char *html = sarg(J, ARG(1));
    struct dnode *frag = dnode_new(n->dom, DN_FRAGMENT, NULL);
    dom_parse_into(n->dom, frag, html, strlen(html));
    for (struct dnode *c = frag->first; c; c = c->next)
        if (c->type == DN_ELEMENT && !strcmp(c->tag, "script"))
            c->script_done = true;
    if (!strcmp(pos, "beforeend")) dnode_insert(n, frag, NULL);
    else if (!strcmp(pos, "afterbegin")) dnode_insert(n, frag, n->first);
    else if (!strcmp(pos, "beforebegin") && n->parent) dnode_insert(n->parent, frag, n);
    else if (!strcmp(pos, "afterend") && n->parent) dnode_insert(n->parent, frag, n->next);
    free(pos);
    return JS_UNDEFINED;
}
DNATIVE(el_getBoundingClientRect)
{
    jsobj *r = js_object_new(J);
    jsval rv = js_objv(r);
    static const char *const keys[] = { "x", "y", "top", "left", "right", "bottom", "width", "height" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        js_set(J, rv, keys[i], js_num(0));
    return rv;
}

/* ---- document methods ----------------------------------------------------------------------------------------- */

DNATIVE(doc_createElement)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    const char *tag = sarg(J, argc > 1 && ARG(0).t != JS_STR ? ARG(1) : ARG(0));
    if (argc > 1 && ARG(0).t == JS_STR && ARG(1).t == JS_STR)
        tag = sarg(J, ARG(1));                                  /* createElementNS(ns, name) */
    if (!*tag || strpbrk(tag, " <>/\"'"))
        return js_throw_error(J, "Error", "InvalidCharacterError: invalid tag name");
    return wrap(n->dom, dnode_new(n->dom, DN_ELEMENT, tag));
}
DNATIVE(doc_createTextNode) { struct dnode *n = this_node(J, thisv); return n ? wrap(n->dom, dtext_new(n->dom, sarg(J, ARG(0)))) : JS_UNDEFINED; }
DNATIVE(doc_createComment)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    struct dnode *c = dnode_new(n->dom, DN_COMMENT, NULL);
    c->text = dstrdup(sarg(J, ARG(0)));
    return wrap(n->dom, c);
}
DNATIVE(doc_createDocumentFragment) { struct dnode *n = this_node(J, thisv); return n ? wrap(n->dom, dnode_new(n->dom, DN_FRAGMENT, NULL)) : JS_UNDEFINED; }
DNATIVE(doc_getElementById)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    return wrap(n->dom, dom_by_id(n->dom, sarg(J, ARG(0))));
}
DNATIVE(doc_write)
{
    struct dnode *n = this_node(J, thisv);
    if (!n) return JS_UNDEFINED;
    struct dom *d = n->dom;
    struct sbuf sb = { 0 };
    for (int i = 0; i < argc; i++)
        sbuf_addz(&sb, sarg(J, argv[i]));
    const char *name = sarg(J, js_get(J, js_objv(callee), "name"));
    if (!strcmp(name, "writeln"))
        sbuf_putc(&sb, '\n');
    struct dnode *frag = dnode_new(d, DN_FRAGMENT, NULL);
    dom_parse_into(d, frag, sb.p ? sb.p : "", sb.n);
    free(sb.p);
    struct dnode *parent = d->body, *ref = NULL;
    if (d->current_script && d->current_script->parent) {
        parent = d->current_script->parent;
        ref = d->current_script->next;
    }
    dnode_insert(parent, frag, ref);
    return JS_UNDEFINED;
}
DNATIVE(doc_createEvent)
{
    struct dom *d = js_fn_data(callee);
    jsval ev = make_event(d, "", false, false, JS_UNDEFINED);
    return ev;
}
DNATIVE(ev_initEvent)
{
    if (thisv.t != JS_OBJ) return JS_UNDEFINED;
    js_set(J, thisv, "type", js_cstr(J, sarg(J, ARG(0))));
    js_set(J, thisv, "bubbles", js_bool(js_truthy(ARG(1))));
    js_set(J, thisv, "cancelable", js_bool(js_truthy(ARG(2))));
    return JS_UNDEFINED;
}
DNATIVE(impl_createHTMLDocument)
{
    struct dom *d = js_fn_data(callee);
    struct dnode *doc = dnode_new(d, DN_DOCUMENT, NULL);
    struct dnode *html = dnode_new(d, DN_ELEMENT, "html"), *head = dnode_new(d, DN_ELEMENT, "head"),
                 *body = dnode_new(d, DN_ELEMENT, "body");
    dnode_insert(doc, html, NULL);
    dnode_insert(html, head, NULL);
    dnode_insert(html, body, NULL);
    if (ARG(0).t == JS_STR) {
        struct dnode *t = dnode_new(d, DN_ELEMENT, "title");
        dnode_insert(t, dtext_new(d, sarg(J, ARG(0))), NULL);
        dnode_insert(head, t, NULL);
    }
    return wrap(d, doc);
}
DNATIVE(doc_null) { return JS_NULLV; }
DNATIVE(doc_false) { return js_bool(false); }

/* ---- timers / window functions --------------------------------------------------------------------------------- */

static long long dom_now(struct dom *d)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + d->clock_offset;
}

static jsval add_timer(js *J, struct dom *d, int argc, jsval *argv, bool interval)
{
    if (!js_is_function(ARG(0))) {
        if (ARG(0).t == JS_STR)
            return js_throw_error(J, "TypeError", "setTimeout with a string is not supported");
        return js_num(0);
    }
    double delay = ARG(1).t == JS_UNDEF ? 0 : js_tonum(J, ARG(1));
    if (!(delay >= 0))
        delay = 0;
    if (interval && delay < 4)
        delay = 4;
    struct timer *t = calloc(1, sizeof(*t));
    if (!t)
        exit(1);
    t->id = ++d->next_timer;
    t->fn = ARG(0);
    t->due = dom_now(d) + (long long)delay;
    t->interval = interval ? (long long)delay : 0;
    if (argc > 2) {
        t->nargs = argc - 2;
        t->args = malloc(sizeof(jsval) * (size_t)t->nargs);
        memcpy(t->args, argv + 2, sizeof(jsval) * (size_t)t->nargs);
    }
    struct timer **p = &d->timers;
    while (*p)
        p = &(*p)->next;
    *p = t;
    return js_num(t->id);
}
DNATIVE(win_setTimeout) { return add_timer(J, js_fn_data(callee), argc, argv, false); }
DNATIVE(win_setInterval) { return add_timer(J, js_fn_data(callee), argc, argv, true); }
DNATIVE(win_requestAnimationFrame)
{
    jsval b[2] = { ARG(0), js_num(16) };
    return add_timer(J, js_fn_data(callee), 2, b, false);
}
DNATIVE(win_clearTimer)
{
    struct dom *d = js_fn_data(callee);
    int id = (int)js_tonum(J, ARG(0));
    for (struct timer **p = &d->timers; *p; p = &(*p)->next)
        if ((*p)->id == id) {
            struct timer *t = *p;
            *p = t->next;
            free(t->args);
            free(t);
            break;
        }
    return JS_UNDEFINED;
}
DNATIVE(win_alert)
{
    struct dom *d = js_fn_data(callee);
    const char *m = sarg(J, ARG(0));
    if (d->host.alert) d->host.alert(d->host.data, m);
    else report(d, "alert", m);
    return JS_UNDEFINED;
}
DNATIVE(win_getComputedStyle)
{
    struct dom *d = js_fn_data(callee);
    struct dnode *n = unwrap(ARG(0));
    if (!n) return js_throw_error(J, "TypeError", "getComputedStyle needs an element");
    return make_sub(d, &style_class, n, d->style_proto);
}
DNATIVE(win_matchMedia)
{
    jsobj *m = js_object_new(J);
    jsval mv = js_objv(m);
    js_set(J, mv, "matches", js_bool(false));
    js_set(J, mv, "media", js_cstr(J, sarg(J, ARG(0))));
    js_def_native(J, m, "addListener", el_noop, 1);
    js_def_native(J, m, "removeListener", el_noop, 1);
    js_def_native(J, m, "addEventListener", el_noop, 2);
    js_def_native(J, m, "removeEventListener", el_noop, 2);
    return mv;
}
DNATIVE(win_performance_now)
{
    struct dom *d = js_fn_data(callee);
    return js_num((double)dom_now(d));
}
DNATIVE(win_btoa)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t l;
    const char *s = js_tostr(J, ARG(0), &l);
    if (!s) return JS_UNDEFINED;
    struct sbuf sb = { 0 };
    for (size_t i = 0; i < l; i += 3) {
        uint32_t v = (uint32_t)(uint8_t)s[i] << 16 | (i + 1 < l ? (uint32_t)(uint8_t)s[i + 1] << 8 : 0) | (i + 2 < l ? (uint8_t)s[i + 2] : 0);
        sbuf_putc(&sb, tab[(v >> 18) & 63]);
        sbuf_putc(&sb, tab[(v >> 12) & 63]);
        sbuf_putc(&sb, i + 1 < l ? tab[(v >> 6) & 63] : '=');
        sbuf_putc(&sb, i + 2 < l ? tab[v & 63] : '=');
    }
    jsval r = jstr(J, sb.p);
    free(sb.p);
    return r;
}
DNATIVE(win_atob)
{
    size_t l;
    const char *s = js_tostr(J, ARG(0), &l);
    if (!s) return JS_UNDEFINED;
    struct sbuf sb = { 0 };
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < l; i++) {
        int v;
        char c = s[i];
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=' || isspace((unsigned char)c)) continue;
        else { free(sb.p); return js_throw_error(J, "Error", "InvalidCharacterError: the string to be decoded is not correctly encoded"); }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            sbuf_putc(&sb, (char)((acc >> bits) & 0xff));
        }
    }
    jsval r = js_strn(J, sb.p ? sb.p : "", sb.n);
    free(sb.p);
    return r;
}

/* observers and other APIs that pages feature-test: harmless stubs */
DNATIVE(Observer_ctor)
{
    if (thisv.t == JS_OBJ) {
        js_def_native(J, thisv.u.o, "observe", el_noop, 1);
        js_def_native(J, thisv.u.o, "unobserve", el_noop, 1);
        js_def_native(J, thisv.u.o, "disconnect", el_noop, 0);
        js_def_native(J, thisv.u.o, "takeRecords", doc_null, 0);
    }
    return thisv;
}
DNATIVE(Illegal_ctor) { return js_throw_error(J, "TypeError", "Illegal constructor"); }

/* storage (localStorage / sessionStorage): a plain object; methods are non-enumerable, items are properties */
DNATIVE(st_getItem)
{
    jsval k = js_cstr(J, sarg(J, ARG(0)));
    jsval has = js_call(J, js_get(J, js_objv(js_object_proto(J)), "hasOwnProperty"), thisv, 1, &k, NULL);
    if (!js_truthy(has)) return JS_NULLV;
    return js_cstr(J, sarg(J, js_getv(J, thisv, k)));
}
DNATIVE(st_setItem)
{
    if (thisv.t == JS_OBJ)
        js_set(J, thisv, sarg(J, ARG(0)), js_cstr(J, sarg(J, ARG(1))));
    return JS_UNDEFINED;
}
DNATIVE(st_removeItem)
{
    if (thisv.t != JS_OBJ) return JS_UNDEFINED;
    jsval objc = js_get(J, js_objv(js_global(J)), "Object");
    (void)objc;
    js_set(J, thisv, sarg(J, ARG(0)), JS_UNDEFINED);
    return JS_UNDEFINED;
}
DNATIVE(st_clear) { (void)thisv; return JS_UNDEFINED; }

/* ---- timers and entry points ------------------------------------------------------------------------------------ */

long long dom_next_timer_ms(struct dom *d)
{
    if (!d->timers)
        return -1;
    long long best = d->timers->due;
    for (struct timer *t = d->timers; t; t = t->next)
        if (t->due < best)
            best = t->due;
    long long ms = best - dom_now(d);
    return ms < 0 ? 0 : ms;
}

static bool run_one_timer(struct dom *d, struct timer *t)
{
    js *J = d->J;
    jsval fn = t->fn;
    int nargs = t->nargs;
    jsval *args = nargs ? malloc(sizeof(jsval) * (size_t)nargs) : NULL;
    if (nargs)
        memcpy(args, t->args, sizeof(jsval) * (size_t)nargs);
    int id = t->id;
    if (t->interval) {
        t->due = dom_now(d) + t->interval;
    } else {
        for (struct timer **p = &d->timers; *p; p = &(*p)->next)
            if (*p == t) {
                *p = t->next;
                break;
            }
        free(t->args);
        free(t);
    }
    (void)id;
    bool threw = false;
    if (run_allowed(d))
        js_call(J, fn, js_objv(js_global(J)), nargs, args, &threw);
    if (threw)
        report_exception(d);
    free(args);
    return true;
}

bool dom_run_timers(struct dom *d)
{
    long long now = dom_now(d);
    int ran = 0;
    for (;;) {
        struct timer *best = NULL;
        for (struct timer *t = d->timers; t; t = t->next)
            if (t->due <= now && (!best || t->due < best->due))
                best = t;
        if (!best || ran >= 50)
            break;
        run_one_timer(d, best);
        ran++;
    }
    flush_scripts(d);
    return dom_take_dirty(d);
}

void dom_run_timers_virtual(struct dom *d, long long max_ms)
{
    long long start = dom_now(d);
    for (int i = 0; i < 100000; i++) {
        struct timer *best = NULL;
        for (struct timer *t = d->timers; t; t = t->next)
            if (!best || t->due < best->due)
                best = t;
        if (!best || best->due - start > max_ms)
            break;
        long long now = dom_now(d);
        if (best->due > now)
            d->clock_offset += best->due - now;
        run_one_timer(d, best);
        flush_scripts(d);
    }
}

bool dom_take_dirty(struct dom *d)
{
    bool r = d->dirty;
    d->dirty = false;
    return r;
}

bool dom_click(struct dom *d, int id, char *href, size_t hrefn)
{
    struct dnode *n = dom_node_by_numeric_id(d, id);
    if (!n)
        return false;
    run_allowed(d);
    jsval ev = make_event(d, "click", true, true, JS_UNDEFINED);
    bool ok = dispatch_node_event(d, n, ev);
    flush_scripts(d);
    const char *h = n->tag && !strcmp(n->tag, "a") ? dattr_get(n, "href") : NULL;
    if (ok && h && h[0]) {
        snprintf(href, hrefn, "%s", h);
        return true;
    }
    return false;
}

/* ---- scripts ------------------------------------------------------------------------------------------------------ */

static bool script_type_ok(const struct dnode *s)
{
    const char *t = dattr_get(s, "type");
    if (!t || !*t)
        return true;
    return !strcasecmp(t, "text/javascript") || !strcasecmp(t, "application/javascript") ||
           !strcasecmp(t, "text/ecmascript") || !strcasecmp(t, "application/x-javascript") || !strcasecmp(t, "text/jscript");
}

static struct dnode *next_script(struct dom *d)
{
    const struct dnode *c = d->doc->first;
    while (c) {
        if (c->type == DN_ELEMENT && !strcmp(c->tag, "script") && !c->script_done)
            return (struct dnode *)c;
        if (c->first) { c = c->first; continue; }
        while (c && c != d->doc && !c->next) c = c->parent;
        if (!c || c == d->doc) break;
        c = c->next;
    }
    return NULL;
}

static void run_script_node(struct dom *d, struct dnode *s)
{
    s->script_done = true;
    if (!script_type_ok(s))
        return;
    const char *src = dattr_get(s, "src");
    char *code = NULL;
    size_t len = 0;
    char name[300];
    if (src && *src) {
        char err[300] = "";
        if (!d->host.fetch_script || !d->host.fetch_script(d->host.data, src, &code, &len, err, sizeof(err))) {
            char m[700];
            snprintf(m, sizeof(m), "cannot load script %.200s: %.300s", src, err[0] ? err : "no loader");
            report(d, "error", m);
            return;
        }
        snprintf(name, sizeof(name), "%s", src);
    } else {
        struct sbuf sb = { 0 };
        for (struct dnode *c = s->first; c; c = c->next)
            if (c->type == DN_TEXT)
                sbuf_addz(&sb, c->text);
        if (!sb.n) {
            free(sb.p);
            return;
        }
        code = sb.p;
        len = sb.n;
        snprintf(name, sizeof(name), "inline script");
    }
    d->current_script = s;
    if (!run_allowed(d)) {
        d->current_script = NULL;
        free(code);
        return;
    }
    char err[500];
    if (!js_run(d->J, code, len, name, err, sizeof(err))) {
        char m[900];
        snprintf(m, sizeof(m), "%.300s: %.500s", name, err);
        report(d, "error", m);
    }
    if (getenv("LUMEN_JS_STATS"))
        fprintf(stderr, "[js stats] %s: %zu bytes of script, memory now %zu MB, %llu steps\n", name, len, js_memory_used(d->J) >> 20,
                (unsigned long long)js_steps_used(d->J));
    d->current_script = NULL;
    free(code);
}

static void flush_scripts(struct dom *d)
{
    for (int guard = 0; guard < 1000; guard++) {
        struct dnode *s = next_script(d);
        if (!s)
            break;
        run_script_node(d, s);
    }
}

/* ---- setup -------------------------------------------------------------------------------------------------------- */

static void console_sink(void *data, const char *level, const char *msg)
{
    report(data, level, msg);
}

static void def_fn(struct dom *d, jsobj *o, const char *name, js_native fn, int nargs)
{
    jsobj *f = js_native_new(d->J, name, fn, nargs, d);
    js_def_value(d->J, o, name, js_objv(f));
}

static jsobj *def_ctor(struct dom *d, const char *name, js_native fn, jsobj *proto)
{
    jsobj *c = js_native_new(d->J, name, fn, 1, d);
    if (proto) {
        js_def_value(d->J, c, "prototype", js_objv(proto));
        js_def_value(d->J, proto, "constructor", js_objv(c));
    }
    js_def_value(d->J, js_global(d->J), name, js_objv(c));
    return c;
}

static void setup_globals(struct dom *d)
{
    js *J = d->J;
    jsobj *g = js_global(J);
    jsval gv = js_objv(g);

    /* node prototype: methods shared by documents, elements, text nodes */
    jsobj *np = js_object_new(J);
    d->node_proto = np;
#define M(name, fn, n) js_def_native(J, np, name, fn, n)
    M("getAttribute", el_getAttribute, 1); M("setAttribute", el_setAttribute, 2); M("removeAttribute", el_removeAttribute, 1);
    M("hasAttribute", el_hasAttribute, 1); M("hasAttributes", el_hasAttributes, 0); M("getAttributeNames", el_getAttributeNames, 0);
    M("appendChild", el_appendChild, 1); M("insertBefore", el_insertBefore, 2); M("removeChild", el_removeChild, 1);
    M("replaceChild", el_replaceChild, 2); M("remove", el_remove, 0); M("append", el_append, 1); M("prepend", el_prepend, 1);
    M("before", el_before, 1); M("after", el_after, 1); M("replaceWith", el_replaceWith, 1); M("cloneNode", el_cloneNode, 1);
    M("contains", el_contains, 1); M("hasChildNodes", el_hasChildNodes, 0); M("normalize", el_noop, 0);
    M("matches", el_matches, 1); M("webkitMatchesSelector", el_matches, 1); M("closest", el_closest, 1);
    M("querySelector", el_querySelector, 1); M("querySelectorAll", el_querySelectorAll, 1);
    M("getElementsByTagName", el_getElementsByTagName, 1); M("getElementsByClassName", el_getElementsByClassName, 1);
    M("getElementsByName", el_getElementsByName, 1); M("insertAdjacentHTML", el_insertAdjacentHTML, 2);
    M("click", el_click, 0); M("focus", el_noop, 0); M("blur", el_noop, 0); M("scrollIntoView", el_noop, 0);
    M("getBoundingClientRect", el_getBoundingClientRect, 0); M("getClientRects", el_noop, 0);
    M("createElement", doc_createElement, 1); M("createElementNS", doc_createElement, 2);
    M("createTextNode", doc_createTextNode, 1); M("createComment", doc_createComment, 1);
    M("createDocumentFragment", doc_createDocumentFragment, 0); M("getElementById", doc_getElementById, 1);
    M("write", doc_write, 1); M("writeln", doc_write, 1); M("open", el_noop, 0); M("close", el_noop, 0);
    M("hasFocus", doc_false, 0); M("elementFromPoint", doc_null, 2);
#undef M
    def_fn(d, np, "addEventListener", addEventListener, 2);
    def_fn(d, np, "removeEventListener", removeEventListener, 2);
    def_fn(d, np, "dispatchEvent", dispatchEvent, 1);
    def_fn(d, np, "createEvent", doc_createEvent, 1);

    d->style_proto = js_object_new(J);
    js_def_native(J, d->style_proto, "getPropertyValue", style_getPropertyValue, 1);
    js_def_native(J, d->style_proto, "setProperty", style_setProperty, 2);
    js_def_native(J, d->style_proto, "removeProperty", style_removeProperty, 1);
    d->classlist_proto = js_object_new(J);
    js_def_native(J, d->classlist_proto, "add", cl_add, 1); js_def_native(J, d->classlist_proto, "remove", cl_remove, 1);
    js_def_native(J, d->classlist_proto, "contains", cl_contains, 1); js_def_native(J, d->classlist_proto, "toggle", cl_toggle, 2);
    js_def_native(J, d->classlist_proto, "replace", cl_replace, 2); js_def_native(J, d->classlist_proto, "item", cl_item, 1);

    /* events */
    d->event_proto = js_object_new(J);
    js_def_native(J, d->event_proto, "preventDefault", ev_preventDefault, 0);
    js_def_native(J, d->event_proto, "stopPropagation", ev_stopPropagation, 0);
    js_def_native(J, d->event_proto, "stopImmediatePropagation", ev_stopImmediate, 0);
    js_def_native(J, d->event_proto, "initEvent", ev_initEvent, 3);
    jsobj *ec = def_ctor(d, "Event", Event_ctor, d->event_proto);
    js_fn_mark_ctor(ec);
    jsobj *cec = def_ctor(d, "CustomEvent", CustomEvent_ctor, d->event_proto);
    js_fn_mark_ctor(cec);

    /* node constructors (for instanceof) */
    static const char *const ctor_names[] = { "Node", "Element", "HTMLElement", "Document", "HTMLDocument", "Text" };
    for (size_t i = 0; i < sizeof(ctor_names) / sizeof(ctor_names[0]); i++)
        def_ctor(d, ctor_names[i], Illegal_ctor, np);
    static const char *const observers[] = { "MutationObserver", "IntersectionObserver", "ResizeObserver" };
    for (size_t i = 0; i < sizeof(observers) / sizeof(observers[0]); i++) {
        jsobj *c = def_ctor(d, observers[i], Observer_ctor, js_object_new(J));
        js_fn_mark_ctor(c);
    }

    /* document, window */
    jsval doc = wrap(d, d->doc);
    {
        jsobj *impl = js_object_new(J);
        def_fn(d, impl, "createHTMLDocument", impl_createHTMLDocument, 1);
        def_fn(d, impl, "hasFeature", doc_false, 0);
        js_set(J, doc, "implementation", js_objv(impl));
    }
    d->document_obj = doc.u.o;
    js_def_value(J, g, "document", doc);
    js_def_value(J, g, "window", gv);
    js_def_value(J, g, "self", gv);
    js_def_value(J, g, "top", gv);
    js_def_value(J, g, "parent", gv);
    js_def_value(J, g, "frames", gv);
    js_def_value(J, g, "name", js_cstr(J, ""));
    js_def_value(J, g, "innerWidth", js_num(900));
    js_def_value(J, g, "innerHeight", js_num(600));
    js_def_value(J, g, "outerWidth", js_num(900));
    js_def_value(J, g, "outerHeight", js_num(640));
    js_def_value(J, g, "devicePixelRatio", js_num(1));
    js_def_value(J, g, "scrollX", js_num(0)); js_def_value(J, g, "scrollY", js_num(0));
    js_def_value(J, g, "pageXOffset", js_num(0)); js_def_value(J, g, "pageYOffset", js_num(0));
    def_fn(d, g, "addEventListener", addEventListener, 2);
    def_fn(d, g, "removeEventListener", removeEventListener, 2);
    def_fn(d, g, "dispatchEvent", dispatchEvent, 1);
    def_fn(d, g, "setTimeout", win_setTimeout, 2); def_fn(d, g, "setInterval", win_setInterval, 2);
    def_fn(d, g, "clearTimeout", win_clearTimer, 1); def_fn(d, g, "clearInterval", win_clearTimer, 1);
    def_fn(d, g, "requestAnimationFrame", win_requestAnimationFrame, 1); def_fn(d, g, "cancelAnimationFrame", win_clearTimer, 1);
    def_fn(d, g, "alert", win_alert, 1);
    def_fn(d, g, "confirm", doc_false, 1); def_fn(d, g, "prompt", doc_null, 1); def_fn(d, g, "open", doc_null, 1);
    def_fn(d, g, "close", el_noop, 0); def_fn(d, g, "focus", el_noop, 0); def_fn(d, g, "blur", el_noop, 0);
    def_fn(d, g, "scrollTo", el_noop, 2); def_fn(d, g, "scrollBy", el_noop, 2); def_fn(d, g, "scroll", el_noop, 2);
    def_fn(d, g, "getComputedStyle", win_getComputedStyle, 1); def_fn(d, g, "matchMedia", win_matchMedia, 1);
    def_fn(d, g, "btoa", win_btoa, 1); def_fn(d, g, "atob", win_atob, 1);
    def_fn(d, g, "getSelection", doc_null, 0);

    /* location, history, navigator, screen, performance, storage */
    d->location_obj = js_host_new(J, &location_class, d, NULL);
    jsobj *lo = d->location_obj;
    def_fn(d, lo, "assign", loc_assign, 1); def_fn(d, lo, "replace", loc_assign, 1); def_fn(d, lo, "reload", loc_reload, 0);
    def_fn(d, lo, "toString", loc_toString, 0);
    js_def_value(J, g, "location", js_objv(lo));
    jsobj *hi = js_object_new(J);
    d->history_obj = hi;
    def_fn(d, hi, "back", hist_go, 0); def_fn(d, hi, "forward", hist_go, 0); def_fn(d, hi, "go", hist_go, 1);
    def_fn(d, hi, "pushState", hist_pushState, 3); def_fn(d, hi, "replaceState", hist_pushState, 3);
    js_def_value(J, hi, "length", js_num(1));
    js_def_value(J, hi, "state", JS_NULLV);
    js_def_value(J, g, "history", js_objv(hi));
    jsobj *nav = js_object_new(J);
    js_set(J, js_objv(nav), "userAgent", js_cstr(J, "Mozilla/5.0 (X11; Linux x86_64) LumenWeb/1.0"));
    js_set(J, js_objv(nav), "language", js_cstr(J, "en-US"));
    js_set(J, js_objv(nav), "platform", js_cstr(J, "Linux x86_64"));
    js_set(J, js_objv(nav), "cookieEnabled", js_bool(false));
    js_set(J, js_objv(nav), "onLine", js_bool(true));
    jsobj *langs = js_array_new(J);
    js_array_push(J, langs, js_cstr(J, "en-US"));
    js_set(J, js_objv(nav), "languages", js_objv(langs));
    js_def_value(J, g, "navigator", js_objv(nav));
    jsobj *scr = js_object_new(J);
    static const char *const sk[] = { "width", "availWidth", "height", "availHeight", "colorDepth", "pixelDepth" };
    static const double sv[] = { 1024, 1024, 768, 768, 24, 24 };
    for (int i = 0; i < 6; i++)
        js_set(J, js_objv(scr), sk[i], js_num(sv[i]));
    js_def_value(J, g, "screen", js_objv(scr));
    jsobj *perf = js_object_new(J);
    def_fn(d, perf, "now", win_performance_now, 0);
    js_def_value(J, g, "performance", js_objv(perf));
    for (int i = 0; i < 2; i++) {
        jsobj *st = js_object_new(J);
        js_def_native(J, st, "getItem", st_getItem, 1);
        js_def_native(J, st, "setItem", st_setItem, 2);
        js_def_native(J, st, "removeItem", st_removeItem, 1);
        js_def_native(J, st, "clear", st_clear, 0);
        js_def_value(J, g, i ? "sessionStorage" : "localStorage", js_objv(st));
    }
}

struct dom *dom_create(const char *url, const struct dom_host *host)
{
    struct dom *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    if (host)
        d->host = *host;
    snprintf(d->url, sizeof(d->url), "%s", url ? url : "about:blank");
    d->J = js_new();
    js_set_console(d->J, console_sink, d);
    d->doc = dnode_new(d, DN_DOCUMENT, NULL);
    setup_globals(d);
    return d;
}

void dom_free(struct dom *d)
{
    if (!d)
        return;
    while (d->timers) {
        struct timer *t = d->timers;
        d->timers = t->next;
        free(t->args);
        free(t);
    }
    for (struct listener *l = d->win_listeners; l;) {
        struct listener *nx = l->next;
        free(l->type);
        free(l);
        l = nx;
    }
    js_free(d->J);
    dom_free_nodes(d);
    free(d->title_cache);
    free(d);
}

void dom_load(struct dom *d, const char *html, size_t len)
{
    dom_parse_into(d, d->doc, html, len);
    dom_normalize(d);
    d->ready = 0;
    flush_scripts(d);
    d->ready = 1;
    run_allowed(d);
    jsval ev = make_event(d, "DOMContentLoaded", true, false, JS_UNDEFINED);
    dispatch_node_event(d, d->doc, ev);
    flush_scripts(d);
    d->ready = 2;
    jsval lev = make_event(d, "load", false, false, JS_UNDEFINED);
    dispatch_on_window(d, lev, "load");
    {   /* <body onload="..."> and window.onload = fn */
        jsval wl = js_get(d->J, js_objv(js_global(d->J)), "onload");
        if (js_is_function(wl)) {
            bool threw;
            js_call(d->J, wl, js_objv(js_global(d->J)), 1, &lev, &threw);
            if (threw)
                report_exception(d);
        }
        ensure_inline_handler(d, d->body, "load");
        for (struct listener *l = d->body->listeners; l; l = l->next)
            if (l->is_prop && !strcmp(l->type, "load")) {
                bool threw;
                js_call(d->J, l->fn, wrap(d, d->body), 1, &lev, &threw);
                if (threw)
                    report_exception(d);
            }
    }
    flush_scripts(d);
    d->dirty = true;
}
