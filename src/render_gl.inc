/*
 * render_gl.inc - the GL renderer (included by render.c, which owns the statics it uses). Hybrid: the client surfaces are
 * composited by the GPU, everything MellaGUI draws itself stays the proven software painter and reaches the GPU as textures.
 *
 *   per output, every frame, from the bottom:
 *     1. the background texture: wallpaper and gadgets, painted on the CPU into an output-sized buffer (only the damaged
 *        rectangles) and uploaded;
 *     2. the scene items in order: a window frame = three border strips and the client's background colour as solid
 *        rectangles plus the title bar as a texture (painted by paint_frame into a scratch buffer, reused while its hash is
 *        the same); a surface tree = one textured quad per (sub)surface, the texture being the surface's own `tex` uploaded
 *        when it changed (surface.c keeps gl_full / gl_damage); the panel, the menu and the lock screen = textures painted
 *        by the software painters into scratch buffers when damage touches them;
 *     3. the screenshot flash (a white quad) and card (a texture painted twice, over black and over white, from which the
 *        alpha is recovered: the painters never need to know about destination alpha), the cursor (a sprite texture or the
 *        client's cursor surface).
 *   The whole output is redrawn each frame (a GPU does not mind); only the CPU-side textures are damage driven.
 *   Headless: the offscreen target is read back into the backend's memory framebuffer, so dump / tests see the same pixels.
 */

#define GL_MAX_FRAMES 48

struct gl_chrome {
    unsigned tex;
    int w, h;
    uint64_t hash;
};

struct gl_out {
    struct lm_gl_target tgt;
    int w, h;
    uint32_t *bg;                       /* wallpaper + gadgets, output sized */
    unsigned bg_tex;
    bool bg_ok;
    struct gl_chrome frame[GL_MAX_FRAMES];
    struct gl_chrome panel, menu, lock, card;
    uint32_t card_gen;
    bool unread;                        /* rendered since the last readback */
};

static void gl_chrome_free(struct lm_renderer *r, struct gl_chrome *c)
{
    if (c->tex)
        lm_gl_tex_delete(r->gl, c->tex);
    memset(c, 0, sizeof(*c));
}

static void gl_out_free(struct lm_renderer *r, struct out_state *st)
{
    struct gl_out *go = st->gl;
    if (!go)
        return;
    lm_gl_make_current(r->gl);
    lm_gl_target_destroy(r->gl, &go->tgt);
    if (go->bg_tex)
        lm_gl_tex_delete(r->gl, go->bg_tex);
    free(go->bg);
    for (int i = 0; i < GL_MAX_FRAMES; i++)
        gl_chrome_free(r, &go->frame[i]);
    gl_chrome_free(r, &go->panel);
    gl_chrome_free(r, &go->menu);
    gl_chrome_free(r, &go->lock);
    gl_chrome_free(r, &go->card);
    free(go);
    st->gl = NULL;
}

static struct gl_out *gl_out_get(struct lm_renderer *r, struct out_state *st)
{
    struct lm_output *out = st->out;
    struct gl_out *go = st->gl;
    if (go && (go->w != out->width || go->h != out->height)) {      /* the mode changed */
        gl_out_free(r, st);
        go = NULL;
    }
    if (go)
        return go;
    lm_gl_make_current(r->gl);
    go = calloc(1, sizeof(*go));
    if (!go || !lm_gl_target_create(r->gl, out->width, out->height, &go->tgt)) {
        free(go);
        lm_error("gl: cannot create the %dx%d render target of %s", out->width, out->height, out->name);
        return NULL;
    }
    go->w = out->width;
    go->h = out->height;
    go->bg = calloc((size_t)go->w * (size_t)go->h, 4);
    go->bg_tex = lm_gl_tex_create(r->gl, go->w, go->h, NULL);
    if (!go->bg || !go->bg_tex) {
        st->gl = go;
        gl_out_free(r, st);
        return NULL;
    }
    st->gl = go;
    st->full = true;
    return go;
}

/* ---- the background ----------------------------------------------------------------------------------------------- */

static bool gl_bg_cb(const struct lm_scene_item *item, void *data)
{
    struct paint_ctx *c = data;
    if (item->kind == LM_SCENE_WALLPAPER) {
        if (item->output == c->out)
            paint_wallpaper(c);
    } else if (item->kind == LM_SCENE_GADGET) {
        paint_gadget(c, &item->u.gadget);
    }
    return true;
}

/* True when one opaque surface covers `rect` entirely: the background cannot show there, so it is not repainted or uploaded
 * (the area is damaged again when the window moves away, and the background is painted from the then current state). */
