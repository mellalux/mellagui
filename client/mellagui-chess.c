/* mellagui-chess - chess against the computer (or against a friend), a window of the wlwin family.
 *
 * The rules are client/chess.[ch], the search client/chessai.[ch], the pieces client/chessart.h (drawn from distance functions: no fonts, no files).
 * This file is the window: the board, the pointer and the keys, the engine in a thread of its own, the cards, the best results, the test dump.
 *
 *   mellagui-chess [--level 0..9] [--black] [--fen FEN] [--clock-seconds BASE+INC] [--size WxH] [--delay MS]
 *   mellagui-chess --selftest | --sheet FILE.ppm [SIZE]      (the pieces on a board, to look at)
 *
 * Level 0 = two players on one screen, 1..9 = the engine (chai_level).  Move a piece by dragging it or by a click on it and a click on where it goes;
 * the arrow keys and Enter do the same; N new game, U / Backspace take a move back, H asks the engine for a hint, F turns the board, 0..9 the level.
 * The engine thinks in a thread (it never stops the window) and answers after at least --delay ms, so a move can be seen.
 * The clock (C steps through the presets): it starts when both sides have made their first move, the move that is made adds the increment,
 * the side whose time runs out loses (a draw when the other side could not mate anyway), the engine plans its thinking by the time it has.
 */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "chess.h"
#include "chessai.h"
#include "chessart.h"
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

#define STAT_H 24
#define LABEL 18                                            /* the room for the file letters and rank numbers */
#define PANEL_W 214
#define DRAG_PX 5
#define HINT_MS 1200
#define HINT_SHOW_MS 12000

/* the board is made of literals on purpose: the same cream and green whatever the theme */
#define SQ_LIGHT 0xeeeed2u
#define SQ_DARK 0x769656u
#define SQ_LAST_L 0xf0e68cu
#define SQ_LAST_D 0xc5b84au
#define SQ_SEL_L 0xf7f26au
#define SQ_SEL_D 0xd6c93au
#define SQ_HINT_L 0x9ec5f0u
#define SQ_HINT_D 0x5f9bd6u
#define SQ_CHECK 0xe84a4au

/* the end of a game by the clock, beside the values of ch_status */
#define ST_FLAG 100                                         /* the side whose clock ran out has lost */
#define ST_FLAG_DRAW 101                                    /* ... but the other side could not have mated */

static const struct { int base_s, inc_s; const char *name; } k_clocks[] = {
    { 0, 0, "No clock" }, { 60, 0, "1 min" }, { 180, 0, "3 min" }, { 180, 2, "3 min + 2 s" }, { 300, 0, "5 min" },
    { 300, 3, "5 min + 3 s" }, { 600, 0, "10 min" }, { 900, 10, "15 min + 10 s" }, { 1800, 0, "30 min" },
};
#define N_CLOCKS ((int)(sizeof(k_clocks) / sizeof(k_clocks[0])))

enum prompt { P_NONE, P_NEW, P_PROMO, P_OVER, P_HELP };
enum { B_NEW, B_UNDO, B_HINT, B_FLIP, B_CLOCK, B_LEVEL, B_HELP, B_COUNT };

struct stats {
    int played, won, drawn, lost;
    int level;                                              /* 0 = two players */
    int human;                                              /* the side the person plays: CH_WHITE or CH_BLACK */
    int clock;                                              /* the preset of k_clocks that a new game starts with */
};

struct clk {
    bool on;
    int64_t left[2];                                        /* ms left for White and Black */
    int64_t inc_ms;
    int64_t last;                                           /* when the time was last counted */
};

/* The engine's thread: it gets a copy of the position and the keys of the game, and says when it is done. */
struct think {
    pthread_t th;
    bool started;
    int mode;                                               /* 0 = a move to play, 1 = a hint */
    int ticket;                                             /* a result is used only if the ticket is still the current one */
    int done;                                               /* atomic */
    int depth, score;                                       /* atomic: what it has found so far */
    struct ch_pos pos;
    uint64_t *hist;
    int nhist;
    struct chai_limits lim;
    struct chai_result res;
    int64_t t0;
};

struct app {
    struct wlwin win;
    struct ch_game g;
    struct chai *ai;
    int human;                                              /* the side the person plays; -1 = both (two players) */
    int level;
    bool flipped;
    int sel;                                                /* the selected square, -1 none */
    int kb;                                                 /* the keyboard's square, -1 none */
    ch_move legal[CH_MAX_MOVES];
    int nlegal;
    bool pressed, dragging, was_selected;
    int drag_from;
    double px, py, press_x, press_y;
    bool has_pointer;
    int hover;
    struct wlrail_anim ran;
    enum prompt prompt;
    int pc_hover, pc_down;
    int promo_from, promo_to;
    struct think tk;
    int ticket;
    struct clk clk;
    int64_t (*snap)[2];                                     /* the clocks after 0, 1, 2 ... plies: what undo puts back */
    int snapcap;
    int cli_base, cli_inc;                                  /* --clock-seconds, -1 = not given */
    int timeout;                                            /* the side that lost on time, -1 none */
    bool timeout_draw;
    ch_move hint;
    int64_t hint_ms;
    int delay_ms;
    bool banked;
    char msg[160];
    int64_t msg_ms;
    struct stats st;
    uint32_t settings;
    int64_t last_sound_ms;
    uint32_t *spr[2][7];                                    /* the pieces at the size of the board's squares (buffer pixels) */
    int sprsz;
    const char *start_fen;
};

static const char *g_dump;

static int64_t now_ms(void) { return wlrail_now(); }

#define COL_BAR (lm_theme.bar)
#define COL_TEXT (lm_theme.text_soft)
#define COL_DIM (lm_theme.text_dim)
#define COL_ACCENT (lm_theme.accent)

static void say(struct app *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->msg, sizeof(a->msg), fmt, ap);
    va_end(ap);
    a->msg_ms = now_ms();
    a->win.redraw = true;
}

/* The compositor plays the sounds (write-only setting card-sound); the card game's are borrowed: place = a move, draw = a capture, win / no = the end. */
static void sound(struct app *a, const char *name)
{
    int64_t now = now_ms();
    if (!a->settings || !a->win.c || (now - a->last_sound_ms < 90 && strcmp(name, "win") && strcmp(name, "no")))
        return;
    a->last_sound_ms = now;
    WLC_SEND(a->win.c, a->settings, LM_MELLAGUI_SETTINGS_V1_REQ_SET, { .s = "card-sound" }, { .s = name });
    wlc_flush(a->win.c);
}

/* ---- the file with the results ---- */

static void stats_path(char *out, size_t n)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x)
        snprintf(out, n, "%s/mellagui/chess.conf", x);
    else
        snprintf(out, n, "%s/.config/mellagui/chess.conf", h && *h ? h : ".");
}

static void stats_load(struct stats *s)
{
    memset(s, 0, sizeof(*s));
    s->level = 4;
    s->human = CH_WHITE;
    char path[PATH_MAX];
    stats_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "played = %d", &v) == 1) s->played = v;
        else if (sscanf(line, "won = %d", &v) == 1) s->won = v;
        else if (sscanf(line, "drawn = %d", &v) == 1) s->drawn = v;
        else if (sscanf(line, "lost = %d", &v) == 1) s->lost = v;
        else if (sscanf(line, "level = %d", &v) == 1) s->level = LM_CLAMP(v, 0, chai_level_count());
        else if (sscanf(line, "side = %d", &v) == 1) s->human = v == 1 ? CH_BLACK : CH_WHITE;
        else if (sscanf(line, "clock = %d", &v) == 1) s->clock = LM_CLAMP(v, 0, N_CLOCKS - 1);
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
    fprintf(f, "played = %d\nwon = %d\ndrawn = %d\nlost = %d\nlevel = %d\nside = %d\nclock = %d\n", s->played, s->won, s->drawn, s->lost, s->level,
            s->human == CH_BLACK ? 1 : 0, s->clock);
    fclose(f);
}

static const char *level_name(int level)
{
    return level == 0 ? "Two players" : chai_level_name(level);
}

/* ---- the geometry ---- */

struct lay {
    int cs, bx, by;                                         /* a square's edge, the board's top left corner */
    bool panel;
    int px, pw;
};

static void layout(const struct app *a, struct lay *L)
{
    int W = a->win.bw - WLRAIL_W, H = a->win.bh - STAT_H;
    int availh = H - 16 - LABEL, availw = W - 16 - LABEL;
    int cs_panel = LM_MIN(availh / 8, (availw - PANEL_W - 18) / 8), cs_full = LM_MIN(availh / 8, availw / 8);
    L->panel = cs_panel >= 44;
    L->cs = LM_MAX(16, L->panel ? cs_panel : cs_full);
    L->pw = L->panel ? PANEL_W : 0;
    int total = LABEL + 8 * L->cs + (L->panel ? 18 + L->pw : 0);
    int x0 = WLRAIL_W + LM_MAX(0, (W - total) / 2);
    L->bx = x0 + LABEL;
    L->by = LM_MAX(8, (H - LABEL - 8 * L->cs) / 2);
    L->px = L->bx + 8 * L->cs + 18;
}

static void sq_xy(const struct lay *L, bool flipped, int sq, int *x, int *y)
{
    int f = sq & 7, r = sq >> 3;
    int col = flipped ? 7 - f : f, row = flipped ? r : 7 - r;
    *x = L->bx + col * L->cs;
    *y = L->by + row * L->cs;
}

