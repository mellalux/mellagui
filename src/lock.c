/*
 * Screen lock. See lock.h for the contract.
 */
#include "lock.h"
#include "idleinhibit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>

#include "backend.h"
#include "clockfmt.h"
#include "input.h"
#include "keymap.h"
#include "log.h"
#include "loop.h"
#include "render.h"
#include "seat.h"
#include "settings.h"
#include "sha256.h"
#include "wm.h"

#define SALT_LEN 16
#define HASH_LEN 32
#define DEFAULT_ITER 200000u
#define MIN_ITER 1000u
#define MAX_ITER 10000000u
#define FREE_TRIES 3                 /* wrong attempts before the delay starts */
#define BASE_DELAY_MS 5000u
#define MAX_DELAY_MS 300000u
#define MAX_TIMEOUT_MIN 1440
#define MAX_BLANK_MIN 240
#define DEFAULT_BLANK_MIN 3

enum kind { KIND_NONE, KIND_PASSWORD, KIND_PIN };

struct lm_lock {
    struct lm_server *srv;
    /* what lock.conf holds */
    enum kind kind;
    uint32_t iter;
    uint8_t salt[SALT_LEN], hash[HASH_LEN];
    int timeout_min;
    bool lock_on_start;
    int blank_min;                   /* monitors go to standby this long after the last input while locked, 0 = never */
    bool vt_switch;                  /* Ctrl+Alt+F1..F12 works while locked (default yes) */
    bool quit_pending;               /* locked on purpose to ask for the secret before MellaGUI quits */
    /* run time */
    bool locked;
    bool blanked;                    /* we put the monitors in standby */
    char input[LM_LOCK_MAX_SECRET + 1];
    size_t len;                      /* bytes in input */
    int nchars;                      /* code points in input */
    int fails;                       /* consecutive wrong secrets (lock screen and lm_lock_apply together) */
    uint64_t blocked_until;          /* lm_now_ms(): no attempts before this */
    /* The same for changing the lock through the settings (lm_lock_apply). A separate counter: a program that spams wrong
     * secrets there must not be able to keep the lock screen's own delay running (a denial of service on unlocking). */
    int set_fails;
    uint64_t set_blocked_until;
    char message[64];
    char status[24];
    uint64_t last_activity;
    struct lm_timer *tick;           /* 1 s: idle check, countdown, clock */
    char shown_clock[16];
    int shown_wait;
};

/* ---- small helpers ------------------------------------------------------------------------------------------- */

static uint64_t unit_ms(void)
{
    const char *e = getenv("MELLAGUI_LOCK_UNIT_MS");      /* a "minute" in milliseconds; tests make it short */
    long v = e ? atol(e) : 0;
    return v > 0 ? (uint64_t)v : 60000u;
}

static uint32_t delay_for(int fails)
{
    if (fails < FREE_TRIES)
        return 0;
    const char *e = getenv("MELLAGUI_LOCK_DELAY_MS");     /* tests */
    long base = e ? atol(e) : 0;
    uint64_t d = base > 0 ? (uint64_t)base : BASE_DELAY_MS;
    for (int i = FREE_TRIES + 1; i <= fails && d < MAX_DELAY_MS; i++)
        d *= 2;
    return d > MAX_DELAY_MS ? MAX_DELAY_MS : (uint32_t)d;
}

static uint32_t iterations_for_new(void)
{
    const char *e = getenv("MELLAGUI_LOCK_ITER");        /* tests: the 200000 default takes ~0.1 s per check */
    long v = e ? atol(e) : 0;
    if (v >= (long)MIN_ITER && v <= (long)MAX_ITER)
        return (uint32_t)v;
    return DEFAULT_ITER;
}

static int wait_seconds(const struct lm_lock *l)
{
    uint64_t now = lm_now_ms();
    return l->blocked_until > now ? (int)((l->blocked_until - now + 999) / 1000) : 0;
}

static void hash_secret(const struct lm_lock *l, const char *secret, size_t len, uint8_t out[HASH_LEN])
{
    lm_pbkdf2_sha256(secret, len, l->salt, SALT_LEN, l->iter, out, HASH_LEN);
}

