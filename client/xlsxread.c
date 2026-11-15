#include "xlsxread.h"

#include "../src/img.h"

#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define PART_SMALL (8u << 20)                   /* workbook, relationships, styles: at most this much inflated */
#define PART_FIRST (2u << 20)                  /* a sheet: the first this much of its XML is tried first */
#define PART_BIG (32u << 20)                    /* a sheet or the shared strings: the first this much */
#define COMP_BIG (16u << 20)                    /* ... read from the file, at most (the stream may be cut: the start still reads) */
#define MAX_SHEETS 64
#define MAX_XF 100000
#define MAX_SST 2000000
#define CELL_BYTES 200                          /* a cell is shown at most this long */
#define BUF_LIMIT 4096                          /* what one cell's text may collect while it is read */

static void seterr(char *err, size_t n, const char *s)
{
    if (err && n)
        snprintf(err, n, "%s", s);
}

/* ---- a growing string ---- */
struct xbuf {
    char *s;
    size_t n, cap;
};

static void xb_add(struct xbuf *b, const char *p, size_t n)
{
    if (b->n + n > BUF_LIMIT)
        n = b->n < BUF_LIMIT ? BUF_LIMIT - b->n : 0;
    if (!n)
        return;
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, p, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void xb_reset(struct xbuf *b)
{
    b->n = 0;
    if (b->s)
        b->s[0] = 0;
}

static const char *xb_str(const struct xbuf *b)
{
    return b->s ? b->s : "";
}

static void xb_utf8(struct xbuf *b, unsigned cp)
{
    char t[4];
    size_t n;
    if (cp < 0x80) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xc0 | cp >> 6); t[1] = (char)(0x80 | (cp & 0x3f)); n = 2; }
    else if (cp < 0x10000) { t[0] = (char)(0xe0 | cp >> 12); t[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); t[2] = (char)(0x80 | (cp & 0x3f)); n = 3; }
    else { t[0] = (char)(0xf0 | cp >> 18); t[1] = (char)(0x80 | ((cp >> 12) & 0x3f)); t[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); t[3] = (char)(0x80 | (cp & 0x3f)); n = 4; }
    xb_add(b, t, n);
}

/* The text with its XML entities decoded (&lt; &gt; &amp; &quot; &apos; &#65; &#x41;); an unknown one stays as it is. */
static void xb_add_xml(struct xbuf *b, const char *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        const char *amp = memchr(p + i, '&', n - i);
        if (!amp) {
            xb_add(b, p + i, n - i);
            return;
        }
        xb_add(b, p + i, (size_t)(amp - (p + i)));
        i = (size_t)(amp - p);
        const char *semi = memchr(p + i, ';', n - i < 12 ? n - i : 12);
        if (!semi) {
            xb_add(b, "&", 1);
            i++;
            continue;
        }
        size_t el = (size_t)(semi - (p + i + 1));
        const char *e = p + i + 1;
        if (el == 2 && !memcmp(e, "lt", 2)) xb_add(b, "<", 1);
        else if (el == 2 && !memcmp(e, "gt", 2)) xb_add(b, ">", 1);
        else if (el == 3 && !memcmp(e, "amp", 3)) xb_add(b, "&", 1);
        else if (el == 4 && !memcmp(e, "quot", 4)) xb_add(b, "\"", 1);
        else if (el == 4 && !memcmp(e, "apos", 4)) xb_add(b, "'", 1);
        else if (el >= 2 && e[0] == '#') {
            char t[12];
            memcpy(t, e + 1, el - 1 < sizeof(t) - 1 ? el - 1 : sizeof(t) - 1);
            t[el - 1 < sizeof(t) - 1 ? el - 1 : sizeof(t) - 1] = 0;
            char *endp;
            unsigned long cp = t[0] == 'x' || t[0] == 'X' ? strtoul(t + 1, &endp, 16) : strtoul(t, &endp, 10);
            if (*endp || cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp < 0xe000))
                xb_add(b, "?", 1);
            else
                xb_utf8(b, (unsigned)cp);
        } else {
            xb_add(b, p + i, el + 2);
        }
        i += el + 2;
    }
}

/* OOXML writes a character that XML cannot hold as _xHHHH_ (and a real "_x005F_" as _x005F_): turns those back, in place. */
static void unescape_x(char *s)
{
    char *o = s;
    for (char *p = s; *p;) {
        if (p[0] == '_' && p[1] == 'x' && strspn(p + 2, "0123456789abcdefABCDEF") >= 4 && p[6] == '_') {
            char h[5] = { p[2], p[3], p[4], p[5], 0 };
            unsigned cp = (unsigned)strtoul(h, NULL, 16);
            p += 7;
            if (cp == '\t' || cp == '\n' || cp == '\r')
                *o++ = ' ';
            else if (cp >= 0x20 && cp < 0x80)
                *o++ = (char)cp;
            else if (cp >= 0x80 && cp < 0x800) {
                *o++ = (char)(0xc0 | cp >> 6);
                *o++ = (char)(0x80 | (cp & 0x3f));
            } else if (cp >= 0x800 && (cp < 0xd800 || cp >= 0xe000)) {
                *o++ = (char)(0xe0 | cp >> 12);
                *o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
                *o++ = (char)(0x80 | (cp & 0x3f));
            }
        } else {
            *o++ = *p++;
        }
    }
    *o = 0;
}

/* A string safe to show in one line of the table: line breaks and tabs are blanks, other control characters a ?, at most CELL_BYTES bytes. */
static char *cell_text(const char *s)
{
    size_t n = strlen(s);
    if (n > CELL_BYTES) {
        n = CELL_BYTES;
        while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80)
            n--;
    }
    char *o = malloc(n + 1);
    if (!o)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        o[i] = c == '\t' || c == '\n' || c == '\r' ? ' ' : c < 0x20 || c == 0x7f ? '?' : (char)c;
    }
    o[n] = 0;
    return o;
}

/* A cell's text as it is shown: the _xHHHH_ escapes undone, one line, cut. */
static char *make_cell(const char *val)
{
    char *tmp = strdup(val);
    if (!tmp)
        return NULL;
    unescape_x(tmp);
    char *o = cell_text(tmp);
    free(tmp);
    return o;
}

/* ---- the ZIP container ---- */
struct zent {
    char *name;
    uint32_t method, flags, csize, usize, off;
};

struct zip {
    int fd;
    uint64_t size;
    struct zent *e;
    int n;
};

static bool pread_all(int fd, void *buf, size_t n, uint64_t off)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r <= 0)
            return false;
        p += r;
        n -= (size_t)r;
        off += (uint64_t)r;
    }
    return true;
}

static uint32_t u16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void zip_close(struct zip *z)
{
    for (int i = 0; i < z->n; i++)
        free(z->e[i].name);
    free(z->e);
    if (z->fd >= 0)
        close(z->fd);
    z->e = NULL;
    z->n = 0;
    z->fd = -1;
}

/* 0 = opened; -1 = not a ZIP file; -2 = a ZIP this reader cannot read (err). */
static int zip_open(struct zip *z, const char *path, char *err, size_t errn)
{
    memset(z, 0, sizeof(*z));
    z->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (z->fd < 0)
        return -1;
    struct stat st;
    uint8_t sig[4];
    if (fstat(z->fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 22 || !pread_all(z->fd, sig, 4, 0) || memcmp(sig, "PK\3\4", 4)) {
        zip_close(z);
        return -1;
    }
    z->size = (uint64_t)st.st_size;
    size_t tail = z->size < 22 + 65535 ? (size_t)z->size : 22 + 65535;
    uint8_t *t = malloc(tail);
    if (!t || !pread_all(z->fd, t, tail, z->size - tail)) {
        free(t);
        zip_close(z);
        return -1;
    }
    long at = -1;
    for (long i = (long)tail - 22; i >= 0; i--)
        if (!memcmp(t + i, "PK\5\6", 4)) {
            at = i;
            break;
        }
    if (at < 0) {
        free(t);
        zip_close(z);
        return -1;
    }
    uint32_t count = u16(t + at + 10), cdsize = u32(t + at + 12), cdoff = u32(t + at + 16);
    free(t);
    if (count == 0xffff || cdsize == 0xffffffffu || cdoff == 0xffffffffu) {
        seterr(err, errn, "A ZIP64 file: not read.");
        zip_close(z);
        return -2;
    }
    if (cdsize > (8u << 20) || (uint64_t)cdoff + cdsize > z->size) {
        zip_close(z);
        return -1;
    }
    uint8_t *cd = malloc(cdsize ? cdsize : 1);
    if (!cd || !pread_all(z->fd, cd, cdsize, cdoff)) {
        free(cd);
        zip_close(z);
        return -1;
    }
    z->e = calloc(count ? count : 1, sizeof(struct zent));
    size_t p = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 46 > cdsize || memcmp(cd + p, "PK\1\2", 4))
            break;
        uint32_t nl = u16(cd + p + 28), xl = u16(cd + p + 30), cl = u16(cd + p + 32);
        if (p + 46 + nl + xl + cl > cdsize)
            break;
        struct zent *e = &z->e[z->n];
        e->flags = u16(cd + p + 8);
        e->method = u16(cd + p + 10);
        e->csize = u32(cd + p + 20);
        e->usize = u32(cd + p + 24);
        e->off = u32(cd + p + 42);
        e->name = strndup((const char *)cd + p + 46, nl);
        if (!e->name)
            break;
        z->n++;
        p += 46 + nl + xl + cl;
    }
    free(cd);
    if (z->n == 0) {
        zip_close(z);
        return -1;
    }
    return 0;
}

