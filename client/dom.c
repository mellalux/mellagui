/* DOM for lumen-web: tree, HTML parser, serializers, selectors. The JavaScript bindings are in dom_js.c. */
#include "dom_int.h"

#include <ctype.h>
#include <stdio.h>
#include <time.h>

/* ---- small helpers -------------------------------------------------------------------------------------------- */

char *dstrdup(const char *s)
{
    char *r = strdup(s ? s : "");
    if (!r)
        exit(1);
    return r;
}

char *dstrndup(const char *s, size_t n)
{
    char *r = malloc(n + 1);
    if (!r)
        exit(1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

void sbuf_add(struct sbuf *s, const char *d, size_t n)
{
    if (s->n + n + 1 > s->cap) {
        s->cap = s->cap * 2 > s->n + n + 1 ? s->cap * 2 : s->n + n + 256;
        s->p = realloc(s->p, s->cap);
        if (!s->p)
            exit(1);
    }
    memcpy(s->p + s->n, d, n);
    s->n += n;
    s->p[s->n] = 0;
}
void sbuf_addz(struct sbuf *s, const char *z) { sbuf_add(s, z, strlen(z)); }
void sbuf_putc(struct sbuf *s, char c) { sbuf_add(s, &c, 1); }

static int utf8_encode(uint32_t cp, char *o)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xc0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xe0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); return 3; }
    o[0] = (char)(0xf0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 63)); o[2] = (char)(0x80 | ((cp >> 6) & 63));
    o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

static const struct { const char *name; uint32_t cp; } ENT[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xa0 }, { "copy", 0xa9 },
    { "reg", 0xae }, { "deg", 0xb0 }, { "plusmn", 0xb1 }, { "middot", 0xb7 }, { "laquo", 0xab }, { "raquo", 0xbb },
    { "times", 0xd7 }, { "divide", 0xf7 }, { "auml", 0xe4 }, { "ouml", 0xf6 }, { "uuml", 0xfc }, { "otilde", 0xf5 },
    { "Auml", 0xc4 }, { "Ouml", 0xd6 }, { "Uuml", 0xdc }, { "Otilde", 0xd5 }, { "szlig", 0xdf }, { "eacute", 0xe9 },
    { "euro", 0x20ac }, { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "lsquo", 0x2018 },
    { "rsquo", 0x2019 }, { "ldquo", 0x201c }, { "rdquo", 0x201d }, { "bull", 0x2022 },
};

/* Decode character references in s[0..n) into a malloc'ed string. */
char *dom_decode(const char *s, size_t n)
{
    char *out = malloc(n + 1);
    if (!out)
        exit(1);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '&') {
            size_t j = i + 1;
            uint32_t cp = 0;
            bool ok = false;
            if (j < n && s[j] == '#') {
                j++;
                int base = 10;
                if (j < n && (s[j] == 'x' || s[j] == 'X')) { base = 16; j++; }
                size_t st = j;
                while (j < n && j - st < 8 && (base == 16 ? isxdigit((unsigned char)s[j]) : isdigit((unsigned char)s[j]))) {
                    int dg = isdigit((unsigned char)s[j]) ? s[j] - '0' : tolower(s[j]) - 'a' + 10;
                    cp = cp * (uint32_t)base + (uint32_t)dg;
                    j++;
                }
                ok = j > st && cp > 0 && cp < 0x110000;
                if (ok && j < n && s[j] == ';') j++;
            } else {
                size_t st = j;
                while (j < n && j - st < 10 && isalnum((unsigned char)s[j])) j++;
                for (size_t k = 0; k < sizeof(ENT) / sizeof(ENT[0]); k++)
                    if (strlen(ENT[k].name) == j - st && !strncmp(ENT[k].name, s + st, j - st)) {
                        cp = ENT[k].cp;
                        ok = true;
                    }
                if (ok && j < n && s[j] == ';') j++;
            }
            if (ok) {
                char b[4];
                int l = utf8_encode(cp, b);
                if (o + (size_t)l > i + (j - i) + 3) {          /* (never longer than the source text in practice) */
                    out = realloc(out, n + 16 + o);
                }
                memcpy(out + o, b, (size_t)l);
                o += (size_t)l;
                i = j - 1;
                continue;
            }
        }
        out[o++] = s[i];
    }
    out[o] = 0;
    return out;
}

static void escape_text(struct sbuf *sb, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': sbuf_addz(sb, "&amp;"); break;
        case '<': sbuf_addz(sb, "&lt;"); break;
        case '>': sbuf_addz(sb, "&gt;"); break;
        default: sbuf_putc(sb, *s);
        }
    }
}

static void escape_attr(struct sbuf *sb, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': sbuf_addz(sb, "&amp;"); break;
        case '"': sbuf_addz(sb, "&quot;"); break;
        case '<': sbuf_addz(sb, "&lt;"); break;
        case '>': sbuf_addz(sb, "&gt;"); break;
        default: sbuf_putc(sb, *s);
        }
    }
}

/* ---- nodes ------------------------------------------------------------------------------------------------------ */

struct dnode *dnode_new(struct dom *d, int type, const char *tag)
{
    struct dnode *n = calloc(1, sizeof(*n));
    if (!n)
        exit(1);
    n->type = type;
    n->dom = d;
    if (tag) {
        n->tag = dstrdup(tag);
        for (char *p = n->tag; *p; p++)
            *p = (char)tolower((unsigned char)*p);
    }
    n->all_next = d->all_nodes;
    d->all_nodes = n;
    n->id = ++d->next_id;
    n->defstate = -1;
    return n;
}

static void dnode_free(struct dnode *n)
{
    free(n->defval);
    free(n->st);
    free(n->st_before);
    free(n->st_after);
    free(n->before_txt);
    free(n->after_txt);
    free(n->tag);
    free(n->text);
    for (int i = 0; i < n->nattr; i++) {
        free(n->attrs[i].name);
        free(n->attrs[i].value);
    }
    free(n->attrs);
    for (struct listener *l = n->listeners; l;) {
        struct listener *nx = l->next;
        free(l->type);
        free(l);
        l = nx;
    }
    free(n);
}

void dom_free_nodes(struct dom *d)
{
    for (struct dnode *n = d->all_nodes; n;) {
        struct dnode *nx = n->all_next;
        dnode_free(n);
        n = nx;
    }
    d->all_nodes = NULL;
}

const char *dattr_get(const struct dnode *n, const char *name)
{
    for (int i = 0; i < n->nattr; i++)
        if (!strcmp(n->attrs[i].name, name))
            return n->attrs[i].value;
    return NULL;
}

