/*
 * mellagui-tetris - Tetris for MellaGUI.
 *
 *   mellagui-tetris [--seed N] [--level 1..15] [--size WxH]     a game (the seed names the order of the pieces: the same seed is the same game)
 *   mellagui-tetris --selftest                                  the rules without a display
 *
 * Ten columns, twenty visible rows. Seven pieces come in bags of seven (every piece once, in a random order), so a long wait for the long one never
 * happens; the next three show on the right, the held one on the left. A piece that lands has half a second to be moved before it locks (fifteen
 * moves at most), the rotation is the Super Rotation System of the guideline (wall kicks), the fall speeds up every ten lines. A line is 100, two 300,
 * three 500, four 800 points times the level; a second four in a row is a half more, a line after a line adds a combo; a soft drop is a point a row, a
 * hard drop two. Not done: T-spins, a lock out of the board's top as a different kind of end, a two player game.
 * Keys: Left / Right move (held: they repeat), Down drops softly, Up / X turn clockwise, Z anticlockwise, Space drops hard, C / Shift holds a piece,
 * P / Esc pauses, G switches the ghost (where the piece would land), N a new game, F1 the keys, Ctrl+Q quits. The window looks like the other
 * programs: a rail of round drops at the left, theme colours, hidpi. A window that loses the keyboard pauses the game.
 * The best score is kept in $XDG_CONFIG_HOME/mellagui/tetris.conf.
 *
 * $MELLAGUI_TETRIS_DUMP=FILE writes the state after every paint for the tests (the score, the piece in the air, every row of the board, where the
 * buttons are); `--still` makes the pieces stop falling by themselves, `--piece I|J|L|O|S|T|Z` names the first piece and `--setup almost` /
 * `--setup top` deal a board that is one drop from four lines / already full (also for the tests).
 */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "font.h"
#include "keymap.h"
#include "metrics.h"
#include "protocol.h"
#include "theme.h"
#include "uikit.h"
#include "util.h"
#include "wlc.h"
#include "wlcard.h"
#include "wlrail.h"
#include "wlwin.h"

/* ================================================================ the rules (no display needed) ================================================================ */

#define BW 10                                               /* columns */
#define BH 22                                               /* rows: two hidden ones above the twenty that show */
#define VIS 20
#define HID (BH - VIS)
#define LOCK_MS 500                                         /* how long a piece that stands on something may still be moved */
#define LOCK_RESETS 15
#define QMAX 24

enum { T_I, T_J, T_L, T_O, T_S, T_T, T_Z, T_N };
enum { EV_LOCK = 1, EV_CLEAR = 2, EV_TETRIS = 4, EV_LEVEL = 8, EV_OVER = 16 };

static const char T_NAME[] = "IJLOSTZ";

/* the pieces turned the way they spawn, as cells (x, y) of a box of k_box x k_box */
static const int8_t k_shape0[T_N][4][2] = {
    [T_I] = { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } },
    [T_J] = { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },
    [T_L] = { { 2, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },
    [T_O] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } },
    [T_S] = { { 1, 0 }, { 2, 0 }, { 0, 1 }, { 1, 1 } },
    [T_T] = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },
    [T_Z] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 2, 1 } },
};
static const int k_box[T_N] = { 4, 3, 3, 2, 3, 3, 3 };
static int8_t g_shape[T_N][4][4][2];                        /* [piece][rotation][cell] = x, y */

static void shapes_init(void)
{
    static bool done;
    if (done)
        return;
    for (int t = 0; t < T_N; t++) {
        for (int c = 0; c < 4; c++) {
            g_shape[t][0][c][0] = k_shape0[t][c][0];
            g_shape[t][0][c][1] = k_shape0[t][c][1];
        }
        for (int r = 1; r < 4; r++)
            for (int c = 0; c < 4; c++) {                   /* a quarter turn clockwise inside the box */
                g_shape[t][r][c][0] = (int8_t)(k_box[t] - 1 - g_shape[t][r - 1][c][1]);
                g_shape[t][r][c][1] = g_shape[t][r - 1][c][0];
            }
    }
    done = true;
}

/* The wall kicks of the Super Rotation System, as the guideline writes them (x right, y UP): the offsets tried in this order for the turns 0>1, 1>0, 1>2, 2>1,
 * 2>3, 3>2, 3>0, 0>3. The board has y down, so the y of an offset is subtracted. */
static const int8_t k_kick[8][5][2] = {
    { { 0, 0 }, { -1, 0 }, { -1, 1 }, { 0, -2 }, { -1, -2 } },
    { { 0, 0 }, { 1, 0 }, { 1, -1 }, { 0, 2 }, { 1, 2 } },
    { { 0, 0 }, { 1, 0 }, { 1, -1 }, { 0, 2 }, { 1, 2 } },
    { { 0, 0 }, { -1, 0 }, { -1, 1 }, { 0, -2 }, { -1, -2 } },
    { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, -2 }, { 1, -2 } },
    { { 0, 0 }, { -1, 0 }, { -1, -1 }, { 0, 2 }, { -1, 2 } },
    { { 0, 0 }, { -1, 0 }, { -1, -1 }, { 0, 2 }, { -1, 2 } },
    { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, -2 }, { 1, -2 } },
};
static const int8_t k_kick_i[8][5][2] = {
    { { 0, 0 }, { -2, 0 }, { 1, 0 }, { -2, -1 }, { 1, 2 } },
    { { 0, 0 }, { 2, 0 }, { -1, 0 }, { 2, 1 }, { -1, -2 } },
    { { 0, 0 }, { -1, 0 }, { 2, 0 }, { -1, 2 }, { 2, -1 } },
    { { 0, 0 }, { 1, 0 }, { -2, 0 }, { 1, -2 }, { -2, 1 } },
    { { 0, 0 }, { 2, 0 }, { -1, 0 }, { 2, 1 }, { -1, -2 } },
    { { 0, 0 }, { -2, 0 }, { 1, 0 }, { -2, -1 }, { 1, 2 } },
    { { 0, 0 }, { 1, 0 }, { -2, 0 }, { 1, -2 }, { -2, 1 } },
    { { 0, 0 }, { -1, 0 }, { 2, 0 }, { -1, 2 }, { 2, -1 } },
};

struct piece {
    int type, rot;
    int x, y;                                               /* the top left corner of the piece's box on the board (rows count from the hidden top) */
};

struct game {
    uint8_t cell[BH][BW];                                   /* 0 = empty, else the piece's type + 1 */
    struct piece cur;
    bool have_cur;
    int hold;                                               /* the held piece, -1 = none */
    bool hold_used;                                         /* one swap for each piece that comes */
    uint8_t q[QMAX];                                        /* the pieces to come, the first one next */
    int nq;
    uint32_t rng, seed;
    int score, lines, level, start_level;
    int combo;                                              /* lines cleared in a row by consecutive pieces, minus one (-1 = the last piece cleared none) */
    bool b2b, over;
    int pieces;                                             /* pieces locked */
    int fall_ms, lock_ms, lock_resets;
    uint32_t clear_mask;                                    /* the rows (bit = row) that are full and wait to be taken away */
    int ev, last_clear, last_gain;                          /* what happened since the window looked, the lines of the last clear and its points */
    int64_t play_ms;
};

