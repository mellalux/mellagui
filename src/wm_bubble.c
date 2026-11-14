/* Part of the window manager (see wm_priv.h): rasterising the mesh, the launcher's bubble */
#include "wm_priv.h"

/* ---- the launcher's bubble --------------------------------------------------------------------------------------------------------- */


struct lm_output *bubble_output(const struct lm_wm *wm)
{
    if (!wm->srv->backend)
        return NULL;
    struct lm_output *o;
    lm_list_for_each(o, &wm->srv->backend->outputs, link)
        if (!strcmp(o->name, wm->launcher_out))
            return o;
    return NULL;
}

/* The middle of the Launcher button in the dock (where it would be even if autohide keeps the dock away), else the bottom middle of the output. */
static void bubble_origin(const struct lm_wm *wm, struct lm_output *o, float *cx, float *cy)
{
    struct lm_output *dock = lm_output_primary(wm->srv);
    if (dock == o && lm_dock.enabled && lm_dock_count() > 0) {
        struct dock_geo g;
        dock_geometry(o, &g);
        for (int i = 0; i < g.n; i++)
            if (launcher_is_self(lm_dock_item(i))) {
                *cx = (float)(g.x0 + i * (g.size + g.gap)) + (float)g.size / 2.0f;
                *cy = (float)g.icon_bottom - (float)g.size / 2.0f;
                return;
            }
    }
    *cx = (float)(o->x + o->width / 2);
    *cy = (float)(o->y + o->height - 40);
}