static bool verify(const struct lm_lock *l, const char *secret, size_t len)
{
    uint8_t h[HASH_LEN];
    hash_secret(l, secret, len, h);
    bool ok = lm_ct_equal(h, l->hash, HASH_LEN);
    lm_secure_zero(h, sizeof(h));
    return ok;
}

static int set_wait_seconds(const struct lm_lock *l)
{
    uint64_t now = lm_now_ms();
    return l->set_blocked_until > now ? (int)((l->set_blocked_until - now + 999) / 1000) : 0;
}

static void note_set_failure(struct lm_lock *l)
{
    if (l->set_fails < 1000)
        l->set_fails++;
    uint32_t d = delay_for(l->set_fails);
    l->set_blocked_until = d ? lm_now_ms() + d : 0;
}

static void note_failure(struct lm_lock *l)
{
    if (l->fails < 1000)
        l->fails++;
    uint32_t d = delay_for(l->fails);
    l->blocked_until = d ? lm_now_ms() + d : 0;
}

static void announce(struct lm_lock *l)
{
    lm_settings_changed(l->srv->settings);
}

static void repaint(struct lm_lock *l)
{
    if (l->srv->render)
        lm_render_damage_all(l->srv->render);
}

static void wipe_input(struct lm_lock *l)
{
    lm_secure_zero(l->input, sizeof(l->input));
    l->len = 0;
    l->nchars = 0;
}

static void set_status(struct lm_lock *l, const char *s)
{
    snprintf(l->status, sizeof(l->status), "%s", s);
}

/* ---- lock.conf ------------------------------------------------------------------------------------------------ */

static void mkdir_parents(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(path, 0700) < 0 && errno != EEXIST) {
            *p = '/';
            return;
        }
        *p = '/';
    }
}

static bool load(struct lm_lock *l)
{
    const char *path = l->srv->config.lock_path;
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f)
        return false;
    struct stat st;
    if (fstat(fileno(f), &st) == 0 && (st.st_mode & 077))
        lm_warn("lock: %s is readable by others (mode %o); chmod 600 it", path, (unsigned)(st.st_mode & 0777));
    enum kind kind = KIND_NONE;
    uint32_t iter = 0;
    uint8_t salt[SALT_LEN], hash[HASH_LEN];
    bool have_salt = false, have_hash = false;
    int timeout = 0, blank = DEFAULT_BLANK_MIN;
    bool start = false, vt = true;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq)
            continue;
        *eq = 0;
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = 0;
        if (!strcmp(line, "kind"))
            kind = !strcmp(val, "password") ? KIND_PASSWORD : !strcmp(val, "pin") ? KIND_PIN : KIND_NONE;
        else if (!strcmp(line, "iter"))
            iter = (uint32_t)strtoul(val, NULL, 10);
        else if (!strcmp(line, "salt"))
            have_salt = lm_hex_decode(val, salt, SALT_LEN);
        else if (!strcmp(line, "hash"))
            have_hash = lm_hex_decode(val, hash, HASH_LEN);
        else if (!strcmp(line, "timeout"))
            timeout = atoi(val);
        else if (!strcmp(line, "start"))
            start = atoi(val) != 0;
        else if (!strcmp(line, "blank"))
            blank = atoi(val);
        else if (!strcmp(line, "vt"))
            vt = atoi(val) != 0;
    }
    fclose(f);
    if (kind == KIND_NONE || !have_salt || !have_hash || iter < MIN_ITER || iter > MAX_ITER) {
        lm_warn("lock: %s is not a valid lock file; the screen lock is off", path);
        return false;
    }
    l->kind = kind;
    l->iter = iter;
    memcpy(l->salt, salt, SALT_LEN);
    memcpy(l->hash, hash, HASH_LEN);
    l->timeout_min = timeout < 0 ? 0 : timeout > MAX_TIMEOUT_MIN ? MAX_TIMEOUT_MIN : timeout;
    l->lock_on_start = start;
    l->blank_min = blank < 0 ? 0 : blank > MAX_BLANK_MIN ? MAX_BLANK_MIN : blank;
    l->vt_switch = vt;
    return true;
}

