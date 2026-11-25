/* blocksmobs.c - goblins, workers and soldiers in a block world. See blocksmobs.h. */
#include "blocksmobs.h"
#include "blocksview.h"

#include <math.h>
#include <string.h>

#define GRAV 30.0f
#define STEP 0.016666667f
#define AGGRO 26.0f                     /* how far a goblin sees the player */
#define CHASE_SPEED 2.7f                /* metres a second (the player walks 4.3 and runs 5.6) */
#define WANDER_SPEED 0.9f
#define STRIDE 0.9f
#define ATTACK_REACH 1.15f              /* the horizontal distance between the middles at which a blow lands */
#define ATTACK_COOLDOWN 1.0f
#define PLAYER_H 1.8f
#define SPAWN_MIN 14.0f
#define SPAWN_MAX 26.0f
#define DESPAWN_FAR 56.0f
#define SUN_BURN 2.0f                   /* hit points a second in the sun */
#define BRUTE_SCALE 1.5f
#define BRUTE_SPEED 2.1f
#define BRUTE_REACH 1.4f
#define BRUTE_COOLDOWN 1.4f
#define GOBLIN_SCALE 1.2f               /* the boxes of k_goblin are 1.42 m tall; the goblin is that times this */
#define WORKER_SCALE 1.1f
#define SOLDIER_SCALE 1.15f
#define WAVE_MIN 20.0f                  /* a wave's goblins come out of the ground this far from the keep, at least ... */
#define WAVE_MAX 32.0f                  /* ... and at most */
#define WORKER_SEE 10.0f                /* how far a goblin sees a worker */
#define GATHER_FACTOR 1.5f              /* a worker takes this many times the player's time */
#define GATHER_RADIUS 6                 /* it goes on to the next block of the kind within this many blocks */

static float rf(uint32_t *s)
{
	uint32_t x = *s ? *s : 2463534242u;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return (float)(x >> 8) * (1.0f / 16777216.0f);
}

