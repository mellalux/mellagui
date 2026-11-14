#ifndef MELLAGUI_PROTOCOL_H
#define MELLAGUI_PROTOCOL_H

/*
 * Hand-written interface descriptors for every supported Wayland interface,
 * plus opcode and enum constants. Owner: protocol.c (descriptor tables only;
 * no dependency on server.c so clients can link it too).
 *
 * protocol.c MUST define every `extern const struct lm_interface` below with
 * message tables exactly matching the signature comments in the opcode
 * enums. All modules use the opcode names here, never raw numbers.
 *
 * Naming: LM_<INTERFACE>_REQ_<NAME> (client->server request opcodes),
 *         LM_<INTERFACE>_EVT_<NAME> (server->client event opcodes),
 *         LM_<INTERFACE>_ERR_<NAME> / LM_<ENUM>_<NAME> (protocol enum values).
 * The trailing comments give the wire signature and the `since` version
 * when > 1. '?' = nullable.
 *
 * ---------------------------------------------------------------------------
 * RESOURCE DATA CONTRACT: what struct->data points to, per interface
 * (the struct is defined in the named header; the *only* module allowed to
 * create such resources is the owner)
 *   wl_display, wl_registry, wl_callback .... server.c internal
 *   wl_compositor / wl_subcompositor ....... surface.c (global data only)
 *   wl_surface ............................. struct lm_surface   (surface.h)
 *   wl_region .............................. surface.c internal
 *   wl_subsurface .......................... surface.c internal
 *   wl_shm / wl_shm_pool ................... shm.c internal
 *   wl_buffer .............................. struct lm_buffer    (shm.h)
 *   wl_output .............................. struct lm_output    (backend.h);
 *                                            NULL after the output is removed
 *   wl_seat, wl_pointer, wl_keyboard,
 *   wl_touch, wl_data_* .................... seat.c internal
 *   xdg_*, zxdg_* .......................... xdg.c internal
 * A typed 'o' request argument is resolved by the server to a
 * struct lm_resource * whose iface was verified to equal the one in the
 * message's `types[]`; handlers may then trust res->data's type per this table.
 * ---------------------------------------------------------------------------
 */

#include <stdint.h>
#include "wire.h"

struct lm_interface;

struct lm_message {
    const char *name;
    const char *signature;                 /* see wire.h */
    uint32_t since;                        /* interface version that introduced it */
    /*
     * One entry per argument (letters only, '?' not counted): the interface
     * expected for 'o'/'n' args, NULL for every other arg and for untyped
     * object args. May itself be NULL when no argument needs a type.
     */
    const struct lm_interface *const *types;
};

struct lm_interface {
    const char *name;
    uint32_t version;                      /* highest version we implement */
    int n_requests;
    const struct lm_message *requests;     /* indexed by opcode */
    int n_events;
    const struct lm_message *events;
};

/* ---- interfaces ------------------------------------------------------------ */

extern const struct lm_interface lm_wl_display_interface;
extern const struct lm_interface lm_wl_registry_interface;
extern const struct lm_interface lm_wl_callback_interface;
extern const struct lm_interface lm_wl_compositor_interface;
extern const struct lm_interface lm_wl_shm_pool_interface;
extern const struct lm_interface lm_wl_shm_interface;
extern const struct lm_interface lm_wl_buffer_interface;
extern const struct lm_interface lm_wl_data_offer_interface;
extern const struct lm_interface lm_wl_data_source_interface;
extern const struct lm_interface lm_wl_data_device_interface;
extern const struct lm_interface lm_wl_data_device_manager_interface;
extern const struct lm_interface lm_wl_surface_interface;
extern const struct lm_interface lm_wl_seat_interface;
extern const struct lm_interface lm_wl_pointer_interface;
extern const struct lm_interface lm_wl_keyboard_interface;
extern const struct lm_interface lm_wl_touch_interface;
extern const struct lm_interface lm_wl_output_interface;
extern const struct lm_interface lm_wl_region_interface;
extern const struct lm_interface lm_wl_subcompositor_interface;
extern const struct lm_interface lm_wl_subsurface_interface;
extern const struct lm_interface lm_xdg_wm_base_interface;
extern const struct lm_interface lm_xdg_positioner_interface;
extern const struct lm_interface lm_xdg_surface_interface;
extern const struct lm_interface lm_xdg_toplevel_interface;
extern const struct lm_interface lm_xdg_popup_interface;
extern const struct lm_interface lm_zxdg_decoration_manager_v1_interface;
extern const struct lm_interface lm_zxdg_toplevel_decoration_v1_interface;
extern const struct lm_interface lm_zwp_idle_inhibit_manager_v1_interface;   /* idleinhibit.c */
extern const struct lm_interface lm_zwp_idle_inhibitor_v1_interface;
extern const struct lm_interface lm_zwp_linux_dmabuf_v1_interface;     /* dmabuf.c */
extern const struct lm_interface lm_zwp_linux_buffer_params_v1_interface;
extern const struct lm_interface lm_zwp_linux_dmabuf_feedback_v1_interface;
extern const struct lm_interface lm_mellagui_settings_v1_interface;     /* MellaGUI private: settings.c */
extern const struct lm_interface lm_mellagui_capture_v1_interface;      /* MellaGUI private: capture.c */
extern const struct lm_interface lm_mellagui_menu_v1_interface;         /* MellaGUI private: menu.c */
extern const struct lm_interface lm_mellagui_tray_v1_interface;         /* MellaGUI private: tray.c */
extern const struct lm_interface lm_mellagui_tray_item_v1_interface;

