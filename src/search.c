/*
 * The launcher's search: a calculator, the installed programs and the files of the home folder. See search.h.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "dockicon.h"
#include "launchers.h"
#include "log.h"
#include "loop.h"
#include "search.h"
#include "util.h"

/* ---- the calculator -------------------------------------------------------------------------------------------------------- */

struct calc {
    const char *p;
    bool ok;
    bool fancy;                         /* an operator, a function or a bracket was used: it is a calculation and not a number or a word */
};

static void calc_ws(struct calc *c)
{
    while (*c->p == ' ' || *c->p == '\t')
        c->p++;
}

static double calc_expr(struct calc *c, int depth);

static double calc_primary(struct calc *c, int depth)
{
    calc_ws(c);
    if (depth > 40) {
        c->ok = false;
        return 0.0;
    }
    if (*c->p == '(') {
        c->p++;
        c->fancy = true;
        double v = calc_expr(c, depth + 1);
        calc_ws(c);
        if (*c->p != ')') {
            c->ok = false;
            return 0.0;
        }
        c->p++;
        return v;
    }
    if (isdigit((unsigned char)*c->p) || ((*c->p == '.' || *c->p == ',') && isdigit((unsigned char)c->p[1]))) {
        char buf[64];
        size_t n = 0;
        while ((isdigit((unsigned char)*c->p) || *c->p == '.' || *c->p == ',') && n < sizeof(buf) - 1)
            buf[n++] = *c->p == ',' ? '.' : *c->p, c->p++;
        buf[n] = 0;
        char *end;
        double v = strtod(buf, &end);
        if (*end) {
            c->ok = false;
            return 0.0;
        }
        return v;
    }
    if (isalpha((unsigned char)*c->p)) {
        char name[16];
        size_t n = 0;
        while (isalpha((unsigned char)*c->p) && n < sizeof(name) - 1)
            name[n++] = (char)tolower((unsigned char)*c->p++);
        name[n] = 0;
        if (!strcmp(name, "pi")) {
            return M_PI;
        }
        if (!strcmp(name, "e") && *c->p != '(')
            return M_E;
        calc_ws(c);
        if (*c->p != '(') {
            c->ok = false;
            return 0.0;
        }
        c->p++;
        double v = calc_expr(c, depth + 1);
        calc_ws(c);
        if (*c->p != ')') {
            c->ok = false;
            return 0.0;
        }
        c->p++;
        c->fancy = true;
        static const struct { const char *name; double (*fn)(double); } fns[] = {
            { "sqrt", sqrt }, { "sin", sin }, { "cos", cos }, { "tan", tan }, { "ln", log }, { "log", log10 }, { "exp", exp }, { "abs", fabs },
            { "floor", floor }, { "ceil", ceil }, { "round", round }, { "asin", asin }, { "acos", acos }, { "atan", atan },
        };
        for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++)
            if (!strcmp(name, fns[i].name))
                return fns[i].fn(v);
        c->ok = false;
        return 0.0;
    }
    c->ok = false;
    return 0.0;
}

static double calc_unary(struct calc *c, int depth)
{
    calc_ws(c);
    if (*c->p == '-') {
        c->p++;
        return -calc_unary(c, depth + 1);
    }
    if (*c->p == '+') {
        c->p++;
        return calc_unary(c, depth + 1);
    }
    double v = calc_primary(c, depth);
    calc_ws(c);
    if (*c->p == '^') {                                         /* right associative */
        c->p++;
        c->fancy = true;
        return pow(v, calc_unary(c, depth + 1));
    }
    return v;
}