static float ap(float v, float target, float rate, float dt)
{
	float d = target - v, m = rate * dt;
	return d > m ? v + m : d < -m ? v - m : target;
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static float angdiff(float a, float b)                  /* a - b, wrapped to -pi .. pi */
{
	return fmodf(a - b + 9.42477796f, 6.28318531f) - 3.14159265f;
}

void bl_mobs_init(struct bl_mobs *m, uint32_t seed)
{
	memset(m, 0, sizeof *m);
	m->enabled = true;
	m->cap = 6;
	m->rng = seed * 2246822519u + 374761393u;
	m->spawn_t = 3.0f;
}

void bl_mobs_set_keep(struct bl_mobs *m, bool on, float x, float y, float z)
{
	m->keep_on = on;
	m->keep[0] = x;
	m->keep[1] = y;
	m->keep[2] = z;
}

void bl_mobs_clear(struct bl_mobs *m)
{
	for (int i = 0; i < BL_MOBS_MAX; i++)
		m->mob[i].alive = false;
}

void bl_mobs_clear_kind(struct bl_mobs *m, int kind)
{
	for (int i = 0; i < BL_MOBS_MAX; i++)
		if (m->mob[i].kind == kind)
			m->mob[i].alive = false;
}

int bl_mobs_count(const struct bl_mobs *m)
{
	int n = 0;
	for (int i = 0; i < BL_MOBS_MAX; i++)
		n += m->mob[i].alive;
	return n;
}

int bl_mobs_count_kind(const struct bl_mobs *m, int kind)
{
	int n = 0;
	for (int i = 0; i < BL_MOBS_MAX; i++)
		n += m->mob[i].alive && m->mob[i].kind == kind;
	return n;
}

int bl_mobs_count_brutes(const struct bl_mobs *m)
{
	int n = 0;
	for (int i = 0; i < BL_MOBS_MAX; i++)
		n += m->mob[i].alive && m->mob[i].brute;
	return n;
}

void bl_mobs_start_wave(struct bl_mobs *m, int n, int cap, int brutes)
{
	m->wave_on = n > 0;
	m->wave_left = n > 0 ? n : 0;
	m->wave_size = m->wave_left;
	m->wave_brutes = brutes < 0 ? 0 : brutes > m->wave_left ? m->wave_left : brutes;
	m->wave_cap = cap < 1 ? 1 : cap > 20 ? 20 : cap;
	m->spawn_t = 1.0f;
}

void bl_mobs_end_wave(struct bl_mobs *m)
{
	m->wave_on = false;
	m->wave_left = 0;
}

int bl_mobs_wave_remaining(const struct bl_mobs *m)
{
	return m->wave_left + bl_mobs_count_kind(m, BL_MOB_GOBLIN);
}

/* ---- the body --------------------------------------------------------------------------------------------------------- */

/* where the mob went (an index of M->mob), or -1 */
static int place_mob_i(struct bl_mobs *M, const struct bl_world *w, int kind, bool brute, float x, float y, float z)
{
	if ((kind != BL_MOB_GOBLIN && kind != BL_MOB_WORKER && kind != BL_MOB_SOLDIER) || (brute && kind != BL_MOB_GOBLIN) || !bl_inside(w, (int)floorf(x), (int)floorf(y), (int)floorf(z)) ||
	    (kind == BL_MOB_GOBLIN ? bl_box_hits_foe : bl_box_hits_unit)(w, x, y, z, BL_GOBLIN_HW, brute ? BL_BRUTE_H : bl_mob_h(kind)))
		return -1;
	if (kind == BL_MOB_WORKER && bl_mobs_count_kind(M, BL_MOB_WORKER) >= BL_WORKER_MAX)
		return -1;
	if (kind == BL_MOB_SOLDIER && bl_mobs_count_kind(M, BL_MOB_SOLDIER) >= BL_SOLDIER_MAX)
		return -1;
	for (int i = 0; i < BL_MOBS_MAX; i++)
		if (!M->mob[i].alive) {
			struct bl_mob *b = &M->mob[i];
			memset(b, 0, sizeof *b);
			b->alive = true;
			b->kind = kind;
			b->x = x;
			b->y = y;
			b->z = z;
			b->brute = brute;
			b->hp = brute ? BL_BRUTE_HP : kind == BL_MOB_WORKER ? BL_WORKER_HP : kind == BL_MOB_SOLDIER ? BL_SOLDIER_HP : BL_GOBLIN_HP;
			b->tx = x;                                  /* (a soldier's post is where it stands) */
			b->tz = z;
			b->rng = M->rng ^ (uint32_t)(i * 7919 + 13);
			rf(&M->rng);
			b->yaw = rf(&b->rng) * 6.2831853f;
			b->ycos = cosf(b->yaw);
			b->ysin = sinf(b->yaw);
			b->wander_t = 1.0f + 2.0f * rf(&b->rng);
			b->widle = true;
			b->lit[0] = b->lit[1] = b->lit[2] = 0.5f;
			return i;
		}
	return -1;
}

static bool place_mob(struct bl_mobs *M, const struct bl_world *w, int kind, float x, float y, float z)
{
	return place_mob_i(M, w, kind, false, x, y, z) >= 0;
}

bool bl_mobs_add(struct bl_mobs *m, const struct bl_world *w, int kind, float x, float y, float z)
{
	return place_mob(m, w, kind, x, y, z);
}

bool bl_mobs_add_brute(struct bl_mobs *m, const struct bl_world *w, float x, float y, float z)
{
	return place_mob_i(m, w, BL_MOB_GOBLIN, true, x, y, z) >= 0;
}

/* the surface of the world at a column, in the open: the y a creature stands at, or false */
static bool surface_at(const struct bl_world *w, int x, int z, float *y)
{
	if (x < 2 || z < 2 || x >= w->w - 2 || z >= w->d - 2)
		return false;
	int top = bl_top(w, x, z);
	if (top < 1 || top + 3 >= w->h)
		return false;
	if (bl_get(w, x, top + 1, z) != BL_AIR || bl_get(w, x, top + 2, z) != BL_AIR)
		return false;
	*y = (float)(top + 1);
	return true;
}

static void try_spawn(struct bl_mobs *M, const struct bl_world *w, const float pl[3], struct bl_mobs_result *R)
{
	for (int tries = 0; tries < 10; tries++) {
		float a = rf(&M->rng) * 6.2831853f, d = SPAWN_MIN + rf(&M->rng) * (SPAWN_MAX - SPAWN_MIN);
		int x = (int)floorf(pl[0] + sinf(a) * d), z = (int)floorf(pl[2] + cosf(a) * d);
		float y;
		if (!surface_at(w, x, z, &y) || fabsf(y - pl[1]) > 14.0f)
			continue;
		if (bl_water(bl_get(w, x, (int)y, z)))
			continue;
		if (place_mob(M, w, BL_MOB_GOBLIN, (float)x + 0.5f, y, (float)z + 0.5f)) {
			M->spawned++;
			R->born++;
			return;
		}
	}
}

/* a goblin of a wave: out of the ground 20 to 32 blocks from the keep, in the open, anywhere round it */
static void try_spawn_wave(struct bl_mobs *M, const struct bl_world *w, struct bl_mobs_result *R)
{
	for (int tries = 0; tries < 16; tries++) {
		float a = rf(&M->rng) * 6.2831853f, d = WAVE_MIN + rf(&M->rng) * (WAVE_MAX - WAVE_MIN);
		int x = (int)floorf(M->keep[0] + sinf(a) * d), z = (int)floorf(M->keep[2] + cosf(a) * d);
		float y;
		if (!surface_at(w, x, z, &y) || fabsf(y - M->keep[1]) > 14.0f || bl_water(bl_get(w, x, (int)y, z)))
			continue;
		bool brute = M->wave_brutes > 0 && M->wave_left <= M->wave_brutes;           /* the brutes come last */
		if (place_mob_i(M, w, BL_MOB_GOBLIN, brute, (float)x + 0.5f, y, (float)z + 0.5f) >= 0) {
			M->spawned++;
			M->wave_left--;
			R->born++;
			if (brute) {
				M->wave_brutes--;
				R->born_brutes++;
			}
			return;
		}
	}
}

/* a goblin is stopped by a gate and by leaves, the player's units by neither */
static bool mob_hits(const struct bl_world *w, const struct bl_mob *b, float x, float y, float z, float hw, float h)
{
	return b->kind == BL_MOB_GOBLIN ? bl_box_hits_foe(w, x, y, z, hw, h) : bl_box_hits_unit(w, x, y, z, hw, h);
}

static bool mob_move(const struct bl_world *w, struct bl_mob *b, int axis, float d, float h)
{
	return b->kind == BL_MOB_GOBLIN ? bl_box_move_foe(w, &b->x, &b->y, &b->z, axis, d, BL_GOBLIN_HW, h) : bl_box_move_unit(w, &b->x, &b->y, &b->z, axis, d, BL_GOBLIN_HW, h);
}

static void mob_kill(struct bl_mob *b)
{
	b->alive = false;
}

/* ---- workers: orders ------------------------------------------------------------------------------------------------------ */

/* A place to stand beside the block (gx, gy, gz) from which its hands can reach it: a free spot 1.15 m from the block's middle on the ground next to it, the nearest to
 * (fx, fz) of the eight round it; false when there is none. */
static bool stand_spot(const struct bl_world *w, int gx, int gy, int gz, float fx, float fz, float out[3])
{
	float best = 1e9f;
	bool found = false;
	for (int k = 0; k < 8; k++) {
		float ang = (float)k * 0.785398f, cx = (float)gx + 0.5f + sinf(ang) * 1.15f, cz = (float)gz + 0.5f + cosf(ang) * 1.15f;
		float sy = bl_box_support_unit(w, cx, (float)gy + 1.0f, cz, 0.0f, 4.0f);
		if (sy < -1e8f || fabsf(sy + 1.0f - ((float)gy + 0.5f)) > 2.1f)           /* nothing to stand on, or the block is out of reach from there */
			continue;
		if (bl_box_hits_unit(w, cx, sy + 0.002f, cz, BL_GOBLIN_HW, BL_WORKER_H) || bl_water(bl_get(w, (int)floorf(cx), (int)floorf(sy), (int)floorf(cz))))
			continue;
		float d = hypotf(cx - fx, cz - fz);
		if (d < best) {
			best = d;
			found = true;
			out[0] = cx;
			out[1] = sy;
			out[2] = cz;
		}
	}
	return found;
}

/* the time a worker needs for the block, and how many blocks of a tree's trunk go with it (1 for anything else) */
static float gather_time(const struct bl_world *w, int gx, int gy, int gz, int *stack)
{
	int b = bl_get(w, gx, gy, gz), n = 1;
	if (b == BL_LOG) {
		while (n < 8 && bl_get(w, gx, gy + n, gz) == BL_LOG)
			n++;
		*stack = n;
		return GATHER_FACTOR * 1.2f + 0.6f * (float)(n - 1);
	}
	*stack = 1;
	return GATHER_FACTOR * bl_hardness(b);
}

/* The trunk of the tree a leaf belongs to: the nearest log within two blocks whose column ends under a leaf (a log somebody has put there with leaves beside it
 * is no tree). From above it is mostly the crown that is pointed at, so a click on it means the tree. */
static bool tree_of(const struct bl_world *w, int lx, int ly, int lz, int out[3])
{
	int best = 1 << 30;
	for (int dy = -2; dy <= 2; dy++)
		for (int dz = -2; dz <= 2; dz++)
			for (int dx = -2; dx <= 2; dx++) {
				int x = lx + dx, y = ly + dy, z = lz + dz, d = 8 * (dx * dx + dz * dz) + dy * dy;
				if (d >= best || bl_get(w, x, y, z) != BL_LOG)
					continue;
				int top = y;
				while (bl_get(w, x, top + 1, z) == BL_LOG)
					top++;
				if (bl_get(w, x, top + 1, z) != BL_LEAVES)
					continue;
				best = d;
				out[0] = x;
				out[1] = y;
				out[2] = z;
			}
	return best < (1 << 30);
}

bool bl_mobs_order_gather(struct bl_mobs *m, const struct bl_world *w, int i, int bx, int by, int bz)
{
	if (i < 0 || i >= BL_MOBS_MAX || !m->mob[i].alive || m->mob[i].kind != BL_MOB_WORKER || !bl_inside(w, bx, by, bz))
		return false;
	int blk = w->b[bl_index(w, bx, by, bz)];
	if (!bl_breakable(blk))
		return false;
	int trunk[3];
	if (blk == BL_LEAVES && tree_of(w, bx, by, bz, trunk)) {
		bx = trunk[0];
		by = trunk[1];
		bz = trunk[2];
		blk = BL_LOG;
	}
	if (blk == BL_LOG)
		while (by > 0 && bl_get(w, bx, by - 1, bz) == BL_LOG)                   /* the foot of the tree */
			by--;
	struct bl_mob *b = &m->mob[i];
	float spot[3];
	if (!stand_spot(w, bx, by, bz, b->x, b->z, spot))
		return false;
	b->order = BL_ORDER_GATHER;
	b->at_site = false;
	b->best_d = 1e9f;
	b->gx = bx;
	b->gy = by;
	b->gz = bz;
	b->gblock = blk;
	b->tx = spot[0];
	b->tz = spot[2];
	b->work = 0.0f;
	b->wait = 0.0f;
	b->stuck = 0.0f;
	return true;
}

void bl_mobs_order_move(struct bl_mobs *m, int i, float x, float z)
{
	if (i < 0 || i >= BL_MOBS_MAX || !m->mob[i].alive || !bl_mob_is_unit(m->mob[i].kind))
		return;
	struct bl_mob *b = &m->mob[i];
	if (b->kind == BL_MOB_SOLDIER) {                    /* a soldier's post */
		b->order = BL_ORDER_IDLE;                           /* (one on a watchtower comes down: mob_step) */
		b->tx = x;
		b->tz = z;
		b->stuck = 0.0f;
		return;
	}
	b->order = BL_ORDER_MOVE;
	b->at_site = false;
	b->best_d = 1e9f;
	b->tx = x;
	b->tz = z;
	b->work = 0.0f;
	b->stuck = 0.0f;
}

void bl_mobs_order_stop(struct bl_mobs *m, int i)
{
	if (i < 0 || i >= BL_MOBS_MAX || !m->mob[i].alive || !bl_mob_is_unit(m->mob[i].kind))
		return;
	m->mob[i].order = BL_ORDER_IDLE;
	m->mob[i].work = 0.0f;
	if (m->mob[i].kind == BL_MOB_SOLDIER) {             /* it stands where it is */
		m->mob[i].tx = m->mob[i].x;
		m->mob[i].tz = m->mob[i].z;
	}
}

/* ---- building ------------------------------------------------------------------------------------------------------------ */

#define BUILD_NEAR 3.5f                 /* a worker that cannot get to its spot builds from this near the building */
#define BUILD_SEARCH 48.0f              /* how far a worker looks for the next building */

int bl_mobs_count_sites(const struct bl_mobs *m)
{
	int n = 0;
	for (int i = 0; i < BL_SITES_MAX; i++)
		n += m->site[i].on;
	return n;
}

int bl_mobs_add_site(struct bl_mobs *m, const struct bl_site *s)
{
	if (s->n <= 0 || s->n > BL_SITE_CELLS)
		return -1;
	for (int i = 0; i < BL_SITES_MAX; i++)
		if (!m->site[i].on) {
			m->site[i] = *s;
			m->site[i].on = true;
			for (int k = 0; k < BL_MOBS_MAX; k++)          /* (one that was on the site that was here looks again) */
				if (m->mob[k].order == BL_ORDER_BUILD && m->mob[k].site == i)
					m->mob[k].site = -1;
			return i;
		}
	return -1;
}

void bl_mobs_remove_site(struct bl_mobs *m, int site)
{
	if (site < 0 || site >= BL_SITES_MAX)
		return;
	m->site[site].on = false;
	for (int k = 0; k < BL_MOBS_MAX; k++)
		if (m->mob[k].order == BL_ORDER_BUILD && m->mob[k].site == site) {
			m->mob[k].order = BL_ORDER_IDLE;
			m->mob[k].work = 0.0f;
		}
}

/* how far a point is, along the ground, from the box round a site */
static float site_dist(const struct bl_site *s, float x, float z)
{
	int lo[3], hi[3];
	bl_site_box(s, lo, hi);
	float dx = fmaxf(fmaxf((float)lo[0] - x, x - (float)(hi[0] + 1)), 0.0f), dz = fmaxf(fmaxf((float)lo[2] - z, z - (float)(hi[2] + 1)), 0.0f);
	return hypotf(dx, dz);
}

/* a place to stand beside a site: the free one nearest the worker in the two rings round it that no other builder is going to */
static bool site_spot(const struct bl_mobs *M, const struct bl_world *w, const struct bl_site *s, const struct bl_mob *b, float out[2])
{
	int lo[3], hi[3];
	bl_site_box(s, lo, hi);
	float best = 1e9f;
	bool found = false;
	for (int r = 1; r <= 2; r++)
		for (int z = lo[2] - r; z <= hi[2] + r; z++)
			for (int x = lo[0] - r; x <= hi[0] + r; x++) {
				if (x != lo[0] - r && x != hi[0] + r && z != lo[2] - r && z != hi[2] + r)
					continue;
				float cx = (float)x + 0.5f, cz = (float)z + 0.5f;
				float sy = bl_box_support_unit(w, cx, (float)lo[1] + 2.0f, cz, 0.0f, 5.0f);
				if (sy < -1e8f || fabsf(sy - (float)lo[1]) > 2.1f)
					continue;
				if (bl_box_hits_unit(w, cx, sy + 0.002f, cz, BL_GOBLIN_HW, BL_WORKER_H) || bl_water(bl_get(w, x, (int)floorf(sy), z)))
					continue;
				bool taken = false;
				for (int k = 0; k < BL_MOBS_MAX && !taken; k++) {
					const struct bl_mob *o = &M->mob[k];
					taken = o != b && o->alive && o->order == BL_ORDER_BUILD && hypotf(o->tx - cx, o->tz - cz) < 0.8f;
				}
				float d = hypotf(cx - b->x, cz - b->z) + 1.5f * (float)(r - 1) + (taken ? 100.0f : 0.0f);
				if (d < best) {
					best = d;
					found = true;
					out[0] = cx;
					out[1] = cz;
				}
			}
	return found;
}

bool bl_mobs_order_build(struct bl_mobs *m, const struct bl_world *w, int i, int site)
{
	if (i < 0 || i >= BL_MOBS_MAX || !m->mob[i].alive || m->mob[i].kind != BL_MOB_WORKER || site < 0 || site >= BL_SITES_MAX || !m->site[site].on)
		return false;
	struct bl_mob *b = &m->mob[i];
	float spot[2];
	if (!site_spot(m, w, &m->site[site], b, spot))
		return false;
	b->order = BL_ORDER_BUILD;
	b->site = site;
	b->at_site = false;
	b->tx = spot[0];
	b->tz = spot[1];
	b->work = 0.0f;
	b->wait = 0.0f;
	b->stuck = 0.0f;
	b->best_d = 1e9f;
	return true;
}

bool bl_mobs_order_tower(struct bl_mobs *m, const struct bl_world *w, int i, int x, int y, int z)
{
	if (i < 0 || i >= BL_MOBS_MAX || !m->mob[i].alive || m->mob[i].kind != BL_MOB_SOLDIER || bl_get(w, x, y, z) != BL_TOWERPOST)
		return false;
	struct bl_mob *b = &m->mob[i];
	if (b->order == BL_ORDER_TOWER && b->gx == x && b->gy == y && b->gz == z)
		return true;
	if (bl_box_hits_unit(w, (float)x + 0.5f, (float)y + 1.002f, (float)z + 0.5f, BL_GOBLIN_HW, BL_SOLDIER_H))
		return false;
	for (int k = 0; k < BL_MOBS_MAX; k++) {
		const struct bl_mob *o = &m->mob[k];
		if (k != i && o->alive && o->order == BL_ORDER_TOWER && o->gx == x && o->gy == y && o->gz == z)
			return false;
	}
	b->order = BL_ORDER_TOWER;
	b->gx = x;
	b->gy = y;
	b->gz = z;
	b->tx = (float)x + 0.5f;
	b->tz = (float)z + 0.5f;
	b->stuck = 0.0f;
	b->best_d = 1e9f;
	return true;                    /* (one that stands on another tower comes down first: mob_step) */
}

/* is there somebody in the cell (a block is not put into a body)? */
static bool cell_busy(const struct bl_mobs *M, const float pl[3], bool pl_alive, const struct bl_cell *c)
{
	if (pl_alive && pl[0] + 0.3f > (float)c->x && pl[0] - 0.3f < (float)c->x + 1.0f && pl[2] + 0.3f > (float)c->z && pl[2] - 0.3f < (float)c->z + 1.0f &&
	    pl[1] + 1.8f > (float)c->y && pl[1] < (float)c->y + 1.0f)
		return true;
	return bl_mobs_hit_cell(M, c->x, c->y, c->z);
}

static float build_think(struct bl_mobs *M, struct bl_mob *b, const struct bl_world *w, const float pl[3], bool pl_alive, struct bl_mobs_result *R, float *want)
{
	const float dt = STEP;
	if (b->site < 0 || b->site >= BL_SITES_MAX || !M->site[b->site].on || bl_site_missing(w, &M->site[b->site]) == 0) {
		int best = -1;                                          /* that one is done: the nearest other one */
		float bd = BUILD_SEARCH;
		for (int k = 0; k < BL_SITES_MAX; k++)
			if (M->site[k].on && bl_site_missing(w, &M->site[k]) > 0) {
				float d = site_dist(&M->site[k], b->x, b->z);
				if (d < bd) {
					bd = d;
					best = k;
				}
			}
		if (best < 0 || !bl_mobs_order_build(M, w, (int)(b - M->mob), best)) {
			b->order = BL_ORDER_IDLE;
			b->work = 0.0f;
			return 0.0f;
		}
	}
	const struct bl_site *s = &M->site[b->site];
	float dx = b->tx - b->x, dz = b->tz - b->z, d = hypotf(dx, dz);
	if (!b->at_site && (d <= 0.5f || (b->stuck > 1.0f && site_dist(s, b->x, b->z) < BUILD_NEAR)))
		b->at_site = true;
	if (!b->at_site) {
		*want = atan2f(dx, dz);
		return clampf(3.0f * (d - 0.3f), 0.0f, BL_WORKER_SPEED);
	}
	int lo[3], hi[3];
	bl_site_box(s, lo, hi);
	*want = atan2f(0.5f * (float)(lo[0] + hi[0] + 1) - b->x, 0.5f * (float)(lo[2] + hi[2] + 1) - b->z);
	b->work += dt;
	if (fmodf(b->work, 0.55f) < dt)
		b->swing = 1.0f;
	if (b->work < BL_BUILD_TIME || R->nplace >= (int)(sizeof R->place / sizeof R->place[0]))
		return 0.0f;
	int pick = -1;
	float pd = 1e9f;
	bool in_my_way = false;
	for (int k = 0; k < s->n; k++) {
		const struct bl_cell *c = &s->c[k];
		if (!bl_cell_missing(w, c))
			continue;
		bool placed = false;                                    /* (another worker has just put it) */
		for (int j = 0; j < R->nplace && !placed; j++)
			placed = R->place[j].x == c->x && R->place[j].y == c->y && R->place[j].z == c->z;
		if (placed)
			continue;
		if (cell_busy(M, pl, pl_alive, c)) {
			in_my_way |= b->x + BL_GOBLIN_HW > (float)c->x && b->x - BL_GOBLIN_HW < (float)c->x + 1.0f && b->z + BL_GOBLIN_HW > (float)c->z && b->z - BL_GOBLIN_HW < (float)c->z + 1.0f;
			continue;
		}
		float score = (float)c->y * 1000.0f + hypotf((float)c->x + 0.5f - b->x, (float)c->z + 0.5f - b->z);
		if (score < pd) {
			pd = score;
			pick = k;
		}
	}
	if (pick >= 0) {
		R->place[R->nplace++] = s->c[pick];
		b->work = 0.001f;                                       /* (more than nothing: it is at work, not stuck) */
	} else if (in_my_way) {                                     /* it stands where a block goes: out to its spot again */
		b->at_site = false;
		b->work = 0.0f;
		float spot[2];
		if (site_spot(M, w, s, b, spot)) {
			b->tx = spot[0];
			b->tz = spot[1];
			b->best_d = 1e9f;
		}
	} else {
		b->work = 0.001f;                                       /* somebody stands there: it waits */
	}
	return 0.0f;
}

/* ---- the watchtower ------------------------------------------------------------------------------------------------------ */

/* off the post: onto the ground beside the tower (or it just falls when there is no room) */
static void climb_down(const struct bl_world *w, struct bl_mob *b)
{
	const float cx = (float)b->gx + 0.5f, cz = (float)b->gz + 0.5f;
	b->posted = false;
	for (int r = 2; r <= 4; r++)
		for (int k = 0; k < 8; k++) {
			float ang = (float)k * 0.785398f, x = cx + sinf(ang) * (float)r, z = cz + cosf(ang) * (float)r;
			float sy = bl_box_support_unit(w, x, (float)b->gy - 2.0f, z, 0.0f, 10.0f);
			if (sy < -1e8f || bl_box_hits_unit(w, x, sy + 0.002f, z, BL_GOBLIN_HW, BL_SOLDIER_H))
				continue;
			if (hypotf(b->tx - cx, b->tz - cz) < 0.1f) {        /* (it has no other place to go to: it stays where it comes down) */
				b->tx = x;
				b->tz = z;
			}
			b->x = x;
			b->y = sy + 0.002f;
			b->z = z;
			b->vy = 0.0f;
			return;
		}
}

/* a soldier on a watchtower: the nearest goblin it can see gets an arrow */
static void archer(struct bl_mobs *M, const struct bl_world *w, struct bl_mob *b, struct bl_mobs_result *R)
{
	int best = -1;
	float bd = BL_BOW_RANGE, from[3] = { 0 }, to[3] = { 0 };
	for (int k = 0; k < BL_MOBS_MAX; k++) {
		const struct bl_mob *o = &M->mob[k];
		if (!o->alive || o->kind != BL_MOB_GOBLIN)
			continue;
		float ex = o->x - b->x, ez = o->z - b->z, e = hypotf(ex, ez);
		if (e >= bd)
			continue;
		/* from the edge of the platform towards it, at the eyes */
		float out = e > 1.6f ? 1.6f : 0.0f, inv = e > 0.01f ? 1.0f / e : 0.0f;
		float a[3] = { b->x + ex * inv * out, b->y + 1.5f, b->z + ez * inv * out };
		float t[3] = { o->x, o->y + 1.0f, o->z };
		float dir[3] = { t[0] - a[0], t[1] - a[1], t[2] - a[2] };
		float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
		struct bl_hit h;
		if (len > 0.6f && bl_pick(w, a, dir, len - 0.4f, &h))
			continue;
		bd = e;
		best = k;
		memcpy(from, a, sizeof from);
		memcpy(to, t, sizeof to);
	}
	b->speed = 0.0f;
	if (best < 0)
		return;
	struct bl_mob *o = &M->mob[best];
	b->yaw = atan2f(o->x - b->x, o->z - b->z);
	b->ycos = cosf(b->yaw);
	b->ysin = sinf(b->yaw);
	if (b->attack_cd > 0.0f)
		return;
	b->attack_cd = BL_BOW_COOLDOWN;
	b->swing = 1.0f;
	R->shots++;
	int ai = 0;
	for (int k = 1; k < BL_ARROWS_MAX; k++)
		if (M->arrow[k].t < M->arrow[ai].t)
			ai = k;
	M->arrow[ai].t = BL_ARROW_TIME;
	memcpy(M->arrow[ai].a, from, sizeof from);
	memcpy(M->arrow[ai].b, to, sizeof to);
	o->hp -= BL_BOW_DAMAGE;
	o->hurt = 0.35f;
	if (o->hp <= 0.0f) {
		o->alive = false;
		M->kills++;
		R->slain++;
	}
}

/* ---- the goblins and what is in their way ------------------------------------------------------------------------------ */

/* The made block that stops a goblin walking towards `dir` (a yaw): something above one block high in front of it (what is one high it climbs). */
static bool siege_cell(const struct bl_world *w, const struct bl_mob *b, float dir, float H, int out[3])
{
	float px = b->x + sinf(dir) * 0.55f, pz = b->z + cosf(dir) * 0.55f;
	if (!bl_box_hits_foe(w, px, b->y + 1.05f, pz, 0.25f, H))
		return false;
	const int x0 = (int)floorf(b->x), z0 = (int)floorf(b->z), feet = (int)floorf(b->y + 0.05f);
	/* the columns the probe touches, the one straight ahead first; from one up to the head, then the feet */
	const int cx[3] = { (int)floorf(px), (int)floorf(px + (sinf(dir) > 0 ? 0.25f : -0.25f)), (int)floorf(px) };
	const int cz[3] = { (int)floorf(pz), (int)floorf(pz), (int)floorf(pz + (cosf(dir) > 0 ? 0.25f : -0.25f)) };
	const int top = (int)floorf(b->y + 1.05f + H - 0.01f);
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < 3; i++) {
			if (cx[i] == x0 && cz[i] == z0)
				continue;
			for (int y = pass ? feet : feet + 1; y <= (pass ? feet : top); y++) {
				int blk = bl_get(w, cx[i], y, cz[i]);
				if (bl_solid_foe(blk) && bl_siege_hp(blk) > 0) {
					out[0] = cx[i];
					out[1] = y;
					out[2] = cz[i];
					return true;
				}
			}
		}
	return false;
}

