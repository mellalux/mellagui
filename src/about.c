/*
 * The About dialog's system information. See about.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "about.h"
#include "backend.h"
#include "output.h"
#include "render.h"
#include "server.h"

static void add(struct lm_about_row *rows, int *n, int max, const char *label, const char *fmt, ...) __attribute__((format(printf, 5, 6)));

static void add(struct lm_about_row *rows, int *n, int max, const char *label, const char *fmt, ...)
{
    if (*n >= max)
        return;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(rows[*n].value, sizeof(rows[*n].value), fmt, ap);
    __builtin_va_end(ap);
    snprintf(rows[*n].label, sizeof(rows[*n].label), "%s", label);
    (*n)++;
}

/* The value of `key` (like "model name") in a /proc file of "key : value" lines, copied to out; false when absent. */
static bool proc_value(const char *path, const char *key, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[256];
    size_t kl = strlen(key);
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, kl))
            continue;
        const char *p = line + kl;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != ':')
            continue;
        p++;
        while (*p == ' ' || *p == '\t')
            p++;
        snprintf(out, n, "%s", p);
        out[strcspn(out, "\n")] = 0;
        found = true;
    }
    fclose(f);
    return found;
}

int lm_about_collect(struct lm_server *srv, struct lm_about_row *rows, int max)
{
    int n = 0;
    char buf[160];

    if (gethostname(buf, sizeof(buf)) == 0 && buf[0]) {
        buf[sizeof(buf) - 1] = 0;
        add(rows, &n, max, "Computer", "%s", buf);
    }
    const char *user = getenv("USER");
    add(rows, &n, max, "User", "%s", user && *user ? user : "?");
    struct utsname un;
    if (uname(&un) == 0)
        add(rows, &n, max, "System", "%s %s %s", un.sysname, un.release, un.machine);

    bool fixed = getenv("MELLAGUI_ABOUT_STATIC") != NULL;       /* tests: no rows that change from one run to the next */
    double up = 0;
    FILE *f = fixed ? NULL : fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &up) == 1) {
            long s = (long)up, d = s / 86400, h = s % 86400 / 3600, m = s % 3600 / 60;
            if (d > 0)
                add(rows, &n, max, "Uptime", "%ld d %ld h %ld min", d, h, m);
            else if (h > 0)
                add(rows, &n, max, "Uptime", "%ld h %ld min", h, m);
            else
                add(rows, &n, max, "Uptime", "%ld min", m);
        }
        fclose(f);
    }
    if (proc_value("/proc/cpuinfo", "model name", buf, sizeof(buf))) {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        add(rows, &n, max, "Processor", "%.46s (%ld)", buf, cpus > 0 ? cpus : 1);
    }
    char total[64], avail[64];
    if (!fixed && proc_value("/proc/meminfo", "MemTotal", total, sizeof(total)) && proc_value("/proc/meminfo", "MemAvailable", avail, sizeof(avail))) {
        long t = atol(total), a = atol(avail);
        add(rows, &n, max, "Memory", "%.1f of %.1f GiB in use", (double)(t - a) / 1048576.0, (double)t / 1048576.0);
    }

    struct lm_output *o = srv->backend ? lm_output_primary(srv) : NULL;
    if (o)
        add(rows, &n, max, "Display", "%s  %dx%d @ %.0f Hz", o->name, o->width, o->height, (double)o->refresh_mhz / 1000.0);
    if (!fixed)
        add(rows, &n, max, "Graphics", "%s renderer, %s backend", srv->render && lm_render_gl(srv->render) ? "OpenGL" : "software",
        srv->config.backend);
    const char *wl = getenv("WAYLAND_DISPLAY");
    if (wl && *wl && !fixed)
        add(rows, &n, max, "Session", "Wayland, %s", wl);
    const char *conf = srv->config.conf_path;
    if (!fixed)
        add(rows, &n, max, "Config", "%.56s", conf && *conf && strcmp(conf, "none") ? conf : "(none, built-in defaults)");
    add(rows, &n, max, "Build", "%s", __DATE__);
    return n;
}
