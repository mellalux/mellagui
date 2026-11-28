/*
 * mellagui-term: terminal emulator Wayland client for MellaGUI.
 *
 * pty + xterm-compatible VT parser (cursor movement, erase, SGR with 16/256/
 * truecolor, scroll regions, alternate screen, bracketed paste, DSR/DA
 * replies), 8x16 bitmap font (zoom 50..500 %, any step; fractional sizes are area-averaged), scrollback, mouse selection with clipboard copy
 * and paste. Keyboard translation uses the built-in US layout from keymap.c.
 *
 *   mellagui-term                    your shell
 *   mellagui-term -e CMD [ARGS...]   run CMD in the terminal instead of the shell (everything after -e is the command); the window closes
 *                                    when it ends. "@terminal -e alpine" in a dock / launcher entry starts a text program.
 *   mellagui-term --title TEXT       the window's title until the program sets its own
 *   mellagui-term --adopt PID        (internal) take over the shell PID that another window handed over: its pty on fd 3, its screen
 *                                    on fd 4 (a tab dragged out of a window becomes a window of its own this way)
 *
 * Tabs: Ctrl+Shift+T a new one, Ctrl+Shift+W closes it, Ctrl+PageUp / PageDown or Alt+1..9 switch, Ctrl+Shift+PageUp / PageDown move
 * it, Ctrl+Shift+N a new window. Pull a tab out of the bar with the mouse to make it a window of its own. The bar shows only while
 * there are two tabs or more; a status bar at the bottom (Ctrl+Shift+B hides it) tells the size, the cursor, the scrollback ...
 */
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>

#include "theme.h"
#include "font.h"
#include "keymap.h"
#include "metrics.h"
#include "uikit.h"
#include "util.h"
#include "wlc.h"
#include "wlrail.h"
#include "charwidth.h"
#include "wltabs.h"

#define PAD 3
#define SCROLLBACK 1000
#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24
/* Font zoom in percent. Whole multiples of 100 draw the bitmap font crisply, anything else is area-averaged. */
#define ZOOM_LEVELS LM_ZOOM_LEVELS
#define N_ZOOM LM_N_ZOOM
#define ZOOM_DEFAULT LM_ZOOM_DEFAULT
#define MAX_MIMES 32
/* The buttons are a rail of round drops at the left (wlrail.h; MELLAGUI_TERM_RAIL=0 or MELLAGUI_RAILS=0 bring back the bar above the text). */
static bool g_rail;
#define BAR_H (g_rail ? 0 : (LM_FONT_H + 8))      /* button bar above the text area (not zoomed) */
#define RAILX (g_rail ? WLRAIL_W : 0)             /* the left rail's width: the grid starts that far in */
#define RAILR (g_rail ? WLRAIL_W : 0)             /* the right rail: the scroll bar is that far from the edge */
#define BAR_BG (lm_theme.bar)
#define BAR_BTN_ON (lm_theme.terminal_button_on)
#define MENU_BORDER (lm_theme.menu_border)
#define MENU_CURRENT (lm_theme.menu_current)
#define BAR_FG (lm_theme.bar_text_bright)
/* Cell size in pixels at the current zoom. */
#define CELL_W cell_w()
#define CELL_H cell_h()

#define DEFAULT_FG (lm_theme.text_soft)
#define DEFAULT_BG (lm_theme.background)
enum {
    ATTR_BOLD = 1, ATTR_UNDERLINE = 2, ATTR_REVERSE = 4, ATTR_DIM = 8, ATTR_ITALIC = 16,
    ATTR_WIDE = 32, ATTR_CONT = 64,         /* the left and the right cell of a character two cells wide */
};

struct cell {
    uint32_t cp;
    uint32_t fg, bg;
    uint8_t attr;
};

struct line {
    struct cell *cells;     /* cols entries (width at creation time) */
    int cols;
    bool wrapped;
};

enum pstate { P_GROUND, P_ESC, P_CSI, P_OSC, P_OSC_ESC, P_DCS, P_ESC_SKIP };

struct term {
    /* screen */
    int cols, rows;
    struct line *scr;           /* rows lines, active screen */
    struct line *main_scr, *alt_scr;
    bool alt_active;
    struct line *sb;            /* scrollback ring */
    int sb_count, sb_head;
    int view_off;               /* lines scrolled back (0 = live) */
    int cx, cy;
    bool wrap_pending;
    int top, bot;               /* scroll region, inclusive */
    uint32_t fg, bg;
    uint8_t attr;
    int saved_cx, saved_cy;
    uint32_t saved_fg, saved_bg;
    uint8_t saved_attr;
    bool autowrap, cursor_visible, app_cursor, bracketed, origin, insert;
    bool tabs[1024];
    /* parser */
    enum pstate ps;
    int params[32];
    int nparams;
    char priv_marker;
    char inter;
    char osc[512];
    int osc_len;
    uint32_t utf_cp;
    int utf_need;
    /* selection */
    bool sel_active, sel_dragging;
    int sel_x0, sel_y0, sel_x1, sel_y1;     /* absolute rows (scrollback aware) are kept view-relative */
};

#define MAX_TABS 16
#define KBD_DEPTH 8
enum { MOUSE_1000 = 1, MOUSE_1002 = 2, MOUSE_1003 = 4, MOUSE_SGR = 8, MOUSE_ON = MOUSE_1000 | MOUSE_1002 | MOUSE_1003 };
struct tab {
    struct term t;
    int pty;
    pid_t child;                /* > 0: our own child, we reap it */
    pid_t pid;                  /* the program's pid (also when it was handed over to us), 0 = gone */
    char title[256];            /* what the program set (OSC 0 / 2), or --title / the -e program's name */
    bool title_set;             /* title came from the program: the tab shows it, else the folder */
    char cwd[PATH_MAX];         /* the program's working folder, looked at about once a second */
    bool activity;              /* it printed something while another tab was in front */
    /* what the program asked for or did last; here and not in struct term, which is handed over raw */
    struct tab_modes {
        uint8_t mouse;          /* mouse reporting (MOUSE_*) */
        uint8_t kbd[2][KBD_DEPTH], kbd_n[2];        /* the keyboard protocol's flag stack of the main and the other screen */
        pid_t pgrp;             /* the foreground process group that asked: the modes go when it is gone (it crashed) */
    } m;
    uint32_t last_cp;           /* the last character printed (CSI b repeats it) */
    bool zwj;                   /* the last thing was a zero width joiner: the next emoji belongs to the one before */
};

struct app {
    struct wlc *c;
    uint32_t registry, compositor, shm, wm_base, seat, decoration_mgr, ddm, menu;     /* menu: mellagui_menu_v1 (the right-click menu), 0 without it */
    uint32_t surface, xsurface, toplevel, deco, pointer, keyboard, ddev, dsrc;
    uint32_t frame_cb, pool, buffers[2];
    bool buf_busy[2];
    int bw, bh;
    uint32_t *pixels;
    size_t map_size;
    bool configured, running, dirty, focused;
    int win_w, win_h;
    struct tab *tabs[MAX_TABS];
    int ntabs, cur;
    pid_t zombies[MAX_TABS];    /* children handed over to another window: reaped here when they end */
    int nzombies;
    struct wltabs_drag tdrag;   /* a tab is being dragged */
    int thover;                 /* tab under the pointer: 2 * index (+1 over its x mark), -1 none, -2 the + button */
    bool status;                /* the status bar at the bottom */
    int status_conf;            /* what terminal.conf says about it (-1: nothing: the line is only written once the user has chosen) */
    int64_t cwd_ms;             /* when the working folders were read */
    uint32_t mods;
    uint32_t last_serial;
    int repeat_fd;
    int repeat_rate, repeat_delay;
    uint32_t repeat_code;
    double px, py;
    char *clip_out;             /* text we offer */
    bool have_offer;
    uint32_t offer_id;
    char offer_mimes[MAX_MIMES][64];
    int n_offer_mimes;
    int zoom;                   /* font size in percent (one of ZOOM_LEVELS, see set_zoom_pct) */
    /* weights of the source font pixels under each destination pixel of a cell (fractional zooms), see build_zoom_tables */
    struct lm_zt xt[96], yt[192];
    bool fixed_size;            /* maximized / fullscreen: the compositor decides the size, zooming only changes the grid */
    bool grown;                 /* the window was made bigger by a zoom and the new buffer is not committed yet */
    int hover;                  /* button under the pointer (-1 = none) */
    double wheel_acc;           /* wheel travel not yet reported to a program that watches the mouse */
    struct wlrail_anim ran[2];  /* the springs of the left and the right rail */
    int follow;                 /* a text size button was pressed: the pointer follows it when the window has changed (its place in the right rail, -1 = none) */
    int follow_w;               /* the window's width when it was pressed */
    int64_t follow_ms;
    bool menu_open;             /* the zoom drop-down list is showing */
    int menu_hover;             /* its highlighted entry (index into ZOOM_LEVELS) */
    bool conf_dirty;            /* the zoom changed and is not in the settings file yet */
    int64_t conf_time;          /* when (ms) it changed: saved a moment later so a wheel burst writes once */
    bool sb_hot;                /* the pointer is over the scroll bar */
    bool sb_drag;               /* the scroll bar thumb is being dragged */
    int sb_grab;                /* pixels between the top of the thumb and the pointer */
    bool eat_release;           /* the press that closed the menu: its release must not start / end a selection */
    char title[256];            /* the window's title now */
    /* HiDPI: the buffer has `k` times as many pixels as the window has logical ones (the biggest wl_output scale); bw / bh stay logical */
    int k, pw, ph;
    uint32_t out_id[8];
    int out_scale[8], nout;
};

static struct app A;
#define CUR (A.tabs[A.cur])
static struct tab *g_feed;      /* the tab whose output term_feed is working on (its replies and title belong to it) */

static void modes_asked(struct tab *tb)         /* remember who asked, see modes_check() */
{
    tb->m.pgrp = tcgetpgrp(tb->pty);
}

static void modes_check(struct tab *tb)         /* a program that died without taking its modes back leaves a usable shell */
{
    if (tb->m.pgrp > 0 && kill(-tb->m.pgrp, 0) < 0 && errno == ESRCH)
        memset(&tb->m, 0, sizeof(tb->m));
}

static int want_k(void)
{
    int k = 1;
    for (int i = 0; i < A.nout; i++)
        k = LM_MAX(k, A.out_scale[i]);
    return LM_CLAMP(k, 1, 4);
}

static void output_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)data;
    if (op != LM_WL_OUTPUT_EVT_SCALE)
        return;
    for (int i = 0; i < A.nout; i++)
        if (A.out_id[i] == id) {
            A.out_scale[i] = LM_MAX(args[0].i, 1);
            if (A.k != want_k())
                A.dirty = true;                                 /* render() makes the new buffer */
        }
}

static int cell_w(void) { return LM_MAX((A.zoom * LM_FONT_W + 50) / 100, 1); }
static int cell_h(void) { return LM_MAX((A.zoom * LM_FONT_H + 50) / 100, 1); }
static int sb_w(void) { return lm_metrics.scrollbar_width; }      /* [windows] scrollbar-width: the strip at the right edge */

/* The window from the top: [button bar] [tab bar, with two tabs or more] grid ... [status bar]. */
static bool tabbar_on(void) { return A.ntabs > 1; }
static int tabs_h(void) { return tabbar_on() ? WLTABS_H : 0; }
static int top_h(void) { return BAR_H + tabs_h(); }                 /* where the grid's area starts */
static int stat_h(void) { return A.status ? LM_FONT_H + 6 : 0; }
static int grid_bottom(void) { return A.bh - stat_h(); }            /* where it ends */
static int tabbar_x(void) { return RAILX; }
static int tabbar_w(void) { return A.bw - RAILX - RAILR; }

/* ======================================================================== *
 *  Terminal model
 * ======================================================================== */

static struct line line_new(int cols)
{
    struct line l = { .cells = calloc((size_t)cols, sizeof(struct cell)), .cols = cols };
    for (int i = 0; i < cols; i++) {
        l.cells[i].cp = ' ';
        l.cells[i].fg = DEFAULT_FG;
        l.cells[i].bg = DEFAULT_BG;
    }
    return l;
}

static void line_clear(struct line *l, uint32_t fg, uint32_t bg)
{
    for (int i = 0; i < l->cols; i++)
        l->cells[i] = (struct cell){ ' ', fg, bg, 0 };
    l->wrapped = false;
}

static struct line *screen_new(int rows, int cols)
{
    struct line *s = calloc((size_t)rows, sizeof(*s));
    for (int i = 0; i < rows; i++)
        s[i] = line_new(cols);
    return s;
}

static void screen_free(struct line *s, int rows)
{
    if (!s)
        return;
    for (int i = 0; i < rows; i++)
        free(s[i].cells);
    free(s);
}

static void tabs_reset(struct term *t)
{
    memset(t->tabs, 0, sizeof(t->tabs));
    for (int i = 8; i < (int)sizeof(t->tabs); i += 8)
        t->tabs[i] = true;
}

static void term_reset(struct term *t)
{
    t->fg = DEFAULT_FG;
    t->bg = DEFAULT_BG;
    t->attr = 0;
    t->cx = t->cy = 0;
    t->wrap_pending = false;
    t->top = 0;
    t->bot = t->rows - 1;
    t->autowrap = true;
    t->cursor_visible = true;
    t->app_cursor = false;
    t->bracketed = false;
    t->origin = false;
    t->insert = false;
    t->ps = P_GROUND;
    t->utf_need = 0;
    tabs_reset(t);
    for (int i = 0; i < t->rows; i++)
        line_clear(&t->scr[i], t->fg, t->bg);
}

static void term_init(struct term *t, int cols, int rows)
{
    memset(t, 0, sizeof(*t));
    t->cols = cols;
    t->rows = rows;
    t->main_scr = screen_new(rows, cols);
    t->alt_scr = screen_new(rows, cols);
    t->scr = t->main_scr;
    t->sb = calloc(SCROLLBACK, sizeof(struct line));
    term_reset(t);
}

static void sb_push(struct term *t, const struct line *l)
{
    if (t->alt_active)
        return;
    struct line *slot = &t->sb[t->sb_head];
    free(slot->cells);
    slot->cells = malloc((size_t)l->cols * sizeof(struct cell));
    memcpy(slot->cells, l->cells, (size_t)l->cols * sizeof(struct cell));
    slot->cols = l->cols;
    slot->wrapped = l->wrapped;
    t->sb_head = (t->sb_head + 1) % SCROLLBACK;
    if (t->sb_count < SCROLLBACK)
        t->sb_count++;
    if (t->view_off > 0 && t->view_off < t->sb_count)
        t->view_off++;
}

static void scroll_up(struct term *t, int n)
{
    if (n > t->bot - t->top + 1)
        n = t->bot - t->top + 1;
    for (int k = 0; k < n; k++) {
        if (t->top == 0 && !t->alt_active)
            sb_push(t, &t->scr[0]);
        struct cell *first = t->scr[t->top].cells;
        for (int i = t->top; i < t->bot; i++)
            t->scr[i] = t->scr[i + 1];
        t->scr[t->bot].cells = first;
        line_clear(&t->scr[t->bot], t->fg, t->bg);
    }
}

static void scroll_down(struct term *t, int n)
{
    if (n > t->bot - t->top + 1)
        n = t->bot - t->top + 1;
    for (int k = 0; k < n; k++) {
        struct cell *last = t->scr[t->bot].cells;
        for (int i = t->bot; i > t->top; i--)
            t->scr[i] = t->scr[i - 1];
        t->scr[t->top].cells = last;
        line_clear(&t->scr[t->top], t->fg, t->bg);
    }
}

static void linefeed(struct term *t)
{
    if (t->cy == t->bot)
        scroll_up(t, 1);
    else if (t->cy < t->rows - 1)
        t->cy++;
}

static void blank_cell(struct cell *c)          /* empties a cell, keeping its colours */
{
    c->cp = ' ';
    c->attr &= (uint8_t)~(ATTR_WIDE | ATTR_CONT);
}

/* Cells x0..x1 of row y are about to change: a wide character with only one half inside goes altogether. */
static void split_wide(struct term *t, int y, int x0, int x1)
{
    struct cell *row = t->scr[y].cells;
    if (x0 > 0 && x0 < t->cols && (row[x0].attr & ATTR_CONT))
        blank_cell(&row[x0 - 1]);
    if (x1 >= 0 && x1 + 1 < t->cols && (row[x1].attr & ATTR_WIDE))
        blank_cell(&row[x1 + 1]);
}

/* A character that goes with the one before it: an accent, the "show as emoji" selector, a skin tone, a joined emoji. */
static bool join_char(struct term *t, uint32_t cp, int w)
{
    bool joined = g_feed && g_feed->zwj;
    if (g_feed)
        g_feed->zwj = cp == 0x200d;
    struct cell *row = t->scr[t->cy].cells;
    int x = t->wrap_pending ? t->cx : t->cx - 1;
    if (x > 0 && (row[x].attr & ATTR_CONT))
        x--;
    struct cell *prev = x >= 0 ? &row[x] : NULL;
    bool emoji = prev && (prev->attr & ATTR_WIDE) && prev->cp >= 0x2300;
    if (w == 2 && emoji && (joined || (cp >= 0x1f3fb && cp <= 0x1f3ff)))
        return true;
    if (w != 0)
        return false;
    if (!prev)
        return true;
    uint32_t both = lm_compose(prev->cp, cp);
    if (both) {
        prev->cp = both;
    } else if (cp == 0xfe0f && prev->cp >= 0x2100 && !(prev->attr & ATTR_WIDE) && !t->wrap_pending && x == t->cx - 1 && t->cx < t->cols) {
        split_wide(t, t->cy, t->cx, t->cx);                    /* a symbol shown as an emoji takes two cells */
        prev->attr |= ATTR_WIDE;
        row[t->cx] = (struct cell){ ' ', prev->fg, prev->bg, (uint8_t)((prev->attr & ~ATTR_WIDE) | ATTR_CONT) };
        if (t->cx == t->cols - 1)
            t->wrap_pending = true;
        else
            t->cx++;
    }
    return true;
}