struct gl_cover {
    struct lm_rect rect;
    bool covered;
};

static bool gl_cover_cb(const struct lm_scene_item *item, void *data)
{
    struct gl_cover *g = data;
    if (item->kind != LM_SCENE_SURFACE)
        return true;
    struct lm_rect r;
    if (surface_opaque_rect(item->u.surface.root, item->u.surface.x, item->u.surface.y, &g->rect, &r) > 0 &&
        r.w == g->rect.w && r.h == g->rect.h) {
        g->covered = true;
        return false;
    }
    return true;
}

static void gl_bg_update(struct lm_renderer *r, struct out_state *st, struct gl_out *go, const struct lm_region *redraw)
{
    struct lm_framebuffer fb = { .pixels = go->bg, .width = go->w, .height = go->h, .stride = go->w * 4 };
    struct lm_rect orect = lm_output_rect(st->out);
    for (int i = 0; i < redraw->n; i++) {
        struct lm_rect rc;
        if (!lm_rect_intersect(&redraw->rects[i], &orect, &rc))
            continue;
        if (r->srv->wm) {
            struct gl_cover g = { .rect = rc };
            lm_wm_scene_for_each(r->srv->wm, &rc, gl_cover_cb, &g);
            if (g.covered)
                continue;
        }
        struct paint_ctx c = { .r = r, .out = st->out, .fb = &fb, .clip = rc, .cov_idx = -1 };
        if (r->srv->wm)
            lm_wm_scene_for_each(r->srv->wm, &rc, gl_bg_cb, &c);
        else
            paint_wallpaper(&c);
        int lx = rc.x - orect.x, ly = rc.y - orect.y;
        lm_gl_tex_update(r->gl, go->bg_tex, lx, ly, rc.w, rc.h, go->bg + (size_t)ly * (size_t)go->w + (size_t)lx, go->w);
    }
}

/* ---- chrome painted by the software painters ------------------------------------------------------------------------- */

struct gl_pass {
    struct lm_renderer *r;
    struct out_state *st;
    struct gl_out *go;
    const struct lm_region *dirty;      /* what changed since the last frame (global) */
    bool full;
    int fidx;                           /* frames seen so far in this pass */
    int ox, oy;                         /* the output's origin: global -> target coordinates */
};

typedef void (*gl_paint_fn)(const struct paint_ctx *c, const void *item);

static void gl_do_frame(const struct paint_ctx *c, const void *it) { paint_frame(c, it); }
static void gl_do_panel(const struct paint_ctx *c, const void *it) { paint_panel(c, it); }
static void gl_do_menu(const struct paint_ctx *c, const void *it) { paint_menu(c, it); }
static void gl_do_lock(const struct paint_ctx *c, const void *it) { paint_lock(c, it); }

static bool gl_dirty(const struct gl_pass *p, const struct lm_rect *rect)
{
    if (p->full)
        return true;
    for (int i = 0; i < p->dirty->n; i++)
        if (lm_rect_overlaps(&p->dirty->rects[i], rect))
            return true;
    return false;
}

/* Paints `item` with `fn` into a scratch buffer of `rect` (global) and (re)uploads it into `ch`. Returns the texture. */
static unsigned gl_scratch(struct lm_renderer *r, struct out_state *st, struct gl_chrome *ch, const struct lm_rect *rect,
                           gl_paint_fn fn, const void *item)
{
    int w = rect->w, h = rect->h;
    if (w <= 0 || h <= 0)
        return 0;
    uint32_t *pix = calloc((size_t)w * (size_t)h, 4);
    if (!pix)
        return 0;
    struct lm_output fake = *st->out;                   /* the painters only look at its origin and size */
    fake.x = rect->x;
    fake.y = rect->y;
    fake.width = w;
    fake.height = h;
    struct lm_framebuffer fb = { .pixels = pix, .width = w, .height = h, .stride = w * 4 };
    struct paint_ctx c = { .r = r, .out = &fake, .fb = &fb, .clip = *rect, .cov_idx = -1 };
    fn(&c, item);
    if (ch->tex && (ch->w != w || ch->h != h)) {
        lm_gl_tex_delete(r->gl, ch->tex);
        ch->tex = 0;
    }
    if (!ch->tex)
        ch->tex = lm_gl_tex_create(r->gl, w, h, pix);
    else
        lm_gl_tex_update(r->gl, ch->tex, 0, 0, w, h, pix, w);
    ch->w = w;
    ch->h = h;
    free(pix);
    return ch->tex;
}

