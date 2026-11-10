/*
 * mellagui-run: "Run command" dialog (wl_shm + xdg-shell, bitmap font).
 * Type a shell command and press Enter: it is started detached (own session, stdio on /dev/null)
 * and the dialog closes. Esc closes without running.
 *
 *   Left/Right/Home/End, Backspace/Delete  edit        Ctrl+U  clear the line   Ctrl+A / Ctrl+E  home / end
 *   Up/Down                                 history (kept in $XDG_CACHE_HOME/mellagui-run-history or ~/.cache/...)
 *
 * `mellagui-run --selftest` checks the line editor without a display.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "theme.h"
#include "font.h"
#include "keymap.h"
#include "util.h"
#include "wlclip.h"
#include "wltext.h"
#include "wlwin.h"

/* ---- line editor ----------------------------------------------------------------------- */

#define LINE_MAX_BYTES 1000
#define HIST_MAX 50

struct line {
    char text[LINE_MAX_BYTES + 1];
    size_t len, cur;            /* bytes; cur is on a UTF-8 boundary */
};

static bool is_cont(unsigned char c)
{
    return (c & 0xc0) == 0x80;
}

static size_t prev_boundary(const struct line *l, size_t i)
{
    if (i == 0)
        return 0;
    i--;
    while (i > 0 && is_cont((unsigned char)l->text[i]))
        i--;
    return i;
}

static size_t next_boundary(const struct line *l, size_t i)
{
    if (i >= l->len)
        return l->len;
    i++;
    while (i < l->len && is_cont((unsigned char)l->text[i]))
        i++;
    return i;
}

static void line_set(struct line *l, const char *s)
{
    snprintf(l->text, sizeof(l->text), "%s", s);
    l->len = strlen(l->text);
    l->cur = l->len;
    wltext_end(l->text);                                    /* the field's caret goes with it */
}

static void line_insert(struct line *l, const char *s, size_t n)
{
    if (l->len + n > LINE_MAX_BYTES)
        return;
    memmove(l->text + l->cur + n, l->text + l->cur, l->len - l->cur + 1);
    memcpy(l->text + l->cur, s, n);
    l->len += n;
    l->cur += n;
}

static void line_backspace(struct line *l)
{
    size_t p = prev_boundary(l, l->cur);
    memmove(l->text + p, l->text + l->cur, l->len - l->cur + 1);
    l->len -= l->cur - p;
    l->cur = p;
}

static void line_delete(struct line *l)
{
    size_t n = next_boundary(l, l->cur);
    memmove(l->text + l->cur, l->text + n, l->len - n + 1);
    l->len -= n - l->cur;
}

/* ---- history ---------------------------------------------------------------------------- */

struct hist {
    char *items[HIST_MAX];      /* oldest first */
    int n;
    int pos;                    /* browsing index, n = the line being edited */
    char draft[LINE_MAX_BYTES + 1];
};

static void hist_add(struct hist *h, const char *s)
{
    if (!*s || (h->n && !strcmp(h->items[h->n - 1], s)))
        return;
    if (h->n == HIST_MAX) {
        free(h->items[0]);
        memmove(h->items, h->items + 1, sizeof(h->items[0]) * (HIST_MAX - 1));
        h->n--;
    }
    h->items[h->n++] = strdup(s);
}

static void hist_free(struct hist *h)
{
    for (int i = 0; i < h->n; i++)
        free(h->items[i]);
    h->n = 0;
}

static char *hist_path(void)
{
    const char *x = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    static char path[1024];
    if (x && *x)
        snprintf(path, sizeof(path), "%s/mellagui-run-history", x);
    else if (home && *home)
        snprintf(path, sizeof(path), "%s/.cache/mellagui-run-history", home);
    else
        return NULL;
    return path;
}

static void hist_load(struct hist *h)
{
    char *path = hist_path();
    FILE *f = path ? fopen(path, "r") : NULL;
    char buf[LINE_MAX_BYTES + 2];
    while (f && fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\n")] = 0;
        hist_add(h, buf);
    }
    if (f)
        fclose(f);
    h->pos = h->n;
}