static const struct zent *zip_find(const struct zip *z, const char *name)
{
    for (int i = 0; i < z->n; i++)
        if (!strcmp(z->e[i].name, name))
            return &z->e[i];
    return NULL;
}

/* The (first `cap` bytes of the) entry, NUL ended; NULL when it is missing or cannot be read (a reason in *why for the ones that are there). */
static char *zip_read(const struct zip *z, const char *name, size_t cap, size_t *lenp, const char **why)
{
    *why = NULL;
    const struct zent *e = zip_find(z, name);
    if (!e)
        return NULL;
    if (e->flags & 1) {
        *why = "The workbook is encrypted.";
        return NULL;
    }
    if (e->method != 0 && e->method != 8) {
        *why = "A workbook part with a compression this reader does not know.";
        return NULL;
    }
    uint8_t lh[30];
    if (!pread_all(z->fd, lh, 30, e->off) || memcmp(lh, "PK\3\4", 4)) {
        *why = "A damaged workbook (a part's header).";
        return NULL;
    }
    uint64_t data = (uint64_t)e->off + 30 + u16(lh + 26) + u16(lh + 28);
    if (data > z->size || e->csize > z->size - data) {
        *why = "A damaged workbook (a part runs past the file).";
        return NULL;
    }
    size_t want = e->usize < cap ? e->usize : cap;
    size_t cread = e->csize < COMP_BIG ? e->csize : COMP_BIG;
    char *out = malloc(want + 1);
    uint8_t *comp = e->method == 8 ? malloc(cread ? cread : 1) : NULL;
    if (!out || (e->method == 8 && !comp)) {
        free(out);
        free(comp);
        return NULL;
    }
    long got = 0;
    if (e->method == 0) {
        size_t n = want < e->csize ? want : e->csize;
        if (pread_all(z->fd, out, n, data))
            got = (long)n;
        else
            got = -1;
    } else if (pread_all(z->fd, comp, cread, data)) {
        got = want ? lm_inflate_prefix(comp, cread, (uint8_t *)out, want) : 0;
    } else {
        got = -1;
    }
    free(comp);
    if (got < 0) {
        free(out);
        *why = "A damaged workbook (a part does not inflate).";
        return NULL;
    }
    out[got] = 0;
    *lenp = (size_t)got;
    return out;
}

/* ---- a small XML tokenizer: text, open / close / empty tags with the attributes left raw ---- */
enum { XE_END, XE_TEXT, XE_OPEN, XE_CLOSE, XE_SELF };

struct xml {
    const char *p, *end;
};

struct xev {
    int kind;
    const char *name;                   /* the local name: the prefix before ':' is left off */
    size_t nlen;
    const char *attr;
    size_t alen;
    const char *text;
    size_t tlen;
};

static int xml_next(struct xml *x, struct xev *e)
{
    for (;;) {
        if (x->p >= x->end)
            return e->kind = XE_END;
        if (*x->p != '<') {
            const char *q = memchr(x->p, '<', (size_t)(x->end - x->p));
            if (!q)
                q = x->end;
            e->kind = XE_TEXT;
            e->text = x->p;
            e->tlen = (size_t)(q - x->p);
            x->p = q;
            return XE_TEXT;
        }
        size_t left = (size_t)(x->end - x->p);
        if (left >= 4 && !memcmp(x->p, "<!--", 4)) {
            const char *q = memmem(x->p + 4, left - 4, "-->", 3);
            if (!q) {
                x->p = x->end;
                continue;
            }
            x->p = q + 3;
            continue;
        }
        if (left >= 9 && !memcmp(x->p, "<![CDATA[", 9)) {
            const char *q = memmem(x->p + 9, left - 9, "]]>", 3);
            e->kind = XE_TEXT;
            e->text = x->p + 9;
            e->tlen = q ? (size_t)(q - (x->p + 9)) : left - 9;
            x->p = q ? q + 3 : x->end;
            return XE_TEXT;
        }
        if (left >= 2 && (x->p[1] == '?' || x->p[1] == '!')) {
            const char *q = memchr(x->p, '>', left);
            x->p = q ? q + 1 : x->end;
            continue;
        }
        bool close = left >= 2 && x->p[1] == '/';
        const char *s = x->p + (close ? 2 : 1);
        const char *q = s;
        char quote = 0;
        while (q < x->end && (quote || *q != '>')) {                    /* the end of the tag, > inside quotes does not count */
            if (quote) {
                if (*q == quote)
                    quote = 0;
            } else if (*q == '"' || *q == '\'') {
                quote = *q;
            }
            q++;
        }
        if (q >= x->end) {                                              /* a tag cut off by the end of the data */
            x->p = x->end;
            return e->kind = XE_END;
        }
        const char *ne = s;
        while (ne < q && *ne != ' ' && *ne != '\t' && *ne != '\n' && *ne != '\r' && *ne != '/')
            ne++;
        const char *ln = s;
        for (const char *c = s; c < ne; c++)
            if (*c == ':')
                ln = c + 1;
        e->name = ln;
        e->nlen = (size_t)(ne - ln);
        e->attr = ne;
        bool self = !close && q > s && q[-1] == '/';
        e->alen = (size_t)(q - ne) - (self ? 1 : 0);
        e->kind = close ? XE_CLOSE : self ? XE_SELF : XE_OPEN;
        x->p = q + 1;
        return e->kind;
    }
}

static bool is(const struct xev *e, const char *n)
{
    return strlen(n) == e->nlen && !memcmp(e->name, n, e->nlen);
}

/* The value of the attribute whose local name is `key` (r:id is "id"), decoded into `out`; false when there is none. */
static bool xattr(const struct xev *e, const char *key, struct xbuf *out)
{
    const char *p = e->attr, *end = e->attr + e->alen;
    size_t kl = strlen(key);
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
        const char *ns = p;
        while (p < end && *p != '=' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
            p++;
        const char *ne = p;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
        if (p >= end || *p != '=')
            return false;
        p++;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
        if (p >= end || (*p != '"' && *p != '\''))
            return false;
        char q = *p++;
        const char *vs = p;
        while (p < end && *p != q)
            p++;
        if (p >= end)
            return false;
        const char *ln = ns;
        for (const char *c = ns; c < ne; c++)
            if (*c == ':')
                ln = c + 1;
        if ((size_t)(ne - ln) == kl && !memcmp(ln, key, kl)) {
            xb_reset(out);
            xb_add_xml(out, vs, (size_t)(p - vs));
            return true;
        }
        p++;
    }
    return false;
}

/* ---- number formats: a date, a percentage, or a plain number ---- */
enum { K_PLAIN = 0, K_DATE = 1, K_PERCENT = 2 };

static int classify_format(const char *f)
{
    bool date = false, pct = false;
    int dec = 0;
    bool after_dot = false;
    for (const char *p = f; *p; p++) {
        if (*p == '"') {
            while (p[1] && p[1] != '"')
                p++;
            if (p[1])
                p++;
        } else if (*p == '[') {
            while (p[1] && p[1] != ']')
                p++;
            if (p[1])
                p++;
        } else if (*p == '\\' || *p == '_' || *p == '*') {
            if (p[1])
                p++;
        } else if (strchr("yYdDhHsSmM", *p)) {
            date = true;
        } else if (*p == '%') {
            pct = true;
        } else if (*p == '.') {
            after_dot = true;
        } else if (*p == '0' && after_dot && !pct) {
            dec++;
        }
    }
    if (date)
        return K_DATE;
    if (pct)
        return K_PERCENT | (dec > 15 ? 15 : dec) << 4;
    return K_PLAIN;
}

static int builtin_format(int id)
{
    if (id == 9)
        return K_PERCENT;
    if (id == 10)
        return K_PERCENT | 2 << 4;
    if ((id >= 14 && id <= 22) || (id >= 27 && id <= 36) || (id >= 45 && id <= 47) || (id >= 50 && id <= 58))
        return K_DATE;
    return K_PLAIN;
}

/* ---- the workbook's parts ---- */
struct sheetref {
    char *name, *rid, *path;
    bool hidden;
};

struct rel {
    char *id, *target;
    bool sheet;
};

struct styles {
    int *xf;                            /* per cell format: kind | decimals << 4 */
    int nxf;
};

