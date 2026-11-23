/*
 * wlhelp.h - a help card of several pages, the same everywhere (header only; the picture editor has an older copy of the same thing in its own file, a
 * program that is new takes this one). The card is a `wlcard` the program already makes (a width of about 900 and a height of about 560 are comfortable; it
 * shrinks with the window): the list of pages at the left, the text of the page at the right with a scroll bar (`wlsbar.h`), and a footer with < Back /
 * Next > and the page number. The program draws the frame and its own Close button (the last button of the card), this file the rest.
 *
 * A page is a plain list of source lines (single blanks between the words only: the lines are wrapped to the card's width, in pixels):
 *     "# Heading"            a heading, in the accent colour
 *     "- a bullet"           a paragraph with a dot
 *     "|keys|what they do"   a row of a table: the keys in bold, the words wrapped beside them
 *     ""                     a gap
 *     anything else          a paragraph
 *
 *   static const char *const P1[] = { "# Moving", "|W A S D|Walk." };
 *   static const struct wlhelp_page PAGES[] = { { "Moving", P1, WLHELP_N(P1) }, ... };
 *   wlhelp_init(&h, PAGES, n);                  once
 *   wlhelp_open(&h);                            when the card opens (the page last read is kept, scrolled to its top)
 *   wlhelp_paint(&h, win, &card, px, py, has_pointer);
 *   wlhelp_motion / wlhelp_press / wlhelp_release / wlhelp_wheel                         the pointer
 *   wlhelp_nav(&h, &card, WLHELP_LEFT ...)                                               a key (the program maps its keys to the actions)
 *   wlhelp_dump(&h, FILE *, &card)                                                       `help` / `hpage` / `hbtn` / `hbar` lines for a test
 */
#ifndef MELLAGUI_WLHELP_H
#define MELLAGUI_WLHELP_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "font.h"
#include "theme.h"
#include "wlcard.h"
#include "wlsbar.h"
#include "wlwin.h"

struct wlhelp_page {
    const char *title;
    const char *const *lines;
    int n;
};
#define WLHELP_N(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

enum { WLHELP_HEAD = 1, WLHELP_TEXT, WLHELP_KEY, WLHELP_DOT };
struct wlhelp_line {
    const char *p;
    int len, x, y, kind;
};
#define WLHELP_MAX 900
#define WLHELP_LH (LM_FONT_H + 4)

struct wlhelp {
    const struct wlhelp_page *pages;
    int npages;
    int page, top, hover, btn;                  /* the page shown, how far it is scrolled (pixels), the page row / button the pointer is on */
    int grab;                                   /* where in the thumb the scroll bar is held, -1 = not held */
    struct wlhelp_line L[WLHELP_MAX];
    int nl, h, lpage, lw;                       /* the laid out page: its lines, its height, which page and which width it was made for */
    char pool[8192];
    int pool_n;
};

/* Where the parts lie, from the card. */
struct wlhelp_geo {
    int sx, sy, sw, rowh;                       /* the list of pages */
    int cx, cy, cw, ch;                         /* the text */
    int bx, by, bw, bh;                         /* the scroll bar's track */
    int py, pw, ph, prev_x, next_x;             /* the footer's buttons */
};

enum { WLHELP_LEFT = 1, WLHELP_RIGHT, WLHELP_UP, WLHELP_DOWN, WLHELP_PGUP, WLHELP_PGDN, WLHELP_HOME, WLHELP_END, WLHELP_PAGE /* + arg: a page number */ };

static inline void wlhelp_init(struct wlhelp *h, const struct wlhelp_page *pages, int n)
{
    memset(h, 0, sizeof *h);
    h->pages = pages;
    h->npages = n;
    h->hover = -1;
    h->grab = -1;
    h->lpage = h->lw = -1;
}

static inline void wlhelp_open(struct wlhelp *h)
{
    h->hover = -1;
    h->btn = 0;
    h->grab = -1;
    h->top = 0;
}

static inline int wlhelp_text_w(const char *s, int kind)
{
    if (kind == WLHELP_HEAD || kind == WLHELP_KEY)
        return lm_font_ui_cells_get() ? (int)strlen(s) * LM_FONT_W : lm_font_text_width_bold(s);
    return wlcard_text_w(s);
}