static void put_char(struct term *t, uint32_t cp)
{
    int w = lm_char_width(cp);
    if (join_char(t, cp, w))
        return;
    if (g_feed)
        g_feed->last_cp = cp;
    if (t->wrap_pending && t->autowrap) {
        t->scr[t->cy].wrapped = true;
        t->cx = 0;
        linefeed(t);
    }
    t->wrap_pending = false;
    if (w == 2 && t->cx == t->cols - 1) {                       /* no room for both halves */
        if (t->autowrap) {
            split_wide(t, t->cy, t->cx, t->cx);
            blank_cell(&t->scr[t->cy].cells[t->cx]);
            t->scr[t->cy].wrapped = true;
            t->cx = 0;
            linefeed(t);
        } else {
            t->cx--;
        }
    }
    struct cell *row = t->scr[t->cy].cells;
    if (t->insert && t->cx + w < t->cols) {
        split_wide(t, t->cy, t->cx, t->cx - 1);
        memmove(&row[t->cx + w], &row[t->cx], (size_t)(t->cols - t->cx - w) * sizeof(struct cell));
    } else {
        split_wide(t, t->cy, t->cx, t->cx + w - 1);
    }
    row[t->cx] = (struct cell){ cp, t->fg, t->bg, (uint8_t)(t->attr | (w == 2 ? ATTR_WIDE : 0)) };
    if (w == 2)
        row[t->cx + 1] = (struct cell){ ' ', t->fg, t->bg, (uint8_t)(t->attr | ATTR_CONT) };
    if (t->cx + w >= t->cols) {
        t->cx = t->cols - 1;
        t->wrap_pending = true;
    } else {
        t->cx += w;
    }
}

static void erase_cells(struct term *t, int y, int x0, int x1)
{
    split_wide(t, y, x0, LM_MIN(x1, t->cols - 1));
    for (int x = x0; x <= x1 && x < t->cols; x++)
        t->scr[y].cells[x] = (struct cell){ ' ', t->fg, t->bg, 0 };
}

static uint32_t palette256(int i)
{
    static const uint32_t base[16] = {
        0x1d1f21, 0xcc6666, 0xb5bd68, 0xf0c674, 0x81a2be, 0xb294bb, 0x8abeb7, 0xc5c8c6,
        0x666666, 0xff7b7b, 0xc9e07a, 0xffd780, 0x9fc3e8, 0xd3a8e0, 0x9ee3da, 0xffffff,
    };
    if (i < 16)
        return base[i];
    if (i < 232) {
        i -= 16;
        int r = i / 36, g = (i / 6) % 6, b = i % 6;
        static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
        return (uint32_t)lv[r] << 16 | (uint32_t)lv[g] << 8 | (uint32_t)lv[b];
    }
    int v = 8 + (i - 232) * 10;
    return (uint32_t)v << 16 | (uint32_t)v << 8 | (uint32_t)v;
}

static void pty_write(const void *buf, size_t n);

static int param(const struct term *t, int i, int def)
{
    if (i >= t->nparams || t->params[i] <= 0)
        return def;
    return t->params[i];
}

static void sgr(struct term *t)
{
    if (t->nparams == 0) {
        t->params[0] = 0;
        t->nparams = 1;
    }
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i];
        if (p == 0) { t->fg = DEFAULT_FG; t->bg = DEFAULT_BG; t->attr = 0; }
        else if (p == 1) t->attr |= ATTR_BOLD;
        else if (p == 2) t->attr |= ATTR_DIM;
        else if (p == 3) t->attr |= ATTR_ITALIC;
        else if (p == 4) t->attr |= ATTR_UNDERLINE;
        else if (p == 7) t->attr |= ATTR_REVERSE;
        else if (p == 22) t->attr &= (uint8_t)~(ATTR_BOLD | ATTR_DIM);
        else if (p == 23) t->attr &= (uint8_t)~ATTR_ITALIC;
        else if (p == 24) t->attr &= (uint8_t)~ATTR_UNDERLINE;
        else if (p == 27) t->attr &= (uint8_t)~ATTR_REVERSE;
        else if (p >= 30 && p <= 37) t->fg = palette256(p - 30);
        else if (p == 39) t->fg = DEFAULT_FG;
        else if (p >= 40 && p <= 47) t->bg = palette256(p - 40);
        else if (p == 49) t->bg = DEFAULT_BG;
        else if (p >= 90 && p <= 97) t->fg = palette256(p - 90 + 8);
        else if (p >= 100 && p <= 107) t->bg = palette256(p - 100 + 8);
        else if (p == 38 || p == 48) {
            uint32_t *dst = p == 38 ? &t->fg : &t->bg;
            if (i + 1 < t->nparams && t->params[i + 1] == 5 && i + 2 < t->nparams) {
                *dst = palette256(LM_CLAMP(t->params[i + 2], 0, 255));
                i += 2;
            } else if (i + 1 < t->nparams && t->params[i + 1] == 2 && i + 4 < t->nparams) {
                *dst = (uint32_t)LM_CLAMP(t->params[i + 2], 0, 255) << 16 |
                       (uint32_t)LM_CLAMP(t->params[i + 3], 0, 255) << 8 |
                       (uint32_t)LM_CLAMP(t->params[i + 4], 0, 255);
                i += 4;
            }
        }
    }
}

static void switch_screen(struct term *t, bool alt)
{
    if (alt == t->alt_active)
        return;
    t->alt_active = alt;
    t->scr = alt ? t->alt_scr : t->main_scr;
    if (alt)
        for (int i = 0; i < t->rows; i++)
            line_clear(&t->scr[i], DEFAULT_FG, DEFAULT_BG);
    t->view_off = 0;
}

static void set_mode(struct term *t, bool on)
{
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i];
        if (t->priv_marker == '?') {
            switch (p) {
            case 1: t->app_cursor = on; break;
            case 6: t->origin = on; t->cx = 0; t->cy = on ? t->top : 0; break;
            case 7: t->autowrap = on; break;
            case 25: t->cursor_visible = on; break;
            case 47: case 1047: switch_screen(t, on); break;
            case 1048:
                if (on) { t->saved_cx = t->cx; t->saved_cy = t->cy; }
                else { t->cx = t->saved_cx; t->cy = t->saved_cy; }
                break;
            case 1049:
                if (on) { t->saved_cx = t->cx; t->saved_cy = t->cy; switch_screen(t, true); }
                else { switch_screen(t, false); t->cx = t->saved_cx; t->cy = t->saved_cy; }
                break;
            case 2004: t->bracketed = on; break;
            case 1000: case 1002: case 1003: case 1006:
                if (g_feed) {
                    uint8_t bit = p == 1000 ? MOUSE_1000 : p == 1002 ? MOUSE_1002 : p == 1003 ? MOUSE_1003 : MOUSE_SGR;
                    g_feed->m.mouse = (uint8_t)(on ? g_feed->m.mouse | bit : g_feed->m.mouse & ~bit);
                    if (on)
                        modes_asked(g_feed);
                }
                break;
            }
        } else if (p == 4) {
            t->insert = on;
        }
    }
}

/* The keyboard protocol of kitty (https://sw.kovidgoyal.net/kitty/keyboard-protocol/), of which we speak flag 1: keys that
 * the old way cannot tell apart (Shift+Enter, Esc, Ctrl+letter) are sent as CSI code ; modifiers u. */
static void kbd_command(struct term *t)
{
    struct tab *tb = g_feed;
    if (!tb)
        return;
    int s = t->alt_active ? 1 : 0, n = tb->m.kbd_n[s];
    int flags = (t->nparams ? t->params[0] : 0) & 1;
    if (t->priv_marker == '?') {
        char buf[16];
        int l = snprintf(buf, sizeof(buf), "\033[?%du", n ? tb->m.kbd[s][n - 1] : 0);
        pty_write(buf, (size_t)l);
        return;
    }
    if (t->priv_marker == '>') {                                /* push */
        if (n == KBD_DEPTH) {
            memmove(&tb->m.kbd[s][0], &tb->m.kbd[s][1], KBD_DEPTH - 1);
            n--;
        }
        tb->m.kbd[s][n++] = (uint8_t)flags;
        modes_asked(tb);
    } else if (t->priv_marker == '<') {                         /* pop */
        n = LM_MAX(n - param(t, 0, 1), 0);
    } else if (t->priv_marker == '=') {                         /* change the flags in force */
        int how = param(t, 1, 1);
        if (!n)
            tb->m.kbd[s][n++] = 0;
        uint8_t *f = &tb->m.kbd[s][n - 1];
        *f = (uint8_t)(how == 2 ? *f | flags : how == 3 ? *f & ~flags : flags);
        modes_asked(tb);
    }
    tb->m.kbd_n[s] = (uint8_t)n;
}

static void csi_dispatch(struct term *t, char final)
{
    int n;
    /* A prefix makes it another command: CSI ? u (a keyboard protocol query) is not "restore the cursor", CSI > 4 m is not a colour.
     * Of those only the '?' forms below are ours ('c' looks at the prefix itself). */
    if (t->priv_marker && final == 'u') {
        kbd_command(t);
        return;
    }
    if (t->priv_marker && final != 'c' && !(t->priv_marker == '?' && strchr("hlJKn", final)))
        return;
    switch (final) {
    case 'b':                                                   /* repeat the character before (ncurses programs use it) */
        if (g_feed && g_feed->last_cp)
            for (n = LM_MIN(param(t, 0, 1), t->cols * t->rows); n > 0; n--)
                put_char(t, g_feed->last_cp);
        break;
    case 'A': t->cy = LM_MAX(t->cy - param(t, 0, 1), t->cy >= t->top ? t->top : 0); t->wrap_pending = false; break;
    case 'B': t->cy = LM_MIN(t->cy + param(t, 0, 1), t->cy <= t->bot ? t->bot : t->rows - 1); t->wrap_pending = false; break;
    case 'C': t->cx = LM_MIN(t->cx + param(t, 0, 1), t->cols - 1); t->wrap_pending = false; break;
    case 'D': t->cx = LM_MAX(t->cx - param(t, 0, 1), 0); t->wrap_pending = false; break;
    case 'E': t->cx = 0; t->cy = LM_MIN(t->cy + param(t, 0, 1), t->rows - 1); t->wrap_pending = false; break;
    case 'F': t->cx = 0; t->cy = LM_MAX(t->cy - param(t, 0, 1), 0); t->wrap_pending = false; break;
    case 'G': case '`': t->cx = LM_CLAMP(param(t, 0, 1) - 1, 0, t->cols - 1); t->wrap_pending = false; break;
    case 'H': case 'f': {
        int r = param(t, 0, 1) - 1, c = param(t, 1, 1) - 1;
        if (t->origin)
            r += t->top;
        t->cy = LM_CLAMP(r, 0, t->rows - 1);
        t->cx = LM_CLAMP(c, 0, t->cols - 1);
        t->wrap_pending = false;
        break;
    }
    case 'd': t->cy = LM_CLAMP(param(t, 0, 1) - 1, 0, t->rows - 1); t->wrap_pending = false; break;
    case 'J':
        n = t->nparams ? t->params[0] : 0;
        if (n == 0) {
            erase_cells(t, t->cy, t->cx, t->cols - 1);
            for (int y = t->cy + 1; y < t->rows; y++) erase_cells(t, y, 0, t->cols - 1);
        } else if (n == 1) {
            for (int y = 0; y < t->cy; y++) erase_cells(t, y, 0, t->cols - 1);
            erase_cells(t, t->cy, 0, t->cx);
        } else if (n == 2 || n == 3) {
            for (int y = 0; y < t->rows; y++) erase_cells(t, y, 0, t->cols - 1);
            if (n == 3) { t->sb_count = 0; t->view_off = 0; }
        }
        break;
    case 'K':
        n = t->nparams ? t->params[0] : 0;
        if (n == 0) erase_cells(t, t->cy, t->cx, t->cols - 1);
        else if (n == 1) erase_cells(t, t->cy, 0, t->cx);
        else if (n == 2) erase_cells(t, t->cy, 0, t->cols - 1);
        break;
    case 'L':
        if (t->cy >= t->top && t->cy <= t->bot) {
            int save = t->top;
            t->top = t->cy;
            scroll_down(t, param(t, 0, 1));
            t->top = save;
        }
        break;
    case 'M':
        if (t->cy >= t->top && t->cy <= t->bot) {
            int save = t->top;
            t->top = t->cy;
            int sbc = t->sb_count;
            scroll_up(t, param(t, 0, 1));
            t->sb_count = sbc; /* deleting lines inside a region never feeds the scrollback */
            t->top = save;
        }
        break;
    case '@': {
        n = LM_MIN(param(t, 0, 1), t->cols - t->cx);
        struct cell *row = t->scr[t->cy].cells;
        memmove(&row[t->cx + n], &row[t->cx], (size_t)(t->cols - t->cx - n) * sizeof(struct cell));
        erase_cells(t, t->cy, t->cx, t->cx + n - 1);
        break;
    }
    case 'P': {
        n = LM_MIN(param(t, 0, 1), t->cols - t->cx);
        struct cell *row = t->scr[t->cy].cells;
        memmove(&row[t->cx], &row[t->cx + n], (size_t)(t->cols - t->cx - n) * sizeof(struct cell));
        erase_cells(t, t->cy, t->cols - n, t->cols - 1);
        break;
    }
    case 'X': erase_cells(t, t->cy, t->cx, t->cx + param(t, 0, 1) - 1); break;
    case 'S': scroll_up(t, param(t, 0, 1)); break;
    case 'T': scroll_down(t, param(t, 0, 1)); break;
    case 'm': sgr(t); break;
    case 'r': {
        int top = param(t, 0, 1) - 1, bot = param(t, 1, t->rows) - 1;
        if (top < 0) top = 0;
        if (bot >= t->rows) bot = t->rows - 1;
        if (top < bot) {
            t->top = top;
            t->bot = bot;
        }
        t->cx = 0;
        t->cy = t->origin ? t->top : 0;
        t->wrap_pending = false;
        break;
    }
    case 's': t->saved_cx = t->cx; t->saved_cy = t->cy; break;
    case 'u': t->cx = t->saved_cx; t->cy = t->saved_cy; t->wrap_pending = false; break;
    case 'h': set_mode(t, true); break;
    case 'l': set_mode(t, false); break;
    case 'c':
        if (t->priv_marker == 0 || t->priv_marker == '?')
            pty_write("\033[?6c", 5);
        break;
    case 'n':
        if (t->nparams && t->params[0] == 5) {
            pty_write("\033[0n", 4);
        } else if (t->nparams && t->params[0] == 6) {
            char buf[32];
            int l = snprintf(buf, sizeof(buf), "\033[%d;%dR", t->cy + 1 - (t->origin ? t->top : 0), t->cx + 1);
            pty_write(buf, (size_t)l);
        }
        break;
    case 'g':
        if (!t->nparams || t->params[0] == 0) t->tabs[t->cx] = false;
        else if (t->params[0] == 3) memset(t->tabs, 0, sizeof(t->tabs));
        break;
    case 'q': case 't': break;
    default: break;
    }
}

static void set_title(const char *s);

static void esc_dispatch(struct term *t, char c)
{
    switch (c) {
    case '7': t->saved_cx = t->cx; t->saved_cy = t->cy; t->saved_fg = t->fg; t->saved_bg = t->bg; t->saved_attr = t->attr; break;
    case '8': t->cx = t->saved_cx; t->cy = t->saved_cy; t->fg = t->saved_fg; t->bg = t->saved_bg; t->attr = t->saved_attr; t->wrap_pending = false; break;
    case 'D': linefeed(t); break;
    case 'E': t->cx = 0; linefeed(t); break;
    case 'M':
        if (t->cy == t->top) scroll_down(t, 1);
        else if (t->cy > 0) t->cy--;
        break;
    case 'H': t->tabs[t->cx] = true; break;
    case 'c':
        term_reset(t);
        if (g_feed)
            memset(&g_feed->m, 0, sizeof(g_feed->m));
        break;
    default: break;
    }
}

