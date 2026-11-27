/* blocks_test.c - the block world of client/blocks.c and its picture (client/blocksview.c).
 *
 *   build/blocks-test                       the checks (a few seconds)
 *   build/blocks-test --bench WxH [N]       milliseconds a frame at that size on N threads (default 4); MELLAGUI_BLOCKS_PPM=FILE
 *                                           writes the first picture
 *   build/blocks-test --shot FILE.ppm WxH [key=value ...]    one picture to look at: seed size flat ex ey ez (the eye) yaw pitch
 *                                           (degrees) time dist fov (degrees) threads target=1
 *   build/blocks-test --info [seed]         what a world made from a seed contains
 *
 * Exit 0 = everything held.
 */
#include "blocks.h"
#include "blocksview.h"
#include "blockspets.h"
#include "blocksbuild.h"
#include "blockshero.h"
#include "blocksmobs.h"
#include "blockspath.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int g_fail, g_checks;
static char g_dir[256];
static void tmpdir(void);

#define CHECK(cond, ...) do { \
	g_checks++; \
	if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static uint64_t g_rng = 88172645463325252ULL;
static uint32_t rnd(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 7;
	g_rng ^= g_rng << 17;
	return (uint32_t)(g_rng >> 11);
}

static float rndf(void)       /* 0 .. 1 */
{
	return (float)(rnd() & 0xffffff) / 16777216.0f;
}

static float rndr(float lo, float hi)
{
	return lo + (hi - lo) * rndf();
}

/* ---- the world ------------------------------------------------------------------------------------------------------ */

/* the highest opaque / non-air block of a column, found the slow way */
static void brute_column(const struct bl_world *w, int x, int z, int *top, int *hi)
{
	*top = -1;
	*hi = -1;
	for (int y = w->h - 1; y >= 0; y--) {
		int b = w->b[bl_index(w, x, y, z)];
		if (b != BL_AIR && *hi < 0)
			*hi = y;
		if (bl_blocks_light(b)) {
			*top = y;
			break;
		}
	}
}

static void check_columns(const struct bl_world *w, const char *what)
{
	int bad = 0, maxhi = -1;
	for (int z = 0; z < w->d; z++)
		for (int x = 0; x < w->w; x++) {
			int top, hi;
			brute_column(w, x, z, &top, &hi);
			if (top != w->top[z * w->w + x] || hi != w->hi[z * w->w + x])
				bad++;
			if (hi > maxhi)
				maxhi = hi;
		}
	CHECK(bad == 0, "%s: %d columns have a wrong top / hi", what, bad);
	CHECK(maxhi == w->maxhi, "%s: maxhi %d, should be %d", what, w->maxhi, maxhi);
}

static void test_world(void)
{
	CHECK(bl_world_new(1, 8, 64, 64) == NULL, "a world 8 wide is refused");
	CHECK(bl_world_new(1, 64, 64, 8) == NULL, "a world 8 high is refused");
	CHECK(bl_world_new(1, 2000, 64, 64) == NULL, "a world 2000 wide is refused");
	CHECK(bl_world_flat(64, 64, 64, 1) == NULL && bl_world_flat(64, 64, 64, 60) == NULL, "a flat world with no floor / no room is refused");

	double t0 = now_s();
	struct bl_world *a = bl_world_new(1234, 256, 256, 64), *b = bl_world_new(1234, 256, 256, 64), *c = bl_world_new(1235, 256, 256, 64);
	double dt = (now_s() - t0) / 3.0;
	CHECK(a && b && c, "worlds are made");
	if (!a || !b || !c)
		return;
	printf("  a 256 x 256 x 64 world takes %.0f ms to make\n", dt * 1000.0);
	CHECK(bl_hash(a) == bl_hash(b), "the same seed makes the same world");
	CHECK(bl_hash(a) != bl_hash(c), "another seed makes another world");

	long count[BL_COUNT] = { 0 };
	size_t n = (size_t)a->w * a->d * a->h;
	int bad = 0;
	for (size_t i = 0; i < n; i++) {
		if (a->b[i] >= BL_COUNT)
			bad++;
		else
			count[a->b[i]]++;
	}
	CHECK(bad == 0, "%d cells hold a number that is no block", bad);
	for (int k = BL_GRASS; k < BL_COUNT; k++)
		if (k != BL_GLASS && k != BL_PLANKS && k != BL_BRICK && k != BL_TORCH && k != BL_LAMP && k != BL_KEEP && k != BL_BARRACKS && k != BL_GATE && k != BL_TOWERPOST && k != BL_REDKEEP && k != BL_GOLD && k != BL_SMITHY && k != BL_MAGE && k != BL_INFIRMARY && k != BL_MINE && k != BL_KEEPFLAG && k != BL_REDFLAG && !(k >= BL_FLOW1 && k <= BL_FLOW7))
			CHECK(count[k] > 0, "the world has no %s", bl_block_name(k));
	CHECK(count[BL_GLASS] == 0 && count[BL_PLANKS] == 0 && count[BL_TORCH] == 0 && count[BL_LAMP] == 0 && count[BL_KEEP] == 0 && count[BL_BARRACKS] == 0 && count[BL_GATE] == 0 && count[BL_TOWERPOST] == 0, "glass / planks / torches / lamps are only for the player to put, the keep for the game");
	long flowing = 0;
	for (int k = BL_FLOW1; k <= BL_FLOW7; k++)
		flowing += count[k];
	CHECK(flowing == 0, "the generated world has no flowing water, only the sea");
	CHECK((size_t)count[BL_AIR] > n / 4 && (size_t)count[BL_STONE] > n / 4, "a quarter of the world is air and a quarter stone");

	/* the floor: bedrock, everywhere; the rest of the bottom is stone or something carved out of it */
	int floor_bad = 0, level_bad = 0;
	for (int z = 0; z < a->d; z++)
		for (int x = 0; x < a->w; x++) {
			if (bl_get(a, x, 0, z) != BL_BEDROCK)
				floor_bad++;
			int hi = a->hi[z * a->w + x];
			if (hi < 3 || hi >= a->h - 3)
				level_bad++;
		}
	CHECK(floor_bad == 0, "%d columns have no bedrock at the bottom", floor_bad);
	CHECK(level_bad == 0, "%d columns are lower than 3 or touch the roof", level_bad);

	/* water only up to the sea level, and sea water is never above sand-free air: a water cell's neighbour above is water or air */
	int water_high = 0, water_hover = 0;
	for (int z = 0; z < a->d; z++)
		for (int x = 0; x < a->w; x++)
			for (int y = 1; y < a->h; y++) {
				if (a->b[bl_index(a, x, y, z)] != BL_WATER)
					continue;
				if (y > a->sea)
					water_high++;
				if (bl_get(a, x, y - 1, z) == BL_AIR)
					water_hover++;
			}
	CHECK(water_high == 0, "%d water cells above the sea level", water_high);
	CHECK(a->light && bl_light(a, 10, 40, 10) == 0, "no light in a world without lamps");
	CHECK(water_hover == 0, "%d water cells with air right under them", water_hover);

	/* a tree is a log with something solid under it and leaves near its top */
	int logs_bad = 0;
	for (int z = 0; z < a->d; z++)
		for (int x = 0; x < a->w; x++)
			for (int y = 1; y < a->h; y++)
				if (a->b[bl_index(a, x, y, z)] == BL_LOG) {
					int below = bl_get(a, x, y - 1, z);
					if (below != BL_LOG && below != BL_GRASS)
						logs_bad++;
				}
	CHECK(logs_bad == 0, "%d logs stand on nothing", logs_bad);

	/* tunnels: air in the rock that is not part of the surface */
	long tunnel = 0;
	for (int z = 0; z < a->d; z++)
		for (int x = 0; x < a->w; x++) {
			int top = a->top[z * a->w + x];
			for (int y = 2; y < top - 3; y++)
				if (a->b[bl_index(a, x, y, z)] == BL_AIR)
					tunnel++;
		}
	CHECK(tunnel > 2000, "only %ld air cells under the ground: no tunnels", tunnel);
	CHECK(tunnel < (long)(n / 6), "%ld cells of tunnel is too much of the ground", tunnel);

	check_columns(a, "generated world");
	float s[3];
	bl_spawn(a, s);
	CHECK(bl_get(a, (int)s[0], (int)s[1], (int)s[2]) == BL_AIR && bl_get(a, (int)s[0], (int)s[1] - 1, (int)s[2]) != BL_AIR,
	      "the spawn (%.1f %.1f %.1f) is not on the ground", s[0], s[1], s[2]);
	CHECK(bl_get(a, (int)s[0], (int)s[1] + 1, (int)s[2]) == BL_AIR, "no room for the player at the spawn");

	bl_world_free(a);
	bl_world_free(b);
	bl_world_free(c);

	struct bl_world *f = bl_world_flat(40, 40, 64, 20);
	CHECK(f != NULL, "a flat world is made");
	if (f) {
		CHECK(bl_get(f, 5, 0, 5) == BL_BEDROCK && bl_get(f, 5, 19, 5) == BL_GRASS && bl_get(f, 5, 20, 5) == BL_AIR, "the layers of a flat world");
		check_columns(f, "flat world");
		bl_world_free(f);
	}
}

/* ---- editing -------------------------------------------------------------------------------------------------------- */

static void check_light(struct bl_world *w, const char *what);

static void test_edit(void)
{
	struct bl_world *w = bl_world_new(77, 96, 96, 64);
	CHECK(w != NULL, "world");
	if (!w)
		return;
	uint32_t rev = w->rev;
	CHECK(!bl_set(w, -1, 10, 10, BL_STONE) && !bl_set(w, 10, 64, 10, BL_STONE) && !bl_set(w, 10, 10, 96, BL_STONE), "outside the world is refused");
	CHECK(!bl_set(w, 10, 0, 10, BL_AIR), "bedrock cannot be broken");
	CHECK(!bl_set(w, 10, 30, 10, BL_BEDROCK) && !bl_set(w, 10, 30, 10, BL_COUNT) && !bl_set(w, 10, 30, 10, -1), "bedrock cannot be made, nor a block that is none");
	CHECK(w->rev == rev, "a refused edit does not count");

	/* edits keep the columns right: 600 random ones, then the whole world is checked against a slow scan */
	for (int i = 0; i < 600; i++) {
		int x = (int)(rnd() % 96), z = (int)(rnd() % 96), y = (int)(rnd() % 64);
		int blk = (int)(rnd() % BL_COUNT);
		bl_set(w, x, y, z, blk);
	}
	check_columns(w, "after random edits");
	check_light(w, "after random edits");
	CHECK(w->rev > rev, "the edits are counted");

	/* a tower and then glass on top of it: glass is seen but does not shade */
	int x = 40, z = 40;
	for (int y = 1; y < 64; y++)
		bl_set(w, x, y, z, BL_AIR);
	int t0 = bl_top(w, x, z);
	CHECK(t0 < 1 || t0 >= 0, "top");
	bl_set(w, x, 30, z, BL_STONE);
	CHECK(bl_top(w, x, z) == 30 && w->hi[z * 96 + x] == 30, "a block raises the top");
	bl_set(w, x, 31, z, BL_GLASS);
	CHECK(bl_top(w, x, z) == 30 && w->hi[z * 96 + x] == 31, "glass raises hi and not top");
	bl_set(w, x, 30, z, BL_AIR);
	CHECK(bl_top(w, x, z) == -1 || bl_top(w, x, z) < 30, "breaking it lowers the top again");
	bl_set(w, x, 62, z, BL_PLANKS);
	CHECK(w->maxhi >= 62, "maxhi follows a block put high up");
	bl_set(w, x, 62, z, BL_AIR);
	check_columns(w, "after the tower");

	bl_world_free(w);

	/* pointing, breaking and placing on a flat world */
	w = bl_world_flat(48, 48, 64, 20);
	struct bl_player p;
	bl_player_init(&p, w);
	struct bl_input in = { 0 };
	for (int i = 0; i < 20; i++)
		bl_player_step(w, &p, &in, 0.02f);
	CHECK(p.on_ground && fabsf(p.y - 20.0f) < 0.01f, "the player stands on the grass (y %.3f)", p.y);
	float eye[3], look[3];
	p.pitch = -1.5f;
	bl_player_eye(&p, eye);
	bl_player_look(&p, look);
	struct bl_hit h;
	CHECK(bl_pick(w, eye, look, BL_REACH, &h), "looking down hits the ground");
	CHECK(h.face == BL_PY && h.y == 19 && h.py == 20 && h.x == (int)floorf(p.x) && h.z == (int)floorf(p.z), "the face is the top of the grass under the feet (%d %d %d, face %d)", h.x, h.y, h.z, h.face);
	CHECK(!bl_place(w, &h, BL_PLANKS, &p), "a block cannot be put where the player stands");
	struct bl_hit away = h;
	away.px += 3;
	CHECK(bl_place(w, &away, BL_PLANKS, &p) && bl_get(w, away.px, away.py, away.pz) == BL_PLANKS, "a block can be put beside the player");
	CHECK(!bl_place(w, &away, BL_STONE, &p), "not over a block that is there");
	CHECK(!bl_place(w, &away, BL_WATER, &p) && !bl_place(w, &away, BL_AIR, &p) && !bl_place(w, &away, BL_BEDROCK, &p), "water, air and bedrock are not placed");
	struct bl_hit g = away;
	g.x = away.px;
	g.y = away.py;
	g.z = away.pz;
	g.hit = true;
	CHECK(bl_break(w, &g) && bl_get(w, away.px, away.py, away.pz) == BL_AIR, "it can be broken again");
	struct bl_hit deep = { .hit = true, .x = 5, .y = 0, .z = 5 };
	CHECK(!bl_break(w, &deep), "bedrock stays");
	/* water is not aimed at, but a block can be put into it */
	bl_set(w, 30, 20, 30, BL_WATER);
	float o2[3] = { 30.5f, 24.5f, 30.5f }, d2[3] = { 0, -1, 0 };
	CHECK(bl_pick(w, o2, d2, 10.0f, &h) && h.y == 19, "a ray goes through water to the block under it");
	struct bl_hit into = { .hit = true, .x = 30, .y = 19, .z = 30, .face = BL_PY, .px = 30, .py = 20, .pz = 30 };
	CHECK(bl_place(w, &into, BL_STONE, NULL) && bl_get(w, 30, 20, 30) == BL_STONE, "a block replaces water");
	CHECK(!bl_pick(w, o2, (float[3]){ 0, 1, 0 }, 10.0f, &h), "looking up at the sky hits nothing");
	CHECK(!bl_pick(w, o2, (float[3]){ 0, 0, 0 }, 10.0f, &h), "no direction, no hit");
	bl_world_free(w);
}

/* the ray against a slow march: it must find exactly the first block that a fine walk along the ray finds */
static void test_pick(void)
{
	struct bl_world *w = bl_world_new(99, 128, 128, 64);
	if (!w) {
		CHECK(0, "world");
		return;
	}
	/* some glass and water to be looked through, more than the generator has */
	for (int i = 0; i < 400; i++)
		bl_set(w, (int)(rnd() % 128), 20 + (int)(rnd() % 20), (int)(rnd() % 128), (int[]){ BL_GLASS, BL_WATER, BL_TORCH, BL_LAMP, BL_FLOW3 }[rnd() % 5]);
	int hits = 0, misses = 0, grazes = 0, wrong_hit = 0, missed_block = 0, wrong_face = 0;
	for (int i = 0; i < 30000; i++) {
		float o[3] = { rndr(2, 126), rndr(5, 55), rndr(2, 126) };
		float d[3] = { rndr(-1, 1), rndr(-1, 1), rndr(-1, 1) };
		float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (l < 0.05f)
			continue;
		if (i % 7 == 0)
			d[1] = 0;
		if (i % 11 == 0)
			d[0] = 0;
		l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (l < 0.05f)
			continue;
		for (int k = 0; k < 3; k++)
			d[k] /= l;
		const float reach = 14.0f;
		struct bl_hit h;
		bool got = bl_pick(w, o, d, reach, &h);
		int sx = (int)floorf(o[0]), sy = (int)floorf(o[1]), sz = (int)floorf(o[2]);
		float tstop = got ? h.t : reach;
		int bad = 0;
		for (float t = 0.0f; t < tstop - 0.004f; t += 0.01f) {
			float q[3] = { o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t };
			int cx = (int)floorf(q[0]), cy = (int)floorf(q[1]), cz = (int)floorf(q[2]);
			if (!bl_inside(w, cx, cy, cz))
				break;
			if (cx == sx && cy == sy && cz == sz)
				continue;
			float fx = q[0] - (float)cx, fy = q[1] - (float)cy, fz = q[2] - (float)cz;
			if (fx < 0.003f || fx > 0.997f || fy < 0.003f || fy > 0.997f || fz < 0.003f || fz > 0.997f) {
				grazes++;
				continue;
			}
			if (bl_targetable(bl_get(w, cx, cy, cz))) {
				bad = 1;
				break;
			}
		}
		if (bad)
			missed_block++;
		if (got) {
			hits++;
			float q[3] = { o[0] + d[0] * h.t, o[1] + d[1] * h.t, o[2] + d[2] * h.t };
			int axis = h.face >> 1;
			float plane = (float)(axis == 0 ? h.x : axis == 1 ? h.y : h.z) + (bl_face_n[h.face][axis] > 0 ? 1.0f : 0.0f);
			if (fabsf(q[axis] - plane) > 0.002f)
				wrong_face++;
			for (int k = 0; k < 3; k++) {
				float lo = (float)(k == 0 ? h.x : k == 1 ? h.y : h.z);
				if (q[k] < lo - 0.002f || q[k] > lo + 1.002f)
					wrong_hit++;
			}
			if (!bl_targetable(bl_get(w, h.x, h.y, h.z)) || h.px != h.x + bl_face_n[h.face][0] || h.py != h.y + bl_face_n[h.face][1] ||
			    h.pz != h.z + bl_face_n[h.face][2])
				wrong_hit++;
		} else {
			misses++;
		}
	}
	CHECK(missed_block == 0, "%d rays went through a block that a fine walk finds", missed_block);
	CHECK(wrong_hit == 0, "%d hits are not on the block they name", wrong_hit);
	CHECK(wrong_face == 0, "%d hits are not on the face they name", wrong_face);
	CHECK(hits > 5000 && misses > 5000, "the rays should hit and miss often (hits %d, misses %d)", hits, misses);
	printf("  %d rays: %d hits, %d misses, %d sample points too near an edge to judge\n", hits + misses, hits, misses, grazes);
	bl_world_free(w);
}

/* ---- the player ----------------------------------------------------------------------------------------------------- */

static bool player_in_solid(const struct bl_world *w, const struct bl_player *p)
{
	for (int y = (int)floorf(p->y) - 1; y <= (int)floorf(p->y + BL_PLAYER_H) + 1; y++)
		for (int z = (int)floorf(p->z) - 2; z <= (int)floorf(p->z) + 2; z++)
			for (int x = (int)floorf(p->x) - 2; x <= (int)floorf(p->x) + 2; x++)
				if (bl_solid(bl_get(w, x, y, z)) && bl_player_overlaps(p, x, y, z))
					return true;
	return false;
}

static void settle(const struct bl_world *w, struct bl_player *p)
{
	struct bl_input in = { 0 };
	for (int i = 0; i < 200; i++)
		bl_player_step(w, p, &in, 0.02f);
}


/* ---- light and water that flows ---------------------------------------------------------------------------------------- */

/* the light the incremental updates made equals the light made from the emitters from scratch */
static void check_light(struct bl_world *w, const char *what)
{
	size_t n = (size_t)w->w * w->d * w->h;
	uint8_t *keep = malloc(n);
	memcpy(keep, w->light, n);
	bl_lights_rebuild(w);
	size_t bad = 0, first = n;
	for (size_t i = 0; i < n; i++)
		if (keep[i] != w->light[i]) {
			bad++;
			if (first == n)
				first = i;
		}
	CHECK(bad == 0, "%s: %zu cells have light that differs from the light made from scratch (first at index %zu: %d, should be %d)", what, bad, first,
	      first < n ? keep[first] : 0, first < n ? w->light[first] : 0);
	free(keep);
}

static void test_light(void)
{
	struct bl_world *w = bl_world_flat(48, 48, 64, 20);
	CHECK(bl_light(w, 10, 25, 10) == 0 && bl_light(w, -1, 5, 5) == 0 && bl_light(w, 5, 99, 5) == 0, "no light, and none outside the world");
	/* a torch lights 14 in its cell and one less for every step, in the open as far as the Manhattan distance goes */
	struct bl_hit floor = { .hit = true, .x = 24, .y = 19, .z = 24, .face = BL_PY, .px = 24, .py = 20, .pz = 24 };
	CHECK(bl_place(w, &floor, BL_TORCH, NULL), "a torch on the grass");
	CHECK(bl_light(w, 24, 20, 24) == 14 && bl_light(w, 25, 20, 24) == 13 && bl_light(w, 24, 22, 24) == 12 && bl_light(w, 24 + 7, 20, 24 + 7) == 0,
	      "14 at the torch, 13 beside it, 12 two up, nothing 14 steps away (%d %d %d %d)", bl_light(w, 24, 20, 24), bl_light(w, 25, 20, 24), bl_light(w, 24, 22, 24),
	      bl_light(w, 31, 20, 31));
	CHECK(bl_light(w, 24 + 13, 20, 24) == 1 && bl_light(w, 24 + 14, 20, 24) == 0, "the light reaches 13 cells and no further");
	CHECK(bl_light(w, 24, 19, 24) == 0, "no light in the ground (a block that is opaque holds none)");
	check_light(w, "one torch");
	/* a lamp is 15 and an opaque block that shines */
	CHECK(bl_set(w, 10, 20, 10, BL_LAMP) && bl_light(w, 10, 20, 10) == 15 && bl_light(w, 11, 20, 10) == 14, "a lamp gives 15 and 14 beside it");
	/* a wall of stone holds light back and taking it away lets it through again */
	for (int y = 20; y < 30; y++)
		for (int z = 20; z < 29; z++)
			bl_set(w, 27, y, z, BL_STONE);
	int behind = bl_light(w, 29, 20, 24);
	CHECK(behind < 10, "behind a wall the light is weaker (%d)", behind);
	check_light(w, "with a wall");
	for (int y = 20; y < 30; y++)
		for (int z = 20; z < 29; z++)
			bl_set(w, 27, y, z, BL_AIR);
	CHECK(bl_light(w, 29, 20, 24) == 14 - 5, "without it the light is as before (%d)", bl_light(w, 29, 20, 24));
	check_light(w, "wall taken away");
	/* a torch taken away takes its light with it, the lamp's stays */
	struct bl_hit tor = { .hit = true, .x = 24, .y = 20, .z = 24 };
	CHECK(bl_break(w, &tor) && bl_light(w, 24, 20, 24) == 0 && bl_light(w, 25, 20, 24) == 0, "no torch, no light");
	CHECK(bl_light(w, 11, 20, 10) == 14, "the lamp still shines");
	check_light(w, "torch taken away");
	/* glass and water let light through, a room closed with stone does not */
	bl_set(w, 30, 20, 30, BL_TORCH);
	for (int x = 29; x <= 31; x++)
		for (int z = 29; z <= 31; z++)
			for (int y = 20; y <= 22; y++)
				if (!(x == 30 && z == 30 && y == 20))
					bl_set(w, x, y, z, y == 22 || x != 30 || z != 30 ? BL_STONE : BL_AIR);
	CHECK(bl_light(w, 30, 21, 30) == 13 && bl_light(w, 33, 20, 30) == 0, "a closed room: lit inside, dark outside (%d, %d)", bl_light(w, 30, 21, 30), bl_light(w, 33, 20, 30));
	bl_set(w, 31, 20, 30, BL_GLASS);
	CHECK(bl_light(w, 32, 20, 30) == 13 - 0 - 1 + 0 && bl_light(w, 33, 20, 30) == 11, "a pane of glass lets it out (%d %d)", bl_light(w, 32, 20, 30), bl_light(w, 33, 20, 30));
	check_light(w, "room with glass");
	/* a torch needs a floor and goes when it is taken away */
	struct bl_hit air = { .hit = true, .x = 5, .y = 30, .z = 5, .face = BL_PY, .px = 5, .py = 31, .pz = 5 };
	CHECK(!bl_place(w, &air, BL_TORCH, NULL), "a torch is not put in the air");
	bl_set(w, 40, 20, 40, BL_STONE);
	struct bl_hit on = { .hit = true, .x = 40, .y = 20, .z = 40, .face = BL_PY, .px = 40, .py = 21, .pz = 40 };
	CHECK(bl_place(w, &on, BL_TORCH, NULL) && bl_get(w, 40, 21, 40) == BL_TORCH && bl_light(w, 40, 21, 40) == 14, "a torch on a stone");
	bl_set(w, 40, 20, 40, BL_AIR);
	CHECK(bl_get(w, 40, 21, 40) == BL_AIR && bl_light(w, 40, 21, 40) == 0, "the torch goes with the block it stood on");
	check_light(w, "torch without floor");
	/* a torch can be walked through, aimed at and broken */
	CHECK(!bl_solid(BL_TORCH) && bl_targetable(BL_TORCH) && !bl_opaque(BL_TORCH) && bl_opaque(BL_LAMP) && bl_solid(BL_LAMP), "what a torch and a lamp are");
	/* water put into a torch's cell takes it away, water that flows does not */
	bl_world_free(w);

	/* 600 random edits with lamps, torches, glass, water and stone: the light is always what the emitters make */
	w = bl_world_new(52, 64, 64, 64);
	int kinds[] = { BL_TORCH, BL_LAMP, BL_STONE, BL_AIR, BL_AIR, BL_GLASS, BL_WATER, BL_DIRT, BL_LEAVES, BL_FLOW2 };
	for (int round = 0; round < 6; round++) {
		for (int i = 0; i < 120; i++)
			bl_set(w, 20 + (int)(rnd() % 24), 18 + (int)(rnd() % 20), 20 + (int)(rnd() % 24), kinds[rnd() % (sizeof kinds / sizeof kinds[0])]);
		check_light(w, "random edits");
		check_columns(w, "random edits with light");
	}
	bl_world_free(w);
}

static int ref_desired(const struct bl_world *w, int x, int y, int z)       /* the rule of the flow, written again from the description in blocks.h */
{
	if (bl_water(bl_get(w, x, y + 1, z)))
		return 1;
	int best = 8;
	static const int dx[4] = { 1, -1, 0, 0 }, dz[4] = { 0, 0, 1, -1 };
	for (int k = 0; k < 4; k++) {
		int nb = bl_get(w, x + dx[k], y, z + dz[k]);
		if (!bl_water(nb))
			continue;
		int ln = bl_water_level(nb);
		int below = bl_get(w, x + dx[k], y - 1, z + dz[k]);
		if (ln >= 7 || (ln > 0 && (below == BL_AIR || bl_water(below))))
			continue;
		if (ln + 1 < best)
			best = ln + 1;
	}
	return best <= 7 ? best : 0;
}

/* every cell of air or flowing water holds what the rule says it should: the flow has settled */
static long water_unsettled(const struct bl_world *w)
{
	long bad = 0;
	for (int z = 0; z < w->d; z++)
		for (int x = 0; x < w->w; x++)
			for (int y = 0; y < w->h; y++) {
				int b = w->b[bl_index(w, x, y, z)];
				if (b != BL_AIR && !(b >= BL_FLOW1 && b <= BL_FLOW7))
					continue;
				int d = ref_desired(w, x, y, z);
				if (b != (d ? BL_FLOW1 + d - 1 : BL_AIR))
					bad++;
			}
	return bad;
}

static int settle_water(struct bl_world *w)
{
	int rounds = 0;
	while (bl_water_pending(w) && rounds < 3000) {
		bl_world_tick(w, 1 << 20);
		rounds++;
	}
	return rounds;
}

static long count_water(const struct bl_world *w, long *sources)
{
	long n = 0;
	*sources = 0;
	size_t cells = (size_t)w->w * w->d * w->h;
	for (size_t i = 0; i < cells; i++)
		if (bl_water(w->b[i])) {
			n++;
			*sources += w->b[i] == BL_WATER;
		}
	return n;
}

static void test_water(void)
{
	struct bl_world *w = bl_world_flat(64, 64, 64, 20);
	long src;
	CHECK(bl_world_tick(w, 100) == 0, "a tick with nothing to do changes nothing");
	/* a source on the floor spreads in a diamond: level = how many steps from it, 7 at the most */
	bl_set(w, 32, 20, 32, BL_WATER);
	int rounds = settle_water(w);
	CHECK(rounds > 4 && rounds < 40, "the flow takes %d ticks to settle", rounds);
	CHECK(bl_get(w, 33, 20, 32) == BL_FLOW1 && bl_get(w, 34, 20, 32) == BL_FLOW2 && bl_get(w, 33, 20, 33) == BL_FLOW2 && bl_get(w, 32 + 7, 20, 32) == BL_FLOW7 &&
	      bl_get(w, 32 + 8, 20, 32) == BL_AIR, "levels 1 .. 7 going out from the source");
	CHECK(count_water(w, &src) == 1 + 4 * 28 && src == 1, "a diamond of 113 cells, one of them a source (%ld)", count_water(w, &src));
	CHECK(water_unsettled(w) == 0, "and it is settled: every cell holds what the rule asks");
	/* the source taken away: the flow dries up behind it */
	bl_set(w, 32, 20, 32, BL_AIR);
	settle_water(w);
	CHECK(count_water(w, &src) == 0, "it dries up (%ld cells left)", count_water(w, &src));
	CHECK(water_unsettled(w) == 0, "settled");

	/* water in the air falls and spreads where it lands */
	bl_set(w, 20, 26, 20, BL_WATER);
	settle_water(w);
	CHECK(bl_get(w, 20, 21, 20) == BL_FLOW1 && bl_get(w, 20, 20, 20) == BL_FLOW1, "a column of falling water down to the floor");
	/* the four neighbours of the source at its own height have air under them: they fall too, and the pool is spread from their four columns, level 1 at the foot of each */
	CHECK(bl_get(w, 21, 26, 20) == BL_FLOW1 && bl_get(w, 22, 26, 20) == BL_AIR && bl_get(w, 21, 20, 20) == BL_FLOW1 && bl_get(w, 22, 20, 20) == BL_FLOW2 &&
	      bl_get(w, 27, 20, 20) == BL_FLOW7 && bl_get(w, 28, 20, 20) == BL_AIR, "a flowing cell with air under it falls and does not spread; a pool where the columns land");
	CHECK(water_unsettled(w) == 0, "settled, with the waterfalls round the source in the air");
	bl_set(w, 20, 26, 20, BL_AIR);
	settle_water(w);
	CHECK(count_water(w, &src) == 0 && water_unsettled(w) == 0, "all of it dries up when the source goes (%ld)", count_water(w, &src));

	/* a wall holds it, a torch is not washed away, a hole in the floor takes it down */
	for (int y = 20; y < 24; y++)
		for (int x = 10; x < 20; x++)
			bl_set(w, x, y, 40, BL_STONE);
	bl_set(w, 15, 20, 41, BL_WATER);
	bl_set(w, 16, 20, 41, BL_TORCH);
	settle_water(w);
	CHECK(bl_get(w, 15, 20, 40) == BL_STONE && bl_get(w, 15, 20, 39) == BL_AIR, "the wall stops it");
	CHECK(bl_get(w, 16, 20, 41) == BL_TORCH && bl_water(bl_get(w, 17, 20, 41)), "a torch is not washed away; the water goes round it by the next row");
	bl_set(w, 14, 19, 41, BL_AIR);
	bl_set(w, 14, 18, 41, BL_AIR);
	settle_water(w);
	CHECK(bl_water(bl_get(w, 14, 19, 41)) && bl_water(bl_get(w, 14, 18, 41)), "a hole in the floor takes the water down");
	CHECK(water_unsettled(w) == 0, "settled with a hole and a wall and a torch");
	bl_world_free(w);

	/* what the player may do with it */
	w = bl_world_flat(32, 32, 64, 20);
	struct bl_hit top = { .hit = true, .x = 10, .y = 19, .z = 10, .face = BL_PY, .px = 10, .py = 20, .pz = 10 };
	CHECK(bl_place(w, &top, BL_WATER, NULL) && bl_get(w, 10, 20, 10) == BL_WATER, "water is put by hand as a source");
	settle_water(w);
	CHECK(bl_get(w, 11, 20, 10) == BL_FLOW1, "and it flows");
	struct bl_hit side = { .hit = true, .x = 10, .y = 19, .z = 10, .face = BL_PY, .px = 11, .py = 20, .pz = 10 };
	CHECK(bl_place(w, &side, BL_STONE, NULL) && bl_get(w, 11, 20, 10) == BL_STONE, "a block replaces flowing water");
	settle_water(w);
	CHECK(water_unsettled(w) == 0, "settled again");
	CHECK(!bl_place(w, &top, BL_FLOW3, NULL), "flowing water is not something to put");
	CHECK(bl_water(BL_FLOW4) && !bl_solid(BL_FLOW4) && bl_water_level(BL_FLOW4) == 4 && bl_water_level(BL_WATER) == 0 && !bl_targetable(BL_FLOW7), "what flowing water is");
	/* the budget: no more cells looked at than asked */
	bl_set(w, 20, 20, 20, BL_WATER);
	long before = bl_water_pending(w);
	int changed = bl_world_tick(w, 1);
	CHECK(changed <= 1 && bl_water_pending(w) >= (size_t)before - 1, "a tick of budget 1 changes at most one cell (%d changed, %ld waiting before)", changed, before);
	settle_water(w);
	CHECK(water_unsettled(w) == 0, "the rest of the queue is looked at later");
	/* the player in flowing water */
	struct bl_player p;
	bl_player_init(&p, w);
	p.x = 22.5f;
	p.z = 20.5f;
	p.y = 20.0f;
	struct bl_input none = { 0 };
	bl_player_step(w, &p, &none, 0.05f);
	CHECK(p.in_water || bl_water(bl_get(w, 22, 20, 20)), "standing in flowing water counts as water");
	bl_world_free(w);

	/* water flows into a hole dug at the shore of the sea, and the result is settled */
	w = bl_world_new(31, 96, 96, 64);
	int sx = -1, hx = -1, hz = -1;
	static const int dx[4] = { 1, -1, 0, 0 }, dz[4] = { 0, 0, 1, -1 };
	for (int z = 2; z < 94 && sx < 0; z++)
		for (int x = 2; x < 94 && sx < 0; x++)
			if (w->b[bl_index(w, x, w->sea, z)] == BL_WATER)
				for (int k = 0; k < 4; k++) {
					int nx = x + dx[k], nz = z + dz[k];
					int b = w->b[bl_index(w, nx, w->sea, nz)];
					if (bl_solid(b) && b != BL_BEDROCK && bl_get(w, nx, w->sea + 1, nz) == BL_AIR) {
						sx = x;
						hx = nx;
						hz = nz;
						break;
					}
				}
	CHECK(sx >= 0, "the sea has a shore");
	if (sx >= 0) {
		CHECK(water_unsettled(w) == 0, "the generated sea is settled as it is");
		bl_set(w, hx, w->sea, hz, BL_AIR);
		settle_water(w);
		CHECK(bl_water(bl_get(w, hx, w->sea, hz)), "the sea runs into the hole dug at its shore");
		CHECK(water_unsettled(w) == 0, "and the whole world is settled again (%ld cells are not)", water_unsettled(w));
		bl_set(w, hx, w->sea, hz, BL_STONE);
		settle_water(w);
		CHECK(water_unsettled(w) == 0, "closing the hole dries what came through");
	}
	bl_world_free(w);

	/* a flood and a saved file: flowing water is saved and looked at again when it is loaded (what has lost its source dries up) */
	tmpdir();
	char path[400];
	snprintf(path, sizeof path, "%s/flow.bl", g_dir);
	w = bl_world_flat(48, 48, 64, 20);
	bl_set(w, 24, 20, 24, BL_WATER);
	settle_water(w);
	uint64_t hash = bl_hash(w);
	CHECK(bl_save(w, path) == 0, "saved a world with a flood");
	const char *err = NULL;
	struct bl_world *r = bl_load(path, &err);
	CHECK(r && bl_hash(r) == hash && count_water(r, &src) == 113, "the flood comes back");
	if (r) {
		settle_water(r);
		CHECK(bl_hash(r) == hash && water_unsettled(r) == 0, "and stays as it was when looked at again");
		bl_world_free(r);
	}
	bl_set(w, 24, 20, 24, BL_AIR);               /* the source goes and the file is written before the water has had time to dry up */
	CHECK(w->wq_n > 0, "the water waits to dry");
	bl_save(w, path);
	r = bl_load(path, &err);
	CHECK(r && count_water(r, &src) == 112, "the stale flow is in the file (%ld)", r ? count_water(r, &src) : -1L);
	if (r) {
		settle_water(r);
		CHECK(count_water(r, &src) == 0, "and dries up after the load (%ld)", count_water(r, &src));
		bl_world_free(r);
	}
	bl_world_free(w);
}

