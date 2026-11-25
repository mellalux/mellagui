/* blocksbuild.h - what can be built in the block world: the plans of the buildings, a building site, and the see-through picture of what is planned.
 *
 * Plain code like blocks.[ch]: values in, values out. A PLAN is a barracks, a watchtower, a wall or a gate; laid out at a place of the world it becomes a SITE, a
 * list of cells with the block each wants. Workers (blocksmobs.[ch]) fill the cells one at a time; the game applies the blocks. The GHOSTS are the cells that are
 * planned but not there yet (and the preview under the pointer), drawn by blocksview.c as see-through boxes with brighter edges.
 */
#ifndef MELLAGUI_BLOCKSBUILD_H
#define MELLAGUI_BLOCKSBUILD_H

#include "blocks.h"

enum bl_plan { BL_PLAN_BARRACKS = 0, BL_PLAN_TOWER, BL_PLAN_WALL, BL_PLAN_GATE, BL_PLAN_COUNT };

#define BL_SITE_CELLS 80                /* the biggest plan (the barracks) has 60 */
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

/* Lay a plan out with the block (x, y, z) as its middle at ground level (the first empty cell above the ground there), turned by `rot` quarter turns: with 0 its
 * width runs along +x and its front (the barracks' door) looks towards -z; each turn goes the way the yaw goes. A barracks (5 x 4, 4 high) and a watchtower
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
