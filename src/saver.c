/*
 * The screensaver: start a program after idle time, end it at the first input. See saver.h.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    uint64_t started;                   /* when the running program was started */
    bool locking;                       /* we are locking the screen right now: the saver stays */
    bool shield;                        /* the lock was made by the saver and it still runs over it */
};

static uint64_t unit_ms(void)
{
    const char *e = getenv("MELLAGUI_LOCK_UNIT_MS");      /* a "minute" in milliseconds; tests make it short */
    long v = e ? atol(e) : 0;
    return v > 0 ? (uint64_t)v : 60000u;
}

/* The saver no longer covers the lock: the lock card has to be painted. */
static void drop_shield(struct lm_saver *s)
{
    if (!s->shield)
        return;
    s->shield = false;
    if (s->srv->wm)
        lm_wm_lock_shield_changed(s->srv->wm);
}

static void reap_if_gone(struct lm_saver *s)
{
    if (s->pid <= 0)
        return;
    int st;
    pid_t r = waitpid(s->pid, &st, WNOHANG);
    if (r == s->pid || (r < 0 && kill(s->pid, 0) != 0)) {          /* ended (by itself, or already reaped elsewhere) */
        s->pid = 0;
        drop_shield(s);
    }
}

bool lm_saver_running(const struct lm_saver *s)
{
    return s && s->pid > 0;
}

bool lm_saver_shields(const struct lm_saver *s)
{
    return s && s->pid > 0 && s->shield;
}

bool lm_saver_start(struct lm_saver *s)
{
    if (!s || s->pid > 0)
        return false;
    const char *cmd = s->srv->config.saver_cmd;
    if (!cmd || !*cmd)
        return false;
    char styled[1024];
    if (s->srv->config.saver_style == 1 && strstr(cmd, "mellagui-matrix") && !strstr(cmd, "--flat")) {      /* the style is for our own program only */
        snprintf(styled, sizeof(styled), "%s --flat", cmd);
        cmd = styled;
    }
    pid_t pid = lm_server_spawn(s->srv, cmd);
    if (pid <= 0)
        return false;
    s->pid = pid;
    s->started = lm_now_ms();
    s->shield = false;
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
    drop_shield(s);
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
    if (locked && s->locking && s->pid > 0) {                  /* our own lock: the saver goes on running over it */
        s->shield = true;
        return;
    }
    if (locked)
        lm_saver_stop(s);
    else if (s->shield)
        lm_saver_stop(s);                                      /* unlocked some other way while it covered the lock: it must not cover the session */
}

static void tick_cb(void *data)
{
    struct lm_saver *s = data;
    struct lm_config *cfg = &s->srv->config;
    reap_if_gone(s);
    if (s->pid > 0 && cfg->saver_lock_min > 0 && !s->shield && s->srv->lock && !lm_lock_active(s->srv->lock) &&
        lm_lock_configured(s->srv->lock) && lm_now_ms() - s->started >= (uint64_t)cfg->saver_lock_min * unit_ms()) {
        s->locking = true;                                     /* it has run long enough: lock the screen, but keep it running */
        lm_lock_now(s->srv->lock);
        s->locking = false;
        return;
    }
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
