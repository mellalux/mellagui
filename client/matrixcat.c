/* matrixcat: Tom, the cat on the floor of the Matrix room. See matrixcat.h. */
#pragma GCC optimize("O3", "fast-math")
#include "matrixcat.h"

#include <math.h>
#include <string.h>

#define CELL 0.032f                     /* one letter of the fur, in the cat's own metres */
#define SHELL 0.011f                    /* how far the loose letters of the fluffy edge reach beyond the surface */
#define SEG_S 6.0f                      /* the head's script: one action every 6 seconds (a little late, a little different) */
#define WALK_SPEED 0.28f                /* metres a second */
#define STRIDE 0.30f                    /* one gait cycle moves the body this far (the cat's own metres) */

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
    p->irs[0] = 1.0f / (rx + SHELL); p->irs[1] = 1.0f / (ry + SHELL); p->irs[2] = 1.0f / (rz + SHELL);
    float rmin = fminf(rx, fminf(ry, rz));
    p->shell2 = (1.0f + SHELL / rmin) * (1.0f + SHELL / rmin);
    p->frame = (uint8_t)frame;
    p->part = (uint8_t)part;
}

/* ---- the body: every part in two poses (sitting, lying down), blended by `lie` -------------------------------------------------------------- */

/* The trunk in three poses: sitting, lying down, standing (walking). The legs have their own tables below. */
struct pdef {
    uint8_t part;
    bool mirror;                        /* also its mirror image (x -> -x) */
    float s[6], l[6], t[6];             /* centre and radii: sitting / lying / standing */
};

static const struct pdef body_defs[] = {
    { P_BODY, false, { 0.0f, 0.14f, -0.07f, 0.170f, 0.140f, 0.170f }, { 0.0f, 0.11f, -0.12f, 0.190f, 0.110f, 0.180f }, { 0.0f, 0.29f, -0.13f, 0.115f, 0.115f, 0.130f } },       /* haunch */
    { P_BODY, true, { 0.12f, 0.16f, -0.03f, 0.070f, 0.120f, 0.120f }, { 0.14f, 0.08f, -0.06f, 0.070f, 0.075f, 0.130f }, { 0.095f, 0.26f, -0.14f, 0.065f, 0.115f, 0.110f } }, /* thighs */
    { P_BODY, false, { 0.0f, 0.29f, 0.00f, 0.100f, 0.200f, 0.100f }, { 0.0f, 0.13f, 0.07f, 0.115f, 0.110f, 0.200f }, { 0.0f, 0.30f, 0.04f, 0.100f, 0.105f, 0.190f } },       /* torso (breathes) */
    { P_BELLY, false, { 0.0f, 0.21f, 0.075f, 0.090f, 0.130f, 0.075f }, { 0.0f, 0.11f, 0.18f, 0.100f, 0.100f, 0.090f }, { 0.0f, 0.27f, 0.17f, 0.095f, 0.115f, 0.090f } },     /* chest */
    { P_BODY, false, { 0.0f, 0.375f, 0.08f, 0.150f, 0.080f, 0.110f }, { 0.0f, 0.23f, 0.21f, 0.140f, 0.085f, 0.100f }, { 0.0f, 0.33f, 0.215f, 0.125f, 0.085f, 0.090f } },     /* the ruff */
};

/* A leg: hip, knee and paw in each pose (sitting / lying / standing), the radii of the tube (hip, knee, ankle) and of the paw's ellipsoid in each pose.
 * `rear` = a hind leg: its knee bends forwards, a front leg's elbow bends back. */
struct legdef {
    bool rear;
    float hip[3][3], knee[3][3], paw[3][3], tr[3], pr[3][3];
};

static const struct legdef k_front = { false,
    { { 0.050f, 0.250f, 0.085f }, { 0.060f, 0.120f, 0.200f }, { 0.070f, 0.275f, 0.185f } },
    { { 0.047f, 0.135f, 0.110f }, { 0.055f, 0.055f, 0.285f }, { 0.070f, 0.150f, 0.160f } },
    { { 0.047f, 0.026f, 0.165f }, { 0.050f, 0.030f, 0.345f }, { 0.070f, 0.026f, 0.195f } },
    { 0.038f, 0.030f, 0.026f },
    { { 0.036f, 0.024f, 0.052f }, { 0.038f, 0.028f, 0.050f }, { 0.036f, 0.024f, 0.048f } } };
static const struct legdef k_hind = { true,
    { { 0.115f, 0.150f, -0.055f }, { 0.140f, 0.100f, -0.080f }, { 0.090f, 0.270f, -0.130f } },
    { { 0.140f, 0.065f, 0.085f }, { 0.160f, 0.050f, 0.020f }, { 0.100f, 0.160f, -0.080f } },
    { { 0.115f, 0.030f, 0.080f }, { 0.140f, 0.030f, 0.040f }, { 0.090f, 0.026f, -0.115f } },
    { 0.060f, 0.040f, 0.028f },
    { { 0.042f, 0.030f, 0.100f }, { 0.040f, 0.030f, 0.100f }, { 0.038f, 0.026f, 0.058f } } };

/* ---- Tom's day: a timeline of what he does ------------------------------------------------------------------------------------------- */

enum { K_SIT, K_STAND, K_WALK, K_SITDOWN, K_LIE };
struct seg { int kind, a, b; float dur; };

