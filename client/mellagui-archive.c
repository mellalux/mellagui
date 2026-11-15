/*
 * mellagui-archive - the archiver: look into an archive, extract all or some of it, make new ones.
 *
 *   mellagui-archive [ARCHIVE]            the window (no argument: a start page with Open / New)
 *   mellagui-archive --new PATH...        the window in "new archive" mode with these files and folders already added
 *   mellagui-archive --list ARCHIVE       what is in it (kind, then type TAB name TAB size TAB mode TAB time TAB link), no window
 *   mellagui-archive --test ARCHIVE       read everything and check the checksums
 *   mellagui-archive --extract ARCHIVE [DIR] [--overwrite]    extract into DIR (default: a folder named like the archive, beside it)
 *   mellagui-archive --create OUT.zip|.tar|.tar.gz|.tgz PATH... [--level N]
 *   mellagui-archive --selftest
 *
 * The engine is client/arcio.[ch] (ZIP, TAR, tar.gz / xz / bz2 / zst, ar, deb, rpm: open and extract; 7z: open and extract; make ZIP, TAR, TAR.GZ). The window lists
 * the folder you are in; Enter / a double click goes into a folder (or opens a file: it is extracted to the cache and handed to xdg-open), Backspace goes up,
 * Space / click / Ctrl+click / Shift+click select, Ctrl+A all, E extracts the selection (or everything when nothing is selected), Ctrl+O opens another
 * archive, Ctrl+N starts a new one (A adds, Delete removes, F changes the format, Enter makes it), T tests, R switches "Replace existing files", Esc stops a job,
 * closes a question, then the window. Extraction and creation run in a thread of their own with a progress bar.
 * $MELLAGUI_ARCHIVE_DUMP=FILE writes the state after every paint (tests).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "arcio.h"
#include "deflate.h"
#include "font.h"
#include "keymap.h"
#include "pathcomp.h"
#include "protocol.h"
#include "theme.h"
#include "uikit.h"
#include "util.h"
#include "wlc.h"
#include "wlcard.h"
#include "wlclip.h"
#include "wlrail.h"
#include "wltext.h"
#include "wlwin.h"

#define PAD 10
#define HEAD_H 22
#define ROW_H 18
#define FOOT_H 46
#define PATH_MAX_LEN 4096

struct row {
    char *name;
    bool dir;
    int64_t size, packed, mtime;
    int idx;                            /* the entry (file, or a folder the archive lists), -1 for a folder that only exists through its files */
    int items;                          /* a folder: the entries below it */
};

enum { B_OPEN = 1, B_NEW, B_EXTRACT, B_EXTRACT_ALL, B_TEST, B_ADD, B_REMOVE, B_FORMAT, B_CREATE, B_CLOSE, B_UP, B_REPLACE, B_SHOW, B_CANCEL_JOB };
enum { P_NONE, P_OPEN, P_EXTRACT, P_ADD, P_CREATE };
enum { J_NONE, J_EXTRACT, J_TEST, J_CREATE };
enum { SORT_NAME, SORT_SIZE, SORT_TIME };


struct centry {
    char name[256];
    bool dir;
};

struct app {
    struct wlwin win;
    struct wlclip clip;
    struct aio *arc;
    char arc_path[PATH_MAX_LEN];
    char cwd[PATH_MAX_LEN];             /* the folder shown, "" or "a/b/" */
    struct row *rows;
    int nrows;
    uint8_t *sel;                       /* per row */
    int cur, top, anchor;
    bool sel_by_cursor;                 /* Extract takes the row under the cursor when nothing is marked (Extract all does not) */
    int sort, sort_desc;
    /* new archive mode */
    bool newmode;
    char **items;
    int nitems, capitems, item_cur;
    int fmt;
    /* the question card; for "Open" it has a list of the folder that filters while you type (like the editor's) */
    int prompt;
    struct centry *cent;
    int cn, ccap, csel, ctop;
    char cdir[PATH_MAX_LEN];
    uint32_t cclick_ms;
    int csb_drag;                       /* the list's scroll bar thumb is held: the offset inside it, -1 = no */
    char cmsg[200];
    char field[PATH_MAX_LEN];
    /* the job */
    int job;
    pthread_t thread;
    atomic_int job_done;
    struct aio_progress prog;
    struct aio_report rep;
    char job_err[300], job_dest[PATH_MAX_LEN], job_out[PATH_MAX_LEN];
    int job_rc;
    int *job_sel, job_nsel;
    bool job_overwrite;
    bool overwrite;
    char last_dest[PATH_MAX_LEN];       /* where the last extraction went (Show folder) */
    char status[400];
    char title[4200];
    bool status_err;
    double px, py;
    uint32_t last_click_ms;
    int last_click_row;
    int drag_bar;                       /* dragging the scroll bar thumb: the y offset inside it, -1 no */
    struct wlrail_anim ran;             /* the springs of the rail */
    int rail_ids[WLRAIL_MAX], rail_n;
    bool rail_idle[WLRAIL_MAX];
    int hover_btn, hover_card;          /* hover_btn: the id of the rail button under the pointer */
    const char *dump;
};

static struct app g_app;

/* ---- small helpers ---- */
static void set_status(struct app *a, bool err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_status(struct app *a, bool err, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(a->status, sizeof(a->status), fmt, ap);
    __builtin_va_end(ap);
    a->status_err = err;
    a->win.redraw = true;
}

static void human(int64_t n, char *o, size_t cap)
{
    if (n < 0) {
        o[0] = 0;
        return;
    }
    static const char *const u[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)n;
    int k = 0;
    while (v >= 1024.0 && k < 4) {
        v /= 1024.0;
        k++;
    }
    if (k == 0)
        snprintf(o, cap, "%lld B", (long long)n);
    else
        snprintf(o, cap, v < 10 ? "%.1f %s" : "%.0f %s", v, u[k]);
}
static void time_text(int64_t t, char *o, size_t cap)
{
    o[0] = 0;
    if (t <= 0)
        return;
    time_t tt = (time_t)t;
    struct tm tm;
    if (localtime_r(&tt, &tm))
        strftime(o, cap, "%Y-%m-%d %H:%M", &tm);
}
static const char *base_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void expand_home(const char *in, char *out, size_t cap)
{
    if (in[0] == '~' && (in[1] == '/' || !in[1]) && getenv("HOME"))
        snprintf(out, cap, "%s%s", getenv("HOME"), in + 1);
    else
        snprintf(out, cap, "%s", in);
}

/* The name without the archive extensions: "a.tar.gz" -> "a". */
static void stem_of(const char *path, char *out, size_t cap)
{
    snprintf(out, cap, "%s", base_of(path));
    static const char *const exts[] = { ".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst", ".tgz", ".txz", ".tbz2", ".zip", ".tar", ".gz", ".xz", ".bz2", ".zst", ".deb", ".rpm", ".7z", ".jar", ".apk", ".a", ".epub", ".docx", ".odt" };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t n = strlen(out), l = strlen(exts[i]);
        if (n > l && !strcasecmp(out + n - l, exts[i])) {
            out[n - l] = 0;
            break;
        }
    }
    if (!out[0])
        snprintf(out, cap, "archive");
}

static void default_dest(const struct app *a, char *out, size_t cap)
{
    char dir[PATH_MAX_LEN], stem[256];
    snprintf(dir, sizeof(dir), "%s", a->arc_path);
    char *s = strrchr(dir, '/');
    if (s)
        *s = 0;
    else
        snprintf(dir, sizeof(dir), ".");
    stem_of(a->arc_path, stem, sizeof(stem));
    snprintf(out, cap, "%s/%s", dir, stem);
}

static void set_title(struct app *a)
{
    if (!a->win.toplevel)
        return;
    char t[PATH_MAX_LEN + 32];
    if (a->newmode)
        snprintf(t, sizeof(t), "New archive - Archiver");
    else if (a->arc)
        snprintf(t, sizeof(t), "%s - Archiver", base_of(a->arc_path));
    else
        snprintf(t, sizeof(t), "Archiver");
    if (!strcmp(t, a->title))
        return;
    snprintf(a->title, sizeof(a->title), "%s", t);
    WLC_SEND(a->win.c, a->win.toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = t });
}

/* ---- the folder listing ---- */
static void free_rows(struct app *a)
{
    for (int i = 0; i < a->nrows; i++)
        free(a->rows[i].name);
    free(a->rows);
    free(a->sel);
    a->rows = NULL;
    a->sel = NULL;
    a->nrows = 0;
}

static const char *norm_path(const char *p)
{
    while (*p == '/' || (p[0] == '.' && p[1] == '/'))
        p += *p == '/' ? 1 : 2;
    return p;
}

static int row_cmp_key;
static int row_cmp_desc;

static int row_cmp(const void *x, const void *y)
{
    const struct row *a = x, *b = y;
    if (a->dir != b->dir)
        return a->dir ? -1 : 1;                                             /* folders first, whatever the sort */
    int r = 0;
    if (row_cmp_key == SORT_SIZE)
        r = a->size < b->size ? -1 : a->size > b->size;
    else if (row_cmp_key == SORT_TIME)
        r = a->mtime < b->mtime ? -1 : a->mtime > b->mtime;
    if (r == 0)
        r = strcasecmp(a->name, b->name);
    return row_cmp_desc ? -r : r;
}

