#include "xlsxread.h"

#include "../src/img.h"
#include "parquet.h"

#include <ctype.h>
#include "cptables.h"
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
    size_t limit;                       /* 0 = BUF_LIMIT */
};

static void xb_add(struct xbuf *b, const char *p, size_t n)
{
    size_t lim = b->limit ? b->limit : BUF_LIMIT;
    if (b->n + n > lim)
        n = b->n < lim ? lim - b->n : 0;
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

/* ======================================================================================================================================================
 * OpenDocument spreadsheets: .ods / .ots = a ZIP with content.xml (all sheets in one XML part).
 * ====================================================================================================================================================== */

#define PART_HUGE (128u << 20)                  /* content.xml: as much as this when the first parts did not reach the wanted sheet */
#define ODS_MAX_REPEAT 16384                    /* a cell repeated more often than this is cut */
#define ODS_STYLES 32

struct ods_out {
    char *name[MAX_SHEETS];
    bool hidden[MAX_SHEETS];
    int nt;                             /* the tables (sheets) seen */
    int vis;                            /* how many of them are eligible (visible, or all) */
    bool sel_found, sel_done;
    struct grid g;
};

static void ods_out_free(struct ods_out *o)
{
    for (int i = 0; i < o->nt; i++)
        free(o->name[i]);
    for (int r = 0; r < o->g.nrow; r++)
        row_free(&o->g.row[r]);
    free(o->g.row);
    memset(o, 0, sizeof(*o));
}

/* Text of a cell: whitespace runs of the XML are one blank (text:s adds real ones). */
static void ods_text(struct xbuf *b, struct xbuf *tmp, const char *p, size_t n)
{
    xb_reset(tmp);
    xb_add_xml(tmp, p, n);
    for (size_t i = 0; i < tmp->n; i++) {
        char c = tmp->s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (b->n && b->s[b->n - 1] != ' ')
                xb_add(b, " ", 1);
        } else {
            xb_add(b, &c, 1);
        }
    }
}

/* What a cell shows when the file stored no text for it (a library wrote it): from the typed value. */
static char *ods_fallback(const char *vtype, const char *val, const char *date, const char *tim, const char *boolv, const char *str, const char *formula)
{
    char out[160] = "";
    if (!strcmp(vtype, "float") || !strcmp(vtype, "currency")) {
        if (val[0])
            fmt_number(val, K_PLAIN, false, out, sizeof(out));
    } else if (!strcmp(vtype, "percentage")) {
        char *endp;
        double v = strtod(val, &endp);
        if (val[0] && !*endp) {
            fmt_plain(v * 100, out, sizeof(out) - 2);
            strcat(out, "%");
        }
    } else if (!strcmp(vtype, "date")) {
        snprintf(out, sizeof(out), "%s", date);
        char *t = strchr(out, 'T');
        if (t) {
            if (!strcmp(t, "T00:00:00") || !strncmp(t, "T00:00:00.", 10))
                *t = 0;
            else
                *t = ' ';
        }
    } else if (!strcmp(vtype, "time")) {
        int h = 0, m = 0;
        double sec = 0;
        if (sscanf(tim, "PT%dH%dM%lfS", &h, &m, &sec) == 3)
            snprintf(out, sizeof(out), "%02d:%02d:%02d", h, m, (int)sec);
        else
            snprintf(out, sizeof(out), "%s", tim);
    } else if (!strcmp(vtype, "boolean")) {
        snprintf(out, sizeof(out), "%s", !strcmp(boolv, "true") ? "TRUE" : boolv[0] ? "FALSE" : "");
    } else if (!strcmp(vtype, "string")) {
        snprintf(out, sizeof(out), "%s", str);
    }
    if (!out[0] && formula[0])
        snprintf(out, sizeof(out), "=%.100s", strncmp(formula, "of:=", 4) ? (formula[0] == '=' ? formula + 1 : formula) : formula + 4);
    return cell_text(out);
}

static void ods_parse(const char *buf, size_t len, int sel, bool allow_hidden, int max_rows, int max_cols, struct ods_out *o)
{
    struct xml x = { buf, buf + len };
    struct xev e;
    struct xbuf a = { 0 }, text = { 0 }, tmp = { 0 }, vt = { 0 }, val = { 0 }, date = { 0 }, tim = { 0 }, boolv = { 0 }, str = { 0 }, formula = { 0 };
    char hid_style[ODS_STYLES][48];
    int nhid = 0;
    char cur_tstyle[48] = "";
    int depth_t = 0, skip = 0;
    bool in_sel = false, in_row = false, in_cell = false, in_p = false, fast = false;
    int row = 1, nrr = 1, col = 0, nrep = 1;
    bool covered = false;
    struct { int col; char *s; } *rc = NULL;
    int nrc = 0, caprc = 0;
    struct grid *g = &o->g;
    struct gb gb = { .g = g, .max_rows = max_rows, .max_cols = max_cols };
    g->maxsst = -1;
    g->maxcol = -1;
    while (true) {
        if (fast) {                                                     /* the wanted sheet is done: only the names of the later sheets are wanted */
            const char *q = memmem(x.p, (size_t)(x.end - x.p), "<table:table ", 13);
            if (!q)
                break;
            x.p = q;
        }
        if (xml_next(&x, &e) == XE_END)
            break;
        bool open = e.kind == XE_OPEN || e.kind == XE_SELF;
        if (e.kind == XE_TEXT) {
            if (in_cell && in_p && !skip)
                ods_text(&text, &tmp, e.text, e.tlen);
            continue;
        }
        if (is(&e, "annotation") || is(&e, "frame") || is(&e, "note")) {          /* a comment, a picture, a footnote: not the cell's text */
            if (e.kind == XE_OPEN)
                skip++;
            else if (e.kind == XE_CLOSE && skip)
                skip--;
            continue;
        }
        if (skip)
            continue;
        if (is(&e, "style") && open) {                                  /* automatic styles: which table style hides a sheet */
            cur_tstyle[0] = 0;
            if (e.kind == XE_OPEN && xattr(&e, "family", &a) && !strcmp(xb_str(&a), "table") && xattr(&e, "name", &a))
                snprintf(cur_tstyle, sizeof(cur_tstyle), "%s", xb_str(&a));
        } else if (is(&e, "style") && e.kind == XE_CLOSE) {
            cur_tstyle[0] = 0;
        } else if (is(&e, "table-properties") && open && cur_tstyle[0] && nhid < ODS_STYLES) {
            if (xattr(&e, "display", &a) && !strcmp(xb_str(&a), "false"))
                snprintf(hid_style[nhid++], sizeof(hid_style[0]), "%s", cur_tstyle);
        } else if (is(&e, "table")) {
            if (e.kind == XE_CLOSE) {
                if (depth_t > 0 && --depth_t == 0 && in_sel) {
                    in_sel = false;
                    o->sel_done = true;
                    fast = true;
                }
                continue;
            }
            if (depth_t > 0) {                                          /* a table inside a table: not a sheet */
                if (e.kind == XE_OPEN)
                    depth_t++;
                continue;
            }
            if (o->nt < MAX_SHEETS) {
                bool hidden = false;
                if (xattr(&e, "style-name", &a))
                    for (int i = 0; i < nhid; i++)
                        if (!strcmp(hid_style[i], xb_str(&a)))
                            hidden = true;
                o->name[o->nt] = xattr(&e, "name", &a) ? cell_text(xb_str(&a)) : strdup("");
                o->hidden[o->nt] = hidden;
                o->nt++;
                if (!hidden || allow_hidden) {
                    if (o->vis == sel && !o->sel_found && !o->sel_done) {
                        o->sel_found = true;
                        in_sel = e.kind == XE_OPEN;
                        row = 1;
                        if (e.kind == XE_SELF) {
                            o->sel_done = true;
                            fast = true;
                        }
                    }
                    o->vis++;
                }
            }
            if (e.kind == XE_OPEN)
                depth_t = fast ? 0 : 1;                                 /* (in the names-only mode no closing tags are looked at) */
        } else if (!in_sel) {
            continue;
        } else if (is(&e, "table-row")) {
            if (open) {
                nrr = xattr(&e, "number-rows-repeated", &a) ? atoi(xb_str(&a)) : 1;
                nrr = nrr < 1 ? 1 : nrr > 1048576 ? 1048576 : nrr;
                for (int i = 0; i < nrc; i++)
                    free(rc[i].s);                              /* a row that was never closed */
                in_row = true;
                col = 0;
                nrc = 0;
            }
            if (e.kind == XE_CLOSE || e.kind == XE_SELF) {
                if (in_row && nrc > 0) {
                    for (int k = 0; k < nrr && !gb.stop; k++)
                        for (int i = 0; i < nrc; i++)
                            gb_cell(&gb, row + k, rc[i].col, strdup(rc[i].s), -1);
                }
                for (int i = 0; i < nrc; i++)
                    free(rc[i].s);
                nrc = 0;
                if (in_row)
                    row += nrr;
                in_row = false;
                if (gb.stop) {                                          /* enough rows: the rest of the file only has names to give */
                    in_sel = false;
                    o->sel_done = true;
                    fast = true;
                    depth_t = 0;
                }
            }
        } else if ((is(&e, "table-cell") || is(&e, "covered-table-cell")) && in_row) {
            if (open) {
                covered = is(&e, "covered-table-cell");
                nrep = xattr(&e, "number-columns-repeated", &a) ? atoi(xb_str(&a)) : 1;
                nrep = nrep < 1 ? 1 : nrep > ODS_MAX_REPEAT ? ODS_MAX_REPEAT : nrep;
                xb_reset(&vt);
                xb_reset(&val);
                xb_reset(&date);
                xb_reset(&tim);
                xb_reset(&boolv);
                xb_reset(&str);
                xb_reset(&formula);
                xb_reset(&text);
                if (!covered) {
                    if (xattr(&e, "value-type", &a))
                        xb_add(&vt, xb_str(&a), a.n);
                    if (xattr(&e, "value", &a))
                        xb_add(&val, xb_str(&a), a.n);
                    if (xattr(&e, "date-value", &a))
                        xb_add(&date, xb_str(&a), a.n);
                    if (xattr(&e, "time-value", &a))
                        xb_add(&tim, xb_str(&a), a.n);
                    if (xattr(&e, "boolean-value", &a))
                        xb_add(&boolv, xb_str(&a), a.n);
                    if (xattr(&e, "string-value", &a))
                        xb_add(&str, xb_str(&a), a.n);
                    if (xattr(&e, "formula", &a))
                        xb_add(&formula, xb_str(&a), a.n);
                }
                in_cell = e.kind == XE_OPEN;
                in_p = false;
            }
            if (e.kind == XE_CLOSE || e.kind == XE_SELF) {
                char *shown = NULL;
                if (!covered) {
                    if (text.n)
                        shown = cell_text(xb_str(&text));
                    else
                        shown = ods_fallback(xb_str(&vt), xb_str(&val), xb_str(&date), xb_str(&tim), xb_str(&boolv), xb_str(&str), xb_str(&formula));
                }
                if (shown && shown[0]) {
                    for (int k = 0; k < nrep; k++) {
                        if (col + k >= max_cols) {
                            g->more_cols = true;
                            break;
                        }
                        if (nrc == caprc) {
                            caprc = caprc ? caprc * 2 : 16;
                            rc = realloc(rc, (size_t)caprc * sizeof(*rc));
                        }
                        rc[nrc].col = col + k;
                        rc[nrc++].s = strdup(shown);
                    }
                }
                free(shown);
                col += nrep;
                in_cell = in_p = false;
            }
        } else if (in_cell) {
            if (is(&e, "p") || is(&e, "h")) {
                if (e.kind == XE_OPEN) {
                    if (text.n && text.s[text.n - 1] != ' ')
                        xb_add(&text, " ", 1);
                    in_p = true;
                } else {
                    in_p = false;
                }
            } else if (in_p && is(&e, "s") && open) {
                int n = xattr(&e, "c", &a) ? atoi(xb_str(&a)) : 1;
                for (int k = 0; k < n && k < 64; k++)
                    xb_add(&text, " ", 1);
            } else if (in_p && (is(&e, "tab") || is(&e, "line-break")) && open) {
                xb_add(&text, " ", 1);
            }
        }
    }
    gb_flush(&gb);
    if (gb.have)
        row_free(&gb.cur);
    for (int i = 0; i < nrc; i++)
        free(rc[i].s);
    free(rc);
    free(a.s);
    free(text.s);
    free(tmp.s);
    free(vt.s);
    free(val.s);
    free(date.s);
    free(tim.s);
    free(boolv.s);
    free(str.s);
    free(formula.s);
}

/* The content.xml read in stages (2 MB, 32 MB, 128 MB of it): enough when the wanted sheet is done and the part was read whole, or 32 MB were looked at. */
static int ods_scan(const struct zip *z, int sel, bool allow_hidden, int max_rows, int max_cols, struct ods_out *o, const char **why)
{
    for (size_t cap = PART_FIRST;; cap = cap == PART_FIRST ? PART_BIG : PART_HUGE) {
        size_t len;
        char *c = zip_read(z, "content.xml", cap, &len, why);
        if (!c)
            return -1;
        ods_parse(c, len, sel, allow_hidden, max_rows, max_cols, o);
        free(c);
        if (len < cap || (o->sel_done && cap >= PART_BIG) || cap >= PART_HUGE)
            return 0;
        ods_out_free(o);
    }
}

static int ods_read(const char *path, int sheet, int max_rows, int max_cols, struct xl_book *b, char *err, size_t errn)
{
    memset(b, 0, sizeof(*b));
    struct zip z;
    int zr = zip_open(&z, path, err, errn);
    if (zr < 0)
        return zr;
    size_t len;
    const char *why = NULL;
    int result = -2;
    struct ods_out o = { 0 };
    char *mt = zip_read(&z, "mimetype", 256, &len, &why);
    bool ok = mt && strstr(mt, "opendocument.spreadsheet");
    bool has_mt = mt != NULL;
    free(mt);
    if (!has_mt && zip_find(&z, "content.xml")) {                       /* no mimetype part: look at the content */
        char *c = zip_read(&z, "content.xml", 4096, &len, &why);
        ok = c && strstr(c, "office:spreadsheet");
        free(c);
    }
    if (!ok) {                                                          /* a text document, a presentation, or no OpenDocument file */
        zip_close(&z);
        return -1;
    }
    bool all = false;
    int sel = sheet < 0 ? 0 : sheet;
    if (ods_scan(&z, sel, false, max_rows, max_cols, &o, &why) < 0) {
        seterr(err, errn, why ? why : "The spreadsheet has no content.xml.");
        goto done;
    }
    int nvis = 0;
    for (int i = 0; i < o.nt; i++)
        nvis += !o.hidden[i];
    if (nvis == 0 && o.nt > 0) {                                        /* only hidden sheets: show them */
        all = true;
        ods_out_free(&o);
        if (ods_scan(&z, sel, true, max_rows, max_cols, &o, &why) < 0)
            goto done;
        nvis = o.nt;
    } else if (!o.sel_found && nvis > 0) {                              /* a sheet number past the last one: the last */
        sel = nvis - 1;
        ods_out_free(&o);
        if (ods_scan(&z, sel, false, max_rows, max_cols, &o, &why) < 0)
            goto done;
    }
    if (o.nt == 0) {
        seterr(err, errn, "A spreadsheet with no sheets.");
        goto done;
    }
    b->kind = "OpenDocument";
    b->nsheets = nvis;
    b->sheet_name = calloc((size_t)nvis, sizeof(char *));
    for (int i = 0, k = 0; i < o.nt && k < nvis; i++)
        if (all || !o.hidden[i])
            b->sheet_name[k++] = strdup(o.name[i]);
    b->sheet = sel < nvis ? sel : nvis - 1;
    b->more_rows = o.g.more_rows;
    b->more_cols = o.g.more_cols;
    fill_book(b, &o.g, NULL);
    result = 0;
done:
    if (result != 0)
        xl_free(b);
    ods_out_free(&o);
    zip_close(&z);
    return result;
}