void dattr_set(struct dnode *n, const char *name, const char *value)
{
    for (int i = 0; i < n->nattr; i++)
        if (!strcmp(n->attrs[i].name, name)) {
            free(n->attrs[i].value);
            n->attrs[i].value = dstrdup(value);
            n->dom->dirty = true;
            return;
        }
    n->attrs = realloc(n->attrs, sizeof(*n->attrs) * (size_t)(n->nattr + 1));
    if (!n->attrs)
        exit(1);
    n->attrs[n->nattr].name = dstrdup(name);
    for (char *p = n->attrs[n->nattr].name; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    n->attrs[n->nattr].value = dstrdup(value);
    n->nattr++;
    n->dom->dirty = true;
}

bool dattr_remove(struct dnode *n, const char *name)
{
    for (int i = 0; i < n->nattr; i++)
        if (!strcmp(n->attrs[i].name, name)) {
            free(n->attrs[i].name);
            free(n->attrs[i].value);
            memmove(&n->attrs[i], &n->attrs[i + 1], sizeof(*n->attrs) * (size_t)(n->nattr - i - 1));
            n->nattr--;
            n->dom->dirty = true;
            return true;
        }
    return false;
}

void dnode_detach(struct dnode *n)
{
    if (!n->parent)
        return;
    if (n->prev) n->prev->next = n->next; else n->parent->first = n->next;
    if (n->next) n->next->prev = n->prev; else n->parent->last = n->prev;
    n->parent = n->prev = n->next = NULL;
    n->dom->dirty = true;
}

bool dnode_contains(const struct dnode *a, const struct dnode *b)
{
    for (; b; b = b->parent)
        if (a == b)
            return true;
    return false;
}

/* Insert `n` before `ref` (NULL = append) under `parent`; fragments move their children. */
bool dnode_insert(struct dnode *parent, struct dnode *n, struct dnode *ref)
{
    if (n->type == DN_FRAGMENT) {
        while (n->first) {
            struct dnode *c = n->first;
            dnode_detach(c);
            dnode_insert(parent, c, ref);
        }
        return true;
    }
    if (n == ref)
        return true;
    if (dnode_contains(n, parent))
        return false;                                   /* would create a cycle */
    dnode_detach(n);
    n->parent = parent;
    n->prev = ref ? ref->prev : parent->last;
    n->next = ref;
    if (n->prev) n->prev->next = n; else parent->first = n;
    if (ref) ref->prev = n; else parent->last = n;
    parent->dom->dirty = true;
    return true;
}

struct dnode *dtext_new(struct dom *d, const char *text)
{
    struct dnode *t = dnode_new(d, DN_TEXT, NULL);
    t->text = dstrdup(text);
    return t;
}

/* ---- HTML parser -------------------------------------------------------------------------------------------------- */

static const char *const VOID_TAGS[] = { "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param",
                                          "source", "track", "wbr", NULL };

bool tag_is_void(const char *t)
{
    for (int i = 0; VOID_TAGS[i]; i++)
        if (!strcmp(VOID_TAGS[i], t))
            return true;
    return false;
}

static bool in_list(const char *t, const char *list)
{
    size_t l = strlen(t);
    for (const char *p = list; *p;) {
        size_t n = strcspn(p, " ");
        if (n == l && !strncmp(p, t, l))
            return true;
        p += n + (p[n] ? 1 : 0);
    }
    return false;
}

static bool closes_p(const char *t)
{
    return in_list(t, "address article aside blockquote div dl fieldset footer form h1 h2 h3 h4 h5 h6 header hr main nav ol p "
                      "pre section table ul li dd dt");
}

/* Does opening `tag` implicitly close the open element `open`? */
static bool implied_close(const char *open, const char *tag)
{
    if (!strcmp(open, "p"))
        return closes_p(tag);
    if (!strcmp(open, "li"))
        return !strcmp(tag, "li");
    if (!strcmp(open, "dt") || !strcmp(open, "dd"))
        return !strcmp(tag, "dt") || !strcmp(tag, "dd");
    if (!strcmp(open, "td") || !strcmp(open, "th"))
        return in_list(tag, "td th tr tbody thead tfoot");
    if (!strcmp(open, "tr"))
        return in_list(tag, "tr tbody thead tfoot");
    if (!strcmp(open, "option"))
        return !strcmp(tag, "option") || !strcmp(tag, "optgroup");
    if (!strcmp(open, "thead") || !strcmp(open, "tbody") || !strcmp(open, "tfoot"))
        return in_list(tag, "tbody thead tfoot");
    return false;
}

/* Elements that stop the search for an implied close (a <li> inside a nested <ul> must not close the outer <li>). */
static bool scope_barrier(const char *t)
{
    return in_list(t, "ul ol dl table td th tr tbody thead tfoot select div section article body html blockquote");
}

static void parse_attrs(struct dnode *el, const char *a, size_t alen)
{
    size_t i = 0;
    while (i < alen) {
        while (i < alen && (isspace((unsigned char)a[i]) || a[i] == '/'))
            i++;
        size_t ns = i;
        while (i < alen && !isspace((unsigned char)a[i]) && a[i] != '=' && a[i] != '/')
            i++;
        size_t nl = i - ns;
        while (i < alen && isspace((unsigned char)a[i]))
            i++;
        char *val = NULL;
        if (i < alen && a[i] == '=') {
            i++;
            while (i < alen && isspace((unsigned char)a[i]))
                i++;
            size_t vs, ve;
            if (i < alen && (a[i] == '"' || a[i] == '\'')) {
                char q = a[i++];
                vs = i;
                while (i < alen && a[i] != q)
                    i++;
                ve = i;
                if (i < alen) i++;
            } else {
                vs = i;
                while (i < alen && !isspace((unsigned char)a[i]))
                    i++;
                ve = i;
            }
            val = dom_decode(a + vs, ve - vs);
        }
        if (nl) {
            char *name = dstrndup(a + ns, nl);
            for (char *p = name; *p; p++)
                *p = (char)tolower((unsigned char)*p);
            if (!dattr_get(el, name))
                dattr_set(el, name, val ? val : "");
            free(name);
        }
        free(val);
    }
}

/* Parse markup into children of `root` (a DN_DOCUMENT / DN_FRAGMENT / element used as context). */
void dom_parse_into(struct dom *d, struct dnode *root, const char *src, size_t len)
{
    struct dnode *stack[256];
    int sp = 0;
    stack[sp++] = root;
    const char *s = src, *end = src + len;
    while (s < end) {
        struct dnode *cur = stack[sp - 1];
        if (*s != '<') {
            const char *e = memchr(s, '<', (size_t)(end - s));
            if (!e)
                e = end;
            char *txt = dom_decode(s, (size_t)(e - s));
            struct dnode *t = dnode_new(d, DN_TEXT, NULL);
            t->text = txt;
            dnode_insert(cur, t, NULL);
            s = e;
            continue;
        }
        if (end - s >= 4 && !strncmp(s, "<!--", 4)) {
            const char *e = NULL;
            for (const char *q = s + 4; q + 2 < end + 0 && q + 3 <= end; q++)
                if (q[0] == '-' && q[1] == '-' && q[2] == '>') { e = q; break; }
            struct dnode *c = dnode_new(d, DN_COMMENT, NULL);
            c->text = dstrndup(s + 4, e ? (size_t)(e - s - 4) : (size_t)(end - s - 4));
            dnode_insert(cur, c, NULL);
            s = e ? e + 3 : end;
            continue;
        }
        bool close = s + 1 < end && s[1] == '/';
        const char *nm = s + 1 + (close ? 1 : 0);
        if (nm >= end || !isalpha((unsigned char)*nm)) {
            if (nm < end && (*nm == '!' || *nm == '?')) {            /* doctype, processing instruction */
                const char *e = memchr(s, '>', (size_t)(end - s));
                s = e ? e + 1 : end;
            } else {
                struct dnode *t = dtext_new(d, "<");
                dnode_insert(cur, t, NULL);
                s++;
            }
            continue;
        }
        char name[32];
        size_t nl = 0;
        const char *p = nm;
        while (p < end && (isalnum((unsigned char)*p) || *p == '-' || *p == ':')) {
            if (nl < sizeof(name) - 1)
                name[nl++] = (char)tolower((unsigned char)*p);
            p++;
        }
        name[nl] = 0;
        const char *attrs = p;
        char q = 0;
        while (p < end && (q || *p != '>')) {
            if (q && *p == q) q = 0;
            else if (!q && (*p == '"' || *p == '\'')) q = *p;
            p++;
        }
        size_t alen = (size_t)(p - attrs);
        bool selfclose = alen > 0 && attrs[alen - 1] == '/';
        s = p < end ? p + 1 : end;
        if (close) {
            for (int i = sp - 1; i > 0; i--)
                if (stack[i]->tag && !strcmp(stack[i]->tag, name)) {
                    sp = i;
                    break;
                }
            continue;
        }
        /* implied end tags */
        for (int i = sp - 1; i > 0; i--) {
            const char *ot = stack[i]->tag;
            if (!ot)
                break;
            if (implied_close(ot, name)) {
                sp = i;
                break;
            }
            if (scope_barrier(ot) || !in_list(ot, "p li dd dt td th tr option tbody thead tfoot"))
                break;
        }
        cur = stack[sp - 1];
        struct dnode *el = dnode_new(d, DN_ELEMENT, name);
        parse_attrs(el, attrs, alen);
        dnode_insert(cur, el, NULL);
        if (tag_is_void(name) || selfclose) {
            continue;
        }
        if (in_list(name, "script style textarea title")) {              /* raw text up to the matching end tag */
            const char *e = s;
            size_t l = strlen(name);
            for (; e + 2 + l <= end; e++)
                if (e[0] == '<' && e[1] == '/' && !strncasecmp(e + 2, name, l))
                    break;
            if (e + 2 + l > end)
                e = end;
            if (e > s) {
                bool literal = !strcmp(name, "script") || !strcmp(name, "style");
                struct dnode *t = dnode_new(d, DN_TEXT, NULL);
                t->text = literal ? dstrndup(s, (size_t)(e - s)) : dom_decode(s, (size_t)(e - s));
                dnode_insert(el, t, NULL);
            }
            const char *gt = e < end ? memchr(e, '>', (size_t)(end - e)) : NULL;
            s = gt ? gt + 1 : end;
            continue;
        }
        if (sp < 250)
            stack[sp++] = el;
    }
}

/* ---- tree access --------------------------------------------------------------------------------------------------- */

struct dnode *dom_find_tag(struct dnode *root, const char *tag)
{
    for (struct dnode *c = root->first; c; c = c->next) {
        if (c->type != DN_ELEMENT)
            continue;
        if (!strcmp(c->tag, tag))
            return c;
        struct dnode *r = dom_find_tag(c, tag);
        if (r)
            return r;
    }
    return NULL;
}

/* After parsing: make sure document -> html -> (head, body) exists. */
void dom_normalize(struct dom *d)
{
    struct dnode *doc = d->doc;
    struct dnode *html = NULL;
    for (struct dnode *c = doc->first; c; c = c->next)
        if (c->type == DN_ELEMENT && !strcmp(c->tag, "html"))
            html = c;
    if (!html) {
        html = dnode_new(d, DN_ELEMENT, "html");
        struct dnode *c = doc->first;
        while (c) {
            struct dnode *nx = c->next;
            dnode_detach(c);
            dnode_insert(html, c, NULL);
            c = nx;
        }
        dnode_insert(doc, html, NULL);
    } else {
        /* stray elements / text next to <html> go inside it */
        for (struct dnode *c = doc->first; c;) {
            struct dnode *nx = c->next;
            if (c != html && (c->type == DN_ELEMENT || (c->type == DN_TEXT && c->text[strspn(c->text, " \t\r\n")]))) {
                dnode_detach(c);
                dnode_insert(html, c, NULL);
            }
            c = nx;
        }
    }
    struct dnode *head = NULL, *body = NULL;
    for (struct dnode *c = html->first; c; c = c->next) {
        if (c->type != DN_ELEMENT)
            continue;
        if (!head && !strcmp(c->tag, "head")) head = c;
        if (!body && !strcmp(c->tag, "body")) body = c;
    }
    if (!head) {
        head = dnode_new(d, DN_ELEMENT, "head");
        dnode_insert(html, head, html->first);
    }
    if (!body) {
        body = dnode_new(d, DN_ELEMENT, "body");
        struct dnode *c = html->first;
        while (c) {
            struct dnode *nx = c->next;
            if (c != head) {
                bool headish = c->type == DN_ELEMENT && in_list(c->tag, "title meta link style base script noscript template");
                if (headish && !body->first) {                            /* head-type elements before any content */
                    dnode_detach(c);
                    dnode_insert(head, c, NULL);
                } else {
                    dnode_detach(c);
                    dnode_insert(body, c, NULL);
                }
            }
            c = nx;
        }
        dnode_insert(html, body, NULL);
    }
    d->html = html;
    d->head = head;
    d->body = body;
}

/* ---- serializers --------------------------------------------------------------------------------------------------- */

/* Faithful markup (innerHTML / outerHTML). */
void ser_full(struct sbuf *sb, const struct dnode *n, bool inner_only)
{
    switch (n->type) {
    case DN_TEXT: {
        bool raw = n->parent && n->parent->tag && in_list(n->parent->tag, "script style");
        if (raw) sbuf_addz(sb, n->text);
        else escape_text(sb, n->text);
        return;
    }
    case DN_COMMENT:
        sbuf_addz(sb, "<!--");
        sbuf_addz(sb, n->text ? n->text : "");
        sbuf_addz(sb, "-->");
        return;
    case DN_ELEMENT:
        if (!inner_only) {
            sbuf_putc(sb, '<');
            sbuf_addz(sb, n->tag);
            for (int i = 0; i < n->nattr; i++) {
                sbuf_putc(sb, ' ');
                sbuf_addz(sb, n->attrs[i].name);
                sbuf_addz(sb, "=\"");
                escape_attr(sb, n->attrs[i].value);
                sbuf_putc(sb, '"');
            }
            sbuf_putc(sb, '>');
            if (tag_is_void(n->tag))
                return;
        }
        break;
    default:
        break;
    }
    for (const struct dnode *c = n->first; c; c = c->next)
        ser_full(sb, c, false);
    if (n->type == DN_ELEMENT && !inner_only) {
        sbuf_addz(sb, "</");
        sbuf_addz(sb, n->tag);
        sbuf_putc(sb, '>');
    }
}

void dom_text_content(struct sbuf *sb, const struct dnode *n)
{
    if (n->type == DN_TEXT) {
        sbuf_addz(sb, n->text);
        return;
    }
    if (n->type == DN_COMMENT)
        return;
    for (const struct dnode *c = n->first; c; c = c->next)
        dom_text_content(sb, c);
}

/* style="a: b; c: d": value of property `prop` (lowercase kebab) or NULL; the result is malloc'ed. */
char *style_get(const struct dnode *n, const char *prop)
{
    const char *st = dattr_get(n, "style");
    if (!st)
        return NULL;
    size_t pl = strlen(prop);
    const char *p = st;
    char *res = NULL;
    while (*p) {
        const char *semi = strchr(p, ';');
        size_t l = semi ? (size_t)(semi - p) : strlen(p);
        const char *colon = memchr(p, ':', l);
        if (colon) {
            const char *k = p;
            while (k < colon && isspace((unsigned char)*k)) k++;
            const char *ke = colon;
            while (ke > k && isspace((unsigned char)ke[-1])) ke--;
            if ((size_t)(ke - k) == pl && !strncasecmp(k, prop, pl)) {
                const char *v = colon + 1, *ve = p + l;
                while (v < ve && isspace((unsigned char)*v)) v++;
                while (ve > v && isspace((unsigned char)ve[-1])) ve--;
                free(res);
                res = dstrndup(v, (size_t)(ve - v));
            }
        }
        p += l + (semi ? 1 : 0);
    }
    return res;
}

void style_set(struct dnode *n, const char *prop, const char *value)
{
    const char *st = dattr_get(n, "style");
    struct sbuf sb = { 0 };
    size_t pl = strlen(prop);
    const char *p = st ? st : "";
    while (*p) {
        const char *semi = strchr(p, ';');
        size_t l = semi ? (size_t)(semi - p) : strlen(p);
        const char *colon = memchr(p, ':', l);
        bool same = false;
        if (colon) {
            const char *k = p;
            while (k < colon && isspace((unsigned char)*k)) k++;
            const char *ke = colon;
            while (ke > k && isspace((unsigned char)ke[-1])) ke--;
            same = (size_t)(ke - k) == pl && !strncasecmp(k, prop, pl);
        }
        if (!same && colon) {
            const char *k = p;
            while (k < p + l && isspace((unsigned char)*k)) k++;
            sbuf_add(&sb, k, l - (size_t)(k - p));
            sbuf_addz(&sb, "; ");
        }
        p += l + (semi ? 1 : 0);
    }
    if (value && *value) {
        sbuf_addz(&sb, prop);
        sbuf_addz(&sb, ": ");
        sbuf_addz(&sb, value);
        sbuf_addz(&sb, ";");
    } else if (sb.n >= 2) {
        sb.n -= 1;                                          /* drop the trailing blank */
        sb.p[sb.n] = 0;
    }
    dattr_set(n, "style", sb.p ? sb.p : "");
    free(sb.p);
}

static bool is_hidden(const struct dnode *n)
{
    if (dattr_get(n, "hidden"))
        return true;
    char *disp = style_get(n, "display");
    bool none = disp && !strcmp(disp, "none");
    free(disp);
    return none;
}

bool node_clickable(const struct dnode *n)
{
    if (dattr_get(n, "onclick"))
        return true;
    for (const struct listener *l = n->listeners; l; l = l->next)
        if (!strcmp(l->type, "click"))
            return true;
    return false;
}

/* ---- form controls ------------------------------------------------------------------------------------------- */

int widget_kind(const struct dnode *n)
{
    if (n->type != DN_ELEMENT)
        return WK_NONE;
    if (!strcmp(n->tag, "input")) {
        const char *t = dattr_get(n, "type");
        if (!t || !*t) return WK_TEXT;
        if (!strcasecmp(t, "hidden")) return WK_NONE;
        if (!strcasecmp(t, "password")) return WK_PASSWORD;
        if (!strcasecmp(t, "checkbox")) return WK_CHECKBOX;
        if (!strcasecmp(t, "radio")) return WK_RADIO;
        if (!strcasecmp(t, "submit") || !strcasecmp(t, "image")) return WK_SUBMIT;
        if (!strcasecmp(t, "reset")) return WK_RESET;
        if (!strcasecmp(t, "button") || !strcasecmp(t, "file") || !strcasecmp(t, "color") || !strcasecmp(t, "range")) return WK_BUTTON;
        return WK_TEXT;                                         /* text, search, email, url, tel, number, date ... */
    }
    if (!strcmp(n->tag, "textarea")) return WK_TEXTAREA;
    if (!strcmp(n->tag, "select")) return WK_SELECT;
    if (!strcmp(n->tag, "button")) {
        const char *t = dattr_get(n, "type");
        return t && !strcasecmp(t, "reset") ? WK_RESET : t && !strcasecmp(t, "button") ? WK_BUTTON : WK_SUBMIT;
    }
    return WK_NONE;
}

struct dnode *dom_form_of(const struct dnode *n)
{
    for (struct dnode *p = n->parent; p; p = p->parent)
        if (p->type == DN_ELEMENT && !strcmp(p->tag, "form"))
            return p;
    return NULL;
}

/* The <option>s of a <select> in document order (also inside <optgroup>): the one after `cur`, or the first. */
const struct dnode *dom_option_next(const struct dnode *sel, const struct dnode *cur)
{
    const struct dnode *c = cur ? cur : sel;
    for (;;) {
        if (c->first && !(cur && c == cur))
            c = c->first;
        else {
            while (c != sel && !c->next)
                c = c->parent;
            if (c == sel)
                return NULL;
            c = c->next;
        }
        if (c->type == DN_ELEMENT && !strcmp(c->tag, "option"))
            return c;
        cur = NULL;
    }
}

/* the text a widget shows: value of an input / textarea text, selected option label, button label */
void widget_text(const struct dnode *n, struct sbuf *out)
{
    int k = widget_kind(n);
    if (k == WK_TEXTAREA || k == WK_BUTTON || (k == WK_SUBMIT && !strcmp(n->tag, "button")) || (k == WK_RESET && !strcmp(n->tag, "button"))) {
        if (!strcmp(n->tag, "input")) {
            const char *v = dattr_get(n, "value");
            sbuf_addz(out, v && *v ? v : (n->type == DN_ELEMENT && dattr_get(n, "type") && !strcasecmp(dattr_get(n, "type"), "file") ? "Choose file" : "Button"));
        } else {
            dom_text_content(out, n);
        }
        return;
    }
    if (k == WK_SELECT) {
        const struct dnode *first = NULL, *sel = NULL;
        for (const struct dnode *o = dom_option_next(n, NULL); o; o = dom_option_next(n, o)) {
            if (!first) first = o;
            if (dattr_get(o, "selected")) sel = o;
        }
        const struct dnode *o = sel ? sel : first;
        if (o) {
            const char *lab = dattr_get(o, "label");
            if (lab) sbuf_addz(out, lab); else dom_text_content(out, o);
        }
        return;
    }
    if (k == WK_SUBMIT || k == WK_RESET) {
        const char *v = dattr_get(n, "value");
        sbuf_addz(out, v && *v ? v : k == WK_RESET ? "Reset" : "Submit");
        return;
    }
    const char *v = dattr_get(n, "value");
    if (v) sbuf_addz(out, v);
}

static void emit_widget(struct sbuf *sb, const struct dnode *n, int k)
{
    struct sbuf txt = { 0 };
    widget_text(n, &txt);
    /* labels collapse whitespace; textarea keeps its lines */
    char num[32];
    const char *types[] = { "", "text", "password", "checkbox", "radio", "button", "submit", "reset", "select", "textarea" };
    sbuf_addz(sb, "<x-w t=\"");
    sbuf_addz(sb, types[k]);
    snprintf(num, sizeof(num), "\" id=\"%d\"", n->id);
    sbuf_addz(sb, num);
    const char *size = dattr_get(n, "size"), *cols = dattr_get(n, "cols"), *rows = dattr_get(n, "rows");
    int w = k == WK_TEXTAREA ? (cols ? atoi(cols) : 20) : (size ? atoi(size) : 20);
    if (w < 4 || w > 120) w = 20;
    if (k == WK_SELECT) {
        w = 6;
        for (const struct dnode *o = dom_option_next(n, NULL); o; o = dom_option_next(n, o)) {
            struct sbuf t = { 0 };
            dom_text_content(&t, o);
            int l = t.p ? (int)strlen(t.p) : 0;
            if (l > w) w = l > 60 ? 60 : l;
            free(t.p);
        }
    }
    snprintf(num, sizeof(num), " w=\"%d\"", w);
    sbuf_addz(sb, num);
    if (k == WK_TEXTAREA) {
        snprintf(num, sizeof(num), " r=\"%d\"", rows && atoi(rows) > 0 ? (atoi(rows) > 20 ? 20 : atoi(rows)) : 3);
        sbuf_addz(sb, num);
    }
    if ((k == WK_CHECKBOX || k == WK_RADIO) && dattr_get(n, "checked"))
        sbuf_addz(sb, " c=\"1\"");
    if (dattr_get(n, "disabled"))
        sbuf_addz(sb, " d=\"1\"");
    const char *ph = dattr_get(n, "placeholder");
    if (ph && (k == WK_TEXT || k == WK_PASSWORD || k == WK_TEXTAREA)) {
        sbuf_addz(sb, " p=\"");
        escape_attr(sb, ph);
        sbuf_putc(sb, '"');
    }
    if (n->dom->focus == n && (k == WK_TEXT || k == WK_PASSWORD || k == WK_TEXTAREA)) {
        int chars = 0;
        const char *v = txt.p ? txt.p : "";
        for (int i = 0; i < n->caret && v[i]; i++)
            chars += ((unsigned char)v[i] & 0xc0) != 0x80;
        snprintf(num, sizeof(num), " ca=\"%d\"", chars);
        sbuf_addz(sb, num);
    }
    if (n->dom->focus == n)
        sbuf_addz(sb, " f=\"1\"");
    sbuf_addz(sb, " v=\"");
    escape_attr(sb, txt.p ? txt.p : "");
    sbuf_addz(sb, "\"></x-w>");
    free(txt.p);
}

/* Renderer markup: no scripts/styles, hidden elements omitted, clickable elements wrapped in js: links. */
static void ser_render(struct sbuf *sb, const struct dnode *n, bool in_link)
{
    switch (n->type) {
    case DN_TEXT:
        if (!(n->parent && n->parent->tag && in_list(n->parent->tag, "script style textarea")))
            escape_text(sb, n->text);
        return;
    case DN_ELEMENT:
        break;
    default:
        if (n->type == DN_DOCUMENT || n->type == DN_FRAGMENT)
            for (const struct dnode *c = n->first; c; c = c->next)
                ser_render(sb, c, in_link);
        return;
    }
    const char *t = n->tag;
    if (in_list(t, "script style template meta link base noscript") || is_hidden(n))
        return;
    const struct estyle *st = n->st;
    if (st && (st->display == DS_NONE || st->hidden == 1))
        return;
    if (!strcmp(t, "head")) {
        for (const struct dnode *c = n->first; c; c = c->next)
            if (c->type == DN_ELEMENT && !strcmp(c->tag, "title")) {
                sbuf_addz(sb, "<title>");
                struct sbuf tc = { 0 };
                dom_text_content(&tc, c);
                escape_text(sb, tc.p ? tc.p : "");
                free(tc.p);
                sbuf_addz(sb, "</title>");
            }
        return;
    }
    if (!strcmp(t, "title"))
        return;
    int wk = widget_kind(n);
    if (wk != WK_NONE) {
        bool o = css_open_outer(sb, st);
        emit_widget(sb, n, wk);
        if (o)
            sbuf_addz(sb, "</x-o>");
        return;
    }
    bool clickable = node_clickable(n);
    const char *href = !strcmp(t, "a") ? dattr_get(n, "href") : NULL;
    bool is_input_button = !strcmp(t, "input") && dattr_get(n, "type") && in_list(dattr_get(n, "type"), "button submit reset");
    if (is_input_button) {
        const char *v = dattr_get(n, "value");
        char b[40];
        snprintf(b, sizeof(b), "js:%d", n->id);
        sbuf_addz(sb, "<a href=\"");
        sbuf_addz(sb, clickable ? b : "js:0");
        sbuf_addz(sb, "\">[");
        escape_text(sb, v && *v ? v : "button");
        sbuf_addz(sb, "]</a>");
        return;
    }
    if (!strcmp(t, "img")) {
        static const char *const keep[] = { "src", "srcset", "data-src", "alt", "width", "height" };
        sbuf_addz(sb, "<img");
        for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
            const char *v = dattr_get(n, keep[i]);
            if (v) {
                sbuf_putc(sb, ' ');
                sbuf_addz(sb, keep[i]);
                sbuf_addz(sb, "=\"");
                escape_attr(sb, v);
                sbuf_putc(sb, '"');
            }
        }
        sbuf_putc(sb, '>');
        return;
    }
    bool wrap = clickable && !in_link;
    char link[40];
    bool is_void = tag_is_void(t);
    bool css_o = !is_void && css_open_outer(sb, st);
    if (wrap && !href) {
        snprintf(link, sizeof(link), "js:%d", n->id);
        sbuf_addz(sb, "<a href=\"");
        sbuf_addz(sb, link);
        sbuf_addz(sb, "\">");
    }
    sbuf_putc(sb, '<');
    sbuf_addz(sb, t);
    if (href) {
        sbuf_addz(sb, " href=\"");
        if (clickable) {                                    /* the click goes through the script first */
            snprintf(link, sizeof(link), "js:%d", n->id);
            sbuf_addz(sb, link);
        } else {
            escape_attr(sb, href);
        }
        sbuf_putc(sb, '"');
    }
    sbuf_putc(sb, '>');
    if (is_void)
        goto after;
    bool css_i = css_open_inner(sb, st, href != NULL || (wrap && !href));
    int label_target = 0;
    if (!strcmp(t, "label")) {                              /* clicking the label text acts on its control */
        const char *forid = dattr_get(n, "for");
        const struct dnode *c = forid ? dom_by_id(n->dom, forid) : NULL;
        for (const struct dnode *q = n->first; !c && q;) {
            if (q->type == DN_ELEMENT && widget_kind(q) != WK_NONE) c = q;
            if (q->first) { q = q->first; continue; }
            while (q && q != n && !q->next) q = q->parent;
            if (!q || q == n) break;
            q = q->next;
        }
        if (c && widget_kind(c) != WK_NONE) {
            char b[40];
            snprintf(b, sizeof(b), "<x-l id=\"%d\">", c->id);
            sbuf_addz(sb, b);
            label_target = c->id;
        }
    }
    if (n->before_txt) {
        bool b = css_open_inner(sb, n->st_before, false);
        escape_text(sb, n->before_txt);
        if (b) sbuf_addz(sb, "</x-i>");
    }
    for (const struct dnode *c = n->first; c; c = c->next)
        ser_render(sb, c, in_link || href != NULL || (wrap && !href));
    if (n->after_txt) {
        bool b = css_open_inner(sb, n->st_after, false);
        escape_text(sb, n->after_txt);
        if (b) sbuf_addz(sb, "</x-i>");
    }
    if (label_target)
        sbuf_addz(sb, "</x-l>");
    if (css_i)
        sbuf_addz(sb, "</x-i>");
    sbuf_addz(sb, "</");
    sbuf_addz(sb, t);
    sbuf_putc(sb, '>');
after:
    if (wrap && !href)
        sbuf_addz(sb, "</a>");
    if (css_o)
        sbuf_addz(sb, "</x-o>");
}

