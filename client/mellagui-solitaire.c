/*
 * mellagui-solitaire - Klondike solitaire for MellaGUI.
 *
 *   mellagui-solitaire [--seed N] [--draw 1|3] [--size WxH]     a game (the seed names the deal: the same seed is the same game)
 *   mellagui-solitaire --selftest                               the rules without a display
 *
 * Seven columns, a stock and a waste, four foundations (one a suit, Ace to King). Click the stock to turn a card (or three), drag a card or a
 * run of cards from the table, the waste or a foundation to where it goes; a double click (or the right button) sends a card to its foundation.
 * Keys: Space draws, U / Ctrl+Z takes a move back, H shows a move, F finishes a game that is won on the table, N a new game, 1 / 3 the number of
 * cards turned, F1 the keys, Ctrl+Q quits. The window looks like the other programs: a rail of round drops at the left, theme colours, hidpi.
 * Wins and the best time are kept in $XDG_CONFIG_HOME/mellagui/solitaire.conf.
 *
 * $MELLAGUI_SOLITAIRE_DUMP=FILE writes the state after every paint for the tests (the cards of every pile, where they are on the screen, the
 * move the hint would show); `--setup almost` deals a game that is one move from won (also for the tests).
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

enum { SP, HE, DI, CL };                                    /* suits; hearts and diamonds are red */
enum { L_NONE, L_STOCK, L_WASTE, L_FOUND, L_TAB };

struct card {
    uint8_t rank;                                           /* 1 (Ace) .. 13 (King) */
    uint8_t suit;
    uint8_t up;
};

struct game {
    struct card stock[52], waste[52];
    int nstock, nwaste;
    struct card found[4][13];
    int nfound[4];                                          /* the foundation of suit s holds the cards of that suit */
    struct card tab[7][20];
    int ntab[7];
    int draw;                                               /* cards turned from the stock at a time: 1 or 3 */
    int moves;
    uint32_t seed;
};

struct loc {
    int kind;
    int i;                                                  /* the column, or the suit of a foundation */
    int j;                                                  /* the first card of a run in a column; -1 = the empty column */
};

static bool is_red(const struct card *c) { return c->suit == HE || c->suit == DI; }