static void damage_block(struct bl_mobs *M, const struct bl_world *w, const int c[3], float dmg, struct bl_mobs_result *R)
{
	int e = -1, spare = 0;
	for (int i = 0; i < BL_DAMAGED_MAX; i++) {
		const struct bl_damaged *d = &M->damaged[i];
		if (d->dmg > 0.0f && d->x == c[0] && d->y == c[1] && d->z == c[2]) {
			e = i;
			break;
		}
		if (M->damaged[spare].dmg > 0.0f && (d->dmg <= 0.0f || d->age > M->damaged[spare].age))
			spare = i;
	}
	if (e < 0) {
		e = spare;
		M->damaged[e] = (struct bl_damaged){ c[0], c[1], c[2], 0.0f, 0.0f };
	}
	M->damaged[e].dmg += dmg;
	M->damaged[e].age = 0.0f;
	R->siege_hits++;
	if (M->damaged[e].dmg >= (float)bl_siege_hp(bl_get(w, c[0], c[1], c[2])) && R->nsmash < (int)(sizeof R->smash / sizeof R->smash[0])) {
		R->smash[R->nsmash++] = (struct bl_break){ c[0], c[1], c[2] };
		M->damaged[e].dmg = 0.0f;
	}
}

/* the nearest block of the kind the worker was on within GATHER_RADIUS of it that it can reach; a log is only taken at the foot of its tree */
static bool next_block(const struct bl_world *w, const struct bl_mob *b, int *ox, int *oy, int *oz)
{
	int px = (int)floorf(b->x), py = (int)floorf(b->y), pz = (int)floorf(b->z);
	float best = 1e9f;
	bool found = false;
	for (int dz = -GATHER_RADIUS; dz <= GATHER_RADIUS; dz++)
		for (int dx = -GATHER_RADIUS; dx <= GATHER_RADIUS; dx++)
			for (int dy = -4; dy <= 5; dy++) {
				int x = px + dx, y = py + dy, z = pz + dz;
				if (bl_get(w, x, y, z) != b->gblock || (b->gblock == BL_LOG && bl_get(w, x, y - 1, z) == BL_LOG))
					continue;
				float d = (float)(dx * dx + dz * dz) + 0.5f * (float)(dy * dy);
				if (d >= best)
					continue;
				float spot[3];
				if (!stand_spot(w, x, y, z, b->x, b->z, spot))
					continue;
				best = d;
				found = true;
				*ox = x;
				*oy = y;
				*oz = z;
			}
	return found;
}

