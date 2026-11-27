/* blockshero.c - the player's body. See blockshero.h. */
#include "blockshero.h"
#include "blocksmobs.h"
#include "blocksview.h"

#include <math.h>
#include <string.h>

enum { P_BODY, P_LEG_L, P_LEG_R, P_ARM_L, P_ARM_R, P_HEAD, P_CAPE, P_HAND };

struct part {
	float x0, x1, y0, y1, z0, z1;           /* the hero's own frame: x right, y up, z forward, the feet at 0; 1.8 high, the eyes at 1.62 */
	uint32_t rgb;
	int kind;
};

#define SKIN 0xd9a066u
#define TUNIC 0x7a45b8u
#define BOOT 0x2e2018u
#define TROUSER 0x54402cu
#define HAIR 0x5a3a1cu

static const struct part k_hero[] = {
	{ -0.22f, -0.02f, 0.14f, 0.78f, -0.10f, 0.10f, TROUSER, P_LEG_L },
	{ 0.02f, 0.22f, 0.14f, 0.78f, -0.10f, 0.10f, TROUSER, P_LEG_R },
	{ -0.23f, -0.01f, 0.00f, 0.14f, -0.11f, 0.17f, BOOT, P_LEG_L },
	{ 0.01f, 0.23f, 0.00f, 0.14f, -0.11f, 0.17f, BOOT, P_LEG_R },
	{ -0.26f, 0.26f, 0.78f, 0.87f, -0.145f, 0.145f, 0xd9a82cu, P_BODY },           /* the belt */
	{ -0.26f, 0.26f, 0.87f, 1.38f, -0.14f, 0.14f, TUNIC, P_BODY },
	{ -0.39f, -0.27f, 1.06f, 1.37f, -0.08f, 0.08f, TUNIC, P_ARM_L },
	{ -0.385f, -0.275f, 0.78f, 1.06f, -0.07f, 0.07f, SKIN, P_ARM_L },
	{ 0.27f, 0.39f, 1.06f, 1.37f, -0.08f, 0.08f, TUNIC, P_ARM_R },
	{ 0.275f, 0.385f, 0.78f, 1.06f, -0.07f, 0.07f, SKIN, P_ARM_R },
	{ -0.18f, 0.18f, 1.38f, 1.72f, -0.17f, 0.17f, SKIN, P_HEAD },
	{ -0.20f, 0.20f, 1.70f, 1.80f, -0.20f, 0.19f, HAIR, P_HEAD },
	{ -0.20f, 0.20f, 1.46f, 1.70f, -0.20f, -0.16f, HAIR, P_HEAD },
	{ -0.11f, -0.05f, 1.58f, 1.64f, 0.165f, 0.18f, 0x2250a8u, P_HEAD },
	{ 0.05f, 0.11f, 1.58f, 1.64f, 0.165f, 0.18f, 0x2250a8u, P_HEAD },
	{ -0.25f, 0.25f, 0.42f, 1.34f, -0.21f, -0.155f, 0xe8e8f0u, P_CAPE },
};
#define N_PARTS ((int)(sizeof k_hero / sizeof k_hero[0]))

