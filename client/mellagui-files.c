/*
 * mellagui-files - a two-pane file manager for MellaGUI, in the spirit of Norton / Midnight Commander.
 *
 *   mellagui-files [DIR]          two panes, both starting in DIR (default: the home directory)
 *   mellagui-files --choose KEY [DIR]
 *                                 pick a picture instead of opening files: one pane, Enter or a double click on a png / jpg / ppm sets
 *                                 the compositor setting KEY (only "wallpaper") to its full path and the program ends; Esc ends it
 *                                 without a choice. The settings panel's Desktop page starts it from its "Browse..." button.
 *   mellagui-files --selftest     listing, sorting, classification, copy / move / delete and the preview without a display
 *
 * Each pane lists a folder: folders first, then files, sorted by name without regard to case; ".." goes up. The pane with the
 * highlighted bar is the active one. One of the two panes can show a preview of the file under the cursor of the other (F3, Ctrl+Q or
 * the Preview button): text, a hex dump of binary files, a picture, or the inside of a folder.
 *
 * Keys: Tab switches the active pane; Up / Down / PageUp / PageDown / Home / End move; Enter or Right opens (a folder: goes in; png / jpg /
 * ppm and mp4 videos: mellagui-view; text: mellagui-edit; a program (an ELF file you may execute): it is started in this folder; other binary files are not opened); Backspace / Left / Alt+Up goes to the parent folder;
 * Insert or Space marks the file and moves down; F1 help, F2 rename, F3 preview, F4 edit, F5 copy, F6 move, F7 new folder,
 * F8 (or Delete) delete into the trash (Shift+F8 / Shift+Delete: for good; Ctrl+B opens the trash, Ctrl+Z puts the marked files in it back, Ctrl+E empties it), F9 terminal here, F10 quit; copy / move / delete work on the marked
 * files, else on the one under the cursor, copy and move go to the folder shown in the other pane (editable first, Tab completes names) and
 * never replace an existing file, delete asks first. They run in a thread: the window stays alive, the status line shows the progress and Esc
 * cancels.
 * Network drives: the Network button / Ctrl+N shows "smb://" (the servers you connected to, the router when there is one, and
 * "Connect to a server..."; Ctrl+K asks for a name, user@name or "name legacy"). A server's shares are folders, and everything works there as on
 * a local disk (F2 / F5 / F6 / F7 / F8, a file opens as a copy) through smbclient: nothing is mounted, no root is needed. Servers are kept in
 * network.conf (never the password); a login is asked for when the guest is refused. Delete in the list of servers forgets one.
 * Ctrl+H hidden files, Ctrl+R reread, Ctrl+T terminal here, Ctrl+L type a path, Shift+Up / Down scroll the preview, a letter jumps
 * to the next name starting with it, Esc clears the message. Mouse: click selects (and activates the pane), double click opens, wheel
 * scrolls the pane under the pointer, the round buttons at the top and bottom do what their names say.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
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

#include "img.h"
#include "sftp.h"
#include "smb.h"
#include "metrics.h"
#include "pathcomp.h"
#include "theme.h"
#include "font.h"
#include "keymap.h"
#include "protocol.h"
#include "uikit.h"
#include "wlc.h"
#include "util.h"
#include "wlclip.h"
#include "wltext.h"
#include "wlwin.h"

#define MAX_ENTRIES 100000
#define ROW_H 18
#define TOOL_H 30
#define HEAD_H 22
#define NET_PREFIX "smb://"                /* the list of servers; a Windows share is smb://SERVER/SHARE/..., an SSH server sftp://SERVER/abs/path */
#define SFTP_PREFIX "sftp://"
#define SFTP_TAG " (sftp)"                 /* how the list of servers marks an SSH server (a name may be both) */
#define STATUS_H 20
#define FKEY_H 30
#define COL_BG (lm_theme.background)
#define COL_FG (lm_theme.text_soft)
#define COL_DIM (lm_theme.text_dim)
#define COL_DIR (lm_theme.accent)
#define COL_SEL (lm_theme.button_hover)
#define COL_SEL_IDLE (lm_theme.bar)
#define COL_BAR (lm_theme.bar)
#define COL_BTN (lm_theme.control)
#define COL_BTN_ON (lm_theme.button_hover)
#define COL_WARN (lm_theme.caution)
#define COL_MARK (lm_theme.warn)
#define COL_EXEC (lm_theme.ok)
#define COL_PIC (lm_theme.picture)
#define COL_ERR (lm_theme.error)

#define PREVIEW_MAX_BYTES (128 * 1024)
#define PREVIEW_MAX_LINES 3000
#define PREVIEW_MAX_IMAGE (40u << 20)
#define HEX_BYTES 1024

struct entry {
    char *name;
    bool dir;
    bool broken;                        /* a link to nothing */
    bool hidden;
    bool exec;                          /* a file somebody may run */
    bool marked;                        /* Insert / Space: the file takes part in copy, move and delete */
    off_t size;
    time_t mtime;
};

/* One folder listing with its cursor. */
struct pane {
    char cwd[PATH_MAX];
    struct entry *e;
    int n, cap;
    int sel, top;
    bool sb_drag;                       /* the scroll bar's thumb is being dragged ... */
    int sb_grab;                        /* ... held this many pixels below its top */
    uint32_t last_click_ms;
    int last_click_row;
};

enum pv_kind { PV_NONE, PV_TEXT, PV_HEX, PV_IMAGE, PV_DIR, PV_ERROR, PV_VIDEO_WAIT, PV_VIDEO };     /* PV_VIDEO: a picture out of a video (img), PV_VIDEO_WAIT: it is on its way */

/* What the preview pane shows: the file under the cursor of the active pane. */
struct preview {
    char want[PATH_MAX];                /* the path asked for ("" = nothing) */
    bool loaded;                        /* `kind` and the rest belong to `want` */
    int64_t due;                        /* a picture is decoded only when the cursor has rested for a moment */
    enum pv_kind kind;
    char title[NAME_MAX + 1];
    char info[160];
    char **lines;                       /* PV_TEXT / PV_HEX / PV_DIR / PV_ERROR */
    int nlines;
    int top;                            /* first visible line */
    struct lm_image img;                /* PV_IMAGE: the decoded picture ... */
    uint32_t *scaled;                   /* ... and a copy that fits the pane (rebuilt when the pane size changes) */
    int sw, sh;
};

enum prompt { P_NONE, P_GOTO, P_COPY, P_MOVE, P_MKDIR, P_RENAME, P_DELETE, P_DELETE_HARD, P_EMPTY, P_CONNECT, P_NETUSER, P_NETPASS };

enum job_kind { J_COPY, J_MOVE, J_DELETE, J_TRASH, J_RESTORE };

struct job_item {
    char src[PATH_MAX];
    char dst[PATH_MAX];                 /* copy / move: where it goes */
};

/* A copy, move or delete that runs in its own thread while the window keeps going. The thread works through `items`; the window
 * reads the counters to draw the progress and sets `cancel` when Esc is pressed. `units` are bytes for copy, move and trash, and files
 * and folders for a real delete. */
struct job {
    enum job_kind kind;
    struct job_item *items;
    int n;
    char trash[PATH_MAX];               /* J_TRASH: the Trash folder */
    _Atomic long long total, done;      /* units */
    _Atomic int items_done;
    _Atomic bool cancel, finished;
    pthread_mutex_t mu;                 /* guards current and first_err */
    char current[NAME_MAX + 1];
    char first_err[PATH_MAX + 200];
    int ok, failed;                     /* set by the thread before `finished` */
    int pane;                           /* the pane the names came from (its marks are cleared when the job is over) */
    bool emptying;                      /* "Empty": a J_DELETE of everything in the trash (its info files included) */
    int nfiles;                         /* ... and how many of the items are trashed files (what the message counts) */
    int requested;                      /* how many things were asked for */
    int planned_skips;                  /* items left out while planning (they exist, are the same, ...) */
    char first_skip[PATH_MAX + 200];
    char target[PATH_MAX];              /* for the message */
    pthread_t thread;
    bool threaded;
};

struct app {
    struct wlwin win;
    const char *choose_key;             /* --choose: the setting a picked picture is written to (NULL = the two-pane manager) */
    uint32_t settings;                  /* mellagui_settings_v1 (only bound with --choose) */
    struct pane pane[2];
    int act;                            /* the active pane */
    bool dual;                          /* two panes (always, except for --choose) */
    bool preview;                       /* the pane that is not active shows a preview */
    bool show_hidden;
    bool help;
    enum prompt prompt;
    int menu_kind;                      /* the right-click menu that is showing: 0 none / the prompt's field, 1 a file's, 2 an empty place's */
    char pbuf[PATH_MAX];
    char net_pending[PATH_MAX];         /* the network place that wants a login (P_NETUSER / P_NETPASS) ... */
    char net_user[64];                  /* ... the user name typed so far ... */
    int net_pane;                       /* ... and the pane that asked */
    struct wlclip clip;                 /* the clipboard of the text prompts (Ctrl+V / C / X, the right-click menu) */
    char msg[300];
    int64_t msg_until;
    int rows_visible;
    struct preview pv;
    struct job *job;                    /* the running copy / move / delete, or NULL */
    bool sync_jobs;                     /* the selftest: do the work in the calling thread */
    double px, py;
    int hover;                          /* round button under the pointer: 0..N_TOOL-1 toolbar, 100 + n function key bar */
    uint32_t rep_code;
    int64_t rep_next;
    int rep_rate, rep_delay;
};

static bool is_net(const char *path);
static int net_list(struct pane *p, const char *dir, bool hidden, char *err, size_t errsz);
static void net_join(const char *dir, const char *name, char *out, size_t n);

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct pane *cur(struct app *a)
{
    return &a->pane[a->act];
}

static struct pane *other(struct app *a)
{
    return &a->pane[a->dual ? 1 - a->act : 0];
}

/* ---- listing ------------------------------------------------------------------------------------------------------------ */

static void entries_free(struct pane *p)
{
    for (int i = 0; i < p->n; i++)
        free(p->e[i].name);
    p->n = 0;
}

static int entry_cmp(const void *pa, const void *pb)
{
    const struct entry *x = pa, *y = pb;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

static bool add_entry(struct pane *p, const char *name, bool dir, bool broken, bool exec, off_t size, time_t mtime)
{
    if (p->n >= MAX_ENTRIES)
        return false;
    if (p->n == p->cap) {
        int nc = p->cap ? p->cap * 2 : 64;
        struct entry *t = realloc(p->e, (size_t)nc * sizeof(*t));
        if (!t)
            return false;
        p->e = t;
        p->cap = nc;
    }
    char *d = strdup(name);
    if (!d)
        return false;
    p->e[p->n++] = (struct entry){ .name = d, .dir = dir, .broken = broken, .hidden = name[0] == '.', .exec = exec, .size = size, .mtime = mtime };
    return true;
}

/* Reads `dir` into the list (hidden files only when `hidden`). 0, or -1 with the reason in err; the old list stays then. */
static int list_dir(struct pane *p, const char *dir, bool hidden, char *err, size_t errsz)
{
    if (is_net(dir))
        return net_list(p, dir, hidden, err, errsz);
    DIR *d = opendir(dir);
    if (!d) {
        snprintf(err, errsz, "cannot open %s: %s", dir, strerror(errno));
        return -1;
    }
    struct pane tmp = { 0 };
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (de->d_name[0] == '.' && !hidden)
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
        bool isdir = !broken && S_ISDIR(st.st_mode);
        add_entry(&tmp, de->d_name, isdir, broken, !isdir && !broken && (st.st_mode & 0111), st.st_size, st.st_mtime);
    }
    closedir(d);
    qsort(tmp.e, (size_t)tmp.n, sizeof(tmp.e[0]), entry_cmp);
    entries_free(p);
    free(p->e);
    p->e = tmp.e;
    p->n = tmp.n;
    p->cap = tmp.cap;
    return 0;
}

/* Row 0 is ".." (except in "/"), the rest are the entries. */
static bool has_up(const struct pane *p)
{
    return strcmp(p->cwd, "/") != 0 && strcmp(p->cwd, NET_PREFIX) != 0;
}

static int rows(const struct pane *p)
{
    return p->n + (has_up(p) ? 1 : 0);
}

static const struct entry *row_entry(const struct pane *p, int row)
{
    int i = row - (has_up(p) ? 1 : 0);
    return i >= 0 && i < p->n ? &p->e[i] : NULL;
}

static struct entry *row_entry_rw(struct pane *p, int row)
{
    return (struct entry *)row_entry(p, row);
}

static const char *row_name(const struct pane *p, int row)
{
    const struct entry *e = row_entry(p, row);
    return e ? e->name : "..";
}

static int count_marked(const struct pane *p)
{
    int k = 0;
    for (int i = 0; i < p->n; i++)
        k += p->e[i].marked;
    return k;
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
    if (is_net(name) || (name[0] != '/' && is_net(dir))) {              /* a network place: no normalizing of "smb://" */
        net_join(dir, name, out, outsz);
        return;
    }
    if (name[0] == '/')
        snprintf(raw, sizeof(raw), "%s", name);
    else
        snprintf(raw, sizeof(raw), "%s/%s", dir, name);
    normalize(raw, out, outsz);
}

/* ---- what a file is ------------------------------------------------------------------------------------------------------- */

enum kind { K_IMAGE, K_VIDEO, K_PROGRAM, K_TEXT, K_BINARY, K_UNREADABLE };

static bool ext_is(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    return dot && dot != name && !strcasecmp(dot + 1, ext);
}

/* Images and videos by extension; everything else by looking at the first 4 KB: an ELF file that may be executed is a program, a NUL
 * byte means binary (an executable script stays text: it opens in the editor). */
static enum kind classify(const char *path)
{
    if (ext_is(path, "png") || ext_is(path, "ppm") || ext_is(path, "jpg") || ext_is(path, "jpeg"))
        return K_IMAGE;
    if (ext_is(path, "mp4") || ext_is(path, "m4v"))
        return K_VIDEO;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return K_UNREADABLE;
    unsigned char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n < 0)
        return K_UNREADABLE;
    if (n >= 4 && !memcmp(buf, "\x7f" "ELF", 4) && access(path, X_OK) == 0)
        return K_PROGRAM;
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

/* Starts `prog [arg]` in `cwd`: a name is looked for next to this binary, then in PATH; a path (starts with "/") is run as it is, with
 * its output thrown away (it is somebody's program, not ours). false when it could not be executed (reported through a close-on-exec pipe). */
static bool launch(const char *prog, const char *arg, const char *cwd, char *err, size_t errsz)
{
    char path[PATH_MAX];
    bool foreign = prog[0] == '/';
    if (foreign)
        snprintf(path, sizeof(path), "%s", prog);
    else
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
        if (foreign) {
            int nul = open("/dev/null", O_RDWR);
            if (nul >= 0) {
                dup2(nul, 0);
                dup2(nul, 1);
                dup2(nul, 2);
                if (nul > 2)
                    close(nul);
            }
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



/* ---- copy, move, delete ------------------------------------------------------------------------------------------------ */

#define MAX_DEPTH 64

static __thread struct job *tl_job;                                    /* the job the calling thread works for (NULL: none) */

static bool job_cancelled(void)
{
    return tl_job && atomic_load(&tl_job->cancel);
}

static void job_add(long long units)
{
    if (tl_job)
        atomic_fetch_add(&tl_job->done, units);
}

static bool path_inside(const char *path, const char *dir)            /* is `path` the folder `dir` or below it? */
{
    size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && (path[n] == 0 || path[n] == '/' || (n > 0 && dir[n - 1] == '/'));
}

static int copy_node(const char *src, const char *dst, int depth, char *err, size_t errsz);

static int copy_file(const char *src, const char *dst, const struct stat *st, char *err, size_t errsz)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        snprintf(err, errsz, "cannot read %s: %s", src, strerror(errno));
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st->st_mode & 0777);
    if (out < 0) {
        snprintf(err, errsz, "cannot create %s: %s", dst, strerror(errno));
        close(in);
        return -1;
    }
    char buf[65536];
    int rc = 0;
    for (;;) {
        if (job_cancelled()) {
            snprintf(err, errsz, "cancelled");
            rc = -1;
            break;
        }
        ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n < 0) {
                snprintf(err, errsz, "cannot read %s: %s", src, strerror(errno));
                rc = -1;
            }
            break;
        }
        for (ssize_t off = 0; off < n;) {
            ssize_t k = write(out, buf + off, (size_t)(n - off));
            if (k < 0 && errno == EINTR)
                continue;
            if (k < 0) {
                snprintf(err, errsz, "cannot write %s: %s", dst, strerror(errno));
                rc = -1;
                break;
            }
            off += k;
        }
        if (rc)
            break;
        job_add(n);
    }
    struct timespec ts[2] = { st->st_atim, st->st_mtim };
    if (rc == 0)
        futimens(out, ts);
    if (close(out) < 0 && rc == 0) {
        snprintf(err, errsz, "cannot write %s: %s", dst, strerror(errno));
        rc = -1;
    }
    close(in);
    if (rc)
        unlink(dst);
    return rc;
}

/* Copies a file, a link or a whole folder tree to `dst`, which must not exist. Links are copied as links. */
static int copy_node(const char *src, const char *dst, int depth, char *err, size_t errsz)
{
    struct stat st;
    if (lstat(src, &st) < 0) {
        snprintf(err, errsz, "%s: %s", src, strerror(errno));
        return -1;
    }
    if (depth > MAX_DEPTH) {
        snprintf(err, errsz, "%s is nested too deeply", src);
        return -1;
    }
    if (job_cancelled()) {
        snprintf(err, errsz, "cancelled");
        return -1;
    }
    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t n = readlink(src, target, sizeof(target) - 1);
        if (n < 0 || (target[n] = 0, symlink(target, dst) < 0)) {
            snprintf(err, errsz, "cannot copy the link %s: %s", src, strerror(errno));
            return -1;
        }
        return 0;
    }
    if (S_ISREG(st.st_mode))
        return copy_file(src, dst, &st, err, errsz);
    if (!S_ISDIR(st.st_mode)) {
        snprintf(err, errsz, "%s is not a plain file or folder: not copied", src);
        return -1;
    }
    if (mkdir(dst, (st.st_mode & 0777) | 0700) < 0) {
        snprintf(err, errsz, "cannot create %s: %s", dst, strerror(errno));
        return -1;
    }
    DIR *d = opendir(src);
    if (!d) {
        snprintf(err, errsz, "cannot open %s: %s", src, strerror(errno));
        return -1;
    }
    int rc = 0;
    struct dirent *de;
    while (rc == 0 && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char s2[PATH_MAX], d2[PATH_MAX];
        if (snprintf(s2, sizeof(s2), "%s/%s", src, de->d_name) >= (int)sizeof(s2) ||
            snprintf(d2, sizeof(d2), "%s/%s", dst, de->d_name) >= (int)sizeof(d2)) {
            snprintf(err, errsz, "a path in %s is too long", src);
            rc = -1;
            break;
        }
        rc = copy_node(s2, d2, depth + 1, err, errsz);
    }
    closedir(d);
    if (rc == 0) {
        chmod(dst, st.st_mode & 0777);
        struct timespec ts[2] = { st.st_atim, st.st_mtim };
        utimensat(AT_FDCWD, dst, ts, 0);
    }
    return rc;
}

/* Deletes a file, a link or a folder with everything in it (links are not followed). */
static int remove_tree(const char *path, int depth, char *err, size_t errsz)
{
    struct stat st;
    if (strcmp(path, "/") == 0 || !path[0]) {
        snprintf(err, errsz, "will not delete the root folder");
        return -1;
    }
    if (lstat(path, &st) < 0) {
        snprintf(err, errsz, "%s: %s", path, strerror(errno));
        return -1;
    }
    if (job_cancelled()) {
        snprintf(err, errsz, "cancelled");
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) < 0) {
            snprintf(err, errsz, "cannot delete %s: %s", path, strerror(errno));
            return -1;
        }
        if (tl_job && tl_job->kind == J_DELETE)
            job_add(1);
        return 0;
    }
    if (depth > MAX_DEPTH) {
        snprintf(err, errsz, "%s is nested too deeply", path);
        return -1;
    }
    DIR *d = opendir(path);
    if (!d) {
        snprintf(err, errsz, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    int rc = 0;
    struct dirent *de;
    while (rc == 0 && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char p2[PATH_MAX];
        if (snprintf(p2, sizeof(p2), "%s/%s", path, de->d_name) >= (int)sizeof(p2)) {
            snprintf(err, errsz, "a path in %s is too long", path);
            rc = -1;
            break;
        }
        rc = remove_tree(p2, depth + 1, err, errsz);
    }
    closedir(d);
    if (rc == 0 && rmdir(path) < 0) {
        snprintf(err, errsz, "cannot delete %s: %s", path, strerror(errno));
        rc = -1;
    } else if (rc == 0 && tl_job && tl_job->kind == J_DELETE) {
        job_add(1);
    }
    return rc;
}

/* Moves `src` to `dst` (which must not exist): a rename, or a copy and delete across file systems. */
static int move_node(const char *src, const char *dst, char *err, size_t errsz)
{
    if (rename(src, dst) == 0)
        return 0;
    if (errno != EXDEV) {
        snprintf(err, errsz, "cannot move %s: %s", src, strerror(errno));
        return -1;
    }
    if (copy_node(src, dst, 0, err, errsz) < 0) {
        char ignore[64];
        remove_tree(dst, 0, ignore, sizeof(ignore));                   /* no half a copy is left behind */
        return -1;
    }
    return remove_tree(src, 0, err, errsz);
}

static bool exists_any(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

/* ---- the trash --------------------------------------------------------------------------------------------------------- */

/* The Trash folder of the user ($XDG_DATA_HOME/Trash, else ~/.local/share/Trash, FreeDesktop layout: files/ and info/). */
static bool trash_dir(char *out, size_t n)
{
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    int w;
    if (xdg && *xdg)
        w = snprintf(out, n, "%s/Trash", xdg);
    else if (home && *home)
        w = snprintf(out, n, "%s/.local/share/Trash", home);
    else
        return false;
    return w > 0 && (size_t)w < n;
}

static bool in_trash(const char *path)
{
    char t[PATH_MAX];
    return trash_dir(t, sizeof(t)) && path_inside(path, t);
}

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(tmp, mode) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    return mkdir(tmp, mode) < 0 && errno != EEXIST ? -1 : 0;
}

static void url_encode(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (; *in && k + 4 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
            out[k++] = (char)c;
        else
            k += (size_t)snprintf(out + k, n - k, "%%%02X", c);
    }
    out[k] = 0;
}

/* Moves `src` (an absolute path) into the trash and writes its .trashinfo so that it can be put back. A name that is taken there
 * gets ".2", ".3" ... */
static int trash_item(const char *trash, const char *src, char *err, size_t errsz)
{
    char files[PATH_MAX + 16], info[PATH_MAX + 16];
    snprintf(files, sizeof(files), "%s/files", trash);
    snprintf(info, sizeof(info), "%s/info", trash);
    if (mkdir_p(files, 0700) < 0 || mkdir_p(info, 0700) < 0) {
        snprintf(err, errsz, "cannot make the trash folder %s: %s", trash, strerror(errno));
        return -1;
    }
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    char name[NAME_MAX + 16], dst[PATH_MAX * 2], ipath[PATH_MAX * 2];
    int fd = -1;
    for (int k = 1; k < 1000 && fd < 0; k++) {
        if (k == 1)
            snprintf(name, sizeof(name), "%.*s", NAME_MAX, base);
        else
            snprintf(name, sizeof(name), "%.*s.%d", NAME_MAX - 5, base, k);
        snprintf(dst, sizeof(dst), "%s/%s", files, name);
        snprintf(ipath, sizeof(ipath), "%s/%s.trashinfo", info, name);
        if (exists_any(dst))
            continue;
        fd = open(ipath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) {
            snprintf(err, errsz, "cannot write the trash info: %s", strerror(errno));
            return -1;
        }
    }
    if (fd < 0) {
        snprintf(err, errsz, "no free name in the trash for %s", base);
        return -1;
    }
    char enc[PATH_MAX * 3], date[32], body[PATH_MAX * 3 + 96];
    url_encode(src, enc, sizeof(enc));
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &lt);
    int bl = snprintf(body, sizeof(body), "[Trash Info]\nPath=%s\nDeletionDate=%s\n", enc, date);
    bool ok = write(fd, body, (size_t)bl) == bl;
    ok = close(fd) == 0 && ok;
    if (!ok) {
        snprintf(err, errsz, "cannot write the trash info");
        unlink(ipath);
        return -1;
    }
    if (move_node(src, dst, err, errsz) < 0) {
        unlink(ipath);
        return -1;
    }
    return 0;
}

