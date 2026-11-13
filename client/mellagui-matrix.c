/*
 * mellagui-matrix: a screensaver, falling green code in the manner of the film.
 *
 *   mellagui-matrix                 full screen, any key, button or a pointer movement ends it (the compositor also ends it by itself
 *                                   when it started the program as the screensaver: settings, Screensaver page)
 *   mellagui-matrix --flat          the classic flat rain instead of the room
 *   mellagui-matrix --at SECONDS   starts the camera's drift and the cat's story at that moment (to look at a certain pose, and the tests)
 *   mellagui-matrix --no-cat        the room without Tom, the cat that lives on its floor (client/matrixcat.c)
 *   mellagui-matrix --density PCT   how crowded the rain is, 50..300 (default 260)
 *   mellagui-matrix --time HH:MM    the window shows that time of day (default: the real time, following the clock): sun, moon and stars, colours
 *   mellagui-matrix --clock-speed N the day passes N minutes a second (to look at it)
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
#include "matrixcat.h"
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


struct cam {
    float ox, oy, oz;                   /* position */
    float fx, fy, fz, rx, ry, rz, ux, uy, uz;       /* forward, right, up */
};

/* The sky behind the window, from the time of day: the sun and the moon in the sky, the colours, and which light falls on the floor. */
struct sky {
    float sun[3], moon[3];              /* directions towards them (unit; x to the right, y up, z out of the window) */
    float sun_el, moon_el;              /* elevations in degrees */
    float zen[3], hor[3];               /* the sky's colour at the zenith and at the horizon */
    float day, night;                   /* 0..1: how much of a day it is / how deep the night is */
    float land;                         /* 0..1: how much light the land gets (it is lit a little longer than the sky is blue) */
    float light_dir[3], light_col[3];   /* the light on the floor: direction towards its source (z > 0) and its colour, already scaled by its strength */
    float light_str;                    /* 0 = no light */
    float phase;                        /* seconds, for the twinkling of the stars */
};

struct room {
    struct sky sky;
    float minutes;                      /* the time of day shown, minutes since midnight */
    float clock_speed;                  /* simulated minutes per real second; 0 = the real clock */
    bool fixed;                         /* --time: the clock stands still */
    struct rain s[NSURF];
    struct rain fr;                     /* the letters of the window's frame, bars and recess (finer) */
    bool ready;
    float t;                            /* seconds of the camera's drift */
    bool no_cat;                        /* --no-cat */
    const uint8_t *catg[MCAT_GLYPHS];   /* the glyphs the cat's fur is made of */
    float lut[256][3];                  /* cell colour by brightness */
    const uint8_t *gptr[GLYPH_MAX_CP];  /* glyph rows by code point */
};

static void room_surface_size(int surf, int *cols, int *rows)
{
    float lu = surf < 2 ? ROOM_D : ROOM_W, lv = surf < 4 ? ROOM_H : (surf == 4 || surf == 5) ? ROOM_D : ROOM_H;
    *cols = (int)ceilf(lu / CELL_W);
    *rows = (int)ceilf(lv / CELL_H);
}

static void sky_update(struct sky *k, float minutes);
static void ridge_init(void);

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
    if (!rm->fixed) {
        time_t now = time(NULL);
        struct tm tm;
        if (localtime_r(&now, &tm))
            rm->minutes = (float)tm.tm_hour * 60.0f + (float)tm.tm_min + (float)tm.tm_sec / 60.0f;
    }
    for (int i = 0; i < MCAT_GLYPHS; i++)                               /* the fur's alphabet: a spread over the rain's own set */
        rm->catg[i] = lm_font_glyph(rm->s[0].set[(i * 7) % rm->s[0].nset]);
    ridge_init();
    sky_update(&rm->sky, rm->minutes);
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
    if (rm->fixed) {
        /* a fixed time stands still */
    } else if (rm->clock_speed > 0.0f) {
        rm->minutes = fmodf(rm->minutes + rm->clock_speed * (float)TICK_MS / 1000.0f, 1440.0f);
    } else {
        time_t now = time(NULL);
        struct tm tm;
        if (localtime_r(&now, &tm))
            rm->minutes = (float)tm.tm_hour * 60.0f + (float)tm.tm_min + (float)tm.tm_sec / 60.0f;
    }
    sky_update(&rm->sky, rm->minutes);
    rm->sky.phase = rm->t;
}

