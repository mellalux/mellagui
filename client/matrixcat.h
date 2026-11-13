/*
 * matrixcat: Tom, the cat that lives on the floor of the Matrix screensaver's room (client/mellagui-matrix.c).
 *
 * A cream-yellow British longhair, in the room's own material: built of ellipsoids (haunch and thighs, torso, chest, ruff, slim front legs with paws and toes,
 * hind feet, head with cheeks, muzzle, nose and ears) and a tube for the tail; a ray meets them analytically (the tail by sphere tracing). The fur is letters - a
 * cell of the cat's own 3D grid shows one glyph of the room's alphabet, seen on the face of the cell the surface turns to, and the glyphs keep changing - and
 * round the edge a thin shell of loose letters makes it fluffy. Whiskers, a mouth, ears that flick, a bushy tail with a swishing tip. Nothing here draws a
 * picture: `mcat_pose` computes where everything is at a time, `mcat_shade` answers for one ray, `mcat_floor_shade` for the shadow on the floor.
 *
 * Tom sits, stands up and WALKS to another place in the room (a gait with four legs; `stand` / `lie` blend the poses), sits there, lies down for a while, gets up and walks back (`mcat_cycle`). The head turns now and then (a script from the time alone: look
 * ahead, left, right, down, or AT THE CAMERA - that target follows the drifting camera). The eyes are dark with a slit pupil, and glow (an eyeshine with a halo
 * round them) in proportion to how straight the head really points at the camera (`mcat.glow`), so the glow is a property of where the head is, not of the
 * script.
 *
 * World: x to the right, y up, z into the room, as in the screensaver. Everything is deterministic from the time (tests).
 */
#ifndef LM_MATRIXCAT_H
#define LM_MATRIXCAT_H

#include <stdbool.h>
#include <stdint.h>

#define MCAT_GLYPHS 64                  /* the glyphs the fur picks from */
#define MCAT_PRIMS 48

struct mcat_prim {
    float c[3], r[3];                   /* centre and radii in the frame of the part */
    float ir[3];                        /* 1 / r */
    float irs[3];                       /* 1 / (r + the thickness of the fluffy shell) */
    float shell2;                       /* (1 + shell / smallest radius)^2: a ray whose closest approach (in units of r) is farther cannot touch the shell */
    uint8_t frame;                      /* 0 the body's, 1 the head's */
    uint8_t part;
};

#define MCAT_TUBES 5                    /* 0 the tail, 1..4 the legs (front left, front right, hind left, hind right) */

/* A tube of changing radius through up to 8 points (x, y, z, radius; body frame): the tail and the legs, which a ray finds by sphere tracing. */
struct mcat_tube {
    int n;
    float p[8][4];
    float seg[7][4];                    /* the vector from point i to i + 1 and 1 / its length squared */
    float lo[3], hi[3];                 /* a box round it, with the fluffy shell */
    uint8_t part;
};

struct mcat {
    bool enabled;
    float t;                            /* the time of this pose, seconds */
    float pos[3], yaw, scale;           /* where it sits (on the floor), which way the body faces, its size */
    float cy, sy;                       /* cos / sin of the body's yaw */
    float lie;                          /* 0 sitting .. 1 lying down */
    float rh[9];                        /* head rotation (head frame -> body frame), row major */
    float neck[3];                      /* the head's pivot, body frame */
    float head_yaw, head_pitch;         /* relative to the body (radians), for the tests */
    struct mcat_prim prim[MCAT_PRIMS];
    int n;
    struct mcat_tube tube[MCAT_TUBES];  /* the tail and the four legs */
    float stand;                        /* 0 sitting / lying .. 1 standing (walking) */
    float walk;                         /* 0..1: how much of a walk's stride the legs make (0 = standing still) */
    float eye[2][3];                    /* the eyes in the world */
    float eye_open;                     /* 1 open .. 0.08 blinking */
    float look;                         /* cosine of the angle between where the head points and the direction to the camera */
    float glow;                         /* 0..1: how much the eyes glow */
    float centre[3], radius;            /* a sphere round everything, including the glow */
    const uint8_t *const *glyph;        /* MCAT_GLYPHS glyph bitmaps (16 rows of 8 pixels) */
    int nglyph;
    float ldir[3], lcol[3], lstr;       /* the room's key light: direction towards it, colour, strength (0 = none) */
    float sh_w, sh_l, sh_h;             /* the shadow: half width and half length in the body's units, the height in metres */
};

/* Where Tom is and what he does at a time (his day, see mcat_cycle): the same for every model of him. */
struct mcat_state { float x, z, yaw, stand, lie, walk, dist; };
void mcat_state(float t, struct mcat_state *out);
/* The length of Tom's whole day in seconds (he sits, stands up, walks to another place, sits, lies down, gets up, walks back ...): the pose repeats every cycle. */
float mcat_cycle(void);

/* Poses the cat for time `t` (seconds) with the camera at `cam`. */
void mcat_pose(struct mcat *c, float t, const float cam[3], const uint8_t *const *glyph, int nglyph, const float ldir[3], const float lcol[3], float lstr);

/* The cat along the ray from `o` in the unit direction `dn`, if it is nearer than `tmax` (metres): replaces rgb (0..255 per channel) with its colour and returns true;
 * the eyes' halo is added to rgb as well when it is in front of whatever is behind. `inv_f` = metres a pixel covers at a distance of one metre. */
bool mcat_shade(const struct mcat *c, const float o[3], const float dn[3], float tmax, float inv_f, float rgb[3]);

/* Bounding spheres (x, y, z, radius; world, metres) that together hold everything the cat can show - its parts, the tail, the head with whiskers and the eyes' glow - for
 * a coarse screen mask: returns how many (at most `max`). */
int mcat_spheres(const struct mcat *c, float out[][4], int max);

/* How much of the floor's light is left at (x, z): the cat's shadow (it falls away from the key light), 1 = none. */
float mcat_floor_shade(const struct mcat *c, float x, float z);

#endif
