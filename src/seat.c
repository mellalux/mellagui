/*
 * wl_seat (pointer, keyboard, touch), wl_data_device_manager (text clipboard,
 * drag-and-drop is accepted but cancelled), cursor handling and pointer grabs.
 *
 * Everything runs on the single event-loop thread. Surfaces that hold focus
 * are watched through their destroy_signal; wl_pointer/wl_keyboard/wl_touch/
 * wl_data_* resources are kept in per-seat lists so events can be fanned out
 * to every object a client created.
 */


#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "backend.h"
#include "input.h"
#include "keymap.h"
#include "log.h"
#include "output.h"
#include "protocol.h"
#include "render.h"
#include "seat.h"
#include "lock.h"
#include "server.h"
#include "surface.h"
#include "wm.h"

#define MAX_KEYS        64
#define MAX_BUTTONS     16
#define MAX_SERIALS     32
#define MAX_TOUCH       10
#define MAX_MIMES       64
#define MAX_MIME_LEN    255
#define SEAT_NAME       "seat0"

/* ---- state ---------------------------------------------------------------------------- */

struct seat_binding {                   /* a bound wl_seat */
    struct lm_list link;
    struct lm_seat *seat;
    struct lm_resource *res;
};

struct seat_pointer {
    struct lm_list link;
    struct lm_seat *seat;
    struct lm_resource *res;
};

struct seat_keyboard {
    struct lm_list link;
    struct lm_seat *seat;
    struct lm_resource *res;
};

struct seat_touch {
    struct lm_list link;
    struct lm_seat *seat;
    struct lm_resource *res;
};

struct data_source {
    struct lm_list link;                /* in seat->sources */
    struct lm_seat *seat;
    struct lm_resource *res;            /* NULL for the compositor's own source (clipboard history entry) */
    char *text;                         /* own source: the text it serves */
    uint8_t *data;                      /* own source: binary data it serves instead (a screenshot) */
    size_t data_len;
    char *mimes[MAX_MIMES];
    int n_mimes;
    struct lm_list offers;              /* struct data_offer.src_link */
};

struct data_offer {
    struct lm_list link;                /* in seat->offers */
    struct lm_list src_link;            /* in source->offers (detached when source gone) */
    struct lm_seat *seat;
    struct data_source *source;         /* NULL once the source is destroyed */
    struct lm_resource *res;
};

struct data_device {
    struct lm_list link;
    struct lm_seat *seat;
    struct lm_resource *res;
};

struct seat_client {                    /* per client: purge serials when it leaves */
    struct lm_list link;
    struct lm_seat *seat;
    struct lm_client *client;
    struct lm_listener destroy;
};

struct serial_rec {
    uint32_t serial;
    struct lm_client *client;
};

struct touch_point {
    bool active;
    bool via_pointer;                   /* decoration touch: emulated through the pointer path */
    int32_t id;
    struct lm_surface *surface;
    double ox, oy;                      /* global origin of the surface */
    struct lm_listener destroy;
    struct lm_seat *seat;
};

struct lm_seat {
    struct lm_server *srv;
    struct lm_global *seat_global;
    struct lm_global *ddm_global;
    bool dying;
    bool locked;                        /* screen lock active: no input reaches clients (lm_seat_set_locked) */

    struct lm_list bindings;            /* struct seat_binding */
    struct lm_list pointers;            /* struct seat_pointer */
    struct lm_list keyboards;           /* struct seat_keyboard */
    struct lm_list touches;             /* struct seat_touch */
    struct lm_list devices;             /* struct data_device */
    struct lm_list sources;             /* struct data_source */
    struct lm_list offers;              /* struct data_offer */
    struct lm_list clients;             /* struct seat_client */

    /* pointer */
    double px, py;
    bool pos_valid;
    struct lm_surface *pfocus;
    struct lm_listener pfocus_destroy;
    double pox, poy;                    /* global origin of the focus surface */
    uint32_t enter_serial;
    struct lm_pointer_grab *grab;
    uint32_t fwd_buttons[MAX_BUTTONS];  /* presses forwarded to a client, not yet released */
    int n_fwd;
    struct lm_idle *refocus_idle;

    /* cursor */
    enum lm_cursor_shape override;
    struct lm_surface *cursor_surface;
    struct lm_listener cursor_destroy;
    struct lm_client *cursor_client;
    int32_t cursor_hot_x, cursor_hot_y;
    bool cursor_hidden;

    /* keyboard */
    struct lm_surface *kfocus;
    struct lm_listener kfocus_destroy;
    uint32_t keys_down[MAX_KEYS];       /* evdev codes of forwarded, still held keys */
    int n_keys;
    uint32_t swallowed[MAX_KEYS];       /* codes whose press the wm consumed */
    int n_swallowed;
    uint32_t mods_dep, mods_lat, mods_lock, group;
    int keymap_fd;
    uint32_t keymap_size;

    /* serials */
    struct serial_rec serials[MAX_SERIALS];
    int serial_next;

    /* touch */
    bool has_touch;
    struct touch_point points[MAX_TOUCH];

    /* clipboard */
    struct data_source *selection;
    struct data_source *own;            /* the selection when the compositor itself owns it (history entry picked in the gadget) */

    /* clipboard history for the gadget: the compositor itself reads the text of every new selection */
    char *clip_hist[LM_CLIP_HISTORY];   /* newest first */
    int clip_n;
    struct {
        int fd;                         /* read end of the pipe handed to the source, -1 = idle */
        struct lm_source *src;
        struct lm_timer *timer;         /* gives up on a source that never answers */
        char *buf;
        size_t len;
    } cap;
};

/* ---- small helpers -------------------------------------------------------------------- */

static uint32_t next_serial(struct lm_seat *seat)
{
    return lm_server_next_serial(seat->srv);
}

static void cursor_changed(struct lm_seat *seat)
{
    if (seat->srv->render)
        lm_render_cursor_changed(seat->srv->render);
}

static struct lm_client *surface_client(const struct lm_surface *s)
{
    return s && s->resource ? s->resource->client : NULL;
}

static bool code_find(const uint32_t *arr, int n, uint32_t code)
{
    for (int i = 0; i < n; i++)
        if (arr[i] == code)
            return true;
    return false;
}

static bool code_add(uint32_t *arr, int *n, int max, uint32_t code)
{
    if (code_find(arr, *n, code))
        return true;
    if (*n >= max)
        return false;
    arr[(*n)++] = code;
    return true;
}

static bool code_remove(uint32_t *arr, int *n, uint32_t code)
{
    for (int i = 0; i < *n; i++) {
        if (arr[i] == code) {
            memmove(&arr[i], &arr[i + 1], sizeof(arr[0]) * (size_t)(*n - i - 1));
            (*n)--;
            return true;
        }
    }
    return false;
}

/* Destroy every resource of a per-seat list (used when the seat goes away first). */
#define DESTROY_ALL(type, head) \
    do { \
        type *x_, *t_; \
        lm_list_for_each_safe(x_, t_, head, link) \
            lm_resource_destroy(x_->res); \
    } while (0)

/* Every pointer/keyboard/... object belonging to `cl`. */
#define FOR_CLIENT_OBJECTS(pos, head, cl) \
    lm_list_for_each(pos, head, link) if ((pos)->res->client == (cl))

static void ensure_pointer_pos(struct lm_seat *seat)
{
    if (seat->pos_valid)
        return;
    struct lm_rect b;
    if (lm_output_layout_bounds(seat->srv, &b)) {
        seat->px = b.x + b.w / 2.0;
        seat->py = b.y + b.h / 2.0;
        seat->pos_valid = true;
    }
}

/* ---- serials ---------------------------------------------------------------------------- */

static void seat_client_gone(struct lm_listener *l, void *data)
{
    (void)data;
    struct seat_client *sc = lm_container_of(l, struct seat_client, destroy);
    struct lm_seat *seat = sc->seat;
    lm_listener_remove(l);
    for (int i = 0; i < MAX_SERIALS; i++)
        if (seat->serials[i].client == sc->client)
            seat->serials[i].client = NULL;
    if (seat->cursor_client == sc->client)
        seat->cursor_client = NULL;
    lm_list_remove(&sc->link);
    free(sc);
}

static void ensure_seat_client(struct lm_seat *seat, struct lm_client *client)
{
    struct seat_client *sc;
    lm_list_for_each(sc, &seat->clients, link)
        if (sc->client == client)
            return;
    sc = calloc(1, sizeof(*sc));
    if (!sc)
        return;
    sc->seat = seat;
    sc->client = client;
    sc->destroy.notify = seat_client_gone;
    lm_signal_add(&client->destroy_signal, &sc->destroy);
    lm_list_append(&seat->clients, &sc->link);
}

static void serial_record(struct lm_seat *seat, struct lm_client *client, uint32_t serial)
{
    if (!client)
        return;
    ensure_seat_client(seat, client);
    seat->serials[seat->serial_next].serial = serial;
    seat->serials[seat->serial_next].client = client;
    seat->serial_next = (seat->serial_next + 1) % MAX_SERIALS;
}

bool lm_seat_serial_valid(const struct lm_seat *seat, struct lm_client *client, uint32_t serial)
{
    if (!seat || !client || serial == 0)
        return false;
    for (int i = 0; i < MAX_SERIALS; i++)
        if (seat->serials[i].serial == serial && seat->serials[i].client == client)
            return true;
    return false;
}

/* ---- cursor ----------------------------------------------------------------------------------- */

static void cursor_role_commit(struct lm_surface *s)
{
    struct lm_seat *seat = s->role_data;
    if (seat)
        cursor_changed(seat);
}