/*
 * ---- COPY-PASTE LIST OF ADVERTISED GLOBALS (interface, version) -------------
 *   wl_compositor                 4    created by surface.c
 *   wl_subcompositor              1    surface.c
 *   wl_shm                        1    shm.c      (formats ARGB8888, XRGB8888)
 *   wl_output                     3    output.c   (one per output)
 *   wl_seat                       5    seat.c
 *   wl_data_device_manager        3    seat.c
 *   xdg_wm_base                   3    xdg.c
 *   zxdg_decoration_manager_v1    1    xdg.c      (always answers server_side)
 *   zwp_linux_dmabuf_v1           4    dmabuf.c   (linear ARGB8888 / XRGB8888 only; the pixels are read with mmap)
 *   zwp_idle_inhibit_manager_v1   1    idleinhibit.c (a visible surface keeps the screensaver and the auto lock away)
 *   mellagui_settings_v1             1    settings.c (MellaGUI private: read/change the user settings)
 *   mellagui_capture_v1              1    capture.c  (MellaGUI private: the screen into a wl_shm buffer, for mellagui-record)
 *   mellagui_tray_v1                 1    tray.c     (MellaGUI private: icons / buttons of programs in the top bar)
 *   mellagui_menu_v1                 1    menu.c     (MellaGUI private: a menu at the pointer, e.g. the actions of a text field)
 */
#define LM_WL_COMPOSITOR_VERSION            4
#define LM_WL_SUBCOMPOSITOR_VERSION         1
#define LM_WL_SHM_VERSION                   1
#define LM_WL_OUTPUT_VERSION                3
#define LM_WL_SEAT_VERSION                  5
#define LM_WL_DATA_DEVICE_MANAGER_VERSION   3
#define LM_XDG_WM_BASE_VERSION              3
#define LM_ZXDG_DECORATION_MANAGER_V1_VERSION 1
#define LM_ZWP_LINUX_DMABUF_V1_VERSION      4
#define LM_ZWP_IDLE_INHIBIT_MANAGER_V1_VERSION 1
#define LM_MELLAGUI_SETTINGS_V1_VERSION        1
#define LM_MELLAGUI_CAPTURE_V1_VERSION         1
#define LM_MELLAGUI_TRAY_V1_VERSION            1
#define LM_MELLAGUI_MENU_V1_VERSION            1

/* ---- wl_display (version 1) ------------------------------------------------- */
enum {
    LM_WL_DISPLAY_REQ_SYNC = 0,         /* "n"  callback:new_id(wl_callback) */
    LM_WL_DISPLAY_REQ_GET_REGISTRY = 1, /* "n"  registry:new_id(wl_registry) */
    LM_WL_DISPLAY_EVT_ERROR = 0,        /* "ous" object_id, code, message */
    LM_WL_DISPLAY_EVT_DELETE_ID = 1,    /* "u" */
};
enum {
    LM_WL_DISPLAY_ERR_INVALID_OBJECT = 0,
    LM_WL_DISPLAY_ERR_INVALID_METHOD = 1,
    LM_WL_DISPLAY_ERR_NO_MEMORY = 2,
    LM_WL_DISPLAY_ERR_IMPLEMENTATION = 3,
};

/* ---- wl_registry ------------------------------------------------------------- */
enum {
    LM_WL_REGISTRY_REQ_BIND = 0,        /* "usun" name, interface, version, id (see wire.h) */
    LM_WL_REGISTRY_EVT_GLOBAL = 0,      /* "usu" name, interface, version */
    LM_WL_REGISTRY_EVT_GLOBAL_REMOVE = 1, /* "u" */
};

/* ---- wl_callback --------------------------------------------------------------- */
enum {
    LM_WL_CALLBACK_EVT_DONE = 0,        /* "u" callback_data */
};

/* ---- wl_compositor (v4) -------------------------------------------------------- */
enum {
    LM_WL_COMPOSITOR_REQ_CREATE_SURFACE = 0, /* "n" id(wl_surface) */
    LM_WL_COMPOSITOR_REQ_CREATE_REGION = 1,  /* "n" id(wl_region) */
};

/* ---- wl_shm_pool --------------------------------------------------------------- */
enum {
    LM_WL_SHM_POOL_REQ_CREATE_BUFFER = 0, /* "niiiiu" id(wl_buffer), offset, width, height, stride, format */
    LM_WL_SHM_POOL_REQ_DESTROY = 1,       /* "" */
    LM_WL_SHM_POOL_REQ_RESIZE = 2,        /* "i" size */
};

/* ---- wl_shm (v1) --------------------------------------------------------------- */
enum {
    LM_WL_SHM_REQ_CREATE_POOL = 0,      /* "nhi" id(wl_shm_pool), fd, size */
    LM_WL_SHM_EVT_FORMAT = 0,           /* "u" */
};
enum {
    LM_WL_SHM_ERR_INVALID_FORMAT = 0,
    LM_WL_SHM_ERR_INVALID_STRIDE = 1,
    LM_WL_SHM_ERR_INVALID_FD = 2,
};
enum {
    LM_WL_SHM_FORMAT_ARGB8888 = 0,
    LM_WL_SHM_FORMAT_XRGB8888 = 1,
};

