/*
 * mellagui-files - a small file browser for MellaGUI.
 *
 *   mellagui-files [DIR]          browse DIR (default: the home directory)
 *   mellagui-files --selftest     the listing / sorting / classification without a display
 *
 * Folders come first, then files, each sorted by name without regard to case; ".." goes up. Hidden files (names with a leading
 * dot) are shown with Ctrl+H. Opening a file starts a sibling program: images (png, ppm) in mellagui-view, anything that looks
 * like text in mellagui-edit; binary files are not opened. Ctrl+T starts mellagui-term in the folder being shown.
 *
 * Keys: Up / Down / PageUp / PageDown / Home / End, Enter or Right opens, Backspace / Left / Alt+Up goes to the parent folder,
 * Ctrl+H hidden files, F5 / Ctrl+R reread, Ctrl+T terminal here, Ctrl+L type a path (Enter goes / opens it, Esc cancels), a
 * letter jumps to the next name starting with it, Esc clears the message. Mouse: click selects, double click opens, wheel
 * scrolls, the toolbar has Up, Home, Hidden, Terminal. Nothing here changes or deletes files.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "theme.h"
#include "font.h"
#include "keymap.h"
#include "protocol.h"
#include "util.h"
#include "wlwin.h"

#define MAX_ENTRIES 100000
#define ROW_H 18
#define TOOL_H 28
#define HEAD_H 20
#define STATUS_H 20
#define COL_BG (lm_theme.background)
#define COL_FG (lm_theme.text_soft)
#define COL_DIM (lm_theme.text_dim)
#define COL_DIR (lm_theme.accent)
#define COL_SEL (lm_theme.button_hover)
#define COL_SEL_IDLE (lm_theme.bar)
#define COL_BAR (lm_theme.bar)
#define COL_BTN 0x353b49u
#define COL_BTN_ON (lm_theme.button_hover)
#define COL_WARN (lm_theme.caution)
struct entry {
    char *name;
    bool dir;
    bool broken;                        /* a link to nothing */
    bool hidden;
    off_t size;
    time_t mtime;
};

struct button {
    const char *label;
    int x, w;
};

struct app {
    struct wlwin win;
    char cwd[PATH_MAX];
    struct entry *e;
    int n, cap;
    int sel, top;
    bool show_hidden;
    bool prompt;
    char pbuf[PATH_MAX];
    char msg[256];
    int64_t msg_until;
    int rows_visible;
    double px, py;
    uint32_t last_click_ms;
    int last_click_row;
    uint32_t rep_code;
    int64_t rep_next;
    int rep_rate, rep_delay;
};

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- listing ------------------------------------------------------------------------------------------------------------ */

static void entries_free(struct app *a)
{
    for (int i = 0; i < a->n; i++)
        free(a->e[i].name);
    a->n = 0;
}

static int entry_cmp(const void *pa, const void *pb)
{
    const struct entry *x = pa, *y = pb;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

static bool add_entry(struct app *a, const char *name, bool dir, bool broken, off_t size, time_t mtime)
{
    if (a->n >= MAX_ENTRIES)
        return false;
    if (a->n == a->cap) {
        int nc = a->cap ? a->cap * 2 : 64;
        struct entry *t = realloc(a->e, (size_t)nc * sizeof(*t));
        if (!t)
            return false;
        a->e = t;
        a->cap = nc;
    }
    char *d = strdup(name);
    if (!d)
        return false;
    a->e[a->n++] = (struct entry){ d, dir, broken, name[0] == '.', size, mtime };
    return true;
}

/* Reads `dir` into the list (hidden files only when show_hidden). 0, or -1 with the reason in err; the old list stays then. */
static int list_dir(struct app *a, const char *dir, char *err, size_t errsz)
{
    DIR *d = opendir(dir);
    if (!d) {
        snprintf(err, errsz, "cannot open %s: %s", dir, strerror(errno));
        return -1;
    }
    struct app tmp = { 0 };
    tmp.show_hidden = a->show_hidden;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (de->d_name[0] == '.' && !a->show_hidden)
            continue;
        char path[PATH_MAX + 1];
        if (snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) >= (int)sizeof(path))
            continue;
        struct stat st;
        bool broken = false;
        if (stat(path, &st) < 0) {
            if (lstat(path, &st) < 0)
                continue;
            broken = true;
        }
        add_entry(&tmp, de->d_name, !broken && S_ISDIR(st.st_mode), broken, st.st_size, st.st_mtime);
    }
    closedir(d);
    qsort(tmp.e, (size_t)tmp.n, sizeof(tmp.e[0]), entry_cmp);
    entries_free(a);
    free(a->e);
    a->e = tmp.e;
    a->n = tmp.n;
    a->cap = tmp.cap;
    return 0;
}

