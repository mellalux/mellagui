#include "svg.h"

#include "img.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NODES 200000
#define MAX_DEPTH 48
#define MAX_POINTS 4000000
#define MAX_USE_DEPTH 12
#define MAX_STOPS 64

static void fail(char *err, size_t n, const char *msg)
{
    if (err && n)
        snprintf(err, n, "%s", msg);
}

/* ---- the XML tree ---- */
struct attr {
    char *k, *v;
};

struct node {
    char *name;
    struct attr *at;
    int nat;
    struct node **kid;
    int nkid;
    struct node *parent;
    char *text;
    size_t tlen;
};

struct doc {
    struct node *root;
    int nnodes;
    char **css;                         /* the text of every <style> */
    int ncss;
    struct rule *rules;
    int nrules;
};

struct rule {
    char *sel_tag, *sel_class, *sel_id;       /* the last compound of a selector */
    char *decl;                               /* "fill:red;stroke:blue" */
    int spec;
};

static char *sdup(const char *s, size_t n)
{
    char *o = malloc(n + 1);
    if (!o)
        return NULL;
    memcpy(o, s, n);
    o[n] = 0;
    return o;
}

static const char *attr_get(const struct node *n, const char *k)
{
    for (int i = 0; i < n->nat; i++)
        if (!strcmp(n->at[i].k, k))
            return n->at[i].v;
    return NULL;
}

static void node_free(struct node *n)
{
    if (!n)
        return;
    for (int i = 0; i < n->nkid; i++)
        node_free(n->kid[i]);
    for (int i = 0; i < n->nat; i++) {
        free(n->at[i].k);
        free(n->at[i].v);
    }
    free(n->at);
    free(n->kid);
    free(n->name);
    free(n->text);
    free(n);
}

/* Entities in an attribute value or text, decoded in place. */
static void unentity(char *s)
{
    char *o = s;
    for (char *p = s; *p;) {
        if (*p != '&') {
            *o++ = *p++;
            continue;
        }
        char *semi = strchr(p, ';');
        if (!semi || semi - p > 10) {
            *o++ = *p++;
            continue;
        }
        size_t l = (size_t)(semi - p - 1);
        const char *e = p + 1;
        if (l == 2 && !strncmp(e, "lt", 2)) *o++ = '<';
        else if (l == 2 && !strncmp(e, "gt", 2)) *o++ = '>';
        else if (l == 3 && !strncmp(e, "amp", 3)) *o++ = '&';
        else if (l == 4 && !strncmp(e, "quot", 4)) *o++ = '"';
        else if (l == 4 && !strncmp(e, "apos", 4)) *o++ = '\'';
        else if (l >= 2 && e[0] == '#') {
            unsigned long cp = e[1] == 'x' ? strtoul(e + 2, NULL, 16) : strtoul(e + 1, NULL, 10);
            if (cp > 0 && cp < 0x80)
                *o++ = (char)cp;
            else if (cp < 0x800 && cp) {
                *o++ = (char)(0xc0 | cp >> 6);
                *o++ = (char)(0x80 | (cp & 0x3f));
            } else if (cp < 0x10000 && cp) {
                *o++ = (char)(0xe0 | cp >> 12);
                *o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
                *o++ = (char)(0x80 | (cp & 0x3f));
            } else
                *o++ = '?';
        } else {
            *o++ = *p++;
            continue;
        }
        p = semi + 1;
    }
    *o = 0;
}

static const char *local_name(const char *s, size_t n, size_t *ln)
{
    const char *c = memchr(s, ':', n);
    if (c) {
        *ln = n - (size_t)(c + 1 - s);
        return c + 1;
    }
    *ln = n;
    return s;
}

/* A forgiving XML parser: elements, attributes, text of <style> and <text>-like nodes, entities, CDATA; comments, PIs and DOCTYPE skipped. */
static struct node *xml_parse(const char *buf, size_t len, struct doc *doc, char *err, size_t errn)
{
    const char *p = buf, *end = buf + len;
    struct node *root = NULL, *cur = NULL;
    int depth = 0;
    while (p < end) {
        if (*p != '<') {
            const char *q = memchr(p, '<', (size_t)(end - p));
            if (!q)
                q = end;
            if (cur && (!strcmp(cur->name, "style") || !strcmp(cur->name, "title"))) {
                cur->text = realloc(cur->text, cur->tlen + (size_t)(q - p) + 1);
                memcpy(cur->text + cur->tlen, p, (size_t)(q - p));
                cur->tlen += (size_t)(q - p);
                cur->text[cur->tlen] = 0;
            }
            p = q;
            continue;
        }
        if (end - p >= 4 && !memcmp(p, "<!--", 4)) {
            const char *q = p + 4;
            while (q + 3 <= end && memcmp(q, "-->", 3))
                q++;
            p = q + 3 <= end ? q + 3 : end;
            continue;
        }
        if (end - p >= 9 && !memcmp(p, "<![CDATA[", 9)) {
            const char *q = p + 9;
            while (q + 3 <= end && memcmp(q, "]]>", 3))
                q++;
            if (cur && !strcmp(cur->name, "style")) {
                size_t l = (size_t)(q - (p + 9));
                cur->text = realloc(cur->text, cur->tlen + l + 1);
                memcpy(cur->text + cur->tlen, p + 9, l);
                cur->tlen += l;
                cur->text[cur->tlen] = 0;
            }
            p = q + 3 <= end ? q + 3 : end;
            continue;
        }
        if (p + 1 < end && (p[1] == '?' || p[1] == '!')) {                /* a processing instruction or DOCTYPE (with an internal subset in [ ]) */
            int br = 0;
            const char *q = p + 2;
            while (q < end && (br > 0 || *q != '>')) {
                if (*q == '[') br++;
                else if (*q == ']' && br > 0) br--;
                q++;
            }
            p = q < end ? q + 1 : end;
            continue;
        }
        bool closing = p + 1 < end && p[1] == '/';
        const char *s = p + (closing ? 2 : 1), *q = s;
        char quote = 0;
        while (q < end && (quote || *q != '>')) {
            if (quote) {
                if (*q == quote) quote = 0;
            } else if (*q == '"' || *q == '\'') {
                quote = *q;
            }
            q++;
        }
        if (q >= end)
            break;
        const char *te = s;
        while (te < q && !isspace((unsigned char)*te) && *te != '/')
            te++;
        if (closing) {
            if (cur) {
                cur = cur->parent;
                depth--;
            }
            p = q + 1;
            continue;
        }
        if (++doc->nnodes > MAX_NODES || depth >= MAX_DEPTH) {
            fail(err, errn, "SVG: too big or too deeply nested");
            node_free(root);
            return NULL;
        }
        struct node *n = calloc(1, sizeof(*n));
        size_t ln;
        const char *nm = local_name(s, (size_t)(te - s), &ln);
        n->name = sdup(nm, ln);
        n->parent = cur;
        bool self = q > s && q[-1] == '/';
        const char *a = te, *ae = self ? q - 1 : q;
        while (a < ae) {                                                /* attributes */
            while (a < ae && isspace((unsigned char)*a))
                a++;
            const char *ks = a;
            while (a < ae && *a != '=' && !isspace((unsigned char)*a))
                a++;
            const char *ke = a;
            while (a < ae && isspace((unsigned char)*a))
                a++;
            if (a >= ae || *a != '=')
                break;
            a++;
            while (a < ae && isspace((unsigned char)*a))
                a++;
            if (a >= ae || (*a != '"' && *a != '\''))
                break;
            char qc = *a++;
            const char *vs = a;
            while (a < ae && *a != qc)
                a++;
            if (a >= ae)
                break;
            size_t kl;
            const char *kn = ke > ks ? local_name(ks, (size_t)(ke - ks), &kl) : ks;
            if (ke > ks && !(ke - ks > 5 && !strncmp(ks, "xmlns", 5)) && n->nat < 400) {
                n->at = realloc(n->at, (size_t)(n->nat + 1) * sizeof(struct attr));
                n->at[n->nat].k = sdup(kn, kl);
                n->at[n->nat].v = sdup(vs, (size_t)(a - vs));
                unentity(n->at[n->nat].v);
                n->nat++;
            }
            a++;
        }
        if (cur) {
            if (cur->nkid < 20000) {
                cur->kid = realloc(cur->kid, (size_t)(cur->nkid + 1) * sizeof(struct node *));
                cur->kid[cur->nkid++] = n;
            } else {
                node_free(n);
                n = NULL;
            }
        } else if (!root) {
            root = n;
        } else {
            node_free(n);
            n = NULL;
        }
        if (n && !self) {
            cur = n;
            depth++;
        }
        p = q + 1;
    }
    if (!root)
        fail(err, errn, "SVG: no element found");
    return root;
}

/* ---- CSS: rules with simple selectors ---- */
static char *trim_dup(const char *s, size_t n)
{
    while (n && isspace((unsigned char)*s)) {
        s++;
        n--;
    }
    while (n && isspace((unsigned char)s[n - 1]))
        n--;
    return sdup(s, n);
}

static void css_add_rule(struct doc *d, const char *sel, size_t sl, const char *decl, size_t dl)
{
    /* the last compound of the selector: after the last blank, '>' or '+' */
    const char *e = sel + sl, *b = sel;
    for (const char *c = sel; c < e; c++)
        if (isspace((unsigned char)*c) || *c == '>' || *c == '+' || *c == '~')
            b = c + 1;
    if (b >= e || d->nrules >= 20000)
        return;
    struct rule r = { 0 };
    const char *c = b;
    while (c < e) {
        char kind = (*c == '.' || *c == '#') ? *c : 0;
        const char *s = kind ? c + 1 : c, *t = s;
        while (t < e && *t != '.' && *t != '#' && *t != ':' && *t != '[')
            t++;
        if (t < e && (*t == ':' || *t == '['))                          /* pseudo-classes and attribute selectors: not understood, the rule is dropped */
            return;
        char *name = sdup(s, (size_t)(t - s));
        if (kind == '.') { free(r.sel_class); r.sel_class = name; r.spec += 10; }
        else if (kind == '#') { free(r.sel_id); r.sel_id = name; r.spec += 100; }
        else if (strcmp(name, "*")) { free(r.sel_tag); r.sel_tag = name; r.spec += 1; }
        else free(name);
        c = t;
    }
    r.decl = trim_dup(decl, dl);
    d->rules = realloc(d->rules, (size_t)(d->nrules + 1) * sizeof(struct rule));
    d->rules[d->nrules++] = r;
}

static void css_parse(struct doc *d, const char *css)
{
    const char *p = css;
    while (*p) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (p[0] == '/' && p[1] == '*') {
            const char *q = strstr(p + 2, "*/");
            p = q ? q + 2 : p + strlen(p);
            continue;
        }
        if (*p == '@') {                                                /* at-rules: skipped with their block */
            const char *q = p;
            while (*q && *q != '{' && *q != ';')
                q++;
            if (*q == '{') {
                int depth = 1;
                q++;
                while (*q && depth) {
                    depth += *q == '{';
                    depth -= *q == '}';
                    q++;
                }
            } else if (*q) {
                q++;
            }
            p = q;
            continue;
        }
        const char *sel = p;
        while (*p && *p != '{')
            p++;
        if (!*p)
            break;
        const char *sel_end = p;
        const char *decl = ++p;
        while (*p && *p != '}')
            p++;
        const char *decl_end = p;
        if (*p)
            p++;
        const char *s = sel;
        for (const char *c = sel; c <= sel_end; c++)
            if (c == sel_end || *c == ',') {
                css_add_rule(d, s, (size_t)(c - s), decl, (size_t)(decl_end - decl));
                s = c + 1;
            }
    }
}

