#include "ttf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILE (64u << 20)            /* a font file bigger than this is refused */
#define MAX_POINTS 20000                /* per glyph, after compound glyphs are put together */
#define MAX_DEPTH 6                     /* compound glyphs inside compound glyphs */
#define MAX_RASTER 4096                 /* edge of the biggest scratch picture */

struct tab {
    uint32_t off, len;
};

struct pt {
    float x, y;
    uint8_t on, end;                    /* on the curve; the last point of its contour */
};

struct lm_ttf {
    uint8_t *d;
    size_t n;
    struct tab head, maxp, hhea, hmtx, loca, glyf, cmap, os2;
    int units, ascent, descent, xheight;
    int glyphs, hmetrics, loca_long;
    uint32_t cmap4, cmap12;             /* offsets of the chosen subtables in the file, 0 = none */
    struct pt *pts;                     /* the glyph being painted */
    int npts, cap;
    float *acc;                         /* the rasteriser's accumulation buffer */
    size_t acc_cap;
};

/* ---- reading ------------------------------------------------------------------------------------------------- */

static bool has(const struct lm_ttf *f, uint64_t off, uint64_t len)
{
    return off <= f->n && len <= f->n - off;
}

static uint32_t u16(const struct lm_ttf *f, uint64_t off)
{
    return has(f, off, 2) ? (uint32_t)f->d[off] << 8 | f->d[off + 1] : 0;
}

static int s16(const struct lm_ttf *f, uint64_t off)
{
    return (int16_t)u16(f, off);
}

static uint32_t u32(const struct lm_ttf *f, uint64_t off)
{
    return has(f, off, 4) ? (uint32_t)f->d[off] << 24 | (uint32_t)f->d[off + 1] << 16 | (uint32_t)f->d[off + 2] << 8 | f->d[off + 3] : 0;
}

static bool find_table(const struct lm_ttf *f, uint32_t base, const char *tag, struct tab *t)
{
    uint32_t n = u16(f, (uint64_t)base + 4);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t e = (uint64_t)base + 12 + 16ull * i;
        if (!has(f, e, 16))
            return false;
        if (memcmp(f->d + e, tag, 4))
            continue;
        t->off = u32(f, e + 8);
        t->len = u32(f, e + 12);
        return has(f, t->off, t->len);
    }
    return false;
}

static void pick_cmap(struct lm_ttf *f)
{
    uint32_t n = u16(f, (uint64_t)f->cmap.off + 2);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t e = (uint64_t)f->cmap.off + 4 + 8ull * i;
        if (!has(f, e, 8))
            break;
        uint32_t plat = u16(f, e), enc = u16(f, e + 2);
        uint64_t sub = (uint64_t)f->cmap.off + u32(f, e + 4);
        if (!has(f, sub, 16))
            continue;
        bool unicode = plat == 0 || (plat == 3 && (enc == 1 || enc == 10));
        if (!unicode)
            continue;
        uint32_t fmt = u16(f, sub);
        if (fmt == 12 && !f->cmap12)
            f->cmap12 = (uint32_t)sub;
        else if (fmt == 4 && !f->cmap4)
            f->cmap4 = (uint32_t)sub;
    }
}

struct lm_ttf *lm_ttf_open(const char *path, const char **why)
{
    const char *dummy;
    if (!why)
        why = &dummy;
    *why = "cannot be read";
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    struct lm_ttf *f = calloc(1, sizeof(*f));
    long size = -1;
    if (f && fseek(fp, 0, SEEK_END) == 0)
        size = ftell(fp);
    if (size < 12 || (unsigned long)size > MAX_FILE || fseek(fp, 0, SEEK_SET) != 0) {
        *why = size > 0 ? "too big or too small to be a font" : "cannot be read";
        goto fail;
    }
    f->d = malloc((size_t)size);
    if (!f->d || fread(f->d, 1, (size_t)size, fp) != (size_t)size)
        goto fail;
    f->n = (size_t)size;
    fclose(fp);
    fp = NULL;

