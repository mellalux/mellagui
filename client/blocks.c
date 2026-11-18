/* blocks.c - the world of blocks: generation, editing, picking, the player's physics, the save file. See blocks.h. */
#include "blocks.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const int8_t bl_face_n[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };

static const char *const k_names[BL_COUNT] = {
	"air", "grass", "dirt", "stone", "sand", "log", "leaves", "planks", "glass", "water", "bedrock", "brick", "snow", "cobble",
};

const char *bl_block_name(int block)
{
	return block >= 0 && block < BL_COUNT ? k_names[block] : "?";
}

/* ---- noise ---------------------------------------------------------------------------------------------------------- */

static inline uint32_t mix32(uint32_t h)
{
	h ^= h >> 16;
	h *= 0x7feb352dU;
	h ^= h >> 15;
	h *= 0x846ca68bU;
	h ^= h >> 16;
	return h;
}

static inline uint32_t h4(uint32_t seed, int a, int b, int c)
{
	return mix32(mix32(seed + 0x9e3779b9U) + (uint32_t)a * 0x9e3779b1U + (uint32_t)b * 0x85ebca77U + (uint32_t)c * 0xc2b2ae3dU);
}

static inline float u01(uint32_t h)
{
	return (float)(h >> 8) * (1.0f / 16777216.0f);
}

