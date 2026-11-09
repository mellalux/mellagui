#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "protocol.h"

#include <string.h>

/*
 * Interface descriptors. Signatures and `since` values follow wayland.xml and
 * xdg-shell.xml; the opcode order is the order of the tables below and must
 * match the LM_*_REQ_* / LM_*_EVT_* enums in protocol.h.
 */

#define N_ELEMS(a) (int)(sizeof(a) / sizeof((a)[0]))

/* Per-argument interface types, one entry per argument (NULL = untyped). */
#define TY(...) ((const struct lm_interface *const[]){ __VA_ARGS__ })

#define MSG(n, s, v, ty) { .name = (n), .signature = (s), .since = (v), .types = (ty) }

#define DEFINE_IFACE(var, nm, ver, reqs, evs) \
    const struct lm_interface var = { \
        .name = (nm), .version = (ver), \
        .n_requests = N_ELEMS(reqs), .requests = (reqs), \
        .n_events = N_ELEMS(evs), .events = (evs), \
    }
#define DEFINE_IFACE_REQ(var, nm, ver, reqs) \
    const struct lm_interface var = { \
        .name = (nm), .version = (ver), \
        .n_requests = N_ELEMS(reqs), .requests = (reqs), \
        .n_events = 0, .events = NULL, \
    }
#define DEFINE_IFACE_EVT(var, nm, ver, evs) \
    const struct lm_interface var = { \
        .name = (nm), .version = (ver), \
        .n_requests = 0, .requests = NULL, \
        .n_events = N_ELEMS(evs), .events = (evs), \
    }

/* ---- wl_display ----------------------------------------------------------------- */
static const struct lm_message display_requests[] = {
    MSG("sync", "n", 1, TY(&lm_wl_callback_interface)),
    MSG("get_registry", "n", 1, TY(&lm_wl_registry_interface)),
};
static const struct lm_message display_events[] = {
    MSG("error", "ous", 1, NULL),
    MSG("delete_id", "u", 1, NULL),
};
DEFINE_IFACE(lm_wl_display_interface, "wl_display", 1, display_requests, display_events);

/* ---- wl_registry ---------------------------------------------------------------- */
static const struct lm_message registry_requests[] = {
    MSG("bind", "usun", 1, NULL),
};
static const struct lm_message registry_events[] = {
    MSG("global", "usu", 1, NULL),
    MSG("global_remove", "u", 1, NULL),
};
DEFINE_IFACE(lm_wl_registry_interface, "wl_registry", 1, registry_requests, registry_events);

/* ---- wl_callback ---------------------------------------------------------------- */
static const struct lm_message callback_events[] = {
    MSG("done", "u", 1, NULL),
};
DEFINE_IFACE_EVT(lm_wl_callback_interface, "wl_callback", 1, callback_events);

/* ---- wl_compositor -------------------------------------------------------------- */
static const struct lm_message compositor_requests[] = {
    MSG("create_surface", "n", 1, TY(&lm_wl_surface_interface)),
    MSG("create_region", "n", 1, TY(&lm_wl_region_interface)),
};
DEFINE_IFACE_REQ(lm_wl_compositor_interface, "wl_compositor", 4, compositor_requests);

/* ---- wl_shm_pool ---------------------------------------------------------------- */
static const struct lm_message shm_pool_requests[] = {
    MSG("create_buffer", "niiiiu", 1, TY(&lm_wl_buffer_interface, NULL, NULL, NULL, NULL, NULL)),
    MSG("destroy", "", 1, NULL),
    MSG("resize", "i", 1, NULL),
};
DEFINE_IFACE_REQ(lm_wl_shm_pool_interface, "wl_shm_pool", 1, shm_pool_requests);

