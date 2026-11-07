/*
 * DRM/KMS backend for a real Linux virtual console.
 *
 * Legacy KMS (drmModeSetCrtc + drmModePageFlip) with two dumb buffers per
 * output. The virtual terminal is put into graphics mode with raw keyboard
 * off (input comes from evdev) and VT switching is handled with VT_PROCESS
 * so that Ctrl+Alt+Fn (done by the wm through switch_vt) and the kernel's own
 * switching cooperate. Everything is restored on exit and on fatal signals.
 *
 * This code needs real hardware and a real VT to run; it cannot be exercised
 * by the headless test-suite.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <linux/netlink.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <drm.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "backend.h"
#include "cvt.h"
#include "edid.h"
#include "log.h"
#include "logind.h"

#define HOTPLUG_POLL_MS 2000
#define SIG_VT_RELEASE SIGUSR2
#define SIG_VT_ACQUIRE (SIGRTMIN)

struct drm;

/* Parts of a dumb buffer that are older than the shadow frame and must still be copied into it. */
#define DIRTY_MAX 16
struct dirty {
    struct lm_rect r[DIRTY_MAX];
    int n;
};

struct drm_buf {
    struct dirty dirty;
    uint32_t handle;
    uint32_t fb_id;
    uint32_t pitch;
    size_t size;
    uint32_t *map;
    uint64_t seq;               /* frame number last presented through this buffer, 0 = never */
};

struct drm_out {
    struct lm_output base;      /* must be first */
    struct drm *d;
    uint32_t conn_id, crtc_id;
    drmModeModeInfo mode;
    drmModeCrtc *saved;         /* CRTC state to restore on exit */
    struct drm_buf buf[2];
    int back;                   /* buffer the next frame is shown through */
    /* The scene is composed into ordinary (cached) memory: the dumb buffers are write-combining, reading them back
     * for alpha blending is ~100x slower than writing (measured 54 MB/s vs 6.9 GB/s on Intel). present() copies only
     * the changed rectangles into the back buffer. */
    uint32_t *shadow;
    size_t shadow_size;
    bool shadow_valid;          /* holds a complete frame (age 1) */
    bool flip_pending;
    bool busy;                  /* inside a mode change: no output_ready meanwhile */
    bool live;
    uint64_t seq;               /* frames presented */
    struct lm_framebuffer fb;
    struct lm_timer *retry;     /* delayed output_ready after a failed flip */
};

struct drm {
    struct lm_backend base;     /* must be first */
    int fd;
    char path[300];
    struct lm_source *drm_source;
    struct lm_timer *hotplug;       /* cheap periodic look at the connectors' last known state */
    struct lm_timer *reprobe;       /* debounced forced probe after a kernel hotplug uevent */
    int uevent_fd;                  /* NETLINK_KOBJECT_UEVENT, -1 = none */
    struct lm_source *uevent_source;
    struct lm_timer *ready_timer;   /* "ready" after (re)activation */
    int tty_fd;
    struct lm_source *sig_release, *sig_acquire;
    bool is_master;                 /* with logind: we hold master through the session manager */
    bool active;
    struct lm_logind *logind;       /* NULL = plain open(2) + VT_PROCESS */
    dev_t dev;                      /* of the card, for logind's PauseDevice / ResumeDevice */
};

/* ---- console restore (also called from fatal signal handlers) ---------------------- */

static volatile int g_tty_fd = -1;
static volatile int g_old_kbmode = -1;
static volatile int g_old_kdmode = KD_TEXT;
static bool g_handlers_installed;

static void console_restore(void)
{
    int fd = g_tty_fd;
    if (fd < 0)
        return;
    struct vt_mode vm;
    memset(&vm, 0, sizeof(vm));
    vm.mode = VT_AUTO;
    ioctl(fd, VT_SETMODE, &vm);
    if (g_old_kbmode >= 0)
        ioctl(fd, KDSKBMODE, g_old_kbmode);
    ioctl(fd, KDSETMODE, g_old_kdmode == KD_GRAPHICS ? KD_TEXT : g_old_kdmode);
    g_tty_fd = -1;
}

static void fatal_signal(int sig)
{
    console_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_fatal_handlers(void)
{
    if (g_handlers_installed)
        return;
    g_handlers_installed = true;
    atexit(console_restore);
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = fatal_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESETHAND;
        sigaction(sigs[i], &sa, NULL);
    }
}

static struct drm *DRM(struct lm_backend *b) { return (struct drm *)b; }

/* ---- dumb buffers ------------------------------------------------------------------------ */

static void buf_destroy(struct drm *d, struct drm_buf *b)
{
    if (b->map && b->map != MAP_FAILED)
        munmap(b->map, b->size);
    if (b->fb_id)
        drmModeRmFB(d->fd, b->fb_id);
    if (b->handle) {
        struct drm_mode_destroy_dumb dd = { .handle = b->handle };
        drmIoctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    }
    memset(b, 0, sizeof(*b));
}

static int buf_create(struct drm *d, struct drm_buf *b, uint32_t w, uint32_t h)
{
    struct drm_mode_create_dumb cd = { .width = w, .height = h, .bpp = 32 };
    if (drmIoctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
        lm_error("drm: cannot create %ux%u dumb buffer: %s", w, h, strerror(errno));
        return -1;
    }
    b->handle = cd.handle;
    b->pitch = cd.pitch;
    b->size = cd.size;
    if (drmModeAddFB(d->fd, w, h, 24, 32, cd.pitch, cd.handle, &b->fb_id) < 0) {
        lm_error("drm: drmModeAddFB failed: %s", strerror(errno));
        buf_destroy(d, b);
        return -1;
    }
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (drmIoctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        lm_error("drm: map_dumb failed: %s", strerror(errno));
        buf_destroy(d, b);
        return -1;
    }
    void *p = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, (off_t)md.offset);
    if (p == MAP_FAILED) {
        lm_error("drm: mmap of dumb buffer failed: %s", strerror(errno));
        buf_destroy(d, b);
        return -1;
    }
    b->map = p;
    memset(b->map, 0, b->size);
    b->dirty.r[0] = lm_rect_make(0, 0, (int32_t)w, (int32_t)h);     /* everything must still be copied */
    b->dirty.n = 1;
    return 0;
}


/* ---- shadow frame ---------------------------------------------------------------------------- */