/* Writes the current state (mode 0600, atomically). No path = nothing to write = fine. Returns false on a write error. */
static bool save(const struct lm_lock *l)
{
    const char *path = l->srv->config.lock_path;
    if (!path || !*path)
        return true;
    if (l->kind == KIND_NONE) {
        if (unlink(path) < 0 && errno != ENOENT) {
            lm_warn("lock: cannot remove %s: %s", path, strerror(errno));
            return false;
        }
        return true;
    }
    char dir[512], tmp[560];
    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir) ||
        snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid()) >= (int)sizeof(tmp))
        return false;
    mkdir_parents(dir);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        lm_warn("lock: cannot write %s: %s", tmp, strerror(errno));
        return false;
    }
    char salt[2 * SALT_LEN + 1], hash[2 * HASH_LEN + 1], text[512];
    lm_hex_encode(l->salt, SALT_LEN, salt);
    lm_hex_encode(l->hash, HASH_LEN, hash);
    int n = snprintf(text, sizeof(text),
                     "# MellaGUI screen lock (written by the settings panel). Only a salted PBKDF2-HMAC-SHA256 hash is kept.\n"
                     "# Forgot the secret? Delete this file from another login (ssh / another VT) and restart MellaGUI.\n"
                     "kind=%s\niter=%u\nsalt=%s\nhash=%s\ntimeout=%d\nstart=%d\nblank=%d\nvt=%d\n",
                     l->kind == KIND_PIN ? "pin" : "password", (unsigned)l->iter, salt, hash, l->timeout_min,
                     l->lock_on_start ? 1 : 0, l->blank_min, l->vt_switch ? 1 : 0);
    bool ok = n > 0 && n < (int)sizeof(text) && write(fd, text, (size_t)n) == n;
    if (ok && fsync(fd) < 0)
        ok = false;
    if (close(fd) < 0)
        ok = false;
    if (!ok || rename(tmp, path) < 0) {
        lm_warn("lock: cannot save %s: %s", path, strerror(errno));
        unlink(tmp);
        return false;
    }
    return true;
}

/* ---- locking / unlocking ---------------------------------------------------------------------------------------- */

/* Monitors on / in standby (DPMS). `blanked` only says that it was us. */
static void set_blank(struct lm_lock *l, bool blank)
{
    if (l->blanked == blank)
        return;
    l->blanked = blank;
    if (l->srv->backend)
        lm_backend_set_power(l->srv->backend, !blank);
    if (!blank)
        repaint(l);
    lm_info("lock: monitors %s", blank ? "in standby" : "on");
}

static void lock_enter(struct lm_lock *l)
{
    if (l->locked)
        return;
    l->locked = true;
    l->last_activity = lm_now_ms();               /* the standby timeout counts from now */
    wipe_input(l);
    l->message[0] = 0;
    l->shown_clock[0] = 0;
    l->shown_wait = -1;
    if (l->srv->seat)
        lm_seat_set_locked(l->srv->seat, true);
    if (l->srv->wm)
        lm_wm_lock_changed(l->srv->wm, true);
    repaint(l);
    announce(l);
    lm_info("lock: screen locked");
}

static void lock_leave(struct lm_lock *l)
{
    if (!l->locked)
        return;
    l->locked = false;
    l->quit_pending = false;
    set_blank(l, false);
    wipe_input(l);
    l->message[0] = 0;
    l->fails = 0;
    l->blocked_until = 0;
    l->last_activity = lm_now_ms();
    if (l->srv->seat)
        lm_seat_set_locked(l->srv->seat, false);
    if (l->srv->wm)
        lm_wm_lock_changed(l->srv->wm, false);
    repaint(l);
    announce(l);
    lm_info("lock: screen unlocked");
}

bool lm_lock_now(struct lm_lock *l)
{
    if (!l || l->kind == KIND_NONE)
        return false;
    lock_enter(l);
    return true;
}