static int sq_at(const struct lay *L, bool flipped, double px, double py)
{
    int col = (int)floor((px - L->bx) / L->cs), row = (int)floor((py - L->by) / L->cs);
    if (px < L->bx || py < L->by || col < 0 || col > 7 || row < 0 || row > 7)
        return -1;
    int f = flipped ? 7 - col : col, r = flipped ? row : 7 - row;
    return r * 8 + f;
}

static void sq_name(int sq, char *out)
{
    out[0] = (char)('a' + (sq & 7));
    out[1] = (char)('1' + (sq >> 3));
    out[2] = 0;
}

/* ---- the game ---- */

static bool human_turn(const struct app *a)
{
    return a->human < 0 || a->g.pos.side == a->human;
}

static int status(const struct app *a)
{
    if (a->timeout >= 0)
        return a->timeout_draw ? ST_FLAG_DRAW : ST_FLAG;
    return ch_game_status(&a->g);
}

static void refresh_legal(struct app *a)
{
    a->nlegal = ch_gen_legal(&a->g.pos, a->legal);
}

static bool is_legal_to(const struct app *a, int from, int to)
{
    for (int i = 0; i < a->nlegal; i++)
        if (CH_FROM(a->legal[i]) == from && CH_TO(a->legal[i]) == to)
            return true;
    return false;
}

static bool is_promotion(const struct app *a, int from, int to)
{
    for (int i = 0; i < a->nlegal; i++)
        if (CH_FROM(a->legal[i]) == from && CH_TO(a->legal[i]) == to && CH_PROMO(a->legal[i]))
            return true;
    return false;
}

static int material(const struct ch_pos *p, int colour)
{
    static const int v[7] = { 0, 1, 3, 3, 5, 9, 0 };
    int m = 0;
    for (int s = 0; s < 64; s++)
        if (p->sq[s] && CH_COLOR(p->sq[s]) == colour)
            m += v[CH_TYPE(p->sq[s])];
    return m;
}

/* ---- the engine's thread ---- */

static void think_info(const struct chai_result *r, void *ud)
{
    struct think *t = ud;
    __atomic_store_n(&t->depth, r->depth, __ATOMIC_RELAXED);
    __atomic_store_n(&t->score, r->score, __ATOMIC_RELAXED);
}

struct think_job {
    struct think *t;
    struct chai *ai;
};

static void *think_thread(void *arg)
{
    struct think_job *j = arg;
    struct think *t = j->t;
    chai_search(j->ai, &t->pos, t->hist, t->nhist, &t->lim, &t->res, think_info, t);
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
    free(j);
    return NULL;
}

static void think_cancel(struct app *a)
{
    struct think *t = &a->tk;
    if (!t->started)
        return;
    chai_stop(a->ai);
    pthread_join(t->th, NULL);
    t->started = false;
    free(t->hist);
    t->hist = NULL;
    a->ticket++;
    a->win.redraw = true;
}

static void think_start(struct app *a, int mode)
{
    struct think *t = &a->tk;
    if (t->started)
        return;
    memset(&t->res, 0, sizeof(t->res));
    t->mode = mode;
    t->ticket = a->ticket;
    t->done = 0;
    t->depth = t->score = 0;
    t->pos = a->g.pos;
    t->nhist = a->g.n + 1;
    t->hist = malloc((size_t)t->nhist * sizeof(uint64_t));
    if (!t->hist)
        return;
    memcpy(t->hist, a->g.hist, (size_t)t->nhist * sizeof(uint64_t));
    memset(&t->lim, 0, sizeof(t->lim));
    if (mode == 1) {
        t->lim.time_ms = HINT_MS;
    } else {
        chai_level(a->level, &t->lim);
        if (t->lim.noise == 0)
            t->lim.noise = 4;                               /* so that the strong levels do not play the same game every time */
        t->lim.seed = (uint32_t)(a->g.n * 2654435761u) ^ (uint32_t)a->g.hist[a->g.n];
        if (a->clk.on) {                                    /* it plans by the time it has: a 25th of it and most of the increment, never a third */
            int64_t left = a->clk.left[a->g.pos.side], budget = left / 25 + a->clk.inc_ms * 3 / 4;
            if (left < 3000)
                budget = left / 15;
            budget = LM_CLAMP(budget, 20, LM_MAX(20, left / 3));
            if (t->lim.time_ms == 0 || budget < t->lim.time_ms)
                t->lim.time_ms = (int)budget;
        }
    }
    struct think_job *j = malloc(sizeof(*j));
    if (!j) {
        free(t->hist);
        t->hist = NULL;
        return;
    }
    j->t = t;
    j->ai = a->ai;
    t->t0 = now_ms();
    if (pthread_create(&t->th, NULL, think_thread, j) != 0) {
        free(j);
        free(t->hist);
        t->hist = NULL;
        return;
    }
    t->started = true;
    a->win.redraw = true;
}

static bool engine_to_move(const struct app *a)
{
    return a->level > 0 && a->human >= 0 && a->g.pos.side != a->human && status(a) == CH_ONGOING;
}

/* ---- moves ---- */

static void deselect(struct app *a)
{
    a->sel = -1;
    a->pressed = a->dragging = false;
}

static void bank(struct app *a, int s)
{
    if (a->banked || a->level == 0 || a->human < 0)
        return;
    a->banked = true;
    a->st.played++;
    int loser = s == CH_CHECKMATE ? a->g.pos.side : s == ST_FLAG ? a->timeout : -1;
    if (loser >= 0) {
        if (loser == a->human)
            a->st.lost++;
        else
            a->st.won++;
    } else {
        a->st.drawn++;
    }
    stats_save(&a->st);
}

static const char *result_text(const struct app *a, int s)
{
    if (s == CH_CHECKMATE) {
        bool human_lost = a->human >= 0 && a->g.pos.side == a->human;
        if (a->human < 0)
            return a->g.pos.side == CH_WHITE ? "Checkmate - Black wins" : "Checkmate - White wins";
        return human_lost ? "Checkmate - the computer wins" : "Checkmate - you win!";
    }
    if (s == ST_FLAG) {
        if (a->human < 0)
            return a->timeout == CH_WHITE ? "White's time is up - Black wins" : "Black's time is up - White wins";
        return a->timeout == a->human ? "Your time is up - the computer wins" : "The computer's time is up - you win!";
    }
    if (s == ST_FLAG_DRAW)
        return "Time is up - a draw: the other side could not have mated";
    if (s == CH_STALEMATE)
        return "Stalemate - a draw";
    return s == CH_DRAW_MATERIAL ? "A draw: neither side can mate" : s == CH_DRAW_REPETITION ? "A draw by repetition" : "A draw by the fifty move rule";
}

/* the game is over (a mate, a draw, a flag): the card, the result counted, a sound */
static void finish(struct app *a, int s)
{
    think_cancel(a);
    bank(a, s);
    a->prompt = P_OVER;
    a->pc_hover = a->pc_down = -1;
    a->hint = 0;
    deselect(a);
    bool human_lost = s == CH_CHECKMATE ? a->g.pos.side == a->human : s == ST_FLAG ? a->timeout == a->human : false;
    bool decisive = s == CH_CHECKMATE || s == ST_FLAG;
    sound(a, !decisive ? "draw" : (a->human < 0 || !human_lost) ? "win" : "no");
    a->win.redraw = true;
}

/* what happened after a move: the sounds, the end of the game, the engine's turn */
static void after_move(struct app *a, bool capture)
{
    refresh_legal(a);
    a->hint = 0;
    int s = status(a);
    a->win.redraw = true;
    if (s != CH_ONGOING) {
        finish(a, s);
        return;
    }
    sound(a, capture ? "draw" : "place");
    if (ch_in_check(&a->g.pos))
        say(a, "Check!");
}

/* ---- the clock ---- */

/* The clock runs for the side to move once both sides have made their first move (those two are not timed and earn no increment); it stands
 * while the new-game or key card is open and after the end of the game. Time is counted in steps (the loop turns): a step longer than 2 s
 * (the process was stopped) counts as 2 s. */
static bool clock_running(const struct app *a)
{
    return a->clk.on && a->g.n >= 2 && status(a) == CH_ONGOING && a->prompt != P_NEW && a->prompt != P_HELP;
}

static void clock_advance(struct app *a, int64_t now)
{
    int64_t dt = now - a->clk.last;
    a->clk.last = now;
    if (dt <= 0 || !clock_running(a))
        return;
    if (dt > 2000)
        dt = 2000;
    a->clk.left[a->g.pos.side] -= dt;
}

/* can this side mate at all (with the help of the other)?  A bare king, a king and one minor piece, or bishops all on one colour cannot. */
static bool could_mate(const struct ch_pos *p, int side)
{
    int minors = 0, knights = 0, light = 0, dark = 0;
    for (int s = 0; s < 64; s++) {
        uint8_t pc = p->sq[s];
        if (!pc || CH_COLOR(pc) != side)
            continue;
        int t = CH_TYPE(pc);
        if (t == CH_PAWN || t == CH_ROOK || t == CH_QUEEN)
            return true;
        if (t == CH_KNIGHT) {
            knights++;
            minors++;
        } else if (t == CH_BISHOP) {
            minors++;
            if (((s & 7) + (s >> 3)) & 1)
                light++;
            else
                dark++;
        }
    }
    if (minors <= 1)
        return false;
    return !(knights == 0 && (light == 0 || dark == 0));
}

static void clock_check_flag(struct app *a)
{
    if (!clock_running(a))
        return;
    int side = a->g.pos.side;
    if (a->clk.left[side] > 0)
        return;
    a->clk.left[side] = 0;
    a->timeout = side;
    a->timeout_draw = !could_mate(&a->g.pos, side ^ 1);
    finish(a, status(a));
}

