#ifndef LUMEN_UTIL_H
#define LUMEN_UTIL_H

/*
 * Small shared helpers: macros, rectangles and rectangle-list regions.
 * Header-only, written and owned by the architect, fully implemented here.
 *
 * Coordinates that come from clients are hostile: all rect/region helpers
 * clamp to +-LM_COORD_LIMIT and use 64-bit intermediate math, so no
 * arithmetic overflow can happen inside them.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LM_MIN(a, b) ((a) < (b) ? (a) : (b))
#define LM_MAX(a, b) ((a) > (b) ? (a) : (b))
#define LM_CLAMP(v, lo, hi) LM_MIN(LM_MAX((v), (lo)), (hi))
#define LM_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define LM_PRINTF(fmt_idx, arg_idx) \
    __attribute__((format(printf, fmt_idx, arg_idx)))

#define LM_COORD_LIMIT (1 << 28)

/* ---- rectangles ------------------------------------------------------- */

struct lm_rect {
    int32_t x, y, w, h; /* w/h <= 0 means empty */
};

static inline struct lm_rect lm_rect_make(int32_t x, int32_t y, int32_t w,
                                          int32_t h)
{
    struct lm_rect r = { x, y, w, h };
    return r;
}

static inline bool lm_rect_is_empty(const struct lm_rect *r)
{
    return r->w <= 0 || r->h <= 0;
}

