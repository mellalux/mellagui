/*
 * wlpicker.h - the folder list of MellaGUI's Open / Save as cards, the same in every program.
 *
 * A card has a text field with a path (`field`) and, under it, a list of the folder the path names: ".." first, folders, then files (a
 * program may say which files it wants), only what starts with the name typed after the last slash. The program owns the card (its frame,
 * the field, the buttons) and the one `struct wlpicker`; this header does the rest, so that a click, a key or a double click means the same
 * in the file manager's neighbours:
 *
 *   wlpicker_init(&p, want)             once (want(name) = false hides a file; NULL = every file)
 *   wlpicker_reset(&p)                  when the card opens or the path is replaced: the list is read again, nothing is chosen
 *   wlpicker_fit(window_h, other_h)     how many rows fit (0 = too low for a list; other_h = the card's height without the rows)
 *   wlpicker_paint(...)                 the list with its scroll bar
 *   wlpicker_key(...)                   Up / Down / PgUp / PgDn move the row, Enter on a chosen row takes it
 *   wlpicker_press / _motion / _wheel / _release     the pointer: a click puts the name in the field (a folder is entered), a double click
 *                                       on a file takes it, the scroll bar's thumb is dragged, the track pages
 *   wlpicker_typed(&p)                  the field was edited: the list follows what is typed
 *   wlpicker_dump(...)                  the `clist` / `crow` lines of the programs' test dumps
 *
 * `base` is the folder a relative path (and the empty field) means. `~/` and absolute paths are understood (pathcomp.h).
 */
#ifndef MELLAGUI_WLPICKER_H
#define MELLAGUI_WLPICKER_H

#include <dirent.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "font.h"
#include "keymap.h"
#include "metrics.h"
#include "pathcomp.h"
#include "theme.h"
#include "uikit.h"
#include "util.h"
#include "wltext.h"
#include "wlwin.h"

#define WLPICKER_ROW_H 22
#define WLPICKER_MAX_ROWS 10
#define WLPICKER_MAX_ENTRIES 4000

struct wlpicker_ent {
    char name[256];
    bool dir;
};

struct wlpicker {
    struct wlpicker_ent *ent;
    int n, cap;
    int sel, top;                       /* the chosen row (-1 none) and the first row shown */
    int sb_drag;                        /* the scroll bar's thumb is held: the offset inside it, -1 = no */
    uint32_t click_ms;                  /* when the last row was clicked (double clicks) */
    char dir[PATH_MAX];                 /* the folder `ent` was read from ("" = read it again) */
    bool (*want)(const char *name);
};

enum { WLPICKER_NOTHING = 0, WLPICKER_MOVED = 1, WLPICKER_OPEN = 2 };     /* what a click or a key did: nothing / changed the field / a file was taken */

/* A file name of a picture the programs can open (png jpg gif bmp webp svg ppm ... ico): a `want` for the programs that pick pictures. */
static inline bool wlpicker_picture(const char *name)
{
    static const char *const ext[] = { ".png", ".jpg", ".jpeg", ".jpe", ".gif", ".bmp", ".webp", ".svg", ".ppm", ".pgm", ".pbm", ".pnm", ".tif", ".tiff", ".tga", ".ico" };
    const char *dot = strrchr(name, '.');
    for (size_t k = 0; dot && k < sizeof(ext) / sizeof(ext[0]); k++)
        if (!strcasecmp(dot, ext[k]))
            return true;
    return false;
}

static inline void wlpicker_init(struct wlpicker *p, bool (*want)(const char *name))
{
    memset(p, 0, sizeof(*p));
    p->sel = -1;
    p->sb_drag = -1;
    p->want = want;
}

static inline void wlpicker_reset(struct wlpicker *p)
{
    p->sel = -1;
    p->top = 0;
    p->sb_drag = -1;
    p->dir[0] = 0;
}

static inline void wlpicker_free(struct wlpicker *p)
{
    free(p->ent);
    p->ent = NULL;
    p->n = p->cap = 0;
}

static inline void wlpicker_typed(struct wlpicker *p)
{
    p->sel = -1;
    p->top = 0;
}

static inline int wlpicker_fit(int window_h, int other_h)
{
    int r = (window_h - 16 - other_h) / WLPICKER_ROW_H;
    if (r > WLPICKER_MAX_ROWS)
        r = WLPICKER_MAX_ROWS;
    return r >= 3 ? r : 0;
}

/* The folder part of the field ("~/a/" of "~/a/b.png") and the rest, which filters the list. */
static inline void wlpicker_split(const char *field, char *dirtext, size_t n, const char **filter)
{
    const char *sl = strrchr(field, '/');
    size_t len = sl ? (size_t)(sl - field) + 1 : 0;
    snprintf(dirtext, n, "%.*s", (int)(len < n - 1 ? len : n - 1), field);
    *filter = field + len;
}

