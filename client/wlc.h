#ifndef MELLAGUI_WLC_H
#define MELLAGUI_WLC_H

/*
 * Tiny Wayland client library for the mellagui clients: connection,
 * object table with per-object event handlers, and request marshalling. It
 * reuses src/wire.c and the interface tables of src/protocol.c, so no
 * libwayland is needed.
 */

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"
#include "wire.h"

struct wlc;

typedef void (*wlc_event_fn)(struct wlc *c, uint32_t id, uint32_t opcode,
                             const union lm_arg *args, void *data);

/* name NULL = $WAYLAND_DISPLAY (default "wayland-0"). NULL + message on stderr on failure. */
struct wlc *wlc_connect(const char *name);
void wlc_destroy(struct wlc *c);
int wlc_fd(const struct wlc *c);
bool wlc_dead(const struct wlc *c);

/* Allocate a new client-side object id of `iface`; events go to fn(data). */
uint32_t wlc_alloc(struct wlc *c, const struct lm_interface *iface, wlc_event_fn fn, void *data);
/* Change the handler of an existing object (also for server-created ones). */
void wlc_set_handler(struct wlc *c, uint32_t id, wlc_event_fn fn, void *data);
/* Forget an object locally (call after sending its destructor request). */
void wlc_forget(struct wlc *c, uint32_t id);

/* Send request `opcode` on object `id`; args follow the request signature. */
int wlc_send(struct wlc *c, uint32_t id, uint32_t opcode, const union lm_arg *args);
#define WLC_SEND(c, id, op, ...) wlc_send((c), (id), (op), (const union lm_arg[]){ __VA_ARGS__ })
#define WLC_SEND0(c, id, op) wlc_send((c), (id), (op), NULL)

/* wl_display.get_registry; global events go to fn. Returns the registry id. */
uint32_t wlc_get_registry(struct wlc *c, wlc_event_fn fn, void *data);
/* wl_registry.bind: returns the new object id. */
uint32_t wlc_bind(struct wlc *c, uint32_t registry, uint32_t name, const struct lm_interface *iface,
                  uint32_t version, wlc_event_fn fn, void *data);

/* Flush output, wait up to timeout_ms (-1 = block) for input and dispatch it. Returns events dispatched or -1 on fatal error. */
int wlc_dispatch(struct wlc *c, int timeout_ms);
/* Dispatch only what is already readable (never blocks). */
int wlc_dispatch_pending(struct wlc *c);
int wlc_flush(struct wlc *c);
/* wl_display.sync and wait for the callback. Returns 0 or -1. */
int wlc_roundtrip(struct wlc *c);

#endif
