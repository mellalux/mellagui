/* Part of the window manager (see wm_priv.h): window animations, wobbling windows */
#include "wm_priv.h"

/* ---- window animations ------------------------------------------------------------------------------------------------ */

/* How long an animation takes (ms); 0 = none. MELLAGUI_ANIMATIONS (tests; headless otherwise never animates): a number above 1 is the
 * time, 1 the metrics value, 0 off. */
int anim_duration(const struct lm_wm *wm)
{
    const char *e = getenv("MELLAGUI_ANIMATIONS");
    if (e && *e) {
        int v = atoi(e);
        return v > 1 ? LM_MIN(v, 5000) : v == 1 ? lm_metrics.animation_ms : 0;
    }
    if (wm->srv->backend && !strcmp(wm->srv->backend->ops->name, "headless"))
        return 0;
    return lm_metrics.animation_ms;
}

bool anim_wanted(struct lm_wm *wm, const struct lm_window *w)
{
    return anim_duration(wm) > 0 && !wm->anim_suppress && wm->srv->render && !lm_lock_active(wm->srv->lock) &&
           !(w->states & LM_WINDOW_FULLSCREEN) && !win_is_aux(w);
}

/* The window's picture (frame and content, without popups) and the rectangle it covers. */
bool anim_prepare(struct lm_wm *wm, struct lm_window *w, struct lm_ghost *g, struct lm_rect *home)
{
    struct lm_scene_item it[2];
    memset(it, 0, sizeof(it));
    it[0].kind = LM_SCENE_FRAME;
    frame_info(wm, w, &it[0].u.frame);
    int32_t ox, oy;
    win_origin(w, &ox, &oy);
    it[1].kind = LM_SCENE_SURFACE;
    it[1].u.surface.root = w->surface;
    it[1].u.surface.x = ox;
    it[1].u.surface.y = oy;
    struct lm_rect outer = frame_outer(w), ext;
    *home = outer;
    if (lm_surface_tree_extents(w->surface, &ext)) {
        struct lm_rect eb = lm_rect_make(ox + ext.x, oy + ext.y, ext.w, ext.h);
        *home = lm_rect_union(&outer, &eb);
    }
    return lm_render_snapshot(wm->srv->render, it, 2, home, g);
}

/* Where a minimized window goes to: the dock icon of its program when the dock shows one, else its taskbar button, else the bottom centre. */
static struct lm_rect anim_target(struct lm_wm *wm, struct lm_window *w, const struct lm_rect *home)
{
    struct lm_output *o = window_output(wm, w);
    if (!o)
        o = lm_output_nearest(wm->srv, home->x + home->w / 2, home->y + home->h / 2);
    if (o && lm_dock.enabled && dock_shown(wm)) {
        struct lm_dock_info di;
        dock_info(wm, o, &di);
        for (int i = 0; i < di.n; i++)
            if (di.icon[i].item && dock_app_matches(di.icon[i].item->match, w->app_id))
                return di.icon[i].box;
    }
    if (o) {
        struct lm_panel_info pi;
        panel_info(wm, o, &pi);
        for (int i = 0; i < pi.ntasks; i++)
            if (pi.task_win[i] == w)
                return pi.task[i].rect;
        return lm_rect_make(o->x + o->width / 2 - 24, o->y + o->height - 12, 48, 12);
    }
    return lm_rect_make(home->x + home->w / 2 - 24, home->y + home->h - 12, 48, 12);
}

static float ease_out(float t) { float u = 1.0f - t; return 1.0f - u * u * u; }
float ease_smooth(float t) { return t * t * (3.0f - 2.0f * t); }

struct lm_rect anim_lerp_rect(const struct lm_rect *a, const struct lm_rect *b, float e)
{
    return lm_rect_make((int)lroundf((float)a->x + (float)(b->x - a->x) * e), (int)lroundf((float)a->y + (float)(b->y - a->y) * e),
                        LM_MAX((int)lroundf((float)a->w + (float)(b->w - a->w) * e), 1),
                        LM_MAX((int)lroundf((float)a->h + (float)(b->h - a->h) * e), 1));
}

