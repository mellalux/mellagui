/* TrueType glyph rasterizer, see ttf.h. */
#include "ttf.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct gcache { uint32_t cp; int px; struct ttf_glyph g; bool used; };

struct ttf {
    uint8_t *d;
    size_t n;
    uint32_t glyf, loca, head, hhea, hmtx, cmap, maxp;
    int units, nglyphs, nhm, loca_long, ascent, descent;
    uint32_t cmap_off;              /* chosen subtable */
    int cmap_fmt;
    struct gcache *cache;
    size_t ncache, capcache;
};

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static int16_t s16(const uint8_t *p) { return (int16_t)u16(p); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

struct ttf *ttf_load(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 64 || n > (32l << 20)) {
        fclose(fp);
        return NULL;
    }
    struct ttf *f = calloc(1, sizeof(*f));
    f->d = malloc((size_t)n);
    if (!f->d || fread(f->d, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        free(f->d);
        free(f);
        return NULL;
    }
    fclose(fp);
    f->n = (size_t)n;
    const uint8_t *d = f->d;
    uint32_t tag0 = u32(d);
    if (tag0 != 0x00010000 && tag0 != 0x74727565) {
        free(f->d);
        free(f);
        return NULL;
    }
    int nt = u16(d + 4);
    for (int i = 0; i < nt && 12 + (size_t)i * 16 + 16 <= f->n; i++) {
        const uint8_t *r = d + 12 + i * 16;
        uint32_t off = u32(r + 8);
        if (!memcmp(r, "glyf", 4)) f->glyf = off;
        else if (!memcmp(r, "loca", 4)) f->loca = off;
        else if (!memcmp(r, "head", 4)) f->head = off;
        else if (!memcmp(r, "hhea", 4)) f->hhea = off;
        else if (!memcmp(r, "hmtx", 4)) f->hmtx = off;
        else if (!memcmp(r, "cmap", 4)) f->cmap = off;
        else if (!memcmp(r, "maxp", 4)) f->maxp = off;
    }
    if (!f->glyf || !f->loca || !f->head || !f->hhea || !f->hmtx || !f->cmap || !f->maxp || f->head + 54 > f->n ||
        f->hhea + 36 > f->n || f->maxp + 6 > f->n) {
        free(f->d);
        free(f);
        return NULL;
    }
    f->units = u16(d + f->head + 18);
    f->loca_long = s16(d + f->head + 50);
    f->nglyphs = u16(d + f->maxp + 4);
    f->ascent = s16(d + f->hhea + 4);
    f->descent = -s16(d + f->hhea + 6);
    f->nhm = u16(d + f->hhea + 34);
    if (f->units <= 0 || f->cmap + 4 > f->n) {
        free(f->d);
        free(f);
        return NULL;
    }
    int ns = u16(d + f->cmap + 2);
    for (int i = 0; i < ns && f->cmap + 4 + (size_t)i * 8 + 8 <= f->n; i++) {
        const uint8_t *r = d + f->cmap + 4 + i * 8;
        int plat = u16(r), enc = u16(r + 2);
        uint32_t off = f->cmap + u32(r + 4);
        if (off + 4 > f->n)
            continue;
        int fmt = u16(d + off);
        bool unicode = plat == 0 || (plat == 3 && (enc == 1 || enc == 10));
        if (unicode && (fmt == 12 || (fmt == 4 && f->cmap_fmt != 12))) {
            f->cmap_off = off;
            f->cmap_fmt = fmt;
        }
    }
    if (!f->cmap_off) {
        free(f->d);
        free(f);
        return NULL;
    }
    return f;
}

static int glyph_index(const struct ttf *f, uint32_t cp)
{
    const uint8_t *d = f->d + f->cmap_off;
    size_t room = f->n - f->cmap_off;
    if (f->cmap_fmt == 12) {
        if (room < 16) return 0;
        uint32_t ng = u32(d + 12);
        for (uint32_t i = 0; i < ng && 16 + (size_t)i * 12 + 12 <= room; i++) {
            const uint8_t *g = d + 16 + i * 12;
            if (cp >= u32(g) && cp <= u32(g + 4))
                return (int)(u32(g + 8) + (cp - u32(g)));
        }
        return 0;
    }
    if (cp > 0xffff || room < 14) return 0;
    int segx2 = u16(d + 6);
    int seg = segx2 / 2;
    if (16 + (size_t)segx2 * 4 > room) return 0;
    const uint8_t *endc = d + 14, *startc = d + 16 + segx2, *delta = d + 16 + segx2 * 2, *ro = d + 16 + segx2 * 3;
    for (int i = 0; i < seg; i++) {
        if (cp > u16(endc + i * 2)) continue;
        if (cp < u16(startc + i * 2)) return 0;
        int off = u16(ro + i * 2);
        if (!off) return (int)((cp + u16(delta + i * 2)) & 0xffff);
        const uint8_t *gp = ro + i * 2 + off + (cp - u16(startc + i * 2)) * 2;
        if (gp + 2 > f->d + f->n) return 0;
        int g = u16(gp);
        return g ? (int)((g + u16(delta + i * 2)) & 0xffff) : 0;
    }
    return 0;
}

static int hadv(const struct ttf *f, int gi)
{
    if (f->nhm <= 0) return 0;
    int i = gi < f->nhm ? gi : f->nhm - 1;
    if (f->hmtx + (size_t)i * 4 + 2 > f->n) return 0;
    return u16(f->d + f->hmtx + i * 4);
}

/* ---- outlines ------------------------------------------------------------------------------------------------- */

struct pt { float x, y; bool on; };
struct outline { struct pt *p; int n, cap; int *ends; int nend, capend; };

static void ol_pt(struct outline *o, float x, float y, bool on)
{
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 128;
        o->p = realloc(o->p, sizeof(*o->p) * (size_t)o->cap);
        if (!o->p)
            exit(1);
    }
    o->p[o->n++] = (struct pt){ x, y, on };
}

