#include "cctvalarm.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "dvrip.h"

#define MAX_WATCHED 8

void cctv_wants_load(const char *path, const char *const *name, struct cctv_want *want, int n)
{
    memset(want, 0, (size_t)n * sizeof(*want));
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *save = NULL, *w = strtok_r(line, " \t\r\n", &save);
        if (!w || *w == '#')
            continue;
        int i = 0;
        while (i < n && strcmp(name[i], w))
            i++;
        while (i < n && (w = strtok_r(NULL, " \t\r\n", &save))) {
            if (!strcmp(w, "notify"))
                want[i].notify = true;
            else if (!strcmp(w, "record"))
                want[i].record = true;
        }
    }
    fclose(f);
}

bool cctv_wants_save(const char *path, const char *const *name, const struct cctv_want *want, int n)
{
    char tmp[PATH_MAX + 16], dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        mkdir(dir, 0700);
    }
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) {
        if (fd >= 0)
            close(fd);
        return false;
    }
    fprintf(f, "# What mellagui-cctv --watch does when a camera raises an alarm: NAME [notify] [record]\n");
    for (int i = 0; i < n; i++)
        if (want[i].notify || want[i].record)
            fprintf(f, "%s%s%s\n", name[i], want[i].notify ? " notify" : "", want[i].record ? " record" : "");
    bool ok = !ferror(f);
    ok = fclose(f) == 0 && ok && rename(tmp, path) == 0;
    return ok;
}

struct watched {
    struct cctv_target t;
    struct dv_session s;
    struct dv_packets packets;
    struct cctv_job *rec;
    int64_t next_try, next_alive, rec_end, rec_start;
    int64_t told_motion, told_other;
    int backoff;
    bool refused;
};

