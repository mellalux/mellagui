/*
 * wl_shm, wl_shm_pool, wl_buffer and the private pixel textures surfaces keep.
 *
 * Client memory is never mmap()ed. A pool keeps a private dup of the client's
 * fd and every access is a pread(2) into the surface texture, so a client that
 * truncates its pool file (the classic SIGBUS attack on compositors) simply
 * produces a short read, which lm_buffer_copy() reports as -2. No signal
 * handler (and therefore no clash with the fatal-signal handlers of the DRM
 * backend) is needed. The conversion to premultiplied ARGB happens in place on
 * the destination row right after the read.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "log.h"
#include "protocol.h"
#include "server.h"
#include "shm.h"

#ifndef O_PATH
#define O_PATH 010000000
#endif

#define LM_SHM_MAX_POOL    (1024 * 1024 * 1024)  /* 1 GiB */
#define LM_SHM_MAX_DIM     16384
#define LM_SHM_READ_CHUNK  (1u << 20)            /* bytes per pread for contiguous copies */

struct lm_shm {
    struct lm_server *srv;
    struct lm_global *global;
};

struct lm_shm_pool {
    int refs;
    int fd;             /* private dup, closed with the last reference */
    size_t size;        /* bytes, only ever grows */
};

/* ---- pool ------------------------------------------------------------------- */

static void pool_unref(struct lm_shm_pool *pool)
{
    if (!pool || --pool->refs > 0)
        return;
    if (pool->fd >= 0)
        close(pool->fd);
    free(pool);
}

static void pool_resource_destroy(struct lm_resource *res)
{
    pool_unref(res->data);
    res->data = NULL;
}

/* ---- buffer ----------------------------------------------------------------- */

struct lm_buffer *lm_buffer_from_resource(struct lm_resource *res)
{
    if (!res || res->iface != &lm_wl_buffer_interface)
        return NULL;
    return res->data;
}

void lm_buffer_ref(struct lm_buffer *b)
{
    if (b)
        b->refs++;
}

void lm_buffer_unref(struct lm_buffer *b)
{
    if (!b || --b->refs > 0)
        return;
    pool_unref(b->pool);
    free(b);
}

void lm_buffer_release(struct lm_buffer *b)
{
    if (b && b->resource)
        lm_resource_post(b->resource, LM_WL_BUFFER_EVT_RELEASE);
}

static void buffer_resource_destroy(struct lm_resource *res)
{
    struct lm_buffer *b = res->data;
    res->data = NULL;
    if (!b)
        return;
    b->resource = NULL;
    lm_buffer_unref(b);
}

static void buffer_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static const lm_request_fn buffer_handlers[] = {
    [LM_WL_BUFFER_REQ_DESTROY] = buffer_req_destroy,
};

/* ---- texture ---------------------------------------------------------------- */

int lm_texture_resize(struct lm_texture *tex, int32_t w, int32_t h)
{
    if (!tex || w <= 0 || h <= 0 || w > LM_SHM_MAX_DIM || h > LM_SHM_MAX_DIM)
        return -1;
    if (tex->pixels && tex->width == w && tex->height == h) {
        tex->stride = w * 4;
        return 0;
    }
    size_t bytes;
    if (__builtin_mul_overflow((size_t)w, (size_t)h, &bytes) ||
        __builtin_mul_overflow(bytes, sizeof(uint32_t), &bytes))
        return -1;
    uint32_t *px = malloc(bytes);
    if (!px)
        return -1;
    free(tex->pixels);
    tex->pixels = px;
    tex->width = w;
    tex->height = h;
    tex->stride = w * 4;
    return 0;
}

void lm_texture_free(struct lm_texture *tex)
{
    if (!tex)
        return;
    free(tex->pixels);
    memset(tex, 0, sizeof(*tex));
}

/* ---- pixel copy ------------------------------------------------------------ */