static uint32_t rng_next(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static void game_deal(struct game *g, uint32_t seed, int draw)
{
    memset(g, 0, sizeof(*g));
    g->seed = seed;
    g->draw = draw == 3 ? 3 : 1;
    struct card deck[52];
    for (int i = 0; i < 52; i++)
        deck[i] = (struct card){ (uint8_t)(i % 13 + 1), (uint8_t)(i / 13), 0 };
    uint32_t s = seed * 2654435761u + 12345u;
    for (int i = 0; i < 8; i++)
        rng_next(&s);                                       /* (a few turns so that near seeds do not start alike) */
    for (int i = 51; i > 0; i--) {
        int k = (int)(rng_next(&s) % (uint32_t)(i + 1));
        struct card t = deck[i];
        deck[i] = deck[k];
        deck[k] = t;
    }
    int at = 0;
    for (int col = 0; col < 7; col++) {
        for (int j = 0; j <= col; j++) {
            struct card c = deck[at++];
            c.up = j == col;
            g->tab[col][g->ntab[col]++] = c;
        }
    }
    while (at < 52)
        g->stock[g->nstock++] = deck[at++];
}

/* A game one move from won (for the tests): every foundation is at the Queen and the four Kings lie face up on the table. */
static void game_almost(struct game *g, int draw)
{
    memset(g, 0, sizeof(*g));
    g->draw = draw == 3 ? 3 : 1;
    for (int s = 0; s < 4; s++) {
        for (int r = 1; r <= 12; r++)
            g->found[s][g->nfound[s]++] = (struct card){ (uint8_t)r, (uint8_t)s, 1 };
        g->tab[s][0] = (struct card){ 13, (uint8_t)s, 1 };
        g->ntab[s] = 1;
    }
}

static struct card *loc_card(struct game *g, struct loc l)
{
    switch (l.kind) {
    case L_WASTE: return g->nwaste ? &g->waste[g->nwaste - 1] : NULL;
    case L_STOCK: return g->nstock ? &g->stock[g->nstock - 1] : NULL;
    case L_FOUND: return g->nfound[l.i] ? &g->found[l.i][g->nfound[l.i] - 1] : NULL;
    case L_TAB: return l.j >= 0 && l.j < g->ntab[l.i] ? &g->tab[l.i][l.j] : NULL;
    default: return NULL;
    }
}

static int loc_count(const struct game *g, struct loc l)
{
    return l.kind == L_TAB ? g->ntab[l.i] - l.j : 1;
}

static bool can_drop(struct game *g, struct loc from, struct loc to)
{
    struct card *first = loc_card(g, from);
    if (!first || !first->up || from.kind == L_STOCK)
        return false;
    int n = loc_count(g, from);
    if (to.kind == L_FOUND)
        return n == 1 && from.kind != L_FOUND && first->suit == to.i && first->rank == g->nfound[to.i] + 1;
    if (to.kind != L_TAB || (from.kind == L_TAB && from.i == to.i))
        return false;
    if (g->ntab[to.i] == 0)
        return first->rank == 13;
    const struct card *top = &g->tab[to.i][g->ntab[to.i] - 1];
    return top->up && is_red(top) != is_red(first) && top->rank == first->rank + 1;
}

static bool game_move(struct game *g, struct loc from, struct loc to)
{
    if (!can_drop(g, from, to))
        return false;
    int n = loc_count(g, from);
    struct card run[13];
    for (int k = 0; k < n; k++) {
        struct loc l = from;
        if (from.kind == L_TAB)
            l.j = from.j + k;
        run[k] = *loc_card(g, l);
    }
    switch (from.kind) {
    case L_WASTE: g->nwaste--; break;
    case L_FOUND: g->nfound[from.i]--; break;
    case L_TAB:
        g->ntab[from.i] = from.j;
        if (g->ntab[from.i] > 0)
            g->tab[from.i][g->ntab[from.i] - 1].up = 1;     /* the card that was under the run turns over */
        break;
    default: break;
    }
    for (int k = 0; k < n; k++) {
        if (to.kind == L_FOUND)
            g->found[to.i][g->nfound[to.i]++] = run[k];
        else
            g->tab[to.i][g->ntab[to.i]++] = run[k];
    }
    g->moves++;
    return true;
}

static bool game_draw(struct game *g)
{
    if (g->nstock > 0) {
        for (int k = 0; k < g->draw && g->nstock > 0; k++) {
            struct card c = g->stock[--g->nstock];
            c.up = 1;
            g->waste[g->nwaste++] = c;
        }
        g->moves++;
        return true;
    }
    if (g->nwaste > 0) {                                    /* the waste goes back, turned over, in the order it will come out again */
        while (g->nwaste > 0) {
            struct card c = g->waste[--g->nwaste];
            c.up = 0;
            g->stock[g->nstock++] = c;
        }
        g->moves++;
        return true;
    }
    return false;
}

static bool game_won(const struct game *g)
{
    for (int s = 0; s < 4; s++)
        if (g->nfound[s] != 13)
            return false;
    return true;
}

/* Nothing is hidden and nothing is left to draw: the rest is only moving cards up. */
static bool game_finishable(const struct game *g)
{
    if (g->nstock || g->nwaste || game_won(g))
        return false;
    for (int c = 0; c < 7; c++)
        for (int j = 0; j < g->ntab[c]; j++)
            if (!g->tab[c][j].up)
                return false;
    return true;
}

/* Where a card at `from` goes to a foundation (true), or to a column when `any` and no foundation takes it. */
static bool auto_target(struct game *g, struct loc from, bool any, struct loc *to)
{
    struct card *c = loc_card(g, from);
    if (!c || !c->up)
        return false;
    struct loc f = { L_FOUND, c->suit, 0 };
    if (can_drop(g, from, f)) {
        *to = f;
        return true;
    }
    if (!any)
        return false;
    for (int col = 0; col < 7; col++) {
        struct loc t = { L_TAB, col, 0 };
        if (g->ntab[col] > 0 && can_drop(g, from, t)) {     /* (a King is not moved to an empty column by a click: it would only shuffle) */
            *to = t;
            return true;
        }
    }
    return false;
}

/* One card up to a foundation from the waste or the top of a column; true when one moved (the finish). */
static bool game_finish_step(struct game *g)
{
    struct loc to;
    struct loc w = { L_WASTE, 0, 0 };
    if (g->nwaste && auto_target(g, w, false, &to))
        return game_move(g, w, to);
    for (int col = 0; col < 7; col++) {
        struct loc t = { L_TAB, col, g->ntab[col] - 1 };
        if (g->ntab[col] && auto_target(g, t, false, &to))
            return game_move(g, t, to);
    }
    return false;
}

struct hint {
    int kind;                                               /* 0 none, 1 a move, 2 turn the stock */
    struct loc from, to;
};

/* A move worth making: to a foundation first, then one that turns a hidden card over, then from the waste, then the stock. */
static struct hint game_hint(struct game *g)
{
    struct hint h = { 0, { 0, 0, 0 }, { 0, 0, 0 } };
    struct loc to;
    struct loc w = { L_WASTE, 0, 0 };
    if (g->nwaste && auto_target(g, w, false, &to))
        return (struct hint){ 1, w, to };
    for (int col = 0; col < 7; col++) {
        struct loc t = { L_TAB, col, g->ntab[col] - 1 };
        if (g->ntab[col] && auto_target(g, t, false, &to))
            return (struct hint){ 1, t, to };
    }
    for (int col = 0; col < 7; col++) {                     /* a run that uncovers a hidden card, or a King that takes a whole column to an empty one */
        for (int j = 0; j < g->ntab[col]; j++) {
            if (!g->tab[col][j].up)
                continue;
            struct loc from = { L_TAB, col, j };
            for (int d = 0; d < 7; d++) {
                struct loc dst = { L_TAB, d, 0 };
                if (!can_drop(g, from, dst))
                    continue;
                bool uncovers = j > 0 && !g->tab[col][j - 1].up;
                bool king_empty = g->ntab[d] == 0 && j > 0;
                if (uncovers || king_empty)
                    return (struct hint){ 1, from, dst };
            }
            break;                                          /* (only the first face up card of a column is the start of the longest run) */
        }
    }
    if (g->nwaste) {
        for (int d = 0; d < 7; d++) {
            struct loc dst = { L_TAB, d, 0 };
            if (can_drop(g, w, dst))
                return (struct hint){ 1, w, dst };
        }
    }
    for (int col = 0; col < 7; col++) {                     /* any other move on the table that gets somewhere (not a King from the top of an empty column) */
        for (int j = 0; j < g->ntab[col]; j++) {
            if (!g->tab[col][j].up)
                continue;
            struct loc from = { L_TAB, col, j };
            for (int d = 0; d < 7; d++) {
                struct loc dst = { L_TAB, d, 0 };
                if (g->ntab[d] > 0 && can_drop(g, from, dst) && j > 0)
                    return (struct hint){ 1, from, dst };
            }
        }
    }
    if (g->nstock > 0 || g->nwaste > 0)
        h.kind = 2;
    return h;
}

/* ================================================================ the window ================================================================ */

#define STAT_H 24
#define MAX_UNDO 2000

enum prompt { P_NONE, P_NEW, P_WON, P_HELP };
enum { B_NEW, B_UNDO, B_HINT, B_DRAW, B_FINISH, B_HELP, B_COUNT };

struct stats {
    int played, won, best;                                  /* best time in seconds, 0 = none yet */
    int draw;
};

struct app {
    struct wlwin win;
    struct game g;
    struct game *undo;
    int nundo, cap;
    double px, py;
    bool has_pointer;
    int hover;                                              /* the rail item under the pointer */
    struct wlrail_anim ran;
    /* a press on a card: it becomes a drag when the pointer moves */
    bool press;
    int press_button;
    struct loc pressed;
    double press_x, press_y;
    bool dragging;
    struct card drag[13];
    int ndrag;
    double dox, doy;                                        /* where in the card the pointer took it */
    uint32_t click_ms;
    struct loc click_loc;
    enum prompt prompt;
    int pc_hover, pc_down;
    bool timing, won;
    int64_t t0, elapsed_ms;
    struct hint hint;
    int64_t hint_until;
    bool finishing;
    int64_t next_finish;
    char msg[160];
    int64_t msg_ms;
    struct stats st;
    bool counted;                                           /* this game is in `played` */
};

static const char *g_dump;

static int64_t now_ms(void) { return wlrail_now(); }

#define COL_BAR (lm_theme.bar)
#define COL_TEXT (lm_theme.text_soft)
#define COL_DIM (lm_theme.text_dim)
#define COL_ACCENT (lm_theme.accent)

/* the table and the cards are literals on purpose: a green felt and white cards whatever the theme */
#define FELT 0x1d6b43u
#define FELT_DARK 0x155735u
#define FELT_LIGHT 0x2a8a58u
#define CARD_FACE 0xfbfaf4u
#define CARD_EDGE 0x8c8c84u
#define CARD_RED 0xc62828u
#define CARD_BLACK 0x1c1c20u
#define CARD_BACK 0x244f9eu
#define CARD_BACK_LIGHT 0x3f72cfu

static void say(struct app *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->msg, sizeof(a->msg), fmt, ap);
    va_end(ap);
    a->msg_ms = now_ms();
    a->win.redraw = true;
}

/* ---- the file with the wins ---- */

static void stats_path(char *out, size_t n)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x)
        snprintf(out, n, "%s/mellagui/solitaire.conf", x);
    else
        snprintf(out, n, "%s/.config/mellagui/solitaire.conf", h && *h ? h : ".");
}

static void stats_load(struct stats *s)
{
    memset(s, 0, sizeof(*s));
    s->draw = 1;
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
        else if (sscanf(line, "best = %d", &v) == 1) s->best = v;
        else if (sscanf(line, "draw = %d", &v) == 1) s->draw = v == 3 ? 3 : 1;
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
    fprintf(f, "played = %d\nwon = %d\nbest = %d\ndraw = %d\n", s->played, s->won, s->best, s->draw);
    fclose(f);
}

/* ---- undo ---- */

static void push_undo(struct app *a)
{
    if (a->nundo == a->cap) {
        if (a->cap >= MAX_UNDO) {
            memmove(a->undo, a->undo + 1, (size_t)(a->cap - 1) * sizeof(a->undo[0]));
            a->nundo--;
        } else {
            int nc = a->cap ? a->cap * 2 : 64;
            struct game *n = realloc(a->undo, (size_t)nc * sizeof(a->undo[0]));
            if (!n)
                return;
            a->undo = n;
            a->cap = nc;
        }
    }
    a->undo[a->nundo++] = a->g;
}

