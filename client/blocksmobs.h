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
 * A mob that cannot walk straight to where it is going finds a way round (blockspath.h): workers and soldiers through gates, goblins when the way gets them there
 * and is not much longer than the straight line - else they go straight on and break what a hand has made.
 *
 * Not done: shooting goblins, mobs that spawn in caves or by day.
 */
#ifndef MELLAGUI_BLOCKSMOBS_H
#define MELLAGUI_BLOCKSMOBS_H

#include "blocks.h"
#include "blocksbuild.h"
#include "blockspath.h"
#include <math.h>

#define BL_MOBS_MAX 72                  /* goblins, workers, soldiers and the rival clan's people together */
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

/* The rival clan, the Red Banner: its guards hold a camp round its keep. They fight the player, his people and the goblins that come within sight of their post.
 * BL_GUARD_MAX is for the camp's (those on the ground and the archers on its towers); the raiders (below) are counted apart. */
#define BL_GUARD_MAX 11
#define BL_GUARD_HP 18.0f               /* (a soldier has 14: they are the better fighters, and they are at home) */
#define BL_GUARD_HIT 3.0f               /* hit points a blow of a guard takes (from the player, his people or a goblin) */
#define BL_GUARD_COOLDOWN 0.9f
#define BL_GUARD_SPEED 2.8f
#define BL_GUARD_SEE 14.0f              /* how far a guard sees an enemy ... */
#define BL_GUARD_LEASH 20.0f            /* ... and how far from its post it goes after one */
#define BL_RIVAL_AT_KEEP 14.0f          /* a soldier of the player whose post is this near the middle of the rival keep attacks it when no enemy is about */
#define BL_SOLDIER_KEEP_HIT 4.0f        /* what a blow of his takes from it */

/* Their raiding parties: guards that march from the camp to a place the caller names (bl_mobs_set_raid), fight whom they meet on the way, hammer at made blocks
 * that stop them and, when the player's keep stands (bl_mobs_set_keep), strike it. With no keep to strike they go after the player and his people near that
 * place. One that finds nothing to do, gets no nearer or has been out too long goes home. */
#define BL_RAIDER_MAX 6                 /* (eight soldiers, the most there can be, hold a keep against six of them) */
#define BL_RAIDER_KEEP_HIT 2.0f         /* what a blow of a raider takes from the player's keep */
#define BL_RAID_HUNT 40.0f              /* with no keep to strike: how far from that place a raider goes after somebody */
#define BL_RAID_IDLE 60.0f              /* seconds with nothing to strike, or without getting nearer, after which a raider goes home */
#define BL_RAID_LIMIT 300.0f            /* seconds a party stays out at most before it turns back ... */
#define BL_RAID_BACK 180.0f             /* ... and seconds more after which one that has not got home is gone */
#define BL_RAID_HOME 14.0f              /* this near their keep a raider that goes home is back */

/* Their labourers: they fetch material to the camp's keep (a tree is felled, else stone that sticks out of the ground is cut, else they forage) and build what
 * the camp is building (bl_mobs_set_rsite). They do not fight: with an enemy near they run for the keep. */
#define BL_RWORKER_MAX 4
#define BL_RWORKER_HP 6.0f
#define BL_RWORKER_SPEED 2.4f
#define BL_RWORKER_FEAR 7.0f            /* an enemy this near sends one running home ... */
#define BL_RWORKER_SCARE 8.0f           /* ... for this many seconds, and work this near an enemy (and 3 more) is not taken */
#define BL_RWORKER_RANGE 56             /* how far from their keep they look for trees and stone ... */
#define BL_RWORKER_CAMP 18              /* ... and how near it they leave the ground alone (their camp) */
#define BL_RWORKER_TREE_TIME 8.0f       /* seconds at a tree, at stone, foraging */
#define BL_RWORKER_STONE_TIME 6.0f
#define BL_RWORKER_FORAGE_TIME 14.0f
#define BL_RWORKER_TREE_LOAD 2          /* the material a trip brings (stone and foraging: 1) */
#define BL_RWORKER_BUILD_TIME 1.0f      /* seconds to put one block of the camp's building */
#define BL_RWORKER_BUILDERS 2           /* how many of them build at a time, the others fetch */
#define BL_RWORKER_FAILS 4              /* one that has given up this often without bringing anything in is gone (another comes) */
#define BL_RSKIP_MAX 12                 /* places they could not get to and do not try again for a while */
#define BL_RWORKER_GOLD_TIME 25.0f      /* seconds at a piece of gold (a trip brings one) */
#define BL_RWORKER_MINERS 1             /* how many of them go for gold at a time, while it is wanted */
#define BL_RWORKER_GOLD_RANGE 160.0f    /* a deposit this far from their keep is still worked */
#define BL_DEPOSITS_MAX 8