float smooth01(float x)
{
    x = LM_CLAMP(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

/* Where the corners of the cells are at progress p. The bubble leaves the button slowly, grows as it rises, wobbles (squashed one way, then the other)
 * and, from a little way on, spreads out into the card; the card is exactly there at p = 1. */
static void bubble_nodes(const struct wm_bubble *b, float p, float *xs, float *ys, float *s_out)
{
    float pe = smooth01(p);
    float s = smooth01((p - 0.28f) / 0.64f);                                 /* a bubble for the first quarter, then it spreads out */
    s += 0.05f * sinf(p * 18.8496f) * sinf(p * 3.14159265f);               /* it jiggles as it takes its shape (zero at both ends) */
    s = LM_CLAMP(s, 0.0f, 1.06f);
    float ccx = (float)b->card.x + (float)b->card.w / 2.0f, ccy = (float)b->card.y + (float)b->card.h / 2.0f;
    float cx = b->cx0 + (ccx - b->cx0) * pe, cy = b->cy0 + (ccy - b->cy0) * pe;
    float r1 = 0.32f * (float)LM_MIN(b->card.w, b->card.h);
    float R = BUBBLE_R0 + (r1 - BUBBLE_R0) * pe;
    float k = 0.12f * sinf(p * 25.0f) * (1.0f - fminf(s, 1.0f));              /* the squash: wider and flatter, then taller and narrower */
    float sx = 1.0f + k, sy = 1.0f - k;
    for (int j = 0; j <= BUB_R; j++)
        for (int i = 0; i <= BUB_C; i++) {
            float u = (float)i / (float)BUB_C, v = (float)j / (float)BUB_R, a = 2.0f * u - 1.0f, c = 2.0f * v - 1.0f;
            float dx = a * sqrtf(1.0f - c * c / 2.0f), dy = c * sqrtf(1.0f - a * a / 2.0f);       /* the square onto a disc */
            float bx = cx + dx * R * sx, by = cy + dy * R * sy;
            float rx = (float)b->card.x + u * (float)b->card.w, ry = (float)b->card.y + v * (float)b->card.h;
            xs[j * (BUB_C + 1) + i] = bx + (rx - bx) * s;
            ys[j * (BUB_C + 1) + i] = by + (ry - by) * s;
        }
    if (s_out)
        *s_out = s;
}

/* The highlight of a wet bubble: a soft bright patch up on the left and a thin lit rim, on a transparent square. */
static void bubble_make_sheen(struct lm_ghost *g)
{
    const int n = 96;
    uint32_t *px = calloc((size_t)n * (size_t)n, 4);
    if (!px)
        return;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            float fx = ((float)x + 0.5f) / (float)n * 2.0f - 1.0f, fy = ((float)y + 0.5f) / (float)n * 2.0f - 1.0f, r = sqrtf(fx * fx + fy * fy);
            float px_ = fx + 0.36f, py_ = fy + 0.40f, rot = 0.6f;                                   /* the patch, turned a little */
            float ex = (px_ * cosf(rot) + py_ * sinf(rot)) / 0.30f, ey = (-px_ * sinf(rot) + py_ * cosf(rot)) / 0.13f;
            float patch = fmaxf(1.0f - (ex * ex + ey * ey), 0.0f);
            float rim = r > 0.86f && r < 1.0f ? (1.0f - fabsf(r - 0.93f) / 0.07f) * (0.35f + 0.25f * (fx - fy)) : 0.0f;
            float film = r < 0.93f ? 0.10f * (1.0f - 0.5f * r) : 0.0f;                                  /* the thin skin of the bubble */
            float a = fminf(patch * patch * 0.85f + fmaxf(rim, 0.0f) * 0.55f + film, 1.0f);
            uint32_t av = (uint32_t)lroundf(a * 255.0f);
            px[y * n + x] = (av << 24) | (av << 16) | (av << 8) | av;                                  /* white, premultiplied */
        }
    g->px = px;
    g->w = g->h = n;
    g->gl_tex = 0;
}

/* ---- rasterising the mesh ----------------------------------------------------------------------------------------------------- */

void mips_free(struct wm_mips *m)
{
    for (int l = BUB_LEVELS - 1; l >= 1; l--) {                    /* (a level that is a copy of the next bigger one, after an out-of-memory, is freed once) */
        if (m->mip[l] && m->mip[l] != m->mip[l - 1])
            free(m->mip[l]);
        m->mip[l] = NULL;
    }
    m->mip[0] = NULL;
}

static void bubble_free_pictures(struct lm_wm *wm, struct wm_bubble *b)
{
    mips_free(&b->m);
    if (wm->srv->render) {
        lm_render_ghost_free(wm->srv->render, &b->frame);
        lm_render_ghost_free(wm->srv->render, &b->ghost);
        lm_render_ghost_free(wm->srv->render, &b->sheen);
    }
}

/* The card's picture and, halved by averaging 2 x 2 each time, its smaller copies. */
void mips_build(struct wm_mips *b, uint32_t *px0, int w0, int h0)
{
    b->mip[0] = px0;
    b->mw[0] = w0;
    b->mh[0] = h0;
    for (int l = 1; l < BUB_LEVELS; l++) {
        int w = LM_MAX(b->mw[l - 1] / 2, 1), h = LM_MAX(b->mh[l - 1] / 2, 1);
        uint32_t *px = calloc((size_t)w * (size_t)h, 4);
        if (!px) {
            b->mip[l] = b->mip[l - 1];                              /* out of memory: the bigger one will do */
            b->mw[l] = b->mw[l - 1];
            b->mh[l] = b->mh[l - 1];
            continue;
        }
        b->mip[l] = px;
        b->mw[l] = w;
        b->mh[l] = h;
        const uint32_t *src = b->mip[l - 1];
        int sw = b->mw[l - 1], sh = b->mh[l - 1];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t acc[4] = { 0, 0, 0, 0 };
                for (int k = 0; k < 4; k++) {
                    int sx = LM_MIN(2 * x + (k & 1), sw - 1), sy = LM_MIN(2 * y + (k >> 1), sh - 1);
                    uint32_t p = src[(size_t)sy * (size_t)sw + (size_t)sx];
                    acc[0] += p >> 24; acc[1] += (p >> 16) & 255; acc[2] += (p >> 8) & 255; acc[3] += p & 255;
                }
                px[(size_t)y * (size_t)w + (size_t)x] = ((acc[0] / 4) << 24) | ((acc[1] / 4) << 16) | ((acc[2] / 4) << 8) | (acc[3] / 4);
            }
    }
}

/* One texel of level `l`: a pixel beyond the picture repeats its edge, or with `soft` is transparent. */
static inline uint32_t mips_texel(const struct wm_mips *m, int l, int x, int y)
{
    int w = m->mw[l], h = m->mh[l];
    if (x < 0 || y < 0 || x >= w || y >= h) {
        if (m->soft)
            return 0;
        x = LM_CLAMP(x, 0, w - 1);
        y = LM_CLAMP(y, 0, h - 1);
    }
    return m->mip[l][(size_t)y * (size_t)w + (size_t)x];
}

/* Bilinear lookup in level `l` at (u, v) in that level's texels (premultiplied ARGB), in fixed point: the four colour channels are worked on two at a
 * time in the lanes of a 32 bit word, with the four weights adding up to 256. */
