/* blocksview.c - the picture of a block world. See blocksview.h. */
#include "blocksview.h"
#include "blockspets.h"
#include "blocksbuild.h"
#include "blockshero.h"
#include "blocksmobs.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ---- the textures --------------------------------------------------------------------------------------------------- */

#define TEX 16

static uint32_t g_tex[BL_COUNT][BL_FACE_KINDS][TEX * TEX];
static float g_avg[BL_COUNT][BL_FACE_KINDS][3];          /* 0..1 */
static bool g_ready;

static inline uint32_t hmix(uint32_t h)
{
	h ^= h >> 16;
	h *= 0x7feb352dU;
	h ^= h >> 15;
	h *= 0x846ca68bU;
	h ^= h >> 16;
	return h;
}

static inline uint32_t hxy(int x, int y, uint32_t s)
{
	return hmix((uint32_t)x * 0x9e3779b1U + (uint32_t)y * 0x85ebca77U + s * 0xc2b2ae3dU + 0x27d4eb2fU);
}

/* a number in -amp .. amp that belongs to this pixel */
static inline int tn(int x, int y, uint32_t s, int amp)
{
	return (int)(hxy(x, y, s) % (uint32_t)(2 * amp + 1)) - amp;
}

static inline int clamp8(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : v;
}

static inline uint32_t argb(int r, int g, int b, int a)
{
	return (uint32_t)clamp8(a) << 24 | (uint32_t)clamp8(r) << 16 | (uint32_t)clamp8(g) << 8 | (uint32_t)clamp8(b);
}

/* 0xRRGGBB plus the same number on every channel */
static inline uint32_t tinted(uint32_t rgb, int d)
{
	return argb((int)((rgb >> 16) & 255) + d, (int)((rgb >> 8) & 255) + d, (int)(rgb & 255) + d, 255);
}

