/* blockshero.h - the body of the player in the block world: a picture made of boxes, like the goblins and the workers.
 *
 * Plain code: the caller fills in where the hero is and what he does, `bl_hero_set` works the rest out for a frame, and blocksview.c asks it for every pixel.
 * Seen from outside (the command view) it is the whole figure: boots, legs, a purple tunic with a golden belt, a white cape, a head with brown hair, and in the
 * right hand the sword or the block he holds. Seen from his own eyes (`first`) the head, the cape and the right arm are left out, the legs stand under the eyes and
 * the trunk is cut at BL_HERO_CHEST and set back behind them (a whole chest that near the eyes would fill the picture), so that looking down shows the boots, the
 * legs and the chest; the right arm with what it holds is drawn in front of the camera, in the lower right of the picture, and it swings when he breaks, puts or hits.
 */
#ifndef MELLAGUI_BLOCKSHERO_H
#define MELLAGUI_BLOCKSHERO_H

#include "blocks.h"

#define BL_HERO_BACK 0.22f      /* how far behind the eyes the trunk stands in the first person */
#define BL_HERO_CHEST 1.16f     /* ... and how high it goes */
#define BL_HERO_SNEAK 0.12f     /* how much lower the upper body is while he sneaks */

#define BL_HERO_BOXES 22
#define BL_HERO_ARM 8                   /* the boxes of the arm and what it holds */

/* a box of the figure for one frame, with the rectangle of the picture it can be seen in (in x / z and y / z of the camera's frame) */
struct bl_hero_box {
	float lo[3], hi[3];
	uint32_t rgb;
	int block;                      /* the arm: > 0 wears that block's pictures, -1 shines by itself */
	bool bound;                     /* sx0 .. sy1 say where it can be seen (the tests switch it off to compare) */
	float sx0, sx1, sy0, sy1;
};

struct bl_hero {
	/* filled in by the caller */
	float x, y, z;                  /* the feet */
	float yaw, pitch;
	float phase;                    /* the walk: a stride is one turn */
	float stride;                   /* 0 standing .. 1 walking: how far the legs go */
	float swing;                    /* 1 when a swing of the arm begins, going to 0 */
	float hurt;                     /* 0 .. 1: red from a blow */
	bool sneak;
	bool first;                     /* seen from his own eyes */
	bool sword;                     /* a weapon is in the hand, else the block */
	int weapon;                     /* which (enum bl_weapon of blocksmobs.h: 0 sword, 1 spear, 2 war hammer, 3 bow, 4 fire staff, 5 laser) */
	int held;                       /* the block in the hand, 0 = an empty hand */
	float eye[3];                   /* first: the camera */
	float fov, aspect;              /* first: the camera's vertical angle and the picture's width / height */
	/* worked out (bl_hero_set) */
	float ycos, ysin;
	float f[3], r[3], u[3];         /* the camera's directions */
	float lit[3];
	float arm[3];                   /* the arm's pivot in the camera's frame ... */
	float ac, as, bc, bs;           /* ... and how it is turned (cos / sin of its yaw and pitch) */
	int nbody, narm;
	struct bl_hero_box body[BL_HERO_BOXES], vm[BL_HERO_ARM];  /* the figure (in his own frame) and the arm with what it holds (in the arm's) */
	float vo[3];                    /* the camera in the arm's frame */
};

/* Work out what a frame needs: the directions, the boxes and the light (the sky's by the hour, and torches near him). Call it after anything above changes. */
void bl_hero_set(struct bl_hero *h, const struct bl_world *w, float time_h);

/* The body in the world: the nearest part the ray (origin o, unit direction d) hits within tmax, its distance and colour (0 .. 1, lit); false when none. */
bool bl_hero_ray(const struct bl_hero *h, const float o[3], const float d[3], float tmax, float rgb[3], float *t);
/* First person only: the arm and what it holds, which are in front of everything; false when the ray does not meet them. */
bool bl_hero_arm_ray(const struct bl_hero *h, const float d[3], float rgb[3]);
/* how much of the light is left on the top of a block at (x, y, z): the soft shadow under his feet (1 = none) */
float bl_hero_floor_shade(const struct bl_hero *h, float x, float y, float z);

#endif
