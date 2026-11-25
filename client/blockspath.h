/* blockspath.h - finding a way in the block world, for the creatures that walk in it: an A* search over the places a body can stand in.
 *
 * A body is as the mobs are (blocksmobs.c): narrower than a block and `tall` cells high; it walks to the next cell, diagonally too when both cells beside the
 * corner are open, climbs a step of one block, lets itself down at most BL_WALK_DROP, and swims (slowly, so it goes round a pond when that is not far). What
 * stops it is the same as what stops the mob: a goblin (`foe`) is stopped by a gate, the player's own are not, and leaves stop neither.
 *
 * Plain code: no state is kept between calls, nothing in the world is changed. Not reentrant (the search's tables are shared): one thread at a time.
 */
#ifndef MELLAGUI_BLOCKSPATH_H
#define MELLAGUI_BLOCKSPATH_H

#include "blocks.h"

#define BL_PATH_MAX 32          /* the turns of a way that are kept (a longer way is looked for again when they are walked) */
#define BL_PATH_NODES 6000      /* the most places one search looks at */
#define BL_WALK_DROP 3          /* how many blocks a way goes down at a time */

struct bl_walk {
	bool foe;               /* a goblin: a gate stops it */
	int tall;               /* cells of room it needs (1 .. 4) */
};

/* a turn of a way: the cell the feet are to be in */
struct bl_way {
	int16_t x, y, z;
};

/* can the body stand in the cell (room for it and something under its feet), or float in it (water with no water over it) */
bool bl_walk_stands(const struct bl_world *w, const struct bl_walk *k, int x, int y, int z);
/* Can it simply walk straight from `from` (its feet) to `to`, climbing and letting itself down on the way, and arrive no more than 3 blocks above or below
 * `to[1]` (`any_height`: wherever it gets to)? Only the first BL_WALK_LINE blocks are looked at. */
#define BL_WALK_LINE 48.0f
bool bl_walk_line(const struct bl_world *w, const struct bl_walk *k, const float from[3], const float to[3], bool any_height);
/*
 * A way from `from` to within `radius` (along the ground; at least the cell itself) of `to`, at its height give or take 3 (`any_height`: at any). Looks at no
 * more than `budget` places (BL_PATH_NODES at most). The turns go into `out` (at most `max`; the first is the one to walk to first, the body's own cell is not
 * among them); returns how many, 0 when there is nowhere better to go than where it is. *full: the way ends at the goal (else it ends at the place nearest to
 * the goal that was found). *steps: the length of the whole way in cells, also when not all of it fits in `out`. *used: places looked at. Any of the three may
 * be NULL.
 */
int bl_walk_find(const struct bl_world *w, const struct bl_walk *k, const float from[3], const float to[3], bool any_height, float radius, int budget,
                 struct bl_way *out, int max, bool *full, int *steps, int *used);

#endif