static void cursor_role_damage(struct lm_surface *s, const struct lm_rect *rect)
{
    (void)rect;
    struct lm_seat *seat = s->role_data;
    if (seat)
        cursor_changed(seat);
}

static const struct lm_surface_role cursor_role = {
    .name = "cursor",
    .commit = cursor_role_commit,
    .damage = cursor_role_damage,
};

static void cursor_surface_destroyed(struct lm_listener *l, void *data)
{
    (void)data;
    struct lm_seat *seat = lm_container_of(l, struct lm_seat, cursor_destroy);
    seat->cursor_surface = NULL;
    seat->cursor_client = NULL;
    lm_listener_remove(l);
    cursor_changed(seat);
}

/* Forget the client-provided cursor surface (and release its role). */
static void cursor_drop(struct lm_seat *seat)
{
    struct lm_surface *s = seat->cursor_surface;
    seat->cursor_surface = NULL;
    seat->cursor_client = NULL;
    seat->cursor_hidden = false;
    lm_listener_remove(&seat->cursor_destroy);
    if (s && s->role == &cursor_role && s->role_data)
        lm_surface_clear_role(s);
}

void lm_seat_get_cursor(const struct lm_seat *seat, struct lm_cursor *out)
{
    memset(out, 0, sizeof(*out));
    out->shape = LM_CURSOR_DEFAULT;
    if (!seat)
        return;
    if (seat->locked) {
        out->shape = LM_CURSOR_HIDDEN;
        return;
    }
    if (seat->override != LM_CURSOR_NONE) {
        out->shape = seat->override;
        return;
    }
    if (seat->pfocus && seat->cursor_client == surface_client(seat->pfocus)) {
        if (seat->cursor_surface && seat->cursor_surface->has_content) {
            out->shape = LM_CURSOR_SURFACE;
            out->surface = seat->cursor_surface;
            out->hot_x = seat->cursor_hot_x;
            out->hot_y = seat->cursor_hot_y;
        } else if (seat->cursor_hidden || seat->cursor_surface) {
            out->shape = LM_CURSOR_HIDDEN;
        }
    }
}

void lm_seat_set_cursor_override(struct lm_seat *seat, enum lm_cursor_shape shape)
{
    if (!seat || seat->override == shape)
        return;
    seat->override = shape;
    cursor_changed(seat);
}

void lm_seat_send_frame_done(struct lm_seat *seat, uint32_t time_ms)
{
    if (seat && seat->cursor_surface)
        lm_surface_send_frame_done(seat->cursor_surface, time_ms);
}

/* ---- wl_pointer events ---------------------------------------------------------------------------- */

static void pointer_frame(struct lm_seat *seat, struct lm_client *cl)
{
    struct seat_pointer *p;
    FOR_CLIENT_OBJECTS(p, &seat->pointers, cl)
        lm_resource_post(p->res, LM_WL_POINTER_EVT_FRAME);
}

static void pfocus_surface_destroyed(struct lm_listener *l, void *data)
{
    (void)data;
    struct lm_seat *seat = lm_container_of(l, struct lm_seat, pfocus_destroy);
    lm_listener_remove(l);
    seat->pfocus = NULL;
    seat->n_fwd = 0;
    cursor_changed(seat);
    if (seat->refocus_idle && !seat->dying)
        lm_idle_schedule(seat->refocus_idle);
}

void lm_seat_pointer_set_focus(struct lm_seat *seat, struct lm_surface *s, double sx, double sy)
{
    if (!seat)
        return;
    if (s && (!s->resource || seat->locked))
        s = NULL;
    if (s == seat->pfocus) {
        if (s) {
            seat->pox = seat->px - sx;
            seat->poy = seat->py - sy;
        }
        return;
    }

    if (seat->pfocus) {
        struct lm_surface *old = seat->pfocus;
        struct lm_client *cl = surface_client(old);
        uint32_t serial = next_serial(seat);
        struct seat_pointer *p;
        FOR_CLIENT_OBJECTS(p, &seat->pointers, cl)
            lm_resource_post(p->res, LM_WL_POINTER_EVT_LEAVE, serial, old->resource);
        pointer_frame(seat, cl);
        lm_listener_remove(&seat->pfocus_destroy);
        seat->pfocus = NULL;
    }

    if (s) {
        seat->pfocus = s;
        seat->pfocus_destroy.notify = pfocus_surface_destroyed;
        lm_signal_add(&s->destroy_signal, &seat->pfocus_destroy);
        seat->pox = seat->px - sx;
        seat->poy = seat->py - sy;
        seat->enter_serial = next_serial(seat);
        struct lm_client *cl = surface_client(s);
        struct seat_pointer *p;
        FOR_CLIENT_OBJECTS(p, &seat->pointers, cl)
            lm_resource_post(p->res, LM_WL_POINTER_EVT_ENTER, seat->enter_serial, s->resource,
                             lm_fixed_from_double(sx), lm_fixed_from_double(sy));
        pointer_frame(seat, cl);
    }

    /* The cursor image belongs to the client that set it. */
    if (seat->cursor_client && seat->cursor_client != surface_client(s))
        cursor_drop(seat);
    cursor_changed(seat);
}

void lm_seat_pointer_send_motion(struct lm_seat *seat, uint32_t time_ms)
{
    if (!seat || !seat->pfocus)
        return;
    struct lm_client *cl = surface_client(seat->pfocus);
    lm_fixed_t fx = lm_fixed_from_double(seat->px - seat->pox);
    lm_fixed_t fy = lm_fixed_from_double(seat->py - seat->poy);
    struct seat_pointer *p;
    FOR_CLIENT_OBJECTS(p, &seat->pointers, cl)
        lm_resource_post(p->res, LM_WL_POINTER_EVT_MOTION, time_ms, fx, fy);
    pointer_frame(seat, cl);
}

void lm_seat_pointer_send_button(struct lm_seat *seat, uint32_t time_ms, uint32_t button,
                                 bool pressed)
{
    if (!seat || !seat->pfocus)
        return;
    struct lm_client *cl = surface_client(seat->pfocus);
    uint32_t serial = next_serial(seat);
    if (pressed)
        serial_record(seat, cl, serial);
    uint32_t state = pressed ? LM_POINTER_BUTTON_PRESSED : LM_POINTER_BUTTON_RELEASED;
    struct seat_pointer *p;
    FOR_CLIENT_OBJECTS(p, &seat->pointers, cl)
        lm_resource_post(p->res, LM_WL_POINTER_EVT_BUTTON, serial, time_ms, button, state);
    pointer_frame(seat, cl);
}

void lm_seat_pointer_send_axis(struct lm_seat *seat, uint32_t time_ms, int orientation,
                               double value, int32_t discrete)
{
    if (!seat || !seat->pfocus)
        return;
    if (orientation != LM_POINTER_AXIS_VERTICAL && orientation != LM_POINTER_AXIS_HORIZONTAL)
        return;
    struct lm_client *cl = surface_client(seat->pfocus);
    uint32_t axis = (uint32_t)orientation;
    lm_fixed_t fv = lm_fixed_from_double(value);
    struct seat_pointer *p;
    FOR_CLIENT_OBJECTS(p, &seat->pointers, cl) {
        if (discrete != 0) {
            lm_resource_post(p->res, LM_WL_POINTER_EVT_AXIS_SOURCE,
                             (uint32_t)LM_POINTER_AXIS_SOURCE_WHEEL);
            lm_resource_post(p->res, LM_WL_POINTER_EVT_AXIS_DISCRETE, axis, discrete);
            lm_resource_post(p->res, LM_WL_POINTER_EVT_AXIS, time_ms, axis, fv);
        } else if (value == 0.0) {
            lm_resource_post(p->res, LM_WL_POINTER_EVT_AXIS_SOURCE,
                             (uint32_t)LM_POINTER_AXIS_SOURCE_FINGER);
            lm_resource_post(p->res, LM_WL_POINTER_EVT_AXIS_STOP, time_ms, axis);
        } else {
            lm_resource_post(p->res, LM_WL_POINTER_EVT_AXIS_SOURCE,
                             (uint32_t)LM_POINTER_AXIS_SOURCE_FINGER);
            lm_resource_post(p->res, LM_WL_POINTER_EVT_AXIS, time_ms, axis, fv);
        }
    }
    pointer_frame(seat, cl);
}

void lm_seat_pointer_position(const struct lm_seat *seat, double *x, double *y)
{
    double px = 0, py = 0;
    if (seat) {
        px = seat->px;
        py = seat->py;
        if (!seat->pos_valid) {
            struct lm_rect b;
            if (lm_output_layout_bounds(seat->srv, &b)) {
                px = b.x + b.w / 2.0;
                py = b.y + b.h / 2.0;
            }
        }
    }
    if (x)
        *x = px;
    if (y)
        *y = py;
}

struct lm_surface *lm_seat_pointer_focus(const struct lm_seat *seat)
{
    return seat ? seat->pfocus : NULL;
}

/* ---- pointer routing ---------------------------------------------------------------------------- */

static void pick_at(struct lm_seat *seat, struct lm_pick *pick)
{
    memset(pick, 0, sizeof(*pick));
    if (seat->srv->wm)
        lm_wm_pick(seat->srv->wm, seat->px, seat->py, pick);
}

static bool pick_is_surface(const struct lm_pick *pick)
{
    return pick->kind == LM_PICK_SURFACE && pick->surface && pick->surface->resource;
}

/* Set the pointer focus from a pick. Returns true if the focus changed. */
static bool focus_from_pick(struct lm_seat *seat, const struct lm_pick *pick)
{
    struct lm_surface *old = seat->pfocus;
    if (pick_is_surface(pick))
        lm_seat_pointer_set_focus(seat, pick->surface, pick->sx, pick->sy);
    else
        lm_seat_pointer_set_focus(seat, NULL, 0, 0);
    return old != seat->pfocus;
}

