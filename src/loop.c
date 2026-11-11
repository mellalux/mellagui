#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "loop.h"

#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "list.h"
#include "log.h"

#define LM_LOOP_MAX_EVENTS 64

struct lm_source {
    struct lm_loop *loop;
    int fd;
    uint32_t events;
    lm_fd_fn fn;
    void *data;
    bool removed;
    bool owns_fd;       /* close fd when the source is released */
    void *owned_data;   /* free() when the source is released */
    struct lm_list link;
};

struct lm_timer {
    struct lm_loop *loop;
    int fd;
    struct lm_source *src;
    lm_timer_fn fn;
    void *data;
    bool armed;
    uint32_t interval_ms;
    struct lm_list link;
};

struct lm_idle {
    struct lm_loop *loop;
    lm_idle_fn fn;
    void *data;
    struct lm_list link;      /* in loop->pending (or a running batch) when scheduled */
    struct lm_list all_link;  /* in loop->idles */
};

struct lm_loop {
    int epfd;
    bool quit;
    int exit_code;
    int depth;                /* > 0 while inside lm_loop_dispatch */
    struct lm_list sources;   /* live sources */
    struct lm_list dead;      /* removed sources awaiting release */
    struct lm_list timers;
    struct lm_list idles;
    struct lm_list pending;   /* scheduled idles */
    struct lm_loop_stats stats;
};

struct signal_data {
    lm_signal_fn fn;
    void *data;
};

/* ---- time --------------------------------------------------------------------- */

uint64_t lm_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ---- loop ---------------------------------------------------------------------- */

struct lm_loop *lm_loop_create(void)
{
    struct lm_loop *loop = calloc(1, sizeof(*loop));
    if (!loop)
        return NULL;
    loop->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epfd < 0) {
        lm_error("epoll_create1: %s", strerror(errno));
        free(loop);
        return NULL;
    }
    lm_list_init(&loop->sources);
    lm_list_init(&loop->dead);
    lm_list_init(&loop->timers);
    lm_list_init(&loop->idles);
    lm_list_init(&loop->pending);
    return loop;
}

static void source_release(struct lm_source *src)
{
    lm_list_remove(&src->link);
    if (src->owns_fd && src->fd >= 0)
        close(src->fd);
    free(src->owned_data);
    free(src);
}

static void free_dead(struct lm_loop *loop)
{
    while (!lm_list_empty(&loop->dead)) {
        struct lm_source *src = lm_container_of(loop->dead.next, struct lm_source, link);
        source_release(src);
    }
}

void lm_loop_destroy(struct lm_loop *loop)
{
    if (!loop)
        return;
    while (!lm_list_empty(&loop->timers)) {
        struct lm_timer *t = lm_container_of(loop->timers.next, struct lm_timer, link);
        lm_timer_destroy(t);
    }
    while (!lm_list_empty(&loop->idles)) {
        struct lm_idle *i = lm_container_of(loop->idles.next, struct lm_idle, all_link);
        lm_idle_destroy(i);
    }
    while (!lm_list_empty(&loop->sources)) {
        struct lm_source *s = lm_container_of(loop->sources.next, struct lm_source, link);
        s->removed = true;
        source_release(s);
    }
    free_dead(loop);
    close(loop->epfd);
    free(loop);
}

void lm_loop_quit(struct lm_loop *loop, int exit_code)
{
    loop->quit = true;
    loop->exit_code = exit_code;
}

/* Times one callback; a long one is counted and logged (see lm_loop_stats). */
static void timed_call(struct lm_loop *loop, void (*fn)(void), uint64_t t0, const char *what, int fd)
{
    uint64_t ms = lm_now_ms() - t0;
    if (ms < loop->stats.threshold_ms)
        return;
    loop->stats.stalls++;
    unsigned long long off = 0;
    Dl_info info;
    if (dladdr((void *)fn, &info) && info.dli_fbase)
        off = (unsigned long long)((const char *)fn - (const char *)info.dli_fbase);
    if (ms > loop->stats.worst_ms) {
        loop->stats.worst_ms = (unsigned)ms;
        loop->stats.worst_off = off;
    }
    lm_warn("stall: a %s callback (fd %d) took %llu ms - addr2line -f -e <executable> 0x%llx", what, fd, (unsigned long long)ms, off);
}

void lm_loop_stats(const struct lm_loop *loop, struct lm_loop_stats *out)
{
    *out = loop->stats;
}

void lm_loop_set_stall_ms(struct lm_loop *loop, unsigned ms)
{
    loop->stats.threshold_ms = ms;
}