/* ---- wl_shm --------------------------------------------------------------------- */
static const struct lm_message shm_requests[] = {
    MSG("create_pool", "nhi", 1, TY(&lm_wl_shm_pool_interface, NULL, NULL)),
};
static const struct lm_message shm_events[] = {
    MSG("format", "u", 1, NULL),
};
DEFINE_IFACE(lm_wl_shm_interface, "wl_shm", 1, shm_requests, shm_events);

/* ---- wl_buffer ------------------------------------------------------------------ */
static const struct lm_message buffer_requests[] = {
    MSG("destroy", "", 1, NULL),
};
static const struct lm_message buffer_events[] = {
    MSG("release", "", 1, NULL),
};
DEFINE_IFACE(lm_wl_buffer_interface, "wl_buffer", 1, buffer_requests, buffer_events);

/* ---- wl_data_offer -------------------------------------------------------------- */
static const struct lm_message data_offer_requests[] = {
    MSG("accept", "u?s", 1, NULL),
    MSG("receive", "sh", 1, NULL),
    MSG("destroy", "", 1, NULL),
    MSG("finish", "", 3, NULL),
    MSG("set_actions", "uu", 3, NULL),
};
static const struct lm_message data_offer_events[] = {
    MSG("offer", "s", 1, NULL),
    MSG("source_actions", "u", 3, NULL),
    MSG("action", "u", 3, NULL),
};
DEFINE_IFACE(lm_wl_data_offer_interface, "wl_data_offer", 3,
             data_offer_requests, data_offer_events);

/* ---- wl_data_source ------------------------------------------------------------- */
static const struct lm_message data_source_requests[] = {
    MSG("offer", "s", 1, NULL),
    MSG("destroy", "", 1, NULL),
    MSG("set_actions", "u", 3, NULL),
};
static const struct lm_message data_source_events[] = {
    MSG("target", "?s", 1, NULL),
    MSG("send", "sh", 1, NULL),
    MSG("cancelled", "", 1, NULL),
    MSG("dnd_drop_performed", "", 3, NULL),
    MSG("dnd_finished", "", 3, NULL),
    MSG("action", "u", 3, NULL),
};
DEFINE_IFACE(lm_wl_data_source_interface, "wl_data_source", 3,
             data_source_requests, data_source_events);

/* ---- wl_data_device ------------------------------------------------------------- */
static const struct lm_message data_device_requests[] = {
    MSG("start_drag", "?oo?ou", 1,
        TY(&lm_wl_data_source_interface, &lm_wl_surface_interface,
           &lm_wl_surface_interface, NULL)),
    MSG("set_selection", "?ou", 1, TY(&lm_wl_data_source_interface, NULL)),
    MSG("release", "", 2, NULL),
};
static const struct lm_message data_device_events[] = {
    MSG("data_offer", "n", 1, TY(&lm_wl_data_offer_interface)),
    MSG("enter", "uoff?o", 1,
        TY(NULL, &lm_wl_surface_interface, NULL, NULL, &lm_wl_data_offer_interface)),
    MSG("leave", "", 1, NULL),
    MSG("motion", "uff", 1, NULL),
    MSG("drop", "", 1, NULL),
    MSG("selection", "?o", 1, TY(&lm_wl_data_offer_interface)),
};
DEFINE_IFACE(lm_wl_data_device_interface, "wl_data_device", 3,
             data_device_requests, data_device_events);

/* ---- wl_data_device_manager ----------------------------------------------------- */
static const struct lm_message data_device_manager_requests[] = {
    MSG("create_data_source", "n", 1, TY(&lm_wl_data_source_interface)),
    MSG("get_data_device", "no", 1, TY(&lm_wl_data_device_interface, &lm_wl_seat_interface)),
};
DEFINE_IFACE_REQ(lm_wl_data_device_manager_interface, "wl_data_device_manager", 3,
                 data_device_manager_requests);

