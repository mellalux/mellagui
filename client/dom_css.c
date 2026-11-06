/*
 * CSS for lumen-web: style sheets (<style>, <link rel=stylesheet>, @import, @media), the cascade (specificity, order,
 * !important, inline style, var()), and the author style of every element as `struct estyle`. The renderer cannot do
 * boxes, so only what a bitmap-font text layout can show is kept: colours, background colours, weight/style/underline,
 * size (1x..3x), alignment, margins/padding in whole lines or pixels, display none/block/inline, flex rows as inline
 * items, list-style none, text-transform, white-space, ::before/::after text. The result is serialized by dom.c as the
 * private tags <x-o> (outer: display, margin, background, alignment) and <x-i> (inner: character style) that lumen-web's
 * parser understands.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "dom_int.h"

#define VIEW_W 900
#define VIEW_H 600
#define MAX_SHEET_BYTES (3u << 20)
#define MAX_RULES 60000

struct css_decl { char *prop, *value; bool important; };

struct css_rule {
    struct selector sel;            /* one selector (parts owned by `list`) */
    int spec, order;
    struct css_decl *decls;
    int ndecls;
    struct css_list *list;
};

struct css_list {                   /* a parsed selector list shared by the rules made from it */
    struct selector_list sl;
    struct css_decl *decls;
    int ndecls;
};

/* Rules are looked up by the key of their rightmost compound selector: its id, else its first class, else its tag. */
struct css_bucket { char *key; int *idx; int n, cap; struct css_bucket *next; };

struct css_state {
    struct css_bucket *buckets[1024];
    struct css_bucket universal;
    bool indexed;
    struct css_rule *rules;
    int n, cap;
    struct css_list **lists;
    int nlists, caplists;
    unsigned hash;
    int order;
    int fetched;
};

/* ---- values --------------------------------------------------------------------------------------------------- */

static const struct { const char *name; uint32_t rgb; } NAMED[] = {
    { "aliceblue", 0xf0f8ff }, { "antiquewhite", 0xfaebd7 }, { "aqua", 0x00ffff }, { "aquamarine", 0x7fffd4 }, { "azure", 0xf0ffff },
    { "beige", 0xf5f5dc }, { "bisque", 0xffe4c4 }, { "black", 0x000000 }, { "blanchedalmond", 0xffebcd }, { "blue", 0x0000ff },
    { "blueviolet", 0x8a2be2 }, { "brown", 0xa52a2a }, { "burlywood", 0xdeb887 }, { "cadetblue", 0x5f9ea0 }, { "chartreuse", 0x7fff00 },
    { "chocolate", 0xd2691e }, { "coral", 0xff7f50 }, { "cornflowerblue", 0x6495ed }, { "cornsilk", 0xfff8dc }, { "crimson", 0xdc143c },
    { "cyan", 0x00ffff }, { "darkblue", 0x00008b }, { "darkcyan", 0x008b8b }, { "darkgoldenrod", 0xb8860b }, { "darkgray", 0xa9a9a9 },
    { "darkgreen", 0x006400 }, { "darkgrey", 0xa9a9a9 }, { "darkkhaki", 0xbdb76b }, { "darkmagenta", 0x8b008b }, { "darkolivegreen", 0x556b2f },
    { "darkorange", 0xff8c00 }, { "darkorchid", 0x9932cc }, { "darkred", 0x8b0000 }, { "darksalmon", 0xe9967a }, { "darkseagreen", 0x8fbc8f },
    { "darkslateblue", 0x483d8b }, { "darkslategray", 0x2f4f4f }, { "darkslategrey", 0x2f4f4f }, { "darkturquoise", 0x00ced1 },
    { "darkviolet", 0x9400d3 }, { "deeppink", 0xff1493 }, { "deepskyblue", 0x00bfff }, { "dimgray", 0x696969 }, { "dimgrey", 0x696969 },
    { "dodgerblue", 0x1e90ff }, { "firebrick", 0xb22222 }, { "floralwhite", 0xfffaf0 }, { "forestgreen", 0x228b22 }, { "fuchsia", 0xff00ff },
    { "gainsboro", 0xdcdcdc }, { "ghostwhite", 0xf8f8ff }, { "gold", 0xffd700 }, { "goldenrod", 0xdaa520 }, { "gray", 0x808080 },
    { "green", 0x008000 }, { "greenyellow", 0xadff2f }, { "grey", 0x808080 }, { "honeydew", 0xf0fff0 }, { "hotpink", 0xff69b4 },
    { "indianred", 0xcd5c5c }, { "indigo", 0x4b0082 }, { "ivory", 0xfffff0 }, { "khaki", 0xf0e68c }, { "lavender", 0xe6e6fa },
    { "lavenderblush", 0xfff0f5 }, { "lawngreen", 0x7cfc00 }, { "lemonchiffon", 0xfffacd }, { "lightblue", 0xadd8e6 }, { "lightcoral", 0xf08080 },
    { "lightcyan", 0xe0ffff }, { "lightgoldenrodyellow", 0xfafad2 }, { "lightgray", 0xd3d3d3 }, { "lightgreen", 0x90ee90 },
    { "lightgrey", 0xd3d3d3 }, { "lightpink", 0xffb6c1 }, { "lightsalmon", 0xffa07a }, { "lightseagreen", 0x20b2aa },
    { "lightskyblue", 0x87cefa }, { "lightslategray", 0x778899 }, { "lightslategrey", 0x778899 }, { "lightsteelblue", 0xb0c4de },
    { "lightyellow", 0xffffe0 }, { "lime", 0x00ff00 }, { "limegreen", 0x32cd32 }, { "linen", 0xfaf0e6 }, { "magenta", 0xff00ff },
    { "maroon", 0x800000 }, { "mediumaquamarine", 0x66cdaa }, { "mediumblue", 0x0000cd }, { "mediumorchid", 0xba55d3 },
    { "mediumpurple", 0x9370db }, { "mediumseagreen", 0x3cb371 }, { "mediumslateblue", 0x7b68ee }, { "mediumspringgreen", 0x00fa9a },
    { "mediumturquoise", 0x48d1cc }, { "mediumvioletred", 0xc71585 }, { "midnightblue", 0x191970 }, { "mintcream", 0xf5fffa },
    { "mistyrose", 0xffe4e1 }, { "moccasin", 0xffe4b5 }, { "navajowhite", 0xffdead }, { "navy", 0x000080 }, { "oldlace", 0xfdf5e6 },
    { "olive", 0x808000 }, { "olivedrab", 0x6b8e23 }, { "orange", 0xffa500 }, { "orangered", 0xff4500 }, { "orchid", 0xda70d6 },
    { "palegoldenrod", 0xeee8aa }, { "palegreen", 0x98fb98 }, { "paleturquoise", 0xafeeee }, { "palevioletred", 0xdb7093 },
    { "papayawhip", 0xffefd5 }, { "peachpuff", 0xffdab9 }, { "peru", 0xcd853f }, { "pink", 0xffc0cb }, { "plum", 0xdda0dd },
    { "powderblue", 0xb0e0e6 }, { "purple", 0x800080 }, { "rebeccapurple", 0x663399 }, { "red", 0xff0000 }, { "rosybrown", 0xbc8f8f },
    { "royalblue", 0x4169e1 }, { "saddlebrown", 0x8b4513 }, { "salmon", 0xfa8072 }, { "sandybrown", 0xf4a460 }, { "seagreen", 0x2e8b57 },
    { "seashell", 0xfff5ee }, { "sienna", 0xa0522d }, { "silver", 0xc0c0c0 }, { "skyblue", 0x87ceeb }, { "slateblue", 0x6a5acd },
    { "slategray", 0x708090 }, { "slategrey", 0x708090 }, { "snow", 0xfffafa }, { "springgreen", 0x00ff7f }, { "steelblue", 0x4682b4 },
    { "tan", 0xd2b48c }, { "teal", 0x008080 }, { "thistle", 0xd8bfd8 }, { "tomato", 0xff6347 }, { "turquoise", 0x40e0d0 },
    { "violet", 0xee82ee }, { "wheat", 0xf5deb3 }, { "white", 0xffffff }, { "whitesmoke", 0xf5f5f5 }, { "yellow", 0xffff00 },
    { "yellowgreen", 0x9acd32 },
};