    uint32_t base = 0;
    if (!memcmp(f->d, "ttcf", 4))                       /* a collection: its first font */
        base = u32(f, 12);
    uint32_t ver = u32(f, base);
    if (ver == 0x4f54544f) {                            /* "OTTO" */
        *why = "has CFF outlines (.otf), only TrueType outlines are read";
        goto fail;
    }
    if (ver != 0x00010000 && ver != 0x74727565) {       /* 1.0 or "true" */
        *why = "is not a TrueType font";
        goto fail;
    }
    *why = "lacks a table a TrueType font needs";
    if (!find_table(f, base, "head", &f->head) || !find_table(f, base, "maxp", &f->maxp) ||
        !find_table(f, base, "hhea", &f->hhea) || !find_table(f, base, "hmtx", &f->hmtx) ||
        !find_table(f, base, "loca", &f->loca) || !find_table(f, base, "glyf", &f->glyf) ||
        !find_table(f, base, "cmap", &f->cmap))
        goto fail;
    f->units = (int)u16(f, (uint64_t)f->head.off + 18);
    f->loca_long = s16(f, (uint64_t)f->head.off + 50) != 0;
    f->glyphs = (int)u16(f, (uint64_t)f->maxp.off + 4);
    f->ascent = s16(f, (uint64_t)f->hhea.off + 4);
    f->descent = s16(f, (uint64_t)f->hhea.off + 6);
    f->hmetrics = (int)u16(f, (uint64_t)f->hhea.off + 34);
    if (f->units < 16 || f->units > 16384 || f->glyphs < 1 || f->ascent <= 0 || f->descent > 0 || f->ascent - f->descent < f->units / 4) {
        *why = "has metrics that make no sense";
        goto fail;
    }
    if (find_table(f, base, "OS/2", &f->os2) && u16(f, f->os2.off) >= 2 && f->os2.len >= 88) {
        int xh = s16(f, (uint64_t)f->os2.off + 86);
        if (xh > f->units / 8 && xh < f->units)
            f->xheight = xh;
    }
    pick_cmap(f);
    if (!f->cmap4 && !f->cmap12) {
        *why = "has no Unicode character map";
        goto fail;
    }
    *why = NULL;
    return f;

fail:
    if (fp)
        fclose(fp);
    lm_ttf_close(f);
    return NULL;
}

void lm_ttf_close(struct lm_ttf *f)
{
    if (!f)
        return;
    free(f->d);
    free(f->pts);
    free(f->acc);
    free(f);
}

int lm_ttf_units(const struct lm_ttf *f) { return f->units; }
int lm_ttf_ascent(const struct lm_ttf *f) { return f->ascent; }
int lm_ttf_descent(const struct lm_ttf *f) { return f->descent; }
int lm_ttf_xheight(const struct lm_ttf *f) { return f->xheight; }

int lm_ttf_advance(const struct lm_ttf *f, int glyph)
{
    if (glyph < 0 || glyph >= f->glyphs || f->hmetrics < 1)
        return 0;
    int i = glyph < f->hmetrics ? glyph : f->hmetrics - 1;
    return (int)u16(f, (uint64_t)f->hmtx.off + 4ull * (unsigned)i);
}

static int cmap4_lookup(const struct lm_ttf *f, uint32_t cp)
{
    if (cp > 0xffff)
        return 0;
    uint64_t t = f->cmap4;
    uint32_t segs = u16(f, t + 6) / 2;
    uint64_t ends = t + 14, starts = ends + 2ull * segs + 2, deltas = starts + 2ull * segs, ranges = deltas + 2ull * segs;
    if (!segs || !has(f, ranges, 2ull * segs))
        return 0;
    uint32_t lo = 0, hi = segs;                         /* the first segment whose end is >= cp */
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (u16(f, ends + 2ull * mid) < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= segs)
        return 0;
    uint32_t start = u16(f, starts + 2ull * lo);
    if (cp < start)
        return 0;
    uint32_t delta = u16(f, deltas + 2ull * lo), range = u16(f, ranges + 2ull * lo);
    if (!range)
        return (int)((cp + delta) & 0xffff);
    uint64_t at = ranges + 2ull * lo + range + 2ull * (cp - start);
    uint32_t g = u16(f, at);
    return g ? (int)((g + delta) & 0xffff) : 0;
}