static void gl_tex_at(struct gl_pass *p, unsigned tex, const struct lm_rect *rect, bool opaque)
{
    lm_gl_draw_tex(p->r->gl, tex, rect->x - p->ox, rect->y - p->oy, rect->w, rect->h, 0, 0, 1, 1, 1.0f, opaque);
}

static void gl_solid(struct gl_pass *p, const struct lm_rect *rect, uint32_t rgb)
{
    if (rect->w <= 0 || rect->h <= 0)
        return;
    lm_gl_draw_rect(p->r->gl, rect->x - p->ox, rect->y - p->oy, rect->w, rect->h, (float)((rgb >> 16) & 255) / 255.0f,
                    (float)((rgb >> 8) & 255) / 255.0f, (float)(rgb & 255) / 255.0f, 1.0f);
}

static uint64_t fnv(uint64_t h, const void *data, size_t n)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static uint64_t hash_frame(const struct lm_frame_info *f)
{
    uint64_t h = 1469598103934665603ull;
    const struct lm_rect *rs[] = { &f->outer, &f->titlebar, &f->btn_close, &f->btn_max, &f->btn_min, &f->title_text };
    for (size_t i = 0; i < sizeof(rs) / sizeof(rs[0]); i++)
        h = fnv(h, rs[i], sizeof(*rs[i]));
    const char *t = f->title ? f->title : "";
    h = fnv(h, t, strlen(t) + 1);
    uint32_t flags = (f->active ? 1u : 0) | (f->maximized ? 2u : 0);
    h = fnv(h, &flags, sizeof(flags));
    h = fnv(h, &f->hover, sizeof(f->hover));
    return fnv(h, &f->pressed, sizeof(f->pressed));
}

static void gl_frame(struct gl_pass *p, const struct lm_frame_info *f)
{
    struct frame_parts fp;
    frame_parts(f, &fp);
    if (fp.has_body) {
        gl_solid(p, &fp.left, fp.col);
        gl_solid(p, &fp.right, fp.col);
        gl_solid(p, &fp.bottom, fp.col);
        gl_solid(p, &fp.body, fp.body_col);
    }
    struct gl_chrome own = { 0 };                       /* beyond the cache: painted every time */
    struct gl_chrome *ch = p->fidx < GL_MAX_FRAMES ? &p->go->frame[p->fidx] : &own;
    p->fidx++;
    uint64_t h = hash_frame(f);
    if (!ch->tex || ch->hash != h || ch->w != f->titlebar.w || ch->h != f->titlebar.h) {
        gl_scratch(p->r, p->st, ch, &f->titlebar, gl_do_frame, f);
        ch->hash = h;
    }
    if (ch->tex)
        gl_tex_at(p, ch->tex, &f->titlebar, true);
    if (ch == &own)
        gl_chrome_free(p->r, &own);
}

static void gl_chrome_item(struct gl_pass *p, struct gl_chrome *ch, const struct lm_rect *rect, gl_paint_fn fn, const void *item)
{
    if (!ch->tex || ch->w != rect->w || ch->h != rect->h || gl_dirty(p, rect))
        gl_scratch(p->r, p->st, ch, rect, fn, item);
    if (ch->tex)
        gl_tex_at(p, ch->tex, rect, true);
}

/* ---- client surfaces ------------------------------------------------------------------------------------------------ */

/* Brings the GL texture of `s` up to date with s->tex (new texture, whole copy, or just the damaged rectangles). */
static void gl_sync_tex(struct lm_renderer *r, struct lm_surface *s)
{
    int w = s->tex.width, h = s->tex.height;
    if (!s->gl_tex || s->gl_w != w || s->gl_h != h) {
        if (s->gl_tex)
            lm_gl_tex_delete(r->gl, s->gl_tex);
        s->gl_tex = lm_gl_tex_create(r->gl, w, h, s->tex.pixels);
        s->gl_w = w;
        s->gl_h = h;
    } else if (s->gl_full) {
        lm_gl_tex_update(r->gl, s->gl_tex, 0, 0, w, h, s->tex.pixels, w);
    } else {
        for (int i = 0; i < s->gl_damage.n; i++) {
            struct lm_rect q = s->gl_damage.rects[i], whole = lm_rect_make(0, 0, w, h), c;
            if (lm_rect_intersect(&q, &whole, &c))
                lm_gl_tex_update(r->gl, s->gl_tex, c.x, c.y, c.w, c.h, s->tex.pixels + (size_t)c.y * (size_t)w + (size_t)c.x, w);
        }
    }
    s->gl_full = false;
    lm_region_clear(&s->gl_damage);
}