static void css_collect(struct doc *d, struct node *n)
{
    if (n->name && !strcmp(n->name, "style") && n->text)
        css_parse(d, n->text);
    for (int i = 0; i < n->nkid; i++)
        css_collect(d, n->kid[i]);
}

static bool has_class(const char *classes, const char *c)
{
    size_t l = strlen(c);
    for (const char *p = classes; *p;) {
        while (*p && isspace((unsigned char)*p))
            p++;
        const char *q = p;
        while (*q && !isspace((unsigned char)*q))
            q++;
        if ((size_t)(q - p) == l && !strncmp(p, c, l))
            return true;
        p = q;
    }
    return false;
}

static bool rule_matches(const struct rule *r, const struct node *n)
{
    if (r->sel_tag && strcmp(r->sel_tag, n->name))
        return false;
    if (r->sel_class) {
        const char *c = attr_get(n, "class");
        if (!c || !has_class(c, r->sel_class))
            return false;
    }
    if (r->sel_id) {
        const char *i = attr_get(n, "id");
        if (!i || strcmp(i, r->sel_id))
            return false;
    }
    return true;
}

/* The value of the declaration `prop` in a "a:b;c:d" string, into out; false when there is none. */
static bool decl_get(const char *decl, const char *prop, char *out, size_t cap)
{
    size_t pl = strlen(prop);
    bool found = false;
    for (const char *p = decl; *p;) {
        while (*p && (isspace((unsigned char)*p) || *p == ';'))
            p++;
        const char *colon = strchr(p, ':');
        if (!colon)
            break;
        const char *semi = colon;
        while (*semi && *semi != ';')
            semi++;
        const char *ke = colon;
        while (ke > p && isspace((unsigned char)ke[-1]))
            ke--;
        if ((size_t)(ke - p) == pl && !strncmp(p, prop, pl)) {
            const char *vs = colon + 1;
            while (vs < semi && isspace((unsigned char)*vs))
                vs++;
            const char *ve = semi;
            while (ve > vs && isspace((unsigned char)ve[-1]))
                ve--;
            size_t l = (size_t)(ve - vs);
            if (l >= 10 && !strncmp(ve - 10, "!important", 10)) {
                l -= 10;
                while (l && isspace((unsigned char)vs[l - 1]))
                    l--;
            }
            if (l >= cap)
                l = cap - 1;
            memcpy(out, vs, l);
            out[l] = 0;
            found = true;                                               /* the last one wins */
        }
        p = *semi ? semi + 1 : semi;
    }
    return found;
}

/* The specified value of a property of a node: style attribute, then the matching rules (higher specificity later), then the presentation attribute. */
static bool prop_get(const struct doc *d, const struct node *n, const char *prop, char *out, size_t cap)
{
    const char *st = attr_get(n, "style");
    if (st && decl_get(st, prop, out, cap))
        return true;
    int best = -1;
    char tmp[256];
    for (int i = 0; i < d->nrules; i++) {
        const struct rule *r = &d->rules[i];
        if (r->spec >= best && rule_matches(r, n) && decl_get(r->decl, prop, tmp, sizeof(tmp))) {
            snprintf(out, cap, "%s", tmp);
            best = r->spec;
        }
    }
    if (best >= 0)
        return true;
    const char *a = attr_get(n, prop);
    if (a) {
        snprintf(out, cap, "%s", a);
        return true;
    }
    return false;
}

/* ---- numbers, lengths, colours ---- */
struct col {
    float r, g, b, a;                   /* 0..1, not premultiplied */
};

static const struct { const char *n; uint32_t c; } k_colors[] = {
    {"aliceblue",0xf0f8ff},{"antiquewhite",0xfaebd7},{"aqua",0x00ffff},{"aquamarine",0x7fffd4},{"azure",0xf0ffff},{"beige",0xf5f5dc},{"bisque",0xffe4c4},{"black",0x000000},
    {"blanchedalmond",0xffebcd},{"blue",0x0000ff},{"blueviolet",0x8a2be2},{"brown",0xa52a2a},{"burlywood",0xdeb887},{"cadetblue",0x5f9ea0},{"chartreuse",0x7fff00},
    {"chocolate",0xd2691e},{"coral",0xff7f50},{"cornflowerblue",0x6495ed},{"cornsilk",0xfff8dc},{"crimson",0xdc143c},{"cyan",0x00ffff},{"darkblue",0x00008b},
    {"darkcyan",0x008b8b},{"darkgoldenrod",0xb8860b},{"darkgray",0xa9a9a9},{"darkgreen",0x006400},{"darkgrey",0xa9a9a9},{"darkkhaki",0xbdb76b},{"darkmagenta",0x8b008b},
    {"darkolivegreen",0x556b2f},{"darkorange",0xff8c00},{"darkorchid",0x9932cc},{"darkred",0x8b0000},{"darksalmon",0xe9967a},{"darkseagreen",0x8fbc8f},
    {"darkslateblue",0x483d8b},{"darkslategray",0x2f4f4f},{"darkslategrey",0x2f4f4f},{"darkturquoise",0x00ced1},{"darkviolet",0x9400d3},{"deeppink",0xff1493},
    {"deepskyblue",0x00bfff},{"dimgray",0x696969},{"dimgrey",0x696969},{"dodgerblue",0x1e90ff},{"firebrick",0xb22222},{"floralwhite",0xfffaf0},{"forestgreen",0x228b22},
    {"fuchsia",0xff00ff},{"gainsboro",0xdcdcdc},{"ghostwhite",0xf8f8ff},{"gold",0xffd700},{"goldenrod",0xdaa520},{"gray",0x808080},{"green",0x008000},
    {"greenyellow",0xadff2f},{"grey",0x808080},{"honeydew",0xf0fff0},{"hotpink",0xff69b4},{"indianred",0xcd5c5c},{"indigo",0x4b0082},{"ivory",0xfffff0},{"khaki",0xf0e68c},
    {"lavender",0xe6e6fa},{"lavenderblush",0xfff0f5},{"lawngreen",0x7cfc00},{"lemonchiffon",0xfffacd},{"lightblue",0xadd8e6},{"lightcoral",0xf08080},{"lightcyan",0xe0ffff},
    {"lightgoldenrodyellow",0xfafad2},{"lightgray",0xd3d3d3},{"lightgreen",0x90ee90},{"lightgrey",0xd3d3d3},{"lightpink",0xffb6c1},{"lightsalmon",0xffa07a},
    {"lightseagreen",0x20b2aa},{"lightskyblue",0x87cefa},{"lightslategray",0x778899},{"lightslategrey",0x778899},{"lightsteelblue",0xb0c4de},{"lightyellow",0xffffe0},
    {"lime",0x00ff00},{"limegreen",0x32cd32},{"linen",0xfaf0e6},{"magenta",0xff00ff},{"maroon",0x800000},{"mediumaquamarine",0x66cdaa},{"mediumblue",0x0000cd},
    {"mediumorchid",0xba55d3},{"mediumpurple",0x9370db},{"mediumseagreen",0x3cb371},{"mediumslateblue",0x7b68ee},{"mediumspringgreen",0x00fa9a},{"mediumturquoise",0x48d1cc},
    {"mediumvioletred",0xc71585},{"midnightblue",0x191970},{"mintcream",0xf5fffa},{"mistyrose",0xffe4e1},{"moccasin",0xffe4b5},{"navajowhite",0xffdead},{"navy",0x000080},
    {"oldlace",0xfdf5e6},{"olive",0x808000},{"olivedrab",0x6b8e23},{"orange",0xffa500},{"orangered",0xff4500},{"orchid",0xda70d6},{"palegoldenrod",0xeee8aa},
    {"palegreen",0x98fb98},{"paleturquoise",0xafeeee},{"palevioletred",0xdb7093},{"papayawhip",0xffefd5},{"peachpuff",0xffdab9},{"peru",0xcd853f},{"pink",0xffc0cb},
    {"plum",0xdda0dd},{"powderblue",0xb0e0e6},{"purple",0x800080},{"rebeccapurple",0x663399},{"red",0xff0000},{"rosybrown",0xbc8f8f},{"royalblue",0x4169e1},
    {"saddlebrown",0x8b4513},{"salmon",0xfa8072},{"sandybrown",0xf4a460},{"seagreen",0x2e8b57},{"seashell",0xfff5ee},{"sienna",0xa0522d},{"silver",0xc0c0c0},
    {"skyblue",0x87ceeb},{"slateblue",0x6a5acd},{"slategray",0x708090},{"slategrey",0x708090},{"snow",0xfffafa},{"springgreen",0x00ff7f},{"steelblue",0x4682b4},
    {"tan",0xd2b48c},{"teal",0x008080},{"thistle",0xd8bfd8},{"tomato",0xff6347},{"turquoise",0x40e0d0},{"violet",0xee82ee},{"wheat",0xf5deb3},{"white",0xffffff},
    {"whitesmoke",0xf5f5f5},{"yellow",0xffff00},{"yellowgreen",0x9acd32},
};

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void hsl_to_rgb(double h, double s, double l, float *r, float *g, float *b)
{
    h = fmod(h, 360);
    if (h < 0)
        h += 360;
    double c = (1 - fabs(2 * l - 1)) * s, x = c * (1 - fabs(fmod(h / 60, 2) - 1)), m = l - c / 2, rr = 0, gg = 0, bb = 0;
    int sec = (int)(h / 60);
    switch (sec) {
    case 0: rr = c; gg = x; break;
    case 1: rr = x; gg = c; break;
    case 2: gg = c; bb = x; break;
    case 3: gg = x; bb = c; break;
    case 4: rr = x; bb = c; break;
    default: rr = c; bb = x; break;
    }
    *r = (float)(rr + m);
    *g = (float)(gg + m);
    *b = (float)(bb + m);
}

