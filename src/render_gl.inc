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
    int w, h;                           /* the LOGICAL size of what it shows */
    int pw, ph;                         /* the texture's own size: physical pixels */
    uint64_t hash;
};

struct gl_out {
    struct lm_gl_target tgt;
    int w, h;
    uint32_t *bg;                       /* wallpaper + gadgets, output sized */
    unsigned bg_tex;
    bool bg_ok;
    struct gl_chrome frame[GL_MAX_FRAMES];
    struct gl_chrome panel, shadow, lock, card, switcher, power, about, launcher, dock, menu, osk, overview;
    struct gl_chrome notify[LM_NOTIFY_MAX];
    uint32_t card_gen;
    uint32_t theme_gen;                 /* the palette, the font and the scale the chromes below were painted with */
    struct lm_gl_target mag;            /* the scene before the magnifier zooms it (made when it is first needed) */
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
    if (go->tgt.fbo)
        lm_gl_target_destroy(r->gl, &go->tgt);
    if (go->mag.fbo)
        lm_gl_target_destroy(r->gl, &go->mag);
    if (go->bg_tex)
        lm_gl_tex_delete(r->gl, go->bg_tex);
    free(go->bg);
    for (int i = 0; i < GL_MAX_FRAMES; i++)
        gl_chrome_free(r, &go->frame[i]);
    gl_chrome_free(r, &go->panel);
    gl_chrome_free(r, &go->shadow);
    gl_chrome_free(r, &go->about);
    gl_chrome_free(r, &go->launcher);
    gl_chrome_free(r, &go->lock);
    gl_chrome_free(r, &go->card);
    gl_chrome_free(r, &go->switcher);
    gl_chrome_free(r, &go->power);
    gl_chrome_free(r, &go->dock);
    gl_chrome_free(r, &go->menu);
    gl_chrome_free(r, &go->osk);
    gl_chrome_free(r, &go->overview);
    for (int i = 0; i < LM_NOTIFY_MAX; i++)
        gl_chrome_free(r, &go->notify[i]);
    free(go);
    st->gl = NULL;
}

/* The palette changed (a theme, a colour): everything painted into a texture with the old colours goes. */
static void gl_chromes_free(struct lm_renderer *r, struct gl_out *go)
{
    for (int i = 0; i < GL_MAX_FRAMES; i++)
        gl_chrome_free(r, &go->frame[i]);
    struct gl_chrome *all[] = { &go->panel, &go->shadow, &go->about, &go->launcher, &go->lock, &go->card, &go->switcher, &go->power,
                                &go->dock, &go->menu, &go->osk, &go->overview };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        gl_chrome_free(r, all[i]);
    for (int i = 0; i < LM_NOTIFY_MAX; i++)
        gl_chrome_free(r, &go->notify[i]);
}

static struct gl_out *gl_out_get(struct lm_renderer *r, struct out_state *st)
{
    struct lm_output *out = st->out;
    struct gl_out *go = st->gl;
    if (go && (go->w != lm_output_pw(out) || go->h != lm_output_ph(out))) {      /* the mode changed */
        gl_out_free(r, st);
        go = NULL;
    }
    if (go)
        return go;
    lm_gl_make_current(r->gl);
    go = calloc(1, sizeof(*go));
    bool window = lm_output_gl_window(out) != NULL;                 /* DRM: it renders into a GBM window, no offscreen target */
    if (!go || (!window && !lm_gl_target_create(r->gl, lm_output_pw(out), lm_output_ph(out), &go->tgt))) {
        free(go);
        lm_error("gl: cannot create the %dx%d render target of %s", lm_output_pw(out), lm_output_ph(out), out->name);
        return NULL;
    }
    go->w = lm_output_pw(out);                          /* everything the GPU draws is in the screen's own pixels */
    go->h = lm_output_ph(out);
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
        ctx_init(&c);
        if (r->srv->wm)
            lm_wm_scene_for_each(r->srv->wm, &rc, gl_bg_cb, &c);
        else
            paint_wallpaper(&c);
        struct lm_rect pr = out_prect(st->out, &rc);                    /* the physical pixels that were painted */
        lm_gl_tex_update(r->gl, go->bg_tex, pr.x, pr.y, pr.w, pr.h, go->bg + (size_t)pr.y * (size_t)go->w + (size_t)pr.x, go->w);
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
    int nidx;                           /* notification cards seen so far */
    int ox, oy;                         /* the output's origin: global -> target coordinates */
};

typedef void (*gl_paint_fn)(const struct paint_ctx *c, const void *item);

