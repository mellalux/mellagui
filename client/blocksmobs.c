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

void bl_mobs_start_wave(struct bl_mobs *m, int n, int cap)
{
	m->wave_on = n > 0;
	m->wave_left = n > 0 ? n : 0;
	m->wave_size = m->wave_left;
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

static bool place_mob(struct bl_mobs *M, const struct bl_world *w, int kind, float x, float y, float z)
{
	if ((kind != BL_MOB_GOBLIN && kind != BL_MOB_WORKER && kind != BL_MOB_SOLDIER) || !bl_inside(w, (int)floorf(x), (int)floorf(y), (int)floorf(z)) ||
	    bl_box_hits(w, x, y, z, BL_GOBLIN_HW, bl_mob_h(kind)))
		return false;
	if (kind == BL_MOB_WORKER && bl_mobs_count_kind(M, BL_MOB_WORKER) >= BL_WORKER_MAX)
		return false;
	if (kind == BL_MOB_SOLDIER && bl_mobs_count_kind(M, BL_MOB_SOLDIER) >= BL_SOLDIER_MAX)
		return false;
	for (int i = 0; i < BL_MOBS_MAX; i++)
		if (!M->mob[i].alive) {
			struct bl_mob *b = &M->mob[i];
			memset(b, 0, sizeof *b);
			b->alive = true;
			b->kind = kind;
			b->x = x;
			b->y = y;
			b->z = z;
			b->hp = kind == BL_MOB_WORKER ? BL_WORKER_HP : kind == BL_MOB_SOLDIER ? BL_SOLDIER_HP : BL_GOBLIN_HP;
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
			return true;
		}
	return false;
}

bool bl_mobs_add(struct bl_mobs *m, const struct bl_world *w, int kind, float x, float y, float z)
{
	return place_mob(m, w, kind, x, y, z);
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
		if (place_mob(M, w, BL_MOB_GOBLIN, (float)x + 0.5f, y, (float)z + 0.5f)) {
			M->spawned++;
			M->wave_left--;
			R->born++;
			return;
		}
	}
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
		float sy = bl_box_support(w, cx, (float)gy + 1.0f, cz, 0.0f, 4.0f);
		if (sy < -1e8f || fabsf(sy + 1.0f - ((float)gy + 0.5f)) > 2.1f)           /* nothing to stand on, or the block is out of reach from there */
			continue;
		if (bl_box_hits(w, cx, sy + 0.002f, cz, BL_GOBLIN_HW, BL_WORKER_H) || bl_water(bl_get(w, (int)floorf(cx), (int)floorf(sy), (int)floorf(cz))))
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

bool bl_mobs_order_gather(struct bl_mobs *m, const struct bl_world *w, int i, int bx, int by, int bz)
{
	if (i < 0 || i >= BL_MOBS_MAX || !m->mob[i].alive || m->mob[i].kind != BL_MOB_WORKER || !bl_inside(w, bx, by, bz))
		return false;
	int blk = w->b[bl_index(w, bx, by, bz)];
	if (!bl_breakable(blk))
		return false;
	if (blk == BL_LOG)
		while (by > 0 && bl_get(w, bx, by - 1, bz) == BL_LOG)                   /* the foot of the tree */
			by--;
	struct bl_mob *b = &m->mob[i];
	float spot[3];
	if (!stand_spot(w, bx, by, bz, b->x, b->z, spot))
		return false;
	b->order = BL_ORDER_GATHER;
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
		b->tx = x;
		b->tz = z;
		b->stuck = 0.0f;
		return;
	}
	b->order = BL_ORDER_MOVE;
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
static float worker_think(struct bl_mob *b, const struct bl_world *w, struct bl_mobs_result *R, float *want)
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
static float soldier_think(struct bl_mobs *M, struct bl_mob *b, struct bl_mobs_result *R, float *want)
{
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
	const float H = bl_mob_h(b->kind);
	b->age += dt;
	b->hurt = fmaxf(0.0f, b->hurt - dt);
	b->attack_cd -= dt;
	b->swing = fmaxf(0.0f, b->swing - dt * 2.5f);
	b->jump_cd = fmaxf(0.0f, b->jump_cd - dt);

	if (bl_box_hits(w, b->x, b->y, b->z, BL_GOBLIN_HW, H)) {                    /* inside a block (one was made round it): up through it, or it is gone */
		bool free = false;
		for (int i = 1; i <= 8 && !free; i++)
			if (!bl_box_hits(w, b->x, b->y + 0.25f * (float)i, b->z, BL_GOBLIN_HW, H)) {
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
	bool hit = bl_box_move(w, &b->x, &b->y, &b->z, 1, b->vy * dt, BL_GOBLIN_HW, H);
	if (hit) {
		b->ground = vy0 < 0;
		b->vy = 0;
	} else {
		b->ground = false;
	}
	if (!hit && b->vy <= 0 && bl_box_hits(w, b->x, b->y - 0.02f, b->z, BL_GOBLIN_HW, H))
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
				if (!o->alive || !bl_mob_is_unit(o->kind) || fabsf(o->y - b->y) > 6.0f)
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
			vmax = clampf(3.0f * (wd - 0.85f), 0.0f, CHASE_SPEED);
		} else if (to_keep) {                                                   /* the player is not near: the keep is what it came for */
			want = atan2f(kx, kz);
			vmax = clampf(3.0f * (dk - 1.0f), 0.0f, CHASE_SPEED);                /* it stops at the foot of it */
		} else if (chase) {
			want = atan2f(dx, dz);
			vmax = clampf(3.0f * (d - 0.85f), 0.0f, CHASE_SPEED);               /* it slows down as it gets there and stops at arm's length */
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
		vmax = soldier_think(M, b, R, &want);
	} else {
		vmax = worker_think(b, w, R, &want);
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
	bool bx = bl_box_move(w, &b->x, &b->y, &b->z, 0, sx, BL_GOBLIN_HW, H);
	bool bz = bl_box_move(w, &b->x, &b->y, &b->z, 2, sz, BL_GOBLIN_HW, H);
	if ((bx || bz) && b->ground && b->jump_cd <= 0.0f && b->speed > 0.1f) {    /* a block one high in the way is climbed, two high is a wall */
		float nx = b->x + b->ysin * 0.4f, nz = b->z + b->ycos * 0.4f;
		if (bl_box_hits(w, nx, b->y, nz, BL_GOBLIN_HW, H) && !bl_box_hits(w, nx, b->y + 1.05f, nz, BL_GOBLIN_HW, H) && !bl_box_hits(w, b->x, b->y + 1.05f, b->z, BL_GOBLIN_HW, H)) {
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
	if (to_worker && b->attack_cd <= 0.0f && wd < ATTACK_REACH) {
		b->attack_cd = ATTACK_COOLDOWN;
		b->swing = 1.0f;
		struct bl_mob *o = &M->mob[wi];
		o->hp -= BL_GOBLIN_HIT;
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
		b->attack_cd = ATTACK_COOLDOWN;
		b->swing = 1.0f;
		R->keep_damage += BL_GOBLIN_KEEP_HIT;
		R->keep_hits++;
	}
	/* a blow at the player */
	if (chase && b->attack_cd <= 0.0f && d < ATTACK_REACH && pl[1] < b->y + BL_GOBLIN_H && pl[1] + PLAYER_H > b->y) {
		b->attack_cd = ATTACK_COOLDOWN;
		b->swing = 1.0f;
		R->damage += BL_GOBLIN_HIT;
		R->hits++;
		float inv = d > 0.01f ? 1.0f / d : 0.0f;
		R->kbx += dx * inv * 6.0f;
		R->kbz += dz * inv * 6.0f;
	}
}

void bl_mobs_step(struct bl_mobs *M, const struct bl_world *w, const float pl[3], bool pl_alive, float time_h, float dt, struct bl_mobs_result *out)
{
	struct bl_mobs_result R = { 0 };
	if (dt > 0.0f && isfinite(dt)) {
		M->acc += dt > 0.25f ? 0.25f : dt;
		while (M->acc >= STEP) {
			M->acc -= STEP;
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
					if (!p->alive || !q->alive)
						continue;
					float ex = q->x - p->x, ez = q->z - p->z, e = hypotf(ex, ez);
					if (e >= 0.55f || fabsf(p->y - q->y) > 1.2f)
						continue;
					float push = (0.55f - e) * 0.5f, nx = e > 0.001f ? ex / e : 1.0f, nz = e > 0.001f ? ez / e : 0.0f;
					bl_box_move(w, &p->x, &p->y, &p->z, 0, -nx * push, BL_GOBLIN_HW, bl_mob_h(p->kind));
					bl_box_move(w, &p->x, &p->y, &p->z, 2, -nz * push, BL_GOBLIN_HW, bl_mob_h(p->kind));
					bl_box_move(w, &q->x, &q->y, &q->z, 0, nx * push, BL_GOBLIN_HW, bl_mob_h(q->kind));
					bl_box_move(w, &q->x, &q->y, &q->z, 2, nz * push, BL_GOBLIN_HW, bl_mob_h(q->kind));
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
		float h = bl_mob_h(b->kind);
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
		if (b->alive && b->x + BL_GOBLIN_HW > fx && b->x - BL_GOBLIN_HW < fx + 1.0f && b->y + bl_mob_h(b->kind) > fy && b->y < fy + 1.0f && b->z + BL_GOBLIN_HW > fz &&
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

void bl_mobs_light(struct bl_mobs *m, const struct bl_world *w, float time_h)
{
	float day = bl_daylight(time_h);
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		struct bl_mob *b = &m->mob[i];
		if (!b->alive)
			continue;
		int x = (int)floorf(b->x), z = (int)floorf(b->z), y = (int)floorf(b->y + 1.0f);
		bool open = bl_top(w, x, z) < y;
		float sky = open ? 0.22f + 0.78f * day : 0.20f + 0.12f * day;
		float bl = (float)bl_light(w, x, y, z) / 15.0f;
		bl = bl > 0.0f ? powf(bl, 0.85f) : 0.0f;
		b->lit[0] = fmaxf(sky * (open && day < 0.3f ? 0.85f : 1.0f), bl * 1.0f);
		b->lit[1] = fmaxf(sky * (open && day < 0.3f ? 0.9f : 1.0f), bl * 0.80f);
		b->lit[2] = fmaxf(sky * (open && day < 0.3f ? 1.15f : 1.0f), bl * 0.52f);
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
		const struct bbox *tab = soldier ? k_soldier : worker ? k_worker : k_goblin;
		const int ntab = soldier ? (int)(sizeof k_soldier / sizeof k_soldier[0]) : worker ? (int)(sizeof k_worker / sizeof k_worker[0]) : (int)(sizeof k_goblin / sizeof k_goblin[0]);
		const float scale = soldier ? SOLDIER_SCALE : worker ? WORKER_SCALE : GOBLIN_SCALE;
		/* the box round the whole creature (arms, ears and a swinging leg reach 0.55 sideways and 0.6 forward) */
		float lo[3] = { b->x - 0.66f, b->y, b->z - 0.66f }, hi[3] = { b->x + 0.66f, b->y + bl_mob_h(b->kind) + 0.06f, b->z + 0.66f };
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
	return found;
}
