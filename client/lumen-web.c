/*
 * lumen-web: a tiny web browser for lumen (wl_shm + xdg-shell, bitmap font, no libraries).
 *
 *   lumen-web [URL|PATH]     default: about:home
 *   lumen-web --selftest     parser / layout / URL checks without a display
 *
 * Supported: file:// (paths, directory listings), http:// (HTTP/1.0, redirects, chunked), about:home.
 * Not supported: https (no TLS library), CSS, JavaScript, images (shown as [alt]), forms.
 * HTML subset: headings, paragraphs, lists, links, pre/code, bold/italic, blockquote, hr, br, simple tables.
 * Fetching is synchronous (the window does not repaint while a page is loading, 10 s at most).
 *
 * Keys: Ctrl+L / F6 address bar, Enter go, Esc leave the address bar, Alt+Left / Backspace back, Alt+Right forward,
 *       F5 / Ctrl+R reload, Up/Down/PgUp/PgDn/Home/End/Space scroll (also j / k), mouse wheel scrolls.
 */
/* snprintf truncation of urls/titles is intentional and always NUL-terminated */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "font.h"
#include "keymap.h"
#include "util.h"
#include "wlc.h"

#define DEFAULT_W 900
#define DEFAULT_H 640
#define TOOLBAR_H 34
#define STATUS_H 20
#define PAGE_PAD 16
#define INDENT_PX 28
#define MAX_BODY (8u << 20)
#define FETCH_TIMEOUT_MS 10000

#define COL_BG      0x1d2230u
#define COL_FIELD   0x10131cu
#define COL_BTN     0x2c3550u
#define COL_HOVER   0x41527eu
#define COL_ACCENT  0x6fa8ffu
#define COL_UI_TEXT 0xe6e9efu
#define COL_UI_DIM  0x7b8294u
#define COL_PAGE    0xfbfbf8u
#define COL_TEXT    0x1b1b1bu
#define COL_LINK    0x0b52c2u
#define COL_HEAD    0x0d2a5cu
#define COL_CODE    0x7a2152u
#define COL_GRAY    0x777777u
#define COL_RULE    0xb8b8b0u

/* ---- small string builder ------------------------------------------------------------------ */

struct sb { char *p; size_t n, cap; };

static void sb_add(struct sb *s, const char *d, size_t n)
{
    if (s->n + n + 1 > s->cap) {
        s->cap = LM_MAX(s->cap * 2, s->n + n + 1);
        s->cap = LM_MAX(s->cap, (size_t)256);
        s->p = realloc(s->p, s->cap);
        if (!s->p) {
            perror("realloc");
            exit(1);
        }
    }
    memcpy(s->p + s->n, d, n);
    s->n += n;
    s->p[s->n] = 0;
}

static void sb_str(struct sb *s, const char *z) { sb_add(s, z, strlen(z)); }

static void sb_printf(struct sb *s, const char *fmt, ...) LM_PRINTF(2, 3);
static void sb_printf(struct sb *s, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0)
        sb_add(s, tmp, (size_t)LM_MIN(n, (int)sizeof(tmp) - 1));
}

static void sb_escape(struct sb *s, const char *z)
{
    for (; *z; z++) {
        switch (*z) {
        case '<': sb_str(s, "&lt;"); break;
        case '>': sb_str(s, "&gt;"); break;
        case '&': sb_str(s, "&amp;"); break;
        case '"': sb_str(s, "&quot;"); break;
        default: sb_add(s, z, 1);
        }
    }
}

/* ---- URLs ------------------------------------------------------------------------------------ */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void pct_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (s[0] == '%' && hexval((unsigned char)s[1]) >= 0 && hexval((unsigned char)s[2]) >= 0) {
            *o++ = (char)(hexval((unsigned char)s[1]) * 16 + hexval((unsigned char)s[2]));
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static void pct_encode_path(struct sb *s, const char *z)
{
    for (; *z; z++) {
        unsigned char c = (unsigned char)*z;
        if (c <= 0x20 || c == '%' || c == '#' || c == '?' || c == '"' || c == '\'' || c == '<' || c == '>' || c == 0x7f)
            sb_printf(s, "%%%02X", c);
        else
            sb_add(s, z, 1);
    }
}

/* Length of the "scheme://authority" prefix (0 when the url has none). file:// has an empty authority. */
static size_t url_origin_len(const char *u)
{
    const char *p = strstr(u, "://");
    if (!p)
        return 0;
    for (const char *q = u; q < p; q++)
        if (!isalpha((unsigned char)*q) && !strchr("+-.", *q))
            return 0;
    p += 3;
    while (*p && *p != '/' && *p != '?' && *p != '#')
        p++;
    return (size_t)(p - u);
}

/* Remove "." / ".." segments from the path part (before '?') of an absolute url, in place. */
static void url_dot_segments(char *u)
{
    size_t ol = url_origin_len(u);
    char *path = u + ol;
    size_t plen = strcspn(path, "?");
    char *rest = strdup(path + plen);
    char *copy = strndup(path, plen);
    char *out = malloc(plen + 2);
    if (!rest || !copy || !out)
        exit(1);
    size_t on = 0;
    bool trailing = false;
    char *save = NULL;
    for (char *seg = strtok_r(copy, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
        trailing = false;
        if (!strcmp(seg, ".")) {
            trailing = true;
        } else if (!strcmp(seg, "..")) {
            while (on > 0 && out[on - 1] != '/')
                on--;
            if (on > 0)
                on--;                       /* drop the '/' in front of the removed segment */
            trailing = true;
        } else {
            out[on++] = '/';
            size_t l = strlen(seg);
            memmove(out + on, seg, l);
            on += l;
        }
    }
    if (plen > 0 && (path[plen - 1] == '/' || trailing))
        out[on++] = '/';
    if (on == 0 && plen > 0)
        out[on++] = '/';
    out[on] = 0;
    memcpy(path, out, on + 1);
    memmove(path + on, rest, strlen(rest) + 1);
    free(rest); free(copy); free(out);
}

static bool url_has_scheme(const char *s, size_t *len)
{
    const char *p = s;
    if (!isalpha((unsigned char)*p))
        return false;
    while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.')
        p++;
    if (*p != ':' || p == s)
        return false;
    if (len)
        *len = (size_t)(p - s);
    return true;
}

static bool scheme_is(const char *s, size_t len, const char *name)
{
    return strlen(name) == len && !strncasecmp(s, name, len);
}

static bool url_supported(const char *u)
{
    size_t l;
    if (!url_has_scheme(u, &l))
        return false;
    return scheme_is(u, l, "http") || scheme_is(u, l, "https") || scheme_is(u, l, "file") || scheme_is(u, l, "about");
}

/* Resolve `ref` against `base` into out. False for fragments, mailto:, javascript: ... (nothing to load). */
static bool url_resolve(const char *base, const char *ref, char *out, size_t n)
{
    while (isspace((unsigned char)*ref))
        ref++;
    if (!*ref || ref[0] == '#')
        return false;
    char tmp[2048];
    size_t sl;
    if (url_has_scheme(ref, &sl)) {
        if (!url_supported(ref))
            return false;
        snprintf(tmp, sizeof(tmp), "%s", ref);
    } else {
        char cwd[PATH_MAX], b[2048];
        if (!strncmp(base, "about:", 6)) {
            snprintf(b, sizeof(b), "file://%s/", getcwd(cwd, sizeof(cwd)) ? cwd : "");
            base = b;
        }
        size_t ol = url_origin_len(base);
        if (ol == 0)
            return false;
        char origin[512];
        snprintf(origin, sizeof(origin), "%.*s", (int)LM_MIN(ol, sizeof(origin) - 1), base);
        const char *bpath = base + ol;
        size_t bplen = strcspn(bpath, "?#");
        if (ref[0] == '/' && ref[1] == '/') {
            size_t pl = strcspn(origin, ":");
            snprintf(tmp, sizeof(tmp), "%.*s:%s", (int)pl, origin, ref);
        } else if (ref[0] == '/') {
            snprintf(tmp, sizeof(tmp), "%s%s", origin, ref);
        } else if (ref[0] == '?') {
            snprintf(tmp, sizeof(tmp), "%s%.*s%s", origin, (int)bplen, bpath, ref);
        } else {
            size_t dl = bplen;
            while (dl > 0 && bpath[dl - 1] != '/')
                dl--;
            if (dl == 0)
                snprintf(tmp, sizeof(tmp), "%s/%s", origin, ref);
            else
                snprintf(tmp, sizeof(tmp), "%s%.*s%s", origin, (int)dl, bpath, ref);
        }
    }
    char *hash = strchr(tmp, '#');
    if (hash)
        *hash = 0;
    if (url_origin_len(tmp) > 0) {
        if (!tmp[url_origin_len(tmp)] && strncasecmp(tmp, "file:", 5))
            strncat(tmp, "/", sizeof(tmp) - strlen(tmp) - 1);
        url_dot_segments(tmp);
    }
    if (strlen(tmp) >= n)
        return false;
    memcpy(out, tmp, strlen(tmp) + 1);
    return true;
}

/* What the user typed in the address bar -> a loadable url. */
static bool url_from_input(const char *typed, char *out, size_t n)
{
    while (isspace((unsigned char)*typed))
        typed++;
    char in[1024];
    snprintf(in, sizeof(in), "%s", typed);
    size_t l = strlen(in);
    while (l > 0 && isspace((unsigned char)in[l - 1]))
        in[--l] = 0;
    if (!l)
        return false;
    if (url_supported(in)) {
        if (!strncasecmp(in, "about:", 6)) {
            snprintf(out, n, "%s", in);
            return true;
        }
        return url_resolve("file:///", in, out, n);
    }
    char path[PATH_MAX], tmp[PATH_MAX];
    if (in[0] == '~' && (in[1] == '/' || !in[1]) && getenv("HOME")) {
        snprintf(tmp, sizeof(tmp), "%s%s", getenv("HOME"), in + 1);
    } else {
        snprintf(tmp, sizeof(tmp), "%s", in);
    }
    bool exists = access(tmp, F_OK) == 0;
    bool hostlike = !exists && !strchr(in, ' ') && (strchr(in, '.') || strchr(in, ':') || !strncmp(in, "localhost", 9)) &&
                    in[0] != '/' && in[0] != '.';
    if (hostlike) {
        snprintf(path, sizeof(path), "http://%s", in);
        return url_resolve(path, "", out, n) || (snprintf(out, n, "%s%s", path, strchr(in, '/') ? "" : "/"), true);
    }
    if (tmp[0] != '/') {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd)))
            return false;
        snprintf(path, sizeof(path), "%s/%s", cwd, tmp);
    } else {
        snprintf(path, sizeof(path), "%s", tmp);
    }
    struct sb s = { 0 };
    sb_str(&s, "file://");
    pct_encode_path(&s, path);
    bool ok = s.n < n;
    if (ok) {
        memcpy(out, s.p, s.n + 1);
        url_dot_segments(out);
    }
    free(s.p);
    return ok;
}

