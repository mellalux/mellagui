#ifndef MELLAGUI_SERVER_H
#define MELLAGUI_SERVER_H

/*
 * Wayland display server core: socket, clients, per-client object table,
 * global registry, request dispatch, event posting, protocol errors, and the
 * aggregate struct lm_server. Owner: server.c.
 *
 * ---- Object / resource model ------------------------------------------------
 * Every protocol object is a struct lm_resource owned by its client. Module
 * code creates resources (lm_resource_create / lm_resource_create_server),
 * attaches a request-handler table and a `data` pointer, and receives requests
 * through the handlers. The handler table is an array indexed by request
 * opcode (use the LM_*_REQ_* names from protocol.h); a NULL entry means "not
 * implemented": the request is ignored (and logged at debug level).
 *
 * Request dispatch (server.c), per message:
 *   1. lm_wire_next; unknown object id -> wl_display.error invalid_object
 *   2. opcode >= iface->n_requests or message.since > res->version
 *      -> error invalid_method
 *   3. lm_wire_decode with the message signature; failure -> error
 *      invalid_method (malformed) and disconnect
 *   4. every 'o' arg is resolved: 0 allowed only if nullable, unknown id or
 *      wrong interface (vs. message.types[]) -> error invalid_object;
 *      args[i].r is then set. 'n' args stay raw ids in args[i].n.
 *   5. handlers[opcode](res, args)
 *   6. fds in args are closed by the server after the handler returns:
 *      a handler that wants to keep one MUST dup() it.
 * After the handler returns the server never touches `res` again (so
 * a destructor request handler may call lm_resource_destroy(res)).
 * A protocol error disconnects the client once the current dispatch round
 * ends; the error event is flushed first on a best-effort basis.
 *
 * Handler contract: if a handler detects a protocol violation it calls
 * lm_resource_post_error()/lm_client_post_error() and returns immediately
 * (it must leave its module state consistent: the client's resources are
 * destroyed right afterwards).
 *
 * Destruction: lm_resource_destroy() (a) emits res->destroy_signal,
 * (b) calls res->destroy(res) (module frees res->data), (c) unlinks the
 * resource, and (d) for client-allocated ids queues wl_display.delete_id.
 * On client disconnect all remaining resources are destroyed in REVERSE
 * creation order (newest first), then client->destroy_signal is emitted.
 * Hence a dependent object (xdg_surface, wl_subsurface, wl_buffer from a
 * pool...) created after the object it references is torn down first.
 * Destroy hooks must never touch resources of the same client other than
 * through their own references (use listeners on destroy_signal).
 *
 * Output backpressure: if a client's pending output exceeds the wire limit
 * the client is disconnected (no error event can be delivered).
 * Malformed input must never crash or hang the compositor.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "clockfmt.h"
#include "keys.h"
#include "list.h"
#include "loop.h"
#include "protocol.h"
#include "util.h"
#include "wire.h"

struct lm_server;
struct lm_client;
struct lm_resource;
struct lm_global;

/* Opaque module states, each defined privately in its own .c and created by main.c. */
struct lm_backend;
struct lm_renderer;
struct lm_compositor;
struct lm_shm;
struct lm_output_mgr;
struct lm_seat;
struct lm_xdg;
struct lm_wm;
struct lm_input;
struct lm_settings;
struct lm_capture;
struct lm_tray;
struct lm_menu;
struct lm_idle_inhibit;
struct lm_mixer;
struct lm_sysinfo;
struct lm_dmabuf;

typedef void (*lm_request_fn)(struct lm_resource *res, const union lm_arg *args);
typedef void (*lm_resource_destroy_fn)(struct lm_resource *res);
/* Called when a client binds a global: create the resource with `id`. */
typedef void (*lm_bind_fn)(struct lm_client *client, void *data,
                           uint32_t version, uint32_t id);

#define LM_ID_MAX_CLIENT   0xfeffffffu /* client-allocated ids: 1..LM_ID_MAX_CLIENT */
#define LM_ID_SERVER_BASE  0xff000000u /* server-allocated ids start here */