static void test_player(void)
{
	struct bl_world *w = bl_world_flat(64, 64, 64, 20);
	struct bl_player p;
	bl_player_init(&p, w);
	settle(w, &p);
	CHECK(p.on_ground && fabsf(p.y - 20.0f) < 0.01f, "standing: on_ground %d y %.4f", p.on_ground, p.y);

	/* the height of a jump: v^2 / 2g = 1.27 */
	struct bl_input jump = { .jump = true }, none = { 0 };
	float top = p.y;
	bl_player_step(w, &p, &jump, 1.0f / 60.0f);
	for (int i = 0; i < 120; i++) {
		bl_player_step(w, &p, &none, 1.0f / 60.0f);
		if (p.y > top)
			top = p.y;
	}
	CHECK(top - 20.0f > 1.22f && top - 20.0f < 1.31f, "a jump is %.3f blocks high", top - 20.0f);
	CHECK(p.on_ground && fabsf(p.y - 20.0f) < 0.01f, "and it comes down again (y %.3f)", p.y);
	/* the same jump at a different frame rate */
	float top2 = p.y;
	bl_player_step(w, &p, &jump, 0.05f);
	for (int i = 0; i < 40; i++) {
		bl_player_step(w, &p, &none, 0.05f);
		if (p.y > top2)
			top2 = p.y;
	}
	CHECK(fabsf((top2 - 20.0f) - (top - 20.0f)) < 0.12f, "a jump at 20 Hz (%.3f) is about the one at 60 Hz (%.3f)", top2 - 20.0f, top - 20.0f);

	/* walking, sprinting, sneaking: the distance in three seconds */
	struct { struct bl_input in; float lo, hi; const char *name; } walks[] = {
		{ { .fwd = 1 }, 12.0f, 13.2f, "walk" },
		{ { .fwd = 1, .sprint = true }, 15.5f, 17.0f, "sprint" },
		{ { .fwd = 1, .sneak = true }, 3.5f, 4.1f, "sneak" },
	};
	for (size_t k = 0; k < sizeof walks / sizeof walks[0]; k++) {
		bl_player_init(&p, w);
		p.x = 32.5f;
		p.z = 20.5f;
		p.yaw = 0;
		settle(w, &p);
		float z0 = p.z;
		for (int i = 0; i < 180; i++)
			bl_player_step(w, &p, &walks[k].in, 1.0f / 60.0f);
		CHECK(p.z - z0 > walks[k].lo && p.z - z0 < walks[k].hi, "%s: %.2f blocks in 3 s", walks[k].name, p.z - z0);
	}
	/* walking sideways and diagonally is not faster than straight */
	bl_player_init(&p, w);
	p.x = 20.5f;
	p.z = 20.5f;
	settle(w, &p);
	struct bl_input diag = { .fwd = 1, .strafe = 1 };
	float x0 = p.x, z0 = p.z;
	for (int i = 0; i < 180; i++)
		bl_player_step(w, &p, &diag, 1.0f / 60.0f);
	float run = sqrtf((p.x - x0) * (p.x - x0) + (p.z - z0) * (p.z - z0));
	CHECK(run < 13.2f && run > 12.0f, "a diagonal run is %.2f blocks, as long as a straight one", run);
	CHECK(p.x > x0 + 5 && p.z > z0 + 5, "forward + right at yaw 0 goes to +x and +z");

	/* a wall of stone: pushing into it for five seconds does not get through */
	for (int x = 10; x < 50; x++)
		for (int y = 20; y < 26; y++)
			bl_set(w, x, y, 40, BL_STONE);
	bl_player_init(&p, w);
	p.x = 30.5f;
	p.z = 32.5f;
	settle(w, &p);
	struct bl_input fwd = { .fwd = 1, .sprint = true };
	for (int i = 0; i < 300; i++)
		bl_player_step(w, &p, &fwd, 1.0f / 60.0f);
	CHECK(p.z + 0.3f <= 40.0f + 0.001f && p.z > 39.5f - 0.35f, "stopped by the wall at z = %.4f", p.z);
	/* the same on a diagonal: slides along the wall */
	p.yaw = 0.7f;
	for (int i = 0; i < 300; i++)
		bl_player_step(w, &p, &fwd, 1.0f / 60.0f);
	CHECK(p.z + 0.3f <= 40.0f + 0.001f && p.x > 33.0f, "slides along the wall (x %.2f z %.4f)", p.x, p.z);
	/* and from the other side, and in x */
	for (int z = 10; z < 50; z++)
		for (int y = 20; y < 26; y++)
			bl_set(w, 15, y, z, BL_STONE);
	p.x = 25.5f;
	p.z = 20.5f;
	p.yaw = -1.5707964f;
	settle(w, &p);
	for (int i = 0; i < 300; i++)
		bl_player_step(w, &p, &fwd, 1.0f / 60.0f);
	CHECK(p.x - 0.3f >= 16.0f - 0.001f && p.x < 16.8f, "stopped by the wall in -x at x = %.4f", p.x);

	/* a low ceiling stops a jump */
	for (int z = 44; z < 60; z++)
		for (int x = 44; x < 60; x++)
			bl_set(w, x, 22, z, BL_STONE);
	bl_player_init(&p, w);
	p.x = 50.5f;
	p.z = 50.5f;
	settle(w, &p);
	float peak = 0;
	bl_player_step(w, &p, &jump, 1.0f / 60.0f);
	for (int i = 0; i < 100; i++) {
		bl_player_step(w, &p, &none, 1.0f / 60.0f);
		if (p.y > peak)
			peak = p.y;
	}
	CHECK(peak + BL_PLAYER_H <= 22.0f + 0.001f, "the head stops at the ceiling (head at %.4f)", peak + BL_PLAYER_H);
	CHECK(p.on_ground, "and the player is back on the ground");

	/* a thin floor high up: a fast fall does not go through it, at any frame rate */
	for (int z = 20; z < 44; z++)
		for (int x = 20; x < 44; x++)
			bl_set(w, x, 40, z, BL_PLANKS);
	for (int pass = 0; pass < 4; pass++) {
		float dts[4] = { 0.01f, 0.05f, 0.1f, 0.5f };
		bl_player_init(&p, w);
		p.x = 32.5f;
		p.z = 32.5f;
		p.y = 60.0f;
		p.vy = -75.0f;
		float lowest = p.y;
		for (int i = 0; i < 400; i++) {
			bl_player_step(w, &p, &none, dts[pass]);
			if (p.y < lowest)
				lowest = p.y;
		}
		CHECK(lowest > 40.99f && p.on_ground, "a fall at dt %.2f lands on the planks (lowest %.3f)", dts[pass], lowest);
	}

	/* sneaking stops at an edge, walking falls off it */
	for (int z = 28; z < 32; z++)
		for (int x = 28; x < 32; x++)
			for (int y = 20; y < 22; y++)
				bl_set(w, x, y, z, BL_STONE);
	for (int sneak = 1; sneak >= 0; sneak--) {
		bl_player_init(&p, w);
		p.x = 29.5f;
		p.z = 29.5f;
		p.y = 22.0f;
		p.yaw = 1.5707964f;        /* looking along +x */
		struct bl_input go = { .fwd = 1, .sneak = sneak != 0 };
		for (int i = 0; i < 240; i++)
			bl_player_step(w, &p, &go, 1.0f / 60.0f);
		if (sneak)
			CHECK(p.x < 32.35f && p.y > 21.9f, "a sneaking player stops at the edge (x %.3f y %.3f)", p.x, p.y);
		else
			CHECK(p.x > 33.0f && p.y < 20.1f, "a walking player falls off (x %.3f y %.3f)", p.x, p.y);
	}

	/* water: it holds you up, you sink slowly, space swims up and out */
	for (int z = 44; z < 52; z++)
		for (int x = 4; x < 12; x++) {
			for (int y = 16; y < 20; y++)
				bl_set(w, x, y, z, BL_WATER);
		}
	bl_player_init(&p, w);
	p.x = 7.5f;
	p.z = 47.5f;
	p.y = 26.0f;
	bool saw_water = false;
	float vmin = 0;
	for (int i = 0; i < 400; i++) {
		bl_player_step(w, &p, &none, 0.02f);
		saw_water |= p.in_water;
		if (p.in_water && p.vy < vmin)
			vmin = p.vy;
	}
	CHECK(saw_water && p.y > 16.0f - 0.01f && p.y < 17.01f, "the player sinks to the bottom of the pool (y %.3f)", p.y);
	CHECK(vmin > -3.0f, "and is slowed down by the water (fastest fall in it %.1f)", vmin);
	CHECK(!player_in_solid(w, &p), "not inside the floor");
	for (int i = 0; i < 300; i++)
		bl_player_step(w, &p, &jump, 0.02f);
	CHECK(p.y > 19.0f, "space swims up to the surface (y %.3f)", p.y);
	CHECK(!player_in_solid(w, &p), "not inside anything");

	/* flying: no fall, up and down on keys */
	bl_player_init(&p, w);
	p.x = 40.5f;
	p.z = 10.5f;
	settle(w, &p);
	p.flying = true;
	for (int i = 0; i < 60; i++)
		bl_player_step(w, &p, &jump, 1.0f / 60.0f);
	CHECK(p.y > 25.0f, "flying up for a second: y %.2f", p.y);
	for (int i = 0; i < 90; i++)
		bl_player_step(w, &p, &none, 1.0f / 60.0f);       /* the speed dies away */
	float hold = p.y;
	for (int i = 0; i < 60; i++)
		bl_player_step(w, &p, &none, 1.0f / 60.0f);
	CHECK(fabsf(p.y - hold) < 0.05f, "flying does not fall (%.2f -> %.2f)", hold, p.y);
	bl_world_free(w);

	/* a long random walk on real terrain: never inside a block, whatever the frame rate and the keys */
	w = bl_world_new(31337, 128, 128, 64);
	bl_player_init(&p, w);
	struct bl_input in = { 0 };
	int inside = 0, nans = 0;
	for (int i = 0; i < 40000; i++) {
		if (i % 40 == 0) {
			in.fwd = (float)((int)(rnd() % 3) - 1);
			in.strafe = (float)((int)(rnd() % 3) - 1);
			in.jump = rnd() % 3 == 0;
			in.sneak = rnd() % 8 == 0;
			in.sprint = rnd() % 3 == 0;
			p.yaw += rndr(-1.0f, 1.0f);
		}
		if (i % 4000 == 3999) {                      /* now and then: somewhere else, in the air, moving fast */
			p.x = rndr(5, 120);
			p.z = rndr(5, 120);
			p.y = rndr(30, 70);
			p.vx = rndr(-30, 30);
			p.vy = rndr(-60, 40);
			p.vz = rndr(-30, 30);
			if (player_in_solid(w, &p))
				p.y = 66;
		}
		float dt = i % 100 < 10 ? rndr(0.05f, 0.2f) : rndr(0.002f, 0.03f);
		bl_player_step(w, &p, &in, dt);
		if (player_in_solid(w, &p))
			inside++;
		if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z))
			nans++;
	}
	CHECK(inside == 0, "%d steps ended inside a block", inside);
	CHECK(nans == 0, "%d steps made a position that is not a number", nans);
	CHECK(p.x > 0.0f && p.x < 128.0f && p.z > 0.0f && p.z < 128.0f, "the player stays in the world (%.1f %.1f)", p.x, p.z);
	bl_world_free(w);
}

/* ---- the save file -------------------------------------------------------------------------------------------------- */

static void tmpdir(void)
{
	if (g_dir[0])
		return;
	const char *base = getenv("TMPDIR");
	snprintf(g_dir, sizeof g_dir, "%s/blocks-test-XXXXXX", base && *base ? base : "/tmp");
	if (!mkdtemp(g_dir))
		g_dir[0] = 0;
}

static void test_save(void)
{
	tmpdir();
	if (!g_dir[0]) {
		CHECK(0, "no temporary folder");
		return;
	}
	char path[400];
	snprintf(path, sizeof path, "%s/world.bl", g_dir);

	struct bl_world *w = bl_world_new(7, 96, 96, 64);
	for (int i = 0; i < 300; i++)
		bl_set(w, (int)(rnd() % 96), (int)(rnd() % 64), (int)(rnd() % 96), (int)(rnd() % BL_COUNT));
	CHECK(bl_save(w, path) == 0, "saved");
	struct stat st;
	CHECK(stat(path, &st) == 0 && (st.st_mode & 077) == 0, "the file is private");
	CHECK(stat(path, &st) == 0 && st.st_size < 4000, "only the edits are saved (%ld bytes)", (long)st.st_size);
	const char *err = NULL;
	struct bl_world *r = bl_load(path, &err);
	CHECK(r != NULL, "loaded: %s", err ? err : "");
	if (r) {
		CHECK(bl_hash(r) == bl_hash(w) && r->seed == 7 && r->w == 96 && r->h == 64, "the world comes back the same");
		check_columns(r, "loaded world");
		bl_world_free(r);
	}
	bl_world_free(w);

	/* with a note of the caller's own at the end */
	w = bl_world_flat(32, 32, 48, 12);
	bl_set(w, 4, 11, 4, BL_LAMP);
	const char *note = "x = 1.5\ninv = 3:4\n";
	CHECK(bl_save_with(w, path, note, strlen(note)) == 0, "saved with a note");
	char *got = NULL;
	r = bl_load_with(path, &got, &err);
	CHECK(r && bl_hash(r) == bl_hash(w) && got && !strcmp(got, note), "the world and the note come back");
	bl_world_free(r);
	free(got);
	r = bl_load(path, &err);
	CHECK(r && bl_hash(r) == bl_hash(w), "a reader that wants no note reads the world");
	bl_world_free(r);
	got = (char *)path;
	CHECK(bl_save(w, path) == 0 && (r = bl_load_with(path, &got, &err)) != NULL && got == NULL, "a file without one gives none");
	bl_world_free(r);
	CHECK(bl_save_with(w, path, note, BL_NOTE_MAX + 1) != 0, "a note that is too long is refused");
	bl_save_with(w, path, note, strlen(note));
	CHECK(stat(path, &st) == 0 && truncate(path, st.st_size - 1) == 0 && bl_load(path, &err) == NULL, "a note that ends early is refused");
	bl_world_free(w);

	/* a flat world too, and an unedited one is a header only */
	w = bl_world_flat(32, 32, 48, 12);
	bl_set(w, 3, 11, 3, BL_BRICK);
	CHECK(bl_save(w, path) == 0, "flat saved");
	r = bl_load(path, &err);
	CHECK(r && r->kind == 1 && bl_hash(r) == bl_hash(w), "the flat world comes back");
	bl_world_free(r);
	bl_world_free(w);

	/* what is not a world file: nothing opens, whatever is cut off or changed */
	CHECK(bl_load("/nonexistent/dir/file", &err) == NULL && err, "a missing file is refused");
	w = bl_world_flat(16, 16, 32, 10);
	for (int i = 0; i < 6; i++)
		bl_set(w, i * 2, 9, 5, BL_STONE + i % 3 + 1 > BL_COUNT - 1 ? BL_BRICK : BL_STONE + i % 3 + 1);
	bl_save(w, path);
	FILE *f = fopen(path, "rb");
	uint8_t data[512];
	size_t len = f ? fread(data, 1, sizeof data, f) : 0;
	if (f)
		fclose(f);
	CHECK(len > 40 && len < 100, "the small file is %zu bytes", len);
	char bad[400];
	snprintf(bad, sizeof bad, "%s/bad.bl", g_dir);
	int cut_ok = 0;
	for (size_t n = 0; n < len; n++) {
		f = fopen(bad, "wb");
		fwrite(data, 1, n, f);
		fclose(f);
		struct bl_world *x = bl_load(bad, &err);
		if (x) {
			cut_ok++;
			bl_world_free(x);
		}
	}
	CHECK(cut_ok == 0, "%d cut-off files were accepted", cut_ok);
	f = fopen(bad, "wb");
	fwrite(data, 1, len, f);
	fputc(0, f);
	fclose(f);
	struct bl_world *x = bl_load(bad, &err);
	CHECK(x == NULL, "a file with something after the end is refused");
	bl_world_free(x);
	/* single bytes changed: it may open or not, but it must not crash, and a world that opens is a whole one */
	int opened = 0, refused = 0;
	for (int i = 0; i < 3000; i++) {
		uint8_t m[512];
		memcpy(m, data, len);
		int k = (int)(rnd() % (uint32_t)len);
		m[k] = (uint8_t)rnd();
		if (i % 3 == 0 && len > 2) {
			int k2 = (int)(rnd() % (uint32_t)len);
			m[k2] ^= (uint8_t)(1u << (rnd() % 8));
		}
		f = fopen(bad, "wb");
		fwrite(m, 1, len, f);
		fclose(f);
		x = bl_load(bad, &err);
		if (x) {
			opened++;
			bl_hash(x);
			check_columns(x, "a changed file");
			bl_world_free(x);
		} else {
			refused++;
			if (!err || !*err)
				CHECK(0, "a refusal without a reason");
		}
	}
	CHECK(refused > 1000, "changed files are mostly refused (%d refused, %d opened)", refused, opened);
	bl_world_free(w);
	/* the save never overwrites by accident: a leftover temporary name does not matter, the target is replaced whole */
	w = bl_world_flat(16, 16, 32, 10);
	CHECK(bl_save(w, path) == 0, "saved over the old file");
	r = bl_load(path, &err);
	CHECK(r && bl_hash(r) == bl_hash(w), "and it is the new one");
	bl_world_free(r);
	bl_world_free(w);
	CHECK(bl_save(w = bl_world_flat(16, 16, 32, 10), "/nonexistent/dir/file") == -1, "saving where it cannot fails");
	bl_world_free(w);
}

/* ---- the textures and the picture ------------------------------------------------------------------------------------ */

static void test_textures(void)
{
	for (int b = 1; b < BL_COUNT; b++)
		for (int f = 0; f < BL_FACE_KINDS; f++) {
			const uint32_t *t = bl_texture(b, f);
			CHECK(t != NULL, "no texture for %s", bl_block_name(b));
			if (!t)
				continue;
			int clear = 0, solid = 0, partial = 0;
			uint32_t seen[8];
			int nseen = 0;
			for (int i = 0; i < 256; i++) {
				int a = (int)(t[i] >> 24);
				if (a == 0)
					clear++;
				else if (a == 255)
					solid++;
				else
					partial++;
				bool found = false;
				for (int k = 0; k < nseen; k++)
					found |= seen[k] == t[i];
				if (!found && nseen < 8)
					seen[nseen++] = t[i];
			}
			if (b == BL_GLASS) {
				CHECK(clear > 100 && partial + solid > 20, "glass: a clear middle and a frame (%d clear)", clear);
			} else if (bl_water(b)) {
				CHECK(solid == 0 && clear == 0, "water is see-through everywhere");
			} else if (b == BL_GATE) {
				CHECK(clear > 40 && solid > 100 && partial == 0, "a gate is bars with gaps between them (%d clear, %d solid)", clear, solid);
			} else if (b == BL_TORCH) {
				CHECK(clear > 150 && solid > 8, "a torch is a stick and a flame on a clear background (%d clear, %d solid)", clear, solid);
			} else {
				CHECK(solid == 256, "%s has pixels that are not solid", bl_block_name(b));
				CHECK(nseen >= 4, "%s is one flat colour", bl_block_name(b));
			}
		}
	CHECK(memcmp(bl_texture(BL_GRASS, BL_FACE_TOP), bl_texture(BL_GRASS, BL_FACE_SIDE), 1024) != 0, "grass has a top and a side");
	CHECK(memcmp(bl_texture(BL_LOG, BL_FACE_TOP), bl_texture(BL_LOG, BL_FACE_SIDE), 1024) != 0, "a log has rings on its end");
	CHECK(bl_texture(BL_AIR, 0) == NULL && bl_texture(BL_COUNT, 0) == NULL && bl_texture(BL_STONE, 3) == NULL, "no texture for what is none");
	uint32_t g = bl_block_color(BL_LEAVES), s = bl_block_color(BL_SNOW), t = bl_block_color(BL_STONE), a = bl_block_color(BL_SAND);
	CHECK(((g >> 8) & 255) > ((g >> 16) & 255) && ((g >> 8) & 255) > (g & 255), "leaves are green (%06x)", g);
	CHECK((s >> 16) > 230 && ((s >> 8) & 255) > 230, "snow is white (%06x)", s);
	CHECK(abs((int)((t >> 16) & 255) - (int)(t & 255)) < 12, "stone is grey (%06x)", t);
	CHECK((a >> 16) > 180, "sand is light (%06x)", a);
	/* sun and daylight */
	float sun[3];
	bl_sun_dir(12.0f, sun);
	CHECK(sun[1] > 0.9f, "the sun is high at noon");
	bl_sun_dir(6.0f, sun);
	CHECK(fabsf(sun[1]) < 0.01f && sun[0] > 0.8f, "the sun is on the horizon at six");
	CHECK(bl_daylight(12.0f) > 0.99f && bl_daylight(0.0f) < 0.01f && bl_daylight(6.0f) > 0.2f && bl_daylight(6.0f) < 0.9f, "day and night");
}

struct rgb { float r, g, b; };

static struct rgb average(const uint32_t *px, int stride, int x0, int y0, int w, int h)
{
	double r = 0, g = 0, b = 0;
	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++) {
			uint32_t c = px[y * stride + x];
			r += (c >> 16) & 255;
			g += (c >> 8) & 255;
			b += c & 255;
		}
	double n = (double)w * (double)h;
	return (struct rgb){ (float)(r / n), (float)(g / n), (float)(b / n) };
}

static void test_render(void)
{
	struct bl_world *w = bl_world_flat(64, 64, 64, 20);
	enum { W = 96, H = 64 };
	static uint32_t a[W * H], b[W * H], c[W * H];
	struct bl_camera cam = { 32.5f, 21.62f, 32.5f, 0.0f, -1.2f, 1.2f };
	struct bl_view v;
	bl_view_init(&v);

	bl_render(w, &cam, &v, a, W, H, W, 1);
	bl_render(w, &cam, &v, b, W, H, W, 1);
	bl_render(w, &cam, &v, c, W, H, W, 4);
	CHECK(memcmp(a, b, sizeof a) == 0, "the same view makes the same picture");
	CHECK(memcmp(a, c, sizeof a) == 0, "four threads make the same picture as one");
	int not_opaque = 0;
	for (int i = 0; i < W * H; i++)
		if ((a[i] >> 24) != 255)
			not_opaque++;
	CHECK(not_opaque == 0, "every pixel is opaque");
	struct rgb mid = average(a, W, W / 2 - 4, H / 2 - 4, 8, 8);
	CHECK(mid.g > mid.r && mid.g > mid.b && mid.g > 70.0f, "looking down at noon: grass (%.0f %.0f %.0f)", mid.r, mid.g, mid.b);

	/* the sky */
	cam.pitch = 1.4f;
	bl_render(w, &cam, &v, a, W, H, W, 2);
	mid = average(a, W, W / 2 - 4, H / 2 - 4, 8, 8);
	CHECK(mid.b > mid.r + 40.0f && mid.b > 150.0f, "the noon sky is blue (%.0f %.0f %.0f)", mid.r, mid.g, mid.b);
	v.time_h = 0.0f;
	bl_render(w, &cam, &v, b, W, H, W, 2);
	struct rgb night = average(b, W, W / 2 - 4, H / 2 - 4, 8, 8);
	CHECK(night.r + night.g + night.b < 90.0f, "the midnight sky is dark (%.0f %.0f %.0f)", night.r, night.g, night.b);
	int stars = 0;
	for (int i = 0; i < W * H; i++)
		if (((b[i] >> 16) & 255) > 60)
			stars++;
	CHECK(stars > 3, "there are stars at night (%d bright pixels)", stars);
	v.time_h = 18.2f;
	bl_render(w, &cam, &v, c, W, H, W, 2);
	CHECK(memcmp(a, c, sizeof a) != 0 && memcmp(b, c, sizeof b) != 0, "sunset looks like neither noon nor midnight");

	/* the horizon: ground below the middle, sky above, fog between */
	cam.pitch = 0.0f;
	v.time_h = 12.0f;
	bl_render(w, &cam, &v, a, W, H, W, 2);
	struct rgb up = average(a, W, W / 2 - 4, 2, 8, 8), down = average(a, W, W / 2 - 4, H - 10, 8, 8);
	CHECK(up.b > up.r + 30.0f, "sky above the horizon");
	CHECK(down.g > down.b && down.g > 60.0f && up.b > down.b + 10.0f, "ground below it (%.0f %.0f %.0f)", down.r, down.g, down.b);

	/* the outline of the block aimed at */
	cam.pitch = -1.2f;
	bl_render(w, &cam, &v, a, W, H, W, 1);
	struct bl_hit h;
	float eye[3] = { cam.x, cam.y, cam.z }, look[3] = { 0, 0, 0 };
	struct bl_player pl = { .x = cam.x, .y = cam.y - BL_EYE, .z = cam.z, .yaw = cam.yaw, .pitch = cam.pitch };
	bl_player_look(&pl, look);
	CHECK(bl_pick(w, eye, look, BL_REACH, &h), "something to aim at");
	v.target = true;
	v.tx = h.x;
	v.ty = h.y;
	v.tz = h.z;
	bl_render(w, &cam, &v, b, W, H, W, 1);
	int changed = 0;
	for (int i = 0; i < W * H; i++)
		changed += a[i] != b[i];
	CHECK(changed > 10 && changed < W * H / 4, "the aimed block gets an outline (%d pixels)", changed);
	v.tx = 3;
	v.ty = 10;
	v.tz = 3;
	bl_render(w, &cam, &v, c, W, H, W, 1);
	CHECK(memcmp(a, c, sizeof a) == 0, "an outline round a block nobody sees changes nothing");
	v.target = false;

	/* a torch at night lights the ground round it, warm, and its flame is seen; a lamp shines in daylight too */
	{
		struct bl_world *tw = bl_world_flat(64, 64, 64, 20);
		struct bl_camera tc = { 32.5f, 21.62f, 30.5f, 0.0f, -0.35f, 1.2f };
		struct bl_view nv;
		bl_view_init(&nv);
		nv.time_h = 0.0f;
		bl_render(tw, &tc, &nv, a, W, H, W, 1);
		struct rgb dark = average(a, W, 20, H / 2, W - 40, H / 2 - 4);
		bl_set(tw, 32, 20, 34, BL_TORCH);
		bl_render(tw, &tc, &nv, b, W, H, W, 1);
		struct rgb lit = average(b, W, 20, H / 2, W - 40, H / 2 - 4);
		CHECK(lit.r + lit.g + lit.b > 1.4f * (dark.r + dark.g + dark.b), "a torch lights the ground at night (%.0f %.0f %.0f -> %.0f %.0f %.0f)", dark.r, dark.g, dark.b, lit.r, lit.g, lit.b);
		CHECK(lit.r > lit.b + 6.0f && lit.r / (lit.b + 1.0f) > 1.5f * dark.r / (dark.b + 1.0f), "and the light is warm (red against blue: %.1f against %.1f before)", lit.r / (lit.b + 1.0f), dark.r / (dark.b + 1.0f));
		int flame = 0;
		for (int i = 0; i < W * H; i++) {
			int r = (int)((b[i] >> 16) & 255), g = (int)((b[i] >> 8) & 255), bl = (int)(b[i] & 255);
			flame += r > 220 && g > 140 && bl < 110;
		}
		CHECK(flame >= 2, "the flame of the torch is seen (%d pixels)", flame);
		bl_set(tw, 32, 20, 34, BL_AIR);
		bl_render(tw, &tc, &nv, c, W, H, W, 1);
		CHECK(memcmp(a, c, sizeof a) == 0, "a torch taken away leaves the picture as it was");
		bl_set(tw, 32, 20, 33, BL_LAMP);
		bl_view_init(&nv);
		bl_render(tw, &tc, &nv, a, W, H, W, 1);
		struct rgb lamp = average(a, W, W / 2 - 3, H / 2 - 3, 6, 6);
		CHECK(lamp.r > 190.0f && lamp.g > 140.0f && lamp.b < lamp.r - 60.0f, "a lamp shines in the day (%.0f %.0f %.0f)", lamp.r, lamp.g, lamp.b);
		bl_world_free(tw);
	}

	/* glass: the picture behind a pane shows through, the frame does not */
	bl_set(w, 32, 21, 33, BL_GLASS);
	bl_set(w, 32, 21, 34, BL_BRICK);
	cam.y = 21.5f;
	cam.pitch = 0.0f;
	cam.x = 32.5f;
	cam.z = 31.5f;
	bl_render(w, &cam, &v, a, W, H, W, 1);
	bl_set(w, 32, 21, 33, BL_AIR);
	bl_render(w, &cam, &v, b, W, H, W, 1);
	CHECK(memcmp(a, b, sizeof a) != 0, "a pane of glass is seen");
	struct rgb pane = average(a, W, W / 2 - 2, H / 2 - 2, 4, 4), bare = average(b, W, W / 2 - 2, H / 2 - 2, 4, 4);
	CHECK(fabsf(pane.r - bare.r) + fabsf(pane.g - bare.g) < 40.0f && bare.r > bare.b + 20.0f, "and the brick behind it too (%.0f %.0f %.0f / %.0f %.0f %.0f)", pane.r, pane.g, pane.b, bare.r, bare.g, bare.b);
	bl_world_free(w);

	/* under water the picture is blue and the lake bottom is seen through it */
	w = bl_world_flat(64, 64, 64, 20);
	for (int z = 20; z < 44; z++)
		for (int x = 20; x < 44; x++)
			for (int y = 14; y < 20; y++)
				bl_set(w, x, y, z, y < 19 ? BL_WATER : BL_WATER);
	struct bl_camera under = { 32.5f, 18.5f, 32.5f, 0.0f, -0.2f, 1.2f };
	bl_view_init(&v);
	bl_render(w, &under, &v, a, W, H, W, 2);
	struct rgb all = average(a, W, 0, 0, W, H);
	CHECK(all.b > all.r * 1.5f && all.b > all.g, "under water everything is blue (%.0f %.0f %.0f)", all.r, all.g, all.b);
	struct bl_camera above = { 32.5f, 26.0f, 32.5f, 0.0f, -1.3f, 1.2f };
	bl_render(w, &above, &v, b, W, H, W, 2);
	struct rgb lake = average(b, W, W / 2 - 4, H / 2 - 4, 8, 8);
	CHECK(lake.b > lake.r + 20.0f, "a lake seen from above is blue (%.0f %.0f %.0f)", lake.r, lake.g, lake.b);
	bl_world_free(w);

	/* the picture and the pointing agree: the first block a pixel's ray meets is the one `bl_pick` finds */
	w = bl_world_new(4242, 128, 128, 64);
	for (int i = 0; i < 300; i++)
		bl_set(w, (int)(rnd() % 128), 18 + (int)(rnd() % 24), (int)(rnd() % 128), (int[]){ BL_GLASS, BL_WATER, BL_TORCH, BL_LAMP, BL_FLOW3 }[rnd() % 5]);
	int same = 0, diff = 0, sky = 0;
	struct bl_view idv;
	bl_view_init(&idv);
	idv.ids = true;
	idv.dist = 120.0f;
	for (int i = 0; i < 30000; i++) {
		struct bl_camera cm = { rndr(2, 126), rndr(5, 90), rndr(2, 126), rndr(-7, 7), rndr(-1.5f, 1.5f), 1.0f };
		if (i % 4 == 0)
			cm.y = rndr(18, 50);
		uint32_t px;
		bl_render(w, &cm, &idv, &px, 1, 1, 1, 1);
		float look[3] = { cosf(cm.pitch) * sinf(cm.yaw), sinf(cm.pitch), cosf(cm.pitch) * cosf(cm.yaw) }, o[3] = { cm.x, cm.y, cm.z };
		struct bl_hit h;
		bool got = bl_pick(w, o, look, 120.0f, &h);
		uint32_t want = got ? (0xff000000u | (uint32_t)bl_get(w, h.x, h.y, h.z) << 8 | (uint32_t)h.face) : 0xff0000ffu;
		if (px == want)
			same++;
		else
			diff++;
		sky += !got;
	}
	/* The two use different arithmetic (the picture leaps over empty space). What differs is a ray that grazes the edge of a block 50 to 100
	 * blocks away by less than 1e-4 (600000 rays: 58 such, all of that kind): not visible, not a logic error. */
	CHECK(diff <= 20, "the picture and `bl_pick` disagree on %d of 30000 rays", diff);
	CHECK(sky > 2000 && same - sky > 10000, "the rays hit and miss both (%d misses, %d hits)", sky, same - sky);
	bl_world_free(w);

	/* any camera: inside the ground, outside the world, straight up and down, tiny pictures, no distance - nothing may crash or loop */
	w = bl_world_new(5, 96, 96, 64);
	static uint32_t small[40 * 24];
	int odd = 0;
	for (int i = 0; i < 1500; i++) {
		struct bl_camera cm = { rndr(-60, 160), rndr(-20, 200), rndr(-60, 160), rndr(-10, 10), rndr(-1.7f, 1.7f), rndr(0.1f, 3.0f) };
		if (i % 5 == 0) {
			cm.x = rndr(0, 96);
			cm.z = rndr(0, 96);
			cm.y = rndr(0, 64);
		}
		struct bl_view vv = { .time_h = rndr(0, 24), .dist = rndr(0, 400), .target = i % 3 == 0, .tx = (int)(rnd() % 96), .ty = (int)(rnd() % 64), .tz = (int)(rnd() % 96) };
		bl_render(w, &cm, &vv, small, 40, 24, 40, 1 + (int)(rnd() % 3));
		for (int k = 0; k < 40 * 24; k++)
			odd += (small[k] >> 24) != 255;
	}
	CHECK(odd == 0, "%d pixels that are not opaque", odd);
	bl_render(w, &(struct bl_camera){ 48, 40, 48, 0, 0, 1.2f }, &v, small, 1, 1, 1, 1);
	bl_render(w, &(struct bl_camera){ 48, 40, 48, 0, 0, 1.2f }, &v, small, 0, 0, 0, 1);
	bl_render(w, &(struct bl_camera){ NAN, 40, 48, 0, 0, 1.2f }, &v, small, 40, 24, 40, 1);
	bl_world_free(w);
}


/* ---- Tom and Filip ------------------------------------------------------------------------------------------------------- */

static uint8_t g_glyph[MCAT_GLYPHS][16];
static const uint8_t *g_glyph_ptr[MCAT_GLYPHS];

static void glyphs_make(void)             /* a made up alphabet: dense noise, enough for the fur's shape, shade and place */
{
	for (int i = 0; i < MCAT_GLYPHS; i++) {
		for (int r = 0; r < 16; r++)
			g_glyph[i][r] = (uint8_t)(rnd() | rnd());
		g_glyph_ptr[i] = g_glyph[i];
	}
}

/* the same measure as the module's, written again: twice the spread of the heights + 1.5 for every column that is water or has a tree on it */
static float ref_patch(const struct bl_world *w, int x0, int z0, int *wet_or_tree)
{
	int lo = 999, hi = -1, bad = 0;
	for (int z = z0; z <= z0 + 11; z++)
		for (int x = x0; x <= x0 + 8; x++) {
			int top = bl_top(w, x, z), hy = w->hi[z * w->w + x];
			int b = w->b[bl_index(w, x, hy, z)];
			if (bl_water(b))
				bad += 2;
			else if (b == BL_LEAVES || b == BL_LOG || hy > top)
				bad += 1;
			if (top < lo) lo = top;
			if (top > hi) hi = top;
		}
	*wet_or_tree = bad;
	return 2.0f * (float)(hi - lo) + 1.5f * (float)bad;
}

#define T_HW 0.26f                                  /* the boxes of the pets (blockspets.c) */
#define T_H 0.70f

static bool box_in_solid(const struct bl_world *w, float x, float y, float z, float hw, float h)
{
	for (int yy = (int)floorf(y + 1e-3f); yy <= (int)floorf(y + h - 1e-3f); yy++)
		for (int zz = (int)floorf(z - hw + 1e-3f); zz <= (int)floorf(z + hw - 1e-3f); zz++)
			for (int xx = (int)floorf(x - hw + 1e-3f); xx <= (int)floorf(x + hw - 1e-3f); xx++)
				if (bl_solid(bl_get(w, xx, yy, zz)))
					return true;
	return false;
}

/* the same for a goblin, a worker or a soldier: leaves are no block to them */
static bool mob_in_solid(const struct bl_world *w, float x, float y, float z, float hw, float h)
{
	for (int yy = (int)floorf(y + 1e-3f); yy <= (int)floorf(y + h - 1e-3f); yy++)
		for (int zz = (int)floorf(z - hw + 1e-3f); zz <= (int)floorf(z + hw - 1e-3f); zz++)
			for (int xx = (int)floorf(x - hw + 1e-3f); xx <= (int)floorf(x + hw - 1e-3f); xx++)
				if (bl_solid_unit(bl_get(w, xx, yy, zz)))
					return true;
	return false;
}

static bool wet(const struct bl_world *w, float x, float y, float z)
{
	return bl_water(bl_get(w, (int)floorf(x), (int)floorf(y), (int)floorf(z)));
}

static void run_pets(struct bl_pets *p, struct bl_world *w, float secs)
{
	int n = (int)(secs * 30.0f);
	for (int i = 0; i < n; i++)
		bl_pets_step(p, w, 1.0f / 30.0f);
}

/* a flat world with a yard and a pond dug next to it */
static struct bl_world *pets_flat(struct bl_pets *p)
{
	struct bl_world *w = bl_world_flat(64, 64, 64, 20);
	bl_pets_init(p, 0, g_glyph_ptr, MCAT_GLYPHS);
	float near[3] = { 32.5f, 20.0f, 32.5f };
	bl_pets_place(p, w, near);
	return w;
}