static void snap_set(struct app *a, int n)
{
    if (n + 1 > a->snapcap) {
        int cap = LM_MAX(n + 1, a->snapcap * 2 + 64);
        int64_t (*p)[2] = realloc(a->snap, (size_t)cap * sizeof(*p));
        if (!p)
            return;
        a->snap = p;
        a->snapcap = cap;
    }
    a->snap[n][0] = a->clk.left[0];
    a->snap[n][1] = a->clk.left[1];
}

static void clock_reset(struct app *a)
{
    int base = a->cli_base >= 0 ? a->cli_base : k_clocks[a->st.clock].base_s;
    int inc = a->cli_base >= 0 ? a->cli_inc : k_clocks[a->st.clock].inc_s;
    a->clk.on = base > 0;
    a->clk.left[0] = a->clk.left[1] = (int64_t)base * 1000;
    a->clk.inc_ms = (int64_t)inc * 1000;
    a->clk.last = now_ms();
    a->timeout = -1;
    a->timeout_draw = false;
    snap_set(a, 0);
}

/* "4:59", "12:03", "1:02:03", and tenths when it gets short: "9.4" */
static void fmt_clock(int64_t ms, char *out, size_t n)
{
    if (ms < 0)
        ms = 0;
    if (ms < 20000)
        snprintf(out, n, "%d.%d", (int)(ms / 1000), (int)(ms % 1000 / 100));
    else if (ms >= 3600000)
        snprintf(out, n, "%d:%02d:%02d", (int)(ms / 3600000), (int)(ms / 60000 % 60), (int)(ms / 1000 % 60));
    else
        snprintf(out, n, "%d:%02d", (int)(ms / 60000), (int)(ms / 1000 % 60));
}

static bool play(struct app *a, ch_move m)
{
    if (!m)
        return false;
    bool capture = ch_move_is_capture(&a->g.pos, m);
    if (a->tk.started && a->tk.mode == 1)
        think_cancel(a);                                    /* a hint for a position that is gone */
    int mover = a->g.pos.side;
    clock_advance(a, now_ms());                             /* (the time of this move goes to the mover first) */
    if (ch_game_play(&a->g, m) < 0)
        return false;
    if (a->clk.on) {
        if (a->g.n >= 3)
            a->clk.left[mover] += a->clk.inc_ms;
        snap_set(a, a->g.n);
    }
    deselect(a);
    a->msg[0] = 0;
    after_move(a, capture);
    return true;
}

static void new_game(struct app *a, int human)
{
    think_cancel(a);
    ch_game_free(&a->g);
    const char *err = NULL;
    if (ch_game_init(&a->g, a->start_fen, &err) < 0)
        ch_game_init(&a->g, NULL, NULL);
    chai_clear(a->ai);
    a->human = a->level == 0 ? -1 : human;
    if (a->level > 0) {
        a->st.human = human;
        a->flipped = human == CH_BLACK;
    }
    a->banked = false;
    a->prompt = P_NONE;
    a->hint = 0;
    a->msg[0] = 0;
    a->kb = -1;
    clock_reset(a);
    deselect(a);
    refresh_legal(a);
    stats_save(&a->st);
    a->win.redraw = true;
}

static void set_level(struct app *a, int level)
{
    level = LM_CLAMP(level, 0, chai_level_count());
    if (level == a->level)
        return;
    a->level = level;
    a->st.level = level;
    stats_save(&a->st);
    think_cancel(a);
    if (level == 0)
        a->human = -1;
    else if (a->human < 0)
        a->human = a->g.pos.side;                           /* from two players to the computer: the person keeps the side to move */
    if (level == 0)
        say(a, "Two players");
    else
        say(a, "Level %d: %s", level, level_name(level));
    a->win.redraw = true;
}

static void undo(struct app *a)
{
    think_cancel(a);
    if (a->g.n == 0)
        return;
    int back = 1;
    if (a->level > 0 && a->human >= 0 && a->g.pos.side == a->human && a->g.n >= 2)
        back = 2;                                           /* the computer's answer and the person's move */
    for (int i = 0; i < back; i++)
        ch_game_undo(&a->g);
    if (a->timeout >= 0) {                                  /* a game lost on time is taken up again */
        a->timeout = -1;
        a->timeout_draw = false;
    }
    if (a->clk.on && a->snap && a->g.n < a->snapcap) {
        a->clk.left[0] = a->snap[a->g.n][0];
        a->clk.left[1] = a->snap[a->g.n][1];
        a->clk.last = now_ms();
    }
    a->prompt = P_NONE;
    a->banked = false;
    a->hint = 0;
    deselect(a);
    refresh_legal(a);
    say(a, back == 2 ? "Two moves taken back" : "One move taken back");
    sound(a, "place");
    a->win.redraw = true;
}

static void ask_hint(struct app *a)
{
    if (status(a) != CH_ONGOING || a->tk.started || a->prompt != P_NONE)
        return;
    if (!human_turn(a)) {
        say(a, "Wait for the computer");
        return;
    }
    say(a, "Looking for a good move ...");
    think_start(a, 1);
}

/* the engine's thread has finished: use the result */
static void think_collect(struct app *a, int64_t now)
{
    struct think *t = &a->tk;
    if (!t->started || !__atomic_load_n(&t->done, __ATOMIC_ACQUIRE))
        return;
    if (t->mode == 0 && now - t->t0 < a->delay_ms)
        return;                                             /* a move is shown after a short wait */
    pthread_join(t->th, NULL);
    t->started = false;
    free(t->hist);
    t->hist = NULL;
    if (t->ticket != a->ticket)
        return;
    if (t->mode == 1) {
        a->hint = t->res.best;
        a->hint_ms = now;
        if (a->hint) {
            char san[16];
            ch_move_san(&a->g.pos, a->hint, san);
            say(a, "Hint: %s", san);
        }
        a->win.redraw = true;
        return;
    }
    if (t->res.best && engine_to_move(a))
        play(a, t->res.best);
}

/* ---- the sprites ---- */

static void sprites_free(struct app *a)
{
    for (int c = 0; c < 2; c++)
        for (int t = 0; t < 7; t++) {
            free(a->spr[c][t]);
            a->spr[c][t] = NULL;
        }
}

static const uint32_t *sprite(struct app *a, int colour, int type, int size)
{
    if (size != a->sprsz) {
        sprites_free(a);
        a->sprsz = size;
    }
    if (!a->spr[colour][type])
        a->spr[colour][type] = chessart_piece(type, colour, size);
    return a->spr[colour][type];
}

static void draw_piece(struct app *a, int piece, int x, int y, int cs)
{
    int sc = wlwin_scale(&a->win), sz = cs * sc;
    const uint32_t *px = sprite(a, CH_COLOR(piece), CH_TYPE(piece), sz);
    if (px)
        wlwin_blit_px_over(&a->win, x, y, px, sz, sz, sz);
}

/* ---- painting ---- */

static void label(struct wlwin *w, int x, int y, const char *s, uint32_t rgb)
{
    wlwin_text_bold(w, x, y, s, 1, rgb, 0);
}

static uint32_t sq_color(int sq, bool last, bool sel, bool hint)
{
    bool light = ((sq & 7) + (sq >> 3)) & 1;                /* a1 is dark */
    if (sel)
        return light ? SQ_SEL_L : SQ_SEL_D;
    if (hint)
        return light ? SQ_HINT_L : SQ_HINT_D;
    if (last)
        return light ? SQ_LAST_L : SQ_LAST_D;
    return light ? SQ_LIGHT : SQ_DARK;
}

static void paint_board(struct app *a, const struct lay *L)
{
    struct wlwin *w = &a->win;
    const struct ch_pos *p = &a->g.pos;
    int cs = L->cs;
    ch_move last = a->g.n > 0 ? a->g.ply[a->g.n - 1].move : 0;
    int check_sq = ch_in_check(p) ? p->ksq[p->side] : -1;
    wlwin_fill(w, L->bx - 3, L->by - 3, 8 * cs + 6, 8 * cs + 6, 0x2c3340u);
    for (int sq = 0; sq < 64; sq++) {
        int x, y;
        sq_xy(L, a->flipped, sq, &x, &y);
        bool islast = last && (sq == CH_FROM(last) || sq == CH_TO(last));
        bool ishint = a->hint && (sq == CH_FROM(a->hint) || sq == CH_TO(a->hint));
        uint32_t c = sq_color(sq, islast, sq == a->sel, ishint);
        if (sq == check_sq)
            c = lm_mix_rgb(c, SQ_CHECK, 0.62);
        else if (a->sel >= 0 && p->sq[sq] && is_legal_to(a, a->sel, sq))
            c = lm_mix_rgb(c, 0xe05050u, 0.40);
        wlwin_fill(w, x, y, cs, cs, c);
        if (a->sel >= 0 && !p->sq[sq] && is_legal_to(a, a->sel, sq)) {    /* where the selected piece can go */
            int d = LM_MAX(6, cs / 3);
            wlwin_round_fill(w, x + (cs - d) / 2, y + (cs - d) / 2, d, d, d / 2, lm_mix_rgb(c, 0x000000, 0.24));
        }
    }
    if (a->sel >= 0 && p->sq[a->sel] == 0)
        a->sel = -1;
    /* the coordinates */
    for (int i = 0; i < 8; i++) {
        char t[2] = { 0, 0 };
        int f = a->flipped ? 7 - i : i;
        t[0] = (char)('a' + f);
        wlwin_text(w, L->bx + i * cs + cs / 2 - lm_font_text_width(t) / 2, L->by + 8 * cs + 4, t, 1, COL_DIM, 0);
        int r = a->flipped ? i : 7 - i;
        t[0] = (char)('1' + r);
        wlwin_text(w, L->bx - LABEL + 4, L->by + i * cs + cs / 2 - LM_FONT_H / 2, t, 1, COL_DIM, 0);
    }
    /* the pieces (the one that is carried is drawn last, at the pointer) */
    for (int sq = 0; sq < 64; sq++) {
        if (!p->sq[sq] || (a->dragging && sq == a->drag_from))
            continue;
        int x, y;
        sq_xy(L, a->flipped, sq, &x, &y);
        draw_piece(a, p->sq[sq], x, y, cs);
    }
    if (a->kb >= 0) {                                       /* the keyboard's square */
        int x, y, t = 3;
        sq_xy(L, a->flipped, a->kb, &x, &y);
        wlwin_fill(w, x, y, cs, t, COL_ACCENT);
        wlwin_fill(w, x, y + cs - t, cs, t, COL_ACCENT);
        wlwin_fill(w, x, y + t, t, cs - 2 * t, COL_ACCENT);
        wlwin_fill(w, x + cs - t, y + t, t, cs - 2 * t, COL_ACCENT);
    }
    if (a->dragging && a->drag_from >= 0 && p->sq[a->drag_from])
        draw_piece(a, p->sq[a->drag_from], (int)a->px - cs / 2, (int)a->py - cs / 2, cs);
}