/* The picture's rectangle and opacity `t` (0..1) of the way through. */
void anim_state(const struct wm_anim *a, float t, struct lm_rect *r, int *alpha)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    switch (a->kind) {
    case ANIM_OPEN:
    case ANIM_CLOSE: {
        float e = ease_out(t), s, op;
        if (a->kind == ANIM_OPEN) {
            s = 0.9f + 0.1f * e;
            op = e;
        } else {
            s = 1.0f - 0.08f * e;
            op = 1.0f - e;
        }
        int w = LM_MAX((int)lroundf((float)a->home.w * s), 1), h = LM_MAX((int)lroundf((float)a->home.h * s), 1);
        *r = lm_rect_make(a->home.x + (a->home.w - w) / 2, a->home.y + (a->home.h - h) / 2, w, h);
        *alpha = (int)lroundf(255.0f * op);
        break;
    }
    case ANIM_SLIDE_OUT:
    case ANIM_SLIDE_IN: {                                           /* sideways: the leaving windows go, the arriving ones come from the other side */
        float e = ease_smooth(t);
        float off = (float)a->shift * (a->kind == ANIM_SLIDE_OUT ? e : 1.0f - e);
        *r = a->home;
        r->x += (int)lroundf(off);
        *alpha = 255;
        break;
    }
    case ANIM_MINIMIZE:
    case ANIM_RESTORE: {
        float e = ease_smooth(a->kind == ANIM_MINIMIZE ? t : 1.0f - t);          /* a restore runs the minimize backwards */
        *r = anim_lerp_rect(&a->home, &a->away, e);
        *alpha = (int)lroundf(255.0f * (1.0f - e * e));
        break;
    }
    }
    if (*alpha < 0) *alpha = 0;
    if (*alpha > 255) *alpha = 255;
}

void anim_end(struct lm_wm *wm, struct wm_anim *a)
{
    struct lm_rect d = lm_rect_union(&a->shown, &a->home);
    if (a->win)
        a->win->anim_hidden = false;
    lm_render_ghost_free(wm->srv->render, &a->ghost);
    a->used = false;
    a->win = NULL;
    damage_rect(wm, &d);
}

void anim_finish_all(struct lm_wm *wm)
{
    wobble_stop(wm);
    bubble_end(wm);
    for (int i = 0; i < ANIM_MAX; i++)
        if (wm->anim[i].used)
            anim_end(wm, &wm->anim[i]);
}

/* The window is going away: its animation (and the picture it holds) ends. */
void anim_forget(struct lm_wm *wm, struct lm_window *w)
{
    if (wm->glide_win == w)
        wm->glide_win = NULL;
    if (wm->wb.active && wm->wb.win == w)
        wobble_stop(wm);
    for (int i = 0; i < ANIM_MAX; i++)
        if (wm->anim[i].used && wm->anim[i].win == w)
            anim_end(wm, &wm->anim[i]);
    if (w->close_ghost) {
        free(w->close_ghost->px);
        free(w->close_ghost);
        w->close_ghost = NULL;
    }
}

/* Starts an animation; the ghost's pixels now belong to it. */
struct wm_anim *anim_start(struct lm_wm *wm, enum anim_kind kind, struct lm_window *w, struct lm_ghost *g, const struct lm_rect *home)
{
    struct wm_anim *a = NULL;
    for (int i = 0; i < ANIM_MAX && !a; i++)
        if (!wm->anim[i].used)
            a = &wm->anim[i];
    if (!a) {                                                       /* all busy: the first one ends at once */
        a = &wm->anim[0];
        anim_end(wm, a);
    }
    if (w)
        for (int i = 0; i < ANIM_MAX; i++)                          /* one animation per window: the older one stops */
            if (wm->anim[i].used && wm->anim[i].win == w)
                anim_end(wm, &wm->anim[i]);
    memset(a, 0, sizeof(*a));
    a->used = true;
    a->kind = kind;
    a->ghost = *g;
    memset(g, 0, sizeof(*g));
    a->home = *home;
    a->dur = anim_duration(wm);
    a->start = lm_now_ms();
    if (kind == ANIM_MINIMIZE || kind == ANIM_RESTORE) {
        a->away = anim_target(wm, w, home);
        a->away.y += a->away.h / 2;                                 /* shrinks towards the middle of the button */
        a->away.x += a->away.w / 4;
        a->away.w = LM_MAX(a->away.w / 2, 2);
        a->away.h = 2;
    }
    if (kind == ANIM_OPEN || kind == ANIM_RESTORE || kind == ANIM_SLIDE_IN) {
        a->win = w;
        w->anim_hidden = true;
    }
    anim_state(a, 0.0f, &a->shown, &a->alpha);
    struct lm_rect d = lm_rect_union(&a->home, &a->shown);
    damage_rect(wm, &d);
    if (wm->anim_timer && !lm_timer_is_armed(wm->anim_timer))
        lm_timer_arm_ms(wm->anim_timer, ANIM_TICK_MS, ANIM_TICK_MS);
    return a;
}

