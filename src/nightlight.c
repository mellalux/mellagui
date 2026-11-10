#include "nightlight.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "backend.h"
#include "log.h"
#include "loop.h"
#include "server.h"
#include "settings.h"

#define TICK_MS 100
#define CHECK_MS 20000
#define FADE_USER_MS 1500
#define FADE_SCHEDULE_MS 30000

struct lm_night {
    struct lm_server *srv;
    struct lm_timer *timer;
    double level;           /* what is shown now, 0..1 */
    double target;          /* 0 or 1 */
    double step;            /* level change per tick */
    bool sched_target;      /* what the schedule said last time (a change of it is a slow fade) */
    bool sched_known;
};

/* Tanner Helland's black body approximation, divided by its value at 6500 K so that 6500 K is exactly "no change". */
static void raw_rgb(double k, double rgb[3])
{
    double t = k / 100.0, r, g, b;
    r = t <= 66 ? 255 : 329.698727446 * pow(t - 60, -0.1332047592);
    g = t <= 66 ? 99.4708025861 * log(t) - 161.1195681661 : 288.1221695283 * pow(t - 60, -0.0755148492);
    b = t >= 66 ? 255 : t <= 19 ? 0 : 138.5177312231 * log(t - 10) - 305.0447927307;
    rgb[0] = r < 0 ? 0 : r > 255 ? 255 : r;
    rgb[1] = g < 0 ? 0 : g > 255 ? 255 : g;
    rgb[2] = b < 0 ? 0 : b > 255 ? 255 : b;
}

void lm_night_gains(int kelvin, double rgb[3])
{
    double ref[3], c[3];
    if (kelvin > 6500)
        kelvin = 6500;
    if (kelvin < 1000)
        kelvin = 1000;
    raw_rgb(6500, ref);
    raw_rgb(kelvin, c);
    for (int i = 0; i < 3; i++) {
        double g = c[i] / ref[i];
        rgb[i] = g > 1 ? 1 : g < 0 ? 0 : g;
    }
}

int lm_night_parse_time(const char *text)
{
    int h, m;
    char junk;
    if (!text || sscanf(text, "%d:%d%c", &h, &m, &junk) != 2 || h < 0 || h > 23 || m < 0 || m > 59)
        return -1;
    return h * 60 + m;
}

bool lm_night_in_span(int now, int from, int to)
{
    if (from < 0 || to < 0 || from == to)
        return false;
    return from < to ? now >= from && now < to : now >= from || now < to;
}

static int minutes_now(void)
{
    const char *fake = getenv("MELLAGUI_NIGHT_NOW");
    int m = fake ? lm_night_parse_time(fake) : -1;
    if (m >= 0)
        return m;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

static int fade_ms(bool schedule)
{
    const char *e = getenv("MELLAGUI_NIGHT_FADE_MS");
    if (e && atoi(e) > 0)
        return atoi(e);
    return schedule ? FADE_SCHEDULE_MS : FADE_USER_MS;
}

static void apply(struct lm_night *n)
{
    struct lm_backend *b = n->srv->backend;
    if (!b)
        return;
    double g[3];
    lm_night_gains(n->srv->config.night_kelvin, g);
    double l = n->level;
    lm_backend_set_tint(b, (float)(1.0 + (g[0] - 1.0) * l), (float)(1.0 + (g[1] - 1.0) * l), (float)(1.0 + (g[2] - 1.0) * l));
}

static bool wanted(struct lm_night *n)
{
    const struct lm_config *c = &n->srv->config;
    if (c->night_mode == 1)
        return true;
    if (c->night_mode == 2)
        return lm_night_in_span(minutes_now(), lm_night_parse_time(c->night_from), lm_night_parse_time(c->night_to));
    return false;
}

static void retarget(struct lm_night *n, bool schedule_change)
{
    double t = wanted(n) ? 1.0 : 0.0;
    if (t != n->target || (schedule_change && t != n->level)) {
        n->target = t;
        n->step = TICK_MS / (double)fade_ms(schedule_change);
    }
    if (n->level != n->target)
        lm_timer_arm_ms(n->timer, TICK_MS, 0);
    else
        lm_timer_arm_ms(n->timer, CHECK_MS, 0);
}

static void timer_cb(void *data)
{
    struct lm_night *n = data;
    if (n->level != n->target) {
        double d = n->target - n->level;
        n->level = fabs(d) <= n->step ? n->target : n->level + (d > 0 ? n->step : -n->step);
        apply(n);
        if (n->level != n->target) {
            lm_timer_arm_ms(n->timer, TICK_MS, 0);
            return;
        }
    }
    /* idle: look at the clock now and then (the schedule's edge is a slow fade) */
    bool w = wanted(n);
    bool edge = n->sched_known && w != n->sched_target && n->srv->config.night_mode == 2;
    n->sched_target = w;
    n->sched_known = true;
    retarget(n, edge);
}

struct lm_night *lm_night_create(struct lm_server *srv)
{
    struct lm_night *n = calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->srv = srv;
    n->timer = lm_loop_add_timer(srv->loop, timer_cb, n);
    if (!n->timer) {
        free(n);
        return NULL;
    }
    n->sched_target = wanted(n);
    n->sched_known = true;
    n->target = n->level = n->sched_target ? 1.0 : 0.0;          /* start in the right state, no fade at the start */
    n->step = 1.0;
    apply(n);
    lm_timer_arm_ms(n->timer, CHECK_MS, 0);
    return n;
}

void lm_night_destroy(struct lm_night *n)
{
    if (!n)
        return;
    if (n->timer)
        lm_timer_destroy(n->timer);
    free(n);
}

void lm_night_changed(struct lm_night *n)
{
    if (!n)
        return;
    n->sched_target = wanted(n);
    n->sched_known = true;
    retarget(n, false);
    apply(n);                                                   /* a new temperature shows at once when it is on */
}

void lm_night_toggle(struct lm_night *n)
{
    if (!n || !n->srv->settings)
        return;
    lm_settings_set(n->srv->settings, "night-light", n->target > 0.5 ? "off" : "on");
}

bool lm_night_active(const struct lm_night *n)
{
    return n && n->target > 0.5;
}

double lm_night_level(const struct lm_night *n)
{
    return n ? n->level : 0.0;
}