/* Row 0 is ".." (except in "/"), the rest are the entries. */
static bool has_up(const struct app *a)
{
    return strcmp(a->cwd, "/") != 0;
}

static int rows(const struct app *a)
{
    return a->n + (has_up(a) ? 1 : 0);
}

static const struct entry *row_entry(const struct app *a, int row)
{
    int i = row - (has_up(a) ? 1 : 0);
    return i >= 0 && i < a->n ? &a->e[i] : NULL;
}

static const char *row_name(const struct app *a, int row)
{
    const struct entry *e = row_entry(a, row);
    return e ? e->name : "..";
}

/* ---- paths -------------------------------------------------------------------------------------------------------------- */

/* Collapses ., .. and repeated slashes of an absolute path (without touching the file system). */
static void normalize(const char *in, char *out, size_t outsz)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", in);
    size_t len = 0;
    char *save = NULL;
    out[0] = 0;
    for (char *tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (!strcmp(tok, ".") || !*tok)
            continue;
        if (!strcmp(tok, "..")) {
            while (len > 0 && out[len - 1] != '/')
                len--;
            if (len > 0)
                len--;
            out[len] = 0;
            continue;
        }
        size_t tl = strlen(tok);
        if (len + 1 + tl + 1 > outsz)
            break;
        out[len++] = '/';
        memcpy(out + len, tok, tl + 1);
        len += tl;
    }
    if (!out[0])
        snprintf(out, outsz, "/");
}

static void join_path(const char *dir, const char *name, char *out, size_t outsz)
{
    char raw[PATH_MAX * 2];
    if (name[0] == '/')
        snprintf(raw, sizeof(raw), "%s", name);
    else
        snprintf(raw, sizeof(raw), "%s/%s", dir, name);
    normalize(raw, out, outsz);
}

/* ---- what a file is ------------------------------------------------------------------------------------------------------- */

enum kind { K_IMAGE, K_TEXT, K_BINARY, K_UNREADABLE };

static bool ext_is(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    return dot && dot != name && !strcasecmp(dot + 1, ext);
}

/* Images by extension; everything else by looking at the first 4 KB: a NUL byte means binary. */
static enum kind classify(const char *path)
{
    if (ext_is(path, "png") || ext_is(path, "ppm"))
        return K_IMAGE;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return K_UNREADABLE;
    unsigned char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n < 0)
        return K_UNREADABLE;
    return memchr(buf, 0, (size_t)n) ? K_BINARY : K_TEXT;
}

static void human_size(off_t size, char *out, size_t n)
{
    static const char *const unit[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)size;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    if (u == 0)
        snprintf(out, n, "%lld B", (long long)size);
    else
        snprintf(out, n, v < 10 ? "%.1f %s" : "%.0f %s", v, unit[u]);
}

/* ---- starting other programs ---------------------------------------------------------------------------------------------- */

/* The program `name` next to this binary when it exists, else whatever PATH has. */
static void sibling(const char *name, char *out, size_t outsz)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            snprintf(slash + 1, sizeof(exe) - (size_t)(slash + 1 - exe), "%s", name);
            if (access(exe, X_OK) == 0) {
                snprintf(out, outsz, "%s", exe);
                return;
            }
        }
    }
    snprintf(out, outsz, "%s", name);
}