static inline int wlpicker_cmp(const void *x, const void *y)
{
    const struct wlpicker_ent *a = x, *b = y;
    if (a->dir != b->dir)
        return a->dir ? -1 : 1;
    return strcasecmp(a->name, b->name);
}

static inline void wlpicker_load(struct wlpicker *p, const char *base, const char *dirtext)
{
    char full[PATH_MAX];
    lm_resolve_path(base, dirtext[0] ? dirtext : ".", full, sizeof(full));
    if (!strcmp(full, p->dir) && p->ent)
        return;
    snprintf(p->dir, sizeof(p->dir), "%s", full);
    p->n = 0;
    DIR *d = opendir(full);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && p->n < WLPICKER_MAX_ENTRIES) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strlen(e->d_name) >= sizeof(p->ent[0].name))
            continue;
        char path[PATH_MAX + 260];
        struct stat st;
        snprintf(path, sizeof(path), "%.*s/%s", (int)(strlen(full) < PATH_MAX - 1 ? strlen(full) : PATH_MAX - 1), full, e->d_name);
        bool dir = e->d_type == DT_DIR || (e->d_type == DT_LNK && stat(path, &st) == 0 && S_ISDIR(st.st_mode));
        if (!dir && p->want && !p->want(e->d_name))
            continue;
        if (p->n == p->cap) {
            int nc = p->cap ? p->cap * 2 : 128;
            struct wlpicker_ent *ne = realloc(p->ent, (size_t)nc * sizeof(p->ent[0]));
            if (!ne)
                break;
            p->ent = ne;
            p->cap = nc;
        }
        struct wlpicker_ent *c = &p->ent[p->n++];
        snprintf(c->name, sizeof(c->name), "%s", e->d_name);
        c->dir = dir;
    }
    closedir(d);
    if (p->n > 1)
        qsort(p->ent, (size_t)p->n, sizeof(p->ent[0]), wlpicker_cmp);
}

/* The rows that show: ".." first (not at the root, nor while a name is being typed), then what starts with the typed name. idx[k] = -1 is "..". */
static inline int wlpicker_list(struct wlpicker *p, const char *base, const char *field, int *idx, int max)
{
    char dirtext[PATH_MAX];
    const char *filter;
    wlpicker_split(field, dirtext, sizeof(dirtext), &filter);
    wlpicker_load(p, base, dirtext);
    int n = 0;
    size_t fl = strlen(filter);
    for (int i = 0; i < p->n && fl; i++)                                        /* a name typed out in full (a file just picked): the whole folder shows */
        if (!strcmp(p->ent[i].name, filter))
            fl = 0;
    if (strcmp(p->dir, "/") && n < max && fl == 0)
        idx[n++] = -1;
    for (int i = 0; i < p->n && n < max; i++) {
        if (p->ent[i].name[0] == '.' && filter[0] != '.')
            continue;
        if (fl && strncasecmp(p->ent[i].name, filter, fl))
            continue;
        idx[n++] = i;
    }
    return n;
}

/* The field with the entry's name in place of the part after the last slash; a folder gets its slash. idx -1 = "..". */
static inline void wlpicker_put(struct wlpicker *p, char *field, size_t fsz, int idx)
{
    char dirtext[PATH_MAX], next[PATH_MAX + 260];
    const char *filter;
    wlpicker_split(field, dirtext, sizeof(dirtext), &filter);
    if (idx < 0) {                                                              /* ".." */
        size_t n = strlen(dirtext);
        if (n == 0)
            snprintf(next, sizeof(next), "../");
        else {
            const char *last = n >= 2 ? (const char *)memrchr(dirtext, '/', n - 1) : NULL;
            const char *comp = last ? last + 1 : dirtext;
            if (!strncmp(comp, "../", 3) || !strcmp(dirtext, "/") || !strcmp(dirtext, "~/"))
                snprintf(next, sizeof(next), "%s../", dirtext);
            else
                snprintf(next, sizeof(next), "%.*s", (int)(last ? last - dirtext + 1 : 0), dirtext);
        }
    } else {
        snprintf(next, sizeof(next), "%s%s%s", dirtext, p->ent[idx].name, p->ent[idx].dir ? "/" : "");
    }
    snprintf(field, fsz, "%.*s", (int)fsz - 1, next);
    wltext_end(field);
    p->sel = -1;
    p->top = 0;
}

