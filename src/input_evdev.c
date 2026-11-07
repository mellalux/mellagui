/*
 * evdev input: keyboards, mice, touchpads, touchscreens; key repeat for
 * compositor shortcuts; modifier tracking; synthetic injection for tests.
 */
#include "input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "list.h"
#include "log.h"
#include "loop.h"
#include "settings.h"
#include "logind.h"
#include "sound.h"
#include "wm.h"

#define MAX_SLOTS 10
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define NBITS(x) ((((x) - 1) / BITS_PER_LONG) + 1)

enum dev_class {
    DEV_KEYBOARD = 1 << 0,
    DEV_POINTER = 1 << 1,
    DEV_TOUCHPAD = 1 << 2,   /* indirect absolute: relative motion + 2-finger scroll */
    DEV_DIRECT_ABS = 1 << 3, /* tablet / single-touch screen: absolute pointer */
    DEV_TOUCH_MT = 1 << 4,   /* multitouch touchscreen */
};

struct slot {
    bool active, changed;
    int32_t id;
    int32_t x, y;
};

struct device {
    struct lm_list link;
    struct lm_input *in;
    char path[320];
    int fd;
    struct lm_source *source;
    unsigned classes;
    bool grabbed;
    bool has_leds;          /* EV_LED with LED_CAPSL / LED_NUML: the lock lamps are driven from in->locked */
    /* relative accumulation */
    double rel_x, rel_y;
    int wheel, hwheel;
    /* absolute devices */
    struct input_absinfo abs_x, abs_y;
    int32_t cur_x, cur_y, last_x, last_y;
    bool have_x, have_y, touching, was_touching, moved;
    int fingers, last_fingers;
    double scroll_acc;
    struct slot slots[MAX_SLOTS];
    int cur_slot;
    bool touch_frame;
};

struct lm_input {
    struct lm_server *srv;
    lm_input_fn handler;
    void *handler_data;
    struct lm_list devices;
    struct lm_source *inotify_source;
    int inotify_fd;
    struct lm_timer *repeat_timer;
    bool active;
    bool headless;
    bool warned_eacces;
    bool hotplug_scan;          /* scanning because /dev/input changed: a fresh node may not have its ACL yet (udev) */
    uint16_t key_count[KEY_MAX + 1];
    uint32_t locked;       /* LM_MOD_CAPS / LM_MOD_NUM */
    /* keyboard layouts */
    int layouts[LM_KBD_MAX_LAYOUTS];
    int nlayouts;
    int slot;              /* active layout = XKB group */
    bool altgr;            /* right Alt is AltGr (LM_MOD_ALTGR) */
    /* sticky keys */
    bool sticky;
    uint32_t latched;      /* applies to the next key press only */
    uint32_t slocked;      /* locked by tapping a latched modifier again */
    bool tap_ok;           /* the modifier key that is down was pressed with no other key down */
    int shift_taps;        /* consecutive Shift presses (5 in a row toggle sticky keys) */
    const char *last_sound;
    unsigned sound_count;
    int32_t repeat_code;   /* -1 = none */
    int n_keyboards;
};

static void device_close(struct device *d);

/* Light the Caps / Num Lock lamps of every keyboard that has them (the kernel does not do it for a grabbed device,
 * and with the console in K_OFF the VT layer does not either). Best effort: a read-only node just stays dark. */
static void leds_sync_device(const struct lm_input *in, const struct device *d)
{
    if (!d->has_leds)
        return;
    struct input_event ev[3];
    memset(ev, 0, sizeof(ev));
    ev[0].type = EV_LED; ev[0].code = LED_CAPSL; ev[0].value = (in->locked & LM_MOD_CAPS) ? 1 : 0;
    ev[1].type = EV_LED; ev[1].code = LED_NUML;  ev[1].value = (in->locked & LM_MOD_NUM) ? 1 : 0;
    ev[2].type = EV_SYN; ev[2].code = SYN_REPORT;
    if (write(d->fd, ev, sizeof(ev)) < 0 && errno != EAGAIN)
        lm_debug("input: cannot set the lock LEDs of %s: %s", d->path, strerror(errno));
}

static void leds_sync(const struct lm_input *in)
{
    struct device *d;
    lm_list_for_each(d, &in->devices, link)
        leds_sync_device(in, d);
}