static uint32_t rng_next(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static void bag_refill(struct game *g)
{
    int bag[T_N];
    for (int i = 0; i < T_N; i++)
        bag[i] = i;
    for (int i = T_N - 1; i > 0; i--) {
        int k = (int)(rng_next(&g->rng) % (uint32_t)(i + 1));
        int t = bag[i];
        bag[i] = bag[k];
        bag[k] = t;
    }
    for (int i = 0; i < T_N && g->nq < QMAX; i++)
        g->q[g->nq++] = (uint8_t)bag[i];
}

static int take_next(struct game *g)
{
    while (g->nq < 8)
        bag_refill(g);
    int t = g->q[0];
    memmove(g->q, g->q + 1, (size_t)(g->nq - 1));
    g->nq--;
    while (g->nq < 8)
        bag_refill(g);
    return t;
}

static bool collides(const struct game *g, int type, int rot, int x, int y)
{
    for (int c = 0; c < 4; c++) {
        int cx = x + g_shape[type][rot][c][0], cy = y + g_shape[type][rot][c][1];
        if (cx < 0 || cx >= BW || cy < 0 || cy >= BH || g->cell[cy][cx])
            return true;
    }
    return false;
}

static bool grounded(const struct game *g)
{
    return g->have_cur && collides(g, g->cur.type, g->cur.rot, g->cur.x, g->cur.y + 1);
}

static void spawn(struct game *g, int type)
{
    g->cur = (struct piece){ type, 0, type == T_O ? 4 : 3, 1 };
    g->have_cur = true;
    g->fall_ms = g->lock_ms = g->lock_resets = 0;
    if (collides(g, type, 0, g->cur.x, g->cur.y)) {         /* no room for it: the end */
        g->over = true;
        g->ev |= EV_OVER;
    }
}

/* `first` >= 0 names the first piece (the tests); the rest of its bag follows */
static void game_new(struct game *g, uint32_t seed, int level, int first)
{
    shapes_init();
    memset(g, 0, sizeof(*g));
    g->seed = seed;
    g->rng = seed * 2654435761u + 12345u;
    for (int i = 0; i < 8; i++)
        rng_next(&g->rng);                                  /* (a few turns so that near seeds do not start alike) */
    g->hold = -1;
    g->combo = -1;
    g->start_level = g->level = LM_CLAMP(level, 1, 15);
    bag_refill(g);
    if (first >= 0 && first < T_N)
        for (int i = 0; i < T_N; i++)
            if (g->q[i] == first) {
                g->q[i] = g->q[0];
                g->q[0] = (uint8_t)first;
                break;
            }
    spawn(g, take_next(g));
}

static int gravity_ms(int level)                            /* the time a piece takes for a row: the guideline's (0.8 - (level - 1) * 0.007) ^ (level - 1) seconds */
{
    double t = pow(0.8 - (level - 1) * 0.007, level - 1) * 1000.0;
    return t < 1 ? 1 : (int)t;
}

static int soft_ms(int level)
{
    int iv = gravity_ms(level);
    return LM_MIN(iv, LM_MAX(iv / 20, 1));
}

static int ghost_y(const struct game *g)
{
    int y = g->cur.y;
    while (!collides(g, g->cur.type, g->cur.rot, g->cur.x, y + 1))
        y++;
    return y;
}

static void score_clear(struct game *g, int n)
{
    static const int base[5] = { 0, 100, 300, 500, 800 };
    int gain = base[n] * g->level;
    if (n == 4) {
        if (g->b2b)
            gain = gain * 3 / 2;
        g->b2b = true;
        g->ev |= EV_TETRIS;
    } else if (n > 0) {
        g->b2b = false;
    }
    g->combo++;
    if (g->combo > 0)
        gain += 50 * g->combo * g->level;
    g->score += gain;
    g->last_gain = gain;
    g->last_clear = n;
    g->lines += n;
    int lv = LM_MIN(g->start_level + g->lines / 10, 30);
    if (lv > g->level) {
        g->level = lv;
        g->ev |= EV_LEVEL;
    }
}

static void lock_piece(struct game *g)
{
    bool above = true;                                      /* a piece that locks wholly in the hidden rows is a lock out */
    for (int c = 0; c < 4; c++) {
        int cx = g->cur.x + g_shape[g->cur.type][g->cur.rot][c][0], cy = g->cur.y + g_shape[g->cur.type][g->cur.rot][c][1];
        g->cell[cy][cx] = (uint8_t)(g->cur.type + 1);
        if (cy >= HID)
            above = false;
    }
    g->pieces++;
    g->hold_used = false;
    g->have_cur = false;
    g->fall_ms = g->lock_ms = 0;
    g->ev |= EV_LOCK;
    uint32_t mask = 0;
    int n = 0;
    for (int y = 0; y < BH; y++) {
        bool full = true;
        for (int x = 0; x < BW; x++)
            full = full && g->cell[y][x];
        if (full) {
            mask |= 1u << y;
            n++;
        }
    }
    if (above) {
        g->over = true;
        g->ev |= EV_OVER;
        return;
    }
    if (n) {
        g->clear_mask = mask;
        g->ev |= EV_CLEAR;
        score_clear(g, n);
        return;
    }
    g->combo = -1;
    g->last_clear = 0;
    g->last_gain = 0;
    spawn(g, take_next(g));
}

/* The full rows go and the next piece comes (the window waits a moment between, to show them). */
static void commit_clear(struct game *g)
{
    if (!g->clear_mask)
        return;
    int to = BH - 1;
    for (int y = BH - 1; y >= 0; y--) {
        if (g->clear_mask & (1u << y))
            continue;
        if (to != y)
            memcpy(g->cell[to], g->cell[y], BW);
        to--;
    }
    while (to >= 0)
        memset(g->cell[to--], 0, BW);
    g->clear_mask = 0;
    spawn(g, take_next(g));
}

static void lock_reset(struct game *g, bool was_grounded)
{
    if ((was_grounded || grounded(g)) && g->lock_resets < LOCK_RESETS) {
        g->lock_ms = 0;
        g->lock_resets++;
    }
}

static bool game_move(struct game *g, int dx)
{
    if (g->over || !g->have_cur || g->clear_mask || collides(g, g->cur.type, g->cur.rot, g->cur.x + dx, g->cur.y))
        return false;
    bool was = grounded(g);
    g->cur.x += dx;
    lock_reset(g, was);
    return true;
}

static bool game_rotate(struct game *g, int dir)            /* dir 1 = clockwise, -1 = anticlockwise */
{
    if (g->over || !g->have_cur || g->clear_mask)
        return false;
    int from = g->cur.rot, to = (from + dir + 4) % 4;
    int pair = dir > 0 ? from * 2 : to * 2 + 1;
    int tries = g->cur.type == T_O ? 1 : 5;
    for (int k = 0; k < tries; k++) {
        const int8_t *off = g->cur.type == T_I ? k_kick_i[pair][k] : k_kick[pair][k];
        int nx = g->cur.x + off[0], ny = g->cur.y - off[1];
        if (!collides(g, g->cur.type, to, nx, ny)) {
            bool was = grounded(g);
            g->cur.rot = to;
            g->cur.x = nx;
            g->cur.y = ny;
            lock_reset(g, was);
            return true;
        }
    }
    return false;
}

static bool game_soft(struct game *g)                       /* one row down by hand: a point */
{
    if (g->over || !g->have_cur || g->clear_mask || collides(g, g->cur.type, g->cur.rot, g->cur.x, g->cur.y + 1))
        return false;
    g->cur.y++;
    g->score++;
    g->fall_ms = 0;
    g->lock_resets = 0;
    g->lock_ms = 0;
    return true;
}

static int game_hard(struct game *g)                        /* all the way down and locked at once: two points a row */
{
    if (g->over || !g->have_cur || g->clear_mask)
        return -1;
    int n = 0;
    while (!collides(g, g->cur.type, g->cur.rot, g->cur.x, g->cur.y + 1)) {
        g->cur.y++;
        n++;
    }
    g->score += 2 * n;
    lock_piece(g);
    return n;
}

static bool game_hold(struct game *g)
{
    if (g->over || !g->have_cur || g->clear_mask || g->hold_used)
        return false;
    int t = g->cur.type, take;
    if (g->hold < 0)
        take = take_next(g);
    else
        take = g->hold;
    g->hold = t;
    spawn(g, take);
    g->hold_used = true;
    return true;
}

/* `dt` milliseconds go by: the piece falls (faster with `soft`), or, standing on something, waits to lock. Returns true when something changed. */
static bool game_tick(struct game *g, int dt, bool soft)
{
    if (g->over || !g->have_cur || g->clear_mask)
        return false;
    if (grounded(g)) {
        g->lock_ms += dt;
        if (g->lock_ms >= LOCK_MS) {
            lock_piece(g);
            return true;
        }
        return false;
    }
    int iv = soft ? soft_ms(g->level) : gravity_ms(g->level);
    bool changed = false;
    g->fall_ms += dt;
    for (int n = 0; g->fall_ms >= iv && n < 24; n++) {
        g->fall_ms -= iv;
        if (collides(g, g->cur.type, g->cur.rot, g->cur.x, g->cur.y + 1))
            break;
        g->cur.y++;
        g->lock_resets = 0;
        if (soft)
            g->score++;
        changed = true;
        if (grounded(g)) {
            g->fall_ms = g->lock_ms = 0;
            break;
        }
    }
    return changed;
}

/* the milliseconds until game_tick has something to do (for the loop's timeout) */
static int game_next_ms(const struct game *g, bool soft)
{
    if (g->over || !g->have_cur || g->clear_mask)
        return -1;
    int left = grounded(g) ? LOCK_MS - g->lock_ms : (soft ? soft_ms(g->level) : gravity_ms(g->level)) - g->fall_ms;
    return left < 1 ? 1 : left;
}

/* ================================================================ the window ================================================================ */

#define STAT_H 24
#define CLEAR_MS 240                                        /* the full rows flash this long before they go */
#define DAS_MS 170                                          /* a held Left / Right moves again after this, */
#define ARR_MS 45                                           /* and then every this */

enum prompt { P_NONE, P_NEW, P_OVER, P_HELP };
enum { B_NEW, B_PAUSE, B_GHOST, B_LEVEL, B_HELP, B_COUNT };

struct stats {
    int played, best, lines;                                /* the best score and the most lines of one game */
    int level;                                              /* the level a new game starts at */
    int ghost;
};

struct app {
    struct wlwin win;
    struct game g;
    double px, py;
    bool has_pointer;
    int hover;                                              /* the rail item under the pointer */
    struct wlrail_anim ran;
    enum prompt prompt;
    int pc_hover, pc_down;
    bool paused, still, counted, banked;
    bool left_down, right_down, soft_down;
    int das_dir;                                            /* the way a held key repeats: -1, 0, 1 */
    int64_t das_next, last_ms, clear_at;
    char msg[160];
    int64_t msg_ms;
    struct stats st;
    uint32_t settings;                                      /* mellagui_settings_v1: the compositor plays the sounds (`card-sound`) */
    int64_t last_sound_ms;
};

static const char *g_dump;

static int64_t now_ms(void) { return wlrail_now(); }

#define COL_BAR (lm_theme.bar)
#define COL_TEXT (lm_theme.text_soft)
#define COL_DIM (lm_theme.text_dim)
#define COL_ACCENT (lm_theme.accent)

/* the board and the pieces are literals on purpose: a dark well and the guideline's colours whatever the theme */
#define WELL 0x0f131cu
#define WELL_LINE 0x1a2030u
#define WELL_EDGE 0x2c3752u

static const uint32_t k_piece_rgb[T_N] = { 0x1ec8e8u, 0x3361d8u, 0xf28c1eu, 0xf2d21cu, 0x3fbd4cu, 0xa44fd3u, 0xe23e3eu };

static void say(struct app *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->msg, sizeof(a->msg), fmt, ap);
    va_end(ap);
    a->msg_ms = now_ms();
    a->win.redraw = true;
}