static void build_rows(struct app *a)
{
    free_rows(a);
    if (!a->arc)
        return;
    size_t cl = strlen(a->cwd);
    int cap = 0;
    for (int i = 0; i < a->arc->n; i++) {
        const struct aio_entry *e = &a->arc->e[i];
        const char *p = norm_path(e->path ? e->path : "");
        if (strncmp(p, a->cwd, cl))
            continue;
        const char *rest = p + cl;
        if (!*rest)
            continue;
        const char *sl = strchr(rest, '/');
        bool dir = sl != NULL || e->type == AIO_DIR;
        size_t nl = sl ? (size_t)(sl - rest) : strlen(rest);
        if (nl == 0)
            continue;
        bool direct = !sl || !sl[1];                                        /* the entry is the folder itself / the file itself, not something below it */
        int found = -1;
        for (int k = 0; k < a->nrows; k++)
            if (strlen(a->rows[k].name) == nl && !strncmp(a->rows[k].name, rest, nl) && a->rows[k].dir == dir) {
                found = k;
                break;
            }
        if (found < 0) {
            if (a->nrows == cap) {
                cap = cap ? cap * 2 : 64;
                a->rows = realloc(a->rows, (size_t)cap * sizeof(struct row));
            }
            struct row *r = &a->rows[a->nrows++];
            memset(r, 0, sizeof(*r));
            r->name = strndup(rest, nl);
            r->dir = dir;
            r->idx = -1;
            r->size = dir ? 0 : -1;
            r->packed = -1;
            found = a->nrows - 1;
        }
        struct row *r = &a->rows[found];
        if (dir) {
            r->items++;
            if (direct) {
                r->idx = i;
                if (e->mtime)
                    r->mtime = e->mtime;
            } else if (e->type == AIO_FILE && e->size > 0)
                r->size += e->size;
            if (!r->mtime && e->mtime)
                r->mtime = e->mtime;
        } else {
            r->idx = i;
            r->size = e->size;
            r->packed = e->packed;
            r->mtime = e->mtime;
        }
    }
    row_cmp_key = a->sort;
    row_cmp_desc = a->sort_desc;
    qsort(a->rows, (size_t)a->nrows, sizeof(struct row), row_cmp);
    a->sel = calloc((size_t)a->nrows + 1, 1);
    a->cur = LM_CLAMP(a->cur, 0, LM_MAX(a->nrows - 1, 0));
    a->top = 0;
    a->anchor = 0;
}

static int list_y(void) { return HEAD_H; }

static int rows_visible(const struct app *a)
{
    return LM_MAX((a->win.bh - list_y() - FOOT_H) / ROW_H, 1);
}

static int list_count(const struct app *a)
{
    return a->newmode ? a->nitems : a->nrows;
}

static void clamp_view(struct app *a)
{
    int n = list_count(a), vis = rows_visible(a);
    int *cur = a->newmode ? &a->item_cur : &a->cur;
    *cur = LM_CLAMP(*cur, 0, LM_MAX(n - 1, 0));
    if (*cur < a->top)
        a->top = *cur;
    if (*cur >= a->top + vis)
        a->top = *cur - vis + 1;
    a->top = LM_CLAMP(a->top, 0, LM_MAX(n - vis, 0));
}

static void close_archive(struct app *a)
{
    free_rows(a);
    aio_close(a->arc);
    a->arc = NULL;
    a->cwd[0] = 0;
    a->arc_path[0] = 0;
}

static bool open_archive(struct app *a, const char *path)
{
    char err[300] = "", full[PATH_MAX_LEN];
    expand_home(path, full, sizeof(full));
    struct aio *n = NULL;
    int r = aio_open(full, &n, err, sizeof(err));
    if (r < 0) {
        set_status(a, true, "%s", r == -1 ? "That is not an archive this program can read." : err);
        return false;
    }
    close_archive(a);
    a->arc = n;
    a->newmode = false;
    char *rp = realpath(full, NULL);
    snprintf(a->arc_path, sizeof(a->arc_path), "%s", rp ? rp : full);
    free(rp);
    build_rows(a);
    char sz[32];
    human(n->total_size, sz, sizeof(sz));
    set_status(a, false, "%s: %d entries, %s unpacked%s%s", n->kind, n->n, sz, n->note[0] ? ". " : "", n->note);
    if (!n->extractable)
        set_status(a, false, "%s: %d entries. %s", n->kind, n->n, n->why_not);
    return true;
}

/* ---- the rail (the same side rail of round buttons as the file manager's and the editor's) ---- */
static void rail_icon_text(struct wlwin *w, int cx, int cy, const char *t, uint32_t ink)
{
    wlwin_text(w, cx - lm_font_text_width(t) / 2, cy - LM_FONT_H / 2 - 1, t, 1, ink, 0);
}

static void ic_all(struct wlwin *w, int cx, int cy, uint32_t ink, uint32_t bg) { (void)bg; rail_icon_text(w, cx, cy, "All", ink); }
static void ic_ok(struct wlwin *w, int cx, int cy, uint32_t ink, uint32_t bg) { (void)bg; rail_icon_text(w, cx, cy, "Ok", ink); }
static void ic_zip(struct wlwin *w, int cx, int cy, uint32_t ink, uint32_t bg) { (void)bg; rail_icon_text(w, cx, cy, "Zip", ink); }
static void ic_tar(struct wlwin *w, int cx, int cy, uint32_t ink, uint32_t bg) { (void)bg; rail_icon_text(w, cx, cy, "Tar", ink); }
static void ic_tgz(struct wlwin *w, int cx, int cy, uint32_t ink, uint32_t bg) { (void)bg; rail_icon_text(w, cx, cy, "Tgz", ink); }
static void ic_stop(struct wlwin *w, int cx, int cy, uint32_t ink, uint32_t bg) { (void)bg; rail_icon_text(w, cx, cy, "X", ink); }

static void rail_add(struct app *a, struct wlrail *L, int id, const char *tip, void (*icon)(struct wlwin *, int, int, uint32_t, uint32_t), uint32_t base, uint32_t lite,
                     bool enabled, bool bottom)
{
    if (L->n >= WLRAIL_MAX)
        return;
    a->rail_ids[L->n] = id;
    a->rail_idle[L->n] = !enabled;
    L->it[L->n++] = (struct wlrail_item){ .tip = tip, .icon = icon, .base = base, .lite = lite, .show = true, .idle = !enabled, .bottom = bottom };
}

/* The rail for the present state; a->rail_ids[i] is the id of item i. */
static void files_rail_like(struct app *a, struct wlrail *L)
{
    memset(L, 0, sizeof(*L));
    L->x = 0;
    L->y0 = 0;
    L->y1 = a->win.bh > 0 ? a->win.bh : 400;
    a->rail_n = 0;
    bool idle = a->job == J_NONE;
    if (a->newmode) {
        static const char *const ftip[3] = { "Format: ZIP  F", "Format: TAR  F", "Format: TAR.GZ  F" };
        rail_add(a, L, B_ADD, "Add a file or a folder  A", wlrail_ic_new, lm_theme.btn_green, lm_theme.btn_green_lit, idle, false);
        rail_add(a, L, B_REMOVE, "Remove it from the list  Del", wlrail_ic_trash, lm_theme.btn_red, lm_theme.btn_red_lit, idle && a->nitems > 0, false);
        rail_add(a, L, B_FORMAT, ftip[a->fmt], a->fmt == 0 ? ic_zip : a->fmt == 1 ? ic_tar : ic_tgz, lm_theme.btn_slate, lm_theme.btn_slate_lit, idle, false);
        rail_add(a, L, B_CREATE, "Make the archive  Enter", wlrail_ic_save, lm_theme.btn_blue, lm_theme.btn_blue_lit, idle && a->nitems > 0, false);
        rail_add(a, L, B_CLOSE, "Back  Esc", wlrail_ic_up, lm_theme.btn_slate, lm_theme.btn_slate_lit, idle, false);
    } else {
        bool have = a->arc != NULL, can = have && a->arc->extractable;
        rail_add(a, L, B_UP, "Up one folder  Backspace", wlrail_ic_up, lm_theme.btn_slate, lm_theme.btn_slate_lit, idle && have && a->cwd[0], false);
        rail_add(a, L, B_OPEN, "Open an archive  Ctrl+O", wlrail_ic_open, lm_theme.btn_blue, lm_theme.btn_blue_lit, idle, false);
        rail_add(a, L, B_NEW, "A new archive  Ctrl+N", wlrail_ic_new, lm_theme.btn_teal, lm_theme.btn_teal_lit, idle, false);
        L->it[L->n - 1].gap = true;
        rail_add(a, L, B_EXTRACT, "Extract the selection  E", wlrail_ic_down, lm_theme.btn_green, lm_theme.btn_green_lit, idle && can, false);
        rail_add(a, L, B_EXTRACT_ALL, "Extract everything", ic_all, lm_theme.btn_green, lm_theme.btn_green_lit, idle && can, false);
        rail_add(a, L, B_TEST, "Test the checksums  T", ic_ok, lm_theme.btn_amber, lm_theme.btn_amber_lit, idle && can, false);
    }
    if (a->job != J_NONE)
        rail_add(a, L, B_CANCEL_JOB, "Stop  Esc", ic_stop, lm_theme.btn_red, lm_theme.btn_red_lit, true, true);
    a->rail_n = L->n;
}