/* ---- wl_buffer ----------------------------------------------------------------- */
enum {
    LM_WL_BUFFER_REQ_DESTROY = 0,       /* "" */
    LM_WL_BUFFER_EVT_RELEASE = 0,       /* "" */
};

/* ---- wl_data_offer (v3) -------------------------------------------------------- */
enum {
    LM_WL_DATA_OFFER_REQ_ACCEPT = 0,      /* "u?s" serial, mime_type */
    LM_WL_DATA_OFFER_REQ_RECEIVE = 1,     /* "sh" mime_type, fd */
    LM_WL_DATA_OFFER_REQ_DESTROY = 2,     /* "" */
    LM_WL_DATA_OFFER_REQ_FINISH = 3,      /* "" since 3 */
    LM_WL_DATA_OFFER_REQ_SET_ACTIONS = 4, /* "uu" dnd_actions, preferred_action; since 3 */
    LM_WL_DATA_OFFER_EVT_OFFER = 0,       /* "s" mime_type */
    LM_WL_DATA_OFFER_EVT_SOURCE_ACTIONS = 1, /* "u" since 3 */
    LM_WL_DATA_OFFER_EVT_ACTION = 2,      /* "u" since 3 */
};

/* ---- wl_data_source (v3) ------------------------------------------------------- */
enum {
    LM_WL_DATA_SOURCE_REQ_OFFER = 0,      /* "s" mime_type */
    LM_WL_DATA_SOURCE_REQ_DESTROY = 1,    /* "" */
    LM_WL_DATA_SOURCE_REQ_SET_ACTIONS = 2, /* "u" since 3 */
    LM_WL_DATA_SOURCE_EVT_TARGET = 0,     /* "?s" */
    LM_WL_DATA_SOURCE_EVT_SEND = 1,       /* "sh" mime_type, fd */
    LM_WL_DATA_SOURCE_EVT_CANCELLED = 2,  /* "" */
    LM_WL_DATA_SOURCE_EVT_DND_DROP_PERFORMED = 3, /* "" since 3 */
    LM_WL_DATA_SOURCE_EVT_DND_FINISHED = 4,       /* "" since 3 */
    LM_WL_DATA_SOURCE_EVT_ACTION = 5,     /* "u" since 3 */
};

/* ---- wl_data_device (v3) ------------------------------------------------------- */
enum {
    LM_WL_DATA_DEVICE_REQ_START_DRAG = 0,    /* "?oo?ou" source, origin, icon, serial */
    LM_WL_DATA_DEVICE_REQ_SET_SELECTION = 1, /* "?ou" source, serial */
    LM_WL_DATA_DEVICE_REQ_RELEASE = 2,       /* "" since 2 */
    LM_WL_DATA_DEVICE_EVT_DATA_OFFER = 0,    /* "n" id(wl_data_offer) */
    LM_WL_DATA_DEVICE_EVT_ENTER = 1,         /* "uoff?o" serial, surface, x, y, id */
    LM_WL_DATA_DEVICE_EVT_LEAVE = 2,         /* "" */
    LM_WL_DATA_DEVICE_EVT_MOTION = 3,        /* "uff" time, x, y */
    LM_WL_DATA_DEVICE_EVT_DROP = 4,          /* "" */
    LM_WL_DATA_DEVICE_EVT_SELECTION = 5,     /* "?o" id */
};
enum { LM_WL_DATA_DEVICE_ERR_ROLE = 0 };
enum {
    LM_WL_DATA_OFFER_ERR_INVALID_FINISH = 0,
    LM_WL_DATA_OFFER_ERR_INVALID_ACTION_MASK = 1,
    LM_WL_DATA_OFFER_ERR_INVALID_ACTION = 2,
    LM_WL_DATA_OFFER_ERR_INVALID_OFFER = 3,
};
enum {
    LM_WL_DATA_SOURCE_ERR_INVALID_ACTION_MASK = 0,
    LM_WL_DATA_SOURCE_ERR_INVALID_SOURCE = 1,
};
enum {
    LM_DND_ACTION_NONE = 0,
    LM_DND_ACTION_COPY = 1,
    LM_DND_ACTION_MOVE = 2,
    LM_DND_ACTION_ASK = 4,
};

/* ---- wl_data_device_manager (v3) ----------------------------------------------- */
enum {
    LM_WL_DATA_DEVICE_MANAGER_REQ_CREATE_DATA_SOURCE = 0, /* "n" */
    LM_WL_DATA_DEVICE_MANAGER_REQ_GET_DATA_DEVICE = 1,    /* "no" id, seat(wl_seat) */
};

/* ---- wl_surface (v4) ----------------------------------------------------------- */
enum {
    LM_WL_SURFACE_REQ_DESTROY = 0,            /* "" */
    LM_WL_SURFACE_REQ_ATTACH = 1,             /* "?oii" buffer(wl_buffer), x, y */
    LM_WL_SURFACE_REQ_DAMAGE = 2,             /* "iiii" */
    LM_WL_SURFACE_REQ_FRAME = 3,              /* "n" callback(wl_callback) */
    LM_WL_SURFACE_REQ_SET_OPAQUE_REGION = 4,  /* "?o" region(wl_region) */
    LM_WL_SURFACE_REQ_SET_INPUT_REGION = 5,   /* "?o" region(wl_region) */
    LM_WL_SURFACE_REQ_COMMIT = 6,             /* "" */
    LM_WL_SURFACE_REQ_SET_BUFFER_TRANSFORM = 7, /* "i" since 2 */
    LM_WL_SURFACE_REQ_SET_BUFFER_SCALE = 8,   /* "i" since 3 */
    LM_WL_SURFACE_REQ_DAMAGE_BUFFER = 9,      /* "iiii" since 4 */
    LM_WL_SURFACE_EVT_ENTER = 0,              /* "o" output(wl_output) */
    LM_WL_SURFACE_EVT_LEAVE = 1,              /* "o" */
};
enum {
    LM_WL_SURFACE_ERR_INVALID_SCALE = 0,
    LM_WL_SURFACE_ERR_INVALID_TRANSFORM = 1,
    LM_WL_SURFACE_ERR_INVALID_SIZE = 2,
    LM_WL_SURFACE_ERR_INVALID_OFFSET = 3,
};

