/*
 * wl_shm, wl_shm_pool, wl_buffer and the private pixel textures surfaces keep.
 *
 * (Client shm memory is never mmap()ed; dmabufs - GPU memory - are, see below.) A pool keeps a private dup of the client's
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

#ifdef __SSE2__
#include <emmintrin.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/dma-buf.h>

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
    if (b->dmabuf) {
        if (b->dmabuf->map)
            munmap((void *)b->dmabuf->map, b->dmabuf->size);
        if (b->dmabuf->fd >= 0)
            close(b->dmabuf->fd);
        free(b->dmabuf);
    }
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

/* ---- dmabuf reads ------------------------------------------------------------------------------------------------ */

/*
 * GPU memory is mapped (never pread) and read after DMA_BUF_IOCTL_SYNC, which also waits for the GPU to finish drawing into
 * it. Reading write-combined memory with plain loads is slow, so on x86 the copy uses SSE4.1 streaming loads (movntdqa) when
 * the CPU has them. A dmabuf can in principle vanish under us (SIGBUS): the reads run under a small guard that turns the
 * signal into an error (-2) instead of a crash; for any other SIGBUS the previous handler gets it.
 */
static sigjmp_buf g_bus_jmp;
static volatile sig_atomic_t g_bus_armed;
static struct sigaction g_bus_old;
static bool g_bus_installed;

static void bus_handler(int sig, siginfo_t *si, void *ctx)
{
    if (g_bus_armed) {
        g_bus_armed = 0;
        siglongjmp(g_bus_jmp, 1);
    }
    sigaction(SIGBUS, &g_bus_old, NULL);                   /* not ours: hand it back (crash handlers, default action) */
    (void)si;
    (void)ctx;
    raise(sig);
}

static void bus_guard_install(void)
{
    if (g_bus_installed)
        return;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = bus_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGBUS, &sa, &g_bus_old) == 0)
        g_bus_installed = true;
}

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
__attribute__((target("sse4.1")))
static void stream_copy_sse41(uint8_t *dst, const uint8_t *src, size_t n)
{
    while (n && ((uintptr_t)src & 15)) {                   /* head up to a 16-byte boundary of the source */
        *dst++ = *src++;
        n--;
    }
    while (n >= 64) {
        __m128i a = _mm_stream_load_si128((__m128i *)(void *)src);
        __m128i b = _mm_stream_load_si128((__m128i *)(void *)(src + 16));
        __m128i c = _mm_stream_load_si128((__m128i *)(void *)(src + 32));
        __m128i d = _mm_stream_load_si128((__m128i *)(void *)(src + 48));
        _mm_storeu_si128((__m128i *)(void *)dst, a);
        _mm_storeu_si128((__m128i *)(void *)(dst + 16), b);
        _mm_storeu_si128((__m128i *)(void *)(dst + 32), c);
        _mm_storeu_si128((__m128i *)(void *)(dst + 48), d);
        dst += 64;
        src += 64;
        n -= 64;
    }
    while (n) {
        *dst++ = *src++;
        n--;
    }
}
#endif

static void gpu_copy(void *dst, const void *src, size_t n)
{
#if defined(__x86_64__) || defined(__i386__)
    static int sse41 = -1;
    if (sse41 < 0)
        sse41 = __builtin_cpu_supports("sse4.1") ? 1 : 0;
    if (sse41 && n >= 64) {
        stream_copy_sse41(dst, src, n);
        return;
    }
#endif
    memcpy(dst, src, n);
}

static void dmabuf_sync(const struct lm_dmabuf_fd *d, bool start)
{
    struct dma_buf_sync sync = { .flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_READ };
    int r;
    do
        r = ioctl(d->fd, DMA_BUF_IOCTL_SYNC, &sync);
    while (r < 0 && errno == EINTR);
}