static uint32_t hsl_to_rgb(double h, double s, double l)
{
    h = fmod(h, 360.0);
    if (h < 0) h += 360.0;
    h /= 360.0;
    double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    double t[3] = { h + 1.0 / 3, h, h - 1.0 / 3 };
    uint32_t c[3];
    for (int i = 0; i < 3; i++) {
        if (t[i] < 0) t[i] += 1;
        if (t[i] > 1) t[i] -= 1;
        double v = t[i] < 1.0 / 6 ? p + (q - p) * 6 * t[i] : t[i] < 0.5 ? q : t[i] < 2.0 / 3 ? p + (q - p) * (2.0 / 3 - t[i]) * 6 : p;
        c[i] = (uint32_t)(v * 255 + 0.5);
    }
    return c[0] << 16 | c[1] << 8 | c[2];
}

/* Parse a colour at the start of v. Returns chars consumed (0 = none); *rgb gets 0xRRGGBB, *transparent set for alpha 0. */
static int css_color_at(const char *v, uint32_t *rgb, bool *transparent)
{
    *transparent = false;
    while (isspace((unsigned char)*v)) v++;
    const char *st = v;
    if (*v == '#') {
        int n = 0;
        while (isxdigit((unsigned char)v[1 + n]) && n < 8) n++;
        if (n == 3 || n == 4) {
            uint32_t c = 0;
            for (int i = 0; i < 3; i++) { int d = (int)strtol((char[]){ v[1 + i], 0 }, NULL, 16); c = c << 8 | (uint32_t)(d * 17); }
            if (n == 4 && (int)strtol((char[]){ v[4], 0 }, NULL, 16) == 0) *transparent = true;
            *rgb = c;
            return (int)(v - st) + 1 + n;
        }
        if (n == 6 || n == 8) {
            char b[9];
            memcpy(b, v + 1, (size_t)n);
            b[n] = 0;
            uint32_t c = (uint32_t)strtoul(b, NULL, 16);
            if (n == 8) { if ((c & 0xff) < 8) *transparent = true; c >>= 8; }
            *rgb = c;
            return (int)(v - st) + 1 + n;
        }
        return 0;
    }
    if ((!strncasecmp(v, "rgb", 3) || !strncasecmp(v, "hsl", 3))) {
        bool hsl = tolower((unsigned char)v[0]) == 'h';
        const char *p = v + 3;
        if (tolower((unsigned char)*p) == 'a') p++;
        if (*p != '(') return 0;
        p++;
        double c[4] = { 0, 0, 0, 1 };
        bool pct[4] = { false };
        for (int i = 0; i < 4; i++) {
            while (isspace((unsigned char)*p) || *p == ',' || *p == '/') p++;
            if (*p == ')') break;
            char *e;
            c[i] = strtod(p, &e);
            if (e == p) return 0;
            p = e;
            if (*p == '%') { pct[i] = true; p++; }
            else if (!strncasecmp(p, "deg", 3)) p += 3;
        }
        while (*p && *p != ')') p++;
        if (*p != ')') return 0;
        p++;
        if (pct[3]) c[3] /= 100;
        *transparent = c[3] < 0.03;
        if (hsl) {
            *rgb = hsl_to_rgb(c[0], c[1] / 100, c[2] / 100);
        } else {
            uint32_t o[3];
            for (int i = 0; i < 3; i++) {
                double x = pct[i] ? c[i] * 2.55 : c[i];
                o[i] = (uint32_t)(x < 0 ? 0 : x > 255 ? 255 : x + 0.5);
            }
            *rgb = o[0] << 16 | o[1] << 8 | o[2];
        }
        return (int)(p - st);
    }
    const char *e = v;
    while (isalpha((unsigned char)*e)) e++;
    size_t l = (size_t)(e - v);
    if (!l || l > 24) return 0;
    if (l == 11 && !strncasecmp(v, "transparent", 11)) { *transparent = true; *rgb = 0; return (int)(e - st); }
    for (size_t i = 0; i < sizeof(NAMED) / sizeof(NAMED[0]); i++)
        if (strlen(NAMED[i].name) == l && !strncasecmp(NAMED[i].name, v, l)) {
            *rgb = NAMED[i].rgb;
            return (int)(e - st);
        }
    return 0;
}

/* A length to pixels. Returns false for auto/unknown units. */
static bool css_length(const char *v, int parent_px, double *px)
{
    while (isspace((unsigned char)*v)) v++;
    char *e;
    double x = strtod(v, &e);
    if (e == v) return false;
    if (!strncasecmp(e, "px", 2)) *px = x;
    else if (!strncasecmp(e, "rem", 3)) *px = x * 16;
    else if (!strncasecmp(e, "em", 2)) *px = x * parent_px;
    else if (!strncasecmp(e, "pt", 2)) *px = x * 4.0 / 3.0;
    else if (!strncasecmp(e, "vw", 2)) *px = x * VIEW_W / 100.0;
    else if (!strncasecmp(e, "vh", 2)) *px = x * VIEW_H / 100.0;
    else if (*e == '%') *px = x * parent_px / 100.0;
    else if (x == 0) *px = 0;
    else return false;
    return true;
}

/* ---- parsing -------------------------------------------------------------------------------------------------- */

static bool prop_supported(const char *p)
{
    static const char *const list[] = { "color", "background-color", "display", "font-weight", "font-style", "font-size",
        "text-decoration", "text-align", "text-transform", "white-space", "visibility", "opacity", "margin-top",
        "margin-bottom", "margin-left", "padding-top", "padding-bottom", "padding-left", "list-style-type", "content",
        "flex-direction", NULL };
    if (p[0] == '-' && p[1] == '-')
        return true;
    for (int i = 0; list[i]; i++)
        if (!strcmp(list[i], p))
            return true;
    return false;
}

static void decls_add(struct css_decl **arr, int *n, const char *prop, const char *value, bool important)
{
    *arr = realloc(*arr, sizeof(**arr) * (size_t)(*n + 1));
    if (!*arr)
        exit(1);
    (*arr)[*n].prop = dstrdup(prop);
    (*arr)[*n].value = dstrdup(value);
    (*arr)[*n].important = important;
    (*n)++;
}

/* split a value into whitespace separated tokens (parentheses and quotes stay together) */
static int css_tokens(const char *v, char tok[][96], int max)
{
    int n = 0;
    const char *p = v;
    while (*p && n < max) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        size_t l = 0;
        int depth = 0;
        char q = 0;
        while (*p && (depth || q || !isspace((unsigned char)*p))) {
            if (q) { if (*p == q) q = 0; }
            else if (*p == '"' || *p == '\'') q = *p;
            else if (*p == '(') depth++;
            else if (*p == ')' && depth) depth--;
            if (l < 95) tok[n][l++] = *p;
            p++;
        }
        tok[n][l] = 0;
        n++;
    }
    return n;
}