static void format_moves(const struct app *a, int row, char *out, size_t n)
{
    /* row r holds the plies 2r and 2r + 1 of the list; a game from a FEN may start with Black's move */
    int first_no = a->g.start.full + 0;
    int skip = a->g.start.side == CH_BLACK ? 1 : 0;
    int i0 = row * 2 - skip, i1 = i0 + 1;
    int no = first_no + row;
    char w1[20] = "", b1[20] = "";
    if (i0 >= 0 && i0 < a->g.n)
        snprintf(w1, sizeof(w1), "%s", a->g.ply[i0].san);
    else if (i0 < 0)
        snprintf(w1, sizeof(w1), "...");
    if (i1 >= 0 && i1 < a->g.n)
        snprintf(b1, sizeof(b1), "%s", a->g.ply[i1].san);
    snprintf(out, n, "%d. %s  %s", no, w1, b1);
}

static void side_line(struct wlwin *w, int x, int y, int pw, int colour, const char *who, int plus)
{
    wlwin_round_fill(w, x, y + 1, 14, 14, 3, colour == CH_WHITE ? 0xf6f1e4u : 0x3a3d48u);
    label(w, x + 20, y, who, lm_theme.text_bright);
    if (plus > 0) {
        char t[16];
        snprintf(t, sizeof(t), "+%d", plus);
        wlwin_text(w, x + pw - lm_font_text_width(t), y, t, 1, COL_DIM, 0);
    }
}

/* one side's clock: a bar with the time, lit while it runs, red when it is nearly gone */
static void paint_clock(struct app *a, int x, int y, int pw, int side)
{
    struct wlwin *w = &a->win;
    bool running = clock_running(a) && a->g.pos.side == side, low = a->clk.left[side] < 10000;
    char t[24];
    fmt_clock(a->clk.left[side], t, sizeof(t));
    uint32_t bg = running ? (low ? 0xb03030u : COL_ACCENT) : COL_BAR;
    wlwin_round_fill(w, x, y, pw, 30, 10, bg);
    uint32_t ink = running ? lm_theme.on_colour : (low && a->clk.on ? 0xff8a80u : lm_theme.text_bright);
    wlwin_text_bold(w, x + pw - 12 - lm_font_text_width_bold(t) * 2, y + (30 - 2 * LM_FONT_H) / 2, t, 2, ink, 0);
}

static void paint_panel(struct app *a, const struct lay *L)
{
    if (!L->panel)
        return;
    struct wlwin *w = &a->win;
    int x = L->px, pw = L->pw, y0 = L->by, h = 8 * L->cs;
    int top_side = a->flipped ? CH_WHITE : CH_BLACK, bot_side = top_side ^ 1;
    int diff = material(&a->g.pos, CH_WHITE) - material(&a->g.pos, CH_BLACK);
    int clk_h = a->clk.on ? 36 : 0;
    char who[48];
    for (int k = 0; k < 2; k++) {
        int side = k == 0 ? top_side : bot_side;
        if (a->human < 0)
            snprintf(who, sizeof(who), "%s", side == CH_WHITE ? "White" : "Black");
        else if (side == a->human)
            snprintf(who, sizeof(who), "%s  (you)", side == CH_WHITE ? "White" : "Black");
        else
            snprintf(who, sizeof(who), "%s  (%s)", side == CH_WHITE ? "White" : "Black", level_name(a->level));
        int plus = side == CH_WHITE ? diff : -diff;
        side_line(w, x, k == 0 ? y0 : y0 + h - LM_FONT_H - 2, pw, side, who, plus);
        if (a->clk.on)
            paint_clock(a, x, k == 0 ? y0 + LM_FONT_H + 6 : y0 + h - LM_FONT_H - 2 - 36, pw, side);
    }
    /* the moves */
    int ly = y0 + LM_FONT_H + 10 + clk_h, lh = h - 2 * (LM_FONT_H + 10) - 2 * clk_h;
    wlwin_round_fill(w, x, ly, pw, lh, 10, COL_BAR);
    int rowh = LM_FONT_H + 4, rows = (lh - 12) / rowh;
    int total = (a->g.n + (a->g.start.side == CH_BLACK ? 1 : 0) + 1) / 2;
    int first = LM_MAX(0, total - rows);
    for (int r = first; r < total; r++) {
        char line[64];
        format_moves(a, r, line, sizeof(line));
        bool newest = r == total - 1;
        wlwin_text(w, x + 12, ly + 8 + (r - first) * rowh, line, 1, newest ? lm_theme.text_bright : COL_TEXT, pw - 20);
    }
    if (a->g.n == 0)
        wlwin_text(w, x + 12, ly + 8, "The moves will be here", 1, COL_DIM, pw - 20);
    /* what the computer is doing */
    if (a->tk.started) {
        char t[64];
        int d = __atomic_load_n(&a->tk.depth, __ATOMIC_RELAXED);
        snprintf(t, sizeof(t), a->tk.mode ? "Looking... depth %d" : "Thinking... depth %d", d);
        wlwin_text_bold(w, x + 12, ly + lh - LM_FONT_H - 8, t, 1, COL_ACCENT, pw - 20);
    }
}

static void paint_status(struct app *a)
{
    struct wlwin *w = &a->win;
    int y = w->bh - STAT_H;
    wlwin_fill(w, 0, y, w->bw, STAT_H, COL_BAR);
    char line[200];
    int s = status(a);
    if (s != CH_ONGOING)
        snprintf(line, sizeof(line), "%s", result_text(a, s));
    else if (a->tk.started && a->tk.mode == 0)
        snprintf(line, sizeof(line), "The computer is thinking ...");
    else if (a->human < 0)
        snprintf(line, sizeof(line), "%s to move%s", a->g.pos.side == CH_WHITE ? "White" : "Black", ch_in_check(&a->g.pos) ? " - check" : "");
    else
        snprintf(line, sizeof(line), "%s%s", human_turn(a) ? "Your move" : "The computer's move", ch_in_check(&a->g.pos) ? " - check" : "");
    wlwin_text(w, WLRAIL_W + 12, y + (STAT_H - LM_FONT_H) / 2, line, 1, COL_TEXT, w->bw - WLRAIL_W - 24);
    char right[120];
    struct lay Lay;
    layout(a, &Lay);
    if (a->clk.on && !Lay.panel) {                          /* no room for the clocks beside the board: they go here */
        char tw[24], tb[24];
        fmt_clock(a->clk.left[CH_WHITE], tw, sizeof(tw));
        fmt_clock(a->clk.left[CH_BLACK], tb, sizeof(tb));
        snprintf(right, sizeof(right), "White %s   Black %s", tw, tb);
    } else {
        snprintf(right, sizeof(right), "Level %s   Won %d  Drawn %d  Lost %d", level_name(a->level), a->st.won, a->st.drawn, a->st.lost);
    }
    int rw = lm_font_text_width(right);
    if (a->msg[0] && now_ms() - a->msg_ms < 6000)
        wlwin_text_bold(w, WLRAIL_W + 12, y - 24, a->msg, 1, 0xfff3bfu, w->bw - WLRAIL_W - 24);
    else if (w->bw - WLRAIL_W - 24 > rw + lm_font_text_width(line) + 20)
        wlwin_text(w, w->bw - rw - 12, y + (STAT_H - LM_FONT_H) / 2, right, 1, COL_DIM, rw);
}

/* ---- the rail ---- */

static void ic_undo(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    for (int i = 0; i < 5; i++)                                 /* an arrow head pointing left */
        wlwin_fill(w, cx - 7 + i, cy - i, 1, 2 * i + 1, c);
    wlwin_fill(w, cx - 3, cy - 1, 10, 3, c);
}

static void ic_flip(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    for (int i = 0; i < 4; i++) {                               /* two arrows, one up and one down */
        wlwin_fill(w, cx - 4 - i, cy - 7 + i, 2 * i + 1, 1, c);
        wlwin_fill(w, cx + 4 - i, cy + 7 - i, 2 * i + 1, 1, c);
    }
    wlwin_fill(w, cx - 5, cy - 3, 3, 10, c);
    wlwin_fill(w, cx + 3, cy - 7, 3, 10, c);
}