/* The windows of the current workspace that show now (before a switch: those that leave; after it: those that arrive) slide: each is a
 * picture moving by one output width. Not for fullscreen windows (they just appear) or while the overview or the lock screen is up. */
void anim_workspace(struct lm_wm *wm, int dir, bool leaving)
{
    if (anim_duration(wm) <= 0 || !wm->srv->render || lm_lock_active(wm->srv->lock) || wm->ov.open || wm->ov.closing)
        return;
    struct lm_window *w;
    lm_list_for_each(w, &wm->windows, link) {
        if (!win_visible(wm, w) || !w->mapped || !anim_wanted(wm, w))
            continue;
        struct lm_ghost g;
        struct lm_rect home;
        if (!anim_prepare(wm, w, &g, &home))
            continue;
        struct lm_output *o = window_output(wm, w);
        if (!o)
            o = lm_output_nearest(wm->srv, home.x + home.w / 2, home.y + home.h / 2);
        int width = o ? o->width : 1280;
        struct wm_anim *a = anim_start(wm, leaving ? ANIM_SLIDE_OUT : ANIM_SLIDE_IN, leaving ? NULL : w, &g, &home);
        a->shift = leaving ? -dir * width : dir * width;
        anim_state(a, 0.0f, &a->shown, &a->alpha);
    }
}

/* ---- wobbling windows ------------------------------------------------------------------------------------------------- */

bool wobble_wanted(const struct lm_wm *wm, const struct lm_window *w)
{
    return lm_metrics.wobble > 0 && anim_duration(wm) > 0 && wm->srv->render && !(w->states & LM_WINDOW_FULLSCREEN) && !win_is_aux(w);
}

/* Where node (i, j) is when the window is rigid. */
static void wobble_rest(const struct wm_wobble *wb, int i, int j, float *x, float *y)
{
    *x = (float)(wb->win->x + wb->off_x) + (float)wb->ghost.w * (float)i / (float)WB_COLS;
    *y = (float)(wb->win->y + wb->off_y) + (float)wb->ghost.h * (float)j / (float)WB_ROWS;
}

/* The rectangle all the nodes lie in. */
static struct lm_rect wobble_bounds(const struct wm_wobble *wb)
{
    float x0 = wb->px[0], x1 = wb->px[0], y0 = wb->py[0], y1 = wb->py[0];
    for (int n = 1; n < WB_N; n++) {
        x0 = fminf(x0, wb->px[n]);
        x1 = fmaxf(x1, wb->px[n]);
        y0 = fminf(y0, wb->py[n]);
        y1 = fmaxf(y1, wb->py[n]);
    }
    return lm_rect_make((int)floorf(x0) - 1, (int)floorf(y0) - 1, (int)ceilf(x1 - x0) + 3, (int)ceilf(y1 - y0) + 3);
}

/* The mesh as one picture: every cell is two triangles of the window's picture (and its smaller copies), moved to where the nodes are, rasterised bilinearly into
 * `wb->frame` - smooth, in one piece (no seams, no gaps), and it can be painted see-through without the cells showing through each other. */
