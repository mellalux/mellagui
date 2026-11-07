/*
 * gl-test - src/gl.c against a real (or Mesa software) EGL: surfaceless context, an offscreen target, textures, blending and
 * the readback. Prints "gl-test ok"; exit status 77 = no usable OpenGL ES here (the test is skipped).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "gl.h"

static int bad;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)

static uint32_t px(const uint32_t *p, int w, int x, int y) { return p[y * w + x] & 0x00ffffffu; }

/* The GBM / window surface path on a render node: what the DRM backend will do per output, minus the KMS part. */
static int gbm_test(void)
{
    char err[200];
    int fd = open(getenv("GL_TEST_DEVICE") ? getenv("GL_TEST_DEVICE") : "/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("no render node\n");
        return 77;
    }
    struct lm_gl *gl = lm_gl_create(fd, err, sizeof(err));
    if (!gl) {
        printf("no GBM / EGL: %s\n", err);
        return 77;
    }
    enum { W = 64, H = 48 };
    struct lm_gl_window *win = lm_gl_window_create(gl, W, H, getenv("GL_TEST_SCANOUT") != NULL);
    CHECK(win, "window");
    if (!win)
        return 1;
    uint32_t *out = calloc(W * H, 4);
    for (int frame = 0; frame < 3; frame++) {                      /* several swaps: the buffers have to be handed back and forth */
        CHECK(lm_gl_window_bind(gl, win), "bind");
        lm_gl_clear(gl, 0, 0, 0, 1);
        lm_gl_draw_rect(gl, 0, 0, 8, 4, 0.0f, 1.0f, 0.0f, 1.0f);                  /* green, top left corner */
        lm_gl_draw_rect(gl, 10 + frame, 20, 6, 6, 1.0f, 0.0f, 0.0f, 1.0f);        /* red */
        uint32_t px_blue = 0xff0000ffu;
        unsigned t = lm_gl_tex_create(gl, 1, 1, &px_blue);
        lm_gl_draw_tex(gl, t, W - 5, H - 3, 5, 3, 0, 0, 1, 1, 1.0f, true);       /* blue, bottom right corner */
        lm_gl_tex_delete(gl, t);
        struct lm_gl_bo bo;
        CHECK(lm_gl_window_swap(gl, win, &bo), "swap %d", frame);
        CHECK(bo.w == W && bo.h == H && bo.stride >= W * 4, "bo %dx%d stride %u", bo.w, bo.h, bo.stride);
        if (lm_gl_bo_read(gl, &bo, out, W * 4)) {
            CHECK(px(out, W, 0, 0) == 0x00ff00u && px(out, W, 7, 3) == 0x00ff00u, "green top left (upright): %06x", px(out, W, 0, 0));
            CHECK(px(out, W, 0, H - 1) == 0, "nothing at the bottom left: %06x", px(out, W, 0, H - 1));
            CHECK(px(out, W, 10 + frame, 20) == 0xff0000u && px(out, W, 15 + frame, 25) == 0xff0000u, "red in frame %d", frame);
            CHECK(px(out, W, W - 1, H - 1) == 0x0000ffu && px(out, W, W - 5, H - 3) == 0x0000ffu, "blue bottom right: %06x", px(out, W, W - 1, H - 1));
            CHECK(px(out, W, W - 6, H - 1) == 0, "blue stops at its edge");
        } else {
            CHECK(0, "cannot map the buffer");
        }
        if (getenv("GL_TEST_ADDFB")) {                              /* the KMS side: a framebuffer on the GBM buffer (no master needed) */
            uint32_t handles[4] = { bo.handle }, pitches[4] = { bo.stride }, offsets[4] = { 0 }, fb = 0;
            int rc = drmModeAddFB2(fd, W, H, 0x34325258u, handles, pitches, offsets, &fb, 0);
            CHECK(rc == 0, "drmModeAddFB2 failed: %m");
            if (rc == 0)
                drmModeRmFB(fd, fb);
        }
        lm_gl_window_release(gl, win, &bo);
    }
    free(out);
    lm_gl_window_destroy(gl, win);
    lm_gl_destroy(gl);
    close(fd);
    printf(bad ? "gl-test gbm FAILED\n" : "gl-test gbm ok\n");
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "gbm"))
        return gbm_test();
    char err[200];
    struct lm_gl *gl = lm_gl_create(-1, err, sizeof(err));
    if (!gl) {
        printf("no OpenGL ES: %s\n", err);
        return 77;
    }
    printf("renderer: %s\n", lm_gl_renderer_name(gl));
    enum { W = 40, H = 30 };
    struct lm_gl_target t;
    CHECK(lm_gl_target_create(gl, W, H, &t), "target");
    lm_gl_bind_target(gl, &t, W, H);
    lm_gl_clear(gl, 0, 0, 0, 1);

    /* an opaque 4x2 texture: top row red, green, blue, white; bottom row all grey: it must not come out flipped */
    uint32_t tex_px[8] = { 0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu, 0xff808080u, 0xff808080u, 0xff808080u, 0xff808080u };
    unsigned tex = lm_gl_tex_create(gl, 4, 2, tex_px);
    CHECK(tex, "texture");
    lm_gl_draw_tex(gl, tex, 10, 5, 4, 2, 0, 0, 1, 1, 1.0f, true);
    /* half transparent premultiplied red over black: 0x80 0 0 */
    uint32_t half = 0x80800000u;
    unsigned ht = lm_gl_tex_create(gl, 1, 1, &half);
    lm_gl_draw_tex(gl, ht, 20, 10, 3, 3, 0, 0, 1, 1, 1.0f, false);
    /* a colour rectangle with a scissor */
    lm_gl_scissor(gl, true, 30, 20, 5, 4);
    lm_gl_draw_rect(gl, 28, 18, 10, 10, 0.0f, 0.0f, 1.0f, 1.0f);
    lm_gl_scissor(gl, false, 0, 0, 0, 0);
    /* a texture sub-update */
    uint32_t patch[4] = { 0xff00ffffu, 0xff00ffffu, 0xff00ffffu, 0xff00ffffu };
    unsigned t2 = lm_gl_tex_create(gl, 4, 4, NULL);
    uint32_t zero[16] = { 0 };
    lm_gl_tex_update(gl, t2, 0, 0, 4, 4, zero, 4);
    lm_gl_tex_update(gl, t2, 1, 1, 2, 2, patch, 2);
    lm_gl_draw_tex(gl, t2, 2, 20, 4, 4, 0, 0, 1, 1, 1.0f, false);
    lm_gl_finish(gl);

    uint32_t *out = calloc(W * H, 4);
    CHECK(lm_gl_readback(gl, &t, out, W * 4), "readback");
    CHECK(px(out, W, 10, 5) == 0xff0000u, "red at 10,5: %06x", px(out, W, 10, 5));
    CHECK(px(out, W, 11, 5) == 0x00ff00u, "green at 11,5");
    CHECK(px(out, W, 12, 5) == 0x0000ffu, "blue at 12,5");
    CHECK(px(out, W, 13, 5) == 0xffffffu, "white at 13,5");
    CHECK(px(out, W, 10, 6) == 0x808080u, "grey below (not flipped): %06x", px(out, W, 10, 6));
    CHECK(px(out, W, 10, 4) == 0 && px(out, W, 10, 7) == 0, "nothing around the texture");
    uint32_t h = px(out, W, 21, 11);
    CHECK((h >> 16) >= 0x7f && (h >> 16) <= 0x81 && (h & 0xffff) == 0, "half red over black: %06x", h);
    CHECK(px(out, W, 30, 20) == 0x0000ffu && px(out, W, 34, 23) == 0x0000ffu, "scissored blue inside");
    CHECK(px(out, W, 29, 20) == 0 && px(out, W, 35, 20) == 0 && px(out, W, 30, 19) == 0 && px(out, W, 30, 24) == 0, "scissor clips");
    CHECK(px(out, W, 3, 21) == 0x00ffffu && px(out, W, 4, 22) == 0x00ffffu && px(out, W, 2, 20) == 0 && px(out, W, 5, 23) == 0,
          "sub-update lands where asked: %06x", px(out, W, 3, 21));
    free(out);
    lm_gl_target_destroy(gl, &t);
    lm_gl_destroy(gl);
    printf(bad ? "gl-test FAILED\n" : "gl-test ok\n");
    return bad ? 1 : 0;
}