/* Reads `len` bytes at `off` of the buffer: pread() from an shm pool, a streaming copy from a mapped dmabuf. */
static int buf_read(const struct lm_buffer *b, void *dst, size_t len, off_t off)
{
    if (b->dmabuf) {
        const struct lm_dmabuf_fd *d = b->dmabuf;
        if (!d->map || off < 0 || (size_t)off > d->size || len > d->size - (size_t)off)
            return -1;
        gpu_copy(dst, d->map + off, len);
        return 0;
    }
    return read_full(b->pool->fd, dst, len, off);
}

/*
 * Make pixels valid premultiplied ARGB. XRGB: force alpha. ARGB: clamp the
 * colour channels to alpha so a hostile client can never make the renderer's
 * blend arithmetic overflow.
 */
/* One translucent pixel made valid: premultiplied, colour channels never above alpha. */
static inline uint32_t fix_one(uint32_t p)
{
    uint32_t a = p >> 24;
    if (a == 0xff)
        return p;
    if (a == 0)
        return 0;
    uint32_t r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, bl = p & 0xff;
    if (r > a)
        r = a;
    if (g > a)
        g = a;
    if (bl > a)
        bl = a;
    return (a << 24) | (r << 16) | (g << 8) | bl;
}

/* Returns true when the buffer had pixels with alpha != 0xff (always false for an opaque format). */
static bool fix_pixels(uint32_t *px, size_t n, bool opaque)
{
    size_t i = 0;
    bool translucent = false;
#ifdef __SSE2__
    const __m128i amask = _mm_set1_epi32((int)0xff000000u);
    if (opaque) {
        for (; i + 4 <= n; i += 4) {
            __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(px + i));
            _mm_storeu_si128((__m128i *)(void *)(px + i), _mm_or_si128(v, amask));
        }
    } else {                                                            /* alpha 0xff everywhere is by far the usual case */
        for (; i + 4 <= n; i += 4) {
            __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(px + i));
            if (_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(v, amask), amask))) == 0xf)
                continue;
            translucent = true;
            for (size_t k = i; k < i + 4; k++)
                px[k] = fix_one(px[k]);
        }
    }
#endif
    if (opaque) {
        for (; i < n; i++)
            px[i] |= 0xff000000u;
        return false;
    }
    for (; i < n; i++) {
        if ((px[i] >> 24) != 0xff)
            translucent = true;
        px[i] = fix_one(px[i]);
    }
    return translucent;
}

#if defined(__x86_64__) || defined(__i386__)
/*
 * Streaming copy from a dmabuf mapping with the alpha fix done on the way: the pixels pass through registers once
 * instead of being written to the texture and read back by a second fix_pixels() pass (that second pass was 22 % of
 * the compositor's time with a GPU-rendering Firefox).
 */
__attribute__((target("sse4.1")))
static void stream_copy_fix_sse41(uint32_t *dst, const uint32_t *src, size_t n, bool opaque, bool *translucent)
{
    const __m128i amask = _mm_set1_epi32((int)0xff000000u);
    size_t i = 0;
    for (; i < n && ((uintptr_t)(src + i) & 15); i++) {                 /* head up to a 16-byte boundary of the source */
        uint32_t p = src[i];
        if (opaque) {
            dst[i] = p | 0xff000000u;
        } else {
            if ((p >> 24) != 0xff)
                *translucent = true;
            dst[i] = fix_one(p);
        }
    }
    for (; i + 16 <= n; i += 16) {
        __m128i a = _mm_stream_load_si128((__m128i *)(void *)(src + i));
        __m128i b = _mm_stream_load_si128((__m128i *)(void *)(src + i + 4));
        __m128i c = _mm_stream_load_si128((__m128i *)(void *)(src + i + 8));
        __m128i d = _mm_stream_load_si128((__m128i *)(void *)(src + i + 12));
        if (opaque) {
            a = _mm_or_si128(a, amask);
            b = _mm_or_si128(b, amask);
            c = _mm_or_si128(c, amask);
            d = _mm_or_si128(d, amask);
        }
        _mm_storeu_si128((__m128i *)(void *)(dst + i), a);
        _mm_storeu_si128((__m128i *)(void *)(dst + i + 4), b);
        _mm_storeu_si128((__m128i *)(void *)(dst + i + 8), c);
        _mm_storeu_si128((__m128i *)(void *)(dst + i + 12), d);
        if (!opaque) {                                                  /* alpha 0xff everywhere is by far the usual case */
            __m128i all = _mm_and_si128(_mm_and_si128(a, b), _mm_and_si128(c, d));
            if (_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(all, amask), amask))) != 0xf) {
                for (size_t k = i; k < i + 16; k++) {
                    if ((dst[k] >> 24) != 0xff)
                        *translucent = true;
                    dst[k] = fix_one(dst[k]);
                }
            }
        }
    }
    for (; i < n; i++) {
        uint32_t p = src[i];
        if (opaque) {
            dst[i] = p | 0xff000000u;
        } else {
            if ((p >> 24) != 0xff)
                *translucent = true;
            dst[i] = fix_one(p);
        }
    }
}
#endif