static int elapsed_s(const struct app *a)
{
    int64_t ms = a->elapsed_ms + (a->timing ? now_ms() - a->t0 : 0);
    return (int)(ms / 1000);
}

static void clock_start(struct app *a)
{
    if (!a->timing && !a->won) {
        a->timing = true;
        a->t0 = now_ms();
    }
    if (!a->counted) {
        a->counted = true;
        a->st.played++;
        stats_save(&a->st);
    }
}

static void check_won(struct app *a)
{
    if (a->won || !game_won(&a->g))
        return;
    a->won = true;
    a->finishing = false;
    int secs = elapsed_s(a);
    if (a->timing) {
        a->elapsed_ms += now_ms() - a->t0;
        a->timing = false;
    }
    a->st.won++;
    if (!a->st.best || secs < a->st.best)
        a->st.best = secs ? secs : 1;
    stats_save(&a->st);
    a->prompt = P_WON;
    a->pc_hover = a->pc_down = -1;
}

static void new_game(struct app *a, uint32_t seed, int draw)
{
    game_deal(&a->g, seed, draw);
    a->nundo = 0;
    a->press = a->dragging = false;
    a->timing = a->won = a->finishing = a->counted = false;
    a->elapsed_ms = 0;
    a->hint.kind = 0;
    a->hint_until = 0;
    a->prompt = P_NONE;
    a->win.redraw = true;
}

static uint32_t fresh_seed(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint32_t)(ts.tv_nsec ^ (uint32_t)ts.tv_sec * 2654435761u ^ (uint32_t)getpid() << 7) % 1000000u + 1u;
}

/* ---- what a move does to the game state of the window ---- */

static void after_move(struct app *a)
{
    a->hint.kind = 0;
    a->hint_until = 0;
    check_won(a);
    a->win.redraw = true;
}

static bool do_move(struct app *a, struct loc from, struct loc to)
{
    struct game saved = a->g;
    if (!game_move(&a->g, from, to))
        return false;
    struct game after = a->g;
    a->g = saved;
    push_undo(a);
    a->g = after;
    clock_start(a);
    after_move(a);
    return true;
}

static void do_draw(struct app *a)
{
    struct game saved = a->g;
    if (!game_draw(&a->g)) {
        say(a, "The stock and the waste are empty");
        return;
    }
    struct game after = a->g;
    a->g = saved;
    push_undo(a);
    a->g = after;
    clock_start(a);
    after_move(a);
}

static void do_undo(struct app *a)
{
    if (a->nundo == 0) {
        say(a, "Nothing to take back");
        return;
    }
    if (a->won) {
        a->won = false;                                     /* a game that was won goes on from the move before */
        a->prompt = P_NONE;
        a->timing = true;
        a->t0 = now_ms();
    }
    a->g = a->undo[--a->nundo];
    a->finishing = false;
    a->hint.kind = 0;
    a->win.redraw = true;
}

static void do_hint(struct app *a)
{
    a->hint = game_hint(&a->g);
    a->hint_until = now_ms() + 3000;
    if (a->hint.kind == 0)
        say(a, "No move is left: take some back (U) or start a new game (N)");
    else if (a->hint.kind == 2)
        say(a, "Nothing to move on the table: turn the stock");
    a->win.redraw = true;
}

static void start_finish(struct app *a)
{
    if (!game_finishable(&a->g)) {
        say(a, "The finish is for when every card is face up and the stock is empty");
        return;
    }
    push_undo(a);
    clock_start(a);
    a->finishing = true;
    a->next_finish = now_ms();
}

/* ---- the geometry ---- */

struct lay {
    int x[7];
    int cw, ch, top_y, tab_y;
};

static void layout(const struct app *a, struct lay *L)
{
    int ox = WLRAIL_W + 14, aw = a->win.bw - ox - 14, gap = 12;
    int cw = (aw - 6 * gap) / 7;
    int avail_h = a->win.bh - STAT_H - 16;
    int cw_h = (avail_h - 20) * 100 / (140 * 2 + 40);       /* two card heights and the fan have to fit in the height too */
    cw = LM_CLAMP(LM_MIN(cw, LM_MAX(cw_h, 56)), 56, 120);
    L->cw = cw;
    L->ch = cw * 140 / 100;
    int total = 7 * cw + 6 * gap;
    int start = ox + (aw - total) / 2;
    for (int i = 0; i < 7; i++)
        L->x[i] = start + i * (cw + gap);
    L->top_y = 14;
    L->tab_y = L->top_y + L->ch + 20;
}

/* The tops of the cards of a column (a column that is long is squeezed so that it ends above the status line). */
static void col_ys(const struct app *a, const struct lay *L, int col, int *ys)
{
    const struct game *g = &a->g;
    int n = g->ntab[col], du = LM_MAX(24, L->ch / 4), dd = LM_MAX(8, L->ch / 9);
    int avail = a->win.bh - STAT_H - 6 - L->tab_y - L->ch, total = 0;
    for (int j = 0; j + 1 < n; j++)
        total += g->tab[col][j].up ? du : dd;
    double f = total > avail && total > 0 ? (double)avail / total : 1.0;
    if (f < 0.4)
        f = 0.4;
    int y = L->tab_y;
    for (int j = 0; j < n; j++) {
        ys[j] = y;
        y += (int)((g->tab[col][j].up ? du : dd) * f + 0.5);
    }
}

static int waste_x(const struct app *a, const struct lay *L, int k, int shown)   /* the k-th of `shown` fanned cards of the waste */
{
    (void)shown;
    return L->x[1] + k * (a->g.draw == 3 ? L->cw / 4 : 0);
}

static int waste_shown(const struct app *a)
{
    return a->g.draw == 3 ? LM_MIN(a->g.nwaste, 3) : LM_MIN(a->g.nwaste, 1);
}

