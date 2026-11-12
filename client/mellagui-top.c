/*
 * mellagui-top - the process and service manager (Ctrl+Shift+Esc, or the dock's Processes button).
 *
 *   mellagui-top                 the window
 *   mellagui-top --list [N]      print the N busiest processes (default 15) after a short sample, no window
 *   mellagui-top --selftest
 *
 * Processes: every process of the machine, read from /proc once a second (CPU of one core in percent, resident memory, state, nice value, the
 * user and the command line), with a header of the whole machine (CPU with a history graph, memory, swap, load, uptime). Click a column or press
 * C / M / P / N to sort, U shows only your own processes, / filters (name, command, user or number), Space stops the refresh. T / Delete ends the
 * selected process (SIGTERM), K / Shift+Delete kills it (SIGKILL) - both ask first - S pauses it (SIGSTOP), R lets it go on, + / - change its nice
 * value (lower priority / higher: only root may raise), a right click gives the same as a menu. Only what the user may signal works: the
 * message says when the system refuses.
 *
 * Services: the systemd services of the system or (U) of the user session, from `systemctl list-units`; S starts, X stops, R restarts the
 * selected one, E / D enable / disable it, Enter or L shows its journal (`journalctl -u`; Esc goes back, R reads it again). A system service
 * needs the rights polkit gives: without them the program shows systemd's refusal. $MELLAGUI_SYSTEMCTL / $MELLAGUI_JOURNALCTL name other
 * programs (the tests use stand-ins).
 *
 * Tab switches between the two lists, Q or Esc (with no filter) closes.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "font.h"
#include "keymap.h"
#include "metrics.h"
#include "protocol.h"
#include "sysinfo.h"
#include "theme.h"
#include "uikit.h"
#include "util.h"
#include "wlc.h"
#include "wlclip.h"
#include "wltext.h"
#include "wlwin.h"

#define PAD 10
#define ROW_H 18
#define HEAD_H 62
#define TOOL_H 32
#define COLH_H 20
#define STATUS_H 24
#define MAX_PROCS 8192
#define MAX_SVCS 2048
#define MAX_LOG 600
#define HIST_N 120
#define REFRESH_MS 1000

#define COL_BG (lm_theme.background)
#define COL_FIELD (lm_theme.field)
#define COL_BAR (lm_theme.bar)
#define COL_TEXT (lm_theme.text_soft)
#define COL_DIM (lm_theme.text_dim)
#define COL_SEL (lm_theme.button_hover)

/* ---- data ----------------------------------------------------------------------------------------------------------------- */

struct proc {
    int pid, ppid;
    uid_t uid;
    char user[24];
    char name[20];                      /* comm */
    char cmd[200];                      /* the command line ("[name]" for kernel threads) */
    char state;                         /* R S D Z T I ... */
    int nice, threads;
    long long rss_kb;
    unsigned long long ticks, start;    /* utime + stime, start time (jiffies since boot) */
    double cpu;                         /* percent of one core since the previous sample */
};

struct pstat {                          /* what /proc/PID/stat says (the fields we use) */
    char name[20];
    char state;
    int ppid, nice, threads;
    unsigned long long ticks, start;
    long long rss_pages;
};

struct svc {
    char unit[96], load[12], active[12], sub[16], desc[120];
};

enum sortkey { S_CPU, S_MEM, S_PID, S_NAME, S_USER };
enum tab { T_PROCS, T_SVCS };
enum confirm { C_NONE, C_TERM, C_KILL, C_SVC_STOP };

struct vlist {
    int sel, top;
};

struct app {
    struct wlwin win;
    struct wlclip clip;
    enum tab tab;
    /* the machine */
    struct lm_cpu_times prev_total, cur_total;
    int ncores, cpu_pct, hist[HIST_N], nhist;
    long mem_total_kb, mem_avail_kb, swap_total_kb, swap_free_kb;
    double load1;
    long uptime_s;
    /* processes */
    struct proc *p, *old;
    int np, nold, cap, oldcap;
    enum sortkey sort;
    bool reverse, mine, paused;
    int sel_pid;
    struct vlist pv;
    int *shown;                         /* indexes into p[] after the filter and the sort */
    int nshown;
    int nrunning;
    /* services */
    struct svc *s;
    int ns;
    bool user_scope;
    struct vlist sv;
    int *sshown;
    int nsshown;
    char sel_unit[96];
    /* the journal of one service */
    bool logview;
    char **log;
    int nlog, logtop, logx;
    char log_unit[96];
    /* input */
    char filter[96];
    bool ffocus;
    enum confirm confirm;
    char msg[240];
    int64_t msg_until, next_refresh;
    double px, py;
    int hover;
    bool sb_drag;
    int sb_grab;
    uint32_t last_click_ms;
    int last_click_row;
    int menu_kind;                      /* 0 none, 1 process menu, 2 service menu */
    int menu_pid;
};

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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

/* ---- parsing (no I/O: --selftest) --------------------------------------------------------------------------------------------- */

/* "123 (my (prog)) S 1 ..." -> st. The name is whatever lies between the first "(" and the LAST ")". False on a line that is not one. */
static bool parse_proc_stat(const char *text, int *pid, struct pstat *st)
{
    const char *lp = strchr(text, '('), *rp = strrchr(text, ')');
    if (!lp || !rp || rp < lp || sscanf(text, "%d", pid) != 1)
        return false;
    memset(st, 0, sizeof(*st));
    size_t l = (size_t)(rp - lp - 1);
    snprintf(st->name, sizeof(st->name), "%.*s", (int)LM_MIN(l, sizeof(st->name) - 1), lp + 1);
    unsigned long long ut, stt, start;
    long long rss;
    long nice, threads;
    int ppid;
    char state;
    if (sscanf(rp + 1, " %c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %*d %ld %ld %*d %llu %*u %lld", &state, &ppid, &ut, &stt, &nice,
               &threads, &start, &rss) != 8)
        return false;
    st->state = state;
    st->ppid = ppid;
    st->ticks = ut + stt;
    st->nice = (int)nice;
    st->threads = (int)threads;
    st->start = start;
    st->rss_pages = rss;
    return true;
}

static void human_kb(long long kb, char *out, size_t n)
{
    if (kb >= 10LL << 20)
        snprintf(out, n, "%.0fG", (double)kb / (1 << 20));
    else if (kb >= 1LL << 20)
        snprintf(out, n, "%.1fG", (double)kb / (1 << 20));
    else if (kb >= 10 << 10)
        snprintf(out, n, "%.0fM", (double)kb / 1024);
    else if (kb >= 1 << 10)
        snprintf(out, n, "%.1fM", (double)kb / 1024);
    else
        snprintf(out, n, "%dK", (int)(kb < 0 ? 0 : kb));
}

/* "nginx.service   loaded active running A web server" -> svc; false on anything else. */
static bool parse_unit_line(const char *line, struct svc *s)
{
    while (*line == ' ' || *line == '*' || (unsigned char)*line == 0xe2)               /* (--plain leaves no bullet, but be kind to the others) */
        line += (unsigned char)*line == 0xe2 ? 3 : 1;
    char unit[96], load[12], active[12], sub[16];
    int used = 0;
    if (sscanf(line, "%95s %11s %11s %15s %n", unit, load, active, sub, &used) < 4 || !strchr(unit, '.'))
        return false;
    if (strcmp(load, "loaded") && strcmp(load, "not-found") && strcmp(load, "masked") && strcmp(load, "error") && strcmp(load, "bad-setting"))
        return false;
    memset(s, 0, sizeof(*s));
    snprintf(s->unit, sizeof(s->unit), "%s", unit);
    snprintf(s->load, sizeof(s->load), "%s", load);
    snprintf(s->active, sizeof(s->active), "%s", active);
    snprintf(s->sub, sizeof(s->sub), "%s", sub);
    size_t l = used > 0 ? strcspn(line + used, "\r\n") : 0;
    snprintf(s->desc, sizeof(s->desc), "%.*s", (int)LM_MIN(l, sizeof(s->desc) - 1), used > 0 ? line + used : "");
    return true;
}

static bool contains_ci(const char *hay, const char *needle)
{
    return !*needle || strcasestr(hay, needle) != NULL;
}

/* ---- reading /proc ------------------------------------------------------------------------------------------------------------ */

static ssize_t slurp(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t k = read(fd, buf, n - 1);
    close(fd);
    if (k < 0)
        return -1;
    buf[k] = 0;
    return k;
}