static void gen_face(int blk, int fk, uint32_t *px)
{
	for (int y = 0; y < TEX; y++)
		for (int x = 0; x < TEX; x++) {
			uint32_t c = 0xff000000u;
			switch (blk) {
			case BL_GRASS:
				if (fk == BL_FACE_TOP) {
					c = tinted(0x5ea43a, tn(x, y, 1, 13) - (hxy(x, y, 2) % 10 == 0 ? 24 : 0));
				} else if (fk == BL_FACE_BOTTOM) {
					c = tinted(0x86603f, tn(x, y, 3, 11));
				} else {
					int depth = 2 + (int)(hxy(x, 0, 4) % 3U);
					if (y < depth)
						c = tinted(0x5ea43a, tn(x, y, 1, 12) - (y == depth - 1 ? 16 : 0));
					else
						c = tinted(0x86603f, tn(x, y, 3, 11));
				}
				break;
			case BL_DIRT:
				c = tinted(0x86603f, tn(x, y, 3, 11) + (hxy(x, y, 9) % 12 == 0 ? -18 : 0));
				break;
			case BL_STONE:
				c = tinted(0x808080, tn(x, y, 5, 9) - (hxy(x / 2, y / 2, 6) % 7 == 0 ? 22 : 0));
				break;
			case BL_SAND:
				c = tinted(0xdad29e, tn(x, y, 7, 7) - (hxy(x, y, 8) % 14 == 0 ? 16 : 0));
				break;
			case BL_LOG:
				if (fk == BL_FACE_SIDE) {
					c = tinted(0x6e5533, tn(x, 0, 10, 9) + tn(x, y, 11, 4));
				} else {
					int dx = abs(2 * x - 15), dy = abs(2 * y - 15);
					int d = (dx > dy ? dx : dy) / 2;           /* 0..7: rings out from the middle */
					if (d >= 7)
						c = tinted(0x5b4528, tn(x, y, 12, 5));
					else
						c = tinted((d & 1) ? 0x8e7143 : 0xa88a56, tn(x, y, 13, 4));
				}
				break;
			case BL_LEAVES:
				c = tinted(0x3f8a2b, tn(x, y, 14, 20) - (hxy(x, y, 15) % 8 == 0 ? 26 : 0));
				break;
			case BL_PLANKS: {
				int board = y / 4;
				int joint = (int)(hxy(0, board, 16) % 16U);
				int d = tn(0, board, 17, 8) + tn(x, y, 18, 4);
				if (y % 4 == 3 || x == joint)
					d -= 30;
				c = tinted(0xb08c55, d);
				break;
			}
			case BL_GLASS:
				if (x == 0 || y == 0 || x == TEX - 1 || y == TEX - 1)
					c = argb(216, 240, 246, 220);
				else if ((x + y == 9 || x + y == 10) && x >= 3 && x <= 8 && y >= 3 && y <= 8)
					c = argb(255, 255, 255, 80);
				else
					c = argb(0, 0, 0, 0);
				break;
			case BL_WATER: case BL_FLOW1: case BL_FLOW2: case BL_FLOW3: case BL_FLOW4: case BL_FLOW5: case BL_FLOW6: case BL_FLOW7:
				c = argb(0x3a + tn(x, y, 19, 12), 0x78 + tn(x, y, 20, 12), 0xd0 + tn(x, y, 21, 12), fk == BL_FACE_TOP ? 100 : 55);
				break;
			case BL_BEDROCK:
				c = tinted(0x353535, tn(x, y, 22, 14) - (hxy(x / 3, y / 3, 23) % 3 == 0 ? 14 : 0));
				break;
			case BL_BRICK: {
				int row = y / 4;
				bool mortar = (y % 4 == 3) || ((x + (row & 1) * 4) % 8 == 7);
				if (mortar)
					c = tinted(0xb9b2a6, tn(x, y, 24, 5));
				else
					c = tinted(0xa14b34, tn((x + (row & 1) * 4) / 8, row, 25, 12) + tn(x, y, 26, 6));
				break;
			}
			case BL_SNOW:
				c = tinted(0xf6f9fc, tn(x, y, 27, 4));
				break;
			case BL_TORCH:                               /* a stick with a flame on a clear background (the picture of the block; the renderer draws a thin box) */
				if ((x == 7 || x == 8) && y >= 6)
					c = tinted(0x7a5428, tn(x, y, 30, 6));
				else if (x >= 6 && x <= 9 && y >= 2 && y <= 5 && !((x == 6 || x == 9) && (y == 2 || y == 5)))
					c = argb(255, y <= 3 ? 0xf0 : 0xb0, y <= 3 ? 0x80 : 0x28, 255);
				else
					c = argb(0, 0, 0, 0);
				break;
			case BL_LAMP:                                /* a glowing block: bright cells with darker seams */
				c = (x % 8 == 0 || y % 8 == 0) ? tinted(0xc88a2c, tn(x, y, 31, 6)) : tinted(0xffe8a0, tn(x, y, 32, 10));
				break;
			case BL_KEEP: case BL_KEEPFLAG: {            /* grey bricks; on the flag's sides a golden rim and a red shield with a gold cross */
				int row = y / 4;
				bool mortar = (y % 4 == 3) || ((x + (row & 1) * 4) % 8 == 7);
				c = mortar ? tinted(0x6a6a72, tn(x, y, 40, 4)) : tinted(0x9a9ba4, tn((x + (row & 1) * 4) / 8, row, 41, 10) + tn(x, y, 42, 5));
				if (blk == BL_KEEP)
					break;
				if (x == 0 || x == TEX - 1 || y == 0 || y == TEX - 1)
					c = tinted(0xd9a82c, tn(x, y, 43, 6));
				if (fk == BL_FACE_SIDE) {
					int dx = x - 7 > 0 ? x - 7 : 7 - x, dy = y - 8 > 0 ? y - 8 : 8 - y;
					if (x >= 4 && x <= 11 && y >= 4 && y <= 12 && !(y > 10 && dx < 3 && y > 11 + dx))
						c = tinted(0xb02020, tn(x, y, 44, 6));
					if ((x == 7 || x == 8) && y >= 5 && y <= 11)
						c = tinted(0xf0c040, 0);
					if ((y == 7 || y == 8) && x >= 5 && x <= 10)
						c = tinted(0xf0c040, 0);
					(void)dx;
					(void)dy;
				} else if (fk == BL_FACE_TOP && ((x - 8) * (x - 8) + (y - 8) * (y - 8) < 14)) {
					c = tinted(0xd9a82c, tn(x, y, 45, 8));
				}
				break;
			}
			case BL_BARRACKS: {                          /* red brick in a timber frame; on the sides a blue banner with crossed swords, on the top planks */
				int row = y / 4;
				bool mortar = (y % 4 == 3) || ((x + (row & 1) * 4) % 8 == 7);
				c = fk == BL_FACE_TOP ? tinted(0x8a6a40, tn(x, y / 4, 50, 8) + tn(x, y, 51, 5) - (y % 4 == 3 ? 24 : 0))
				                      : mortar ? tinted(0x9c8f80, tn(x, y, 52, 5)) : tinted(0x8e3f2e, tn((x + (row & 1) * 4) / 8, row, 53, 12) + tn(x, y, 54, 6));
				if (x <= 1 || x >= TEX - 2 || y <= 1 || y >= TEX - 2)
					c = tinted(0x4a3320, tn(x, y, 55, 6));
				if (fk == BL_FACE_SIDE && x >= 4 && x <= 11 && y >= 3 && y <= 12) {
					c = tinted(0x2c4fa8, tn(x, y, 56, 6));
					if (x - 4 == y - 4 || (11 - x) == y - 4 || x - 4 == y - 5 || (11 - x) == y - 5)
						if (y >= 4 && y <= 11)
							c = tinted(0xe8ecf4, 0);
				}
				break;
			}
			case BL_GATE: {                              /* upright wooden bars with gaps and two iron bands */
				bool bar = x % 4 <= 1, band = y == 2 || y == 3 || y == 12 || y == 13, frame = y == 0 || y == TEX - 1;
				if (band)
					c = tinted(0x4a4c52, tn(x, y, 60, 6));
				else if (bar || frame)
					c = tinted(0x7c5a30, tn(x / 4, y, 61, 8) - (x % 4 == 1 ? 14 : 0));
				else
					c = argb(0, 0, 0, 0);
				break;
			}
			case BL_REDKEEP: case BL_REDFLAG: {          /* dark stone; on the flag's sides a red rim and a long red banner with a black sun */
				int row = y / 4;
				bool mortar = (y % 4 == 3) || ((x + (row & 1) * 4) % 8 == 7);
				c = mortar ? tinted(0x26242a, tn(x, y, 72, 4)) : tinted(0x4c4854, tn((x + (row & 1) * 4) / 8, row, 73, 9) + tn(x, y, 74, 5));
				if (blk == BL_REDKEEP)
					break;
				if (x == 0 || x == 15 || y == 0 || y == 15)
					c = tinted(0x7a1818, tn(x, y, 75, 8));
				if (fk == BL_FACE_SIDE && x >= 4 && x <= 11 && y >= 1 && y <= 13 && !(y >= 12 && x >= 7 && x <= 8)) {
					int d2 = (2 * x - 15) * (2 * x - 15) + (2 * y - 13) * (2 * y - 13);
					c = d2 <= 20 ? tinted(0x140c0c, 0) : tinted(0xd02828, tn(x, y, 76, 9));
				} else if (fk == BL_FACE_TOP && x >= 5 && x <= 10 && y >= 5 && y <= 10) {
					c = tinted(0xd02828, tn(x, y, 77, 9));
				}
				break;
			}
			case BL_TOWERPOST: {                         /* dressed stone with a yellow flag on the sides and a ringed plate on top */
				int row = y / 4;
				bool mortar = (y % 4 == 3) || ((x + (row & 1) * 4) % 8 == 7);
				c = mortar ? tinted(0x77736a, tn(x, y, 62, 4)) : tinted(0x9a958a, tn((x + (row & 1) * 4) / 8, row, 63, 10) + tn(x, y, 64, 5));
				if (fk == BL_FACE_SIDE) {
					if (x == 4 && y >= 2 && y <= 13)
						c = tinted(0x4a3320, 0);
					if (x >= 5 && x <= 12 && y >= 2 && y <= 8 && !(x >= 11 && y >= 4 && y <= 6))
						c = tinted(0xe0b030, tn(x, y, 65, 8));
				} else if (fk == BL_FACE_TOP) {
					int d2 = (2 * x - 15) * (2 * x - 15) + (2 * y - 15) * (2 * y - 15);
					if (d2 > 60 && d2 < 150)
						c = tinted(0xe0b030, tn(x, y, 66, 8));
				}
				break;
			}
			case BL_GOLD: {                              /* stone with fat nuggets of gold */
				c = tinted(0x7f7f86, tn(x / 2, y / 2, 80, 12) + tn(x, y, 81, 6));
				uint32_t k = hxy(x / 3 + fk * 7, y / 3, 82) % 5U;
				if (k < 2 && !((x % 3 == 2) && (y % 3 == 2)))
					c = tinted((x % 3 == 0 && y % 3 == 0) ? 0xfff2a8 : k ? 0xf2c230 : 0xd89c18, tn(x, y, 83, 8));
				break;
			}
			case BL_SMITHY: {                            /* dark timber with an anvil and sparks; a glowing hearth on top */
				c = tinted(0x4a3a30, tn(x / 4, y, 84, 8) + tn(x, y, 85, 4));
				if (fk == BL_FACE_SIDE) {
					if ((y >= 6 && y <= 7 && x >= 3 && x <= 12) || (y >= 8 && y <= 10 && x >= 6 && x <= 9) || (y >= 11 && y <= 12 && x >= 4 && x <= 11))
						c = tinted(0xc8ced8, tn(x, y, 86, 8));
					else if (y < 5 && hxy(x, y, 87) % 9 == 0)
						c = tinted(0xffa828, 0);
				} else if (fk == BL_FACE_TOP && x >= 4 && x <= 11 && y >= 4 && y <= 11) {
					c = tinted(0xe85818, tn(x, y, 88, 30));
				}
				break;
			}
			case BL_MAGE: {                              /* a violet crystal in a dark frame */
				int dx = x * 2 - 15, dy = y * 2 - 15, m = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
				c = tinted(0x2a2040, tn(x, y, 89, 6));
				if (m <= 16)
					c = tinted(m <= 6 ? 0xf0d8ff : (x + y) % 3 ? 0xa050e8 : 0x7838c0, tn(x, y, 90, 10));
				break;
			}
			case BL_INFIRMARY: {                         /* white boards with a red cross */
				c = tinted(0xe8e4dc, tn(x / 4, y, 91, 6) - (y % 4 == 3 ? 20 : 0));
				if ((x >= 6 && x <= 9 && y >= 3 && y <= 12) || (y >= 6 && y <= 9 && x >= 3 && x <= 12))
					c = tinted(0xd02424, tn(x, y, 92, 6));
				break;
			}
			case BL_MINE: {                              /* a timber frame with a pick and a nugget */
				c = tinted(0x8a6a40, tn(x / 4, y, 93, 8) + tn(x, y, 94, 4));
				if (x <= 1 || x >= TEX - 2 || y <= 1 || y >= TEX - 2)
					c = tinted(0x4a3320, tn(x, y, 95, 6));
				else if ((x == y && x >= 4 && x <= 11))
					c = tinted(0x5a4024, 0);
				else if ((y >= 3 && y <= 4 && x >= 7 && x <= 12) || (x >= 11 && x <= 12 && y >= 3 && y <= 8))
					c = tinted(0xc8ced8, tn(x, y, 96, 8));
				else if (x >= 3 && x <= 6 && y >= 10 && y <= 12)
					c = tinted(0xf2c230, tn(x, y, 97, 10));
				break;
			}
			case BL_COBBLE: {
				int off = ((y / 4) & 1) * 2;
				int sx = (x + off) / 4, sy = y / 4;
				bool seam = ((x + off) % 4 == 0) || (y % 4 == 0);
				int base = 0x6a + (int)(hxy(sx, sy, 28) % 40U);
				c = argb(base + tn(x, y, 29, 4) - (seam ? 28 : 0), base + tn(x, y, 29, 4) - (seam ? 28 : 0), base + tn(x, y, 29, 4) - (seam ? 28 : 0), 255);
				break;
			}
			default:
				break;
			}
			px[y * TEX + x] = c;
		}
}