static void parse_workbook(const char *buf, size_t len, struct sheetref *refs, int *n, bool *d1904)
{
    struct xml x = { buf, buf + len };
    struct xev e;
    struct xbuf a = { 0 };
    *n = 0;
    *d1904 = false;
    while (xml_next(&x, &e) != XE_END) {
        if ((e.kind == XE_OPEN || e.kind == XE_SELF) && is(&e, "workbookPr")) {
            if (xattr(&e, "date1904", &a) && (!strcmp(xb_str(&a), "1") || !strcmp(xb_str(&a), "true")))
                *d1904 = true;
        } else if ((e.kind == XE_OPEN || e.kind == XE_SELF) && is(&e, "sheet") && *n < MAX_SHEETS) {
            struct sheetref *r = &refs[*n];
            memset(r, 0, sizeof(*r));
            if (xattr(&e, "name", &a))
                r->name = strdup(xb_str(&a));
            if (xattr(&e, "id", &a))
                r->rid = strdup(xb_str(&a));
            if (xattr(&e, "state", &a) && strcmp(xb_str(&a), "visible"))
                r->hidden = true;
            if (r->name && r->rid)
                (*n)++;
            else {
                free(r->name);
                free(r->rid);
            }
        }
    }
    free(a.s);
}

static int parse_rels(const char *buf, size_t len, struct rel *rels, int max)
{
    struct xml x = { buf, buf + len };
    struct xev e;
    struct xbuf a = { 0 };
    int n = 0;
    while (xml_next(&x, &e) != XE_END) {
        if ((e.kind == XE_OPEN || e.kind == XE_SELF) && is(&e, "Relationship") && n < max) {
            struct rel *r = &rels[n];
            memset(r, 0, sizeof(*r));
            if (xattr(&e, "Id", &a))
                r->id = strdup(xb_str(&a));
            if (xattr(&e, "Type", &a)) {
                size_t l = a.n;
                r->sheet = l >= 10 && !strcmp(xb_str(&a) + l - 10, "/worksheet");
            }
            if (xattr(&e, "Target", &a))
                r->target = strdup(xb_str(&a));
            if (r->id && r->target)
                n++;
            else {
                free(r->id);
                free(r->target);
            }
        }
    }
    free(a.s);
    return n;
}

/* "worksheets/sheet1.xml" next to xl/workbook.xml -> "xl/worksheets/sheet1.xml"; "/xl/..." is absolute; ".." steps up. */
static char *resolve_target(const char *target)
{
    char *full = malloc(strlen(target) + 8);
    if (target[0] == '/')
        strcpy(full, target + 1);
    else
        sprintf(full, "xl/%s", target);
    char *o = full;
    char *seg = full;
    char *out = malloc(strlen(full) + 1);
    o = out;
    *o = 0;
    while (*seg) {
        char *sl = strchr(seg, '/');
        size_t l = sl ? (size_t)(sl - seg) : strlen(seg);
        if (l == 2 && !strncmp(seg, "..", 2)) {
            while (o > out && o[-1] == '/')
                o--;
            while (o > out && o[-1] != '/')
                o--;
        } else if (!(l == 1 && seg[0] == '.') && l > 0) {
            memcpy(o, seg, l);
            o += l;
            if (sl)
                *o++ = '/';
        }
        seg += l + (sl ? 1 : 0);
    }
    *o = 0;
    free(full);
    return out;
}

static void parse_styles(const char *buf, size_t len, struct styles *st)
{
    struct xml x = { buf, buf + len };
    struct xev e;
    struct xbuf a = { 0 }, b = { 0 };
    struct { int id, kind; } custom[256];
    int ncustom = 0;
    bool in_xfs = false;
    while (xml_next(&x, &e) != XE_END) {
        if ((e.kind == XE_OPEN || e.kind == XE_SELF) && is(&e, "numFmt") && ncustom < 256) {
            if (xattr(&e, "numFmtId", &a) && xattr(&e, "formatCode", &b)) {
                custom[ncustom].id = atoi(xb_str(&a));
                custom[ncustom++].kind = classify_format(xb_str(&b));
            }
        } else if (is(&e, "cellXfs")) {
            in_xfs = e.kind == XE_OPEN;
        } else if (in_xfs && (e.kind == XE_OPEN || e.kind == XE_SELF) && is(&e, "xf") && st->nxf < MAX_XF) {
            int id = xattr(&e, "numFmtId", &a) ? atoi(xb_str(&a)) : 0, kind = -1;
            for (int i = 0; i < ncustom; i++)
                if (custom[i].id == id)
                    kind = custom[i].kind;
            if (kind < 0)
                kind = builtin_format(id);
            st->xf = realloc(st->xf, (size_t)(st->nxf + 1) * sizeof(int));
            st->xf[st->nxf++] = kind;
        }
    }
    free(a.s);
    free(b.s);
}

/* ---- a number as the cell would show it ---- */
static void civil_from_days(long z, int *y, int *m, int *d)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

static void fmt_plain(double v, char *o, size_t n)
{
    if (fabs(v) < 1e15 && v == floor(v))
        snprintf(o, n, "%.0f", v == 0 ? 0.0 : v);                      /* no "-0" */
    else
        snprintf(o, n, "%.15g", v);
}

static void fmt_double(double v, int kind, bool d1904, char *o, size_t n);

static void fmt_number(const char *text, int kind, bool d1904, char *o, size_t n)
{
    char *endp;
    double v = strtod(text, &endp);
    while (*endp == ' ')
        endp++;
    if (!*text || *endp || v != v || fabs(v) > 1e300) {
        snprintf(o, n, "%s", text);
        return;
    }
    fmt_double(v, kind, d1904, o, n);
}

static void fmt_double(double v, int kind, bool d1904, char *o, size_t n)
{
    if (v != v || fabs(v) > 1e300) {
        snprintf(o, n, "%g", v);
        return;
    }
    if ((kind & 15) == K_DATE && v >= 0 && v < 2958466) {
        double days = floor(v);
        long secs = (long)floor((v - days) * 86400 + 0.5);
        if (secs >= 86400) {
            secs -= 86400;
            days += 1;
        }
        long d1970 = d1904 ? (long)days - 24107 : (long)days - (days < 61 ? 25568 : 25569);
        int y, m, d;
        civil_from_days(d1970, &y, &m, &d);
        int hh = (int)(secs / 3600), mm = (int)(secs / 60 % 60), ss = (int)(secs % 60);
        if (days < 1 && !d1904)
            snprintf(o, n, "%02d:%02d:%02d", hh, mm, ss);
        else if (secs == 0)
            snprintf(o, n, "%04d-%02d-%02d", y, m, d);
        else
            snprintf(o, n, "%04d-%02d-%02d %02d:%02d:%02d", y, m, d, hh, mm, ss);
    } else if ((kind & 15) == K_PERCENT) {
        snprintf(o, n, "%.*f%%", (kind >> 4) & 15, v * 100);
    } else {
        fmt_plain(v, o, n);
    }
}

/* ---- one sheet's cells ---- */
struct xcell {
    int col;
    char *s;
    long sst;                           /* >= 0: the text is the shared string with this number, not yet read */
};

struct xrow {
    int num;
    struct xcell *c;
    int n, cap;
};

struct grid {
    struct xrow *row;
    int nrow, cap;
    int maxcol;
    long maxsst;
    int last_row;
    bool more_rows, more_cols;
};

static void row_free(struct xrow *r)
{
    for (int i = 0; i < r->n; i++)
        free(r->c[i].s);
    free(r->c);
}

static int col_of(const char *ref)
{
    int c = 0, k = 0;
    for (; (*ref >= 'A' && *ref <= 'Z') || (*ref >= 'a' && *ref <= 'z'); ref++, k++)
        c = c * 26 + (*ref & 31);
    return k > 0 && k <= 3 && c <= 16384 ? c - 1 : -1;
}