/* The compositor plays them (write-only setting card-sound: draw place shuffle win no), so they follow Settings > Sound > Sound feedback and the chosen output.
 * The card game's sounds are borrowed: place = a piece locked, draw = lines went, win = four lines or a new level, no = the end. */
static void sound(struct app *a, const char *name)
{
    int64_t now = now_ms();
    if (!a->settings || !a->win.c || (now - a->last_sound_ms < 90 && strcmp(name, "win") && strcmp(name, "no")))
        return;
    a->last_sound_ms = now;
    WLC_SEND(a->win.c, a->settings, LM_MELLAGUI_SETTINGS_V1_REQ_SET, { .s = "card-sound" }, { .s = name });
    wlc_flush(a->win.c);
}

/* ---- the file with the best score ---- */

static void stats_path(char *out, size_t n)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x)
        snprintf(out, n, "%s/mellagui/tetris.conf", x);
    else
        snprintf(out, n, "%s/.config/mellagui/tetris.conf", h && *h ? h : ".");
}

static void stats_load(struct stats *s)
{
    memset(s, 0, sizeof(*s));
    s->level = 1;
    s->ghost = 1;
    char path[PATH_MAX];
    stats_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "played = %d", &v) == 1) s->played = v;
        else if (sscanf(line, "best = %d", &v) == 1) s->best = v;
        else if (sscanf(line, "lines = %d", &v) == 1) s->lines = v;
        else if (sscanf(line, "level = %d", &v) == 1) s->level = LM_CLAMP(v, 1, 15);
        else if (sscanf(line, "ghost = %d", &v) == 1) s->ghost = v != 0;
    }
    fclose(f);
}

static void stats_save(const struct stats *s)
{
    char path[PATH_MAX], dir[PATH_MAX];
    stats_path(path, sizeof(path));
    snprintf(dir, sizeof(dir), "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl) {
        *sl = 0;
        for (char *p = dir + 1; *p; p++) {
            if (*p == '/') {
                *p = 0;
                mkdir(dir, 0700);
                *p = '/';
            }
        }
        mkdir(dir, 0700);
    }
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "played = %d\nbest = %d\nlines = %d\nlevel = %d\nghost = %d\n", s->played, s->best, s->lines, s->level, s->ghost);
    fclose(f);
}

/* the game's score goes into the best ones (when it is better); true when it is a new best score */
static bool bank(struct app *a)
{
    if (a->banked || a->g.pieces == 0)
        return false;
    a->banked = true;
    bool best = a->g.score > a->st.best;
    if (best)
        a->st.best = a->g.score;
    if (a->g.lines > a->st.lines)
        a->st.lines = a->g.lines;
    stats_save(&a->st);
    return best;
}

static bool running(const struct app *a) { return !a->paused && a->prompt == P_NONE && !a->g.over; }

static void keys_release(struct app *a)
{
    a->left_down = a->right_down = a->soft_down = false;
    a->das_dir = 0;
}

static void new_game(struct app *a, uint32_t seed, int level, int first)
{
    bank(a);
    game_new(&a->g, seed, level, first);
    a->paused = a->counted = a->banked = false;
    a->prompt = P_NONE;
    a->msg[0] = 0;
    a->clear_at = 0;
    a->last_ms = now_ms();
    keys_release(a);
    a->win.redraw = true;
}

static uint32_t fresh_seed(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint32_t)(ts.tv_nsec ^ (uint32_t)ts.tv_sec * 2654435761u ^ (uint32_t)getpid() << 7) % 1000000u + 1u;
}

/* What the rules did since the window last looked: the sounds, the message, the pause for the full rows, the end of the game. */
static void events(struct app *a)
{
    struct game *g = &a->g;
    int ev = g->ev;
    if (!ev)
        return;
    g->ev = 0;
    a->win.redraw = true;
    if ((ev & EV_LOCK) && !a->counted) {
        a->counted = true;
        a->st.played++;
        stats_save(&a->st);
    }
    if (ev & EV_CLEAR) {
        a->clear_at = now_ms() + CLEAR_MS;
        if (g->last_clear == 4)
            say(a, "Tetris!  +%d", g->last_gain);
        else if (g->combo > 0)
            say(a, "%d lines  combo x%d  +%d", g->last_clear, g->combo + 1, g->last_gain);
        else
            say(a, "%d line%s  +%d", g->last_clear, g->last_clear == 1 ? "" : "s", g->last_gain);
    }
    if (ev & EV_LEVEL)
        say(a, "Level %d", g->level);
    if (ev & EV_OVER) {
        bool best = bank(a);
        a->prompt = P_OVER;
        a->pc_hover = a->pc_down = -1;
        keys_release(a);
        if (best)
            say(a, "A new best score!");
        sound(a, "no");
        return;
    }
    if (ev & (EV_TETRIS | EV_LEVEL))
        sound(a, "win");
    else if (ev & EV_CLEAR)
        sound(a, "draw");
    else if (ev & EV_LOCK)
        sound(a, "place");
}

static int elapsed_s(const struct app *a) { return (int)(a->g.play_ms / 1000); }

/* The time goes on: the held keys repeat, the full rows go, the piece falls. */
static void advance(struct app *a, int64_t now)
{
    int64_t dt = now - a->last_ms;
    a->last_ms = now;
    if (!running(a))
        return;
    dt = LM_CLAMP(dt, 0, 100);
    struct game *g = &a->g;
    g->play_ms += dt;
    if (g->clear_mask) {
        if (now < a->clear_at) {
            a->win.redraw = true;                           /* the rows flash */
            return;
        }
        commit_clear(g);
        a->win.redraw = true;
        events(a);
        return;
    }
    for (int n = 0; a->das_dir && now >= a->das_next && n < 12; n++) {
        if (game_move(g, a->das_dir))
            a->win.redraw = true;
        a->das_next += ARR_MS;
    }
    if (!a->still && game_tick(g, (int)dt, a->soft_down))
        a->win.redraw = true;
    events(a);
}

/* ---- the geometry ---- */

struct lay {
    int cs;                                                 /* the edge of a block */
    int hx, bx, nx, by;                                     /* the hold panel, the board, the next panel (left edges), the board's top */
    int pw;                                                 /* the panels' width */
};