static void add_shorthand(struct css_decl **arr, int *n, const char *prop, const char *value, bool imp)
{
    char tok[8][96];
    if (strstr(value, "var(") && (!strcmp(prop, "background") || !strcmp(prop, "margin") || !strcmp(prop, "padding"))) {
        /* the value is only known after var() substitution: keep it whole (a single value is the common case) */
        if (!strcmp(prop, "background")) {
            decls_add(arr, n, "background-color", value, imp);
        } else {
            char name[40];
            snprintf(name, sizeof(name), "%s-top", prop); decls_add(arr, n, name, value, imp);
            snprintf(name, sizeof(name), "%s-bottom", prop); decls_add(arr, n, name, value, imp);
            snprintf(name, sizeof(name), "%s-left", prop); decls_add(arr, n, name, value, imp);
        }
        return;
    }
    if (!strcmp(prop, "margin") || !strcmp(prop, "padding")) {
        int c = css_tokens(value, tok, 4);
        if (!c) return;
        const char *top = tok[0], *right = c > 1 ? tok[1] : tok[0], *bottom = c > 2 ? tok[2] : tok[0], *left = c > 3 ? tok[3] : right;
        char name[40];
        snprintf(name, sizeof(name), "%s-top", prop); decls_add(arr, n, name, top, imp);
        snprintf(name, sizeof(name), "%s-bottom", prop); decls_add(arr, n, name, bottom, imp);
        snprintf(name, sizeof(name), "%s-left", prop); decls_add(arr, n, name, left, imp);
    } else if (!strcmp(prop, "background")) {
        /* the colour among the words, or the first colour of a gradient; `none` clears */
        const char *p = value;
        uint32_t rgb;
        bool tr;
        for (; *p; p++) {
            if (p != value && (isalnum((unsigned char)p[-1]) || p[-1] == '-' || p[-1] == '_'))
                continue;
            int l = css_color_at(p, &rgb, &tr);
            if (l > 0) {
                char b[40];
                snprintf(b, sizeof(b), tr ? "transparent" : "#%06x", rgb);
                decls_add(arr, n, "background-color", b, imp);
                return;
            }
        }
        if (strstr(value, "none") || strstr(value, "url(") == NULL)
            decls_add(arr, n, "background-color", "transparent", imp);
    } else if (!strcmp(prop, "font")) {
        int c = css_tokens(value, tok, 8);
        for (int i = 0; i < c; i++) {
            char *t = tok[i];
            if (!strcmp(t, "italic") || !strcmp(t, "oblique")) decls_add(arr, n, "font-style", "italic", imp);
            else if (!strcmp(t, "bold") || !strcmp(t, "bolder") || (isdigit((unsigned char)t[0]) && atoi(t) >= 100 && !strchr(t, 'p') && !strchr(t, 'e') && !strchr(t, '.')))
                decls_add(arr, n, "font-weight", t, imp);
            else if (isdigit((unsigned char)t[0]) || t[0] == '.') {
                char *sl = strchr(t, '/');
                if (sl) *sl = 0;
                decls_add(arr, n, "font-size", t, imp);
                break;
            }
        }
    } else if (!strcmp(prop, "list-style")) {
        decls_add(arr, n, "list-style-type", strstr(value, "none") ? "none" : "disc", imp);
    } else if (!strcmp(prop, "text-decoration-line")) {
        decls_add(arr, n, "text-decoration", value, imp);
    } else if (prop_supported(prop)) {
        decls_add(arr, n, prop, value, imp);
    }
}

/* declarations of a block "a: b; c: d !important" */
static void parse_decls(const char *s, size_t n, struct css_decl **arr, int *cnt)
{
    size_t i = 0;
    while (i < n) {
        size_t st = i;
        int depth = 0, braces = 0;
        char q = 0;
        while (i < n && (q || depth || braces || s[i] != ';')) {
            if (q) { if (s[i] == q) q = 0; else if (s[i] == '\\') i++; }
            else if (s[i] == '"' || s[i] == '\'') q = s[i];
            else if (s[i] == '(') depth++;
            else if (s[i] == ')' && depth) depth--;
            else if (s[i] == '{') braces++;
            else if (s[i] == '}' && braces) braces--;
            i++;
        }
        size_t en = i;
        if (i < n) i++;
        const char *colon = memchr(s + st, ':', en - st);
        if (!colon || braces)
            continue;
        const char *k = s + st, *ke = colon;
        while (k < ke && isspace((unsigned char)*k)) k++;
        while (ke > k && isspace((unsigned char)ke[-1])) ke--;
        const char *v = colon + 1, *ve = s + en;
        while (v < ve && isspace((unsigned char)*v)) v++;
        while (ve > v && isspace((unsigned char)ve[-1])) ve--;
        if (ke == k || ve == v)
            continue;
        bool imp = false;
        const char *bang = ve;
        while (bang > v && !(bang[-1] == '!')) bang--;
        if (bang > v && !strncasecmp(bang, "important", 9)) {
            imp = true;
            ve = bang - 1;
            while (ve > v && isspace((unsigned char)ve[-1])) ve--;
        }
        char prop[64];
        size_t pl = (size_t)(ke - k);
        if (pl >= sizeof(prop)) continue;
        memcpy(prop, k, pl);
        prop[pl] = 0;
        if (!(prop[0] == '-' && prop[1] == '-'))
            for (char *c = prop; *c; c++) *c = (char)tolower((unsigned char)*c);
        char *val = dstrndup(v, (size_t)(ve - v));
        if (prop[0] == '-' && prop[1] == '-') decls_add(arr, cnt, prop, val, imp);
        else add_shorthand(arr, cnt, prop, val, imp);
        free(val);
    }
}

static bool media_matches(const char *q, size_t n)
{
    char *s = dstrndup(q, n);
    for (char *c = s; *c; c++) *c = (char)tolower((unsigned char)*c);
    bool any = false;
    char *save = NULL;
    for (char *part = strtok_r(s, ",", &save); part; part = strtok_r(NULL, ",", &save)) {
        bool neg = false, ok = true, seen_type = false;
        char *p = part;
        while (*p) {
            while (isspace((unsigned char)*p)) p++;
            if (!*p) break;
            if (!strncmp(p, "not ", 4)) { neg = true; p += 4; continue; }
            if (!strncmp(p, "only ", 5)) { p += 5; continue; }
            if (!strncmp(p, "and ", 4) || !strncmp(p, "and(", 4)) { p += 3; continue; }
            if (*p == '(') {
                char *e = strchr(p, ')');
                if (!e) { ok = false; break; }
                *e = 0;
                char *feat = p + 1;
                double w;
                if (!strncmp(feat, "min-width", 9) && css_length(strchr(feat, ':') ? strchr(feat, ':') + 1 : "", 16, &w)) ok = ok && VIEW_W >= w;
                else if (!strncmp(feat, "max-width", 9) && css_length(strchr(feat, ':') ? strchr(feat, ':') + 1 : "", 16, &w)) ok = ok && VIEW_W <= w;
                else if (!strncmp(feat, "min-height", 10) && css_length(strchr(feat, ':') ? strchr(feat, ':') + 1 : "", 16, &w)) ok = ok && VIEW_H >= w;
                else if (!strncmp(feat, "max-height", 10) && css_length(strchr(feat, ':') ? strchr(feat, ':') + 1 : "", 16, &w)) ok = ok && VIEW_H <= w;
                else if (strstr(feat, "prefers-color-scheme") && strstr(feat, "dark")) ok = false;
                else if (strstr(feat, "prefers-reduced-motion") && strstr(feat, "reduce")) ok = false;
                else if (strstr(feat, "hover") && strstr(feat, "none")) ok = false;
                else if (strstr(feat, "orientation") && strstr(feat, "portrait")) ok = false;
                else if (strstr(feat, "resolution") || strstr(feat, "-webkit-min-device-pixel-ratio")) ok = ok && !strstr(feat, "min");
                p = e + 1;
                continue;
            }
            char *e = p;
            while (*e && !isspace((unsigned char)*e) && *e != '(') e++;
            char w[24];
            size_t l = (size_t)(e - p) < sizeof(w) - 1 ? (size_t)(e - p) : sizeof(w) - 1;
            memcpy(w, p, l);
            w[l] = 0;
            seen_type = true;
            if (!strcmp(w, "print") || !strcmp(w, "speech") || !strcmp(w, "tv") || !strcmp(w, "projection")) ok = false;
            p = e;
        }
        (void)seen_type;
        if (neg) ok = !ok;
        if (ok) any = true;
    }
    free(s);
    return any;
}