/* ---- wl_surface ----------------------------------------------------------------- */
static const struct lm_message surface_requests[] = {
    MSG("destroy", "", 1, NULL),
    MSG("attach", "?oii", 1, TY(&lm_wl_buffer_interface, NULL, NULL)),
    MSG("damage", "iiii", 1, NULL),
    MSG("frame", "n", 1, TY(&lm_wl_callback_interface)),
    MSG("set_opaque_region", "?o", 1, TY(&lm_wl_region_interface)),
    MSG("set_input_region", "?o", 1, TY(&lm_wl_region_interface)),
    MSG("commit", "", 1, NULL),
    MSG("set_buffer_transform", "i", 2, NULL),
    MSG("set_buffer_scale", "i", 3, NULL),
    MSG("damage_buffer", "iiii", 4, NULL),
};
static const struct lm_message surface_events[] = {
    MSG("enter", "o", 1, TY(&lm_wl_output_interface)),
    MSG("leave", "o", 1, TY(&lm_wl_output_interface)),
};
DEFINE_IFACE(lm_wl_surface_interface, "wl_surface", 4, surface_requests, surface_events);

/* ---- wl_seat -------------------------------------------------------------------- */
static const struct lm_message seat_requests[] = {
    MSG("get_pointer", "n", 1, TY(&lm_wl_pointer_interface)),
    MSG("get_keyboard", "n", 1, TY(&lm_wl_keyboard_interface)),
    MSG("get_touch", "n", 1, TY(&lm_wl_touch_interface)),
    MSG("release", "", 5, NULL),
};
static const struct lm_message seat_events[] = {
    MSG("capabilities", "u", 1, NULL),
    MSG("name", "s", 2, NULL),
};
DEFINE_IFACE(lm_wl_seat_interface, "wl_seat", 5, seat_requests, seat_events);

/* ---- wl_pointer ----------------------------------------------------------------- */
static const struct lm_message pointer_requests[] = {
    MSG("set_cursor", "u?oii", 1, TY(NULL, &lm_wl_surface_interface, NULL, NULL)),
    MSG("release", "", 3, NULL),
};
static const struct lm_message pointer_events[] = {
    MSG("enter", "uoff", 1, TY(NULL, &lm_wl_surface_interface, NULL, NULL)),
    MSG("leave", "uo", 1, TY(NULL, &lm_wl_surface_interface)),
    MSG("motion", "uff", 1, NULL),
    MSG("button", "uuuu", 1, NULL),
    MSG("axis", "uuf", 1, NULL),
    MSG("frame", "", 5, NULL),
    MSG("axis_source", "u", 5, NULL),
    MSG("axis_stop", "uu", 5, NULL),
    MSG("axis_discrete", "ui", 5, NULL),
};
DEFINE_IFACE(lm_wl_pointer_interface, "wl_pointer", 5, pointer_requests, pointer_events);

/* ---- wl_keyboard ---------------------------------------------------------------- */
static const struct lm_message keyboard_requests[] = {
    MSG("release", "", 3, NULL),
};
static const struct lm_message keyboard_events[] = {
    MSG("keymap", "uhu", 1, NULL),
    MSG("enter", "uoa", 1, TY(NULL, &lm_wl_surface_interface, NULL)),
    MSG("leave", "uo", 1, TY(NULL, &lm_wl_surface_interface)),
    MSG("key", "uuuu", 1, NULL),
    MSG("modifiers", "uuuuu", 1, NULL),
    MSG("repeat_info", "ii", 4, NULL),
};
DEFINE_IFACE(lm_wl_keyboard_interface, "wl_keyboard", 5, keyboard_requests, keyboard_events);

/* ---- wl_touch ------------------------------------------------------------------- */
static const struct lm_message touch_requests[] = {
    MSG("release", "", 3, NULL),
};
static const struct lm_message touch_events[] = {
    MSG("down", "uuoiff", 1, TY(NULL, NULL, &lm_wl_surface_interface, NULL, NULL, NULL)),
    MSG("up", "uui", 1, NULL),
    MSG("motion", "uiff", 1, NULL),
    MSG("frame", "", 1, NULL),
    MSG("cancel", "", 1, NULL),
};
DEFINE_IFACE(lm_wl_touch_interface, "wl_touch", 5, touch_requests, touch_events);

