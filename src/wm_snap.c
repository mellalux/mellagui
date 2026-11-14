/* Part of the window manager (see wm_priv.h): tiling zones and the preview shown while a window is dragged to an edge or a corner */
#include "wm_priv.h"

#include <math.h>

#include "theme.h"

/* A zone is a bit set of LM_EDGE_*: LEFT or RIGHT = that half of the usable area, with TOP or BOTTOM added = the quarter in that corner. TOP alone is
 * not a place to tile to: dragging to the top edge maximizes. */
bool snap_zone_valid(uint32_t zone)
{
    if (zone == LM_EDGE_NONE || zone == LM_EDGE_TOP)
        return true;
    uint32_t side = zone & (LM_EDGE_LEFT | LM_EDGE_RIGHT), vert = zone & (LM_EDGE_TOP | LM_EDGE_BOTTOM);
    if (zone & ~(uint32_t)(LM_EDGE_LEFT | LM_EDGE_RIGHT | LM_EDGE_TOP | LM_EDGE_BOTTOM))
        return false;
    return (side == LM_EDGE_LEFT || side == LM_EDGE_RIGHT) && vert != (LM_EDGE_TOP | LM_EDGE_BOTTOM);
}

void snap_zone_rect(const struct lm_rect *ua, uint32_t zone, struct lm_rect *out)
{
    int32_t x = ua->x, y = ua->y, w = ua->w, h = ua->h, hw = ua->w / 2, hh = ua->h / 2;
    if (zone & LM_EDGE_LEFT) {
        w = hw;
    } else if (zone & LM_EDGE_RIGHT) {
        x += hw;
        w -= hw;
    }
    if (zone != LM_EDGE_TOP) {
        if (zone & LM_EDGE_TOP) {
            h = hh;
        } else if (zone & LM_EDGE_BOTTOM) {
            y += hh;
            h -= hh;
        }
    }
    *out = lm_rect_make(x, y, w, h);
}

const char *snap_zone_name(uint32_t zone)
{
    switch (zone) {
    case LM_EDGE_LEFT: return "left";
    case LM_EDGE_RIGHT: return "right";
    case LM_EDGE_TOP | LM_EDGE_LEFT: return "topleft";
    case LM_EDGE_TOP | LM_EDGE_RIGHT: return "topright";
    case LM_EDGE_BOTTOM | LM_EDGE_LEFT: return "bottomleft";
    case LM_EDGE_BOTTOM | LM_EDGE_RIGHT: return "bottomright";
    case LM_EDGE_TOP: return "maximize";
    default: return "none";
    }
}

/* Is there another output beyond this edge of `ua` at the pointer's height or width? Then the pointer is only crossing to it, not pushing against the
 * edge of the screen. */