/* ---- document model ---------------------------------------------------------------------------- */

enum tok_kind { T_WORD, T_SPACE, T_BREAK, T_BLOCK, T_HR, T_BULLET };

struct tok {
    enum tok_kind kind;
    char *text;                     /* T_WORD / numbered T_BULLET; NULL otherwise */
    uint8_t bold, ital, scale, pre;
    int link;                       /* index into doc.links or -1 */
    int indent;
    int margin;                     /* T_BLOCK: blank lines (0/1) */
    uint32_t color;
};

struct doc {
    struct tok *t;
    size_t n, cap;
    char **links;
    int nlinks;
    char title[256];
};

static void doc_free(struct doc *d)
{
    for (size_t i = 0; i < d->n; i++)
        free(d->t[i].text);
    for (int i = 0; i < d->nlinks; i++)
        free(d->links[i]);
    free(d->t);
    free(d->links);
    memset(d, 0, sizeof(*d));
}

struct pstate {
    struct doc *d;
    int bold, ital, pre, scale, link, indent, heading, code, cell;
    bool in_title, pre_first, last_space;
    int ordered[16], count[16], nlist;
    int pre_col;
};

static struct tok *add_tok(struct pstate *ps, enum tok_kind kind, char *text)
{
    struct doc *d = ps->d;
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 256;
        d->t = realloc(d->t, d->cap * sizeof(*d->t));
        if (!d->t)
            exit(1);
    }
    struct tok *t = &d->t[d->n++];
    *t = (struct tok){ .kind = kind, .text = text, .bold = ps->bold > 0, .ital = ps->ital > 0,
                       .scale = (uint8_t)ps->scale, .pre = ps->pre > 0, .link = ps->link, .indent = ps->indent };
    t->color = ps->link >= 0 ? COL_LINK : ps->code ? COL_CODE : ps->heading ? COL_HEAD : COL_TEXT;
    return t;
}

static void add_block(struct pstate *ps, int margin)
{
    add_tok(ps, T_BLOCK, NULL)->margin = margin;
    ps->last_space = false;
}

static const struct { const char *name; uint32_t cp; } ENTITIES[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xa0 },
    { "copy", 0xa9 }, { "reg", 0xae }, { "deg", 0xb0 }, { "plusmn", 0xb1 }, { "middot", 0xb7 }, { "laquo", 0xab },
    { "raquo", 0xbb }, { "times", 0xd7 }, { "divide", 0xf7 }, { "auml", 0xe4 }, { "ouml", 0xf6 }, { "uuml", 0xfc },
    { "otilde", 0xf5 }, { "Auml", 0xc4 }, { "Ouml", 0xd6 }, { "Uuml", 0xdc }, { "Otilde", 0xd5 },
    { "szlig", 0xdf }, { "eacute", 0xe9 }, { "euro", 0x20ac }, { "mdash", '-' }, { "ndash", '-' },
    { "hellip", '.' }, { "lsquo", '\'' }, { "rsquo", '\'' }, { "ldquo", '"' }, { "rdquo", '"' }, { "bull", '*' },
};

static int utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xc0 | cp >> 6); out[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | cp >> 12); out[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | cp >> 18); out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* s points at '&'. On success stores the UTF-8 bytes in out, returns the number of input bytes consumed (else 0). */
static size_t decode_entity(const char *s, size_t avail, char out[8], int *outlen)
{
    if (avail < 3)
        return 0;
    size_t i = 1, max = LM_MIN(avail, (size_t)12);
    uint32_t cp = 0;
    if (s[1] == '#') {
        bool hex = s[2] == 'x' || s[2] == 'X';
        i = hex ? 3 : 2;
        size_t start = i;
        while (i < max && (hex ? hexval((unsigned char)s[i]) >= 0 : isdigit((unsigned char)s[i]))) {
            cp = cp * (hex ? 16 : 10) + (uint32_t)(hex ? hexval((unsigned char)s[i]) : s[i] - '0');
            if (cp > 0x10ffff)
                return 0;
            i++;
        }
        if (i == start || i >= max + 1 || s[i] != ';' || cp == 0)
            return 0;
        *outlen = utf8_encode(cp, out);
        return i + 1;
    }
    while (i < max && isalnum((unsigned char)s[i]))
        i++;
    if (i >= avail || s[i] != ';')
        return 0;
    for (size_t k = 0; k < LM_ARRAY_LEN(ENTITIES); k++) {
        if (strlen(ENTITIES[k].name) == i - 1 && !strncmp(s + 1, ENTITIES[k].name, i - 1)) {
            *outlen = utf8_encode(ENTITIES[k].cp, out);
            return i + 1;
        }
    }
    return 0;
}

static void title_add(struct pstate *ps, const char *s, size_t n)
{
    char *t = ps->d->title;
    size_t l = strlen(t);
    char tmp[8];
    int tl;
    for (size_t i = 0; i < n && l + 8 < sizeof(ps->d->title); i++) {
        if (s[i] == '&' && decode_entity(s + i, n - i, tmp, &tl)) {
            i += decode_entity(s + i, n - i, tmp, &tl) - 1;
            memcpy(t + l, tmp, (size_t)tl);
            l += (size_t)tl;
        } else if (isspace((unsigned char)s[i])) {
            if (l > 0 && t[l - 1] != ' ')
                t[l++] = ' ';
        } else {
            t[l++] = s[i];
        }
    }
    t[l] = 0;
}

static void parse_text(struct pstate *ps, const char *s, size_t n)
{
    if (ps->in_title) {
        title_add(ps, s, n);
        return;
    }
    char word[1024];
    size_t wl = 0;
#define FLUSH() do { if (wl) { word[wl] = 0; add_tok(ps, T_WORD, strdup(word)); wl = 0; ps->last_space = false; } } while (0)
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (wl > sizeof(word) - 16 && (c & 0xc0) != 0x80)
            FLUSH();
        if (c == '&') {
            char ent[8];
            int el;
            size_t used = decode_entity(s + i, n - i, ent, &el);
            if (used) {
                memcpy(word + wl, ent, (size_t)el);
                wl += (size_t)el;
                ps->pre_col++;
                i += used - 1;
                continue;
            }
        }
        if (ps->pre > 0) {
            if (c == '\r')
                continue;
            if (c == '\n') {
                if (ps->pre_first) {            /* a newline right after <pre> is dropped */
                    ps->pre_first = false;
                    continue;
                }
                FLUSH();
                add_tok(ps, T_BREAK, NULL);
                ps->pre_col = 0;
                continue;
            }
            ps->pre_first = false;
            if (c == '\t') {
                int pad = 8 - ps->pre_col % 8;
                for (int k = 0; k < pad && wl < sizeof(word) - 2; k++)
                    word[wl++] = ' ';
                ps->pre_col += pad;
                continue;
            }
            word[wl++] = (char)c;
            if ((c & 0xc0) != 0x80)
                ps->pre_col++;
        } else if (isspace(c)) {
            FLUSH();
            if (!ps->last_space) {
                add_tok(ps, T_SPACE, NULL);
                ps->last_space = true;
            }
        } else {
            word[wl++] = (char)c;
        }
    }
    FLUSH();
#undef FLUSH
}

/* Value of attribute `name` in the tag attribute text (decoded entities). */
static bool attr_get(const char *attrs, size_t alen, const char *name, char *out, size_t on)
{
    size_t nl = strlen(name);
    size_t i = 0;
    while (i < alen) {
        while (i < alen && (isspace((unsigned char)attrs[i]) || attrs[i] == '/'))
            i++;
        size_t ns = i;
        while (i < alen && !isspace((unsigned char)attrs[i]) && attrs[i] != '=' && attrs[i] != '/')
            i++;
        size_t nlen = i - ns;
        while (i < alen && isspace((unsigned char)attrs[i]))
            i++;
        char val[1024];
        size_t vl = 0;
        if (i < alen && attrs[i] == '=') {
            i++;
            while (i < alen && isspace((unsigned char)attrs[i]))
                i++;
            if (i < alen && (attrs[i] == '"' || attrs[i] == '\'')) {
                char q = attrs[i++];
                while (i < alen && attrs[i] != q) {
                    if (vl < sizeof(val) - 1)
                        val[vl++] = attrs[i];
                    i++;
                }
                i++;
            } else {
                while (i < alen && !isspace((unsigned char)attrs[i])) {
                    if (vl < sizeof(val) - 1)
                        val[vl++] = attrs[i];
                    i++;
                }
            }
        }
        val[vl] = 0;
        if (nlen == nl && !strncasecmp(attrs + ns, name, nl)) {
            size_t o = 0;
            for (size_t k = 0; k < vl && o + 8 < on; k++) {
                char ent[8];
                int el;
                size_t used = val[k] == '&' ? decode_entity(val + k, vl - k, ent, &el) : 0;
                if (used) {
                    memcpy(out + o, ent, (size_t)el);
                    o += (size_t)el;
                    k += used - 1;
                } else {
                    out[o++] = val[k];
                }
            }
            out[o] = 0;
            return true;
        }
    }
    return false;
}

static bool name_is(const char *n, const char *list)
{
    /* list is a space separated set of names */
    size_t nl = strlen(n);
    for (const char *p = list; *p;) {
        size_t l = strcspn(p, " ");
        if (l == nl && !strncmp(p, n, l))
            return true;
        p += l + (p[l] ? 1 : 0);
    }
    return false;
}