static inline bool bit_test(const unsigned long *a, int bit)
{
    return a[bit / BITS_PER_LONG] & (1UL << (bit % BITS_PER_LONG));
}

/* ---- modifiers & key handling -------------------------------------------- */

/* LM_MOD_* bit of a (non-lock) modifier key, 0 for every other key. */
static uint32_t key_mod_bit(const struct lm_input *in, uint32_t code)
{
    switch (code) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: return LM_MOD_SHIFT;
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL: return LM_MOD_CTRL;
    case KEY_LEFTALT: return LM_MOD_ALT;
    case KEY_RIGHTALT: return in->altgr ? LM_MOD_ALTGR : LM_MOD_ALT;
    case KEY_LEFTMETA: case KEY_RIGHTMETA: return LM_MOD_SUPER;
    }
    return 0;
}

static uint32_t mods_depressed(const struct lm_input *in)
{
    uint32_t m = 0;
    if (in->key_count[KEY_LEFTSHIFT] || in->key_count[KEY_RIGHTSHIFT]) m |= LM_MOD_SHIFT;
    if (in->key_count[KEY_LEFTCTRL] || in->key_count[KEY_RIGHTCTRL]) m |= LM_MOD_CTRL;
    if (in->key_count[KEY_LEFTALT]) m |= LM_MOD_ALT;
    if (in->key_count[KEY_RIGHTALT]) m |= in->altgr ? LM_MOD_ALTGR : LM_MOD_ALT;
    if (in->key_count[KEY_LEFTMETA] || in->key_count[KEY_RIGHTMETA]) m |= LM_MOD_SUPER;
    return m;
}

static bool is_modifier_key(uint32_t code)
{
    switch (code) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
    case KEY_LEFTALT: case KEY_RIGHTALT: case KEY_LEFTMETA: case KEY_RIGHTMETA:
    case KEY_CAPSLOCK: case KEY_NUMLOCK:
        return true;
    }
    return false;
}

static void emit(struct lm_input *in, struct lm_input_event *ev)
{
    if (ev->time_ms == 0)
        ev->time_ms = lm_now_msec32();
    if (in->handler)
        in->handler(ev, in->handler_data);
}

static void repeat_stop(struct lm_input *in)
{
    in->repeat_code = -1;
    if (in->repeat_timer)
        lm_timer_arm_ms(in->repeat_timer, 0, 0);
}

static void emit_key(struct lm_input *in, uint32_t code, bool pressed, bool repeat, uint32_t time_ms)
{
    struct lm_input_event ev = { .type = LM_INPUT_KEY, .time_ms = time_ms };
    ev.u.key.code = code;
    ev.u.key.pressed = pressed;
    ev.u.key.repeat = repeat;
    ev.u.key.mods_depressed = mods_depressed(in);
    ev.u.key.mods_latched = in->latched;
    ev.u.key.mods_locked = in->locked | in->slocked;
    ev.u.key.group = (uint32_t)in->slot;
    emit(in, &ev);
}

/* Modifier / layout state changed without a key event. */
static void emit_mods(struct lm_input *in)
{
    struct lm_input_event ev = { .type = LM_INPUT_MODS };
    ev.u.mods.depressed = mods_depressed(in);
    ev.u.mods.latched = in->latched;
    ev.u.mods.locked = in->locked | in->slocked;
    ev.u.mods.group = (uint32_t)in->slot;
    emit(in, &ev);
}

/* Feedback for a sticky-keys state change. */
static void sticky_sound(struct lm_input *in, enum lm_sound s)
{
    in->last_sound = lm_sound_name(s);
    in->sound_count++;
    if (!in->headless && in->srv->config.sound)
        lm_sound_play(s);
}

/* The feature itself was switched: the panel button must repaint even when no modifier bit changed. */
static void sticky_switched(struct lm_input *in)
{
    sticky_sound(in, in->sticky ? LM_SOUND_STICKY_ON : LM_SOUND_STICKY_OFF);
    if (in->srv->wm)
        lm_wm_keyboard_state_changed(in->srv->wm);
    lm_settings_changed(in->srv->settings);
}

static void sticky_clear(struct lm_input *in)
{
    in->latched = 0;
    in->slocked = 0;
    in->tap_ok = false;
}

