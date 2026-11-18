/* blocksview.h - the picture of a block world: a ray for every pixel, stepped through the grid (Amanatides & Woo).
 *
 * No window and no GPU: it fills a buffer of 0xFFRRGGBB pixels, on up to 8 threads (each takes every n-th row, so a busy
 * part of the picture is shared). Reads the world only, so the world must not be edited while it renders.
 *
 * What it draws: the 16 x 16 textures of every block face (made in code, no files: `bl_texture`), shaded by the face (top
 * bright, bottom dark), by the sky above the cell in front of it (`bl_world.top`: caves and the ground under trees are dark)
 * and by the time of day, fog towards the horizon, glass with a see-through middle, water as a surface and a blue volume
 * that gets thicker with the distance travelled in it (so the lake floor fades and the camera can be under water), a sky
 * with a sun, a moon and stars that follow `time_h`, and an outline round the block the player aims at. Far away a texture
 * fades to its average colour, so the ground does not sparkle.
 */
#ifndef MELLAGUI_BLOCKSVIEW_H
#define MELLAGUI_BLOCKSVIEW_H

#include "blocks.h"

struct bl_camera {
	float x, y, z;          /* the eye */
	float yaw, pitch;       /* as in blocks.h */
	float fov;              /* the whole vertical angle, radians (about 1.2 = 70 degrees) */
};

struct bl_view {
	float time_h;           /* the hour of the day, 0 .. 24: 6 sunrise, 12 noon, 18 sunset */
	float dist;             /* how far to look, in blocks (16 .. 256) */
	bool target;            /* outline the block (tx, ty, tz) */
	int tx, ty, tz;
	bool ids;               /* a hook for the tests: a pixel is 0xFF000000 | block << 8 | face of the first block a ray could aim at
	                         * (what `bl_pick` finds), 0xFF0000FF for none; no shading, no sky */
};

void bl_view_init(struct bl_view *v);                 /* noon, 96 blocks, no target */

/* Fill `dst` (width x height pixels, `stride` pixels a row) with the picture. threads 1 .. 8 (1 = this thread only). */
void bl_render(const struct bl_world *w, const struct bl_camera *c, const struct bl_view *v,
               uint32_t *dst, int width, int height, int stride, int threads);

void bl_sun_dir(float time_h, float out[3]);          /* the unit vector towards the sun (the moon is the opposite) */
float bl_daylight(float time_h);                      /* 0 at night .. 1 by day, smooth over dusk and dawn */

/* The 16 x 16 ARGB picture a block shows on a kind of face (BL_FACE_TOP / SIDE / BOTTOM); alpha 0 = see through. */
const uint32_t *bl_texture(int block, int face_kind);
uint32_t bl_block_color(int block);                   /* 0xRRGGBB, the average of its side */

#endif
