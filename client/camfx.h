#ifndef MELLAGUI_CAMFX_H
#define MELLAGUI_CAMFX_H

/*
 * camfx - the picture effects of mellagui-cam (like Apple's Photo Booth): a pure function from one XRGB8888 picture to another of the same size.
 * No state, no allocation that outlives the call, no time: the same picture gives the same result (so the tests can compare pixels), and the same
 * function serves the live preview, the grid of little previews, the snapshots, the clips and `mellagui-cam --snapshot FILE --effect ID`.
 *
 *   colour:      normal, bw, sepia, pencil, comic, pop, thermal, xray, glow, vintage
 *   mirrors:     mirror (the left half is reflected on the right), kaleidoscope, tunnel
 *   distortions: squeeze, stretch, bulge, dent, twirl (inside an ellipse / circle round the middle; the rest of the picture stays)
 *   focus:       the mirrors, the distortions and vintage (CAMFX_MIRROR .. CAMFX_VINTAGE, `camfx_has_focus`) work round a point the user can move
 *                (camfx_apply_at: 0..1 of the width / height, 0.5 0.5 = the middle): the axis of the mirror, the centre of the kaleidoscope, the tunnel,
 *                the lens and the twirl, the light spot of the vintage vignette.
 *   backdrops:   sunset, space, neon (a made-up picture behind you) and ghost (you, see-through, in the empty room): they need a REFERENCE picture - the
 *                scene with nobody in it (Photo Booth asks you to step out of the picture) - and keep what differs from it. Without one (`ref` NULL, or the
 *                grid's little previews) an oval round the middle stands in for the person. The difference is made of the colour distance, a binary mask
 *                cleaned by two box blurs and a soft edge; it does not follow a change of light (a camera that re-exposes when you leave shows a halo).
 */

#include <stdint.h>

enum {
    CAMFX_NORMAL, CAMFX_BW, CAMFX_SEPIA, CAMFX_PENCIL, CAMFX_COMIC, CAMFX_POP, CAMFX_THERMAL, CAMFX_XRAY, CAMFX_GLOW,
    CAMFX_MIRROR, CAMFX_KALEIDO, CAMFX_TUNNEL, CAMFX_SQUEEZE, CAMFX_STRETCH, CAMFX_BULGE, CAMFX_DENT, CAMFX_TWIRL, CAMFX_VINTAGE,
    CAMFX_SUNSET, CAMFX_SPACE, CAMFX_NEON, CAMFX_GHOST,
    CAMFX_COUNT
};

const char *camfx_name(int fx);                 /* "Black & White" ... (NULL for a number out of range) */
const char *camfx_id(int fx);                   /* "bw" ... the word of the command line */
int camfx_by_id(const char *id);                /* the number of an id (case does not matter), -1 for an unknown one */

int camfx_is_backdrop(int fx);                  /* 1 for the effects that want a reference picture */
int camfx_has_focus(int fx);                    /* 1 for the effects that work round a point the user can move */

/* src and dst are w * h pixels each (stride w) and must not overlap. A picture under 2 x 2 is copied. */
void camfx_apply(int fx, const uint32_t *src, int w, int h, uint32_t *dst);
/* The same with the reference picture of the backdrops (w * h, may be NULL; the others ignore it). */
void camfx_apply_ref(int fx, const uint32_t *src, const uint32_t *ref, int w, int h, uint32_t *dst);
/* ... and with the focus point (fcx, fcy: 0..1 of the width and the height, clamped; only the effects that have a focus use it). */
void camfx_apply_at(int fx, const uint32_t *src, const uint32_t *ref, int w, int h, uint32_t *dst, float fcx, float fcy);

#endif