static void dirty_add(struct dirty *dt, struct lm_rect r)
{
    if (r.w <= 0 || r.h <= 0)
        return;
    if (dt->n == DIRTY_MAX) {                   /* too many pieces: keep one bounding rectangle */
        struct lm_rect e = dt->r[0];
        for (int i = 1; i < dt->n; i++)
            e = lm_rect_union(&e, &dt->r[i]);
        dt->r[0] = e;
        dt->n = 1;
    }
    dt->r[dt->n++] = r;
}

static void shadow_free(struct drm_out *o)
{
    free(o->shadow);
    o->shadow = NULL;
    o->shadow_size = 0;
    o->shadow_valid = false;
}

static int shadow_alloc(struct drm_out *o, size_t size)
{
    void *p = NULL;
    if (posix_memalign(&p, 64, size) != 0) {
        lm_error("drm: out of memory for the %zu byte shadow frame", size);
        return -1;
    }
    memset(p, 0, size);
    o->shadow = p;
    o->shadow_size = size;
    o->shadow_valid = false;
    return 0;
}

/* Copies `r` (output-local) from the shadow frame into dumb buffer `b`. */
static void copy_to_buf(const struct drm_out *o, struct drm_buf *b, struct lm_rect r)
{
    struct lm_rect full = lm_rect_make(0, 0, o->base.width, o->base.height);
    struct lm_rect c;
    if (!lm_rect_intersect(&r, &full, &c))
        return;
    size_t bytes = (size_t)c.w * 4;
    for (int y = c.y; y < c.y + c.h; y++) {
        size_t off = (size_t)y * b->pitch + (size_t)c.x * 4;
        memcpy((char *)b->map + off, (const char *)o->shadow + off, bytes);
    }
}

/* ---- output management ------------------------------------------------------------------- */

static const char *connector_type_name(uint32_t type)
{
    const char *n = drmModeGetConnectorTypeName(type);
    return n ? n : "Unknown";
}

static void out_ready_notify(struct drm_out *o, uint32_t time_ms)
{
    struct lm_backend *b = &o->d->base;
    if (o->live && !o->busy && b->listener.output_ready)
        b->listener.output_ready(b->listener.data, &o->base, time_ms);
}

static void retry_fire(void *data)
{
    struct drm_out *o = data;
    if (o->d->active)
        out_ready_notify(o, lm_now_msec32());
}

static bool crtc_in_use(struct drm *d, uint32_t crtc_id)
{
    struct lm_output *lo;
    lm_list_for_each(lo, &d->base.outputs, link) {
        struct drm_out *o = (struct drm_out *)lo;
        if (o->crtc_id == crtc_id)
            return true;
    }
    return false;
}

static uint32_t pick_crtc(struct drm *d, drmModeRes *res, drmModeConnector *conn)
{
    if (conn->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(d->fd, conn->encoder_id);
        if (enc) {
            uint32_t crtc = enc->crtc_id;
            drmModeFreeEncoder(enc);
            if (crtc && !crtc_in_use(d, crtc))
                return crtc;
        }
    }
    for (int i = 0; i < conn->count_encoders; i++) {
        drmModeEncoder *enc = drmModeGetEncoder(d->fd, conn->encoders[i]);
        if (!enc)
            continue;
        uint32_t possible = enc->possible_crtcs;
        drmModeFreeEncoder(enc);
        for (int j = 0; j < res->count_crtcs; j++)
            if ((possible & (1u << j)) && !crtc_in_use(d, res->crtcs[j]))
                return res->crtcs[j];
    }
    return 0;
}

static int out_modeset(struct drm_out *o)
{
    struct drm_buf *front = &o->buf[o->back ^ 1];
    uint32_t conn = o->conn_id;
    if (drmModeSetCrtc(o->d->fd, o->crtc_id, front->fb_id, 0, 0, &conn, 1, &o->mode) < 0) {
        lm_error("drm: modeset of %s failed: %s", o->base.name, strerror(errno));
        return -1;
    }
    return 0;
}

static void out_destroy(struct drm_out *o, bool restore)
{
    struct drm *d = o->d;
    struct lm_backend *b = &d->base;
    if (o->live) {
        o->live = false;
        if (b->listener.output_removed)
            b->listener.output_removed(b->listener.data, &o->base);
        lm_list_remove(&o->base.link);
    }
    if (o->retry)
        lm_timer_destroy(o->retry);
    if (restore && o->saved && d->is_master) {
        uint32_t conn = o->conn_id;
        if (o->saved->buffer_id && o->saved->mode_valid)
            drmModeSetCrtc(d->fd, o->saved->crtc_id, o->saved->buffer_id, o->saved->x, o->saved->y,
                           &conn, 1, &o->saved->mode);
    }
    if (o->saved)
        drmModeFreeCrtc(o->saved);
    shadow_free(o);
    buf_destroy(d, &o->buf[0]);
    buf_destroy(d, &o->buf[1]);
    free(o);
}

static drmModeModeInfo preferred_mode(const drmModeConnector *conn)
{
    drmModeModeInfo m = conn->modes[0];
    for (int i = 0; i < conn->count_modes; i++)
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            m = conn->modes[i];
            break;
        }
    return m;
}

/* A generated CVT reduced-blanking mode; false for sizes cvt.c refuses. */
static bool cvt_mode(int w, int h, int hz, drmModeModeInfo *m)
{
    struct lm_cvt c;
    if (!lm_cvt_rb(w, h, hz, &c))
        return false;
    memset(m, 0, sizeof(*m));
    m->clock = c.clock_khz;
    m->hdisplay = c.hdisplay; m->hsync_start = c.hsync_start; m->hsync_end = c.hsync_end; m->htotal = c.htotal;
    m->vdisplay = c.vdisplay; m->vsync_start = c.vsync_start; m->vsync_end = c.vsync_end; m->vtotal = c.vtotal;
    m->vrefresh = c.refresh;
    m->flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC;
    m->type = DRM_MODE_TYPE_USERDEF;
    snprintf(m->name, sizeof(m->name), "%dx%d", c.hdisplay, c.vdisplay);
    return true;
}

/* Mode for a connector.
 *  --mode WxH[@Hz]   a listed mode of that size, else a generated CVT-RB one;
 *  --mode preferred  the monitor's preferred mode, untouched;
 *  default           the preferred mode, unless its shape differs from the panel's physical shape (mm size from the EDID,
 *                    e.g. a 21:9 panel whose GPU only offers 16:9 modes): then a mode of the same width and the panel's
 *                    shape is generated, so the monitor's scaler does not stretch the picture. If the kernel refuses it,
 *                    out_create falls back to the preferred mode. */