/* Starts `prog [arg]` in `cwd`. false when it could not be executed (reported through a close-on-exec pipe). */
static bool launch(const char *prog, const char *arg, const char *cwd, char *err, size_t errsz)
{
    char path[PATH_MAX];
    sibling(prog, path, sizeof(path));
    int pf[2];
    if (pipe2(pf, O_CLOEXEC) < 0) {
        snprintf(err, errsz, "pipe: %s", strerror(errno));
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pf[0]);
        close(pf[1]);
        snprintf(err, errsz, "fork: %s", strerror(errno));
        return false;
    }
    if (pid == 0) {
        setsid();
        if (cwd && chdir(cwd) < 0) {
            int e = errno;
            if (write(pf[1], &e, sizeof(e)) < 0) {}
            _exit(127);
        }
        char *argv[3] = { path, (char *)arg, NULL };
        execvp(path, argv);
        int e = errno;
        if (write(pf[1], &e, sizeof(e)) < 0) {}
        _exit(127);
    }
    close(pf[1]);
    int e = 0;
    ssize_t k = read(pf[0], &e, sizeof(e));
    close(pf[0]);
    if (k == (ssize_t)sizeof(e)) {
        waitpid(pid, NULL, 0);
        snprintf(err, errsz, "cannot start %s: %s", prog, strerror(e));
        return false;
    }
    return true;
}

/* ---- the window state ---------------------------------------------------------------------------------------------------- */

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

static void set_title(struct app *a)
{
    if (!a->win.toplevel)
        return;
    const char *base = strrchr(a->cwd, '/') ? strrchr(a->cwd, '/') + 1 : a->cwd;
    char t[PATH_MAX + 16];
    snprintf(t, sizeof(t), "%s - Files", *base ? base : "/");
    WLC_SEND(a->win.c, a->win.toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = t });
}

/* Shows `dir` (an absolute, normalized path), selecting the row called `select` when there is one, else the first entry. */
static bool go_to(struct app *a, const char *dir, const char *select)
{
    char err[PATH_MAX + 80];
    if (list_dir(a, dir, err, sizeof(err)) < 0) {
        msg(a, "%s", err);
        return false;
    }
    snprintf(a->cwd, sizeof(a->cwd), "%s", dir);
    a->sel = rows(a) > (has_up(a) ? 1 : 0) ? (has_up(a) ? 1 : 0) : 0;
    a->top = 0;
    if (select)
        for (int r = 0; r < rows(a); r++)
            if (!strcmp(row_name(a, r), select)) {
                a->sel = r;
                break;
            }
    a->msg[0] = 0;
    set_title(a);
    a->win.redraw = true;
    return true;
}

static void reread(struct app *a)
{
    char keep[NAME_MAX + 1];
    snprintf(keep, sizeof(keep), "%s", row_name(a, a->sel));
    int top = a->top;
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", a->cwd);
    if (go_to(a, dir, keep))
        a->top = top;
}

static void go_up(struct app *a)
{
    if (!has_up(a))
        return;
    char parent[PATH_MAX];
    join_path(a->cwd, "..", parent, sizeof(parent));
    const char *base = strrchr(a->cwd, '/');
    char from[NAME_MAX + 1];
    snprintf(from, sizeof(from), "%s", base ? base + 1 : "");
    go_to(a, parent, from);
}

static void open_path(struct app *a, const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        msg(a, "%s: %s", path, strerror(errno));
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        go_to(a, path, NULL);
        return;
    }
    char err[PATH_MAX + 80];
    switch (classify(path)) {
    case K_IMAGE:
        if (!launch("mellagui-view", path, a->cwd, err, sizeof(err)))
            msg(a, "%s", err);
        break;
    case K_TEXT:
        if (!launch("mellagui-edit", path, a->cwd, err, sizeof(err)))
            msg(a, "%s", err);
        break;
    case K_BINARY: msg(a, "%s looks like a binary file: not opened", strrchr(path, '/') + 1); break;
    case K_UNREADABLE: msg(a, "cannot read %s: %s", path, strerror(errno)); break;
    }
}