/* A colour: false when the text is no colour. */
static bool col_parse(const char *s, struct col *c)
{
    while (isspace((unsigned char)*s))
        s++;
    c->a = 1;
    if (*s == '#') {
        int n = 0, v[8];
        for (const char *p = s + 1; isxdigit((unsigned char)*p) && n < 8; p++)
            v[n++] = isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10);
        if (n == 3 || n == 4) {
            c->r = (float)(v[0] * 17) / 255; c->g = (float)(v[1] * 17) / 255; c->b = (float)(v[2] * 17) / 255;
            if (n == 4) c->a = (float)(v[3] * 17) / 255;
            return true;
        }
        if (n == 6 || n == 8) {
            c->r = (float)(v[0] * 16 + v[1]) / 255; c->g = (float)(v[2] * 16 + v[3]) / 255; c->b = (float)(v[4] * 16 + v[5]) / 255;
            if (n == 8) c->a = (float)(v[6] * 16 + v[7]) / 255;
            return true;
        }
        return false;
    }
    if (!strncasecmp(s, "rgb", 3) || !strncasecmp(s, "hsl", 3)) {
        bool hsl = tolower((unsigned char)s[0]) == 'h';
        const char *p = strchr(s, '(');
        if (!p)
            return false;
        double v[4] = { 0, 0, 0, 1 };
        bool pct[4] = { false };
        int n = 0;
        p++;
        while (*p && *p != ')' && n < 4) {
            while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == '/'))
                p++;
            char *e;
            double x = strtod(p, &e);
            if (e == p)
                break;
            v[n] = x;
            p = e;
            if (*p == '%') {
                pct[n] = true;
                p++;
            }
            while (isalpha((unsigned char)*p))
                p++;                                                    /* deg, turn ... */
            n++;
        }
        if (n < 3)
            return false;
        if (hsl) {
            hsl_to_rgb(v[0], clampd(v[1] / 100, 0, 1), clampd(v[2] / 100, 0, 1), &c->r, &c->g, &c->b);
        } else {
            c->r = (float)clampd(pct[0] ? v[0] / 100 : v[0] / 255, 0, 1);
            c->g = (float)clampd(pct[1] ? v[1] / 100 : v[1] / 255, 0, 1);
            c->b = (float)clampd(pct[2] ? v[2] / 100 : v[2] / 255, 0, 1);
        }
        if (n == 4)
            c->a = (float)clampd(pct[3] ? v[3] / 100 : v[3], 0, 1);
        return true;
    }
    char name[32];
    size_t l = 0;
    while (isalpha((unsigned char)s[l]) && l < sizeof(name) - 1) {
        name[l] = (char)tolower((unsigned char)s[l]);
        l++;
    }
    name[l] = 0;
    if (!l)
        return false;
    if (!strcmp(name, "transparent")) {
        c->r = c->g = c->b = 0;
        c->a = 0;
        return true;
    }
    for (size_t i = 0; i < sizeof(k_colors) / sizeof(k_colors[0]); i++)
        if (!strcmp(k_colors[i].n, name)) {
            c->r = (float)((k_colors[i].c >> 16) & 255) / 255;
            c->g = (float)((k_colors[i].c >> 8) & 255) / 255;
            c->b = (float)(k_colors[i].c & 255) / 255;
            return true;
        }
    return false;
}

/* A length in user units: px, pt, pc, mm, cm, in, em, ex, % (of `ref`). */
static double len_parse(const char *s, double ref, double fsize, bool *ok)
{
    char *e;
    while (isspace((unsigned char)*s))
        s++;
    double v = strtod(s, &e);
    if (e == s) {
        if (ok)
            *ok = false;
        return 0;
    }
    if (ok)
        *ok = true;
    while (isspace((unsigned char)*e))
        e++;
    if (!strncmp(e, "pt", 2)) return v * 4.0 / 3;
    if (!strncmp(e, "pc", 2)) return v * 16;
    if (!strncmp(e, "mm", 2)) return v * 96 / 25.4;
    if (!strncmp(e, "cm", 2)) return v * 96 / 2.54;
    if (!strncmp(e, "in", 2)) return v * 96;
    if (!strncmp(e, "em", 2)) return v * fsize;
    if (!strncmp(e, "ex", 2)) return v * fsize / 2;
    if (*e == '%') return v * ref / 100;
    return v;
}

/* ---- matrices ---- */
struct mat {
    double a, b, c, d, e, f;
};

static const struct mat k_ident = { 1, 0, 0, 1, 0, 0 };

static struct mat mat_mul(struct mat m, struct mat n)                  /* m * n: n first, then m */
{
    struct mat r = {
        m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b, m.a * n.c + m.c * n.d, m.b * n.c + m.d * n.d,
        m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f,
    };
    return r;
}

static void mat_apply(const struct mat *m, double x, double y, double *ox, double *oy)
{
    *ox = m->a * x + m->c * y + m->e;
    *oy = m->b * x + m->d * y + m->f;
}

static bool mat_inv(const struct mat *m, struct mat *o)
{
    double det = m->a * m->d - m->b * m->c;
    if (fabs(det) < 1e-12)
        return false;
    o->a = m->d / det;
    o->b = -m->b / det;
    o->c = -m->c / det;
    o->d = m->a / det;
    o->e = (m->c * m->f - m->d * m->e) / det;
    o->f = (m->b * m->e - m->a * m->f) / det;
    return true;
}

static double mat_scale(const struct mat *m)
{
    return sqrt(fabs(m->a * m->d - m->b * m->c));
}

/* "translate(10 20) rotate(45) ..." */
static struct mat transform_parse(const char *s)
{
    struct mat m = k_ident;
    while (*s) {
        while (*s && (isspace((unsigned char)*s) || *s == ','))
            s++;
        const char *name = s;
        while (isalpha((unsigned char)*s))
            s++;
        size_t nl = (size_t)(s - name);
        while (isspace((unsigned char)*s))
            s++;
        if (*s != '(' || !nl)
            break;
        s++;
        double v[6] = { 0 };
        int n = 0;
        while (*s && *s != ')' && n < 6) {
            while (*s && (isspace((unsigned char)*s) || *s == ','))
                s++;
            char *e;
            double x = strtod(s, &e);
            if (e == s)
                break;
            v[n++] = x;
            s = e;
        }
        while (*s && *s != ')')
            s++;
        if (*s)
            s++;
        struct mat t = k_ident;
        if (nl == 6 && !strncmp(name, "matrix", 6) && n == 6) { t.a = v[0]; t.b = v[1]; t.c = v[2]; t.d = v[3]; t.e = v[4]; t.f = v[5]; }
        else if (nl == 9 && !strncmp(name, "translate", 9)) { t.e = v[0]; t.f = n > 1 ? v[1] : 0; }
        else if (nl == 5 && !strncmp(name, "scale", 5)) { t.a = v[0]; t.d = n > 1 ? v[1] : v[0]; }
        else if (nl == 6 && !strncmp(name, "rotate", 6)) {
            double a = v[0] * M_PI / 180, cs = cos(a), sn = sin(a);
            struct mat r = { cs, sn, -sn, cs, 0, 0 };
            if (n >= 3) {
                struct mat to = { 1, 0, 0, 1, v[1], v[2] }, back = { 1, 0, 0, 1, -v[1], -v[2] };
                r = mat_mul(to, mat_mul(r, back));
            }
            t = r;
        } else if (nl == 5 && !strncmp(name, "skewX", 5)) t.c = tan(v[0] * M_PI / 180);
        else if (nl == 5 && !strncmp(name, "skewY", 5)) t.b = tan(v[0] * M_PI / 180);
        m = mat_mul(m, t);
    }
    return m;
}

/* ---- paths: flattened into polylines in user space ---- */
struct poly {
    double *p;                          /* x, y pairs */
    int n, cap;
    bool closed;
};

struct path {
    struct poly *sub;
    int nsub, cap;
    long total;
    double minx, miny, maxx, maxy;
};

static void path_free(struct path *pa)
{
    for (int i = 0; i < pa->nsub; i++)
        free(pa->sub[i].p);
    free(pa->sub);
    memset(pa, 0, sizeof(*pa));
}

static struct poly *path_new_sub(struct path *pa)
{
    if (pa->nsub == pa->cap) {
        pa->cap = pa->cap ? pa->cap * 2 : 8;
        pa->sub = realloc(pa->sub, (size_t)pa->cap * sizeof(struct poly));
    }
    struct poly *s = &pa->sub[pa->nsub++];
    memset(s, 0, sizeof(*s));
    return s;
}

static void path_pt(struct path *pa, double x, double y)
{
    if (pa->total >= MAX_POINTS || !pa->nsub || !isfinite(x) || !isfinite(y))
        return;
    struct poly *s = &pa->sub[pa->nsub - 1];
    if (s->n && s->p[2 * s->n - 2] == x && s->p[2 * s->n - 1] == y)
        return;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->p = realloc(s->p, (size_t)s->cap * 2 * sizeof(double));
    }
    s->p[2 * s->n] = x;
    s->p[2 * s->n + 1] = y;
    s->n++;
    pa->total++;
    if (x < pa->minx) pa->minx = x;
    if (y < pa->miny) pa->miny = y;
    if (x > pa->maxx) pa->maxx = x;
    if (y > pa->maxy) pa->maxy = y;
}

static void path_move(struct path *pa, double x, double y)
{
    path_new_sub(pa);
    path_pt(pa, x, y);
}

static void path_cubic(struct path *pa, double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3, double tol)
{
    double len = hypot(x1 - x0, y1 - y0) + hypot(x2 - x1, y2 - y1) + hypot(x3 - x2, y3 - y2);
    int n = (int)(sqrt(len / tol) / 2) + 2;
    n = n > 100 ? 100 : n;
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        path_pt(pa, u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
                u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3);
    }
}

/* An elliptical arc from (x0,y0) to (x,y) (SVG's endpoint form) as cubic pieces. */
static void path_arc(struct path *pa, double x0, double y0, double rx, double ry, double rot, bool large, bool sweep, double x, double y, double tol)
{
    if (x0 == x && y0 == y)
        return;
    rx = fabs(rx);
    ry = fabs(ry);
    if (rx < 1e-9 || ry < 1e-9) {
        path_pt(pa, x, y);
        return;
    }
    double phi = rot * M_PI / 180, cp = cos(phi), sp = sin(phi);
    double dx = (x0 - x) / 2, dy = (y0 - y) / 2;
    double x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
    double lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
    if (lam > 1) {
        rx *= sqrt(lam);
        ry *= sqrt(lam);
    }
    double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1, den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double co = den > 0 ? sqrt(fmax(0, num / den)) : 0;
    if (large == sweep)
        co = -co;
    double cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
    double cx = cp * cxp - sp * cyp + (x0 + x) / 2, cy = sp * cxp + cp * cyp + (y0 + y) / 2;
    double th1 = atan2((y1 - cyp) / ry, (x1 - cxp) / rx), th2 = atan2((-y1 - cyp) / ry, (-x1 - cxp) / rx), dth = th2 - th1;
    if (sweep && dth < 0)
        dth += 2 * M_PI;
    else if (!sweep && dth > 0)
        dth -= 2 * M_PI;
    int segs = (int)ceil(fabs(dth) / (M_PI / 2) - 1e-9);
    segs = segs < 1 ? 1 : segs;
    double step = dth / segs, t = 4.0 / 3 * tan(step / 4);
    double px = x0, py = y0;
    for (int i = 0; i < segs; i++) {
        double a1 = th1 + i * step, a2 = a1 + step;
        double c1 = cos(a1), s1 = sin(a1), c2 = cos(a2), s2 = sin(a2);
        double e1x = c1 - t * s1, e1y = s1 + t * c1, e2x = c2 + t * s2, e2y = s2 - t * c2;
        double qx[3] = { e1x, e2x, c2 }, qy[3] = { e1y, e2y, s2 }, ox[3], oy[3];
        for (int k = 0; k < 3; k++) {
            double ux = qx[k] * rx, uy = qy[k] * ry;
            ox[k] = cp * ux - sp * uy + cx;
            oy[k] = sp * ux + cp * uy + cy;
        }
        if (i == segs - 1) {
            ox[2] = x;
            oy[2] = y;
        }
        path_cubic(pa, px, py, ox[0], oy[0], ox[1], oy[1], ox[2], oy[2], tol);
        px = ox[2];
        py = oy[2];
    }
}

static bool svg_num(const char **p, double *v)
{
    const char *s = *p;
    while (*s && (isspace((unsigned char)*s) || *s == ','))
        s++;
    char *e;
    double x = strtod(s, &e);
    if (e == s)
        return false;
    *v = x;
    *p = e;
    return true;
}

static bool svg_flag(const char **p, bool *v)
{
    const char *s = *p;
    while (*s && (isspace((unsigned char)*s) || *s == ','))
        s++;
    if (*s != '0' && *s != '1')
        return false;
    *v = *s == '1';
    *p = s + 1;
    return true;
}