static volatile sig_atomic_t g_quit;
static void on_quit(int sig) { (void)sig; g_quit = 1; }

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void notify(const char *title, const char *body)
{
    const char *own = getenv("MELLAGUI_CCTV_NOTIFY");
    char prog[PATH_MAX + 32] = "mellagui-notify";
    if (own && !strcmp(own, "none"))
        return;
    if (own && *own) {
        snprintf(prog, sizeof(prog), "%s", own);
    } else {
        ssize_t n = readlink("/proc/self/exe", prog, PATH_MAX - 1);
        char *slash = n > 0 ? (prog[n] = 0, strrchr(prog, '/')) : NULL;
        strcpy(slash ? slash + 1 : prog, "mellagui-notify");
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            execlp(prog, prog, title, body, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

struct heard {
    struct watched *w;
    const struct cctv_want *want;
    const char *dir, *ffmpeg;
    int after_ms;
};

static void on_message(void *ud, int msg, const uint8_t *body, size_t len)
{
    struct heard *h = ud;
    struct watched *w = h->w;
    char event[64];
    bool begins;
    if (msg != DV_MSG_ALARM || !dv_alarm_parse((const char *)body, len, event, sizeof(event), &begins))
        return;
    bool human = strstr(event, "Human") || strstr(event, "human"), motion = human || strstr(event, "Motion") || strstr(event, "motion");
    int64_t now = now_ms();
    if (motion && h->want->record) {
        if (begins && !w->rec) {
            w->rec = cctv_job_start(&w->t, CCTV_RECORD, h->dir, h->ffmpeg);
            w->rec_start = now;
        }
        if (w->rec)
            w->rec_end = now + h->after_ms;
    }
    if (!begins || !h->want->notify)
        return;
    int64_t *told = motion ? &w->told_motion : &w->told_other;
    if (*told && now - *told < CCTV_NOTIFY_EVERY_MS)
        return;
    *told = now;
    char title[96], text[160];
    snprintf(title, sizeof(title), "Camera %s", w->t.name);
    if (motion)
        snprintf(text, sizeof(text), "%s%s", human ? "A person is moving" : "Something is moving", h->want->record ? " - recording" : "");
    else
        snprintf(text, sizeof(text), "Alarm: %s", event);
    notify(title, text);
}

static void drop(struct watched *w, int64_t now)
{
    dv_close(&w->s);
    w->packets.len = 0;
    w->backoff = w->backoff ? (w->backoff * 2 > 60000 ? 60000 : w->backoff * 2) : 2000;
    w->next_try = now + w->backoff;
}

int cctv_watch(const struct cctv_target *t, int n, const char *wants_path, const char *dir, const char *ffmpeg)
{
    const char *run = getenv("XDG_RUNTIME_DIR"), *display = getenv("WAYLAND_DISPLAY");
    if (!run || !*run) {
        fprintf(stderr, "mellagui-cctv: XDG_RUNTIME_DIR is not set\n");
        return 1;
    }
    char lock[PATH_MAX], socket_path[PATH_MAX] = "";
    snprintf(lock, sizeof(lock), "%s/mellagui-cctv.watch", run);
    int lfd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lfd < 0 || flock(lfd, LOCK_EX | LOCK_NB) < 0)
        return 1;                           /* another one watches: it reads the file anew by itself */
    if (display && *display)
        snprintf(socket_path, sizeof(socket_path), "%s%s%s", display[0] == '/' ? "" : run, display[0] == '/' ? "" : "/", display);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = on_quit };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    if (n > MAX_WATCHED)
        n = MAX_WATCHED;
    static struct watched w[MAX_WATCHED];
    const char *names[MAX_WATCHED];
    struct cctv_want want[MAX_WATCHED];
    for (int i = 0; i < n; i++) {
        w[i].t = t[i];
        names[i] = t[i].name;
        dv_init(&w[i].s);
    }
    const char *env = getenv("MELLAGUI_CCTV_AFTER_MS");
    int after = env && atoi(env) > 0 ? atoi(env) : CCTV_AFTER_MS;
    static uint8_t buf[65536];
    while (!g_quit) {
        cctv_wants_load(wants_path, names, want, n);
        struct stat st;
        if (socket_path[0] && stat(socket_path, &st) != 0)
            break;
        bool anything = false;
        int64_t now = now_ms();
        for (int i = 0; i < n; i++) {
            bool wanted = want[i].notify || (want[i].record && ffmpeg);
            anything |= wanted;
            if (w[i].rec) {
                struct cctv_status rs;
                cctv_job_status(w[i].rec, &rs);
                if (rs.state == CCTV_DONE || rs.state == CCTV_FAILED) {
                    cctv_job_free(w[i].rec);
                    w[i].rec = NULL;
                } else if (!want[i].record || now >= w[i].rec_end || now - w[i].rec_start >= CCTV_LONGEST_MS) {
                    cctv_job_stop(w[i].rec);
                }
            }
            if (!wanted) {
                dv_close(&w[i].s);
                continue;
            }
            if (w[i].s.ctl >= 0 || w[i].refused || now < w[i].next_try)
                continue;
            char err[160] = "";
            int r = dv_login(&w[i].s, w[i].t.host, w[i].t.port, w[i].t.user, w[i].t.password, -1, err, sizeof(err));
            if (r > 0) {
                w[i].refused = true;
                char title[96];
                snprintf(title, sizeof(title), "Camera %s is not watched", w[i].t.name);
                notify(title, err);
                continue;
            }
            if (r != 0 || dv_alarm_subscribe(&w[i].s, -1) != 0) {
                drop(&w[i], now_ms());
                continue;
            }
            w[i].packets.len = 0;
            w[i].next_alive = now_ms() + w[i].s.alive_s * 500;
            fcntl(w[i].s.ctl, F_SETFL, O_NONBLOCK);
        }
        bool recording = false;
        for (int i = 0; i < n; i++)
            recording |= w[i].rec != NULL;
        if (!anything && !recording)
            break;
        struct pollfd p[MAX_WATCHED];
        for (int i = 0; i < n; i++)
            p[i] = (struct pollfd){ .fd = w[i].s.ctl, .events = POLLIN };
        if (poll(p, (nfds_t)n, 500) < 0 && errno != EINTR)
            break;
        now = now_ms();
        for (int i = 0; i < n; i++) {
            if (w[i].s.ctl < 0)
                continue;
            if (p[i].revents) {
                ssize_t k = recv(w[i].s.ctl, buf, sizeof(buf), 0);
                if (k > 0) {
                    struct heard h = { .w = &w[i], .want = &want[i], .dir = dir, .ffmpeg = ffmpeg };
                    h.after_ms = after;
                    if (!dv_packets_feed(&w[i].packets, buf, (size_t)k, on_message, &h)) {
                        drop(&w[i], now);
                        continue;
                    }
                    w[i].backoff = 0;
                } else if (!(k < 0 && (errno == EAGAIN || errno == EINTR))) {
                    drop(&w[i], now);
                    continue;
                }
            }
            if (now >= w[i].next_alive) {
                w[i].next_alive = now + w[i].s.alive_s * 500;
                if (dv_keepalive(&w[i].s) < 0)
                    drop(&w[i], now);
            }
        }
    }
    for (int i = 0; i < n; i++)
        if (w[i].rec)
            cctv_job_stop(w[i].rec);
    for (int i = 0; i < n; i++) {
        cctv_job_free(w[i].rec);
        w[i].rec = NULL;
        dv_close(&w[i].s);
        dv_packets_free(&w[i].packets);
    }
    close(lfd);
    return 0;
}
