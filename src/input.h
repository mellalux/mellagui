#ifndef LUMEN_INPUT_H
#define LUMEN_INPUT_H

/*
 * evdev input. Implemented by input_evdev.c. Produces struct lm_input_event
 * for the seat module; also provides synthetic injection for the headless
 * test backend.
 *
 * Responsibilities of input_evdev.c:
 *  - open /dev/input/event* directly (no logind) and
 *    hot-plug via inotify on /dev/input; classify with EVIOCGBIT into
 *    keyboard / pointer / touch(pad/screen)
 *  - EACCES: log ONE clear error telling the user to add themself to group
 *    'input' (e.g. `sudo usermod -aG input $USER`, re-login) or run from a
 *    logind seat session; keep running (other devices / injection still work)
 *  - EVIOCGRAB devices if config.grab_input (so keys do not leak to the tty)
 *  - key auto-repeat (timerfd, config.repeat_delay_ms / repeat_rate): repeats
 *    are emitted as key events with repeat = true. The seat does NOT forward
 *    them to clients (clients repeat themselves using wl_keyboard.repeat_info);
 *    they exist for compositor shortcuts (wm). The repeat stops on release of
 *    that key or when another key is pressed.
 *  - relative pointer with simple acceleration (config.pointer_speed)
 *  - absolute devices: direct-input devices (touchscreens, tablets, INPUT_PROP_DIRECT)
 *    -> LM_INPUT_POINTER_MOTION_ABS / touch events normalised to 0..1;
 *    touchpads (BTN_TOOL_FINGER, buttonpad) -> relative motion + buttons + 2-finger scroll
 *  - REL_WHEEL / REL_HWHEEL (+ HI_RES variants) -> axis events
 *  - modifier tracking across all keyboards (shared state), see mods fields
 */

#include <stdbool.h>
#include <stdint.h>
#include "keymap.h"
#include "server.h"

enum lm_input_type {
    LM_INPUT_KEY,
    LM_INPUT_POINTER_MOTION,       /* relative, already accelerated */
    LM_INPUT_POINTER_MOTION_ABS,   /* absolute, normalised 0..1 over the whole layout */
    LM_INPUT_POINTER_BUTTON,
    LM_INPUT_POINTER_AXIS,
    LM_INPUT_TOUCH_DOWN,
    LM_INPUT_TOUCH_MOTION,
    LM_INPUT_TOUCH_UP,
    LM_INPUT_TOUCH_CANCEL,         /* cancel all touch points */
    LM_INPUT_TOUCH_FRAME,
    LM_INPUT_MODS,                 /* modifier / layout state changed without a key (sticky keys, layout switch) */
};

struct lm_input_event {
    enum lm_input_type type;
    uint32_t time_ms;              /* lm_now_msec32() when it happened */
    union {
        struct {
            uint32_t code;         /* evdev KEY_* */
            bool pressed;
            bool repeat;           /* synthetic auto-repeat (pressed is true) */
            /*
             * Modifier state AFTER this key was applied (LM_MOD_*): the
             * input module tracks Shift/Ctrl/Alt/Super (depressed) and
             * Caps/Num (locked, toggled on press).
             */
            uint32_t mods_depressed;
            uint32_t mods_locked;
            /*
             * Sticky keys: latched modifiers (consumed by the next key press) and sticky-locked ones (part of
             * mods_locked together with Caps/Num). A key's own event still shows the latch it consumes; a
             * separate LM_INPUT_MODS event follows once the latch is cleared.
             */
            uint32_t mods_latched;
            uint32_t group;        /* active keyboard layout (XKB group) */
        } key;
        struct { uint32_t depressed, latched, locked, group; } mods;
        struct { double dx, dy; } motion;         /* pixels */
        struct { double x, y; } motion_abs;       /* 0..1 */
        struct { uint32_t code; bool pressed; } button;   /* evdev BTN_* */
        struct {
            int orientation;       /* LM_POINTER_AXIS_VERTICAL / HORIZONTAL (protocol.h) */
            double value;          /* wl_pointer.axis value; positive = down/right, ~15 per notch */
            int32_t discrete;      /* wheel steps (+1 = one notch down/right), 0 for smooth */
        } axis;
        struct { int32_t id; double x, y; } touch; /* x,y normalised 0..1 */
    } u;
};

typedef void (*lm_input_fn)(const struct lm_input_event *ev, void *data);

/*
 * Creates the input subsystem: scans devices (unless server->config.backend
 * is "headless", where only injection is active), starts inotify hot-plug.
 * Never fails because of missing permissions; returns NULL only on OOM.
 */
struct lm_input *lm_input_create(struct lm_server *srv);
void lm_input_destroy(struct lm_input *in);

/* Where events go (main.c sets this to lm_seat_handle_input). */
void lm_input_set_handler(struct lm_input *in, lm_input_fn fn, void *data);

/* Session activity (VT switch): when false stop reading + release grabs and
 * reset key/modifier/repeat state; when true re-scan and resume. */
void lm_input_set_active(struct lm_input *in, bool active);

/*
 * Synthetic event (tests / control socket). Goes through the same
 * modifier tracking and repeat logic as a real device and then the handler.
 * For key events only u.key.code/pressed need to be filled; time_ms == 0
 * means "now".
 */
void lm_input_inject(struct lm_input *in, const struct lm_input_event *ev);

/*
 * Keyboard layouts and sticky keys. The state lives here (config.kbd_layouts / config.sticky_keys give the start
 * values); every change reaches the seat as an LM_INPUT_MODS event so clients get a wl_keyboard.modifiers.
 *  - layout `slot` indexes the configured list (= XKB group of the generated keymap)
 *  - sticky keys: tapping Shift/Ctrl/Alt/AltGr/Super latches it for the next key press, tapping it twice locks it,
 *    a third tap unlocks; pressing Shift five times in a row toggles the feature.
 */
int lm_input_layout_count(const struct lm_input *in);
int lm_input_layout(const struct lm_input *in);
const char *lm_input_layout_id(const struct lm_input *in, int slot);     /* "ee" */
void lm_input_set_layout(struct lm_input *in, int slot);
/* Settings: replace the configured list (lm_layout_*() indices, 1..LM_KBD_MAX_LAYOUTS). `active` is the layout (not
 * slot) to stay on, -1 = keep the current one if it is still in the list, else the first. The caller has already
 * given the seat the matching keymap (lm_seat_set_layouts). */
void lm_input_set_layouts(struct lm_input *in, const int *layouts, int n, int active);
void lm_input_cycle_layout(struct lm_input *in, int delta);              /* +1 next, -1 previous */
bool lm_input_sticky(const struct lm_input *in);
void lm_input_set_sticky(struct lm_input *in, bool on);
uint32_t lm_input_latched(const struct lm_input *in);                    /* LM_MOD_* */
uint32_t lm_input_sticky_locked(const struct lm_input *in);              /* LM_MOD_* */
/* Sticky-key feedback beeps (sound.h): played on drm, only recorded on headless (the `kbd` oracle shows them). */
const char *lm_input_last_sound(const struct lm_input *in);              /* "none" until the first one */
unsigned lm_input_sound_count(const struct lm_input *in);

/* Diagnostics: number of open keyboards. */
int lm_input_count_keyboards(const struct lm_input *in);

#endif