static bool in_rect(double x, double y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* What is under a point. A card of a column counts for the part of it that shows. */
static struct loc hit(const struct app *a, double x, double y)
{
    struct lay L;
    layout(a, &L);
    const struct game *g = &a->g;
    struct loc none = { L_NONE, 0, 0 };
    if (in_rect(x, y, L.x[0], L.top_y, L.cw, L.ch))
        return (struct loc){ L_STOCK, 0, 0 };
    int shown = waste_shown(a);
    if (shown && in_rect(x, y, waste_x(a, &L, shown - 1, shown), L.top_y, L.cw, L.ch))
        return (struct loc){ L_WASTE, 0, 0 };
    for (int s = 0; s < 4; s++)
        if (in_rect(x, y, L.x[3 + s], L.top_y, L.cw, L.ch))
            return (struct loc){ L_FOUND, s, 0 };
    for (int col = 0; col < 7; col++) {
        int ys[20], n = g->ntab[col];
        col_ys(a, &L, col, ys);
        if (n == 0) {
            if (in_rect(x, y, L.x[col], L.tab_y, L.cw, L.ch))
                return (struct loc){ L_TAB, col, -1 };
            continue;
        }
        for (int j = n - 1; j >= 0; j--) {
            int h = j == n - 1 ? L.ch : ys[j + 1] - ys[j];
            if (in_rect(x, y, L.x[col], ys[j], L.cw, h))
                return (struct loc){ L_TAB, col, j };
        }
    }
    return none;
}

/* Where a dragged card is dropped: the column or foundation under its middle. */
static struct loc drop_target(const struct app *a, double x, double y)
{
    struct lay L;
    layout(a, &L);
    double cx = x - a->dox + L.cw / 2.0, cy = y - a->doy + L.ch / 2.0;
    int best = -1;
    double bd = 1e9;
    for (int i = 0; i < 7; i++) {
        double d = fabs(cx - (L.x[i] + L.cw / 2.0));
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    if (bd > L.cw * 0.75 + 6)
        return (struct loc){ L_NONE, 0, 0 };
    if (cy < L.tab_y - 10 && best >= 3)
        return (struct loc){ L_FOUND, best - 3, 0 };
    if (cy >= L.tab_y - 10)
        return (struct loc){ L_TAB, best, 0 };
    return (struct loc){ L_NONE, 0, 0 };
}

/* ---- painting the cards ---- */

static bool suit_in(int suit, double u, double v)             /* u, v in -1..1, v down */
{
    switch (suit) {
    case HE: {
        double x = u * 1.1, y = -v * 1.1 + 0.12, t = x * x + y * y - 1;
        return t * t * t - x * x * y * y * y <= 0;
    }
    case DI: return fabs(u) / 0.64 + fabs(v) / 0.95 <= 1;
    case SP: {
        double x = u * 1.1, y = v * 1.1 - 0.12, t = x * x + y * y - 1;
        if (t * t * t - x * x * y * y * y <= 0 && v < 0.55)
            return true;
        return v >= 0.45 && v <= 0.95 && fabs(u) <= 0.10 + 0.30 * (v - 0.45) / 0.5;
    }
    default: {
        static const double cx[3] = { 0, -0.44, 0.44 }, cy[3] = { -0.42, 0.12, 0.12 };
        for (int k = 0; k < 3; k++)
            if ((u - cx[k]) * (u - cx[k]) + (v - cy[k]) * (v - cy[k]) <= 0.36 * 0.36)
                return true;
        return v >= 0.1 && v <= 0.95 && fabs(u) <= 0.08 + 0.30 * (v - 0.1) / 0.85;
    }
    }
}

/* A suit, `size` pixels square with its top left at (x, y), drawn as runs of one row each. */
static void draw_suit(struct wlwin *w, int x, int y, int size, int suit, uint32_t rgb)
{
    for (int r = 0; r < size; r++) {
        int run = -1;
        for (int c = 0; c <= size; c++) {
            bool in = c < size && suit_in(suit, (c + 0.5) / size * 2 - 1, (r + 0.5) / size * 2 - 1);
            if (in && run < 0)
                run = c;
            if (!in && run >= 0) {
                wlwin_fill(w, x + run, y + r, c - run, 1, rgb);
                run = -1;
            }
        }
    }
}

static const char *rank_text(int rank, char *buf)
{
    static const char *const names[] = { "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K" };
    snprintf(buf, 4, "%s", names[rank - 1]);
    return buf;
}

static void draw_card_back(struct wlwin *w, int x, int y, int cw, int ch)
{
    wlwin_round_fill(w, x, y, cw, ch, 7, CARD_EDGE);
    wlwin_round_fill(w, x + 1, y + 1, cw - 2, ch - 2, 6, CARD_FACE);
    wlwin_round_fill(w, x + 4, y + 4, cw - 8, ch - 8, 4, CARD_BACK);
    int ix = x + 9, iy = y + 9, iw = cw - 18, ih = ch - 18;
    wlwin_round_fill(w, ix, iy, iw, ih, 3, CARD_BACK_LIGHT);
    wlwin_round_fill(w, ix + 2, iy + 2, iw - 4, ih - 4, 2, CARD_BACK);
    int cxm = x + cw / 2, cym = y + ch / 2, r = LM_MIN(iw, ih) / 4;      /* a diamond in the middle */
    for (int dy = -r; dy <= r; dy++)
        wlwin_fill(w, cxm - (r - abs(dy)), cym + dy, 2 * (r - abs(dy)) + 1, 1, CARD_BACK_LIGHT);
}

static void draw_card_face(struct wlwin *w, int x, int y, int cw, int ch, const struct card *c)
{
    uint32_t ink = is_red(c) ? CARD_RED : CARD_BLACK;
    wlwin_round_fill(w, x, y, cw, ch, 7, CARD_EDGE);
    wlwin_round_fill(w, x + 1, y + 1, cw - 2, ch - 2, 6, CARD_FACE);
    char t[4];
    rank_text(c->rank, t);
    int tw = lm_font_text_width(t);
    wlwin_text_bold(w, x + 6, y + 4, t, 1, ink, 0);
    draw_suit(w, x + 6 + tw + 3, y + 7, 12, c->suit, ink);
    int big = LM_CLAMP(cw / 2, 20, 48);
    if (c->rank >= 11) {                                    /* a court card: its letter large, with the suit under it */
        const char *letter = c->rank == 11 ? "J" : c->rank == 12 ? "Q" : "K";
        wlwin_text_bold(w, x + (cw - lm_font_text_width(letter) * 2) / 2, y + ch / 2 - LM_FONT_H - 4, letter, 2, ink, 0);
        draw_suit(w, x + (cw - big / 2) / 2, y + ch / 2 + 6, big / 2, c->suit, ink);
    } else {
        draw_suit(w, x + (cw - big) / 2, y + (ch - big) / 2 + 6, big, c->suit, ink);
    }
}

static void draw_slot(struct wlwin *w, int x, int y, int cw, int ch, int suit, bool recycle)
{
    wlwin_round_fill(w, x, y, cw, ch, 7, FELT_LIGHT);
    wlwin_round_fill(w, x + 2, y + 2, cw - 4, ch - 4, 6, FELT);
    if (suit >= 0)
        draw_suit(w, x + cw / 2 - cw / 6, y + ch / 2 - cw / 6, cw / 3, suit, FELT_LIGHT);
    if (recycle) {                                          /* a ring: the stock is empty, a click turns the waste over */
        int r = cw / 5, cx = x + cw / 2, cy = y + ch / 2;
        for (int dy = -r; dy <= r; dy++) {
            int half = (int)sqrt((double)(r * r - dy * dy)), inner = (int)sqrt((double)LM_MAX((r - 3) * (r - 3) - dy * dy, 0));
            if (abs(dy) >= r - 3)
                wlwin_fill(w, cx - half, cy + dy, 2 * half + 1, 1, FELT_LIGHT);
            else {
                wlwin_fill(w, cx - half, cy + dy, half - inner, 1, FELT_LIGHT);
                wlwin_fill(w, cx + inner, cy + dy, half - inner, 1, FELT_LIGHT);
            }
        }
    }
}

static void hint_box(struct wlwin *w, int x, int y, int cw, int h, uint32_t rgb)
{
    wlwin_fill(w, x - 3, y - 3, cw + 6, 3, rgb);
    wlwin_fill(w, x - 3, y + h, cw + 6, 3, rgb);
    wlwin_fill(w, x - 3, y, 3, h, rgb);
    wlwin_fill(w, x + cw, y, 3, h, rgb);
}

static bool hidden_by_drag(const struct app *a, int kind, int i, int j)
{
    if (!a->dragging)
        return false;
    const struct loc *p = &a->pressed;
    if (p->kind != kind)
        return false;
    if (kind == L_TAB)
        return p->i == i && j >= p->j;
    if (kind == L_FOUND)
        return p->i == i && j == a->g.nfound[i] - 1;
    return kind == L_WASTE && j == a->g.nwaste - 1;
}

static void paint_table(struct app *a)
{
    struct wlwin *w = &a->win;
    struct lay L;
    layout(a, &L);
    const struct game *g = &a->g;
    wlwin_fill(w, WLRAIL_W, 0, w->bw - WLRAIL_W, w->bh, FELT);
    wlwin_fill(w, WLRAIL_W, 0, 3, w->bh, FELT_DARK);
    /* stock */
    if (g->nstock)
        draw_card_back(w, L.x[0], L.top_y, L.cw, L.ch);
    else
        draw_slot(w, L.x[0], L.top_y, L.cw, L.ch, -1, g->nwaste > 0);
    /* waste */
    int shown = waste_shown(a);
    if (!shown)
        draw_slot(w, L.x[1], L.top_y, L.cw, L.ch, -1, false);
    for (int k = 0; k < shown; k++) {
        int idx = g->nwaste - shown + k;
        if (hidden_by_drag(a, L_WASTE, 0, idx))
            continue;
        draw_card_face(w, waste_x(a, &L, k, shown), L.top_y, L.cw, L.ch, &g->waste[idx]);
    }
    /* foundations */
    for (int s = 0; s < 4; s++) {
        int n = g->nfound[s];
        draw_slot(w, L.x[3 + s], L.top_y, L.cw, L.ch, s, false);
        if (n && !hidden_by_drag(a, L_FOUND, s, n - 1))
            draw_card_face(w, L.x[3 + s], L.top_y, L.cw, L.ch, &g->found[s][n - 1]);
        else if (n > 1)
            draw_card_face(w, L.x[3 + s], L.top_y, L.cw, L.ch, &g->found[s][n - 2]);
    }
    /* the table */
    for (int col = 0; col < 7; col++) {
        int ys[20], n = g->ntab[col];
        col_ys(a, &L, col, ys);
        if (n == 0)
            draw_slot(w, L.x[col], L.tab_y, L.cw, L.ch, -1, false);
        for (int j = 0; j < n; j++) {
            if (hidden_by_drag(a, L_TAB, col, j))
                break;
            if (g->tab[col][j].up)
                draw_card_face(w, L.x[col], ys[j], L.cw, L.ch, &g->tab[col][j]);
            else
                draw_card_back(w, L.x[col], ys[j], L.cw, L.ch);
        }
    }
    /* the hint: a frame round what to move and where to */
    if (a->hint.kind && now_ms() < a->hint_until) {
        uint32_t hc = 0xffd43bu;
        if (a->hint.kind == 2) {
            hint_box(w, L.x[0], L.top_y, L.cw, L.ch, hc);
        } else {
            struct loc f = a->hint.from, t = a->hint.to;
            if (f.kind == L_WASTE && shown)
                hint_box(w, waste_x(a, &L, shown - 1, shown), L.top_y, L.cw, L.ch, hc);
            else if (f.kind == L_TAB) {
                int ys[20];
                col_ys(a, &L, f.i, ys);
                hint_box(w, L.x[f.i], ys[f.j], L.cw, ys[g->ntab[f.i] - 1] + L.ch - ys[f.j], hc);
            }
            if (t.kind == L_FOUND)
                hint_box(w, L.x[3 + t.i], L.top_y, L.cw, L.ch, hc);
            else if (t.kind == L_TAB) {
                int ys[20], n = g->ntab[t.i];
                col_ys(a, &L, t.i, ys);
                hint_box(w, L.x[t.i], n ? ys[n - 1] : L.tab_y, L.cw, L.ch, hc);
            }
        }
    }
    /* the cards in the hand */
    if (a->dragging) {
        int n = a->ndrag;
        for (int k = 0; k < n; k++) {
            int x = (int)(a->px - a->dox), y = (int)(a->py - a->doy) + k * LM_MAX(24, L.ch / 4);
            draw_card_face(w, x, y, L.cw, L.ch, &a->drag[k]);
        }
    }
}

static void fmt_time(int s, char *out, size_t n)
{
    snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static void paint_status(struct app *a)
{
    struct wlwin *w = &a->win;
    int y = w->bh - STAT_H;
    wlwin_fill(w, 0, y, w->bw, STAT_H, COL_BAR);
    char t[16], line[200];
    fmt_time(elapsed_s(a), t, sizeof(t));
    snprintf(line, sizeof(line), "Moves %d   Time %s   Stock %d   Game %u   Turn %d", a->g.moves, t, a->g.nstock, a->g.seed, a->g.draw);
    wlwin_text(w, WLRAIL_W + 12, y + (STAT_H - LM_FONT_H) / 2, line, 1, COL_TEXT, w->bw - WLRAIL_W - 24);
    char right[100];
    char best[16] = "-";
    if (a->st.best)
        fmt_time(a->st.best, best, sizeof(best));
    snprintf(right, sizeof(right), "Won %d of %d   Best %s", a->st.won, a->st.played, best);
    int rw = lm_font_text_width(right);
    if (a->msg[0] && now_ms() - a->msg_ms < 6000)
        wlwin_text(w, WLRAIL_W + 12, y - 22, a->msg, 1, 0xfff3bfu, w->bw - WLRAIL_W - 24);
    else if (w->bw - WLRAIL_W - 24 > rw + lm_font_text_width(line) + 20)
        wlwin_text(w, w->bw - rw - 12, y + (STAT_H - LM_FONT_H) / 2, right, 1, COL_DIM, rw);
}

/* ---- the rail ---- */

static void ic_new(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    wlrail_ic_new(w, cx, cy, c, bg);
}

static void ic_undo(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_fill(w, cx - 4, cy - 5, 9, 2, c);
    wlrail_line(w, cx + 5, cy - 4, cx + 7, cy + 1, 2, c);
    wlrail_line(w, cx + 7, cy + 1, cx + 3, cy + 5, 2, c);
    wlrail_line(w, cx - 4, cy - 4, cx - 1, cy - 8, 2, c);
    wlrail_line(w, cx - 4, cy - 4, cx - 1, cy, 2, c);
}

static void ic_hint(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    wlwin_round_fill(w, cx - 5, cy - 8, 10, 10, 5, c);                  /* a bulb */
    wlwin_fill(w, cx - 3, cy + 2, 6, 2, c);
    wlwin_fill(w, cx - 2, cy + 5, 4, 2, c);
}

static void ic_finish(struct wlwin *w, int cx, int cy, uint32_t c, uint32_t bg)
{
    (void)bg;
    for (int k = 0; k < 2; k++) {                                       /* two chevrons: on, on */
        int x = cx - 7 + k * 6;
        wlrail_line(w, x, cy - 6, x + 5, cy, 2, c);
        wlrail_line(w, x + 5, cy, x, cy + 6, 2, c);
    }
}

static void rails(const struct app *a, struct wlrail *L)
{
    static const char *const tips[B_COUNT] = { "New game  N", "Take a move back  U", "Show a move  H", "Cards turned  1 / 3", "Finish the game  F", "Keys  F1" };
    static void (*const icons[B_COUNT])(struct wlwin *, int, int, uint32_t, uint32_t) = { ic_new, ic_undo, ic_hint, NULL, ic_finish, NULL };
    const uint32_t base[B_COUNT] = { lm_theme.btn_green, lm_theme.btn_slate, lm_theme.btn_amber, lm_theme.btn_purple, lm_theme.btn_blue, lm_theme.btn_rose };
    const uint32_t lite[B_COUNT] = { lm_theme.btn_green_lit, lm_theme.btn_slate_lit, lm_theme.btn_amber_lit, lm_theme.btn_purple_lit, lm_theme.btn_blue_lit, lm_theme.btn_rose_lit };
    memset(L, 0, sizeof(*L));
    L->x = 0;
    L->y0 = 0;
    L->y1 = a->win.bh > 0 ? a->win.bh - STAT_H : 400;
    L->n = B_COUNT;
    for (int i = 0; i < B_COUNT; i++) {
        bool idle = (i == B_UNDO && a->nundo == 0) || (i == B_FINISH && !game_finishable(&a->g));
        L->it[i] = (struct wlrail_item){ .tip = tips[i], .icon = icons[i], .text = i == B_DRAW ? (a->g.draw == 3 ? "3" : "1") : i == B_HELP ? "?" : NULL,
                                         .base = base[i], .lite = lite[i], .show = true, .idle = idle, .lit = i == B_FINISH && a->finishing,
                                         .gap = i == B_UNDO || i == B_DRAW, .bottom = i == B_HELP };
    }
}

static void press_button(struct app *a, int i);

/* ---- the cards of the questions ---- */

static void card_rect(const struct app *a, struct wlcard *c, int *nb)
{
    int ch = a->prompt == P_HELP ? 250 : 130;
    *nb = a->prompt == P_HELP ? 1 : 2;
    c->w = LM_MIN(a->prompt == P_HELP ? 540 : 440, a->win.bw - 16);
    c->h = LM_MIN(ch, a->win.bh - 16);
    c->x = (a->win.bw - c->w) / 2;
    c->y = (a->win.bh - c->h) / 2;
}

static const char *const HELP_LINES[] = {
    "Click the stock to turn a card (three with Turn 3).",
    "Drag a card, or a run of cards, to where it fits: a card of the",
    "opposite colour one lower on the table, a King on an empty column,",
    "or the foundation of its suit (Ace first).",
    "Double click or right click sends a card up to its foundation.",
    "Space draws, U or Ctrl+Z takes a move back, H shows a move.",
    "F finishes a game that is won on the table, N a new game,",
    "1 and 3 change how many cards are turned (a new game).",
};

static void paint_card(struct app *a)
{
    struct wlwin *w = &a->win;
    struct wlcard c;
    int nb;
    card_rect(a, &c, &nb);
    const char *title = a->prompt == P_NEW ? "New game" : a->prompt == P_WON ? "You won!" : "Keys";
    wlcard_frame(w, &c, c.w, c.h, title, COL_ACCENT, lm_theme.background, lm_theme.text_bright);
    char line[160] = "", line2[120] = "";
    const char *labels[2] = { "Cancel", "OK" };
    uint32_t cols[2] = { lm_theme.btn_grey, lm_theme.btn_blue };
    if (a->prompt == P_NEW) {
        snprintf(line, sizeof(line), "Give this game up and deal a new one?");
        labels[1] = "New game";
        cols[1] = lm_theme.btn_green;
    } else if (a->prompt == P_WON) {
        char t[16];
        fmt_time(elapsed_s(a), t, sizeof(t));
        snprintf(line, sizeof(line), "%d moves in %s.", a->g.moves, t);
        snprintf(line2, sizeof(line2), "Won %d of %d games.", a->st.won, a->st.played);
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
    wlcard_buttons(w, &c, labels, nb, cols, a->pc_hover, a->pc_down, lm_theme.on_colour);
}

static void card_accept(struct app *a, int which)
{
    if (a->prompt == P_HELP || which == 0) {
        a->prompt = P_NONE;
        return;
    }
    new_game(a, fresh_seed(), a->st.draw);
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
    fprintf(f, "state moves=%d time=%d won=%d draw=%d seed=%u stock=%d waste=%d prompt=%d undo=%d finishing=%d played=%d wins=%d\n", g->moves, elapsed_s(a), a->won, g->draw,
            g->seed, g->nstock, g->nwaste, (int)a->prompt, a->nundo, a->finishing, a->st.played, a->st.won);
    struct lay L;
    layout(a, &L);
    fprintf(f, "lay cw=%d ch=%d\n", L.cw, L.ch);
    fprintf(f, "slot stock %d %d %d %d\n", L.x[0], L.top_y, L.cw, L.ch);
    int shown = waste_shown(a);
    fprintf(f, "slot waste %d %d %d %d\n", shown ? waste_x(a, &L, shown - 1, shown) : L.x[1], L.top_y, L.cw, L.ch);
    for (int s = 0; s < 4; s++) {
        fprintf(f, "found %d n=%d x=%d y=%d w=%d h=%d\n", s, g->nfound[s], L.x[3 + s], L.top_y, L.cw, L.ch);
    }
    fprintf(f, "waste");
    for (int k = 0; k < g->nwaste; k++)
        fprintf(f, " %c%c", "A23456789TJQK"[g->waste[k].rank - 1], "SHDC"[g->waste[k].suit]);
    fprintf(f, "\n");
    for (int col = 0; col < 7; col++) {
        int ys[20];
        col_ys(a, &L, col, ys);
        fprintf(f, "tab %d x=%d y=%d w=%d h=%d n=%d cards", col, L.x[col], g->ntab[col] ? ys[g->ntab[col] - 1] : L.tab_y, L.cw, L.ch, g->ntab[col]);
        for (int j = 0; j < g->ntab[col]; j++) {
            const struct card *c = &g->tab[col][j];
            if (c->up)
                fprintf(f, " %c%c@%d", "A23456789TJQK"[c->rank - 1], "SHDC"[c->suit], ys[j]);
            else
                fprintf(f, " ??@%d", ys[j]);
        }
        fprintf(f, "\n");
    }
    struct hint h = game_hint((struct game *)g);
    if (h.kind == 1)
        fprintf(f, "hint move from=%d,%d,%d to=%d,%d,%d\n", h.from.kind, h.from.i, h.from.j, h.to.kind, h.to.i, h.to.j);
    else
        fprintf(f, "hint %s\n", h.kind == 2 ? "draw" : "none");
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
    static const char *const names[B_COUNT] = { "New", "Undo", "Hint", "Draw", "Finish", "Help" };
    for (int i = 0; i < R.n; i++)
        if (ys[i] >= 0)
            fprintf(f, "btn %s x=%d y=%d w=%d h=%d\n", names[i], R.x + (WLRAIL_W - WLRAIL_D) / 2, ys[i], WLRAIL_D, WLRAIL_D);
    fclose(f);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    wlwin_fill(w, 0, 0, w->bw, w->bh, lm_theme.background);
    paint_table(a);
    paint_status(a);
    struct wlrail L;
    rails(a, &L);
    wlrail_paint(w, &L, &a->ran, a->hover, now_ms(), COL_BAR, COL_BAR, COL_ACCENT, lm_theme.text);
    if (a->prompt != P_NONE)
        paint_card(a);
    write_dump(a);
}

/* ---- what the buttons and the pointer do ---- */

static void press_button(struct app *a, int i)
{
    switch (i) {
    case B_NEW:
        if (a->g.moves > 0 && !a->won) {
            a->prompt = P_NEW;
            a->pc_hover = a->pc_down = -1;
        } else
            new_game(a, fresh_seed(), a->st.draw);
        break;
    case B_UNDO: do_undo(a); break;
    case B_HINT: do_hint(a); break;
    case B_DRAW: {
        int d = a->g.draw == 3 ? 1 : 3;
        a->st.draw = d;
        stats_save(&a->st);
        if (a->g.moves == 0 || a->won)
            new_game(a, fresh_seed(), d);
        else {
            a->prompt = P_NEW;
            a->pc_hover = a->pc_down = -1;
            say(a, "Turn %d starts with the next new game", d);
        }
        break;
    }
    case B_FINISH: start_finish(a); break;
    case B_HELP: a->prompt = P_HELP; a->pc_hover = a->pc_down = -1; break;
    default: break;
    }
    a->win.redraw = true;
}

static void send_up(struct app *a, struct loc from)
{
    struct loc to;
    if (auto_target(&a->g, from, true, &to))
        do_move(a, from, to);
    else
        say(a, "That card has nowhere to go");
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
        else if (!wlcard_has(&c, (int)a->px, (int)a->py) && a->prompt != P_WON)
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
        if (a->press && !a->dragging && (fabs(a->px - a->press_x) > 5 || fabs(a->py - a->press_y) > 5) && a->press_button == 272) {
            struct loc p = a->pressed;
            struct card *first = loc_card(&a->g, p);
            if (first && first->up && p.kind != L_STOCK) {              /* the cards leave their pile and follow the pointer */
                struct lay Ly;
                layout(a, &Ly);
                a->ndrag = loc_count(&a->g, p);
                for (int k = 0; k < a->ndrag; k++) {
                    struct loc l = p;
                    if (p.kind == L_TAB)
                        l.j = p.j + k;
                    a->drag[k] = *loc_card(&a->g, l);
                }
                a->dragging = true;
                a->hint.kind = 0;
            }
        }
        w->redraw = true;
        return;
    }
    if (op != LM_WL_POINTER_EVT_BUTTON)
        return;
    uint32_t button = args[2].u, state = args[3].u, t = args[1].u;
    if (state != 1) {
        wlrail_release(&a->ran);
        if (a->dragging && button == 272) {
            struct loc to = drop_target(a, a->px, a->py);
            struct loc from = a->pressed;
            a->dragging = false;
            a->press = false;
            if (to.kind != L_NONE && !do_move(a, from, to))
                say(a, "That card does not go there");
        } else if (a->press && button == (uint32_t)a->press_button) {
            struct loc p = a->pressed;
            a->press = false;
            if (p.kind == L_STOCK) {
                do_draw(a);
            } else if (button == 273) {
                send_up(a, p);
            } else if (p.kind != L_NONE) {
                bool dbl = t - a->click_ms < (uint32_t)lm_metrics.double_click_ms && a->click_loc.kind == p.kind && a->click_loc.i == p.i && a->click_loc.j == p.j;
                a->click_ms = t;
                a->click_loc = p;
                if (dbl) {
                    send_up(a, p);
                    a->click_ms = 0;
                }
            }
        }
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
    if (a->finishing)
        return;
    struct loc h = hit(a, a->px, a->py);
    if ((button == 272 || button == 273) && h.kind != L_NONE) {
        struct lay Ly;
        layout(a, &Ly);
        a->press = true;
        a->press_button = (int)button;
        a->pressed = h;
        a->press_x = a->px;
        a->press_y = a->py;
        int cx = Ly.x[0], cy = Ly.top_y;                                /* where in the card it was taken */
        if (h.kind == L_WASTE) {
            int shown = waste_shown(a);
            cx = waste_x(a, &Ly, shown - 1, shown);
        } else if (h.kind == L_FOUND) {
            cx = Ly.x[3 + h.i];
        } else if (h.kind == L_TAB) {
            int ys[20];
            col_ys(a, &Ly, h.i, ys);
            cx = Ly.x[h.i];
            cy = h.j >= 0 ? ys[h.j] : Ly.tab_y;
        }
        a->dox = a->px - cx;
        a->doy = a->py - cy;
    }
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
        if (ks == LM_KS_Escape || (c == 'n' && a->prompt == P_NEW))
            a->prompt = P_NONE;
        else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter || (c == 'y' && a->prompt == P_NEW) || (a->prompt == P_WON && c == 'n'))
            card_accept(a, 1);
        return;
    }
    if (ctrl) {
        if (c == 'z')
            do_undo(a);
        else if (c == 'q')
            w->running = false;
        else if (c == 'n')
            press_button(a, B_NEW);
        return;
    }
    if (a->finishing)
        return;
    if (ks == LM_KS_F1)
        a->prompt = P_HELP, a->pc_hover = a->pc_down = -1;
    else if (c == ' ')
        do_draw(a);
    else if (c == 'u')
        do_undo(a);
    else if (c == 'h')
        do_hint(a);
    else if (c == 'f')
        start_finish(a);
    else if (c == 'n')
        press_button(a, B_NEW);
    else if (c == '1' || c == '3') {
        int d = c == '3' ? 3 : 1;
        if (d != a->g.draw)
            press_button(a, B_DRAW);
    }
}

static bool close_event(struct wlwin *w)
{
    (void)w;
    return true;
}

/* ---- the rules' test ---- */

static int selftest(void)
{
    int bad = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "selftest: " __VA_ARGS__); fputc('\n', stderr); bad++; } } while (0)
    for (uint32_t seed = 1; seed <= 200; seed++) {                      /* every deal is the 52 cards once, and has the shape of a game */
        struct game g;
        game_deal(&g, seed, 1);
        int seen[52] = { 0 };
        int total = g.nstock;
        for (int i = 0; i < g.nstock; i++)
            seen[(g.stock[i].suit * 13) + g.stock[i].rank - 1]++;
        for (int c = 0; c < 7; c++) {
            CHECK(g.ntab[c] == c + 1, "column %d has %d cards", c, g.ntab[c]);
            total += g.ntab[c];
            for (int j = 0; j < g.ntab[c]; j++) {
                seen[(g.tab[c][j].suit * 13) + g.tab[c][j].rank - 1]++;
                CHECK(g.tab[c][j].up == (j == c), "face of card %d of column %d", j, c);
            }
        }
        CHECK(total == 52 && g.nstock == 24, "deal %u: %d cards, stock %d", seed, total, g.nstock);
        for (int i = 0; i < 52; i++)
            CHECK(seen[i] == 1, "deal %u: card %d seen %d times", seed, i, seen[i]);
    }
    struct game a, b;
    game_deal(&a, 7, 1);
    game_deal(&b, 7, 1);
    CHECK(!memcmp(&a, &b, sizeof(a)), "the same seed is not the same deal");
    game_deal(&b, 8, 1);
    CHECK(memcmp(&a, &b, sizeof(a)) != 0, "two seeds gave the same deal");
    /* drawing one and three cards, and the waste going back */
    game_deal(&a, 3, 3);
    CHECK(game_draw(&a) && a.nwaste == 3 && a.nstock == 21 && a.waste[2].up, "draw three");
    for (int i = 0; i < 7; i++)
        game_draw(&a);
    CHECK(a.nstock == 0 && a.nwaste == 24, "all drawn: %d / %d", a.nstock, a.nwaste);
    struct card first_waste = a.waste[0];
    CHECK(game_draw(&a) && a.nstock == 24 && a.nwaste == 0 && !a.stock[0].up, "the waste goes back");
    CHECK(a.stock[23].rank == first_waste.rank && a.stock[23].suit == first_waste.suit, "the waste goes back in the same order");
    /* what may go where */
    struct game g;
    memset(&g, 0, sizeof(g));
    g.tab[0][0] = (struct card){ 7, HE, 1 };
    g.ntab[0] = 1;
    g.tab[1][0] = (struct card){ 6, SP, 1 };
    g.ntab[1] = 1;
    g.tab[2][0] = (struct card){ 6, DI, 1 };
    g.ntab[2] = 1;
    g.tab[3][0] = (struct card){ 13, CL, 1 };
    g.ntab[3] = 1;
    struct loc c0 = { L_TAB, 0, 0 }, c1 = { L_TAB, 1, 0 }, c2 = { L_TAB, 2, 0 }, c4 = { L_TAB, 4, 0 };
    CHECK(can_drop(&g, c1, c0), "a black 6 on a red 7");
    CHECK(!can_drop(&g, c2, c0), "a red 6 on a red 7");
    CHECK(!can_drop(&g, c0, c1), "a 7 on a 6");
    struct loc k = { L_TAB, 3, 0 };
    CHECK(can_drop(&g, k, c4), "a King on an empty column");
    CHECK(!can_drop(&g, c1, c4), "a 6 on an empty column");
    /* a run moves whole, and the card under it turns over */
    memset(&g, 0, sizeof(g));
    g.tab[0][0] = (struct card){ 9, CL, 0 };
    g.tab[0][1] = (struct card){ 8, CL, 1 };
    g.tab[0][2] = (struct card){ 7, HE, 1 };
    g.ntab[0] = 3;
    g.tab[1][0] = (struct card){ 9, DI, 1 };
    g.ntab[1] = 1;
    struct loc run = { L_TAB, 0, 1 }, onto = { L_TAB, 1, 0 };
    CHECK(game_move(&g, run, onto) && g.ntab[1] == 3 && g.ntab[0] == 1 && g.tab[0][0].up && g.tab[1][2].rank == 7 && g.moves == 1, "moving a run");
    /* foundations: the suit, in order, one card at a time, and not from a run */
    memset(&g, 0, sizeof(g));
    g.tab[0][0] = (struct card){ 1, HE, 1 };
    g.tab[0][1] = (struct card){ 2, HE, 1 };
    g.ntab[0] = 2;
    struct loc ace = { L_TAB, 0, 0 }, two = { L_TAB, 0, 1 }, fh = { L_FOUND, HE, 0 }, fs = { L_FOUND, SP, 0 };
    CHECK(!can_drop(&g, ace, fh), "an Ace under a card goes up with it");
    CHECK(!can_drop(&g, two, fh), "a 2 before the Ace");
    g.tab[0][1].rank = 5;
    g.ntab[0] = 1;
    CHECK(!can_drop(&g, ace, fs) && game_move(&g, ace, fh) && g.nfound[HE] == 1, "an Ace to its own foundation");
    /* the hint finds the move up, the undo of the window is a copy of the game */
    game_almost(&g, 1);
    struct hint h = game_hint(&g);
    CHECK(h.kind == 1 && h.to.kind == L_FOUND, "the hint of the almost won game");
    CHECK(!game_won(&g) && game_finishable(&g), "the almost won game is not won, and can be finished");
    int steps = 0;
    while (game_finish_step(&g))
        steps++;
    CHECK(steps == 4 && game_won(&g), "the finish took %d steps", steps);
    /* a hint is always a move that is possible */
    for (uint32_t seed = 1; seed <= 300; seed++) {
        game_deal(&g, seed, seed % 2 ? 3 : 1);
        for (int turn = 0; turn < 120; turn++) {
            struct hint hh = game_hint(&g);
            if (hh.kind == 0)
                break;
            if (hh.kind == 2) {
                CHECK(game_draw(&g), "a hint to draw with nothing to draw");
            } else {
                CHECK(game_move(&g, hh.from, hh.to), "deal %u: the hint is no legal move", seed);
            }
            int cards = g.nstock + g.nwaste + g.nfound[0] + g.nfound[1] + g.nfound[2] + g.nfound[3];
            for (int c = 0; c < 7; c++)
                cards += g.ntab[c];
            CHECK(cards == 52, "deal %u: %d cards after %d moves", seed, cards, turn);
            if (bad > 10)
                return 1;
        }
    }
    printf("mellagui-solitaire selftest %s\n", bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}

