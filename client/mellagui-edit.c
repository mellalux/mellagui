/*
 * mellagui-edit - a small text editor for MellaGUI (UTF-8 text, line numbers, selection, clipboard, undo, find).
 *
 *   mellagui-edit [FILE]          open FILE (a new file when it does not exist), else an empty document
 *   mellagui-edit --selftest      the editing model without a display
 *
 * Keys: arrows, Home / End, PageUp / PageDown, Ctrl+Left / Right (words), Ctrl+Home / End; Shift extends the selection;
 * Ctrl+A select all, Ctrl+C / X / V clipboard (Ctrl+Insert, Shift+Delete, Shift+Insert too), Ctrl+Z / Ctrl+Y undo / redo, Ctrl+S save, Ctrl+Shift+S save as, Ctrl+O open,
 * Ctrl+N new, Ctrl+F find (Enter / F3 next), Ctrl+G go to line, Ctrl+Q quit (twice to discard changes), Esc clears the
 * message / selection, F1 help, Ctrl+= / Ctrl+- / Ctrl+0 (or Ctrl+wheel, or the A- / A+ buttons) zoom the text 50..500 % like the
 * terminal and the window grows with it (kept in $XDG_CONFIG_HOME/mellagui/editor.conf). A button bar (New, Open, Save, Save as, Cut, Copy, Paste, Find, A-, A+, Help) does the same with the mouse. The Open
 * and Save as prompts start in the folder of the current file, accept ~/ and relative names, and Tab completes names.
 * Mouse: click, drag, double click (word), wheel. Files up to 8 MB; binary files and text that is not
 * valid UTF-8 are refused (saving them would damage them). CRLF files keep their line ends; a final newline is kept as it was.
 */
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mdview.h"
#include "theme.h"
#include "font.h"
#include "keymap.h"
#include "metrics.h"
#include "pathcomp.h"
#include "protocol.h"
#include "uikit.h"
#include "util.h"
#include "wlcard.h"
#include "wlrail.h"
#include "wlclip.h"
#include "wltext.h"
#include "wlwin.h"

#define MAX_FILE (8u << 20)
#define TAB_W 4
/* A sticky note (`--note FILE`): the same editor on coloured paper with no button bar, no line numbers and no status line; it saves by itself. */
static bool g_note;
/* The buttons are in two rails of round drops, at the left and the right of the text (not in a note); `--bar` (or MELLAGUI_EDIT_RAIL=0) brings back the bar above it. */
static bool g_rail = true;
#define RAIL_W (g_rail && !g_note ? WLRAIL_W : 0)
#define RIGHT_W RAIL_W
static uint32_t g_paper = 0xfff2a8, g_ink = 0x2a2a20;
#define STATUS_H (g_note ? 0 : 20)
#define PBAR(a) ((a)->prompt != P_NONE && !(g_rail && !g_note) ? STATUS_H : 0)     /* the prompt's own strip (the prototype has a card instead) */
#define TOP_H (g_note || g_rail ? 0 : (LM_FONT_H + 8))           /* button bar above the text */
#define UNDO_MAX 64
#define COL_BG (g_note ? g_paper : lm_theme.background)
#define COL_FG (g_note ? g_ink : lm_theme.text_soft)
#define COL_DIM (g_note ? lm_mix_rgb(g_paper, g_ink, 0.45) : lm_theme.text_dim)
#define COL_GUTTER_BG (g_note ? g_paper : lm_theme.gutter)
#define COL_LINE (g_note ? lm_mix_rgb(g_paper, 0x000000, 0.06) : lm_theme.current_line)
#define COL_SEL (g_note ? lm_mix_rgb(g_paper, 0x3a6fb4, 0.35) : lm_theme.button_hover)
#define COL_BAR (g_note ? lm_mix_rgb(g_paper, 0x000000, 0.12) : lm_theme.bar)
#define COL_ACCENT (g_note ? 0xb5651d : lm_theme.accent)
#define COL_WARN (lm_theme.caution)
/* ---- the document ----------------------------------------------------------------------------------------------------- */

struct line {
    char *s;
    size_t len, cap;
};

struct snap {
    char *text;
    size_t len;
    int cy;
    size_t cx;
};

struct app {
    struct wlwin win;
    int64_t edit_ms;                    /* when the text was last changed (a note saves itself a moment after) */
    int paper;                          /* a note's colour: an index of note_papers */
    bool delete_armed;                  /* a note: Ctrl+D was pressed once, a second press within 3 s deletes it */
    int64_t delete_ms;
    struct line *l;                     /* the lines, without line ends */
    int n, cap;
    int cy;                             /* cursor line and byte offset in it */
    size_t cx;
    int ay;                             /* selection anchor; equal to the cursor = no selection */
    size_t ax;
    int want_col;                       /* display column kept while moving up / down */
    int top, left;                      /* first visible line / display column */
    char path[PATH_MAX];
    bool modified, final_nl, crlf;
    /* prompt line: open / save as / find / go to */
    enum { P_NONE, P_OPEN, P_SAVEAS, P_FIND, P_GOTO } prompt;
    char pbuf[512];
    char find[256];
    char msg[200];
    int64_t msg_until;
    bool quit_armed;
    int pfresh;                         /* prompt text is a suggestion: 1 = a folder (typing "/" or "~" replaces it), 2 = a file name (any typing replaces it) */
    bool ow_armed;                      /* "file exists" was shown, the next Enter overwrites */
    bool help;                          /* the key list is on screen */
    int hover;                          /* button under the pointer, -1 = none */
    /* the rail's buttons are drops of water: springs for the swelling / squashing, and a ripple after a press */
    struct wlrail_anim ran[2];          /* the springs of the left and the right rail */
    /* the card of the prototype (Open / Save as / Find / Go to line): the folder's entries, the highlighted row, how far the list is scrolled */
    struct centry { char name[256]; bool dir; } *cent;
    int cn, ccap, csel, ctop, cbtn_down;
    char cdir[PATH_MAX];
    uint32_t cclick_ms;
    int zoom;                           /* text size in percent (LM_ZOOM_LEVELS); the button bar, status line and prompts keep their size */
    struct lm_zoomtab zt;               /* the cell size and font weights for it */
    bool conf_dirty;                    /* the zoom changed and is not in editor.conf yet */
    int64_t conf_time;
    struct snap undo[UNDO_MAX], redo[UNDO_MAX];
    int nundo, nredo;
    bool typing_run;                    /* the last change was typing: the next typed character joins its undo step */
    struct wlclip clip;                 /* the clipboard (wl_data_device) */
    int menu_kind;                      /* the right-click menu that is showing: 0 none, 1 the prompt's field, 2 the text */
    /* key repeat */
    uint32_t rep_code;
    int64_t rep_next;
    int rep_rate, rep_delay;
    /* mouse */
    double px, py;
    bool dragging;
    uint32_t last_click_ms;
    int rows_visible, cols_visible;
    bool sb_drag;                       /* the scroll bar thumb is being dragged */
    int sb_grab;                        /* pixels between the top of the thumb and the pointer */
    int seen_cy, seen_n;                /* where the cursor was when the view last followed it (a wheel / scroll bar move must stay) */
    size_t seen_cx;
    int seen_rows, seen_cols;
    /* Markdown preview (a .md file opens as a page; Ctrl+M switches to the text and back) */
    bool preview;
    struct md_doc *md;
    int md_scroll;                      /* pixels scrolled down */
    const char *md_hover;               /* the address of the link under the pointer */
    bool md_sb_drag;
    int md_sb_grab;
    bool md_selecting;                  /* the left button is down on the page: the pointer extends the selection */
    bool md_press_link;                 /* ... and it went down on a link (opened on release when nothing was selected) */
    uint32_t md_click_ms;               /* the time of the last press on the page, and how many in a row (double click: a word, triple: a line) */
    int md_clicks;
};

static void preview_auto(struct app *a);
static void preview_set(struct app *a, bool on);
static bool is_md_path(const char *p);

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void line_reserve(struct line *l, size_t need)
{
    if (need + 1 <= l->cap)
        return;
    size_t nc = (need + 1) * 2 + 16;
    char *t = realloc(l->s, nc);
    if (!t)
        abort();
    l->s = t;
    l->cap = nc;
}

static void line_set(struct line *l, const char *s, size_t n)
{
    line_reserve(l, n);
    memcpy(l->s, s, n);
    l->s[n] = 0;
    l->len = n;
}

static void doc_reserve(struct app *a, int n)
{
    if (n <= a->cap)
        return;
    int nc = LM_MAX(n * 2, 16);
    struct line *t = realloc(a->l, (size_t)nc * sizeof(*a->l));
    if (!t)
        abort();
    memset(t + a->cap, 0, (size_t)(nc - a->cap) * sizeof(*t));
    a->l = t;
    a->cap = nc;
}

static void doc_clear(struct app *a)
{
    for (int i = 0; i < a->cap; i++)
        free(a->l[i].s);
    free(a->l);
    a->l = NULL;
    a->n = a->cap = 0;
    doc_reserve(a, 1);
    a->n = 1;
    line_set(&a->l[0], "", 0);
    a->cy = a->ay = 0;
    a->cx = a->ax = 0;
    a->top = a->left = 0;
    a->want_col = 0;
}

/* UTF-8 steps within a line */
static size_t cp_next(const struct line *l, size_t off)
{
    if (off >= l->len)
        return l->len;
    off++;
    while (off < l->len && ((unsigned char)l->s[off] & 0xc0) == 0x80)
        off++;
    return off;
}

static size_t cp_prev(const struct line *l, size_t off)
{
    if (off == 0)
        return 0;
    off--;
    while (off > 0 && ((unsigned char)l->s[off] & 0xc0) == 0x80)
        off--;
    return off;
}

/* Display column of byte offset `off` (tabs go to the next multiple of TAB_W) and the other way round. */
static int col_of(const struct line *l, size_t off)
{
    int col = 0;
    for (size_t i = 0; i < off && i < l->len; i = cp_next(l, i))
        col = l->s[i] == '\t' ? (col / TAB_W + 1) * TAB_W : col + 1;
    return col;
}

static size_t off_at_col(const struct line *l, int col)
{
    int c = 0;
    size_t i = 0;
    while (i < l->len) {
        int nc = l->s[i] == '\t' ? (c / TAB_W + 1) * TAB_W : c + 1;
        if (nc > col && !(l->s[i] == '\t' && col - c >= (nc - c + 1) / 2))   /* a click on the right half of a tab goes after it */
            break;
        c = nc;
        i = cp_next(l, i);
        if (c >= col)
            break;
    }
    return i;
}

static bool is_word(unsigned char c)
{
    return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/* ---- selection, text of a range ---------------------------------------------------------------------------------------- */

static bool has_sel(const struct app *a)
{
    return a->cy != a->ay || a->cx != a->ax;
}

static void sel_order(const struct app *a, int *y0, size_t *x0, int *y1, size_t *x1)
{
    bool fwd = a->ay < a->cy || (a->ay == a->cy && a->ax <= a->cx);
    *y0 = fwd ? a->ay : a->cy;
    *x0 = fwd ? a->ax : a->cx;
    *y1 = fwd ? a->cy : a->ay;
    *x1 = fwd ? a->cx : a->ax;
}

/* The text between two positions with \n between lines (malloc'd, NUL terminated). */
static char *range_text(const struct app *a, int y0, size_t x0, int y1, size_t x1, size_t *len_out)
{
    size_t cap = 64, len = 0;
    char *out = malloc(cap);
    if (!out)
        abort();
    for (int y = y0; y <= y1; y++) {
        size_t from = y == y0 ? x0 : 0, to = y == y1 ? x1 : a->l[y].len;
        size_t need = len + (to - from) + 2;
        if (need > cap) {
            cap = need * 2;
            out = realloc(out, cap);
            if (!out)
                abort();
        }
        memcpy(out + len, a->l[y].s + from, to - from);
        len += to - from;
        if (y < y1)
            out[len++] = '\n';
    }
    out[len] = 0;
    if (len_out)
        *len_out = len;
    return out;
}

/* ---- undo ------------------------------------------------------------------------------------------------------------- */

static void snap_free(struct snap *s)
{
    free(s->text);
    s->text = NULL;
}

static void snap_take(const struct app *a, struct snap *s)
{
    s->text = range_text(a, 0, 0, a->n - 1, a->l[a->n - 1].len, &s->len);
    s->cy = a->cy;
    s->cx = a->cx;
}

static void doc_set_text(struct app *a, const char *t, size_t n);

static void snap_restore(struct app *a, const struct snap *s)
{
    doc_set_text(a, s->text, s->len);
    a->cy = LM_MIN(s->cy, a->n - 1);
    a->cx = LM_MIN(s->cx, a->l[a->cy].len);
    a->ay = a->cy;
    a->ax = a->cx;
}

static void stack_push(struct snap *st, int *n, const struct snap *s)
{
    if (*n == UNDO_MAX) {
        snap_free(&st[0]);
        memmove(st, st + 1, sizeof(st[0]) * (UNDO_MAX - 1));
        (*n)--;
    }
    st[(*n)++] = *s;
}

/* Call before a change. `typing`: a typed character, which shares the step of the characters typed just before it. */
static void before_edit(struct app *a, bool typing)
{
    if (typing && a->typing_run && a->nundo > 0) {
        a->typing_run = true;
    } else {
        struct snap s;
        snap_take(a, &s);
        stack_push(a->undo, &a->nundo, &s);
        a->typing_run = typing;
    }
    for (int i = 0; i < a->nredo; i++)
        snap_free(&a->redo[i]);
    a->nredo = 0;
    a->modified = true;
    a->edit_ms = now_ms();
    a->quit_armed = false;
}

static bool undo(struct app *a, bool redo)
{
    struct snap *from = redo ? a->redo : a->undo, *to = redo ? a->undo : a->redo;
    int *nf = redo ? &a->nredo : &a->nundo, *nt = redo ? &a->nundo : &a->nredo;
    if (*nf == 0)
        return false;
    struct snap now;
    snap_take(a, &now);
    stack_push(to, nt, &now);
    struct snap s = from[--*nf];
    snap_restore(a, &s);
    snap_free(&s);
    a->typing_run = false;
    a->modified = true;
    a->edit_ms = now_ms();
    return true;
}

/* ---- editing ------------------------------------------------------------------------------------------------------------ */

/* Inserts text (with \n line ends; \r dropped) at the cursor and leaves the cursor after it. */
static void insert_at_cursor(struct app *a, const char *t, size_t n)
{
    const char *p = t, *end = t + n;
    while (p <= end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *stop = nl ? nl : end;
        size_t seg = (size_t)(stop - p);
        struct line *l = &a->l[a->cy];
        line_reserve(l, l->len + seg);
        memmove(l->s + a->cx + seg, l->s + a->cx, l->len - a->cx + 1);
        memcpy(l->s + a->cx, p, seg);
        l->len += seg;
        a->cx += seg;
        if (!nl)
            break;
        doc_reserve(a, a->n + 1);                                /* split the line at the cursor */
        memmove(&a->l[a->cy + 2], &a->l[a->cy + 1], (size_t)(a->n - a->cy - 1) * sizeof(a->l[0]));
        memset(&a->l[a->cy + 1], 0, sizeof(a->l[0]));
        a->n++;
        struct line *cur = &a->l[a->cy], *nx = &a->l[a->cy + 1];
        line_set(nx, cur->s + a->cx, cur->len - a->cx);
        cur->len = a->cx;
        cur->s[cur->len] = 0;
        a->cy++;
        a->cx = 0;
        p = nl + 1;
    }
    a->ay = a->cy;
    a->ax = a->cx;
}

static void delete_range(struct app *a, int y0, size_t x0, int y1, size_t x1)
{
    struct line *f = &a->l[y0], *t = &a->l[y1];
    size_t tail = t->len - x1;
    line_reserve(f, x0 + tail);
    memmove(f->s + x0, t->s + x1, tail);
    f->len = x0 + tail;
    f->s[f->len] = 0;
    for (int y = y0 + 1; y <= y1; y++)
        free(a->l[y].s);
    memmove(&a->l[y0 + 1], &a->l[y1 + 1], (size_t)(a->n - y1 - 1) * sizeof(a->l[0]));
    memset(&a->l[a->n - (y1 - y0)], 0, (size_t)(y1 - y0) * sizeof(a->l[0]));
    a->n -= y1 - y0;
    a->cy = a->ay = y0;
    a->cx = a->ax = x0;
}

static void delete_selection(struct app *a)
{
    int y0, y1;
    size_t x0, x1;
    sel_order(a, &y0, &x0, &y1, &x1);
    delete_range(a, y0, x0, y1, x1);
}

/* Replaces the whole document by `t` (the line ends are \n). */
static void doc_set_text(struct app *a, const char *t, size_t n)
{
    doc_clear(a);
    insert_at_cursor(a, t, n);
    a->cy = a->ay = 0;
    a->cx = a->ax = 0;
}

static void move_to(struct app *a, int y, size_t x, bool extend)
{
    a->cy = LM_CLAMP(y, 0, a->n - 1);
    a->cx = LM_MIN(x, a->l[a->cy].len);
    if (!extend) {
        a->ay = a->cy;
        a->ax = a->cx;
    }
    a->typing_run = false;
}

static void set_want(struct app *a)
{
    a->want_col = col_of(&a->l[a->cy], a->cx);
}

/* ---- files -------------------------------------------------------------------------------------------------------------- */

static void msg(struct app *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void msg(struct app *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->msg, sizeof(a->msg), fmt, ap);
    va_end(ap);
    a->msg_until = now_ms() + 6000;
    a->win.redraw = true;
}

/* Well formed UTF-8: no truncated or overlong sequences, no surrogates, nothing above U+10FFFF. */
static bool valid_utf8(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        int k = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (k < 0 || i + (size_t)k >= n)
            return false;
        uint32_t cp = c & (0x3f >> k);
        for (int j = 1; j <= k; j++) {
            if ((s[i + (size_t)j] & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (s[i + (size_t)j] & 0x3f);
        }
        if ((k == 1 && cp < 0x80) || (k == 2 && cp < 0x800) || (k == 3 && cp < 0x10000) || (cp >= 0xd800 && cp <= 0xdfff) ||
            cp > 0x10ffff)
            return false;
        i += (size_t)k + 1;
    }
    return true;
}

/* Reads `path` into the document. 0, or -1 with a reason in err (the document is untouched then). */
static int load_file(struct app *a, const char *path, char *err, size_t errsz)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errsz, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size < 0 || (unsigned long)size > MAX_FILE) {
        fclose(f);
        snprintf(err, errsz, "%s is too big (8 MB at most)", path);
        return -1;
    }
    unsigned char *buf = malloc((size_t)size + 1);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(buf);
        snprintf(err, errsz, "cannot read %s", path);
        return -1;
    }
    fclose(f);
    if (memchr(buf, 0, (size_t)size)) {
        free(buf);
        snprintf(err, errsz, "%s looks like a binary file", path);
        return -1;
    }
    if (!valid_utf8(buf, (size_t)size)) {
        free(buf);
        snprintf(err, errsz, "%s is not valid UTF-8 text", path);
        return -1;
    }
    bool crlf = false;
    size_t n = 0;
    for (size_t i = 0; i < (size_t)size; i++) {                         /* CRLF -> LF, remembered */
        if (buf[i] == '\r' && i + 1 < (size_t)size && buf[i + 1] == '\n') {
            crlf = true;
            continue;
        }
        buf[n++] = buf[i];
    }
    bool final_nl = n > 0 && buf[n - 1] == '\n';
    doc_set_text(a, (const char *)buf, final_nl ? n - 1 : n);
    free(buf);
    a->crlf = crlf;
    a->final_nl = final_nl || n == 0;
    return 0;
}