/* buf_read() of `n` pixels plus fix_pixels(); for a mapped dmabuf both happen in one pass. */
static int buf_read_fix(const struct lm_buffer *b, uint32_t *dst, size_t n, off_t off, bool opaque, bool *translucent)
{
#if defined(__x86_64__) || defined(__i386__)
    if (b->dmabuf && (off & 3) == 0 && __builtin_cpu_supports("sse4.1")) {
        const struct lm_dmabuf_fd *d = b->dmabuf;
        size_t len = n * 4;
        if (!d->map || off < 0 || (size_t)off > d->size || len > d->size - (size_t)off)
            return -1;
        stream_copy_fix_sse41(dst, (const uint32_t *)(const void *)(d->map + off), n, opaque, translucent);
        return 0;
    }
#endif
    if (buf_read(b, dst, n * 4, off) < 0)
        return -1;
    *translucent |= fix_pixels(dst, n, opaque);
    return 0;
}


/*
 * Copy the rect (x, y, w, h), already clipped to the buffer, from the pool
 * into `dst` (a w-wide row of which starts at dst + row * dst_stride_px).
 * `dst` points at the top-left pixel of the rect in the texture.
 */
static int copy_rect(const struct lm_buffer *b, int x, int y, int w, int h,
                     uint32_t *dst, size_t dst_stride_px, bool opaque, bool *translucent)
{
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
            if (buf_read_fix(b, d, n * (size_t)w, base + (off_t)r * b->stride, opaque, translucent) < 0)
                return -2;
        }
        return 0;
    }
    for (int r = 0; r < h; r++) {
        uint32_t *d = dst + (size_t)r * dst_stride_px;
        if (buf_read_fix(b, d, (size_t)w, base + (off_t)r * b->stride, opaque, translucent) < 0)
            return -2;
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
                            uint32_t transform, bool opaque, bool *translucent)
{
    size_t row_bytes = (size_t)b->width * 4;
    uint32_t *row = malloc(row_bytes);
    if (!row)
        return -1;
    int rc = 0;
    for (int32_t by = 0; by < b->height; by++) {
        off_t off = (off_t)b->offset + (off_t)by * b->stride;
        if (buf_read_fix(b, row, (size_t)b->width, off, opaque, translucent) < 0) {
            rc = -2;
            break;
        }
        int64_t base, step;
        transform_row_map(transform, tex->width, tex->height, by, &base, &step);
        for (int32_t bx = 0; bx < b->width; bx++)
            tex->pixels[base + (int64_t)bx * step] = row[bx];
    }
    free(row);
    return rc;
}

static int buffer_copy_inner(struct lm_buffer *b, struct lm_texture *tex, uint32_t transform, const struct lm_region *damage);