struct lm_resource {
    struct lm_client *client;
    uint32_t id;
    const struct lm_interface *iface;
    uint32_t version;                 /* negotiated version of this object */
    const lm_request_fn *handlers;    /* may be NULL (no requests handled) */
    void *data;                       /* owned by the creating module */
    lm_resource_destroy_fn destroy;   /* may be NULL */
    struct lm_signal destroy_signal;  /* emitted with data = struct lm_resource * */
    struct lm_list link;              /* in client->resources, creation order */
    void *priv;                       /* server.c private */
};

struct lm_client {
    struct lm_server *server;
    struct lm_list link;              /* in server->clients */
    int fd;                           /* owned: closed on destroy */
    struct lm_wire wire;
    struct lm_source *source;
    pid_t pid;                        /* SO_PEERCRED */
    uid_t uid;
    gid_t gid;
    struct lm_list resources;         /* creation order (tail = newest) */
    struct lm_signal destroy_signal;  /* data = struct lm_client *, emitted last on teardown */
    bool dying;                       /* disconnect scheduled; ignore further requests */
    void *priv;                       /* server.c private (object table, ...) */
};

struct lm_global {
    struct lm_list link;              /* in server->globals */
    struct lm_server *server;
    uint32_t name;                    /* registry name, never reused */
    const struct lm_interface *iface;
    uint32_t version;
    lm_bind_fn bind;
    void *data;
    void *priv;
};

/* ---- configuration (filled by main.c from argv/env) ------------------------- */