static void hist_save(const struct hist *h)
{
    char *path = hist_path();
    if (!path)
        return;
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        mkdir(dir, 0700);       /* may already exist */
    }
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (int i = 0; i < h->n; i++)
        fprintf(f, "%s\n", h->items[i]);
    fclose(f);
}

static void hist_move(struct hist *h, struct line *l, int dir)
{
    int np = h->pos + dir;
    if (np < 0 || np > h->n)
        return;
    if (h->pos == h->n)
        snprintf(h->draft, sizeof(h->draft), "%s", l->text);
    h->pos = np;
    line_set(l, np == h->n ? h->draft : h->items[np]);
}

/* ---- starting the command ------------------------------------------------------------------ */

/* Detached start: fork, setsid, fork again so nothing is left to reap, stdio on /dev/null. */
static int spawn_detached(const char *cmd, int close_fd)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setsid();
        pid_t p2 = fork();
        if (p2 != 0)
            _exit(p2 < 0 ? 1 : 0);
        if (close_fd >= 0)
            close(close_fd);
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
            if (nul > 2)
                close(nul);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int st;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* ---- selftest -------------------------------------------------------------------------------- */

static int selftest(void)
{
    int bad = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "selftest failed: %s (line %d)\n", #c, __LINE__); bad++; } } while (0)
    struct line l = { 0 };
    line_insert(&l, "ls", 2);
    line_insert(&l, " -l", 3);
    CHECK(!strcmp(l.text, "ls -l") && l.cur == 5);
    l.cur = 2;
    line_insert(&l, "x", 1);
    CHECK(!strcmp(l.text, "lsx -l"));
    line_backspace(&l);
    CHECK(!strcmp(l.text, "ls -l") && l.cur == 2);
    line_delete(&l);
    CHECK(!strcmp(l.text, "ls-l"));
    l.cur = 0;
    line_backspace(&l);                         /* nothing before the cursor */
    CHECK(!strcmp(l.text, "ls-l") && l.cur == 0);
    line_set(&l, "t\xc3\xb5u");                 /* t õ u */
    l.cur = 3;
    CHECK(prev_boundary(&l, l.cur) == 1);
    line_backspace(&l);                         /* removes the whole 2-byte õ */
    CHECK(!strcmp(l.text, "tu") && l.cur == 1);
    l.cur = l.len;
    line_delete(&l);                            /* at the end: no-op */
    CHECK(!strcmp(l.text, "tu"));
    for (int i = 0; i < 2000; i++)
        line_insert(&l, "a", 1);                /* bounded */
    CHECK(l.len == LINE_MAX_BYTES);

    struct hist h = { 0 };
    hist_add(&h, "one");
    hist_add(&h, "one");                        /* duplicate of the last entry ignored */
    hist_add(&h, "two");
    hist_add(&h, "");
    CHECK(h.n == 2);
    h.pos = h.n;
    line_set(&l, "draft");
    hist_move(&h, &l, -1);
    CHECK(!strcmp(l.text, "two"));
    hist_move(&h, &l, -1);
    CHECK(!strcmp(l.text, "one"));
    hist_move(&h, &l, -1);                      /* already at the oldest */
    CHECK(!strcmp(l.text, "one"));
    hist_move(&h, &l, 1);
    hist_move(&h, &l, 1);
    CHECK(!strcmp(l.text, "draft"));
    for (int i = 0; i < 3 * HIST_MAX; i++) {
        char b[16];
        snprintf(b, sizeof(b), "c%d", i);
        hist_add(&h, b);
    }
    CHECK(h.n == HIST_MAX);
    hist_free(&h);
    return bad ? 1 : 0;
}

/* ---- window ------------------------------------------------------------------------------------ */