static inline float fade(float t)
{
	return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static inline float lerpf(float a, float b, float t)
{
	return a + (b - a) * t;
}

static float smooth01(float a, float b, float v)
{
	float t = (v - a) / (b - a);
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	return t * t * (3.0f - 2.0f * t);
}

static float vnoise2(uint32_t seed, float x, float z)
{
	int ix = (int)floorf(x), iz = (int)floorf(z);
	float u = fade(x - (float)ix), v = fade(z - (float)iz);
	float a = u01(h4(seed, ix, iz, 1)), b = u01(h4(seed, ix + 1, iz, 1));
	float c = u01(h4(seed, ix, iz + 1, 1)), d = u01(h4(seed, ix + 1, iz + 1, 1));
	return lerpf(lerpf(a, b, u), lerpf(c, d, u), v);
}

/* Octaves of value noise, scaled back to 0..1. */
static float fbm2(uint32_t seed, float x, float z, int octaves)
{
	float sum = 0, amp = 1, norm = 0;
	for (int o = 0; o < octaves; o++) {
		sum += amp * vnoise2(seed + (uint32_t)o * 0x632be5abU, x, z);
		norm += amp;
		amp *= 0.5f;
		x *= 2.0f;
		z *= 2.0f;
	}
	return sum / norm;
}

/* ---- allocation and columns ------------------------------------------------------------------------------------- */

static struct bl_world *world_alloc(int w, int d, int h)
{
	if (w < BL_MIN_SIZE || w > BL_MAX_SIZE || d < BL_MIN_SIZE || d > BL_MAX_SIZE || h < BL_MIN_H || h > BL_MAX_H)
		return NULL;
	struct bl_world *wd = calloc(1, sizeof *wd);
	if (!wd)
		return NULL;
	wd->w = w;
	wd->d = d;
	wd->h = h;
	wd->b = calloc((size_t)w * (size_t)d * (size_t)h, 1);
	wd->top = malloc((size_t)w * (size_t)d * sizeof *wd->top);
	wd->hi = malloc((size_t)w * (size_t)d * sizeof *wd->hi);
	if (!wd->b || !wd->top || !wd->hi) {
		bl_world_free(wd);
		return NULL;
	}
	return wd;
}

void bl_world_free(struct bl_world *w)
{
	if (!w)
		return;
	free(w->b);
	free(w->top);
	free(w->hi);
	free(w);
}

/* top (the highest block that blocks light) and hi (the highest that is not air) of one column */
static void column_scan(struct bl_world *w, int x, int z)
{
	const uint8_t *c = w->b + ((size_t)z * (size_t)w->w + (size_t)x) * (size_t)w->h;
	int top = -1, hi = -1;
	for (int y = w->h - 1; y >= 0; y--) {
		uint8_t b = c[y];
		if (b == BL_AIR)
			continue;
		if (hi < 0)
			hi = y;
		if (bl_blocks_light(b)) {
			top = y;
			break;
		}
	}
	/* hi is the first non-air block seen from the top (it may be glass, leaves or water above the highest light-blocking one) */
	w->top[(size_t)z * (size_t)w->w + (size_t)x] = (int16_t)top;
	w->hi[(size_t)z * (size_t)w->w + (size_t)x] = (int16_t)hi;
}

static void columns_all(struct bl_world *w)
{
	w->maxhi = -1;
	for (int z = 0; z < w->d; z++)
		for (int x = 0; x < w->w; x++) {
			column_scan(w, x, z);
			int hi = w->hi[(size_t)z * (size_t)w->w + (size_t)x];
			if (hi > w->maxhi)
				w->maxhi = hi;
		}
}

/* ---- the terrain ---------------------------------------------------------------------------------------------------- */

static int terrain_height(const struct bl_world *w, int x, int z)
{
	float hs = (float)w->h / 64.0f;
	float fx = (float)x, fz = (float)z;
	float n = fbm2(w->seed, fx / 90.0f, fz / 90.0f, 5);                          /* hills and bays */
	float m = fbm2(w->seed ^ 0x51ed270bU, fx / 240.0f, fz / 240.0f, 3);          /* where mountains are */
	float r = fbm2(w->seed ^ 0x2545f491U, fx / 34.0f, fz / 34.0f, 3);            /* roughness */
	float hh = (float)w->sea + 3.0f * hs + (n - 0.5f) * 56.0f * hs;
	hh += smooth01(0.55f, 0.80f, m) * (0.3f + 0.9f * n) * 30.0f * hs * (0.6f + 0.8f * r);
	hh += (r - 0.5f) * 5.0f * hs;
	int hg = (int)floorf(hh);
	int lo = 3, hi = w->h - 8;
	return hg < lo ? lo : hg > hi ? hi : hg;
}

/* 3D value noise along one column: the eight corners of the lattice cell are looked up only when the cell changes, so a column
 * of 60 cells costs a handful of hashes, not 8 per cell */
struct n3 {
	uint32_t seed;
	float kx, ky, kz;               /* 1 / the size of a lattice cell */
	int ix, iy, iz;
	bool valid;
	float c[8];
};

static void n3_init(struct n3 *n, uint32_t seed, float sx, float sy, float sz)
{
	n->seed = seed;
	n->kx = 1.0f / sx;
	n->ky = 1.0f / sy;
	n->kz = 1.0f / sz;
	n->valid = false;
}

static float n3_at(struct n3 *n, int x, int y, int z)
{
	float fx = (float)x * n->kx, fy = (float)y * n->ky, fz = (float)z * n->kz;
	int ix = (int)floorf(fx), iy = (int)floorf(fy), iz = (int)floorf(fz);
	if (!n->valid || ix != n->ix || iy != n->iy || iz != n->iz) {
		for (int i = 0; i < 8; i++)
			n->c[i] = u01(h4(n->seed, ix + (i & 1), iy + ((i >> 1) & 1), iz + (i >> 2)));
		n->ix = ix;
		n->iy = iy;
		n->iz = iz;
		n->valid = true;
	}
	float u = fade(fx - (float)ix), v = fade(fy - (float)iy), q = fade(fz - (float)iz);
	float a = lerpf(lerpf(n->c[0], n->c[1], u), lerpf(n->c[2], n->c[3], u), v);
	float b = lerpf(lerpf(n->c[4], n->c[5], u), lerpf(n->c[6], n->c[7], u), v);
	return lerpf(a, b, q);
}

/* two noise fields that are both near 0.5 at once: a tunnel; they cross now and then, so the tunnels join */
struct tunnels {
	struct n3 a1, a2, b1, b2;
};

static void tunnels_column(struct tunnels *t, uint32_t seed)
{
	n3_init(&t->a1, seed ^ 0xa11ce5U, 26.0f, 15.0f, 26.0f);
	n3_init(&t->a2, seed ^ 0xa11ce6U, 11.0f, 7.0f, 11.0f);
	n3_init(&t->b1, seed ^ 0xb0b5U, 30.0f, 17.0f, 30.0f);
	n3_init(&t->b2, seed ^ 0xb0b6U, 12.0f, 8.0f, 12.0f);
}

static bool tunnel_at(struct tunnels *t, int x, int y, int z)
{
	float a = n3_at(&t->a1, x, y, z);
	if (fabsf(a - 0.5f) > 0.11f)
		return false;
	a += (n3_at(&t->a2, x, y, z) - 0.5f) * 0.22f;
	if (fabsf(a - 0.5f) > 0.05f)
		return false;
	float b = n3_at(&t->b1, x, y, z);
	b += (n3_at(&t->b2, x, y, z) - 0.5f) * 0.22f;
	return fabsf(b - 0.5f) < 0.05f;
}

/* patches of cobble in the rock: a ball in some of the 6 x 6 x 6 squares, a hash for each square */
static bool cobble_at(uint32_t seed, int x, int y, int z)
{
	int cx = x / 6, cy = y / 6, cz = z / 6;
	uint32_t h = h4(seed ^ 0xc0bbU, cx, cy, cz);
	if ((h & 7U) != 0)
		return false;
	float bx = (float)(cx * 6) + 2.6f + (float)((h >> 4) & 7U) * 0.1f;
	float by = (float)(cy * 6) + 2.6f + (float)((h >> 8) & 7U) * 0.1f;
	float bz = (float)(cz * 6) + 2.6f + (float)((h >> 12) & 7U) * 0.1f;
	float r = 1.4f + (float)((h >> 16) & 7U) * 0.1f;
	float dx = (float)x + 0.5f - bx, dy = (float)y + 0.5f - by, dz = (float)z + 0.5f - bz;
	return dx * dx + dy * dy + dz * dz < r * r;
}

static void gen_terrain(struct bl_world *w)
{
	const int W = w->w, D = w->d, H = w->h, sea = w->sea;
	const float hs = (float)H / 64.0f;
	const int snow = sea + (int)(30.0f * hs), rock = sea + (int)(23.0f * hs);

	for (int z = 0; z < D; z++)
		for (int x = 0; x < W; x++) {
			int hg = terrain_height(w, x, z);
			uint8_t *c = w->b + ((size_t)z * (size_t)W + (size_t)x) * (size_t)H;
#define AT(y) c[y]
			for (int y = 0; y <= hg; y++)
				AT(y) = BL_STONE;
			if (hg >= snow) {
				AT(hg) = BL_SNOW;
			} else if (hg >= rock) {
				/* bare rock */
			} else if (hg <= sea + 1) {
				uint8_t top = hg >= sea - 4 ? BL_SAND : BL_DIRT;
				for (int k = 0; k < 3 && hg - k >= 1; k++)
					AT(hg - k) = top;
			} else {
				AT(hg) = BL_GRASS;
				for (int k = 1; k <= 3; k++)
					AT(hg - k) = BL_DIRT;
			}
			for (int y = hg + 1; y <= sea; y++)
				AT(y) = BL_WATER;

			/* tunnels, cobble in the rock; a few tunnels open to the surface on dry land */
			int cave_top = hg - 3;
			if (hg > sea + 2 && vnoise2(w->seed ^ 0xe17aU, (float)x / 18.0f, (float)z / 18.0f) > 0.8f)
				cave_top = hg;
			struct tunnels tn;
			tunnels_column(&tn, w->seed);
			for (int y = 2; y <= cave_top; y++) {
				if (tunnel_at(&tn, x, y, z))
					AT(y) = BL_AIR;
				else if (AT(y) == BL_STONE && y <= hg - 4 && cobble_at(w->seed, x, y, z))
					AT(y) = BL_COBBLE;
			}

			AT(0) = BL_BEDROCK;
			if (u01(h4(w->seed, x, z, 5)) < 0.35f)
				AT(1) = BL_BEDROCK;
#undef AT
		}
}

/* trees: one candidate in every 6 x 6 square, at a place the hash picks; how many get planted depends on a slow noise (forests) */
static void gen_trees(struct bl_world *w)
{
	const int W = w->w, D = w->d, H = w->h;
	for (int cz = 0; cz * 6 < D; cz++)
		for (int cx = 0; cx * 6 < W; cx++) {
			uint32_t h = h4(w->seed, cx, cz, 77);
			int tx = cx * 6 + (int)((h & 0xffU) % 5U), tz = cz * 6 + (int)(((h >> 8) & 0xffU) % 5U);
			if (tx < 3 || tz < 3 || tx >= W - 3 || tz >= D - 3)
				continue;
			float dens = fbm2(w->seed ^ 0xf0f0U, (float)tx / 45.0f, (float)tz / 45.0f, 3);
			float chance = dens > 0.52f ? 0.85f : dens > 0.42f ? 0.22f : 0.03f;
			if (u01(mix32(h ^ 0x5bd1e995U)) >= chance)
				continue;
			int hg = -1;
			for (int y = H - 1; y >= 0; y--) {
				uint8_t b = w->b[bl_index(w, tx, y, tz)];
				if (b != BL_AIR) {
					if (b == BL_GRASS)
						hg = y;
					break;
				}
			}
			if (hg <= w->sea + 1)
				continue;
			int th = 4 + (int)((h >> 16) % 3U);
			if (hg + th + 3 >= H)
				continue;
			for (int y = hg + 1; y <= hg + th; y++)
				w->b[bl_index(w, tx, y, tz)] = BL_LOG;
			for (int ly = hg + th - 2; ly <= hg + th + 1; ly++) {
				int r = ly <= hg + th - 1 ? 2 : 1;
				for (int dz = -r; dz <= r; dz++)
					for (int dx = -r; dx <= r; dx++) {
						if (r == 2 && abs(dx) == 2 && abs(dz) == 2 && (mix32(h ^ (uint32_t)(ly * 131 + dx * 17 + dz)) & 1U))
							continue;
						if (r == 1 && abs(dx) == 1 && abs(dz) == 1 && ly == hg + th + 1)
							continue;
						uint8_t *c = &w->b[bl_index(w, tx + dx, ly, tz + dz)];
						if (*c == BL_AIR)
							*c = BL_LEAVES;
					}
			}
		}
}

struct bl_world *bl_world_new(uint32_t seed, int wd, int dd, int hd)
{
	struct bl_world *w = world_alloc(wd, dd, hd);
	if (!w)
		return NULL;
	w->seed = seed;
	w->kind = 0;
	w->sea = hd * 22 / 64;
	gen_terrain(w);
	gen_trees(w);
	columns_all(w);
	return w;
}

struct bl_world *bl_world_flat(int wd, int dd, int hd, int ground)
{
	if (ground < 2 || ground > hd - 8)
		return NULL;
	struct bl_world *w = world_alloc(wd, dd, hd);
	if (!w)
		return NULL;
	w->kind = 1;
	w->ground = ground;
	w->sea = -1;
	for (int z = 0; z < w->d; z++)
		for (int x = 0; x < w->w; x++)
			for (int y = 0; y < ground; y++) {
				uint8_t b = y == 0 ? BL_BEDROCK : y == ground - 1 ? BL_GRASS : y >= ground - 3 ? BL_DIRT : BL_STONE;
				w->b[bl_index(w, x, y, z)] = b;
			}
	columns_all(w);
	return w;
}

/* ---- editing -------------------------------------------------------------------------------------------------------- */

bool bl_set(struct bl_world *w, int x, int y, int z, int block)
{
	if (!bl_inside(w, x, y, z) || block < 0 || block >= BL_COUNT || block == BL_BEDROCK)
		return false;
	uint8_t *c = &w->b[bl_index(w, x, y, z)];
	if (*c == BL_BEDROCK)
		return false;
	if (*c == block)
		return true;
	*c = (uint8_t)block;
	w->rev++;
	size_t col = (size_t)z * (size_t)w->w + (size_t)x;
	int oldhi = w->hi[col];
	column_scan(w, x, z);
	int hi = w->hi[col];
	if (hi > w->maxhi) {
		w->maxhi = hi;
	} else if (oldhi == w->maxhi && hi < oldhi) {
		int m = -1;
		for (size_t i = 0; i < (size_t)w->w * (size_t)w->d; i++)
			if (w->hi[i] > m)
				m = w->hi[i];
		w->maxhi = m;
	}
	return true;
}

int bl_top(const struct bl_world *w, int x, int z)
{
	if ((unsigned)x >= (unsigned)w->w || (unsigned)z >= (unsigned)w->d)
		return -1;
	return w->top[(size_t)z * (size_t)w->w + (size_t)x];
}

void bl_spawn(const struct bl_world *w, float out[3])
{
	int cx = w->w / 2, cz = w->d / 2;
	int maxr = w->w > w->d ? w->w : w->d;
	for (int r = 0; r < maxr; r++)
		for (int dz = -r; dz <= r; dz++)
			for (int dx = -r; dx <= r; dx++) {
				if (abs(dx) != r && abs(dz) != r)
					continue;
				int x = cx + dx, z = cz + dz;
				if (x < 2 || z < 2 || x >= w->w - 2 || z >= w->d - 2)
					continue;
				int t = bl_top(w, x, z);
				if (t < 0 || t + 1 <= w->sea + 1)
					continue;
				if (w->hi[(size_t)z * (size_t)w->w + (size_t)x] != t)
					continue;
				if (bl_get(w, x, t + 1, z) != BL_AIR || bl_get(w, x, t + 2, z) != BL_AIR)
					continue;
				out[0] = (float)x + 0.5f;
				out[1] = (float)t + 1.0f;
				out[2] = (float)z + 0.5f;
				return;
			}
	out[0] = (float)cx + 0.5f;
	out[1] = (float)(w->maxhi + 1);
	out[2] = (float)cz + 0.5f;
}

uint64_t bl_hash(const struct bl_world *w)
{
	uint64_t h = 1469598103934665603ULL;
	uint32_t dims[3] = { (uint32_t)w->w, (uint32_t)w->d, (uint32_t)w->h };
	const uint8_t *p = (const uint8_t *)dims;
	for (size_t i = 0; i < sizeof dims; i++)
		h = (h ^ p[i]) * 1099511628211ULL;
	size_t n = (size_t)w->w * (size_t)w->d * (size_t)w->h;
	for (size_t i = 0; i < n; i++)
		h = (h ^ w->b[i]) * 1099511628211ULL;
	return h;
}

/* ---- picking ------------------------------------------------------------------------------------------------------- */

bool bl_pick(const struct bl_world *w, const float o[3], const float dir[3], float reach, struct bl_hit *hit)
{
	float dx = dir[0], dy = dir[1], dz = dir[2];
	float len = sqrtf(dx * dx + dy * dy + dz * dz);
	if (!(len > 1e-9f))
		return false;
	dx /= len;
	dy /= len;
	dz /= len;
	int ix = (int)floorf(o[0]), iy = (int)floorf(o[1]), iz = (int)floorf(o[2]);
	int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
	const float big = 1e30f;
	float tdx = dx != 0 ? fabsf(1.0f / dx) : big, tdy = dy != 0 ? fabsf(1.0f / dy) : big, tdz = dz != 0 ? fabsf(1.0f / dz) : big;
	float tmx = dx != 0 ? (dx > 0 ? ((float)ix + 1.0f - o[0]) : (o[0] - (float)ix)) * tdx : big;
	float tmy = dy != 0 ? (dy > 0 ? ((float)iy + 1.0f - o[1]) : (o[1] - (float)iy)) * tdy : big;
	float tmz = dz != 0 ? (dz > 0 ? ((float)iz + 1.0f - o[2]) : (o[2] - (float)iz)) * tdz : big;
	for (;;) {
		float t;
		int face;
		if (tmx < tmy && tmx < tmz) {
			t = tmx;
			tmx += tdx;
			ix += sx;
			face = sx > 0 ? BL_NX : BL_PX;
		} else if (tmy < tmz) {
			t = tmy;
			tmy += tdy;
			iy += sy;
			face = sy > 0 ? BL_NY : BL_PY;
		} else {
			t = tmz;
			tmz += tdz;
			iz += sz;
			face = sz > 0 ? BL_NZ : BL_PZ;
		}
		if (t > reach || (unsigned)ix >= (unsigned)w->w || (unsigned)iz >= (unsigned)w->d || iy < 0)
			return false;
		if (iy >= w->h) {
			if (sy > 0)
				return false;       /* above the world and going up: nothing more to find */
			continue;               /* above it, coming down: the layers up there are empty */
		}
		if (bl_targetable(w->b[bl_index(w, ix, iy, iz)])) {
			hit->hit = true;
			hit->x = ix;
			hit->y = iy;
			hit->z = iz;
			hit->face = face;
			hit->px = ix + bl_face_n[face][0];
			hit->py = iy + bl_face_n[face][1];
			hit->pz = iz + bl_face_n[face][2];
			hit->t = t;
			return true;
		}
	}
}

bool bl_break(struct bl_world *w, const struct bl_hit *h)
{
	if (!h || !h->hit)
		return false;
	return bl_set(w, h->x, h->y, h->z, BL_AIR);
}

bool bl_place(struct bl_world *w, const struct bl_hit *h, int block, const struct bl_player *p)
{
	if (!h || !h->hit || block <= BL_AIR || block >= BL_COUNT || block == BL_WATER || block == BL_BEDROCK)
		return false;
	if (!bl_inside(w, h->px, h->py, h->pz))
		return false;
	uint8_t cur = w->b[bl_index(w, h->px, h->py, h->pz)];
	if (cur != BL_AIR && cur != BL_WATER)
		return false;
	if (p && bl_player_overlaps(p, h->px, h->py, h->pz))
		return false;
	return bl_set(w, h->px, h->py, h->pz, block);
}

/* ---- the player ----------------------------------------------------------------------------------------------------- */

#define HW (BL_PLAYER_W * 0.5f)
#define PH BL_PLAYER_H

#define GRAVITY 32.0f
#define TERMINAL 78.0f
#define JUMP_V 9.0f
#define WALK_V 4.3f
#define SPRINT_V 5.6f
#define SNEAK_V 1.3f
#define FLY_V 10.8f

static bool box_hits(const struct bl_world *w, float x, float y, float z)
{
	int x0 = (int)floorf(x - HW), x1 = (int)ceilf(x + HW) - 1;
	int y0 = (int)floorf(y), y1 = (int)ceilf(y + PH) - 1;
	int z0 = (int)floorf(z - HW), z1 = (int)ceilf(z + HW) - 1;
	for (int yy = y0; yy <= y1; yy++)
		for (int zz = z0; zz <= z1; zz++)
			for (int xx = x0; xx <= x1; xx++)
				if (bl_solid(bl_get(w, xx, yy, zz)))
					return true;
	return false;
}

/* is there ground right under the feet (within half a block)? what a sneaking player must keep to */
static bool foot_support(const struct bl_world *w, float x, float y, float z)
{
	int x0 = (int)floorf(x - HW), x1 = (int)ceilf(x + HW) - 1;
	int z0 = (int)floorf(z - HW), z1 = (int)ceilf(z + HW) - 1;
	int yy = (int)floorf(y - 0.5f);
	for (int zz = z0; zz <= z1; zz++)
		for (int xx = x0; xx <= x1; xx++)
			if (bl_solid(bl_get(w, xx, yy, zz)))
				return true;
	return false;
}

/* Move along one axis by `d`, a quarter block at a time, and stop at the first solid face. True if it was stopped. */
static bool move_axis(const struct bl_world *w, struct bl_player *p, int axis, float d)
{
	float *c = axis == 0 ? &p->x : axis == 1 ? &p->y : &p->z;
	while (d != 0.0f) {
		float s = d > 0.25f ? 0.25f : d < -0.25f ? -0.25f : d;
		d -= s;
		float old = *c;
		*c = old + s;
		if (!box_hits(w, p->x, p->y, p->z))
			continue;
		float n = *c;
		if (axis == 1) {
			if (s > 0)
				n = (float)(int)floorf(n + PH) - PH - 0.001f;
			else
				n = (float)(int)floorf(n) + 1.0f;
		} else {
			if (s > 0)
				n = (float)(int)floorf(n + HW) - HW - 0.001f;
			else
				n = (float)(int)floorf(n - HW) + 1.0f + HW + 0.001f;
		}
		*c = n;
		if (box_hits(w, p->x, p->y, p->z))
			*c = old;           /* it was in something already: stay */
		return true;
	}
	return false;
}

void bl_player_init(struct bl_player *p, const struct bl_world *w)
{
	float s[3];
	memset(p, 0, sizeof *p);
	bl_spawn(w, s);
	p->x = s[0];
	p->y = s[1];
	p->z = s[2];
}

void bl_player_eye(const struct bl_player *p, float out[3])
{
	out[0] = p->x;
	out[1] = p->y + (p->sneaking ? BL_EYE - 0.12f : BL_EYE);
	out[2] = p->z;
}

void bl_player_look(const struct bl_player *p, float out[3])
{
	float cp = cosf(p->pitch);
	out[0] = cp * sinf(p->yaw);
	out[1] = sinf(p->pitch);
	out[2] = cp * cosf(p->yaw);
}

bool bl_player_overlaps(const struct bl_player *p, int x, int y, int z)
{
	return p->x + HW > (float)x && p->x - HW < (float)x + 1.0f &&
	       p->y + PH > (float)y && p->y < (float)y + 1.0f &&
	       p->z + HW > (float)z && p->z - HW < (float)z + 1.0f;
}

static float approach(float v, float target, float rate, float dt)
{
	float k = rate * dt;
	if (k > 1.0f)
		k = 1.0f;
	return v + (target - v) * k;
}

static void step_once(const struct bl_world *w, struct bl_player *p, const struct bl_input *in, float dt)
{
	float eye[3];
	p->sneaking = in->sneak && !p->flying;
	bl_player_eye(p, eye);
	bool head_water = bl_get(w, (int)floorf(eye[0]), (int)floorf(eye[1]), (int)floorf(eye[2])) == BL_WATER;
	p->in_water = bl_get(w, (int)floorf(p->x), (int)floorf(p->y + 0.4f), (int)floorf(p->z)) == BL_WATER;

	/* what the player wants: a direction on the ground plane and a speed */
	float sy = sinf(p->yaw), cy = cosf(p->yaw);
	float wx = sy * in->fwd + cy * in->strafe, wz = cy * in->fwd - sy * in->strafe;
	float wl = sqrtf(wx * wx + wz * wz);
	if (wl > 1.0f) {
		wx /= wl;
		wz /= wl;
	}
	float speed = p->flying ? (in->sprint ? FLY_V * 2.0f : FLY_V) : p->sneaking ? SNEAK_V : in->sprint ? SPRINT_V : WALK_V;
	if (p->in_water && !p->flying)
		speed *= 0.55f;
	float rate = p->flying ? 8.0f : p->on_ground ? 14.0f : p->in_water ? 6.0f : 2.0f;
	p->vx = approach(p->vx, wx * speed, rate, dt);
	p->vz = approach(p->vz, wz * speed, rate, dt);

	float vy0 = p->vy, dy;
	if (p->flying) {
		float target = (in->jump ? 7.5f : 0.0f) - (in->sneak ? 7.5f : 0.0f);
		p->vy = approach(p->vy, target, 10.0f, dt);
		dy = p->vy * dt;
	} else if (p->in_water) {
		if (in->jump) {
			p->vy += 22.0f * dt;
			if (p->vy > 3.2f)
				p->vy = 3.2f;
			if (!head_water)
				p->vy = 5.0f;      /* at the surface: hop up, to get out onto a bank */
		} else {
			p->vy -= 9.0f * dt;
			if (p->vy < -2.5f)
				p->vy = -2.5f;
		}
		dy = p->vy * dt;
	} else {
		if (p->on_ground && in->jump)
			vy0 = JUMP_V;
		p->vy = vy0 - GRAVITY * dt;
		if (p->vy < -TERMINAL)
			p->vy = -TERMINAL;
		dy = 0.5f * (vy0 + p->vy) * dt;      /* the average speed: exact for a constant pull */
	}

	bool was_ground = p->on_ground;
	float ox = p->x, oz = p->z;
	if (move_axis(w, p, 0, p->vx * dt))
		p->vx = 0;
	if (p->sneaking && was_ground && foot_support(w, ox, p->y, oz) && !foot_support(w, p->x, p->y, p->z)) {
		p->x = ox;
		p->vx = 0;
	}
	if (move_axis(w, p, 2, p->vz * dt))
		p->vz = 0;
	if (p->sneaking && was_ground && foot_support(w, p->x, p->y, oz) && !foot_support(w, p->x, p->y, p->z)) {
		p->z = oz;
		p->vz = 0;
	}
	bool hit = move_axis(w, p, 1, dy);
	if (hit) {
		if (p->vy < 0)
			p->on_ground = true;
		p->vy = 0;
	} else {
		p->on_ground = false;
	}
	if (!hit && p->vy <= 0.0f && box_hits(w, p->x, p->y - 0.02f, p->z))
		p->on_ground = true;
}

void bl_player_step(const struct bl_world *w, struct bl_player *p, const struct bl_input *in, float dt)
{
	if (!(dt > 0.0f))
		return;
	if (dt > 1.0f)
		dt = 1.0f;
	while (dt > 0.0f) {
		float s = dt > 0.02f ? 0.02f : dt;
		dt -= s;
		step_once(w, p, in, s);
	}
}

/* ---- the save file ----------------------------------------------------------------------------------------------- */

#define SAVE_MAGIC "MGBL"
#define SAVE_VERSION 1u
#define SAVE_HEADER 36u

struct buf {
	uint8_t *p;
	size_t n, cap;
	bool bad;
};

static void buf_put(struct buf *b, const void *data, size_t n)
{
	if (b->bad)
		return;
	if (b->n + n > b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 4096;
		while (nc < b->n + n)
			nc *= 2;
		uint8_t *np = realloc(b->p, nc);
		if (!np) {
			b->bad = true;
			return;
		}
		b->p = np;
		b->cap = nc;
	}
	memcpy(b->p + b->n, data, n);
	b->n += n;
}

static void buf_u32(struct buf *b, uint32_t v)
{
	uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
	buf_put(b, t, 4);
}

static void buf_varint(struct buf *b, uint32_t v)
{
	uint8_t t[5];
	int n = 0;
	while (v >= 0x80) {
		t[n++] = (uint8_t)(v | 0x80);
		v >>= 7;
	}
	t[n++] = (uint8_t)v;
	buf_put(b, t, (size_t)n);
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static struct bl_world *baseline(uint8_t kind, uint32_t seed, int w, int d, int h, int ground)
{
	return kind == 0 ? bl_world_new(seed, w, d, h) : bl_world_flat(w, d, h, ground);
}

int bl_save(const struct bl_world *w, const char *path)
{
	struct bl_world *base = baseline(w->kind, w->seed, w->w, w->d, w->h, w->ground);
	if (!base)
		return -1;
	size_t n = (size_t)w->w * (size_t)w->d * (size_t)w->h;
	struct buf b = { 0 };
	uint32_t count = 0;
	for (size_t i = 0; i < n; i++)
		if (w->b[i] != base->b[i])
			count++;
	buf_put(&b, SAVE_MAGIC, 4);
	buf_u32(&b, SAVE_VERSION);
	buf_u32(&b, w->kind);
	buf_u32(&b, w->seed);
	buf_u32(&b, (uint32_t)w->w);
	buf_u32(&b, (uint32_t)w->d);
	buf_u32(&b, (uint32_t)w->h);
	buf_u32(&b, (uint32_t)w->ground);
	buf_u32(&b, count);
	size_t prev = 0;
	bool first = true;
	for (size_t i = 0; i < n; i++) {
		if (w->b[i] == base->b[i])
			continue;
		buf_varint(&b, (uint32_t)(first ? i : i - prev));
		buf_put(&b, &w->b[i], 1);
		prev = i;
		first = false;
	}
	bl_world_free(base);
	if (b.bad) {
		free(b.p);
		return -1;
	}
	char tmp[1100];
	snprintf(tmp, sizeof tmp, "%s.tmp%d", path, (int)getpid());
	int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		free(b.p);
		return -1;
	}
	size_t off = 0;
	int ok = 1;
	while (off < b.n) {
		ssize_t r = write(fd, b.p + off, b.n - off);
		if (r <= 0) {
			ok = 0;
			break;
		}
		off += (size_t)r;
	}
	free(b.p);
	if (close(fd) != 0)
		ok = 0;
	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

struct bl_world *bl_load(const char *path, const char **err)
{
	const char *e = NULL;
	struct bl_world *w = NULL, *res = NULL;
	uint8_t *data = NULL;
	FILE *f = fopen(path, "rb");
	if (!f) {
		if (err)
			*err = "cannot open the file";
		return NULL;
	}
	struct stat st;
	if (fstat(fileno(f), &st) != 0 || st.st_size < (off_t)SAVE_HEADER || st.st_size > (off_t)(512u << 20)) {
		fclose(f);
		if (err)
			*err = "not a world file";
		return NULL;
	}
	size_t len = (size_t)st.st_size;
	data = malloc(len);
	if (!data || fread(data, 1, len, f) != len) {
		free(data);
		fclose(f);
		if (err)
			*err = "cannot read the file";
		return NULL;
	}
	fclose(f);

	if (memcmp(data, SAVE_MAGIC, 4) != 0) {
		e = "not a world file";
		goto out;
	}
	if (rd32(data + 4) != SAVE_VERSION) {
		e = "a world file of another version";
		goto out;
	}
	uint32_t kind = rd32(data + 8), seed = rd32(data + 12), W = rd32(data + 16), D = rd32(data + 20), H = rd32(data + 24);
	uint32_t ground = rd32(data + 28), count = rd32(data + 32);
	if (kind > 1 || W < BL_MIN_SIZE || W > BL_MAX_SIZE || D < BL_MIN_SIZE || D > BL_MAX_SIZE || H < BL_MIN_H || H > BL_MAX_H) {
		e = "the size of the world is not valid";
		goto out;
	}
	if (kind == 1 && (ground < 2 || ground > H - 8)) {
		e = "the size of the world is not valid";
		goto out;
	}
	size_t cells = (size_t)W * D * H;
	if (count > cells || (size_t)count * 2 > len - SAVE_HEADER) {
		e = "the file ends too early";
		goto out;
	}
	w = baseline((uint8_t)kind, seed, (int)W, (int)D, (int)H, (int)ground);
	if (!w) {
		e = "out of memory";
		goto out;
	}
	size_t pos = SAVE_HEADER, idx = 0;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t delta = 0;
		int shift = 0;
		for (;;) {
			if (pos >= len || shift > 28) {
				e = "the file ends too early";
				goto out;
			}
			uint8_t c = data[pos++];
			delta |= (uint32_t)(c & 0x7f) << shift;
			shift += 7;
			if (!(c & 0x80))
				break;
		}
		if (i > 0 && delta == 0) {
			e = "the edits are out of order";
			goto out;
		}
		idx = i == 0 ? (size_t)delta : idx + delta;
		if (idx >= cells) {
			e = "an edit is outside the world";
			goto out;
		}
		if (pos >= len) {
			e = "the file ends too early";
			goto out;
		}
		uint8_t blk = data[pos++];
		if (blk >= BL_COUNT) {
			e = "a block that is none";
			goto out;
		}
		w->b[idx] = blk;
	}
	if (pos != len) {
		e = "there is more in the file than it says";
		goto out;
	}
	columns_all(w);
	res = w;
	w = NULL;
out:
	free(data);
	bl_world_free(w);
	if (!res && err)
		*err = e ? e : "the file is damaged";
	return res;
}