static void ol_end(struct outline *o)
{
    if (o->nend == o->capend) {
        o->capend = o->capend ? o->capend * 2 : 16;
        o->ends = realloc(o->ends, sizeof(int) * (size_t)o->capend);
        if (!o->ends)
            exit(1);
    }
    o->ends[o->nend++] = o->n - 1;
}

/* Append glyph `gi` transformed by [m0 m1; m2 m3] + (tx, ty) (font units). */
static bool load_glyph(const struct ttf *f, int gi, struct outline *o, const float m[4], float tx, float ty, int depth)
{
    if (gi < 0 || gi >= f->nglyphs || depth > 6)
        return false;
    size_t loca = f->loca + (size_t)gi * (f->loca_long ? 4 : 2);
    if (loca + (f->loca_long ? 8 : 4) > f->n)
        return false;
    uint32_t o0 = f->loca_long ? u32(f->d + loca) : (uint32_t)u16(f->d + loca) * 2;
    uint32_t o1 = f->loca_long ? u32(f->d + loca + 4) : (uint32_t)u16(f->d + loca + 2) * 2;
    if (o1 <= o0)
        return true;                                            /* empty glyph (space) */
    if (f->glyf + o1 > f->n || o1 - o0 < 10)
        return false;
    const uint8_t *g = f->d + f->glyf + o0, *end = f->d + f->glyf + o1;
    int nc = s16(g);
    const uint8_t *p = g + 10;
    if (nc >= 0) {
        if (p + nc * 2 + 2 > end)
            return false;
        int npts = nc ? u16(p + (nc - 1) * 2) + 1 : 0;
        const uint8_t *ends = p;
        p += nc * 2;
        p += 2 + u16(p);
        if (npts > 20000 || p > end)
            return false;
        uint8_t *flags = malloc((size_t)npts + 1);
        float *xs = malloc(sizeof(float) * ((size_t)npts + 1)), *ys = malloc(sizeof(float) * ((size_t)npts + 1));
        bool ok = true;
        int i = 0;
        while (i < npts && ok) {
            if (p >= end) { ok = false; break; }
            uint8_t fl = *p++;
            flags[i++] = fl;
            if (fl & 8) {
                if (p >= end) { ok = false; break; }
                int rep = *p++;
                while (rep-- && i < npts)
                    flags[i++] = fl;
            }
        }
        int v = 0;
        for (i = 0; i < npts && ok; i++) {
            if (flags[i] & 2) { if (p >= end) { ok = false; break; } int dx = *p++; v += (flags[i] & 16) ? dx : -dx; }
            else if (!(flags[i] & 16)) { if (p + 2 > end) { ok = false; break; } v += s16(p); p += 2; }
            xs[i] = (float)v;
        }
        v = 0;
        for (i = 0; i < npts && ok; i++) {
            if (flags[i] & 4) { if (p >= end) { ok = false; break; } int dy = *p++; v += (flags[i] & 32) ? dy : -dy; }
            else if (!(flags[i] & 32)) { if (p + 2 > end) { ok = false; break; } v += s16(p); p += 2; }
            ys[i] = (float)v;
        }
        if (ok) {
            int start = 0;
            for (int ci = 0; ci < nc; ci++) {
                int last = u16(ends + ci * 2);
                for (i = start; i <= last && i < npts; i++)
                    ol_pt(o, m[0] * xs[i] + m[2] * ys[i] + tx, m[1] * xs[i] + m[3] * ys[i] + ty, flags[i] & 1);
                ol_end(o);
                start = last + 1;
            }
        }
        free(flags);
        free(xs);
        free(ys);
        return ok;
    }
    for (;;) {                                                  /* composite glyph (accented letters) */
        if (p + 4 > end)
            return false;
        int fl = u16(p), sub = u16(p + 2);
        p += 4;
        float dx = 0, dy = 0;
        if (fl & 1) {
            if (p + 4 > end) return false;
            if (fl & 2) { dx = s16(p); dy = s16(p + 2); }
            p += 4;
        } else {
            if (p + 2 > end) return false;
            if (fl & 2) { dx = (int8_t)p[0]; dy = (int8_t)p[1]; }
            p += 2;
        }
        float c[4] = { 1, 0, 0, 1 };
        if (fl & 8) { if (p + 2 > end) return false; c[0] = c[3] = s16(p) / 16384.0f; p += 2; }
        else if (fl & 0x40) { if (p + 4 > end) return false; c[0] = s16(p) / 16384.0f; c[3] = s16(p + 2) / 16384.0f; p += 4; }
        else if (fl & 0x80) { if (p + 8 > end) return false; c[0] = s16(p) / 16384.0f; c[1] = s16(p + 2) / 16384.0f; c[2] = s16(p + 4) / 16384.0f; c[3] = s16(p + 6) / 16384.0f; p += 8; }
        float nm[4] = { m[0] * c[0] + m[2] * c[1], m[1] * c[0] + m[3] * c[1], m[0] * c[2] + m[2] * c[3], m[1] * c[2] + m[3] * c[3] };
        if (!load_glyph(f, sub, o, nm, m[0] * dx + m[2] * dy + tx, m[1] * dx + m[3] * dy + ty, depth + 1))
            return false;
        if (!(fl & 0x20))
            break;
    }
    return true;
}