/* ---- wl_seat (v5) -------------------------------------------------------------- */
enum {
    LM_WL_SEAT_REQ_GET_POINTER = 0,    /* "n" */
    LM_WL_SEAT_REQ_GET_KEYBOARD = 1,   /* "n" */
    LM_WL_SEAT_REQ_GET_TOUCH = 2,      /* "n" */
    LM_WL_SEAT_REQ_RELEASE = 3,        /* "" since 5 */
    LM_WL_SEAT_EVT_CAPABILITIES = 0,   /* "u" */
    LM_WL_SEAT_EVT_NAME = 1,           /* "s" since 2 */
};
enum {
    LM_SEAT_CAP_POINTER = 1,
    LM_SEAT_CAP_KEYBOARD = 2,
    LM_SEAT_CAP_TOUCH = 4,
};

/* ---- wl_pointer (v5) ----------------------------------------------------------- */
enum {
    LM_WL_POINTER_REQ_SET_CURSOR = 0,  /* "u?oii" serial, surface, hotspot_x, hotspot_y */
    LM_WL_POINTER_REQ_RELEASE = 1,     /* "" since 3 */
    LM_WL_POINTER_EVT_ENTER = 0,       /* "uoff" serial, surface, x, y */
    LM_WL_POINTER_EVT_LEAVE = 1,       /* "uo" serial, surface */
    LM_WL_POINTER_EVT_MOTION = 2,      /* "uff" time, x, y */
    LM_WL_POINTER_EVT_BUTTON = 3,      /* "uuuu" serial, time, button, state */
    LM_WL_POINTER_EVT_AXIS = 4,        /* "uuf" time, axis, value */
    LM_WL_POINTER_EVT_FRAME = 5,       /* "" since 5 */
    LM_WL_POINTER_EVT_AXIS_SOURCE = 6, /* "u" since 5 */
    LM_WL_POINTER_EVT_AXIS_STOP = 7,   /* "uu" time, axis; since 5 */
    LM_WL_POINTER_EVT_AXIS_DISCRETE = 8, /* "ui" axis, discrete; since 5 */
};
enum { LM_WL_POINTER_ERR_ROLE = 0 };
enum { LM_POINTER_BUTTON_RELEASED = 0, LM_POINTER_BUTTON_PRESSED = 1 };
enum { LM_POINTER_AXIS_VERTICAL = 0, LM_POINTER_AXIS_HORIZONTAL = 1 };
enum {
    LM_POINTER_AXIS_SOURCE_WHEEL = 0,
    LM_POINTER_AXIS_SOURCE_FINGER = 1,
    LM_POINTER_AXIS_SOURCE_CONTINUOUS = 2,
    LM_POINTER_AXIS_SOURCE_WHEEL_TILT = 3,
};

/* ---- wl_keyboard (v5) ---------------------------------------------------------- */
enum {
    LM_WL_KEYBOARD_REQ_RELEASE = 0,    /* "" since 3 */
    LM_WL_KEYBOARD_EVT_KEYMAP = 0,     /* "uhu" format, fd, size */
    LM_WL_KEYBOARD_EVT_ENTER = 1,      /* "uoa" serial, surface, keys(array of u32) */
    LM_WL_KEYBOARD_EVT_LEAVE = 2,      /* "uo" serial, surface */
    LM_WL_KEYBOARD_EVT_KEY = 3,        /* "uuuu" serial, time, key, state */
    LM_WL_KEYBOARD_EVT_MODIFIERS = 4,  /* "uuuuu" serial, depressed, latched, locked, group */
    LM_WL_KEYBOARD_EVT_REPEAT_INFO = 5, /* "ii" rate, delay; since 4 */
};
enum { LM_KEYBOARD_KEYMAP_NONE = 0, LM_KEYBOARD_KEYMAP_XKB_V1 = 1 };
enum { LM_KEYBOARD_KEY_RELEASED = 0, LM_KEYBOARD_KEY_PRESSED = 1 };

/* ---- wl_touch (v5) ------------------------------------------------------------- */
enum {
    LM_WL_TOUCH_REQ_RELEASE = 0,       /* "" since 3 */
    LM_WL_TOUCH_EVT_DOWN = 0,          /* "uuoiff" serial, time, surface, id, x, y */
    LM_WL_TOUCH_EVT_UP = 1,            /* "uui" serial, time, id */
    LM_WL_TOUCH_EVT_MOTION = 2,        /* "uiff" time, id, x, y */
    LM_WL_TOUCH_EVT_FRAME = 3,         /* "" */
    LM_WL_TOUCH_EVT_CANCEL = 4,        /* "" */
};