static void run_idles(struct lm_loop *loop)
{
    struct lm_list batch;
    lm_list_init(&batch);
    lm_list_splice(&batch, &loop->pending);
    /*
     * Pop before calling: an idle may destroy or re-schedule itself, cancel or
     * destroy others (they are unlinked from `batch` in place).
     */
    while (!lm_list_empty(&batch)) {
        struct lm_idle *idle = lm_container_of(batch.next, struct lm_idle, link);
        lm_list_remove(&idle->link);
        uint64_t t0 = lm_now_ms();
        idle->fn(idle->data);
        if (loop->stats.threshold_ms)
            timed_call(loop, (void (*)(void))idle->fn, t0, "idle", -1);
    }
}

int lm_loop_dispatch(struct lm_loop *loop, int timeout_ms)
{
    struct epoll_event evs[LM_LOOP_MAX_EVENTS];
    int count = 0;

    loop->depth++;
    run_idles(loop);

    int timeout = timeout_ms;
    if (!lm_list_empty(&loop->pending) || loop->quit)
        timeout = 0;

    int n = epoll_wait(loop->epfd, evs, LM_LOOP_MAX_EVENTS, timeout);
    if (n < 0) {
        if (errno != EINTR) {
            lm_error("epoll_wait: %s", strerror(errno));
            loop->depth--;
            if (loop->depth == 0)
                free_dead(loop);
            return -1;
        }
        n = 0;
    }
    for (int i = 0; i < n; i++) {
        struct lm_source *src = evs[i].data.ptr;
        if (src->removed)
            continue;
        uint64_t t0 = lm_now_ms();
        lm_fd_fn fn = src->fn;
        int sfd = src->fd;
        fn(sfd, evs[i].events, src->data);
        if (loop->stats.threshold_ms)
            timed_call(loop, (void (*)(void))fn, t0, "event", sfd);
        count++;
    }
    loop->depth--;
    if (loop->depth == 0)
        free_dead(loop);
    return count;
}

int lm_loop_run(struct lm_loop *loop)
{
    while (!loop->quit) {
        if (lm_loop_dispatch(loop, -1) < 0)
            lm_loop_quit(loop, 1);
    }
    loop->quit = false;
    return loop->exit_code;
}

/* ---- fd sources ----------------------------------------------------------------- */

static struct lm_source *source_add(struct lm_loop *loop, int fd, uint32_t events,
                                    lm_fd_fn fn, void *data)
{
    if (!loop || fd < 0 || !fn)
        return NULL;
    struct lm_source *src = calloc(1, sizeof(*src));
    if (!src)
        return NULL;
    src->loop = loop;
    src->fd = fd;
    src->events = events;
    src->fn = fn;
    src->data = data;
    struct epoll_event ev = { .events = events, .data.ptr = src };
    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        lm_error("epoll_ctl ADD fd %d: %s", fd, strerror(errno));
        free(src);
        return NULL;
    }
    lm_list_append(&loop->sources, &src->link);
    return src;
}

struct lm_source *lm_loop_add_fd(struct lm_loop *loop, int fd, uint32_t events,
                                 lm_fd_fn fn, void *data)
{
    return source_add(loop, fd, events, fn, data);
}

int lm_source_set_events(struct lm_source *src, uint32_t events)
{
    if (!src || src->removed)
        return -1;
    if (src->events == events)
        return 0;
    struct epoll_event ev = { .events = events, .data.ptr = src };
    if (epoll_ctl(src->loop->epfd, EPOLL_CTL_MOD, src->fd, &ev) < 0) {
        lm_error("epoll_ctl MOD fd %d: %s", src->fd, strerror(errno));
        return -1;
    }
    src->events = events;
    return 0;
}

void lm_source_remove(struct lm_source *src)
{
    if (!src || src->removed)
        return;
    struct lm_loop *loop = src->loop;
    src->removed = true;
    /* Unregister right away so the caller may close the fd after this returns. */
    epoll_ctl(loop->epfd, EPOLL_CTL_DEL, src->fd, NULL);
    if (loop->depth > 0) {
        lm_list_remove(&src->link);
        lm_list_append(&loop->dead, &src->link);
    } else {
        source_release(src);
    }
}

/* ---- timers ---------------------------------------------------------------------- */

