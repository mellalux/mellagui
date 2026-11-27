/* blocksmobs.c - goblins, workers and soldiers in a block world. See blocksmobs.h. */
#include "blocksmobs.h"
#include "blocksview.h"

#include <math.h>
#include <stdlib.h>
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
#define RAID_MIN 10.0f                  /* goblins that come for units that are apart come out this near one of them, near enough to see it ... */
#define RAID_MAX 20.0f                  /* ... and at most this far */
#define GATHER_FACTOR 1.5f              /* a worker takes this many times the player's time */
#define GATHER_RADIUS 6                 /* it goes on to the next block of the kind within this many blocks */

static const struct bl_weapon_info k_weapons[BL_WP_COUNT] = {
	/*  name        dmg   reach  cool   keep splash  shot            chief: dmg cool keep */
	{ "sword",      4.0f,  3.6f, 0.40f,  3.0f, 0.0f, -1,             3.0f, 1.0f, 2.0f },
	{ "spear",      3.5f,  5.5f, 0.45f,  2.5f, 0.0f, -1,             3.0f, 1.1f, 2.0f },
	{ "war hammer", 9.0f,  3.2f, 0.90f, 10.0f, 0.0f, -1,             6.0f, 1.6f, 5.0f },
	{ "bow",        4.0f, 30.0f, 0.80f,  1.0f, 0.0f, BL_SHOT_ARROW,  3.0f, 1.8f, 1.0f },
	{ "fire staff", 7.0f, 26.0f, 1.40f,  8.0f, 3.0f, BL_SHOT_FIRE,   4.0f, 2.8f, 4.0f },
	{ "laser",      2.0f, 44.0f, 0.15f,  1.2f, 0.0f, BL_SHOT_LASER,  3.0f, 1.2f, 2.0f },
};

const struct bl_weapon_info *bl_weapon(int weapon)
{
	return &k_weapons[weapon > 0 && weapon < BL_WP_COUNT ? weapon : BL_WP_SWORD];
}

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
	m->chief_arms = 1u << BL_WP_SWORD | 1u << BL_WP_BOW;
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

void bl_mobs_set_rival(struct bl_mobs *m, bool on, float x, float y, float z)
{
	m->rival_on = on;
	m->rkeep[0] = x;
	m->rkeep[1] = y;
	m->rkeep[2] = z;
}

void bl_mobs_set_rsite(struct bl_mobs *m, const struct bl_site *s)
{
	if (s && s->n > 0 && s->n <= BL_SITE_CELLS) {
		m->rsite = *s;
		m->rsite.on = true;
	} else {
		m->rsite.on = false;
	}
}

void bl_mobs_set_raid(struct bl_mobs *m, float x, float y, float z)
{
	m->raid_at[0] = x;
	m->raid_at[1] = y;
	m->raid_at[2] = z;
}

int bl_mobs_count_raiders(const struct bl_mobs *m)
{
	int n = 0;
	for (int i = 0; i < BL_MOBS_MAX; i++)
		n += m->mob[i].alive && m->mob[i].raider;
	return n;
}

void bl_mobs_set_deposits(struct bl_mobs *m, const int (*at)[3], int n)
{
	m->ndeposits = n < 0 ? 0 : n > BL_DEPOSITS_MAX ? BL_DEPOSITS_MAX : n;
	for (int i = 0; i < m->ndeposits; i++) {
		m->deposit[i].x = at[i][0];
		m->deposit[i].y = at[i][1];
		m->deposit[i].z = at[i][2];
	}
}

int bl_mobs_chief(const struct bl_mobs *m)
{
	for (int i = 0; i < BL_MOBS_MAX; i++)
		if (m->mob[i].alive && m->mob[i].kind == BL_MOB_CHIEF)
			return i;
	return -1;
}

bool bl_mobs_chief_raid(struct bl_mobs *m)
{
	int i = bl_mobs_chief(m);
	if (i < 0)
		return false;
	struct bl_mob *b = &m->mob[i];
	b->raider = true;
	b->retreat = false;
	b->raid0 = b->age;
	b->idle = b->lost = 0.0f;
	b->best_d = 1e9f;
	return true;
}

void bl_mobs_shot(struct bl_mobs *m, const float a[3], const float b[3], int kind)
{
	int ai = 0;
	for (int k = 1; k < BL_ARROWS_MAX; k++)
		if (m->arrow[k].t < m->arrow[ai].t)
			ai = k;
	m->arrow[ai].kind = kind;
	m->arrow[ai].t = kind == BL_SHOT_FIRE ? BL_FIRE_TIME : kind == BL_SHOT_LASER ? BL_LASER_TIME : BL_ARROW_TIME;
	memcpy(m->arrow[ai].a, a, sizeof m->arrow[ai].a);
	memcpy(m->arrow[ai].b, b, sizeof m->arrow[ai].b);
}

int bl_mobs_splash(struct bl_mobs *m, const float at[3], float radius, float damage, int spare)
{
	int dead = 0;
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		const struct bl_mob *b = &m->mob[i];
		if (!b->alive || i == spare || !bl_mob_is_foe(b->kind))
			continue;
		const float dx = b->x - at[0], dy = b->y + 0.8f - at[1], dz = b->z - at[2];
		if (dx * dx + dy * dy + dz * dz > radius * radius)
			continue;
		dead += bl_mobs_hurt(m, i, damage, dx, dz);
	}
	return dead;
}

static void turn_back(struct bl_mob *b)
{
	if (b->retreat)
		return;
	b->retreat = true;
	b->since = b->age;
	b->lost = 0.0f;
	b->best_d = 1e9f;
}

void bl_mobs_raid_retreat(struct bl_mobs *m)
{
	for (int i = 0; i < BL_MOBS_MAX; i++)
		if (m->mob[i].alive && m->mob[i].raider)
			turn_back(&m->mob[i]);
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

/* where a mob means to walk in this step */
struct goal {
	bool on;
	float x, y, z, radius;          /* get within `radius` of it (along the ground) ... */
	bool any_height;                /* ... at its height give or take 3, or at any */
};

static float clampf(float v, float lo, float hi);

/* the goal of one who walks to a keep: the point of its side that is nearest to him */
static struct goal keep_goal(const float keep[3], const struct bl_mob *b, float radius)
{
	const float x = b->x < keep[0] - BL_KEEP_EDGE ? keep[0] - BL_KEEP_EDGE : b->x > keep[0] + BL_KEEP_EDGE ? keep[0] + BL_KEEP_EDGE : b->x;
	const float z = b->z < keep[2] - BL_KEEP_EDGE ? keep[2] - BL_KEEP_EDGE : b->z > keep[2] + BL_KEEP_EDGE ? keep[2] + BL_KEEP_EDGE : b->z;
	return (struct goal){ true, x, keep[1], z, radius, false };
}

static void path_forget(struct bl_mob *b)
{
	b->nway = b->iway = 0;
	b->pathing = false;
	b->line_t = b->repath_t = 0.0f;
	b->lost = 0.0f;
}

/* ---- the body --------------------------------------------------------------------------------------------------------- */

/* where the mob went (an index of M->mob), or -1 */
static int place_mob_i(struct bl_mobs *M, const struct bl_world *w, int kind, bool brute, bool raider, float x, float y, float z)
{
	if (kind < BL_MOB_GOBLIN || kind > BL_MOB_CHIEF || (kind == BL_MOB_CHIEF && bl_mobs_chief(M) >= 0) || (brute && kind != BL_MOB_GOBLIN) || !bl_inside(w, (int)floorf(x), (int)floorf(y), (int)floorf(z)) ||
	    (bl_mob_is_foe(kind) ? bl_box_hits_foe : bl_box_hits_unit)(w, x, y, z, BL_GOBLIN_HW, brute ? BL_BRUTE_H : bl_mob_h(kind)))
		return -1;
	if (kind == BL_MOB_WORKER && bl_mobs_count_kind(M, BL_MOB_WORKER) >= BL_WORKER_MAX)
		return -1;
	if (kind == BL_MOB_SOLDIER && bl_mobs_count_kind(M, BL_MOB_SOLDIER) >= BL_SOLDIER_MAX)
		return -1;
	if (kind == BL_MOB_GUARD && (raider ? bl_mobs_count_raiders(M) >= BL_RAIDER_MAX : bl_mobs_count_kind(M, BL_MOB_GUARD) - bl_mobs_count_raiders(M) >= BL_GUARD_MAX))
		return -1;
	if (kind == BL_MOB_RWORKER && bl_mobs_count_kind(M, BL_MOB_RWORKER) >= BL_RWORKER_MAX)
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
			b->raider = raider;
			b->best_d = 1e9f;
			b->site = -1;
			b->hp = brute ? BL_BRUTE_HP : kind == BL_MOB_RWORKER ? BL_RWORKER_HP : kind == BL_MOB_WORKER ? BL_WORKER_HP : kind == BL_MOB_SOLDIER ? BL_SOLDIER_HP : kind == BL_MOB_GUARD ? BL_GUARD_HP : kind == BL_MOB_CHIEF ? BL_CHIEF_HP : BL_GOBLIN_HP;
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
	return place_mob_i(M, w, kind, false, false, x, y, z) >= 0;
}

bool bl_mobs_add(struct bl_mobs *m, const struct bl_world *w, int kind, float x, float y, float z)
{
	return place_mob(m, w, kind, x, y, z);
}

bool bl_mobs_add_brute(struct bl_mobs *m, const struct bl_world *w, float x, float y, float z)
{
	return place_mob_i(m, w, BL_MOB_GOBLIN, true, false, x, y, z) >= 0;
}

bool bl_mobs_add_chief(struct bl_mobs *m, const struct bl_world *w, float x, float y, float z)
{
	return place_mob(m, w, BL_MOB_CHIEF, x, y, z);
}

bool bl_mobs_add_raider(struct bl_mobs *m, const struct bl_world *w, float x, float y, float z)
{
	return place_mob_i(m, w, BL_MOB_GUARD, false, true, x, y, z) >= 0;
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

/* One goblin (a brute) out of the ground `dmin` .. `dmax` from c, in the open, and no nearer than SPAWN_MIN to `away` (the player) when that is given. */
static bool spawn_round(struct bl_mobs *M, const struct bl_world *w, const float c[3], float dmin, float dmax, const float *away, bool brute, int ntries,
                        struct bl_mobs_result *R)
{
	for (int tries = 0; tries < ntries; tries++) {
		float a = rf(&M->rng) * 6.2831853f, d = dmin + rf(&M->rng) * (dmax - dmin);
		int x = (int)floorf(c[0] + sinf(a) * d), z = (int)floorf(c[2] + cosf(a) * d);
		float y;
		if (!surface_at(w, x, z, &y) || fabsf(y - c[1]) > 14.0f || bl_water(bl_get(w, x, (int)y, z)))
			continue;
		if (away && hypotf((float)x + 0.5f - away[0], (float)z + 0.5f - away[2]) < SPAWN_MIN)
			continue;
		if (place_mob_i(M, w, BL_MOB_GOBLIN, brute, false, (float)x + 0.5f, y, (float)z + 0.5f) >= 0) {
			M->spawned++;
			R->born++;
			if (brute)
				R->born_brutes++;
			return true;
		}
	}
	return false;
}

/* one of the player's units (not one up on a watchtower) that is more than BL_UNIT_APART from `from`, picked by chance; NULL when there is none */
static const struct bl_mob *unit_apart(struct bl_mobs *M, const float from[3])
{
	const struct bl_mob *out[BL_MOBS_MAX];
	int n = 0;
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		const struct bl_mob *o = &M->mob[i];
		if (o->alive && bl_mob_is_unit(o->kind) && !o->posted && hypotf(o->x - from[0], o->z - from[2]) > BL_UNIT_APART)
			out[n++] = o;
	}
	return n ? out[(int)(rf(&M->rng) * (float)n) % n] : NULL;
}

/* the goblins that are out among units that are away from `from` */
static int goblins_raiding(const struct bl_mobs *M, const float from[3])
{
	int n = 0;
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		const struct bl_mob *g = &M->mob[i];
		if (!g->alive || g->kind != BL_MOB_GOBLIN)
			continue;
		for (int k = 0; k < BL_MOBS_MAX; k++) {
			const struct bl_mob *o = &M->mob[k];
			if (o->alive && bl_mob_is_unit(o->kind) && !o->posted && hypotf(o->x - from[0], o->z - from[2]) > BL_UNIT_APART &&
			    hypotf(o->x - g->x, o->z - g->z) < BL_UNIT_APART) {
				n++;
				break;
			}
		}
	}
	return n;
}