static double calc_term(struct calc *c, int depth)
{
    double v = calc_unary(c, depth);
    for (;;) {
        calc_ws(c);
        char op = *c->p;
        if (op == '*' || op == '/' || op == '%' || (unsigned char)op == 0xc3) {          /* (0xc3: the UTF-8 start of the multiplication and division signs) */
            if ((unsigned char)op == 0xc3) {
                if ((unsigned char)c->p[1] == 0x97) { op = '*'; c->p += 2; }
                else if ((unsigned char)c->p[1] == 0xb7) { op = '/'; c->p += 2; }
                else break;
            } else {
                c->p++;
            }
            c->fancy = true;
            double r = calc_unary(c, depth);
            if (op == '*')
                v *= r;
            else if (op == '/')
                v = r == 0.0 ? NAN : v / r;
            else
                v = r == 0.0 ? NAN : fmod(v, r);
        } else {
            break;
        }
    }
    return v;
}

static double calc_expr(struct calc *c, int depth)
{
    double v = calc_term(c, depth);
    for (;;) {
        calc_ws(c);
        if (*c->p == '+' || *c->p == '-') {
            char op = *c->p++;
            c->fancy = true;
            double r = calc_term(c, depth);
            v = op == '+' ? v + r : v - r;
        } else {
            break;
        }
    }
    return v;
}

bool lm_calc_eval(const char *text, double *out)
{
    struct calc c = { .p = text, .ok = true };
    if (!text || !*text || strlen(text) > 200)
        return false;
    double v = calc_expr(&c, 0);
    calc_ws(&c);
    if (!c.ok || *c.p || !c.fancy || !isfinite(v))
        return false;
    *out = v;
    return true;
}

void lm_calc_format(double v, char *out, size_t n)
{
    if (fabs(v) < 1e-12)
        v = 0.0;
    snprintf(out, n, "%.10g", v);
}

/* ---- matching -------------------------------------------------------------------------------------------------------------- */

/* A word of the query found in `hay`: 100 at the start, 80 at the start of a word, 50 inside (a little less the later it is); 0 not found. */
static int word_score(const char *hay, const char *word)
{
    const char *p = strcasestr(hay, word);
    if (!p)
        return 0;
    int sc = p == hay ? 100 : !isalnum((unsigned char)p[-1]) ? 80 : 50;
    int late = (int)(p - hay) / 4;
    return sc - (late > 20 ? 20 : late);
}

struct query {
    char word[4][48];
    int n;
};

static void query_split(const char *q, struct query *out)
{
    out->n = 0;
    while (*q && out->n < 4) {
        while (*q == ' ')
            q++;
        size_t l = 0;
        while (q[l] && q[l] != ' ')
            l++;
        if (!l)
            break;
        snprintf(out->word[out->n], sizeof(out->word[0]), "%.*s", (int)l, q);
        out->n++;
        q += l;
    }
}

/* All the words are in `hay`: the mean of their scores, else 0. */
static int query_score(const struct query *q, const char *hay)
{
    int total = 0;
    for (int i = 0; i < q->n; i++) {
        int s = word_score(hay, q->word[i]);
        if (!s)
            return 0;
        total += s;
    }
    return q->n ? total / q->n : 0;
}

/* ---- the programs ---------------------------------------------------------------------------------------------------------- */

struct app {
    char name[64], generic[64], keywords[160], exec[512], icon[200], id[96];
    bool terminal;
    bool icon_done;                     /* the icon was looked for */
    struct lm_dock_item pub;            /* kind / img / plate / round of the icon */
    struct lm_image img;
};

#define APPS_MAX 1200
static struct app *g_apps;
static int g_napps;
static bool g_apps_read;

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = 0;
}

/* The Exec line without its field codes (%f %U ...). */
static void clean_exec(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n; p++) {
        if (*p == '%' && p[1]) {
            if (p[1] == '%')
                out[o++] = '%';
            p++;
            continue;
        }
        out[o++] = *p;
    }
    while (o && out[o - 1] == ' ')
        o--;
    out[o] = 0;
    for (char *p = out; *p; ) {                                         /* "%f" left an empty pair of quotes: the program would get an empty argument */
        if ((p[0] == '"' && p[1] == '"') || (p[0] == '\'' && p[1] == '\'')) {
            memmove(p, p + 2, strlen(p + 2) + 1);
            continue;
        }
        p++;
    }
    size_t l = strlen(out);
    while (l && out[l - 1] == ' ')
        out[--l] = 0;
}

