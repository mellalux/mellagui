/* blocks.h - a world of blocks as a plain engine: no window, no clock, no I/O except the save file.
 *
 * Used by mellagui-blocks (the window, stage 2) and tested by tests/blocks_test.c. The picture is made by
 * blocksview.[ch], which only reads a `struct bl_world`.
 *
 * Coordinates: x and z are horizontal, y is up, one block is one unit. A cell (x, y, z) is the cube
 * [x, x + 1) x [y, y + 1) x [z, z + 1). The world is a finite box of w x d x h cells (default 256 x 256 x 64); outside
 * it sideways is an invisible wall (`bl_get` says bedrock), below it is bedrock, above it is air.
 * Index: ((z * w + x) * h + y), so y runs fastest: a column of 64 blocks is one cache line, which is what a ray going up or down
 * touches.
 *
 * The terrain is made from the seed alone (`bl_world_new`): value noise hills, mountains with snow, a sea with beaches,
 * tunnels (two noise fields that are both near 0.5), trees on a jittered grid. The same seed is the same world, so a
 * save file holds only the cells that differ from the generated one (`bl_save`).
 *
 * Directions: a face is 0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z (the way its outward normal points). A ray that enters a cell
 * through its -x face moves towards +x; `struct bl_hit.face` names the face it came through, the cell in front of it
 * (px, py, pz) is where a new block goes.
 *
 * Looking: yaw 0 looks along +z, a bigger yaw turns towards +x; pitch > 0 looks up.
 *   forward = (cos p sin yaw, sin p, cos p cos yaw)   right = (cos yaw, 0, -sin yaw)
 *
 * Threads: a world is plain memory; reading it from several threads is fine while nobody edits it.
 */
#ifndef MELLAGUI_BLOCKS_H
#define MELLAGUI_BLOCKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum bl_block {
	BL_AIR = 0, BL_GRASS, BL_DIRT, BL_STONE, BL_SAND, BL_LOG, BL_LEAVES, BL_PLANKS, BL_GLASS,
	BL_WATER, BL_BEDROCK, BL_BRICK, BL_SNOW, BL_COBBLE,
	BL_TORCH,               /* a stick with a flame: stands on the floor of its cell, light 14, can be walked through */
	BL_LAMP,                /* a block that shines (light 15) */
	BL_FLOW1, BL_FLOW2, BL_FLOW3, BL_FLOW4, BL_FLOW5, BL_FLOW6, BL_FLOW7,   /* water that flows: how far it has come from a source (1 = just left it, 7 = the last cell) */
	BL_KEEP,                /* the keep: a tower of three of them (made by the game, never put by hand, never broken by the player): what the goblins attack */
	BL_COUNT
};

enum { BL_PX = 0, BL_NX, BL_PY, BL_NY, BL_PZ, BL_NZ };
enum { BL_FACE_TOP = 0, BL_FACE_SIDE, BL_FACE_BOTTOM, BL_FACE_KINDS };   /* which picture a face shows */

extern const int8_t bl_face_n[6][3];                  /* the outward normal of each face */

const char *bl_block_name(int block);                 /* "grass"; "?" for a number that is no block */

/* what a block is (the renderer and the physics both read it). WATER is the source (the sea, a block put by hand); FLOW1..7 is water that has flowed. */
static inline bool bl_water(int b)        { return b == BL_WATER || (b >= BL_FLOW1 && b <= BL_FLOW7); }
static inline int  bl_water_level(int b)  { return b == BL_WATER ? 0 : b - BL_FLOW1 + 1; }                /* 0 for a source, 1 .. 7 for flowing water (b must be water) */
static inline bool bl_solid(int b)        { return b != BL_AIR && !bl_water(b) && b != BL_TORCH; }          /* stops the player */
static inline bool bl_opaque(int b)       { return b != BL_AIR && !bl_water(b) && b != BL_GLASS && b != BL_TORCH; }   /* stops light and sight */
static inline bool bl_targetable(int b)   { return b != BL_AIR && !bl_water(b); }                           /* can be aimed at and broken */
static inline bool bl_blocks_light(int b) { return bl_opaque(b) && b != BL_LEAVES; }                       /* shades the cells below it from the sky (leaves let most light through) */
static inline int  bl_emission(int b)     { return b == BL_LAMP ? 15 : b == BL_TORCH ? 14 : 0; }            /* the light a block gives (0 .. 15) */
static inline bool bl_breakable(int b)    { return b != BL_AIR && !bl_water(b) && b != BL_BEDROCK && b != BL_KEEP && b < BL_COUNT; }   /* what the player's hand can take away */