/* Wraps `s` to `width` pixels from `x`, one line each; `*y` moves down. */
static inline int wlhelp_wrap(struct wlhelp *h, const char *s, int x, int width, int *y, int kind)
{
    size_t n = strlen(s), pos = 0;
    int lines = 0;
    while (pos < n && h->nl < WLHELP_MAX) {
        size_t end = pos;
        char buf[600];
        for (;;) {
            size_t nx = end;
            while (nx < n && s[nx] == ' ')
                nx++;
            while (nx < n && s[nx] != ' ')
                nx++;
            if (nx == end)
                break;
            size_t len = nx - pos;
            if (len >= sizeof(buf))
                len = sizeof(buf) - 1;
            memcpy(buf, s + pos, len);
            buf[len] = 0;
            if (end > pos && wlhelp_text_w(buf, kind) > width)
                break;                                                  /* (a word that does not fit starts the next line) */
            end = nx;
            if (end >= n)
                break;
        }
        if (end == pos)
            break;
        h->L[h->nl++] = (struct wlhelp_line){ s + pos, (int)(end - pos), x, *y, kind };
        *y += WLHELP_LH;
        lines++;
        pos = end;
        while (pos < n && s[pos] == ' ')
            pos++;
    }
    return lines;
}

/* Sets the page out for a text width; kept until the page or the width changes. */
static inline void wlhelp_layout(struct wlhelp *h, int width)
{
    if (h->page == h->lpage && width == h->lw)
        return;
    h->lpage = h->page;
    h->lw = width;
    h->nl = 0;
    h->pool_n = 0;
    int y = 0;
    const struct wlhelp_page *hp = &h->pages[h->page];
    for (int i = 0; i < hp->n; i++) {
        const char *s = hp->lines[i];
        if (!*s) {
            y += 8;
        } else if (s[0] == '#') {
            if (y)
                y += 10;
            wlhelp_wrap(h, s + 2, 0, width, &y, WLHELP_HEAD);
            y += 3;
        } else if (s[0] == '|') {                                       /* |keys|words: a row of a table */
            const char *bar = strchr(s + 1, '|');
            if (!bar)
                continue;
            int kw = width / 3 < 190 ? width / 3 : 190, y0 = y;
            size_t kl = (size_t)(bar - s - 1);
            if (h->pool_n + (int)kl + 1 > (int)sizeof(h->pool))
                continue;
            char *key = h->pool + h->pool_n;                            /* (the lines keep pointing at it: not a local) */
            memcpy(key, s + 1, kl);
            key[kl] = 0;
            h->pool_n += (int)kl + 1;
            wlhelp_wrap(h, key, 0, kw, &y, WLHELP_KEY);
            wlhelp_wrap(h, bar + 1, kw + 12, width - kw - 12, &y0, WLHELP_TEXT);
            y = (y > y0 ? y : y0) + 4;
        } else if (s[0] == '-' && s[1] == ' ') {                        /* a bullet */
            int y0 = y;
            if (h->nl < WLHELP_MAX)
                h->L[h->nl++] = (struct wlhelp_line){ "", 0, 4, y0, WLHELP_DOT };
            wlhelp_wrap(h, s + 2, 18, width - 18, &y, WLHELP_TEXT);
            y += 3;
        } else {
            wlhelp_wrap(h, s, 0, width, &y, WLHELP_TEXT);
            y += 4;
        }
    }
    h->h = y;
}

static inline void wlhelp_geom(const struct wlhelp *h, const struct wlcard *c, struct wlhelp_geo *g)
{
    int top = c->y + 40, bot = c->y + c->h - WLCARD_PAD - WLCARD_BTN_H - 12;
    int rowh = h->npages > 0 ? (bot - top) / h->npages : 28;
    g->sx = c->x + WLCARD_PAD;
    g->sy = top;
    g->sw = 164;
    g->rowh = rowh < 18 ? 18 : rowh > 28 ? 28 : rowh;
    g->cx = g->sx + g->sw + 16;
    g->cy = top;
    g->cw = c->x + c->w - WLCARD_PAD - wlsbar_w() - 8 - g->cx;                  /* (the text ends where the scroll bar begins) */
    g->ch = bot - top;
    g->bw = wlsbar_w();
    g->bx = c->x + c->w - WLCARD_PAD - g->bw;
    g->by = top;
    g->bh = bot - top;
    g->py = c->y + c->h - WLCARD_PAD - WLCARD_BTN_H;
    g->pw = 84;
    g->ph = WLCARD_BTN_H;
    g->prev_x = g->sx;
    g->next_x = g->sx + g->pw + 6;
}

static inline struct wlsbar wlhelp_sbar(const struct wlhelp *h, const struct wlhelp_geo *g)
{
    return (struct wlsbar){ g->bx, g->by, g->bw, g->bh, h->h, g->ch, h->top };
}

static inline int wlhelp_max_top(const struct wlhelp *h, const struct wlhelp_geo *g)
{
    return h->h - g->ch > 0 ? h->h - g->ch : 0;
}

static inline void wlhelp_set_page(struct wlhelp *h, int page)
{
    page = page < 0 ? 0 : page >= h->npages ? h->npages - 1 : page;
    if (page != h->page)
        h->top = 0;
    h->page = page;
}