bool lm_lock_active(const struct lm_lock *l) { return l && l->locked; }
bool lm_lock_configured(const struct lm_lock *l) { return l && l->kind != KIND_NONE; }
const char *lm_lock_kind(const struct lm_lock *l)
{
    return !l || l->kind == KIND_NONE ? "none" : l->kind == KIND_PIN ? "pin" : "password";
}
int lm_lock_timeout(const struct lm_lock *l) { return l ? l->timeout_min : 0; }
bool lm_lock_start(const struct lm_lock *l) { return l && l->lock_on_start; }
int lm_lock_blank(const struct lm_lock *l) { return l ? l->blank_min : 0; }
bool lm_lock_vt(const struct lm_lock *l) { return !l || l->vt_switch; }

bool lm_lock_request_quit(struct lm_lock *l)
{
    if (!l || l->kind == KIND_NONE)
        return false;
    if (!l->locked) {
        lock_enter(l);
        l->quit_pending = true;
        announce(l);
    }
    return true;
}
const char *lm_lock_status(const struct lm_lock *l) { return l ? l->status : ""; }

bool lm_lock_activity(struct lm_lock *l)
{
    if (!l)
        return false;
    l->last_activity = lm_now_ms();
    if (l->locked && l->blanked) {                  /* the first input only wakes the monitors */
        set_blank(l, false);
        return true;
    }
    return false;
}

/* ---- the lock screen's keyboard ------------------------------------------------------------------------------- */

static void utf8_pop(struct lm_lock *l)
{
    if (l->len == 0)
        return;
    while (l->len > 0 && ((unsigned char)l->input[l->len - 1] & 0xc0) == 0x80)
        l->len--;
    if (l->len > 0)
        l->len--;
    l->input[l->len] = 0;
    if (l->nchars > 0)
        l->nchars--;
}

static void try_unlock(struct lm_lock *l)
{
    int wait = wait_seconds(l);
    if (wait > 0) {
        wipe_input(l);
        return;
    }
    if (l->len == 0)
        return;
    bool ok = verify(l, l->input, l->len);
    wipe_input(l);
    if (ok) {
        bool quit = l->quit_pending;
        lock_leave(l);
        if (quit) {
            lm_info("lock: secret accepted, quitting as asked");
            lm_server_quit(l->srv);
        }
        return;
    }
    note_failure(l);
    snprintf(l->message, sizeof(l->message), "Wrong %s.", l->kind == KIND_PIN ? "PIN" : "password");
    lm_warn("lock: wrong secret (%d in a row)", l->fails);
    repaint(l);
    announce(l);
}