/* A modifier key went down (first physical press). Returns true when the sticky state changed. */
static bool sticky_modifier_press(struct lm_input *in, uint32_t bit)
{
    bool changed = false;
    if (bit == LM_MOD_SHIFT) {
        if (++in->shift_taps >= 5) {                /* Shift x5: toggle sticky keys (like XKB's StickyKeys) */
            in->shift_taps = 0;
            in->sticky = !in->sticky;
            sticky_clear(in);
            sticky_switched(in);
            return true;
        }
    } else {
        in->shift_taps = 0;
    }
    if (!in->sticky)
        return false;
    bool others_down = false;
    for (uint32_t c = 1; c <= KEY_MAX && !others_down; c++)
        others_down = in->key_count[c] > 0 && key_mod_bit(in, c) != bit;
    /* (this key's own count is already 1; a second keyboard's copy of the same modifier does not matter) */
    in->tap_ok = !others_down;
    if (in->slocked & bit) {                        /* locked: a further tap unlocks */
        in->slocked &= ~bit;
        in->latched &= ~bit;
        in->tap_ok = false;
        changed = true;
        sticky_sound(in, LM_SOUND_UNLOCK);
    } else if (in->latched & bit) {                 /* latched: a second tap locks */
        in->latched &= ~bit;
        in->slocked |= bit;
        in->tap_ok = false;
        changed = true;
        sticky_sound(in, LM_SOUND_LOCK);
    }
    return changed;
}

/* A modifier key was released (last physical copy). A clean tap latches it. */
static void sticky_modifier_release(struct lm_input *in, uint32_t bit)
{
    if (in->sticky && in->tap_ok && !(in->slocked & bit)) {
        in->latched |= bit;
        sticky_sound(in, LM_SOUND_LATCH);
    }
    in->tap_ok = false;
}

static void handle_key(struct lm_input *in, uint32_t code, bool pressed, uint32_t time_ms)
{
    if (code > KEY_MAX)
        return;
    uint32_t bit = key_mod_bit(in, code);
    bool sticky_changed = false;
    if (pressed) {
        if (in->key_count[code]++ > 0)
            return; /* already down on another keyboard */
        if (code == KEY_CAPSLOCK || code == KEY_NUMLOCK) {
            in->locked ^= code == KEY_CAPSLOCK ? LM_MOD_CAPS : LM_MOD_NUM;
            leds_sync(in);
            if (in->srv->wm)
                lm_wm_keyboard_state_changed(in->srv->wm);
        }
        if (bit)
            sticky_changed = sticky_modifier_press(in, bit);
        else
            in->shift_taps = 0;
        if (!bit && in->sticky)
            in->tap_ok = false;                     /* a modifier that is still down is now part of a chord */
    } else {
        if (in->key_count[code] == 0)
            return;
        if (--in->key_count[code] > 0)
            return;
        if (bit)
            sticky_modifier_release(in, bit);
    }
    emit_key(in, code, pressed, false, time_ms);
    if (pressed && !bit && in->latched) {           /* the latch is used up by this key press */
        in->latched = 0;
        sticky_changed = true;
    }
    if (sticky_changed)
        emit_mods(in);
    if (pressed) {
        if (!is_modifier_key(code) && in->srv->config.repeat_rate > 0) {
            in->repeat_code = (int32_t)code;
            uint32_t interval = 1000u / (uint32_t)in->srv->config.repeat_rate;
            lm_timer_arm_ms(in->repeat_timer, (uint32_t)in->srv->config.repeat_delay_ms,
                            interval ? interval : 1);
        } else if (!is_modifier_key(code)) {
            repeat_stop(in);
        }
    } else if (in->repeat_code == (int32_t)code) {
        repeat_stop(in);
    }
}

static void repeat_timer_fn(void *data)
{
    struct lm_input *in = data;
    if (in->repeat_code < 0 || !in->key_count[in->repeat_code]) {
        repeat_stop(in);
        return;
    }
    emit_key(in, (uint32_t)in->repeat_code, true, true, 0);
}

/* ---- pointer helpers ----------------------------------------------------------- */

static double accel(const struct lm_input *in, double d)
{
    double mag = fabs(d);
    double f = mag <= 4.0 ? 1.0 : 1.0 + (mag - 4.0) * 0.06;
    if (f > 3.0)
        f = 3.0;
    return d * f * in->srv->config.pointer_speed;
}