static void textures_init(void)
{
	if (g_ready)
		return;
	for (int b = 1; b < BL_COUNT; b++)
		for (int f = 0; f < BL_FACE_KINDS; f++) {
			gen_face(b, f, g_tex[b][f]);
			double sum[3] = { 0, 0, 0 };
			int n = 0;
			for (int i = 0; i < TEX * TEX; i++) {
				uint32_t c = g_tex[b][f][i];
				if ((c >> 24) < 40)
					continue;
				sum[0] += (c >> 16) & 255;
				sum[1] += (c >> 8) & 255;
				sum[2] += c & 255;
				n++;
			}
			for (int k = 0; k < 3; k++)
				g_avg[b][f][k] = n ? (float)(sum[k] / n / 255.0) : 0.5f;
		}
	g_ready = true;
}

const uint32_t *bl_texture(int block, int face_kind)
{
	textures_init();
	if (block <= 0 || block >= BL_COUNT || face_kind < 0 || face_kind >= BL_FACE_KINDS)
		return NULL;
	return g_tex[block][face_kind];
}

uint32_t bl_block_color(int block)
{
	textures_init();
	if (block <= 0 || block >= BL_COUNT)
		return 0;
	const float *a = g_avg[block][BL_FACE_SIDE];
	return (uint32_t)(a[0] * 255.0f + 0.5f) << 16 | (uint32_t)(a[1] * 255.0f + 0.5f) << 8 | (uint32_t)(a[2] * 255.0f + 0.5f);
}

/* ---- the sky -------------------------------------------------------------------------------------------------------- */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void bl_sun_dir(float time_h, float out[3])
{
	float a = (time_h - 6.0f) / 12.0f * (float)M_PI;
	float x = cosf(a), y = sinf(a), z = 0.35f;
	float l = sqrtf(x * x + y * y + z * z);
	out[0] = x / l;
	out[1] = y / l;
	out[2] = z / l;
}

static float smoothf(float a, float b, float v)
{
	float t = (v - a) / (b - a);
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	return t * t * (3.0f - 2.0f * t);
}

float bl_daylight(float time_h)
{
	float s[3];
	bl_sun_dir(time_h, s);
	return smoothf(-0.12f, 0.22f, s[1]);
}

/* ---- one frame ------------------------------------------------------------------------------------------------------ */

struct frame {
	int W, D, H;
	const uint8_t *b;
	const int16_t *top;
	const uint8_t *light;     /* the light of torches and lamps per cell */
	float blight[16];         /* the brightness of that light by level */
	const int16_t *cmax;      /* per 8 x 8 columns: the highest block in them; everything above is empty and a ray leaps over it */
	int W8;
	float ox, oy, oz;
	float f[3], r[3], u[3];
	int width, height;
	float maxd;
	int ytop;                 /* every cell from this height up is air */
	float sun[3];
	float day;
	float zen[3], hor[3];
	float tint[3];
	float lightk;             /* the light in a cell that sees the sky */
	float minlight;           /* the least light anywhere (a cave) */
	float wcol[3];            /* the colour of deep water */
	bool target;
	int tx, ty, tz;
	float crack;
	bool ids;
	const struct bl_pets *pets;
	const struct bl_mobs *mobs;
	const struct bl_ghosts *ghosts;
	const struct bl_hero *hero;
	float inv_f;              /* metres a pixel covers at a distance of one metre */
	float petmax;             /* the pets are drawn up to here (beyond it the fog has them) */
	bool start_water;
	const float *nx, *ny;
};