char *dom_serialize(struct dom *d, size_t *len)
{
    struct sbuf sb = { 0 };
    dom_css_apply(d);
    sbuf_addz(&sb, "<html>");
    {   /* the page background / default colour belong to the whole window, not to the <body> block */
        uint32_t bg, fg;
        bool have = css_page_bg(d, &bg, &fg);
        if (have || fg != 0xffffffffu) {
            char b[64];
            snprintf(b, sizeof(b), "<x-pg%s%s", have ? " bg=" : "", "");
            sbuf_addz(&sb, b);
            if (have) { snprintf(b, sizeof(b), "%06x", bg & 0xffffff); sbuf_addz(&sb, b); }
            if (fg != 0xffffffffu) { snprintf(b, sizeof(b), " fg=%06x", fg & 0xffffff); sbuf_addz(&sb, b); }
            sbuf_addz(&sb, ">");
            if (d->html && d->html->st) d->html->st->bg = -1;
            if (d->body && d->body->st) d->body->st->bg = -1;
        }
    }
    ser_render(&sb, d->head, false);
    ser_render(&sb, d->body, false);
    sbuf_addz(&sb, "</html>");
    if (len)
        *len = sb.n;
    return sb.p;
}

const char *dom_title(struct dom *d)
{
    free(d->title_cache);
    d->title_cache = NULL;
    struct dnode *t = dom_find_tag(d->head, "title");
    if (!t)
        t = dom_find_tag(d->doc, "title");
    struct sbuf sb = { 0 };
    if (t)
        dom_text_content(&sb, t);
    /* collapse whitespace like browsers do for document.title */
    char *r = sb.p ? sb.p : dstrdup("");
    char *o = r;
    bool sp = true;
    for (char *p = r; *p; p++) {
        if (isspace((unsigned char)*p)) {
            if (!sp) *o++ = ' ';
            sp = true;
        } else {
            *o++ = *p;
            sp = false;
        }
    }
    if (o > r && o[-1] == ' ') o--;
    *o = 0;
    d->title_cache = r;
    return r;
}