static void open_selected(struct app *a)
{
    if (a->sel == 0 && has_up(a)) {
        go_up(a);
        return;
    }
    const struct entry *e = row_entry(a, a->sel);
    if (!e)
        return;
    char path[PATH_MAX];
    join_path(a->cwd, e->name, path, sizeof(path));
    if (e->broken) {
        msg(a, "%s is a link to nothing", e->name);
        return;
    }
    open_path(a, path);
}

static void select_row(struct app *a, int row)
{
    a->sel = LM_CLAMP(row, 0, LM_MAX(rows(a) - 1, 0));
    if (a->sel < a->top)
        a->top = a->sel;
    if (a->sel >= a->top + a->rows_visible)
        a->top = a->sel - a->rows_visible + 1;
    a->top = LM_MAX(a->top, 0);
    a->win.redraw = true;
}

static void start_terminal(struct app *a)
{
    char err[PATH_MAX + 80];
    if (!launch("mellagui-term", NULL, a->cwd, err, sizeof(err)))
        msg(a, "%s", err);
}

static void toggle_hidden(struct app *a)
{
    a->show_hidden = !a->show_hidden;
    reread(a);
    msg(a, a->show_hidden ? "Hidden files shown" : "Hidden files hidden");
}

static void go_home(struct app *a)
{
    const char *home = getenv("HOME");
    char dir[PATH_MAX];
    normalize(home && *home ? home : "/", dir, sizeof(dir));
    go_to(a, dir, NULL);
}

/* ---- toolbar ------------------------------------------------------------------------------------------------------------- */

enum { B_UP, B_HOME, B_HIDDEN, B_TERM, N_BUTTONS };

static void toolbar(const struct app *a, struct button *b)
{
    static const char *const labels[N_BUTTONS] = { "Up", "Home", "Hidden", "Terminal" };
    int x = 6;
    for (int i = 0; i < N_BUTTONS; i++) {
        b[i].label = labels[i];
        b[i].w = (int)strlen(labels[i]) * LM_FONT_W + 16;
        b[i].x = x;
        x += b[i].w + 6;
    }
    (void)a;
}

static void press_button(struct app *a, int i)
{
    switch (i) {
    case B_UP: go_up(a); break;
    case B_HOME: go_home(a); break;
    case B_HIDDEN: toggle_hidden(a); break;
    case B_TERM: start_terminal(a); break;
    }
}

/* ---- keys ---------------------------------------------------------------------------------------------------------------- */

static void jump_to_letter(struct app *a, char ch)
{
    int n = rows(a);
    for (int k = 1; k <= n; k++) {
        int r = (a->sel + k) % n;
        const char *nm = row_name(a, r);
        if (r == 0 && has_up(a))
            continue;
        if (tolower((unsigned char)nm[0]) == tolower((unsigned char)ch)) {
            select_row(a, r);
            return;
        }
    }
}

static void prompt_accept(struct app *a)
{
    char text[PATH_MAX], full[PATH_MAX];
    snprintf(text, sizeof(text), "%s", a->pbuf);
    a->prompt = false;
    a->win.redraw = true;
    if (!text[0])
        return;
    if (text[0] == '~' && (text[1] == '/' || !text[1])) {
        const char *home = getenv("HOME");
        char t2[PATH_MAX];
        snprintf(t2, sizeof(t2), "%.*s%s", (int)(sizeof(t2) / 2), home ? home : "", text + 1);
        snprintf(text, sizeof(text), "%s", t2);
    }
    join_path(a->cwd, text, full, sizeof(full));
    struct stat st;
    if (stat(full, &st) < 0) {
        msg(a, "%s: %s", full, strerror(errno));
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        go_to(a, full, NULL);
    } else {                                                           /* a file: show its folder with it selected, and open it */
        char dir[PATH_MAX];
        join_path(full, "..", dir, sizeof(dir));
        go_to(a, dir, strrchr(full, '/') + 1);
        open_path(a, full);
    }
}