/* The connector's EDID blob (free with drmModeFreePropertyBlob), NULL when there is none. */
static drmModePropertyBlobPtr conn_edid_blob(int fd, const drmModeConnector *conn)
{
    for (int i = 0; i < conn->count_props; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, conn->props[i]);
        if (!prop)
            continue;
        bool is_edid = !strcmp(prop->name, "EDID");
        drmModeFreeProperty(prop);
        if (is_edid)
            return drmModeGetPropertyBlob(fd, (uint32_t)conn->prop_values[i]);
    }
    return NULL;
}

/* The monitor's native timing from the connector's EDID blob; false when it cannot be read. */
static bool conn_native(int fd, const drmModeConnector *conn, struct lm_edid_native *out)
{
    drmModePropertyBlobPtr blob = conn_edid_blob(fd, conn);
    if (!blob)
        return false;
    bool ok = lm_edid_native(blob->data, blob->length, out);
    drmModeFreePropertyBlob(blob);
    return ok;
}

/* Who made the monitor and what it is called (wl_output make / model, the settings panel): from the EDID. */
static void conn_identify(int fd, const drmModeConnector *conn, struct lm_output *lo)
{
    snprintf(lo->make, sizeof(lo->make), "DRM");
    snprintf(lo->model, sizeof(lo->model), "Monitor");
    drmModePropertyBlobPtr blob = conn_edid_blob(fd, conn);
    if (!blob)
        return;
    struct lm_edid_info info;
    if (lm_edid_info(blob->data, blob->length, &info)) {
        snprintf(lo->make, sizeof(lo->make), "%s", lm_edid_vendor_name(info.vendor));
        if (info.name[0])
            snprintf(lo->model, sizeof(lo->model), "%s", info.name);
        else
            snprintf(lo->model, sizeof(lo->model), "%s %04X", info.vendor, info.product);
        if (!conn->mmWidth && info.mm_w > 0) {
            lo->phys_width_mm = info.mm_w;
            lo->phys_height_mm = info.mm_h;
        }
    }
    drmModeFreePropertyBlob(blob);
}

static drmModeModeInfo pick_mode(const struct lm_config *cfg, const drmModeConnector *conn, const drmModeModeInfo *preferred,
                                 const struct lm_edid_native *nat, bool log)
{
    drmModeModeInfo m = *preferred;
    if (cfg->drm_mode_w > 0) {
        bool found = false;
        for (int i = 0; i < conn->count_modes && !found; i++)
            if (conn->modes[i].hdisplay == cfg->drm_mode_w && conn->modes[i].vdisplay == cfg->drm_mode_h &&
                (int)conn->modes[i].vrefresh == cfg->drm_mode_hz) {
                m = conn->modes[i];
                found = true;
            }
        for (int i = 0; i < conn->count_modes && !found; i++)             /* same size, other refresh rate */
            if (conn->modes[i].hdisplay == cfg->drm_mode_w && conn->modes[i].vdisplay == cfg->drm_mode_h) {
                m = conn->modes[i];
                found = true;
            }
        if (!found) {
            if (cvt_mode(cfg->drm_mode_w, cfg->drm_mode_h, cfg->drm_mode_hz ? cfg->drm_mode_hz : 60, &m)) {
                if (log)
                    lm_info("drm: --mode %dx%d is not offered by the monitor: using generated CVT-RB timings (%u kHz)",
                            cfg->drm_mode_w, cfg->drm_mode_h, m.clock);
            } else {
                if (log)
                    lm_warn("drm: cannot use --mode %dx%d, using the preferred mode", cfg->drm_mode_w, cfg->drm_mode_h);
                m = *preferred;
            }
        }
        return m;
    }
    if (cfg->drm_mode_w < 0 || m.vdisplay == 0)
        return m;                                                     /* --mode preferred */
    int w = m.hdisplay & ~7, hz = m.vrefresh ? (int)m.vrefresh : 60;
    if (nat) {
        for (int i = 0; i < conn->count_modes; i++)                   /* the kernel offers the native mode itself: leave it */
            if (conn->modes[i].hdisplay == nat->w && conn->modes[i].vdisplay == nat->h)
                return m;
        int h = lm_edid_fix_height(m.hdisplay, m.vdisplay, nat, (int)conn->mmWidth, (int)conn->mmHeight);
        drmModeModeInfo g;
        /* never ask for a faster pixel clock than the monitor already takes (the largest clock among its listed modes is a
         * safe upper bound for the link) */
        uint32_t max_clock = 0;
        for (int i = 0; i < conn->count_modes; i++)
            if (conn->modes[i].clock > max_clock)
                max_clock = conn->modes[i].clock;
        if (h > 0 && cvt_mode(w, h, hz, &g) && g.clock <= max_clock) {
            if (log) {
                lm_info("drm: the monitor's native mode is %dx%d (%.2f:1) but only %ux%u (%.2f:1) is offered and would be "
                        "stretched; using generated %dx%d (CVT-RB, %u kHz). Use --mode preferred to turn this off",
                        nat->w, nat->h, (double)nat->w / nat->h, m.hdisplay, m.vdisplay, (double)m.hdisplay / m.vdisplay,
                        g.hdisplay, g.vdisplay, g.clock);
            }
            return g;
        }
        if (h > 0 && log)
            lm_warn("drm: the monitor's native shape is %dx%d but mode %ux%u would be stretched, and %dx%d needs a faster "
                    "pixel clock than the link takes; set the monitor's aspect-ratio option to keep proportions",
                    nat->w, nat->h, m.hdisplay, m.vdisplay, w, h);
        return m;
    }
    /* No EDID timing to go by: only warn when the physical size says the picture will be stretched. */
    if (conn->mmWidth > 0 && conn->mmHeight > 0 && log) {
        double panel = (double)conn->mmWidth / conn->mmHeight, mode = (double)m.hdisplay / m.vdisplay;
        if (fabs(panel / mode - 1.0) > 0.08) {
            lm_warn("drm: the panel is %ux%u mm (%.2f:1) but mode %ux%u is %.2f:1; the monitor may stretch the picture. "
                    "Try --mode %dx%d (or set the monitor's aspect-ratio option to keep proportions)",
                    conn->mmWidth, conn->mmHeight, panel, m.hdisplay, m.vdisplay, mode, w,
                    (int)lround(w / panel / 2.0) * 2);
        }
    }
    return m;
}

