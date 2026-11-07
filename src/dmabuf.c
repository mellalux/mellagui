#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "dmabuf.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/dma-buf.h>

#include "gl.h"
#include "log.h"
#include "protocol.h"
#include "render.h"
#include "server.h"
#include "shm.h"

#define MAX_DIM 16384

#define MAX_FMTS 32

struct lm_dmabuf {
    struct { uint32_t fourcc; uint64_t modifier; } fmts[MAX_FMTS];     /* the format table: what clients may choose from */
    int nfmts;
    struct lm_server *srv;
    struct lm_global *global;
    dev_t main_device;
    int table_fd;                       /* memfd with the format table */
    uint32_t table_size;
};

/* ---- the GPU's render node ------------------------------------------------------------------------------------- */

static bool same_gpu(const char *a, const char *b)
{
    char pa[PATH_MAX], pb[PATH_MAX], ra[PATH_MAX], rb[PATH_MAX];
    snprintf(pa, sizeof(pa), "/sys/class/drm/%s/device", a);
    snprintf(pb, sizeof(pb), "/sys/class/drm/%s/device", b);
    return realpath(pa, ra) && realpath(pb, rb) && !strcmp(ra, rb);
}

/* The render node to name as the main device: the one of the GPU that drives the display (the DRM backend's card), else the
 * first usable one. False when there is none. */
static bool find_render_node(const struct lm_server *srv, dev_t *out)
{
    const char *want = getenv("MELLAGUI_DMABUF_DEVICE");                  /* tests / odd setups: a path like /dev/dri/renderD129 */
    struct stat st;
    if (want && *want) {
        if (stat(want, &st) == 0 && S_ISCHR(st.st_mode)) {
            *out = st.st_rdev;
            return true;
        }
        return false;
    }
    char card[32] = "";
    const char *dev = srv->config.drm_device;                          /* --drm-device /dev/dri/cardN, else the first card */
    if (dev && strrchr(dev, '/'))
        snprintf(card, sizeof(card), "%s", strrchr(dev, '/') + 1);
    DIR *d = opendir("/dev/dri");
    if (!d)
        return false;
    struct dirent *e;
    char first[32] = "", match[32] = "";
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "renderD", 7) || strlen(e->d_name) >= 32)
            continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/%s", e->d_name);
        if (access(path, R_OK | W_OK) != 0)
            continue;
        if (!first[0] || strcmp(e->d_name, first) < 0)
            snprintf(first, sizeof(first), "%s", e->d_name);
        if (card[0] && same_gpu(card, e->d_name))
            snprintf(match, sizeof(match), "%s", e->d_name);
    }
    closedir(d);
    const char *pick = match[0] ? match : first;
    if (!pick[0])
        return false;
    char path[64];
    snprintf(path, sizeof(path), "/dev/dri/%s", pick);
    if (stat(path, &st) != 0)
        return false;
    *out = st.st_rdev;
    return true;
}

/* The formats and layouts to offer: ARGB8888 and XRGB8888, linear always (the CPU can read those), and with the GL renderer every
 * single-plane layout the GPU can sample from (tiled ones are fast for the GPU that rendered them, and cannot be read by the CPU). */
static void fill_formats(struct lm_dmabuf *d)
{
    static const uint32_t fourccs[] = { LM_DRM_FORMAT_ARGB8888, LM_DRM_FORMAT_XRGB8888 };
    struct lm_gl *gl = lm_render_gl(d->srv->render);
    for (size_t i = 0; i < sizeof(fourccs) / sizeof(fourccs[0]); i++) {
        uint64_t mods[16];
        int n = gl ? lm_gl_dmabuf_modifiers(gl, fourccs[i], mods, 16) : 0;
        bool have_linear = false;
        for (int k = 0; k < n && d->nfmts < MAX_FMTS; k++) {
            d->fmts[d->nfmts].fourcc = fourccs[i];
            d->fmts[d->nfmts++].modifier = mods[k];
            have_linear = have_linear || mods[k] == LM_DRM_FORMAT_MOD_LINEAR;
        }
        if (!have_linear && d->nfmts < MAX_FMTS) {
            d->fmts[d->nfmts].fourcc = fourccs[i];
            d->fmts[d->nfmts++].modifier = LM_DRM_FORMAT_MOD_LINEAR;
        }
    }
}

static bool format_offered(const struct lm_dmabuf *d, uint32_t fourcc, uint64_t modifier)
{
    for (int i = 0; i < d->nfmts; i++)
        if (d->fmts[i].fourcc == fourcc && d->fmts[i].modifier == modifier)
            return true;
    return false;
}