/* How long it takes to break a block by hand, in seconds (survival; creative breaks at once), and what it leaves behind (0 = nothing; grass leaves dirt, stone leaves
 * cobble, glass and leaves break to nothing). Both 0 for what cannot be broken. */
float bl_hardness(int block);
int bl_drop(int block);

#define BL_MIN_SIZE 16
#define BL_MAX_SIZE 1024
#define BL_MIN_H 32
#define BL_MAX_H 128

struct bl_world {
	int w, d, h;            /* cells along x, z and y */
	uint32_t seed;
	uint8_t kind;           /* 0 = terrain from the seed, 1 = flat (a floor `ground` layers thick, no sea) */
	int ground;             /* kind 1: the number of filled layers */
	int sea;                /* the highest cell that holds sea water (-1: no sea) */
	uint8_t *b;             /* w * d * h blocks */
	int16_t *top;           /* per column: y of the highest block that blocks light (opaque, not leaves), -1 = none: what shades the cells below it */
	int16_t *hi;            /* per column: y of the highest block that is not air, -1 = none (what a ray has to look at) */
	int maxhi;              /* the biggest `hi`; nothing exists from y = maxhi + 1 up */
	uint32_t rev;           /* counts changes: a window repaints when it moved */
	uint8_t *light;         /* per cell: the light of torches and lamps, 0 .. 15 (the sky's light is worked out from `top`, not stored) */
	uint32_t *wq;           /* cells whose water has to be looked at again (bl_world_tick) */
	size_t wq_n, wq_cap;
};

/* A world from a seed (kind 0). NULL when the size is outside BL_MIN_SIZE..BL_MAX_SIZE x BL_MIN_H..BL_MAX_H. */
struct bl_world *bl_world_new(uint32_t seed, int w, int d, int h);
/* A flat world: bedrock, stone, dirt and a layer of grass, `ground` layers in all (2..h - 8), no sea. For tests and a plain mode. */
struct bl_world *bl_world_flat(int w, int d, int h, int ground);
void bl_world_free(struct bl_world *w);

static inline size_t bl_index(const struct bl_world *w, int x, int y, int z)
{
	return ((size_t)z * (size_t)w->w + (size_t)x) * (size_t)w->h + (size_t)y;
}

static inline bool bl_inside(const struct bl_world *w, int x, int y, int z)
{
	return (unsigned)x < (unsigned)w->w && (unsigned)y < (unsigned)w->h && (unsigned)z < (unsigned)w->d;
}

/* The block at a cell. Sideways outside the world and below it is bedrock (a wall), above it air. */
static inline uint8_t bl_get(const struct bl_world *w, int x, int y, int z)
{
	if ((unsigned)x >= (unsigned)w->w || (unsigned)z >= (unsigned)w->d || y < 0)
		return BL_BEDROCK;
	if (y >= w->h)
		return BL_AIR;
	return w->b[bl_index(w, x, y, z)];
}

/* Change a cell: false when it is outside the world, the cell is bedrock or the block number is not a block (bedrock
 * cannot be broken or made). Keeps `top`, `hi` and `maxhi` right and counts `rev`. */
bool bl_set(struct bl_world *w, int x, int y, int z, int block);

/* y of the highest opaque block of a column, -1 = none or outside */
int bl_top(const struct bl_world *w, int x, int z);

/* A good place to start: the feet position (the middle of a cell, standing on dry ground near the middle of the world). */
void bl_spawn(const struct bl_world *w, float out[3]);

uint64_t bl_hash(const struct bl_world *w);           /* FNV-1a of the size and every cell (tests: same world or not) */

/* The light of torches and lamps in a cell, 0 .. 15 (0 outside the world). Spreads one less for every cell, round corners and through glass, water and air,
 * not through opaque blocks; `bl_set` keeps it right (a removal takes light away from the cells that had it from there only). */
static inline int bl_light(const struct bl_world *w, int x, int y, int z)
{
	return bl_inside(w, x, y, z) ? w->light[bl_index(w, x, y, z)] : 0;
}
void bl_lights_rebuild(struct bl_world *w);           /* all light from the emitters again (after a load; the tests compare it with what the edits made) */