static void test_pets(void)
{
	glyphs_make();
	/* the room's area: Tom stays inside it for his whole scripted day, whatever the seed (the yard is made to this size) */
	static const uint32_t seeds[4] = { 0u, 1u, 7u, 12345u };
	int out_cat = 0;
	for (int si = 0; si < 4; si++) {
		mcat_set_seed(seeds[si]);
		for (float t = 0.0f; t < 1500.0f; t += 0.7f) {
			struct mcat_state st;
			mcat_state(t, &st);
			if (st.z < MCAT_AREA_Z0 - 0.3f || st.z > MCAT_AREA_Z1 + 0.3f || fabsf(st.x) > MCAT_AREA_X + 0.3f)
				out_cat++;
		}
	}
	CHECK(out_cat == 0, "the scripted Tom stays in the area: %d samples outside it", out_cat);

	/* the yard: on dry ground that is flat, not under the player, inside the world; a tiny world has none */
	struct bl_pets pets;
	bl_pets_init(&pets, 0, g_glyph_ptr, MCAT_GLYPHS);
	struct bl_world *w = bl_world_new(9, 256, 256, 64), *w2 = bl_world_new(9, 256, 256, 64);
	float near[3];
	bl_spawn(w, near);
	CHECK(bl_pets_place(&pets, w, near), "a yard is found in a world made from a seed");
	float dist = hypotf(pets.yard[0] - near[0], pets.yard[2] - near[2]);
	CHECK(dist >= 9.99f && dist <= 60.0f, "the yard is 10 to 48 blocks from the player (%.1f)", dist);
	bool inside = near[0] > pets.yard[0] - 4.3f && near[0] < pets.yard[0] + 4.3f && near[2] > pets.yard[2] - 5.0f && near[2] < pets.yard[2] + 5.7f;
	CHECK(!inside, "the player is not in the yard");
	int x0 = (int)floorf(pets.yard[0] - MCAT_AREA_X - 0.3f), z0 = (int)floorf(pets.yard[2] + MCAT_AREA_Z0 - 0.3f), bad;
	float chosen = ref_patch(w2, x0, z0, &bad);                  /* (measured on a copy: the pond that was dug is not in it) */
	int worse = 0;
	for (int i = 0; i < 60; i++) {
		int rx = 4 + (int)(rnd() % 230), rz = 4 + (int)(rnd() % 230), b2;
		if (ref_patch(w2, rx, rz, &b2) < chosen - 2.0f)
			worse++;
	}
	CHECK(worse <= 4, "the yard is among the best patches: %d of 60 random ones are clearly better (score %.1f)", worse, chosen);
	struct bl_pets_info in;
	bl_pets_info(&pets, &in);
	CHECK(in.filip_in_water, "Filip starts in water (a lake or the sea near the player, or the pond that was dug)");
	CHECK(fabsf(in.tom[0] - pets.yard[0]) < 0.01f && in.tom_mode == 0, "Tom starts in the middle of his yard, sitting");
	struct bl_pets again;
	bl_pets_init(&again, 0, g_glyph_ptr, MCAT_GLYPHS);
	bl_pets_place(&again, w2, near);
	CHECK(again.yard[0] == pets.yard[0] && again.yard[2] == pets.yard[2] && again.pond[0] == pets.pond[0], "the same world and place give the same yard and water");
	bl_world_free(w);
	bl_world_free(w2);
	struct bl_world *tiny = bl_world_new(1, 16, 16, 64);
	CHECK(!bl_pets_place(&pets, tiny, (float[3]){ 8, 30, 8 }), "a world of 16 x 16 has no room for the yard");
	bl_world_free(tiny);

	/* a flat world has no water: a pond is dug next to the yard, level with the ground, and Filip is in it */
	struct bl_world *flat = pets_flat(&pets);
	CHECK(fabsf(pets.yard[0] - 42.5f) < 0.01f && fabsf(pets.yard[2] - 32.5f) < 0.01f && pets.yard[1] == 20.0f && pets.made_pond,
	      "on a flat world the yard is the nearest one allowed and the pond is dug (%.1f %.1f %.1f)", pets.yard[0], pets.yard[1], pets.yard[2]);
	long water = 0;
	for (size_t i = 0; i < (size_t)64 * 64 * 64; i++)
		water += bl_water(flat->b[i]);
	CHECK(water > 60 && water < 120, "a pond of about 30 columns, three deep (%ld cells)", water);
	CHECK(wet(flat, pets.pond[0], 19.5f, pets.pond[2]) && !wet(flat, pets.yard[0], 19.5f, pets.yard[2]), "the pond is where it says and the yard is dry");
	bl_pets_info(&pets, &in);
	CHECK(in.filip_in_water && in.filip[1] > 17.2f && in.filip[1] < 20.0f, "Filip is in the pond (y %.2f)", in.filip[1]);

	/* an hour... of Tom, 15 minutes of it, in steps of 1/30 s: on the ground, never in a block or in water, near his yard, and he does things */
	{
		unsigned modes = 0;
		float path = 0, maxd = 0, px = in.tom[0], pz = in.tom[2];
		int in_block = 0, in_water = 0, off_ground = 0;
		for (int i = 0; i < 27000; i++) {
			bl_pets_step(&pets, flat, 1.0f / 30.0f);
			bl_pets_info(&pets, &in);
			modes |= 1u << in.tom_mode;
			path += hypotf(in.tom[0] - px, in.tom[2] - pz);
			px = in.tom[0];
			pz = in.tom[2];
			float dy = hypotf(in.tom[0] - pets.yard[0], in.tom[2] - pets.yard[2]);
			maxd = dy > maxd ? dy : maxd;
			in_block += box_in_solid(flat, in.tom[0], in.tom[1], in.tom[2], T_HW, T_H);
			in_water += wet(flat, in.tom[0], in.tom[1] + 0.1f, in.tom[2]);
			off_ground += in.tom[1] < 19.99f || in.tom[1] > 21.2f;
		}
		int kinds = 0;
		for (int k = 0; k < 7; k++)
			kinds += (modes >> k) & 1u;
		CHECK(in_block == 0 && in_water == 0 && off_ground == 0, "Tom: in a block %d, in water %d, off the ground %d steps of 27000", in_block, in_water, off_ground);
		CHECK(path > 30.0f && maxd < 11.0f && maxd > 3.0f, "he walks %.0f m and gets as far as %.1f m from his yard", path, maxd);
		CHECK(kinds >= 4, "he sits, rises, walks, settles, lies or hops (%d kinds of doing: mask %x)", kinds, modes);
	}
	bl_world_free(flat);

	/* a wall of two blocks stops him, one is climbed */
	flat = pets_flat(&pets);
	for (int x = 20; x < 60; x++)
		for (int y = 20; y < 22; y++)
			bl_set(flat, x, y, 36, BL_STONE);
	bl_pets_tom_go(&pets, 42.5f, 41.0f, false);
	run_pets(&pets, &*flat, 25.0f);
	bl_pets_info(&pets, &in);
	CHECK(in.tom[2] < 36.0f - T_HW + 0.05f && in.tom[1] < 20.2f, "a wall of two blocks stops him (z %.2f y %.2f)", in.tom[2], in.tom[1]);
	for (int x = 20; x < 60; x++)
		bl_set(flat, x, 21, 36, BL_AIR);
	bl_pets_tom_go(&pets, 42.5f, 41.0f, false);
	float zmax = 0, ytop = 0;
	for (int i = 0; i < 20 * 30; i++) {
		bl_pets_step(&pets, flat, 1.0f / 30.0f);
		bl_pets_info(&pets, &in);
		zmax = in.tom[2] > zmax ? in.tom[2] : zmax;
		ytop = in.tom[1] > ytop ? in.tom[1] : ytop;
		CHECK(!box_in_solid(flat, in.tom[0], in.tom[1], in.tom[2], T_HW, T_H), "Tom is inside the wall at step %d (%.2f %.2f %.2f)", i, in.tom[0], in.tom[1], in.tom[2]);
	}
	CHECK(zmax > 39.5f && ytop > 20.9f, "a wall of one block is climbed and left behind (z %.2f, highest %.2f)", zmax, ytop);
	bl_world_free(flat);

	/* the ground goes: he falls and lands; a trench three deep across his way is not crossed */
	flat = pets_flat(&pets);
	for (int z = 30; z <= 34; z++)
		for (int x = 40; x <= 44; x++)
			for (int y = 17; y < 20; y++)
				bl_set(flat, x, y, z, BL_AIR);
	run_pets(&pets, flat, 1.5f);
	bl_pets_info(&pets, &in);
	CHECK(fabsf(in.tom[1] - 17.0f) < 0.02f && !box_in_solid(flat, in.tom[0], in.tom[1], in.tom[2], T_HW, T_H), "the ground goes and he falls three blocks and lands (y %.3f)", in.tom[1]);
	bl_world_free(flat);
	flat = pets_flat(&pets);
	for (int x = 20; x < 60; x++)
		for (int z = 36; z <= 37; z++)
			for (int y = 17; y < 20; y++)
				bl_set(flat, x, y, z, BL_AIR);
	bl_pets_tom_go(&pets, 42.5f, 41.0f, false);
	run_pets(&pets, flat, 25.0f);
	bl_pets_info(&pets, &in);
	CHECK(in.tom[2] < 36.05f && in.tom[1] > 19.99f, "a trench is not crossed and not fallen into (z %.2f, y %.2f)", in.tom[2], in.tom[1]);
	bl_world_free(flat);

	/* water: he is sent into the pond and never gets wet */
	flat = pets_flat(&pets);
	bl_pets_tom_go(&pets, pets.pond[0], pets.pond[2], true);
	int wetn = 0;
	float nearest = 99;
	for (int i = 0; i < 30 * 40; i++) {
		bl_pets_step(&pets, flat, 1.0f / 30.0f);
		bl_pets_info(&pets, &in);
		wetn += wet(flat, in.tom[0], in.tom[1] + 0.1f, in.tom[2]);
		float d = hypotf(in.tom[0] - pets.pond[0], in.tom[2] - pets.pond[2]);
		nearest = d < nearest ? d : nearest;
	}
	CHECK(wetn == 0 && nearest < 6.0f, "sent into the pond he gets to its edge (%.1f m) and no further (wet %d steps)", nearest, wetn);
	bl_world_free(flat);

	/* the same world, the same day */
	struct bl_pets pa, pb;
	struct bl_world *wa = pets_flat(&pa), *wb = pets_flat(&pb);
	run_pets(&pa, wa, 120.0f);
	run_pets(&pb, wb, 120.0f);
	CHECK(pa.tom.x == pb.tom.x && pa.tom.z == pb.tom.z && pa.tom.mode == pb.tom.mode && pa.filip.x == pb.filip.x && pa.filip.y == pb.filip.y && pa.filip.z == pb.filip.z,
	      "the same world and seed give the same two minutes");
	bl_world_free(wa);
	bl_world_free(wb);

	/* bodies: pushed away by the player, lifted onto, not built into */
	flat = pets_flat(&pets);
	bl_pets_info(&pets, &in);
	struct bl_player pl;
	bl_player_init(&pl, flat);
	pl.x = in.tom[0] + 0.3f;
	pl.z = in.tom[2];
	pl.y = 20.0f;
	float tx0 = in.tom[0];
	bl_pets_push_player(&pets, flat, &pl);
	bl_pets_info(&pets, &in);
	CHECK(fabsf(pl.x - in.tom[0]) >= 0.3f + T_HW - 0.02f && in.tom[0] < tx0, "the player walking into Tom pushes him away (%.3f apart, Tom moved %.3f)", fabsf(pl.x - in.tom[0]), tx0 - in.tom[0]);
	pl.x = in.tom[0];
	pl.z = in.tom[2];
	pl.y = in.tom[1] + T_H - 0.1f;
	pl.vy = -2.0f;
	bl_pets_push_player(&pets, flat, &pl);
	CHECK(fabsf(pl.y - (in.tom[1] + T_H)) < 0.01f && pl.on_ground, "a player that comes down on him stands on him");
	/* a wall right behind him: he cannot give way, so the player steps back */
	for (int z = 28; z <= 36; z++)
		for (int y = 20; y < 22; y++)
			bl_set(flat, 40, y, z, BL_STONE);
	pets.tom.x = 41.0f + T_HW + 0.002f;
	pets.tom.z = 32.5f;
	pets.tom.y = 20.0f;
	pl.x = pets.tom.x + 0.3f;
	pl.z = pets.tom.z;
	pl.y = 20.0f;
	pl.vy = 0.0f;
	bl_pets_push_player(&pets, flat, &pl);
	bl_pets_info(&pets, &in);
	CHECK(fabsf(pl.x - in.tom[0]) >= 0.3f + T_HW - 0.02f && in.tom[0] > 41.0f + T_HW - 0.01f && !box_in_solid(flat, in.tom[0], in.tom[1], in.tom[2], T_HW, T_H),
	      "with a wall behind him the player steps back instead (%.3f apart, Tom at %.3f)", fabsf(pl.x - in.tom[0]), in.tom[0]);
	CHECK(bl_pets_hit_cell(&pets, (int)floorf(in.tom[0]), 20, (int)floorf(in.tom[2])) && !bl_pets_hit_cell(&pets, (int)floorf(in.tom[0]) + 3, 20, (int)floorf(in.tom[2])),
	      "a block cannot be put where he is, one beside him can");
	CHECK(bl_pets_hit_cell(&pets, (int)floorf(in.filip[0]), (int)floorf(in.filip[1]), (int)floorf(in.filip[2])), "nor where Filip is");
	pets.on = false;
	CHECK(!bl_pets_hit_cell(&pets, (int)floorf(in.tom[0]), 20, (int)floorf(in.tom[2])), "pets that are switched off are no bodies");
	bl_world_free(flat);

	/* Filip: 15 minutes in the pond: in the water all the time, between the surface and the bottom, rests on the bottom, lets bubbles go */
	flat = pets_flat(&pets);
	{
		int dry = 0, out = 0, bubbles = 0;
		float rest_max = 0, rest_y_err = 0, ylo = 99, yhi = -99;
		for (int i = 0; i < 27000; i++) {
			bl_pets_step(&pets, flat, 1.0f / 30.0f);
			bl_pets_info(&pets, &in);
			dry += !in.filip_in_water;
			float d = hypotf(in.filip[0] - pets.pond[0], in.filip[2] - pets.pond[2]);
			out += d > 3.6f || in.filip[1] > pets.pond[1] || in.filip[1] < pets.pond[1] - 3.0f;
			bubbles |= pets.filip.nbub > 0;
			ylo = in.filip[1] < ylo ? in.filip[1] : ylo;
			yhi = in.filip[1] > yhi ? in.filip[1] : yhi;
			if (in.filip_rest > rest_max)
				rest_max = in.filip_rest;
			if (in.filip_rest > 0.999f) {
				float e = fabsf(in.filip[1] - (pets.pond[1] - 3.0f + 0.065f));
				rest_y_err = e > rest_y_err ? e : rest_y_err;
			}
		}
		CHECK(dry == 0 && out == 0, "Filip never leaves the water: %d steps dry, %d outside the pond", dry, out);
		CHECK(rest_max > 0.999f && rest_y_err < 0.03f, "he rests on the bottom, the middle of his shell %.3f off where it should be", rest_y_err);
		CHECK(bubbles, "he lets bubbles go");
		CHECK(yhi - ylo > 0.7f, "and swims at different depths (%.2f to %.2f)", ylo, yhi);
	}
	/* the water goes: he sinks to what is under him and stays */
	for (int z = 28; z <= 38; z++)
		for (int x = 30; x <= 42; x++)
			for (int y = 17; y <= 19; y++)
				if (wet(flat, (float)x, (float)y, (float)z))
					bl_set(flat, x, y, z, BL_AIR);
	run_pets(&pets, flat, 6.0f);
	bl_pets_info(&pets, &in);
	float y1 = in.filip[1];
	run_pets(&pets, flat, 2.0f);
	bl_pets_info(&pets, &in);
	CHECK(!in.filip_in_water && fabsf(in.filip[1] - y1) < 0.01f && in.filip[1] < 17.6f && in.filip_rest > 0.99f, "without water he lies on the floor of the pit (y %.2f)", in.filip[1]);
	bl_world_free(flat);

	/* water that was there: the sea of a world made from a seed, near the player */
	struct bl_world *sea = bl_world_new(31, 96, 96, 64);
	float snear[3];
	bl_spawn(sea, snear);
	bl_pets_init(&pets, 31, g_glyph_ptr, MCAT_GLYPHS);
	CHECK(bl_pets_place(&pets, sea, snear), "pets in a world with a sea");
	int sdry = 0;
	for (int i = 0; i < 9000; i++) {
		bl_pets_step(&pets, sea, 1.0f / 30.0f);
		bl_pets_info(&pets, &in);
		sdry += !in.filip_in_water;
		if (box_in_solid(sea, in.tom[0], in.tom[1], in.tom[2], T_HW, T_H))
			sdry += 1000;
	}
	CHECK(sdry == 0, "five minutes in a world with a sea: Filip in water all the time, Tom in no block (%d)", sdry);
	bl_world_free(sea);
}

static void test_pets_render(void)
{
	enum { W = 160, H = 100 };
	static uint32_t a[W * H], b[W * H], c[W * H];
	struct bl_pets pets;
	struct bl_world *w = pets_flat(&pets);
	run_pets(&pets, w, 1.0f);
	struct bl_pets_info in;
	bl_pets_info(&pets, &in);
	float cam0[3] = { in.tom[0] - 4.0f, 21.62f, in.tom[2] - 5.0f };
	bl_pets_pose(&pets, w, 1.0f, 0.016f, cam0, 12.0f);
	/* aim the camera at the middle of Tom */
	float tx = in.tom[0] - cam0[0], ty = in.tom[1] + 0.3f - cam0[1], tz = in.tom[2] - cam0[2];
	struct bl_camera cam = { cam0[0], cam0[1], cam0[2], atan2f(tx, tz), atan2f(ty, hypotf(tx, tz)), 0.8f };
	struct bl_view v;
	bl_view_init(&v);
	bl_render(w, &cam, &v, a, W, H, W, 1);
	v.pets = &pets;
	bl_render(w, &cam, &v, b, W, H, W, 1);
	int changed = 0, cream = 0;
	for (int i = 0; i < W * H; i++)
		if (a[i] != b[i]) {
			changed++;
			int r = (int)((b[i] >> 16) & 255), g = (int)((b[i] >> 8) & 255), bl = (int)(b[i] & 255);
			cream += r > bl + 25 && g > bl + 5 && r > 90;
		}
	CHECK(changed > 150, "Tom is in the picture (%d pixels differ)", changed);
	CHECK(cream > changed / 5, "and he is cream-coloured: %d of %d changed pixels", cream, changed);
	int mid_changed = 0;
	for (int y = H / 2 - 4; y < H / 2 + 4; y++)
		for (int x = W / 2 - 4; x < W / 2 + 4; x++)
			mid_changed += a[y * W + x] != b[y * W + x];
	CHECK(mid_changed > 40, "in the middle, where the camera looks at him (%d of 64)", mid_changed);
	bl_render(w, &cam, &v, c, W, H, W, 4);
	CHECK(memcmp(b, c, sizeof b) == 0, "four threads draw the pets as one thread does");
	pets.on = false;
	bl_render(w, &cam, &v, c, W, H, W, 1);
	CHECK(memcmp(a, c, sizeof a) == 0, "with the pets switched off the picture is the one without them");
	pets.on = true;

	/* the ids hook: Tom where he is, a wall in front of him hides him, a wall behind him does not */
	struct bl_view iv = v;
	iv.ids = true;
	uint32_t px;
	struct bl_camera one = cam;
	bl_render(w, &one, &iv, &px, 1, 1, 1, 1);
	CHECK(px == 0xff0000feu, "the ray at Tom meets Tom (%08x)", px);
	float wallz = cam0[2] + (in.tom[2] - cam0[2]) * 0.5f;
	for (int y = 20; y < 27; y++)
		for (int x = (int)(in.tom[0] - 6); x < (int)(in.tom[0] + 3); x++)
			bl_set(w, x, y, (int)wallz, BL_STONE);
	bl_render(w, &one, &iv, &px, 1, 1, 1, 1);
	CHECK(px == (0xff000000u | (uint32_t)BL_STONE << 8 | (uint32_t)BL_NZ) && px != 0xff0000feu, "a wall in front of him is what is met, and he is hidden (%08x)", px);
	for (int y = 20; y < 27; y++)
		for (int x = (int)(in.tom[0] - 6); x < (int)(in.tom[0] + 3); x++)
			bl_set(w, x, y, (int)wallz, BL_AIR);
	for (int y = 20; y < 27; y++)
		for (int x = (int)(in.tom[0] - 3); x < (int)(in.tom[0] + 4); x++)
			bl_set(w, x, y, (int)(in.tom[2] + 2.0f), BL_STONE);
	bl_render(w, &one, &iv, &px, 1, 1, 1, 1);
	CHECK(px == 0xff0000feu, "a wall behind him leaves him in front of it (%08x)", px);
	for (int y = 20; y < 27; y++)
		for (int x = (int)(in.tom[0] - 3); x < (int)(in.tom[0] + 4); x++)
			bl_set(w, x, y, (int)(in.tom[2] + 2.0f), BL_AIR);

	/* Filip is in the pond, seen through the water: the ray at him meets him, and his colour is tinted blue by the water in front of him */
	struct bl_camera fc = { in.pond[0] + 3.0f, in.pond[1] + 3.0f, in.pond[2] - 3.5f, 0, 0, 0.8f };
	float fx = in.filip[0] - fc.x, fy = in.filip[1] - fc.y, fz = in.filip[2] - fc.z;
	fc.yaw = atan2f(fx, fz);
	fc.pitch = atan2f(fy, hypotf(fx, fz));
	float eye[3] = { fc.x, fc.y, fc.z };
	bl_pets_pose(&pets, w, 1.0f, 0.016f, eye, 12.0f);
	bl_render(w, &fc, &iv, &px, 1, 1, 1, 1);
	CHECK(px == 0xff0000fdu, "the ray at Filip meets Filip (%08x)", px);
	struct bl_view fv = v;
	bl_render(w, &fc, &fv, a, W, H, W, 1);
	fv.pets = NULL;
	bl_render(w, &fc, &fv, b, W, H, W, 1);
	int fch = 0;
	for (int i = 0; i < W * H; i++)
		fch += a[i] != b[i];
	CHECK(fch > 20, "Filip is in the picture (%d pixels)", fch);
	float ray[3] = { sinf(fc.yaw) * cosf(fc.pitch), sinf(fc.pitch), cosf(fc.yaw) * cosf(fc.pitch) };
	struct bl_pet_event ev[2];
	int nev = bl_pets_events(&pets, eye, ray, 50.0f, 0.001f, ev);
	uint32_t mid = a[(H / 2) * W + W / 2];
	if (nev > 0 && ev[0].a > 0.9f) {
		float raw_br = ev[0].c[2] * 255.0f - ev[0].c[0] * 255.0f, seen_br = (float)(mid & 255) - (float)((mid >> 16) & 255);
		CHECK(seen_br > raw_br + 8.0f, "seen through the water he is bluer than he is (%.0f against %.0f)", seen_br, raw_br);
	} else {
		CHECK(nev > 0, "the ray at Filip has an event (%d)", nev);
	}

	/* the light: no light at six (the sun is on the horizon), the sun at three in the afternoon lights Tom from the side, the moon at midnight a little (the sky's sun is
	 * always to the +z side, so at noon he is lit from behind, which his front does not show) */
	float tray[3] = { sinf(cam.yaw) * cosf(cam.pitch), sinf(cam.pitch), cosf(cam.yaw) * cosf(cam.pitch) }, sum[3] = { 0, 0, 0 };
	static const float hours[3] = { 6.0f, 15.0f, 0.0f };
	for (int k = 0; k < 3; k++) {
		bl_pets_pose(&pets, w, 1.0f, 0.016f, cam0, hours[k]);
		struct bl_pet_event e2[2];
		int n2 = bl_pets_events(&pets, cam0, tray, 50.0f, 0.001f, e2);
		sum[k] = n2 > 0 ? (e2[0].c[0] + e2[0].c[1] + e2[0].c[2]) * 255.0f : 0.0f;
	}
	CHECK(sum[0] > 60.0f && sum[1] > sum[0] + 20.0f && sum[2] > sum[0] + 5.0f, "the sun and the moon light him (%.0f without light, %.0f by the sun, %.0f by the moon)", sum[0], sum[1], sum[2]);

	/* any camera, anywhere, with the pets in the world: no crash, and the pictures are opaque */
	bl_pets_pose(&pets, w, 700.0f, 0.016f, cam0, 7.0f);
	static uint32_t small[40 * 24];
	int odd = 0;
	for (int i = 0; i < 800; i++) {
		struct bl_camera cm = { rndr(20, 70), rndr(15, 40), rndr(15, 55), rndr(-10, 10), rndr(-1.6f, 1.6f), rndr(0.3f, 2.5f) };
		struct bl_view vv = v;
		vv.dist = rndr(16, 200);
		vv.ids = i % 5 == 0;
		bl_render(w, &cm, &vv, small, 40, 24, 40, 1 + (int)(rnd() % 3));
		for (int k = 0; k < 40 * 24; k++)
			odd += !vv.ids && (small[k] >> 24) != 255;
	}
	CHECK(odd == 0, "%d pixels that are not opaque", odd);
	bl_world_free(w);
}

/* ---- the goblins -------------------------------------------------------------------------------------------------------- */

struct mrun {
	float damage;
	int hits, born, burned;
	float first_hit, last_hit, min_gap, t;
	float min_dist;
};

/* step the mobs for `secs` with the player standing at `pl`; collects what happened and checks that no mob is ever in a block */
static int run_mobs(struct bl_mobs *M, const struct bl_world *w, const float pl[3], bool alive, float hour, float secs, struct mrun *r)
{
	int in_block = 0;
	int n = (int)(secs * 30.0f);
	for (int i = 0; i < n; i++) {
		struct bl_mobs_result R;
		bl_mobs_step(M, w, pl, alive, hour, 1.0f / 30.0f, &R);
		if (r) {
			r->t += 1.0f / 30.0f;
			r->damage += R.damage;
			r->born += R.born;
			r->burned += R.burned;
			if (R.hits) {
				if (r->hits == 0)
					r->first_hit = r->t;
				else if (r->t - r->last_hit < r->min_gap)
					r->min_gap = r->t - r->last_hit;
				r->last_hit = r->t;
				r->hits += R.hits;
			}
		}
		for (int k = 0; k < BL_MOBS_MAX; k++)
			if (M->mob[k].alive) {
				in_block += box_in_solid(w, M->mob[k].x, M->mob[k].y, M->mob[k].z, BL_GOBLIN_HW, BL_GOBLIN_H);
				if (r) {
					float d = hypotf(M->mob[k].x - pl[0], M->mob[k].z - pl[2]);
					if (d < r->min_dist)
						r->min_dist = d;
				}
			}
	}
	return in_block;
}

static void mrun_init(struct mrun *r)
{
	memset(r, 0, sizeof *r);
	r->min_gap = 99.0f;
	r->min_dist = 99.0f;
}

static const struct bl_mob *first_mob(const struct bl_mobs *M)
{
	for (int i = 0; i < BL_MOBS_MAX; i++)
		if (M->mob[i].alive)
			return &M->mob[i];
	return NULL;
}

static void test_mobs(void)
{
	const float night = 0.0f, noon = 12.0f;
	struct bl_world *w = bl_world_flat(128, 128, 64, 20);
	struct bl_mobs M;
	bl_mobs_init(&M, 7);
	const float pl[3] = { 64.5f, 20.0f, 64.5f };
	CHECK(bl_mobs_count(&M) == 0 && M.enabled && M.cap == 6, "a new set of mobs is empty, on, with room for six");

	/* the places a goblin can be put */
	CHECK(bl_mobs_add(&M, w, BL_MOB_GOBLIN, 50.5f, 20.0f, 50.5f), "a goblin on the floor");
	CHECK(!bl_mobs_add(&M, w, BL_MOB_GOBLIN, 51.5f, 19.5f, 50.5f), "not in the ground");
	CHECK(!bl_mobs_add(&M, w, BL_MOB_GOBLIN, -3.0f, 20.0f, 50.5f), "not outside the world");
	CHECK(bl_mobs_hit_cell(&M, 50, 20, 50) && bl_mobs_hit_cell(&M, 50, 21, 50) && !bl_mobs_hit_cell(&M, 50, 22, 50) && !bl_mobs_hit_cell(&M, 53, 20, 50), "a block cannot be put where it stands");
	bl_mobs_clear(&M);
	CHECK(bl_mobs_count(&M) == 0, "clear");

	/* night: they appear, 14 to 26 blocks away, on the ground, never more than the cap; by day nothing appears */
	{
		struct mrun r;
		mrun_init(&r);
		int bad = 0;
		for (int i = 0; i < 30 * 8 && !bl_mobs_count(&M); i++)
			bad += run_mobs(&M, w, pl, true, night, 1.0f / 30.0f, &r);
		CHECK(bl_mobs_count(&M) == 1 && r.born == 1, "a goblin appears in the first seconds of the night");
		const struct bl_mob *g = first_mob(&M);
		float d = g ? hypotf(g->x - pl[0], g->z - pl[2]) : 0;
		CHECK(g && d >= 13.9f && d <= 27.0f && fabsf(g->y - 20.0f) < 0.05f, "14 to 26 blocks from the player, on the floor (%.1f, y %.2f)", d, g ? g->y : 0);
		int maxn = 0;
		for (int i = 0; i < 30 * 120; i++) {
			bad += run_mobs(&M, w, (float[3]){ 64.5f, 20.0f, 64.5f }, true, night, 1.0f / 30.0f, NULL);
			int c = bl_mobs_count(&M);
			maxn = c > maxn ? c : maxn;
		}
		CHECK(bad == 0, "no goblin is ever in a block (%d steps)", bad);
		CHECK(maxn == 6 && M.spawned >= 6, "two minutes of night: up to the cap of six (%d at most, %d appeared)", maxn, M.spawned);
		M.cap = 2;
		bl_mobs_clear(&M);
		maxn = 0;
		for (int i = 0; i < 30 * 60; i++) {
			run_mobs(&M, w, pl, true, night, 1.0f / 30.0f, NULL);
			maxn = bl_mobs_count(&M) > maxn ? bl_mobs_count(&M) : maxn;
		}
		CHECK(maxn == 2, "a cap of two is two (%d)", maxn);
		M.cap = 6;
		bl_mobs_clear(&M);
		int born = 0;
		for (int i = 0; i < 30 * 120; i++) {
			struct bl_mobs_result R;
			bl_mobs_step(&M, w, pl, true, noon, 1.0f / 30.0f, &R);
			born += R.born;
		}
		CHECK(born == 0 && bl_mobs_count(&M) == 0, "by day nothing appears (%d)", born);
		M.enabled = false;
		bl_mobs_clear(&M);
		for (int i = 0; i < 30 * 60; i++)
			bl_mobs_step(&M, w, pl, true, night, 1.0f / 30.0f, NULL);
		CHECK(bl_mobs_count(&M) == 0, "switched off, none appear");
		M.enabled = true;
		bl_mobs_clear(&M);
		bl_mobs_add(&M, w, BL_MOB_GOBLIN, 60.5f, 20.0f, 60.5f);
		M.enabled = false;
		bl_mobs_step(&M, w, pl, true, night, 0.1f, NULL);
		CHECK(bl_mobs_count(&M) == 0, "and the ones there are go when it is switched off");
		M.enabled = true;
	}

	/* the hunt: it comes, stops next to him, hits every second, and the blow pushes him away from it */
	{
		bl_mobs_clear(&M);
		M.spawn_t = 1e9f;
		bl_mobs_add(&M, w, BL_MOB_GOBLIN, 64.5f, 20.0f, 54.5f);
		struct mrun r;
		mrun_init(&r);
		float kbz = 0;
		int bad = 0;
		for (int i = 0; i < 30 * 14; i++) {
			struct bl_mobs_result R;
			bl_mobs_step(&M, w, pl, true, night, 1.0f / 30.0f, &R);
			r.t += 1.0f / 30.0f;
			if (R.hits) {
				kbz = R.kbz;
				if (r.hits == 0)
					r.first_hit = r.t;
				else if (r.t - r.last_hit < r.min_gap)
					r.min_gap = r.t - r.last_hit;
				r.last_hit = r.t;
				r.hits += R.hits;
				r.damage += R.damage;
			}
			const struct bl_mob *g = first_mob(&M);
			if (g) {
				float d = hypotf(g->x - pl[0], g->z - pl[2]);
				r.min_dist = d < r.min_dist ? d : r.min_dist;
				bad += box_in_solid(w, g->x, g->y, g->z, BL_GOBLIN_HW, BL_GOBLIN_H);
			}
		}
		CHECK(r.first_hit > 2.5f && r.first_hit < 6.5f, "10 blocks away it is on him in a few seconds (the first blow at %.1f s)", r.first_hit);
		CHECK(r.hits >= 7 && r.hits <= 11 && r.min_gap >= 0.95f, "then a blow a second: %d blows, never closer than %.2f s", r.hits, r.min_gap);
		CHECK(fabsf(r.damage - 3.0f * (float)r.hits) < 0.01f, "three hit points a blow (%.1f)", r.damage);
		CHECK(kbz > 4.0f, "and the blow pushes him away from it, along +z (%.1f)", kbz);
		CHECK(r.min_dist > 0.8f && bad == 0, "it does not walk into him (%.2f m) or into a block", r.min_dist);
		/* a player who is not there is not hunted, a hunted one that goes far is not followed beyond 26 blocks */
		bl_mobs_clear(&M);
		bl_mobs_add(&M, w, BL_MOB_GOBLIN, 64.5f, 20.0f, 54.5f);
		struct mrun q;
		mrun_init(&q);
		run_mobs(&M, w, pl, false, night, 12.0f, &q);
		CHECK(q.hits == 0 && q.damage == 0.0f, "nobody to hit: no blows");
		bl_mobs_clear(&M);
		bl_mobs_add(&M, w, BL_MOB_GOBLIN, 64.5f, 20.0f, 30.0f);
		const struct bl_mob *g = first_mob(&M);
		float z0 = g->z;
		run_mobs(&M, w, pl, true, night, 1.5f, NULL);
		g = first_mob(&M);
		CHECK(g && fabsf(g->z - z0) < 3.0f, "a goblin 34 blocks away has not seen him");
	}

	/* a wall of two blocks stops it, a wall of one is climbed */
	{
		struct bl_world *w2 = bl_world_flat(128, 128, 64, 20);
		for (int x = 0; x < 128; x++)
			for (int y = 20; y < 22; y++)
				bl_set(w2, x, y, 58, BL_STONE);
		bl_mobs_clear(&M);
		M.spawn_t = 1e9f;
		bl_mobs_add(&M, w2, BL_MOB_GOBLIN, 64.5f, 20.0f, 50.5f);
		int bad = run_mobs(&M, w2, pl, true, night, 25.0f, NULL);
		const struct bl_mob *g = first_mob(&M);
		CHECK(g && g->z < 58.0f - BL_GOBLIN_HW + 0.05f && g->y < 20.2f && bad == 0, "a wall two blocks high stops it (z %.2f)", g ? g->z : 0);
		for (int x = 0; x < 128; x++)
			bl_set(w2, x, 21, 58, BL_AIR);
		struct mrun r;
		mrun_init(&r);
		bad = run_mobs(&M, w2, pl, true, night, 14.0f, &r);
		g = first_mob(&M);
		CHECK(g && g->z > 58.5f && r.hits >= 2 && bad == 0, "one block high it climbs and gets him (z %.2f, %d blows)", g ? g->z : 0, r.hits);
		bl_world_free(w2);
	}

	/* a hole: a chasing goblin falls into it and lands; a wandering one does not walk off an edge */
	{
		struct bl_world *w2 = bl_world_flat(128, 128, 64, 20);
		for (int z = 40; z < 80; z++)
			for (int x = 30; x < 98; x++)
				for (int y = 14; y < 20; y++)
					if (!(x >= 60 && x <= 68 && z >= 48 && z <= 56))        /* a pit six deep with an island of nine by nine in it */
						bl_set(w2, x, y, z, BL_AIR);
		bl_mobs_clear(&M);
		M.spawn_t = 1e9f;
		bl_mobs_add(&M, w2, BL_MOB_GOBLIN, 64.5f, 20.0f, 52.5f);            /* wandering: nobody to hunt */
		int bad = run_mobs(&M, w2, (float[3]){ 64.5f, 20.0f, 70.0f }, false, night, 40.0f, NULL);
		const struct bl_mob *g = first_mob(&M);
		CHECK(g && g->y > 19.99f && bad == 0, "a goblin that is only wandering does not walk off the edge (y %.2f)", g ? g->y : 0);
		bl_mobs_clear(&M);
		bl_mobs_add(&M, w2, BL_MOB_GOBLIN, 64.5f, 20.0f, 52.5f);
		bad = run_mobs(&M, w2, (float[3]){ 64.5f, 14.0f, 70.0f }, true, night, 5.0f, NULL);       /* he stands on the floor of the pit */
		g = first_mob(&M);
		CHECK(g && bad == 0 && g->y < 14.05f, "a chasing one goes after him into the pit and lands (y %.2f)", g ? g->y : 0);
		bl_world_free(w2);
	}

	/* water: it floats */
	{
		struct bl_world *w2 = bl_world_flat(128, 128, 64, 20);
		for (int z = 50; z <= 56; z++)
			for (int x = 60; x <= 68; x++)
				for (int y = 16; y < 20; y++)
					bl_set(w2, x, y, z, BL_WATER);
		bl_mobs_clear(&M);
		bl_mobs_add(&M, w2, BL_MOB_GOBLIN, 64.5f, 17.0f, 53.5f);
		int bad = run_mobs(&M, w2, (float[3]){ 64.5f, 20.0f, 90.0f }, false, night, 6.0f, NULL);
		const struct bl_mob *g = first_mob(&M);
		CHECK(g && bad == 0 && g->y > 18.2f, "dropped into water it floats up (y %.2f)", g ? g->y : 0);
		bl_world_free(w2);
	}

	/* the sun burns it in the open, not under a roof and not in the water */
	{
		struct bl_world *w2 = bl_world_flat(128, 128, 64, 20);
		for (int z = 50; z <= 56; z++)
			for (int x = 70; x <= 76; x++)
				bl_set(w2, x, 24, z, BL_STONE);                  /* a roof */
		for (int z = 50; z <= 56; z++)
			for (int x = 52; x <= 58; x++)
				for (int y = 16; y < 20; y++)
					bl_set(w2, x, y, z, BL_WATER);
		bl_mobs_clear(&M);
		M.spawn_t = 1e9f;
		bl_mobs_add(&M, w2, BL_MOB_GOBLIN, 64.5f, 20.0f, 58.5f);
		bl_mobs_add(&M, w2, BL_MOB_GOBLIN, 73.5f, 20.0f, 53.5f);
		bl_mobs_add(&M, w2, BL_MOB_GOBLIN, 55.5f, 17.0f, 53.5f);
		struct mrun r;
		mrun_init(&r);
		run_mobs(&M, w2, (float[3]){ 64.5f, 20.0f, 53.5f }, false, noon, 8.0f, &r);
		int alive_roof = 0, alive_open = 0, alive_water = 0;
		for (int i = 0; i < BL_MOBS_MAX; i++)
			if (M.mob[i].alive) {
				if (M.mob[i].x > 69.5f)
					alive_roof++;
				else if (M.mob[i].x < 59.5f)
					alive_water++;
				else
					alive_open++;
			}
		CHECK(alive_open == 0 && r.burned >= 1, "in the open the sun kills it in 8 s (burned %d)", r.burned);
		CHECK(alive_roof == 1 && alive_water == 1, "under a roof and in water it lives (%d, %d)", alive_roof, alive_water);
		bl_world_free(w2);
	}

	/* far away they are gone; by day the ones that are not near go */
	{
		bl_mobs_clear(&M);
		bl_mobs_add(&M, w, BL_MOB_GOBLIN, 5.5f, 20.0f, 5.5f);
		run_mobs(&M, w, pl, true, night, 1.0f, NULL);
		CHECK(bl_mobs_count(&M) == 0, "60 blocks away it is not kept");
	}

	/* the sword */
	{
		bl_mobs_clear(&M);
		M.spawn_t = 1e9f;
		bl_mobs_add(&M, w, BL_MOB_GOBLIN, 64.5f, 20.0f, 67.5f);
		float eye[3] = { 64.5f, 21.62f, 64.5f }, look[3] = { 0, 0, 1 }, t = 0;
		int id = bl_mobs_pick(&M, eye, look, BL_SWORD_REACH, &t);
		CHECK(id >= 0 && fabsf(t - (3.0f - BL_GOBLIN_HW - 0.05f)) < 0.01f, "the sword finds it 3 m ahead (%d at %.2f)", id, t);
		float side[3] = { 0.3f, 0, 1 };
		CHECK(bl_mobs_pick(&M, eye, side, BL_SWORD_REACH, &t) < 0, "a sword aimed a metre beside it misses");
		float high[3] = { 0, 0.5f, 1 };
		CHECK(bl_mobs_pick(&M, eye, high, BL_SWORD_REACH, &t) < 0, "and one aimed over its head");
		eye[2] = 60.0f;
		CHECK(bl_mobs_pick(&M, eye, look, BL_SWORD_REACH, &t) < 0, "6 m is beyond the reach");
		eye[2] = 64.5f;
		float zero[3] = { 0, 0, 0 };
		CHECK(bl_mobs_pick(&M, eye, zero, BL_SWORD_REACH, &t) < 0, "no direction, no hit");
		bl_mobs_add(&M, w, BL_MOB_GOBLIN, 64.5f, 20.0f, 66.0f);
		int near_id = bl_mobs_pick(&M, eye, look, BL_SWORD_REACH, &t);
		CHECK(near_id >= 0 && fabsf(M.mob[near_id].z - 66.0f) < 0.01f, "with two in a line the nearer is hit");
		/* the blows: three of four hit points take ten away, the third kills; the blow throws it back */
		bl_mobs_clear(&M);
		bl_mobs_add(&M, w, BL_MOB_GOBLIN, 64.5f, 20.0f, 67.5f);
		float z0 = M.mob[0].z;
		CHECK(!bl_mobs_hurt(&M, 0, BL_SWORD_DAMAGE, 0, 1) && M.mob[0].hp == 6.0f && M.mob[0].hurt > 0.3f, "the first blow: 6 left and the flash of red");
		run_mobs(&M, w, (float[3]){ 100, 20, 100 }, false, night, 0.5f, NULL);
		CHECK(M.mob[0].z > z0 + 0.5f, "thrown back along the blow (%.2f m)", M.mob[0].z - z0);
		CHECK(!bl_mobs_hurt(&M, 0, BL_SWORD_DAMAGE, 0, 1) && M.mob[0].hp == 2.0f, "the second: 2 left");
		CHECK(bl_mobs_hurt(&M, 0, BL_SWORD_DAMAGE, 0, 1) && !M.mob[0].alive && M.kills == 1, "the third kills it and is counted");
		CHECK(!bl_mobs_hurt(&M, 0, BL_SWORD_DAMAGE, 0, 1) && !bl_mobs_hurt(&M, -1, 1, 0, 1) && !bl_mobs_hurt(&M, 99, 1, 0, 1), "a dead one, and numbers that are no mobs, are not hurt");
	}

	/* the same world and seed give the same goblins */
	{
		struct bl_mobs A, B;
		bl_mobs_init(&A, 99);
		bl_mobs_init(&B, 99);
		float p2[3] = { 64.5f, 20.0f, 64.5f };
		run_mobs(&A, w, p2, true, night, 90.0f, NULL);
		run_mobs(&B, w, p2, true, night, 90.0f, NULL);
		bool same = true;
		for (int i = 0; i < BL_MOBS_MAX; i++)
			same &= A.mob[i].alive == B.mob[i].alive && A.mob[i].x == B.mob[i].x && A.mob[i].z == B.mob[i].z && A.mob[i].hp == B.mob[i].hp;
		CHECK(same && bl_mobs_count(&A) > 0, "the same seed gives the same goblins for 90 s (%d of them)", bl_mobs_count(&A));
	}

	/* stress: random players and random steps, odd times: never a goblin in a block, never a number that is not one */
	{
		struct bl_world *wt = bl_world_new(5, 128, 128, 64);
		struct bl_mobs S;
		bl_mobs_init(&S, 3);
		S.cap = 12;
		int bad = 0, nan = 0;
		float p3[3];
		bl_spawn(wt, p3);
		for (int i = 0; i < 40000; i++) {
			if (i % 500 == 0) {
				p3[0] = rndr(6, 120);
				p3[2] = rndr(6, 120);
				p3[1] = (float)bl_top(wt, (int)p3[0], (int)p3[2]) + 1.0f;
			}
			float dt = i % 97 == 0 ? rndr(0.0f, 0.6f) : i % 211 == 0 ? NAN : rndr(0.0f, 0.05f);
			float hour = (float)(i / 600 % 24);
			bl_mobs_step(&S, wt, p3, i % 1300 > 20, hour, dt, NULL);
			if (i % 40 == 0)
				for (int k = 0; k < BL_MOBS_MAX; k++)
					if (S.mob[k].alive) {
						bad += mob_in_solid(wt, S.mob[k].x, S.mob[k].y, S.mob[k].z, BL_GOBLIN_HW, BL_GOBLIN_H);
						nan += !isfinite(S.mob[k].x + S.mob[k].y + S.mob[k].z + S.mob[k].hp);
					}
			if (i % 1000 == 0) {                                        /* the world changes under them */
				int x = (int)rndr(10, 110), z = (int)rndr(10, 110), y = (int)p3[1] + (int)rndr(-1, 3);
				bl_set(wt, x, y, z, i % 2000 ? BL_STONE : BL_AIR);
			}
		}
		CHECK(bad == 0 && nan == 0, "40000 odd steps on real terrain: %d samples in a block, %d not numbers", bad, nan);
		CHECK(S.spawned > 3, "and they did appear (%d)", S.spawned);
		bl_world_free(wt);
	}
	bl_world_free(w);
}