static void parse_sheet(const char *buf, size_t len, const struct styles *st, bool d1904, int max_rows, int max_cols, struct grid *g)
{
    struct xml x = { buf, buf + len };
    struct xev e;
    struct xbuf a = { 0 }, v = { 0 }, f = { 0 }, t = { 0 };
    bool in_data = false, in_row = false, in_c = false, in_v = false, in_f = false, in_is = false, in_t = false, in_rph = false;
    int rownum = 0, prevcol = -1, col = 0, style = 0;
    char ctype[16] = "";
    struct xrow row = { 0 };
    bool stop = false;
    g->maxsst = -1;
    g->maxcol = -1;
    while (!stop && xml_next(&x, &e) != XE_END) {
        if (e.kind == XE_TEXT) {
            if (in_v)
                xb_add_xml(&v, e.text, e.tlen);
            else if (in_f)
                xb_add_xml(&f, e.text, e.tlen);
            else if (in_t && !in_rph)
                xb_add_xml(&t, e.text, e.tlen);
            continue;
        }
        bool open = e.kind == XE_OPEN || e.kind == XE_SELF;
        if (!in_data) {
            if (open && is(&e, "dimension") && xattr(&e, "ref", &a)) {
                const char *r = strrchr(xb_str(&a), ':');
                r = r ? r + 1 : xb_str(&a);
                while (*r && (*r < '0' || *r > '9'))
                    r++;
                g->last_row = atoi(r);
            } else if (e.kind == XE_OPEN && is(&e, "sheetData")) {
                in_data = true;
            }
            continue;
        }
        if (is(&e, "sheetData") && e.kind == XE_CLOSE) {
            break;
        } else if (is(&e, "row")) {
            if (open) {
                rownum = xattr(&e, "r", &a) ? atoi(xb_str(&a)) : rownum + 1;
                if (in_row)
                    row_free(&row);                                     /* the row before was never closed */
                memset(&row, 0, sizeof(row));
                row.num = rownum;
                prevcol = -1;
                in_row = true;
            }
            if (e.kind == XE_CLOSE || e.kind == XE_SELF) {
                bool any = false;
                for (int i = 0; i < row.n; i++)
                    if (row.c[i].sst >= 0 || row.c[i].s[0])
                        any = true;
                if (in_row && any) {
                    if (g->nrow == max_rows) {
                        g->more_rows = true;
                        stop = true;
                        row_free(&row);
                    } else {
                        if (g->nrow == g->cap) {
                            g->cap = g->cap ? g->cap * 2 : 64;
                            g->row = realloc(g->row, (size_t)g->cap * sizeof(*g->row));
                        }
                        g->row[g->nrow++] = row;
                    }
                } else if (in_row) {
                    row_free(&row);
                }
                memset(&row, 0, sizeof(row));
                in_row = false;
            }
        } else if (is(&e, "c") && in_row) {
            if (open) {
                col = xattr(&e, "r", &a) ? col_of(xb_str(&a)) : prevcol + 1;
                if (col < 0)
                    col = prevcol + 1;
                prevcol = col;
                style = xattr(&e, "s", &a) ? atoi(xb_str(&a)) : 0;
                ctype[0] = 0;
                if (xattr(&e, "t", &a))
                    snprintf(ctype, sizeof(ctype), "%s", xb_str(&a));
                xb_reset(&v);
                xb_reset(&f);
                xb_reset(&t);
                in_c = true;
            }
            if (e.kind == XE_CLOSE || e.kind == XE_SELF) {
                if (in_c && col < max_cols) {
                    char num[64];
                    const char *val = "";
                    long sst = -1;
                    if (!strcmp(ctype, "s")) {
                        char *endp;
                        long k = strtol(xb_str(&v), &endp, 10);
                        if (v.n && !*endp && k >= 0 && k < MAX_SST)
                            sst = k;
                    } else if (!strcmp(ctype, "inlineStr")) {
                        val = xb_str(&t);
                    } else if (!strcmp(ctype, "b")) {
                        val = xb_str(&v)[0] == '1' ? "TRUE" : "FALSE";
                    } else if (!strcmp(ctype, "str") || !strcmp(ctype, "e")) {
                        val = xb_str(&v);
                    } else if (!strcmp(ctype, "d")) {
                        val = xb_str(&v);
                    } else if (v.n) {
                        fmt_number(xb_str(&v), style >= 0 && style < st->nxf ? st->xf[style] : K_PLAIN, d1904, num, sizeof(num));
                        val = num;
                    } else if (f.n) {
                        snprintf(num, sizeof(num), "=%.60s", xb_str(&f));
                        val = num;
                    }
                    if (row.n == row.cap) {
                        row.cap = row.cap ? row.cap * 2 : 8;
                        row.c = realloc(row.c, (size_t)row.cap * sizeof(*row.c));
                    }
                    row.c[row.n].col = col;
                    row.c[row.n].sst = sst;
                    row.c[row.n].s = sst >= 0 ? strdup("") : make_cell(val);
                    row.n++;
                    if (col > g->maxcol)
                        g->maxcol = col;
                    if (sst > g->maxsst)
                        g->maxsst = sst;
                } else if (in_c) {
                    g->more_cols = true;
                }
                in_c = false;
                in_v = in_f = in_is = in_t = in_rph = false;
            }
        } else if (in_c) {
            if (is(&e, "v"))
                in_v = e.kind == XE_OPEN;
            else if (is(&e, "f"))
                in_f = e.kind == XE_OPEN;
            else if (is(&e, "is"))
                in_is = e.kind == XE_OPEN;
            else if (is(&e, "t"))
                in_t = e.kind == XE_OPEN && in_is;
            else if (is(&e, "rPh"))
                in_rph = e.kind == XE_OPEN;
        }
    }
    if (in_row)
        row_free(&row);
    free(a.s);
    free(v.s);
    free(f.s);
    free(t.s);
}

/* The shared strings with the numbers `need` marks (the file is read as far as the last of them). */
static void parse_sst(const char *buf, size_t len, char **sst, const uint8_t *need, long count)
{
    struct xml x = { buf, buf + len };
    struct xev e;
    struct xbuf t = { 0 };
    long idx = 0;
    bool in_t = false, in_rph = false;
    while (idx < count && xml_next(&x, &e) != XE_END) {
        if (e.kind == XE_TEXT) {
            if (in_t && !in_rph)
                xb_add_xml(&t, e.text, e.tlen);
        } else if (is(&e, "si")) {
            if (e.kind == XE_OPEN) {
                xb_reset(&t);
            } else if (e.kind == XE_CLOSE) {
                if (need[idx]) {
                    sst[idx] = make_cell(xb_str(&t));
                }
                idx++;
                in_t = in_rph = false;
            } else {
                idx++;                                                  /* <si/>: an empty one */
            }
        } else if (is(&e, "t")) {
            in_t = e.kind == XE_OPEN;
        } else if (is(&e, "rPh")) {
            in_rph = e.kind == XE_OPEN;
        }
    }
    free(t.s);
}

static char *letters(int col)
{
    char t[8];
    int n = 0;
    for (int c = col + 1; c > 0; c = (c - 1) / 26)
        t[n++] = (char)('A' + (c - 1) % 26);
    char *o = malloc((size_t)n + 1);
    for (int i = 0; i < n; i++)
        o[i] = t[n - 1 - i];
    o[n] = 0;
    return o;
}

void xl_free(struct xl_book *b)
{
    for (int i = 0; i < b->nsheets; i++)
        free(b->sheet_name[i]);
    free(b->sheet_name);
    for (int c = 0; c < b->ncols; c++)
        free(b->head[c]);
    free(b->head);
    for (int r = 0; r < b->nrows; r++) {
        for (int c = 0; c < b->ncols; c++)
            free(b->cell[r][c]);
        free(b->cell[r]);
    }
    free(b->cell);
    memset(b, 0, sizeof(*b));
}

/* The grid as the book's table: the row number in column 0, the letters as the header, shared strings looked up (and copied: two cells may share one). */
static void fill_book(struct xl_book *b, const struct grid *g, char **sst)
{
    b->nrows = g->nrow;
    b->ncols = g->nrow ? g->maxcol + 2 : 0;
    if (!b->ncols)
        return;
    b->head = calloc((size_t)b->ncols, sizeof(char *));
    b->head[0] = strdup("");
    for (int c = 1; c < b->ncols; c++)
        b->head[c] = letters(c - 1);
    b->cell = calloc((size_t)g->nrow, sizeof(char **));
    for (int r = 0; r < g->nrow; r++) {
        b->cell[r] = calloc((size_t)b->ncols, sizeof(char *));
        char num[16];
        snprintf(num, sizeof(num), "%d", g->row[r].num);
        b->cell[r][0] = strdup(num);
        for (int i = 0; i < g->row[r].n; i++) {
            const struct xcell *c = &g->row[r].c[i];
            const char *text = c->sst >= 0 ? (sst && sst[c->sst] ? sst[c->sst] : "") : c->s;
            free(b->cell[r][1 + c->col]);                               /* a column twice in one row: the later wins */
            b->cell[r][1 + c->col] = strdup(text);
        }
        for (int c = 1; c < b->ncols; c++)
            if (!b->cell[r][c])
                b->cell[r][c] = strdup("");
    }
}