static void pointer_refocus(struct lm_seat *seat, uint32_t time_ms)
{
    if (seat->grab || seat->srv->wm == NULL)
        return;
    struct lm_pick pick;
    pick_at(seat, &pick);
    if (focus_from_pick(seat, &pick) && seat->pfocus)
        lm_seat_pointer_send_motion(seat, time_ms);
}

static void refocus_idle_cb(void *data)
{
    struct lm_seat *seat = data;
    if (!seat->dying && seat->n_fwd == 0)
        pointer_refocus(seat, lm_now_msec32());
}

/* The pointer position changed: route the motion. */
static void pointer_moved(struct lm_seat *seat, uint32_t time_ms)
{
    cursor_changed(seat);
    if (seat->grab) {
        struct lm_pointer_grab *g = seat->grab;
        if (g->ops && g->ops->motion)
            g->ops->motion(g, time_ms, seat->px, seat->py);
        return;
    }
    struct lm_pick pick;
    pick_at(seat, &pick);
    if (seat->srv->wm)
        lm_wm_pointer_motion(seat->srv->wm, &pick);
    if (seat->grab)             /* the wm may have started one */
        return;

    if (seat->n_fwd > 0) {
        /* Implicit grab: while a forwarded button is held the focus sticks. */
        if (seat->pfocus)
            lm_seat_pointer_send_motion(seat, time_ms);
        return;
    }
    if (pick_is_surface(&pick) && pick.surface == seat->pfocus) {
        seat->pox = seat->px - pick.sx;
        seat->poy = seat->py - pick.sy;
        lm_seat_pointer_send_motion(seat, time_ms);
    } else {
        focus_from_pick(seat, &pick);
        if (seat->pfocus)
            lm_seat_pointer_send_motion(seat, time_ms);
    }
}

static void pointer_set_position(struct lm_seat *seat, double x, double y, uint32_t time_ms)
{
    lm_output_clamp_point(seat->srv, &x, &y);
    seat->px = x;
    seat->py = y;
    seat->pos_valid = true;
    pointer_moved(seat, time_ms);
}

void lm_seat_pointer_warp(struct lm_seat *seat, double x, double y)
{
    if (!seat)
        return;
    pointer_set_position(seat, x, y, lm_now_msec32());
}

static void pointer_button(struct lm_seat *seat, uint32_t time_ms, uint32_t button, bool pressed)
{
    if (seat->grab) {
        struct lm_pointer_grab *g = seat->grab;
        if (g->ops && g->ops->button)
            g->ops->button(g, time_ms, button, pressed);
        return;
    }
    struct lm_wm *wm = seat->srv->wm;
    struct lm_pick pick;
    if (pressed) {
        if (seat->n_fwd == 0)
            pointer_refocus(seat, time_ms);
        pick_at(seat, &pick);
        bool consumed = wm ? lm_wm_pointer_button(wm, &pick, time_ms, button, true) : false;
        if (seat->grab || consumed)
            return;
        if (seat->pfocus && code_add(seat->fwd_buttons, &seat->n_fwd, MAX_BUTTONS, button))
            lm_seat_pointer_send_button(seat, time_ms, button, true);
    } else {
        pick_at(seat, &pick);
        bool was_fwd = code_remove(seat->fwd_buttons, &seat->n_fwd, button);
        if (wm)
            lm_wm_pointer_button(wm, &pick, time_ms, button, false);
        if (seat->grab)
            return;
        if (was_fwd) {
            if (seat->pfocus)
                lm_seat_pointer_send_button(seat, time_ms, button, false);
            if (seat->n_fwd == 0)
                pointer_refocus(seat, time_ms);
        }
    }
}

static void pointer_axis(struct lm_seat *seat, uint32_t time_ms, int orientation, double value,
                         int32_t discrete)
{
    if (seat->grab) {
        struct lm_pointer_grab *g = seat->grab;
        if (g->ops && g->ops->axis)
            g->ops->axis(g, time_ms, orientation, value, discrete);
        return;
    }
    if (seat->n_fwd == 0)
        pointer_refocus(seat, time_ms);
    lm_seat_pointer_send_axis(seat, time_ms, orientation, value, discrete);
}

/* ---- pointer grabs ------------------------------------------------------------------------------- */

int lm_seat_pointer_grab(struct lm_seat *seat, struct lm_pointer_grab *grab)
{
    if (!seat || !grab)
        return -1;
    struct lm_pointer_grab *old = seat->grab;
    if (old == grab)
        return 0;
    seat->grab = NULL;
    if (old && old->ops && old->ops->cancel)
        old->ops->cancel(old);
    seat->grab = grab;
    seat->n_fwd = 0;                    /* the clients lose their implicit grab */
    lm_seat_pointer_set_focus(seat, NULL, 0, 0);
    return 0;
}

void lm_seat_pointer_ungrab(struct lm_seat *seat, struct lm_pointer_grab *grab)
{
    if (!seat || !grab || seat->grab != grab)
        return;
    seat->grab = NULL;
    if (!seat->dying)
        pointer_refocus(seat, lm_now_msec32());
}

bool lm_seat_pointer_has_grab(const struct lm_seat *seat)
{
    return seat && seat->grab != NULL;
}

/* ---- keyboard ---------------------------------------------------------------------------------------- */

static void kfocus_surface_destroyed(struct lm_listener *l, void *data)
{
    (void)data;
    struct lm_seat *seat = lm_container_of(l, struct lm_seat, kfocus_destroy);
    lm_listener_remove(l);
    seat->kfocus = NULL;
}

static void keyboard_send_enter(struct lm_seat *seat, struct seat_keyboard *k, uint32_t serial)
{
    struct lm_array keys = { (uint32_t)seat->n_keys * (uint32_t)sizeof(uint32_t), seat->keys_down };
    lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_ENTER, serial, seat->kfocus->resource, &keys);
    lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_MODIFIERS, serial, seat->mods_dep, seat->mods_lat,
                     seat->mods_lock, seat->group);
}

struct lm_surface *lm_seat_keyboard_focus(const struct lm_seat *seat)
{
    return seat ? seat->kfocus : NULL;
}

static void selection_update_focus(struct lm_seat *seat);

void lm_seat_set_keyboard_focus(struct lm_seat *seat, struct lm_surface *s)
{
    if (!seat)
        return;
    if (s && (!s->resource || seat->locked))
        s = NULL;
    if (s == seat->kfocus)
        return;

    if (seat->kfocus) {
        struct lm_surface *old = seat->kfocus;
        struct lm_client *cl = surface_client(old);
        uint32_t serial = next_serial(seat);
        struct seat_keyboard *k;
        FOR_CLIENT_OBJECTS(k, &seat->keyboards, cl)
            lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_LEAVE, serial, old->resource);
        lm_listener_remove(&seat->kfocus_destroy);
        seat->kfocus = NULL;
    }
    if (s) {
        seat->kfocus = s;
        seat->kfocus_destroy.notify = kfocus_surface_destroyed;
        lm_signal_add(&s->destroy_signal, &seat->kfocus_destroy);
        uint32_t serial = next_serial(seat);
        struct lm_client *cl = surface_client(s);
        struct seat_keyboard *k;
        FOR_CLIENT_OBJECTS(k, &seat->keyboards, cl)
            keyboard_send_enter(seat, k, serial);
    }
    selection_update_focus(seat);
}

static void keyboard_send_modifiers(struct lm_seat *seat)
{
    if (!seat->kfocus)
        return;
    struct lm_client *cl = surface_client(seat->kfocus);
    uint32_t serial = next_serial(seat);
    struct seat_keyboard *k;
    FOR_CLIENT_OBJECTS(k, &seat->keyboards, cl)
        lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_MODIFIERS, serial, seat->mods_dep, seat->mods_lat,
                         seat->mods_lock, seat->group);
}

/* New modifier / layout state from the input module: remember it, tell the focused client and the panel. */
static void keyboard_set_state(struct lm_seat *seat, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group)
{
    if (dep == seat->mods_dep && lat == seat->mods_lat && lock == seat->mods_lock && group == seat->group)
        return;
    seat->mods_dep = dep;
    seat->mods_lat = lat;
    seat->mods_lock = lock;
    seat->group = group;
    keyboard_send_modifiers(seat);
    if (seat->srv->wm)
        lm_wm_keyboard_state_changed(seat->srv->wm);
}

static void handle_key(struct lm_seat *seat, const struct lm_input_event *ev)
{
    uint32_t code = ev->u.key.code;
    bool pressed = ev->u.key.pressed;
    uint32_t dep = ev->u.key.mods_depressed;
    uint32_t lat = ev->u.key.mods_latched;
    uint32_t lock = ev->u.key.mods_locked;
    uint32_t time_ms = ev->time_ms;

    /* Shortcuts look at depressed, latched and sticky-locked modifiers (a locked NumLock/Caps must not break them). */
    uint32_t effective = dep | lat | (lock & ~(uint32_t)(LM_MOD_CAPS | LM_MOD_NUM));
    bool consumed = seat->srv->wm
        ? lm_wm_key(seat->srv->wm, code, pressed, ev->u.key.repeat, effective)
        : false;

    if (ev->u.key.repeat) {
        /* Clients repeat on their own; repeats exist for compositor shortcuts only. */
        return;
    }

    if (pressed) {
        if (consumed) {
            code_add(seat->swallowed, &seat->n_swallowed, MAX_KEYS, code);
        } else {
            code_add(seat->keys_down, &seat->n_keys, MAX_KEYS, code);
            if (seat->kfocus) {
                struct lm_client *cl = surface_client(seat->kfocus);
                uint32_t serial = next_serial(seat);
                serial_record(seat, cl, serial);
                struct seat_keyboard *k;
                FOR_CLIENT_OBJECTS(k, &seat->keyboards, cl)
                    lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_KEY, serial, time_ms, code,
                                     (uint32_t)LM_KEYBOARD_KEY_PRESSED);
            }
        }
    } else {
        if (code_remove(seat->swallowed, &seat->n_swallowed, code)) {
            /* the matching press never reached a client */
        } else if (code_remove(seat->keys_down, &seat->n_keys, code) && seat->kfocus) {
            struct lm_client *cl = surface_client(seat->kfocus);
            uint32_t serial = next_serial(seat);
            struct seat_keyboard *k;
            FOR_CLIENT_OBJECTS(k, &seat->keyboards, cl)
                lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_KEY, serial, time_ms, code,
                                 (uint32_t)LM_KEYBOARD_KEY_RELEASED);
        }
    }

    keyboard_set_state(seat, dep, lat, lock, ev->u.key.group);
}

