/*
 * Monitors to sleep with a fade, see screenoff.h.
 */
#include <stdlib.h>

#include "backend.h"
#include "loop.h"
#include "log.h"
#include "screenoff.h"
#include "server.h"

#define FADE_OUT_MS 2000
#define STEP_MS 33

enum phase { IDLE, FADING_OUT, OFF, FADING_IN };

struct lm_screenoff {
    struct lm_server *srv;
    struct lm_timer *timer;
    enum phase phase;
    uint64_t t0;                        /* when the current fade began */
    double level;                       /* 1 = full brightness */
    double from;                        /* the level a fade in starts from */
};

static int fade_ms(void)
{
    const char *e = getenv("MELLAGUI_BLANK_FADE_MS");
    return e && atoi(e) > 0 ? atoi(e) : FADE_OUT_MS;
}

static double smooth(double x)
{
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    return x * x * (3.0 - 2.0 * x);
}

static void apply(struct lm_screenoff *s)
{
    if (s->srv->backend)
        lm_backend_set_dim(s->srv->backend, (float)s->level);
}

static void timer_cb(void *data)
{
    struct lm_screenoff *s = data;
    uint64_t now = lm_now_ms();
    if (s->phase == FADING_OUT) {
        double f = (double)(now - s->t0) / (double)fade_ms();
        s->level = 1.0 - smooth(f);
        apply(s);
        if (f >= 1.0) {                                         /* black: now the monitors can go to sleep */
            s->phase = OFF;
            lm_timer_arm_ms(s->timer, 0, 0);
            if (s->srv->backend)
                lm_backend_set_power(s->srv->backend, false);
            lm_info("screen: monitors in standby");
        }
    } else if (s->phase == FADING_IN) {
        int in_ms = fade_ms() / 6 < 30 ? 30 : fade_ms() / 6;
        double f = (double)(now - s->t0) / (double)in_ms;
        s->level = s->from + (1.0 - s->from) * smooth(f);
        apply(s);
        if (f >= 1.0) {
            s->level = 1.0;
            apply(s);
            s->phase = IDLE;
            lm_timer_arm_ms(s->timer, 0, 0);
        }
    }
}

struct lm_screenoff *lm_screenoff_create(struct lm_server *srv)
{
    struct lm_screenoff *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->srv = srv;
    s->level = 1.0;
    s->timer = lm_loop_add_timer(srv->loop, timer_cb, s);
    if (!s->timer) {
        free(s);
        return NULL;
    }
    return s;
}

void lm_screenoff_destroy(struct lm_screenoff *s)
{
    if (!s)
        return;
    if (s->phase != IDLE && s->srv->backend) {
        if (s->phase == OFF || s->phase == FADING_OUT)
            lm_backend_set_power(s->srv->backend, true);
        lm_backend_set_dim(s->srv->backend, 1.0f);
    }
    if (s->timer)
        lm_timer_destroy(s->timer);
    free(s);
}

void lm_screenoff_start(struct lm_screenoff *s)
{
    if (!s || s->phase == FADING_OUT || s->phase == OFF)
        return;
    s->phase = FADING_OUT;
    s->t0 = lm_now_ms() - (uint64_t)((1.0 - s->level) * fade_ms());         /* (from where a fade in had come to) */
    lm_timer_arm_ms(s->timer, STEP_MS, STEP_MS);
}

bool lm_screenoff_wake(struct lm_screenoff *s)
{
    if (!s || s->phase == IDLE)
        return false;
    bool was_fading_in = s->phase == FADING_IN;
    if (s->phase == OFF || s->phase == FADING_OUT) {
        if (s->phase == OFF && s->srv->backend)
            lm_backend_set_power(s->srv->backend, true);
        s->from = s->level;
        s->phase = FADING_IN;
        s->t0 = lm_now_ms();
        lm_timer_arm_ms(s->timer, STEP_MS, STEP_MS);
        lm_info("screen: monitors on");
    }
    return !was_fading_in;
}

bool lm_screenoff_active(const struct lm_screenoff *s)
{
    return s && (s->phase == FADING_OUT || s->phase == OFF);
}

bool lm_screenoff_off(const struct lm_screenoff *s)
{
    return s && s->phase == OFF;
}

double lm_screenoff_level(const struct lm_screenoff *s)
{
    return s ? s->level : 1.0;
}