static int rail_hover_index(const struct app *a)
{
    for (int i = 0; i < a->rail_n; i++)
        if (a->rail_ids[i] == a->hover_btn)
            return i;
    return -1;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool inside(double px, double py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* ---- the question card ---- */
static const char *prompt_title(const struct app *a)
{
    switch (a->prompt) {
    case P_OPEN: return "Open an archive";
    case P_EXTRACT: return "Extract to the folder";
    case P_ADD: return "Add a file or a folder";
    case P_CREATE: return "Make the archive as";
    default: return "";
    }
}

static const char *prompt_go(const struct app *a)
{
    switch (a->prompt) {
    case P_OPEN: return "Open";
    case P_EXTRACT: return "Extract";
    case P_ADD: return "Add";
    default: return "Create";
    }
}

#define CARD_ROW_H 22
#define CARD_MAX_ROWS 10

static void card_layout(struct app *a, struct wlcard *c, int *fx, int *fy, int *fw, int *ly, int *rows)
{
    *rows = a->prompt == P_OPEN ? LM_CLAMP((a->win.bh - 16 - 200) / CARD_ROW_H, 3, CARD_MAX_ROWS) : 0;
    int cw = LM_CLAMP(a->win.bw - 16, 260, 560);
    int ch = WLCARD_PAD + 24 + 34 + (*rows ? *rows * CARD_ROW_H + 12 : 0) + 20 + WLCARD_BTN_H + WLCARD_PAD;
    c->w = LM_MIN(cw, a->win.bw - 16);
    c->h = LM_MIN(ch, a->win.bh - 16);
    c->x = (a->win.bw - c->w) / 2;
    c->y = (a->win.bh - c->h) / 2;
    *fx = c->x + WLCARD_PAD;
    *fy = c->y + WLCARD_PAD + 24;
    *fw = c->w - 2 * WLCARD_PAD;
    *ly = *fy + 34;
}

/* ---- the folder list of the Open card ---- */
static void card_base(const struct app *a, char *out, size_t n)
{
    if (a->arc_path[0]) {
        snprintf(out, n, "%s", a->arc_path);
        char *sl = strrchr(out, '/');
        if (sl && sl != out)
            *sl = 0;
        else if (sl)
            sl[1] = 0;
        return;
    }
    if (!getcwd(out, n))
        snprintf(out, n, "/");
}

/* The folder part of the field ("~/a/" of "~/a/b.zip") and the rest, which filters the list. */
static void card_split(const struct app *a, char *dirtext, size_t n, const char **filter)
{
    const char *sl = strrchr(a->field, '/');
    size_t len = sl ? (size_t)(sl - a->field) + 1 : 0;
    snprintf(dirtext, n, "%.*s", (int)LM_MIN(len, n - 1), a->field);
    *filter = a->field + len;
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
    char base[PATH_MAX_LEN], full[PATH_MAX_LEN];
    card_base(a, base, sizeof(base));
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
        char path[PATH_MAX_LEN + 260];
        struct stat st;
        snprintf(path, sizeof(path), "%.*s/%s", (int)LM_MIN(strlen(full), (size_t)PATH_MAX_LEN - 1), full, e->d_name);
        c->dir = e->d_type == DT_DIR || (e->d_type == DT_LNK && stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    }
    closedir(d);
    if (a->cn > 1)
        qsort(a->cent, (size_t)a->cn, sizeof(a->cent[0]), centry_cmp);
}

/* The rows that show: ".." first (not at the root, nor while a name is being typed), then what starts with the typed name. idx[k] = -1 is "..". */
static int card_rows(struct app *a, int *idx, int max)
{
    char dirtext[PATH_MAX_LEN];
    const char *filter;
    card_split(a, dirtext, sizeof(dirtext), &filter);
    card_load(a, dirtext);
    int n = 0;
    size_t fl = strlen(filter);
    for (int i = 0; i < a->cn && fl; i++)                                   /* a name typed out in full (a file just picked): the whole folder shows */
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
    return LM_CLAMP((a->win.bh - 16 - 200) / CARD_ROW_H, 3, CARD_MAX_ROWS);
}

/* The field with the entry's name in place of the part after the last slash; a folder gets its slash. */
static void card_put(struct app *a, int row_idx)
{
    char dirtext[PATH_MAX_LEN], next[PATH_MAX_LEN + 260];
    const char *filter;
    card_split(a, dirtext, sizeof(dirtext), &filter);
    if (row_idx < 0) {                                                      /* ".." */
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
    snprintf(a->field, sizeof(a->field), "%.4000s", next);
    wltext_end(a->field);
    a->csel = -1;
    a->ctop = 0;
    a->win.redraw = true;
}

static void prompt_accept(struct app *a);

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
static void open_prompt(struct app *a, int prompt, const char *initial)
{
    a->prompt = prompt;
    snprintf(a->field, sizeof(a->field), "%s", initial);
    wltext_end(a->field);
    a->csel = -1;
    a->ctop = 0;
    a->cdir[0] = 0;                                                         /* (the folder list is read again) */
    a->cmsg[0] = 0;
    a->win.redraw = true;
}
/* ---- jobs ---- */
static void *job_main(void *arg)
{
    struct app *a = arg;
    struct aio_opts o = { .overwrite = a->job_overwrite, .test_only = a->job == J_TEST };
    if (a->job == J_CREATE) {
        a->job_rc = aio_create(a->job_out, a->fmt == 0 ? AIO_ZIP : a->fmt == 1 ? AIO_TAR : AIO_TARGZ, (const char *const *)a->items, a->nitems, 6, a->job_overwrite, &a->prog, &a->rep, a->job_err, sizeof(a->job_err));
    } else {
        a->job_rc = aio_extract(a->arc, a->job_sel, a->job_nsel, a->job_dest, &o, &a->prog, &a->rep, a->job_err, sizeof(a->job_err));
    }
    atomic_store(&a->job_done, 1);
    return NULL;
}

static void start_job(struct app *a, int kind)
{
    a->job = kind;
    a->job_err[0] = 0;
    memset(&a->rep, 0, sizeof(a->rep));
    a->prog.cancel = 0;
    a->prog.done = a->prog.total = 0;
    atomic_store(&a->job_done, 0);
    a->job_overwrite = a->overwrite;
    if (pthread_create(&a->thread, NULL, job_main, a) != 0) {
        a->job = J_NONE;
        set_status(a, true, "Cannot start the work: %s", strerror(errno));
        return;
    }
    a->win.redraw = true;
}

static void job_finish(struct app *a)
{
    pthread_join(a->thread, NULL);
    int kind = a->job;
    a->job = J_NONE;
    struct aio_report *r = &a->rep;
    char big[32];
    human((int64_t)r->bytes, big, sizeof(big));
    if (a->job_rc == -2) {
        set_status(a, true, "Stopped. %d files done before that.", r->extracted);
    } else if (a->job_rc < 0) {
        set_status(a, true, "%s", a->job_err[0] ? a->job_err : "It did not work.");
    } else if (kind == J_CREATE) {
        char b2[300];
        snprintf(b2, sizeof(b2), "%s", a->job_out);
        if (r->failed)
            set_status(a, false, "Made %s: %d files, %d folders, %d links. %d problems: %s", base_of(b2), r->extracted, r->dirs, r->links, r->failed, r->err[0]);
        else
            set_status(a, false, "Made %s: %d files, %d folders, %d links.", base_of(b2), r->extracted, r->dirs, r->links);
        open_archive(a, b2);                                                 /* show what was made */
        if (r->failed == 0)
            set_status(a, false, "Made %s: %d files, %d folders, %d links.", base_of(b2), r->extracted, r->dirs, r->links);
    } else if (kind == J_TEST) {
        if (r->failed)
            set_status(a, true, "%d of the files are damaged. %s", r->failed, r->err[0]);
        else
            set_status(a, false, "All %d files are fine (%s checked).", r->extracted, big);
    } else {
        snprintf(a->last_dest, sizeof(a->last_dest), "%s", a->job_dest);
        if (r->failed)
            set_status(a, true, "Extracted %d files (%s), %d problems: %s", r->extracted, big, r->failed, r->err[0]);
        else if (r->skipped)
            set_status(a, false, "Extracted %d files (%s) to %s. %d existing ones were kept (turn on Replace to overwrite).", r->extracted, big, a->job_dest, r->skipped);
        else
            set_status(a, false, "Extracted %d files, %d folders (%s) to %s.", r->extracted, r->dirs, big, a->job_dest);
    }
    free(a->job_sel);
    a->job_sel = NULL;
    a->job_nsel = 0;
    a->win.redraw = true;
}

/* The entries to extract for the selected rows (a folder brings everything below it); NULL with *n = 0 means "nothing selected". */
static int *selection_entries(struct app *a, int *count)
{
    *count = 0;
    bool any = false;
    for (int i = 0; i < a->nrows; i++)
        any |= a->sel[i] != 0;
    bool use_cursor = !any && a->cur >= 0 && a->cur < a->nrows && a->sel_by_cursor;     /* nothing marked: the row under the cursor, like F5 in the file manager */
    if (!any && !use_cursor)
        return NULL;
    int *out = malloc((size_t)a->arc->n * sizeof(int) + 4);
    uint8_t *mark = calloc((size_t)a->arc->n + 1, 1);
    if (!out || !mark) {
        free(out);
        free(mark);
        return NULL;
    }
    for (int r = 0; r < a->nrows; r++) {
        if (!a->sel[r] && !(use_cursor && r == a->cur))
            continue;
        const struct row *row = &a->rows[r];
        if (!row->dir) {
            if (row->idx >= 0)
                mark[row->idx] = 1;
            continue;
        }
        char prefix[PATH_MAX_LEN];
        snprintf(prefix, sizeof(prefix), "%s%s/", a->cwd, row->name);
        size_t pl = strlen(prefix);
        for (int i = 0; i < a->arc->n; i++) {
            const char *p = norm_path(a->arc->e[i].path ? a->arc->e[i].path : "");
            if (!strncmp(p, prefix, pl) || (strlen(p) == pl - 1 && !strncmp(p, prefix, pl - 1)))
                mark[i] = 1;
        }
    }
    int n = 0;
    for (int i = 0; i < a->arc->n; i++)
        if (mark[i])
            out[n++] = i;
    free(mark);
    *count = n;
    return out;
}

static void begin_extract(struct app *a, const char *dest_in)
{
    char dest[PATH_MAX_LEN];
    expand_home(dest_in, dest, sizeof(dest));
    if (!dest[0]) {
        set_status(a, true, "Say where to extract to.");
        return;
    }
    int n = 0;
    a->job_sel = selection_entries(a, &n);
    a->job_nsel = a->job_sel ? n : 0;
    snprintf(a->job_dest, sizeof(a->job_dest), "%s", dest);
    start_job(a, J_EXTRACT);
}

static void begin_create(struct app *a, const char *out_in)
{
    char out[PATH_MAX_LEN];
    expand_home(out_in, out, sizeof(out));
    static const char *const want_ext[] = { ".zip", ".tar", ".tar.gz" };
    int f = aio_format_of(out);
    if (f < 0) {
        size_t l = strlen(out);
        snprintf(out + l, sizeof(out) - l, "%s", want_ext[a->fmt]);
    } else
        a->fmt = f == AIO_ZIP ? 0 : f == AIO_TAR ? 1 : 2;
    snprintf(a->job_out, sizeof(a->job_out), "%s", out);
    start_job(a, J_CREATE);
}

static void add_item(struct app *a, const char *path)
{
    char full[PATH_MAX_LEN];
    expand_home(path, full, sizeof(full));
    struct stat st;
    if (lstat(full, &st) < 0) {
        set_status(a, true, "%s: %s", full, strerror(errno));
        return;
    }
    char *rp = realpath(full, NULL);
    const char *use = rp ? rp : full;
    for (int i = 0; i < a->nitems; i++)
        if (!strcmp(a->items[i], use)) {
            free(rp);
            return;
        }
    if (a->nitems == a->capitems) {
        a->capitems = a->capitems ? a->capitems * 2 : 16;
        a->items = realloc(a->items, (size_t)a->capitems * sizeof(char *));
    }
    a->items[a->nitems++] = strdup(use);
    free(rp);
    a->item_cur = a->nitems - 1;
    a->newmode = true;
    set_status(a, false, "%d item%s to put in the archive.", a->nitems, a->nitems == 1 ? "" : "s");
}

static void enter_new_mode(struct app *a)
{
    a->newmode = true;
    a->top = 0;
    a->win.redraw = true;
}

static void open_entry(struct app *a, const struct row *r)
{
    if (!a->arc->extractable || r->idx < 0 || r->dir)
        return;
    if (r->size > (64 << 20)) {
        set_status(a, true, "That file is big (%lld MB): extract it instead.", (long long)(r->size >> 20));
        return;
    }
    const char *cache = getenv("XDG_CACHE_HOME");
    char dir[PATH_MAX_LEN], root[PATH_MAX_LEN];
    snprintf(root, sizeof(root), "%s/mellagui", cache && cache[0] ? cache : (getenv("HOME") ? "" : "/tmp"));
    if (!(cache && cache[0]) && getenv("HOME"))
        snprintf(root, sizeof(root), "%s/.cache/mellagui", getenv("HOME"));
    mkdir(root, 0700);
    snprintf(dir, sizeof(dir), "%s/archive", root);
    mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/archive/%d", root, (int)getpid());
    struct aio_opts o = { .overwrite = true };
    int sel[1] = { r->idx };
    struct aio_report rep;
    char err[300] = "";
    int rc = aio_extract(a->arc, sel, 1, dir, &o, NULL, &rep, err, sizeof(err));
    if (rc < 0 || rep.failed) {
        set_status(a, true, "%s", rc < 0 ? err : rep.err[0]);
        return;
    }
    char rel[PATH_MAX_LEN];
    snprintf(rel, sizeof(rel), "%s/%s", dir, norm_path(a->arc->e[r->idx].path));
    const char *opener = getenv("MELLAGUI_XDG_OPEN");
    if (!opener || !opener[0])
        opener = "xdg-open";
    if (!strcmp(opener, "none")) {
        set_status(a, false, "Extracted to %s", rel);
        return;
    }
    pid_t p = fork();
    if (p == 0) {
        if (fork() == 0) {
            setsid();
            int nul = open("/dev/null", O_RDWR);
            if (nul >= 0) {
                dup2(nul, 0);
                dup2(nul, 1);
                dup2(nul, 2);
            }
            execlp(opener, opener, rel, (char *)NULL);
        }
        _exit(0);
    } else if (p > 0)
        waitpid(p, NULL, 0);
    set_status(a, false, "Opening %s ...", base_of(rel));
}

static void open_in_files(const char *dir)
{
    pid_t p = fork();
    if (p == 0) {
        if (fork() == 0) {
            setsid();
            execlp("mellagui-files", "mellagui-files", dir, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    } else if (p > 0)
        waitpid(p, NULL, 0);
}

/* ---- painting ---- */

static int col_x(const struct app *a, int col)
{
    int right = a->win.bw - lm_metrics.scrollbar_width;
    switch (col) {
    case 1: return right - 340;                                             /* size */
    case 2: return right - 250;                                             /* packed */
    default: return right - 140;                                            /* modified */
    }
}
static void paint_scrollbar(struct app *a, int n)
{
    struct wlwin *w = &a->win;
    int vis = rows_visible(a), sbw = lm_metrics.scrollbar_width, x = w->bw - sbw, y = list_y(), h = w->bh - FOOT_H - y;
    wlwin_fill(w, x, y, sbw, h, lm_theme.bar);
    if (n > vis) {
        int th = LM_MAX(h * vis / n, 24), ty = y + (h - th) * a->top / LM_MAX(n - vis, 1);
        wlwin_fill(w, x + 2, ty + 1, sbw - 4, th - 2, a->drag_bar >= 0 ? lm_theme.button_hover : lm_theme.control);
    }
}

/* A column label with the little triangle of the sort (up: ascending). */
static void paint_sort_label(struct wlwin *w, int x, int y, const char *label, bool active, bool desc, uint32_t col)
{
    wlwin_text(w, x, y, label, 1, col, 0);
    if (!active)
        return;
    int tx = x + lm_font_text_width(label) + 6, ty = y + 6;
    for (int k = 0; k < 4; k++) {
        int half = desc ? 3 - k : k;
        wlwin_fill(w, tx + 3 - half, ty + k, 2 * half + 1, 1, col);
    }
}
static void paint_rows(struct app *a)
{
    struct wlwin *w = &a->win;
    int y0 = list_y(), vis = rows_visible(a), n = list_count(a);
    int right = w->bw - lm_metrics.scrollbar_width;
    int xs = col_x(a, 1), xp = col_x(a, 2), xm = col_x(a, 3);
    bool wide = w->bw >= 560, mid = w->bw >= 420;
    /* the header: the path (lit), the column labels */
    wlwin_fill(w, 0, 0, w->bw, HEAD_H, lm_theme.button_hover);
    char head[PATH_MAX_LEN + 80];
    if (a->newmode)
        snprintf(head, sizeof(head), "New archive: %d item%s", a->nitems, a->nitems == 1 ? "" : "s");
    else if (a->arc)
        snprintf(head, sizeof(head), "%s  %s  /%s", base_of(a->arc_path), a->arc->kind, a->cwd);
    else
        snprintf(head, sizeof(head), "No archive open");
    int name_x = (wide ? xs : mid ? right - 110 : right) - 70;
    wlwin_text(w, 8, 3, head, 1, lm_theme.text_bright, LM_MAX(name_x - 16, 40));
    if (!a->newmode && mid) {
        uint32_t on = lm_theme.text_bright;
        paint_sort_label(w, name_x, 3, "Name", a->sort == SORT_NAME, a->sort_desc, a->sort == SORT_NAME ? on : lm_theme.text_dim);
        paint_sort_label(w, wide ? xs : right - 110, 3, "Size", a->sort == SORT_SIZE, a->sort_desc, a->sort == SORT_SIZE ? on : lm_theme.text_dim);
        if (wide) {
            wlwin_text(w, xp, 3, "Packed", 1, lm_theme.text_dim, 0);
            paint_sort_label(w, xm, 3, "Modified", a->sort == SORT_TIME, a->sort_desc, a->sort == SORT_TIME ? on : lm_theme.text_dim);
        }
    } else if (a->newmode && mid) {
        wlwin_text(w, right - 110, 3, "Size", 1, lm_theme.text_dim, 0);
    }
    wlwin_fill(w, 0, y0 - 1, w->bw, 1, lm_theme.bar);
    wlwin_fill(w, 0, y0, right, vis * ROW_H, lm_theme.background);
    if (n == 0) {
        const char *msg = a->newmode ? "Nothing added yet: press the + button (or give files on the command line)."
                                     : a->arc ? "This folder is empty." : "Open an archive (Ctrl+O) or start a new one (Ctrl+N).";
        wlwin_text(w, 10, y0 + 6, msg, 1, lm_theme.text_dim, right - 20);
    }
    int size_x = wide ? xs : right - 110;
    for (int r = 0; r < vis && a->top + r < n; r++) {
        int i = a->top + r, y = y0 + r * ROW_H;
        bool cur = i == (a->newmode ? a->item_cur : a->cur), marked = !a->newmode && a->sel && a->sel[i];
        if (cur)
            wlwin_fill(w, 0, y, right, ROW_H, lm_theme.button_hover);
        char sz[32] = "", tm[32] = "", pk[32] = "", name[PATH_MAX_LEN + 8];
        bool dir;
        if (a->newmode) {
            struct stat st;
            bool ok = lstat(a->items[i], &st) == 0;
            dir = ok && S_ISDIR(st.st_mode);
            snprintf(name, sizeof(name), "%s%s", a->items[i], dir ? "/" : "");
            if (ok && !dir)
                human(st.st_size, sz, sizeof(sz));
        } else {
            const struct row *row = &a->rows[i];
            dir = row->dir;
            snprintf(name, sizeof(name), "%s%s%s", marked ? "* " : "", row->name, dir ? "/" : "");
            human(row->size >= 0 && (row->size > 0 || !dir) ? row->size : -1, sz, sizeof(sz));
            human(row->packed, pk, sizeof(pk));
            time_text(row->mtime, tm, sizeof(tm));
        }
        uint32_t col = marked ? lm_theme.warn : dir ? lm_theme.accent : lm_theme.text_soft;
        wlwin_text(w, 10, y + 1, name, 1, col, (mid ? name_x : right) - 20);
        if (sz[0] && mid)
            wlwin_text(w, size_x, y + 1, sz, 1, lm_theme.text_dim, 0);
        if (!a->newmode && wide) {
            if (pk[0])
                wlwin_text(w, xp, y + 1, pk, 1, lm_theme.text_dim, 0);
            if (tm[0])
                wlwin_text(w, xm, y + 1, tm, 1, lm_theme.text_dim, right - xm);
        }
    }
    paint_scrollbar(a, n);
}
static void paint_footer(struct app *a)
{
    struct wlwin *w = &a->win;
    int y = w->bh - FOOT_H;
    wlwin_fill(w, 0, y, w->bw, FOOT_H, lm_theme.bar);
    if (a->job != J_NONE) {
        char cur[260];
        aio_progress_current(&a->prog, cur, sizeof(cur));
        const char *what = a->job == J_CREATE ? "Packing" : a->job == J_TEST ? "Checking" : "Extracting";
        char line[400];
        snprintf(line, sizeof(line), "%s %s", what, cur);
        wlwin_text(w, 6, y + 3, line, 1, lm_theme.caution, w->bw - 12);
        int bw = w->bw - 12, by = y + 26;
        wlwin_fill(w, 6, by, bw, 10, lm_theme.field);
        uint64_t tot = a->prog.total, done = a->prog.done;
        int fill = tot ? (int)((double)LM_MIN(done, tot) / (double)tot * bw) : 0;
        if (fill > 0)
            wlwin_fill(w, 6, by, fill, 10, lm_theme.ok);
        return;
    }
    wlwin_text(w, 6, y + 3, a->status, 1, a->status_err ? lm_theme.error : lm_theme.caution, w->bw - 12);
    int cy = y + 24;                                                         /* the switch for files that exist, and the button that shows the folder */
    wlwin_fill(w, 6, cy + 1, 14, 14, lm_theme.field);
    if (a->overwrite)
        wlwin_fill(w, 9, cy + 4, 8, 8, lm_theme.accent);
    wlwin_text(w, 28, cy, "Replace files that exist already  R", 1, lm_theme.text_dim, 0);
    if (a->last_dest[0] && !a->newmode) {
        const char *l = "Show the folder";
        int bw = lm_font_text_width(l) + 20, bx = w->bw - 6 - bw;
        wlwin_drop(w, bx, cy - 2, bw, 20, lm_button_radius(10), a->hover_btn == B_SHOW ? lm_theme.btn_teal_lit : lm_theme.btn_teal);
        wlwin_text(w, bx + 10, cy, l, 1, lm_theme.on_colour, 0);
    }
}
static void paint_card(struct app *a)
{
    struct wlwin *w = &a->win;
    struct wlcard c;
    int fx, fy, fw, ly, rows;
    card_layout(a, &c, &fx, &fy, &fw, &ly, &rows);
    wlcard_frame(w, &c, c.w, c.h, prompt_title(a), lm_theme.accent, lm_theme.bar, lm_theme.accent);
    if (a->prompt == P_OPEN) {
        const char *h = "Tab completes";
        wlwin_text(w, c.x + c.w - WLCARD_PAD - lm_font_text_width(h), c.y + WLCARD_PAD - 2, h, 1, lm_theme.text_dim, 0);
    }
    wlwin_round_fill(w, fx, fy, fw, 28, 14, lm_theme.background);           /* the field */
    wltext_paint(w, fx + 12, fy + 6, fw - 24, a->field, true, lm_theme.text_soft, lm_theme.button_hover, lm_theme.text_soft);
    if (rows) {
        int idx[4001];
        int n = card_rows(a, idx, 4000);
        a->ctop = LM_CLAMP(a->ctop, 0, LM_MAX(n - rows, 0));
        wlwin_round_fill(w, fx, ly, fw, rows * CARD_ROW_H + 8, 10, lm_theme.background);
        if (n == 0)
            wlwin_text(w, fx + 12, ly + 8, "Nothing here starts like that", 1, lm_theme.text_dim, fw - 24);
        for (int r = 0; r < rows && a->ctop + r < n; r++) {
            int k = a->ctop + r, e = idx[k], ry = ly + 4 + r * CARD_ROW_H;
            bool dir = e < 0 || a->cent[e].dir;
            bool over = a->px >= fx && a->px < fx + fw - 10 && a->py >= ry && a->py < ry + CARD_ROW_H;
            const char *tail = strrchr(a->field, '/');
            bool named = e >= 0 && !strcmp(a->cent[e].name, tail ? tail + 1 : a->field);          /* the file in the field */
            if (k == a->csel || named || over)
                wlwin_round_fill(w, fx + 4, ry, fw - 8 - (n > rows ? lm_metrics.scrollbar_width : 0), CARD_ROW_H, 11,
                                 k == a->csel || named ? lm_theme.button_hover : lm_theme.panel_line);
            int ix = fx + 14, iy = ry + 4;
            if (dir) {                                                      /* a folder: a tab and a body */
                wlwin_fill(w, ix, iy + 1, 6, 3, lm_theme.btn_amber);
                wlwin_round_fill(w, ix, iy + 3, 14, 10, 3, lm_theme.btn_amber_lit);
            } else {
                wlwin_round_fill(w, ix + 2, iy, 10, 14, 2, lm_theme.btn_sky);
                wlwin_fill(w, ix + 4, iy + 5, 6, 1, lm_theme.bar);
                wlwin_fill(w, ix + 4, iy + 8, 6, 1, lm_theme.bar);
            }
            wlwin_text(w, ix + 24, ry + (CARD_ROW_H - LM_FONT_H) / 2, e < 0 ? ".." : a->cent[e].name, 1, dir ? lm_theme.accent : lm_theme.text_soft, fw - 60);
        }
        if (n > rows) {                                                     /* a thin scroll bar */
            int sbw = lm_metrics.scrollbar_width, th = LM_MAX((rows * CARD_ROW_H) * rows / n, 20);
            int ty = ly + 4 + (rows * CARD_ROW_H - th) * a->ctop / LM_MAX(n - rows, 1);
            wlwin_round_fill(w, fx + fw - sbw - 2, ty, sbw - 4, th, (sbw - 4) / 2, lm_theme.control);
        }
    } else {
        const char *hint = a->prompt == P_EXTRACT ? "A folder that does not exist is made. Files that exist are kept unless Replace is on."
                           : a->prompt == P_ADD    ? "The path of a file or a folder (~ means your home folder)."
                                                   : "The name ends in .zip, .tar or .tar.gz (.tgz); it is not written over a file.";
        wlwin_text_small(w, fx, ly - 2, hint, 0.85, lm_theme.text_dim, fw);
    }
    int my = c.y + c.h - WLCARD_PAD - WLCARD_BTN_H - 24;
    if (a->cmsg[0])
        wlwin_text(w, fx, my, a->cmsg, 1, lm_theme.caution, fw);
    const char *labels[2] = { "Cancel", prompt_go(a) };
    uint32_t cols[2] = { lm_theme.btn_slate, a->prompt == P_CREATE ? lm_theme.btn_blue : a->prompt == P_OPEN ? lm_theme.btn_blue : lm_theme.btn_green };
    wlcard_buttons(w, &c, labels, 2, cols, a->hover_card, -1, lm_theme.on_colour);
}
static void write_dump(struct app *a)
{
    if (!a->dump)
        return;
    FILE *f = fopen(a->dump, "w");
    if (!f)
        return;
    int sel = 0;
    for (int i = 0; !a->newmode && a->sel && i < a->nrows; i++)
        sel += a->sel[i] != 0;
    fprintf(f, "mode=%s kind=%s n=%d path=%s cwd=%s rows=%d sel=%d cur=%d top=%d job=%d prompt=%d overwrite=%d fmt=%d items=%d\n", a->newmode ? "new" : a->arc ? "view" : "empty",
            a->arc ? a->arc->kind : "-", a->arc ? a->arc->n : 0, a->arc_path, a->cwd, a->nrows, sel, a->newmode ? a->item_cur : a->cur, a->top, a->job, a->prompt, a->overwrite, a->fmt, a->nitems);
    fprintf(f, "status err=%d %s\n", a->status_err, a->status);
    struct wlrail L;
    files_rail_like(a, &L);
    int ry[WLRAIL_MAX];
    wlrail_layout(&L, ry);
    for (int i = 0; i < L.n; i++)
        if (ry[i] >= 0)
            fprintf(f, "btn %d %d %d %d %d %d\n", a->rail_ids[i], (WLRAIL_W - WLRAIL_D) / 2, ry[i], WLRAIL_D, WLRAIL_D, !a->rail_idle[i]);
    fprintf(f, "rect list %d %d %d %d\n", WLRAIL_W, list_y(), a->win.bw, rows_visible(a) * ROW_H);
    fprintf(f, "rect head %d %d %d %d\n", WLRAIL_W, 0, a->win.bw, HEAD_H);
    fprintf(f, "rect replace %d %d %d %d\n", WLRAIL_W + 6, a->win.bh - FOOT_H + 24, 300, 16);
    int n = list_count(a);
    for (int r = 0; r < rows_visible(a) && a->top + r < n; r++) {
        int i = a->top + r;
        if (a->newmode)
            fprintf(f, "row %d y=%d dir=0 size=-1 sel=0 name=%s\n", i, list_y() + r * ROW_H, a->items[i]);
        else
            fprintf(f, "row %d y=%d dir=%d size=%lld sel=%d name=%s\n", i, list_y() + r * ROW_H, a->rows[i].dir, (long long)a->rows[i].size, a->sel[i], a->rows[i].name);
    }
    if (a->prompt) {
        struct wlcard c;
        int fx, fy, fw, ly, crows;
        card_layout(a, &c, &fx, &fy, &fw, &ly, &crows);
        fprintf(f, "card %d %d %d %d\n", c.x + WLRAIL_W, c.y, c.w, c.h);
        if (crows) {
            int idx[4001];
            int cn = card_rows(a, idx, 4000);
            fprintf(f, "clist %d %d %d %d csel=%d count=%d\n", fx + WLRAIL_W, ly + 4, fw, crows * CARD_ROW_H, a->csel, cn);
            for (int k = a->ctop; k < cn && k < a->ctop + crows; k++)
                fprintf(f, "crow %d y=%d dir=%d name=%s\n", k, ly + 4 + (k - a->ctop) * CARD_ROW_H, idx[k] < 0 || a->cent[idx[k]].dir, idx[k] < 0 ? ".." : a->cent[idx[k]].name);
        }
        for (int k = 0; k < 2; k++) {
            int x, y, bw, bh;
            wlcard_button_rect(&c, k, 2, &x, &y, &bw, &bh);
            fprintf(f, "cbtn %d %d %d %d %d\n", k, x + WLRAIL_W, y, bw, bh);
        }
        fprintf(f, "field %s\n", a->field);
    }
    fprintf(f, "ok\n");
    fclose(f);
}
static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    set_title(a);
    wlwin_fill(w, 0, 0, w->bw, w->bh, lm_theme.background);
    clamp_view(a);
    paint_rows(a);
    paint_footer(a);
    if (a->prompt)
        paint_card(a);
    struct wlrail L;
    files_rail_like(a, &L);
    wlrail_paint(w, &L, &a->ran, rail_hover_index(a), now_ms(), lm_theme.bar, lm_theme.bar, lm_theme.accent, lm_theme.text_soft);
    write_dump(a);
}
/* ---- input ---- */
static void select_range(struct app *a, int from, int to)
{
    memset(a->sel, 0, (size_t)a->nrows + 1);
    if (from > to) {
        int t = from;
        from = to;
        to = t;
    }
    for (int i = LM_MAX(from, 0); i <= to && i < a->nrows; i++)
        a->sel[i] = 1;
}

static void go_folder(struct app *a, const char *cwd)
{
    snprintf(a->cwd, sizeof(a->cwd), "%s", cwd);
    a->cur = 0;
    build_rows(a);
    a->win.redraw = true;
}

static void go_up(struct app *a)
{
    if (!a->cwd[0])
        return;
    char t[PATH_MAX_LEN];
    snprintf(t, sizeof(t), "%s", a->cwd);
    size_t l = strlen(t);
    if (l)
        t[l - 1] = 0;
    char *s = strrchr(t, '/');
    char prev[256] = "";
    snprintf(prev, sizeof(prev), "%s", s ? s + 1 : t);
    if (s)
        s[1] = 0;
    else
        t[0] = 0;
    go_folder(a, t);
    char pn[256];
    snprintf(pn, sizeof(pn), "%s", prev);
    for (int i = 0; i < a->nrows; i++)
        if (a->rows[i].dir && !strcmp(a->rows[i].name, pn)) {                /* the folder we came out of is the current row */
            a->cur = i;
            break;
        }
    a->anchor = a->cur;
}

static void activate_row(struct app *a, int i)
{
    if (i < 0 || i >= a->nrows)
        return;
    struct row *r = &a->rows[i];
    if (r->dir) {
        char t[PATH_MAX_LEN];
        snprintf(t, sizeof(t), "%s%s/", a->cwd, r->name);
        go_folder(a, t);
    } else
        open_entry(a, r);
}

static void do_button(struct app *a, int id)
{
    switch (id) {
    case B_OPEN: {
        char start[PATH_MAX_LEN];
        card_base(a, start, sizeof(start));
        size_t l = strlen(start);
        if (!a->arc_path[0] && getenv("HOME") && !strcmp(start, "/"))
            snprintf(start, sizeof(start), "~");
        if (l == 0 || start[strlen(start) - 1] != '/')
            snprintf(start + strlen(start), sizeof(start) - strlen(start), "/");
        open_prompt(a, P_OPEN, start);
        break;
    }
    case B_NEW:
        enter_new_mode(a);
        break;
    case B_UP:
        go_up(a);
        break;
    case B_EXTRACT:
    case B_EXTRACT_ALL:
        if (!a->arc || !a->arc->extractable) {
            set_status(a, true, "%s", a->arc ? a->arc->why_not : "Open an archive first.");
            break;
        }
        a->sel_by_cursor = id == B_EXTRACT;
        if (id == B_EXTRACT_ALL)
            memset(a->sel, 0, (size_t)a->nrows + 1);
        {
            char d[PATH_MAX_LEN];
            default_dest(a, d, sizeof(d));
            open_prompt(a, P_EXTRACT, d);
        }
        break;
    case B_TEST:
        if (a->arc && a->arc->extractable) {
            a->job_sel = NULL;
            a->job_nsel = 0;
            a->job_dest[0] = 0;
            start_job(a, J_TEST);
        }
        break;
    case B_ADD:
        open_prompt(a, P_ADD, "~/");
        break;
    case B_REMOVE:
        if (a->nitems > 0 && a->item_cur < a->nitems) {
            free(a->items[a->item_cur]);
            memmove(&a->items[a->item_cur], &a->items[a->item_cur + 1], (size_t)(a->nitems - a->item_cur - 1) * sizeof(char *));
            a->nitems--;
            a->win.redraw = true;
        }
        break;
    case B_FORMAT:
        a->fmt = (a->fmt + 1) % 3;
        break;
    case B_CREATE:
        if (a->nitems > 0) {
            char d[PATH_MAX_LEN], dir[PATH_MAX_LEN];
            snprintf(dir, sizeof(dir), "%s", a->items[0]);
            char *s = strrchr(dir, '/');
            if (s && s != dir)
                *s = 0;
            char stem[256];
            stem_of(a->items[0], stem, sizeof(stem));
            static const char *const ext[] = { ".zip", ".tar", ".tar.gz" };
            snprintf(d, sizeof(d), "%s/%s%s", dir, a->nitems == 1 ? stem : "archive", ext[a->fmt]);
            open_prompt(a, P_CREATE, d);
        }
        break;
    case B_CLOSE:
        a->newmode = false;
        a->top = 0;
        break;
    case B_REPLACE:
        a->overwrite = !a->overwrite;
        break;
    case B_SHOW:
        if (a->last_dest[0])
            open_in_files(a->last_dest);
        break;
    case B_CANCEL_JOB:
        a->prog.cancel = 1;
        break;
    }
    a->win.redraw = true;
}

static void prompt_accept(struct app *a)
{
    int p = a->prompt;
    char text[PATH_MAX_LEN];
    snprintf(text, sizeof(text), "%s", a->field);
    a->prompt = 0;
    if (!text[0]) {
        a->win.redraw = true;
        return;
    }
    if (p == P_OPEN) {
        char base[PATH_MAX_LEN], full[PATH_MAX_LEN];
        card_base(a, base, sizeof(base));
        lm_resolve_path(base, text, full, sizeof(full));
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {                  /* Enter on a folder looks inside it */
            size_t l = strlen(text);
            char t2[PATH_MAX_LEN];
            snprintf(t2, sizeof(t2), "%s%s", text, text[l - 1] == '/' ? "" : "/");
            open_prompt(a, P_OPEN, t2);
            return;
        }
        if (!open_archive(a, full)) {                                       /* it is no archive: the card stays, with the reason */
            char why[300];
            snprintf(why, sizeof(why), "%.200s", a->status);
            open_prompt(a, P_OPEN, text);
            snprintf(a->cmsg, sizeof(a->cmsg), "%s", why);
        }
    }
    else if (p == P_EXTRACT)
        begin_extract(a, text);
    else if (p == P_ADD)
        add_item(a, text);
    else if (p == P_CREATE)
        begin_create(a, text);
    a->win.redraw = true;
}

static void move_cursor(struct app *a, int to, bool extend)
{
    int n = list_count(a);
    int *cur = a->newmode ? &a->item_cur : &a->cur;
    to = LM_CLAMP(to, 0, LM_MAX(n - 1, 0));
    *cur = to;
    if (!a->newmode) {
        if (extend)
            select_range(a, a->anchor, to);
        else
            a->anchor = to;                                                  /* (the marks stay, as in the file manager) */
    }
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op == LM_WL_KEYBOARD_EVT_KEY && args[3].u != 0)
        a->clip.serial = args[0].u;
    if (op != LM_WL_KEYBOARD_EVT_KEY || args[3].u == 0)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, w->mods);
    bool ctrl = (w->mods & LM_MOD_CTRL) != 0, shift = (w->mods & LM_MOD_SHIFT) != 0;
    if (a->prompt) {
        bool list = a->prompt == P_OPEN;
        if (list && (ks == LM_KS_Up || ks == LM_KS_Down || ks == LM_KS_Page_Up || ks == LM_KS_Page_Down))
            card_move(a, ks == LM_KS_Up ? -1 : ks == LM_KS_Down ? 1 : ks == LM_KS_Page_Up ? -8 : 8);
        else if (list && (ks == LM_KS_Return || ks == LM_KS_KP_Enter) && a->csel >= 0)
            card_choose(a, a->csel, true);
        else if (ks == LM_KS_Escape)
            a->prompt = 0;
        else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
            prompt_accept(a);
        else if (list && ks == LM_KS_Tab) {
            char base[PATH_MAX_LEN], lst[160];
            card_base(a, base, sizeof(base));
            int n = lm_complete_path(base, a->field, sizeof(a->field), true, lst, sizeof(lst));
            if (n == 0)
                snprintf(a->cmsg, sizeof(a->cmsg), "Nothing here starts like that");
            else if (n > 1)
                snprintf(a->cmsg, sizeof(a->cmsg), "%d matches: %s", n, lst);
            else
                a->cmsg[0] = 0;
            wltext_end(a->field);
            a->csel = -1;
            a->ctop = 0;
        } else if (wltext_edit(&a->clip, a->field, sizeof(a->field), ks, w->mods)) {
            a->csel = -1;                                                   /* (the list follows what is typed) */
            a->ctop = 0;
            a->cmsg[0] = 0;
        }
        w->redraw = true;
        return;
    }
    if (ks == LM_KS_Escape) {
        if (a->job != J_NONE)
            a->prog.cancel = 1;
        else if (a->newmode && a->arc)
            a->newmode = false;
        else
            w->running = false;
        w->redraw = true;
        return;
    }
    if (a->job != J_NONE)
        return;
    int vis = rows_visible(a);
    int *cur = a->newmode ? &a->item_cur : &a->cur;
    if (ctrl && (ks == 'o' || ks == 'O'))
        do_button(a, B_OPEN);
    else if (ctrl && (ks == 'n' || ks == 'N'))
        do_button(a, B_NEW);
    else if (ks == LM_KS_Down)
        move_cursor(a, *cur + 1, shift);
    else if (ks == LM_KS_Up)
        move_cursor(a, *cur - 1, shift);
    else if (ks == LM_KS_Page_Down)
        move_cursor(a, *cur + vis - 1, shift);
    else if (ks == LM_KS_Page_Up)
        move_cursor(a, *cur - vis + 1, shift);
    else if (ks == LM_KS_Home)
        move_cursor(a, 0, shift);
    else if (ks == LM_KS_End)
        move_cursor(a, list_count(a) - 1, shift);
    else if (a->newmode) {
        if (ks == LM_KS_Delete || ks == LM_KS_BackSpace)
            do_button(a, B_REMOVE);
        else if (ks == 'a' || ks == 'A')
            do_button(a, B_ADD);
        else if (ks == 'f' || ks == 'F')
            do_button(a, B_FORMAT);
        else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
            do_button(a, B_CREATE);
    } else {
        if (ctrl && (ks == 'a' || ks == 'A') && a->sel)
            memset(a->sel, 1, (size_t)a->nrows);
        else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter)
            activate_row(a, a->cur);
        else if (ks == LM_KS_BackSpace || ks == LM_KS_Left)
            go_up(a);
        else if (ks == LM_KS_Right && a->cur < a->nrows && a->rows[a->cur].dir)
            activate_row(a, a->cur);
        else if (ks == ' ' && a->sel && a->cur < a->nrows)
            a->sel[a->cur] ^= 1;
        else if (ks == 'e' || ks == 'E')
            do_button(a, B_EXTRACT);
        else if (ks == 't' || ks == 'T')
            do_button(a, B_TEST);
        else if (ks == 'r' || ks == 'R')
            do_button(a, B_REPLACE);
        else if (ks == 'n' || ks == 'N')
            do_button(a, B_NEW);
    }
    w->redraw = true;
}

static int row_at(struct app *a, double py)
{
    if (py < list_y() || py >= list_y() + rows_visible(a) * ROW_H)
        return -1;
    int r = a->top + ((int)py - list_y()) / ROW_H;
    return r < list_count(a) ? r : -1;
}

static bool show_btn_at(const struct app *a, double px, double py)
{
    int bw = lm_font_text_width("Show the folder") + 20;
    return a->last_dest[0] && !a->newmode && inside(px, py, a->win.bw - 6 - bw, a->win.bh - FOOT_H + 22, bw, 20);
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op == LM_WL_POINTER_EVT_MOTION || op == LM_WL_POINTER_EVT_ENTER) {
        a->px = lm_fixed_to_double(args[op == LM_WL_POINTER_EVT_ENTER ? 2 : 1].f);
        a->py = lm_fixed_to_double(args[op == LM_WL_POINTER_EVT_ENTER ? 3 : 2].f);
        int hb = 0, hc = -1;
        struct wlrail L;
        files_rail_like(a, &L);
        int k = wlrail_at(&L, w->rpx, w->rpy);
        if (k >= 0 && !a->rail_idle[k])
            hb = a->rail_ids[k];
        if (show_btn_at(a, a->px, a->py))
            hb = B_SHOW;
        if (a->prompt) {
            struct wlcard c;
            int fx, fy, fw, ly, crows;
            card_layout(a, &c, &fx, &fy, &fw, &ly, &crows);
            hc = wlcard_button_at(&c, 2, (int)a->px, (int)a->py);
            w->redraw = true;                                               /* (the list rows light under the pointer) */
        }
        if (hb != a->hover_btn || hc != a->hover_card) {
            a->hover_btn = hb;
            a->hover_card = hc;
            w->redraw = true;
        }
        if (a->csb_drag >= 0 && a->prompt == P_OPEN) {
            struct wlcard c;
            int fx, fy, fw, ly, crows, idx[4001];
            card_layout(a, &c, &fx, &fy, &fw, &ly, &crows);
            int n = card_rows(a, idx, 4000), th = LM_MAX((crows * CARD_ROW_H) * crows / LM_MAX(n, 1), 20);
            if (n > crows)
                a->ctop = LM_CLAMP((int)((a->py - ly - 4 - a->csb_drag) * (n - crows) / LM_MAX(crows * CARD_ROW_H - th, 1)), 0, n - crows);
            w->redraw = true;
        }
        if (a->drag_bar >= 0) {
            int n = list_count(a), vis = rows_visible(a), h = w->bh - FOOT_H - list_y(), th = LM_MAX(h * vis / LM_MAX(n, 1), 24);
            int span = LM_MAX(h - th, 1);
            a->top = LM_CLAMP((int)((a->py - list_y() - a->drag_bar) * (n - vis) / span), 0, LM_MAX(n - vis, 0));
            w->redraw = true;
        }
    } else if (op == LM_WL_POINTER_EVT_AXIS && args[1].u == 0 && a->prompt) {
        if (a->prompt == P_OPEN)
            a->ctop = LM_MAX(a->ctop + (args[2].f > 0 ? 3 : -3), 0);
        w->redraw = true;
    } else if (op == LM_WL_POINTER_EVT_AXIS && args[1].u == 0) {
        double v = lm_fixed_to_double(args[2].f);
        a->top += v > 0 ? 3 : v < 0 ? -3 : 0;
        a->top = LM_CLAMP(a->top, 0, LM_MAX(list_count(a) - rows_visible(a), 0));
        w->redraw = true;
    } else if (op == LM_WL_POINTER_EVT_BUTTON) {
        a->clip.serial = args[0].u;
        uint32_t t = args[1].u, button = args[2].u, state = args[3].u;
        if (state == 0) {
            a->drag_bar = -1;
            a->csb_drag = -1;
            wlrail_release(&a->ran);
            w->redraw = true;
            return;
        }
        if (a->prompt) {
            struct wlcard c;
            int fx, fy, fw, ly, crows;
            card_layout(a, &c, &fx, &fy, &fw, &ly, &crows);
            int k = wlcard_button_at(&c, 2, (int)a->px, (int)a->py);
            if (button == 272 && k == 0)
                a->prompt = 0;
            else if (button == 272 && k == 1)
                prompt_accept(a);
            else if (button == 272 && inside(a->px, a->py, fx, fy, fw, 28))
                wltext_click(a->field, (int)a->px - (fx + 12), (w->mods & LM_MOD_SHIFT) != 0);
            else if (button == 273 && inside(a->px, a->py, fx, fy, fw, 28))
                wlwin_menu(w, wltext_menu(a->field));
            else if (button == 272 && crows && inside(a->px, a->py, fx, ly + 4, fw, crows * CARD_ROW_H)) {
                int idx[4001];
                int n = card_rows(a, idx, 4000), row = a->ctop + ((int)a->py - ly - 4) / CARD_ROW_H;
                int sbw = lm_metrics.scrollbar_width;
                if (n > crows && a->px >= fx + fw - sbw - 4) {                   /* the scroll bar: the thumb is dragged, the track pages (it is no row) */
                    int th = LM_MAX((crows * CARD_ROW_H) * crows / n, 20), ty = ly + 4 + (crows * CARD_ROW_H - th) * a->ctop / LM_MAX(n - crows, 1);
                    if (a->py >= ty && a->py < ty + th)
                        a->csb_drag = (int)a->py - ty;
                    else
                        a->ctop = LM_CLAMP(a->ctop + (a->py < ty ? -crows : crows), 0, n - crows);
                } else if (row < n) {
                    bool dbl = t - a->cclick_ms < (uint32_t)lm_metrics.double_click_ms;
                    a->cclick_ms = t;
                    bool file = idx[row] >= 0 && !a->cent[idx[row]].dir, was = a->csel == row;
                    card_put(a, idx[row]);
                    if (file) {
                        a->csel = row;                                      /* (the list stays: the field has the whole name now) */
                        if (dbl && was)
                            prompt_accept(a);
                    }
                }
            } else if (button == 272 && !wlcard_has(&c, (int)a->px, (int)a->py))
                a->prompt = 0;
            w->redraw = true;
            return;
        }
        if (button != 272)
            return;
        struct wlrail L;
        files_rail_like(a, &L);
        int rk = wlrail_at(&L, w->rpx, w->rpy);
        if (rk >= 0) {
            if (!a->rail_idle[rk]) {
                wlrail_press(&a->ran, rk, now_ms());
                do_button(a, a->rail_ids[rk]);
            }
            return;
        }
        if (a->job != J_NONE)
            return;
        if (inside(a->px, a->py, 6, w->bh - FOOT_H + 22, 330, 20)) {
            do_button(a, B_REPLACE);
            return;
        }
        if (show_btn_at(a, a->px, a->py)) {
            do_button(a, B_SHOW);
            return;
        }
        if (inside(a->px, a->py, 0, 0, w->bw - lm_metrics.scrollbar_width, HEAD_H) && !a->newmode) {     /* a click on a column label sorts */
            bool wide = w->bw >= 560;
            int right = w->bw - lm_metrics.scrollbar_width, nx = (wide ? col_x(a, 1) : right - 110) - 70;
            int ns = -1;
            if (wide && a->px >= col_x(a, 3))
                ns = SORT_TIME;
            else if (a->px >= (wide ? col_x(a, 1) : right - 110) - 4 && w->bw >= 420)
                ns = SORT_SIZE;
            else if (a->px >= nx - 4 && w->bw >= 420)
                ns = SORT_NAME;
            if (ns >= 0) {
                if (a->sort == ns)
                    a->sort_desc = !a->sort_desc;
                else {
                    a->sort = ns;
                    a->sort_desc = ns != SORT_NAME;
                }
                build_rows(a);
                w->redraw = true;
            }
            return;
        }
        int n = list_count(a), vis = rows_visible(a);
        int bar_h = w->bh - FOOT_H - list_y();
        if (a->px >= w->bw - lm_metrics.scrollbar_width && a->py >= list_y() && a->py < list_y() + bar_h && n > vis) {
            int th = LM_MAX(bar_h * vis / n, 24), ty = list_y() + (bar_h - th) * a->top / LM_MAX(n - vis, 1);
            if (a->py >= ty && a->py < ty + th)
                a->drag_bar = (int)a->py - ty;
            else
                a->top = LM_CLAMP(a->top + (a->py < ty ? -vis : vis), 0, n - vis);
            w->redraw = true;
            return;
        }
        int r = row_at(a, a->py);
        if (r < 0) {
            w->redraw = true;
            return;
        }
        if (a->newmode) {
            a->item_cur = r;
            w->redraw = true;
            return;
        }
        bool ctrl = (w->mods & LM_MOD_CTRL) != 0, shift = (w->mods & LM_MOD_SHIFT) != 0;
        bool dbl = r == a->last_click_row && t - a->last_click_ms <= (uint32_t)lm_metrics.double_click_ms;
        a->last_click_ms = t;
        a->last_click_row = r;
        if (dbl && !ctrl && !shift) {
            activate_row(a, r);
            a->last_click_row = -1;
        } else if (shift) {                                                  /* Shift: mark everything from the last click to this row */
            a->cur = r;
            select_range(a, a->anchor, r);
        } else if (ctrl) {                                                   /* Ctrl: mark or unmark this row */
            a->cur = r;
            a->anchor = r;
            a->sel[r] ^= 1;
        } else {                                                             /* a plain click moves the cursor, like in the file manager */
            a->cur = r;
            a->anchor = r;
        }
        w->redraw = true;
    }
}
static void menu_event(struct wlwin *w, int index)
{
    struct app *a = w->data;
    if (a->prompt)
        wltext_menu_do(&a->clip, a->field, sizeof(a->field), index);
    w->redraw = true;
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    struct app *a = w->data;
    wlclip_global(&a->clip, w, name, iface, ver);
}