static void term_feed(struct term *t, const unsigned char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char b = buf[i];

        if (t->ps == P_OSC || t->ps == P_OSC_ESC) {
            /* not the 8 bit ST (0x9c): the text is UTF-8, where that byte is inside letters (U+2733 is e2 9c b3) */
            if (b == 0x07 || (t->ps == P_OSC_ESC && b == '\\')) {
                t->osc[t->osc_len] = 0;
                if (t->osc_len > 2 && (t->osc[0] == '0' || t->osc[0] == '2') && t->osc[1] == ';')
                    set_title(t->osc + 2);
                t->ps = P_GROUND;
            } else if (b == 0x1b) {
                t->ps = P_OSC_ESC;
            } else {
                t->ps = P_OSC;
                if (t->osc_len < (int)sizeof(t->osc) - 1)
                    t->osc[t->osc_len++] = (char)b;
            }
            continue;
        }
        if (t->ps == P_DCS) {
            if (b == 0x1b || b == 0x07)
                t->ps = b == 0x1b ? P_ESC_SKIP : P_GROUND;
            continue;
        }
        if (t->ps == P_ESC_SKIP) {
            t->ps = P_GROUND;
            continue;
        }

        if (b == 0x1b) { t->ps = P_ESC; t->inter = 0; continue; }
        if (b < 0x20 || b == 0x7f) {
            /* C0 controls work in every state except inside OSC/DCS */
            switch (b) {
            case 0x07: break;
            case 0x08: if (t->cx > 0) t->cx--; t->wrap_pending = false; break;
            case 0x09: {
                int x = t->cx + 1;
                while (x < t->cols - 1 && !t->tabs[x]) x++;
                t->cx = LM_MIN(x, t->cols - 1);
                t->wrap_pending = false;
                break;
            }
            case 0x0a: case 0x0b: case 0x0c: linefeed(t); t->wrap_pending = false; break;
            case 0x0d: t->cx = 0; t->wrap_pending = false; break;
            default: break;
            }
            continue;
        }

        switch (t->ps) {
        case P_ESC:
            if (b == '[') {
                t->ps = P_CSI;
                t->nparams = 0;
                t->priv_marker = 0;
                memset(t->params, 0, sizeof(t->params));
            } else if (b == ']') {
                t->ps = P_OSC;
                t->osc_len = 0;
            } else if (b == 'P' || b == '^' || b == '_' || b == 'X') {
                t->ps = P_DCS;
            } else if (b == '(' || b == ')' || b == '*' || b == '+' || b == '#' || b == '%') {
                t->ps = P_ESC_SKIP;      /* charset / DEC test designators: swallow one more byte */
            } else {
                esc_dispatch(t, (char)b);
                t->ps = P_GROUND;
            }
            break;
        case P_CSI:
            if (b >= '0' && b <= '9') {
                if (t->nparams == 0)
                    t->nparams = 1;
                if (t->nparams <= 32) {
                    int *p = &t->params[t->nparams - 1];
                    if (*p < 100000)
                        *p = *p * 10 + (b - '0');
                }
            } else if (b == ';') {
                if (t->nparams == 0)
                    t->nparams = 1;
                if (t->nparams < 32)
                    t->nparams++;
            } else if (b == ':') {
                /* sub-parameters (e.g. 38:2:r:g:b): treat as separators */
                if (t->nparams < 32)
                    t->nparams++;
            } else if (b == '?' || b == '>' || b == '<' || b == '=') {
                t->priv_marker = (char)b;
            } else if (b >= 0x20 && b <= 0x2f) {
                t->inter = (char)b;
            } else if (b >= 0x40 && b <= 0x7e) {
                if (!t->inter)
                    csi_dispatch(t, (char)b);
                t->ps = P_GROUND;
            } else {
                t->ps = P_GROUND;
            }
            break;
        default:
            /* ground: UTF-8 */
            if (t->utf_need > 0) {
                if ((b & 0xc0) == 0x80) {
                    t->utf_cp = (t->utf_cp << 6) | (b & 0x3f);
                    if (--t->utf_need == 0)
                        put_char(t, t->utf_cp);
                    continue;
                }
                t->utf_need = 0;
                put_char(t, 0xfffd);
            }
            if (b < 0x80) {
                put_char(t, b);
            } else if ((b & 0xe0) == 0xc0) {
                t->utf_cp = b & 0x1f; t->utf_need = 1;
            } else if ((b & 0xf0) == 0xe0) {
                t->utf_cp = b & 0x0f; t->utf_need = 2;
            } else if ((b & 0xf8) == 0xf0) {
                t->utf_cp = b & 0x07; t->utf_need = 3;
            } else {
                put_char(t, 0xfffd);
            }
        }
    }
}

static void term_resize(struct term *t, int cols, int rows)
{
    if (cols == t->cols && rows == t->rows)
        return;
    struct line *ns[2] = { NULL, NULL };
    for (int s = 0; s < 2; s++) {
        struct line *old = s == 0 ? t->main_scr : t->alt_scr;
        ns[s] = screen_new(rows, cols);
        /* Keep the bottom of the old screen when it shrinks (so the cursor line stays visible). */
        int shift = 0;
        if (s == 0 && !t->alt_active)
            shift = t->cy >= rows ? t->cy - rows + 1 : 0;
        for (int y = 0; y < rows; y++) {
            int oy = y + shift;
            if (oy >= t->rows)
                break;
            memcpy(ns[s][y].cells, old[oy].cells, (size_t)LM_MIN(cols, t->cols) * sizeof(struct cell));
        }
        if (s == 0) {
            for (int y = 0; y < shift; y++)
                sb_push(t, &old[y]);
        }
        screen_free(old, t->rows);
    }
    t->main_scr = ns[0];
    t->alt_scr = ns[1];
    t->scr = t->alt_active ? t->alt_scr : t->main_scr;
    int shift = (!t->alt_active && t->cy >= rows) ? t->cy - rows + 1 : 0;
    t->cy -= shift;
    t->cols = cols;
    t->rows = rows;
    t->cx = LM_MIN(t->cx, cols - 1);
    t->cy = LM_CLAMP(t->cy, 0, rows - 1);
    t->top = 0;
    t->bot = rows - 1;
    t->wrap_pending = false;
    t->sel_active = false;
    t->view_off = LM_MIN(t->view_off, t->sb_count);
}

/* ======================================================================== *
 *  pty
 * ======================================================================== */

static void pty_write(const void *buf, size_t n)
{
    int fd = (g_feed ? g_feed : CUR)->pty;
    const char *p = buf;
    while (n > 0) {
        ssize_t k = write(fd, p, n);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                struct pollfd pf = { .fd = fd, .events = POLLOUT };
                poll(&pf, 1, 100);
                continue;
            }
            return;
        }
        p += k;
        n -= (size_t)k;
    }
}

static void pty_set_size(void)                  /* every tab's program is told the grid (they all have the same size) */
{
    for (int i = 0; i < A.ntabs; i++) {
        struct tab *tb = A.tabs[i];
        struct winsize ws = { .ws_row = (unsigned short)tb->t.rows, .ws_col = (unsigned short)tb->t.cols,
                              .ws_xpixel = (unsigned short)(tb->t.cols * CELL_W),
                              .ws_ypixel = (unsigned short)(tb->t.rows * CELL_H) };
        ioctl(tb->pty, TIOCSWINSZ, &ws);
    }
}

static char **g_exec_argv;                       /* -e: the command to run instead of the shell (NULL: the shell) */

/* Starts the shell (or `argv`) on a new pty for `tb` (its grid gives the size) in the folder `cwd` (NULL: where we are). Returns the master, -1 on failure. */
static int spawn_shell(struct tab *tb, char *const *argv, const char *cwd)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (m < 0 || grantpt(m) < 0 || unlockpt(m) < 0) {
        perror("mellagui-term: pty");
        return -1;
    }
    char name[128];
    if (ptsname_r(m, name, sizeof(name)) != 0) {
        perror("ptsname");
        return -1;
    }
    struct winsize ws = { .ws_row = (unsigned short)tb->t.rows, .ws_col = (unsigned short)tb->t.cols };
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        setsid();
        int s = open(name, O_RDWR);
        if (s < 0)
            _exit(127);
        ioctl(s, TIOCSCTTY, 0);
        ioctl(s, TIOCSWINSZ, &ws);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        if (s > 2)
            close(s);
        close(m);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        unsetenv("COLUMNS");
        unsetenv("LINES");
        if (cwd && *cwd && chdir(cwd) < 0)
            (void)!chdir(getenv("HOME") ? getenv("HOME") : "/");
        if (argv) {
            execvp(argv[0], argv);
            fprintf(stderr, "\r\nmellagui-term: cannot run %s: %s\r\n\r\nPress Enter to close this window.\r\n", argv[0], strerror(errno));
            char ch;
            while (read(0, &ch, 1) > 0 && ch != '\n' && ch != '\r')
                ;
            _exit(127);
        }
        const char *shell = getenv("SHELL");
        if (!shell || !*shell)
            shell = "/bin/sh";
        const char *base = strrchr(shell, '/');
        char arg0[128];
        snprintf(arg0, sizeof(arg0), "-%s", base ? base + 1 : shell);
        execl(shell, arg0, (char *)NULL);
        execl("/bin/sh", "sh", (char *)NULL);
        _exit(127);
    }
    tb->child = tb->pid = pid;
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);
    return m;
}

/* ======================================================================== *
 *  Rendering
 * ======================================================================== */

static void send_window_title(void)
{
    snprintf(A.title, sizeof(A.title), "%s", CUR->title);
    if (A.toplevel)
        WLC_SEND(A.c, A.toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = A.title });
}

static void set_title(const char *s)             /* the program (g_feed) set its title: the window shows the front tab's */
{
    struct tab *tb = g_feed ? g_feed : CUR;
    snprintf(tb->title, sizeof(tb->title), "%s", s);
    for (char *p = tb->title; *p; p++)                        /* what the program sends must not carry control characters on */
        if ((unsigned char)*p < 0x20 || *p == 0x7f)
            *p = ' ';
    tb->title_set = true;
    if (tb == CUR)
        send_window_title();
}

static const struct cell *cell_at_view(const struct term *t, int row, int col)
{
    /* row: 0..rows-1 in the visible window, taking scrollback offset into account */
    int src = row - t->view_off;
    if (src >= 0)
        return &t->scr[src].cells[col];
    int back = -src;           /* 1 = newest scrollback line */
    if (back > t->sb_count)
        return NULL;
    int idx = (t->sb_head - back + SCROLLBACK) % SCROLLBACK;
    const struct line *l = &t->sb[idx];
    if (col >= l->cols)
        return NULL;
    return &l->cells[col];
}

static bool selected(const struct term *t, int row, int col)
{
    bool moving = t->sel_dragging && (t->sel_x0 != t->sel_x1 || t->sel_y0 != t->sel_y1);       /* the selection shows while the button is still down */
    if (!t->sel_active && !moving)
        return false;
    int y0 = t->sel_y0, x0 = t->sel_x0, y1 = t->sel_y1, x1 = t->sel_x1;
    if (y0 > y1 || (y0 == y1 && x0 > x1)) {
        int ty = y0, tx = x0;
        y0 = y1; x0 = x1; y1 = ty; x1 = tx;
    }
    if (row < y0 || row > y1)
        return false;
    if (row == y0 && col < x0)
        return false;
    if (row == y1 && col > x1)
        return false;
    return true;
}

static uint32_t dim(uint32_t c)
{
    return ((c >> 17) & 0x7f) << 16 | ((c >> 9) & 0x7f) << 8 | ((c >> 1) & 0x7f);
}

static uint32_t brighten(uint32_t c)
{
    uint32_t r = LM_MIN(((c >> 16) & 255) + 40, 255u), g = LM_MIN(((c >> 8) & 255) + 40, 255u),
             b = LM_MIN((c & 255) + 40, 255u);
    return r << 16 | g << 8 | b;
}

/* ---- button bar: Paste, A-, zoom drop-down, A+, Reset ------------------------------------------------------------- */

enum { BTN_PASTE_ID, BTN_SMALLER_ID, BTN_ZOOM_ID, BTN_BIGGER_ID, BTN_RESET_ID, N_BTNS };
#define ZOOM_BTN_CHARS 4                /* "100%" */
#define ARROW_W 12                      /* room for the drop-down triangle */

static const char *btn_text(int i, char *buf, size_t n)
{
    switch (i) {
    case BTN_PASTE_ID: return "Paste";
    case BTN_SMALLER_ID: return "A-";
    case BTN_BIGGER_ID: return "A+";
    case BTN_RESET_ID: return "Reset";
    default: snprintf(buf, n, "%d%%", A.zoom); return buf;
    }
}

/* A button that does not fit in the window width is left out (from the right: Reset first), so a narrow window shows
 * Paste, A-, the zoom list ... and never draws over the edge. Its rectangle then has no width. */
static void btn_rect(int i, int *x, int *y, int *w, int *h)
{
    char tmp[16];
    int bx = 4;
    for (int k = 0; k < i; k++) {
        int chars = k == BTN_ZOOM_ID ? ZOOM_BTN_CHARS : (int)strlen(btn_text(k, tmp, sizeof(tmp)));
        bx += chars * LM_FONT_W + 16 + (k == BTN_ZOOM_ID ? ARROW_W : 0) + 4;
    }
    int chars = i == BTN_ZOOM_ID ? ZOOM_BTN_CHARS : (int)strlen(btn_text(i, tmp, sizeof(tmp)));
    *x = bx;
    *y = 2;
    *w = chars * LM_FONT_W + 16 + (i == BTN_ZOOM_ID ? ARROW_W : 0);
    *h = BAR_H - 4;
    if (A.bw > 0 && bx + *w + 4 > A.bw)
        *w = 0;
}

/* The rails: Paste and friends at the left, the text size at the right (item ids of the rail mode; the bar above the text has the BTN_ ones). */
enum { RB_NEW, RB_COPY, RB_PASTE, RB_CLEAR, RB_BOTTOM, RB_FILES, RB_BIGGER, RB_ZOOM, RB_SMALLER, RB_RESET, RB_N };
static const int rb_left[6] = { RB_NEW, RB_COPY, RB_PASTE, RB_CLEAR, RB_BOTTOM, RB_FILES };
static const int rb_right[4] = { RB_BIGGER, RB_ZOOM, RB_SMALLER, RB_RESET };
static void term_rail(struct wlrail *L, struct wlrail *R);

static int btn_at(double px, double py)
{
    if (g_rail) {
        struct wlrail L, R;
        term_rail(&L, &R);
        int k = wlrail_at(&L, px, py);
        if (k >= 0)
            return rb_left[k];
        k = wlrail_at(&R, px, py);
        return k >= 0 ? rb_right[k] : -1;
    }
    for (int i = 0; i < N_BTNS; i++) {
        int x, y, w, h;
        btn_rect(i, &x, &y, &w, &h);
        if (w > 0 && px >= x && px < x + w && py >= y && py < y + h)
            return i;
    }
    return -1;
}

/* All the drawing helpers take LOGICAL coordinates and write the buffer's own pixels (`A.k` times as many each way). */
static void put_text(uint32_t *px, int x, int y, const char *text, uint32_t color)
{
    int k = A.k, cw = LM_FONT_W * k, ch = LM_FONT_H * k;
    for (const char *l = text; *l; l++, x += LM_FONT_W) {
        const uint8_t *g = lm_font_cell((uint8_t)*l, cw, ch);
        for (int r = 0; r < ch; r++)
            for (int c = 0; c < cw; c++) {
                unsigned a = g[r * cw + c];
                int xx = x * k + c, yy = y * k + r;
                if (a && xx >= 0 && xx < A.pw && yy >= 0 && yy < A.ph) {
                    uint32_t *p = &px[(size_t)yy * (size_t)A.pw + (size_t)xx];
                    *p = lm_font_mix(*p, color, a);
                }
            }
    }
}

static void fill_box(uint32_t *px, int x, int y, int w, int h, uint32_t color)
{
    int k = A.k;
    for (int yy = LM_MAX(y * k, 0); yy < (y + h) * k && yy < A.ph; yy++)
        for (int xx = LM_MAX(x * k, 0); xx < (x + w) * k && xx < A.pw; xx++)
            px[(size_t)yy * (size_t)A.pw + (size_t)xx] = color;
}

static void drop_box(uint32_t *px, int x, int y, int w, int h, int r, uint32_t color)
{
    int k = A.k;
    lm_drop_px(px, A.pw, A.pw, A.ph, x * k, y * k, w * k, h * k, r * k, color, k);
}

static void round_box(uint32_t *px, int x, int y, int w, int h, int r, uint32_t color)
{
    int k = A.k;
    lm_round_fill(px, A.pw, A.pw, A.ph, x * k, y * k, w * k, h * k, r * k, color);
}

/* One colour for each button, a lighter one under the pointer (the same palette as the file manager's round buttons). */
static void btn_colors(int i, uint32_t *base, uint32_t *hover)
{
    const uint32_t b[N_BTNS] = { lm_theme.btn_green, lm_theme.btn_blue, lm_theme.btn_purple, lm_theme.btn_teal, lm_theme.btn_amber };
    const uint32_t h[N_BTNS] = { lm_theme.btn_green_lit, lm_theme.btn_blue_lit, lm_theme.btn_purple_lit, lm_theme.btn_teal_lit, lm_theme.btn_amber_lit };
    *base = b[i];
    *hover = h[i];
}

static void term_rail(struct wlrail *L, struct wlrail *R)
{
    static char zbuf[8];
    static const char *const ltips[6] = { "New tab  Ctrl+Shift+T", "Copy the selection", "Paste", "Clear the screen and the scrollback", "Back to the newest line", "Files in this folder" };
    static void (*const licons[6])(struct wlwin *, int, int, uint32_t, uint32_t) = { wlrail_ic_term, wlrail_ic_copy, wlrail_ic_paste, NULL, wlrail_ic_down, wlrail_ic_open };
    const uint32_t lbase[6] = { lm_theme.btn_teal, lm_theme.btn_sky, lm_theme.btn_green, lm_theme.btn_amber, lm_theme.btn_slate, lm_theme.btn_blue };
    const uint32_t llite[6] = { lm_theme.btn_teal_lit, lm_theme.btn_sky_lit, lm_theme.btn_green_lit, lm_theme.btn_amber_lit, lm_theme.btn_slate_lit, lm_theme.btn_blue_lit };
    static const char *const rtips[4] = { "Bigger text  Ctrl++", "Text size: a list of sizes", "Smaller text  Ctrl+-", "Normal size  Ctrl+0" };
    const uint32_t rbase[4] = { lm_theme.btn_teal, lm_theme.btn_purple, lm_theme.btn_blue, lm_theme.btn_amber };
    const uint32_t rlite[4] = { lm_theme.btn_teal_lit, lm_theme.btn_purple_lit, lm_theme.btn_blue_lit, lm_theme.btn_amber_lit };
    memset(L, 0, sizeof(*L));
    memset(R, 0, sizeof(*R));
    L->x = 0;
    R->x = A.bw > 0 ? A.bw - WLRAIL_W : 0;
    L->y0 = R->y0 = 0;
    L->y1 = R->y1 = A.bh > 0 ? grid_bottom() : 400;
    L->n = 6;
    for (int i = 0; i < 6; i++)
        L->it[i] = (struct wlrail_item){ .tip = ltips[i], .text = i == 3 ? "Cl" : NULL, .icon = licons[i], .base = lbase[i], .lite = llite[i], .show = true,
                                         .idle = i == 1 && !CUR->t.sel_active, .gap = i == 1 || i == 3 || i == 5 };
    snprintf(zbuf, sizeof(zbuf), "%d", A.zoom);
    const char *rtext[4] = { "A+", zbuf, "A-", "1:1" };
    R->n = 4;
    for (int i = 0; i < 4; i++)
        R->it[i] = (struct wlrail_item){ .tip = rtips[i], .text = rtext[i], .base = rbase[i], .lite = rlite[i], .show = true, .lit = i == 1 && A.menu_open,
                                         .bottom = i == 3 };
}