static void handle_tag(struct pstate *ps, const char *name, bool close, const char *attrs, size_t alen)
{
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        add_block(ps, 1);
        if (!close) {
            ps->heading++;
            ps->bold++;
            ps->scale = name[1] == '1' ? 2 : 1;
        } else {
            ps->heading = LM_MAX(ps->heading - 1, 0);
            ps->bold = LM_MAX(ps->bold - 1, 0);
            ps->scale = 1;
        }
    } else if (!strcmp(name, "p") || !strcmp(name, "table") || !strcmp(name, "address") || !strcmp(name, "figure")) {
        add_block(ps, 1);
    } else if (name_is(name, "div section article header footer nav main aside form figcaption details summary "
                              "fieldset center tr caption body html dl dt")) {
        add_block(ps, 0);
        if (!strcmp(name, "tr"))
            ps->cell = 0;
    } else if (!strcmp(name, "ul") || !strcmp(name, "ol")) {
        if (!close) {
            add_block(ps, ps->nlist == 0);
            if (ps->nlist < 15) {
                ps->ordered[ps->nlist] = name[0] == 'o';
                ps->count[ps->nlist] = 0;
                ps->nlist++;
            }
            ps->indent++;
        } else {
            if (ps->nlist > 0)
                ps->nlist--;
            ps->indent = LM_MAX(ps->indent - 1, 0);
            add_block(ps, ps->nlist == 0);
        }
    } else if (!strcmp(name, "li")) {
        if (!close) {
            add_block(ps, 0);
            char *num = NULL;
            if (ps->nlist > 0 && ps->ordered[ps->nlist - 1]) {
                char b[16];
                snprintf(b, sizeof(b), "%d.", ++ps->count[ps->nlist - 1]);
                num = strdup(b);
            }
            add_tok(ps, T_BULLET, num);
        }
    } else if (!strcmp(name, "dd") || !strcmp(name, "blockquote")) {
        add_block(ps, name[0] == 'b');
        ps->indent = LM_MAX(ps->indent + (close ? -1 : 1), 0);
        if (close)
            add_block(ps, name[0] == 'b');
    } else if (!strcmp(name, "pre")) {
        add_block(ps, 1);
        ps->pre = LM_MAX(ps->pre + (close ? -1 : 1), 0);
        ps->pre_first = !close;
        ps->pre_col = 0;
    } else if (name_is(name, "code tt kbd samp")) {
        ps->code = LM_MAX(ps->code + (close ? -1 : 1), 0);
    } else if (name_is(name, "b strong th")) {
        ps->bold = LM_MAX(ps->bold + (close ? -1 : 1), 0);
        if (!strcmp(name, "th") && !close) {
            if (ps->cell++ > 0)
                add_tok(ps, T_WORD, strdup("|"))->color = COL_GRAY;
        }
    } else if (name_is(name, "i em cite var dfn")) {
        ps->ital = LM_MAX(ps->ital + (close ? -1 : 1), 0);
    } else if (!strcmp(name, "td")) {
        if (!close && ps->cell++ > 0)
            add_tok(ps, T_WORD, strdup("|"))->color = COL_GRAY;
    } else if (!strcmp(name, "a")) {
        if (close) {
            ps->link = -1;
        } else {
            char href[1024];
            if (attr_get(attrs, alen, "href", href, sizeof(href))) {
                struct doc *d = ps->d;
                d->links = realloc(d->links, (size_t)(d->nlinks + 1) * sizeof(*d->links));
                if (!d->links)
                    exit(1);
                d->links[d->nlinks] = strdup(href);
                ps->link = d->nlinks++;
            } else {
                ps->link = -1;
            }
        }
    } else if (!strcmp(name, "br")) {
        add_tok(ps, T_BREAK, NULL);
        ps->last_space = false;
    } else if (!strcmp(name, "hr")) {
        add_block(ps, 0);
        add_tok(ps, T_HR, NULL);
        add_block(ps, 0);
    } else if (!strcmp(name, "img")) {
        char alt[256], label[300];
        if (!attr_get(attrs, alen, "alt", alt, sizeof(alt)) || !alt[0])
            snprintf(alt, sizeof(alt), "image");
        snprintf(label, sizeof(label), "[%s]", alt);
        add_tok(ps, T_WORD, strdup(label))->color = ps->link >= 0 ? COL_LINK : COL_GRAY;
        ps->last_space = false;
    } else if (!strcmp(name, "title")) {
        ps->in_title = !close;
        if (!close)
            ps->d->title[0] = 0;
    }
}

static const char *find_close_tag(const char *s, const char *end, const char *name)
{
    size_t nl = strlen(name);
    for (; s + 2 + nl <= end; s++)
        if (s[0] == '<' && s[1] == '/' && !strncasecmp(s + 2, name, nl))
            return s;
    return end;
}

/* Parse markup (or plain text when `plain`) into d. */
static void doc_parse(struct doc *d, const char *src, size_t len, bool plain)
{
    memset(d, 0, sizeof(*d));
    struct pstate ps = { .d = d, .scale = 1, .link = -1 };
    const char *s = src, *end = src + len;
    if (plain) {
        ps.pre = 1;
        char *z = strndup(src, len);
        if (!z)
            exit(1);
        /* escape-free: feed the text as-is, tags are not interpreted */
        for (const char *p = z; *p;) {
            size_t l = strcspn(p, "&");
            parse_text(&ps, p, l);
            p += l;
            if (*p == '&') {
                char amp[] = "&amp;";
                parse_text(&ps, amp, 5);
                p++;
            }
        }
        free(z);
        return;
    }
    while (s < end) {
        if (*s != '<') {
            const char *e = memchr(s, '<', (size_t)(end - s));
            if (!e)
                e = end;
            parse_text(&ps, s, (size_t)(e - s));
            s = e;
            continue;
        }
        if (end - s >= 4 && !strncmp(s, "<!--", 4)) {
            const char *e = strstr(s + 4, "-->");
            s = e && e + 3 <= end ? e + 3 : end;
            continue;
        }
        bool close = s + 1 < end && s[1] == '/';
        const char *nm = s + 1 + (close ? 1 : 0);
        if (nm >= end || !isalpha((unsigned char)*nm)) {
            if (nm < end && (*nm == '!' || *nm == '?')) {      /* doctype, processing instruction */
                const char *e = memchr(s, '>', (size_t)(end - s));
                s = e ? e + 1 : end;
            } else {
                parse_text(&ps, "<", 1);
                s++;
            }
            continue;
        }
        char name[16];
        size_t nl = 0;
        const char *p = nm;
        while (p < end && (isalnum((unsigned char)*p) || *p == '-')) {
            if (nl < sizeof(name) - 1)
                name[nl++] = (char)tolower((unsigned char)*p);
            p++;
        }
        name[nl] = 0;
        const char *attrs = p;
        char q = 0;
        while (p < end && (q || *p != '>')) {
            if (q && *p == q)
                q = 0;
            else if (!q && (*p == '"' || *p == '\''))
                q = *p;
            p++;
        }
        size_t alen = (size_t)(p - attrs);
        s = p < end ? p + 1 : end;
        if (!close && (!strcmp(name, "script") || !strcmp(name, "style"))) {
            const char *e = find_close_tag(s, end, name);
            const char *gt = e < end ? memchr(e, '>', (size_t)(end - e)) : NULL;
            s = gt ? gt + 1 : end;
            continue;
        }
        handle_tag(&ps, name, close, attrs, alen);
    }
}

/* ---- layout ---------------------------------------------------------------------------------- */

enum run_kind { R_TEXT, R_HR, R_BULLET };

struct run {
    int x, y, w, h;
    enum run_kind kind;
    const char *text;
    int len;
    const struct tok *t;
};

struct layout {
    struct run *r;
    size_t n, cap;
    int height;
};

struct lstate {
    struct layout *L;
    int width, y, x, left, line_h, gap;
    size_t line_start;
    bool open, space;
};

static struct run *add_run(struct layout *L)
{
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 256;
        L->r = realloc(L->r, L->cap * sizeof(*L->r));
        if (!L->r)
            exit(1);
    }
    return &L->r[L->n++];
}

static void ls_finish(struct lstate *ls)
{
    if (!ls->open)
        return;
    int lh = ls->line_h > 0 ? ls->line_h : 18;
    for (size_t i = ls->line_start; i < ls->L->n; i++)
        ls->L->r[i].y = ls->y + lh - ls->L->r[i].h;
    ls->y += lh;
    ls->open = false;
    ls->space = false;
}

static void ls_start(struct lstate *ls, int indent)
{
    if (ls->open)
        return;
    ls->y += ls->gap;
    ls->gap = 0;
    ls->left = PAGE_PAD + indent * INDENT_PX;
    ls->x = ls->left;
    ls->line_h = 0;
    ls->line_start = ls->L->n;
    ls->open = true;
    ls->space = false;
}