/* ---- wl_output (v3) ------------------------------------------------------------ */
enum {
    LM_WL_OUTPUT_REQ_RELEASE = 0,      /* "" since 3 */
    LM_WL_OUTPUT_EVT_GEOMETRY = 0,     /* "iiiiissi" x, y, phys_w, phys_h, subpixel, make, model, transform */
    LM_WL_OUTPUT_EVT_MODE = 1,         /* "uiii" flags, width, height, refresh(mHz) */
    LM_WL_OUTPUT_EVT_DONE = 2,         /* "" since 2 */
    LM_WL_OUTPUT_EVT_SCALE = 3,        /* "i" since 2 */
};
enum { LM_OUTPUT_MODE_CURRENT = 1, LM_OUTPUT_MODE_PREFERRED = 2 };
enum { LM_OUTPUT_SUBPIXEL_UNKNOWN = 0, LM_OUTPUT_SUBPIXEL_NONE = 1 };
/* wl_output.transform / wl_surface.set_buffer_transform values */
enum {
    LM_TRANSFORM_NORMAL = 0,
    LM_TRANSFORM_90 = 1,
    LM_TRANSFORM_180 = 2,
    LM_TRANSFORM_270 = 3,
    LM_TRANSFORM_FLIPPED = 4,
    LM_TRANSFORM_FLIPPED_90 = 5,
    LM_TRANSFORM_FLIPPED_180 = 6,
    LM_TRANSFORM_FLIPPED_270 = 7,
};

/* ---- wl_region ----------------------------------------------------------------- */
enum {
    LM_WL_REGION_REQ_DESTROY = 0,      /* "" */
    LM_WL_REGION_REQ_ADD = 1,          /* "iiii" */
    LM_WL_REGION_REQ_SUBTRACT = 2,     /* "iiii" */
};

/* ---- wl_subcompositor / wl_subsurface ------------------------------------------ */
enum {
    LM_WL_SUBCOMPOSITOR_REQ_DESTROY = 0,        /* "" */
    LM_WL_SUBCOMPOSITOR_REQ_GET_SUBSURFACE = 1, /* "noo" id, surface, parent */
};
enum { LM_WL_SUBCOMPOSITOR_ERR_BAD_SURFACE = 0 };
enum {
    LM_WL_SUBSURFACE_REQ_DESTROY = 0,      /* "" */
    LM_WL_SUBSURFACE_REQ_SET_POSITION = 1, /* "ii" */
    LM_WL_SUBSURFACE_REQ_PLACE_ABOVE = 2,  /* "o" sibling(wl_surface) */
    LM_WL_SUBSURFACE_REQ_PLACE_BELOW = 3,  /* "o" sibling(wl_surface) */
    LM_WL_SUBSURFACE_REQ_SET_SYNC = 4,     /* "" */
    LM_WL_SUBSURFACE_REQ_SET_DESYNC = 5,   /* "" */
};
enum { LM_WL_SUBSURFACE_ERR_BAD_SURFACE = 0 };

/* ---- xdg_wm_base (v3) ---------------------------------------------------------- */
enum {
    LM_XDG_WM_BASE_REQ_DESTROY = 0,           /* "" */
    LM_XDG_WM_BASE_REQ_CREATE_POSITIONER = 1, /* "n" id(xdg_positioner) */
    LM_XDG_WM_BASE_REQ_GET_XDG_SURFACE = 2,   /* "no" id(xdg_surface), surface(wl_surface) */
    LM_XDG_WM_BASE_REQ_PONG = 3,              /* "u" serial */
    LM_XDG_WM_BASE_EVT_PING = 0,              /* "u" serial */
};
enum {
    LM_XDG_WM_BASE_ERR_ROLE = 0,
    LM_XDG_WM_BASE_ERR_DEFUNCT_SURFACES = 1,
    LM_XDG_WM_BASE_ERR_NOT_THE_TOPMOST_POPUP = 2,
    LM_XDG_WM_BASE_ERR_INVALID_POPUP_PARENT = 3,
    LM_XDG_WM_BASE_ERR_INVALID_SURFACE_STATE = 4,
    LM_XDG_WM_BASE_ERR_INVALID_POSITIONER = 5,
};

/* ---- xdg_positioner ------------------------------------------------------------ */
enum {
    LM_XDG_POSITIONER_REQ_DESTROY = 0,                  /* "" */
    LM_XDG_POSITIONER_REQ_SET_SIZE = 1,                 /* "ii" */
    LM_XDG_POSITIONER_REQ_SET_ANCHOR_RECT = 2,          /* "iiii" */
    LM_XDG_POSITIONER_REQ_SET_ANCHOR = 3,               /* "u" */
    LM_XDG_POSITIONER_REQ_SET_GRAVITY = 4,              /* "u" */
    LM_XDG_POSITIONER_REQ_SET_CONSTRAINT_ADJUSTMENT = 5, /* "u" */
    LM_XDG_POSITIONER_REQ_SET_OFFSET = 6,               /* "ii" */
    LM_XDG_POSITIONER_REQ_SET_REACTIVE = 7,             /* "" since 3 */
    LM_XDG_POSITIONER_REQ_SET_PARENT_SIZE = 8,          /* "ii" since 3 */
    LM_XDG_POSITIONER_REQ_SET_PARENT_CONFIGURE = 9,     /* "u" since 3 */
};
enum { LM_XDG_POSITIONER_ERR_INVALID_INPUT = 0 };
enum {
    LM_XDG_ANCHOR_NONE = 0,
    LM_XDG_ANCHOR_TOP = 1,
    LM_XDG_ANCHOR_BOTTOM = 2,
    LM_XDG_ANCHOR_LEFT = 3,
    LM_XDG_ANCHOR_RIGHT = 4,
    LM_XDG_ANCHOR_TOP_LEFT = 5,
    LM_XDG_ANCHOR_BOTTOM_LEFT = 6,
    LM_XDG_ANCHOR_TOP_RIGHT = 7,
    LM_XDG_ANCHOR_BOTTOM_RIGHT = 8,
};
/* xdg_positioner.gravity uses the same numeric values as anchor. */
enum {
    LM_XDG_CA_NONE = 0,
    LM_XDG_CA_SLIDE_X = 1,
    LM_XDG_CA_SLIDE_Y = 2,
    LM_XDG_CA_FLIP_X = 4,
    LM_XDG_CA_FLIP_Y = 8,
    LM_XDG_CA_RESIZE_X = 16,
    LM_XDG_CA_RESIZE_Y = 32,
};