static void layout(const struct app *a, struct lay *L)
{
    int W = a->win.bw - WLRAIL_W, H = a->win.bh - STAT_H;
    int cs = LM_MAX(10, LM_MIN((H - 16) / VIS, (W - 16) / 22));
    L->cs = cs;
    L->pw = 5 * cs;
    int total = L->pw + cs / 2 + BW * cs + cs / 2 + L->pw;
    int x0 = WLRAIL_W + (W - total) / 2;
    L->hx = x0;
    L->bx = x0 + L->pw + cs / 2;
    L->nx = L->bx + BW * cs + cs / 2;
    L->by = (H - VIS * cs) / 2;
}

static void draw_block(struct wlwin *w, int x, int y, int cs, uint32_t rgb)
{
    int t = LM_MAX(2, cs / 8);
    wlwin_fill(w, x, y, cs, cs, lm_mix_rgb(rgb, 0x000000, 0.45));
    wlwin_fill(w, x + 1, y + 1, cs - 2, cs - 2, rgb);
    wlwin_fill(w, x + 1, y + 1, cs - 2, t, lm_mix_rgb(rgb, 0xffffff, 0.45));                   /* the light comes from the upper left */
    wlwin_fill(w, x + 1, y + 1 + t, t, cs - 2 - t, lm_mix_rgb(rgb, 0xffffff, 0.22));
    wlwin_fill(w, x + 1 + t, y + cs - 1 - t, cs - 2 - t, t, lm_mix_rgb(rgb, 0x000000, 0.28));
    wlwin_fill(w, x + cs - 1 - t, y + 1 + t, t, cs - 2 - t, lm_mix_rgb(rgb, 0x000000, 0.18));
}

static void draw_ghost_block(struct wlwin *w, int x, int y, int cs, uint32_t rgb)
{
    uint32_t c = lm_mix_rgb(rgb, WELL, 0.45);
    int t = 2;
    wlwin_fill(w, x + 1, y + 1, cs - 2, t, c);
    wlwin_fill(w, x + 1, y + cs - 1 - t, cs - 2, t, c);
    wlwin_fill(w, x + 1, y + 1 + t, t, cs - 2 - 2 * t, c);
    wlwin_fill(w, x + cs - 1 - t, y + 1 + t, t, cs - 2 - 2 * t, c);
}

/* a piece in its spawn pose centred on (cx, cy): the hold and next panels */
static void draw_preview(struct wlwin *w, int type, int cx, int cy, int cs, bool dim)
{
    int x0 = 99, x1 = -99, y0 = 99, y1 = -99;
    for (int c = 0; c < 4; c++) {
        x0 = LM_MIN(x0, g_shape[type][0][c][0]);
        x1 = LM_MAX(x1, g_shape[type][0][c][0]);
        y0 = LM_MIN(y0, g_shape[type][0][c][1]);
        y1 = LM_MAX(y1, g_shape[type][0][c][1]);
    }
    int ox = cx - (x1 - x0 + 1) * cs / 2 - x0 * cs, oy = cy - (y1 - y0 + 1) * cs / 2 - y0 * cs;
    uint32_t rgb = dim ? lm_mix_rgb(k_piece_rgb[type], 0x707680, 0.65) : k_piece_rgb[type];
    for (int c = 0; c < 4; c++)
        draw_block(w, ox + g_shape[type][0][c][0] * cs, oy + g_shape[type][0][c][1] * cs, cs, rgb);
}

