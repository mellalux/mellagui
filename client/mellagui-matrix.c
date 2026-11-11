/*
 * mellagui-matrix: a screensaver, falling green code in the manner of the film.
 *
 *   mellagui-matrix                 full screen, any key, button or a pointer movement ends it (the compositor also ends it by itself
 *                                   when it started the program as the screensaver: settings, Screensaver page)
 *   mellagui-matrix --flat          the classic flat rain instead of the room
 *   mellagui-matrix --density PCT   how crowded the rain is, 50..300 (default 260)
 *   mellagui-matrix --bench WxH     draws the room a few seconds without a display and prints the time of a frame
 *   mellagui-matrix --windowed      an ordinary window (to look at it, or to leave running beside other windows)
 *   mellagui-matrix --speed PERCENT how fast the rain falls, 25..400 (default 100)
 *   mellagui-matrix --seed N        fixed random numbers (tests)
 *   mellagui-matrix --selftest      checks the rain's arithmetic without a display
 *
 * By default the rain is on the walls, the floor and the ceiling of a room (software ray casting, the camera drifts slowly, there is a dark window in
 * the far wall). Every column has a bright head that falls at its own speed and leaves a trail that fades out. The letters are the digits, the Latin letters
 * and some signs of the built-in font, mirrored like the ones in the film, and they change here and there while they fade.
 * Drawn in software into one shm buffer like the other small programs (wlwin.c); about 25 frames a second.
 */
#pragma GCC optimize("O3", "fast-math")
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "font.h"
#include "keymap.h"
#include "theme.h"
#include "util.h"
#include "wlwin.h"

#define TICK_MS 40                      /* one step of the rain (25 a second) */
#define MOVE_END_PX 14                  /* the pointer has to travel this far to end the show (a bump of the desk should not) */
#define GLYPHS_MAX 128

struct column {
    float pos;                          /* row of the head (fractional; negative = still waiting above the screen) */
    float speed;                        /* rows per step */
    int row;                            /* last row the head has written */
    int fade;                           /* 0..255: how much of a cell's brightness is left after a step (256 = nothing fades) */
};

struct rain {
    int cols, rows;
    uint16_t *glyph;                    /* cols * rows code points */
    uint8_t *bright;                    /* cols * rows: 255 = the head, then fading */
    struct column *col;                 /* cols * heads: several heads may fall down one column when it is crowded */
    uint64_t rng;
    int speed_pct;
    int density;                        /* percent: 100 = the old rain; more = shorter gaps between heads, longer trails */
    uint32_t set[GLYPHS_MAX];
    int nset;
};

/* ---- random numbers and the character set ---------------------------------------------------------------------------- */

static uint32_t rnd(struct rain *r)
{
    r->rng ^= r->rng << 13;
    r->rng ^= r->rng >> 7;
    r->rng ^= r->rng << 17;
    return (uint32_t)(r->rng >> 11);
}

static int rnd_range(struct rain *r, int n)
{
    return n > 0 ? (int)(rnd(r) % (uint32_t)n) : 0;
}

static void make_set(struct rain *r)
{
    r->nset = 0;
    for (uint32_t c = '0'; c <= '9'; c++)
        r->set[r->nset++] = c;
    for (uint32_t c = 'A'; c <= 'Z'; c++)
        r->set[r->nset++] = c;
    for (const char *s = ":.=*+-<>|\"'~^?!#$%&@"; *s; s++)
        r->set[r->nset++] = (uint32_t)(unsigned char)*s;
    for (uint32_t c = 0xc0; c <= 0xff && r->nset < GLYPHS_MAX; c++)     /* the accented Latin letters look the most foreign */
        if (c != 0xd7 && c != 0xf7)
            r->set[r->nset++] = c;
    for (uint32_t c = 0x2550; c <= 0x256c && r->nset < GLYPHS_MAX; c += 3)
        r->set[r->nset++] = c;
}

static uint32_t pick_glyph(struct rain *r)
{
    return r->set[rnd_range(r, r->nset)];
}

/* ---- the rain -------------------------------------------------------------------------------------------------------- */

static void restart(struct rain *r, struct column *c, bool first)
{
    int pct = LM_CLAMP(r->speed_pct, 25, 400);
    c->speed = (0.22f + (float)rnd_range(r, 60) / 100.0f) * (float)pct / 100.0f;      /* 0.22 .. 0.81 rows a step */
    int dens = LM_CLAMP(r->density > 0 ? r->density : 100, 50, 300);
    int fade = 226 + rnd_range(r, 22) + (dens - 100) / 12;                          /* trails of roughly 18 to 70 rows, longer when it is crowded */
    c->fade = fade > 252 ? 252 : fade;
    c->pos = first ? -(float)rnd_range(r, LM_MAX(r->rows * 100 / dens, 2)) : -(float)rnd_range(r, LM_MAX(r->rows / 3 * 100 / dens, 2) + 2);
    c->row = (int)c->pos - 1;
}

static int rain_heads(const struct rain *r)
{
    int n = (LM_CLAMP(r->density > 0 ? r->density : 100, 50, 300) + 50) / 100;
    return LM_CLAMP(n, 1, 3);
}