static struct drm_out *out_create(struct drm *d, drmModeRes *res, drmModeConnector *conn)
{
    if (conn->count_modes <= 0)
        return NULL;
    uint32_t crtc = pick_crtc(d, res, conn);
    if (!crtc) {
        lm_warn("drm: no free CRTC for connector %u", conn->connector_id);
        return NULL;
    }
    struct drm_out *o = calloc(1, sizeof(*o));
    if (!o)
        return NULL;
    o->d = d;
    o->conn_id = conn->connector_id;
    o->crtc_id = crtc;
    drmModeModeInfo preferred = preferred_mode(conn);
    struct lm_edid_native nat;
    bool have_nat = conn_native(d->fd, conn, &nat);
    o->mode = pick_mode(&d->base.server->config, conn, &preferred, have_nat ? &nat : NULL, true);
    o->saved = drmModeGetCrtc(d->fd, crtc);

    uint32_t w, h;
    for (int attempt = 0;; attempt++) {
        w = o->mode.hdisplay;
        h = o->mode.vdisplay;
        o->back = 0;
        if (buf_create(d, &o->buf[0], w, h) == 0 && buf_create(d, &o->buf[1], w, h) == 0 &&
            shadow_alloc(o, o->buf[0].size) == 0) {
            if (out_modeset(o) == 0)
                break;
            shadow_free(o);
        }
        buf_destroy(d, &o->buf[0]);
        buf_destroy(d, &o->buf[1]);
        if (attempt > 0 || !memcmp(&o->mode, &preferred, sizeof(preferred)))
            goto fail;
        lm_warn("drm: mode %s was refused, falling back to the preferred %s", o->mode.name, preferred.name);
        o->mode = preferred;
    }

    struct lm_output *lo = &o->base;
    lo->backend = &d->base;
    snprintf(lo->name, sizeof(lo->name), "%s-%u", connector_type_name(conn->connector_type),
             conn->connector_type_id);
    conn_identify(d->fd, conn, lo);
    lo->width = (int32_t)w;
    lo->height = (int32_t)h;
    uint32_t vr = o->mode.vrefresh ? o->mode.vrefresh : 60;
    if (o->mode.htotal && o->mode.vtotal && o->mode.clock)
        lo->refresh_mhz = (int32_t)(((uint64_t)o->mode.clock * 1000000ull) /
                                    ((uint64_t)o->mode.htotal * o->mode.vtotal));
    else
        lo->refresh_mhz = (int32_t)vr * 1000;
    if (conn->mmWidth) {
        lo->phys_width_mm = (int32_t)conn->mmWidth;
        lo->phys_height_mm = (int32_t)conn->mmHeight;
    }
    lo->scale = 1;
    lo->backend_data = o;
    o->retry = lm_loop_add_timer(d->base.server->loop, retry_fire, o);
    lm_list_append(&d->base.outputs, &lo->link);
    o->live = true;
    if (d->base.listener.output_added)
        d->base.listener.output_added(d->base.listener.data, lo);
    return o;

fail:
    if (o->saved)
        drmModeFreeCrtc(o->saved);
    shadow_free(o);
    buf_destroy(d, &o->buf[0]);
    buf_destroy(d, &o->buf[1]);
    free(o);
    return NULL;
}

static bool conn_has_output(struct drm *d, uint32_t conn_id)
{
    struct lm_output *lo;
    lm_list_for_each(lo, &d->base.outputs, link)
        if (((struct drm_out *)lo)->conn_id == conn_id)
            return true;
    return false;
}

/* A forced probe makes the kernel re-read every connector's EDID over I2C: on this class of hardware that is 40-100 ms
 * per connector (230 ms for 7), during which the single-threaded loop - pointer and picture - stands still. So it is
 * only done at start-up and after the kernel announced a hotplug; the periodic look uses the last known state, which
 * the kernel keeps current for ports with hotplug detection. */
static drmModeConnector *get_conn(struct drm *d, uint32_t id, bool force)
{
    return force ? drmModeGetConnector(d->fd, id) : drmModeGetConnectorCurrent(d->fd, id);
}

/* Returns number of outputs after the scan. */
static int probe_outputs(struct drm *d, bool force)
{
    drmModeRes *res = drmModeGetResources(d->fd);
    if (!res) {
        lm_error("drm: drmModeGetResources failed on %s: %s", d->path, strerror(errno));
        return -1;
    }
    /* Drop outputs whose connector went away. */
    struct lm_output *lo, *tmp;
    lm_list_for_each_safe(lo, tmp, &d->base.outputs, link) {
        struct drm_out *o = (struct drm_out *)lo;
        drmModeConnector *c = get_conn(d, o->conn_id, force);
        bool gone = !c || c->connection != DRM_MODE_CONNECTED;
        if (c)
            drmModeFreeConnector(c);
        if (gone) {
            lm_info("drm: output %s disconnected", lo->name);
            out_destroy(o, false);
        }
    }
    for (int i = 0; i < res->count_connectors; i++) {
        uint32_t id = res->connectors[i];
        if (conn_has_output(d, id))
            continue;
        drmModeConnector *c = get_conn(d, id, force);
        if (!c)
            continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0 && d->active) {
            struct drm_out *o = out_create(d, res, c);
            if (o) {
                lm_info("drm: output %s %dx%d connected", o->base.name, o->base.width, o->base.height);
                lm_timer_arm_ms(d->ready_timer, 1, 0);
            }
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    return lm_list_length(&d->base.outputs);
}

static void ready_all_fire(void *data)
{
    struct drm *d = data;
    if (!d->active)
        return;
    struct lm_output *lo;
    lm_list_for_each(lo, &d->base.outputs, link) {
        struct drm_out *o = (struct drm_out *)lo;
        if (!o->flip_pending)
            out_ready_notify(o, lm_now_msec32());
    }
}

static void hotplug_fire(void *data)
{
    struct drm *d = data;
    if (!d->active)
        return;
    probe_outputs(d, false);
}

static void reprobe_fire(void *data)
{
    struct drm *d = data;
    if (d->active)
        probe_outputs(d, true);
}

/* Kernel uevents: only "change" events of the drm subsystem with HOTPLUG=1 matter. */
static void uevent_cb(int fd, uint32_t events, void *data)
{
    struct drm *d = data;
    (void)events;
    char buf[2048];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
        if (n <= 0)
            break;
        buf[n] = 0;
        bool is_drm = false, hotplug = false;
        for (ssize_t i = 0; i < n; i += (ssize_t)strlen(buf + i) + 1) {
            if (!strcmp(buf + i, "SUBSYSTEM=drm"))
                is_drm = true;
            else if (!strcmp(buf + i, "HOTPLUG=1"))
                hotplug = true;
        }
        if (is_drm && hotplug)
            lm_timer_arm_ms(d->reprobe, 300, 0);       /* the monitor needs a moment; several events become one */
    }
}

