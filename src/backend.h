#ifndef MELLAGUI_BACKEND_H
#define MELLAGUI_BACKEND_H

/*
 * Display backend abstraction. Implemented by backend_drm.c (DRM/KMS on a
 * real VT) and backend_headless.c (offscreen, used by automated tests).
 *
 * A backend owns outputs (struct lm_output) and their framebuffers, tells
 * the compositor when to draw, and presents finished frames. It registers
 * its own fds (DRM events, VT signals, timers, control socket) with
 * server->loop during ops->init.
 *
 * ---- Frame protocol (per output) --------------------------------------------
 *   output_ready(out, t)  backend -> compositor: "you may draw now"; sent
 *                         once after output_added and again after each
 *                         presented frame became visible (DRM: page-flip
 *                         event; headless: 60 Hz timer). `t` is msec32.
 *   get_framebuffer(out)  compositor draws into the returned buffer. NULL
 *                         means "cannot draw now" (flip pending, session
 *                         inactive); wait for the next output_ready.
 *   present(out, damage)  queue the frame. After this get_framebuffer returns
 *                         NULL until the backend sends output_ready again.
 * Software renderer + double buffering: fb->age tells the renderer which
 * previous frame the buffer contains, so it knows how much to redraw
 * (0 = undefined contents, redraw everything; 1 = the frame drawn right
 * before the last present; 2 = the one before that, ...).
 *
 * ---- Headless control socket (backend_headless.c) -----------------------------
 * If config.control_path is set the headless backend listens on that AF_UNIX
 * SOCK_STREAM socket. Line-based; every command is answered with exactly
 * one line "ok[ ...]" or "err <message>" (multi-line output of `windows` is
 * terminated by a line "ok"):
 *   dump <path>        render pending damage now (lm_render_flush), write
 *                      output 0 as binary PPM (P6) to <path>
 *   sync               lm_render_flush, then reply ok (a barrier for tests)
 *   key <code> <0|1>   inject an evdev key (e.g. 28 = KEY_ENTER), 1 = press
 *   motion <dx> <dy>   inject relative pointer motion (already-accelerated px)
 *   abs <x> <y>        warp the pointer to absolute global pixel position
 *   button <code> <0|1>  e.g. 272 = BTN_LEFT
 *   axis <v|h> <value> <discrete>   scroll (value in px-like units, discrete steps)
 *   kbd                reply "ok layout=<id> slot=<n> sticky=<0|1> latched=<mods> slocked=<mods> sound=<name> sounds=<n>"  (last sticky-key beep, see sound.h)
 *   clip [clear]       (`clear` forgets the history first) one "clip <i> <text>" line per clipboard-history entry (newest first, \\n escaped), then "ok n=<count> gadget=<0|1>"
 *   gadget <0|1>       show / hide the desktop gadget
 *   gadget-rect [mixer|system]  reply "ok <x> <y> <w> <h>" (the clipboard / mixer gadget card, without shadow) or "ok hidden"
 *   sysinfo [0|1]      show / hide the system monitor gadget; reply "ok visible=<0|1> ready=<0|1> cpu=<%> cores=<n> mem=<%> mem_total_kb=<n> swap=<%> temp=<C or -1> load=<x.xx> hist=<n>"
 *   mixer [0|1]        show / hide the volume mixer gadget; reply "ok visible=<0|1> backend=<none|pulse|wpctl|alsa> ready=<0|1> volume=<n> mute=<0|1> streams=<n> output=<text, to the end of the line>"
 *   layout <slot|next|prev>   switch the keyboard layout
 *   sticky <0|1>       sticky keys on/off
 *   setting <key>      reply "ok <value>" for one setting (the long lists too)
 *   windows            dump lm_wm_describe() output
 *   quit               lm_server_quit()
 * SIGUSR1 (or `dump`) with config.dump_path set writes that file too.
 */