static void wobble_render(struct lm_wm *wm, struct wm_wobble *wb)
{
    lm_render_ghost_free(wm->srv->render, &wb->frame);
    struct lm_rect r = wb->shown;
    wb->frame_rect = r;
    if (!wb->m.mip[0] || r.w <= 0 || r.h <= 0 || (int64_t)r.w * r.h > 8 * 1000 * 1000)
        return;
    uint32_t *out = calloc((size_t)r.w * (size_t)r.h, 4);
    if (!out)
        return;
    const float W = (float)wb->m.mw[0], H = (float)wb->m.mh[0];
    const int stride = WB_COLS + 1;
    static const int t1[3] = { 0, 1, 2 }, t2[3] = { 1, 3, 2 };
    for (int j = 0; j < WB_ROWS; j++)
        for (int i = 0; i < WB_COLS; i++) {
            int n[4] = { j * stride + i, j * stride + i + 1, (j + 1) * stride + i, (j + 1) * stride + i + 1 };
            float u[4] = { W * (float)i / WB_COLS, W * (float)(i + 1) / WB_COLS, W * (float)i / WB_COLS, W * (float)(i + 1) / WB_COLS };
            float v[4] = { H * (float)j / WB_ROWS, H * (float)j / WB_ROWS, H * (float)(j + 1) / WB_ROWS, H * (float)(j + 1) / WB_ROWS };
            for (int t = 0; t < 2; t++) {
                const int *id = t ? t2 : t1;
                float tx[3], ty[3], tu[3], tv[3];
                for (int k = 0; k < 3; k++) {
                    tx[k] = wb->px[n[id[k]]];
                    ty[k] = wb->py[n[id[k]]];
                    tu[k] = u[id[k]];
                    tv[k] = v[id[k]];
                }
                mips_triangle(&wb->m, out, r.x, r.y, r.w, r.h, tx, ty, tu, tv);
            }
        }
    wb->frame.px = out;
    wb->frame.w = r.w;
    wb->frame.h = r.h;
}

/* The window becomes a drop while it is held: the corners of its picture are rounded to radius `r` (the picture as the window made it is kept in `clean`).
 * Only the four corner squares are touched. */
static void wobble_round(struct lm_wm *wm, struct wm_wobble *wb, int r)
{
    if (!wb->clean || !wb->ghost.px || r == wb->round_applied)
        return;
    int w = wb->ghost.w, h = wb->ghost.h;
    r = LM_CLAMP(r, 0, LM_MIN(w, h) / 2);
    int rr = LM_MAX(r, wb->round_applied);                       /* (what was rounded before has to be undone, too) */
    for (int c = 0; c < 4; c++) {
        int x0 = c & 1 ? w - rr : 0, y0 = c & 2 ? h - rr : 0;
        float cx = c & 1 ? (float)(w - r) : (float)r, cy = c & 2 ? (float)(h - r) : (float)r;
        for (int y = y0; y < y0 + rr && y < h; y++)
            for (int x = x0; x < x0 + rr && x < w; x++) {
                size_t i = (size_t)y * (size_t)w + (size_t)x;
                float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
                bool in_corner = (c & 1 ? dx > 0 : dx < 0) && (c & 2 ? dy > 0 : dy < 0);
                float cov = 1.0f;
                if (in_corner)
                    cov = fminf(fmaxf((float)r - hypotf(dx, dy) + 0.5f, 0.0f), 1.0f);
                if (cov >= 1.0f) {
                    wb->ghost.px[i] = wb->clean[i];
                } else {
                    uint32_t p = wb->clean[i];
                    uint32_t a = (uint32_t)((float)(p >> 24) * cov), rch = (uint32_t)((float)((p >> 16) & 255) * cov), g = (uint32_t)((float)((p >> 8) & 255) * cov),
                             b = (uint32_t)((float)(p & 255) * cov);
                    wb->ghost.px[i] = (a << 24) | (rch << 16) | (g << 8) | b;
                }
            }
    }
    wb->round_applied = r;
    lm_render_ghost_changed(wm->srv->render, &wb->ghost);
}

/* A fresh picture of the window (it is taken again now and then): the new one is rounded like the old one was. */
static void wobble_set_picture(struct lm_wm *wm, struct wm_wobble *wb, struct lm_ghost *g)
{
    lm_render_ghost_free(wm->srv->render, &wb->ghost);
    free(wb->clean);
    wb->ghost = *g;
    wb->clean = g->px ? malloc((size_t)g->w * (size_t)g->h * sizeof(uint32_t)) : NULL;
    if (wb->clean)
        memcpy(wb->clean, g->px, (size_t)g->w * (size_t)g->h * sizeof(uint32_t));
    wb->round_applied = 0;
    wobble_round(wm, wb, (int)(wb->round_f + 0.5f));
    mips_free(&wb->m);
    wb->m.soft = true;
    mips_build(&wb->m, wb->ghost.px, wb->ghost.w, wb->ghost.h);
}

