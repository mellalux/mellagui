/*
 * The screensaver: start a program after idle time, end it at the first input. See saver.h.
 */
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "idleinhibit.h"
#include "lock.h"
#include "log.h"
#include "loop.h"
#include "saver.h"
#include "server.h"
#include "util.h"
#include "wm.h"

struct lm_saver {
    struct lm_server *srv;
    struct lm_timer *tick;              /* 1 s: is it time? did the program end on its own? */
    pid_t pid;                          /* the running program, 0 = none */
    uint64_t last_activity;
};

static uint64_t unit_ms(void)
{
    const char *e = getenv("MELLAGUI_LOCK_UNIT_MS");      /* a "minute" in milliseconds; tests make it short */
    long v = e ? atol(e) : 0;
    return v > 0 ? (uint64_t)v : 60000u;
}

static void reap_if_gone(struct lm_saver *s)
{
    if (s->pid <= 0)
        return;
    int st;
    pid_t r = waitpid(s->pid, &st, WNOHANG);
    if (r == s->pid || (r < 0 && kill(s->pid, 0) != 0))            /* ended (by itself, or already reaped elsewhere) */
        s->pid = 0;
}

bool lm_saver_running(const struct lm_saver *s)
{
    return s && s->pid > 0;
}

bool lm_saver_start(struct lm_saver *s)
{
    if (!s || s->pid > 0)
        return false;
    const char *cmd = s->srv->config.saver_cmd;
    if (!cmd || !*cmd)
        return false;
    pid_t pid = lm_server_spawn(s->srv, cmd);
    if (pid <= 0)
        return false;
    s->pid = pid;
    lm_info("screensaver started (pid %d)", (int)pid);
    return true;
}

void lm_saver_stop(struct lm_saver *s)
{
    if (!s || s->pid <= 0)
        return;
    kill(-s->pid, SIGTERM);                                    /* the program leads its own session: the whole group */
    kill(s->pid, SIGTERM);
    waitpid(s->pid, NULL, WNOHANG);                            /* (a zombie is picked up by the next tick if it is not ready yet) */
    lm_info("screensaver ended");
    s->pid = 0;
}

bool lm_saver_activity(struct lm_saver *s)
{
    if (!s)
        return false;
    s->last_activity = lm_now_ms();
    reap_if_gone(s);
    if (s->pid > 0) {
        lm_saver_stop(s);
        return true;                                           /* this event only woke the screen up */
    }
    return false;
}

void lm_saver_locked(struct lm_saver *s, bool locked)
{
    if (!s)
        return;
    s->last_activity = lm_now_ms();
    if (locked)
        lm_saver_stop(s);
}

static void tick_cb(void *data)
{
    struct lm_saver *s = data;
    struct lm_config *cfg = &s->srv->config;
    reap_if_gone(s);
    if (s->pid > 0 || !cfg->saver || cfg->saver_min <= 0)
        return;
    if (lm_idle_inhibited(s->srv->idle_inhibit)) {              /* a film is on: the idle time starts when it is over */
        s->last_activity = lm_now_ms();
        return;
    }
    if (lm_now_ms() - s->last_activity < (uint64_t)cfg->saver_min * unit_ms())
        return;
    if (s->srv->lock && lm_lock_active(s->srv->lock))
        return;
    if (s->srv->wm && lm_wm_has_fullscreen(s->srv->wm))        /* a film or a game: nobody touches the keyboard, nobody wants a screensaver */
        return;
    if (!lm_saver_start(s))
        s->last_activity = lm_now_ms();                        /* no command / no fork: try again after another idle period */
}

struct lm_saver *lm_saver_create(struct lm_server *srv)
{
    struct lm_saver *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->srv = srv;
    s->last_activity = lm_now_ms();
    s->tick = lm_loop_add_timer(srv->loop, tick_cb, s);
    if (s->tick)
        lm_timer_arm_ms(s->tick, 1000, 1000);
    return s;
}

void lm_saver_destroy(struct lm_saver *s)
{
    if (!s)
        return;
    lm_saver_stop(s);
    if (s->tick)
        lm_timer_destroy(s->tick);
    free(s);
}