/* Writes the document to `path` (a temporary file next to it, then rename). 0, or -1 with err. */
static int save_file(struct app *a, const char *path, char *err, size_t errsz)
{
    char tmp[PATH_MAX + 32];
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    struct stat st;
    mode_t mode = stat(path, &st) == 0 ? st.st_mode & 0777 : 0644;
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        snprintf(err, errsz, "cannot write %s: %s", path, strerror(errno));
        return -1;
    }
    bool ok = true;
    for (int y = 0; y < a->n && ok; y++) {
        ok = fwrite(a->l[y].s, 1, a->l[y].len, f) == a->l[y].len;
        if (y < a->n - 1 || a->final_nl)
            ok = ok && fputs(a->crlf ? "\r\n" : "\n", f) >= 0;
    }
    ok = (fclose(f) == 0) && ok;
    if (!ok || chmod(tmp, mode) < 0 || rename(tmp, path) < 0) {
        snprintf(err, errsz, "cannot save %s: %s", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static void set_title(struct app *a)
{
    if (!a->win.toplevel)
        return;
    const char *base = strrchr(a->path, '/') ? strrchr(a->path, '/') + 1 : a->path;
    char t[PATH_MAX + 32];
    if (g_note) {                                                       /* a note is named by its first line */
        char first[48] = "";
        for (int i = 0; i < a->n && !first[0]; i++) {
            size_t k = 0, j = 0;
            while (j < a->l[i].len && a->l[i].s[j] == ' ')
                j++;
            for (; j < a->l[i].len && k < sizeof(first) - 1; j++)
                first[k++] = a->l[i].s[j];
            first[k] = 0;
        }
        snprintf(t, sizeof(t), "%s - Note", first[0] ? first : "New note");
        WLC_SEND(a->win.c, a->win.toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = t });
        return;
    }
    snprintf(t, sizeof(t), "%s%s - %s", a->modified ? "* " : "", a->path[0] ? base : "untitled", a->preview ? "Preview" : "Editor");
    WLC_SEND(a->win.c, a->win.toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = t });
}

static bool do_save(struct app *a, const char *path)
{
    char err[200];
    if (save_file(a, path, err, sizeof(err)) < 0) {
        msg(a, "%s", err);
        return false;
    }
    if (path != a->path)                                                /* saving to the current name passes a->path itself */
        snprintf(a->path, sizeof(a->path), "%.*s", (int)sizeof(a->path) - 1, path);
    a->modified = false;
    a->quit_armed = false;
    msg(a, "Saved %s", a->path);
    return true;
}

static void do_open(struct app *a, const char *path)
{
    char err[200];
    if (load_file(a, path, err, sizeof(err)) < 0) {
        if (errno == ENOENT && access(path, F_OK) != 0) {              /* a name that does not exist yet: a new file */
            doc_clear(a);
            a->final_nl = true;
            a->crlf = false;
            snprintf(a->path, sizeof(a->path), "%s", path);
            a->modified = false;
            msg(a, "New file %s", path);
        } else {
            msg(a, "%s", err);
        }
    } else {
        snprintf(a->path, sizeof(a->path), "%s", path);
        a->modified = false;
        msg(a, "Opened %s", path);
    }
    for (int i = 0; i < a->nundo; i++)
        snap_free(&a->undo[i]);
    for (int i = 0; i < a->nredo; i++)
        snap_free(&a->redo[i]);
    a->nundo = a->nredo = 0;
    a->typing_run = false;
    a->quit_armed = false;
    preview_auto(a);
}


/* ---- file names in the prompts ------------------------------------------------------------------------------------------- */

/* The folder relative names are taken from: the folder of the current file, else the working directory (always absolute). */
static void base_dir(const struct app *a, char *out, size_t sz)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        snprintf(cwd, sizeof(cwd), "/");
    const char *sl = a->path[0] ? strrchr(a->path, '/') : NULL;
    if (!sl) {
        snprintf(out, sz, "%s", cwd);
    } else if (a->path[0] == '/') {
        snprintf(out, sz, "%.*s", (int)(sl == a->path ? 1 : sl - a->path), a->path);
    } else {
        snprintf(out, sz, "%.*s/%.*s", (int)(sizeof(cwd) / 2), cwd, (int)(sl - a->path), a->path);
    }
}

/* ---- find ---------------------------------------------------------------------------------------------------------------- */

/* The next occurrence of `needle` starting at or after (y, x), wrapping around the document once. */
static bool find_from(const struct app *a, const char *needle, int y, size_t x, int *fy, size_t *fx)
{
    size_t nl = strlen(needle);
    if (!nl)
        return false;
    for (int k = 0; k <= a->n; k++) {                                    /* k == n: the start of the first line again, up to x */
        int yy = (y + k) % a->n;
        const struct line *l = &a->l[yy];
        for (size_t i = k == 0 ? x : 0; i + nl <= l->len; i++) {
            if (k == a->n && i >= x)
                break;
            if (!memcmp(l->s + i, needle, nl)) {
                *fy = yy;
                *fx = i;
                return true;
            }
        }
    }
    return false;
}

static void find_next(struct app *a)
{
    if (!a->find[0])
        return;
    int y, y0, y1;
    size_t x, x0, x1;
    sel_order(a, &y0, &x0, &y1, &x1);
    y = has_sel(a) ? y1 : a->cy;                                         /* continue after the current match */
    x = has_sel(a) ? x1 : a->cx;
    if (find_from(a, a->find, y, x, &y, &x)) {
        a->ay = y;
        a->ax = x;
        a->cy = y;
        a->cx = x + strlen(a->find);
        set_want(a);
        a->win.redraw = true;
    } else {
        msg(a, "Not found: %s", a->find);
    }
}

/* ---- movement and editing commands ----------------------------------------------------------------------------------------- */

static void word_left(struct app *a, bool extend)
{
    int y = a->cy;
    size_t x = a->cx;
    const struct line *l = &a->l[y];
    if (x == 0) {
        if (y > 0) {                                                     /* the end of the line above */
            y--;
            x = a->l[y].len;
        }
    } else {
        while (x > 0 && !is_word((unsigned char)l->s[cp_prev(l, x)]))    /* blanks and punctuation, then the word before them */
            x = cp_prev(l, x);
        while (x > 0 && is_word((unsigned char)l->s[cp_prev(l, x)]))
            x = cp_prev(l, x);
    }
    move_to(a, y, x, extend);
    set_want(a);
}

static void word_right(struct app *a, bool extend)
{
    int y = a->cy;
    size_t x = a->cx;
    while (x < a->l[y].len && !is_word((unsigned char)a->l[y].s[x]))
        x = cp_next(&a->l[y], x);
    if (x >= a->l[y].len && y < a->n - 1 && a->cx == a->l[y].len) {
        y++;
        x = 0;
    }
    while (x < a->l[y].len && is_word((unsigned char)a->l[y].s[x]))
        x = cp_next(&a->l[y], x);
    move_to(a, y, x, extend);
    set_want(a);
}

static void type_text(struct app *a, const char *t, size_t n, bool typing)
{
    before_edit(a, typing);
    if (has_sel(a))
        delete_selection(a);
    insert_at_cursor(a, t, n);
    set_want(a);
}

static void key_enter(struct app *a)
{
    before_edit(a, false);
    if (has_sel(a))
        delete_selection(a);
    const struct line *l = &a->l[a->cy];
    size_t ind = 0;
    while (ind < a->cx && (l->s[ind] == ' ' || l->s[ind] == '\t'))     /* keep the indentation of the line */
        ind++;
    char buf[258];
    buf[0] = '\n';
    memcpy(buf + 1, l->s, LM_MIN(ind, (size_t)256));
    insert_at_cursor(a, buf, 1 + LM_MIN(ind, (size_t)256));
    set_want(a);
}

static void key_backspace(struct app *a, bool word)
{
    if (!has_sel(a)) {
        if (a->cy == 0 && a->cx == 0)
            return;
        if (word)
            word_left(a, true);
        else if (a->cx == 0)
            move_to(a, a->cy - 1, a->l[a->cy - 1].len, true);
        else
            move_to(a, a->cy, cp_prev(&a->l[a->cy], a->cx), true);
    }
    before_edit(a, false);
    delete_selection(a);
    set_want(a);
}

static void key_delete(struct app *a, bool word)
{
    if (!has_sel(a)) {
        if (a->cy == a->n - 1 && a->cx == a->l[a->cy].len)
            return;
        int y = a->cy;
        size_t x = a->cx;
        if (word)
            word_right(a, true);
        else if (a->cx == a->l[a->cy].len)
            move_to(a, y + 1, 0, true);
        else
            move_to(a, y, cp_next(&a->l[y], x), true);
    }
    before_edit(a, false);
    delete_selection(a);
    set_want(a);
}

/* ---- clipboard ------------------------------------------------------------------------------------------------------------ */

static void copy_text(struct app *a, const char *t)
{
    wlclip_copy(&a->clip, t);
}

static void cmd_copy(struct app *a, bool cut)
{
    if (a->preview && a->md) {                                          /* the page: the selected text (it cannot be cut) */
        char *t = md_sel_text(a->md);
        if (t && *t)
            copy_text(a, t);
        free(t);
        return;
    }
    if (!has_sel(a))
        return;
    int y0, y1;
    size_t x0, x1;
    sel_order(a, &y0, &x0, &y1, &x1);
    char *t = range_text(a, y0, x0, y1, x1, NULL);
    copy_text(a, t);
    free(t);
    if (cut) {
        before_edit(a, false);
        delete_selection(a);
        set_want(a);
    }
}

static void cmd_paste(struct app *a)
{
    size_t len = 0;
    char *text = wlclip_paste(&a->clip, &len);
    if (!text)
        return;
    size_t n = 0;
    for (size_t i = 0; i < len; i++)                                    /* \r\n and \r become \n; stray NULs go */
        if (text[i] == '\r')
            text[n++] = (i + 1 < len && text[i + 1] == '\n') ? (i++, '\n') : '\n';
        else if (text[i])
            text[n++] = text[i];
    if (n && valid_utf8((const unsigned char *)text, n))
        type_text(a, text, n, false);
    else if (n)
        msg(a, "The clipboard is not valid UTF-8 text");
    free(text);
}

/* ---- keys ----------------------------------------------------------------------------------------------------------------- */

static void prompt_open(struct app *a, int kind, const char *initial, int fresh)
{
    a->prompt = kind;
    snprintf(a->pbuf, sizeof(a->pbuf), "%.511s", initial);
    if (fresh == 2)
        wltext_select_all(a->pbuf);                                     /* a suggested file name starts selected: typing replaces it */
    else
        wltext_end(a->pbuf);
    a->pfresh = fresh;
    a->ow_armed = false;
    a->csel = -1;
    a->ctop = 0;
    a->cdir[0] = 0;
    if (g_rail && !g_note)
        a->msg[0] = 0;                                                  /* (the card shows only what is said while it is open) */
    a->win.redraw = true;
}

/* A folder name with a closing "/" (but "/" stays "/"). */
static void dir_slash(const char *dir, char *out, size_t sz)
{
    snprintf(out, sz, "%s%s", dir, dir[0] && dir[strlen(dir) - 1] == '/' ? "" : "/");
}

/* ---- commands (keys and buttons) */