static int cmap12_lookup(const struct lm_ttf *f, uint32_t cp)
{
    uint64_t t = f->cmap12;
    uint32_t n = u32(f, t + 12);
    if (!has(f, t + 16, 12ull * n))
        return 0;
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        uint64_t g = t + 16 + 12ull * mid;
        if (u32(f, g + 4) < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= n)
        return 0;
    uint64_t g = t + 16 + 12ull * lo;
    uint32_t start = u32(f, g);
    if (cp < start)
        return 0;
    uint64_t id = (uint64_t)u32(f, g + 8) + (cp - start);
    return id < (uint64_t)f->glyphs ? (int)id : 0;
}

int lm_ttf_glyph(const struct lm_ttf *f, uint32_t cp)
{
    int g = f->cmap12 ? cmap12_lookup(f, cp) : 0;
    if (!g && f->cmap4)
        g = cmap4_lookup(f, cp);
    return g > 0 && g < f->glyphs ? g : 0;
}

/* ---- outlines ------------------------------------------------------------------------------------------------ */

/* Where a glyph's data is; false = no outline (a space) or out of the file. */
static bool glyph_data(const struct lm_ttf *f, int glyph, uint64_t *off, uint64_t *len)
{
    if (glyph < 0 || glyph >= f->glyphs)
        return false;
    uint64_t a, b;
    if (f->loca_long) {
        if (f->loca.len < 4ull * ((unsigned)glyph + 2))
            return false;
        a = u32(f, (uint64_t)f->loca.off + 4ull * (unsigned)glyph);
        b = u32(f, (uint64_t)f->loca.off + 4ull * (unsigned)glyph + 4);
    } else {
        if (f->loca.len < 2ull * ((unsigned)glyph + 2))
            return false;
        a = 2ull * u16(f, (uint64_t)f->loca.off + 2ull * (unsigned)glyph);
        b = 2ull * u16(f, (uint64_t)f->loca.off + 2ull * (unsigned)glyph + 2);
    }
    if (b <= a || b > f->glyf.len || b - a < 10)
        return false;
    *off = f->glyf.off + a;
    *len = b - a;
    return true;
}

static bool add_point(struct lm_ttf *f, float x, float y, bool on, bool end)
{
    if (f->npts >= MAX_POINTS)
        return false;
    if (f->npts == f->cap) {
        int cap = f->cap ? f->cap * 2 : 256;
        struct pt *p = realloc(f->pts, (size_t)cap * sizeof(*p));
        if (!p)
            return false;
        f->pts = p;
        f->cap = cap;
    }
    f->pts[f->npts++] = (struct pt){ x, y, on, end };
    return true;
}