/* wl_keyboard keymap file: a sealed memfd holding the keymap text. */
static int keymap_make_fd(const int *layouts, int nlayouts, uint32_t *size_out)
{
    static const int us_only[1] = { 0 };
    char *built = nlayouts > 0 ? lm_keymap_build(layouts, nlayouts) : lm_keymap_build(us_only, 1);
    if (!built)
        return -1;
    const char *text = built;
    size_t len = strlen(text) + 1;
    int fd = memfd_create("mellagui-xkb-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    bool sealable = fd >= 0;
    if (fd < 0) {
        const char *dir = getenv("XDG_RUNTIME_DIR");
        if (!dir || !*dir)
            dir = "/tmp";
        char path[512];
        if (snprintf(path, sizeof(path), "%s/mellagui-keymap-XXXXXX", dir) >= (int)sizeof(path))
            { free(built); return -1; }
        fd = mkostemp(path, O_CLOEXEC);
        if (fd < 0) {
            free(built);
            return -1;
        }
        unlink(path);
    }
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            free(built);
            return -1;
        }
        off += (size_t)n;
    }
    if (sealable)
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    *size_out = (uint32_t)len;
    free(built);
    return fd;
}

/* ---- wl_pointer / wl_keyboard / wl_touch objects ------------------------------------------------------------ */

static void pointer_set_cursor(struct lm_resource *res, const union lm_arg *args)
{
    struct seat_pointer *p = res->data;
    struct lm_seat *seat = p->seat;
    uint32_t serial = args[0].u;
    struct lm_resource *sres = args[1].r;
    int32_t hx = args[2].i, hy = args[3].i;

    struct lm_surface *s = sres ? lm_surface_from_resource(sres) : NULL;
    if (sres && !s)
        return;
    /* Only the client with pointer focus, answering the latest enter, may set the cursor. */
    if (!seat->pfocus || surface_client(seat->pfocus) != res->client)
        return;
    if (serial != seat->enter_serial)
        return;

    if (!s) {
        cursor_drop(seat);
        seat->cursor_hidden = true;
        seat->cursor_client = res->client;
        cursor_changed(seat);
        return;
    }
    if (s == seat->cursor_surface) {
        seat->cursor_hot_x = hx;
        seat->cursor_hot_y = hy;
        cursor_changed(seat);
        return;
    }
    if (s->role && s->role != &cursor_role) {
        lm_resource_post_error(res, LM_WL_POINTER_ERR_ROLE,
                               "wl_surface@%u already has a different role", sres->id);
        return;
    }
    cursor_drop(seat);
    if (lm_surface_set_role(s, &cursor_role, seat) < 0) {
        lm_resource_post_error(res, LM_WL_POINTER_ERR_ROLE,
                               "wl_surface@%u cannot become a cursor surface", sres->id);
        return;
    }
    seat->cursor_surface = s;
    seat->cursor_client = res->client;
    seat->cursor_hidden = false;
    seat->cursor_hot_x = hx;
    seat->cursor_hot_y = hy;
    seat->cursor_destroy.notify = cursor_surface_destroyed;
    lm_signal_add(&s->destroy_signal, &seat->cursor_destroy);
    cursor_changed(seat);
}

static void generic_release(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static const lm_request_fn pointer_handlers[] = {
    [LM_WL_POINTER_REQ_SET_CURSOR] = pointer_set_cursor,
    [LM_WL_POINTER_REQ_RELEASE] = generic_release,
};

static const lm_request_fn keyboard_handlers[] = {
    [LM_WL_KEYBOARD_REQ_RELEASE] = generic_release,
};

static const lm_request_fn touch_handlers[] = {
    [LM_WL_TOUCH_REQ_RELEASE] = generic_release,
};

static void pointer_res_destroy(struct lm_resource *res)
{
    struct seat_pointer *p = res->data;
    lm_list_remove(&p->link);
    free(p);
}

static void keyboard_res_destroy(struct lm_resource *res)
{
    struct seat_keyboard *k = res->data;
    lm_list_remove(&k->link);
    free(k);
}

static void touch_res_destroy(struct lm_resource *res)
{
    struct seat_touch *t = res->data;
    lm_list_remove(&t->link);
    free(t);
}

static void seat_get_pointer(struct lm_resource *res, const union lm_arg *args)
{
    struct seat_binding *b = res->data;
    struct lm_seat *seat = b->seat;
    struct seat_pointer *p = calloc(1, sizeof(*p));
    if (!p) {
        lm_resource_post_no_memory(res);
        return;
    }
    p->seat = seat;
    p->res = lm_resource_create(res->client, args[0].n, &lm_wl_pointer_interface, res->version,
                                pointer_handlers, p, pointer_res_destroy);
    if (!p->res) {
        free(p);
        return;
    }
    lm_list_append(&seat->pointers, &p->link);
    if (seat->pfocus && surface_client(seat->pfocus) == res->client) {
        lm_resource_post(p->res, LM_WL_POINTER_EVT_ENTER, seat->enter_serial,
                         seat->pfocus->resource,
                         lm_fixed_from_double(seat->px - seat->pox),
                         lm_fixed_from_double(seat->py - seat->poy));
        lm_resource_post(p->res, LM_WL_POINTER_EVT_FRAME);
    }
}

/* The keymap file could not be created: tell the client there is none. */
static void keyboard_send_no_keymap(struct seat_keyboard *k)
{
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_KEYMAP, (uint32_t)LM_KEYBOARD_KEYMAP_NONE, fd, 0u);
    close(fd);
}

static void seat_get_keyboard(struct lm_resource *res, const union lm_arg *args)
{
    struct seat_binding *b = res->data;
    struct lm_seat *seat = b->seat;
    struct seat_keyboard *k = calloc(1, sizeof(*k));
    if (!k) {
        lm_resource_post_no_memory(res);
        return;
    }
    k->seat = seat;
    k->res = lm_resource_create(res->client, args[0].n, &lm_wl_keyboard_interface, res->version,
                                keyboard_handlers, k, keyboard_res_destroy);
    if (!k->res) {
        free(k);
        return;
    }
    lm_list_append(&seat->keyboards, &k->link);

    if (seat->keymap_fd >= 0)
        lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_KEYMAP,
                         (uint32_t)LM_KEYBOARD_KEYMAP_XKB_V1, seat->keymap_fd,
                         seat->keymap_size);
    else
        keyboard_send_no_keymap(k);
    int32_t rate = seat->srv->config.repeat_rate;
    int32_t delay = seat->srv->config.repeat_delay_ms;
    lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_REPEAT_INFO, rate < 0 ? 0 : rate,
                     delay < 0 ? 0 : delay);
    if (seat->kfocus && surface_client(seat->kfocus) == res->client)
        keyboard_send_enter(seat, k, next_serial(seat));
}

/* Settings: a different set of layouts. Every keyboard object gets the new keymap (the group number follows with the
 * LM_INPUT_MODS event the input module emits right after). */
void lm_seat_set_layouts(struct lm_seat *seat, const int *layouts, int n)
{
    uint32_t size = 0;
    int fd = keymap_make_fd(layouts, n, &size);
    if (fd < 0) {
        lm_warn("could not create the new keymap file: %s; clients keep the old one", strerror(errno));
        return;
    }
    if (seat->keymap_fd >= 0)
        close(seat->keymap_fd);
    seat->keymap_fd = fd;
    seat->keymap_size = size;
    struct seat_keyboard *k;
    lm_list_for_each(k, &seat->keyboards, link)
        lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_KEYMAP, (uint32_t)LM_KEYBOARD_KEYMAP_XKB_V1, fd, size);
    /* A new keymap resets the clients' xkb state: whatever modifiers/group event follows must be sent even if it
     * equals what the seat last sent (keyboard_set_state would drop it). */
    seat->group = UINT32_MAX;
}

/* Settings: key repeat rate / delay changed (wl_keyboard.repeat_info exists since version 4). */
void lm_seat_send_repeat_info(struct lm_seat *seat)
{
    int32_t rate = seat->srv->config.repeat_rate;
    int32_t delay = seat->srv->config.repeat_delay_ms;
    struct seat_keyboard *k;
    lm_list_for_each(k, &seat->keyboards, link)
        if (k->res->version >= 4)
            lm_resource_post(k->res, LM_WL_KEYBOARD_EVT_REPEAT_INFO, rate < 0 ? 0 : rate, delay < 0 ? 0 : delay);
}

