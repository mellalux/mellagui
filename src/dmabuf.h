#ifndef LUMEN_DMABUF_H
#define LUMEN_DMABUF_H

/*
 * zwp_linux_dmabuf_v1 (version 4): lets clients render with the GPU (Mesa's EGL, Firefox's WebRender) and hand Lumen the
 * finished frames as dmabufs. Lumen composites in software, so it only needs to READ them: the pixels are mapped and copied
 * into the surface's texture on commit (shm.c: mmap + DMA_BUF_IOCTL_SYNC + streaming loads), exactly like an shm buffer.
 *
 * What is offered: ARGB8888 and XRGB8888 with the LINEAR modifier only (linear memory can be read by the CPU without
 * detiling); one plane; no flags. The default feedback names the render node of the display's GPU as the main device. When
 * the machine has no render node (/dev/dri/renderD*) the global is not created at all and clients fall back to wl_shm.
 */

#include <stdbool.h>

struct lm_server;
struct lm_dmabuf;

/* Creates and advertises the global; NULL (and no global) when there is no render node or `enabled` is false. */
struct lm_dmabuf *lm_dmabuf_create(struct lm_server *srv, bool enabled);
void lm_dmabuf_destroy(struct lm_dmabuf *d);

#endif
