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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int g_fail, g_checks;

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
		if (k != BL_GLASS && k != BL_PLANKS && k != BL_BRICK)
			CHECK(count[k] > 0, "the world has no %s", bl_block_name(k));
	CHECK(count[BL_GLASS] == 0 && count[BL_PLANKS] == 0, "glass / planks are only for the player to put");
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
		bl_set(w, (int)(rnd() % 128), 20 + (int)(rnd() % 20), (int)(rnd() % 128), (rnd() & 1) ? BL_GLASS : BL_WATER);
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

static char g_dir[256];

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
			} else if (b == BL_WATER) {
				CHECK(solid == 0 && clear == 0, "water is see-through everywhere");
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
		bl_set(w, (int)(rnd() % 128), 18 + (int)(rnd() % 24), (int)(rnd() % 128), (rnd() & 1) ? BL_GLASS : BL_WATER);
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
	printf("the player\n");
	test_player();
	printf("the save file\n");
	test_save();
	printf("textures\n");
	test_textures();
	printf("the picture\n");
	test_render();
	if (g_dir[0]) {
		char cmd[400];
		snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
		if (system(cmd) != 0)
			printf("(could not remove %s)\n", g_dir);
	}
	printf("blocks-test: %s - %d checks, %d failed, %.1f s\n", g_fail ? "FAILED" : "ok", g_checks, g_fail, now_s() - t0);
	return g_fail ? 1 : 0;
}