/* ---- rasterizer (exact area coverage, accumulation buffer) ------------------------------------------------------ */

struct raster { float *a; int w, h; };

static void draw_line(struct raster *r, float x0, float y0, float x1, float y1)
{
    if (y0 == y1)
        return;
    float dir = 1;
    if (y0 > y1) { float t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; dir = -1; }
    float dxdy = (x1 - x0) / (y1 - y0), x = x0;
    int ys = y0 < 0 ? 0 : (int)floorf(y0);
    if (y0 < 0)
        x -= y0 * dxdy;
    for (int y = ys; y < r->h && (float)y < y1; y++) {
        float dy = fminf((float)(y + 1), y1) - fmaxf((float)y, y0);
        float xn = x + dxdy * dy, d = dy * dir;
        float xa = x < xn ? x : xn, xb = x < xn ? xn : x;
        float xaf = floorf(xa);
        int xai = (int)xaf;
        float xbc = ceilf(xb);
        int xbi = (int)xbc;
        float *row = r->a + (size_t)y * (size_t)r->w;
        if (xbi <= xai + 1) {
            float xmf = 0.5f * (x + xn) - xaf;
            if (xai >= 0 && xai < r->w) row[xai] += d - d * xmf;
            if (xai + 1 >= 0 && xai + 1 < r->w) row[xai + 1] += d * xmf;
        } else {
            float sl = 1.0f / (xb - xa), x0f = xa - xaf, a0 = 0.5f * sl * (1 - x0f) * (1 - x0f);
            float x1f = xb - xbc + 1, am = 0.5f * sl * x1f * x1f;
            if (xai >= 0 && xai < r->w) row[xai] += d * a0;
            if (xbi == xai + 2) {
                if (xai + 1 >= 0 && xai + 1 < r->w) row[xai + 1] += d * (1 - a0 - am);
            } else {
                float a1 = sl * (1.5f - x0f);
                if (xai + 1 >= 0 && xai + 1 < r->w) row[xai + 1] += d * (a1 - a0);
                for (int xi = xai + 2; xi < xbi - 1; xi++)
                    if (xi >= 0 && xi < r->w) row[xi] += d * sl;
                float a2 = a1 + (float)(xbi - xai - 3) * sl;
                if (xbi - 1 >= 0 && xbi - 1 < r->w) row[xbi - 1] += d * (1 - a2 - am);
            }
            if (xbi >= 0 && xbi < r->w) row[xbi] += d * am;
        }
        x = xn;
    }
}

