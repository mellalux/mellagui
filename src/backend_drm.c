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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "backend.h"
#include "log.h"
#include "seatdev.h"

#define HOTPLUG_POLL_MS 2000
#define SIG_VT_RELEASE SIGUSR2
#define SIG_VT_ACQUIRE (SIGRTMIN)

struct drm;

struct drm_buf {
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
    int back;                   /* buffer the next frame is drawn into */
    bool flip_pending;
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
    struct lm_timer *hotplug;
    struct lm_timer *ready_timer;   /* "ready" after (re)activation */
    int tty_fd;
    struct lm_source *sig_release, *sig_acquire;
    bool vt_setup;
    bool is_master;
    bool active;
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
    return 0;
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
    if (o->live && b->listener.output_ready)
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
    buf_destroy(d, &o->buf[0]);
    buf_destroy(d, &o->buf[1]);
    free(o);
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
    o->mode = conn->modes[0];
    for (int i = 0; i < conn->count_modes; i++)
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            o->mode = conn->modes[i];
            break;
        }
    o->saved = drmModeGetCrtc(d->fd, crtc);

    uint32_t w = o->mode.hdisplay, h = o->mode.vdisplay;
    if (buf_create(d, &o->buf[0], w, h) < 0 || buf_create(d, &o->buf[1], w, h) < 0)
        goto fail;
    o->back = 0;
    if (out_modeset(o) < 0)
        goto fail;

    struct lm_output *lo = &o->base;
    lo->backend = &d->base;
    snprintf(lo->name, sizeof(lo->name), "%s-%u", connector_type_name(conn->connector_type),
             conn->connector_type_id);
    snprintf(lo->make, sizeof(lo->make), "DRM");
    snprintf(lo->model, sizeof(lo->model), "%ux%u", w, h);
    lo->width = (int32_t)w;
    lo->height = (int32_t)h;
    uint32_t vr = o->mode.vrefresh ? o->mode.vrefresh : 60;
    if (o->mode.htotal && o->mode.vtotal && o->mode.clock)
        lo->refresh_mhz = (int32_t)(((uint64_t)o->mode.clock * 1000000ull) /
                                    ((uint64_t)o->mode.htotal * o->mode.vtotal));
    else
        lo->refresh_mhz = (int32_t)vr * 1000;
    lo->phys_width_mm = (int32_t)conn->mmWidth;
    lo->phys_height_mm = (int32_t)conn->mmHeight;
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

/* Returns number of outputs after the scan. */
static int probe_outputs(struct drm *d)
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
        drmModeConnector *c = drmModeGetConnector(d->fd, o->conn_id);
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
        drmModeConnector *c = drmModeGetConnector(d->fd, id);
        if (!c)
            continue;
        if (c->connection == DRM_MODE_CONNECTED && d->active) {
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
    int before = lm_list_length(&d->base.outputs);
    probe_outputs(d);
    (void)before;
}

/* ---- frame protocol -------------------------------------------------------------------------- */

static struct lm_framebuffer *drm_get_framebuffer(struct lm_output *out)
{
    struct drm_out *o = out->backend_data;
    struct drm *d = o->d;
    if (!d->active || o->flip_pending || !o->live)
        return NULL;
    struct drm_buf *b = &o->buf[o->back];
    o->fb.pixels = b->map;
    o->fb.width = out->width;
    o->fb.height = out->height;
    o->fb.stride = (int32_t)b->pitch;
    o->fb.age = b->seq ? (int)(o->seq + 1 - b->seq) : 0;
    if (o->fb.age < 0)
        o->fb.age = 0;
    return &o->fb;
}