static int make_table(const struct lm_dmabuf *d, uint32_t *size)
{
    int fd = memfd_create("mellagui-dmabuf-formats", MFD_CLOEXEC);
    if (fd < 0)
        return -1;
    uint8_t buf[16 * MAX_FMTS];
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < d->nfmts; i++) {
        memcpy(buf + 16 * i, &d->fmts[i].fourcc, 4);
        memcpy(buf + 16 * i + 8, &d->fmts[i].modifier, 8);
    }
    *size = (uint32_t)(16 * d->nfmts);
    if (write(fd, buf, *size) != (ssize_t)*size) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- feedback --------------------------------------------------------------------------------------------------- */

static void feedback_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static const lm_request_fn feedback_handlers[] = {
    [LM_ZWP_LINUX_DMABUF_FEEDBACK_V1_REQ_DESTROY] = feedback_req_destroy,
};

static void send_feedback(struct lm_dmabuf *d, struct lm_resource *fb)
{
    struct lm_array dev = { sizeof(d->main_device), &d->main_device };
    uint16_t idx[MAX_FMTS];
    for (int i = 0; i < d->nfmts; i++)
        idx[i] = (uint16_t)i;
    struct lm_array formats = { sizeof(idx[0]) * (size_t)d->nfmts, idx };
    lm_resource_post(fb, LM_ZWP_LINUX_DMABUF_FEEDBACK_V1_EVT_FORMAT_TABLE, d->table_fd, d->table_size);
    lm_resource_post(fb, LM_ZWP_LINUX_DMABUF_FEEDBACK_V1_EVT_MAIN_DEVICE, &dev);
    lm_resource_post(fb, LM_ZWP_LINUX_DMABUF_FEEDBACK_V1_EVT_TRANCHE_TARGET_DEVICE, &dev);
    lm_resource_post(fb, LM_ZWP_LINUX_DMABUF_FEEDBACK_V1_EVT_TRANCHE_FORMATS, &formats);
    lm_resource_post(fb, LM_ZWP_LINUX_DMABUF_FEEDBACK_V1_EVT_TRANCHE_FLAGS, 0u);
    lm_resource_post(fb, LM_ZWP_LINUX_DMABUF_FEEDBACK_V1_EVT_TRANCHE_DONE);
    lm_resource_post(fb, LM_ZWP_LINUX_DMABUF_FEEDBACK_V1_EVT_DONE);
}

/* ---- zwp_linux_buffer_params_v1 ------------------------------------------------------------------------------- */

struct params {
    struct lm_dmabuf *d;
    bool used;
    bool has_plane;
    int fd;                             /* our dup of the plane's fd */
    uint32_t offset, stride;
    uint64_t modifier;
};

static void params_destroy_cb(struct lm_resource *res)
{
    struct params *p = res->data;
    res->data = NULL;
    if (!p)
        return;
    if (p->fd >= 0)
        close(p->fd);
    free(p);
}

static void params_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void params_req_add(struct lm_resource *res, const union lm_arg *args)
{
    struct params *p = res->data;
    int fd = args[0].h;
    uint32_t plane = args[1].u, offset = args[2].u, stride = args[3].u;
    uint64_t modifier = ((uint64_t)args[4].u << 32) | args[5].u;
    if (!p)
        return;
    if (p->used) {
        lm_resource_post_error(res, LM_ZWP_LINUX_BUFFER_PARAMS_ERR_ALREADY_USED, "the params were already used to create a buffer");
        return;
    }
    if (plane != 0) {
        lm_resource_post_error(res, LM_ZWP_LINUX_BUFFER_PARAMS_ERR_PLANE_IDX, "plane index %u is out of range (one plane only)", plane);
        return;
    }
    if (p->has_plane) {
        lm_resource_post_error(res, LM_ZWP_LINUX_BUFFER_PARAMS_ERR_PLANE_SET, "plane 0 was already set");
        return;
    }
    p->fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);                          /* the server closes args[0].h after we return */
    if (p->fd < 0) {
        lm_resource_post_no_memory(res);
        return;
    }
    p->has_plane = true;
    p->offset = offset;
    p->stride = stride;
    p->modifier = modifier;
}

/* Validates the parameters and creates the wl_buffer. `id` 0: server-allocated (create). Returns false when the import is
 * refused; `*fatal` says it is a protocol error the caller must raise (and which one in *code). */