/* ---- xdg_surface --------------------------------------------------------------- */
enum {
    LM_XDG_SURFACE_REQ_DESTROY = 0,             /* "" */
    LM_XDG_SURFACE_REQ_GET_TOPLEVEL = 1,        /* "n" id(xdg_toplevel) */
    LM_XDG_SURFACE_REQ_GET_POPUP = 2,           /* "n?oo" id(xdg_popup), parent(xdg_surface), positioner */
    LM_XDG_SURFACE_REQ_SET_WINDOW_GEOMETRY = 3, /* "iiii" */
    LM_XDG_SURFACE_REQ_ACK_CONFIGURE = 4,       /* "u" serial */
    LM_XDG_SURFACE_EVT_CONFIGURE = 0,           /* "u" serial */
};
enum {
    LM_XDG_SURFACE_ERR_NOT_CONSTRUCTED = 1,
    LM_XDG_SURFACE_ERR_ALREADY_CONSTRUCTED = 2,
    LM_XDG_SURFACE_ERR_UNCONFIGURED_BUFFER = 3,
    LM_XDG_SURFACE_ERR_INVALID_SERIAL = 4,
    LM_XDG_SURFACE_ERR_INVALID_SIZE = 5,
    LM_XDG_SURFACE_ERR_DEFUNCT_ROLE_OBJECT = 6,
};

/* ---- xdg_toplevel (we implement the v3 subset; configure_bounds etc. are v4+) --- */
enum {
    LM_XDG_TOPLEVEL_REQ_DESTROY = 0,           /* "" */
    LM_XDG_TOPLEVEL_REQ_SET_PARENT = 1,        /* "?o" parent(xdg_toplevel) */
    LM_XDG_TOPLEVEL_REQ_SET_TITLE = 2,         /* "s" */
    LM_XDG_TOPLEVEL_REQ_SET_APP_ID = 3,        /* "s" */
    LM_XDG_TOPLEVEL_REQ_SHOW_WINDOW_MENU = 4,  /* "ouii" seat, serial, x, y */
    LM_XDG_TOPLEVEL_REQ_MOVE = 5,              /* "ou" seat, serial */
    LM_XDG_TOPLEVEL_REQ_RESIZE = 6,            /* "ouu" seat, serial, edges */
    LM_XDG_TOPLEVEL_REQ_SET_MAX_SIZE = 7,      /* "ii" */
    LM_XDG_TOPLEVEL_REQ_SET_MIN_SIZE = 8,      /* "ii" */
    LM_XDG_TOPLEVEL_REQ_SET_MAXIMIZED = 9,     /* "" */
    LM_XDG_TOPLEVEL_REQ_UNSET_MAXIMIZED = 10,  /* "" */
    LM_XDG_TOPLEVEL_REQ_SET_FULLSCREEN = 11,   /* "?o" output(wl_output) */
    LM_XDG_TOPLEVEL_REQ_UNSET_FULLSCREEN = 12, /* "" */
    LM_XDG_TOPLEVEL_REQ_SET_MINIMIZED = 13,    /* "" */
    LM_XDG_TOPLEVEL_EVT_CONFIGURE = 0,         /* "iia" width, height, states(array of u32) */
    LM_XDG_TOPLEVEL_EVT_CLOSE = 1,             /* "" */
};
enum {
    LM_XDG_TOPLEVEL_ERR_INVALID_RESIZE_EDGE = 0,
    LM_XDG_TOPLEVEL_ERR_INVALID_PARENT = 1,
    LM_XDG_TOPLEVEL_ERR_INVALID_SIZE = 2,
};
/* xdg_toplevel.state values placed in the configure `states` array */
enum {
    LM_XDG_TOPLEVEL_STATE_MAXIMIZED = 1,
    LM_XDG_TOPLEVEL_STATE_FULLSCREEN = 2,
    LM_XDG_TOPLEVEL_STATE_RESIZING = 3,
    LM_XDG_TOPLEVEL_STATE_ACTIVATED = 4,
};

