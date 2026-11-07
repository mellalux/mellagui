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

#define LM_MAX_APPS 12

struct lm_app_entry {
    const char *name;           /* menu label */
    const char *cmd;            /* shell command */
};

struct lm_config {
    const char *backend;        /* "drm" | "headless" */
    int headless_width;         /* --size WxH (default 1280x720) */
    int headless_height;
    const char *socket_name;    /* --socket, NULL = first free wayland-N */
    const char *exec_cmd;       /* --exec, shell command spawned after start */
    const char *run_cmd;        /* command for Super+R / the panel's Run button (default "mellagui-run") */
    const char *terminal_cmd;   /* command for Super+Enter (default "mellagui-term") */
    const char *settings_cmd;   /* command for Super+I and the "Settings" menu entry (default "mellagui-settings") */
    const char *settings_path;  /* file the settings panel saves to (--settings); NULL = nothing is saved */
    const char *clip_path;      /* file the clipboard history is kept in (--clipboard-file); NULL = not saved */
    const char *gadgets_path;   /* file the gadgets' places and sizes are kept in (--gadgets-file); NULL = not saved */
    int settings_access;        /* who may use mellagui_settings_v1: 1 = any client of the session, 0 = only MellaGUI's own programs
                                 * (an executable called mellagui-* in the compositor's directory), -1 (default) = 0 on drm, 1 on headless */
    const char *screenshot_dir; /* where Print / Alt+Print save PNGs (--screenshot-dir); NULL = the clipboard only */
    const char *lock_path;      /* file the screen lock's password / PIN hash is kept in (--lock-file); NULL = in memory only */
    struct lm_app_entry apps[LM_MAX_APPS]; /* entries of the "MellaGUI" menu (--app NAME=CMD; defaults filled by main.c) */
    int napps;
    const char *drm_device;     /* NULL = auto-detect /dev/dri/card* */
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
        .sound = true,
        .audio_device = "default",
        .dmabuf = true,
        .gadget = true,
    };
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
    struct lm_lock *lock;               /* screen lock (password / PIN), see lock.h */
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
