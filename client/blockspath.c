/* blockspath.c - ways through the block world. See blockspath.h. */
#include "blockspath.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HASH_N 16384            /* a power of two well over BL_PATH_NODES */
#define HEAP_N (BL_PATH_NODES * 8)

static bool stops(const struct bl_world *w, const struct bl_walk *k, int x, int y, int z)
{
	int b = bl_get(w, x, y, z);
	return k->foe ? bl_solid_foe(b) : bl_solid_unit(b);
}

/* room for the body from cell y up (`tall` cells) */
static bool clear_at(const struct bl_world *w, const struct bl_walk *k, int x, int y, int z)
{
	for (int i = 0; i < k->tall; i++)
		if (stops(w, k, x, y + i, z))
			return false;
	return true;
}

bool bl_walk_stands(const struct bl_world *w, const struct bl_walk *k, int x, int y, int z)
{
	if (x < 0 || z < 0 || x >= w->w || z >= w->d || y < 1 || y >= w->h)
		return false;
	if (!clear_at(w, k, x, y, z))
		return false;
	if (bl_water(bl_get(w, x, y, z)))
		return !bl_water(bl_get(w, x, y + 1, z));               /* it floats: it swims at the top of the water */
	return stops(w, k, x, y - 1, z);
}

/* Going from a cell at height y into the neighbouring column (nx, nz): the height it stands at there, or -1 when it cannot. Level first, else a step up (with
 * room over its own head to jump), else down as far as BL_WALK_DROP. */
static int step_to(const struct bl_world *w, const struct bl_walk *k, int x, int y, int z, int nx, int nz)
{
	if (bl_walk_stands(w, k, nx, y, nz))
		return y;
	if (stops(w, k, nx, y, nz)) {
		if (bl_walk_stands(w, k, nx, y + 1, nz) && !stops(w, k, x, y + k->tall, z))
			return y + 1;
		return -1;
	}
	if (!clear_at(w, k, nx, y, nz))
		return -1;
	for (int d = 1; d <= BL_WALK_DROP; d++) {
		if (stops(w, k, nx, y - d, nz))
			return -1;
		if (bl_walk_stands(w, k, nx, y - d, nz))
			return y - d;
	}
	return -1;
}

/* the cell the feet of a body at p are to be counted in: the one they are in, else (in the air: a jump, a fall) the first one under it that it can stand in */
static bool feet_cell(const struct bl_world *w, const struct bl_walk *k, const float p[3], int c[3])
{
	c[0] = (int)floorf(p[0]);
	c[2] = (int)floorf(p[2]);
	int y = (int)floorf(p[1] + 0.1f);
	while (bl_water(bl_get(w, c[0], y, c[2])) && bl_water(bl_get(w, c[0], y + 1, c[2])))   /* under water: it is on its way up */
		y++;
	for (int d = 0; d <= BL_WALK_DROP + 1; d++)
		if (bl_walk_stands(w, k, c[0], y - d, c[2])) {
			c[1] = y - d;
			return true;
		}
	/* over an edge: its middle is over nothing, but it stands on the cell beside */
	static const int side[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };
	for (int i = 0; i < 8; i++) {
		int x = c[0] + side[i][0], z = c[2] + side[i][1];
		if (hypotf(p[0] - ((float)x + 0.5f), p[2] - ((float)z + 0.5f)) < 1.0f && bl_walk_stands(w, k, x, y, z)) {
			c[0] = x;
			c[1] = y;
			c[2] = z;
			return true;
		}
	}
	c[1] = y;
	return false;
}