static inline uint32_t mips_sample(const struct wm_mips *m, int l, float u, float v)
{
    float fx = u - 0.5f, fy = v - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    int tx = (int)((fx - (float)x0) * 256.0f), ty = (int)((fy - (float)y0) * 256.0f);
    int w = m->mw[l], h = m->mh[l];
    uint32_t p00, p10, p01, p11;
    if (x0 >= 0 && y0 >= 0 && x0 + 1 < w && y0 + 1 < h) {
        const uint32_t *r0 = m->mip[l] + (size_t)y0 * (size_t)w + (size_t)x0;
        p00 = r0[0]; p10 = r0[1]; p01 = r0[w]; p11 = r0[w + 1];
    } else {
        p00 = mips_texel(m, l, x0, y0); p10 = mips_texel(m, l, x0 + 1, y0); p01 = mips_texel(m, l, x0, y0 + 1); p11 = mips_texel(m, l, x0 + 1, y0 + 1);
    }
    uint32_t w00 = (uint32_t)((256 - tx) * (256 - ty) >> 8), w10 = (uint32_t)(tx * (256 - ty) >> 8), w01 = (uint32_t)((256 - tx) * ty >> 8);
    uint32_t w11 = 256 - w00 - w10 - w01;
    uint32_t rb = (p00 & 0x00ff00ffu) * w00 + (p10 & 0x00ff00ffu) * w10 + (p01 & 0x00ff00ffu) * w01 + (p11 & 0x00ff00ffu) * w11;
    uint32_t ag = ((p00 >> 8) & 0x00ff00ffu) * w00 + ((p10 >> 8) & 0x00ff00ffu) * w10 + ((p01 >> 8) & 0x00ff00ffu) * w01 + ((p11 >> 8) & 0x00ff00ffu) * w11;
    return ((rb >> 8) & 0x00ff00ffu) | (ag & 0xff00ff00u);
}

/* One triangle of the mesh: x, y = where its corners are, u, v = which texel of the picture each shows. Covered pixels take the picture's colour; a pixel that
 * two triangles both reach keeps the more solid one, so a seam never shows a hole. Per row the covered stretch is worked out from the three edges, and the
 * texel position moves by a fixed step from pixel to pixel. */