/* ======================================================================================================================================================
 * Apple Numbers (.numbers, 2013 and later): a ZIP whose Index/ *.iwa parts are Snappy-compressed streams of protobuf messages ("objects" with an
 * identifier, a type number and a list of the identifiers they refer to). Apple publishes no description of it; this reader works from what is known of the
 * format and checks what it finds by SHAPE as much as by field number (a string list is a message with repeated entries that have a number and a string,
 * a tile is a message with rows that carry two byte strings ...), and `--numbers-dump` of mellagui-files prints the whole structure of a file for work on
 * a real one. Not read: Numbers '09 (index.xml), rich text cells, cell formats (a percentage shows as its number), formulas (their cached values show).
 * ====================================================================================================================================================== */

#define NB_MAX_OBJECTS 600000
#define NB_PART_MAX (128u << 20)
#define NB_TOTAL_MAX (320u << 20)
#define NB_TILE_ROWS 256                        /* rows in one tile */

struct pbr {
    const uint8_t *p, *end;
};

struct pbf {
    uint32_t num;
    int wt;
    uint64_t iv;
    const uint8_t *d;
    size_t dl;
};

static bool pb_varint(struct pbr *r, uint64_t *v)
{
    uint64_t x = 0;
    for (int sh = 0; r->p < r->end && sh < 70; sh += 7) {
        uint8_t b = *r->p++;
        x |= (uint64_t)(b & 0x7f) << sh;
        if (!(b & 0x80)) {
            *v = x;
            return true;
        }
    }
    return false;
}

static bool pb_next(struct pbr *r, struct pbf *f)
{
    uint64_t key;
    if (r->p >= r->end || !pb_varint(r, &key))
        return false;
    f->num = (uint32_t)(key >> 3);
    f->wt = (int)(key & 7);
    f->iv = 0;
    f->d = NULL;
    f->dl = 0;
    switch (f->wt) {
    case 0:
        return pb_varint(r, &f->iv);
    case 1:
        if (r->end - r->p < 8)
            return false;
        memcpy(&f->iv, r->p, 8);
        r->p += 8;
        return true;
    case 5: {
        if (r->end - r->p < 4)
            return false;
        uint32_t v;
        memcpy(&v, r->p, 4);
        f->iv = v;
        r->p += 4;
        return true;
    }
    case 2: {
        uint64_t l;
        if (!pb_varint(r, &l) || l > (uint64_t)(r->end - r->p))
            return false;
        f->d = r->p;
        f->dl = (size_t)l;
        r->p += l;
        return true;
    }
    default:
        return false;
    }
}

struct nobj {
    uint64_t id;
    uint32_t type;
    const uint8_t *msg;
    size_t len;
    uint64_t *refs;
    uint32_t nrefs;
};

struct nbook {
    struct nobj *o;
    int n, cap;
    uint8_t **bufs;                     /* the decoded streams the objects point into */
    int nbuf;
    size_t total;
    int *idx;                           /* the objects by identifier */
    bool truncated;
};

/* An .iwa part: chunks of a 0 byte, a 3 byte length and raw Snappy data, one after the other. Returns the concatenated plain bytes. */
static uint8_t *iwa_decode(const uint8_t *in, size_t n, size_t *outn)
{
    size_t cap = 1 << 16, o = 0;
    uint8_t *out = malloc(cap);
    size_t i = 0;
    while (out && i + 4 <= n && in[i] == 0) {
        size_t len = in[i + 1] | (size_t)in[i + 2] << 8 | (size_t)in[i + 3] << 16;
        i += 4;
        if (len > n - i || len == 0)
            break;
        struct pbr vr = { in + i, in + i + len };
        uint64_t ulen;
        if (!pb_varint(&vr, &ulen) || ulen > (1u << 24) || o + ulen > NB_PART_MAX)
            break;
        if (o + ulen + 1 > cap) {
            while (o + ulen + 1 > cap)
                cap *= 2;
            uint8_t *t = realloc(out, cap);
            if (!t)
                break;
            out = t;
        }
        if (pq_snappy(in + i, len, out + o, (size_t)ulen) < 0)
            break;
        o += (size_t)ulen;
        i += len;
    }
    if (!out)
        return NULL;
    *outn = o;
    return out;
}

static void nb_add(struct nbook *nb, uint64_t id, uint32_t type, const uint8_t *msg, size_t len, uint64_t *refs, uint32_t nrefs)
{
    if (nb->n >= NB_MAX_OBJECTS) {
        free(refs);
        nb->truncated = true;
        return;
    }
    if (nb->n == nb->cap) {
        nb->cap = nb->cap ? nb->cap * 2 : 1024;
        nb->o = realloc(nb->o, (size_t)nb->cap * sizeof(*nb->o));
    }
    nb->o[nb->n++] = (struct nobj){ id, type, msg, len, refs, nrefs };
}

/* The objects of a decoded stream: a varint length, an ArchiveInfo (identifier, MessageInfos with type, length and the identifiers referred to), then the
 * messages themselves one after another. */
static void nb_stream(struct nbook *nb, const uint8_t *s, size_t n)
{
    struct pbr r = { s, s + n };
    while (r.p < r.end) {
        uint64_t alen;
        if (!pb_varint(&r, &alen) || alen > (uint64_t)(r.end - r.p))
            return;
        struct pbr ar = { r.p, r.p + alen };
        r.p += alen;
        uint64_t id = 0;
        struct { uint32_t type; uint64_t len; uint64_t *refs; uint32_t nrefs, caprefs; } mi[8];
        int nmi = 0;
        struct pbf f;
        while (pb_next(&ar, &f)) {
            if (f.num == 1 && f.wt == 0) {
                id = f.iv;
            } else if (f.num == 2 && f.wt == 2 && nmi < 8) {
                memset(&mi[nmi], 0, sizeof(mi[0]));
                struct pbr mr = { f.d, f.d + f.dl };
                struct pbf g;
                while (pb_next(&mr, &g)) {
                    if (g.num == 1 && g.wt == 0) {
                        mi[nmi].type = (uint32_t)g.iv;
                    } else if (g.num == 3 && g.wt == 0) {
                        mi[nmi].len = g.iv;
                    } else if (g.num == 5) {
                        struct pbr pr = { g.wt == 2 ? g.d : NULL, g.wt == 2 ? g.d + g.dl : NULL };
                        uint64_t v = g.iv;
                        bool more = g.wt == 2;
                        while (true) {
                            if (more && !pb_varint(&pr, &v))
                                break;
                            if (mi[nmi].nrefs == mi[nmi].caprefs) {
                                mi[nmi].caprefs = mi[nmi].caprefs ? mi[nmi].caprefs * 2 : 8;
                                mi[nmi].refs = realloc(mi[nmi].refs, mi[nmi].caprefs * sizeof(uint64_t));
                            }
                            mi[nmi].refs[mi[nmi].nrefs++] = v;
                            if (!more)
                                break;
                        }
                    }
                }
                nmi++;
            }
        }
        for (int k = 0; k < nmi; k++) {
            if (mi[k].len > (uint64_t)(r.end - r.p)) {
                for (int j = k; j < nmi; j++)
                    free(mi[j].refs);
                return;
            }
            nb_add(nb, id, mi[k].type, r.p, (size_t)mi[k].len, mi[k].refs, mi[k].nrefs);
            r.p += mi[k].len;
        }
    }
}

static int nb_cmp(const void *a, const void *b, void *ctx)
{
    const struct nobj *o = ctx;
    const struct nobj *x = &o[*(const int *)a], *y = &o[*(const int *)b];
    return x->id < y->id ? -1 : x->id > y->id ? 1 : *(const int *)a - *(const int *)b;
}

static const struct nobj *nb_find(const struct nbook *nb, uint64_t id)
{
    int lo = 0, hi = nb->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint64_t v = nb->o[nb->idx[mid]].id;
        if (v < id)
            lo = mid + 1;
        else if (v > id)
            hi = mid - 1;
        else {
            while (mid > 0 && nb->o[nb->idx[mid - 1]].id == id)
                mid--;
            return &nb->o[nb->idx[mid]];
        }
    }
    return NULL;
}

static void nb_free(struct nbook *nb)
{
    for (int i = 0; i < nb->n; i++)
        free(nb->o[i].refs);
    free(nb->o);
    for (int i = 0; i < nb->nbuf; i++)
        free(nb->bufs[i]);
    free(nb->bufs);
    free(nb->idx);
    memset(nb, 0, sizeof(*nb));
}

/* Reads every .iwa part under Index/ of the package. 0 = ok. */
static int nb_load(struct nbook *nb, const struct zip *z)
{
    for (int i = 0; i < z->n; i++) {
        const char *nm = z->e[i].name;
        size_t l = strlen(nm);
        if (l < 4 || strcmp(nm + l - 4, ".iwa") || strncmp(nm, "Index/", 6))
            continue;
        size_t len;
        const char *why;
        char *raw = zip_read(z, nm, NB_PART_MAX, &len, &why);
        if (!raw)
            continue;
        size_t pn = 0;
        uint8_t *plain = iwa_decode((const uint8_t *)raw, len, &pn);
        free(raw);
        if (!plain)
            continue;
        nb->total += pn;
        nb->bufs = realloc(nb->bufs, (size_t)(nb->nbuf + 1) * sizeof(uint8_t *));
        nb->bufs[nb->nbuf++] = plain;
        nb_stream(nb, plain, pn);
        if (nb->total > NB_TOTAL_MAX) {
            nb->truncated = true;
            break;
        }
    }
    if (nb->n == 0)
        return -1;
    nb->idx = malloc((size_t)nb->n * sizeof(int));
    for (int i = 0; i < nb->n; i++)
        nb->idx[i] = i;
    qsort_r(nb->idx, (size_t)nb->n, sizeof(int), nb_cmp, nb->o);
    return 0;
}

/* ---- looking into messages ---- */
static bool utf8_text(const uint8_t *p, size_t n)
{
    if (n == 0 || n > 400)
        return false;
    for (size_t i = 0; i < n;) {
        uint8_t c = p[i];
        size_t k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!k || i + k > n || (k == 1 && c < 0x20))
            return false;
        for (size_t j = 1; j < k; j++)
            if ((p[i + j] & 0xc0) != 0x80)
                return false;
        i += k;
    }
    return true;
}