static void css_parse(struct dom *d, struct css_state *cs, const char *s, size_t n, const char *dir, int depth);

static void add_rules(struct css_state *cs, const char *prelude, size_t pl, const char *block, size_t bl)
{
    char *ptext = dstrndup(prelude, pl);
    struct css_list *list = calloc(1, sizeof(*list));
    if (!list)
        exit(1);
    if (!sel_parse(ptext, &list->sl)) {
        free(ptext);
        free(list);
        return;
    }
    free(ptext);
    parse_decls(block, bl, &list->decls, &list->ndecls);
    if (cs->nlists == cs->caplists) {
        cs->caplists = cs->caplists ? cs->caplists * 2 : 64;
        cs->lists = realloc(cs->lists, sizeof(*cs->lists) * (size_t)cs->caplists);
    }
    cs->lists[cs->nlists++] = list;
    for (int i = 0; i < list->sl.n && cs->n < MAX_RULES; i++) {
        if (cs->n == cs->cap) {
            cs->cap = cs->cap ? cs->cap * 2 : 256;
            cs->rules = realloc(cs->rules, sizeof(*cs->rules) * (size_t)cs->cap);
            if (!cs->rules)
                exit(1);
        }
        struct css_rule *r = &cs->rules[cs->n++];
        r->sel = list->sl.sels[i];
        r->spec = sel_specificity(&r->sel);
        r->order = cs->order++;
        r->decls = list->decls;
        r->ndecls = list->ndecls;
        r->list = list;
    }
}

static void css_parse(struct dom *d, struct css_state *cs, const char *s, size_t n, const char *dir, int depth)
{
    size_t i = 0;
    while (i < n) {
        while (i < n && (isspace((unsigned char)s[i]) || (s[i] == '<' && i + 3 < n && !strncmp(s + i, "<!--", 4)) ||
                         (s[i] == '-' && i + 2 < n && !strncmp(s + i, "-->", 3))))
            i += (s[i] == '<') ? 4 : (s[i] == '-') ? 3 : 1;
        if (i >= n)
            break;
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            const char *e = NULL;
            for (size_t k = i + 2; k + 1 < n; k++)
                if (s[k] == '*' && s[k + 1] == '/') { e = s + k; break; }
            i = e ? (size_t)(e - s) + 2 : n;
            continue;
        }
        size_t ps = i;
        int paren = 0;
        char q = 0;
        while (i < n && (q || paren || (s[i] != '{' && s[i] != ';' && s[i] != '}'))) {
            if (q) { if (s[i] == q) q = 0; else if (s[i] == '\\') i++; }
            else if (s[i] == '"' || s[i] == '\'') q = s[i];
            else if (s[i] == '(') paren++;
            else if (s[i] == ')' && paren) paren--;
            else if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
                size_t k = i + 2;
                while (k + 1 < n && !(s[k] == '*' && s[k + 1] == '/')) k++;
                i = k + 1;
            }
            i++;
        }
        size_t pe = i;
        if (i >= n)
            break;
        if (s[i] == '}' || s[i] == ';') {                                     /* at-rule statement or stray */
            const char *pre = s + ps;
            size_t pl = pe - ps;
            if (pl > 7 && !strncasecmp(pre, "@import", 7) && depth < 3 && d->host.fetch_script && cs->fetched < 16) {
                const char *u = pre + 7;
                while (u < pre + pl && isspace((unsigned char)*u)) u++;
                char href[1024] = "";
                if (!strncasecmp(u, "url(", 4)) {
                    u += 4;
                    while (isspace((unsigned char)*u) || *u == '"' || *u == '\'') u++;
                    size_t l = strcspn(u, "\"')");
                    snprintf(href, sizeof(href), "%.*s", (int)l, u);
                } else if (*u == '"' || *u == '\'') {
                    char qq = *u++;
                    size_t l = strcspn(u, (char[]){ qq, 0 });
                    snprintf(href, sizeof(href), "%.*s", (int)l, u);
                }
                if (href[0]) {
                    char full[1200];
                    if (dir && dir[0] && !strstr(href, "://") && href[0] != '/' && strncmp(href, "data:", 5))
                        snprintf(full, sizeof(full), "%s%s", dir, href);
                    else
                        snprintf(full, sizeof(full), "%s", href);
                    char *body = NULL;
                    size_t bl = 0;
                    char err[200];
                    cs->fetched++;
                    if (d->host.fetch_script(d->host.data, full, &body, &bl, err, sizeof(err))) {
                        char ndir[1200];
                        snprintf(ndir, sizeof(ndir), "%s", full);
                        char *sl = strrchr(ndir, '/');
                        if (sl) sl[1] = 0; else ndir[0] = 0;
                        if (bl < MAX_SHEET_BYTES)
                            css_parse(d, cs, body, bl, ndir, depth + 1);
                        free(body);
                    }
                }
            }
            i = s[i] == ';' ? i + 1 : i + 1;
            continue;
        }
        /* a block: find the matching close brace */
        size_t bs = i + 1;
        int braces = 1;
        size_t k = bs;
        q = 0;
        while (k < n && braces) {
            if (q) { if (s[k] == q) q = 0; else if (s[k] == '\\') k++; }
            else if (s[k] == '"' || s[k] == '\'') q = s[k];
            else if (s[k] == '{') braces++;
            else if (s[k] == '}') braces--;
            else if (s[k] == '/' && k + 1 < n && s[k + 1] == '*') {
                k += 2;
                while (k + 1 < n && !(s[k] == '*' && s[k + 1] == '/')) k++;
                k++;
            }
            k++;
        }
        size_t be = braces ? n : k - 1;
        const char *pre = s + ps;
        size_t pl = pe - ps;
        while (pl && isspace((unsigned char)pre[pl - 1])) pl--;
        if (pl && pre[0] == '@') {
            if (pl >= 6 && !strncasecmp(pre, "@media", 6)) {
                if (media_matches(pre + 6, pl - 6))
                    css_parse(d, cs, s + bs, be - bs, dir, depth + 1);
            } else if ((pl >= 9 && !strncasecmp(pre, "@supports", 9)) || (pl >= 6 && !strncasecmp(pre, "@layer", 6)) ||
                       (pl >= 10 && !strncasecmp(pre, "@container", 10) && false)) {
                if (depth < 6)
                    css_parse(d, cs, s + bs, be - bs, dir, depth + 1);
            }                                                                  /* @font-face, @keyframes, @page ...: skipped */
        } else if (pl) {
            add_rules(cs, pre, pl, s + bs, be - bs);
        }
        i = braces ? n : k;
    }
}

/* ---- stylesheets of the document ------------------------------------------------------------------------------- */