void wobble_stop(struct lm_wm *wm)
{
    struct wm_wobble *wb = &wm->wb;
    if (!wb->active)
        return;
    struct lm_rect d = wb->shown;
    if (wb->win) {
        wb->win->anim_hidden = false;
        struct lm_rect f = frame_outer(wb->win);
        d = lm_rect_union(&d, &f);
    }
    lm_render_ghost_free(wm->srv->render, &wb->frame);
    mips_free(&wb->m);
    lm_render_ghost_free(wm->srv->render, &wb->ghost);
    free(wb->clean);
    memset(wb, 0, sizeof(*wb));
    damage_rect(wm, &d);
}

/* The window is being dragged: from now on it wobbles. (px, py) is the grab point. */
void wobble_start(struct lm_wm *wm, struct lm_window *w, double px, double py)
{
    struct wm_wobble *wb = &wm->wb;
    struct lm_ghost g;
    struct lm_rect home;
    if (wb->active)
        wobble_stop(wm);
    if (!wobble_wanted(wm, w) || !anim_prepare(wm, w, &g, &home))
        return;
    memset(wb, 0, sizeof(*wb));
    wb->active = true;
    wb->win = w;
    wb->ghost = g;
    wb->clean = g.px ? malloc((size_t)g.w * (size_t)g.h * sizeof(uint32_t)) : NULL;
    if (wb->clean)
        memcpy(wb->clean, g.px, (size_t)g.w * (size_t)g.h * sizeof(uint32_t));
    wb->alpha = 214;                                             /* a little see-through while it is held */
    wb->off_x = home.x - w->x;
    wb->off_y = home.y - w->y;
    wb->gx = (float)px - (float)home.x;
    wb->gy = (float)py - (float)home.y;
    for (int j = 0; j <= WB_ROWS; j++)
        for (int i = 0; i <= WB_COLS; i++)
            wobble_rest(wb, i, j, &wb->x[j * (WB_COLS + 1) + i], &wb->y[j * (WB_COLS + 1) + i]);
    memcpy(wb->px, wb->x, sizeof(wb->px));
    memcpy(wb->py, wb->y, sizeof(wb->py));
    wb->round_f = fminf(8.0f, (float)LM_MIN(wb->ghost.w, wb->ghost.h) / 12.0f);        /* the corners are a little soft while it is held */
    wobble_round(wm, wb, (int)(wb->round_f + 0.5f));
    wb->m.soft = true;
    mips_build(&wb->m, wb->ghost.px, wb->ghost.w, wb->ghost.h);
    wb->last = lm_now_ms();
    wb->shown = wobble_bounds(wb);
    wobble_render(wm, wb);
    w->anim_hidden = true;
    damage_rect(wm, &wb->shown);
    if (wm->anim_timer && !lm_timer_is_armed(wm->anim_timer))
        lm_timer_arm_ms(wm->anim_timer, ANIM_TICK_MS, ANIM_TICK_MS);
}

/* The ripples: a height field `u` on the same grid with the wave equation (u'' = c^2 * laplacian - damping * u'), the distances in pixels so that the
 * waves are round on a window whose cells are not square. The window being moved stirs it where it is held (the faster, the more); the edges bounce the
 * waves back (a missing neighbour counts as equal), and they die out in a second or two. The picture is then drawn bent along the slope of the surface:
 * every node moves a little towards where the water leans, which is what a ripple looks like when seen through it. */