static void test_mobs_render(void)
{
	enum { W = 160, H = 100 };
	static uint32_t a[W * H], b[W * H], c[W * H];
	struct bl_world *w = bl_world_flat(64, 64, 64, 20);
	struct bl_mobs M;
	bl_mobs_init(&M, 1);
	bl_mobs_add(&M, w, BL_MOB_GOBLIN, 32.5f, 20.0f, 36.5f);
	M.mob[0].yaw = 3.14159265f;                                       /* facing the camera */
	M.mob[0].ycos = cosf(M.mob[0].yaw);
	M.mob[0].ysin = sinf(M.mob[0].yaw);
	struct bl_camera cam = { 32.5f, 21.4f, 32.5f, 0.0f, -0.05f, 0.8f };
	struct bl_view v;
	bl_view_init(&v);
	v.time_h = 12.0f;
	bl_mobs_light(&M, w, v.time_h);
	bl_render(w, &cam, &v, a, W, H, W, 1);
	v.mobs = &M;
	bl_render(w, &cam, &v, b, W, H, W, 1);
	int changed = 0, green = 0, red = 0;
	for (int i = 0; i < W * H; i++)
		if (a[i] != b[i]) {
			changed++;
			int r = (int)((b[i] >> 16) & 255), g = (int)((b[i] >> 8) & 255), bl = (int)(b[i] & 255);
			green += g > r + 15 && g > bl + 40;
			red += r > 200 && g < 90 && bl < 90;
		}
	CHECK(changed > 600, "a goblin 4 m away is in the picture (%d pixels differ)", changed);
	CHECK(green > changed / 3, "and it is green: %d of %d changed pixels", green, changed);
	CHECK(red >= 2, "with red eyes (%d pixels)", red);
	bl_render(w, &cam, &v, c, W, H, W, 4);
	CHECK(memcmp(b, c, sizeof b) == 0, "four threads draw it as one thread does");
	v.mobs = NULL;
	bl_render(w, &cam, &v, c, W, H, W, 1);
	CHECK(memcmp(a, c, sizeof a) == 0, "without the mobs the picture is the one without them");

	/* the hook: the ray at its middle says goblin; a wall in front hides it, a wall behind does not */
	struct bl_view iv = v;
	iv.ids = true;
	iv.mobs = &M;
	uint32_t px;
	struct bl_camera one = { 32.5f, 21.0f, 32.5f, 0.0f, 0.0f, 0.8f };
	bl_render(w, &one, &iv, &px, 1, 1, 1, 1);
	CHECK(px == 0xff0000fcu, "the ray at it meets a goblin (%08x)", px);
	for (int x = 30; x <= 35; x++)
		for (int y = 20; y < 24; y++)
			bl_set(w, x, y, 34, BL_STONE);
	bl_render(w, &one, &iv, &px, 1, 1, 1, 1);
	CHECK(px != 0xff0000fcu && (px & 0xff) == BL_NZ, "a wall in front of it hides it (%08x)", px);
	bl_render(w, &cam, &(struct bl_view){ .time_h = 12.0f, .dist = 96.0f, .mobs = &M }, c, W, H, W, 1);
	int hidden = 0;
	for (int i = 0; i < W * H; i++)
		hidden += c[i] != a[i];
	bl_render(w, &cam, &v, b, W, H, W, 1);
	for (int x = 30; x <= 35; x++)
		for (int y = 20; y < 24; y++)
			bl_set(w, x, y, 34, BL_AIR);
	for (int x = 28; x <= 38; x++)
		for (int y = 20; y < 26; y++)
			bl_set(w, x, y, 38, BL_STONE);
	bl_render(w, &one, &iv, &px, 1, 1, 1, 1);
	CHECK(px == 0xff0000fcu, "a wall behind it leaves it in front of the wall (%08x)", px);
	for (int x = 28; x <= 38; x++)
		for (int y = 20; y < 26; y++)
			bl_set(w, x, y, 38, BL_AIR);

	/* night: darker, the eyes still shine; torch light brightens it; a hurt one is redder */
	bl_mobs_light(&M, w, 12.0f);
	float day_lit = M.mob[0].lit[1];
	bl_mobs_light(&M, w, 0.0f);
	float night_lit = M.mob[0].lit[1];
	CHECK(night_lit < day_lit * 0.6f && night_lit > 0.1f, "at night it is lit less than by day (%.2f against %.2f) but not black", night_lit, day_lit);
	bl_set(w, 33, 20, 35, BL_TORCH);
	bl_mobs_light(&M, w, 0.0f);
	CHECK(M.mob[0].lit[0] > night_lit * 1.4f && M.mob[0].lit[0] > M.mob[0].lit[2], "a torch beside it lights it, warm (%.2f %.2f)", M.mob[0].lit[0], M.mob[0].lit[2]);
	bl_set(w, 33, 20, 35, BL_AIR);
	v.mobs = &M;
	v.time_h = 12.0f;
	bl_mobs_light(&M, w, 12.0f);
	bl_render(w, &cam, &v, a, W, H, W, 1);
	M.mob[0].hurt = 0.35f;
	bl_render(w, &cam, &v, b, W, H, W, 1);
	long ra = 0, rb = 0, ga = 0, gb = 0;
	for (int i = 0; i < W * H; i++) {
		ra += (a[i] >> 16) & 255;
		rb += (b[i] >> 16) & 255;
		ga += (a[i] >> 8) & 255;
		gb += (b[i] >> 8) & 255;
	}
	CHECK(rb > ra && gb < ga, "a goblin that was just hit is redder (red %ld -> %ld, green %ld -> %ld)", ra, rb, ga, gb);
	M.mob[0].hurt = 0;

	/* the walk: the legs and the arms move with the phase (the picture changes) */
	M.mob[0].speed = 2.7f;
	M.mob[0].phase = 0.25f;
	bl_render(w, &cam, &v, a, W, H, W, 1);
	M.mob[0].phase = 0.75f;
	bl_render(w, &cam, &v, b, W, H, W, 1);
	int moved = 0;
	for (int i = 0; i < W * H; i++)
		moved += a[i] != b[i];
	CHECK(moved > 40, "half a stride later the limbs are elsewhere (%d pixels)", moved);
	M.mob[0].speed = 0;

	/* in the water a goblin is seen through it: bluer and dimmer than the same goblin in the air */
	{
		struct bl_world *wp = bl_world_flat(64, 64, 64, 20);
		for (int z = 30; z <= 40; z++)
			for (int x = 28; x <= 38; x++)
				for (int y = 17; y < 20; y++)
					bl_set(wp, x, y, z, BL_WATER);
		struct bl_mobs Mw;
		bl_mobs_init(&Mw, 1);
		bl_mobs_add(&Mw, wp, BL_MOB_GOBLIN, 32.5f, 17.0f, 35.5f);
		struct bl_camera above = { 32.5f, 22.0f, 31.0f, 0.0f, -0.35f, 0.8f };
		struct bl_view vw;
		bl_view_init(&vw);
		bl_mobs_light(&Mw, wp, vw.time_h);
		vw.mobs = &Mw;
		bl_render(wp, &above, &vw, a, W, H, W, 1);
		struct bl_world *wd = bl_world_flat(64, 64, 64, 20);
		struct bl_mobs Md;
		bl_mobs_init(&Md, 1);
		bl_mobs_add(&Md, wd, BL_MOB_GOBLIN, 32.5f, 17.0f, 35.5f);
		for (int z = 30; z <= 40; z++)
			for (int x = 28; x <= 38; x++)
				for (int y = 17; y < 20; y++)
					bl_set(wd, x, y, z, BL_AIR);
		bl_mobs_light(&Md, wd, vw.time_h);
		vw.mobs = &Md;
		bl_render(wd, &above, &vw, b, W, H, W, 1);
		long sr = 0, sb = 0, dr = 0, db = 0;
		for (int i = 0; i < W * H; i++) {
			sr += (a[i] >> 16) & 255;
			sb += a[i] & 255;
			dr += (b[i] >> 16) & 255;
			db += b[i] & 255;
		}
		CHECK((double)sb / (double)(sr + 1) > (double)db / (double)(dr + 1) * 1.3 || sr < dr, "seen through the water it is not the colour it is in the air (blue/red %.2f against %.2f)",
		      (double)sb / (double)(sr + 1), (double)db / (double)(dr + 1));
		bl_world_free(wp);
		bl_world_free(wd);
	}

	/* any camera, mobs everywhere: no crash, opaque pictures */
	bl_mobs_clear(&M);
	for (int i = 0; i < 8; i++)
		bl_mobs_add(&M, w, BL_MOB_GOBLIN, rndr(10, 50), 20.0f, rndr(10, 50));
	bl_mobs_light(&M, w, 20.0f);
	static uint32_t small[40 * 24];
	int odd = 0;
	for (int i = 0; i < 800; i++) {
		struct bl_camera cm = { rndr(5, 58), rndr(18, 40), rndr(5, 58), rndr(-10, 10), rndr(-1.6f, 1.6f), rndr(0.3f, 2.5f) };
		struct bl_view vv = v;
		vv.dist = rndr(16, 200);
		vv.time_h = rndr(0, 24);
		vv.ids = i % 5 == 0;
		bl_render(w, &cm, &vv, small, 40, 24, 40, 1 + (int)(rnd() % 3));
		for (int k = 0; k < 40 * 24; k++)
			odd += !vv.ids && (small[k] >> 24) != 255;
	}
	CHECK(odd == 0, "%d pixels that are not opaque", odd);
	(void)hidden;
	bl_world_free(w);
}

/* ---- survival: hardness, drops, the keep -------------------------------------------------------------------------------- */

static void test_keep(void)
{
	/* what breaking takes and leaves */
	int bad_h = 0, bad_d = 0;
	for (int b = 1; b < BL_COUNT; b++) {
		float h = bl_hardness(b);
		int d = bl_drop(b);
		if (bl_breakable(b)) {
			bad_h += !(h > 0.02f && h <= 3.0f);
			bad_d += !(d >= 0 && d < BL_COUNT && (d == BL_AIR || (bl_breakable(d) && d != BL_WATER)));
		} else {
			bad_h += h != 0.0f;
			bad_d += d != 0;
		}
	}
	CHECK(bad_h == 0 && bad_d == 0, "every block that can be broken takes 0.02 to 3 s and leaves a block that can be had, what cannot be broken takes nothing and leaves nothing (%d, %d)", bad_h, bad_d);
	CHECK(bl_drop(BL_STONE) == BL_COBBLE && bl_drop(BL_GRASS) == BL_DIRT && bl_drop(BL_LEAVES) == BL_AIR && bl_drop(BL_GLASS) == BL_AIR && bl_drop(BL_LOG) == BL_LOG &&
	      bl_drop(BL_TORCH) == BL_TORCH, "stone leaves cobble, grass dirt, leaves and glass nothing, the rest themselves");
	CHECK(bl_hardness(BL_LEAVES) < bl_hardness(BL_DIRT) && bl_hardness(BL_DIRT) < bl_hardness(BL_LOG) && bl_hardness(BL_LOG) < bl_hardness(BL_STONE) && bl_hardness(BL_STONE) < bl_hardness(BL_BRICK),
	      "leaves are quicker than dirt, dirt than wood, wood than stone, stone than brick");
	CHECK(!bl_breakable(BL_AIR) && !bl_breakable(BL_WATER) && !bl_breakable(BL_FLOW3) && !bl_breakable(BL_BEDROCK) && !bl_breakable(BL_KEEP) && bl_breakable(BL_STONE), "what a hand can break");

	/* the keep: a solid, opaque block that can be aimed at, is not broken by a hand and cannot be put by one */
	struct bl_world *w = bl_world_flat(64, 64, 64, 20);
	for (int y = 20; y < 23; y++)
		bl_set(w, 32, y, 32, BL_KEEP);
	CHECK(bl_get(w, 32, 21, 32) == BL_KEEP && bl_solid(BL_KEEP) && bl_opaque(BL_KEEP) && bl_targetable(BL_KEEP) && !strcmp(bl_block_name(BL_KEEP), "keep"), "the keep is a block");
	float o[3] = { 32.5f, 21.5f, 28.5f }, d[3] = { 0, 0, 1 };
	struct bl_hit h;
	CHECK(bl_pick(w, o, d, 6.0f, &h) && h.x == 32 && h.y == 21 && h.z == 32 && h.face == BL_NZ, "it is aimed at");
	CHECK(!bl_break(w, &h) && bl_get(w, 32, 21, 32) == BL_KEEP, "a hand does not break it");
	struct bl_hit ph = h;
	ph.px = 32;
	ph.py = 21;
	ph.pz = 31;
	CHECK(!bl_place(w, &ph, BL_KEEP, NULL) && bl_get(w, 32, 21, 31) == BL_AIR, "and does not put one");
	CHECK(bl_place(w, &ph, BL_STONE, NULL) && bl_break(w, &(struct bl_hit){ .hit = true, .x = 32, .y = 21, .z = 31 }), "(a stone beside it goes and comes as usual)");
	for (int f = 0; f < BL_FACE_KINDS; f++) {
		const uint32_t *t = bl_texture(BL_KEEP, f), *br = bl_texture(BL_BRICK, f);
		int opaque = 0, same = 0;
		for (int i = 0; i < 256; i++) {
			opaque += (t[i] >> 24) == 255;
			same += t[i] == br[i];
		}
		CHECK(opaque == 256 && same < 100, "the keep's picture (%d) is opaque and not the brick's (%d of 256 the same)", f, same);
	}
	const uint32_t *side = bl_texture(BL_KEEPFLAG, BL_FACE_SIDE);
	CHECK(!bl_breakable(BL_KEEPFLAG) && !bl_breakable(BL_REDFLAG) && bl_own_keep(BL_KEEPFLAG) && bl_red_keep(BL_REDFLAG), "the shields are parts of the keeps");
	int red = 0, gold = 0;
	for (int i = 0; i < 256; i++) {
		int r = (int)((side[i] >> 16) & 255), g = (int)((side[i] >> 8) & 255), b = (int)(side[i] & 255);
		red += r > 140 && g < 70 && b < 70;
		gold += r > 200 && g > 150 && b < 100;
	}
	CHECK(red > 25 && gold > 12, "a red shield with a gold cross on its sides (%d red, %d gold)", red, gold);
	/* it is saved like any edit */
	char path[300];
	snprintf(path, sizeof path, "/tmp/mellagui-keep-test-%d.bl", (int)getpid());
	CHECK(bl_save(w, path) == 0, "saved");
	const char *err = NULL;
	struct bl_world *lw = bl_load(path, &err);
	CHECK(lw && bl_get(lw, 32, 22, 32) == BL_KEEP, "and it comes back");
	bl_world_free(lw);
	unlink(path);

	/* a crack shows on the block that is being broken: the same view with a crack is darker over the aimed block and the same elsewhere */
	{
		enum { W = 80, H = 60 };
		static uint32_t a[W * H], b[W * H];
		struct bl_camera cam = { 32.5f, 21.6f, 29.0f, 0.0f, -0.02f, 0.8f };
		struct bl_view v;
		bl_view_init(&v);
		v.target = true;
		v.tx = 32;
		v.ty = 21;
		v.tz = 32;
		bl_render(w, &cam, &v, a, W, H, W, 1);
		v.crack = 0.9f;
		bl_render(w, &cam, &v, b, W, H, W, 1);
		long sa = 0, sb = 0;
		int diff = 0;
		for (int i = 0; i < W * H; i++) {
			if (a[i] != b[i])
				diff++;
			sa += ((a[i] >> 16) & 255) + ((a[i] >> 8) & 255) + (a[i] & 255);
			sb += ((b[i] >> 16) & 255) + ((b[i] >> 8) & 255) + (b[i] & 255);
		}
		CHECK(diff > 60 && sb < sa, "a block that is nearly broken shows cracks (%d pixels darker)", diff);
		v.crack = 0.0f;
		bl_render(w, &cam, &v, b, W, H, W, 1);
		CHECK(memcmp(a, b, sizeof a) == 0, "no crack, no change");
	}

	/* the goblins and the keep */
	struct bl_mobs M;
	const float far_pl[3] = { 64.5f, 20.0f, 100.0f };            /* 36 blocks from the goblins: not seen by them, not far enough for them to be dropped */
	struct bl_world *wk = bl_world_flat(128, 128, 64, 20);
	for (int y = 20; y < 23; y++)
		bl_set(wk, 80, y, 64, BL_KEEP);
	bl_mobs_init(&M, 3);
	bl_mobs_set_keep(&M, true, 80.5f, 20.0f, 64.5f);
	M.spawn_t = 1e9f;
	{
		bl_mobs_add(&M, wk, BL_MOB_GOBLIN, 58.5f, 20.0f, 64.5f);
		float dmg = 0, last = 0, t = 0, first = 0, mingap = 99.0f;
		int hits = 0, bad = 0;
		for (int i = 0; i < 30 * 40; i++) {
			struct bl_mobs_result R;
			bl_mobs_step(&M, wk, far_pl, true, 0.0f, 1.0f / 30.0f, &R);
			t += 1.0f / 30.0f;
			dmg += R.keep_damage;
			if (R.keep_hits) {
				if (!hits)
					first = t;
				else
					mingap = t - last < mingap ? t - last : mingap;
				last = t;
				hits += R.keep_hits;
			}
			if (R.damage > 0)
				bad++;
			const struct bl_mob *g = first_mob(&M);
			if (g)
				bad += box_in_solid(wk, g->x, g->y, g->z, BL_GOBLIN_HW, BL_GOBLIN_H);
		}
		CHECK(first > 4.0f && first < 14.0f, "a goblin 22 blocks from the keep and far from the player is at the keep in a few seconds (the first blow at %.1f s)", first);
		CHECK(hits >= 24 && hits <= 38 && mingap >= 0.95f && fabsf(dmg - 2.0f * (float)hits) < 0.01f, "then a blow a second on it, two hit points each (%d blows, %.0f hit points, never closer than %.2f s)", hits, dmg, mingap);
		CHECK(bad == 0, "and the player, far away, is not touched (%d)", bad);
		const struct bl_mob *g = first_mob(&M);
		const float kat[3] = { 80.5f, 20.0f, 64.5f }, ke = g ? bl_keep_dist(kat, g->x, g->z) : -1.0f;
		CHECK(ke > 0.1f && ke < 1.0f, "it stands at the foot of its wall (%.2f m)", ke);
	}
	{                                                                       /* two goblins hit twice as much */
		bl_mobs_clear(&M);
		bl_mobs_add(&M, wk, BL_MOB_GOBLIN, 78.5f, 20.0f, 64.5f);
		bl_mobs_add(&M, wk, BL_MOB_GOBLIN, 82.5f, 20.0f, 64.5f);
		float dmg = 0;
		for (int i = 0; i < 30 * 10; i++) {
			struct bl_mobs_result R;
			bl_mobs_step(&M, wk, far_pl, true, 0.0f, 1.0f / 30.0f, &R);
			dmg += R.keep_damage;
		}
		CHECK(dmg >= 24.0f && dmg <= 40.0f, "two goblins at the keep: about four hit points a second (%.0f in 10 s)", dmg);
	}
	{                                                                       /* the player near them: they go for him, not for the keep */
		bl_mobs_clear(&M);
		bl_mobs_add(&M, wk, BL_MOB_GOBLIN, 78.5f, 20.0f, 64.5f);
		float near_pl[3] = { 72.5f, 20.0f, 64.5f }, kd = 0, pd = 0;
		for (int i = 0; i < 30 * 8; i++) {
			struct bl_mobs_result R;
			bl_mobs_step(&M, wk, near_pl, true, 0.0f, 1.0f / 30.0f, &R);
			kd += R.keep_damage;
			pd += R.damage;
		}
		CHECK(pd >= 9.0f && kd <= 2.0f, "a goblin that has the player within 8 blocks hits him and not the keep (%.0f to him, %.0f to the keep)", pd, kd);
	}
	{                                                                       /* no keep: they do not go anywhere in particular; a far keep is not seen */
		bl_mobs_clear(&M);
		bl_mobs_set_keep(&M, false, 80.5f, 20.0f, 64.5f);
		bl_mobs_add(&M, wk, BL_MOB_GOBLIN, 79.0f, 20.0f, 64.5f);
		float dmg = 0;
		for (int i = 0; i < 30 * 15; i++) {
			struct bl_mobs_result R;
			bl_mobs_step(&M, wk, far_pl, true, 0.0f, 1.0f / 30.0f, &R);
			dmg += R.keep_damage;
		}
		CHECK(dmg == 0.0f, "with no keep there is nothing to hit");
		bl_mobs_set_keep(&M, true, 80.5f, 20.0f, 64.5f);
		bl_mobs_clear(&M);
		bl_mobs_add(&M, wk, BL_MOB_GOBLIN, 20.5f, 20.0f, 100.5f);
		for (int i = 0; i < 30 * 15; i++) {
			struct bl_mobs_result R;
			bl_mobs_step(&M, wk, far_pl, true, 0.0f, 1.0f / 30.0f, &R);
			dmg += R.keep_damage;
		}
		CHECK(dmg == 0.0f && first_mob(&M) && first_mob(&M)->x < 40.0f, "a keep 70 blocks away is not seen");
	}
	{                                                                       /* a wall round the keep (three high, a ring two blocks out) keeps them off it */
		for (int dx = -6; dx <= 6; dx++)
			for (int dz = -6; dz <= 6; dz++)
				if (abs(dx) == 6 || abs(dz) == 6)
					for (int y = 20; y < 23; y++)
						bl_set(wk, 80 + dx, y, 64 + dz, BL_STONE);
		bl_mobs_clear(&M);
		bl_mobs_add(&M, wk, BL_MOB_GOBLIN, 66.5f, 20.0f, 64.5f);
		bl_mobs_add(&M, wk, BL_MOB_GOBLIN, 94.5f, 20.0f, 70.5f);
		float dmg = 0;
		int bad = 0;
		for (int i = 0; i < 30 * 30; i++) {
			struct bl_mobs_result R;
			bl_mobs_step(&M, wk, far_pl, true, 0.0f, 1.0f / 30.0f, &R);
			dmg += R.keep_damage;
			for (int k = 0; k < BL_MOBS_MAX; k++)
				if (M.mob[k].alive)
					bad += box_in_solid(wk, M.mob[k].x, M.mob[k].y, M.mob[k].z, BL_GOBLIN_HW, BL_GOBLIN_H);
		}
		CHECK(dmg == 0.0f && bad == 0, "a wall of stone round the keep keeps goblins off it for 30 s (%.0f damage)", dmg);
	}
	bl_world_free(wk);
	bl_world_free(w);
}

/* ---- workers ----------------------------------------------------------------------------------------------------------------- */

/* step the mobs, and do what a worker asks: take away the blocks it has finished. Returns how many blocks went. */
static int work_steps(struct bl_mobs *M, struct bl_world *w, const float pl[3], float secs, float hour, int *in_block, float *first_break_t)
{
	int broke = 0;
	float t = 0.0f;
	int n = (int)(secs * 30.0f);
	for (int i = 0; i < n; i++) {
		struct bl_mobs_result R;
		bl_mobs_step(M, w, pl, true, hour, 1.0f / 30.0f, &R);
		t += 1.0f / 30.0f;
		for (int k = 0; k < R.nbreak; k++) {
			bl_set(w, R.brk[k].x, R.brk[k].y, R.brk[k].z, BL_AIR);
			if (!broke && first_break_t)
				*first_break_t = t;
			broke++;
		}
		if (in_block)
			for (int k = 0; k < BL_MOBS_MAX; k++)
				if (M->mob[k].alive)
					*in_block += box_in_solid(w, M->mob[k].x, M->mob[k].y, M->mob[k].z, BL_GOBLIN_HW, bl_mob_h(M->mob[k].kind));
	}
	return broke;
}