/* what a worker does with the time: nothing, walks to a point, or works on a block. Fills in where it wants to go and how fast; returns the speed it asks for. */
static float worker_think(struct bl_mobs *M, struct bl_mob *b, const struct bl_world *w, const float pl[3], bool pl_alive, struct bl_mobs_result *R, float *want)
{
	const float dt = STEP;
	if (b->order == BL_ORDER_MOVE) {
		float dx = b->tx - b->x, dz = b->tz - b->z, d = hypotf(dx, dz);
		if (d < 0.4f) {
			b->order = BL_ORDER_IDLE;
			return 0.0f;
		}
		*want = atan2f(dx, dz);
		return clampf(3.0f * (d - 0.3f), 0.0f, BL_WORKER_SPEED);
	}
	if (b->order == BL_ORDER_BUILD)
		return build_think(M, b, w, pl, pl_alive, R, want);
	if (b->order != BL_ORDER_GATHER)
		return 0.0f;
	b->wait = fmaxf(0.0f, b->wait - dt);
	if (b->wait > 0.0f)
		return 0.0f;
	if (bl_get(w, b->gx, b->gy, b->gz) != b->gblock) {                          /* gone (broken by it, by the player or by another worker): the next one */
		int x, y, z;
		float spot[3];
		if (next_block(w, b, &x, &y, &z) && stand_spot(w, x, y, z, b->x, b->z, spot)) {
			b->gx = x;
			b->gy = y;
			b->gz = z;
			b->tx = spot[0];
			b->tz = spot[2];
			b->work = 0.0f;
			b->stuck = 0.0f;
			b->best_d = 1e9f;
		} else {
			b->order = BL_ORDER_IDLE;
			return 0.0f;
		}
	}
	float dx = b->tx - b->x, dz = b->tz - b->z, d = hypotf(dx, dz);
	if (d > 0.5f) {                                                             /* walk to the place beside it */
		*want = atan2f(dx, dz);
		return clampf(3.0f * (d - 0.3f), 0.0f, BL_WORKER_SPEED);
	}
	/* there: it works, facing the block */
	*want = atan2f((float)b->gx + 0.5f - b->x, (float)b->gz + 0.5f - b->z);
	b->work += dt;
	if (fmodf(b->work, 0.55f) < dt)
		b->swing = 1.0f;
	int stack = 1;
	if (b->work >= gather_time(w, b->gx, b->gy, b->gz, &stack)) {
		if (R->nbreak + stack + 40 <= (int)(sizeof R->brk / sizeof R->brk[0])) {
			for (int k = 0; k < stack; k++)
				R->brk[R->nbreak++] = (struct bl_break){ b->gx, b->gy + k, b->gz };
			if (b->gblock == BL_LOG)                                            /* the crown of the tree goes too */
				for (int ly = b->gy + stack - 3; ly <= b->gy + stack + 1; ly++)
					for (int lz = b->gz - 2; lz <= b->gz + 2; lz++)
						for (int lx = b->gx - 2; lx <= b->gx + 2; lx++)
							if (bl_get(w, lx, ly, lz) == BL_LEAVES && R->nbreak < (int)(sizeof R->brk / sizeof R->brk[0]))
								R->brk[R->nbreak++] = (struct bl_break){ lx, ly, lz };
		} else if (R->nbreak < (int)(sizeof R->brk / sizeof R->brk[0])) {
			R->brk[R->nbreak++] = (struct bl_break){ b->gx, b->gy, b->gz };
		}
		b->work = 0.0f;
		b->wait = 0.4f;                                                         /* (the caller takes the blocks away before it looks for the next) */
	}
	return 0.0f;
}

/* ---- one step --------------------------------------------------------------------------------------------------------- */