int lm_buffer_copy(struct lm_buffer *b, struct lm_texture *tex, uint32_t transform,
                   const struct lm_region *damage)
{
    if (!b || (!b->pool && !b->dmabuf) || !tex || b->width <= 0 || b->height <= 0)
        return -1;
    if (!b->dmabuf)
        return buffer_copy_inner(b, tex, transform, damage);
    struct lm_dmabuf_fd *d = b->dmabuf;
    if (!d->map) {                                                  /* first use: map it (kept until the buffer goes) */
        void *m = mmap(NULL, d->size, PROT_READ, MAP_SHARED, d->fd, 0);
        if (m == MAP_FAILED)
            return -2;
        d->map = m;
    }
    bus_guard_install();
    int rc;
    dmabuf_sync(d, true);                                           /* waits for the GPU's pending writes */
    if (sigsetjmp(g_bus_jmp, 1) == 0) {
        g_bus_armed = 1;
        rc = buffer_copy_inner(b, tex, transform, damage);
        g_bus_armed = 0;
    } else {
        rc = -2;                                                    /* SIGBUS: the memory went away */
        if (tex->pixels && tex->width > 0 && tex->height > 0)
            memset(tex->pixels, 0, (size_t)tex->width * (size_t)tex->height * sizeof(uint32_t));
    }
    dmabuf_sync(d, false);
    return rc;
}

static int buffer_copy_inner(struct lm_buffer *b, struct lm_texture *tex, uint32_t transform, const struct lm_region *damage)
{
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
            bool tr = false;
            int rc = copy_rect(b, r.x, r.y, r.w, r.h,
                               tex->pixels + (size_t)r.y * (size_t)bw + (size_t)r.x,
                               (size_t)bw, opaque, &tr);
            if (rc < 0)
                return rc;
            if (tr)
                tex->solid = false;                              /* sticky until the next full copy */
        }
        return 0;
    }

    if (lm_texture_resize(tex, tw, th) < 0)
        return -1;
    tex->opaque = opaque;

    int rc;
    bool tr = false;
    if (transform == LM_TRANSFORM_NORMAL)
        rc = copy_rect(b, 0, 0, bw, bh, tex->pixels, (size_t)bw, opaque, &tr);
    else
        rc = copy_transformed(b, tex, transform, opaque, &tr);
    tex->solid = rc == 0 && !tr;
    if (rc == -2) /* never expose stale heap contents */
        memset(tex->pixels, 0, (size_t)tw * (size_t)th * sizeof(uint32_t));
    return rc;
}

/* ---- dmabuf buffers (created by dmabuf.c) -------------------------------------------------------------------------- */

struct lm_buffer *lm_buffer_create_dmabuf(struct lm_client *client, uint32_t id, uint32_t version, int fd, size_t size,
                                          int32_t width, int32_t height, int32_t stride, int32_t offset, bool opaque,
                                          uint64_t modifier)
{
    struct lm_buffer *b = calloc(1, sizeof(*b));
    struct lm_dmabuf_fd *d = calloc(1, sizeof(*d));
    if (!b || !d) {
        free(b);
        free(d);
        close(fd);
        lm_client_post_no_memory(client);
        return NULL;
    }
    d->fd = fd;
    d->size = size;
    d->modifier = modifier;
    b->dmabuf = d;
    b->width = width;
    b->height = height;
    b->stride = stride;
    b->offset = offset;
    b->format = opaque ? LM_WL_SHM_FORMAT_XRGB8888 : LM_WL_SHM_FORMAT_ARGB8888;
    b->refs = 1;                                                    /* held by the wl_buffer resource */
    struct lm_resource *br = id ? lm_resource_create(client, id, &lm_wl_buffer_interface, version, buffer_handlers, b,
                                                     buffer_resource_destroy)
                                : lm_resource_create_server(client, &lm_wl_buffer_interface, version, buffer_handlers, b,
                                                            buffer_resource_destroy);
    if (!br) {
        lm_buffer_unref(b);                                         /* closes the fd */
        return NULL;
    }
    b->resource = br;
    return b;
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