/* ---- selectors ----------------------------------------------------------------------------------------------------- */

void sel_free_simple(struct sel_simple *s)
{
    free(s->tag);
    free(s->id);
    for (int i = 0; i < s->nclasses; i++) free(s->classes[i]);
    free(s->classes);
    for (int i = 0; i < s->nattrs; i++) { free(s->attrs[i].name); free(s->attrs[i].val); }
    free(s->attrs);
    if (s->not_) { sel_free_simple(s->not_); free(s->not_); }
}

void sel_list_free(struct selector_list *l)
{
    for (int i = 0; i < l->n; i++) {
        for (int k = 0; k < l->sels[i].n; k++)
            sel_free_simple(&l->sels[i].parts[k]);
        free(l->sels[i].parts);
    }
    free(l->sels);
}

static char *ident(const char **p)
{
    const char *s = *p;
    while (isalnum((unsigned char)**p) || **p == '-' || **p == '_' || (unsigned char)**p >= 0x80 || **p == '\\')
        (*p)++;
    return dstrndup(s, (size_t)(*p - s));
}

static bool parse_compound(const char **pp, struct sel_simple *out)
{
    const char *p = *pp;
    memset(out, 0, sizeof(*out));
    bool any = false;
    if (*p == '*') {
        out->tag = dstrdup("*");
        p++;
        any = true;
    } else if (isalpha((unsigned char)*p)) {
        out->tag = ident(&p);
        for (char *c = out->tag; *c; c++) *c = (char)tolower((unsigned char)*c);
        any = true;
    }
    for (;;) {
        if (*p == '#') {
            p++;
            free(out->id);
            out->id = ident(&p);
            any = true;
        } else if (*p == '.') {
            p++;
            out->classes = realloc(out->classes, sizeof(char *) * (size_t)(out->nclasses + 1));
            out->classes[out->nclasses++] = ident(&p);
            any = true;
        } else if (*p == '[') {
            p++;
            while (isspace((unsigned char)*p)) p++;
            struct sel_attr a = { 0 };
            a.name = ident(&p);
            for (char *c = a.name; *c; c++) *c = (char)tolower((unsigned char)*c);
            while (isspace((unsigned char)*p)) p++;
            if (*p && *p != ']') {
                if (strchr("^$*~|", *p) && p[1] == '=') { a.op = *p; p += 2; }
                else if (*p == '=') { a.op = '='; p++; }
                else { free(a.name); return false; }
                while (isspace((unsigned char)*p)) p++;
                if (*p == '"' || *p == '\'') {
                    char q = *p++;
                    const char *s = p;
                    while (*p && *p != q) p++;
                    a.val = dstrndup(s, (size_t)(p - s));
                    if (*p) p++;
                } else {
                    const char *s = p;
                    while (*p && *p != ']' && !isspace((unsigned char)*p)) p++;
                    a.val = dstrndup(s, (size_t)(p - s));
                }
                while (isspace((unsigned char)*p)) p++;
            }
            if (*p != ']') { free(a.name); free(a.val); return false; }
            p++;
            out->attrs = realloc(out->attrs, sizeof(*out->attrs) * (size_t)(out->nattrs + 1));
            out->attrs[out->nattrs++] = a;
            any = true;
        } else if (*p == ':') {
            p++;
            bool dbl = *p == ':';
            if (dbl) p++;
            char *name = ident(&p);
            bool ok = true;
            if (!strcmp(name, "before")) out->pseudo_elem = 1;
            else if (!strcmp(name, "after")) out->pseudo_elem = 2;
            else if (dbl) ok = false;                           /* other pseudo-elements (::placeholder ...) never match */
            else if (!strcmp(name, "first-child")) out->pseudo |= 1;
            else if (!strcmp(name, "last-child")) out->pseudo |= 2;
            else if (!strcmp(name, "only-child")) out->pseudo |= 4;
            else if (!strcmp(name, "empty")) out->pseudo |= 8;
            else if (!strcmp(name, "root")) out->pseudo |= 16;
            else if (!strcmp(name, "checked")) out->pseudo |= 32;
            else if (!strcmp(name, "disabled")) out->pseudo |= 64;
            else if (!strcmp(name, "nth-child") && *p == '(') {
                p++;
                while (isspace((unsigned char)*p)) p++;
                if (!strncmp(p, "odd", 3)) { out->nth = -1; p += 3; }
                else if (!strncmp(p, "even", 4)) { out->nth = -2; p += 4; }
                else if (isdigit((unsigned char)*p)) out->nth = (int)strtol(p, (char **)&p, 10);
                else ok = false;
                while (isspace((unsigned char)*p)) p++;
                if (*p == ')') p++; else ok = false;
            } else if (!strcmp(name, "not") && *p == '(') {
                p++;
                struct sel_simple *inner = calloc(1, sizeof(*inner));
                if (parse_compound(&p, inner) && *p == ')') {
                    out->not_ = inner;
                    p++;
                } else {
                    sel_free_simple(inner);
                    free(inner);
                    ok = false;
                }
            } else {
                ok = false;                                     /* :hover, :focus ... */
            }
            free(name);
            if (!ok) return false;
            any = true;
        } else {
            break;
        }
    }
    *pp = p;
    return any;
}