static int rb_hover(bool right)
{
    const int *ids = right ? rb_right : rb_left;
    for (int k = 0; k < (right ? 4 : 6); k++)
        if (A.hover == ids[k])
            return k;
    return -1;
}

/* The rails are painted with the wlwin_* calls on a stand-in window that points at this buffer. */
static void draw_rail(uint32_t *px)
{
    struct wlwin sh;
    memset(&sh, 0, sizeof(sh));
    sh.pixels = px;
    sh.pw = A.pw;
    sh.ph = A.ph;
    sh.scale = A.k;
    sh.bw = A.bw;
    sh.bh = A.bh;
    struct wlrail L, R;
    term_rail(&L, &R);
    int64_t now = wlrail_now();
    wlrail_paint(&sh, &L, &A.ran[0], rb_hover(false), now, BAR_BG, BAR_BG, lm_theme.accent, BAR_FG);
    wlrail_paint(&sh, &R, &A.ran[1], rb_hover(true), now, BAR_BG, BAR_BG, lm_theme.accent, BAR_FG);
}

static void draw_bar(uint32_t *px)
{
    if (g_rail) {
        draw_rail(px);
        return;
    }
    fill_box(px, 0, 0, A.bw, BAR_H, BAR_BG);
    for (int i = 0; i < N_BTNS; i++) {
        int bx, by, bw, bh;
        char tmp[16];
        btn_rect(i, &bx, &by, &bw, &bh);
        if (bw == 0)
            continue;
        uint32_t base, hover;
        btn_colors(i, &base, &hover);
        drop_box(px, bx, by, bw, bh, lm_button_radius(bh / 2), (A.menu_open && i == BTN_ZOOM_ID) || A.hover == i ? hover : base);
        const char *t = btn_text(i, tmp, sizeof(tmp));
        int w = i == BTN_ZOOM_ID ? ZOOM_BTN_CHARS * LM_FONT_W : (int)strlen(t) * LM_FONT_W;
        int tx = bx + 8 + (i == BTN_ZOOM_ID ? w - (int)strlen(t) * LM_FONT_W : 0), ty = by + (bh - LM_FONT_H) / 2;
        put_text(px, tx, ty, t, lm_theme.on_colour);
        if (i == BTN_ZOOM_ID) {                                 /* the drop-down triangle */
            int ax = bx + bw - 8 - 7, ay = by + bh / 2 - 2;
            for (int r = 0; r < 4; r++)
                fill_box(px, ax + r, ay + r, 7 - 2 * r, 1, lm_theme.on_colour);
        }
    }
}

/* ---- the drop-down list of zoom levels ----------------------------------------------------------------------------- */

#define MENU_ROW_H (LM_FONT_H + 4)

static void menu_rect(int *x, int *y, int *w, int *h)
{
    int bx, by, bw, bh;
    if (g_rail) {                                               /* to the left of the size drop of the right rail */
        struct wlrail L, R;
        int ys[WLRAIL_MAX];
        term_rail(&L, &R);
        wlrail_layout(&R, ys);
        *w = ZOOM_BTN_CHARS * LM_FONT_W + 16 + ARROW_W;
        *x = A.bw - RAILR - 4 - *w;
        *h = N_ZOOM * MENU_ROW_H + 4;
        *y = LM_CLAMP(ys[1] >= 0 ? ys[1] : 0, 0, LM_MAX(A.bh - *h, 0));
        return;
    }
    btn_rect(BTN_ZOOM_ID, &bx, &by, &bw, &bh);
    *x = bx;
    *y = BAR_H;
    *w = bw;
    *h = N_ZOOM * MENU_ROW_H + 4;
}

static int menu_row_at(double px, double py)                    /* index into ZOOM_LEVELS or -1 */
{
    int x, y, w, h;
    menu_rect(&x, &y, &w, &h);
    if (px < x || px >= x + w || py < y + 2 || py >= y + h - 2)
        return -1;
    return (int)(py - y - 2) / MENU_ROW_H;
}

static void draw_menu(uint32_t *px)
{
    int x, y, w, h;
    menu_rect(&x, &y, &w, &h);
    round_box(px, x - 1, y - 1, w + 2, h + 2, 9, MENU_BORDER);
    round_box(px, x, y, w, h, 8, BAR_BG);
    for (int i = 0; i < N_ZOOM; i++) {
        int ry = y + 2 + i * MENU_ROW_H;
        bool cur = ZOOM_LEVELS[i] == A.zoom;
        if (i == A.menu_hover)
            round_box(px, x + 3, ry, w - 6, MENU_ROW_H, MENU_ROW_H / 2, BAR_BTN_ON);
        char t[16];
        snprintf(t, sizeof(t), "%d%%", ZOOM_LEVELS[i]);
        int tx = x + 8 + (ZOOM_BTN_CHARS - (int)strlen(t)) * LM_FONT_W;
        put_text(px, tx, ry + 2, t, cur ? MENU_CURRENT : BAR_FG);
        if (cur)
            fill_box(px, x + w - 12, ry + MENU_ROW_H / 2 - 2, 4, 4, MENU_CURRENT);       /* a dot marks the active size */
    }
}

/* ---- the tab bar and the status bar ------------------------------------------------------------------------------------- */

static char *selection_text(void);

static const char *u8_next(const char *s, uint32_t *cp)         /* one UTF-8 character: its code point, and the rest of the string */
{
    unsigned char b = (unsigned char)*s;
    int n = b < 0x80 ? 0 : (b & 0xe0) == 0xc0 ? 1 : (b & 0xf0) == 0xe0 ? 2 : (b & 0xf8) == 0xf0 ? 3 : 0;
    uint32_t c = n == 0 ? b : n == 1 ? (b & 0x1fu) : n == 2 ? (b & 0x0fu) : (b & 0x07u);
    s++;
    for (int i = 0; i < n && ((unsigned char)*s & 0xc0) == 0x80; i++, s++)
        c = (c << 6) | ((unsigned char)*s & 0x3f);
    *cp = c;
    return s;
}

static int u8_count(const char *s)
{
    int n = 0;
    uint32_t cp;
    while (*s) {
        s = u8_next(s, &cp);
        n++;
    }
    return n;
}

/* UTF-8 text in the 8 px cells; longer than `maxchars` ends in "..". Returns the number of characters drawn. */
static int put_text_u(uint32_t *px, int x, int y, const char *text, uint32_t color, int maxchars)
{
    int k = A.k, cw = LM_FONT_W * k, ch = LM_FONT_H * k;
    int len = u8_count(text), shown = len > maxchars ? LM_MAX(maxchars - 2, 0) : len, drawn = 0;
    for (const char *l = text; drawn < LM_MAX(maxchars, 0) && *l; drawn++, x += LM_FONT_W) {
        uint32_t cp;
        l = u8_next(l, &cp);
        if (drawn >= shown && len > maxchars)
            cp = '.';
        const uint8_t *g = lm_font_cell(cp, cw, ch);
        for (int r = 0; r < ch; r++)
            for (int c = 0; c < cw; c++) {
                unsigned a = g[r * cw + c];
                int xx = x * k + c, yy = y * k + r;
                if (a && xx >= 0 && xx < A.pw && yy >= 0 && yy < A.ph) {
                    uint32_t *p = &px[(size_t)yy * (size_t)A.pw + (size_t)xx];
                    *p = lm_font_mix(*p, color, a);
                }
            }
    }
    return drawn;
}

static const char *tab_label(const struct tab *tb, char *buf, size_t n)     /* what a tab says: the program's title, else the folder */
{
    if (tb->title_set || !tb->cwd[0]) {
        snprintf(buf, n, "%s", tb->title);
    } else {
        const char *home = getenv("HOME"), *b = strrchr(tb->cwd, '/');
        if (home && !strcmp(tb->cwd, home))
            snprintf(buf, n, "~");
        else
            snprintf(buf, n, "%s", b && b[1] ? b + 1 : tb->cwd);
    }
    return buf;
}

static void draw_one_tab(uint32_t *px, int i, int x, int y, int tw, bool lifted)
{
    const struct tab *tb = A.tabs[i];
    bool active = i == A.cur, hot = A.thover / 2 == i && A.thover >= 0;
    uint32_t bg = active ? BAR_BTN_ON : hot ? lm_theme.control_hot : lm_theme.control;
    round_box(px, x, y + 3, tw, WLTABS_H - 3, 8, lifted ? lm_theme.button_hover : bg);
    if (active)
        fill_box(px, x + 8, y + WLTABS_H - 3, tw - 16, 2, lm_theme.accent);
    bool close = tw >= 60 && (active || hot || lifted);
    int tx = x + 8;
    if (tb->activity && !active) {
        round_box(px, tx, y + WLTABS_H / 2 - 2, 5, 5, 3, lm_theme.accent);          /* it printed something while it was behind */
        tx += 9;
    }
    char lab[PATH_MAX];
    int room = (x + tw - (close ? WLTABS_CLOSE_W : 6) - tx) / LM_FONT_W;
    put_text_u(px, tx, y + 3 + (WLTABS_H - 3 - LM_FONT_H) / 2, tab_label(tb, lab, sizeof(lab)), active ? BAR_FG : lm_theme.bar_text, room);
    if (close) {
        bool on_x = A.thover == i * 2 + 1;
        put_text(px, x + tw - WLTABS_CLOSE_W + 3, y + 3 + (WLTABS_H - 3 - LM_FONT_H) / 2, "x", on_x ? lm_theme.error : lm_theme.muted);
    }
}

static void draw_tabbar(uint32_t *px)
{
    if (!tabbar_on())
        return;
    int x0 = tabbar_x(), y = BAR_H, w = tabbar_w(), n = A.ntabs, tw = wltabs_tw(w, n);
    fill_box(px, x0, y, w, WLTABS_H, BAR_BG);
    bool drag = A.tdrag.active && A.tdrag.moved;
    for (int i = 0; i < n; i++)
        if (!(drag && i == A.tdrag.idx))
            draw_one_tab(px, i, wltabs_x(x0, tw, i), y, tw, false);
        else if (A.tdrag.out)
            round_box(px, wltabs_x(x0, tw, i), y + 3, tw, WLTABS_H - 3, 8, lm_theme.control);   /* the empty slot */
    int plus = wltabs_plus_x(x0, tw, n);
    if (plus + WLTABS_PLUS_W <= x0 + w) {
        drop_box(px, plus + 2, y + 3, WLTABS_PLUS_W - 4, WLTABS_H - 6, lm_button_radius((WLTABS_H - 6) / 2), A.thover == WLTABS_PLUS ? lm_theme.btn_green_lit : lm_theme.btn_green);
        put_text(px, plus + (WLTABS_PLUS_W - LM_FONT_W) / 2, y + (WLTABS_H - LM_FONT_H) / 2, "+", lm_theme.on_colour);
    }
    if (drag && !A.tdrag.out) {                                 /* the tab in the hand follows the pointer along the bar */
        int x = LM_CLAMP((int)(A.tdrag.x - A.tdrag.grab), x0, LM_MAX(x0 + w - tw, x0));
        draw_one_tab(px, A.tdrag.idx, x, y, tw, true);
    } else if (drag) {                                          /* away from the bar: it will be a window */
        const char *hint = "Let go: open in a new window";
        int hw = (int)strlen(hint) * LM_FONT_W + 16, hx = LM_CLAMP((int)A.tdrag.x - hw / 2, 2, LM_MAX(A.bw - hw - 2, 2)), hy = LM_CLAMP((int)A.tdrag.y + 14, 2, LM_MAX(A.bh - 26, 2));
        round_box(px, hx - 1, hy - 1, hw + 2, 24, 12, MENU_BORDER);
        round_box(px, hx, hy, hw, 22, 11, lm_theme.accent);
        put_text(px, hx + 8, hy + 3, hint, lm_theme.ink);
    }
}

static void status_segment(uint32_t *px, int *x, int y, const char *text, uint32_t col, int limit, bool left)
{
    int n = u8_count(text), w = n * LM_FONT_W + 16;
    if (left) {
        if (*x + w > limit)
            return;
        put_text_u(px, *x + 8, y, text, col, n);
        *x += w;
        fill_box(px, *x - 1, y + 1, 1, LM_FONT_H - 2, lm_theme.divider);
    } else {
        if (*x - w < limit)
            return;
        *x -= w;
        put_text_u(px, *x + 8, y, text, col, n);
        fill_box(px, *x, y + 1, 1, LM_FONT_H - 2, lm_theme.divider);
    }
}

/* The bottom line: the grid's size, where the cursor is, the scrollback, a selection, the folder - and at the right the tab, the text size
 * and the program's pid. What does not fit is left out (from the end of each side). */
static void draw_status(uint32_t *px)
{
    if (!A.status)
        return;
    const struct term *t = &CUR->t;
    int h = stat_h(), y = A.bh - h, ty = y + (h - LM_FONT_H) / 2 + 1;
    fill_box(px, 0, y, A.bw, h, BAR_BG);
    fill_box(px, 0, y, A.bw, 1, lm_theme.divider);
    char b[PATH_MAX + 8];
    int rx = A.bw;
    if (A.ntabs > 1) {
        snprintf(b, sizeof(b), "Tab %d/%d", A.cur + 1, A.ntabs);
        status_segment(px, &rx, ty, b, lm_theme.muted, 0, false);
    }
    snprintf(b, sizeof(b), "Zoom %d%%", A.zoom);
    status_segment(px, &rx, ty, b, lm_theme.muted, 0, false);
    if (CUR->pid > 0) {
        snprintf(b, sizeof(b), "PID %d", (int)CUR->pid);
        status_segment(px, &rx, ty, b, lm_theme.muted, 0, false);
    }
    int x = 0;
    snprintf(b, sizeof(b), "%d cols x %d rows", t->cols, t->rows);
    status_segment(px, &x, ty, b, BAR_FG, rx, true);
    snprintf(b, sizeof(b), "Cursor %d,%d", t->cy + 1, t->cx + 1);
    status_segment(px, &x, ty, b, lm_theme.bar_text, rx, true);
    if (t->alt_active)
        snprintf(b, sizeof(b), "Full screen program");
    else if (t->view_off > 0)
        snprintf(b, sizeof(b), "Scrolled back %d of %d", t->view_off, t->sb_count);
    else
        snprintf(b, sizeof(b), "Scrollback %d", t->sb_count);
    status_segment(px, &x, ty, b, t->view_off > 0 ? lm_theme.warn : lm_theme.bar_text, rx, true);
    if (t->sel_active) {
        char *sel = selection_text();
        if (sel) {
            snprintf(b, sizeof(b), "%d characters selected", u8_count(sel));
            status_segment(px, &x, ty, b, lm_theme.accent, rx, true);
            free(sel);
        }
    }
    if (CUR->cwd[0] && x + 3 * LM_FONT_W + 16 <= rx) {
        const char *home = getenv("HOME");
        size_t hl = home ? strlen(home) : 0;
        if (hl > 1 && !strncmp(CUR->cwd, home, hl) && (CUR->cwd[hl] == '/' || !CUR->cwd[hl]))
            snprintf(b, sizeof(b), "~%s", CUR->cwd + hl);
        else
            snprintf(b, sizeof(b), "%s", CUR->cwd);
        int room = (rx - x - 16) / LM_FONT_W;
        put_text_u(px, x + 8, ty, b, lm_theme.text_dim, room);
    }
}

static void build_zoom_tables(void)
{
    lm_zoom_axis(A.xt, LM_MIN(cell_w(), (int)(sizeof(A.xt) / sizeof(A.xt[0]))), LM_FONT_W);
    lm_zoom_axis(A.yt, LM_MIN(cell_h(), (int)(sizeof(A.yt) / sizeof(A.yt[0]))), LM_FONT_H);
}

/* ---- scroll bar -----------------------------------------------------------------------------------------------------------
 * A strip at the right edge, below the button bar. The thumb stands for the visible rows among scrollback + screen; its top is
 * the oldest line shown (sb_count - view_off). Nothing to scroll back to (or the alternate screen): only the empty track. */

static bool sb_active(void) { return CUR->t.sb_count > 0 && !CUR->t.alt_active; }

static void sb_thumb(int *ty, int *th)
{
    const struct term *t = &CUR->t;
    int track = LM_MAX(grid_bottom() - top_h(), 1), total = LM_MAX(t->sb_count + t->rows, 1);
    *th = LM_CLAMP(track * t->rows / total, LM_MIN(28, track), track);
    *ty = top_h() + (track - *th) * (t->sb_count - t->view_off) / LM_MAX(t->sb_count, 1);
}

static void draw_scrollbar(uint32_t *px)
{
    int w = sb_w(), x0 = A.bw - RAILR - w;
    if (x0 < 0 || grid_bottom() <= top_h())
        return;
    fill_box(px, x0, top_h(), w, grid_bottom() - top_h(), lm_theme.bar);
    if (!sb_active())
        return;
    int ty, th;
    sb_thumb(&ty, &th);
    bool hot = A.sb_drag || (A.px >= x0 && A.py >= ty && A.py < ty + th);
    uint32_t col = hot ? lm_theme.button_hover : lm_theme.control;
    fill_box(px, x0 + 2, ty + 1, w - 4, th - 2, col);
}