static void read_desktop(const char *path, const char *id)
{
    if (g_napps >= APPS_MAX)
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    struct app *a = &g_apps[g_napps];
    memset(a, 0, sizeof(*a));
    snprintf(a->id, sizeof(a->id), "%s", id);
    bool in_entry = false, hidden = false, is_app = false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == '[') {
            if (in_entry)
                break;                                                  /* the next group: actions and the like are not entries */
            in_entry = !strcmp(line, "[Desktop Entry]");
            continue;
        }
        if (!in_entry || line[0] == '#')
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "Name")) snprintf(a->name, sizeof(a->name), "%s", v);
        else if (!strcmp(k, "GenericName")) snprintf(a->generic, sizeof(a->generic), "%s", v);
        else if (!strcmp(k, "Keywords")) snprintf(a->keywords, sizeof(a->keywords), "%s", v);
        else if (!strcmp(k, "Exec")) clean_exec(v, a->exec, sizeof(a->exec));
        else if (!strcmp(k, "Icon")) snprintf(a->icon, sizeof(a->icon), "%s", v);
        else if (!strcmp(k, "Terminal")) a->terminal = !strcmp(v, "true");
        else if (!strcmp(k, "Type")) is_app = !strcmp(v, "Application");
        else if ((!strcmp(k, "NoDisplay") || !strcmp(k, "Hidden")) && !strcmp(v, "true")) hidden = true;
        else if (!strcmp(k, "OnlyShowIn")) hidden = true;               /* meant for another desktop */
    }
    fclose(f);
    if (hidden || !is_app || !a->name[0] || !a->exec[0])
        return;
    for (int i = 0; i < g_napps; i++)                                   /* the first folder that has an id wins */
        if (!strcmp(g_apps[i].id, id))
            return;
    g_napps++;
}

static void read_apps_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 9 || strcmp(e->d_name + l - 8, ".desktop"))
            continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        read_desktop(path, e->d_name);
    }
    closedir(d);
}