static void cmd_open(struct app *a)
{
    char base[PATH_MAX], init[PATH_MAX + 2];
    base_dir(a, base, sizeof(base));
    dir_slash(base, init, sizeof(init));
    prompt_open(a, P_OPEN, init, 1);
    if (!g_rail)
        msg(a, "Type a name (Tab completes), or a folder to look inside; Enter opens, Esc cancels");
}

static void cmd_save(struct app *a, bool as)
{
    if (as || !a->path[0]) {
        char base[PATH_MAX], init[PATH_MAX + 2];
        base_dir(a, base, sizeof(base));
        if (a->path[0]) {
            prompt_open(a, P_SAVEAS, a->path, 2);
        } else {
            dir_slash(base, init, sizeof(init));
            prompt_open(a, P_SAVEAS, init, 1);
        }
        if (!g_rail)
            msg(a, "Type a file name (Tab completes); Enter saves, Esc cancels");
    } else {
        do_save(a, a->path);
    }
    set_title(a);
}

static void cmd_new(struct app *a)
{
    if (a->modified && !a->quit_armed) {
        a->quit_armed = true;
        msg(a, "Unsaved changes: save first (Ctrl+S), or start a new document again to discard them");
        return;
    }
    doc_clear(a);
    a->path[0] = 0;
    a->modified = a->quit_armed = false;
    a->final_nl = true;
    a->crlf = false;
    preview_auto(a);
}

static void cmd_find(struct app *a)
{
    prompt_open(a, P_FIND, a->find, 0);
}

/* ---- zoom ------------------------------------------------------------------------------------------------------------------- */

static int gutter_w(const struct app *a);

/* The text gets bigger or smaller; the window follows (the same number of columns and rows stay visible). A maximized
 * window keeps its size and shows more or fewer columns and rows instead. */
static void set_zoom_pct(struct app *a, int pct)
{
    pct = LM_CLAMP(pct, LM_ZOOM_LEVELS[0], LM_ZOOM_LEVELS[LM_N_ZOOM - 1]);
    if (pct == a->zoom)
        return;
    a->zoom = pct;
    lm_zoom_build(&a->zt, pct);
    a->conf_dirty = true;
    a->conf_time = now_ms();
    if (a->rows_visible > 0 && a->cols_visible > 0) {
        int bars = STATUS_H + PBAR(a);
        wlwin_request_size(&a->win, gutter_w(a) + a->cols_visible * a->zt.cw + RIGHT_W + lm_metrics.scrollbar_width, TOP_H + a->rows_visible * a->zt.ch + bars);
    }
    a->win.redraw = true;
}

static void set_zoom(struct app *a, int delta)
{
    set_zoom_pct(a, lm_zoom_step(a->zoom, delta));
}


/* ---- Markdown preview ---------------------------------------------------------------------------------------------------------- */

static bool is_md_path(const char *p)
{
    const char *dot = strrchr(p, '.');
    if (!dot)
        return false;
    static const char *const ext[] = { ".md", ".markdown", ".mdown", ".mkd" };
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
        if (!strcasecmp(dot, ext[i]))
            return true;
    return false;
}

#define PV_STEP 44                                                      /* a pair of lines */
#define PV_PAD 22
#define PV_COLW 820

static int pv_view_h(const struct app *a)
{
    return LM_MAX(a->win.bh - STATUS_H - PBAR(a) - TOP_H, 1);
}

static int pv_col(const struct app *a, int *x)
{
    int avail = a->win.bw - RAIL_W - lm_metrics.scrollbar_width - 2 * PV_PAD;
    int colw = LM_CLAMP(avail, 120, PV_COLW);
    if (x)
        *x = RAIL_W + (a->win.bw - RAIL_W - RIGHT_W - lm_metrics.scrollbar_width - colw) / 2;
    return colw;
}

static void pv_dir(const struct app *a, char *out, size_t n)
{
    snprintf(out, n, "%s", a->path);
    char *sl = strrchr(out, '/');
    if (sl && sl != out)
        *sl = 0;
    else if (sl)
        out[1] = 0;
    else
        snprintf(out, n, ".");
}

static void pv_rebuild(struct app *a)
{
    md_free(a->md);
    size_t total = 0;
    for (int i = 0; i < a->n; i++)
        total += a->l[i].len + 1;
    char *t = malloc(total + 1);
    size_t o = 0;
    for (int i = 0; i < a->n; i++) {
        memcpy(t + o, a->l[i].s, a->l[i].len);
        o += a->l[i].len;
        t[o++] = '\n';
    }
    a->md = md_parse(t, o);
    free(t);
}

static void pv_layout(struct app *a)
{
    if (!a->md)
        return;
    int colw = pv_col(a, NULL);
    if (md_width(a->md) != colw || md_height(a->md) == 0) {
        char dir[PATH_MAX];
        pv_dir(a, dir, sizeof(dir));
        md_layout(a->md, colw, dir);
    }
    int max = LM_MAX(md_height(a->md) - pv_view_h(a), 0);
    a->md_scroll = LM_CLAMP(a->md_scroll, 0, max);
}

static void preview_set(struct app *a, bool on)
{
    if (on && (g_note || !is_md_path(a->path))) {
        msg(a, "The preview is for Markdown files (.md)");
        return;
    }
    if (on == a->preview)
        return;
    a->preview = on;
    a->md_hover = NULL;
    a->md_sb_drag = false;
    if (on) {
        double frac = a->n > 1 ? (double)a->cy / (a->n - 1) : 0;       /* about where the cursor was */
        pv_rebuild(a);
        pv_layout(a);
        a->md_scroll = (int)(frac * LM_MAX(md_height(a->md) - pv_view_h(a), 0));
    } else {
        double frac = md_height(a->md) > pv_view_h(a) ? (double)a->md_scroll / (md_height(a->md) - pv_view_h(a)) : 0;
        int y = (int)(frac * (a->n - 1) + 0.5);                          /* and the other way */
        a->ay = a->cy = LM_CLAMP(y, 0, a->n - 1);
        a->ax = a->cx = 0;
        a->top = LM_MAX(a->cy - 2, 0);
        a->seen_cy = -1;
    }
    set_title(a);
    a->win.redraw = true;
}

/* A Markdown file opens as a page, any other one (and a note) as text. */
static void preview_auto(struct app *a)
{
    bool want = !g_note && a->path[0] && is_md_path(a->path);
    a->preview = false;
    md_free(a->md);
    a->md = NULL;
    a->md_scroll = 0;
    if (want)
        preview_set(a, true);
    else
        set_title(a);
}

static void spawn_detached(char *const argv[])
{
    pid_t pid = fork();
    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 == 0) {
            setsid();
            int nul = open("/dev/null", O_RDWR);
            if (nul >= 0) {
                dup2(nul, 0);
                dup2(nul, 1);
                dup2(nul, 2);
            }
            execvp(argv[0], argv);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

static void open_link(struct app *a, const char *url)
{
    if (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8) || !strncmp(url, "mailto:", 7)) {
        const char *prog = getenv("MELLAGUI_XDG_OPEN");
        char *argv[] = { (char *)(prog && *prog ? prog : "xdg-open"), (char *)url, NULL };
        spawn_detached(argv);
        msg(a, "Opened %s", url);
        return;
    }
    if (url[0] == '#' || strstr(url, "://") || !url[0])
        return;
    char path[PATH_MAX], dir[PATH_MAX];
    pv_dir(a, dir, sizeof(dir));
    snprintf(path, sizeof(path), url[0] == '/' ? "%s%s" : "%s/%s", url[0] == '/' ? "" : dir, url);
    char *hash = strchr(path, '#');
    if (hash)
        *hash = 0;
    if (access(path, R_OK) != 0) {
        msg(a, "No such file: %s", path);
        return;
    }
    char self[PATH_MAX];
    ssize_t k = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (k <= 0)
        return;
    self[k] = 0;
    char *argv[] = { self, path, NULL };
    spawn_detached(argv);
}

/* Keys in the preview. Returns true when the key was dealt with (the text cannot be changed there). */
static bool preview_key(struct app *a, uint32_t ks, char letter)
{
    int vh = pv_view_h(a), max = LM_MAX(md_height(a->md) - vh, 0);
    bool ctrl = (a->win.mods & LM_MOD_CTRL) != 0, shift = (a->win.mods & LM_MOD_SHIFT) != 0;
    if (letter == 'm' || (!ctrl && (ks == 'e' || ks == 'E'))) {
        preview_set(a, false);
        return true;
    }
    if (letter == 'q' || letter == 'o' || letter == 'n' || ks == LM_KS_F1)           /* these go on as in the text */
        return false;
    if (letter == 'c' || ks == 0xff63) {                                /* Ctrl+C, Ctrl+Insert: copy the selected text of the page */
        cmd_copy(a, false);
        return true;
    }
    if (letter == 'a') {
        md_sel_all(a->md);
        a->win.redraw = true;
        return true;
    }
    int d = 0;
    switch (ks) {
    case 0xff52: d = -PV_STEP; break;                                   /* Up */
    case 0xff54: d = PV_STEP; break;                                    /* Down */
    case 0xff55: d = -(vh - PV_STEP); break;                            /* PageUp */
    case 0xff56: d = vh - PV_STEP; break;                               /* PageDown */
    case ' ': d = shift ? -(vh - PV_STEP) : vh - PV_STEP; break;
    case 0xff50: a->md_scroll = 0; break;                               /* Home */
    case 0xff57: a->md_scroll = max; break;                             /* End */
    default: break;
    }
    a->md_scroll = LM_CLAMP(a->md_scroll + d, 0, max);
    return true;
}

static void pv_thumb(const struct app *a, int *ty, int *th)
{
    int track = pv_view_h(a), total = LM_MAX(md_height(a->md), 1), vh = pv_view_h(a);
    *th = LM_CLAMP(track * vh / total, LM_MIN(28, track), track);
    int max = LM_MAX(total - vh, 1);
    *ty = TOP_H + (track - *th) * a->md_scroll / max;
}

static void preview_content(struct app *a)
{
    pv_layout(a);
    int cx;
    pv_col(a, &cx);
    md_paint(&a->win, a->md, cx, TOP_H, pv_col(a, NULL), pv_view_h(a), a->md_scroll);
}

static void preview_scrollbar(struct app *a)
{
    struct wlwin *w = &a->win;
    int sbw = lm_metrics.scrollbar_width, x = w->bw - RIGHT_W - sbw;
    wlwin_fill(w, x, TOP_H, sbw, pv_view_h(a), COL_BAR);
    if (md_height(a->md) <= pv_view_h(a))
        return;
    int ty, th;
    pv_thumb(a, &ty, &th);
    bool hot = a->md_sb_drag || (a->px >= x && a->py >= ty && a->py < ty + th);
    wlwin_fill(w, x + 2, ty + 1, sbw - 4, th - 2, hot ? lm_theme.control_hot : lm_theme.control);
}

static void preview_dump(struct app *a)
{
    const char *path = getenv("MELLAGUI_EDIT_DUMP");
    if (!path || !*path)
        return;
    char tmpn[PATH_MAX + 8];
    snprintf(tmpn, sizeof(tmpn), "%s.tmp", path);
    FILE *f = fopen(tmpn, "w");
    if (!f)
        return;
    int cx = 0;
    if (a->preview) {
        pv_col(a, &cx);
        fprintf(f, "preview=1 colx=%d top=%d scroll=%d view=%d\n", cx, TOP_H, a->md_scroll, pv_view_h(a));
        md_dump(a->md, f);
    } else {
        fprintf(f, "preview=0\nok\n");
    }
    fclose(f);
    rename(tmpn, path);
}

enum { B_NEW, B_OPEN, B_SAVE, B_SAVEAS, B_CUT, B_COPY, B_PASTE, B_FIND, B_SMALLER, B_ZOOM, B_BIGGER, B_HELP, B_VIEW, N_BTNS };

static const char *btn_label(const struct app *a, int i, char *buf, size_t n)
{
    static const char *const l[N_BTNS] = { "New", "Open", "Save", "Save as", "Cut", "Copy", "Paste", "Find", "A-", "", "A+", "Help", "" };
    if (i == B_ZOOM) {
        snprintf(buf, n, "%d%%", a->zoom);
        return buf;
    }
    if (i == B_VIEW)
        return a->preview ? "Edit" : "Preview";
    return l[i];
}

/* Where button i is (x, width). A button that does not fit in the window width is left out (from the right), so a narrow
 * window shows the first buttons whole and never draws over the edge: its width is 0 then. */
static void fill_round(struct wlwin *w, int x, int y, int width, int height, int r, uint32_t rgb);
static void edit_rails(const struct app *a, struct wlrail *L, struct wlrail *R);
static const int rail_right_ids[5];

static void btn_rect(const struct app *a, int i, int *x, int *w)
{
    char tmp[16];
    int bx = 6;
    for (int k = 0; k < i; k++) {
        int chars = k == B_ZOOM ? 4 : (int)strlen(btn_label(a, k, tmp, sizeof(tmp)));
        bx += chars * LM_FONT_W + 16 + 4;
    }
    int chars = i == B_ZOOM ? 4 : (int)strlen(btn_label(a, i, tmp, sizeof(tmp)));
    *x = bx;
    *w = chars * LM_FONT_W + 16;
    if (a->win.bw > 0 && bx + *w + 6 > a->win.bw)
        *w = 0;
    if (i == B_VIEW && (g_note || !is_md_path(a->path)))                /* only a Markdown file has the button */
        *w = 0;
}

static int btn_at(const struct app *a, double px, double py)
{
    if (g_rail && !g_note) {
        struct wlrail L, R;
        edit_rails(a, &L, &R);
        int k = wlrail_at(&L, px, py);
        if (k >= 0)
            return k;
        k = wlrail_at(&R, px, py);
        return k >= 0 ? rail_right_ids[k] : -1;
    }
    if (py < 2 || py >= TOP_H - 2)
        return -1;
    for (int i = 0; i < N_BTNS; i++) {
        int x, w;
        btn_rect(a, i, &x, &w);
        if (w > 0 && px >= x && px < x + w)
            return i;
    }
    return -1;
}

/* The rails (wlrail.h): the file and edit buttons at the left, the page / text size buttons and Help at the right. A rail's item number is its position in it:
 * the left one is B_NEW..B_FIND, the right one is rail_right_ids. */
static const int rail_right_ids[5] = { B_VIEW, B_BIGGER, B_ZOOM, B_SMALLER, B_HELP };

