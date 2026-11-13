/* matrixcat: the cat on the floor of the Matrix room. See matrixcat.h. */
#pragma GCC optimize("O3", "fast-math")
#include "matrixcat.h"

#include <math.h>
#include <string.h>

#define CELL 0.032f                     /* one letter of the fur, in the cat's own metres */
#define SEG_S 6.0f                      /* the head's script: one action every 6 seconds (a little late, a little different) */

enum { P_BODY, P_BELLY, P_PAW, P_TAIL, P_SKULL, P_MUZZLE, P_EAR, P_NOSE };

static uint32_t h32(uint32_t a)
{
    a ^= a >> 16;
    a *= 0x7feb352du;
    a ^= a >> 15;
    a *= 0x846ca68bu;
    a ^= a >> 16;
    return a;
}

static float frand(uint32_t a) { return (float)(h32(a) & 0xffffu) / 65535.0f; }
static float sat(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static float sstep(float a, float b, float v) { float t = sat((v - a) / (b - a)); return t * t * (3.0f - 2.0f * t); }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void rot_y(float a, float m[9]) { float c = cosf(a), s = sinf(a); float r[9] = { c, 0, s, 0, 1, 0, -s, 0, c }; memcpy(m, r, sizeof(r)); }
static void rot_x(float a, float m[9]) { float c = cosf(a), s = sinf(a); float r[9] = { 1, 0, 0, 0, c, -s, 0, s, c }; memcpy(m, r, sizeof(r)); }
static void rot_z(float a, float m[9]) { float c = cosf(a), s = sinf(a); float r[9] = { c, -s, 0, s, c, 0, 0, 0, 1 }; memcpy(m, r, sizeof(r)); }

static void mat_mul(const float a[9], const float b[9], float out[9])
{
    float r[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    memcpy(out, r, sizeof(r));
}

static void add(struct mcat *c, int frame, int part, float cx, float cy, float cz, float rx, float ry, float rz)
{
    if (c->n >= MCAT_PRIMS)
        return;
    struct mcat_prim *p = &c->prim[c->n++];
    p->c[0] = cx; p->c[1] = cy; p->c[2] = cz;
    p->r[0] = rx; p->r[1] = ry; p->r[2] = rz;
    p->ir[0] = 1.0f / rx; p->ir[1] = 1.0f / ry; p->ir[2] = 1.0f / rz;
    p->frame = (uint8_t)frame;
    p->part = (uint8_t)part;
}

/* ---- the head's script ---------------------------------------------------------------------------------------------------- */

struct hang { float yaw, pitch, roll; };

/* Where the head points in segment k: 0 and 1 ahead (a little off), 2 left, 3 right, 4..6 at the camera (`cam_yaw` / `cam_pitch` = the way to it, from the neck), 7 down. */
static struct hang target(uint32_t k, float cam_yaw, float cam_pitch)
{
    uint32_t h = h32(k * 2654435761u + 11u);
    float mag = 0.55f + 0.5f * (float)((h >> 8) % 100) / 100.0f, side = (h & 0x10000u) ? 1.0f : -1.0f;
    struct hang a = { 0, 0, 0 };
    switch (h % 8) {
    case 2: a.yaw = mag; a.pitch = 0.04f; break;
    case 3: a.yaw = -mag; a.pitch = 0.04f; break;
    case 4: case 5: case 6:
        a.yaw = clampf(cam_yaw, -1.25f, 1.25f);
        a.pitch = clampf(cam_pitch, -0.5f, 0.6f);
        a.roll = 0.09f * side;                                           /* (the head tilts, curious) */
        break;
    case 7: a.pitch = 0.5f; a.yaw = 0.15f * side; break;
    default: a.yaw = 0.12f * side * (0.4f + 0.6f * (float)((h >> 4) & 7) / 7.0f); break;
    }
    return a;
}

static struct hang head_at(float t, float cam_yaw, float cam_pitch)
{
    int k = (int)floorf(t / SEG_S);
    float ts = (float)k * SEG_S + frand((uint32_t)k * 7u + 3u) * 1.6f, dur = 0.7f + frand((uint32_t)k * 5u + 1u) * 0.6f;
    float s = sstep(0.0f, 1.0f, (t - ts) / dur);
    struct hang cur = target((uint32_t)k, cam_yaw, cam_pitch), prev = k > 0 ? target((uint32_t)k - 1, cam_yaw, cam_pitch) : (struct hang){ 0, 0, 0 };
    return (struct hang){ mixf(prev.yaw, cur.yaw, s), mixf(prev.pitch, cur.pitch, s), mixf(prev.roll, cur.roll, s) };
}

/* ---- the pose --------------------------------------------------------------------------------------------------------------- */

void mcat_pose(struct mcat *c, float t, const float cam[3], const uint8_t *const *glyph, int nglyph, const float ldir[3], const float lcol[3], float lstr)
{
    c->enabled = true;
    c->t = t;
    c->pos[0] = 0.55f; c->pos[1] = 0.0f; c->pos[2] = 4.85f;
    c->yaw = 3.14159265f + 0.35f;                                        /* sits facing the room, a little towards the left */
    c->scale = 1.25f;
    c->cy = cosf(c->yaw);
    c->sy = sinf(c->yaw);
    c->neck[0] = 0.0f; c->neck[1] = 0.40f; c->neck[2] = 0.04f;
    c->glyph = glyph;
    c->nglyph = nglyph;
    memcpy(c->ldir, ldir, sizeof(c->ldir));
    memcpy(c->lcol, lcol, sizeof(c->lcol));
    c->lstr = lstr;

    float wx = cam[0] - c->pos[0], wy = cam[1] - c->pos[1], wz = cam[2] - c->pos[2], inv = 1.0f / c->scale;       /* the camera as the body sees it */
    float bx = (wx * c->cy - wz * c->sy) * inv, by = wy * inv, bz = (wx * c->sy + wz * c->cy) * inv;
    float vx = bx - c->neck[0], vy = by - (c->neck[1] + 0.10f), vz = bz - c->neck[2];
    float cam_yaw = atan2f(vx, vz), cam_pitch = -atan2f(vy, sqrtf(vx * vx + vz * vz));

    struct hang h = head_at(t, cam_yaw, cam_pitch);
    h.yaw += 0.03f * sinf(t * 0.7f);                                     /* it is never quite still */
    h.pitch += 0.02f * sinf(t * 0.9f + 1.0f);
    c->head_yaw = h.yaw;
    c->head_pitch = h.pitch;
    float ry[9], rx[9], rz[9];
    rot_y(h.yaw, ry);
    rot_x(h.pitch, rx);
    rot_z(h.roll, rz);
    mat_mul(ry, rx, c->rh);
    mat_mul(c->rh, rz, c->rh);

    float tb = fmodf(t + 1.3f, 4.6f);                                    /* a blink every few seconds */
    c->eye_open = tb < 0.18f ? 1.0f - 0.92f * sinf(3.14159265f * tb / 0.18f) : 1.0f;

    /* the body */
    c->n = 0;
    float br = 1.0f + 0.016f * sinf(t * 6.2831853f / 3.4f);               /* breathing */
    add(c, 0, P_BODY, 0.0f, 0.17f, -0.05f, 0.175f, 0.17f, 0.19f);               /* haunch */
    add(c, 0, P_BODY, 0.0f, 0.27f, 0.02f, 0.125f * br, 0.21f, 0.12f * br);      /* torso */
    add(c, 0, P_BELLY, 0.0f, 0.19f, 0.10f, 0.11f, 0.14f, 0.09f);                /* chest */
    add(c, 0, P_BODY, 0.0f, 0.375f, 0.08f, 0.15f, 0.08f, 0.11f);                 /* the ruff */
    for (int s = -1; s <= 1; s += 2) {
        add(c, 0, P_BODY, 0.058f * (float)s, 0.12f, 0.115f, 0.042f, 0.12f, 0.048f);       /* front legs */
        add(c, 0, P_PAW, 0.060f * (float)s, 0.03f, 0.158f, 0.048f, 0.03f, 0.062f);        /* front paws */
        add(c, 0, P_PAW, 0.14f * (float)s, 0.035f, 0.06f, 0.055f, 0.035f, 0.10f);         /* hind feet */
    }
    float sw = 0.045f * sinf(t * 1.3f), sz = 0.03f * sinf(t * 0.9f + 1.0f);         /* the tail is bushy, curls round the paws, and its tip swishes */
    static const float tail[8][5] = {                                                  /* x, y, z, radius, how much the swish moves it */
        { 0.13f, 0.070f, -0.21f, 0.058f, 0.0f }, { 0.20f, 0.065f, -0.18f, 0.060f, 0.0f }, { 0.26f, 0.060f, -0.12f, 0.060f, 0.0f },
        { 0.30f, 0.055f, -0.05f, 0.057f, 0.0f }, { 0.315f, 0.050f, 0.03f, 0.052f, 0.2f }, { 0.30f, 0.047f, 0.10f, 0.046f, 0.5f },
        { 0.27f, 0.044f, 0.16f, 0.040f, 0.8f }, { 0.23f, 0.040f, 0.21f, 0.033f, 1.0f } };
    for (int i = 0; i < 8; i++) {
        c->tail[i][0] = tail[i][0] + tail[i][4] * sw;
        c->tail[i][1] = tail[i][1];
        c->tail[i][2] = tail[i][2] + 0.7f * tail[i][4] * sz;
        c->tail[i][3] = tail[i][3];
    }
    /* the head (its own frame, origin at the neck) */
    add(c, 1, P_SKULL, 0.0f, 0.095f, 0.035f, 0.108f, 0.092f, 0.095f);
    add(c, 1, P_MUZZLE, 0.0f, 0.045f, 0.115f, 0.05f, 0.036f, 0.034f);
    add(c, 1, P_NOSE, 0.0f, 0.064f, 0.146f, 0.015f, 0.011f, 0.010f);
    for (int s = -1; s <= 1; s += 2) {
        add(c, 1, P_BODY, 0.070f * (float)s, 0.05f, 0.065f, 0.056f, 0.052f, 0.056f);       /* cheeks */
        add(c, 1, P_EAR, 0.068f * (float)s, 0.176f, 0.0f, 0.042f, 0.040f, 0.020f);       /* ears: a wide base ... */
        add(c, 1, P_EAR, 0.076f * (float)s, 0.212f, 0.0f, 0.024f, 0.034f, 0.014f);       /* ... and a point */
    }

    /* where the eyes are and how straight the head looks at the camera */
    const float ex = 0.046f, ey = 0.106f, ez = 0.118f;
    for (int e = 0; e < 2; e++) {
        float hx = e ? ex : -ex, hy = ey, hz = ez;
        float b[3] = { c->neck[0] + c->rh[0] * hx + c->rh[1] * hy + c->rh[2] * hz, c->neck[1] + c->rh[3] * hx + c->rh[4] * hy + c->rh[5] * hz,
                       c->neck[2] + c->rh[6] * hx + c->rh[7] * hy + c->rh[8] * hz };
        c->eye[e][0] = c->pos[0] + c->scale * (b[0] * c->cy + b[2] * c->sy);
        c->eye[e][1] = c->pos[1] + c->scale * b[1];
        c->eye[e][2] = c->pos[2] + c->scale * (-b[0] * c->sy + b[2] * c->cy);
    }
    float hc[3] = { 0.0f, 0.095f, 0.035f };
    float hb[3] = { c->neck[0] + c->rh[0] * hc[0] + c->rh[1] * hc[1] + c->rh[2] * hc[2], c->neck[1] + c->rh[3] * hc[0] + c->rh[4] * hc[1] + c->rh[5] * hc[2],
                    c->neck[2] + c->rh[6] * hc[0] + c->rh[7] * hc[1] + c->rh[8] * hc[2] };
    float hw[3] = { c->pos[0] + c->scale * (hb[0] * c->cy + hb[2] * c->sy), c->pos[1] + c->scale * hb[1], c->pos[2] + c->scale * (-hb[0] * c->sy + hb[2] * c->cy) };
    float fb[3] = { c->rh[2], c->rh[5], c->rh[8] };                                       /* the head's forward in the body, then in the world */
    float fw[3] = { fb[0] * c->cy + fb[2] * c->sy, fb[1], -fb[0] * c->sy + fb[2] * c->cy };
    float dx = cam[0] - hw[0], dy = cam[1] - hw[1], dz = cam[2] - hw[2], dl = sqrtf(dx * dx + dy * dy + dz * dz);
    c->look = dl > 1e-3f ? (fw[0] * dx + fw[1] * dy + fw[2] * dz) / dl : 0.0f;
    c->glow = sstep(0.90f, 0.985f, c->look) * (0.88f + 0.12f * sinf(t * 4.1f));

    c->centre[0] = c->pos[0];
    c->centre[1] = c->pos[1] + 0.32f * c->scale;
    c->centre[2] = c->pos[2];
    c->radius = 0.62f * c->scale;                                          /* (the eyes' glow reaches 0.12 m beyond the head) */
}

/* ---- the ray ----------------------------------------------------------------------------------------------------------------- */

/* The colour of the surface of primitive `best`, hit at distance `bt`: the fur's letters, the light, the eyes. ob / db = the ray in the body's frame, oh / dh in the head's. */
static void shade_hit(const struct mcat *c, const float o[3], const float dn[3], float inv_f, int frame, int part, float bt, const float q[3], const float n[3], float rgb[3])
{
    float nb[3] = { n[0], n[1], n[2] };
    if (frame) {
        nb[0] = c->rh[0] * n[0] + c->rh[1] * n[1] + c->rh[2] * n[2];
        nb[1] = c->rh[3] * n[0] + c->rh[4] * n[1] + c->rh[5] * n[2];
        nb[2] = c->rh[6] * n[0] + c->rh[7] * n[1] + c->rh[8] * n[2];
    }
    float nw[3] = { nb[0] * c->cy + nb[2] * c->sy, nb[1], -nb[0] * c->sy + nb[2] * c->cy };
    float ndv = sat(-(nw[0] * dn[0] + nw[1] * dn[1] + nw[2] * dn[2]));
    float wy_hit = o[1] + bt * dn[1];

    /* the letters of the fur: the cell of the 3D grid, the face of it that the surface turns to, a glyph that changes now and then */
    int ci[3] = { (int)floorf(q[0] / CELL), (int)floorf(q[1] / CELL), (int)floorf(q[2] / CELL) };
    uint32_t hc = h32((uint32_t)ci[0] * 73856093u ^ (uint32_t)ci[1] * 19349663u ^ (uint32_t)ci[2] * 83492791u ^ (uint32_t)frame * 0x9e3779b9u);
    int epoch = (int)floorf(c->t * 1.3f + (float)((hc >> 12) & 255) / 256.0f * 4.0f);
    uint32_t hg = h32(hc + (uint32_t)epoch * 2246822519u);
    int ax = fabsf(n[0]) >= fabsf(n[1]) && fabsf(n[0]) >= fabsf(n[2]) ? 0 : fabsf(n[1]) >= fabsf(n[2]) ? 1 : 2;
    int au = ax == 0 ? 1 : 0, av = ax == 2 ? 1 : 2;
    float fu = q[au] / CELL - floorf(q[au] / CELL), fv = q[av] / CELL - floorf(q[av] / CELL);
    const uint8_t *g = c->nglyph > 0 && c->glyph ? c->glyph[hg % (uint32_t)c->nglyph] : NULL;
    int gx = (int)(fu * 9.0f), gy = (int)(fv * 16.0f);
    float bit = (g && gx < 8 && gy < 16 && ((g[gy] >> gx) & 1)) ? 1.0f : 0.0f;
    float foot = bt * inv_f, gp = CELL * c->scale / 16.0f;
    float mix = sat((foot / gp - 0.9f) * 0.6f) * 0.6f;               /* far: part of it stays the letter's own shape, so it still reads as text */
    float cov = 0.28f + 0.72f * (bit * (1.0f - mix) + 0.3f * mix);
    float cellb = 0.62f + 0.38f * frand(hc >> 3);
    float sparkle = ((hg >> 8) & 31) == 0 ? 1.0f : 0.0f;               /* now and then a letter of the rain lights up in the fur */

    float att = 1.0f / (1.0f + 0.018f * bt * bt);
    float L = 0.30f + 0.62f * ndv;                                   /* the room's own glow, from the front */
    float key = c->lstr > 0.0f ? sat(nw[0] * c->ldir[0] + nw[1] * c->ldir[1] + nw[2] * c->ldir[2]) * c->lstr : 0.0f;
    L += 0.55f * key;
    L *= 0.55f + 0.45f * sat(wy_hit / 0.15f);                         /* darker down by the floor */
    float rim = powf(1.0f - ndv, 3.0f);

    float part_gain = (part == P_BELLY || part == P_MUZZLE || part == P_PAW) ? 1.18f : 1.0f;
    if (part == P_EAR && n[2] > 0.45f)
        part_gain = 0.45f;                                            /* the inside of an ear is dark */
    float lum = 2.25f * att * L * cellb * cov * part_gain;
    float fur[3] = { 255.0f * (0.99f * lum + 0.30f * rim), 255.0f * (0.86f * lum + 0.26f * rim), 255.0f * (0.44f * lum + 0.08f * rim) };       /* cream-yellow */
    for (int i = 0; i < 3; i++)
        fur[i] += c->lcol[i] * 0.18f * key * att * cov;
    fur[0] += 70.0f * sparkle * bit * att; fur[1] += 255.0f * sparkle * bit * att; fur[2] += 110.0f * sparkle * bit * att;      /* (a letter of the rain, bright green) */

    if (part == P_NOSE) {
        fur[0] = 255.0f * sat(0.80f * L * att + 0.1f); fur[1] = 150.0f * sat(L * att); fur[2] = 150.0f * sat(L * att);
    } else if (part == P_SKULL) {
        for (int e = 0; e < 2; e++) {                                 /* the eyes: dark with a slit, or glowing */
            float dx = (q[0] - (e ? 0.046f : -0.046f)) / 0.025f, dy = (q[1] - 0.106f) / (0.018f * c->eye_open);
            float r2 = dx * dx + dy * dy;
            if (r2 < 1.4f && q[2] > 0.06f) {
                float edge = 1.0f - sstep(0.75f, 1.1f, r2);
                float gl = c->glow;
                float slit = mixf(0.30f, 0.13f, gl);
                float er = mixf(130.0f, 205.0f, gl), eg = mixf(135.0f, 255.0f, gl), eb = mixf(20.0f, 120.0f, gl);
                float k = fabsf(dx) < slit ? mixf(0.05f, 0.30f, gl) : 1.0f;
                float br = k * (0.55f + 0.7f * gl);
                fur[0] = mixf(fur[0], er * br, edge);
                fur[1] = mixf(fur[1], eg * br, edge);
                fur[2] = mixf(fur[2], eb * br, edge);
            }
        }
    }
    rgb[0] = fur[0]; rgb[1] = fur[1]; rgb[2] = fur[2];
}

/* The distance from p (body frame) to the surface of the tail: a tube of changing radius through the points. */
static float tail_sdf(const struct mcat *c, const float p[3])
{
    float best = 1e9f;
    for (int i = 0; i < 7; i++) {
        const float *a = c->tail[i], *b = c->tail[i + 1];
        float ex = b[0] - a[0], ey = b[1] - a[1], ez = b[2] - a[2], px = p[0] - a[0], py = p[1] - a[1], pz = p[2] - a[2];
        float h = sat((px * ex + py * ey + pz * ez) / (ex * ex + ey * ey + ez * ez));
        float dx = px - h * ex, dy = py - h * ey, dz = pz - h * ez;
        float d = sqrtf(dx * dx + dy * dy + dz * dz) - (a[3] + (b[3] - a[3]) * h);
        if (d < best)
            best = d;
    }
    return best;
}

bool mcat_shade(const struct mcat *c, const float o[3], const float dn[3], float tmax, float inv_f, float rgb[3])
{
    if (!c->enabled)
        return false;
    bool touched = false;
    float wx = c->centre[0] - o[0], wy = c->centre[1] - o[1], wz = c->centre[2] - o[2];
    float tp = wx * dn[0] + wy * dn[1] + wz * dn[2];
    float near2 = wx * wx + wy * wy + wz * wz - tp * tp;
    if (tp >= 0.0f && near2 <= c->radius * c->radius) {                    /* near the cat at all? */
        float inv = 1.0f / c->scale;                                        /* the ray in the body's frame (and the head's) */
        float px = o[0] - c->pos[0], py = o[1] - c->pos[1], pz = o[2] - c->pos[2];
        float ob[3] = { (px * c->cy - pz * c->sy) * inv, py * inv, (px * c->sy + pz * c->cy) * inv };
        float db[3] = { (dn[0] * c->cy - dn[2] * c->sy) * inv, dn[1] * inv, (dn[0] * c->sy + dn[2] * c->cy) * inv };
        static const float lo[3] = { -0.42f, 0.0f, -0.30f }, hi[3] = { 0.42f, 0.74f, 0.27f };       /* a box round the whole cat, in the body's frame */
        float t0 = 0.0f, t1 = tmax;
        for (int i = 0; i < 3 && t0 < t1; i++) {
            if (fabsf(db[i]) < 1e-9f) {
                if (ob[i] < lo[i] || ob[i] > hi[i])
                    t1 = -1.0f;
            } else {
                float a = (lo[i] - ob[i]) / db[i], b = (hi[i] - ob[i]) / db[i];
                t0 = fmaxf(t0, fminf(a, b));
                t1 = fminf(t1, fmaxf(a, b));
            }
        }
        if (t0 < t1) {
            float oh[3], dh[3];
            for (int i = 0; i < 3; i++) {
                oh[i] = c->rh[i] * (ob[0] - c->neck[0]) + c->rh[3 + i] * (ob[1] - c->neck[1]) + c->rh[6 + i] * (ob[2] - c->neck[2]);
                dh[i] = c->rh[i] * db[0] + c->rh[3 + i] * db[1] + c->rh[6 + i] * db[2];
            }
            /* the head's parts only when the ray goes near the head (a sphere of 0.25 round its middle) */
            float hx = -oh[0], hy = 0.10f - oh[1], hz = 0.04f - oh[2], dd2 = dh[0] * dh[0] + dh[1] * dh[1] + dh[2] * dh[2];
            float hp = (hx * dh[0] + hy * dh[1] + hz * dh[2]) / dd2;
            bool head = hx * hx + hy * hy + hz * hz - hp * hp * dd2 <= 0.0625f;
            int best = -1;
            float bt = tmax;
            for (int i = 0; i < c->n; i++) {
                const struct mcat_prim *p = &c->prim[i];
                if (p->frame && !head)
                    continue;
                const float *oo = p->frame ? oh : ob, *dd = p->frame ? dh : db;
                float ox = (oo[0] - p->c[0]) * p->ir[0], oy = (oo[1] - p->c[1]) * p->ir[1], oz = (oo[2] - p->c[2]) * p->ir[2];
                float ddx = dd[0] * p->ir[0], ddy = dd[1] * p->ir[1], ddz = dd[2] * p->ir[2];
                float a = ddx * ddx + ddy * ddy + ddz * ddz, b = ox * ddx + oy * ddy + oz * ddz, cc = ox * ox + oy * oy + oz * oz - 1.0f;
                float disc = b * b - a * cc;
                if (disc <= 0.0f)
                    continue;
                float t = (-b - sqrtf(disc)) / a;
                if (t > 1e-3f && t < bt) {
                    bt = t;
                    best = i;
                }
            }
            /* the tail: sphere tracing through the tube, when the ray goes near it (a sphere of 0.31 round it) */
            float tx = 0.24f - ob[0], ty = 0.05f - ob[1], tz = -0.02f - ob[2], bd2 = db[0] * db[0] + db[1] * db[1] + db[2] * db[2];
            float tq = (tx * db[0] + ty * db[1] + tz * db[2]) / bd2, tr2 = 0.31f * 0.31f - (tx * tx + ty * ty + tz * tz - tq * tq * bd2);
            float tail_t = -1.0f;
            if (tr2 > 0.0f) {
                float half = sqrtf(tr2 / bd2), ts = fmaxf(tq - half, 0.0f), te = fminf(tq + half, bt);
                float tt = ts;
                for (int step = 0; step < 26 && tt < te; step++) {
                    float pt[3] = { ob[0] + tt * db[0], ob[1] + tt * db[1], ob[2] + tt * db[2] };
                    float d = tail_sdf(c, pt);
                    if (d < 0.0015f) {
                        tail_t = tt;
                        break;
                    }
                    tt += d * c->scale * 0.9f;                          /* (the ray's parameter is in metres of the world, the distance in the cat's own) */
                }
            }
            if (tail_t >= 0.0f && tail_t < bt) {
                float pt[3] = { ob[0] + tail_t * db[0], ob[1] + tail_t * db[1], ob[2] + tail_t * db[2] }, e = 0.004f, g[3];
                for (int k = 0; k < 3; k++) {
                    float pp[3] = { pt[0], pt[1], pt[2] }, pm[3] = { pt[0], pt[1], pt[2] };
                    pp[k] += e;
                    pm[k] -= e;
                    g[k] = tail_sdf(c, pp) - tail_sdf(c, pm);
                }
                float gl = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
                if (gl > 1e-9f) {
                    g[0] /= gl; g[1] /= gl; g[2] /= gl;
                    shade_hit(c, o, dn, inv_f, 0, P_TAIL, tail_t, pt, g, rgb);
                    touched = true;
                    best = -2;
                }
            }
            if (best >= 0) {
                const struct mcat_prim *p = &c->prim[best];
                const float *oo = p->frame ? oh : ob, *dd = p->frame ? dh : db;
                float q[3] = { oo[0] + bt * dd[0], oo[1] + bt * dd[1], oo[2] + bt * dd[2] };
                float n[3] = { (q[0] - p->c[0]) * p->ir[0] * p->ir[0], (q[1] - p->c[1]) * p->ir[1] * p->ir[1], (q[2] - p->c[2]) * p->ir[2] * p->ir[2] };
                float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                n[0] /= nl; n[1] /= nl; n[2] /= nl;
                shade_hit(c, o, dn, inv_f, p->frame, p->part, bt, q, n, rgb);
                touched = true;
            }
        }
    }

    if (c->glow > 0.02f && tp >= 0.0f && near2 <= c->radius * c->radius) {      /* the glow round the eyes (in front of what lies behind them) */
        float hr = 0.032f * c->scale;
        for (int e = 0; e < 2; e++) {
            float ex = c->eye[e][0] - o[0], ey = c->eye[e][1] - o[1], ez = c->eye[e][2] - o[2];
            float te = ex * dn[0] + ey * dn[1] + ez * dn[2];
            if (te <= 0.05f || te > tmax + 0.05f)
                continue;
            float pd2 = ex * ex + ey * ey + ez * ez - te * te;
            if (pd2 > 9.0f * hr * hr)
                continue;
            float k = c->glow * c->eye_open * expf(-pd2 / (hr * hr));
            if (k < 0.01f)
                continue;
            touched = true;
            rgb[0] += 170.0f * k; rgb[1] += 255.0f * k; rgb[2] += 110.0f * k;
        }
    }
    return touched;
}

float mcat_floor_shade(const struct mcat *c, float x, float z)
{
    if (!c->enabled)
        return 1.0f;
    float dx = x - c->pos[0], dz = z - c->pos[2];
    if (dx * dx + dz * dz > 1.2f)
        return 1.0f;
    float inv = 1.0f / c->scale, lx = (dx * c->cy - dz * c->sy) * inv, lz = (dx * c->sy + dz * c->cy) * inv;
    float s1 = expf(-(lx * lx / (0.23f * 0.23f) + (lz + 0.02f) * (lz + 0.02f) / (0.26f * 0.26f)));         /* right under it */
    float s2 = expf(-(lx * lx / (0.45f * 0.45f) + lz * lz / (0.50f * 0.50f)));                           /* round it */
    return (1.0f - 0.70f * s1) * (1.0f - 0.30f * s2);
}