/* The `d` attribute of a path. */
static void path_parse(struct path *pa, const char *d, double tol)
{
    double cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0;                 /* current point, subpath start, last control point */
    char cmd = 0, prev = 0;
    const char *p = d;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ','))
            p++;
        if (!*p)
            break;
        if (isalpha((unsigned char)*p)) {
            cmd = *p++;
        } else if (!cmd) {
            break;
        } else if (cmd == 'M') {
            cmd = 'L';
        } else if (cmd == 'm') {
            cmd = 'l';
        } else if (cmd == 'Z' || cmd == 'z') {
            break;
        }
        bool rel = islower((unsigned char)cmd);
        char C = (char)toupper((unsigned char)cmd);
        double v[7];
        bool ok = true;
        if (C == 'Z') {
            if (pa->nsub)
                pa->sub[pa->nsub - 1].closed = true;
            cx = sx;
            cy = sy;
            prev = 'Z';
            continue;
        }
        if (C == 'M' || C == 'L' || C == 'T') ok = svg_num(&p, &v[0]) && svg_num(&p, &v[1]);
        else if (C == 'H' || C == 'V') ok = svg_num(&p, &v[0]);
        else if (C == 'C') ok = svg_num(&p, &v[0]) && svg_num(&p, &v[1]) && svg_num(&p, &v[2]) && svg_num(&p, &v[3]) && svg_num(&p, &v[4]) && svg_num(&p, &v[5]);
        else if (C == 'S' || C == 'Q') ok = svg_num(&p, &v[0]) && svg_num(&p, &v[1]) && svg_num(&p, &v[2]) && svg_num(&p, &v[3]);
        else if (C == 'A') {
            bool la, sw;
            ok = svg_num(&p, &v[0]) && svg_num(&p, &v[1]) && svg_num(&p, &v[2]) && svg_flag(&p, &la) && svg_flag(&p, &sw) && svg_num(&p, &v[5]) && svg_num(&p, &v[6]);
            v[3] = la;
            v[4] = sw;
        } else {
            break;
        }
        if (!ok)
            break;
        if (pa->nsub == 0 && C != 'M') {
            path_move(pa, cx, cy);                                      /* a path that does not start with a move */
            sx = cx;
            sy = cy;
        }
        if (prev == 'Z' && C != 'M') {                                  /* after a close the next drawing starts at the start point */
            path_move(pa, sx, sy);
        }
        double ox = rel ? cx : 0, oy = rel ? cy : 0;
        switch (C) {
        case 'M':
            cx = v[0] + ox; cy = v[1] + oy; sx = cx; sy = cy;
            path_move(pa, cx, cy);
            break;
        case 'L':
            cx = v[0] + ox; cy = v[1] + oy;
            path_pt(pa, cx, cy);
            break;
        case 'H':
            cx = v[0] + ox;
            path_pt(pa, cx, cy);
            break;
        case 'V':
            cy = v[0] + oy;
            path_pt(pa, cx, cy);
            break;
        case 'C':
            path_cubic(pa, cx, cy, v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy, v[4] + ox, v[5] + oy, tol);
            lcx = v[2] + ox; lcy = v[3] + oy; cx = v[4] + ox; cy = v[5] + oy;
            break;
        case 'S': {
            double c1x = (prev == 'C' || prev == 'S') ? 2 * cx - lcx : cx, c1y = (prev == 'C' || prev == 'S') ? 2 * cy - lcy : cy;
            path_cubic(pa, cx, cy, c1x, c1y, v[0] + ox, v[1] + oy, v[2] + ox, v[3] + oy, tol);
            lcx = v[0] + ox; lcy = v[1] + oy; cx = v[2] + ox; cy = v[3] + oy;
            break;
        }
        case 'Q': {
            double qx = v[0] + ox, qy = v[1] + oy, ex = v[2] + ox, ey = v[3] + oy;
            path_cubic(pa, cx, cy, cx + 2.0 / 3 * (qx - cx), cy + 2.0 / 3 * (qy - cy), ex + 2.0 / 3 * (qx - ex), ey + 2.0 / 3 * (qy - ey), ex, ey, tol);
            lcx = qx; lcy = qy; cx = ex; cy = ey;
            break;
        }
        case 'T': {
            double qx = (prev == 'Q' || prev == 'T') ? 2 * cx - lcx : cx, qy = (prev == 'Q' || prev == 'T') ? 2 * cy - lcy : cy, ex = v[0] + ox, ey = v[1] + oy;
            path_cubic(pa, cx, cy, cx + 2.0 / 3 * (qx - cx), cy + 2.0 / 3 * (qy - cy), ex + 2.0 / 3 * (qx - ex), ey + 2.0 / 3 * (qy - ey), ex, ey, tol);
            lcx = qx; lcy = qy; cx = ex; cy = ey;
            break;
        }
        case 'A':
            path_arc(pa, cx, cy, v[0], v[1], v[2], v[3] != 0, v[4] != 0, v[5] + ox, v[6] + oy, tol);
            cx = v[5] + ox; cy = v[6] + oy;
            break;
        }
        prev = C;
    }
}

static void path_rect(struct path *pa, double x, double y, double w, double h, double rx, double ry, double tol)
{
    if (rx <= 0 && ry <= 0) {
        path_move(pa, x, y);
        path_pt(pa, x + w, y);
        path_pt(pa, x + w, y + h);
        path_pt(pa, x, y + h);
        pa->sub[pa->nsub - 1].closed = true;
        return;
    }
    if (rx <= 0) rx = ry;
    if (ry <= 0) ry = rx;
    rx = fmin(rx, w / 2);
    ry = fmin(ry, h / 2);
    path_move(pa, x + rx, y);
    path_pt(pa, x + w - rx, y);
    path_arc(pa, x + w - rx, y, rx, ry, 0, false, true, x + w, y + ry, tol);
    path_pt(pa, x + w, y + h - ry);
    path_arc(pa, x + w, y + h - ry, rx, ry, 0, false, true, x + w - rx, y + h, tol);
    path_pt(pa, x + rx, y + h);
    path_arc(pa, x + rx, y + h, rx, ry, 0, false, true, x, y + h - ry, tol);
    path_pt(pa, x, y + ry);
    path_arc(pa, x, y + ry, rx, ry, 0, false, true, x + rx, y, tol);
    pa->sub[pa->nsub - 1].closed = true;
}

static void path_ellipse(struct path *pa, double cx, double cy, double rx, double ry, double tol)
{
    path_move(pa, cx + rx, cy);
    path_arc(pa, cx + rx, cy, rx, ry, 0, false, true, cx, cy + ry, tol);
    path_arc(pa, cx, cy + ry, rx, ry, 0, false, true, cx - rx, cy, tol);
    path_arc(pa, cx - rx, cy, rx, ry, 0, false, true, cx, cy - ry, tol);
    path_arc(pa, cx, cy - ry, rx, ry, 0, false, true, cx + rx, cy, tol);
    pa->sub[pa->nsub - 1].closed = true;
}

/* ---- the picture being drawn: premultiplied float RGBA ---- */
struct canvas {
    int w, h;
    float *px;
};

static struct canvas *canvas_new(int w, int h)
{
    struct canvas *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->w = w;
    c->h = h;
    c->px = calloc((size_t)w * (size_t)h * 4, sizeof(float));
    if (!c->px) {
        free(c);
        return NULL;
    }
    return c;
}

static void canvas_free(struct canvas *c)
{
    if (c) {
        free(c->px);
        free(c);
    }
}

/* ---- paint, gradients ---- */
enum { P_NONE, P_COLOR, P_URL, P_CURRENT };

struct paint {
    int kind;
    struct col c;
    char url[80];
};

struct stop {
    float off;
    struct col c;
};

struct grad {
    bool radial, bbox, ok;
    double x1, y1, x2, y2, cx, cy, r, fx, fy;
    struct mat tf;
    int spread;                         /* 0 pad, 1 reflect, 2 repeat */
    struct stop st[MAX_STOPS];
    int nst;
};

struct style {
    struct paint fill, stroke;
    double fill_op, stroke_op, sw, miter, dash_off, fsize;
    double dash[32];
    int ndash, cap, join;               /* cap: 0 butt 1 round 2 square; join: 0 miter 1 round 2 bevel */
    bool evenodd;
    struct col color;
};

struct ctx {
    struct doc *doc;
    int w, h;
    float *acc;                         /* coverage accumulation, (w + 2) * h */
    struct { const char *id; const struct node *n; } *ids;
    int nids;
    int layers;
    long work;                          /* a budget so that a hostile file ends */
};

static const struct node *find_id(const struct ctx *c, const char *id)
{
    for (int i = 0; i < c->nids; i++)
        if (!strcmp(c->ids[i].id, id))
            return c->ids[i].n;
    return NULL;
}

static void ids_collect(struct ctx *c, const struct node *n)
{
    const char *id = attr_get(n, "id");
    if (id && c->nids < 20000) {
        c->ids = realloc(c->ids, (size_t)(c->nids + 1) * sizeof(c->ids[0]));
        c->ids[c->nids].id = id;
        c->ids[c->nids].n = n;
        c->nids++;
    }
    for (int i = 0; i < n->nkid; i++)
        ids_collect(c, n->kid[i]);
}

/* "url(#id)" -> the id */
static bool url_id(const char *s, char *out, size_t cap)
{
    while (isspace((unsigned char)*s))
        s++;
    if (strncmp(s, "url(", 4))
        return false;
    s += 4;
    while (isspace((unsigned char)*s) || *s == '\'' || *s == '"')
        s++;
    if (*s != '#')
        return false;
    s++;
    size_t l = 0;
    while (*s && *s != ')' && *s != '\'' && *s != '"' && !isspace((unsigned char)*s) && l < cap - 1)
        out[l++] = *s++;
    out[l] = 0;
    return l > 0;
}

static void paint_parse(const char *s, struct paint *p)
{
    while (isspace((unsigned char)*s))
        s++;
    if (!strncmp(s, "none", 4)) {
        p->kind = P_NONE;
    } else if (!strncasecmp(s, "currentcolor", 12)) {
        p->kind = P_CURRENT;
    } else if (!strncmp(s, "url(", 4)) {
        char id[80];
        if (url_id(s, id, sizeof(id))) {
            p->kind = P_URL;
            snprintf(p->url, sizeof(p->url), "%s", id);
            const char *fb = strchr(s, ')');                            /* a fallback colour after the url */
            struct col c;
            if (fb && col_parse(fb + 1, &c))
                p->c = c;
            else
                p->c = (struct col){ 0, 0, 0, 0 };
        } else {
            p->kind = P_NONE;
        }
    } else {
        struct col c;
        if (col_parse(s, &c)) {
            p->kind = P_COLOR;
            p->c = c;
        }                                                                   /* an unknown value: the inherited one stays */
    }
}