static inline void wlhelp_paint(struct wlhelp *h, struct wlwin *w, const struct wlcard *c, double px, double py, bool has_pointer)
{
    struct wlhelp_geo g;
    wlhelp_geom(h, c, &g);
    wlhelp_layout(h, g.cw);
    int mt = wlhelp_max_top(h, &g);
    h->top = h->top < 0 ? 0 : h->top > mt ? mt : h->top;
    for (int i = 0; i < h->npages; i++) {                               /* the pages */
        int y = g.sy + i * g.rowh;
        bool cur = i == h->page, hot = i == h->hover;
        if (cur)
            wlwin_round_fill(w, g.sx - 4, y, g.sw + 4, g.rowh - 2, 7, lm_theme.control_hot);
        else if (hot)
            wlwin_round_fill(w, g.sx - 4, y, g.sw + 4, g.rowh - 2, 7, lm_theme.control);
        char t[96];
        snprintf(t, sizeof(t), "%d  %s", i + 1, h->pages[i].title);
        if (cur)
            wlwin_text_bold(w, g.sx + 2, y + (g.rowh - 2 - LM_FONT_H) / 2, t, 1, lm_theme.text_bright, g.sw - 4);
        else
            wlwin_text(w, g.sx + 2, y + (g.rowh - 2 - LM_FONT_H) / 2, t, 1, lm_theme.text_soft, g.sw - 4);
    }
    wlwin_fill(w, g.cx - 9, g.cy, 1, g.ch, lm_theme.border);
    for (int i = 0; i < h->nl; i++) {                                   /* the text of the page: only the lines that fit whole */
        const struct wlhelp_line *l = &h->L[i];
        int y = l->y - h->top;
        if (y < 0 || y + WLHELP_LH > g.ch)
            continue;
        char buf[600];
        int n = l->len < (int)sizeof(buf) - 1 ? l->len : (int)sizeof(buf) - 1;
        memcpy(buf, l->p, (size_t)n);
        buf[n] = 0;
        int x = g.cx + l->x, ty = g.cy + y;
        switch (l->kind) {
        case WLHELP_HEAD: wlwin_text_bold(w, x, ty, buf, 1, lm_theme.accent, g.cw - l->x); break;
        case WLHELP_KEY: wlwin_text_bold(w, x, ty, buf, 1, lm_theme.text_bright, g.cw - l->x); break;
        case WLHELP_DOT: wlwin_round_fill(w, x, ty + LM_FONT_H / 2 - 2, 5, 5, 2, lm_theme.accent); break;
        default: wlwin_text(w, x, ty, buf, 1, lm_theme.text_soft, g.cw - l->x); break;
        }
    }
    struct wlsbar sb = wlhelp_sbar(h, &g);                              /* the scroll bar */
    wlsbar_paint(w, &sb, h->grab >= 0 || (has_pointer && wlsbar_has(&sb, px, py)));
    bool can_prev = h->page > 0, can_next = h->page < h->npages - 1;    /* the footer */
    wlcard_drop(w, g.prev_x, g.py, g.pw, g.ph, can_prev ? lm_theme.btn_slate : lm_theme.btn_grey, h->btn == 1 && can_prev, false, "< Back", lm_theme.on_colour);
    wlcard_drop(w, g.next_x, g.py, g.pw, g.ph, can_next ? lm_theme.btn_slate : lm_theme.btn_grey, h->btn == 2 && can_next, false, "Next >", lm_theme.on_colour);
    char pg[48];
    snprintf(pg, sizeof(pg), "Page %d of %d", h->page + 1, h->npages);
    wlwin_text(w, g.next_x + g.pw + 14, g.py + (g.ph - 2 - LM_FONT_H) / 2, pg, 1, lm_theme.text_dim, 0);
    int hx = g.next_x + g.pw + 14 + wlcard_text_w(pg) + 20, hw = c->x + c->w - WLCARD_PAD - WLCARD_BTN_W - 16 - hx;
    if (hw > 120)
        wlwin_text(w, hx, g.py + (g.ph - 2 - LM_FONT_H) / 2, "Left / Right: page   Up / Down / wheel: scroll   Esc: back", 1, lm_theme.text_dim, hw);
}

/* What the pointer is on: 1000 + a page row, 1 Back, 2 Next, 3 the scroll bar, 0 nothing. */
static inline int wlhelp_hit(struct wlhelp *h, const struct wlcard *c, double x, double y)
{
    struct wlhelp_geo g;
    wlhelp_geom(h, c, &g);
    wlhelp_layout(h, g.cw);
    if (x >= g.sx - 4 && x < g.sx + g.sw && y >= g.sy && y < g.sy + h->npages * g.rowh)
        return 1000 + (int)((y - g.sy) / g.rowh);
    if (y >= g.py && y < g.py + g.ph) {
        if (x >= g.prev_x && x < g.prev_x + g.pw)
            return 1;
        if (x >= g.next_x && x < g.next_x + g.pw)
            return 2;
    }
    struct wlsbar sb = wlhelp_sbar(h, &g);
    return wlsbar_has(&sb, x, y) ? 3 : 0;
}