static int xlsx_read(const char *path, int sheet, int max_rows, int max_cols, struct xl_book *b, char *err, size_t errn)
{
    memset(b, 0, sizeof(*b));
    struct zip z;
    int zr = zip_open(&z, path, err, errn);
    if (zr < 0)
        return zr;
    size_t len;
    const char *why;
    int result = -2;
    struct sheetref refs[MAX_SHEETS];
    int nrefs = 0;
    bool d1904 = false;
    struct rel rels[MAX_SHEETS * 4];
    int nrels = 0;
    struct styles st = { 0 };
    struct grid g = { 0 };
    char **sst = NULL;
    uint8_t *need = NULL;
    char *wb = zip_read(&z, "xl/workbook.xml", PART_SMALL, &len, &why);
    if (!wb) {
        if (why) {
            seterr(err, errn, why);
            zip_close(&z);
            return -2;
        }
        zip_close(&z);
        return -1;                                                      /* a ZIP, but no workbook in it */
    }
    parse_workbook(wb, len, refs, &nrefs, &d1904);
    free(wb);
    char *rl = zip_read(&z, "xl/_rels/workbook.xml.rels", PART_SMALL, &len, &why);
    if (rl) {
        nrels = parse_rels(rl, len, rels, MAX_SHEETS * 4);
        free(rl);
    }
    for (int i = 0; i < nrefs; i++) {                                   /* the sheet's file: by its relationship, else sheetN.xml */
        for (int k = 0; k < nrels; k++)
            if (!strcmp(rels[k].id, refs[i].rid) && rels[k].sheet)
                refs[i].path = resolve_target(rels[k].target);
        if (!refs[i].path && !nrels) {
            char p[48];
            snprintf(p, sizeof(p), "xl/worksheets/sheet%d.xml", i + 1);
            refs[i].path = strdup(p);
        }
    }
    int *shown = calloc((size_t)(nrefs ? nrefs : 1), sizeof(int));          /* the visible worksheets (the hidden ones only when nothing else is there) */
    int nshown = 0;
    for (int pass = 0; pass < 2 && nshown == 0; pass++)
        for (int i = 0; i < nrefs; i++)
            if (refs[i].path && (pass == 1 || !refs[i].hidden))
                shown[nshown++] = i;
    if (nshown == 0) {
        seterr(err, errn, "A workbook with no worksheets.");
        goto done;
    }
    b->nsheets = nshown;
    b->sheet_name = calloc((size_t)nshown, sizeof(char *));
    for (int i = 0; i < nshown; i++)
        b->sheet_name[i] = cell_text(refs[shown[i]].name);
    b->sheet = sheet < 0 ? 0 : sheet >= nshown ? nshown - 1 : sheet;

    char *sx = zip_read(&z, "xl/styles.xml", PART_SMALL, &len, &why);
    if (sx) {
        parse_styles(sx, len, &st);
        free(sx);
    }
    /* The first rows lie in the first part of the sheet's XML: inflate 2 MB of it, and more only when that did not hold enough rows. */
    for (size_t cap = PART_FIRST;; cap = PART_BIG) {
        char *sh = zip_read(&z, refs[shown[b->sheet]].path, cap, &len, &why);
        if (!sh) {
            seterr(err, errn, why ? why : "The worksheet is missing from the file.");
            goto done;
        }
        parse_sheet(sh, len, &st, d1904, max_rows, max_cols, &g);
        free(sh);
        if (cap == PART_BIG || len < cap || g.more_rows || g.nrow >= max_rows)
            break;                                                      /* the whole sheet was read, or enough of it */
        for (int r = 0; r < g.nrow; r++)
            row_free(&g.row[r]);
        free(g.row);
        memset(&g, 0, sizeof(g));
    }
    b->last_row = g.last_row;
    b->more_rows = g.more_rows;
    b->more_cols = g.more_cols;

    if (g.maxsst >= 0) {                                                /* the shared strings the shown cells point at */
        long count = g.maxsst + 1;
        sst = calloc((size_t)count, sizeof(char *));
        need = calloc((size_t)count, 1);
        for (int r = 0; r < g.nrow; r++)
            for (int i = 0; i < g.row[r].n; i++)
                if (g.row[r].c[i].sst >= 0)
                    need[g.row[r].c[i].sst] = 1;
        char *ss = zip_read(&z, "xl/sharedStrings.xml", PART_BIG, &len, &why);
        if (ss) {
            parse_sst(ss, len, sst, need, count);
            free(ss);
        }
    }

    fill_book(b, &g, sst);
    result = 0;
done:
    for (int r = 0; r < g.nrow; r++)
        row_free(&g.row[r]);
    free(g.row);
    if (result != 0)
        xl_free(b);
    if (sst)
        for (long i = 0; i < g.maxsst + 1; i++)
            free(sst[i]);
    free(sst);
    free(need);
    free(st.xf);
    free(shown);
    for (int i = 0; i < nrefs; i++) {
        free(refs[i].name);
        free(refs[i].rid);
        free(refs[i].path);
    }
    for (int i = 0; i < nrels; i++) {
        free(rels[i].id);
        free(rels[i].target);
    }
    zip_close(&z);
    return result;
}

/* ======================================================================================================================================================
 * The old binary format: .xls = BIFF8 records in the "Workbook" stream of an OLE2 compound file (Excel 97 .. 2003).
 * ====================================================================================================================================================== */

#define OLE_MAXREG 0xfffffffau                  /* sector numbers from here up are FREE / END / FAT / DIFAT marks */
#define OLE_DIR_MAX 8192                        /* sectors of the directory at most */
#define MINI_MAX (32u << 20)                    /* the mini stream (the small streams' container) at most this big */

struct cfs {
    int fd;
    uint64_t fsize;
    uint32_t ssz, nsect;
    uint32_t *fat;
    uint32_t nfat;
    uint32_t dirstart, cutoff, mfstart, nmf;
};

struct cstream {
    const struct cfs *f;
    uint64_t size;
    uint32_t *sec;                      /* the sectors of a stream in the FAT ... */
    uint32_t nsec;
    uint8_t *mem;                       /* ... or the whole of a small one */
};

static void cfs_close(struct cfs *f)
{
    free(f->fat);
    f->fat = NULL;
}

static bool sector_read(const struct cfs *f, uint32_t id, void *buf)
{
    return id < f->nsect && pread_all(f->fd, buf, f->ssz, ((uint64_t)id + 1) * f->ssz);
}

/* The sectors of a chain: at most `maxn`, so a loop in a damaged file ends. */
static uint32_t *chain_of(const uint32_t *fat, uint32_t nfat, uint32_t start, uint32_t maxn, uint32_t *np)
{
    uint32_t *o = NULL, n = 0, cap = 0, s = start;
    while (s < OLE_MAXREG && s < nfat && n < maxn) {
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            uint32_t *t = realloc(o, cap * sizeof(uint32_t));
            if (!t) {
                free(o);
                *np = 0;
                return NULL;
            }
            o = t;
        }
        o[n++] = s;
        s = fat[s];
    }
    *np = n;
    return o;
}

/* 0 = opened; -1 = not an OLE2 file; -2 = damaged (err). */
static int cfs_open(struct cfs *f, int fd, char *err, size_t errn)
{
    memset(f, 0, sizeof(*f));
    f->fd = fd;
    struct stat st;
    uint8_t h[512];
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 1024 || !pread_all(fd, h, 512, 0) || memcmp(h, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8))
        return -1;
    f->fsize = (uint64_t)st.st_size;
    unsigned shift = u16(h + 30), mshift = u16(h + 32);
    if ((shift != 9 && shift != 12) || mshift != 6) {
        seterr(err, errn, "A damaged compound file (sector sizes).");
        return -2;
    }
    f->ssz = 1u << shift;
    f->nsect = (uint32_t)((f->fsize - f->ssz) / f->ssz);
    uint32_t nfatsec = u32(h + 44), difstart = u32(h + 68), ndif = u32(h + 72);
    f->dirstart = u32(h + 48);
    f->cutoff = u32(h + 56);
    f->mfstart = u32(h + 60);
    f->nmf = u32(h + 64);
    if (nfatsec == 0 || nfatsec > f->nsect || ndif > f->nsect) {
        seterr(err, errn, "A damaged compound file (the sector table).");
        return -2;
    }
    uint32_t *ids = malloc((size_t)nfatsec * sizeof(uint32_t));
    uint8_t *sec = malloc(f->ssz);
    if (!ids || !sec) {
        free(ids);
        free(sec);
        return -2;
    }
    uint32_t got = 0;
    for (uint32_t i = 0; i < 109 && got < nfatsec; i++)
        ids[got++] = u32(h + 76 + 4 * i);
    for (uint32_t s = difstart, k = 0; k < ndif && got < nfatsec && s < OLE_MAXREG; k++) {
        if (!sector_read(f, s, sec))
            break;
        for (uint32_t i = 0; i < f->ssz / 4 - 1 && got < nfatsec; i++)
            ids[got++] = u32(sec + 4 * i);
        s = u32(sec + f->ssz - 4);
    }
    f->nfat = nfatsec * (f->ssz / 4);
    f->fat = calloc(f->nfat, sizeof(uint32_t));
    bool ok = f->fat != NULL;
    for (uint32_t i = 0; ok && i < got; i++) {
        if (!sector_read(f, ids[i], sec)) {
            for (uint32_t k = 0; k < f->ssz / 4; k++)
                f->fat[i * (f->ssz / 4) + k] = 0xffffffffu;          /* a FAT sector that cannot be read: its sectors are free */
            continue;
        }
        for (uint32_t k = 0; k < f->ssz / 4; k++)
            f->fat[i * (f->ssz / 4) + k] = u32(sec + 4 * k);
    }
    for (uint32_t i = got * (f->ssz / 4); ok && i < f->nfat; i++)
        f->fat[i] = 0xffffffffu;
    free(ids);
    free(sec);
    if (!ok) {
        cfs_close(f);
        return -2;
    }
    return 0;
}

