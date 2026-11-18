/* blockspets.h - Tom, the cat, and Filip, the map turtle (client/matrixcat.c, client/matrixturtle.c, made for the Matrix screensaver's room) living in a block world
 * as BODIES.
 *
 * Their models only draw: a pose from a state (`mcat_pose_state`, `mturtle_pose_state`). Here they are simulated, a step at a time, in the world:
 *
 *   TOM walks on the ground, falls when it goes, climbs a block that is one high, does not walk off an edge higher than a block, into water, through a wall or through a
 *   tree; he sits, stands, walks, runs, hops on the spot and lies down (deterministic from the world's seed and the time: the same decisions every run, but what he meets
 *   decides how it turns out). He roams round his yard (a patch of the floor size the Matrix room has, found on the terrain: flat, dry, away from the player).
 *   FILIP swims IN WATER: the nearest lake or sea near the player (a pond is dug beside the yard when there is none), at a depth of his own, never out of the water, turning
 *   away from the shore; now and then he sinks to the bottom, rests there with his head in, his legs stretched out or pulled in, and rises again. He lets a few bubbles go,
 *   which rise to the surface. When the water goes he sinks to what is below and stays there.
 *   BOTH are solid: the player cannot walk through them (he pushes them away, or stands on them), and a block cannot be put where they are (`bl_pets_hit_cell`).
 *
 * The renderer (blocksview.c) draws them where they are, hidden by the blocks in front of them and tinted by the water in front of them.
 *
 * One pair of pets at a time: their day is a global of the modules (mcat_set_seed, mturtle_set_seed): `bl_pets_init` sets it.
 */
#ifndef MELLAGUI_BLOCKSPETS_H
#define MELLAGUI_BLOCKSPETS_H

#include "blocks.h"
#include "matrixcat.h"
#include "matrixturtle.h"

struct bl_tom {
	float x, y, z;                  /* the feet: the middle of the bottom of his box */
	float vy;
	float yaw;
	int mode;
	float timer;                    /* the time left of what he is doing */
	float stand, lie, walk, run, dist, speed, crouch, tuck;     /* the pose: see mcat_state */
	float tx, tz;                   /* where he is going */
	bool ground;
	bool running;
	int next;                       /* what he does after he has risen */
	int hops_left, hop_phase;
	float blocked, jump_cd, air_t, land_fall, steer, lie_total;
	uint32_t rng;
};

struct bl_filip {
	float x, y, z;                  /* the middle of the shell */
	float vy, yaw, pitch, speed, yaw_rate;
	int mode;                       /* swimming, sinking, resting, rising, stranded */
	float rest, rest_tau, rest_d, rest_yaw;
	uint32_t rest_hk;
	float tx, tz;                   /* where he swims to */
	float depth;                    /* how far under the surface he likes to be (m) */
	float depth_timer, rest_timer;
	float bottom;                   /* the floor of the water under him (the top of the block) */
	float ystart, time;             /* where a sink or a rise began; his own clock */
	float bub_timer;
	struct mturtle_bubble bub[MTURTLE_BUBBLES];
	int nbub;
	bool swimming;                  /* he is in water */
	uint32_t rng;
};

struct bl_pets {
	bool on;                      /* drawn at all */
	bool placed;                  /* a yard has been found */
	float yard[3];                /* the middle of the yard: where Tom lives */
	float pond[3];                /* the middle of the water where Filip lives (x, the surface y, z) */
	bool have_pond, made_pond;
	float org_cat[3], org_tur[3]; /* the frame of each model: x = z = 0 (the world's own), y = the floor under him (the ground / the bottom of the water) */
	struct bl_tom tom;
	struct bl_filip filip;
	struct mcat cat;
	struct mturtle tur;
	const uint8_t *glyph[MCAT_GLYPHS];
	int nglyph;
	float ldir[3], lcol[3], lstr; /* the key light of the last pose */
	float acc;                    /* simulation time not yet stepped */
	float tom_floor, tur_floor;   /* the floors of the last pose */
};