/* The weapons of the chiefs: the player and the one of the rival clan, who is called the Red Captain. */
enum bl_weapon { BL_WP_SWORD = 0, BL_WP_SPEAR, BL_WP_HAMMER, BL_WP_BOW, BL_WP_FIRE, BL_WP_LASER, BL_WP_COUNT };
enum { BL_SHOT_ARROW = 0, BL_SHOT_FIRE, BL_SHOT_LASER };

struct bl_weapon_info {
	const char *name;
	float damage;                   /* in the player's hand: what a blow takes from a body ... */
	float reach;                    /* ... how far it goes ... */
	float cooldown;                 /* ... and the seconds between two */
	float keep_damage;              /* what it takes from the rival keep */
	float splash;                   /* > 0: everybody this near the one hit is hurt, too */
	int shot;                       /* what flies (BL_SHOT_*), -1: a blow at arm's (or shaft's) length */
	float c_damage, c_cooldown, c_keep;     /* the same in the Red Captain's hand */
};

const struct bl_weapon_info *bl_weapon(int weapon);     /* the sword for a number that is none */

/* The Red Captain: he flies, takes the best of the weapons he has for the distance, guards the camp and leads raiding parties. */
#define BL_CHIEF_HP 70.0f
#define BL_CHIEF_H 1.90f
#define BL_CHIEF_SPEED 3.2f             /* on foot ... */
#define BL_CHIEF_FLY 6.5f               /* ... and in the air */
#define BL_CHIEF_SEE 30.0f              /* how far he sees an enemy ... */
#define BL_CHIEF_LEASH 38.0f            /* ... and how far from his keep he goes after one when he guards it */

/* the distance along the ground from (x, z) to the side of a keep whose middle is at `keep` (0 inside its square) */
#define BL_KEEP_EDGE ((float)BL_KEEP_R + 0.5f)
static inline float bl_keep_dist(const float keep[3], float x, float z)
{
	const float dx = (x > keep[0] ? x - keep[0] : keep[0] - x) - BL_KEEP_EDGE, dz = (z > keep[2] ? z - keep[2] : keep[2] - z) - BL_KEEP_EDGE;
	return sqrtf((dx > 0.0f ? dx * dx : 0.0f) + (dz > 0.0f ? dz * dz : 0.0f));
}

#define BL_BUILD_TIME 0.5f              /* seconds a worker takes to put one block of a building */
#define BL_BOW_RANGE 20.0f              /* a soldier on a watchtower shoots the goblins this near (horizontally) that it can see */
#define BL_BOW_DAMAGE 3.0f              /* four arrows kill a goblin */
#define BL_BOW_COOLDOWN 1.5f
#define BL_ARROWS_MAX 16                /* arrows, fireballs and beams in the air (their picture only: the hit counts at once) */
#define BL_ARROW_TIME 0.25f
#define BL_FIRE_TIME 0.50f              /* a fireball flies for 0.3 s and bursts */
#define BL_LASER_TIME 0.14f
#define BL_DAMAGED_MAX 24               /* blocks the goblins have started on */
#define BL_PATH_BUDGET 5000             /* places that all the searches for a way may look at in one call of bl_mobs_step */

enum bl_mob_kind { BL_MOB_NONE = 0, BL_MOB_GOBLIN, BL_MOB_WORKER, BL_MOB_SOLDIER, BL_MOB_GUARD, BL_MOB_RWORKER, BL_MOB_CHIEF };    /* (the last two: the rival clan's labourer and its chief) */
enum bl_order { BL_ORDER_IDLE = 0, BL_ORDER_MOVE, BL_ORDER_GATHER, BL_ORDER_BUILD, BL_ORDER_TOWER };