static void handle_key(struct app *a, uint32_t ks)
{
    bool ctrl = (a->win.mods & LM_MOD_CTRL) != 0, alt = (a->win.mods & LM_MOD_ALT) != 0;
    bool super = (a->win.mods & LM_MOD_SUPER) != 0;
    char txt[8];
    int tl = lm_keysym_to_utf8(ks, txt);
    if (a->prompt) {
        size_t len = strlen(a->pbuf);
        if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
            prompt_accept(a);
        } else if (ks == LM_KS_Escape) {
            a->prompt = false;
        } else if (ks == LM_KS_BackSpace) {
            while (len > 0 && (a->pbuf[len - 1] & 0xc0) == 0x80)
                len--;
            if (len > 0)
                len--;
            a->pbuf[len] = 0;
        } else if (!ctrl && !alt && !super && tl > 0 && (unsigned char)txt[0] >= 0x20 && len + (size_t)tl < sizeof(a->pbuf)) {
            memcpy(a->pbuf + len, txt, (size_t)tl + 1);
        }
        a->win.redraw = true;
        return;
    }
    if (tl == 1 && ctrl && !alt && !super) {
        switch (txt[0] | 0x20) {
        case 'h': toggle_hidden(a); return;
        case 'r': reread(a); return;
        case 't': start_terminal(a); return;
        case 'l':
            a->prompt = true;
            snprintf(a->pbuf, sizeof(a->pbuf), "%.*s%s", (int)sizeof(a->pbuf) - 2, a->cwd, strcmp(a->cwd, "/") ? "/" : "");
            a->win.redraw = true;
            return;
        default: return;
        }
    }
    switch (ks) {
    case LM_KS_Up:
        if (alt)
            go_up(a);
        else
            select_row(a, a->sel - 1);
        return;
    case LM_KS_Down: select_row(a, a->sel + 1); return;
    case LM_KS_Page_Up: select_row(a, a->sel - LM_MAX(a->rows_visible - 1, 1)); return;
    case LM_KS_Page_Down: select_row(a, a->sel + LM_MAX(a->rows_visible - 1, 1)); return;
    case LM_KS_Home: select_row(a, 0); return;
    case LM_KS_End: select_row(a, rows(a) - 1); return;
    case LM_KS_Return:
    case LM_KS_KP_Enter:
    case LM_KS_Right: open_selected(a); return;
    case LM_KS_BackSpace:
    case LM_KS_Left: go_up(a); return;
    case LM_KS_F1 + 4: reread(a); return;                              /* F5 */
    case LM_KS_Escape: a->msg[0] = 0; a->win.redraw = true; return;
    default: break;
    }
    if (!ctrl && !alt && !super && tl == 1 && isalnum((unsigned char)txt[0]))
        jump_to_letter(a, txt[0]);
}