static inline float mixf(float a, float b, float t)
{
	return a + (b - a) * t;
}

static void sky_color(const struct frame *F, float dx, float dy, float dz, float out[3])
{
	float h = dy < 0 ? 0 : dy;
	float k = sqrtf(h);
	for (int i = 0; i < 3; i++)
		out[i] = mixf(F->hor[i], F->zen[i], k);
	if (dy < 0) {
		float dk = 1.0f + dy * 0.6f;
		for (int i = 0; i < 3; i++)
			out[i] *= dk < 0.4f ? 0.4f : dk;
	}
	float d = dx * F->sun[0] + dy * F->sun[1] + dz * F->sun[2];
	if (d > 0.93f && F->sun[1] > -0.25f) {
		float g = (d - 0.93f) / 0.07f;
		float glow = g * g * g * 0.45f;
		out[0] += glow;
		out[1] += glow * 0.85f;
		out[2] += glow * 0.55f;
		if (d > 0.9992f) {
			out[0] = 1.0f;
			out[1] = 0.96f;
			out[2] = 0.8f;
		}
	}
	float night = 1.0f - F->day;
	if (night > 0.04f && dy > -0.02f) {
		if (-d > 0.9993f) {                       /* the moon is opposite the sun */
			out[0] = mixf(out[0], 0.92f, night);
			out[1] = mixf(out[1], 0.94f, night);
			out[2] = mixf(out[2], 1.0f, night);
		} else {
			int cx = (int)floorf(dx * 190.0f), cy = (int)floorf(dy * 190.0f), cz = (int)floorf(dz * 190.0f);
			uint32_t hh = hmix((uint32_t)cx * 0x9e3779b1U ^ (uint32_t)cy * 0x85ebca77U ^ (uint32_t)cz * 0xc2b2ae3dU);
			if ((hh & 1023U) < 5U) {
				float s = (0.4f + (float)((hh >> 10) & 255U) / 255.0f * 0.6f) * night * (1.0f - F->day);
				out[0] += s;
				out[1] += s;
				out[2] += s * 1.05f;
			}
		}
	}
}

static const float k_warm[3] = { 1.0f, 0.80f, 0.52f };            /* the colour of torch light */

/* The ray against the box of a torch in the cell (ix, iy, iz): 14 / 16 wide ... the stick is 2 / 16 thick and 10 / 16 high, in the middle of the cell's
 * floor. Returns the face of the box the ray comes through (BL_PX ...) with the distance in *t, or -1 for a miss (also when the eye is inside the box). */
static int torch_hit(const struct frame *F, int ix, int iy, int iz, float dx, float dy, float dz, float *t)
{
	const float o[3] = { F->ox, F->oy, F->oz }, d[3] = { dx, dy, dz };
	const float lo[3] = { (float)ix + 0.4375f, (float)iy, (float)iz + 0.4375f }, hi[3] = { (float)ix + 0.5625f, (float)iy + 0.625f, (float)iz + 0.5625f };
	float tn = -1e30f, tf = 1e30f;
	int axis = 0;
	for (int a = 0; a < 3; a++) {
		if (fabsf(d[a]) < 1e-9f) {
			if (o[a] < lo[a] || o[a] > hi[a])
				return -1;
			continue;
		}
		float t0 = (lo[a] - o[a]) / d[a], t1 = (hi[a] - o[a]) / d[a];
		if (t0 > t1) {
			float x = t0;
			t0 = t1;
			t1 = x;
		}
		if (t0 > tn) {
			tn = t0;
			axis = a;
		}
		if (t1 < tf)
			tf = t1;
	}
	if (tn > tf || tn < 0.0f)
		return -1;
	*t = tn;
	return axis == 0 ? (dx > 0 ? BL_NX : BL_PX) : axis == 1 ? (dy > 0 ? BL_NY : BL_PY) : (dz > 0 ? BL_NZ : BL_PZ);
}

/* 0 for none, 1 for Tom, 2 for Filip, 3 for a goblin: which of them a ray meets nearer than `tmax` (the test hook) */
static int pets_probe(const struct frame *F, float dx, float dy, float dz, float tmax)
{
	float o[3] = { F->ox, F->oy, F->oz }, d[3] = { dx, dy, dz };
	int pk = 0;
	float tp = tmax;
	if (F->pets) {
		pk = bl_pets_probe(F->pets, o, d, tmax < F->petmax ? tmax : F->petmax, F->inv_f);
		(void)tp;
	}
	if (F->mobs && !pk) {
		struct bl_mob_hit mh;
		if (bl_mobs_ray(F->mobs, o, d, tmax, &mh))
			pk = 3;
	}
	return pk;
}

static const float k_face_shade[6] = { 0.62f, 0.62f, 1.0f, 0.5f, 0.8f, 0.8f };

static inline void slab(const struct frame *F, float *acc, float *trans, float dist)
{
	float a = 1.0f - expf(-0.1f * dist);
	for (int i = 0; i < 3; i++)
		acc[i] += *trans * F->wcol[i] * a;
	*trans *= 1.0f - a;
}

/* a creature at the distance of its middle: laid over what is nearer, inside the water's volume if the ray is in it (the volume is cut there) */
static inline void pet_insert(const struct frame *F, float *acc, float *trans, bool in_water, float *wt0, const struct bl_pet_event *e)
{
	if (in_water && e->tp > *wt0) {
		slab(F, acc, trans, e->tp - *wt0);
		*wt0 = e->tp;
	}
	for (int i = 0; i < 3; i++)
		acc[i] += *trans * e->c[i];
	*trans *= 1.0f - e->a;
}


/* the event just written at ev[n] goes to its place by distance; the new count */
static inline int event_sort(struct bl_pet_event *ev, int n)
{
	for (int k = n; k > 0 && ev[k - 1].tp > ev[k].tp; k--) {
		struct bl_pet_event sw = ev[k - 1];
		ev[k - 1] = ev[k];
		ev[k] = sw;
	}
	return n + 1;
}

