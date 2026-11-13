/*
 * matrixcat: the cat that sits on the floor of the Matrix screensaver's room (client/mellagui-matrix.c).
 *
 * A British longhair, in the room's own material: it is built of ellipsoids (body, haunch, ruff, legs, paws, tail, head with cheeks, muzzle and ears), a ray
 * meets them analytically, and the fur is letters - a cell of the cat's own 3D grid shows one glyph of the room's alphabet, seen on the face of the cell the
 * surface turns to, and the glyphs keep changing, so it looks like the rain made a shape. Nothing here draws a picture: `mcat_pose` computes where everything
 * is at a time, `mcat_shade` answers for one ray.
 *
 * The head turns now and then (a script from the time alone: look ahead, left, right, down, or AT THE CAMERA - that target follows the drifting camera). The
 * eyes are dark with a slit pupil, and glow (an eyeshine, with a halo round them) in proportion to how straight the head really points at the camera
 * (`mcat.glow`), so the glow is a property of where the head is, not of the script.
 *
 * World: x to the right, y up, z into the room, as in the screensaver. Everything is deterministic from the time (tests).
 */
#ifndef LM_MATRIXCAT_H
#define LM_MATRIXCAT_H

#include <stdbool.h>
#include <stdint.h>

#define MCAT_GLYPHS 64                  /* the glyphs the fur picks from */
#define MCAT_PRIMS 32

struct mcat_prim {
    float c[3], r[3];                   /* centre and radii in the frame of the part */
    float ir[3];                        /* 1 / r */
    uint8_t frame;                      /* 0 the body's, 1 the head's */
    uint8_t part;
};

struct mcat {
    bool enabled;
    float t;                            /* the time of this pose, seconds */
    float pos[3], yaw, scale;           /* where it sits (on the floor), which way the body faces, its size */
    float cy, sy;                       /* cos / sin of the body's yaw */
    float rh[9];                        /* head rotation (head frame -> body frame), row major */
    float neck[3];                      /* the head's pivot, body frame */
    float head_yaw, head_pitch;         /* relative to the body (radians), for the tests */
    struct mcat_prim prim[MCAT_PRIMS];
    int n;
    float tail[8][4];                   /* the tail: a tube through these points (x, y, z, radius), body frame */
    float eye[2][3];                    /* the eyes in the world */
    float eye_open;                     /* 1 open .. 0.08 blinking */
    float look;                         /* cosine of the angle between where the head points and the direction to the camera */
    float glow;                         /* 0..1: how much the eyes glow */
    float centre[3], radius;            /* a sphere round everything, including the glow */
    const uint8_t *const *glyph;        /* MCAT_GLYPHS glyph bitmaps (16 rows of 8 pixels) */
    int nglyph;
    float ldir[3], lcol[3], lstr;       /* the room's key light: direction towards it, colour, strength (0 = none) */
};

/* Poses the cat for time `t` (seconds) with the camera at `cam`. */
void mcat_pose(struct mcat *c, float t, const float cam[3], const uint8_t *const *glyph, int nglyph, const float ldir[3], const float lcol[3], float lstr);

/* The cat along the ray from `o` in the unit direction `dn`, if it is nearer than `tmax` (metres): replaces rgb (0..255 per channel) with its colour and returns true;
 * the eyes' halo is added to rgb as well when it is in front of whatever is behind. `inv_f` = metres a pixel covers at a distance of one metre. */
bool mcat_shade(const struct mcat *c, const float o[3], const float dn[3], float tmax, float inv_f, float rgb[3]);

/* How much of the floor's light is left at (x, z): the cat's shadow, 1 = none. */
float mcat_floor_shade(const struct mcat *c, float x, float z);

#endif