/* ---- xdg_popup (v3) ------------------------------------------------------------ */
enum {
    LM_XDG_POPUP_REQ_DESTROY = 0,      /* "" */
    LM_XDG_POPUP_REQ_GRAB = 1,         /* "ou" seat, serial */
    LM_XDG_POPUP_REQ_REPOSITION = 2,   /* "ou" positioner(xdg_positioner), token; since 3 */
    LM_XDG_POPUP_EVT_CONFIGURE = 0,    /* "iiii" x, y, width, height */
    LM_XDG_POPUP_EVT_POPUP_DONE = 1,   /* "" */
    LM_XDG_POPUP_EVT_REPOSITIONED = 2, /* "u" token */
};
enum { LM_XDG_POPUP_ERR_INVALID_GRAB = 0 };

/* ---- zwp_idle_inhibit_manager_v1 / zwp_idle_inhibitor_v1 ----------------------- */
enum {
    LM_ZWP_IDLE_INHIBIT_MANAGER_V1_REQ_DESTROY = 0,           /* "" */
    LM_ZWP_IDLE_INHIBIT_MANAGER_V1_REQ_CREATE_INHIBITOR = 1,  /* "no" id, surface(wl_surface) */
    LM_ZWP_IDLE_INHIBITOR_V1_REQ_DESTROY = 0,                 /* "" */
};

/* ---- zxdg_decoration_manager_v1 / zxdg_toplevel_decoration_v1 ------------------ */
enum {
    LM_ZXDG_DECORATION_MANAGER_V1_REQ_DESTROY = 0,               /* "" */
    LM_ZXDG_DECORATION_MANAGER_V1_REQ_GET_TOPLEVEL_DECORATION = 1, /* "no" id, toplevel(xdg_toplevel) */
    LM_ZXDG_TOPLEVEL_DECORATION_V1_REQ_DESTROY = 0,   /* "" */
    LM_ZXDG_TOPLEVEL_DECORATION_V1_REQ_SET_MODE = 1,  /* "u" */
    LM_ZXDG_TOPLEVEL_DECORATION_V1_REQ_UNSET_MODE = 2, /* "" */
    LM_ZXDG_TOPLEVEL_DECORATION_V1_EVT_CONFIGURE = 0, /* "u" mode */
};
enum {
    LM_ZXDG_DECORATION_MODE_CLIENT_SIDE = 1,
    LM_ZXDG_DECORATION_MODE_SERVER_SIDE = 2,
};
enum {
    LM_ZXDG_DECORATION_ERR_UNCONFIGURED_BUFFER = 0,
    LM_ZXDG_DECORATION_ERR_ALREADY_CONSTRUCTED = 1,
    LM_ZXDG_DECORATION_ERR_ORPHANED = 2,
};

/* ---- zwp_linux_dmabuf_v1 (linux-dmabuf-unstable-v1, version 4) ---------------------------------------------- */
enum {
    LM_ZWP_LINUX_DMABUF_V1_REQ_DESTROY = 0,             /* "" */
    LM_ZWP_LINUX_DMABUF_V1_REQ_CREATE_PARAMS = 1,       /* "n" params(zwp_linux_buffer_params_v1) */
    LM_ZWP_LINUX_DMABUF_V1_REQ_GET_DEFAULT_FEEDBACK = 2,/* "n" feedback (since 4) */
    LM_ZWP_LINUX_DMABUF_V1_REQ_GET_SURFACE_FEEDBACK = 3,/* "no" feedback, surface (since 4) */
    LM_ZWP_LINUX_DMABUF_V1_EVT_FORMAT = 0,              /* "u" fourcc (versions 1..3; deprecated) */
    LM_ZWP_LINUX_DMABUF_V1_EVT_MODIFIER = 1,            /* "uuu" fourcc, modifier hi, lo (since 3; versions 3 only) */
};
enum {
    LM_ZWP_LINUX_BUFFER_PARAMS_V1_REQ_DESTROY = 0,      /* "" */
    LM_ZWP_LINUX_BUFFER_PARAMS_V1_REQ_ADD = 1,          /* "huuuuu" fd, plane, offset, stride, modifier hi, lo */
    LM_ZWP_LINUX_BUFFER_PARAMS_V1_REQ_CREATE = 2,       /* "iiuu" width, height, format, flags (answer: created / failed) */
    LM_ZWP_LINUX_BUFFER_PARAMS_V1_REQ_CREATE_IMMED = 3, /* "niiuu" buffer(wl_buffer), width, height, format, flags (since 2) */
    LM_ZWP_LINUX_BUFFER_PARAMS_V1_EVT_CREATED = 0,      /* "n" buffer */
    LM_ZWP_LINUX_BUFFER_PARAMS_V1_EVT_FAILED = 1,       /* "" */
};
enum {
    LM_ZWP_LINUX_BUFFER_PARAMS_ERR_ALREADY_USED = 0,
    LM_ZWP_LINUX_BUFFER_PARAMS_ERR_PLANE_IDX = 1,
    LM_ZWP_LINUX_BUFFER_PARAMS_ERR_PLANE_SET = 2,
    LM_ZWP_LINUX_BUFFER_PARAMS_ERR_INCOMPLETE = 3,
    LM_ZWP_LINUX_BUFFER_PARAMS_ERR_INVALID_FORMAT = 4,
    LM_ZWP_LINUX_BUFFER_PARAMS_ERR_INVALID_DIMENSIONS = 5,
    LM_ZWP_LINUX_BUFFER_PARAMS_ERR_OUT_OF_BOUNDS = 6,
    LM_ZWP_LINUX_BUFFER_PARAMS_ERR_INVALID_WL_BUFFER = 7,
};
enum {
    LM_ZWP_LINUX_DMABUF_FEEDBACK_V1_REQ_DESTROY = 0,    /* "" */
    LM_ZWP_LINUX_DMABUF_FEEDBACK_V1_EVT_DONE = 0,       /* "" */
    LM_ZWP_LINUX_DMABUF_FEEDBACK_V1_EVT_FORMAT_TABLE = 1,   /* "hu" fd, size: array of { u32 fourcc, u32 pad, u64 modifier } */
    LM_ZWP_LINUX_DMABUF_FEEDBACK_V1_EVT_MAIN_DEVICE = 2,    /* "a" dev_t */
    LM_ZWP_LINUX_DMABUF_FEEDBACK_V1_EVT_TRANCHE_DONE = 3,   /* "" */
    LM_ZWP_LINUX_DMABUF_FEEDBACK_V1_EVT_TRANCHE_TARGET_DEVICE = 4,  /* "a" dev_t */
    LM_ZWP_LINUX_DMABUF_FEEDBACK_V1_EVT_TRANCHE_FORMATS = 5,        /* "a" array of u16 indices into the format table */
    LM_ZWP_LINUX_DMABUF_FEEDBACK_V1_EVT_TRANCHE_FLAGS = 6,          /* "u" */
};
/* DRM fourcc codes (drm_fourcc.h) */
#define LM_DRM_FORMAT_ARGB8888 0x34325241u      /* 'A' 'R' '2' '4' */
#define LM_DRM_FORMAT_XRGB8888 0x34325258u      /* 'X' 'R' '2' '4' */
#define LM_DRM_FORMAT_MOD_LINEAR 0ull

