/* Standby failures with a fake clock/backend: no real display or wall-clock waits. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "backend.h"
#include "loop.h"
#include "saver.h"
#include "screenoff.h"
#include "server.h"

struct lm_timer {
    lm_timer_fn fn;
    void *data;
    uint32_t delay;
};

static struct lm_timer timer;
static uint64_t now;
static int failures, off_calls, on_calls;
static bool saver_running, first_monitor_off;

uint64_t lm_now_ms(void) { return now; }
struct lm_timer *lm_loop_add_timer(struct lm_loop *loop, lm_timer_fn fn, void *data)
{
    (void)loop;
    timer = (struct lm_timer){ .fn = fn, .data = data };
    return &timer;
}
int lm_timer_arm_ms(struct lm_timer *t, uint32_t delay, uint32_t interval)
{
    (void)interval;
    t->delay = delay;
    return 0;
}
void lm_timer_destroy(struct lm_timer *t) { t->delay = 0; }
void lm_saver_stop(struct lm_saver *s) { (void)s; saver_running = false; }

static int power(struct lm_backend *b, bool on)
{
    (void)b;
    if (on) {
        on_calls++;
        first_monitor_off = false;
        return 0;
    }
    /* Animation must stop before the first DPMS attempt, even if that attempt fails. */
    assert(!saver_running);
    off_calls++;
    first_monitor_off = true;          /* simulate one output succeeding and another failing */
    if (failures > 0) {
        failures--;
        return -1;
    }
    return 0;
}

static void advance(uint64_t ms)
{
    assert(timer.delay > 0);
    now += ms;
    timer.fn(timer.data);
}

static void run_case(int fail_count, bool wake, bool destroy)
{
    const struct lm_backend_ops ops = { .set_power = power };
    struct lm_backend backend = { .ops = &ops, .dim = 1.0f };
    struct lm_server srv = { .backend = &backend };
    now = 10000;
    failures = fail_count;
    off_calls = on_calls = 0;
    saver_running = true;
    first_monitor_off = false;
    struct lm_screenoff *s = lm_screenoff_create(&srv);
    assert(s);
    lm_screenoff_start(s);
    advance(50);
    assert(saver_running && off_calls == 0);
    assert(lm_screenoff_level(s) > 0 && lm_screenoff_level(s) < 1);
    advance(50);
    assert(off_calls == 1 && !saver_running);
    assert(lm_screenoff_level(s) == 0);
    if (fail_count) {
        assert(lm_screenoff_active(s) && !lm_screenoff_off(s));
        assert(!backend.blanked && first_monitor_off);
        assert(timer.delay == 1000);                /* retry, without a busy loop */
        lm_screenoff_start(s);                      /* another idle timeout must not restart the fade */
        assert(timer.delay == 1000 && off_calls == 1);
        if (destroy) {
            lm_screenoff_destroy(s);
            assert(on_calls == 1 && !first_monitor_off && timer.delay == 0);
            return;
        }
        if (wake) {
            assert(lm_screenoff_wake(s));
            assert(on_calls == 1 && !first_monitor_off);
            assert(!lm_screenoff_active(s));
            advance(100);
            assert(lm_screenoff_level(s) == 1 && timer.delay == 0);
            assert(off_calls == 1);                  /* the abandoned retry must never put it back to sleep */
            lm_screenoff_destroy(s);
            return;
        }
        while (!lm_screenoff_off(s)) {
            assert(off_calls <= fail_count);
            advance(1000);
        }
    }
    assert(lm_screenoff_off(s) && backend.blanked && timer.delay == 0);
    assert(off_calls == fail_count + 1);
    assert(lm_screenoff_wake(s));
    assert(!first_monitor_off && !backend.blanked);
    advance(100);
    assert(lm_screenoff_level(s) == 1 && timer.delay == 0);
    lm_screenoff_destroy(s);
}

int main(void)
{
    setenv("MELLAGUI_BLANK_FADE_MS", "100", 1);
    run_case(0, false, false);
    run_case(2, false, false);
    run_case(2, true, false);
    run_case(2, false, true);
    puts("screenoff-test ok");
    return 0;
}