/* Water that flows. Every change of a cell sets its neighbours to be looked at; `bl_world_tick` looks at up to `budget` of them (at most one step of the
 * flow, so call it a few times a second): a cell of air or flowing water takes the level it is fed with - 1 from water above it (it falls), else 1 + the lowest
 * level of a side neighbour (a source feeds the sides always, flowing water only when it rests on something that is neither air nor water: over air or water it falls into it) - and becomes air when nothing feeds it (the
 * flow dries up behind a source that was taken away), nothing goes beyond level 7. Returns how many cells changed. */
int bl_world_tick(struct bl_world *w, int budget);
size_t bl_water_pending(const struct bl_world *w);    /* cells waiting to be looked at */

/* ---- pointing at a block ------------------------------------------------------------------------------------------ */

struct bl_hit {
	bool hit;
	int x, y, z;            /* the block hit */
	int face;               /* the face of it that the ray came through (BL_PX ...) */
	int px, py, pz;         /* the cell in front of that face: where a block is put */
	float t;                /* distance from the origin along the (unit) direction */
};

/* The first targetable block along the ray within `reach`; water and air do not count and the cell the ray starts in is
 * not looked at. `dir` is turned into a unit vector. A ray that leaves the world sideways or below ends; one above the world
 * goes on as long as it comes down (a player flying over the top can aim at a tower). */
bool bl_pick(const struct bl_world *w, const float o[3], const float dir[3], float reach, struct bl_hit *hit);

/* Break the block of a hit (-> air); false if it cannot be broken (bedrock). */
bool bl_break(struct bl_world *w, const struct bl_hit *h);

/* ---- the player --------------------------------------------------------------------------------------------------- */

#define BL_PLAYER_W 0.6f
#define BL_PLAYER_H 1.8f
#define BL_EYE 1.62f             /* the eyes above the feet (1.5 while sneaking) */
#define BL_REACH 5.0f

struct bl_player {
	float x, y, z;          /* the feet: the middle of the bottom of the box */
	float vx, vy, vz;
	float yaw, pitch;
	bool on_ground, in_water, flying, sneaking;
};

struct bl_input {
	float fwd, strafe;      /* -1..1: forward / back, right / left */
	bool jump, sneak, sprint;
};

void bl_player_init(struct bl_player *p, const struct bl_world *w);        /* at the spawn, looking along +z */
void bl_player_step(const struct bl_world *w, struct bl_player *p, const struct bl_input *in, float dt);
void bl_player_eye(const struct bl_player *p, float out[3]);
void bl_player_look(const struct bl_player *p, float out[3]);              /* the unit vector the player looks along */
bool bl_player_overlaps(const struct bl_player *p, int x, int y, int z);   /* does the box touch that cell? */

/* Put `block` in the cell in front of a hit: false when it would be outside the world, the cell is not air or water, the block is not a placeable one (air,
 * bedrock, flowing water), the player stands in that cell, or it is a torch with nothing solid under it. Water put by hand is a source. */
bool bl_place(struct bl_world *w, const struct bl_hit *h, int block, const struct bl_player *p);

/* ---- bodies that are not the player (Tom, Filip, the mobs): one box, the same collision as the player's ------------------ */

/* does the box (hw half width, h height, feet at (x, y, z)) touch a solid block? */
bool bl_box_hits(const struct bl_world *w, float x, float y, float z, float hw, float h);
/* The top of the highest solid block under the feet (the middle of the box and four points inside its corners) within `down` below them: its height, -1e9 when
 * there is none. */
float bl_box_support(const struct bl_world *w, float x, float y, float z, float hw, float down);
/* Move the box along one axis (0 x, 1 y, 2 z) by d, a quarter block at a time, and snap it to the face it meets (a 0.001 margin): true when it was stopped. A box
 * that already is inside a block is left where it is when the step would not free it. */
bool bl_box_move(const struct bl_world *w, float *x, float *y, float *z, int axis, float d, float hw, float h);

/* ---- saving ------------------------------------------------------------------------------------------------------- */

/* Writes the seed, the size and every cell that differs from the world the seed makes (file mode 0600, written to a temporary
 * name and renamed). 0 = ok, -1 = failed. */
int bl_save(const struct bl_world *w, const char *path);
/* Reads it back (makes the world from the seed and lays the edits over it). NULL with *err set to a message: a missing file, a bad
 * header or a size that is no size, an entry out of order or out of range, a block that is none, a file that ends early. */
struct bl_world *bl_load(const char *path, const char **err);

#endif