static void uevent_open(struct drm *d)
{
    d->uevent_fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (d->uevent_fd < 0) {
        lm_warn("drm: no uevent socket (%s): monitors plugged in later are noticed only by the slow poll", strerror(errno));
        return;
    }
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1;                                   /* the kernel's own events, no udev needed */
    if (bind(d->uevent_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        lm_warn("drm: cannot listen for hotplug uevents (%s)", strerror(errno));
        close(d->uevent_fd);
        d->uevent_fd = -1;
        return;
    }
    d->uevent_source = lm_loop_add_fd(d->base.server->loop, d->uevent_fd, EPOLLIN, uevent_cb, d);
}

/* ---- frame protocol -------------------------------------------------------------------------- */

static struct lm_framebuffer *drm_get_framebuffer(struct lm_output *out)
{
    struct drm_out *o = out->backend_data;
    struct drm *d = o->d;
    if (!d->active || o->flip_pending || !o->live)
        return NULL;
    o->fb.pixels = o->shadow;
    o->fb.width = out->width;
    o->fb.height = out->height;
    o->fb.stride = (int32_t)o->buf[0].pitch;
    o->fb.age = o->shadow_valid ? 1 : 0;        /* the shadow always holds the last composed frame */
    return &o->fb;
}

static int drm_present(struct lm_output *out, const struct lm_rect *damage, int n)
{
    struct drm_out *o = out->backend_data;
    struct drm *d = o->d;
    if (!d->active || !o->live)
        return -1;
    struct drm_buf *b = &o->buf[o->back];
    struct drm_buf *other = &o->buf[o->back ^ 1];
    if (d->base.blanked) {                  /* standby: a flip on a CRTC that is off fails; show the picture once it is back */
        struct lm_rect all = lm_rect_make(0, 0, out->width, out->height);
        dirty_add(&b->dirty, all);
        dirty_add(&other->dirty, all);
        o->shadow_valid = true;
        return 0;
    }
    /* What changed in this frame is missing from both dumb buffers; the back one gets it now, the other (currently
     * on screen) is brought up to date by the next present. */
    struct lm_rect whole = lm_rect_make(0, 0, out->width, out->height);
    if (!damage || n <= 0) {
        dirty_add(&b->dirty, whole);
        dirty_add(&other->dirty, whole);
    } else {
        for (int i = 0; i < n; i++) {
            dirty_add(&b->dirty, damage[i]);
            dirty_add(&other->dirty, damage[i]);
        }
    }
    for (int i = 0; i < b->dirty.n; i++)
        copy_to_buf(o, b, b->dirty.r[i]);
    b->dirty.n = 0;
    o->shadow_valid = true;
    if (drmModePageFlip(d->fd, o->crtc_id, b->fb_id, DRM_MODE_PAGE_FLIP_EVENT, o) < 0) {
        lm_warn("drm: page flip on %s failed: %s", out->name, strerror(errno));
        lm_timer_arm_ms(o->retry, 16, 0);
        return -1;
    }
    b->seq = ++o->seq;
    o->flip_pending = true;
    return 0;
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
    (void)fd;
    (void)frame;
    struct drm_out *o = data;
    o->flip_pending = false;
    o->back ^= 1;
    out_ready_notify(o, (uint32_t)(sec * 1000u + usec / 1000u));
}

static void drm_event_cb(int fd, uint32_t events, void *data)
{
    struct drm *d = data;
    (void)events;
    drmEventContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.version = 2;
    ctx.page_flip_handler = page_flip_handler;
    if (drmHandleEvent(fd, &ctx) < 0)
        lm_warn("drm: drmHandleEvent: %s", strerror(errno));
    (void)d;
}

/* ---- VT handling ----------------------------------------------------------------------------------- */

/* The session goes away (VT switched away): stop drawing, tell the seat. Returns false when already inactive. */
static bool session_pause(struct drm *d, bool drop_master)
{
    if (!d->active)
        return false;
    d->active = false;
    d->base.session_active = false;
    if (d->base.listener.session_active)
        d->base.listener.session_active(d->base.listener.data, false);
    if (d->is_master && drop_master)
        drmDropMaster(d->fd);
    d->is_master = false;
    return true;
}

/* The session is back and we are master again: redo every output. */
static void session_resume(struct drm *d)
{
    d->active = true;
    d->base.session_active = true;
    d->base.blanked = false;                /* the modeset below lights the monitors up again */
    struct lm_output *lo;
    lm_list_for_each(lo, &d->base.outputs, link) {
        struct drm_out *o = (struct drm_out *)lo;
        o->flip_pending = false;
        o->buf[0].seq = o->buf[1].seq = 0;
        o->shadow_valid = false;            /* contents are unknown now: force full redraws */
        out_modeset(o);
    }
    if (d->base.listener.session_active)
        d->base.listener.session_active(d->base.listener.data, true);
    lm_timer_arm_ms(d->ready_timer, 1, 0);
}

static void vt_release(int signo, void *data)
{
    struct drm *d = data;
    (void)signo;
    if (!d->active) {
        ioctl(d->tty_fd, VT_RELDISP, 1);
        return;
    }
    lm_info("VT: switching away");
    session_pause(d, true);
    ioctl(d->tty_fd, VT_RELDISP, 1);
}

static void vt_acquire(int signo, void *data)
{
    struct drm *d = data;
    (void)signo;
    lm_info("VT: switching back");
    ioctl(d->tty_fd, VT_RELDISP, VT_ACKACQ);
    if (drmSetMaster(d->fd) < 0)
        lm_warn("drm: drmSetMaster after VT switch failed: %s", strerror(errno));
    else
        d->is_master = true;
    session_resume(d);
}

/* logind: PauseDevice for the card = the VT is being switched away; logind waits for our ack, then drops master itself. */
static void logind_pause(void *data, const char *type)
{
    struct drm *d = data;
    lm_info("logind: the session is paused (%s)", type);
    session_pause(d, false);
    if (!strcmp(type, "pause"))
        lm_logind_pause_done(d->logind, d->dev);
}

/* ResumeDevice: logind made us master again; the descriptor it passes is the very same open file as ours. */
static void logind_resume(void *data, int fd)
{
    struct drm *d = data;
    if (fd >= 0)
        close(fd);
    lm_info("logind: the session is active again");
    d->is_master = true;
    session_resume(d);
}

static int vt_setup(struct drm *d)
{
    d->tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (d->tty_fd < 0) {
        lm_error("drm: cannot open /dev/tty (%s). Start MellaGUI from a Linux virtual console "
                 "(switch with Ctrl+Alt+F3 and log in), not from a terminal emulator or ssh.",
                 strerror(errno));
        return -1;
    }
    struct vt_stat st;
    if (ioctl(d->tty_fd, VT_GETSTATE, &st) < 0) {
        lm_error("drm: %s is not a Linux virtual console. Start MellaGUI from a text VT "
                 "(Ctrl+Alt+F3), not from a terminal emulator or ssh.", "/dev/tty");
        close(d->tty_fd);
        d->tty_fd = -1;
        return -1;
    }
    int kbmode = -1, kdmode = KD_TEXT;
    ioctl(d->tty_fd, KDGKBMODE, &kbmode);
    ioctl(d->tty_fd, KDGETMODE, &kdmode);
    g_old_kbmode = kbmode;
    g_old_kdmode = kdmode;
    g_tty_fd = d->tty_fd;
    install_fatal_handlers();

    d->sig_release = lm_loop_add_signal(d->base.server->loop, SIG_VT_RELEASE, vt_release, d);
    d->sig_acquire = lm_loop_add_signal(d->base.server->loop, SIG_VT_ACQUIRE, vt_acquire, d);

    if (ioctl(d->tty_fd, KDSETMODE, KD_GRAPHICS) < 0) {
        lm_error("drm: KDSETMODE(KD_GRAPHICS) failed: %s", strerror(errno));
        return -1;
    }
    /* Keys come from evdev; the kernel tty must not interpret or echo them. */
    if (ioctl(d->tty_fd, KDSKBMODE, K_OFF) < 0)
        lm_warn("drm: KDSKBMODE(K_OFF) failed: %s (keystrokes may reach the tty)", strerror(errno));
    struct vt_mode vm;
    memset(&vm, 0, sizeof(vm));
    vm.mode = VT_PROCESS;
    vm.relsig = SIG_VT_RELEASE;
    vm.acqsig = SIG_VT_ACQUIRE;
    if (ioctl(d->tty_fd, VT_SETMODE, &vm) < 0) {
        lm_error("drm: VT_SETMODE failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int drm_switch_vt(struct lm_backend *b, int vt)
{
    struct drm *d = DRM(b);
    if (d->logind)
        return d->active ? lm_logind_switch_vt(d->logind, vt) : -1;
    if (d->tty_fd < 0 || vt < 1 || vt > 12 || !d->active)
        return -1;
    if (ioctl(d->tty_fd, VT_ACTIVATE, vt) < 0) {
        lm_warn("drm: VT_ACTIVATE %d failed: %s", vt, strerror(errno));
        return -1;
    }
    return 0;
}

/* ---- device selection ---------------------------------------------------------------------------------- */

static int open_card(struct drm *d, const char *path)
{
    bool inactive = false;
    int fd = lm_dev_open(d->logind, path, O_RDWR | O_NONBLOCK, &inactive);
    if (fd >= 0 && inactive) {
        lm_error("drm: this session is not the active one (logind): switch to its virtual terminal and start MellaGUI there.");
        lm_dev_close(d->logind, fd);
        return -EAGAIN;
    }
    if (fd < 0) {
        int err = -fd;
        if (err == EACCES && !d->logind)
            lm_error("drm: no permission to open %s. Add yourself to group 'video' "
                     "(sudo usermod -aG video $USER, then log in again) or run from a "
                     "logged-in console session.", path);
        else
            lm_debug("drm: cannot open %s: %s", path, strerror(err));
        return -err;
    }
    uint64_t dumb = 0;
    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &dumb) < 0 || !dumb) {
        lm_debug("drm: %s has no dumb buffer support", path);
        lm_dev_close(d->logind, fd);
        return -ENOTSUP;
    }
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        lm_debug("drm: %s has no KMS resources", path);
        lm_dev_close(d->logind, fd);
        return -ENODEV;
    }
    bool connected = false;
    for (int i = 0; i < res->count_connectors && !connected; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (c) {
            connected = c->connection == DRM_MODE_CONNECTED && c->count_modes > 0;
            drmModeFreeConnector(c);
        }
    }
    drmModeFreeResources(res);
    if (!connected) {
        lm_debug("drm: %s has no connected display", path);
        lm_dev_close(d->logind, fd);
        return -ENOLINK;
    }
    return fd;
}

static int select_device(struct drm *d, const char *forced)
{
    if (forced) {
        snprintf(d->path, sizeof(d->path), "%s", forced);
        d->fd = open_card(d, d->path);
        return d->fd >= 0 ? 0 : -1;
    }
    char names[16][300];
    int n = 0;
    DIR *dir = opendir("/dev/dri");
    if (!dir) {
        lm_error("drm: cannot read /dev/dri: %s (no GPU / KMS driver loaded?)", strerror(errno));
        return -1;
    }
    struct dirent *de;
    while ((de = readdir(dir)) && n < 16)
        if (!strncmp(de->d_name, "card", 4) && de->d_name[4] >= '0' && de->d_name[4] <= '9')
            snprintf(names[n++], sizeof(names[0]), "/dev/dri/%s", de->d_name);
    closedir(dir);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char t[300];
                memcpy(t, names[i], 300);
                memcpy(names[i], names[j], 300);
                memcpy(names[j], t, 300);
            }
    for (int i = 0; i < n; i++) {
        d->fd = open_card(d, names[i]);
        if (d->fd >= 0) {
            snprintf(d->path, sizeof(d->path), "%.*s", (int)sizeof(d->path) - 1, names[i]);
            return 0;
        }
    }
    lm_error("drm: no usable DRM device with a connected display found in /dev/dri");
    return -1;
}