static void ic_clock(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    wlwin_round_fill(w, cx - 7, cy - 7, 14, 14, 7, c);          /* a ring with two hands */
    wlwin_round_fill(w, cx - 5, cy - 5, 10, 10, 5, bg);
    wlwin_fill(w, cx, cy - 4, 1, 5, c);
    wlwin_fill(w, cx, cy, 4, 1, c);
}

static void rails(const struct app *a, struct wlrail *L)
{
    static char clock_tip[64];
    snprintf(clock_tip, sizeof(clock_tip), "Clock: %s  C", a->cli_base >= 0 ? "set on the command line" : k_clocks[a->st.clock].name);
    const char *tips[B_COUNT] = { "New game  N", "Take a move back  U", "A hint  H", "Turn the board  F", clock_tip, "Level  L (or 0 .. 9)", "Keys  F1" };
    static void (*const icons[B_COUNT])(struct wlwin *, int, int, uint32_t, uint32_t) = { NULL, ic_undo, NULL, ic_flip, ic_clock, NULL, NULL };
    const uint32_t base[B_COUNT] = { lm_theme.btn_green, lm_theme.btn_amber, lm_theme.btn_teal, lm_theme.btn_slate, lm_theme.btn_blue, lm_theme.btn_purple, lm_theme.btn_rose };
    const uint32_t lite[B_COUNT] = { lm_theme.btn_green_lit, lm_theme.btn_amber_lit, lm_theme.btn_teal_lit, lm_theme.btn_slate_lit, lm_theme.btn_blue_lit, lm_theme.btn_purple_lit, lm_theme.btn_rose_lit };
    static char lv[4];
    if (a->level == 0)
        snprintf(lv, sizeof(lv), "2P");
    else
        snprintf(lv, sizeof(lv), "%d", a->level);
    memset(L, 0, sizeof(*L));
    L->x = 0;
    L->y0 = 0;
    L->y1 = a->win.bh > 0 ? a->win.bh - STAT_H : 400;
    L->n = B_COUNT;
    for (int i = 0; i < B_COUNT; i++) {
        bool idle = (i == B_UNDO && a->g.n == 0) || (i == B_HINT && (status(a) != CH_ONGOING || !human_turn(a)));
        L->it[i] = (struct wlrail_item){ .tip = tips[i], .icon = icons[i], .text = i == B_NEW ? "N" : i == B_HINT ? "H" : i == B_LEVEL ? lv : i == B_HELP ? "?" : NULL,
                                         .base = base[i], .lite = lite[i], .show = true, .idle = idle, .lit = (i == B_FLIP && a->flipped) || (i == B_CLOCK && a->clk.on),
                                         .gap = i == B_FLIP || i == B_LEVEL, .bottom = i == B_HELP };
    }
}

static void press_button(struct app *a, int i);

/* the clock for the next game; before the first move it is the clock of this game too */
static void step_clock(struct app *a)
{
    if (a->cli_base >= 0) {
        say(a, "The clock was set on the command line (--clock-seconds)");
        return;
    }
    a->st.clock = (a->st.clock + 1) % N_CLOCKS;
    stats_save(&a->st);
    if (a->g.n == 0 && a->timeout < 0) {
        clock_reset(a);
        say(a, "Clock: %s", k_clocks[a->st.clock].name);
    } else {
        say(a, "Clock: %s, from the next game", k_clocks[a->st.clock].name);
    }
    a->win.redraw = true;
}

/* ---- the cards ---- */

static void card_rect(const struct app *a, struct wlcard *c, int *nb)
{
    switch (a->prompt) {
    case P_NEW: *nb = 3; c->h = 150; c->w = 440; break;
    case P_PROMO: *nb = 4; c->h = 130; c->w = 440; break;
    case P_OVER: *nb = 2; c->h = 150; c->w = 440; break;
    default: *nb = 1; c->h = 350; c->w = 560; break;
    }
    c->w = LM_MIN(c->w, a->win.bw - 16);
    c->h = LM_MIN(c->h, a->win.bh - 16);
    c->x = (a->win.bw - c->w) / 2;
    c->y = (a->win.bh - c->h) / 2;
}

static const char *const HELP_LINES[] = {
    "Drag a piece to where it goes, or click the piece and then the square.",
    "The dots show where the selected piece can go.",
    "The arrow keys move a frame over the board; Enter or Space picks up a",
    "piece and puts it down.  Esc lets go.",
    "N new game (and the side you play), U or Backspace takes a move back,",
    "H asks for a hint, F turns the board round, L steps through the levels,",
    "0 is two players and 1 .. 9 the computer at that level. C steps the clock",
    "(it starts after both sides have moved once; the side that runs out loses).",
    "A pawn that reaches the far side becomes a Queen, Rook, Bishop or Knight",
    "(Q R B N). The computer thinks while you can still look around.",
};

static void paint_card(struct app *a)
{
    struct wlwin *w = &a->win;
    struct wlcard c;
    int nb;
    card_rect(a, &c, &nb);
    const char *title = a->prompt == P_NEW ? "New game" : a->prompt == P_PROMO ? "Promote the pawn" : a->prompt == P_OVER ? "Game over" : "Keys";
    wlcard_frame(w, &c, c.w, c.h, title, COL_ACCENT, lm_theme.background, lm_theme.text_bright);
    char line[160] = "", line2[160] = "";
    const char *labels[4] = { "Cancel", "OK", "", "" };
    uint32_t cols[4] = { lm_theme.btn_grey, lm_theme.btn_blue, lm_theme.btn_blue, lm_theme.btn_blue };
    if (a->prompt == P_NEW) {
        snprintf(line, sizeof(line), "Start a new game against level %d (%s)?", a->level, level_name(a->level));
        if (a->level == 0)
            snprintf(line, sizeof(line), "Start a new game for two players?");
        snprintf(line2, sizeof(line2), "Clock: %s.  The side you play is chosen with the button.", a->cli_base >= 0 ? "as set on the command line" : k_clocks[a->st.clock].name);
        labels[0] = "Cancel";
        labels[1] = "As Black";
        labels[2] = "As White";
        cols[1] = lm_theme.btn_slate;
        cols[2] = lm_theme.btn_green;
    } else if (a->prompt == P_PROMO) {
        snprintf(line, sizeof(line), "Choose the piece (or press Q, R, B, N).");
        labels[0] = "Knight";
        labels[1] = "Bishop";
        labels[2] = "Rook";
        labels[3] = "Queen";
        cols[0] = lm_theme.btn_teal;
        cols[1] = lm_theme.btn_purple;
        cols[2] = lm_theme.btn_amber;
        cols[3] = lm_theme.btn_green;
    } else if (a->prompt == P_OVER) {
        snprintf(line, sizeof(line), "%s.", result_text(a, status(a)));
        snprintf(line2, sizeof(line2), "%d moves.  Played %d, won %d, drawn %d, lost %d.", (a->g.n + 1) / 2, a->st.played, a->st.won, a->st.drawn, a->st.lost);
        labels[0] = "Close";
        labels[1] = "Play again";
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
    wlcard_buttons(w, &c, labels, nb, cols, a->pc_hover, a->pc_down, lm_theme.on_colour);
}

static void card_accept(struct app *a, int which)
{
    if (a->prompt == P_HELP) {
        a->prompt = P_NONE;
        return;
    }
    if (a->prompt == P_NEW) {
        if (which == 0) {
            a->prompt = P_NONE;
            return;
        }
        new_game(a, which == 1 ? CH_BLACK : CH_WHITE);
        return;
    }
    if (a->prompt == P_PROMO) {
        static const int pieces[4] = { CH_KNIGHT, CH_BISHOP, CH_ROOK, CH_QUEEN };
        int from = a->promo_from, to = a->promo_to;
        a->prompt = P_NONE;
        play(a, ch_move_find(&a->g.pos, from, to, pieces[which & 3]));
        return;
    }
    /* the game is over */
    if (which == 0) {
        a->prompt = P_NONE;
        return;
    }
    new_game(a, a->human < 0 ? CH_WHITE : a->human);
}

/* ---- paint and the dump ---- */

static const char *status_word(int s)
{
    switch ((int)s) {
    case CH_ONGOING: return "ongoing";
    case CH_CHECKMATE: return "checkmate";
    case CH_STALEMATE: return "stalemate";
    case CH_DRAW_MATERIAL: return "material";
    case CH_DRAW_REPETITION: return "repetition";
    case CH_DRAW_FIFTY: return "fifty";
    case ST_FLAG: return "timeout";
    case ST_FLAG_DRAW: return "timeout-draw";
    }
    return "?";
}

static void write_dump(struct app *a)
{
    if (!g_dump)
        return;
    FILE *f = fopen(g_dump, "w");
    if (!f)
        return;
    char fen[128], sel[4] = "-", hint[8] = "-", last[8] = "-";
    ch_pos_fen(&a->g.pos, fen, sizeof(fen));
    if (a->sel >= 0)
        sq_name(a->sel, sel);
    if (a->hint)
        ch_move_uci(a->hint, hint);
    if (a->g.n > 0)
        ch_move_uci(a->g.ply[a->g.n - 1].move, last);
    char kb[4] = "-";
    if (a->kb >= 0)
        sq_name(a->kb, kb);
    fprintf(f, "state turn=%c human=%c level=%d flipped=%d sel=%s kb=%s thinking=%d plies=%d status=%s prompt=%d dragging=%d hint=%s last=%s "
               "played=%d won=%d drawn=%d lost=%d check=%d depth=%d\n",
            a->g.pos.side == CH_WHITE ? 'w' : 'b', a->human < 0 ? '-' : a->human == CH_WHITE ? 'w' : 'b', a->level, a->flipped, sel, kb,
            a->tk.started && a->tk.mode == 0, a->g.n, status_word(status(a)), (int)a->prompt, a->dragging, hint, last, a->st.played, a->st.won,
            a->st.drawn, a->st.lost, ch_in_check(&a->g.pos), a->tk.depth);
    fprintf(f, "fen %s\n", fen);
    fprintf(f, "clock on=%d white=%lld black=%lld inc=%lld running=%c preset=%d\n", a->clk.on, (long long)a->clk.left[CH_WHITE],
            (long long)a->clk.left[CH_BLACK], (long long)a->clk.inc_ms, clock_running(a) ? (a->g.pos.side == CH_WHITE ? 'w' : 'b') : '-', a->st.clock);
    struct lay L;
    layout(a, &L);
    fprintf(f, "lay cs=%d bx=%d by=%d panel=%d\n", L.cs, L.bx, L.by, L.panel);
    for (int r = 7; r >= 0; r--) {
        char row[9];
        for (int fl = 0; fl < 8; fl++) {
            uint8_t pc = a->g.pos.sq[r * 8 + fl];
            row[fl] = pc ? (CH_COLOR(pc) == CH_WHITE ? (char)toupper("?pnbrqk"[CH_TYPE(pc)]) : "?pnbrqk"[CH_TYPE(pc)]) : '.';
        }
        row[8] = 0;
        fprintf(f, "rank %d %s\n", r + 1, row);
    }
    fprintf(f, "legal");
    for (int i = 0; i < a->nlegal; i++) {
        char u[8];
        ch_move_uci(a->legal[i], u);
        fprintf(f, " %s", u);
    }
    fprintf(f, "\n");
    for (int i = 0; i < a->g.n; i++)
        fprintf(f, "san %d %s\n", i + 1, a->g.ply[i].san);
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
    static const char *const names[B_COUNT] = { "New", "Undo", "Hint", "Flip", "Clock", "Level", "Help" };
    for (int i = 0; i < R.n; i++)
        if (ys[i] >= 0)
            fprintf(f, "btn %s x=%d y=%d w=%d h=%d idle=%d\n", names[i], R.x + (WLRAIL_W - WLRAIL_D) / 2, ys[i], WLRAIL_D, WLRAIL_D, R.it[i].idle);
    fclose(f);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    wlwin_fill(w, 0, 0, w->bw, w->bh, lm_theme.background);
    struct lay L;
    layout(a, &L);
    paint_board(a, &L);
    paint_panel(a, &L);
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
        a->prompt = P_NEW;
        a->pc_hover = a->pc_down = -1;
        break;
    case B_UNDO:
        undo(a);
        break;
    case B_HINT:
        ask_hint(a);
        break;
    case B_FLIP:
        a->flipped = !a->flipped;
        break;
    case B_CLOCK:
        step_clock(a);
        break;
    case B_LEVEL:
        set_level(a, a->level == chai_level_count() ? 0 : a->level + 1);       /* 1 .. 9, then two players, then 1 again */
        break;
    case B_HELP:
        a->prompt = P_HELP;
        a->pc_hover = a->pc_down = -1;
        break;
    default:
        break;
    }
    a->win.redraw = true;
}

