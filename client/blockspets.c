/* blockspets.c - Tom and Filip in a block world, as bodies. See blockspets.h. */
#include "blockspets.h"
#include "blocksview.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* 1 / (1 + k d^2): at 12 blocks 0.76 (the room has 0.018: 0.28) */
#define BL_PETS_ATT 0.0022f

#define YARD_X0 (-MCAT_AREA_X - 0.3f)
#define YARD_X1 (MCAT_AREA_X + 0.3f)
#define YARD_Z0 (MCAT_AREA_Z0 - 0.3f)
#define YARD_Z1 (MCAT_AREA_Z1 + 0.3f)

#define TOM_HW 0.26f                    /* half the width of his box, and its height */
#define TOM_H 0.70f
#define FIL_HW 0.28f
#define FIL_H 0.30f
#define GRAV 30.0f
#define STEP 0.016666667f
#define REST_Y 0.065f
#define LEASH 9.0f                      /* how far from his yard Tom wanders, and from his water Filip */
#define DESC 8.0f
#define ASC 6.0f

enum { T_SIT, T_RISE, T_MOVE, T_SETTLE, T_SITDOWN, T_LIE, T_HOP };
enum { H_CROUCH, H_AIR, H_LAND };
enum { F_SWIM, F_SINK, F_REST, F_RISE, F_STRANDED };

/* ---- small things ----------------------------------------------------------------------------------------------------- */

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