static bool output_beyond(struct lm_wm *wm, const struct lm_rect *ua, int edge, double x, double y)
{
    double px = x, py = y;
    if (edge == LM_EDGE_LEFT)
        px = ua->x - 1.0;
    else if (edge == LM_EDGE_RIGHT)
        px = ua->x + ua->w + 0.0;
    else if (edge == LM_EDGE_TOP)
        py = ua->y - LM_PANEL_HEIGHT - 1.0;
    struct lm_output *o = lm_output_nearest(wm->srv, px, py);
    if (!o)
        return false;
    struct lm_rect r = lm_output_rect(o);
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

/* The zone the pointer is pushing against: the left or right edge (a half), a corner (a quarter), the top edge (maximize). */
static uint32_t zone_at(struct lm_wm *wm, double x, double y, struct lm_rect *ua)
{
    enum { MARGIN = 3, CORNER = 64 };
    lm_wm_usable_area(wm, (int32_t)x, (int32_t)y, ua);
    if (ua->w < 4 * CORNER || ua->h < 4 * CORNER)
        return 0;
    bool l = x < ua->x + MARGIN && !output_beyond(wm, ua, LM_EDGE_LEFT, x, y);
    bool r = x >= ua->x + ua->w - MARGIN && !output_beyond(wm, ua, LM_EDGE_RIGHT, x, y);
    bool t = y < ua->y + MARGIN && !output_beyond(wm, ua, LM_EDGE_TOP, x, y);
    if (l || r) {
        uint32_t zone = l ? LM_EDGE_LEFT : LM_EDGE_RIGHT;
        if (y < ua->y + CORNER)
            zone |= LM_EDGE_TOP;
        else if (y >= ua->y + ua->h - CORNER)
            zone |= LM_EDGE_BOTTOM;
        return zone;
    }
    return t ? LM_EDGE_TOP : 0;
}

/* The picture of the preview: the accent colour, see-through, with a stronger rim and round corners. Premultiplied ARGB. */
static bool make_ghost(struct lm_wm *wm, int w, int h)
{
    if (w < 8 || h < 8 || (int64_t)w * h > 16 * 1024 * 1024)
        return false;
    uint32_t *px = malloc((size_t)w * (size_t)h * 4);
    if (!px)
        return false;
    uint32_t rgb = lm_theme.accent;
    int cr = (int)((rgb >> 16) & 255), cg = (int)((rgb >> 8) & 255), cb = (int)(rgb & 255);
    const float R = 12.0f, RIM = 3.0f, FILL = 64.0f, EDGE = 200.0f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float qx = fabsf((float)x + 0.5f - (float)w / 2.0f) - ((float)w / 2.0f - R), qy = fabsf((float)y + 0.5f - (float)h / 2.0f) - ((float)h / 2.0f - R);
            float ox = fmaxf(qx, 0.0f), oy = fmaxf(qy, 0.0f);
            float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - R;           /* < 0 inside */
            float cov = fminf(fmaxf(0.5f - d, 0.0f), 1.0f);
            float rim = fminf(fmaxf(d + RIM + 0.5f, 0.0f), 1.0f);                           /* 1 on the rim, 0 inside it */
            float a = cov * (FILL + (EDGE - FILL) * rim);
            uint32_t ai = (uint32_t)(a + 0.5f);
            px[(size_t)y * (size_t)w + (size_t)x] = (ai << 24) | ((uint32_t)(cr * (int)ai / 255) << 16) | ((uint32_t)(cg * (int)ai / 255) << 8) |
                                                   (uint32_t)(cb * (int)ai / 255);
        }
    wm->snap_ghost.px = px;
    wm->snap_ghost.w = w;
    wm->snap_ghost.h = h;
    return true;
}

void snap_free(struct lm_wm *wm)
{
    if (wm->snap_ghost.px)
        lm_render_ghost_free(wm->srv->render, &wm->snap_ghost);
}

void snap_clear(struct lm_wm *wm)
{
    if (!wm->snap_zone && !wm->snap_ghost.px)
        return;
    struct lm_rect old = wm->snap_rect;
    snap_free(wm);
    wm->snap_zone = 0;
    damage_rect(wm, &old);
}

void snap_update(struct lm_wm *wm, double x, double y)
{
    struct lm_rect ua, r;
    uint32_t zone = zone_at(wm, x, y, &ua);
    if (!zone) {
        snap_clear(wm);
        return;
    }
    snap_zone_rect(&ua, zone, &r);
    if (zone == wm->snap_zone && lm_rect_equal(&r, &wm->snap_rect) && wm->snap_ghost.px)
        return;
    struct lm_rect old = wm->snap_rect;
    bool had = wm->snap_ghost.px != NULL;
    snap_free(wm);
    if (!make_ghost(wm, r.w, r.h)) {
        wm->snap_zone = 0;
        if (had)
            damage_rect(wm, &old);
        return;
    }
    wm->snap_zone = zone;
    wm->snap_rect = r;
    if (had)
        damage_rect(wm, &old);
    damage_rect(wm, &r);
}

size_t lm_wm_snap_describe(struct lm_wm *wm, char *buf, size_t size)
{
    if (!wm->snap_zone || !wm->snap_ghost.px)
        return (size_t)snprintf(buf, size, "snap zone=none\n");
    return (size_t)snprintf(buf, size, "snap zone=%s x=%d y=%d w=%d h=%d\n", snap_zone_name(wm->snap_zone), wm->snap_rect.x, wm->snap_rect.y,
                            wm->snap_rect.w, wm->snap_rect.h);
}