static bool rain_resize(struct rain *r, int cols, int rows)
{
    if (cols < 1 || rows < 1)
        return false;
    free(r->glyph);
    free(r->bright);
    free(r->col);
    r->cols = cols;
    r->rows = rows;
    r->glyph = calloc((size_t)cols * (size_t)rows, sizeof(uint16_t));
    r->bright = calloc((size_t)cols * (size_t)rows, 1);
    r->col = calloc((size_t)cols * (size_t)rain_heads(r), sizeof(struct column));
    if (!r->glyph || !r->bright || !r->col)
        return false;
    for (int x = 0; x < cols * rain_heads(r); x++)
        restart(r, &r->col[x], true);
    for (size_t i = 0; i < (size_t)cols * (size_t)rows; i++)
        r->glyph[i] = (uint16_t)pick_glyph(r);
    return true;
}

static void rain_step(struct rain *r);

/* A rain that has been falling for a while already (so the screen is not empty at the start). */
static void rain_warm_up(struct rain *r)
{
    for (int i = 0; i < r->rows * 3 + 40; i++)
        rain_step(r);
}

/* One step: everything fades, the heads move on, and a few letters change. */
static void rain_step(struct rain *r)
{
    int nh = rain_heads(r);
    for (int x = 0; x < r->cols; x++) {
        int fade = r->col[x * nh].fade;                              /* the column's cells fade by its first head's rate */
        for (int y = 0; y < r->rows; y++) {
            uint8_t *b = &r->bright[(size_t)y * (size_t)r->cols + (size_t)x];
            if (*b)
                *b = (uint8_t)((*b * fade) >> 8);
        }
        for (int k = 0; k < nh; k++) {
            struct column *c = &r->col[x * nh + k];
            c->pos += c->speed;
            int head = (int)c->pos;
            while (c->row < head) {
                c->row++;
                if (c->row >= 0 && c->row < r->rows) {
                    size_t i = (size_t)c->row * (size_t)r->cols + (size_t)x;
                    r->bright[i] = 255;
                    r->glyph[i] = (uint16_t)pick_glyph(r);
                }
            }
            if (head - 8 > r->rows + (255 * 4 / (256 - c->fade)))      /* the whole trail has left the screen */
                restart(r, c, false);
        }
    }
    int mutate = LM_MAX(r->cols * r->rows / 50, 1);                  /* some letters change while they fade */
    for (int k = 0; k < mutate; k++) {
        size_t i = (size_t)rnd_range(r, r->cols * r->rows);
        if (r->bright[i] > 24 && r->bright[i] < 250)
            r->glyph[i] = (uint16_t)pick_glyph(r);
    }
}

/* Cell colour: the head is almost white, the trail green, fading to black. */
static uint32_t cell_color(int b)
{
    if (b >= 250)
        return 0xd8ffe0u;
    int g = b * b / 255 * 4 / 5 + b / 5;                              /* a little steeper than linear: the trail dies out gently */
    return ((uint32_t)(g / 9) << 16) | ((uint32_t)g << 8) | (uint32_t)(g / 4 + (b > 128 ? (b - 128) / 6 : 0));
}

/* ---- the room: rain on six surfaces, drawn by casting a ray for every pixel ------------------------------------------------ */

#define ROOM_W 6.0f                     /* x from -3 to 3 */
#define ROOM_H 3.0f                     /* y from 0 (floor) to 3 (ceiling) */
#define ROOM_D 6.0f                     /* z from 0 (behind the camera) to 6 (the far wall with the window) */
#define CELL_W 0.096f                   /* one letter (with its gap) in metres: big enough to read at a few metres */
#define CELL_H 0.17f
#define NSURF 6                         /* 0 left, 1 right, 2 near, 3 far, 4 floor, 5 ceiling */
/* The window in the far wall, in cells of that wall's rain (columns from the left, rows from the ceiling): glass in [WIN_C0, WIN_C1) x [WIN_R0, WIN_R1), the frame
 * FRAME_T wide around it and a cross of bars. The frame is made of finer letters (half the size) than the walls: they rain down the frame and the bars. */
#define WIN_C0 17
#define WIN_C1 46
#define WIN_R0 2
#define WIN_R1 13
#define FRAME_T 0.14f                   /* the frame's width in metres */
#define FCW (CELL_W * 0.5f)             /* the frame's letters: half the size of the walls' */
#define FCH (CELL_H * 0.5f)
#define GLYPH_MAX_CP 0x2600

/* The window in 3D: the frame stands out of the wall by ROOM_D - ZF, the opening is a recess (a tube of letters) down to the glass at ZG, and a cross of bars
 * sits in it between ZB0 and ZB1. Everything in metres. */
#define OPEN_X0 ((float)WIN_C0 * CELL_W - ROOM_W / 2)
#define OPEN_X1 ((float)WIN_C1 * CELL_W - ROOM_W / 2)
#define OPEN_Y1 (ROOM_H - (float)WIN_R0 * CELL_H)
#define OPEN_Y0 (ROOM_H - (float)WIN_R1 * CELL_H)
#define OUT_X0 (OPEN_X0 - FRAME_T)
#define OUT_X1 (OPEN_X1 + FRAME_T)
#define OUT_Y0 (OPEN_Y0 - FRAME_T)
#define OUT_Y1 (OPEN_Y1 + FRAME_T)
#define ZF (ROOM_D - 0.12f)
#define ZG (ROOM_D + 0.32f)
#define ZB0 (ROOM_D + 0.04f)
#define ZB1 (ROOM_D + 0.15f)
#define BAR_HALF_W 0.05f
#define BAR_HALF_H 0.06f
#define SUN_X (OPEN_X1 - 0.10f)             /* the sun is just past the glass's upper right corner: only a piece of it shows */
#define SUN_Y (OPEN_Y1 + 0.02f)