/* Appends the glyph's points in font units, put through the matrix m (xx xy yx yy dx dy). */
static bool load_glyph(struct lm_ttf *f, int glyph, const float m[6], int depth)
{
    uint64_t g, len;
    if (!glyph_data(f, glyph, &g, &len))
        return true;                                    /* nothing to draw is no error */
    uint64_t end = g + len;
    int contours = s16(f, g);
    uint64_t p = g + 10;

    if (contours < 0) {                                 /* compound: other glyphs, moved and scaled */
        if (depth >= MAX_DEPTH)
            return false;
        for (int guard = 0; guard < 64; guard++) {
            if (p + 4 > end)
                return false;
            uint32_t flags = u16(f, p), child = u16(f, p + 2);
            p += 4;
            float dx = 0, dy = 0;
            if (flags & 1) {                            /* the two arguments are words */
                if (p + 4 > end)
                    return false;
                if (flags & 2) { dx = (float)s16(f, p); dy = (float)s16(f, p + 2); }
                p += 4;
            } else {
                if (p + 2 > end)
                    return false;
                if (flags & 2) { dx = (float)(int8_t)f->d[p]; dy = (float)(int8_t)f->d[p + 1]; }
                p += 2;
            }                                           /* (arguments that are point numbers are not followed: offset 0) */
            float a = 1, b = 0, c = 0, d = 1;
            if (flags & 0x08) {
                if (p + 2 > end) return false;
                a = d = (float)s16(f, p) / 16384.0f;
                p += 2;
            } else if (flags & 0x40) {
                if (p + 4 > end) return false;
                a = (float)s16(f, p) / 16384.0f;
                d = (float)s16(f, p + 2) / 16384.0f;
                p += 4;
            } else if (flags & 0x80) {
                if (p + 8 > end) return false;
                a = (float)s16(f, p) / 16384.0f;
                b = (float)s16(f, p + 2) / 16384.0f;
                c = (float)s16(f, p + 4) / 16384.0f;
                d = (float)s16(f, p + 6) / 16384.0f;
                p += 8;
            }
            /* child point (x, y) -> (a x + c y + dx, b x + d y + dy), then through m */
            float cm[6] = {
                m[0] * a + m[2] * b, m[1] * a + m[3] * b,
                m[0] * c + m[2] * d, m[1] * c + m[3] * d,
                m[0] * dx + m[2] * dy + m[4], m[1] * dx + m[3] * dy + m[5],
            };
            if (!load_glyph(f, (int)child, cm, depth + 1))
                return false;
            if (!(flags & 0x20))
                break;
        }
        return true;
    }

    if (contours == 0)
        return true;
    if (p + 2ull * (unsigned)contours + 2 > end)
        return false;
    uint64_t ends = p;
    int npts = (int)u16(f, ends + 2ull * (unsigned)(contours - 1)) + 1;
    if (npts < 1 || npts > MAX_POINTS)
        return false;
    p += 2ull * (unsigned)contours;
    p += 2 + u16(f, p);                                 /* the hinting program is skipped */
    if (p > end)
        return false;

    int first = f->npts;
    for (int i = 0; i < npts; i++)
        if (!add_point(f, 0, 0, false, false))
            return false;
    struct pt *pts = f->pts + first;

    uint8_t *flag = malloc((size_t)npts);
    if (!flag)
        return false;
    bool ok = false;
    for (int i = 0; i < npts;) {
        if (p >= end)
            goto done;
        uint8_t fl = f->d[p++];
        int rep = 1;
        if (fl & 8) {
            if (p >= end)
                goto done;
            rep += f->d[p++];
        }
        while (rep-- > 0 && i < npts)
            flag[i++] = fl;
    }
    int v = 0;
    for (int i = 0; i < npts; i++) {                    /* x coordinates: deltas */
        if (flag[i] & 2) {
            if (p >= end) goto done;
            int dv = f->d[p++];
            v += (flag[i] & 0x10) ? dv : -dv;
        } else if (!(flag[i] & 0x10)) {
            if (p + 2 > end) goto done;
            v += s16(f, p);
            p += 2;
        }
        pts[i].x = (float)v;
    }
    v = 0;
    for (int i = 0; i < npts; i++) {
        if (flag[i] & 4) {
            if (p >= end) goto done;
            int dv = f->d[p++];
            v += (flag[i] & 0x20) ? dv : -dv;
        } else if (!(flag[i] & 0x20)) {
            if (p + 2 > end) goto done;
            v += s16(f, p);
            p += 2;
        }
        pts[i].y = (float)v;
    }
    int prev_end = -1;
    for (int c = 0; c < contours; c++) {
        int e = (int)u16(f, ends + 2ull * (unsigned)c);
        if (e <= prev_end || e >= npts)
            goto done;
        pts[e].end = 1;
        prev_end = e;
    }
    for (int i = 0; i < npts; i++) {
        float x = pts[i].x, y = pts[i].y;
        pts[i].x = m[0] * x + m[2] * y + m[4];
        pts[i].y = m[1] * x + m[3] * y + m[5];
        pts[i].on = flag[i] & 1;
    }
    ok = true;
done:
    free(flag);
    if (!ok)
        f->npts = first;
    return ok;
}