static bool looks_like_uuid(const uint8_t *p, size_t n)
{
    if (n != 36)
        return false;
    for (size_t i = 0; i < n; i++)
        if (i == 8 || i == 13 || i == 18 || i == 23 ? p[i] != '-' : !((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f') || (p[i] >= 'A' && p[i] <= 'F')))
            return false;
    return true;
}

/* The name in a message: the string of field `prefer` when it reads as text, else the first text field that is no UUID. */
static char *nb_name(const struct nobj *o, uint32_t prefer)
{
    struct pbr r = { o->msg, o->msg + o->len };
    struct pbf f;
    const uint8_t *best = NULL;
    size_t bl = 0;
    while (pb_next(&r, &f)) {
        if (f.wt != 2 || !utf8_text(f.d, f.dl) || looks_like_uuid(f.d, f.dl))
            continue;
        if (f.num == prefer) {
            best = f.d;
            bl = f.dl;
            break;
        }
        if (!best) {
            best = f.d;
            bl = f.dl;
        }
    }
    if (!best)
        return NULL;
    char *s = strndup((const char *)best, bl);
    char *c = s ? cell_text(s) : NULL;
    free(s);
    return c;
}

/* A string list (a TableDataList): repeated entries with a key (field 1) and a string (field 3). Returns the strings by key, or NULL. */
static char **nb_string_list(const struct nobj *o, uint32_t *nkeys, int *listtype)
{
    struct pbr r = { o->msg, o->msg + o->len };
    struct pbf f;
    uint64_t maxkey = 0;
    int count = 0;
    *listtype = 0;
    while (pb_next(&r, &f)) {
        if (f.num == 1 && f.wt == 0)
            *listtype = (int)f.iv;
        if (f.num != 3 || f.wt != 2)
            continue;
        struct pbr er = { f.d, f.d + f.dl };
        struct pbf g;
        uint64_t key = 0;
        bool has_key = false, has_str = false;
        while (pb_next(&er, &g)) {
            if (g.num == 1 && g.wt == 0) {
                key = g.iv;
                has_key = true;
            } else if (g.num == 3 && g.wt == 2) {
                has_str = true;
            }
        }
        if (has_key && has_str) {
            count++;
            if (key > maxkey)
                maxkey = key;
        }
    }
    if (!count || maxkey > 4000000)
        return NULL;
    char **tab = calloc((size_t)maxkey + 1, sizeof(char *));
    r = (struct pbr){ o->msg, o->msg + o->len };
    while (pb_next(&r, &f)) {
        if (f.num != 3 || f.wt != 2)
            continue;
        struct pbr er = { f.d, f.d + f.dl };
        struct pbf g;
        uint64_t key = 0;
        const uint8_t *sp = NULL;
        size_t sl = 0;
        while (pb_next(&er, &g)) {
            if (g.num == 1 && g.wt == 0) {
                key = g.iv;
            } else if (g.num == 3 && g.wt == 2) {
                sp = g.d;
                sl = g.dl;
            }
        }
        if (sp && key <= maxkey && !tab[key]) {
            char *s = strndup((const char *)sp, sl);
            tab[key] = s ? cell_text(s) : NULL;
            free(s);
        }
    }
    *nkeys = (uint32_t)maxkey + 1;
    return tab;
}

/* A tile: a message that has rows (sub-messages with a number and two byte strings). */
static bool nb_is_tile(const struct nobj *o)
{
    struct pbr r = { o->msg, o->msg + o->len };
    struct pbf f;
    while (pb_next(&r, &f)) {
        if (f.wt != 2 || f.dl < 4)
            continue;
        struct pbr er = { f.d, f.d + f.dl };
        struct pbf g;
        int bytes = 0, varints = 0;
        while (pb_next(&er, &g)) {
            bytes += g.wt == 2;
            varints += g.wt == 0;
        }
        if (bytes >= 2 && varints >= 1)
            return true;
    }
    return false;
}

/* Which tile number each tile object has: somewhere in the table's message there are entries {number (field 1), a reference (field 2: its field 1 is the
 * identifier of the tile)}. */
#define NB_MAX_TILES 512
struct tilemap {
    uint64_t id[NB_MAX_TILES];
    uint32_t num[NB_MAX_TILES];
    int n;
};

static void nb_tile_numbers(const struct nbook *nb, const uint8_t *msg, size_t len, int depth, struct tilemap *m)
{
    struct pbr r = { msg, msg + len };
    struct pbf f;
    uint64_t first = 0;
    bool have_first = false;
    while (pb_next(&r, &f)) {
        if (f.num == 1 && f.wt == 0) {
            first = f.iv;
            have_first = true;
        } else if (f.wt == 2 && f.dl > 0) {
            if (f.num == 2 && have_first && m->n < NB_MAX_TILES) {
                struct pbr er = { f.d, f.d + f.dl };
                struct pbf g;
                if (pb_next(&er, &g) && g.num == 1 && g.wt == 0) {
                    const struct nobj *t = nb_find(nb, g.iv);
                    if (t && nb_is_tile(t)) {
                        m->id[m->n] = g.iv;
                        m->num[m->n++] = (uint32_t)first;
                    }
                }
            }
            if (depth < 5)
                nb_tile_numbers(nb, f.d, f.dl, depth + 1, m);
        }
    }
}

/* ---- the cells of a tile ---- */
static double decimal128(const uint8_t *b)
{
    int exp = ((b[15] & 0x7f) << 7 | b[14] >> 1) - 0x1820;
    long double m = b[14] & 1;
    for (int i = 13; i >= 0; i--)
        m = m * 256 + b[i];
    long double v = m * powl(10.0L, (long double)exp);
    return (double)((b[15] & 0x80) ? -v : v);
}

struct nrowinfo {
    uint32_t row;
    const uint8_t *buf, *offs;
    size_t blen, olen;
};

static int nb_rowinfo_cmp(const void *a, const void *b)
{
    const struct nrowinfo *x = a, *y = b;
    return x->row < y->row ? -1 : x->row > y->row;
}

/* Cell number `col` of a row: its start in the buffer, or -1 for none. `mult` is the unit of the offsets. */
static long cell_start(const struct nrowinfo *ri, size_t col, unsigned mult)
{
    if (col * 2 + 2 > ri->olen)
        return -1;
    unsigned off = u16(ri->offs + col * 2);
    if (off == 0xffff)
        return -1;
    size_t s = (size_t)off * mult;
    return s + 12 <= ri->blen ? (long)s : -1;
}

/* How the offsets count: the unit (4 or 1 byte) for which most cells start inside the buffer with a storage version byte of 3..5. */
static unsigned nb_offset_unit(const struct nrowinfo *ri, size_t n)
{
    static const unsigned units[2] = { 4, 1 };
    unsigned best = 4;
    int bestscore = -1;
    for (int ui = 0; ui < 2; ui++) {
        unsigned mult = units[ui];
        int ok = 0, all = 0;
        for (size_t i = 0; i < n; i++)
            for (size_t c = 0; c * 2 + 2 <= ri[i].olen && c < 64; c++) {
                if (u16(ri[i].offs + c * 2) == 0xffff)
                    continue;
                all++;                                                  /* a cell that starts outside the buffer counts against this unit */
                long st = cell_start(&ri[i], c, mult);
                ok += st >= 0 && ri[i].buf[st] >= 3 && ri[i].buf[st] <= 5;
            }
        int score = all ? ok * 100 / all : 0;
        if (score > bestscore) {
            bestscore = score;
            best = mult;
        }
    }
    return best;
}

static void nb_date_text(double secs, char *o, size_t n)
{
    double days = floor(secs / 86400);
    long sod = (long)floor(secs - days * 86400 + 0.5);
    if (sod >= 86400) {
        sod -= 86400;
        days += 1;
    }
    int y, m, d;
    civil_from_days((long)days + 11323, &y, &m, &d);                    /* Apple counts seconds from 2001-01-01 */
    if (sod == 0)
        snprintf(o, n, "%04d-%02d-%02d", y, m, d);
    else
        snprintf(o, n, "%04d-%02d-%02d %02d:%02d:%02d", y, m, d, (int)(sod / 3600), (int)(sod / 60 % 60), (int)(sod % 60));
}

static char *nb_cell_text(const uint8_t *c, size_t avail, char **strings, uint32_t nstr)
{
    unsigned type = c[1];
    uint32_t flags = u32(c + 8);
    size_t p = 12;
    char out[96] = "";
    bool have = false;
    double num = 0;
    if (flags & 0x1) {
        if (p + 16 > avail)
            return NULL;
        num = decimal128(c + p);
        p += 16;
        have = true;
    }
    if (flags & 0x2) {
        if (p + 8 > avail)
            return NULL;
        memcpy(&num, c + p, 8);
        p += 8;
        have = true;
    }
    if (flags & 0x4) {
        if (p + 8 > avail)
            return NULL;
        double secs;
        memcpy(&secs, c + p, 8);
        p += 8;
        nb_date_text(secs, out, sizeof(out));
        return cell_text(out);
    }
    if (flags & 0x8) {
        if (p + 4 > avail)
            return NULL;
        uint32_t id = u32(c + p);
        return id < nstr && strings[id] ? strdup(strings[id]) : strdup("");
    }
    if (flags & 0x10)
        return strdup("(rich text)");
    if (!have)
        return NULL;
    if (type == 6 && (num == 0 || num == 1))
        return strdup(num ? "TRUE" : "FALSE");
    fmt_double(num, K_PLAIN, false, out, sizeof(out));
    return cell_text(out);
}

/* All the cells of one table into the grid. */
static bool nb_table(const struct nbook *nb, const struct nobj *model, int max_rows, int max_cols, struct grid *g)
{
    char **strings = NULL;
    uint32_t nstr = 0;
    int best_type = -1;
    struct tilemap tm = { .n = 0 };
    const struct nobj *tiles[NB_MAX_TILES];
    int ntiles = 0;
    for (uint32_t i = 0; i < model->nrefs; i++) {
        const struct nobj *t = nb_find(nb, model->refs[i]);
        if (!t)
            continue;
        if (nb_is_tile(t)) {
            if (ntiles < NB_MAX_TILES)
                tiles[ntiles++] = t;
            continue;
        }
        uint32_t nk = 0;
        int lt = 0;
        char **tab = nb_string_list(t, &nk, &lt);
        if (tab && (!strings || (lt == 1 && best_type != 1))) {          /* the list of strings; one marked as the strings' own wins */
            if (strings)
                for (uint32_t k = 0; k < nstr; k++)
                    free(strings[k]);
            free(strings);
            strings = tab;
            nstr = nk;
            best_type = lt;
        } else if (tab) {
            for (uint32_t k = 0; k < nk; k++)
                free(tab[k]);
            free(tab);
        }
    }
    nb_tile_numbers(nb, model->msg, model->len, 0, &tm);
    struct gb b = { .g = g, .max_rows = max_rows, .max_cols = max_cols };
    g->maxsst = -1;
    g->maxcol = -1;
    /* tiles in the order of their numbers */
    int order[NB_MAX_TILES];
    uint32_t tnum[NB_MAX_TILES];
    for (int i = 0; i < ntiles; i++) {
        order[i] = i;
        tnum[i] = (uint32_t)i;
        for (int k = 0; k < tm.n; k++)
            if (tm.id[k] == tiles[i]->id)
                tnum[i] = tm.num[k];
    }
    for (int i = 1; i < ntiles; i++)
        for (int j = i; j > 0 && tnum[order[j]] < tnum[order[j - 1]]; j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    bool any = false;
    for (int ti = 0; ti < ntiles && !b.stop; ti++) {
        const struct nobj *tile = tiles[order[ti]];
        struct nrowinfo *rows = NULL;
        size_t nrows = 0, caprows = 0;
        struct pbr r = { tile->msg, tile->msg + tile->len };
        struct pbf f;
        while (pb_next(&r, &f)) {
            if (f.wt != 2)
                continue;
            struct pbr er = { f.d, f.d + f.dl };
            struct pbf h;
            struct nrowinfo ri = { 0 };
            const uint8_t *bytes[4] = { 0 };
            size_t blens[4] = { 0 };
            int nbytes = 0;
            bool has_row = false;
            while (pb_next(&er, &h)) {
                if (h.num == 1 && h.wt == 0 && !has_row) {
                    ri.row = (uint32_t)h.iv;
                    has_row = true;
                } else if (h.wt == 2 && nbytes < 4 && (h.num == 3 || h.num == 4 || nbytes < 2)) {
                    bytes[nbytes] = h.d;
                    blens[nbytes++] = h.dl;
                }
            }
            if (!has_row || nbytes < 2)
                continue;
            ri.buf = bytes[0];                                          /* the cell buffer (field 3) and the offsets (field 4) */
            ri.blen = blens[0];
            ri.offs = bytes[1];
            ri.olen = blens[1];
            if (ri.olen % 2 || (ri.blen % 2 == 0 && ri.blen < ri.olen)) {         /* the other way round: the shorter, even one is the offsets */
                ri.buf = bytes[1];
                ri.blen = blens[1];
                ri.offs = bytes[0];
                ri.olen = blens[0];
            }
            if (nrows == caprows) {
                caprows = caprows ? caprows * 2 : 64;
                rows = realloc(rows, caprows * sizeof(*rows));
            }
            rows[nrows++] = ri;
        }
        if (nrows) {
            qsort(rows, nrows, sizeof(*rows), nb_rowinfo_cmp);
            unsigned mult = nb_offset_unit(rows, nrows);
            for (size_t i = 0; i < nrows && !b.stop; i++) {
                for (size_t c = 0; c * 2 + 2 <= rows[i].olen && c < 1024 && !b.stop; c++) {
                    long st = cell_start(&rows[i], c, mult);
                    if (st < 0 || rows[i].buf[st] < 3 || rows[i].buf[st] > 5)
                        continue;
                    char *t = nb_cell_text(rows[i].buf + st, rows[i].blen - (size_t)st, strings, nstr);
                    if (t && t[0])
                        any = true;
                    {
                        uint64_t rn = (uint64_t)tnum[order[ti]] * NB_TILE_ROWS + rows[i].row + 1;
                        gb_cell(&b, rn > 0x3fffffff ? 0x3fffffff : (int)rn, (int)c, t, -1);
                    }
                }
            }
        }
        free(rows);
    }
    gb_flush(&b);
    if (b.have)
        row_free(&b.cur);
    if (strings)
        for (uint32_t k = 0; k < nstr; k++)
            free(strings[k]);
    free(strings);
    return any || ntiles > 0;
}

static void nb_add_tab(char ***names, const struct nobj ***models, int *n, int *cap, char *name, const struct nobj *model)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *names = realloc(*names, (size_t)*cap * sizeof(char *));
        *models = realloc(*models, (size_t)*cap * sizeof(**models));
    }
    (*names)[*n] = name;
    (*models)[*n] = model;
    (*n)++;
}

static int numbers_read(const char *path, int sheet, int max_rows, int max_cols, struct xl_book *b, char *err, size_t errn)
{
    memset(b, 0, sizeof(*b));
    struct zip z;
    int zr = zip_open(&z, path, err, errn);
    if (zr < 0)
        return zr;
    bool iwa = false;
    for (int i = 0; i < z.n; i++)
        if (!strncmp(z.e[i].name, "Index/", 6) && strstr(z.e[i].name, ".iwa"))
            iwa = true;
    if (!iwa) {
        zip_close(&z);
        return -1;
    }
    struct nbook nb = { 0 };
    char **names = NULL;
    const struct nobj **models = NULL;
    int ntab = 0, captab = 0, result = -2;
    if (nb_load(&nb, &z) < 0) {
        seterr(err, errn, "A Numbers file whose parts could not be unpacked.");
        goto done;
    }
    const struct nobj *doc = NULL;
    for (int i = 0; i < nb.n && !doc; i++)
        if (nb.o[i].type == 1)
            doc = &nb.o[i];
    const struct nobj **sheets = calloc((size_t)(nb.n + 1), sizeof(*sheets));
    int nsheets = 0;
    if (doc)
        for (uint32_t i = 0; i < doc->nrefs; i++) {
            const struct nobj *s = nb_find(&nb, doc->refs[i]);
            bool dup = false;
            for (int k = 0; k < nsheets; k++)
                dup |= sheets[k] == s;
            if (s && s->type == 2 && !dup)
                sheets[nsheets++] = s;
        }
    if (!nsheets)
        for (int i = 0; i < nb.n; i++)
            if (nb.o[i].type == 2)
                sheets[nsheets++] = &nb.o[i];
    for (int si = 0; si < nsheets; si++) {
        const struct nobj *models_here[16];
        int nm = 0;
        for (uint32_t i = 0; i < sheets[si]->nrefs && nm < 16; i++) {
            const struct nobj *t = nb_find(&nb, sheets[si]->refs[i]);
            if (!t)
                continue;
            if (t->type == 6001) {
                models_here[nm++] = t;
            } else if (t->type == 6000) {
                for (uint32_t k = 0; k < t->nrefs && nm < 16; k++) {
                    const struct nobj *m = nb_find(&nb, t->refs[k]);
                    if (m && m->type == 6001)
                        models_here[nm++] = m;
                }
            }
        }
        char *sname = nb_name(sheets[si], 1);
        for (int k = 0; k < nm; k++) {
            char label[260];
            char *tname = nm > 1 ? nb_name(models_here[k], 4) : NULL;
            snprintf(label, sizeof(label), "%s%s%s", sname ? sname : "Sheet", tname ? " / " : "", tname ? tname : "");
            free(tname);
            nb_add_tab(&names, &models, &ntab, &captab, cell_text(label), models_here[k]);
        }
        free(sname);
    }
    free(sheets);
    if (!ntab)                                                          /* the links were not understood: every table there is */
        for (int i = 0; i < nb.n; i++)
            if (nb.o[i].type == 6001) {
                char *tn = nb_name(&nb.o[i], 4);
                char label[64];
                snprintf(label, sizeof(label), "Table %d", ntab + 1);
                nb_add_tab(&names, &models, &ntab, &captab, tn ? tn : cell_text(label), &nb.o[i]);
            }
    if (!ntab) {
        char msg[200];
        snprintf(msg, sizeof(msg), "A Numbers file in which no table was recognised (%d objects).", nb.n);
        seterr(err, errn, msg);
        goto done;
    }
    b->kind = "Numbers";
    b->nsheets = ntab;
    b->sheet_name = names;
    names = NULL;
    b->sheet = sheet < 0 ? 0 : sheet >= ntab ? ntab - 1 : sheet;
    struct grid g = { 0 };
    nb_table(&nb, models[b->sheet], max_rows, max_cols, &g);
    b->more_rows = g.more_rows;
    b->more_cols = g.more_cols;
    fill_book(b, &g, NULL);
    for (int r = 0; r < g.nrow; r++)
        row_free(&g.row[r]);
    free(g.row);
    result = 0;
done:
    if (names)
        for (int i = 0; i < ntab; i++)
            free(names[i]);
    free(names);
    free(models);
    if (result != 0)
        xl_free(b);
    nb_free(&nb);
    zip_close(&z);
    return result;
}

/* The picture the package carries of itself (Numbers writes a JPEG of the first sheet): the bytes, or NULL. */
unsigned char *xl_thumbnail(const char *path, size_t *len)
{
    struct zip z;
    if (zip_open(&z, path, NULL, 0) < 0)
        return NULL;
    static const char *const names[] = { "preview.jpg", "QuickLook/Thumbnail.jpg", "preview-web.jpg", "preview-micro.jpg", "QuickLook/Thumbnail.png" };
    unsigned char *out = NULL;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !out; i++) {
        const char *why;
        out = (unsigned char *)zip_read(&z, names[i], 8u << 20, len, &why);
    }
    zip_close(&z);
    return out;
}

/* ---- --numbers-dump: the structure of a file as text, for work on a real one ---- */
static void nb_dump_msg(const uint8_t *msg, size_t len, int depth, int maxdepth)
{
    struct pbr r = { msg, msg + len };
    struct pbf f;
    while (pb_next(&r, &f)) {
        printf("%*s%u:", depth * 2 + 4, "", f.num);
        if (f.wt == 0) {
            printf(" %llu\n", (unsigned long long)f.iv);
        } else if (f.wt == 1 || f.wt == 5) {
            printf(" fixed%d 0x%llx\n", f.wt == 1 ? 64 : 32, (unsigned long long)f.iv);
        } else if (utf8_text(f.d, f.dl) && f.dl < 120) {
            printf(" \"%.*s\"\n", (int)f.dl, f.d);
        } else {
            struct pbr t = { f.d, f.d + f.dl };
            struct pbf g;
            bool msgish = f.dl > 1 && pb_next(&t, &g) && g.num > 0 && g.num < 64;
            if (msgish && depth < maxdepth) {
                printf(" {%zu bytes}\n", f.dl);
                nb_dump_msg(f.d, f.dl, depth + 1, maxdepth);
            } else {
                printf(" bytes[%zu]", f.dl);
                for (size_t i = 0; i < f.dl && i < 16; i++)
                    printf(" %02x", f.d[i]);
                printf("%s\n", f.dl > 16 ? " ..." : "");
            }
        }
    }
}

int xl_numbers_dump(const char *path)
{
    struct zip z;
    if (zip_open(&z, path, NULL, 0) < 0) {
        fprintf(stderr, "not a ZIP file\n");
        return 1;
    }
    for (int i = 0; i < z.n; i++)
        printf("part %s (method %u, %u -> %u bytes)\n", z.e[i].name, z.e[i].method, z.e[i].csize, z.e[i].usize);
    struct nbook nb = { 0 };
    if (nb_load(&nb, &z) < 0) {
        printf("no objects\n");
        zip_close(&z);
        return 1;
    }
    printf("%d objects\n", nb.n);
    for (int i = 0; i < nb.n; i++) {
        const struct nobj *o = &nb.o[i];
        printf("object id=%llu type=%u len=%zu refs=", (unsigned long long)o->id, o->type, o->len);
        for (uint32_t k = 0; k < o->nrefs && k < 12; k++)
            printf("%s%llu", k ? "," : "", (unsigned long long)o->refs[k]);
        printf("%s\n", o->nrefs > 12 ? ",..." : "");
        if (o->type == 1 || o->type == 2 || o->type == 6000 || o->type == 6001 || nb_is_tile(o) || i < 0)
            nb_dump_msg(o->msg, o->len > 4000 ? 4000 : o->len, 0, 4);
    }
    nb_free(&nb);
    zip_close(&z);
    return 0;
}

/* ======================================================================================================================================================
 * Documents and presentations (.docx, .odt, .pptx, .odp and their templates) as Markdown text: headings, paragraphs, bold and emphasis, lists, tables, slides.
 * ====================================================================================================================================================== */

#define DOC_OUT_MAX (2u << 20)                  /* the Markdown made, at most */
#define DOC_PARA_MAX (64u << 10)                /* one paragraph, at most */

struct mdb {
    char *s;
    size_t n, cap;
};

static void mdb_add(struct mdb *b, const char *p, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        char *t = realloc(b->s, b->cap);
        if (!t)
            return;
        b->s = t;
    }
    memcpy(b->s + b->n, p, n);
    b->n += n;
    b->s[b->n] = 0;
}

