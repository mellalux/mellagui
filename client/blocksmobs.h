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
 * The player's own are mobs too: workers (they walk, break blocks and BUILD the sites of blocksbuild.h) and soldiers (they fight, and on a watchtower they shoot).
 * A goblin that is after somebody or the keep and finds a made block in its way (bl_siege_hp) hits it until it breaks. The engine never changes the world: what is
 * broken or put comes back in `struct bl_mobs_result` and the caller does it.
 *
 * Not done: finding a way round a wall (a goblin that cannot break what is in its way turns to one side until it is free), shooting goblins, mobs that spawn in caves
 * or by day.
 */
#ifndef MELLAGUI_BLOCKSMOBS_H
#define MELLAGUI_BLOCKSMOBS_H

#include "blocks.h"
#include "blocksbuild.h"

#define BL_MOBS_MAX 40                  /* goblins, workers and soldiers together */
#define BL_GOBLIN_HW 0.30f              /* half the width of its box, and its height */
#define BL_GOBLIN_H 1.70f
#define BL_GOBLIN_HP 10.0f
#define BL_SWORD_DAMAGE 4.0f
#define BL_SWORD_REACH 3.6f
#define BL_GOBLIN_HIT 3.0f              /* hit points of the player's 20 that one blow takes */
#define BL_GOBLIN_KEEP_HIT 2.0f         /* hit points one blow takes from the keep (the game gives it 60) */
#define BL_GOBLIN_NEAR 8.0f             /* a goblin this near the player goes for him, not for the keep */
#define BL_UNIT_NEAR 10.0f              /* a worker or a soldier this near a goblin is gone for before the keep or a player farther off ... */
#define BL_UNIT_SEE 24.0f               /* ... and one this near when it is nearer than both of them */
#define BL_UNIT_APART 30.0f             /* units this far from the player (in a wave: from the keep) have goblins come out of the ground round them, too */
#define BL_RAID_CAP 3                   /* at night with no wave: how many may be out among such units, on top of the `cap` round the player */
#define BL_GOBLIN_KEEP_SEE 48.0f        /* how far from the keep a goblin goes for it */

#define BL_WORKER_H 1.56f               /* a worker is a box of the same width, a little shorter (the model is 1.1 times its boxes) */
#define BL_WORKER_HP 6.0f               /* a goblin's blow is 3 */
#define BL_WORKER_MAX 8
#define BL_WORKER_SPEED 2.2f

#define BL_BRUTE_H 2.10f                /* a brute: a goblin that is bigger, tougher and slower; it comes at the end of a wave on the later nights */
#define BL_BRUTE_HP 26.0f               /* (a sword blow is 4: seven of them) */
#define BL_BRUTE_HIT 5.0f               /* hit points a blow of it takes from the player, a worker or a soldier (a goblin's is 3) */
#define BL_BRUTE_KEEP_HIT 4.0f          /* and from the keep (a goblin's is 2) */

#define BL_SOLDIER_H 1.65f              /* the soldier: a little taller than a worker, with a helmet (the model is 1.15 times its boxes) */
#define BL_SOLDIER_HP 14.0f
#define BL_SOLDIER_MAX 8
#define BL_SOLDIER_SPEED 3.0f
#define BL_SOLDIER_DAMAGE 4.0f          /* a blow of a soldier's sword: three kill a goblin, as the player's */
#define BL_SOLDIER_REACH 1.3f
#define BL_SOLDIER_COOLDOWN 0.8f
#define BL_SOLDIER_SEE 12.0f            /* how far a soldier sees a goblin */
#define BL_SOLDIER_LEASH 18.0f          /* and how far from its post it goes after one */

#define BL_BUILD_TIME 0.5f              /* seconds a worker takes to put one block of a building */
#define BL_BOW_RANGE 20.0f              /* a soldier on a watchtower shoots the goblins this near (horizontally) that it can see */
#define BL_BOW_DAMAGE 3.0f              /* four arrows kill a goblin */
#define BL_BOW_COOLDOWN 1.5f
#define BL_ARROWS_MAX 8                 /* arrows in the air (their picture only: the hit counts at once) */
#define BL_ARROW_TIME 0.25f
#define BL_DAMAGED_MAX 24               /* blocks the goblins have started on */