static void seat_get_touch(struct lm_resource *res, const union lm_arg *args)
{
    struct seat_binding *b = res->data;
    struct lm_seat *seat = b->seat;
    struct seat_touch *t = calloc(1, sizeof(*t));
    if (!t) {
        lm_resource_post_no_memory(res);
        return;
    }
    t->seat = seat;
    t->res = lm_resource_create(res->client, args[0].n, &lm_wl_touch_interface, res->version,
                                touch_handlers, t, touch_res_destroy);
    if (!t->res) {
        free(t);
        return;
    }
    lm_list_append(&seat->touches, &t->link);
}

static const lm_request_fn seat_handlers[] = {
    [LM_WL_SEAT_REQ_GET_POINTER] = seat_get_pointer,
    [LM_WL_SEAT_REQ_GET_KEYBOARD] = seat_get_keyboard,
    [LM_WL_SEAT_REQ_GET_TOUCH] = seat_get_touch,
    [LM_WL_SEAT_REQ_RELEASE] = generic_release,
};

static uint32_t seat_capabilities(const struct lm_seat *seat)
{
    uint32_t caps = LM_SEAT_CAP_POINTER | LM_SEAT_CAP_KEYBOARD;
    if (seat->has_touch)
        caps |= LM_SEAT_CAP_TOUCH;
    return caps;
}

static void seat_binding_destroy(struct lm_resource *res)
{
    struct seat_binding *b = res->data;
    lm_list_remove(&b->link);
    free(b);
}

static void seat_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    struct lm_seat *seat = data;
    struct seat_binding *b = calloc(1, sizeof(*b));
    if (!b) {
        lm_client_post_no_memory(client);
        return;
    }
    b->seat = seat;
    b->res = lm_resource_create(client, id, &lm_wl_seat_interface, version, seat_handlers, b,
                                seat_binding_destroy);
    if (!b->res) {
        free(b);
        return;
    }
    lm_list_append(&seat->bindings, &b->link);
    lm_resource_post(b->res, LM_WL_SEAT_EVT_CAPABILITIES, seat_capabilities(seat));
    lm_resource_post(b->res, LM_WL_SEAT_EVT_NAME, SEAT_NAME);
}

static void seat_note_touch_device(struct lm_seat *seat)
{
    if (seat->has_touch)
        return;
    seat->has_touch = true;
    struct seat_binding *b;
    lm_list_for_each(b, &seat->bindings, link)
        lm_resource_post(b->res, LM_WL_SEAT_EVT_CAPABILITIES, seat_capabilities(seat));
}

/* ---- touch ------------------------------------------------------------------------------------------------------ */

static void touch_surface_destroyed(struct lm_listener *l, void *data)
{
    (void)data;
    struct touch_point *tp = lm_container_of(l, struct touch_point, destroy);
    lm_listener_remove(l);
    tp->active = false;
    tp->surface = NULL;
}

static struct touch_point *touch_find(struct lm_seat *seat, int32_t id)
{
    for (int i = 0; i < MAX_TOUCH; i++)
        if (seat->points[i].active && seat->points[i].id == id)
            return &seat->points[i];
    return NULL;
}

static void touch_release_point(struct touch_point *tp)
{
    if (tp->surface)
        lm_listener_remove(&tp->destroy);
    tp->active = false;
    tp->surface = NULL;
}

static bool touch_global_pos(struct lm_seat *seat, double nx, double ny, double *gx, double *gy)
{
    struct lm_rect b;
    if (!lm_output_layout_bounds(seat->srv, &b))
        return false;
    nx = isfinite(nx) ? LM_CLAMP(nx, 0.0, 1.0) : 0.0;
    ny = isfinite(ny) ? LM_CLAMP(ny, 0.0, 1.0) : 0.0;
    *gx = b.x + nx * b.w;
    *gy = b.y + ny * b.h;
    return true;
}

static void touch_down(struct lm_seat *seat, const struct lm_input_event *ev)
{
    int32_t id = ev->u.touch.id;
    double gx, gy;
    if (!touch_global_pos(seat, ev->u.touch.x, ev->u.touch.y, &gx, &gy))
        return;
    if (touch_find(seat, id))
        return;
    struct touch_point *tp = NULL;
    for (int i = 0; i < MAX_TOUCH; i++) {
        if (!seat->points[i].active) {
            tp = &seat->points[i];
            break;
        }
    }
    if (!tp)
        return;

    struct lm_wm *wm = seat->srv->wm;
    struct lm_pick pick;
    memset(&pick, 0, sizeof(pick));
    if (wm)
        lm_wm_pick(wm, gx, gy, &pick);

    memset(tp, 0, sizeof(*tp));
    lm_list_init(&tp->destroy.link);
    tp->seat = seat;
    tp->id = id;

    if (pick_is_surface(&pick)) {
        /* Let the wm focus/raise the touched window (touch acts like a left click). */
        if (wm)
            lm_wm_pointer_button(wm, &pick, ev->time_ms, BTN_LEFT, true);
        struct lm_surface *s = pick.surface;
        tp->active = true;
        tp->surface = s;
        tp->ox = gx - pick.sx;
        tp->oy = gy - pick.sy;
        tp->destroy.notify = touch_surface_destroyed;
        lm_signal_add(&s->destroy_signal, &tp->destroy);

        struct lm_client *cl = surface_client(s);
        uint32_t serial = next_serial(seat);
        serial_record(seat, cl, serial);
        struct seat_touch *t;
        FOR_CLIENT_OBJECTS(t, &seat->touches, cl) {
            lm_resource_post(t->res, LM_WL_TOUCH_EVT_DOWN, serial, ev->time_ms, s->resource, id,
                             lm_fixed_from_double(pick.sx), lm_fixed_from_double(pick.sy));
            lm_resource_post(t->res, LM_WL_TOUCH_EVT_FRAME);
        }
    } else {
        /* Decorations, panel: emulate a pointer press at the touch position. */
        tp->active = true;
        tp->via_pointer = true;
        pointer_set_position(seat, gx, gy, ev->time_ms);
        pointer_button(seat, ev->time_ms, BTN_LEFT, true);
    }
}

static void touch_motion(struct lm_seat *seat, const struct lm_input_event *ev)
{
    struct touch_point *tp = touch_find(seat, ev->u.touch.id);
    double gx, gy;
    if (!tp || !touch_global_pos(seat, ev->u.touch.x, ev->u.touch.y, &gx, &gy))
        return;
    if (tp->via_pointer) {
        pointer_set_position(seat, gx, gy, ev->time_ms);
        return;
    }
    if (!tp->surface)
        return;
    struct lm_client *cl = surface_client(tp->surface);
    lm_fixed_t fx = lm_fixed_from_double(gx - tp->ox);
    lm_fixed_t fy = lm_fixed_from_double(gy - tp->oy);
    struct seat_touch *t;
    FOR_CLIENT_OBJECTS(t, &seat->touches, cl) {
        lm_resource_post(t->res, LM_WL_TOUCH_EVT_MOTION, ev->time_ms, tp->id, fx, fy);
        lm_resource_post(t->res, LM_WL_TOUCH_EVT_FRAME);
    }
}

static void touch_up(struct lm_seat *seat, const struct lm_input_event *ev)
{
    struct touch_point *tp = touch_find(seat, ev->u.touch.id);
    if (!tp)
        return;
    if (tp->via_pointer) {
        tp->active = false;
        pointer_button(seat, ev->time_ms, BTN_LEFT, false);
        return;
    }
    if (tp->surface) {
        struct lm_client *cl = surface_client(tp->surface);
        uint32_t serial = next_serial(seat);
        struct seat_touch *t;
        FOR_CLIENT_OBJECTS(t, &seat->touches, cl) {
            lm_resource_post(t->res, LM_WL_TOUCH_EVT_UP, serial, ev->time_ms, tp->id);
            lm_resource_post(t->res, LM_WL_TOUCH_EVT_FRAME);
        }
    }
    touch_release_point(tp);
}

static void touch_cancel_all(struct lm_seat *seat, uint32_t time_ms)
{
    struct lm_client *done[MAX_TOUCH];
    int n_done = 0;
    for (int i = 0; i < MAX_TOUCH; i++) {
        struct touch_point *tp = &seat->points[i];
        if (!tp->active)
            continue;
        if (tp->via_pointer) {
            tp->active = false;
            pointer_button(seat, time_ms, BTN_LEFT, false);
            continue;
        }
        struct lm_client *cl = surface_client(tp->surface);
        bool seen = false;
        for (int j = 0; j < n_done; j++)
            seen |= done[j] == cl;
        if (cl && !seen) {
            done[n_done++] = cl;
            struct seat_touch *t;
            FOR_CLIENT_OBJECTS(t, &seat->touches, cl)
                lm_resource_post(t->res, LM_WL_TOUCH_EVT_CANCEL);
        }
        touch_release_point(tp);
    }
}

/* ---- wl_data_device_manager ------------------------------------------------------------------------------------- */

static struct data_source *source_of(struct lm_resource *res)
{
    return res ? res->data : NULL;
}

static void offer_res_destroy(struct lm_resource *res)
{
    struct data_offer *o = res->data;
    lm_list_remove(&o->link);
    lm_list_remove(&o->src_link);
    free(o);
}

static void offer_accept(struct lm_resource *res, const union lm_arg *args)
{
    struct data_offer *o = res->data;
    (void)args;
    (void)o; /* drag-and-drop is not performed: nothing to negotiate */
}

/* Our own big data (a screenshot, MBs) must not block the single-threaded loop on a slow reader: a short-lived child
 * (it owns a copy of the memory) writes it, reaped by the server's SIGCHLD watch. */
static void serve_data_async(int fd, const uint8_t *p, size_t n)
{
    if (fork() != 0)
        return;
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);          /* the loop blocks signals for signalfd */
    signal(SIGPIPE, SIG_DFL);
    setsid();
    alarm(15);
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    while (n > 0) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            break;
        p += k;
        n -= (size_t)k;
    }
    _exit(0);
}