static void timer_fd_cb(int fd, uint32_t events, void *data)
{
    struct lm_timer *t = data;
    (void)events;
    uint64_t expirations;
    ssize_t n = read(fd, &expirations, sizeof(expirations));
    if (n != (ssize_t)sizeof(expirations))
        return;
    if (t->interval_ms == 0)
        t->armed = false;
    t->fn(t->data); /* may destroy or re-arm `t`; do not touch it afterwards */
}

struct lm_timer *lm_loop_add_timer(struct lm_loop *loop, lm_timer_fn fn, void *data)
{
    if (!loop || !fn)
        return NULL;
    struct lm_timer *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (t->fd < 0) {
        lm_error("timerfd_create: %s", strerror(errno));
        free(t);
        return NULL;
    }
    t->loop = loop;
    t->fn = fn;
    t->data = data;
    t->src = source_add(loop, t->fd, EPOLLIN, timer_fd_cb, t);
    if (!t->src) {
        close(t->fd);
        free(t);
        return NULL;
    }
    t->src->owns_fd = true;
    lm_list_append(&loop->timers, &t->link);
    return t;
}

int lm_timer_arm_ms(struct lm_timer *t, uint32_t initial_ms, uint32_t interval_ms)
{
    if (!t)
        return -1;
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    if (initial_ms == 0 && interval_ms > 0) {
        its.it_value.tv_nsec = 1; /* fire as soon as possible, then periodically */
    } else {
        its.it_value.tv_sec = initial_ms / 1000;
        its.it_value.tv_nsec = (long)(initial_ms % 1000) * 1000000L;
    }
    its.it_interval.tv_sec = interval_ms / 1000;
    its.it_interval.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
    if (timerfd_settime(t->fd, 0, &its, NULL) < 0) {
        lm_error("timerfd_settime: %s", strerror(errno));
        return -1;
    }
    t->interval_ms = interval_ms;
    t->armed = initial_ms != 0 || interval_ms != 0;
    return 0;
}

bool lm_timer_is_armed(const struct lm_timer *t)
{
    return t && t->armed;
}

void lm_timer_destroy(struct lm_timer *t)
{
    if (!t)
        return;
    lm_list_remove(&t->link);
    t->src->owns_fd = true; /* closed when the source is finally released */
    lm_source_remove(t->src);
    free(t);
}

/* ---- idle -------------------------------------------------------------------------- */

struct lm_idle *lm_loop_add_idle(struct lm_loop *loop, lm_idle_fn fn, void *data)
{
    if (!loop || !fn)
        return NULL;
    struct lm_idle *idle = calloc(1, sizeof(*idle));
    if (!idle)
        return NULL;
    idle->loop = loop;
    idle->fn = fn;
    idle->data = data;
    lm_list_init(&idle->link);
    lm_list_append(&loop->idles, &idle->all_link);
    return idle;
}

void lm_idle_schedule(struct lm_idle *idle)
{
    if (!idle || lm_list_linked(&idle->link))
        return;
    lm_list_append(&idle->loop->pending, &idle->link);
}

void lm_idle_destroy(struct lm_idle *idle)
{
    if (!idle)
        return;
    lm_list_remove(&idle->link);
    lm_list_remove(&idle->all_link);
    free(idle);
}

/* ---- signals ------------------------------------------------------------------------ */

static void signal_fd_cb(int fd, uint32_t events, void *data)
{
    struct signal_data *sd = data;
    (void)events;
    struct signalfd_siginfo si;
    /* Bounded: a handler may remove the source, so re-check via `data` is not possible. */
    ssize_t n = read(fd, &si, sizeof(si));
    if (n != (ssize_t)sizeof(si))
        return;
    sd->fn((int)si.ssi_signo, sd->data);
}

struct lm_source *lm_loop_add_signal(struct lm_loop *loop, int signo,
                                     lm_signal_fn fn, void *data)
{
    if (!loop || !fn || signo <= 0)
        return NULL;
    sigset_t mask;
    sigemptyset(&mask);
    if (sigaddset(&mask, signo) < 0)
        return NULL;
    struct signal_data *sd = calloc(1, sizeof(*sd));
    if (!sd)
        return NULL;
    sd->fn = fn;
    sd->data = data;
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        free(sd);
        return NULL;
    }
    int fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (fd < 0) {
        lm_error("signalfd(%d): %s", signo, strerror(errno));
        free(sd);
        return NULL;
    }
    struct lm_source *src = source_add(loop, fd, EPOLLIN, signal_fd_cb, sd);
    if (!src) {
        close(fd);
        free(sd);
        return NULL;
    }
    src->owns_fd = true;
    src->owned_data = sd;
    return src;
}