/* Finds the workbook's stream in the directory; 1 = found (`biff5` when it is the old name "Book"), 0 = no. */
static int cfs_find_workbook(const struct cfs *f, uint32_t *start, uint64_t *size, bool *biff5, uint32_t *rstart, uint64_t *rsize)
{
    uint32_t nd = 0;
    uint32_t *ds = chain_of(f->fat, f->nfat, f->dirstart, OLE_DIR_MAX < f->nsect ? OLE_DIR_MAX : f->nsect, &nd);
    uint8_t *sec = malloc(f->ssz);
    int found = 0;
    *rstart = 0xfffffffeu;
    *rsize = 0;
    for (uint32_t i = 0; sec && i < nd; i++) {
        if (!sector_read(f, ds[i], sec))
            break;
        for (uint32_t e = 0; e + 128 <= f->ssz; e += 128) {
            const uint8_t *d = sec + e;
            unsigned type = d[66], nlen = u16(d + 64);
            if (type == 5) {
                *rstart = u32(d + 116);
                *rsize = u32(d + 120);
            }
            if (type != 2 || nlen < 2 || nlen > 64)
                continue;
            char name[33];
            unsigned k = 0;
            for (; k + 1 < nlen / 2 && k < 32; k++)
                name[k] = d[2 * k + 1] ? '?' : (char)d[2 * k];
            name[k] = 0;
            bool wb = !strcasecmp(name, "Workbook"), bk = !strcasecmp(name, "Book");
            if (wb || (bk && !found)) {                                 /* "Workbook" wins over the old name "Book" */
                *start = u32(d + 116);
                *size = u32(d + 120);
                *biff5 = !wb;
                found = 1;
            }
        }
    }
    free(sec);
    free(ds);
    return found;
}

/* A stream: its sector list (the usual) or, when it is under the cutoff, copied out of the mini stream. 0 = ok. */
static int cstream_open(struct cstream *s, const struct cfs *f, uint32_t start, uint64_t size, uint32_t rstart, uint64_t rsize)
{
    memset(s, 0, sizeof(*s));
    s->f = f;
    if (size >= f->cutoff) {
        uint32_t want = (uint32_t)((size + f->ssz - 1) / f->ssz);
        s->sec = chain_of(f->fat, f->nfat, start, want < f->nsect ? want : f->nsect, &s->nsec);
        s->size = (uint64_t)s->nsec * f->ssz < size ? (uint64_t)s->nsec * f->ssz : size;
        return s->nsec ? 0 : -1;
    }
    if (rsize > MINI_MAX || size > rsize)
        return -1;
    uint32_t nr = 0, nm = 0, nmf = 0;
    uint32_t *rs = chain_of(f->fat, f->nfat, rstart, (uint32_t)((rsize + f->ssz - 1) / f->ssz), &nr);
    uint32_t *ms = chain_of(f->fat, f->nfat, f->mfstart, f->nmf < f->nsect ? f->nmf : f->nsect, &nmf);
    uint8_t *mini = calloc(1, (size_t)nr * f->ssz + 1), *sec = malloc(f->ssz);
    uint32_t *mfat = calloc(1, ((size_t)nmf * (f->ssz / 4) + 1) * sizeof(uint32_t));
    int rv = -1;
    if (mini && sec && mfat && rs && ms) {
        for (uint32_t i = 0; i < nr; i++)
            if (sector_read(f, rs[i], sec))
                memcpy(mini + (size_t)i * f->ssz, sec, f->ssz);
        for (uint32_t i = 0; i < nmf; i++)
            if (sector_read(f, ms[i], sec))
                for (uint32_t k = 0; k < f->ssz / 4; k++)
                    mfat[(size_t)i * (f->ssz / 4) + k] = u32(sec + 4 * k);
        uint32_t *mc = chain_of(mfat, (uint32_t)nmf * (f->ssz / 4), start, (uint32_t)((size + 63) / 64), &nm);
        s->mem = calloc(1, (size_t)nm * 64 + 1);
        if (s->mem && mc) {
            uint32_t good = 0;
            for (; good < nm; good++) {
                if ((uint64_t)mc[good] * 64 + 64 > (uint64_t)nr * f->ssz)
                    break;
                memcpy(s->mem + (size_t)good * 64, mini + (size_t)mc[good] * 64, 64);
            }
            s->size = (uint64_t)good * 64 < size ? (uint64_t)good * 64 : size;
            rv = good ? 0 : -1;
        }
        free(mc);
    }
    free(mini);
    free(sec);
    free(mfat);
    free(rs);
    free(ms);
    return rv;
}

static void cstream_close(struct cstream *s)
{
    free(s->sec);
    free(s->mem);
    memset(s, 0, sizeof(*s));
}

/* Up to n bytes at `off`; runs of neighbouring sectors are read with one call. Returns how many. */
static size_t cstream_read(const struct cstream *s, uint64_t off, uint8_t *buf, size_t n)
{
    if (off >= s->size)
        return 0;
    if (n > s->size - off)
        n = (size_t)(s->size - off);
    if (s->mem) {
        memcpy(buf, s->mem + off, n);
        return n;
    }
    size_t done = 0;
    uint32_t ssz = s->f->ssz;
    while (done < n) {
        uint64_t at = off + done;
        uint32_t i = (uint32_t)(at / ssz), within = (uint32_t)(at % ssz), run = 1;
        if (i >= s->nsec || s->sec[i] >= s->f->nsect)
            break;
        while (i + run < s->nsec && s->sec[i + run] == s->sec[i] + run && (uint64_t)run * ssz < n - done + within && s->sec[i + run] < s->f->nsect)
            run++;
        size_t chunk = (size_t)run * ssz - within;
        if (chunk > n - done)
            chunk = n - done;
        if (!pread_all(s->f->fd, buf + done, chunk, ((uint64_t)s->sec[i] + 1) * ssz + within))
            break;
        done += chunk;
    }
    return done;
}

/* ---- a reader of BIFF records: id, length, body, with a buffer so that the small records do not cost a call each ---- */
#define RR_BUF (64u << 10)
#define REC_MAX 8224

struct rr {
    const struct cstream *s;
    uint64_t pos;                       /* the stream offset of the next record */
    uint8_t *buf;
    uint64_t bpos;
    size_t blen;
};

static bool rr_open(struct rr *r, const struct cstream *s, uint64_t pos)
{
    r->s = s;
    r->pos = pos;
    r->bpos = 0;
    r->blen = 0;
    r->buf = malloc(RR_BUF + REC_MAX + 8);
    return r->buf != NULL;
}

static bool rr_have(struct rr *r, uint64_t off, size_t n)
{
    if (off >= r->bpos && off + n <= r->bpos + r->blen)
        return true;
    r->bpos = off;
    r->blen = cstream_read(r->s, off, r->buf, RR_BUF + REC_MAX);
    return r->blen >= n;
}

static bool rr_next(struct rr *r, unsigned *id, unsigned *len, const uint8_t **data)
{
    if (!rr_have(r, r->pos, 4))
        return false;
    const uint8_t *h = r->buf + (r->pos - r->bpos);
    *id = u16(h);
    *len = u16(h + 2);
    if (*len > REC_MAX || !rr_have(r, r->pos, 4 + (size_t)*len))
        return false;
    *data = r->buf + (r->pos - r->bpos) + 4;
    r->pos += 4 + (uint64_t)*len;
    return true;
}

/* ---- strings ---- */
/* Letters of a string, one byte or one UTF-16 unit each, as UTF-8. `hi` (or NULL) carries a high surrogate whose partner is in the next piece. */
static void xb_units(struct xbuf *b, const uint8_t *p, size_t units, bool wide, unsigned *hi)
{
    for (size_t i = 0; i < units; i++) {
        unsigned cp = wide ? u16(p + 2 * i) : p[i];
        if (hi && *hi) {
            if (wide && cp >= 0xdc00 && cp < 0xe000) {
                xb_utf8(b, 0x10000 + ((*hi - 0xd800) << 10) + (cp - 0xdc00));
                *hi = 0;
                continue;
            }
            xb_utf8(b, 0xfffd);
            *hi = 0;
        }
        if (wide && cp >= 0xd800 && cp < 0xdc00) {
            if (i + 1 < units && u16(p + 2 * i + 2) >= 0xdc00 && u16(p + 2 * i + 2) < 0xe000) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (u16(p + 2 * i + 2) - 0xdc00);
                i++;
            } else if (hi && i + 1 == units) {
                *hi = cp;
                continue;
            } else {
                cp = 0xfffd;
            }
        } else if (cp >= 0xd800 && cp < 0xe000) {
            cp = 0xfffd;
        }
        xb_utf8(b, cp ? cp : ' ');
    }
}

/* An XLUnicodeString (a u16 length) or ShortXLUnicodeString (a u8 one) in a record: flags byte, then the letters, one or two bytes each. Returns the
 * bytes it used, or -1 when it does not fit. */
