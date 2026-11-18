/* blocksmobs.h - monsters of the block world, as a plain engine: values in, values out, no window and no clock (the time is an argument).
 *
 * One kind so far: the goblin, a green creature 1.7 m tall that comes out of the ground when it is night, walks towards the player, climbs a block that is one high,
 * hits when it is near (3 of the player's 20 hit points, once a second) and burns when the sun is up and it stands in the open. The player hits back with the sword
 * (`bl_mobs_pick` finds the one the player aims at, `bl_mobs_hurt` knocks it back and takes its life away: a goblin has 10 and a hit is 4).
 *
 * A mob is a body like Tom: a box (0.6 x 0.6 x 1.7 m) moved one axis at a time by `bl_box_move` (blocks.h), with gravity, so it never goes into a block, falls off
 * edges when it chases, floats in water (it does not swim away). `bl_mobs_step` takes any dt (it works in steps of 1/60 s inside) and is deterministic: the same world,
 * seed, player and calls give the same goblins.
 *
 * The picture: `bl_mobs_ray` answers for one ray (blocksview.c asks it for every pixel, like the pets): the nearest mob it meets, its colour and its distance. A
 * goblin is made of eleven boxes (legs that swing, a body, arms, a head with ears, a nose and two red eyes that shine in the dark). The caller sets how brightly the
 * mob is lit with `bl_mobs_light` once a frame.
 *
 * Not done: finding a way round a wall (a goblin that is blocked turns to one side until it is free), more kinds, shooting ones, mobs that are not hostile, mobs
 * that spawn in caves or by day.
 */
#ifndef MELLAGUI_BLOCKSMOBS_H
#define MELLAGUI_BLOCKSMOBS_H

#include "blocks.h"

#define BL_MOBS_MAX 12
#define BL_GOBLIN_HW 0.30f              /* half the width of its box, and its height */
#define BL_GOBLIN_H 1.70f
#define BL_GOBLIN_HP 10.0f
#define BL_SWORD_DAMAGE 4.0f
#define BL_SWORD_REACH 3.6f
#define BL_GOBLIN_HIT 3.0f              /* hit points of the player's 20 that one blow takes */

enum bl_mob_kind { BL_MOB_NONE = 0, BL_MOB_GOBLIN };

struct bl_mob {
	bool alive;
	int kind;
	float x, y, z;                  /* the feet: the middle of the bottom of its box */
	float vy, kx, kz;               /* falling speed, and the speed a blow gave it sideways (it fades away) */
	float yaw, ycos, ysin;          /* facing (0 = +z, a bigger yaw turns towards +x) and its cosine and sine, for the picture */
	float hp;
	float hurt;                     /* seconds of the red flash that are left */
	float attack_cd;                /* seconds until it may hit again */
	float swing;                    /* 1 just after a blow, going to 0: the arms are up */
	float phase;                    /* the walk: a stride is one turn */
	float speed;                    /* the speed it walks at now */
	float blocked, steer, jump_cd;
	float wander_t, wyaw;
	bool widle;
	bool ground, in_water;
	float age;
	float lit[3];                   /* how brightly it is lit, per colour (bl_mobs_light) */
	uint32_t rng;
};

struct bl_mobs {
	struct bl_mob mob[BL_MOBS_MAX];
	bool enabled;                   /* false: nothing spawns and the ones there are go */
	int cap;                        /* how many there may be at a time (1 .. BL_MOBS_MAX) */
	uint32_t rng;
	float spawn_t, acc;
	int kills, spawned, burned;
};

/* what a call of `bl_mobs_step` did to the player */
struct bl_mobs_result {
	float damage;                   /* hit points taken from the player */
	float kbx, kbz;                 /* the push the blows gave him (a speed in blocks a second) */
	int hits;                       /* blows landed */
	int born, burned;               /* mobs that appeared, mobs that the sun killed */
};

void bl_mobs_init(struct bl_mobs *m, uint32_t seed);
void bl_mobs_clear(struct bl_mobs *m);                      /* every mob gone (the player died, a new world) */
int bl_mobs_count(const struct bl_mobs *m);

/* One step of every mob. `player` = the feet of the player; `pl_alive` false = nobody to hunt. `time_h` is the hour of the day (blocksview.h): goblins appear
 * when `bl_daylight` is under 0.3 (14 to 26 blocks from the player, on the surface in the open), burn in the sun and thin out in the day. */
void bl_mobs_step(struct bl_mobs *m, const struct bl_world *w, const float player[3], bool pl_alive, float time_h, float dt, struct bl_mobs_result *out);

/* the one the ray hits first within `reach` (-1 = none, *t = the distance); a box a little bigger than the body, so that a blow does not miss by a hair */
int bl_mobs_pick(const struct bl_mobs *m, const float o[3], const float dir[3], float reach, float *t);
/* a blow: `damage` hit points, knocked back along the horizontal direction (dx, dz) (any length); true when it died */
bool bl_mobs_hurt(struct bl_mobs *m, int i, float damage, float dx, float dz);
/* does a mob stand in the cell (a block cannot be put into it)? */
bool bl_mobs_hit_cell(const struct bl_mobs *m, int x, int y, int z);
/* a mob that is not a body any more; for the tests: put one at a place (false when there is no room or the spot is inside a block) */
bool bl_mobs_add(struct bl_mobs *m, const struct bl_world *w, int kind, float x, float y, float z);

/* ---- the picture ---------------------------------------------------------------------------------------------------- */

struct bl_mob_hit {
	float t;                        /* distance along the (unit) ray */
	int id;                         /* which mob */
	float rgb[3];                   /* its colour there, 0 .. 1, lit and shaded */
};

/* Light every mob for a frame: the sky's light (the hour, and whether the sky is over it) and the light of torches and lamps near it. */
void bl_mobs_light(struct bl_mobs *m, const struct bl_world *w, float time_h);
/* the nearest mob the ray (origin o, unit direction d) hits within tmax; false when none */
bool bl_mobs_ray(const struct bl_mobs *m, const float o[3], const float d[3], float tmax, struct bl_mob_hit *hit);

#endif
