/*
 * matrixturtle: Filip, a Mississippi map turtle (Graptemys pseudogeographica kohni) that SWIMS through the air of the Matrix screensaver's room as if the room were
 * water (client/mellagui-matrix.c), at a safe distance from Tom (client/matrixcat.c).
 *
 * Built the way Tom is: of ellipsoids (here turned ones: the dome of the carapace and the flat plastron, a keel with dark knobs, a serrated rear edge, the neck, the
 * head with its snout, four webbed paddles that really stroke, a short tail), a ray meets them analytically, and the surface is made of the room's letters - a cell
 * of the turtle's own 3D grid shows one glyph of the room's alphabet. The colours are the animal's: an olive brown carapace with the yellow "map" lines (the seams of
 * its scutes and rings round each of them), a cream plastron with dark seams, olive skin with narrow yellow stripes and the yellow crescent behind each eye. Now
 * and then it lets go a few bubbles from its mouth, which rise (the one thing that makes the room look wet).
 *
 * The swimming is a pure function of the time: a slow loop of the room's upper half (an ellipse that drifts, with a changing pace), the heading follows the velocity,
 * the body banks into the turns and pitches with the climb, the paddles stroke (more when it is quick, they hang when it glides), the head turns now and then.
 * Now and then he comes down to the floor to rest (on a spot that Tom does not come near while he is there), pulls his head in, stretches his hind legs out behind
 * him or pulls every limb in, one after another, and rises again.
 * Tom's own state at the same time (`mcat_state`) is looked at to keep away from him: the turtle never comes nearer to the middle of Tom than `MTURTLE_SAFE`.
 *
 * World: x to the right, y up, z into the room, as in the screensaver.
 */
#ifndef LM_MATRIXTURTLE_H
#define LM_MATRIXTURTLE_H

#include <stdbool.h>
#include <stdint.h>

#define MTURTLE_PRIMS 56
#define MTURTLE_BUBBLES 8
#define MTURTLE_SAFE 1.6f               /* the least distance (metres) between the middle of the turtle and the middle of Tom */

struct mturtle_prim {
    float c[3];                         /* centre in the world */
    float a[9];                         /* world offset -> the unit sphere of this ellipsoid (row major) */
    float rw[9];                        /* the ellipsoid's axes in the world (columns), for the normal */
    float r[3], ir[3];                  /* radii and their inverses */
    float rad;                          /* a sphere round it */
    float ycut;                         /* in unit coordinates: only hits with y >= ycut (dir > 0) or y <= ycut (dir < 0) count */
    int8_t dir;                         /* 0 = no cut */
    uint8_t part;
};

struct mturtle_bubble { float c[3], r, age; };

struct mturtle {
    bool enabled;
    float t;
    float pos[3];                       /* the middle of the shell, world */
    float m[9];                         /* turtle -> world (columns: its right, up, forward) */
    float yaw, pitch, roll;             /* radians (forward = (sin yaw cos pitch, sin pitch, cos yaw cos pitch)) */
    float speed;                        /* metres a second along its path */
    float stroke;                       /* 0..1: how hard the paddles work */
    float rest;                         /* 0 swimming .. 1 lying on the floor (going down and rising are in between) */
    float head_in, fore_in, hind_in;    /* how far the head (0..1), the front legs (0..1) and the hind legs (-1 stretched out behind .. 1 pulled in) are drawn in */
    float head_yaw, head_pitch;         /* relative to the body (for the tests) */
    struct mturtle_prim prim[MTURTLE_PRIMS];
    int n;
    struct mturtle_bubble bub[MTURTLE_BUBBLES];
    int nbub;
    float centre[3], radius;            /* a sphere round the turtle (not the bubbles) */
    const uint8_t *const *glyph;        /* the room's alphabet (16 rows of 8 pixels) */
    int nglyph;
    float ldir[3], lcol[3], lstr;       /* the key light: direction towards it, colour, strength (0 = none) */
};

/* Which day the turtle lives: 0 = the same every time (tests, --at), any other number = a day of its own. (The path's phases come from it.) */
void mturtle_set_seed(uint32_t seed);

/* Where the middle of the turtle is at a time (metres; the same call the pose makes) and the middle of Tom there, for the tests and for the distance check. */
void mturtle_where(float t, float out[3]);
void mturtle_tom_centre(float t, float out[3]);

/* How much of the floor's light is left at (x, z): his soft shadow under him (strong when he lies on the floor, faint when he swims high), 1 = none. */
float mturtle_floor_shade(const struct mturtle *m, float x, float z);

/* Poses the turtle for time `t` (seconds). */
void mturtle_pose(struct mturtle *m, float t, const uint8_t *const *glyph, int nglyph, const float ldir[3], const float lcol[3], float lstr);

/* The turtle (and its bubbles) along the ray from `o` in the unit direction `dn`, if nearer than `tmax` (metres): paints rgb (0..255 per channel) and returns true.
 * `inv_f` = metres a pixel covers at a distance of one metre. */
bool mturtle_shade(const struct mturtle *m, const float o[3], const float dn[3], float tmax, float inv_f, float rgb[3]);

/* Bounding spheres (x, y, z, radius; world) of everything the turtle shows, for the coarse screen mask: returns how many (at most `max`). */
int mturtle_spheres(const struct mturtle *m, float out[][4], int max);

#endif