static void edit_rails(const struct app *a, struct wlrail *L, struct wlrail *R)
{
    static const char *const tips[N_BTNS] = { "New  Ctrl+N", "Open  Ctrl+O", "Save  Ctrl+S", "Save as  Ctrl+Shift+S", "Cut  Ctrl+X", "Copy  Ctrl+C", "Paste  Ctrl+V",
                                              "Find  Ctrl+F", "Smaller text  Ctrl+-", "Text size: click for 100 %", "Bigger text  Ctrl++", "Keys  F1", "" };
    static void (*const icons[8])(struct wlwin *, int, int, uint32_t, uint32_t) = { wlrail_ic_new, wlrail_ic_open, wlrail_ic_save, wlrail_ic_saveas, wlrail_ic_cut,
                                                                                     wlrail_ic_copy, wlrail_ic_paste, wlrail_ic_find };
    const uint32_t base[N_BTNS] = { lm_theme.btn_green, lm_theme.btn_blue, lm_theme.btn_amber, lm_theme.btn_teal, lm_theme.btn_rust, lm_theme.btn_sky, lm_theme.btn_lime, lm_theme.btn_purple, lm_theme.btn_slate, lm_theme.btn_grey, lm_theme.btn_slate, lm_theme.btn_rose, lm_theme.btn_orange };
    const uint32_t lite[N_BTNS] = { lm_theme.btn_green_lit, lm_theme.btn_blue_lit, lm_theme.btn_amber_lit, lm_theme.btn_teal_lit, lm_theme.btn_rust_lit, lm_theme.btn_sky_lit, lm_theme.btn_lime_lit, lm_theme.btn_purple_lit, lm_theme.btn_slate_lit, lm_theme.btn_grey_lit, lm_theme.btn_slate_lit, lm_theme.btn_rose_lit, lm_theme.btn_orange_lit };
    static char zbuf[8];
    memset(L, 0, sizeof(*L));
    memset(R, 0, sizeof(*R));
    int bottom = a->win.bh > 0 ? a->win.bh - STATUS_H - PBAR(a) : 400;
    L->x = 0;
    R->x = a->win.bw > 0 ? a->win.bw - WLRAIL_W : 0;
    L->y0 = R->y0 = 0;
    L->y1 = R->y1 = bottom;
    L->n = 8;
    for (int i = 0; i < 8; i++)
        L->it[i] = (struct wlrail_item){ .tip = tips[i], .icon = icons[i], .base = base[i], .lite = lite[i], .show = true,
                                         .idle = (i == B_CUT || i == B_COPY) && !has_sel(a), .lit = i == B_SAVE && a->modified, .gap = i == B_CUT };
    R->n = 5;
    snprintf(zbuf, sizeof(zbuf), "%d", a->zoom);
    for (int j = 0; j < 5; j++) {
        int id = rail_right_ids[j];
        const char *text = id == B_VIEW ? (a->preview ? "Ed" : "Md") : id == B_BIGGER ? "A+" : id == B_ZOOM ? zbuf : id == B_SMALLER ? "A-" : "?";
        const char *tip = id == B_VIEW ? (a->preview ? "Edit the text  Ctrl+M" : "Show the page  Ctrl+M") : tips[id];
        R->it[j] = (struct wlrail_item){ .tip = tip, .text = text, .base = base[id], .lite = lite[id], .show = id != B_VIEW || (!g_note && is_md_path(a->path)),
                                         .bottom = id == B_HELP };
    }
}

static int rail_hover(const struct app *a, bool right)
{
    if (!right)
        return a->hover >= 0 && a->hover <= B_FIND ? a->hover : -1;
    for (int j = 0; j < 5; j++)
        if (a->hover == rail_right_ids[j])
            return j;
    return -1;
}

static void paint_rail(struct app *a)
{
    struct wlrail L, R;
    edit_rails(a, &L, &R);
    int64_t now = now_ms();
    wlrail_paint(&a->win, &L, &a->ran[0], rail_hover(a, false), now, COL_BAR, COL_BAR, COL_ACCENT, COL_FG);
    wlrail_paint(&a->win, &R, &a->ran[1], rail_hover(a, true), now, COL_BAR, COL_BAR, COL_ACCENT, COL_FG);
}

/* The springs of both rails; true while something moves. */
static bool rails_step(struct app *a)
{
    struct wlrail L, R;
    edit_rails(a, &L, &R);
    int64_t now = now_ms();
    bool m0 = wlrail_step(&a->ran[0], &L, rail_hover(a, false), now);
    bool m1 = wlrail_step(&a->ran[1], &R, rail_hover(a, true), now);
    return m0 || m1;
}

static void rails_release(struct app *a)
{
    wlrail_release(&a->ran[0]);
    wlrail_release(&a->ran[1]);
}

static void do_button(struct app *a, int i)
{
    switch (i) {
    case B_NEW: cmd_new(a); break;
    case B_OPEN: cmd_open(a); break;
    case B_SAVE: cmd_save(a, false); break;
    case B_SAVEAS: cmd_save(a, true); break;
    case B_CUT: cmd_copy(a, true); set_title(a); break;
    case B_COPY: cmd_copy(a, false); break;
    case B_PASTE: cmd_paste(a); set_title(a); break;
    case B_FIND: cmd_find(a); break;
    case B_SMALLER: set_zoom(a, -1); break;
    case B_ZOOM: set_zoom(a, 0); break;                                 /* the size shown: a click goes back to 100 % */
    case B_BIGGER: set_zoom(a, 1); break;
    case B_HELP: a->help = true; break;
    case B_VIEW: preview_set(a, !a->preview); break;
    }
    a->win.redraw = true;
}

/* A folder was named: go into it, showing what is inside. */
static void prompt_enter_dir(struct app *a, const char *base)
{
    char tmp[512], list[160];
    dir_slash(a->pbuf, tmp, sizeof(tmp));
    snprintf(a->pbuf, sizeof(a->pbuf), "%s", tmp);
    int n = lm_complete_path(base, a->pbuf, sizeof(a->pbuf), false, list, sizeof(list));
    if (n > 0)
        msg(a, "%d here: %s", n, list);
    else
        msg(a, "An empty folder");
    a->pfresh = 0;
}

static void prompt_accept(struct app *a)
{
    int kind = a->prompt;
    char text[512], base[PATH_MAX], full[PATH_MAX];
    snprintf(text, sizeof(text), "%s", a->pbuf);
    base_dir(a, base, sizeof(base));
    lm_resolve_path(base, text, full, sizeof(full));
    struct stat st;
    bool is_dir = (kind == P_OPEN || kind == P_SAVEAS) && text[0] && stat(full, &st) == 0 && S_ISDIR(st.st_mode);
    if (is_dir) {                                                       /* Enter on a folder looks inside it */
        prompt_enter_dir(a, base);
        a->win.redraw = true;
        return;
    }
    switch (kind) {
    case P_OPEN:
        a->prompt = P_NONE;
        if (a->modified && !a->quit_armed) {
            a->quit_armed = true;
            msg(a, "Unsaved changes: press Enter again to open %s anyway, Esc to stay", text);
            a->prompt = P_OPEN;
            return;
        }
        if (text[0])
            do_open(a, full);
        break;
    case P_SAVEAS: {
        struct stat cur;
        bool other = text[0] && stat(full, &st) == 0 &&
                     !(a->path[0] && stat(a->path, &cur) == 0 && cur.st_dev == st.st_dev && cur.st_ino == st.st_ino);
        if (other && !a->ow_armed) {                                    /* do not overwrite another file by accident */
            a->ow_armed = true;
            msg(a, "%s exists: Enter again to replace it, Esc to cancel", text);
            return;
        }
        a->prompt = P_NONE;
        if (text[0])
            do_save(a, full);
        break;
    }
    case P_FIND:
        a->prompt = P_NONE;
        snprintf(a->find, sizeof(a->find), "%.255s", text);
        find_next(a);
        break;
    case P_GOTO: {
        a->prompt = P_NONE;
        int line = atoi(text);
        if (line >= 1) {
            move_to(a, line - 1, 0, false);
            set_want(a);
        }
        break;
    }
    }
    set_title(a);
    a->win.redraw = true;
}

static bool card_on(const struct app *a);
static bool card_files(const struct app *a);
static void card_move(struct app *a, int delta);
static void card_choose(struct app *a, int row, bool accept);

static void edit_prompt(struct app *a, uint32_t ks, bool plain)
{
    char txt[8];
    bool files = a->prompt == P_OPEN || a->prompt == P_SAVEAS;
    if (card_on(a) && card_files(a)) {                                  /* the card's list: Up / Down / PageUp / PageDown choose a row, Enter takes it */
        if (ks == LM_KS_Up || ks == LM_KS_Down || ks == LM_KS_Page_Up || ks == LM_KS_Page_Down) {
            card_move(a, ks == LM_KS_Up ? -1 : ks == LM_KS_Down ? 1 : ks == LM_KS_Page_Up ? -8 : 8);
            a->win.redraw = true;
            return;
        }
        if ((ks == LM_KS_Return || ks == LM_KS_KP_Enter) && a->csel >= 0) {
            card_choose(a, a->csel, true);
            a->win.redraw = true;
            return;
        }
    }
    if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
        prompt_accept(a);
        return;
    }
    a->ow_armed = false;
    if (ks == LM_KS_Escape) {
        a->prompt = P_NONE;
        a->quit_armed = false;
        a->msg[0] = 0;
    } else if (ks == LM_KS_Tab) {
        if (files) {
            char base[PATH_MAX], list[160];
            base_dir(a, base, sizeof(base));
            int n = lm_complete_path(base, a->pbuf, sizeof(a->pbuf), true, list, sizeof(list));
            if (n == 0)
                msg(a, "Nothing here starts like that");
            else if (n > 1)
                msg(a, "%d matches: %s", n, list);
            a->pfresh = 0;
        }
    } else {
        if (a->pfresh == 1 && plain && lm_keysym_to_utf8(ks, txt) > 0 && (txt[0] == '/' || txt[0] == '~'))
            a->pbuf[0] = 0;                                             /* typing a path over a suggested folder replaces it */
        wltext_edit(&a->clip, a->pbuf, sizeof(a->pbuf), ks, a->win.mods);       /* a caret, a selection, the clipboard (wltext.h) */
        a->pfresh = 0;
        a->csel = -1;                                                   /* (the list follows what is typed) */
        a->ctop = 0;
    }
    a->win.redraw = true;
}

/* ---- the card (prototype): Open / Save as / Find / Go to line as a modal over the text ---------------------------------------------------- */

#define CARD_ROW_H 22
#define CARD_MAX_ROWS 10

static bool card_on(const struct app *a)
{
    return g_rail && !g_note && a->prompt != P_NONE;
}

static bool card_files(const struct app *a)
{
    return a->prompt == P_OPEN || a->prompt == P_SAVEAS;
}

/* The folder part of the field ("~/a/b/" of "~/a/b/c.txt"; "" when there is no slash) and the rest, which filters the list. */
static void card_split(const struct app *a, char *dirtext, size_t n, const char **filter)
{
    const char *sl = strrchr(a->pbuf, '/');
    size_t len = sl ? (size_t)(sl - a->pbuf) + 1 : 0;
    snprintf(dirtext, n, "%.*s", (int)LM_MIN(len, n - 1), a->pbuf);
    *filter = a->pbuf + len;
}

static int centry_cmp(const void *x, const void *y)
{
    const struct centry *p = x, *q = y;
    if (p->dir != q->dir)
        return p->dir ? -1 : 1;
    return strcasecmp(p->name, q->name);
}

static void card_load(struct app *a, const char *dirtext)
{
    char base[PATH_MAX], full[PATH_MAX];
    base_dir(a, base, sizeof(base));
    lm_resolve_path(base, dirtext[0] ? dirtext : ".", full, sizeof(full));
    if (!strcmp(full, a->cdir) && a->cent)
        return;
    snprintf(a->cdir, sizeof(a->cdir), "%s", full);
    a->cn = 0;
    DIR *d = opendir(full);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && a->cn < 4000) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strlen(e->d_name) >= sizeof(a->cent[0].name))
            continue;
        if (a->cn == a->ccap) {
            a->ccap = a->ccap ? a->ccap * 2 : 128;
            a->cent = realloc(a->cent, (size_t)a->ccap * sizeof(a->cent[0]));
        }
        struct centry *c = &a->cent[a->cn++];
        snprintf(c->name, sizeof(c->name), "%s", e->d_name);
        char path[PATH_MAX + 260];
        struct stat st;
        snprintf(path, sizeof(path), "%.*s/%s", (int)LM_MIN(strlen(full), (size_t)PATH_MAX - 1), full, e->d_name);
        c->dir = e->d_type == DT_DIR || (e->d_type == DT_LNK && stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    }
    closedir(d);
    if (a->cn > 1)
        qsort(a->cent, (size_t)a->cn, sizeof(a->cent[0]), centry_cmp);
}

/* The rows that show: ".." first (unless this is the root), then the entries that start with what is typed after the last slash.
 * Returns the count; idx[k] = -1 is "..", else an index into a->cent. */
static int card_rows(struct app *a, int *idx, int max)
{
    char dirtext[PATH_MAX];
    const char *filter;
    card_split(a, dirtext, sizeof(dirtext), &filter);
    card_load(a, dirtext);
    int n = 0;
    size_t fl = strlen(filter);
    for (int i = 0; i < a->cn && fl; i++)                               /* a name typed out in full (a file just picked): the whole folder shows */
        if (!strcmp(a->cent[i].name, filter))
            fl = 0;
    if (strcmp(a->cdir, "/") && n < max && fl == 0)
        idx[n++] = -1;
    for (int i = 0; i < a->cn && n < max; i++) {
        if (a->cent[i].name[0] == '.' && filter[0] != '.')
            continue;
        if (fl && strncasecmp(a->cent[i].name, filter, fl))
            continue;
        idx[n++] = i;
    }
    return n;
}

static int card_visible_rows(const struct app *a)
{
    int room = (a->win.bh - 16 - 200) / CARD_ROW_H;
    return LM_CLAMP(room, 3, CARD_MAX_ROWS);
}

static void card_geometry(const struct app *a, int *cw, int *ch, int *list_rows)
{
    *cw = LM_CLAMP(a->win.bw - 16, 260, 560);
    *list_rows = card_files(a) ? card_visible_rows(a) : 0;
    *ch = WLCARD_PAD + 24 + 34 + (*list_rows ? *list_rows * CARD_ROW_H + 12 : 0) + 20 + WLCARD_BTN_H + WLCARD_PAD;
}

/* The field box, the list's top and the card: where everything is, for the painter and the mouse alike. */
static void card_layout(const struct app *a, struct wlcard *c, int *fx, int *fy, int *fw, int *ly, int *rows)
{
    int cw, ch;
    card_geometry(a, &cw, &ch, rows);
    c->w = LM_MIN(cw, a->win.bw - 16);
    c->h = LM_MIN(ch, a->win.bh - 16);
    c->x = (a->win.bw - c->w) / 2;
    c->y = (a->win.bh - c->h) / 2;
    *fx = c->x + WLCARD_PAD;
    *fy = c->y + WLCARD_PAD + 24;
    *fw = c->w - 2 * WLCARD_PAD;
    *ly = *fy + 34;
}

/* A text path with the entry's name in place of the part after the last slash; a folder gets its slash. */
static void card_put(struct app *a, int row_idx)
{
    char dirtext[PATH_MAX], next[PATH_MAX + 260];
    const char *filter;
    card_split(a, dirtext, sizeof(dirtext), &filter);
    if (row_idx < 0) {                                                  /* ".." */
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
        snprintf(next, sizeof(next), "%s%s%s", dirtext, a->cent[row_idx].name, a->cent[row_idx].dir ? "/" : "");
    }
    snprintf(a->pbuf, sizeof(a->pbuf), "%.511s", next);
    wltext_end(a->pbuf);
    a->pfresh = 0;
    a->csel = -1;
    a->ctop = 0;
    a->win.redraw = true;
}

static void prompt_accept(struct app *a);

/* A row chosen (Enter or a click): a folder is entered, a file goes into the field and, with `accept`, is opened or saved at once. */
static void card_choose(struct app *a, int row, bool accept)
{
    int idx[4001];
    int n = card_rows(a, idx, 4000);
    if (row < 0 || row >= n)
        return;
    int e = idx[row];
    card_put(a, e);
    if (accept && e >= 0 && !a->cent[e].dir)
        prompt_accept(a);
}