static void test_soldiers(void)
{
	const float far_pl[3] = { 50.5f, 20.0f, 95.0f };                /* (near enough to keep the goblins from despawning; the soldiers' tests say nobody is alive to hunt) */
	const float dusk = 17.9f;                                   /* (neither spawning nor burning) */
	struct bl_world *w = bl_world_flat(128, 128, 64, 20);
	struct bl_mobs M;
	bl_mobs_init(&M, 21);
	M.spawn_t = 1e9f;

	CHECK(bl_mobs_add(&M, w, BL_MOB_SOLDIER, 50.5f, 20.0f, 50.5f) && M.mob[0].kind == BL_MOB_SOLDIER && M.mob[0].hp == BL_SOLDIER_HP && M.mob[0].tx == 50.5f, "a soldier, with fourteen hit points and a post");
	for (int i = 1; i < BL_SOLDIER_MAX; i++)
		bl_mobs_add(&M, w, BL_MOB_SOLDIER, 50.5f + 1.5f * (float)i, 20.0f, 50.5f);
	CHECK(bl_mobs_count_kind(&M, BL_MOB_SOLDIER) == BL_SOLDIER_MAX && !bl_mobs_add(&M, w, BL_MOB_SOLDIER, 50.5f, 20.0f, 54.0f), "eight at most");
	CHECK(bl_mobs_add(&M, w, BL_MOB_WORKER, 50.5f, 20.0f, 55.0f) && bl_mobs_add(&M, w, BL_MOB_GOBLIN, 90.5f, 20.0f, 90.5f), "workers and goblins go in beside them");
	CHECK(bl_mobs_hit_cell(&M, 50, 21, 50) && !bl_mobs_hit_cell(&M, 50, 22, 50), "a block cannot be put where a soldier stands (1.65 m)");
	float eye[3] = { 50.5f, 21.0f, 47.0f }, look[3] = { 0, 0, 1 }, t = 0;
	CHECK(bl_mobs_pick(&M, eye, look, BL_SWORD_REACH, &t) < 0 && bl_units_pick(&M, eye, look, 20.0f, &t) == 0, "the sword goes through a soldier, the pointer finds him");
	bl_mobs_clear(&M);

	/* it takes a goblin that comes near and stands guard again afterwards */
	{
		bl_mobs_add(&M, w, BL_MOB_SOLDIER, 50.5f, 20.0f, 50.5f);
		bl_mobs_add(&M, w, BL_MOB_GOBLIN, 58.5f, 20.0f, 50.5f);
		int slain = 0, in_block = 0;
		float dead_t = 0.0f, t0 = 0.0f;
		for (int i = 0; i < 30 * 14; i++) {
			struct bl_mobs_result R;
			bl_mobs_step(&M, w, far_pl, false, dusk, 1.0f / 30.0f, &R);
			t0 += 1.0f / 30.0f;
			slain += R.slain;
			if (!dead_t && slain)
				dead_t = t0;
			for (int k = 0; k < BL_MOBS_MAX; k++)
				if (M.mob[k].alive)
					in_block += box_in_solid(w, M.mob[k].x, M.mob[k].y, M.mob[k].z, BL_GOBLIN_HW, bl_mob_h(M.mob[k].kind));
		}
		CHECK(slain == 1 && M.kills == 1 && bl_mobs_count_kind(&M, BL_MOB_GOBLIN) == 0, "a goblin 8 m away is killed by the soldier (%d)", slain);
		CHECK(dead_t > 2.0f && dead_t < 10.0f, "in a few seconds - three blows and the walk (%.1f s)", dead_t);
		CHECK(M.mob[0].alive && M.mob[0].hp > 0 && hypotf(M.mob[0].x - 50.5f, M.mob[0].z - 50.5f) < 1.6f && in_block == 0, "and it is back at its post (%.1f m), alive with %.0f hit points, never in a block", hypotf(M.mob[0].x - 50.5f, M.mob[0].z - 50.5f), M.mob[0].hp);
	}
	bl_mobs_clear(&M);

	/* a goblin far from the post (past the leash) is left alone */
	{
		bl_mobs_add(&M, w, BL_MOB_SOLDIER, 50.5f, 20.0f, 50.5f);
		bl_mobs_add(&M, w, BL_MOB_GOBLIN, 50.5f, 20.0f, 76.5f);                  /* (26 m: past what a goblin sees of a soldier, too) */
		M.mob[1].wander_t = 1e9f;
		M.mob[1].widle = true;
		int slain = 0;
		for (int i = 0; i < 30 * 8; i++) {
			struct bl_mobs_result R;
			bl_mobs_step(&M, w, far_pl, false, dusk, 1.0f / 30.0f, &R);
			slain += R.slain;
		}
		CHECK(slain == 0 && M.mob[1].alive && hypotf(M.mob[0].x - 50.5f, M.mob[0].z - 50.5f) < 1.5f, "a goblin 26 m away is not seen (the soldier stays at its post)");
	}
	bl_mobs_clear(&M);

	/* a post: ordered somewhere it walks there at about 3 m/s and stands there; stop makes the place it is at the post */
	{
		bl_mobs_add(&M, w, BL_MOB_SOLDIER, 50.5f, 20.0f, 50.5f);
		bl_mobs_order_move(&M, 0, 62.5f, 56.5f);
		CHECK(M.mob[0].order == BL_ORDER_IDLE && M.mob[0].tx == 62.5f && M.mob[0].tz == 56.5f, "the order gives it a post");
		float tt = 0, arrived = 0, vmax = 0;
		for (int i = 0; i < 30 * 12; i++) {
			float ox = M.mob[0].x, oz = M.mob[0].z;
			bl_mobs_step(&M, w, far_pl, false, dusk, 1.0f / 30.0f, NULL);
			tt += 1.0f / 30.0f;
			float v = hypotf(M.mob[0].x - ox, M.mob[0].z - oz) * 30.0f;
			vmax = v > vmax ? v : vmax;
			if (!arrived && hypotf(M.mob[0].x - 62.5f, M.mob[0].z - 56.5f) < 1.2f)
				arrived = tt;
		}
		CHECK(arrived > 2.5f && arrived < 7.0f && vmax > 2.4f && vmax < 3.3f, "it walks 13 m there in %.1f s at up to %.2f m/s", arrived, vmax);
		bl_mobs_order_stop(&M, 0);
		CHECK(fabsf(M.mob[0].tx - M.mob[0].x) < 1e-3f, "stop makes where it stands the post");
		bl_mobs_order_gather(&M, w, 0, 60, 19, 60);
		CHECK(M.mob[0].order == BL_ORDER_IDLE, "a soldier does not gather");
	}
	bl_mobs_clear(&M);

	/* goblins go for a soldier like for a worker, and four of them kill one */
	{
		bl_mobs_add(&M, w, BL_MOB_SOLDIER, 50.5f, 20.0f, 50.5f);
		for (int i = 0; i < 4; i++)
			bl_mobs_add(&M, w, BL_MOB_GOBLIN, 50.5f + (i < 2 ? -4.0f : 4.0f), 20.0f, 50.5f + (i & 1 ? 3.0f : -3.0f));
		int lost = 0, slain = 0;
		for (int i = 0; i < 30 * 20; i++) {
			struct bl_mobs_result R;
			bl_mobs_step(&M, w, far_pl, false, dusk, 1.0f / 30.0f, &R);
			lost += R.soldiers_lost;
			slain += R.slain;
		}
		CHECK(lost == 1 && !M.mob[0].alive, "four goblins kill the soldier (%d lost)", lost);
		(void)slain;
	}
	bl_mobs_clear(&M);

	/* two soldiers hold off two goblins */
	{
		bl_mobs_add(&M, w, BL_MOB_SOLDIER, 50.5f, 20.0f, 50.5f);
		bl_mobs_add(&M, w, BL_MOB_SOLDIER, 52.0f, 20.0f, 50.5f);
		bl_mobs_add(&M, w, BL_MOB_GOBLIN, 44.5f, 20.0f, 50.5f);
		bl_mobs_add(&M, w, BL_MOB_GOBLIN, 44.5f, 20.0f, 54.5f);
		int lost = 0;
		for (int i = 0; i < 30 * 20; i++) {
			struct bl_mobs_result R;
			bl_mobs_step(&M, w, far_pl, false, dusk, 1.0f / 30.0f, &R);
			lost += R.soldiers_lost;
		}
		CHECK(bl_mobs_count_kind(&M, BL_MOB_GOBLIN) == 0 && lost == 0, "two soldiers kill two goblins and lose nobody (%d lost, %d goblins left)", lost, bl_mobs_count_kind(&M, BL_MOB_GOBLIN));
	}
	bl_mobs_clear(&M);

	/* the picture: a red tunic, lighter when chosen, a steel helmet */
	{
		bl_mobs_add(&M, w, BL_MOB_SOLDIER, 50.5f, 20.0f, 50.5f);
		struct bl_mob *b = &M.mob[0];
		b->yaw = 0.0f;
		b->ycos = 1.0f;
		b->ysin = 0.0f;
		bl_mobs_light(&M, w, 12.0f);
		float o[3] = { 50.5f, 20.0f + 0.8f * 1.15f, 47.0f }, d[3] = { 0, 0, 1 };
		struct bl_mob_hit hit;
		CHECK(bl_mobs_ray(&M, o, d, 20.0f, &hit) && hit.id == 0 && hit.rgb[0] > 1.8f * hit.rgb[2], "a soldier's tunic is red (%.2f %.2f %.2f)", hit.rgb[0], hit.rgb[1], hit.rgb[2]);
		float o2[3] = { 50.5f, 20.0f + 1.4f * 1.15f, 47.0f };
		struct bl_mob_hit h2;
		CHECK(bl_mobs_ray(&M, o2, d, 20.0f, &h2) && h2.rgb[0] > 0.2f && fabsf(h2.rgb[0] - h2.rgb[2]) < 0.15f, "the helmet is steel grey (%.2f %.2f %.2f)", h2.rgb[0], h2.rgb[1], h2.rgb[2]);
		b->selected = true;
		struct bl_mob_hit h3;
		CHECK(bl_mobs_ray(&M, o, d, 20.0f, &h3) && h3.rgb[1] > hit.rgb[1], "chosen, it is lighter");
	}
	bl_mobs_clear(&M);

	/* a wave: so many goblins, so many at a time, out of the ground round the keep and not round the player */
	{
		const float keep[3] = { 64.5f, 20.0f, 64.5f }, pl[3] = { 8.5f, 20.0f, 8.5f };
		bl_mobs_init(&M, 5);
		bl_mobs_set_keep(&M, true, keep[0], keep[1], keep[2]);
		bl_mobs_start_wave(&M, 7, 3, 0);
		CHECK(M.wave_on && M.wave_left == 7 && M.wave_size == 7 && M.wave_cap == 3 && bl_mobs_wave_remaining(&M) == 7, "a wave of seven, three at a time");
		float dmin = 1e9f, dmax = 0.0f;
		int maxalive = 0, born = 0, kept_hits = 0, in_block = 0;
		bool far_from_pl = true;
		for (int round = 0; round < 3; round++) {
			if (round > 0) {                                    /* two die: two more come out, and no more than three are ever there */
				int killed = 0;
				for (int k = 0; k < BL_MOBS_MAX && killed < 2; k++)
					if (M.mob[k].alive && M.mob[k].kind == BL_MOB_GOBLIN) {
						M.mob[k].alive = false;
						killed++;
					}
			}
			for (int i = 0; i < 30 * 25; i++) {
				bool had[BL_MOBS_MAX];
				for (int k = 0; k < BL_MOBS_MAX; k++)
					had[k] = M.mob[k].alive;
				struct bl_mobs_result R;
				bl_mobs_step(&M, w, pl, true, 0.0f, 1.0f / 30.0f, &R);
				born += R.born;
				kept_hits += R.keep_hits;
				for (int k = 0; k < BL_MOBS_MAX; k++) {
					const struct bl_mob *g = &M.mob[k];
					if (g->alive && !had[k]) {
						float dk = hypotf(g->x - keep[0], g->z - keep[2]);
						dmin = dk < dmin ? dk : dmin;
						dmax = dk > dmax ? dk : dmax;
						if (hypotf(g->x - pl[0], g->z - pl[2]) < 30.0f)
							far_from_pl = false;
					}
					if (g->alive)
						in_block += box_in_solid(w, g->x, g->y, g->z, BL_GOBLIN_HW, bl_mob_h(g->kind));
				}
				int n = bl_mobs_count_kind(&M, BL_MOB_GOBLIN);
				maxalive = n > maxalive ? n : maxalive;
			}
			if (round == 0)
				CHECK(born == 3 && M.wave_left == 4 && bl_mobs_wave_remaining(&M) == 7, "three came out and no more while they live (%d born, %d left)", born, M.wave_left);
		}
		CHECK(born == 7 && M.wave_left == 0, "two more after two died, two more after that (%d born, %d to come)", born, M.wave_left);
		CHECK(maxalive == 3, "never more than three at a time (%d)", maxalive);
		CHECK(dmin > 19.0f && dmax < 33.0f, "each comes 20 to 32 m from the keep (%.1f .. %.1f)", dmin, dmax);
		CHECK(far_from_pl && in_block == 0, "nowhere near the player (%d) and never in a block (%d)", far_from_pl, in_block);
		CHECK(kept_hits > 0, "and they go to the keep and hit it (%d blows)", kept_hits);
	}
	/* by day a wave does not come; a wave over leaves nobody to spawn; the goblins of one are kept alive far from the player */
	{
		const float keep[3] = { 64.5f, 20.0f, 64.5f }, pl[3] = { 8.5f, 20.0f, 8.5f };
		bl_mobs_init(&M, 6);
		bl_mobs_set_keep(&M, true, keep[0], keep[1], keep[2]);
		bl_mobs_start_wave(&M, 5, 5, 0);
		for (int i = 0; i < 30 * 20; i++)
			bl_mobs_step(&M, w, pl, true, 12.0f, 1.0f / 30.0f, NULL);
		CHECK(bl_mobs_count_kind(&M, BL_MOB_GOBLIN) == 0 && M.wave_left == 5, "by day nobody comes");
		bl_mobs_end_wave(&M);
		CHECK(!M.wave_on && M.wave_left == 0 && bl_mobs_wave_remaining(&M) == 0, "end_wave");
		bl_mobs_start_wave(&M, 0, 3, 0);
		CHECK(!M.wave_on, "a wave of none is no wave");
		bl_mobs_init(&M, 7);                                    /* with no keep a wave does nothing and the old spawning goes on at night near the player */
		bl_mobs_start_wave(&M, 5, 5, 0);
		const float pl2[3] = { 64.5f, 20.0f, 64.5f };
		for (int i = 0; i < 30 * 30; i++)
			bl_mobs_step(&M, w, pl2, true, 0.0f, 1.0f / 30.0f, NULL);
		CHECK(bl_mobs_count_kind(&M, BL_MOB_GOBLIN) > 0 && M.wave_left == 5, "no keep: the goblins come round the player as before (%d)", bl_mobs_count_kind(&M, BL_MOB_GOBLIN));
	}
	bl_world_free(w);
}

/* ---- building: plans, builders, gates and walls, the watchtower ---------------------------------------------------------- */

/* one step as the game does it: what the workers put goes into the world, what the goblins broke goes out */
static void build_step(struct bl_mobs *M, struct bl_world *w, const float pl[3], bool alive, float hour, struct bl_mobs_result *sum, int *bad)
{
	struct bl_mobs_result R;
	bl_mobs_step(M, w, pl, alive, hour, 1.0f / 30.0f, &R);
	for (int i = 0; i < R.nplace; i++) {
		if (bl_mobs_hit_cell(M, R.place[i].x, R.place[i].y, R.place[i].z) || !bl_cell_missing(w, &R.place[i]))
			(*bad)++;
		else
			bl_set(w, R.place[i].x, R.place[i].y, R.place[i].z, R.place[i].block);
	}
	for (int i = 0; i < R.nsmash; i++)
		bl_set(w, R.smash[i].x, R.smash[i].y, R.smash[i].z, BL_AIR);
	sum->nbuilt += R.nbuilt;
	sum->nsmash += R.nsmash;
	sum->siege_hits += R.siege_hits;
	sum->keep_damage += R.keep_damage;
	sum->shots += R.shots;
	sum->slain += R.slain;
	sum->soldiers_lost += R.soldiers_lost;
}

static int count_block(const struct bl_site *s, int block)
{
	int n = 0;
	for (int i = 0; i < s->n; i++)
		n += s->c[i].block == block;
	return n;
}

static void test_chief(void)
{
	struct bl_world *w = bl_world_flat(128, 128, 64, 20);
	static struct bl_mobs M;
	bl_mobs_init(&M, 9);
	M.enabled = false;
	CHECK(bl_weapon(BL_WP_SWORD)->damage == BL_SWORD_DAMAGE && bl_weapon(BL_WP_SPEAR)->reach > bl_weapon(BL_WP_SWORD)->reach &&
	      bl_weapon(BL_WP_HAMMER)->keep_damage > bl_weapon(BL_WP_SWORD)->keep_damage && bl_weapon(BL_WP_BOW)->shot == BL_SHOT_ARROW && bl_weapon(BL_WP_FIRE)->splash > 0 &&
	      bl_weapon(BL_WP_LASER)->reach > bl_weapon(BL_WP_BOW)->reach && bl_weapon(99) == bl_weapon(0), "the six weapons");
	bl_mobs_set_rival(&M, true, 90.5f, 20.0f, 64.5f);
	CHECK(bl_mobs_add_chief(&M, w, 90.5f, 20.0f, 58.5f) && !bl_mobs_add_chief(&M, w, 94.5f, 20.0f, 58.5f), "there is one chief");
	int ci = bl_mobs_chief(&M);
	CHECK(ci >= 0 && M.mob[ci].hp == BL_CHIEF_HP && bl_mob_is_foe(BL_MOB_CHIEF) && bl_mob_is_rival(BL_MOB_CHIEF), "a foe with %d hit points", (int)BL_CHIEF_HP);
	/* a wall in his way: he flies over it to the player and hits him */
	for (int z = 40; z < 90; z++)
		for (int y = 20; y < 25; y++)
			bl_set(w, 80, y, z, BL_STONE);
	float pl[3] = { 70.5f, 20.0f, 58.5f }, dmg = 0, high = 0;
	int bad = 0;
	for (int i = 0; i < 30 * 20; i++) {
		struct bl_mobs_result R;
		bl_mobs_step(&M, w, pl, true, 12.0f, 1.0f / 30.0f, &R);
		dmg += R.damage;
		high = fmaxf(high, M.mob[ci].y);
		bad += box_in_solid(w, M.mob[ci].x, M.mob[ci].y, M.mob[ci].z, BL_GOBLIN_HW, BL_CHIEF_H);
	}
	CHECK(M.mob[ci].alive && M.mob[ci].x < 80.0f && high > 24.5f && bad == 0, "he flies over a wall five high (x %.1f, up to %.1f, %d in blocks)", M.mob[ci].x, high, bad);
	CHECK(dmg >= 9.0f, "and strikes the player (%.0f)", dmg);
	/* far from home he lets be and goes back */
	pl[0] = 20.5f;
	for (int i = 0; i < 30 * 20; i++) {
		struct bl_mobs_result R;
		bl_mobs_step(&M, w, pl, true, 12.0f, 1.0f / 30.0f, &R);
	}
	CHECK(fabsf(M.mob[ci].x - 90.5f) < 3.0f && fabsf(M.mob[ci].z - 58.5f) < 3.0f && M.mob[ci].y < 21.0f, "back at his post, on the ground (%.1f %.1f %.1f)", M.mob[ci].x, M.mob[ci].y, M.mob[ci].z);
	/* on a raid he strikes the keep */
	bl_mobs_set_keep(&M, true, 40.5f, 20.0f, 100.5f);
	bl_mobs_set_raid(&M, 40.5f, 20.0f, 100.5f);
	M.chief_arms = 1u << BL_WP_SWORD | 1u << BL_WP_FIRE;
	CHECK(bl_mobs_chief_raid(&M) && bl_mobs_count_raiders(&M) == 1, "he leads a raid");
	float kd = 0;
	for (int i = 0; i < 30 * 30; i++) {
		struct bl_mobs_result R;
		const float far[3] = { 5.5f, 20.0f, 5.5f };
		bl_mobs_step(&M, w, far, false, 12.0f, 1.0f / 30.0f, &R);
		kd += R.keep_damage;
	}
	CHECK(kd >= 12.0f && M.mob[ci].weapon == BL_WP_FIRE, "the keep is under his fire (%.0f)", kd);
	bl_mobs_raid_retreat(&M);
	for (int i = 0; i < 30 * 40; i++) {
		struct bl_mobs_result R;
		const float far[3] = { 5.5f, 20.0f, 5.5f };
		bl_mobs_step(&M, w, far, false, 12.0f, 1.0f / 30.0f, &R);
	}
	CHECK(M.mob[ci].alive && !M.mob[ci].raider, "home again, he is no raider");
	/* the sword finds him, a splash hurts those near, soldiers and archers count him */
	float o[3] = { M.mob[ci].x, M.mob[ci].y + 1.0f, M.mob[ci].z - 3.0f }, d[3] = { 0, 0, 1 }, t = 0;
	CHECK(bl_mobs_pick(&M, o, d, 5.0f, &t) == ci, "the sword finds him");
	bl_mobs_add(&M, w, BL_MOB_GUARD, M.mob[ci].x + 1.5f, M.mob[ci].y, M.mob[ci].z);
	bl_mobs_add(&M, w, BL_MOB_WORKER, M.mob[ci].x - 1.5f, M.mob[ci].y, M.mob[ci].z);
	const float at[3] = { M.mob[ci].x, M.mob[ci].y + 0.8f, M.mob[ci].z };
	const float whp = BL_WORKER_HP;
	CHECK(bl_mobs_splash(&M, at, 3.0f, 100.0f, -1) == 2 && bl_mobs_chief(&M) < 0 && bl_mobs_count_kind(&M, BL_MOB_GUARD) == 0, "a splash takes the enemies near");
	int wi = -1;
	for (int i = 0; i < BL_MOBS_MAX; i++)
		if (M.mob[i].alive && M.mob[i].kind == BL_MOB_WORKER)
			wi = i;
	CHECK(wi >= 0 && M.mob[wi].hp == whp, "and none of the player's own");
	/* shots are pictures that end */
	const float a0[3] = { 10, 22, 10 }, a1[3] = { 10, 22, 20 };
	bl_mobs_shot(&M, a0, a1, BL_SHOT_LASER);
	struct bl_mob_hit hit;
	const float ro[3] = { 5, 22, 15 }, rd[3] = { 1, 0, 0 };
	CHECK(bl_mobs_ray(&M, ro, rd, 30.0f, &hit) && fabsf(hit.t - 5.0f) < 0.1f && hit.rgb[0] > 0.9f, "a beam is seen (%.2f)", hit.t);
	/* their diggers bring gold home */
	bl_mobs_init(&M, 4);
	M.enabled = false;
	bl_mobs_set_rival(&M, true, 40.5f, 20.0f, 40.5f);
	for (int x = 63; x <= 65; x++)
		for (int z = 39; z <= 41; z++)
			bl_set(w, x, 19, z, BL_GOLD);
	const int dep[1][3] = { { 64, 19, 40 } };
	bl_mobs_set_deposits(&M, dep, 1);
	M.rgold_wanted = true;
	for (int k = 0; k < 3; k++)
		bl_mobs_add(&M, w, BL_MOB_RWORKER, 46.5f, 20.0f, 36.5f + 2.0f * (float)k);
	int gold = 0, taken = 0;
	for (int i = 0; i < 30 * 120; i++) {
		struct bl_mobs_result R;
		const float far[3] = { 5.5f, 20.0f, 5.5f };
		bl_mobs_step(&M, w, far, false, 12.0f, 1.0f / 30.0f, &R);
		gold += R.delivered_gold;
		for (int k = 0; k < R.nraze; k++)
			if (bl_get(w, R.raze[k].x, R.raze[k].y, R.raze[k].z) == BL_GOLD)
				taken += bl_set(w, R.raze[k].x, R.raze[k].y, R.raze[k].z, BL_AIR);
	}
	CHECK(gold >= 2 && gold <= 5 && taken >= gold && taken <= 6, "one of their diggers at a time brings the gold in (%d brought, %d taken)", gold, taken);
	bl_world_free(w);
}