static void emit_motion(struct lm_input *in, double dx, double dy)
{
    if (dx == 0 && dy == 0)
        return;
    struct lm_input_event ev = { .type = LM_INPUT_POINTER_MOTION };
    ev.u.motion.dx = accel(in, dx);
    ev.u.motion.dy = accel(in, dy);
    emit(in, &ev);
}

static void emit_button(struct lm_input *in, uint32_t code, bool pressed)
{
    struct lm_input_event ev = { .type = LM_INPUT_POINTER_BUTTON };
    ev.u.button.code = code;
    ev.u.button.pressed = pressed;
    emit(in, &ev);
}

static void emit_axis(struct lm_input *in, int orientation, double value, int32_t discrete)
{
    struct lm_input_event ev = { .type = LM_INPUT_POINTER_AXIS };
    if (in->srv->config.natural_scroll) {
        value = -value;
        discrete = -discrete;
    }
    ev.u.axis.orientation = orientation;
    ev.u.axis.value = value;
    ev.u.axis.discrete = discrete;
    emit(in, &ev);
}

static double norm(int32_t v, const struct input_absinfo *ai)
{
    int32_t range = ai->maximum - ai->minimum;
    if (range <= 0)
        return 0;
    return LM_CLAMP((double)(v - ai->minimum) / (double)range, 0.0, 1.0);
}

/* ---- SYN_REPORT processing per device ------------------------------------------ */

static void device_sync(struct device *d)
{
    struct lm_input *in = d->in;

    if (d->wheel) {
        emit_axis(in, LM_POINTER_AXIS_VERTICAL, -15.0 * d->wheel, -d->wheel);
        d->wheel = 0;
    }
    if (d->hwheel) {
        emit_axis(in, LM_POINTER_AXIS_HORIZONTAL, 15.0 * d->hwheel, d->hwheel);
        d->hwheel = 0;
    }
    if (d->rel_x != 0 || d->rel_y != 0) {
        emit_motion(in, d->rel_x, d->rel_y);
        d->rel_x = d->rel_y = 0;
    }

    if (d->classes & DEV_TOUCHPAD) {
        bool two = d->fingers >= 2;
        if (d->touching && d->was_touching && d->have_x && d->have_y &&
            d->fingers == d->last_fingers) {
            double dx = d->cur_x - d->last_x, dy = d->cur_y - d->last_y;
            /* scale device units to roughly pixels */
            double sx = (double)(d->abs_x.maximum - d->abs_x.minimum);
            double sy = (double)(d->abs_y.maximum - d->abs_y.minimum);
            double ux = sx > 0 ? 1400.0 / sx : 1.0, uy = sy > 0 ? 900.0 / sy : 1.0;
            if (two) {
                d->scroll_acc += dy * uy;
                if (fabs(d->scroll_acc) >= 1.0) {
                    emit_axis(in, LM_POINTER_AXIS_VERTICAL, d->scroll_acc, 0);
                    d->scroll_acc = 0;
                }
            } else if (d->fingers == 1) {
                emit_motion(in, dx * ux * 0.5, dy * uy * 0.5);
            }
        }
        d->last_x = d->cur_x;
        d->last_y = d->cur_y;
        d->last_fingers = d->fingers;
        d->was_touching = d->touching;
        if (!d->touching)
            d->scroll_acc = 0;
    } else if ((d->classes & DEV_DIRECT_ABS) && d->moved) {
        struct lm_input_event ev = { .type = LM_INPUT_POINTER_MOTION_ABS };
        ev.u.motion_abs.x = norm(d->cur_x, &d->abs_x);
        ev.u.motion_abs.y = norm(d->cur_y, &d->abs_y);
        emit(in, &ev);
        d->moved = false;
    }

    if (d->classes & DEV_TOUCH_MT && d->touch_frame) {
        for (int i = 0; i < MAX_SLOTS; i++) {
            struct slot *s = &d->slots[i];
            if (!s->changed)
                continue;
            s->changed = false;
            struct lm_input_event ev = { .type = s->active ? LM_INPUT_TOUCH_MOTION : LM_INPUT_TOUCH_UP };
            ev.u.touch.id = i;
            ev.u.touch.x = norm(s->x, &d->abs_x);
            ev.u.touch.y = norm(s->y, &d->abs_y);
            if (s->id == -2) { /* new contact */
                ev.type = LM_INPUT_TOUCH_DOWN;
                s->id = i;
            }
            emit(in, &ev);
        }
        struct lm_input_event fr = { .type = LM_INPUT_TOUCH_FRAME };
        emit(in, &fr);
        d->touch_frame = false;
    }
}

