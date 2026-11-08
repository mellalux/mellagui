#ifndef MELLAGUI_LOCK_H
#define MELLAGUI_LOCK_H

/*
 * Screen lock. Owner: lock.c. The lock lives inside the compositor, not in a client, so nothing a client does (or a crash
 * of it) can unlock the screen.
 *
 * While the screen is locked
 *   - the scene is nothing but the lock screen (LM_SCENE_LOCK, one per output): no window, panel, gadget or dock is painted,
 *   - the seat sends nothing to clients: keyboard and pointer focus are NULL, key / pointer / touch input goes only to
 *     lm_lock_key() (seat.c, lm_seat_set_locked); the shortcuts of the wm (Super+Shift+E quits, Super+Enter starts a
 *     terminal ...) are off. The two exceptions: Ctrl+Alt+F1..F12 still switches the virtual terminal unless `vt=0`
 *     (the other VTs are protected by their own login) and Super+Space still switches the keyboard layout (so the secret can be typed),
 *   - the secret is checked with PBKDF2-HMAC-SHA256 (sha256.h); three wrong attempts in a row start a delay that doubles
 *     (5 s, 10 s, ... at most 5 min), during which even the right secret is refused.
 *
 * Kinds: a password (8..128 bytes of text) or a PIN (4..12 digits). Only a random salt and the PBKDF2 hash are stored, in
 * `lock.conf` (--lock-file; default $XDG_CONFIG_HOME/mellagui/lock.conf, headless: none = kept in memory only), created
 * with mode 0600:
 *
 *     kind=password|pin   iter=<PBKDF2 iterations>   salt=<32 hex>   hash=<64 hex>   timeout=<minutes, 0 = never>   start=0|1
 *     blank=<minutes, 0 = never, default 3>   vt=0|1 (console switching on the lock screen, default 1)
 *
 * Standby: while locked and with no input for `blank` minutes the monitors are switched off (DPMS, backend.h set_power).
 * The first input wakes them and is swallowed; unlocking wakes them too.
 *
 * Locking: Super+L, the "Lock now" button of the settings panel (`lock-now`), the idle `timeout` (no input for that many
 * minutes; env MELLAGUI_LOCK_UNIT_MS shrinks the minute for tests) and, with `start=1`, when MellaGUI starts.
 *
 * Changing the lock (settings key `lock-set`, control socket `lock set`) needs the current secret as soon as one exists and is
 * refused while the screen is locked (status "locked"). Wrong secrets there start the same doubling delay, but with a counter of
 * their own: a program that spams the settings must not be able to keep the lock screen's delay running. There is no way to recover a forgotten secret from inside the
 * session: delete lock.conf from another login (ssh, another VT).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "server.h"

struct lm_output;

#define LM_LOCK_MAX_SECRET 128
#define LM_LOCK_MIN_PASSWORD 8
#define LM_LOCK_MIN_PIN 4
#define LM_LOCK_MAX_PIN 12

/* What the renderer paints on one output. */
struct lm_lock_info {
    struct lm_output *output;
    bool pin;                   /* false = password */
    int nchars;                 /* typed characters so far (shown as dots) */
    char message[96];           /* "Wrong password.", "Too many attempts. Try again in 25 s." or "" */
    bool quit;                  /* locked on purpose: the secret quits MellaGUI (lm_lock_request_quit) */
    bool blocked;               /* inside the delay after wrong attempts: typing does nothing */
    char clock[8];              /* "HH:MM" */
    char date[24];              /* "DD.MM.YYYY" */
    char layout[8];             /* "US", "EE" ... */
};

struct lm_lock;

/* Reads lock.conf (config.lock_path) and, with start=1, locks at once. Create after the seat, the wm and the settings. */
struct lm_lock *lm_lock_create(struct lm_server *srv);
void lm_lock_destroy(struct lm_lock *l);

/* NULL-safe queries. */
bool lm_lock_active(const struct lm_lock *l);       /* the screen is locked now */
bool lm_lock_configured(const struct lm_lock *l);   /* a password / PIN exists */
const char *lm_lock_kind(const struct lm_lock *l);  /* "none" | "password" | "pin" */
int lm_lock_timeout(const struct lm_lock *l);       /* minutes, 0 = never */
bool lm_lock_start(const struct lm_lock *l);        /* lock when MellaGUI starts */
bool lm_lock_vt(const struct lm_lock *l);           /* Ctrl+Alt+F1..F12 works on the lock screen */
int lm_lock_blank(const struct lm_lock *l);         /* minutes of quiet after which the locked screen's monitors go to standby, 0 = never */
const char *lm_lock_status(const struct lm_lock *l);/* result of the last lm_lock_apply(): "" | ok | wrong | wait:<s> | weak | invalid | locked | error */

/* Locks the screen now; false (and nothing happens) when no secret is set. */
bool lm_lock_now(struct lm_lock *l);
/*
 * Quitting MellaGUI (Super+Shift+E, the panel's Exit button) when a secret is set: locks the screen and quits only once the
 * secret was typed (Esc on an empty field cancels), so that leaving to the console the compositor was started from needs the
 * secret too. Returns false when no secret is set: the caller quits at once. (SIGTERM and the control socket `quit` are
 * not asked: whoever can send signals as the user already owns the session.)
 */
bool lm_lock_request_quit(struct lm_lock *l);
/*
 * Any input event happened (idle timers). Returns true when the event only woke the monitors from standby: the seat then
 * drops it (the first key press or click after a standby is not typing).
 */
bool lm_lock_activity(struct lm_lock *l);
/*
 * A key was pressed while locked (seat.c): `mods` = LM_MOD_* (depressed | latched | locked), `group` = active layout.
 * Typing, Backspace, Escape (clears), Enter (tries the secret), Ctrl+Alt+Fn, Super+Space.
 */
void lm_lock_key(struct lm_lock *l, uint32_t code, uint32_t mods, uint32_t group);

/*
 * Change the lock. `old`: the current secret (ignored while none is set); `kind`: "password", "pin", "keep" (only change
 * timeout / start / blank) or "none" (remove the lock); `secret`: the new one (for password / pin); `minutes`, `start`,
 * `blank`, `vt` (1 = Ctrl+Alt+Fn works while locked): "" = keep, else the new value (blank: minutes until the standby, 0 = never). Returns true on success. The text result is lm_lock_status().
 */
bool lm_lock_apply(struct lm_lock *l, const char *old, const char *kind, const char *secret, const char *minutes,
                   const char *start, const char *blank, const char *vt);

/* Fill what to paint on `out`. */
void lm_lock_info(const struct lm_lock *l, struct lm_output *out, struct lm_lock_info *info);

/* One line for the control socket: "kind=.. locked=.. timeout=.. start=.. typed=.. fails=.. wait=.. status=.. message=..". */
void lm_lock_describe(const struct lm_lock *l, char *out, size_t n);

#endif