static const char *user_name(uid_t uid, char *out, size_t n)
{
    static struct { uid_t uid; char name[24]; } cache[64];
    static int ncache;
    for (int i = 0; i < ncache; i++)
        if (cache[i].uid == uid) {
            snprintf(out, n, "%s", cache[i].name);
            return out;
        }
    struct passwd *pw = getpwuid(uid);
    char nm[24];
    if (pw)
        snprintf(nm, sizeof(nm), "%s", pw->pw_name);
    else
        snprintf(nm, sizeof(nm), "%u", (unsigned)uid);
    if (ncache < 64) {
        cache[ncache].uid = uid;
        snprintf(cache[ncache].name, sizeof(cache[ncache].name), "%s", nm);
        ncache++;
    }
    snprintf(out, n, "%s", nm);
    return out;
}

static const struct proc *find_old(const struct app *a, int pid)
{
    int lo = 0, hi = a->nold - 1;                                       /* `old` is sorted by pid */
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (a->old[mid].pid == pid)
            return &a->old[mid];
        if (a->old[mid].pid < pid)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

static int cmp_pid(const void *x, const void *y)
{
    return ((const struct proc *)x)->pid - ((const struct proc *)y)->pid;
}

static void read_machine(struct app *a)
{
    char buf[8192];
    struct lm_cpu_times cores[LM_SYS_MAX_CORES];
    a->prev_total = a->cur_total;
    if (slurp("/proc/stat", buf, sizeof(buf)) > 0) {
        int n = lm_sysinfo_parse_stat(buf, &a->cur_total, cores, LM_SYS_MAX_CORES);
        if (n > 0)
            a->ncores = n;
    }
    if (a->prev_total.total) {
        a->cpu_pct = lm_sysinfo_cpu_percent(&a->prev_total, &a->cur_total);
        if (a->nhist == HIST_N) {
            memmove(a->hist, a->hist + 1, (HIST_N - 1) * sizeof(int));
            a->nhist--;
        }
        a->hist[a->nhist++] = a->cpu_pct;
    }
    if (slurp("/proc/meminfo", buf, sizeof(buf)) > 0)
        lm_sysinfo_parse_meminfo(buf, &a->mem_total_kb, &a->mem_avail_kb, &a->swap_total_kb, &a->swap_free_kb);
    if (slurp("/proc/loadavg", buf, sizeof(buf)) > 0)
        a->load1 = atof(buf);
    if (slurp("/proc/uptime", buf, sizeof(buf)) > 0)
        a->uptime_s = lm_sysinfo_parse_uptime(buf);
}

static void read_procs(struct app *a)
{
    struct proc *t = a->old;                                            /* the table of the last time becomes the table to compare with */
    int tcap = a->oldcap;
    a->old = a->p;
    a->oldcap = a->cap;
    a->nold = a->np;
    a->p = t;
    a->cap = tcap;
    a->np = 0;
    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    unsigned long long dtotal = a->cur_total.total - a->prev_total.total;
    DIR *d = opendir("/proc");
    struct dirent *de;
    a->nrunning = 0;
    while (d && (de = readdir(d)) && a->np < MAX_PROCS) {
        if (!isdigit((unsigned char)de->d_name[0]))
            continue;
        char path[64], buf[2048];
        snprintf(path, sizeof(path), "/proc/%.40s/stat", de->d_name);
        if (slurp(path, buf, sizeof(buf)) <= 0)
            continue;
        int pid;
        struct pstat st;
        if (!parse_proc_stat(buf, &pid, &st))
            continue;
        if (a->np == a->cap) {
            int nc = a->cap ? a->cap * 2 : 512;
            struct proc *np = realloc(a->p, (size_t)nc * sizeof(*np));
            if (!np)
                break;
            a->p = np;
            a->cap = nc;
        }
        struct proc *p = &a->p[a->np];
        memset(p, 0, sizeof(*p));
        p->pid = pid;
        p->ppid = st.ppid;
        snprintf(p->name, sizeof(p->name), "%s", st.name);
        p->state = st.state;
        p->nice = st.nice;
        p->threads = st.threads;
        p->ticks = st.ticks;
        p->start = st.start;
        p->rss_kb = st.rss_pages * page_kb;
        snprintf(path, sizeof(path), "/proc/%.40s", de->d_name);
        struct stat sb;
        p->uid = stat(path, &sb) == 0 ? sb.st_uid : 0;
        user_name(p->uid, p->user, sizeof(p->user));
        snprintf(path, sizeof(path), "/proc/%.40s/cmdline", de->d_name);
        ssize_t k = slurp(path, buf, sizeof(buf));
        if (k > 0) {
            for (ssize_t i = 0; i < k; i++)
                if (buf[i] == 0)
                    buf[i] = ' ';
            while (k > 0 && buf[k - 1] == ' ')
                buf[--k] = 0;
            snprintf(p->cmd, sizeof(p->cmd), "%.199s", buf);
        }
        if (!p->cmd[0])
            snprintf(p->cmd, sizeof(p->cmd), "[%s]", p->name);
        const struct proc *o = find_old(a, pid);
        if (o && o->start == p->start && dtotal && p->ticks >= o->ticks)
            p->cpu = 100.0 * (double)(p->ticks - o->ticks) * (double)a->ncores / (double)dtotal;
        if (p->state == 'R')
            a->nrunning++;
        a->np++;
    }
    if (d)
        closedir(d);
    qsort(a->p, (size_t)a->np, sizeof(a->p[0]), cmp_pid);
}

/* ---- sorting and filtering ---------------------------------------------------------------------------------------------------- */

static struct app *g_sort_app;

static int cmp_shown(const void *x, const void *y)
{
    const struct app *a = g_sort_app;
    const struct proc *p = &a->p[*(const int *)x], *q = &a->p[*(const int *)y];
    int c = 0;
    switch (a->sort) {
    case S_CPU: c = p->cpu < q->cpu ? 1 : p->cpu > q->cpu ? -1 : 0; if (!c) c = q->rss_kb > p->rss_kb ? 1 : q->rss_kb < p->rss_kb ? -1 : 0; break;
    case S_MEM: c = q->rss_kb > p->rss_kb ? 1 : q->rss_kb < p->rss_kb ? -1 : 0; break;
    case S_PID: c = p->pid - q->pid; break;
    case S_NAME: c = strcasecmp(p->name, q->name); break;
    case S_USER: c = strcasecmp(p->user, q->user); break;
    }
    if (a->reverse)
        c = -c;
    return c ? c : p->pid - q->pid;
}

static bool proc_matches(const struct app *a, const struct proc *p)
{
    if (a->mine && p->uid != getuid())
        return false;
    if (!a->filter[0])
        return true;
    char num[16];
    snprintf(num, sizeof(num), "%d", p->pid);
    return contains_ci(p->name, a->filter) || contains_ci(p->cmd, a->filter) || contains_ci(p->user, a->filter) || !strcmp(num, a->filter);
}

static const struct proc *selected_proc(const struct app *a)
{
    if (a->pv.sel < 0 || a->pv.sel >= a->nshown)
        return NULL;
    return &a->p[a->shown[a->pv.sel]];
}

static void clamp_view(struct vlist *v, int n, int visible)
{
    v->sel = LM_CLAMP(v->sel, 0, LM_MAX(n - 1, 0));
    v->top = LM_CLAMP(v->top, 0, LM_MAX(n - visible, 0));
}

static int list_rows(const struct app *a)
{
    return LM_MAX((a->win.bh - HEAD_H - TOOL_H - COLH_H - STATUS_H) / ROW_H, 1);
}

static void keep_visible(struct vlist *v, int rows)
{
    if (v->sel < v->top)
        v->top = v->sel;
    if (v->sel >= v->top + rows)
        v->top = v->sel - rows + 1;
}

static void rebuild_procs(struct app *a)
{
    int *sh = realloc(a->shown, (size_t)LM_MAX(a->np, 1) * sizeof(int));
    if (!sh)
        return;
    a->shown = sh;
    a->nshown = 0;
    for (int i = 0; i < a->np; i++)
        if (proc_matches(a, &a->p[i]))
            a->shown[a->nshown++] = i;
    g_sort_app = a;
    qsort(a->shown, (size_t)a->nshown, sizeof(int), cmp_shown);
    int at = -1;                                                        /* the cursor stays on its process when the order changes */
    for (int i = 0; i < a->nshown; i++)
        if (a->p[a->shown[i]].pid == a->sel_pid)
            at = i;
    if (at >= 0)
        a->pv.sel = at;
    clamp_view(&a->pv, a->nshown, list_rows(a));
    const struct proc *sp = selected_proc(a);
    a->sel_pid = sp ? sp->pid : 0;
    keep_visible(&a->pv, list_rows(a));
}

static bool svc_matches(const struct app *a, const struct svc *s)
{
    return !a->filter[0] || contains_ci(s->unit, a->filter) || contains_ci(s->desc, a->filter) || contains_ci(s->active, a->filter) ||
           contains_ci(s->sub, a->filter);
}

static int rank_active(const char *active)
{
    return !strcmp(active, "failed") ? 0 : !strcmp(active, "active") ? 1 : 2;
}

static struct app *g_svc_app;

static int cmp_svc(const void *x, const void *y)
{
    const struct svc *p = &g_svc_app->s[*(const int *)x], *q = &g_svc_app->s[*(const int *)y];
    int c = rank_active(p->active) - rank_active(q->active);
    return c ? c : strcasecmp(p->unit, q->unit);
}

static void rebuild_svcs(struct app *a)
{
    int *sh = realloc(a->sshown, (size_t)LM_MAX(a->ns, 1) * sizeof(int));
    if (!sh)
        return;
    a->sshown = sh;
    a->nsshown = 0;
    for (int i = 0; i < a->ns; i++)
        if (svc_matches(a, &a->s[i]))
            a->sshown[a->nsshown++] = i;
    g_svc_app = a;
    qsort(a->sshown, (size_t)a->nsshown, sizeof(int), cmp_svc);
    for (int i = 0; i < a->nsshown; i++)
        if (!strcmp(a->s[a->sshown[i]].unit, a->sel_unit))
            a->sv.sel = i;
    clamp_view(&a->sv, a->nsshown, list_rows(a));
    if (a->sv.sel < a->nsshown)
        snprintf(a->sel_unit, sizeof(a->sel_unit), "%s", a->s[a->sshown[a->sv.sel]].unit);
    keep_visible(&a->sv, list_rows(a));
}

static const struct svc *selected_svc(const struct app *a)
{
    return a->sv.sel >= 0 && a->sv.sel < a->nsshown ? &a->s[a->sshown[a->sv.sel]] : NULL;
}

/* ---- running programs ----------------------------------------------------------------------------------------------------- */

/* Runs argv with stdout and stderr in one pipe; the text comes back in *out (malloc'd, NUL terminated). Exit status, or -1 (timeout, no program). */
static int run_cmd(char *const argv[], int timeout_ms, char **out)
{
    int po[2];
    *out = NULL;
    if (pipe2(po, O_CLOEXEC) < 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(po[0]);
        close(po[1]);
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        int nul = open("/dev/null", O_RDONLY);
        dup2(nul, 0);
        dup2(po[1], 1);
        dup2(po[1], 2);
        setenv("LC_ALL", "C", 1);
        setenv("SYSTEMD_PAGER", "", 1);
        setenv("SYSTEMD_COLORS", "0", 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(po[1]);
    size_t cap = 16384, len = 0;
    char *buf = malloc(cap);
    int64_t t0 = now_ms();
    bool timed_out = false;
    for (;;) {
        struct pollfd pf = { po[0], POLLIN, 0 };
        int r = poll(&pf, 1, 100);
        if (r > 0) {
            if (len + 8192 + 1 > cap && cap < (4u << 20)) {
                char *nb = realloc(buf, cap * 2);
                if (!nb)
                    break;
                buf = nb;
                cap *= 2;
            }
            ssize_t k = read(po[0], buf + len, cap - len - 1);
            if (k <= 0)
                break;
            len += (size_t)k;
            if (len + 1 >= cap && cap >= (4u << 20))
                break;
        } else if (r < 0 && errno != EINTR) {
            break;
        }
        if (now_ms() - t0 > timeout_ms) {
            timed_out = true;
            break;
        }
    }
    close(po[0]);
    if (timed_out) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
    }
    int status = 0;
    pid_t w = waitpid(pid, &status, 0);
    buf[len] = 0;
    *out = buf;
    if (timed_out)
        return -1;
    return w > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static const char *prog(const char *env, const char *def)
{
    const char *e = getenv(env);
    return e && *e ? e : def;
}

static void first_line(const char *text, char *out, size_t n)
{
    while (*text == '\n' || *text == ' ')
        text++;
    size_t l = strcspn(text, "\n");
    snprintf(out, n, "%.*s", (int)LM_MIN(l, n - 1), text);
}

static void read_services(struct app *a)
{
    char *argv[12];
    int n = 0;
    argv[n++] = (char *)prog("MELLAGUI_SYSTEMCTL", "systemctl");
    if (a->user_scope)
        argv[n++] = "--user";
    argv[n++] = "list-units";
    argv[n++] = "--type=service";
    argv[n++] = "--all";
    argv[n++] = "--no-legend";
    argv[n++] = "--plain";
    argv[n++] = "--no-pager";
    argv[n++] = "--full";
    argv[n] = NULL;
    char *out;
    int rc = run_cmd(argv, 8000, &out);
    free(a->s);
    a->s = NULL;
    a->ns = 0;
    if (rc != 0 || !out) {
        char why[160];
        first_line(out ? out : "", why, sizeof(why));
        msg(a, "systemctl: %s", why[0] ? why : rc < 0 ? "no answer (is systemd running?)" : "failed");
        free(out);
        rebuild_svcs(a);
        return;
    }
    int cap = 0;
    for (char *s = out; *s;) {
        size_t l = strcspn(s, "\n");
        char line[400];
        snprintf(line, sizeof(line), "%.*s", (int)LM_MIN(l, sizeof(line) - 1), s);
        s += l + (s[l] ? 1 : 0);
        struct svc sv;
        if (!parse_unit_line(line, &sv))
            continue;
        if (a->ns == cap) {
            cap = cap ? cap * 2 : 128;
            struct svc *nb = realloc(a->s, (size_t)cap * sizeof(*nb));
            if (!nb)
                break;
            a->s = nb;
        }
        if (a->ns < MAX_SVCS)
            a->s[a->ns++] = sv;
    }
    free(out);
    rebuild_svcs(a);
}

static void svc_action(struct app *a, const char *verb)
{
    const struct svc *s = selected_svc(a);
    if (!s)
        return;
    char unit[96];
    snprintf(unit, sizeof(unit), "%s", s->unit);
    char *argv[10];
    int n = 0;
    argv[n++] = (char *)prog("MELLAGUI_SYSTEMCTL", "systemctl");
    if (a->user_scope)
        argv[n++] = "--user";
    argv[n++] = "--no-ask-password";
    argv[n++] = (char *)verb;
    argv[n++] = unit;
    argv[n] = NULL;
    char *out;
    int rc = run_cmd(argv, 30000, &out);
    char why[200];
    first_line(out ? out : "", why, sizeof(why));
    free(out);
    if (rc == 0)
        msg(a, "%s %s: done", verb, unit);
    else
        msg(a, "%s %s: %s", verb, unit, why[0] ? why : rc < 0 ? "no answer" : "refused");
    read_services(a);
}

static void log_free(struct app *a)
{
    for (int i = 0; i < a->nlog; i++)
        free(a->log[i]);
    free(a->log);
    a->log = NULL;
    a->nlog = 0;
}

static void read_log(struct app *a)
{
    log_free(a);
    char *argv[12];
    int n = 0;
    argv[n++] = (char *)prog("MELLAGUI_JOURNALCTL", "journalctl");
    if (a->user_scope)
        argv[n++] = "--user";
    argv[n++] = "-u";
    argv[n++] = a->log_unit;
    argv[n++] = "-n";
    argv[n++] = "400";
    argv[n++] = "--no-pager";
    argv[n] = NULL;
    char *out;
    int rc = run_cmd(argv, 10000, &out);
    a->log = calloc(MAX_LOG, sizeof(char *));
    if (!a->log) {
        free(out);
        return;
    }
    if (out) {
        for (char *s = out; *s && a->nlog < MAX_LOG;) {
            size_t l = strcspn(s, "\n");
            a->log[a->nlog++] = strndup(s, l);
            s += l + (s[l] ? 1 : 0);
        }
    }
    if (rc < 0 && a->nlog == 0)
        a->log[a->nlog++] = strdup("(journalctl gave no answer)");
    free(out);
    a->logtop = LM_MAX(a->nlog - 1, 0);                                 /* the end: the newest lines */
    a->logx = 0;
}

/* ---- actions ------------------------------------------------------------------------------------------------------------- */

static void refresh(struct app *a)
{
    read_machine(a);
    read_procs(a);
    rebuild_procs(a);
    a->next_refresh = now_ms() + REFRESH_MS;
    a->win.redraw = true;
}

static void send_signal(struct app *a, int sig, const char *what)
{
    const struct proc *p = selected_proc(a);
    if (!p)
        return;
    if (p->pid == getpid()) {
        msg(a, "That is this program");
        return;
    }
    if (kill(p->pid, sig) < 0)
        msg(a, "%s %s (%d): %s", what, p->name, p->pid, errno == EPERM ? "not permitted (it belongs to another user)" : strerror(errno));
    else
        msg(a, "%s %s (%d)", what, p->name, p->pid);
    a->next_refresh = now_ms() + 300;                                   /* show the result soon */
}

static void renice(struct app *a, int delta)
{
    const struct proc *p = selected_proc(a);
    if (!p)
        return;
    int nv = LM_CLAMP(p->nice + delta, -20, 19);
    if (setpriority(PRIO_PROCESS, (id_t)p->pid, nv) < 0)
        msg(a, "nice %s (%d): %s", p->name, p->pid, errno == EACCES || errno == EPERM ? "only root may raise the priority or change another user's process" : strerror(errno));
    else
        msg(a, "%s (%d) now has nice %d", p->name, p->pid, nv);
    a->next_refresh = now_ms() + 300;
}

static void ask(struct app *a, enum confirm c)
{
    if (a->tab == T_PROCS ? !selected_proc(a) : !selected_svc(a))
        return;
    a->confirm = c;
    a->win.redraw = true;
}

static void do_confirmed(struct app *a)
{
    enum confirm c = a->confirm;
    a->confirm = C_NONE;
    if (c == C_TERM)
        send_signal(a, SIGTERM, "Asked to end");
    else if (c == C_KILL)
        send_signal(a, SIGKILL, "Killed");
    else if (c == C_SVC_STOP)
        svc_action(a, "stop");
    a->win.redraw = true;
}

static void open_log(struct app *a)
{
    const struct svc *s = selected_svc(a);
    if (!s)
        return;
    snprintf(a->log_unit, sizeof(a->log_unit), "%s", s->unit);
    a->logview = true;
    read_log(a);
    a->win.redraw = true;
}

static void switch_tab(struct app *a, enum tab t)
{
    if (a->tab == t)
        return;
    a->tab = t;
    a->logview = false;
    a->confirm = C_NONE;
    if (t == T_SVCS && !a->s)
        read_services(a);
    a->win.redraw = true;
}

static void set_sort(struct app *a, enum sortkey k)
{
    if (a->sort == k)
        a->reverse = !a->reverse;
    else {
        a->sort = k;
        a->reverse = false;
    }
    rebuild_procs(a);
    a->win.redraw = true;
}

static void copy_text(struct app *a, const char *what, const char *text)
{
    wlclip_copy(&a->clip, text);
    msg(a, "Copied %s", what);
}

/* ---- buttons ----------------------------------------------------------------------------------------------------------- */

enum btn_id { B_TAB_PROCS, B_TAB_SVCS, B_END, B_KILL, B_PAUSE, B_GO, B_MINE, B_START, B_STOP, B_RESTART, B_LOG, B_SCOPE, B_BACK, B_RELOAD, N_BTN };

struct btn {
    enum btn_id id;
    const char *label;
    int x, w;
    uint32_t base, hover;
    bool on, enabled;
};

static int build_buttons(const struct app *a, struct btn *b)
{
    int n = 0, x = PAD;
    struct def { enum btn_id id; const char *label; uint32_t base, hover; bool on, enabled; };
    struct def defs[16];
    int nd = 0;
    bool procs = a->tab == T_PROCS, has = procs ? selected_proc(a) != NULL : selected_svc(a) != NULL;
    defs[nd++] = (struct def){ B_TAB_PROCS, "Processes", lm_theme.btn_blue, lm_theme.btn_blue_lit, procs, true };
    defs[nd++] = (struct def){ B_TAB_SVCS, "Services", lm_theme.btn_blue, lm_theme.btn_blue_lit, !procs, true };
    if (a->logview) {
        defs[nd++] = (struct def){ B_BACK, "Back", lm_theme.btn_grey, lm_theme.btn_grey_lit, false, true };
        defs[nd++] = (struct def){ B_RELOAD, "Reload", lm_theme.btn_teal, lm_theme.btn_teal_lit, false, true };
    } else if (procs) {
        defs[nd++] = (struct def){ B_END, "End", lm_theme.btn_amber, lm_theme.btn_amber_lit, false, has };
        defs[nd++] = (struct def){ B_KILL, "Kill", lm_theme.btn_red, lm_theme.btn_red_lit, false, has };
        defs[nd++] = (struct def){ B_PAUSE, "Pause", lm_theme.btn_purple, lm_theme.btn_purple_lit, false, has };
        defs[nd++] = (struct def){ B_GO, "Go on", lm_theme.btn_green, lm_theme.btn_green_lit, false, has };
        defs[nd++] = (struct def){ B_MINE, "Mine", lm_theme.btn_teal, lm_theme.btn_teal_lit, a->mine, true };
    } else {
        defs[nd++] = (struct def){ B_START, "Start", lm_theme.btn_green, lm_theme.btn_green_lit, false, has };
        defs[nd++] = (struct def){ B_STOP, "Stop", lm_theme.btn_red, lm_theme.btn_red_lit, false, has };
        defs[nd++] = (struct def){ B_RESTART, "Restart", lm_theme.btn_amber, lm_theme.btn_amber_lit, false, has };
        defs[nd++] = (struct def){ B_LOG, "Log", lm_theme.btn_teal, lm_theme.btn_teal_lit, false, has };
        defs[nd++] = (struct def){ B_SCOPE, a->user_scope ? "User" : "System", lm_theme.btn_purple, lm_theme.btn_purple_lit, a->user_scope, true };
        defs[nd++] = (struct def){ B_RELOAD, "Reload", lm_theme.btn_grey, lm_theme.btn_grey_lit, false, true };
    }
    for (int i = 0; i < nd; i++) {
        b[n].id = defs[i].id;
        b[n].label = defs[i].label;
        b[n].w = lm_font_text_width(defs[i].label) + 22;
        b[n].x = x;
        b[n].base = defs[i].on ? defs[i].hover : defs[i].base;
        b[n].hover = defs[i].on ? lm_mix_rgb(defs[i].hover, 0xffffff, 0.25) : defs[i].hover;
        b[n].on = defs[i].on;
        b[n].enabled = defs[i].enabled;
        x += b[n].w + 6 + (i == 1 ? 14 : 0);                            /* a gap after the tabs */
        n++;
    }
    return n;
}

static int button_at(const struct app *a, double px, double py)
{
    int y0 = HEAD_H;
    if (py < y0 + 4 || py >= y0 + TOOL_H - 4)
        return -1;
    struct btn b[16];
    int n = build_buttons(a, b);
    for (int i = 0; i < n; i++)
        if (px >= b[i].x && px < b[i].x + b[i].w)
            return i;
    return -1;
}

static void press_button(struct app *a, enum btn_id id)
{
    switch (id) {
    case B_TAB_PROCS: switch_tab(a, T_PROCS); break;
    case B_TAB_SVCS: switch_tab(a, T_SVCS); break;
    case B_END: ask(a, C_TERM); break;
    case B_KILL: ask(a, C_KILL); break;
    case B_PAUSE: send_signal(a, SIGSTOP, "Paused"); break;
    case B_GO: send_signal(a, SIGCONT, "Continued"); break;
    case B_MINE: a->mine = !a->mine; rebuild_procs(a); break;
    case B_START: svc_action(a, "start"); break;
    case B_STOP: ask(a, C_SVC_STOP); break;
    case B_RESTART: svc_action(a, "restart"); break;
    case B_LOG: open_log(a); break;
    case B_SCOPE:
        a->user_scope = !a->user_scope;
        a->sv = (struct vlist){ 0, 0 };
        a->sel_unit[0] = 0;
        read_services(a);
        break;
    case B_BACK: a->logview = false; break;
    case B_RELOAD:
        if (a->logview)
            read_log(a);
        else
            read_services(a);
        break;
    default: break;
    }
    a->win.redraw = true;
}

/* ---- painting ----------------------------------------------------------------------------------------------------------- */

static void draw_bar(struct wlwin *w, int x, int y, int bw, int bh, int pct, uint32_t col)
{
    wlwin_fill(w, x, y, bw, bh, COL_FIELD);
    wlwin_fill(w, x, y, bw * LM_CLAMP(pct, 0, 100) / 100, bh, col);
}

static uint32_t load_colour(int pct)
{
    return pct >= 90 ? lm_theme.error : pct >= 65 ? lm_theme.warn : lm_theme.ok;
}

static void paint_head(struct app *a, struct wlwin *w)
{
    wlwin_fill(w, 0, 0, w->bw, HEAD_H, COL_BG);
    int lw = 40, bx = PAD + lw, bw = LM_CLAMP(w->bw / 4, 110, 200);
    char t[96];
    wlwin_text(w, PAD, 6, "CPU", 1, COL_DIM, 0);
    draw_bar(w, bx, 8, bw, 10, a->cpu_pct, load_colour(a->cpu_pct));
    snprintf(t, sizeof(t), "%d%%  %d cores", a->cpu_pct, a->ncores);
    wlwin_text(w, bx + bw + 8, 6, t, 1, COL_TEXT, 0);
    int mp = a->mem_total_kb ? (int)(100 * (a->mem_total_kb - a->mem_avail_kb) / a->mem_total_kb) : 0;
    char u[16], tt[16];
    wlwin_text(w, PAD, 24, "Mem", 1, COL_DIM, 0);
    draw_bar(w, bx, 26, bw, 10, mp, load_colour(mp));
    human_kb(a->mem_total_kb - a->mem_avail_kb, u, sizeof(u));
    human_kb(a->mem_total_kb, tt, sizeof(tt));
    snprintf(t, sizeof(t), "%d%%  %s / %s", mp, u, tt);
    wlwin_text(w, bx + bw + 8, 24, t, 1, COL_TEXT, 0);
    int sp = a->swap_total_kb ? (int)(100 * (a->swap_total_kb - a->swap_free_kb) / a->swap_total_kb) : 0;
    wlwin_text(w, PAD, 42, "Swap", 1, COL_DIM, 0);
    draw_bar(w, bx, 44, bw, 10, sp, load_colour(sp));
    human_kb(a->swap_total_kb - a->swap_free_kb, u, sizeof(u));
    human_kb(a->swap_total_kb, tt, sizeof(tt));
    if (a->swap_total_kb)
        snprintf(t, sizeof(t), "%d%%  %s / %s", sp, u, tt);
    else
        snprintf(t, sizeof(t), "none");
    wlwin_text(w, bx + bw + 8, 42, t, 1, COL_TEXT, 0);
    /* the history of the CPU load and the numbers of the machine on the right */
    int gx = bx + bw + 8 + 17 * LM_FONT_W + 16, gw = w->bw - gx - PAD;
    if (gw > 60) {
        wlwin_fill(w, gx, 6, gw, HEAD_H - 14, COL_FIELD);
        int gh = HEAD_H - 16;
        for (int i = 0; i < a->nhist; i++) {
            int x = gx + 1 + (int)((int64_t)i * (gw - 2) / HIST_N), h = a->hist[i] * gh / 100;
            int colw = LM_MAX((gw - 2) / HIST_N, 1);
            wlwin_fill(w, x, 7 + gh - h, colw, LM_MAX(h, 1), load_colour(a->hist[i]));
        }
        char info[96], up[24];
        lm_sysinfo_format_uptime(a->uptime_s, up, sizeof(up));
        snprintf(info, sizeof(info), "load %.2f  up %s  %d tasks, %d running", a->load1, up, a->np, a->nrunning);
        wlwin_text(w, gx + 4, 8, info, 1, COL_DIM, gw - 8);
    }
}

static void draw_rbtn(struct wlwin *w, const struct btn *b, bool hot)
{
    int y = HEAD_H + 4, h = TOOL_H - 8;
    uint32_t base = b->enabled ? (hot ? b->hover : b->base) : lm_mix_rgb(b->base, COL_BG, 0.6);
    lm_round_fill(w->pixels, w->bw, w->bw, w->bh, b->x, y, b->w, h, lm_button_radius(h / 2), base);
    int tw = lm_font_text_width(b->label);
    wlwin_text(w, b->x + (b->w - tw) / 2, y + (h - LM_FONT_H) / 2, b->label, 1, b->enabled ? lm_theme.on_colour : COL_DIM, b->w - 4);
}

static void col_pos(int *pid, int *user, int *cpu, int *mem, int *st, int *ni, int *cmd)
{
    int x = PAD;
    *pid = x;
    x += 8 * LM_FONT_W;
    *user = x;
    x += 12 * LM_FONT_W;
    *cpu = x;
    x += 8 * LM_FONT_W;
    *mem = x;
    x += 8 * LM_FONT_W;
    *st = x;
    x += 3 * LM_FONT_W;
    *ni = x;
    x += 5 * LM_FONT_W;
    *cmd = x;
}

static int sort_column_at(int x)
{
    int pid, user, cpu, mem, st, ni, cmd;
    col_pos(&pid, &user, &cpu, &mem, &st, &ni, &cmd);
    if (x >= pid && x < user)
        return S_PID;
    if (x >= user && x < cpu)
        return S_USER;
    if (x >= cpu && x < mem)
        return S_CPU;
    if (x >= mem && x < st)
        return S_MEM;
    if (x >= cmd)
        return S_NAME;
    return -1;
}

static int scrollbar_geom(const struct app *a, int total, int top, int *ty, int *th)
{
    int rows = list_rows(a), area = rows * ROW_H;
    if (total <= rows)
        return 0;
    *th = LM_MAX(area * rows / total, 16);
    *ty = HEAD_H + TOOL_H + COLH_H + (area - *th) * top / LM_MAX(total - rows, 1);
    return 1;
}

static void paint_scrollbar(struct app *a, struct wlwin *w, int total, int top)
{
    int ty, th, sbw = lm_metrics.scrollbar_width, y0 = HEAD_H + TOOL_H + COLH_H;
    if (!scrollbar_geom(a, total, top, &ty, &th))
        return;
    wlwin_fill(w, w->bw - sbw, y0, sbw, list_rows(a) * ROW_H, COL_BAR);
    wlwin_fill(w, w->bw - sbw + 2, ty + 1, sbw - 4, th - 2, a->sb_drag ? COL_SEL : lm_theme.button);
}

static void paint_procs(struct app *a, struct wlwin *w)
{
    int pid, user, cpu, mem, st, ni, cmd;
    col_pos(&pid, &user, &cpu, &mem, &st, &ni, &cmd);
    int y = HEAD_H + TOOL_H;
    wlwin_fill(w, 0, y, w->bw, COLH_H, COL_BAR);
    static const char *const names[] = { "PID", "USER", "CPU%", "MEM", "S", "NI", "COMMAND" };
    int xs[] = { pid, user, cpu, mem, st, ni, cmd };
    int keys[] = { S_PID, S_USER, S_CPU, S_MEM, -1, -1, S_NAME };
    for (int i = 0; i < 7; i++) {
        char h[24];
        bool cur = keys[i] == (int)a->sort;
        snprintf(h, sizeof(h), "%s%s", names[i], cur ? (a->reverse == (a->sort == S_CPU || a->sort == S_MEM) ? "^" : "v") : "");
        wlwin_text(w, xs[i], y + 2, h, 1, cur ? lm_theme.accent : COL_TEXT, 0);
    }
    y += COLH_H;
    int rows = list_rows(a), sbw = a->nshown > rows ? lm_metrics.scrollbar_width : 0;
    wlwin_fill(w, 0, y, w->bw, rows * ROW_H, COL_FIELD);
    for (int r = 0; r < rows && a->pv.top + r < a->nshown; r++) {
        int i = a->pv.top + r;
        const struct proc *p = &a->p[a->shown[i]];
        int ry = y + r * ROW_H;
        if (i == a->pv.sel)
            wlwin_fill(w, 0, ry, w->bw - sbw, ROW_H, COL_SEL);
        char t[48], m[16];
        snprintf(t, sizeof(t), "%d", p->pid);
        wlwin_text(w, pid, ry + 1, t, 1, COL_DIM, 8 * LM_FONT_W);
        wlwin_text(w, user, ry + 1, p->user, 1, p->uid == 0 ? lm_theme.warn : COL_TEXT, 12 * LM_FONT_W - 4);
        snprintf(t, sizeof(t), "%.1f", p->cpu);
        wlwin_text(w, cpu, ry + 1, t, 1, p->cpu >= 90 ? lm_theme.error : p->cpu >= 50 ? lm_theme.warn : COL_TEXT, 8 * LM_FONT_W);
        human_kb(p->rss_kb, m, sizeof(m));
        wlwin_text(w, mem, ry + 1, m, 1, p->rss_kb > (long long)a->mem_total_kb / 4 ? lm_theme.warn : COL_TEXT, 8 * LM_FONT_W);
        t[0] = p->state;
        t[1] = 0;
        wlwin_text(w, st, ry + 1, t, 1, p->state == 'Z' ? lm_theme.error : p->state == 'T' ? lm_theme.warn : p->state == 'R' ? lm_theme.ok : COL_DIM, 0);
        snprintf(t, sizeof(t), "%d", p->nice);
        wlwin_text(w, ni, ry + 1, t, 1, p->nice < 0 ? lm_theme.warn : COL_DIM, 5 * LM_FONT_W);
        wlwin_text(w, cmd, ry + 1, p->cmd, 1, p->state == 'T' ? lm_theme.warn : COL_TEXT, w->bw - cmd - sbw - 4);
    }
    paint_scrollbar(a, w, a->nshown, a->pv.top);
}

static void paint_svcs(struct app *a, struct wlwin *w)
{
    int y = HEAD_H + TOOL_H;
    wlwin_fill(w, 0, y, w->bw, COLH_H, COL_BAR);
    int xu = PAD, xa = PAD + 34 * LM_FONT_W, xs = xa + 10 * LM_FONT_W, xd = xs + 10 * LM_FONT_W;
    wlwin_text(w, xu, y + 2, "SERVICE", 1, COL_TEXT, 0);
    wlwin_text(w, xa, y + 2, "STATE", 1, COL_TEXT, 0);
    wlwin_text(w, xs, y + 2, "SUB", 1, COL_TEXT, 0);
    wlwin_text(w, xd, y + 2, "DESCRIPTION", 1, COL_TEXT, 0);
    y += COLH_H;
    int rows = list_rows(a), sbw = a->nsshown > rows ? lm_metrics.scrollbar_width : 0;
    wlwin_fill(w, 0, y, w->bw, rows * ROW_H, COL_FIELD);
    if (!a->ns)
        wlwin_text(w, PAD, y + 4, "No services listed (is systemd running? see the message below)", 1, COL_DIM, w->bw - 2 * PAD);
    for (int r = 0; r < rows && a->sv.top + r < a->nsshown; r++) {
        int i = a->sv.top + r;
        const struct svc *s = &a->s[a->sshown[i]];
        int ry = y + r * ROW_H;
        if (i == a->sv.sel)
            wlwin_fill(w, 0, ry, w->bw - sbw, ROW_H, COL_SEL);
        uint32_t ac = !strcmp(s->active, "failed") ? lm_theme.error : !strcmp(s->active, "active") ? lm_theme.ok : COL_DIM;
        wlwin_text(w, xu, ry + 1, s->unit, 1, COL_TEXT, 34 * LM_FONT_W - 6);
        wlwin_text(w, xa, ry + 1, s->active, 1, ac, 10 * LM_FONT_W - 4);
        wlwin_text(w, xs, ry + 1, s->sub, 1, ac, 10 * LM_FONT_W - 4);
        wlwin_text(w, xd, ry + 1, s->desc, 1, COL_DIM, w->bw - xd - sbw - 4);
    }
    paint_scrollbar(a, w, a->nsshown, a->sv.top);
}

static void paint_log(struct app *a, struct wlwin *w)
{
    int y = HEAD_H + TOOL_H;
    char t[300];
    wlwin_fill(w, 0, y, w->bw, COLH_H, COL_BAR);
    snprintf(t, sizeof(t), "Journal of %s%s   (%d lines; Up / Down / PgUp / PgDn, Left / Right, R reads again, Esc goes back)", a->log_unit,
             a->user_scope ? " (user)" : "", a->nlog);
    wlwin_text(w, PAD, y + 2, t, 1, COL_TEXT, w->bw - 2 * PAD);
    y += COLH_H;
    int rows = list_rows(a);
    wlwin_fill(w, 0, y, w->bw, rows * ROW_H, COL_FIELD);
    a->logtop = LM_CLAMP(a->logtop, 0, LM_MAX(a->nlog - rows, 0));
    for (int r = 0; r < rows && a->logtop + r < a->nlog; r++) {
        const char *line = a->log[a->logtop + r];
        size_t l = strlen(line);
        size_t skip = LM_MIN((size_t)a->logx, l);
        uint32_t col = strcasestr(line, "fail") || strcasestr(line, "error") ? lm_theme.error : strcasestr(line, "warn") ? lm_theme.warn : COL_TEXT;
        wlwin_text_cells(w, PAD, y + r * ROW_H + 1, line + skip, 1, col, w->bw - 2 * PAD);    /* a log: columns, and logx scrolls by characters */
    }
    paint_scrollbar(a, w, a->nlog, a->logtop);
}

static void paint(struct wlwin *w)
{
    struct app *a = w->data;
    wlwin_fill(w, 0, 0, w->bw, w->bh, COL_BG);
    paint_head(a, w);
    wlwin_fill(w, 0, HEAD_H, w->bw, TOOL_H, COL_BG);
    struct btn b[16];
    int n = build_buttons(a, b);
    for (int i = 0; i < n; i++)
        draw_rbtn(w, &b[i], a->hover == i);
    if (a->logview)
        paint_log(a, w);
    else if (a->tab == T_PROCS)
        paint_procs(a, w);
    else
        paint_svcs(a, w);
    int sy = w->bh - STATUS_H;
    wlwin_fill(w, 0, sy, w->bw, STATUS_H, COL_BAR);
    char line[400];
    if (a->confirm != C_NONE) {
        const struct proc *p = selected_proc(a);
        const struct svc *s = selected_svc(a);
        if (a->confirm == C_SVC_STOP)
            snprintf(line, sizeof(line), "Stop %s? Enter = yes, Esc = no", s ? s->unit : "?");
        else
            snprintf(line, sizeof(line), "%s %s (pid %d)? Enter = yes, Esc = no", a->confirm == C_KILL ? "Kill" : "End", p ? p->name : "?", p ? p->pid : 0);
        wlwin_text(w, PAD, sy + 4, line, 1, a->confirm == C_KILL ? lm_theme.error : lm_theme.warn, w->bw - 2 * PAD);
    } else if (a->ffocus || a->filter[0]) {
        wlwin_text(w, PAD, sy + 4, "Filter: ", 1, COL_DIM, 0);
        wltext_paint(w, PAD + 8 * LM_FONT_W, sy + 4, w->bw - 2 * PAD - 8 * LM_FONT_W, a->filter, a->ffocus, COL_TEXT, lm_theme.button_hover, COL_TEXT);
    } else if (a->msg[0] && now_ms() < a->msg_until) {
        wlwin_text(w, PAD, sy + 4, a->msg, 1, lm_theme.caution, w->bw - 2 * PAD);
    } else if (a->tab == T_PROCS) {
        snprintf(line, sizeof(line), "%d of %d processes%s%s   / filters   T end   K kill   S pause   R go on   U mine   C M P N sort   Tab services",
                 a->nshown, a->np, a->mine ? " (yours)" : "", a->paused ? "   REFRESH STOPPED (Space)" : "");
        wlwin_text(w, PAD, sy + 4, line, 1, COL_DIM, w->bw - 2 * PAD);
    } else {
        snprintf(line, sizeof(line), "%d of %d services%s   S start   X stop   R restart   E enable   D disable   Enter log   U %s   Tab processes", a->nsshown,
                 a->ns, a->user_scope ? " (user session)" : "", a->user_scope ? "system" : "user");
        wlwin_text(w, PAD, sy + 4, line, 1, COL_DIM, w->bw - 2 * PAD);
    }
}

/* ---- input ----------------------------------------------------------------------------------------------------------------- */

static struct vlist *cur_view(struct app *a)
{
    return a->tab == T_PROCS ? &a->pv : &a->sv;
}

static int cur_total(const struct app *a)
{
    return a->logview ? a->nlog : a->tab == T_PROCS ? a->nshown : a->nsshown;
}

static void select_row(struct app *a, int row)
{
    struct vlist *v = cur_view(a);
    if (a->logview) {
        a->logtop = LM_CLAMP(row, 0, LM_MAX(a->nlog - list_rows(a), 0));
        a->win.redraw = true;
        return;
    }
    v->sel = LM_CLAMP(row, 0, LM_MAX(cur_total(a) - 1, 0));
    keep_visible(v, list_rows(a));
    if (a->tab == T_PROCS) {
        const struct proc *p = selected_proc(a);
        a->sel_pid = p ? p->pid : 0;
    } else if (a->sv.sel < a->nsshown) {
        snprintf(a->sel_unit, sizeof(a->sel_unit), "%s", a->s[a->sshown[a->sv.sel]].unit);
    }
    a->win.redraw = true;
}

static void open_menu(struct app *a)
{
    if (a->tab == T_PROCS) {
        if (!selected_proc(a))
            return;
        a->menu_kind = 1;
        wlwin_menu(&a->win, "Terminate\nKill\nPause\nContinue\nRenice +5 (lower priority)\nRenice -5 (higher priority)\nCopy command\nCopy PID");
    } else {
        if (!selected_svc(a))
            return;
        a->menu_kind = 2;
        wlwin_menu(&a->win, "Start\nStop\nRestart\nEnable\nDisable\nShow log\nCopy name");
    }
}

static void menu_event(struct wlwin *w, int index)
{
    struct app *a = w->data;
    int kind = a->menu_kind;
    a->menu_kind = 0;
    if (index < 0)
        return;
    if (kind == 1) {
        const struct proc *p = selected_proc(a);
        switch (index) {
        case 0: ask(a, C_TERM); break;
        case 1: ask(a, C_KILL); break;
        case 2: send_signal(a, SIGSTOP, "Paused"); break;
        case 3: send_signal(a, SIGCONT, "Continued"); break;
        case 4: renice(a, 5); break;
        case 5: renice(a, -5); break;
        case 6: if (p) copy_text(a, "the command", p->cmd); break;
        case 7: if (p) { char t[16]; snprintf(t, sizeof(t), "%d", p->pid); copy_text(a, "the PID", t); } break;
        }
    } else if (kind == 2) {
        const struct svc *s = selected_svc(a);
        switch (index) {
        case 0: svc_action(a, "start"); break;
        case 1: ask(a, C_SVC_STOP); break;
        case 2: svc_action(a, "restart"); break;
        case 3: svc_action(a, "enable"); break;
        case 4: svc_action(a, "disable"); break;
        case 5: open_log(a); break;
        case 6: if (s) copy_text(a, "the name", s->unit); break;
        }
    }
    w->redraw = true;
}

static int row_at(const struct app *a, double py)
{
    int y0 = HEAD_H + TOOL_H + COLH_H;
    if (py < y0 || py >= y0 + list_rows(a) * ROW_H)
        return -1;
    return (int)((py - y0) / ROW_H);
}

static void pointer_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    int sbw = lm_metrics.scrollbar_width;
    switch (op) {
    case LM_WL_POINTER_EVT_ENTER:
        a->px = lm_fixed_to_double(args[2].f);
        a->py = lm_fixed_to_double(args[3].f);
        break;
    case LM_WL_POINTER_EVT_LEAVE:
        a->hover = -1;
        a->sb_drag = false;
        w->redraw = true;
        break;
    case LM_WL_POINTER_EVT_MOTION: {
        a->px = lm_fixed_to_double(args[1].f);
        a->py = lm_fixed_to_double(args[2].f);
        int h = button_at(a, a->px, a->py);
        if (h != a->hover) {
            a->hover = h;
            w->redraw = true;
        }
        if (a->sb_drag) {
            int total = cur_total(a), rows = list_rows(a), ty, th;
            if (scrollbar_geom(a, total, a->logview ? a->logtop : cur_view(a)->top, &ty, &th)) {
                int area = rows * ROW_H - th, y0 = HEAD_H + TOOL_H + COLH_H;
                int top = area > 0 ? (int)((a->py - a->sb_grab - y0) * (total - rows) / area) : 0;
                top = LM_CLAMP(top, 0, LM_MAX(total - rows, 0));
                if (a->logview)
                    a->logtop = top;
                else
                    cur_view(a)->top = top;
                w->redraw = true;
            }
        }
        break;
    }
    case LM_WL_POINTER_EVT_BUTTON: {
        a->clip.serial = args[0].u;
        bool left = args[2].u == 272, right = args[2].u == 273, down = args[3].u == 1;
        if (!down) {
            a->sb_drag = false;
            break;
        }
        a->confirm = C_NONE;
        int bi = button_at(a, a->px, a->py);
        if (left && bi >= 0) {
            struct btn b[16];
            build_buttons(a, b);
            if (b[bi].enabled)
                press_button(a, b[bi].id);
            break;
        }
        int total = cur_total(a), ty, th;
        if (left && a->px >= w->bw - sbw && scrollbar_geom(a, total, a->logview ? a->logtop : cur_view(a)->top, &ty, &th) && a->py >= HEAD_H + TOOL_H + COLH_H &&
            a->py < HEAD_H + TOOL_H + COLH_H + list_rows(a) * ROW_H) {
            if (a->py >= ty && a->py < ty + th) {
                a->sb_drag = true;
                a->sb_grab = (int)a->py - ty;
            } else {                                                    /* beside the thumb: a page */
                int page = list_rows(a) - 1;
                if (a->logview)
                    select_row(a, a->logtop + (a->py < ty ? -page : page));
                else {
                    cur_view(a)->top = LM_CLAMP(cur_view(a)->top + (a->py < ty ? -page : page), 0, LM_MAX(total - list_rows(a), 0));
                    w->redraw = true;
                }
            }
            break;
        }
        if (a->py >= HEAD_H + TOOL_H && a->py < HEAD_H + TOOL_H + COLH_H && left && a->tab == T_PROCS && !a->logview) {    /* the column heads sort */
            int k = sort_column_at((int)a->px);
            if (k >= 0)
                set_sort(a, (enum sortkey)k);
            break;
        }
        int r = row_at(a, a->py);
        if ((left || right) && r >= 0 && !a->logview) {
            struct vlist *v = cur_view(a);
            int i = v->top + r;
            if (i < total) {
                select_row(a, i);
                uint32_t t = args[1].u;
                if (left && a->tab == T_SVCS && t - a->last_click_ms < (uint32_t)lm_metrics.double_click_ms && a->last_click_row == i) {
                    open_log(a);
                    a->last_click_ms = 0;
                } else {
                    a->last_click_ms = t;
                    a->last_click_row = i;
                }
                if (right)
                    open_menu(a);
            }
        }
        break;
    }
    case LM_WL_POINTER_EVT_AXIS:
        if (args[1].u == 0) {
            double v = lm_fixed_to_double(args[2].f);
            int step = v > 0 ? 3 : v < 0 ? -3 : 0;
            if (a->logview)
                select_row(a, a->logtop + step);
            else {
                struct vlist *vl = cur_view(a);
                vl->top = LM_CLAMP(vl->top + step, 0, LM_MAX(cur_total(a) - list_rows(a), 0));
                w->redraw = true;
            }
        }
        break;
    }
}

static void keyboard_event(struct wlwin *w, uint32_t op, const union lm_arg *args)
{
    struct app *a = w->data;
    if (op != LM_WL_KEYBOARD_EVT_KEY) {
        return;
    }
    a->clip.serial = args[0].u;
    if (args[3].u == 0)
        return;
    uint32_t ks = lm_keymap_keysym(args[2].u, w->mods);
    bool ctrl = (w->mods & LM_MOD_CTRL) != 0, shift = (w->mods & LM_MOD_SHIFT) != 0, alt = (w->mods & LM_MOD_ALT) != 0;
    char txt[8];
    int tl = lm_keysym_to_utf8(ks, txt);
    char c = tl == 1 && !ctrl && !alt ? txt[0] : 0;
    w->redraw = true;

    if (a->confirm != C_NONE) {                                         /* a question: only Enter / Y and Esc / N answer it */
        if (ks == LM_KS_Return || ks == LM_KS_KP_Enter || c == 'y' || c == 'Y')
            do_confirmed(a);
        else
            a->confirm = C_NONE;
        return;
    }
    if (a->ffocus) {                                                    /* the filter field */
        if (ks == LM_KS_Escape) {
            a->filter[0] = 0;
            a->ffocus = false;
        } else if (ks == LM_KS_Return || ks == LM_KS_KP_Enter || ks == LM_KS_Down || ks == LM_KS_Tab) {
            a->ffocus = false;
        } else {
            wltext_edit(&a->clip, a->filter, sizeof(a->filter), ks, w->mods);
        }
        if (a->tab == T_PROCS)
            rebuild_procs(a);
        else
            rebuild_svcs(a);
        return;
    }
    if (a->logview) {
        int page = LM_MAX(list_rows(a) - 1, 1);
        if (ks == LM_KS_Escape || ks == LM_KS_BackSpace || c == 'q' || c == 'Q')
            a->logview = false;
        else if (ks == LM_KS_Down)
            select_row(a, a->logtop + 1);
        else if (ks == LM_KS_Up)
            select_row(a, a->logtop - 1);
        else if (ks == LM_KS_Page_Down || c == ' ')
            select_row(a, a->logtop + page);
        else if (ks == LM_KS_Page_Up)
            select_row(a, a->logtop - page);
        else if (ks == LM_KS_Home)
            select_row(a, 0);
        else if (ks == LM_KS_End)
            select_row(a, a->nlog);
        else if (ks == LM_KS_Right)
            a->logx += 8;
        else if (ks == LM_KS_Left)
            a->logx = LM_MAX(a->logx - 8, 0);
        else if (c == 'r' || c == 'R')
            read_log(a);
        return;
    }
    int page = LM_MAX(list_rows(a) - 1, 1);
    struct vlist *v = cur_view(a);
    if (ctrl && (c == 0) && (ks == 'f' || ks == 'F')) {
        a->ffocus = true;
        wltext_end(a->filter);
        return;
    }
    if (ks == LM_KS_Escape) {
        if (a->filter[0])
            a->filter[0] = 0, a->tab == T_PROCS ? rebuild_procs(a) : rebuild_svcs(a);
        else
            w->running = false;
        return;
    }
    switch (ks) {
    case LM_KS_Up: select_row(a, v->sel - 1); return;
    case LM_KS_Down: select_row(a, v->sel + 1); return;
    case LM_KS_Page_Up: select_row(a, v->sel - page); return;
    case LM_KS_Page_Down: select_row(a, v->sel + page); return;
    case LM_KS_Home: select_row(a, 0); return;
    case LM_KS_End: select_row(a, cur_total(a) - 1); return;
    case LM_KS_Tab: switch_tab(a, a->tab == T_PROCS ? T_SVCS : T_PROCS); return;
    case LM_KS_Delete: if (a->tab == T_PROCS) ask(a, shift ? C_KILL : C_TERM); return;
    case LM_KS_Return:
    case LM_KS_KP_Enter: if (a->tab == T_SVCS) open_log(a); return;
    default: break;
    }
    if (ctrl)
        return;
    switch (c) {
    case '/': a->ffocus = true; wltext_end(a->filter); return;
    case 'q': case 'Q': w->running = false; return;
    case '1': switch_tab(a, T_PROCS); return;
    case '2': switch_tab(a, T_SVCS); return;
    case ' ': if (a->tab == T_PROCS) { a->paused = !a->paused; if (!a->paused) a->next_refresh = 0; } return;
    default: break;
    }
    if (a->tab == T_PROCS) {
        switch (c) {
        case 'c': case 'C': set_sort(a, S_CPU); break;
        case 'm': case 'M': set_sort(a, S_MEM); break;
        case 'p': case 'P': set_sort(a, S_PID); break;
        case 'n': case 'N': set_sort(a, S_NAME); break;
        case 'o': case 'O': set_sort(a, S_USER); break;
        case 'u': case 'U': a->mine = !a->mine; rebuild_procs(a); break;
        case 't': case 'T': ask(a, C_TERM); break;
        case 'k': case 'K': ask(a, C_KILL); break;
        case 's': case 'S': send_signal(a, SIGSTOP, "Paused"); break;
        case 'r': case 'R': send_signal(a, SIGCONT, "Continued"); break;
        case '+': case '=': renice(a, 5); break;
        case '-': case '_': renice(a, -5); break;
        }
    } else {
        switch (c) {
        case 's': case 'S': svc_action(a, "start"); break;
        case 'x': case 'X': ask(a, C_SVC_STOP); break;
        case 'r': case 'R': svc_action(a, "restart"); break;
        case 'e': case 'E': svc_action(a, "enable"); break;
        case 'd': case 'D': svc_action(a, "disable"); break;
        case 'l': case 'L': open_log(a); break;
        case 'u': case 'U': press_button(a, B_SCOPE); break;
        case 'g': case 'G': read_services(a); break;
        }
    }
}

static void global_event(struct wlwin *w, uint32_t name, const char *iface, uint32_t ver)
{
    struct app *a = w->data;
    wlclip_global(&a->clip, w, name, iface, ver);
}

/* ---- main ----------------------------------------------------------------------------------------------------------------- */

static int selftest(void)
{
    int bad = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "mellagui-top: selftest failed: %s (line %d)\n", #cond, __LINE__); bad++; } } while (0)
    int pid = 0;
    struct pstat st;
    CHECK(parse_proc_stat("1234 (my (odd) prog) S 1 1234 1234 0 -1 4194560 100 0 0 0 150 50 0 0 20 -5 7 0 98765 123456789 2500 18446744073709551615", &pid, &st));
    CHECK(pid == 1234 && !strcmp(st.name, "my (odd) prog") && st.state == 'S' && st.ppid == 1 && st.ticks == 200 && st.nice == -5 && st.threads == 7 &&
          st.start == 98765 && st.rss_pages == 2500);
    CHECK(!parse_proc_stat("garbage", &pid, &st));
    CHECK(!parse_proc_stat("12 (x) S", &pid, &st));
    char b[16];
    human_kb(512, b, sizeof(b));
    CHECK(!strcmp(b, "512K"));
    human_kb(1536, b, sizeof(b));
    CHECK(!strcmp(b, "1.5M"));
    human_kb(20 << 10, b, sizeof(b));
    CHECK(!strcmp(b, "20M"));
    human_kb(3LL << 20, b, sizeof(b));
    CHECK(!strcmp(b, "3.0G"));
    struct svc s;
    CHECK(parse_unit_line("nginx.service                loaded    active   running A high performance web server", &s));
    CHECK(!strcmp(s.unit, "nginx.service") && !strcmp(s.active, "active") && !strcmp(s.sub, "running") && !strcmp(s.desc, "A high performance web server"));
    CHECK(parse_unit_line("foo.service loaded failed failed Foo", &s) && !strcmp(s.active, "failed"));
    CHECK(parse_unit_line("ghost.service not-found inactive dead ghost.service", &s) && !strcmp(s.load, "not-found"));
    CHECK(!parse_unit_line("LOAD   = Reflects whether the unit definition was properly loaded.", &s));
    CHECK(!parse_unit_line("", &s));
    CHECK(contains_ci("Firefox", "FIRE") && !contains_ci("Firefox", "chrome") && contains_ci("x", ""));
    CHECK(rank_active("failed") < rank_active("active") && rank_active("active") < rank_active("inactive"));
    if (!bad)
        puts("mellagui-top selftest ok");
    return bad ? 1 : 0;
}

static int list_cmd(int n)
{
    static struct app a;
    a.ncores = 1;
    read_machine(&a);
    read_procs(&a);
    usleep(600 * 1000);
    read_machine(&a);
    read_procs(&a);
    a.sort = S_CPU;
    rebuild_procs(&a);
    printf("%d processes, cpu %d%%, %d cores\n", a.np, a.cpu_pct, a.ncores);
    printf("%7s %-12s %6s %8s %s\n", "PID", "USER", "CPU%", "MEM", "COMMAND");
    for (int i = 0; i < a.nshown && i < n; i++) {
        const struct proc *p = &a.p[a.shown[i]];
        char m[16];
        human_kb(p->rss_kb, m, sizeof(m));
        printf("%7d %-12s %6.1f %8s %s\n", p->pid, p->user, p->cpu, m, p->cmd);
    }
    return 0;
}

int main(int argc, char **argv)
{
    lm_theme_load_env();
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();
    if (argc > 1 && !strcmp(argv[1], "--list"))
        return list_cmd(argc > 2 ? atoi(argv[2]) : 15);
    if (argc > 1) {
        fprintf(stderr, "usage: mellagui-top [--list [N] | --selftest]\n");
        return 2;
    }
    static const struct wlwin_ops ops = { .paint = paint, .global = global_event, .pointer = pointer_event, .keyboard = keyboard_event, .menu = menu_event };
    static struct app a;
    a.ncores = 1;
    a.hover = -1;
    a.sort = S_CPU;
    if (wlwin_connect(&a.win, "mellagui-top", &ops, &a) < 0)
        return 1;
    if (wlwin_open(&a.win, "Processes", "mellagui.top", 860, 560, 560, 320) < 0)
        return 1;
    wlclip_attach(&a.clip, &a.win);
    refresh(&a);
    int64_t t0 = now_ms();
    (void)t0;
    while (a.win.running) {
        int64_t now = now_ms();
        int timeout = a.paused ? -1 : (int)LM_CLAMP(a.next_refresh - now, 0, 250);
        if (!wlwin_step(&a.win, timeout))
            break;
        if (!a.paused && !a.logview && now_ms() >= a.next_refresh && a.confirm == C_NONE)
            refresh(&a);
        else if (!a.paused && a.next_refresh <= now_ms())
            a.next_refresh = now_ms() + REFRESH_MS;
    }
    wlwin_destroy(&a.win);
    return 0;
}