static void card_move(struct app *a, int delta)
{
    int idx[4001];
    int n = card_rows(a, idx, 4000), rows = card_visible_rows(a);
    if (n == 0)
        return;
    a->csel = LM_CLAMP(a->csel < 0 ? (delta > 0 ? 0 : n - 1) : a->csel + delta, 0, n - 1);
    if (a->csel < a->ctop)
        a->ctop = a->csel;
    if (a->csel >= a->ctop + rows)
        a->ctop = a->csel - rows + 1;
}

static const char *card_title(const struct app *a)
{
    static const char *const t[] = { "", "Open a file", "Save as", "Find", "Go to line" };
    return t[a->prompt];
}

static void card_button_labels(const struct app *a, const char **labels, uint32_t *cols)
{
    static const char *const act[] = { "", "Open", "Save", "Find", "Go" };
    labels[0] = "Cancel";
    labels[1] = act[a->prompt];
    cols[0] = lm_theme.btn_slate;
    cols[1] = a->prompt == P_SAVEAS ? lm_theme.btn_amber : a->prompt == P_OPEN ? lm_theme.btn_blue : lm_theme.btn_green;
}

static void paint_card(struct app *a)
{
    struct wlwin *w = &a->win;
    struct wlcard c;
    int fx, fy, fw, ly, rows;
    card_layout(a, &c, &fx, &fy, &fw, &ly, &rows);
    wlcard_frame(w, &c, c.w, c.h, card_title(a), COL_ACCENT, COL_BAR, COL_ACCENT);
    static const char *const hints[] = { "", "Tab completes", "Tab completes", "Enter = next", "" };
    int hw = (int)strlen(hints[a->prompt]) * LM_FONT_W;
    if (hw)
        wlwin_text(w, c.x + c.w - WLCARD_PAD - hw, c.y + WLCARD_PAD - 2, hints[a->prompt], 1, COL_DIM, 0);
    fill_round(w, fx, fy, fw, 28, 14, COL_BG);                            /* the field */
    if (a->pfresh == 1)
        wlwin_fill(w, fx + 12, fy + 6, LM_MIN((int)strlen(a->pbuf) * LM_FONT_W, fw - 24), LM_FONT_H, COL_SEL);
    wltext_paint(w, fx + 12, fy + 6, fw - 24, a->pbuf, true, COL_FG, COL_SEL, COL_FG);
    if (rows) {
        int idx[4001];
        int n = card_rows(a, idx, 4000);
        a->ctop = LM_CLAMP(a->ctop, 0, LM_MAX(n - rows, 0));
        fill_round(w, fx, ly, fw, rows * CARD_ROW_H + 8, 10, COL_BG);
        if (n == 0)
            wlwin_text(w, fx + 12, ly + 8, "Nothing here starts like that", 1, COL_DIM, fw - 24);
        for (int r = 0; r < rows && a->ctop + r < n; r++) {
            int k = a->ctop + r, e = idx[k], ry = ly + 4 + r * CARD_ROW_H;
            bool dir = e < 0 || a->cent[e].dir;
            bool over = a->px >= fx && a->px < fx + fw - 10 && a->py >= ry && a->py < ry + CARD_ROW_H;
            const char *tail = strrchr(a->pbuf, '/');
            bool named = e >= 0 && !strcmp(a->cent[e].name, tail ? tail + 1 : a->pbuf);     /* the file in the field */
            if (k == a->csel || named || over)
                fill_round(w, fx + 4, ry, fw - 8 - (n > rows ? lm_metrics.scrollbar_width : 0), CARD_ROW_H, 11, k == a->csel || named ? COL_SEL : COL_LINE);
            int ix = fx + 14, iy = ry + 4;
            if (dir) {                                                  /* a folder: a tab and a body */
                wlwin_fill(w, ix, iy + 1, 6, 3, lm_theme.btn_amber);
                wlwin_round_fill(w, ix, iy + 3, 14, 10, 3, lm_theme.btn_amber_lit);
            } else {
                wlwin_round_fill(w, ix + 2, iy, 10, 14, 2, lm_theme.btn_sky);
                wlwin_fill(w, ix + 4, iy + 5, 6, 1, COL_BAR);
                wlwin_fill(w, ix + 4, iy + 8, 6, 1, COL_BAR);
            }
            wlwin_text(w, ix + 24, ry + (CARD_ROW_H - LM_FONT_H) / 2, e < 0 ? ".." : a->cent[e].name, 1, dir ? COL_ACCENT : COL_FG, fw - 60);
        }
        if (n > rows) {                                                 /* a thin scroll bar */
            int sbw = lm_metrics.scrollbar_width, th = LM_MAX((rows * CARD_ROW_H) * rows / n, 20);
            int ty = ly + 4 + (rows * CARD_ROW_H - th) * a->ctop / LM_MAX(n - rows, 1);
            wlwin_round_fill(w, fx + fw - sbw - 2, ty, sbw - 4, th, (sbw - 4) / 2, lm_theme.control);
        }
    }
    bool showing = a->msg[0] && now_ms() < a->msg_until;                /* what the program has to say (a name exists, the folder is empty) */
    int my = c.y + c.h - WLCARD_PAD - WLCARD_BTN_H - 24;
    if (showing)
        wlwin_text(w, fx, my, a->msg, 1, COL_WARN, fw);
    const char *labels[2];
    uint32_t cols[2];
    card_button_labels(a, labels, cols);
    int hot = wlcard_button_at(&c, 2, (int)a->px, (int)a->py);
    wlcard_buttons(w, &c, labels, 2, cols, hot, a->cbtn_down, lm_theme.on_colour);
}

/* A press on the card. Everything outside the buttons and the list is swallowed (it is modal); a press outside the card cancels. */
static void card_press(struct app *a, uint32_t time_ms)
{
    struct wlcard c;
    int fx, fy, fw, ly, rows;
    card_layout(a, &c, &fx, &fy, &fw, &ly, &rows);
    int px = (int)a->px, py = (int)a->py;
    if (!wlcard_has(&c, px, py)) {
        a->prompt = P_NONE;
        a->quit_armed = false;
        a->msg[0] = 0;
        set_title(a);
        return;
    }
    int b = wlcard_button_at(&c, 2, px, py);
    if (b == 0) {
        a->prompt = P_NONE;
        a->quit_armed = false;
        a->msg[0] = 0;
        set_title(a);
        return;
    }
    if (b == 1) {
        a->cbtn_down = 1;
        prompt_accept(a);
        return;
    }
    if (px >= fx && px < fx + fw && py >= fy && py < fy + 28) {         /* the field: the caret */
        wltext_click(a->pbuf, px - (fx + 12), (a->win.mods & LM_MOD_SHIFT) != 0);
        a->pfresh = 0;
        return;
    }
    if (rows && px >= fx && px < fx + fw && py >= ly + 4 && py < ly + 4 + rows * CARD_ROW_H) {
        int idx[4001];
        int n = card_rows(a, idx, 4000), row = a->ctop + (py - ly - 4) / CARD_ROW_H;
        if (row < n) {
            bool dbl = time_ms - a->cclick_ms < (uint32_t)lm_metrics.double_click_ms;
            a->cclick_ms = time_ms;
            bool file = idx[row] >= 0 && !a->cent[idx[row]].dir;
            bool was = a->csel == row;
            card_put(a, idx[row]);
            if (file) {
                a->csel = row;                                          /* (the list stays as it is: the field now has the whole name) */
                if (dbl && was)
                    prompt_accept(a);
            }
        }
    }
}

static void page_move(struct app *a, int dir, bool extend)
{
    int step = LM_MAX(a->rows_visible - 1, 1) * dir;
    int y = LM_CLAMP(a->cy + step, 0, a->n - 1);
    move_to(a, y, off_at_col(&a->l[y], a->want_col), extend);
    a->top = LM_CLAMP(a->top + step, 0, LM_MAX(a->n - 1, 0));
}

/* ---- sticky notes: files in a folder, one window (one process) each ------------------------------------------------------------------------------- */

static const uint32_t note_papers[] = { 0xfff2a8, 0xffd1dc, 0xd5f5c3, 0xcfe4ff, 0xffdfb8, 0xe6e6e6 };
#define N_PAPERS ((int)(sizeof(note_papers) / sizeof(note_papers[0])))

static const char *note_dir(char *out, size_t n)
{
    const char *e = getenv("MELLAGUI_NOTES_DIR"), *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (e && *e)
        snprintf(out, n, "%s", e);
    else if (xdg && *xdg)
        snprintf(out, n, "%s/mellagui/notes", xdg);
    else
        snprintf(out, n, "%s/.local/share/mellagui/notes", home && *home ? home : "/tmp");
    for (char *p = out + 1; *p; p++)                                    /* mkdir -p */
        if (*p == '/') {
            *p = 0;
            mkdir(out, 0700);
            *p = '/';
        }
    mkdir(out, 0700);
    return out;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"          /* (a path that is too long is a failure to open, not a crash) */
static void note_new_path(char *out, size_t n)
{
    char dir[PATH_MAX], tmp[PATH_MAX + 64];
    note_dir(dir, sizeof(dir));
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    for (int k = 0; k < 100; k++) {
        snprintf(tmp, sizeof(tmp), "%s/%04d%02d%02d-%02d%02d%02d-%d.txt", dir, (tm.tm_year + 1900) % 10000, (tm.tm_mon + 1) % 100, tm.tm_mday % 100,
                 tm.tm_hour % 100, tm.tm_min % 100, tm.tm_sec % 100, k);
        snprintf(out, n, "%s", tmp);
        int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);          /* the name is taken from now on (an empty note is removed again when it is closed) */
        if (fd >= 0) {
            close(fd);
            return;
        }
    }
}

#pragma GCC diagnostic pop

static void note_spawn(const char *path)
{
    char exe[PATH_MAX];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (k <= 0)
        return;
    exe[k] = 0;
    pid_t pid = fork();
    if (pid == 0) {                                                     /* twice, so that nobody has to wait for the window */
        if (fork() == 0) {
            setsid();
            execl(exe, exe, "--note", path, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

static int cmp_names(const void *x, const void *y)
{
    return strcmp(*(char *const *)x, *(char *const *)y);
}

/* `--notes`: a window for every note there is (a new one when there is none). */
static int notes_open_all(void)
{
    char dir[PATH_MAX];
    note_dir(dir, sizeof(dir));
    DIR *d = opendir(dir);
    char *names[64];
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 64) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".txt"))
            names[n++] = strdup(e->d_name);
    }
    if (d)
        closedir(d);
    qsort(names, (size_t)n, sizeof(names[0]), cmp_names);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX + 300];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        note_spawn(path);
        free(names[i]);
    }
    if (n == 0) {
        char path[PATH_MAX];
        note_new_path(path, sizeof(path));
        note_spawn(path);
    }
    return 0;
}

static int g_note_lock = -1;

/* One window per note: a lock file beside it, held while the window lives. False when another window has it. */
static bool note_lock(const char *path)
{
    char lock[PATH_MAX + 8];
    snprintf(lock, sizeof(lock), "%s.lock", path);
    g_note_lock = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (g_note_lock < 0)
        return true;                                                    /* (no lock possible: better a second window than none) */
    if (flock(g_note_lock, LOCK_EX | LOCK_NB) < 0) {
        close(g_note_lock);
        g_note_lock = -1;
        return false;
    }
    return true;
}

static void note_paper_load(struct app *a)
{
    char side[PATH_MAX + 8];
    snprintf(side, sizeof(side), "%s.col", a->path);
    FILE *f = fopen(side, "r");
    int v = 0;
    if (f) {
        if (fscanf(f, "%d", &v) != 1)
            v = 0;
        fclose(f);
    }
    a->paper = LM_CLAMP(v, 0, N_PAPERS - 1);
    g_paper = note_papers[a->paper];
}

static void note_paper_next(struct app *a)
{
    a->paper = (a->paper + 1) % N_PAPERS;
    g_paper = note_papers[a->paper];
    char side[PATH_MAX + 8];
    snprintf(side, sizeof(side), "%s.col", a->path);
    FILE *f = fopen(side, "w");
    if (f) {
        fprintf(f, "%d\n", a->paper);
        fclose(f);
    }
    a->win.redraw = true;
}

/* Keys of a note: Ctrl+N a new note, Ctrl+D delete it (twice), Ctrl+K another colour, Ctrl+Q closes (it is saved); the file prompts of the editor are off. */
static bool note_key(struct app *a, char letter)
{
    switch (letter) {
    case 'n': {
        char path[PATH_MAX];
        note_new_path(path, sizeof(path));
        note_spawn(path);
        return true;
    }
    case 'k': note_paper_next(a); return true;
    case 'd':
        if (a->delete_armed && now_ms() - a->delete_ms < 3000) {
            char side[PATH_MAX + 8];
            snprintf(side, sizeof(side), "%s.col", a->path);
            unlink(a->path);
            unlink(side);
            a->modified = false;
            a->win.running = false;
        } else {
            a->delete_armed = true;
            a->delete_ms = now_ms();
            msg(a, "Ctrl+D again deletes this note");
        }
        return true;
    case 'q':
        a->win.running = false;
        return true;
    case 'o': case 'f': case 'g': case 'h':                             /* open, find, go to line, replace: not in a note */
        return true;
    default:
        return false;
    }
}