static uint32_t trace(const struct frame *F, float dx, float dy, float dz)
{
	enum { END_SKY, END_FAR, END_EDGE };
	if (F->hero && F->hero->first && !F->ids) {       /* the player's own arm and what it holds are in front of everything */
		float ad[3] = { dx, dy, dz }, rgb[3];
		if (bl_hero_arm_ray(F->hero, ad, rgb))
			return 0xff000000u | (uint32_t)clamp8((int)(rgb[0] * 255.0f + 0.5f)) << 16 | (uint32_t)clamp8((int)(rgb[1] * 255.0f + 0.5f)) << 8 |
			       (uint32_t)clamp8((int)(rgb[2] * 255.0f + 0.5f));
	}
	float acc[3] = { 0, 0, 0 }, trans = 1.0f;
	float px = F->ox, py = F->oy, pz = F->oz;
	float t = 0.0f;
	int end = END_FAR;
	bool in_water = F->start_water;
	float wt0 = 0.0f;
	const float maxd = F->maxd;
	struct bl_pet_event pev[5];                       /* Tom, Filip, a mob, the player's body and a planned block along this ray: laid over what is nearer when the ray gets to their distance */
	int npev = 0, ipev = 0;
	if (F->pets && !F->ids) {
		float po[3] = { F->ox, F->oy, F->oz }, pd[3] = { dx, dy, dz };
		npev = bl_pets_events(F->pets, po, pd, F->petmax, F->inv_f, pev);
	}
	if (F->mobs && !F->ids) {
		float po[3] = { F->ox, F->oy, F->oz }, pd[3] = { dx, dy, dz };
		struct bl_mob_hit mh;
		if (bl_mobs_ray(F->mobs, po, pd, F->maxd, &mh)) {
			float fog = mh.t > 0.6f * F->maxd ? (mh.t - 0.6f * F->maxd) / (0.4f * F->maxd) : 0.0f;
			if (fog > 1.0f)
				fog = 1.0f;
			for (int i = 0; i < 3; i++)
				pev[npev].c[i] = mh.rgb[i] * (1.0f - fog) + F->hor[i] * fog;
			pev[npev].a = 1.0f;
			pev[npev].tp = mh.t;
			npev = event_sort(pev, npev);
		}
	}
	if (F->hero && !F->ids) {
		float po[3] = { F->ox, F->oy, F->oz }, pd[3] = { dx, dy, dz }, rgb[3], th;
		if (bl_hero_ray(F->hero, po, pd, F->maxd, rgb, &th)) {
			float fog = th > 0.6f * F->maxd ? (th - 0.6f * F->maxd) / (0.4f * F->maxd) : 0.0f;
			if (fog > 1.0f)
				fog = 1.0f;
			for (int i = 0; i < 3; i++)
				pev[npev].c[i] = rgb[i] * (1.0f - fog) + F->hor[i] * fog;
			pev[npev].a = 1.0f;
			pev[npev].tp = th;
			npev = event_sort(pev, npev);
		}
	}
	if (F->ghosts && !F->ids) {
		float po[3] = { F->ox, F->oy, F->oz }, pd[3] = { dx, dy, dz }, rgb[3], al, tg;
		if (bl_ghosts_ray(F->ghosts, po, pd, F->maxd, rgb, &al, &tg)) {
			for (int i = 0; i < 3; i++)
				pev[npev].c[i] = rgb[i] * al;             /* (an event's colour is already multiplied by how much it covers) */
			pev[npev].a = al;
			pev[npev].tp = tg;
			npev = event_sort(pev, npev);
		}
	}
	const int W = F->W, D = F->D, H = F->H, ytop = F->ytop;

	int ix, iy, iz;
	bool clipped = false;
	if (py >= (float)ytop) {
		if (dy >= 0.0f) {
			end = END_SKY;
			goto finish;
		}
		float t0 = (py - (float)ytop) / -dy;
		if (t0 >= maxd)
			goto finish;
		px += dx * t0;
		pz += dz * t0;
		py = (float)ytop;
		t = t0;
		iy = ytop;                /* the empty layer on top: the first step goes down into the top layer, through its top face */
		clipped = true;
	} else {
		iy = (int)floorf(py);
	}
	ix = (int)floorf(px);
	iz = (int)floorf(pz);
	if ((unsigned)ix >= (unsigned)W || (unsigned)iz >= (unsigned)D || iy < 0) {
		end = END_EDGE;
		goto finish;
	}

	{
		const int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
		const float big = 1e30f;
		const float ix_ = dx != 0 ? 1.0f / dx : 0.0f, iy_ = dy != 0 ? 1.0f / dy : 0.0f, iz_ = dz != 0 ? 1.0f / dz : 0.0f;   /* 1 / the direction */
		const float tdx = dx != 0 ? fabsf(ix_) : big, tdy = dy != 0 ? fabsf(iy_) : big, tdz = dz != 0 ? fabsf(iz_) : big;
		const float ox = F->ox, oy = F->oy, oz = F->oz;
		/* the time at which the ray reaches the next plane of the grid on each axis, from the eye (not from where the loop started: no drift) */
		float tmx = dx != 0 ? ((float)(dx > 0 ? ix + 1 : ix) - ox) * ix_ : big;
		float tmy = dy != 0 ? ((float)(dy > 0 ? iy + 1 : iy) - oy) * iy_ : big;
		float tmz = dz != 0 ? ((float)(dz > 0 ? iz + 1 : iz) - oz) * iz_ : big;
		size_t idx = ((size_t)iz * (size_t)W + (size_t)ix) * (size_t)H + (size_t)iy;
		int pb = clipped ? BL_AIR : F->b[idx];           /* the block the ray comes out of (the layer above the world is air) */

		int lface = -1;                                  /* >= 0 after a leap: the face the ray entered the cell it landed in by */
		for (;;) {
			int face;
			if (lface >= 0) {
				face = lface;                            /* the cell the leap landed in has not been looked at yet */
				lface = -1;
			} else if (tmx < tmy && tmx < tmz) {
				t = tmx;
				tmx += tdx;
				ix += sx;
				idx += (size_t)(ptrdiff_t)sx * (size_t)H;
				if ((unsigned)ix >= (unsigned)W) {
					end = END_EDGE;
					break;
				}
				face = sx > 0 ? BL_NX : BL_PX;
			} else if (tmy < tmz) {
				t = tmy;
				tmy += tdy;
				iy += sy;
				idx += (size_t)(ptrdiff_t)sy;
				if (iy >= ytop) {
					end = END_SKY;
					break;
				}
				if (iy < 0) {
					end = END_EDGE;
					break;
				}
				face = sy > 0 ? BL_NY : BL_PY;
			} else {
				t = tmz;
				tmz += tdz;
				iz += sz;
				idx += (size_t)(ptrdiff_t)sz * (size_t)W * (size_t)H;
				if ((unsigned)iz >= (unsigned)D) {
					end = END_EDGE;
					break;
				}
				face = sz > 0 ? BL_NZ : BL_PZ;
			}
			if (t >= maxd) {
				t = maxd;
				end = END_FAR;
				break;
			}
			while (ipev < npev && pev[ipev].tp <= t) {
				pet_insert(F, acc, &trans, in_water, &wt0, &pev[ipev]);
				ipev++;
			}
			if (trans <= 0.0f)                                 /* something solid, a goblin, is in front of everything that follows */
				goto finish;
			const int blk = F->b[idx];
			if (blk == BL_AIR) {
				if (in_water) {
					slab(F, acc, &trans, t - wt0);
					in_water = false;
				}
				pb = blk;
				/* above everything in this 8 x 8 stretch of columns: leave it in one leap */
				const int cm = F->cmax[(iz >> 3) * F->W8 + (ix >> 3)];
				if (iy > cm) {
					const int bx = (ix >> 3) << 3, bz = (iz >> 3) << 3;
					const float tex_ = dx > 0 ? ((float)(bx + 8) - ox) * ix_ : dx < 0 ? ((float)bx - ox) * ix_ : big;
					const float tez_ = dz > 0 ? ((float)(bz + 8) - oz) * iz_ : dz < 0 ? ((float)bz - oz) * iz_ : big;
					const float tey_ = dy < 0 ? ((float)(cm + 1) - oy) * iy_ : big;
					float te = tex_ < tez_ ? tex_ : tez_;
					if (tey_ < te)
						te = tey_;
					if (te > t && te < big) {
						if (te >= maxd) {
							t = maxd;
							end = END_FAR;
							break;
						}
						const float qx = ox + dx * te, qy = oy + dy * te, qz = oz + dz * te;
						if (te == tey_) {
							iy = cm;
							ix = (int)floorf(qx);
							iz = (int)floorf(qz);
							lface = BL_PY;
						} else if (te == tex_) {
							ix = dx > 0 ? bx + 8 : bx - 1;
							iy = (int)floorf(qy);
							iz = (int)floorf(qz);
							lface = dx > 0 ? BL_NX : BL_PX;
						} else {
							iz = dz > 0 ? bz + 8 : bz - 1;
							ix = (int)floorf(qx);
							iy = (int)floorf(qy);
							lface = dz > 0 ? BL_NZ : BL_PZ;
						}
						t = te;
						if ((unsigned)ix >= (unsigned)W || (unsigned)iz >= (unsigned)D || iy < 0) {
							end = END_EDGE;
							break;
						}
						if (iy >= ytop) {
							end = END_SKY;
							break;
						}
						tmx = dx != 0 ? ((float)(dx > 0 ? ix + 1 : ix) - ox) * ix_ : big;
						tmy = dy != 0 ? ((float)(dy > 0 ? iy + 1 : iy) - oy) * iy_ : big;
						tmz = dz != 0 ? ((float)(dz > 0 ? iz + 1 : iz) - oz) * iz_ : big;
						idx = ((size_t)iz * (size_t)W + (size_t)ix) * (size_t)H + (size_t)iy;
					}
				}
				continue;
			}
			if (F->ids) {
				if (bl_water(blk))
					continue;                            /* the test hook sees through water like `bl_pick` */
				if (F->pets || F->mobs) {
					int pk = pets_probe(F, dx, dy, dz, t);
					if (pk)
						return pk == 1 ? 0xff0000feu : pk == 2 ? 0xff0000fdu : 0xff0000fcu;
				}
				return 0xff000000u | (uint32_t)blk << 8 | (uint32_t)face;
			}
			if (bl_water(blk) && bl_water(pb)) {
				continue;                                /* inside the water: the volume is dealt with at its ends */
			}
			if (!bl_water(blk) && in_water) {
				slab(F, acc, &trans, t - wt0);
				in_water = false;
			}
			if (blk == pb && blk == BL_GLASS) {
				continue;                                /* the face between two panes of glass */
			}

			if (blk == BL_TORCH) {
				/* a thin stick with a flame in the middle of the cell's floor: the ray meets that little box or goes on through the cell */
				float th;
				int tf = torch_hit(F, ix, iy, iz, dx, dy, dz, &th);
				if (F->target && ix == F->tx && iy == F->ty && iz == F->tz) {      /* the aimed cell gets its rim even where the stick is not */
					float qx = F->ox + dx * t - (float)ix, qy = F->oy + dy * t - (float)iy, qz = F->oz + dz * t - (float)iz, e = 0.03f;
					float a1 = face <= BL_NX ? qz : qx, a2 = face <= BL_NX ? qy : face <= BL_NY ? qz : qy;       /* the two coordinates along the face the ray came through */
					if (a1 < e || a1 > 1.0f - e || a2 < e || a2 > 1.0f - e)
						trans *= 0.45f;
				}
				pb = blk;
				if (tf < 0 || th >= maxd)
					continue;
				float hy = F->oy + dy * th - (float)iy;
				float col[3];
				if (tf == BL_PY || hy > 0.4375f) {                                    /* the flame: bright whatever the light */
					col[0] = 1.0f;
					col[1] = hy > 0.55f ? 0.9f : 0.7f;
					col[2] = 0.3f;
				} else {
					float k = k_face_shade[tf] * 0.55f + 0.35f;
					col[0] = 0.48f * k;
					col[1] = 0.33f * k;
					col[2] = 0.16f * k;
				}
				float fg = (th - 0.6f * maxd) / (0.4f * maxd);
				if (fg > 0.0f) {
					fg = fg > 1.0f ? 1.0f : fg;
					fg *= fg;
					for (int i = 0; i < 3; i++)
						col[i] = mixf(col[i], F->hor[i], fg);
				}
				for (int i = 0; i < 3; i++)
					acc[i] += trans * col[i];
				trans = 0.0f;
				t = th;
				break;
			}

			/* the cell the ray came from: its light */
			const int pcx = ix + bl_face_n[face][0], pcy = iy + bl_face_n[face][1], pcz = iz + bl_face_n[face][2];
			float lit = 1.0f;
			if ((unsigned)pcx < (unsigned)W && (unsigned)pcz < (unsigned)D) {
				int tp = F->top[pcz * W + pcx];
				if (pcy <= tp) {
					lit = 1.0f - 0.10f * (float)(tp - pcy);
					if (lit < 0.0f)
						lit = 0.0f;
				}
			}
			float L = lit * F->lightk;
			if (L < F->minlight)
				L = F->minlight;                     /* a cave is dim, not black */
			float bt = 0.0f;                         /* the light of torches and lamps in the cell in front of the face */
			if ((unsigned)pcx < (unsigned)W && (unsigned)pcz < (unsigned)D && (unsigned)pcy < (unsigned)H)
				bt = F->blight[F->light[((size_t)pcz * (size_t)W + (size_t)pcx) * (size_t)H + (size_t)pcy]];
			if (blk == BL_LAMP)
				bt = 1.0f;                           /* a lamp shines on itself */
			const float sh = k_face_shade[face];
			float mult[3];
			for (int i = 0; i < 3; i++) {            /* sky light and torch light do not add: the brighter one counts, torch light is warm */
				float sk = L * F->tint[i], tl = bt * k_warm[i];
				mult[i] = sk > tl ? sk : tl;
			}

			/* where on the face */
			float hpx = F->ox + dx * t, hpy = F->oy + dy * t, hpz = F->oz + dz * t;
			float u, v;
			if (face <= BL_NX) {
				u = hpz - (float)iz;
				v = 1.0f - (hpy - (float)iy);
			} else if (face <= BL_NY) {
				u = hpx - (float)ix;
				v = hpz - (float)iz;
			} else {
				u = hpx - (float)ix;
				v = 1.0f - (hpy - (float)iy);
			}
			int tu = (int)(u * (float)TEX), tv = (int)(v * (float)TEX);
			tu = tu < 0 ? 0 : tu > TEX - 1 ? TEX - 1 : tu;
			tv = tv < 0 ? 0 : tv > TEX - 1 ? TEX - 1 : tv;
			const int fk = face == BL_PY ? BL_FACE_TOP : face == BL_NY ? BL_FACE_BOTTOM : BL_FACE_SIDE;
			const uint32_t tx = g_tex[blk][fk][tv * TEX + tu];
			const int ta = (int)(tx >> 24);
			if (ta == 0) {
				pb = blk;
				continue;                                /* the middle of a pane of glass */
			}
			float col[3] = { (float)((tx >> 16) & 255) * (1.0f / 255.0f), (float)((tx >> 8) & 255) * (1.0f / 255.0f), (float)(tx & 255) * (1.0f / 255.0f) };
			if (t > 24.0f) {                             /* far away a texture goes to its average */
				float m = (t - 24.0f) * (1.0f / 24.0f);
				if (m > 1.0f)
					m = 1.0f;
				for (int i = 0; i < 3; i++)
					col[i] = mixf(col[i], g_avg[blk][fk][i], m);
			}
			for (int i = 0; i < 3; i++)
				col[i] *= sh * mult[i];
			if (F->hero && face == BL_PY) {                  /* the player's shadow */
				float hs = bl_hero_floor_shade(F->hero, hpx, (float)(iy + 1), hpz);
				if (hs < 1.0f)
					for (int i = 0; i < 3; i++)
						col[i] *= hs;
			}
			if (F->pets && face == BL_PY) {                  /* the shadow of Tom and of Filip on the top of a block at their feet */
				float fs = bl_pets_floor_shade(F->pets, hpx, (float)(iy + 1), hpz);
				if (fs < 1.0f)
					for (int i = 0; i < 3; i++)
						col[i] *= fs;
			}
			if (F->target && ix == F->tx && iy == F->ty && iz == F->tz) {
				const float e = 0.03f;
				if (u < e || u > 1.0f - e || v < e || v > 1.0f - e)
					for (int i = 0; i < 3; i++)
						col[i] *= 0.3f;
				if (F->crack > 0.0f) {                       /* a block that is being broken: cracks, more of them as it goes (dark texels from a fixed pattern) */
					int cu = (int)(u * 16.0f), cv = (int)(v * 16.0f);
					if ((int)(hxy(cu, cv, 77) % 100U) < (int)(F->crack * 62.0f))
						for (int i = 0; i < 3; i++)
							col[i] *= 0.42f;
				}
			}
			float fogt = (t - 0.6f * maxd) / (0.4f * maxd);
			if (fogt > 0.0f) {
				fogt = fogt > 1.0f ? 1.0f : fogt;
				fogt *= fogt;
				for (int i = 0; i < 3; i++)
					col[i] = mixf(col[i], F->hor[i], fogt);
			}
			if (bl_water(blk)) {
				/* the surface of water: a little colour and a glint, then the volume starts */
				float al = (float)ta * (1.0f / 255.0f);
				for (int i = 0; i < 3; i++)
					acc[i] += trans * al * col[i];
				trans *= 1.0f - al;
				in_water = true;
				wt0 = t;
				pb = blk;
				continue;
			}
			if (ta >= 255) {
				for (int i = 0; i < 3; i++)
					acc[i] += trans * col[i];
				trans = 0.0f;
				break;
			}
			float al = (float)ta * (1.0f / 255.0f);
			for (int i = 0; i < 3; i++)
				acc[i] += trans * al * col[i];
			trans *= 1.0f - al;
			pb = blk;
		}
	}

finish:
	if (F->ids) {
		if (F->pets || F->mobs) {
			int pk = pets_probe(F, dx, dy, dz, F->maxd);
			if (pk)
				return pk == 1 ? 0xff0000feu : pk == 2 ? 0xff0000fdu : 0xff0000fcu;
		}
		return 0xff0000ffu;
	}
	if (trans > 0.0f) {
		while (ipev < npev) {                        /* the ray got to the end without meeting an opaque block: what is left is in front of the sky or the fog */
			pet_insert(F, acc, &trans, in_water, &wt0, &pev[ipev]);
			ipev++;
		}
		if (in_water)
			slab(F, acc, &trans, t - wt0);
		if (trans > 0.0f) {
			float bg[3];
			if (end != END_EDGE)
				sky_color(F, dx, dy, dz, bg);            /* far away there is only sky; it joins the fog at the horizon */
			else
				memcpy(bg, F->hor, sizeof bg);
			for (int i = 0; i < 3; i++)
				acc[i] += trans * bg[i];
		}
	}
	int r = (int)(acc[0] * 255.0f + 0.5f), g = (int)(acc[1] * 255.0f + 0.5f), b = (int)(acc[2] * 255.0f + 0.5f);
	return 0xff000000u | (uint32_t)clamp8(r) << 16 | (uint32_t)clamp8(g) << 8 | (uint32_t)clamp8(b);
}

