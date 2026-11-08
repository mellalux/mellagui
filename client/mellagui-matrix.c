/*
 * mellagui-matrix: a screensaver, falling green code in the manner of the film.
 *
 *   mellagui-matrix                 full screen, any key, button or a pointer movement ends it (the compositor also ends it by itself
 *                                   when it started the program as the screensaver: settings, Screensaver page)
 *   mellagui-matrix --windowed      an ordinary window (to look at it, or to leave running beside other windows)
 *   mellagui-matrix --speed PERCENT how fast the rain falls, 25..400 (default 100)
 *   mellagui-matrix --seed N        fixed random numbers (tests)
 *   mellagui-matrix --selftest      checks the rain's arithmetic without a display
 *
 * Every column has a bright head that falls at its own speed and leaves a trail that fades out. The letters are the digits, the Latin letters
 * and some signs of the built-in font, mirrored like the ones in the film, and they change here and there while they fade.
 * Drawn in software into one shm buffer like the other small programs (wlwin.c); about 25 frames a second.
 */
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
    struct column *col;
    uint64_t rng;
    int speed_pct;
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
    c->fade = 226 + rnd_range(r, 22);                                               /* trails of roughly 18 to 70 rows */
    c->pos = first ? -(float)rnd_range(r, LM_MAX(r->rows, 2)) : -(float)rnd_range(r, LM_MAX(r->rows / 3, 2) + 2);
    c->row = (int)c->pos - 1;
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
    r->col = calloc((size_t)cols, sizeof(struct column));
    if (!r->glyph || !r->bright || !r->col)
        return false;
    for (int x = 0; x < cols; x++)
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
    for (int x = 0; x < r->cols; x++) {
        struct column *c = &r->col[x];
        for (int y = 0; y < r->rows; y++) {
            uint8_t *b = &r->bright[(size_t)y * (size_t)r->cols + (size_t)x];
            if (*b)
                *b = (uint8_t)((*b * c->fade) >> 8);
        }
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

/* ---- the window ------------------------------------------------------------------------------------------------------ */

struct app {
    struct wlwin win;
    struct rain rain;
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

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
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
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) {
            selftest_only = true;
        } else if (!strcmp(argv[i], "--windowed")) {
            a.windowed = true;
        } else if (!strcmp(argv[i], "--speed") && i + 1 < argc) {
            a.rain.speed_pct = LM_CLAMP(atoi(argv[++i]), 25, 400);
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            a.rain.rng = (uint64_t)atoll(argv[++i]) * 2654435761u + 1;
        } else {
            fprintf(stderr, "usage: mellagui-matrix [--windowed] [--speed PERCENT] [--seed N] [--selftest]\n");
            return 2;
        }
    }
    make_set(&a.rain);
    if (selftest_only)
        return selftest();

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
            rain_step(&a.rain);
            a.last_ms += TICK_MS;
            ticks++;
        }
        if (now - a.last_ms >= TICK_MS * 4)
            a.last_ms = now;
        if (ticks)
            a.win.redraw = true;
    }
    wlwin_destroy(&a.win);
    free(a.rain.glyph);
    free(a.rain.bright);
    free(a.rain.col);
    return 0;
}