static void device_event(struct device *d, const struct input_event *e)
{
    struct lm_input *in = d->in;
    uint32_t t = (uint32_t)(e->time.tv_sec * 1000 + e->time.tv_usec / 1000);
    (void)t;

    switch (e->type) {
    case EV_KEY:
        if (e->code < BTN_MISC) {
            if (!(d->classes & DEV_KEYBOARD) || e->value == 2)
                break;
            handle_key(in, e->code, e->value != 0, lm_now_msec32());
        } else if (e->code >= BTN_MOUSE && e->code < BTN_JOYSTICK) {
            if (e->value == 2)
                break;
            if (d->classes & (DEV_POINTER | DEV_TOUCHPAD))
                emit_button(in, e->code, e->value != 0);
        } else if (e->code == BTN_TOUCH) {
            d->touching = e->value != 0;
            if ((d->classes & DEV_DIRECT_ABS) && !(d->classes & DEV_TOUCH_MT))
                emit_button(in, BTN_LEFT, e->value != 0);
        } else if (e->code == BTN_TOOL_FINGER) {
            d->fingers = e->value ? 1 : (d->fingers == 1 ? 0 : d->fingers);
        } else if (e->code == BTN_TOOL_DOUBLETAP) {
            d->fingers = e->value ? 2 : (d->fingers == 2 ? 1 : d->fingers);
        } else if (e->code == BTN_TOOL_TRIPLETAP) {
            d->fingers = e->value ? 3 : (d->fingers == 3 ? 2 : d->fingers);
        }
        break;
    case EV_REL:
        switch (e->code) {
        case REL_X: d->rel_x += e->value; break;
        case REL_Y: d->rel_y += e->value; break;
        case REL_WHEEL: d->wheel += e->value; break;
        case REL_HWHEEL: d->hwheel += e->value; break;
        }
        break;
    case EV_ABS:
        if (d->classes & DEV_TOUCH_MT) {
            struct slot *s = &d->slots[d->cur_slot];
            switch (e->code) {
            case ABS_MT_SLOT:
                if (e->value >= 0 && e->value < MAX_SLOTS)
                    d->cur_slot = e->value;
                break;
            case ABS_MT_TRACKING_ID:
                if (e->value >= 0) {
                    s->active = true;
                    s->id = -2;
                } else {
                    s->active = false;
                }
                s->changed = true;
                d->touch_frame = true;
                break;
            case ABS_MT_POSITION_X: s->x = e->value; s->changed = true; d->touch_frame = true; break;
            case ABS_MT_POSITION_Y: s->y = e->value; s->changed = true; d->touch_frame = true; break;
            }
            break;
        }
        if (e->code == ABS_X) {
            d->cur_x = e->value;
            d->have_x = true;
            d->moved = true;
        } else if (e->code == ABS_Y) {
            d->cur_y = e->value;
            d->have_y = true;
            d->moved = true;
        }
        break;
    case EV_SYN:
        if (e->code == SYN_REPORT)
            device_sync(d);
        else if (e->code == SYN_DROPPED) {
            memset(d->slots, 0, sizeof(d->slots));
            d->rel_x = d->rel_y = 0;
        }
        break;
    }
}

static void device_fd_cb(int fd, uint32_t events, void *data)
{
    struct device *d = data;
    struct lm_input *in = d->in;
    struct input_event evs[64];

    if (events & (EPOLLERR | EPOLLHUP)) {
        lm_info("input: %s went away", d->path);
        device_close(d);
        return;
    }
    for (;;) {
        ssize_t n = read(fd, evs, sizeof(evs));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN) {
                lm_info("input: read error on %s: %s", d->path, strerror(errno));
                device_close(d);
            }
            return;
        }
        if (n == 0) {
            device_close(d);
            return;
        }
        size_t cnt = (size_t)n / sizeof(evs[0]);
        for (size_t i = 0; i < cnt; i++) {
            device_event(d, &evs[i]);
            if (!in->active)
                return;
        }
        if ((size_t)n < sizeof(evs))
            return;
    }
}

/* ---- device management -------------------------------------------------------------- */