/* ---- wl_output ------------------------------------------------------------------ */
static const struct lm_message output_requests[] = {
    MSG("release", "", 3, NULL),
};
static const struct lm_message output_events[] = {
    MSG("geometry", "iiiiissi", 1, NULL),
    MSG("mode", "uiii", 1, NULL),
    MSG("done", "", 2, NULL),
    MSG("scale", "i", 2, NULL),
};
DEFINE_IFACE(lm_wl_output_interface, "wl_output", 3, output_requests, output_events);

/* ---- wl_region ------------------------------------------------------------------ */
static const struct lm_message region_requests[] = {
    MSG("destroy", "", 1, NULL),
    MSG("add", "iiii", 1, NULL),
    MSG("subtract", "iiii", 1, NULL),
};
DEFINE_IFACE_REQ(lm_wl_region_interface, "wl_region", 1, region_requests);

/* ---- wl_subcompositor / wl_subsurface ------------------------------------------- */
static const struct lm_message subcompositor_requests[] = {
    MSG("destroy", "", 1, NULL),
    MSG("get_subsurface", "noo", 1,
        TY(&lm_wl_subsurface_interface, &lm_wl_surface_interface, &lm_wl_surface_interface)),
};
DEFINE_IFACE_REQ(lm_wl_subcompositor_interface, "wl_subcompositor", 1,
                 subcompositor_requests);

static const struct lm_message subsurface_requests[] = {
    MSG("destroy", "", 1, NULL),
    MSG("set_position", "ii", 1, NULL),
    MSG("place_above", "o", 1, TY(&lm_wl_surface_interface)),
    MSG("place_below", "o", 1, TY(&lm_wl_surface_interface)),
    MSG("set_sync", "", 1, NULL),
    MSG("set_desync", "", 1, NULL),
};
DEFINE_IFACE_REQ(lm_wl_subsurface_interface, "wl_subsurface", 1, subsurface_requests);

/* ---- xdg_wm_base ---------------------------------------------------------------- */
static const struct lm_message xdg_wm_base_requests[] = {
    MSG("destroy", "", 1, NULL),
    MSG("create_positioner", "n", 1, TY(&lm_xdg_positioner_interface)),
    MSG("get_xdg_surface", "no", 1, TY(&lm_xdg_surface_interface, &lm_wl_surface_interface)),
    MSG("pong", "u", 1, NULL),
};
static const struct lm_message xdg_wm_base_events[] = {
    MSG("ping", "u", 1, NULL),
};
DEFINE_IFACE(lm_xdg_wm_base_interface, "xdg_wm_base", 3,
             xdg_wm_base_requests, xdg_wm_base_events);

/* ---- xdg_positioner ------------------------------------------------------------- */
static const struct lm_message xdg_positioner_requests[] = {
    MSG("destroy", "", 1, NULL),
    MSG("set_size", "ii", 1, NULL),
    MSG("set_anchor_rect", "iiii", 1, NULL),
    MSG("set_anchor", "u", 1, NULL),
    MSG("set_gravity", "u", 1, NULL),
    MSG("set_constraint_adjustment", "u", 1, NULL),
    MSG("set_offset", "ii", 1, NULL),
    MSG("set_reactive", "", 3, NULL),
    MSG("set_parent_size", "ii", 3, NULL),
    MSG("set_parent_configure", "u", 3, NULL),
};
DEFINE_IFACE_REQ(lm_xdg_positioner_interface, "xdg_positioner", 3, xdg_positioner_requests);