static void handle_key(struct app *a, uint32_t code, uint32_t ks)
{
    bool ctrl = (a->win.mods & LM_MOD_CTRL) != 0, shift = (a->win.mods & LM_MOD_SHIFT) != 0;
    bool alt = (a->win.mods & LM_MOD_ALT) != 0, super = (a->win.mods & LM_MOD_SUPER) != 0;
    bool plain = !ctrl && !alt && !super;
    (void)code;
    if (a->help) {                                                      /* any key closes the key list (but not a bare modifier) */
        if (ks < 0xffe1 || ks > 0xffee)
            a->help = false;
        return;
    }
    if (a->prompt != P_NONE) {
        edit_prompt(a, ks, plain);
        return;
    }
    char txt[8];
    int tl = lm_keysym_to_utf8(ks, txt);
    char letter = (tl == 1 && ctrl && !alt && !super) ? (char)((txt[0] >= 'A' && txt[0] <= 'Z') ? txt[0] + 32 : txt[0]) : 0;
    if (letter == 'm' && !g_note && is_md_path(a->path) && !a->preview) {
        preview_set(a, true);
        return;
    }
    if (a->preview && a->md && preview_key(a, ks, letter))
        return;
    if (g_note && letter && note_key(a, letter))
        return;
    if (letter) {
        switch (letter) {
        case 'q':
            if (a->modified && !a->quit_armed) {
                a->quit_armed = true;
                msg(a, "Unsaved changes: save first (Ctrl+S), or press Ctrl+Q again to discard them and quit");
            } else {
                a->win.running = false;
            }
            return;
        case 's': cmd_save(a, shift); return;
        case 'o': cmd_open(a); return;
        case 'n': cmd_new(a); return;
        case 'f': cmd_find(a); return;
        case '=':
        case '+': set_zoom(a, 1); return;
        case '-': set_zoom(a, -1); return;
        case '0': set_zoom(a, 0); return;
        case 'g': prompt_open(a, P_GOTO, "", 0); return;
        case 'a':
            a->ay = 0;
            a->ax = 0;
            move_to(a, a->n - 1, a->l[a->n - 1].len, true);
            return;
        case 'c': cmd_copy(a, false); return;
        case 'x': cmd_copy(a, true); set_title(a); return;
        case 'v': cmd_paste(a); set_title(a); return;
        case 'z':
            if (!undo(a, shift))
                msg(a, "Nothing to %s", shift ? "redo" : "undo");
            set_title(a);
            return;
        case 'y':
            if (!undo(a, true))
                msg(a, "Nothing to redo");
            set_title(a);
            return;
        default: break;
        }
    }
    switch (ks) {
    case LM_KS_Escape:
        a->msg[0] = 0;
        a->quit_armed = false;
        a->ay = a->cy;
        a->ax = a->cx;
        return;
    case LM_KS_Left:
        if (has_sel(a) && !shift && !ctrl) {
            int y0, y1;
            size_t x0, x1;
            sel_order(a, &y0, &x0, &y1, &x1);
            move_to(a, y0, x0, false);
        } else if (ctrl) {
            word_left(a, shift);
            return;
        } else if (a->cx == 0) {
            if (a->cy > 0)
                move_to(a, a->cy - 1, a->l[a->cy - 1].len, shift);
        } else {
            move_to(a, a->cy, cp_prev(&a->l[a->cy], a->cx), shift);
        }
        set_want(a);
        return;
    case LM_KS_Right:
        if (has_sel(a) && !shift && !ctrl) {
            int y0, y1;
            size_t x0, x1;
            sel_order(a, &y0, &x0, &y1, &x1);
            move_to(a, y1, x1, false);
        } else if (ctrl) {
            word_right(a, shift);
            return;
        } else if (a->cx >= a->l[a->cy].len) {
            if (a->cy < a->n - 1)
                move_to(a, a->cy + 1, 0, shift);
        } else {
            move_to(a, a->cy, cp_next(&a->l[a->cy], a->cx), shift);
        }
        set_want(a);
        return;
    case LM_KS_Up:
    case LM_KS_Down: {
        int y = a->cy + (ks == LM_KS_Up ? -1 : 1);
        if (y < 0)
            move_to(a, 0, 0, shift);
        else if (y >= a->n)
            move_to(a, a->n - 1, a->l[a->n - 1].len, shift);
        else
            move_to(a, y, off_at_col(&a->l[y], a->want_col), shift);
        return;
    }
    case LM_KS_Home:
        if (ctrl)
            move_to(a, 0, 0, shift);
        else {
            size_t ind = 0;                                              /* first press: the first non-blank, then the line start */
            while (ind < a->l[a->cy].len && (a->l[a->cy].s[ind] == ' ' || a->l[a->cy].s[ind] == '\t'))
                ind++;
            move_to(a, a->cy, a->cx == ind || ind == a->l[a->cy].len ? 0 : ind, shift);
        }
        set_want(a);
        return;
    case LM_KS_End:
        if (ctrl)
            move_to(a, a->n - 1, a->l[a->n - 1].len, shift);
        else
            move_to(a, a->cy, a->l[a->cy].len, shift);
        set_want(a);
        return;
    case LM_KS_Page_Up: page_move(a, -1, shift); return;
    case LM_KS_Page_Down: page_move(a, 1, shift); return;
    case LM_KS_F1: a->help = true; return;
    case LM_KS_F1 + 2: find_next(a); return;                            /* F3 */
    case LM_KS_BackSpace: key_backspace(a, ctrl); set_title(a); return;
    case LM_KS_Delete:
        if (shift && !ctrl) {                                           /* Shift+Delete: cut (the classic keys) */
            cmd_copy(a, true);
            set_title(a);
            return;
        }
        key_delete(a, ctrl);
        set_title(a);
        return;
    case LM_KS_Insert:
        if (ctrl && !shift)
            cmd_copy(a, false);                                          /* Ctrl+Insert: copy, Shift+Insert: paste */
        else if (shift && !ctrl) {
            cmd_paste(a);
            set_title(a);
        }
        return;
    case LM_KS_Return:
    case LM_KS_KP_Enter: key_enter(a); set_title(a); return;
    case LM_KS_Tab: type_text(a, "\t", 1, true); set_title(a); return;
    default: break;
    }
    if (plain && tl > 0 && (unsigned char)txt[0] >= 0x20 && txt[0] != 0x7f) {
        type_text(a, txt, (size_t)tl, true);
        set_title(a);
    }
}

/* ---- drawing --------------------------------------------------------------------------------------------------------------- */

static int gutter_w(const struct app *a)
{
    if (g_note)
        return 8;
    int digits = 1;
    for (int n = a->n; n >= 10; n /= 10)
        digits++;
    return RAIL_W + (LM_MAX(digits, 3) + 2) * a->zt.cw;
}

static uint32_t cp_at(const struct line *l, size_t off)
{
    int used = 0;
    uint32_t cp = lm_utf8_decode(l->s + off, (int)(l->len - off), &used);
    return cp;
}

static void scroll_to_cursor(struct app *a)
{
    int rows = LM_MAX(a->rows_visible, 1), cols = LM_MAX(a->cols_visible, 1);
    if (a->cy < a->top)
        a->top = a->cy;
    if (a->cy >= a->top + rows)
        a->top = a->cy - rows + 1;
    int col = col_of(&a->l[a->cy], a->cx);
    if (col < a->left)
        a->left = col;
    if (col >= a->left + cols - 1)
        a->left = col - cols + 2;
    a->left = LM_MAX(a->left, 0);
    a->top = LM_CLAMP(a->top, 0, a->n - 1);
}

static void fill_round(struct wlwin *w, int x, int y, int width, int height, int r, uint32_t rgb)
{
    wlwin_round_fill(w, x, y, width, height, r, rgb);
}

static void paint_help(struct wlwin *w)
{
    static const char *const lines[] = {
        "Editor keys",
        "",
        "Ctrl+N          new document",
        "Ctrl+O          open a file (Tab completes names)",
        "Ctrl+S          save (asks for a name when untitled)",
        "Ctrl+Shift+S    save under another name",
        "Ctrl+Q          quit (asks when there are unsaved changes)",
        "",
        "Ctrl+F          find, then Enter or F3 for the next match",
        "Ctrl+G          go to a line number",
        "Ctrl+M          a Markdown file: the page / the text (E also goes back to the text)",
        "Ctrl+Z / Ctrl+Y undo / redo",
        "Ctrl+A C X V    select all, copy, cut, paste (also Ctrl+Insert, Shift+Delete, Shift+Insert)",
        "",
        "Shift+arrows    select; Ctrl+Left / Right moves by word",
        "Home / End      line start / end (Ctrl: document)",
        "Mouse           click, drag to select, double click = word",
        "Ctrl+= / Ctrl+-  bigger / smaller text (or Ctrl+wheel); Ctrl+0 resets",
        "",
        "Any key or click closes this list",
    };
    int n = (int)(sizeof(lines) / sizeof(lines[0])), cw = 0;
    for (int i = 0; i < n; i++)
        cw = LM_MAX(cw, (int)strlen(lines[i]));
    int bw = LM_MIN(cw * LM_FONT_W + 32, w->bw - 8), bh = LM_MIN(n * LM_FONT_H + 24, w->bh - 8);
    int x = (w->bw - bw) / 2, y = (w->bh - bh) / 2;
    fill_round(w, x - 1, y - 1, bw + 2, bh + 2, 9, COL_ACCENT);
    fill_round(w, x, y, bw, bh, 8, COL_BAR);
    for (int i = 0; i < n && 12 + (i + 1) * LM_FONT_H <= bh; i++)
        wlwin_text(w, x + 16, y + 12 + i * LM_FONT_H, lines[i], 1, i == 0 ? COL_ACCENT : COL_FG, bw - 24);
}

/* ---- scroll bar -------------------------------------------------------------------------------------------------------------
 * A strip at the right edge between the button bar and the status line. The first visible line can go anywhere from 0 to n - 1,
 * so the thumb (as long as the visible part of n + rows - 1 lines) slides over that whole range. */

static int sb_track_h(const struct app *a)
{
    int bars = STATUS_H + PBAR(a);
    return LM_MAX(a->win.bh - bars - TOP_H, 1);
}

static void sb_thumb(const struct app *a, int *ty, int *th)
{
    int track = sb_track_h(a), total = LM_MAX(a->n + a->rows_visible - 1, 1);
    *th = LM_CLAMP(track * a->rows_visible / total, LM_MIN(28, track), track);
    *ty = TOP_H + (track - *th) * a->top / LM_MAX(a->n - 1, 1);
}

static void paint_scrollbar(struct wlwin *w, const struct app *a)
{
    int sbw = lm_metrics.scrollbar_width, x = w->bw - RIGHT_W - sbw;
    wlwin_fill(w, x, TOP_H, sbw, sb_track_h(a), COL_BAR);
    if (a->n <= 1)
        return;
    int ty, th;
    sb_thumb(a, &ty, &th);
    bool hot = a->sb_drag || (a->px >= x && a->py >= ty && a->py < ty + th);
    wlwin_fill(w, x + 2, ty + 1, sbw - 4, th - 2, hot ? lm_theme.control_hot : lm_theme.control);
}

static void sb_drag_to(struct app *a, double py)
{
    int ty, th, room = LM_MAX(sb_track_h(a) - 0, 1);
    sb_thumb(a, &ty, &th);
    room = LM_MAX(room - th, 1);
    int pos = LM_CLAMP((int)py - a->sb_grab - TOP_H, 0, room);
    a->top = LM_CLAMP(pos * (a->n - 1) / room, 0, LM_MAX(a->n - 1, 0));
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    int bars = STATUS_H + PBAR(a);
    int gw = gutter_w(a);
    a->rows_visible = LM_MAX((w->bh - bars - TOP_H) / a->zt.ch, 1);
    a->cols_visible = LM_MAX((w->bw - gw - RIGHT_W - lm_metrics.scrollbar_width) / a->zt.cw, 1);
    if (a->cy != a->seen_cy || a->cx != a->seen_cx || a->n != a->seen_n || a->rows_visible != a->seen_rows ||
        a->cols_visible != a->seen_cols) {                               /* the view follows the cursor only when that moved */
        scroll_to_cursor(a);
        a->seen_cy = a->cy;
        a->seen_cx = a->cx;
        a->seen_n = a->n;
        a->seen_rows = a->rows_visible;
        a->seen_cols = a->cols_visible;
    }
    wlwin_fill(w, 0, 0, w->bw, w->bh, COL_BG);
    if (a->preview && (g_note || !is_md_path(a->path)))                  /* (Save as to another name) */
        a->preview = false;
    if (a->preview)
        preview_content(a);                                              /* (under the bars: they are drawn over its ends) */
    if (!g_note && !g_rail)
        wlwin_fill(w, 0, 0, w->bw, TOP_H, COL_BAR);                     /* the button bar */
    for (int i = 0; i < (g_note || g_rail ? 0 : N_BTNS); i++) {
        int bx, bwid;
        char tmp[16];
        btn_rect(a, i, &bx, &bwid);
        if (bwid == 0)
            continue;
        const uint32_t base[N_BTNS] = { lm_theme.btn_green, lm_theme.btn_blue, lm_theme.btn_amber, lm_theme.btn_teal, lm_theme.btn_rust, lm_theme.btn_sky, lm_theme.btn_lime, lm_theme.btn_purple, lm_theme.btn_slate, lm_theme.btn_grey, lm_theme.btn_slate, lm_theme.btn_rose, lm_theme.btn_orange };
        const uint32_t lite[N_BTNS] = { lm_theme.btn_green_lit, lm_theme.btn_blue_lit, lm_theme.btn_amber_lit, lm_theme.btn_teal_lit, lm_theme.btn_rust_lit, lm_theme.btn_sky_lit, lm_theme.btn_lime_lit, lm_theme.btn_purple_lit, lm_theme.btn_slate_lit, lm_theme.btn_grey_lit, lm_theme.btn_slate_lit, lm_theme.btn_rose_lit, lm_theme.btn_orange_lit };
        bool lit = a->hover == i || (i == B_SAVE && a->modified);          /* Save is lit while there is something to save */
        bool idle = (i == B_CUT || i == B_COPY) && !has_sel(a);             /* nothing selected: Cut and Copy have nothing to take */
        wlwin_drop(w, bx, 2, bwid, TOP_H - 4, lm_button_radius((TOP_H - 4) / 2), idle ? lm_theme.control_idle : lit ? lite[i] : base[i]);
        const char *t = btn_label(a, i, tmp, sizeof(tmp));
        int tx = bx + (bwid - (int)strlen(t) * LM_FONT_W) / 2;
        wlwin_text(w, tx, (TOP_H - LM_FONT_H) / 2, t, 1, idle ? lm_theme.title_text_idle : lm_theme.on_colour, 0);
    }
    if (a->preview) {
        preview_scrollbar(a);
        goto status;
    }
    wlwin_fill(w, RAIL_W, TOP_H, gw - RAIL_W, w->bh - bars - TOP_H, COL_GUTTER_BG);
    int y0, y1;
    size_t x0, x1;
    sel_order(a, &y0, &x0, &y1, &x1);
    bool sel = has_sel(a);
    for (int r = 0; r < a->rows_visible; r++) {
        int ly = a->top + r;
        if (ly >= a->n)
            break;
        const struct line *l = &a->l[ly];
        int py = TOP_H + r * a->zt.ch;
        if (ly == a->cy)
            wlwin_fill(w, gw, py, w->bw - gw - RIGHT_W - lm_metrics.scrollbar_width, a->zt.ch, COL_LINE);
        char num[16];
        snprintf(num, sizeof(num), "%d", ly + 1);
        for (int k = 0; num[k] && !g_note; k++)
            wlwin_cell(w, gw - ((int)strlen(num) + 1 - k) * a->zt.cw, py, (uint32_t)num[k], a->zt.cw, a->zt.ch, ly == a->cy ? COL_FG : COL_DIM, false);
        int col = 0;
        size_t i = 0;
        int sel_from = -1, sel_to = -1;                                  /* selected display columns of this line */
        if (sel && ly >= y0 && ly <= y1) {
            sel_from = ly == y0 ? col_of(l, x0) : 0;
            sel_to = ly == y1 ? col_of(l, x1) : col_of(l, l->len) + 1;     /* a selected line end shows one extra cell */
        }
        if (sel_from >= 0)
            for (int c = LM_MAX(sel_from, a->left); c < sel_to && c < a->left + a->cols_visible; c++)
                wlwin_fill(w, gw + (c - a->left) * a->zt.cw, py, a->zt.cw, a->zt.ch, COL_SEL);
        while (i < l->len) {
            int nc = l->s[i] == '\t' ? (col / TAB_W + 1) * TAB_W : col + 1;
            if (l->s[i] != '\t' && col >= a->left && col < a->left + a->cols_visible)
                wlwin_cell(w, gw + (col - a->left) * a->zt.cw, py, cp_at(l, i), a->zt.cw, a->zt.ch, COL_FG, false);
            col = nc;
            i = cp_next(l, i);
            if (col >= a->left + a->cols_visible)
                break;
        }
        if (ly == a->cy) {                                               /* the cursor: a bar at the left edge of its cell */
            int cc = col_of(l, a->cx);
            if (cc >= a->left && cc < a->left + a->cols_visible)
                wlwin_fill(w, gw + (cc - a->left) * a->zt.cw, py, LM_MAX(2, a->zoom / 100 + 1), a->zt.ch, COL_ACCENT);
        }
    }
    paint_scrollbar(w, a);
status:;
    if (g_rail && !g_note)
        paint_rail(a);
    int sy = w->bh - STATUS_H;
    if (g_note) {
        if (a->msg[0] && now_ms() < a->msg_until) {                     /* a note has no status line: what it has to say comes as a strip over the bottom */
            wlwin_fill(w, 0, w->bh - 20, w->bw, 20, COL_BAR);
            wlwin_text(w, 6, w->bh - 18, a->msg, 1, COL_FG, w->bw - 12);
        }
        if (a->help)
            paint_help(w);
        return;
    }
    wlwin_fill(w, 0, sy, w->bw, STATUS_H, COL_BAR);
    char left[PATH_MAX + 64], right[64];
    const char *name = a->path[0] ? a->path : "untitled";
    snprintf(left, sizeof(left), "%s%s", a->modified ? "* " : "", name);
    snprintf(right, sizeof(right), "Ln %d, Col %d   %d lines", a->cy + 1, col_of(&a->l[a->cy], a->cx) + 1, a->n);
    if (a->preview) {
        int range = LM_MAX(md_height(a->md) - pv_view_h(a), 0);
        snprintf(right, sizeof(right), "Ctrl+M edits   %d%%", range ? a->md_scroll * 100 / range : 100);
        if (a->md_hover)
            snprintf(left, sizeof(left), "%s", a->md_hover);
    }
    bool showing = a->msg[0] && now_ms() < a->msg_until;
    wlwin_text(w, 6, sy + 2, showing ? a->msg : left, 1, showing ? COL_WARN : COL_FG, w->bw - 12 - (int)strlen(right) * LM_FONT_W);
    wlwin_text(w, w->bw - 6 - (int)strlen(right) * LM_FONT_W, sy + 2, right, 1, COL_DIM, 0);
    if (a->prompt != P_NONE && !card_on(a)) {
        static const char *const labels[] = { "", "Open: ", "Save as: ", "Find: ", "Go to line: " };
        static const char *const hints[] = { "", "Tab completes", "Tab completes", "Enter = next", "" };
        int py = sy - STATUS_H;
        wlwin_fill(w, 0, py, w->bw, STATUS_H, COL_BAR);
        wlwin_fill(w, 0, py, w->bw, 1, COL_ACCENT);
        int hw = (int)strlen(hints[a->prompt]) * LM_FONT_W;
        int lw = (int)strlen(labels[a->prompt]) * LM_FONT_W;
        if (a->pfresh == 1)                                              /* a suggested folder: shown selected (the file name's is a real selection) */
            wlwin_fill(w, 6 + lw, py + 1, LM_MIN((int)strlen(a->pbuf) * LM_FONT_W, w->bw - 12 - lw), LM_FONT_H, COL_SEL);
        wlwin_text(w, 6, py + 2, labels[a->prompt], 1, COL_FG, 0);
        wltext_paint(w, 6 + lw, py + 2, w->bw - 18 - hw - lw, a->pbuf, true, COL_FG, COL_SEL, COL_FG);
        if (hw)
            wlwin_text(w, w->bw - 6 - hw, py + 2, hints[a->prompt], 1, COL_DIM, 0);
    }
    if (a->help)
        paint_help(w);
    if (card_on(a))
        paint_card(a);
    preview_dump(a);
}