struct drun {
    char *t;
    size_t n;
    bool b, i;
};

struct docb {
    struct mdb out;
    struct drun *runs;
    int nruns, caprun;
    size_t ptext;
    bool list_prev, list_bullet;
    int paragraphs, tables, slides;
    int tdepth;                         /* table nesting: only the outermost one makes rows and cells */
    struct mdb cell;
    char **cells;
    int ncells, capcells;
    bool first_row;
    bool full;                          /* the output limit was reached */
};

static void db_free(struct docb *db)
{
    for (int i = 0; i < db->nruns; i++)
        free(db->runs[i].t);
    free(db->runs);
    free(db->out.s);
    free(db->cell.s);
    for (int i = 0; i < db->ncells; i++)
        free(db->cells[i]);
    free(db->cells);
}

static void db_out(struct docb *db, const char *p, size_t n)
{
    if (db->out.n + n > DOC_OUT_MAX) {
        db->full = true;
        return;
    }
    mdb_add(&db->out, p, n);
}

static void db_outs(struct docb *db, const char *s)
{
    db_out(db, s, strlen(s));
}

/* A piece of the paragraph being read, with its emphasis; neighbours with the same emphasis are one piece. */
static void db_run(struct docb *db, const char *p, size_t n, bool b, bool i)
{
    if (!n || db->ptext > DOC_PARA_MAX)
        return;
    db->ptext += n;
    if (db->nruns > 0 && db->runs[db->nruns - 1].b == b && db->runs[db->nruns - 1].i == i) {
        struct drun *r = &db->runs[db->nruns - 1];
        r->t = realloc(r->t, r->n + n + 1);
        memcpy(r->t + r->n, p, n);
        r->n += n;
        r->t[r->n] = 0;
        return;
    }
    if (db->nruns == db->caprun) {
        db->caprun = db->caprun ? db->caprun * 2 : 16;
        db->runs = realloc(db->runs, (size_t)db->caprun * sizeof(*db->runs));
    }
    struct drun *r = &db->runs[db->nruns++];
    r->t = malloc(n + 1);
    memcpy(r->t, p, n);
    r->t[n] = 0;
    r->n = n;
    r->b = b;
    r->i = i;
}

/* The paragraph as Markdown text into `o`: emphasis marks round the words (the blanks stay outside), characters that Markdown reads as marks escaped. */
static void db_compose(struct docb *db, struct mdb *o)
{
    for (int k = 0; k < db->nruns; k++) {
        const struct drun *r = &db->runs[k];
        size_t a = 0, e = r->n;
        while (a < e && (r->t[a] == ' ' || r->t[a] == '\t' || r->t[a] == '\n'))
            a++;
        while (e > a && (r->t[e - 1] == ' ' || r->t[e - 1] == '\t' || r->t[e - 1] == '\n'))
            e--;
        if (a > 0 && o->n && o->s[o->n - 1] != ' ')
            mdb_add(o, " ", 1);
        if (a < e) {
            if (r->b)
                mdb_add(o, "**", 2);
            else if (r->i)
                mdb_add(o, "*", 1);
            if (r->b && r->i)
                mdb_add(o, "*", 1);
            for (size_t j = a; j < e; j++) {
                unsigned char c = (unsigned char)r->t[j];
                if (c == '\n' || c == '\t' || c == '\r')
                    c = ' ';
                if (strchr("\\`*_[]<>|~#", c))
                    mdb_add(o, "\\", 1);
                mdb_add(o, (char *)&c, 1);
            }
            if (r->b && r->i)
                mdb_add(o, "*", 1);
            if (r->b)
                mdb_add(o, "**", 2);
            else if (r->i)
                mdb_add(o, "*", 1);
        }
        if (e < r->n && a <= e)
            mdb_add(o, " ", 1);
    }
    for (int k = 0; k < db->nruns; k++)
        free(db->runs[k].t);
    db->nruns = 0;
    db->ptext = 0;
}

/* A paragraph that is not wanted (a slide number): forgotten. */
static void db_discard(struct docb *db)
{
    for (int k = 0; k < db->nruns; k++)
        free(db->runs[k].t);
    db->nruns = 0;
    db->ptext = 0;
}

/* The paragraph ends: a heading (level 1..6), a list item (depth >= 0), or a plain paragraph; in a table it goes to the cell. */
static void db_para(struct docb *db, int heading, int list_depth, bool bullet)
{
    struct mdb t = { 0 };
    db_compose(db, &t);
    size_t a = 0;
    while (a < t.n && t.s[a] == ' ')
        a++;
    if (a >= t.n) {
        free(t.s);
        return;
    }
    db->paragraphs++;
    if (db->tdepth > 0) {
        if (db->cell.n)
            mdb_add(&db->cell, " ", 1);
        mdb_add(&db->cell, t.s + a, t.n - a);
    } else if (heading > 0) {
        db_outs(db, "\n");
        for (int k = 0; k < heading && k < 6; k++)
            db_outs(db, "#");
        db_outs(db, " ");
        db_out(db, t.s + a, t.n - a);
        db_outs(db, "\n");
        db->list_prev = false;
    } else if (list_depth >= 0) {
        if (!db->list_prev || db->list_bullet != bullet)             /* a list of the other kind is another list */
            db_outs(db, "\n");
        db->list_bullet = bullet;
        for (int k = 0; k < list_depth && k < 6; k++)
            db_outs(db, "  ");
        db_outs(db, bullet ? "- " : "1. ");
        db_out(db, t.s + a, t.n - a);
        db_outs(db, "\n");
        db->list_prev = true;
    } else {
        db_outs(db, "\n");
        db_out(db, t.s + a, t.n - a);
        db_outs(db, "\n");
        db->list_prev = false;
    }
    free(t.s);
}

static void db_table_begin(struct docb *db)
{
    if (db->tdepth++ == 0) {
        db->first_row = true;
        for (int i = 0; i < db->ncells; i++)
            free(db->cells[i]);
        db->ncells = 0;
        db->tables++;
        db_outs(db, "\n");
        db->list_prev = false;
    }
}

static void db_table_end(struct docb *db)
{
    if (db->tdepth > 0)
        db->tdepth--;
}

static void db_row_begin(struct docb *db)
{
    if (db->tdepth == 1) {
        for (int i = 0; i < db->ncells; i++)
            free(db->cells[i]);
        db->ncells = 0;
    }
}

static void db_cell_begin(struct docb *db)
{
    if (db->tdepth == 1) {
        db->cell.n = 0;
        if (db->cell.s)
            db->cell.s[0] = 0;
    }
}

static void db_cell_end(struct docb *db)
{
    if (db->tdepth != 1)
        return;
    if (db->ncells == db->capcells) {
        db->capcells = db->capcells ? db->capcells * 2 : 8;
        db->cells = realloc(db->cells, (size_t)db->capcells * sizeof(char *));
    }
    db->cells[db->ncells++] = strdup(db->cell.s ? db->cell.s : "");
    db->cell.n = 0;
    if (db->cell.s)
        db->cell.s[0] = 0;
}

static void db_row_end(struct docb *db)
{
    if (db->tdepth != 1 || db->ncells == 0)
        return;
    db_outs(db, "|");
    for (int i = 0; i < db->ncells; i++) {
        db_outs(db, " ");
        db_outs(db, db->cells[i][0] ? db->cells[i] : " ");
        db_outs(db, " |");
    }
    db_outs(db, "\n");
    if (db->first_row) {
        db_outs(db, "|");
        for (int i = 0; i < db->ncells; i++)
            db_outs(db, " --- |");
        db_outs(db, "\n");
        db->first_row = false;
    }
}

static void db_slide(struct docb *db, int n)
{
    char t[40];
    snprintf(t, sizeof(t), "\n## Slide %d\n", n);
    db_outs(db, t);
    db->slides = n;
    db->list_prev = false;
}

/* ---- DOCX ---- */
struct dstyle {
    char id[64];
    int level;                          /* a heading level, 0 for none */
};

struct dnum {
    int num_abs[256];                   /* numId -> index of its abstract numbering, -1 */
    int nabs;
    int abs_id[64];
    unsigned char bullet[64][9];        /* per abstract numbering and level: 1 = a bullet, 0 = a number */
};

static int style_level(const char *name)
{
    char l[64];
    size_t n = 0;
    for (; name[n] && n < sizeof(l) - 1; n++)
        l[n] = (char)(name[n] >= 'A' && name[n] <= 'Z' ? name[n] + 32 : name[n]);
    l[n] = 0;
    if (!strcmp(l, "title"))
        return 1;
    if (!strncmp(l, "heading ", 8) && l[8] >= '1' && l[8] <= '9')
        return l[8] - '0';
    return 0;
}

static int parse_dstyles(const char *buf, size_t len, struct dstyle *st, int max)
{
    struct xml x = { buf, buf + len };
    struct xev e;
    struct xbuf a = { 0 };
    int n = 0;
    char cur[64] = "";
    while (xml_next(&x, &e) != XE_END) {
        if (e.kind == XE_OPEN && is(&e, "style") && xattr(&e, "styleId", &a)) {
            snprintf(cur, sizeof(cur), "%s", xb_str(&a));
        } else if ((e.kind == XE_OPEN || e.kind == XE_SELF) && is(&e, "name") && cur[0] && n < max && xattr(&e, "val", &a)) {
            int lv = style_level(xb_str(&a));
            if (lv) {
                snprintf(st[n].id, sizeof(st[n].id), "%s", cur);
                st[n++].level = lv;
            }
            cur[0] = 0;
        } else if (e.kind == XE_CLOSE && is(&e, "style")) {
            cur[0] = 0;
        }
    }
    free(a.s);
    return n;
}