/* The camera: a slow drift in the room and a slow turn of the head, so the walls move and the window slides over. */
static struct cam cam_at(float t)
{
    struct cam c;
    c.ox = 0.55f * sinf(t * 0.11f) + 0.2f * sinf(t * 0.29f + 1.0f);
    c.oy = 1.55f + 0.05f * sinf(t * 0.63f);
    c.oz = 1.6f + 0.7f * sinf(t * 0.07f + 0.5f);
    float yaw = 0.22f * sinf(t * 0.09f) + 0.05f * sinf(t * 0.37f), pitch = 0.05f * sinf(t * 0.13f + 2.0f) - 0.09f;
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
    const struct mcat *cat;             /* NULL = no cat */
    int cx0, cx1, cy0, cy1;             /* the part of the picture the cat (and its glow) can be in ... */
    const uint8_t *cmask;               /* ... and which 8 x 8 pixel tiles of it (the spheres of its parts, projected), `ctw` tiles a row */
    int ctw;
    uint32_t *dst;
    int w, h, stride, y0, y1, ystep;    /* the rows y0, y0 + ystep, ... below y1: the threads take every n-th row, so that a busy part of the picture is shared */
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

/* ---- the sky ---------------------------------------------------------------------------------------------------------- */

static float mixf(float a, float b, float t) { return a + (b - a) * t; }

static float smooth(float a, float b, float v)
{
    float t = clamp01((v - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

/* A direction towards a body that crosses the sky between 06:00 and 18:00 of its own day (`phase` hours since it rose; outside 0..12 it is below the horizon):
 * from the left to the right of the window, as high as `max_el` degrees. */
static void body_dir(float phase, float max_el, float dir[3], float *el_deg, float *az_rad)
{
    float t = phase / 12.0f, el = max_el * sinf(3.14159265f * t);
    float az = (-40.0f + 80.0f * clamp01(t)) * 3.14159265f / 180.0f, e = el * 3.14159265f / 180.0f;
    dir[0] = sinf(az) * cosf(e);
    dir[1] = sinf(e);
    dir[2] = cosf(az) * cosf(e);
    *el_deg = el;
    *az_rad = az;
}

/* The sky colours by the sun's elevation: night, dusk, sunset, golden hour, day. */
static void sky_colors(float el, float zen[3], float hor[3])
{
    static const float E[5] = { -8.0f, -3.0f, 0.0f, 3.0f, 10.0f };
    static const float Z[5][3] = { { 3, 5, 14 }, { 22, 30, 70 }, { 70, 90, 160 }, { 80, 130, 210 }, { 72, 135, 228 } };
    static const float H[5][3] = { { 6, 10, 22 }, { 120, 70, 90 }, { 255, 150, 80 }, { 240, 185, 140 }, { 160, 205, 245 } };
    int i = 0;
    while (i < 3 && el > E[i + 1])
        i++;
    float t = clamp01((el - E[i]) / (E[i + 1] - E[i]));
    for (int q = 0; q < 3; q++) {
        zen[q] = mixf(Z[i][q], Z[i + 1][q], t);
        hor[q] = mixf(H[i][q], H[i + 1][q], t);
    }
}

static void sky_update(struct sky *k, float minutes)
{
    float h = minutes / 60.0f;
    float saz, maz;
    body_dir(h - 6.0f, 11.0f, k->sun, &k->sun_el, &saz);
    float mh = h - 18.0f;
    if (mh < 0.0f)
        mh += 24.0f;
    if (mh > 24.0f)
        mh -= 24.0f;
    body_dir(mh, 10.0f, k->moon, &k->moon_el, &maz);
    if (k->sun_el < 0.0f && h > 6.0f && h < 18.0f)
        k->sun_el = 0.0f;
    sky_colors(k->sun_el, k->zen, k->hor);
    k->day = smooth(-3.0f, 6.0f, k->sun_el);
    k->night = smooth(-2.0f, -8.0f, k->sun_el);
    k->land = smooth(-4.0f, 3.0f, k->sun_el);
    /* the light on the floor: the sun while it is up, else the moon */
    float sunl = smooth(0.5f, 4.5f, k->sun_el), moonl = 0.3f * smooth(1.0f, 6.0f, k->moon_el) * k->night;
    /* (the light comes from a higher point of the sky than the body shows at: the patch has to land where the camera can see it) */
    float az = sunl >= moonl ? saz : maz, el = sunl >= moonl ? k->sun_el : k->moon_el;
    float lel = (sunl >= moonl ? 38.0f + 0.9f * el : 42.0f + 0.7f * el) * 3.14159265f / 180.0f;
    k->light_str = sunl >= moonl ? sunl : moonl;
    k->light_dir[0] = sinf(az) * cosf(lel);
    k->light_dir[1] = sinf(lel);
    k->light_dir[2] = cosf(az) * cosf(lel);
    if (sunl >= moonl) {
        float w = clamp01(k->sun_el / 12.0f);                            /* low sun: orange; high sun: pale yellow */
        k->light_col[0] = 255.0f;
        k->light_col[1] = mixf(140.0f, 228.0f, w);
        k->light_col[2] = mixf(60.0f, 150.0f, w);
    } else {
        k->light_col[0] = 150.0f; k->light_col[1] = 180.0f; k->light_col[2] = 255.0f;
    }
    if (k->light_dir[1] < 0.05f || k->light_dir[2] < 0.05f)
        k->light_str = 0.0f;
}

/* How much of the light (direction b towards its source) reaches the floor at (x, z): the window's opening carried along the light, tested at the frame's and
 * the glass's plane, with the shadows of the bars at their depth and soft edges. */
static float sun_light(float x, float z, const float b[3])
{
    float cx = (OPEN_X0 + OPEN_X1) * 0.5f, cy = (OPEN_Y0 + OPEN_Y1) * 0.5f, f = 1.0f;
    static const float planes[3] = { ZG, ZF, ZB0 + (ZB1 - ZB0) * 0.5f };
    if (b[2] < 0.05f || b[1] < 0.03f)
        return 0.0f;
    for (int i = 0; i < 3; i++) {
        float sdist = (planes[i] - z) / b[2];
        if (sdist < 0.0f)
            return 0.0f;                                                  /* the point is behind that plane: not lit through it */
        float qx = x + sdist * b[0], qy = sdist * b[1];
        if (i < 2) {                                                      /* through the frame's opening and the glass's */
            float m = fminf(fminf(qx - OPEN_X0, OPEN_X1 - qx), fminf(qy - OPEN_Y0, OPEN_Y1 - qy));
            f *= clamp01(m / 0.07f);
        } else {                                                          /* the bars throw shadows */
            float m = fminf(fabsf(qx - cx) - BAR_HALF_W, fabsf(qy - cy) - BAR_HALF_H);
            f *= clamp01(m / 0.05f);
        }
        if (f <= 0.0f)
            return 0.0f;
    }
    return f;
}

static inline uint32_t hash3(int a, int b, int c)
{
    uint32_t h = (uint32_t)a * 73856093u ^ (uint32_t)b * 19349663u ^ (uint32_t)c * 83492791u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h;
}

/* ---- the land behind the window: three rows of hills, a meadow with fields and a path, village lights at night ---------------------------------- */

static inline float noise1(float x, float p)
{
    return 0.5f + 0.5f * (0.5f * sinf(x * 1.7f + p) + 0.3f * sinf(x * 4.3f + p * 2.1f) + 0.2f * sinf(x * 9.1f + p * 0.7f));
}

/* The three ridges' heights by the sideways position u (-4..4), made once: three rows of sines per pixel were the most expensive part of the picture. */
#define RIDGE_N 4096
#define RIDGE_U 4.0f
static float ridge_tab[3][RIDGE_N + 1];
static bool ridge_ready;

static void ridge_init(void)
{
    if (ridge_ready)
        return;
    for (int i = 0; i <= RIDGE_N; i++) {
        float u = -RIDGE_U + 2.0f * RIDGE_U * (float)i / (float)RIDGE_N;
        ridge_tab[0][i] = 0.012f + 0.030f * noise1(u * 2.0f, 1.3f);
        ridge_tab[1][i] = -0.018f + 0.034f * noise1(u * 3.1f, 4.1f);
        ridge_tab[2][i] = -0.080f + 0.038f * noise1(u * 4.3f, 7.7f);
    }
    ridge_ready = true;
}

static inline float ridge_at(int which, float u)
{
    float t = (clamp01((u + RIDGE_U) / (2.0f * RIDGE_U))) * (float)RIDGE_N;
    int i = (int)t;
    if (i >= RIDGE_N)
        return ridge_tab[which][RIDGE_N];
    float f = t - (float)i;
    return ridge_tab[which][i] + (ridge_tab[which][i + 1] - ridge_tab[which][i]) * f;
}

/* Blends the colour `c` (scaled by the light of the hour) over rgb with coverage `cov`. */
static inline void land_mix(float rgb[3], const float c[3], float cov)
{
    for (int q = 0; q < 3; q++)
        rgb[q] = mixf(rgb[q], c[q], cov);
}

/* Lights a colour like the day does: full sun by day (warm when low), the dim blue of the night otherwise, a little of the sky's horizon colour as haze. */
static void land_light(const struct sky *k, const float base[3], float haze, float out[3])
{
    float w = clamp01(k->sun_el / 12.0f);
    float tint[3] = { 1.0f, mixf(0.66f, 1.0f, w), mixf(0.48f, 0.96f, w) };
    for (int q = 0; q < 3; q++) {
        float nightc = base[q] * (q == 2 ? 0.20f : q == 1 ? 0.13f : 0.09f) + (q == 2 ? 10.0f : 3.0f);     /* dark blue-green */
        float dayc = base[q] * tint[q];
        float c = mixf(nightc, dayc, k->land);
        out[q] = mixf(c, k->hor[q] * mixf(0.35f, 0.85f, k->land), haze);
    }
}

/* The three layers of the land, one function each. `cov` = how much of the pixel the layer covers. */
static void layer_far(const struct sky *k, float cov, float rgb[3])
{
    float c[3], base[3] = { 78.0f, 108.0f, 140.0f };                        /* blue with haze */
    land_light(k, base, 0.55f, c);
    land_mix(rgb, c, cov);
}

static void layer_mid(const struct sky *k, float u, float hy, float rmid, float cov, float rgb[3])
{
    float c[3];
    float edge = clamp01(1.0f - (rmid - hy) / 0.012f);                      /* the top of the hill catches the light */
    float base[3] = { 58.0f + 22.0f * edge, 98.0f + 24.0f * edge, 80.0f + 8.0f * edge };
    land_light(k, base, 0.28f, c);
    land_mix(rgb, c, cov);
    if (k->night > 0.3f && hy < rmid - 0.002f) {                            /* the lights of a village */
        uint32_t h = hash3((int)floorf(u * 520.0f), (int)floorf(hy * 700.0f), 5);
        if ((h & 0xffu) < 5u) {
            float tw = 0.7f + 0.3f * sinf(k->phase * 2.0f + (float)(h >> 8));
            static const float lamp[3] = { 255.0f, 205.0f, 120.0f };
            land_mix(rgb, lamp, clamp01(k->night * tw));
        }
    }
}

static void layer_near(const struct sky *k, float hx, float hy, float hz, float cov, float rgb[3])
{
    float c[3], base[3];                                                    /* a meadow of fields in perspective with a winding path */
    float ey = fminf(hy, -0.004f);                                          /* (a view from a height of 40 m: the ground at 40 / -hy metres) */
    float inv = 40.0f / -ey, gx = hx * inv, gz = hz * inv, dist = inv;
    uint32_t h = hash3((int)floorf(gx * (1.0f / 70.0f)), (int)floorf(gz * (1.0f / 70.0f)), 11);
    static const float fields[4][3] = { { 66, 116, 56 }, { 92, 132, 60 }, { 126, 142, 66 }, { 52, 98, 62 } };
    const float *f = fields[h & 3];
    float stripe = 0.88f + 0.12f * sinf((h & 4u ? gx : gz) * 0.55f);       /* furrows, one way or the other */
    base[0] = f[0] * stripe; base[1] = f[1] * stripe; base[2] = f[2] * stripe;
    float pathd = fabsf(gx - 55.0f * sinf(gz * (1.0f / 160.0f)) - 20.0f);  /* the path */
    if (pathd < 6.0f + dist * 0.002f) {
        base[0] = 150.0f; base[1] = 126.0f; base[2] = 92.0f;
    }
    land_light(k, base, clamp01(dist * (1.0f / 2600.0f)) * 0.45f, c);
    land_mix(rgb, c, cov);
    if (k->night > 0.3f && dist > 200.0f) {                                 /* a few windows in the dark */
        uint32_t hl = hash3((int)floorf(gx * (1.0f / 9.0f)), (int)floorf(gz * (1.0f / 9.0f)), 23);
        if ((hl & 0xffu) < 3u) {
            static const float lamp[3] = { 255.0f, 205.0f, 120.0f };
            land_mix(rgb, lamp, clamp01(k->night * 0.9f));
        }
    }
}

/* Draws the land over `rgb` for the direction (hx, hy, hz): from the far hills to the near meadow (a layer that covers a pixel completely hides the ones behind). */
static void land_px(const struct sky *k, float hx, float hy, float hz, float rgb[3])
{
    float u = hx / fmaxf(hz, 0.3f);                                         /* the sideways position along the view */
    const float w = 0.0016f;                                                /* the soft edge of a ridge */
    float rfar = ridge_at(0, u), rmid = ridge_at(1, u), rnear = ridge_at(2, u);
    float cn = clamp01((rnear - hy) / w + 0.5f), cm = clamp01((rmid - hy) / w + 0.5f), cf = clamp01((rfar - hy) / w + 0.5f);
    if (cn >= 1.0f) {                                                       /* the meadow only: the common case */
        layer_near(k, hx, hy, hz, 1.0f, rgb);
        return;
    }
    if (cm >= 1.0f) {
        layer_mid(k, u, hy, rmid, 1.0f, rgb);
    } else {
        if (cf > 0.0f)
            layer_far(k, cf, rgb);
        if (cm > 0.0f)
            layer_mid(k, u, hy, rmid, cm, rgb);
    }
    if (cn > 0.0f)
        layer_near(k, hx, hy, hz, cn, rgb);
}

/* The sky seen along the unit direction (hx, hy, hz): the sky's gradient, the sun, the moon and stars at night, the ground below the horizon. */
static void sky_px(const struct sky *k, float hx, float hy, float hz, float rgb[3])
{
    if (hy < 0.0f) {                                                        /* below the horizon: only land */
        for (int q = 0; q < 3; q++)
            rgb[q] = k->hor[q];
        land_px(k, hx, hy, hz, rgb);
        return;
    }
    float t = powf(clamp01(hy / 0.65f), 0.7f);
    for (int q = 0; q < 3; q++)
        rgb[q] = mixf(k->hor[q], k->zen[q], t);
    if (k->night > 0.02f) {                                                 /* a starry sky: the Milky Way, faint stars, bright stars that twinkle */
        float side = hx * 0.45f - hy * 0.35f + hz * 0.82f;                   /* the band's distance (in the sky's units) from its middle plane */
        if (side * side < 0.2f) {
            float band = expf(-(side * side) / 0.03f);
            uint32_t mh = hash3((int)floorf(hx * 14.0f), (int)floorf(hy * 14.0f), (int)floorf(hz * 14.0f));
            float mott = 0.55f + 0.45f * (float)(mh & 255) / 255.0f;          /* cloudy, not smooth */
            float glow = band * mott * 0.16f * k->night * clamp01(hy / 0.12f);
            static const float milk[3] = { 150.0f, 165.0f, 215.0f };
            for (int q = 0; q < 3; q++)
                rgb[q] = mixf(rgb[q], milk[q], glow);
        }
        for (int layer = 0; layer < 2; layer++) {
            const float K = layer == 0 ? 90.0f : 230.0f;
            int ix = (int)floorf(hx * K), iy = (int)floorf(hy * K), iz = (int)floorf(hz * K);
            uint32_t h = hash3(ix + layer * 977, iy, iz);
            if ((h & 0x3ffu) < (layer == 0 ? 60u : 90u)) {
                float cxs = ((float)((h >> 10) & 255) + 0.5f) / 256.0f, cys = ((float)((h >> 18) & 255) + 0.5f) / 256.0f;
                float dx = hx * K - (float)ix - cxs, dy = hy * K - (float)iy - cys;
                float r2 = (dx * dx + dy * dy) * (1.0f / (K * K));
                float rad = layer == 0 ? 0.0013f + 0.0006f * (float)((h >> 26) & 3) : 0.0007f;
                if (r2 < rad * rad) {
                    float tw = 0.78f + 0.22f * sinf(k->phase * (1.0f + (float)((h >> 5) & 3)) + (float)((h >> 8) & 63));
                    float b = k->night * (layer == 0 ? 0.55f + 0.45f * (float)((h >> 8) & 255) / 255.0f : 0.4f) * tw * clamp01(hy / 0.08f);
                    float tint = (float)((h >> 12) & 3);                         /* a few are bluish or warm */
                    float col[3] = { tint == 0.0f ? 255.0f : 235.0f, 244.0f, tint == 1.0f ? 255.0f : 225.0f };
                    for (int q = 0; q < 3; q++)
                        rgb[q] = mixf(rgb[q], col[q], clamp01(b));
                }
            }
        }
    }
    float cs = hx * k->sun[0] + hy * k->sun[1] + hz * k->sun[2];            /* the sun: a disc and a glow */
    if (cs > 0.76f && k->sun_el > -3.0f) {
        float ang = acosf(fminf(cs, 1.0f)), vis = smooth(-3.0f, 1.0f, k->sun_el);
        float glow = clamp01(1.0f - ang / 0.7f);
        glow = glow * glow * 0.8f * vis;
        float core = clamp01((0.085f - ang) / 0.012f) * vis;
        float w = clamp01(k->sun_el / 12.0f);
        float warm[3] = { 255.0f, mixf(150.0f, 238.0f, w), mixf(70.0f, 175.0f, w) }, white[3] = { 255.0f, mixf(225.0f, 252.0f, w), mixf(190.0f, 235.0f, w) };
        for (int q = 0; q < 3; q++) {
            rgb[q] = mixf(rgb[q], warm[q], glow);
            rgb[q] = mixf(rgb[q], white[q], core);
        }
    }
    float cm = hx * k->moon[0] + hy * k->moon[1] + hz * k->moon[2];         /* the moon: a crescent (a disc with another disc cut out of it) and a soft glow */
    if (cm > 0.92f && k->night > 0.05f && k->moon_el > -1.0f) {
        const float *m = k->moon;
        float t1[3] = { m[2], 0.0f, -m[0] }, tl = sqrtf(t1[0] * t1[0] + t1[2] * t1[2]);        /* the sky's right and up at the moon */
        t1[0] /= tl; t1[2] /= tl;
        float t2[3] = { m[1] * t1[2] - m[2] * t1[1], m[2] * t1[0] - m[0] * t1[2], m[0] * t1[1] - m[1] * t1[0] };
        float lx = hx * t1[0] + hy * t1[1] + hz * t1[2], ly = hx * t2[0] + hy * t2[1] + hz * t2[2];
        const float R = 0.088f, e = 0.009f;
        float d1 = hypotf(lx, ly), d2 = hypotf(lx - 0.058f, ly - 0.026f);                      /* the second disc is moved up and to the right: the lit part is the left */
        float disc = clamp01((R - d1) / e), bite = clamp01((R * 0.92f - d2) / e);
        float core = disc * (1.0f - bite) * k->night;
        float glow = clamp01(1.0f - d1 / 0.4f);
        glow = glow * glow * 0.22f * k->night;
        for (int q = 0; q < 3; q++) {
            static const float pale[3] = { 238, 240, 255 };
            rgb[q] = mixf(rgb[q], 150.0f, glow);
            rgb[q] = mixf(rgb[q], pale[q], core);
        }
    }
    if (hy < 0.07f)
        land_px(k, hx, hy, hz, rgb);                                        /* the hills rise above the horizon: they hide the sun and the stars behind them */
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
        sky_px(&rm->sky, dx / len, dy / len, dz / len, rgb);                              /* the sky at infinity, seen along the ray */
        (void)px; (void)py;
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
    for (int y = j->y0; y < j->y1; y += j->ystep) {
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
                if (surf == 4) {                                               /* the sun's patch on the floor: the letters in it are brighter and the light is warm */
                    float sl = rm->sky.light_str > 0.0f ? sun_light(px, pz, rm->sky.light_dir) * rm->sky.light_str : 0.0f;
                    if (sl > 0.0f) {
                        float lit = sl / (1.0f + 0.02f * dist * dist);
                        for (int q = 0; q < 3; q++)
                            rgb[q] = rgb[q] * (1.0f + 1.3f * sl) + rm->sky.light_col[q] * 0.50f * lit;
                    }
                }
                if (surf == 4 && j->cat) {                                     /* Tom's shadow on the floor (after the sun's patch: the light is what he blocks) */
                    float sh = mcat_floor_shade(j->cat, px, pz);
                    rgb[0] *= sh; rgb[1] *= sh; rgb[2] *= sh;
                }
            }
            if (j->cat && x >= j->cx0 && x <= j->cx1 && y >= j->cy0 && y <= j->cy1 && j->cmask[(y >> 3) * j->ctw + (x >> 3)]) {   /* the cat, if it is nearer than what the ray met */
                float o3[3] = { c->ox, c->oy, c->oz }, dn[3] = { dx / len, dy / len, dz / len };
                mcat_shade(j->cat, o3, dn, (win ? (ZF - c->oz) / dz : t) * len, inv_f, rgb);
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
    struct mcat cat;
    memset(&cat, 0, sizeof(cat));
    if (!rm->no_cat) {
        float co[3] = { c.ox, c.oy, c.oz };
        mcat_pose(&cat, rm->t, co, rm->catg, MCAT_GLYPHS, rm->sky.light_dir, rm->sky.light_col, rm->sky.light_str);
    }
    float f = (float)h * 0.5f / tanf(0.5f * 62.0f * 3.14159265f / 180.0f);          /* a vertical field of view of 62 degrees */
    int cx0 = 0, cx1 = w - 1, cy0 = 0, cy1 = h - 1;
    if (!rm->no_cat) {                                                  /* where the cat's sphere shows: a rectangle round it (the whole picture when we are close) */
        float vx = cat.centre[0] - c.ox, vy = cat.centre[1] - c.oy, vz = cat.centre[2] - c.oz;
        float zc = vx * c.fx + vy * c.fy + vz * c.fz, xc = vx * c.rx + vy * c.ry + vz * c.rz, yc = vx * c.ux + vy * c.uy + vz * c.uz;
        float rr = cat.radius;
        if (zc > rr * 1.2f) {
            float rpx = f * rr / sqrtf(zc * zc - rr * rr) + 4.0f, mx = (float)w * 0.5f + f * xc / zc, my = (float)h * 0.5f - f * yc / zc;
            cx0 = (int)fmaxf(mx - rpx, 0.0f); cx1 = (int)fminf(mx + rpx, (float)(w - 1));
            cy0 = (int)fmaxf(my - rpx, 0.0f); cy1 = (int)fminf(my + rpx, (float)(h - 1));
        }
    }
    int ctw = (w + 7) / 8, cth = (h + 7) / 8;
    uint8_t *cmask = NULL;
    if (!rm->no_cat) {                                                  /* which 8 x 8 tiles the parts of the cat can be in: the spheres of its parts, projected */
        cmask = calloc((size_t)ctw * (size_t)cth, 1);
        float sp[48][4];
        int ns = cmask ? mcat_spheres(&cat, sp, 48) : 0;
        for (int i = 0; i < ns; i++) {
            float vx = sp[i][0] - c.ox, vy = sp[i][1] - c.oy, vz = sp[i][2] - c.oz;
            float zc = vx * c.fx + vy * c.fy + vz * c.fz, xc = vx * c.rx + vy * c.ry + vz * c.rz, yc = vx * c.ux + vy * c.uy + vz * c.uz;
            if (zc <= sp[i][3] * 1.2f) {                                /* too close to project: the whole picture */
                memset(cmask, 1, (size_t)ctw * (size_t)cth);
                break;
            }
            float rpx = f * sp[i][3] / sqrtf(zc * zc - sp[i][3] * sp[i][3]) + 3.0f, mx = (float)w * 0.5f + f * xc / zc, my = (float)h * 0.5f - f * yc / zc;
            int tx0 = (int)fmaxf((mx - rpx) / 8.0f, 0.0f), tx1 = (int)fminf((mx + rpx) / 8.0f, (float)(ctw - 1));
            int ty0 = (int)fmaxf((my - rpx) / 8.0f, 0.0f), ty1 = (int)fminf((my + rpx) / 8.0f, (float)(cth - 1));
            for (int ty = ty0; ty <= ty1; ty++)
                for (int tx = tx0; tx <= tx1; tx++) {
                    float ddx = ((float)tx + 0.5f) * 8.0f - mx, ddy = ((float)ty + 0.5f) * 8.0f - my, lim = rpx + 6.0f;
                    if (ddx * ddx + ddy * ddy <= lim * lim)
                        cmask[ty * ctw + tx] = 1;
                }
        }
    }
    threads = LM_CLAMP(threads, 1, 8);
    struct rjob jobs[8];
    pthread_t th[8];
    bool started[8] = { false };
    for (int i = 0; i < threads; i++) {
        jobs[i] = (struct rjob){ rm, &c, cmask ? &cat : NULL, cx0, cx1, cy0, cy1, cmask, ctw, dst, w, h, stride, i, h, threads, f };
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
    free(cmask);
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
                if (y < 0 || y >= w->ph)
                    break;
                uint32_t *line = w->pixels + (size_t)y * (size_t)w->pw;
                for (int dx = 0; dx < scale; dx++) {
                    int x = x0 + bit * scale + dx;
                    if (x >= 0 && x < w->pw)
                        line[x] = px;
                }
            }
        }
    }
}

/* The room, drawn at the buffer's size (the screen's own pixels) or (a very big one) at half of it and doubled: the picture is a ray cast,
 * so a scaled screen costs only the pixels that are really drawn. */
static void paint_room(struct wlwin *w)
{
    struct app *a = w->data;
    int rs = (size_t)w->pw * (size_t)w->ph > 2600000u ? 2 : 1;
    int rw = w->pw / rs, rh = w->ph / rs;
    if (rs == 1) {
        room_render(&a->room, w->pixels, w->pw, w->ph, w->pw, a->threads);
    } else {
        if (a->scratch_n < (size_t)rw * (size_t)rh) {
            free(a->scratch);
            a->scratch_n = (size_t)rw * (size_t)rh;
            a->scratch = malloc(a->scratch_n * 4);
        }
        if (!a->scratch)
            return;
        room_render(&a->room, a->scratch, rw, rh, rw, a->threads);
        for (int y = 0; y < w->ph; y++) {
            const uint32_t *src = a->scratch + (size_t)LM_MIN(y / rs, rh - 1) * (size_t)rw;
            uint32_t *dst = w->pixels + (size_t)y * (size_t)w->pw;
            for (int x = 0; x < w->pw; x++)
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
    a->scale = (w->bh >= 900 ? 2 : 1) * wlwin_scale(w);               /* the letters are as big as before on the screen: in the buffer's pixels */
    a->cw = (LM_FONT_W + 1) * a->scale;
    a->ch = LM_FONT_H * a->scale;
    int cols = w->pw / a->cw, rows = (w->ph + a->ch - 1) / a->ch;
    if (cols != r->cols || rows != r->rows) {
        if (!rain_resize(r, cols, rows))
            return;
        rain_warm_up(r);
    }
    memset(w->pixels, 0, (size_t)w->pw * (size_t)w->ph * 4);
    int xoff = (w->pw - cols * a->cw) / 2;
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
        {                                                                /* the sky, hour by hour */
            struct sky k;
            float g[3];
            ridge_init();
            sky_update(&k, 12.0f * 60.0f);
            CHECK(k.day > 0.95f && k.night < 0.05f && k.sun_el > 9.0f && k.light_str > 0.9f);
            sky_px(&k, 0.0f, 0.7f, 0.714f, g);                                               /* a blue sky by day */
            CHECK(g[2] > g[0] + 60.0f && g[2] > 150.0f);
            sky_px(&k, 0.0f, -0.3f, 0.95f, g);                                               /* ground below the horizon */
            CHECK(g[1] > g[2] && g[1] > 40.0f);
            int green = 0, lit = 0;                                                           /* the land: fields and hills in daylight ... */
            for (int i = 0; i < 4000; i++) {
                float hy = -0.02f - 0.12f * (float)(i / 100) / 40.0f, hx = -0.3f + 0.006f * (float)(i % 100);
                sky_px(&k, hx, hy, 0.9f, g);
                green += g[1] > g[2] && g[1] > g[0] && g[1] > 50.0f;
            }
            CHECK(green > 2400);
            sky_update(&k, 0.0f);                                                             /* ... and with lights at night */
            for (int i = 0; i < 40000; i++) {
                float hy = -0.06f - 0.09f * (float)(i / 200) / 200.0f, hx = -0.3f + 0.003f * (float)(i % 200);
                sky_px(&k, hx, hy, 0.9f, g);
                lit += g[0] > 150.0f && g[1] > 120.0f;
            }
            CHECK(lit > 3);
            sky_update(&k, 12.0f * 60.0f);
            sky_update(&k, 0.0f);
            CHECK(k.day < 0.05f && k.night > 0.95f && k.sun_el < -9.0f);
            sky_px(&k, -0.5f, 0.6f, 0.62f, g);                                               /* nearly black at night (away from the moon) */
            CHECK(g[0] < 40.0f && g[1] < 40.0f && g[2] < 60.0f);
            int stars = 0;
            for (int i = 0; i < 40000; i++) {                                                /* ... with stars in it */
                float hx = -0.3f + 0.0015f * (float)(i % 200), hy = 0.3f + 0.0015f * (float)(i / 200);
                sky_px(&k, hx, hy, 0.8f, g);
                stars += g[0] > 100.0f;
            }
            CHECK(stars > 20 && stars < 4000);
            sky_update(&k, 18.0f * 60.0f);                                                   /* sunset: an orange horizon, the sun low on the right */
            CHECK(k.hor[0] > 200.0f && k.hor[2] < k.hor[0] - 60.0f && k.sun[0] > 0.5f && k.sun_el < 1.0f);
            sky_update(&k, 6.0f * 60.0f);                                                    /* sunrise: on the left */
            CHECK(k.sun[0] < -0.5f);
            sky_update(&k, 14.0f * 60.0f);
            float sunp[3] = { k.light_dir[0], k.light_dir[1], k.light_dir[2] }, qx = (OPEN_X0 + OPEN_X1) * 0.5f + 0.7f, qy = OPEN_Y0 + 0.55f * (OPEN_Y1 - OPEN_Y0);
            float sd = qy / sunp[1];                                                         /* a floor point whose light comes through a pane */
            CHECK(sun_light(qx - sd * sunp[0], ZG - sd * sunp[2], sunp) > 0.0f);
            CHECK(sun_light(-2.9f, 0.3f, sunp) == 0.0f);
            sky_update(&k, 23.0f * 60.0f);                                                   /* the moon's light is dimmer and blue */
            CHECK(k.moon_el > 4.0f && k.light_str > 0.0f && k.light_str < 0.35f && k.light_col[2] > k.light_col[0]);
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
        {                                                                /* the cat: it is hit, it turns its head, its eyes glow when it looks at the camera */
            struct mcat ct;
            float cam[3] = { 0.0f, 1.55f, 1.6f }, ld[3] = { 0.0f, 0.7f, 0.7f }, lc[3] = { 255.0f, 230.0f, 180.0f };
            int n = 0, glowing = 0, away = 0;
            float ymin = 9.0f, ymax = -9.0f;
            for (float t = 0.0f; t < 240.0f; t += 0.1f) {
                mcat_pose(&ct, t, cam, rm.catg, MCAT_GLYPHS, ld, lc, 1.0f);
                n++;
                glowing += ct.glow > 0.5f;
                away += ct.glow < 0.01f;
                ymin = fminf(ymin, ct.head_yaw);
                ymax = fmaxf(ymax, ct.head_yaw);
                CHECK(ct.glow <= 1.0f && (ct.glow < 0.5f || ct.look > 0.9f));        /* a glow means the head really points at the camera */
            }
            CHECK(glowing > n / 10 && glowing < n * 3 / 4 && away > n / 5);            /* it does look at us now and then, and elsewhere the rest of the time */
            CHECK(ymax - ymin > 1.0f);                                                /* the head turns */
            mcat_pose(&ct, 12.0f, cam, rm.catg, MCAT_GLYPHS, ld, lc, 1.0f);
            float chest[3] = { ct.pos[0] + ct.scale * 0.25f * ct.sy, ct.pos[1] + ct.scale * 0.25f, ct.pos[2] + ct.scale * 0.25f * ct.cy }, dn[3], rgb[3] = { 12.0f, 34.0f, 56.0f };
            float len = sqrtf((chest[0] - cam[0]) * (chest[0] - cam[0]) + (chest[1] - cam[1]) * (chest[1] - cam[1]) + (chest[2] - cam[2]) * (chest[2] - cam[2]));
            for (int i = 0; i < 3; i++)
                dn[i] = (chest[i] - cam[i]) / len;
            CHECK(mcat_shade(&ct, cam, dn, 30.0f, 0.001f, rgb) && (rgb[0] != 12.0f || rgb[1] != 34.0f));        /* a ray at its chest meets it */
            rgb[0] = 12.0f; rgb[1] = 34.0f; rgb[2] = 56.0f;
            CHECK(mcat_shade(&ct, cam, dn, len - 0.5f, 0.001f, rgb) == false && rgb[1] == 34.0f);            /* ... not when something nearer was hit first */
            float up[3] = { 0.0f, 1.0f, 0.0f };
            CHECK(!mcat_shade(&ct, cam, up, 30.0f, 0.001f, rgb));                                             /* ... and a ray at the ceiling misses it */
            CHECK(mcat_floor_shade(&ct, ct.pos[0], ct.pos[2]) < 0.5f && mcat_floor_shade(&ct, -2.0f, 1.0f) == 1.0f);   /* its shadow */
            {                                                                   /* Tom's day: sits, stands up and walks, sits elsewhere, lies down, walks back */
                float cyc = mcat_cycle(), maxlie = 0.0f, maxwalk = 0.0f, xmin = 99.0f, xmax = -99.0f, tlie = 0.0f, twalk = 0.0f;
                for (float t = 0.0f; t < cyc; t += 0.25f) {
                    struct mcat d;
                    mcat_pose(&d, t, cam, rm.catg, MCAT_GLYPHS, ld, lc, 1.0f);
                    CHECK(d.lie == 0.0f || d.stand == 0.0f);                                   /* never lying and standing at once */
                    if (d.lie > maxlie) { maxlie = d.lie; tlie = t; }
                    if (d.walk > maxwalk) { maxwalk = d.walk; twalk = t; }
                    xmin = fminf(xmin, d.pos[0]);
                    xmax = fmaxf(xmax, d.pos[0]);
                }
                CHECK(maxlie > 0.99f && maxwalk > 0.95f && xmax - xmin > 1.2f);               /* he lies down, he walks, and he goes somewhere */
                struct mcat w0, w1, home;
                mcat_pose(&home, cyc, cam, rm.catg, MCAT_GLYPHS, ld, lc, 1.0f);
                mcat_pose(&ct, 0.0f, cam, rm.catg, MCAT_GLYPHS, ld, lc, 1.0f);
                CHECK(fabsf(home.pos[0] - ct.pos[0]) < 1e-3f && fabsf(home.pos[2] - ct.pos[2]) < 1e-3f);   /* the day starts and ends on the same spot */
                mcat_pose(&w0, twalk, cam, rm.catg, MCAT_GLYPHS, ld, lc, 1.0f);
                mcat_pose(&w1, twalk + 0.3f, cam, rm.catg, MCAT_GLYPHS, ld, lc, 1.0f);
                CHECK(w0.stand > 0.99f && fabsf(w1.pos[0] - w0.pos[0]) + fabsf(w1.pos[2] - w0.pos[2]) > 0.05f);       /* the body moves ... */
                CHECK(fabsf(w1.tube[1].p[2][2] - w0.tube[1].p[2][2]) > 0.02f || fabsf(w1.tube[2].p[2][2] - w0.tube[2].p[2][2]) > 0.02f);   /* ... and so do the paws */
                CHECK(w0.neck[1] > ct.neck[1] - 0.1f && w0.tube[0].p[7][1] > 0.5f);                  /* standing, the tail is up */
                struct mcat lying;
                mcat_pose(&lying, tlie, cam, rm.catg, MCAT_GLYPHS, ld, lc, 1.0f);
                CHECK(ct.lie == 0.0f && lying.lie > 0.99f && lying.neck[1] < ct.neck[1] - 0.15f);
                float low[3] = { lying.pos[0] + lying.scale * 0.12f * lying.sy, lying.pos[1] + lying.scale * 0.12f, lying.pos[2] + lying.scale * 0.12f * lying.cy };
                float dl = sqrtf((low[0] - cam[0]) * (low[0] - cam[0]) + (low[1] - cam[1]) * (low[1] - cam[1]) + (low[2] - cam[2]) * (low[2] - cam[2]));
                for (int i = 0; i < 3; i++)
                    dn[i] = (low[i] - cam[i]) / dl;
                rgb[0] = 12.0f; rgb[1] = 34.0f; rgb[2] = 56.0f;
                CHECK(mcat_shade(&lying, cam, dn, 30.0f, 0.001f, rgb) && (rgb[0] != 12.0f || rgb[1] != 34.0f));      /* a ray at its low back meets it */
                CHECK(mcat_floor_shade(&lying, lying.pos[0], lying.pos[2]) < 0.4f);                                  /* and it casts a shadow, too */
            }
            mcat_pose(&ct, 12.0f, cam, rm.catg, MCAT_GLYPHS, ld, lc, 1.0f);
            struct mcat c2;
            mcat_pose(&c2, 12.0f, cam, rm.catg, MCAT_GLYPHS, ld, lc, 1.0f);
            CHECK(c2.glow == ct.glow && c2.head_yaw == ct.head_yaw);                  /* the same time, the same pose */
        }
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
    wlwin_hidpi();                      /* the picture is made for the screen's own pixels */
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
        } else if (!strcmp(argv[i], "--at") && i + 1 < argc) {
            double at = atof(argv[++i]);                                        /* (a macro would evaluate argv[++i] several times) */
            a.room.t = (float)LM_CLAMP(at, 0.0, 100000.0);
        } else if (!strcmp(argv[i], "--no-cat")) {
            a.room.no_cat = true;
        } else if (!strcmp(argv[i], "--density") && i + 1 < argc) {
            int dens = atoi(argv[++i]);
            a.rain.density = LM_CLAMP(dens, 50, 300);
        } else if (!strcmp(argv[i], "--time") && i + 1 < argc) {              /* the time of day the window shows: HH:MM, standing still */
            int hh = 0, mm = 0;
            if (sscanf(argv[++i], "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59) {
                fprintf(stderr, "mellagui-matrix: --time wants HH:MM\n");
                return 2;
            }
            a.room.minutes = (float)(hh * 60 + mm);
            a.room.fixed = true;
        } else if (!strcmp(argv[i], "--clock-speed") && i + 1 < argc) {      /* the day passes faster: this many minutes a second (to look at the sky) */
            double spd = atof(argv[++i]);                                       /* (a macro would evaluate argv[++i] several times) */
            a.room.clock_speed = (float)LM_CLAMP(spd, 0.0, 1440.0);
        } else if (!strcmp(argv[i], "--bench") && i + 1 < argc) {
            bench = argv[++i];
        } else if (!strcmp(argv[i], "--speed") && i + 1 < argc) {
            int pct = atoi(argv[++i]);
            a.rain.speed_pct = LM_CLAMP(pct, 25, 400);
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            a.rain.rng = (uint64_t)atoll(argv[++i]) * 2654435761u + 1;
        } else {
            fprintf(stderr, "usage: mellagui-matrix [--windowed] [--flat] [--no-cat] [--at SECONDS] [--density PERCENT] [--time HH:MM] [--clock-speed MIN_PER_S] [--speed PERCENT] [--seed N] [--bench WxH] [--selftest]\n");
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