/* ---- mouse ----------------------------------------------------------------------------------------------------------------- */

static void place_from_pointer(struct app *a, bool extend)
{
    int gw = gutter_w(a);
    int row = LM_CLAMP(((int)a->py - TOP_H) / a->zt.ch, 0, a->rows_visible - 1);
    int y = LM_CLAMP(a->top + row, 0, a->n - 1);
    int col = a->left + (int)((a->px - gw) / a->zt.cw + (a->px < gw ? -1 : 0));
    move_to(a, y, off_at_col(&a->l[y], LM_MAX(col, 0)), extend);
    set_want(a);
}

static void menu_event(struct wlwin *w, int index)
{
    struct app *a = w->data;
    int kind = a->menu_kind;
    a->menu_kind = 0;
    if (index < 0)
        return;
    if (kind == 1 && a->prompt != P_NONE) {
        wltext_menu_do(&a->clip, a->pbuf, sizeof(a->pbuf), index);
        a->pfresh = 0;
    } else if (kind == 3 && a->md) {
        if (index == 0)
            cmd_copy(a, false);
        else if (index == 1)
            md_sel_all(a->md);
    } else if (kind == 2) {
        if (index == 0) {
            cmd_copy(a, true);
            set_title(a);
        } else if (index == 1) {
            cmd_copy(a, false);
        } else if (index == 2) {
            cmd_paste(a);
            set_title(a);
        } else if (index == 3) {
            a->ay = 0;
            a->ax = 0;
            move_to(a, a->n - 1, a->l[a->n - 1].len, true);
        }
    }
    w->redraw = true;
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        a->hover = btn_at(a, a->px, a->py);
        w->redraw = true;
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        a->hover = -1;
        rails_release(a);
        w->redraw = true;
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        if (btn_at(a, a->px, a->py) != a->hover) {
            a->hover = btn_at(a, a->px, a->py);
            w->redraw = true;
        }
        if (a->preview && a->md) {
            int cx;
            pv_col(a, &cx);
            const char *url = a->px >= RAIL_W && a->py >= TOP_H && a->px < w->bw - RIGHT_W - lm_metrics.scrollbar_width ?
                              md_link_at(a->md, (int)a->px - cx, (int)a->py - TOP_H + a->md_scroll) : NULL;
            a->md_hover = url;
            if (a->md_selecting) {
                int vh = pv_view_h(a), max = LM_MAX(md_height(a->md) - vh, 0);
                if (a->py < TOP_H)                                       /* the pointer is above / below the page: it scrolls on */
                    a->md_scroll = LM_CLAMP(a->md_scroll - 24, 0, max);
                else if (a->py >= TOP_H + vh)
                    a->md_scroll = LM_CLAMP(a->md_scroll + 24, 0, max);
                md_sel_drag(a->md, (int)a->px - cx, (int)a->py - TOP_H + a->md_scroll);
            }
            if (a->md_sb_drag) {
                int ty, th;
                pv_thumb(a, &ty, &th);
                int room = LM_MAX(pv_view_h(a) - th, 1), max = LM_MAX(md_height(a->md) - pv_view_h(a), 0);
                a->md_scroll = LM_CLAMP(((int)a->py - a->md_sb_grab - TOP_H) * max / room, 0, max);
            }
        } else if (a->sb_drag) {
            sb_drag_to(a, a->py);
        } else if (a->dragging) {
            place_from_pointer(a, true);
        }
        w->redraw = true;                                                /* (the thumb lights up under the pointer) */
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        a->clip.serial = args[0].u;
        if (args[2].u == 272 && args[3].u == 0)
            rails_release(a);                                           /* (the drop springs back) */
        if (args[2].u == 273 && args[3].u == 1) {                        /* BTN_RIGHT: the field's menu, or the text's */
            if (a->prompt != P_NONE) {
                a->menu_kind = 1;
                if (!wlwin_menu(w, wltext_menu(a->pbuf)))
                    a->menu_kind = 0;
            } else if (!a->help && a->preview && a->md && a->px >= RAIL_W && a->py >= TOP_H && a->py < TOP_H + pv_view_h(a) && a->px < w->bw - RIGHT_W - lm_metrics.scrollbar_width) {
                a->menu_kind = 3;
                if (!wlwin_menu(w, md_sel_active(a->md) ? "Copy\nSelect all" : "~Copy\nSelect all"))
                    a->menu_kind = 0;
                w->redraw = true;
            } else if (!a->help && !a->preview && a->px >= RAIL_W && a->py >= TOP_H && a->py < TOP_H + a->rows_visible * a->zt.ch && a->px < w->bw - RIGHT_W - lm_metrics.scrollbar_width) {
                if (!has_sel(a))
                    place_from_pointer(a, false);
                a->menu_kind = 2;
                if (!wlwin_menu(w, has_sel(a) ? "Cut\nCopy\nPaste\nSelect all" : "~Cut\n~Copy\nPaste\nSelect all"))
                    a->menu_kind = 0;
                w->redraw = true;
            }
            break;
        }
        if (card_on(a)) {                                               /* the card is modal: it takes every click */
            if (args[2].u == 272) {
                if (args[3].u == 1)
                    card_press(a, args[1].u);
                else
                    a->cbtn_down = -1;
            }
            w->redraw = true;
            break;
        }
        if (args[2].u != 272)
            break;
        if (args[3].u == 1 && a->help) {                                 /* a click closes the key list */
            a->help = false;
            w->redraw = true;
            break;
        }
        if (args[3].u == 1 && (a->py < TOP_H || a->px < RAIL_W || (RIGHT_W && a->px >= w->bw - RIGHT_W))) {          /* the button bar, or a rail */
            int b = btn_at(a, a->px, a->py);
            if (b >= 0 && g_rail) {
                if (b <= B_FIND)
                    wlrail_press(&a->ran[0], b, now_ms());
                else
                    wlrail_press(&a->ran[1], rail_hover(a, true), now_ms());
            }
            if (b >= 0)
                do_button(a, b);
            break;
        }
        if (a->preview && a->md) {
            if (args[3].u == 1) {
                int sbx = w->bw - RIGHT_W - lm_metrics.scrollbar_width, max = LM_MAX(md_height(a->md) - pv_view_h(a), 0);
                if (a->px >= sbx && a->py < TOP_H + pv_view_h(a)) {      /* the scroll bar: drag the thumb, a click beside it pages */
                    int ty, th;
                    pv_thumb(a, &ty, &th);
                    if (a->py >= ty && a->py < ty + th) {
                        a->md_sb_drag = true;
                        a->md_sb_grab = (int)a->py - ty;
                    } else {
                        a->md_scroll = LM_CLAMP(a->md_scroll + (a->py < ty ? -1 : 1) * (pv_view_h(a) - PV_STEP), 0, max);
                    }
                } else if (a->px >= RAIL_W && a->py >= TOP_H && a->py < TOP_H + pv_view_h(a)) {
                    int cx, px = 0, py;
                    pv_col(a, &cx);
                    px = (int)a->px - cx;
                    py = (int)a->py - TOP_H + a->md_scroll;
                    a->md_press_link = md_link_at(a->md, px, py) != NULL;
                    a->md_clicks = args[1].u - a->md_click_ms < (uint32_t)lm_metrics.double_click_ms ? a->md_clicks + 1 : 1;
                    a->md_click_ms = args[1].u;
                    if (a->md_clicks == 2)
                        md_sel_word(a->md, px, py);
                    else if (a->md_clicks >= 3)
                        md_sel_line(a->md, px, py);
                    else
                        md_sel_begin(a->md, px, py);
                    a->md_selecting = a->md_clicks == 1;                 /* (a word or a line is not extended by dragging) */
                }
            } else {
                a->md_sb_drag = false;
                if (a->md_selecting || a->md_press_link) {
                    int cx;
                    pv_col(a, &cx);
                    const char *url = md_link_at(a->md, (int)a->px - cx, (int)a->py - TOP_H + a->md_scroll);
                    if (a->md_press_link && url && a->md_clicks == 1 && !md_sel_active(a->md))
                        open_link(a, url);                               /* a click on a link (not a drag over it) */
                }
                a->md_selecting = false;
                a->md_press_link = false;
            }
            w->redraw = true;
            break;
        }
        if (args[3].u == 1 && a->px >= w->bw - RIGHT_W - lm_metrics.scrollbar_width && a->px < w->bw - RIGHT_W && a->py < TOP_H + sb_track_h(a)) {
            int ty, th;                                                  /* the scroll bar: drag the thumb, or a click beside it pages */
            sb_thumb(a, &ty, &th);
            if (a->py >= ty && a->py < ty + th) {
                a->sb_drag = true;
                a->sb_grab = (int)a->py - ty;
            } else {
                a->top = LM_CLAMP(a->top + (a->py < ty ? -1 : 1) * LM_MAX(a->rows_visible - 1, 1), 0, LM_MAX(a->n - 1, 0));
            }
            w->redraw = true;
            break;
        }
        if (args[3].u == 1 && a->py >= TOP_H + a->rows_visible * a->zt.ch)    /* the status line is not text */
            break;
        if (args[3].u == 1) {
            bool dbl = args[1].u - a->last_click_ms < (uint32_t)lm_metrics.double_click_ms;
            a->last_click_ms = args[1].u;
            place_from_pointer(a, (w->mods & LM_MOD_SHIFT) != 0);
            if (dbl) {                                                   /* double click: the word under the pointer */
                const struct line *l = &a->l[a->cy];
                size_t s = a->cx, e = a->cx;
                while (s > 0 && is_word((unsigned char)l->s[cp_prev(l, s)]))
                    s = cp_prev(l, s);
                while (e < l->len && is_word((unsigned char)l->s[e]))
                    e = cp_next(l, e);
                a->ay = a->cy;
                a->ax = s;
                a->cx = e;
                a->dragging = false;
            } else {
                a->dragging = true;
            }
        } else {
            a->dragging = false;
            a->sb_drag = false;
        }
        w->redraw = true;
        break;
    case LM_WL_POINTER_EVT_AXIS:
        if (card_on(a)) {
            if (card_files(a) && args[1].u == 0)
                a->ctop = LM_MAX(a->ctop + (args[2].f > 0 ? 3 : -3), 0);
            w->redraw = true;
        } else if (a->preview && a->md && args[1].u == 0) {
            int lines = (int)(lm_fixed_to_double(args[2].f) / 15.0 * 3.0);
            if (lines == 0)
                lines = args[2].f > 0 ? 1 : -1;
            a->md_scroll = LM_CLAMP(a->md_scroll + lines * (PV_STEP / 2), 0, LM_MAX(md_height(a->md) - pv_view_h(a), 0));
            w->redraw = true;
        } else if (args[1].u == 0 && (w->mods & LM_MOD_CTRL)) {          /* Ctrl + wheel: the text size */
            set_zoom(a, args[2].f > 0 ? -1 : 1);
        } else if (args[1].u == 0) {
            int lines = (int)(lm_fixed_to_double(args[2].f) / 15.0 * 3.0);
            if (lines == 0)
                lines = args[2].f > 0 ? 1 : -1;
            a->top = LM_CLAMP(a->top + lines, 0, a->n - 1);
            w->redraw = true;
        }
        break;
    }
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    switch (op) {
    case LM_WL_KEYBOARD_EVT_KEY: {
        a->clip.serial = args[0].u;
        uint32_t code = args[2].u;
        if (args[3].u == 0) {
            if (a->rep_code == code)
                a->rep_code = 0;
            break;
        }
        uint32_t ks = lm_keymap_keysym(code, w->mods);
        handle_key(a, code, ks);
        w->redraw = true;
        if (ks && !(w->mods & (LM_MOD_CTRL | LM_MOD_ALT)) && a->rep_rate > 0) {
            a->rep_code = code;
            a->rep_next = now_ms() + a->rep_delay;
        } else {
            a->rep_code = 0;
        }
        break;
    }
    case LM_WL_KEYBOARD_EVT_REPEAT_INFO:
        a->rep_rate = args[0].i;
        a->rep_delay = args[1].i;
        break;
    }
}

static bool close_event(struct wlwin *w)
{
    struct app *a = w->data;
    if (g_note)                                                         /* a note is saved when it is closed (the loop's end does it) */
        return true;
    if (a->modified && !a->quit_armed) {
        a->quit_armed = true;
        msg(a, "Unsaved changes: save first (Ctrl+S), or close again to discard them");
        return false;
    }
    return true;
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    struct app *a = w->data;
    wlclip_global(&a->clip, w, name, iface, ver);
}

/* ---- self test --------------------------------------------------------------------------------------------------------------- */

static int bad;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "selftest: line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); bad++; } } while (0)

