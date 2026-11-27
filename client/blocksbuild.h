/* blocksbuild.h - what can be built in the block world: the plans of the buildings, a building site, and the see-through picture of what is planned.
 *
 * Plain code like blocks.[ch]: values in, values out. A PLAN is a barracks, a watchtower, a wall or a gate; laid out at a place of the world it becomes a SITE, a
 * list of cells with the block each wants. Workers (blocksmobs.[ch]) fill the cells one at a time; the game applies the blocks. The GHOSTS are the cells that are
 * planned but not there yet (and the preview under the pointer), drawn by blocksview.c as see-through boxes with brighter edges.
 */
#ifndef MELLAGUI_BLOCKSBUILD_H
#define MELLAGUI_BLOCKSBUILD_H

#include "blocks.h"

enum bl_plan { BL_PLAN_BARRACKS = 0, BL_PLAN_TOWER, BL_PLAN_WALL, BL_PLAN_GATE, BL_PLAN_SMITHY, BL_PLAN_MAGE, BL_PLAN_INFIRMARY, BL_PLAN_MINE, BL_PLAN_COUNT };

/* The keep: a castle on a square of 2 * BL_KEEP_R + 1 blocks a side - a wall with merlons, a turret at each corner, a tall tower in the middle and a doorway
 * towards -z. The block (x, y, z) is the middle of its lowest layer. */
#define BL_KEEP_R 3
#define BL_KEEP_TALL 10                 /* its highest block is at y + BL_KEEP_TALL - 1 */
#define BL_KEEP_CELLS 360

#define BL_SITE_CELLS 80                /* the biggest plan (the mage tower) has 48 */
#define BL_SITES_MAX 12                 /* buildings that may be under way at a time */

struct bl_cell {
	int16_t x, y, z;
	uint8_t block;
};

struct bl_site {
	bool on;
	int plan;                       /* enum bl_plan */
	int n;                          /* cells */
	struct bl_cell c[BL_SITE_CELLS];
};

const char *bl_plan_name(int plan);                          /* "barracks"; "?" for none */
void bl_plan_cost(int plan, int *cobble, int *planks);       /* what it takes (survival) */
int bl_plan_gold(int plan);                                  /* ... and the gold */
/* the cells of a plan by themselves, for a picture of it: round (0, 0, 0), not turned (the front looks towards -z), lowest first; how many */
int bl_plan_shape(int plan, struct bl_cell out[BL_SITE_CELLS]);
/* the cells of a keep of `block` (BL_KEEP or BL_REDKEEP) round (x, y, z), lowest first; how many (at most BL_KEEP_CELLS) */
int bl_keep_cells(struct bl_cell out[BL_KEEP_CELLS], int x, int y, int z, int block);

/* The smithy (3 x 3, 4 high), the mage tower (3 x 3, 7 high), the infirmary (3 x 3, 4 high) and the mine (3 x 3, 4 high) want flat, empty ground, too.
 *
 * Lay a plan out with the block (x, y, z) as its middle at ground level (the first empty cell above the ground there), turned by `rot` quarter turns: with 0 its
 * width runs along +x and its front (the barracks' door) looks towards -z; each turn goes the way the yaw goes. A barracks (3 x 3, 3 high) and a watchtower
 * (3 x 3, 6 high) want flat, empty ground; a wall (3 long, 2 high) follows the ground, up to two blocks up or down in each column; a gate (3 wide: two posts and
 * the gate between them) wants flat ground. `others` / `nothers`: the sites that are under way; a wall or a gate may share cells that want the same block (they
 * are left out here), nothing else may overlap. False with *why set when it cannot be built there; `out` still holds the cells (for the red preview). */
bool bl_plan_lay(const struct bl_world *w, int plan, int rot, int x, int y, int z, const struct bl_site *others, int nothers, struct bl_site *out, const char **why);

/* is the cell still to be filled? (air or water: anything else that stands there stays, the site does not wait for it) */
static inline bool bl_cell_missing(const struct bl_world *w, const struct bl_cell *c)
{
	int b = bl_get(w, c->x, c->y, c->z);
	return b == BL_AIR || bl_water(b);
}
int bl_site_missing(const struct bl_world *w, const struct bl_site *s);
void bl_site_box(const struct bl_site *s, int lo[3], int hi[3]);        /* the box round its cells (hi is the last cell, not past it) */

/* ---- the see-through picture ------------------------------------------------------------------------------------------ */

#define BL_GHOST_GROUPS (BL_SITES_MAX + 1)

struct bl_ghosts {
	int ngroups;
	struct bl_ghost_group {
		int first, n;
		float lo[3], hi[3];
	} group[BL_GHOST_GROUPS];
	int n;
	struct bl_ghost {
		int16_t x, y, z;
		uint32_t rgb;
	} g[BL_GHOST_GROUPS * BL_SITE_CELLS];
};

void bl_ghosts_clear(struct bl_ghosts *g);
/* the cells of a site as one group, in one colour; only the missing ones when `w` is given */
void bl_ghosts_add(struct bl_ghosts *g, const struct bl_world *w, const struct bl_site *s, uint32_t rgb);
/* the nearest ghost the ray (origin o, unit direction d) meets within tmax: its colour (0 .. 1), how solid it looks there (edges more than faces) and the distance */
bool bl_ghosts_ray(const struct bl_ghosts *g, const float o[3], const float d[3], float tmax, float rgb[3], float *a, float *t);

#endif