static void fmt_time(int s, char *out, size_t n)
{
    snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static void label(struct wlwin *w, int x, int y, const char *s, uint32_t rgb)
{
    wlwin_text_bold(w, x, y, s, 1, rgb, 0);
}

static void paint_board(struct app *a, const struct lay *L)
{
    struct wlwin *w = &a->win;
    const struct game *g = &a->g;
    int cs = L->cs, bx = L->bx, by = L->by;
    wlwin_fill(w, bx - 4, by - 4, BW * cs + 8, VIS * cs + 8, WELL_EDGE);
    wlwin_fill(w, bx, by, BW * cs, VIS * cs, WELL);
    for (int x = 1; x < BW; x++)
        wlwin_fill(w, bx + x * cs, by, 1, VIS * cs, WELL_LINE);
    for (int y = 1; y < VIS; y++)
        wlwin_fill(w, bx, by + y * cs, BW * cs, 1, WELL_LINE);
    if (a->paused) {                                        /* the board is hidden while the game waits */
        const char *t = "Paused", *h = "P continues";
        wlwin_text_bold(w, bx + (BW * cs - lm_font_text_width_bold(t) * 2) / 2, by + VIS * cs / 2 - 24, t, 2, COL_TEXT, 0);
        wlwin_text(w, bx + (BW * cs - lm_font_text_width(h)) / 2, by + VIS * cs / 2 + 14, h, 1, COL_DIM, 0);
        return;
    }
    bool flash = g->clear_mask && ((now_ms() - (a->clear_at - CLEAR_MS)) / 55) % 2 == 0;
    for (int y = HID; y < BH; y++)
        for (int x = 0; x < BW; x++) {
            if (!g->cell[y][x])
                continue;
            if (flash && (g->clear_mask & (1u << y)))
                draw_block(w, bx + x * cs, by + (y - HID) * cs, cs, 0xffffffu);
            else
                draw_block(w, bx + x * cs, by + (y - HID) * cs, cs, k_piece_rgb[g->cell[y][x] - 1]);
        }
    if (!g->have_cur)
        return;
    if (a->st.ghost && !g->over) {
        int gy = ghost_y(g);
        for (int c = 0; c < 4; c++) {
            int cx = g->cur.x + g_shape[g->cur.type][g->cur.rot][c][0], cy = gy + g_shape[g->cur.type][g->cur.rot][c][1];
            if (cy >= HID)
                draw_ghost_block(w, bx + cx * cs, by + (cy - HID) * cs, cs, k_piece_rgb[g->cur.type]);
        }
    }
    for (int c = 0; c < 4; c++) {
        int cx = g->cur.x + g_shape[g->cur.type][g->cur.rot][c][0], cy = g->cur.y + g_shape[g->cur.type][g->cur.rot][c][1];
        if (cy >= HID)
            draw_block(w, bx + cx * cs, by + (cy - HID) * cs, cs, k_piece_rgb[g->cur.type]);
    }
}

static void stat_line(struct wlwin *w, int x, int y, int pw, const char *name, const char *value, bool big)
{
    wlwin_text(w, x, y, name, 1, COL_DIM, pw);
    int scale = big && lm_font_text_width(value) * 2 <= pw ? 2 : 1;
    wlwin_text_bold(w, x, y + LM_FONT_H + 2, value, scale, lm_theme.text_bright, pw);
}

static void paint_panels(struct app *a, const struct lay *L)
{
    struct wlwin *w = &a->win;
    const struct game *g = &a->g;
    int cs = L->cs, pw = L->pw, by = L->by;
    int boxh = 3 * cs + cs / 2;
    /* hold */
    label(w, L->hx, by, "HOLD", COL_ACCENT);
    wlwin_round_fill(w, L->hx, by + 22, pw, boxh, 10, COL_BAR);
    if (g->hold >= 0 && !a->paused)
        draw_preview(w, g->hold, L->hx + pw / 2, by + 22 + boxh / 2, cs * 4 / 5, g->hold_used);
    /* the numbers under it */
    char v[32];
    int y = by + 22 + boxh + cs;
    int step = 2 * LM_FONT_H + cs / 2 + 6;
    snprintf(v, sizeof(v), "%d", g->score);
    stat_line(w, L->hx, y, pw, "SCORE", v, true);
    snprintf(v, sizeof(v), "%d", g->lines);
    stat_line(w, L->hx, y + step, pw, "LINES", v, false);
    snprintf(v, sizeof(v), "%d", g->level);
    stat_line(w, L->hx, y + 2 * step, pw, "LEVEL", v, false);
    /* next */
    label(w, L->nx, by, "NEXT", COL_ACCENT);
    int ph = cs * 4 / 5, slot = ph * 3;
    wlwin_round_fill(w, L->nx, by + 22, pw, slot * 3 + 8, 10, COL_BAR);
    if (!a->paused)
        for (int i = 0; i < 3 && i < g->nq; i++)
            draw_preview(w, g->q[i], L->nx + pw / 2, by + 22 + 4 + slot * i + slot / 2, ph, false);
    int y2 = by + 22 + slot * 3 + 8 + cs;
    fmt_time(elapsed_s(a), v, sizeof(v));
    stat_line(w, L->nx, y2, pw, "TIME", v, false);
    snprintf(v, sizeof(v), "%d", LM_MAX(a->st.best, g->score));
    stat_line(w, L->nx, y2 + step, pw, "BEST", v, false);
}

static void paint_status(struct app *a)
{
    struct wlwin *w = &a->win;
    int y = w->bh - STAT_H;
    wlwin_fill(w, 0, y, w->bw, STAT_H, COL_BAR);
    char line[200];
    snprintf(line, sizeof(line), "Pieces %d   Next level in %d lines   Game %u", a->g.pieces, 10 - a->g.lines % 10, a->g.seed);
    wlwin_text(w, WLRAIL_W + 12, y + (STAT_H - LM_FONT_H) / 2, line, 1, COL_TEXT, w->bw - WLRAIL_W - 24);
    char right[100];
    snprintf(right, sizeof(right), "Games %d   Most lines %d", a->st.played, a->st.lines);
    int rw = lm_font_text_width(right);
    if (a->msg[0] && now_ms() - a->msg_ms < 6000)
        wlwin_text_bold(w, WLRAIL_W + 12, y - 24, a->msg, 1, 0xfff3bfu, w->bw - WLRAIL_W - 24);
    else if (w->bw - WLRAIL_W - 24 > rw + lm_font_text_width(line) + 20)
        wlwin_text(w, w->bw - rw - 12, y + (STAT_H - LM_FONT_H) / 2, right, 1, COL_DIM, rw);
}

/* ---- the rail ---- */

static void ic_new(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    wlrail_ic_new(w, cx, cy, c, bg);
}

static void ic_pause(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_fill(w, cx - 5, cy - 6, 4, 12, c);
    wlwin_fill(w, cx + 2, cy - 6, 4, 12, c);
}

static void ic_ghost(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlrail_box(w, cx - 6, cy - 6, 12, 12, c);               /* a piece that is only an outline */
    wlwin_fill(w, cx - 2, cy - 2, 5, 5, c);
}

static void rails(const struct app *a, struct wlrail *L)
{
    static const char *const tips[B_COUNT] = { "New game  N", "Pause  P", "Ghost piece  G", "Starting level", "Keys  F1" };
    static void (*const icons[B_COUNT])(struct wlwin *, int, int, uint32_t, uint32_t) = { ic_new, ic_pause, ic_ghost, NULL, NULL };
    const uint32_t base[B_COUNT] = { lm_theme.btn_green, lm_theme.btn_amber, lm_theme.btn_slate, lm_theme.btn_purple, lm_theme.btn_rose };
    const uint32_t lite[B_COUNT] = { lm_theme.btn_green_lit, lm_theme.btn_amber_lit, lm_theme.btn_slate_lit, lm_theme.btn_purple_lit, lm_theme.btn_rose_lit };
    static char lv[4];
    snprintf(lv, sizeof(lv), "%d", a->st.level);
    memset(L, 0, sizeof(*L));
    L->x = 0;
    L->y0 = 0;
    L->y1 = a->win.bh > 0 ? a->win.bh - STAT_H : 400;
    L->n = B_COUNT;
    for (int i = 0; i < B_COUNT; i++) {
        bool idle = i == B_PAUSE && a->g.over;
        L->it[i] = (struct wlrail_item){ .tip = tips[i], .icon = icons[i], .text = i == B_LEVEL ? lv : i == B_HELP ? "?" : NULL,
                                         .base = base[i], .lite = lite[i], .show = true, .idle = idle,
                                         .lit = (i == B_PAUSE && a->paused) || (i == B_GHOST && a->st.ghost), .gap = i == B_PAUSE || i == B_LEVEL, .bottom = i == B_HELP };
    }
}

static void press_button(struct app *a, int i);

/* ---- the cards of the questions ---- */

static void card_rect(const struct app *a, struct wlcard *c, int *nb)
{
    int ch = a->prompt == P_HELP ? 300 : a->prompt == P_OVER ? 170 : 130;
    *nb = a->prompt == P_HELP ? 1 : 2;
    c->w = LM_MIN(a->prompt == P_HELP ? 540 : 440, a->win.bw - 16);
    c->h = LM_MIN(ch, a->win.bh - 16);
    wlcard_center(&a->win, c);
}

static const char *const HELP_LINES[] = {
    "Left and Right move the piece (hold the key to keep going),",
    "Down drops it softly, Space drops it all the way.",
    "Up or X turn it clockwise, Z anticlockwise.",
    "C or Shift keeps a piece for later (one swap for each piece).",
    "A full row goes; four at once (a Tetris) is worth the most.",
    "The game speeds up every ten lines.",
    "P or Esc pauses, G shows or hides the ghost where the piece lands,",
    "N a new game. The game also pauses when the window loses the keyboard.",
};

static void paint_card(struct app *a)
{
    struct wlwin *w = &a->win;
    struct wlcard c;
    int nb;
    card_rect(a, &c, &nb);
    const char *title = a->prompt == P_NEW ? "New game" : a->prompt == P_OVER ? "Game over" : "Keys";
    wlcard_frame(w, &c, c.w, c.h, title, COL_ACCENT, lm_theme.background, lm_theme.text_bright);
    char line[160] = "", line2[120] = "", line3[120] = "";
    const char *labels[2] = { "Cancel", "OK" };
    uint32_t cols[2] = { lm_theme.btn_grey, lm_theme.btn_blue };
    if (a->prompt == P_NEW) {
        snprintf(line, sizeof(line), "Give this game up and start a new one?");
        labels[1] = "New game";
        cols[1] = lm_theme.btn_green;
    } else if (a->prompt == P_OVER) {
        char t[16];
        fmt_time(elapsed_s(a), t, sizeof(t));
        snprintf(line, sizeof(line), "Score %d.", a->g.score);
        snprintf(line2, sizeof(line2), "%d lines, level %d, %s.", a->g.lines, a->g.level, t);
        snprintf(line3, sizeof(line3), a->g.score >= a->st.best && a->g.score > 0 ? "A new best score!" : "Best score %d.", a->st.best);
        labels[0] = "Close";
        labels[1] = "New game";
        cols[1] = lm_theme.btn_green;
    } else {
        labels[0] = "Close";
        cols[0] = lm_theme.btn_blue;
        for (size_t i = 0; i < sizeof(HELP_LINES) / sizeof(HELP_LINES[0]); i++)
            wlwin_text(w, c.x + WLCARD_PAD, c.y + 40 + (int)i * (LM_FONT_H + 4), HELP_LINES[i], 1, lm_theme.text, c.w - 2 * WLCARD_PAD);
    }
    if (line[0])
        wlwin_text(w, c.x + WLCARD_PAD, c.y + 46, line, 1, lm_theme.text, c.w - 2 * WLCARD_PAD);
    if (line2[0])
        wlwin_text(w, c.x + WLCARD_PAD, c.y + 46 + LM_FONT_H + 6, line2, 1, lm_theme.text_dim, c.w - 2 * WLCARD_PAD);
    if (line3[0])
        wlwin_text(w, c.x + WLCARD_PAD, c.y + 46 + 2 * (LM_FONT_H + 6), line3, 1, lm_theme.text_dim, c.w - 2 * WLCARD_PAD);
    wlcard_buttons(w, &c, labels, nb, cols, a->pc_hover, a->pc_down, lm_theme.on_colour);
}

static void card_accept(struct app *a, int which)
{
    if (a->prompt == P_HELP || which == 0) {
        a->prompt = P_NONE;
        return;
    }
    new_game(a, fresh_seed(), a->st.level, -1);
}

/* ---- paint and the dump ---- */

static void write_dump(struct app *a)
{
    if (!g_dump)
        return;
    FILE *f = fopen(g_dump, "w");
    if (!f)
        return;
    const struct game *g = &a->g;
    fprintf(f, "state score=%d lines=%d level=%d pieces=%d over=%d paused=%d prompt=%d hold=%c cur=%c rot=%d x=%d y=%d ghosty=%d clearing=%d time=%d seed=%u "
               "played=%d best=%d bestlines=%d ghost=%d startlevel=%d combo=%d b2b=%d\n",
            g->score, g->lines, g->level, g->pieces, g->over, a->paused, (int)a->prompt, g->hold >= 0 ? T_NAME[g->hold] : '-',
            g->have_cur ? T_NAME[g->cur.type] : '-', g->cur.rot, g->cur.x, g->cur.y, g->have_cur ? ghost_y(g) : -1, g->clear_mask != 0, elapsed_s(a), g->seed,
            a->st.played, a->st.best, a->st.lines, a->st.ghost, a->st.level, g->combo, g->b2b);
    fprintf(f, "next");
    for (int i = 0; i < 3 && i < g->nq; i++)
        fprintf(f, " %c", T_NAME[g->q[i]]);
    fprintf(f, "\n");
    struct lay L;
    layout(a, &L);
    fprintf(f, "lay cs=%d bx=%d by=%d w=%d h=%d\n", L.cs, L.bx, L.by, BW * L.cs, VIS * L.cs);
    for (int y = HID; y < BH; y++) {
        char row[BW + 1];
        for (int x = 0; x < BW; x++)
            row[x] = g->cell[y][x] ? T_NAME[g->cell[y][x] - 1] : '.';
        row[BW] = 0;
        fprintf(f, "row %d %s\n", y - HID, row);
    }
    if (a->prompt != P_NONE) {
        struct wlcard c;
        int nb;
        card_rect(a, &c, &nb);
        for (int k = 0; k < nb; k++) {
            int x, y, bw, bh;
            wlcard_button_rect(&c, k, nb, &x, &y, &bw, &bh);
            fprintf(f, "cbtn %d %d %d %d %d\n", k, x, y, bw, bh);
        }
    }
    struct wlrail R;
    rails(a, &R);
    int ys[WLRAIL_MAX];
    wlrail_layout(&R, ys);
    static const char *const names[B_COUNT] = { "New", "Pause", "Ghost", "Level", "Help" };
    for (int i = 0; i < R.n; i++)
        if (ys[i] >= 0)
            fprintf(f, "btn %s x=%d y=%d w=%d h=%d\n", names[i], R.x + (WLRAIL_W - WLRAIL_D) / 2, ys[i], WLRAIL_D, WLRAIL_D);
    fclose(f);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    wlwin_fill(w, 0, 0, w->bw, w->bh, lm_theme.background);
    struct lay L;
    layout(a, &L);
    paint_panels(a, &L);
    paint_board(a, &L);
    paint_status(a);
    struct wlrail R;
    rails(a, &R);
    wlrail_paint(w, &R, &a->ran, a->hover, now_ms(), COL_BAR, COL_BAR, COL_ACCENT, lm_theme.text);
    if (a->prompt != P_NONE)
        paint_card(a);
    write_dump(a);
}

/* ---- what the buttons, the pointer and the keys do ---- */

static void press_button(struct app *a, int i)
{
    switch (i) {
    case B_NEW:
        if (a->g.pieces > 0 && !a->g.over) {
            a->prompt = P_NEW;
            a->pc_hover = a->pc_down = -1;
        } else
            new_game(a, fresh_seed(), a->st.level, -1);
        break;
    case B_PAUSE:
        if (!a->g.over && a->prompt == P_NONE) {
            a->paused = !a->paused;
            keys_release(a);
        }
        break;
    case B_GHOST:
        a->st.ghost = !a->st.ghost;
        stats_save(&a->st);
        break;
    case B_LEVEL: {
        int lv = a->st.level % 15 + 1;
        a->st.level = lv;
        stats_save(&a->st);
        if (a->g.pieces == 0 || a->g.over)
            new_game(a, fresh_seed(), lv, -1);
        else {
            a->prompt = P_NEW;
            a->pc_hover = a->pc_down = -1;
            say(a, "Level %d starts with the next new game", lv);
        }
        break;
    }
    case B_HELP: a->prompt = P_HELP; a->pc_hover = a->pc_down = -1; break;
    default: break;
    }
    a->win.redraw = true;
}

static void card_pointer(struct app *a, uint32_t op, const union lm_arg *args)
{
    struct wlcard c;
    int nb;
    card_rect(a, &c, &nb);
    if (op == LM_WL_POINTER_EVT_MOTION) {
        if (wlcard_drag_motion(&a->win, &c, a->px, a->py))              /* the card is held by its title */
            return;
        a->pc_hover = wlcard_button_at(&c, nb, (int)a->px, (int)a->py);
        a->win.redraw = true;
        return;
    }
    if (op != LM_WL_POINTER_EVT_BUTTON || args[2].u != 272)
        return;
    int k = wlcard_button_at(&c, nb, (int)a->px, (int)a->py);
    if (args[3].u == 1) {
        if (k >= 0)
            a->pc_down = k;
        else if (wlcard_drag_press(&a->win, &c, a->px, a->py))          /* the title: take hold of the card */
            ;
        else if (!wlcard_has(&c, (int)a->px, (int)a->py) && a->prompt != P_OVER)
            a->prompt = P_NONE;
    } else {
        wlcard_drag_release(&a->win);
        if (a->pc_down >= 0 && a->pc_down == k)
            card_accept(a, k);
        a->pc_down = -1;
    }
    a->win.redraw = true;
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        a->has_pointer = true;
        w->redraw = true;
        return;
    case LM_WL_POINTER_EVT_LEAVE:
        a->has_pointer = false;
        a->hover = -1;
        wlrail_release(&a->ran);
        w->redraw = true;
        return;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        a->has_pointer = true;
        break;
    default: break;
    }
    if (a->prompt != P_NONE) {
        card_pointer(a, op, args);
        return;
    }
    struct wlrail L;
    rails(a, &L);
    if (op == LM_WL_POINTER_EVT_MOTION) {
        a->hover = wlrail_at(&L, a->px, a->py);
        w->redraw = true;
        return;
    }
    if (op != LM_WL_POINTER_EVT_BUTTON)
        return;
    uint32_t button = args[2].u, state = args[3].u;
    if (state != 1) {
        wlrail_release(&a->ran);
        w->redraw = true;
        return;
    }
    int bi = wlrail_at(&L, a->px, a->py);
    if (button == 272 && bi >= 0) {
        wlrail_press(&a->ran, bi, now_ms());
        if (!L.it[bi].idle)
            press_button(a, bi);
    }
}