static bool gl_visit(struct lm_surface *s, int32_t x, int32_t y, void *data)
{
    struct gl_pass *p = data;
    if (!s->has_content || !s->tex.pixels || s->width <= 0 || s->height <= 0)
        return true;
    struct lm_rect where = lm_rect_make(x, y, s->width, s->height), orect = lm_output_rect(p->st->out);
    if (!lm_rect_overlaps(&where, &orect))
        return true;
    gl_sync_tex(p->r, s);
    if (s->gl_tex)
        lm_gl_draw_tex(p->r->gl, s->gl_tex, x - p->ox, y - p->oy, s->width, s->height, 0, 0, 1, 1, 1.0f,
                       s->tex.opaque || s->tex.solid);
    return true;
}

static bool gl_scene_cb(const struct lm_scene_item *item, void *data)
{
    struct gl_pass *p = data;
    switch (item->kind) {
    case LM_SCENE_WALLPAPER:
    case LM_SCENE_GADGET:
        break;                                          /* in the background texture */
    case LM_SCENE_FRAME:
        gl_frame(p, &item->u.frame);
        break;
    case LM_SCENE_SURFACE:
        lm_surface_tree_visit(item->u.surface.root, item->u.surface.x, item->u.surface.y, gl_visit, p);
        break;
    case LM_SCENE_PANEL:
        if (item->output == p->st->out)
            gl_chrome_item(p, &p->go->panel, &item->u.panel.bar, gl_do_panel, &item->u.panel);
        break;
    case LM_SCENE_MENU:
        gl_chrome_item(p, &p->go->menu, &item->u.menu.rect, gl_do_menu, &item->u.menu);
        break;
    case LM_SCENE_LOCK:
        if (item->output == p->st->out) {
            struct lm_rect o = lm_output_rect(p->st->out);
            gl_chrome_item(p, &p->go->lock, &o, gl_do_lock, &item->u.lock);
        }
        break;
    }
    return true;
}

/* ---- overlays: screenshot flash and card, the cursor ------------------------------------------------------------------ */

static void gl_card_paint(struct lm_renderer *r, struct out_state *st, struct gl_chrome *ch)
{
    const struct shot_fx *fx = &r->shot;
    struct lm_rect cr = shot_card_rect(fx);
    int w = cr.w, h = cr.h;
    size_t n = (size_t)w * (size_t)h;
    uint32_t *black = malloc(n * 4), *white = malloc(n * 4);
    if (black && white) {
        for (size_t i = 0; i < n; i++) {
            black[i] = 0x000000u;
            white[i] = 0xffffffu;
        }
        uint32_t *bufs[2] = { black, white };
        for (int k = 0; k < 2; k++) {                   /* the same painter over two known backgrounds */
            struct lm_output fake = *st->out;
            fake.x = cr.x;
            fake.y = cr.y;
            fake.width = w;
            fake.height = h;
            struct lm_framebuffer fb = { .pixels = bufs[k], .width = w, .height = h, .stride = w * 4 };
            struct paint_ctx c = { .r = r, .out = &fake, .fb = &fb, .clip = cr, .cov_idx = -1 };
            paint_shot_card(&c, fx);
        }
        for (size_t i = 0; i < n; i++) {                /* over black: premultiplied colour; white - black = 255 * (1 - alpha) */
            uint32_t b = black[i], wh = white[i];
            int d = (int)((((wh >> 16) & 255) - ((b >> 16) & 255)) + (((wh >> 8) & 255) - ((b >> 8) & 255)) +
                          ((wh & 255) - (b & 255))) / 3;
            int a = LM_CLAMP(255 - d, 0, 255);
            uint32_t rr = LM_MIN((b >> 16) & 255, (uint32_t)a), gg = LM_MIN((b >> 8) & 255, (uint32_t)a), bb = LM_MIN(b & 255, (uint32_t)a);
            black[i] = ((uint32_t)a << 24) | (rr << 16) | (gg << 8) | bb;
        }
        if (ch->tex && (ch->w != w || ch->h != h)) {
            lm_gl_tex_delete(r->gl, ch->tex);
            ch->tex = 0;
        }
        if (!ch->tex)
            ch->tex = lm_gl_tex_create(r->gl, w, h, black);
        else
            lm_gl_tex_update(r->gl, ch->tex, 0, 0, w, h, black, w);
        ch->w = w;
        ch->h = h;
    }
    free(black);
    free(white);
}