static void draw_quad(struct raster *r, float x0, float y0, float x1, float y1, float x2, float y2)
{
    float dd = fabsf(x0 - 2 * x1 + x2) + fabsf(y0 - 2 * y1 + y2);
    int n = (int)ceilf(sqrtf(dd) * 1.2f) + 1;
    if (n > 24) n = 24;
    float px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1 - t;
        float x = u * u * x0 + 2 * u * t * x1 + t * t * x2, y = u * u * y0 + 2 * u * t * y1 + t * t * y2;
        draw_line(r, px, py, x, y);
        px = x;
        py = y;
    }
}

static void fill_outline(struct raster *r, const struct outline *o, float ox, float oy)
{
    int start = 0;
    for (int c = 0; c < o->nend; c++) {
        int last = o->ends[c], n = last - start + 1;
        if (n >= 2) {
            const struct pt *P = o->p + start;
            /* find a starting on-curve point (or the midpoint of two off points) */
            int first = -1;
            for (int i = 0; i < n; i++)
                if (P[i].on) { first = i; break; }
            float sx, sy;
            int idx;
            if (first >= 0) { sx = P[first].x; sy = P[first].y; idx = first + 1; }
            else { sx = (P[0].x + P[1].x) / 2; sy = (P[0].y + P[1].y) / 2; first = 0; idx = 1; }
            float cx = sx, cy = sy;
            bool have_off = false;
            float ofx = 0, ofy = 0;
            int cnt = P[first].on ? n - 1 : n;
            for (int k = 0; k < cnt; k++, idx++) {
                const struct pt *q = &P[idx % n];
                if (q->on) {
                    if (have_off) { draw_quad(r, cx - ox, cy - oy, ofx - ox, ofy - oy, q->x - ox, q->y - oy); have_off = false; }
                    else draw_line(r, cx - ox, cy - oy, q->x - ox, q->y - oy);
                    cx = q->x; cy = q->y;
                } else {
                    if (have_off) {
                        float mx = (ofx + q->x) / 2, my = (ofy + q->y) / 2;
                        draw_quad(r, cx - ox, cy - oy, ofx - ox, ofy - oy, mx - ox, my - oy);
                        cx = mx; cy = my;
                    }
                    ofx = q->x; ofy = q->y;
                    have_off = true;
                }
            }
            if (have_off) draw_quad(r, cx - ox, cy - oy, ofx - ox, ofy - oy, sx - ox, sy - oy);
            else draw_line(r, cx - ox, cy - oy, sx - ox, sy - oy);
        }
        start = last + 1;
    }
}