static unsigned hash_str(unsigned h, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

/* hash of everything that decides which sheets apply (so JS adding a <style> is noticed) */
static unsigned sheets_hash(struct dnode *root)
{
    unsigned h = 2166136261u;
    const struct dnode *c = root->first;
    while (c) {
        if (c->type == DN_ELEMENT) {
            if (!strcmp(c->tag, "style")) {
                for (const struct dnode *t = c->first; t; t = t->next)
                    if (t->type == DN_TEXT) h = hash_str(h, t->text, strlen(t->text));
                const char *m = dattr_get(c, "media");
                if (m) h = hash_str(h, m, strlen(m));
            } else if (!strcmp(c->tag, "link")) {
                const char *rel = dattr_get(c, "rel"), *href = dattr_get(c, "href");
                if (rel && href && strcasestr(rel, "stylesheet"))
                    h = hash_str(h, href, strlen(href));
            }
        }
        if (c->first) { c = c->first; continue; }
        while (c && c != root && !c->next) c = c->parent;
        if (!c || c == root) break;
        c = c->next;
    }
    return h;
}

static unsigned key_hash(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static void bucket_add(struct css_bucket *b, int idx)
{
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4;
        b->idx = realloc(b->idx, sizeof(int) * (size_t)b->cap);
        if (!b->idx)
            exit(1);
    }
    b->idx[b->n++] = idx;
}

static struct css_bucket *bucket_for(struct css_state *cs, const char *key, bool create)
{
    unsigned h = key_hash(key) & 1023;
    for (struct css_bucket *b = cs->buckets[h]; b; b = b->next)
        if (!strcmp(b->key, key))
            return b;
    if (!create)
        return NULL;
    struct css_bucket *b = calloc(1, sizeof(*b));
    if (!b)
        exit(1);
    b->key = dstrdup(key);
    b->next = cs->buckets[h];
    cs->buckets[h] = b;
    return b;
}

static void build_index(struct css_state *cs)
{
    for (int i = 0; i < cs->n; i++) {
        const struct sel_simple *last = &cs->rules[i].sel.parts[cs->rules[i].sel.n - 1];
        char key[160];
        if (last->id) snprintf(key, sizeof(key), "#%s", last->id);
        else if (last->nclasses) snprintf(key, sizeof(key), ".%s", last->classes[0]);
        else if (last->tag && strcmp(last->tag, "*")) snprintf(key, sizeof(key), "%s", last->tag);
        else { bucket_add(&cs->universal, i); continue; }
        bucket_add(bucket_for(cs, key, true), i);
    }
    cs->indexed = true;
}

static void index_clear(struct css_state *cs)
{
    for (int h = 0; h < 1024; h++)
        for (struct css_bucket *b = cs->buckets[h]; b;) {
            struct css_bucket *nx = b->next;
            free(b->key);
            free(b->idx);
            free(b);
            b = nx;
        }
    memset(cs->buckets, 0, sizeof(cs->buckets));
    free(cs->universal.idx);
    memset(&cs->universal, 0, sizeof(cs->universal));
    cs->indexed = false;
}

static void css_state_clear(struct css_state *cs)
{
    index_clear(cs);
    for (int i = 0; i < cs->nlists; i++) {
        struct css_list *l = cs->lists[i];
        for (int k = 0; k < l->ndecls; k++) {
            free(l->decls[k].prop);
            free(l->decls[k].value);
        }
        free(l->decls);
        sel_list_free(&l->sl);
        free(l);
    }
    free(cs->lists);
    free(cs->rules);
    cs->lists = NULL;
    cs->rules = NULL;
    cs->n = cs->cap = cs->nlists = cs->caplists = 0;
    cs->order = 0;
    cs->fetched = 0;
}

void dom_css_free(struct dom *d)
{
    if (!d->css)
        return;
    css_state_clear(d->css);
    free(d->css);
    d->css = NULL;
}

static void load_sheets(struct dom *d, struct css_state *cs)
{
    const struct dnode *c = d->doc->first;
    while (c) {
        if (c->type == DN_ELEMENT) {
            if (!strcmp(c->tag, "style")) {
                const char *type = dattr_get(c, "type"), *media = dattr_get(c, "media");
                if ((!type || !*type || !strcasecmp(type, "text/css")) && (!media || media_matches(media, strlen(media)))) {
                    struct sbuf sb = { 0 };
                    for (const struct dnode *t = c->first; t; t = t->next)
                        if (t->type == DN_TEXT) sbuf_addz(&sb, t->text);
                    if (sb.p && sb.n < MAX_SHEET_BYTES)
                        css_parse(d, cs, sb.p, sb.n, "", 0);
                    free(sb.p);
                }
            } else if (!strcmp(c->tag, "link") && d->host.fetch_script && cs->fetched < 16) {
                const char *rel = dattr_get(c, "rel"), *href = dattr_get(c, "href"), *media = dattr_get(c, "media");
                if (rel && href && *href && strcasestr(rel, "stylesheet") && !strcasestr(rel, "alternate") &&
                    (!media || media_matches(media, strlen(media)))) {
                    char *body = NULL;
                    size_t bl = 0;
                    char err[200];
                    cs->fetched++;
                    if (d->host.fetch_script(d->host.data, href, &body, &bl, err, sizeof(err))) {
                        char dir[1200];
                        snprintf(dir, sizeof(dir), "%s", href);
                        char *sl = strrchr(dir, '/');
                        if (sl) sl[1] = 0; else dir[0] = 0;
                        if (bl < MAX_SHEET_BYTES)
                            css_parse(d, cs, body, bl, dir, 0);
                        free(body);
                    }
                }
            }
        }
        if (c->first) { c = c->first; continue; }
        while (c && c != d->doc && !c->next) c = c->parent;
        if (!c || c == d->doc) break;
        c = c->next;
    }
}

/* ---- the cascade ------------------------------------------------------------------------------------------------ */

enum { P_COLOR, P_BG, P_DISPLAY, P_WEIGHT, P_STYLE, P_SIZE, P_DECO, P_ALIGN, P_TRANSFORM, P_WS, P_VIS, P_OPACITY, P_MT, P_MB,
       P_ML, P_PT, P_PB, P_PL, P_LIST, P_CONTENT, P_FLEXDIR, P_COUNT };

static const char *const PROP_NAME[P_COUNT] = { "color", "background-color", "display", "font-weight", "font-style", "font-size",
    "text-decoration", "text-align", "text-transform", "white-space", "visibility", "opacity", "margin-top", "margin-bottom",
    "margin-left", "padding-top", "padding-bottom", "padding-left", "list-style-type", "content", "flex-direction" };

static int prop_index(const char *p)
{
    for (int i = 0; i < P_COUNT; i++)
        if (!strcmp(PROP_NAME[i], p))
            return i;
    return -1;
}

struct varmap { char *name, *val; struct varmap *next; };

struct cand { const char *value; int imp, spec, order; };

static bool cand_wins(const struct cand *a, const struct cand *b)    /* does a beat b? */
{
    if (!b->value) return true;
    if (a->imp != b->imp) return a->imp > b->imp;
    if (a->spec != b->spec) return a->spec > b->spec;
    return a->order >= b->order;
}

static const char *var_lookup(const struct varmap *m, const char *name, size_t nl)
{
    for (; m; m = m->next)
        if (strlen(m->name) == nl && !strncmp(m->name, name, nl))
            return m->val;
    return NULL;
}

/* substitute var(--x, fallback) in value (malloc'ed result) */
static char *var_subst(const char *v, const struct varmap *vars, int depth)
{
    if (!strstr(v, "var(") || depth > 6)
        return dstrdup(v);
    struct sbuf sb = { 0 };
    for (const char *p = v; *p;) {
        if (!strncmp(p, "var(", 4)) {
            const char *a = p + 4;
            int paren = 1;
            const char *e = a;
            while (*e && paren) { if (*e == '(') paren++; else if (*e == ')') paren--; if (paren) e++; }
            const char *comma = a;
            int pd = 0;
            while (comma < e && !(*comma == ',' && !pd)) { if (*comma == '(') pd++; else if (*comma == ')') pd--; comma++; }
            const char *nm = a;
            while (nm < comma && isspace((unsigned char)*nm)) nm++;
            size_t nl = (size_t)(comma - nm);
            while (nl && isspace((unsigned char)nm[nl - 1])) nl--;
            const char *val = var_lookup(vars, nm, nl);
            char *sub = NULL;
            if (val) sub = var_subst(val, vars, depth + 1);
            else if (comma < e) {
                char *fb = dstrndup(comma + 1, (size_t)(e - comma - 1));
                sub = var_subst(fb, vars, depth + 1);
                free(fb);
            }
            if (sub) { sbuf_addz(&sb, sub); free(sub); }
            p = *e ? e + 1 : e;
        } else {
            sbuf_putc(&sb, *p++);
        }
    }
    return sb.p ? sb.p : dstrdup("");
}

static int font_scale_px(int px) { return px < 22 ? 1 : px < 34 ? 2 : 3; }

static const struct { const char *tag; double mult; } TAG_FONT[] = {
    { "h1", 2.0 }, { "h2", 1.5 }, { "h3", 1.17 }, { "h4", 1.0 }, { "h5", 0.83 }, { "h6", 0.67 }, { "small", 0.83 }, { "big", 1.2 },
    { "sub", 0.83 }, { "sup", 0.83 },
};

static void eval_value(struct estyle *st, int prop, const char *v, int parent_px, int *px_out)
{
    uint32_t rgb;
    bool tr;
    switch (prop) {
    case P_COLOR:
        if (css_color_at(v, &rgb, &tr) > 0 && !tr) st->fg = (int32_t)rgb;
        break;
    case P_BG:
        if (css_color_at(v, &rgb, &tr) > 0) st->bg = tr ? -2 : (int32_t)rgb;
        break;
    case P_DISPLAY: {
        if (!strncasecmp(v, "none", 4)) st->display = DS_NONE;
        else if (!strncasecmp(v, "inline-block", 12) || !strncasecmp(v, "inline-flex", 11) || !strncasecmp(v, "inline-grid", 11) || !strncasecmp(v, "inline-table", 12)) st->display = DS_INLINE_BLOCK;
        else if (!strncasecmp(v, "inline", 6)) st->display = DS_INLINE;
        else if (!strncasecmp(v, "flex", 4) || !strncasecmp(v, "grid", 4)) st->display = DS_FLEX;
        else if (!strncasecmp(v, "list-item", 9)) st->display = DS_LIST_ITEM;
        else if (!strncasecmp(v, "table-cell", 10) || !strncasecmp(v, "table-row", 9)) st->display = DS_INLINE_BLOCK;
        else if (!strncasecmp(v, "table", 5)) st->display = DS_TABLE;
        else if (!strncasecmp(v, "contents", 8)) st->display = DS_CONTENTS;
        else if (!strncasecmp(v, "block", 5) || !strncasecmp(v, "flow-root", 9)) st->display = DS_BLOCK;
        break;
    }
    case P_WEIGHT:
        if (!strncasecmp(v, "bold", 4)) st->bold = 1;
        else if (!strncasecmp(v, "normal", 6) || !strncasecmp(v, "lighter", 7)) st->bold = 0;
        else if (isdigit((unsigned char)*v)) st->bold = atoi(v) >= 600;
        break;
    case P_STYLE:
        if (!strncasecmp(v, "italic", 6) || !strncasecmp(v, "oblique", 7)) st->italic = 1;
        else if (!strncasecmp(v, "normal", 6)) st->italic = 0;
        break;
    case P_SIZE: {
        double px = 0;
        static const struct { const char *k; double px; } kw[] = { { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 },
            { "large", 18 }, { "x-large", 24 }, { "xx-large", 32 }, { "xxx-large", 48 } };
        bool ok = false;
        for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++)
            if (!strncasecmp(v, kw[i].k, strlen(kw[i].k)) && !isalpha((unsigned char)v[strlen(kw[i].k)])) { px = kw[i].px; ok = true; }
        if (!ok && !strncasecmp(v, "larger", 6)) { px = parent_px * 1.2; ok = true; }
        if (!ok && !strncasecmp(v, "smaller", 7)) { px = parent_px / 1.2; ok = true; }
        if (!ok) ok = css_length(v, parent_px, &px);
        if (ok && px > 0) { *px_out = (int)(px + 0.5); st->font_px = (int16_t)*px_out; }
        break;
    }
    case P_DECO:
        if (strstr(v, "none")) { st->underline = 0; st->strike = 0; }
        if (strstr(v, "underline")) st->underline = 1;
        if (strstr(v, "line-through")) st->strike = 1;
        break;
    case P_ALIGN:
        if (!strncasecmp(v, "center", 6) || !strncasecmp(v, "-webkit-center", 14)) st->align = 1;
        else if (!strncasecmp(v, "right", 5) || !strncasecmp(v, "end", 3)) st->align = 2;
        else if (!strncasecmp(v, "left", 4) || !strncasecmp(v, "start", 5) || !strncasecmp(v, "justify", 7)) st->align = 0;
        break;
    case P_TRANSFORM:
        if (!strncasecmp(v, "uppercase", 9)) st->transform = 1;
        else if (!strncasecmp(v, "lowercase", 9)) st->transform = 2;
        else if (!strncasecmp(v, "capitalize", 10)) st->transform = 3;
        else if (!strncasecmp(v, "none", 4)) st->transform = 0;
        break;
    case P_WS:
        if (!strncasecmp(v, "pre", 3)) st->pre = 1;
        else if (!strncasecmp(v, "normal", 6) || !strncasecmp(v, "nowrap", 6)) st->pre = 0;
        break;
    case P_VIS:
        if (!strncasecmp(v, "hidden", 6) || !strncasecmp(v, "collapse", 8)) st->hidden = 1;
        else if (!strncasecmp(v, "visible", 7)) st->hidden = 0;
        break;
    case P_OPACITY:
        if (atof(v) == 0 && (isdigit((unsigned char)*v) || *v == '.')) st->hidden = 1;
        break;
    case P_MT: case P_MB: case P_PT: case P_PB: case P_ML: case P_PL: {
        double px;
        if (css_length(v, parent_px, &px)) {
            int16_t val = (int16_t)(px < 0 ? 0 : px > 400 ? 400 : px + 0.5);
            if (prop == P_MT) st->mt = val;
            else if (prop == P_MB) st->mb = val;
            else if (prop == P_PT) st->pt = val;
            else if (prop == P_PB) st->pb = val;
            else if (prop == P_PL || prop == P_ML) st->pl = (int16_t)((st->pl > 0 ? st->pl : 0) + val);
        }
        break;
    }
    case P_LIST:
        st->listnone = !strncasecmp(v, "none", 4);
        break;
    case P_FLEXDIR:
        st->column = !strncasecmp(v, "column", 6);
        break;
    default:
        break;
    }
}

