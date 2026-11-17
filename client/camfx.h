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
 *   more colour: negative, solarize, duotone, emboss, night (night vision), pixel (a coarse 8 bit palette), halftone (dots of ink on paper), glitch, ascii
 *                (green letters of the built-in font, by brightness)
 *   more focus:  triple, mandala (kaleidoscopes of 3 and 10 sectors), fisheye, ripple, water (a reflection under the horizon at the point), tiltshift
 *                (the sharp band is at the point's height)
 *   history:     trails (bright things leave a fading trail), echo (a slow average of the pictures), motion (what changed since the picture before,
 *                cyan = got brighter, magenta = got darker), timewarp (the rows are pictures of different ages, a slit scan): they need the pictures
 *                that came before (`struct camfx_hist`, made by the program; `camfx_needs_history`); without them they show the picture in a plain form
 *   faces:       hat (a top hat), shades (sunglasses), moustache, clown (a rainbow wig and a red nose): drawn on the faces the program found
 *                (`struct camfx_face`, client/facedet.c) - on a picture with no face they change nothing
 *   backdrops:   picture (a picture of your own: camera.conf `backdrop =`) and room (Tom's room of the Matrix screensaver, drawn by mellagui-matrix)
 *                behind you, like the ones below; sunset, space, neon (a made-up picture behind you) and ghost (you, see-through, in the empty room), rain (a wet window on a city at
 *                night), sky (clouds over a meadow), matrix (falling letters): they need a REFERENCE picture - the
 *                scene with nobody in it (Photo Booth asks you to step out of the picture) - and keep what differs from it. Without one (`ref` NULL, or the
 *                grid's little previews) an oval round the middle stands in for the person. The difference is made of the colour distance, a binary mask
 *                cleaned by two box blurs and a soft edge; it does not follow a change of light (a camera that re-exposes when you leave shows a halo).
 */

#include <stdint.h>

/* The order is the order of the grid (nine to a page): classic colour, mirrors and lenses, more colour, more focus and history, backdrops. */
enum {
    CAMFX_NORMAL, CAMFX_BW, CAMFX_SEPIA, CAMFX_PENCIL, CAMFX_COMIC, CAMFX_POP, CAMFX_THERMAL, CAMFX_XRAY, CAMFX_GLOW,
    CAMFX_MIRROR, CAMFX_KALEIDO, CAMFX_TUNNEL, CAMFX_SQUEEZE, CAMFX_STRETCH, CAMFX_BULGE, CAMFX_DENT, CAMFX_TWIRL, CAMFX_VINTAGE,
    CAMFX_NEGATIVE, CAMFX_SOLARIZE, CAMFX_DUOTONE, CAMFX_EMBOSS, CAMFX_NIGHT, CAMFX_PIXEL, CAMFX_HALFTONE, CAMFX_GLITCH, CAMFX_ASCII,
    CAMFX_TRIPLE, CAMFX_MANDALA, CAMFX_FISHEYE, CAMFX_RIPPLE, CAMFX_WATER, CAMFX_TILTSHIFT, CAMFX_TRAILS, CAMFX_ECHO, CAMFX_MOTION,
    CAMFX_SUNSET, CAMFX_SPACE, CAMFX_NEON, CAMFX_GHOST, CAMFX_RAIN, CAMFX_SKY, CAMFX_MATRIX, CAMFX_PICTURE, CAMFX_ROOM,
    CAMFX_TIMEWARP, CAMFX_HAT, CAMFX_SHADES, CAMFX_MOUSTACHE, CAMFX_CLOWN,
    CAMFX_COUNT
};

#define CAMFX_RING 12                           /* how many older pictures timewarp wants */

/* A face found in the picture (client/facedet.c): the box in the picture's own pixels. */
struct camfx_face {
    float x, y, w, h;
};

/* What an effect may need besides the picture (everything optional): the pictures before this one (trails, echo, motion, timewarp), the faces
 * (hat, shades, moustache, clown) and a picture to put behind you (picture, room; exactly as big as the picture). */
struct camfx_hist {
    const uint32_t *prev_src;                   /* the camera picture before this one */
    const uint32_t *prev_out;                   /* what this effect made of that one */
    const uint32_t *const *ring;                /* older camera pictures, ring[0] the one before this one (timewarp) */
    int nring;
    const struct camfx_face *faces;             /* the faces in this picture, the biggest first */
    int nfaces;
    const uint32_t *pic;                        /* the backdrop picture */
};

const char *camfx_name(int fx);                 /* "Black & White" ... (NULL for a number out of range) */
const char *camfx_id(int fx);                   /* "bw" ... the word of the command line */
int camfx_by_id(const char *id);                /* the number of an id (case does not matter), -1 for an unknown one */

int camfx_is_backdrop(int fx);                  /* 1 for the effects that want a reference picture */
int camfx_has_focus(int fx);                    /* 1 for the effects that work round a point the user can move */
int camfx_needs_history(int fx);                /* 1 for trails, echo, motion and timewarp */
int camfx_needs_faces(int fx);                  /* 1 for hat, shades, moustache and clown: the program finds the faces (client/facedet.c) */
int camfx_needs_picture(int fx);                /* 1 for picture and room: the program gives the backdrop picture (hist->pic) */

/* src and dst are w * h pixels each (stride w) and must not overlap. A picture under 2 x 2 is copied. */
void camfx_apply(int fx, const uint32_t *src, int w, int h, uint32_t *dst);
/* The same with the reference picture of the backdrops (w * h, may be NULL; the others ignore it). */
void camfx_apply_ref(int fx, const uint32_t *src, const uint32_t *ref, int w, int h, uint32_t *dst);
/* ... and with the focus point (fcx, fcy: 0..1 of the width and the height, clamped; only the effects that have a focus use it). */
void camfx_apply_at(int fx, const uint32_t *src, const uint32_t *ref, int w, int h, uint32_t *dst, float fcx, float fcy);

/* ... and with the history (hist may be NULL). */
void camfx_apply_h(int fx, const uint32_t *src, const uint32_t *ref, const struct camfx_hist *hist, int w, int h, uint32_t *dst, float fcx, float fcy);

#endif
