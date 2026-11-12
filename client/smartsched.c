#include "smartsched.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "json.h"
#include "tuya.h"

static bool sched_path(char *out, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    int w;
    if (xdg && *xdg)
        w = snprintf(out, n, "%s/mellagui/smart-schedule.conf", xdg);
    else if (home && *home)
        w = snprintf(out, n, "%s/.config/mellagui/smart-schedule.conf", home);
    else
        return false;
    return w > 0 && (size_t)w < n;
}

bool sched_parse(const char *line, struct sched_rule *r)
{
    char state[8], days[16];
    memset(r, 0, sizeof(*r));
    if (sscanf(line, "%7s %d:%d %15s %39s %39s %23s", state, &r->hour, &r->min, days, r->dev, r->code, r->value) != 7)
        return false;
    if ((strcmp(state, "on") && strcmp(state, "off")) || r->hour < 0 || r->hour > 23 || r->min < 0 || r->min > 59 || strlen(days) != 7)
        return false;
    r->on = !strcmp(state, "on");
    for (int i = 0; i < 7; i++)
        if (days[i] != '-')
            r->days |= 1u << i;
    return true;
}

int sched_load(struct sched_rule *r, int max)
{
    char path[1024], line[256];
    int n = 0;
    if (!sched_path(path, sizeof(path)))
        return 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    while (n < max && fgets(line, sizeof(line), f))
        if (line[0] != '#' && sched_parse(line, &r[n]))
            n++;
    fclose(f);
    return n;
}