bool sel_parse(const char *text, struct selector_list *out)
{
    memset(out, 0, sizeof(*out));
    const char *p = text;
    for (;;) {
        struct selector sel = { 0 };
        int comb = SC_NONE;
        for (;;) {
            while (isspace((unsigned char)*p)) {
                p++;
                if (comb == SC_NONE && sel.n) comb = SC_DESC;
            }
            if (*p == '>' || *p == '+' || *p == '~') {
                comb = *p == '>' ? SC_CHILD : *p == '+' ? SC_ADJ : SC_SIB;
                p++;
                while (isspace((unsigned char)*p)) p++;
            }
            if (!*p || *p == ',')
                break;
            struct sel_simple s;
            if (!parse_compound(&p, &s)) {
                sel_free_simple(&s);
                sel_list_free(out);
                for (int k = 0; k < sel.n; k++) sel_free_simple(&sel.parts[k]);
                free(sel.parts);
                memset(out, 0, sizeof(*out));
                return false;
            }
            s.combinator = sel.n ? (comb == SC_NONE ? SC_DESC : comb) : SC_NONE;
            comb = SC_NONE;
            sel.parts = realloc(sel.parts, sizeof(*sel.parts) * (size_t)(sel.n + 1));
            sel.parts[sel.n++] = s;
        }
        if (!sel.n) {
            free(sel.parts);
            sel_list_free(out);
            memset(out, 0, sizeof(*out));
            return false;
        }
        out->sels = realloc(out->sels, sizeof(*out->sels) * (size_t)(out->n + 1));
        out->sels[out->n++] = sel;
        if (*p == ',') { p++; continue; }
        break;
    }
    return true;
}