/* ---- drawing ------------------------------------------------------------------------------------------------------------- */

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    int list_y = TOOL_H + HEAD_H;
    int bars = a->prompt ? STATUS_H * 2 : STATUS_H;
    a->rows_visible = LM_MAX((w->bh - list_y - bars) / ROW_H, 1);
    if (a->sel >= a->top + a->rows_visible)
        a->top = a->sel - a->rows_visible + 1;
    if (a->sel < a->top)
        a->top = a->sel;
    wlwin_fill(w, 0, 0, w->bw, w->bh, COL_BG);

    wlwin_fill(w, 0, 0, w->bw, TOOL_H, COL_BAR);
    struct button b[N_BUTTONS];
    toolbar(a, b);
    for (int i = 0; i < N_BUTTONS; i++) {
        bool on = i == B_HIDDEN && a->show_hidden;
        wlwin_fill(w, b[i].x, 4, b[i].w, TOOL_H - 8, on ? COL_BTN_ON : COL_BTN);
        wlwin_text(w, b[i].x + 8, 6, b[i].label, 1, COL_FG, 0);
    }
    int px = b[N_BUTTONS - 1].x + b[N_BUTTONS - 1].w + 16;
    wlwin_text(w, px, 6, a->cwd, 1, COL_DIM, w->bw - px - 6);

    int size_x = LM_MAX(w->bw - 250, 200), time_x = LM_MAX(w->bw - 140, 330);
    wlwin_fill(w, 0, TOOL_H, w->bw, HEAD_H, COL_BG);
    wlwin_text(w, 10, TOOL_H + 2, "Name", 1, COL_DIM, 0);
    wlwin_text(w, size_x, TOOL_H + 2, "Size", 1, COL_DIM, 0);
    wlwin_text(w, time_x, TOOL_H + 2, "Modified", 1, COL_DIM, 0);
    wlwin_fill(w, 0, list_y - 1, w->bw, 1, COL_BAR);

    int total = rows(a);
    for (int r = 0; r < a->rows_visible; r++) {
        int row = a->top + r;
        if (row >= total)
            break;
        const struct entry *e = row_entry(a, row);
        int y = list_y + r * ROW_H;
        if (row == a->sel)
            wlwin_fill(w, 0, y, w->bw - 8, ROW_H, COL_SEL);
        bool dir = !e || e->dir;
        uint32_t col = !e || e->dir ? COL_DIR : e->broken ? COL_DIM : COL_FG;
        char name[NAME_MAX + 4];
        snprintf(name, sizeof(name), "%s%s", e ? e->name : "..", dir ? "/" : "");
        wlwin_text(w, 10, y + 1, name, 1, col, size_x - 20);
        if (e && !e->dir && !e->broken) {
            char sz[32];
            human_size(e->size, sz, sizeof(sz));
            wlwin_text(w, size_x, y + 1, sz, 1, COL_DIM, 0);
        }
        if (e) {
            char tm[32];
            struct tm lt;
            localtime_r(&e->mtime, &lt);
            strftime(tm, sizeof(tm), "%Y-%m-%d %H:%M", &lt);
            wlwin_text(w, time_x, y + 1, tm, 1, COL_DIM, 0);
        }
    }
    if (total > a->rows_visible) {                                      /* a thin scroll bar */
        int track = a->rows_visible * ROW_H;
        int th = LM_MAX(track * a->rows_visible / total, 16);
        int ty = list_y + (track - th) * a->top / LM_MAX(total - a->rows_visible, 1);
        wlwin_fill(w, w->bw - 6, ty, 4, th, COL_BTN);
    }

    int sy = w->bh - STATUS_H;
    wlwin_fill(w, 0, sy, w->bw, STATUS_H, COL_BAR);
    bool showing = a->msg[0] && now_ms() < a->msg_until;
    char status[PATH_MAX + 64];
    snprintf(status, sizeof(status), "%d item%s%s", a->n, a->n == 1 ? "" : "s", a->show_hidden ? "  (hidden files shown)" : "");
    wlwin_text(w, 6, sy + 2, showing ? a->msg : status, 1, showing ? COL_WARN : COL_FG, w->bw - 12);
    if (a->prompt) {
        int py = sy - STATUS_H;
        wlwin_fill(w, 0, py, w->bw, STATUS_H, COL_BAR);
        wlwin_fill(w, 0, py, w->bw, 1, COL_DIR);
        char line[PATH_MAX + 16];
        snprintf(line, sizeof(line), "Go to: %s_", a->pbuf);
        wlwin_text(w, 6, py + 2, line, 1, COL_FG, w->bw - 12);
    }
}

/* ---- events --------------------------------------------------------------------------------------------------------------- */