/* At night, with no wave: round the player, at most `cap` of them; and round units that are away from him BL_RAID_CAP more, which the ones that crowd round
 * the player do not use up. */
static void try_spawn(struct bl_mobs *M, const struct bl_world *w, const float pl[3], int cap, struct bl_mobs_result *R)
{
	int raiding = goblins_raiding(M, pl);
	bool home = bl_mobs_count_kind(M, BL_MOB_GOBLIN) - raiding < cap;
	const struct bl_mob *u = raiding < BL_RAID_CAP ? unit_apart(M, pl) : NULL;
	if (u && (!home || rf(&M->rng) < 0.5f)) {
		float at[3] = { u->x, u->y, u->z };
		if (spawn_round(M, w, at, RAID_MIN, RAID_MAX, pl, false, 10, R))
			return;
	}
	if (home)
		spawn_round(M, w, pl, SPAWN_MIN, SPAWN_MAX, NULL, false, 10, R);
}

/* a goblin of a wave: out of the ground 20 to 32 blocks from the keep, in the open, anywhere round it; every third one goes for units that are away from it */
static void try_spawn_wave(struct bl_mobs *M, const struct bl_world *w, const float pl[3], struct bl_mobs_result *R)
{
	bool brute = M->wave_brutes > 0 && M->wave_left <= M->wave_brutes;           /* the brutes come last */
	const struct bl_mob *u = unit_apart(M, M->keep);
	bool born = false;
	if (u && rf(&M->rng) < 0.3333f) {
		float at[3] = { u->x, u->y, u->z };
		born = spawn_round(M, w, at, RAID_MIN, RAID_MAX, pl, brute, 10, R);
	}
	if (!born)
		born = spawn_round(M, w, M->keep, WAVE_MIN, WAVE_MAX, NULL, brute, 16, R);
	if (born) {
		M->wave_left--;
		if (brute)
			M->wave_brutes--;
	}
}

/* a goblin is stopped by a gate and by leaves, the player's units by neither */
static bool mob_hits(const struct bl_world *w, const struct bl_mob *b, float x, float y, float z, float hw, float h)
{
	return bl_mob_is_foe(b->kind) ? bl_box_hits_foe(w, x, y, z, hw, h) : bl_box_hits_unit(w, x, y, z, hw, h);
}

static bool mob_move(const struct bl_world *w, struct bl_mob *b, int axis, float d, float h)
{
	return bl_mob_is_foe(b->kind) ? bl_box_move_foe(w, &b->x, &b->y, &b->z, axis, d, BL_GOBLIN_HW, h) : bl_box_move_unit(w, &b->x, &b->y, &b->z, axis, d, BL_GOBLIN_HW, h);
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
	path_forget(b);
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
	path_forget(b);
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
	path_forget(b);
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
	if (i < 0 || i >= BL_MOBS_MAX || !m->mob[i].alive || (m->mob[i].kind != BL_MOB_SOLDIER && m->mob[i].kind != BL_MOB_GUARD) || bl_get(w, x, y, z) != BL_TOWERPOST)
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
	path_forget(b);
	b->gx = x;
	b->gy = y;
	b->gz = z;
	b->tx = (float)x + 0.5f;
	b->tz = (float)z + 0.5f;
	b->stuck = 0.0f;
	b->best_d = 1e9f;
	return true;                    /* (one that stands on another tower comes down first: mob_step) */
}

bool bl_mobs_add_archer(struct bl_mobs *m, const struct bl_world *w, int x, int y, int z)
{
	if (bl_get(w, x, y, z) != BL_TOWERPOST)
		return false;
	int i = place_mob_i(m, w, BL_MOB_GUARD, false, false, (float)x + 0.5f, (float)y + 1.002f, (float)z + 0.5f);
	if (i < 0)
		return false;
	struct bl_mob *b = &m->mob[i];
	b->order = BL_ORDER_TOWER;
	b->gx = x;
	b->gy = y;
	b->gz = z;
	b->tx = b->x;
	b->tz = b->z;
	b->posted = true;
	return true;
}

/* is there somebody in the cell (a block is not put into a body)? */
static bool cell_busy(const struct bl_mobs *M, const float pl[3], bool pl_alive, const struct bl_cell *c)
{
	if (pl_alive && pl[0] + 0.3f > (float)c->x && pl[0] - 0.3f < (float)c->x + 1.0f && pl[2] + 0.3f > (float)c->z && pl[2] - 0.3f < (float)c->z + 1.0f &&
	    pl[1] + 1.8f > (float)c->y && pl[1] < (float)c->y + 1.0f)
		return true;
	return bl_mobs_hit_cell(M, c->x, c->y, c->z);
}

/* is the body in the cell (seen from the side, too)? */
static bool body_in(const struct bl_mob *b, const struct bl_cell *c)
{
	return b->x + BL_GOBLIN_HW > (float)c->x && b->x - BL_GOBLIN_HW < (float)c->x + 1.0f && b->z + BL_GOBLIN_HW > (float)c->z && b->z - BL_GOBLIN_HW < (float)c->z + 1.0f &&
	       b->y + bl_mob_height(b) > (float)c->y && b->y < (float)c->y + 1.0f;
}