static bool trash_files_dir(char *out, size_t n)
{
    char t[PATH_MAX];
    return trash_dir(t, sizeof(t)) && snprintf(out, n, "%s/files", t) < (int)n;
}

/* Is `path` the folder of the trashed files or inside it? (Only there a restore makes sense.) */
static bool in_trash_files(const char *path)
{
    char t[PATH_MAX];
    return trash_files_dir(t, sizeof(t)) && path_inside(path, t);
}

static void url_decode(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (; *in && k + 1 < n; in++) {
        if (*in == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) {
            char h[3] = { in[1], in[2], 0 };
            out[k++] = (char)strtol(h, NULL, 16);
            in += 2;
        } else {
            out[k++] = *in;
        }
    }
    out[k] = 0;
}

/* Where the trashed file `name` came from, read from its .trashinfo (and when it was deleted). false when there is no usable info. */
static bool trash_origin(const char *name, char *path, size_t pn, char *date, size_t dn)
{
    char t[PATH_MAX], ipath[PATH_MAX * 2 + 32], buf[8192];
    path[0] = date[0] = 0;
    if (!trash_dir(t, sizeof(t)))
        return false;
    snprintf(ipath, sizeof(ipath), "%.*s/info/%.*s.trashinfo", PATH_MAX - 16, t, NAME_MAX, name);
    FILE *f = fopen(ipath, "r");
    if (!f)
        return false;
    size_t k = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[k] = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "Path=", 5))
            url_decode(line + 5, path, pn);
        else if (!strncmp(line, "DeletionDate=", 13))
            snprintf(date, dn, "%s", line + 13);
    }
    return path[0] == '/';
}

/* Puts `src` (a file in the trash) back at `dst`, makes the folders on the way, never replaces, removes the .trashinfo. */
static int restore_item(const char *src, const char *dst, char *err, size_t errsz)
{
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", dst);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = 0;
        if (mkdir_p(parent, 0755) < 0) {
            snprintf(err, errsz, "cannot make the folder %s: %s", parent, strerror(errno));
            return -1;
        }
    }
    if (exists_any(dst)) {
        snprintf(err, errsz, "%s already exists: not replaced", dst);
        return -1;
    }
    if (move_node(src, dst, err, errsz) < 0)
        return -1;
    char t[PATH_MAX], ipath[PATH_MAX * 2 + 32];
    const char *base = strrchr(src, '/');
    if (trash_dir(t, sizeof(t))) {
        snprintf(ipath, sizeof(ipath), "%.*s/info/%.*s.trashinfo", PATH_MAX - 16, t, NAME_MAX, base ? base + 1 : src);
        unlink(ipath);
    }
    return 0;
}


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"            /* names are cut on purpose where a message does not fit */

/* ---- network drives (smb://) ---------------------------------------------------------------------------------------------- */

/* A network place is the path "smb://" (the servers), "smb://SERVER" (its shares), "smb://SERVER/SHARE/folder/...". Everything goes through
 * smbclient (smb.c): nothing is mounted. The servers are in network.conf (host, user, legacy; never a password). */
#define NET_MAX 16
#define NET_ASK "Connect to a server..."          /* the first row of "smb://": the name has blanks, so no host can have it */
#define NET_CACHE_MAX (64ll << 20)

struct netconn {
    struct smb_conn c;
    bool saved;                         /* in network.conf */
    bool keep;                          /* the user typed it or logged in: it is saved once it worked */
    char home[256];                     /* an SSH server: its home folder (asked once) */
};

static struct netconn g_net[NET_MAX];
static int g_nnet;
static bool g_net_loaded;
static char g_net_gateway[32];
static bool g_net_need_login;           /* set by net_list when the server wants a user and password */
static char g_net_login_dir[PATH_MAX];  /* ... and the place that was asked for */

static bool is_net(const char *path)
{
    return !strncmp(path, NET_PREFIX, strlen(NET_PREFIX)) || !strncmp(path, SFTP_PREFIX, strlen(SFTP_PREFIX));
}

static bool net_conf_path(char *out, size_t n)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x)
        snprintf(out, n, "%s/mellagui/network.conf", x);
    else if (h && *h)
        snprintf(out, n, "%s/.config/mellagui/network.conf", h);
    else
        return false;
    return true;
}

static struct netconn *net_find_kind(const char *name, bool sftp)
{
    for (int i = 0; i < g_nnet; i++)
        if (g_net[i].c.sftp == sftp && !strcasecmp(g_net[i].c.name, name))
            return &g_net[i];
    return NULL;
}

static struct netconn *net_find(const char *name)                       /* a Windows share's server */
{
    return net_find_kind(name, false);
}

/* An SSH server: `host` (the name in paths is host, or host:port for another port). */
static struct netconn *net_add_sftp(const char *host, const char *user, int port)
{
    char name[48];
    if (port > 0 && port != 22)
        snprintf(name, sizeof(name), "%.40s:%d", host, port);
    else
        snprintf(name, sizeof(name), "%.47s", host);
    struct netconn *n = net_find_kind(name, true);
    if (!n) {
        if (g_nnet >= NET_MAX)
            return NULL;
        n = &g_net[g_nnet++];
        memset(n, 0, sizeof(*n));
        snprintf(n->c.name, sizeof(n->c.name), "%s", name);
        snprintf(n->c.host, sizeof(n->c.host), "%s", host);
        n->c.sftp = true;
        n->c.port = port > 0 && port != 22 ? port : 0;
    }
    if (user)
        snprintf(n->c.user, sizeof(n->c.user), "%s", user);
    return n;
}

static struct netconn *net_add(const char *host, const char *user, bool legacy)
{
    struct netconn *n = net_find(host);
    if (!n) {
        if (g_nnet >= NET_MAX)
            return NULL;
        n = &g_net[g_nnet++];
        memset(n, 0, sizeof(*n));
        snprintf(n->c.name, sizeof(n->c.name), "%s", host);
        snprintf(n->c.host, sizeof(n->c.host), "%s", host);
    }
    if (user)
        snprintf(n->c.user, sizeof(n->c.user), "%s", user);
    n->c.legacy = legacy;
    return n;
}

static void net_load(void)
{
    if (g_net_loaded)
        return;
    g_net_loaded = true;
    char path[PATH_MAX];
    if (net_conf_path(path, sizeof(path))) {
        FILE *f = fopen(path, "r");
        char line[400];
        while (f && fgets(line, sizeof(line), f)) {
            char host[128], user[64] = "", flag[16] = "";
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == '#' || sscanf(line, "%127s %63s %15s", host, user, flag) < 1)
                continue;
            struct netconn *n;
            if (!strncmp(host, SFTP_PREFIX, strlen(SFTP_PREFIX))) {         /* "sftp://host[:port] user" */
                char *hp = host + strlen(SFTP_PREFIX), *colon = strrchr(hp, ':');
                int port = 0;
                if (colon) {
                    port = atoi(colon + 1);
                    *colon = 0;
                }
                n = net_add_sftp(hp, !strcmp(user, "-") ? "" : user, port);
            } else {
                n = net_add(host, !strcmp(user, "-") ? "" : user, !strcmp(flag, "legacy"));
            }
            if (n)
                n->saved = true;
        }
        if (f)
            fclose(f);
    }
    smb_default_gateway(g_net_gateway, sizeof(g_net_gateway));
}

static void net_save(void)
{
    char path[PATH_MAX], dir[PATH_MAX];
    if (!net_conf_path(path, sizeof(path)))
        return;
    snprintf(dir, sizeof(dir), "%s", path);
    *strrchr(dir, '/') = 0;
    mkdir_p(dir, 0700);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fchmod(fileno(f), 0600);
    fprintf(f, "# MellaGUI file manager: network servers (host user [legacy] for a Windows share, sftp://host[:port] user for an SSH server); no passwords are kept\n");
    for (int i = 0; i < g_nnet; i++) {
        if (!g_net[i].saved)
            continue;
        const struct smb_conn *c = &g_net[i].c;
        if (c->sftp)
            fprintf(f, SFTP_PREFIX "%s%s%.0d %s\n", c->host, c->port ? ":" : "", c->port, c->user[0] ? c->user : "-");
        else
            fprintf(f, "%s %s%s\n", c->host, c->user[0] ? c->user : "-", c->legacy ? " legacy" : "");
    }
    fclose(f);
}

static void net_forget(const char *shown)                               /* the name as the list of servers shows it ("nas" or "nas (sftp)") */
{
    char name[64];
    snprintf(name, sizeof(name), "%s", shown);
    size_t tl = strlen(SFTP_TAG), nl = strlen(name);
    bool sftp = nl > tl && !strcmp(name + nl - tl, SFTP_TAG);
    if (sftp)
        name[nl - tl] = 0;
    for (int i = 0; i < g_nnet; i++)
        if (g_net[i].c.sftp == sftp && !strcasecmp(g_net[i].c.name, name)) {
            memmove(&g_net[i], &g_net[i + 1], (size_t)(g_nnet - i - 1) * sizeof(g_net[0]));
            g_nnet--;
            break;
        }
    net_save();
}

/* "smb://SERVER/SHARE/a/b" -> conn "SERVER", share "SHARE", dir "a/b". `level`: 0 servers, 1 shares, 2 inside a share. */
struct netpath {
    int level;
    bool sftp;                          /* an SSH server: level 1 is its home folder, level 2 any folder (`dir` is the absolute path without the first slash, "" = /) */
    char conn[48], share[64], dir[PATH_MAX];
};

static bool net_parse(const char *path, struct netpath *np)
{
    memset(np, 0, sizeof(*np));
    if (!is_net(path))
        return false;
    if (!strncmp(path, SFTP_PREFIX, strlen(SFTP_PREFIX))) {
        const char *q = path + strlen(SFTP_PREFIX);
        size_t cl = strcspn(q, "/");
        if (!cl || cl >= sizeof(np->conn))
            return false;
        np->sftp = true;
        snprintf(np->conn, sizeof(np->conn), "%.*s", (int)cl, q);
        if (!q[cl]) {
            np->level = 1;
            return true;
        }
        np->level = 2;
        snprintf(np->dir, sizeof(np->dir), "%s", q + cl + 1);
        size_t dl = strlen(np->dir);
        while (dl && np->dir[dl - 1] == '/')
            np->dir[--dl] = 0;
        return true;
    }
    const char *s = path + strlen(NET_PREFIX);
    size_t l = strcspn(s, "/");
    if (!l) {
        np->level = 0;
        return true;
    }
    if (l >= sizeof(np->conn))
        return false;
    snprintf(np->conn, sizeof(np->conn), "%.*s", (int)l, s);
    np->level = 1;
    s += l + (s[l] ? 1 : 0);
    l = strcspn(s, "/");
    if (!l)
        return true;
    if (l >= sizeof(np->share))
        return false;
    snprintf(np->share, sizeof(np->share), "%.*s", (int)l, s);
    np->level = 2;
    s += l + (s[l] ? 1 : 0);
    snprintf(np->dir, sizeof(np->dir), "%s", s);
    size_t dl = strlen(np->dir);
    while (dl && np->dir[dl - 1] == '/')
        np->dir[--dl] = 0;
    return true;
}

static void net_make(const struct netpath *np, char *out, size_t n)
{
    if (np->sftp) {
        if (np->level == 1)
            snprintf(out, n, SFTP_PREFIX "%s", np->conn);
        else
            snprintf(out, n, SFTP_PREFIX "%s/%s", np->conn, np->dir);
        return;
    }
    if (np->level == 0)
        snprintf(out, n, NET_PREFIX);
    else if (np->level == 1)
        snprintf(out, n, NET_PREFIX "%s", np->conn);
    else if (np->dir[0])
        snprintf(out, n, NET_PREFIX "%s/%s/%s", np->conn, np->share, np->dir);
    else
        snprintf(out, n, NET_PREFIX "%s/%s", np->conn, np->share);
}

static void net_parent(const char *path, char *out, size_t n)
{
    struct netpath np;
    if (!net_parse(path, &np) || np.level == 0) {
        snprintf(out, n, NET_PREFIX);
        return;
    }
    if (np.sftp) {                                                      /* up from a folder; from "/" to the list of servers */
        char *slash = strrchr(np.dir, '/');
        if (np.level == 2 && np.dir[0]) {
            if (slash)
                *slash = 0;
            else
                np.dir[0] = 0;
            net_make(&np, out, n);
        } else {
            snprintf(out, n, NET_PREFIX);
        }
        return;
    }
    char *sl = strrchr(np.dir, '/');
    if (np.level == 2 && np.dir[0]) {
        if (sl)
            *sl = 0;
        else
            np.dir[0] = 0;
    } else {
        np.level--;
    }
    net_make(&np, out, n);
}

/* `join_path` for network places: a name goes after a slash, ".." goes up, "smb://..." and absolute local paths replace everything. */
static void net_join(const char *dir, const char *name, char *out, size_t n)
{
    if (!strcmp(name, ".."))
        return net_parent(dir, out, n);
    if (is_net(name)) {
        struct netpath np;
        if (net_parse(name, &np))
            return net_make(&np, out, n);
    }
    if (!strcmp(dir, NET_PREFIX)) {
        size_t tl = strlen(SFTP_TAG), nl = strlen(name);
        if (nl > tl && !strcmp(name + nl - tl, SFTP_TAG))               /* an SSH server of the list */
            snprintf(out, n, SFTP_PREFIX "%.*s", (int)(nl - tl), name);
        else
            snprintf(out, n, NET_PREFIX "%s", name);
    } else {
        size_t dl = strlen(dir);
        snprintf(out, n, "%s%s%s", dir, dl && dir[dl - 1] == '/' ? "" : "/", name);
    }
}

static bool net_poll_cancel(void *data)
{
    (void)data;
    return !job_cancelled();
}

/* Progress of a transfer: a download is the size of the file being written, an upload is how far smbclient has read the local file (its
 * position in /proc/PID/fdinfo). Called by smb.c about every 50 ms in the job's thread; the bytes seen so far are added to the job. */
struct net_prog {
    char path[PATH_MAX];                /* the file that grows (download) or is read (upload) */
    bool upload;
    long long seen;
};

static long long net_progress_now(const struct net_prog *np)
{
    if (!np->upload) {
        struct stat st;
        return stat(np->path, &st) == 0 ? (long long)st.st_size : 0;
    }
    pid_t pid = smb_child_pid();
    if (pid <= 0)
        return 0;
    char dir[64], link[96], target[PATH_MAX];
    snprintf(dir, sizeof(dir), "/proc/%d/fd", (int)pid);
    DIR *d = opendir(dir);
    long long pos = 0;
    struct dirent *de;
    while (d && (de = readdir(d))) {
        snprintf(link, sizeof(link), "%s/%s", dir, de->d_name);
        ssize_t l = readlink(link, target, sizeof(target) - 1);
        if (l <= 0)
            continue;
        target[l] = 0;
        if (strcmp(target, np->path) != 0)
            continue;
        snprintf(link, sizeof(link), "/proc/%d/fdinfo/%s", (int)pid, de->d_name);
        FILE *f = fopen(link, "r");
        char line[128];
        while (f && fgets(line, sizeof(line), f))
            if (!strncmp(line, "pos:", 4))
                pos = atoll(line + 4);
        if (f)
            fclose(f);
        break;
    }
    if (d)
        closedir(d);
    return pos;
}

static bool net_poll_progress(void *data)
{
    struct net_prog *np = data;
    if (job_cancelled())
        return false;
    long long now = net_progress_now(np);
    if (now > np->seen && tl_job) {
        atomic_fetch_add(&tl_job->done, now - np->seen);
        np->seen = now;
    }
    return true;
}

static const struct smb_conn *net_conn(const struct netpath *np, char *err, size_t errsz)
{
    net_load();
    struct netconn *c = np->conn[0] ? net_find_kind(np->conn, np->sftp) : NULL;
    if (!c)
        snprintf(err, errsz, "No server called %s", np->conn);
    return c ? &c->c : NULL;
}

/* The calls that differ between the two kinds of server (a Windows share / an SSH server); `rel` is a path inside the share, or the absolute path
 * below "/" of an SSH server. Same contracts as smb.h / sftp.h. */
static void rf_abs(const struct netpath *np, const char *rel, char *out, size_t n)
{
    (void)np;
    snprintf(out, n, "/%s", rel);
}

static int rf_ls(const struct smb_conn *c, const struct netpath *np, const char *rel, struct smb_item **it, int *n, char *err, size_t errsz, bool *need_login,
                 smb_poll_fn poll)
{
    if (need_login)
        *need_login = false;
    if (np->sftp) {
        char abs[PATH_MAX];
        rf_abs(np, rel, abs, sizeof(abs));
        return sftp_ls(c, abs, it, n, err, errsz, need_login, poll, NULL);
    }
    return smb_ls(c, np->share, rel, it, n, err, errsz, poll, NULL);
}

static int rf_get(const struct smb_conn *c, const struct netpath *np, const char *rel, const char *local, bool dir, char *err, size_t errsz, smb_poll_fn poll,
                  void *pd)
{
    if (np->sftp) {
        char abs[PATH_MAX];
        rf_abs(np, rel, abs, sizeof(abs));
        return sftp_get(c, abs, local, dir, err, errsz, poll, pd);
    }
    return smb_get(c, np->share, rel, local, err, errsz, poll, pd);
}

static int rf_put(const struct smb_conn *c, const struct netpath *np, const char *local, const char *rel, bool dir, char *err, size_t errsz,
                  smb_poll_fn poll, void *pd)
{
    if (np->sftp) {
        char abs[PATH_MAX];
        rf_abs(np, rel, abs, sizeof(abs));
        return sftp_put(c, local, abs, dir, err, errsz, poll, pd);
    }
    return smb_put(c, np->share, local, rel, err, errsz, poll, pd);
}

static int rf_del(const struct smb_conn *c, const struct netpath *np, const char *rel, bool dir, char *err, size_t errsz, smb_poll_fn poll)
{
    if (np->sftp) {
        char abs[PATH_MAX];
        rf_abs(np, rel, abs, sizeof(abs));
        return sftp_del(c, abs, dir, err, errsz, poll, NULL);
    }
    return smb_del(c, np->share, rel, dir, err, errsz, poll, NULL);
}

static int rf_mkdir(const struct smb_conn *c, const struct netpath *np, const char *rel, char *err, size_t errsz, smb_poll_fn poll)
{
    if (np->sftp) {
        char abs[PATH_MAX];
        rf_abs(np, rel, abs, sizeof(abs));
        return sftp_mkdir(c, abs, err, errsz, poll, NULL);
    }
    return smb_mkdir(c, np->share, rel, err, errsz, poll, NULL);
}

static int rf_rename(const struct smb_conn *c, const struct netpath *np, const char *from, const char *to, char *err, size_t errsz, smb_poll_fn poll)
{
    if (np->sftp) {
        char a[PATH_MAX], b[PATH_MAX];
        rf_abs(np, from, a, sizeof(a));
        rf_abs(np, to, b, sizeof(b));
        return sftp_rename(c, a, b, err, errsz, poll, NULL);
    }
    return smb_rename(c, np->share, from, to, err, errsz, poll, NULL);
}

/* A failed call that means "log in": the user is asked. */
static void net_note_error(const char *reason, const char *place)
{
    if (!strcmp(reason, "Wrong user name or password") || !strcmp(reason, "Access denied")) {
        g_net_need_login = true;
        snprintf(g_net_login_dir, sizeof(g_net_login_dir), "%s", place);
    }
}

static bool net_conn_used(const struct netpath *np)
{
    struct netconn *c = net_find_kind(np->conn, np->sftp);
    if (c && !c->saved && c->keep) {                                    /* a server the user typed or logged in to is kept */
        c->saved = true;
        net_save();
    }
    return c != NULL;
}

/* "sftp://SERVER" is the server's home folder: its real path comes from the server (asked once). Other paths stay as they are. */
static bool net_resolve(const char *path, char *out, size_t n, char *err, size_t errsz)
{
    net_load();
    struct netpath np;
    snprintf(out, n, "%s", path);
    if (!net_parse(path, &np) || !np.sftp || np.level != 1)
        return true;
    struct netconn *c = net_find_kind(np.conn, true);
    if (!c) {
        snprintf(err, errsz, "No server called %s", np.conn);
        return false;
    }
    if (!c->home[0]) {
        bool login = false;
        char why[SMB_ERR];
        if (sftp_pwd(&c->c, c->home, sizeof(c->home), why, sizeof(why), &login, NULL, NULL) < 0) {
            c->home[0] = 0;
            snprintf(err, errsz, "%s: %s", np.conn, why);
            if (login) {
                g_net_need_login = true;
                snprintf(g_net_login_dir, sizeof(g_net_login_dir), "%s", path);
            }
            return false;
        }
    }
    snprintf(out, n, SFTP_PREFIX "%s%s", np.conn, c->home);
    size_t l = strlen(out);
    while (l > strlen(SFTP_PREFIX) + strlen(np.conn) + 1 && out[l - 1] == '/')
        out[--l] = 0;
    return true;
}