bool bl_walk_line(const struct bl_world *w, const struct bl_walk *k, const float from[3], const float to[3], bool any_height)
{
	int c[3];
	if (!feet_cell(w, k, from, c))
		return false;
	float dx = to[0] - from[0], dz = to[2] - from[2], len = hypotf(dx, dz);
	if (len < 0.01f)
		return any_height || fabsf((float)c[1] - to[1]) <= 3.0f;
	float see = fminf(len, BL_WALK_LINE), ux = dx / len, uz = dz / len;
	int x = c[0], y = c[1], z = c[2];
	for (float s = 0.2f; s < see + 0.2f; s += 0.2f) {
		float t = fminf(s, see), px = from[0] + ux * t, pz = from[2] + uz * t;
		/* the middle of the body and its two sides: each moves on cell by cell as the middle does */
		for (int side = -1; side <= 1; side += 2) {
			int sx = (int)floorf(px - uz * 0.3f * (float)side), sz = (int)floorf(pz + ux * 0.3f * (float)side);
			if ((sx != x || sz != z) && step_to(w, k, x, y, z, sx, sz) < 0)
				return false;
		}
		int nx = (int)floorf(px), nz = (int)floorf(pz);
		if (nx == x && nz == z)
			continue;
		if (nx != x && nz != z && (step_to(w, k, x, y, z, nx, z) < 0 || step_to(w, k, x, y, z, x, nz) < 0))
			return false;
		int ny = step_to(w, k, x, y, z, nx, nz);
		if (ny < 0)
			return false;
		x = nx;
		y = ny;
		z = nz;
	}
	return any_height || see < len || fabsf((float)y - to[1]) <= 3.0f;
}

/* ---- the search ---------------------------------------------------------------------------------------------------------- */

struct node {
	int16_t x, y, z;
	bool done;
	int parent;
	int g, h;
};

static struct node g_node[BL_PATH_NODES];
static int g_hash[HASH_N];              /* node + 1, 0 = free */
static struct { int f, n; } g_heap[HEAP_N];
static int g_nheap, g_nnode;

static uint32_t key_of(int x, int y, int z)
{
	uint32_t k = (uint32_t)x * 73856093u ^ (uint32_t)z * 19349663u ^ (uint32_t)y * 83492791u;
	return (k ^ (k >> 15)) & (HASH_N - 1);
}

static int node_at(int x, int y, int z, bool make)
{
	uint32_t i = key_of(x, y, z);
	for (;; i = (i + 1) & (HASH_N - 1)) {
		int n = g_hash[i] - 1;
		if (n < 0)
			break;
		if (g_node[n].x == x && g_node[n].y == y && g_node[n].z == z)
			return n;
	}
	if (!make || g_nnode >= BL_PATH_NODES)
		return -1;
	int n = g_nnode++;
	g_node[n] = (struct node){ (int16_t)x, (int16_t)y, (int16_t)z, false, -1, 1 << 30, 0 };
	g_hash[i] = n + 1;
	return n;
}

static void heap_put(int f, int n)
{
	if (g_nheap >= HEAP_N)
		return;
	int i = g_nheap++;
	while (i > 0 && g_heap[(i - 1) / 2].f > f) {
		g_heap[i] = g_heap[(i - 1) / 2];
		i = (i - 1) / 2;
	}
	g_heap[i].f = f;
	g_heap[i].n = n;
}

static int heap_take(void)
{
	if (g_nheap == 0)
		return -1;
	int top = g_heap[0].n, f = g_heap[--g_nheap].f, n = g_heap[g_nheap].n, i = 0;
	for (;;) {
		int c = 2 * i + 1;
		if (c >= g_nheap)
			break;
		if (c + 1 < g_nheap && g_heap[c + 1].f < g_heap[c].f)
			c++;
		if (g_heap[c].f >= f)
			break;
		g_heap[i] = g_heap[c];
		i = c;
	}
	g_heap[i].f = f;
	g_heap[i].n = n;
	return top;
}

/* the cost still ahead, at least: eight ways along the ground, 10 a cell and 14 a diagonal */
static int ahead(int x, int z, float gx, float gz)
{
	float dx = fabsf((float)x + 0.5f - gx), dz = fabsf((float)z + 0.5f - gz);
	return (int)(10.0f * fmaxf(dx, dz) + 4.0f * fminf(dx, dz));
}