/* `glyph` = nglyph bitmaps of 16 rows of 8 pixels (lm_font_glyph) for the fur; seed 0 = the same day every time, another number a day of its own. */
void bl_pets_init(struct bl_pets *p, uint32_t seed, const uint8_t *const *glyph, int nglyph);

/* Looks for the yard on `w`, at least 10 blocks from `near` (x, y, z; the player), puts Tom in it, finds the water near the player (a lake, the sea; a pond of 7 x 7
 * is DUG next to the yard when there is none within 56 blocks: the world is changed) and puts Filip in it. False when no patch of the size is inside the world. */
bool bl_pets_place(struct bl_pets *p, struct bl_world *w, const float near[3]);
/* A yard where the caller wants it (tests): Tom there, Filip in the water near (`near` may be NULL: near the yard). */
void bl_pets_set_yard(struct bl_pets *p, struct bl_world *w, float x, float z, const float near[3]);
/* Where they were (a saved game): positions that are free of solid blocks are kept, the others go back to the yard / the water. */
void bl_pets_restore(struct bl_pets *p, const struct bl_world *w, const float tom[3], const float filip[3]);

/* Moves them `dt` seconds on (in steps of 1/60 s, at most 0.25 s at a time). The world is only read. */
void bl_pets_step(struct bl_pets *p, const struct bl_world *w, float dt);
/* They are bodies: if the player's box overlaps one, it is pushed away (as far as the world lets it; the rest is the player's to step back), or the player is lifted onto it. */
void bl_pets_push_player(struct bl_pets *p, const struct bl_world *w, struct bl_player *pl);
/* Would a block put in the cell (x, y, z) be inside one of them? */
bool bl_pets_hit_cell(const struct bl_pets *p, int x, int y, int z);
/* For the tests: send Tom to a place (run or walk) now. */
void bl_pets_tom_go(struct bl_pets *p, float x, float z, bool run);

/* The pose of the state, seen from `cam`, in the light of the hour of the day; `t` is the pets' own clock (seconds: the head's looks, the blinks, the strokes). `dt` is
 * unused by the floors now (they are exact) and kept for the callers. */
void bl_pets_pose(struct bl_pets *p, const struct bl_world *w, float t, float dt, const float cam[3], float time_h);

/* What the pets add to a ray, for the renderer: up to two events (Tom, Filip) in the order of their distance along the ray, each a premultiplied colour (0..1), an opacity
 * and the distance of its middle. `o` and `dn` (unit) are in the world; `inv_f` = metres a pixel covers at one metre; only what is nearer than `tmax`. */
struct bl_pet_event {
	float c[3];                   /* premultiplied colour, 0..1 */
	float a;                      /* how much of what is behind it is covered, 0..1 */
	float tp;                     /* distance along the ray of the middle of the creature */
};
int bl_pets_events(const struct bl_pets *p, const float o[3], const float dn[3], float tmax, float inv_f, struct bl_pet_event ev[2]);

/* Which of them a ray meets nearer than `tmax`: 1 Tom, 2 Filip, 0 none (the picture's test hook). */
int bl_pets_probe(const struct bl_pets *p, const float o[3], const float dn[3], float tmax, float inv_f);

/* The light left on the top face of a block at (x, y, z) (world; y = the height of the face): their shadows on the ground, 1 = none. */
float bl_pets_floor_shade(const struct bl_pets *p, float x, float y, float z);

struct bl_pets_info {
	float tom[3], tom_yaw, tom_stand, tom_lie, tom_walk, tom_air;   /* world position of his feet, and what he does */
	int tom_mode;
	float filip[3], filip_rest;                                      /* where the middle of the turtle is; 0 swimming .. 1 on the bottom */
	bool filip_in_water;
	float yard[3], pond[3];
};
void bl_pets_info(const struct bl_pets *p, struct bl_pets_info *out);

#endif