struct lm_config {
    const char *backend;        /* "drm" | "headless" */
    int headless_width;         /* --size WxH (default 1280x720) */
    int headless_height;
    int headless_extra[3][2];   /* --output WxH (repeatable): more headless outputs, to the right of the first one */
    int headless_nextra;
    const char *socket_name;    /* --socket, NULL = first free wayland-N */
    const char *exec_cmd;       /* --exec, shell command spawned after start */
    const char *run_cmd;        /* command for Super+R / the panel's Run button (default "mellagui-run") */
    const char *terminal_cmd;   /* command for Super+Enter (default "mellagui-term") */
    const char *view_cmd;       /* command for the screenshot card's click: the image viewer (default "mellagui-view") */
    const char *suspend_cmd, *reboot_cmd, *poweroff_cmd;     /* the session dialog's Suspend / Restart / Shutdown ([commands] suspend, reboot, poweroff; default systemctl ...) */
    const char *saver_cmd;      /* the screensaver program, a full screen client ([commands] saver, default "mellagui-matrix") */
    bool saver;                 /* start the screensaver when idle (settings: saver; default true) */
    int saver_min;              /* ... after this many minutes without input, 0 = never (settings: saver-minutes; default 10) */
    int night_mode;             /* night light: 0 off, 1 on, 2 between night_from and night_to (settings: night-light), see nightlight.h */
    int night_kelvin;           /* ... at this colour temperature (settings: night-temp; default 3400) */
    char night_from[8], night_to[8];   /* "HH:MM" (settings: night-from, night-to; default 21:00 and 06:30) */
    const char *status_cmd;     /* the battery / network buttons of the panel, started with the compositor ([commands] status, default "mellagui-status", "none" = off) */
    const char *calendar_cmd;   /* the calendar program, started with the compositor and shown by a click on the clock ([commands] calendar, default "mellagui-calendar") */
    struct lm_clockfmt clock;   /* how the panel's clock and the date are written (settings: clock-hours, clock-seconds, clock-date, clock-weekday, date-format) */
    bool cal_enabled;           /* a click on the clock shows the calendar (settings: cal-enabled; default true) */
    int cal_week_start;         /* the calendar's first weekday: 1 Monday (default), 0 Sunday (settings: cal-week-start mon|sun) */
    bool cal_weeknum;           /* ... with ISO week numbers (settings: cal-weeks; default false) */
    bool osk_avoid;             /* windows keep out of the on-screen keyboard's way (settings: osk-avoid; default true) */
    const char *settings_cmd;   /* command for Super+I (default "mellagui-settings") */
    const char *conf_path;      /* the mellagui.conf that is read (--config, default per user), NULL = none (settings: config-file) */
    const char *settings_path;  /* file the settings panel saves to (--settings); NULL = nothing is saved */
    const char *clip_path;      /* file the clipboard history is kept in (--clipboard-file); NULL = not saved */
    const char *gadgets_path;   /* file the gadgets' places and sizes are kept in (--gadgets-file); NULL = not saved */
    int settings_access;        /* who may use mellagui_settings_v1: 1 = any client of the session, 0 = only MellaGUI's own programs
                                 * (an executable called mellagui-* in the compositor's directory), -1 (default) = 0 on drm, 1 on headless */
    const char *screenshot_dir; /* where Print / Alt+Print save PNGs (--screenshot-dir); NULL = the clipboard only */
    const char *lock_path;      /* file the screen lock's password / PIN hash is kept in (--lock-file); NULL = in memory only */
    struct lm_keybinds keys;     /* keyboard shortcuts (keys.h; mellagui.conf [keys]) */
    const char *drm_device;     /* NULL = auto-detect /dev/dri/card* */
    char wallpaper[256];        /* image file of the desktop background (settings: wallpaper), "" = the gradient; "~/" is the home directory */
    char output_order[200];     /* connector names, leftmost first (settings: output-order); outputs not named follow in connection order */
    int wallpaper_mode;         /* 0 fill (cover, cropped), 1 fit (whole picture, bars), 2 stretch, 3 center */
    char audio_profile[192];    /* the sound card profile to restore at start, "card description|profile name" (settings: audio-profile-keep) */
    char audio_device[64];      /* ALSA device for MellaGUI's own beeps (settings: audio-out) */
    int drm_mode_w, drm_mode_h, drm_mode_hz;   /* --mode WxH[@Hz]: 0 = automatic (preferred, corrected to the panel's shape), -1 = --mode preferred */
    const char *control_path;   /* headless control socket path (--control), NULL = none */
    const char *dump_path;      /* env MELLAGUI_DUMP: headless dump target for SIGUSR1 */
    const char *log_file;
    int log_level;              /* enum lm_log_level */
    int repeat_delay_ms;        /* default 400 */
    int repeat_rate;            /* keys/second, default 25 */
    double pointer_speed;       /* multiplier, default 1.0 */
    bool natural_scroll;
    int kbd_layouts[4];         /* --layouts: indices for lm_layout_*() (keymap.h), the order is the XKB group order */
    int kbd_nlayouts;           /* 0 = just "us" */
    int kbd_active;             /* layout (lm_layout_*() index) active at start, -1 = the first of kbd_layouts */
    bool sticky_keys;           /* --sticky: start with sticky keys enabled */
    bool gadget;                /* desktop gadget (clipboard viewer) visible at start (default true; --no-gadget; Super+G) */
    bool mixer;                 /* volume mixer gadget visible at start (default false; --mixer; Super+V; setting `mixer`) */
    bool dmabuf;                /* offer zwp_linux_dmabuf_v1 so clients can render with the GPU (default true; --no-dmabuf) */
    bool sysinfo;               /* system monitor gadget visible at start (default false; --sysinfo; Super+S; setting `sysinfo`) */
    bool sound;                 /* feedback beeps for sticky keys (default true; --no-sound; never on headless) */
    int renderer;               /* --renderer software|auto|gl: 0 = software, 1 = GL when it works, 2 = GL required (default: auto on drm, software headless) */
    int logind;                 /* --logind off|auto|on: 0 = never, 1 = when the session allows it (default), 2 = required */
    bool grab_input;            /* EVIOCGRAB the evdev devices (default true on drm) */
};

static inline void lm_config_defaults(struct lm_config *c)
{
    *c = (struct lm_config){
        .backend = "drm",
        .headless_width = 1280,
        .headless_height = 720,
        .run_cmd = "mellagui-run",
        .terminal_cmd = "mellagui-term",
        .settings_cmd = "mellagui-settings",
        .kbd_active = -1,
        .settings_access = -1,
        .log_level = 2, /* LM_LOG_INFO */
        .repeat_delay_ms = 400,
        .repeat_rate = 25,
        .pointer_speed = 1.0,
        .grab_input = true,
        .logind = 1,
        .renderer = -1,
        .sound = true,
        .audio_device = "default",
        .dmabuf = true,
        .gadget = true,
        .saver = true,
        .saver_min = 10,
        .night_kelvin = 3400,
        .night_from = "21:00",
        .night_to = "06:30",
        .osk_avoid = true,
        .clock = { .show_date = true },
        .cal_enabled = true,
        .cal_week_start = 1,
    };
    lm_keys_defaults(&c->keys);
}