/* pread() the whole range or fail (EOF/short file included). */
static int read_full(int fd, void *buf, size_t len, off_t off)
{
    uint8_t *p = buf;
    while (len > 0) {
        size_t want = len > LM_SHM_READ_CHUNK ? LM_SHM_READ_CHUNK : len;
        ssize_t n = pread(fd, p, want, off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        p += n;
        off += n;
        len -= (size_t)n;
    }
    return 0;
}

/*
 * Make pixels valid premultiplied ARGB. XRGB: force alpha. ARGB: clamp the
 * colour channels to alpha so a hostile client can never make the renderer's
 * blend arithmetic overflow.
 */
static void fix_pixels(uint32_t *px, size_t n, bool opaque)
{
    if (opaque) {
        for (size_t i = 0; i < n; i++)
            px[i] |= 0xff000000u;
        return;
    }
    for (size_t i = 0; i < n; i++) {
        uint32_t p = px[i];
        uint32_t a = p >> 24;
        if (a == 0xff)
            continue;
        if (a == 0) {
            px[i] = 0;
            continue;
        }
        uint32_t r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, bl = p & 0xff;
        if (r > a)
            r = a;
        if (g > a)
            g = a;
        if (bl > a)
            bl = a;
        px[i] = (a << 24) | (r << 16) | (g << 8) | bl;
    }
}

/*
 * Copy the rect (x, y, w, h), already clipped to the buffer, from the pool
 * into `dst` (a w-wide row of which starts at dst + row * dst_stride_px).
 * `dst` points at the top-left pixel of the rect in the texture.
 */
static int copy_rect(const struct lm_buffer *b, int x, int y, int w, int h,
                     uint32_t *dst, size_t dst_stride_px, bool opaque)
{
    int fd = b->pool->fd;
    off_t base = (off_t)b->offset + (off_t)y * b->stride + (off_t)x * 4;
    size_t row_bytes = (size_t)w * 4;

    if (dst_stride_px == (size_t)w && (size_t)b->stride == row_bytes) {
        /* contiguous in both places: a few big reads */
        size_t rows_per = LM_SHM_READ_CHUNK / row_bytes;
        if (rows_per == 0)
            rows_per = 1;
        for (int r = 0; r < h; r += (int)rows_per) {
            size_t n = (size_t)(h - r) < rows_per ? (size_t)(h - r) : rows_per;
            uint32_t *d = dst + (size_t)r * w;
            if (read_full(fd, d, n * row_bytes, base + (off_t)r * b->stride) < 0)
                return -2;
            fix_pixels(d, n * (size_t)w, opaque);
        }
        return 0;
    }
    for (int r = 0; r < h; r++) {
        uint32_t *d = dst + (size_t)r * dst_stride_px;
        if (read_full(fd, d, row_bytes, base + (off_t)r * b->stride) < 0)
            return -2;
        fix_pixels(d, (size_t)w, opaque);
    }
    return 0;
}

/*
 * Transformed full copy. For source pixel (bx, by) the destination index is
 * base + bx * step (tw x th = texture size); derived from weston's
 * surface -> buffer coordinate mapping.
 */
static void transform_row_map(uint32_t transform, int32_t tw, int32_t th, int32_t by,
                              int64_t *base, int64_t *step)
{
    switch (transform) {
    case LM_TRANSFORM_FLIPPED:
        *base = (int64_t)by * tw + (tw - 1);
        *step = -1;
        break;
    case LM_TRANSFORM_90:
        *base = (int64_t)(th - 1) * tw + by;
        *step = -(int64_t)tw;
        break;
    case LM_TRANSFORM_FLIPPED_90:
        *base = (int64_t)(th - 1) * tw + (tw - 1 - by);
        *step = -(int64_t)tw;
        break;
    case LM_TRANSFORM_180:
        *base = (int64_t)(th - 1 - by) * tw + (tw - 1);
        *step = -1;
        break;
    case LM_TRANSFORM_FLIPPED_180:
        *base = (int64_t)(th - 1 - by) * tw;
        *step = 1;
        break;
    case LM_TRANSFORM_270:
        *base = tw - 1 - by;
        *step = tw;
        break;
    case LM_TRANSFORM_FLIPPED_270:
        *base = by;
        *step = tw;
        break;
    case LM_TRANSFORM_NORMAL:
    default:
        *base = (int64_t)by * tw;
        *step = 1;
        break;
    }
}

static int copy_transformed(const struct lm_buffer *b, struct lm_texture *tex,
                            uint32_t transform, bool opaque)
{
    size_t row_bytes = (size_t)b->width * 4;
    uint32_t *row = malloc(row_bytes);
    if (!row)
        return -1;
    int rc = 0;
    for (int32_t by = 0; by < b->height; by++) {
        off_t off = (off_t)b->offset + (off_t)by * b->stride;
        if (read_full(b->pool->fd, row, row_bytes, off) < 0) {
            rc = -2;
            break;
        }
        fix_pixels(row, (size_t)b->width, opaque);
        int64_t base, step;
        transform_row_map(transform, tex->width, tex->height, by, &base, &step);
        for (int32_t bx = 0; bx < b->width; bx++)
            tex->pixels[base + (int64_t)bx * step] = row[bx];
    }
    free(row);
    return rc;
}

int lm_buffer_copy(struct lm_buffer *b, struct lm_texture *tex, uint32_t transform,
                   const struct lm_region *damage)
{
    if (!b || !b->pool || !tex || b->width <= 0 || b->height <= 0)
        return -1;
    if (transform > LM_TRANSFORM_FLIPPED_270)
        transform = LM_TRANSFORM_NORMAL;

    bool opaque = b->format == LM_WL_SHM_FORMAT_XRGB8888;
    int32_t bw = b->width, bh = b->height;
    bool swapped = transform & 1;
    int32_t tw = swapped ? bh : bw;
    int32_t th = swapped ? bw : bh;

    /* Partial update only makes sense on top of a texture with identical layout. */
    if (damage && transform == LM_TRANSFORM_NORMAL && tex->pixels && tex->width == bw &&
        tex->height == bh && tex->opaque == opaque) {
        const struct lm_rect whole = { 0, 0, bw, bh };
        for (int32_t i = 0; i < damage->n; i++) {
            struct lm_rect r;
            if (!lm_rect_intersect(&damage->rects[i], &whole, &r))
                continue;
            int rc = copy_rect(b, r.x, r.y, r.w, r.h,
                               tex->pixels + (size_t)r.y * (size_t)bw + (size_t)r.x,
                               (size_t)bw, opaque);
            if (rc < 0)
                return rc;
        }
        return 0;
    }

    if (lm_texture_resize(tex, tw, th) < 0)
        return -1;
    tex->opaque = opaque;

    int rc;
    if (transform == LM_TRANSFORM_NORMAL)
        rc = copy_rect(b, 0, 0, bw, bh, tex->pixels, (size_t)bw, opaque);
    else
        rc = copy_transformed(b, tex, transform, opaque);
    if (rc == -2) /* never expose stale heap contents */
        memset(tex->pixels, 0, (size_t)tw * (size_t)th * sizeof(uint32_t));
    return rc;
}

/* ---- wl_shm_pool ---------------------------------------------------------------- */

static bool valid_format(uint32_t format)
{
    return format == LM_WL_SHM_FORMAT_ARGB8888 || format == LM_WL_SHM_FORMAT_XRGB8888;
}

static void pool_req_create_buffer(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_shm_pool *pool = res->data;
    uint32_t id = args[0].n;
    int32_t offset = args[1].i, width = args[2].i, height = args[3].i, stride = args[4].i;
    uint32_t format = args[5].u;

    if (!pool)
        return;
    if (!valid_format(format)) {
        lm_resource_post_error(res, LM_WL_SHM_ERR_INVALID_FORMAT,
                               "invalid format 0x%x", format);
        return;
    }
    if (offset < 0 || width <= 0 || height <= 0 || width > LM_SHM_MAX_DIM ||
        height > LM_SHM_MAX_DIM || stride < 0 || (stride & 3) ||
        (int64_t)stride < (int64_t)width * 4) {
        lm_resource_post_error(res, LM_WL_SHM_ERR_INVALID_STRIDE,
                               "invalid width, height or stride (%dx%d, %d)", width,
                               height, stride);
        return;
    }
    /* offset + (height - 1) * stride + width * 4 must fit in the pool (64-bit, no overflow) */
    int64_t end = (int64_t)offset + (int64_t)(height - 1) * stride + (int64_t)width * 4;
    if (end > (int64_t)pool->size) {
        lm_resource_post_error(res, LM_WL_SHM_ERR_INVALID_STRIDE,
                               "buffer extends past the end of the pool (%lld > %zu)",
                               (long long)end, pool->size);
        return;
    }

    struct lm_buffer *b = calloc(1, sizeof(*b));
    if (!b) {
        lm_resource_post_no_memory(res);
        return;
    }
    b->pool = pool;
    pool->refs++;
    b->width = width;
    b->height = height;
    b->stride = stride;
    b->offset = offset;
    b->format = format;
    b->refs = 1; /* held by the wl_buffer resource */

    struct lm_resource *br = lm_resource_create(res->client, id, &lm_wl_buffer_interface,
                                                res->version, buffer_handlers, b,
                                                buffer_resource_destroy);
    if (!br) {
        lm_buffer_unref(b); /* also drops the pool reference */
        return;
    }
    b->resource = br;
}

static void pool_req_destroy(struct lm_resource *res, const union lm_arg *args)
{
    (void)args;
    lm_resource_destroy(res);
}

static void pool_req_resize(struct lm_resource *res, const union lm_arg *args)
{
    struct lm_shm_pool *pool = res->data;
    int32_t size = args[0].i;

    if (!pool)
        return;
    if (size <= 0 || (int64_t)size > LM_SHM_MAX_POOL) {
        lm_resource_post_error(res, LM_WL_SHM_ERR_INVALID_FD, "invalid pool size %d", size);
        return;
    }
    if ((size_t)size < pool->size) {
        lm_resource_post_error(res, LM_WL_SHM_ERR_INVALID_FD, "shrinking a pool is invalid");
        return;
    }
    pool->size = (size_t)size;
}

static const lm_request_fn pool_handlers[] = {
    [LM_WL_SHM_POOL_REQ_CREATE_BUFFER] = pool_req_create_buffer,
    [LM_WL_SHM_POOL_REQ_DESTROY] = pool_req_destroy,
    [LM_WL_SHM_POOL_REQ_RESIZE] = pool_req_resize,
};

/* ---- wl_shm ------------------------------------------------------------------------ */

static void shm_req_create_pool(struct lm_resource *res, const union lm_arg *args)
{
    uint32_t id = args[0].n;
    int fd = args[1].h;
    int32_t size = args[2].i;

    if (size <= 0) {
        lm_resource_post_error(res, LM_WL_SHM_ERR_INVALID_STRIDE, "invalid pool size %d", size);
        return;
    }
    if ((int64_t)size > LM_SHM_MAX_POOL) {
        lm_resource_post_error(res, LM_WL_SHM_ERR_INVALID_FD, "pool too large (%d bytes)", size);
        return;
    }
    struct stat st;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || (fl & O_PATH) || (fl & O_ACCMODE) == O_WRONLY || fstat(fd, &st) < 0 ||
        !S_ISREG(st.st_mode)) {
        lm_resource_post_error(res, LM_WL_SHM_ERR_INVALID_FD,
                               "pool fd must be a readable regular file or memfd");
        return;
    }

    struct lm_shm_pool *pool = calloc(1, sizeof(*pool));
    if (!pool) {
        lm_resource_post_no_memory(res);
        return;
    }
    pool->fd = fcntl(fd, F_DUPFD_CLOEXEC, 3); /* the server closes args[1].h after we return */
    if (pool->fd < 0) {
        free(pool);
        lm_resource_post_no_memory(res);
        return;
    }
    pool->refs = 1; /* held by the wl_shm_pool resource */
    pool->size = (size_t)size;

    struct lm_resource *pr = lm_resource_create(res->client, id, &lm_wl_shm_pool_interface,
                                                res->version, pool_handlers, pool,
                                                pool_resource_destroy);
    if (!pr)
        pool_unref(pool);
}

