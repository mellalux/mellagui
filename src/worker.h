#ifndef MELLAGUI_WORKER_H
#define MELLAGUI_WORKER_H

/*
 * The one worker thread of the compositor. The event loop (loop.c) is single threaded and must never wait for anything: a program that is slow to answer
 * (pactl, wpctl, pw-cli ...) or a disk that sleeps would stop input, painting and every client for as long as it takes. Such work goes here:
 *
 *   lm_work_submit(fn, done, data, owner)
 *     `fn(data)` runs on the worker thread, in submission order (one queue, so "set the volume" and "play the beep" keep their order);
 *     `done(data)` then runs on the loop's thread (through an eventfd in the loop) and may touch everything the loop owns. Either may be NULL.
 *     `fn` must not touch anything but `data` and its own module's state, and must not call into the loop.
 *   lm_work_forget(owner)
 *     the `done` callbacks of every job submitted with that `owner` that did not run yet are dropped (call it before the owner is freed).
 *
 * Before lm_work_init() (the clients and the unit tests link some of the same modules) and after lm_work_shutdown() the jobs run at once, in the caller's
 * thread, `done` right behind `fn`.
 */

#include <stdbool.h>

struct lm_loop;

typedef void (*lm_work_fn)(void *data);

void lm_work_init(struct lm_loop *loop);
void lm_work_submit(lm_work_fn fn, lm_work_fn done, void *data, const void *owner);
void lm_work_forget(const void *owner);
/* Waits for the queued jobs (their `done` is not called any more) and stops the thread. */
void lm_work_shutdown(void);
/* Jobs not finished yet (tests / the control socket `loop` line). */
int lm_work_pending(void);

#endif