static void parse_dnum(const char *buf, size_t len, struct dnum *dn)
{
    struct xml x = { buf, buf + len };
    struct xev e;
    struct xbuf a = { 0 };
    int cur_abs = -1, cur_lvl = -1, cur_num = -1;
    for (int i = 0; i < 256; i++)
        dn->num_abs[i] = -1;
    memset(dn->bullet, 1, sizeof(dn->bullet));
    while (xml_next(&x, &e) != XE_END) {
        bool open = e.kind == XE_OPEN || e.kind == XE_SELF;
        if (open && is(&e, "abstractNum") && xattr(&e, "abstractNumId", &a) && dn->nabs < 64) {
            cur_abs = dn->nabs;
            dn->abs_id[dn->nabs++] = atoi(xb_str(&a));
            cur_lvl = -1;
        } else if (open && is(&e, "lvl") && xattr(&e, "ilvl", &a)) {
            cur_lvl = atoi(xb_str(&a));
        } else if (open && is(&e, "numFmt") && cur_abs >= 0 && cur_lvl >= 0 && cur_lvl < 9 && xattr(&e, "val", &a)) {
            dn->bullet[cur_abs][cur_lvl] = !strcmp(xb_str(&a), "bullet") || !strcmp(xb_str(&a), "none");
        } else if (e.kind == XE_OPEN && is(&e, "num") && xattr(&e, "numId", &a)) {
            cur_num = atoi(xb_str(&a));
        } else if (open && is(&e, "abstractNumId") && cur_num >= 0 && cur_num < 256 && xattr(&e, "val", &a)) {
            int id = atoi(xb_str(&a));
            for (int i = 0; i < dn->nabs; i++)
                if (dn->abs_id[i] == id)
                    dn->num_abs[cur_num] = i;
        } else if (e.kind == XE_CLOSE && is(&e, "abstractNum")) {
            cur_abs = -1;
        } else if (e.kind == XE_CLOSE && is(&e, "num")) {
            cur_num = -1;
        }
    }
    free(a.s);
}

static bool val_off(struct xev *e, struct xbuf *a)
{
    if (!xattr(e, "val", a))
        return false;
    const char *v = xb_str(a);
    return !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "off") || !strcmp(v, "none");
}

static void docx_convert(struct docb *db, const char *buf, size_t len, const struct dstyle *st, int nst, const struct dnum *dn)
{
    struct xml x = { buf, buf + len };
    struct xev e;
    struct xbuf a = { 0 }, tx = { 0 };
    tx.limit = 1u << 20;
    int skip = 0;
    bool in_ppr = false, in_rpr = false, in_t = false, rb = false, ri = false;
    char pstyle[64] = "";
    int numid = -1, ilvl = 0;
    while (!db->full && xml_next(&x, &e) != XE_END) {
        if (e.kind == XE_TEXT) {
            if (in_t && !skip) {
                xb_reset(&tx);
                xb_add_xml(&tx, e.text, e.tlen);
                db_run(db, xb_str(&tx), tx.n, rb, ri);
            }
            continue;
        }
        bool open = e.kind == XE_OPEN || e.kind == XE_SELF;
        if (is(&e, "drawing") || is(&e, "pict") || is(&e, "Fallback") || is(&e, "object") || is(&e, "instrText") || is(&e, "footnoteReference")) {
            if (e.kind == XE_OPEN)
                skip++;
            else if (e.kind == XE_CLOSE && skip)
                skip--;
            continue;
        }
        if (skip)
            continue;
        if (is(&e, "p")) {
            if (e.kind == XE_OPEN || e.kind == XE_SELF) {
                pstyle[0] = 0;
                numid = -1;
                ilvl = 0;
            }
            if (e.kind == XE_CLOSE || e.kind == XE_SELF) {
                int level = 0;
                for (int i = 0; i < nst && pstyle[0]; i++)
                    if (!strcmp(st[i].id, pstyle))
                        level = st[i].level;
                if (!level && pstyle[0] && !strncmp(pstyle, "Heading", 7) && pstyle[7] >= '1' && pstyle[7] <= '9')
                    level = pstyle[7] - '0';                    /* no styles part: the usual style ids */
                if (!level && !strcmp(pstyle, "Title"))
                    level = 1;
                int ni = numid >= 0 && numid < 256 ? dn->num_abs[numid] : -1;
                bool bullet = ni >= 0 && ilvl >= 0 && ilvl < 9 ? dn->bullet[ni][ilvl] : true;
                db_para(db, level, numid > 0 ? (ilvl < 0 ? 0 : ilvl) : -1, bullet);
            }
        } else if (is(&e, "pPr")) {
            in_ppr = e.kind == XE_OPEN;
        } else if (is(&e, "pStyle") && open && xattr(&e, "val", &a)) {
            snprintf(pstyle, sizeof(pstyle), "%s", xb_str(&a));
        } else if (is(&e, "numId") && open && in_ppr && xattr(&e, "val", &a)) {
            numid = atoi(xb_str(&a));
        } else if (is(&e, "ilvl") && open && in_ppr && xattr(&e, "val", &a)) {
            ilvl = atoi(xb_str(&a));
        } else if (is(&e, "r") && e.kind == XE_OPEN) {
            rb = ri = false;
        } else if (is(&e, "rPr")) {
            in_rpr = e.kind == XE_OPEN && !in_ppr;
        } else if (is(&e, "b") && open && in_rpr) {
            rb = !val_off(&e, &a);
        } else if (is(&e, "i") && open && in_rpr) {
            ri = !val_off(&e, &a);
        } else if (is(&e, "t")) {
            in_t = e.kind == XE_OPEN;
        } else if (open && (is(&e, "tab") || is(&e, "br") || is(&e, "cr"))) {
            db_run(db, " ", 1, rb, ri);
        } else if (open && is(&e, "noBreakHyphen")) {
            db_run(db, "-", 1, rb, ri);
        } else if (is(&e, "tbl")) {
            if (e.kind == XE_OPEN)
                db_table_begin(db);
            else if (e.kind == XE_CLOSE)
                db_table_end(db);
        } else if (is(&e, "tr")) {
            if (e.kind == XE_OPEN)
                db_row_begin(db);
            else if (e.kind == XE_CLOSE)
                db_row_end(db);
        } else if (is(&e, "tc")) {
            if (e.kind == XE_OPEN)
                db_cell_begin(db);
            else if (e.kind == XE_CLOSE)
                db_cell_end(db);
        }
    }
    free(a.s);
    free(tx.s);
}

/* ---- OpenDocument text and presentations ---- */
struct ostyle {
    char name[48];
    bool b, i;
};

static void odf_convert(struct docb *db, const char *buf, size_t len, bool presentation)
{
    struct xml x = { buf, buf + len };
    struct xev e;
    struct xbuf a = { 0 }, tx = { 0 }, tmp = { 0 };
    tx.limit = 1u << 20;
    tmp.limit = 1u << 20;
    struct ostyle *os = NULL;
    int nos = 0, capos = 0;
    char cur_style[48] = "";
    bool stack_b[16] = { false }, stack_i[16] = { false };
    int sp = 0, skip = 0, ldepth = 0, heading = 0, frame_title_depth = 0, fdepth = 0;
    bool in_para = false, last_space = true;
    while (!db->full && xml_next(&x, &e) != XE_END) {
        bool open = e.kind == XE_OPEN || e.kind == XE_SELF;
        if (e.kind == XE_TEXT) {
            if (in_para && !skip) {
                xb_reset(&tmp);
                xb_add_xml(&tmp, e.text, e.tlen);
                xb_reset(&tx);
                for (size_t i = 0; i < tmp.n; i++) {                      /* white space of the XML is one blank, counted over the whole paragraph */
                    char c = tmp.s[i];
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                        if (!last_space)
                            xb_add(&tx, " ", 1);
                        last_space = true;
                    } else {
                        xb_add(&tx, &c, 1);
                        last_space = false;
                    }
                }
                db_run(db, xb_str(&tx), tx.n, stack_b[sp], stack_i[sp]);
            }
            continue;
        }
        if (is(&e, "annotation") || is(&e, "note") || is(&e, "object") || is(&e, "plugin") || (!presentation && is(&e, "frame"))) {
            if (e.kind == XE_OPEN)
                skip++;
            else if (e.kind == XE_CLOSE && skip)
                skip--;
            continue;
        }
        if (skip)
            continue;
        if (is(&e, "style") && open) {                                  /* automatic styles: which text styles are bold or emphasised */
            cur_style[0] = 0;
            if (e.kind == XE_OPEN && xattr(&e, "name", &a))
                snprintf(cur_style, sizeof(cur_style), "%s", xb_str(&a));
        } else if (is(&e, "style") && e.kind == XE_CLOSE) {
            cur_style[0] = 0;
        } else if (is(&e, "text-properties") && open && cur_style[0]) {
            bool b = xattr(&e, "font-weight", &a) && !strcmp(xb_str(&a), "bold");
            bool i = xattr(&e, "font-style", &a) && !strcmp(xb_str(&a), "italic");
            if ((b || i) && nos < 4096) {
                if (nos == capos) {
                    capos = capos ? capos * 2 : 32;
                    os = realloc(os, (size_t)capos * sizeof(*os));
                }
                snprintf(os[nos].name, sizeof(os[nos].name), "%s", cur_style);
                os[nos].b = b;
                os[nos++].i = i;
            }
        } else if (is(&e, "page") && presentation && e.kind == XE_OPEN) {
            db_slide(db, db->slides + 1);
        } else if (is(&e, "frame") && presentation) {
            if (e.kind == XE_OPEN) {
                fdepth++;
                if (!frame_title_depth && xattr(&e, "class", &a) && !strcmp(xb_str(&a), "title"))
                    frame_title_depth = fdepth;
            } else if (e.kind == XE_CLOSE) {
                if (frame_title_depth == fdepth)
                    frame_title_depth = 0;
                if (fdepth > 0)
                    fdepth--;
            }
        } else if ((is(&e, "p") || is(&e, "h")) && !in_para && e.kind == XE_OPEN) {
            in_para = true;
            heading = 0;
            if (is(&e, "h")) {
                heading = xattr(&e, "outline-level", &a) ? atoi(xb_str(&a)) : 1;
                heading = heading < 1 ? 1 : heading > 6 ? 6 : heading;
            }
            sp = 0;
            stack_b[0] = stack_i[0] = false;
            last_space = true;
        } else if ((is(&e, "p") || is(&e, "h")) && e.kind == XE_CLOSE && in_para) {
            in_para = false;
            db_para(db, heading ? heading : frame_title_depth ? 3 : 0, !heading && !frame_title_depth && ldepth > 0 ? ldepth - 1 : -1, true);
        } else if (in_para && is(&e, "span")) {
            if (e.kind == XE_OPEN && sp < 15) {
                bool b = stack_b[sp], i = stack_i[sp];
                if (xattr(&e, "style-name", &a))
                    for (int k = 0; k < nos; k++)
                        if (!strcmp(os[k].name, xb_str(&a))) {
                            b |= os[k].b;
                            i |= os[k].i;
                        }
                sp++;
                stack_b[sp] = b;
                stack_i[sp] = i;
            } else if (e.kind == XE_CLOSE && sp > 0) {
                sp--;
            }
        } else if (in_para && is(&e, "s") && open) {
            db_run(db, " ", 1, stack_b[sp], stack_i[sp]);
            last_space = true;
        } else if (in_para && (is(&e, "tab") || is(&e, "line-break")) && open) {
            db_run(db, " ", 1, stack_b[sp], stack_i[sp]);
            last_space = true;
        } else if (is(&e, "list")) {
            if (e.kind == XE_OPEN)
                ldepth++;
            else if (e.kind == XE_CLOSE && ldepth > 0)
                ldepth--;
        } else if (is(&e, "table")) {
            if (e.kind == XE_OPEN)
                db_table_begin(db);
            else if (e.kind == XE_CLOSE)
                db_table_end(db);
        } else if (is(&e, "table-row")) {
            if (e.kind == XE_OPEN)
                db_row_begin(db);
            else if (e.kind == XE_CLOSE)
                db_row_end(db);
        } else if (is(&e, "table-cell")) {
            if (e.kind == XE_OPEN)
                db_cell_begin(db);
            else if (e.kind == XE_CLOSE)
                db_cell_end(db);
            else if (e.kind == XE_SELF && db->tdepth == 1) {
                db_cell_begin(db);
                db_cell_end(db);
            }
        }
    }
    free(os);
    free(a.s);
    free(tx.s);
    free(tmp.s);
}

/* ---- PPTX ---- */
static void pptx_convert(struct docb *db, const char *buf, size_t len)
{
    struct xml x = { buf, buf + len };
    struct xev e;
    struct xbuf a = { 0 }, tx = { 0 };
    tx.limit = 1u << 20;
    bool in_t = false, title = false, nobullet = false, rb = false, ri = false, junk = false;
    int lvl = 0, skip = 0;
    while (!db->full && xml_next(&x, &e) != XE_END) {
        if (e.kind == XE_TEXT) {
            if (in_t && !skip) {
                xb_reset(&tx);
                xb_add_xml(&tx, e.text, e.tlen);
                db_run(db, xb_str(&tx), tx.n, rb, ri);
            }
            continue;
        }
        bool open = e.kind == XE_OPEN || e.kind == XE_SELF;
        if (is(&e, "Fallback") || is(&e, "pic") || is(&e, "extLst")) {
            if (e.kind == XE_OPEN)
                skip++;
            else if (e.kind == XE_CLOSE && skip)
                skip--;
            continue;
        }
        if (skip)
            continue;
        if (is(&e, "sp") && e.kind == XE_OPEN) {
            title = junk = false;
        } else if (is(&e, "ph") && open) {
            if (xattr(&e, "type", &a) && (!strcmp(xb_str(&a), "title") || !strcmp(xb_str(&a), "ctrTitle")))
                title = true;
            if (xattr(&e, "type", &a) && (!strcmp(xb_str(&a), "sldNum") || !strcmp(xb_str(&a), "ftr") || !strcmp(xb_str(&a), "dt") || !strcmp(xb_str(&a), "hdr")))
                junk = true;                                    /* the slide number, the footer, the date: not the slide's text */
        } else if (is(&e, "p")) {
            if (e.kind == XE_OPEN) {
                lvl = 0;
                nobullet = false;
            } else if (e.kind == XE_CLOSE) {
                if (junk)
                    db_discard(db);
                else
                    db_para(db, title && db->tdepth == 0 ? 3 : 0, !title && !nobullet && db->tdepth == 0 ? lvl : -1, true);
            }
        } else if (is(&e, "pPr") && open && xattr(&e, "lvl", &a)) {
            lvl = atoi(xb_str(&a));
            lvl = lvl < 0 ? 0 : lvl > 5 ? 5 : lvl;
        } else if (is(&e, "buNone") && open) {
            nobullet = true;
        } else if (is(&e, "r") && e.kind == XE_OPEN) {
            rb = ri = false;
        } else if (is(&e, "rPr") && open) {
            rb = xattr(&e, "b", &a) && !strcmp(xb_str(&a), "1");
            ri = xattr(&e, "i", &a) && !strcmp(xb_str(&a), "1");
        } else if (is(&e, "t")) {
            in_t = e.kind == XE_OPEN;
        } else if (is(&e, "br") && open) {
            db_run(db, " ", 1, rb, ri);
        } else if (is(&e, "tbl")) {
            if (e.kind == XE_OPEN)
                db_table_begin(db);
            else if (e.kind == XE_CLOSE)
                db_table_end(db);
        } else if (is(&e, "tr")) {
            if (e.kind == XE_OPEN)
                db_row_begin(db);
            else if (e.kind == XE_CLOSE)
                db_row_end(db);
        } else if (is(&e, "tc")) {
            if (e.kind == XE_OPEN)
                db_cell_begin(db);
            else if (e.kind == XE_CLOSE)
                db_cell_end(db);
        }
    }
    free(a.s);
    free(tx.s);
}