/* ---- lifecycle ---------------------------------------------------------------------------------------------- */

static int drm_init(struct lm_backend *b)
{
    struct drm *d = DRM(b);
    struct lm_server *srv = b->server;

    d->ready_timer = lm_loop_add_timer(srv->loop, ready_all_fire, d);
    d->hotplug = lm_loop_add_timer(srv->loop, hotplug_fire, d);
    d->reprobe = lm_loop_add_timer(srv->loop, reprobe_fire, d);
    d->uevent_fd = -1;

    d->logind = srv->logind;
    if (!d->logind && vt_setup(d) < 0)          /* with logind the session manager owns the VT and the console mode */
        return -1;
    if (select_device(d, srv->config.drm_device) < 0)
        return -1;
    lm_info("drm: using %s%s", d->path, d->logind ? " (through logind)" : "");

    if (d->logind) {
        struct stat st;
        if (fstat(d->fd, &st) == 0)
            d->dev = st.st_rdev;
        lm_logind_watch(d->logind, d->dev, logind_pause, logind_resume, d);       /* logind made the fd DRM master */
    } else if (drmSetMaster(d->fd) < 0) {
        lm_error("drm: cannot become DRM master on %s: %s. Another display server "
                 "(X, Wayland compositor) probably owns the GPU on this VT.", d->path, strerror(errno));
        return -1;
    }
    d->is_master = true;
    d->active = true;
    b->session_active = true;

    if (probe_outputs(d, true) <= 0) {
        lm_error("drm: no output could be set up on %s", d->path);
        return -1;
    }
    d->drm_source = lm_loop_add_fd(srv->loop, d->fd, EPOLLIN, drm_event_cb, d);
    uevent_open(d);
    lm_timer_arm_ms(d->ready_timer, 1, 0);
    lm_timer_arm_ms(d->hotplug, HOTPLUG_POLL_MS, HOTPLUG_POLL_MS);
    return 0;
}