/* Left / Right start to repeat after DAS_MS and then go every ARR_MS; the key pressed last wins while both are down. */
static void das_press(struct app *a, int dir)
{
    if (dir < 0)
        a->left_down = true;
    else
        a->right_down = true;
    a->das_dir = dir;
    a->das_next = now_ms() + DAS_MS;
    if (game_move(&a->g, dir))
        a->win.redraw = true;
}

static void das_release(struct app *a, int dir)
{
    if (dir < 0)
        a->left_down = false;
    else
        a->right_down = false;
    if (a->das_dir == dir) {
        a->das_dir = a->left_down ? -1 : a->right_down ? 1 : 0;
        a->das_next = now_ms() + DAS_MS;
    }
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op == LM_WL_KEYBOARD_EVT_LEAVE) {                   /* the keyboard went elsewhere: the keys that were down are up, and the game waits */
        keys_release(a);
        if (running(a))
            a->paused = true;
        w->redraw = true;
        return;
    }
    if (op != LM_WL_KEYBOARD_EVT_KEY)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, w->mods);
    if (args[3].u == 0) {                                   /* a key let go */
        if (ks == LM_KS_Left)
            das_release(a, -1);
        else if (ks == LM_KS_Right)
            das_release(a, 1);
        else if (ks == LM_KS_Down)
            a->soft_down = false;
        return;
    }
    bool ctrl = (w->mods & LM_MOD_CTRL) != 0;
    char txt[8];
    int tl = lm_keysym_to_utf8(ks, txt);
    char c = tl == 1 ? (char)(txt[0] >= 'A' && txt[0] <= 'Z' ? txt[0] + 32 : txt[0]) : 0;
    w->redraw = true;
    if (a->prompt != P_NONE) {
        if (ks == LM_KS_Escape || (c == 'n' && a->prompt == P_NEW))
            a->prompt = P_NONE;
        else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter || (c == 'y' && a->prompt == P_NEW) || (a->prompt == P_OVER && c == 'n'))
            card_accept(a, 1);
        return;
    }
    if (ctrl) {
        if (c == 'q')
            w->running = false;
        else if (c == 'n')
            press_button(a, B_NEW);
        return;
    }
    if (ks == LM_KS_F1) {
        a->prompt = P_HELP;
        a->pc_hover = a->pc_down = -1;
        return;
    }
    if (c == 'n') {
        press_button(a, B_NEW);
        return;
    }
    if (c == 'g') {
        press_button(a, B_GHOST);
        return;
    }
    if (c == 'p' || (ks == LM_KS_Escape && !a->paused)) {
        press_button(a, B_PAUSE);
        return;
    }
    if (a->paused || a->g.over || a->g.clear_mask)
        return;
    struct game *g = &a->g;
    if (ks == LM_KS_Left && !a->left_down)
        das_press(a, -1);
    else if (ks == LM_KS_Right && !a->right_down)
        das_press(a, 1);
    else if (ks == LM_KS_Down) {
        if (!a->soft_down) {
            a->soft_down = true;
            game_soft(g);
        }
    } else if (ks == LM_KS_Up || c == 'x')
        game_rotate(g, 1);
    else if (c == 'z')
        game_rotate(g, -1);
    else if (c == ' ')
        game_hard(g);
    else if (c == 'c' || ks == LM_KS_Shift_L || ks == 0xffe2)
        game_hold(g);
    events(a);
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t version)
{
    struct app *a = w->data;
    (void)version;
    if (!strcmp(iface, "mellagui_settings_v1"))
        a->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, NULL, a);
}