/* ---- mellagui_settings_v1 (MellaGUI's own protocol, see settings.h for the keys) ----------------------------------- */
enum {
    LM_MELLAGUI_SETTINGS_V1_REQ_DESTROY = 0,   /* "" */
    LM_MELLAGUI_SETTINGS_V1_REQ_SET = 1,       /* "ss" key, value: change one setting (the answer is a value burst + done) */
    LM_MELLAGUI_SETTINGS_V1_EVT_VALUE = 0,     /* "ss" key, value: current value of one setting */
    LM_MELLAGUI_SETTINGS_V1_EVT_DONE = 1,      /* "" end of a burst of value events */
};

/* ---- mellagui_capture_v1 (see capture.h) --------------------------------------------------------------------------- */
enum {
    LM_MELLAGUI_CAPTURE_V1_REQ_DESTROY = 0,    /* "" */
    LM_MELLAGUI_CAPTURE_V1_REQ_LIST = 1,       /* "" the answer is one `output` event per output, then `done` */
    LM_MELLAGUI_CAPTURE_V1_REQ_GRAB = 2,       /* "io" output index, wl_shm buffer of exactly the output's size: paint the output into it */
    LM_MELLAGUI_CAPTURE_V1_EVT_OUTPUT = 0,     /* "isiiii" index, name, x, y, width, height */
    LM_MELLAGUI_CAPTURE_V1_EVT_DONE = 1,       /* "" */
    LM_MELLAGUI_CAPTURE_V1_EVT_READY = 2,      /* "iii" output index, width, height: the buffer holds the picture (XRGB8888 content) */
    LM_MELLAGUI_CAPTURE_V1_EVT_FAILED = 3,     /* "is" output index, reason: "no-output" "buffer" "size" "locked" "capture" */
};

/* ---- mellagui_menu_v1 (see menu.h) --------------------------------------------------------------------------------- */
enum {
    LM_MELLAGUI_MENU_V1_REQ_DESTROY = 0,       /* "" */
    LM_MELLAGUI_MENU_V1_REQ_POPUP = 1,         /* "s" the entries, one per line (a leading '~' = dim, cannot be chosen) */
    LM_MELLAGUI_MENU_V1_REQ_WARP = 2,          /* "ii" move the pointer to this place of the surface it is over (surface pixels): see menu.h */
    LM_MELLAGUI_MENU_V1_EVT_CHOSEN = 0,        /* "i" the entry */
    LM_MELLAGUI_MENU_V1_EVT_CLOSED = 1,        /* "" the menu went away without a choice */
};

/* ---- mellagui_tray_v1 / mellagui_tray_item_v1 (see tray.h) -------------------------------------------------------- */
enum {
    LM_MELLAGUI_TRAY_V1_REQ_DESTROY = 0,       /* "" (the items stay) */
    LM_MELLAGUI_TRAY_V1_REQ_CREATE_ITEM = 1,   /* "n" item:new_id(mellagui_tray_item_v1) */
    LM_MELLAGUI_TRAY_ITEM_V1_REQ_DESTROY = 0,  /* "" the item leaves the bar */
    LM_MELLAGUI_TRAY_ITEM_V1_REQ_SET_ICON = 1, /* "?o" wl_shm buffer (ARGB8888 / XRGB8888, at most 64x64; copied at once, then released); null = no icon */
    LM_MELLAGUI_TRAY_ITEM_V1_REQ_SET_TEXT = 2, /* "s" label right of the icon, at most 31 bytes */
    LM_MELLAGUI_TRAY_ITEM_V1_EVT_SIZE = 0,     /* "i" the icon edge in pixels the bar wants (sent when the item is created) */
    LM_MELLAGUI_TRAY_ITEM_V1_EVT_CLICKED = 1,  /* "u" evdev button (272 left, 273 right, 274 middle): pressed and let go on the item */
};

#endif