static void gl_overlays(struct gl_pass *p)
{
    struct lm_renderer *r = p->r;
    const struct shot_fx *fx = &r->shot;
    if (!fx->active || fx->out != p->st->out || (r->srv->lock && lm_lock_active(r->srv->lock)))
        return;
    int a = shot_flash_alpha(fx);
    if (a > 0) {
        float f = (float)a / 255.0f;
        struct lm_rect orect = lm_output_rect(p->st->out), area;
        if (lm_rect_intersect(&fx->area, &orect, &area))
            lm_gl_draw_rect(r->gl, area.x - p->ox, area.y - p->oy, area.w, area.h, f, f, f, f);
    }
    struct gl_out *go = p->go;
    if (!go->card.tex || go->card_gen != fx->gen) {
        gl_card_paint(r, p->st, &go->card);
        go->card_gen = fx->gen;
    }
    if (go->card.tex) {
        struct lm_rect cr = shot_card_rect(fx);
        lm_gl_draw_tex(r->gl, go->card.tex, cr.x - p->ox, cr.y - p->oy, cr.w, cr.h, 0, 0, 1, 1, 1.0f, false);
    }
}

static void gl_cursor(struct gl_pass *p)
{
    struct lm_renderer *r = p->r;
    struct cursor_view v;
    cursor_view(r, &v);
    struct lm_rect orect = lm_output_rect(p->st->out);
    if (!v.visible || !lm_rect_overlaps(&v.rect, &orect))
        return;
    if (v.surface) {
        lm_surface_tree_visit(v.surface, v.origin_x, v.origin_y, gl_visit, p);
        return;
    }
    int shape = (int)(v.sprite - sprites);
    if (shape < 0 || shape > LM_CURSOR_POINTER)
        return;
    if (!r->sprite_tex[shape])
        r->sprite_tex[shape] = lm_gl_tex_create(r->gl, SPR_W, SPR_H, v.sprite->px);
    lm_gl_draw_tex(r->gl, r->sprite_tex[shape], v.rect.x - p->ox, v.rect.y - p->oy, SPR_W, SPR_H, 0, 0, 1, 1, 1.0f, false);
}

/* ---- one frame ----------------------------------------------------------------------------------------------------- */

static bool draw_output_gl(struct lm_renderer *r, struct out_state *st, bool force)
{
    struct lm_output *out = st->out;
    if (!force && !st->ready)
        return false;
    bool has_damage = !lm_region_is_empty(&st->pending) || st->full;
    if (!has_damage)
        return false;
    struct gl_out *go = gl_out_get(r, st);
    if (!go)
        return false;
    lm_gl_make_current(r->gl);

    struct lm_rect orect = lm_output_rect(out);
    struct lm_region redraw;
    lm_region_init(&redraw);
    bool full = st->full || !go->bg_ok;
    if (full)
        lm_region_add_rect(&redraw, &orect);
    else
        lm_region_add_region(&redraw, &st->pending);
    lm_region_intersect_rect(&redraw, &orect);

    gl_bg_update(r, st, go, &redraw);
    go->bg_ok = true;

    lm_gl_bind_target(r->gl, &go->tgt, go->w, go->h);
    lm_gl_scissor(r->gl, false, 0, 0, 0, 0);
    lm_gl_draw_tex(r->gl, go->bg_tex, 0, 0, go->w, go->h, 0, 0, 1, 1, 1.0f, true);
    struct gl_pass p = { .r = r, .st = st, .go = go, .dirty = &redraw, .full = full, .ox = out->x, .oy = out->y };
    if (r->srv->wm)
        lm_wm_scene_for_each(r->srv->wm, &orect, gl_scene_cb, &p);
    gl_overlays(&p);
    gl_cursor(&p);

    go->unread = true;                                  /* headless: copied to the memory framebuffer when something reads it */
    lm_output_present(out, NULL, 0);

    lm_region_clear(&st->pending);
    lm_region_fini(&redraw);
    st->full = false;
    st->ready = false;
    st->presented = true;
    return true;
}

/* The headless backend is about to read its memory framebuffer (dump): copy what the GPU drew into it. */
void lm_render_readback(struct lm_renderer *r)
{
    if (!r->gl || !r->srv->backend)
        return;
    struct lm_output *o;
    lm_list_for_each(o, &r->srv->backend->outputs, link) {
        struct out_state *st = state_of(o);
        struct gl_out *go = st ? st->gl : NULL;
        if (!go || !go->unread)
            continue;
        struct lm_framebuffer *fb = lm_output_get_framebuffer(o);
        lm_gl_make_current(r->gl);
        if (fb && fb->pixels)
            lm_gl_readback(r->gl, &go->tgt, fb->pixels, (size_t)fb->stride);
        go->unread = false;
    }
}