#define N_SPOTS 2
static const float k_spot[N_SPOTS][3] = { { 0.55f, 4.85f, 3.14159265f + 0.35f }, { -0.85f, 4.50f, 3.14159265f - 0.30f } };      /* x, z, the way he faces sitting */

static struct seg g_seg[16];
static int g_nseg;
static float g_cycle;

/* The path between two spots: a curve (so the way back is a different one); u 0..1 along the curve. */
static void bez_point(int a, int b, float u, float *x, float *z, float *dx, float *dz)
{
    float ax = k_spot[a][0], az = k_spot[a][1], bx = k_spot[b][0], bz = k_spot[b][1];
    float ex = bx - ax, ez = bz - az, el = sqrtf(ex * ex + ez * ez);
    float cx = (ax + bx) * 0.5f - ez / el * 0.6f, cz = (az + bz) * 0.5f + ex / el * 0.6f, v = 1.0f - u;
    *x = v * v * ax + 2.0f * v * u * cx + u * u * bx;
    *z = v * v * az + 2.0f * v * u * cz + u * u * bz;
    *dx = 2.0f * v * (cx - ax) + 2.0f * u * (bx - cx);
    *dz = 2.0f * v * (cz - az) + 2.0f * u * (bz - cz);
}

/* The arc length of a path and the u that is `frac` of the way along it. */
static float path_arc(int a, int b, float frac, float *u_out)
{
    float cum[33], px = 0, pz = 0, dx, dz;
    cum[0] = 0.0f;
    bez_point(a, b, 0.0f, &px, &pz, &dx, &dz);
    for (int i = 1; i <= 32; i++) {
        float x, z;
        bez_point(a, b, (float)i / 32.0f, &x, &z, &dx, &dz);
        cum[i] = cum[i - 1] + sqrtf((x - px) * (x - px) + (z - pz) * (z - pz));
        px = x;
        pz = z;
    }
    if (u_out) {
        float want = frac * cum[32];
        int i = 1;
        while (i < 32 && cum[i] < want)
            i++;
        float seg = cum[i] - cum[i - 1];
        *u_out = ((float)(i - 1) + (seg > 1e-6f ? (want - cum[i - 1]) / seg : 0.0f)) / 32.0f;
    }
    return cum[32];
}

static void timeline_build(void)
{
    if (g_nseg)
        return;
    float ab = path_arc(0, 1, 1.0f, NULL) / WALK_SPEED, ba = path_arc(1, 0, 1.0f, NULL) / WALK_SPEED;
    const struct seg day[] = {
        { K_SIT, 0, 0, 55.0f }, { K_STAND, 0, 1, 4.0f }, { K_WALK, 0, 1, ab }, { K_SITDOWN, 0, 1, 4.0f },
        { K_SIT, 1, 1, 25.0f }, { K_LIE, 1, 1, 52.0f }, { K_SIT, 1, 1, 10.0f },
        { K_STAND, 1, 0, 4.0f }, { K_WALK, 1, 0, ba }, { K_SITDOWN, 1, 0, 4.0f }, { K_SIT, 0, 0, 20.0f },
    };
    g_nseg = (int)(sizeof(day) / sizeof(day[0]));
    g_cycle = 0.0f;
    for (int i = 0; i < g_nseg; i++) {
        g_seg[i] = day[i];
        g_cycle += day[i].dur;
    }
}

float mcat_cycle(void)
{
    timeline_build();
    return g_cycle;
}

static float lerp_angle(float a, float b, float t)
{
    float d = fmodf(b - a + 3.0f * 3.14159265f, 2.0f * 3.14159265f) - 3.14159265f;
    return a + d * t;
}