static inline bool lm_rect_equal(const struct lm_rect *a, const struct lm_rect *b)
{
    return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

static inline bool lm_rect_contains(const struct lm_rect *r, int32_t x, int32_t y)
{
    return x >= r->x && y >= r->y && (int64_t)x < (int64_t)r->x + r->w &&
           (int64_t)y < (int64_t)r->y + r->h;
}

/* Returns true if the intersection is non-empty. `out` may alias a or b. */
static inline bool lm_rect_intersect(const struct lm_rect *a,
                                     const struct lm_rect *b,
                                     struct lm_rect *out)
{
    if (lm_rect_is_empty(a) || lm_rect_is_empty(b))
        return false;
    int64_t x1 = LM_MAX((int64_t)a->x, (int64_t)b->x);
    int64_t y1 = LM_MAX((int64_t)a->y, (int64_t)b->y);
    int64_t x2 = LM_MIN((int64_t)a->x + a->w, (int64_t)b->x + b->w);
    int64_t y2 = LM_MIN((int64_t)a->y + a->h, (int64_t)b->y + b->h);
    if (x2 <= x1 || y2 <= y1)
        return false;
    if (out) {
        out->x = (int32_t)x1;
        out->y = (int32_t)y1;
        out->w = (int32_t)(x2 - x1);
        out->h = (int32_t)(y2 - y1);
    }
    return true;
}

static inline bool lm_rect_overlaps(const struct lm_rect *a, const struct lm_rect *b)
{
    return lm_rect_intersect(a, b, NULL);
}

/* Bounding box of two rects (an empty rect is ignored). Inputs must be sane. */
static inline struct lm_rect lm_rect_union(const struct lm_rect *a,
                                           const struct lm_rect *b)
{
    if (lm_rect_is_empty(a))
        return *b;
    if (lm_rect_is_empty(b))
        return *a;
    int64_t x1 = LM_MIN(a->x, b->x), y1 = LM_MIN(a->y, b->y);
    int64_t x2 = LM_MAX((int64_t)a->x + a->w, (int64_t)b->x + b->w);
    int64_t y2 = LM_MAX((int64_t)a->y + a->h, (int64_t)b->y + b->h);
    struct lm_rect r = { (int32_t)x1, (int32_t)y1, (int32_t)(x2 - x1),
                         (int32_t)(y2 - y1) };
    return r;
}

/*
 * Clamp a hostile rect into +-LM_COORD_LIMIT. Returns false (and leaves *r
 * unspecified) if the result is empty.
 */
static inline bool lm_rect_sanitize(struct lm_rect *r)
{
    if (r->w <= 0 || r->h <= 0)
        return false;
    int64_t x1 = r->x, y1 = r->y;
    int64_t x2 = x1 + (int64_t)r->w, y2 = y1 + (int64_t)r->h;
    x1 = LM_CLAMP(x1, -(int64_t)LM_COORD_LIMIT, (int64_t)LM_COORD_LIMIT);
    y1 = LM_CLAMP(y1, -(int64_t)LM_COORD_LIMIT, (int64_t)LM_COORD_LIMIT);
    x2 = LM_CLAMP(x2, -(int64_t)LM_COORD_LIMIT, (int64_t)LM_COORD_LIMIT);
    y2 = LM_CLAMP(y2, -(int64_t)LM_COORD_LIMIT, (int64_t)LM_COORD_LIMIT);
    if (x2 <= x1 || y2 <= y1)
        return false;
    r->x = (int32_t)x1;
    r->y = (int32_t)y1;
    r->w = (int32_t)(x2 - x1);
    r->h = (int32_t)(y2 - y1);
    return true;
}

/* ---- regions (union of rectangles, may overlap) ----------------------- */

#define LM_REGION_MAX_RECTS 256

struct lm_region {
    struct lm_rect *rects;
    int32_t n;
    int32_t cap;
};

static inline void lm_region_init(struct lm_region *r)
{
    r->rects = NULL;
    r->n = 0;
    r->cap = 0;
}

static inline void lm_region_fini(struct lm_region *r)
{
    free(r->rects);
    lm_region_init(r);
}

static inline void lm_region_clear(struct lm_region *r)
{
    r->n = 0;
}

static inline bool lm_region_is_empty(const struct lm_region *r)
{
    return r->n == 0;
}

/* Bounding box; returns false if the region is empty. */
static inline bool lm_region_extents(const struct lm_region *r, struct lm_rect *out)
{
    if (r->n == 0)
        return false;
    struct lm_rect e = r->rects[0];
    for (int32_t i = 1; i < r->n; i++)
        e = lm_rect_union(&e, &r->rects[i]);
    *out = e;
    return true;
}

static inline int lm_region_reserve(struct lm_region *r, int32_t want)
{
    if (want <= r->cap)
        return 0;
    int32_t ncap = r->cap ? r->cap : 4;
    while (ncap < want)
        ncap *= 2;
    struct lm_rect *p = realloc(r->rects, sizeof(*p) * (size_t)ncap);
    if (!p)
        return -1;
    r->rects = p;
    r->cap = ncap;
    return 0;
}

/*
 * Add a rect (hostile input is clamped). If the region would exceed
 * LM_REGION_MAX_RECTS it degrades to its bounding box. Returns 0, or -1 on OOM.
 */
static inline int lm_region_add_rect(struct lm_region *r, const struct lm_rect *in)
{
    struct lm_rect s = *in;
    if (!lm_rect_sanitize(&s))
        return 0;
    for (int32_t i = 0; i < r->n; i++) {
        const struct lm_rect *o = &r->rects[i];
        if (s.x >= o->x && s.y >= o->y &&
            (int64_t)s.x + s.w <= (int64_t)o->x + o->w &&
            (int64_t)s.y + s.h <= (int64_t)o->y + o->h)
            return 0; /* already covered */
    }
    if (r->n >= LM_REGION_MAX_RECTS) {
        struct lm_rect e;
        lm_region_extents(r, &e);
        e = lm_rect_union(&e, &s);
        r->rects[0] = e;
        r->n = 1;
        return 0;
    }
    if (lm_region_reserve(r, r->n + 1) < 0)
        return -1;
    r->rects[r->n++] = s;
    return 0;
}

static inline int lm_region_add_region(struct lm_region *dst, const struct lm_region *src)
{
    for (int32_t i = 0; i < src->n; i++)
        if (lm_region_add_rect(dst, &src->rects[i]) < 0)
            return -1;
    return 0;
}

static inline int lm_region_copy(struct lm_region *dst, const struct lm_region *src)
{
    lm_region_clear(dst);
    return lm_region_add_region(dst, src);
}

/*
 * Remove a rect from the region (splits rects into up to 4 pieces each).
 * Returns 0, or -1 on OOM / if the result would exceed LM_REGION_MAX_RECTS;
 * on -1 the region is left unchanged and callers should treat it as unusable
 * (e.g. clear an opaque region).
 */
static inline int lm_region_subtract_rect(struct lm_region *r, const struct lm_rect *in)
{
    struct lm_rect s = *in;
    if (!lm_rect_sanitize(&s) || r->n == 0)
        return 0;
    size_t maxn = (size_t)r->n * 4;
    struct lm_rect *out = malloc(sizeof(*out) * maxn);
    if (!out)
        return -1;
    size_t n = 0;
    for (int32_t i = 0; i < r->n; i++) {
        struct lm_rect a = r->rects[i], x;
        if (!lm_rect_intersect(&a, &s, &x)) {
            out[n++] = a;
            continue;
        }
        int32_t ax2 = a.x + a.w, ay2 = a.y + a.h;
        int32_t xx2 = x.x + x.w, xy2 = x.y + x.h;
        if (x.y > a.y)  /* top band */
            out[n++] = lm_rect_make(a.x, a.y, a.w, x.y - a.y);
        if (xy2 < ay2)  /* bottom band */
            out[n++] = lm_rect_make(a.x, xy2, a.w, ay2 - xy2);
        if (x.x > a.x)  /* left piece */
            out[n++] = lm_rect_make(a.x, x.y, x.x - a.x, x.h);
        if (xx2 < ax2)  /* right piece */
            out[n++] = lm_rect_make(xx2, x.y, ax2 - xx2, x.h);
    }
    if (n > LM_REGION_MAX_RECTS || lm_region_reserve(r, (int32_t)n) < 0) {
        free(out);
        return -1;
    }
    memcpy(r->rects, out, sizeof(*out) * n);
    r->n = (int32_t)n;
    free(out);
    return 0;
}

static inline bool lm_region_contains_point(const struct lm_region *r, int32_t x, int32_t y)
{
    for (int32_t i = 0; i < r->n; i++)
        if (lm_rect_contains(&r->rects[i], x, y))
            return true;
    return false;
}

/* Intersect in place with `clip`. */
static inline void lm_region_intersect_rect(struct lm_region *r, const struct lm_rect *clip)
{
    int32_t k = 0;
    for (int32_t i = 0; i < r->n; i++) {
        struct lm_rect x;
        if (lm_rect_intersect(&r->rects[i], clip, &x))
            r->rects[k++] = x;
    }
    r->n = k;
}


#endif
