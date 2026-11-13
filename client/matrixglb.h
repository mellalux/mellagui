/*
 * matrixglb: the cat of the Matrix screensaver's room made of a rigged .glb model (client/glb.[ch]) instead of the built-in ellipsoids.
 *
 * Each frame: the day's state (where he is, whether he walks: the same `mcat_state` as the built-in Tom, which gives a model with only a walk animation a
 * path to walk), the pose (the rest pose blended with the walk by how fast he goes, the head turned by the same script as Tom's, towards the camera now and
 * then), the skinning of every vertex, and a small z-buffer rasteriser into the part of the picture the cat covers. The rasteriser keeps, for every pixel,
 * which triangle is nearest and the barycentric weights; the room's render threads then colour the pixels (`mglb_pixel`) - the fur is LETTERS again (a
 * cell of a 3D grid in the rest pose, so the letters stay on the fur while it moves), coloured by the model's own texture, lit by the room - and add the
 * eyes' glow: the eyes are painted on the texture, so their place is given (glb.eye) and they glow in proportion to where the head really points.
 */
#ifndef LM_MATRIXGLB_H
#define LM_MATRIXGLB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "matrixcat.h"

struct mglb;

/* Loads the model; NULL (and a reason in err) when it cannot be used. */
struct mglb *mglb_load(const char *path, char *err, size_t errsz);
void mglb_free(struct mglb *m);

/* Poses him for time `t` with the camera at `cam` and the room's key light, and sets the shadow in `shadow` (for mcat_floor_shade). */
void mglb_pose(struct mglb *m, float t, const float cam[3], const uint8_t *const *glyph, int nglyph, const float ldir[3], const float lcol[3], float lstr);

/* Skins and rasterises for a camera (position, forward / right / up unit vectors), a picture of w x h pixels and a focal length of f pixels, on `threads` threads. */
void mglb_prepare(struct mglb *m, const float o[3], const float fwd[3], const float right[3], const float up[3], int w, int h, float f, int threads);

/* The part of the picture (x0 <= x < x1, y0 <= y < y1) the cat and the glow of its eyes can be in, after mglb_prepare. */
void mglb_rect(const struct mglb *m, int *x0, int *y0, int *x1, int *y1);

/* The cat at pixel (x, y), if it is nearer than `tmax` metres along the ray (o, dn): replaces rgb and returns true; the glow round the eyes is added. */
bool mglb_pixel(const struct mglb *m, int x, int y, const float o[3], const float dn[3], float tmax, float inv_f, float rgb[3]);

/* His shadow on the floor (see mcat_floor_shade). */
const struct mcat *mglb_shadow(const struct mglb *m);

/* For the tests and the information: the glow 0..1, where he is, whether the model has eyes. */
float mglb_glow(const struct mglb *m);
void mglb_position(const struct mglb *m, float *x, float *z);

#endif