static int slide_num(const char *name)
{
    return atoi(name + strlen("ppt/slides/slide"));
}

static int cmp_slide(const void *a, const void *b)
{
    return slide_num(*(char *const *)a) - slide_num(*(char *const *)b);
}

/* ---- EPUB: the spine's XHTML files in reading order ---- */
static bool ci_name(const struct xev *e, const char *n)
{
    return is(e, n);
}

static void epub_convert(struct docb *db, const char *buf, size_t len)
{
    struct xml x = { buf, buf + len };
    struct xev e;
    struct xbuf tx = { 0 }, tmp = { 0 }, a = { 0 };
    tx.limit = 1u << 20;
    tmp.limit = 1u << 20;
    int skip = 0, bold = 0, ital = 0, ldepth = 0, heading = 0, in_li = 0;
    bool last_space = true;
    bool ordered[16] = { false };
    while (!db->full && xml_next(&x, &e) != XE_END) {
        bool open = e.kind == XE_OPEN || e.kind == XE_SELF;
        if (e.kind == XE_TEXT) {
            if (skip)
                continue;
            xb_reset(&tmp);
            xb_add_xml(&tmp, e.text, e.tlen);
            xb_reset(&tx);
            for (size_t i = 0; i < tmp.n; i++) {
                char c = tmp.s[i];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    if (!last_space)
                        xb_add(&tx, " ", 1);
                    last_space = true;
                } else {
                    xb_add(&tx, &c, 1);
                    last_space = false;
                }
            }
            db_run(db, xb_str(&tx), tx.n, bold > 0, ital > 0);
            continue;
        }
        if (ci_name(&e, "head") || ci_name(&e, "script") || ci_name(&e, "style") || ci_name(&e, "svg") || ci_name(&e, "math")) {
            if (e.kind == XE_OPEN)
                skip++;
            else if (e.kind == XE_CLOSE && skip)
                skip--;
            continue;
        }
        if (skip)
            continue;
        int hl = e.nlen == 2 && (e.name[0] == 'h' || e.name[0] == 'H') && e.name[1] >= '1' && e.name[1] <= '6' ? e.name[1] - '0' : 0;
        bool list_tag = ci_name(&e, "ul") || ci_name(&e, "ol");
        bool block = hl || list_tag || ci_name(&e, "p") || ci_name(&e, "div") || ci_name(&e, "li") || ci_name(&e, "blockquote") || ci_name(&e, "pre")
                     || ci_name(&e, "section") || ci_name(&e, "article") || ci_name(&e, "aside") || ci_name(&e, "header") || ci_name(&e, "footer")
                     || ci_name(&e, "figure") || ci_name(&e, "figcaption") || ci_name(&e, "dt") || ci_name(&e, "dd") || ci_name(&e, "dl");
        if (block) {                                                    /* every block tag ends the paragraph before it */
            if (db->nruns)
                db_para(db, heading, in_li && ldepth > 0 ? ldepth - 1 : -1, ldepth > 0 && !ordered[ldepth - 1 < 15 ? ldepth - 1 : 15]);
            last_space = true;
            if (hl) {
                heading = e.kind == XE_OPEN ? hl : 0;
            } else if (list_tag) {
                if (e.kind == XE_OPEN && ldepth < 15)
                    ordered[ldepth++] = ci_name(&e, "ol");
                else if (e.kind == XE_CLOSE && ldepth > 0)
                    ldepth--;
            } else if (ci_name(&e, "li")) {
                in_li = e.kind == XE_OPEN ? 1 : 0;
            }
            continue;
        }
        if (ci_name(&e, "b") || ci_name(&e, "strong")) {
            if (e.kind == XE_OPEN) bold++; else if (e.kind == XE_CLOSE && bold) bold--;
        } else if (ci_name(&e, "i") || ci_name(&e, "em") || ci_name(&e, "cite") || ci_name(&e, "var")) {
            if (e.kind == XE_OPEN) ital++; else if (e.kind == XE_CLOSE && ital) ital--;
        } else if (ci_name(&e, "br") && open) {
            db_run(db, " ", 1, false, false);
            last_space = true;
        } else if (ci_name(&e, "hr") && open) {
            if (db->nruns)
                db_para(db, heading, -1, true);
            db_outs(db, "\n---\n");
        } else if (ci_name(&e, "img") && open) {
            if (xattr(&e, "alt", &a) && a.n) {
                xb_reset(&tx);
                xb_add(&tx, "[", 1);
                xb_add(&tx, xb_str(&a), a.n);
                xb_add(&tx, "]", 1);
                db_run(db, xb_str(&tx), tx.n, false, false);
                last_space = false;
            }
        } else if (ci_name(&e, "table")) {
            if (db->nruns)
                db_para(db, heading, -1, true);
            if (e.kind == XE_OPEN) db_table_begin(db); else if (e.kind == XE_CLOSE) db_table_end(db);
            last_space = true;
        } else if (ci_name(&e, "tr")) {
            if (db->nruns)
                db_para(db, heading, -1, true);
            if (e.kind == XE_OPEN) db_row_begin(db); else if (e.kind == XE_CLOSE) db_row_end(db);
            last_space = true;
        } else if (ci_name(&e, "td") || ci_name(&e, "th")) {
            if (db->nruns)
                db_para(db, heading, -1, true);
            if (e.kind == XE_OPEN) db_cell_begin(db);
            else if (e.kind == XE_CLOSE) db_cell_end(db);
            else if (e.kind == XE_SELF && db->tdepth == 1) { db_cell_begin(db); db_cell_end(db); }
            last_space = true;
        }
    }
    if (db->nruns)
        db_para(db, heading, -1, true);
    free(tx.s);
    free(tmp.s);
    free(a.s);
}

static void url_decode(char *s)
{
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char t[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(t, NULL, 16);
            r += 2;
        } else if (*r == '#') {
            break;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

/* The chapters of an EPUB in reading order; returns 0 and fills the title / author, or -1 when it is no EPUB. */
static int epub_convert_book(const struct zip *z, struct docb *db, char *title, size_t tn, char *author, size_t an, int *chapters, const char **why)
{
    size_t len;
    char *cx = zip_read(z, "META-INF/container.xml", PART_SMALL, &len, why);
    if (!cx)
        return -1;
    char opf[512] = "";
    {
        struct xml x = { cx, cx + len };
        struct xev e;
        struct xbuf a = { 0 };
        while (xml_next(&x, &e) != XE_END)
            if (is(&e, "rootfile") && (e.kind == XE_OPEN || e.kind == XE_SELF) && xattr(&e, "full-path", &a)) {
                snprintf(opf, sizeof(opf), "%s", xb_str(&a));
                break;
            }
        free(a.s);
    }
    free(cx);
    if (!opf[0])
        return -1;
    char *ox = zip_read(z, opf, PART_SMALL, &len, why);
    if (!ox)
        return -1;
    char dir[512] = "";
    {
        char *sl = strrchr(opf, '/');
        if (sl)
            snprintf(dir, sizeof(dir), "%.*s", (int)(sl - opf + 1), opf);
    }
    struct item { char id[96]; char href[400]; char type[64]; } *items = calloc(2000, sizeof(struct item));
    int nitems = 0;
    char **spine = calloc(1000, sizeof(char *));
    int nspine = 0;
    struct xml x = { ox, ox + len };
    struct xev e;
    struct xbuf a = { 0 }, t = { 0 };
    bool in_title = false, in_author = false;
    title[0] = author[0] = 0;
    while (xml_next(&x, &e) != XE_END) {
        if (e.kind == XE_TEXT) {
            if ((in_title && !title[0]) || (in_author && !author[0])) {
                xb_reset(&t);
                xb_add_xml(&t, e.text, e.tlen);
                snprintf(in_title ? title : author, in_title ? tn : an, "%s", xb_str(&t));
            }
            continue;
        }
        if (is(&e, "title")) in_title = e.kind == XE_OPEN;
        else if (is(&e, "creator")) in_author = e.kind == XE_OPEN;
        else if (is(&e, "item") && (e.kind == XE_OPEN || e.kind == XE_SELF) && nitems < 2000) {
            struct item *it = &items[nitems];
            if (xattr(&e, "id", &a)) snprintf(it->id, sizeof(it->id), "%s", xb_str(&a));
            if (xattr(&e, "href", &a)) snprintf(it->href, sizeof(it->href), "%s", xb_str(&a));
            if (xattr(&e, "media-type", &a)) snprintf(it->type, sizeof(it->type), "%s", xb_str(&a));
            nitems++;
        } else if (is(&e, "itemref") && (e.kind == XE_OPEN || e.kind == XE_SELF) && nspine < 1000 && xattr(&e, "idref", &a))
            spine[nspine++] = strdup(xb_str(&a));
    }
    free(ox);
    int done = 0;
    for (int i = 0; i < nspine && !db->full && done < 300; i++) {
        for (int k = 0; k < nitems; k++) {
            if (strcmp(items[k].id, spine[i]) || !strstr(items[k].type, "html"))
                continue;
            char name[1024];
            snprintf(name, sizeof(name), "%s%s", dir, items[k].href);
            url_decode(name);
            char *hx = zip_read(z, name, PART_BIG, &len, why);
            if (hx) {
                epub_convert(db, hx, len);
                free(hx);
                done++;
            }
            break;
        }
    }
    for (int i = 0; i < nspine; i++)
        free(spine[i]);
    free(spine);
    free(items);
    free(a.s);
    free(t.s);
    *chapters = done;
    return 0;
}

/* ---- the old binary Word (.doc) and PowerPoint (.ppt): streams of an OLE2 compound file ---- */
static int cfs_find_stream(const struct cfs *f, const char *want, uint32_t *start, uint64_t *size, uint32_t *rstart, uint64_t *rsize)
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
            if (!strcmp(name, want)) {
                *start = u32(d + 116);
                *size = u32(d + 120);
                found = 1;
            }
        }
    }
    free(sec);
    free(ds);
    return found;
}

/* A whole stream by name (malloc'd, *n bytes, at most `cap`); NULL when it is missing. */
static uint8_t *ole_stream(const struct cfs *f, const char *name, size_t cap, size_t *n)
{
    uint32_t start = 0, rstart = 0;
    uint64_t size = 0, rsize = 0;
    if (!cfs_find_stream(f, name, &start, &size, &rstart, &rsize) || size > cap)
        return NULL;
    struct cstream cs;
    if (cstream_open(&cs, f, start, size, rstart, rsize) != 0) {
        cstream_close(&cs);                                                  /* (a failed open can still hold a buffer) */
        return NULL;
    }
    uint8_t *buf = malloc((size_t)cs.size + 1);
    size_t got = buf ? cstream_read(&cs, 0, buf, (size_t)cs.size) : 0;
    cstream_close(&cs);
    if (!buf)
        return NULL;
    buf[got] = 0;
    *n = got;
    return buf;
}

static unsigned cp1252_char(unsigned b)
{
    static const uint16_t hi[32] = { 0x20ac, 0x81, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8d, 0x017d, 0x8f,
                                     0x90, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x9d, 0x017e, 0x0178 };
    return b >= 0x80 && b < 0xa0 ? hi[b - 0x80] : b;
}

/* The code page the document's SummaryInformation property set declares (PID_CODEPAGE), 0 when there is none or it is Unicode. */
static int ole_summary_codepage(const struct cfs *f)
{
    size_t n = 0;
    uint8_t *b = ole_stream(f, "\x05SummaryInformation", 1u << 20, &n);
    int cp = 0;
    if (b && n >= 48) {
        uint32_t off = u32(b + 44);
        if ((uint64_t)off + 8 <= n) {
            uint32_t np = u32(b + off + 4);
            for (uint32_t i = 0; i < np && i < 64 && (uint64_t)off + 8 + 8 * (i + 1) <= n; i++) {
                uint32_t id = u32(b + off + 8 + 8 * i), po = u32(b + off + 12 + 8 * i);
                if (id == 1 && (uint64_t)off + po + 8 <= n && u16(b + off + po) == 2)
                    cp = (int)(uint16_t)u16(b + off + po + 4);
            }
        }
    }
    free(b);
    return cp >= 1250 && cp <= 1258 ? cp : 0;
}

/* The Windows code page of a Word 6 / 95 document's language (its FIB's lid): the upper half of the byte text. NULL = Western (cp1252). */
static const uint16_t *lid_codepage(unsigned lid)
{
    switch (lid & 0x3ff) {
    case 0x02: case 0x19: case 0x22: case 0x23: case 0x2f: case 0x44: case 0x3f:
        return k_cp1251;                                                    /* Bulgarian, Russian, Ukrainian, Belarusian, Macedonian, Tatar, Kazakh */
    case 0x05: case 0x0e: case 0x15: case 0x18: case 0x1b: case 0x24: case 0x1a: case 0x1c:
        return k_cp1250;                                                    /* Czech, Hungarian, Polish, Romanian, Slovak, Slovenian, Croatian / Serbian, Albanian */
    case 0x25: case 0x26: case 0x27:
        return k_cp1257;                                                    /* Estonian, Latvian, Lithuanian */
    case 0x08:
        return k_cp1253;                                                    /* Greek */
    case 0x1f:
        return k_cp1254;                                                    /* Turkish */
    default:
        return NULL;
    }
}

struct docstate {
    struct docb *db;
    int field_depth;
    uint32_t field_state[16];           /* per depth: 1 = in the code (skipped), 2 = in the result */
    bool in_table, cell_empty;
    int cells_in_row;
    struct xbuf tx;
    uint32_t total;
};

/* One character of the main text of a .doc: fields, paragraph ends, table cell marks, placeholders. */
static void doc_char(struct docstate *s, unsigned ch)
{
    struct docb *db = s->db;
    s->total++;
    if (ch == 0x13) {
        if (s->field_depth < 15)
            s->field_state[++s->field_depth] = 1;
        return;
    }
    if (ch == 0x14) {
        if (s->field_depth > 0)
            s->field_state[s->field_depth] = 2;
        return;
    }
    if (ch == 0x15) {
        if (s->field_depth > 0)
            s->field_depth--;
        return;
    }
    if (s->field_depth > 0 && s->field_state[s->field_depth] == 1)          /* a field's code */
        return;
    if (ch == 0x0d) {                                                       /* the end of a paragraph */
        if (s->in_table) {
            db_table_end(db);
            s->in_table = false;
            s->cells_in_row = 0;
        }
        db_para(db, 0, -1, true);
        return;
    }
    if (ch == 0x07) {                                                       /* the end of a table cell, or (an empty one) of the row */
        if (!s->in_table) {
            db_table_begin(db);
            db_row_begin(db);
            s->in_table = true;
            s->cells_in_row = 0;
        }
        if (s->cell_empty && s->cells_in_row > 0 && db->nruns == 0) {
            db_row_end(db);
            db_row_begin(db);
            s->cells_in_row = 0;
        } else {
            db_para(db, 0, -1, true);
            db_cell_end(db);
            s->cells_in_row++;
        }
        s->cell_empty = true;
        return;
    }
    if (ch < 0x20 && ch != 0x09 && ch != 0x0b && ch != 0x0c)
        return;                                                             /* pictures, footnote marks and other placeholders */
    if (ch == 0x0b || ch == 0x0c || ch == 0x09)
        ch = ' ';
    else if (ch == 0x1e)
        ch = '-';
    xb_reset(&s->tx);
    xb_utf8(&s->tx, ch);
    db_run(db, xb_str(&s->tx), s->tx.n, false, false);
    s->cell_empty = false;
}