/* a soldier: it goes after the nearest goblin that is near it and near its post, hits it when it is there, and otherwise goes back to its post and stands */
static float soldier_think(struct bl_mobs *M, struct bl_mob *b, const struct bl_world *w, struct bl_mobs_result *R, float *want)
{
	if (b->order == BL_ORDER_TOWER) {                           /* to the foot of the tower, then up */
		if (bl_get(w, b->gx, b->gy, b->gz) != BL_TOWERPOST) {
			b->order = BL_ORDER_IDLE;
		} else {
			float dx = (float)b->gx + 0.5f - b->x, dz = (float)b->gz + 0.5f - b->z, d = hypotf(dx, dz);
			if (d < 2.4f) {
				b->posted = true;
				b->x = (float)b->gx + 0.5f;
				b->y = (float)b->gy + 1.002f;
				b->z = (float)b->gz + 0.5f;
				b->vy = b->kx = b->kz = b->speed = b->blocked = b->stuck = 0.0f;
				b->ground = true;
				return 0.0f;
			}
			*want = atan2f(dx, dz);
			return BL_SOLDIER_SPEED;
		}
	}
	int gi = -1;
	float gd = 1e9f, gdx = 0.0f, gdz = 0.0f;
	for (int k = 0; k < BL_MOBS_MAX; k++) {
		const struct bl_mob *o = &M->mob[k];
		if (!o->alive || o->kind != BL_MOB_GOBLIN || fabsf(o->y - b->y) > 6.0f)
			continue;
		float ex = o->x - b->x, ez = o->z - b->z, e = hypotf(ex, ez);
		if (e < BL_SOLDIER_SEE && e < gd && hypotf(o->x - b->tx, o->z - b->tz) < BL_SOLDIER_LEASH) {
			gd = e;
			gi = k;
			gdx = ex;
			gdz = ez;
		}
	}
	if (gi >= 0) {
		*want = atan2f(gdx, gdz);
		if (gd < BL_SOLDIER_REACH && b->attack_cd <= 0.0f) {
			struct bl_mob *o = &M->mob[gi];
			b->attack_cd = BL_SOLDIER_COOLDOWN;
			b->swing = 1.0f;
			R->sword_hits++;
			o->hp -= BL_SOLDIER_DAMAGE;
			o->hurt = 0.35f;
			float inv = gd > 0.01f ? 1.0f / gd : 0.0f;
			o->kx += gdx * inv * 6.0f;
			o->kz += gdz * inv * 6.0f;
			if (o->ground) {
				o->vy = 4.2f;
				o->ground = false;
			}
			if (o->hp <= 0.0f) {
				o->alive = false;
				M->kills++;
				R->slain++;
			}
		}
		return clampf(3.0f * (gd - 0.9f), 0.0f, BL_SOLDIER_SPEED);
	}
	float dx = b->tx - b->x, dz = b->tz - b->z, d = hypotf(dx, dz);
	if (d > 1.0f) {
		*want = atan2f(dx, dz);
		return clampf(3.0f * (d - 0.5f), 0.0f, BL_SOLDIER_SPEED);
	}
	return 0.0f;
}

static void mob_step(struct bl_mobs *M, struct bl_mob *b, const struct bl_world *w, const float pl[3], bool pl_alive, float time_h, struct bl_mobs_result *R)
{
	const float dt = STEP;
	const bool goblin = b->kind == BL_MOB_GOBLIN;
	const float H = bl_mob_height(b);
	const float cspeed = b->brute ? BRUTE_SPEED : CHASE_SPEED, reach = b->brute ? BRUTE_REACH : ATTACK_REACH, cool = b->brute ? BRUTE_COOLDOWN : ATTACK_COOLDOWN;
	const float hit_p = b->brute ? BL_BRUTE_HIT : BL_GOBLIN_HIT, hit_k = b->brute ? BL_BRUTE_KEEP_HIT : BL_GOBLIN_KEEP_HIT, push = b->brute ? 8.0f : 6.0f;
	b->age += dt;
	b->hurt = fmaxf(0.0f, b->hurt - dt);
	b->attack_cd -= dt;
	b->swing = fmaxf(0.0f, b->swing - dt * 2.5f);
	b->jump_cd = fmaxf(0.0f, b->jump_cd - dt);

	if (b->posted) {
		if (b->order == BL_ORDER_TOWER && bl_get(w, b->gx, b->gy, b->gz) == BL_TOWERPOST && hypotf((float)b->gx + 0.5f - b->x, (float)b->gz + 0.5f - b->z) < 0.1f) {
			archer(M, w, b, R);
			return;
		}
		int gx = b->gx, gy = b->gy, gz = b->gz;
		if (b->order == BL_ORDER_TOWER) {                       /* (sent to another tower: down from this one first) */
			b->gx = (int)floorf(b->x);
			b->gy = (int)floorf(b->y) - 1;
			b->gz = (int)floorf(b->z);
		}
		climb_down(w, b);
		if (b->order == BL_ORDER_TOWER) {
			b->gx = gx;
			b->gy = gy;
			b->gz = gz;
		} else {
			b->order = BL_ORDER_IDLE;
		}
	}

	if (mob_hits(w, b, b->x, b->y, b->z, BL_GOBLIN_HW, H)) {                    /* inside a block (one was made round it): up through it, or it is gone */
		bool free = false;
		for (int i = 1; i <= 8 && !free; i++)
			if (!mob_hits(w, b, b->x, b->y + 0.25f * (float)i, b->z, BL_GOBLIN_HW, H)) {
				b->y += 0.25f * (float)i;
				free = true;
			}
		if (!free) {
			mob_kill(b);
			return;
		}
	}

	int cx = (int)floorf(b->x), cz = (int)floorf(b->z);
	b->in_water = bl_water(bl_get(w, cx, (int)floorf(b->y + 0.4f), cz));

	/* falling, and floating in water */
	if (b->in_water)
		b->vy = ap(b->vy, 1.6f, 14.0f, dt);
	else
		b->vy = fmaxf(b->vy - GRAV * dt, -40.0f);
	float vy0 = b->vy;
	bool hit = mob_move(w, b, 1, b->vy * dt, H);
	if (hit) {
		b->ground = vy0 < 0;
		b->vy = 0;
	} else {
		b->ground = false;
	}
	if (!hit && b->vy <= 0 && mob_hits(w, b, b->x, b->y - 0.02f, b->z, BL_GOBLIN_HW, H))
		b->ground = true;

	/* what it wants */
	float dx = pl[0] - b->x, dz = pl[2] - b->z, d = hypotf(dx, dz);
	float kx = M->keep[0] - b->x, kz = M->keep[2] - b->z, dk = hypotf(kx, kz);
	float want = b->yaw, vmax = 0.0f;
	bool to_keep = false, to_worker = false, chase = false;
	int wi = -1;
	float wdx = 0, wdz = 0, wd = 1e9f;
	if (goblin) {
		/* the sun: a goblin in the open burns, and the ones far away in the day just are not there any more */
		float day = bl_daylight(time_h);
		if (day > 0.55f && !b->in_water && bl_top(w, cx, cz) < (int)floorf(b->y)) {
			b->hp -= SUN_BURN * dt;
			b->hurt = fmaxf(b->hurt, 0.12f);
			if (b->hp <= 0.0f) {
				mob_kill(b);
				M->burned++;
				R->burned++;
				return;
			}
		}
		bool near_pl = pl_alive && d < BL_GOBLIN_NEAR && fabsf(pl[1] - b->y) < 8.0f;
		if (!near_pl)                                                           /* a worker in sight: the nearest */
			for (int k = 0; k < BL_MOBS_MAX; k++) {
				const struct bl_mob *o = &M->mob[k];
				if (!o->alive || !bl_mob_is_unit(o->kind) || o->posted || fabsf(o->y - b->y) > 6.0f)
					continue;
				float ex = o->x - b->x, ez = o->z - b->z, e = hypotf(ex, ez);
				if (e < WORKER_SEE && e < wd) {
					wd = e;
					wi = k;
					wdx = ex;
					wdz = ez;
				}
			}
		to_worker = wi >= 0;
		to_keep = M->keep_on && !near_pl && !to_worker && dk < BL_GOBLIN_KEEP_SEE && fabsf(M->keep[1] - b->y) < 14.0f;
		chase = !to_keep && !to_worker && pl_alive && d < AGGRO && fabsf(pl[1] - b->y) < 12.0f;
		if (to_worker) {
			want = atan2f(wdx, wdz);
			vmax = clampf(3.0f * (wd - 0.85f), 0.0f, cspeed);
		} else if (to_keep) {                                                   /* the player is not near: the keep is what it came for */
			want = atan2f(kx, kz);
			vmax = clampf(3.0f * (dk - 1.0f), 0.0f, cspeed);                /* it stops at the foot of it */
		} else if (chase) {
			want = atan2f(dx, dz);
			vmax = clampf(3.0f * (d - 0.85f), 0.0f, cspeed);               /* it slows down as it gets there and stops at arm's length */
		} else {
			b->wander_t -= dt;
			if (b->wander_t <= 0.0f) {
				b->widle = rf(&b->rng) < 0.4f;
				b->wyaw = rf(&b->rng) * 6.2831853f;
				b->wander_t = 2.0f + 3.0f * rf(&b->rng);
			}
			want = b->wyaw;
			vmax = b->widle ? 0.0f : WANDER_SPEED;
		}
	} else if (b->kind == BL_MOB_SOLDIER) {
		vmax = soldier_think(M, b, w, R, &want);
		if (b->posted)
			return;
	} else {
		vmax = worker_think(M, b, w, pl, pl_alive, R, &want);
	}
	b->sieging = false;
	if (goblin && (to_worker || to_keep || chase) && (to_worker ? wd : to_keep ? dk : d) > (to_keep ? 1.6f : reach)) {
		int c[3];
		bool found = siege_cell(w, b, want, H, c);
		for (int k = 0; k < 4 && !found && b->blocked > 0.1f; k++) {        /* slid along it: the side of the world's axes that leads towards the target */
			float ax = (float)k * 1.5707963f;
			if (cosf(ax - want) > 0.3f && siege_cell(w, b, ax, H, c)) {
				found = true;
				want = ax;
			}
		}
		if (found) {
			b->sieging = true;
			vmax = 0.0f;
			b->blocked = 0.0f;
			want = atan2f((float)c[0] + 0.5f - b->x, (float)c[2] + 0.5f - b->z);
			if (b->attack_cd <= 0.0f) {
				b->attack_cd = cool;
				b->swing = 1.0f;
				damage_block(M, w, c, hit_p, R);
			}
		}
	}
	float da = angdiff(want, b->yaw);
	if (b->blocked > 0.1f) {                                                    /* something in the way: turn to one side until it is clear */
		if (b->steer == 0.0f)
			b->steer = rf(&b->rng) < 0.5f ? 1.0f : -1.0f;
		b->yaw += b->steer * 3.5f * dt;
	} else {
		b->steer = 0.0f;
		b->yaw += clampf(da, -6.0f * dt, 6.0f * dt);
	}
	if (b->yaw > 3.14159265f)
		b->yaw -= 6.2831853f;
	if (b->yaw < -3.14159265f)
		b->yaw += 6.2831853f;
	b->ycos = cosf(b->yaw);
	b->ysin = sinf(b->yaw);
	if (fabsf(da) > 1.2f)
		vmax *= 0.3f;
	if (b->in_water)
		vmax *= 0.6f;
	b->speed = ap(b->speed, vmax, 8.0f, dt);

	/* a wanderer does not walk off an edge */
	if (goblin && !chase && !to_keep && !to_worker && b->ground && b->speed > 0.05f) {
		float ax = b->x + b->ysin * 0.55f, az = b->z + b->ycos * 0.55f;
		if (bl_box_support(w, ax, b->y, az, 0.0f, 2.2f) < -1e8f) {
			b->wyaw += 1.7f + rf(&b->rng);
			b->speed = 0.0f;
		}
	}

	float sx = (b->ysin * b->speed + b->kx) * dt, sz = (b->ycos * b->speed + b->kz) * dt;
	float kd = fmaxf(0.0f, 1.0f - 6.0f * dt);
	b->kx *= kd;
	b->kz *= kd;
	float ox = b->x, oz = b->z;
	bool bx = mob_move(w, b, 0, sx, H);
	bool bz = mob_move(w, b, 2, sz, H);
	if ((bx || bz) && b->ground && b->jump_cd <= 0.0f && b->speed > 0.1f) {    /* a block one high in the way is climbed, two high is a wall */
		float nx = b->x + b->ysin * 0.4f, nz = b->z + b->ycos * 0.4f;
		if (mob_hits(w, b, nx, b->y, nz, BL_GOBLIN_HW, H) && !mob_hits(w, b, nx, b->y + 1.05f, nz, BL_GOBLIN_HW, H) && !mob_hits(w, b, b->x, b->y + 1.05f, b->z, BL_GOBLIN_HW, H)) {
			b->vy = 8.4f;
			b->ground = false;
			b->jump_cd = 0.5f;
		}
	}
	float moved = hypotf(b->x - ox, b->z - oz), wanted = hypotf(sx, sz);
	b->phase += moved / STRIDE;
	if (wanted > 0.002f && moved < 0.35f * wanted && !(b->vy > 0.0f))
		b->blocked += dt;
	else
		b->blocked = fmaxf(0.0f, b->blocked - dt);
	if (goblin) {
		if (b->blocked > 2.5f && !chase) {
			b->wander_t = 0.0f;
			b->blocked = 0.0f;
		}
	} else if (b->order != BL_ORDER_IDLE) {                                     /* a worker that does not get nearer to where it is going for four seconds gives up */
		float dd = hypotf(b->tx - b->x, b->tz - b->z);
		if (b->work > 0.0f || b->wait > 0.0f || dd < 0.6f || dd < b->best_d - 0.25f) {
			if (dd < b->best_d)
				b->best_d = dd;
			b->stuck = 0.0f;
		} else {
			b->stuck += dt;
			if (b->stuck > 4.0f) {
				b->order = BL_ORDER_IDLE;
				b->stuck = 0.0f;
			}
		}
	}
	if (!goblin)
		return;

	/* a blow at a worker */
	if (to_worker && b->attack_cd <= 0.0f && wd < reach) {
		b->attack_cd = cool;
		b->swing = 1.0f;
		struct bl_mob *o = &M->mob[wi];
		o->hp -= hit_p;
		o->hurt = 0.35f;
		if (o->hp <= 0.0f) {
			o->alive = false;
			if (o->kind == BL_MOB_SOLDIER)
				R->soldiers_lost++;
			else
				R->workers_lost++;
		}
	}
	/* a blow at the keep */
	if (to_keep && b->attack_cd <= 0.0f && dk < 1.45f && fabsf(M->keep[1] - b->y) < 2.6f) {
		b->attack_cd = cool;
		b->swing = 1.0f;
		R->keep_damage += hit_k;
		R->keep_hits++;
	}
	/* a blow at the player */
	if (chase && b->attack_cd <= 0.0f && d < reach && pl[1] < b->y + H && pl[1] + PLAYER_H > b->y) {
		b->attack_cd = cool;
		b->swing = 1.0f;
		R->damage += hit_p;
		R->hits++;
		float inv = d > 0.01f ? 1.0f / d : 0.0f;
		R->kbx += dx * inv * push;
		R->kbz += dz * inv * push;
	}
}