static void drm_destroy(struct lm_backend *b)
{
    struct drm *d = DRM(b);
    struct lm_output *lo, *tmp;

    if (d->hotplug)
        lm_timer_destroy(d->hotplug);
    if (d->reprobe)
        lm_timer_destroy(d->reprobe);
    if (d->uevent_source)
        lm_source_remove(d->uevent_source);
    if (d->uevent_fd >= 0)
        close(d->uevent_fd);
    if (d->ready_timer)
        lm_timer_destroy(d->ready_timer);
    if (d->drm_source)
        lm_source_remove(d->drm_source);
    lm_list_for_each_safe(lo, tmp, &b->outputs, link)
        out_destroy((struct drm_out *)lo, true);
    if (d->fd >= 0) {
        if (d->is_master && !d->logind)
            drmDropMaster(d->fd);
        lm_dev_close(d->logind, d->fd);
    }
    if (d->tty_fd >= 0) {
        console_restore();
        close(d->tty_fd);
    }
    if (d->sig_release)
        lm_source_remove(d->sig_release);
    if (d->sig_acquire)
        lm_source_remove(d->sig_acquire);
    free(d);
}

/* ---- mode list and live mode change (settings panel) ---------------------------------------------- */

static bool same_timing(const drmModeModeInfo *a, const drmModeModeInfo *b)
{
    return a->hdisplay == b->hdisplay && a->vdisplay == b->vdisplay && a->clock == b->clock && a->htotal == b->htotal &&
           a->vtotal == b->vtotal;
}

static int drm_list_modes(struct lm_output *out, struct lm_mode_info *modes, int max)
{
    struct drm_out *o = out->backend_data;
    drmModeConnector *conn = drmModeGetConnectorCurrent(o->d->fd, o->conn_id);
    if (!conn || conn->count_modes <= 0) {
        if (conn)
            drmModeFreeConnector(conn);
        return 0;
    }
    drmModeModeInfo pref = preferred_mode(conn);
    int n = 0;
    for (int i = 0; i < conn->count_modes && n < max; i++) {            /* one entry per size: the fastest refresh rate */
        const drmModeModeInfo *m = &conn->modes[i];
        int at = -1;
        for (int j = 0; j < n; j++)
            if (modes[j].w == m->hdisplay && modes[j].h == m->vdisplay)
                at = j;
        int hz = (int)m->vrefresh;
        if (at >= 0) {
            if (hz > modes[at].hz)
                modes[at].hz = hz;
            if (same_timing(m, &pref))
                modes[at].preferred = true;
            continue;
        }
        modes[n++] = (struct lm_mode_info){ m->hdisplay, m->vdisplay, hz, same_timing(m, &pref), false };
    }
    struct lm_config none = { 0 };                                      /* what "automatic" would generate */
    struct lm_edid_native nat;
    bool have_nat = conn_native(o->d->fd, conn, &nat);
    drmModeModeInfo au = pick_mode(&none, conn, &pref, have_nat ? &nat : NULL, false);
    if (au.type & DRM_MODE_TYPE_USERDEF) {
        bool known = false;
        for (int j = 0; j < n; j++)
            known = known || (modes[j].w == au.hdisplay && modes[j].h == au.vdisplay);
        if (!known && n < max)
            modes[n++] = (struct lm_mode_info){ au.hdisplay, au.vdisplay, (int)au.vrefresh, false, true };
    }
    if (have_nat && n < max) {                                          /* the panel's own size, if the link can carry it at some rate */
        bool known = false;
        uint32_t max_clock = 0;
        for (int j = 0; j < n; j++)
            known = known || (modes[j].w == nat.w && modes[j].h == nat.h);
        for (int i = 0; i < conn->count_modes; i++)
            if (conn->modes[i].clock > max_clock)
                max_clock = conn->modes[i].clock;
        struct lm_cvt cv;
        if (!known && lm_cvt_rb_fit(nat.w, nat.h, 60, max_clock, &cv))
            modes[n++] = (struct lm_mode_info){ cv.hdisplay, cv.vdisplay, cv.refresh, false, true };
    }
    drmModeFreeConnector(conn);
    for (int i = 1; i < n; i++) {                                       /* biggest area first */
        struct lm_mode_info t = modes[i];
        int j = i;
        while (j > 0 && modes[j - 1].w * modes[j - 1].h < t.w * t.h) {
            modes[j] = modes[j - 1];
            j--;
        }
        modes[j] = t;
    }
    return n;
}