static void doc_finish(struct docstate *s)
{
    if (s->in_table)
        db_table_end(s->db);
    db_para(s->db, 0, -1, true);
    free(s->tx.s);
}

/* The main text of a .doc: Word 97 and later through the piece table (the CLX of the table stream), Word 6 / 95 as the plain bytes from fcMin to fcMac. */
static bool doc_convert(struct docb *db, const uint8_t *wd, size_t wdn, const uint8_t *tbl, size_t tbn, int sumcp, char *err, size_t errn)
{
    if (wdn < 0x40 || u16(wd) != 0xa5ec) {
        if (wdn >= 0x40 && (u16(wd) == 0xa5db || u16(wd) == 0xa5dc || (u16(wd) >= 0x0200 && u16(wd) <= 0x0300 && false))) {
            /* Word 6 / 95: fall through to the plain text below */
        } else if (wdn >= 0x40 && u16(wd + 2) < 0xc1 && u16(wd) >= 0xa500) {
        } else
            return false;
    }
    struct docstate st;
    memset(&st, 0, sizeof(st));
    st.db = db;
    st.tx.limit = 1u << 20;
    st.cell_empty = true;
    unsigned nfib = u16(wd + 2);
    if (nfib < 0xc1 || !tbl) {                                              /* Word 6 / 95 */
        uint32_t fmin = u32(wd + 0x18), fmac = u32(wd + 0x1c);
        if (fmin < 0x40 || fmac > wdn || fmac <= fmin) {
            seterr(err, errn, "The text of this old Word document cannot be found.");
            return false;
        }
        const uint16_t *cpt = sumcp == 1250 ? k_cp1250 : sumcp == 1251 ? k_cp1251 : sumcp == 1253 ? k_cp1253 : sumcp == 1254 ? k_cp1254 : sumcp == 1257 ? k_cp1257 : sumcp == 1252 ? NULL : lid_codepage(u16(wd + 6));
        for (uint32_t i = fmin; i < fmac && !db->full; i++) {
            unsigned b = wd[i];
            doc_char(&st, b >= 0x80 && cpt && cpt[b - 0x80] ? cpt[b - 0x80] : cp1252_char(b));
        }
        doc_finish(&st);
        return st.total > 0;
    }
    if (wdn < 0x1a8)
        return false;
    unsigned csw = u16(wd + 32);
    size_t o = 34 + (size_t)csw * 2;
    if (o + 2 > wdn)
        return false;
    unsigned cslw = u16(wd + o);
    size_t lw = o + 2;
    if (lw + (size_t)cslw * 4 > wdn || cslw < 4)
        return false;
    uint32_t ccp = u32(wd + lw + 12);
    size_t fcl = lw + (size_t)cslw * 4;
    if (fcl + 2 > wdn)
        return false;
    unsigned cb = u16(wd + fcl);
    if (cb < 34 || fcl + 2 + 34 * 8 > wdn)
        return false;
    uint32_t fc_clx = u32(wd + fcl + 2 + 33 * 8), lcb_clx = u32(wd + fcl + 2 + 33 * 8 + 4);
    if ((u16(wd + 10) & 0x0100)) {
        seterr(err, errn, "The document is encrypted.");
        return false;
    }
    if ((uint64_t)fc_clx + lcb_clx > tbn || lcb_clx < 5) {
        seterr(err, errn, "The text table of the document is missing.");
        return false;
    }
    const uint8_t *clx = tbl + fc_clx;
    size_t cl = lcb_clx, p = 0;
    while (p < cl && clx[p] == 1) {                                          /* Prc: skipped */
        if (p + 3 > cl)
            return false;
        p += 3 + u16(clx + p + 1);
    }
    if (p + 5 > cl || clx[p] != 2)
        return false;
    uint32_t plc = u32(clx + p + 1);
    p += 5;
    if (p + plc > cl || plc < 4)
        return false;
    int n = (int)((plc - 4) / 12);
    if (n < 1 || n > 200000)
        return false;
    const uint8_t *cps = clx + p, *pcd = cps + 4 * (size_t)(n + 1);
    for (int i = 0; i < n && !db->full; i++) {
        uint32_t c0 = u32(cps + 4 * i), c1 = u32(cps + 4 * (i + 1));
        if (c0 >= ccp)
            break;
        if (c1 > ccp)
            c1 = ccp;
        if (c1 <= c0)
            continue;
        uint32_t fc = u32(pcd + 8 * (size_t)i + 2);
        bool bytes = (fc & 0x40000000u) != 0;
        uint64_t off = bytes ? (uint64_t)(fc & 0x3fffffffu) / 2 : fc;
        uint32_t len = c1 - c0;
        if (off + (uint64_t)len * (bytes ? 1 : 2) > wdn)
            len = (uint32_t)((wdn > off ? wdn - off : 0) / (bytes ? 1 : 2));
        for (uint32_t k = 0; k < len && !db->full; k++) {
            unsigned ch = bytes ? cp1252_char(wd[off + k]) : u16(wd + off + 2 * k);
            if (!bytes && ch >= 0xd800 && ch < 0xdc00 && k + 1 < len) {
                unsigned lo = u16(wd + off + 2 * (k + 1));
                if (lo >= 0xdc00 && lo < 0xe000) {
                    ch = 0x10000 + ((ch - 0xd800) << 10) + (lo - 0xdc00);
                    k++;
                }
            }
            doc_char(&st, ch);
        }
    }
    doc_finish(&st);
    return st.total > 0;
}

/* One slide's text of a .ppt: the text atoms between two SlidePersistAtoms of the slide list. */
static void ppt_text_atom(struct docb *db, const uint8_t *d, size_t n, bool wide, int text_type)
{
    struct xbuf line = { 0 };
    line.limit = 1u << 20;
    size_t units = wide ? n / 2 : n;
    for (size_t i = 0; i <= units && !db->full; i++) {
        unsigned ch = i == units ? '\r' : wide ? u16(d + 2 * i) : cp1252_char(d[i]);
        if (wide && ch >= 0xd800 && ch < 0xdc00 && i + 1 < units) {
            unsigned lo = u16(d + 2 * (i + 1));
            if (lo >= 0xdc00 && lo < 0xe000) {
                ch = 0x10000 + ((ch - 0xd800) << 10) + (lo - 0xdc00);
                i++;
            }
        }
        if (ch == '\r' || ch == '\n' || ch == 0x0b) {
            if (ch == 0x0b) {
                db_run(db, " ", 1, false, false);
                continue;
            }
            bool title = text_type == 0 || text_type == 6;
            db_para(db, title ? 3 : 0, title ? -1 : 0, true);
            continue;
        }
        if (ch < 0x20 || ch == 0x7f)
            continue;
        xb_reset(&line);
        xb_utf8(&line, ch);
        db_run(db, xb_str(&line), line.n, false, false);
    }
    free(line.s);
}

static void ppt_walk(struct docb *db, const uint8_t *d, size_t n, int depth, bool *in_slides, int *text_type)
{
    size_t p = 0;
    while (p + 8 <= n && !db->full) {
        unsigned ver = d[p] & 15, inst = (u16(d + p) >> 4), type = u16(d + p + 2);
        size_t len = u32(d + p + 4);
        if (len > n - p - 8)
            len = n - p - 8;
        const uint8_t *body = d + p + 8;
        if (type == 0x0ff0 && ver == 0xf && depth < 6) {                     /* SlideListWithText: instance 0 = the slides (1 masters, 2 notes) */
            if (inst == 0) {
                bool was = *in_slides;
                *in_slides = true;
                ppt_walk(db, body, len, depth + 1, in_slides, text_type);
                *in_slides = was;
            }
        } else if (type == 0x03f3 && *in_slides) {                           /* SlidePersistAtom: the next slide starts */
            db_slide(db, db->slides + 1);
            *text_type = 1;
        } else if (type == 0x0f9f && *in_slides && len >= 4) {               /* TextHeaderAtom: what kind of text follows */
            *text_type = (int)u32(body);
        } else if ((type == 0x0fa0 || type == 0x0fa8) && *in_slides) {
            ppt_text_atom(db, body, len, type == 0x0fa0, *text_type);
        } else if (ver == 0xf && depth < 6 && type != 0x0ff0 && len) {       /* other containers (the document) are looked into */
            ppt_walk(db, body, len, depth + 1, in_slides, text_type);
        }
        p += 8 + len;
    }
}

/* .doc / .ppt: 0 = converted; -1 = an OLE2 file that is neither; -2 = unreadable. */
static int ole_document(const char *path, struct docb *db, char *what, size_t whatn, char *err, size_t errn)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct cfs f;
    int r = cfs_open(&f, fd, err, errn);
    if (r < 0) {
        close(fd);
        return r;
    }
    int result = -1;
    size_t wn = 0, tn = 0, pn = 0;
    uint8_t *wd = ole_stream(&f, "WordDocument", 64u << 20, &wn);
    if (wd) {
        snprintf(what, whatn, "Word document (97-2003)");
        bool second = wn > 11 && (u16(wd + 10) & 0x0200);
        uint8_t *tbl = ole_stream(&f, second ? "1Table" : "0Table", 64u << 20, &tn);
        if (doc_convert(db, wd, wn, tbl, tn, ole_summary_codepage(&f), err, errn))
            result = 0;
        else {
            if (!err[0])
                seterr(err, errn, "The text of the document cannot be read.");
            result = -2;
        }
        free(tbl);
        free(wd);
    } else {
        uint8_t *pp = ole_stream(&f, "PowerPoint Document", 128u << 20, &pn);
        if (pp) {
            snprintf(what, whatn, "Presentation (97-2003)");
            bool in_slides = false;
            int tt = 1;
            /* a presentation saved again and again keeps its older documents in the stream: the LAST DocumentContainer is the present one */
            size_t q = 0, last_off = 0, last_len = 0;
            bool have = false;
            while (q + 8 <= pn) {
                unsigned ty = u16(pp + q + 2);
                size_t ln = u32(pp + q + 4);
                if (ln > pn - q - 8)
                    ln = pn - q - 8;
                if (ty == 0x03e8) {
                    last_off = q + 8;
                    last_len = ln;
                    have = true;
                }
                q += 8 + ln;
            }
            if (have)
                ppt_walk(db, pp + last_off, last_len, 0, &in_slides, &tt);
            else
                ppt_walk(db, pp, pn, 0, &in_slides, &tt);
            result = 0;
            free(pp);
        }
    }
    cfs_close(&f);
    close(fd);
    return result;
}

/* ---- RTF ---- */
struct rtfstate {
    bool skip;                          /* inside a destination that shows nothing */
    bool bold, ital;
    int uc;                             /* the characters a \u escape is followed by in a fallback */
    int font;                           /* \fN: the font in use (its charset says how \'hh bytes are read) */
};

struct rtfctx {
    struct docb *db;
    const uint16_t *cp;                 /* the code page's upper half, NULL = Latin-1 / cp1252 */
    int style_head[256];                /* \sN -> heading level (0 = none) */
    bool in_style_table;
    int style_depth;
    int cur_style_def;                  /* the style being defined in the stylesheet */
    char style_name[64];
    int para_style;
    bool in_list;
    bool in_table, row_pending_end;
    int cells;
    int skip_chars;                     /* the fallback characters after \uN */
    uint8_t font_charset[8192];          /* \fcharsetN of each font of the font table (0 = default) */
    bool in_fonttbl;
    int fonttbl_depth, cur_font_def;
};

static void rtf_put(struct rtfctx *c, const struct rtfstate *st, unsigned cp)
{
    if (c->skip_chars > 0) {
        c->skip_chars--;
        return;
    }
    if (c->in_style_table) {
        if (cp == ';') {
            if (c->cur_style_def >= 0 && c->cur_style_def < 256) {
                int lvl = 0;
                if (!strncasecmp(c->style_name, "heading ", 8))
                    lvl = atoi(c->style_name + 8);
                else if (!strcasecmp(c->style_name, "title"))
                    lvl = 1;
                c->style_head[c->cur_style_def] = lvl > 6 ? 6 : lvl;
            }
            c->style_name[0] = 0;
        } else if (strlen(c->style_name) < sizeof(c->style_name) - 5) {
            struct xbuf t = { 0 };
            t.limit = 16;
            xb_utf8(&t, cp);
            strncat(c->style_name, xb_str(&t), 4);
            free(t.s);
        }
        return;
    }
    if (st->skip)
        return;
    if (c->row_pending_end && c->in_table && cp != ' ') {                    /* text after the last row: the table is over */
        db_table_end(c->db);
        c->in_table = c->row_pending_end = false;
    }
    struct xbuf t = { 0 };
    t.limit = 16;
    xb_utf8(&t, cp);
    db_run(c->db, xb_str(&t), t.n, st->bold, st->ital);
    free(t.s);
}

static void rtf_end_para(struct rtfctx *c)
{
    if (c->in_table && c->row_pending_end) {
        db_table_end(c->db);
        c->in_table = c->row_pending_end = false;
    }
    int head = c->para_style >= 0 && c->para_style < 256 ? c->style_head[c->para_style] : 0;
    db_para(c->db, head, !c->in_table && !head && c->in_list ? 0 : -1, true);
}