static void read_apps(void)
{
    if (g_apps_read)
        return;
    g_apps_read = true;
    g_apps = calloc(APPS_MAX, sizeof(*g_apps));
    if (!g_apps)
        return;
    const char *over = getenv("MELLAGUI_APPS_DIRS");
    char list[4096];
    if (over) {
        snprintf(list, sizeof(list), "%s", over);
    } else {
        const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME"), *dirs = getenv("XDG_DATA_DIRS");
        char data[512] = "";
        if (xdg && *xdg)
            snprintf(data, sizeof(data), "%s", xdg);
        else if (home && *home)
            snprintf(data, sizeof(data), "%s/.local/share", home);
        snprintf(list, sizeof(list), "%s/applications:%s:/var/lib/snapd/desktop:/var/lib/flatpak/exports/share:%s/flatpak/exports/share", data,
                 dirs && *dirs ? dirs : "/usr/local/share:/usr/share", data);
        /* (every entry of the data dirs is a share folder: its applications/ is looked at below) */
    }
    char *save = NULL;
    for (char *dir = strtok_r(list, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char path[PATH_MAX];
        if (over || strstr(dir, "/applications")) {
            read_apps_dir(dir);
        } else {
            snprintf(path, sizeof(path), "%s/applications", dir);
            read_apps_dir(path);
        }
    }
    lm_info("search: %d programs", g_napps);
}

/* ---- the file index -------------------------------------------------------------------------------------------------------- */

struct fent {
    uint32_t off;                       /* the path in the pool */
    uint16_t base;                      /* where the file name starts in it */
    uint8_t dir;
};

#define FILES_MAX 150000
#define POOL_MAX (14u << 20)
#define MAX_DEPTH 6

struct index {
    struct fent *ents;
    size_t n;
    char *pool;
    size_t pool_n, pool_cap;
};

/* The index is made by a thread of its own (a dead network folder or a slow disk must not stop the compositor) and published, whole, when it is
 * ready; the old one is searched until then. */
static struct index g_idx;                      /* the published one, guarded by g_lock */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static bool g_thread_made;                      /* a thread was made and not joined yet */
static atomic_bool g_stop, g_scan_done;
static bool g_scan_started;
static time_t g_scan_finished;                  /* guarded by g_lock */
static char g_roots[2048];

static uint32_t pool_add(struct index *ix, const char *s, size_t len)
{
    if (ix->pool_n + len + 1 > POOL_MAX)
        return UINT32_MAX;
    if (ix->pool_n + len + 1 > ix->pool_cap) {
        size_t cap = ix->pool_cap ? ix->pool_cap * 2 : (1u << 20);
        while (cap < ix->pool_n + len + 1)
            cap *= 2;
        char *np = realloc(ix->pool, cap);
        if (!np)
            return UINT32_MAX;
        ix->pool = np;
        ix->pool_cap = cap;
    }
    uint32_t off = (uint32_t)ix->pool_n;
    memcpy(ix->pool + off, s, len);
    ix->pool[off + len] = 0;
    ix->pool_n += len + 1;
    return off;
}

static bool skip_name(const char *n)
{
    return n[0] == '.' || !strcmp(n, "node_modules") || !strcmp(n, "__pycache__") || !strcmp(n, "venv") || !strcmp(n, "snap");
}

struct qdir {
    uint32_t off;                       /* in the scan's own pool */
    uint8_t depth;
};

static void index_free(struct index *ix)
{
    free(ix->ents);
    free(ix->pool);
    memset(ix, 0, sizeof(*ix));
}

/* The thread: walks the roots breadth first (names only, links not followed), then publishes what it found. */
static void *scan_main(void *arg)
{
    (void)arg;
    struct index ix = { 0 };
    struct index paths = { 0 };                                         /* the folders still to read (a pool of their own) */
    struct qdir *queue = NULL;
    size_t q_n = 0, q_cap = 0, q_head = 0;
    char roots[sizeof(g_roots)];
    snprintf(roots, sizeof(roots), "%s", g_roots);
    char *save = NULL;
    for (char *r = strtok_r(roots, ":", &save); r; r = strtok_r(NULL, ":", &save)) {
        uint32_t off = pool_add(&paths, r, strlen(r));
        if (off == UINT32_MAX)
            break;
        if (q_n == q_cap) {
            q_cap = q_cap ? q_cap * 2 : 256;
            queue = realloc(queue, q_cap * sizeof(*queue));
            if (!queue)
                break;
        }
        queue[q_n++] = (struct qdir){ off, 0 };
    }
    size_t ents_cap = 0;
    while (queue && q_head < q_n && ix.n < FILES_MAX && !atomic_load(&g_stop)) {
        struct qdir qd = queue[q_head++];
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s", paths.pool + qd.off);
        DIR *d = opendir(dir);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d)) && ix.n < FILES_MAX && !atomic_load(&g_stop)) {
            if (skip_name(e->d_name) || !strcmp(e->d_name, ".."))
                continue;
            char path[PATH_MAX];
            int pl = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            if (pl <= 0 || pl >= (int)sizeof(path))
                continue;
            bool isdir = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN) {
                struct stat st;
                isdir = lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
            }
            if (e->d_type == DT_LNK)
                isdir = false;                                          /* a link is listed as a file and not followed */
            if (ix.n == ents_cap) {
                struct fent *ne = realloc(ix.ents, (ents_cap + 4096) * sizeof(*ne));
                if (!ne)
                    break;
                ix.ents = ne;
                ents_cap += 4096;
            }
            uint32_t off = pool_add(&ix, path, (size_t)pl);
            if (off == UINT32_MAX)
                break;
            ix.ents[ix.n++] = (struct fent){ off, (uint16_t)(pl - (int)strlen(e->d_name)), (uint8_t)isdir };
            if (isdir && qd.depth + 1 < MAX_DEPTH) {
                uint32_t qo = pool_add(&paths, path, (size_t)pl);
                if (qo == UINT32_MAX)
                    continue;
                if (q_n == q_cap) {
                    struct qdir *nq = realloc(queue, q_cap * 2 * sizeof(*nq));
                    if (!nq)
                        continue;
                    queue = nq;
                    q_cap *= 2;
                }
                queue[q_n++] = (struct qdir){ qo, (uint8_t)(qd.depth + 1) };
            }
        }
        closedir(d);
    }
    free(queue);
    index_free(&paths);
    if (atomic_load(&g_stop)) {
        index_free(&ix);
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    index_free(&g_idx);
    g_idx = ix;
    g_scan_finished = time(NULL);
    pthread_mutex_unlock(&g_lock);
    atomic_store(&g_scan_done, true);
    return NULL;
}

