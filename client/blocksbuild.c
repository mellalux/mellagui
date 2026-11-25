/* blocksbuild.c - the plans, the sites and the ghosts. See blocksbuild.h. */
#include "blocksbuild.h"

#include <math.h>
#include <string.h>

static const struct {
	const char *name;
	int cobble, planks;
} k_plans[BL_PLAN_COUNT] = {
	{ "barracks", 20, 16 }, { "watchtower", 20, 10 }, { "wall", 6, 0 }, { "gate", 6, 4 },
};

const char *bl_plan_name(int plan)
{
	return plan >= 0 && plan < BL_PLAN_COUNT ? k_plans[plan].name : "?";
}

void bl_plan_cost(int plan, int *cobble, int *planks)
{
	bool ok = plan >= 0 && plan < BL_PLAN_COUNT;
	*cobble = ok ? k_plans[plan].cobble : 0;
	*planks = ok ? k_plans[plan].planks : 0;
}

/* a cell of the plan before it is turned and placed: u along the width, v into the depth, y up */
struct pcell {
	int u, y, v, block;
};

/* the cells of a plan, lowest first; how many */
static int plan_cells(int plan, struct pcell *o)
{
	int n = 0;
	if (plan == BL_PLAN_BARRACKS) {             /* a house of cobble with plank corners and roof, a door in the front, the banner over the door */
		for (int y = 0; y < 3; y++)
			for (int v = -1; v <= 2; v++)
				for (int u = -2; u <= 2; u++) {
					if (u != -2 && u != 2 && v != -1 && v != 2)
						continue;
					if (u == 0 && v == -1 && y < 2)
						continue;                       /* the door */
					bool corner = (u == -2 || u == 2) && (v == -1 || v == 2);
					int b = u == 0 && v == -1 ? BL_BARRACKS : corner ? BL_PLANKS : BL_COBBLE;
					o[n++] = (struct pcell){ u, y, v, b };
				}
		for (int v = -1; v <= 2; v++)
			for (int u = -2; u <= 2; u++)
				o[n++] = (struct pcell){ u, 3, v, BL_PLANKS };
	} else if (plan == BL_PLAN_TOWER) {         /* four pillars, a platform with the watchpost in its middle, four merlons */
		for (int y = 0; y < 4; y++)
			for (int v = -1; v <= 1; v += 2)
				for (int u = -1; u <= 1; u += 2)
					o[n++] = (struct pcell){ u, y, v, BL_COBBLE };
		for (int v = -1; v <= 1; v++)
			for (int u = -1; u <= 1; u++)
				o[n++] = (struct pcell){ u, 4, v, u == 0 && v == 0 ? BL_TOWERPOST : BL_PLANKS };
		for (int v = -1; v <= 1; v += 2)
			for (int u = -1; u <= 1; u += 2)
				o[n++] = (struct pcell){ u, 5, v, BL_COBBLE };
	} else if (plan == BL_PLAN_WALL) {
		for (int y = 0; y < 2; y++)
			for (int u = -1; u <= 1; u++)
				o[n++] = (struct pcell){ u, y, 0, BL_COBBLE };
	} else if (plan == BL_PLAN_GATE) {
		for (int y = 0; y < 3; y++)
			for (int u = -1; u <= 1; u++) {
				if (u == 0 && y == 2)
					continue;
				o[n++] = (struct pcell){ u, y, 0, u == 0 ? BL_GATE : BL_COBBLE };
			}
	}
	return n;
}

void bl_site_box(const struct bl_site *s, int lo[3], int hi[3])
{
	for (int k = 0; k < 3; k++) {
		lo[k] = 1 << 20;
		hi[k] = -(1 << 20);
	}
	for (int i = 0; i < s->n; i++) {
		const int p[3] = { s->c[i].x, s->c[i].y, s->c[i].z };
		for (int k = 0; k < 3; k++) {
			lo[k] = p[k] < lo[k] ? p[k] : lo[k];
			hi[k] = p[k] > hi[k] ? p[k] : hi[k];
		}
	}
}

int bl_site_missing(const struct bl_world *w, const struct bl_site *s)
{
	int n = 0;
	for (int i = 0; i < s->n; i++)
		n += bl_cell_missing(w, &s->c[i]);
	return n;
}