/* ---- rasteriser ---------------------------------------------------------------------------------------------- */

/*
 * Signed area accumulation: every edge adds, to the pixels of each row it crosses, the part of the pixel that lies to
 * its right (times the direction it runs in); a running sum along the row then is the coverage.
 * All x are in [0, w - 1), all y are clipped to [0, h) here.
 */
static void edge(float *acc, int w, int h, float x0, float y0, float x1, float y1)
{
    if (y0 == y1)
        return;
    float dir = 1.0f;
    if (y0 > y1) {
        float t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
        dir = -1.0f;
    }
    if (y1 <= 0 || y0 >= (float)h)
        return;
    float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    if (y0 < 0) {
        x -= y0 * dxdy;
        y0 = 0;
    }
    int ylast = (int)ceilf(y1);
    if (ylast > h)
        ylast = h;
    for (int y = (int)y0; y < ylast; y++) {
        float *row = acc + (size_t)y * (size_t)w;
        float top = (float)y > y0 ? (float)y : y0, bot = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        float dy = bot - top;
        if (dy <= 0)
            continue;
        float xnext = x + dxdy * dy;
        float d = dy * dir;
        float xa = x < xnext ? x : xnext, xb = x < xnext ? xnext : x;
        float xaf = floorf(xa);
        int xi = (int)xaf, xj = (int)ceilf(xb);
        if (xi < 0) xi = 0;
        if (xj > w - 1) xj = w - 1;
        if (xj <= xi + 1) {                             /* inside one pixel column */
            float xmf = 0.5f * (x + xnext) - xaf;
            row[xi] += d - d * xmf;
            row[xi + 1] += d * xmf;
        } else {
            float s = 1.0f / (xb - xa);
            float xf0 = xa - xaf, a0 = 0.5f * s * (1.0f - xf0) * (1.0f - xf0);
            float xf1 = xb - (float)xj + 1.0f, am = 0.5f * s * xf1 * xf1;
            row[xi] += d * a0;
            if (xj == xi + 2) {
                row[xi + 1] += d * (1.0f - a0 - am);
            } else {
                float a1 = s * (1.5f - xf0);
                row[xi + 1] += d * (a1 - a0);
                for (int k = xi + 2; k < xj - 1; k++)
                    row[k] += d * s;
                float a2 = a1 + (float)(xj - xi - 3) * s;
                row[xj - 1] += d * (1.0f - a2 - am);
            }
            row[xj] += d * am;
        }
        x = xnext;
    }
}

static void quad(float *acc, int w, int h, float x0, float y0, float cx, float cy, float x1, float y1)
{
    float ddx = x0 - 2 * cx + x1, ddy = y0 - 2 * cy + y1;
    float dev = sqrtf(ddx * ddx + ddy * ddy);           /* how far the curve bends away from its chord, in pixels */
    int n = 1 + (int)sqrtf(dev * 1.5f);
    if (n > 24)
        n = 24;
    float px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1 - t;
        float x = u * u * x0 + 2 * u * t * cx + t * t * x1;
        float y = u * u * y0 + 2 * u * t * cy + t * t * y1;
        edge(acc, w, h, px, py, x, y);
        px = x;
        py = y;
    }
}

