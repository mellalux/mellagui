/* facedet - the Viola-Jones face detector of mellagui-cam. See facedet.h. */
#include "facedet.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "facedet_data.h"

#define MAX_HITS 6000
#define MIN_FACE 36                     /* the smallest face looked for, in pixels of the squeezed picture */
#define MAX_WIDTH 320
#define SCALE_STEP 1.2f
#define NEIGHBOURS 3

struct hit {
    float x, y, w, h;
};

/* The picture as 8 bit grey, squeezed by a whole factor, its histogram stretched. */
static uint8_t *make_grey(const uint32_t *px, int w, int h, int *aw, int *ah, int *factor)
{
    int f = w / MAX_WIDTH < 1 ? 1 : w / MAX_WIDTH;
    int gw = w / f, gh = h / f;
    uint8_t *g = malloc((size_t)gw * (size_t)gh);
    if (!g)
        return NULL;
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++) {
            int sum = 0;
            for (int j = 0; j < f; j++)
                for (int i = 0; i < f; i++) {
                    uint32_t p = px[(size_t)(y * f + j) * (size_t)w + (size_t)(x * f + i)];
                    sum += (int)(((p >> 16 & 255) * 77 + (p >> 8 & 255) * 150 + (p & 255) * 29) >> 8);
                }
            g[(size_t)y * (size_t)gw + (size_t)x] = (uint8_t)(sum / (f * f));
        }
    int hist[256] = { 0 };
    for (int i = 0; i < gw * gh; i++)
        hist[g[i]]++;
    int map[256], acc = 0, first = 0;
    while (first < 255 && hist[first] == 0)
        first++;
    int total = gw * gh - hist[first];
    for (int v = 0; v < 256; v++) {
        acc += hist[v];
        map[v] = total > 0 && v >= first ? (acc - hist[first]) * 255 / total : v;
        map[v] = map[v] < 0 ? 0 : map[v] > 255 ? 255 : map[v];
    }
    for (int i = 0; i < gw * gh; i++)
        g[i] = (uint8_t)map[g[i]];
    *aw = gw;
    *ah = gh;
    *factor = f;
    return g;
}

/* The cascade at one position of an image whose integral pictures are `sum` and `sq` (stride lw + 1). */
static int window_passes(const uint32_t *sum, const uint64_t *sq, int stride, int x, int y)
{
    const uint32_t *s = sum + (size_t)y * (size_t)stride + (size_t)x;
    const uint64_t *q = sq + (size_t)y * (size_t)stride + (size_t)x;
#define SUM(ox, oy, ww, hh) ((int)(s[(size_t)((oy) + (hh)) * (size_t)stride + (size_t)((ox) + (ww))] - s[(size_t)(oy) * (size_t)stride + (size_t)((ox) + (ww))] - \
                                   s[(size_t)((oy) + (hh)) * (size_t)stride + (size_t)(ox)] + s[(size_t)(oy) * (size_t)stride + (size_t)(ox)]))
    int vs = SUM(1, 1, HF_WIN - 2, HF_WIN - 2);
    uint64_t vq = q[(size_t)(1 + HF_WIN - 2) * (size_t)stride + (size_t)(1 + HF_WIN - 2)] - q[(size_t)1 * (size_t)stride + (size_t)(1 + HF_WIN - 2)] -
                  q[(size_t)(1 + HF_WIN - 2) * (size_t)stride + 1] + q[(size_t)1 * (size_t)stride + 1];
    double area = (double)((HF_WIN - 2) * (HF_WIN - 2)), nf = area * (double)vq - (double)vs * (double)vs;
    if (nf <= 0.0)
        return 0;
    nf = sqrt(nf);
    if (area / nf >= 0.1)                                                  /* a window with hardly any contrast (a wall) is no face */
        return 0;
    float inv = (float)(1.0 / nf);
    int wi = 0;
    for (int st = 0; st < (int)(sizeof(k_hf_stage) / sizeof(k_hf_stage[0])); st++) {
        float total = 0;
        for (int k = 0; k < k_hf_stage[st].n; k++, wi++) {
            const struct hf_weak *wk = &k_hf_weak[wi];
            const struct hf_feat *f = &k_hf_feat[wk->feat];
            int raw = f->w[0] * SUM(f->r[0][0], f->r[0][1], f->r[0][2], f->r[0][3]) + f->w[1] * SUM(f->r[1][0], f->r[1][1], f->r[1][2], f->r[1][3]);
            if (f->n > 2)
                raw += f->w[2] * SUM(f->r[2][0], f->r[2][1], f->r[2][2], f->r[2][3]);
            total += (float)raw * inv < wk->thr ? wk->left : wk->right;
        }
        if (total < k_hf_stage[st].thr - 0.00001f)
            return 0;
    }