static struct estyle *estyle_new(void)
{
    struct estyle *st = calloc(1, sizeof(*st));
    if (!st)
        exit(1);
    st->fg = st->bg = -1;
    st->display = -1;
    st->bold = st->italic = st->underline = st->strike = st->pre = st->align = st->transform = st->listnone = st->hidden = -1;
    st->mt = st->mb = st->pl = st->pt = st->pb = -1;
    return st;
}

static bool estyle_empty(const struct estyle *st)
{
    return st->fg == -1 && st->bg == -1 && st->display == -1 && st->bold == -1 && st->italic == -1 && st->underline == -1 &&
           st->strike == -1 && st->pre == -1 && st->align == -1 && st->transform == -1 && st->listnone == -1 && st->hidden == -1 &&
           st->font_px == 0 && st->mt == -1 && st->mb == -1 && st->pl == -1 && st->pt == -1 && st->pb == -1 && !st->flex_item;
}

static void free_styles(struct dom *d)
{
    for (struct dnode *n = d->all_nodes; n; n = n->all_next) {
        free(n->st); free(n->st_before); free(n->st_after);
        free(n->before_txt); free(n->after_txt);
        n->st = n->st_before = n->st_after = NULL;
        n->before_txt = n->after_txt = NULL;
    }
}

/* content: "text" attr(x) ... -> text (malloc'ed) or NULL when nothing printable */
static char *content_text(const char *v, const struct dnode *n)
{
    struct sbuf sb = { 0 };
    for (const char *p = v; *p;) {
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q) {
                if (*p == '\\') {
                    p++;
                    if (isxdigit((unsigned char)*p)) {
                        uint32_t cp = 0;
                        int k = 0;
                        while (isxdigit((unsigned char)*p) && k < 6) { cp = cp * 16 + (uint32_t)(isdigit((unsigned char)*p) ? *p - '0' : tolower(*p) - 'a' + 10); p++; k++; }
                        if (*p == ' ') p++;
                        if (cp >= 0xe000 && cp <= 0xf8ff) { free(sb.p); return NULL; }       /* icon font glyph: nothing to show */
                        char b[4];
                        int l = cp < 0x80 ? (b[0] = (char)cp, 1) : cp < 0x800 ? (b[0] = (char)(0xc0 | cp >> 6), b[1] = (char)(0x80 | (cp & 63)), 2) :
                                cp < 0x10000 ? (b[0] = (char)(0xe0 | cp >> 12), b[1] = (char)(0x80 | ((cp >> 6) & 63)), b[2] = (char)(0x80 | (cp & 63)), 3) :
                                (b[0] = (char)(0xf0 | cp >> 18), b[1] = (char)(0x80 | ((cp >> 12) & 63)), b[2] = (char)(0x80 | ((cp >> 6) & 63)), b[3] = (char)(0x80 | (cp & 63)), 4);
                        sbuf_add(&sb, b, (size_t)l);
                    } else if (*p) {
                        sbuf_putc(&sb, *p++);
                    }
                } else {
                    sbuf_putc(&sb, *p++);
                }
            }
            if (*p) p++;
        } else if (!strncasecmp(p, "attr(", 5)) {
            const char *e = strchr(p, ')');
            if (!e) break;
            char name[64];
            snprintf(name, sizeof(name), "%.*s", (int)(e - p - 5), p + 5);
            const char *a = dattr_get(n, name);
            if (a) sbuf_addz(&sb, a);
            p = e + 1;
        } else if (!strncasecmp(p, "url(", 4) || !strncasecmp(p, "counter", 7)) {
            free(sb.p);
            return NULL;
        } else {
            p++;
        }
    }
    if (!sb.p || !sb.p[strspn(sb.p, " ")]) {
        free(sb.p);
        return NULL;
    }
    return sb.p;
}