struct cam {
    float ox, oy, oz;                   /* position */
    float fx, fy, fz, rx, ry, rz, ux, uy, uz;       /* forward, right, up */
};

struct room {
    struct rain s[NSURF];
    struct rain fr;                     /* the letters of the window's frame, bars and recess (finer) */
    bool ready;
    float t;                            /* seconds of the camera's drift */
    float lut[256][3];                  /* cell colour by brightness */
    const uint8_t *gptr[GLYPH_MAX_CP];  /* glyph rows by code point */
};

static void room_surface_size(int surf, int *cols, int *rows)
{
    float lu = surf < 2 ? ROOM_D : ROOM_W, lv = surf < 4 ? ROOM_H : (surf == 4 || surf == 5) ? ROOM_D : ROOM_H;
    *cols = (int)ceilf(lu / CELL_W);
    *rows = (int)ceilf(lv / CELL_H);
}

static bool room_init(struct room *rm, uint64_t seed, int speed_pct, int density)
{
    for (int i = 0; i < NSURF; i++) {
        struct rain *r = &rm->s[i];
        memset(r, 0, sizeof(*r));
        r->rng = seed * 6364136223846793005ull + (uint64_t)(i + 1) * 1442695040888963407ull + 1;
        r->speed_pct = speed_pct;
        r->density = density;
        make_set(r);
        int cols, rows;
        room_surface_size(i, &cols, &rows);
        if (!rain_resize(r, cols, rows))
            return false;
        rain_warm_up(r);
    }
    {                                                                   /* the finer letters of the window's frame */
        struct rain *f = &rm->fr;
        memset(f, 0, sizeof(*f));
        f->rng = seed * 6364136223846793005ull + 99991ull;
        f->speed_pct = speed_pct;
        f->density = density;
        make_set(f);
        if (!rain_resize(f, (int)ceilf((OUT_X1 - OUT_X0) / FCW) + 2, (int)ceilf((OUT_Y1 - OUT_Y0) / FCH) + 2))
            return false;
        rain_warm_up(f);
    }
    for (int b = 0; b < 256; b++) {
        uint32_t c = cell_color(b);
        float gain = b >= 250 ? 1.0f : 2.0f;                              /* the room is lit by its own letters: brighter than the flat rain */
        rm->lut[b][0] = fminf((float)((c >> 16) & 255) * gain, 255.0f);
        rm->lut[b][1] = fminf((float)((c >> 8) & 255) * gain, 255.0f);
        rm->lut[b][2] = fminf((float)(c & 255) * gain, 255.0f);
    }
    for (int i = 0; i < rm->s[0].nset; i++)
        if (rm->s[0].set[i] < GLYPH_MAX_CP)
            rm->gptr[rm->s[0].set[i]] = lm_font_glyph(rm->s[0].set[i]);
    rm->ready = true;
    return true;
}

static void room_free(struct room *rm)
{
    for (int i = 0; i < NSURF; i++) {
        free(rm->s[i].glyph);
        free(rm->s[i].bright);
        free(rm->s[i].col);
    }
    free(rm->fr.glyph);
    free(rm->fr.bright);
    free(rm->fr.col);
}

static void room_step(struct room *rm)
{
    for (int i = 0; i < NSURF; i++)
        rain_step(&rm->s[i]);
    rain_step(&rm->fr);
    for (size_t i = 0; i < (size_t)rm->fr.cols * (size_t)rm->fr.rows; i++)      /* the letters of the frame keep changing, as the others do while they fade */
        if (rnd_range(&rm->fr, 100) < 4)
            rm->fr.glyph[i] = (uint16_t)pick_glyph(&rm->fr);
    rm->t += (float)TICK_MS / 1000.0f;
}

/* The camera: a slow drift in the room and a slow turn of the head, so the walls move and the window slides over. */
static struct cam cam_at(float t)
{
    struct cam c;
    c.ox = 0.55f * sinf(t * 0.11f) + 0.2f * sinf(t * 0.29f + 1.0f);
    c.oy = 1.55f + 0.05f * sinf(t * 0.63f);
    c.oz = 1.6f + 0.7f * sinf(t * 0.07f + 0.5f);
    float yaw = 0.22f * sinf(t * 0.09f) + 0.05f * sinf(t * 0.37f), pitch = 0.05f * sinf(t * 0.13f + 2.0f) - 0.02f;
    float sy = sinf(yaw), cy = cosf(yaw), sp = sinf(pitch), cp = cosf(pitch);
    c.fx = sy * cp; c.fy = sp; c.fz = cy * cp;
    c.rx = cy; c.ry = 0.0f; c.rz = -sy;
    c.ux = -sy * sp; c.uy = cp; c.uz = -cy * sp;
    return c;
}