static void gl_do_frame(const struct paint_ctx *c, const void *it) { paint_frame(c, it); }
static void gl_do_panel(const struct paint_ctx *c, const void *it) { paint_panel(c, it); }
static void gl_do_shadow(const struct paint_ctx *c, const void *it) { paint_panel_shadow(c, it); }
static void gl_do_about(const struct paint_ctx *c, const void *it) { paint_about(c, it); }
static void gl_do_launcher(const struct paint_ctx *c, const void *it) { paint_launcher(c, it); }
static void gl_do_lock(const struct paint_ctx *c, const void *it) { paint_lock(c, it); }
static void gl_do_switcher(const struct paint_ctx *c, const void *it) { paint_switcher(c, it); }
static void gl_do_power(const struct paint_ctx *c, const void *it) { paint_power(c, it); }
static void gl_do_menu(const struct paint_ctx *c, const void *it) { paint_menu(c, it); }
static void gl_do_dock(const struct paint_ctx *c, const void *it) { paint_dock(c, it); }
static void gl_do_osk(const struct paint_ctx *c, const void *it) { paint_osk(c, it); }
static void gl_do_notify(const struct paint_ctx *c, const void *it) { paint_notify(c, it); }
static void gl_do_overview(const struct paint_ctx *c, const void *it) { paint_overview_ui(c, it); }

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
    if (rect->w <= 0 || rect->h <= 0)
        return 0;
    struct lm_rect pr = out_prect(st->out, rect);       /* the picture is made of the physical pixels the rectangle covers on the screen */
    int w = pr.w, h = pr.h;
    if (w <= 0 || h <= 0)
        return 0;
    uint32_t *pix = calloc((size_t)w * (size_t)h, 4);
    if (!pix)
        return 0;
    struct lm_output fake = *st->out;
    struct lm_framebuffer fb = { .pixels = pix, .width = w, .height = h, .stride = w * 4 };
    struct paint_ctx c = { .r = r, .out = &fake, .fb = &fb, .clip = *rect, .cov_idx = -1 };
    ctx_init(&c);
    c.fbx = pr.x;
    c.fby = pr.y;
    fn(&c, item);
    if (ch->tex && (ch->pw != w || ch->ph != h)) {
        lm_gl_tex_delete(r->gl, ch->tex);
        ch->tex = 0;
    }
    if (!ch->tex)
        ch->tex = lm_gl_tex_create(r->gl, w, h, pix);
    else
        lm_gl_tex_update(r->gl, ch->tex, 0, 0, w, h, pix, w);
    ch->w = rect->w;
    ch->h = rect->h;
    ch->pw = w;
    ch->ph = h;
    free(pix);
    return ch->tex;
}

/* Like gl_scratch for painters that blend (the software painters assume an opaque target): the item is painted twice, over black and
 * over white, and the alpha follows from the difference (white - black = 255 * (1 - alpha)); the texture is premultiplied. */
static void gl_scratch_alpha(struct lm_renderer *r, struct out_state *st, struct gl_chrome *ch, const struct lm_rect *rect,
                             gl_paint_fn fn, const void *item)
{
    struct lm_rect pr = out_prect(st->out, rect);       /* physical, as in gl_scratch */
    int w = pr.w, h = pr.h;
    size_t n = (size_t)w * (size_t)h;
    if (rect->w <= 0 || rect->h <= 0 || w <= 0 || h <= 0)
        return;
    uint32_t *black = malloc(n * 4), *white = malloc(n * 4);
    if (black && white) {
        for (size_t i = 0; i < n; i++) {
            black[i] = 0x000000u;
            white[i] = 0xffffffu;
        }
        uint32_t *bufs[2] = { black, white };
        for (int k = 0; k < 2; k++) {
            struct lm_output fake = *st->out;
            struct lm_framebuffer fb = { .pixels = bufs[k], .width = w, .height = h, .stride = w * 4 };
            struct paint_ctx c = { .r = r, .out = &fake, .fb = &fb, .clip = *rect, .cov_idx = -1 };
            ctx_init(&c);
            c.fbx = pr.x;
            c.fby = pr.y;
            fn(&c, item);
        }
        for (size_t i = 0; i < n; i++) {
            uint32_t b = black[i], wh = white[i];
            int d = (int)((((wh >> 16) & 255) - ((b >> 16) & 255)) + (((wh >> 8) & 255) - ((b >> 8) & 255)) +
                          ((wh & 255) - (b & 255))) / 3;
            int a = LM_CLAMP(255 - d, 0, 255);
            uint32_t rr = LM_MIN((b >> 16) & 255, (uint32_t)a), gg = LM_MIN((b >> 8) & 255, (uint32_t)a), bb = LM_MIN(b & 255, (uint32_t)a);
            black[i] = ((uint32_t)a << 24) | (rr << 16) | (gg << 8) | bb;
        }
        if (ch->tex && (ch->pw != w || ch->ph != h)) {
            lm_gl_tex_delete(r->gl, ch->tex);
            ch->tex = 0;
        }
        if (!ch->tex)
            ch->tex = lm_gl_tex_create(r->gl, w, h, black);
        else
            lm_gl_tex_update(r->gl, ch->tex, 0, 0, w, h, black, w);
        ch->w = rect->w;
        ch->h = rect->h;
        ch->pw = w;
        ch->ph = h;
    }
    free(black);
    free(white);
}