enum bl_mob_kind { BL_MOB_NONE = 0, BL_MOB_GOBLIN, BL_MOB_WORKER, BL_MOB_SOLDIER };
enum bl_order { BL_ORDER_IDLE = 0, BL_ORDER_MOVE, BL_ORDER_GATHER, BL_ORDER_BUILD, BL_ORDER_TOWER };

static inline float bl_mob_h(int kind)                         /* the height of a kind's box */
{
	return kind == BL_MOB_WORKER ? BL_WORKER_H : kind == BL_MOB_SOLDIER ? BL_SOLDIER_H : BL_GOBLIN_H;
}

static inline bool bl_mob_is_unit(int kind)                    /* the player's: a worker or a soldier */
{
	return kind == BL_MOB_WORKER || kind == BL_MOB_SOLDIER;
}

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
	/* workers: what they have been told to do */
	int order;                      /* enum bl_order */
	float tx, tz;                   /* MOVE: where to; GATHER: where to stand; a soldier: its post (where it goes back to) */
	int gx, gy, gz, gblock;         /* GATHER: the block (the lowest log of a tree), and what kind of block to go on with */
	float work;                     /* seconds spent on the block so far */
	float wait;                     /* seconds to wait before the next block (the game takes the broken ones away meanwhile) */
	float stuck;                    /* seconds it has got no nearer to where it is going */
	float best_d;                   /* the nearest it has been to it so far */
	bool selected;                  /* the player has chosen it (drawn lighter) */
	bool brute;                     /* a goblin of the big kind (BL_BRUTE_*) */
	int site;                       /* BUILD: which of bl_mobs.site */
	bool at_site;                   /* BUILD: it has got near enough and works (until it gets another site) */
	bool posted;                    /* TOWER: it stands on the watchpost (gx, gy, gz): it does not move, goblins leave it alone, it shoots */
	bool sieging;                   /* a goblin: it is hitting a block that is in its way */
};

static inline float bl_mob_height(const struct bl_mob *b)       /* the height of this one's box (a brute is taller than a goblin) */
{
	return b->brute ? BL_BRUTE_H : bl_mob_h(b->kind);
}

struct bl_mobs {
	struct bl_mob mob[BL_MOBS_MAX];
	bool enabled;                   /* false: nothing spawns and the ones there are go */
	int cap;                        /* how many there may be at a time (1 .. BL_MOBS_MAX) */
	bool keep_on;                   /* there is a keep: the goblins that are not near the player go to it and hit it */
	float keep[3];                  /* the middle of the bottom of it */
	uint32_t rng;
	float spawn_t, acc;
	int kills, spawned, burned;
	/* a wave: the goblins of one night, coming to the keep from all round (not to the player) */
	bool wave_on;                   /* a wave is on: while there is a keep the goblins come from round IT, `wave_left` of them in all */
	int wave_left;                  /* goblins still to appear */
	int wave_cap;                   /* how many may be there at a time */
	int wave_size;                  /* how many the wave was made of */
	int wave_brutes;                /* the last this many goblins of the wave are brutes */
	struct bl_site site[BL_SITES_MAX];      /* what is being built */
	struct bl_arrow {
		float t;                        /* seconds of flight left (0 = none) */
		float a[3], b[3];               /* from the bow to the goblin */
	} arrow[BL_ARROWS_MAX];
	struct bl_damaged {
		int x, y, z;
		float dmg;                      /* what the goblins have taken of its bl_siege_hp (0 = a free entry) */
		float age;                      /* seconds since the last blow */
	} damaged[BL_DAMAGED_MAX];
};