/* ---- command line ---- */
static int cli_report(const struct aio_report *r)
{
    for (int i = 0; i < r->nerr; i++)
        fprintf(stderr, "mellagui-archive: %s\n", r->err[i]);
    if (r->failed > r->nerr)
        fprintf(stderr, "mellagui-archive: ... and %d more problems\n", r->failed - r->nerr);
    return r->failed ? 1 : 0;
}

static int cli_list(const char *path)
{
    struct aio *a;
    char err[300] = "";
    int r = aio_open(path, &a, err, sizeof(err));
    if (r < 0) {
        fprintf(stderr, "mellagui-archive: %s\n", r == -1 ? "not an archive" : err);
        return r == -1 ? 1 : 2;
    }
    printf("kind=%s entries=%d extractable=%d note=%s\n", a->kind, a->n, a->extractable, a->note);
    for (int i = 0; i < a->n; i++) {
        const struct aio_entry *e = &a->e[i];
        printf("%d\t%s\t%lld\t%o\t%lld\t%s\n", e->type, e->path ? e->path : "", (long long)e->size, e->mode, (long long)e->mtime, e->link ? e->link : "");
    }
    aio_close(a);
    return 0;
}

static int cli_extract(const char *path, const char *dest, bool overwrite, bool test)
{
    struct aio *a;
    char err[300] = "", d[PATH_MAX_LEN];
    int r = aio_open(path, &a, err, sizeof(err));
    if (r < 0) {
        fprintf(stderr, "mellagui-archive: %s\n", r == -1 ? "not an archive" : err);
        return r == -1 ? 1 : 2;
    }
    if (dest)
        expand_home(dest, d, sizeof(d));
    else {
        struct app tmp;
        memset(&tmp, 0, sizeof(tmp));
        snprintf(tmp.arc_path, sizeof(tmp.arc_path), "%s", path);
        default_dest(&tmp, d, sizeof(d));
    }
    struct aio_opts o = { .overwrite = overwrite, .test_only = test };
    struct aio_report rep;
    struct aio_progress p;
    aio_progress_init(&p);
    r = aio_extract(a, NULL, 0, d, &o, &p, &rep, err, sizeof(err));
    aio_progress_free(&p);
    if (r < 0) {
        fprintf(stderr, "mellagui-archive: %s\n", err);
        aio_close(a);
        return 2;
    }
    printf("%s: %d files, %d folders, %d links, %d kept, %d problems, %llu bytes\n", test ? "tested" : d, rep.extracted, rep.dirs, rep.links, rep.skipped, rep.failed, (unsigned long long)rep.bytes);
    aio_close(a);
    return cli_report(&rep);
}