static void doc_layout(const struct doc *d, int width, struct layout *L)
{
    L->n = 0;
    struct lstate ls = { .L = L, .width = width, .y = 10 };
    int right = width - PAGE_PAD;
    for (size_t i = 0; i < d->n; i++) {
        const struct tok *t = &d->t[i];
        switch (t->kind) {
        case T_BLOCK:
            ls_finish(&ls);
            ls.gap = LM_MAX(ls.gap, t->margin * 12);
            break;
        case T_BREAK:
            if (ls.open) {
                ls_finish(&ls);
            } else {
                ls.y += ls.gap + 18 * t->scale;
                ls.gap = 0;
            }
            break;
        case T_SPACE:
            ls.space = ls.open && ls.x > ls.left;
            break;
        case T_HR: {
            ls_finish(&ls);
            ls.y += ls.gap + 6;
            ls.gap = 0;
            struct run *r = add_run(L);
            *r = (struct run){ .x = PAGE_PAD, .y = ls.y, .w = LM_MAX(width - 2 * PAGE_PAD, 0), .h = 1, .kind = R_HR, .t = t };
            ls.y += 8;
            break;
        }
        case T_BULLET: {
            ls_start(&ls, t->indent);
            struct run *r = add_run(L);
            if (t->text) {
                int w = lm_font_text_width(t->text);
                *r = (struct run){ .x = ls.left - w - 6, .w = w, .h = 16, .kind = R_TEXT, .text = t->text,
                                   .len = (int)strlen(t->text), .t = t };
            } else {
                *r = (struct run){ .x = ls.left - 16, .w = 6, .h = 6, .kind = R_BULLET, .t = t };
                r->h = 16;                      /* bottom aligned with the text line; painted as a square */
            }
            ls.line_h = LM_MAX(ls.line_h, 18);
            break;
        }
        case T_WORD: {
            int scale = t->scale ? t->scale : 1;
            const char *text = t->text;
            int total = (int)strlen(text);
            int pos = 0;
            ls_start(&ls, t->indent);
            while (pos < total) {
                int w = lm_font_text_width(text + pos) * scale;
                int sp = ls.space && ls.x > ls.left ? LM_FONT_W * scale : 0;
                int avail = right - ls.x - sp;
                if (!t->pre && w > avail && ls.x > ls.left) {
                    ls_finish(&ls);
                    ls_start(&ls, t->indent);
                    continue;
                }
                int take = total - pos;
                if (!t->pre && w > right - ls.left) {          /* longer than a whole line: cut at char boundary */
                    int fit = LM_MAX((right - ls.x - sp) / (LM_FONT_W * scale), 1), used_b = 0, cnt = 0;
                    while (used_b < total - pos && cnt < fit) {
                        int u;
                        lm_utf8_decode(text + pos + used_b, total - pos - used_b, &u);
                        used_b += LM_MAX(u, 1);
                        cnt++;
                    }
                    take = used_b;
                    char *tmp = strndup(text + pos, (size_t)take);
                    w = lm_font_text_width(tmp) * scale;
                    free(tmp);
                }
                struct run *r = add_run(L);
                *r = (struct run){ .x = ls.x + sp, .w = w, .h = LM_FONT_H * scale, .kind = R_TEXT, .text = text + pos,
                                   .len = take, .t = t };
                ls.x += sp + w;
                ls.space = false;
                ls.line_h = LM_MAX(ls.line_h, 18 * scale);
                pos += take;
                if (pos < total) {
                    ls_finish(&ls);
                    ls_start(&ls, t->indent);
                }
            }
            break;
        }
        }
    }
    ls_finish(&ls);
    L->height = ls.y + PAGE_PAD;
}

/* ---- loading -------------------------------------------------------------------------------- */

struct page {
    char *body;
    size_t len;
    bool html;
    char url[2048];                 /* final url (after redirects) */
};

static void page_error(struct page *p, const char *title, const char *fmt, ...) LM_PRINTF(3, 4);
static void page_error(struct page *p, const char *title, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    struct sb s = { 0 };
    sb_str(&s, "<html><head><title>");
    sb_escape(&s, title);
    sb_str(&s, "</title></head><body><h1>");
    sb_escape(&s, title);
    sb_str(&s, "</h1><p>");
    sb_escape(&s, msg);
    sb_str(&s, "</p><p><a href=\"about:home\">Home</a></p></body></html>");
    free(p->body);
    p->body = s.p;
    p->len = s.n;
    p->html = true;
}

static void page_home(struct page *p)
{
    char cwd[PATH_MAX];
    struct sb s = { 0 };
    sb_str(&s,
           "<html><head><title>Lumen Web</title></head><body>"
           "<h1>Lumen Web</h1>"
           "<p>A tiny browser that runs on the <b>lumen</b> Wayland compositor. It speaks <code>file://</code> and "
           "plain <code>http://</code>, understands a subset of HTML and has no JavaScript, CSS or TLS.</p>"
           "<h2>Try</h2><ul>");
    if (getcwd(cwd, sizeof(cwd))) {
        sb_str(&s, "<li><a href=\"file://");
        pct_encode_path(&s, cwd);
        sb_str(&s, "/\">The current directory</a> (");
        sb_escape(&s, cwd);
        sb_str(&s, ")</li>");
    }
    sb_str(&s,
           "<li><a href=\"file:///\">The file system root</a></li>"
           "<li>Type an address in the bar above: a path, <code>file:///...</code> or <code>http://host/</code>.</li>"
           "</ul><h2>Keys</h2><pre>"
           "Ctrl+L / F6      address bar        Enter  go            Esc  leave the bar\n"
           "Alt+Left, Bksp   back               Alt+Right  forward   F5 / Ctrl+R  reload\n"
           "Up Down PgUp PgDn Home End Space    scroll (also j / k and the mouse wheel)\n"
           "</pre><hr><p>Example list:</p><ol><li>one</li><li>two<ul><li>nested</li></ul></li><li>three</li></ol>"
           "<blockquote>A quoted paragraph is indented.</blockquote>"
           "</body></html>");
    free(p->body);
    p->body = s.p;
    p->len = s.n;
    p->html = true;
}

static int name_cmp(const struct dirent **a, const struct dirent **b) { return strcmp((*a)->d_name, (*b)->d_name); }

static void page_dir(struct page *p, const char *path, const char *url)
{
    struct dirent **list = NULL;
    int n = scandir(path, &list, NULL, name_cmp);
    if (n < 0) {
        page_error(p, "Cannot open directory", "%s: %s", path, strerror(errno));
        return;
    }
    struct sb s = { 0 };
    sb_str(&s, "<html><head><title>Index of ");
    sb_escape(&s, path);
    sb_str(&s, "</title></head><body><h1>Index of ");
    sb_escape(&s, path);
    sb_str(&s, "</h1><pre>");
    for (int i = 0; i < n; i++) {
        const char *nm = list[i]->d_name;
        if (!strcmp(nm, ".") || (nm[0] == '.' && strcmp(nm, "..")))
            goto next;
        char full[PATH_MAX];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", path, nm);
        bool dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        sb_str(&s, "<a href=\"");
        pct_encode_path(&s, nm);
        if (dir)
            sb_str(&s, "/");
        sb_str(&s, "\">");
        sb_escape(&s, nm);
        if (dir)
            sb_str(&s, "/");
        sb_str(&s, "</a>");
        if (!dir && stat(full, &st) == 0)
            sb_printf(&s, "  (%lld bytes)", (long long)st.st_size);
        sb_str(&s, "\n");
next:
        free(list[i]);
    }
    free(list);
    sb_str(&s, "</pre></body></html>");
    (void)url;
    free(p->body);
    p->body = s.p;
    p->len = s.n;
    p->html = true;
}