void bl_mobs_step(struct bl_mobs *M, const struct bl_world *w, const float pl[3], bool pl_alive, float time_h, float dt, struct bl_mobs_result *out)
{
	struct bl_mobs_result R = { 0 };
	if (dt > 0.0f && isfinite(dt)) {
		M->acc += dt > 0.25f ? 0.25f : dt;
		for (int i = 0; i < BL_SITES_MAX; i++)                  /* the buildings that are done */
			if (M->site[i].on && bl_site_missing(w, &M->site[i]) == 0) {
				M->site[i].on = false;
				R.built[R.nbuilt++] = M->site[i].plan;
			}
		while (M->acc >= STEP) {
			M->acc -= STEP;
			for (int i = 0; i < BL_ARROWS_MAX; i++)
				M->arrow[i].t = fmaxf(0.0f, M->arrow[i].t - STEP);
			for (int i = 0; i < BL_DAMAGED_MAX; i++) {          /* a block that has been left alone for half a minute is whole again */
				M->damaged[i].age += STEP;
				if (M->damaged[i].age > 30.0f)
					M->damaged[i].dmg = 0.0f;
			}
			for (int i = 0; i < BL_MOBS_MAX; i++)
				if (M->mob[i].alive) {
					struct bl_mob *b = &M->mob[i];
					mob_step(M, b, w, pl, pl_alive, time_h, &R);
					if (!b->alive || b->kind != BL_MOB_GOBLIN)
						continue;
					float dd = hypotf(pl[0] - b->x, pl[2] - b->z);
					if (M->keep_on) {                                   /* (the wave's goblins are far from the player, near the keep) */
						float dkk = hypotf(M->keep[0] - b->x, M->keep[2] - b->z);
						if (dkk < dd)
							dd = dkk;
					}
					if (!M->enabled || dd > DESPAWN_FAR || (bl_daylight(time_h) > 0.55f && dd > 16.0f && rf(&M->rng) < 0.5f * STEP))
						mob_kill(b);
				}
			/* the bodies do not share a place */
			for (int i = 0; i < BL_MOBS_MAX; i++)
				for (int j = i + 1; j < BL_MOBS_MAX; j++) {
					struct bl_mob *p = &M->mob[i], *q = &M->mob[j];
					if (!p->alive || !q->alive || p->posted || q->posted)
						continue;
					float ex = q->x - p->x, ez = q->z - p->z, e = hypotf(ex, ez);
					if (e >= 0.55f || fabsf(p->y - q->y) > 1.2f)
						continue;
					float push = (0.55f - e) * 0.5f, nx = e > 0.001f ? ex / e : 1.0f, nz = e > 0.001f ? ez / e : 0.0f;
					mob_move(w, p, 0, -nx * push, bl_mob_height(p));
					mob_move(w, p, 2, -nz * push, bl_mob_height(p));
					mob_move(w, q, 0, nx * push, bl_mob_height(q));
					mob_move(w, q, 2, nz * push, bl_mob_height(q));
				}
			M->spawn_t -= STEP;
			if (M->spawn_t <= 0.0f) {
				M->spawn_t = 2.5f + 2.0f * rf(&M->rng);
				int cap = M->cap < BL_MOBS_MAX ? M->cap : BL_MOBS_MAX;
				if (M->enabled && M->wave_on && M->keep_on) {              /* a wave: round the keep, whatever the player does */
					M->spawn_t = 1.0f + 1.5f * rf(&M->rng);
					if (M->wave_left > 0 && bl_daylight(time_h) < 0.55f && bl_mobs_count_kind(M, BL_MOB_GOBLIN) < M->wave_cap)
						try_spawn_wave(M, w, &R);
				} else if (M->enabled && pl_alive && bl_daylight(time_h) < 0.3f && bl_mobs_count_kind(M, BL_MOB_GOBLIN) < cap) {
					try_spawn(M, w, pl, &R);
				}
			}
		}
	}
	if (out)
		*out = R;
}

/* ---- the player's body ------------------------------------------------------------------------------------------------ */

void bl_mobs_push_player(struct bl_mobs *M, const struct bl_world *w, struct bl_player *p)
{
	const float phw = BL_PLAYER_W * 0.5f, near = phw + BL_GOBLIN_HW;
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		struct bl_mob *b = &M->mob[i];
		if (!b->alive || p->y >= b->y + bl_mob_height(b) || p->y + BL_PLAYER_H <= b->y)
			continue;
		float ex = b->x - p->x, ez = b->z - p->z, e = hypotf(ex, ez);
		if (e >= near)
			continue;
		float over = near - e, nx = e > 0.001f ? ex / e : sinf(p->yaw), nz = e > 0.001f ? ez / e : cosf(p->yaw);
		float give = b->posted ? 0.0f : b->kind == BL_MOB_GOBLIN ? 0.5f : 1.0f, ox = b->x, oz = b->z;
		if (give > 0.0f) {
			mob_move(w, b, 0, nx * over * give, bl_mob_height(b));
			mob_move(w, b, 2, nz * over * give, bl_mob_height(b));
		}
		float rest = over - ((b->x - ox) * nx + (b->z - oz) * nz);
		if (rest > 0.002f) {
			bl_box_move(w, &p->x, &p->y, &p->z, 0, -nx * rest, phw, BL_PLAYER_H);
			bl_box_move(w, &p->x, &p->y, &p->z, 2, -nz * rest, phw, BL_PLAYER_H);
		}
	}
}