static void contour(float *acc, int w, int h, const struct pt *p, int n)
{
    if (n < 2)
        return;
    /* the starting point: an on-curve point, else the middle between two control points */
    float sx, sy;
    int first;
    if (p[0].on) {
        sx = p[0].x; sy = p[0].y; first = 1;
    } else if (p[n - 1].on) {
        sx = p[n - 1].x; sy = p[n - 1].y; first = 0;
    } else {
        sx = (p[0].x + p[n - 1].x) / 2; sy = (p[0].y + p[n - 1].y) / 2; first = 0;
    }
    float x = sx, y = sy, cx = 0, cy = 0;
    bool ctrl = false;
    for (int k = 0; k < n; k++) {
        const struct pt *q = &p[(first + k) % n];
        if (q->on) {
            if (ctrl)
                quad(acc, w, h, x, y, cx, cy, q->x, q->y);
            else
                edge(acc, w, h, x, y, q->x, q->y);
            x = q->x; y = q->y;
            ctrl = false;
        } else {
            if (ctrl) {
                float mx = (cx + q->x) / 2, my = (cy + q->y) / 2;
                quad(acc, w, h, x, y, cx, cy, mx, my);
                x = mx; y = my;
            }
            cx = q->x; cy = q->y;
            ctrl = true;
        }
    }
    if (ctrl)
        quad(acc, w, h, x, y, cx, cy, sx, sy);
    else
        edge(acc, w, h, x, y, sx, sy);
}

bool lm_ttf_render(struct lm_ttf *f, int glyph, double sx, double sy, double ox, double baseline,
                   uint8_t *out, int w, int h)
{
    if (w <= 0 || h <= 0)
        return false;
    memset(out, 0, (size_t)w * (size_t)h);
    f->npts = 0;
    const float id[6] = { 1, 0, 0, 1, 0, 0 };
    if (!load_glyph(f, glyph, id, 0) || f->npts < 2)
        return false;

    /* to pixels (y down), and the box of all points - the curves stay inside it */
    float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
    for (int i = 0; i < f->npts; i++) {
        struct pt *p = &f->pts[i];
        p->x = (float)(ox + p->x * sx);
        p->y = (float)(baseline - p->y * sy);
        if (!(p->x > -1e6f && p->x < 1e6f && p->y > -1e6f && p->y < 1e6f))
            return false;
        if (p->x < minx) minx = p->x;
        if (p->x > maxx) maxx = p->x;
        if (p->y < miny) miny = p->y;
        if (p->y > maxy) maxy = p->y;
    }
    int bx = (int)floorf(minx), by = (int)floorf(miny);
    int bw = (int)ceilf(maxx) - bx + 2, bh = (int)ceilf(maxy) - by + 1;
    if (bw < 2 || bh < 1 || bw > MAX_RASTER || bh > MAX_RASTER)
        return false;
    size_t need = (size_t)bw * (size_t)bh;
    if (need > f->acc_cap) {
        float *a = realloc(f->acc, need * sizeof(float));
        if (!a)
            return false;
        f->acc = a;
        f->acc_cap = need;
    }
    memset(f->acc, 0, need * sizeof(float));
    for (int i = 0; i < f->npts; i++) {
        f->pts[i].x -= (float)bx;
        f->pts[i].y -= (float)by;
    }
    for (int i = 0, start = 0; i < f->npts; i++)
        if (f->pts[i].end) {
            contour(f->acc, bw, bh, f->pts + start, i - start + 1);
            start = i + 1;
        }

    bool any = false;
    for (int y = 0; y < bh; y++) {
        int oy = y + by;
        if (oy < 0 || oy >= h)
            continue;
        const float *row = f->acc + (size_t)y * (size_t)bw;
        float sum = 0;
        for (int x = 0; x < bw; x++) {
            sum += row[x];
            int oxp = x + bx;
            if (oxp < 0 || oxp >= w)
                continue;
            float c = fabsf(sum);
            if (c > 1.0f)
                c = 1.0f;
            int v = (int)(c * 255.0f + 0.5f);
            if (v) {
                out[(size_t)oy * (size_t)w + (size_t)oxp] = (uint8_t)v;
                any = true;
            }
        }
    }
    return any;
}