static int elem_index(const struct dnode *n)                    /* 1-based among element siblings */
{
    int i = 1;
    for (const struct dnode *p = n->prev; p; p = p->prev)
        if (p->type == DN_ELEMENT) i++;
    return i;
}

static bool class_has(const char *cls, const char *name)
{
    size_t nl = strlen(name);
    for (const char *p = cls; *p;) {
        while (isspace((unsigned char)*p)) p++;
        const char *s = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if ((size_t)(p - s) == nl && !strncmp(s, name, nl))
            return true;
    }
    return false;
}

static bool simple_matches(const struct sel_simple *s, const struct dnode *n)
{
    if (n->type != DN_ELEMENT)
        return false;
    if (s->tag && strcmp(s->tag, "*") && strcmp(s->tag, n->tag))
        return false;
    if (s->id) {
        const char *id = dattr_get(n, "id");
        if (!id || strcmp(id, s->id))
            return false;
    }
    if (s->nclasses) {
        const char *cls = dattr_get(n, "class");
        if (!cls)
            return false;
        for (int i = 0; i < s->nclasses; i++)
            if (!class_has(cls, s->classes[i]))
                return false;
    }
    for (int i = 0; i < s->nattrs; i++) {
        const char *v = dattr_get(n, s->attrs[i].name);
        if (!v)
            return false;
        const char *want = s->attrs[i].val;
        size_t vl = strlen(v), wl = want ? strlen(want) : 0;
        switch (s->attrs[i].op) {
        case 0: break;
        case '=': if (strcmp(v, want)) return false; break;
        case '^': if (!wl || strncmp(v, want, wl)) return false; break;
        case '$': if (!wl || vl < wl || strcmp(v + vl - wl, want)) return false; break;
        case '*': if (!wl || !strstr(v, want)) return false; break;
        case '~': if (!class_has(v, want)) return false; break;
        case '|': if (strncmp(v, want, wl) || (v[wl] && v[wl] != '-')) return false; break;
        }
    }
    if (s->pseudo & 1) { if (elem_index(n) != 1) return false; }
    if (s->pseudo & 2) { for (const struct dnode *p = n->next; p; p = p->next) if (p->type == DN_ELEMENT) return false; }
    if (s->pseudo & 4) {
        for (const struct dnode *p = n->parent ? n->parent->first : n; p; p = p->next)
            if (p->type == DN_ELEMENT && p != n)
                return false;
    }
    if (s->pseudo & 8) { if (n->first) return false; }
    if (s->pseudo & 16) { if (!n->parent || n->parent->type != DN_DOCUMENT) return false; }
    if (s->pseudo & 32) { if (!dattr_get(n, "checked")) return false; }
    if (s->pseudo & 64) { if (!dattr_get(n, "disabled")) return false; }
    if (s->nth) {
        int idx = elem_index(n);
        if (s->nth == -1 && idx % 2 != 1) return false;
        if (s->nth == -2 && idx % 2 != 0) return false;
        if (s->nth > 0 && idx != s->nth) return false;
    }
    if (s->not_ && simple_matches(s->not_, n))
        return false;
    return true;
}