static float clamp01(float v)
{
	return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

#define NEAR_CUT 0.01f

static void rect_add(struct bl_hero_box *b, float x, float y)
{
	b->sx0 = x < b->sx0 ? x : b->sx0;
	b->sx1 = x > b->sx1 ? x : b->sx1;
	b->sy0 = y < b->sy0 ? y : b->sy0;
	b->sy1 = y > b->sy1 ? y : b->sy1;
}

/* The rectangle of the picture a box can be seen in: its corners as the camera sees them (`corner` gives corner k in the camera's frame), an edge that goes behind
 * the camera cut just in front of it. A box that is all behind gets an empty rectangle: no ray of the picture meets it. */
static void screen_rect(struct bl_hero_box *b, const struct bl_hero *h, void (*corner)(const struct bl_hero *, const float p[3], float out[3]))
{
	float c[8][3];
	b->bound = true;
	b->sx0 = b->sy0 = 1e9f;
	b->sx1 = b->sy1 = -1e9f;
	for (int k = 0; k < 8; k++) {
		const float p[3] = { k & 1 ? b->hi[0] : b->lo[0], k & 2 ? b->hi[1] : b->lo[1], k & 4 ? b->hi[2] : b->lo[2] };
		corner(h, p, c[k]);
		if (c[k][2] >= NEAR_CUT)
			rect_add(b, c[k][0] / c[k][2], c[k][1] / c[k][2]);
	}
	for (int k = 0; k < 8; k++)
		for (int bit = 1; bit < 8; bit <<= 1) {
			if (k & bit)
				continue;
			const float *p = c[k], *q = c[k | bit];
			if ((p[2] >= NEAR_CUT) == (q[2] >= NEAR_CUT))
				continue;
			float f = (NEAR_CUT - p[2]) / (q[2] - p[2]);
			rect_add(b, (p[0] + (q[0] - p[0]) * f) / NEAR_CUT, (p[1] + (q[1] - p[1]) * f) / NEAR_CUT);
		}
	b->sx0 -= 0.002f;
	b->sy0 -= 0.002f;
	b->sx1 += 0.002f;
	b->sy1 += 0.002f;
}

static void to_camera(const struct bl_hero *h, const float v[3], float out[3])
{
	out[0] = v[0] * h->r[0] + v[2] * h->r[2];
	out[1] = v[0] * h->u[0] + v[1] * h->u[1] + v[2] * h->u[2];
	out[2] = v[0] * h->f[0] + v[1] * h->f[1] + v[2] * h->f[2];
}

/* a corner of a box of the figure: his own frame -> the world -> the camera */
static void body_corner(const struct bl_hero *h, const float p[3], float out[3])
{
	const float w[3] = { h->x + p[0] * h->ycos + p[2] * h->ysin - h->eye[0], h->y + p[1] - h->eye[1], h->z - p[0] * h->ysin + p[2] * h->ycos - h->eye[2] };
	to_camera(h, w, out);
}

/* a corner of a box of the arm: the arm's frame -> the camera's (the other way round from `into_arm`) */
static void arm_corner(const struct bl_hero *h, const float p[3], float out[3])
{
	float y = p[1] * h->bc + p[2] * h->bs, z = -p[1] * h->bs + p[2] * h->bc;
	out[0] = p[0] * h->ac + z * h->as + h->arm[0];
	out[1] = y + h->arm[1];
	out[2] = -p[0] * h->as + z * h->ac + h->arm[2];
}

/* a direction (or a point, the arm's pivot taken off) of the camera's frame into the arm's: undo its yaw about y, then its pitch about x */
static void into_arm(const struct bl_hero *h, float v[3])
{
	float x = v[0] * h->ac - v[2] * h->as, z = v[0] * h->as + v[2] * h->ac, y = v[1];
	v[0] = x;
	v[1] = y * h->bc - z * h->bs;
	v[2] = y * h->bs + z * h->bc;
}

static void add_arm(struct bl_hero *h, float x0, float x1, float y0, float y1, float z0, float z1, uint32_t rgb, int block)
{
	struct bl_hero_box *b = &h->vm[h->narm++];
	*b = (struct bl_hero_box){ .lo = { x0, y0, z0 }, .hi = { x1, y1, z1 }, .rgb = rgb, .block = block };
	screen_rect(b, h, arm_corner);
}

void bl_hero_set(struct bl_hero *h, const struct bl_world *w, float time_h)
{
	h->ycos = cosf(h->yaw);
	h->ysin = sinf(h->yaw);
	float cp = cosf(h->pitch), sp = sinf(h->pitch);
	h->f[0] = cp * h->ysin;
	h->f[1] = sp;
	h->f[2] = cp * h->ycos;
	h->r[0] = h->ycos;
	h->r[1] = 0.0f;
	h->r[2] = -h->ysin;
	h->u[0] = -sp * h->ysin;
	h->u[1] = cp;
	h->u[2] = -sp * h->ycos;
	bl_mobs_light_at(w, time_h, h->x, h->y, h->z, h->lit);

	/* the figure */
	const float walk = sinf(h->phase * 6.2831853f) * clamp01(h->stride);
	const float sw = h->swing > 0.0f ? sinf((1.0f - clamp01(h->swing)) * 3.14159265f) : 0.0f;
	h->nbody = 0;
	/* the weapons seen from outside, two boxes each, in the right hand */
	static const struct part k_held[6][2] = {
		{ { 0.31f, 0.35f, 0.88f, 1.58f, 0.08f, 0.12f, 0xdde2e8u, P_HAND }, { 0.26f, 0.40f, 0.84f, 0.89f, 0.07f, 0.13f, 0x8a6a30u, P_HAND } },   /* sword */
		{ { 0.31f, 0.35f, 0.40f, 1.80f, 0.08f, 0.12f, 0x6a4a2au, P_HAND }, { 0.29f, 0.37f, 1.80f, 2.02f, 0.07f, 0.13f, 0xd8dee6u, P_HAND } },   /* spear */
		{ { 0.31f, 0.35f, 0.70f, 1.45f, 0.08f, 0.12f, 0x6a4a2au, P_HAND }, { 0.23f, 0.43f, 1.45f, 1.68f, -0.04f, 0.24f, 0x7a808au, P_HAND } },  /* war hammer */
		{ { 0.31f, 0.35f, 0.55f, 1.35f, 0.24f, 0.28f, 0x8a5a2au, P_HAND }, { 0.32f, 0.34f, 0.55f, 1.35f, 0.14f, 0.15f, 0xe8e8e0u, P_HAND } },   /* bow */
		{ { 0.31f, 0.35f, 0.45f, 1.55f, 0.08f, 0.12f, 0x3a2418u, P_HAND }, { 0.26f, 0.40f, 1.55f, 1.70f, 0.03f, 0.17f, 0xffa020u, P_HAND } },   /* fire staff */
		{ { 0.29f, 0.38f, 0.74f, 0.86f, 0.10f, 0.52f, 0x9aa2aeu, P_HAND }, { 0.30f, 0.37f, 0.76f, 0.84f, 0.52f, 0.58f, 0xff2020u, P_HAND } },   /* laser */
	};
	const int wp = h->weapon >= 0 && h->weapon < 6 ? h->weapon : 0;
	for (int k = 0; k < N_PARTS + 2; k++) {
		struct part p;
		if (k < N_PARTS) {
			p = k_hero[k];
		} else if (h->sword) {
			p = k_held[wp][k - N_PARTS];
		} else if (h->held > 0 && k == N_PARTS) {                /* ... or the block */
			p = (struct part){ 0.25f, 0.43f, 0.70f, 0.88f, 0.07f, 0.25f, bl_block_color(h->held), P_HAND };
		} else {
			continue;
		}
		if (h->first && (p.kind == P_HEAD || p.kind == P_CAPE || p.kind == P_ARM_R || p.kind == P_HAND))
			continue;
		float dz = 0.0f, dy = 0.0f;
		switch (p.kind) {
		case P_LEG_L: dz = 0.20f * walk; break;
		case P_LEG_R: dz = -0.20f * walk; break;
		case P_ARM_L: dz = -0.14f * walk; break;
		case P_ARM_R: case P_HAND: dz = 0.14f * walk + 0.28f * sw; dy = 0.10f * sw; break;
		default: break;
		}
		const bool leg = p.kind == P_LEG_L || p.kind == P_LEG_R;
		if (h->first && !leg) {                                  /* the trunk and the left arm: behind the eyes, and no higher than the chest */
			dz -= BL_HERO_BACK;
			if (p.y1 > BL_HERO_CHEST)
				p.y1 = BL_HERO_CHEST;
		}
		if (h->sneak && !leg)
			dy -= BL_HERO_SNEAK;
		struct bl_hero_box *b = &h->body[h->nbody++];
		*b = (struct bl_hero_box){ .lo = { p.x0, p.y0 + dy, p.z0 + dz }, .hi = { p.x1, p.y1 + dy, p.z1 + dz }, .rgb = p.rgb };
		if (h->first)
			screen_rect(b, h, body_corner);
	}

	/* the arm: it comes in from the lower right and points a little up and to the middle; a swing takes it forward and down and back */
	h->narm = 0;
	if (!h->first)
		return;
	const float z = 0.66f, th = tanf(h->fov * 0.5f), asp = h->aspect < 1.9f ? h->aspect : 1.9f;
	float bob = h->stride * sinf(h->phase * 6.2831853f);
	h->arm[0] = 0.66f * th * asp * z - 0.07f * sw + 0.012f * bob;
	h->arm[1] = -0.80f * th * z - 0.03f * sw - 0.014f * fabsf(bob);
	h->arm[2] = z + 0.09f * sw;
	float yaw = -0.42f - 0.25f * sw, pitch = 0.50f - 0.95f * sw;
	h->ac = cosf(yaw);
	h->as = sinf(yaw);
	h->bc = cosf(pitch);
	h->bs = sinf(pitch);
	for (int i = 0; i < 3; i++)
		h->vo[i] = -h->arm[i];
	into_arm(h, h->vo);
	add_arm(h, -0.062f, 0.062f, -0.062f, 0.062f, -0.45f, -0.02f, TUNIC, 0);         /* the sleeve */
	add_arm(h, -0.050f, 0.050f, -0.050f, 0.050f, -0.02f, 0.17f, SKIN, 0);           /* the forearm and the hand */
	if (h->sword && wp == 1) {                                                          /* spear */
		add_arm(h, -0.012f, 0.012f, -0.20f, 0.62f, 0.108f, 0.132f, 0x7a5428u, 0);
		add_arm(h, -0.028f, 0.028f, 0.62f, 0.74f, 0.104f, 0.136f, 0xdde2e8u, 0);
	} else if (h->sword && wp == 2) {                                                   /* war hammer */
		add_arm(h, -0.016f, 0.016f, -0.08f, 0.36f, 0.108f, 0.14f, 0x7a5428u, 0);
		add_arm(h, -0.085f, 0.085f, 0.36f, 0.48f, 0.07f, 0.19f, 0x8a929cu, 0);
	} else if (h->sword && wp == 3) {                                                   /* bow */
		add_arm(h, -0.012f, 0.012f, -0.16f, 0.40f, 0.20f, 0.225f, 0x8a5a2au, 0);
		add_arm(h, -0.003f, 0.003f, -0.16f, 0.40f, 0.10f, 0.106f, 0xe8e8e0u, 0);
		add_arm(h, -0.005f, 0.005f, 0.11f, 0.125f, 0.10f, 0.40f, 0xc8b070u, 0);
	} else if (h->sword && wp == 4) {                                                   /* fire staff */
		add_arm(h, -0.014f, 0.014f, -0.18f, 0.40f, 0.108f, 0.136f, 0x3a2418u, 0);
		add_arm(h, -0.040f, 0.040f, 0.40f, 0.49f, 0.085f, 0.165f, 0xffa020u, -1);
		add_arm(h, -0.018f, 0.018f, 0.49f, 0.53f, 0.105f, 0.14f, 0xffe060u, -1);
	} else if (h->sword && wp == 5) {                                                   /* laser */
		add_arm(h, -0.030f, 0.030f, -0.01f, 0.09f, 0.11f, 0.15f, 0x30343cu, 0);
		add_arm(h, -0.040f, 0.040f, 0.09f, 0.18f, 0.10f, 0.40f, 0x9aa2aeu, 0);
		add_arm(h, -0.018f, 0.018f, 0.115f, 0.155f, 0.40f, 0.46f, 0xff2020u, -1);
	} else if (h->sword) {
		add_arm(h, -0.014f, 0.014f, -0.07f, 0.11f, 0.11f, 0.14f, 0x4a3320u, 0);     /* the grip */
		add_arm(h, -0.075f, 0.075f, 0.11f, 0.135f, 0.095f, 0.155f, 0xb08a3au, 0);   /* the guard */
		add_arm(h, -0.012f, 0.012f, 0.135f, 0.52f, 0.108f, 0.142f, 0xdde2e8u, 0);   /* the blade */
	} else if (h->held == BL_TORCH) {
		add_arm(h, -0.016f, 0.016f, -0.02f, 0.17f, 0.12f, 0.152f, 0x7a5428u, 0);
		add_arm(h, -0.026f, 0.026f, 0.17f, 0.23f, 0.11f, 0.162f, 0xffd060u, -1);    /* the flame shines by itself */
	} else if (h->held > 0 && h->held < BL_COUNT) {
		add_arm(h, -0.075f, 0.075f, -0.02f, 0.13f, 0.14f, 0.29f, 0, h->held);
	}
}

/* a ray into a box: the distance in and the axis of the face it comes through (-1: it starts inside), or false */
static bool box_in(const float o[3], const float d[3], const float lo[3], const float hi[3], float tmax, float *t, int *axis)
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

static void lit_colour(const struct bl_hero *h, uint32_t c, float shade, float rgb[3])
{
	for (int i = 0; i < 3; i++) {
		float v = (float)((c >> (16 - 8 * i)) & 255) / 255.0f * shade * h->lit[i];
		if (h->hurt > 0.0f) {
			float f = clamp01(h->hurt) * 0.5f;
			v = v * (1.0f - f) + (i == 0 ? 1.0f : 0.1f) * f;
		}
		rgb[i] = clamp01(v);
	}
}

bool bl_hero_ray(const struct bl_hero *h, const float o[3], const float d[3], float tmax, float rgb[3], float *t)
{
	/* a cheap no: nothing of him is above `top` over his feet (in the first person that is the chest, under the eyes) or further than 0.75 sideways */
	const float top = h->first ? BL_HERO_CHEST + 0.01f : 2.1f;
	if (o[1] > h->y + top && d[1] >= 0.0f)
		return false;
	float wlo[3] = { h->x - 0.75f, h->y - 0.01f, h->z - 0.75f }, whi[3] = { h->x + 0.75f, h->y + top, h->z + 0.75f }, tt;
	int ax;
	/* from his own eyes the rectangles of the boxes in the picture say at once which can be met at all */
	const bool seen = h->first && o[0] == h->eye[0] && o[1] == h->eye[1] && o[2] == h->eye[2];
	float sx = 0.0f, sy = 0.0f;
	bool front = false;
	if (seen) {
		float c[3];
		to_camera(h, d, c);
		front = c[2] > 1e-4f;
		if (front) {
			sx = c[0] / c[2];
			sy = c[1] / c[2];
		}
	} else if (!box_in(o, d, wlo, whi, tmax, &tt, &ax)) {
		return false;
	}
	float lo_[3], ld[3];
	bool turned = false, found = false;
	float best = tmax;
	for (int k = 0; k < h->nbody; k++) {
		const struct bl_hero_box *b = &h->body[k];
		if (front && b->bound && (sx < b->sx0 || sx > b->sx1 || sy < b->sy0 || sy > b->sy1))
			continue;
		if (!turned) {                                           /* into his own frame */
			float rx = o[0] - h->x, ry = o[1] - h->y, rz = o[2] - h->z;
			lo_[0] = rx * h->ycos - rz * h->ysin;
			lo_[1] = ry;
			lo_[2] = rx * h->ysin + rz * h->ycos;
			ld[0] = d[0] * h->ycos - d[2] * h->ysin;
			ld[1] = d[1];
			ld[2] = d[0] * h->ysin + d[2] * h->ycos;
			turned = true;
		}
		if (!box_in(lo_, ld, b->lo, b->hi, best, &tt, &ax) || ax < 0 || tt >= best)
			continue;
		best = tt;
		found = true;
		*t = tt;
		lit_colour(h, b->rgb, ax == 1 ? (ld[1] < 0.0f ? 1.0f : 0.55f) : ax == 0 ? 0.70f : 0.85f, rgb);
	}
	return found;
}

bool bl_hero_arm_ray(const struct bl_hero *h, const float d[3], float rgb[3])
{
	float v[3];
	to_camera(h, d, v);
	if (v[2] <= 0.05f || v[0] < 0.0f)                            /* the arm is on the right */
		return false;
	const float sx = v[0] / v[2], sy = v[1] / v[2];
	bool turned = false, found = false;
	float best = 10.0f;
	for (int k = 0; k < h->narm; k++) {
		const struct bl_hero_box *b = &h->vm[k];
		if (b->bound && (sx < b->sx0 || sx > b->sx1 || sy < b->sy0 || sy > b->sy1))
			continue;
		if (!turned) {
			into_arm(h, v);
			turned = true;
		}
		float t;
		int ax;
		if (!box_in(h->vo, v, b->lo, b->hi, best, &t, &ax) || ax < 0 || t >= best)
			continue;
		float shade = ax == 1 ? (v[1] < 0.0f ? 1.0f : 0.55f) : ax == 0 ? 0.70f : 0.85f;
		uint32_t c = b->rgb;
		if (b->block > 0) {                                      /* the block's own picture on the face; a see-through spot of it lets the ray go on */
			float q[3];
			for (int i = 0; i < 3; i++)
				q[i] = clamp01((h->vo[i] + v[i] * t - b->lo[i]) / (b->hi[i] - b->lo[i]));
			const int fk = ax == 1 ? (v[1] < 0.0f ? BL_FACE_TOP : BL_FACE_BOTTOM) : BL_FACE_SIDE;
			float tu = ax == 0 ? q[2] : q[0], tv = ax == 1 ? q[2] : 1.0f - q[1];
			const uint32_t *tex = bl_texture(b->block, fk);
			if (!tex)
				continue;
			c = tex[(int)(tv * 15.99f) * 16 + (int)(tu * 15.99f)];
			if ((c >> 24) < 40)
				continue;
		}
		best = t;
		found = true;
		if (b->block < 0) {
			rgb[0] = (float)((c >> 16) & 255) / 255.0f;
			rgb[1] = (float)((c >> 8) & 255) / 255.0f;
			rgb[2] = (float)(c & 255) / 255.0f;
		} else {
			lit_colour(h, c, shade, rgb);
		}
	}
	return found;
}

float bl_hero_floor_shade(const struct bl_hero *h, float x, float y, float z)
{
	float up = h->y - y;                                        /* how high his feet are above this top */
	if (up < -0.05f || up > 3.0f)
		return 1.0f;
	float dx = x - h->x, dz = z - h->z, d2 = dx * dx + dz * dz, rad = 0.46f;
	if (d2 >= rad * rad)
		return 1.0f;
	float k = (1.0f - d2 / (rad * rad)) * (1.0f - clamp01(up) * 0.7f);
	return 1.0f - 0.45f * k;
}