/* the person tries to move from -> to */
static void try_move(struct app *a, int from, int to)
{
    if (!is_legal_to(a, from, to)) {
        deselect(a);
        return;
    }
    if (is_promotion(a, from, to)) {
        a->promo_from = from;
        a->promo_to = to;
        a->prompt = P_PROMO;
        a->pc_hover = a->pc_down = -1;
        a->pressed = a->dragging = false;
        a->win.redraw = true;
        return;
    }
    play(a, ch_move_find(&a->g.pos, from, to, 0));
}

static bool own_piece(const struct app *a, int sq)
{
    uint8_t pc = a->g.pos.sq[sq];
    return pc && CH_COLOR(pc) == a->g.pos.side && human_turn(a) && status(a) == CH_ONGOING && !(a->tk.started && a->tk.mode == 0);
}

static void board_press(struct app *a, int sq)
{
    if (a->tk.started && a->tk.mode == 0)
        return;                                             /* the computer's turn */
    if (sq >= 0 && a->sel >= 0 && sq != a->sel && is_legal_to(a, a->sel, sq)) {
        try_move(a, a->sel, sq);
        return;
    }
    if (sq >= 0 && own_piece(a, sq)) {
        a->was_selected = sq == a->sel;
        a->sel = sq;
        a->drag_from = sq;
        a->pressed = true;
        a->dragging = false;
        a->press_x = a->px;
        a->press_y = a->py;
        a->kb = -1;                                         /* (the keyboard's frame is for the keyboard) */
        return;
    }
    deselect(a);
}

static void board_release(struct app *a, int sq)
{
    if (!a->pressed)
        return;
    bool dragging = a->dragging;
    int from = a->drag_from;
    a->pressed = a->dragging = false;
    if (dragging) {
        if (sq >= 0 && sq != from)
            try_move(a, from, sq);
        else
            a->sel = from;
    } else if (a->was_selected && sq == from) {
        a->sel = -1;                                        /* a click on the selected piece lets go of it */
    }
}

static void card_pointer(struct app *a, uint32_t op, const union lm_arg *args)
{
    struct wlcard c;
    int nb;
    card_rect(a, &c, &nb);
    if (op == LM_WL_POINTER_EVT_MOTION) {
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
        else if (!wlcard_has(&c, (int)a->px, (int)a->py) && a->prompt != P_OVER && a->prompt != P_PROMO)
            a->prompt = P_NONE;
    } else {
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
    default:
        break;
    }
    if (a->prompt != P_NONE) {
        card_pointer(a, op, args);
        return;
    }
    struct wlrail L;
    rails(a, &L);
    struct lay Ly;
    layout(a, &Ly);
    if (op == LM_WL_POINTER_EVT_MOTION) {
        a->hover = wlrail_at(&L, a->px, a->py);
        if (a->pressed && !a->dragging && hypot(a->px - a->press_x, a->py - a->press_y) > DRAG_PX)
            a->dragging = true;
        w->redraw = true;
        return;
    }
    if (op != LM_WL_POINTER_EVT_BUTTON)
        return;
    uint32_t button = args[2].u, state = args[3].u;
    if (state != 1) {
        wlrail_release(&a->ran);
        if (button == 272)
            board_release(a, sq_at(&Ly, a->flipped, a->px, a->py));
        w->redraw = true;
        return;
    }
    int bi = wlrail_at(&L, a->px, a->py);
    if (button == 272 && bi >= 0) {
        wlrail_press(&a->ran, bi, now_ms());
        if (!L.it[bi].idle)
            press_button(a, bi);
        return;
    }
    if (button == 272)
        board_press(a, sq_at(&Ly, a->flipped, a->px, a->py));
    else if (button == 273) {
        deselect(a);
        a->hint = 0;
    }
    w->redraw = true;
}

static void move_kb(struct app *a, int df, int dr)
{
    if (a->kb < 0)
        a->kb = a->human == CH_BLACK ? 52 : 12;             /* e2 / e7: where the king's pawn stands */
    int f = a->kb & 7, r = a->kb >> 3;
    if (a->flipped) {
        df = -df;
        dr = -dr;
    }
    f = LM_CLAMP(f + df, 0, 7);
    r = LM_CLAMP(r + dr, 0, 7);
    a->kb = r * 8 + f;
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op != LM_WL_KEYBOARD_EVT_KEY || args[3].u == 0)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, w->mods);
    bool ctrl = (w->mods & LM_MOD_CTRL) != 0;
    char txt[8];
    int tl = lm_keysym_to_utf8(ks, txt);
    char c = tl == 1 ? (char)(txt[0] >= 'A' && txt[0] <= 'Z' ? txt[0] + 32 : txt[0]) : 0;
    w->redraw = true;
    if (a->prompt != P_NONE) {
        if (a->prompt == P_PROMO) {
            int k = c == 'n' ? 0 : c == 'b' ? 1 : c == 'r' ? 2 : c == 'q' || ks == LM_KS_Return ? 3 : -1;
            if (k >= 0)
                card_accept(a, k);
            else if (ks == LM_KS_Escape)
                a->prompt = P_NONE;
            return;
        }
        if (ks == LM_KS_Escape) {
            a->prompt = P_NONE;
        } else if (a->prompt == P_NEW) {
            if (c == 'w')
                card_accept(a, 2);
            else if (c == 'b')
                card_accept(a, 1);
            else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
                card_accept(a, (a->human == CH_BLACK) ? 1 : 2);
            else if (c == 'n')
                a->prompt = P_NONE;
        } else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter || c == 'n') {
            card_accept(a, a->prompt == P_HELP ? 0 : 1);
        }
        return;
    }
    if (ctrl) {
        if (c == 'q')
            w->running = false;
        else if (c == 'n')
            press_button(a, B_NEW);
        else if (c == 'z')
            undo(a);
        return;
    }
    if (ks == LM_KS_F1) {
        a->prompt = P_HELP;
        a->pc_hover = a->pc_down = -1;
        return;
    }
    if (c == 'n') { press_button(a, B_NEW); return; }
    if (c == 'u' || ks == LM_KS_BackSpace) { undo(a); return; }
    if (c == 'h') { ask_hint(a); return; }
    if (c == 'f') { a->flipped = !a->flipped; return; }
    if (c == 'l') { press_button(a, B_LEVEL); return; }
    if (c == 'c') { press_button(a, B_CLOCK); return; }
    if (c >= '0' && c <= '9') { set_level(a, c - '0'); return; }
    if (ks == LM_KS_Left) { move_kb(a, -1, 0); return; }
    if (ks == LM_KS_Right) { move_kb(a, 1, 0); return; }
    if (ks == LM_KS_Up) { move_kb(a, 0, 1); return; }
    if (ks == LM_KS_Down) { move_kb(a, 0, -1); return; }
    if (ks == LM_KS_Escape) {
        deselect(a);
        a->hint = 0;
        return;
    }
    if ((ks == LM_KS_Return || ks == LM_KS_KP_Enter || c == ' ') && a->kb >= 0) {
        if (a->tk.started && a->tk.mode == 0)
            return;
        if (a->sel >= 0 && a->kb == a->sel)
            a->sel = -1;
        else if (a->sel >= 0 && is_legal_to(a, a->sel, a->kb))
            try_move(a, a->sel, a->kb);
        else if (own_piece(a, a->kb))
            a->sel = a->kb;
        else
            deselect(a);
    }
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