void lm_lock_key(struct lm_lock *l, uint32_t code, uint32_t mods, uint32_t group)
{
    if (!l || !l->locked)
        return;
    bool ctrl = (mods & LM_MOD_CTRL) != 0, alt = (mods & LM_MOD_ALT) != 0, super = (mods & LM_MOD_SUPER) != 0;

    if (ctrl && alt && !super) {                                  /* Ctrl+Alt+F1..F12: the other VTs have their own login */
        int vt = 0;
        if (code >= KEY_F1 && code <= KEY_F10)
            vt = (int)(code - KEY_F1) + 1;
        else if (code == KEY_F11)
            vt = 11;
        else if (code == KEY_F12)
            vt = 12;
        if (vt && l->vt_switch && l->srv->backend)
            lm_backend_switch_vt(l->srv->backend, vt);
        return;
    }
    if (super && code == KEY_SPACE && !ctrl && !alt) {            /* the secret may need another layout */
        if (l->srv->input)
            lm_input_cycle_layout(l->srv->input, (mods & LM_MOD_SHIFT) ? -1 : 1);
        repaint(l);
        return;
    }
    if (ctrl || alt || super)
        return;

    if (wait_seconds(l) > 0) {                                    /* in the delay: typing does nothing */
        wipe_input(l);
        return;
    }
    if (l->message[0]) {
        l->message[0] = 0;
        announce(l);
    }
    if (code == KEY_ENTER || code == KEY_KPENTER) {
        try_unlock(l);
        return;
    }
    if (code == KEY_BACKSPACE) {
        utf8_pop(l);
    } else if (code == KEY_ESC) {
        if (l->len == 0 && l->quit_pending) {                     /* Esc on an empty field: do not quit after all */
            l->quit_pending = false;
            announce(l);
        }
        wipe_input(l);
    } else {
        int layouts[LM_KBD_MAX_LAYOUTS], n = l->srv->config.kbd_nlayouts;
        if (n < 1 || n > LM_KBD_MAX_LAYOUTS)
            n = 0;
        for (int i = 0; i < n; i++)
            layouts[i] = l->srv->config.kbd_layouts[i];
        lm_keymap_set_groups(layouts, n);
        lm_keymap_client_group(group);
        if (code >= KEY_KP7 && code <= KEY_KPDOT)
            mods |= LM_MOD_NUM;                                   /* the keypad types digits here whatever NumLock says */
        uint32_t ks = lm_keymap_keysym(code, mods & ~(uint32_t)(LM_MOD_CTRL | LM_MOD_ALT | LM_MOD_SUPER));
        char txt[8];
        int k = ks ? lm_keysym_to_utf8(ks, txt) : 0;
        if (k <= 0)
            return;
        if (l->kind == KIND_PIN && (k != 1 || txt[0] < '0' || txt[0] > '9'))
            return;
        if (l->len + (size_t)k > LM_LOCK_MAX_SECRET)
            return;
        memcpy(l->input + l->len, txt, (size_t)k);
        l->len += (size_t)k;
        l->input[l->len] = 0;
        l->nchars++;
    }
    repaint(l);
}

/* The 1 s timer only runs while there is a lock to watch. */
static void update_timer(struct lm_lock *l)
{
    if (l->tick)
        lm_timer_arm_ms(l->tick, l->kind != KIND_NONE ? 1000 : 0, l->kind != KIND_NONE ? 1000 : 0);
}

/* ---- changing the lock -------------------------------------------------------------------------------------------- */

static bool valid_password(const char *s)
{
    size_t n = strlen(s);
    if (n < LM_LOCK_MIN_PASSWORD || n > LM_LOCK_MAX_SECRET)
        return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f)
            return false;
    return true;
}

static bool valid_pin(const char *s)
{
    size_t n = strlen(s);
    if (n < LM_LOCK_MIN_PIN || n > LM_LOCK_MAX_PIN)
        return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return false;
    return true;
}