static int drm_present(struct lm_output *out, const struct lm_rect *damage, int n)
{
    struct drm_out *o = out->backend_data;
    struct drm *d = o->d;
    (void)damage;
    (void)n;
    if (!d->active || !o->live)
        return -1;
    struct drm_buf *b = &o->buf[o->back];
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

static void vt_release(int signo, void *data)
{
    struct drm *d = data;
    (void)signo;
    if (!d->active) {
        ioctl(d->tty_fd, VT_RELDISP, 1);
        return;
    }
    lm_info("VT: switching away");
    d->active = false;
    d->base.session_active = false;
    if (d->base.listener.session_active)
        d->base.listener.session_active(d->base.listener.data, false);
    if (d->is_master) {
        drmDropMaster(d->fd);
        d->is_master = false;
    }
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
    d->active = true;
    d->base.session_active = true;
    struct lm_output *lo;
    lm_list_for_each(lo, &d->base.outputs, link) {
        struct drm_out *o = (struct drm_out *)lo;
        o->flip_pending = false;
        o->buf[0].seq = o->buf[1].seq = 0;   /* contents are unknown now: force full redraws */
        out_modeset(o);
    }
    if (d->base.listener.session_active)
        d->base.listener.session_active(d->base.listener.data, true);
    lm_timer_arm_ms(d->ready_timer, 1, 0);
}

static int vt_setup(struct drm *d)
{
    d->tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (d->tty_fd < 0) {
        lm_error("drm: cannot open /dev/tty (%s). Start Lumen from a Linux virtual console "
                 "(switch with Ctrl+Alt+F3 and log in), not from a terminal emulator or ssh.",
                 strerror(errno));
        return -1;
    }
    struct vt_stat st;
    if (ioctl(d->tty_fd, VT_GETSTATE, &st) < 0) {
        lm_error("drm: %s is not a Linux virtual console. Start Lumen from a text VT "
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
    d->vt_setup = true;
    return 0;
}

static int drm_switch_vt(struct lm_backend *b, int vt)
{
    struct drm *d = DRM(b);
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
    int fd = lm_seatdev_open(d->base.server->seatdev, path, O_RDWR | O_NONBLOCK, NULL);
    if (fd < 0) {
        if (fd == -EACCES)
            lm_error("drm: no permission to open %s. Add yourself to group 'video' "
                     "(sudo usermod -aG video $USER, then log in again) or run from a "
                     "logged-in console session.", path);
        else
            lm_debug("drm: cannot open %s: %s", path, strerror(-fd));
        return fd;
    }
    uint64_t dumb = 0;
    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &dumb) < 0 || !dumb) {
        lm_debug("drm: %s has no dumb buffer support", path);
        lm_seatdev_close(d->base.server->seatdev, fd);
        return -ENOTSUP;
    }
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        lm_debug("drm: %s has no KMS resources", path);
        lm_seatdev_close(d->base.server->seatdev, fd);
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
        lm_seatdev_close(d->base.server->seatdev, fd);
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

    if (vt_setup(d) < 0)
        return -1;
    if (select_device(d, srv->config.drm_device) < 0)
        return -1;
    lm_info("drm: using %s", d->path);

    if (drmSetMaster(d->fd) < 0) {
        lm_error("drm: cannot become DRM master on %s: %s. Another display server "
                 "(X, Wayland compositor) probably owns the GPU on this VT.", d->path, strerror(errno));
        return -1;
    }
    d->is_master = true;
    d->active = true;
    b->session_active = true;

    if (probe_outputs(d) <= 0) {
        lm_error("drm: no output could be set up on %s", d->path);
        return -1;
    }
    d->drm_source = lm_loop_add_fd(srv->loop, d->fd, EPOLLIN, drm_event_cb, d);
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
    if (d->ready_timer)
        lm_timer_destroy(d->ready_timer);
    if (d->drm_source)
        lm_source_remove(d->drm_source);
    lm_list_for_each_safe(lo, tmp, &b->outputs, link)
        out_destroy((struct drm_out *)lo, true);
    if (d->fd >= 0) {
        if (d->is_master)
            drmDropMaster(d->fd);
        lm_seatdev_close(b->server->seatdev, d->fd);
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

static const struct lm_backend_ops drm_ops = {
    .name = "drm",
    .init = drm_init,
    .destroy = drm_destroy,
    .get_framebuffer = drm_get_framebuffer,
    .present = drm_present,
    .switch_vt = drm_switch_vt,
};

struct lm_backend *lm_backend_drm_create(struct lm_server *srv, const char *device)
{
    struct drm *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->base.ops = &drm_ops;
    d->base.server = srv;
    lm_list_init(&d->base.outputs);
    d->fd = -1;
    d->tty_fd = -1;
    (void)device; /* taken from srv->config.drm_device in init */
    return &d->base;
}