static void sb_drag_to(double py)
{
    struct term *t = &CUR->t;
    int ty, th;
    sb_thumb(&ty, &th);
    int room = LM_MAX(grid_bottom() - top_h() - th, 1);
    int pos = LM_CLAMP((int)py - A.sb_grab - top_h(), 0, room);
    t->view_off = LM_CLAMP(t->sb_count - pos * t->sb_count / room, 0, t->sb_count);
    A.dirty = true;
}

static void render(void)
{
    struct term *t = &CUR->t;
    int need_w = t->cols * CELL_W + 2 * PAD + sb_w() + RAILX + RAILR, need_h = t->rows * CELL_H + 2 * PAD + top_h() + stat_h();
    bool rescaled = A.k != want_k();
    if (!A.pixels || A.bw != need_w || A.bh != need_h || rescaled) {
        /* (re)create pool and buffers */
        if (A.pixels) {
            for (int i = 0; i < 2; i++)
                if (A.buffers[i]) {
                    WLC_SEND0(A.c, A.buffers[i], LM_WL_BUFFER_REQ_DESTROY);
                    wlc_forget(A.c, A.buffers[i]);
                    A.buffers[i] = 0;
                    A.buf_busy[i] = false;
                }
            WLC_SEND0(A.c, A.pool, LM_WL_SHM_POOL_REQ_DESTROY);
            wlc_forget(A.c, A.pool);
            munmap(A.pixels, A.map_size);
        }
        A.bw = need_w;
        A.bh = need_h;
        A.k = want_k();
        A.pw = need_w * A.k;
        A.ph = need_h * A.k;
        WLC_SEND(A.c, A.surface, LM_WL_SURFACE_REQ_SET_BUFFER_SCALE, { .i = A.k });
        size_t one = (size_t)A.pw * (size_t)A.ph * 4;
        A.map_size = one * 2;
        int fd = memfd_create("mellagui-term", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, (off_t)A.map_size) < 0) {
            perror("memfd");
            A.running = false;
            return;
        }
        A.pixels = mmap(NULL, A.map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        A.pool = wlc_alloc(A.c, &lm_wl_shm_pool_interface, NULL, NULL);
        WLC_SEND(A.c, A.shm, LM_WL_SHM_REQ_CREATE_POOL, { .n = A.pool }, { .h = fd }, { .i = (int32_t)A.map_size });
        close(fd);
    }
    int idx = -1;
    for (int i = 0; i < 2; i++)
        if (!A.buf_busy[i]) { idx = i; break; }
    if (idx < 0)
        return;     /* both buffers still held by the compositor: retry on release */

    size_t one = (size_t)A.pw * (size_t)A.ph;
    uint32_t *px = A.pixels + one * (size_t)idx;
    for (size_t i = 0; i < one; i++)
        px[i] = DEFAULT_BG;

    for (int row = 0; row < t->rows; row++) {
        for (int col = 0; col < t->cols; col++) {
            const struct cell *c = cell_at_view(t, row, col);
            struct cell blank = { ' ', DEFAULT_FG, DEFAULT_BG, 0 };
            if (!c)
                c = &blank;
            int span = 1;
            uint32_t cp = c->cp;
            if (c->attr & ATTR_WIDE) {                          /* (half a wide character, cut by a resize, shows as nothing) */
                const struct cell *r = col + 1 < t->cols ? cell_at_view(t, row, col + 1) : NULL;
                if (r && (r->attr & ATTR_CONT))
                    span = 2;
                else
                    cp = ' ';
            }
            uint32_t fg = c->fg, bg = c->bg;
            if (c->attr & ATTR_BOLD)
                fg = brighten(fg);
            if (c->attr & ATTR_DIM)
                fg = dim(fg);
            bool rev = (c->attr & ATTR_REVERSE) != 0;
            bool cursor = t->view_off == 0 && t->cursor_visible && row == t->cy && t->cx >= col && t->cx < col + span;
            if (selected(t, row, col) || (span == 2 && selected(t, row, col + 1)))
                rev = !rev;
            if (cursor && A.focused)
                rev = !rev;
            if (rev) {
                uint32_t tmp = fg;
                fg = bg;
                bg = tmp;
            }
            int k = A.k, cw = CELL_W * k, ch = CELL_H * k;           /* the grid is exact in the buffer's own pixels */
            int x0 = (RAILX + PAD + col * CELL_W) * k, y0 = (top_h() + PAD + row * CELL_H) * k;
            cw *= span;
            bool bold = (c->attr & ATTR_BOLD) != 0;
            const uint8_t *g = span == 2 ? lm_font_cell_wide(cp, cw, ch, bold)
                                         : lm_font_cell_styled(cp, cw, ch, bold);       /* drawn at the cell's own size, whatever the zoom */
            for (int r = 0; r < ch; r++) {
                uint32_t *dst = px + (size_t)(y0 + r) * (size_t)A.pw + (size_t)x0;
                const uint8_t *cov = g + (size_t)r * (size_t)cw;
                for (int x = 0; x < cw; x++)
                    dst[x] = cov[x] == 0 ? bg : cov[x] == 255 ? fg : lm_font_mix(bg, fg, cov[x]);
            }
            if (c->attr & ATTR_UNDERLINE) {
                int thick = LM_MAX(ch / LM_FONT_H, 1), uy = y0 + ch - 2 * thick;
                for (int r = 0; r < thick; r++)
                    for (int x = 0; x < cw; x++)
                        px[(size_t)(uy + r) * (size_t)A.pw + (size_t)(x0 + x)] = fg;
            }
            if (cursor && !A.focused) {
                /* hollow cursor when unfocused */
                for (int x = 0; x < cw; x++) {
                    px[(size_t)y0 * (size_t)A.pw + (size_t)(x0 + x)] = DEFAULT_FG;
                    px[(size_t)(y0 + ch - 1) * (size_t)A.pw + (size_t)(x0 + x)] = DEFAULT_FG;
                }
                for (int r = 0; r < ch; r++) {
                    px[(size_t)(y0 + r) * (size_t)A.pw + (size_t)x0] = DEFAULT_FG;
                    px[(size_t)(y0 + r) * (size_t)A.pw + (size_t)(x0 + cw - 1)] = DEFAULT_FG;
                }
            }
            col += span - 1;
        }
    }

    draw_scrollbar(px);
    draw_bar(px);
    draw_tabbar(px);
    draw_status(px);
    if (A.menu_open)
        draw_menu(px);

    if (!A.buffers[idx]) {
        A.buffers[idx] = wlc_alloc(A.c, &lm_wl_buffer_interface, NULL, NULL);
        WLC_SEND(A.c, A.pool, LM_WL_SHM_POOL_REQ_CREATE_BUFFER, { .n = A.buffers[idx] },
                 { .i = (int32_t)(one * 4 * (size_t)idx) }, { .i = A.pw }, { .i = A.ph },
                 { .i = A.pw * 4 }, { .u = LM_WL_SHM_FORMAT_XRGB8888 });
        wlc_set_handler(A.c, A.buffers[idx], NULL, NULL);
    }
    A.buf_busy[idx] = true;
    WLC_SEND(A.c, A.surface, LM_WL_SURFACE_REQ_ATTACH, { .o = A.buffers[idx] }, { .i = 0 }, { .i = 0 });
    WLC_SEND(A.c, A.surface, LM_WL_SURFACE_REQ_DAMAGE, { .i = 0 }, { .i = 0 }, { .i = A.bw }, { .i = A.bh });
    WLC_SEND(A.c, A.xsurface, LM_XDG_SURFACE_REQ_SET_WINDOW_GEOMETRY, { .i = 0 }, { .i = 0 }, { .i = A.bw }, { .i = A.bh });
    WLC_SEND0(A.c, A.surface, LM_WL_SURFACE_REQ_COMMIT);
    A.dirty = false;
    if (A.grown) {                                              /* the window now has the size of its grid at this zoom */
        A.win_w = A.bw;
        A.win_h = A.bh;
        A.grown = false;
    }
}

static void frame_done(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)op; (void)args; (void)data;
    wlc_forget(c, id);
    A.frame_cb = 0;
}

static void buffer_released(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)args; (void)data;
    if (op != LM_WL_BUFFER_EVT_RELEASE)
        return;
    for (int i = 0; i < 2; i++)
        if (A.buffers[i] == id)
            A.buf_busy[i] = false;
}

static void maybe_render(void)
{
    if (!A.configured || !A.dirty || A.frame_cb)
        return;
    render();
    if (!A.dirty) {
        for (int i = 0; i < 2; i++)
            if (A.buffers[i])
                wlc_set_handler(A.c, A.buffers[i], buffer_released, NULL);
        A.frame_cb = wlc_alloc(A.c, &lm_wl_callback_interface, frame_done, NULL);
        WLC_SEND(A.c, A.surface, LM_WL_SURFACE_REQ_FRAME, { .n = A.frame_cb });
        WLC_SEND0(A.c, A.surface, LM_WL_SURFACE_REQ_COMMIT);
    }
}

/* ======================================================================== *
 *  Selection / clipboard
 * ======================================================================== */

static char *selection_text(void)
{
    struct term *t = &CUR->t;
    if (!t->sel_active)
        return NULL;
    int y0 = t->sel_y0, x0 = t->sel_x0, y1 = t->sel_y1, x1 = t->sel_x1;
    if (y0 > y1 || (y0 == y1 && x0 > x1)) {
        int ty = y0, tx = x0;
        y0 = y1; x0 = x1; y1 = ty; x1 = tx;
    }
    size_t cap = (size_t)(y1 - y0 + 1) * (size_t)(t->cols * 4 + 2) + 1;
    char *out = malloc(cap), *p = out;
    for (int y = y0; y <= y1; y++) {
        int xa = y == y0 ? x0 : 0, xb = y == y1 ? x1 : t->cols - 1;
        char *line_start = p;
        const struct cell *first = xa > 0 ? cell_at_view(t, y, xa) : NULL;
        if (first && (first->attr & ATTR_CONT))                 /* it starts on the right half: the whole character */
            xa--;
        for (int x = xa; x <= xb; x++) {
            const struct cell *c = cell_at_view(t, y, x);
            if (c && (c->attr & ATTR_CONT))
                continue;
            uint32_t cp = c ? c->cp : ' ';
            if (cp < 0x80) *p++ = (char)cp;
            else if (cp < 0x800) { *p++ = (char)(0xc0 | cp >> 6); *p++ = (char)(0x80 | (cp & 0x3f)); }
            else if (cp < 0x10000) { *p++ = (char)(0xe0 | cp >> 12); *p++ = (char)(0x80 | ((cp >> 6) & 0x3f)); *p++ = (char)(0x80 | (cp & 0x3f)); }
            else { *p++ = (char)(0xf0 | cp >> 18); *p++ = (char)(0x80 | ((cp >> 12) & 0x3f)); *p++ = (char)(0x80 | ((cp >> 6) & 0x3f)); *p++ = (char)(0x80 | (cp & 0x3f)); }
        }
        while (p > line_start && p[-1] == ' ')
            p--;
        if (y != y1)
            *p++ = '\n';
    }
    *p = 0;
    return out;
}

static void dsrc_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)data;
    if (op == LM_WL_DATA_SOURCE_EVT_SEND) {
        int fd = args[1].h;
        if (A.clip_out) {
            const char *p = A.clip_out;
            size_t n = strlen(p);
            signal(SIGPIPE, SIG_IGN);
            while (n > 0) {
                ssize_t k = write(fd, p, n);
                if (k <= 0)
                    break;
                p += k;
                n -= (size_t)k;
            }
        }
        close(fd);
    } else if (op == LM_WL_DATA_SOURCE_EVT_CANCELLED) {
        WLC_SEND0(c, id, LM_WL_DATA_SOURCE_REQ_DESTROY);
        wlc_forget(c, id);
        if (A.dsrc == id)
            A.dsrc = 0;
    }
}

static void copy_selection(void)
{
    if (!A.ddm || !A.ddev)
        return;
    char *text = selection_text();
    if (!text || !*text) {
        free(text);
        return;
    }
    free(A.clip_out);
    A.clip_out = text;
    if (A.dsrc) {
        WLC_SEND0(A.c, A.dsrc, LM_WL_DATA_SOURCE_REQ_DESTROY);
        wlc_forget(A.c, A.dsrc);
    }
    A.dsrc = wlc_alloc(A.c, &lm_wl_data_source_interface, dsrc_event, NULL);
    WLC_SEND(A.c, A.ddm, LM_WL_DATA_DEVICE_MANAGER_REQ_CREATE_DATA_SOURCE, { .n = A.dsrc });
    WLC_SEND(A.c, A.dsrc, LM_WL_DATA_SOURCE_REQ_OFFER, { .s = "text/plain;charset=utf-8" });
    WLC_SEND(A.c, A.dsrc, LM_WL_DATA_SOURCE_REQ_OFFER, { .s = "text/plain" });
    WLC_SEND(A.c, A.ddev, LM_WL_DATA_DEVICE_REQ_SET_SELECTION, { .o = A.dsrc }, { .u = A.last_serial });
}

/* Send pasted text to the shell: bracketed when the application asked for it, else with \n as \r like xterm. */
static void paste_text(const char *buf, size_t len)
{
    if (!len)
        return;
    if (CUR->t.bracketed) {
        pty_write("\033[200~", 6);
        pty_write(buf, len);
        pty_write("\033[201~", 6);
        return;
    }
    char *tmp = malloc(len);
    if (!tmp)
        return;
    for (size_t i = 0; i < len; i++)
        tmp[i] = buf[i] == '\n' ? '\r' : buf[i];
    pty_write(tmp, len);
    free(tmp);
}

static void paste(void);

/* The answer of the right-click menu: 0 Copy (what is selected), 1 Paste. */
static void menu_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id; (void)data;
    if (op != LM_MELLAGUI_MENU_V1_EVT_CHOSEN)
        return;
    if (args[0].i == 0)
        copy_selection();
    else if (args[0].i == 1)
        paste();
}

static const char *const paste_mimes[] = { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT" };

static void paste(void)
{
    /* We own the selection ourselves: the compositor's offer would need us to write into the pipe while we block
     * reading it, so use our own copy. */
    if (A.dsrc && A.clip_out) {
        paste_text(A.clip_out, strlen(A.clip_out));
        return;
    }
    if (!A.have_offer || !A.offer_id)
        return;
    const char *mime = NULL;
    for (size_t m = 0; m < sizeof(paste_mimes) / sizeof(paste_mimes[0]) && !mime; m++)
        for (int i = 0; i < A.n_offer_mimes; i++)
            if (!strcmp(A.offer_mimes[i], paste_mimes[m])) {
                mime = A.offer_mimes[i];
                break;
            }
    if (!mime)
        return;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0)
        return;
    WLC_SEND(A.c, A.offer_id, LM_WL_DATA_OFFER_REQ_RECEIVE, { .s = mime }, { .h = fds[1] });
    close(fds[1]);
    wlc_flush(A.c);
    /* The source writes into the pipe; read until EOF (bounded wait). */
    char *buf = malloc(1 << 20);
    if (!buf) {
        close(fds[0]);
        return;
    }
    size_t len = 0;
    struct pollfd pf = { .fd = fds[0], .events = POLLIN };
    while (len < (1u << 20)) {
        if (poll(&pf, 1, 1000) <= 0)
            break;
        ssize_t k = read(fds[0], buf + len, (1u << 20) - len);
        if (k <= 0)
            break;
        len += (size_t)k;
    }
    close(fds[0]);
    paste_text(buf, len);
    free(buf);
}

/* ---- the terminal's own settings file -------------------------------------------------------------------------------------
 * $XDG_CONFIG_HOME/mellagui/terminal.conf (else ~/.config/...): `key=value` lines: `zoom=<percent>`, `statusbar=0|1`. Read at
 * start, written ~0.4 s after the zoom last changed (and on exit), so the next terminal starts at the same size. */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool conf_path(char *out, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    int w;
    if (xdg && *xdg)
        w = snprintf(out, n, "%s/mellagui/terminal.conf", xdg);
    else if (home && *home)
        w = snprintf(out, n, "%s/.config/mellagui/terminal.conf", home);
    else
        return false;
    return w > 0 && (size_t)w < n;
}

static void conf_load(void)
{
    char path[1024], line[128];
    if (!conf_path(path, sizeof(path)))
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "zoom=%d", &v) == 1 && v >= ZOOM_LEVELS[0] && v <= ZOOM_LEVELS[N_ZOOM - 1])
            A.zoom = v;                                         /* anything else in the file is ignored */
        else if (sscanf(line, "statusbar=%d", &v) == 1) {
            A.status_conf = v != 0 ? 1 : 0;
            A.status = A.status_conf != 0;
        }
    }
    fclose(f);
    build_zoom_tables();
}

static void conf_save(void)
{
    char path[1024], tmp[1100];
    if (!conf_path(path, sizeof(path)))
        return;
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    for (char *p = path + 1; *p; p++) {                         /* parent directories */
        if (*p != '/')
            continue;
        *p = 0;
        mkdir(path, 0755);
        *p = '/';
    }
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "# mellagui-term: written by the terminal itself\nzoom=%d\n", A.zoom);
    if (A.status_conf >= 0)
        fprintf(f, "statusbar=%d\n", A.status_conf);
    bool ok = fclose(f) == 0;
    if (!ok || rename(tmp, path) < 0)
        unlink(tmp);
}

/* Font zoom. A floating window keeps its grid (columns x rows) and grows or shrinks with the font; a maximized or
 * fullscreen one keeps its size and gets a new grid. (A window that would not fit the screen is asked to shrink by the
 * compositor: that arrives as a normal configure.) */