#include <stdbool.h>
#include <stdint.h>
#include "list.h"
#include "server.h"
#include "util.h"

struct lm_backend;
struct lm_output;
struct lm_gl;
struct lm_gl_window;

/* All backends draw XRGB8888 (0x00RRGGBB native endian = bytes B,G,R,X). */
struct lm_framebuffer {
    uint32_t *pixels;
    int32_t width, height;
    int32_t stride;     /* bytes */
    int age;            /* see "Frame protocol" */
};

struct lm_output {
    struct lm_list link;            /* in backend->outputs */
    struct lm_backend *backend;
    char name[32];                  /* "HDMI-A-1", "headless-0" */
    char make[32];
    char model[32];
    int32_t x, y;                   /* position in global compositor space; set by output.c */
    int32_t width, height;          /* current mode in pixels */
    int32_t refresh_mhz;            /* e.g. 60000 */
    int32_t phys_width_mm, phys_height_mm;
    int32_t scale;                  /* always 1 for now */
    uint32_t transform;             /* LM_TRANSFORM_NORMAL */

    void *backend_data;             /* backend private */
    void *render_data;              /* renderer private (damage, history) */
    void *output_data;              /* output.c private (bound wl_output resources) */
};

/* Callbacks installed by main.c before ops->init(). `data` is passed back. */
struct lm_backend_listener {
    void *data;
    /* A new output exists (called from init() for the initial ones and on hotplug). */
    void (*output_added)(void *data, struct lm_output *out);
    /* Mode/size changed (hotplug re-probe): width/height/refresh were updated. */
    void (*output_changed)(void *data, struct lm_output *out);
    /* Called before the output is unlinked and freed. */
    void (*output_removed)(void *data, struct lm_output *out);
    /* See "Frame protocol". */
    void (*output_ready)(void *data, struct lm_output *out, uint32_t time_ms);
    /*
     * VT/session switched away (false) or back (true). While inactive the
     * backend does not present and get_framebuffer returns NULL. On
     * re-activation everything must be redrawn (main.c calls
     * lm_render_damage_all) and input devices resumed (lm_input_set_active).
     */
    void (*session_active)(void *data, bool active);
};

/* One selectable video mode of an output (settings panel). hz is the rounded refresh rate. */
struct lm_mode_info {
    int w, h, hz;
    bool preferred;                 /* the monitor's own preferred mode */
    bool generated;                 /* not offered by the monitor: CVT timings made up for the panel's shape */
};
#define LM_MAX_MODES 24

struct lm_backend_ops {
    const char *name;
    /*
     * Open devices, probe outputs, register fds with b->server->loop and
     * emit output_added for each output. Returns 0 or -1 with a diagnostic
     * logged (e.g. how to get access to /dev/dri/card*).
     */
    int (*init)(struct lm_backend *b);
    /*
     * Restore the display (DRM: previous CRTC config, KD_TEXT, VT mode,
     * drop master) and free everything. Emits output_removed first.
     */
    void (*destroy)(struct lm_backend *b);
    struct lm_framebuffer *(*get_framebuffer)(struct lm_output *out);
    /*
     * Queue `out`'s current framebuffer for display. `damage` (global
     * coordinates translated to output-local) may be NULL = whole output.
     * Returns 0 or -1 (then the backend still sends output_ready later).
     */
    int (*present)(struct lm_output *out, const struct lm_rect *damage, int n_damage);
    /* Switch to virtual terminal `vt` (1..12). NULL if unsupported (headless). */
    int (*switch_vt)(struct lm_backend *b, int vt);
    /*
     * Settings panel: the modes `out` can show (best first, one entry per size, at most `max`), returns the count.
     * NULL = unsupported.
     */
    int (*list_modes)(struct lm_output *out, struct lm_mode_info *modes, int max);
    /*
     * Re-pick `out`'s mode from server->config.drm_mode_w/h/hz (0 = automatic, -1 = the preferred one) and switch to it
     * when it differs from the current one; on success width/height/refresh are updated and listener.output_changed
     * has been called. -1 = refused/impossible (the old mode stays). NULL = unsupported.
     */
    int (*apply_mode)(struct lm_output *out);
    /*
     * Monitors on (true) / in standby (false, DPMS off: the monitor loses its signal and sleeps). While off present() keeps
     * the picture up to date in memory but shows nothing and sends no output_ready; turning on again shows the current
     * picture and resumes output_ready. Sets b->blanked. NULL = unsupported. Returns 0 or -1.
     */
    int (*set_power)(struct lm_backend *b, bool on);
    /*
     * GL scanout (DRM only, when the backend made a GL context: lm_backend.gl): the window `out` renders into, or NULL
     * when this output is drawn through get_framebuffer. gl_present swaps its buffers and queues the page flip (like
     * present(): -1 = failed, output_ready follows anyway). NULL = unsupported.
     */
    struct lm_gl_window *(*gl_window)(struct lm_output *out);
    int (*gl_present)(struct lm_output *out);
};