/* ---- the sword and the pointer ---------------------------------------------------------------------------------------------- */

/* where a ray meets a box: the distance in, or -1 (the slab method; the ray's start inside the box counts as 0) */
static float ray_box(const float o[3], const float d[3], const float lo[3], const float hi[3], float tmax)
{
	float t0 = 0.0f, t1 = tmax;
	for (int k = 0; k < 3; k++) {
		if (fabsf(d[k]) < 1e-9f) {
			if (o[k] < lo[k] || o[k] > hi[k])
				return -1.0f;
			continue;
		}
		float a = (lo[k] - o[k]) / d[k], b = (hi[k] - o[k]) / d[k];
		if (a > b) {
			float s = a;
			a = b;
			b = s;
		}
		if (a > t0)
			t0 = a;
		if (b < t1)
			t1 = b;
		if (t0 > t1)
			return -1.0f;
	}
	return t0;
}

static int pick_kind(const struct bl_mobs *m, int kind /* -1: the player's own (workers and soldiers) */, const float o[3], const float dir[3], float reach, float *t)
{
	float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if (!(len > 1e-9f))
		return -1;
	float d[3] = { dir[0] / len, dir[1] / len, dir[2] / len };
	int best = -1;
	float bt = reach;
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		const struct bl_mob *b = &m->mob[i];
		if (!b->alive || (kind < 0 ? !bl_mob_is_unit(b->kind) : b->kind != kind))
			continue;
		float h = bl_mob_height(b);
		float lo[3] = { b->x - BL_GOBLIN_HW - 0.05f, b->y, b->z - BL_GOBLIN_HW - 0.05f }, hi[3] = { b->x + BL_GOBLIN_HW + 0.05f, b->y + h + 0.05f, b->z + BL_GOBLIN_HW + 0.05f };
		float tt = ray_box(o, d, lo, hi, bt);
		if (tt >= 0.0f && tt <= bt) {
			bt = tt;
			best = i;
		}
	}
	if (best >= 0 && t)
		*t = bt;
	return best;
}

int bl_mobs_pick(const struct bl_mobs *m, const float o[3], const float dir[3], float reach, float *t)
{
	return pick_kind(m, BL_MOB_GOBLIN, o, dir, reach, t);
}

int bl_units_pick(const struct bl_mobs *m, const float o[3], const float dir[3], float reach, float *t)
{
	return pick_kind(m, -1, o, dir, reach, t);
}

bool bl_mobs_hurt(struct bl_mobs *m, int i, float damage, float dx, float dz)
{
	if (i < 0 || i >= BL_MOBS_MAX || !m->mob[i].alive)
		return false;
	struct bl_mob *b = &m->mob[i];
	b->hp -= damage;
	b->hurt = 0.35f;
	float l = hypotf(dx, dz);
	if (l > 1e-6f) {
		b->kx += dx / l * 6.5f;
		b->kz += dz / l * 6.5f;
	}
	if (b->ground) {
		b->vy = 4.2f;
		b->ground = false;
	}
	if (b->hp <= 0.0f) {
		b->alive = false;
		if (b->kind == BL_MOB_GOBLIN)
			m->kills++;
		return true;
	}
	return false;
}

bool bl_mobs_hit_cell(const struct bl_mobs *m, int x, int y, int z)
{
	float fx = (float)x, fy = (float)y, fz = (float)z;
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		const struct bl_mob *b = &m->mob[i];
		if (b->alive && b->x + BL_GOBLIN_HW > fx && b->x - BL_GOBLIN_HW < fx + 1.0f && b->y + bl_mob_height(b) > fy && b->y < fy + 1.0f && b->z + BL_GOBLIN_HW > fz &&
		    b->z - BL_GOBLIN_HW < fz + 1.0f)
			return true;
	}
	return false;
}

/* ---- the picture ------------------------------------------------------------------------------------------------------ */

enum { LIMB_NONE, LIMB_LEG_L, LIMB_LEG_R, LIMB_ARM_L, LIMB_ARM_R };

struct bbox {
	float x0, x1, y0, y1, z0, z1;                   /* in the creature's own frame: x right, y up, z forward, the feet at 0 */
	uint32_t rgb;
	int limb;
	bool glow;                                      /* a light of its own: the eyes */
};

/* a goblin: green skin, a brown tunic, dark legs, pointed ears and a nose, two red eyes */
static const struct bbox k_goblin[] = {
	{ -0.20f, -0.02f, 0.00f, 0.55f, -0.09f, 0.09f, 0x4a3a28u, LIMB_LEG_L, false },
	{ 0.02f, 0.20f, 0.00f, 0.55f, -0.09f, 0.09f, 0x4a3a28u, LIMB_LEG_R, false },
	{ -0.24f, 0.24f, 0.55f, 1.02f, -0.13f, 0.13f, 0x7a4f2au, LIMB_NONE, false },
	{ -0.38f, -0.25f, 0.60f, 1.00f, -0.07f, 0.07f, 0x5aa23au, LIMB_ARM_L, false },
	{ 0.25f, 0.38f, 0.60f, 1.00f, -0.07f, 0.07f, 0x5aa23au, LIMB_ARM_R, false },
	{ -0.18f, 0.18f, 1.02f, 1.38f, -0.17f, 0.17f, 0x5aa23au, LIMB_NONE, false },
	{ -0.31f, -0.18f, 1.19f, 1.27f, -0.03f, 0.03f, 0x5aa23au, LIMB_NONE, false },
	{ 0.18f, 0.31f, 1.19f, 1.27f, -0.03f, 0.03f, 0x5aa23au, LIMB_NONE, false },
	{ -0.04f, 0.04f, 1.10f, 1.19f, 0.17f, 0.24f, 0x4d8a30u, LIMB_NONE, false },
	{ -0.11f, -0.05f, 1.23f, 1.29f, 0.165f, 0.18f, 0xff2a1au, LIMB_NONE, true },
	{ 0.05f, 0.11f, 1.23f, 1.29f, 0.165f, 0.18f, 0xff2a1au, LIMB_NONE, true },
};

/* a brute: a goblin as big again, dark green skin, a plate of iron on its chest, horns and a heavy brow, two red eyes */
static const struct bbox k_brute[] = {
	{ -0.22f, -0.02f, 0.00f, 0.55f, -0.10f, 0.10f, 0x3a2e22u, LIMB_LEG_L, false },
	{ 0.02f, 0.22f, 0.00f, 0.55f, -0.10f, 0.10f, 0x3a2e22u, LIMB_LEG_R, false },
	{ -0.28f, 0.28f, 0.55f, 1.02f, -0.15f, 0.15f, 0x5a5a64u, LIMB_NONE, false },
	{ -0.28f, 0.28f, 0.55f, 0.66f, -0.155f, 0.155f, 0x3a2e22u, LIMB_NONE, false },
	{ -0.46f, -0.28f, 0.58f, 1.04f, -0.09f, 0.09f, 0x3f6e2au, LIMB_ARM_L, false },
	{ 0.28f, 0.46f, 0.58f, 1.04f, -0.09f, 0.09f, 0x3f6e2au, LIMB_ARM_R, false },
	{ -0.18f, 0.18f, 1.02f, 1.36f, -0.17f, 0.17f, 0x3f6e2au, LIMB_NONE, false },
	{ -0.20f, 0.20f, 1.28f, 1.34f, 0.10f, 0.19f, 0x2a4a1cu, LIMB_NONE, false },
	{ -0.26f, -0.18f, 1.36f, 1.56f, -0.05f, 0.03f, 0xe8e0c8u, LIMB_NONE, false },
	{ 0.18f, 0.26f, 1.36f, 1.56f, -0.05f, 0.03f, 0xe8e0c8u, LIMB_NONE, false },
	{ -0.11f, -0.05f, 1.20f, 1.26f, 0.165f, 0.18f, 0xff2a1au, LIMB_NONE, true },
	{ 0.05f, 0.11f, 1.20f, 1.26f, 0.165f, 0.18f, 0xff2a1au, LIMB_NONE, true },
};

/* a worker: the same build, skin the colour of a person, a blue tunic, brown trousers, a yellow cap, small dark eyes */
static const struct bbox k_worker[] = {
	{ -0.20f, -0.02f, 0.00f, 0.55f, -0.09f, 0.09f, 0x55402au, LIMB_LEG_L, false },
	{ 0.02f, 0.20f, 0.00f, 0.55f, -0.09f, 0.09f, 0x55402au, LIMB_LEG_R, false },
	{ -0.24f, 0.24f, 0.55f, 1.02f, -0.13f, 0.13f, 0x3a6ab8u, LIMB_NONE, false },
	{ -0.38f, -0.25f, 0.60f, 1.00f, -0.07f, 0.07f, 0xd9a066u, LIMB_ARM_L, false },
	{ 0.25f, 0.38f, 0.60f, 1.00f, -0.07f, 0.07f, 0xd9a066u, LIMB_ARM_R, false },
	{ -0.18f, 0.18f, 1.02f, 1.38f, -0.17f, 0.17f, 0xd9a066u, LIMB_NONE, false },
	{ -0.22f, 0.22f, 1.36f, 1.45f, -0.21f, 0.21f, 0xe0b030u, LIMB_NONE, false },
	{ -0.11f, -0.05f, 1.20f, 1.26f, 0.165f, 0.18f, 0x20180fu, LIMB_NONE, false },
	{ 0.05f, 0.11f, 1.20f, 1.26f, 0.165f, 0.18f, 0x20180fu, LIMB_NONE, false },
};