static inline int wlpicker_choose(struct wlpicker *p, const char *base, char *field, size_t fsz, int row)
{
    int idx[WLPICKER_MAX_ENTRIES + 1];
    int n = wlpicker_list(p, base, field, idx, WLPICKER_MAX_ENTRIES);
    if (row < 0 || row >= n)
        return WLPICKER_NOTHING;
    int e = idx[row];
    bool file = e >= 0 && !p->ent[e].dir;
    wlpicker_put(p, field, fsz, e);
    return file ? WLPICKER_OPEN : WLPICKER_MOVED;
}

/* Up / Down / PgUp / PgDn move the chosen row, Enter on a chosen row takes it (WLPICKER_OPEN: a file is in the field, the program accepts it). */
static inline int wlpicker_key(struct wlpicker *p, const char *base, char *field, size_t fsz, int rows, uint32_t ks)
{
    if (rows <= 0)
        return WLPICKER_NOTHING;
    if (ks == LM_KS_Up || ks == LM_KS_Down || ks == LM_KS_Page_Up || ks == LM_KS_Page_Down) {
        int idx[WLPICKER_MAX_ENTRIES + 1];
        int n = wlpicker_list(p, base, field, idx, WLPICKER_MAX_ENTRIES);
        int delta = ks == LM_KS_Up ? -1 : ks == LM_KS_Down ? 1 : ks == LM_KS_Page_Up ? -8 : 8;
        if (n == 0)
            return WLPICKER_MOVED;
        p->sel = p->sel < 0 ? (delta > 0 ? 0 : n - 1) : p->sel + delta;
        p->sel = p->sel < 0 ? 0 : p->sel > n - 1 ? n - 1 : p->sel;
        if (p->sel < p->top)
            p->top = p->sel;
        if (p->sel >= p->top + rows)
            p->top = p->sel - rows + 1;
        return WLPICKER_MOVED;
    }
    if ((ks == LM_KS_Return || ks == LM_KS_KP_Enter) && p->sel >= 0)
        return wlpicker_choose(p, base, field, fsz, p->sel);
    return WLPICKER_NOTHING;
}

/* ---- painting ---- */

static inline void wlpicker_paint(struct wlwin *w, struct wlpicker *p, const char *base, const char *field, int x, int y, int wd, int rows,
                                  double px, double py, bool has_pointer)
{
    int idx[WLPICKER_MAX_ENTRIES + 1];
    int n = wlpicker_list(p, base, field, idx, WLPICKER_MAX_ENTRIES);
    p->top = p->top < 0 ? 0 : p->top > n - rows ? (n - rows > 0 ? n - rows : 0) : p->top;
    wlwin_round_fill(w, x, y, wd, rows * WLPICKER_ROW_H + 8, 10, lm_theme.background);
    if (n == 0)
        wlwin_text(w, x + 12, y + 8, "Nothing here starts like that", 1, lm_theme.text_dim, wd - 24);
    const char *tail = strrchr(field, '/');
    for (int r = 0; r < rows && p->top + r < n; r++) {
        int k = p->top + r, e = idx[k], ry = y + 4 + r * WLPICKER_ROW_H;
        bool dir = e < 0 || p->ent[e].dir;
        bool over = has_pointer && px >= x && px < x + wd - 10 && py >= ry && py < ry + WLPICKER_ROW_H;
        bool named = e >= 0 && !strcmp(p->ent[e].name, tail ? tail + 1 : field);            /* the file in the field */
        if (k == p->sel || named || over)
            wlwin_round_fill(w, x + 4, ry, wd - 8 - (n > rows ? lm_metrics.scrollbar_width : 0), WLPICKER_ROW_H, 11,
                             k == p->sel || named ? lm_theme.button_hover : lm_theme.panel_line);
        int ix = x + 14, iy = ry + 4;
        if (dir) {                                                              /* a folder: a tab and a body */
            wlwin_fill(w, ix, iy + 1, 6, 3, lm_theme.btn_amber);
            wlwin_round_fill(w, ix, iy + 3, 14, 10, 3, lm_theme.btn_amber_lit);
        } else {
            wlwin_round_fill(w, ix + 2, iy, 10, 14, 2, lm_theme.btn_sky);
            wlwin_fill(w, ix + 4, iy + 5, 6, 1, lm_theme.bar);
            wlwin_fill(w, ix + 4, iy + 8, 6, 1, lm_theme.bar);
        }
        wlwin_text(w, ix + 24, ry + (WLPICKER_ROW_H - LM_FONT_H) / 2, e < 0 ? ".." : p->ent[e].name, 1, dir ? lm_theme.accent : lm_theme.text_soft, wd - 60);
    }
    if (n > rows) {                                                             /* a thin scroll bar */
        int sbw = lm_metrics.scrollbar_width, th = (rows * WLPICKER_ROW_H) * rows / n;
        th = th < 20 ? 20 : th;
        int ty = y + 4 + (rows * WLPICKER_ROW_H - th) * p->top / (n - rows > 1 ? n - rows : 1);
        wlwin_round_fill(w, x + wd - sbw - 2, ty, sbw - 4, th, (sbw - 4) / 2, lm_theme.control);
    }
}