static bool sel_match_from(const struct selector *sel, int k, const struct dnode *n)
{
    if (!simple_matches(&sel->parts[k], n))
        return false;
    if (k == 0)
        return true;
    switch (sel->parts[k].combinator) {
    case SC_CHILD:
        return n->parent && sel_match_from(sel, k - 1, n->parent);
    case SC_ADJ: {
        const struct dnode *p = n->prev;
        while (p && p->type != DN_ELEMENT) p = p->prev;
        return p && sel_match_from(sel, k - 1, p);
    }
    case SC_SIB:
        for (const struct dnode *p = n->prev; p; p = p->prev)
            if (p->type == DN_ELEMENT && sel_match_from(sel, k - 1, p))
                return true;
        return false;
    default:
        for (const struct dnode *p = n->parent; p; p = p->parent)
            if (sel_match_from(sel, k - 1, p))
                return true;
        return false;
    }
}

bool list_matches(const struct selector_list *l, const struct dnode *n)
{
    for (int i = 0; i < l->n; i++)
        if (sel_match_from(&l->sels[i], l->sels[i].n - 1, n))
            return true;
    return false;
}

bool dom_matches(const struct dnode *n, const char *selector, bool *valid)
{
    struct selector_list l;
    bool ok = sel_parse(selector, &l);
    if (valid)
        *valid = ok;
    if (!ok)
        return false;
    bool r = list_matches(&l, n);
    sel_list_free(&l);
    return r;
}