/* ---- aggregate server state ------------------------------------------------- */

struct lm_server {
    struct lm_config config;
    struct lm_loop *loop;

    /* display core: private to server.c */
    char socket_name[64];             /* e.g. "wayland-1" */
    char socket_path[256];            /* full path under $XDG_RUNTIME_DIR */
    int listen_fd;
    int lock_fd;
    struct lm_source *listen_source;
    struct lm_list clients;           /* struct lm_client.link */
    struct lm_list globals;           /* struct lm_global.link */
    uint32_t next_global_name;
    uint32_t serial;

    /*
     * Module states. Created by main.c in this order, destroyed in reverse;
     * any pointer may be NULL while the system is starting up or shutting
     * down, so modules must tolerate a NULL peer at those times.
     */
    struct lm_backend *backend;
    struct lm_renderer *render;
    struct lm_compositor *compositor;   /* wl_compositor, wl_subcompositor, surfaces */
    struct lm_shm *shm;
    struct lm_output_mgr *outputs;      /* wl_output globals + layout */
    struct lm_seat *seat;               /* wl_seat, data device, cursor */
    struct lm_xdg *xdg;                 /* xdg_wm_base, decoration manager */
    struct lm_wm *wm;
    struct lm_input *input;             /* evdev (or inject-only for headless) */
    struct lm_settings *settings;       /* mellagui_settings_v1 + settings file */
    struct lm_capture *capture;         /* mellagui_capture_v1: the screen for mellagui-record (capture.h) */
    struct lm_idle_inhibit *idle_inhibit; /* zwp_idle_inhibit_manager_v1 (idleinhibit.h) */
    struct lm_menu *menu;               /* mellagui_menu_v1: a menu at the pointer for programs (menu.h) */
    struct lm_tray *tray;               /* mellagui_tray_v1: programs' icons in the top bar (tray.h) */
    bool reloaded;                      /* a reload happened; reload_result = rejected lines of the last one, -1 = no readable file */
    int reload_result;
    int (*reload_config)(struct lm_server *, bool quiet); /* set by main.c: read mellagui.conf again (SIGUSR2, control `reload`, the reload-config shortcut); returns the number of rejected lines, -1 = no file; `quiet` = no "read again" card on the screen */
    struct lm_lock *lock;               /* screen lock (password / PIN), see lock.h */
    struct lm_night *night;             /* night light: warmer colours in the evening (nightlight.h) */
    struct lm_saver *saver;             /* the screensaver (starts a program when idle), see saver.h */
    struct lm_logind *logind;           /* device access through systemd-logind, NULL = open(2) + VT_PROCESS (logind.h) */
    struct lm_mixer *mixer;             /* volume mixer behind the desktop gadget (pactl / amixer) */
    struct lm_sysinfo *sysinfo;         /* CPU / memory / temperature behind the system monitor gadget */
    struct lm_dmabuf *dmabuf;           /* zwp_linux_dmabuf_v1 (NULL when off or no render node) */

    void *priv;
};

/* ---- server lifecycle -------------------------------------------------------- */

/* Allocates and initialises a server; copies *cfg. Does not open the socket. */
struct lm_server *lm_server_create(struct lm_loop *loop, const struct lm_config *cfg);
/*
 * Create $XDG_RUNTIME_DIR/<name> + ".lock" (flock), listen, and register the
 * accept source with the loop. Name: config.socket_name or first free
 * wayland-0 .. wayland-32. Returns 0 or -1 (message logged). Also installs a
 * SIGCHLD handler through the loop (reaps lm_server_spawn children).
 */
int lm_server_listen(struct lm_server *srv);
/*
 * Disconnects all clients (destroying their resources), destroys remaining
 * globals, unlinks socket and lock file, frees the server. Module states must
 * already have been destroyed by main.c (they unregister their own globals).
 */
void lm_server_destroy(struct lm_server *srv);

const char *lm_server_socket_name(const struct lm_server *srv);
/* Monotonic serial for input/configure events (never 0). */
uint32_t lm_server_next_serial(struct lm_server *srv);
/* Ask the main loop to exit with code 0. */
void lm_server_quit(struct lm_server *srv);
/*
 * fork/exec `/bin/sh -c cmd` detached, with WAYLAND_DISPLAY set to our socket
 * and DISPLAY removed from the environment. Returns pid or -1.
 */