static void device_close(struct device *d)
{
    struct lm_input *in = d->in;
    if (d->classes & DEV_KEYBOARD) {
        in->n_keyboards--;
        /* release keys that were down so nothing sticks */
    }
    if (d->source)
        lm_source_remove(d->source);
    if (d->grabbed)
        ioctl(d->fd, EVIOCGRAB, 0);
    lm_dev_close(in->srv->logind, d->fd);
    lm_list_remove(&d->link);
    free(d);
}

static bool device_known(const struct lm_input *in, const char *path)
{
    struct device *d;
    lm_list_for_each(d, &in->devices, link)
        if (!strcmp(d->path, path))
            return true;
    return false;
}

/* The device node as an fd (open(2), or logind's TakeDevice); returns the fd or -errno. A node of an inactive session
 * is of no use: it is handed back at once. */
static int open_node(struct lm_input *in, const char *path, int flags)
{
    bool inactive = false;
    int fd = lm_dev_open(in->srv->logind, path, flags, &inactive);
    if (fd >= 0 && inactive) {
        lm_dev_close(in->srv->logind, fd);
        return -EAGAIN;
    }
    return fd;
}

static void device_open(struct lm_input *in, const char *path)
{
    int fd = open_node(in, path, O_RDWR | O_NONBLOCK);
    if ((fd == -EACCES || fd == -EPERM || fd == -EROFS) && !in->srv->logind)
        fd = open_node(in, path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (fd == -EACCES && in->hotplug_scan) {
            lm_debug("input: %s is not accessible yet (udev still setting permissions); waiting for its attribute change", path);
        } else if (fd == -EACCES && !in->warned_eacces) {
            in->warned_eacces = true;
            lm_error("input: no permission to open %s. Add yourself to group 'input' "
                     "(sudo usermod -aG input $USER, then log out and in again).", path);
        } else if (fd == -EPERM && in->srv->logind && !in->warned_eacces) {
            in->warned_eacces = true;
            lm_warn("input: logind refused %s (is it a device of another seat?)", path);
        } else if (fd != -EACCES) {
            lm_debug("input: cannot open %s: %s", path, strerror(-fd));
        }
        return;
    }

    unsigned long evbits[NBITS(EV_MAX + 1)] = { 0 };
    unsigned long keybits[NBITS(KEY_MAX + 1)] = { 0 };
    unsigned long relbits[NBITS(REL_MAX + 1)] = { 0 };
    unsigned long absbits[NBITS(ABS_MAX + 1)] = { 0 };
    unsigned long propbits[NBITS(INPUT_PROP_MAX + 1)] = { 0 };
    ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits);
    if (bit_test(evbits, EV_KEY))
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
    if (bit_test(evbits, EV_REL))
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits);
    if (bit_test(evbits, EV_ABS))
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
    ioctl(fd, EVIOCGPROP(sizeof(propbits)), propbits);

    unsigned classes = 0;
    if (bit_test(evbits, EV_KEY) && bit_test(keybits, KEY_A) && bit_test(keybits, KEY_Z) &&
        bit_test(keybits, KEY_ENTER))
        classes |= DEV_KEYBOARD;
    if (bit_test(evbits, EV_REL) && bit_test(relbits, REL_X) && bit_test(relbits, REL_Y) &&
        bit_test(keybits, BTN_LEFT))
        classes |= DEV_POINTER;
    if (bit_test(evbits, EV_ABS) && bit_test(absbits, ABS_X) && bit_test(absbits, ABS_Y)) {
        bool direct = bit_test(propbits, INPUT_PROP_DIRECT);
        bool mt = bit_test(absbits, ABS_MT_POSITION_X) && bit_test(absbits, ABS_MT_SLOT);
        if (bit_test(keybits, BTN_TOOL_FINGER) && !direct)
            classes |= DEV_TOUCHPAD;
        else if (direct || bit_test(keybits, BTN_TOUCH) || bit_test(keybits, BTN_TOOL_PEN)) {
            classes |= DEV_DIRECT_ABS;
            if (mt)
                classes |= DEV_TOUCH_MT;
        }
    }
    if (!classes) {
        lm_dev_close(in->srv->logind, fd);
        return;
    }

    struct device *d = calloc(1, sizeof(*d));
    if (!d) {
        lm_dev_close(in->srv->logind, fd);
        return;
    }
    d->in = in;
    d->fd = fd;
    d->classes = classes;
    snprintf(d->path, sizeof(d->path), "%s", path);
    if (classes & (DEV_TOUCHPAD | DEV_DIRECT_ABS)) {
        int ax = (classes & DEV_TOUCH_MT) ? ABS_MT_POSITION_X : ABS_X;
        int ay = (classes & DEV_TOUCH_MT) ? ABS_MT_POSITION_Y : ABS_Y;
        ioctl(fd, EVIOCGABS(ax), &d->abs_x);
        ioctl(fd, EVIOCGABS(ay), &d->abs_y);
    }
    if (in->srv->config.grab_input && (classes & DEV_KEYBOARD)) {
        if (ioctl(fd, EVIOCGRAB, 1) == 0)
            d->grabbed = true;
        else
            lm_warn("input: EVIOCGRAB failed on %s: %s", path, strerror(errno));
    }
    d->source = lm_loop_add_fd(in->srv->loop, fd, EPOLLIN, device_fd_cb, d);
    if (!d->source) {
        if (d->grabbed)
            ioctl(fd, EVIOCGRAB, 0);
        lm_dev_close(in->srv->logind, fd);
        free(d);
        return;
    }
    lm_list_append(&in->devices, &d->link);
    if (classes & DEV_KEYBOARD) {
        in->n_keyboards++;
        unsigned long ledbits[NBITS(LED_MAX + 1)] = { 0 };
        if (bit_test(evbits, EV_LED) && ioctl(fd, EVIOCGBIT(EV_LED, sizeof(ledbits)), ledbits) >= 0 &&
            (bit_test(ledbits, LED_CAPSL) || bit_test(ledbits, LED_NUML))) {
            d->has_leds = true;
            leds_sync_device(in, d);
        }
    }
    char name[96] = "?";
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    lm_info("input: %s \"%s\" (%s%s%s%s)", path, name,
            classes & DEV_KEYBOARD ? "keyboard " : "",
            classes & DEV_POINTER ? "pointer " : "",
            classes & DEV_TOUCHPAD ? "touchpad " : "",
            classes & DEV_DIRECT_ABS ? "touch/tablet" : "");
}