static void apply_size(int w, int h);

static void set_zoom_pct(int pct)
{
    pct = LM_CLAMP(pct, ZOOM_LEVELS[0], ZOOM_LEVELS[N_ZOOM - 1]);
    if (pct == A.zoom)
        return;
    A.zoom = pct;
    build_zoom_tables();
    A.conf_dirty = true;
    A.conf_time = now_ms();
    if (!A.pixels && A.win_w <= 0)                              /* no window yet (startup): the first render sizes it */
        return;
    if (A.fixed_size) {
        apply_size(A.win_w > 0 ? A.win_w : A.bw, A.win_h > 0 ? A.win_h : A.bh);
    } else {
        A.grown = true;                                         /* render() makes the buffer for the same grid at the new size */
        A.dirty = true;
    }
}

/* delta +1 / -1: the next level up / down the list; 0: back to 100 %. */
static void set_zoom(int delta)
{
    set_zoom_pct(lm_zoom_step(A.zoom, delta));
}

static void menu_set(bool open)
{
    if (open) {
        A.menu_hover = 0;
        for (int i = 0; i < N_ZOOM; i++)
            if (ZOOM_LEVELS[i] == A.zoom)
                A.menu_hover = i;
    }
    A.menu_open = open;
    A.dirty = true;
}

static void doffer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)data;
    if (op == LM_WL_DATA_OFFER_EVT_OFFER && id == A.offer_id && A.n_offer_mimes < MAX_MIMES)
        snprintf(A.offer_mimes[A.n_offer_mimes++], sizeof(A.offer_mimes[0]), "%s", args[0].s);
}

static void ddev_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)id; (void)data;
    if (op == LM_WL_DATA_DEVICE_EVT_DATA_OFFER) {
        if (A.offer_id && !A.have_offer) {
            WLC_SEND0(c, A.offer_id, LM_WL_DATA_OFFER_REQ_DESTROY);
            wlc_forget(c, A.offer_id);
        }
        A.offer_id = args[0].n;
        A.n_offer_mimes = 0;
        A.have_offer = false;
        wlc_set_handler(c, A.offer_id, doffer_event, NULL);
    } else if (op == LM_WL_DATA_DEVICE_EVT_SELECTION) {
        uint32_t sel = args[0].o;
        if (sel && sel == A.offer_id) {
            A.have_offer = true;
        } else {
            A.have_offer = false;
            if (A.offer_id) {
                WLC_SEND0(c, A.offer_id, LM_WL_DATA_OFFER_REQ_DESTROY);
                wlc_forget(c, A.offer_id);
                A.offer_id = 0;
            }
        }
    }
}

/* ======================================================================== *
 *  Input
 * ======================================================================== */

static void send_str(const char *s)
{
    pty_write(s, strlen(s));
}

static void tab_new(void);
static void tab_remove(int i, bool end_program);
static void tab_switch(int i);
static void tab_cycle(int d);
static void tab_move(int from, int to);
static void new_terminal(void);
static void layout_changed(void);

static void key_to_pty(uint32_t code, bool repeat)
{
    struct term *t = &CUR->t;
    bool shift = A.mods & LM_MOD_SHIFT, ctrl = A.mods & LM_MOD_CTRL, alt = A.mods & LM_MOD_ALT;
    (void)repeat;

    if (A.menu_open) {                                  /* the zoom list: arrows, Enter, Esc */
        if (code == KEY_ESC) {
            menu_set(false);
        } else if (code == KEY_UP || code == KEY_DOWN) {
            A.menu_hover = LM_CLAMP(A.menu_hover + (code == KEY_UP ? -1 : 1), 0, N_ZOOM - 1);
            A.dirty = true;
        } else if (code == KEY_ENTER || code == KEY_KPENTER) {
            set_zoom_pct(ZOOM_LEVELS[A.menu_hover]);
            menu_set(false);
        }
        return;
    }
    /* tabs (the keys that open or close things do not repeat) */
    if (ctrl && !alt && (code == KEY_T || code == KEY_W || code == KEY_N || code == KEY_B) && shift) {
        if (repeat)
            return;
        if (code == KEY_T) tab_new();
        else if (code == KEY_W) tab_remove(A.cur, true);
        else if (code == KEY_N) new_terminal();
        else {
            A.status = !A.status;
            A.status_conf = A.status ? 1 : 0;
            A.conf_dirty = true;
            A.conf_time = now_ms();
            layout_changed();
        }
        return;
    }
    if (ctrl && !alt && (code == KEY_PAGEUP || code == KEY_PAGEDOWN || code == KEY_TAB) && A.ntabs > 1) {
        int d = code == KEY_PAGEUP || (code == KEY_TAB && shift) ? -1 : 1;
        if (shift && code != KEY_TAB)
            tab_move(A.cur, LM_CLAMP(A.cur + d, 0, A.ntabs - 1));
        else
            tab_cycle(d);
        return;
    }
    if (alt && !ctrl && !shift && code >= KEY_1 && code <= KEY_9 && (int)(code - KEY_1) < A.ntabs) {
        tab_switch(code - KEY_1);
        return;
    }
    if (shift && ctrl && code == KEY_C) { copy_selection(); return; }
    if (ctrl && !alt) {
        if (code == KEY_EQUAL || code == KEY_KPPLUS) { set_zoom(1); return; }
        if (code == KEY_MINUS || code == KEY_KPMINUS) { set_zoom(-1); return; }
        if (code == KEY_0 || code == KEY_KP0) { set_zoom(0); return; }
    }
    if ((shift && ctrl && code == KEY_V) || (shift && code == KEY_INSERT)) { paste(); return; }
    if (shift && (code == KEY_PAGEUP || code == KEY_PAGEDOWN)) {
        int step = t->rows / 2;
        if (code == KEY_PAGEUP)
            t->view_off = LM_MIN(t->view_off + step, t->sb_count);
        else
            t->view_off = LM_MAX(t->view_off - step, 0);
        A.dirty = true;
        return;
    }
    if (shift && (code == KEY_HOME || code == KEY_END) && !t->alt_active) {
        t->view_off = code == KEY_HOME ? t->sb_count : 0;
        A.dirty = true;
        return;
    }

    /* Typing returns the view to the live screen. */
    if (t->view_off) {
        t->view_off = 0;
        A.dirty = true;
    }
    if (t->sel_active) {
        t->sel_active = false;
        A.dirty = true;
    }

    const char *seq = NULL;
    char buf[16];
    int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    modes_check(CUR);
    int screen = t->alt_active ? 1 : 0, depth = CUR->m.kbd_n[screen];
    if (depth && (CUR->m.kbd[screen][depth - 1] & 1)) {         /* the keyboard protocol: see kbd_command() */
        uint32_t k = code == KEY_ESC ? 27 : 0;
        if (mod > 1 && (code == KEY_ENTER || code == KEY_KPENTER)) k = 13;
        if (mod > 1 && code == KEY_TAB) k = 9;
        if (mod > 1 && code == KEY_BACKSPACE) k = 127;
        if (!k && (ctrl || alt)) {
            uint32_t base = lm_keymap_keysym(code, 0);
            if (base >= 32 && base < 127)
                k = base;
        }
        if (k) {
            if (mod > 1)
                snprintf(buf, sizeof(buf), "\033[%u;%du", k, mod);
            else
                snprintf(buf, sizeof(buf), "\033[%uu", k);
            send_str(buf);
            return;
        }
    }
    char c1 = t->app_cursor ? 'O' : '[';
    switch (code) {
    case KEY_UP: case KEY_DOWN: case KEY_RIGHT: case KEY_LEFT: case KEY_HOME: case KEY_END: {
        char f = code == KEY_UP ? 'A' : code == KEY_DOWN ? 'B' : code == KEY_RIGHT ? 'C' :
                 code == KEY_LEFT ? 'D' : code == KEY_HOME ? 'H' : 'F';
        if (mod > 1)
            snprintf(buf, sizeof(buf), "\033[1;%d%c", mod, f);
        else
            snprintf(buf, sizeof(buf), "\033%c%c", c1, f);
        seq = buf;
        break;
    }
    case KEY_INSERT: seq = "\033[2~"; break;
    case KEY_DELETE: seq = "\033[3~"; break;
    case KEY_PAGEUP: seq = "\033[5~"; break;
    case KEY_PAGEDOWN: seq = "\033[6~"; break;
    case KEY_BACKSPACE: seq = ctrl ? "\x08" : "\x7f"; break;
    case KEY_TAB: seq = shift ? "\033[Z" : "\t"; break;
    case KEY_ENTER: case KEY_KPENTER: seq = "\r"; break;
    case KEY_ESC: seq = "\033"; break;
    default:
        if (code >= KEY_F1 && code <= KEY_F10) {
            static const char *fk[] = { "\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~", "\033[17~",
                                        "\033[18~", "\033[19~", "\033[20~", "\033[21~" };
            seq = fk[code - KEY_F1];
        } else if (code == KEY_F11) seq = "\033[23~";
        else if (code == KEY_F12) seq = "\033[24~";
    }
    if (seq) {
        if (alt && seq[0] != '\033')
            pty_write("\033", 1);
        send_str(seq);
        return;
    }

    uint32_t ks = lm_keymap_keysym(code, A.mods);
    if (!ks)
        return;
    if (ctrl) {
        uint32_t base = lm_keymap_keysym(code, 0);
        char ch = 0;
        if (base >= 'a' && base <= 'z') ch = (char)(base - 'a' + 1);
        else if (base == '[' ) ch = 0x1b;
        else if (base == '\\') ch = 0x1c;
        else if (base == ']') ch = 0x1d;
        else if (base == '6' || base == '^') ch = 0x1e;
        else if (base == '-' || base == '/') ch = 0x1f;
        else if (base == '2' || base == ' ') ch = 0;
        else if (base == '8') ch = 0x7f;
        if (ch != 0 || base == ' ' || base == '2') {
            if (alt)
                pty_write("\033", 1);
            pty_write(&ch, 1);
            return;
        }
    }
    char txt[8];
    int n = lm_keysym_to_utf8(ks, txt);
    if (n > 0) {
        if (alt)
            pty_write("\033", 1);
        pty_write(txt, (size_t)n);
    }
}

static void repeat_arm(uint32_t code)
{
    if (A.repeat_rate <= 0 || A.repeat_fd < 0)
        return;
    A.repeat_code = code;
    long interval_ns = 1000000000L / A.repeat_rate;
    struct itimerspec it = {
        .it_value = { .tv_sec = A.repeat_delay / 1000, .tv_nsec = (A.repeat_delay % 1000) * 1000000L },
        .it_interval = { .tv_sec = interval_ns / 1000000000L, .tv_nsec = interval_ns % 1000000000L },
    };
    timerfd_settime(A.repeat_fd, 0, &it, NULL);
}

static void repeat_stop(void)
{
    A.repeat_code = 0;
    struct itimerspec it = { { 0, 0 }, { 0, 0 } };
    if (A.repeat_fd >= 0)
        timerfd_settime(A.repeat_fd, 0, &it, NULL);
}

static void keyboard_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id; (void)data;
    switch (op) {
    case LM_WL_KEYBOARD_EVT_KEYMAP:
        lm_keymap_client_load(args[1].h, args[2].u);
        break;
    case LM_WL_KEYBOARD_EVT_ENTER:
        A.focused = true;
        A.last_serial = args[0].u;
        A.dirty = true;
        break;
    case LM_WL_KEYBOARD_EVT_LEAVE:
        A.focused = false;
        repeat_stop();
        A.dirty = true;
        break;
    case LM_WL_KEYBOARD_EVT_KEY:
        A.last_serial = args[0].u;
        if (args[3].u == 1) {
            key_to_pty(args[2].u, false);
            uint32_t c0 = args[2].u;
            if (c0 != KEY_LEFTSHIFT && c0 != KEY_RIGHTSHIFT && c0 != KEY_LEFTCTRL && c0 != KEY_RIGHTCTRL &&
                c0 != KEY_LEFTALT && c0 != KEY_RIGHTALT && c0 != KEY_LEFTMETA && c0 != KEY_RIGHTMETA &&
                c0 != KEY_CAPSLOCK && c0 != KEY_NUMLOCK)
                repeat_arm(c0);
        } else if (A.repeat_code == args[2].u) {
            repeat_stop();
        }
        break;
    case LM_WL_KEYBOARD_EVT_MODIFIERS:
        A.mods = args[1].u | args[2].u | args[3].u;
        lm_keymap_client_group(args[4].u);
        break;
    case LM_WL_KEYBOARD_EVT_REPEAT_INFO:
        A.repeat_rate = args[0].i;
        A.repeat_delay = args[1].i;
        break;
    }
}

/* ---- tabs ------------------------------------------------------------------------------------------------------------------ */

static void apply_size(int w, int h);
static void new_terminal(void);
static void spawn_handover(char *const argv[], int pty, int state);

static struct tab *tab_alloc(int cols, int rows)
{
    struct tab *tb = calloc(1, sizeof(*tb));
    term_init(&tb->t, cols, rows);
    tb->pty = -1;
    snprintf(tb->title, sizeof(tb->title), "mellagui-term");
    return tb;
}

static void term_free(struct term *t)
{
    screen_free(t->main_scr, t->rows);
    screen_free(t->alt_scr, t->rows);
    if (t->sb)
        for (int i = 0; i < SCROLLBACK; i++)
            free(t->sb[i].cells);
    free(t->sb);
}

static bool tab_read_cwd(struct tab *tb)                         /* true when the folder changed */
{
    char link[64], dir[PATH_MAX];
    if (tb->pid <= 0)
        return false;
    snprintf(link, sizeof(link), "/proc/%d/cwd", (int)tb->pid);
    ssize_t k = readlink(link, dir, sizeof(dir) - 1);
    if (k <= 0)
        return false;
    dir[k] = 0;
    if (!strcmp(dir, tb->cwd))
        return false;
    snprintf(tb->cwd, sizeof(tb->cwd), "%s", dir);
    return true;
}

static void cwd_refresh(void)
{
    for (int i = 0; i < A.ntabs; i++)
        if (tab_read_cwd(A.tabs[i]) && (i == A.cur || tabbar_on()))
            A.dirty = true;
    A.cwd_ms = now_ms();
}

/* A tab came or went so that the bar shows or hides: the grid keeps its size (a floating window grows / shrinks by the bar), a
 * maximized window keeps its size and gets another grid. */
static void layout_changed(void)
{
    if (!A.pixels && A.win_w <= 0)
        return;
    if (A.fixed_size) {
        apply_size(A.win_w > 0 ? A.win_w : A.bw, A.win_h > 0 ? A.win_h : A.bh);
    } else {
        A.grown = true;
        A.dirty = true;
    }
}

static void tab_switch(int i)
{
    if (i < 0 || i >= A.ntabs || i == A.cur)
        return;
    CUR->t.sel_dragging = false;
    A.sb_drag = false;
    if (A.menu_open)
        menu_set(false);
    A.cur = i;
    CUR->activity = false;
    send_window_title();
    A.dirty = true;
}

static void tab_cycle(int d)
{
    if (A.ntabs > 1)
        tab_switch((A.cur + d + A.ntabs) % A.ntabs);
}

/* A new tab next to the one in front, with a shell in the same folder. */
static void tab_new(void)
{
    if (A.ntabs >= MAX_TABS || A.ntabs < 1)
        return;
    struct tab *tb = tab_alloc(CUR->t.cols, CUR->t.rows);
    tab_read_cwd(CUR);
    tb->pty = spawn_shell(tb, NULL, CUR->cwd);
    if (tb->pty < 0) {
        term_free(&tb->t);
        free(tb);
        return;
    }
    memmove(&A.tabs[A.cur + 2], &A.tabs[A.cur + 1], (size_t)(A.ntabs - A.cur - 1) * sizeof(A.tabs[0]));
    A.tabs[A.cur + 1] = tb;
    A.ntabs++;
    A.cur++;
    tab_read_cwd(tb);
    send_window_title();
    if (A.ntabs == 2)
        layout_changed();
    pty_set_size();
    A.dirty = true;
}

static void zombie_add(pid_t pid)                                 /* a child that no tab owns any more: reaped when it ends */
{
    if (pid <= 0)
        return;
    if (waitpid(pid, NULL, WNOHANG) != 0)
        return;
    if (A.nzombies < MAX_TABS)
        A.zombies[A.nzombies++] = pid;
}

static void zombies_reap(void)
{
    for (int i = 0; i < A.nzombies;)
        if (waitpid(A.zombies[i], NULL, WNOHANG) != 0)
            A.zombies[i] = A.zombies[--A.nzombies];
        else
            i++;
}

/* The tab goes. `end_program`: its shell is hung up; else the program lives on elsewhere (it was handed to another window). */
static void tab_remove(int i, bool end_program)
{
    struct tab *tb = A.tabs[i];
    if (tb->pty >= 0)
        close(tb->pty);
    if (tb->child > 0) {
        if (end_program)
            kill(tb->child, SIGHUP);
        zombie_add(tb->child);
    } else if (end_program && tb->pid > 0) {
        kill(tb->pid, SIGHUP);
    }
    term_free(&tb->t);
    free(tb);
    memmove(&A.tabs[i], &A.tabs[i + 1], (size_t)(A.ntabs - i - 1) * sizeof(A.tabs[0]));
    A.ntabs--;
    A.tdrag.active = false;
    A.thover = -1;
    if (A.ntabs == 0) {
        A.running = false;
        return;
    }
    bool was_cur = i == A.cur;
    if (i < A.cur)
        A.cur--;
    A.cur = LM_MIN(A.cur, A.ntabs - 1);
    if (was_cur) {
        CUR->activity = false;
        A.sb_drag = false;
        send_window_title();
    }
    if (A.ntabs == 1)
        layout_changed();
    A.dirty = true;
}