/* A logical (global) rectangle in the framebuffer's physical pixels: where the GPU draws it. */
static inline struct lm_rect gl_pr(const struct gl_pass *p, const struct lm_rect *rect)
{
    return out_prect(p->st->out, rect);
}

static void gl_tex_at(struct gl_pass *p, unsigned tex, const struct lm_rect *rect, bool opaque)
{
    struct lm_rect pr = gl_pr(p, rect);
    lm_gl_draw_tex(p->r->gl, tex, pr.x, pr.y, pr.w, pr.h, 0, 0, 1, 1, 1.0f, opaque);
}

static void gl_solid(struct gl_pass *p, const struct lm_rect *rect, uint32_t rgb)
{
    if (rect->w <= 0 || rect->h <= 0)
        return;
    struct lm_rect pr = gl_pr(p, rect);
    lm_gl_draw_rect(p->r->gl, pr.x, pr.y, pr.w, pr.h, (float)((rgb >> 16) & 255) / 255.0f,
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

/* Zero copy: `s` shows the client's dmabuf through an EGLImage bound to its texture. */
bool lm_render_dmabuf_import(struct lm_renderer *r, struct lm_surface *s, struct lm_buffer *b)
{
    if (!r->gl || !lm_gl_has_dmabuf(r->gl) || !b->dmabuf || b->dmabuf->fd < 0)
        return false;
    uint32_t fourcc = b->format == LM_WL_SHM_FORMAT_XRGB8888 ? LM_DRM_FORMAT_XRGB8888 : LM_DRM_FORMAT_ARGB8888;
    lm_gl_make_current(r->gl);
    void *img = lm_gl_dmabuf_image(r->gl, b->dmabuf->fd, b->width, b->height, b->stride, b->offset, fourcc, b->dmabuf->modifier);
    if (!img)
        return false;
    unsigned tex = (s->gl_tex && s->gl_dmabuf) ? s->gl_tex : lm_gl_tex_alloc(r->gl);
    if (!tex || !lm_gl_tex_bind_image(r->gl, tex, img)) {
        lm_gl_image_destroy(r->gl, img);
        if (tex && tex != s->gl_tex)
            lm_gl_tex_delete(r->gl, tex);
        return false;
    }
    if (s->gl_tex && s->gl_tex != tex)                              /* it was a copied texture: replaced */
        lm_gl_tex_delete(r->gl, s->gl_tex);
    if (s->gl_img)
        lm_gl_image_destroy(r->gl, s->gl_img);                      /* the previous frame's image */
    s->gl_tex = tex;
    s->gl_img = img;
    s->gl_dmabuf = true;
    s->gl_w = b->width;
    s->gl_h = b->height;
    s->gl_full = false;
    lm_region_clear(&s->gl_damage);
    return true;
}

void lm_render_dmabuf_drop(struct lm_renderer *r, struct lm_surface *s)
{
    if (r->gl) {
        lm_gl_make_current(r->gl);
        if (s->gl_img)
            lm_gl_image_destroy(r->gl, s->gl_img);
        if (s->gl_tex)
            lm_gl_tex_delete(r->gl, s->gl_tex);
    }
    s->gl_img = NULL;
    s->gl_tex = 0;
    s->gl_w = s->gl_h = 0;
}

/* Brings the GL texture of `s` up to date with s->tex (new texture, whole copy, or just the damaged rectangles). */
static void gl_sync_tex(struct lm_renderer *r, struct lm_surface *s)
{
    if (s->gl_dmabuf)                                   /* the GPU reads the client's buffer itself */
        return;
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
    if (!s->has_content || (!s->tex.pixels && !s->gl_dmabuf) || s->width <= 0 || s->height <= 0)
        return true;
    struct lm_rect where = lm_rect_make(x, y, s->width, s->height), orect = lm_output_rect(p->st->out);
    if (!lm_rect_overlaps(&where, &orect))
        return true;
    gl_sync_tex(p->r, s);
    if (s->gl_tex) {
        struct lm_rect pr = gl_pr(p, &where);                           /* the GPU maps the buffer onto the physical pixels the surface covers */
        lm_gl_draw_tex(p->r->gl, s->gl_tex, pr.x, pr.y, pr.w, pr.h, 0, 0, 1, 1, 1.0f, s->tex.opaque || s->tex.solid);
    }
    return true;
}

static bool gl_scene_cb(const struct lm_scene_item *item, void *data)
{
    struct gl_pass *p = data;
    switch (item->kind) {
    case LM_SCENE_WALLPAPER:
        break;                                          /* in the background texture */
    case LM_SCENE_FRAME:
        gl_frame(p, &item->u.frame);
        break;
    case LM_SCENE_SURFACE:
        lm_surface_tree_visit(item->u.surface.root, item->u.surface.x, item->u.surface.y, gl_visit, p);
        break;
    case LM_SCENE_OVERVIEW:
        if (item->u.overview.rect.x == p->st->out->x && item->u.overview.rect.y == p->st->out->y) {
            const struct lm_overview_info *ov = &item->u.overview;
            if (ov->dim > 0) {                                  /* the dimming changes every frame while it opens: a plain translucent quad, not a texture */
                struct lm_rect pr = gl_pr(p, &ov->rect);
                lm_gl_draw_rect(p->r->gl, pr.x, pr.y, pr.w, pr.h,
                                (float)((lm_theme.ink >> 16) & 255) / 255.0f * (float)ov->dim / 255.0f,
                                (float)((lm_theme.ink >> 8) & 255) / 255.0f * (float)ov->dim / 255.0f,
                                (float)(lm_theme.ink & 255) / 255.0f * (float)ov->dim / 255.0f, (float)ov->dim / 255.0f);
            }
            if (ov->ui) {                                       /* strip, frames and labels: redrawn only when something they show changed */
                struct gl_chrome *ch = &p->go->overview;
                uint64_t h = fnv(1469598103934665603ull, &ov->rect, sizeof(ov->rect));
                h = fnv(h, ov->ws, sizeof(ov->ws));
                h = fnv(h, &ov->n, sizeof(ov->n));
                for (int i = 0; i < ov->n; i++) {
                    h = fnv(h, &ov->item[i].frame, sizeof(ov->item[i].frame));
                    h = fnv(h, &ov->item[i].sel, 3 * sizeof(bool));
                    h = fnv(h, ov->item[i].title, strlen(ov->item[i].title) + 1);
                }
                if (!ch->tex || ch->hash != h || ch->w != ov->rect.w || ch->h != ov->rect.h) {
                    gl_scratch_alpha(p->r, p->st, ch, &ov->rect, gl_do_overview, ov);
                    ch->hash = h;
                }
                if (ch->tex)
                    gl_tex_at(p, ch->tex, &ov->rect, false);
            }
        }
        break;
    case LM_SCENE_GHOST: {                              /* a window that opens / closes / is minimized: its picture, scaled by the GPU */
        struct lm_ghost *g = item->u.ghost.ghost;
        const struct lm_rect *d = &item->u.ghost.dst;
        if (!g || !g->px || d->w <= 0 || d->h <= 0 || item->u.ghost.alpha <= 0)
            break;
        if (!g->gl_tex)
            g->gl_tex = lm_gl_tex_create(p->r->gl, g->w, g->h, g->px);
        const struct lm_rect *sr = &item->u.ghost.src;
        bool part = sr->w > 0 && sr->h > 0;
        struct lm_rect pd = gl_pr(p, d);
        if (g->gl_tex)
            lm_gl_draw_tex(p->r->gl, g->gl_tex, pd.x, pd.y, pd.w, pd.h,
                           part ? (float)sr->x / (float)g->w : 0, part ? (float)sr->y / (float)g->h : 0,
                           part ? (float)(sr->x + sr->w) / (float)g->w : 1, part ? (float)(sr->y + sr->h) / (float)g->h : 1,
                           (float)item->u.ghost.alpha / 255.0f, false);
        break;
    }
    case LM_SCENE_PANEL_SHADOW:
        if (item->output == p->st->out) {               /* translucent and always the same for a size: made once */
            struct gl_chrome *ch = &p->go->shadow;
            if (!ch->tex || ch->w != item->u.shadow.w || ch->h != item->u.shadow.h)
                gl_scratch_alpha(p->r, p->st, ch, &item->u.shadow, gl_do_shadow, &item->u.shadow);
            if (ch->tex)
                gl_tex_at(p, ch->tex, &item->u.shadow, false);
        }
        break;
    case LM_SCENE_PANEL:
        if (item->output == p->st->out)
            gl_chrome_item(p, &p->go->panel, &item->u.panel.bar, gl_do_panel, &item->u.panel);
        break;
    case LM_SCENE_DOCK:
        if (item->output == p->st->out) {               /* translucent: redrawn only when something it shows changed */
            const struct lm_dock_info *d = &item->u.dock;
            struct gl_chrome *ch = &p->go->dock;
            uint64_t h = fnv(1469598103934665603ull, &d->rect, sizeof(d->rect));
            h = fnv(h, &d->hover, sizeof(d->hover));
            h = fnv(h, &d->label, sizeof(d->label));
            h = fnv(h, &d->drag_item, sizeof(d->drag_item));
            h = fnv(h, &d->drag_box, sizeof(d->drag_box));
            h = fnv(h, &d->drag_remove, sizeof(d->drag_remove));
            h = fnv(h, &lm_dock.icon_bg, sizeof(lm_dock.icon_bg));
            h = fnv(h, &lm_dock.icon_bg_rgb, sizeof(lm_dock.icon_bg_rgb));
            h = fnv(h, &lm_metrics.button_roundness, sizeof(lm_metrics.button_roundness));
            for (int i = 0; i < d->n; i++) {
                h = fnv(h, &d->icon[i].box, sizeof(d->icon[i].box));
                h = fnv(h, &d->icon[i].running, sizeof(d->icon[i].running));
                h = fnv(h, &d->icon[i].alpha, sizeof(d->icon[i].alpha));
                h = fnv(h, &d->icon[i].blur, sizeof(d->icon[i].blur));
                h = fnv(h, &d->icon[i].item, sizeof(d->icon[i].item));
                h = fnv(h, &d->icon[i].item->kind, sizeof(d->icon[i].item->kind));
            }
            if (!ch->tex || ch->hash != h || ch->w != d->rect.w || ch->h != d->rect.h) {
                gl_scratch_alpha(p->r, p->st, ch, &d->rect, gl_do_dock, d);
                ch->hash = h;
            }
            if (ch->tex)
                gl_tex_at(p, ch->tex, &d->rect, false);
        }
        break;
    case LM_SCENE_ABOUT:
        if (item->output == p->st->out) {               /* translucent, like the session dialog: redrawn only when it changes */
            const struct lm_about_info *ai = &item->u.about;
            struct gl_chrome *ch = &p->go->about;
            uint64_t h = fnv(1469598103934665603ull, &ai->card, sizeof(ai->card));
            h = fnv(h, &ai->hover_close, sizeof(ai->hover_close));
            for (int i = 0; i < ai->n; i++) {
                h = fnv(h, ai->row[i].label, strlen(ai->row[i].label) + 1);
                h = fnv(h, ai->row[i].value, strlen(ai->row[i].value) + 1);
            }
            if (!ch->tex || ch->hash != h || ch->w != ai->rect.w || ch->h != ai->rect.h) {
                gl_scratch_alpha(p->r, p->st, ch, &ai->rect, gl_do_about, ai);
                ch->hash = h;
            }
            if (ch->tex)
                gl_tex_at(p, ch->tex, &ai->rect, false);
        }
        break;
    case LM_SCENE_LAUNCHER:
        if (item->output == p->st->out) {               /* translucent like the About dialog: redrawn only when it changes */
            const struct lm_launcher_info *li = &item->u.launcher;
            struct gl_chrome *ch = &p->go->launcher;
            uint64_t h = fnv(1469598103934665603ull, &li->card, sizeof(li->card));
            h = fnv(h, &li->veil, sizeof(li->veil));
            h = fnv(h, &li->veil_only, sizeof(li->veil_only));
            h = fnv(h, &li->form, sizeof(li->form));
            h = fnv(h, &li->edit, sizeof(li->edit));
            h = fnv(h, &li->n, sizeof(li->n));
            h = fnv(h, &li->sel, sizeof(li->sel));
            h = fnv(h, &li->hover_button, sizeof(li->hover_button));
            h = fnv(h, &li->focus, sizeof(li->focus));
            h = fnv(h, &li->drag, sizeof(li->drag));
            h = fnv(h, &li->drag_box, sizeof(li->drag_box));
            h = fnv(h, &li->drop_pos, sizeof(li->drop_pos));
            h = fnv(h, &li->kind, sizeof(li->kind));
            h = fnv(h, li->caret, sizeof(li->caret));
            h = fnv(h, &li->help, sizeof(li->help));
            h = fnv(h, &lm_dock.icon_bg, sizeof(lm_dock.icon_bg));
            h = fnv(h, &lm_dock.icon_bg_rgb, sizeof(lm_dock.icon_bg_rgb));
            h = fnv(h, &lm_metrics.button_roundness, sizeof(lm_metrics.button_roundness));
            h = fnv(h, li->search, strlen(li->search) + 1);
            h = fnv(h, &li->searching, sizeof(li->searching));
            h = fnv(h, li->button_text[0], strlen(li->button_text[0]) + 1);
            h = fnv(h, li->message, strlen(li->message) + 1);
            if (li->form)
                for (int i = 0; i < 3; i++)                      /* name, command and icon text: a changed one is redrawn */
                    h = fnv(h, li->field_text[i], strlen(li->field_text[i]) + 1);
            else
                for (int i = 0; i < li->n; i++) {
                    h = fnv(h, li->icon[i].item->name, strlen(li->icon[i].item->name) + 1);
                    h = fnv(h, &li->icon[i].item->kind, sizeof(int));
                    h = fnv(h, &li->icon[i].item->img, sizeof(void *));
                    h = fnv(h, &li->icon[i].running, sizeof(bool));
                }
            if (!ch->tex || ch->hash != h || ch->w != li->rect.w || ch->h != li->rect.h) {
                gl_scratch_alpha(p->r, p->st, ch, &li->rect, gl_do_launcher, li);
                ch->hash = h;
            }
            if (ch->tex)
                gl_tex_at(p, ch->tex, &li->rect, false);
        }
        break;
    case LM_SCENE_NOTIFY: {                             /* a texture per card, kept while its content (hashed) stays the same */
        const struct lm_notify_info *n = &item->u.notify;
        uint64_t h = fnv(1469598103934665603ull, &n->rect, sizeof(n->rect));
        h = fnv(h, n->title, strlen(n->title) + 1);
        for (int i = 0; i < n->nlines; i++)
            h = fnv(h, n->line[i], strlen(n->line[i]) + 1);
        struct gl_chrome own = { 0 };
        struct gl_chrome *ch = p->nidx < LM_NOTIFY_MAX ? &p->go->notify[p->nidx] : &own;
        p->nidx++;
        if (!ch->tex || ch->hash != h || ch->w != n->rect.w || ch->h != n->rect.h) {
            gl_scratch(p->r, p->st, ch, &n->rect, gl_do_notify, n);
            ch->hash = h;
        }
        if (ch->tex)
            gl_tex_at(p, ch->tex, &n->rect, true);
        if (ch == &own)
            gl_chrome_free(p->r, &own);
        break;
    }
    case LM_SCENE_MENU:
        if (item->output == p->st->out) {               /* rounded and a little translucent: redrawn only when it changes */
            const struct lm_menu_info *mi = &item->u.menu;
            struct gl_chrome *ch = &p->go->menu;
            uint64_t h = fnv(1469598103934665603ull, &mi->rect, sizeof(mi->rect));
            h = fnv(h, &mi->sel, sizeof(mi->sel));
            h = fnv(h, mi->enabled, sizeof(mi->enabled));
            h = fnv(h, mi->title, strlen(mi->title) + 1);
            h = fnv(h, &mi->n, sizeof(mi->n));
            for (int i = 0; i < mi->n; i++)
                h = fnv(h, mi->label[i], strlen(mi->label[i]) + 1);
            if (!ch->tex || ch->hash != h || ch->w != mi->rect.w || ch->h != mi->rect.h) {
                gl_scratch_alpha(p->r, p->st, ch, &mi->rect, gl_do_menu, mi);
                ch->hash = h;
            }
            if (ch->tex)
                gl_tex_at(p, ch->tex, &mi->rect, false);
        }
        break;
    case LM_SCENE_POWER:
        if (item->output == p->st->out) {               /* translucent and independent of what is below: redrawn only when it changes */
            const struct lm_power_info *pw = &item->u.power;
            struct gl_chrome *ch = &p->go->power;
            uint64_t h = fnv(1469598103934665603ull, &pw->card, sizeof(pw->card));
            h = fnv(h, &pw->sel, sizeof(pw->sel));
            h = fnv(h, &pw->confirm, sizeof(pw->confirm));
            h = fnv(h, &pw->others, sizeof(pw->others));
            h = fnv(h, pw->off, sizeof(pw->off));
            if (!ch->tex || ch->hash != h || ch->w != pw->rect.w || ch->h != pw->rect.h) {
                gl_scratch_alpha(p->r, p->st, ch, &pw->rect, gl_do_power, pw);
                ch->hash = h;
            }
            if (ch->tex)
                gl_tex_at(p, ch->tex, &pw->rect, false);
        }
        break;
    case LM_SCENE_OSK:
        if (item->output == p->st->out) {               /* translucent and independent of what is below: redrawn only when it changes */
            const struct lm_osk_info *oi = &item->u.osk;
            struct gl_chrome *ch = &p->go->osk;
            uint64_t h = fnv(1469598103934665603ull, oi, sizeof(*oi));
            if (!ch->tex || ch->hash != h || ch->w != oi->rect.w || ch->h != oi->rect.h) {
                gl_scratch_alpha(p->r, p->st, ch, &oi->rect, gl_do_osk, oi);
                ch->hash = h;
            }
            if (ch->tex)
                gl_tex_at(p, ch->tex, &oi->rect, false);
        }
        break;
    case LM_SCENE_SWITCHER:
        gl_chrome_item(p, &p->go->switcher, &item->u.switcher.rect, gl_do_switcher, &item->u.switcher);
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

static void lm_render_ghost_gl_free(struct lm_renderer *r, struct lm_ghost *g)
{
    if (g->gl_tex && r->gl)
        lm_gl_tex_delete(r->gl, g->gl_tex);
    g->gl_tex = 0;
}

/* ---- overlays: screenshot flash and card, the cursor ------------------------------------------------------------------ */

static void gl_do_card(const struct paint_ctx *c, const void *it) { paint_shot_card(c, it); }

static void gl_card_paint(struct lm_renderer *r, struct out_state *st, struct gl_chrome *ch)
{
    const struct shot_fx *fx = &r->shot;
    struct lm_rect cr = shot_card_rect(fx);
    gl_scratch_alpha(r, st, ch, &cr, gl_do_card, fx);
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
        if (lm_rect_intersect(&fx->area, &orect, &area)) {
            struct lm_rect pa = gl_pr(p, &area);
            lm_gl_draw_rect(r->gl, pa.x, pa.y, pa.w, pa.h, f, f, f, f);
        }
    }
    struct gl_out *go = p->go;
    if (!go->card.tex || go->card_gen != fx->gen) {
        gl_card_paint(r, p->st, &go->card);
        go->card_gen = fx->gen;
    }
    if (go->card.tex) {
        struct lm_rect cr = shot_card_rect(fx), pc = gl_pr(p, &cr);
        lm_gl_draw_tex(r->gl, go->card.tex, pc.x, pc.y, pc.w, pc.h, 0, 0, 1, 1, 1.0f, false);
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
    double sc = lm_output_scalef(p->st->out);
    int k = sc == 1.0 ? 1 : v.k;                                        /* the arrow is made k times as big with whole pixels: it stays sharp */
    if (r->sprite_tex[shape] && r->sprite_k[shape] != k) {
        lm_gl_tex_delete(r->gl, r->sprite_tex[shape]);
        r->sprite_tex[shape] = 0;
    }
    if (!r->sprite_tex[shape]) {
        uint32_t *big = k == 1 ? NULL : malloc((size_t)SPR_W * (size_t)SPR_H * (size_t)k * (size_t)k * 4);
        if (k != 1 && !big)
            return;
        if (big)
            for (int y = 0; y < SPR_H * k; y++)
                for (int x = 0; x < SPR_W * k; x++)
                    big[y * SPR_W * k + x] = v.sprite->px[(y / k) * SPR_W + x / k];
        r->sprite_tex[shape] = lm_gl_tex_create(r->gl, SPR_W * k, SPR_H * k, big ? big : v.sprite->px);
        r->sprite_k[shape] = k;
        free(big);
    }
    int px = sround((double)(v.ix - p->ox) * sc) - v.sprite->hot_x * k, py = sround((double)(v.iy - p->oy) * sc) - v.sprite->hot_y * k;
    lm_gl_draw_tex(r->gl, r->sprite_tex[shape], px, py, SPR_W * k, SPR_H * k, 0, 0, 1, 1, 1.0f, false);
}

/* ---- one frame ----------------------------------------------------------------------------------------------------- */

/* Draws the whole output into the bound target: background, scene, and (not for screenshots) flash, card and cursor. */
static void gl_draw_scene(struct lm_renderer *r, struct out_state *st, struct gl_out *go, const struct lm_region *redraw,
                          bool full, bool overlays)
{
    struct lm_output *out = st->out;
    struct lm_rect orect = lm_output_rect(out);
    lm_gl_scissor(r->gl, false, 0, 0, 0, 0);
    lm_gl_draw_tex(r->gl, go->bg_tex, 0, 0, go->w, go->h, 0, 0, 1, 1, 1.0f, true);
    struct gl_pass p = { .r = r, .st = st, .go = go, .dirty = redraw, .full = full, .ox = out->x, .oy = out->y };
    if (r->srv->wm)
        lm_wm_scene_for_each(r->srv->wm, &orect, gl_scene_cb, &p);
    if (overlays) {
        gl_overlays(&p);
        gl_cursor(&p);
    }
}

/* A screenshot: the scene (no cursor, flash or card) rendered offscreen and read back; the part inside `clip` (global). */
static uint32_t *gl_capture(struct lm_renderer *r, struct out_state *st, const struct lm_rect *clip, int *w, int *h)
{
    struct lm_output *out = st->out;
    struct gl_out *go = gl_out_get(r, st);
    if (!go)
        return NULL;
    lm_gl_make_current(r->gl);
    struct lm_gl_target tgt;
    if (!lm_gl_target_create(r->gl, go->w, go->h, &tgt))
        return NULL;
    struct lm_rect orect = lm_output_rect(out);
    struct lm_region all;
    lm_region_init(&all);
    lm_region_add_rect(&all, &orect);
    gl_bg_update(r, st, go, &all);
    go->bg_ok = true;
    lm_gl_bind_target(r->gl, &tgt, go->w, go->h);
    gl_draw_scene(r, st, go, &all, true, false);
    lm_region_fini(&all);
    size_t stride = (size_t)go->w * 4;
    struct lm_rect pclip = out_prect(out, clip);                        /* the picture is made of the screen's own pixels */
    uint32_t *whole = malloc((size_t)go->h * stride), *res = malloc((size_t)pclip.w * (size_t)pclip.h * 4);
    bool ok = whole && res && lm_gl_readback(r->gl, &tgt, whole, stride);
    lm_gl_target_destroy(r->gl, &tgt);
    if (ok) {
        for (int y = 0; y < pclip.h; y++)
            memcpy(res + (size_t)y * (size_t)pclip.w,
                   (const uint8_t *)whole + (size_t)(pclip.y + y) * stride + (size_t)pclip.x * 4,
                   (size_t)pclip.w * 4);
        *w = pclip.w;
        *h = pclip.h;
    }
    free(whole);
    if (!ok) {
        free(res);
        return NULL;
    }
    return res;
}

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
    uint32_t look_gen = lm_theme_generation() + lm_font_generation() + (uint32_t)out->scale_milli * 7u;   /* the palette and the font only grow: the sum moves when either does; the scale is mixed in */
    if (go->theme_gen != look_gen) {
        if (go->theme_gen)
            gl_chromes_free(r, go);
        go->theme_gen = look_gen;
        st->full = true;
        go->bg_ok = false;
    }

    struct lm_rect orect = lm_output_rect(out);
    struct lm_region redraw;
    lm_region_init(&redraw);
    float mz = 1.0f, mx = 0.0f, my = 0.0f;
    bool mag = mag_view(r, out, &mz, &mx, &my);                     /* the magnifier: every frame is whole and goes through an offscreen picture */
    bool full = st->full || !go->bg_ok || mag;
    if (full)
        lm_region_add_rect(&redraw, &orect);
    else
        lm_region_add_region(&redraw, &st->pending);
    lm_region_intersect_rect(&redraw, &orect);

    gl_bg_update(r, st, go, &redraw);
    go->bg_ok = true;
    if (mag && !go->mag.fbo && !lm_gl_target_create(r->gl, go->w, go->h, &go->mag))
        mag = false;

    struct lm_gl_window *win = lm_output_gl_window(out);
    if (mag) {
        lm_gl_bind_target(r->gl, &go->mag, go->w, go->h);
        gl_draw_scene(r, st, go, &redraw, true, true);
    }
    if (win) {
        if (!lm_gl_window_bind(r->gl, win)) {
            lm_region_fini(&redraw);
            return false;
        }
    } else {
        lm_gl_bind_target(r->gl, &go->tgt, go->w, go->h);
    }
    if (mag) {
        lm_gl_scissor(r->gl, false, 0, 0, 0, 0);
        lm_gl_draw_tex(r->gl, go->mag.tex, 0, 0, go->w, go->h, mx / (float)go->w, my / (float)go->h, (mx + (float)go->w / mz) / (float)go->w,
                       (my + (float)go->h / mz) / (float)go->h, 1.0f, true);
    } else {
        gl_draw_scene(r, st, go, &redraw, full, true);
    }

    if (win) {
        lm_output_gl_present(out);                      /* swap the window's buffers, queue the page flip */
    } else {
        go->unread = true;                              /* headless: copied to the memory framebuffer when something reads it */
        lm_output_present(out, NULL, 0);
    }

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