static void scan_devices(struct lm_input *in)
{
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        lm_warn("input: cannot read /dev/input: %s", strerror(errno));
        return;
    }
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (strncmp(de->d_name, "event", 5) != 0)
            continue;
        char path[320];
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        if (!device_known(in, path))
            device_open(in, path);
    }
    closedir(dir);
}

static void inotify_cb(int fd, uint32_t events, void *data)
{
    struct lm_input *in = data;
    char buf[2048];
    (void)events;
    while (read(fd, buf, sizeof(buf)) > 0)
        ;
    if (in->active) {
        in->hotplug_scan = true;
        scan_devices(in);
        in->hotplug_scan = false;
    }
}

/* ---- public API ------------------------------------------------------------------------ */

struct lm_input *lm_input_create(struct lm_server *srv)
{
    struct lm_input *in = calloc(1, sizeof(*in));
    if (!in)
        return NULL;
    in->srv = srv;
    in->active = true;
    in->inotify_fd = -1;
    in->repeat_code = -1;
    lm_list_init(&in->devices);
    in->headless = srv->config.backend && !strcmp(srv->config.backend, "headless");
    in->nlayouts = srv->config.kbd_nlayouts > 0 ? srv->config.kbd_nlayouts : 1;
    for (int i = 0; i < in->nlayouts; i++)
        in->layouts[i] = srv->config.kbd_nlayouts > 0 ? srv->config.kbd_layouts[i] : 0;
    in->altgr = lm_layout_needs_altgr(in->layouts, in->nlayouts);
    for (int s = 0; s < in->nlayouts && srv->config.kbd_active >= 0; s++)
        if (in->layouts[s] == srv->config.kbd_active)
            in->slot = s;
    in->sticky = srv->config.sticky_keys;
    in->repeat_timer = lm_loop_add_timer(srv->loop, repeat_timer_fn, in);
    if (!in->headless) {
        scan_devices(in);
        in->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (in->inotify_fd >= 0 &&
            inotify_add_watch(in->inotify_fd, "/dev/input", IN_CREATE | IN_ATTRIB | IN_DELETE) >= 0)
            in->inotify_source = lm_loop_add_fd(srv->loop, in->inotify_fd, EPOLLIN, inotify_cb, in);
        if (in->n_keyboards == 0)
            lm_warn("input: no keyboard found (see messages above)");
    }
    return in;
}