static void rtf_convert(struct docb *db, const char *buf, size_t len)
{
    struct rtfctx c;
    memset(&c, 0, sizeof(c));
    c.db = db;
    c.cur_style_def = -1;
    c.para_style = -1;
    struct rtfstate stack[64];
    int sp = 0;
    stack[0] = (struct rtfstate){ false, false, false, 1, -1 };
    for (size_t i = 0; i < len && !db->full; i++) {
        char ch = buf[i];
        struct rtfstate *st = &stack[sp];
        if (ch == '{') {
            if (sp < 63) {
                stack[sp + 1] = *st;
                sp++;
                if (c.in_style_table)
                    c.style_depth++;
                if (c.in_fonttbl)
                    c.fonttbl_depth++;
            }
        } else if (ch == '}') {
            if (c.in_style_table) {
                if (c.style_depth > 0)
                    c.style_depth--;
                if (c.style_depth == 0 && sp > 0)
                    c.in_style_table = false;
            }
            if (c.in_fonttbl) {
                if (c.fonttbl_depth > 0)
                    c.fonttbl_depth--;
                if (c.fonttbl_depth == 0)
                    c.in_fonttbl = false;
            }
            if (sp > 0)
                sp--;
        } else if (ch == '\\' && i + 1 < len) {
            char n = buf[i + 1];
            if (n == '\\' || n == '{' || n == '}') {
                rtf_put(&c, st, (unsigned char)n);
                i++;
            } else if (n == '\'') {                                          /* \'hh: a character of the code page */
                if (i + 3 < len + 0 && isxdigit((unsigned char)buf[i + 2]) && isxdigit((unsigned char)buf[i + 3])) {
                    char hx[3] = { buf[i + 2], buf[i + 3], 0 };
                    unsigned b = (unsigned)strtoul(hx, NULL, 16), cp = b;
                    if (b >= 0x80) {
                        const uint16_t *tab = c.cp;
                        int cs = st->font >= 0 && st->font < 8192 ? c.font_charset[st->font] : 0;
                        if (cs == 204 || cs == 238 || cs == 186 || cs == 161 || cs == 162)
                            tab = cs == 204 ? k_cp1251 : cs == 238 ? k_cp1250 : cs == 186 ? k_cp1257 : cs == 161 ? k_cp1253 : k_cp1254;
                        cp = tab && tab[b - 0x80] ? tab[b - 0x80] : cp1252_char(b);
                    }
                    rtf_put(&c, st, cp);
                    i += 3;
                } else
                    i++;
            } else if (n == '~') {
                rtf_put(&c, st, 0xa0);
                i++;
            } else if (n == '-' || n == '_') {
                if (n == '_')
                    rtf_put(&c, st, 0x2011);
                i++;
            } else if (n == '*') {                                           /* an ignorable destination: the group is skipped */
                st->skip = true;
                i++;
            } else if (isalpha((unsigned char)n)) {
                size_t k = i + 1;
                char word[32];
                size_t wl = 0;
                while (k < len && isalpha((unsigned char)buf[k]) && wl < sizeof(word) - 1)
                    word[wl++] = buf[k++];
                word[wl] = 0;
                bool has_num = false, neg = false;
                long num = 0;
                if (k < len && buf[k] == '-') {
                    neg = true;
                    k++;
                }
                while (k < len && isdigit((unsigned char)buf[k])) {
                    has_num = true;
                    if (num < 100000000)
                        num = num * 10 + (buf[k] - '0');
                    k++;
                }
                if (neg)
                    num = -num;
                if (k < len && buf[k] == ' ')
                    k++;                                                     /* the delimiter */
                i = k - 1;
                if (!strcmp(word, "par") || !strcmp(word, "sect") || !strcmp(word, "page")) {
                    if (!st->skip) {
                        rtf_end_para(&c);
                        c.skip_chars = 0;
                    }
                } else if (!strcmp(word, "pard")) {
                    c.para_style = -1;
                    c.in_list = false;
                    if (c.in_table && c.row_pending_end) {
                        db_table_end(db);
                        c.in_table = c.row_pending_end = false;
                    }
                } else if (!strcmp(word, "s") && has_num) {
                    if (c.in_style_table)
                        c.cur_style_def = (int)num;
                    else
                        c.para_style = (int)num;
                } else if (!strcmp(word, "ls") || !strcmp(word, "pnlvl")) {
                    c.in_list = true;
                } else if (!strcmp(word, "line")) {
                    rtf_put(&c, st, ' ');
                } else if (!strcmp(word, "tab")) {
                    rtf_put(&c, st, ' ');
                } else if (!strcmp(word, "emdash")) {
                    rtf_put(&c, st, 0x2014);
                } else if (!strcmp(word, "endash")) {
                    rtf_put(&c, st, 0x2013);
                } else if (!strcmp(word, "bullet")) {
                    rtf_put(&c, st, 0x2022);
                } else if (!strcmp(word, "lquote")) {
                    rtf_put(&c, st, 0x2018);
                } else if (!strcmp(word, "rquote")) {
                    rtf_put(&c, st, 0x2019);
                } else if (!strcmp(word, "ldblquote")) {
                    rtf_put(&c, st, 0x201c);
                } else if (!strcmp(word, "rdblquote")) {
                    rtf_put(&c, st, 0x201d);
                } else if (!strcmp(word, "b")) {
                    st->bold = !has_num || num != 0;
                } else if (!strcmp(word, "i")) {
                    st->ital = !has_num || num != 0;
                } else if (!strcmp(word, "plain")) {
                    st->bold = st->ital = false;
                } else if (!strcmp(word, "uc") && has_num) {
                    st->uc = (int)num;
                } else if (!strcmp(word, "u") && has_num) {
                    unsigned cp = (unsigned)(num < 0 ? num + 65536 : num);
                    if (cp >= 0xd800 && cp < 0xe000)
                        cp = '?';
                    rtf_put(&c, st, cp);
                    if (!st->skip && !c.in_style_table)
                        c.skip_chars = st->uc;
                } else if (!strcmp(word, "ansicpg") && has_num) {
                    c.cp = num == 1250 ? k_cp1250 : num == 1251 ? k_cp1251 : num == 1253 ? k_cp1253 : num == 1254 ? k_cp1254 : num == 1257 ? k_cp1257 : NULL;
                } else if (!strcmp(word, "trowd")) {
                    if (!c.in_table) {
                        db_table_begin(db);
                        c.in_table = true;
                    }
                    c.row_pending_end = false;
                    db_row_begin(db);
                } else if (!strcmp(word, "cell")) {
                    if (c.in_table) {
                        db_para(db, 0, -1, true);
                        db_cell_end(db);
                        db_cell_begin(db);
                    }
                } else if (!strcmp(word, "row")) {
                    if (c.in_table) {
                        db_row_end(db);
                        c.row_pending_end = true;
                    }
                } else if (!strcmp(word, "f") && has_num) {
                    if (c.in_fonttbl)
                        c.cur_font_def = (int)num;
                    else
                        st->font = (int)num;
                } else if (!strcmp(word, "fcharset") && has_num) {
                    if (c.in_fonttbl && c.cur_font_def >= 0 && c.cur_font_def < 8192)
                        c.font_charset[c.cur_font_def] = (uint8_t)(num & 255);
                } else if (!strcmp(word, "fonttbl")) {
                    c.in_fonttbl = true;
                    c.fonttbl_depth = 1;
                    c.cur_font_def = -1;
                    st->skip = true;
                } else if (!strcmp(word, "stylesheet")) {
                    c.in_style_table = true;
                    c.style_depth = 1;
                } else if (!strcmp(word, "colortbl") || !strcmp(word, "info") || !strcmp(word, "pict") || !strcmp(word, "header") ||
                           !strcmp(word, "footer") || !strcmp(word, "headerl") || !strcmp(word, "headerr") || !strcmp(word, "footerl") || !strcmp(word, "footerr") ||
                           !strcmp(word, "listtable") || !strcmp(word, "listoverridetable") || !strcmp(word, "themedata") || !strcmp(word, "colorschememapping") ||
                           !strcmp(word, "latentstyles") || !strcmp(word, "datastore") || !strcmp(word, "rsidtbl") || !strcmp(word, "generator") ||
                           !strcmp(word, "fldinst") || !strcmp(word, "xmlnstbl") || !strcmp(word, "pgptbl") || !strcmp(word, "private") || !strcmp(word, "object") ||
                           !strcmp(word, "footnote") || !strcmp(word, "stylerestrictions") || !strcmp(word, "mmathPr") || !strcmp(word, "wgrffmtfilter")) {
                    st->skip = true;
                }
            } else {                                                         /* another control symbol */
                i++;
            }
        } else if (ch == '\r' || ch == '\n') {
            /* line breaks in the file mean nothing */
        } else {
            unsigned cp = (unsigned char)ch;
            if (cp >= 0x80)
                cp = c.cp && c.cp[cp - 0x80] ? c.cp[cp - 0x80] : cp1252_char(cp);
            rtf_put(&c, st, cp);
        }
    }
    if (c.in_table)
        db_table_end(db);
    db_para(db, 0, -1, true);
}

/* 0 = the document as Markdown in *md (malloc'd, `info` says what it was); -1 = not a document of these kinds; -2 = one that cannot be read (err). */
int xl_document(const char *path, char **md, size_t *mdlen, char *info, size_t infon, char *err, size_t errn)
{
    {                                                                       /* the old formats first: OLE2 (.doc, .ppt) and RTF */
        int ofd = open(path, O_RDONLY | O_CLOEXEC);
        uint8_t mg[8] = { 0 };
        bool got = ofd >= 0 && pread_all(ofd, mg, 8, 0);
        bool ole = got && !memcmp(mg, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8), rtf = got && !memcmp(mg, "{\\rtf", 5);
        if (ofd >= 0)
            close(ofd);
        if (ole || rtf) {
            struct docb ob = { 0 };
            char what2[64] = "";
            int res = -1;
            if (ole)
                res = ole_document(path, &ob, what2, sizeof(what2), err, errn);
            else {
                int fd2 = open(path, O_RDONLY | O_CLOEXEC);
                struct stat st2;
                if (fd2 >= 0 && fstat(fd2, &st2) == 0 && st2.st_size > 0 && st2.st_size <= (64 << 20)) {
                    char *rb = malloc((size_t)st2.st_size + 1);
                    if (rb && pread_all(fd2, rb, (size_t)st2.st_size, 0)) {
                        rb[st2.st_size] = 0;
                        rtf_convert(&ob, rb, (size_t)st2.st_size);
                        snprintf(what2, sizeof(what2), "Rich Text document");
                        res = 0;
                    }
                    free(rb);
                }
                if (fd2 >= 0)
                    close(fd2);
            }
            if (res == 0) {
                if (ob.full)
                    db_outs(&ob, "\n*(the rest of the document is not shown)*\n");
                *md = ob.out.s ? ob.out.s : strdup("");
                *mdlen = ob.out.n;
                ob.out.s = NULL;
                if (ob.slides)
                    snprintf(info, infon, "%s, %d slide%s", what2, ob.slides, ob.slides == 1 ? "" : "s");
                else
                    snprintf(info, infon, "%s, %d paragraph%s, %d table%s", what2, ob.paragraphs, ob.paragraphs == 1 ? "" : "s", ob.tables, ob.tables == 1 ? "" : "s");
            }
            db_free(&ob);
            if (res != -1 || rtf)
                return res;
        }
    }
    struct zip z;
    int zr = zip_open(&z, path, err, errn);
    if (zr < 0)
        return zr;
    size_t len;
    const char *why = NULL;
    struct docb db = { 0 };
    int result = -1;
    char *mt = zip_read(&z, "mimetype", 256, &len, &why);
    bool odt = mt && strstr(mt, "opendocument.text");
    bool odp = mt && strstr(mt, "opendocument.presentation");
    bool mt_epub = mt && strstr(mt, "epub+zip");
    char epubinfo[320] = "";
    free(mt);
    const char *what = NULL;
    if (zip_find(&z, "word/document.xml")) {
        what = "Word document";
        char *dx = zip_read(&z, "word/document.xml", PART_BIG, &len, &why);
        if (!dx) {
            seterr(err, errn, why ? why : "The document part cannot be read.");
            result = -2;
            goto done;
        }
        struct dstyle *st = calloc(512, sizeof(*st));
        int nst = 0;
        struct dnum *dn = calloc(1, sizeof(*dn));
        for (int i = 0; i < 256; i++)
            dn->num_abs[i] = -1;
        memset(dn->bullet, 1, sizeof(dn->bullet));
        size_t sl;
        char *sx = zip_read(&z, "word/styles.xml", PART_SMALL, &sl, &why);
        if (sx) {
            nst = parse_dstyles(sx, sl, st, 512);
            free(sx);
        }
        char *nx = zip_read(&z, "word/numbering.xml", PART_SMALL, &sl, &why);
        if (nx) {
            parse_dnum(nx, sl, dn);
            free(nx);
        }
        docx_convert(&db, dx, len, st, nst, dn);
        free(dx);
        free(st);
        free(dn);
        result = 0;
    } else if (zip_find(&z, "ppt/slides/slide1.xml")) {
        what = "Presentation";
        char **names = NULL;
        int nn = 0;
        for (int i = 0; i < z.n && nn < 500; i++) {
            const char *nm = z.e[i].name;
            size_t l = strlen(nm);
            if (!strncmp(nm, "ppt/slides/slide", 16) && l > 20 && !strcmp(nm + l - 4, ".xml") && nm[16] >= '0' && nm[16] <= '9') {
                names = realloc(names, (size_t)(nn + 1) * sizeof(char *));
                names[nn++] = z.e[i].name;
            }
        }
        qsort(names, (size_t)nn, sizeof(char *), cmp_slide);
        for (int i = 0; i < nn && !db.full; i++) {
            char *sx = zip_read(&z, names[i], PART_BIG, &len, &why);
            if (!sx)
                continue;
            db_slide(&db, slide_num(names[i]));
            pptx_convert(&db, sx, len);
            free(sx);
        }
        free(names);
        result = 0;
    } else if (zip_find(&z, "META-INF/container.xml") && mt_epub) {
        char title[200], author[120];
        int ch = 0;
        what = "EPUB book";
        if (epub_convert_book(&z, &db, title, sizeof(title), author, sizeof(author), &ch, &why) < 0) {
            seterr(err, errn, why ? why : "An EPUB whose reading order cannot be found.");
            result = -2;
            goto done;
        }
        snprintf(epubinfo, sizeof(epubinfo), "EPUB book%s%.80s%s%.60s, %d chapter%s", title[0] ? ", " : "", title, author[0] ? " - " : "", author, ch, ch == 1 ? "" : "s");
        result = 0;
    } else if ((odt || odp) && zip_find(&z, "content.xml")) {
        what = odp ? "Presentation" : "Text document";
        char *cx = zip_read(&z, "content.xml", PART_BIG, &len, &why);
        if (!cx) {
            seterr(err, errn, why ? why : "The document part cannot be read.");
            result = -2;
            goto done;
        }
        odf_convert(&db, cx, len, odp);
        free(cx);
        result = 0;
    }
    if (result == 0) {
        if (db.full)
            db_outs(&db, "\n*(the rest of the document is not shown)*\n");
        *md = db.out.s ? db.out.s : strdup("");
        *mdlen = db.out.n;
        db.out.s = NULL;
        if (epubinfo[0])
            snprintf(info, infon, "%s", epubinfo);
        else if (db.slides)
            snprintf(info, infon, "%s, %d slide%s", what, db.slides, db.slides == 1 ? "" : "s");
        else
            snprintf(info, infon, "%s, %d paragraph%s, %d table%s", what, db.paragraphs, db.paragraphs == 1 ? "" : "s", db.tables, db.tables == 1 ? "" : "s");
    }
done:
    db_free(&db);
    zip_close(&z);
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
    int r;
    if (ole) {
        r = xls_read(path, sheet, max_rows, max_cols, b, err, errn);
    } else {
        r = xlsx_read(path, sheet, max_rows, max_cols, b, err, errn);
        if (r == -1)
            r = ods_read(path, sheet, max_rows, max_cols, b, err, errn);          /* a ZIP with no xl/workbook.xml: maybe an OpenDocument sheet */
        if (r == -1)
            r = numbers_read(path, sheet, max_rows, max_cols, b, err, errn);      /* ... or an Apple Numbers package */
    }
    if (r == 0)
        b->kind = b->kind ? b->kind : "Excel";
    return r;
}