void lm_search_begin(struct lm_loop *loop)
{
    (void)loop;
    read_apps();
    bool old = false;
    if (g_scan_started && atomic_load(&g_scan_done)) {
        pthread_mutex_lock(&g_lock);
        old = time(NULL) - g_scan_finished > 600;
        pthread_mutex_unlock(&g_lock);
    }
    if (g_scan_started && !old)
        return;                                                        /* running, or recent enough */
    if (g_thread_made) {                                               /* the last scan is over: reap it */
        pthread_join(g_thread, NULL);
        g_thread_made = false;
    }
    const char *over = getenv("MELLAGUI_SEARCH_ROOT"), *home = getenv("HOME");
    snprintf(g_roots, sizeof(g_roots), "%s", over ? over : home && *home ? home : "");
    atomic_store(&g_stop, false);
    atomic_store(&g_scan_done, false);
    g_scan_started = true;
    sigset_t all, oldmask;                                                  /* (no signal of the process may land in this thread) */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &oldmask);
    if (pthread_create(&g_thread, NULL, scan_main, NULL) == 0)
        g_thread_made = true;
    else
        g_scan_started = false;
    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
}

int lm_search_indexed(bool *done)
{
    if (done)
        *done = atomic_load(&g_scan_done);
    pthread_mutex_lock(&g_lock);
    int n = (int)g_idx.n;
    pthread_mutex_unlock(&g_lock);
    return n;
}

void lm_search_shutdown(void)
{
    atomic_store(&g_stop, true);
    if (g_thread_made)
        pthread_join(g_thread, NULL);
    g_thread_made = false;
    index_free(&g_idx);
    for (int i = 0; i < g_napps; i++)
        if (g_apps[i].img.px)
            lm_img_free(&g_apps[i].img);
    free(g_apps);
    g_apps = NULL;
    g_napps = 0;
    g_apps_read = false;
    g_scan_started = false;
    atomic_store(&g_scan_done, false);
}

/* ---- the results ----------------------------------------------------------------------------------------------------------- */

struct result {
    struct lm_dock_item pub;
    char name[LM_DOCK_NAME_MAX];
    char cmd[LM_DOCK_CMD_MAX];
    char detail[PATH_MAX + 40];
};

static struct result g_res[LM_SEARCH_MAX];
static int g_nres;

const struct lm_dock_item *lm_search_item(int i)
{
    return i >= 0 && i < g_nres ? &g_res[i].pub : NULL;
}

const char *lm_search_detail(int i)
{
    return i >= 0 && i < g_nres ? g_res[i].detail : "";
}

bool lm_search_owns(const struct lm_dock_item *it)
{
    return it >= &g_res[0].pub && it <= &g_res[LM_SEARCH_MAX - 1].pub;
}