static void click(struct app *a, uint32_t time_ms)
{
    int x = (int)a->px, y = (int)a->py;
    if (y < TOOL_H) {
        struct button b[N_BUTTONS];
        toolbar(a, b);
        for (int i = 0; i < N_BUTTONS; i++)
            if (x >= b[i].x && x < b[i].x + b[i].w && y >= 4 && y < TOOL_H - 4)
                press_button(a, i);
        return;
    }
    int list_y = TOOL_H + HEAD_H;
    if (y < list_y)
        return;
    int row = a->top + (y - list_y) / ROW_H;
    if (row >= rows(a) || (y - list_y) / ROW_H >= a->rows_visible)
        return;
    bool dbl = row == a->last_click_row && time_ms - a->last_click_ms < 400;
    a->last_click_ms = time_ms;
    a->last_click_row = row;
    select_row(a, row);
    if (dbl) {
        a->last_click_row = -1;
        open_selected(a);
    }
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        if (args[2].u == 272 && args[3].u == 1)
            click(a, args[1].u);
        w->redraw = true;
        break;
    case LM_WL_POINTER_EVT_AXIS:
        if (args[1].u == 0) {
            int lines = (int)(lm_fixed_to_double(args[2].f) / 15.0 * 3.0);
            if (lines == 0)
                lines = args[2].f > 0 ? 1 : -1;
            a->top = LM_CLAMP(a->top + lines, 0, LM_MAX(rows(a) - a->rows_visible, 0));
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
        uint32_t code = args[2].u;
        if (args[3].u == 0) {
            if (a->rep_code == code)
                a->rep_code = 0;
            break;
        }
        uint32_t ks = lm_keymap_keysym(code, w->mods);
        handle_key(a, ks);
        w->redraw = true;
        bool movement = ks == LM_KS_Up || ks == LM_KS_Down || ks == LM_KS_Page_Up || ks == LM_KS_Page_Down;
        if (movement && a->rep_rate > 0) {
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

/* ---- self test ------------------------------------------------------------------------------------------------------------- */

static int bad;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "selftest: line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); bad++; } } while (0)

static void touch(const char *dir, const char *name, const char *content, size_t n)
{
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (f) {
        fwrite(content, 1, n, f);
        fclose(f);
    }
}

static int selftest(void)
{
    char out[PATH_MAX];
    normalize("/a/b/../c//d/./", out, sizeof(out));
    CHECK(!strcmp(out, "/a/c/d"), "normalize: %s", out);
    normalize("/..", out, sizeof(out));
    CHECK(!strcmp(out, "/"), "normalize above the root: %s", out);
    normalize("/", out, sizeof(out));
    CHECK(!strcmp(out, "/"), "normalize the root: %s", out);
    join_path("/home/x", "../y", out, sizeof(out));
    CHECK(!strcmp(out, "/home/y"), "join relative: %s", out);
    join_path("/home/x", "/etc", out, sizeof(out));
    CHECK(!strcmp(out, "/etc"), "join absolute: %s", out);

    human_size(0, out, sizeof(out));
    CHECK(!strcmp(out, "0 B"), "size 0: %s", out);
    human_size(1536, out, sizeof(out));
    CHECK(!strcmp(out, "1.5 KB"), "size 1536: %s", out);
    human_size(20 * 1024 * 1024, out, sizeof(out));
    CHECK(!strcmp(out, "20 MB"), "size 20 MB: %s", out);

    CHECK(ext_is("a/b.PNG", "png") && !ext_is(".png", "png") && !ext_is("png", "png"), "extensions");

    char dir[] = "/tmp/mellagui-files-selftest-XXXXXX";
    if (!mkdtemp(dir)) {
        fprintf(stderr, "selftest: mkdtemp failed\n");
        return 1;
    }
    char p[PATH_MAX + 32];
    snprintf(p, sizeof(p), "%s/zeta", dir);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/Alpha", dir);
    mkdir(p, 0755);
    touch(dir, "beta.txt", "hello\n", 6);
    touch(dir, "Apple.txt", "x", 1);
    touch(dir, ".secret", "x", 1);
    touch(dir, "bin.dat", "a\0b", 3);
    touch(dir, "pic.PNG", "", 0);
    snprintf(p, sizeof(p), "%s/dangling", dir);
    if (symlink("/nonexistent-target", p) < 0) {}
    static struct app a;
    char err[200];
    CHECK(list_dir(&a, dir, err, sizeof(err)) == 0, "list: %s", err);
    const char *want[] = { "Alpha", "zeta", "Apple.txt", "beta.txt", "bin.dat", "dangling", "pic.PNG" };
    CHECK(a.n == 7, "%d entries without hidden files", a.n);
    for (int i = 0; i < a.n && i < 7; i++)
        CHECK(!strcmp(a.e[i].name, want[i]), "entry %d is %s, want %s", i, a.e[i].name, want[i]);
    CHECK(a.n >= 6 && a.e[5].broken && !a.e[5].dir, "a dangling link is marked");
    CHECK(a.n >= 2 && a.e[0].dir && a.e[1].dir && !a.e[2].dir, "folders first");
    a.show_hidden = true;
    CHECK(list_dir(&a, dir, err, sizeof(err)) == 0 && a.n == 8 && !strcmp(a.e[2].name, ".secret"), "hidden files listed and sorted");
    CHECK(list_dir(&a, "/nonexistent-dir", err, sizeof(err)) < 0 && a.n == 8, "a failed listing keeps the old one");

    snprintf(p, sizeof(p), "%s/beta.txt", dir);
    CHECK(classify(p) == K_TEXT, "text");
    snprintf(p, sizeof(p), "%s/bin.dat", dir);
    CHECK(classify(p) == K_BINARY, "binary");
    snprintf(p, sizeof(p), "%s/pic.PNG", dir);
    CHECK(classify(p) == K_IMAGE, "image by extension");
    snprintf(p, sizeof(p), "%s/nope", dir);
    CHECK(classify(p) == K_UNREADABLE, "unreadable");

    entries_free(&a);
    free(a.e);
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) {}
    puts(bad ? "selftest FAILED" : "selftest ok");
    return bad ? 1 : 0;
}

/* ---- main ----------------------------------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();
    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        fprintf(stderr, "usage: mellagui-files [DIR | --selftest]\n");
        return 2;
    }
    signal(SIGCHLD, SIG_IGN);                                           /* the programs we start are nobody's business afterwards */
    static struct app a;
    a.rep_rate = 25;
    a.rep_delay = 400;
    a.last_click_row = -1;
    static const struct wlwin_ops ops = { .paint = paint, .pointer = pointer_event, .keyboard = keyboard_event };
    if (wlwin_connect(&a.win, "mellagui-files", &ops, &a) < 0)
        return 1;
    if (wlwin_open(&a.win, "Files", "mellagui.files", 640, 460, 360, 160) < 0)
        return 1;
    char start[PATH_MAX], cwd[PATH_MAX];
    const char *home = getenv("HOME");
    if (argc == 2) {
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), "/");
        join_path(cwd, argv[1], start, sizeof(start));
    } else {
        normalize(home && *home ? home : "/", start, sizeof(start));
    }
    if (!go_to(&a, start, NULL) && !go_to(&a, "/", NULL)) {
        fprintf(stderr, "mellagui-files: cannot list %s\n", start);
        return 1;
    }
    if (strcmp(a.cwd, start) != 0)
        msg(&a, "Could not open %s", start);
    while (a.win.running) {
        int timeout = 250;
        if (a.rep_code) {
            int64_t wait = a.rep_next - now_ms();
            timeout = (int)LM_CLAMP(wait, 1, 250);
        }
        if (!wlwin_step(&a.win, timeout))
            break;
        if (a.rep_code && now_ms() >= a.rep_next) {
            handle_key(&a, lm_keymap_keysym(a.rep_code, a.win.mods));
            a.rep_next = now_ms() + (a.rep_rate > 0 ? 1000 / a.rep_rate : 40);
            a.win.redraw = true;
        }
        if (a.msg[0] && now_ms() >= a.msg_until) {
            a.msg[0] = 0;
            a.win.redraw = true;
        }
    }
    wlwin_destroy(&a.win);
    return 0;
}