static void offer_receive(struct lm_resource *res, const union lm_arg *args)
{
    struct data_offer *o = res->data;
    const char *mime = args[0].s;
    if (!o->source || !mime)
        return;
    for (int i = 0; i < o->source->n_mimes; i++) {
        if (strcmp(o->source->mimes[i], mime) == 0) {
            if (!o->source->res && o->source->data) {
                serve_data_async(args[1].h, o->source->data, o->source->data_len);
                return;
            }
            if (!o->source->res) {              /* our own text: write it (a few KiB fit into the pipe) */
                const char *p = o->source->text;
                size_t left = p ? strlen(p) : 0;
                while (left > 0) {
                    ssize_t k = write(args[1].h, p, left);
                    if (k <= 0)
                        break;
                    p += k;
                    left -= (size_t)k;
                }
                return;
            }
            /* the wire layer dup()s the fd; the server closes ours after this handler */
            lm_resource_post(o->source->res, LM_WL_DATA_SOURCE_EVT_SEND, mime, args[1].h);
            return;
        }
    }
}

static void offer_finish(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_post_error(res, LM_WL_DATA_OFFER_ERR_INVALID_FINISH,
                           "wl_data_offer.finish on a selection offer");
}

static void offer_set_actions(struct lm_resource *res, const union lm_arg *args)
{
    uint32_t actions = args[0].u, preferred = args[1].u;
    if (actions & ~7u) {
        lm_resource_post_error(res, LM_WL_DATA_OFFER_ERR_INVALID_ACTION_MASK,
                               "invalid action mask %u", actions);
        return;
    }
    if (preferred != 0 && (preferred & (preferred - 1)) != 0) {
        lm_resource_post_error(res, LM_WL_DATA_OFFER_ERR_INVALID_ACTION,
                               "invalid preferred action %u", preferred);
    }
}

static const lm_request_fn offer_handlers[] = {
    [LM_WL_DATA_OFFER_REQ_ACCEPT] = offer_accept,
    [LM_WL_DATA_OFFER_REQ_RECEIVE] = offer_receive,
    [LM_WL_DATA_OFFER_REQ_DESTROY] = generic_release,
    [LM_WL_DATA_OFFER_REQ_FINISH] = offer_finish,
    [LM_WL_DATA_OFFER_REQ_SET_ACTIONS] = offer_set_actions,
};

/* Introduce the current selection to one data device. */
static void device_send_selection(struct lm_seat *seat, struct data_device *dev)
{
    struct data_source *src = seat->selection;
    if (!src) {
        lm_resource_post(dev->res, LM_WL_DATA_DEVICE_EVT_SELECTION, (struct lm_resource *)NULL);
        return;
    }
    struct data_offer *o = calloc(1, sizeof(*o));
    if (!o) {
        lm_resource_post_no_memory(dev->res);
        return;
    }
    o->seat = seat;
    o->source = src;
    lm_list_init(&o->link);
    lm_list_init(&o->src_link);
    o->res = lm_resource_create_server(dev->res->client, &lm_wl_data_offer_interface,
                                       dev->res->version, offer_handlers, o, offer_res_destroy);
    if (!o->res) {
        free(o);
        return;
    }
    lm_list_append(&seat->offers, &o->link);
    lm_list_append(&src->offers, &o->src_link);
    lm_resource_post(dev->res, LM_WL_DATA_DEVICE_EVT_DATA_OFFER, o->res);
    for (int i = 0; i < src->n_mimes; i++)
        lm_resource_post(o->res, LM_WL_DATA_OFFER_EVT_OFFER, src->mimes[i]);
    lm_resource_post(dev->res, LM_WL_DATA_DEVICE_EVT_SELECTION, o->res);
}

/* Tell the keyboard-focus client about the current selection. */
static void selection_update_focus(struct lm_seat *seat)
{
    if (seat->dying || !seat->kfocus)
        return;
    struct lm_client *cl = surface_client(seat->kfocus);
    struct data_device *d;
    FOR_CLIENT_OBJECTS(d, &seat->devices, cl)
        device_send_selection(seat, d);
}

/* ---- clipboard history (gadget) ---------------------------------------------------------------------------------- */

#define CLIP_MAX 2048                   /* bytes kept of one entry */
#define CLIP_TIMEOUT_MS 1000

static const char *const clip_mimes[] = { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT" };

/* ---- the history file: one entry per line, newest first; \\ \n \r escaped. Private (0600) because clipboards hold secrets. ---- */

static void clip_save(struct lm_seat *seat)
{
    const char *path = seat->srv->config.clip_path;
    if (!path || !*path || seat->dying)
        return;
    char dir[512], tmp[560];
    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir) ||
        snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid()) >= (int)sizeof(tmp))
        return;
    for (char *p = dir + 1; *p; p++) {                  /* parent directories, private */
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(dir, 0700) < 0 && errno != EEXIST)
            return;
        *p = '/';
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) {
        if (fd >= 0)
            close(fd);
        lm_warn("clipboard: cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    for (int i = 0; i < seat->clip_n; i++) {
        for (const char *p = seat->clip_hist[i]; *p; p++) {
            if (*p == '\\') fputs("\\\\", f);
            else if (*p == '\n') fputs("\\n", f);
            else if (*p == '\r') fputs("\\r", f);
            else fputc(*p, f);
        }
        fputc('\n', f);
    }
    bool ok = !ferror(f);
    if (fclose(f) != 0)
        ok = false;
    if (!ok || rename(tmp, path) < 0) {
        lm_warn("clipboard: cannot save %s: %s", path, strerror(errno));
        unlink(tmp);
    }
}

static void clip_load(struct lm_seat *seat)
{
    const char *path = seat->srv->config.clip_path;
    FILE *f = path && *path ? fopen(path, "r") : NULL;
    if (!f)
        return;
    char line[CLIP_MAX * 2 + 8];
    while (seat->clip_n < LM_CLIP_HISTORY && fgets(line, sizeof(line), f)) {
        size_t n = strcspn(line, "\n");
        if (line[n] != '\n' && !feof(f))               /* longer than anything we write: skip the rest of the line */
            while (fgets(line, sizeof(line), f) && !strchr(line, '\n'))
                ;
        line[n] = 0;
        char *text = malloc(n + 1), *w = text;
        if (!text)
            break;
        for (const char *p = line; *p; p++) {
            if (*p == '\\' && p[1]) {
                p++;
                *w++ = *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p;
            } else {
                *w++ = *p;
            }
        }
        *w = 0;
        if (!*text) {
            free(text);
            continue;
        }
        seat->clip_hist[seat->clip_n++] = text;
    }
    fclose(f);
}

static void clip_push(struct lm_seat *seat, char *text)   /* takes ownership */
{
    for (int i = 0; i < seat->clip_n; i++) {
        if (strcmp(seat->clip_hist[i], text) == 0) {      /* copied again: move to the top */
            free(text);
            text = seat->clip_hist[i];
            memmove(&seat->clip_hist[1], &seat->clip_hist[0], (size_t)i * sizeof(char *));
            seat->clip_hist[0] = text;
            goto changed;
        }
    }
    if (seat->clip_n == LM_CLIP_HISTORY)
        free(seat->clip_hist[--seat->clip_n]);
    memmove(&seat->clip_hist[1], &seat->clip_hist[0], (size_t)seat->clip_n * sizeof(char *));
    seat->clip_hist[0] = text;
    seat->clip_n++;
changed:
    clip_save(seat);
    if (!seat->dying && seat->srv->wm)
        lm_wm_clipboard_changed(seat->srv->wm);
}

static void clip_capture_stop(struct lm_seat *seat)
{
    if (seat->cap.src)
        lm_source_remove(seat->cap.src);
    seat->cap.src = NULL;
    if (seat->cap.timer)
        lm_timer_arm_ms(seat->cap.timer, 0, 0);
    if (seat->cap.fd >= 0)
        close(seat->cap.fd);
    seat->cap.fd = -1;
}

static void clip_capture_finish(struct lm_seat *seat)
{
    char *text = seat->cap.buf;
    size_t len = seat->cap.len;
    seat->cap.buf = NULL;
    seat->cap.len = 0;
    clip_capture_stop(seat);
    bool blank = true;
    for (size_t i = 0; i < len && blank; i++)
        blank = text[i] == ' ' || text[i] == '\n' || text[i] == '\t' || text[i] == '\r' || !text[i];
    if (!text || blank) {
        free(text);
        return;
    }
    text[len] = 0;
    clip_push(seat, text);
}

static void clip_fd_cb(int fd, uint32_t events, void *data)
{
    struct lm_seat *seat = data;
    (void)events;
    for (;;) {
        size_t room = CLIP_MAX - seat->cap.len;
        if (!room) {                    /* enough: the rest is cut off */
            clip_capture_finish(seat);
            return;
        }
        ssize_t k = read(fd, seat->cap.buf + seat->cap.len, room);
        if (k > 0) {
            seat->cap.len += (size_t)k;
            continue;
        }
        if (k < 0 && errno == EINTR)
            continue;
        if (k < 0 && errno == EAGAIN)
            return;                     /* more may come */
        clip_capture_finish(seat);      /* EOF or error */
        return;
    }
}

static void clip_timeout_cb(void *data)
{
    struct lm_seat *seat = data;
    if (seat->cap.fd >= 0)
        clip_capture_finish(seat);
}

static void clip_capture_abort(struct lm_seat *seat)
{
    free(seat->cap.buf);
    seat->cap.buf = NULL;
    seat->cap.len = 0;
    clip_capture_stop(seat);
}