static int xls_string(const uint8_t *p, size_t avail, bool short_cch, struct xbuf *b)
{
    size_t hs = short_cch ? 2 : 3;
    if (avail < hs)
        return -1;
    size_t cch = short_cch ? p[0] : u16(p);
    bool wide = p[hs - 1] & 1;
    size_t need = cch * (wide ? 2 : 1);
    if (avail < hs + need)
        return -1;
    xb_units(b, p + hs, cch, wide, NULL);
    return (int)(hs + need);
}

/* ---- the workbook's globals ---- */
struct xls {
    bool d1904;
    int *xfi;                           /* the number format of each XF */
    int nxf;
    struct { int id, kind; } custom[512];
    int ncustom;
    struct {
        uint32_t pos;
        char *name;
        bool hidden;
    } sh[MAX_SHEETS];
    int nsh;
    uint64_t sstpos;
    bool have_sst, encrypted;
    unsigned vers;
};

static int xls_kind(const struct xls *x, int ixfe)
{
    if (ixfe < 0 || ixfe >= x->nxf)
        return K_PLAIN;
    int id = x->xfi[ixfe];
    for (int i = 0; i < x->ncustom; i++)
        if (x->custom[i].id == id)
            return x->custom[i].kind;
    return builtin_format(id);
}

static void xls_globals(const struct cstream *s, struct xls *x)
{
    struct rr r;
    if (!rr_open(&r, s, 0))
        return;
    unsigned id, len, depth = 0;
    const uint8_t *d;
    struct xbuf t = { 0 };
    bool first = true;
    while (true) {
        uint64_t at = r.pos;
        if (!rr_next(&r, &id, &len, &d))
            break;
        if (first) {
            if (id != 0x0809 || len < 4)
                break;
            x->vers = u16(d);
            first = false;
            depth = 1;
            continue;
        }
        if (id == 0x0809) {
            depth++;
        } else if (id == 0x000a) {
            if (--depth == 0)
                break;
        } else if (depth != 1) {
            continue;
        } else if (id == 0x0022 && len >= 2) {
            x->d1904 = u16(d) != 0;
        } else if (id == 0x002f) {
            x->encrypted = true;
        } else if (id == 0x041e && len >= 5 && x->ncustom < 512) {
            xb_reset(&t);
            if (xls_string(d + 2, len - 2, false, &t) > 0) {
                x->custom[x->ncustom].id = u16(d);
                x->custom[x->ncustom++].kind = classify_format(xb_str(&t));
            }
        } else if (id == 0x00e0 && len >= 4 && x->nxf < MAX_XF) {
            x->xfi = realloc(x->xfi, (size_t)(x->nxf + 1) * sizeof(int));
            x->xfi[x->nxf++] = (int)u16(d + 2);
        } else if (id == 0x0085 && len >= 8 && x->nsh < MAX_SHEETS) {
            xb_reset(&t);
            if (d[5] == 0 && xls_string(d + 6, len - 6, true, &t) > 0) {          /* dt 0 = a worksheet (not a chart or a macro sheet) */
                x->sh[x->nsh].pos = u32(d);
                x->sh[x->nsh].hidden = (d[4] & 3) != 0;
                x->sh[x->nsh++].name = strdup(xb_str(&t));
            }
        } else if (id == 0x00fc && !x->have_sst) {
            x->sstpos = at;
            x->have_sst = true;
        }
    }
    free(t.s);
    free(r.buf);
}

/* ---- one sheet's cells ---- */
struct gb {
    struct grid *g;
    struct xrow cur;
    bool have, stop;
    int max_rows, max_cols;
};

static void gb_flush(struct gb *b)
{
    if (!b->have)
        return;
    bool any = false;
    for (int i = 0; i < b->cur.n; i++)
        if (b->cur.c[i].sst >= 0 || b->cur.c[i].s[0])
            any = true;
    if (any) {
        struct grid *g = b->g;
        if (g->nrow == g->cap) {
            g->cap = g->cap ? g->cap * 2 : 64;
            g->row = realloc(g->row, (size_t)g->cap * sizeof(*g->row));
        }
        g->row[g->nrow++] = b->cur;
    } else {
        row_free(&b->cur);
    }
    memset(&b->cur, 0, sizeof(b->cur));
    b->have = false;
}

/* A cell: the text (taken over) or a shared string number; `row` counts from 1, `col` from 0. */
static void gb_cell(struct gb *b, int row, int col, char *text, long sst)
{
    if (b->stop || (!text && sst < 0)) {
        free(text);
        return;
    }
    if (col >= b->max_cols) {
        b->g->more_cols = true;
        free(text);
        return;
    }
    if (!b->have || b->cur.num != row) {
        gb_flush(b);
        if (b->g->nrow >= b->max_rows) {
            b->g->more_rows = true;
            b->stop = true;
            free(text);
            return;
        }
        b->cur.num = row;
        b->have = true;
    }
    if (b->cur.n == b->cur.cap) {
        b->cur.cap = b->cur.cap ? b->cur.cap * 2 : 8;
        b->cur.c = realloc(b->cur.c, (size_t)b->cur.cap * sizeof(*b->cur.c));
    }
    b->cur.c[b->cur.n].col = col;
    b->cur.c[b->cur.n].sst = sst;
    b->cur.c[b->cur.n].s = sst >= 0 ? strdup("") : text;
    b->cur.n++;
    if (col > b->g->maxcol)
        b->g->maxcol = col;
    if (sst > b->g->maxsst)
        b->g->maxsst = sst;
}

static double rk_value(uint32_t rk)
{
    double v;
    if (rk & 2) {
        v = (double)((int32_t)rk >> 2);
    } else {
        uint64_t bits = (uint64_t)(rk & 0xfffffffcu) << 32;
        memcpy(&v, &bits, sizeof(v));
    }
    return rk & 1 ? v / 100 : v;
}

static const char *biff_error(unsigned code)
{
    switch (code) {
    case 0x00: return "#NULL!";
    case 0x07: return "#DIV/0!";
    case 0x0f: return "#VALUE!";
    case 0x17: return "#REF!";
    case 0x1d: return "#NAME?";
    case 0x24: return "#NUM!";
    case 0x2a: return "#N/A";
    default: return "#ERR!";
    }
}

static void xls_number(struct gb *b, const struct xls *x, int row, int col, int ixfe, double v)
{
    char num[64];
    fmt_double(v, xls_kind(x, ixfe), x->d1904, num, sizeof(num));
    gb_cell(b, row, col, cell_text(num), -1);
}

static void xls_sheet(const struct cstream *s, uint32_t pos, const struct xls *x, int max_rows, int max_cols, struct grid *g)
{
    struct rr r;
    if (!rr_open(&r, s, pos))
        return;
    struct gb b = { .g = g, .max_rows = max_rows, .max_cols = max_cols };
    struct xbuf t = { 0 };
    unsigned id, len, depth = 0;
    const uint8_t *d;
    bool first = true, pend = false;
    int prow = 0, pcol = 0;
    g->maxsst = -1;
    g->maxcol = -1;
    while (!b.stop && rr_next(&r, &id, &len, &d)) {
        if (first) {
            if (id != 0x0809)
                break;
            first = false;
            depth = 1;
            continue;
        }
        if (id == 0x0809) {
            depth++;
            continue;
        }
        if (id == 0x000a) {
            if (--depth == 0)
                break;
            continue;
        }
        if (depth != 1)
            continue;
        if (id != 0x0207 && id != 0x0006)
            pend = pend && (id == 0x04bc || id == 0x0221 || id == 0x0236);          /* a formula's string follows it (maybe after its shared / array formula) */
        switch (id) {
        case 0x0200:                                                    /* DIMENSION */
            if (len >= 10)
                g->last_row = (int)u32(d + 4);
            break;
        case 0x00fd:                                                    /* LABELSST */
            if (len >= 10) {
                uint32_t k = u32(d + 6);
                if (k < MAX_SST)
                    gb_cell(&b, (int)u16(d) + 1, (int)u16(d + 2), NULL, (long)k);
                else
                    gb_cell(&b, (int)u16(d) + 1, (int)u16(d + 2), strdup(""), -1);
            }
            break;
        case 0x0203:                                                    /* NUMBER */
            if (len >= 14) {
                double v;
                memcpy(&v, d + 6, 8);
                xls_number(&b, x, (int)u16(d) + 1, (int)u16(d + 2), (int)u16(d + 4), v);
            }
            break;
        case 0x027e:                                                    /* RK */
            if (len >= 10)
                xls_number(&b, x, (int)u16(d) + 1, (int)u16(d + 2), (int)u16(d + 4), rk_value(u32(d + 6)));
            break;
        case 0x00bd:                                                    /* MULRK: a run of RK numbers */
            for (unsigned k = 0; len >= 6 && 4 + 6 * (k + 1) + 2 <= len; k++)
                xls_number(&b, x, (int)u16(d) + 1, (int)u16(d + 2) + (int)k, (int)u16(d + 4 + 6 * k), rk_value(u32(d + 6 + 6 * k)));
            break;
        case 0x0204:                                                    /* LABEL, RSTRING */
        case 0x00d6:
            if (len >= 9) {
                xb_reset(&t);
                if (xls_string(d + 6, len - 6, false, &t) >= 0)
                    gb_cell(&b, (int)u16(d) + 1, (int)u16(d + 2), cell_text(xb_str(&t)), -1);
            }
            break;
        case 0x0205:                                                    /* BOOLERR */
            if (len >= 8)
                gb_cell(&b, (int)u16(d) + 1, (int)u16(d + 2), strdup(d[7] ? biff_error(d[6]) : d[6] ? "TRUE" : "FALSE"), -1);
            break;
        case 0x0006:                                                    /* FORMULA: the value Excel stored with it */
            pend = false;
            if (len >= 20) {
                int row = (int)u16(d) + 1, col = (int)u16(d + 2);
                const uint8_t *res = d + 6;
                if (res[6] == 0xff && res[7] == 0xff) {
                    if (res[0] == 0) {
                        pend = true;
                        prow = row;
                        pcol = col;
                    } else if (res[0] == 1) {
                        gb_cell(&b, row, col, strdup(res[2] ? "TRUE" : "FALSE"), -1);
                    } else if (res[0] == 2) {
                        gb_cell(&b, row, col, strdup(biff_error(res[2])), -1);
                    }
                } else {
                    double v;
                    memcpy(&v, res, 8);
                    xls_number(&b, x, row, col, (int)u16(d + 4), v);
                }
            }
            break;
        case 0x0207:                                                    /* STRING: the text result of the formula before */
            if (pend) {
                xb_reset(&t);
                if (xls_string(d, len, false, &t) >= 0)
                    gb_cell(&b, prow, pcol, cell_text(xb_str(&t)), -1);
                pend = false;
            }
            break;
        default:
            break;
        }
    }
    gb_flush(&b);
    if (b.have)
        row_free(&b.cur);
    free(t.s);
    free(r.buf);
}