static inline float bl_mob_h(int kind)                         /* the height of a kind's box */
{
	return kind == BL_MOB_WORKER || kind == BL_MOB_RWORKER ? BL_WORKER_H : kind == BL_MOB_SOLDIER || kind == BL_MOB_GUARD ? BL_SOLDIER_H : kind == BL_MOB_CHIEF ? BL_CHIEF_H : BL_GOBLIN_H;
}

static inline bool bl_mob_is_unit(int kind)                    /* the player's: a worker or a soldier */
{
	return kind == BL_MOB_WORKER || kind == BL_MOB_SOLDIER;
}

static inline bool bl_mob_is_rival(int kind)                   /* of the rival clan: a guard (raiders among them), a labourer or the chief */
{
	return kind == BL_MOB_GUARD || kind == BL_MOB_RWORKER || kind == BL_MOB_CHIEF;
}

static inline bool bl_mob_is_foe(int kind)                     /* an enemy of the player: a goblin or one of the rival clan (the sword hits it; a gate stops it) */
{
	return kind == BL_MOB_GOBLIN || bl_mob_is_rival(kind);
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
	bool sieging;                   /* a goblin or a raider: it is hitting a block that is in its way */
	bool raider;                    /* a guard of the rival clan out on a raid (BL_RAIDER_*) */
	bool retreat;                   /* ... on its way home */
	float idle, since;              /* a raider: seconds it has had nothing to strike, and when it turned back (its age); a labourer: `idle` = seconds of its fright left */
	int load;                       /* a labourer of the rival clan: the material it carries ... */
	int errand;                     /* ... the order it is on (to notice that it was given up) */
	int fails;                      /* ... and how often it has given up since it last brought something in */
	/* the way to where it is going, when it cannot walk straight there (blockspath.h) */
	struct bl_way way[BL_PATH_MAX];
	int nway, iway;                 /* the turns of it, and the one it walks to */
	bool pathing;                   /* it is walking along it now */
	bool way_full;                  /* it leads to the goal (else only as near as was found) */
	bool direct;                    /* the straight line was open when it was last looked at ... */
	float line_t, repath_t, path_age;       /* ... seconds until it is looked at again, until a way may be looked for again, since this one was found */
	float goal_x, goal_z;           /* what the way was found for */
	float way_d;                    /* the nearest it has been to the turn it walks to */
	float lost;                     /* seconds an order has not brought it nearer than ever before */
	int gold_load;                  /* a labourer of the rival clan: the gold it carries */
	float raid0;                    /* a raider: its age when it set out */
	int weapon;                     /* the chief: what he holds (enum bl_weapon) */
	bool flying;                    /* ... and whether he is in the air */
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
	bool rival_on;                  /* the rival clan's keep stands: the player's soldiers that are sent to it attack it */
	float rkeep[3];                 /* the middle of the bottom of it */
	float raid_at[3];               /* where a raiding party of theirs is bound (the player's keep, or him) */
	bool rkeep_down;                /* their keep is down for now (they are building it again): it cannot be attacked */
	bool rgold_wanted;              /* their labourers go for gold, too */
	struct { int x, y, z; } deposit[BL_DEPOSITS_MAX];      /* where gold lies (bl_mobs_set_deposits) */
	int ndeposits;
	uint32_t chief_arms;            /* the weapons the chief has, bit 1 << BL_WP_* (the sword always) */
	struct bl_site rsite;           /* what the rival camp is building (on = something): its labourers put the blocks */
	struct { int x, z; } rskip[BL_RSKIP_MAX];       /* places they could not get to */
	int nrskip;                     /* how many have been noted (the oldest are written over) */
	int path_budget;                /* places that may still be looked at in this call (BL_PATH_BUDGET) */
	int paths;                      /* ways that have been looked for since the start */
	struct bl_site site[BL_SITES_MAX];      /* what is being built */
	struct bl_arrow {
		float t;                        /* seconds that are left of it (0 = none) */
		float a[3], b[3];               /* from the bow to the goblin */
		int kind;                       /* BL_SHOT_* */
	} arrow[BL_ARROWS_MAX];
	struct bl_damaged {
		int x, y, z;
		float dmg;                      /* what the goblins have taken of its bl_siege_hp (0 = a free entry) */
		float age;                      /* seconds since the last blow */
	} damaged[BL_DAMAGED_MAX];
};