/* ---- xdg_surface ---------------------------------------------------------------- */
static const struct lm_message xdg_surface_requests[] = {
    MSG("destroy", "", 1, NULL),
    MSG("get_toplevel", "n", 1, TY(&lm_xdg_toplevel_interface)),
    MSG("get_popup", "n?oo", 1,
        TY(&lm_xdg_popup_interface, &lm_xdg_surface_interface, &lm_xdg_positioner_interface)),
    MSG("set_window_geometry", "iiii", 1, NULL),
    MSG("ack_configure", "u", 1, NULL),
};
static const struct lm_message xdg_surface_events[] = {
    MSG("configure", "u", 1, NULL),
};
DEFINE_IFACE(lm_xdg_surface_interface, "xdg_surface", 3,
             xdg_surface_requests, xdg_surface_events);

/* ---- xdg_toplevel --------------------------------------------------------------- */
static const struct lm_message xdg_toplevel_requests[] = {
    MSG("destroy", "", 1, NULL),
    MSG("set_parent", "?o", 1, TY(&lm_xdg_toplevel_interface)),
    MSG("set_title", "s", 1, NULL),
    MSG("set_app_id", "s", 1, NULL),
    MSG("show_window_menu", "ouii", 1, TY(&lm_wl_seat_interface, NULL, NULL, NULL)),
    MSG("move", "ou", 1, TY(&lm_wl_seat_interface, NULL)),
    MSG("resize", "ouu", 1, TY(&lm_wl_seat_interface, NULL, NULL)),
    MSG("set_max_size", "ii", 1, NULL),
    MSG("set_min_size", "ii", 1, NULL),
    MSG("set_maximized", "", 1, NULL),
    MSG("unset_maximized", "", 1, NULL),
    MSG("set_fullscreen", "?o", 1, TY(&lm_wl_output_interface)),
    MSG("unset_fullscreen", "", 1, NULL),
    MSG("set_minimized", "", 1, NULL),
};
static const struct lm_message xdg_toplevel_events[] = {
    MSG("configure", "iia", 1, NULL),
    MSG("close", "", 1, NULL),
};
DEFINE_IFACE(lm_xdg_toplevel_interface, "xdg_toplevel", 3,
             xdg_toplevel_requests, xdg_toplevel_events);

/* ---- xdg_popup ------------------------------------------------------------------ */
static const struct lm_message xdg_popup_requests[] = {
    MSG("destroy", "", 1, NULL),
    MSG("grab", "ou", 1, TY(&lm_wl_seat_interface, NULL)),
    MSG("reposition", "ou", 3, TY(&lm_xdg_positioner_interface, NULL)),
};
static const struct lm_message xdg_popup_events[] = {
    MSG("configure", "iiii", 1, NULL),
    MSG("popup_done", "", 1, NULL),
    MSG("repositioned", "u", 3, NULL),
};
DEFINE_IFACE(lm_xdg_popup_interface, "xdg_popup", 3, xdg_popup_requests, xdg_popup_events);

/* ---- zxdg_decoration_manager_v1 / zxdg_toplevel_decoration_v1 -------------------- */
static const struct lm_message zxdg_decoration_manager_requests[] = {
    MSG("destroy", "", 1, NULL),
    MSG("get_toplevel_decoration", "no", 1,
        TY(&lm_zxdg_toplevel_decoration_v1_interface, &lm_xdg_toplevel_interface)),
};
DEFINE_IFACE_REQ(lm_zxdg_decoration_manager_v1_interface, "zxdg_decoration_manager_v1", 1,
                 zxdg_decoration_manager_requests);

static const struct lm_message zxdg_toplevel_decoration_requests[] = {
    MSG("destroy", "", 1, NULL),
    MSG("set_mode", "u", 1, NULL),
    MSG("unset_mode", "", 1, NULL),
};
static const struct lm_message zxdg_toplevel_decoration_events[] = {
    MSG("configure", "u", 1, NULL),
};
DEFINE_IFACE(lm_zxdg_toplevel_decoration_v1_interface, "zxdg_toplevel_decoration_v1", 1,
             zxdg_toplevel_decoration_requests, zxdg_toplevel_decoration_events);

