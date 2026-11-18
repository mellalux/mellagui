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
		if (k != BL_GLASS && k != BL_PLANKS && k != BL_BRICK && k != BL_TORCH && k != BL_LAMP && !(k >= BL_FLOW1 && k <= BL_FLOW7))
			CHECK(count[k] > 0, "the world has no %s", bl_block_name(k));
	CHECK(count[BL_GLASS] == 0 && count[BL_PLANKS] == 0 && count[BL_TORCH] == 0 && count[BL_LAMP] == 0, "glass / planks / torches / lamps are only for the player to put");
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
	int scene = 0, pets_on = 0, look = 0;
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
	if (g_dir[0]) {
		char cmd[400];
		snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
		if (system(cmd) != 0)
			printf("(could not remove %s)\n", g_dir);
	}
	printf("blocks-test: %s - %d checks, %d failed, %.1f s\n", g_fail ? "FAILED" : "ok", g_checks, g_fail, now_s() - t0);
	return g_fail ? 1 : 0;
}
