/* blocksmobs.c - goblins in a block world. See blocksmobs.h. */
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
#define MODEL_SCALE 1.2f                /* the boxes of k_goblin are 1.42 m tall; the goblin is that times this */

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

int bl_mobs_count(const struct bl_mobs *m)
{
	int n = 0;
	for (int i = 0; i < BL_MOBS_MAX; i++)
		n += m->mob[i].alive;
	return n;
}

/* ---- the body --------------------------------------------------------------------------------------------------------- */

static bool place_mob(struct bl_mobs *M, const struct bl_world *w, int kind, float x, float y, float z)
{
	if (!bl_inside(w, (int)floorf(x), (int)floorf(y), (int)floorf(z)) || bl_box_hits(w, x, y, z, BL_GOBLIN_HW, BL_GOBLIN_H))
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
			b->hp = BL_GOBLIN_HP;
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

static void mob_kill(struct bl_mob *b)
{
	b->alive = false;
}

static void mob_step(struct bl_mobs *M, struct bl_mob *b, const struct bl_world *w, const float pl[3], bool pl_alive, float time_h, struct bl_mobs_result *R)
{
	const float dt = STEP;
	b->age += dt;
	b->hurt = fmaxf(0.0f, b->hurt - dt);
	b->attack_cd -= dt;
	b->swing = fmaxf(0.0f, b->swing - dt * 2.5f);
	b->jump_cd = fmaxf(0.0f, b->jump_cd - dt);

	if (bl_box_hits(w, b->x, b->y, b->z, BL_GOBLIN_HW, BL_GOBLIN_H)) {          /* inside a block (one was made round it): up through it, or it is gone */
		bool free = false;
		for (int i = 1; i <= 8 && !free; i++)
			if (!bl_box_hits(w, b->x, b->y + 0.25f * (float)i, b->z, BL_GOBLIN_HW, BL_GOBLIN_H)) {
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
	bool hit = bl_box_move(w, &b->x, &b->y, &b->z, 1, b->vy * dt, BL_GOBLIN_HW, BL_GOBLIN_H);
	if (hit) {
		b->ground = vy0 < 0;
		b->vy = 0;
	} else {
		b->ground = false;
	}
	if (!hit && b->vy <= 0 && bl_box_hits(w, b->x, b->y - 0.02f, b->z, BL_GOBLIN_HW, BL_GOBLIN_H))
		b->ground = true;

	/* the sun: a goblin in the open burns, and the ones far away in the day just are not there any more */
	float day = bl_daylight(time_h);
	if (day > 0.55f) {
		if (!b->in_water && bl_top(w, cx, cz) < (int)floorf(b->y)) {
			b->hp -= SUN_BURN * dt;
			b->hurt = fmaxf(b->hurt, 0.12f);
			if (b->hp <= 0.0f) {
				mob_kill(b);
				M->burned++;
				R->burned++;
				return;
			}
		}
	}

	/* what it wants: to go to the player, or to wander about */
	float dx = pl[0] - b->x, dz = pl[2] - b->z, d = hypotf(dx, dz);
	float kx = M->keep[0] - b->x, kz = M->keep[2] - b->z, dk = hypotf(kx, kz);
	bool near_pl = pl_alive && d < BL_GOBLIN_NEAR && fabsf(pl[1] - b->y) < 8.0f;
	bool to_keep = M->keep_on && !near_pl && dk < BL_GOBLIN_KEEP_SEE && fabsf(M->keep[1] - b->y) < 14.0f;
	bool chase = !to_keep && pl_alive && d < AGGRO && fabsf(pl[1] - b->y) < 12.0f;
	float want = b->yaw, vmax = 0.0f;
	if (to_keep) {                                                              /* the player is not near: the keep is what it came for */
		want = atan2f(kx, kz);
		vmax = clampf(3.0f * (dk - 1.0f), 0.0f, CHASE_SPEED);                    /* it stops at the foot of it */
	} else if (chase) {
		want = atan2f(dx, dz);
		vmax = clampf(3.0f * (d - 0.85f), 0.0f, CHASE_SPEED);       /* it slows down as it gets there and stops at arm's length */
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
	if (!chase && b->ground && b->speed > 0.05f) {
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
	bool bx = bl_box_move(w, &b->x, &b->y, &b->z, 0, sx, BL_GOBLIN_HW, BL_GOBLIN_H);
	bool bz = bl_box_move(w, &b->x, &b->y, &b->z, 2, sz, BL_GOBLIN_HW, BL_GOBLIN_H);
	if ((bx || bz) && b->ground && b->jump_cd <= 0.0f && b->speed > 0.1f) {  /* a block one high in the way is climbed, two high is a wall */
		float nx = b->x + b->ysin * 0.4f, nz = b->z + b->ycos * 0.4f;
		if (bl_box_hits(w, nx, b->y, nz, BL_GOBLIN_HW, BL_GOBLIN_H) && !bl_box_hits(w, nx, b->y + 1.05f, nz, BL_GOBLIN_HW, BL_GOBLIN_H) &&
		    !bl_box_hits(w, b->x, b->y + 1.05f, b->z, BL_GOBLIN_HW, BL_GOBLIN_H)) {
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
	if (b->blocked > 2.5f && !chase) {
		b->wander_t = 0.0f;
		b->blocked = 0.0f;
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
					if (!b->alive)
						continue;
					float dd = hypotf(pl[0] - b->x, pl[2] - b->z);
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
					bl_box_move(w, &p->x, &p->y, &p->z, 0, -nx * push, BL_GOBLIN_HW, BL_GOBLIN_H);
					bl_box_move(w, &p->x, &p->y, &p->z, 2, -nz * push, BL_GOBLIN_HW, BL_GOBLIN_H);
					bl_box_move(w, &q->x, &q->y, &q->z, 0, nx * push, BL_GOBLIN_HW, BL_GOBLIN_H);
					bl_box_move(w, &q->x, &q->y, &q->z, 2, nz * push, BL_GOBLIN_HW, BL_GOBLIN_H);
				}
			M->spawn_t -= STEP;
			if (M->spawn_t <= 0.0f) {
				M->spawn_t = 2.5f + 2.0f * rf(&M->rng);
				if (M->enabled && pl_alive && bl_daylight(time_h) < 0.3f && bl_mobs_count(M) < (M->cap < BL_MOBS_MAX ? M->cap : BL_MOBS_MAX))
					try_spawn(M, w, pl, &R);
			}
		}
	}
	if (out)
		*out = R;
}

/* ---- the sword -------------------------------------------------------------------------------------------------------- */

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

int bl_mobs_pick(const struct bl_mobs *m, const float o[3], const float dir[3], float reach, float *t)
{
	float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if (!(len > 1e-9f))
		return -1;
	float d[3] = { dir[0] / len, dir[1] / len, dir[2] / len };
	int best = -1;
	float bt = reach;
	for (int i = 0; i < BL_MOBS_MAX; i++) {
		const struct bl_mob *b = &m->mob[i];
		if (!b->alive)
			continue;
		float lo[3] = { b->x - BL_GOBLIN_HW - 0.05f, b->y, b->z - BL_GOBLIN_HW - 0.05f }, hi[3] = { b->x + BL_GOBLIN_HW + 0.05f, b->y + BL_GOBLIN_H + 0.05f, b->z + BL_GOBLIN_HW + 0.05f };
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
		if (b->alive && b->x + BL_GOBLIN_HW > fx && b->x - BL_GOBLIN_HW < fx + 1.0f && b->y + BL_GOBLIN_H > fy && b->y < fy + 1.0f && b->z + BL_GOBLIN_HW > fz &&
		    b->z - BL_GOBLIN_HW < fz + 1.0f)
			return true;
	}
	return false;
}

/* ---- the picture ------------------------------------------------------------------------------------------------------ */

enum { LIMB_NONE, LIMB_LEG_L, LIMB_LEG_R, LIMB_ARM_L, LIMB_ARM_R };

struct bbox {
	float x0, x1, y0, y1, z0, z1;                   /* in the goblin's own frame: x right, y up, z forward, the feet at 0 */
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
#define N_GOBLIN ((int)(sizeof k_goblin / sizeof k_goblin[0]))

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
		float k = sky > bl ? sky : bl;
		b->lit[0] = fmaxf(sky * (open && day < 0.3f ? 0.85f : 1.0f), bl * 1.0f);
		b->lit[1] = fmaxf(sky * (open && day < 0.3f ? 0.9f : 1.0f), bl * 0.80f);
		b->lit[2] = fmaxf(sky * (open && day < 0.3f ? 1.15f : 1.0f), bl * 0.52f);
		(void)k;
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
		/* the box round the whole goblin (arms, ears and a swinging leg reach 0.55 sideways and 0.6 forward) */
		float lo[3] = { b->x - 0.66f, b->y, b->z - 0.66f }, hi[3] = { b->x + 0.66f, b->y + 1.76f, b->z + 0.66f };
		float tb = ray_box(o, d, lo, hi, best);
		if (tb < 0.0f)
			continue;
		/* into the goblin's own frame: x = right (cos, -sin), z = forward (sin, cos) */
		float rx = o[0] - b->x, ry = o[1] - b->y, rz = o[2] - b->z;
		const float is = 1.0f / MODEL_SCALE;                                      /* (scaling the origin and the direction alike leaves the distance as it is) */
		float lo_[3] = { (rx * b->ycos - rz * b->ysin) * is, ry * is, (rx * b->ysin + rz * b->ycos) * is };
		float ld[3] = { (d[0] * b->ycos - d[2] * b->ysin) * is, d[1] * is, (d[0] * b->ysin + d[2] * b->ycos) * is };
		for (int k = 0; k < N_GOBLIN; k++) {
			const struct bbox *bx = &k_goblin[k];
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
			/* the shade of the face it was hit on (in the goblin's frame: top 1, bottom 0.55, the sides 0.85 and 0.7) */
			float shade = axis == 1 ? (ld[1] < 0.0f ? 1.0f : 0.55f) : axis == 0 ? 0.70f : 0.85f;
			uint32_t c = bx->rgb;
			float col[3] = { (float)((c >> 16) & 255) / 255.0f, (float)((c >> 8) & 255) / 255.0f, (float)(c & 255) / 255.0f };
			for (int ch = 0; ch < 3; ch++) {
				float v = bx->glow ? col[ch] : col[ch] * shade * b->lit[ch];
				if (b->hurt > 0.0f && !bx->glow) {                  /* the red flash of a blow (and of the sun) */
					float f = clampf(b->hurt / 0.35f, 0.0f, 1.0f) * 0.6f;
					v = v * (1.0f - f) + (ch == 0 ? 1.0f : 0.1f) * f;
				}
				hit->rgb[ch] = clampf(v, 0.0f, 1.0f);
			}
		}
	}
	(void)tmax;
	return found;
}