/* Lists a network place into `p` (same contract as list_dir: 0, or -1 with the reason in err). */
static int net_list(struct pane *p, const char *dir, bool hidden, char *err, size_t errsz)
{
    net_load();
    struct netpath np;
    if (!net_parse(dir, &np)) {
        snprintf(err, errsz, "bad network address %s", dir);
        return -1;
    }
    struct pane tmp = { 0 };
    if (np.level == 0) {
        for (int i = 0; i < g_nnet; i++) {
            char shown[64];
            snprintf(shown, sizeof(shown), "%s%s", g_net[i].c.name, g_net[i].c.sftp ? SFTP_TAG : "");
            add_entry(&tmp, shown, true, false, false, 0, 0);
        }
        if (g_net_gateway[0] && !net_find(g_net_gateway))
            net_add(g_net_gateway, "", false), add_entry(&tmp, g_net_gateway, true, false, false, 0, 0);
        qsort(tmp.e, (size_t)tmp.n, sizeof(tmp.e[0]), entry_cmp);
        add_entry(&tmp, NET_ASK, false, false, false, 0, 0);
        if (tmp.n > 1) {                                                /* the question goes first */
            struct entry q = tmp.e[tmp.n - 1];
            memmove(&tmp.e[1], &tmp.e[0], (size_t)(tmp.n - 1) * sizeof(tmp.e[0]));
            tmp.e[0] = q;
        }
    } else {
        const struct smb_conn *c = net_conn(&np, err, errsz);
        if (!c)
            return -1;
        char why[SMB_ERR];
        if (np.level == 1 && !np.sftp) {
            char (*sh)[64] = NULL;
            int n = 0;
            if (smb_shares(c, &sh, &n, why, sizeof(why), NULL, NULL) < 0) {
                snprintf(err, errsz, "%s: %s", np.conn, why);
                net_note_error(why, dir);
                return -1;
            }
            for (int i = 0; i < n; i++)
                add_entry(&tmp, sh[i], true, false, false, 0, 0);
            free(sh);
        } else {
            struct smb_item *it = NULL;
            int n = 0;
            bool login = false;
            if (np.sftp && np.level == 1) {                             /* the home folder is listed as the place it is (go_to resolves it first) */
                snprintf(err, errsz, "%s: open its folders from the path", np.conn);
                return -1;
            }
            if (rf_ls(c, &np, np.dir, &it, &n, why, sizeof(why), &login, NULL) < 0) {
                snprintf(err, errsz, "%s: %s", dir, why);
                if (login) {
                    g_net_need_login = true;
                    snprintf(g_net_login_dir, sizeof(g_net_login_dir), "%s", dir);
                } else {
                    net_note_error(why, dir);
                }
                return -1;
            }
            for (int i = 0; i < n; i++)
                if (hidden || it[i].name[0] != '.')
                    add_entry(&tmp, it[i].name, it[i].dir, false, false, (off_t)it[i].size, it[i].mtime);
            free(it);
        }
        net_conn_used(&np);
        qsort(tmp.e, (size_t)tmp.n, sizeof(tmp.e[0]), entry_cmp);
    }
    entries_free(p);
    free(p->e);
    p->e = tmp.e;
    p->n = tmp.n;
    p->cap = tmp.cap;
    return 0;
}

/* Is there something called like this, and is it a folder? 0, or -1 with the reason. Servers, shares and the top of a share count as folders. */
static int net_stat(const char *path, bool *isdir, long long *size, char *err, size_t errsz)
{
    struct netpath np;
    if (!net_parse(path, &np)) {
        snprintf(err, errsz, "bad network address");
        return -1;
    }
    *size = 0;
    if (np.level < 2 || !np.dir[0]) {
        *isdir = true;
        return 0;
    }
    const struct smb_conn *c = net_conn(&np, err, errsz);
    if (!c)
        return -1;
    char parent[PATH_MAX], base[NAME_MAX + 1], why[SMB_ERR];
    snprintf(parent, sizeof(parent), "%s", np.dir);
    char *sl = strrchr(parent, '/');
    snprintf(base, sizeof(base), "%s", sl ? sl + 1 : parent);
    if (sl)
        *sl = 0;
    else
        parent[0] = 0;
    struct smb_item *it = NULL;
    int n = 0;
    if (rf_ls(c, &np, parent, &it, &n, why, sizeof(why), NULL, net_poll_cancel) < 0) {
        snprintf(err, errsz, "%s", why);
        return -1;
    }
    int rc = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(it[i].name, base)) {
            *isdir = it[i].dir;
            *size = it[i].size;
            rc = 0;
        }
    free(it);
    if (rc < 0)
        snprintf(err, errsz, "%s does not exist", base);
    return rc;
}

/* The same for any path: a local one (stat) or a network one. */
static int stat_any(const char *path, bool *isdir, long long *size, char *err, size_t errsz)
{
    if (is_net(path))
        return net_stat(path, isdir, size, err, errsz);
    struct stat st;
    if (stat(path, &st) < 0) {
        snprintf(err, errsz, "%s", strerror(errno));
        return -1;
    }
    *isdir = S_ISDIR(st.st_mode);
    *size = (long long)st.st_size;
    return 0;
}

static bool exists_net_or_local(const char *path)
{
    if (!is_net(path))
        return exists_any(path);
    bool d;
    long long sz;
    char e[200];
    return net_stat(path, &d, &sz, e, sizeof(e)) == 0;
}

/* ---- copying to and from a network place (in a job's thread) ----- */

static int net_get_node(const struct smb_conn *c, const struct netpath *np, const char *dst, bool isdir, int depth, char *err, size_t errsz)
{
    char why[SMB_ERR];
    if (depth > MAX_DEPTH || job_cancelled()) {
        snprintf(err, errsz, "cancelled");
        return -1;
    }
    if (!isdir) {
        char part[PATH_MAX + 8];
        snprintf(part, sizeof(part), "%s.part", dst);
        char rp[PATH_MAX];
        snprintf(rp, sizeof(rp), "%s", np->dir);
        struct net_prog pr = { .upload = false };
        snprintf(pr.path, sizeof(pr.path), "%s", part);
        if (rf_get(c, np, rp, part, false, why, sizeof(why), net_poll_progress, &pr) < 0) {
            unlink(part);
            snprintf(err, errsz, "%s: %s", np->dir, why);
            return -1;
        }
        struct stat st;
        if (stat(part, &st) == 0 && tl_job && (long long)st.st_size > pr.seen)
            atomic_fetch_add(&tl_job->done, (long long)st.st_size - pr.seen);
        if (rename(part, dst) < 0) {
            snprintf(err, errsz, "cannot write %s: %s", dst, strerror(errno));
            unlink(part);
            return -1;
        }
        return 0;
    }
    if (np->sftp) {                                                     /* sftp copies a folder with what is in it in one go */
        if (rf_get(c, np, np->dir, dst, true, why, sizeof(why), net_poll_cancel, NULL) < 0) {
            snprintf(err, errsz, "%s: %s", np->dir, why);
            return -1;
        }
        return 0;
    }
    if (mkdir(dst, 0755) < 0 && errno != EEXIST) {
        snprintf(err, errsz, "cannot create %s: %s", dst, strerror(errno));
        return -1;
    }
    struct smb_item *it = NULL;
    int n = 0;
    if (rf_ls(c, np, np->dir, &it, &n, why, sizeof(why), NULL, net_poll_cancel) < 0) {
        snprintf(err, errsz, "%s: %s", np->dir, why);
        return -1;
    }
    int rc = 0;
    for (int i = 0; i < n && rc == 0; i++) {
        struct netpath sub = *np;
        char d2[PATH_MAX];
        snprintf(sub.dir, sizeof(sub.dir), "%s%s%s", np->dir, np->dir[0] ? "/" : "", it[i].name);
        if (snprintf(d2, sizeof(d2), "%s/%s", dst, it[i].name) >= (int)sizeof(d2) || !strcmp(it[i].name, ".."))
            continue;
        rc = net_get_node(c, &sub, d2, it[i].dir, depth + 1, err, errsz);
    }
    free(it);
    return rc;
}

static int net_put_node(const struct smb_conn *c, const struct netpath *np, const char *src, int depth, char *err, size_t errsz)
{
    char why[SMB_ERR];
    struct stat st;
    if (depth > MAX_DEPTH || job_cancelled()) {
        snprintf(err, errsz, "cancelled");
        return -1;
    }
    if (lstat(src, &st) < 0) {
        snprintf(err, errsz, "%s: %s", src, strerror(errno));
        return -1;
    }
    if (S_ISREG(st.st_mode)) {
        struct net_prog pr = { .upload = true };
        if (!realpath(src, pr.path))
            snprintf(pr.path, sizeof(pr.path), "%s", src);
        if (rf_put(c, np, src, np->dir, false, why, sizeof(why), net_poll_progress, &pr) < 0) {
            snprintf(err, errsz, "%s: %s", np->dir, why);
            return -1;
        }
        if (tl_job && (long long)st.st_size > pr.seen)
            atomic_fetch_add(&tl_job->done, (long long)st.st_size - pr.seen);
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        snprintf(err, errsz, "%s is not a plain file or folder: left out", src);
        return -1;
    }
    if (np->sftp) {                                                     /* a folder goes up with what is in it in one go */
        if (rf_put(c, np, src, np->dir, true, why, sizeof(why), net_poll_cancel, NULL) < 0) {
            snprintf(err, errsz, "%s: %s", np->dir, why);
            return -1;
        }
        return 0;
    }
    if (rf_mkdir(c, np, np->dir, why, sizeof(why), net_poll_cancel) < 0 && strcmp(why, "It exists already") != 0) {
        snprintf(err, errsz, "%s: %s", np->dir, why);
        return -1;
    }
    DIR *d = opendir(src);
    if (!d) {
        snprintf(err, errsz, "cannot read %s: %s", src, strerror(errno));
        return -1;
    }
    int rc = 0;
    struct dirent *de;
    while (rc == 0 && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        struct netpath sub = *np;
        char s2[PATH_MAX];
        snprintf(sub.dir, sizeof(sub.dir), "%s/%s", np->dir, de->d_name);
        if (snprintf(s2, sizeof(s2), "%s/%s", src, de->d_name) >= (int)sizeof(s2))
            continue;
        rc = net_put_node(c, &sub, s2, depth + 1, err, errsz);
    }
    closedir(d);
    return rc;
}

static int net_remove_node(const char *path, char *err, size_t errsz)
{
    struct netpath np;
    char why[SMB_ERR];
    if (!net_parse(path, &np) || np.level < 2 || !np.dir[0]) {
        snprintf(err, errsz, "Servers and shares cannot be deleted here");
        return -1;
    }
    const struct smb_conn *c = net_conn(&np, err, errsz);
    if (!c)
        return -1;
    bool isdir;
    long long sz;
    if (net_stat(path, &isdir, &sz, err, errsz) < 0)
        return -1;
    if (rf_del(c, &np, np.dir, isdir, why, sizeof(why), net_poll_cancel) < 0) {
        snprintf(err, errsz, "%s: %s", path, why);
        return -1;
    }
    return 0;
}

/* Copies src to dst when at least one of them is a network place. A move is the copy and then removing the source. */
static int net_copy_node(const char *src, const char *dst, bool move, char *err, size_t errsz)
{
    struct netpath s, d;
    bool sn = is_net(src), dn = is_net(dst);
    if (sn && dn) {
        if (net_parse(src, &s) && net_parse(dst, &d) && move && s.level == 2 && d.level == 2 && s.sftp == d.sftp && !strcmp(s.conn, d.conn) && !strcmp(s.share, d.share)) {
            const struct smb_conn *c = net_conn(&s, err, errsz);          /* inside one share a move is a rename */
            char why[SMB_ERR];
            if (!c)
                return -1;
            if (rf_rename(c, &s, s.dir, d.dir, why, sizeof(why), net_poll_cancel) < 0) {
                snprintf(err, errsz, "%s: %s", src, why);
                return -1;
            }
            return 0;
        }
        snprintf(err, errsz, "Copying from one network place to another is not possible yet: copy to this computer first");
        return -1;
    }
    int rc;
    if (sn) {
        bool isdir;
        long long sz;
        const struct smb_conn *c;
        if (!net_parse(src, &s) || s.level < 2 || !s.dir[0]) {
            snprintf(err, errsz, "Only files and folders inside a share can be copied");
            return -1;
        }
        if (!(c = net_conn(&s, err, errsz)) || net_stat(src, &isdir, &sz, err, errsz) < 0)
            return -1;
        rc = net_get_node(c, &s, dst, isdir, 0, err, errsz);
    } else {
        const struct smb_conn *c;
        if (!net_parse(dst, &d) || d.level < 2 || !d.dir[0]) {
            snprintf(err, errsz, "Files go into a folder of a share, not into a server");
            return -1;
        }
        if (!(c = net_conn(&d, err, errsz)))
            return -1;
        rc = net_put_node(c, &d, src, 0, err, errsz);
    }
    if (rc == 0 && move) {
        if (sn)
            rc = net_remove_node(src, err, errsz);
        else
            rc = remove_tree(src, 0, err, errsz);
    }
    return rc;
}

/* Opening a file: a copy goes to the cache folder and is opened from there (edits are not sent back). */
static bool net_poll_slow(void *data)
{
    return now_ms() - *(int64_t *)data < 60000;
}

static bool net_fetch(const char *path, char *local, size_t lsz, char *err, size_t errsz)
{
    struct netpath np;
    bool isdir;
    long long size;
    if (net_stat(path, &isdir, &size, err, errsz) < 0)
        return false;
    if (isdir || !net_parse(path, &np))
        return false;
    if (size > NET_CACHE_MAX) {
        snprintf(err, errsz, "%s is too big to open from the network (%lld MB): copy it with F5", strrchr(path, '/') + 1, size >> 20);
        return false;
    }
    const struct smb_conn *c = net_conn(&np, err, errsz);
    if (!c)
        return false;
    const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
    char dir[PATH_MAX];
    if (x && *x)
        snprintf(dir, sizeof(dir), "%s/mellagui/network/%s/%s", x, np.conn, np.share);
    else if (h && *h)
        snprintf(dir, sizeof(dir), "%s/.cache/mellagui/network/%s/%s", h, np.conn, np.share);
    else {
        snprintf(err, errsz, "There is no place for a copy (HOME is not set)");
        return false;
    }
    char sub[PATH_MAX];
    snprintf(sub, sizeof(sub), "%s", np.dir);
    char *sl = strrchr(sub, '/');
    if (sl) {
        *sl = 0;
        size_t dl = strlen(dir);
        snprintf(dir + dl, sizeof(dir) - dl, "/%s", sub);
    }
    mkdir_p(dir, 0700);
    snprintf(local, lsz, "%s/%s", dir, strrchr(np.dir, '/') ? strrchr(np.dir, '/') + 1 : np.dir);
    unlink(local);
    char why[SMB_ERR];
    int64_t t0 = now_ms();
    if (rf_get(c, &np, np.dir, local, false, why, sizeof(why), net_poll_slow, &t0) < 0) {
        snprintf(err, errsz, "%s", why);
        return false;
    }
    chmod(local, 0600);
    return true;
}

#pragma GCC diagnostic pop

/* ---- running a job -------------------------------------------------------------------------------------------------------- */

/* The number of units (bytes, or files and folders when `nodes`) below `path`. */
static long long scan_units(const char *path, bool nodes, int depth)
{
    struct stat st;
    if (depth > MAX_DEPTH || job_cancelled() || lstat(path, &st) < 0)
        return 0;
    long long total = nodes ? 1 : (S_ISREG(st.st_mode) ? (long long)st.st_size : 0);
    if (!S_ISDIR(st.st_mode))
        return total;
    DIR *d = opendir(path);
    if (!d)
        return total;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char p2[PATH_MAX];
        if (snprintf(p2, sizeof(p2), "%s/%s", path, de->d_name) < (int)sizeof(p2))
            total += scan_units(p2, nodes, depth + 1);
    }
    closedir(d);
    return total;
}

static void job_note_error(struct job *j, const char *err)
{
    pthread_mutex_lock(&j->mu);
    if (!j->first_err[0])
        snprintf(j->first_err, sizeof(j->first_err), "%s", err);
    pthread_mutex_unlock(&j->mu);
}

/* The work itself (in the job's thread, or in the caller's with sync_jobs). */
static void job_execute(struct job *j)
{
    tl_job = j;
    bool nodes = j->kind == J_DELETE;
    long long total = 0;
    long long *sizes = calloc((size_t)LM_MAX(j->n, 1), sizeof(long long));
    for (int i = 0; i < j->n && !job_cancelled(); i++) {
        if (is_net(j->items[i].src)) {                                   /* a network file: its size is one question to the server */
            bool d;
            long long sz;
            char e[200];
            sizes[i] = nodes ? 1 : (net_stat(j->items[i].src, &d, &sz, e, sizeof(e)) == 0 && !d ? sz : 0);
        } else {
            sizes[i] = scan_units(j->items[i].src, nodes, 0);
        }
        total += sizes[i];
    }
    atomic_store(&j->total, total);
    for (int i = 0; i < j->n && !job_cancelled(); i++) {
        const char *src = j->items[i].src, *dst = j->items[i].dst;
        const char *base = strrchr(src, '/');
        pthread_mutex_lock(&j->mu);
        snprintf(j->current, sizeof(j->current), "%.*s", NAME_MAX, base ? base + 1 : src);
        pthread_mutex_unlock(&j->mu);
        char err[PATH_MAX + 200] = "";
        long long before = atomic_load(&j->done);
        int rc;
        switch (j->kind) {
        case J_COPY: rc = is_net(src) || is_net(dst) ? net_copy_node(src, dst, false, err, sizeof(err)) : copy_node(src, dst, 0, err, sizeof(err)); break;
        case J_MOVE: rc = is_net(src) || is_net(dst) ? net_copy_node(src, dst, true, err, sizeof(err)) : move_node(src, dst, err, sizeof(err)); break;
        case J_TRASH: rc = trash_item(j->trash, src, err, sizeof(err)); break;
        case J_RESTORE: rc = restore_item(src, dst, err, sizeof(err)); break;
        default: rc = is_net(src) ? net_remove_node(src, err, sizeof(err)) : remove_tree(src, 0, err, sizeof(err)); break;
        }
        if (rc < 0) {
            j->failed++;
            job_note_error(j, err);
            if (job_cancelled())
                break;
            continue;
        }
        j->ok++;
        atomic_fetch_add(&j->items_done, 1);
        if (atomic_load(&j->done) == before)                              /* a rename or a rename into the trash: no bytes were copied */
            atomic_fetch_add(&j->done, sizes[i]);
    }
    free(sizes);
    tl_job = NULL;
}

static void *job_thread(void *arg)
{
    struct job *j = arg;
    job_execute(j);
    atomic_store(&j->finished, true);
    return NULL;
}

static void job_free(struct job *j)
{
    if (!j)
        return;
    pthread_mutex_destroy(&j->mu);
    free(j->items);
    free(j);
}

static struct job *job_new(enum job_kind kind, int max_items)
{
    struct job *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->items = calloc((size_t)LM_MAX(max_items, 1), sizeof(struct job_item));
    if (!j->items) {
        free(j);
        return NULL;
    }
    j->kind = kind;
    pthread_mutex_init(&j->mu, NULL);
    return j;
}

/* ---- the preview -------------------------------------------------------------------------------------------------------- */

static void preview_clear(struct preview *pv)
{
    for (int i = 0; i < pv->nlines; i++)
        free(pv->lines[i]);
    free(pv->lines);
    pv->lines = NULL;
    pv->nlines = 0;
    lm_img_free(&pv->img);
    free(pv->scaled);
    pv->scaled = NULL;
    pv->sw = pv->sh = 0;
    pv->kind = PV_NONE;
    pv->top = 0;
    pv->title[0] = pv->info[0] = 0;
}

static void pv_line(struct preview *pv, int *cap, const char *s, size_t n)
{
    if (pv->nlines >= PREVIEW_MAX_LINES)
        return;
    if (pv->nlines == *cap) {
        int nc = *cap ? *cap * 2 : 64;
        char **t = realloc(pv->lines, (size_t)nc * sizeof(char *));
        if (!t)
            return;
        pv->lines = t;
        *cap = nc;
    }
    char *d = malloc(n + 1);
    if (!d)
        return;
    memcpy(d, s, n);
    d[n] = 0;
    pv->lines[pv->nlines++] = d;
}