static void test_plans(void)
{
	struct bl_world *w = bl_world_flat(96, 96, 64, 20);
	struct bl_site s, t;
	const char *why = NULL;
	int cob, pl;
	bl_plan_cost(BL_PLAN_BARRACKS, &cob, &pl);
	CHECK(cob == 12 && pl == 8 && bl_plan_gold(BL_PLAN_MAGE) == 8 && !strcmp(bl_plan_name(BL_PLAN_TOWER), "watchtower") && !strcmp(bl_plan_name(BL_PLAN_COUNT), "?"), "the plans have names and a price");
	CHECK(bl_plan_lay(w, BL_PLAN_BARRACKS, 0, 40, 20, 40, NULL, 0, &s, &why) && s.n == 23 && count_block(&s, BL_BARRACKS) == 1, "a barracks is 23 blocks with one banner (%d, %s)", s.n, why ? why : "-");
	int lo[3], hi[3];
	bl_site_box(&s, lo, hi);
	CHECK(lo[0] == 39 && hi[0] == 41 && lo[2] == 39 && hi[2] == 41 && lo[1] == 20 && hi[1] == 22, "3 wide, 3 deep, 3 high (%d..%d, %d..%d, %d..%d)", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
	bool door = true;
	for (int i = 0; i < s.n; i++)
		door &= !(s.c[i].x == 40 && s.c[i].z == 39 && s.c[i].y < 22);
	CHECK(door, "there is a door in the front, two high");
	CHECK(bl_plan_lay(w, BL_PLAN_BARRACKS, 1, 40, 20, 40, NULL, 0, &t, NULL), "turned once");
	bl_site_box(&t, lo, hi);
	CHECK(hi[0] - lo[0] == 2 && hi[2] - lo[2] == 2 && lo[0] == 39, "turned it is 3 by 3, its front towards -x (%d..%d, %d..%d)", lo[0], hi[0], lo[2], hi[2]);
	CHECK(bl_plan_lay(w, BL_PLAN_TOWER, 0, 60, 20, 60, NULL, 0, &t, NULL) && t.n == 29 && count_block(&t, BL_TOWERPOST) == 1, "a watchtower is 29 blocks with one post");
	CHECK(bl_plan_lay(w, BL_PLAN_GATE, 0, 60, 20, 70, NULL, 0, &t, NULL) && t.n == 8 && count_block(&t, BL_GATE) == 2, "a gate: two posts and two blocks of gate");
	CHECK(!bl_plan_lay(w, BL_PLAN_BARRACKS, 0, 42, 20, 41, &s, 1, &t, &why) && t.n > 0 && why, "not over a planned building (%s), the cells are there for the red preview", why ? why : "-");
	CHECK(!bl_plan_lay(w, BL_PLAN_BARRACKS, 0, 1, 20, 40, NULL, 0, &t, &why), "not at the edge of the world");
	bl_set(w, 20, 20, 20, BL_LOG);
	CHECK(!bl_plan_lay(w, BL_PLAN_BARRACKS, 0, 20, 20, 21, NULL, 0, &t, &why) && strstr(why, "way"), "not where something stands (%s)", why);
	bl_set(w, 30, 19, 60, BL_AIR);
	CHECK(!bl_plan_lay(w, BL_PLAN_TOWER, 0, 30, 20, 60, NULL, 0, &t, &why) && strstr(why, "flat"), "not over a hole (%s)", why);
	/* a wall follows the ground */
	bl_set(w, 51, 20, 30, BL_STONE);
	CHECK(bl_plan_lay(w, BL_PLAN_WALL, 0, 50, 20, 30, NULL, 0, &t, &why) && t.n == 6, "a wall over a step (%s)", why ? why : "-");
	int ys[2] = { 0, 0 };
	for (int i = 0; i < t.n; i++)
		if (t.c[i].x == 51)
			ys[t.c[i].y >= 22] = t.c[i].y;
	CHECK(ys[0] == 21 && ys[1] == 22, "the column on the step stands one higher (%d %d)", ys[0], ys[1]);
	/* a corner: the second wall shares a column that wants the same block */
	struct bl_site a, b;
	CHECK(bl_plan_lay(w, BL_PLAN_WALL, 0, 70, 20, 30, NULL, 0, &a, NULL), "one wall");
	CHECK(bl_plan_lay(w, BL_PLAN_WALL, 1, 71, 20, 29, &a, 1, &b, &why) && b.n == 4, "a second one round the corner leaves out the shared column (%d, %s)", b.n, why ? why : "-");
	CHECK(!bl_plan_lay(w, BL_PLAN_WALL, 0, 70, 20, 30, &a, 1, &b, &why), "not the same wall twice (%s)", why ? why : "-");
	/* the same with the first one built: the shared column is not stacked on, and a gap broken in a wall is planned again */
	for (int i = 0; i < a.n; i++)
		bl_set(w, a.c[i].x, a.c[i].y, a.c[i].z, a.c[i].block);
	CHECK(bl_plan_lay(w, BL_PLAN_WALL, 1, 71, 20, 29, NULL, 0, &b, &why) && b.n == 4, "round the corner of a wall that stands (%d, %s)", b.n, why ? why : "-");
	bl_set(w, 70, 21, 30, BL_AIR);
	CHECK(bl_plan_lay(w, BL_PLAN_WALL, 0, 70, 20, 30, NULL, 0, &b, &why) && b.n == 1 && b.c[0].x == 70 && b.c[0].y == 21, "a hole in a wall is planned again (%d, %s)", b.n, why ? why : "-");
	bl_world_free(w);
}

static void test_builders(void)
{
	const float pl[3] = { 40.5f, 20.0f, 20.5f };
	struct bl_world *w = bl_world_flat(96, 96, 64, 20);
	struct bl_mobs M;
	bl_mobs_init(&M, 5);
	M.enabled = false;
	struct bl_site s;
	bl_plan_lay(w, BL_PLAN_BARRACKS, 0, 40, 20, 40, NULL, 0, &s, NULL);
	int si = bl_mobs_add_site(&M, &s);
	CHECK(si == 0 && bl_mobs_count_sites(&M) == 1, "a site");
	bl_mobs_add(&M, w, BL_MOB_WORKER, 40.5f, 20.0f, 33.5f);
	bl_mobs_add(&M, w, BL_MOB_WORKER, 43.5f, 20.0f, 33.5f);
	bl_mobs_add(&M, w, BL_MOB_SOLDIER, 30.5f, 20.0f, 30.5f);
	CHECK(bl_mobs_order_build(&M, w, 0, si) && bl_mobs_order_build(&M, w, 1, si) && !bl_mobs_order_build(&M, w, 2, si) && !bl_mobs_order_build(&M, w, 0, 3), "workers build, soldiers and no site do not");
	CHECK(hypotf(M.mob[0].tx - M.mob[1].tx, M.mob[0].tz - M.mob[1].tz) > 0.7f, "they take different places to stand");
	struct bl_mobs_result sum = { 0 };
	int bad = 0, in_block = 0;
	float t = 0, done_t = 0;
	for (int i = 0; i < 30 * 60; i++) {
		build_step(&M, w, pl, true, 12.0f, &sum, &bad);
		t += 1.0f / 30.0f;
		for (int k = 0; k < 2; k++)
			in_block += box_in_solid(w, M.mob[k].x, M.mob[k].y, M.mob[k].z, BL_GOBLIN_HW, BL_WORKER_H);
		if (!done_t && sum.nbuilt)
			done_t = t;
	}
	CHECK(bl_site_missing(w, &s) == 0 && sum.nbuilt == 1 && bl_mobs_count_sites(&M) == 0, "two workers build the barracks (%d missing, %d reports)", bl_site_missing(w, &s), sum.nbuilt);
	CHECK(done_t > 4.0f && done_t < 22.0f, "23 blocks, two builders, half a second a block: %.1f s", done_t);
	{                                                       /* the other buildings, and the castle */
		struct bl_site o;
		const int sign[4] = { BL_SMITHY, BL_MAGE, BL_INFIRMARY, BL_MINE };
		for (int k = 0; k < 4; k++)
			CHECK(bl_plan_lay(w, BL_PLAN_SMITHY + k, k, 20, 20, 70, NULL, 0, &o, NULL) && o.n > 15 && o.n <= BL_SITE_CELLS && count_block(&o, sign[k]) == 1, "%s: laid out, one sign (%d blocks)", bl_plan_name(BL_PLAN_SMITHY + k), o.n);
		struct bl_cell kc[BL_KEEP_CELLS];
		const int kn = bl_keep_cells(kc, 40, 20, 40, BL_KEEP);
		int out = 0, door = 0, top = 0, core = 0;
		for (int i = 0; i < kn; i++) {
			out += abs(kc[i].x - 40) > BL_KEEP_R || abs(kc[i].z - 40) > BL_KEEP_R || kc[i].y < 20 || kc[i].y >= 20 + BL_KEEP_TALL;
			door += kc[i].x == 40 && kc[i].z == 40 - BL_KEEP_R && kc[i].y < 22;
			top += kc[i].y == 20 + BL_KEEP_TALL - 1;
			core += kc[i].x == 40 && kc[i].z == 40 && kc[i].y == 20;
		}
		CHECK(kn > 150 && kn < BL_KEEP_CELLS && !out && !door && top == 1 && core == 1, "the castle: %d blocks in its box, a doorway, one spire (%d %d %d)", kn, out, door, top);
	}
	CHECK(bad == 0 && in_block == 0, "no block is put into a body, no worker ends up inside one (%d, %d)", bad, in_block);
	CHECK(M.mob[0].order == BL_ORDER_IDLE && M.mob[1].order == BL_ORDER_IDLE, "then they are idle");

	/* one worker and three plans: it goes on to the next by itself */
	struct bl_site tw, wl, gt;
	bl_plan_lay(w, BL_PLAN_TOWER, 0, 55, 20, 40, NULL, 0, &tw, NULL);
	bl_plan_lay(w, BL_PLAN_WALL, 0, 48, 20, 30, NULL, 0, &wl, NULL);
	bl_plan_lay(w, BL_PLAN_GATE, 0, 52, 20, 30, NULL, 0, &gt, NULL);
	int i1 = bl_mobs_add_site(&M, &wl);
	bl_mobs_add_site(&M, &gt);
	bl_mobs_add_site(&M, &tw);
	CHECK(bl_mobs_order_build(&M, w, 0, i1), "the first of three");
	sum = (struct bl_mobs_result){ 0 };
	for (int i = 0; i < 30 * 70; i++)
		build_step(&M, w, pl, true, 12.0f, &sum, &bad);
	CHECK(sum.nbuilt == 3 && bl_site_missing(w, &wl) + bl_site_missing(w, &gt) + bl_site_missing(w, &tw) == 0 && bad == 0, "one worker builds a wall, a gate and a tower in turn (%d done, %d %d %d missing)",
	      sum.nbuilt, bl_site_missing(w, &wl), bl_site_missing(w, &gt), bl_site_missing(w, &tw));

	/* a site taken away lets the builder go; a cell somebody filled is not waited for */
	bl_plan_lay(w, BL_PLAN_WALL, 0, 40, 20, 60, NULL, 0, &wl, NULL);
	int i2 = bl_mobs_add_site(&M, &wl);
	bl_mobs_order_build(&M, w, 0, i2);
	bl_mobs_remove_site(&M, i2);
	CHECK(M.mob[0].order == BL_ORDER_IDLE && bl_mobs_count_sites(&M) == 0, "a plan taken away");
	i2 = bl_mobs_add_site(&M, &wl);
	bl_set(w, wl.c[0].x, wl.c[0].y, wl.c[0].z, BL_DIRT);
	bl_mobs_order_build(&M, w, 0, i2);
	sum = (struct bl_mobs_result){ 0 };
	for (int i = 0; i < 30 * 30; i++)
		build_step(&M, w, pl, true, 12.0f, &sum, &bad);
	CHECK(sum.nbuilt == 1 && bl_get(w, wl.c[0].x, wl.c[0].y, wl.c[0].z) == BL_DIRT, "a cell filled by someone else stays and the site is done");

	/* the player standing where a block goes: it waits, and finishes when he moves */
	bl_plan_lay(w, BL_PLAN_WALL, 0, 40, 20, 70, NULL, 0, &wl, NULL);
	i2 = bl_mobs_add_site(&M, &wl);
	bl_mobs_order_build(&M, w, 0, i2);
	const float in_way[3] = { 40.5f, 20.0f, 70.5f };
	sum = (struct bl_mobs_result){ 0 };
	for (int i = 0; i < 30 * 30; i++)
		build_step(&M, w, in_way, true, 12.0f, &sum, &bad);
	CHECK(sum.nbuilt == 0 && bl_site_missing(w, &wl) == 2 && M.mob[0].order == BL_ORDER_BUILD, "no block into the player: two are left and the worker waits (%d missing)", bl_site_missing(w, &wl));
	for (int i = 0; i < 30 * 6; i++)
		build_step(&M, w, pl, true, 12.0f, &sum, &bad);
	CHECK(sum.nbuilt == 1, "he steps away and it is finished");
	bl_world_free(w);
}

static void test_gates_and_siege(void)
{
	const float far_pl[3] = { 50.5f, 20.0f, 100.0f };
	const float dusk = 17.9f;
	struct bl_world *w = bl_world_flat(128, 128, 64, 20);
	bl_set(w, 50, 20, 50, BL_GATE);
	CHECK(!bl_box_hits(w, 50.5f, 20.0f, 50.5f, 0.3f, 1.7f) && bl_box_hits_foe(w, 50.5f, 20.0f, 50.5f, 0.3f, 1.7f) && !bl_solid(BL_GATE) && bl_targetable(BL_GATE) && !bl_opaque(BL_GATE),
	      "a gate lets the player's people through and stops a goblin; it can be aimed at and does not shut out the light");
	CHECK(bl_siege_hp(BL_STONE) == 0 && bl_siege_hp(BL_DIRT) == 0 && bl_siege_hp(BL_KEEP) == 0 && bl_siege_hp(BL_LOG) == 0 && bl_siege_hp(BL_COBBLE) > bl_siege_hp(BL_GATE) && bl_siege_hp(BL_GATE) > bl_siege_hp(BL_PLANKS),
	      "goblins smash made things only, a wall holds longer than a gate");
	bl_set(w, 50, 20, 50, BL_AIR);

	/* a wall right across the world with a gate in it: a worker walks through, a goblin has to break it */
	for (int x = 0; x < 128; x++)
		for (int y = 20; y < 23; y++)
			bl_set(w, x, y, 50, x == 50 && y < 22 ? BL_GATE : BL_STONE);
	struct bl_mobs M;
	bl_mobs_init(&M, 31);
	M.spawn_t = 1e9f;
	bl_mobs_add(&M, w, BL_MOB_WORKER, 50.5f, 20.0f, 46.5f);
	bl_mobs_order_move(&M, 0, 50.5f, 55.5f);
	struct bl_mobs_result sum = { 0 };
	int bad = 0;
	for (int i = 0; i < 30 * 8; i++)
		build_step(&M, w, far_pl, false, dusk, &sum, &bad);
	CHECK(M.mob[0].z > 55.0f, "a worker walks through the gate (z %.1f)", M.mob[0].z);
	bl_mobs_clear(&M);

	bl_mobs_set_keep(&M, true, 50.5f, 20.0f, 58.5f);
	for (int y = 20; y < 23; y++)
		bl_set(w, 50, y, 58, BL_KEEP);
	bl_mobs_add(&M, w, BL_MOB_GOBLIN, 50.5f, 20.0f, 42.5f);
	float t = 0, through = 0, first_smash = 0, keep_t = 0;
	bool early = false;
	sum = (struct bl_mobs_result){ 0 };
	for (int i = 0; i < 30 * 60; i++) {
		build_step(&M, w, far_pl, false, dusk, &sum, &bad);
		t += 1.0f / 30.0f;
		if (!first_smash && sum.nsmash)
			first_smash = t;
		early |= !sum.nsmash && M.mob[0].z > 50.0f;
		if (!through && M.mob[0].z > 51.5f)
			through = t;
		if (!keep_t && sum.keep_damage > 0)
			keep_t = t;
	}
	CHECK(!early && first_smash > 10.0f && first_smash < 30.0f, "the gate holds the goblin until it has broken it (%.1f s, %d blows)", first_smash, sum.siege_hits);
	CHECK(through > first_smash && keep_t > through, "then it comes through and hits the keep (%.1f, %.1f s)", through, keep_t);
	CHECK(bl_get(w, 50, 22, 50) == BL_STONE && bl_get(w, 49, 20, 50) == BL_STONE, "the stone round it is untouched");

	/* a cobble wall two high takes longer; a brute is quicker at it */
	float tw[2];
	for (int brute = 0; brute < 2; brute++) {
		bl_mobs_clear(&M);
		memset(M.damaged, 0, sizeof M.damaged);
		for (int x = 0; x < 128; x++)
			for (int y = 20; y < 23; y++)
				bl_set(w, x, y, 50, y < 22 ? BL_COBBLE : BL_AIR);
		if (brute)
			bl_mobs_add_brute(&M, w, 50.5f, 20.0f, 46.5f);
		else
			bl_mobs_add(&M, w, BL_MOB_GOBLIN, 50.5f, 20.0f, 46.5f);
		sum = (struct bl_mobs_result){ 0 };
		t = 0;
		tw[brute] = 0;
		for (int i = 0; i < 30 * 90; i++) {
			build_step(&M, w, far_pl, false, dusk, &sum, &bad);
			t += 1.0f / 30.0f;
			if (!tw[brute] && M.mob[0].z > 51.5f)
				tw[brute] = t;
		}
		CHECK(tw[brute] > 8.0f && sum.nsmash >= 1, "%s gets through a cobble wall after %.1f s (%d broken)", brute ? "a brute" : "a goblin", tw[brute], sum.nsmash);
	}
	CHECK(tw[0] > 14.0f && tw[1] < tw[0], "a wall is worth building (%.1f s), a brute is quicker (%.1f s)", tw[0], tw[1]);

	/* the player behind his wall: the goblin comes for him through it; a goblin with nobody to go for leaves walls alone */
	bl_mobs_clear(&M);
	bl_mobs_set_keep(&M, false, 0, 0, 0);
	for (int x = 0; x < 128; x++)
		for (int y = 20; y < 22; y++)
			bl_set(w, x, y, 50, BL_PLANKS);
	bl_mobs_add(&M, w, BL_MOB_GOBLIN, 50.5f, 20.0f, 47.5f);
	sum = (struct bl_mobs_result){ 0 };
	for (int i = 0; i < 30 * 40; i++)
		build_step(&M, w, far_pl, false, dusk, &sum, &bad);
	CHECK(sum.siege_hits == 0, "a wandering goblin smashes nothing");
	const float behind[3] = { 50.5f, 20.0f, 55.5f };
	bl_mobs_clear(&M);
	bl_mobs_add(&M, w, BL_MOB_GOBLIN, 50.5f, 20.0f, 47.5f);
	for (int i = 0; i < 30 * 20; i++)
		build_step(&M, w, behind, true, dusk, &sum, &bad);
	CHECK(sum.nsmash >= 1, "one that is after the player breaks through (%d)", sum.nsmash);
	bl_world_free(w);
}

static void test_tower(void)
{
	const float far_pl[3] = { 50.5f, 20.0f, 100.0f };
	const float dusk = 17.9f;
	struct bl_world *w = bl_world_flat(128, 128, 64, 20);
	struct bl_site s;
	bl_plan_lay(w, BL_PLAN_TOWER, 0, 50, 20, 50, NULL, 0, &s, NULL);
	for (int i = 0; i < s.n; i++)
		bl_set(w, s.c[i].x, s.c[i].y, s.c[i].z, s.c[i].block);
	struct bl_mobs M;
	bl_mobs_init(&M, 41);
	M.spawn_t = 1e9f;
	bl_mobs_add(&M, w, BL_MOB_SOLDIER, 50.5f, 20.0f, 40.5f);
	bl_mobs_add(&M, w, BL_MOB_SOLDIER, 46.5f, 20.0f, 40.5f);
	bl_mobs_add(&M, w, BL_MOB_WORKER, 54.5f, 20.0f, 40.5f);
	CHECK(!bl_mobs_order_tower(&M, w, 2, 50, 24, 50) && !bl_mobs_order_tower(&M, w, 0, 50, 23, 50), "only a soldier, only on a watchpost");
	CHECK(bl_mobs_order_tower(&M, w, 0, 50, 24, 50) && !bl_mobs_order_tower(&M, w, 1, 50, 24, 50), "one soldier to a tower");
	struct bl_mobs_result sum = { 0 };
	int bad = 0;
	for (int i = 0; i < 30 * 8; i++)
		build_step(&M, w, far_pl, false, dusk, &sum, &bad);
	CHECK(M.mob[0].posted && fabsf(M.mob[0].y - 25.0f) < 0.05f && fabsf(M.mob[0].x - 50.5f) < 0.01f, "he walks there and climbs up (y %.2f)", M.mob[0].y);
	bl_mobs_clear_kind(&M, BL_MOB_WORKER);
	M.mob[1].alive = false;

	/* a goblin 15 m away is shot dead from up there; one 24 m away is left alone */
	bl_mobs_add(&M, w, BL_MOB_GOBLIN, 50.5f, 20.0f, 65.5f);
	sum = (struct bl_mobs_result){ 0 };
	float t = 0, dead = 0;
	bool arrow_seen = false;
	for (int i = 0; i < 30 * 12; i++) {
		build_step(&M, w, far_pl, false, dusk, &sum, &bad);
		t += 1.0f / 30.0f;
		for (int k = 0; k < BL_ARROWS_MAX; k++)
			if (M.arrow[k].t > 0.1f && !arrow_seen) {
				float f = 1.0f - M.arrow[k].t / BL_ARROW_TIME + 0.2f, c[3], o[3], d[3];
				for (int a = 0; a < 3; a++)
					c[a] = M.arrow[k].a[a] + (M.arrow[k].b[a] - M.arrow[k].a[a]) * f;
				o[0] = c[0] + 3.0f;
				o[1] = c[1];
				o[2] = c[2];
				d[0] = -1.0f;
				d[1] = d[2] = 0.0f;
				struct bl_mob_hit h;
				arrow_seen = bl_mobs_ray(&M, o, d, 10.0f, &h) && h.id == -1;
			}
		if (!dead && sum.slain)
			dead = t;
	}
	CHECK(sum.slain == 1 && sum.shots == 4 && dead > 4.0f && dead < 8.0f && M.mob[0].posted && M.mob[0].hp == BL_SOLDIER_HP, "four arrows kill it (%d shots, %.1f s)", sum.shots, dead);
	CHECK(arrow_seen, "an arrow can be seen on its way");
	bl_mobs_add(&M, w, BL_MOB_GOBLIN, 50.5f, 20.0f, 75.5f);
	M.mob[1].widle = true;
	M.mob[1].wander_t = 1e9f;
	sum = (struct bl_mobs_result){ 0 };
	for (int i = 0; i < 30 * 4; i++)
		build_step(&M, w, far_pl, false, dusk, &sum, &bad);
	CHECK(sum.shots == 0, "one 25 m away is out of range");
	M.mob[1].alive = false;

	/* one at the foot of the tower: it does not go for the man up there; a stone screen stops the arrows */
	for (int x = 40; x <= 60; x++)
		for (int y = 20; y < 30; y++)
			bl_set(w, x, y, 56, BL_STONE);
	bl_mobs_add(&M, w, BL_MOB_GOBLIN, 50.5f, 20.0f, 60.5f);
	M.mob[1].widle = true;
	M.mob[1].wander_t = 1e9f;
	sum = (struct bl_mobs_result){ 0 };
	for (int i = 0; i < 30 * 4; i++)
		build_step(&M, w, far_pl, false, dusk, &sum, &bad);
	CHECK(sum.shots == 0 && sum.siege_hits == 0 && sum.soldiers_lost == 0, "no shot through a wall, and the goblin takes no notice of the man up there (%d shots)", sum.shots);
	for (int x = 40; x <= 60; x++)
		for (int y = 20; y < 30; y++)
			bl_set(w, x, y, 56, BL_AIR);
	M.mob[1].alive = false;

	/* sent away he comes down; the post broken he falls */
	bl_mobs_order_move(&M, 0, 42.5f, 50.5f);
	for (int i = 0; i < 30 * 8; i++)
		build_step(&M, w, far_pl, false, dusk, &sum, &bad);
	CHECK(!M.mob[0].posted && M.mob[0].y < 20.1f && hypotf(M.mob[0].x - 42.5f, M.mob[0].z - 50.5f) < 1.6f, "a new post: he comes down and walks there (%.1f %.1f %.1f)", M.mob[0].x, M.mob[0].y, M.mob[0].z);
	bl_mobs_order_tower(&M, w, 0, 50, 24, 50);
	for (int i = 0; i < 30 * 8; i++)
		build_step(&M, w, far_pl, false, dusk, &sum, &bad);
	CHECK(M.mob[0].posted, "up again");
	bl_mobs_order_stop(&M, 0);
	for (int i = 0; i < 30 * 3; i++)
		build_step(&M, w, far_pl, false, dusk, &sum, &bad);
	CHECK(!M.mob[0].posted && M.mob[0].y < 20.1f && hypotf(M.mob[0].x - 50.5f, M.mob[0].z - 50.5f) > 1.5f && hypotf(M.mob[0].tx - M.mob[0].x, M.mob[0].tz - M.mob[0].z) < 1.1f,
	      "stop: down beside the tower, and that is his post now");
	bl_mobs_order_tower(&M, w, 0, 50, 24, 50);
	for (int i = 0; i < 30 * 8; i++)
		build_step(&M, w, far_pl, false, dusk, &sum, &bad);
	bl_set(w, 50, 24, 50, BL_AIR);
	for (int i = 0; i < 30 * 4; i++)
		build_step(&M, w, far_pl, false, dusk, &sum, &bad);
	CHECK(M.mob[0].alive && !M.mob[0].posted && M.mob[0].y < 21.0f && M.mob[0].order == BL_ORDER_IDLE, "the post gone: he is on the ground again (y %.1f)", M.mob[0].y);
	bl_world_free(w);
}

static void test_ghosts(void)
{
	struct bl_world *w = bl_world_flat(96, 96, 64, 20);
	struct bl_site s;
	bl_plan_lay(w, BL_PLAN_WALL, 0, 48, 20, 52, NULL, 0, &s, NULL);
	static struct bl_ghosts g;
	bl_ghosts_clear(&g);
	bl_ghosts_add(&g, w, &s, 0x80c0ffu);
	float o[3] = { 48.5f, 20.5f, 48.0f }, d[3] = { 0, 0, 1 }, rgb[3], a = 0, t = 0;
	CHECK(g.n == 6 && bl_ghosts_ray(&g, o, d, 50.0f, rgb, &a, &t) && fabsf(t - 4.02f) < 0.01f && a < 0.5f && rgb[2] > rgb[0], "a planned block is met by a ray, see-through in the middle of a face (%.2f at %.2f)", a, t);
	float o2[3] = { 48.04f, 20.5f, 48.0f };
	CHECK(bl_ghosts_ray(&g, o2, d, 50.0f, rgb, &a, &t) && a > 0.7f, "and solid at its edges");
	CHECK(!bl_ghosts_ray(&g, o, d, 3.0f, rgb, &a, &t), "not beyond the reach");
	enum { W = 160, H = 100 };
	static uint32_t p0[W * H], p1[W * H];
	struct bl_camera cam = { 48.5f, 22.0f, 46.0f, 0.0f, -0.15f, 1.2f };
	struct bl_view v;
	bl_view_init(&v);
	bl_render(w, &cam, &v, p0, W, H, W, 2);
	v.ghosts = &g;
	bl_render(w, &cam, &v, p1, W, H, W, 2);
	int diff = 0, blue = 0;
	for (int i = 0; i < W * H; i++)
		if (p0[i] != p1[i]) {
			diff++;
			blue += (int)(p1[i] & 255) - (int)(p0[i] & 255) > 10;
		}
	CHECK(diff > 600 && blue > 300, "the picture shows them (%d pixels, %d bluer)", diff, blue);
	bl_set(w, s.c[0].x, s.c[0].y, s.c[0].z, BL_COBBLE);
	bl_ghosts_clear(&g);
	bl_ghosts_add(&g, w, &s, 0x80c0ffu);
	CHECK(g.n == 5, "a block that is there is no ghost");
	bl_world_free(w);
}

/* ---- the player's body ---------------------------------------------------------------------------------------------------- */

static bool purple(const float c[3])
{
	return c[2] > c[0] && c[0] > c[1] * 1.2f;
}

static void test_hero(void)
{
	struct bl_world *w = bl_world_flat(96, 96, 64, 20);
	struct bl_hero h;
	memset(&h, 0, sizeof h);
	h.x = 48.5f;
	h.y = 20.0f;
	h.z = 48.5f;
	h.fov = 1.2f;
	h.aspect = 1.6f;
	h.held = BL_STONE;
	bl_hero_set(&h, w, 12.0f);
	float rgb[3], t = 0;
	float front[3] = { 48.5f, 21.1f, 51.5f }, back[3] = { 48.5f, 21.1f, 45.5f }, tz[3] = { 0, 0, 1 }, mz[3] = { 0, 0, -1 };
	CHECK(bl_hero_ray(&h, front, mz, 20.0f, rgb, &t) && purple(rgb) && fabsf(t - 2.86f) < 0.02f, "from the front: the purple tunic (%.2f at %.2f, %.2f %.2f %.2f)", t, 3.0f, rgb[0], rgb[1], rgb[2]);
	CHECK(bl_hero_ray(&h, back, tz, 20.0f, rgb, &t) && rgb[0] > 0.6f && rgb[2] > 0.6f && rgb[1] > 0.6f, "from behind: the white cape (%.2f %.2f %.2f)", rgb[0], rgb[1], rgb[2]);
	front[1] = back[1] = 21.62f;
	CHECK(bl_hero_ray(&h, front, mz, 20.0f, rgb, &t) && bl_hero_ray(&h, back, tz, 20.0f, rgb, &t) && rgb[0] > rgb[2], "a head at the height of the eyes, brown hair behind");
	front[1] = 21.95f;
	CHECK(!bl_hero_ray(&h, front, mz, 20.0f, rgb, &t), "nothing over it");
	float side[3] = { 49.3f, 21.1f, 51.5f };
	CHECK(!bl_hero_ray(&h, side, mz, 20.0f, rgb, &t) && !bl_hero_ray(&h, back, tz, 2.0f, rgb, &t), "nothing beside it, or beyond the reach");
	h.yaw = 3.14159265f;
	bl_hero_set(&h, w, 12.0f);
	back[1] = 21.1f;
	CHECK(bl_hero_ray(&h, back, tz, 20.0f, rgb, &t) && purple(rgb), "turned round, the front is the other way");
	h.yaw = 0.0f;

	/* from his own eyes */
	h.first = true;
	h.eye[0] = 48.5f;
	h.eye[1] = 21.62f;
	h.eye[2] = 48.5f;
	bl_hero_set(&h, w, 12.0f);
	float boots[3] = { 0.1f, -1.5f, 0.12f }, ahead[3] = { 0, 0, 1 }, chest[3] = { 0.0f, -1.0f, -0.4f };
	for (int i = 0; i < 2; i++) {
		float *d = i ? chest : boots, l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		for (int k = 0; k < 3; k++)
			d[k] /= l;
	}
	CHECK(bl_hero_ray(&h, h.eye, boots, 20.0f, rgb, &t) && !purple(rgb) && t > 0.75f, "looking down he sees a boot or a leg (%.2f)", t);
	CHECK(bl_hero_ray(&h, h.eye, chest, 20.0f, rgb, &t) && purple(rgb) && t > 0.4f, "and his chest below and behind (%.2f)", t);
	CHECK(!bl_hero_ray(&h, h.eye, ahead, 20.0f, rgb, &t), "his own head is not in the way");
	float lr[3] = { 0.48f, -0.27f, 1.0f }, mid[3] = { 0, 0, 1 }, ll[3] = { -0.48f, -0.27f, 1.0f };
	for (int k = 0; k < 3; k++) {
		lr[k] /= 1.19f;
		ll[k] /= 1.19f;
	}
	CHECK(bl_hero_arm_ray(&h, lr, rgb) && !bl_hero_arm_ray(&h, mid, rgb) && !bl_hero_arm_ray(&h, ll, rgb), "the arm is in the lower right, not in the middle or on the left");
	/* what the arm holds changes the picture */
	enum { W = 160, H = 100 };
	static uint32_t base[W * H], stone[W * H], sword[W * H], empty[W * H], swung[W * H], t4[W * H];
	struct bl_camera cam = { 48.5f, 21.62f, 48.5f, 0.0f, 0.0f, 1.2f };
	struct bl_view v;
	bl_view_init(&v);
	bl_render(w, &cam, &v, base, W, H, W, 2);
	v.hero = &h;
	bl_render(w, &cam, &v, stone, W, H, W, 2);
	bl_render(w, &cam, &v, t4, W, H, W, 4);
	h.sword = true;
	bl_hero_set(&h, w, 12.0f);
	bl_render(w, &cam, &v, sword, W, H, W, 2);
	h.sword = false;
	h.held = 0;
	bl_hero_set(&h, w, 12.0f);
	bl_render(w, &cam, &v, empty, W, H, W, 2);
	h.held = BL_STONE;
	h.swing = 0.5f;
	bl_hero_set(&h, w, 12.0f);
	bl_render(w, &cam, &v, swung, W, H, W, 2);
	h.swing = 0.0f;
	bl_hero_set(&h, w, 12.0f);
	int n_arm = 0, n_left = 0, grey = 0, high = 0, d_empty = 0, d_swing = 0;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			int i = y * W + x;
			if (stone[i] != base[i]) {
				n_arm++;
				n_left += x < W / 2;
				int r = (int)(stone[i] >> 16 & 255), g = (int)(stone[i] >> 8 & 255), b = (int)(stone[i] & 255);
				grey += abs(r - g) < 12 && abs(g - b) < 12;
			}
			high += sword[i] != base[i] && y < H / 2;
			d_empty += empty[i] != stone[i];
			d_swing += swung[i] != stone[i];
		}
	CHECK(n_arm > 600 && n_left == 0 && grey > 150, "the arm and a stone in the right half of the picture (%d pixels, %d grey)", n_arm, grey);
	CHECK(high > 30, "the sword reaches above the middle (%d)", high);
	CHECK(d_empty > 150 && d_swing > 200, "an empty hand and a swing look different (%d, %d)", d_empty, d_swing);
	CHECK(memcmp(stone, t4, sizeof stone) == 0, "four threads draw the same");
	/* the hook for the tests does not see him */
	v.ids = true;
	bl_render(w, &cam, &v, stone, W, H, W, 2);
	v.hero = NULL;
	bl_render(w, &cam, &v, base, W, H, W, 2);
	CHECK(memcmp(stone, base, sizeof stone) == 0, "pointing at blocks is not changed");
	/* the rectangles the boxes are looked for in miss nothing: the same pictures without them, looking down and turned and walking */
	{
		static uint32_t with[W * H], without[W * H];
		int differ = 0;
		for (int i = 0; i < 24; i++) {
			h.first = true;
			h.sword = i % 3 == 0;
			h.held = i % 3 == 1 ? BL_TORCH : BL_PLANKS;
			h.yaw = (float)i * 0.7f;
			h.pitch = -1.5f + (float)(i % 8) * 0.3f;
			h.phase = (float)i * 0.13f;
			h.stride = (float)(i % 2);
			h.swing = (float)(i % 5) * 0.2f;
			h.sneak = i % 7 == 0;
			bl_hero_set(&h, w, 12.0f);
			struct bl_camera c2 = { h.eye[0], h.eye[1], h.eye[2], h.yaw, h.pitch, 1.2f };
			v.hero = &h;
			bl_render(w, &c2, &v, with, W, H, W, 2);
			static struct bl_hero plain;
			plain = h;
			for (int k = 0; k < plain.nbody; k++)
				plain.body[k].bound = false;
			for (int k = 0; k < plain.narm; k++)
				plain.vm[k].bound = false;
			v.hero = &plain;
			bl_render(w, &c2, &v, without, W, H, W, 2);
			for (int k = 0; k < W * H; k++)
				differ += with[k] != without[k];
		}
		CHECK(differ == 0, "24 poses with and without the rectangles: %d pixels differ", differ);
		h.yaw = h.pitch = h.phase = h.stride = h.swing = 0.0f;
		h.sneak = h.sword = false;
		h.held = BL_STONE;
	}
	/* a night is darker, a torch lights him */
	h.first = false;
	bl_hero_set(&h, w, 0.0f);
	float night[3];
	back[1] = 21.1f;
	CHECK(bl_hero_ray(&h, back, tz, 20.0f, night, &t) && night[1] < 0.4f && night[1] > 0.05f, "dim at night (%.2f)", night[1]);
	bl_set(w, 49, 20, 47, BL_TORCH);
	bl_hero_set(&h, w, 0.0f);
	CHECK(bl_hero_ray(&h, back, tz, 20.0f, rgb, &t) && rgb[0] > night[0] * 1.5f, "a torch lights him");
	CHECK(bl_hero_floor_shade(&h, 48.5f, 20.0f, 48.5f) < 0.7f && bl_hero_floor_shade(&h, 49.5f, 20.0f, 48.5f) == 1.0f && bl_hero_floor_shade(&h, 48.5f, 16.0f, 48.5f) == 1.0f, "a shadow under his feet only");

	/* nobody walks through him */
	struct bl_mobs M;
	bl_mobs_init(&M, 3);
	struct bl_player p;
	bl_player_init(&p, w);
	p.x = 30.5f;
	p.y = 20.0f;
	p.z = 30.5f;
	bl_mobs_add(&M, w, BL_MOB_WORKER, 30.8f, 20.0f, 30.5f);
	bl_mobs_push_player(&M, w, &p);
	CHECK(fabsf(M.mob[0].x - p.x) > 0.59f && p.x == 30.5f, "a worker he walks into gives way all the way (%.2f, %.2f)", M.mob[0].x, p.x);
	bl_mobs_clear(&M);
	bl_mobs_add(&M, w, BL_MOB_GOBLIN, 30.8f, 20.0f, 30.5f);
	bl_mobs_push_player(&M, w, &p);
	CHECK(fabsf((M.mob[0].x - 30.8f) - 0.15f) < 0.01f && fabsf((30.5f - p.x) - 0.15f) < 0.01f, "a goblin and he each give half (%.3f, %.3f)", M.mob[0].x, p.x);
	bl_mobs_clear(&M);
	for (int y = 20; y < 23; y++)
		bl_set(w, 32, y, 30, BL_STONE);
	p.x = 31.2f;
	bl_mobs_add(&M, w, BL_MOB_WORKER, 31.69f, 20.0f, 30.5f);
	bl_mobs_push_player(&M, w, &p);
	CHECK(M.mob[0].x < 31.71f && M.mob[0].x - p.x > 0.58f && !bl_box_hits(w, M.mob[0].x, M.mob[0].y, M.mob[0].z, BL_GOBLIN_HW, BL_WORKER_H), "against a wall it cannot give way: he steps back (%.2f, %.2f)", M.mob[0].x, p.x);
	bl_mobs_clear(&M);
	p.x = 40.5f;
	bl_mobs_add(&M, w, BL_MOB_SOLDIER, 40.7f, 20.0f, 30.5f);
	M.mob[0].posted = true;
	bl_mobs_push_player(&M, w, &p);
	CHECK(M.mob[0].x == 40.7f && 40.7f - p.x > 0.59f, "one on a watchtower does not move");
	p.y = 22.0f;
	p.x = 40.6f;
	bl_mobs_push_player(&M, w, &p);
	CHECK(p.x == 40.6f, "somebody above his head is not pushed");
	/* walking into a crowd never puts anybody into a block */
	bl_mobs_clear(&M);
	struct bl_world *tw = bl_world_new(7, 128, 128, 64);
	bl_player_init(&p, tw);
	for (int i = 0; i < 12; i++)
		bl_mobs_add(&M, tw, i & 1 ? BL_MOB_WORKER : BL_MOB_GOBLIN, p.x + (float)(i % 4) - 1.5f, p.y + 1.0f, p.z + (float)(i / 4) - 1.0f);
	int bad = 0;
	uint32_t r = 5;
	for (int i = 0; i < 4000; i++) {
		r = r * 1103515245u + 12345u;
		struct bl_input in = { .fwd = (float)((r >> 8) % 3) - 1.0f, .strafe = (float)((r >> 16) % 3) - 1.0f, .jump = (r >> 24) % 9 == 0 };
		p.yaw += (float)((r >> 4) % 100) * 0.002f - 0.1f;
		bl_player_step(tw, &p, &in, 1.0f / 30.0f);
		bl_mobs_push_player(&M, tw, &p);
		bad += bl_box_hits(tw, p.x, p.y, p.z, BL_PLAYER_W * 0.5f, BL_PLAYER_H);
		for (int k = 0; k < BL_MOBS_MAX; k++)
			if (M.mob[k].alive)
				bad += bl_box_hits_foe(tw, M.mob[k].x, M.mob[k].y, M.mob[k].z, BL_GOBLIN_HW, bl_mob_h(M.mob[k].kind)) && M.mob[k].kind == BL_MOB_WORKER;
	}
	CHECK(bad == 0, "4000 steps among twelve: nobody is pushed into a block (%d)", bad);
	bl_world_free(tw);
	bl_world_free(w);
}

static void test_brutes(void)
{
	const float dusk = 17.9f;
	struct bl_world *w = bl_world_flat(128, 128, 64, 20);
	struct bl_mobs M;
	bl_mobs_init(&M, 31);
	M.spawn_t = 1e9f;

	/* a brute is a taller, tougher goblin */
	CHECK(bl_mobs_add_brute(&M, w, 50.5f, 20.0f, 50.5f) && M.mob[0].brute && M.mob[0].kind == BL_MOB_GOBLIN && M.mob[0].hp == BL_BRUTE_HP, "a brute: a goblin with 26 hit points");
	CHECK(bl_mobs_add(&M, w, BL_MOB_GOBLIN, 60.5f, 20.0f, 50.5f) && !M.mob[1].brute && M.mob[1].hp == BL_GOBLIN_HP, "a plain goblin is none");
	bl_set(w, 70, 22, 50, BL_STONE);
	CHECK(!bl_mobs_add_brute(&M, w, 70.5f, 20.0f, 50.5f) && bl_mobs_add(&M, w, BL_MOB_GOBLIN, 70.5f, 20.0f, 50.5f), "under a roof two blocks up there is room for a goblin and none for a brute");
	bl_set(w, 70, 22, 50, BL_AIR);
	CHECK(bl_mobs_hit_cell(&M, 50, 21, 50) && bl_mobs_hit_cell(&M, 50, 22, 50) && !bl_mobs_hit_cell(&M, 60, 22, 50), "a block cannot be put in it up to 2.1 m");
	float eye[3] = { 50.5f, 21.9f, 46.0f }, look[3] = { 0, 0, 1 }, eye2[3] = { 60.5f, 21.9f, 46.0f }, t = 0;
	CHECK(bl_mobs_pick(&M, eye, look, 6.0f, &t) == 0 && bl_mobs_pick(&M, eye2, look, 6.0f, &t) < 0, "the sword finds a brute at 1.9 m and the goblin's head is lower");
	/* seven blows of the sword, not three */
	{
		int blows = 0;
		while (M.mob[0].alive && blows < 20) {
			bl_mobs_hurt(&M, 0, BL_SWORD_DAMAGE, 0, 1);
			blows++;
		}
		CHECK(blows == 7 && M.kills == 1, "seven blows kill a brute (%d)", blows);
	}
	bl_mobs_clear(&M);

	/* the picture: taller than a goblin, darker, with horns */
	{
		bl_mobs_add_brute(&M, w, 50.5f, 20.0f, 50.5f);
		bl_mobs_add(&M, w, BL_MOB_GOBLIN, 56.5f, 20.0f, 50.5f);
		for (int i = 0; i < 2; i++) {
			M.mob[i].yaw = 0.0f;
			M.mob[i].ycos = 1.0f;
			M.mob[i].ysin = 0.0f;
		}
		bl_mobs_light(&M, w, 12.0f);
		struct bl_mob_hit hit;
		float d[3] = { 0, 0, 1 }, o1[3] = { 50.5f, 22.0f, 46.0f }, o2[3] = { 56.5f, 22.0f, 46.0f };
		CHECK(bl_mobs_ray(&M, o1, d, 20.0f, &hit) && hit.id == 0, "a ray at 2.0 m hits the brute");
		CHECK(!bl_mobs_ray(&M, o2, d, 20.0f, &hit), "and goes over the goblin's head");
		float o3[3] = { 50.5f, 20.0f + 1.15f * 1.5f, 46.0f };
		struct bl_mob_hit h3;
		CHECK(bl_mobs_ray(&M, o3, d, 20.0f, &h3) && h3.id == 0 && h3.rgb[1] > h3.rgb[0], "its chest plate is grey-green (%.2f %.2f %.2f)", h3.rgb[0], h3.rgb[1], h3.rgb[2]);
	}
	bl_mobs_clear(&M);

	/* it is slower than a goblin (2.1 against 2.7 m/s) and a blow of it is 5 of the player's hit points, with a bigger push */
	{
		const float pl[3] = { 55.5f, 20.0f, 72.5f };
		bl_mobs_add_brute(&M, w, 50.5f, 20.0f, 50.5f);
		bl_mobs_add(&M, w, BL_MOB_GOBLIN, 60.5f, 20.0f, 50.5f);
		float dist_b = 0, dist_g = 0, pz = M.mob[0].z, gz = M.mob[1].z;
		for (int i = 0; i < 30 * 4; i++) {
			bl_mobs_step(&M, w, pl, true, dusk, 1.0f / 30.0f, NULL);
			dist_b = M.mob[0].z - pz;
			dist_g = M.mob[1].z - gz;
		}
		CHECK(dist_b > 6.0f && dist_b < 8.8f && dist_g > 8.8f && dist_g < 11.5f, "a brute walks about 8 m in four seconds, a goblin about 10 (%.1f, %.1f)", dist_b, dist_g);
	}
	bl_mobs_clear(&M);
	{
		const float pl[3] = { 50.5f, 20.0f, 52.5f };
		bl_mobs_add_brute(&M, w, 50.5f, 20.0f, 50.5f);
		float damage = 0, kb = 0;
		int hits = 0;
		for (int i = 0; i < 30 * 6; i++) {
			struct bl_mobs_result R;
			bl_mobs_step(&M, w, pl, true, dusk, 1.0f / 30.0f, &R);
			damage += R.damage;
			hits += R.hits;
			kb = fmaxf(kb, hypotf(R.kbx, R.kbz));
		}
		CHECK(hits >= 3 && fabsf(damage - 5.0f * (float)hits) < 0.01f, "a blow of a brute takes five hit points (%d blows, %.0f)", hits, damage);
		CHECK(kb > 7.0f, "and pushes harder (%.1f)", kb);
	}
	bl_mobs_clear(&M);

	/* a wave with brutes: they are the last ones, and they hit the keep for four */
	{
		const float keep[3] = { 64.5f, 20.0f, 64.5f }, pl[3] = { 8.5f, 20.0f, 8.5f };
		bl_mobs_init(&M, 8);
		bl_mobs_set_keep(&M, true, keep[0], keep[1], keep[2]);
		bl_mobs_start_wave(&M, 5, 5, 2);
		CHECK(M.wave_brutes == 2, "two of the five are brutes");
		bl_mobs_start_wave(&M, 5, 5, 9);
		CHECK(M.wave_brutes == 5, "never more brutes than goblins");
		bl_mobs_start_wave(&M, 5, 5, 2);
		int born = 0, born_brutes = 0, order_ok = 1, seen_brute = 0;
		float kd = 0.0f;
		int kh = 0;
		for (int i = 0; i < 30 * 60; i++) {
			bool had[BL_MOBS_MAX];
			for (int k = 0; k < BL_MOBS_MAX; k++)
				had[k] = M.mob[k].alive;
			struct bl_mobs_result R;
			bl_mobs_step(&M, w, pl, true, 0.0f, 1.0f / 30.0f, &R);
			born += R.born;
			born_brutes += R.born_brutes;
			kd += R.keep_damage;
			kh += R.keep_hits;
			for (int k = 0; k < BL_MOBS_MAX; k++)
				if (M.mob[k].alive && !had[k]) {
					if (M.mob[k].brute)
						seen_brute = 1;
					else if (seen_brute)
						order_ok = 0;                       /* a plain goblin after a brute */
				}
		}
		CHECK(born == 5 && born_brutes == 2 && bl_mobs_count_brutes(&M) == 2 && order_ok, "five born, two of them brutes, and the brutes last (%d, %d)", born, born_brutes);
		CHECK(kh > 0 && kd > 2.0f * (float)kh, "the keep took %.0f in %d blows: the brutes hit harder", kd, kh);
	}
	bl_world_free(w);
}

/* trees: a click on the leaves means the tree, every tree of a made world can be gone to, and leaves do not stop the player's own */
static void test_trees(void)
{
	static struct bl_mobs M;
	struct bl_world *w = bl_world_new(7, 128, 128, 64);
	int trees = 0, by_trunk = 0, by_leaf = 0, leaf_is_log = 0;
	for (int z = 8; z < 120; z++)
		for (int x = 8; x < 120; x++)
			for (int y = 2; y < 60; y++) {
				if (bl_get(w, x, y, z) != BL_LOG || bl_get(w, x, y - 1, z) == BL_LOG)
					continue;
				int top = y;
				while (bl_get(w, x, top + 1, z) == BL_LOG)
					top++;
				trees++;
				bl_mobs_init(&M, 1);
				bool in = false;
				for (int k = 3; k < 8 && !in; k++)
					in = bl_mobs_add(&M, w, BL_MOB_WORKER, (float)(x + k) + 0.5f, (float)(bl_top(w, x + k, z) + 1), (float)z + 0.5f);
				if (!in)
					M.mob[0] = (struct bl_mob){ .alive = true, .kind = BL_MOB_WORKER, .x = (float)x + 3.5f, .y = (float)y, .z = (float)z + 0.5f };
				by_trunk += bl_mobs_order_gather(&M, w, 0, x, top, z);
				int leaf = top + 1;
				while (bl_get(w, x, leaf + 1, z) == BL_LEAVES)
					leaf++;
				if (bl_mobs_order_gather(&M, w, 0, x, leaf, z)) {
					by_leaf++;
					leaf_is_log += M.mob[0].gblock == BL_LOG && bl_get(w, M.mob[0].gx, M.mob[0].gy, M.mob[0].gz) == BL_LOG && bl_get(w, M.mob[0].gx, M.mob[0].gy - 1, M.mob[0].gz) != BL_LOG;
				}
			}
	CHECK(trees > 50 && by_trunk == trees, "every tree's trunk can be gone to (%d of %d)", by_trunk, trees);
	CHECK(by_leaf == trees && leaf_is_log == trees, "an order on the crown is an order for the tree: the foot of a trunk (%d, %d of %d)", by_leaf, leaf_is_log, trees);
	bl_world_free(w);

	/* leaves beside a post of logs make no tree; a hedge of leaves stops the player and the goblins, not the workers */
	w = bl_world_flat(96, 96, 64, 20);
	bl_set(w, 40, 20, 40, BL_LOG);
	bl_set(w, 41, 20, 40, BL_LEAVES);
	bl_mobs_init(&M, 2);
	bl_mobs_add(&M, w, BL_MOB_WORKER, 44.5f, 20.0f, 40.5f);
	CHECK(bl_mobs_order_gather(&M, w, 0, 41, 20, 40) && M.mob[0].gblock == BL_LEAVES, "a leaf beside a log that has none over it is just a leaf");
	for (int x = 0; x < 96; x++)
		for (int y = 20; y < 24; y++)
			bl_set(w, x, y, 60, BL_LEAVES);
	bl_mobs_clear(&M);
	bl_mobs_add(&M, w, BL_MOB_WORKER, 50.5f, 20.0f, 55.5f);
	bl_mobs_order_move(&M, 0, 50.5f, 66.5f);
	M.enabled = false;
	struct bl_mobs_result R;
	float far_pl[3] = { 5.5f, 20.0f, 5.5f };
	for (int i = 0; i < 30 * 10; i++)
		bl_mobs_step(&M, w, far_pl, true, 12.0f, 1.0f / 30.0f, &R);
	CHECK(M.mob[0].z > 65.5f && M.mob[0].y < 20.1f, "a worker goes through a hedge four high (z %.1f y %.1f)", M.mob[0].z, M.mob[0].y);
	CHECK(bl_box_hits(w, 50.5f, 20.0f, 60.5f, 0.3f, 1.8f) && !bl_box_hits_foe(w, 50.5f, 20.0f, 60.5f, 0.3f, 1.8f) && !bl_box_hits_unit(w, 50.5f, 20.0f, 60.5f, 0.3f, 1.8f),
	      "it stops the player still, not a goblin and not a unit");
	/* and a goblin comes through it after a worker */
	bl_mobs_clear(&M);
	M.enabled = true;
	M.cap = 1;
	bl_mobs_add(&M, w, BL_MOB_WORKER, 50.5f, 20.0f, 64.5f);
	bl_mobs_add(&M, w, BL_MOB_GOBLIN, 50.5f, 20.0f, 56.5f);
	int lost = 0;
	for (int i = 0; i < 30 * 15 && !lost; i++) {
		bl_mobs_step(&M, w, far_pl, true, 17.9f, 1.0f / 30.0f, &R);
		lost += R.workers_lost;
	}
	CHECK(lost == 1, "a goblin gets through the hedge to a worker and kills it");
	bl_world_free(w);
}

/* units that are away from the player (in a wave: from the keep) have goblins come out of the ground round them */
static void test_raids(void)
{
	static struct bl_mobs M;
	struct bl_world *w = bl_world_flat(256, 256, 64, 20);
	float pl[3] = { 60.5f, 20.0f, 60.5f };
	for (int wave = 0; wave < 2; wave++) {
		bl_mobs_init(&M, 11);
		M.cap = wave ? 12 : 2;                          /* (the two round the player stay: the ones for the unit are on top of them) */
		if (wave) {
			bl_mobs_set_keep(&M, true, 60.5f, 20.0f, 70.5f);
			bl_mobs_start_wave(&M, 18, 12, 0);
		}
		bl_mobs_add(&M, w, BL_MOB_WORKER, 190.5f, 20.0f, 190.5f);          /* some 180 blocks from the player and the keep */
		bool seen[BL_MOBS_MAX] = { false };
		int near_unit = 0, near_home = 0, lost = 0;
		for (int i = 0; i < 30 * 70; i++) {
			struct bl_mobs_result R;
			bl_mobs_step(&M, w, pl, false || !wave, 0.0f, 1.0f / 30.0f, &R);
			lost += R.workers_lost;
			for (int k = 0; k < BL_MOBS_MAX; k++) {
				const struct bl_mob *m = &M.mob[k];
				if (!m->alive || m->kind != BL_MOB_GOBLIN) {
					seen[k] = false;
					continue;
				}
				if (seen[k])
					continue;
				seen[k] = true;
				if (hypotf(m->x - 190.5f, m->z - 190.5f) < 30.0f)
					near_unit++;
				else if (hypotf(m->x - 60.5f, m->z - (wave ? 70.5f : 60.5f)) < 34.0f)
					near_home++;
			}
		}
		CHECK(near_unit >= 1 && near_home >= 2 && lost == 1, "%s: goblins come out round a unit far away (%d) as well as at home (%d), and get it (%d)", wave ? "in a wave" : "at night", near_unit, near_home, lost);
		if (wave)
			CHECK(near_home > near_unit, "most of a wave comes for the keep (%d at the keep, %d at the unit)", near_home, near_unit);
	}
	/* no unit away from him: nobody else is raided, and a unit near the player changes nothing */
	bl_mobs_init(&M, 11);
	bl_mobs_add(&M, w, BL_MOB_WORKER, 80.5f, 20.0f, 60.5f);
	M.mob[0].hp = 1e9f;
	int far_born = 0;
	for (int i = 0; i < 30 * 40; i++) {
		bl_mobs_step(&M, w, pl, true, 0.0f, 1.0f / 30.0f, NULL);
		for (int k = 1; k < BL_MOBS_MAX; k++)
			if (M.mob[k].alive && M.mob[k].age < 0.04f && hypotf(M.mob[k].x - 60.5f, M.mob[k].z - 60.5f) < 13.9f)
				far_born++;
	}
	CHECK(far_born == 0, "with the unit near him the goblins appear 14 blocks and more from the player, as before (%d nearer)", far_born);
	bl_world_free(w);
}

/* ---- finding a way (blockspath.c) ---------------------------------------------------------------------------------------- */

static void wall_x(struct bl_world *w, int x0, int x1, int z, int high, int block)
{
	for (int x = x0; x <= x1; x++)
		for (int y = 20; y < 20 + high; y++)
			bl_set(w, x, y, z, block);
}

/* every turn of a way is a place the body can be in */
static bool way_stands(const struct bl_world *w, const struct bl_walk *k, const struct bl_way *way, int n)
{
	for (int i = 0; i < n; i++)
		if (!bl_walk_stands(w, k, way[i].x, way[i].y, way[i].z))
			return false;
	return true;
}