/* The pointer moved: what it is on lights up, a held thumb follows it. */
static inline void wlhelp_motion(struct wlhelp *h, const struct wlcard *c, double px, double py)
{
    int hit = wlhelp_hit(h, c, px, py);
    h->hover = hit >= 1000 ? hit - 1000 : -1;
    h->btn = hit == 1 || hit == 2 ? hit : 0;
    if (h->grab >= 0) {
        struct wlhelp_geo g;
        wlhelp_geom(h, c, &g);
        struct wlsbar sb = wlhelp_sbar(h, &g);
        h->top = wlsbar_drag(&sb, py, h->grab);
    }
}

/* A press: the scroll bar's thumb is taken, the track beside it pages. True when it did something. */
static inline bool wlhelp_press(struct wlhelp *h, const struct wlcard *c, double px, double py)
{
    if (wlhelp_hit(h, c, px, py) != 3)
        return false;
    struct wlhelp_geo g;
    wlhelp_geom(h, c, &g);
    struct wlsbar sb = wlhelp_sbar(h, &g);
    int grab = 0;
    if (wlsbar_press(&sb, px, py, &h->top, &grab) == 1)
        h->grab = grab;
    return true;
}

/* The button is let go: a page row or Back / Next is done where it is let go. True when it was one of them. */
static inline bool wlhelp_release(struct wlhelp *h, const struct wlcard *c, double px, double py)
{
    h->grab = -1;
    int hit = wlhelp_hit(h, c, px, py);
    if (hit >= 1000)
        wlhelp_set_page(h, hit - 1000);
    else if (hit == 1)
        wlhelp_set_page(h, h->page - 1);
    else if (hit == 2)
        wlhelp_set_page(h, h->page + 1);
    else
        return false;
    return true;
}

static inline void wlhelp_scroll(struct wlhelp *h, const struct wlcard *c, int dy)
{
    struct wlhelp_geo g;
    wlhelp_geom(h, c, &g);
    wlhelp_layout(h, g.cw);
    int mt = wlhelp_max_top(h, &g), t = h->top + dy;
    h->top = t < 0 ? 0 : t > mt ? mt : t;
}

/* A key, as one of the WLHELP_ actions (WLHELP_PAGE: `arg` is the page, 0 based). */
static inline void wlhelp_nav(struct wlhelp *h, const struct wlcard *c, int action, int arg)
{
    switch (action) {
    case WLHELP_LEFT: wlhelp_set_page(h, h->page - 1); break;
    case WLHELP_RIGHT: wlhelp_set_page(h, h->page + 1); break;
    case WLHELP_UP: wlhelp_scroll(h, c, -WLHELP_LH * 2); break;
    case WLHELP_DOWN: wlhelp_scroll(h, c, WLHELP_LH * 2); break;
    case WLHELP_PGUP: wlhelp_scroll(h, c, -200); break;
    case WLHELP_PGDN: wlhelp_scroll(h, c, 200); break;
    case WLHELP_HOME: h->top = 0; break;
    case WLHELP_END: wlhelp_scroll(h, c, 100000); break;
    case WLHELP_PAGE: wlhelp_set_page(h, arg); break;
    default: break;
    }
}

/* For a test's dump: the page, where the parts lie. Spaces in the title as `_`. */
static inline void wlhelp_dump(struct wlhelp *h, FILE *f, const struct wlcard *c)
{
    struct wlhelp_geo g;
    wlhelp_geom(h, c, &g);
    wlhelp_layout(h, g.cw);
    char t[96];
    snprintf(t, sizeof t, "%s", h->pages[h->page].title);
    for (char *p = t; *p; p++)
        if (*p == ' ')
            *p = '_';
    fprintf(f, "help page=%d pages=%d title=%s top=%d height=%d view=%d lines=%d\n", h->page, h->npages, t, h->top, h->h, g.ch, h->nl);
    for (int i = 0; i < h->npages; i++)
        fprintf(f, "hpage %d %d %d %d %d\n", i, g.sx - 4, g.sy + i * g.rowh, g.sw + 4, g.rowh);
    fprintf(f, "hbtn back %d %d %d %d\n", g.prev_x, g.py, g.pw, g.ph);
    fprintf(f, "hbtn next %d %d %d %d\n", g.next_x, g.py, g.pw, g.ph);
    fprintf(f, "hbar %d %d %d %d\n", g.bx, g.by, g.bw, g.bh);
}

#endif