static bool close_event(struct wlwin *w)
{
    (void)w;
    return true;
}

/* ---- the rules' test ---- */

static int popcount_board(const struct game *g)
{
    int n = 0;
    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++)
            n += g->cell[y][x] != 0;
    return n;
}

static int selftest(void)
{
    int bad = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "selftest: " __VA_ARGS__); fputc('\n', stderr); bad++; } } while (0)
    shapes_init();
    /* the shapes: four cells each, four turns bring a piece back, the O stays itself, the others look different in every turn */
    for (int t = 0; t < T_N; t++) {
        for (int r = 0; r < 4; r++) {
            int seen = 0;
            for (int c = 0; c < 4; c++) {
                int x = g_shape[t][r][c][0], y = g_shape[t][r][c][1];
                CHECK(x >= 0 && x < k_box[t] && y >= 0 && y < k_box[t], "%c turn %d: a cell outside its box", T_NAME[t], r);
                seen |= 1 << (y * 4 + x);
            }
            CHECK(__builtin_popcount((unsigned)seen) == 4, "%c turn %d has %d different cells", T_NAME[t], r, __builtin_popcount((unsigned)seen));
        }
        bool same = true;
        for (int c = 0; c < 4; c++) {
            int cx = k_box[t] - 1 - g_shape[t][3][c][1], cy = g_shape[t][3][c][0];                 /* a fifth turn is the first one */
            bool found = false;
            for (int d = 0; d < 4; d++)
                found = found || (g_shape[t][0][d][0] == cx && g_shape[t][0][d][1] == cy);
            same = same && found;
        }
        CHECK(same, "%c: four turns do not bring it back", T_NAME[t]);
    }
    /* every piece spawns on an empty board and the bag gives all seven once in every seven */
    for (uint32_t seed = 1; seed <= 60; seed++) {
        struct game g;
        game_new(&g, seed, 1, -1);
        CHECK(g.have_cur && !g.over, "seed %u: no piece at the start", seed);
        int counts[T_N] = { 0 }, first = g.cur.type;
        counts[first]++;
        for (int i = 0; i < 6; i++)
            counts[take_next(&g)]++;
        for (int t = 0; t < T_N; t++)
            CHECK(counts[t] == 1, "seed %u: piece %c came %d times in the first bag", seed, T_NAME[t], counts[t]);
        for (int bag = 0; bag < 20; bag++) {
            int cn[T_N] = { 0 };
            for (int i = 0; i < 7; i++)
                cn[take_next(&g)]++;
            for (int t = 0; t < T_N; t++)
                CHECK(cn[t] == 1, "seed %u: piece %c came %d times in bag %d", seed, T_NAME[t], cn[t], bag);
        }
    }
    for (int t = 0; t < T_N; t++) {
        struct game g;
        game_new(&g, 5, 1, t);
        CHECK(g.cur.type == t && !g.over, "the first piece should be %c, it is %c", T_NAME[t], T_NAME[g.cur.type]);
        bool shows = false;                                 /* it comes in at the top of the visible rows, not wholly hidden above them */
        for (int c = 0; c < 4; c++)
            shows = shows || g.cur.y + g_shape[t][0][c][1] >= HID;
        CHECK(!collides(&g, t, 0, g.cur.x, g.cur.y) && shows, "%c: the spawn place", T_NAME[t]);
    }
    /* the walls stop a piece, a hard drop lands where the ghost says */
    {
        struct game g;
        game_new(&g, 3, 1, T_T);
        int moves = 0;
        while (game_move(&g, -1))
            moves++;
        CHECK(moves == 3 && g.cur.x == 0, "T went %d to the left wall (x %d)", moves, g.cur.x);
        moves = 0;
        while (game_move(&g, 1))
            moves++;
        CHECK(g.cur.x + 2 == BW - 1, "T stops at the right wall at x %d", g.cur.x);
        int y0 = g.cur.y, gy = ghost_y(&g), r = game_hard(&g);
        CHECK(r == gy - y0 && r == 19, "hard drop went %d rows, the ghost was %d rows down", r, gy - y0);
        CHECK(g.cell[BH - 1][BW - 2] == T_T + 1 && g.cell[BH - 2][BW - 2] == T_T + 1 && g.cell[BH - 1][BW - 1] == T_T + 1, "T lay on the floor");
        CHECK(g.score == 2 * r, "hard drop is two points a row: %d for %d", g.score, r);
        CHECK(g.pieces == 1 && g.have_cur, "the next piece did not come");
    }
    /* four rows with a gap in one column, an I turned upright into the gap: a Tetris (800 at level 1), the rows go and the board is empty */
    {
        struct game g;
        game_new(&g, 9, 1, T_I);
        for (int y = BH - 4; y < BH; y++)
            for (int x = 0; x < BW - 1; x++)
                g.cell[y][x] = T_O + 1;
        CHECK(game_rotate(&g, 1), "the I would not turn");
        while (game_move(&g, 1))
            ;
        int before = g.score;
        game_hard(&g);
        CHECK(g.clear_mask == 0xfu << (BH - 4) && g.lines == 4, "four lines were not seen: mask %x lines %d", g.clear_mask, g.lines);
        CHECK(g.score - before >= 800, "a Tetris gave %d points", g.score - before);
        CHECK(!g.have_cur, "the next piece came before the rows were taken away");
        commit_clear(&g);
        CHECK(popcount_board(&g) == 0 && g.have_cur, "the board is not empty after the clear (%d cells)", popcount_board(&g));
        CHECK(g.b2b, "back to back not noted");
    }
    /* a second Tetris right after the first is worth half as much again; the level follows the lines */
    {
        struct game g;
        game_new(&g, 9, 1, T_I);
        g.b2b = true;
        for (int y = BH - 4; y < BH; y++)
            for (int x = 0; x < BW - 1; x++)
                g.cell[y][x] = T_O + 1;
        game_rotate(&g, 1);
        while (game_move(&g, 1))
            ;
        int before = g.score;
        game_hard(&g);
        CHECK(g.score - before >= 1200, "a back to back Tetris gave %d points", g.score - before);
        g.lines = 9;
        g.level = 1;
        score_clear(&g, 1);
        CHECK(g.level == 2 && (g.ev & EV_LEVEL), "level %d after ten lines", g.level);
    }
    /* a single line leaves the rows above it standing, shifted down */
    {
        struct game g;
        game_new(&g, 4, 1, T_O);
        for (int x = 0; x < BW; x++)
            g.cell[BH - 1][x] = x == 0 || x == 1 ? 0 : T_L + 1;
        g.cell[BH - 2][5] = T_S + 1;
        game_move(&g, -1);
        game_move(&g, -1);
        game_move(&g, -1);
        game_move(&g, -1);
        game_hard(&g);                                      /* the O fills the gap of two cells wide at the left */
        CHECK(g.clear_mask == 1u << (BH - 1), "mask %x", g.clear_mask);
        commit_clear(&g);
        CHECK(g.cell[BH - 1][5] == T_S + 1, "the S above did not fall one row");
    }
    /* hold: the first one takes the next piece, a second swap waits for the next lock, then the held piece comes back */
    {
        struct game g;
        game_new(&g, 11, 1, T_J);
        int next = g.q[0];
        CHECK(game_hold(&g) && g.hold == T_J && g.cur.type == next, "hold: first swap");
        CHECK(!game_hold(&g), "hold: two swaps for one piece");
        game_hard(&g);
        CHECK(game_hold(&g) && g.cur.type == T_J, "hold: the held piece did not come back (got %c)", T_NAME[g.cur.type]);
    }
    /* gravity and the lock delay */
    {
        struct game g;
        game_new(&g, 2, 1, T_O);
        int y0 = g.cur.y;
        CHECK(!game_tick(&g, 900, false) && g.cur.y == y0, "a piece fell before a second was over");
        CHECK(game_tick(&g, 100, false) && g.cur.y == y0 + 1, "no fall after a second");
        CHECK(gravity_ms(1) == 1000 && gravity_ms(10) < gravity_ms(5) && gravity_ms(15) >= 1, "the speeds %d %d %d", gravity_ms(1), gravity_ms(5), gravity_ms(10));
        CHECK(soft_ms(1) == 50, "soft drop at level 1: %d ms", soft_ms(1));
        while (!grounded(&g))
            game_soft(&g);
        int pieces = g.pieces;
        game_tick(&g, 400, false);
        CHECK(g.pieces == pieces, "locked after 400 ms");
        game_move(&g, 1);                                   /* a move restarts the wait */
        game_tick(&g, 400, false);
        CHECK(g.pieces == pieces, "a move did not restart the lock delay");
        for (int k = 0; k < 40 && g.pieces == pieces; k++) {
            game_move(&g, k % 2 ? -1 : 1);
            game_tick(&g, 100, false);
        }
        CHECK(g.pieces > pieces, "fifteen moves did not end the wait (a piece may not be held in the air for ever)");
    }
    /* a rotation against a wall kicks the piece out of it */
    {
        struct game g;
        game_new(&g, 6, 1, T_I);
        game_rotate(&g, 1);
        while (game_move(&g, -1))
            ;
        CHECK(game_rotate(&g, 1) && game_rotate(&g, 1) && game_rotate(&g, 1), "an upright I could not turn at the left wall");
        game_new(&g, 6, 1, T_T);
        game_rotate(&g, 1);
        while (game_move(&g, 1))
            ;
        CHECK(game_rotate(&g, -1) && !collides(&g, g.cur.type, g.cur.rot, g.cur.x, g.cur.y), "T at the right wall would not turn back");
    }
    /* a full spawn area is the end */
    {
        struct game g;
        game_new(&g, 8, 1, T_Z);
        for (int x = 3; x < 7; x++)
            g.cell[1][x] = T_I + 1;
        spawn(&g, T_T);
        CHECK(g.over && (g.ev & EV_OVER), "no end with a blocked spawn");
        CHECK(!game_move(&g, 1) && !game_rotate(&g, 1) && game_hard(&g) < 0, "a finished game moves");
    }
    /* chance: thousands of random actions on random boards never put a piece in a wall, and the cells add up (four a piece, ten a line) */
    {
        uint32_t s = 12345;
        for (int round = 0; round < 40 && bad < 10; round++) {
            struct game g;
            game_new(&g, (uint32_t)round + 100, 1 + round % 8, -1);
            for (int step = 0; step < 4000 && !g.over; step++) {
                switch (rng_next(&s) % 9) {
                case 0: game_move(&g, -1); break;
                case 1: game_move(&g, 1); break;
                case 2: game_rotate(&g, 1); break;
                case 3: game_rotate(&g, -1); break;
                case 4: game_soft(&g); break;
                case 5: game_hold(&g); break;
                case 6: game_tick(&g, (int)(rng_next(&s) % 300), rng_next(&s) % 2); break;
                case 7: if (rng_next(&s) % 4 == 0) game_hard(&g); break;
                default: game_tick(&g, 600, false); break;
                }
                if (g.clear_mask)
                    commit_clear(&g);
                if (g.over)
                    break;
                CHECK(!g.have_cur || !collides(&g, g.cur.type, g.cur.rot, g.cur.x, g.cur.y), "round %d: the piece is inside something", round);
                CHECK(popcount_board(&g) == 4 * g.pieces - BW * g.lines, "round %d: %d cells, %d pieces, %d lines", round, popcount_board(&g), g.pieces, g.lines);
                if (bad > 10)
                    break;
            }
        }
    }
    printf("mellagui-tetris selftest %s\n", bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}