/* Which surface a ray from the camera meets first and how far along it (t in units of the direction's length). */
static inline int room_hit(const struct cam *c, float dx, float dy, float dz, float *t_out)
{
    float tx = 1e9f, ty = 1e9f, tz = 1e9f;
    int sx = 0, sz = 3, sy = 4;
    if (dx > 1e-6f) { tx = (ROOM_W / 2 - c->ox) / dx; sx = 1; }
    else if (dx < -1e-6f) { tx = (-ROOM_W / 2 - c->ox) / dx; sx = 0; }
    if (dy > 1e-6f) { ty = (ROOM_H - c->oy) / dy; sy = 5; }
    else if (dy < -1e-6f) { ty = -c->oy / dy; sy = 4; }
    if (dz > 1e-6f) { tz = (ROOM_D - c->oz) / dz; sz = 3; }
    else if (dz < -1e-6f) { tz = -c->oz / dz; sz = 2; }
    if (tx <= ty && tx <= tz) { *t_out = tx; return sx; }
    if (ty <= tz) { *t_out = ty; return sy; }
    *t_out = tz;
    return sz;
}

static inline float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

struct rjob {
    const struct room *rm;
    const struct cam *cam;
    uint32_t *dst;
    int w, h, stride, y0, y1;
    float f;                            /* focal length in pixels */
};