struct lm_backend {
    const struct lm_backend_ops *ops;
    struct lm_server *server;
    struct lm_backend_listener listener;
    struct lm_list outputs;         /* struct lm_output.link */
    bool session_active;
    bool blanked;                   /* set_power(false) is in effect */
    struct lm_gl *gl;               /* set by a backend that renders with OpenGL (owned by it); the renderer picks it up */
    void *priv;
};

/*
 * DRM/KMS backend. Uses srv->config.drm_device, NULL = scan /dev/dri/card*. Opens devices with
 * open(2) (no logind). Manages VT (KD_GRAPHICS, VT_PROCESS switching) and restores
 * everything on exit, fatal signals and crashes.
 */
struct lm_backend *lm_backend_drm_create(struct lm_server *srv);

/* Offscreen backend with one w x h output named "headless-0". */
struct lm_backend *lm_backend_headless_create(struct lm_server *srv, int w, int h);
/* Write output 0's last drawn frame to `path` as P6 PPM. Returns 0 or -1. */
int lm_backend_headless_dump(struct lm_backend *b, const char *path);

/* Thin wrappers around the vtable. */
static inline int lm_backend_init(struct lm_backend *b) { return b->ops->init(b); }
static inline void lm_backend_destroy(struct lm_backend *b) { b->ops->destroy(b); }
static inline struct lm_framebuffer *lm_output_get_framebuffer(struct lm_output *o)
{
    return o->backend->ops->get_framebuffer(o);
}
static inline int lm_output_present(struct lm_output *o, const struct lm_rect *d, int n)
{
    return o->backend->ops->present(o, d, n);
}
static inline struct lm_gl_window *lm_output_gl_window(struct lm_output *o)
{
    return o->backend->ops->gl_window ? o->backend->ops->gl_window(o) : NULL;
}
static inline int lm_output_gl_present(struct lm_output *o)
{
    return o->backend->ops->gl_present ? o->backend->ops->gl_present(o) : -1;
}
static inline int lm_backend_switch_vt(struct lm_backend *b, int vt)
{
    return b->ops->switch_vt ? b->ops->switch_vt(b, vt) : -1;
}
static inline int lm_backend_set_power(struct lm_backend *b, bool on)
{
    if (!b->ops->set_power)
        return -1;
    int rc = b->ops->set_power(b, on);
    if (rc == 0)
        b->blanked = !on;
    return rc;
}
static inline int lm_output_list_modes(struct lm_output *o, struct lm_mode_info *m, int max)
{
    return o->backend->ops->list_modes ? o->backend->ops->list_modes(o, m, max) : 0;
}
static inline int lm_output_apply_mode(struct lm_output *o)
{
    return o->backend->ops->apply_mode ? o->backend->ops->apply_mode(o) : -1;
}

#endif