#undef SUM
    return 1;
}

static int similar(const struct hit *a, const struct hit *b)
{
    float delta = 0.2f * (fminf(a->w, b->w) + fminf(a->h, b->h)) * 0.5f;
    return fabsf(a->x - b->x) <= delta && fabsf(a->y - b->y) <= delta && fabsf(a->x + a->w - b->x - b->w) <= delta && fabsf(a->y + a->h - b->y - b->h) <= delta;
}

static int uf_find(int *p, int i)
{
    while (p[i] != i)
        i = p[i] = p[p[i]];
    return i;
}

int facedet_same(const struct camfx_face *a, const struct camfx_face *b)
{
    float x0 = fmaxf(a->x, b->x), y0 = fmaxf(a->y, b->y), x1 = fminf(a->x + a->w, b->x + b->w), y1 = fminf(a->y + a->h, b->y + b->h);
    if (x1 <= x0 || y1 <= y0)
        return 0;
    float inter = (x1 - x0) * (y1 - y0), un = a->w * a->h + b->w * b->h - inter;
    return inter > 0.4f * un;
}

int facedet_detect(const uint32_t *px, int w, int h, struct camfx_face *out, int max)
{
    if (!px || !out || max <= 0 || w < 2 * MIN_FACE || h < 2 * MIN_FACE)
        return 0;
    int aw, ah, f;
    uint8_t *grey = make_grey(px, w, h, &aw, &ah, &f);
    if (!grey)
        return 0;
    struct hit *hits = malloc(sizeof(*hits) * MAX_HITS);
    int *par = malloc(sizeof(int) * MAX_HITS), *cnt = malloc(sizeof(int) * MAX_HITS);
    uint8_t *lv = malloc((size_t)aw * (size_t)ah);
    uint32_t *sum = malloc((size_t)(aw + 1) * (size_t)(ah + 1) * 4);
    uint64_t *sq = malloc((size_t)(aw + 1) * (size_t)(ah + 1) * 8);
    int nh = 0, n = 0;
    if (!hits || !par || !cnt || !lv || !sum || !sq)
        goto done;
    for (float sc = (float)MIN_FACE / (float)HF_WIN; HF_WIN * sc <= (float)(aw < ah ? aw : ah) && nh < MAX_HITS; sc *= SCALE_STEP) {
        int lw = (int)((float)aw / sc), lh = (int)((float)ah / sc);
        if (lw < HF_WIN || lh < HF_WIN)
            break;
        for (int y = 0; y < lh; y++) {                                      /* the picture at this level (bilinear) */
            float sy = (float)y * sc;
            int y0 = (int)sy, y1 = y0 + 1 < ah ? y0 + 1 : y0;
            float fy = sy - (float)y0;
            for (int x = 0; x < lw; x++) {
                float sx = (float)x * sc;
                int x0 = (int)sx, x1 = x0 + 1 < aw ? x0 + 1 : x0;
                float fx = sx - (float)x0;
                float a = (float)grey[(size_t)y0 * (size_t)aw + (size_t)x0] * (1 - fx) + (float)grey[(size_t)y0 * (size_t)aw + (size_t)x1] * fx;
                float b = (float)grey[(size_t)y1 * (size_t)aw + (size_t)x0] * (1 - fx) + (float)grey[(size_t)y1 * (size_t)aw + (size_t)x1] * fx;
                lv[(size_t)y * (size_t)lw + (size_t)x] = (uint8_t)(a * (1 - fy) + b * fy + 0.5f);
            }
        }
        int stride = lw + 1;
        memset(sum, 0, (size_t)stride * 4);
        memset(sq, 0, (size_t)stride * 8);
        for (int y = 0; y < lh; y++) {
            uint32_t rs = 0;
            uint64_t rq = 0;
            sum[(size_t)(y + 1) * (size_t)stride] = 0;
            sq[(size_t)(y + 1) * (size_t)stride] = 0;
            for (int x = 0; x < lw; x++) {
                uint32_t v = lv[(size_t)y * (size_t)lw + (size_t)x];
                rs += v;
                rq += (uint64_t)v * v;
                sum[(size_t)(y + 1) * (size_t)stride + (size_t)(x + 1)] = sum[(size_t)y * (size_t)stride + (size_t)(x + 1)] + rs;
                sq[(size_t)(y + 1) * (size_t)stride + (size_t)(x + 1)] = sq[(size_t)y * (size_t)stride + (size_t)(x + 1)] + rq;
            }
        }
        for (int y = 0; y + HF_WIN <= lh && nh < MAX_HITS; y += 2)
            for (int x = 0; x + HF_WIN <= lw && nh < MAX_HITS; x += 2)
                if (window_passes(sum, sq, stride, x, y))
                    hits[nh++] = (struct hit){ (float)x * sc, (float)y * sc, (float)HF_WIN * sc, (float)HF_WIN * sc };
    }
    /* group the hits that lie on top of each other; a group needs more than NEIGHBOURS hits */
    for (int i = 0; i < nh; i++)
        par[i] = i, cnt[i] = 0;
    for (int i = 0; i < nh; i++)
        for (int j = i + 1; j < nh; j++)
            if (similar(&hits[i], &hits[j])) {
                int a = uf_find(par, i), b = uf_find(par, j);
                if (a != b)
                    par[b] = a;
            }
    size_t navg = nh > 0 ? (size_t)nh : 1;
    struct hit *avg = calloc(navg, sizeof(*avg));
    if (!avg)
        goto done;
    for (int i = 0; i < nh; i++) {
        int r = uf_find(par, i);
        avg[r].x += hits[i].x, avg[r].y += hits[i].y, avg[r].w += hits[i].w, avg[r].h += hits[i].h;
        cnt[r]++;
    }
    struct camfx_face cand[64];
    int ncand = 0, cc[64];
    for (int i = 0; i < nh && ncand < 64; i++)
        if (par[i] == i && cnt[i] > NEIGHBOURS) {
            float k = 1.0f / (float)cnt[i];
            cand[ncand] = (struct camfx_face){ avg[i].x * k * (float)f, avg[i].y * k * (float)f, avg[i].w * k * (float)f, avg[i].h * k * (float)f };
            cc[ncand++] = cnt[i];
        }
    free(avg);
    for (int i = 0; i < ncand; i++) {                                       /* one inside a bigger one with as many hits is part of it */
        int inside = 0;
        for (int j = 0; j < ncand && !inside; j++) {
            float cx = cand[i].x + cand[i].w / 2, cy = cand[i].y + cand[i].h / 2;
            inside = j != i && cand[j].w > cand[i].w * 1.1f && cx > cand[j].x && cx < cand[j].x + cand[j].w && cy > cand[j].y && cy < cand[j].y + cand[j].h &&
                     cc[j] * 3 >= cc[i] * 2;
        }
        if (!inside)
            for (int k = 0; k < 64; k++) {                                  /* (insert by size, the biggest first) */
                if (k == n) {
                    if (n < max)
                        out[n++] = cand[i];
                    break;
                }
                if (cand[i].w * cand[i].h > out[k].w * out[k].h) {
                    int last = n < max ? n : max - 1;
                    for (int m = last; m > k; m--)
                        out[m] = out[m - 1];
                    out[k] = cand[i];
                    if (n < max)
                        n++;
                    break;
                }
            }
    }
done:
    free(grey);
    free(hits);
    free(par);
    free(cnt);
    free(lv);
    free(sum);
    free(sq);
    return n;
}