static void wobble_ripples(struct wm_wobble *wb, float dt, float ox, float oy, float strength)
{
    const int stride = WB_COLS + 1;
    float dx = fmaxf((float)wb->ghost.w / (float)WB_COLS, 1.0f), dy = fmaxf((float)wb->ghost.h / (float)WB_ROWS, 1.0f);
    float cmax = 0.9f / (dt * sqrtf(1.0f / (dx * dx) + 1.0f / (dy * dy)));
    float c = fminf(520.0f, cmax), c2 = c * c;
    static float lap[WB_N];
    for (int j = 0; j <= WB_ROWS; j++)
        for (int i = 0; i <= WB_COLS; i++) {
            int n = j * stride + i;
            float l = wb->u[i > 0 ? n - 1 : n], r = wb->u[i < WB_COLS ? n + 1 : n], t = wb->u[j > 0 ? n - stride : n], b = wb->u[j < WB_ROWS ? n + stride : n];
            lap[n] = (l + r - 2.0f * wb->u[n]) / (dx * dx) + (t + b - 2.0f * wb->u[n]) / (dy * dy);
        }
    if (wb->have_last && !wb->released) {                          /* stirred at the grab point by how fast the window moves */
        float sp = hypotf(ox - wb->last_ox, oy - wb->last_oy) / dt;
        float gi = wb->gx / dx, gj = wb->gy / dy;
        for (int j = 0; j <= WB_ROWS; j++)
            for (int i = 0; i <= WB_COLS; i++) {
                float d2 = ((float)i - gi) * ((float)i - gi) * (dx * dx) + ((float)j - gj) * ((float)j - gj) * (dy * dy);
                float wgt = expf(-d2 / (2.0f * 40.0f * 40.0f));
                if (wgt > 0.01f)
                    wb->uv[j * stride + i] += fminf(sp, 1800.0f) * 0.3f * strength * wgt * dt;
            }
    }
    float speed = wb->have_last ? hypotf(ox - wb->last_ox, oy - wb->last_oy) / dt : 0.0f;
    float amp_target = wb->released ? 0.0f : fminf(speed, 1200.0f) / 1200.0f * 30.0f * (0.5f + strength);
    wb->amp += (amp_target - wb->amp) * fminf(1.0f, dt * (amp_target > wb->amp ? 10.0f : 3.0f));
    wb->wave_t += dt;
    wb->last_ox = ox;
    wb->last_oy = oy;
    wb->have_last = true;
    float damp = wb->released ? 9.0f : 5.0f;                         /* (once it is let go the water calms faster) */
    for (int n = 0; n < WB_N; n++) {
        wb->uv[n] += (c2 * lap[n] - 60.0f * wb->u[n] - damp * wb->uv[n]) * dt;      /* (the spring to zero keeps the long, slow waves from lingering) */
        wb->u[n] += wb->uv[n] * dt;
    }
    float gain = 130.0f * strength;                                  /* pixels of bending per unit of slope */
    for (int j = 0; j <= WB_ROWS; j++)
        for (int i = 0; i <= WB_COLS; i++) {
            int n = j * stride + i;
            int il = i > 0 ? n - 1 : n, ir = i < WB_COLS ? n + 1 : n, jt = j > 0 ? n - stride : n, jb = j < WB_ROWS ? n + stride : n;
            float sx = (wb->u[ir] - wb->u[il]) / (((i > 0) + (i < WB_COLS)) * dx), sy = (wb->u[jb] - wb->u[jt]) / (((j > 0) + (j < WB_ROWS)) * dy);
            float ex = LM_CLAMP(gain * sx, -0.45f * dx, 0.45f * dx), ey = LM_CLAMP(gain * sy, -0.45f * dy, 0.45f * dy);
            /* the sheet waves: one runs along the window sideways and moves it up and down, one runs down it and moves it sideways (more far from where it is held) */
            float far = fminf(hypotf((float)i * dx - wb->gx, (float)j * dy - wb->gy) / (0.7f * hypotf((float)wb->ghost.w, (float)wb->ghost.h)), 1.0f);
            float aw = wb->amp * (0.4f + 0.6f * far);
            float wy = aw * sinf(6.2832f * ((float)i * dx / 210.0f - 2.6f * wb->wave_t));
            float wx = aw * 0.8f * sinf(6.2832f * ((float)j * dy / 170.0f - 3.0f * wb->wave_t) + 1.3f);
            wb->px[n] = wb->x[n] + ex + wx;
            wb->py[n] = wb->y[n] + ey + wy;
        }
}

/* One physics step of dt seconds: every node is a damped spring to its rigid place, soft far from the grab point, and drawn
 * towards the mean of its neighbours (so the bend travels through the window like a wave). */
