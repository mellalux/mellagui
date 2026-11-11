#ifndef MELLAGUI_SAVER_H
#define MELLAGUI_SAVER_H

/*
 * The screensaver: after `saver_min` minutes without any input the compositor starts a program (config `[commands] saver`, default
 * mellagui-matrix, a full screen window) and ends it again at the first input. The program is an ordinary Wayland client: nothing here
 * paints, and any other program that fills the screen can take its place.
 *
 *  - Idle time is counted from the last input event (lm_saver_activity, called by seat.c for every one of them).
 *  - It does not start while the screen is locked, while the saver already runs, or while some window is fullscreen (a film or a game
 *    gets no input for a long time and must not be covered); settings: `saver` (on / off) and `saver-minutes`.
 *  - The first input that arrives while it runs only ends it: the event is swallowed (nothing is typed into the window below, no click
 *    goes through), the same way the first input wakes a standby monitor (lock.c).
 *  - Locking the screen ends it, too (lm_saver_locked) - except the lock that the saver makes itself: `saver-lock` minutes after it started
 *    (default 1; only when a lock secret is set) it locks the screen and keeps running. While it runs over the lock the scene shows the
 *    saver's window instead of the lock card (lm_saver_shields); the first input ends it (swallowed) and the lock card is there.
 *  - Tests make the minute short with the same MELLAGUI_LOCK_UNIT_MS as the lock.
 */

#include <stdbool.h>

struct lm_server;
struct lm_saver;

struct lm_saver *lm_saver_create(struct lm_server *srv);
void lm_saver_destroy(struct lm_saver *s);

/* Any input event. Returns true when it only ended the screensaver (the seat then drops the event). */
bool lm_saver_activity(struct lm_saver *s);
/* The screen was locked / unlocked. */
void lm_saver_locked(struct lm_saver *s, bool locked);

/* The screen is locked by the saver and the saver's window is what the screen shows (instead of the lock card). */
bool lm_saver_shields(const struct lm_saver *s);

/* Start it now (settings `saver-now`, control socket `saver start`); false when it runs already or there is no command. */
bool lm_saver_start(struct lm_saver *s);
/* End it (no-op when it does not run). */
void lm_saver_stop(struct lm_saver *s);
bool lm_saver_running(const struct lm_saver *s);

#endif