/* ---- the pointer ---- */

static inline bool wlpicker_inside(int x, int y, int wd, int rows, double px, double py)
{
    return px >= x && px < x + wd && py >= y + 4 && py < y + 4 + rows * WLPICKER_ROW_H;
}

/* A left button press at (px, py); `time_ms` and `dbl_ms` decide a double click. */
static inline int wlpicker_press(struct wlpicker *p, const char *base, char *field, size_t fsz, int x, int y, int wd, int rows, double px, double py,
                                 uint32_t time_ms, uint32_t dbl_ms)
{
    if (rows <= 0 || !wlpicker_inside(x, y, wd, rows, px, py))
        return WLPICKER_NOTHING;
    int idx[WLPICKER_MAX_ENTRIES + 1];
    int n = wlpicker_list(p, base, field, idx, WLPICKER_MAX_ENTRIES), row = p->top + ((int)py - y - 4) / WLPICKER_ROW_H;
    int sbw = lm_metrics.scrollbar_width;
    if (n > rows && px >= x + wd - sbw - 4) {                                   /* the scroll bar: the thumb is dragged, the track pages (it is no row) */
        int th = (rows * WLPICKER_ROW_H) * rows / n;
        th = th < 20 ? 20 : th;
        int ty = y + 4 + (rows * WLPICKER_ROW_H - th) * p->top / (n - rows > 1 ? n - rows : 1);
        if (py >= ty && py < ty + th)
            p->sb_drag = (int)py - ty;
        else {
            p->top += py < ty ? -rows : rows;
            p->top = p->top < 0 ? 0 : p->top > n - rows ? n - rows : p->top;
        }
        return WLPICKER_MOVED;
    }
    if (row >= n)
        return WLPICKER_NOTHING;
    bool dbl = time_ms - p->click_ms < dbl_ms;
    p->click_ms = time_ms;
    bool file = idx[row] >= 0 && !p->ent[idx[row]].dir, was = p->sel == row;
    wlpicker_put(p, field, fsz, idx[row]);
    if (file) {
        p->sel = row;                                                           /* (the list stays: the field has the whole name now) */
        if (dbl && was)
            return WLPICKER_OPEN;
    }
    return WLPICKER_MOVED;
}

/* The pointer moved: a held thumb follows it. True when something changed. */
static inline bool wlpicker_motion(struct wlpicker *p, const char *base, const char *field, int y, int rows, double py)
{
    if (p->sb_drag < 0 || rows <= 0)
        return false;
    int idx[WLPICKER_MAX_ENTRIES + 1];
    int n = wlpicker_list(p, base, field, idx, WLPICKER_MAX_ENTRIES), th = (rows * WLPICKER_ROW_H) * rows / (n > 1 ? n : 1);
    th = th < 20 ? 20 : th;
    if (n > rows) {
        int span = rows * WLPICKER_ROW_H - th;
        p->top = (int)((py - y - 4 - p->sb_drag) * (n - rows) / (span > 1 ? span : 1));
        p->top = p->top < 0 ? 0 : p->top > n - rows ? n - rows : p->top;
    }
    return true;
}

static inline void wlpicker_wheel(struct wlpicker *p, int dir)
{
    p->top += dir > 0 ? 3 : -3;
    if (p->top < 0)
        p->top = 0;
}

static inline void wlpicker_release(struct wlpicker *p)
{
    p->sb_drag = -1;
}

/* The `clist` and `crow` lines of a test dump; `xoff` is added to x (a program with a rail counts its list from the rail's edge). */
static inline void wlpicker_dump(FILE *f, struct wlpicker *p, const char *base, const char *field, int x, int y, int wd, int rows, int xoff)
{
    int idx[WLPICKER_MAX_ENTRIES + 1];
    int n = wlpicker_list(p, base, field, idx, WLPICKER_MAX_ENTRIES);
    fprintf(f, "clist %d %d %d %d csel=%d count=%d\n", x + xoff, y + 4, wd, rows * WLPICKER_ROW_H, p->sel, n);
    for (int k = p->top; k < n && k < p->top + rows; k++)
        fprintf(f, "crow %d y=%d dir=%d name=%s\n", k, y + 4 + (k - p->top) * WLPICKER_ROW_H, idx[k] < 0 || p->ent[idx[k]].dir, idx[k] < 0 ? ".." : p->ent[idx[k]].name);
}

#endif