struct bl_break { int x, y, z; };

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
	float rkeep_damage;             /* hit points the player's soldiers took from the rival keep */
	int guards_slain;               /* guards the player's soldiers and archers killed (raiders among them) */
	int raiders_slain;              /* ... and how many of those were raiders */
	int raiders_home;               /* raiders that went home (they are gone) */
	int rworkers_slain;             /* labourers of the rival clan the player's soldiers and archers killed (not in guards_slain) */
	int delivered;                  /* material their labourers brought to their keep */
	int delivered_gold;             /* ... and gold */
	int chief_slain;                /* the player's soldiers or archers brought down the rival chief */
	int chief_hits;                 /* blows and shots of the chief at the player; they are in `hits` and `damage`, too */
	int rbuilt;                     /* 1: what their camp was building is finished (bl_mobs.rsite is off again) */
	int nraze;                      /* blocks their labourers have taken from the land: the caller takes them away */
	struct bl_break raze[64];
	int guard_hits;                 /* blows of the guards (and their archer) at the player; they are in `hits` and `damage`, too */
	int nbreak;                     /* blocks the workers have finished: the caller takes them away (bl_break) and keeps what they leave */
	struct bl_break brk[64];
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
void bl_mobs_set_rival(struct bl_mobs *m, bool on, float x, float y, float z);   /* where the rival keep is, or that there is none */
void bl_mobs_set_raid(struct bl_mobs *m, float x, float y, float z);    /* where the raiders are bound */
void bl_mobs_set_deposits(struct bl_mobs *m, const int (*at)[3], int n); /* where gold lies, for their labourers */
bool bl_mobs_add_chief(struct bl_mobs *m, const struct bl_world *w, float x, float y, float z);   /* the rival chief (there is one at most) */
int bl_mobs_chief(const struct bl_mobs *m);                 /* his index, -1: he is not about */
bool bl_mobs_chief_raid(struct bl_mobs *m);                 /* he sets out with the raiders (bl_mobs_set_raid); false when he is not about */
/* something flies from a to b (a picture only): an arrow, a fireball, a beam */
void bl_mobs_shot(struct bl_mobs *m, const float a[3], const float b[3], int kind);
/* every enemy of the player within `radius` of `at` takes `damage`; how many died */
int bl_mobs_splash(struct bl_mobs *m, const float at[3], float radius, float damage, int spare);
/* what the rival camp is building now (its labourers come and put the blocks, bl_mobs_result.place; rbuilt says when it is whole); NULL: nothing */
void bl_mobs_set_rsite(struct bl_mobs *m, const struct bl_site *s);
/* a raider of the rival clan; false when there are BL_RAIDER_MAX already or there is no room for him there */
bool bl_mobs_add_raider(struct bl_mobs *m, const struct bl_world *w, float x, float y, float z);
int bl_mobs_count_raiders(const struct bl_mobs *m);
void bl_mobs_raid_retreat(struct bl_mobs *m);               /* every raider turns back */
/* a guard of the rival clan up on a watchtower of theirs (its post at x, y, z), shooting; false when there is no room */
bool bl_mobs_add_archer(struct bl_mobs *m, const struct bl_world *w, int x, int y, int z);
void bl_mobs_clear_kind(struct bl_mobs *m, int kind);       /* only that kind (the player died: the goblins go, the workers stay) */

/* One step of every mob. `player` = the feet of the player; `pl_alive` false = nobody to hunt. `time_h` is the hour of the day (blocksview.h): goblins appear
 * when `bl_daylight` is under 0.3 (14 to 26 blocks from the player, on the surface in the open; when workers or soldiers are more than BL_UNIT_APART from him,
 * up to BL_RAID_CAP more 10 to 20 blocks from those; in a wave every third comes to units that far from the keep), burn in the sun and thin out in the
 * day. A goblin is dropped when it is far from the player, the keep and every unit. */
void bl_mobs_step(struct bl_mobs *m, const struct bl_world *w, const float player[3], bool pl_alive, float time_h, float dt, struct bl_mobs_result *out);

/* the enemy (a goblin or a rival guard) the ray hits first within `reach` (-1 = none, *t = the distance); a box a little bigger than the body, so that a blow
 * does not miss by a hair. The player's own are not hit. */
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