/* a soldier: a red tunic, steel arms and legs, a helmet with a red plume, a sword in the right hand */
static const struct bbox k_soldier[] = {
	{ -0.20f, -0.02f, 0.00f, 0.55f, -0.09f, 0.09f, 0x6a7078u, LIMB_LEG_L, false },
	{ 0.02f, 0.20f, 0.00f, 0.55f, -0.09f, 0.09f, 0x6a7078u, LIMB_LEG_R, false },
	{ -0.24f, 0.24f, 0.55f, 1.02f, -0.13f, 0.13f, 0xb02a2au, LIMB_NONE, false },
	{ -0.24f, 0.24f, 0.55f, 0.62f, -0.135f, 0.135f, 0x4a3a28u, LIMB_NONE, false },
	{ -0.38f, -0.25f, 0.60f, 1.00f, -0.07f, 0.07f, 0x9aa2aau, LIMB_ARM_L, false },
	{ 0.25f, 0.38f, 0.60f, 1.00f, -0.07f, 0.07f, 0x9aa2aau, LIMB_ARM_R, false },
	{ 0.30f, 0.34f, 0.95f, 1.50f, 0.00f, 0.05f, 0xdde2e8u, LIMB_ARM_R, false },
	{ -0.18f, 0.18f, 1.02f, 1.38f, -0.17f, 0.17f, 0xd9a066u, LIMB_NONE, false },
	{ -0.21f, 0.21f, 1.32f, 1.47f, -0.20f, 0.20f, 0x8a929au, LIMB_NONE, false },
	{ -0.03f, 0.03f, 1.47f, 1.60f, -0.14f, 0.12f, 0xc02020u, LIMB_NONE, false },
	{ -0.11f, -0.05f, 1.20f, 1.26f, 0.165f, 0.18f, 0x20180fu, LIMB_NONE, false },
	{ 0.05f, 0.11f, 1.20f, 1.26f, 0.165f, 0.18f, 0x20180fu, LIMB_NONE, false },
};

void bl_mobs_light_at(const struct bl_world *w, float time_h, float px, float py, float pz, float lit[3])
{
	float day = bl_daylight(time_h);
	int x = (int)floorf(px), z = (int)floorf(pz), y = (int)floorf(py + 1.0f);
	bool open = bl_top(w, x, z) < y;
	float sky = open ? 0.22f + 0.78f * day : 0.20f + 0.12f * day;
	float bl = (float)bl_light(w, x, y, z) / 15.0f;
	bl = bl > 0.0f ? powf(bl, 0.85f) : 0.0f;
	lit[0] = fmaxf(sky * (open && day < 0.3f ? 0.85f : 1.0f), bl * 1.0f);
	lit[1] = fmaxf(sky * (open && day < 0.3f ? 0.9f : 1.0f), bl * 0.80f);
	lit[2] = fmaxf(sky * (open && day < 0.3f ? 1.15f : 1.0f), bl * 0.52f);
}

void bl_mobs_light(struct bl_mobs *m, const struct bl_world *w, float time_h)
{
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		struct bl_mob *b = &m->mob[i];
		if (b->alive)
			bl_mobs_light_at(w, time_h, b->x, b->y, b->z, b->lit);
	}
}

/* the limbs' offsets (along z, forward) for the walk and the blow */
static void limb_offset(const struct bl_mob *b, int limb, float *dz, float *dy)
{
	float sw = sinf(b->phase * 6.2831853f) * clampf(b->speed / 1.5f, 0.0f, 1.0f), z = 0.0f, y = 0.0f;
	switch (limb) {
	case LIMB_LEG_L: z = 0.16f * sw; break;
	case LIMB_LEG_R: z = -0.16f * sw; break;
	case LIMB_ARM_L: z = -0.14f * sw + 0.30f * b->swing; y = 0.06f * b->swing; break;
	case LIMB_ARM_R: z = 0.14f * sw + 0.30f * b->swing; y = 0.06f * b->swing; break;
	default: break;
	}
	*dz = z;
	*dy = y;
}

bool bl_mobs_ray(const struct bl_mobs *m, const float o[3], const float d[3], float tmax, struct bl_mob_hit *hit)
{
	bool found = false;
	float best = tmax;
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		const struct bl_mob *b = &m->mob[i];
		if (!b->alive)
			continue;
		const bool worker = b->kind == BL_MOB_WORKER, soldier = b->kind == BL_MOB_SOLDIER;
		const struct bbox *tab = soldier ? k_soldier : worker ? k_worker : b->brute ? k_brute : k_goblin;
		const int ntab = soldier ? (int)(sizeof k_soldier / sizeof k_soldier[0]) : worker ? (int)(sizeof k_worker / sizeof k_worker[0]) : b->brute ? (int)(sizeof k_brute / sizeof k_brute[0]) : (int)(sizeof k_goblin / sizeof k_goblin[0]);
		const float scale = soldier ? SOLDIER_SCALE : worker ? WORKER_SCALE : b->brute ? BRUTE_SCALE : GOBLIN_SCALE;
		/* the box round the whole creature (arms, ears and a swinging leg reach 0.55 sideways and 0.6 forward) */
		const float rr = b->brute ? 0.92f : 0.66f;
		float lo[3] = { b->x - rr, b->y, b->z - rr }, hi[3] = { b->x + rr, b->y + bl_mob_height(b) + 0.06f, b->z + rr };
		float tb = ray_box(o, d, lo, hi, best);
		if (tb < 0.0f)
			continue;
		/* into the creature's own frame: x = right (cos, -sin), z = forward (sin, cos) */
		float rx = o[0] - b->x, ry = o[1] - b->y, rz = o[2] - b->z;
		const float is = 1.0f / scale;                                           /* (scaling the origin and the direction alike leaves the distance as it is) */
		float lo_[3] = { (rx * b->ycos - rz * b->ysin) * is, ry * is, (rx * b->ysin + rz * b->ycos) * is };
		float ld[3] = { (d[0] * b->ycos - d[2] * b->ysin) * is, d[1] * is, (d[0] * b->ysin + d[2] * b->ycos) * is };
		for (int k = 0; k < ntab; k++) {
			const struct bbox *bx = &tab[k];
			float dz, dy;
			limb_offset(b, bx->limb, &dz, &dy);
			float blo[3] = { bx->x0, bx->y0 + dy, bx->z0 + dz }, bhi[3] = { bx->x1, bx->y1 + dy, bx->z1 + dz };
			/* which face: the slab that gave the entry */
			float t0 = 0.0f, t1 = best;
			int axis = -1;
			bool miss = false;
			for (int a = 0; a < 3; a++) {
				if (fabsf(ld[a]) < 1e-9f) {
					if (lo_[a] < blo[a] || lo_[a] > bhi[a]) {
						miss = true;
						break;
					}
					continue;
				}
				float s0 = (blo[a] - lo_[a]) / ld[a], s1 = (bhi[a] - lo_[a]) / ld[a];
				if (s0 > s1) {
					float s = s0;
					s0 = s1;
					s1 = s;
				}
				if (s0 > t0) {
					t0 = s0;
					axis = a;
				}
				if (s1 < t1)
					t1 = s1;
				if (t0 > t1) {
					miss = true;
					break;
				}
			}
			if (miss || t0 >= best)
				continue;
			best = t0;
			found = true;
			hit->t = t0;
			hit->id = i;
			/* the shade of the face it was hit on (in the creature's frame: top 1, bottom 0.55, the sides 0.85 and 0.7) */
			float shade = axis == 1 ? (ld[1] < 0.0f ? 1.0f : 0.55f) : axis == 0 ? 0.70f : 0.85f;
			uint32_t c = bx->rgb;
			float col[3] = { (float)((c >> 16) & 255) / 255.0f, (float)((c >> 8) & 255) / 255.0f, (float)(c & 255) / 255.0f };
			for (int ch = 0; ch < 3; ch++) {
				float v = bx->glow ? col[ch] : col[ch] * shade * b->lit[ch];
				if (b->hurt > 0.0f && !bx->glow) {                              /* the red flash of a blow (and of the sun) */
					float f = clampf(b->hurt / 0.35f, 0.0f, 1.0f) * 0.6f;
					v = v * (1.0f - f) + (ch == 0 ? 1.0f : 0.1f) * f;
				}
				if (b->selected && !bx->glow)                                   /* chosen by the player: lighter, towards a pale green */
					v = v * 0.6f + (ch == 0 ? 0.45f : ch == 1 ? 1.0f : 0.7f) * 0.4f;
				hit->rgb[ch] = clampf(v, 0.0f, 1.0f);
			}
		}
	}
	for (int i = 0; i < BL_ARROWS_MAX; i++) {                   /* an arrow: three small pale boxes one behind the other */
		const struct bl_arrow *ar = &m->arrow[i];
		if (ar->t <= 0.0f)
			continue;
		float f = 1.0f - ar->t / BL_ARROW_TIME;
		for (int k = 0; k < 3; k++) {
			float u = clampf(f + 0.2f - 0.07f * (float)k, 0.0f, 1.0f);
			float lo[3], hi[3];
			for (int a = 0; a < 3; a++) {
				float c = ar->a[a] + (ar->b[a] - ar->a[a]) * u;
				lo[a] = c - 0.08f;
				hi[a] = c + 0.08f;
			}
			float t = ray_box(o, d, lo, hi, best);
			if (t >= 0.0f && t < best) {
				best = t;
				found = true;
				hit->t = t;
				hit->id = -1;
				hit->rgb[0] = 1.0f - 0.15f * (float)k;
				hit->rgb[1] = 0.93f - 0.2f * (float)k;
				hit->rgb[2] = 0.55f - 0.15f * (float)k;
			}
		}
	}
	return found;
}