static bool params_make_buffer(struct params *p, struct lm_resource *res, uint32_t id, int32_t width, int32_t height,
                               uint32_t format, uint32_t flags, uint32_t *code, const char **why)
{
    *code = LM_ZWP_LINUX_BUFFER_PARAMS_ERR_INVALID_WL_BUFFER;
    *why = "";
    if (!p->has_plane) {
        *code = LM_ZWP_LINUX_BUFFER_PARAMS_ERR_INCOMPLETE;
        *why = "no plane was added";
        return false;
    }
    if (width <= 0 || height <= 0 || width > MAX_DIM || height > MAX_DIM) {
        *code = LM_ZWP_LINUX_BUFFER_PARAMS_ERR_INVALID_DIMENSIONS;
        *why = "invalid buffer size";
        return false;
    }
    if (format != LM_DRM_FORMAT_ARGB8888 && format != LM_DRM_FORMAT_XRGB8888) {
        *code = LM_ZWP_LINUX_BUFFER_PARAMS_ERR_INVALID_FORMAT;
        *why = "format not offered (ARGB8888 and XRGB8888 only)";
        return false;
    }
    if (flags != 0 || !format_offered(p->d, format, p->modifier)) {   /* y-invert/interlaced and layouts that were not offered */
        *why = flags ? "flags are not supported" : "that layout (modifier) was not offered";
        return false;
    }
    if (p->stride < (uint32_t)width * 4 || (p->stride & 3) || p->offset > INT32_MAX || p->stride > INT32_MAX) {
        *code = LM_ZWP_LINUX_BUFFER_PARAMS_ERR_OUT_OF_BOUNDS;
        *why = "invalid stride or offset";
        return false;
    }
    /* A real dmabuf only: anything else (a regular file the client could truncate) is refused. The sync ioctl is what tells. */
    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
    if (ioctl(p->fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
        *why = "the file descriptor is not a dmabuf";
        return false;
    }
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ioctl(p->fd, DMA_BUF_IOCTL_SYNC, &sync);
    off_t size = lseek(p->fd, 0, SEEK_END);
    if (size <= 0) {
        *why = "cannot size the dmabuf";
        return false;
    }
    int64_t end = (int64_t)p->offset + (int64_t)(height - 1) * p->stride + (int64_t)width * 4;
    if (end > (int64_t)size) {
        *code = LM_ZWP_LINUX_BUFFER_PARAMS_ERR_OUT_OF_BOUNDS;
        *why = "the buffer extends past the end of the dmabuf";
        return false;
    }
    int fd = fcntl(p->fd, F_DUPFD_CLOEXEC, 3);                      /* the buffer owns its own dup */
    if (fd < 0) {
        lm_resource_post_no_memory(res);
        *code = 0;
        return false;
    }
    struct lm_buffer *b = lm_buffer_create_dmabuf(res->client, id, res->version, fd, (size_t)size, width, height,
                                                  (int32_t)p->stride, (int32_t)p->offset, format == LM_DRM_FORMAT_XRGB8888,
                                                  p->modifier);
    if (!b) {
        *code = 0;                                                  /* the client has been told */
        return false;
    }
    /* `created` needs the resource */
    if (!id)
        lm_resource_post(res, LM_ZWP_LINUX_BUFFER_PARAMS_V1_EVT_CREATED, b->resource);
    return true;
}

static void params_create_common(struct lm_resource *res, uint32_t id, const union lm_arg *wh)
{
    struct params *p = res->data;
    if (!p)
        return;
    if (p->used) {
        lm_resource_post_error(res, LM_ZWP_LINUX_BUFFER_PARAMS_ERR_ALREADY_USED, "the params were already used to create a buffer");
        return;
    }
    p->used = true;
    uint32_t code;
    const char *why;
    if (params_make_buffer(p, res, id, wh[0].i, wh[1].i, wh[2].u, wh[3].u, &code, &why))
        return;
    if (code == 0)
        return;                                                     /* out of memory, already reported */
    if (id) {                                                       /* create_immed: any failure is a protocol error */
        lm_resource_post_error(res, code, "dmabuf import failed: %s", why);
        return;
    }
    if (code == LM_ZWP_LINUX_BUFFER_PARAMS_ERR_INVALID_WL_BUFFER)   /* create: an import that is simply refused */
        lm_resource_post(res, LM_ZWP_LINUX_BUFFER_PARAMS_V1_EVT_FAILED);
    else
        lm_resource_post_error(res, code, "%s", why);
}

static void params_req_create(struct lm_resource *res, const union lm_arg *args)
{
    params_create_common(res, 0, args);
}

static void params_req_create_immed(struct lm_resource *res, const union lm_arg *args)
{
    params_create_common(res, args[0].n, args + 1);
}

static const lm_request_fn params_handlers[] = {
    [LM_ZWP_LINUX_BUFFER_PARAMS_V1_REQ_DESTROY] = params_req_destroy,
    [LM_ZWP_LINUX_BUFFER_PARAMS_V1_REQ_ADD] = params_req_add,
    [LM_ZWP_LINUX_BUFFER_PARAMS_V1_REQ_CREATE] = params_req_create,
    [LM_ZWP_LINUX_BUFFER_PARAMS_V1_REQ_CREATE_IMMED] = params_req_create_immed,
};

/* ---- zwp_linux_dmabuf_v1 ---------------------------------------------------------------------------------------- */

static void dmabuf_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void dmabuf_req_create_params(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_dmabuf *d = res->data;
    struct params *p = calloc(1, sizeof(*p));
    if (!p || !d) {
        free(p);
        lm_resource_post_no_memory(res);
        return;
    }
    p->d = d;
    p->fd = -1;
    if (!lm_resource_create(res->client, args[0].n, &lm_zwp_linux_buffer_params_v1_interface, res->version, params_handlers, p,
                            params_destroy_cb))
        free(p);
}

static void dmabuf_feedback(struct lm_resource *res, uint32_t id)
{
    struct lm_dmabuf *d = res->data;
    if (!d)
        return;
    struct lm_resource *fb = lm_resource_create(res->client, id, &lm_zwp_linux_dmabuf_feedback_v1_interface, res->version,
                                                feedback_handlers, NULL, NULL);
    if (fb)
        send_feedback(d, fb);
}

static void dmabuf_req_get_default_feedback(struct lm_resource *res, const union lm_arg *args)
{
    dmabuf_feedback(res, args[0].n);
}

static void dmabuf_req_get_surface_feedback(struct lm_resource *res, const union lm_arg *args)
{
    dmabuf_feedback(res, args[0].n);                                /* one device, one set of formats: the same for every surface */
}

static const lm_request_fn dmabuf_handlers[] = {
    [LM_ZWP_LINUX_DMABUF_V1_REQ_DESTROY] = dmabuf_req_destroy,
    [LM_ZWP_LINUX_DMABUF_V1_REQ_CREATE_PARAMS] = dmabuf_req_create_params,
    [LM_ZWP_LINUX_DMABUF_V1_REQ_GET_DEFAULT_FEEDBACK] = dmabuf_req_get_default_feedback,
    [LM_ZWP_LINUX_DMABUF_V1_REQ_GET_SURFACE_FEEDBACK] = dmabuf_req_get_surface_feedback,
};

static void dmabuf_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    struct lm_dmabuf *d = data;
    struct lm_resource *res = lm_resource_create(client, id, &lm_zwp_linux_dmabuf_v1_interface, version, dmabuf_handlers, d, NULL);
    if (!res)
        return;
    uint32_t last_plain = 0;
    for (int i = 0; i < d->nfmts; i++) {                            /* versions before 4 learn the formats from events */
        if (version == 3)
            lm_resource_post(res, LM_ZWP_LINUX_DMABUF_V1_EVT_MODIFIER, d->fmts[i].fourcc,
                             (uint32_t)(d->fmts[i].modifier >> 32), (uint32_t)(d->fmts[i].modifier & 0xffffffffu));
        else if (version < 3 && d->fmts[i].fourcc != last_plain) {
            lm_resource_post(res, LM_ZWP_LINUX_DMABUF_V1_EVT_FORMAT, d->fmts[i].fourcc);
            last_plain = d->fmts[i].fourcc;
        }
    }
}