/* seconds until the unit is within 1.2 of (x, z), -1 when it is not after `secs` */
static float walk_to(struct bl_mobs *M, const struct bl_world *w, float x, float z, float secs)
{
	static const float nobody[3] = { 2.5f, 20.0f, 2.5f };
	bl_mobs_order_move(M, 0, x, z);
	for (int i = 0; i < (int)(secs * 30.0f); i++) {
		bl_mobs_step(M, w, nobody, false, 12.0f, 1.0f / 30.0f, NULL);
		if (hypotf(M->mob[0].x - x, M->mob[0].z - z) < 1.2f)
			return (float)i / 30.0f;
	}
	return -1.0f;
}

static void test_paths(void)
{
	static struct bl_mobs M;
	struct bl_world *w = bl_world_flat(96, 96, 64, 20);
	const struct bl_walk man = { false, 2 }, gob = { true, 2 };
	struct bl_way way[BL_PATH_MAX];
	bool full = false;
	int steps = 0, used = 0;
	float from[3] = { 40.5f, 20.0f, 35.5f }, to[3] = { 40.5f, 20.0f, 45.5f };
	CHECK(bl_walk_line(w, &man, from, to, false), "on open ground the straight line is open");
	CHECK(bl_walk_stands(w, &man, 40, 20, 35) && !bl_walk_stands(w, &man, 40, 21, 35) && !bl_walk_stands(w, &man, 40, 19, 35), "one stands on the ground, not in the air or in it");

	wall_x(w, 30, 52, 40, 3, BL_STONE);
	CHECK(!bl_walk_line(w, &man, from, to, false), "a wall closes the line");
	int n = bl_walk_find(w, &man, from, to, false, 0.4f, BL_PATH_NODES, way, BL_PATH_MAX, &full, &steps, &used);
	CHECK(n > 1 && full && way_stands(w, &man, way, n) && way[n - 1].x == 40 && way[n - 1].z == 45, "a way round it (%d turns, %d steps, %d places looked at)", n, steps, used);
	CHECK(steps > 20 && steps < 45, "no longer than it must be (%d steps)", steps);
	bl_mobs_init(&M, 1);
	bl_mobs_add(&M, w, BL_MOB_WORKER, 40.5f, 20.0f, 35.5f);
	float t = walk_to(&M, w, 40.5f, 45.5f, 40.0f);
	CHECK(t > 6.0f && t < 20.0f && M.paths >= 1, "a worker walks round the wall (%.1f s, %d searches)", t, M.paths);

	/* a second wall the other way: round both */
	wall_x(w, 20, 60, 43, 3, BL_STONE);
	wall_x(w, 20, 20, 41, 3, BL_STONE);
	wall_x(w, 20, 20, 42, 3, BL_STONE);
	bl_mobs_clear(&M);
	bl_mobs_add(&M, w, BL_MOB_WORKER, 40.5f, 20.0f, 35.5f);
	t = walk_to(&M, w, 40.5f, 45.5f, 60.0f);
	CHECK(t > 10.0f && t < 45.0f, "and round two (%.1f s)", t);

	/* shut in: it gives up and stands */
	wall_x(w, 20, 60, 33, 3, BL_STONE);
	for (int z = 33; z <= 40; z++) {
		wall_x(w, 20, 20, z, 3, BL_STONE);
		wall_x(w, 60, 60, z, 3, BL_STONE);
	}
	wall_x(w, 20, 60, 40, 3, BL_STONE);
	bl_mobs_clear(&M);
	bl_mobs_add(&M, w, BL_MOB_WORKER, 40.5f, 20.0f, 35.5f);
	n = bl_walk_find(w, &man, from, to, true, 0.4f, BL_PATH_NODES, way, BL_PATH_MAX, &full, NULL, NULL);
	CHECK(!full, "there is no way out of a closed yard");
	t = walk_to(&M, w, 40.5f, 45.5f, 50.0f);
	CHECK(t < 0.0f && M.mob[0].order == BL_ORDER_IDLE && M.mob[0].z < 40.0f, "a worker shut in gives up (order %d)", M.mob[0].order);
	/* a gate in the yard's wall lets him out, not a goblin */
	bl_set(w, 40, 20, 40, BL_GATE);
	bl_set(w, 40, 21, 40, BL_GATE);
	n = bl_walk_find(w, &man, from, to, true, 0.4f, BL_PATH_NODES, way, BL_PATH_MAX, &full, NULL, NULL);
	bool gate_full = full;
	bl_walk_find(w, &gob, from, to, true, 0.4f, BL_PATH_NODES, way, BL_PATH_MAX, &full, NULL, NULL);
	CHECK(gate_full && !full, "a way leads through a gate for the player's own, none for a goblin");
	bl_world_free(w);

	/* steps and drops */
	w = bl_world_flat(64, 64, 64, 20);
	for (int z = 0; z < 64; z++)
		for (int x = 32; x < 64; x++)
			for (int y = 20; y < 23; y++)
				bl_set(w, x, y, z, BL_STONE);                       /* a terrace three up from x = 32 */
	float low[3] = { 28.5f, 20.0f, 30.5f }, high[3] = { 36.5f, 23.0f, 30.5f };
	bl_walk_find(w, &man, high, low, false, 0.4f, BL_PATH_NODES, way, BL_PATH_MAX, &full, NULL, NULL);
	bool down = full;
	bl_walk_find(w, &man, low, high, false, 0.4f, 3000, way, BL_PATH_MAX, &full, NULL, NULL);
	CHECK(down && !full, "three down is a way, three up is none");
	bl_set(w, 31, 20, 36, BL_STONE);                            /* two steps up beside it */
	bl_set(w, 31, 20, 37, BL_STONE);
	bl_set(w, 31, 21, 37, BL_STONE);
	bl_set(w, 31, 20, 35, BL_AIR);
	n = bl_walk_find(w, &man, low, high, false, 0.4f, BL_PATH_NODES, way, BL_PATH_MAX, &full, NULL, NULL);
	CHECK(full && way_stands(w, &man, way, n), "stairs make a way up (%d turns)", n);
	bl_mobs_clear(&M);
	bl_mobs_add(&M, w, BL_MOB_WORKER, 28.5f, 20.0f, 30.5f);
	t = walk_to(&M, w, 36.5f, 30.5f, 40.0f);
	CHECK(t >= 0.0f && M.mob[0].y > 22.9f, "and he climbs them (%.1f s, y %.1f)", t, M.mob[0].y);
	bl_world_free(w);

	/* water: round a pond when that is not far, else across it and out on the other bank */
	w = bl_world_flat(96, 96, 64, 20);
	for (int z = 36; z <= 44; z++)
		for (int x = 36; x <= 44; x++)
			for (int y = 17; y < 20; y++)
				bl_set(w, x, y, z, BL_WATER);
	float pa[3] = { 40.5f, 20.0f, 32.5f }, pb[3] = { 40.5f, 20.0f, 48.5f };
	n = bl_walk_find(w, &man, pa, pb, false, 0.4f, BL_PATH_NODES, way, BL_PATH_MAX, &full, NULL, NULL);
	int wet_turns = 0;
	for (int i = 0; i < n; i++)
		wet_turns += bl_water(bl_get(w, way[i].x, way[i].y, way[i].z));
	CHECK(full && wet_turns == 0, "round a small pond (%d turns in the water)", wet_turns);
	for (int z = 36; z <= 40; z++)                              /* a river across the world */
		for (int x = 0; x < 96; x++)
			for (int y = 17; y < 20; y++)
				bl_set(w, x, y, z, BL_WATER);
	bl_mobs_clear(&M);
	bl_mobs_add(&M, w, BL_MOB_WORKER, 60.5f, 20.0f, 32.5f);
	t = walk_to(&M, w, 60.5f, 50.5f, 40.0f);
	CHECK(t >= 0.0f && M.mob[0].y > 19.9f, "across a river and out of it (%.1f s, at %.1f %.1f %.1f)", t, M.mob[0].x, M.mob[0].y, M.mob[0].z);
	bl_world_free(w);

	/* a goblin walks round a wall to the player and leaves it whole; a soldier goes round one to a goblin */
	w = bl_world_flat(96, 96, 64, 20);
	wall_x(w, 34, 46, 40, 2, BL_COBBLE);
	bl_mobs_init(&M, 2);
	M.cap = 0;
	bl_mobs_add(&M, w, BL_MOB_GOBLIN, 40.5f, 20.0f, 34.5f);
	float pl[3] = { 40.5f, 20.0f, 46.5f };
	int hits = 0, siege = 0;
	for (int i = 0; i < 30 * 20 && !hits; i++) {
		struct bl_mobs_result R;
		bl_mobs_step(&M, w, pl, true, 17.9f, 1.0f / 30.0f, &R);
		hits += R.hits;
		siege += R.siege_hits;
	}
	CHECK(hits > 0 && siege == 0, "a goblin goes round a short wall and gets the player, without a blow at it (%d, %d)", hits, siege);
	wall_x(w, 0, 95, 40, 3, BL_COBBLE);                         /* now there is no way round */
	bl_mobs_clear(&M);
	bl_mobs_add(&M, w, BL_MOB_GOBLIN, 40.5f, 20.0f, 34.5f);
	siege = 0;
	for (int i = 0; i < 30 * 20; i++) {
		struct bl_mobs_result R;
		bl_mobs_step(&M, w, pl, true, 17.9f, 1.0f / 30.0f, &R);
		siege += R.siege_hits;
	}
	CHECK(siege > 3, "with no way round it breaks in (%d blows)", siege);
	for (int y = 20; y < 23; y++)
		for (int x = 0; x < 96; x++)
			bl_set(w, x, y, 40, x >= 36 && x <= 46 && y < 22 ? BL_COBBLE : BL_AIR);
	bl_mobs_clear(&M);
	bl_mobs_add(&M, w, BL_MOB_SOLDIER, 41.5f, 20.0f, 37.5f);
	bl_mobs_add(&M, w, BL_MOB_GOBLIN, 41.5f, 20.0f, 43.5f);
	M.mob[1].hp = 1.0f;
	int slain = 0;
	for (int i = 0; i < 30 * 20 && !slain; i++) {
		struct bl_mobs_result R;
		bl_mobs_step(&M, w, from, false, 17.9f, 1.0f / 30.0f, &R);
		slain += R.slain + (!M.mob[1].alive);
	}
	CHECK(slain > 0, "a soldier gets to a goblin behind a wall");
	bl_world_free(w);

	/* odd input */
	w = bl_world_flat(32, 32, 48, 10);
	float bad_to[3] = { NAN, 10.0f, 5.0f }, out[3] = { -50.0f, 10.0f, 500.0f }, in[3] = { 5.5f, 10.0f, 5.5f }, sky[3] = { 5.5f, 45.0f, 5.5f };
	CHECK(bl_walk_find(w, &man, in, bad_to, false, 0.4f, 100, way, BL_PATH_MAX, NULL, NULL, NULL) == 0, "no way to a number that is none");
	bl_walk_find(w, &man, in, out, false, 0.4f, 100000, way, BL_PATH_MAX, &full, NULL, &used);
	CHECK(!full && used <= BL_PATH_NODES, "a goal outside the world: none, within the budget (%d)", used);
	CHECK(bl_walk_find(w, &man, sky, in, false, 0.4f, 100, way, BL_PATH_MAX, NULL, NULL, NULL) == 0 && bl_walk_find(w, &man, in, in, false, 0.4f, 100, way, 0, NULL, NULL, NULL) == 0, "nor from the air, nor into no room");
	bl_world_free(w);
}

/* ---- the rival clan's guards -------------------------------------------------------------------------------------------- */

static void guard_steps(struct bl_mobs *M, const struct bl_world *w, const float pl[3], bool alive, float secs, struct bl_mobs_result *sum)
{
	memset(sum, 0, sizeof *sum);
	for (int i = 0; i < (int)(secs * 30.0f); i++) {
		struct bl_mobs_result R;
		bl_mobs_step(M, w, pl, alive, 12.0f, 1.0f / 30.0f, &R);
		sum->damage += R.damage;
		sum->hits += R.hits;
		sum->guard_hits += R.guard_hits;
		sum->guards_slain += R.guards_slain;
		sum->soldiers_lost += R.soldiers_lost;
		sum->workers_lost += R.workers_lost;
		sum->slain += R.slain;
		sum->rkeep_damage += R.rkeep_damage;
		sum->shots += R.shots;
	}
}

static void test_rival(void)
{
	static struct bl_mobs M;
	struct bl_world *w = bl_world_flat(128, 128, 64, 20);
	const float away[3] = { 5.5f, 20.0f, 5.5f };
	struct bl_mobs_result S;
	bl_mobs_init(&M, 4);
	M.enabled = false;
	int n = 0;
	for (int i = 0; i < 12; i++)
		n += bl_mobs_add(&M, w, BL_MOB_GUARD, 20.5f + (float)i * 2.0f, 20.0f, 100.5f);
	CHECK(n == BL_GUARD_MAX && bl_mob_is_foe(BL_MOB_GUARD) && !bl_mob_is_unit(BL_MOB_GUARD), "eleven of the camp at most (eight on the ground, three archers); they are foes, not the player's own (%d)", n);
	CHECK(bl_mobs_hit_cell(&M, 20, 21, 100), "no block is put into one");
	float o[3] = { 20.5f, 21.0f, 97.5f }, dir[3] = { 0.0f, 0.0f, 1.0f }, t = 0.0f;
	CHECK(bl_mobs_pick(&M, o, dir, 4.0f, &t) == 0 && bl_units_pick(&M, o, dir, 4.0f, &t) < 0, "the sword finds a guard, the command view does not choose him");

	/* he goes for the player who comes near and hits him; one far off he leaves alone; then back to his post */
	bl_mobs_clear(&M);
	bl_mobs_add(&M, w, BL_MOB_GUARD, 60.5f, 20.0f, 60.5f);
	float pl[3] = { 60.5f, 20.0f, 80.5f };
	guard_steps(&M, w, pl, true, 3.0f, &S);
	CHECK(S.hits == 0 && fabsf(M.mob[0].z - 60.5f) < 0.5f, "a player 20 blocks off is not seen");
	pl[2] = 69.5f;
	guard_steps(&M, w, pl, true, 8.0f, &S);
	CHECK(S.guard_hits >= 3 && S.damage == BL_GUARD_HIT * (float)S.guard_hits && S.hits == S.guard_hits, "9 blocks off he comes and strikes, BL_GUARD_HIT a blow (%d blows, %.0f)", S.guard_hits, S.damage);
	guard_steps(&M, w, away, false, 8.0f, &S);
	CHECK(hypotf(M.mob[0].x - 60.5f, M.mob[0].z - 60.5f) < 1.5f, "and goes back to his post (%.1f %.1f)", M.mob[0].x, M.mob[0].z);

	/* against the player's own: soldier and guard fight; the dead are counted on the right side */
	bl_mobs_clear(&M);
	bl_mobs_add(&M, w, BL_MOB_GUARD, 60.5f, 20.0f, 60.5f);
	bl_mobs_add(&M, w, BL_MOB_SOLDIER, 60.5f, 20.0f, 66.5f);
	bl_mobs_add(&M, w, BL_MOB_SOLDIER, 62.5f, 20.0f, 66.5f);
	guard_steps(&M, w, away, false, 12.0f, &S);
	CHECK(S.guards_slain == 1 && S.slain == 0 && bl_mobs_count_kind(&M, BL_MOB_GUARD) == 0 && M.kills == 0, "two soldiers bring a guard down: not a goblin kill (%d)", S.guards_slain);
	bl_mobs_clear(&M);
	bl_mobs_add(&M, w, BL_MOB_GUARD, 60.5f, 20.0f, 60.5f);
	bl_mobs_add(&M, w, BL_MOB_WORKER, 60.5f, 20.0f, 66.5f);
	guard_steps(&M, w, away, false, 12.0f, &S);
	CHECK(S.workers_lost == 1, "a guard kills a worker");
	bl_mobs_clear(&M);
	M.enabled = true;
	M.cap = 0;
	bl_mobs_add(&M, w, BL_MOB_GUARD, 60.5f, 20.0f, 60.5f);
	bl_mobs_add(&M, w, BL_MOB_GOBLIN, 60.5f, 20.0f, 66.5f);
	for (int i = 0; i < 30 * 20; i++)
		bl_mobs_step(&M, w, away, false, 17.9f, 1.0f / 30.0f, NULL);
	CHECK(bl_mobs_count(&M) == 1 && M.kills == 0, "a goblin and a guard fight to the death, and it is no kill of the player's (%d left)", bl_mobs_count(&M));
	M.enabled = false;

	/* the keep of the clan: a soldier sent there strikes it, when no enemy is near */
	bl_mobs_clear(&M);
	for (int y = 20; y < 23; y++)
		bl_set(w, 90, y, 90, BL_REDKEEP);
	bl_mobs_set_rival(&M, true, 90.5f, 20.0f, 90.5f);
	bl_mobs_add(&M, w, BL_MOB_SOLDIER, 90.5f, 20.0f, 20.5f);
	guard_steps(&M, w, away, false, 2.0f, &S);
	CHECK(S.rkeep_damage == 0.0f, "not before he is sent");
	bl_mobs_order_move(&M, 0, 90.5f, 89.5f);
	guard_steps(&M, w, away, false, 40.0f, &S);
	CHECK(S.rkeep_damage >= 40.0f && fmodf(S.rkeep_damage, BL_SOLDIER_KEEP_HIT) == 0.0f, "sent 70 blocks, he gets there and strikes it (%.0f)", S.rkeep_damage);
	bl_mobs_set_rival(&M, false, 90.5f, 20.0f, 90.5f);
	guard_steps(&M, w, away, false, 5.0f, &S);
	CHECK(S.rkeep_damage == 0.0f, "no blows when the clan is off");
	CHECK(!bl_breakable(BL_REDKEEP) && bl_solid(BL_REDKEEP) && !strcmp(bl_block_name(BL_REDKEEP), "redkeep") && bl_siege_hp(BL_REDKEEP) == 0, "their keep is a block no hand takes away");

	/* an archer of theirs on a watchtower shoots the player and his soldiers */
	bl_mobs_clear(&M);
	bl_set(w, 40, 24, 40, BL_TOWERPOST);
	CHECK(bl_mobs_add_archer(&M, w, 40, 24, 40) && !bl_mobs_add_archer(&M, w, 41, 24, 40), "an archer stands on a watchpost, nowhere else");
	float near_pl[3] = { 48.5f, 20.0f, 40.5f };
	guard_steps(&M, w, near_pl, true, 5.0f, &S);
	CHECK(S.shots >= 3 && S.guard_hits == S.shots && S.damage == BL_BOW_DAMAGE * (float)S.shots, "and shoots the player (%d shots, %.0f)", S.shots, S.damage);
	bl_mobs_add(&M, w, BL_MOB_WORKER, 45.5f, 20.0f, 40.5f);
	guard_steps(&M, w, away, false, 6.0f, &S);
	CHECK(S.workers_lost == 1, "or a worker");

	/* a gate stops a guard as it stops a goblin */
	bl_mobs_clear(&M);
	for (int x = 0; x < 128; x++)
		for (int y = 20; y < 23; y++)
			bl_set(w, x, y, 62, x == 60 && y < 22 ? BL_GATE : BL_STONE);
	bl_mobs_add(&M, w, BL_MOB_GUARD, 60.5f, 20.0f, 58.5f);
	float beyond[3] = { 60.5f, 20.0f, 66.5f };
	guard_steps(&M, w, beyond, true, 8.0f, &S);
	CHECK(S.hits == 0 && M.mob[0].z < 62.0f, "he does not pass the player's gate (z %.1f)", M.mob[0].z);
	bl_world_free(w);
}

/* run until no raider is left (or `secs`), adding up what happens; returns the seconds it took */
static float raid_steps(struct bl_mobs *M, struct bl_world *w, const float pl[3], bool alive, float secs, struct bl_mobs_result *S)
{
	memset(S, 0, sizeof *S);
	float t = 0.0f;
	for (; t < secs && bl_mobs_count_raiders(M) > 0; t += 1.0f / 30.0f) {
		struct bl_mobs_result R;
		bl_mobs_step(M, w, pl, alive, 12.0f, 1.0f / 30.0f, &R);
		for (int k = 0; k < R.nsmash; k++)
			bl_set(w, R.smash[k].x, R.smash[k].y, R.smash[k].z, BL_AIR);
		S->keep_damage += R.keep_damage;
		S->damage += R.damage;
		S->hits += R.hits;
		S->guards_slain += R.guards_slain;
		S->raiders_slain += R.raiders_slain;
		S->raiders_home += R.raiders_home;
		S->workers_lost += R.workers_lost;
		S->siege_hits += R.siege_hits;
	}
	return t;
}

static void test_raiders(void)
{
	static struct bl_mobs M;
	struct bl_world *w = bl_world_flat(128, 128, 64, 20);
	const float away[3] = { 5.5f, 20.0f, 5.5f };
	struct bl_mobs_result S;
	bl_mobs_init(&M, 7);
	M.enabled = false;

	/* counted apart from the camp's guards, with a cap of their own */
	int camp = 0, out = 0;
	for (int i = 0; i < 12; i++)
		camp += bl_mobs_add(&M, w, BL_MOB_GUARD, 20.5f + (float)i * 2.0f, 20.0f, 100.5f);
	for (int i = 0; i < 12; i++)
		out += bl_mobs_add_raider(&M, w, 20.5f + (float)i * 2.0f, 20.0f, 104.5f);
	CHECK(camp == BL_GUARD_MAX && out == BL_RAIDER_MAX && bl_mobs_count_raiders(&M) == BL_RAIDER_MAX, "the camp's guards and six raiders (%d, %d)", camp, out);

	/* they march 60 blocks to the player's keep and strike it; goblins' target and theirs is the same block */
	bl_mobs_clear(&M);
	bl_mobs_set_rival(&M, true, 64.5f, 20.0f, 110.5f);
	for (int y = 20; y < 23; y++)
		bl_set(w, 64, y, 30, BL_KEEP);
	bl_mobs_set_keep(&M, true, 64.5f, 20.0f, 30.5f);
	bl_mobs_set_raid(&M, 64.5f, 20.0f, 30.5f);
	bl_mobs_add_raider(&M, w, 64.5f, 20.0f, 90.5f);
	raid_steps(&M, w, away, false, 18.0f, &S);
	CHECK(S.keep_damage == 0.0f && M.mob[0].z < 50.0f, "on the way after 18 s (z %.1f)", M.mob[0].z);
	raid_steps(&M, w, away, false, 15.0f, &S);
	CHECK(S.keep_damage >= 10.0f && fmodf(S.keep_damage, BL_RAIDER_KEEP_HIT) == 0.0f, "and strikes the keep (%.0f)", S.keep_damage);

	/* a wall in the way with no way round: broken; a gate does not let them through */
	bl_mobs_clear(&M);
	for (int x = 0; x < 128; x++)
		for (int y = 20; y < 22; y++)
			bl_set(w, x, y, 50, x == 64 ? BL_GATE : BL_PLANKS);
	bl_mobs_add_raider(&M, w, 64.5f, 20.0f, 62.5f);
	raid_steps(&M, w, away, false, 60.0f, &S);
	CHECK(S.siege_hits > 5 && S.keep_damage > 0.0f, "he breaks through and gets to the keep (%d blows at the wall, %.0f at the keep)", S.siege_hits, S.keep_damage);
	for (int x = 0; x < 128; x++)
		for (int y = 20; y < 22; y++)
			bl_set(w, x, y, 50, BL_AIR);

	/* they fight whom they meet; the dead are counted as raiders */
	bl_mobs_clear(&M);
	bl_mobs_add_raider(&M, w, 64.5f, 20.0f, 70.5f);
	bl_mobs_add(&M, w, BL_MOB_SOLDIER, 64.5f, 20.0f, 62.5f);
	bl_mobs_add(&M, w, BL_MOB_SOLDIER, 66.5f, 20.0f, 62.5f);
	for (int i = 0; i < 30 * 15; i++) {
		struct bl_mobs_result R;
		bl_mobs_step(&M, w, away, false, 12.0f, 1.0f / 30.0f, &R);
		S.raiders_slain += R.raiders_slain;
	}
	CHECK(bl_mobs_count_raiders(&M) == 0 && S.raiders_slain == 1, "two soldiers stop one (%d)", S.raiders_slain);
	/* a camp guard and a raider are friends */
	bl_mobs_clear(&M);
	bl_mobs_add(&M, w, BL_MOB_GUARD, 64.5f, 20.0f, 108.5f);
	bl_mobs_add_raider(&M, w, 64.5f, 20.0f, 105.5f);
	raid_steps(&M, w, away, false, 5.0f, &S);
	CHECK(bl_mobs_count(&M) == 2 && M.mob[0].hp == BL_GUARD_HP && M.mob[1].hp == BL_GUARD_HP, "they do not fight each other");

	/* no keep to strike (creative): they come for the player near the goal, and with nobody there go home */
	bl_mobs_clear(&M);
	bl_mobs_set_keep(&M, false, 64.5f, 20.0f, 30.5f);
	bl_mobs_add_raider(&M, w, 64.5f, 20.0f, 90.5f);
	float pl[3] = { 90.5f, 20.0f, 30.5f };                         /* 26 blocks from the goal: hunted */
	raid_steps(&M, w, pl, true, 45.0f, &S);
	CHECK(S.hits > 3 && S.keep_damage == 0.0f, "he finds the player and the keep is left alone (%d blows)", S.hits);
	bl_mobs_clear(&M);
	bl_mobs_add_raider(&M, w, 64.5f, 20.0f, 90.5f);
	float t = raid_steps(&M, w, away, false, 400.0f, &S);
	CHECK(S.raiders_home == 1 && S.keep_damage == 0.0f && t > BL_RAID_IDLE + 30.0f && t < 200.0f, "nobody there: he waits and goes home (%.0f s)", t);

	/* told to turn back, he walks home and is gone near his keep */
	bl_mobs_clear(&M);
	bl_mobs_set_keep(&M, true, 64.5f, 20.0f, 30.5f);
	bl_mobs_add_raider(&M, w, 64.5f, 20.0f, 60.5f);
	bl_mobs_raid_retreat(&M);
	raid_steps(&M, w, away, false, 10.0f, &S);
	CHECK(S.raiders_home == 0 && M.mob[0].z > 80.0f, "on his way back (z %.1f)", M.mob[0].z);
	raid_steps(&M, w, away, false, 30.0f, &S);
	CHECK(S.raiders_home == 1 && S.keep_damage == 0.0f && bl_mobs_count(&M) == 0, "home");

	/* shut in by the land itself he gives up and is gone: no party stays for ever */
	bl_mobs_clear(&M);
	for (int d = -3; d <= 3; d++)
		for (int y = 20; y < 24; y++) {
			bl_set(w, 64 + d, y, 87, BL_STONE);
			bl_set(w, 64 + d, y, 93, BL_STONE);
			bl_set(w, 61, y, 90 + d, BL_STONE);
			bl_set(w, 67, y, 90 + d, BL_STONE);
		}
	bl_mobs_add_raider(&M, w, 64.5f, 20.0f, 90.5f);
	t = raid_steps(&M, w, away, false, BL_RAID_LIMIT + BL_RAID_BACK + 10.0f, &S);
	CHECK(S.raiders_home == 1 && t < 3.0f * BL_RAID_IDLE + 10.0f, "gone after %.0f s", t);
	bl_world_free(w);
}

/* the labourers' steps: what they take from the land goes, what they put is set; *s adds up */
static void camp_steps(struct bl_mobs *M, struct bl_world *w, const float pl[3], bool alive, float secs, struct bl_mobs_result *S)
{
	memset(S, 0, sizeof *S);
	for (int i = 0; i < (int)(secs * 30.0f); i++) {
		struct bl_mobs_result R;
		bl_mobs_step(M, w, pl, alive, 12.0f, 1.0f / 30.0f, &R);
		for (int k = 0; k < R.nraze; k++)
			bl_set(w, R.raze[k].x, R.raze[k].y, R.raze[k].z, BL_AIR);
		for (int k = 0; k < R.nplace; k++)
			if (bl_cell_missing(w, &R.place[k]))
				bl_set(w, R.place[k].x, R.place[k].y, R.place[k].z, R.place[k].block);
		S->nraze += R.nraze;
		S->nplace += R.nplace;
		S->delivered += R.delivered;
		S->rbuilt += R.rbuilt;
		S->rworkers_slain += R.rworkers_slain;
		S->guards_slain += R.guards_slain;
		S->hits += R.hits;
		int building = 0;
		for (int k = 0; k < BL_MOBS_MAX; k++)
			building += M->mob[k].alive && M->mob[k].kind == BL_MOB_RWORKER && M->mob[k].order == BL_ORDER_BUILD;
		if (building > S->shots)                    /* (here: the most builders at a time) */
			S->shots = building;
	}
}

static void put_tree(struct bl_world *w, int x, int z)
{
	for (int dy = 3; dy <= 4; dy++)
		for (int dz = -1; dz <= 1; dz++)
			for (int dx = -1; dx <= 1; dx++)
				bl_set(w, x + dx, 20 + dy, z + dz, BL_LEAVES);
	for (int y = 20; y < 24; y++)
		bl_set(w, x, y, z, BL_LOG);
}

static int count_blocks(const struct bl_world *w, int block)
{
	int n = 0;
	for (int x = 0; x < w->w; x++)
		for (int z = 0; z < w->d; z++)
			for (int y = 18; y < 30; y++)
				n += bl_get(w, x, y, z) == block;
	return n;
}

static void test_rival_workers(void)
{
	static struct bl_mobs M;
	struct bl_world *w = bl_world_flat(128, 128, 64, 20);
	const float away[3] = { 5.5f, 20.0f, 5.5f };
	struct bl_mobs_result S;
	bl_mobs_init(&M, 11);
	M.enabled = false;
	for (int y = 20; y < 23; y++)
		bl_set(w, 64, y, 64, BL_REDKEEP);
	bl_mobs_set_rival(&M, true, 64.5f, 20.0f, 64.5f);

	int n = 0;
	for (int i = 0; i < 6; i++)
		n += bl_mobs_add(&M, w, BL_MOB_RWORKER, 60.5f + (float)i, 20.0f, 66.5f);
	CHECK(n == BL_RWORKER_MAX && bl_mob_is_foe(BL_MOB_RWORKER) && bl_mob_is_rival(BL_MOB_RWORKER) && !bl_mob_is_unit(BL_MOB_RWORKER), "four of their workers at most; foes (%d)", n);
	float o[3] = { 60.5f, 20.8f, 62.5f }, dir[3] = { 0.0f, 0.0f, 1.0f }, t = 0.0f;
	struct bl_mob_hit h;
	bl_mobs_light(&M, w, 12.0f);
	CHECK(bl_mobs_pick(&M, o, dir, 6.0f, &t) == 0 && bl_units_pick(&M, o, dir, 6.0f, &t) < 0 && bl_mobs_hit_cell(&M, 60, 20, 66) && bl_mobs_ray(&M, o, dir, 10.0f, &h) &&
	      h.rgb[0] > 2.0f * h.rgb[1], "the sword and the picture find one, in a red tunic; the command view does not choose him");

	/* a tree is felled and brought home; a log somebody has put down is no tree */
	bl_mobs_clear(&M);
	put_tree(w, 90, 64);
	bl_set(w, 40, 20, 64, BL_LOG);
	bl_mobs_add(&M, w, BL_MOB_RWORKER, 66.5f, 20.0f, 64.5f);
	camp_steps(&M, w, away, false, 45.0f, &S);
	CHECK(S.delivered == BL_RWORKER_TREE_LOAD && count_blocks(w, BL_LEAVES) == 0 && count_blocks(w, BL_LOG) == 1 && bl_get(w, 40, 20, 64) == BL_LOG,
	      "the tree with its crown, 25 blocks off, is in the store (%d)", S.delivered);

	/* then the stone that sticks out of the ground, but none in the camp */
	bl_set(w, 64, 20, 90, BL_STONE);
	bl_set(w, 64, 20, 91, BL_STONE);
	bl_set(w, 70, 20, 70, BL_STONE);
	camp_steps(&M, w, away, false, 90.0f, &S);
	CHECK(S.delivered >= 2 && bl_get(w, 64, 20, 90) == BL_AIR && bl_get(w, 64, 20, 91) == BL_AIR && bl_get(w, 70, 20, 70) == BL_STONE, "stone is cut, the camp's own stays (%d)", S.delivered);

	/* with nothing to fell or cut they forage: it brings material and changes nothing */
	camp_steps(&M, w, away, false, 120.0f, &S);
	CHECK(S.delivered >= 2 && S.nraze == 0 && bl_mobs_count_kind(&M, BL_MOB_RWORKER) == 1, "foraging (%d in two minutes)", S.delivered);

	/* an enemy near: home, and no work; gone: back to it */
	bl_mobs_clear(&M);
	put_tree(w, 90, 64);
	bl_mobs_add(&M, w, BL_MOB_RWORKER, 86.5f, 20.0f, 64.5f);
	float pl[3] = { 90.5f, 20.0f, 68.5f };
	camp_steps(&M, w, pl, true, 6.0f, &S);
	CHECK(M.mob[0].x < 76.0f && S.hits == 0, "he runs for the keep and strikes nobody (x %.1f)", M.mob[0].x);
	camp_steps(&M, w, pl, true, 40.0f, &S);
	CHECK(S.nraze == 0 && bl_get(w, 90, 20, 64) == BL_LOG, "and leaves the tree beside the player alone");
	camp_steps(&M, w, away, false, 90.0f, &S);
	CHECK(bl_get(w, 90, 20, 64) == BL_AIR && S.delivered >= BL_RWORKER_TREE_LOAD, "and fetches it when the player is gone (%d)", S.delivered);

	/* four of them build what the camp is building, two at a time, and it is said once */
	bl_mobs_clear(&M);
	for (int i = 0; i < 4; i++)
		bl_mobs_add(&M, w, BL_MOB_RWORKER, 62.5f + (float)i, 20.0f, 66.5f);
	struct bl_site site;
	const char *why = NULL;
	CHECK(bl_plan_lay(w, BL_PLAN_TOWER, 0, 67, 20, 67, NULL, 0, &site, &why), "room for a tower");
	bl_mobs_set_rsite(&M, &site);
	camp_steps(&M, w, away, false, 60.0f, &S);
	CHECK(S.rbuilt == 1 && !M.rsite.on && bl_site_missing(w, &site) == 0 && S.shots >= 1 && S.shots <= BL_RWORKER_BUILDERS && S.delivered > 0,
	      "the tower stands (%d missing, %d builders, the others fetched %d)", bl_site_missing(w, &site), S.shots, S.delivered);

	/* a guard of theirs leaves them alone; the player's soldiers do not */
	bl_mobs_clear(&M);
	bl_mobs_add(&M, w, BL_MOB_RWORKER, 80.5f, 20.0f, 40.5f);
	bl_mobs_add(&M, w, BL_MOB_GUARD, 80.5f, 20.0f, 43.5f);
	camp_steps(&M, w, away, false, 3.0f, &S);
	CHECK(M.mob[0].hp == BL_RWORKER_HP && M.mob[1].hp == BL_GUARD_HP, "friends");
	M.mob[1].alive = false;
	bl_mobs_add(&M, w, BL_MOB_SOLDIER, M.mob[0].x + 2.0f, 20.0f, M.mob[0].z);
	camp_steps(&M, w, away, false, 25.0f, &S);
	CHECK(S.rworkers_slain == 1 && S.guards_slain == 0 && M.kills == 0, "a soldier brings one down: counted as a worker (%d)", S.rworkers_slain);

	/* switched off they stand; shut in, one gives up in the end and is gone */
	bl_mobs_clear(&M);
	bl_mobs_set_rival(&M, false, 64.5f, 20.0f, 64.5f);
	bl_mobs_add(&M, w, BL_MOB_RWORKER, 66.5f, 20.0f, 60.5f);
	camp_steps(&M, w, away, false, 5.0f, &S);
	CHECK(S.delivered == 0 && fabsf(M.mob[0].x - 66.5f) < 0.1f, "switched off: nothing");
	bl_mobs_clear(&M);
	bl_mobs_set_rival(&M, true, 64.5f, 20.0f, 64.5f);
	for (int d = -3; d <= 3; d++)
		for (int y = 20; y < 24; y++) {
			bl_set(w, 30 + d, y, 27, BL_BRICK);
			bl_set(w, 30 + d, y, 33, BL_BRICK);
			bl_set(w, 27, y, 30 + d, BL_BRICK);
			bl_set(w, 33, y, 30 + d, BL_BRICK);
		}
	bl_mobs_add(&M, w, BL_MOB_RWORKER, 30.5f, 20.0f, 30.5f);
	camp_steps(&M, w, away, false, 600.0f, &S);
	CHECK(bl_mobs_count(&M) == 0 && S.delivered == 0, "one who is shut in is gone after a while (%d left)", bl_mobs_count(&M));
	bl_world_free(w);
}