static void style_compute(const struct ctx *c, const struct node *n, const struct style *parent, struct style *st)
{
    *st = *parent;
    char v[512];
    const struct doc *d = c->doc;
    if (prop_get(d, n, "color", v, sizeof(v))) {
        struct col cc;
        if (col_parse(v, &cc))
            st->color = cc;
    }
    if (prop_get(d, n, "font-size", v, sizeof(v)))
        st->fsize = len_parse(v, parent->fsize, parent->fsize, NULL);
    if (prop_get(d, n, "fill", v, sizeof(v)) && strcmp(v, "inherit"))
        paint_parse(v, &st->fill);
    if (prop_get(d, n, "stroke", v, sizeof(v)) && strcmp(v, "inherit"))
        paint_parse(v, &st->stroke);
    if (prop_get(d, n, "fill-opacity", v, sizeof(v)))
        st->fill_op = clampd(strchr(v, '%') ? atof(v) / 100 : atof(v), 0, 1);
    if (prop_get(d, n, "stroke-opacity", v, sizeof(v)))
        st->stroke_op = clampd(strchr(v, '%') ? atof(v) / 100 : atof(v), 0, 1);
    if (prop_get(d, n, "fill-rule", v, sizeof(v)))
        st->evenodd = !strcmp(v, "evenodd");
    if (prop_get(d, n, "stroke-width", v, sizeof(v))) {
        bool ok;
        double w = len_parse(v, 100, st->fsize, &ok);
        if (ok && w >= 0)
            st->sw = w;
    }
    if (prop_get(d, n, "stroke-linecap", v, sizeof(v)))
        st->cap = !strcmp(v, "round") ? 1 : !strcmp(v, "square") ? 2 : 0;
    if (prop_get(d, n, "stroke-linejoin", v, sizeof(v)))
        st->join = !strcmp(v, "round") ? 1 : !strcmp(v, "bevel") ? 2 : 0;
    if (prop_get(d, n, "stroke-miterlimit", v, sizeof(v)))
        st->miter = fmax(1, atof(v));
    if (prop_get(d, n, "stroke-dashoffset", v, sizeof(v)))
        st->dash_off = len_parse(v, 100, st->fsize, NULL);
    if (prop_get(d, n, "stroke-dasharray", v, sizeof(v))) {
        st->ndash = 0;
        if (strcmp(v, "none")) {
            const char *p = v;
            double x, sum = 0;
            while (st->ndash < 32 && svg_num(&p, &x)) {
                while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != '-' && *p != '+' && !isdigit((unsigned char)*p) && *p != '.')
                    p++;                                                /* units */
                st->dash[st->ndash++] = x < 0 ? 0 : x;
                sum += x;
            }
            if (sum <= 1e-6)
                st->ndash = 0;
            else if (st->ndash & 1)                                     /* an odd list is repeated */
                for (int i = 0, n0 = st->ndash; i < n0 && st->ndash < 32; i++)
                    st->dash[st->ndash++] = st->dash[i];
        }
    }
}

static struct col stop_col(const struct ctx *c, const struct node *s, const struct style *st)
{
    char v[128];
    struct col col = { 0, 0, 0, 1 };
    if (prop_get(c->doc, s, "stop-color", v, sizeof(v))) {
        if (!strncasecmp(v, "currentcolor", 12))
            col = st->color;
        else
            col_parse(v, &col);
    }
    if (prop_get(c->doc, s, "stop-opacity", v, sizeof(v)))
        col.a *= (float)clampd(atof(v), 0, 1);
    return col;
}

/* A gradient element with everything it inherits through href. */
static void grad_load(const struct ctx *c, const struct node *n, struct grad *g, const struct style *st)
{
    memset(g, 0, sizeof(*g));
    g->tf = k_ident;
    g->bbox = true;
    g->radial = !strcmp(n->name, "radialGradient");
    g->x2 = 1;
    g->cx = g->cy = g->r = 0.5;
    bool have_stops = false, fx_set = false, fy_set = false;
    double sx1 = 0, sy1 = 0, sx2 = 1, sy2 = 0;
    (void)sx1; (void)sy1; (void)sx2; (void)sy2;
    const struct node *chain[8];
    int nc = 0;
    for (const struct node *cur = n; cur && nc < 8;) {
        chain[nc++] = cur;
        const char *h = attr_get(cur, "href");
        cur = (h && h[0] == '#') ? find_id(c, h + 1) : NULL;
        if (cur && strcmp(cur->name, "linearGradient") && strcmp(cur->name, "radialGradient"))
            break;
    }
    bool set_units = false, set_tf = false, set_spread = false;
    for (int i = 0; i < nc; i++) {                                      /* the nearest definition of each attribute wins */
        const struct node *a = chain[i];
        const char *v;
        bool pct_ok;
        if (!set_units && (v = attr_get(a, "gradientUnits"))) {
            g->bbox = strcmp(v, "userSpaceOnUse") != 0;
            set_units = true;
        }
        if (!set_tf && (v = attr_get(a, "gradientTransform"))) {
            g->tf = transform_parse(v);
            set_tf = true;
        }
        if (!set_spread && (v = attr_get(a, "spreadMethod"))) {
            g->spread = !strcmp(v, "reflect") ? 1 : !strcmp(v, "repeat") ? 2 : 0;
            set_spread = true;
        }
        bool rad = !strcmp(a->name, "radialGradient");
        if (rad == g->radial) {
            const char *names_l[4] = { "x1", "y1", "x2", "y2" }, *names_r[5] = { "cx", "cy", "r", "fx", "fy" };
            double *dl[4] = { &g->x1, &g->y1, &g->x2, &g->y2 }, *dr[5] = { &g->cx, &g->cy, &g->r, &g->fx, &g->fy };
            (void)dl;
            for (int k = 0; k < (rad ? 5 : 4); k++) {
                v = attr_get(a, rad ? names_r[k] : names_l[k]);
                if (!v)
                    continue;
                bool pct = strchr(v, '%') != NULL;
                double val = len_parse(v, 1, 16, &pct_ok);
                if (pct)
                    val = atof(v) / 100;
                if (rad) {
                    static bool seen[5];
                    (void)seen;
                    *dr[k] = val;
                    if (k == 3) fx_set = true;
                    if (k == 4) fy_set = true;
                } else {
                    *dl[k] = val;
                }
            }
        }
        if (!have_stops) {
            for (int k = 0; k < a->nkid && g->nst < MAX_STOPS; k++) {
                const struct node *s = a->kid[k];
                if (strcmp(s->name, "stop"))
                    continue;
                char ov[64] = "0";
                prop_get(c->doc, s, "offset", ov, sizeof(ov));
                double off = strchr(ov, '%') ? atof(ov) / 100 : atof(ov);
                g->st[g->nst].off = (float)clampd(off, 0, 1);
                g->st[g->nst].c = stop_col(c, s, st);
                if (g->nst && g->st[g->nst].off < g->st[g->nst - 1].off)
                    g->st[g->nst].off = g->st[g->nst - 1].off;
                g->nst++;
            }
            have_stops = g->nst > 0;
        }
    }
    if (g->radial) {
        if (!fx_set) g->fx = g->cx;
        if (!fy_set) g->fy = g->cy;
    }
    g->ok = g->nst > 0;
}

static struct col grad_color(const struct grad *g, double t)
{
    if (g->spread == 2)
        t = t - floor(t);
    else if (g->spread == 1) {
        t = fmod(fabs(t), 2);
        if (t > 1)
            t = 2 - t;
    }
    t = clampd(t, 0, 1);
    if (t <= g->st[0].off)
        return g->st[0].c;
    for (int i = 1; i < g->nst; i++)
        if (t <= g->st[i].off) {
            double a = g->st[i - 1].off, b = g->st[i].off, u = b > a ? (t - a) / (b - a) : 1;
            struct col c0 = g->st[i - 1].c, c1 = g->st[i].c;
            return (struct col){ (float)(c0.r + (c1.r - c0.r) * u), (float)(c0.g + (c1.g - c0.g) * u), (float)(c0.b + (c1.b - c0.b) * u), (float)(c0.a + (c1.a - c0.a) * u) };
        }
    return g->st[g->nst - 1].c;
}

/* What a fill paints: a colour, or a gradient evaluated per pixel. */
struct src {
    bool grad;
    struct col c;                       /* the colour (with its opacity in .a) */
    struct grad g;
    struct mat inv;                     /* device -> gradient space */
    float opacity;
};

static struct col src_at(const struct src *s, int x, int y)
{
    if (!s->grad)
        return s->c;
    double gx, gy;
    mat_apply(&s->inv, x + 0.5, y + 0.5, &gx, &gy);
    double t;
    if (!s->g.radial) {
        double dx = s->g.x2 - s->g.x1, dy = s->g.y2 - s->g.y1, den = dx * dx + dy * dy;
        t = den > 0 ? ((gx - s->g.x1) * dx + (gy - s->g.y1) * dy) / den : 0;
    } else {
        double r = s->g.r, ex = s->g.cx - s->g.fx, ey = s->g.cy - s->g.fy, px = gx - s->g.fx, py = gy - s->g.fy;
        double a = ex * ex + ey * ey - r * r, de = px * ex + py * ey, dd = px * px + py * py;
        if (r <= 0) {
            t = 1;
        } else if (fabs(a) < 1e-12) {
            t = dd / (2 * de + 1e-12);
        } else {
            double disc = de * de - a * dd;
            t = (de - sqrt(fmax(0, disc))) / a;
            if (a > 0 && t < 0)
                t = (de + sqrt(fmax(0, disc))) / a;
        }
    }
    struct col c = grad_color(&s->g, t);
    c.a *= s->opacity;
    return c;
}

/* ---- the rasteriser: exact area coverage by accumulating signed areas ---- */
static void acc_line(float *a, int stride, int h, double x0, double y0, double x1, double y1)
{
    if (y0 == y1)
        return;
    float dir = 1;
    if (y0 > y1) {
        double t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
        dir = -1;
    }
    if (y1 <= 0 || y0 >= h)
        return;
    double dxdy = (x1 - x0) / (y1 - y0), x = x0;
    if (y0 < 0) {
        x -= y0 * dxdy;
        y0 = 0;
    }
    int ystart = (int)y0, yend = (int)ceil(y1 < h ? y1 : h);
    for (int y = ystart; y < yend; y++) {
        float *row = a + (size_t)y * stride;
        double dy = fmin(y + 1, y1) - fmax(y, y0), xnext = x + dxdy * dy;
        float d = (float)(dy * dir);
        double xa = x < xnext ? x : xnext, xb = x < xnext ? xnext : x;
        double xaf = floor(xa);
        int xai = (int)xaf, xbi = (int)ceil(xb);
        if (xbi <= xai + 1) {
            float xmf = (float)(0.5 * (x + xnext) - xaf);
            row[xai] += d - d * xmf;
            row[xai + 1] += d * xmf;
        } else {
            double s = 1 / (xb - xa), x0f = xa - xaf, a0 = 0.5 * s * (1 - x0f) * (1 - x0f), x1f = xb - xbi + 1, am = 0.5 * s * x1f * x1f;
            row[xai] += (float)(d * a0);
            if (xbi == xai + 2) {
                row[xai + 1] += (float)(d * (1 - a0 - am));
            } else {
                double a1 = s * (1.5 - x0f);
                row[xai + 1] += (float)(d * (a1 - a0));
                for (int xi = xai + 2; xi < xbi - 1; xi++)
                    row[xi] += (float)(d * s);
                double a2 = a1 + (xbi - xai - 3) * s;
                row[xbi - 1] += (float)(d * (1 - a2 - am));
            }
            row[xbi] += (float)(d * am);
        }
        x = xnext;
    }
}