static void pv_linef(struct preview *pv, int *cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void pv_linef(struct preview *pv, int *cap, const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    pv_line(pv, cap, b, strlen(b));
}

/* Text: tabs become spaces, CR is dropped, a line is cut at 500 bytes (or at the end of the 128 KB that are read). */
static void pv_text(struct preview *pv, const unsigned char *buf, size_t n)
{
    int cap = 0;
    char line[600];
    size_t len = 0;
    for (size_t i = 0; i <= n; i++) {
        unsigned char c = i < n ? buf[i] : '\n';
        if (c == '\n') {
            if (i < n || len > 0)
                pv_line(pv, &cap, line, len);
            len = 0;
            continue;
        }
        if (c == '\r')
            continue;
        if (c == '\t') {
            do {
                if (len < 500)
                    line[len++] = ' ';
            } while (len % 4);
            continue;
        }
        if (c < 0x20 && c != 0x1b)
            c = '?';
        if (len < 500)
            line[len++] = (char)c;
    }
}

static void pv_hex(struct preview *pv, const unsigned char *buf, size_t n)
{
    int cap = 0;
    for (size_t off = 0; off < n; off += 8) {                            /* 8 bytes a line: it fits half a window */
        char b[96], asc[12];
        int k = snprintf(b, sizeof(b), "%08zx ", off);
        for (size_t j = 0; j < 8; j++) {
            if (off + j < n) {
                k += snprintf(b + k, sizeof(b) - (size_t)k, " %02x", buf[off + j]);
                asc[j] = buf[off + j] >= 0x20 && buf[off + j] < 0x7f ? (char)buf[off + j] : '.';
            } else {
                k += snprintf(b + k, sizeof(b) - (size_t)k, "   ");
                asc[j] = ' ';
            }
        }
        asc[8] = 0;
        snprintf(b + k, sizeof(b) - (size_t)k, "  |%s|", asc);
        pv_line(pv, &cap, b, strlen(b));
    }
}

/* Loads what the preview pane will show for `path` (a file, a folder, or an error text). Always sets some kind. */
static void preview_load(struct preview *pv, const char *path, bool hidden)
{
    preview_clear(pv);
    const char *base = strrchr(path, '/');
    snprintf(pv->title, sizeof(pv->title), "%s", base ? base + 1 : path);
    int cap = 0;
    struct stat st;
    if (is_net(path)) {                                                  /* nothing is read from the network just for a look */
        pv->kind = PV_ERROR;
        pv_linef(pv, &cap, "No preview of a network place: copy the file first (F5)");
        return;
    }
    if (stat(path, &st) < 0) {
        pv->kind = PV_ERROR;
        pv_linef(pv, &cap, "%s", strerror(errno));
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        struct pane tmp = { 0 };
        char err[200];
        pv->kind = PV_DIR;
        if (list_dir(&tmp, path, hidden, err, sizeof(err)) < 0) {
            pv->kind = PV_ERROR;
            pv_linef(pv, &cap, "%s", err);
            return;
        }
        snprintf(pv->info, sizeof(pv->info), "folder, %d item%s", tmp.n, tmp.n == 1 ? "" : "s");
        for (int i = 0; i < tmp.n; i++)
            pv_linef(pv, &cap, "%s%s", tmp.e[i].name, tmp.e[i].dir ? "/" : "");
        entries_free(&tmp);
        free(tmp.e);
        return;
    }
    char sz[32];
    human_size(st.st_size, sz, sizeof(sz));
    snprintf(pv->info, sizeof(pv->info), "%s", sz);
    if (!S_ISREG(st.st_mode)) {
        pv->kind = PV_ERROR;
        pv_linef(pv, &cap, "not a plain file");
        return;
    }
    if (classify(path) == K_VIDEO) {                                     /* a picture of the video comes from mellagui-view (video_poll below) */
        pv->kind = PV_VIDEO_WAIT;
        return;
    }
    if (classify(path) == K_IMAGE) {
        if ((unsigned long long)st.st_size > PREVIEW_MAX_IMAGE) {
            pv->kind = PV_ERROR;
            pv_linef(pv, &cap, "the picture is too big to preview (%s)", sz);
            return;
        }
        char err[200];
        if (lm_img_load(path, &pv->img, err, sizeof(err)) < 0) {
            pv->kind = PV_ERROR;
            pv_linef(pv, &cap, "%s", err);
            return;
        }
        pv->kind = PV_IMAGE;
        snprintf(pv->info, sizeof(pv->info), "%s, %dx%d", sz, pv->img.w, pv->img.h);
        return;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        pv->kind = PV_ERROR;
        pv_linef(pv, &cap, "cannot read: %s", strerror(errno));
        return;
    }
    unsigned char *buf = malloc(PREVIEW_MAX_BYTES + 1);
    ssize_t n = buf ? read(fd, buf, PREVIEW_MAX_BYTES) : -1;
    close(fd);
    if (n < 0) {
        free(buf);
        pv->kind = PV_ERROR;
        pv_linef(pv, &cap, "cannot read: %s", strerror(errno));
        return;
    }
    if (memchr(buf, 0, (size_t)n < 4096 ? (size_t)n : 4096)) {
        pv->kind = PV_HEX;
        pv_hex(pv, buf, (size_t)n < HEX_BYTES ? (size_t)n : HEX_BYTES);
    } else {
        pv->kind = PV_TEXT;
        pv_text(pv, buf, (size_t)n);
        if ((off_t)n < st.st_size)
            pv_linef(pv, &cap, "...");
    }
    free(buf);
}

/* The picture scaled down to fit w x h (never up beyond 8 times), by averaging the source pixels under each target pixel. */
static void preview_scale(struct preview *pv, int w, int h)
{
    free(pv->scaled);
    pv->scaled = NULL;
    pv->sw = pv->sh = 0;
    if ((pv->kind != PV_IMAGE && pv->kind != PV_VIDEO) || w < 4 || h < 4 || pv->img.w < 1 || pv->img.h < 1)
        return;
    double s = LM_MIN((double)w / pv->img.w, (double)h / pv->img.h);
    if (s > 8.0)
        s = 8.0;
    int tw = LM_MAX((int)(pv->img.w * s), 1), th = LM_MAX((int)(pv->img.h * s), 1);
    uint32_t *out = malloc((size_t)tw * (size_t)th * 4);
    if (!out)
        return;
    for (int y = 0; y < th; y++) {
        int y0 = (int)((int64_t)y * pv->img.h / th), y1 = LM_MAX((int)((int64_t)(y + 1) * pv->img.h / th), y0 + 1);
        for (int x = 0; x < tw; x++) {
            int x0 = (int)((int64_t)x * pv->img.w / tw), x1 = LM_MAX((int)((int64_t)(x + 1) * pv->img.w / tw), x0 + 1);
            uint64_t r = 0, g = 0, b = 0, cnt = 0;
            for (int yy = y0; yy < y1 && yy < pv->img.h; yy++)
                for (int xx = x0; xx < x1 && xx < pv->img.w; xx++) {
                    uint32_t c = pv->img.px[(size_t)yy * (size_t)pv->img.w + (size_t)xx];
                    r += (c >> 16) & 255;
                    g += (c >> 8) & 255;
                    b += c & 255;
                    cnt++;
                }
            out[(size_t)y * (size_t)tw + (size_t)x] = cnt ? (uint32_t)((r / cnt) << 16 | (g / cnt) << 8 | (b / cnt)) : 0;
        }
    }
    pv->scaled = out;
    pv->sw = tw;
    pv->sh = th;
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
    const char *dir = cur(a)->cwd;
    const char *base = strrchr(dir, '/') ? strrchr(dir, '/') + 1 : dir;
    char t[PATH_MAX + 32];
    struct netpath tnp;
    if (!*base && net_parse(dir, &tnp) && tnp.sftp)                     /* "sftp://nas/": the server's name */
        base = tnp.conn;
    snprintf(t, sizeof(t), "%s - %s", !strcmp(dir, NET_PREFIX) ? "Network" : *base ? base : "/", a->choose_key ? "Choose a picture" : "Files");
    WLC_SEND(a->win.c, a->win.toplevel, LM_XDG_TOPLEVEL_REQ_SET_TITLE, { .s = t });
}

static void prompt_start(struct app *a, enum prompt kind, const char *initial);
static void go_network(struct app *a);

/* Shows `dir` (an absolute, normalized path) in pane p, selecting the row called `select` when there is one, else the first entry. */
static bool go_to(struct app *a, struct pane *p, const char *dir, const char *select)
{
    char err[PATH_MAX + 80], rdir[PATH_MAX];
    g_net_need_login = false;
    bool listed = false;
    if (is_net(dir)) {                                                  /* an SSH server's home folder has its real path first */
        if (net_resolve(dir, rdir, sizeof(rdir), err, sizeof(err))) {
            dir = rdir;
            listed = list_dir(p, dir, a->show_hidden, err, sizeof(err)) == 0;
        }
    } else {
        listed = list_dir(p, dir, a->show_hidden, err, sizeof(err)) == 0;
    }
    if (!listed) {
        msg(a, "%s", err);
        if (g_net_need_login) {                                         /* the server wants a user name and a password */
            struct netpath np;
            net_parse(g_net_login_dir, &np);
            struct netconn *c = net_find_kind(np.conn, np.sftp);
            g_net_need_login = false;
            snprintf(a->net_pending, sizeof(a->net_pending), "%s", g_net_login_dir);
            a->net_pane = (int)(p - a->pane);
            prompt_start(a, P_NETUSER, c ? c->c.user : "");
        }
        return false;
    }
    snprintf(p->cwd, sizeof(p->cwd), "%s", dir);
    p->sel = rows(p) > (has_up(p) ? 1 : 0) ? (has_up(p) ? 1 : 0) : 0;
    p->top = 0;
    if (select)
        for (int r = 0; r < rows(p); r++)
            if (!strcmp(row_name(p, r), select)) {
                p->sel = r;
                break;
            }
    a->msg[0] = 0;
    set_title(a);
    a->pv.want[0] = 0;
    a->pv.loaded = false;
    a->win.redraw = true;
    return true;
}

/* Reads the folder again, keeping the cursor, the scroll position and the marks. */
static void reread_pane(struct app *a, struct pane *p)
{
    char keep[NAME_MAX + 1];
    snprintf(keep, sizeof(keep), "%s", row_name(p, p->sel));
    int top = p->top, sel = p->sel;
    int nm = count_marked(p);
    char **marked = nm ? calloc((size_t)nm, sizeof(char *)) : NULL;
    for (int i = 0, k = 0; marked && i < p->n && k < nm; i++)
        if (p->e[i].marked)
            marked[k++] = strdup(p->e[i].name);
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", p->cwd);
    bool ok = go_to(a, p, dir, keep);
    if (ok) {
        p->top = top;
        if (strcmp(row_name(p, p->sel), keep) != 0)                     /* the file is gone: stay near where it was */
            p->sel = LM_CLAMP(sel, 0, LM_MAX(rows(p) - 1, 0));
        for (int k = 0; marked && k < nm; k++)
            for (int i = 0; i < p->n; i++)
                if (marked[k] && !strcmp(p->e[i].name, marked[k]))
                    p->e[i].marked = true;
    } else if (!go_to(a, p, "/", NULL)) {
        /* nothing to show */
    }
    for (int k = 0; marked && k < nm; k++)
        free(marked[k]);
    free(marked);
}

static void reread(struct app *a)
{
    reread_pane(a, cur(a));
    if (a->dual)
        reread_pane(a, other(a));
}

static void go_up(struct app *a)
{
    struct pane *p = cur(a);
    if (!has_up(p))
        return;
    char parent[PATH_MAX];
    join_path(p->cwd, "..", parent, sizeof(parent));
    const char *base = strrchr(p->cwd, '/');
    char from[NAME_MAX + 1];
    snprintf(from, sizeof(from), "%s", base ? base + 1 : "");
    go_to(a, p, parent, from);
}

static void settings_event(struct wlc *c, uint32_t id, uint32_t op, const union lm_arg *args, void *data)
{
    (void)c; (void)id; (void)op; (void)args; (void)data;            /* the value bursts are of no interest here */
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    struct app *a = w->data;
    if (a->choose_key && !strcmp(iface, "mellagui_settings_v1"))
        a->settings = wlc_bind(w->c, w->registry, name, &lm_mellagui_settings_v1_interface, 1, settings_event, a);
    else
        wlclip_global(&a->clip, w, name, iface, ver);
}

/* --choose: hand the picture to the compositor and end. */
static void choose_path(struct app *a, const char *path)
{
    if (!a->settings) {
        msg(a, "The compositor does not serve settings to this program");
        return;
    }
    WLC_SEND(a->win.c, a->settings, LM_MELLAGUI_SETTINGS_V1_REQ_SET, { .s = a->choose_key }, { .s = path });
    wlc_roundtrip(a->win.c);
    a->win.running = false;
}

/* Where a started program runs: the folder of the pane, or the home folder when that is a network place. */
static const char *workdir(const struct app *a)
{
    const char *c = a->pane[a->act].cwd;
    if (!is_net(c))
        return c;
    const char *h = getenv("HOME");
    return h && *h ? h : "/";
}

static void open_path(struct app *a, const char *path)
{
    struct stat st;
    if (is_net(path)) {
        bool isdir;
        long long size;
        char err[300], local[PATH_MAX];
        if (stat_any(path, &isdir, &size, err, sizeof(err)) < 0) {
            msg(a, "%s", err);
        } else if (isdir) {
            go_to(a, cur(a), path, NULL);
        } else if (a->choose_key) {
            msg(a, "Choose a picture from this computer");
        } else if (net_fetch(path, local, sizeof(local), err, sizeof(err))) {
            open_path(a, local);
            if (a->msg[0] == 0)
                msg(a, "Opened a copy of %s (changes are not sent back)", strrchr(path, '/') + 1);
        } else {
            msg(a, "%s", err);
        }
        return;
    }
    if (stat(path, &st) < 0) {
        msg(a, "%s: %s", path, strerror(errno));
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        go_to(a, cur(a), path, NULL);
        return;
    }
    char err[PATH_MAX + 80];
    if (a->choose_key) {
        if (classify(path) == K_IMAGE)
            choose_path(a, path);
        else
            msg(a, "Choose a picture (png, jpg or ppm)");
        return;
    }
    switch (classify(path)) {
    case K_IMAGE:
    case K_VIDEO:
        if (!launch("mellagui-view", path, workdir(a), err, sizeof(err)))
            msg(a, "%s", err);
        break;
    case K_PROGRAM: {
        char full[PATH_MAX];
        join_path(cur(a)->cwd, path, full, sizeof(full));
        if (!launch(full, NULL, workdir(a), err, sizeof(err)))
            msg(a, "%s", err);
        break;
    }
    case K_TEXT:
        if (!launch("mellagui-edit", path, workdir(a), err, sizeof(err)))
            msg(a, "%s", err);
        break;
    case K_BINARY: msg(a, "%s looks like a binary file: not opened (F3 shows it)", strrchr(path, '/') + 1); break;
    case K_UNREADABLE: msg(a, "cannot read %s: %s", path, strerror(errno)); break;
    }
}

static void open_selected(struct app *a)
{
    struct pane *p = cur(a);
    if (p->sel == 0 && has_up(p)) {
        go_up(a);
        return;
    }
    const struct entry *e = row_entry(p, p->sel);
    if (!e)
        return;
    if (!strcmp(p->cwd, NET_PREFIX) && !strcmp(e->name, NET_ASK)) {
        prompt_start(a, P_CONNECT, "");
        return;
    }
    char path[PATH_MAX];
    join_path(p->cwd, e->name, path, sizeof(path));
    if (e->broken) {
        msg(a, "%s is a link to nothing", e->name);
        return;
    }
    open_path(a, path);
}

static void select_row(struct app *a, int row)
{
    struct pane *p = cur(a);
    p->sel = LM_CLAMP(row, 0, LM_MAX(rows(p) - 1, 0));
    if (p->sel < p->top)
        p->top = p->sel;
    if (p->sel >= p->top + a->rows_visible)
        p->top = p->sel - a->rows_visible + 1;
    p->top = LM_MAX(p->top, 0);
    a->win.redraw = true;
}

static void start_terminal(struct app *a)
{
    char err[PATH_MAX + 80];
    if (is_net(cur(a)->cwd)) {
        msg(a, "There is no terminal on a network drive");
        return;
    }
    if (!launch("mellagui-term", NULL, cur(a)->cwd, err, sizeof(err)))
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
    go_to(a, cur(a), dir, NULL);
}

static void switch_pane(struct app *a)
{
    if (!a->dual)
        return;
    a->act = 1 - a->act;
    set_title(a);
    a->win.redraw = true;
}

static void toggle_preview(struct app *a)
{
    if (!a->dual) {
        msg(a, "No preview here");
        return;
    }
    a->preview = !a->preview;
    a->pv.want[0] = 0;
    a->pv.loaded = false;
    if (!a->preview)
        preview_clear(&a->pv);
    a->win.redraw = true;
}

/* Insert / Space: mark or unmark the file under the cursor and move down. */
static void toggle_mark(struct app *a)
{
    struct pane *p = cur(a);
    struct entry *e = row_entry_rw(p, p->sel);
    if (e)
        e->marked = !e->marked;
    select_row(a, p->sel + 1);
}

/* ---- file operations --------------------------------------------------------------------------------------------------- */

/* The names an operation works on: the marked entries, else the one under the cursor ("..", which is no file, never). Free with
 * free_names. Returns the count. */
static int op_targets(struct app *a, char ***out)
{
    struct pane *p = cur(a);
    int nm = count_marked(p), n = 0;
    char **names = calloc((size_t)LM_MAX(nm, 1), sizeof(char *));
    if (!names) {
        *out = NULL;
        return 0;
    }
    if (nm) {
        for (int i = 0; i < p->n; i++)
            if (p->e[i].marked)
                names[n++] = strdup(p->e[i].name);
    } else {
        const struct entry *e = row_entry(p, p->sel);
        if (e)
            names[n++] = strdup(e->name);
    }
    *out = names;
    return n;
}

static void free_names(char **names, int n)
{
    for (int i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

static void expand_home(const char *in, char *out, size_t n)
{
    const char *home = getenv("HOME");
    if (in[0] == '~' && (in[1] == '/' || !in[1]) && home && *home)
        snprintf(out, n, "%.*s%s", (int)(n / 2), home, in + 1);
    else
        snprintf(out, n, "%s", in);
}

static void job_finish(struct app *a);

/* Starts the job: in a thread (the window stays alive and shows the progress), or at once in the caller with `sync_jobs`. */
static void job_start(struct app *a, struct job *j)
{
    a->job = j;
    a->msg[0] = 0;                                                      /* the progress line takes the place of the last message */
    if (!a->sync_jobs && pthread_create(&j->thread, NULL, job_thread, j) == 0) {
        j->threaded = true;
        a->win.redraw = true;
        return;
    }
    job_execute(j);
    atomic_store(&j->finished, true);
    job_finish(a);
}

/* The job is over (called from the main loop once `finished` is set): take the thread back, show the result. */
static void job_finish(struct app *a)
{
    struct job *j = a->job;
    if (!j)
        return;
    if (j->threaded)
        pthread_join(j->thread, NULL);
    a->job = NULL;
    struct pane *p = &a->pane[j->pane];
    for (int i = 0; i < p->n; i++)
        p->e[i].marked = false;
    reread(a);
    bool cancelled = atomic_load(&j->cancel);
    int n = j->requested, bad = n - j->ok;
    const char *first = j->first_skip[0] ? j->first_skip : j->first_err;
    const char *verb = j->kind == J_COPY ? "Copied" : j->kind == J_MOVE ? "Moved" : j->kind == J_TRASH ? "Trashed" : j->kind == J_RESTORE ? "Restored" : "Deleted";
    if (j->emptying && !cancelled && bad <= 0)
        msg(a, "Emptied the trash (%d item%s)", j->nfiles, j->nfiles == 1 ? "" : "s");
    else if (cancelled)
        msg(a, "Cancelled: %s %d of %d", verb, j->ok, n);
    else if (bad > 0)
        msg(a, "%s %d of %d: %s", verb, j->ok, n, first);
    else if (j->kind == J_TRASH)
        msg(a, "Moved %d item%s to the trash", j->ok, j->ok == 1 ? "" : "s");
    else if (j->kind == J_RESTORE)
        msg(a, "Put back %d item%s where %s from", j->ok, j->ok == 1 ? "" : "s", j->ok == 1 ? "it came" : "they came");
    else if (j->kind == J_DELETE)
        msg(a, "Deleted %d item%s", j->ok, j->ok == 1 ? "" : "s");
    else
        msg(a, "%s %d item%s to %s", verb, j->ok, j->ok == 1 ? "" : "s", j->target);
    job_free(j);
}

static bool job_busy(struct app *a)
{
    if (!a->job)
        return false;
    msg(a, "Busy: Esc cancels the running operation first");
    return true;
}

/* Copy or move to `where` (a folder, or a new name when only one thing is moved or copied). Nothing is ever replaced. */
static void do_copy_move(struct app *a, const char *where, bool move)
{
    char **names;
    int n = op_targets(a, &names);
    if (n == 0) {
        msg(a, "Nothing to %s", move ? "move" : "copy");
        free_names(names, n);
        return;
    }
    struct pane *p = cur(a);
    char t0[PATH_MAX], target[PATH_MAX];
    expand_home(where, t0, sizeof(t0));
    join_path(p->cwd, t0, target, sizeof(target));
    bool into = false;
    {
        bool isdir;
        long long sz;
        char e[200];
        into = stat_any(target, &isdir, &sz, e, sizeof(e)) == 0 && isdir;
    }
    if (!into && n > 1) {
        msg(a, "%s is not a folder: several files can only go into a folder", target);
        free_names(names, n);
        return;
    }
    struct job *j = job_new(move ? J_MOVE : J_COPY, n);
    if (!j) {
        free_names(names, n);
        return;
    }
    j->pane = a->act;
    j->requested = n;
    snprintf(j->target, sizeof(j->target), "%s", target);
    for (int i = 0; i < n; i++) {
        struct job_item *it = &j->items[j->n];
        char err[PATH_MAX + 120] = "";
        join_path(p->cwd, names[i], it->src, sizeof(it->src));
        if (into)
            join_path(target, names[i], it->dst, sizeof(it->dst));
        else
            snprintf(it->dst, sizeof(it->dst), "%s", target);
        if (!strcmp(it->src, it->dst))
            snprintf(err, sizeof(err), "%s is already there", names[i]);
        else if (path_inside(it->dst, it->src))
            snprintf(err, sizeof(err), "cannot put %s into itself", names[i]);
        else if (exists_net_or_local(it->dst))
            snprintf(err, sizeof(err), "%s already exists: not replaced", it->dst);
        else {
            j->n++;
            continue;
        }
        j->planned_skips++;
        if (!j->first_skip[0])
            snprintf(j->first_skip, sizeof(j->first_skip), "%s", err);
    }
    free_names(names, n);
    if (j->n == 0) {                                                    /* nothing left to do: say why */
        char why[sizeof(j->first_skip)];
        snprintf(why, sizeof(why), "%s", j->first_skip);
        for (int i = 0; i < p->n; i++)
            p->e[i].marked = false;
        job_free(j);
        msg(a, "%s 0 of %d: %s", move ? "Moved" : "Copied", n, why);
        return;
    }
    job_start(a, j);
}

/* Delete: into the trash, or for good when `hard` (or when the folder is the trash itself). */
static void do_delete(struct app *a, bool hard)
{
    char **names;
    int n = op_targets(a, &names);
    struct pane *p = cur(a);
    hard = hard || in_trash(p->cwd) || is_net(p->cwd);
    struct job *j = job_new(hard ? J_DELETE : J_TRASH, n);
    if (!j) {
        free_names(names, n);
        return;
    }
    if (!hard && !trash_dir(j->trash, sizeof(j->trash))) {
        msg(a, "There is no place for a trash folder (HOME is not set): use Shift+F8 to delete for good");
        free_names(names, n);
        job_free(j);
        return;
    }
    j->pane = a->act;
    j->requested = n;
    for (int i = 0; i < n; i++)
        join_path(p->cwd, names[i], j->items[j->n++].src, sizeof(j->items[0].src));
    free_names(names, n);
    if (j->n == 0) {
        job_free(j);
        return;
    }
    job_start(a, j);
}

/* Puts the marked (or the one under the cursor) trashed files back where they were deleted from. */
static void do_restore(struct app *a)
{
    struct pane *p = cur(a);
    if (job_busy(a))
        return;
    if (!in_trash_files(p->cwd)) {
        msg(a, "Open the trash first (the Trash button or Ctrl+B), then pick what to put back");
        return;
    }
    char **names;
    int n = op_targets(a, &names);
    struct job *j = job_new(J_RESTORE, n);
    if (!j) {
        free_names(names, n);
        return;
    }
    j->pane = a->act;
    j->requested = n;
    for (int i = 0; i < n; i++) {
        struct job_item *it = &j->items[j->n];
        char date[40], err[PATH_MAX + 160] = "";
        join_path(p->cwd, names[i], it->src, sizeof(it->src));
        if (!trash_origin(names[i], it->dst, sizeof(it->dst), date, sizeof(date)))
            snprintf(err, sizeof(err), "%s: no record of where it came from (use F6 to move it)", names[i]);
        else if (exists_any(it->dst))
            snprintf(err, sizeof(err), "%s already exists: not replaced", it->dst);
        else {
            j->n++;
            continue;
        }
        j->planned_skips++;
        if (!j->first_skip[0])
            snprintf(j->first_skip, sizeof(j->first_skip), "%s", err);
    }
    free_names(names, n);
    if (j->n == 0) {
        char why[sizeof(j->first_skip)];
        snprintf(why, sizeof(why), "%s", j->first_skip);
        job_free(j);
        msg(a, "Restored 0 of %d: %s", n, why);
        return;
    }
    job_start(a, j);
}

/* The top-level entries of a folder (not . and ..). */
static int count_dir(const char *path)
{
    DIR *d = opendir(path);
    int n = 0;
    struct dirent *de;
    while (d && (de = readdir(d)))
        n += strcmp(de->d_name, ".") && strcmp(de->d_name, "..");
    if (d)
        closedir(d);
    return n;
}

static void prompt_start(struct app *a, enum prompt kind, const char *initial);

static void cmd_empty_trash(struct app *a)
{
    if (job_busy(a))
        return;
    char t[PATH_MAX];
    if (!trash_files_dir(t, sizeof(t))) {
        msg(a, "There is no trash folder (HOME is not set)");
        return;
    }
    if (count_dir(t) == 0) {
        msg(a, "The trash is already empty");
        return;
    }
    prompt_start(a, P_EMPTY, "");
}

/* Deletes everything that is in the trash for good: the trashed files and their info files. The folders themselves stay. */
static void do_empty_trash(struct app *a)
{
    char files[PATH_MAX], info[PATH_MAX + 16];
    if (!trash_files_dir(files, sizeof(files)))
        return;
    snprintf(info, sizeof(info), "%.*s", PATH_MAX - 1, files);
    char *slash = strrchr(info, '/');
    if (slash)
        snprintf(slash + 1, sizeof(info) - (size_t)(slash + 1 - info), "info");
    int nf = count_dir(files), ni = count_dir(info);
    struct job *j = job_new(J_DELETE, nf + ni);
    if (!j)
        return;
    j->pane = a->act;
    j->emptying = true;
    j->nfiles = nf;
    for (int pass = 0; pass < 2; pass++) {
        const char *dir = pass == 0 ? files : info;
        DIR *d = opendir(dir);
        struct dirent *de;
        while (d && (de = readdir(d)) && j->n < nf + ni) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            snprintf(j->items[j->n++].src, sizeof(j->items[0].src), "%.*s/%.*s", PATH_MAX / 2, dir, NAME_MAX, de->d_name);
        }
        if (d)
            closedir(d);
    }
    j->requested = j->n;
    if (j->n == 0) {
        job_free(j);
        msg(a, "The trash is already empty");
        return;
    }
    job_start(a, j);
}

static void go_trash(struct app *a)
{
    char t[PATH_MAX];
    if (!trash_files_dir(t, sizeof(t))) {
        msg(a, "There is no trash folder (HOME is not set)");
        return;
    }
    mkdir_p(t, 0700);
    char info[PATH_MAX + 8];
    snprintf(info, sizeof(info), "%.*s/../info", PATH_MAX - 8, t);
    char norm[PATH_MAX];
    normalize(info, norm, sizeof(norm));
    mkdir_p(norm, 0700);
    go_to(a, cur(a), t, NULL);
}

/* The server and share of the pane's network place for an operation in it (NULL with a message when it cannot be one). */
static const struct smb_conn *net_pane_conn(struct app *a, struct netpath *np)
{
    char err[200];
    if (!net_parse(cur(a)->cwd, np) || np->level < 2) {
        msg(a, "This works inside a share, not in the list of servers or shares");
        return NULL;
    }
    const struct smb_conn *c = net_conn(np, err, sizeof(err));
    if (!c)
        msg(a, "%s", err);
    return c;
}

static void net_join_dir(const struct netpath *np, const char *name, char *out, size_t n)
{
    int w = snprintf(out, n, "%.2000s%s%.1000s", np->dir, np->dir[0] ? "/" : "", name);
    (void)w;
}

static void do_net_mkdir(struct app *a, const char *name)
{
    struct netpath np;
    const struct smb_conn *c = net_pane_conn(a, &np);
    char rp[PATH_MAX], why[SMB_ERR];
    if (!c)
        return;
    if (strchr(name, '/')) {
        msg(a, "A name has no slash here");
        return;
    }
    net_join_dir(&np, name, rp, sizeof(rp));
    if (rf_mkdir(c, &np, rp, why, sizeof(why), NULL) < 0) {
        msg(a, "cannot create %s: %s", name, why);
        return;
    }
    reread(a);
    for (int r = 0; r < rows(cur(a)); r++)
        if (!strcmp(row_name(cur(a), r), name))
            select_row(a, r);
    msg(a, "Created %s", name);
}

static void do_net_rename(struct app *a, const char *old, const char *new_name)
{
    struct netpath np;
    const struct smb_conn *c = net_pane_conn(a, &np);
    char from[PATH_MAX], to[PATH_MAX], why[SMB_ERR];
    if (!c)
        return;
    net_join_dir(&np, old, from, sizeof(from));
    net_join_dir(&np, new_name, to, sizeof(to));
    if (rf_rename(c, &np, from, to, why, sizeof(why), NULL) < 0) {
        msg(a, "cannot rename %s: %s", old, why);
        return;
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", cur(a)->cwd);
    reread_pane(a, other(a));
    go_to(a, cur(a), dir, new_name);
    msg(a, "Renamed to %s", new_name);
}

static void do_mkdir(struct app *a, const char *name)
{
    struct pane *p = cur(a);
    char full[PATH_MAX], t0[PATH_MAX];
    if (is_net(p->cwd)) {
        do_net_mkdir(a, name);
        return;
    }
    expand_home(name, t0, sizeof(t0));
    join_path(p->cwd, t0, full, sizeof(full));
    if (exists_any(full)) {
        msg(a, "%s already exists", full);
        return;
    }
    for (char *s = full + 1; *s; s++) {                                 /* "a/b/c" makes the folders in between, too */
        if (*s != '/')
            continue;
        *s = 0;
        if (mkdir(full, 0755) < 0 && errno != EEXIST) {
            msg(a, "cannot create %s: %s", full, strerror(errno));
            return;
        }
        *s = '/';
    }
    if (mkdir(full, 0755) < 0) {
        msg(a, "cannot create %s: %s", full, strerror(errno));
        return;
    }
    char dir[PATH_MAX];
    join_path(full, "..", dir, sizeof(dir));
    if (!strcmp(dir, p->cwd)) {
        reread(a);
        for (int r = 0; r < rows(p); r++)
            if (!strcmp(row_name(p, r), strrchr(full, '/') + 1))
                select_row(a, r);
    } else {
        reread(a);
    }
    msg(a, "Created %s", full);
}

static void do_rename(struct app *a, const char *new_name)
{
    struct pane *p = cur(a);
    const struct entry *e = row_entry(p, p->sel);
    if (!e || !new_name[0])
        return;
    if (strchr(new_name, '/')) {
        msg(a, "A name has no slash: use F6 to move a file somewhere else");
        return;
    }
    if (is_net(p->cwd)) {
        if (strcmp(e->name, new_name) != 0)
            do_net_rename(a, e->name, new_name);
        return;
    }
    char src[PATH_MAX], dst[PATH_MAX];
    join_path(p->cwd, e->name, src, sizeof(src));
    join_path(p->cwd, new_name, dst, sizeof(dst));
    if (!strcmp(src, dst))
        return;
    if (exists_any(dst)) {
        msg(a, "%s already exists: not replaced", new_name);
        return;
    }
    if (rename(src, dst) < 0) {
        msg(a, "cannot rename %s: %s", e->name, strerror(errno));
        return;
    }
    char keep[NAME_MAX + 1];
    snprintf(keep, sizeof(keep), "%s", new_name);
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", p->cwd);
    reread_pane(a, other(a));
    go_to(a, p, dir, keep);
    msg(a, "Renamed to %s", new_name);
}

static void prompt_start(struct app *a, enum prompt kind, const char *initial)
{
    a->prompt = kind;
    snprintf(a->pbuf, sizeof(a->pbuf), "%.*s", (int)sizeof(a->pbuf) - 1, initial);
    wltext_end(a->pbuf);                                                /* the caret at the end of what is suggested */
    a->win.redraw = true;
}

static void cmd_copy_move(struct app *a, bool move)
{
    if (job_busy(a))
        return;
    char **names;
    int n = op_targets(a, &names);
    free_names(names, n);
    if (!a->dual || n == 0) {
        msg(a, "Nothing to %s", move ? "move" : "copy");
        return;
    }
    char init[PATH_MAX + 2];
    snprintf(init, sizeof(init), "%s%s", other(a)->cwd, other(a)->cwd[strlen(other(a)->cwd) - 1] != '/' ? "/" : "");
    prompt_start(a, move ? P_MOVE : P_COPY, init);
}

static void cmd_delete(struct app *a, bool hard)
{
    if (job_busy(a))
        return;
    if (!strcmp(cur(a)->cwd, NET_PREFIX)) {                             /* in the list of servers a delete forgets one */
        const struct entry *e = row_entry(cur(a), cur(a)->sel);
        if (!e || !strcmp(e->name, NET_ASK)) {
            msg(a, "Select a server to remove it from the list");
            return;
        }
        char nm[64];
        snprintf(nm, sizeof(nm), "%s", e->name);
        net_forget(nm);
        reread(a);
        msg(a, "%s is not in the list any more", nm);
        return;
    }
    if (is_net(cur(a)->cwd)) {
        struct netpath np;
        if (net_parse(cur(a)->cwd, &np) && np.level < 2) {
            msg(a, "Shares cannot be deleted here");
            return;
        }
    }
    char **names;
    int n = op_targets(a, &names);
    free_names(names, n);
    if (!a->dual || n == 0) {
        msg(a, "Nothing to delete");
        return;
    }
    prompt_start(a, hard || in_trash(cur(a)->cwd) || is_net(cur(a)->cwd) ? P_DELETE_HARD : P_DELETE, "");
}

static void cmd_mkdir(struct app *a)
{
    if (a->dual && !job_busy(a))
        prompt_start(a, P_MKDIR, "");
}

static void cmd_rename(struct app *a)
{
    struct pane *p = cur(a);
    const struct entry *e = row_entry(p, p->sel);
    if (job_busy(a))
        return;
    if (!a->dual || !e) {
        msg(a, "Nothing to rename");
        return;
    }
    prompt_start(a, P_RENAME, e->name);
}

static void cmd_edit(struct app *a)
{
    struct pane *p = cur(a);
    const struct entry *e = row_entry(p, p->sel);
    if (!e || e->dir) {
        msg(a, "Select a file to edit");
        return;
    }
    char path[PATH_MAX], err[PATH_MAX + 80];
    join_path(p->cwd, e->name, path, sizeof(path));
    if (is_net(path)) {                                                 /* edit a copy of it */
        char local[PATH_MAX];
        if (!net_fetch(path, local, sizeof(local), err, sizeof(err))) {
            msg(a, "%s", err);
            return;
        }
        snprintf(path, sizeof(path), "%s", local);
        msg(a, "Editing a copy of %s: it is not sent back, copy it over with F5", e->name);
    }
    if (classify(path) != K_TEXT) {
        msg(a, "%s is not a text file (Enter opens pictures)", e->name);
        return;
    }
    if (!launch("mellagui-edit", path, workdir(a), err, sizeof(err)))
        msg(a, "%s", err);
}

/* ---- round buttons ------------------------------------------------------------------------------------------------------- */

struct rbtn {
    const char *label;
    uint32_t base, hover;
    int x, w;
};

enum { B_UP, B_HOME, B_HIDDEN, B_TERM, B_PREVIEW, B_TRASH, B_RESTORE, B_EMPTY, B_NET, N_TOOL };

/* The buttons that belong to the two-pane manager (the picture chooser has neither). */
static bool tool_visible(const struct app *a, int i)
{
    if (i == B_RESTORE)                                                 /* only inside the trashed files */
        return a->dual && in_trash_files(a->pane[a->act].cwd);
    if (i == B_EMPTY)                                                   /* only while looking at the trash */
        return a->dual && in_trash(a->pane[a->act].cwd);
    return a->dual || (i != B_PREVIEW && i != B_TRASH && i != B_NET);
}
enum { F_HELP, F_RENAME, F_VIEW, F_EDIT, F_COPY, F_MOVE, F_MKDIR, F_DELETE, F_TERM, F_QUIT, N_FKEY };

static void toolbar(const struct app *a, struct rbtn *b)
{
    static const char *const labels[N_TOOL] = { "Up", "Home", "Hidden", "Terminal", "Preview", "Trash", "Restore", "Empty", "Network" };
    const uint32_t base[N_TOOL] = { lm_theme.btn_blue, lm_theme.btn_green, lm_theme.btn_purple, lm_theme.btn_teal, lm_theme.btn_amber, lm_theme.btn_red, lm_theme.btn_olive, lm_theme.btn_maroon, lm_theme.btn_deep_teal };
    const uint32_t hover[N_TOOL] = { lm_theme.btn_blue_lit, lm_theme.btn_green_lit, lm_theme.btn_purple_lit, lm_theme.btn_teal_lit, lm_theme.btn_amber_lit, lm_theme.btn_red_lit, lm_theme.btn_olive_lit, lm_theme.btn_maroon_lit, lm_theme.btn_deep_teal_lit };
    int x = 8;
    for (int i = 0; i < N_TOOL; i++) {
        b[i].label = labels[i];
        b[i].base = base[i];
        b[i].hover = hover[i];
        bool on = (i == B_HIDDEN && a->show_hidden) || (i == B_PREVIEW && a->preview) ||
                  (i == B_TRASH && a->dual && in_trash(a->pane[a->act].cwd)) || (i == B_NET && is_net(a->pane[a->act].cwd));
        if (on) {                                                       /* a switch that is on is lighter */
            b[i].base = hover[i];
            b[i].hover = lm_mix_rgb(hover[i], 0xffffff, 0.25);
        }
        b[i].w = (int)strlen(labels[i]) * LM_FONT_W + 20;
        b[i].x = x;
        if (tool_visible(a, i))                                         /* hidden buttons take no room */
            x += b[i].w + 6;
    }
}

/* The function key bar: ten buttons side by side over the whole width. */
static void fkeybar(const struct app *a, struct rbtn *b)
{
    static const char *const labels[N_FKEY] = { "Help", "Rename", "Preview", "Edit", "Copy", "Move", "Mkdir", "Delete", "Term", "Quit" };
    const uint32_t base[N_FKEY] = { lm_theme.btn_slate, lm_theme.btn_amber, lm_theme.btn_purple, lm_theme.btn_teal, lm_theme.btn_green, lm_theme.btn_orange, lm_theme.btn_blue, lm_theme.btn_red, lm_theme.btn_deep_teal, lm_theme.btn_grey };
    const uint32_t hover[N_FKEY] = { lm_theme.btn_slate_lit, lm_theme.btn_amber_lit, lm_theme.btn_purple_lit, lm_theme.btn_teal_lit, lm_theme.btn_green_lit, lm_theme.btn_orange_lit, lm_theme.btn_blue_lit, lm_theme.btn_red_lit, lm_theme.btn_deep_teal_lit, lm_theme.btn_grey_lit };
    int gap = 4, width = a->win.bw - 12;
    int bw = (width - gap * (N_FKEY - 1)) / N_FKEY;
    for (int i = 0; i < N_FKEY; i++) {
        b[i].label = labels[i];
        b[i].base = (i == F_VIEW && a->preview) ? hover[i] : base[i];
        b[i].hover = hover[i];
        b[i].x = 6 + i * (bw + gap);
        b[i].w = bw;
    }
}

static void draw_rbtn(struct wlwin *w, const struct rbtn *b, int y, int h, bool hot, int num)
{
    lm_round_fill(w->pixels, w->bw, w->bw, w->bh, b->x, y, b->w, h, lm_button_radius(h / 2), hot ? b->hover : b->base);
    char t[48];
    if (num > 0)
        snprintf(t, sizeof(t), "%d%s", num, b->label);
    else
        snprintf(t, sizeof(t), "%s", b->label);
    int tw = (int)strlen(t) * LM_FONT_W;
    int tx = b->x + LM_MAX((b->w - tw) / 2, 4);
    if (num > 0) {                                                      /* the key number a little dimmer than the name */
        char ns[8];
        int nl = snprintf(ns, sizeof(ns), "%d", num);
        wlwin_text(w, tx, y + (h - LM_FONT_H) / 2, ns, 1, lm_mix_rgb(lm_theme.on_colour, b->base, 0.45), 0);
        wlwin_text(w, tx + nl * LM_FONT_W, y + (h - LM_FONT_H) / 2, b->label, 1, lm_theme.on_colour, b->w - 8 - nl * LM_FONT_W);
    } else {
        wlwin_text(w, tx, y + (h - LM_FONT_H) / 2, t, 1, lm_theme.on_colour, b->w - 8);
    }
}

static void press_tool(struct app *a, int i)
{
    switch (i) {
    case B_UP: go_up(a); break;
    case B_HOME: go_home(a); break;
    case B_HIDDEN: toggle_hidden(a); break;
    case B_TERM: start_terminal(a); break;
    case B_PREVIEW: toggle_preview(a); break;
    case B_TRASH: go_trash(a); break;
    case B_RESTORE: do_restore(a); break;
    case B_EMPTY: cmd_empty_trash(a); break;
    case B_NET: go_network(a); break;
    }
}

static void press_fkey(struct app *a, int i)
{
    switch (i) {
    case F_HELP: a->help = true; break;
    case F_RENAME: cmd_rename(a); break;
    case F_VIEW: toggle_preview(a); break;
    case F_EDIT: cmd_edit(a); break;
    case F_COPY: cmd_copy_move(a, false); break;
    case F_MOVE: cmd_copy_move(a, true); break;
    case F_MKDIR: cmd_mkdir(a); break;
    case F_DELETE: cmd_delete(a, (a->win.mods & LM_MOD_SHIFT) != 0); break;
    case F_TERM: start_terminal(a); break;
    case F_QUIT: a->win.running = false; break;
    }
    a->win.redraw = true;
}

/* ---- keys ---------------------------------------------------------------------------------------------------------------- */

static void jump_to_letter(struct app *a, char ch)
{
    struct pane *p = cur(a);
    int n = rows(p);
    for (int k = 1; k <= n; k++) {
        int r = (p->sel + k) % n;
        const char *nm = row_name(p, r);
        if (r == 0 && has_up(p))
            continue;
        if (tolower((unsigned char)nm[0]) == tolower((unsigned char)ch)) {
            select_row(a, r);
            return;
        }
    }
}

/* "host", "user@host", "host legacy": adds the server and goes in. */
static void net_connect(struct app *a, const char *text)
{
    net_load();
    char host[128] = "", user[64] = "", flag[16] = "";
    if (sscanf(text, "%127s %15s", host, flag) < 1)
        return;
    bool legacy = !strcmp(flag, "legacy");
    bool sftp = false;
    int port = 0;
    if (!strncmp(host, SFTP_PREFIX, strlen(SFTP_PREFIX)) || !strncmp(host, "ssh://", 6)) {          /* an SSH server: sftp://[user@]host[:port] */
        memmove(host, strstr(host, "://") + 3, strlen(strstr(host, "://") + 3) + 1);
        sftp = true;
    }
    char *at = strchr(host, '@');
    if (at) {
        *at = 0;
        snprintf(user, sizeof(user), "%s", host);
        memmove(host, at + 1, strlen(at + 1) + 1);
    }
    if (sftp) {
        char *colon = strrchr(host, ':');
        if (colon && colon != host && atoi(colon + 1) > 0) {
            port = atoi(colon + 1);
            *colon = 0;
        }
        if (!sftp_available()) {
            msg(a, "SSH servers need sftp (package openssh-client)");
            return;
        }
    }
    if (!host[0] || strchr(host, '/') || strchr(host, '\\') || !smb_name_ok(host)) {
        msg(a, "That is not a server name");
        return;
    }
    struct netconn *c = sftp ? net_add_sftp(host, user, port) : net_add(host, user, legacy);
    if (!c) {
        msg(a, "Too many servers: remove one first (Delete in the list of servers)");
        return;
    }
    c->keep = true;
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s%s", c->c.sftp ? SFTP_PREFIX : NET_PREFIX, c->c.name);
    go_to(a, cur(a), dir, NULL);
}

static void go_network(struct app *a)
{
    if (!smb_available() && !sftp_available()) {
        msg(a, "Network drives need smbclient (package smbclient) or sftp (package openssh-client)");
        return;
    }
    go_to(a, cur(a), NET_PREFIX, NULL);
}

static void prompt_accept(struct app *a)
{
    enum prompt kind = a->prompt;
    char text[PATH_MAX], full[PATH_MAX];
    snprintf(text, sizeof(text), "%s", a->pbuf);
    a->prompt = P_NONE;
    a->win.redraw = true;
    switch (kind) {
    case P_DELETE: do_delete(a, false); return;
    case P_DELETE_HARD: do_delete(a, true); return;
    case P_EMPTY: do_empty_trash(a); return;
    case P_COPY: if (text[0]) do_copy_move(a, text, false); return;
    case P_MOVE: if (text[0]) do_copy_move(a, text, true); return;
    case P_MKDIR: if (text[0]) do_mkdir(a, text); return;
    case P_RENAME: do_rename(a, text); return;
    case P_CONNECT: if (text[0]) net_connect(a, text); return;
    case P_NETUSER:
        snprintf(a->net_user, sizeof(a->net_user), "%.63s", text);
        prompt_start(a, P_NETPASS, "");
        return;
    case P_NETPASS: {
        struct netpath np;
        struct netconn *c = net_parse(a->net_pending, &np) ? net_find_kind(np.conn, np.sftp) : NULL;
        if (c) {
            snprintf(c->c.user, sizeof(c->c.user), "%s", a->net_user);
            snprintf(c->c.pass, sizeof(c->c.pass), "%.127s", text);
            c->keep = true;
            go_to(a, &a->pane[a->net_pane], a->net_pending, NULL);
        }
        explicit_bzero(text, sizeof(text));
        explicit_bzero(a->pbuf, sizeof(a->pbuf));
        return;
    }
    default: break;
    }
    if (!text[0])
        return;
    char t2[PATH_MAX];
    expand_home(text, t2, sizeof(t2));
    struct pane *p = cur(a);
    join_path(p->cwd, t2, full, sizeof(full));
    bool isdir;
    long long fsize;
    char serr[300];
    if (stat_any(full, &isdir, &fsize, serr, sizeof(serr)) < 0) {
        msg(a, "%s: %s", full, serr);
        return;
    }
    if (isdir) {
        go_to(a, p, full, NULL);
    } else {                                                           /* a file: show its folder with it selected, and open it */
        char dir[PATH_MAX];
        join_path(full, "..", dir, sizeof(dir));
        go_to(a, p, dir, strrchr(full, '/') + 1);
        open_path(a, full);
    }
}

static const char *prompt_label(enum prompt k)
{
    switch (k) {
    case P_GOTO: return "Go to: ";
    case P_COPY: return "Copy to: ";
    case P_MOVE: return "Move to: ";
    case P_MKDIR: return "New folder: ";
    case P_RENAME: return "Rename to: ";
    case P_CONNECT: return "Server (name, user@name, sftp://user@name): ";
    case P_NETUSER: return "User name (empty = guest): ";
    case P_NETPASS: return "Password: ";
    case P_DELETE:
    case P_DELETE_HARD:
    case P_EMPTY: return "";
    default: return "";
    }
}

static void scroll_preview(struct app *a, int lines)
{
    struct preview *pv = &a->pv;
    pv->top = LM_CLAMP(pv->top + lines, 0, LM_MAX(pv->nlines - 1, 0));
    a->win.redraw = true;
}

static void handle_key(struct app *a, uint32_t ks)
{
    bool ctrl = (a->win.mods & LM_MOD_CTRL) != 0, alt = (a->win.mods & LM_MOD_ALT) != 0;
    bool super = (a->win.mods & LM_MOD_SUPER) != 0, shift = (a->win.mods & LM_MOD_SHIFT) != 0;
    char txt[8];
    int tl = lm_keysym_to_utf8(ks, txt);
    if (a->help) {                                                      /* any key closes the key list (but not a bare modifier) */
        if (ks < 0xffe1 || ks > 0xffee)
            a->help = false;
        a->win.redraw = true;
        return;
    }
    if (a->prompt == P_NONE && ks == LM_KS_Escape && a->job) {          /* Esc stops a running copy / move / delete */
        atomic_store(&a->job->cancel, true);
        msg(a, "Cancelling...");
        return;
    }
    if (a->prompt != P_NONE) {
        if (ks == LM_KS_Return || ks == LM_KS_KP_Enter) {
            prompt_accept(a);
        } else if (ks == LM_KS_Escape) {
            if (a->prompt == P_NETPASS)
                explicit_bzero(a->pbuf, sizeof(a->pbuf));
            a->prompt = P_NONE;
        } else if (a->prompt == P_DELETE || a->prompt == P_DELETE_HARD || a->prompt == P_EMPTY) {
            /* a question: only Enter and Esc answer it */
        } else if (ks == LM_KS_Tab) {                                   /* complete a name (not for the new folder / rename fields) */
            if ((a->prompt == P_GOTO || a->prompt == P_COPY || a->prompt == P_MOVE) && !is_net(cur(a)->cwd) && !is_net(a->pbuf)) {
                char list[200];
                int cn = lm_complete_path(cur(a)->cwd, a->pbuf, sizeof(a->pbuf), true, list, sizeof(list));
                if (cn == 0)
                    msg(a, "Nothing here starts like that");
                else if (cn > 1)
                    msg(a, "%d matches: %s", cn, list);
            }
        } else if (a->prompt == P_NETPASS && (((a->win.mods & LM_MOD_CTRL) && ks != 'v' && ks != 'V' && ks != 'a' && ks != 'A') ||
                                              (ks == LM_KS_Delete && (a->win.mods & LM_MOD_SHIFT)))) {
            /* a password is never copied or cut to the clipboard */
        } else {
            wltext_edit(&a->clip, a->pbuf, sizeof(a->pbuf), ks, a->win.mods);      /* a caret, a selection, the clipboard (wltext.h) */
        }
        a->win.redraw = true;
        return;
    }
    if (tl == 1 && ctrl && !alt && !super) {
        switch (txt[0] | 0x20) {
        case 'h': toggle_hidden(a); return;
        case 'r': reread(a); return;
        case 't': start_terminal(a); return;
        case 'q': toggle_preview(a); return;
        case 'b': if (a->dual) go_trash(a); return;
        case 'z': if (a->dual) do_restore(a); return;
        case 'e': if (a->dual) cmd_empty_trash(a); return;
        case 'n': if (a->dual) go_network(a); return;
        case 'u': {                                                     /* what eats the disk in this folder */
            if (is_net(cur(a)->cwd)) {
                msg(a, "Disk usage is for folders on this computer");
                return;
            }
            char err[PATH_MAX + 80];
            if (!launch("mellagui-du", cur(a)->cwd, cur(a)->cwd, err, sizeof(err)))
                msg(a, "%s", err);
            return;
        }
        case 'k':
            if (a->dual) {
                if (smb_available() || sftp_available())
                    prompt_start(a, P_CONNECT, "");
                else
                    go_network(a);
            }
            return;
        case 'l':
            prompt_start(a, P_GOTO, "");
            snprintf(a->pbuf, sizeof(a->pbuf), "%.*s%s", (int)sizeof(a->pbuf) - 2, cur(a)->cwd, cur(a)->cwd[strlen(cur(a)->cwd) - 1] != '/' ? "/" : "");
            return;
        default: return;
        }
    }
    struct pane *p = cur(a);
    switch (ks) {
    case LM_KS_Up:
        if (alt)
            go_up(a);
        else if (shift && a->preview)
            scroll_preview(a, -1);
        else
            select_row(a, p->sel - 1);
        return;
    case LM_KS_Down:
        if (shift && a->preview)
            scroll_preview(a, 1);
        else
            select_row(a, p->sel + 1);
        return;
    case LM_KS_Page_Up:
        if (shift && a->preview)
            scroll_preview(a, -LM_MAX(a->rows_visible - 1, 1));
        else
            select_row(a, p->sel - LM_MAX(a->rows_visible - 1, 1));
        return;
    case LM_KS_Page_Down:
        if (shift && a->preview)
            scroll_preview(a, LM_MAX(a->rows_visible - 1, 1));
        else
            select_row(a, p->sel + LM_MAX(a->rows_visible - 1, 1));
        return;
    case LM_KS_Home: select_row(a, 0); return;
    case LM_KS_End: select_row(a, rows(p) - 1); return;
    case LM_KS_Return:
    case LM_KS_KP_Enter:
    case LM_KS_Right: open_selected(a); return;
    case LM_KS_BackSpace:
    case LM_KS_Left: go_up(a); return;
    case LM_KS_Tab: switch_pane(a); return;
    case LM_KS_Insert: if (a->dual) toggle_mark(a); return;
    case LM_KS_Delete: cmd_delete(a, shift); return;
    case LM_KS_F1: if (a->dual) a->help = true; return;
    case LM_KS_F1 + 1: if (a->dual) cmd_rename(a); return;
    case LM_KS_F1 + 2: if (a->dual) toggle_preview(a); return;
    case LM_KS_F1 + 3: if (a->dual) cmd_edit(a); return;
    case LM_KS_F1 + 4:                                                  /* F5: copy (the chooser has no files to copy: it rereads) */
        if (a->dual)
            cmd_copy_move(a, false);
        else
            reread(a);
        return;
    case LM_KS_F1 + 5: if (a->dual) cmd_copy_move(a, true); return;
    case LM_KS_F1 + 6: if (a->dual) cmd_mkdir(a); return;
    case LM_KS_F1 + 7: if (a->dual) cmd_delete(a, shift); return;
    case LM_KS_F1 + 8: if (a->dual) start_terminal(a); return;
    case LM_KS_F1 + 9: if (a->dual) a->win.running = false; return;
    case LM_KS_Escape:
        if (a->choose_key) {
            a->win.running = false;
            return;
        }
        a->msg[0] = 0;
        a->win.redraw = true;
        return;
    default: break;
    }
    if (!ctrl && !alt && !super && tl == 1 && txt[0] == ' ' && a->dual) {
        toggle_mark(a);
        return;
    }
    if (!ctrl && !alt && !super && tl == 1 && isalnum((unsigned char)txt[0]))
        jump_to_letter(a, txt[0]);
}

/* ---- layout -------------------------------------------------------------------------------------------------------------- */

static int bottom_h(const struct app *a)
{
    return STATUS_H + (a->dual ? FKEY_H : 0) + (a->prompt != P_NONE ? STATUS_H : 0);
}

static int list_top(void)
{
    return TOOL_H + HEAD_H;
}

/* A pane's column: x and width. */
static void pane_rect(const struct app *a, int i, int *x, int *w)
{
    if (!a->dual) {
        *x = 0;
        *w = a->win.bw;
        return;
    }
    int half = a->win.bw / 2;
    *x = i == 0 ? 0 : half;
    *w = i == 0 ? half : a->win.bw - half;
}

static bool pane_is_preview(const struct app *a, int i)
{
    return a->dual && a->preview && i != a->act;
}

/* The scroll bar's track runs from the top of the list to the bottom bars; the thumb is as long as the visible part of the list. */
static int sb_track_h(const struct app *a)
{
    return a->win.bh - list_top() - bottom_h(a);
}

static void scrollbar_thumb(const struct app *a, const struct pane *p, int *ty, int *th)
{
    int total = LM_MAX(rows(p), 1), track = LM_MAX(sb_track_h(a), 1);
    *th = LM_CLAMP(track * a->rows_visible / total, LM_MIN(28, track), track);
    *ty = list_top() + (track - *th) * p->top / LM_MAX(total - a->rows_visible, 1);
}

/* The thumb follows the pointer (grabbed `sb_grab` pixels below its top). */
static void scrollbar_drag(struct app *a, struct pane *p, double py)
{
    int ty, th, track = sb_track_h(a), span = LM_MAX(track, 1);
    scrollbar_thumb(a, p, &ty, &th);
    int room = LM_MAX(span - th, 1), total = rows(p);
    int pos = LM_CLAMP((int)py - p->sb_grab - list_top(), 0, room);
    p->top = LM_CLAMP(pos * LM_MAX(total - a->rows_visible, 0) / room, 0, LM_MAX(total - a->rows_visible, 0));
}

/* ---- drawing ------------------------------------------------------------------------------------------------------------- */

static uint32_t entry_color(const struct entry *e)
{
    if (!e || e->dir)
        return COL_DIR;
    if (e->marked)
        return COL_MARK;
    if (e->broken)
        return COL_DIM;
    if (ext_is(e->name, "png") || ext_is(e->name, "jpg") || ext_is(e->name, "jpeg") || ext_is(e->name, "ppm") || ext_is(e->name, "mp4") ||
        ext_is(e->name, "m4v"))
        return COL_PIC;
    return e->exec ? COL_EXEC : COL_FG;
}

static void paint_pane(struct wlwin *w, struct app *a, int idx)
{
    struct pane *p = &a->pane[idx];
    int x0, pw;
    pane_rect(a, idx, &x0, &pw);
    bool active = idx == a->act;
    int list_y = list_top(), total = rows(p), sbw = lm_metrics.scrollbar_width;
    p->top = LM_CLAMP(p->top, 0, LM_MAX(total - a->rows_visible, 0));        /* (the wheel and the scroll bar may leave the selection off screen) */
    bool bar = total > a->rows_visible;
    int right = x0 + pw - (bar ? sbw : 0);                                    /* the list ends where the scroll bar begins */

    int hy = TOOL_H;                                                          /* the header: the path, lit when the pane is active */
    wlwin_fill(w, x0, hy, pw, HEAD_H, active && a->dual ? COL_SEL : COL_BG);
    wlwin_text(w, x0 + 8, hy + 3, p->cwd, 1, active && a->dual ? lm_theme.text_bright : COL_DIM, pw - 16 - (count_marked(p) ? 80 : 0));
    int nm = count_marked(p);
    if (nm) {
        char mk[32];
        snprintf(mk, sizeof(mk), "%d marked", nm);
        wlwin_text(w, x0 + pw - 8 - (int)strlen(mk) * LM_FONT_W, hy + 3, mk, 1, COL_MARK, 0);
    }
    wlwin_fill(w, x0, list_y - 1, pw, 1, COL_BAR);

    int cols = pw >= 440 ? 3 : pw >= 260 ? 2 : 1;
    int time_x = right - 140, size_x = cols == 3 ? right - 250 : right - 86;
    for (int r = 0; r < a->rows_visible; r++) {
        int row = p->top + r;
        if (row >= total)
            break;
        const struct entry *e = row_entry(p, row);
        int y = list_y + r * ROW_H;
        if (row == p->sel)
            wlwin_fill(w, x0, y, right - x0, ROW_H, active ? COL_SEL : COL_SEL_IDLE);
        bool dir = !e || e->dir;
        char name[NAME_MAX + 4];
        snprintf(name, sizeof(name), "%s%s%s", e && e->marked ? "* " : "", e ? e->name : "..", dir ? "/" : "");
        wlwin_text(w, x0 + 10, y + 1, name, 1, entry_color(e), (cols > 1 ? size_x : right) - x0 - 20);
        if (cols > 1 && e && !e->dir && !e->broken && !(is_net(p->cwd) && !e->mtime)) {
            char sz[32];
            human_size(e->size, sz, sizeof(sz));
            wlwin_text(w, size_x, y + 1, sz, 1, COL_DIM, 0);
        }
        if (cols > 2 && e && !(is_net(p->cwd) && !e->mtime)) {
            char tm[32];
            struct tm lt;
            localtime_r(&e->mtime, &lt);
            strftime(tm, sizeof(tm), "%Y-%m-%d %H:%M", &lt);
            wlwin_text(w, time_x, y + 1, tm, 1, COL_DIM, 0);
        }
    }
    if (bar) {
        int ty, th;
        scrollbar_thumb(a, p, &ty, &th);
        bool hot = p->sb_drag || (a->px >= right && a->px < x0 + pw && a->py >= ty && a->py < ty + th);
        wlwin_fill(w, right, list_y, sbw, w->bh - list_y - bottom_h(a), COL_BAR);
        wlwin_fill(w, right + 2, ty + 1, sbw - 4, th - 2, hot ? COL_BTN_ON : COL_BTN);
    }
}

/* The path the preview wants: the file under the cursor of the active pane ("" for "..", or an empty folder). */
static void preview_target(struct app *a, char *out, size_t n)
{
    struct pane *p = cur(a);
    const struct entry *e = row_entry(p, p->sel);
    if (!e)
        out[0] = 0;
    else
        join_path(p->cwd, e->name, out, n);
}

/* Called while painting: notices a new cursor position and loads for it (a picture only once the cursor has rested). */
/* ---- a picture of a video for the preview: `mellagui-view --poster FILE OUT.ppm` in a thread (one at a time; it can take a moment) -------------------- */

static struct vjob {
    pthread_t th;
    bool started;                       /* a thread was made and not joined yet */
    atomic_int done;                    /* the thread has finished (0 / 1) */
    char path[PATH_MAX];                /* the video it works on */
    char out[PATH_MAX];                 /* the picture it writes (PPM) */
    char info[200];                     /* the info line mellagui-view printed, "" when it failed */
    char err[200];
} g_vj;

static void *vjob_main(void *arg)
{
    struct vjob *j = arg;
    char prog[PATH_MAX];
    sibling("mellagui-view", prog, sizeof(prog));
    int pf[2], ef[2];
    j->info[0] = j->err[0] = 0;
    if (pipe2(pf, O_CLOEXEC) == 0 && pipe2(ef, O_CLOEXEC) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            int nul = open("/dev/null", O_RDONLY);
            if (nul >= 0)
                dup2(nul, 0);
            dup2(pf[1], 1);
            dup2(ef[1], 2);
            execl(prog, prog, "--poster", j->path, j->out, (char *)NULL);
            _exit(127);
        }
        close(pf[1]);
        close(ef[1]);
        if (pid > 0) {
            char *dst[2] = { j->info, j->err };
            size_t cap[2] = { sizeof(j->info), sizeof(j->err) }, len[2] = { 0, 0 };
            int fds[2] = { pf[0], ef[0] };
            int64_t end = now_ms() + 8000;
            bool open_[2] = { true, true };
            while ((open_[0] || open_[1]) && now_ms() < end) {
                struct pollfd p[2] = { { .fd = fds[0], .events = POLLIN }, { .fd = fds[1], .events = POLLIN } };
                if (!open_[0]) p[0].fd = -1;
                if (!open_[1]) p[1].fd = -1;
                if (poll(p, 2, 100) <= 0)
                    continue;
                for (int k = 0; k < 2; k++)
                    if (open_[k] && (p[k].revents & (POLLIN | POLLHUP))) {
                        ssize_t n = read(fds[k], dst[k] + len[k], cap[k] - 1 - len[k]);
                        if (n <= 0)
                            open_[k] = false;
                        else
                            len[k] += (size_t)n;
                    }
            }
            if (open_[0] || open_[1]) {                                  /* too slow: give up (SIGCHLD is ignored here, the child goes away by itself) */
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
                snprintf(j->err, sizeof(j->err), "no preview: it took too long");
                j->info[0] = 0;
            }
            dst[0][len[0]] = 0;
            dst[1][len[1]] = 0;
        }
        close(pf[0]);
        close(ef[0]);
    }
    for (char *c = j->info; *c; c++)
        if (*c == '\n') { *c = 0; break; }
    for (char *c = j->err; *c; c++)
        if (*c == '\n') { *c = 0; break; }
    atomic_store(&j->done, 1);
    return NULL;
}

/* Starts / collects the thread for `pv`'s video; true while a picture is still being waited for. */
static bool video_poll(struct preview *pv)
{
    if (pv->kind != PV_VIDEO_WAIT)
        return false;
    if (g_vj.started && atomic_load(&g_vj.done)) {
        pthread_join(g_vj.th, NULL);
        g_vj.started = false;
        if (!strcmp(g_vj.path, pv->want)) {                                   /* it is the one that is asked for now */
            char sz[32] = "";
            struct stat st;
            if (stat(pv->want, &st) == 0)
                human_size(st.st_size, sz, sizeof(sz));
            struct lm_image img = { 0 };
            char err[160];
            if (g_vj.info[0] && lm_img_load(g_vj.out, &img, err, sizeof(err)) == 0) {
                int w = 0, h = 0, audio = 0;
                char codec[32] = "";
                unsigned long long ms = 0;
                sscanf(g_vj.info, "%dx%d codec=%31s duration_ms=%llu audio=%d", &w, &h, codec, &ms, &audio);
                int secs = (int)(ms / 1000);
                char dur[24] = "";
                if (secs >= 3600)
                    snprintf(dur, sizeof(dur), ", %d:%02d:%02d", secs / 3600, secs / 60 % 60, secs % 60);
                else if (ms)
                    snprintf(dur, sizeof(dur), ", %d:%02d", secs / 60, secs % 60);
                snprintf(pv->info, sizeof(pv->info), "%s, %dx%d%s, %s%s", sz, w, h, dur, codec, audio ? ", sound" : "");
                pv->img = img;
                pv->kind = PV_VIDEO;
            } else {
                int cap = 0;
                pv->kind = PV_ERROR;
                pv_linef(pv, &cap, "%s", g_vj.err[0] ? g_vj.err : "no preview of this video");
            }
        }
        unlink(g_vj.out);
        return pv->kind == PV_VIDEO_WAIT;                                     /* (the cursor moved on meanwhile: the next one starts below) */
    }
    if (!g_vj.started) {
        snprintf(g_vj.path, sizeof(g_vj.path), "%s", pv->want);
        const char *rt = getenv("XDG_RUNTIME_DIR");
        snprintf(g_vj.out, sizeof(g_vj.out), "%s/mellagui-files-poster-%d.ppm", rt && *rt ? rt : "/tmp", (int)getpid());
        atomic_store(&g_vj.done, 0);
        if (pthread_create(&g_vj.th, NULL, vjob_main, &g_vj) == 0)
            g_vj.started = true;
        else {
            int cap = 0;
            pv->kind = PV_ERROR;
            pv_linef(pv, &cap, "no preview: cannot start a thread");
            return false;
        }
    }
    return true;
}

static void preview_update(struct app *a)
{
    struct preview *pv = &a->pv;
    char want[PATH_MAX];
    preview_target(a, want, sizeof(want));
    if (strcmp(want, pv->want) != 0) {
        snprintf(pv->want, sizeof(pv->want), "%s", want);
        pv->loaded = false;
        bool pic = want[0] && (ext_is(want, "png") || ext_is(want, "jpg") || ext_is(want, "jpeg") || ext_is(want, "ppm"));
        pv->due = now_ms() + (pic ? 150 : 0);
    }
    if (!pv->loaded && now_ms() >= pv->due) {
        if (want[0])
            preview_load(pv, want, a->show_hidden);
        else
            preview_clear(pv);
        pv->loaded = true;
    }
    video_poll(pv);
}

static void paint_preview(struct wlwin *w, struct app *a, int idx)
{
    struct preview *pv = &a->pv;
    int x0, pw;
    pane_rect(a, idx, &x0, &pw);
    int list_y = list_top();
    int body_h = w->bh - list_y - bottom_h(a);
    wlwin_fill(w, x0, TOOL_H, pw, HEAD_H, COL_BG);
    char head[NAME_MAX + 200];
    if (pv->kind != PV_NONE && pv->loaded)
        snprintf(head, sizeof(head), "%s  -  %s", pv->title, pv->info);
    else
        snprintf(head, sizeof(head), "Preview");
    wlwin_text(w, x0 + 8, TOOL_H + 3, head, 1, COL_DIM, pw - 16);
    wlwin_fill(w, x0, list_y - 1, pw, 1, COL_BAR);
    wlwin_fill(w, x0, list_y, pw, body_h, lm_theme.field);
    if (!pv->loaded) {
        if (pv->want[0])
            wlwin_text(w, x0 + 10, list_y + 6, "...", 1, COL_DIM, 0);
        return;
    }
    if (pv->kind == PV_NONE) {
        wlwin_text(w, x0 + 10, list_y + 6, "Nothing to show", 1, COL_DIM, pw - 20);
        return;
    }
    if (pv->kind == PV_VIDEO_WAIT) {
        wlwin_text(w, x0 + 10, list_y + 6, "Reading the video...", 1, COL_DIM, pw - 20);
        return;
    }
    if (pv->kind == PV_IMAGE || pv->kind == PV_VIDEO) {
        int aw = pw - 16, ah = body_h - 16 - (pv->kind == PV_VIDEO ? LM_FONT_H + 6 : 0);
        if (!pv->scaled || pv->sw > aw || pv->sh > ah || (pv->sw < aw && pv->sh < ah && pv->sw < pv->img.w * 8))
            preview_scale(pv, aw, ah);
        if (pv->scaled) {
            int ox = x0 + (pw - pv->sw) / 2, oy = list_y + (body_h - pv->sh) / 2;
            for (int y = 0; y < pv->sh; y++) {
                if (oy + y < 0 || oy + y >= w->bh)
                    continue;
                for (int x = 0; x < pv->sw; x++)
                    if (ox + x >= x0 && ox + x < x0 + pw && ox + x < w->bw)
                        w->pixels[(size_t)(oy + y) * (size_t)w->bw + (size_t)(ox + x)] = 0xff000000u | pv->scaled[(size_t)y * (size_t)pv->sw + (size_t)x];
            }
            if (pv->kind == PV_VIDEO) {                                          /* a play mark on the picture and a hint under it */
                int cx = ox + pv->sw / 2, cy = oy + pv->sh / 2, r = LM_CLAMP(LM_MIN(pv->sw, pv->sh) / 7, 10, 28);
                for (int y = -r; y <= r; y++)
                    for (int x = -r; x <= r; x++) {
                        int px = cx + x, py = cy + y;
                        if (px < 0 || py < 0 || px >= w->bw || py >= w->bh || x * x + y * y > r * r)
                            continue;
                        bool tri = x > -r / 3 && x < r / 2 && abs(y) < (r / 2 - x) * 7 / 10 + 1 && x > -r / 3 + abs(y) / 2 - 1;
                        uint32_t *px32 = &w->pixels[(size_t)py * (size_t)w->bw + (size_t)px];
                        uint32_t o = *px32;
                        if (tri)
                            *px32 = 0xffffffffu;
                        else
                            *px32 = 0xff000000u | ((((o >> 16) & 255) / 2) << 16) | ((((o >> 8) & 255) / 2) << 8) | ((o & 255) / 2);
                    }
                wlwin_text(w, x0 + 8, list_y + body_h - LM_FONT_H - 4, "Enter plays it in the viewer", 1, COL_DIM, pw - 16);
            }
        }
        return;
    }
    int rowsv = LM_MAX(body_h / LM_FONT_H, 1);
    pv->top = LM_CLAMP(pv->top, 0, LM_MAX(pv->nlines - 1, 0));
    for (int r = 0; r < rowsv && pv->top + r < pv->nlines; r++) {
        uint32_t col = pv->kind == PV_ERROR ? COL_ERR : pv->kind == PV_DIR ? COL_DIR : pv->kind == PV_HEX ? COL_DIM : COL_FG;
        const char *s = pv->lines[pv->top + r];
        if (pv->kind == PV_DIR && s[0] && s[strlen(s) - 1] != '/')
            col = COL_FG;
        wlwin_text(w, x0 + 8, list_y + 2 + r * LM_FONT_H, s, 1, col, pw - 16);
    }
    if (pv->nlines == 0)
        wlwin_text(w, x0 + 10, list_y + 6, pv->kind == PV_DIR ? "An empty folder" : "An empty file", 1, COL_DIM, pw - 20);
}

static void paint_help(struct wlwin *w)
{
    static const char *const lines[] = {
        "File manager keys",
        "",
        "Tab               the other pane",
        "Enter / Right     open (a folder: go in); Backspace / Left: up",
        "Insert / Space    mark a file, then move down",
        "F2                rename the file under the cursor",
        "F3 / Ctrl+Q       preview of the file under the cursor in the other pane",
        "F4                edit a text file",
        "F5 / F6           copy / move to the other pane's folder (the name is editable)",
        "F7                new folder",
        "F8 / Delete       move to the trash (asks first); Shift+F8: delete for good",
        "Ctrl+B            open the trash; in it, Ctrl+Z / Restore puts files back",
        "Ctrl+E / Empty    delete everything in the trash for good (asks first)",
        "F9 / Ctrl+T       a terminal in this folder",
        "F10               quit",
        "",
        "Copy, move and delete take the marked files, else the one under the cursor.",
        "Nothing that exists is ever replaced. They run in the background: Esc cancels.",
        "In the question of F5 / F6 / Ctrl+L, Tab completes the names.",
        "Ctrl+H hidden files   Ctrl+R reread   Ctrl+L type a path",
        "Ctrl+N / Network  network drives (smb:// and sftp://); Ctrl+K connect to a server",
        "Ctrl+U            disk usage of this folder (mellagui-du)",
        "Shift+Up / Down scrolls the preview   a letter jumps to a name",
        "",
        "Any key or click closes this list",
    };
    int n = (int)(sizeof(lines) / sizeof(lines[0])), cw = 0;
    for (int i = 0; i < n; i++)
        cw = LM_MAX(cw, (int)strlen(lines[i]));
    int bw = LM_MIN(cw * LM_FONT_W + 32, w->bw - 8), bh = LM_MIN(n * LM_FONT_H + 24, w->bh - 8);
    int x = (w->bw - bw) / 2, y = (w->bh - bh) / 2;
    lm_round_fill(w->pixels, w->bw, w->bw, w->bh, x - 1, y - 1, bw + 2, bh + 2, 9, COL_DIR);
    lm_round_fill(w->pixels, w->bw, w->bw, w->bh, x, y, bw, bh, 8, COL_BAR);
    for (int i = 0; i < n && 12 + (i + 1) * LM_FONT_H <= bh; i++)
        wlwin_text(w, x + 16, y + 12 + i * LM_FONT_H, lines[i], 1, i == 0 ? COL_DIR : COL_FG, bw - 24);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    int list_y = list_top();
    a->rows_visible = LM_MAX((w->bh - list_y - bottom_h(a)) / ROW_H, 1);
    wlwin_fill(w, 0, 0, w->bw, w->bh, COL_BG);

    wlwin_fill(w, 0, 0, w->bw, TOOL_H, COL_BAR);
    struct rbtn b[N_FKEY];
    toolbar(a, b);
    for (int i = 0; i < N_TOOL; i++) {
        if (!tool_visible(a, i))
            continue;
        draw_rbtn(w, &b[i], 4, TOOL_H - 8, a->hover == i, 0);
    }
    if (a->preview)
        preview_update(a);
    for (int i = 0; i < (a->dual ? 2 : 1); i++) {
        if (pane_is_preview(a, i))
            paint_preview(w, a, i);
        else
            paint_pane(w, a, i);
    }
    if (a->dual) {
        int mid = w->bw / 2;
        wlwin_fill(w, mid - 1, TOOL_H, 1, w->bh - TOOL_H - bottom_h(a), COL_BAR);
    }

    int sy = w->bh - STATUS_H - (a->dual ? FKEY_H : 0);
    wlwin_fill(w, 0, sy, w->bw, STATUS_H, COL_BAR);
    struct pane *p = cur(a);
    bool showing = a->msg[0] && now_ms() < a->msg_until;
    char status[PATH_MAX + 128];
    const struct entry *e = row_entry(p, p->sel);
    char origin[PATH_MAX], odate[40];
    if (e && a->dual && in_trash_files(p->cwd) && trash_origin(e->name, origin, sizeof(origin), odate, sizeof(odate))) {
        for (char *t = odate; *t; t++)
            if (*t == 'T')
                *t = ' ';
        snprintf(status, sizeof(status), "%s  was in %s  (deleted %s)", e->name, origin, odate);
    } else if (e && a->dual) {
        char sz[32], tm[32];
        struct tm lt;
        human_size(e->size, sz, sizeof(sz));
        localtime_r(&e->mtime, &lt);
        strftime(tm, sizeof(tm), "%Y-%m-%d %H:%M", &lt);
        snprintf(status, sizeof(status), "%s  %s  %s   (%d item%s)", e->name, e->dir ? "folder" : sz, tm, p->n, p->n == 1 ? "" : "s");
    } else {
        snprintf(status, sizeof(status), "%d item%s%s", p->n, p->n == 1 ? "" : "s", a->show_hidden ? "  (hidden files shown)" : "");
    }
    if (a->job) {                                                       /* a copy / move / delete is running: a progress bar */
        struct job *j = a->job;
        long long total = atomic_load(&j->total), done = atomic_load(&j->done);
        int pct = total > 0 ? (int)LM_CLAMP(done * 100 / total, 0, 100) : (j->n ? atomic_load(&j->items_done) * 100 / j->n : 0);
        wlwin_fill(w, 0, sy, w->bw * pct / 100, STATUS_H, lm_theme.progress);
        char cur_name[NAME_MAX + 1], amount[96] = "", line[NAME_MAX + 200];
        pthread_mutex_lock(&j->mu);
        snprintf(cur_name, sizeof(cur_name), "%s", j->current);
        pthread_mutex_unlock(&j->mu);
        if (j->kind != J_DELETE && total > 0) {
            char d1[32], d2[32];
            human_size((off_t)done, d1, sizeof(d1));
            human_size((off_t)total, d2, sizeof(d2));
            snprintf(amount, sizeof(amount), "  (%s of %s)", d1, d2);
        }
        const char *verb = j->kind == J_COPY ? "Copying" : j->kind == J_MOVE ? "Moving" : j->kind == J_TRASH ? "Moving to the trash" : j->kind == J_RESTORE ? "Restoring" : "Deleting";
        snprintf(line, sizeof(line), "%s %s  %d%%%s   -   Esc cancels", verb, cur_name, pct, amount);
        wlwin_text(w, 6, sy + 2, a->msg[0] && now_ms() < a->msg_until ? a->msg : line, 1, lm_theme.on_colour, w->bw - 12);
    } else {
        wlwin_text(w, 6, sy + 2, showing ? a->msg : status, 1, showing ? COL_WARN : COL_FG, w->bw - 12);
    }
    if (a->prompt != P_NONE) {
        int py = sy - STATUS_H;
        wlwin_fill(w, 0, py, w->bw, STATUS_H, COL_BAR);
        wlwin_fill(w, 0, py, w->bw, 1, COL_DIR);
        char line[PATH_MAX + 80];
        if (a->prompt == P_DELETE || a->prompt == P_DELETE_HARD) {
            char **names;
            int n = op_targets(a, &names);
            bool hard = a->prompt == P_DELETE_HARD;
            snprintf(line, sizeof(line), "%s %d item%s%s%s%s %s", hard ? "Delete" : "Move", n, n == 1 ? "" : "s", n == 1 ? " (" : "",
                     n == 1 ? names[0] : "", n == 1 ? ")" : "",
                     hard ? "for good? This cannot be undone: Enter deletes, Esc cancels" : "to the trash? Enter moves, Esc cancels");
            free_names(names, n);
        } else if (a->prompt == P_EMPTY) {
            char t[PATH_MAX];
            int n = trash_files_dir(t, sizeof(t)) ? count_dir(t) : 0;
            snprintf(line, sizeof(line), "Empty the trash (%d item%s) for good? This cannot be undone: Enter empties it, Esc cancels", n,
                     n == 1 ? "" : "s");
        } else {
            line[0] = 0;
        }
        if (a->prompt != P_DELETE && a->prompt != P_DELETE_HARD && a->prompt != P_EMPTY) {          /* the label, then the field with its caret and selection */
            const char *label = prompt_label(a->prompt);
            wlwin_text(w, 6, py + 2, label, 1, COL_FG, w->bw - 12);
            int lw = lm_font_text_width(label);
            if (a->prompt == P_NETPASS) {                               /* stars, no caret model: a password is never shown */
                char stars[130];
                int k = 0;
                for (const char *c = a->pbuf; *c && k < 126; c++)
                    if (((unsigned char)*c & 0xc0) != 0x80)
                        stars[k++] = '*';
                stars[k++] = '_';
                stars[k] = 0;
                wlwin_text(w, 6 + lw, py + 2, stars, 1, COL_FG, w->bw - 12 - lw);
            } else {
                wltext_paint(w, 6 + lw, py + 2, w->bw - 12 - lw, a->pbuf, true, COL_FG, lm_theme.button_hover, COL_FG);
            }
        } else {
            wlwin_text(w, 6, py + 2, line, 1, a->prompt == P_DELETE_HARD || a->prompt == P_EMPTY ? COL_ERR : a->prompt == P_DELETE ? COL_WARN : COL_FG, w->bw - 12);
        }
    }
    if (a->dual) {
        struct rbtn f[N_FKEY];
        fkeybar(a, f);
        for (int i = 0; i < N_FKEY; i++)
            draw_rbtn(w, &f[i], w->bh - FKEY_H + 3, FKEY_H - 6, a->hover == 100 + i, i + 1);
    }
    if (a->help)
        paint_help(w);
}

/* ---- events --------------------------------------------------------------------------------------------------------------- */

static int hover_at(const struct app *a, double px, double py)
{
    int x = (int)px, y = (int)py;
    struct rbtn b[N_FKEY];
    if (y >= 4 && y < TOOL_H - 4) {
        toolbar(a, b);
        for (int i = 0; i < N_TOOL; i++)
            if (tool_visible(a, i) && x >= b[i].x && x < b[i].x + b[i].w)
                return i;
    }
    if (a->dual && y >= a->win.bh - FKEY_H + 3 && y < a->win.bh - 3) {
        fkeybar(a, b);
        for (int i = 0; i < N_FKEY; i++)
            if (x >= b[i].x && x < b[i].x + b[i].w)
                return 100 + i;
    }
    return -1;
}

static int pane_at(const struct app *a, int x)
{
    if (!a->dual)
        return 0;
    return x < a->win.bw / 2 ? 0 : 1;
}

static void click(struct app *a, uint32_t time_ms)
{
    int x = (int)a->px, y = (int)a->py;
    int h = hover_at(a, a->px, a->py);
    if (h >= 100) {
        press_fkey(a, h - 100);
        return;
    }
    if (h >= 0) {
        press_tool(a, h);
        return;
    }
    if (y < TOOL_H || y >= a->win.bh - bottom_h(a))
        return;
    int idx = pane_at(a, x);
    if (pane_is_preview(a, idx))
        return;
    if (idx != a->act) {
        a->act = idx;
        set_title(a);
        a->win.redraw = true;
    }
    struct pane *p = &a->pane[idx];
    int px0, pw;
    pane_rect(a, idx, &px0, &pw);
    int list_y = list_top();
    if (y < list_y)
        return;
    if (rows(p) > a->rows_visible && x >= px0 + pw - lm_metrics.scrollbar_width) {       /* the scroll bar */
        int ty, th;
        scrollbar_thumb(a, p, &ty, &th);
        int page = LM_MAX(a->rows_visible - 1, 1), maxtop = LM_MAX(rows(p) - a->rows_visible, 0);
        if (y >= ty && y < ty + th) {
            p->sb_drag = true;
            p->sb_grab = y - ty;
        } else {
            p->top = LM_CLAMP(p->top + (y < ty ? -page : page), 0, maxtop);
        }
        return;
    }
    int row = p->top + (y - list_y) / ROW_H;
    if (row >= rows(p) || (y - list_y) / ROW_H >= a->rows_visible)
        return;
    bool dbl = row == p->last_click_row && time_ms - p->last_click_ms < (uint32_t)lm_metrics.double_click_ms;
    p->last_click_ms = time_ms;
    p->last_click_row = row;
    select_row(a, row);
    if (dbl) {
        p->last_click_row = -1;
        open_selected(a);
    }
}

/* The row of the list under the pointer (making its pane the active one), -1 when the pointer is not on a row; *in_list tells whether it is in a list at all. */
static int row_under_pointer(struct app *a, bool *in_list)
{
    int x = (int)a->px, y = (int)a->py;
    *in_list = false;
    if (hover_at(a, a->px, a->py) >= 0 || y < TOOL_H || y >= a->win.bh - bottom_h(a))
        return -1;
    int idx = pane_at(a, x);
    if (pane_is_preview(a, idx) || y < list_top())
        return -1;
    *in_list = true;
    if (idx != a->act) {
        a->act = idx;
        set_title(a);
    }
    struct pane *p = &a->pane[idx];
    int px0, pw;
    pane_rect(a, idx, &px0, &pw);
    if (rows(p) > a->rows_visible && x >= px0 + pw - lm_metrics.scrollbar_width)
        return -1;
    int row = p->top + (y - list_top()) / ROW_H;
    if (row >= rows(p) || (y - list_top()) / ROW_H >= a->rows_visible)
        return -1;
    return row;
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        a->hover = hover_at(a, a->px, a->py);
        w->redraw = true;
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        a->hover = -1;
        w->redraw = true;
        break;
    case LM_WL_POINTER_EVT_MOTION:
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        a->hover = hover_at(a, a->px, a->py);
        for (int i = 0; i < 2; i++)
            if (a->pane[i].sb_drag)
                scrollbar_drag(a, &a->pane[i], a->py);
        w->redraw = true;                                               /* (the thumb lights up under the pointer) */
        break;
    case LM_WL_POINTER_EVT_BUTTON:
        a->clip.serial = args[0].u;
        if (args[2].u == 273 && args[3].u == 1) {                       /* BTN_RIGHT while a text prompt is asking: Cut / Copy / Paste / Clear */
            if (a->prompt != P_NONE && a->prompt != P_DELETE && a->prompt != P_DELETE_HARD && a->prompt != P_EMPTY && a->prompt != P_NETPASS) {
                a->menu_kind = 0;
                wlwin_menu(w, wltext_menu(a->pbuf));
            } else if (a->prompt == P_NONE && a->dual && !a->help && !a->job) {      /* a file's menu, or one for the empty part of the list */
                bool in_list;
                int row = row_under_pointer(a, &in_list);
                const struct entry *e = row >= 0 ? row_entry(cur(a), row) : NULL;
                if (e) {
                    select_row(a, row);
                    a->menu_kind = 1;
                    wlwin_menu(w, e->dir ? "Open\n~Edit\nRename...\nCopy...\nMove...\nDelete...\nNew folder...\nTerminal here" :
                                                "Open\nEdit\nRename...\nCopy...\nMove...\nDelete...\nNew folder...\nTerminal here");
                } else if (in_list) {
                    a->menu_kind = 2;
                    wlwin_menu(w, "New folder...\nTerminal here\nRead the folder again");
                }
                w->redraw = true;
            }
            break;
        }
        if (args[2].u == 272 && args[3].u == 1) {
            if (a->help) {                                              /* a click closes the key list */
                a->help = false;
                w->redraw = true;
                break;
            }
            click(a, args[1].u);
        } else if (args[2].u == 272) {
            a->pane[0].sb_drag = a->pane[1].sb_drag = false;
        }
        w->redraw = true;
        break;
    case LM_WL_POINTER_EVT_AXIS:
        if (args[1].u == 0) {
            int lines = (int)(lm_fixed_to_double(args[2].f) / 15.0 * 3.0);
            if (lines == 0)
                lines = args[2].f > 0 ? 1 : -1;
            int idx = pane_at(a, (int)a->px);
            if (pane_is_preview(a, idx))
                scroll_preview(a, lines);
            else {
                struct pane *p = &a->pane[idx];
                p->top = LM_CLAMP(p->top + lines, 0, LM_MAX(rows(p) - a->rows_visible, 0));
            }
            w->redraw = true;
        }
        break;
    }
}

static void menu_event(struct wlwin *w, int index)
{
    struct app *a = w->data;
    int kind = a->menu_kind;
    a->menu_kind = 0;
    if (index < 0)
        return;
    if (kind == 1) {                                                    /* a file: the same as its keys */
        static const int fkey[8] = { -1, F_EDIT, F_RENAME, F_COPY, F_MOVE, F_DELETE, F_MKDIR, F_TERM };
        if (index == 0)
            open_selected(a);
        else if (index < 8)
            press_fkey(a, fkey[index]);
    } else if (kind == 2) {
        if (index == 0)
            press_fkey(a, F_MKDIR);
        else if (index == 1)
            press_fkey(a, F_TERM);
        else
            reread(a);
    } else if (a->prompt != P_NONE && a->prompt != P_DELETE && a->prompt != P_DELETE_HARD && a->prompt != P_EMPTY) {
        wltext_menu_do(&a->clip, a->pbuf, sizeof(a->pbuf), index);
    }
    w->redraw = true;
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

static void write_file(const char *path, const char *content, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(content, 1, n, f);
        fclose(f);
    }
}

static bool file_is(const char *path, const char *content)
{
    char buf[256];
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return !strcmp(buf, content);
}

static int selftest(void)
{
    char out[PATH_MAX];
    {
        struct netpath np;
        CHECK(net_parse("smb://nas/data/a/b", &np) && np.level == 2 && !strcmp(np.conn, "nas") && !strcmp(np.share, "data") && !strcmp(np.dir, "a/b"), "net_parse");
        CHECK(net_parse("smb://", &np) && np.level == 0 && net_parse("smb://nas", &np) && np.level == 1 && net_parse("smb://nas/data/", &np) && np.level == 2 && !np.dir[0], "net_parse levels");
        net_parent("smb://nas/data/a/b", out, sizeof(out));
        CHECK(!strcmp(out, "smb://nas/data/a"), "net_parent: %s", out);
        net_parent("smb://nas/data/a", out, sizeof(out));
        net_parent(out, out, sizeof(out));
        CHECK(!strcmp(out, "smb://nas"), "net_parent to the server: %s", out);
        net_parent("smb://nas", out, sizeof(out));
        CHECK(!strcmp(out, "smb://"), "net_parent to the list: %s", out);
        join_path("smb://", "nas", out, sizeof(out));
        CHECK(!strcmp(out, "smb://nas"), "join in the list: %s", out);
        join_path("smb://nas/data", "x y", out, sizeof(out));
        CHECK(!strcmp(out, "smb://nas/data/x y"), "join in a share: %s", out);
        join_path("smb://nas/data/x", "..", out, sizeof(out));
        CHECK(!strcmp(out, "smb://nas/data"), "join ..: %s", out);
        CHECK(net_parse("sftp://nas", &np) && np.sftp && np.level == 1 && !strcmp(np.conn, "nas"), "sftp level 1");
        CHECK(net_parse("sftp://nas/", &np) && np.sftp && np.level == 2 && !np.dir[0], "sftp root");
        CHECK(net_parse("sftp://nas:2222/home/me/x/", &np) && np.level == 2 && !strcmp(np.conn, "nas:2222") && !strcmp(np.dir, "home/me/x"), "sftp path");
        net_parent("sftp://nas/home/me", out, sizeof(out));
        CHECK(!strcmp(out, "sftp://nas/home"), "sftp parent: %s", out);
        net_parent("sftp://nas/home", out, sizeof(out));
        CHECK(!strcmp(out, "sftp://nas/"), "sftp parent to the root: %s", out);
        net_parent("sftp://nas/", out, sizeof(out));
        CHECK(!strcmp(out, "smb://"), "sftp root to the list: %s", out);
        join_path("sftp://nas/", "etc", out, sizeof(out));
        CHECK(!strcmp(out, "sftp://nas/etc"), "sftp join in the root: %s", out);
        join_path("sftp://nas/etc", "ssh", out, sizeof(out));
        CHECK(!strcmp(out, "sftp://nas/etc/ssh"), "sftp join: %s", out);
        join_path("smb://", "nas" SFTP_TAG, out, sizeof(out));
        CHECK(!strcmp(out, "sftp://nas"), "the list's tagged name opens the SSH server: %s", out);
        join_path("smb://nas/data", "/home", out, sizeof(out));
        CHECK(!strcmp(out, "/home"), "an absolute local path leaves the network: %s", out);
        CHECK(smb_selftest() == 0, "smb selftest");
        CHECK(sftp_selftest() == 0, "sftp selftest");
        char tmpf[] = "/tmp/mellagui-files-prog-XXXXXX";                     /* the progress of a download is the size of the file that grows */
        int tfd = mkstemp(tmpf);
        if (tfd >= 0) {
            struct job pj = { 0 };
            struct net_prog pr = { .upload = false };
            snprintf(pr.path, sizeof(pr.path), "%s", tmpf);
            tl_job = &pj;
            CHECK(write(tfd, "x", 1) == 1 && net_poll_progress(&pr) && atomic_load(&pj.done) == 1, "progress of 1 byte: %lld", (long long)atomic_load(&pj.done));
            char blk[999];
            memset(blk, 'y', sizeof(blk));
            CHECK(write(tfd, blk, sizeof(blk)) == (ssize_t)sizeof(blk) && net_poll_progress(&pr) && atomic_load(&pj.done) == 1000, "progress adds only what is new");
            CHECK(net_poll_progress(&pr) && atomic_load(&pj.done) == 1000, "no growth, no change");
            atomic_store(&pj.cancel, true);
            CHECK(!net_poll_progress(&pr), "a cancelled job stops the transfer");
            tl_job = NULL;
            close(tfd);
            unlink(tmpf);
        }
    }
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
    CHECK(path_inside("/a/b", "/a") && path_inside("/a", "/a") && !path_inside("/ab", "/a") && path_inside("/x", "/"), "path_inside");

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
    static struct pane lp;
    char err[200];
    CHECK(list_dir(&lp, dir, false, err, sizeof(err)) == 0, "list: %s", err);
    const char *want[] = { "Alpha", "zeta", "Apple.txt", "beta.txt", "bin.dat", "dangling", "pic.PNG" };
    CHECK(lp.n == 7, "%d entries without hidden files", lp.n);
    for (int i = 0; i < lp.n && i < 7; i++)
        CHECK(!strcmp(lp.e[i].name, want[i]), "entry %d is %s, want %s", i, lp.e[i].name, want[i]);
    CHECK(lp.n >= 6 && lp.e[5].broken && !lp.e[5].dir, "a dangling link is marked");
    CHECK(lp.n >= 2 && lp.e[0].dir && lp.e[1].dir && !lp.e[2].dir, "folders first");
    CHECK(list_dir(&lp, dir, true, err, sizeof(err)) == 0 && lp.n == 8 && !strcmp(lp.e[2].name, ".secret"), "hidden files listed and sorted");
    CHECK(list_dir(&lp, "/nonexistent-dir", true, err, sizeof(err)) < 0 && lp.n == 8, "a failed listing keeps the old one");

    snprintf(p, sizeof(p), "%s/beta.txt", dir);
    CHECK(classify(p) == K_TEXT, "text");
    snprintf(p, sizeof(p), "%s/bin.dat", dir);
    CHECK(classify(p) == K_BINARY, "binary");
    snprintf(p, sizeof(p), "%s/pic.PNG", dir);
    CHECK(classify(p) == K_IMAGE, "image by extension");
    snprintf(p, sizeof(p), "%s/nope", dir);
    CHECK(classify(p) == K_UNREADABLE, "unreadable");
    touch(dir, "clip.MP4", "\0\0\0\x10", 4);
    snprintf(p, sizeof(p), "%s/clip.MP4", dir);
    CHECK(classify(p) == K_VIDEO, "video by extension");
    touch(dir, "prog", "\x7f" "ELF\0\1\2\3", 8);
    snprintf(p, sizeof(p), "%s/prog", dir);
    CHECK(classify(p) == K_BINARY, "an ELF file that cannot be executed is a binary file");
    chmod(p, 0755);
    CHECK(classify(p) == K_PROGRAM, "an executable ELF file is a program");
    touch(dir, "script.sh", "#!/bin/sh\necho hi\n", 18);
    snprintf(p, sizeof(p), "%s/script.sh", dir);
    chmod(p, 0755);
    CHECK(classify(p) == K_TEXT, "an executable script is still text");
    entries_free(&lp);
    free(lp.e);

    /* ---- the preview ---- */
    static struct preview pv;
    snprintf(p, sizeof(p), "%s/beta.txt", dir);
    preview_load(&pv, p, false);
    CHECK(pv.kind == PV_TEXT && pv.nlines == 1 && !strcmp(pv.lines[0], "hello"), "text preview: kind %d, %d lines", pv.kind, pv.nlines);
    touch(dir, "tabs.txt", "a\tb\r\nsecond\n\nlast", 17);
    snprintf(p, sizeof(p), "%s/tabs.txt", dir);
    preview_load(&pv, p, false);
    CHECK(pv.kind == PV_TEXT && pv.nlines == 4 && !strcmp(pv.lines[0], "a   b") && !strcmp(pv.lines[2], "") && !strcmp(pv.lines[3], "last"),
          "tabs and line ends: %d lines, first '%s'", pv.nlines, pv.nlines ? pv.lines[0] : "");
    snprintf(p, sizeof(p), "%s/bin.dat", dir);
    preview_load(&pv, p, false);
    CHECK(pv.kind == PV_HEX && pv.nlines == 1 && strstr(pv.lines[0], "61 00 62") && strstr(pv.lines[0], "|a.b"), "hex dump: '%s'", pv.nlines ? pv.lines[0] : "");
    preview_load(&pv, dir, false);
    CHECK(pv.kind == PV_DIR && pv.nlines >= 7 && !strcmp(pv.lines[0], "Alpha/") && strstr(pv.info, "items"), "folder preview: %d lines '%s'", pv.nlines, pv.info);
    snprintf(p, sizeof(p), "%s/nope", dir);
    preview_load(&pv, p, false);
    CHECK(pv.kind == PV_ERROR, "a missing file is an error");
    touch(dir, "one.ppm", "P6\n2 1\n255\n\xff\x00\x00\x00\x00\xff", 17);
    snprintf(p, sizeof(p), "%s/one.ppm", dir);
    preview_load(&pv, p, false);
    CHECK(pv.kind == PV_IMAGE && pv.img.w == 2 && pv.img.h == 1, "picture preview: kind %d", pv.kind);
    preview_scale(&pv, 100, 100);
    CHECK(pv.scaled && pv.sw == 16 && pv.sh == 8, "scaled up at most 8 times: %dx%d", pv.sw, pv.sh);
    CHECK(pv.scaled && pv.scaled[0] == 0xff0000 && pv.scaled[15] == 0x0000ff, "scaled colours %06x %06x", pv.scaled ? pv.scaled[0] : 0, pv.scaled ? pv.scaled[15] : 0);
    preview_clear(&pv);

    /* ---- copy, move, delete (the app functions, without a window) ---- */
    static struct app a;
    a.dual = true;
    a.sync_jobs = true;                                                   /* the work is done in this thread */
    a.hover = -1;
    char src[PATH_MAX], dst[PATH_MAX];
    snprintf(src, sizeof(src), "%s/left", dir);
    snprintf(dst, sizeof(dst), "%s/right", dir);
    mkdir(src, 0755);
    mkdir(dst, 0755);
    snprintf(p, sizeof(p), "%s/tree", src);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/tree/deep", src);
    mkdir(p, 0755);
    touch(src, "tree/deep/f.txt", "deep\n", 5);
    touch(src, "one.txt", "one\n", 4);
    touch(src, "two.txt", "two\n", 4);
    snprintf(p, sizeof(p), "%s/link", src);
    if (symlink("one.txt", p) < 0) {}
    CHECK(go_to(&a, &a.pane[0], src, NULL) && go_to(&a, &a.pane[1], dst, NULL), "both panes list");
    CHECK(a.pane[0].n == 4, "left has %d entries", a.pane[0].n);
    a.act = 0;
    int r_one = -1;
    for (int r = 0; r < rows(&a.pane[0]); r++)
        if (!strcmp(row_name(&a.pane[0], r), "one.txt"))
            r_one = r;
    a.pane[0].sel = r_one;
    do_copy_move(&a, dst, false);                                         /* copy one file into the other folder */
    snprintf(p, sizeof(p), "%s/one.txt", dst);
    CHECK(file_is(p, "one\n"), "copied: %s", a.msg);
    CHECK(a.pane[1].n == 1, "the right pane was reread: %d", a.pane[1].n);
    write_file(p, "changed", 7);
    do_copy_move(&a, dst, false);                                         /* the same copy again: nothing is replaced */
    CHECK(file_is(p, "changed") && strstr(a.msg, "not replaced"), "an existing file stays: %s", a.msg);
    struct entry *e;
    for (int i = 0; i < a.pane[0].n; i++) {                              /* mark the tree and the link: copy both */
        e = &a.pane[0].e[i];
        e->marked = !strcmp(e->name, "tree") || !strcmp(e->name, "link");
    }
    do_copy_move(&a, dst, false);
    snprintf(p, sizeof(p), "%s/tree/deep/f.txt", dst);
    CHECK(file_is(p, "deep\n"), "a folder is copied with everything in it: %s", a.msg);
    snprintf(p, sizeof(p), "%s/link", dst);
    char tgt[64] = "";
    ssize_t tl = readlink(p, tgt, sizeof(tgt) - 1);
    CHECK(tl > 0 && !strcmp(tgt, "one.txt"), "a link is copied as a link");
    CHECK(count_marked(&a.pane[0]) == 0, "the marks are cleared after the operation");
    char into_self[PATH_MAX + 32];
    snprintf(into_self, sizeof(into_self), "%s/tree/deep", src);
    for (int i = 0; i < a.pane[0].n; i++)
        a.pane[0].e[i].marked = !strcmp(a.pane[0].e[i].name, "tree");
    do_copy_move(&a, into_self, false);                                   /* a folder into itself is refused */
    CHECK(strstr(a.msg, "into itself"), "into itself: %s", a.msg);

    for (int i = 0; i < a.pane[0].n; i++)                                 /* move two.txt under a new name */
        a.pane[0].e[i].marked = false;
    for (int r = 0; r < rows(&a.pane[0]); r++)
        if (!strcmp(row_name(&a.pane[0], r), "two.txt"))
            a.pane[0].sel = r;
    snprintf(p, sizeof(p), "%s/renamed.txt", dst);
    do_copy_move(&a, p, true);
    snprintf(src, sizeof(src), "%s/left/two.txt", dir);
    CHECK(file_is(p, "two\n") && access(src, F_OK) != 0, "moved under a new name: %s", a.msg);
    for (int r = 0; r < rows(&a.pane[0]); r++)                            /* several things need a folder */
        a.pane[0].e[r > 0 ? r - 1 : 0].marked = true;
    snprintf(p, sizeof(p), "%s/renamed.txt", dst);
    do_copy_move(&a, p, false);
    CHECK(strstr(a.msg, "not a folder"), "several files to a file name: %s", a.msg);
    for (int i = 0; i < a.pane[0].n; i++)
        a.pane[0].e[i].marked = false;

    a.act = 1;                                                            /* delete in the right pane: the tree and a file */
    for (int i = 0; i < a.pane[1].n; i++)
        a.pane[1].e[i].marked = !strcmp(a.pane[1].e[i].name, "tree") || !strcmp(a.pane[1].e[i].name, "one.txt");
    do_delete(&a, true);
    snprintf(p, sizeof(p), "%s/right/tree", dir);
    char p2[PATH_MAX + 64];
    snprintf(p2, sizeof(p2), "%s/right/one.txt", dir);
    CHECK(access(p, F_OK) != 0 && access(p2, F_OK) != 0 && strstr(a.msg, "Deleted 2"), "deleted: %s", a.msg);
    snprintf(p, sizeof(p), "%s/left/tree/deep/f.txt", dir);
    CHECK(access(p, F_OK) == 0, "the source of the copy is untouched");

    a.act = 1;                                                            /* rename and mkdir */
    for (int r = 0; r < rows(&a.pane[1]); r++)
        if (!strcmp(row_name(&a.pane[1], r), "renamed.txt"))
            a.pane[1].sel = r;
    do_rename(&a, "final.txt");
    snprintf(p, sizeof(p), "%s/right/final.txt", dir);
    CHECK(file_is(p, "two\n"), "renamed: %s", a.msg);
    do_rename(&a, "a/b");
    CHECK(strstr(a.msg, "no slash"), "a name with a slash: %s", a.msg);
    do_mkdir(&a, "x/y/z");
    snprintf(p, sizeof(p), "%s/right/x/y/z", dir);
    CHECK(access(p, F_OK) == 0, "mkdir makes the folders in between: %s", a.msg);
    do_mkdir(&a, "x");
    CHECK(strstr(a.msg, "already exists"), "mkdir on an existing name: %s", a.msg);
    CHECK(remove_tree("/", 0, err, sizeof(err)) < 0, "the root is never deleted");

    /* the trash: a file goes to XDG_DATA_HOME/Trash with an info file, a second one of the same name gets another name */
    char xdg[PATH_MAX];
    snprintf(xdg, sizeof(xdg), "%s/data", dir);
    setenv("XDG_DATA_HOME", xdg, 1);
    a.act = 1;
    CHECK(go_to(&a, &a.pane[1], dst, NULL), "right pane again");
    for (int i = 0; i < a.pane[1].n; i++)
        a.pane[1].e[i].marked = false;
    for (int r = 0; r < rows(&a.pane[1]); r++)
        if (!strcmp(row_name(&a.pane[1], r), "final.txt"))
            a.pane[1].sel = r;
    do_delete(&a, false);
    snprintf(p, sizeof(p), "%s/Trash/files/final.txt", xdg);
    snprintf(p2, sizeof(p2), "%s/Trash/info/final.txt.trashinfo", xdg);
    char info[512] = "";
    FILE *fi = fopen(p2, "r");
    if (fi) {
        size_t k = fread(info, 1, sizeof(info) - 1, fi);
        info[k] = 0;
        fclose(fi);
    }
    snprintf(src, sizeof(src), "Path=%s/right/final.txt", dir);
    CHECK(file_is(p, "two\n") && strstr(info, "[Trash Info]") && strstr(info, src) && strstr(info, "DeletionDate=") && strstr(a.msg, "trash"),
          "trashed: %s / %s", a.msg, info);
    snprintf(p, sizeof(p), "%s/right/final.txt", dir);
    CHECK(access(p, F_OK) != 0, "and gone from where it was");
    write_file(p, "again", 5);                                            /* a second file of that name */
    CHECK(go_to(&a, &a.pane[1], dst, NULL), "reread");
    for (int r = 0; r < rows(&a.pane[1]); r++)
        if (!strcmp(row_name(&a.pane[1], r), "final.txt"))
            a.pane[1].sel = r;
    do_delete(&a, false);
    snprintf(p, sizeof(p), "%s/Trash/files/final.txt.2", xdg);
    CHECK(file_is(p, "again"), "a taken name in the trash gets a new one");
    CHECK(in_trash(p) && !in_trash("/tmp/elsewhere"), "in_trash");
    CHECK(go_to(&a, &a.pane[0], p2, NULL) == false || true, "(no-op)");
    snprintf(p, sizeof(p), "%s/Trash/files", xdg);                         /* in the trash a delete is for good */
    CHECK(go_to(&a, &a.pane[1], p, NULL), "list the trash");
    a.act = 1;
    for (int i = 0; i < a.pane[1].n; i++)
        a.pane[1].e[i].marked = !strcmp(a.pane[1].e[i].name, "final.txt.2");
    do_delete(&a, false);
    snprintf(p, sizeof(p), "%s/Trash/files/final.txt.2", xdg);
    CHECK(access(p, F_OK) != 0 && strstr(a.msg, "Deleted 1"), "deleting inside the trash is for good: %s", a.msg);

    /* restoring: back to where it was deleted from, the info file goes, nothing is replaced */
    snprintf(p, sizeof(p), "%s/Trash/files", xdg);
    CHECK(go_to(&a, &a.pane[1], p, NULL), "list the trash again");
    a.act = 1;
    for (int i = 0; i < a.pane[1].n; i++)
        a.pane[1].e[i].marked = false;
    CHECK(a.pane[1].n == 1 && !strcmp(a.pane[1].e[0].name, "final.txt"), "the trash holds final.txt (%d entries)", a.pane[1].n);
    char origin[PATH_MAX], odate[40];
    CHECK(trash_origin("final.txt", origin, sizeof(origin), odate, sizeof(odate)) && strstr(origin, "/right/final.txt") && strlen(odate) == 19,
          "origin '%s' date '%s'", origin, odate);
    CHECK(in_trash_files(p) && !in_trash_files(dir), "in_trash_files");
    a.pane[1].sel = 1;                                                    /* (row 0 is "..") */
    do_restore(&a);
    snprintf(p, sizeof(p), "%s/right/final.txt", dir);
    snprintf(p2, sizeof(p2), "%s/Trash/info/final.txt.trashinfo", xdg);
    CHECK(file_is(p, "two\n") && access(p2, F_OK) != 0 && strstr(a.msg, "Put back 1"), "restored: %s", a.msg);
    snprintf(p, sizeof(p), "%s/Trash/files/final.txt", xdg);
    CHECK(access(p, F_OK) != 0, "and gone from the trash");
    snprintf(p, sizeof(p), "%s/right/clash.txt", dir);                     /* the old place is taken again: not replaced */
    write_file(p, "one", 3);
    CHECK(go_to(&a, &a.pane[1], dst, NULL), "right pane");
    for (int r = 0; r < rows(&a.pane[1]); r++)
        if (!strcmp(row_name(&a.pane[1], r), "clash.txt"))
            a.pane[1].sel = r;
    do_delete(&a, false);
    write_file(p, "two", 3);
    snprintf(p, sizeof(p), "%s/Trash/files", xdg);
    CHECK(go_to(&a, &a.pane[1], p, NULL), "trash");
    a.pane[1].sel = 1;
    do_restore(&a);
    snprintf(p, sizeof(p), "%s/right/clash.txt", dir);
    snprintf(p2, sizeof(p2), "%s/Trash/files/clash.txt", xdg);
    CHECK(file_is(p, "two") && access(p2, F_OK) == 0 && strstr(a.msg, "not replaced"), "an existing file stays: %s", a.msg);
    snprintf(p, sizeof(p), "%s/Trash/info/clash.txt.trashinfo", xdg);       /* no record: a clear message, nothing happens */
    unlink(p);
    CHECK(go_to(&a, &a.pane[1], xdg, NULL), "elsewhere");
    do_restore(&a);
    CHECK(strstr(a.msg, "Open the trash first"), "outside the trash: %s", a.msg);
    snprintf(p, sizeof(p), "%s/Trash/files", xdg);
    CHECK(go_to(&a, &a.pane[1], p, NULL), "trash once more");
    a.pane[1].sel = 1;
    do_restore(&a);
    CHECK(strstr(a.msg, "no record") && access(p2, F_OK) == 0, "no info file: %s", a.msg);

    /* emptying the trash: files and their info files go, the folders stay */
    snprintf(p, sizeof(p), "%s/Trash/files", xdg);
    CHECK(count_dir(p) == 1, "the trash still holds clash.txt (%d)", count_dir(p));
    snprintf(src, sizeof(src), "%s/right/more.txt", dir);
    write_file(src, "m", 1);
    CHECK(go_to(&a, &a.pane[1], dst, NULL), "right pane");
    a.act = 1;
    for (int r = 0; r < rows(&a.pane[1]); r++)
        if (!strcmp(row_name(&a.pane[1], r), "more.txt"))
            a.pane[1].sel = r;
    do_delete(&a, false);                                                 /* now two things are in the trash */
    CHECK(count_dir(p) == 2, "two trashed files (%d)", count_dir(p));
    snprintf(p2, sizeof(p2), "%s/Trash/info", xdg);
    do_empty_trash(&a);
    CHECK(count_dir(p) == 0 && count_dir(p2) == 0 && strstr(a.msg, "Emptied the trash (2 items)"), "emptied: %s", a.msg);
    CHECK(access(p, F_OK) == 0 && access(p2, F_OK) == 0, "the folders stay");
    do_empty_trash(&a);
    CHECK(strstr(a.msg, "already empty"), "nothing to empty: %s", a.msg);
    cmd_empty_trash(&a);
    CHECK(a.prompt == P_NONE && strstr(a.msg, "already empty"), "no question for an empty trash");

    /* a cancelled job stops at once and leaves no half-written file behind */
    static struct job *cj;
    cj = job_new(J_COPY, 1);
    snprintf(cj->items[0].src, sizeof(cj->items[0].src), "%s/left/one.txt", dir);
    snprintf(cj->items[0].dst, sizeof(cj->items[0].dst), "%s/cancelled.txt", dir);
    cj->n = 1;
    cj->requested = 1;
    atomic_store(&cj->cancel, true);
    job_execute(cj);                                                      /* cancelled before it began: nothing is done */
    snprintf(p, sizeof(p), "%s/cancelled.txt", dir);
    CHECK(access(p, F_OK) != 0 && cj->ok == 0, "a job cancelled at the start does nothing");
    tl_job = cj;                                                          /* cancelled in the middle of a file: no half file */
    CHECK(copy_node(cj->items[0].src, cj->items[0].dst, 0, err, sizeof(err)) < 0 && !strcmp(err, "cancelled") && access(p, F_OK) != 0,
          "a cancelled copy leaves nothing: '%s'", err);
    tl_job = NULL;
    job_free(cj);

    entries_free(&a.pane[0]);
    entries_free(&a.pane[1]);
    free(a.pane[0].e);
    free(a.pane[1].e);
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) {}
    puts(bad ? "selftest FAILED" : "selftest ok");
    return bad ? 1 : 0;
}