static void test_workers(void)
{
	const float far_pl[3] = { 64.5f, 20.0f, 110.0f };
	struct bl_world *w = bl_world_flat(128, 128, 64, 20);
	struct bl_mobs M;
	bl_mobs_init(&M, 11);
	M.spawn_t = 1e9f;

	/* putting them, and counting */
	CHECK(bl_mobs_add(&M, w, BL_MOB_WORKER, 50.5f, 20.0f, 50.5f) && M.mob[0].kind == BL_MOB_WORKER && M.mob[0].hp == BL_WORKER_HP, "a worker, with six hit points");
	CHECK(!bl_mobs_add(&M, w, BL_MOB_WORKER, 51.5f, 19.5f, 50.5f), "not in the ground");
	for (int i = 1; i < BL_WORKER_MAX; i++)
		bl_mobs_add(&M, w, BL_MOB_WORKER, 50.5f + (float)i, 20.0f, 50.5f);
	CHECK(bl_mobs_count_kind(&M, BL_MOB_WORKER) == BL_WORKER_MAX && !bl_mobs_add(&M, w, BL_MOB_WORKER, 50.5f, 20.0f, 53.0f), "eight at most");
	CHECK(bl_mobs_add(&M, w, BL_MOB_GOBLIN, 70.5f, 20.0f, 70.5f) && bl_mobs_count_kind(&M, BL_MOB_GOBLIN) == 1 && bl_mobs_count(&M) == BL_WORKER_MAX + 1, "a goblin still goes in beside them");
	CHECK(bl_mobs_hit_cell(&M, 50, 20, 50) && bl_mobs_hit_cell(&M, 50, 21, 50) && !bl_mobs_hit_cell(&M, 50, 22, 50), "a block cannot be put where a worker stands (1.56 m)");
	/* the sword does not find them; the pointer does */
	float eye[3] = { 50.5f, 21.0f, 47.0f }, look[3] = { 0, 0, 1 }, t = 0;
	CHECK(bl_mobs_pick(&M, eye, look, BL_SWORD_REACH, &t) < 0, "the sword goes through a worker");
	float hi[3] = { 50.5f, 40.0f, 47.0f }, down[3] = { 0.0f, -1.0f, 0.2f };
	CHECK(bl_units_pick(&M, eye, look, 20.0f, &t) == 0 && fabsf(t - (3.5f - BL_GOBLIN_HW - 0.05f)) < 0.01f, "the pointer finds it (%.2f)", t);
	CHECK(bl_units_pick(&M, hi, down, 60.0f, &t) >= 0, "also from high above (the command view)");
	float side[3] = { -0.3f, 0, 1 };
	CHECK(bl_units_pick(&M, eye, side, 20.0f, &t) < 0 && bl_units_pick(&M, (float[3]){ 70.5f, 21.0f, 67.0f }, look, 20.0f, &t) < 0, "a miss is a miss, and a goblin is no worker");
	bl_mobs_clear_kind(&M, BL_MOB_GOBLIN);
	CHECK(bl_mobs_count_kind(&M, BL_MOB_GOBLIN) == 0 && bl_mobs_count_kind(&M, BL_MOB_WORKER) == BL_WORKER_MAX, "clear_kind takes one kind");
	bl_mobs_clear(&M);

	/* a move order: it walks there, at no more than 2.2 m/s, and is idle when it has come; a wall of two gives it up after three seconds, one block high it climbs */
	{
		bl_mobs_add(&M, w, BL_MOB_WORKER, 50.5f, 20.0f, 50.5f);
		bl_mobs_order_move(&M, 0, 60.5f, 56.5f);
		CHECK(M.mob[0].order == BL_ORDER_MOVE, "ordered");
		float tt = 0, arrived = 0, vmax = 0;
		int bad = 0;
		for (int i = 0; i < 30 * 12; i++) {
			float ox = M.mob[0].x, oz = M.mob[0].z;
			bl_mobs_step(&M, w, far_pl, true, 12.0f, 1.0f / 30.0f, NULL);
			tt += 1.0f / 30.0f;
			float v = hypotf(M.mob[0].x - ox, M.mob[0].z - oz) * 30.0f;
			vmax = v > vmax ? v : vmax;
			bad += box_in_solid(w, M.mob[0].x, M.mob[0].y, M.mob[0].z, BL_GOBLIN_HW, BL_WORKER_H);
			if (!arrived && M.mob[0].order == BL_ORDER_IDLE)
				arrived = tt;
		}
		CHECK(arrived > 3.5f && arrived < 9.0f && hypotf(M.mob[0].x - 60.5f, M.mob[0].z - 56.5f) < 0.7f && bad == 0, "it walks 11.7 m and stops there (%.1f s, %.2f m off)", arrived, hypotf(M.mob[0].x - 60.5f, M.mob[0].z - 56.5f));
		CHECK(vmax < 2.4f && vmax > 1.5f, "at about 2.2 m/s (%.2f)", vmax);
		for (int x = 0; x < 128; x++)
			for (int y = 20; y < 22; y++)
				bl_set(w, x, y, 62, BL_STONE);
		bl_mobs_order_move(&M, 0, 60.5f, 70.5f);
		for (int i = 0; i < 30 * 14; i++)
			bl_mobs_step(&M, w, far_pl, true, 12.0f, 1.0f / 30.0f, NULL);
		CHECK(M.mob[0].order == BL_ORDER_IDLE && M.mob[0].z < 62.0f - BL_GOBLIN_HW + 0.05f, "a wall of two blocks: it gives up (z %.2f)", M.mob[0].z);
		for (int x = 0; x < 128; x++)
			bl_set(w, x, 21, 62, BL_AIR);
		bl_mobs_order_move(&M, 0, 60.5f, 70.5f);
		for (int i = 0; i < 30 * 12; i++)
			bl_mobs_step(&M, w, far_pl, true, 12.0f, 1.0f / 30.0f, NULL);
		CHECK(hypotf(M.mob[0].x - 60.5f, M.mob[0].z - 70.5f) < 0.8f, "one block high it climbs and gets there");
		for (int x = 0; x < 128; x++)
			bl_set(w, x, 20, 62, BL_AIR);
		bl_mobs_order_stop(&M, 0);
		bl_mobs_order_move(&M, 0, 60.5f, 75.5f);
		bl_mobs_order_stop(&M, 0);
		CHECK(M.mob[0].order == BL_ORDER_IDLE, "stop");
		bl_mobs_order_move(&M, 5, 1, 1);
		bl_mobs_order_move(&M, -1, 1, 1);
		CHECK(true, "orders to numbers that are no workers do nothing");
	}

	/* gathering: it stands beside a block, takes 1.5 times the player's time, and goes on to the next of its kind */
	{
		bl_mobs_clear(&M);
		for (int x = 60; x <= 62; x++)
			bl_set(w, x, 20, 90, BL_STONE);
		bl_set(w, 60, 20, 95, BL_DIRT);                                       /* another kind, nearby: not touched */
		bl_mobs_add(&M, w, BL_MOB_WORKER, 55.5f, 20.0f, 90.5f);
		CHECK(bl_mobs_order_gather(&M, w, 0, 60, 20, 90), "a gather order on a stone");
		CHECK(!bl_mobs_order_gather(&M, w, 0, 60, 30, 90) && !bl_mobs_order_gather(&M, w, 0, 60, 5, 90) && !bl_mobs_order_gather(&M, w, 0, 60, 22, 90), "not on air, in the ground out of reach or in the air");
		CHECK(!bl_mobs_order_gather(&M, w, 3, 60, 20, 90) && !bl_mobs_order_gather(&M, w, -1, 60, 20, 90), "not for a number that is no worker");
		struct bl_world *wb = bl_world_flat(64, 64, 64, 20);
		bl_mobs_add(&M, w, BL_MOB_GOBLIN, 100.5f, 20.0f, 100.5f);
		CHECK(!bl_mobs_order_gather(&M, w, 1, 60, 20, 90), "a goblin takes no orders");
		bl_mobs_clear_kind(&M, BL_MOB_GOBLIN);
		bl_world_free(wb);
		struct bl_hit dummy = { .hit = true, .x = 60, .y = 0, .z = 60 };
		CHECK(!bl_mobs_order_gather(&M, w, 0, 60, 0, 60) && !bl_break(w, &dummy), "bedrock is not gathered");
		int in_block = 0;
		float first = 0;
		int n = work_steps(&M, w, far_pl, 9.0f, 12.0f, &in_block, &first);
		CHECK(n == 1 && bl_get(w, 60, 20, 90) == BL_AIR && first >= 3.0f && first <= 7.0f, "9 s: one stone is gone, after the walk and 3 s of work (%d, first at %.1f s)", n, first);
		n += work_steps(&M, w, far_pl, 12.0f, 12.0f, &in_block, NULL);
		CHECK(n == 3 && bl_get(w, 61, 20, 90) == BL_AIR && bl_get(w, 62, 20, 90) == BL_AIR, "then the next two of its kind");
		CHECK(bl_get(w, 60, 20, 95) == BL_DIRT, "and not the dirt");
		work_steps(&M, w, far_pl, 3.0f, 12.0f, &in_block, NULL);
		CHECK(M.mob[0].order == BL_ORDER_IDLE && in_block == 0, "nothing left: it is idle (and never was in a block)");
	}

	/* a tree: its foot is taken wherever it was ordered, the whole trunk goes with it and the leaves round its crown; a second tree within 6 blocks after that */
	{
		bl_mobs_clear(&M);
		for (int tx = 80; tx <= 84; tx += 4) {
			for (int y = 20; y <= 24; y++)
				bl_set(w, tx, y, 100, BL_LOG);
			for (int ly = 22; ly <= 25; ly++)
				for (int dz = -2; dz <= 2; dz++)
					for (int dx = -2; dx <= 2; dx++)
						if (bl_get(w, tx + dx, ly, 100 + dz) == BL_AIR)
							bl_set(w, tx + dx, ly, 100 + dz, BL_LEAVES);
		}
		long leaves0 = 0;
		for (int y = 20; y < 28; y++)
			for (int z = 95; z < 106; z++)
				for (int x = 76; x < 90; x++)
					leaves0 += bl_get(w, x, y, z) == BL_LEAVES;
		bl_mobs_add(&M, w, BL_MOB_WORKER, 77.5f, 20.0f, 102.5f);
		CHECK(bl_mobs_order_gather(&M, w, 0, 80, 22, 100) && M.mob[0].gy == 20, "ordered on the middle of the trunk it goes for the foot");
		int in_block = 0, total = 0;
		float first = 0;
		total += work_steps(&M, w, far_pl, 6.0f, 12.0f, &in_block, &first);
		CHECK(total >= 5 && bl_get(w, 80, 20, 100) == BL_AIR && bl_get(w, 80, 24, 100) == BL_AIR, "the whole trunk goes at once (%d blocks, at %.1f s)", total, first);
		CHECK(first >= 4.0f && first <= 6.0f, "after 1.8 s and 0.6 s a log more (%.1f s with the walk)", first);
		long leaves1 = 0;
		for (int y = 20; y < 28; y++)
			for (int z = 95; z < 106; z++)
				for (int x = 76; x < 82; x++)
					leaves1 += bl_get(w, x, y, z) == BL_LEAVES;
		CHECK(leaves1 < leaves0 / 2 - 5, "and the leaves of its crown (%ld left of the first tree's)", leaves1);
		total += work_steps(&M, w, far_pl, 14.0f, 12.0f, &in_block, NULL);
		CHECK(bl_get(w, 84, 20, 100) == BL_AIR && bl_get(w, 84, 24, 100) == BL_AIR && in_block == 0, "then the second tree, 4 blocks away");
	}

	/* goblins hunt workers: the nearest within 10 blocks, before the keep; the player within 8 blocks first; a worker dies of two blows and is counted */
	{
		struct bl_world *w2 = bl_world_flat(128, 128, 64, 20);
		bl_mobs_clear(&M);
		for (int y = 20; y < 23; y++)
			bl_set(w2, 100, y, 64, BL_KEEP);
		bl_mobs_set_keep(&M, true, 100.5f, 20.0f, 64.5f);
		bl_mobs_add(&M, w2, BL_MOB_WORKER, 70.5f, 20.0f, 64.5f);
		bl_mobs_add(&M, w2, BL_MOB_GOBLIN, 78.5f, 20.0f, 64.5f);
		int lost = 0;
		float kd = 0;
		float t_dead = 0, tt = 0;
		for (int i = 0; i < 30 * 15 && !lost; i++) {
			struct bl_mobs_result R;
			bl_mobs_step(&M, w2, far_pl, true, 0.0f, 1.0f / 30.0f, &R);
			tt += 1.0f / 30.0f;
			lost += R.workers_lost;
			kd += R.keep_damage;
			if (lost && !t_dead)
				t_dead = tt;
		}
		CHECK(lost == 1 && t_dead > 2.0f && t_dead < 10.0f && bl_mobs_count_kind(&M, BL_MOB_WORKER) == 0, "a goblin goes for the worker 8 blocks away, not the keep, and kills it with two blows (%d, %.1f s)", lost, t_dead);
		CHECK(kd == 0.0f && M.kills == 0, "the keep was not touched and a dead worker is no kill");
		bl_mobs_clear(&M);
		bl_mobs_add(&M, w2, BL_MOB_WORKER, 70.5f, 20.0f, 64.5f);
		bl_mobs_add(&M, w2, BL_MOB_GOBLIN, 74.5f, 20.0f, 64.5f);
		float near_pl[3] = { 76.5f, 20.0f, 64.5f };
		float pd = 0;
		int wl = 0;
		for (int i = 0; i < 30 * 6; i++) {
			struct bl_mobs_result R;
			bl_mobs_step(&M, w2, near_pl, true, 0.0f, 1.0f / 30.0f, &R);
			pd += R.damage;
			wl += R.workers_lost;
		}
		CHECK(pd >= 3.0f && wl == 0, "with the player within 8 blocks the goblin goes for him (%.0f to him, %d workers lost)", pd, wl);
		/* 20 blocks off: gone for when it is nearer than the player and the keep */
		static const struct { float wx, gx, px, kx; bool taken; const char *what; } k_far[4] = {
			{ 40.5f, 60.5f, 5.5f, 100.5f, true, "a worker 20 blocks off, nearer than the player and the keep, is gone for" },
			{ 40.5f, 60.5f, 75.5f, 100.5f, false, "not when the player is nearer" },
			{ 40.5f, 60.5f, 5.5f, 75.5f, false, "not when the keep is nearer" },
			{ 30.5f, 60.5f, 120.5f, 120.5f, false, "30 blocks away it is not seen" },
		};
		for (int k = 0; k < 4; k++) {
			bl_mobs_clear(&M);
			bl_mobs_set_keep(&M, true, k_far[k].kx, 20.0f, 64.5f);
			bl_mobs_add(&M, w2, BL_MOB_WORKER, k_far[k].wx, 20.0f, 64.5f);
			bl_mobs_add(&M, w2, BL_MOB_GOBLIN, k_far[k].gx, 20.0f, 64.5f);
			float pp[3] = { k_far[k].px, 20.0f, 64.5f };
			float x0 = M.mob[1].x;
			for (int i = 0; i < 30 * 2; i++)
				bl_mobs_step(&M, w2, pp, true, 17.9f, 1.0f / 30.0f, NULL);
			bool toward = x0 - M.mob[1].x > 3.0f;
			CHECK(toward == k_far[k].taken, "%s (it went %.1f m towards the worker)", k_far[k].what, x0 - M.mob[1].x);
		}
		bl_mobs_set_keep(&M, true, 100.5f, 20.0f, 64.5f);
		bl_mobs_clear(&M);
		bl_mobs_add(&M, w2, BL_MOB_WORKER, 30.5f, 20.0f, 64.5f);
		bl_mobs_add(&M, w2, BL_MOB_GOBLIN, 60.5f, 20.0f, 64.5f);
		int wl2 = 0;
		for (int i = 0; i < 30 * 4; i++) {
			struct bl_mobs_result R;
			bl_mobs_step(&M, w2, far_pl, true, 0.0f, 1.0f / 30.0f, &R);
			wl2 += R.workers_lost;
		}
		CHECK(wl2 == 0 && bl_mobs_count_kind(&M, BL_MOB_WORKER) == 1, "30 blocks away it is left alone");
		bl_world_free(w2);
	}

	/* what does not touch them: M (enabled = false) takes the goblins, not the workers; the sun and the far-away rule are for goblins; they are never burned or dropped */
	{
		bl_mobs_clear(&M);
		M.enabled = false;
		bl_mobs_add(&M, w, BL_MOB_WORKER, 20.5f, 20.0f, 20.5f);
		bl_mobs_add(&M, w, BL_MOB_GOBLIN, 22.5f, 20.0f, 22.5f);
		for (int i = 0; i < 30 * 60; i++)
			bl_mobs_step(&M, w, far_pl, true, 12.0f, 1.0f / 30.0f, NULL);
		CHECK(bl_mobs_count_kind(&M, BL_MOB_WORKER) == 1 && bl_mobs_count_kind(&M, BL_MOB_GOBLIN) == 0, "a minute at noon, 90 blocks from the player, the goblins switched off: the worker is still there");
		CHECK(M.mob[0].hp == BL_WORKER_HP || M.mob[1].hp == BL_WORKER_HP, "unburned");
		M.enabled = true;
	}

	/* the picture: a worker is a person in blue with a yellow cap; chosen, it is lighter */
	{
		enum { W = 160, H = 100 };
		static uint32_t a[W * H], b[W * H], c[W * H];
		struct bl_world *wp = bl_world_flat(64, 64, 64, 20);
		struct bl_mobs P;
		bl_mobs_init(&P, 1);
		bl_mobs_add(&P, wp, BL_MOB_WORKER, 32.5f, 20.0f, 36.5f);
		P.mob[0].yaw = 3.14159265f;
		P.mob[0].ycos = cosf(P.mob[0].yaw);
		P.mob[0].ysin = sinf(P.mob[0].yaw);
		struct bl_camera cam = { 32.5f, 21.4f, 32.5f, 0.0f, -0.05f, 0.8f };
		struct bl_view v;
		bl_view_init(&v);
		v.time_h = 12.0f;
		bl_mobs_light(&P, wp, v.time_h);
		bl_render(wp, &cam, &v, a, W, H, W, 1);
		v.mobs = &P;
		bl_render(wp, &cam, &v, b, W, H, W, 1);
		int changed = 0, blue = 0, yellow = 0;
		for (int i = 0; i < W * H; i++)
			if (a[i] != b[i]) {
				changed++;
				int r = (int)((b[i] >> 16) & 255), g = (int)((b[i] >> 8) & 255), bl = (int)(b[i] & 255);
				blue += bl > r + 40 && bl > g + 10;
				yellow += r > 150 && g > 110 && bl < 90 && r > bl + 80;
			}
		CHECK(changed > 500 && blue > 60 && yellow > 8, "a worker 4 m away: %d pixels differ, %d blue (the tunic), %d yellow (the cap)", changed, blue, yellow);
		long sum_a = 0, sum_b = 0;
		for (int i = 0; i < W * H; i++)
			sum_a += (long)((b[i] >> 16) & 255) + ((b[i] >> 8) & 255) + (b[i] & 255);
		P.mob[0].selected = true;
		bl_render(wp, &cam, &v, c, W, H, W, 1);
		for (int i = 0; i < W * H; i++)
			sum_b += (long)((c[i] >> 16) & 255) + ((c[i] >> 8) & 255) + (c[i] & 255);
		CHECK(sum_b > sum_a + 3000, "chosen it is lighter (%ld against %ld)", sum_b, sum_a);
		struct bl_view iv = v;
		iv.ids = true;
		uint32_t px;
		struct bl_camera one = { 32.5f, 21.0f, 32.5f, 0.0f, 0.0f, 0.8f };
		bl_render(wp, &one, &iv, &px, 1, 1, 1, 1);
		CHECK(px == 0xff0000fcu, "the ray at it meets a creature (%08x)", px);
		bl_world_free(wp);
	}

	/* stress: random orders, goblins about, terrain: never in a block, never a number that is not one */
	{
		struct bl_world *wt = bl_world_new(8, 128, 128, 64);
		struct bl_mobs S;
		bl_mobs_init(&S, 5);
		float p3[3];
		bl_spawn(wt, p3);
		for (int i = 0; i < 5; i++)
			bl_mobs_add(&S, wt, BL_MOB_WORKER, p3[0] + (float)i, p3[1], p3[2] + 2.0f);
		int bad = 0, nan = 0, orders = 0;
		for (int i = 0; i < 30000; i++) {
			if (i % 120 == 0) {
				int wi = (int)(rnd() % 8U);
				int x = (int)(p3[0] + rndr(-9, 9)), z = (int)(p3[2] + rndr(-9, 9)), y = bl_top(wt, x, z);
				if (rnd() % 2U && y > 0)
					orders += bl_mobs_order_gather(&S, wt, wi, x, y, z);
				else
					bl_mobs_order_move(&S, wi, rndr(10, 118), rndr(10, 118));
			}
			struct bl_mobs_result R;
			float dt = i % 97 == 0 ? rndr(0.0f, 0.5f) : i % 233 == 0 ? NAN : rndr(0.0f, 0.05f);
			bl_mobs_step(&S, wt, p3, true, (float)(i / 700 % 24), dt, &R);
			for (int k = 0; k < R.nbreak; k++)
				bl_set(wt, R.brk[k].x, R.brk[k].y, R.brk[k].z, BL_AIR);
			if (i % 30 == 0)
				for (int k = 0; k < BL_MOBS_MAX; k++)
					if (S.mob[k].alive) {
						bad += mob_in_solid(wt, S.mob[k].x, S.mob[k].y, S.mob[k].z, BL_GOBLIN_HW, bl_mob_h(S.mob[k].kind));
						nan += !isfinite(S.mob[k].x + S.mob[k].y + S.mob[k].z + S.mob[k].hp);
					}
		}
		CHECK(bad == 0 && nan == 0 && orders > 5, "30000 odd steps with random orders on terrain: %d samples in a block, %d not numbers, %d gather orders taken", bad, nan, orders);
		bl_world_free(wt);
	}
	bl_world_free(w);
}

/* ---- the command line: bench, shot, info ------------------------------------------------------------------------------- */

static void write_ppm(const char *path, const uint32_t *px, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		printf("cannot write %s\n", path);
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int i = 0; i < w * h; i++) {
		uint8_t rgb[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

static int parse_size(const char *s, int *w, int *h)
{
	return sscanf(s, "%dx%d", w, h) == 2 && *w >= 1 && *h >= 1 && *w <= 8192 && *h <= 8192 ? 0 : -1;
}

static int run_bench(int argc, char **argv)
{
	int w = 640, h = 360, threads = 4;
	if (argc > 0 && parse_size(argv[0], &w, &h) != 0) {
		printf("bad size %s\n", argv[0]);
		return 2;
	}
	if (argc > 1)
		threads = atoi(argv[1]);
	double t0 = now_s();
	struct bl_world *world = bl_world_new(2024, 256, 256, 64);
	printf("world 256 x 256 x 64 made in %.0f ms\n", (now_s() - t0) * 1000.0);
	uint32_t *px = malloc((size_t)w * (size_t)h * 4);
	float s[3];
	bl_spawn(world, s);
	struct bl_view v;
	bl_view_init(&v);
	struct { const char *name; struct bl_camera cam; } views[] = {
		{ "ground, looking along", { s[0], s[1] + BL_EYE, s[2], 0.0f, -0.05f, 1.2f } },
		{ "ground, looking at the sea", { s[0], s[1] + BL_EYE, s[2], 2.4f, -0.05f, 1.2f } },
		{ "20 blocks up, looking down", { s[0], s[1] + 20.0f, s[2], 0.8f, -0.7f, 1.2f } },
		{ "high up, looking at the horizon", { s[0], 70.0f, s[2], 3.9f, -0.15f, 1.2f } },
		{ "looking up", { s[0], s[1] + BL_EYE, s[2], 0.0f, 0.9f, 1.2f } },
	};
	double total = 0;
	for (size_t i = 0; i < sizeof views / sizeof views[0]; i++) {
		bl_render(world, &views[i].cam, &v, px, w, h, w, threads);
		bl_render(world, &views[i].cam, &v, px, w, h, w, threads);
		double best = 1e9, sum = 0;
		const int frames = 12;
		for (int k = 0; k < frames; k++) {
			double a = now_s();
			bl_render(world, &views[i].cam, &v, px, w, h, w, threads);
			double d = now_s() - a;
			sum += d;
			if (d < best)
				best = d;
		}
		printf("%-34s %7.1f ms (best %.1f)  %.1f Mpix/s\n", views[i].name, sum / frames * 1000.0, best * 1000.0, (double)w * h / (sum / frames) / 1e6);
		total += sum / frames;
		if (i == 0) {
			const char *ppm = getenv("MELLAGUI_BLOCKS_PPM");
			if (ppm && *ppm)
				write_ppm(ppm, px, w, h);
		}
	}
	printf("%dx%d on %d threads: %.1f ms a frame on average, %.0f frames a second\n", w, h, threads, total / 5.0 * 1000.0, 5.0 / total);
	{                                                          /* Tom and Filip: far away (a few pixels), near (a big part of the picture), and the same views without them */
		glyphs_make();
		struct bl_pets pets;
		bl_pets_init(&pets, 0, g_glyph_ptr, MCAT_GLYPHS);
		bl_pets_place(&pets, world, s);
		struct bl_pets_info in;
		float eye[3] = { s[0], s[1] + BL_EYE, s[2] };
		bl_pets_pose(&pets, world, 1.0f, 0.016f, eye, 12.0f);
		bl_pets_info(&pets, &in);
		static const float back[3] = { 12.0f, 1.2f, 3.0f };
		for (int near = 0; near < 2; near++) {
			float d = near ? 1.4f : 1.0f;
			struct bl_camera cam = { in.tom[0] - (near ? 1.3f : back[0]), in.tom[1] + (near ? 0.7f : back[1] + 0.4f), in.tom[2] - (near ? 1.3f : back[2] * 0.0f + 3.0f), 0, 0, 1.2f };
			float dx = in.tom[0] - cam.x, dy = in.tom[1] + 0.3f - cam.y, dz = in.tom[2] - cam.z;
			cam.yaw = atan2f(dx, dz);
			cam.pitch = atan2f(dy, hypotf(dx, dz));
			(void)d;
			for (int with = 0; with < 2; with++) {
				bl_pets_pose(&pets, world, 1.0f, 0.016f, (float[3]){ cam.x, cam.y, cam.z }, 12.0f);
				struct bl_view pv = v;
				pv.pets = with ? &pets : NULL;
				bl_render(world, &cam, &pv, px, w, h, w, threads);
				double sum = 0;
				for (int k = 0; k < 8; k++) {
					double a = now_s();
					bl_render(world, &cam, &pv, px, w, h, w, threads);
					sum += now_s() - a;
				}
				printf("%-34s %7.1f ms\n", near ? (with ? "Tom close, with the pets" : "Tom close, without") : (with ? "Tom 12 blocks away, with the pets" : "Tom 12 blocks away, without"), sum / 8.0 * 1000.0);
			}
		}
	}
	free(px);
	bl_world_free(world);
	return 0;
}

static int run_shot(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: --shot FILE.ppm WxH [key=value ...]\n");
		return 2;
	}
	int w, h;
	if (parse_size(argv[1], &w, &h) != 0) {
		printf("bad size %s\n", argv[1]);
		return 2;
	}
	uint32_t seed = 2024;
	int size = 256, flat = 0, threads = 4;
	float ex = NAN, ey = NAN, ez = NAN, yaw = 0, pitch = -5.0f, fov = 70.0f;
	struct bl_view v;
	bl_view_init(&v);
	bool target = false;
	int scene = 0, pets_on = 0, look = 0, mobs_n = 0;
	float pt = 1.0f;
	for (int i = 2; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		if (!eq) {
			printf("bad argument %s\n", argv[i]);
			return 2;
		}
		*eq = 0;
		const char *k = argv[i], *val = eq + 1;
		if (!strcmp(k, "seed")) seed = (uint32_t)strtoul(val, NULL, 10);
		else if (!strcmp(k, "size")) size = atoi(val);
		else if (!strcmp(k, "flat")) flat = atoi(val);
		else if (!strcmp(k, "ex")) ex = (float)atof(val);
		else if (!strcmp(k, "ey")) ey = (float)atof(val);
		else if (!strcmp(k, "ez")) ez = (float)atof(val);
		else if (!strcmp(k, "yaw")) yaw = (float)atof(val);
		else if (!strcmp(k, "pitch")) pitch = (float)atof(val);
		else if (!strcmp(k, "time")) v.time_h = (float)atof(val);
		else if (!strcmp(k, "dist")) v.dist = (float)atof(val);
		else if (!strcmp(k, "fov")) fov = (float)atof(val);
		else if (!strcmp(k, "threads")) threads = atoi(val);
		else if (!strcmp(k, "target")) target = atoi(val) != 0;
		else if (!strcmp(k, "scene")) scene = atoi(val);
		else if (!strcmp(k, "pets")) pets_on = atoi(val);
		else if (!strcmp(k, "mobs")) mobs_n = atoi(val);
		else if (!strcmp(k, "pt")) pt = (float)atof(val);
		else if (!strcmp(k, "look")) look = !strcmp(val, "filip") ? 2 : 1;
		else {
			printf("unknown key %s\n", k);
			return 2;
		}
	}
	struct bl_world *world = flat ? bl_world_flat(size, size, 64, 20) : bl_world_new(seed, size, size, 64);
	if (!world) {
		printf("cannot make the world\n");
		return 1;
	}
	float s[3];
	bl_spawn(world, s);
	if (scene) {                                 /* a torch, a lamp tower, a pool and a waterfall round the spawn (for looking at the light and the flow) */
		int cx = (int)s[0], cy = (int)s[1], cz = (int)s[2];
		bl_set(world, cx + 2, cy, cz + 4, BL_TORCH);
		for (int y = 0; y < 3; y++)
			bl_set(world, cx - 3, cy + y, cz + 5, y == 2 ? BL_LAMP : BL_STONE);
		for (int x = -2; x <= 2; x++)
			bl_set(world, cx + x, cy + 3, cz + 9, BL_BRICK);
		if (scene == 1) {
			bl_set(world, cx, cy + 6, cz + 9, BL_WATER);
			bl_set(world, cx + 5, cy, cz + 2, BL_WATER);
		}
		for (int i = 0; i < 400 && bl_water_pending(world); i++)
			bl_world_tick(world, 1 << 20);
	}
	struct bl_camera cam = { isnan(ex) ? s[0] : ex, isnan(ey) ? s[1] + BL_EYE : ey, isnan(ez) ? s[2] : ez,
	                         yaw * (float)M_PI / 180.0f, pitch * (float)M_PI / 180.0f, fov * (float)M_PI / 180.0f };
	if (target) {
		struct bl_player pl = { .x = cam.x, .y = cam.y - BL_EYE, .z = cam.z, .yaw = cam.yaw, .pitch = cam.pitch };
		float eye[3] = { cam.x, cam.y, cam.z }, look[3];
		bl_player_look(&pl, look);
		struct bl_hit hit;
		if (bl_pick(world, eye, look, BL_REACH, &hit)) {
			v.target = true;
			v.tx = hit.x;
			v.ty = hit.y;
			v.tz = hit.z;
		}
	}
	struct bl_pets pets;
	if (pets_on) {                               /* Tom and Filip in a yard near the spawn, at `pt` seconds of their day */
		glyphs_make();
		bl_pets_init(&pets, seed == 2024 ? 0 : seed, g_glyph_ptr, MCAT_GLYPHS);
		if (!bl_pets_place(&pets, world, s)) {
			printf("no yard\n");
			return 1;
		}
		float eye[3] = { cam.x, cam.y, cam.z };
		for (int i = 0; i < (int)(pt * 30.0f); i++)
			bl_pets_step(&pets, world, 1.0f / 30.0f);
		bl_pets_pose(&pets, world, pt, 0.016f, eye, v.time_h);
		struct bl_pets_info in;
		bl_pets_info(&pets, &in);
		if (look) {
			const float *at = look == 2 ? in.filip : in.tom;
			cam.x = at[0] - 3.0f;
			cam.y = at[1] + 1.6f;
			cam.z = at[2] - 5.0f;
			float dx = at[0] - cam.x, dy = at[1] + (look == 2 ? 0.0f : 0.3f) - cam.y, dz = at[2] - cam.z;
			cam.yaw = atan2f(dx, dz);
			cam.pitch = atan2f(dy, hypotf(dx, dz));
			eye[0] = cam.x;
			eye[1] = cam.y;
			eye[2] = cam.z;
			bl_pets_pose(&pets, world, pt, 0.016f, eye, v.time_h);
		}
		v.pets = &pets;
		printf("Tom at %.1f %.1f %.1f (stands %.2f, lies %.2f), Filip at %.1f %.1f %.1f, yard %.1f %.1f\n", in.tom[0], in.tom[1], in.tom[2], in.tom_stand, in.tom_lie,
		       in.filip[0], in.filip[1], in.filip[2], in.yard[0], in.yard[2]);
	}
	struct bl_mobs mobs;
	if (mobs_n > 0) {                            /* goblins in a fan in front of the camera, 3 to 9 m away, some walking, some blow-flashed */
		bl_mobs_init(&mobs, 1);
		for (int i = 0; i < mobs_n && i < BL_MOBS_MAX; i++) {
			float a = cam.yaw + ((float)i - (float)(mobs_n - 1) * 0.5f) * 0.5f, d = 3.0f + 1.8f * (float)i;
			float x = cam.x + sinf(a) * d, z = cam.z + cosf(a) * d;
			int top = bl_top(world, (int)floorf(x), (int)floorf(z));
			if (top < 0 || !bl_mobs_add(&mobs, world, BL_MOB_GOBLIN, x, (float)top + 1.0f, z))
				continue;
			struct bl_mob *g = &mobs.mob[bl_mobs_count(&mobs) - 1];
			g->yaw = cam.yaw + 3.14159265f + 0.3f * (float)i;
			g->ycos = cosf(g->yaw);
			g->ysin = sinf(g->yaw);
			g->speed = i % 2 ? 2.7f : 0.0f;
			g->phase = 0.2f * (float)i;
			g->swing = i == 1 ? 1.0f : 0.0f;
			g->hurt = i == 2 ? 0.3f : 0.0f;
		}
		bl_mobs_light(&mobs, world, v.time_h);
		v.mobs = &mobs;
	}
	uint32_t *px = malloc((size_t)w * (size_t)h * 4);
	double t0 = now_s();
	bl_render(world, &cam, &v, px, w, h, w, threads);
	printf("%dx%d in %.1f ms, eye %.1f %.1f %.1f\n", w, h, (now_s() - t0) * 1000.0, cam.x, cam.y, cam.z);
	write_ppm(argv[0], px, w, h);
	free(px);
	bl_world_free(world);
	return 0;
}

static int run_info(int argc, char **argv)
{
	uint32_t seed = argc > 0 ? (uint32_t)strtoul(argv[0], NULL, 10) : 2024;
	double t0 = now_s();
	struct bl_world *w = bl_world_new(seed, 256, 256, 64);
	printf("seed %u: made in %.0f ms, sea level %d\n", seed, (now_s() - t0) * 1000.0, w->sea);
	long count[BL_COUNT] = { 0 };
	size_t n = (size_t)w->w * w->d * w->h;
	for (size_t i = 0; i < n; i++)
		count[w->b[i]]++;
	for (int k = 0; k < BL_COUNT; k++)
		printf("  %-8s %9ld  (%.2f %%)\n", bl_block_name(k), count[k], 100.0 * (double)count[k] / (double)n);
	long hist[16] = { 0 };
	int lo = 99, hi = 0;
	long sum = 0, wet = 0, trees = 0, beach = 0, snow = 0;
	for (int z = 0; z < w->d; z++)
		for (int x = 0; x < w->w; x++) {
			int t = w->hi[z * w->w + x];
			if (t < lo) lo = t;
			if (t > hi) hi = t;
			sum += t;
			hist[t / 4 > 15 ? 15 : t / 4]++;
			uint8_t b = w->b[bl_index(w, x, t, z)];
			if (b == BL_WATER) wet++;
			if (b == BL_SAND) beach++;
			if (b == BL_SNOW) snow++;
			if (b == BL_LEAVES) trees++;
		}
	printf("surface height: lowest %d, mean %.1f, highest %d; water %.1f %%, sand %.1f %%, snow %.1f %%, leaves %.1f %% of the columns\n",
	       lo, (double)sum / ((double)w->w * w->d), hi, 100.0 * wet / (w->w * w->d), 100.0 * beach / (w->w * w->d),
	       100.0 * snow / (w->w * w->d), 100.0 * trees / (w->w * w->d));
	for (int i = 0; i < 16; i++)
		if (hist[i])
			printf("  y %2d..%2d  %6ld %.*s\n", i * 4, i * 4 + 3, hist[i], (int)(60.0 * (double)hist[i] / (double)(w->w * w->d / 3)), "############################################################");
	bl_world_free(w);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--bench"))
		return run_bench(argc - 2, argv + 2);
	if (argc > 1 && !strcmp(argv[1], "--shot"))
		return run_shot(argc - 2, argv + 2);
	if (argc > 1 && !strcmp(argv[1], "--info"))
		return run_info(argc - 2, argv + 2);
	if (argc > 1) {
		printf("usage: blocks-test [--bench WxH [threads] | --shot FILE WxH [key=value ...] | --info [seed]]\n");
		return 2;
	}

	double t0 = now_s();
	printf("the world\n");
	test_world();
	printf("editing\n");
	test_edit();
	printf("pointing\n");
	test_pick();
	printf("light\n");
	test_light();
	printf("water that flows\n");
	test_water();
	printf("the player\n");
	test_player();
	printf("the save file\n");
	test_save();
	printf("textures\n");
	test_textures();
	printf("the picture\n");
	test_render();
	printf("Tom and Filip\n");
	test_pets();
	test_pets_render();
	printf("goblins\n");
	test_mobs();
	test_mobs_render();
	printf("survival: hardness, drops, the keep\n");
	test_keep();
	printf("workers\n");
	test_trees();
	test_paths();
	test_workers();
	test_raids();
	test_rival();
	test_raiders();
	test_rival_workers();
	test_soldiers();
	test_brutes();
	printf("building\n");
	printf("the chief, weapons, gold\n");
	test_chief();
	test_plans();
	test_builders();
	test_gates_and_siege();
	test_tower();
	test_ghosts();
	printf("the player's body\n");
	test_hero();
	if (g_dir[0]) {
		char cmd[400];
		snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
		if (system(cmd) != 0)
			printf("(could not remove %s)\n", g_dir);
	}
	printf("blocks-test: %s - %d checks, %d failed, %.1f s\n", g_fail ? "FAILED" : "ok", g_checks, g_fail, now_s() - t0);
	return g_fail ? 1 : 0;
}