/* The shared strings the cells use (a string runs over CONTINUE records, and each such piece starts with its own flags byte). */
struct sc {
    struct rr r;
    const uint8_t *p, *end;
};

static bool sc_more(struct sc *c)
{
    unsigned id, len;
    const uint8_t *d;
    if (!rr_next(&c->r, &id, &len, &d) || id != 0x003c)
        return false;
    c->p = d;
    c->end = d + len;
    return true;
}

static bool sc_skip(struct sc *c, size_t n)
{
    while (n) {
        size_t avail = (size_t)(c->end - c->p), take = avail < n ? avail : n;
        c->p += take;
        n -= take;
        if (n && !sc_more(c))
            return false;
    }
    return true;
}

static void xls_sst(const struct cstream *s, uint64_t at, char **sst, const uint8_t *need, long count)
{
    struct sc c;
    if (!rr_open(&c.r, s, at))
        return;
    unsigned id, len;
    const uint8_t *d;
    struct xbuf t = { 0 };
    if (rr_next(&c.r, &id, &len, &d) && id == 0x00fc && len >= 8) {
        uint32_t unique = u32(d + 4);
        c.p = d + 8;
        c.end = d + len;
        for (long i = 0; i < count && i < (long)unique; i++) {
            if (c.p >= c.end && !sc_more(&c))
                break;
            if (c.end - c.p < 3)
                break;
            size_t cch = u16(c.p);
            unsigned flags = c.p[2];
            c.p += 3;
            size_t runs = 0, ext = 0;
            if (flags & 8) {
                if (c.end - c.p < 2)
                    break;
                runs = u16(c.p);
                c.p += 2;
            }
            if (flags & 4) {
                if (c.end - c.p < 4)
                    break;
                ext = u32(c.p);
                c.p += 4;
            }
            bool wide = flags & 1, bad = false;
            unsigned hi = 0;
            xb_reset(&t);
            for (size_t left = cch; left > 0;) {
                size_t unit = wide ? 2 : 1, take = (size_t)(c.end - c.p) / unit;
                if (take > left)
                    take = left;
                if (need[i])
                    xb_units(&t, c.p, take, wide, &hi);
                c.p += take * unit;
                left -= take;
                if (left > 0) {
                    if (!sc_more(&c) || c.p >= c.end) {
                        bad = true;
                        break;
                    }
                    wide = *c.p & 1;                                    /* a new piece: its first byte says how the letters are written */
                    c.p++;
                }
            }
            if (hi && need[i])
                xb_utf8(&t, 0xfffd);
            if (bad || !sc_skip(&c, runs * 4 + ext))
                break;
            if (need[i])
                sst[i] = cell_text(xb_str(&t));
        }
    }
    free(t.s);
    free(c.r.buf);
}

static int xls_read(const char *path, int sheet, int max_rows, int max_cols, struct xl_book *b, char *err, size_t errn)
{
    memset(b, 0, sizeof(*b));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct cfs f;
    int r = cfs_open(&f, fd, err, errn);
    if (r < 0) {
        close(fd);
        return r;
    }
    uint32_t wstart = 0, rstart = 0;
    uint64_t wsize = 0, rsize = 0;
    bool biff5 = false;
    struct cstream s = { 0 };
    struct xls x = { 0 };
    struct grid g = { 0 };
    char **sst = NULL;
    uint8_t *need = NULL;
    int *shown = NULL;
    int result = -2;
    if (!cfs_find_workbook(&f, &wstart, &wsize, &biff5, &rstart, &rsize)) {
        result = -1;                                                    /* an OLE2 file with no workbook (a .doc, a .ppt) */
        goto done;
    }
    if (biff5) {
        seterr(err, errn, "An old Excel file (Excel 5 / 95, \"BIFF5\"): not read.");
        goto done;
    }
    if (cstream_open(&s, &f, wstart, wsize, rstart, rsize) < 0) {
        seterr(err, errn, "A damaged workbook (its stream cannot be read).");
        goto done;
    }
    xls_globals(&s, &x);
    if (x.encrypted) {
        seterr(err, errn, "The workbook is password protected.");
        goto done;
    }
    if (x.vers != 0x0600) {
        seterr(err, errn, "An Excel file older than Excel 97: not read.");
        goto done;
    }
    shown = calloc((size_t)(x.nsh ? x.nsh : 1), sizeof(int));
    int nshown = 0;
    for (int pass = 0; pass < 2 && nshown == 0; pass++)
        for (int i = 0; i < x.nsh; i++)
            if (pass == 1 || !x.sh[i].hidden)
                shown[nshown++] = i;
    if (nshown == 0) {
        seterr(err, errn, "A workbook with no worksheets.");
        goto done;
    }
    b->nsheets = nshown;
    b->sheet_name = calloc((size_t)nshown, sizeof(char *));
    for (int i = 0; i < nshown; i++)
        b->sheet_name[i] = cell_text(x.sh[shown[i]].name);
    b->sheet = sheet < 0 ? 0 : sheet >= nshown ? nshown - 1 : sheet;
    if (x.sh[shown[b->sheet]].pos >= s.size) {
        seterr(err, errn, "The worksheet lies beyond the part of the file that was read.");
        goto done;
    }
    xls_sheet(&s, x.sh[shown[b->sheet]].pos, &x, max_rows, max_cols, &g);
    b->last_row = g.last_row;
    b->more_rows = g.more_rows;
    b->more_cols = g.more_cols;
    if (g.maxsst >= 0 && x.have_sst) {
        long count = g.maxsst + 1;
        sst = calloc((size_t)count, sizeof(char *));
        need = calloc((size_t)count, 1);
        for (int rr = 0; rr < g.nrow; rr++)
            for (int i = 0; i < g.row[rr].n; i++)
                if (g.row[rr].c[i].sst >= 0)
                    need[g.row[rr].c[i].sst] = 1;
        xls_sst(&s, x.sstpos, sst, need, count);
    }
    fill_book(b, &g, sst);
    result = 0;
done:
    for (int rr = 0; rr < g.nrow; rr++)
        row_free(&g.row[rr]);
    free(g.row);
    if (result != 0)
        xl_free(b);
    if (sst)
        for (long i = 0; i < g.maxsst + 1; i++)
            free(sst[i]);
    free(sst);
    free(need);
    free(shown);
    for (int i = 0; i < x.nsh; i++)
        free(x.sh[i].name);
    free(x.xfi);
    cstream_close(&s);
    cfs_close(&f);
    close(fd);
    return result;
}

int xl_read(const char *path, int sheet, int max_rows, int max_cols, struct xl_book *b, char *err, size_t errn)
{
    uint8_t magic[8] = { 0 };
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    bool ole = pread_all(fd, magic, 8, 0) && !memcmp(magic, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8);
    close(fd);
    return ole ? xls_read(path, sheet, max_rows, max_cols, b, err, errn) : xlsx_read(path, sheet, max_rows, max_cols, b, err, errn);
}