static struct result *add_result(const char *name, const char *cmd, int kind)
{
    if (g_nres >= LM_SEARCH_MAX)
        return NULL;
    struct result *r = &g_res[g_nres];
    memset(r, 0, sizeof(*r));
    snprintf(r->name, sizeof(r->name), "%s", name);
    snprintf(r->cmd, sizeof(r->cmd), "%s", cmd);
    r->pub.name = r->name;
    r->pub.cmd = r->cmd;
    r->pub.match = "";
    r->pub.kind = kind;
    r->pub.raw = -1000 - g_nres;
    g_nres++;
    return r;
}

static void shell_quote(const char *in, char *out, size_t n)
{
    size_t o = 0;
    if (o + 1 < n)
        out[o++] = '\'';
    for (const char *p = in; *p && o + 5 < n; p++) {
        if (*p == '\'') {
            memcpy(out + o, "'\\''", 4);
            o += 4;
        } else {
            out[o++] = *p;
        }
    }
    if (o + 2 < n) {
        out[o++] = '\'';
    }
    out[o] = 0;
}

static bool has_ext(const char *name, const char *const *list)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    for (; *list; list++)
        if (!strcasecmp(dot + 1, *list))
            return true;
    return false;
}

static bool in_path(const char *prog)
{
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/local/bin:/usr/bin:/bin";
    char buf[PATH_MAX];
    while (*path) {
        size_t len = strcspn(path, ":");
        snprintf(buf, sizeof(buf), "%.*s/%s", (int)len, path, prog);
        if (len && access(buf, X_OK) == 0)
            return true;
        path += len + (path[len] ? 1 : 0);
    }
    return false;
}

/* What opens a file, and the icon that stands for it. The program is expanded alone and the quoted path is added after it: the launcher's expansion
 * splits at ` || ` and would cut a file name that has those characters in it. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"          /* (a command that is too long is cut, and then fails to start) */
static void file_open(const char *path, bool dir, char *cmd, size_t n, int *kind)
{
    static const char *const pics[] = { "png", "jpg", "jpeg", "ppm", "gif", "bmp", "webp", "mp4", "m4v", "mkv", "webm", "avi", "mov", NULL };
    static const char *const text[] = { "txt", "md", "c", "h", "cpp", "py", "sh", "conf", "ini", "json", "xml", "html", "css", "js", "ts", "yml", "yaml", "toml",
                                        "log", "csv", "rs", "go", "java", "cfg", "tex", "rst", NULL };
    const char *prog;
    if (dir) {
        *kind = LM_DOCKICON_FILES;
        prog = "mellagui-files";
    } else if (has_ext(path, pics)) {
        *kind = LM_DOCKICON_IMAGES;
        prog = "@view";
    } else if (has_ext(path, text)) {
        *kind = LM_DOCKICON_EDITOR;
        prog = "mellagui-edit";
    } else {
        *kind = LM_DOCKICON_GENERIC;
        prog = in_path("xdg-open") ? "xdg-open" : "mellagui-edit";
    }
    char expanded[LM_DOCK_CMD_MAX], q[PATH_MAX * 2];
    lm_launch_expand(prog, expanded, sizeof(expanded));
    shell_quote(path, q, sizeof(q));
    snprintf(cmd, n, "%s %s", expanded, q);
}

#pragma GCC diagnostic pop

static void load_app_icon(struct app *a)
{
    if (a->icon_done)
        return;
    a->icon_done = true;
    char spec[260] = "";
    if (a->icon[0] == '/')
        snprintf(spec, sizeof(spec), "%s", a->icon);
    else if (a->icon[0])
        snprintf(spec, sizeof(spec), "theme:%s", a->icon);
    memset(&a->pub, 0, sizeof(a->pub));
    lm_dock_icon_load(&a->pub, &a->img, a->name, spec);
}

struct scored {
    int score;
    int index;
};

static int cmp_scored(const void *a, const void *b)
{
    const struct scored *x = a, *y = b;
    return y->score != x->score ? y->score - x->score : x->index - y->index;
}