int bl_walk_find(const struct bl_world *w, const struct bl_walk *k, const float from[3], const float to[3], bool any_height, float radius, int budget,
                 struct bl_way *out, int max, bool *full, int *steps, int *used)
{
	static const int dir[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };
	if (full)
		*full = false;
	if (steps)
		*steps = 0;
	if (used)
		*used = 0;
	int c[3];
	if (max <= 0 || !feet_cell(w, k, from, c) || !isfinite(to[0] + to[1] + to[2]))
		return 0;
	if (budget > BL_PATH_NODES)
		budget = BL_PATH_NODES;
	memset(g_hash, 0, sizeof g_hash);
	g_nheap = g_nnode = 0;
	int gx = (int)floorf(to[0]), gz = (int)floorf(to[2]);
	float rad = fmaxf(radius, 0.1f);
	int start = node_at(c[0], c[1], c[2], true);
	g_node[start].g = 0;
	g_node[start].h = ahead(c[0], c[2], to[0], to[2]);
	heap_put(g_node[start].h, start);
	int best = start, looked = 0, found = -1;
	int best_h = g_node[start].h + (any_height ? 0 : (int)(8.0f * fabsf((float)c[1] - to[1])));
	while (looked < budget) {
		int n = heap_take();
		if (n < 0)
			break;
		if (g_node[n].done)
			continue;
		g_node[n].done = true;
		looked++;
		int x = g_node[n].x, y = g_node[n].y, z = g_node[n].z;
		bool there = (x == gx && z == gz) || hypotf((float)x + 0.5f - to[0], (float)z + 0.5f - to[2]) <= rad;
		if (there && (any_height || fabsf((float)y - to[1]) <= 3.0f)) {
			found = n;
			break;
		}
		int h = g_node[n].h + (any_height ? 0 : (int)(8.0f * fabsf((float)y - to[1])));
		if (h < best_h) {
			best_h = h;
			best = n;
		}
		for (int d = 0; d < 8; d++) {
			int nx = x + dir[d][0], nz = z + dir[d][1], ny;
			if (d < 4) {
				ny = step_to(w, k, x, y, z, nx, nz);
				if (ny < 0)
					continue;
			} else {                                            /* a diagonal: on the level, past two open corners */
				if (!bl_walk_stands(w, k, nx, y, nz) || !clear_at(w, k, nx, y, z) || !clear_at(w, k, x, y, nz))
					continue;
				ny = y;
			}
			int m = node_at(nx, ny, nz, true);
			if (m < 0 || g_node[m].done)
				continue;
			int cost = (d < 4 ? 10 : 14) + (ny > y ? 6 : (y - ny) * 3) + (bl_water(bl_get(w, nx, ny, nz)) ? 20 : 0);
			if (g_node[n].g + cost >= g_node[m].g)
				continue;
			g_node[m].g = g_node[n].g + cost;
			g_node[m].h = ahead(nx, nz, to[0], to[2]);
			g_node[m].parent = n;
			heap_put(g_node[m].g + g_node[m].h, m);
		}
	}
	if (used)
		*used = looked;
	int end = found >= 0 ? found : best;
	if (full)
		*full = found >= 0;
	/* the cells of the way, from the end back; then only those where it turns or changes height */
	static int chain[BL_PATH_NODES];
	int len = 0;
	for (int n = end; n >= 0 && n != start && len < BL_PATH_NODES; n = g_node[n].parent)
		chain[len++] = n;
	if (steps)
		*steps = len;
	int count = 0, prev = start;
	for (int i = len - 1; i >= 0 && count < max; i--) {
		const struct node *a = &g_node[prev], *b = &g_node[chain[i]], *next = i > 0 ? &g_node[chain[i - 1]] : NULL;
		bool keep = !next || b->y != a->y || next->y != b->y || next->x - b->x != b->x - a->x || next->z - b->z != b->z - a->z;
		if (keep)
			out[count++] = (struct bl_way){ b->x, b->y, b->z };
		prev = chain[i];
	}
	return count;
}