/* the ground of a column near height y: the first empty cell above solid ground, at most two blocks up or down (a wall that stands there is not ground to build on:
 * its cells are the ones that are asked for again) */
static bool column_base(const struct bl_world *w, int x, int y, int z, int *base)
{
	for (int up = 0; up <= 2 && bl_solid(bl_get(w, x, y, z)) && bl_get(w, x, y, z) != BL_COBBLE; up++)
		y++;
	for (int down = 0; down < 2 && !bl_solid(bl_get(w, x, y - 1, z)); down++)
		y--;
	*base = y;
	return bl_solid(bl_get(w, x, y - 1, z)) && (!bl_solid(bl_get(w, x, y, z)) || bl_get(w, x, y, z) == BL_COBBLE);
}

bool bl_plan_lay(const struct bl_world *w, int plan, int rot, int x, int y, int z, const struct bl_site *others, int nothers, struct bl_site *out, const char **why)
{
	static const int k_cos[4] = { 1, 0, -1, 0 }, k_sin[4] = { 0, 1, 0, -1 };
	struct pcell p[BL_SITE_CELLS];
	int n = plan_cells(plan, p);
	const char *err = NULL;
	memset(out, 0, sizeof *out);
	out->plan = plan;
	rot &= 3;
	if (!n) {
		if (why)
			*why = "There is no such building";
		return false;
	}
	const bool follows = plan == BL_PLAN_WALL, joins = plan == BL_PLAN_WALL || plan == BL_PLAN_GATE;
	for (int i = 0; i < n; i++) {
		int cx = x + p[i].u * k_cos[rot] + p[i].v * k_sin[rot], cz = z - p[i].u * k_sin[rot] + p[i].v * k_cos[rot], base = y;
		if (follows) {
			if (!column_base(w, cx, y, cz, &base) && !err) {
				/* (a column of the wall that is already built is its own ground: the cell is looked at below) */
				int b = bl_get(w, cx, base - 1, cz);
				if (!bl_solid(b))
					err = "There is no ground for it";
			}
		}
		struct bl_cell c = { (int16_t)cx, (int16_t)(base + p[i].y), (int16_t)cz, (uint8_t)p[i].block };
		if (cx < 1 || cz < 1 || cx >= w->w - 1 || cz >= w->d - 1 || c.y < 1 || c.y >= w->h - 2) {
			if (!err)
				err = "Too near the edge of the world";
		}
		int cur = bl_get(w, c.x, c.y, c.z);
		if (joins && cur == c.block)
			continue;                                   /* already there: walls meet at a corner */
		if (cur != BL_AIR && !err)
			err = bl_water(cur) ? "Not in water" : "Something is in the way";
		if (!follows && p[i].y == 0 && !bl_solid(bl_get(w, c.x, c.y - 1, c.z)) && !err)
			err = "The ground is not flat there";
		bool dup = false;
		for (int s = 0; s < nothers; s++) {
			if (!others[s].on)
				continue;
			for (int k = 0; k < others[s].n; k++)
				if (others[s].c[k].x == c.x && others[s].c[k].y == c.y && others[s].c[k].z == c.z) {
					if (joins && others[s].c[k].block == c.block)
						dup = true;
					else if (!err)
						err = "Another building is planned there";
				}
		}
		if (!dup)
			out->c[out->n++] = c;
	}
	if (!joins) {                                           /* a house wants its whole floor: the ground under the inside, and nothing inside it or round another plan */
		int lo[3], hi[3];
		bl_site_box(out, lo, hi);
		for (int cz = lo[2]; cz <= hi[2] && !err; cz++)
			for (int cx = lo[0]; cx <= hi[0] && !err; cx++) {
				if (!bl_solid(bl_get(w, cx, y - 1, cz)))
					err = "The ground is not flat there";
				for (int cy = y; cy <= hi[1] && !err; cy++)
					if (bl_get(w, cx, cy, cz) != BL_AIR)
						err = "Something is in the way";
			}
		for (int s = 0; s < nothers && !err; s++) {
			if (!others[s].on)
				continue;
			int olo[3], ohi[3];
			bl_site_box(&others[s], olo, ohi);
			if (olo[0] <= hi[0] && ohi[0] >= lo[0] && olo[2] <= hi[2] && ohi[2] >= lo[2])
				err = "Another building is planned there";
		}
	}
	if (!out->n && !err)
		err = "It is there already";
	out->on = !err;
	if (why)
		*why = err;
	return !err;
}