static void close_all_devices(struct lm_input *in)
{
    struct device *d, *tmp;
    lm_list_for_each_safe(d, tmp, &in->devices, link)
        device_close(d);
}

void lm_input_destroy(struct lm_input *in)
{
    if (!in)
        return;
    close_all_devices(in);
    if (in->inotify_source)
        lm_source_remove(in->inotify_source);
    if (in->inotify_fd >= 0)
        close(in->inotify_fd);
    if (in->repeat_timer)
        lm_timer_destroy(in->repeat_timer);
    free(in);
}

void lm_input_set_handler(struct lm_input *in, lm_input_fn fn, void *data)
{
    in->handler = fn;
    in->handler_data = data;
}

void lm_input_set_active(struct lm_input *in, bool active)
{
    if (in->active == active)
        return;
    in->active = active;
    repeat_stop(in);
    if (!active) {
        /* Synthesize releases so no key or modifier stays stuck in the seat. */
        for (uint32_t c = 0; c <= KEY_MAX; c++) {
            if (in->key_count[c]) {
                in->key_count[c] = 0;
                emit_key(in, c, false, false, 0);
            }
        }
        if (in->latched || in->slocked) {
            sticky_clear(in);
            emit_mods(in);
        }
        close_all_devices(in);
    } else if (!in->headless) {
        scan_devices(in);
    }
}

void lm_input_inject(struct lm_input *in, const struct lm_input_event *ev)
{
    struct lm_input_event e = *ev;
    if (e.type == LM_INPUT_KEY) {
        handle_key(in, e.u.key.code, e.u.key.pressed, e.time_ms ? e.time_ms : lm_now_msec32());
        return;
    }
    emit(in, &e);
}

int lm_input_layout_count(const struct lm_input *in) { return in->nlayouts; }
int lm_input_layout(const struct lm_input *in) { return in->slot; }
const char *lm_input_layout_id(const struct lm_input *in, int slot)
{
    return lm_layout_id(slot >= 0 && slot < in->nlayouts ? in->layouts[slot] : 0);
}

void lm_input_set_layout(struct lm_input *in, int slot)
{
    if (slot < 0 || slot >= in->nlayouts || slot == in->slot)
        return;
    in->slot = slot;
    emit_mods(in);
    lm_settings_changed(in->srv->settings);
}

void lm_input_set_layouts(struct lm_input *in, const int *layouts, int n, int active)
{
    if (n < 1 || n > LM_KBD_MAX_LAYOUTS)
        return;
    int old = in->nlayouts > 0 ? in->layouts[in->slot] : 0;
    if (active < 0)
        active = old;
    memcpy(in->layouts, layouts, sizeof(int) * (size_t)n);
    in->nlayouts = n;
    in->slot = 0;
    for (int s = 0; s < n; s++)
        if (layouts[s] == active)
            in->slot = s;
    in->altgr = lm_layout_needs_altgr(in->layouts, n);
    sticky_clear(in);
    emit_mods(in);
    if (in->srv->wm)
        lm_wm_keyboard_state_changed(in->srv->wm);
    lm_settings_changed(in->srv->settings);
}

void lm_input_cycle_layout(struct lm_input *in, int delta)
{
    if (in->nlayouts > 1)
        lm_input_set_layout(in, ((in->slot + delta) % in->nlayouts + in->nlayouts) % in->nlayouts);
}

bool lm_input_sticky(const struct lm_input *in) { return in->sticky; }
uint32_t lm_input_latched(const struct lm_input *in) { return in->latched; }
uint32_t lm_input_sticky_locked(const struct lm_input *in) { return in->slocked; }
uint32_t lm_input_locks(const struct lm_input *in) { return in->locked; }

void lm_input_set_sticky(struct lm_input *in, bool on)
{
    if (in->sticky == on)
        return;
    in->sticky = on;
    in->shift_taps = 0;
    sticky_clear(in);
    sticky_switched(in);
    emit_mods(in);
}

const char *lm_input_last_sound(const struct lm_input *in) { return in->last_sound ? in->last_sound : "none"; }
unsigned lm_input_sound_count(const struct lm_input *in) { return in->sound_count; }

int lm_input_count_keyboards(const struct lm_input *in) { return in->n_keyboards; }