static void wobble_step(struct wm_wobble *wb, float dt)
{
    const int stride = WB_COLS + 1;
    float s = (float)lm_metrics.wobble / 100.0f;
    float kn = 1600.0f, kf = 1600.0f * powf(0.07f, s), reach = 0.7f * hypotf((float)wb->ghost.w, (float)wb->ghost.h);
    float ox = (float)(wb->win->x + wb->off_x), oy = (float)(wb->win->y + wb->off_y);
    static float dx[WB_N], dy[WB_N];                            /* displacement from the rigid place */
    float rx[WB_N], ry[WB_N];
    for (int j = 0; j <= WB_ROWS; j++)
        for (int i = 0; i <= WB_COLS; i++) {
            int n = j * stride + i;
            wobble_rest(wb, i, j, &rx[n], &ry[n]);
            dx[n] = wb->x[n] - rx[n];
            dy[n] = wb->y[n] - ry[n];
        }
    for (int j = 0; j <= WB_ROWS; j++)
        for (int i = 0; i <= WB_COLS; i++) {
            int n = j * stride + i, cnt = 0;
            float ax = 0, ay = 0;
            if (i > 0) { ax += dx[n - 1]; ay += dy[n - 1]; cnt++; }
            if (i < WB_COLS) { ax += dx[n + 1]; ay += dy[n + 1]; cnt++; }
            if (j > 0) { ax += dx[n - stride]; ay += dy[n - stride]; cnt++; }
            if (j < WB_ROWS) { ax += dx[n + stride]; ay += dy[n + stride]; cnt++; }
            ax = ax / (float)cnt - dx[n];
            ay = ay / (float)cnt - dy[n];
            float w = fminf(hypotf(rx[n] - ox - wb->gx, ry[n] - oy - wb->gy) / reach, 1.0f);
            float k = kn + (kf - kn) * w, c = 2.0f * 0.26f * sqrtf(k);
            wb->vx[n] += (-k * dx[n] - c * wb->vx[n] + 90.0f * ax) * dt;
            wb->vy[n] += (-k * dy[n] - c * wb->vy[n] + 90.0f * ay) * dt;
        }
    for (int n = 0; n < WB_N; n++) {
        wb->x[n] += wb->vx[n] * dt;
        wb->y[n] += wb->vy[n] * dt;
    }
    wobble_ripples(wb, dt, ox, oy, s);
}

void wobble_tick(struct lm_wm *wm, uint64_t now)
{
    struct wm_wobble *wb = &wm->wb;
    float dt = fminf((float)(now - wb->last) / 1000.0f, 0.05f);
    wb->last = now;
    for (float t = 0; t < dt; t += 0.008f)
        wobble_step(wb, fminf(0.008f, dt - t));
    if (++wb->ticks % 6 == 0) {                                 /* the content stays alive: a fresh picture every ~100 ms */
        struct lm_ghost g;
        struct lm_rect home;
        if (anim_prepare(wm, wb->win, &g, &home)) {
            if (g.w == wb->ghost.w && g.h == wb->ghost.h) {
                wobble_set_picture(wm, wb, &g);
            } else {
                lm_render_ghost_free(wm->srv->render, &g);
            }
        }
    }
    struct lm_rect old = wb->shown;
    wb->shown = wobble_bounds(wb);
    wobble_render(wm, wb);
    struct lm_rect d = lm_rect_union(&old, &wb->shown);
    damage_rect(wm, &d);
    if (wb->released) {                                         /* settled when every node is back and still */
        float dev = 0, spd = 0;
        for (int j = 0; j <= WB_ROWS; j++)
            for (int i = 0; i <= WB_COLS; i++) {
                int n = j * (WB_COLS + 1) + i;
                float rx, ry;
                wobble_rest(wb, i, j, &rx, &ry);
                dev = fmaxf(dev, fmaxf(fabsf(wb->x[n] - rx), fabsf(wb->y[n] - ry)));
                dev = fmaxf(dev, fabsf(wb->u[n]) * 0.6f);              /* (the ripples have to die out, too) */
                spd = fmaxf(spd, fmaxf(fabsf(wb->vx[n]), fabsf(wb->vy[n])));
                spd = fmaxf(spd, fabsf(wb->uv[n]) * 0.3f);
            }
        if (dev < 0.4f && spd < 6.0f && wb->amp < 0.4f)
            wobble_stop(wm);
    }
}