/* One edge, cut at the sides of the picture: what lies left of it is moved onto the left edge (it still counts), what lies right of it is dropped. */
static void acc_edge(float *a, int w, int h, double x0, double y0, double x1, double y1)
{
    int stride = w + 2;
    double W = w;
    if (x0 <= 0 && x1 <= 0) {
        acc_line(a, stride, h, 0, y0, 0, y1);
        return;
    }
    if (x0 >= W && x1 >= W)
        return;
    if ((x0 < 0) != (x1 < 0) || (x0 > W) != (x1 > W)) {                 /* it crosses a side: split there */
        double t = 2;
        double bound = 0;
        if ((x0 < 0) != (x1 < 0)) {
            t = (0 - x0) / (x1 - x0);
            bound = 0;
        }
        if ((x0 > W) != (x1 > W)) {
            double t2 = (W - x0) / (x1 - x0);
            if (t > 1 || t2 < t) {
                t = t2;
                bound = W;
            }
        }
        if (t > 0 && t < 1) {
            double mx = x0 + (x1 - x0) * t, my = y0 + (y1 - y0) * t;
            (void)bound;
            acc_edge(a, w, h, x0, y0, mx, my);
            acc_edge(a, w, h, mx, my, x1, y1);
            return;
        }
    }
    acc_line(a, stride, h, x0 < 0 ? 0 : x0 > W ? W : x0, y0, x1 < 0 ? 0 : x1 > W ? W : x1, y1);
}

struct ring {
    double *p;
    int n, cap;
};

struct rings {
    struct ring *r;
    int n, cap;
};

static void rings_free(struct rings *rs)
{
    for (int i = 0; i < rs->n; i++)
        free(rs->r[i].p);
    free(rs->r);
    memset(rs, 0, sizeof(*rs));
}

static struct ring *rings_add(struct rings *rs)
{
    if (rs->n == rs->cap) {
        rs->cap = rs->cap ? rs->cap * 2 : 16;
        rs->r = realloc(rs->r, (size_t)rs->cap * sizeof(struct ring));
    }
    struct ring *r = &rs->r[rs->n++];
    memset(r, 0, sizeof(*r));
    return r;
}

static void ring_pt(struct ring *r, double x, double y)
{
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 16;
        r->p = realloc(r->p, (size_t)r->cap * 2 * sizeof(double));
    }
    r->p[2 * r->n] = x;
    r->p[2 * r->n + 1] = y;
    r->n++;
}

/* Fills the rings (closed polygons in device space) into the canvas with the given paint; `mask` (or NULL) scales the coverage. */
static void raster(struct ctx *c, struct canvas *cv, const struct rings *rs, bool evenodd, const struct src *src, const float *mask)
{
    double minx = 1e30, miny = 1e30, maxx = -1e30, maxy = -1e30;
    long pts = 0;
    for (int i = 0; i < rs->n; i++)
        for (int k = 0; k < rs->r[i].n; k++) {
            double x = rs->r[i].p[2 * k], y = rs->r[i].p[2 * k + 1];
            if (!isfinite(x) || !isfinite(y))
                return;
            minx = fmin(minx, x); maxx = fmax(maxx, x); miny = fmin(miny, y); maxy = fmax(maxy, y);
            pts++;
        }
    c->work += pts;
    if (!pts || maxx < 0 || minx > cv->w || maxy < 0 || miny > cv->h)
        return;
    int w = cv->w, h = cv->h, stride = w + 2;
    int y0 = (int)fmax(0, floor(miny)), y1 = (int)fmin(h, ceil(maxy) + 1), x0 = (int)fmax(0, floor(minx)), x1 = (int)fmin(w + 1, ceil(maxx) + 2);
    for (int i = 0; i < rs->n; i++) {
        const struct ring *r = &rs->r[i];
        for (int k = 0; k < r->n; k++) {
            int k2 = k + 1 == r->n ? 0 : k + 1;
            acc_edge(c->acc, w, h, r->p[2 * k], r->p[2 * k + 1], r->p[2 * k2], r->p[2 * k2 + 1]);
        }
    }
    for (int y = y0; y < y1; y++) {
        float *row = c->acc + (size_t)y * stride, sum = 0;
        for (int x = x0; x < x1; x++) {
            sum += row[x];
            row[x] = 0;
            if (x >= w)
                continue;
            float cov = fabsf(sum);
            if (evenodd) {
                float m = fmodf(cov, 2);
                cov = m > 1 ? 2 - m : m;
            } else if (cov > 1) {
                cov = 1;
            }
            if (mask)
                cov *= mask[(size_t)y * w + x];
            if (cov < 0.002f)
                continue;
            struct col s = src_at(src, x, y);
            float a = s.a * cov;
            float *d = cv->px + ((size_t)y * w + x) * 4;
            float ia = 1 - a;
            d[0] = s.r * a + d[0] * ia;
            d[1] = s.g * a + d[1] * ia;
            d[2] = s.b * a + d[2] * ia;
            d[3] = a + d[3] * ia;
        }
        for (int x = x1; x < stride && x < x1 + 2; x++)
            row[x] = 0;
    }
}

static void ring_orient(struct ring *r)
{
    double area = 0;
    for (int k = 0; k < r->n; k++) {
        int k2 = k + 1 == r->n ? 0 : k + 1;
        area += r->p[2 * k] * r->p[2 * k2 + 1] - r->p[2 * k2] * r->p[2 * k + 1];
    }
    if (area < 0)
        for (int i = 0, j = r->n - 1; i < j; i++, j--) {
            double tx = r->p[2 * i], ty = r->p[2 * i + 1];
            r->p[2 * i] = r->p[2 * j]; r->p[2 * i + 1] = r->p[2 * j + 1];
            r->p[2 * j] = tx; r->p[2 * j + 1] = ty;
        }
}

/* ---- strokes: user space pieces (quads, joins, caps), moved to device space, all turning the same way ---- */
struct stroker {
    struct rings *out;
    const struct mat *m;
    double hw, miter;
    int cap, join;
    double devr;                        /* the radius in device pixels (for the number of steps of a circle) */
};

static void st_piece(struct stroker *s, const double *pts, int n)
{
    struct ring *r = rings_add(s->out);
    for (int i = 0; i < n; i++) {
        double x, y;
        mat_apply(s->m, pts[2 * i], pts[2 * i + 1], &x, &y);
        ring_pt(r, x, y);
    }
    ring_orient(r);
}

static void st_circle(struct stroker *s, double cx, double cy)
{
    int n = (int)clampd(8 + s->devr * 1.5, 8, 72);
    double pts[2 * 80];
    for (int i = 0; i < n; i++) {
        double a = 2 * M_PI * i / n;
        pts[2 * i] = cx + s->hw * cos(a);
        pts[2 * i + 1] = cy + s->hw * sin(a);
    }
    st_piece(s, pts, n);
}

static void stroke_polyline(struct stroker *s, const double *p, int n, bool closed)
{
    if (n < 1)
        return;
    if (n == 1) {                                                       /* a dot: only a round or a square cap shows */
        if (s->cap == 1) {
            st_circle(s, p[0], p[1]);
        } else if (s->cap == 2) {
            double q[8] = { p[0] - s->hw, p[1] - s->hw, p[0] + s->hw, p[1] - s->hw, p[0] + s->hw, p[1] + s->hw, p[0] - s->hw, p[1] + s->hw };
            st_piece(s, q, 4);
        }
        return;
    }
    int segs = closed ? n : n - 1;
    for (int i = 0; i < segs; i++) {
        const double *a = p + 2 * i, *b = p + 2 * ((i + 1) % n);
        double dx = b[0] - a[0], dy = b[1] - a[1], l = hypot(dx, dy);
        if (l < 1e-12)
            continue;
        dx /= l;
        dy /= l;
        double nx = -dy * s->hw, ny = dx * s->hw;
        double ax = a[0], ay = a[1], bx = b[0], by = b[1];
        if (!closed && s->cap == 2) {                                   /* a square cap goes on the segment itself */
            if (i == 0) { ax -= dx * s->hw; ay -= dy * s->hw; }
            if (i == segs - 1) { bx += dx * s->hw; by += dy * s->hw; }
        }
        double q[8] = { ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, ax - nx, ay - ny };
        st_piece(s, q, 4);
        bool has_next = closed || i + 1 < segs;
        if (has_next) {                                                 /* the join at b */
            const double *c = p + 2 * ((i + 2) % n);
            double ex = c[0] - b[0], ey = c[1] - b[1], el = hypot(ex, ey);
            if (el < 1e-12)
                continue;
            ex /= el;
            ey /= el;
            double cross = dx * ey - dy * ex, dot = dx * ex + dy * ey;
            if (fabs(cross) < 1e-9 && dot > 0)
                continue;
            if (s->join == 1) {
                st_circle(s, b[0], b[1]);
            } else {
                double sg = cross > 0 ? -1 : 1;                         /* the outer side */
                double o1x = b[0] + sg * nx, o1y = b[1] + sg * ny, o2x = b[0] + sg * (-ey * s->hw), o2y = b[1] + sg * (ex * s->hw);
                double tri[8] = { b[0], b[1], o1x, o1y, o2x, o2y, 0, 0 };
                int cnt = 3;
                if (s->join == 0) {
                    double cs = -dot;                                   /* cosine of the angle between the segments seen from the vertex */
                    double sinhalf = sqrt(fmax(0, (1 - cs) / 2));
                    if (sinhalf > 1e-9 && 1 / sinhalf <= s->miter) {
                        double mx = (o1x - b[0]) + (o2x - b[0]), my = (o1y - b[1]) + (o2y - b[1]), ml = hypot(mx, my);
                        if (ml > 1e-12) {
                            double tip = s->hw / sinhalf;
                            tri[6] = b[0] + mx / ml * tip;
                            tri[7] = b[1] + my / ml * tip;
                            /* the quad b, o1, tip, o2 */
                            double q4[8] = { b[0], b[1], o1x, o1y, tri[6], tri[7], o2x, o2y };
                            st_piece(s, q4, 4);
                            continue;
                        }
                    }
                }
                st_piece(s, tri, cnt);
            }
        }
    }
    if (!closed && s->cap == 1) {
        st_circle(s, p[0], p[1]);
        st_circle(s, p[2 * (n - 1)], p[2 * (n - 1) + 1]);
    }
}

/* Cuts a polyline by a dash pattern: each dash is stroked as an open line. */
static void stroke_dashed(struct stroker *s, const double *p, int n, bool closed, const double *dash, int nd, double off)
{
    int total = closed ? n + 1 : n;
    double period = 0;
    for (int i = 0; i < nd; i++)
        period += dash[i];
    if (period <= 1e-9) {
        stroke_polyline(s, p, n, closed);
        return;
    }
    off = fmod(off, period);
    if (off < 0)
        off += period;
    int di = 0;
    double remain = dash[0];
    while (off > 0) {                                                   /* start inside the pattern */
        if (off >= remain) {
            off -= remain;
            di = (di + 1) % nd;
            remain = dash[di];
        } else {
            remain -= off;
            off = 0;
        }
    }
    bool on = (di & 1) == 0;
    double *cur = malloc(sizeof(double) * 2 * (size_t)(total + 4096));
    int ncur = 0, capc = total + 4096;
    long dashes = 0;
    if (on && cur) {
        cur[0] = p[0];
        cur[1] = p[1];
        ncur = 1;
    }
    for (int i = 0; i + 1 < total && cur && dashes < 20000; i++) {
        const double *a = p + 2 * (i % n), *b = p + 2 * ((i + 1) % n);
        double dx = b[0] - a[0], dy = b[1] - a[1], l = hypot(dx, dy), pos = 0;
        if (l < 1e-12)
            continue;
        while (pos < l && dashes < 20000) {
            double take = fmin(remain, l - pos);
            pos += take;
            remain -= take;
            double x = a[0] + dx * pos / l, y = a[1] + dy * pos / l;
            if (on) {
                if (ncur >= capc) {
                    capc *= 2;
                    cur = realloc(cur, sizeof(double) * 2 * (size_t)capc);
                }
                cur[2 * ncur] = x;
                cur[2 * ncur + 1] = y;
                ncur++;
            }
            if (remain <= 1e-12) {
                if (on) {
                    stroke_polyline(s, cur, ncur, false);
                    ncur = 0;
                    dashes++;
                }
                di = (di + 1) % nd;
                remain = dash[di];
                on = (di & 1) == 0;
                if (on) {
                    cur[0] = x;
                    cur[1] = y;
                    ncur = 1;
                }
            }
        }
    }
    if (on && ncur > 1 && cur)
        stroke_polyline(s, cur, ncur, false);
    free(cur);
}