#define DEFAULT_W 520
#define DEFAULT_H 64
#define PAD 10
#define COL_BG      (lm_theme.background)
#define COL_FIELD   (lm_theme.field)
#define COL_ACCENT  (lm_theme.accent)
#define COL_TEXT    (lm_theme.text)
#define COL_DIM     (lm_theme.text_dim)
struct app {
    struct wlwin win;
    struct line line;
    struct hist hist;
    int repeat_rate, repeat_delay;
    uint32_t repeat_code;
    struct timespec repeat_next;
    struct wlclip clip;                 /* the clipboard, the caret and the selection (wltext.h) */
    int fx, fy, fw;                     /* where the text of the field is (set by paint) */
    bool selecting;                     /* the left button is down in the field */
};

static void fill(struct app *a, int x, int y, int w, int h, uint32_t color)
{
    wlwin_fill(&a->win, x, y, w, h, color);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    fill(a, 0, 0, a->win.bw, a->win.bh, COL_BG);
    int label_w = lm_font_text_width("Run:") + 8;
    int fy = (a->win.bh - (LM_FONT_H + 12)) / 2;
    wlwin_text(w, PAD, fy + 6, "Run:", 1, COL_DIM, 0);
    int fx = PAD + label_w, fw = a->win.bw - fx - PAD;
    fill(a, fx, fy, fw, LM_FONT_H + 12, COL_FIELD);
    fill(a, fx, fy + LM_FONT_H + 11, fw, 1, COL_ACCENT);

    a->fx = fx + 6;
    a->fy = fy + 6;
    a->fw = fw - 12;
    wltext_paint(w, a->fx, a->fy, a->fw, a->line.text, true, COL_TEXT, lm_theme.button_hover, COL_ACCENT);
}

/* ---- keyboard ------------------------------------------------------------------------------------ */

static void run_line(struct app *a)
{
    if (!a->line.len) {
        a->win.running = false;
        return;
    }
    hist_add(&a->hist, a->line.text);
    hist_save(&a->hist);
    if (spawn_detached(a->line.text, wlc_fd(a->win.c)) < 0)
        fprintf(stderr, "mellagui-run: cannot start '%s': %s\n", a->line.text, strerror(errno));
    a->win.running = false;
}

/* The line was changed through wltext: keep struct line in step (the history and the selftest use it). */
static void line_synced(struct app *a)
{
    a->line.len = strlen(a->line.text);
    a->line.cur = wltext_cursor(a->line.text);
    a->win.redraw = true;
}

static void handle_key(struct app *a, uint32_t code)
{
    uint32_t ks = lm_keymap_keysym(code, a->win.mods);
    bool ctrl = (a->win.mods & LM_MOD_CTRL) != 0;
    if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
        run_line(a);
    } else if (ks == LM_KS_Escape) {
        a->win.running = false;
    } else if (ctrl && (ks == 'u' || ks == 'U')) {                  /* Ctrl+U: clear the line (readline) */
        line_set(&a->line, "");
    } else if (ctrl && (ks == 'e' || ks == 'E')) {                  /* Ctrl+E: the end of the line (readline) */
        wltext_end(a->line.text);
    } else if (ks == LM_KS_Up) {
        hist_move(&a->hist, &a->line, -1);
    } else if (ks == LM_KS_Down) {
        hist_move(&a->hist, &a->line, 1);
    } else if (wltext_edit(&a->clip, a->line.text, sizeof(a->line.text), ks, a->win.mods)) {
        line_synced(a);
    }
    a->win.redraw = true;
}

static void repeat_stop(struct app *a)
{
    a->repeat_code = 0;
}

static void repeat_arm(struct app *a, uint32_t code)
{
    if (a->repeat_rate <= 0)
        return;
    a->repeat_code = code;
    clock_gettime(CLOCK_MONOTONIC, &a->repeat_next);
    long ns = a->repeat_next.tv_nsec + (a->repeat_delay % 1000) * 1000000L;
    a->repeat_next.tv_sec += a->repeat_delay / 1000 + ns / 1000000000L;
    a->repeat_next.tv_nsec = ns % 1000000000L;
}