/* ---- main ---- */

int main(int argc, char **argv)
{
    wlwin_hidpi();
    lm_theme_load_env();
    signal(SIGPIPE, SIG_IGN);
    uint32_t seed = 0;
    int draw = 0, win_w = 960, win_h = 700;
    bool almost = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest"))
            return selftest();
        if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--draw") && i + 1 < argc) {
            draw = atoi(argv[++i]);
            if (draw != 1 && draw != 3) {
                fprintf(stderr, "mellagui-solitaire: --draw wants 1 or 3\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            char junk;
            if (sscanf(argv[++i], "%dx%d%c", &win_w, &win_h, &junk) != 2 || win_w < 300 || win_h < 300) {
                fprintf(stderr, "mellagui-solitaire: --size wants a size like 960x700\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--setup") && i + 1 < argc) {
            if (strcmp(argv[++i], "almost")) {
                fprintf(stderr, "mellagui-solitaire: --setup knows only 'almost'\n");
                return 2;
            }
            almost = true;
        } else {
            fprintf(stderr, "usage: mellagui-solitaire [--seed N] [--draw 1|3] [--size WxH] | --selftest\n");
            return 2;
        }
    }
    g_dump = getenv("MELLAGUI_SOLITAIRE_DUMP");
    static struct app a;
    stats_load(&a.st);
    if (!draw)
        draw = a.st.draw;
    a.st.draw = draw;
    a.hover = -1;
    a.pc_hover = a.pc_down = -1;
    wlrail_init(&a.ran);
    static const struct wlwin_ops ops = { .paint = paint, .pointer = pointer_event, .keyboard = keyboard_event, .close = close_event };
    if (wlwin_connect(&a.win, "mellagui-solitaire", &ops, &a) < 0)
        return 1;
    if (wlwin_open(&a.win, "Solitaire", "mellagui.solitaire", win_w, win_h, 640, 520) < 0)
        return 1;
    if (almost) {
        game_almost(&a.g, draw);
    } else {
        new_game(&a, seed ? seed : fresh_seed(), draw);
    }
    while (a.win.running) {
        int timeout = -1;
        struct wlrail L;
        rails(&a, &L);
        timeout = wlrail_loop(&a.win, &a.ran, &L, a.hover, timeout);
        int64_t now = now_ms();
        if (a.timing && (timeout < 0 || timeout > 500))
            timeout = 500;                                              /* the clock in the status line */
        if (a.finishing)
            timeout = LM_MAX(1, (int)(a.next_finish - now));
        if (a.hint.kind && now < a.hint_until && (timeout < 0 || timeout > 400))
            timeout = 400;                                              /* the frame of a hint goes away by itself */
        if (a.msg[0] && (timeout < 0 || timeout > 500))
            timeout = 500;
        if (!wlwin_step(&a.win, timeout))
            break;
        now = now_ms();
        if (a.finishing && now >= a.next_finish) {
            if (game_finish_step(&a.g)) {
                a.next_finish = now + 70;
                check_won(&a);
            } else {
                a.finishing = false;
            }
            a.win.redraw = true;
        }
        if (a.timing || (a.hint.kind && now >= a.hint_until) || a.msg[0])
            a.win.redraw = true;
        if (a.msg[0] && now - a.msg_ms >= 6000)
            a.msg[0] = 0;
        if (a.hint.kind && now >= a.hint_until)
            a.hint.kind = 0;
    }
    free(a.undo);
    wlwin_destroy(&a.win);
    return 0;
}