/* ---- main ----------------------------------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    lm_font_ui_cells(true);             /* this program lays its text out in columns of characters */
    if (getenv("MELLAGUI_ASKPASS"))                                     /* ssh asks for a password through SSH_ASKPASS, which names this program (sftp.h) */
        return sftp_askpass_main();
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();
    const char *choose = NULL;
    if (argc >= 3 && !strcmp(argv[1], "--choose")) {
        if (strcmp(argv[2], "wallpaper")) {                             /* the only setting a file may be chosen for */
            fprintf(stderr, "mellagui-files: --choose knows only 'wallpaper'\n");
            return 2;
        }
        choose = argv[2];
        argv += 2;
        argc -= 2;
    }
    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        fprintf(stderr, "usage: mellagui-files [DIR | --selftest | --choose wallpaper [DIR]]\n");
        return 2;
    }
    signal(SIGCHLD, SIG_IGN);                                           /* the programs we start are nobody's business afterwards */
    static struct app a;
    a.rep_rate = 25;
    a.rep_delay = 400;
    a.pane[0].last_click_row = a.pane[1].last_click_row = -1;
    a.hover = -1;
    a.dual = choose == NULL;
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event, .menu = menu_event };
    a.choose_key = choose;
    if (wlwin_connect(&a.win, "mellagui-files", &ops, &a) < 0)
        return 1;
    if (wlwin_open(&a.win, choose ? "Choose a picture" : "Files", choose ? "mellagui.files.choose" : "mellagui.files",
                   choose ? 640 : 880, choose ? 460 : 460, choose ? 360 : 640, choose ? 160 : 240) < 0)
        return 1;
    wlclip_attach(&a.clip, &a.win);
    char start[PATH_MAX], cwd[PATH_MAX];
    const char *home = getenv("HOME");
    if (argc == 2) {
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), "/");
        join_path(cwd, argv[1], start, sizeof(start));
    } else if (choose && home && *home) {                               /* pictures usually live in ~/Pictures */
        char pics[PATH_MAX];
        snprintf(pics, sizeof(pics), "%s/Pictures", home);
        struct stat st;
        normalize(stat(pics, &st) == 0 && S_ISDIR(st.st_mode) ? pics : home, start, sizeof(start));
    } else {
        normalize(home && *home ? home : "/", start, sizeof(start));
    }
    if (!go_to(&a, &a.pane[0], start, NULL) && !go_to(&a, &a.pane[0], "/", NULL)) {
        fprintf(stderr, "mellagui-files: cannot list %s\n", start);
        return 1;
    }
    if (a.dual && !go_to(&a, &a.pane[1], a.pane[0].cwd, NULL))
        return 1;
    if (strcmp(a.pane[0].cwd, start) != 0)
        msg(&a, "Could not open %s", start);
    else if (choose)
        msg(&a, "Choose a picture: Enter or double click (Esc cancels)");
    else
        msg(&a, "F1 shows the keys: Tab switches panes, F3 previews, F5 copies, F6 moves, F8 deletes");
    while (a.win.running) {
        int timeout = 250;
        if (a.rep_code) {
            int64_t wait = a.rep_next - now_ms();
            timeout = (int)LM_CLAMP(wait, 1, 250);
        }
        if (a.job)                                                      /* a copy / move / delete: the progress bar moves */
            timeout = LM_MIN(timeout, 80);
        if (a.preview && a.pv.kind == PV_VIDEO_WAIT)                    /* a video's picture is on its way */
            timeout = LM_MIN(timeout, 80);
        if (a.preview && !a.pv.loaded && a.pv.want[0]) {                /* a picture waits for the cursor to rest */
            int64_t wait = a.pv.due - now_ms();
            timeout = (int)LM_CLAMP(wait, 1, timeout);
            a.win.redraw = a.win.redraw || wait <= 0;
        }
        if (!wlwin_step(&a.win, timeout))
            break;
        if (a.preview && !a.pv.loaded && a.pv.want[0] && now_ms() >= a.pv.due)
            a.win.redraw = true;
        if (a.preview && a.pv.kind == PV_VIDEO_WAIT)                    /* (preview_update collects the picture when it is painted) */
            a.win.redraw = true;
        if (a.job) {
            a.win.redraw = true;
            if (atomic_load(&a.job->finished))
                job_finish(&a);
        }
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
    if (a.job) {                                                        /* the window is going: stop the work and wait for it */
        atomic_store(&a.job->cancel, true);
        if (a.job->threaded)
            pthread_join(a.job->thread, NULL);
    }
    wlwin_destroy(&a.win);
    return 0;
}