static void cascade(struct dom *d, struct css_state *cs, struct dnode *n, int parent_px, const struct varmap *pvars, int depth)
{
    if (n->type != DN_ELEMENT) {
        for (struct dnode *c = n->first; c; c = c->next)
            cascade(d, cs, c, parent_px, pvars, depth);
        return;
    }
    struct cand cands[P_COUNT][3];                       /* [prop][0 element, 1 ::before, 2 ::after] */
    memset(cands, 0, sizeof(cands));
    struct { const char *name; struct cand c; } custom[24];
    int ncustom = 0;
    bool any_pseudo[3] = { true, false, false };

    /* presentational attributes: weakest */
    struct estyle *pre = estyle_new();
    const char *al = dattr_get(n, "align");
    if (al && (!strcasecmp(al, "center"))) pre->align = 1;
    else if (al && !strcasecmp(al, "right")) pre->align = 2;
    if (!strcmp(n->tag, "center")) pre->align = 1;
    const char *bgc = dattr_get(n, "bgcolor");
    uint32_t rgb;
    bool tr;
    if (bgc && css_color_at(bgc, &rgb, &tr) > 0 && !tr) pre->bg = (int32_t)rgb;
    const char *fc = !strcmp(n->tag, "font") ? dattr_get(n, "color") : NULL;
    if (fc && css_color_at(fc, &rgb, &tr) > 0 && !tr) pre->fg = (int32_t)rgb;

    /* candidate rules: the buckets of this element's id, classes and tag, plus the universal ones */
    int cand_idx_n = 0;
    int *cand_idx = NULL;
    {
        const struct css_bucket *bl[24];
        int nb = 0;
        char key[160];
        const char *id = dattr_get(n, "id");
        if (id && *id) { snprintf(key, sizeof(key), "#%s", id); const struct css_bucket *b = bucket_for(cs, key, false); if (b) bl[nb++] = b; }
        const char *cls = dattr_get(n, "class");
        for (const char *p = cls ? cls : ""; *p && nb < 20;) {
            while (isspace((unsigned char)*p)) p++;
            const char *st = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (p > st && (size_t)(p - st) < sizeof(key) - 2) {
                snprintf(key, sizeof(key), ".%.*s", (int)(p - st), st);
                const struct css_bucket *b = bucket_for(cs, key, false);
                if (b) bl[nb++] = b;
            }
        }
        { const struct css_bucket *b = bucket_for(cs, n->tag, false); if (b) bl[nb++] = b; }
        if (cs->universal.n) bl[nb++] = &cs->universal;
        int total = 0;
        for (int k = 0; k < nb; k++) total += bl[k]->n;
        cand_idx = malloc(sizeof(int) * (size_t)(total + 1));
        for (int k = 0; k < nb; k++)
            for (int m = 0; m < bl[k]->n; m++)
                cand_idx[cand_idx_n++] = bl[k]->idx[m];
    }
    for (int ci = 0; ci < cand_idx_n; ci++) {
        struct css_rule *r = &cs->rules[cand_idx[ci]];
        int slot = r->sel.parts[r->sel.n - 1].pseudo_elem;
        if (!sel_match_one(&r->sel, n))
            continue;
        any_pseudo[slot] = true;
        for (int k = 0; k < r->ndecls; k++) {
            struct css_decl *dc = &r->decls[k];
            struct cand c = { dc->value, dc->important, r->spec, r->order };
            if (dc->prop[0] == '-' && dc->prop[1] == '-') {
                if (slot) continue;
                int found = -1;
                for (int m = 0; m < ncustom; m++) if (!strcmp(custom[m].name, dc->prop)) found = m;
                if (found < 0 && ncustom < 24) { custom[ncustom].name = dc->prop; custom[ncustom].c = c; ncustom++; }
                else if (found >= 0 && cand_wins(&c, &custom[found].c)) custom[found].c = c;
                continue;
            }
            int pi = prop_index(dc->prop);
            if (pi < 0) continue;
            if (cand_wins(&c, &cands[pi][slot]))
                cands[pi][slot] = c;
        }
    }
    free(cand_idx);
    /* inline style attribute: highest specificity */
    const char *inl = dattr_get(n, "style");
    struct css_decl *idecls = NULL;
    int nid = 0;
    if (inl) {
        parse_decls(inl, strlen(inl), &idecls, &nid);
        for (int k = 0; k < nid; k++) {
            struct cand c = { idecls[k].value, idecls[k].important, 100000, 1 << 30 };
            if (idecls[k].prop[0] == '-' && idecls[k].prop[1] == '-') {
                int found = -1;
                for (int m = 0; m < ncustom; m++) if (!strcmp(custom[m].name, idecls[k].prop)) found = m;
                if (found < 0 && ncustom < 24) { custom[ncustom].name = idecls[k].prop; custom[ncustom].c = c; ncustom++; }
                else if (found >= 0) custom[found].c = c;
                continue;
            }
            int pi = prop_index(idecls[k].prop);
            if (pi >= 0 && cand_wins(&c, &cands[pi][0]))
                cands[pi][0] = c;
        }
    }
    /* custom properties of this element */
    struct varmap *vm = NULL;
    for (int m = 0; m < ncustom; m++) {
        struct varmap *e = malloc(sizeof(*e));
        e->name = dstrdup(custom[m].name);
        e->val = var_subst(custom[m].c.value, pvars, 0);
        e->next = vm;
        vm = e;
    }
    for (struct varmap *e = vm; e; e = e->next)
        if (!e->next) { e->next = (struct varmap *)pvars; break; }
    const struct varmap *vars = vm ? vm : pvars;

    int px = parent_px;
    for (int slot = 0; slot < 3; slot++) {
        if (slot && !any_pseudo[slot]) continue;
        struct estyle *st = slot == 0 ? n->st = pre : estyle_new();
        if (slot == 0) { /* keep presentational values */ }
        else {
            if (slot == 1) n->st_before = st; else n->st_after = st;
        }
        /* font-size first (em units of the other properties are relative to the parent, margins to the new size) */
        int my_px = parent_px;
        if (slot == 0) {
            for (int t = 0; t < (int)(sizeof(TAG_FONT) / sizeof(TAG_FONT[0])); t++)
                if (!strcmp(n->tag, TAG_FONT[t].tag)) my_px = (int)(parent_px * TAG_FONT[t].mult + 0.5);
        }
        if (cands[P_SIZE][slot].value) {
            char *v = var_subst(cands[P_SIZE][slot].value, vars, 0);
            int out = my_px;
            eval_value(st, P_SIZE, v, parent_px, &out);
            if (st->font_px) my_px = out;
            free(v);
        }
        if (slot == 0) px = my_px;
        for (int p = 0; p < P_COUNT; p++) {
            if (p == P_SIZE || !cands[p][slot].value) continue;
            char *v = var_subst(cands[p][slot].value, vars, 0);
            if (p == P_CONTENT) {
                if (slot) {
                    char *t = content_text(v, n);
                    if (slot == 1) n->before_txt = t; else n->after_txt = t;
                } 
            } else {
                int dummy = my_px;
                eval_value(st, p, v, my_px, &dummy);
            }
            free(v);
        }
    }
    free(idecls);
    /* a pseudo-element without text is not generated */
    if (n->st_before && !n->before_txt) { free(n->st_before); n->st_before = NULL; }
    if (n->st_after && !n->after_txt) { free(n->st_after); n->st_after = NULL; }
    /* flex rows lay their children out as inline items */
    struct estyle *st = n->st;
    bool row_flex = st->display == DS_FLEX && !st->column;
    (void)row_flex;
    for (struct dnode *c = n->first; c; c = c->next) {
        cascade(d, cs, c, px, vars, depth + 1);
        if (row_flex && c->type == DN_ELEMENT && c->st && (c->st->display == -1 || c->st->display == DS_BLOCK))
            c->st->flex_item = 1;
    }
    /* free the custom properties we added (the shared tail belongs to the parent) */
    for (struct varmap *e = vm; e && e != pvars;) {
        struct varmap *nx = e->next;
        free(e->name);
        free(e->val);
        free(e);
        e = nx;
    }
    if (estyle_empty(n->st)) { free(n->st); n->st = NULL; }
}