static int cli_create(const char *out, char **paths, int n, int level, bool overwrite)
{
    int f = aio_format_of(out);
    if (f < 0) {
        fprintf(stderr, "mellagui-archive: the name must end in .zip, .tar, .tar.gz or .tgz\n");
        return 1;
    }
    struct aio_report rep;
    struct aio_progress p;
    aio_progress_init(&p);
    char err[300] = "";
    int r = aio_create(out, f, (const char *const *)paths, n, level, overwrite, &p, &rep, err, sizeof(err));
    aio_progress_free(&p);
    if (r < 0) {
        fprintf(stderr, "mellagui-archive: %s\n", err);
        return 2;
    }
    printf("%s: %d files, %d folders, %d links, %d problems\n", out, rep.extracted, rep.dirs, rep.links, rep.failed);
    return cli_report(&rep);
}

static int selftest(void)
{
    int bad = 0;
    char out[64];
    human(999, out, sizeof(out));
    bad += strcmp(out, "999 B") != 0;
    human(1536000, out, sizeof(out));
    bad += strcmp(out, "1.5 MB") != 0;
    stem_of("/x/y/data.tar.gz", out, sizeof(out));
    bad += strcmp(out, "data") != 0;
    stem_of("a.zip", out, sizeof(out));
    bad += strcmp(out, "a") != 0;
    bad += strcmp(norm_path("./a/b"), "a/b") != 0 || strcmp(norm_path("/a"), "a") != 0;
    bad += aio_format_of("x.TAR.GZ") != AIO_TARGZ || aio_format_of("x.tgz") != AIO_TARGZ || aio_format_of("x.zip") != AIO_ZIP || aio_format_of("x.txt") != -1;
    bad += lm_crc32(0, (const uint8_t *)"123456789", 9) != 0xcbf43926u;
    if (bad)
        fprintf(stderr, "mellagui-archive: selftest failed (%d)\n", bad);
    else
        printf("mellagui-archive selftest ok\n");
    return bad != 0;
}