/* ---- the ghosts ------------------------------------------------------------------------------------------------------- */

void bl_ghosts_clear(struct bl_ghosts *g)
{
	g->n = 0;
	g->ngroups = 0;
}

void bl_ghosts_add(struct bl_ghosts *g, const struct bl_world *w, const struct bl_site *s, uint32_t rgb)
{
	if (g->ngroups >= BL_GHOST_GROUPS)
		return;
	struct bl_ghost_group *gr = &g->group[g->ngroups];
	gr->first = g->n;
	gr->n = 0;
	for (int k = 0; k < 3; k++) {
		gr->lo[k] = 1e9f;
		gr->hi[k] = -1e9f;
	}
	for (int i = 0; i < s->n && i < BL_SITE_CELLS; i++) {
		const struct bl_cell *c = &s->c[i];
		if (w && !bl_cell_missing(w, c))
			continue;
		g->g[g->n++] = (struct bl_ghost){ c->x, c->y, c->z, rgb };
		gr->n++;
		const float p[3] = { (float)c->x, (float)c->y, (float)c->z };
		for (int k = 0; k < 3; k++) {
			gr->lo[k] = p[k] < gr->lo[k] ? p[k] : gr->lo[k];
			gr->hi[k] = p[k] + 1.0f > gr->hi[k] ? p[k] + 1.0f : gr->hi[k];
		}
	}
	if (gr->n)
		g->ngroups++;
}

/* the ray into a box: the distance in and the axis of the face, or false */
static bool slab_in(const float o[3], const float d[3], const float lo[3], const float hi[3], float tmax, float *t, int *axis)
{
	float t0 = 0.0f, t1 = tmax;
	int ax = -1;
	for (int a = 0; a < 3; a++) {
		if (fabsf(d[a]) < 1e-9f) {
			if (o[a] < lo[a] || o[a] > hi[a])
				return false;
			continue;
		}
		float s0 = (lo[a] - o[a]) / d[a], s1 = (hi[a] - o[a]) / d[a];
		if (s0 > s1) {
			float x = s0;
			s0 = s1;
			s1 = x;
		}
		if (s0 > t0) {
			t0 = s0;
			ax = a;
		}
		if (s1 < t1)
			t1 = s1;
		if (t0 > t1)
			return false;
	}
	*t = t0;
	*axis = ax;
	return true;
}

bool bl_ghosts_ray(const struct bl_ghosts *g, const float o[3], const float d[3], float tmax, float rgb[3], float *a, float *t)
{
	float best = tmax;
	bool found = false;
	for (int gi = 0; gi < g->ngroups; gi++) {
		const struct bl_ghost_group *gr = &g->group[gi];
		float tt;
		int ax;
		if (!slab_in(o, d, gr->lo, gr->hi, best, &tt, &ax))
			continue;
		for (int i = gr->first; i < gr->first + gr->n; i++) {
			const struct bl_ghost *c = &g->g[i];
			const float lo[3] = { (float)c->x + 0.02f, (float)c->y + 0.02f, (float)c->z + 0.02f }, hi[3] = { (float)c->x + 0.98f, (float)c->y + 0.98f, (float)c->z + 0.98f };
			if (!slab_in(o, d, lo, hi, best, &tt, &ax) || ax < 0 || tt >= best)
				continue;
			best = tt;
			found = true;
			/* near an edge of the face it came in by: the frame of the box */
			int edges = 0;
			for (int k = 0; k < 3; k++) {
				float f = o[k] + d[k] * tt - lo[k];
				if (k != ax && (f < 0.09f || f > 0.87f))
					edges++;
			}
			const float shade = ax == 1 ? 1.0f : ax == 0 ? 0.75f : 0.88f;
			rgb[0] = (float)((c->rgb >> 16) & 255) / 255.0f * shade;
			rgb[1] = (float)((c->rgb >> 8) & 255) / 255.0f * shade;
			rgb[2] = (float)(c->rgb & 255) / 255.0f * shade;
			*a = edges ? 0.85f : 0.30f;
			*t = tt;
		}
	}
	return found;
}