static struct mcat_state state_at(float t)
{
    timeline_build();
    float f = fmodf(t, g_cycle), acc = 0.0f;
    int i = 0;
    while (i < g_nseg - 1 && f >= acc + g_seg[i].dur) {
        acc += g_seg[i].dur;
        i++;
    }
    const struct seg *sg = &g_seg[i];
    float tau = f - acc, u = sat(tau / sg->dur);
    struct mcat_state st = { k_spot[sg->b][0], k_spot[sg->b][1], k_spot[sg->b][2], 0, 0, 0, 0 };
    float x, z, dx, dz;
    switch (sg->kind) {
    case K_SIT:
        st.x = k_spot[sg->a][0]; st.z = k_spot[sg->a][1]; st.yaw = k_spot[sg->a][2];
        break;
    case K_LIE:
        st.x = k_spot[sg->a][0]; st.z = k_spot[sg->a][1]; st.yaw = k_spot[sg->a][2];
        st.lie = sstep(0.0f, 6.0f, tau) * (1.0f - sstep(sg->dur - 6.0f, sg->dur, tau));
        break;
    case K_STAND:                                                       /* up, and turning to where the path begins */
        bez_point(sg->a, sg->b, 0.0f, &x, &z, &dx, &dz);
        st.x = k_spot[sg->a][0]; st.z = k_spot[sg->a][1];
        st.yaw = lerp_angle(k_spot[sg->a][2], atan2f(dx, dz), sstep(0.25f, 1.0f, u));
        st.stand = sstep(0.0f, 0.8f, u);
        break;
    case K_WALK: {
        float arc = path_arc(sg->a, sg->b, 1.0f, NULL), e = mixf(u, sstep(0.0f, 1.0f, u), 0.6f), uu;
        path_arc(sg->a, sg->b, e, &uu);
        bez_point(sg->a, sg->b, uu, &x, &z, &dx, &dz);
        st.x = x; st.z = z; st.yaw = atan2f(dx, dz);
        st.stand = 1.0f;
        st.dist = e * arc;
        float speed = ((1.0f - 0.6f) + 0.6f * 6.0f * u * (1.0f - u)) * arc / sg->dur;      /* (the derivative of e) */
        st.walk = sat(speed / (0.55f * WALK_SPEED));
        break;
    }
    case K_SITDOWN:                                                     /* turns to face the room, and down */
        bez_point(sg->a, sg->b, 1.0f, &x, &z, &dx, &dz);
        st.yaw = lerp_angle(atan2f(dx, dz), k_spot[sg->b][2], sstep(0.0f, 0.75f, u));
        st.stand = 1.0f - sstep(0.2f, 1.0f, u);
        break;
    }
    return st;
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

void mcat_state(float t, struct mcat_state *out)
{
    *out = state_at(t);
}

void mcat_head_script(float t, float cam_yaw, float cam_pitch, float *yaw, float *pitch, float *roll)
{
    struct hang h = head_at(t, cam_yaw, cam_pitch);
    *yaw = h.yaw + 0.03f * sinf(t * 0.7f);                              /* (never quite still) */
    *pitch = h.pitch + 0.02f * sinf(t * 0.9f + 1.0f);
    *roll = h.roll;
}

float mcat_eye_open(float t)
{
    float tb = fmodf(t + 1.3f, 4.6f);
    return tb < 0.18f ? 1.0f - 0.92f * sinf(3.14159265f * tb / 0.18f) : 1.0f;
}

/* An ear's flick: 0 most of the time, a quick 0..1..0 every few seconds. */
static float ear_flick(float t, float period, float phase)
{
    float f = fmodf(t + phase, period);
    return f < 0.28f ? sinf(3.14159265f * f / 0.28f) : 0.0f;
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
    struct mcat_state st = state_at(t);
    c->pos[0] = st.x; c->pos[1] = 0.0f; c->pos[2] = st.z;
    c->yaw = st.yaw;
    c->cy = cosf(c->yaw);
    c->sy = sinf(c->yaw);
    c->lie = st.lie;
    c->stand = st.stand;
    c->walk = st.walk;
    float lie = c->lie, stand = c->stand, phase = st.dist / (STRIDE * c->scale);         /* the gait goes by the distance walked, so the paws do not slide */
    float bob = 0.011f * sinf(6.2831853f * 2.0f * phase) * st.walk * stand;                 /* the body rises and falls twice a stride */
    c->neck[0] = 0.0f;
    c->neck[1] = mixf(mixf(0.40f, 0.20f, lie), 0.355f + bob, stand);
    c->neck[2] = mixf(mixf(0.04f, 0.215f, lie), 0.255f, stand);
    c->glyph = glyph;
    c->nglyph = nglyph;
    memcpy(c->ldir, ldir, sizeof(c->ldir));
    memcpy(c->lcol, lcol, sizeof(c->lcol));
    c->lstr = lstr;
    c->sh_w = 0.23f; c->sh_l = 0.28f; c->sh_h = mixf(0.55f, 0.28f, c->lie) * c->scale;

    float wx = cam[0] - c->pos[0], wy = cam[1] - c->pos[1], wz = cam[2] - c->pos[2], inv = 1.0f / c->scale;       /* the camera as the body sees it */
    float bx = (wx * c->cy - wz * c->sy) * inv, by = wy * inv, bz = (wx * c->sy + wz * c->cy) * inv;
    float vx = bx - c->neck[0], vy = by - (c->neck[1] + 0.10f), vz = bz - c->neck[2];
    float cam_yaw = atan2f(vx, vz), cam_pitch = -atan2f(vy, sqrtf(vx * vx + vz * vz));

    struct hang h = head_at(t, cam_yaw, cam_pitch);
    h.yaw += 0.03f * sinf(t * 0.7f);                                     /* it is never quite still */
    h.pitch += 0.02f * sinf(t * 0.9f + 1.0f) + 0.10f * lie + 0.025f * sinf(6.2831853f * 2.0f * phase) * st.walk * stand;   /* (lying, the head rests lower; walking, it bobs) */
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
    for (size_t i = 0; i < sizeof(body_defs) / sizeof(body_defs[0]); i++) {
        const struct pdef *d = &body_defs[i];
        float v[6];
        for (int k = 0; k < 6; k++)
            v[k] = mixf(mixf(d->s[k], d->l[k], lie), d->t[k], stand);
        v[1] += bob;
        if (i == 2) {
            v[3] *= br;
            v[5] *= br;
        }
        add(c, 0, d->part, v[0], v[1], v[2], v[3], v[4], v[5]);
        if (d->mirror)
            add(c, 0, d->part, -v[0], v[1], v[2], v[3], v[4], v[5]);
    }
    /* the legs: each a tube hip - knee - ankle, with a paw and two toes; standing, the paws follow the gait (diagonal pairs together) and the knee is
     * found from the hip and the paw (two equal bones, bending back for a front leg and forward for a hind one) */
    for (int leg = 0; leg < 4; leg++) {
        const struct legdef *d = leg < 2 ? &k_front : &k_hind;
        float m = (leg & 1) ? -1.0f : 1.0f, hip[3], knee[3], paw[3], pr[3];
        float u = fmodf(phase + ((leg == 1 || leg == 2) ? 0.5f : 0.0f), 1.0f), dz = 0.0f, dy = 0.0f, amp = 0.09f * st.walk;
        if (u < 0.6f) {                                                  /* stance: the paw goes back as fast as the body goes forward */
            dz = amp * (1.0f - 2.0f * u / 0.6f);
        } else {                                                         /* swing: up and forward */
            float v = (u - 0.6f) / 0.4f;
            dz = amp * (-1.0f + 2.0f * sstep(0.0f, 1.0f, v));
            dy = 0.05f * st.walk * sinf(3.14159265f * v);
        }
        for (int k = 0; k < 3; k++) {
            hip[k] = mixf(mixf(d->hip[0][k], d->hip[1][k], lie), d->hip[2][k], stand);
            paw[k] = mixf(mixf(d->paw[0][k], d->paw[1][k], lie), d->paw[2][k], stand);
            pr[k] = mixf(mixf(d->pr[0][k], d->pr[1][k], lie), d->pr[2][k], stand);
        }
        hip[1] += bob * stand;
        float sh[3] = { d->hip[2][0], d->hip[2][1] + bob, d->hip[2][2] }, sp[3] = { d->paw[2][0], d->paw[2][1] + dy, d->paw[2][2] + dz };       /* the standing leg */
        paw[1] = mixf(mixf(d->paw[0][1], d->paw[1][1], lie), sp[1], stand);
        paw[2] = mixf(mixf(d->paw[0][2], d->paw[1][2], lie), sp[2], stand);
        {                                                                /* the knee of the standing leg: two bones of 0.13 */
            float ey = sp[1] - sh[1], ez = sp[2] - sh[2], len = sqrtf(ey * ey + ez * ez), half = fminf(0.5f * len, 0.13f), bend = sqrtf(0.13f * 0.13f - half * half);
            float ny = -ez / fmaxf(len, 1e-4f), nz = ey / fmaxf(len, 1e-4f);
            if ((d->rear ? nz : -nz) < 0.0f) {
                ny = -ny;
                nz = -nz;
            }
            float ks[3] = { 0.5f * (sh[0] + sp[0]), 0.5f * (sh[1] + sp[1]) + bend * ny, 0.5f * (sh[2] + sp[2]) + bend * nz };
            for (int k = 0; k < 3; k++)
                knee[k] = mixf(mixf(d->knee[0][k], d->knee[1][k], lie), ks[k], stand);
        }
        struct mcat_tube *tb = &c->tube[1 + leg];
        tb->n = 3;
        tb->part = P_BODY;
        float pts[3][4] = { { m * hip[0], hip[1], hip[2], d->tr[0] }, { m * knee[0], knee[1], knee[2], d->tr[1] }, { m * paw[0], paw[1] + 0.012f, paw[2], d->tr[2] } };
        memcpy(tb->p, pts, sizeof(pts));
        add(c, 0, P_PAW, m * paw[0], paw[1], paw[2], pr[0], pr[1], pr[2]);
        for (int k = -1; k <= 1; k += 2)                                 /* two toes at the front of the paw */
            add(c, 0, P_PAW, m * paw[0] + (float)k * 0.014f, paw[1] - 0.004f, paw[2] + 0.88f * pr[2], 0.014f, 0.014f, 0.016f);
    }
    /* the tail: sitting it curls round the paws and its tip swishes; walking it stands up and sways */
    {
        float sw = 0.045f * sinf(t * 1.3f), sz = 0.03f * sinf(t * 0.9f + 1.0f), sw2 = 0.05f * sinf(t * 2.1f);
        static const float tl[8][5] = {                                  /* x, y, z, radius, how much the swish moves it */
            { 0.13f, 0.070f, -0.21f, 0.058f, 0.0f }, { 0.20f, 0.065f, -0.18f, 0.060f, 0.0f }, { 0.26f, 0.060f, -0.12f, 0.060f, 0.0f },
            { 0.30f, 0.055f, -0.05f, 0.057f, 0.0f }, { 0.315f, 0.050f, 0.03f, 0.052f, 0.2f }, { 0.30f, 0.047f, 0.10f, 0.046f, 0.5f },
            { 0.27f, 0.044f, 0.16f, 0.040f, 0.8f }, { 0.23f, 0.040f, 0.21f, 0.033f, 1.0f } };
        static const float ts[8][5] = {
            { 0.0f, 0.30f, -0.25f, 0.052f, 0.0f }, { 0.0f, 0.37f, -0.32f, 0.054f, 0.1f }, { 0.0f, 0.47f, -0.37f, 0.052f, 0.25f },
            { 0.0f, 0.57f, -0.38f, 0.048f, 0.45f }, { 0.0f, 0.65f, -0.36f, 0.044f, 0.65f }, { 0.0f, 0.71f, -0.32f, 0.040f, 0.8f },
            { 0.0f, 0.74f, -0.27f, 0.035f, 0.9f }, { 0.0f, 0.745f, -0.22f, 0.030f, 1.0f } };
        struct mcat_tube *tb = &c->tube[0];
        tb->n = 8;
        tb->part = P_TAIL;
        for (int i = 0; i < 8; i++) {
            tb->p[i][0] = mixf(tl[i][0] + tl[i][4] * sw, ts[i][0] + ts[i][4] * sw2, stand);
            tb->p[i][1] = mixf(tl[i][1], ts[i][1] + bob, stand);
            tb->p[i][2] = mixf(tl[i][2] + 0.7f * tl[i][4] * sz, ts[i][2], stand);
            tb->p[i][3] = mixf(tl[i][3], ts[i][3], stand);
        }
    }
    for (int k = 0; k < MCAT_TUBES; k++) {                               /* the segments and the box of every tube */
        struct mcat_tube *tb = &c->tube[k];
        for (int i = 0; i + 1 < tb->n; i++) {
            float ex = tb->p[i + 1][0] - tb->p[i][0], ey = tb->p[i + 1][1] - tb->p[i][1], ez = tb->p[i + 1][2] - tb->p[i][2];
            tb->seg[i][0] = ex; tb->seg[i][1] = ey; tb->seg[i][2] = ez;
            tb->seg[i][3] = 1.0f / fmaxf(ex * ex + ey * ey + ez * ez, 1e-8f);
        }
        for (int a = 0; a < 3; a++) {
            tb->lo[a] = 1e9f;
            tb->hi[a] = -1e9f;
            for (int i = 0; i < tb->n; i++) {
                tb->lo[a] = fminf(tb->lo[a], tb->p[i][a] - tb->p[i][3] - SHELL - 0.012f);
                tb->hi[a] = fmaxf(tb->hi[a], tb->p[i][a] + tb->p[i][3] + SHELL + 0.012f);
            }
        }
    }
    /* the head (its own frame, origin at the neck) */
    add(c, 1, P_SKULL, 0.0f, 0.095f, 0.035f, 0.108f, 0.092f, 0.095f);
    add(c, 1, P_MUZZLE, 0.0f, 0.045f, 0.115f, 0.05f, 0.036f, 0.034f);
    add(c, 1, P_NOSE, 0.0f, 0.064f, 0.146f, 0.015f, 0.011f, 0.010f);
    for (int s = -1; s <= 1; s += 2) {
        float fl = ear_flick(t, s < 0 ? 3.9f : 5.3f, s < 0 ? 0.5f : 2.1f);          /* the ears flick now and then, on their own */
        add(c, 1, P_BODY, 0.070f * (float)s, 0.05f, 0.065f, 0.056f, 0.052f, 0.056f);                                   /* cheeks */
        add(c, 1, P_EAR, 0.068f * (float)s, 0.176f - 0.006f * fl, 0.0f, 0.042f, 0.040f, 0.020f);                      /* ears: a wide base ... */
        add(c, 1, P_EAR, (0.076f + 0.016f * fl) * (float)s, 0.212f - 0.010f * fl, -0.012f * fl, 0.024f, 0.034f, 0.014f);   /* ... and a point */
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
    c->centre[1] = c->pos[1] + 0.36f * c->scale;
    c->centre[2] = c->pos[2] + 0.04f * c->scale * lie;
    c->radius = 0.72f * c->scale;                                          /* (the eyes' glow reaches 0.12 m beyond the head) */
}

/* ---- the ray ----------------------------------------------------------------------------------------------------------------- */

/* The colour of a point of the fur: the fur's letters, the light, the eyes, the mouth. `frame` / `part` say which part it is on, q is the point in the frame of that
 * part, n the surface normal there (in that frame), bt the distance from the camera. */
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
    } else if (part == P_MUZZLE && q[2] > 0.105f) {                   /* the mouth: a line down from the nose that parts into two small arcs */
        float ax_ = fabsf(q[0]), line = 0.0f, tw = 0.0034f;
        if (ax_ < tw && q[1] > 0.040f && q[1] < 0.058f)
            line = 1.0f;
        float arc = 0.041f - 0.55f * ax_ - 0.0f;
        if (ax_ < 0.026f && fabsf(q[1] - arc) < tw)
            line = 1.0f;
        float shade = 1.0f - 0.78f * line;
        fur[0] *= shade; fur[1] *= shade; fur[2] *= shade;
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

/* A loose letter of the fluffy edge: the point q is on the shell (within SHELL of the surface), in `frame`; paints only the strokes of the letter. */
static bool fuzz_letter(const struct mcat *c, float bt, float inv_f, int frame, const float q[3], const float dn[3], float rgb[3])
{
    int ci[3] = { (int)floorf(q[0] / CELL), (int)floorf(q[1] / CELL), (int)floorf(q[2] / CELL) };
    uint32_t hc = h32((uint32_t)ci[0] * 73856093u ^ (uint32_t)ci[1] * 19349663u ^ (uint32_t)ci[2] * 83492791u ^ (uint32_t)frame * 0x9e3779b9u ^ 0x5bd1e995u);
    if ((hc >> 5) % 8 >= 3)                                          /* about three cells in eight carry a letter */
        return false;
    int epoch = (int)floorf(c->t * 1.1f + (float)((hc >> 12) & 255) / 256.0f * 4.0f);
    uint32_t hg = h32(hc + (uint32_t)epoch * 2246822519u);
    const uint8_t *g = c->nglyph > 0 && c->glyph ? c->glyph[hg % (uint32_t)c->nglyph] : NULL;
    int ax = fabsf(dn[0]) >= fabsf(dn[1]) && fabsf(dn[0]) >= fabsf(dn[2]) ? 0 : fabsf(dn[1]) >= fabsf(dn[2]) ? 1 : 2;   /* (the letter faces the way we look) */
    int au = ax == 0 ? 1 : 0, av = ax == 2 ? 1 : 2;
    float fu = q[au] / CELL - floorf(q[au] / CELL), fv = q[av] / CELL - floorf(q[av] / CELL);
    int gx = (int)(fu * 9.0f), gy = (int)(fv * 16.0f);
    if (!(g && gx < 8 && gy < 16 && ((g[gy] >> gx) & 1)))
        return false;
    float att = 1.0f / (1.0f + 0.018f * bt * bt), k = 0.80f * att * (0.55f + 0.45f * frand(hc >> 3));
    (void)inv_f;
    rgb[0] = mixf(rgb[0], 255.0f * 0.97f * (0.45f + k), 0.75f);
    rgb[1] = mixf(rgb[1], 255.0f * 0.85f * (0.45f + k), 0.75f);
    rgb[2] = mixf(rgb[2], 255.0f * 0.45f * (0.45f + k), 0.75f);
    return true;
}

/* The distance from p (body frame) to the surface of a tube of changing radius through its points. */
static float tube_sdf(const struct mcat_tube *tb, const float p[3])
{
    float best = 1e9f;
    for (int i = 0; i + 1 < tb->n; i++) {
        const float *a = tb->p[i], *b = tb->p[i + 1], *e = tb->seg[i];
        float px = p[0] - a[0], py = p[1] - a[1], pz = p[2] - a[2];
        float h = sat((px * e[0] + py * e[1] + pz * e[2]) * e[3]);
        float dx = px - h * e[0], dy = py - h * e[1], dz = pz - h * e[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz) - (a[3] + (b[3] - a[3]) * h);
        if (d < best)
            best = d;
    }
    return best;
}

/* Whiskers: three a side, from the muzzle (head frame; segment start and the way it goes). */
static const float k_whisker[6][6] = {
    { -0.030f, 0.048f, 0.128f, -0.115f, 0.032f, 0.045f }, { -0.030f, 0.044f, 0.128f, -0.125f, -0.004f, 0.030f }, { -0.030f, 0.040f, 0.126f, -0.105f, -0.040f, 0.020f },
    { 0.030f, 0.048f, 0.128f, 0.115f, 0.032f, 0.045f }, { 0.030f, 0.044f, 0.128f, 0.125f, -0.004f, 0.030f }, { 0.030f, 0.040f, 0.126f, 0.105f, -0.040f, 0.020f },
};

/* The whiskers along the ray (head frame: oh + t dh, t in metres): the nearest one with its coverage, if it is in front of `limit`. */
static float whiskers(const float oh[3], const float dh[3], float limit, float inv_f, float inv_s, float *at)
{
    float best = 0.0f, a = dh[0] * dh[0] + dh[1] * dh[1] + dh[2] * dh[2];
    for (int i = 0; i < 6; i++) {
        const float *w = k_whisker[i];
        float e[3] = { w[3], w[4], w[5] }, w0[3] = { oh[0] - w[0], oh[1] - w[1], oh[2] - w[2] };
        float b = dh[0] * e[0] + dh[1] * e[1] + dh[2] * e[2], cc = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
        float d = dh[0] * w0[0] + dh[1] * w0[1] + dh[2] * w0[2], ee = e[0] * w0[0] + e[1] * w0[1] + e[2] * w0[2];
        float den = a * cc - b * b;
        if (den < 1e-12f)
            continue;
        float s = sat((a * ee - b * d) / den), t = (b * s - d) / a;
        if (t <= 0.05f || t >= limit)
            continue;
        float px = w0[0] + t * dh[0] - s * e[0], py = w0[1] + t * dh[1] - s * e[1], pz = w0[2] + t * dh[2] - s * e[2];
        float dist = sqrtf(px * px + py * py + pz * pz);
        float pix = t * inv_f * inv_s, rad = fmaxf(0.0016f, 0.55f * pix);         /* (a pixel in the cat's own units) */
        float cov = (1.0f - sstep(rad * 0.6f, rad * 1.5f, dist)) * clampf(0.0016f / (0.55f * pix), 0.4f, 1.0f);
        if (cov > best) {
            best = cov;
            *at = t;
        }
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
        static const float lo[3] = { -0.45f, 0.0f, -0.50f }, hi[3] = { 0.45f, 0.80f, 0.50f };       /* a box round the whole cat, in the body's frame */
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
            /* the head's parts only when the ray goes near the head (a sphere of 0.28 round its middle: the whiskers reach out) */
            float hx = -oh[0], hy = 0.10f - oh[1], hz = 0.04f - oh[2], dd2 = dh[0] * dh[0] + dh[1] * dh[1] + dh[2] * dh[2];
            float hp = (hx * dh[0] + hy * dh[1] + hz * dh[2]) / dd2;
            bool head = hx * hx + hy * hy + hz * hz - hp * hp * dd2 <= 0.0784f;
            /* ... and the body's parts only when it goes near the body (a sphere of 0.42; the tail has its own test below) */
            float kx = -ob[0], ky = 0.20f - ob[1], kz = 0.05f + 0.10f * c->lie - ob[2], bd2_ = db[0] * db[0] + db[1] * db[1] + db[2] * db[2];
            float kp = (kx * db[0] + ky * db[1] + kz * db[2]) / bd2_;
            bool body = kx * kx + ky * ky + kz * kz - kp * kp * bd2_ <= 0.2209f;
            int best = -1, shell = -1;
            float bt = tmax, st = tmax;
            for (int i = 0; i < c->n; i++) {
                const struct mcat_prim *p = &c->prim[i];
                if (p->frame ? !head : !body)
                    continue;
                const float *oo = p->frame ? oh : ob, *dd = p->frame ? dh : db;
                float ox = (oo[0] - p->c[0]) * p->ir[0], oy = (oo[1] - p->c[1]) * p->ir[1], oz = (oo[2] - p->c[2]) * p->ir[2];
                float ddx = dd[0] * p->ir[0], ddy = dd[1] * p->ir[1], ddz = dd[2] * p->ir[2];
                float a = ddx * ddx + ddy * ddy + ddz * ddz, b = ox * ddx + oy * ddy + oz * ddz, cc = ox * ox + oy * oy + oz * oz - 1.0f;
                float disc = b * b - a * cc;
                if (disc > 0.0f) {
                    float t = (-b - sqrtf(disc)) / a;
                    if (t > 1e-3f && t < bt) {
                        bt = t;
                        best = i;
                    }
                }
                if (p->part == P_EAR || p->part == P_NOSE || (cc + 1.0f) - b * b / a > p->shell2)
                    continue;                                                   /* (no shell, or the ray passes too far from it) */
                /* the shell of loose letters round it (only needed where the surface itself is missed) */
                ox = (oo[0] - p->c[0]) * p->irs[0]; oy = (oo[1] - p->c[1]) * p->irs[1]; oz = (oo[2] - p->c[2]) * p->irs[2];
                ddx = dd[0] * p->irs[0]; ddy = dd[1] * p->irs[1]; ddz = dd[2] * p->irs[2];
                a = ddx * ddx + ddy * ddy + ddz * ddz; b = ox * ddx + oy * ddy + oz * ddz; cc = ox * ox + oy * oy + oz * oz - 1.0f;
                disc = b * b - a * cc;
                if (disc > 0.0f) {
                    float t = (-b - sqrtf(disc)) / a;
                    if (t > 1e-3f && t < st) {
                        st = t;
                        shell = i;
                    }
                }
            }
            /* the tail and the legs: sphere tracing through each tube, over the stretch where the ray is inside the box round it */
            float tube_t = 1e9f, tail_near = 1e9f, tail_near_t = 0.0f;
            int tube_hit = -1;
            for (int k = 0; k < MCAT_TUBES; k++) {
                const struct mcat_tube *tb = &c->tube[k];
                float ts = 0.0f, te = bt;
                for (int i = 0; i < 3 && ts < te; i++) {
                    if (fabsf(db[i]) < 1e-9f) {
                        if (ob[i] < tb->lo[i] || ob[i] > tb->hi[i])
                            te = -1.0f;
                    } else {
                        float a = (tb->lo[i] - ob[i]) / db[i], b = (tb->hi[i] - ob[i]) / db[i];
                        ts = fmaxf(ts, fminf(a, b));
                        te = fminf(te, fmaxf(a, b));
                    }
                }
                float tt = ts;
                for (int step = 0; step < 20 && tt < te; step++) {
                    float pt[3] = { ob[0] + tt * db[0], ob[1] + tt * db[1], ob[2] + tt * db[2] };
                    float d = tube_sdf(tb, pt);
                    if (d < tail_near) {
                        tail_near = d;
                        tail_near_t = tt;
                    }
                    if (d < 0.0015f) {
                        if (tt < tube_t) {
                            tube_t = tt;
                            tube_hit = k;
                        }
                        break;
                    }
                    tt += d * c->scale * 0.95f;                          /* (the ray's parameter is in metres of the world, the distance in the cat's own) */
                }
            }
            bool surface = false;
            if (tube_hit >= 0 && tube_t < bt) {
                const struct mcat_tube *tb = &c->tube[tube_hit];
                float pt[3] = { ob[0] + tube_t * db[0], ob[1] + tube_t * db[1], ob[2] + tube_t * db[2] }, e = 0.004f, g[3];
                for (int k = 0; k < 3; k++) {
                    float pp[3] = { pt[0], pt[1], pt[2] }, pm[3] = { pt[0], pt[1], pt[2] };
                    pp[k] += e;
                    pm[k] -= e;
                    g[k] = tube_sdf(tb, pp) - tube_sdf(tb, pm);
                }
                float gl = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
                if (gl > 1e-9f) {
                    g[0] /= gl; g[1] /= gl; g[2] /= gl;
                    shade_hit(c, o, dn, inv_f, 0, tb->part, tube_t, pt, g, rgb);
                    touched = surface = true;
                    bt = tube_t;
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
                touched = surface = true;
            }
            /* the fluffy edge: loose letters on the shell where the surface was missed (or lies behind the shell) */
            if (shell >= 0 && st < bt - 0.004f && st < tmax) {
                const struct mcat_prim *p = &c->prim[shell];
                const float *oo = p->frame ? oh : ob, *dd = p->frame ? dh : db;
                float q[3] = { oo[0] + st * dd[0], oo[1] + st * dd[1], oo[2] + st * dd[2] };
                if (fuzz_letter(c, st, inv_f, p->frame, q, dn, rgb))
                    touched = true;
            } else if (!surface && tail_near < SHELL && tail_near_t < tmax) {
                float q[3] = { ob[0] + tail_near_t * db[0], ob[1] + tail_near_t * db[1], ob[2] + tail_near_t * db[2] };
                if (fuzz_letter(c, tail_near_t, inv_f, 0, q, dn, rgb))
                    touched = true;
            }
            if (head) {                                                 /* the whiskers, in front of whatever is behind them */
                float wt = 0.0f, cov = whiskers(oh, dh, surface ? bt : tmax, inv_f, inv, &wt);
                if (cov > 0.02f) {
                    float att = 1.0f / (1.0f + 0.018f * wt * wt), k = cov * 0.9f;
                    rgb[0] = mixf(rgb[0], 250.0f * (0.35f + 0.65f * att), k);
                    rgb[1] = mixf(rgb[1], 240.0f * (0.35f + 0.65f * att), k);
                    rgb[2] = mixf(rgb[2], 190.0f * (0.35f + 0.65f * att), k);
                    touched = true;
                }
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

int mcat_spheres(const struct mcat *c, float out[][4], int max)
{
    int n = 0;
    if (!c->enabled)
        return 0;
    float S = c->scale;
    for (int i = 0; i < c->n && n < max; i++) {
        const struct mcat_prim *p = &c->prim[i];
        if (p->frame)
            continue;
        out[n][0] = c->pos[0] + S * (p->c[0] * c->cy + p->c[2] * c->sy);
        out[n][1] = c->pos[1] + S * p->c[1];
        out[n][2] = c->pos[2] + S * (-p->c[0] * c->sy + p->c[2] * c->cy);
        out[n][3] = S * (fmaxf(p->r[0], fmaxf(p->r[1], p->r[2])) + SHELL) + 0.004f;
        n++;
    }
    for (int k = 0; k < MCAT_TUBES; k++)                                     /* the tail and the legs */
        for (int i = 0; i < c->tube[k].n && n < max; i++) {
            const float *q = c->tube[k].p[i];
            out[n][0] = c->pos[0] + S * (q[0] * c->cy + q[2] * c->sy);
            out[n][1] = c->pos[1] + S * q[1];
            out[n][2] = c->pos[2] + S * (-q[0] * c->sy + q[2] * c->cy);
            out[n][3] = S * (q[3] + SHELL) + 0.004f;
            n++;
        }
    if (n < max) {                                                           /* the head: its parts, the whiskers and the glow, in one */
        float hc[3] = { 0.0f, 0.10f, 0.06f };
        float b[3] = { c->neck[0] + c->rh[0] * hc[0] + c->rh[1] * hc[1] + c->rh[2] * hc[2], c->neck[1] + c->rh[3] * hc[0] + c->rh[4] * hc[1] + c->rh[5] * hc[2],
                       c->neck[2] + c->rh[6] * hc[0] + c->rh[7] * hc[1] + c->rh[8] * hc[2] };
        out[n][0] = c->pos[0] + S * (b[0] * c->cy + b[2] * c->sy);
        out[n][1] = c->pos[1] + S * b[1];
        out[n][2] = c->pos[2] + S * (-b[0] * c->sy + b[2] * c->cy);
        out[n][3] = S * 0.235f + 0.06f;
        n++;
    }
    return n;
}

/* Tom's shadow on the floor: right under him, and a longer one that falls away from the key light (the window), as long as his height asks. */
float mcat_floor_shade(const struct mcat *c, float x, float z)
{
    if (!c->enabled)
        return 1.0f;
    float dx = x - c->pos[0], dz = z - c->pos[2];
    float reach = (c->sh_l * c->scale * 2.2f + 0.9f);
    if (dx * dx + dz * dz > reach * reach)
        return 1.0f;
    float inv = 1.0f / c->scale, lx = (dx * c->cy - dz * c->sy) * inv, lz = (dx * c->sy + dz * c->cy) * inv;
    float v = 1.0f, w = c->sh_w, l = c->sh_l;
    float a1 = lx * lx / (w * w) + (lz + 0.02f) * (lz + 0.02f) / (l * l);                                 /* right under it */
    if (a1 < 6.0f)
        v *= 1.0f - 0.88f * expf(-a1);
    float a2 = (lx * lx / (w * w) + lz * lz / (l * l)) * 0.27f;                                          /* round it */
    if (a2 < 6.0f)
        v *= 1.0f - 0.45f * expf(-a2);
    if (c->lstr > 0.05f && c->ldir[1] > 0.2f) {                                                         /* cast by the key light: an ellipse along its direction */
        float k = fminf(c->sh_h / c->ldir[1], 1.1f);                                                    /* (how far the top of him is thrown) */
        float ux = -c->ldir[0], uz = -c->ldir[2], ul2 = ux * ux + uz * uz;
        if (ul2 > 1e-6f) {
            float il = 1.0f / sqrtf(ul2), sx = x - (c->pos[0] + ux * k * 0.5f), sz = z - (c->pos[2] + uz * k * 0.5f);
            ux *= il; uz *= il;
            float al = sx * ux + sz * uz, ac = -sx * uz + sz * ux, ax = k * 0.5f, ww = 0.9f * w * c->scale;
            float a3 = al * al / (ax * ax + 0.02f) + ac * ac / (ww * ww);
            if (a3 < 6.0f)
                v *= 1.0f - 0.82f * expf(-a3) * sat(c->lstr);
        }
    }
    return v < 0.0f ? 0.0f : v;
}