void mips_triangle(const struct wm_mips *m, uint32_t *out, int bx, int by, int bw, int bh, const float *x, const float *y, const float *u, const float *v)
{
    float det = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
    float src = (u[1] - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (v[1] - v[0]);
    if (fabsf(det) < 1e-3f || fabsf(src) < 1e-6f)
        return;
    float step = sqrtf(fabsf(src) / fabsf(det));                    /* picture texels under one pixel: above 1 the picture is shrunk */
    int level = step > 1.0f ? LM_CLAMP((int)floorf(log2f(step)), 0, BUB_LEVELS - 1) : 0;
    float scale = 1.0f / (float)(1 << level), inv = 1.0f / det;
    int y0 = LM_MAX((int)floorf(fminf(y[0], fminf(y[1], y[2]))) - by, 0), y1 = LM_MIN((int)ceilf(fmaxf(y[0], fmaxf(y[1], y[2]))) - by, bh - 1);
    int xlo = LM_MAX((int)floorf(fminf(x[0], fminf(x[1], x[2]))) - bx, 0), xhi = LM_MIN((int)ceilf(fmaxf(x[0], fmaxf(x[1], x[2]))) - bx, bw - 1);
    /* l1 = a1 + b1 * fx, l2 = a2 + b2 * fx (fx = the pixel's centre), l0 = 1 - l1 - l2; they are the weights of the corners 1, 2 and 0 */
    float b1 = (y[2] - y[0]) * inv, b2 = -(y[1] - y[0]) * inv;
    float du = ((u[1] - u[0]) * b1 + (u[2] - u[0]) * b2) * scale, dv = ((v[1] - v[0]) * b1 + (v[2] - v[0]) * b2) * scale;
    const float e = -0.02f;
    for (int py = y0; py <= y1; py++) {
        float fy = (float)(by + py) + 0.5f;
        float a1 = (-x[0] * (y[2] - y[0]) - (x[2] - x[0]) * (fy - y[0])) * inv;
        float a2 = ((x[1] - x[0]) * (fy - y[0]) + x[0] * (y[1] - y[0])) * inv;
        float a0 = 1.0f - a1 - a2, b0 = -b1 - b2;
        float lo = (float)(bx + xlo) + 0.5f, hi = (float)(bx + xhi) + 0.5f;           /* the stretch of fx that is inside all three edges */
        const float aa[3] = { a0, a1, a2 }, bb[3] = { b0, b1, b2 };
        bool empty = false;
        for (int k = 0; k < 3; k++) {
            if (fabsf(bb[k]) < 1e-7f) {
                if (aa[k] < e)
                    empty = true;
                continue;
            }
            float edge = (e - aa[k]) / bb[k];
            if (bb[k] > 0.0f)
                lo = fmaxf(lo, edge);
            else
                hi = fminf(hi, edge);
        }
        if (empty || lo > hi)
            continue;
        int px0 = LM_MAX((int)ceilf(lo - 0.5f) - bx, xlo), px1 = LM_MIN((int)floorf(hi - 0.5f) - bx, xhi);
        if (px0 > px1)
            continue;
        float fx = (float)(bx + px0) + 0.5f;
        float l1 = a1 + b1 * fx, l2 = a2 + b2 * fx, l0 = 1.0f - l1 - l2;
        float cu = (l0 * u[0] + l1 * u[1] + l2 * u[2]) * scale, cv = (l0 * v[0] + l1 * v[1] + l2 * v[2]) * scale;
        uint32_t *dst = &out[(size_t)py * (size_t)bw + (size_t)px0];
        const int w = m->mw[level], h = m->mh[level];                       /* the texel position in 16.16 fixed point, moved by a fixed step */
        const uint32_t *tex = m->mip[level];
        int fu = (int)((cu - 0.5f) * 65536.0f), fv = (int)((cv - 0.5f) * 65536.0f), fdu = (int)(du * 65536.0f), fdv = (int)(dv * 65536.0f);
        for (int px = px0; px <= px1; px++, dst++, fu += fdu, fv += fdv) {
            int x0 = fu >> 16, y0 = fv >> 16, tx = (fu >> 8) & 255, ty = (fv >> 8) & 255;
            uint32_t p00, p10, p01, p11;
            if (x0 >= 0 && y0 >= 0 && x0 + 1 < w && y0 + 1 < h) {
                const uint32_t *r0 = tex + (size_t)y0 * (size_t)w + (size_t)x0;
                p00 = r0[0]; p10 = r0[1]; p01 = r0[w]; p11 = r0[w + 1];
            } else {
                p00 = mips_texel(m, level, x0, y0); p10 = mips_texel(m, level, x0 + 1, y0); p01 = mips_texel(m, level, x0, y0 + 1);
                p11 = mips_texel(m, level, x0 + 1, y0 + 1);
            }
            uint32_t w00 = (uint32_t)((256 - tx) * (256 - ty) >> 8), w10 = (uint32_t)(tx * (256 - ty) >> 8), w01 = (uint32_t)((256 - tx) * ty >> 8);
            uint32_t w11 = 256 - w00 - w10 - w01;
            uint32_t rb = (p00 & 0x00ff00ffu) * w00 + (p10 & 0x00ff00ffu) * w10 + (p01 & 0x00ff00ffu) * w01 + (p11 & 0x00ff00ffu) * w11;
            uint32_t ag = ((p00 >> 8) & 0x00ff00ffu) * w00 + ((p10 >> 8) & 0x00ff00ffu) * w10 + ((p01 >> 8) & 0x00ff00ffu) * w01 + ((p11 >> 8) & 0x00ff00ffu) * w11;
            uint32_t c = ((rb >> 8) & 0x00ff00ffu) | (ag & 0xff00ff00u);
            if ((c >> 24) >= (*dst >> 24))
                *dst = c;
        }
    }
}

/* The mesh as it is at b->prog, painted into b->frame (and b->shown, b->alpha, b->s set). */
static void bubble_render_frame(struct lm_wm *wm)
{
    struct wm_bubble *b = &wm->bubble;
    float xs[BUB_N], ys[BUB_N], s;
    bubble_nodes(b, b->prog, xs, ys, &s);
    float x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    for (int i = 1; i < BUB_N; i++) {
        x0 = fminf(x0, xs[i]); x1 = fmaxf(x1, xs[i]);
        y0 = fminf(y0, ys[i]); y1 = fmaxf(y1, ys[i]);
    }
    struct lm_rect r = lm_rect_make((int)floorf(x0) - 2, (int)floorf(y0) - 2, (int)ceilf(x1 - x0) + 5, (int)ceilf(y1 - y0) + 5);
    b->s = s;
    b->alpha = (int)lroundf(255.0f * (0.62f + 0.38f * LM_CLAMP(s, 0.0f, 1.0f)) * fminf(b->prog / 0.05f, 1.0f));
    if (wm->srv->render)
        lm_render_ghost_free(wm->srv->render, &b->frame);
    memset(&b->frame, 0, sizeof(b->frame));
    b->shown = r;
    if (r.w <= 0 || r.h <= 0 || (int64_t)r.w * r.h > 8 * 1000 * 1000)
        return;
    uint32_t *out = calloc((size_t)r.w * (size_t)r.h, 4);
    if (!out)
        return;
    const float W = (float)b->m.mw[0], H = (float)b->m.mh[0];
    const int stride = BUB_C + 1;
    for (int j = 0; j < BUB_R; j++)
        for (int i = 0; i < BUB_C; i++) {
            int n[4] = { j * stride + i, j * stride + i + 1, (j + 1) * stride + i, (j + 1) * stride + i + 1 };
            float u[4] = { W * (float)i / BUB_C, W * (float)(i + 1) / BUB_C, W * (float)i / BUB_C, W * (float)(i + 1) / BUB_C };
            float v[4] = { H * (float)j / BUB_R, H * (float)j / BUB_R, H * (float)(j + 1) / BUB_R, H * (float)(j + 1) / BUB_R };
            float cx[4], cy[4];
            for (int k = 0; k < 4; k++) {
                cx[k] = xs[n[k]];
                cy[k] = ys[n[k]];
            }
            static const int t1[3] = { 0, 1, 2 }, t2[3] = { 1, 3, 2 };
            float tx[3], ty[3], tu[3], tv[3];
            for (int k = 0; k < 3; k++) {
                tx[k] = cx[t1[k]]; ty[k] = cy[t1[k]]; tu[k] = u[t1[k]]; tv[k] = v[t1[k]];
            }
            mips_triangle(&b->m, out, r.x, r.y, r.w, r.h, tx, ty, tu, tv);
            for (int k = 0; k < 3; k++) {
                tx[k] = cx[t2[k]]; ty[k] = cy[t2[k]]; tu[k] = u[t2[k]]; tv[k] = v[t2[k]];
            }
            mips_triangle(&b->m, out, r.x, r.y, r.w, r.h, tx, ty, tu, tv);
        }
    b->frame.px = out;
    b->frame.w = r.w;
    b->frame.h = r.h;
}


void bubble_end(struct lm_wm *wm)
{
    struct wm_bubble *b = &wm->bubble;
    if (!b->active)
        return;
    struct lm_rect d = b->shown;
    bubble_free_pictures(wm, b);
    b->active = false;
    damage_rect(wm, &d);
    struct lm_output *o = bubble_output(wm);
    if (o) {                                                        /* the whole output was dimmed, and the real launcher shows now (or nothing) */
        struct lm_rect r = lm_output_rect(o);
        damage_rect(wm, &r);
    }
}

/* The launcher opens (the picture is made now, of the card as it will be) or closes (as it is now). */
void bubble_begin(struct lm_wm *wm, bool opening)
{
    struct lm_output *o = opening ? bubble_output(wm) : launcher_output(wm);
    if (!o || anim_duration(wm) <= 0 || !wm->srv->render || lm_lock_active(wm->srv->lock))
        return;
    if (wm->bubble.active)
        bubble_end(wm);
    struct wm_bubble *b = &wm->bubble;
    memset(b, 0, sizeof(*b));
    struct lm_scene_item it;
    memset(&it, 0, sizeof(it));
    it.kind = LM_SCENE_LAUNCHER;
    it.output = o;
    launcher_info(wm, o, &it.u.launcher);
    it.u.launcher.veil = 0.0f;                                      /* the dimming is not part of the picture: it fades on its own */
    b->card = it.u.launcher.card;
    struct lm_rect snap = lm_rect_make(b->card.x - 2, b->card.y - 2, b->card.w + 4, b->card.h + 4);
    if (!lm_render_snapshot(wm->srv->render, &it, 1, &snap, &b->ghost))
        return;
    b->card = snap;                                                 /* (the picture is a little larger than the card: the cells map it exactly) */
    bubble_make_sheen(&b->sheen);
    bubble_origin(wm, o, &b->cx0, &b->cy0);
    b->active = true;
    b->closing = !opening;
    b->prog = opening ? 0.0f : 1.0f;
    b->last = lm_now_ms();
    mips_build(&b->m, b->ghost.px, b->ghost.w, b->ghost.h);
    bubble_render_frame(wm);
    damage_rect(wm, &b->shown);
    struct lm_rect r = lm_output_rect(o);
    damage_rect(wm, &r);
    if (wm->anim_timer && !lm_timer_is_armed(wm->anim_timer))
        lm_timer_arm_ms(wm->anim_timer, ANIM_TICK_MS, ANIM_TICK_MS);
}

/* The opening is cut short (a key, a click): the card is there now. */
void bubble_finish(struct lm_wm *wm)
{
    if (wm->bubble.active)
        bubble_end(wm);
}

bool bubble_tick(struct lm_wm *wm, uint64_t now)
{
    struct wm_bubble *b = &wm->bubble;
    if (!b->active)
        return false;
    float step = (float)(now - b->last) / (float)BUBBLE_MS;
    b->last = now;
    b->prog += b->closing ? -step : step;
    if (b->prog >= 1.0f && !b->closing) {
        bubble_end(wm);
        return false;
    }
    if (b->prog <= 0.0f) {
        b->prog = 0.0f;
        if (b->closing) {
            bubble_end(wm);
            return false;
        }
    }
    struct lm_rect old = b->shown;
    bubble_render_frame(wm);
    struct lm_rect d = lm_rect_union(&old, &b->shown);
    damage_rect(wm, &d);
    struct lm_output *o = bubble_output(wm);
    if (o) {                                                        /* the dimming fades with it */
        struct lm_rect r = lm_output_rect(o);
        damage_rect(wm, &r);
    }
    return true;
}


void anim_tick(void *data)
{
    struct lm_wm *wm = data;
    bool any = false;
    uint64_t now = lm_now_ms();
    if (bubble_tick(wm, now))
        any = true;
    for (int i = 0; i < ANIM_MAX; i++) {
        struct wm_anim *a = &wm->anim[i];
        if (!a->used)
            continue;
        float t = a->dur > 0 ? (float)(now - a->start) / (float)a->dur : 1.0f;
        if (t >= 1.0f) {
            anim_end(wm, a);
            continue;
        }
        struct lm_rect old = a->shown;
        anim_state(a, t, &a->shown, &a->alpha);
        struct lm_rect d = lm_rect_union(&old, &a->shown);
        damage_rect(wm, &d);
        any = true;
    }
    if (wm->wb.active) {
        wobble_tick(wm, now);
        any = true;
    }
    if (overview_tick(wm, now))
        any = true;
    struct lm_window *g = wm->glide_win;
    if (g) {                                                        /* a dragged window follows the pointer with a short delay: it glides */
        float tau = (float)LM_MAX(anim_duration(wm) / 5, 20);       /* ms: how fast it catches up (a fifth of the animation time) */
        float f = 1.0f - expf(-(float)(now - wm->glide_last) / tau);
        wm->glide_last = now;
        int32_t dx = wm->glide_tx - g->x, dy = wm->glide_ty - g->y;
        int32_t sx = (int32_t)lroundf((float)dx * f), sy = (int32_t)lroundf((float)dy * f);
        if (dx && !sx) sx = dx > 0 ? 1 : -1;
        if (dy && !sy) sy = dy > 0 ? 1 : -1;
        if (sx || sy) {
            struct lm_rect before = frame_outer(g);
            g->x += sx;
            g->y += sy;
            damage_rect(wm, &before);
            damage_frame(wm, g);
            update_outputs(wm, g);
            any = true;
        }
        if (g->x == wm->glide_tx && g->y == wm->glide_ty)
            wm->glide_win = NULL;                                   /* arrived (a new motion of the pointer starts it again) */
    }
    if (!any && wm->anim_timer)
        lm_timer_arm_ms(wm->anim_timer, 0, 0);
}

/* The dragged window should be at (x, y): with animations on it glides there (true), else the caller moves it at once (false). */
bool anim_glide_to(struct lm_wm *wm, struct lm_window *w, int32_t x, int32_t y)
{
    if (anim_duration(wm) <= 0 || !wm->anim_timer || (w->states & LM_WINDOW_FULLSCREEN))
        return false;
    wm->glide_win = w;
    wm->glide_tx = x;
    wm->glide_ty = y;
    if (!lm_timer_is_armed(wm->anim_timer)) {
        wm->glide_last = lm_now_ms();
        lm_timer_arm_ms(wm->anim_timer, ANIM_TICK_MS, ANIM_TICK_MS);
    }
    return true;
}