static const lm_request_fn shm_handlers[] = {
    [LM_WL_SHM_REQ_CREATE_POOL] = shm_req_create_pool,
};

static void shm_bind(struct lm_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct lm_resource *res = lm_resource_create(client, id, &lm_wl_shm_interface,
                                                 version, shm_handlers, NULL, NULL);
    if (!res)
        return;
    lm_resource_post(res, LM_WL_SHM_EVT_FORMAT, (uint32_t)LM_WL_SHM_FORMAT_ARGB8888);
    lm_resource_post(res, LM_WL_SHM_EVT_FORMAT, (uint32_t)LM_WL_SHM_FORMAT_XRGB8888);
}

struct lm_shm *lm_shm_create(struct lm_server *srv)
{
    struct lm_shm *shm = calloc(1, sizeof(*shm));
    if (!shm)
        return NULL;
    shm->srv = srv;
    shm->global = lm_global_create(srv, &lm_wl_shm_interface, LM_WL_SHM_VERSION, shm_bind, shm);
    if (!shm->global) {
        free(shm);
        return NULL;
    }
    return shm;
}

void lm_shm_destroy(struct lm_shm *shm)
{
    if (!shm)
        return;
    if (shm->global)
        lm_global_destroy(shm->global);
    free(shm);
}