static bool looks_like_html(const char *path, const char *body, size_t len)
{
    const char *dot = strrchr(path, '.');
    if (dot && (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm") || !strcasecmp(dot, ".xhtml")))
        return true;
    char head[512];
    size_t n = LM_MIN(len, sizeof(head) - 1);
    memcpy(head, body, n);
    head[n] = 0;
    return strcasestr(head, "<html") || strcasestr(head, "<!doctype html") || strcasestr(head, "<body") ||
           strcasestr(head, "<h1") || strcasestr(head, "<p>") || strcasestr(head, "<title");
}

static void load_file(struct page *p, const char *url)
{
    char path[PATH_MAX];
    size_t ol = url_origin_len(url);
    snprintf(path, sizeof(path), "%s", url + ol);
    path[strcspn(path, "?")] = 0;
    pct_decode(path);
    struct stat st;
    if (stat(path, &st) < 0) {
        page_error(p, "File not found", "%s: %s", path, strerror(errno));
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        if (path[strlen(path) - 1] != '/' && strlen(p->url) + 1 < sizeof(p->url))
            strcat(p->url, "/");
        page_dir(p, path, url);
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        page_error(p, "Cannot read file", "%s: %s", path, strerror(errno));
        return;
    }
    size_t cap = (size_t)LM_MIN(st.st_size > 0 ? st.st_size : 4096, (off_t)MAX_BODY);
    char *buf = malloc(cap + 1);
    size_t len = buf ? fread(buf, 1, cap, f) : 0;
    fclose(f);
    if (!buf)
        exit(1);
    buf[len] = 0;
    free(p->body);
    p->body = buf;
    p->len = len;
    p->html = looks_like_html(path, buf, len);
}

static int tcp_connect(const char *host, const char *port, int timeout_ms)
{
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
        return -1;
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        if (errno == EINPROGRESS) {
            struct pollfd pf = { .fd = fd, .events = POLLOUT };
            int err = 0;
            socklen_t el = sizeof(err);
            if (poll(&pf, 1, timeout_ms) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0)
                break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Decode a chunked body in place; returns the new length. */
static size_t dechunk(char *b, size_t len)
{
    size_t in = 0, out = 0;
    while (in < len) {
        char *e;
        unsigned long sz = strtoul(b + in, &e, 16);
        char *nl = memchr(e, '\n', len - (size_t)(e - b));
        if (!nl)
            break;
        in = (size_t)(nl - b) + 1;
        if (sz == 0 || in + sz > len)
            sz = LM_MIN(sz, (unsigned long)(len - in));
        memmove(b + out, b + in, sz);
        out += sz;
        in += sz;
        if (sz == 0)
            break;
        while (in < len && (b[in] == '\r' || b[in] == '\n'))
            in++;
    }
    return out;
}

/* One HTTP GET. Returns the status (>0), or 0 with err filled in. */
static int http_get(const char *url, struct page *p, char *location, size_t locn, char *err, size_t errn)
{
    size_t ol = url_origin_len(url);
    char host[256], port[8] = "80";
    const char *auth = url + 7, *authend = url + ol;
    size_t al = (size_t)(authend - auth);
    snprintf(host, sizeof(host), "%.*s", (int)LM_MIN(al, sizeof(host) - 1), auth);
    char *colon = strrchr(host, ':');
    if (colon && !strchr(colon, ']')) {
        snprintf(port, sizeof(port), "%s", colon + 1);
        *colon = 0;
    }
    const char *path = url + ol;
    char req[4096];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %.*s\r\nUser-Agent: lumen-web/1.0\r\nAccept: text/html, text/plain;q=0.9\r\n"
             "Accept-Encoding: identity\r\nConnection: close\r\n\r\n",
             *path ? path : "/", (int)al, auth);
    long long deadline = now_ms() + FETCH_TIMEOUT_MS;
    int fd = tcp_connect(host, port, 5000);
    if (fd < 0) {
        snprintf(err, errn, "cannot connect to %s:%s", host, port);
        return 0;
    }
    size_t sent = 0, rl = strlen(req);
    while (sent < rl) {
        struct pollfd pf = { .fd = fd, .events = POLLOUT };
        if (poll(&pf, 1, (int)LM_MAX(deadline - now_ms(), 0LL)) <= 0) {
            snprintf(err, errn, "timeout sending the request");
            close(fd);
            return 0;
        }
        ssize_t w = send(fd, req + sent, rl - sent, MSG_NOSIGNAL);
        if (w < 0 && errno != EAGAIN && errno != EINTR) {
            snprintf(err, errn, "send: %s", strerror(errno));
            close(fd);
            return 0;
        }
        if (w > 0)
            sent += (size_t)w;
    }
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap + 1);
    if (!buf)
        exit(1);
    for (;;) {
        long long left = deadline - now_ms();
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        if (left <= 0 || poll(&pf, 1, (int)left) <= 0) {
            if (len == 0) {
                snprintf(err, errn, "timeout waiting for %s", host);
                free(buf);
                close(fd);
                return 0;
            }
            break;
        }
        if (len == cap) {
            if (cap >= MAX_BODY)
                break;
            cap *= 2;
            buf = realloc(buf, cap + 1);
            if (!buf)
                exit(1);
        }
        ssize_t r = recv(fd, buf + len, cap - len, 0);
        if (r < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (r <= 0)
            break;
        len += (size_t)r;
    }
    close(fd);
    buf[len] = 0;
    char *hend = strstr(buf, "\r\n\r\n");
    size_t hl = hend ? (size_t)(hend - buf) + 4 : 0;
    int status = 0;
    if (!hend || sscanf(buf, "HTTP/%*d.%*d %d", &status) != 1) {
        snprintf(err, errn, "not an HTTP response");
        free(buf);
        return 0;
    }
    char headers[8192];
    snprintf(headers, sizeof(headers), "%.*s", (int)LM_MIN(hl, sizeof(headers) - 1), buf);
    location[0] = 0;
    char *hl_ = strcasestr(headers, "\nlocation:");
    if (hl_) {
        hl_ += 10;
        while (*hl_ == ' ')
            hl_++;
        snprintf(location, locn, "%.*s", (int)strcspn(hl_, "\r\n"), hl_);
    }
    bool html = true;
    char *ct = strcasestr(headers, "\ncontent-type:");
    if (ct) {
        ct += 14;
        char type[128];
        snprintf(type, sizeof(type), "%.*s", (int)strcspn(ct, "\r\n"), ct);
        if (strcasestr(type, "text/html") || strcasestr(type, "xhtml"))
            html = true;
        else if (strcasestr(type, "text/") || strcasestr(type, "json") || strcasestr(type, "xml") ||
                 strcasestr(type, "javascript"))
            html = false;
        else {
            snprintf(err, errn, "cannot display content of type%.*s", (int)strlen(type), type);
            free(buf);
            return 0;
        }
    }
    size_t bl = len - hl;
    memmove(buf, buf + hl, bl + 1);
    if (strcasestr(headers, "\ntransfer-encoding: chunked"))
        bl = dechunk(buf, bl);
    buf[bl] = 0;
    free(p->body);
    p->body = buf;
    p->len = bl;
    p->html = html;
    return status;
}

/* Fetch `url` (absolute). Always yields a page (errors become pages). */
static void page_load(struct page *p, const char *url)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->url, sizeof(p->url), "%s", url);
    if (!strncasecmp(url, "about:", 6)) {
        page_home(p);
        snprintf(p->url, sizeof(p->url), "about:home");
    } else if (!strncasecmp(url, "file:", 5)) {
        load_file(p, url);
    } else if (!strncasecmp(url, "https:", 6)) {
        page_error(p, "https is not supported", "This browser has no TLS library, so it cannot open %s. "
                                                "Plain http:// and file:// pages work.", url);
    } else if (!strncasecmp(url, "http:", 5)) {
        char cur[2048], loc[2048], err[256] = "";
        snprintf(cur, sizeof(cur), "%s", url);
        for (int hop = 0; hop < 6; hop++) {
            int st = http_get(cur, p, loc, sizeof(loc), err, sizeof(err));
            if (st == 0) {
                page_error(p, "Cannot load page", "%s: %s", cur, err);
                break;
            }
            snprintf(p->url, sizeof(p->url), "%s", cur);
            if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && loc[0]) {
                char next[2048];
                if (!url_resolve(cur, loc, next, sizeof(next))) {
                    page_error(p, "Bad redirect", "Location: %s", loc);
                    break;
                }
                if (!strncasecmp(next, "https:", 6)) {
                    page_error(p, "https is not supported", "The server redirected to %s, which needs TLS.", next);
                    break;
                }
                snprintf(cur, sizeof(cur), "%s", next);
                continue;
            }
            if (st >= 400) {
                page_error(p, "HTTP error", "%s answered with status %d.", cur, st);
            }
            break;
        }
    } else {
        page_error(p, "Unknown address", "Cannot open %s", url);
    }
}

/* ---- selftest -------------------------------------------------------------------------------- */

static int selftest_fail;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "selftest FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); selftest_fail++; } } while (0)

static void doc_words(const struct doc *d, char *out, size_t n)
{
    size_t o = 0;
    out[0] = 0;
    for (size_t i = 0; i < d->n && o + 2 < n; i++) {
        const struct tok *t = &d->t[i];
        if (t->kind == T_WORD)
            o += (size_t)snprintf(out + o, n - o, "%s", t->text);
        else if (t->kind == T_SPACE)
            o += (size_t)snprintf(out + o, n - o, " ");
        else if (t->kind == T_BREAK)
            o += (size_t)snprintf(out + o, n - o, "\\n");
        else if (t->kind == T_BLOCK)
            o += (size_t)snprintf(out + o, n - o, "/");
    }
}

static void expect_resolve(const char *base, const char *ref, const char *want)
{
    char out[2048] = "";
    bool ok = url_resolve(base, ref, out, sizeof(out));
    if (want)
        CHECK(ok && !strcmp(out, want), "resolve(%s, %s) = %s, want %s", base, ref, ok ? out : "(none)", want);
    else
        CHECK(!ok, "resolve(%s, %s) should fail, got %s", base, ref, out);
}

static int selftest(void)
{
    struct doc d;
    char w[512];
    const char *h1 = "<!DOCTYPE html><html><head><title>My &amp; Page</title><style>p{x}</style></head><body>"
                     "<h1>Hi &amp; bye</h1><!-- c --><p>a <a href='x.html?a=1&amp;b=2'>link</a>  b</p>"
                     "<script>if (a<b) document.write('<p>no</p>')</script><p>&lt;tag&gt; &#65;&#x42;&euro;</p></body></html>";
    doc_parse(&d, h1, strlen(h1), false);
    doc_words(&d, w, sizeof(w));
    CHECK(!strcmp(d.title, "My & Page"), "title '%s'", d.title);
    CHECK(!strstr(w, "document") && !strstr(w, "p{x}"), "script/style leaked: %s", w);
    CHECK(strstr(w, "/Hi & bye/") != NULL, "heading: %s", w);
    CHECK(strstr(w, "a link b") != NULL, "paragraph: %s", w);
    CHECK(strstr(w, "<tag> AB\xe2\x82\xac") != NULL, "entities: %s", w);
    CHECK(d.nlinks == 1 && !strcmp(d.links[0], "x.html?a=1&b=2"), "link %d", d.nlinks);
    int linked = 0;
    for (size_t i = 0; i < d.n; i++)
        linked += d.t[i].kind == T_WORD && d.t[i].link == 0;
    CHECK(linked == 1, "linked words %d", linked);
    doc_free(&d);

    const char *h2 = "<ul><li>one</li><li>two<ol><li>x</li></ol></li></ul><pre>\n a  b\n\tc</pre>";
    doc_parse(&d, h2, strlen(h2), false);
    int bullets = 0, numbered = 0, breaks = 0;
    bool spaces_kept = false;
    for (size_t i = 0; i < d.n; i++) {
        bullets += d.t[i].kind == T_BULLET && !d.t[i].text;
        numbered += d.t[i].kind == T_BULLET && d.t[i].text && !strcmp(d.t[i].text, "1.");
        breaks += d.t[i].kind == T_BREAK;
        spaces_kept |= d.t[i].kind == T_WORD && !strcmp(d.t[i].text, " a  b");
    }
    CHECK(bullets == 2 && numbered == 1, "bullets %d numbered %d", bullets, numbered);
    CHECK(breaks == 1 && spaces_kept, "pre: breaks %d spaces_kept %d", breaks, spaces_kept);
    doc_free(&d);

    const char *h3 = "<p>The quick brown fox jumps over the lazy dog and keeps running through the forest "
                     "supercalifragilisticexpialidocious_supercalifragilisticexpialidocious_end</p>";
    doc_parse(&d, h3, strlen(h3), false);
    struct layout L = { 0 };
    doc_layout(&d, 240, &L);
    int maxr = 0, lines = 0, lasty = -1;
    for (size_t i = 0; i < L.n; i++) {
        maxr = LM_MAX(maxr, L.r[i].x + L.r[i].w);
        if (L.r[i].y != lasty) {
            lines++;
            lasty = L.r[i].y;
        }
    }
    CHECK(maxr <= 240 - PAGE_PAD, "text overflows the width: %d", maxr);
    CHECK(lines >= 5, "expected wrapping, got %d lines", lines);
    CHECK(L.height > lines * 16, "height %d", L.height);
    free(L.r);
    doc_free(&d);

    const char *plain = "a <b> & c\nline2";
    doc_parse(&d, plain, strlen(plain), true);
    doc_words(&d, w, sizeof(w));
    CHECK(strstr(w, "a <b> & c\\nline2") != NULL, "plain: %s", w);
    doc_free(&d);

    expect_resolve("http://h/a/b.html", "c.html", "http://h/a/c.html");
    expect_resolve("http://h/a/b.html", "../x", "http://h/x");
    expect_resolve("http://h/a/b.html", "/y?q=1", "http://h/y?q=1");
    expect_resolve("http://h/a/b.html", "//z/q", "http://z/q");
    expect_resolve("http://h", "p", "http://h/p");
    expect_resolve("http://h/a/b.html?x=1", "?y=2", "http://h/a/b.html?y=2");
    expect_resolve("http://h/a/", "./d/./e/../f.html#top", "http://h/a/d/f.html");
    expect_resolve("file:///d/e.html", "f/g.html", "file:///d/f/g.html");
    expect_resolve("file:///d/e.html", "/abs.html", "file:///abs.html");
    expect_resolve("http://h/", "http://other.org", "http://other.org/");
    expect_resolve("http://h/", "mailto:a@b", NULL);
    expect_resolve("http://h/", "javascript:void(0)", NULL);
    expect_resolve("http://h/", "#frag", NULL);

    char u[2048];
    CHECK(url_from_input("/tmp", u, sizeof(u)) && !strcmp(u, "file:///tmp"), "input /tmp -> %s", u);
    CHECK(url_from_input("example.com", u, sizeof(u)) && !strcmp(u, "http://example.com/"), "input host -> %s", u);
    CHECK(url_from_input("localhost:8000/x", u, sizeof(u)) && !strcmp(u, "http://localhost:8000/x"), "input lh -> %s", u);
    CHECK(url_from_input("about:home", u, sizeof(u)) && !strcmp(u, "about:home"), "input about -> %s", u);

    char dec[] = "a%20b%2Fc%zz";
    pct_decode(dec);
    CHECK(!strcmp(dec, "a b/c%zz"), "pct_decode '%s'", dec);

    if (selftest_fail == 0)
        puts("lumen-web selftest ok");
    return selftest_fail ? 1 : 0;
}