pid_t lm_server_spawn(struct lm_server *srv, const char *cmd);
/*
 * The same, but the caller is told how the command ended: `fn(data, status, err)` runs from the loop when the child is gone (`status` = the wait
 * status as WIFEXITED / WEXITSTATUS see it, `err` = the first 200 bytes of what it wrote to stderr, "" none). stdout goes nowhere. At most 4 such
 * children at a time (more run unwatched). Returns the pid or -1.
 */
typedef void (*lm_exit_fn)(void *data, int status, const char *err);
pid_t lm_server_spawn_watch(struct lm_server *srv, const char *cmd, lm_exit_fn fn, void *data);

/* ---- globals ------------------------------------------------------------------ */

/*
 * Advertise a global to all current and future registries. `version` is the
 * max version offered. A bind with a version of 0 or above `version` is a
 * protocol error. NULL on OOM.
 */
struct lm_global *lm_global_create(struct lm_server *srv,
                                   const struct lm_interface *iface,
                                   uint32_t version, lm_bind_fn bind, void *data);
/* Sends wl_registry.global_remove to registries; existing bound resources stay valid. */
void lm_global_destroy(struct lm_global *g);

/* ---- clients -------------------------------------------------------------------- */

/* Schedule disconnect after the current dispatch round (idempotent, safe anywhere). */
void lm_client_disconnect(struct lm_client *client);
/* Look up a live resource by id (client or server range); NULL if none. */
struct lm_resource *lm_client_get_object(struct lm_client *client, uint32_t id);
/*
 * Post wl_display.error(object_id, code, message) then schedule disconnect.
 * object_id may be 0 if no object applies.
 */
void lm_client_post_error(struct lm_client *client, uint32_t object_id,
                          uint32_t code, const char *fmt, ...) LM_PRINTF(4, 5);
void lm_client_post_no_memory(struct lm_client *client);

/* ---- resources ------------------------------------------------------------------- */

/*
 * Create a client-allocated object. `id` comes from a 'n' request argument.
 * Fails (returns NULL) if the id is 0, out of the client range, or already in
 * use; in those cases a wl_display.error has already been posted, so the
 * caller just returns. `version` is clamped by the caller to what it supports.
 */
struct lm_resource *lm_resource_create(struct lm_client *client, uint32_t id,
                                       const struct lm_interface *iface,
                                       uint32_t version,
                                       const lm_request_fn *handlers, void *data,
                                       lm_resource_destroy_fn destroy);
/* Create a server-allocated object (id >= LM_ID_SERVER_BASE), e.g. wl_data_offer. */
struct lm_resource *lm_resource_create_server(struct lm_client *client,
                                              const struct lm_interface *iface,
                                              uint32_t version,
                                              const lm_request_fn *handlers,
                                              void *data,
                                              lm_resource_destroy_fn destroy);
/* See "Destruction" above. Safe to call exactly once per resource. */
void lm_resource_destroy(struct lm_resource *res);

/*
 * Send an event. Variadic arguments follow the event's signature
 * (iface->events[opcode].signature) with these C types:
 *   i: int32_t   u: uint32_t   f: lm_fixed_t   s: const char * (NULL if '?')
 *   o: struct lm_resource * (NULL if '?')
 *   n: struct lm_resource * (a resource created with lm_resource_create_server)
 *   a: const struct lm_array *    h: int fd (dup'd; caller keeps its fd)
 * Wrong types are undefined behaviour; the signature is the contract.
 * Returns 0, or -1 if the client had to be disconnected (output overflow);
 * callers normally ignore the result.
 */
int lm_resource_post(struct lm_resource *res, uint32_t opcode, ...);
int lm_resource_post_va(struct lm_resource *res, uint32_t opcode, va_list ap);

/* wl_display.error on res->id, then disconnect the client. */
void lm_resource_post_error(struct lm_resource *res, uint32_t code,
                            const char *fmt, ...) LM_PRINTF(3, 4);
void lm_resource_post_no_memory(struct lm_resource *res);

#endif