int main(int argc, char **argv)
{
    wlwin_hidpi();
    lm_theme_load_env();
    signal(SIGPIPE, SIG_IGN);
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();
    if (argc > 2 && !strcmp(argv[1], "--list"))
        return cli_list(argv[2]);
    if (argc > 2 && (!strcmp(argv[1], "--test") || !strcmp(argv[1], "--extract"))) {
        const char *dest = NULL;
        bool ow = false;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--overwrite"))
                ow = true;
            else if (!dest)
                dest = argv[i];
        }
        return cli_extract(argv[2], dest, ow, !strcmp(argv[1], "--test"));
    }
    if (argc > 3 && !strcmp(argv[1], "--create")) {
        int level = 6, n = 0;
        bool ow = false;
        char **paths = calloc((size_t)argc, sizeof(char *));
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--level") && i + 1 < argc)
                level = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--overwrite"))
                ow = true;
            else
                paths[n++] = argv[i];
        }
        int r = cli_create(argv[2], paths, n, level, ow);
        free(paths);
        return r;
    }
    struct app *a = &g_app;
    a->hover_btn = 0;
    a->hover_card = -1;
    a->drag_bar = -1;
    a->last_click_row = -1;
    a->csb_drag = -1;
    a->dump = getenv("MELLAGUI_ARCHIVE_DUMP");
    aio_progress_init(&a->prog);
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event, .menu = menu_event };
    if (wlwin_connect(&a->win, "mellagui-archive", &ops, a) < 0)
        return 1;
    wlrail_init(&a->ran);
    wlwin_set_inset(&a->win, WLRAIL_W);
    snprintf(a->title, sizeof(a->title), "Archiver");
    if (wlwin_open(&a->win, "Archiver", "mellagui.archive", 760, 480, 440, 300) < 0)
        return 1;
    wlclip_attach(&a->clip, &a->win);
    if (argc > 2 && !strcmp(argv[1], "--new")) {
        for (int i = 2; i < argc; i++)
            add_item(a, argv[i]);
        enter_new_mode(a);
    } else if (argc > 1 && argv[1][0] != '-') {
        open_archive(a, argv[1]);
    } else {
        set_status(a, false, "Open an archive (Ctrl+O), or make a new one (Ctrl+N).");
    }
    if (wlc_roundtrip(a->win.c) < 0) {
        wlwin_destroy(&a->win);
        return 1;
    }
    for (;;) {
        int timeout = a->job != J_NONE ? 80 : -1;
        struct wlrail L;
        files_rail_like(a, &L);
        if (wlrail_step(&a->ran, &L, rail_hover_index(a), now_ms())) {         /* the drops of the rail are moving: a frame every 16 ms */
            a->win.redraw = true;
            timeout = timeout < 0 ? 16 : LM_MIN(timeout, 16);
        }
        if (!wlwin_step(&a->win, timeout))
            break;
        if (a->job != J_NONE) {
            a->win.redraw = true;
            if (atomic_load(&a->job_done))
                job_finish(a);
        }
    }
    if (a->job != J_NONE) {
        a->prog.cancel = 1;
        pthread_join(a->thread, NULL);
    }
    close_archive(a);
    aio_progress_free(&a->prog);
    wlwin_destroy(&a->win);
    return 0;
}