/* Ask the owner of a new selection for its text (asynchronously: the pipe is read from the loop). */
static void clip_capture_start(struct lm_seat *seat, struct data_source *src)
{
    clip_capture_abort(seat);
    if (!seat->srv->loop || seat->dying)
        return;
    const char *mime = NULL;
    for (size_t m = 0; m < sizeof(clip_mimes) / sizeof(clip_mimes[0]) && !mime; m++)
        for (int i = 0; i < src->n_mimes; i++)
            if (strcmp(src->mimes[i], clip_mimes[m]) == 0) {
                mime = src->mimes[i];
                break;
            }
    if (!mime)
        return;                         /* images, files ...: nothing to show */
    int p[2];
    if (pipe2(p, O_CLOEXEC | O_NONBLOCK) < 0)
        return;
    seat->cap.buf = malloc(CLIP_MAX + 1);
    if (!seat->cap.buf) {
        close(p[0]);
        close(p[1]);
        return;
    }
    seat->cap.fd = p[0];
    seat->cap.src = lm_loop_add_fd(seat->srv->loop, p[0], 1 /* EPOLLIN */, clip_fd_cb, seat);
    if (!seat->cap.timer)
        seat->cap.timer = lm_loop_add_timer(seat->srv->loop, clip_timeout_cb, seat);
    if (!seat->cap.src || !seat->cap.timer) {
        clip_capture_abort(seat);
        close(p[1]);
        return;
    }
    lm_timer_arm_ms(seat->cap.timer, CLIP_TIMEOUT_MS, 0);
    lm_resource_post(src->res, LM_WL_DATA_SOURCE_EVT_SEND, mime, p[1]);   /* the wire layer dup()s the fd */
    close(p[1]);
}

int lm_seat_clip_count(const struct lm_seat *seat) { return seat ? seat->clip_n : 0; }

const char *lm_seat_clip_get(const struct lm_seat *seat, int i)
{
    return seat && i >= 0 && i < seat->clip_n ? seat->clip_hist[i] : NULL;
}

void lm_seat_clip_clear(struct lm_seat *seat)
{
    if (!seat)
        return;
    clip_capture_abort(seat);
    for (int i = 0; i < seat->clip_n; i++)
        free(seat->clip_hist[i]);
    seat->clip_n = 0;
    clip_save(seat);
    if (!seat->dying && seat->srv->wm)
        lm_wm_clipboard_changed(seat->srv->wm);
}

static void own_source_free(struct lm_seat *seat)
{
    struct data_source *src = seat->own;
    if (!src)
        return;
    seat->own = NULL;
    struct data_offer *o, *tmp;
    lm_list_for_each_safe(o, tmp, &src->offers, src_link) {
        o->source = NULL;
        lm_list_remove(&o->src_link);
    }
    for (int i = 0; i < src->n_mimes; i++)
        free(src->mimes[i]);
    free(src->text);
    free(src);
}

static void seat_set_selection(struct lm_seat *seat, struct data_source *src)
{
    if (seat->selection == src)
        return;
    struct data_source *old = seat->selection;
    seat->selection = src;
    if (src)
        clip_capture_start(seat, src);
    if (old && old->res)
        lm_resource_post(old->res, LM_WL_DATA_SOURCE_EVT_CANCELLED);
    if (old && old == seat->own)
        own_source_free(seat);
    selection_update_focus(seat);
}

/* A history entry picked in the gadget becomes the selection, served by the compositor itself. */
void lm_seat_clip_use(struct lm_seat *seat, int i)
{
    if (!seat || seat->dying || i < 0 || i >= seat->clip_n)
        return;
    struct data_source *src = calloc(1, sizeof(*src));
    char *copy = strdup(seat->clip_hist[i]);
    if (!src || !copy) {
        free(src);
        free(copy);
        return;
    }
    static const char *const mimes[] = { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT" };
    src->seat = seat;
    src->text = copy;
    lm_list_init(&src->offers);
    lm_list_init(&src->link);
    for (size_t m = 0; m < sizeof(mimes) / sizeof(mimes[0]); m++)
        src->mimes[src->n_mimes++] = strdup(mimes[m]);
    struct data_source *old = seat->selection;
    seat->selection = src;
    clip_capture_abort(seat);
    if (old && old->res)
        lm_resource_post(old->res, LM_WL_DATA_SOURCE_EVT_CANCELLED);
    if (old && old == seat->own)
        own_source_free(seat);
    seat->own = src;
    clip_push(seat, strdup(copy));                      /* moves the entry to the top (same text) */
    selection_update_focus(seat);
}

/* The compositor itself becomes the selection owner and serves `data` (taken over, malloc'd) as `mime`. */
bool lm_seat_set_selection_data(struct lm_seat *seat, const char *mime, uint8_t *data, size_t len)
{
    struct data_source *src = seat && !seat->dying ? calloc(1, sizeof(*src)) : NULL;
    char *m = src ? strdup(mime) : NULL;
    if (!m) {
        free(src);
        free(data);
        return false;
    }
    src->seat = seat;
    src->data = data;
    src->data_len = len;
    lm_list_init(&src->offers);
    lm_list_init(&src->link);
    src->mimes[src->n_mimes++] = m;
    struct data_source *old = seat->selection;
    seat->selection = src;
    clip_capture_abort(seat);
    if (old && old->res)
        lm_resource_post(old->res, LM_WL_DATA_SOURCE_EVT_CANCELLED);
    if (old && old == seat->own)
        own_source_free(seat);
    seat->own = src;
    selection_update_focus(seat);
    return true;
}

static void source_res_destroy(struct lm_resource *res)
{
    struct data_source *src = res->data;
    struct lm_seat *seat = src->seat;
    struct data_offer *o, *tmp;
    lm_list_for_each_safe(o, tmp, &src->offers, src_link) {
        o->source = NULL;
        lm_list_remove(&o->src_link);
    }
    lm_list_remove(&src->link);
    if (seat->selection == src) {
        seat->selection = NULL;
        selection_update_focus(seat);
    }
    for (int i = 0; i < src->n_mimes; i++)
        free(src->mimes[i]);
    free(src);
}

static void source_offer(struct lm_resource *res, const union lm_arg *args)
{
    struct data_source *src = res->data;
    const char *mime = args[0].s;
    if (!mime || !*mime || strlen(mime) > MAX_MIME_LEN || src->n_mimes >= MAX_MIMES)
        return;
    for (int i = 0; i < src->n_mimes; i++)
        if (strcmp(src->mimes[i], mime) == 0)
            return;
    char *copy = strdup(mime);
    if (!copy) {
        lm_resource_post_no_memory(res);
        return;
    }
    src->mimes[src->n_mimes++] = copy;
}

/* Drag-and-drop is not supported: the mask is only validated. */
static void source_set_actions(struct lm_resource *res, const union lm_arg *args)
{
    if (args[0].u & ~7u) {
        lm_resource_post_error(res, LM_WL_DATA_SOURCE_ERR_INVALID_ACTION_MASK,
                               "invalid action mask %u", args[0].u);
        return;
    }
}

static const lm_request_fn source_handlers[] = {
    [LM_WL_DATA_SOURCE_REQ_OFFER] = source_offer,
    [LM_WL_DATA_SOURCE_REQ_DESTROY] = generic_release,
    [LM_WL_DATA_SOURCE_REQ_SET_ACTIONS] = source_set_actions,
};

static void device_res_destroy(struct lm_resource *res)
{
    struct data_device *d = res->data;
    lm_list_remove(&d->link);
    free(d);
}

static void device_start_drag(struct lm_resource *res, const union lm_arg *args)
{
    (void)res;
    /* Drag-and-drop is not performed; refuse politely so the client resets its state. */
    struct data_source *src = source_of(args[0].r);
    if (src)
        lm_resource_post(src->res, LM_WL_DATA_SOURCE_EVT_CANCELLED);
}

static void device_set_selection(struct lm_resource *res, const union lm_arg *args)
{
    struct data_device *d = res->data;
    struct lm_seat *seat = d->seat;
    struct data_source *src = source_of(args[0].r);
    /*
     * Only the client that has the keyboard focus may own the clipboard
     * (serials are not checked: toolkits pass whatever input serial they saw last).
     */
    bool focused = seat->kfocus && surface_client(seat->kfocus) == res->client;
    if (!focused) {
        if (src)
            lm_resource_post(src->res, LM_WL_DATA_SOURCE_EVT_CANCELLED);
        return;
    }
    seat_set_selection(seat, src);
}

static const lm_request_fn device_handlers[] = {
    [LM_WL_DATA_DEVICE_REQ_START_DRAG] = device_start_drag,
    [LM_WL_DATA_DEVICE_REQ_SET_SELECTION] = device_set_selection,
    [LM_WL_DATA_DEVICE_REQ_RELEASE] = generic_release,
};

static void ddm_create_data_source(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_seat *seat = res->data;
    struct data_source *src = calloc(1, sizeof(*src));
    if (!src) {
        lm_resource_post_no_memory(res);
        return;
    }
    src->seat = seat;
    lm_list_init(&src->offers);
    src->res = lm_resource_create(res->client, args[0].n, &lm_wl_data_source_interface,
                                  res->version, source_handlers, src, source_res_destroy);
    if (!src->res) {
        free(src);
        return;
    }
    lm_list_append(&seat->sources, &src->link);
}

static void ddm_get_data_device(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_seat *seat = res->data;
    struct data_device *d = calloc(1, sizeof(*d));
    if (!d) {
        lm_resource_post_no_memory(res);
        return;
    }
    d->seat = seat;
    d->res = lm_resource_create(res->client, args[0].n, &lm_wl_data_device_interface,
                                res->version, device_handlers, d, device_res_destroy);
    if (!d->res) {
        free(d);
        return;
    }
    lm_list_append(&seat->devices, &d->link);
    if (seat->kfocus && surface_client(seat->kfocus) == res->client)
        device_send_selection(seat, d);
}

static const lm_request_fn ddm_handlers[] = {
    [LM_WL_DATA_DEVICE_MANAGER_REQ_CREATE_DATA_SOURCE] = ddm_create_data_source,
    [LM_WL_DATA_DEVICE_MANAGER_REQ_GET_DATA_DEVICE] = ddm_get_data_device,
};

static void ddm_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    struct lm_seat *seat = data;
    lm_resource_create(client, id, &lm_wl_data_device_manager_interface, version, ddm_handlers,
                       seat, NULL);
}