/* Collect matching descendants of `root` in document order; first_only stops at the first one. False: bad selector. */
bool dom_query(const struct dnode *root, const char *selector, bool first_only, struct dnode ***out, int *n)
{
    struct selector_list l;
    *out = NULL;
    *n = 0;
    if (!sel_parse(selector, &l))
        return false;
    int cap = 0;
    const struct dnode *c = root->first;
    while (c) {
        if (c->type == DN_ELEMENT && list_matches(&l, c)) {
            if (*n == cap) {
                cap = cap ? cap * 2 : 8;
                *out = realloc(*out, sizeof(**out) * (size_t)cap);
            }
            (*out)[(*n)++] = (struct dnode *)c;
            if (first_only)
                break;
        }
        if (c->first) { c = c->first; continue; }
        while (c && c != root && !c->next) c = c->parent;
        if (!c || c == root) break;
        c = c->next;
    }
    sel_list_free(&l);
    return true;
}

struct dnode *dom_by_id(struct dom *d, const char *id)
{
    const struct dnode *c = d->doc->first;
    while (c) {
        if (c->type == DN_ELEMENT) {
            const char *v = dattr_get(c, "id");
            if (v && !strcmp(v, id))
                return (struct dnode *)c;
        }
        if (c->first) { c = c->first; continue; }
        while (c && c != d->doc && !c->next) c = c->parent;
        if (!c || c == d->doc) break;
        c = c->next;
    }
    return NULL;
}

struct dnode *dom_node_by_numeric_id(struct dom *d, int id)
{
    for (struct dnode *n = d->all_nodes; n; n = n->all_next)
        if (n->id == id)
            return n;
    return NULL;
}

bool sel_match_one(const struct selector *sel, const struct dnode *n)
{
    return sel_match_from(sel, sel->n - 1, n);
}

int sel_specificity(const struct selector *sel)
{
    int ids = 0, cls = 0, tags = 0;
    for (int i = 0; i < sel->n; i++) {
        const struct sel_simple *s = &sel->parts[i];
        if (s->id) ids++;
        cls += s->nclasses + s->nattrs + (s->pseudo ? 1 : 0) + (s->nth ? 1 : 0);
        if (s->tag && strcmp(s->tag, "*")) tags++;
        if (s->not_) {
            if (s->not_->id) ids++;
            cls += s->not_->nclasses + s->not_->nattrs;
        }
    }
    return ids * 10000 + cls * 100 + tags;
}