struct job {
	const struct frame *F;
	uint32_t *dst;
	int stride, y0, ystep;
};

static void render_rows(const struct job *j)
{
	const struct frame *F = j->F;
	for (int y = j->y0; y < F->height; y += j->ystep) {
		uint32_t *row = j->dst + (size_t)y * (size_t)j->stride;
		const float ny = F->ny[y];
		const float bx = F->f[0] + F->u[0] * ny, by = F->f[1] + F->u[1] * ny, bz = F->f[2] + F->u[2] * ny;
		for (int x = 0; x < F->width; x++) {
			const float nx = F->nx[x];
			float dx = bx + F->r[0] * nx, dy = by + F->r[1] * nx, dz = bz + F->r[2] * nx;
			float il = 1.0f / sqrtf(dx * dx + dy * dy + dz * dz);
			row[x] = trace(F, dx * il, dy * il, dz * il);
		}
	}
}

static void *render_thread(void *arg)
{
	render_rows(arg);
	return NULL;
}

void bl_view_init(struct bl_view *v)
{
	memset(v, 0, sizeof *v);
	v->time_h = 12.0f;
	v->dist = 96.0f;
}

void bl_render(const struct bl_world *w, const struct bl_camera *c, const struct bl_view *v,
               uint32_t *dst, int width, int height, int stride, int threads)
{
	if (width <= 0 || height <= 0)
		return;
	textures_init();

	struct frame F;
	memset(&F, 0, sizeof F);
	F.W = w->w;
	F.D = w->d;
	F.H = w->h;
	F.b = w->b;
	F.top = w->top;
	F.light = w->light;
	for (int l = 0; l < 16; l++)
		F.blight[l] = powf((float)l / 15.0f, 0.85f);
	F.ox = c->x;
	F.oy = c->y;
	F.oz = c->z;
	F.W8 = (w->w + 7) / 8;
	const int D8 = (w->d + 7) / 8;
	int16_t *cmax = malloc((size_t)F.W8 * (size_t)D8 * sizeof *cmax);
	if (!cmax)
		return;
	for (int i = 0; i < F.W8 * D8; i++)
		cmax[i] = -1;
	for (int z = 0; z < w->d; z++) {
		int16_t *row = cmax + (z >> 3) * F.W8;
		const int16_t *hi = w->hi + (size_t)z * (size_t)w->w;
		for (int x = 0; x < w->w; x++)
			if (hi[x] > row[x >> 3])
				row[x >> 3] = hi[x];
	}
	F.cmax = cmax;
	F.width = width;
	F.height = height;

	float cp = cosf(c->pitch), sp = sinf(c->pitch), cy = cosf(c->yaw), sy = sinf(c->yaw);
	F.f[0] = cp * sy;
	F.f[1] = sp;
	F.f[2] = cp * cy;
	F.r[0] = cy;
	F.r[1] = 0.0f;
	F.r[2] = -sy;
	F.u[0] = F.f[1] * F.r[2] - F.f[2] * F.r[1];
	F.u[1] = F.f[2] * F.r[0] - F.f[0] * F.r[2];
	F.u[2] = F.f[0] * F.r[1] - F.f[1] * F.r[0];

	float dist = v->dist < 16.0f ? 16.0f : v->dist > 256.0f ? 256.0f : v->dist;
	F.maxd = dist;
	F.ytop = w->maxhi + 1;
	if (F.ytop > w->h)
		F.ytop = w->h;
	if (F.ytop < 1)
		F.ytop = 1;

	bl_sun_dir(v->time_h, F.sun);
	F.day = bl_daylight(v->time_h);
	static const float zen_d[3] = { 0.30f, 0.52f, 0.93f }, hor_d[3] = { 0.72f, 0.84f, 0.97f };
	static const float zen_n[3] = { 0.010f, 0.020f, 0.070f }, hor_n[3] = { 0.040f, 0.060f, 0.130f };
	float dusk = 1.0f - fabsf(F.sun[1]) / 0.28f;
	dusk = dusk < 0 ? 0 : dusk;
	for (int i = 0; i < 3; i++) {
		F.zen[i] = mixf(zen_n[i], zen_d[i], F.day);
		F.hor[i] = mixf(hor_n[i], hor_d[i], F.day);
	}
	F.hor[0] = mixf(F.hor[0], 1.0f, dusk * 0.45f);
	F.hor[1] = mixf(F.hor[1], 0.55f, dusk * 0.4f);
	F.hor[2] = mixf(F.hor[2], 0.3f, dusk * 0.4f);
	F.tint[0] = mixf(0.62f, 1.0f, F.day);
	F.tint[1] = mixf(0.74f, 1.0f, F.day);
	F.tint[2] = 1.0f;
	F.lightk = 0.22f + 0.78f * F.day;
	F.minlight = 0.20f + 0.12f * F.day;
	F.wcol[0] = 0.06f * (0.3f + 0.7f * F.lightk);
	F.wcol[1] = 0.25f * (0.3f + 0.7f * F.lightk);
	F.wcol[2] = 0.46f * (0.3f + 0.7f * F.lightk);
	F.target = v->target;
	F.tx = v->tx;
	F.ty = v->ty;
	F.tz = v->tz;
	F.crack = v->crack;
	F.ids = v->ids;
	F.pets = v->pets;
	F.mobs = v->mobs;
	F.ghosts = v->ghosts;
	F.hero = v->hero;
	F.petmax = 0.7f * F.maxd;

	int ex = (int)floorf(c->x), ey = (int)floorf(c->y), ez = (int)floorf(c->z);
	F.start_water = bl_water(bl_get(w, ex, ey, ez));
	if (F.start_water && F.maxd > 48.0f)
		F.maxd = 48.0f;

	float fov = c->fov < 0.2f ? 0.2f : c->fov > 2.8f ? 2.8f : c->fov;
	float th = tanf(fov * 0.5f), aspect = (float)width / (float)height;
	F.inv_f = 2.0f * th / (float)height;
	float *nx = malloc(((size_t)width + (size_t)height) * sizeof *nx);
	if (!nx) {
		free(cmax);
		return;
	}
	float *ny = nx + width;
	for (int x = 0; x < width; x++)
		nx[x] = (((float)x + 0.5f) / (float)width * 2.0f - 1.0f) * aspect * th;
	for (int y = 0; y < height; y++)
		ny[y] = (1.0f - ((float)y + 0.5f) / (float)height * 2.0f) * th;
	F.nx = nx;
	F.ny = ny;

	if (threads < 1)
		threads = 1;
	if (threads > 8)
		threads = 8;
	if (threads > height)
		threads = height;
	struct job jobs[8];
	pthread_t th_[8];
	bool started[8] = { false };
	for (int i = 0; i < threads; i++)
		jobs[i] = (struct job){ &F, dst, stride, i, threads };
	for (int i = 1; i < threads; i++)
		started[i] = pthread_create(&th_[i], NULL, render_thread, &jobs[i]) == 0;
	render_rows(&jobs[0]);
	for (int i = 1; i < threads; i++) {
		if (started[i])
			pthread_join(th_[i], NULL);
		else
			render_rows(&jobs[i]);
	}
	free(nx);
	free(cmax);
}