bool lm_lock_apply(struct lm_lock *l, const char *old, const char *kind, const char *secret, const char *minutes,
                   const char *start, const char *blank, const char *vt)
{
    if (!l)
        return false;
    old = old ? old : "";
    kind = kind ? kind : "";
    secret = secret ? secret : "";
    minutes = minutes ? minutes : "";
    start = start ? start : "";
    blank = blank ? blank : "";
    vt = vt ? vt : "";
    bool ok = false;
    char st[24];
    struct lm_lock next;
    memset(&next, 0, sizeof(next));

    if (l->locked) {                                              /* nobody changes the lock behind a locked screen */
        set_status(l, "locked");
        goto done;
    }
    if (l->kind != KIND_NONE) {                                   /* changing an existing lock needs the secret */
        int wait = set_wait_seconds(l);
        if (wait > 0) {
            snprintf(st, sizeof(st), "wait:%d", wait);
            set_status(l, st);
            goto done;
        }
        if (!verify(l, old, strlen(old))) {
            note_set_failure(l);
            int w = set_wait_seconds(l);
            if (w > 0)
                snprintf(st, sizeof(st), "wait:%d", w);
            else
                snprintf(st, sizeof(st), "wrong");
            set_status(l, st);
            lm_warn("lock: wrong current secret in a settings change (%d in a row)", l->set_fails);
            goto done;
        }
        l->set_fails = 0;
        l->set_blocked_until = 0;
    }

    next = *l;
    wipe_input(&next);                                            /* a copy must not carry what is being typed */
    bool new_secret = false;
    if (!strcmp(kind, "none")) {
        next.kind = KIND_NONE;
        next.timeout_min = 0;
        next.lock_on_start = true;                                /* the defaults of the next lock */
        next.blank_min = DEFAULT_BLANK_MIN;
        next.vt_switch = true;
    } else if (!strcmp(kind, "password") || !strcmp(kind, "pin")) {
        bool pin = !strcmp(kind, "pin");
        if (!(pin ? valid_pin(secret) : valid_password(secret))) {
            set_status(l, "weak");
            goto done;
        }
        next.kind = pin ? KIND_PIN : KIND_PASSWORD;
        new_secret = true;
    } else if (!strcmp(kind, "keep")) {
        if (l->kind == KIND_NONE) {
            set_status(l, "invalid");
            goto done;
        }
    } else {
        set_status(l, "invalid");
        goto done;
    }
    if (next.kind != KIND_NONE) {
        if (*minutes) {
            char *end;
            long m = strtol(minutes, &end, 10);
            if (*end || m < 0 || m > MAX_TIMEOUT_MIN) {
                set_status(l, "invalid");
                goto done;
            }
            next.timeout_min = (int)m;
        }
        if (*start) {
            if (strcmp(start, "0") && strcmp(start, "1")) {
                set_status(l, "invalid");
                goto done;
            }
            next.lock_on_start = start[0] == '1';
        }
        if (*vt) {
            if (strcmp(vt, "0") && strcmp(vt, "1")) {
                set_status(l, "invalid");
                goto done;
            }
            next.vt_switch = vt[0] == '1';
        }
        if (*blank) {
            char *end;
            long m = strtol(blank, &end, 10);
            if (*end || m < 0 || m > MAX_BLANK_MIN) {
                set_status(l, "invalid");
                goto done;
            }
            next.blank_min = (int)m;
        }
    }
    if (new_secret) {
        if (l->kind == KIND_NONE && !*blank)
            next.blank_min = DEFAULT_BLANK_MIN;                   /* a fresh lock: monitors sleep 3 minutes after it locked */
        next.iter = iterations_for_new();
        if (getrandom(next.salt, SALT_LEN, 0) != SALT_LEN) {
            set_status(l, "error");
            goto done;
        }
        hash_secret(&next, secret, strlen(secret), next.hash);
    }
    if (!save(&next)) {
        set_status(l, "error");
        goto done;
    }
    l->kind = next.kind;
    l->iter = next.iter;
    memcpy(l->salt, next.salt, SALT_LEN);
    memcpy(l->hash, next.hash, HASH_LEN);
    l->timeout_min = next.timeout_min;
    l->lock_on_start = next.lock_on_start;
    l->blank_min = next.blank_min;
    l->vt_switch = next.vt_switch;
    l->last_activity = lm_now_ms();
    set_status(l, "ok");
    ok = true;
    lm_info("lock: now %s, timeout %d, lock at start %d, standby after %d, console switch %d", lm_lock_kind(l),
            l->timeout_min, l->lock_on_start ? 1 : 0, l->blank_min, l->vt_switch ? 1 : 0);
done:
    lm_secure_zero(&next, sizeof(next));
    update_timer(l);
    announce(l);
    return ok;
}

/* ---- timer, scene info ------------------------------------------------------------------------------------------ */

static void format_clock(const struct lm_lock *l, char *clock, size_t cn, char *date, size_t dn)
{
    time_t t = time(NULL);
    struct tm tm;
    if (!localtime_r(&t, &tm)) {
        clock[0] = date[0] = 0;
        return;
    }
    const struct lm_clockfmt f = l->srv->config.clock;                  /* the lock screen writes the time and the date like the panel does */
    lm_clock_time_text(&f, &tm, clock, cn);
    lm_clock_date_text(&f, &tm, date, dn);
}

