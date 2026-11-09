#ifndef MELLAGUI_SHM_H
#define MELLAGUI_SHM_H

/*
 * wl_shm, wl_shm_pool, wl_buffer and the pixel "texture" a surface keeps.
 * Owner: shm.c.
 *
 * Design: a surface NEVER keeps reading a client's shared memory after
 * commit. On commit, surface.c calls lm_buffer_copy() which copies (and
 * converts to premultiplied ARGB8888, applying the buffer transform) the
 * damaged part into the surface's own struct lm_texture, then immediately
 * releases the wl_buffer. The renderer therefore only reads private
 * malloc'd memory and never needs SIGBUS protection; all SIGBUS handling
 * (client truncated the pool file) lives inside lm_buffer_copy().
 *
 * Hostile-input rules (shm.c): pool size, offset, width, height, stride are
 * validated with overflow-checked arithmetic before use. Limits: pool size
 * <= 1 GiB, width/height in 1..16384, stride >= width*4 and a multiple of 4.
 * Violations are wl_shm errors (invalid_stride / invalid_format / invalid_fd)
 * and disconnect.
 */

#include <stdbool.h>
#include <stdint.h>
#include "server.h"
#include "util.h"

struct lm_shm_pool; /* private to shm.c, refcounted */

/* A client's dmabuf (linux-dmabuf): GPU-rendered pixels read through mmap + DMA_BUF_IOCTL_SYNC (see dmabuf.c). */
struct lm_dmabuf_fd {
    int fd;                         /* private dup, closed with the buffer */
    size_t size;                    /* bytes of the object (lseek SEEK_END) */
    const uint8_t *map;             /* mapped on first use, read-only */
    uint64_t modifier;              /* DRM format modifier of the layout; 0 = linear (the only one the CPU can read) */
};

struct lm_buffer {
    struct lm_resource *resource;   /* the wl_buffer; NULL once the client destroyed it */
    struct lm_shm_pool *pool;       /* wl_shm buffers */
    struct lm_dmabuf_fd *dmabuf;    /* linux-dmabuf buffers (pool is NULL then) */
    int32_t width, height;
    int32_t stride;                 /* bytes */
    int32_t offset;                 /* bytes into the pool */
    uint32_t format;                /* LM_WL_SHM_FORMAT_* */
    int refs;                       /* the struct outlives the wl_buffer while ref'd */
    void *priv;
};

/* Pixel storage owned by a surface: premultiplied ARGB8888, native endian. */
struct lm_texture {
    uint32_t *pixels;               /* malloc'd, width*height, tightly packed */
    int32_t width, height;
    int32_t stride;                 /* bytes (== width * 4) */
    bool opaque;                    /* source format had no alpha (alpha forced to 0xff) */
    bool solid;                     /* every pixel has alpha 0xff: opaque format, or an alpha format (Firefox, GTK) that never used it
                                     * since the last full copy (sticky: one translucent pixel keeps it false until then) */
};

/* Creates and advertises the wl_shm global (v1). */
struct lm_shm *lm_shm_create(struct lm_server *srv);
void lm_shm_destroy(struct lm_shm *shm);

/*
 * A wl_buffer for a dmabuf (dmabuf.c has validated everything): `id` 0 = create it with a server-allocated id (the `created`
 * event of zwp_linux_buffer_params_v1), else the client's id (create_immed). Takes ownership of `fd` on success AND on failure.
 * XRGB8888 when `opaque`, else ARGB8888. NULL on failure (the client has been told when it was its fault).
 */
struct lm_buffer *lm_buffer_create_dmabuf(struct lm_client *client, uint32_t id, uint32_t version, int fd, size_t size,
                                          int32_t width, int32_t height, int32_t stride, int32_t offset, bool opaque,
                                          uint64_t modifier);

/* The lm_buffer behind a wl_buffer resource (resource->data), or NULL. */
struct lm_buffer *lm_buffer_from_resource(struct lm_resource *res);

void lm_buffer_ref(struct lm_buffer *b);
/* Drops a reference; frees the buffer (and unrefs the pool) at zero. NULL ok. */
void lm_buffer_unref(struct lm_buffer *b);
/* Sends wl_buffer.release if the resource is still alive. */
void lm_buffer_release(struct lm_buffer *b);

/*
 * Copy pixels from `b` into `tex` (resizing it when needed) applying
 * `transform` (LM_TRANSFORM_*): tex gets the buffer size, with width/height
 * swapped for 90/270 transforms. If `damage` is non-NULL, the tex size is
 * unchanged and transform == NORMAL, only those rects (buffer coordinates,
 * clipped to the buffer) are copied; otherwise the whole buffer is copied.
 * Returns 0, -1 on OOM, -2 if the pool memory became inaccessible (SIGBUS):
 * the caller then posts wl_shm.error invalid_fd on the wl_buffer.
 */
int lm_buffer_copy(struct lm_buffer *b, struct lm_texture *tex, uint32_t transform,
                   const struct lm_region *damage);

/*
 * Writes `px` (w x h, XRGB8888, tightly packed) into the top left corner of a wl_shm buffer of the client (pwrite on the
 * pool's fd: a client that truncated its pool gets an error, not a crash). Opaque pixels, so XRGB8888 and ARGB8888 buffers both
 * hold them as they are (alpha forced to 0xff). The buffer must be at least w x h. Used by capture.c. Returns 0 or -1.
 */
int lm_buffer_write(const struct lm_buffer *b, const uint32_t *px, int32_t w, int32_t h);

/* Resize tex storage to w*h (contents unspecified). Overflow-checked. 0 or -1. */
int lm_texture_resize(struct lm_texture *tex, int32_t w, int32_t h);
void lm_texture_free(struct lm_texture *tex);

#endif