/* One letter's pixel: cell (ci, cj) of surface `r`, (fu, fv) inside the cell, brightness at least `bfloor`, `foot` = metres a pixel covers, k = light. */
static inline void letters_px(const struct room *rm, const struct rain *r, int ci, int cj, float fu, float fv, int bfloor, float foot, float gp, float k, float rgb[3])
{
    size_t ix = (size_t)cj * (size_t)r->cols + (size_t)ci;
    int b = r->bright[ix];
    if (b < bfloor)
        b = bfloor;
    rgb[0] = 0.0f; rgb[1] = 7.0f; rgb[2] = 3.0f;                            /* the bare wall: a very dark green */
    if (b > 6) {
        int gx = (int)(fu * 9.0f), gy = (int)(fv * 16.0f);
        const uint8_t *g = r->glyph[ix] < GLYPH_MAX_CP ? rm->gptr[r->glyph[ix]] : NULL;
        float bit = (g && gx < 8 && gy < 16 && ((g[gy] >> gx) & 1)) ? 1.0f : 0.0f;
        float mix = clamp01((foot / gp - 0.9f) * 0.6f) * 0.6f;              /* far / grazing: part of it stays the letter's own shape, so it still reads as text */
        float cov = bit * (1.0f - mix) + 0.3f * mix;
        cov = cov * 0.92f + 0.08f;                                          /* the cell glows a little around its letter */
        for (int q = 0; q < 3; q++)
            rgb[q] += rm->lut[b][q] * cov;
    }
    rgb[0] *= k; rgb[1] *= k; rgb[2] *= k;
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* The glass: light blue sky, brighter towards the horizon, with a piece of the sun in its upper right corner. (x, y) on the glass plane. */
static void glass_px(float x, float y, float rgb[3])
{
    float kk = clamp01((y - OPEN_Y0) / (OPEN_Y1 - OPEN_Y0));
    rgb[0] = 118.0f - 40.0f * kk; rgb[1] = 182.0f - 28.0f * kk; rgb[2] = 240.0f - 12.0f * kk;
    float d = hypotf(x - SUN_X, y - SUN_Y);
    float glow = clamp01(1.0f - d / 1.1f);
    glow = glow * glow;
    float core = clamp01((0.34f - d) / 0.05f);                              /* the disc, a soft edge */
    for (int q = 0; q < 3; q++) {
        static const float warm[3] = { 255.0f, 236.0f, 170.0f }, white[3] = { 255.0f, 252.0f, 235.0f };
        rgb[q] = rgb[q] + (warm[q] - rgb[q]) * glow * 0.8f;
        rgb[q] = rgb[q] + (white[q] - rgb[q]) * core;
    }
}

/* A ray (from the camera, direction d) that reaches the window's frame or opening before anything else of the room: its colour. `tf` = where it crosses the
 * frame's front plane ZF. False when it misses the window altogether. */
static bool window_trace(const struct room *rm, const struct cam *c, float dx, float dy, float dz, float tf, float len, float inv_f, float rgb[3])
{
    const struct rain *r = &rm->fr;
    const float gp = FCH / 16.0f;
    float x = c->ox + tf * dx, y = c->oy + tf * dy;
    if (x < OUT_X0 || x > OUT_X1 || y < OUT_Y0 || y > OUT_Y1)
        return false;
    if (x < OPEN_X0 || x > OPEN_X1 || y < OPEN_Y0 || y > OPEN_Y1) {                        /* the front of the frame */
        float cu = (x - OUT_X0) / FCW, cv = (OUT_Y1 - y) / FCH;
        int ci = clampi((int)cu, 0, r->cols - 1), cj = clampi((int)cv, 0, r->rows - 1);
        float dist = tf * len, foot = dist * inv_f / fmaxf(fabsf(dz) / len, 0.12f);
        letters_px(rm, r, ci, cj, cu - (float)ci, cv - (float)cj, 150, foot, gp, 1.0f / (1.0f + 0.018f * dist * dist), rgb);
        return true;
    }
    /* inside the opening: the first of the bars, the sides of the recess and the glass */
    float tg = (ZG - c->oz) / dz;
    float tsx = dx > 1e-6f ? (OPEN_X1 - c->ox) / dx : dx < -1e-6f ? (OPEN_X0 - c->ox) / dx : 1e9f;
    float tsy = dy > 1e-6f ? (OPEN_Y1 - c->oy) / dy : dy < -1e-6f ? (OPEN_Y0 - c->oy) / dy : 1e9f;
    float tend = fminf(tg, fminf(tsx, tsy));
    float cx = (OPEN_X0 + OPEN_X1) * 0.5f, cy = (OPEN_Y0 + OPEN_Y1) * 0.5f;
    for (int face = 0; face < 2; face++) {                                                   /* the bars: the front face, then the side the ray comes in at */
        float tb = ((face == 0 ? ZB0 : ZB1) - c->oz) / dz;
        if (tb >= tend)
            continue;
        float bx = c->ox + tb * dx, by = c->oy + tb * dy;
        if (fabsf(bx - cx) < BAR_HALF_W || fabsf(by - cy) < BAR_HALF_H) {
            float cu = (bx - OUT_X0) / FCW, cv = (OUT_Y1 - by) / FCH;
            int ci = clampi((int)cu, 0, r->cols - 1), cj = clampi((int)cv, 0, r->rows - 1);
            float dist = tb * len, foot = dist * inv_f / fmaxf(fabsf(dz) / len, 0.12f);
            letters_px(rm, r, ci, cj, cu - (float)ci, cv - (float)cj, 140, foot, gp, (face == 0 ? 1.0f : 0.6f) / (1.0f + 0.018f * dist * dist), rgb);
            return true;
        }
    }
    float dist = tend * len;
    float px = c->ox + tend * dx, py = c->oy + tend * dy, pz = c->oz + tend * dz;
    if (tend == tg) {                                                                        /* the glass */
        glass_px(px, py, rgb);
        return true;
    }
    float foot = dist * inv_f / 0.5f, k = 0.8f / (1.0f + 0.018f * dist * dist);              /* the sides of the recess: letters on the reveal */
    if (tend == tsx) {
        float cv = (OUT_Y1 - py) / FCH, cu = (pz - ZF) / FCW + (dx > 0 ? 0.0f : 7.0f);
        int cj = clampi((int)cv, 0, r->rows - 1), ci = (int)cu % r->cols;
        letters_px(rm, r, ci, cj, cu - floorf(cu), cv - (float)cj, 140, foot, gp, k, rgb);
    } else {
        float cu = (px - OUT_X0) / FCW, cv = (pz - ZF) / FCH + (dy > 0 ? 0.0f : 5.0f);
        int ci = clampi((int)cu, 0, r->cols - 1), cj = (int)cv % r->rows;
        letters_px(rm, r, ci, cj, cu - (float)ci, cv - floorf(cv), 140, foot, gp, k, rgb);
    }
    return true;
}

static void render_rows(const struct rjob *j)
{
    const struct room *rm = j->rm;
    const struct cam *c = j->cam;
    float inv_f = 1.0f / j->f, cxp = (float)j->w * 0.5f, cyp = (float)j->h * 0.5f;
    for (int y = j->y0; y < j->y1; y++) {
        float vy = (cyp - ((float)y + 0.5f)) * inv_f;
        float bx = c->fx + vy * c->ux, by = c->fy + vy * c->uy, bz = c->fz + vy * c->uz;
        uint32_t *row = j->dst + (size_t)y * (size_t)j->stride;
        for (int x = 0; x < j->w; x++) {
            float hx = (((float)x + 0.5f) - cxp) * inv_f;
            float dx = bx + hx * c->rx, dy = by + hx * c->ry, dz = bz + hx * c->rz;
            float t, rgb[3];
            int surf = room_hit(c, dx, dy, dz, &t);
            float len = sqrtf(dx * dx + dy * dy + dz * dz);
            bool win = false;
            if (dz > 1e-6f) {
                float tf = (ZF - c->oz) / dz;
                if (tf > 0.0f && tf < t)
                    win = window_trace(rm, c, dx, dy, dz, tf, len, inv_f, rgb);
            }
            if (!win) {
                float px = c->ox + t * dx, py = c->oy + t * dy, pz = c->oz + t * dz;
                float dist = t * len;
                float u, v, du, dvv, along;                                    /* the point on the surface, the distance to its edges, the ray's share across it */
                if (surf < 2) {
                    u = pz; v = ROOM_H - py; du = fminf(pz, ROOM_D - pz); dvv = fminf(py, ROOM_H - py); along = fabsf(dx);
                } else if (surf < 4) {
                    u = px + ROOM_W / 2; v = ROOM_H - py; du = fminf(u, ROOM_W - u); dvv = fminf(py, ROOM_H - py); along = fabsf(dz);
                } else {
                    u = px + ROOM_W / 2; v = pz; du = fminf(u, ROOM_W - u); dvv = fminf(pz, ROOM_D - pz); along = fabsf(dy);
                }
                const struct rain *r = &rm->s[surf];
                float cu = u / CELL_W, cv = v / CELL_H;
                int ci = clampi((int)cu, 0, r->cols - 1), cj = clampi((int)cv, 0, r->rows - 1);
                float ao = 0.45f + 0.55f * clamp01(fminf(du, dvv) / 0.5f);
                letters_px(rm, r, ci, cj, cu - (float)ci, cv - (float)cj, 0, dist * inv_f / fmaxf(along / len, 0.12f), CELL_H / 16.0f,
                           ao / (1.0f + 0.018f * dist * dist), rgb);
            }
            int r8 = (int)rgb[0], g8 = (int)rgb[1], b8 = (int)rgb[2];
            row[x] = 0xff000000u | ((uint32_t)(r8 > 255 ? 255 : r8) << 16) | ((uint32_t)(g8 > 255 ? 255 : g8) << 8) | (uint32_t)(b8 > 255 ? 255 : b8);
        }
    }
}

static void *render_thread(void *arg)
{
    render_rows(arg);
    return NULL;
}

/* One picture of the room into dst (w x h, `stride` pixels per row), on up to `threads` threads. */
static void room_render(const struct room *rm, uint32_t *dst, int w, int h, int stride, int threads)
{
    struct cam c = cam_at(rm->t);
    float f = (float)h * 0.5f / tanf(0.5f * 62.0f * 3.14159265f / 180.0f);          /* a vertical field of view of 62 degrees */
    threads = LM_CLAMP(threads, 1, 8);
    struct rjob jobs[8];
    pthread_t th[8];
    bool started[8] = { false };
    for (int i = 0; i < threads; i++) {
        jobs[i] = (struct rjob){ rm, &c, dst, w, h, stride, h * i / threads, h * (i + 1) / threads, f };
        if (i > 0)
            started[i] = pthread_create(&th[i], NULL, render_thread, &jobs[i]) == 0;
    }
    render_rows(&jobs[0]);
    for (int i = 1; i < threads; i++) {
        if (started[i])
            pthread_join(th[i], NULL);
        else
            render_rows(&jobs[i]);
    }
}

/* ---- the window ------------------------------------------------------------------------------------------------------ */

struct app {
    struct wlwin win;
    struct rain rain;
    struct room room;                   /* the room (unless --flat) */
    bool flat;
    int threads;
    uint32_t *scratch;                  /* the room's picture when it is drawn smaller than the window */
    size_t scratch_n;
    int scale, cw, ch;                  /* glyph scale, cell size in pixels */
    bool windowed;
    bool have_ptr;
    double px, py, bx, by;              /* pointer now / where it first appeared */
    int64_t last_ms;
    int frames;
};

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Mirrored glyph, `scale` times bigger, written straight into the buffer. */
static void draw_cell(struct wlwin *w, int x0, int y0, uint32_t cp, int scale, uint32_t rgb)
{
    const uint8_t *g = lm_font_glyph(cp);
    uint32_t px = 0xff000000u | rgb;
    for (int row = 0; row < LM_FONT_H; row++) {
        for (int bit = 0; bit < LM_FONT_W; bit++) {
            if (!(g[row] & (1u << bit)))                    /* bit 0 is the rightmost pixel: reading it first mirrors the letter */
                continue;
            for (int dy = 0; dy < scale; dy++) {
                int y = y0 + row * scale + dy;
                if (y < 0 || y >= w->bh)
                    break;
                uint32_t *line = w->pixels + (size_t)y * (size_t)w->bw;
                for (int dx = 0; dx < scale; dx++) {
                    int x = x0 + bit * scale + dx;
                    if (x >= 0 && x < w->bw)
                        line[x] = px;
                }
            }
        }
    }
}

/* The room, drawn at the window's size or (a very big window) at half of it and doubled. */
static void paint_room(struct wlwin *w)
{
    struct app *a = w->data;
    int rs = (size_t)w->bw * (size_t)w->bh > 2600000u ? 2 : 1;
    int rw = w->bw / rs, rh = w->bh / rs;
    if (rs == 1) {
        room_render(&a->room, w->pixels, w->bw, w->bh, w->bw, a->threads);
    } else {
        if (a->scratch_n < (size_t)rw * (size_t)rh) {
            free(a->scratch);
            a->scratch_n = (size_t)rw * (size_t)rh;
            a->scratch = malloc(a->scratch_n * 4);
        }
        if (!a->scratch)
            return;
        room_render(&a->room, a->scratch, rw, rh, rw, a->threads);
        for (int y = 0; y < w->bh; y++) {
            const uint32_t *src = a->scratch + (size_t)LM_MIN(y / rs, rh - 1) * (size_t)rw;
            uint32_t *dst = w->pixels + (size_t)y * (size_t)w->bw;
            for (int x = 0; x < w->bw; x++)
                dst[x] = src[LM_MIN(x / rs, rw - 1)];
        }
    }
    a->frames++;
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    if (!a->flat && a->room.ready) {
        paint_room(w);
        return;
    }
    struct rain *r = &a->rain;
    a->scale = w->bh >= 900 ? 2 : 1;
    a->cw = (LM_FONT_W + 1) * a->scale;
    a->ch = LM_FONT_H * a->scale;
    int cols = w->bw / a->cw, rows = (w->bh + a->ch - 1) / a->ch;
    if (cols != r->cols || rows != r->rows) {
        if (!rain_resize(r, cols, rows))
            return;
        rain_warm_up(r);
    }
    memset(w->pixels, 0, (size_t)w->bw * (size_t)w->bh * 4);
    int xoff = (w->bw - cols * a->cw) / 2;
    for (int y = 0; y < r->rows; y++)
        for (int x = 0; x < r->cols; x++) {
            int b = r->bright[(size_t)y * (size_t)r->cols + (size_t)x];
            if (b > 10)
                draw_cell(w, xoff + x * a->cw, y * a->ch, r->glyph[(size_t)y * (size_t)r->cols + (size_t)x], a->scale, cell_color(b));
        }
    a->frames++;
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->px = a->bx = lm_fixed_to_double(args[2].f);
        a->py = a->by = lm_fixed_to_double(args[3].f);
        a->have_ptr = true;
        if (!a->windowed)                       /* no cursor in the middle of the show */
            WLC_SEND(w->c, w->pointer, LM_WL_POINTER_REQ_SET_CURSOR, { .u = args[0].u }, { .o = 0 }, { .i = 0 }, { .i = 0 });
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        if (!a->windowed && a->have_ptr && (a->px - a->bx > MOVE_END_PX || a->bx - a->px > MOVE_END_PX || a->py - a->by > MOVE_END_PX ||
                                            a->by - a->py > MOVE_END_PX))
            w->running = false;
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        if (!a->windowed && args[3].u == 1)
            w->running = false;
        break;
    case LM_WL_POINTER_EVT_AXIS:
        if (!a->windowed)
            w->running = false;
        break;
    }
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op == LM_WL_KEYBOARD_EVT_KEY && args[3].u == 1) {
        if (!a->windowed || lm_keymap_keysym(args[2].u, w->mods) == LM_KS_Escape)
            w->running = false;
    }
}