/* ---- public ----------------------------------------------------------------------------------------------------- */

int ttf_ascent(struct ttf *f, int px) { return (int)ceilf((float)f->ascent * (float)px / (float)f->units); }
int ttf_descent(struct ttf *f, int px) { return (int)ceilf((float)f->descent * (float)px / (float)f->units); }

int ttf_advance(struct ttf *f, uint32_t cp, int px)
{
    return (int)floorf((float)hadv(f, glyph_index(f, cp)) * (float)px / (float)f->units + 0.5f);
}

static void render(struct ttf *f, uint32_t cp, int px, struct ttf_glyph *out)
{
    memset(out, 0, sizeof(*out));
    int gi = glyph_index(f, cp);
    out->advance = (int)floorf((float)hadv(f, gi) * (float)px / (float)f->units + 0.5f);
    struct outline o = { 0 };
    const float ident[4] = { 1, 0, 0, 1 };
    if (!load_glyph(f, gi, &o, ident, 0, 0, 0) || o.n < 3) {
        free(o.p);
        free(o.ends);
        return;
    }
    float sc = (float)px / (float)f->units;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < o.n; i++) {
        o.p[i].x *= sc;
        o.p[i].y = -o.p[i].y * sc;                              /* y down */
        minx = fminf(minx, o.p[i].x); maxx = fmaxf(maxx, o.p[i].x);
        miny = fminf(miny, o.p[i].y); maxy = fmaxf(maxy, o.p[i].y);
    }
    int x0 = (int)floorf(minx) - 1, y0 = (int)floorf(miny) - 1, w = (int)ceilf(maxx) + 1 - x0 + 1, h = (int)ceilf(maxy) + 1 - y0 + 1;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
        free(o.p);
        free(o.ends);
        return;
    }
    struct raster r = { calloc((size_t)w * (size_t)h + 2, sizeof(float)), w, h };
    fill_outline(&r, &o, (float)x0, (float)y0);
    out->cov = malloc((size_t)w * (size_t)h);
    for (int y = 0; y < h; y++) {
        float acc = 0;
        for (int x = 0; x < w; x++) {
            acc += r.a[(size_t)y * (size_t)w + (size_t)x];
            float v = fabsf(acc);
            out->cov[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)((v > 1 ? 1 : v) * 255.0f + 0.5f);
        }
    }
    out->w = w;
    out->h = h;
    out->left = x0;
    out->top = -y0;
    free(r.a);
    free(o.p);
    free(o.ends);
}

const struct ttf_glyph *ttf_glyph(struct ttf *f, uint32_t cp, int px)
{
    /* open addressing cache keyed by (cp, px) */
    if (f->ncache * 2 + 2 > f->capcache) {
        size_t nc = f->capcache ? f->capcache * 2 : 512;
        struct gcache *old = f->cache;
        size_t oc = f->capcache;
        f->cache = calloc(nc, sizeof(*f->cache));
        f->capcache = nc;
        for (size_t i = 0; i < oc; i++)
            if (old[i].used) {
                size_t h = (old[i].cp * 2654435761u + (uint32_t)old[i].px * 40503u) & (nc - 1);
                while (f->cache[h].used) h = (h + 1) & (nc - 1);
                f->cache[h] = old[i];
            }
        free(old);
    }
    size_t h = (cp * 2654435761u + (uint32_t)px * 40503u) & (f->capcache - 1);
    while (f->cache[h].used) {
        if (f->cache[h].cp == cp && f->cache[h].px == px)
            return &f->cache[h].g;
        h = (h + 1) & (f->capcache - 1);
    }
    f->cache[h].used = true;
    f->cache[h].cp = cp;
    f->cache[h].px = px;
    render(f, cp, px, &f->cache[h].g);
    f->ncache++;
    return &f->cache[h].g;
}
