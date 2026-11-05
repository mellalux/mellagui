#ifndef LUMEN_LOOP_H
#define LUMEN_LOOP_H

/*
 * Single-threaded epoll event loop. Owner: loop.c.
 *
 * Everything in Lumen runs on one thread inside lm_loop_run(). Callbacks may
 * freely add/remove sources, timers and idle callbacks (including themselves);
 * removal is deferred until the current dispatch round finishes, so a source
 * pointer stays valid until its callback returns.
 */

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct lm_loop;
struct lm_source;
struct lm_timer;
struct lm_idle;

/* Event mask bits are the EPOLLIN/EPOLLOUT/EPOLLERR/EPOLLHUP values. */
typedef void (*lm_fd_fn)(int fd, uint32_t events, void *data);
typedef void (*lm_timer_fn)(void *data);
typedef void (*lm_idle_fn)(void *data);
typedef void (*lm_signal_fn)(int signo, void *data);

struct lm_loop *lm_loop_create(void);
/* Frees the loop. Remaining sources/timers/idles are dropped (fds not closed). */
void lm_loop_destroy(struct lm_loop *loop);

/* Run until lm_loop_quit(); returns the exit code passed to lm_loop_quit. */
int lm_loop_run(struct lm_loop *loop);
void lm_loop_quit(struct lm_loop *loop, int exit_code);
/*
 * One iteration (for tests): idle callbacks, wait up to timeout_ms
 * (-1 = block, 0 = poll), dispatch. Returns number of events dispatched or -1.
 */
int lm_loop_dispatch(struct lm_loop *loop, int timeout_ms);

/* ---- fd sources --------------------------------------------------------- */

/* The loop does not own `fd` (never closes it). Level-triggered. NULL on error. */
struct lm_source *lm_loop_add_fd(struct lm_loop *loop, int fd, uint32_t events,
                                 lm_fd_fn fn, void *data);
int lm_source_set_events(struct lm_source *src, uint32_t events);
/* Safe from inside any callback, including the source's own. */
void lm_source_remove(struct lm_source *src);

/* ---- timers (timerfd, CLOCK_MONOTONIC) ----------------------------------- */

struct lm_timer *lm_loop_add_timer(struct lm_loop *loop, lm_timer_fn fn, void *data);
/*
 * Arm: first expiry after initial_ms, then every interval_ms (0 = one shot).
 * initial_ms == 0 && interval_ms == 0 disarms. Returns 0 or -1.
 */
int lm_timer_arm_ms(struct lm_timer *t, uint32_t initial_ms, uint32_t interval_ms);
bool lm_timer_is_armed(const struct lm_timer *t);
void lm_timer_destroy(struct lm_timer *t);

/* ---- idle callbacks ------------------------------------------------------ */

/*
 * Runs once, at the start of the next loop iteration, before blocking in
 * epoll. If any idle is queued the loop does not block. Calling
 * lm_idle_schedule() while already scheduled is a no-op.
 */
struct lm_idle *lm_loop_add_idle(struct lm_loop *loop, lm_idle_fn fn, void *data);
void lm_idle_schedule(struct lm_idle *idle);
void lm_idle_cancel(struct lm_idle *idle);
void lm_idle_destroy(struct lm_idle *idle);

/* ---- signals (signalfd) ---------------------------------------------------- */

/*
 * Blocks `signo` for the process and delivers it through the loop. Several
 * callers may register for different signals. Returns an fd source (remove it
 * with lm_source_remove to unregister; the signal stays blocked) or NULL.
 */
struct lm_source *lm_loop_add_signal(struct lm_loop *loop, int signo,
                                     lm_signal_fn fn, void *data);

/* ---- time ---------------------------------------------------------------- */

uint64_t lm_now_ms(void);          /* CLOCK_MONOTONIC, milliseconds */
/* Truncated to 32 bit: the `time` argument used in wl_pointer/wl_keyboard events. */
static inline uint32_t lm_now_msec32(void) { return (uint32_t)lm_now_ms(); }

#endif