static float sat(float v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static float angdiff(float a, float b)                  /* a - b, wrapped to -pi .. pi */
{
	return fmodf(a - b + 9.42477796f, 6.28318531f) - 3.14159265f;
}

static float sstep(float a, float b, float v)
{
	float t = sat((v - a) / (b - a));
	return t * t * (3.0f - 2.0f * t);
}

/* ---- the world as a body meets it -------------------------------------------------------------------------------------- */

#define box_hits bl_box_hits
#define support_y bl_box_support
#define body_axis bl_box_move

static bool wet_at(const struct bl_world *w, float x, float y, float z, float hw)
{
	static const float k[5][2] = { { 0, 0 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
	for (int i = 0; i < 5; i++)
		if (bl_water(bl_get(w, (int)floorf(x + k[i][0] * hw), (int)floorf(y + 0.1f), (int)floorf(z + k[i][1] * hw))))
			return true;
	return false;
}

/* ---- the yard and the water ------------------------------------------------------------------------------------------- */

static float ground_at(const struct bl_world *w, float x, float z)
{
	int t = bl_top(w, (int)floorf(x), (int)floorf(z));
	return t < 0 ? 1.0f : (float)(t + 1);
}

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

/* How bad a patch of ground is for a yard centred at (cx, cz): twice the difference of its heights plus a point and a half for every column that is water or has a tree
 * on it (the median height in *ground); 1e9 when it does not lie inside the world. */
static float patch_score(const struct bl_world *w, float cx, float cz, float *ground)
{
	int x0 = (int)floorf(cx + YARD_X0), x1 = (int)ceilf(cx + YARD_X1), z0 = (int)floorf(cz + YARD_Z0), z1 = (int)ceilf(cz + YARD_Z1);
	if (x0 < 1 || z0 < 1 || x1 >= w->w - 1 || z1 >= w->d - 1)
		return 1e9f;
	int hs[256], n = 0, bad = 0, lo = 1 << 20, hi = -1;
	for (int z = z0; z <= z1; z++)
		for (int x = x0; x <= x1; x++) {
			int top = w->top[(size_t)z * (size_t)w->w + (size_t)x], top_y = w->hi[(size_t)z * (size_t)w->w + (size_t)x];
			if (top < 0 || top_y < 0) {
				bad += 2;
				continue;
			}
			int b = w->b[bl_index(w, x, top_y, z)];
			if (bl_water(b))
				bad += 2;
			else if (b == BL_LEAVES || b == BL_LOG || top_y > top)
				bad += 1;
			if (top < lo)
				lo = top;
			if (top > hi)
				hi = top;
			if (n < 256)
				hs[n++] = top;
		}
	if (n == 0)
		return 1e9f;
	qsort(hs, (size_t)n, sizeof hs[0], cmp_int);
	*ground = (float)(hs[n / 2] + 1);
	return 2.0f * (float)(hi - lo) + 1.5f * (float)bad;
}

/* water at least 3 deep under the middle and a 5 x 5 of it round, as far as `maxr` blocks from `near` (and not nearer than 4): the nearest such place, its surface in *sy */
static bool find_water(const struct bl_world *w, const float near[3], int maxr, float *cx, float *cz, float *sy)
{
	float best = 1e9f;
	int nx = (int)floorf(near[0]), nz = (int)floorf(near[2]);
	for (int z = nz - maxr; z <= nz + maxr; z++)
		for (int x = nx - maxr; x <= nx + maxr; x++) {
			if (x < 3 || z < 3 || x >= w->w - 3 || z >= w->d - 3)
				continue;
			float d = hypotf((float)(x - nx), (float)(z - nz));
			if (d < 4.0f || d >= best)
				continue;
			int hy = w->hi[(size_t)z * (size_t)w->w + (size_t)x];
			if (hy < 3 || !bl_water(w->b[bl_index(w, x, hy, z)]))
				continue;
			int depth = 0;
			for (int y = hy; y >= 0 && bl_water(w->b[bl_index(w, x, y, z)]); y--)
				depth++;
			if (depth < 3)
				continue;
			bool ok = true;
			for (int dz = -2; dz <= 2 && ok; dz++)
				for (int dx = -2; dx <= 2 && ok; dx++)
					ok = bl_water(bl_get(w, x + dx, hy, z + dz)) && bl_water(bl_get(w, x + dx, hy - 1, z + dz));
			if (!ok)
				continue;
			best = d;
			*cx = (float)x + 0.5f;
			*cz = (float)z + 0.5f;
			*sy = (float)(hy + 1);
		}
	return best < 1e8f;
}

/* a pond of 7 x 7 (a disc of radius 3, three deep, level with the ground) next to the yard: the blocks above go, the blocks of the pit are water */
static void make_pond(struct bl_world *w, float cx, float cz, float *sy)
{
	int ix = (int)floorf(cx), iz = (int)floorf(cz);
	int base = bl_top(w, ix, iz);
	if (base < 3)
		base = 3;
	for (int dz = -3; dz <= 3; dz++)
		for (int dx = -3; dx <= 3; dx++) {
			if (dx * dx + dz * dz > 10)
				continue;
			int x = ix + dx, z = iz + dz;
			if (!bl_inside(w, x, 0, z))
				continue;
			for (int y = base + 8; y > base; y--)
				if (w->b[bl_index(w, x, y, z)] != BL_AIR)
					bl_set(w, x, y, z, BL_AIR);
			for (int y = base - 2; y <= base; y++)
				bl_set(w, x, y, z, BL_WATER);
		}
	*sy = (float)(base + 1);
}

/* ---- Tom ---------------------------------------------------------------------------------------------------------------- */

/* is the place at (x, z), at the height he stands at, one he can be at: ground under it (not a drop of more than a block), no water */
static bool walkable(const struct bl_world *w, float x, float y, float z)
{
	if (support_y(w, x, y, z, 0.0f, 1.25f) < -1e8f)                     /* the middle of him must be over ground: he may hang a little over an edge, not walk off it */
		return false;
	return !wet_at(w, x, y, z, TOM_HW);
}

static bool pick_target(struct bl_pets *p, const struct bl_world *w, bool far)
{
	struct bl_tom *t = &p->tom;
	for (int i = 0; i < 14; i++) {
		float a = rf(&t->rng) * 6.2831853f, d = far ? 5.0f + rf(&t->rng) * (LEASH - 4.0f) : 1.5f + rf(&t->rng) * 5.0f;
		float x = p->yard[0] + sinf(a) * d, z = p->yard[2] + cosf(a) * d;
		if (hypotf(x - t->x, z - t->z) < 1.2f)
			continue;
		float s = support_y(w, x, t->y + 1.2f, z, TOM_HW, 3.5f);
		if (s < -1e8f || fabsf(s - t->y) > 1.6f || wet_at(w, x, s, z, TOM_HW) || box_hits(w, x, s + 0.01f, z, TOM_HW, TOM_H))
			continue;
		t->tx = x;
		t->tz = z;
		return true;
	}
	return false;
}

static void tom_sit(struct bl_tom *t)
{
	t->mode = T_SIT;
	t->timer = 6.0f + 14.0f * rf(&t->rng);
}

static void tom_decide(struct bl_pets *p, const struct bl_world *w)
{
	struct bl_tom *t = &p->tom;
	float r = rf(&t->rng);
	if (r < 0.18f) {
		t->mode = T_LIE;
		t->lie_total = 30.0f + 30.0f * rf(&t->rng);
		t->timer = t->lie_total;
		return;
	}
	if (r < 0.36f) {
		t->mode = T_RISE;
		t->next = T_HOP;
		t->hops_left = 2;
		t->timer = 1.4f;
		return;
	}
	bool run = rf(&t->rng) < 0.3f;
	if (pick_target(p, w, run)) {
		t->running = run;
		t->mode = T_RISE;
		t->next = T_MOVE;
		t->timer = 1.4f;
		t->blocked = 0.0f;
	} else {
		tom_sit(t);
	}
}

static void tom_unstick(struct bl_pets *p, const struct bl_world *w)
{
	struct bl_tom *t = &p->tom;
	if (!box_hits(w, t->x, t->y, t->z, TOM_HW, TOM_H))
		return;
	for (int i = 1; i <= 8; i++)                                         /* up through what he is in */
		if (!box_hits(w, t->x, t->y + 0.25f * (float)i, t->z, TOM_HW, TOM_H)) {
			t->y += 0.25f * (float)i;
			return;
		}
	t->x = p->yard[0];                                                   /* or back to the yard */
	t->z = p->yard[2];
	t->y = ground_at(w, t->x, t->z);
	t->vy = 0;
}

/* a step on the level: false when nothing moved */
static bool tom_walk(struct bl_pets *p, const struct bl_world *w, float sx, float sz)
{
	struct bl_tom *t = &p->tom;
	float nx = t->x + sx, nz = t->z + sz;
	if (t->ground && !walkable(w, nx, t->y, nz)) {                       /* a drop, water: not that way */
		t->blocked += STEP;
		return false;
	}
	float ox = t->x, oz = t->z;
	bool bx = body_axis(w, &t->x, &t->y, &t->z, 0, sx, TOM_HW, TOM_H), bz = body_axis(w, &t->x, &t->y, &t->z, 2, sz, TOM_HW, TOM_H);
	if ((bx || bz) && t->ground && t->jump_cd <= 0.0f) {                  /* a block in the way: one high is climbed, two is a wall */
		float jx = sx * (bx ? 3.0f : 1.0f), jz = sz * (bz ? 3.0f : 1.0f);
		if (box_hits(w, t->x + jx, t->y, t->z + jz, TOM_HW, TOM_H) && !box_hits(w, t->x + jx, t->y + 1.05f, t->z + jz, TOM_HW, TOM_H) &&
		    !box_hits(w, t->x, t->y + 1.05f, t->z, TOM_HW, TOM_H)) {
			t->vy = 8.4f;
			t->ground = false;
			t->jump_cd = 0.6f;
			t->crouch = 0.5f;
		}
	}
	float moved = hypotf(t->x - ox, t->z - oz);
	t->dist += moved;
	if (moved < 0.35f * hypotf(sx, sz) && !(t->vy > 0)) {
		t->blocked += STEP;
		return false;
	}
	t->blocked = fmaxf(0.0f, t->blocked - STEP);
	return true;
}

static void tom_step(struct bl_pets *p, const struct bl_world *w)
{
	struct bl_tom *t = &p->tom;
	const float dt = STEP;
	tom_unstick(p, w);
	/* falling and standing */
	t->vy = fmaxf(t->vy - GRAV * dt, -40.0f);
	float vy0 = t->vy;
	bool hit = body_axis(w, &t->x, &t->y, &t->z, 1, t->vy * dt, TOM_HW, TOM_H);
	if (hit) {
		if (vy0 < 0) {
			if (!t->ground && vy0 < -5.0f)
				t->crouch = fmaxf(t->crouch, 0.7f);               /* landing from a height */
			t->ground = true;
		}
		t->vy = 0;
	} else {
		t->ground = false;
	}
	if (!hit && t->vy <= 0 && box_hits(w, t->x, t->y - 0.02f, t->z, TOM_HW, TOM_H))
		t->ground = true;
	t->jump_cd = fmaxf(0.0f, t->jump_cd - dt);
	if (t->ground)
		t->air_t = 0.0f;
	else
		t->air_t += dt;

	float stand_to = 0.0f, lie_to = 0.0f, walk_to = 0.0f, run_to = 0.0f;
	switch (t->mode) {
	case T_SIT:
		t->speed = ap(t->speed, 0.0f, 6.0f, dt);
		t->timer -= dt;
		if (t->timer <= 0.0f)
			tom_decide(p, w);
		break;
	case T_RISE:
		stand_to = 1.0f;
		t->timer -= dt;
		if (t->timer <= 0.0f) {
			t->mode = t->next;
			t->hop_phase = H_CROUCH;
			t->timer = 0.3f;
		}
		break;
	case T_MOVE: {
		stand_to = 1.0f;
		float dx = t->tx - t->x, dz = t->tz - t->z, d = hypotf(dx, dz);
		if (d < 0.3f || t->blocked > 1.5f) {
			t->mode = T_SETTLE;
			t->timer = 0.5f;
			break;
		}
		float da = angdiff(atan2f(dx, dz), t->yaw);
		if (t->blocked > 0.1f) {                                          /* something in the way: turn to one side until it is clear */
			if (t->steer == 0.0f)
				t->steer = rf(&t->rng) < 0.5f ? 1.0f : -1.0f;
			t->yaw += t->steer * 3.5f * dt;
		} else {
			t->steer = 0.0f;
			t->yaw += clampf(da, -5.0f * dt, 5.0f * dt);
		}
		float vmax = t->running ? 2.3f : 0.9f;
		if (fabsf(da) > 1.0f)
			vmax *= 0.35f;
		t->speed = ap(t->speed, vmax, t->speed < vmax ? 4.0f : 8.0f, dt);
		walk_to = 1.0f;
		run_to = t->running ? 1.0f : 0.0f;
		if (t->speed > 0.02f)
			tom_walk(p, w, sinf(t->yaw) * t->speed * dt, cosf(t->yaw) * t->speed * dt);
		break;
	}
	case T_SETTLE:
		stand_to = 1.0f;
		t->speed = ap(t->speed, 0.0f, 5.0f, dt);
		if (t->speed > 0.02f)
			tom_walk(p, w, sinf(t->yaw) * t->speed * dt, cosf(t->yaw) * t->speed * dt);
		t->timer -= dt;
		if (t->timer <= 0.0f) {
			t->mode = T_SITDOWN;
			t->timer = 1.4f;
		}
		break;
	case T_SITDOWN:
		t->speed = ap(t->speed, 0.0f, 6.0f, dt);
		t->timer -= dt;
		if (t->timer <= 0.0f)
			tom_sit(t);
		break;
	case T_LIE: {
		t->speed = ap(t->speed, 0.0f, 6.0f, dt);
		t->timer -= dt;
		float elapsed = t->lie_total - t->timer;
		lie_to = sat(fminf(elapsed, t->timer) / 6.0f) > 0.0f ? 1.0f : 0.0f;
		if (t->timer <= 0.0f && t->lie < 0.02f)
			tom_sit(t);
		break;
	}
	case T_HOP:
		stand_to = 1.0f;
		t->speed = ap(t->speed, 0.0f, 6.0f, dt);
		if (t->hop_phase == H_CROUCH) {
			t->crouch = ap(t->crouch, 1.0f, 1.0f / 0.3f, dt);
			t->timer -= dt;
			if (t->timer <= 0.0f && t->ground) {
				t->vy = 4.9f;
				t->ground = false;
				t->hop_phase = H_AIR;
			}
		} else if (t->hop_phase == H_AIR) {
			if (t->ground && t->air_t == 0.0f && t->vy == 0.0f && t->crouch >= 0.0f) {
				t->hop_phase = H_LAND;
				t->timer = 0.55f;
				t->crouch = 0.8f;
				t->hops_left--;
			}
		} else {
			t->timer -= dt;
			if (t->timer <= 0.0f) {
				if (t->hops_left > 0) {
					t->hop_phase = H_CROUCH;
					t->timer = 0.3f;
				} else {
					t->mode = T_SETTLE;
					t->timer = 0.3f;
				}
			}
		}
		break;
	}
	/* the pose follows what he does */
	t->stand = ap(t->stand, stand_to, 1.0f / 1.4f, dt);
	t->lie = ap(t->lie, lie_to, 1.0f / 6.0f, dt);
	t->walk = ap(t->walk, walk_to, 1.0f / 0.3f, dt);
	t->run = ap(t->run, run_to, 1.0f / 0.4f, dt);
	if (t->mode != T_HOP || t->hop_phase != H_CROUCH)
		t->crouch = ap(t->crouch, 0.0f, 1.0f / 0.35f, dt);
	float air = t->y - (support_y(w, t->x, t->y, t->z, TOM_HW, 4.0f) > -1e8f ? support_y(w, t->x, t->y, t->z, TOM_HW, 4.0f) : t->y);
	t->tuck = ap(t->tuck, sat(air / 0.22f), 8.0f, dt);
}

/* ---- Filip -------------------------------------------------------------------------------------------------------------- */

static bool wcell(const struct bl_world *w, float x, float y, float z)
{
	return bl_water(bl_get(w, (int)floorf(x), (int)floorf(y), (int)floorf(z)));
}

/* the shell and a little round it are all in water */
static bool swim_ok(const struct bl_world *w, float x, float y, float z)
{
	return wcell(w, x, y, z) && wcell(w, x, y + 0.14f, z) && wcell(w, x, y - 0.14f, z) && wcell(w, x + FIL_HW, y, z) && wcell(w, x - FIL_HW, y, z) &&
	       wcell(w, x, y, z + FIL_HW) && wcell(w, x, y, z - FIL_HW);
}

/* the surface of the water over (x, y, z) and the floor under it (the first thing that is not water, going up and going down) */
static void water_column(const struct bl_world *w, float x, float y, float z, float *surface, float *bottom)
{
	int ix = (int)floorf(x), iz = (int)floorf(z), iy = (int)floorf(y), top = iy, bot = iy;
	while (top < w->h && bl_water(bl_get(w, ix, top, iz)))
		top++;
	while (bot >= 0 && bl_water(bl_get(w, ix, bot, iz)))
		bot--;
	*surface = (float)top;
	*bottom = (float)(bot + 1);
}

static bool pick_swim_target(struct bl_pets *p, const struct bl_world *w)
{
	struct bl_filip *f = &p->filip;
	for (int i = 0; i < 14; i++) {
		float a = rf(&f->rng) * 6.2831853f, d = 1.5f + rf(&f->rng) * (LEASH - 2.0f);
		float x = p->pond[0] + sinf(a) * d, z = p->pond[2] + cosf(a) * d, sf, bt;
		if (hypotf(x - f->x, z - f->z) < 1.5f || !wcell(w, x, f->y, z))
			continue;
		water_column(w, x, f->y, z, &sf, &bt);
		if (sf - bt < 1.0f)
			continue;
		f->tx = x;
		f->tz = z;
		return true;
	}
	return false;
}

static void filip_bubbles(struct bl_filip *f, const struct bl_world *w, float dt)
{
	for (int i = 0; i < f->nbub; i++) {
		struct mturtle_bubble *b = &f->bub[i];
		b->age += dt;
		if (b->age < 0.0f)
			continue;
		b->c[1] += 0.32f * dt;
		float sf, bt;
		water_column(w, b->c[0], b->c[1] - 0.05f, b->c[2], &sf, &bt);
		if (b->c[1] >= sf - 0.03f || !wcell(w, b->c[0], b->c[1], b->c[2]) || b->age > 12.0f) {
			f->bub[i] = f->bub[--f->nbub];
			i--;
		}
	}
	f->bub_timer -= dt;
	if (f->bub_timer <= 0.0f) {
		f->bub_timer = 6.0f + 4.0f * rf(&f->rng);
		if (f->rest < 0.3f && f->mode == F_SWIM) {
			int nb = 2 + (int)(rf(&f->rng) * 3.0f);
			for (int i = 0; i < nb && f->nbub < MTURTLE_BUBBLES; i++) {
				struct mturtle_bubble *b = &f->bub[f->nbub++];
				b->c[0] = f->x + sinf(f->yaw) * 0.52f + 0.05f * (rf(&f->rng) - 0.5f);
				b->c[1] = f->y - 0.01f;
				b->c[2] = f->z + cosf(f->yaw) * 0.52f + 0.05f * (rf(&f->rng) - 0.5f);
				b->r = 0.013f + 0.008f * floorf(rf(&f->rng) * 4.0f);
				b->age = -0.35f * (float)i;
			}
		}
	}
}

static void filip_step(struct bl_pets *p, const struct bl_world *w)
{
	struct bl_filip *f = &p->filip;
	const float dt = STEP;
	f->time += dt;
	bool clear_water = swim_ok(w, f->x, f->y, f->z);
	f->swimming = wcell(w, f->x, f->y + 0.05f, f->z);
	float sf, bt;
	water_column(w, f->x, f->y, f->z, &sf, &bt);
	f->bottom = bt;
	if (!clear_water && f->mode != F_STRANDED && f->mode != F_REST && f->mode != F_SINK) {
		f->mode = F_STRANDED;                                            /* the water went */
		f->speed = 0;
	}
	switch (f->mode) {
	case F_STRANDED: {
		f->vy = fmaxf(f->vy - GRAV * dt, -20.0f);
		float x = f->x, y = f->y, z = f->z;
		bool hit = body_axis(w, &x, &y, &z, 1, f->vy * dt, FIL_HW, FIL_H * 0.5f);
		f->y = y;
		if (hit)
			f->vy = 0;
		f->rest = ap(f->rest, 1.0f, 1.0f, dt);
		f->speed = ap(f->speed, 0.0f, 2.0f, dt);
		f->pitch = ap(f->pitch, 0.0f, 1.0f, dt);
		f->rest_tau = 0.0f;
		f->rest_d = 30.0f;
		if (swim_ok(w, f->x, f->y, f->z)) {                                  /* water again: he swims */
			f->mode = F_SWIM;
			f->rest = 0.0f;
		}
		return;
	}
	case F_SINK: {
		f->rest = ap(f->rest, 1.0f, 1.0f / DESC, dt);
		float e = sstep(0.0f, 1.0f, f->rest), ty = f->bottom + REST_Y;
		f->y = f->ystart + (ty - f->ystart) * e;
		f->speed = ap(f->speed, 0.0f, 0.3f, dt);
		f->yaw_rate = 0.0f;
		float da = angdiff(f->rest_yaw, f->yaw);
		f->yaw += clampf(da, -0.5f * dt, 0.5f * dt);
		f->pitch = ap(f->pitch, 0.0f, 0.5f, dt);
		if (f->rest >= 1.0f) {
			f->mode = F_REST;
			f->rest_tau = 0.0f;
		}
		return;
	}
	case F_REST:
		f->rest_tau += dt;
		f->rest = 1.0f;
		f->speed = 0.0f;
		f->yaw_rate = 0.0f;
		f->y = f->bottom + REST_Y;
		if (f->rest_tau >= f->rest_d) {
			f->mode = F_RISE;
			f->ystart = f->y;
			f->rest_tau = f->rest_d;
		}
		return;
	case F_RISE: {
		f->rest_tau += dt;
		f->rest = ap(f->rest, 0.0f, 1.0f / ASC, dt);
		float e = sstep(0.0f, 1.0f, 1.0f - f->rest), ty = clampf(sf - f->depth, f->bottom + 0.25f, sf - 0.2f);
		f->y = f->ystart + (ty - f->ystart) * e;
		if (f->rest <= 0.0f) {
			f->mode = F_SWIM;
			f->rest_timer = 45.0f + 45.0f * rf(&f->rng);
		}
		return;
	}
	default:
		break;
	}

	/* swimming: heading, depth, bubbles */
	f->rest_timer -= dt;
	f->depth_timer -= dt;
	if (f->depth_timer <= 0.0f) {
		f->depth = 0.5f + 1.9f * rf(&f->rng);
		f->depth_timer = 6.0f + 6.0f * rf(&f->rng);
	}
	if (f->rest_timer <= 0.0f && sf - bt >= 0.8f) {                      /* down to the bottom to rest */
		f->mode = F_SINK;
		f->ystart = f->y;
		f->rest_d = 22.0f + 12.0f * rf(&f->rng);
		f->rest_hk = (uint32_t)(rf(&f->rng) * 4294967295.0f);
		f->rest_yaw = rf(&f->rng) * 6.2831853f;
		return;
	}
	float dx = f->tx - f->x, dz = f->tz - f->z;
	if (hypotf(dx, dz) < 0.8f || hypotf(f->x - p->pond[0], f->z - p->pond[2]) > LEASH + 2.0f) {
		if (hypotf(f->x - p->pond[0], f->z - p->pond[2]) > LEASH + 2.0f) {
			f->tx = p->pond[0];
			f->tz = p->pond[2];
		} else if (!pick_swim_target(p, w)) {
			f->tx = p->pond[0];
			f->tz = p->pond[2];
		}
		dx = f->tx - f->x;
		dz = f->tz - f->z;
	}
	float da = angdiff(atan2f(dx, dz), f->yaw), rate = clampf(da * 1.6f, -0.7f, 0.7f);
	float speed_to = 0.42f + 0.12f * sinf(f->time * 0.2f);
	/* the shore ahead: turn to the side that has water */
	float ax = f->x + sinf(f->yaw) * 0.9f, az = f->z + cosf(f->yaw) * 0.9f;
	if (!swim_ok(w, ax, f->y, az)) {
		static const float off[6] = { 0.6f, -0.6f, 1.2f, -1.2f, 2.0f, -2.0f };
		bool found = false;
		for (int i = 0; i < 6 && !found; i++)
			if (swim_ok(w, f->x + sinf(f->yaw + off[i]) * 0.9f, f->y, f->z + cosf(f->yaw + off[i]) * 0.9f)) {
				rate = off[i] > 0 ? 1.1f : -1.1f;
				found = true;
			}
		if (!found)
			rate = 1.4f;
		speed_to *= 0.5f;
		f->tx = p->pond[0];                                                /* and then away from the shore, to the middle */
		f->tz = p->pond[2];
	}
	f->yaw_rate = ap(f->yaw_rate, rate, 2.5f, dt);
	f->yaw += f->yaw_rate * dt;
	f->speed = ap(f->speed, speed_to, 0.3f, dt);
	float nx = f->x + sinf(f->yaw) * f->speed * dt, nz = f->z + cosf(f->yaw) * f->speed * dt;
	if (swim_ok(w, nx, f->y, nz)) {
		f->x = nx;
		f->z = nz;
	} else {
		f->speed = ap(f->speed, 0.0f, 1.0f, dt);
	}
	float ty = clampf(sf - f->depth, bt + 0.25f, sf - 0.2f);
	f->vy = ap(f->vy, clampf((ty - f->y) * 0.9f, -0.35f, 0.35f), 0.8f, dt);
	if (swim_ok(w, f->x, f->y + f->vy * dt, f->z))
		f->y += f->vy * dt;
	else
		f->vy = 0.0f;
	f->pitch = ap(f->pitch, clampf(atan2f(f->vy, fmaxf(f->speed, 0.1f)) * 0.9f, -0.45f, 0.45f), 1.5f, dt);
	f->rest = 0.0f;
	filip_bubbles(f, w, dt);
}

/* ---- setting up -------------------------------------------------------------------------------------------------------------- */

void bl_pets_init(struct bl_pets *p, uint32_t seed, const uint8_t *const *glyph, int nglyph)
{
	memset(p, 0, sizeof *p);
	mcat_set_seed(seed);
	mturtle_set_seed(seed ? (seed ^ 0x5bd1e995u) : 0u);
	if (nglyph < 1)
		nglyph = 1;
	int n = nglyph < MCAT_GLYPHS ? nglyph : MCAT_GLYPHS;
	for (int i = 0; i < MCAT_GLYPHS; i++)
		p->glyph[i] = glyph[i % n];
	p->nglyph = MCAT_GLYPHS;
	p->tom.rng = seed * 2654435761u + 12345u;
	p->filip.rng = seed * 40503u + 777u;
	p->on = true;
}

static void place_filip(struct bl_pets *p, const struct bl_world *w)
{
	struct bl_filip *f = &p->filip;
	f->x = p->pond[0];
	f->z = p->pond[2];
	float sf, bt;
	water_column(w, f->x, p->pond[1] - 0.5f, f->z, &sf, &bt);
	f->y = clampf(sf - 1.0f, bt + 0.25f, sf - 0.2f);
	f->bottom = bt;
	f->yaw = 1.0f;
	f->mode = F_SWIM;
	f->depth = 1.0f;
	f->depth_timer = 5.0f;
	f->rest_timer = 40.0f + 40.0f * rf(&f->rng);
	f->bub_timer = 3.0f;
	f->tx = f->x;
	f->tz = f->z;
	f->swimming = true;
}

void bl_pets_set_yard(struct bl_pets *p, struct bl_world *w, float x, float z, const float near[3])
{
	p->yard[0] = x;
	p->yard[2] = z;
	p->yard[1] = ground_at(w, x, z);
	struct bl_tom *t = &p->tom;
	t->x = x;
	t->z = z;
	t->y = p->yard[1];
	t->vy = 0;
	t->yaw = 0.0f;
	t->ground = true;
	t->mode = T_SIT;
	t->timer = 8.0f + 6.0f * rf(&t->rng);
	/* Filip's water */
	float ref[3] = { near ? near[0] : x, 0.0f, near ? near[2] : z }, cx = 0, cz = 0, sy = 0;
	p->made_pond = false;
	if (find_water(w, ref, 56, &cx, &cz, &sy)) {
		p->pond[0] = cx;
		p->pond[1] = sy;
		p->pond[2] = cz;
	} else {
		float px = x - 6.5f, pz = z;
		if (px < 6.0f)
			px = x + 6.5f;
		make_pond(w, px, pz, &sy);
		p->pond[0] = floorf(px) + 0.5f;
		p->pond[1] = sy;
		p->pond[2] = floorf(pz) + 0.5f;
		p->made_pond = true;
	}
	p->have_pond = true;
	place_filip(p, w);
	p->placed = true;
	p->acc = 0.0f;
}

bool bl_pets_place(struct bl_pets *p, struct bl_world *w, const float near[3])
{
	float best = 1e9f, bx = 0, bz = 0;
	for (int r = 10; r <= 48; r += 3)
		for (int k = 0; k < 16; k++) {
			float a = (float)k * 0.39269908f, cx = near[0] + (float)r * cosf(a), cz = near[2] + (float)r * sinf(a), g;
			float sc = patch_score(w, cx, cz, &g) + 0.04f * (float)r;
			if (sc < best) {
				best = sc;
				bx = cx;
				bz = cz;
			}
		}
	if (best >= 1e8f) {
		p->placed = false;
		return false;
	}
	bl_pets_set_yard(p, w, bx, bz, near);
	return true;
}

void bl_pets_restore(struct bl_pets *p, const struct bl_world *w, const float tom[3], const float filip[3])
{
	if (!p->placed)
		return;
	if (tom && !box_hits(w, tom[0], tom[1], tom[2], TOM_HW, TOM_H) && support_y(w, tom[0], tom[1], tom[2], TOM_HW, 6.0f) > -1e8f) {
		p->tom.x = tom[0];
		p->tom.y = tom[1];
		p->tom.z = tom[2];
	}
	if (filip && swim_ok(w, filip[0], filip[1], filip[2])) {
		p->filip.x = filip[0];
		p->filip.y = filip[1];
		p->filip.z = filip[2];
	}
}

void bl_pets_tom_go(struct bl_pets *p, float x, float z, bool run)
{
	struct bl_tom *t = &p->tom;
	t->tx = x;
	t->tz = z;
	t->running = run;
	t->mode = T_RISE;
	t->next = T_MOVE;
	t->timer = 0.2f;
	t->blocked = 0.0f;
	t->steer = 0.0f;
}

void bl_pets_step(struct bl_pets *p, const struct bl_world *w, float dt)
{
	if (!p->placed || !(dt > 0.0f))
		return;
	p->acc += dt > 0.25f ? 0.25f : dt;
	while (p->acc >= STEP) {
		p->acc -= STEP;
		tom_step(p, w);
		filip_step(p, w);
	}
}

/* ---- bodies ------------------------------------------------------------------------------------------------------------------- */

static bool player_free(const struct bl_world *w, float x, float y, float z)
{
	return !box_hits(w, x, y, z, BL_PLAYER_W * 0.5f, BL_PLAYER_H);
}

void bl_pets_push_player(struct bl_pets *p, const struct bl_world *w, struct bl_player *pl)
{
	if (!p->placed || !p->on)
		return;
	const float pw = BL_PLAYER_W * 0.5f, ph = BL_PLAYER_H;
	for (int id = 1; id <= 2; id++) {
		float cx = id == 1 ? p->tom.x : p->filip.x, cz = id == 1 ? p->tom.z : p->filip.z, hw = id == 1 ? TOM_HW : FIL_HW;
		float y0 = id == 1 ? p->tom.y : p->filip.y - 0.12f, h = id == 1 ? TOM_H : FIL_H;
		if (!(fabsf(pl->x - cx) < pw + hw && fabsf(pl->z - cz) < pw + hw && pl->y < y0 + h && pl->y + ph > y0))
			continue;
		float top = y0 + h;
		if (pl->y >= top - 0.30f && pl->vy <= 0.5f) {                     /* from above: he stands on it */
			if (player_free(w, pl->x, top, pl->z)) {
				pl->y = top;
				pl->vy = 0.0f;
				pl->on_ground = true;
			}
			continue;
		}
		float ox = (pw + hw) - fabsf(pl->x - cx), oz = (pw + hw) - fabsf(pl->z - cz);
		int axis = ox < oz ? 0 : 2;
		float ov = (axis == 0 ? ox : oz) + 0.01f, sign = (axis == 0 ? pl->x - cx : pl->z - cz) >= 0.0f ? 1.0f : -1.0f;
		float moved = 0.0f;
		if (id == 1) {                                                    /* Tom is pushed away, as far as the world lets him */
			float x = p->tom.x, y = p->tom.y, z = p->tom.z;
			body_axis(w, &x, &y, &z, axis, -sign * ov, TOM_HW, TOM_H);
			moved = fabsf(axis == 0 ? x - p->tom.x : z - p->tom.z);
			p->tom.x = x;
			p->tom.z = z;
		} else {                                                          /* Filip too, but only inside the water */
			float nx = p->filip.x - (axis == 0 ? sign * ov : 0.0f), nz = p->filip.z - (axis == 2 ? sign * ov : 0.0f);
			if (swim_ok(w, nx, p->filip.y, nz)) {
				moved = ov;
				p->filip.x = nx;
				p->filip.z = nz;
			}
		}
		float rest = ov - moved;
		if (rest > 0.005f) {                                              /* the pet could not give way: the player steps back */
			float nx = pl->x + (axis == 0 ? sign * rest : 0.0f), nz = pl->z + (axis == 2 ? sign * rest : 0.0f);
			if (player_free(w, nx, pl->y, nz)) {
				pl->x = nx;
				pl->z = nz;
			}
		}
	}
}

bool bl_pets_hit_cell(const struct bl_pets *p, int x, int y, int z)
{
	if (!p->placed || !p->on)
		return false;
	float fx = (float)x, fy = (float)y, fz = (float)z;
	if (p->tom.x + TOM_HW > fx && p->tom.x - TOM_HW < fx + 1.0f && p->tom.y + TOM_H > fy && p->tom.y < fy + 1.0f && p->tom.z + TOM_HW > fz && p->tom.z - TOM_HW < fz + 1.0f)
		return true;
	float y0 = p->filip.y - 0.12f;
	return p->filip.x + FIL_HW > fx && p->filip.x - FIL_HW < fx + 1.0f && y0 + FIL_H > fy && y0 < fy + 1.0f && p->filip.z + FIL_HW > fz && p->filip.z - FIL_HW < fz + 1.0f;
}

/* ---- the pose and the picture ----------------------------------------------------------------------------------------------- */

static void light_for(float time_h, float ld[3], float lc[3], float *ls)
{
	float sun[3];
	bl_sun_dir(time_h, sun);
	if (sun[1] > 0.05f) {
		float t = (sun[1] - 0.05f) / 0.25f, w = sun[1] / 0.5f;
		t = t < 0 ? 0 : t > 1 ? 1 : t;
		w = w < 0 ? 0 : w > 1 ? 1 : w;
		memcpy(ld, sun, 3 * sizeof(float));
		lc[0] = 255.0f;
		lc[1] = 140.0f + (232.0f - 140.0f) * w;
		lc[2] = 60.0f + (150.0f - 60.0f) * w;
		*ls = t * t * (3.0f - 2.0f * t);
	} else if (sun[1] < -0.05f) {
		for (int i = 0; i < 3; i++)
			ld[i] = -sun[i];
		lc[0] = 150.0f;
		lc[1] = 180.0f;
		lc[2] = 255.0f;
		*ls = 0.3f;
	} else {
		ld[0] = 0;
		ld[1] = 1;
		ld[2] = 0;
		lc[0] = lc[1] = lc[2] = 0;
		*ls = 0.0f;
	}
}

void bl_pets_pose(struct bl_pets *p, const struct bl_world *w, float t, float dt, const float cam[3], float time_h)
{
	(void)dt;
	if (!p->on || !p->placed) {
		p->cat.enabled = false;
		p->tur.enabled = false;
		return;
	}
	light_for(time_h, p->ldir, p->lcol, &p->lstr);
	/* Tom: the floor under him is the ground (the model's y = 0) and what he is above it is his hop */
	struct bl_tom *t0 = &p->tom;
	float fl = support_y(w, t0->x, t0->y, t0->z, TOM_HW, 4.0f);
	if (fl < -1e8f)
		fl = t0->y;
	p->tom_floor = fl;
	p->org_cat[0] = 0.0f;
	p->org_cat[1] = fl;
	p->org_cat[2] = 0.0f;
	struct mcat_state st = { .x = t0->x, .z = t0->z, .yaw = t0->yaw, .stand = t0->stand, .lie = t0->lie, .walk = t0->walk, .dist = t0->dist, .run = t0->run,
	                         .air = fmaxf(0.0f, t0->y - fl), .crouch = t0->crouch, .tuck = t0->tuck, .stretch = 0.0f };
	float rel[3] = { cam[0] - p->org_cat[0], cam[1] - p->org_cat[1], cam[2] - p->org_cat[2] };
	mcat_pose_state(&p->cat, t, &st, rel, p->glyph, p->nglyph, p->ldir, p->lcol, p->lstr);
	/* Filip: the floor is the bottom of the water */
	struct bl_filip *f = &p->filip;
	p->tur_floor = f->bottom;
	p->org_tur[0] = 0.0f;
	p->org_tur[1] = f->bottom;
	p->org_tur[2] = 0.0f;
	struct mturtle_state ts = { .pos = { f->x, f->y - f->bottom, f->z }, .yaw = f->yaw, .pitch = f->pitch, .speed = f->speed, .yaw_rate = f->yaw_rate, .rest = f->rest };
	if (f->mode == F_REST || f->mode == F_RISE)
		mturtle_rest_params(f->rest_tau, f->rest_d, f->rest_hk, &ts.head_in, &ts.fore_in, &ts.hind_in);
	else if (f->mode == F_SINK || f->mode == F_STRANDED)
		mturtle_rest_params(0.0f, f->rest_d > 1.0f ? f->rest_d : 30.0f, f->rest_hk, &ts.head_in, &ts.fore_in, &ts.hind_in);
	mturtle_pose_state(&p->tur, t, &ts, p->glyph, p->nglyph, p->ldir, p->lcol, p->lstr);
	p->tur.nbub = 0;
	for (int i = 0; i < f->nbub; i++)
		if (f->bub[i].age >= 0.0f && p->tur.nbub < MTURTLE_BUBBLES) {
			struct mturtle_bubble *b = &p->tur.bub[p->tur.nbub++];
			*b = f->bub[i];
			b->c[1] -= f->bottom;
		}
	/* in the open they are seen from far further than across a room: the room's glow fades with the distance much more slowly */
	p->cat.att_k = BL_PETS_ATT;
	p->tur.att_k = BL_PETS_ATT;
}

struct cand {
	int id;
	float tp;
};

static int candidates(const struct bl_pets *p, const float o[3], const float dn[3], float tmax, struct cand c[2])
{
	int n = 0;
	if (p->cat.enabled) {
		float wx = p->org_cat[0] + p->cat.centre[0] - o[0], wy = p->org_cat[1] + p->cat.centre[1] - o[1], wz = p->org_cat[2] + p->cat.centre[2] - o[2];
		float tp = wx * dn[0] + wy * dn[1] + wz * dn[2], near2 = wx * wx + wy * wy + wz * wz - tp * tp, r = p->cat.radius;
		if (tp > -r && tp - r < tmax && near2 <= r * r) {
			c[n].id = 1;
			c[n++].tp = tp;
		}
	}
	if (p->tur.enabled) {
		float wx = p->org_tur[0] + p->tur.centre[0] - o[0], wy = p->org_tur[1] + p->tur.centre[1] - o[1], wz = p->org_tur[2] + p->tur.centre[2] - o[2];
		float tp = wx * dn[0] + wy * dn[1] + wz * dn[2], near2 = wx * wx + wy * wy + wz * wz - tp * tp, r = p->tur.radius + 0.35f;      /* (the bubbles reach a little beyond the shell) */
		if (tp > -r && tp - r < tmax && near2 <= r * r) {
			c[n].id = 2;
			c[n++].tp = tp;
		}
	}
	if (n == 2 && c[0].tp > c[1].tp) {
		struct cand x = c[0];
		c[0] = c[1];
		c[1] = x;
	}
	return n;
}

static bool shade_one(const struct bl_pets *p, int id, const float o[3], const float dn[3], float tmax, float inv_f, float rgb[3])
{
	if (id == 1) {
		float orel[3] = { o[0] - p->org_cat[0], o[1] - p->org_cat[1], o[2] - p->org_cat[2] };
		return mcat_shade(&p->cat, orel, dn, tmax, inv_f, rgb);
	}
	float orel[3] = { o[0] - p->org_tur[0], o[1] - p->org_tur[1], o[2] - p->org_tur[2] };
	return mturtle_shade(&p->tur, orel, dn, tmax, inv_f, rgb);
}

int bl_pets_events(const struct bl_pets *p, const float o[3], const float dn[3], float tmax, float inv_f, struct bl_pet_event ev[2])
{
	if (!p->on || !p->placed)
		return 0;
	struct cand c[2];
	int nc = candidates(p, o, dn, tmax, c), n = 0;
	for (int i = 0; i < nc; i++) {
		float b0[3] = { 0, 0, 0 }, b1[3] = { 255, 255, 255 };
		if (!shade_one(p, c[i].id, o, dn, tmax, inv_f, b0))
			continue;
		shade_one(p, c[i].id, o, dn, tmax, inv_f, b1);                  /* the same over white: what the creature covers shows in the difference */
		float a = 1.0f - ((b1[0] - b0[0]) + (b1[1] - b0[1]) + (b1[2] - b0[2])) * (1.0f / (3.0f * 255.0f));
		ev[n].a = sat(a);
		for (int k = 0; k < 3; k++)
			ev[n].c[k] = sat(b0[k] * (1.0f / 255.0f));
		ev[n].tp = c[i].tp;
		n++;
	}
	return n;
}

int bl_pets_probe(const struct bl_pets *p, const float o[3], const float dn[3], float tmax, float inv_f)
{
	if (!p->on || !p->placed)
		return 0;
	struct cand c[2];
	int nc = candidates(p, o, dn, tmax, c), got = 0;
	for (int i = 0; i < nc; i++) {
		float rgb[3] = { 0, 0, 0 };
		if (shade_one(p, c[i].id, o, dn, tmax, inv_f, rgb) && !got)
			got = c[i].id;
	}
	return got;
}

float bl_pets_floor_shade(const struct bl_pets *p, float x, float y, float z)
{
	if (!p->on || !p->placed)
		return 1.0f;
	float s = 1.0f;
	if (p->cat.enabled && fabsf(y - p->org_cat[1]) < 0.4f && fabsf(x - p->tom.x) < 2.5f && fabsf(z - p->tom.z) < 2.5f) {
		float f = mcat_floor_shade(&p->cat, x - p->org_cat[0], z - p->org_cat[2]);
		if (f < s)
			s = f;
	}
	if (p->tur.enabled && fabsf(y - p->org_tur[1]) < 0.4f && fabsf(x - p->filip.x) < 2.0f && fabsf(z - p->filip.z) < 2.0f) {
		float f = mturtle_floor_shade(&p->tur, x - p->org_tur[0], z - p->org_tur[2]);
		if (f < s)
			s = f;
	}
	return s;
}

void bl_pets_info(const struct bl_pets *p, struct bl_pets_info *out)
{
	memset(out, 0, sizeof *out);
	memcpy(out->yard, p->yard, sizeof out->yard);
	memcpy(out->pond, p->pond, sizeof out->pond);
	out->tom[0] = p->tom.x;
	out->tom[1] = p->tom.y;
	out->tom[2] = p->tom.z;
	out->tom_yaw = p->tom.yaw;
	out->tom_stand = p->tom.stand;
	out->tom_lie = p->tom.lie;
	out->tom_walk = p->tom.walk;
	out->tom_air = p->cat.air;
	out->tom_mode = p->tom.mode;
	out->filip[0] = p->filip.x;
	out->filip[1] = p->filip.y;
	out->filip[2] = p->filip.z;
	out->filip_rest = p->filip.rest;
	out->filip_in_water = p->filip.swimming;
}