int lm_search_query(const char *query, bool (*known)(const char *name))
{
    g_nres = 0;
    if (!query || !*query)
        return 0;
    double v;
    if (lm_calc_eval(query, &v)) {                                      /* "= 40": Enter copies it */
        char num[48], title[64], cmd[96];
        lm_calc_format(v, num, sizeof(num));
        snprintf(title, sizeof(title), "= %s", num);
        snprintf(cmd, sizeof(cmd), "@copy:%s", num);
        struct result *r = add_result(title, cmd, LM_DOCKICON_CALCULATOR);
        if (r)
            snprintf(r->detail, sizeof(r->detail), "Enter copies %s", num);
    }
    struct query q;
    query_split(query, &q);
    if (!q.n)
        return g_nres;
    struct scored best[12];
    int nb = 0;
    for (int i = 0; i < g_napps && g_apps; i++) {                       /* the programs */
        struct app *a = &g_apps[i];
        int sc = query_score(&q, a->name);
        if (!sc && a->generic[0])
            sc = query_score(&q, a->generic) * 6 / 10;
        if (!sc && a->keywords[0])
            sc = query_score(&q, a->keywords) * 5 / 10;
        if (!sc || (known && known(a->name)))
            continue;
        struct scored s = { sc, i };
        if (nb < 12) {
            best[nb++] = s;
        } else {
            int w = 0;
            for (int k = 1; k < nb; k++)
                if (cmp_scored(&best[k], &best[w]) > 0)
                    w = k;
            if (cmp_scored(&s, &best[w]) < 0)
                best[w] = s;
        }
    }
    qsort(best, (size_t)nb, sizeof(best[0]), cmp_scored);
    for (int k = 0; k < nb; k++) {
        struct app *a = &g_apps[best[k].index];
        load_app_icon(a);
        char raw[640], cmd[LM_DOCK_CMD_MAX];
        if (a->terminal)
            snprintf(raw, sizeof(raw), "@terminal -e %s", a->exec);
        else
            snprintf(raw, sizeof(raw), "%s", a->exec);
        lm_launch_expand(raw, cmd, sizeof(cmd));
        struct result *r = add_result(a->name, cmd, a->pub.kind);
        if (!r)
            break;
        r->pub.img = a->pub.img;
        r->pub.plate = a->pub.plate;
        r->pub.round = a->pub.round;
        snprintf(r->detail, sizeof(r->detail), "%s", a->exec);
    }
    struct scored files[8];                                             /* the files: the name matches, a shorter path first */
    int nf = 0;
    pthread_mutex_lock(&g_lock);                                        /* (the thread that makes the index publishes a new one under this) */
    for (size_t i = 0; i < g_idx.n && g_idx.pool; i++) {
        const char *path = g_idx.pool + g_idx.ents[i].off, *base = path + g_idx.ents[i].base;
        int sc = query_score(&q, base);
        if (!sc)
            continue;
        sc = sc * 4 - (int)strlen(path) / 8 + (g_idx.ents[i].dir ? 4 : 0);
        struct scored s = { sc, (int)i };
        if (nf < 8) {
            files[nf++] = s;
        } else {
            int w = 0;
            for (int k = 1; k < nf; k++)
                if (cmp_scored(&files[k], &files[w]) > 0)
                    w = k;
            if (cmp_scored(&s, &files[w]) < 0)
                files[w] = s;
        }
    }
    qsort(files, (size_t)nf, sizeof(files[0]), cmp_scored);
    for (int k = 0; k < nf; k++) {
        const struct fent *e = &g_idx.ents[files[k].index];
        const char *path = g_idx.pool + e->off;
        char cmd[LM_DOCK_CMD_MAX];
        int kind;
        file_open(path, e->dir, cmd, sizeof(cmd), &kind);
        struct result *r = add_result(path + e->base, cmd, kind);
        if (!r)
            break;
        snprintf(r->detail, sizeof(r->detail), "%s", path);
    }
    pthread_mutex_unlock(&g_lock);
    return g_nres;
}
