/*
 * gl.h - the little OpenGL ES 2 layer under the GL renderer. No headers and no link-time dependency: libEGL, libGLESv2 (and
 * libgbm for scanout) are dlopen()ed at start and the few functions and constants needed are declared here / in gl.c.
 * When anything is missing lm_gl_create() fails with a reason and the software renderer stays in charge.
 *
 * Coordinates are output pixels with the origin top-left. Textures hold premultiplied ARGB8888 as the rest of MellaGUI
 * does (bytes B, G, R, A in memory: uploaded as GL_BGRA_EXT), so a surface's texture memory can be uploaded as it is.
 *
 * Two kinds of render target:
 *   - an offscreen target (framebuffer object): headless backend, tests; lm_gl_readback() copies it to XRGB8888 memory;
 *   - a window surface made by the DRM backend for a GBM surface (lm_gl_*_window); the image is then upright in memory
 *     without a flip (GL's origin is bottom-left, the buffer's first row is the top: the vertex shader takes care).
 */
#ifndef MELLAGUI_GL_H
#define MELLAGUI_GL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct lm_gl;

/* Opens EGL. `gbm_fd` >= 0: the platform is GBM on that DRM device (scanout); -1: Mesa's surfaceless platform (headless,
 * offscreen only). NULL with a reason in err when it cannot be done (no libraries, no GPU / driver, too old). */
struct lm_gl *lm_gl_create(int gbm_fd, char *err, size_t errsz);
void lm_gl_destroy(struct lm_gl *gl);
const char *lm_gl_renderer_name(const struct lm_gl *gl);     /* GL_RENDERER, e.g. "Mesa Intel(R) HD Graphics 4000 (IVB GT2)" */
bool lm_gl_make_current(struct lm_gl *gl);

/* ---- textures ------------------------------------------------------------------------------------------------------- */

/* A w x h texture, filled from `bgra` (premultiplied ARGB8888, tightly packed) when not NULL. 0 on failure. */
unsigned lm_gl_tex_create(struct lm_gl *gl, int w, int h, const void *bgra);
/* Replaces a rectangle: `src` points at the pixel (x, y) of an image with `stride_px` pixels per row. */
void lm_gl_tex_update(struct lm_gl *gl, unsigned tex, int x, int y, int w, int h, const void *src, int stride_px);
void lm_gl_tex_delete(struct lm_gl *gl, unsigned tex);

/* ---- offscreen target ----------------------------------------------------------------------------------------------- */

struct lm_gl_target {
    unsigned fbo, tex;
    int w, h;
};
bool lm_gl_target_create(struct lm_gl *gl, int w, int h, struct lm_gl_target *t);
void lm_gl_target_destroy(struct lm_gl *gl, struct lm_gl_target *t);
/* Draw into it from now on. A NULL target = the window surface that is current (DRM). */
void lm_gl_bind_target(struct lm_gl *gl, const struct lm_gl_target *t, int w, int h);
/* Copies the target to XRGB8888 (0x00RRGGBB, native endian) memory, `stride` in bytes. */
bool lm_gl_readback(struct lm_gl *gl, const struct lm_gl_target *t, uint32_t *dst, size_t stride);

/* ---- window surfaces (DRM scanout through GBM) --------------------------------------------------------------------------- */

/* A GBM surface with an EGL window surface on it: what one display output renders into. Needs lm_gl_create(gbm_fd >= 0). */
struct lm_gl_window;
struct lm_gl_bo {                       /* a finished frame: what to put on a CRTC */
    uint32_t handle, stride;            /* GEM handle on the GBM device (the DRM fd) and bytes per row */
    int w, h;
    void *priv;                         /* gbm_bo, for lm_gl_window_release */
};
/* `scanout`: the buffers must be usable by KMS (GBM_BO_USE_SCANOUT) as well as for rendering. NULL on failure. */
struct lm_gl_window *lm_gl_window_create(struct lm_gl *gl, int w, int h, bool scanout);
void lm_gl_window_destroy(struct lm_gl *gl, struct lm_gl_window *win);
/* Make the window current and draw into it from now on (lm_gl_clear / lm_gl_draw_* ...). */
bool lm_gl_window_bind(struct lm_gl *gl, struct lm_gl_window *win);
/* eglSwapBuffers, then take the new front buffer. Every bo must be given back (lm_gl_window_release) once it is off screen;
 * a surface only has a few. false: swap failed / no free buffer. */
bool lm_gl_window_swap(struct lm_gl *gl, struct lm_gl_window *win, struct lm_gl_bo *bo);
void lm_gl_window_release(struct lm_gl *gl, struct lm_gl_window *win, struct lm_gl_bo *bo);
/* Copies a bo to XRGB8888 memory (tests; maps the buffer). */
bool lm_gl_bo_read(struct lm_gl *gl, const struct lm_gl_bo *bo, uint32_t *dst, size_t stride);

/* ---- drawing -------------------------------------------------------------------------------------------------------- */

void lm_gl_clear(struct lm_gl *gl, float r, float g, float b, float a);
/* Rectangle of `tex` (u0..u1, v0..v1 in 0..1) onto dst (x, y, w, h) of the bound target; `alpha` scales the premultiplied
 * texel (1 = as it is); `opaque` skips blending. Nearest-neighbour sampling. */
void lm_gl_draw_tex(struct lm_gl *gl, unsigned tex, int x, int y, int w, int h, float u0, float v0, float u1, float v1,
                    float alpha, bool opaque);
/* Premultiplied colour rectangle. */
void lm_gl_draw_rect(struct lm_gl *gl, int x, int y, int w, int h, float r, float g, float b, float a);
void lm_gl_scissor(struct lm_gl *gl, bool on, int x, int y, int w, int h);   /* y from the top, like everything else */
void lm_gl_flush(struct lm_gl *gl);                           /* glFlush */
void lm_gl_finish(struct lm_gl *gl);                          /* glFinish */

#endif