/* ---- input entry point ---------------------------------------------------------------------------------------------- */

/*
 * Screen lock: keys go to the lock screen (and nowhere else, the wm's shortcuts included), everything else is dropped. The
 * bookkeeping of pressed keys stays right so that a key released after the unlock does not confuse the clients.
 */
static void handle_locked_input(struct lm_seat *seat, const struct lm_input_event *ev)
{
    if (ev->type == LM_INPUT_MODS) {
        keyboard_set_state(seat, ev->u.mods.depressed, ev->u.mods.latched, ev->u.mods.locked, ev->u.mods.group);
        return;
    }
    if (ev->type != LM_INPUT_KEY)
        return;
    uint32_t code = ev->u.key.code;
    if (ev->u.key.repeat)
        return;
    if (ev->u.key.pressed) {
        code_add(seat->swallowed, &seat->n_swallowed, MAX_KEYS, code);     /* its release must not reach a client either */
        uint32_t mods = ev->u.key.mods_depressed | ev->u.key.mods_latched | ev->u.key.mods_locked;
        if (seat->srv->lock)
            lm_lock_key(seat->srv->lock, code, mods, ev->u.key.group);
    } else {
        code_remove(seat->swallowed, &seat->n_swallowed, code);
        code_remove(seat->keys_down, &seat->n_keys, code);
    }
    keyboard_set_state(seat, ev->u.key.mods_depressed, ev->u.key.mods_latched, ev->u.key.mods_locked, ev->u.key.group);
}

void lm_seat_set_locked(struct lm_seat *seat, bool locked)
{
    if (!seat || seat->locked == locked)
        return;
    seat->locked = locked;
    if (locked) {
        if (seat->grab) {
            struct lm_pointer_grab *g = seat->grab;
            seat->grab = NULL;
            if (g->ops && g->ops->cancel)
                g->ops->cancel(g);
        }
        seat->n_fwd = 0;                /* buttons held now are released while locked, nobody sees that */
        lm_seat_pointer_set_focus(seat, NULL, 0, 0);
        lm_seat_set_keyboard_focus(seat, NULL);
        touch_cancel_all(seat, lm_now_msec32());
    }
    cursor_changed(seat);
    if (!locked && seat->refocus_idle)
        lm_idle_schedule(seat->refocus_idle);       /* the pointer finds the surface under it again */
}

void lm_seat_handle_input(struct lm_seat *seat, const struct lm_input_event *ev)
{
    if (!seat || !ev || seat->dying)
        return;
    if (seat->srv->lock && lm_lock_activity(seat->srv->lock))
        return;                         /* it only woke the monitors from standby */
    ensure_pointer_pos(seat);
    if (seat->locked) {
        handle_locked_input(seat, ev);
        return;
    }

    switch (ev->type) {
    case LM_INPUT_KEY:
        handle_key(seat, ev);
        break;
    case LM_INPUT_MODS:
        keyboard_set_state(seat, ev->u.mods.depressed, ev->u.mods.latched, ev->u.mods.locked, ev->u.mods.group);
        break;
    case LM_INPUT_POINTER_MOTION:
        if (!seat->pos_valid)
            break;
        if (!isfinite(ev->u.motion.dx) || !isfinite(ev->u.motion.dy))
            break;
        pointer_set_position(seat, seat->px + ev->u.motion.dx, seat->py + ev->u.motion.dy,
                             ev->time_ms);
        break;
    case LM_INPUT_POINTER_MOTION_ABS: {
        double gx, gy;
        if (touch_global_pos(seat, ev->u.motion_abs.x, ev->u.motion_abs.y, &gx, &gy))
            pointer_set_position(seat, gx, gy, ev->time_ms);
        break;
    }
    case LM_INPUT_POINTER_BUTTON:
        pointer_button(seat, ev->time_ms, ev->u.button.code, ev->u.button.pressed);
        break;
    case LM_INPUT_POINTER_AXIS:
        if (isfinite(ev->u.axis.value))
            pointer_axis(seat, ev->time_ms, ev->u.axis.orientation, ev->u.axis.value,
                         ev->u.axis.discrete);
        break;
    case LM_INPUT_TOUCH_DOWN:
        seat_note_touch_device(seat);
        touch_down(seat, ev);
        break;
    case LM_INPUT_TOUCH_MOTION:
        seat_note_touch_device(seat);
        touch_motion(seat, ev);
        break;
    case LM_INPUT_TOUCH_UP:
        seat_note_touch_device(seat);
        touch_up(seat, ev);
        break;
    case LM_INPUT_TOUCH_CANCEL:
        seat_note_touch_device(seat);
        touch_cancel_all(seat, ev->time_ms);
        break;
    case LM_INPUT_TOUCH_FRAME:
        /* every touch event is already sent in its own frame */
        break;
    }
}

void lm_seat_handle_input_cb(const struct lm_input_event *ev, void *data)
{
    lm_seat_handle_input(data, ev);
}

/* ---- lifecycle ----------------------------------------------------------------------------------------------------------- */

struct lm_seat *lm_seat_create(struct lm_server *srv)
{
    struct lm_seat *seat = calloc(1, sizeof(*seat));
    if (!seat)
        return NULL;
    seat->srv = srv;
    seat->keymap_fd = -1;
    seat->cap.fd = -1;
    lm_list_init(&seat->bindings);
    lm_list_init(&seat->pointers);
    lm_list_init(&seat->keyboards);
    lm_list_init(&seat->touches);
    lm_list_init(&seat->devices);
    lm_list_init(&seat->sources);
    lm_list_init(&seat->offers);
    lm_list_init(&seat->clients);
    lm_list_init(&seat->pfocus_destroy.link);
    lm_list_init(&seat->kfocus_destroy.link);
    lm_list_init(&seat->cursor_destroy.link);
    for (int i = 0; i < MAX_TOUCH; i++)
        lm_list_init(&seat->points[i].destroy.link);

    seat->keymap_fd = keymap_make_fd(srv->config.kbd_layouts, srv->config.kbd_nlayouts, &seat->keymap_size);
    if (seat->keymap_fd < 0)
        lm_warn("could not create the keymap file: %s; clients get no keymap", strerror(errno));

    clip_load(seat);
    if (srv->loop)
        seat->refocus_idle = lm_loop_add_idle(srv->loop, refocus_idle_cb, seat);

    seat->seat_global = lm_global_create(srv, &lm_wl_seat_interface, LM_WL_SEAT_VERSION,
                                         seat_bind, seat);
    seat->ddm_global = lm_global_create(srv, &lm_wl_data_device_manager_interface,
                                        LM_WL_DATA_DEVICE_MANAGER_VERSION, ddm_bind, seat);
    if (!seat->seat_global || !seat->ddm_global) {
        lm_error("could not advertise wl_seat");
        lm_seat_destroy(seat);
        return NULL;
    }
    return seat;
}

void lm_seat_destroy(struct lm_seat *seat)
{
    if (!seat)
        return;
    seat->dying = true;

    if (seat->grab) {
        struct lm_pointer_grab *g = seat->grab;
        seat->grab = NULL;
        if (g->ops && g->ops->cancel)
            g->ops->cancel(g);
    }
    if (seat->seat_global)
        lm_global_destroy(seat->seat_global);
    if (seat->ddm_global)
        lm_global_destroy(seat->ddm_global);
    if (seat->refocus_idle)
        lm_idle_destroy(seat->refocus_idle);

    clip_capture_abort(seat);
    if (seat->cap.timer)
        lm_timer_destroy(seat->cap.timer);
    seat->cap.timer = NULL;
    for (int i = 0; i < seat->clip_n; i++)
        free(seat->clip_hist[i]);
    seat->clip_n = 0;

    /* Clients may outlive the seat: release every resource that points into it. */
    seat->selection = NULL;
    DESTROY_ALL(struct data_offer, &seat->offers);
    own_source_free(seat);
    DESTROY_ALL(struct data_source, &seat->sources);
    DESTROY_ALL(struct data_device, &seat->devices);
    DESTROY_ALL(struct seat_touch, &seat->touches);
    DESTROY_ALL(struct seat_keyboard, &seat->keyboards);
    DESTROY_ALL(struct seat_pointer, &seat->pointers);
    DESTROY_ALL(struct seat_binding, &seat->bindings);

    for (int i = 0; i < MAX_TOUCH; i++)
        if (seat->points[i].active)
            touch_release_point(&seat->points[i]);
    lm_listener_remove(&seat->pfocus_destroy);
    lm_listener_remove(&seat->kfocus_destroy);
    cursor_drop(seat);
    struct seat_client *sc, *sc_tmp;
    lm_list_for_each_safe(sc, sc_tmp, &seat->clients, link) {
        lm_listener_remove(&sc->destroy);
        lm_list_remove(&sc->link);
        free(sc);
    }

    if (seat->keymap_fd >= 0)
        close(seat->keymap_fd);
    free(seat);
}