bool sched_save(const struct sched_rule *r, int n)
{
    char path[1024], tmp[1100];
    if (!sched_path(path, sizeof(path)))
        return false;
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        mkdir(path, 0700);
        *p = '/';
    }
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f)
        return false;
    fprintf(f, "# mellagui-smart: its timetable (on|off, the time, the days from Monday, the device, what it is told). Written by the program itself.\n");
    for (int i = 0; i < n; i++) {
        char days[8];
        for (int d = 0; d < 7; d++)
            days[d] = r[i].days & (1u << d) ? "MTWTFSS"[d] : '-';
        days[7] = 0;
        fprintf(f, "%s %02d:%02d %s %s %s %s\n", r[i].on ? "on" : "off", r[i].hour, r[i].min, days, r[i].dev, r[i].code, r[i].value);
    }
    if (fclose(f) != 0 || rename(tmp, path) < 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

void sched_days_text(unsigned days, char *out, size_t n)
{
    static const char *const names[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    days &= 0x7f;
    if (days == 0x7f) {
        snprintf(out, n, "every day");
    } else if (days == 0x1f) {
        snprintf(out, n, "weekdays");
    } else if (days == 0x60) {
        snprintf(out, n, "weekends");
    } else if (!days) {
        snprintf(out, n, "never");
    } else {
        size_t o = 0;
        out[0] = 0;
        for (int i = 0; i < 7 && o + 5 < n; i++)
            if (days & (1u << i))
                o += (size_t)snprintf(out + o, n - o, "%s%s", o ? " " : "", names[i]);
    }
}

/* ---- the process that carries the rules out ---------------------------------------------------------------------------------------------- */

static void exe_sibling(const char *name, char *out, size_t n)
{
    char exe[1024];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    out[0] = 0;
    if (k <= 0)
        return;
    exe[k] = 0;
    char *slash = strrchr(exe, '/');
    if (!slash)
        return;
    *slash = 0;
    snprintf(out, n, "%s/%s", exe, name);
}

/* Runs a program without waiting for it (forked twice, so nobody has to reap it). */
static void run_detached(const char *path, const char *a1, const char *a2)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            execl(path, path, a1, a2, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

void sched_spawn_background(void)
{
    char exe[1024];
    if (getenv("MELLAGUI_SMART_NO_BACKGROUND"))
        return;
    ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (k <= 0)
        return;
    exe[k] = 0;
    run_detached(exe, "--background", NULL);
}

static void notify(const char *body)
{
    char prog[1100];
    exe_sibling("mellagui-notify", prog, sizeof(prog));
    if (prog[0] && access(prog, X_OK) == 0 && getenv("WAYLAND_DISPLAY"))
        run_detached(prog, "Smart home", body);
}

static bool session_gone(void)
{
    const char *rt = getenv("XDG_RUNTIME_DIR"), *disp = getenv("WAYLAND_DISPLAY");
    char path[600];
    if (!rt || !disp || !*disp)
        return false;
    if (disp[0] == '/')
        snprintf(path, sizeof(path), "%s", disp);
    else
        snprintf(path, sizeof(path), "%s/%s", rt, disp);
    return access(path, F_OK) != 0;
}

struct fired {
    char key[160];
    long stamp;                         /* the minute it belongs to */
    int tries;
    bool done;
};

int sched_background(void)
{
    const char *rt = getenv("XDG_RUNTIME_DIR"), *te = getenv("MELLAGUI_SMART_TICK_MS");
    char lock[600];
    snprintf(lock, sizeof(lock), "%s/mellagui-smart.duty", rt && *rt ? rt : "/tmp");
    int lfd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lfd < 0 || flock(lfd, LOCK_EX | LOCK_NB) != 0)
        return 0;                                               /* another one is at it */
    int tick_ms = te && atoi(te) >= 20 ? atoi(te) : 15000;
    static struct fired fired[SCHED_MAX];
    static struct sched_rule rules[SCHED_MAX];
    int nfired = 0;
    for (;;) {
        int n = sched_load(rules, SCHED_MAX), enabled = 0;
        for (int i = 0; i < n; i++)
            enabled += rules[i].on;
        if (!enabled || session_gone())
            return 0;
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        const char *now = getenv("MELLAGUI_SMART_NOW");
        int h = tm.tm_hour, m = tm.tm_min;
        if (now && sscanf(now, "%d:%d", &h, &m) != 2) {
            h = tm.tm_hour;
            m = tm.tm_min;
        }
        long stamp = ((long)tm.tm_year * 366 + tm.tm_yday) * 1440 + h * 60 + m;
        unsigned today = 1u << ((tm.tm_wday + 6) % 7);
        for (int i = 0; i < n; i++) {
            struct sched_rule *r = &rules[i];
            char key[160], err[200] = "", json[64], *end;
            if (!r->on || r->hour != h || r->min != m || !(r->days & today))
                continue;
            snprintf(key, sizeof(key), "%s %s %s", r->dev, r->code, r->value);
            struct fired *f = NULL;
            for (int k = 0; k < nfired; k++)
                if (!strcmp(fired[k].key, key) && fired[k].stamp == stamp)
                    f = &fired[k];
            if (f && f->done)
                continue;
            if (!f) {
                f = &fired[nfired < SCHED_MAX ? nfired++ : 0];  /* (a full table starts over: the stamps tell old entries apart) */
                snprintf(f->key, sizeof(f->key), "%s", key);
                f->stamp = stamp;
                f->tries = 0;
                f->done = false;
            }
            static struct tuya_session s;
            strtod(r->value, &end);
            if (!strcmp(r->value, "true") || !strcmp(r->value, "false") || (r->value[0] && !*end))
                snprintf(json, sizeof(json), "%s", r->value);
            else
                json_quote(r->value, json, sizeof(json));
            bool ok = tuya_session_load(&s) && tuya_command(&s, r->dev, r->code, json, err, sizeof(err));
            if (!ok && !err[0])
                snprintf(err, sizeof(err), "not logged in");
            f->tries++;
            printf("%02d:%02d %s %s = %s: %s\n", h, m, r->dev, r->code, json, ok ? "sent" : err);
            fflush(stdout);
            if (ok) {
                f->done = true;
            } else if (f->tries >= 3) {
                char body[300];
                snprintf(body, sizeof(body), "The %02d:%02d command (%s) did not go out: %s", h, m, r->code, err);
                notify(body);
                f->done = true;
            }
        }
        struct timespec ts = { .tv_sec = tick_ms / 1000, .tv_nsec = (long)(tick_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
}

int sched_selftest(void)
{
    int bad = 0;
    struct sched_rule r;
    char text[64];
#define CHECK(cond, what) do { if (!(cond)) { fprintf(stderr, "smartsched selftest: %s\n", what); bad++; } } while (0)
    CHECK(sched_parse("on 07:30 MTWTF-- dev1 percent_control 30\n", &r) && r.on && r.hour == 7 && r.min == 30 && r.days == 0x1f && !strcmp(r.dev, "dev1") &&
          !strcmp(r.code, "percent_control") && !strcmp(r.value, "30"), "a rule line");
    CHECK(sched_parse("off 23:05 -----SS d switch_1 false", &r) && !r.on && r.days == 0x60, "an off rule");
    CHECK(!sched_parse("on 24:00 MTWTFSS d c v", &r) && !sched_parse("on 07:30 MTW d c v", &r) && !sched_parse("maybe 07:30 MTWTFSS d c v", &r) &&
          !sched_parse("on 07:30 MTWTFSS d c", &r), "bad lines are accepted");
    sched_days_text(0x7f, text, sizeof(text));
    CHECK(!strcmp(text, "every day"), "every day");
    sched_days_text(0x1f, text, sizeof(text));
    CHECK(!strcmp(text, "weekdays"), "weekdays");
    sched_days_text(0x15, text, sizeof(text));
    CHECK(!strcmp(text, "Mon Wed Fri"), "single days");
#undef CHECK
    return bad;
}