/* ms until the next repeat is due, -1 if no key repeats */
static int repeat_timeout(const struct app *a)
{
    if (!a->repeat_code)
        return -1;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (a->repeat_next.tv_sec - now.tv_sec) * 1000L + (a->repeat_next.tv_nsec - now.tv_nsec) / 1000000L;
    return ms < 0 ? 0 : (int)ms;
}

static void repeat_fire(struct app *a)
{
    if (!a->repeat_code || repeat_timeout(a) > 0)
        return;
    handle_key(a, a->repeat_code);
    long iv = 1000000000L / a->repeat_rate;
    long ns = a->repeat_next.tv_nsec + iv;
    a->repeat_next.tv_sec += ns / 1000000000L;
    a->repeat_next.tv_nsec = ns % 1000000000L;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);       /* do not burst after a stall */
    if (now.tv_sec > a->repeat_next.tv_sec ||
        (now.tv_sec == a->repeat_next.tv_sec && now.tv_nsec > a->repeat_next.tv_nsec))
        a->repeat_next = now;
}

/* ---- protocol events ------------------------------------------------------------------------------- */

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    switch (op) {
    case LM_WL_KEYBOARD_EVT_LEAVE:
        repeat_stop(a);
        break;
    case LM_WL_KEYBOARD_EVT_REPEAT_INFO:
        a->repeat_rate = args[0].i;
        a->repeat_delay = args[1].i;
        break;
    case LM_WL_KEYBOARD_EVT_KEY:
        a->clip.serial = args[0].u;
        if (args[3].u == 0) {
            if (a->repeat_code == args[2].u)
                repeat_stop(a);
            break;
        }
        handle_key(a, args[2].u);
        if (a->win.running) {
            uint32_t ks = lm_keymap_keysym(args[2].u, a->win.mods);
            if (ks != LM_KS_Return && ks != LM_KS_Escape)
                repeat_arm(a, args[2].u);
        }
        break;
    }
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    static double px;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        px = lm_fixed_to_double(args[2].f);
        break;
    case LM_WL_POINTER_EVT_MOTION:
        px = lm_fixed_to_double(args[1].f);
        if (a->selecting) {                                          /* drag: the selection follows the pointer */
            wltext_click(a->line.text, (int)px - a->fx, true);
            w->redraw = true;
        }
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        a->clip.serial = args[0].u;
        if (args[2].u == 272) {                                      /* BTN_LEFT: the caret goes where it was clicked; Shift extends the selection */
            if (args[3].u == 1) {
                wltext_click(a->line.text, (int)px - a->fx, (w->mods & LM_MOD_SHIFT) != 0);
                a->selecting = true;
                w->redraw = true;
            } else {
                a->selecting = false;
            }
        } else if (args[2].u == 273 && args[3].u == 1) {            /* BTN_RIGHT: Cut / Copy / Paste / Select all / Clear */
            wlwin_menu(w, wltext_menu(a->line.text));
        }
        break;
    }
}

static void menu_event(struct wlwin *w, int index)
{
    struct app *a = w->data;
    if (index < 0)
        return;
    wltext_menu_do(&a->clip, a->line.text, sizeof(a->line.text), index);
    line_synced(a);
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    struct app *a = w->data;
    wlclip_global(&a->clip, w, name, iface, ver);
}

int main(int argc, char **argv)
{
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();

    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event, .menu = menu_event };
    struct app a = { .repeat_rate = 25, .repeat_delay = 400 };
    int rc = 1;
    hist_load(&a.hist);
    if (wlwin_connect(&a.win, "mellagui-run", &ops, &a) < 0)
        goto out;
    if (!a.win.seat) {
        fprintf(stderr, "mellagui-run: compositor lacks wl_seat\n");
        goto out;
    }
    if (wlwin_open(&a.win, "Run", "mellagui.run", DEFAULT_W, DEFAULT_H, 160, 40) < 0)
        goto out;
    wlclip_attach(&a.clip, &a.win);
    while (wlwin_step(&a.win, repeat_timeout(&a)))
        repeat_fire(&a);
    rc = 0;
out:
    if (a.win.c)
        wlwin_destroy(&a.win);
    hist_free(&a.hist);
    return rc;
}