/* ---- the boards the tests ask for ---- */

static void setup_board(struct game *g, const char *what)
{
    if (!strcmp(what, "almost")) {                          /* four rows full but for the last column: an upright I there makes a Tetris */
        for (int y = BH - 4; y < BH; y++)
            for (int x = 0; x < BW - 1; x++)
                g->cell[y][x] = (uint8_t)(T_J + 1 + (x + y) % 3);
    } else if (!strcmp(what, "top")) {                      /* no room for the next piece */
        for (int y = HID + 1; y < BH; y++)
            for (int x = 0; x < BW - 1; x++)
                g->cell[y][x] = (uint8_t)(T_S + 1);
        for (int x = 3; x < 7; x++)
            g->cell[1][x] = (uint8_t)(T_Z + 1);
        g->cell[2][3] = g->cell[2][4] = (uint8_t)(T_Z + 1);
        g->over = true;
        g->ev |= EV_OVER;
    }
}

/* ---- main ---- */

int main(int argc, char **argv)
{
    wlwin_hidpi();
    lm_theme_load_env();
    signal(SIGPIPE, SIG_IGN);
    uint32_t seed = 0;
    int level = 0, win_w = 780, win_h = 700, first = -1;
    bool still = false;
    const char *setup = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest"))
            return selftest();
        if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--level") && i + 1 < argc) {
            level = atoi(argv[++i]);
            if (level < 1 || level > 15) {
                fprintf(stderr, "mellagui-tetris: --level wants 1 to 15\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            char junk;
            if (sscanf(argv[++i], "%dx%d%c", &win_w, &win_h, &junk) != 2 || win_w < 300 || win_h < 300) {
                fprintf(stderr, "mellagui-tetris: --size wants a size like 780x700\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--piece") && i + 1 < argc) {
            const char *p = strchr(T_NAME, argv[++i][0]);
            if (!p || argv[i][1]) {
                fprintf(stderr, "mellagui-tetris: --piece wants one of I J L O S T Z\n");
                return 2;
            }
            first = (int)(p - T_NAME);
        } else if (!strcmp(argv[i], "--still")) {
            still = true;
        } else if (!strcmp(argv[i], "--setup") && i + 1 < argc) {
            setup = argv[++i];
            if (strcmp(setup, "almost") && strcmp(setup, "top")) {
                fprintf(stderr, "mellagui-tetris: --setup knows 'almost' and 'top'\n");
                return 2;
            }
        } else {
            fprintf(stderr, "usage: mellagui-tetris [--seed N] [--level 1..15] [--size WxH] | --selftest\n");
            return 2;
        }
    }
    g_dump = getenv("MELLAGUI_TETRIS_DUMP");
    shapes_init();
    static struct app a;
    stats_load(&a.st);
    if (level)
        a.st.level = level;
    a.still = still;
    a.hover = -1;
    a.pc_hover = a.pc_down = -1;
    wlrail_init(&a.ran);
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event, .close = close_event };
    if (wlwin_connect(&a.win, "mellagui-tetris", &ops, &a) < 0)
        return 1;
    if (wlwin_open(&a.win, "Tetris", "mellagui.tetris", win_w, win_h, 600, 540) < 0)
        return 1;
    new_game(&a, seed ? seed : fresh_seed(), a.st.level, first);
    if (setup) {
        setup_board(&a.g, setup);
        if (!strcmp(setup, "almost")) {
            a.g.hold = -1;
            spawn(&a.g, first >= 0 ? first : T_I);
        }
    }
    events(&a);
    while (a.win.running) {
        struct wlrail L;
        rails(&a, &L);
        int timeout = wlrail_loop(&a.win, &a.ran, &L, a.hover, -1);
        int64_t now = now_ms();
        if (running(&a)) {
            if (a.g.clear_mask)
                timeout = LM_CLAMP((int)(a.clear_at - now), 1, 55);       /* the rows flash */
            else if (!a.still) {
                int t = game_next_ms(&a.g, a.soft_down);
                if (t > 0 && (timeout < 0 || t < timeout))
                    timeout = t;
            }
            if (a.das_dir) {
                int t = LM_MAX(1, (int)(a.das_next - now));
                if (timeout < 0 || t < timeout)
                    timeout = t;
            }
            if (timeout < 0 || timeout > 500)
                timeout = 500;                                          /* the clock */
        }
        if (a.msg[0] && (timeout < 0 || timeout > 500))
            timeout = 500;
        if (!wlwin_step(&a.win, timeout))
            break;
        now = now_ms();
        advance(&a, now);
        if (running(&a) || a.msg[0])
            a.win.redraw = true;
        if (a.msg[0] && now - a.msg_ms >= 6000)
            a.msg[0] = 0;
    }
    bank(&a);
    wlwin_destroy(&a.win);
    return 0;
}