/* ---- application ------------------------------------------------------------------------------ */

struct rect { int x, y, w, h; };

struct hist { char url[1024]; int scroll; };

struct app {
    struct wlc *c;
    uint32_t registry, compositor, shm, wm_base, seat, pointer, keyboard;
    uint32_t surface, xsurface, toplevel, pool, buffer;
    int w, h, bw, bh;
    uint32_t *pixels;
    size_t map_size;
    bool configured, running, buffer_busy, redraw;
    uint32_t mods;
    double px, py;
    bool has_pointer;

    struct doc doc;
    struct layout layout;
    int layout_w;
    int scroll;
    char cur_url[2048];
    char status[300];
    int hover_link;
    int press_link;
    int press_btn;                  /* toolbar button pressed: 0 back, 1 forward, 2 reload, 3 address, -1 none */

    struct hist hist[100];
    int nhist, hpos;

    char addr[1024];
    int cursor;                     /* byte offset in addr */
    bool addr_focus, addr_fresh;
    char title[300];
};

static struct rect btn_rect(int i) { return (struct rect){ 6 + i * 30, 4, 26, 26 }; }
static struct rect addr_rect(const struct app *a) { return (struct rect){ 6 + 3 * 30 + 4, 4, a->bw - (6 + 3 * 30 + 4) - 6, 26 }; }
static int view_h(const struct app *a) { return LM_MAX(a->bh - TOOLBAR_H - STATUS_H, 1); }
static int max_scroll(const struct app *a) { return LM_MAX(a->layout.height - view_h(a), 0); }

static bool in_rect(struct rect r, double x, double y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void fill(struct app *a, struct rect r, uint32_t color)
{
    for (int y = LM_MAX(r.y, 0); y < LM_MIN(r.y + r.h, a->bh); y++)
        for (int x = LM_MAX(r.x, 0); x < LM_MIN(r.x + r.w, a->bw); x++)
            a->pixels[(size_t)y * (size_t)a->bw + (size_t)x] = 0xff000000u | color;
}

static void put_px(struct app *a, struct rect clip, int x, int y, uint32_t color)
{
    if (x >= clip.x && x < clip.x + clip.w && y >= clip.y && y < clip.y + clip.h && x >= 0 && x < a->bw && y >= 0 && y < a->bh)
        a->pixels[(size_t)y * (size_t)a->bw + (size_t)x] = 0xff000000u | color;
}

static void draw_text(struct app *a, struct rect clip, int x, int y, const char *s, size_t len, int scale, uint32_t color,
                      bool bold, bool ital)
{
    for (size_t i = 0; i < len;) {
        int used;
        uint32_t cp = lm_utf8_decode(s + i, (int)(len - i), &used);
        i += used > 0 ? (size_t)used : 1;
        if (x + LM_FONT_W * scale > clip.x && x < clip.x + clip.w) {
            const uint8_t *g = lm_font_glyph(cp);
            for (int r = 0; r < LM_FONT_H * scale; r++) {
                int py = y + r;
                if (py < clip.y || py >= clip.y + clip.h)
                    continue;
                int shear = ital ? (LM_FONT_H - 1 - r / scale) / 5 : 0;
                for (int cx = 0; cx < LM_FONT_W * scale; cx++) {
                    if (g[r / scale] & (0x80 >> (cx / scale))) {
                        put_px(a, clip, x + cx + shear, py, color);
                        if (bold)
                            put_px(a, clip, x + cx + shear + 1, py, color);
                    }
                }
            }
        }
        x += LM_FONT_W * scale;
    }
}

static void draw_label(struct app *a, struct rect r, const char *s, uint32_t color)
{
    int tw = lm_font_text_width(s);
    draw_text(a, r, r.x + (r.w - tw) / 2, r.y + (r.h - LM_FONT_H) / 2, s, strlen(s), 1, color, false, false);
}

static const char *btn_label(int i) { return i == 0 ? "<" : i == 1 ? ">" : "R"; }

static bool btn_enabled(const struct app *a, int i)
{
    return i == 0 ? a->hpos > 0 : i == 1 ? a->hpos + 1 < a->nhist : true;
}

/* Index of the first visible char of the address text so that the cursor stays in view. */
static int addr_first_char(const struct app *a, int visible)
{
    int cursor_chars = 0;
    for (int i = 0; i < a->cursor; i++)
        cursor_chars += (a->addr[i] & 0xc0) != 0x80;
    return LM_MAX(cursor_chars - visible + 1, 0);
}

static int link_at(const struct app *a, double x, double y)
{
    if (y < TOOLBAR_H || y >= a->bh - STATUS_H)
        return -1;
    int py = (int)y - TOOLBAR_H + a->scroll;
    for (size_t i = 0; i < a->layout.n; i++) {
        const struct run *r = &a->layout.r[i];
        if (r->kind == R_TEXT && r->t->link >= 0 && x >= r->x && x < r->x + r->w && py >= r->y && py < r->y + r->h)
            return r->t->link;
    }
    return -1;
}

static void paint(struct app *a)
{
    fill(a, (struct rect){ 0, 0, a->bw, a->bh }, COL_BG);

    /* page */
    struct rect view = { 0, TOOLBAR_H, a->bw, view_h(a) };
    fill(a, view, COL_PAGE);
    for (size_t i = 0; i < a->layout.n; i++) {
        const struct run *r = &a->layout.r[i];
        int sy = view.y + r->y - a->scroll;
        if (sy + r->h < view.y || sy > view.y + view.h)
            continue;
        if (r->kind == R_HR) {
            fill(a, (struct rect){ r->x, sy, r->w, 1 }, COL_RULE);
        } else if (r->kind == R_BULLET) {
            struct rect sq = { r->x, sy + 6, 6, 6 };
            fill(a, (struct rect){ sq.x, LM_MAX(sq.y, view.y), sq.w, LM_MIN(sq.h, view.y + view.h - sq.y) }, COL_TEXT);
        } else {
            const struct tok *t = r->t;
            uint32_t col = t->color;
            if (t->link >= 0 && t->link == a->hover_link)
                col = 0x1f7bff;
            draw_text(a, view, r->x, sy, r->text, (size_t)r->len, t->scale ? t->scale : 1, col, t->bold, t->ital);
            if (t->link >= 0 && sy + r->h - 2 >= view.y && sy + r->h - 2 < view.y + view.h)
                fill(a, (struct rect){ r->x, sy + r->h - 2, r->w, 1 }, col);
        }
    }
    int ms = max_scroll(a);
    if (ms > 0) {
        int th = LM_MAX(view.h * view.h / a->layout.height, 20);
        int ty = view.y + (int)((long long)(view.h - th) * a->scroll / ms);
        fill(a, (struct rect){ a->bw - 6, ty, 4, th }, 0xa9aebb);
    }

    /* toolbar */
    for (int i = 0; i < 3; i++) {
        struct rect b = btn_rect(i);
        bool en = btn_enabled(a, i);
        bool hov = a->has_pointer && en && in_rect(b, a->px, a->py);
        fill(a, b, hov ? (a->press_btn == i ? COL_ACCENT : COL_HOVER) : COL_BTN);
        draw_label(a, b, btn_label(i), en ? COL_UI_TEXT : COL_UI_DIM);
    }
    struct rect ar = addr_rect(a);
    fill(a, ar, a->addr_focus ? 0x000000u : COL_FIELD);
    if (a->addr_focus)
        fill(a, (struct rect){ ar.x + 1, ar.y + 1, ar.w - 2, ar.h - 2 }, COL_FIELD);
    int visible = LM_MAX((ar.w - 12) / LM_FONT_W, 1);
    int first = a->addr_focus ? addr_first_char(a, visible) : 0;
    const char *s = a->addr;
    for (int k = 0; k < first && *s; k++) {
        s++;
        while (((unsigned char)*s & 0xc0) == 0x80)
            s++;
    }
    struct rect inner = { ar.x + 2, ar.y, ar.w - 4, ar.h };
    int ty = ar.y + (ar.h - LM_FONT_H) / 2;
    if (a->addr_focus && a->addr_fresh && a->addr[0])
        fill(a, (struct rect){ ar.x + 5, ty, LM_MIN((int)lm_font_text_width(s), inner.w - 6), LM_FONT_H }, COL_HOVER);
    draw_text(a, inner, ar.x + 6, ty, s, strlen(s), 1, COL_UI_TEXT, false, false);
    if (a->addr_focus && !a->addr_fresh) {
        int cc = 0;
        for (int i = 0; i < a->cursor; i++)
            cc += (a->addr[i] & 0xc0) != 0x80;
        fill(a, (struct rect){ ar.x + 6 + (cc - first) * LM_FONT_W, ty, 1, LM_FONT_H }, COL_ACCENT);
    }

    /* status bar */
    struct rect st = { 0, a->bh - STATUS_H, a->bw, STATUS_H };
    fill(a, st, COL_BTN);
    char line[2400];
    if (a->hover_link >= 0 && a->hover_link < a->doc.nlinks) {
        char abs[2048];
        if (url_resolve(a->cur_url, a->doc.links[a->hover_link], abs, sizeof(abs)))
            snprintf(line, sizeof(line), "%s", abs);
        else
            snprintf(line, sizeof(line), "%s", a->doc.links[a->hover_link]);
    } else {
        snprintf(line, sizeof(line), "%s", a->status[0] ? a->status : a->cur_url);
    }
    draw_text(a, st, 8, st.y + (st.h - LM_FONT_H) / 2, line, strlen(line), 1, COL_UI_TEXT, false, false);
}

/* ---- navigation ------------------------------------------------------------------------------- */

static void set_window_title(struct app *a)
{
    char t[400];
    snprintf(t, sizeof(t), "%s - Lumen Web", a->doc.title[0] ? a->doc.title : a->cur_url);
    if (strcmp(t, a->title)) {
        snprintf(a->title, sizeof(a->title), "%s", t);
        if (a->toplevel)
            WLC_SEND(a->c, a->toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = a->title });
    }
}