static void tab_move(int from, int to)
{
    if (from == to || from < 0 || to < 0 || from >= A.ntabs || to >= A.ntabs)
        return;
    struct tab *front = CUR, *tb = A.tabs[from];
    if (from < to)
        memmove(&A.tabs[from], &A.tabs[from + 1], (size_t)(to - from) * sizeof(A.tabs[0]));
    else
        memmove(&A.tabs[to + 1], &A.tabs[to], (size_t)(from - to) * sizeof(A.tabs[0]));
    A.tabs[to] = tb;
    for (int i = 0; i < A.ntabs; i++)
        if (A.tabs[i] == front)
            A.cur = i;
    A.dirty = true;
}

/* ---- a tab as a window of its own ------------------------------------------------------------------------------------------
 * The window's screen (the grid, both screens, the scrollback, the parser's state) goes into a memfd; the shell's pty master
 * and that memfd are handed to a new `mellagui-term --adopt PID` (fds 3 and 4), which shows them in a window of its own. The shell
 * never notices: it keeps its pty. We stay its parent and reap it when it ends. */

#define STATE_MAGIC "MTERM01\n"

static bool wr_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

static bool rd_all(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n > 0) {
        ssize_t k = read(fd, p, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

static bool line_save(int fd, const struct line *l)
{
    int32_t c = l->cols;
    uint8_t w = l->wrapped;
    return wr_all(fd, &c, sizeof(c)) && wr_all(fd, &w, 1) && wr_all(fd, l->cells, (size_t)c * sizeof(struct cell));
}

static bool line_load(int fd, struct line *l)
{
    int32_t c;
    uint8_t w;
    if (!rd_all(fd, &c, sizeof(c)) || !rd_all(fd, &w, 1) || c < 1 || c > 1024)
        return false;
    l->cols = c;
    l->wrapped = w != 0;
    l->cells = calloc((size_t)c, sizeof(struct cell));
    return rd_all(fd, l->cells, (size_t)c * sizeof(struct cell));
}

static bool tab_save(int fd, const struct tab *tb)
{
    const struct term *t = &tb->t;
    struct term cp = *t;
    cp.scr = cp.main_scr = cp.alt_scr = cp.sb = NULL;               /* pointers mean nothing in another process */
    cp.sel_active = cp.sel_dragging = false;
    uint8_t set = tb->title_set;
    if (!wr_all(fd, STATE_MAGIC, 8) || !wr_all(fd, &cp, sizeof(cp)) || !wr_all(fd, tb->title, sizeof(tb->title)) || !wr_all(fd, &set, 1))
        return false;
    for (int y = 0; y < t->rows; y++)
        if (!line_save(fd, &t->main_scr[y]))
            return false;
    for (int y = 0; y < t->rows; y++)
        if (!line_save(fd, &t->alt_scr[y]))
            return false;
    for (int k = 0; k < t->sb_count; k++)                           /* oldest first */
        if (!line_save(fd, &t->sb[(t->sb_head - t->sb_count + k + 2 * SCROLLBACK) % SCROLLBACK]))
            return false;
    return wr_all(fd, &tb->m, sizeof(tb->m));
}

static bool tab_load(int fd, struct tab *tb)
{
    struct term cp;
    char magic[8];
    uint8_t set;
    if (!rd_all(fd, magic, 8) || memcmp(magic, STATE_MAGIC, 8) || !rd_all(fd, &cp, sizeof(cp)))
        return false;
    if (cp.cols < 8 || cp.cols > 500 || cp.rows < 2 || cp.rows > 200 || cp.sb_count < 0 || cp.sb_count > SCROLLBACK)
        return false;
    if (!rd_all(fd, tb->title, sizeof(tb->title)) || !rd_all(fd, &set, 1))
        return false;
    tb->title[sizeof(tb->title) - 1] = 0;
    tb->title_set = set != 0;
    struct term *t = &tb->t;
    term_free(t);
    *t = cp;
    t->main_scr = calloc((size_t)t->rows, sizeof(struct line));
    t->alt_scr = calloc((size_t)t->rows, sizeof(struct line));
    t->sb = calloc(SCROLLBACK, sizeof(struct line));
    bool ok = true;
    for (int y = 0; ok && y < t->rows; y++)
        ok = line_load(fd, &t->main_scr[y]) && t->main_scr[y].cols == t->cols;
    for (int y = 0; ok && y < t->rows; y++)
        ok = line_load(fd, &t->alt_scr[y]) && t->alt_scr[y].cols == t->cols;
    for (int k = 0; ok && k < t->sb_count; k++)
        ok = line_load(fd, &t->sb[k]);
    if (!ok || !rd_all(fd, &tb->m, sizeof(tb->m)))      /* at the end: a state written by an older terminal has none */
        memset(&tb->m, 0, sizeof(tb->m));
    for (int i = 0; i < 2; i++)
        tb->m.kbd_n[i] = LM_MIN(tb->m.kbd_n[i], KBD_DEPTH);
    t->sb_head = t->sb_count % SCROLLBACK;
    t->scr = t->alt_active ? t->alt_scr : t->main_scr;
    t->view_off = LM_CLAMP(t->view_off, 0, t->sb_count);
    t->cx = LM_CLAMP(t->cx, 0, t->cols - 1);
    t->cy = LM_CLAMP(t->cy, 0, t->rows - 1);
    t->top = LM_CLAMP(t->top, 0, t->rows - 1);
    t->bot = LM_CLAMP(t->bot, t->top, t->rows - 1);
    t->nparams = LM_CLAMP(t->nparams, 0, 32);
    t->osc_len = LM_CLAMP(t->osc_len, 0, (int)sizeof(t->osc) - 1);
    return ok;
}

/* Pulls tab i out of this window into a window of its own. */
static void tab_detach(int i)
{
    if (A.ntabs < 2 || i < 0 || i >= A.ntabs)
        return;
    struct tab *tb = A.tabs[i];
    char self[512], pid[24];
    ssize_t k = readlink("/proc/self/exe", self, sizeof(self) - 1);
    int sfd = memfd_create("mellagui-term-state", 0);
    if (k <= 0 || tb->pid <= 0 || sfd < 0 || !tab_save(sfd, tb) || lseek(sfd, 0, SEEK_SET) < 0) {
        if (sfd >= 0)
            close(sfd);
        return;
    }
    self[k] = 0;
    snprintf(pid, sizeof(pid), "%d", (int)tb->pid);
    char *argv[] = { self, "--adopt", pid, NULL };
    spawn_handover(argv, tb->pty, sfd);
    close(sfd);
    tab_remove(i, false);
}

/* ---- the rail's actions ---------------------------------------------------------------------------------------------------- */

static int rb_hover_of(int id)                                  /* the item's place in its rail */
{
    const int *ids = id >= RB_BIGGER ? rb_right : rb_left;
    for (int k = 0; k < (id >= RB_BIGGER ? 4 : 6); k++)
        if (ids[k] == id)
            return k;
    return -1;
}

/* Starts a program that has nothing to do with this window (a double fork: nobody has to wait for it, and it keeps none of our files). */
static void spawn_detached(char *const argv[])
{
    pid_t p = fork();
    if (p == 0) {
        if (fork() == 0) {
            setsid();
            signal(SIGCHLD, SIG_DFL);
            close_range(3, ~0U, 0);
            execvp(argv[0], argv);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0)
        waitpid(p, NULL, 0);
}

/* Like spawn_detached, but the new program keeps two files: `pty` as fd 3 and `state` as fd 4. */
static void spawn_handover(char *const argv[], int pty, int state)
{
    pid_t p = fork();
    if (p == 0) {
        if (fork() == 0) {
            setsid();
            signal(SIGCHLD, SIG_DFL);
            int a = fcntl(pty, F_DUPFD, 10), b = fcntl(state, F_DUPFD, 10);      /* out of the way of 3 and 4 first */
            if (a < 0 || b < 0 || dup2(a, 3) < 0 || dup2(b, 4) < 0)
                _exit(127);
            close_range(5, ~0U, 0);
            execv(argv[0], argv);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0)
        waitpid(p, NULL, 0);
}

static void sibling_path(const char *name, char *out, size_t n)
{
    char self[512];
    ssize_t k = readlink("/proc/self/exe", self, sizeof(self) - 1);
    snprintf(out, n, "%s", name);
    if (k > 0) {
        self[k] = 0;
        char *sl = strrchr(self, '/');
        if (sl) {
            snprintf(sl + 1, sizeof(self) - (size_t)(sl + 1 - self), "%s", name);
            if (access(self, X_OK) == 0)
                snprintf(out, n, "%s", self);
        }
    }
}

static void new_terminal(void)
{
    char self[512];
    ssize_t k = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (k <= 0)
        return;
    self[k] = 0;
    char *argv[] = { self, NULL };
    spawn_detached(argv);
}

static void files_here(void)
{
    char link[64], dir[PATH_MAX], prog[600];
    snprintf(link, sizeof(link), "/proc/%d/cwd", (int)CUR->pid);
    ssize_t k = CUR->pid > 0 ? readlink(link, dir, sizeof(dir) - 1) : -1;
    if (k <= 0) {
        snprintf(dir, sizeof(dir), "%s", getenv("HOME") ? getenv("HOME") : "/");
        k = (ssize_t)strlen(dir);
    }
    dir[k] = 0;
    sibling_path("mellagui-files", prog, sizeof(prog));
    char *argv[] = { prog, dir, NULL };
    spawn_detached(argv);
}

/* Clear: the scrollback goes (ESC [ 3 J) and the shell is told to redraw its prompt on an empty screen (Ctrl+L). */
static void clear_screen(void)
{
    term_feed(&CUR->t, (const unsigned char *)"\033[3J", 4);
    send_str("\f");
    A.dirty = true;
}

static void cell_from_pointer(int *col, int *row)
{
    *col = LM_CLAMP((int)(A.px - PAD - RAILX) / CELL_W, 0, CUR->t.cols - 1);
    *row = LM_CLAMP((int)(A.py - top_h() - PAD) / CELL_H, 0, CUR->t.rows - 1);
}

/* The wheel, for a program that watches the mouse (it scrolls its own view): one button 64 / 65 report per notch at the pointer's cell.
 * Clicks and drags stay with the terminal (selection). */
static void wheel_report(double travel)
{
    int col, row;
    cell_from_pointer(&col, &row);
    A.wheel_acc += travel;
    while (A.wheel_acc >= 15.0 || A.wheel_acc <= -15.0) {
        bool down = A.wheel_acc > 0;
        A.wheel_acc -= down ? 15.0 : -15.0;
        int b = (down ? 65 : 64) + ((A.mods & LM_MOD_ALT) ? 8 : 0);
        char buf[32];
        if (CUR->m.mouse & MOUSE_SGR) {
            int l = snprintf(buf, sizeof(buf), "\033[<%d;%d;%dM", b, col + 1, row + 1);
            pty_write(buf, (size_t)l);
        } else {                                        /* the old form: three bytes, each 32 more than its value */
            memcpy(buf, "\033[M", 3);
            buf[3] = (char)(32 + b);
            buf[4] = (char)(32 + LM_MIN(col + 1, 223));
            buf[5] = (char)(32 + LM_MIN(row + 1, 223));
            pty_write(buf, 6);
        }
    }
}

static void pointer_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id; (void)data;
    struct term *t = &CUR->t;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        A.px = lm_fixed_to_double(args[2].f);
        A.py = lm_fixed_to_double(args[3].f);
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        wlrail_release(&A.ran[0]);
        wlrail_release(&A.ran[1]);
        if (A.sb_hot) {
            A.sb_hot = false;
            A.dirty = true;
        }
        if (A.hover >= 0) {
            A.hover = -1;
            A.dirty = true;
        }
        if (A.thover != -1) {
            A.thover = -1;
            A.dirty = true;
        }
        break;
    case LM_WL_POINTER_EVT_MOTION:
        A.px = lm_fixed_to_double(args[1].f);
        A.py = lm_fixed_to_double(args[2].f);
        if (btn_at(A.px, A.py) != A.hover) {
            A.hover = btn_at(A.px, A.py);
            A.dirty = true;
        }
        if (A.tdrag.active) {                           /* a tab in the hand: along the bar it changes places, away from it it leaves */
            int from = A.tdrag.idx, to = wltabs_motion(&A.tdrag, A.px, A.py, tabbar_x(), BAR_H, tabbar_w(), A.ntabs);
            if (to >= 0)
                tab_move(from, to);
            A.dirty = true;
            break;
        }
        {
            bool on_x = false;
            int h = tabbar_on() ? wltabs_hit(A.px, A.py, tabbar_x(), BAR_H, tabbar_w(), A.ntabs, &on_x) : WLTABS_NONE;
            int code = h >= 0 ? h * 2 + (on_x ? 1 : 0) : h;
            if (code != A.thover) {
                A.thover = code;
                A.dirty = true;
            }
        }
        if (A.menu_open) {
            int row = menu_row_at(A.px, A.py);
            if (row >= 0 && row != A.menu_hover) {
                A.menu_hover = row;
                A.dirty = true;
            }
            break;
        }
        if (A.sb_drag) {
            sb_drag_to(A.py);
            break;
        }
        bool over_sb = sb_active() && A.px >= A.bw - RAILR - sb_w() && A.px < A.bw - RAILR;
        if (over_sb != A.sb_hot) {                      /* (the thumb lights up under the pointer) */
            A.sb_hot = over_sb;
            A.dirty = true;
        }
        if (t->sel_dragging) {
            cell_from_pointer(&t->sel_x1, &t->sel_y1);
            A.dirty = true;
        }
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        A.last_serial = args[0].u;
        if (A.tdrag.active && args[2].u == BTN_LEFT && args[3].u == 0) {          /* the tab is let go */
            int idx = A.tdrag.idx;
            if (wltabs_release(&A.tdrag) == WLTABS_DETACH)
                tab_detach(idx);
            A.dirty = true;
            break;
        }
        if (args[3].u == 1 && !A.menu_open && !t->sel_dragging && tabbar_on() && A.py >= BAR_H && A.py < BAR_H + WLTABS_H &&
            A.px >= tabbar_x() && A.px < tabbar_x() + tabbar_w()) {
            bool on_x = false;
            int h = wltabs_hit(A.px, A.py, tabbar_x(), BAR_H, tabbar_w(), A.ntabs, &on_x);
            if (args[2].u == BTN_LEFT) {
                if (h == WLTABS_PLUS) {
                    tab_new();
                } else if (h >= 0 && on_x) {
                    tab_remove(h, true);
                } else if (h >= 0) {
                    tab_switch(h);
                    wltabs_press(&A.tdrag, h, A.px, A.py, tabbar_x(), tabbar_w(), A.ntabs);
                }
            } else if (args[2].u == BTN_MIDDLE && h >= 0) {
                tab_remove(h, true);
            }
            A.dirty = true;
            break;
        }
        if (args[3].u == 1 && stat_h() > 0 && A.py >= grid_bottom() && !t->sel_dragging)         /* the status bar is only to look at */
            break;
        if (args[2].u == 273 && args[3].u == 1 && !A.menu_open && A.py >= top_h() && A.py < grid_bottom() && A.px >= RAILX && A.px < A.bw - RAILR && A.menu) {       /* BTN_RIGHT: Copy / Paste, drawn by the compositor */
            WLC_SEND(A.c, A.menu, LM_MELLAGUI_MENU_V1_REQ_POPUP, { .s = t->sel_active ? "Copy\nPaste" : "~Copy\nPaste" });
            break;
        }
        if (A.eat_release && args[3].u == 0) {          /* the release of the click that closed the zoom list */
            A.eat_release = false;
            break;
        }
        if (A.menu_open && args[3].u == 1) {            /* any press: pick an entry, or just close the list */
            int row = menu_row_at(A.px, A.py);
            if (row >= 0 && args[2].u == BTN_LEFT)
                set_zoom_pct(ZOOM_LEVELS[row]);
            A.eat_release = true;
            menu_set(false);
            break;
        }
        if (args[2].u == BTN_LEFT && args[3].u == 0 && A.sb_drag) {
            A.sb_drag = false;
            A.dirty = true;
            break;
        }
        if (args[2].u == BTN_LEFT && args[3].u == 0 && g_rail) {
            wlrail_release(&A.ran[0]);                           /* (the drops spring back) */
            wlrail_release(&A.ran[1]);
            A.dirty = true;
        }
        if (args[2].u == BTN_LEFT && args[3].u == 1 && A.py >= top_h() && A.py < grid_bottom() && A.px >= A.bw - RAILR - sb_w() && A.px < A.bw - RAILR && !t->sel_dragging) {
            if (sb_active()) {                          /* the scroll bar: drag the thumb, or a click beside it pages */
                int ty, th;
                sb_thumb(&ty, &th);
                if (A.py >= ty && A.py < ty + th) {
                    A.sb_drag = true;
                    A.sb_grab = (int)A.py - ty;
                } else {
                    int page = LM_MAX(t->rows - 1, 1);
                    t->view_off = LM_CLAMP(t->view_off + (A.py < ty ? page : -page), 0, t->sb_count);
                    A.dirty = true;
                }
            }
            break;
        }
        if (args[2].u == BTN_LEFT && (g_rail ? (A.px < RAILX || A.px >= A.bw - RAILR) : A.py < BAR_H) && !t->sel_dragging) {
            if (args[3].u == 1 && g_rail) {
                int bi = btn_at(A.px, A.py);
                if (bi >= 0) {
                    bool right = bi >= RB_BIGGER;
                    wlrail_press(&A.ran[right ? 1 : 0], rb_hover_of(bi), wlrail_now());
                    A.dirty = true;
                }
                switch (bi) {
                case RB_NEW: tab_new(); break;
                case RB_COPY: copy_selection(); break;
                case RB_PASTE: paste(); break;
                case RB_CLEAR: clear_screen(); break;
                case RB_BOTTOM: t->view_off = 0; A.dirty = true; break;
                case RB_FILES: files_here(); break;
                case RB_BIGGER: set_zoom(1); break;
                case RB_ZOOM: menu_set(true); break;
                case RB_SMALLER: set_zoom(-1); break;
                case RB_RESET: set_zoom(0); break;
                }
                if (bi == RB_BIGGER || bi == RB_SMALLER || bi == RB_RESET) {         /* the window grows or shrinks: the pointer goes along with the button */
                    A.follow = rb_hover_of(bi);
                    A.follow_w = A.bw;
                    A.follow_ms = now_ms();
                }
            } else if (args[3].u == 1) {
                int bi = btn_at(A.px, A.py);
                switch (bi) {
                case BTN_PASTE_ID: paste(); break;
                case BTN_SMALLER_ID: set_zoom(-1); break;
                case BTN_ZOOM_ID: menu_set(true); break;
                case BTN_BIGGER_ID: set_zoom(1); break;
                case BTN_RESET_ID: set_zoom(0); break;
                }
            }
        } else if (args[2].u == BTN_LEFT) {
            if (args[3].u == 1) {
                cell_from_pointer(&t->sel_x0, &t->sel_y0);
                t->sel_x1 = t->sel_x0;
                t->sel_y1 = t->sel_y0;
                t->sel_dragging = true;
                t->sel_active = false;
                A.dirty = true;
            } else {
                t->sel_dragging = false;
                if (t->sel_x0 != t->sel_x1 || t->sel_y0 != t->sel_y1) {
                    t->sel_active = true;
                    copy_selection();
                } else {
                    t->sel_active = false;
                }
                A.dirty = true;
            }
        } else if (args[2].u == BTN_MIDDLE && args[3].u == 1) {
            paste();
        }
        break;
    case LM_WL_POINTER_EVT_AXIS:
        modes_check(CUR);
        if (tabbar_on() && A.py >= BAR_H && A.py < BAR_H + WLTABS_H && args[1].u == LM_POINTER_AXIS_VERTICAL) {      /* the wheel over the tabs changes tab */
            tab_cycle(args[2].f > 0 ? 1 : -1);
        } else if ((A.mods & LM_MOD_CTRL) && args[1].u == LM_POINTER_AXIS_VERTICAL) {
            set_zoom(args[2].f > 0 ? -1 : 1);
        } else if (args[1].u == LM_POINTER_AXIS_VERTICAL && (CUR->m.mouse & MOUSE_ON) && !(A.mods & LM_MOD_SHIFT)) {
            wheel_report(lm_fixed_to_double(args[2].f));
        } else if (args[1].u == LM_POINTER_AXIS_VERTICAL && !t->alt_active) {
            int lines = (int)(lm_fixed_to_double(args[2].f) / 15.0 * 3.0);
            if (lines == 0)
                lines = args[2].f > 0 ? 1 : -1;
            t->view_off = LM_CLAMP(t->view_off - lines, 0, t->sb_count);
            A.dirty = true;
        } else if (t->alt_active) {
            send_str(args[2].f > 0 ? "\033[B\033[B\033[B" : "\033[A\033[A\033[A");
        }
        break;
    }
}

/* ======================================================================== *
 *  Protocol plumbing
 * ======================================================================== */

static void apply_size(int w, int h)
{
    int cols = (w - 2 * PAD - sb_w() - RAILX - RAILR) / CELL_W, rows = (h - 2 * PAD - top_h() - stat_h()) / CELL_H;
    cols = LM_CLAMP(cols, 8, 500);
    rows = LM_CLAMP(rows, 2, 200);
    bool changed = false;
    for (int i = 0; i < A.ntabs; i++)
        if (cols != A.tabs[i]->t.cols || rows != A.tabs[i]->t.rows) {
            term_resize(&A.tabs[i]->t, cols, rows);
            changed = true;
        }
    if (changed)
        pty_set_size();
    A.dirty = true;
}

static void wm_base_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)data;
    if (op == LM_XDG_WM_BASE_EVT_PING)
        WLC_SEND(c, id, LM_XDG_WM_BASE_REQ_PONG, { .u = args[0].u });
}