/* what a call of `bl_mobs_step` did to the player */
struct bl_mobs_result {
	float damage;                   /* hit points taken from the player */
	float kbx, kbz;                 /* the push the blows gave him (a speed in blocks a second) */
	int hits;                       /* blows landed */
	int born, burned;               /* mobs that appeared, mobs that the sun killed */
	float keep_damage;              /* hit points taken from the keep (BL_GOBLIN_KEEP_HIT a blow) */
	int keep_hits;
	int workers_lost;               /* workers the goblins killed */
	int soldiers_lost;              /* soldiers the goblins killed */
	int born_brutes;                /* brutes that appeared (they are in `born`, too) */
	int slain;                      /* goblins the soldiers killed */
	int sword_hits;                 /* blows the soldiers landed */
	int nbreak;                     /* blocks the workers have finished: the caller takes them away (bl_break) and keeps what they leave */
	struct bl_break { int x, y, z; } brk[64];
	int nplace;                     /* blocks the workers have put: the caller sets them (bl_set), if the cell is still empty */
	struct bl_cell place[16];
	int nbuilt;                     /* buildings that are finished (their sites are free again): which plans */
	int built[BL_SITES_MAX];
	int nsmash;                     /* blocks the goblins have broken: the caller takes them away */
	struct bl_break smash[16];
	int siege_hits;                 /* blows at blocks */
	int shots;                      /* arrows shot from the watchtowers */
};

void bl_mobs_init(struct bl_mobs *m, uint32_t seed);
void bl_mobs_clear(struct bl_mobs *m);                      /* every mob gone (the player died, a new world) */
void bl_mobs_set_keep(struct bl_mobs *m, bool on, float x, float y, float z);    /* where the keep is (the middle of its bottom), or that there is none */
int bl_mobs_count(const struct bl_mobs *m);                 /* every kind */
int bl_mobs_count_kind(const struct bl_mobs *m, int kind);
/* A wave: `n` goblins in all, at most `cap` of them at a time, coming out of the ground 20 to 32 blocks from the keep while it is night (it ends by itself at dawn).
 * The last `brutes` of them are brutes. */
void bl_mobs_start_wave(struct bl_mobs *m, int n, int cap, int brutes);
int bl_mobs_count_brutes(const struct bl_mobs *m);
void bl_mobs_end_wave(struct bl_mobs *m);
int bl_mobs_wave_remaining(const struct bl_mobs *m);        /* goblins still to come + goblins alive */
void bl_mobs_clear_kind(struct bl_mobs *m, int kind);       /* only that kind (the player died: the goblins go, the workers stay) */

/* One step of every mob. `player` = the feet of the player; `pl_alive` false = nobody to hunt. `time_h` is the hour of the day (blocksview.h): goblins appear
 * when `bl_daylight` is under 0.3 (14 to 26 blocks from the player, on the surface in the open; when workers or soldiers are more than BL_UNIT_APART from him,
 * up to BL_RAID_CAP more 10 to 20 blocks from those; in a wave every third comes to units that far from the keep), burn in the sun and thin out in the
 * day. A goblin is dropped when it is far from the player, the keep and every unit. */
void bl_mobs_step(struct bl_mobs *m, const struct bl_world *w, const float player[3], bool pl_alive, float time_h, float dt, struct bl_mobs_result *out);

/* the goblin the ray hits first within `reach` (-1 = none, *t = the distance); a box a little bigger than the body, so that a blow does not miss by a hair. Workers are not hit. */
int bl_mobs_pick(const struct bl_mobs *m, const float o[3], const float dir[3], float reach, float *t);
/* a blow: `damage` hit points, knocked back along the horizontal direction (dx, dz) (any length); true when it died */
bool bl_mobs_hurt(struct bl_mobs *m, int i, float damage, float dx, float dz);
/* does a mob stand in the cell (a block cannot be put into it)? */
bool bl_mobs_hit_cell(const struct bl_mobs *m, int x, int y, int z);
/* put one at a place: false when there is no room, the spot is inside a block, or there are too many of the kind (a worker: BL_WORKER_MAX) */
bool bl_mobs_add(struct bl_mobs *m, const struct bl_world *w, int kind, float x, float y, float z);
/* a brute (a goblin of the big kind); false when there is no room for one 2.1 m tall */
bool bl_mobs_add_brute(struct bl_mobs *m, const struct bl_world *w, float x, float y, float z);