static char *whole(const struct app *a)
{
    return range_text(a, 0, 0, a->n - 1, a->l[a->n - 1].len, NULL);
}

static int selftest(void)
{
    static struct app a;
    doc_clear(&a);
    type_text(&a, "hello", 5, true);
    type_text(&a, " world", 6, true);                                  /* typing joins one undo step */
    CHECK(a.nundo == 1, "undo steps %d", a.nundo);
    char *t = whole(&a);
    CHECK(!strcmp(t, "hello world"), "'%s'", t);
    free(t);
    CHECK(undo(&a, false) && a.n == 1 && a.l[0].len == 0, "undo of the typing run");
    CHECK(undo(&a, true), "redo");
    t = whole(&a);
    CHECK(!strcmp(t, "hello world"), "after redo '%s'", t);
    free(t);

    doc_clear(&a);
    insert_at_cursor(&a, "one\ntwo\nthree", 13);                       /* a paste with line ends */
    CHECK(a.n == 3 && a.cy == 2 && a.cx == 5, "paste: %d lines, cursor %d:%zu", a.n, a.cy, a.cx);
    delete_range(&a, 0, 1, 2, 2);                                       /* "o" + "ree" */
    t = whole(&a);
    CHECK(!strcmp(t, "oree") && a.n == 1, "delete across lines '%s'", t);
    free(t);

    doc_clear(&a);
    insert_at_cursor(&a, "a\tb \xc3\xb5\xe2\x82\xac z", 11);            /* tab, o-tilde (2 bytes), euro (3 bytes) */
    CHECK(col_of(&a.l[0], 2) == 4, "tab column %d", col_of(&a.l[0], 2));
    CHECK(col_of(&a.l[0], a.l[0].len) == 10, "columns %d", col_of(&a.l[0], a.l[0].len));
    size_t o = a.l[0].len;
    CHECK(cp_prev(&a.l[0], o) == o - 1 && cp_prev(&a.l[0], o - 1) == o - 2, "utf8 steps back over a space and a letter");
    CHECK(off_at_col(&a.l[0], 4) == 2 && off_at_col(&a.l[0], 0) == 0, "offset at a column");

    doc_clear(&a);
    insert_at_cursor(&a, "alpha beta  gamma", 17);
    a.cy = 0;
    a.cx = 17;
    word_left(&a, false);
    CHECK(a.cx == 12, "word left: %zu", a.cx);
    word_left(&a, false);
    CHECK(a.cx == 6, "word left again: %zu", a.cx);
    a.cx = 0;
    word_right(&a, false);
    CHECK(a.cx == 5, "word right: %zu", a.cx);

    doc_clear(&a);
    insert_at_cursor(&a, "foo bar\nbar baz\nfoo", 19);
    int fy;
    size_t fx;
    CHECK(find_from(&a, "bar", 0, 0, &fy, &fx) && fy == 0 && fx == 4, "find first");
    CHECK(find_from(&a, "bar", 0, 5, &fy, &fx) && fy == 1 && fx == 0, "find next line");
    CHECK(find_from(&a, "foo", 2, 1, &fy, &fx) && fy == 0 && fx == 0, "after the last match: wraps to the first");
    CHECK(find_from(&a, "foo", 2, 0, &fy, &fx) && fy == 2 && fx == 0, "a match at the position itself counts");
    CHECK(find_from(&a, "bar", 1, 1, &fy, &fx) && fy == 0 && fx == 4, "find wraps around");
    CHECK(!find_from(&a, "nope", 0, 0, &fy, &fx), "no match");

    /* files: CRLF kept, no final newline kept, binary and invalid UTF-8 refused */
    char path[] = "/tmp/mellagui-edit-selftest-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp");
    if (fd >= 0) {
        close(fd);
        FILE *f = fopen(path, "wb");
        fputs("one\r\ntwo\r\nthree", f);
        fclose(f);
        char err[200];
        CHECK(load_file(&a, path, err, sizeof(err)) == 0 && a.n == 3 && a.crlf && !a.final_nl, "load crlf: %s", err);
        a.l[1].s[0] = 'T';
        CHECK(save_file(&a, path, err, sizeof(err)) == 0, "save: %s", err);
        char buf[64] = "";
        f = fopen(path, "rb");
        size_t got = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        CHECK(got == 15 && !memcmp(buf, "one\r\nTwo\r\nthree", 15), "round trip: %zu bytes", got);
        f = fopen(path, "wb");
        fwrite("a\0b", 1, 3, f);
        fclose(f);
        CHECK(load_file(&a, path, err, sizeof(err)) < 0 && strstr(err, "binary"), "binary refused: %s", err);
        f = fopen(path, "wb");
        fwrite("ok \xff\xfe", 1, 5, f);
        fclose(f);
        CHECK(load_file(&a, path, err, sizeof(err)) < 0 && strstr(err, "UTF-8"), "invalid UTF-8 refused: %s", err);
        f = fopen(path, "wb");
        fputs("x\n", f);
        fclose(f);
        CHECK(load_file(&a, path, err, sizeof(err)) == 0 && a.n == 1 && a.final_nl, "final newline: %s", err);
        unlink(path);
    }
    CHECK(valid_utf8((const unsigned char *)"\xc3\xb5\xe2\x82\xac\xf0\x9f\x98\x80", 9), "valid sequences");
    CHECK(!valid_utf8((const unsigned char *)"\xc3", 1) && !valid_utf8((const unsigned char *)"\xe2\x82", 2), "truncated sequences");
    CHECK(!valid_utf8((const unsigned char *)"\xc0\xaf", 2), "overlong form");
    puts(bad ? "selftest FAILED" : "selftest ok");
    char tdir[] = "/tmp/mellagui-edit-selftest-XXXXXX";                  /* names in the prompts */
    if (mkdtemp(tdir)) {
        char p[PATH_MAX], buf[512], list[160];
        snprintf(p, sizeof(p), "%s/alpha.txt", tdir);
        FILE *f = fopen(p, "w");
        if (f)
            fclose(f);
        snprintf(p, sizeof(p), "%s/alpine.txt", tdir);
        f = fopen(p, "w");
        if (f)
            fclose(f);
        snprintf(p, sizeof(p), "%s/sub", tdir);
        mkdir(p, 0755);
        snprintf(buf, sizeof(buf), "%s/a", tdir);
        CHECK(lm_complete_path("/", buf, sizeof(buf), true, list, sizeof(list)) == 2, "two names start with a");
        CHECK(strlen(buf) > 3 && !strcmp(buf + strlen(buf) - 4, "/alp") && strstr(list, "alpha.txt  alpine.txt"), "common prefix '%s' list '%s'", buf, list);
        snprintf(buf, sizeof(buf), "%s/su", tdir);
        CHECK(lm_complete_path("/", buf, sizeof(buf), true, list, sizeof(list)) == 1 && strstr(buf, "/sub/"), "a folder gets a slash: '%s'", buf);
        snprintf(buf, sizeof(buf), "sub");                              /* relative to the base folder */
        CHECK(lm_complete_path(tdir, buf, sizeof(buf), true, list, sizeof(list)) == 1 && !strcmp(buf, "sub/"), "relative: '%s'", buf);
        snprintf(buf, sizeof(buf), "%s/zzz", tdir);
        CHECK(lm_complete_path("/", buf, sizeof(buf), true, list, sizeof(list)) == 0, "no candidates");
        snprintf(buf, sizeof(buf), "%s/", tdir);
        CHECK(lm_complete_path("/", buf, sizeof(buf), false, list, sizeof(list)) == 3 && buf[strlen(buf) - 1] == '/', "list only, three entries");
        char r[PATH_MAX];
        lm_resolve_path("/base", "x/y", r, sizeof(r));
        CHECK(!strcmp(r, "/base/x/y"), "relative name '%s'", r);
        lm_resolve_path("/", "x", r, sizeof(r));
        CHECK(!strcmp(r, "/x"), "relative to / '%s'", r);
        lm_resolve_path("/base", "/abs", r, sizeof(r));
        CHECK(!strcmp(r, "/abs"), "absolute name '%s'", r);
        snprintf(p, sizeof(p), "%s/sub", tdir);
        rmdir(p);
        snprintf(p, sizeof(p), "%s/alpha.txt", tdir);
        unlink(p);
        snprintf(p, sizeof(p), "%s/alpine.txt", tdir);
        unlink(p);
        rmdir(tdir);
    }
    /* Markdown */
    {
        const char *src = "# Title\n\nSome **bold** and *it* and `code` with [a link](http://x.y/z) and https://bare.example/a.\n\n"
                          "- one\n- two\n  - nested\n- [x] done\n\n1. a\n2. b\n\n> quote\n> more\n\n```\ncode block\n  indented\n```\n\n"
                          "| a | b |\n|---|:-:|\n| 1 | **2** |\n\n---\n\nSetext\n======\n\n![pic](nofile.png)\n\n    four spaces\n";
        struct md_doc *d = md_parse(src, strlen(src));
        md_layout(d, 400, NULL);
        CHECK(md_count(d, MD_N_HEADINGS) == 2, "markdown: %d headings", md_count(d, MD_N_HEADINGS));
        CHECK(md_count(d, MD_N_CODE) == 2, "markdown: %d code blocks", md_count(d, MD_N_CODE));
        CHECK(md_count(d, MD_N_TABLES) == 1, "markdown: %d tables", md_count(d, MD_N_TABLES));
        CHECK(md_count(d, MD_N_LINKS) == 3, "markdown: %d addresses", md_count(d, MD_N_LINKS));       /* the link, the bare one, the picture */
        CHECK(md_count(d, MD_N_BLOCKS) == 15, "markdown: %d blocks", md_count(d, MD_N_BLOCKS));
        int h400 = md_height(d);
        md_layout(d, 400, NULL);
        CHECK(md_height(d) == h400, "markdown: the layout is not repeatable");
        md_layout(d, 160, NULL);
        CHECK(md_height(d) > h400, "markdown: a narrow page is taller (%d, %d)", md_height(d), h400);
        char *buf = NULL;
        size_t bl = 0;
        FILE *mf = open_memstream(&buf, &bl);
        md_dump(d, mf);
        fclose(mf);
        CHECK(strstr(buf, "link=http://x.y/z") && strstr(buf, "link=https://bare.example/a") && strstr(buf, "[image: pic]"), "markdown: runs\n%s", buf);
        CHECK(strstr(buf, "\"bold\"") && strstr(buf, "\"nested\"") && strstr(buf, "\"four spaces\""), "markdown: words\n%s", buf);
        free(buf);
        md_free(d);
    }
    return bad ? 1 : 0;
}

/* ---- main ------------------------------------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    wlwin_hidpi();                      /* draws through the wlwin_* calls only: sharp on a scaled screen */
    lm_font_ui_cells(true);             /* this program lays its text out in columns of characters */
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();
    const char *rail_env = getenv("MELLAGUI_EDIT_RAIL");
    if (rail_env && !strcmp(rail_env, "0"))
        g_rail = false;
    for (int i = 1; i < argc; i++)                                      /* --bar: the old bar of buttons above the text; --rail: (the default) */
        if (!strcmp(argv[i], "--bar") || !strcmp(argv[i], "--rail")) {
            g_rail = argv[i][2] == 'r';
            memmove(&argv[i], &argv[i + 1], (size_t)(argc - i) * sizeof(argv[0]));
            argc--;
            break;
        }
    if (argc == 2 && !strcmp(argv[1], "--notes"))
        return notes_open_all();
    const char *note_file = NULL;
    if (argc == 3 && !strcmp(argv[1], "--note")) {                    /* one sticky note */
        note_file = argv[2];
        g_note = true;
        argc = 1;
        if (!note_lock(note_file))
            return 0;                                                   /* it is open in a window already */
    }
    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        fprintf(stderr, "usage: mellagui-edit [--bar] [FILE | --note FILE | --notes | --selftest]\n");
        return 2;
    }
    static struct app a;
    doc_clear(&a);
    a.final_nl = true;
    a.hover = -1;
    a.cbtn_down = -1;
    a.csel = -1;
    wlrail_init(&a.ran[0]);
    wlrail_init(&a.ran[1]);
    a.zoom = lm_zoom_conf_load("editor");
    if (a.zoom == 0)
        a.zoom = LM_ZOOM_DEFAULT;
    lm_zoom_build(&a.zt, a.zoom);
    a.rep_rate = 25;
    a.rep_delay = 400;
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event,
                                          .keyboard = keyboard_event, .close = close_event, .menu = menu_event };
    if (wlwin_connect(&a.win, "mellagui-edit", &ops, &a) < 0)
        return 1;
    if (wlwin_open(&a.win, g_note ? "Note" : "Editor", g_note ? "mellagui.note" : "mellagui.edit", (g_note ? 300 : 760) * a.zoom / 100,
                   (g_note ? 320 : 520) * a.zoom / 100, g_note ? 160 : 240, g_note ? 120 : 120) < 0)
        return 1;
    wlclip_attach(&a.clip, &a.win);
    if (note_file) {
        do_open(&a, note_file);
        note_paper_load(&a);
        a.msg[0] = 0;
    }
    if (argc == 2) {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s", argv[1]);
        do_open(&a, full);
    }
    set_title(&a);
    if (argc == 1 && !g_note)
        msg(&a, "Welcome! F1 shows the keys, Ctrl+O opens a file, Ctrl+S saves");
    while (a.win.running) {
        int timeout = 250;
        if (a.rep_code) {
            int64_t wait = a.rep_next - now_ms();
            timeout = (int)LM_CLAMP(wait, 1, 250);
        }
        if (g_rail && !g_note && rails_step(&a)) {                         /* the drops are moving: a frame every 16 ms */
            a.win.redraw = true;
            timeout = LM_MIN(timeout, 16);
        }
        if (!wlwin_step(&a.win, timeout))
            break;
        if (a.rep_code && now_ms() >= a.rep_next) {                      /* key repeat */
            handle_key(&a, a.rep_code, lm_keymap_keysym(a.rep_code, a.win.mods));
            a.rep_next = now_ms() + (a.rep_rate > 0 ? 1000 / a.rep_rate : 40);
            a.win.redraw = true;
        }
        if (g_note && a.modified && a.path[0] && now_ms() - a.edit_ms >= 700) {      /* a note saves itself a moment after the last key */
            do_save(&a, a.path);
            a.msg[0] = 0;
            set_title(&a);
        }
        if (g_note && now_ms() - a.delete_ms >= 3000)
            a.delete_armed = false;
        if (a.conf_dirty && now_ms() - a.conf_time >= 400) {              /* a burst of zoom steps writes the file once */
            lm_zoom_conf_save("editor", a.zoom);
            a.conf_dirty = false;
        }
        if (a.msg[0] && now_ms() >= a.msg_until) {
            a.msg[0] = 0;
            a.quit_armed = false;                                        /* the question has timed out, too */
            a.win.redraw = true;
        }
    }
    if (g_note && a.modified && a.path[0])
        do_save(&a, a.path);
    if (g_note && a.path[0] && a.n == 1 && a.l[0].len == 0) {           /* an empty note is not kept */
        char side[PATH_MAX + 8];
        snprintf(side, sizeof(side), "%s.col", a.path);
        unlink(a.path);
        unlink(side);
    }
    if (g_note && note_file) {
        char lock[PATH_MAX + 8];
        snprintf(lock, sizeof(lock), "%s.lock", note_file);
        unlink(lock);
    }
    if (a.conf_dirty)
        lm_zoom_conf_save("editor", a.zoom);
    wlwin_destroy(&a.win);
    return 0;
}