struct lm_dmabuf *lm_dmabuf_create(struct lm_server *srv, bool enabled)
{
    if (!enabled)
        return NULL;
    struct lm_dmabuf *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->srv = srv;
    d->table_fd = -1;
    if (!find_render_node(srv, &d->main_device)) {
        lm_info("dmabuf: no render node (/dev/dri/renderD*): clients use wl_shm");
        free(d);
        return NULL;
    }
    fill_formats(d);
    d->table_fd = make_table(d, &d->table_size);
    if (d->table_fd < 0) {
        free(d);
        return NULL;
    }
    d->global = lm_global_create(srv, &lm_zwp_linux_dmabuf_v1_interface, LM_ZWP_LINUX_DMABUF_V1_VERSION, dmabuf_bind, d);
    if (!d->global) {
        lm_dmabuf_destroy(d);
        return NULL;
    }
    int tiled = 0;
    for (int i = 0; i < d->nfmts; i++)
        tiled += d->fmts[i].modifier != LM_DRM_FORMAT_MOD_LINEAR;
    lm_info("dmabuf: zwp_linux_dmabuf_v1 on render node %u:%u (ARGB8888/XRGB8888, %d layouts%s)", major(d->main_device),
            minor(d->main_device), d->nfmts, tiled ? ", tiled ones sampled by the GPU" : ": linear only");
    return d;
}

void lm_dmabuf_destroy(struct lm_dmabuf *d)
{
    if (!d)
        return;
    if (d->global)
        lm_global_destroy(d->global);
    if (d->table_fd >= 0)
        close(d->table_fd);
    free(d);
}