static void relayout(struct app *a)
{
    a->layout_w = a->bw > 0 ? a->bw : a->w;
    doc_layout(&a->doc, a->layout_w, &a->layout);
    a->scroll = LM_CLAMP(a->scroll, 0, max_scroll(a));
}

enum nav { NAV_PUSH, NAV_RELOAD, NAV_HISTORY };

static void navigate(struct app *a, const char *url, enum nav mode, int restore_scroll)
{
    if (a->nhist > 0)
        a->hist[a->hpos].scroll = a->scroll;
    struct page p;
    page_load(&p, url);
    doc_free(&a->doc);
    doc_parse(&a->doc, p.body ? p.body : "", p.body ? p.len : 0, !p.html);
    free(p.body);
    snprintf(a->cur_url, sizeof(a->cur_url), "%s", p.url);
    snprintf(a->addr, sizeof(a->addr), "%s", strcmp(p.url, "about:home") ? p.url : "about:home");
    a->cursor = (int)strlen(a->addr);
    a->addr_focus = false;
    a->hover_link = -1;
    a->status[0] = 0;
    a->scroll = 0;
    a->layout_w = 0;
    if (mode == NAV_PUSH) {
        a->nhist = a->hpos + (a->nhist > 0 ? 1 : 0);
        if (a->nhist >= (int)LM_ARRAY_LEN(a->hist)) {
            memmove(a->hist, a->hist + 1, (LM_ARRAY_LEN(a->hist) - 1) * sizeof(a->hist[0]));
            a->nhist--;
        }
        snprintf(a->hist[a->nhist].url, sizeof(a->hist[0].url), "%s", p.url);
        a->hist[a->nhist].scroll = 0;
        a->hpos = a->nhist++;
    } else if (mode == NAV_HISTORY) {
        snprintf(a->hist[a->hpos].url, sizeof(a->hist[0].url), "%s", p.url);
    }
    a->scroll = restore_scroll;
    relayout(a);
    set_window_title(a);
    a->redraw = true;
}

static void go_input(struct app *a, const char *typed)
{
    char url[2048];
    if (!url_from_input(typed, url, sizeof(url))) {
        snprintf(a->status, sizeof(a->status), "Not a valid address");
        a->redraw = true;
        return;
    }
    navigate(a, url, NAV_PUSH, 0);
}

static void go_history(struct app *a, int delta)
{
    int np = a->hpos + delta;
    if (np < 0 || np >= a->nhist)
        return;
    if (a->nhist > 0)
        a->hist[a->hpos].scroll = a->scroll;
    a->hpos = np;
    char url[1024];
    snprintf(url, sizeof(url), "%s", a->hist[np].url);
    navigate(a, url, NAV_HISTORY, a->hist[np].scroll);
}

static void follow_link(struct app *a, int link)
{
    char abs[2048];
    if (link < 0 || link >= a->doc.nlinks)
        return;
    char href[1024];
    snprintf(href, sizeof(href), "%s", a->doc.links[link]);
    if (href[0] == '#') {
        a->scroll = 0;
        a->redraw = true;
        return;
    }
    if (!url_resolve(a->cur_url, href, abs, sizeof(abs))) {
        snprintf(a->status, sizeof(a->status), "Cannot open link: %s", href);
        a->redraw = true;
        return;
    }
    navigate(a, abs, NAV_PUSH, 0);
}

static void scroll_by(struct app *a, int dy)
{
    int ns = LM_CLAMP(a->scroll + dy, 0, max_scroll(a));
    if (ns != a->scroll) {
        a->scroll = ns;
        a->redraw = true;
    }
}

/* ---- wayland plumbing -------------------------------------------------------------------------- */

static void setup_buffer(struct app *a)
{
    if (a->pool) {
        WLC_SEND0(a->c, a->pool, LM_WL_SHM_POOL_REQ_DESTROY);
        wlc_forget(a->c, a->pool);
        a->pool = 0;
    }
    if (a->pixels)
        munmap(a->pixels, a->map_size);
    a->bw = a->w;
    a->bh = a->h;
    a->map_size = (size_t)a->bw * (size_t)a->bh * 4;
    int fd = memfd_create("lumen-web", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)a->map_size) < 0) {
        perror("memfd");
        exit(1);
    }
    a->pixels = mmap(NULL, a->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (a->pixels == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    a->pool = wlc_alloc(a->c, &lm_wl_shm_pool_interface, NULL, NULL);
    WLC_SEND(a->c, a->shm, LM_WL_SHM_REQ_CREATE_POOL, { .n = a->pool }, { .h = fd }, { .i = (int32_t)a->map_size });
    close(fd);
}

static void buffer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)args;
    struct app *a = data;
    if (op != LM_WL_BUFFER_EVT_RELEASE)
        return;
    WLC_SEND0(c, id, LM_WL_BUFFER_REQ_DESTROY);
    wlc_forget(c, id);
    a->buffer = 0;
    a->buffer_busy = false;
}

static void draw(struct app *a)
{
    if (a->bw != a->w || a->bh != a->h || !a->pixels)
        setup_buffer(a);
    if (a->layout_w != a->bw)
        relayout(a);
    paint(a);
    a->buffer = wlc_alloc(a->c, &lm_wl_buffer_interface, buffer_event, a);
    WLC_SEND(a->c, a->pool, LM_WL_SHM_POOL_REQ_CREATE_BUFFER, { .n = a->buffer }, { .i = 0 }, { .i = a->bw },
             { .i = a->bh }, { .i = a->bw * 4 }, { .u = LM_WL_SHM_FORMAT_XRGB8888 });
    WLC_SEND(a->c, a->surface, LM_WL_SURFACE_REQ_ATTACH, { .o = a->buffer }, { .i = 0 }, { .i = 0 });
    WLC_SEND(a->c, a->surface, LM_WL_SURFACE_REQ_DAMAGE, { .i = 0 }, { .i = 0 }, { .i = a->bw }, { .i = a->bh });
    WLC_SEND0(a->c, a->surface, LM_WL_SURFACE_REQ_COMMIT);
    a->buffer_busy = true;
    a->redraw = false;
}

static void wm_base_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)data;
    if (op == LM_XDG_WM_BASE_EVT_PING)
        WLC_SEND(c, id, LM_XDG_WM_BASE_REQ_PONG, { .u = args[0].u });
}

static void xsurface_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    struct app *a = data;
    if (op == LM_XDG_SURFACE_EVT_CONFIGURE) {
        WLC_SEND(c, id, LM_XDG_SURFACE_REQ_ACK_CONFIGURE, { .u = args[0].u });
        a->configured = true;
        a->redraw = true;
    }
}

static void toplevel_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    if (op == LM_XDG_TOPLEVEL_EVT_CONFIGURE) {
        a->w = args[0].i > 0 ? args[0].i : DEFAULT_W;
        a->h = args[1].i > 0 ? args[1].i : DEFAULT_H;
        a->w = LM_MAX(a->w, 320);
        a->h = LM_MAX(a->h, 200);
    } else if (op == LM_XDG_TOPLEVEL_EVT_CLOSE) {
        a->running = false;
    }
}

static void pointer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->has_pointer = true;
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        a->has_pointer = false;
        a->press_link = -1;
        a->press_btn = -1;
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        if (args[2].u != 272) /* BTN_LEFT */
            break;
        if (args[3].u == 1) {
            a->press_link = link_at(a, a->px, a->py);
            a->press_btn = -1;
            for (int i = 0; i < 3; i++)
                if (in_rect(btn_rect(i), a->px, a->py) && btn_enabled(a, i))
                    a->press_btn = i;
            if (in_rect(addr_rect(a), a->px, a->py)) {
                if (!a->addr_focus) {
                    a->addr_focus = true;
                    a->addr_fresh = true;
                    a->cursor = (int)strlen(a->addr);
                } else {
                    a->addr_fresh = false;
                    a->cursor = (int)strlen(a->addr);
                }
            } else if (a->addr_focus) {
                a->addr_focus = false;
            }
        } else {
            if (a->press_btn >= 0 && in_rect(btn_rect(a->press_btn), a->px, a->py)) {
                if (a->press_btn == 0)
                    go_history(a, -1);
                else if (a->press_btn == 1)
                    go_history(a, 1);
                else {
                    char u[2048];
                    snprintf(u, sizeof(u), "%s", a->cur_url);
                    navigate(a, u, NAV_RELOAD, a->scroll);
                }
            } else if (a->press_link >= 0 && link_at(a, a->px, a->py) == a->press_link) {
                follow_link(a, a->press_link);
            }
            a->press_link = -1;
            a->press_btn = -1;
        }
        a->redraw = true;
        break;
    case LM_WL_POINTER_EVT_AXIS:
        if (args[1].u == LM_POINTER_AXIS_VERTICAL)
            scroll_by(a, (int)(lm_fixed_to_double(args[2].f) * 3.0));
        break;
    }
    if (op == LM_WL_POINTER_EVT_ENTER || op == LM_WL_POINTER_EVT_MOTION || op == LM_WL_POINTER_EVT_LEAVE) {
        int hl = a->has_pointer ? link_at(a, a->px, a->py) : -1;
        if (hl != a->hover_link)
            a->hover_link = hl;
        a->redraw = true;
    }
}