/* ---- self test ------------------------------------------------------------------------------------------------------- */

static int selftest_fail;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "selftest: %s:%d: %s\n", __FILE__, __LINE__, #cond); selftest_fail++; } } while (0)

static int selftest(void)
{
    struct rain r = { .rng = 88172645463325252ull, .speed_pct = 100 };
    make_set(&r);
    CHECK(r.nset > 60 && r.nset <= GLYPHS_MAX);
    for (int i = 0; i < r.nset; i++)
        CHECK(lm_font_glyph(r.set[i]) != lm_font_glyph(0xfffd));        /* every letter of the set really exists in the font */
    CHECK(rain_resize(&r, 40, 30));
    rain_warm_up(&r);
    bool saw_head = false, saw_trail = false;
    for (int step = 0; step < 400; step++) {
        rain_step(&r);
        for (int i = 0; i < r.cols * r.rows; i++) {
            if (r.bright[i] == 255)
                saw_head = true;
            else if (r.bright[i] > 30 && r.bright[i] < 250)
                saw_trail = true;
            CHECK(r.glyph[i] != 0);
        }
    }
    CHECK(saw_head && saw_trail);
    int lit = 0;
    for (int i = 0; i < r.cols * r.rows; i++)
        lit += r.bright[i] > 10;
    CHECK(lit > r.cols * r.rows / 20 && lit < r.cols * r.rows);          /* a rain, not a blank screen and not a solid block */
    CHECK(cell_color(255) == 0xd8ffe0u);
    CHECK(((cell_color(120) >> 8) & 255) > ((cell_color(60) >> 8) & 255));
    CHECK(((cell_color(120) >> 8) & 255) > ((cell_color(120) >> 16) & 255));      /* green dominates */
    CHECK(cell_color(0) == 0);
    struct rain s1 = { .rng = 7, .speed_pct = 100 }, s2 = { .rng = 7, .speed_pct = 100 };    /* the same seed gives the same rain */
    make_set(&s1);
    make_set(&s2);
    CHECK(rain_resize(&s1, 20, 20) && rain_resize(&s2, 20, 20));
    for (int step = 0; step < 50; step++) {
        rain_step(&s1);
        rain_step(&s2);
    }
    CHECK(!memcmp(s1.bright, s2.bright, 400) && !memcmp(s1.glyph, s2.glyph, 800));
    CHECK(!rain_resize(&s1, 0, 5));
    {                                                                    /* the room: which wall a ray meets, the window, a picture that is not blank */
        struct cam c = { 0.0f, 1.5f, 2.0f, 0, 0, 1, 1, 0, 0, 0, 1, 0 };
        float t;
        CHECK(room_hit(&c, 0, 0, 1, &t) == 3 && fabsf(t - (ROOM_D - 2.0f)) < 1e-3f);        /* ahead: the far wall */
        CHECK(room_hit(&c, 0, 0, -1, &t) == 2 && fabsf(t - 2.0f) < 1e-3f);                  /* behind: the near wall */
        CHECK(room_hit(&c, -1, 0, 0, &t) == 0 && room_hit(&c, 1, 0, 0, &t) == 1);            /* left, right */
        CHECK(room_hit(&c, 0, -1, 0, &t) == 4 && room_hit(&c, 0, 1, 0, &t) == 5);            /* floor, ceiling */
        {                                                                /* the glass is blue, the sun is at its corner, the recess and the frame have letters */
            float g1[3], g2[3];
            glass_px((OPEN_X0 + OPEN_X1) / 2, (OPEN_Y0 + OPEN_Y1) / 2, g1);
            CHECK(g1[2] > g1[0] + 40.0f && g1[2] > 150.0f);
            glass_px(SUN_X, SUN_Y, g2);
            CHECK(g2[0] > 240.0f && g2[1] > 240.0f);
            CHECK(OPEN_X1 - OPEN_X0 > 2.0f && OPEN_Y1 > OPEN_Y0 && ZG > ROOM_D && ZF < ROOM_D);
        }
        struct cam c0 = cam_at(0.0f), c1 = cam_at(20.0f);
        CHECK(c0.fz > 0.9f && (fabsf(c0.ox - c1.ox) > 0.05f || fabsf(c0.oz - c1.oz) > 0.05f));    /* it looks into the room and it drifts */
        struct room rm;
        CHECK(room_init(&rm, 3, 100, 220));
        for (int i = 0; i < 40; i++)
            room_step(&rm);
        static uint32_t img[96 * 54];
        room_render(&rm, img, 96, 54, 96, 2);
        int lit = 0, dark = 0;
        for (int i = 0; i < 96 * 54; i++) {
            lit += ((img[i] >> 8) & 255) > 40;
            dark += ((img[i] >> 8) & 255) < 12;
        }
        CHECK(lit > 20 && dark > 96 * 54 / 3);                                             /* rain on dark walls, not one or the other */
        CHECK(rain_heads(&rm.s[0]) == 2);
        room_free(&rm);
    }
    free(r.glyph); free(r.bright); free(r.col);
    free(s1.glyph); free(s1.bright); free(s1.col);
    free(s2.glyph); free(s2.bright); free(s2.col);
    if (selftest_fail)
        return 1;
    puts("mellagui-matrix selftest ok");
    return 0;
}