/* ---- the pieces on a board, to look at ---- */

static int write_sheet(const char *path, int size)
{
    size = LM_CLAMP(size, 24, 400);
    int W = 6 * size, H = 2 * size;
    uint8_t *img = malloc((size_t)W * (size_t)H * 3);
    if (!img)
        return 1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t bg = (((x / size) + (y / size)) & 1) ? SQ_DARK : SQ_LIGHT;
            uint8_t *d = &img[((size_t)y * (size_t)W + (size_t)x) * 3];
            d[0] = (uint8_t)(bg >> 16);
            d[1] = (uint8_t)(bg >> 8);
            d[2] = (uint8_t)bg;
        }
    for (int c = 0; c < 2; c++)
        for (int t = 1; t <= 6; t++) {
            uint32_t *p = chessart_piece(t, c, size);
            if (!p)
                continue;
            for (int y = 0; y < size; y++)
                for (int x = 0; x < size; x++) {
                    uint32_t v = p[y * size + x];
                    float al = (float)(v >> 24) / 255.0f;
                    uint8_t *d = &img[((size_t)(c * size + y) * (size_t)W + (size_t)((t - 1) * size + x)) * 3];
                    d[0] = (uint8_t)((float)d[0] * (1 - al) + (float)((v >> 16) & 255) * al);
                    d[1] = (uint8_t)((float)d[1] * (1 - al) + (float)((v >> 8) & 255) * al);
                    d[2] = (uint8_t)((float)d[2] * (1 - al) + (float)(v & 255) * al);
                }
            free(p);
        }
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(img);
        return 1;
    }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    fwrite(img, 1, (size_t)W * (size_t)H * 3, f);
    fclose(f);
    free(img);
    return 0;
}

/* ---- the self test: the pieces, the board's geometry, a game against the engine ---- */

static int selftest(void)
{
    int bad = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "selftest: " __VA_ARGS__); fputc('\n', stderr); bad++; } } while (0)

    /* every piece has a body, the white and the black one differ, no two kinds look alike, and the corners are see-through */
    uint32_t *pic[2][7];
    for (int c = 0; c < 2; c++)
        for (int t = 1; t <= 6; t++) {
            pic[c][t] = chessart_piece(t, c, 96);
            CHECK(pic[c][t] != NULL, "a piece picture was not made");
            if (!pic[c][t])
                return 1;
            int solid = 0;
            for (int i = 0; i < 96 * 96; i++)
                solid += (pic[c][t][i] >> 24) == 255;
            CHECK(solid > 96 * 96 / 8 && solid < 96 * 96 / 2, "piece %d colour %d covers %d pixels", t, c, solid);
            CHECK((pic[c][t][0] >> 24) == 0 && (pic[c][t][96 * 96 - 1] >> 24) == 0, "the corner of piece %d is not see-through", t);
        }
    for (int t = 1; t <= 6; t++) {
        CHECK(memcmp(pic[0][t], pic[1][t], 96 * 96 * 4) != 0, "white and black %d are the same", t);
        for (int u = t + 1; u <= 6; u++) {
            int diff = 0;
            for (int i = 0; i < 96 * 96; i++)
                diff += (pic[0][t][i] >> 24) != (pic[0][u][i] >> 24);
            CHECK(diff > 400, "pieces %d and %d have almost the same shape (%d pixels differ)", t, u, diff);
        }
    }
    /* a piece is lit from the top: the upper part of a white pawn is lighter than the lower part */
    {
        long top = 0, bot = 0;
        int nt = 0, nb = 0;
        for (int y = 0; y < 96; y++)
            for (int x = 0; x < 96; x++) {
                uint32_t v = pic[0][1][y * 96 + x];
                if ((v >> 24) < 255)
                    continue;
                int lum = (int)(((v >> 16) & 255) + ((v >> 8) & 255) + (v & 255));
                if (y >= 16 && y < 28) { top += lum; nt++; }
                if (y >= 62 && y < 74) { bot += lum; nb++; }
            }
        CHECK(nt && nb && top / nt > bot / nb, "the shading of a pawn");
    }
    for (int c = 0; c < 2; c++)
        for (int t = 1; t <= 6; t++)
            free(pic[c][t]);

    /* the board's geometry in both directions */
    {
        struct lay L = { .cs = 50, .bx = 100, .by = 20 };
        for (int flipped = 0; flipped < 2; flipped++)
            for (int sq = 0; sq < 64; sq++) {
                int x, y;
                sq_xy(&L, flipped, sq, &x, &y);
                CHECK(sq_at(&L, flipped, x + 25, y + 25) == sq, "square %d (flipped %d) is not found again", sq, flipped);
            }
        int x, y;
        sq_xy(&L, false, 0, &x, &y);                          /* a1 is at the lower left for White, the upper right for Black */
        CHECK(x == 100 && y == 20 + 7 * 50, "a1 for White");
        sq_xy(&L, true, 0, &x, &y);
        CHECK(x == 100 + 7 * 50 && y == 20, "a1 for Black");
        CHECK(sq_at(&L, false, 99, 100) == -1 && sq_at(&L, false, 100 + 400, 100) == -1 && sq_at(&L, false, 150, 19) == -1, "outside the board");
    }

    /* the engine plays a few legal moves at the lowest level, in the same code that the window uses */
    {
        struct app a;
        memset(&a, 0, sizeof(a));
        a.ai = chai_new(4);
        a.level = 1;
        a.human = CH_WHITE;
        a.sel = a.kb = -1;
        a.timeout = -1;
        ch_game_init(&a.g, NULL, NULL);
        refresh_legal(&a);
        CHECK(a.nlegal == 20, "twenty moves at the start");
        CHECK(play(&a, ch_move_from_uci(&a.g.pos, "e2e4")), "e2e4 was refused");
        CHECK(!play(&a, ch_move_from_uci(&a.g.pos, "e2e4")), "an illegal move was played");
        CHECK(engine_to_move(&a) && !human_turn(&a), "it is the engine's move");
        for (int i = 0; i < 6; i++) {
            struct think *t = &a.tk;
            think_start(&a, 0);
            pthread_join(t->th, NULL);
            t->started = false;
            free(t->hist);
            t->hist = NULL;
            CHECK(t->res.best && ch_move_find(&a.g.pos, CH_FROM(t->res.best), CH_TO(t->res.best), 0) == t->res.best, "the engine's move is not legal");
            if (!t->res.best || !play(&a, t->res.best))
                break;
            if (!human_turn(&a))
                break;
            ch_move mv[CH_MAX_MOVES];
            int n = ch_gen_legal(&a.g.pos, mv);
            if (!n || !play(&a, mv[0]))
                break;
        }
        CHECK(a.g.n >= 4, "the game went on (%d plies)", a.g.n);
        int n = a.g.n;
        a.human = CH_WHITE;
        undo(&a);
        CHECK(a.g.n < n, "undo took a move back");
        think_cancel(&a);
        ch_game_free(&a.g);
        chai_free(a.ai);
    }

    /* the clock: how it is written, who could have mated, and how the time is counted */
    {
        char t[24];
        static const struct { int64_t ms; const char *want; } fm[] = {
            { 300000, "5:00" }, { 59000, "0:59" }, { 19900, "19.9" }, { 20000, "0:20" }, { 3723000, "1:02:03" }, { -5, "0.0" }, { 9400, "9.4" } };
        for (size_t i = 0; i < sizeof(fm) / sizeof(fm[0]); i++) {
            fmt_clock(fm[i].ms, t, sizeof(t));
            CHECK(!strcmp(t, fm[i].want), "fmt_clock(%lld) = %s, wanted %s", (long long)fm[i].ms, t, fm[i].want);
        }
        static const struct { const char *fen; bool white_mates; } mate[] = {
            { "4k3/8/8/8/8/8/8/4K3 w - - 0 1", false }, { "4k3/8/8/8/8/8/8/3BK3 w - - 0 1", false }, { "4k3/8/8/8/8/8/8/3NK3 w - - 0 1", false },
            { "4k3/8/8/8/8/8/8/2NNK3 w - - 0 1", true }, { "4k3/8/8/8/8/8/8/2BBK3 w - - 0 1", true }, { "4k3/8/8/8/8/8/8/B1B1K3 w - - 0 1", false },
            { "4k3/8/8/8/8/8/P7/4K3 w - - 0 1", true }, { "4k3/8/8/8/8/8/8/R3K3 w - - 0 1", true } };
        for (size_t i = 0; i < sizeof(mate) / sizeof(mate[0]); i++) {
            struct ch_pos p;
            CHECK(ch_pos_set_fen(&p, mate[i].fen, NULL) == 0, "FEN %s", mate[i].fen);
            CHECK(could_mate(&p, CH_WHITE) == mate[i].white_mates, "could_mate(%s) is not %d", mate[i].fen, mate[i].white_mates);
        }
        struct app a;
        memset(&a, 0, sizeof(a));
        a.ai = chai_new(1);
        a.level = 0;
        a.human = -1;
        a.sel = a.kb = -1;
        a.timeout = -1;
        a.cli_base = 60;
        a.cli_inc = 2;
        ch_game_init(&a.g, NULL, NULL);
        clock_reset(&a);
        refresh_legal(&a);
        CHECK(a.clk.on && a.clk.left[0] == 60000 && a.clk.inc_ms == 2000, "the clock starts with 60 s and 2 s");
        a.clk.last -= 3000;
        clock_advance(&a, now_ms());
        CHECK(a.clk.left[0] == 60000 && !clock_running(&a), "nothing runs before a move");
        play(&a, ch_move_from_uci(&a.g.pos, "e2e4"));
        play(&a, ch_move_from_uci(&a.g.pos, "e7e5"));
        CHECK(a.clk.left[0] == 60000 && a.clk.left[1] == 60000 && clock_running(&a), "the first two moves are free and then White's clock runs");
        a.clk.last = now_ms() - 1500;
        clock_advance(&a, now_ms());
        CHECK(a.clk.left[0] >= 58000 && a.clk.left[0] <= 58600 && a.clk.left[1] == 60000, "White's clock ran: %lld / %lld", (long long)a.clk.left[0], (long long)a.clk.left[1]);
        a.clk.last = now_ms() - 5000;                          /* a step of 5 s (the process was stopped) counts as 2 */
        int64_t before = a.clk.left[0];
        clock_advance(&a, now_ms());
        CHECK(before - a.clk.left[0] >= 1990 && before - a.clk.left[0] <= 2100, "a long step counts as 2 s: %lld", (long long)(before - a.clk.left[0]));
        play(&a, ch_move_from_uci(&a.g.pos, "g1f3"));
        CHECK(a.clk.left[0] > 58000 && a.clk.left[0] < 59000 && a.clk.left[1] == 60000, "the increment is added after the third move (60 - 1.5 - 2 + 2 s): %lld", (long long)a.clk.left[0]);
        CHECK(a.g.pos.side == CH_BLACK && clock_running(&a), "now Black's clock");
        undo(&a);
        CHECK(a.g.n == 2 && a.clk.left[0] == 60000 && a.clk.left[1] == 60000, "undo gives the clocks back: %lld / %lld", (long long)a.clk.left[0], (long long)a.clk.left[1]);
        a.clk.left[CH_WHITE] = 0;
        clock_check_flag(&a);
        CHECK(a.timeout == CH_WHITE && status(&a) == ST_FLAG && a.prompt == P_OVER, "White's time ran out");
        undo(&a);
        CHECK(a.timeout == -1 && status(&a) == CH_ONGOING, "undo takes a lost game up again");
        ch_game_free(&a.g);
        /* time out against a side that could not have mated is a draw */
        ch_game_init(&a.g, "8/8/4k3/8/8/3K4/6p1/7B b - - 0 1", NULL);
        a.timeout = -1;
        a.prompt = P_NONE;
        a.clk.left[CH_BLACK] = 0;
        play(&a, ch_move_from_uci(&a.g.pos, "e6e5"));
        play(&a, ch_move_from_uci(&a.g.pos, "d3c3"));
        a.clk.left[CH_BLACK] = 0;
        clock_check_flag(&a);
        CHECK(status(&a) == ST_FLAG_DRAW && a.timeout == CH_BLACK, "a bare bishop cannot mate: a draw (%d)", status(&a));
        ch_game_free(&a.g);
        free(a.snap);
        chai_free(a.ai);
    }
    if (!bad)
        printf("selftest ok\n");
    return bad ? 1 : 0;
}