static void addr_insert(struct app *a, const char *txt)
{
    size_t tl = strlen(txt), len = strlen(a->addr);
    if (a->addr_fresh) {
        a->addr[0] = 0;
        len = 0;
        a->cursor = 0;
        a->addr_fresh = false;
    }
    if (len + tl + 1 >= sizeof(a->addr))
        return;
    memmove(a->addr + a->cursor + tl, a->addr + a->cursor, len - (size_t)a->cursor + 1);
    memcpy(a->addr + a->cursor, txt, tl);
    a->cursor += (int)tl;
}

static void addr_key(struct app *a, uint32_t ks, const char *txt, int tl)
{
    size_t len = strlen(a->addr);
    if (ks == LM_KS_Escape) {
        a->addr_focus = false;
        snprintf(a->addr, sizeof(a->addr), "%s", a->cur_url);
        a->cursor = (int)strlen(a->addr);
    } else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
        char typed[1024];
        snprintf(typed, sizeof(typed), "%s", a->addr);
        go_input(a, typed);
    } else if (ks == LM_KS_BackSpace) {
        if (a->addr_fresh) {
            a->addr[0] = 0;
            a->cursor = 0;
            a->addr_fresh = false;
        } else if (a->cursor > 0) {
            int s = a->cursor - 1;
            while (s > 0 && (a->addr[s] & 0xc0) == 0x80)
                s--;
            memmove(a->addr + s, a->addr + a->cursor, len - (size_t)a->cursor + 1);
            a->cursor = s;
        }
    } else if (ks == LM_KS_Delete) {
        if (a->cursor < (int)len) {
            int e = a->cursor + 1;
            while (e < (int)len && (a->addr[e] & 0xc0) == 0x80)
                e++;
            memmove(a->addr + a->cursor, a->addr + e, len - (size_t)e + 1);
        }
    } else if (ks == LM_KS_Left) {
        a->addr_fresh = false;
        if (a->cursor > 0) {
            a->cursor--;
            while (a->cursor > 0 && (a->addr[a->cursor] & 0xc0) == 0x80)
                a->cursor--;
        }
    } else if (ks == LM_KS_Right) {
        a->addr_fresh = false;
        if (a->cursor < (int)len) {
            a->cursor++;
            while (a->cursor < (int)len && (a->addr[a->cursor] & 0xc0) == 0x80)
                a->cursor++;
        }
    } else if (ks == LM_KS_Home) {
        a->addr_fresh = false;
        a->cursor = 0;
    } else if (ks == LM_KS_End) {
        a->addr_fresh = false;
        a->cursor = (int)len;
    } else if ((a->mods & LM_MOD_CTRL) && (ks == 'u' || ks == 'U')) {
        a->addr[0] = 0;
        a->cursor = 0;
        a->addr_fresh = false;
    } else if (tl > 0 && !(a->mods & (LM_MOD_CTRL | LM_MOD_ALT | LM_MOD_SUPER))) {
        addr_insert(a, txt);
    }
    a->redraw = true;
}

static void page_key(struct app *a, uint32_t ks, const char *txt, int tl)
{
    bool ctrl = a->mods & LM_MOD_CTRL, alt = a->mods & LM_MOD_ALT;
    bool shift = a->mods & LM_MOD_SHIFT;
    int page = view_h(a) - 40;
    if ((ctrl && (ks == 'l' || ks == 'L')) || ks == LM_KS_F1 + 5) {
        a->addr_focus = true;
        a->addr_fresh = true;
        a->cursor = (int)strlen(a->addr);
        a->redraw = true;
    } else if ((alt && ks == LM_KS_Left) || (!alt && !ctrl && ks == LM_KS_BackSpace)) {
        go_history(a, -1);
    } else if (alt && ks == LM_KS_Right) {
        go_history(a, 1);
    } else if (ks == LM_KS_F1 + 4 || (ctrl && (ks == 'r' || ks == 'R'))) {
        char u[2048];
        snprintf(u, sizeof(u), "%s", a->cur_url);
        navigate(a, u, NAV_RELOAD, a->scroll);
    } else if (ks == LM_KS_Down || (tl == 1 && txt[0] == 'j' && !ctrl)) {
        scroll_by(a, 48);
    } else if (ks == LM_KS_Up || (tl == 1 && txt[0] == 'k' && !ctrl)) {
        scroll_by(a, -48);
    } else if (ks == LM_KS_Page_Down || (tl == 1 && txt[0] == ' ' && !shift)) {
        scroll_by(a, page);
    } else if (ks == LM_KS_Page_Up || (tl == 1 && txt[0] == ' ' && shift) || (tl == 1 && txt[0] == 'b' && !ctrl)) {
        scroll_by(a, -page);
    } else if (ks == LM_KS_Home) {
        scroll_by(a, -a->scroll);
    } else if (ks == LM_KS_End) {
        scroll_by(a, max_scroll(a));
    }
}

static void keyboard_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id;
    struct app *a = data;
    switch (op) {
    case LM_WL_KEYBOARD_EVT_KEYMAP:
        lm_keymap_client_load(args[1].h, args[2].u);
        break;
    case LM_WL_KEYBOARD_EVT_MODIFIERS:
        a->mods = args[1].u | args[2].u | args[3].u;
        lm_keymap_client_group(args[4].u);
        break;
    case LM_WL_KEYBOARD_EVT_KEY: {
        if (args[3].u == 0)
            break;
        uint32_t ks = lm_keymap_keysym(args[2].u, a->mods);
        char txt[8];
        int tl = lm_keysym_to_utf8(ks, txt);
        txt[tl > 0 ? tl : 0] = 0;
        if (a->addr_focus)
            addr_key(a, ks, txt, tl);
        else
            page_key(a, ks, txt, tl);
        break;
    }
    }
}

static void seat_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    struct app *a = data;
    if (op == LM_WL_SEAT_EVT_CAPABILITIES) {
        if ((args[0].u & 1) && !a->pointer) {
            a->pointer = wlc_alloc(c, &lm_wl_pointer_interface, pointer_event, a);
            WLC_SEND(c, id, LM_WL_SEAT_REQ_GET_POINTER, { .n = a->pointer });
        }
        if ((args[0].u & 2) && !a->keyboard) {
            a->keyboard = wlc_alloc(c, &lm_wl_keyboard_interface, keyboard_event, a);
            WLC_SEND(c, id, LM_WL_SEAT_REQ_GET_KEYBOARD, { .n = a->keyboard });
        }
    }
}

static void registry_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)id;
    struct app *a = data;
    if (op != LM_WL_REGISTRY_EVT_GLOBAL)
        return;
    uint32_t name = args[0].u, ver = args[2].u;
    const char *iface = args[1].s;
    if (!strcmp(iface, "wl_compositor"))
        a->compositor = wlc_bind(c, a->registry, name, &lm_wl_compositor_interface, LM_MIN(ver, 4u), NULL, a);
    else if (!strcmp(iface, "wl_shm"))
        a->shm = wlc_bind(c, a->registry, name, &lm_wl_shm_interface, 1, NULL, a);
    else if (!strcmp(iface, "xdg_wm_base"))
        a->wm_base = wlc_bind(c, a->registry, name, &lm_xdg_wm_base_interface, LM_MIN(ver, 3u), wm_base_event, a);
    else if (!strcmp(iface, "wl_seat"))
        a->seat = wlc_bind(c, a->registry, name, &lm_wl_seat_interface, LM_MIN(ver, 5u), seat_event, a);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();

    static struct app a;
    a.w = DEFAULT_W;
    a.h = DEFAULT_H;
    a.running = true;
    a.hover_link = a.press_link = a.press_btn = -1;
    a.c = wlc_connect(NULL);
    if (!a.c)
        return 1;
    a.registry = wlc_get_registry(a.c, registry_event, &a);
    if (wlc_roundtrip(a.c) < 0 || !a.compositor || !a.shm || !a.wm_base) {
        fprintf(stderr, "lumen-web: compositor lacks wl_compositor/wl_shm/xdg_wm_base\n");
        return 1;
    }
    a.surface = wlc_alloc(a.c, &lm_wl_surface_interface, NULL, NULL);
    WLC_SEND(a.c, a.compositor, LM_WL_COMPOSITOR_REQ_CREATE_SURFACE, { .n = a.surface });
    a.xsurface = wlc_alloc(a.c, &lm_xdg_surface_interface, xsurface_event, &a);
    WLC_SEND(a.c, a.wm_base, LM_XDG_WM_BASE_REQ_GET_XDG_SURFACE, { .n = a.xsurface }, { .o = a.surface });
    a.toplevel = wlc_alloc(a.c, &lm_xdg_toplevel_interface, toplevel_event, &a);
    WLC_SEND(a.c, a.xsurface, LM_XDG_SURFACE_REQ_GET_TOPLEVEL, { .n = a.toplevel });
    WLC_SEND(a.c, a.toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = "Lumen Web" });
    WLC_SEND(a.c, a.toplevel, LM_XDG_TOPLEVEL_REQ_SET_APP_ID, { .s = "lumen.web" });

    char start[2048];
    if (argc > 1 && url_from_input(argv[1], start, sizeof(start)))
        navigate(&a, start, NAV_PUSH, 0);
    else
        navigate(&a, "about:home", NAV_PUSH, 0);
    WLC_SEND0(a.c, a.surface, LM_WL_SURFACE_REQ_COMMIT);

    while (a.running && !wlc_dead(a.c)) {
        if (a.redraw && a.configured && !a.buffer_busy)
            draw(&a);
        if (wlc_dispatch(a.c, -1) < 0)
            break;
    }
    doc_free(&a.doc);
    free(a.layout.r);
    wlc_destroy(a.c);
    return 0;
}