/* ---- main ------------------------------------------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    lm_theme_load_env();
    struct app a = { 0 };
    a.rain.speed_pct = 100;
    a.rain.rng = (uint64_t)now_ms() * 2654435761u + (uint64_t)getpid() + 1;
    bool selftest_only = false;
    const char *bench = NULL;
    a.rain.density = 260;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) {
            selftest_only = true;
        } else if (!strcmp(argv[i], "--windowed")) {
            a.windowed = true;
        } else if (!strcmp(argv[i], "--flat")) {
            a.flat = true;
        } else if (!strcmp(argv[i], "--density") && i + 1 < argc) {
            a.rain.density = LM_CLAMP(atoi(argv[++i]), 50, 300);
        } else if (!strcmp(argv[i], "--bench") && i + 1 < argc) {
            bench = argv[++i];
        } else if (!strcmp(argv[i], "--speed") && i + 1 < argc) {
            a.rain.speed_pct = LM_CLAMP(atoi(argv[++i]), 25, 400);
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            a.rain.rng = (uint64_t)atoll(argv[++i]) * 2654435761u + 1;
        } else {
            fprintf(stderr, "usage: mellagui-matrix [--windowed] [--flat] [--density PERCENT] [--speed PERCENT] [--seed N] [--bench WxH] [--selftest]\n");
            return 2;
        }
    }
    make_set(&a.rain);
    if (selftest_only)
        return selftest();
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    a.threads = (int)LM_CLAMP(ncpu, 1, 4);
    if (!a.flat) {
        if (!room_init(&a.room, a.rain.rng, a.rain.speed_pct, a.rain.density)) {
            fprintf(stderr, "mellagui-matrix: out of memory\n");
            return 1;
        }
    }
    if (bench) {
        int bw = 1920, bh = 1080;
        sscanf(bench, "%dx%d", &bw, &bh);
        uint32_t *buf = malloc((size_t)bw * (size_t)bh * 4);
        if (!buf || a.flat)
            return 1;
        int64_t t0 = now_ms();
        int n = 20;
        for (int i = 0; i < n; i++) {
            room_step(&a.room);
            room_render(&a.room, buf, bw, bh, bw, a.threads);
        }
        printf("%dx%d: %.1f ms a frame on %d threads\n", bw, bh, (double)(now_ms() - t0) / n, a.threads);
        if (getenv("MELLAGUI_MATRIX_PPM")) {
            FILE *f = fopen(getenv("MELLAGUI_MATRIX_PPM"), "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", bw, bh);
                for (size_t i = 0; i < (size_t)bw * (size_t)bh; i++) {
                    uint8_t rgb[3] = { (uint8_t)(buf[i] >> 16), (uint8_t)(buf[i] >> 8), (uint8_t)buf[i] };
                    fwrite(rgb, 1, 3, f);
                }
                fclose(f);
            }
        }
        return 0;
    }

    static const struct wlwin_ops ops = { .paint = paint, .pointer = pointer_event, .keyboard = keyboard_event };
    if (wlwin_connect(&a.win, "mellagui-matrix", &ops, &a) < 0)
        return 1;
    a.win.want_fullscreen = !a.windowed;
    if (wlwin_open(&a.win, "Matrix", "mellagui.matrix", 960, 540, 160, 100) < 0)
        return 1;
    a.last_ms = now_ms();
    a.win.redraw = true;
    while (wlwin_step(&a.win, TICK_MS)) {
        int64_t now = now_ms();
        int ticks = 0;
        while (now - a.last_ms >= TICK_MS && ticks < 4) {            /* keep the speed when a frame took long, but never run far behind */
            if (a.flat)
                rain_step(&a.rain);
            else
                room_step(&a.room);
            a.last_ms += TICK_MS;
            ticks++;
        }
        if (now - a.last_ms >= TICK_MS * 4)
            a.last_ms = now;
        if (ticks)
            a.win.redraw = true;
    }
    wlwin_destroy(&a.win);
    if (a.room.ready)
        room_free(&a.room);
    free(a.scratch);
    free(a.rain.glyph);
    free(a.rain.bright);
    free(a.rain.col);
    return 0;
}