/* ---- shapes -> paths ---- */
static double num_attr(const struct ctx *c, const struct node *n, const char *k, double def, double ref, double fsize)
{
    (void)c;
    const char *v = attr_get(n, k);
    if (!v)
        return def;
    bool ok;
    double x = len_parse(v, ref, fsize, &ok);
    return ok ? x : def;
}

static bool shape_path(const struct ctx *c, const struct node *n, const struct style *st, double tol, struct path *pa)
{
    const char *nm = n->name;
    pa->minx = pa->miny = 1e30;
    pa->maxx = pa->maxy = -1e30;
    double fs = st->fsize, W = c->w, H = c->h;
    (void)W; (void)H;
    if (!strcmp(nm, "path")) {
        const char *d = attr_get(n, "d");
        if (!d)
            return false;
        path_parse(pa, d, tol);
    } else if (!strcmp(nm, "rect")) {
        double w = num_attr(c, n, "width", 0, 100, fs), h = num_attr(c, n, "height", 0, 100, fs);
        if (w <= 0 || h <= 0)
            return false;
        path_rect(pa, num_attr(c, n, "x", 0, 100, fs), num_attr(c, n, "y", 0, 100, fs), w, h, num_attr(c, n, "rx", 0, 100, fs), num_attr(c, n, "ry", 0, 100, fs), tol);
    } else if (!strcmp(nm, "circle")) {
        double r = num_attr(c, n, "r", 0, 100, fs);
        if (r <= 0)
            return false;
        path_ellipse(pa, num_attr(c, n, "cx", 0, 100, fs), num_attr(c, n, "cy", 0, 100, fs), r, r, tol);
    } else if (!strcmp(nm, "ellipse")) {
        double rx = num_attr(c, n, "rx", 0, 100, fs), ry = num_attr(c, n, "ry", 0, 100, fs);
        if (rx <= 0 || ry <= 0)
            return false;
        path_ellipse(pa, num_attr(c, n, "cx", 0, 100, fs), num_attr(c, n, "cy", 0, 100, fs), rx, ry, tol);
    } else if (!strcmp(nm, "line")) {
        path_move(pa, num_attr(c, n, "x1", 0, 100, fs), num_attr(c, n, "y1", 0, 100, fs));
        path_pt(pa, num_attr(c, n, "x2", 0, 100, fs), num_attr(c, n, "y2", 0, 100, fs));
    } else if (!strcmp(nm, "polyline") || !strcmp(nm, "polygon")) {
        const char *p = attr_get(n, "points");
        double x, y;
        bool first = true;
        while (p && svg_num(&p, &x) && svg_num(&p, &y)) {
            if (first)
                path_move(pa, x, y);
            else
                path_pt(pa, x, y);
            first = false;
        }
        if (first)
            return false;
        pa->sub[pa->nsub - 1].closed = nm[4] == 'g';
    } else {
        return false;
    }
    return pa->nsub > 0;
}

static void src_solid(struct src *s, struct col c, double opacity)
{
    memset(s, 0, sizeof(*s));
    s->c = c;
    s->c.a = (float)(c.a * opacity);
    s->c.r *= 1;                                                        /* (premultiplied in raster by `a`) */
}

/* The paint of a fill or stroke for a shape with the given bounding box; false when nothing is to be painted. */
static bool paint_src(struct ctx *c, const struct paint *p, double opacity, const struct style *st, const struct mat *ctm, const struct path *pa, struct src *s)
{
    if (p->kind == P_NONE)
        return false;
    if (p->kind == P_COLOR) {
        src_solid(s, p->c, opacity);
        return true;
    }
    if (p->kind == P_CURRENT) {
        src_solid(s, st->color, opacity);
        return true;
    }
    const struct node *gn = find_id(c, p->url);
    if (gn && (!strcmp(gn->name, "linearGradient") || !strcmp(gn->name, "radialGradient"))) {
        memset(s, 0, sizeof(*s));
        grad_load(c, gn, &s->g, st);
        if (s->g.ok) {
            struct mat m = *ctm;
            if (s->g.bbox) {
                double bw = pa->maxx - pa->minx, bh = pa->maxy - pa->miny;
                if (bw <= 1e-9 || bh <= 1e-9)
                    return false;
                struct mat bb = { bw, 0, 0, bh, pa->minx, pa->miny };
                m = mat_mul(m, bb);
            }
            m = mat_mul(m, s->g.tf);
            if (!mat_inv(&m, &s->inv))
                return false;
            s->grad = true;
            s->opacity = (float)opacity;
            return true;
        }
        if (s->g.nst == 0 && gn)                                        /* a gradient without stops paints nothing */
            return false;
    }
    if (p->c.a > 0) {
        src_solid(s, p->c, opacity);
        return true;
    }
    return false;
}

static void draw_path(struct ctx *c, struct canvas *cv, const struct path *pa, const struct mat *ctm, const struct style *st, const float *mask, bool force_fill)
{
    struct src s;
    struct rings rs = { 0 };
    if (force_fill || st->fill.kind != P_NONE) {
        if (force_fill) {
            src_solid(&s, (struct col){ 1, 1, 1, 1 }, 1);
        }
        if (force_fill || paint_src(c, &st->fill, st->fill_op, st, ctm, pa, &s)) {
            for (int i = 0; i < pa->nsub; i++) {
                const struct poly *sp = &pa->sub[i];
                if (sp->n < 3)
                    continue;
                struct ring *r = rings_add(&rs);
                for (int k = 0; k < sp->n; k++) {
                    double x, y;
                    mat_apply(ctm, sp->p[2 * k], sp->p[2 * k + 1], &x, &y);
                    ring_pt(r, x, y);
                }
            }
            raster(c, cv, &rs, st->evenodd, &s, mask);
            rings_free(&rs);
        }
    }
    if (force_fill || st->stroke.kind == P_NONE || st->sw <= 0)
        return;
    if (!paint_src(c, &st->stroke, st->stroke_op, st, ctm, pa, &s))
        return;
    double sc = mat_scale(ctm);
    struct stroker sk = { &rs, ctm, st->sw / 2, st->miter, st->cap, st->join, st->sw / 2 * sc };
    for (int i = 0; i < pa->nsub && c->work < 60000000; i++) {
        const struct poly *sp = &pa->sub[i];
        int n = 0;
        double *tmp = malloc(sizeof(double) * 2 * (size_t)(sp->n + 1));                    /* without repeated points */
        if (!tmp)
            break;
        for (int k = 0; k < sp->n; k++)
            if (!n || hypot(sp->p[2 * k] - tmp[2 * n - 2], sp->p[2 * k + 1] - tmp[2 * n - 1]) > 1e-9) {
                tmp[2 * n] = sp->p[2 * k];
                tmp[2 * n + 1] = sp->p[2 * k + 1];
                n++;
            }
        bool closed = sp->closed;
        if (closed && n > 1 && hypot(tmp[0] - tmp[2 * n - 2], tmp[1] - tmp[2 * n - 1]) < 1e-9)
            n--;
        if (st->ndash)
            stroke_dashed(&sk, tmp, n, closed, st->dash, st->ndash, st->dash_off);
        else
            stroke_polyline(&sk, tmp, n, closed && n > 2);
        free(tmp);
    }
    raster(c, cv, &rs, false, &s, mask);
    rings_free(&rs);
}

/* ---- the tree ---- */
static struct mat viewbox_ctm(const struct node *n, double vw, double vh, double *ow, double *oh)
{
    (void)ow; (void)oh;
    const char *vb = attr_get(n, "viewBox");
    struct mat m = k_ident;
    double x, y, w, h;
    if (!vb)
        return m;
    const char *p = vb;
    if (!(svg_num(&p, &x) && svg_num(&p, &y) && svg_num(&p, &w) && svg_num(&p, &h)) || w <= 0 || h <= 0)
        return m;
    const char *par = attr_get(n, "preserveAspectRatio");
    bool none = par && strstr(par, "none"), slice = par && strstr(par, "slice");
    double sx = vw / w, sy = vh / h;
    if (none) {
        m.a = sx;
        m.d = sy;
        m.e = -x * sx;
        m.f = -y * sy;
        return m;
    }
    double s = slice ? fmax(sx, sy) : fmin(sx, sy);
    double ax = 0.5, ay = 0.5;
    if (par) {
        if (strstr(par, "xMin")) ax = 0;
        else if (strstr(par, "xMax")) ax = 1;
        if (strstr(par, "YMin")) ay = 0;
        else if (strstr(par, "YMax")) ay = 1;
    }
    m.a = m.d = s;
    m.e = (vw - w * s) * ax - x * s;
    m.f = (vh - h * s) * ay - y * s;
    return m;
}

static float *clip_mask(struct ctx *c, const struct node *clip, const struct mat *ctm, const struct style *st, int depth);

static void render_node(struct ctx *c, const struct node *n, const struct style *parent, struct mat ctm, struct canvas *cv, const float *mask, int depth, int use_depth);

static void render_children(struct ctx *c, const struct node *n, const struct style *st, struct mat ctm, struct canvas *cv, const float *mask, int depth, int use_depth)
{
    for (int i = 0; i < n->nkid; i++)
        render_node(c, n->kid[i], st, ctm, cv, mask, depth + 1, use_depth);
}

static void composite_layer(struct canvas *dst, const struct canvas *layer, double opacity)
{
    size_t np = (size_t)dst->w * dst->h;
    for (size_t i = 0; i < np; i++) {
        const float *s = layer->px + i * 4;
        if (s[3] <= 0)
            continue;
        float *d = dst->px + i * 4;
        float a = (float)opacity, ia = 1 - s[3] * a;
        d[0] = s[0] * a + d[0] * ia;
        d[1] = s[1] * a + d[1] * ia;
        d[2] = s[2] * a + d[2] * ia;
        d[3] = s[3] * a + d[3] * ia;
    }
}