static void xsurface_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)data;
    if (op == LM_XDG_SURFACE_EVT_CONFIGURE) {
        WLC_SEND(c, id, LM_XDG_SURFACE_REQ_ACK_CONFIGURE, { .u = args[0].u });
        if (A.win_w > 0 && A.win_h > 0 && !A.grown)             /* an old size must not undo a zoom that is about to be committed */
            apply_size(A.win_w, A.win_h);
        A.configured = true;
        A.dirty = true;
    }
}

static void toplevel_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id; (void)data;
    if (op == LM_XDG_TOPLEVEL_EVT_CONFIGURE) {
        if (args[0].i > 0 && args[1].i > 0) {
            A.win_w = args[0].i;
            A.win_h = args[1].i;
        }
        A.fixed_size = false;
        const uint32_t *st = args[2].a.data;
        for (uint32_t i = 0; st && i < args[2].a.size / 4; i++)
            if (st[i] == LM_XDG_TOPLEVEL_STATE_MAXIMIZED || st[i] == LM_XDG_TOPLEVEL_STATE_FULLSCREEN)
                A.fixed_size = true;
    } else if (op == LM_XDG_TOPLEVEL_EVT_CLOSE) {
        A.running = false;
    }
}

static void seat_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)data;
    if (op != LM_WL_SEAT_EVT_CAPABILITIES)
        return;
    if ((args[0].u & 1) && !A.pointer) {
        A.pointer = wlc_alloc(c, &lm_wl_pointer_interface, pointer_event, NULL);
        WLC_SEND(c, id, LM_WL_SEAT_REQ_GET_POINTER, { .n = A.pointer });
    }
    if ((args[0].u & 2) && !A.keyboard) {
        A.keyboard = wlc_alloc(c, &lm_wl_keyboard_interface, keyboard_event, NULL);
        WLC_SEND(c, id, LM_WL_SEAT_REQ_GET_KEYBOARD, { .n = A.keyboard });
    }
}

static void registry_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)id; (void)data;
    if (op != LM_WL_REGISTRY_EVT_GLOBAL)
        return;
    uint32_t name = args[0].u, ver = args[2].u;
    const char *iface = args[1].s;
    if (!strcmp(iface, "wl_compositor"))
        A.compositor = wlc_bind(c, A.registry, name, &lm_wl_compositor_interface, LM_MIN(ver, 4u), NULL, NULL);
    else if (!strcmp(iface, "wl_shm"))
        A.shm = wlc_bind(c, A.registry, name, &lm_wl_shm_interface, 1, NULL, NULL);
    else if (!strcmp(iface, "xdg_wm_base"))
        A.wm_base = wlc_bind(c, A.registry, name, &lm_xdg_wm_base_interface, LM_MIN(ver, 3u), wm_base_event, NULL);
    else if (!strcmp(iface, "wl_seat"))
        A.seat = wlc_bind(c, A.registry, name, &lm_wl_seat_interface, LM_MIN(ver, 5u), seat_event, NULL);
    else if (!strcmp(iface, "wl_output") && A.nout < 8) {
        A.out_id[A.nout] = wlc_bind(c, A.registry, name, &lm_wl_output_interface, LM_MIN(ver, 3u), output_event, NULL);
        A.out_scale[A.nout++] = 1;
    } else if (!strcmp(iface, "wl_data_device_manager"))
        A.ddm = wlc_bind(c, A.registry, name, &lm_wl_data_device_manager_interface, LM_MIN(ver, 3u), NULL, NULL);
    else if (!strcmp(iface, "mellagui_menu_v1"))
        A.menu = wlc_bind(c, A.registry, name, &lm_mellagui_menu_v1_interface, 1, menu_event, NULL);
    else if (!strcmp(iface, "zxdg_decoration_manager_v1"))
        A.decoration_mgr = wlc_bind(c, A.registry, name, &lm_zxdg_decoration_manager_v1_interface, 1, NULL, NULL);
}

static void sigchld(int s)
{
    (void)s;
}

int main(int argc, char **argv)
{
    lm_theme_load_env();
    const char *title = NULL;
    pid_t adopt = 0;                                            /* --adopt PID: the shell of a tab that was dragged out of another window */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "--exec")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "mellagui-term: -e needs a command\n");
                return 2;
            }
            g_exec_argv = &argv[i + 1];
            break;
        } else if ((!strcmp(argv[i], "--title") || !strcmp(argv[i], "-T")) && i + 1 < argc) {
            title = argv[++i];
        } else if (!strcmp(argv[i], "--adopt") && i + 1 < argc) {
            adopt = (pid_t)atoi(argv[++i]);
            if (adopt <= 0) {
                fprintf(stderr, "mellagui-term: --adopt needs the shell's pid\n");
                return 2;
            }
        } else {
            fprintf(stderr, "usage: mellagui-term [--title TEXT] [-e COMMAND [ARGS...]]\n");
            return strcmp(argv[i], "-h") && strcmp(argv[i], "--help") ? 2 : 0;
        }
    }
    A.running = true;
    g_rail = wlrail_wanted("MELLAGUI_TERM_RAIL");
    wlrail_init(&A.ran[0]);
    wlrail_init(&A.ran[1]);
    A.follow = -1;
    A.hover = -1;
    A.zoom = ZOOM_DEFAULT;
    A.status = true;
    A.status_conf = -1;
    A.thover = -1;
    build_zoom_tables();
    conf_load();                                                /* the size of last time */
    const char *st = getenv("MELLAGUI_TERM_STATUS");           /* 0 / 1 decide over the settings file (the tests switch the bar off) */
    if (st && (!strcmp(st, "0") || !strcmp(st, "1")))
        A.status = *st == '1';
    A.repeat_fd = -1;
    A.repeat_rate = 25;
    A.repeat_delay = 400;
    struct tab *first = tab_alloc(DEFAULT_COLS, DEFAULT_ROWS);
    A.tabs[0] = first;
    A.ntabs = 1;
    if (adopt) {                                                /* a tab handed over: its pty is fd 3, its screen fd 4 */
        if (!tab_load(4, first)) {
            fprintf(stderr, "mellagui-term: the handed over screen could not be read\n");
            return 1;
        }
        close(4);
        first->pty = 3;
        first->pid = adopt;
        fcntl(3, F_SETFD, FD_CLOEXEC);
    } else if (title) {
        snprintf(first->title, sizeof(first->title), "%s", title);
        first->title_set = true;
    } else if (g_exec_argv) {
        const char *bn = strrchr(g_exec_argv[0], '/');
        snprintf(first->title, sizeof(first->title), "%s", bn ? bn + 1 : g_exec_argv[0]);
        first->title_set = true;
    }
    snprintf(A.title, sizeof(A.title), "%s", first->title);

    A.c = wlc_connect(NULL);
    if (!A.c)
        return 1;
    A.registry = wlc_get_registry(A.c, registry_event, NULL);
    if (wlc_roundtrip(A.c) < 0 || !A.compositor || !A.shm || !A.wm_base) {
        fprintf(stderr, "mellagui-term: compositor lacks wl_compositor / wl_shm / xdg_wm_base\n");
        return 1;
    }
    if (!adopt) {
        first->pty = spawn_shell(first, g_exec_argv, NULL);
        if (first->pty < 0)
            return 1;
    }
    tab_read_cwd(first);
    A.cwd_ms = now_ms();
    signal(SIGCHLD, sigchld);
    A.repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

    A.surface = wlc_alloc(A.c, &lm_wl_surface_interface, NULL, NULL);
    WLC_SEND(A.c, A.compositor, LM_WL_COMPOSITOR_REQ_CREATE_SURFACE, { .n = A.surface });
    A.xsurface = wlc_alloc(A.c, &lm_xdg_surface_interface, xsurface_event, NULL);
    WLC_SEND(A.c, A.wm_base, LM_XDG_WM_BASE_REQ_GET_XDG_SURFACE, { .n = A.xsurface }, { .o = A.surface });
    A.toplevel = wlc_alloc(A.c, &lm_xdg_toplevel_interface, toplevel_event, NULL);
    WLC_SEND(A.c, A.xsurface, LM_XDG_SURFACE_REQ_GET_TOPLEVEL, { .n = A.toplevel });
    WLC_SEND(A.c, A.toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = A.title });
    WLC_SEND(A.c, A.toplevel, LM_XDG_TOPLEVEL_REQ_SET_APP_ID, { .s = "mellagui.term" });
    if (A.decoration_mgr) {
        A.deco = wlc_alloc(A.c, &lm_zxdg_toplevel_decoration_v1_interface, NULL, NULL);
        WLC_SEND(A.c, A.decoration_mgr, LM_ZXDG_DECORATION_MANAGER_V1_REQ_GET_TOPLEVEL_DECORATION,
                 { .n = A.deco }, { .o = A.toplevel });
    }
    if (A.ddm && A.seat) {
        A.ddev = wlc_alloc(A.c, &lm_wl_data_device_interface, ddev_event, NULL);
        WLC_SEND(A.c, A.ddm, LM_WL_DATA_DEVICE_MANAGER_REQ_GET_DATA_DEVICE, { .n = A.ddev }, { .o = A.seat });
    }
    WLC_SEND0(A.c, A.surface, LM_WL_SURFACE_REQ_COMMIT);

    unsigned char buf[16384];
    while (A.running && !wlc_dead(A.c)) {
        maybe_render();
        if (A.follow >= 0) {                                     /* the new size is on screen: the pointer to the button where it is now */
            if (A.bw != A.follow_w && !A.dirty) {
                struct wlrail L, R;
                int ys[WLRAIL_MAX];
                term_rail(&L, &R);
                wlrail_layout(&R, ys);
                if (A.menu && ys[A.follow] >= 0)
                    WLC_SEND(A.c, A.menu, LM_MELLAGUI_MENU_V1_REQ_WARP, { .i = R.x + WLRAIL_W / 2 }, { .i = ys[A.follow] + WLRAIL_D / 2 });
                A.follow = -1;
            } else if (now_ms() - A.follow_ms > 800) {
                A.follow = -1;
            }
        }
        wlc_flush(A.c);
        struct pollfd pf[MAX_TABS + 2];
        pf[0] = (struct pollfd){ .fd = wlc_fd(A.c), .events = POLLIN };
        pf[1] = (struct pollfd){ .fd = A.repeat_fd, .events = POLLIN };
        int nt = A.ntabs;
        for (int i = 0; i < nt; i++)
            pf[2 + i] = (struct pollfd){ .fd = A.tabs[i]->pty, .events = POLLIN };
        int to = A.dirty && A.frame_cb ? 50 : A.conf_dirty ? 200 : 1000;
        if (g_rail) {                                            /* the drops of the rails are moving: a frame every 16 ms */
            struct wlrail L, R;
            term_rail(&L, &R);
            int64_t now = wlrail_now();
            bool m0 = wlrail_step(&A.ran[0], &L, rb_hover(false), now), m1 = wlrail_step(&A.ran[1], &R, rb_hover(true), now);
            if (m0 || m1) {
                A.dirty = true;
                to = LM_MIN(to, 16);
            }
        }
        int r = poll(pf, (nfds_t)(2 + nt), to);
        if (r < 0 && errno != EINTR)
            break;
        /* the programs' output first (nothing has changed the tabs since poll), from the last tab to the first because a tab may end */
        for (int i = nt - 1; r > 0 && i >= 0; i--) {
            if (!(pf[2 + i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            struct tab *tb = A.tabs[i];
            bool ended = false;
            for (;;) {
                ssize_t n = read(tb->pty, buf, sizeof(buf));
                if (n > 0) {
                    g_feed = tb;
                    term_feed(&tb->t, buf, (size_t)n);
                    g_feed = NULL;
                    if (i == A.cur) {
                        A.dirty = true;
                    } else if (!tb->activity) {
                        tb->activity = true;
                        A.dirty = true;
                    }
                } else if (n == 0 || (n < 0 && errno == EIO)) {
                    ended = true;
                    break;
                } else {
                    break;
                }
            }
            if (ended) {
                tb->child = tb->child > 0 && waitpid(tb->child, NULL, WNOHANG) == 0 ? tb->child : 0;     /* (a child still running is reaped later) */
                tab_remove(i, true);
            }
        }
        if (!A.running)
            break;
        if (r > 0 && (pf[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (wlc_dispatch_pending(A.c) < 0)
                break;
        }
        if (r > 0 && (pf[1].revents & POLLIN)) {
            uint64_t exp;
            if (read(A.repeat_fd, &exp, sizeof(exp)) > 0 && A.repeat_code)
                key_to_pty(A.repeat_code, true);
        }
        if (A.conf_dirty && now_ms() - A.conf_time >= 400) {
            conf_save();
            A.conf_dirty = false;
        }
        for (int i = A.ntabs - 1; i >= 0; i--) {                 /* a program that has ended: its tab closes with it */
            struct tab *tb = A.tabs[i];
            int status;
            if (tb->child > 0 && waitpid(tb->child, &status, WNOHANG) == tb->child) {
                tb->child = 0;
                tb->pid = 0;
                tab_remove(i, false);
            }
        }
        zombies_reap();
        if (now_ms() - A.cwd_ms >= 1000 && A.ntabs > 0)
            cwd_refresh();
    }
    if (A.conf_dirty)
        conf_save();
    for (int i = 0; i < A.ntabs; i++) {
        struct tab *tb = A.tabs[i];
        if (tb->child > 0) {
            kill(tb->child, SIGHUP);
            waitpid(tb->child, NULL, WNOHANG);
        } else if (tb->pid > 0) {
            kill(tb->pid, SIGHUP);
        }
    }
    wlc_destroy(A.c);
    return 0;
}