/* A builder at its site: it walks to its spot beside it and then puts a block every `every` seconds, lowest first, none into a body. */
static float site_work(struct bl_mobs *M, struct bl_mob *b, const struct bl_world *w, const struct bl_site *s, float every, float speed, const float pl[3], bool pl_alive,
                       struct bl_mobs_result *R, float *want)
{
	const float dt = STEP;
	float dx = b->tx - b->x, dz = b->tz - b->z, d = hypotf(dx, dz);
	if (!b->at_site && (d <= 0.5f || (b->stuck > 1.0f && site_dist(s, b->x, b->z) < BUILD_NEAR))) {
		bool in = false;                    /* it does not start while it stands where a block goes: on, to the middle of its spot (or it would stay there for good) */
		for (int k = 0; k < s->n && !in && d > 0.12f; k++)
			in = body_in(b, &s->c[k]) && bl_cell_missing(w, &s->c[k]);
		b->at_site = !in;
	}
	if (!b->at_site) {
		*want = atan2f(dx, dz);
		return d <= 0.6f ? 0.8f : clampf(3.0f * (d - 0.3f), 0.0f, speed);
	}
	int lo[3], hi[3];
	bl_site_box(s, lo, hi);
	*want = atan2f(0.5f * (float)(lo[0] + hi[0] + 1) - b->x, 0.5f * (float)(lo[2] + hi[2] + 1) - b->z);
	b->work += dt;
	if (fmodf(b->work, 0.55f) < dt)
		b->swing = 1.0f;
	if (b->work < every || R->nplace >= (int)(sizeof R->place / sizeof R->place[0]))
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
			in_my_way |= body_in(b, c);
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

static float build_think(struct bl_mobs *M, struct bl_mob *b, const struct bl_world *w, const float pl[3], bool pl_alive, struct bl_mobs_result *R, float *want)
{
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
	return site_work(M, b, w, &M->site[b->site], BL_BUILD_TIME, BL_WORKER_SPEED, pl, pl_alive, R, want);
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

/* A blow or an arrow of mob `by` at mob `at`: what it takes, and who is counted when it dies (a goblin the player's own kill is a kill of his). */
static void strike(struct bl_mobs *M, const struct bl_mob *by, struct bl_mob *at, float damage, struct bl_mobs_result *R)
{
	at->hp -= damage;
	at->hurt = 0.35f;
	if (at->hp > 0.0f)
		return;
	at->alive = false;
	if (at->kind == BL_MOB_WORKER)
		R->workers_lost++;
	else if (at->kind == BL_MOB_SOLDIER)
		R->soldiers_lost++;
	else if (bl_mob_is_unit(by->kind)) {
		if (at->kind == BL_MOB_GOBLIN) {
			M->kills++;
			R->slain++;
		} else if (at->kind == BL_MOB_CHIEF) {
			R->chief_slain++;
		} else if (at->kind == BL_MOB_RWORKER) {
			R->rworkers_slain++;
		} else {
			R->guards_slain++;
			R->raiders_slain += at->raider;
		}
	}
}

/* whom the one fights: the player's own the goblins and the rival clan, a guard everybody who is not of his clan */
static bool fights(int kind, int other)
{
	return bl_mob_is_rival(kind) ? !bl_mob_is_rival(other) : bl_mob_is_foe(other);
}

/* a soldier (or a rival guard) on a watchtower: the nearest enemy it can see gets an arrow; a guard's may be meant for the player (`pl`, NULL for the player's own) */
static void archer(struct bl_mobs *M, const struct bl_world *w, struct bl_mob *b, const float *pl, struct bl_mobs_result *R)
{
	int best = -1;                                          /* a mob, or -2: the player */
	float bd = BL_BOW_RANGE, from[3] = { 0 }, to[3] = { 0 };
	for (int k = pl ? -1 : 0; k < BL_MOBS_MAX; k++) {
		const struct bl_mob *o = k < 0 ? NULL : &M->mob[k];
		if (o && (!o->alive || !fights(b->kind, o->kind)))
			continue;
		float ox = o ? o->x : pl[0], oy = o ? o->y : pl[1], oz = o ? o->z : pl[2];
		float ex = ox - b->x, ez = oz - b->z, e = hypotf(ex, ez);
		if (e >= bd)
			continue;
		/* from the edge of the platform towards it, at the eyes */
		float out = e > 1.6f ? 1.6f : 0.0f, inv = e > 0.01f ? 1.0f / e : 0.0f;
		float a[3] = { b->x + ex * inv * out, b->y + 1.5f, b->z + ez * inv * out };
		float t[3] = { ox, oy + 1.0f, oz };
		float dir[3] = { t[0] - a[0], t[1] - a[1], t[2] - a[2] };
		float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
		struct bl_hit h;
		if (len > 0.6f && bl_pick(w, a, dir, len - 0.4f, &h))
			continue;
		bd = e;
		best = o ? k : -2;
		memcpy(from, a, sizeof from);
		memcpy(to, t, sizeof to);
	}
	b->speed = 0.0f;
	if (best == -1)
		return;
	b->yaw = atan2f(to[0] - b->x, to[2] - b->z);
	b->ycos = cosf(b->yaw);
	b->ysin = sinf(b->yaw);
	if (b->attack_cd > 0.0f)
		return;
	b->attack_cd = BL_BOW_COOLDOWN;
	b->swing = 1.0f;
	R->shots++;
	bl_mobs_shot(M, from, to, BL_SHOT_ARROW);
	if (best == -2) {
		R->damage += BL_BOW_DAMAGE;
		R->hits++;
		R->guard_hits++;
	} else {
		strike(M, b, &M->mob[best], BL_BOW_DAMAGE, R);
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

/* Which way to walk for a goal: `want` is left as it is (straight at it) when that way is open, else it is turned along a way found round what is in the way.
 * True while it follows such a way. */
static bool steer(struct bl_mobs *M, struct bl_mob *b, const struct bl_world *w, const struct goal *g, float *want)
{
	const struct bl_walk k = { bl_mob_is_foe(b->kind), (int)ceilf(bl_mob_height(b) - 0.01f) };
	const float from[3] = { b->x, b->y, b->z }, to[3] = { g->x, g->y, g->z };
	b->line_t -= STEP;
	b->repath_t -= STEP;
	b->path_age += STEP;
	if (b->line_t <= 0.0f) {
		b->direct = bl_walk_line(w, &k, from, to, g->any_height);
		b->line_t = 0.25f;
	}
	if (b->direct && b->blocked < 0.3f) {
		if (b->nway) {                  /* round the obstacle: from here on it is judged by how it gets nearer again, not by how near it was before it set off */
			b->best_d = 1e9f;
			b->stuck = 0.0f;
		}
		b->nway = b->iway = 0;
		return false;
	}
	bool moved = hypotf(g->x - b->goal_x, g->z - b->goal_z) > 2.0f;
	if ((b->nway == 0 || b->iway >= b->nway || moved || b->blocked > 0.5f || b->path_age > 4.0f) && b->repath_t <= 0.0f && M->path_budget > 200) {
		bool full = false;
		int steps = 0, used = 0;
		b->nway = bl_walk_find(w, &k, from, to, g->any_height, g->radius, M->path_budget, b->way, BL_PATH_MAX, &full, &steps, &used);
		b->iway = 0;
		b->way_d = 1e9f;
		b->way_full = full;
		M->path_budget -= used + 50;
		M->paths++;
		b->repath_t = 0.6f;
		b->path_age = 0.0f;
		b->blocked = 0.0f;
		b->goal_x = g->x;
		b->goal_z = g->z;
		/* a goblin goes round only when that gets it there and is no long way round: else straight at what it is after, breaking what is in the way */
		float line = hypotf(g->x - b->x, g->z - b->z);
		if ((b->kind == BL_MOB_GOBLIN || (b->raider && !b->retreat)) && ((!full && line < 24.0f) || (full && (float)steps > 2.5f * line + 12.0f)))
			b->nway = 0;
	}
	while (b->iway < b->nway) {
		const struct bl_way *p = &b->way[b->iway];
		float dx = (float)p->x + 0.5f - b->x, dz = (float)p->z + 0.5f - b->z;
		float left = hypotf(dx, dz);
		if (left < 0.4f && fabsf(b->y - (float)p->y) < 1.2f) {
			b->iway++;
			b->way_d = 1e9f;
			continue;
		}
		if (left < b->way_d - 0.25f) {
			b->way_d = left;
			if (b->way_full)
				b->stuck = 0.0f;                        /* (it is getting on, even when that takes it away from the goal) */
		}
		*want = atan2f(dx, dz);
		return true;
	}
	return false;
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

/* the block a worker has finished (`stack` of them for a tree's trunk, and then the crown goes too) into a list the caller takes away */
static void take_stack(const struct bl_world *w, const struct bl_mob *b, int stack, struct bl_break *out, int *n, int max)
{
	for (int k = 0; k < stack && *n < max; k++)
		out[(*n)++] = (struct bl_break){ b->gx, b->gy + k, b->gz };
	if (b->gblock == BL_LOG)                                            /* the crown of the tree goes too */
		for (int ly = b->gy + stack - 3; ly <= b->gy + stack + 1; ly++)
			for (int lz = b->gz - 2; lz <= b->gz + 2; lz++)
				for (int lx = b->gx - 2; lx <= b->gx + 2; lx++)
					if (bl_get(w, lx, ly, lz) == BL_LEAVES && *n < max)
						out[(*n)++] = (struct bl_break){ lx, ly, lz };
}

/* what a worker does with the time: nothing, walks to a point, or works on a block. Fills in where it wants to go and how fast; returns the speed it asks for. */
static float worker_think(struct bl_mobs *M, struct bl_mob *b, const struct bl_world *w, const float pl[3], bool pl_alive, struct bl_mobs_result *R, float *want,
                          struct goal *g)
{
	const float dt = STEP;
	if (b->order == BL_ORDER_MOVE) {
		float dx = b->tx - b->x, dz = b->tz - b->z, d = hypotf(dx, dz);
		if (d < 0.4f) {
			b->order = BL_ORDER_IDLE;
			return 0.0f;
		}
		*want = atan2f(dx, dz);
		*g = (struct goal){ true, b->tx, b->y, b->tz, 0.4f, true };
		return clampf(3.0f * (d - 0.3f), 0.0f, BL_WORKER_SPEED);
	}
	if (b->order == BL_ORDER_BUILD) {
		float v = build_think(M, b, w, pl, pl_alive, R, want);
		if (b->order == BL_ORDER_BUILD && !b->at_site)
			*g = (struct goal){ true, b->tx, b->y, b->tz, 0.5f, true };
		return v;
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
		*g = (struct goal){ true, b->tx, (float)b->gy, b->tz, 0.5f, false };
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
			take_stack(w, b, stack, R->brk, &R->nbreak, (int)(sizeof R->brk / sizeof R->brk[0]));
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
static float soldier_think(struct bl_mobs *M, struct bl_mob *b, const struct bl_world *w, struct bl_mobs_result *R, float *want, struct goal *g)
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
			*g = (struct goal){ true, (float)b->gx + 0.5f, b->y, (float)b->gz + 0.5f, 2.2f, true };
			return BL_SOLDIER_SPEED;
		}
	}
	/* the nearest enemy that fights, and the nearest of their workers: one of those is gone after only when there is nobody else, and not by a soldier who was
	 * sent to their keep (that is what he is there for) */
	const bool at_keep = M->rival_on && !M->rkeep_down && hypotf(b->tx - M->rkeep[0], b->tz - M->rkeep[2]) < BL_RIVAL_AT_KEEP;
	int found[2] = { -1, -1 };
	float near[2] = { 1e9f, 1e9f };
	for (int k = 0; k < BL_MOBS_MAX; k++) {
		const struct bl_mob *o = &M->mob[k];
		if (!o->alive || !bl_mob_is_foe(o->kind) || o->posted || fabsf(o->y - b->y) > 6.0f)
			continue;
		float e = hypotf(o->x - b->x, o->z - b->z);
		int c = o->kind == BL_MOB_RWORKER;
		if (e < BL_SOLDIER_SEE && e < near[c] && (e < 3.0f || hypotf(o->x - b->tx, o->z - b->tz) < BL_SOLDIER_LEASH)) {  /* (one at its side it fights wherever it is going) */
			near[c] = e;
			found[c] = k;
		}
	}
	const int gi = found[0] >= 0 || at_keep ? found[0] : found[1];
	const float gd = gi >= 0 ? near[gi == found[1]] : 1e9f;
	const float gdx = gi >= 0 ? M->mob[gi].x - b->x : 0.0f, gdz = gi >= 0 ? M->mob[gi].z - b->z : 0.0f;
	if (gi >= 0) {
		*want = atan2f(gdx, gdz);
		if (gd < BL_SOLDIER_REACH && fabsf(M->mob[gi].y - b->y) < 2.0f && b->attack_cd <= 0.0f) {       /* (a chief in the air is out of reach) */
			struct bl_mob *o = &M->mob[gi];
			b->attack_cd = BL_SOLDIER_COOLDOWN;
			b->swing = 1.0f;
			R->sword_hits++;
			float inv = gd > 0.01f ? 1.0f / gd : 0.0f;
			o->kx += gdx * inv * 6.0f;
			o->kz += gdz * inv * 6.0f;
			if (o->ground) {
				o->vy = 4.2f;
				o->ground = false;
			}
			strike(M, b, o, BL_SOLDIER_DAMAGE, R);
		}
		*g = (struct goal){ true, M->mob[gi].x, M->mob[gi].y, M->mob[gi].z, 0.9f, false };
		return clampf(3.0f * (gd - 0.9f), 0.0f, BL_SOLDIER_SPEED);
	}
	if (at_keep) {                                                                          /* sent to the rival keep: at it */
		float kx = M->rkeep[0] - b->x, kz = M->rkeep[2] - b->z, kd = bl_keep_dist(M->rkeep, b->x, b->z);
		*want = atan2f(kx, kz);
		if (kd < 1.2f && fabsf(M->rkeep[1] - b->y) < 2.6f) {
			if (b->attack_cd <= 0.0f) {
				b->attack_cd = BL_SOLDIER_COOLDOWN;
				b->swing = 1.0f;
				R->rkeep_damage += BL_SOLDIER_KEEP_HIT;
			}
			return 0.0f;
		}
		*g = keep_goal(M->rkeep, b, 0.7f);
		return clampf(3.0f * (kd - 0.6f), 0.0f, BL_SOLDIER_SPEED);
	}
	float dx = b->tx - b->x, dz = b->tz - b->z, d = hypotf(dx, dz);
	if (d > 1.0f) {
		*want = atan2f(dx, dz);
		*g = (struct goal){ true, b->tx, b->y, b->tz, 0.8f, true };
		return clampf(3.0f * (d - 0.5f), 0.0f, BL_SOLDIER_SPEED);
	}
	return 0.0f;
}

/* The enemy of a guard nearest to him that he sees (BL_GUARD_SEE, or `see`): a mob's index, -2 the player, -1 nobody. `leash`: not one farther from his post
 * than that, unless it is at his side (0 = he has no post). */
static int guard_enemy(const struct bl_mobs *M, const struct bl_mob *b, const float pl[3], bool pl_alive, float see, float leash, float *dist, float at[3])
{
	int gi = -1;
	float gd = 1e9f;
	for (int k = pl_alive ? -1 : 0; k < BL_MOBS_MAX; k++) {
		const struct bl_mob *o = k < 0 ? NULL : &M->mob[k];
		if (o && (!o->alive || bl_mob_is_rival(o->kind) || o->posted))
			continue;
		float ox = o ? o->x : pl[0], oy = o ? o->y : pl[1], oz = o ? o->z : pl[2];
		float e = hypotf(ox - b->x, oz - b->z);
		if (fabsf(oy - b->y) < 6.0f && e < see && e < gd && (leash <= 0.0f || e < 3.0f || hypotf(ox - b->tx, oz - b->tz) < leash)) {
			gd = e;
			gi = o ? k : -2;
			at[0] = ox;
			at[1] = oy;
			at[2] = oz;
		}
	}
	*dist = gd;
	return gi;
}

/* he goes at it, and strikes when it is near enough; returns the speed he asks for */
static float guard_fight(struct bl_mobs *M, struct bl_mob *b, int gi, float gd, const float at[3], struct bl_mobs_result *R, float *want, struct goal *g)
{
	float dx = at[0] - b->x, dz = at[2] - b->z;
	*want = atan2f(dx, dz);
	if (gd < BL_SOLDIER_REACH && fabsf(at[1] - b->y) < 2.0f && b->attack_cd <= 0.0f) {
		float inv = gd > 0.01f ? 1.0f / gd : 0.0f;
		b->attack_cd = BL_GUARD_COOLDOWN;
		b->swing = 1.0f;
		if (gi == -2) {
			R->damage += BL_GUARD_HIT;
			R->hits++;
			R->guard_hits++;
			R->kbx += dx * inv * 5.0f;
			R->kbz += dz * inv * 5.0f;
		} else {
			struct bl_mob *o = &M->mob[gi];
			o->kx += dx * inv * 5.0f;
			o->kz += dz * inv * 5.0f;
			strike(M, b, o, BL_GUARD_HIT, R);
		}
	}
	*g = (struct goal){ true, at[0], at[1], at[2], 0.9f, false };
	return clampf(3.0f * (gd - 0.9f), 0.0f, BL_GUARD_SPEED);
}

/* a rival guard: after the nearest enemy near it and near its post (the player, his people, a goblin), else back to its post */
static float guard_think(struct bl_mobs *M, struct bl_mob *b, const float pl[3], bool pl_alive, struct bl_mobs_result *R, float *want, struct goal *g)
{
	float gd, at[3] = { 0 };
	int gi = guard_enemy(M, b, pl, pl_alive, BL_GUARD_SEE, BL_GUARD_LEASH, &gd, at);
	if (gi != -1)
		return guard_fight(M, b, gi, gd, at, R, want, g);
	float dx = b->tx - b->x, dz = b->tz - b->z, d = hypotf(dx, dz);
	if (d > 1.0f) {
		*want = atan2f(dx, dz);
		*g = (struct goal){ true, b->tx, b->y, b->tz, 0.8f, true };
		return clampf(3.0f * (d - 0.5f), 0.0f, BL_GUARD_SPEED * 0.7f);
	}
	return 0.0f;
}

/* is it getting nearer to where it is going (`d` away)? The seconds it has not are counted. */
static void raid_progress(struct bl_mob *b, float d)
{
	if (d < b->best_d - 1.0f) {
		b->best_d = d;
		b->lost = 0.0f;
	} else {
		b->lost += STEP;
	}
}

/* A raider: whoever it meets it fights; else it goes to where the raid is bound and strikes the player's keep (with none: goes after the player and his people
 * near there); with nothing to do, or getting nowhere, it goes home. */
static float raider_think(struct bl_mobs *M, struct bl_mob *b, const float pl[3], bool pl_alive, struct bl_mobs_result *R, float *want, struct goal *g)
{
	if (!b->retreat && b->age - b->raid0 > BL_RAID_LIMIT)
		turn_back(b);
	float gd, at[3] = { 0 };
	int gi = guard_enemy(M, b, pl, pl_alive, b->retreat ? 3.0f : BL_GUARD_SEE, 0.0f, &gd, at);    /* (on the way home: only who is at its side) */
	if (gi != -1) {
		if (gd < 2.0f && fabsf(at[1] - b->y) < 2.0f) {                  /* (one it can get at: it is not idle and not lost) */
			b->idle = b->lost = 0.0f;
		} else {
			raid_progress(b, gd);
			if (!b->retreat && b->lost > BL_RAID_IDLE)
				turn_back(b);
		}
		return guard_fight(M, b, gi, gd, at, R, want, g);
	}
	if (b->retreat) {
		float hx = M->rkeep[0] - b->x, hz = M->rkeep[2] - b->z, hd = hypotf(hx, hz);
		raid_progress(b, hd);
		if (!M->rival_on || hd < BL_RAID_HOME || b->lost > BL_RAID_IDLE || b->age - b->since > BL_RAID_BACK) {
			b->alive = false;
			R->raiders_home++;
			return 0.0f;
		}
		*want = atan2f(hx, hz);
		*g = (struct goal){ true, M->rkeep[0], M->rkeep[1], M->rkeep[2], 6.0f, true };
		return BL_GUARD_SPEED;
	}
	if (M->keep_on) {
		float kx = M->keep[0] - b->x, kz = M->keep[2] - b->z, kd = bl_keep_dist(M->keep, b->x, b->z);
		*want = atan2f(kx, kz);
		if (kd < 1.2f && fabsf(M->keep[1] - b->y) < 2.6f) {
			b->idle = b->lost = 0.0f;
			if (b->attack_cd <= 0.0f) {
				b->attack_cd = BL_GUARD_COOLDOWN;
				b->swing = 1.0f;
				R->keep_damage += BL_RAIDER_KEEP_HIT;
				R->keep_hits++;
			}
			return 0.0f;
		}
		raid_progress(b, kd);
		if (b->lost > BL_RAID_IDLE)
			turn_back(b);
		*g = keep_goal(M->keep, b, 0.7f);
		return clampf(3.0f * (kd - 0.6f), 0.0f, BL_GUARD_SPEED);
	}
	/* no keep to strike: the nearest of the player and his people who is near where the raid is bound */
	float best = 1e9f, tx = M->raid_at[0], ty = M->raid_at[1], tz = M->raid_at[2];
	bool somebody = false;
	for (int k = pl_alive ? -1 : 0; k < BL_MOBS_MAX; k++) {
		const struct bl_mob *o = k < 0 ? NULL : &M->mob[k];
		if (o && (!o->alive || !bl_mob_is_unit(o->kind) || o->posted))
			continue;
		float ox = o ? o->x : pl[0], oy = o ? o->y : pl[1], oz = o ? o->z : pl[2];
		float e = hypotf(ox - b->x, oz - b->z);
		if (e < best && fabsf(oy - M->raid_at[1]) < 12.0f && hypotf(ox - M->raid_at[0], oz - M->raid_at[2]) < BL_RAID_HUNT) {
			best = e;
			tx = ox;
			ty = oy;
			tz = oz;
			somebody = true;
		}
	}
	float dx = tx - b->x, dz = tz - b->z, d = hypotf(dx, dz);
	if (!somebody && d < 5.0f) {                                    /* there, and nobody about: it waits a while */
		b->lost = 0.0f;
		b->idle += STEP;
		if (b->idle > BL_RAID_IDLE)
			turn_back(b);
		return 0.0f;
	}
	raid_progress(b, d);
	if (b->lost > BL_RAID_IDLE)
		turn_back(b);
	*want = atan2f(dx, dz);
	*g = (struct goal){ true, tx, ty, tz, somebody ? 0.9f : 3.0f, !somebody };
	return clampf(3.0f * (d - 0.9f), 0.0f, BL_GUARD_SPEED);
}

/* ---- the rival chief --------------------------------------------------------------------------------------------------- */

/* how many of the player's side are within `r` of the place */
static int crowd(const struct bl_mobs *M, const float at[3], float r, const float pl[3], bool pl_alive)
{
	int n = pl_alive && hypotf(pl[0] - at[0], pl[2] - at[2]) < r;
	for (int i = 0; i < BL_MOBS_MAX; i++)
		n += M->mob[i].alive && bl_mob_is_unit(M->mob[i].kind) && hypotf(M->mob[i].x - at[0], M->mob[i].z - at[2]) < r;
	return n;
}

static bool chief_sees(const struct bl_world *w, const struct bl_mob *b, const float to[3])
{
	const float a[3] = { b->x, b->y + 1.6f, b->z }, dir[3] = { to[0] - a[0], to[1] + 1.0f - a[1], to[2] - a[2] };
	const float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	struct bl_hit h;
	return len < 0.8f || !bl_pick(w, a, dir, len - 0.5f, &h);
}

static void chief_shot(struct bl_mobs *M, const struct bl_mob *b, const float to[3], float up, int weapon)
{
	const struct bl_weapon_info *wi = bl_weapon(weapon);
	if (wi->shot < 0)
		return;
	const float a[3] = { b->x + b->ysin * 0.4f, b->y + 1.35f, b->z + b->ycos * 0.4f }, t[3] = { to[0], to[1] + up, to[2] };
	bl_mobs_shot(M, a, t, wi->shot);
}

/* what he has done to the one he struck (-2: the player), and to those beside him */
static void chief_hit(struct bl_mobs *M, struct bl_mob *b, int gi, const float at[3], const float pl[3], bool pl_alive, struct bl_mobs_result *R)
{
	const struct bl_weapon_info *wi = bl_weapon(b->weapon);
	const float dx = at[0] - b->x, dz = at[2] - b->z, d = hypotf(dx, dz), inv = d > 0.01f ? 1.0f / d : 0.0f;
	for (int k = pl_alive ? -1 : 0; k < BL_MOBS_MAX; k++) {
		const int who = k < 0 ? -2 : k;
		const struct bl_mob *o = k < 0 ? NULL : &M->mob[k];
		if (o && (!o->alive || bl_mob_is_rival(o->kind)))
			continue;
		if (who != gi && (wi->splash <= 0.0f || (o && !bl_mob_is_unit(o->kind)) || hypotf((o ? o->x : pl[0]) - at[0], (o ? o->z : pl[2]) - at[2]) > wi->splash ||
		                  fabsf((o ? o->y : pl[1]) - at[1]) > wi->splash))
			continue;
		if (!o) {
			R->damage += wi->c_damage;
			R->hits++;
			R->chief_hits++;
			R->kbx += dx * inv * 5.0f;
			R->kbz += dz * inv * 5.0f;
		} else {
			M->mob[k].kx += dx * inv * 4.0f;
			M->mob[k].kz += dz * inv * 4.0f;
			strike(M, b, &M->mob[k], wi->c_damage, R);
		}
	}
}

/* The chief: whom he sees he goes for, with the weapon that suits the distance; else he guards his keep, or, leading a raid, strikes the player's. He flies when
 * he has somewhere to get to, over walls and water. Returns the speed along the ground he asks for; *up is the height he wants to fly at (b->flying). */
static float chief_think(struct bl_mobs *M, struct bl_mob *b, const struct bl_world *w, const float pl[3], bool pl_alive, struct bl_mobs_result *R, float *want,
                         float *up)
{
	if (b->raider && !b->retreat && (b->age - b->raid0 > BL_RAID_LIMIT || b->hp < BL_CHIEF_HP * 0.35f))
		turn_back(b);
	float gd, at[3] = { 0 };
	const float see = b->retreat ? 6.0f : BL_CHIEF_SEE;
	int gi = guard_enemy(M, b, pl, pl_alive, see, 0.0f, &gd, at);
	if (gi == -1 && pl_alive && !b->retreat && hypotf(pl[0] - b->x, pl[2] - b->z) < see) {  /* (the player in the air, too) */
		gi = -2;
		gd = hypotf(pl[0] - b->x, pl[2] - b->z);
		memcpy(at, pl, sizeof at);
	}
	if (gi != -1 && !b->raider && M->rival_on && hypotf(at[0] - M->rkeep[0], at[2] - M->rkeep[2]) > BL_CHIEF_LEASH)
		gi = -1;                                                    /* guarding: not far from home */
	float tx = b->tx, ty = (float)bl_top(w, (int)floorf(b->tx), (int)floorf(b->tz)) + 1.0f, tz = b->tz, stop = 0.8f;
	bool keep_attack = false;
	const bool has_hammer = M->chief_arms & 1u << BL_WP_HAMMER, has_spear = M->chief_arms & 1u << BL_WP_SPEAR, has_bow = M->chief_arms & 1u << BL_WP_BOW,
	           has_fire = M->chief_arms & 1u << BL_WP_FIRE, has_laser = M->chief_arms & 1u << BL_WP_LASER;
	const int melee = has_hammer ? BL_WP_HAMMER : has_spear ? BL_WP_SPEAR : BL_WP_SWORD;
	if (gi != -1) {
		const float d = hypotf(gd, at[1] - b->y);
		const bool sees = chief_sees(w, b, at);
		int wp = melee;
		if (d > 3.0f && sees) {
			if (has_fire && d < bl_weapon(BL_WP_FIRE)->reach && (!has_laser || crowd(M, at, 3.0f, pl, pl_alive) > 1))
				wp = BL_WP_FIRE;
			else if (has_laser && d < bl_weapon(BL_WP_LASER)->reach)
				wp = BL_WP_LASER;
			else if (has_bow && d < bl_weapon(BL_WP_BOW)->reach)
				wp = BL_WP_BOW;
		}
		b->weapon = wp;
		const struct bl_weapon_info *wi = bl_weapon(wp);
		tx = at[0];
		ty = at[1];
		tz = at[2];
		if (wi->shot >= 0) {
			stop = 9.0f;                                            /* he keeps his distance, a little off the ground: those who get to him can strike him */
			ty = at[1] + 1.2f;
			if (b->attack_cd <= 0.0f) {
				b->attack_cd = wi->c_cooldown;
				b->swing = 1.0f;
				chief_shot(M, b, at, 1.0f, wp);
				chief_hit(M, b, gi, at, pl, pl_alive, R);
			}
		} else {
			stop = 1.0f;
			const float reach = wp == BL_WP_SPEAR ? 2.4f : 1.6f;
			if (gd < reach && fabsf(at[1] - b->y) < 2.0f && b->attack_cd <= 0.0f) {
				b->attack_cd = wi->c_cooldown;
				b->swing = 1.0f;
				chief_hit(M, b, gi, at, pl, pl_alive, R);
			}
		}
	} else if (b->raider && b->retreat) {
		const float hd = hypotf(M->rkeep[0] - b->x, M->rkeep[2] - b->z);
		if (hd < BL_RAID_HOME) {                                    /* home: he guards again */
			b->raider = b->retreat = false;
			R->raiders_home++;
		}
		tx = b->tx;
		tz = b->tz;
	} else if (b->raider) {
		const float *to = M->keep_on ? M->keep : M->raid_at;
		const float edge = M->keep_on ? bl_keep_dist(M->keep, b->x, b->z) : hypotf(to[0] - b->x, to[2] - b->z);
		tx = to[0];
		ty = to[1];
		tz = to[2];
		b->weapon = has_fire ? BL_WP_FIRE : has_laser ? BL_WP_LASER : melee;
		const struct bl_weapon_info *wi = bl_weapon(b->weapon);
		const float reach = wi->shot >= 0 ? 12.0f : 1.4f;
		stop = (M->keep_on ? BL_KEEP_EDGE : 0.0f) + reach - 0.3f;
		keep_attack = M->keep_on && edge < reach && (wi->shot >= 0 || fabsf(M->keep[1] - b->y) < 2.6f);
		if (keep_attack) {
			b->idle = b->lost = 0.0f;
			if (b->attack_cd <= 0.0f) {
				b->attack_cd = wi->c_cooldown;
				b->swing = 1.0f;
				chief_shot(M, b, M->keep, 2.5f, b->weapon);
				R->keep_damage += wi->c_keep;
				R->keep_hits++;
			}
		} else {
			raid_progress(b, edge);
			if (!M->keep_on && edge < 6.0f) {
				b->idle += STEP;
				if (b->idle > BL_RAID_IDLE * 0.5f)
					turn_back(b);
			}
			if (b->lost > BL_RAID_IDLE)
				turn_back(b);
		}
	} else {
		b->weapon = melee;
	}
	const float dx = tx - b->x, dz = tz - b->z, d = hypotf(dx, dz);
	if (d > 0.3f)
		*want = atan2f(dx, dz);
	/* up he goes when it is some way, when what he is after is above him, or when something is in the way */
	b->flying = b->in_water || b->blocked > 0.15f || d > stop + 5.0f || ty > b->y + 1.5f || (gi != -1 && bl_weapon(b->weapon)->shot >= 0) ||
	            (b->flying && !b->ground && d > stop + 1.0f);
	if (b->flying) {
		const float gy = (float)bl_top(w, (int)floorf(b->x), (int)floorf(b->z)) + 1.0f;
		float ay = gy;                                          /* the highest thing under him and just ahead: he clears it */
		for (float s = 0.5f; s <= 3.0f && s <= d; s += 0.5f)
			ay = fmaxf(ay, (float)bl_top(w, (int)floorf(b->x + sinf(*want) * s), (int)floorf(b->z + cosf(*want) * s)) + 1.0f);
		float h = ay + (d > stop + 4.0f ? 4.0f : 0.0f);
		h = d > stop + 4.0f ? fmaxf(h, ty + 2.0f) : ty;
		h = fmaxf(h, ay + (d > stop + 1.5f ? 1.2f : 0.0f));
		if (b->blocked > 0.15f)
			h = b->y + 4.0f;
		*up = fminf(h, (float)w->h + 4.0f);
	}
	if (d <= stop)
		return 0.0f;
	return clampf(3.0f * (d - stop), 0.0f, b->flying ? BL_CHIEF_FLY : BL_CHIEF_SPEED);
}

/* ---- the rival clan's labourers ------------------------------------------------------------------------------------------- */

/* is there somebody a labourer runs from within `r` of the place? */
static bool rworker_danger(const struct bl_mobs *M, float x, float y, float z, float r, const float pl[3], bool pl_alive)
{
	for (int k = pl_alive ? -1 : 0; k < BL_MOBS_MAX; k++) {
		const struct bl_mob *o = k < 0 ? NULL : &M->mob[k];
		if (o && (!o->alive || bl_mob_is_rival(o->kind) || o->posted))
			continue;
		float ox = o ? o->x : pl[0], oy = o ? o->y : pl[1], oz = o ? o->z : pl[2];
		if (hypotf(ox - x, oz - z) < r && fabsf(oy - y) < 5.0f)
			return true;
	}
	return false;
}

static void rskip_add(struct bl_mobs *M, int x, int z)
{
	M->rskip[M->nrskip % BL_RSKIP_MAX].x = x;
	M->rskip[M->nrskip % BL_RSKIP_MAX].z = z;
	M->nrskip++;
}

/* is the column somebody else's work, or one that could not be got to? */
static bool rjob_taken(const struct bl_mobs *M, const struct bl_mob *b, int x, int z)
{
	for (int i = 0; i < BL_RSKIP_MAX && i < M->nrskip; i++)
		if (M->rskip[i].x == x && M->rskip[i].z == z)
			return true;
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		const struct bl_mob *o = &M->mob[i];
		if (o != b && o->alive && o->kind == BL_MOB_RWORKER && o->order == BL_ORDER_GATHER && o->gblock != BL_AIR && o->gx == x && o->gz == z)
			return true;
	}
	return false;
}

/* Work for a labourer that has none: the nearest tree round their keep (a little chance in it, so they do not all go the same way), else the nearest stone that
 * sticks out of the ground (taken from the top, so no pits are left), else a walk somewhere to forage. False when there is nothing at all just now. */
static bool rworker_find(struct bl_mobs *M, struct bl_mob *b, const struct bl_world *w, const float pl[3], bool pl_alive)
{
	const int cx = (int)floorf(M->rkeep[0]), cy = (int)floorf(M->rkeep[1]), cz = (int)floorf(M->rkeep[2]);
	float best[2] = { 1e9f, 1e9f };
	int at[2][3] = { { 0 } };
	for (int dz = -BL_RWORKER_RANGE; dz <= BL_RWORKER_RANGE; dz++)
		for (int dx = -BL_RWORKER_RANGE; dx <= BL_RWORKER_RANGE; dx++) {
			int x = cx + dx, z = cz + dz;
			if ((abs(dx) <= BL_RWORKER_CAMP && abs(dz) <= BL_RWORKER_CAMP) || x < 2 || z < 2 || x >= w->w - 2 || z >= w->d - 2)
				continue;
			float d = hypotf((float)dx, (float)dz);
			if (d > (float)BL_RWORKER_RANGE)
				continue;
			int top = bl_top(w, x, z);
			if (top < cy - 12 || top > cy + 16 || top < 2)
				continue;
			int blk = bl_get(w, x, top, z), kind, y = top;
			if (blk == BL_LOG && bl_get(w, x, top + 1, z) == BL_LEAVES) {          /* a tree (a log somebody has put down has no leaves on it) */
				kind = 0;
				while (y > 1 && bl_get(w, x, y - 1, z) == BL_LOG)
					y--;
			} else if (blk == BL_STONE && top >= cy - 3 && bl_get(w, x, top + 1, z) == BL_AIR && bl_top(w, x + 1, z) <= top && bl_top(w, x - 1, z) <= top &&
			           bl_top(w, x, z + 1) <= top && bl_top(w, x, z - 1) <= top) {
				kind = 1;
			} else {
				continue;
			}
			float score = d + 10.0f * rf(&b->rng);
			if (score >= best[kind] || rjob_taken(M, b, x, z) || rworker_danger(M, (float)x + 0.5f, (float)y, (float)z + 0.5f, BL_RWORKER_FEAR + 3.0f, pl, pl_alive))
				continue;
			best[kind] = score;
			at[kind][0] = x;
			at[kind][1] = y;
			at[kind][2] = z;
		}
	float spot[3];
	for (int kind = 0; kind < 2; kind++) {
		if (best[kind] >= 1e9f)
			continue;
		if (!stand_spot(w, at[kind][0], at[kind][1], at[kind][2], b->x, b->z, spot)) {
			rskip_add(M, at[kind][0], at[kind][2]);
			continue;
		}
		b->gx = at[kind][0];
		b->gy = at[kind][1];
		b->gz = at[kind][2];
		b->gblock = kind == 0 ? BL_LOG : BL_STONE;
		b->tx = spot[0];
		b->tz = spot[2];
		return true;
	}
	for (int tries = 0; tries < 12; tries++) {                              /* nothing to fell or to cut: foraging */
		float a = rf(&b->rng) * 6.2831853f, d = (float)BL_RWORKER_CAMP + 4.0f + rf(&b->rng) * 14.0f, y;
		int x = (int)floorf((float)cx + sinf(a) * d), z = (int)floorf((float)cz + cosf(a) * d);
		if (!surface_at(w, x, z, &y) || fabsf(y - (float)cy) > 10.0f || bl_water(bl_get(w, x, (int)y, z)) ||
		    rworker_danger(M, (float)x + 0.5f, y, (float)z + 0.5f, BL_RWORKER_FEAR + 3.0f, pl, pl_alive))
			continue;
		b->gx = x;
		b->gy = (int)y;
		b->gz = z;
		b->gblock = BL_AIR;
		b->tx = (float)x + 0.5f;
		b->tz = (float)z + 0.5f;
		return true;
	}
	return false;
}

/* a piece of gold of one of the deposits, the one nearest their keep that nobody else is at and that is not dangerous */
static bool rworker_gold(struct bl_mobs *M, struct bl_mob *b, const struct bl_world *w, const float pl[3], bool pl_alive)
{
	float best = 1e9f;
	int at[3] = { 0 };
	for (int k = 0; k < M->ndeposits; k++) {
		const int x0 = M->deposit[k].x, z0 = M->deposit[k].z;
		if (hypotf((float)x0 - M->rkeep[0], (float)z0 - M->rkeep[2]) > BL_RWORKER_GOLD_RANGE)
			continue;
		for (int z = z0 - 3; z <= z0 + 3; z++)
			for (int x = x0 - 3; x <= x0 + 3; x++) {
				const int top = bl_top(w, x, z);
				if (top < 1 || bl_get(w, x, top, z) != BL_GOLD)
					continue;
				float score = hypotf((float)x - M->rkeep[0], (float)z - M->rkeep[2]) + 3.0f * rf(&b->rng);
				bool skip = score >= best || rjob_taken(M, b, x, z) || rworker_danger(M, (float)x + 0.5f, (float)top, (float)z + 0.5f, BL_RWORKER_FEAR + 3.0f, pl, pl_alive);
				for (int i = 0; !skip && i < BL_RSKIP_MAX && i < M->nrskip; i++)
					skip = M->rskip[i].x == x && M->rskip[i].z == z;
				if (skip)
					continue;
				best = score;
				at[0] = x;
				at[1] = top;
				at[2] = z;
			}
	}
	float spot[3];
	if (best >= 1e9f)
		return false;
	if (!stand_spot(w, at[0], at[1], at[2], b->x, b->z, spot)) {
		rskip_add(M, at[0], at[2]);
		return false;
	}
	b->gx = at[0];
	b->gy = at[1];
	b->gz = at[2];
	b->gblock = BL_GOLD;
	b->tx = spot[0];
	b->tz = spot[2];
	return true;
}

static void rworker_order(struct bl_mob *b, int order)
{
	b->order = b->errand = order;
	path_forget(b);
	b->at_site = false;
	b->work = b->wait = b->stuck = 0.0f;
	b->best_d = 1e9f;
}

/* A labourer: with an enemy near it runs home; with a load it carries it to the keep; else it builds, when the camp is building and not enough others are at it,
 * or fetches more. */
static float rworker_think(struct bl_mobs *M, struct bl_mob *b, const struct bl_world *w, const float pl[3], bool pl_alive, struct bl_mobs_result *R, float *want,
                           struct goal *g)
{
	const float dt = STEP;
	float kx = M->rkeep[0] - b->x, kz = M->rkeep[2] - b->z;
	const bool at_keep = bl_keep_dist(M->rkeep, b->x, b->z) < 2.0f && fabsf(M->rkeep[1] - b->y) < 3.5f;
	if (b->order == BL_ORDER_IDLE && b->errand != BL_ORDER_IDLE) {          /* it has given up (mob_step) */
		if (b->errand == BL_ORDER_GATHER)
			rskip_add(M, b->gx, b->gz);
		b->errand = BL_ORDER_IDLE;
		if (++b->fails >= BL_RWORKER_FAILS) {                               /* hopelessly stuck somewhere */
			b->alive = false;
			return 0.0f;
		}
	}
	if (!M->rival_on) {
		b->order = b->errand = BL_ORDER_IDLE;
		return 0.0f;
	}
	if (b->load + b->gold_load > 0 && at_keep) {
		R->delivered += b->load;
		R->delivered_gold += b->gold_load;
		b->load = b->gold_load = b->fails = 0;
		b->order = b->errand = BL_ORDER_IDLE;
		b->wait = 1.5f;
	}
	if (rworker_danger(M, b->x, b->y, b->z, BL_RWORKER_FEAR, pl, pl_alive))
		b->idle = BL_RWORKER_SCARE;
	if (b->idle > 0.0f) {
		b->idle -= dt;
		b->order = b->errand = BL_ORDER_IDLE;
		b->work = b->wait = 0.0f;
		if (at_keep)
			return 0.0f;
		*want = atan2f(kx, kz);
		*g = keep_goal(M->rkeep, b, 1.4f);
		g->any_height = true;
		return BL_RWORKER_SPEED;
	}
	if (b->order == BL_ORDER_IDLE) {
		b->wait = fmaxf(0.0f, b->wait - dt);
		if (b->wait > 0.0f)
			return 0.0f;
		int builders = 0;
		for (int i = 0; i < BL_MOBS_MAX; i++)
			builders += M->mob[i].alive && M->mob[i].kind == BL_MOB_RWORKER && M->mob[i].order == BL_ORDER_BUILD;
		float spot[2];
		int miners = 0;
		for (int i = 0; i < BL_MOBS_MAX; i++)
			miners += M->mob[i].alive && M->mob[i].kind == BL_MOB_RWORKER && M->mob[i].order == BL_ORDER_GATHER && M->mob[i].gblock == BL_GOLD;
		if (b->load + b->gold_load > 0) {
			rworker_order(b, BL_ORDER_MOVE);
		} else if (M->rsite.on && builders < BL_RWORKER_BUILDERS && bl_site_missing(w, &M->rsite) > 0 && site_spot(M, w, &M->rsite, b, spot)) {
			rworker_order(b, BL_ORDER_BUILD);
			b->tx = spot[0];
			b->tz = spot[1];
		} else if (M->rgold_wanted && miners < BL_RWORKER_MINERS && rworker_gold(M, b, w, pl, pl_alive)) {
			rworker_order(b, BL_ORDER_GATHER);
		} else if (rworker_find(M, b, w, pl, pl_alive)) {
			rworker_order(b, BL_ORDER_GATHER);
		} else {
			b->wait = 3.0f;
			return 0.0f;
		}
	}
	if (b->order == BL_ORDER_MOVE) {                                        /* home with the load */
		b->tx = M->rkeep[0];
		b->tz = M->rkeep[2];
		*want = atan2f(kx, kz);
		*g = keep_goal(M->rkeep, b, 1.4f);
		g->any_height = true;
		return clampf(3.0f * (bl_keep_dist(M->rkeep, b->x, b->z) - 1.0f), 0.0f, BL_RWORKER_SPEED);
	}
	if (b->order == BL_ORDER_BUILD) {
		if (!M->rsite.on || bl_site_missing(w, &M->rsite) == 0) {
			b->order = b->errand = BL_ORDER_IDLE;
			return 0.0f;
		}
		float v = site_work(M, b, w, &M->rsite, BL_RWORKER_BUILD_TIME, BL_RWORKER_SPEED, pl, pl_alive, R, want);
		if (!b->at_site)
			*g = (struct goal){ true, b->tx, b->y, b->tz, 0.5f, true };
		return v;
	}
	if (b->gblock != BL_AIR && bl_get(w, b->gx, b->gy, b->gz) != b->gblock) {        /* somebody has taken it */
		b->order = b->errand = BL_ORDER_IDLE;
		return 0.0f;
	}
	float dx = b->tx - b->x, dz = b->tz - b->z, d = hypotf(dx, dz);
	if (d > 0.6f) {
		*want = atan2f(dx, dz);
		*g = (struct goal){ true, b->tx, (float)b->gy, b->tz, 0.5f, b->gblock == BL_AIR };
		return clampf(3.0f * (d - 0.3f), 0.0f, BL_RWORKER_SPEED);
	}
	if (b->gblock != BL_AIR)
		*want = atan2f((float)b->gx + 0.5f - b->x, (float)b->gz + 0.5f - b->z);
	b->work += dt;
	if (fmodf(b->work, 0.55f) < dt)
		b->swing = 1.0f;
	const float need = b->gblock == BL_LOG ? BL_RWORKER_TREE_TIME : b->gblock == BL_STONE ? BL_RWORKER_STONE_TIME : b->gblock == BL_GOLD ? BL_RWORKER_GOLD_TIME : BL_RWORKER_FORAGE_TIME;
	const int max = (int)(sizeof R->raze / sizeof R->raze[0]);
	if (b->work < need || R->nraze + 40 > max)
		return 0.0f;
	if (b->gblock != BL_AIR) {
		int stack = 1;
		gather_time(w, b->gx, b->gy, b->gz, &stack);
		take_stack(w, b, stack, R->raze, &R->nraze, max);
	}
	if (b->gblock == BL_GOLD)
		b->gold_load = 1;
	else
		b->load = b->gblock == BL_LOG ? BL_RWORKER_TREE_LOAD : 1;
	rworker_order(b, BL_ORDER_MOVE);
	b->tx = M->rkeep[0];
	b->tz = M->rkeep[2];
	return 0.0f;
}

static void mob_step(struct bl_mobs *M, struct bl_mob *b, const struct bl_world *w, const float pl[3], bool pl_alive, float time_h, struct bl_mobs_result *R)
{
	const float dt = STEP;
	const bool goblin = b->kind == BL_MOB_GOBLIN;
	const float H = bl_mob_height(b);
	const float cspeed = b->brute ? BRUTE_SPEED : CHASE_SPEED, reach = b->brute ? BRUTE_REACH : ATTACK_REACH;
	const float cool = b->raider ? BL_GUARD_COOLDOWN : b->brute ? BRUTE_COOLDOWN : ATTACK_COOLDOWN;
	const float hit_p = b->raider ? BL_GUARD_HIT : b->brute ? BL_BRUTE_HIT : BL_GOBLIN_HIT, hit_k = b->brute ? BL_BRUTE_KEEP_HIT : BL_GOBLIN_KEEP_HIT, push = b->brute ? 8.0f : 6.0f;
	b->age += dt;
	b->hurt = fmaxf(0.0f, b->hurt - dt);
	b->attack_cd -= dt;
	b->swing = fmaxf(0.0f, b->swing - dt * 2.5f);
	b->jump_cd = fmaxf(0.0f, b->jump_cd - dt);

	if (b->posted) {
		if (b->order == BL_ORDER_TOWER && bl_get(w, b->gx, b->gy, b->gz) == BL_TOWERPOST && hypotf((float)b->gx + 0.5f - b->x, (float)b->gz + 0.5f - b->z) < 0.1f) {
			archer(M, w, b, b->kind == BL_MOB_GUARD && pl_alive ? pl : NULL, R);
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

	/* falling, and floating in water; the chief in the air goes to the height he wants */
	if (b->flying)
		b->vy = ap(b->vy, clampf((b->wyaw - b->y) * 3.0f, -8.0f, 8.0f), 40.0f, dt);        /* (a chief does not wander: `wyaw` is his height) */
	else if (b->in_water)
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
	const float dke = bl_keep_dist(M->keep, b->x, b->z);
	float want = b->yaw, vmax = 0.0f;
	bool to_keep = false, to_worker = false, chase = false;
	int wi = -1;
	float wdx = 0, wdz = 0, wd = 1e9f;
	struct goal goal = { 0 };
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
		if (!near_pl)                                                           /* a worker or a soldier in sight: the nearest */
			for (int k = 0; k < BL_MOBS_MAX; k++) {
				const struct bl_mob *o = &M->mob[k];
				if (!o->alive || !(bl_mob_is_unit(o->kind) || bl_mob_is_rival(o->kind)) || o->posted || fabsf(o->y - b->y) > 10.0f)
					continue;
				float ex = o->x - b->x, ez = o->z - b->z, e = hypotf(ex, ez);
				if (e < BL_UNIT_SEE && e < wd) {
					wd = e;
					wi = k;
					wdx = ex;
					wdz = ez;
				}
			}
		bool keep_there = M->keep_on && dk < BL_GOBLIN_KEEP_SEE && fabsf(M->keep[1] - b->y) < 14.0f;
		/* one that is close comes first; one farther off only when it is nearer than the player and the keep */
		to_worker = wi >= 0 && (wd < BL_UNIT_NEAR || ((!pl_alive || wd < d) && (!keep_there || wd < dk)));
		to_keep = keep_there && !near_pl && !to_worker;
		chase = !to_keep && !to_worker && pl_alive && d < AGGRO && fabsf(pl[1] - b->y) < 12.0f;
		if (to_worker) {
			want = atan2f(wdx, wdz);
			vmax = clampf(3.0f * (wd - 0.85f), 0.0f, cspeed);
			goal = (struct goal){ true, M->mob[wi].x, M->mob[wi].y, M->mob[wi].z, 0.9f, false };
		} else if (to_keep) {                                                   /* the player is not near: the keep is what it came for */
			want = atan2f(kx, kz);
			vmax = clampf(3.0f * (dke - 0.45f), 0.0f, cspeed);              /* it stops at the foot of it */
			goal = keep_goal(M->keep, b, 0.8f);
		} else if (chase) {
			want = atan2f(dx, dz);
			vmax = clampf(3.0f * (d - 0.85f), 0.0f, cspeed);               /* it slows down as it gets there and stops at arm's length */
			goal = (struct goal){ true, pl[0], pl[1], pl[2], 0.9f, false };
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
		vmax = soldier_think(M, b, w, R, &want, &goal);
		if (b->posted)
			return;
	} else if (b->kind == BL_MOB_CHIEF) {
		float up = b->y;
		vmax = chief_think(M, b, w, pl, pl_alive, R, &want, &up);
		b->wyaw = up;
	} else if (b->raider) {
		vmax = raider_think(M, b, pl, pl_alive, R, &want, &goal);
		if (!b->alive)
			return;
	} else if (b->kind == BL_MOB_GUARD) {
		vmax = guard_think(M, b, pl, pl_alive, R, &want, &goal);
	} else if (b->kind == BL_MOB_RWORKER) {
		vmax = rworker_think(M, b, w, pl, pl_alive, R, &want, &goal);
		if (!b->alive)
			return;
	} else {
		vmax = worker_think(M, b, w, pl, pl_alive, R, &want, &goal);
	}
	b->pathing = false;
	if (goal.on && vmax > 0.05f && hypotf(goal.x - b->x, goal.z - b->z) > goal.radius + 0.4f)
		b->pathing = steer(M, b, w, &goal, &want);
	b->sieging = false;
	const bool raiding = b->raider && !b->retreat && goal.on && hypotf(goal.x - b->x, goal.z - b->z) > goal.radius + 0.3f;
	if (!b->pathing && (raiding || (goblin && (to_worker || to_keep || chase) && (to_worker ? wd : to_keep ? dke : d) > (to_keep ? 1.1f : reach)))) {
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
	if (b->kind != BL_MOB_CHIEF && b->blocked > (b->pathing ? 1.2f : 0.1f)) {                              /* something in the way: turn to one side until it is clear */
		if (b->steer == 0.0f)
			b->steer = rf(&b->rng) < 0.5f ? 1.0f : -1.0f;
		b->yaw += b->steer * 3.5f * dt;
	} else {
		b->steer = 0.0f;
		float turn = (b->pathing ? 10.0f : 6.0f) * dt;                      /* (quicker round the corners of a way) */
		b->yaw += clampf(da, -turn, turn);
	}
	if (b->yaw > 3.14159265f)
		b->yaw -= 6.2831853f;
	if (b->yaw < -3.14159265f)
		b->yaw += 6.2831853f;
	b->ycos = cosf(b->yaw);
	b->ysin = sinf(b->yaw);
	if (fabsf(da) > 1.2f)
		vmax *= 0.3f;
	if (b->in_water && !b->flying)
		vmax *= 0.6f;
	b->speed = ap(b->speed, vmax, 8.0f, dt);

	/* a wanderer does not walk off an edge */
	if (goblin && !chase && !to_keep && !to_worker && b->ground && b->speed > 0.05f) {
		float ax = b->x + b->ysin * 0.55f, az = b->z + b->ycos * 0.55f;
		if (bl_box_support_unit(w, ax, b->y, az, 0.0f, 2.2f) < -1e8f) {
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
	if ((bx || bz) && !b->flying && (b->ground || b->in_water) && b->jump_cd <= 0.0f && b->speed > 0.1f) {    /* a block one high in the way is climbed (also out of water, onto the bank), two high is a wall */
		float nx = b->x + b->ysin * 0.4f, nz = b->z + b->ycos * 0.4f;
		if (mob_hits(w, b, nx, b->y, nz, BL_GOBLIN_HW, H) && !mob_hits(w, b, nx, b->y + 1.05f, nz, BL_GOBLIN_HW, H) && !mob_hits(w, b, b->x, b->y + 1.05f, b->z, BL_GOBLIN_HW, H)) {
			b->vy = b->in_water ? 6.5f : 8.4f;
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
	if (b->kind == BL_MOB_CHIEF) {
		;
	} else if (goblin) {
		if (b->blocked > 2.5f && !chase) {
			b->wander_t = 0.0f;
			b->blocked = 0.0f;
		}
	} else if (b->order != BL_ORDER_IDLE) {                                     /* a worker that does not get nearer to where it is going for four seconds gives up */
		float dd = hypotf(b->tx - b->x, b->tz - b->z);
		if (b->work > 0.0f || b->wait > 0.0f || dd < 0.6f || dd < b->best_d - 0.25f) {
			if (dd < b->best_d)
				b->best_d = dd;
			b->stuck = b->lost = 0.0f;
		} else {
			b->stuck += dt;
			b->lost += dt;
			if (b->stuck > 4.0f || b->lost > 90.0f) {               /* (going round in circles ends, too) */
				b->order = BL_ORDER_IDLE;
				b->stuck = b->lost = 0.0f;
			}
		}
	}
	if (!goblin)
		return;

	/* a blow at a worker */
	if (to_worker && b->attack_cd <= 0.0f && wd < reach) {
		b->attack_cd = cool;
		b->swing = 1.0f;
		strike(M, b, &M->mob[wi], hit_p, R);
	}
	/* a blow at the keep */
	if (to_keep && b->attack_cd <= 0.0f && dke < 0.95f && fabsf(M->keep[1] - b->y) < 2.6f) {
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
		M->path_budget = BL_PATH_BUDGET;
		M->acc += dt > 0.25f ? 0.25f : dt;
		for (int i = 0; i < BL_SITES_MAX; i++)                  /* the buildings that are done */
			if (M->site[i].on && bl_site_missing(w, &M->site[i]) == 0) {
				M->site[i].on = false;
				R.built[R.nbuilt++] = M->site[i].plan;
			}
		if (M->rsite.on && bl_site_missing(w, &M->rsite) == 0) {
			M->rsite.on = false;
			R.rbuilt = 1;
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
					for (int k = 0; k < BL_MOBS_MAX; k++)
						if (M->mob[k].alive && bl_mob_is_unit(M->mob[k].kind))
							dd = fminf(dd, hypotf(M->mob[k].x - b->x, M->mob[k].z - b->z));
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
						try_spawn_wave(M, w, pl, &R);
				} else if (M->enabled && pl_alive && bl_daylight(time_h) < 0.3f) {
					try_spawn(M, w, pl, cap, &R);
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
		float give = b->posted ? 0.0f : bl_mob_is_foe(b->kind) ? 0.5f : 1.0f, ox = b->x, oz = b->z;
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

static int pick_kind(const struct bl_mobs *m, int kind /* -1: the player's own (workers and soldiers), -2: his enemies */, const float o[3], const float dir[3], float reach, float *t)
{
	float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if (!(len > 1e-9f))
		return -1;
	float d[3] = { dir[0] / len, dir[1] / len, dir[2] / len };
	int best = -1;
	float bt = reach;
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		const struct bl_mob *b = &m->mob[i];
		if (!b->alive || (kind == -1 ? !bl_mob_is_unit(b->kind) : kind == -2 ? !bl_mob_is_foe(b->kind) : b->kind != kind))
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
	return pick_kind(m, -2, o, dir, reach, t);
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

/* a labourer of the rival clan: a worker's build in their colours - a dark red tunic, black cap and trousers, a sack on the back */
static const struct bbox k_rworker[] = {
	{ -0.20f, -0.02f, 0.00f, 0.55f, -0.09f, 0.09f, 0x24222au, LIMB_LEG_L, false },
	{ 0.02f, 0.20f, 0.00f, 0.55f, -0.09f, 0.09f, 0x24222au, LIMB_LEG_R, false },
	{ -0.24f, 0.24f, 0.55f, 1.02f, -0.13f, 0.13f, 0x8a2424u, LIMB_NONE, false },
	{ -0.38f, -0.25f, 0.60f, 1.00f, -0.07f, 0.07f, 0xd9a066u, LIMB_ARM_L, false },
	{ 0.25f, 0.38f, 0.60f, 1.00f, -0.07f, 0.07f, 0xd9a066u, LIMB_ARM_R, false },
	{ -0.18f, 0.18f, 0.62f, 0.98f, -0.30f, -0.14f, 0x8a744au, LIMB_NONE, false },
	{ -0.18f, 0.18f, 1.02f, 1.38f, -0.17f, 0.17f, 0xd9a066u, LIMB_NONE, false },
	{ -0.22f, 0.22f, 1.36f, 1.45f, -0.21f, 0.21f, 0x2a262cu, LIMB_NONE, false },
	{ -0.11f, -0.05f, 1.20f, 1.26f, 0.165f, 0.18f, 0x20180fu, LIMB_NONE, false },
	{ 0.05f, 0.11f, 1.20f, 1.26f, 0.165f, 0.18f, 0x20180fu, LIMB_NONE, false },
};

static const struct bbox k_guard[] = {
	{ -0.20f, -0.02f, 0.00f, 0.55f, -0.09f, 0.09f, 0x24222au, LIMB_LEG_L, false },
	{ 0.02f, 0.20f, 0.00f, 0.55f, -0.09f, 0.09f, 0x24222au, LIMB_LEG_R, false },
	{ -0.24f, 0.24f, 0.55f, 1.02f, -0.13f, 0.13f, 0x34303cu, LIMB_NONE, false },
	{ -0.24f, 0.24f, 0.72f, 0.82f, -0.135f, 0.135f, 0xc02222u, LIMB_NONE, false },
	{ -0.38f, -0.25f, 0.60f, 1.00f, -0.07f, 0.07f, 0x45414cu, LIMB_ARM_L, false },
	{ 0.25f, 0.38f, 0.60f, 1.00f, -0.07f, 0.07f, 0x45414cu, LIMB_ARM_R, false },
	{ 0.30f, 0.34f, 0.95f, 1.42f, 0.00f, 0.05f, 0xc8ccd4u, LIMB_ARM_R, false },
	{ -0.18f, 0.18f, 1.02f, 1.32f, -0.17f, 0.17f, 0xd9a066u, LIMB_NONE, false },
	{ -0.21f, 0.21f, 1.26f, 1.43f, -0.20f, 0.20f, 0xb81c1cu, LIMB_NONE, false },
	{ -0.28f, -0.24f, 0.80f, 1.43f, -0.24f, -0.20f, 0x3a2a1au, LIMB_NONE, false },      /* the pole of the banner ... */
	{ -0.54f, -0.28f, 1.05f, 1.43f, -0.23f, -0.21f, 0xe02828u, LIMB_NONE, false },      /* ... and the banner */
	{ -0.11f, -0.05f, 1.14f, 1.20f, 0.165f, 0.18f, 0x20180fu, LIMB_NONE, false },
	{ 0.05f, 0.11f, 1.14f, 1.20f, 0.165f, 0.18f, 0x20180fu, LIMB_NONE, false },
};

/* the rival chief: black armour with gold, a long red cape, a gold crown */
static const struct bbox k_chief[] = {
	{ -0.20f, -0.02f, 0.00f, 0.55f, -0.09f, 0.09f, 0x1c1a22u, LIMB_LEG_L, false },
	{ 0.02f, 0.20f, 0.00f, 0.55f, -0.09f, 0.09f, 0x1c1a22u, LIMB_LEG_R, false },
	{ -0.25f, 0.25f, 0.55f, 1.04f, -0.14f, 0.14f, 0x2a2630u, LIMB_NONE, false },
	{ -0.25f, 0.25f, 0.55f, 0.63f, -0.145f, 0.145f, 0xe0b030u, LIMB_NONE, false },
	{ -0.07f, 0.07f, 0.72f, 0.88f, 0.14f, 0.155f, 0xd02828u, LIMB_NONE, false },
	{ -0.30f, 0.30f, 0.15f, 1.04f, -0.23f, -0.17f, 0xa81414u, LIMB_NONE, false },       /* the cape */
	{ -0.40f, -0.25f, 0.62f, 1.06f, -0.09f, 0.09f, 0x3a3642u, LIMB_ARM_L, false },
	{ 0.25f, 0.40f, 0.62f, 1.06f, -0.09f, 0.09f, 0x3a3642u, LIMB_ARM_R, false },
	{ -0.18f, 0.18f, 1.04f, 1.36f, -0.17f, 0.17f, 0xd9a066u, LIMB_NONE, false },
	{ -0.20f, 0.20f, 1.34f, 1.40f, -0.19f, 0.19f, 0xf0c030u, LIMB_NONE, false },        /* the crown ... */
	{ -0.20f, -0.14f, 1.40f, 1.50f, 0.11f, 0.19f, 0xf0c030u, LIMB_NONE, false },
	{ -0.03f, 0.03f, 1.40f, 1.50f, 0.13f, 0.19f, 0xffe060u, LIMB_NONE, false },
	{ 0.14f, 0.20f, 1.40f, 1.50f, 0.11f, 0.19f, 0xf0c030u, LIMB_NONE, false },
	{ -0.11f, -0.05f, 1.18f, 1.24f, 0.165f, 0.18f, 0xff5018u, LIMB_NONE, true },
	{ 0.05f, 0.11f, 1.18f, 1.24f, 0.165f, 0.18f, 0xff5018u, LIMB_NONE, true },
};
#define CHIEF_SCALE 1.27f

/* what he holds, and the flames under his boots when he flies; how many boxes were added */
static int chief_extra(const struct bl_mob *b, struct bbox *o)
{
	int n = 0;
	switch (b->weapon) {
	case BL_WP_SPEAR:
		o[n++] = (struct bbox){ 0.30f, 0.34f, 0.30f, 1.55f, 0.02f, 0.06f, 0x6a4a2au, LIMB_ARM_R, false };
		o[n++] = (struct bbox){ 0.28f, 0.36f, 1.55f, 1.75f, 0.01f, 0.07f, 0xd8dee6u, LIMB_ARM_R, false };
		break;
	case BL_WP_HAMMER:
		o[n++] = (struct bbox){ 0.30f, 0.34f, 0.60f, 1.35f, 0.02f, 0.06f, 0x6a4a2au, LIMB_ARM_R, false };
		o[n++] = (struct bbox){ 0.22f, 0.44f, 1.35f, 1.58f, -0.10f, 0.18f, 0x7a808au, LIMB_ARM_R, false };
		break;
	case BL_WP_BOW:
		o[n++] = (struct bbox){ 0.30f, 0.34f, 0.55f, 1.35f, 0.18f, 0.22f, 0x8a5a2au, LIMB_ARM_R, false };
		o[n++] = (struct bbox){ 0.31f, 0.33f, 0.55f, 1.35f, 0.08f, 0.09f, 0xe8e8e0u, LIMB_ARM_R, false };
		break;
	case BL_WP_FIRE:
		o[n++] = (struct bbox){ 0.30f, 0.34f, 0.30f, 1.45f, 0.02f, 0.06f, 0x3a2418u, LIMB_ARM_R, false };
		o[n++] = (struct bbox){ 0.25f, 0.39f, 1.45f, 1.59f, -0.03f, 0.11f, 0xffa020u, LIMB_ARM_R, true };
		break;
	case BL_WP_LASER:
		o[n++] = (struct bbox){ 0.28f, 0.37f, 0.68f, 0.80f, 0.05f, 0.45f, 0x9aa2aeu, LIMB_ARM_R, false };
		o[n++] = (struct bbox){ 0.29f, 0.36f, 0.70f, 0.78f, 0.45f, 0.50f, 0xff2020u, LIMB_ARM_R, true };
		break;
	default:
		o[n++] = (struct bbox){ 0.30f, 0.35f, 0.95f, 1.58f, 0.00f, 0.05f, 0xf0f4f8u, LIMB_ARM_R, false };
		break;
	}
	if (b->flying) {
		o[n++] = (struct bbox){ -0.17f, -0.05f, -0.22f, -0.01f, -0.06f, 0.06f, 0xff8818u, LIMB_LEG_L, true };
		o[n++] = (struct bbox){ 0.05f, 0.17f, -0.22f, -0.01f, -0.06f, 0.06f, 0xffc030u, LIMB_LEG_R, true };
	}
	return n;
}

bool bl_mobs_ray(const struct bl_mobs *m, const float o[3], const float d[3], float tmax, struct bl_mob_hit *hit)
{
	bool found = false;
	float best = tmax;
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		const struct bl_mob *b = &m->mob[i];
		if (!b->alive)
			continue;
		const bool rworker = b->kind == BL_MOB_RWORKER, worker = b->kind == BL_MOB_WORKER, soldier = b->kind == BL_MOB_SOLDIER, guard = b->kind == BL_MOB_GUARD;
		const struct bbox *tab = rworker ? k_rworker : guard ? k_guard : soldier ? k_soldier : worker ? k_worker : b->brute ? k_brute : k_goblin;
		const int ntab = rworker ? (int)(sizeof k_rworker / sizeof k_rworker[0]) : guard ? (int)(sizeof k_guard / sizeof k_guard[0]) : soldier ? (int)(sizeof k_soldier / sizeof k_soldier[0]) : worker ? (int)(sizeof k_worker / sizeof k_worker[0]) : b->brute ? (int)(sizeof k_brute / sizeof k_brute[0]) : (int)(sizeof k_goblin / sizeof k_goblin[0]);
		const bool chief = b->kind == BL_MOB_CHIEF;
		const float scale = chief ? CHIEF_SCALE : soldier || guard ? SOLDIER_SCALE : worker || rworker ? WORKER_SCALE : b->brute ? BRUTE_SCALE : GOBLIN_SCALE;
		/* the box round the whole creature (arms, ears and a swinging leg reach 0.55 sideways and 0.6 forward) */
		const float rr = b->brute ? 0.92f : chief ? 0.80f : 0.66f;
		float lo[3] = { b->x - rr, b->y - (chief ? 0.35f : 0.0f), b->z - rr }, hi[3] = { b->x + rr, b->y + bl_mob_height(b) + (chief ? 0.4f : 0.06f), b->z + rr };
		float tb = ray_box(o, d, lo, hi, best);
		if (tb < 0.0f)
			continue;
		/* into the creature's own frame: x = right (cos, -sin), z = forward (sin, cos) */
		float rx = o[0] - b->x, ry = o[1] - b->y, rz = o[2] - b->z;
		const float is = 1.0f / scale;                                           /* (scaling the origin and the direction alike leaves the distance as it is) */
		float lo_[3] = { (rx * b->ycos - rz * b->ysin) * is, ry * is, (rx * b->ysin + rz * b->ycos) * is };
		float ld[3] = { (d[0] * b->ycos - d[2] * b->ysin) * is, d[1] * is, (d[0] * b->ysin + d[2] * b->ycos) * is };
		struct bbox extra[4];
		const int nextra = chief ? chief_extra(b, extra) : 0;
		const int nbase = chief ? (int)(sizeof k_chief / sizeof k_chief[0]) : ntab;
		for (int k = 0; k < nbase + nextra; k++) {
			const struct bbox *bx = k >= nbase ? &extra[k - nbase] : chief ? &k_chief[k] : &tab[k];
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
		if (ar->kind == BL_SHOT_LASER) {            /* a thin red beam: where the ray passes nearest to it */
			const float u[3] = { ar->b[0] - ar->a[0], ar->b[1] - ar->a[1], ar->b[2] - ar->a[2] };
			const float r0[3] = { o[0] - ar->a[0], o[1] - ar->a[1], o[2] - ar->a[2] };
			const float uu = u[0] * u[0] + u[1] * u[1] + u[2] * u[2], ud = u[0] * d[0] + u[1] * d[1] + u[2] * d[2];
			const float ur = u[0] * r0[0] + u[1] * r0[1] + u[2] * r0[2], dr = d[0] * r0[0] + d[1] * r0[1] + d[2] * r0[2];
			const float den = uu - ud * ud;
			if (den < 1e-6f || uu < 1e-6f)
				continue;
			const float s = clampf((ur - ud * dr) / den, 0.0f, 1.0f), t = s * ud - dr;
			if (t < 0.0f || t >= best)
				continue;
			float e = 0.0f;
			for (int a = 0; a < 3; a++) {
				const float q = r0[a] + d[a] * t - u[a] * s;
				e += q * q;
			}
			if (e > 0.045f * 0.045f)
				continue;
			best = t;
			found = true;
			hit->t = t;
			hit->id = -1;
			hit->rgb[0] = 1.0f;
			hit->rgb[1] = e < 0.018f * 0.018f ? 0.85f : 0.12f;
			hit->rgb[2] = e < 0.018f * 0.018f ? 0.80f : 0.10f;
			continue;
		}
		if (ar->kind == BL_SHOT_FIRE) {             /* a ball that flies, then bursts */
			const float age = BL_FIRE_TIME - ar->t, f = clampf(age / 0.3f, 0.0f, 1.0f);
			const float rad = age < 0.3f ? 0.22f : 0.22f + (age - 0.3f) / (BL_FIRE_TIME - 0.3f) * 1.0f;
			float c[3], oc[3];
			for (int a = 0; a < 3; a++) {
				c[a] = ar->a[a] + (ar->b[a] - ar->a[a]) * f;
				oc[a] = o[a] - c[a];
			}
			const float bq = oc[0] * d[0] + oc[1] * d[1] + oc[2] * d[2];
			const float cq = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - rad * rad;
			const float disc = bq * bq - cq;
			if (disc <= 0.0f)
				continue;
			const float t = -bq - sqrtf(disc);
			if (t < 0.0f || t >= best)
				continue;
			const float core = sqrtf(disc) / rad;
			if (age >= 0.3f && core < 0.55f && ((int)((o[0] + d[0] * t) * 7.0f) + (int)((o[1] + d[1] * t) * 7.0f) + (int)((o[2] + d[2] * t) * 7.0f)) % 3 == 0)
				continue;                               /* (the burst is ragged) */
			best = t;
			found = true;
			hit->t = t;
			hit->id = -1;
			hit->rgb[0] = 1.0f;
			hit->rgb[1] = 0.25f + 0.7f * core * core;
			hit->rgb[2] = 0.55f * core * core * core;
			continue;
		}
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
