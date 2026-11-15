/* Part of the window manager (see wm_priv.h): tiling zones and the preview shown while a window is dragged to an edge or a corner */
#include "wm_priv.h"

#include <linux/input-event-codes.h>
#include <math.h>

#include "theme.h"

/* A zone is a bit set of LM_EDGE_*: LEFT or RIGHT = that half of the usable area, with TOP or BOTTOM added = the quarter in that corner. TOP alone is
 * not a place to tile to: dragging to the top edge maximizes. */
static bool grid_decode(uint32_t zone, int *cols, int *rows, int *c0, int *c1, int *r0, int *r1)
{
    if (!(zone & LM_ZONE_GRID))
        return false;
    *cols = (int)(zone & 15u), *rows = (int)(zone >> 4 & 15u), *c0 = (int)(zone >> 8 & 15u), *c1 = (int)(zone >> 12 & 15u);
    *r0 = (int)(zone >> 16 & 15u), *r1 = (int)(zone >> 20 & 15u);
    return true;
}

bool snap_zone_valid(uint32_t zone)
{
    int cols, rows, c0, c1, r0, r1;
    if (grid_decode(zone, &cols, &rows, &c0, &c1, &r0, &r1))
        return !(zone & 0x7f000000u) && cols >= 1 && cols <= LM_ZONE_MAXGRID && rows >= 1 && rows <= LM_ZONE_MAXGRID && c0 >= 0 && c0 < c1 && c1 <= cols &&
               r0 >= 0 && r0 < r1 && r1 <= rows && (c1 - c0 < cols || r1 - r0 < rows);                /* (not the whole screen: that is maximize) */
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
    int cols, rows, c0, c1, r0, r1;
    if (grid_decode(zone, &cols, &rows, &c0, &c1, &r0, &r1)) {                  /* both edges of a block are cut the same way, so neighbouring blocks touch */
        int32_t x0 = ua->x + (int32_t)((int64_t)ua->w * c0 / cols), x1 = ua->x + (int32_t)((int64_t)ua->w * c1 / cols);
        int32_t y0 = ua->y + (int32_t)((int64_t)ua->h * r0 / rows), y1 = ua->y + (int32_t)((int64_t)ua->h * r1 / rows);
        *out = lm_rect_make(x0, y0, x1 - x0, y1 - y0);
        return;
    }
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

/* "third-left" / "third-middle" / "third-right" / "twothirds-left" / "twothirds-right" for the cuts in three, else "grid-CxR-c.r" (a cell, 1-based) or
 * "grid-CxR-c.r-c.r" (a block). The text of a grid zone is in a static buffer: use it at once. */
const char *snap_zone_name(uint32_t zone)
{
    int cols, rows, c0, c1, r0, r1;
    if (grid_decode(zone, &cols, &rows, &c0, &c1, &r0, &r1)) {
        static char name[40];
        if (cols == 3 && rows == 1) {
            static const char *const third[3] = { "third-left", "third-middle", "third-right" };
            if (c1 - c0 == 1)
                return third[c0];
            if (c1 - c0 == 2)
                return c0 == 0 ? "twothirds-left" : "twothirds-right";
        }
        if (c1 - c0 == 1 && r1 - r0 == 1)
            snprintf(name, sizeof(name), "grid-%dx%d-%d.%d", cols, rows, c0 + 1, r0 + 1);
        else
            snprintf(name, sizeof(name), "grid-%dx%d-%d.%d-%d.%d", cols, rows, c0 + 1, r0 + 1, c1, r1);
        return name;
    }
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

/* With Shift held the whole screen is the grid: the cell under the pointer is the zone. */
static uint32_t grid_cell_at(struct lm_wm *wm, double x, double y, struct lm_rect *ua)
{
    lm_wm_usable_area(wm, (int32_t)x, (int32_t)y, ua);
    int cols = lm_metrics.grid_columns, rows = lm_metrics.grid_rows;
    if (ua->w < 4 * 64 || ua->h < 4 * 64 || cols * rows < 2 || x < ua->x || y < ua->y || x >= ua->x + ua->w || y >= ua->y + ua->h)
        return 0;
    int c = (int)((int64_t)(x - ua->x) * cols / ua->w), r = (int)((int64_t)(y - ua->y) * rows / ua->h);
    return snap_grid_zone(cols, rows, c, c + 1, r, r + 1);
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
    bool shift = osk_phys_down(wm, KEY_LEFTSHIFT) || osk_phys_down(wm, KEY_RIGHTSHIFT);
    uint32_t zone = shift ? grid_cell_at(wm, x, y, &ua) : zone_at(wm, x, y, &ua);
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

/* ---- shortcuts ------------------------------------------------------------------------------------------------------ */

/* tile-third N, tile-twothirds N (full height, 3 columns) and tile-grid-move DIR: the window goes into the cell of the grid its centre is in (no move
 * yet when it was not in a cell), and then one cell on, stopping at the edge. */
void snap_key(struct lm_wm *wm, struct lm_window *w, enum lm_action action, int num)
{
    uint32_t zone = 0;
    if (action == LM_ACT_TILE_THIRD)
        zone = snap_grid_zone(3, 1, num, num + 1, 0, 1);
    else if (action == LM_ACT_TILE_TWOTHIRDS)
        zone = snap_grid_zone(3, 1, num, num + 2, 0, 1);
    else if (action == LM_ACT_TILE_GRID_MOVE) {
        int cols = lm_metrics.grid_columns, rows = lm_metrics.grid_rows, c, r, c0, c1, r0, r1, oc, orow;
        struct lm_rect ua;
        if (cols * rows < 2)
            return;
        int32_t cx = w->x + w->width / 2, cy = w->y + w->height / 2;
        lm_wm_usable_area(wm, cx, cy, &ua);
        uint32_t cur = WIN(w)->tiled;
        if (grid_decode(cur, &oc, &orow, &c0, &c1, &r0, &r1) && oc == cols && orow == rows && c1 - c0 == 1 && r1 - r0 == 1) {
            c = c0;                                                         /* in a cell of this grid: move on from there */
            r = r0;
            if (num == 0) c--;
            else if (num == 1) c++;
            else if (num == 2) r--;
            else r++;
        } else {                                                            /* anywhere else: into the cell the window is in */
            c = ua.w > 0 ? (int)((int64_t)(cx - ua.x) * cols / ua.w) : 0;
            r = ua.h > 0 ? (int)((int64_t)(cy - ua.y) * rows / ua.h) : 0;
        }
        c = c < 0 ? 0 : c >= cols ? cols - 1 : c;
        r = r < 0 ? 0 : r >= rows ? rows - 1 : r;
        zone = snap_grid_zone(cols, rows, c, c + 1, r, r + 1);
    }
    if (zone)
        lm_wm_window_request_tile(w, zone);
}