static void render_node(struct ctx *c, const struct node *n, const struct style *parent, struct mat ctm, struct canvas *cv, const float *mask, int depth, int use_depth)
{
    static const char *const skip[] = { "defs", "clipPath", "mask", "marker", "pattern", "symbol", "linearGradient", "radialGradient", "style", "title", "desc",
                                        "metadata", "filter", "text", "image", "script", "foreignObject", "stop", "animate", "set", "animateTransform" };
    if (depth > MAX_DEPTH || c->work > 80000000)
        return;
    for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
        if (!strcmp(n->name, skip[i]))
            return;
    char v[512];
    if (prop_get(c->doc, n, "display", v, sizeof(v)) && !strcmp(v, "none"))
        return;
    if (prop_get(c->doc, n, "visibility", v, sizeof(v)) && (!strcmp(v, "hidden") || !strcmp(v, "collapse")) && strcmp(n->name, "g") && strcmp(n->name, "svg") && strcmp(n->name, "use"))
        return;
    struct style st;
    style_compute(c, n, parent, &st);
    const char *tf = attr_get(n, "transform");
    if (!tf && prop_get(c->doc, n, "transform", v, sizeof(v)))
        tf = v;
    if (tf)
        ctm = mat_mul(ctm, transform_parse(tf));
    double opacity = 1;
    if (prop_get(c->doc, n, "opacity", v, sizeof(v)))
        opacity = clampd(strchr(v, '%') ? atof(v) / 100 : atof(v), 0, 1);
    float *own_mask = NULL;
    const float *use_mask = mask;
    if (prop_get(c->doc, n, "clip-path", v, sizeof(v))) {
        char id[80];
        const struct node *cp = url_id(v, id, sizeof(id)) ? find_id(c, id) : NULL;
        if (cp && !strcmp(cp->name, "clipPath")) {
            own_mask = clip_mask(c, cp, &ctm, &st, depth);
            if (own_mask) {
                if (mask) {
                    size_t np = (size_t)c->w * c->h;
                    for (size_t i = 0; i < np; i++)
                        own_mask[i] *= mask[i];
                }
                use_mask = own_mask;
            }
        }
    }
    struct canvas *target = cv, *layer = NULL;
    if (opacity < 0.999 && c->layers < 6) {
        layer = canvas_new(cv->w, cv->h);
        if (layer) {
            target = layer;
            c->layers++;
        }
    }
    const char *nm = n->name;
    if (!strcmp(nm, "g") || !strcmp(nm, "a") || !strcmp(nm, "svg") || !strcmp(nm, "switch")) {
        struct mat m = ctm;
        if (!strcmp(nm, "svg") && depth > 0) {                          /* a nested svg: its own viewport and viewBox */
            double x = num_attr(c, n, "x", 0, 100, st.fsize), y = num_attr(c, n, "y", 0, 100, st.fsize);
            double w = num_attr(c, n, "width", 0, 100, st.fsize), h = num_attr(c, n, "height", 0, 100, st.fsize);
            struct mat t = { 1, 0, 0, 1, x, y };
            m = mat_mul(m, t);
            if (w > 0 && h > 0)
                m = mat_mul(m, viewbox_ctm(n, w, h, NULL, NULL));
        }
        if (!strcmp(nm, "switch")) {
            if (n->nkid)
                render_node(c, n->kid[0], &st, m, target, use_mask, depth + 1, use_depth);
        } else {
            render_children(c, n, &st, m, target, use_mask, depth, use_depth);
        }
    } else if (!strcmp(nm, "use")) {
        const char *h = attr_get(n, "href");
        const struct node *ref = (h && h[0] == '#') ? find_id(c, h + 1) : NULL;
        if (ref && use_depth < MAX_USE_DEPTH) {
            struct mat t = { 1, 0, 0, 1, num_attr(c, n, "x", 0, 100, st.fsize), num_attr(c, n, "y", 0, 100, st.fsize) };
            struct mat m = mat_mul(ctm, t);
            if (!strcmp(ref->name, "symbol") || !strcmp(ref->name, "svg")) {
                double w = num_attr(c, n, "width", num_attr(c, ref, "width", 0, 100, st.fsize), 100, st.fsize);
                double hh = num_attr(c, n, "height", num_attr(c, ref, "height", 0, 100, st.fsize), 100, st.fsize);
                if (w > 0 && hh > 0)
                    m = mat_mul(m, viewbox_ctm(ref, w, hh, NULL, NULL));
                else
                    m = mat_mul(m, viewbox_ctm(ref, 100, 100, NULL, NULL));
                struct style rs;
                style_compute(c, ref, &st, &rs);
                render_children(c, ref, &rs, m, target, use_mask, depth, use_depth + 1);
            } else {
                render_node(c, ref, &st, m, target, use_mask, depth + 1, use_depth + 1);
            }
        }
    } else {
        struct path pa = { 0 };
        double sc = mat_scale(&ctm);
        double tol = 0.15 / (sc > 1e-9 ? sc : 1);
        if (shape_path(c, n, &st, tol, &pa)) {
            draw_path(c, target, &pa, &ctm, &st, use_mask, false);
            c->work += pa.total;
        }
        path_free(&pa);
    }
    if (layer) {
        composite_layer(cv, layer, opacity);
        canvas_free(layer);
        c->layers--;
    }
    free(own_mask);
}

/* The coverage of a clipPath (the union of its shapes) as a mask over the whole picture. */
static float *clip_mask(struct ctx *c, const struct node *clip, const struct mat *ctm, const struct style *st, int depth)
{
    if (depth > MAX_DEPTH)
        return NULL;
    struct canvas *cv = canvas_new(c->w, c->h);
    if (!cv)
        return NULL;
    struct mat m = *ctm;
    const char *tf = attr_get(clip, "transform");
    if (tf)
        m = mat_mul(m, transform_parse(tf));
    for (int i = 0; i < clip->nkid; i++) {
        const struct node *k = clip->kid[i];
        const struct node *shape = k;
        struct mat km = m;
        if (!strcmp(k->name, "use")) {                                  /* a use of a shape */
            const char *h = attr_get(k, "href");
            shape = (h && h[0] == '#') ? find_id(c, h + 1) : NULL;
            struct mat t = { 1, 0, 0, 1, num_attr(c, k, "x", 0, 100, st->fsize), num_attr(c, k, "y", 0, 100, st->fsize) };
            km = mat_mul(km, t);
        }
        if (!shape)
            continue;
        char v[64];
        if (prop_get(c->doc, k, "display", v, sizeof(v)) && !strcmp(v, "none"))
            continue;
        const char *tk = attr_get(k, "transform");
        if (tk)
            km = mat_mul(km, transform_parse(tk));
        if (shape != k && attr_get(shape, "transform"))
            km = mat_mul(km, transform_parse(attr_get(shape, "transform")));
        struct style ss = *st;
        ss.evenodd = prop_get(c->doc, k, "clip-rule", v, sizeof(v)) && !strcmp(v, "evenodd");
        struct path pa = { 0 };
        double sc = mat_scale(&km);
        if (shape_path(c, shape, &ss, 0.15 / (sc > 1e-9 ? sc : 1), &pa))
            draw_path(c, cv, &pa, &km, &ss, NULL, true);
        path_free(&pa);
    }
    float *mask = malloc(sizeof(float) * (size_t)c->w * c->h);
    if (mask)
        for (size_t i = 0; i < (size_t)c->w * c->h; i++)
            mask[i] = cv->px[i * 4 + 3];
    canvas_free(cv);
    return mask;
}

/* ---- the entry points ---- */
int lm_svg_sniff(const uint8_t *d, size_t len)
{
    size_t n = len < 4096 ? len : 4096;
    if (n >= 3 && d[0] == 0xef && d[1] == 0xbb && d[2] == 0xbf) {
        d += 3;
        n -= 3;
    }
    for (size_t i = 0; i + 4 <= n; i++)
        if (d[i] == '<' && d[i + 1] == 's' && d[i + 2] == 'v' && d[i + 3] == 'g' && (i + 4 == n || isspace(d[i + 4]) || d[i + 4] == '>'))
            return i < 3000;
    return 0;
}

int lm_svg_render(const uint8_t *data, size_t len, struct lm_image *img, char *err, size_t errsz)
{
    img->px = NULL;
    if (len > (64u << 20)) {
        fail(err, errsz, "SVG: the file is too big");
        return -1;
    }
    char *text = sdup((const char *)data, len);
    if (!text) {
        fail(err, errsz, "out of memory");
        return -1;
    }
    struct doc doc = { 0 };
    struct node *root = xml_parse(text, len, &doc, err, errsz);
    free(text);
    if (!root)
        return -1;
    if (strcmp(root->name, "svg")) {
        node_free(root);
        fail(err, errsz, "SVG: the root element is not <svg>");
        return -1;
    }
    doc.root = root;
    css_collect(&doc, root);
    /* the size */
    double vbx = 0, vby = 0, vbw = 0, vbh = 0;
    bool have_vb = false;
    const char *vb = attr_get(root, "viewBox");
    if (vb) {
        const char *p = vb;
        have_vb = svg_num(&p, &vbx) && svg_num(&p, &vby) && svg_num(&p, &vbw) && svg_num(&p, &vbh) && vbw > 0 && vbh > 0;
    }
    bool okw = false, okh = false;
    const char *ws = attr_get(root, "width"), *hs = attr_get(root, "height");
    double iw = ws && !strchr(ws, '%') ? len_parse(ws, 0, 16, &okw) : 0, ih = hs && !strchr(hs, '%') ? len_parse(hs, 0, 16, &okh) : 0;
    if (!okw || iw <= 0) {
        iw = have_vb ? vbw : 100;
        if (okh && ih > 0 && have_vb)
            iw = ih * vbw / vbh;
    }
    if (!okh || ih <= 0) {
        ih = have_vb ? vbh : 100;
        if (okw && have_vb)
            ih = iw * vbh / vbw;
    }
    double longer = fmax(iw, ih), scale = longer > 1024 ? 1024 / longer : longer >= 512 ? 1 : 512 / longer;
    int w = (int)clampd(floor(iw * scale + 0.5), 1, 2048), h = (int)clampd(floor(ih * scale + 0.5), 1, 2048);
    struct ctx c = { 0 };
    c.doc = &doc;
    c.w = w;
    c.h = h;
    c.acc = calloc((size_t)(w + 2) * h, sizeof(float));
    struct canvas *cv = canvas_new(w, h);
    if (!c.acc || !cv) {
        free(c.acc);
        canvas_free(cv);
        node_free(root);
        fail(err, errsz, "out of memory");
        return -1;
    }
    ids_collect(&c, root);
    struct style st;
    memset(&st, 0, sizeof(st));
    st.fill.kind = P_COLOR;
    st.fill.c = (struct col){ 0, 0, 0, 1 };
    st.fill_op = st.stroke_op = 1;
    st.sw = 1;
    st.miter = 4;
    st.fsize = 16;
    st.color = (struct col){ 0, 0, 0, 1 };
    struct mat base = { scale, 0, 0, scale, 0, 0 };
    if (have_vb)
        base = mat_mul(base, viewbox_ctm(root, iw, ih, NULL, NULL));
    struct style rs;
    style_compute(&c, root, &st, &rs);
    render_children(&c, root, &rs, base, cv, NULL, 0, 0);
    img->px = malloc((size_t)w * h * sizeof(uint32_t));
    if (!img->px) {
        free(c.acc);
        free(c.ids);
        canvas_free(cv);
        node_free(root);
        fail(err, errsz, "out of memory");
        return -1;
    }
    img->w = w;
    img->h = h;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const float *p = cv->px + i * 4;
        float a = p[3];
        unsigned A = (unsigned)clampd(a * 255 + 0.5, 0, 255), R = 0, G = 0, B = 0;
        if (a > 1e-6f) {
            R = (unsigned)clampd(p[0] / a * 255 + 0.5, 0, 255);
            G = (unsigned)clampd(p[1] / a * 255 + 0.5, 0, 255);
            B = (unsigned)clampd(p[2] / a * 255 + 0.5, 0, 255);
        }
        img->px[i] = A << 24 | R << 16 | G << 8 | B;
    }
    free(c.acc);
    free(c.ids);
    canvas_free(cv);
    for (int i = 0; i < doc.nrules; i++) {
        free(doc.rules[i].sel_tag);
        free(doc.rules[i].sel_class);
        free(doc.rules[i].sel_id);
        free(doc.rules[i].decl);
    }
    free(doc.rules);
    node_free(root);
    return 0;
}