static void drm_wait_flip(struct drm_out *o)
{
    for (int i = 0; i < 20 && o->flip_pending; i++) {
        struct pollfd p = { .fd = o->d->fd, .events = POLLIN };
        if (poll(&p, 1, 50) > 0)
            drm_event_cb(o->d->fd, 0, o->d);
    }
}

static int drm_apply_mode(struct lm_output *out)
{
    struct drm_out *o = out->backend_data;
    struct drm *d = o->d;
    if (!d->active || !o->live)
        return -1;
    drmModeConnector *conn = drmModeGetConnectorCurrent(d->fd, o->conn_id);
    if (!conn || conn->connection != DRM_MODE_CONNECTED || conn->count_modes <= 0) {
        if (conn)
            drmModeFreeConnector(conn);
        return -1;
    }
    drmModeModeInfo pref = preferred_mode(conn);
    struct lm_edid_native nat;
    bool have_nat = conn_native(d->fd, conn, &nat);
    drmModeModeInfo m = pick_mode(&d->base.server->config, conn, &pref, have_nat ? &nat : NULL, false);
    uint32_t mm_w = conn->mmWidth, mm_h = conn->mmHeight;
    drmModeFreeConnector(conn);
    if (same_timing(&m, &o->mode))
        return 0;

    o->busy = true;
    drm_wait_flip(o);
    struct drm_buf nb[2];
    memset(nb, 0, sizeof(nb));
    if (buf_create(d, &nb[0], m.hdisplay, m.vdisplay) < 0 || buf_create(d, &nb[1], m.hdisplay, m.vdisplay) < 0) {
        buf_destroy(d, &nb[0]);
        buf_destroy(d, &nb[1]);
        o->busy = false;
        return -1;
    }
    struct drm_buf old[2] = { o->buf[0], o->buf[1] };
    drmModeModeInfo old_mode = o->mode;
    int old_back = o->back;
    uint32_t *old_shadow = o->shadow;
    size_t old_shadow_size = o->shadow_size;
    bool old_shadow_valid = o->shadow_valid;
    o->shadow = NULL;
    if (shadow_alloc(o, nb[0].size) < 0) {
        o->shadow = old_shadow;
        o->shadow_size = old_shadow_size;
        o->shadow_valid = old_shadow_valid;
        buf_destroy(d, &nb[0]);
        buf_destroy(d, &nb[1]);
        o->busy = false;
        return -1;
    }
    o->buf[0] = nb[0];
    o->buf[1] = nb[1];
    o->mode = m;
    o->back = 0;
    if (out_modeset(o) < 0) {                                           /* refused: put everything back */
        o->buf[0] = old[0];
        o->buf[1] = old[1];
        o->mode = old_mode;
        o->back = old_back;
        shadow_free(o);
        o->shadow = old_shadow;
        o->shadow_size = old_shadow_size;
        o->shadow_valid = old_shadow_valid;
        buf_destroy(d, &nb[0]);
        buf_destroy(d, &nb[1]);
        o->busy = false;
        return -1;
    }
    free(old_shadow);
    buf_destroy(d, &old[0]);
    buf_destroy(d, &old[1]);
    o->seq = 0;
    out->width = (int32_t)m.hdisplay;
    out->height = (int32_t)m.vdisplay;
    if (m.htotal && m.vtotal && m.clock)
        out->refresh_mhz = (int32_t)(((uint64_t)m.clock * 1000000ull) / ((uint64_t)m.htotal * m.vtotal));
    if (mm_w) {                                                         /* (else keep the size the EDID gave) */
        out->phys_width_mm = (int32_t)mm_w;
        out->phys_height_mm = (int32_t)mm_h;
    }
    lm_info("drm: %s now %ux%u@%.2f", out->name, m.hdisplay, m.vdisplay, out->refresh_mhz / 1000.0);
    o->busy = false;
    if (d->base.listener.output_changed)
        d->base.listener.output_changed(d->base.listener.data, out);
    lm_timer_arm_ms(d->ready_timer, 1, 0);
    return 0;
}

/* The connector's DPMS property (0 on, 3 off); 0 when the driver has none. */
static uint32_t conn_dpms_prop(int fd, uint32_t conn_id)
{
    uint32_t found = 0;
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
    if (!props)
        return 0;
    for (uint32_t i = 0; i < props->count_props && !found; i++) {
        drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
        if (p && !strcmp(p->name, "DPMS"))
            found = p->prop_id;
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return found;
}

static int drm_set_power(struct lm_backend *b, bool on)
{
    struct drm *d = (struct drm *)b;
    if (!d->active || !d->is_master)
        return -1;                          /* another VT owns the display now; it comes back through vt_acquire */
    int rc = 0;
    struct lm_output *lo;
    lm_list_for_each(lo, &b->outputs, link) {
        struct drm_out *o = (struct drm_out *)lo;
        uint32_t prop = conn_dpms_prop(d->fd, o->conn_id);
        if (!prop || drmModeConnectorSetProperty(d->fd, o->conn_id, prop, on ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF) < 0) {
            lm_warn("drm: cannot switch %s %s: %s", o->base.name, on ? "on" : "off", prop ? strerror(errno) : "no DPMS property");
            rc = -1;
        }
        o->flip_pending = false;
    }
    if (on)
        lm_timer_arm_ms(d->ready_timer, 1, 0);      /* the frame loop stood still while the picture was not shown */
    lm_info("drm: monitors %s", on ? "on" : "in standby");
    return rc;
}

static const struct lm_backend_ops drm_ops = {
    .name = "drm",
    .init = drm_init,
    .destroy = drm_destroy,
    .get_framebuffer = drm_get_framebuffer,
    .present = drm_present,
    .switch_vt = drm_switch_vt,
    .list_modes = drm_list_modes,
    .apply_mode = drm_apply_mode,
    .set_power = drm_set_power,
};

struct lm_backend *lm_backend_drm_create(struct lm_server *srv)
{
    struct drm *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->base.ops = &drm_ops;
    d->base.server = srv;
    lm_list_init(&d->base.outputs);
    d->fd = -1;
    d->tty_fd = -1;
    d->uevent_fd = -1;
    return &d->base;
}