/* ---- workers: friendly, they do what they are told ------------------------------------------------------------------------ */

/* The worker or soldier the ray hits first within `reach` (-1 = none, *t = the distance): for choosing one with the pointer. */
int bl_units_pick(const struct bl_mobs *m, const float o[3], const float dir[3], float reach, float *t);
/* Walk to (x, z); it is idle when it gets there or has got no nearer for 4 s (a wall in the way). A soldier takes (x, z) as its post: it goes there, stands there and
 * goes after the goblins that come near it, then back. */
void bl_mobs_order_move(struct bl_mobs *m, int i, float x, float z);
/* Go and break the block (bx, by, bz) and then the next of its kind within 6 blocks, as long as there is one it can reach: it stands beside the block (one it can reach
 * with its hands: about 2 blocks up or down) and takes 1.5 times the time the player would, a tree (the lowest log is taken, the whole trunk goes with it and the leaves
 * round its crown) 1.8 s and 0.6 s a log more. The blocks it has finished come back in `bl_mobs_result.brk`. False (and nothing is changed) when there is no place to
 * stand or the block cannot be broken. */
bool bl_mobs_order_gather(struct bl_mobs *m, const struct bl_world *w, int i, int bx, int by, int bz);
void bl_mobs_order_stop(struct bl_mobs *m, int i);

/* The player is a body to the mobs (call it after his step): one he walks into gives way (a worker or a soldier all the way, a goblin half), as far as the world
 * lets it, and he steps back by the rest; one on a watchtower does not move. Nobody stands on anybody: they slide apart sideways. */
void bl_mobs_push_player(struct bl_mobs *m, const struct bl_world *w, struct bl_player *p);

/* ---- building ---------------------------------------------------------------------------------------------------------- */

/* A site (bl_plan_lay) to be built: its index, -1 when there are BL_SITES_MAX already or it has no cells. */
int bl_mobs_add_site(struct bl_mobs *m, const struct bl_site *s);
void bl_mobs_remove_site(struct bl_mobs *m, int site);
int bl_mobs_count_sites(const struct bl_mobs *m);
/* A worker builds the site: it walks beside it and puts a block every BL_BUILD_TIME seconds, lowest first, none into a body; the blocks come back in
 * `bl_mobs_result.place`. When the site is done it goes on to the nearest other one within 48 blocks. False when it is no worker or no site. */
bool bl_mobs_order_build(struct bl_mobs *m, const struct bl_world *w, int i, int site);
/* A soldier mans the watchpost at (x, y, z): it walks there, climbs up (it stands on the post), shoots the goblins it sees and the goblins do not go for it.
 * Another order brings it down. False when it is no soldier, there is no post, the place on top is not free or another soldier is on it or going to it. */
bool bl_mobs_order_tower(struct bl_mobs *m, const struct bl_world *w, int i, int x, int y, int z);

/* ---- the picture ---------------------------------------------------------------------------------------------------- */

struct bl_mob_hit {
	float t;                        /* distance along the (unit) ray */
	int id;                         /* which mob */
	float rgb[3];                   /* its colour there, 0 .. 1, lit and shaded */
};

/* Light every mob for a frame: the sky's light (the hour, and whether the sky is over it) and the light of torches and lamps near it. */
void bl_mobs_light(struct bl_mobs *m, const struct bl_world *w, float time_h);
/* the light on a body that stands at (x, y, z) (the feet), per colour: what bl_mobs_light gives a mob (the player's body uses it too) */
void bl_mobs_light_at(const struct bl_world *w, float time_h, float x, float y, float z, float lit[3]);
/* the nearest mob the ray (origin o, unit direction d) hits within tmax; false when none */
bool bl_mobs_ray(const struct bl_mobs *m, const float o[3], const float d[3], float tmax, struct bl_mob_hit *hit);

#endif