static void tick_cb(void *data)
{
    struct lm_lock *l = data;
    if (l->kind == KIND_NONE)
        return;
    if (!l->locked) {
        if (lm_idle_inhibited(l->srv->idle_inhibit)) {
            l->last_activity = lm_now_ms();                       /* a film is on: the idle time starts when it is over */
            return;
        }
        if (l->timeout_min > 0 && lm_now_ms() - l->last_activity >= (uint64_t)l->timeout_min * unit_ms())
            lock_enter(l);
        return;
    }
    if (l->blanked)
        return;                                                   /* nobody is looking: no clock, no countdown */
    if (l->blank_min > 0 && lm_now_ms() - l->last_activity >= (uint64_t)l->blank_min * unit_ms()) {
        set_blank(l, true);
        return;
    }
    char clock[16], date[32];
    format_clock(l, clock, sizeof(clock), date, sizeof(date));
    int wait = wait_seconds(l);
    if (strcmp(clock, l->shown_clock) || wait != l->shown_wait) {
        snprintf(l->shown_clock, sizeof(l->shown_clock), "%s", clock);
        l->shown_wait = wait;
        repaint(l);
    }
}

void lm_lock_info(const struct lm_lock *l, struct lm_output *out, struct lm_lock_info *info)
{
    memset(info, 0, sizeof(*info));
    info->output = out;
    if (!l)
        return;
    info->pin = l->kind == KIND_PIN;
    info->nchars = l->nchars;
    int wait = wait_seconds(l);
    info->blocked = wait > 0;
    if (wait > 0)
        snprintf(info->message, sizeof(info->message), "Too many attempts. Try again in %d s.", wait);
    else if (l->message[0])
        snprintf(info->message, sizeof(info->message), "%s", l->message);
    else if (l->quit_pending)
        snprintf(info->message, sizeof(info->message), "Enter the secret to quit MellaGUI. Esc on an empty field cancels.");
    info->quit = l->quit_pending;
    format_clock(l, info->clock, sizeof(info->clock), info->date, sizeof(info->date));
    if (l->srv->input)
        snprintf(info->layout, sizeof(info->layout), "%s", lm_input_layout_id(l->srv->input, lm_input_layout(l->srv->input)));
    for (char *p = info->layout; *p; p++)
        if (*p >= 'a' && *p <= 'z')
            *p = (char)(*p - 'a' + 'A');
}

void lm_lock_describe(const struct lm_lock *l, char *out, size_t n)
{
    if (!l) {
        snprintf(out, n, "kind=none");
        return;
    }
    struct lm_lock_info info;
    lm_lock_info(l, NULL, &info);
    snprintf(out, n, "kind=%s locked=%d timeout=%d start=%d blank=%d blanked=%d vt=%d quit=%d typed=%d fails=%d wait=%d status=%s message=%s",
             lm_lock_kind(l), l->locked ? 1 : 0, l->timeout_min, l->lock_on_start ? 1 : 0, l->blank_min, l->blanked ? 1 : 0,
             l->vt_switch ? 1 : 0, l->quit_pending ? 1 : 0,
             l->nchars, l->fails,
             wait_seconds(l), l->status[0] ? l->status : "-", info.message[0] ? info.message : "-");
}

/* ---- lifecycle ------------------------------------------------------------------------------------------------ */

struct lm_lock *lm_lock_create(struct lm_server *srv)
{
    struct lm_lock *l = calloc(1, sizeof(*l));
    if (!l)
        return NULL;
    l->srv = srv;
    l->last_activity = lm_now_ms();
    l->shown_wait = -1;
    l->blank_min = DEFAULT_BLANK_MIN;                             /* what a new lock starts with (the panel shows it) */
    l->vt_switch = true;
    l->lock_on_start = true;
    load(l);
    l->tick = lm_loop_add_timer(srv->loop, tick_cb, l);
    update_timer(l);
    if (l->kind != KIND_NONE && l->lock_on_start)
        lock_enter(l);
    return l;
}

void lm_lock_destroy(struct lm_lock *l)
{
    if (!l)
        return;
    if (l->tick)
        lm_timer_destroy(l->tick);
    if (l->blanked && l->srv->backend)
        lm_backend_set_power(l->srv->backend, true);
    lm_secure_zero(l, sizeof(*l));
    free(l);
}