/* ---- mellagui_settings_v1 ------------------------------------------------------------ */
static const struct lm_message mellagui_settings_requests[] = {
    MSG("destroy", "", 1, NULL),
    MSG("set", "ss", 1, NULL),
};
static const struct lm_message mellagui_settings_events[] = {
    MSG("value", "ss", 1, NULL),
    MSG("done", "", 1, NULL),
};
DEFINE_IFACE(lm_mellagui_settings_v1_interface, "mellagui_settings_v1", 1,
             mellagui_settings_requests, mellagui_settings_events);

/* ---- mellagui_capture_v1 ------------------------------------------------------------ */
static const struct lm_message mellagui_capture_requests[] = {
    MSG("destroy", "", 1, NULL),
    MSG("list", "", 1, NULL),
    MSG("grab", "io", 1, TY(NULL, &lm_wl_buffer_interface)),
};
static const struct lm_message mellagui_capture_events[] = {
    MSG("output", "isiiii", 1, NULL),
    MSG("done", "", 1, NULL),
    MSG("ready", "iii", 1, NULL),
    MSG("failed", "is", 1, NULL),
};
DEFINE_IFACE(lm_mellagui_capture_v1_interface, "mellagui_capture_v1", 1,
             mellagui_capture_requests, mellagui_capture_events);

/* ---- zwp_linux_dmabuf_v1 -------------------------------------------------------------------- */
static const struct lm_message dmabuf_requests[] = {
    MSG("destroy", "", 1, NULL),
    MSG("create_params", "n", 1, TY(&lm_zwp_linux_buffer_params_v1_interface)),
    MSG("get_default_feedback", "n", 4, TY(&lm_zwp_linux_dmabuf_feedback_v1_interface)),
    MSG("get_surface_feedback", "no", 4, TY(&lm_zwp_linux_dmabuf_feedback_v1_interface, &lm_wl_surface_interface)),
};
static const struct lm_message dmabuf_events[] = {
    MSG("format", "u", 1, NULL),
    MSG("modifier", "uuu", 3, NULL),
};
DEFINE_IFACE(lm_zwp_linux_dmabuf_v1_interface, "zwp_linux_dmabuf_v1", 4, dmabuf_requests, dmabuf_events);

static const struct lm_message dmabuf_params_requests[] = {
    MSG("destroy", "", 1, NULL),
    MSG("add", "huuuuu", 1, NULL),
    MSG("create", "iiuu", 1, NULL),
    MSG("create_immed", "niiuu", 2, TY(&lm_wl_buffer_interface, NULL, NULL, NULL, NULL)),
};
static const struct lm_message dmabuf_params_events[] = {
    MSG("created", "n", 1, TY(&lm_wl_buffer_interface)),
    MSG("failed", "", 1, NULL),
};
DEFINE_IFACE(lm_zwp_linux_buffer_params_v1_interface, "zwp_linux_buffer_params_v1", 4,
             dmabuf_params_requests, dmabuf_params_events);

static const struct lm_message dmabuf_feedback_requests[] = {
    MSG("destroy", "", 1, NULL),
};
static const struct lm_message dmabuf_feedback_events[] = {
    MSG("done", "", 1, NULL),
    MSG("format_table", "hu", 1, NULL),
    MSG("main_device", "a", 1, NULL),
    MSG("tranche_done", "", 1, NULL),
    MSG("tranche_target_device", "a", 1, NULL),
    MSG("tranche_formats", "a", 1, NULL),
    MSG("tranche_flags", "u", 1, NULL),
};
DEFINE_IFACE(lm_zwp_linux_dmabuf_feedback_v1_interface, "zwp_linux_dmabuf_feedback_v1", 4,
             dmabuf_feedback_requests, dmabuf_feedback_events);