/* ---- main ---- */

int main(int argc, char **argv)
{
    wlwin_hidpi();
    lm_theme_load_env();
    signal(SIGPIPE, SIG_IGN);
    int level = -1, win_w = 900, win_h = 680, delay = 350;
    int side_arg = -1, cli_base = -1, cli_inc = 0;
    const char *fen = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest"))
            return selftest();
        if (!strcmp(argv[i], "--sheet") && i + 1 < argc) {
            const char *path = argv[++i];
            int size = i + 1 < argc ? atoi(argv[++i]) : 120;
            return write_sheet(path, size);
        }
        if (!strcmp(argv[i], "--level") && i + 1 < argc) {
            level = atoi(argv[++i]);
            if (level < 0 || level > chai_level_count()) {
                fprintf(stderr, "mellagui-chess: --level wants 0 (two players) to %d\n", chai_level_count());
                return 2;
            }
        } else if (!strcmp(argv[i], "--black")) {
            side_arg = CH_BLACK;
        } else if (!strcmp(argv[i], "--white")) {
            side_arg = CH_WHITE;
        } else if (!strcmp(argv[i], "--fen") && i + 1 < argc) {
            fen = argv[++i];
            struct ch_pos probe;
            const char *err = NULL;
            if (ch_pos_set_fen(&probe, fen, &err) < 0) {
                fprintf(stderr, "mellagui-chess: bad FEN: %s\n", err);
                return 2;
            }
        } else if (!strcmp(argv[i], "--clock-seconds") && i + 1 < argc) {
            int base, inc = 0;
            char junk;
            const char *v = argv[++i];
            if ((sscanf(v, "%d+%d%c", &base, &inc, &junk) != 2 && sscanf(v, "%d%c", &base, &junk) != 1) || base < 1 || base > 36000 || inc < 0 || inc > 3600) {
                fprintf(stderr, "mellagui-chess: --clock-seconds wants BASE+INCREMENT in seconds, like 300+3 (or 60)\n");
                return 2;
            }
            cli_base = base;
            cli_inc = inc;
        } else if (!strcmp(argv[i], "--delay") && i + 1 < argc) {
            delay = atoi(argv[++i]);                         /* (not inside LM_CLAMP: a macro that evaluates its argument three times) */
            delay = LM_CLAMP(delay, 0, 5000);
        } else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            char junk;
            if (sscanf(argv[++i], "%dx%d%c", &win_w, &win_h, &junk) != 2 || win_w < 300 || win_h < 300) {
                fprintf(stderr, "mellagui-chess: --size wants a size like 900x680\n");
                return 2;
            }
        } else {
            fprintf(stderr, "usage: mellagui-chess [--level 0..%d] [--black] [--fen FEN] [--clock-seconds BASE+INC] [--size WxH] [--delay MS] | --selftest | --sheet FILE.ppm [SIZE]\n",
                    chai_level_count());
            return 2;
        }
    }
    g_dump = getenv("MELLAGUI_CHESS_DUMP");
    static struct app a;
    stats_load(&a.st);
    if (level >= 0)
        a.st.level = level;
    a.level = a.st.level;
    a.delay_ms = delay;
    a.cli_base = cli_base;
    a.cli_inc = cli_inc;
    a.timeout = -1;
    a.start_fen = fen;
    a.hover = -1;
    a.sel = a.kb = -1;
    a.pc_hover = a.pc_down = -1;
    a.ai = chai_new(32);
    if (!a.ai)
        return 1;
    wlrail_init(&a.ran);
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event, .close = close_event };
    if (wlwin_connect(&a.win, "mellagui-chess", &ops, &a) < 0)
        return 1;
    if (wlwin_open(&a.win, "Chess", "mellagui.chess", win_w, win_h, 460, 380) < 0)
        return 1;
    new_game(&a, side_arg >= 0 ? side_arg : a.st.human);
    while (a.win.running) {
        struct wlrail L;
        rails(&a, &L);
        int timeout = wlrail_loop(&a.win, &a.ran, &L, a.hover, -1);
        int64_t now = now_ms();
        if (engine_to_move(&a) && !a.tk.started && a.prompt != P_PROMO)
            think_start(&a, 0);
        if (a.tk.started) {
            int t = 25;                                                 /* look at the thread often while it works */
            if (timeout < 0 || t < timeout)
                timeout = t;
        }
        if (clock_running(&a)) {                                        /* the clock: ten looks a second when it is short, five otherwise */
            int t = LM_MIN(a.clk.left[CH_WHITE], a.clk.left[CH_BLACK]) < 20000 ? 100 : 200;
            if (timeout < 0 || t < timeout)
                timeout = t;
        }
        if (a.msg[0] && (timeout < 0 || timeout > 500))
            timeout = 500;
        if (!wlwin_step(&a.win, timeout))
            break;
        now = now_ms();
        if (a.clk.on) {
            bool was = clock_running(&a);
            clock_advance(&a, now);
            clock_check_flag(&a);
            if (was)
                a.win.redraw = true;
        }
        think_collect(&a, now);
        if (a.tk.started) {
            int d = __atomic_load_n(&a.tk.depth, __ATOMIC_RELAXED);
            static int shown_depth = -1;
            if (d != shown_depth) {
                shown_depth = d;
                a.win.redraw = true;
            }
        }
        if (a.hint && now - a.hint_ms > HINT_SHOW_MS) {
            a.hint = 0;
            a.win.redraw = true;
        }
        if (a.msg[0] && now - a.msg_ms >= 6000) {
            a.msg[0] = 0;
            a.win.redraw = true;
        }
    }
    think_cancel(&a);
    stats_save(&a.st);
    free(a.snap);
    sprites_free(&a);
    ch_game_free(&a.g);
    chai_free(a.ai);
    wlwin_destroy(&a.win);
    return 0;
}