void dom_css_apply(struct dom *d)
{
    if (!d->css) {
        d->css = calloc(1, sizeof(*d->css));
        if (!d->css)
            exit(1);
        d->css->hash = 0;
    }
    struct css_state *cs = d->css;
    unsigned h = sheets_hash(d->doc);
    struct timespec t0, t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (h != cs->hash || (!cs->hash && !cs->n)) {
        css_state_clear(cs);
        cs->hash = h;
        load_sheets(d, cs);
    }
    if (!cs->indexed)
        build_index(cs);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    free_styles(d);
    for (struct dnode *c = d->doc->first; c; c = c->next)
        cascade(d, cs, c, 16, NULL, 0);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    if (getenv("LUMEN_WEB_TIMING"))
        fprintf(stderr, "[timing] css: load+parse %ld ms (%d rules), cascade %ld ms\n",
                (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000, cs->n,
                (t2.tv_sec - t1.tv_sec) * 1000 + (t2.tv_nsec - t1.tv_nsec) / 1000000);
}

/* ---- serialization hooks --------------------------------------------------------------------------------------- */

static const char *hexcol(char *b, int32_t c) { snprintf(b, 8, "%06x", (unsigned)c & 0xffffff); return b; }

bool css_page_bg(struct dom *d, uint32_t *rgb, uint32_t *fg)
{
    bool have = false;
    *fg = 0xffffffffu;
    struct dnode *els[2] = { d->html, d->body };
    for (int i = 0; i < 2; i++)
        if (els[i] && els[i]->st) {
            if (els[i]->st->bg >= 0) { *rgb = (uint32_t)els[i]->st->bg; have = true; }
            if (els[i]->st->fg >= 0) *fg = (uint32_t)els[i]->st->fg;
        }
    return have;
}

bool css_open_outer(struct sbuf *sb, const struct estyle *st)
{
    if (!st)
        return false;
    bool force_block = st->display == DS_BLOCK || st->display == DS_LIST_ITEM || st->display == DS_TABLE;
    bool inl = st->display == DS_INLINE || st->display == DS_INLINE_BLOCK || st->flex_item;
    bool any = force_block || inl || st->display == DS_FLEX || st->bg >= 0 || st->align >= 0 || st->mt > 0 || st->mb > 0 ||
               st->pl > 0 || st->pt > 0 || st->pb > 0 || st->listnone == 1;
    if (!any)
        return false;
    char b[8], num[24];
    sbuf_addz(sb, "<x-o");
    if (st->display == DS_FLEX) sbuf_addz(sb, " d=f");
    else if (inl) sbuf_addz(sb, st->display == DS_INLINE ? " d=s" : " d=i");
    else if (force_block) sbuf_addz(sb, " d=b");
    if (st->bg >= 0) { sbuf_addz(sb, " bg="); sbuf_addz(sb, hexcol(b, st->bg)); }
    if (st->align >= 0) sbuf_addz(sb, st->align == 1 ? " al=c" : st->align == 2 ? " al=r" : " al=l");
    if (st->mt > 0) { snprintf(num, sizeof(num), " mt=%d", st->mt); sbuf_addz(sb, num); }
    if (st->mb > 0) { snprintf(num, sizeof(num), " mb=%d", st->mb); sbuf_addz(sb, num); }
    if (st->pt > 0) { snprintf(num, sizeof(num), " pt=%d", st->pt); sbuf_addz(sb, num); }
    if (st->pb > 0) { snprintf(num, sizeof(num), " pb=%d", st->pb); sbuf_addz(sb, num); }
    if (st->pl > 0) { snprintf(num, sizeof(num), " pl=%d", st->pl); sbuf_addz(sb, num); }
    if (st->listnone == 1) sbuf_addz(sb, " nb=1");
    sbuf_putc(sb, '>');
    return true;
}

bool css_open_inner(struct sbuf *sb, const struct estyle *st, bool is_link)
{
    if (!st)
        return false;
    bool any = st->fg >= 0 || st->bold >= 0 || st->italic >= 0 || st->underline >= 0 || st->strike >= 0 || st->font_px > 0 ||
               st->pre >= 0 || st->transform >= 0;
    if (!any)
        return false;
    char b[8], num[24];
    sbuf_addz(sb, "<x-i");
    if (st->fg >= 0) { sbuf_addz(sb, is_link ? " lc=" : " fg="); sbuf_addz(sb, hexcol(b, st->fg)); }
    if (st->bold >= 0) sbuf_addz(sb, st->bold ? " b=1" : " b=0");
    if (st->italic >= 0) sbuf_addz(sb, st->italic ? " i=1" : " i=0");
    if (st->underline >= 0) sbuf_addz(sb, st->underline ? " u=1" : " u=0");
    if (st->strike >= 0) sbuf_addz(sb, st->strike ? " s=1" : " s=0");
    if (st->font_px > 0) { snprintf(num, sizeof(num), " sz=%d", font_scale_px(st->font_px)); sbuf_addz(sb, num); }
    if (st->pre >= 0) sbuf_addz(sb, st->pre ? " ws=p" : " ws=n");
    if (st->transform >= 0) { snprintf(num, sizeof(num), " tt=%d", st->transform); sbuf_addz(sb, num); }
    sbuf_putc(sb, '>');
    return true;
}
