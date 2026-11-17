/* matrixturtle: a map turtle swimming through the Matrix room. See matrixturtle.h. */
#pragma GCC optimize("O3", "fast-math")
#include "matrixturtle.h"
#include "matrixcat.h"

#include <math.h>
#include <string.h>

#define CELL 0.018f                     /* one letter of the turtle's skin, in metres */
#define SHELL_A 0.215f                  /* the carapace: half width ... */
#define SHELL_B 0.270f                  /* ... and half length */
#define DOME_H 0.140f                   /* how high the carapace rises above its rim */
#define PLAST_H 0.072f                  /* and how deep the plastron goes below it */
#define STROKE_T 2.1f                   /* one stroke of the paddles, seconds */
#define BUBBLE_PERIOD 7.0f              /* a few bubbles every so often */
#define BUBBLE_LIFE 5.0f

enum { P_DOME, P_PLASTRON, P_KNOB, P_RIM, P_SKIN, P_SKULL, P_PADDLE, P_TAIL };

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

static float vlen(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static void vnorm(float v[3])
{
    float l = vlen(v);
    if (l > 1e-9f) {
        v[0] /= l; v[1] /= l; v[2] /= l;
    }
}
static void vcross(const float a[3], const float b[3], float out[3])
{
    float r[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    memcpy(out, r, sizeof(r));
}
/* y = M x for a row major 3 x 3 matrix. */
static void mat_vec(const float m[9], const float x[3], float y[3])
{
    float r[3] = { m[0] * x[0] + m[1] * x[1] + m[2] * x[2], m[3] * x[0] + m[4] * x[1] + m[5] * x[2], m[6] * x[0] + m[7] * x[1] + m[8] * x[2] };
    memcpy(y, r, sizeof(r));
}
/* y = M^T x */
static void mat_tvec(const float m[9], const float x[3], float y[3])
{
    float r[3] = { m[0] * x[0] + m[3] * x[1] + m[6] * x[2], m[1] * x[0] + m[4] * x[1] + m[7] * x[2], m[2] * x[0] + m[5] * x[1] + m[8] * x[2] };
    memcpy(y, r, sizeof(r));
}
static void mat_mul(const float a[9], const float b[9], float out[9])
{
    float r[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    memcpy(out, r, sizeof(r));
}
/* The matrix whose columns are the three axes. */
static void mat_cols(const float x[3], const float y[3], const float z[3], float m[9])
{
    for (int i = 0; i < 3; i++) {
        m[i * 3] = x[i]; m[i * 3 + 1] = y[i]; m[i * 3 + 2] = z[i];
    }
}

/* ---- the swimming -------------------------------------------------------------------------------------------------------------- */

static uint32_t g_seed;
static float g_ph[8];
static float g_dir = 1.0f;
static uint32_t g_ph_for = 0xffffffffu;

static void phases(void)
{
    if (g_ph_for == g_seed)
        return;
    g_ph_for = g_seed;
    if (g_seed == 0) {
        static const float k[8] = { 0.4f, 1.3f, 2.2f, 0.9f, 0.7f, 3.9f, 1.1f, 4.4f };
        memcpy(g_ph, k, sizeof(g_ph));
        g_dir = 1.0f;
        return;
    }
    for (int i = 0; i < 8; i++)
        g_ph[i] = frand(g_seed * 2654435761u + (uint32_t)i * 40503u + 17u) * 6.2831853f;
    g_dir = (h32(g_seed + 77u) & 1u) ? 1.0f : -1.0f;
}

void mturtle_set_seed(uint32_t seed)
{
    g_seed = seed;
}

/* The slow loop of the upper half of the room, before it gives way to Tom. th always grows (its rate is 0.19 +- 0.04 rad/s), so the turtle never stops or
 * doubles back on itself. */
static void swim_base(float t, float p[3])
{
    phases();
    float th = 0.09f * t + 0.25f * sinf(0.037f * t + g_ph[0]);
    float rr = 1.0f + 0.15f * sinf(2.0f * th + g_ph[1]) + 0.06f * sinf(3.0f * th + g_ph[2]);
    float a = g_dir * th + g_ph[3];
    float cx = 0.50f * sinf(0.043f * t + g_ph[4]), cz = 0.10f + 0.45f * sinf(0.031f * t + g_ph[5]);
    p[0] = cx + 2.40f * rr * cosf(a);
    p[2] = cz + 3.60f * rr * sinf(a);                                    /* the loop reaches the whole length of the room */
    p[1] = 1.68f + 0.26f * sinf(0.17f * t + g_ph[6]) + 0.09f * sinf(0.43f * t + g_ph[7]);
}

/* ---- resting on the floor ---------------------------------------------------------------------------------------------------- */

/* Time is cut into slots of SLOT seconds; in most of them Filip comes down to the floor, rests there (head in, hind legs out, or everything pulled in, one thing after
 * another) and rises again. The place is chosen from a row of spots on the floor that Tom does not come near for the whole of that time (Tom's own day is a pure
 * function of time, so we can look ahead): a slot where no spot is free has no rest. */
#define SLOT 75.0f
#define DESC 8.0f                       /* going down ... */
#define ASC 6.0f                        /* ... and up again */
#define REST_Y 0.065f                   /* the middle of the shell when it lies on the floor */

struct slot { uint32_t seed; int k; bool have, active; float w0, d, spot[2], yaw; uint32_t hk; };
struct rest { float r, tau, d; bool active; float spot[2], yaw; uint32_t hk; };

static struct slot g_slots[4];
static int g_slot_next;

static void slot_make(int k, struct slot *s)
{
    memset(s, 0, sizeof(*s));
    s->seed = g_seed;
    s->k = k;
    s->have = true;
    if (k < 0)
        return;
    uint32_t hk = h32((uint32_t)k * 2246822519u + g_seed * 374761393u + 11u);
    s->hk = hk;
    if (frand(hk) > 0.7f)
        return;
    s->d = 22.0f + 12.0f * frand(hk + 2u);
    for (int attempt = 0; attempt < 3; attempt++) {                      /* a few starting times: Tom is often in the way of the first */
        s->w0 = (float)k * SLOT + 12.0f + 14.0f * frand(hk + 1u + 10u * (uint32_t)attempt);
        float t1 = s->w0 + DESC + s->d + ASC, tx[220][3];
        int n = (int)((t1 - s->w0) / 0.25f) + 1;
        if (n > 220)
            n = 220;
        for (int i = 0; i < n; i++)
            mturtle_tom_centre(s->w0 + 0.25f * (float)i, tx[i]);
        uint32_t start = hk % 72u;
        for (uint32_t c = 0; c < 72u; c++) {
            uint32_t idx = (start + c * 11u) % 72u;
            float x = -3.0f + 0.75f * (float)(idx % 9u), z = -4.6f + 1.4f * (float)(idx / 9u);          /* (the whole floor) */
            bool ok = true;
            for (int i = 0; i < n && ok; i++) {                          /* the whole way down, the rest and the way up stay clear of Tom (the swim loop is a pure function too) */
                float ti = s->w0 + 0.25f * (float)i, u = ti - s->w0, r = u < DESC ? sat(u / DESC) : u < DESC + s->d ? 1.0f : 1.0f - sat((u - DESC - s->d) / ASC);
                float e = sstep(0.0f, 1.0f, r), bb[3];
                swim_base(ti, bb);
                float px = mixf(bb[0], x, e), py = mixf(bb[1], REST_Y, e), pz = mixf(bb[2], z, e);
                float dx = px - tx[i][0], dy = py - tx[i][1], dz = pz - tx[i][2];
                ok = dx * dx + dy * dy + dz * dz >= (MTURTLE_SAFE + 0.35f) * (MTURTLE_SAFE + 0.35f);
            }
            if (ok) {
                s->active = true;
                s->spot[0] = x;
                s->spot[1] = z;
                s->yaw = 3.1415927f + (frand(hk + 3u) * 2.0f - 1.0f) * 1.0f;               /* (turned towards the room, so that the head can be seen going in) */
                return;
            }
        }
    }
}

static void rest_at(float t, struct rest *r)
{
    int k = (int)floorf(t / SLOT);
    struct slot *s = NULL;
    for (int i = 0; i < 4; i++)
        if (g_slots[i].have && g_slots[i].k == k && g_slots[i].seed == g_seed)
            s = &g_slots[i];
    if (!s) {
        s = &g_slots[g_slot_next];
        g_slot_next = (g_slot_next + 1) & 3;
        slot_make(k, s);
    }
    memset(r, 0, sizeof(*r));
    if (!s->active)
        return;
    float u = t - s->w0;
    if (u < 0.0f || u > DESC + s->d + ASC)
        return;
    r->active = true;
    r->r = u < DESC ? sat(u / DESC) : u < DESC + s->d ? 1.0f : 1.0f - sat((u - DESC - s->d) / ASC);
    r->tau = fmaxf(0.0f, u - DESC);
    r->d = s->d;
    r->spot[0] = s->spot[0]; r->spot[1] = s->spot[1];
    r->yaw = s->yaw;
    r->hk = s->hk;
}

/* What the resting turtle does at `tau` seconds into its rest: how far the head is pulled in (0..1), the front legs (0..1 pulled in) and the hind legs (-1 stretched
 * straight back .. 0 as they lie .. 1 pulled in). It goes from one state to the next in 1.6 s; the first is "as it lay down", the second always pulls something in. */
static void rest_params(const struct rest *r, float *head, float *fore, float *hind)
{
    static const float st[5][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, -1 }, { 1, 1, 1 }, { 0, 0, -1 } };
    int prev = 0, cur = 0;
    float start = 0.0f, tau = r->tau;
    for (int i = 0; i < 8; i++) {
        uint32_t h = h32(r->hk + 77u * (uint32_t)(i + 1));
        float dur = i == 0 ? 3.0f + 2.0f * frand(h) : 5.0f + 4.0f * frand(h);
        int next = i == 0 ? 0 : i == 1 ? 1 + (int)(h % 3u) : (int)((h >> 4) % 5u);
        if (i > 0 && next == cur)
            next = (next + 1 + (int)(h % 3u)) % 5;
        if (i > 0) {
            prev = cur;
            cur = next;
        }
        if (tau < start + dur || start + dur >= r->d)
            break;
        start += dur;
    }
    float k = sstep(0.0f, 1.6f, tau - start);
    float a[3] = { mixf(st[prev][0], st[cur][0], k), mixf(st[prev][1], st[cur][1], k), mixf(st[prev][2], st[cur][2], k) };
    if (tau > r->d) {                                                    /* it is about to rise: everything comes out again */
        float b = sstep(0.0f, 1.6f, tau - r->d);
        for (int i = 0; i < 3; i++)
            a[i] = mixf(a[i], 0.0f, b);
    }
    *head = a[0]; *fore = a[1]; *hind = a[2];
}

void mturtle_tom_centre(float t, float out[3])
{
    struct mcat_state st;
    mcat_state(t, &st);
    out[0] = st.x;
    out[1] = 0.45f + st.air;                                             /* the middle of the cat's sphere (0.36 * its scale) */
    out[2] = st.z;
}

/* Where the middle of the turtle is: the loop, kept at least MTURTLE_SAFE from the middle of Tom (a smooth maximum of the distance and the safe one, so the turtle
 * is pushed away from him gently and never nearer), and inside the room. */
void mturtle_where(float t, float out[3])
{
    float p[3], tc[3], q[3];
    swim_base(t, p);
    struct rest rs;
    rest_at(t, &rs);
    if (rs.active) {                                                     /* going down to the spot, lying there, coming up again */
        float e = sstep(0.0f, 1.0f, rs.r);
        p[0] = mixf(p[0], rs.spot[0], e);
        p[1] = mixf(p[1], REST_Y, e);
        p[2] = mixf(p[2], rs.spot[1], e);
    }
    memcpy(q, p, sizeof(q));
    mturtle_tom_centre(t, tc);
    float d[3] = { p[0] - tc[0], p[1] - tc[1], p[2] - tc[2] };
    float dist = vlen(d);
    if (dist < 1e-3f) {
        d[0] = 0.0f; d[1] = 1.0f; d[2] = 0.0f;
        dist = 1.0f;
    } else {
        d[0] /= dist; d[1] /= dist; d[2] /= dist;
    }
    const float W = 0.30f;
    float nd = 0.5f * (dist + MTURTLE_SAFE) + sqrtf(0.25f * (dist - MTURTLE_SAFE) * (dist - MTURTLE_SAFE) + W * W);
    for (int i = 0; i < 3; i++)
        p[i] = tc[i] + d[i] * nd;
    float lie = sstep(0.9f, 1.0f, rs.active ? rs.r : 0.0f);               /* lying: not pushed about by Tom (the spot was chosen to be far from him) */
    for (int i = 0; i < 3; i++)
        p[i] = mixf(p[i], q[i], lie);
    out[0] = clampf(p[0], -3.40f, 3.40f);
    out[1] = clampf(p[1], rs.active ? 0.05f : 1.00f, 2.55f);
    out[2] = clampf(p[2], -5.0f, 5.25f);
}

/* ---- building the turtle ------------------------------------------------------------------------------------------------------- */

/* An ellipsoid with the axes `rot` (columns, in the turtle's frame) at c (turtle frame). */
static void add_prim(struct mturtle *m, int part, const float c[3], const float rot[9], float rx, float ry, float rz, int dir, float ycut)
{
    if (m->n >= MTURTLE_PRIMS)
        return;
    struct mturtle_prim *p = &m->prim[m->n++];
    float rw[9], cw[3];
    mat_mul(m->m, rot, rw);
    mat_vec(m->m, c, cw);
    p->c[0] = m->pos[0] + cw[0]; p->c[1] = m->pos[1] + cw[1]; p->c[2] = m->pos[2] + cw[2];
    memcpy(p->rw, rw, sizeof(rw));
    p->r[0] = rx; p->r[1] = ry; p->r[2] = rz;
    p->ir[0] = 1.0f / rx; p->ir[1] = 1.0f / ry; p->ir[2] = 1.0f / rz;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            p->a[i * 3 + j] = p->ir[i] * rw[j * 3 + i];
    p->rad = fmaxf(rx, fmaxf(ry, rz));
    p->dir = (int8_t)dir;
    p->ycut = ycut;
    p->part = (uint8_t)part;
}

static const float k_id[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

/* A segment from p0 to p1 as a flattened ellipsoid: `rw` across (its x), `rt` thick (its y), the length along the axis. `up` says which way the thickness points. */
static void add_seg(struct mturtle *m, int part, const float p0[3], const float p1[3], const float up[3], float rw, float rt, float stretch)
{
    float z[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
    float len = vlen(z);
    if (len < 1e-5f)
        return;
    z[0] /= len; z[1] /= len; z[2] /= len;
    float x[3], y[3];
    vcross(up, z, x);
    if (vlen(x) < 1e-4f) {                                               /* the segment points along `up`: any x will do */
        float alt[3] = { 1.0f, 0.0f, 0.0f };
        vcross(alt, z, x);
    }
    vnorm(x);
    vcross(z, x, y);
    float rot[9], c[3] = { 0.5f * (p0[0] + p1[0]), 0.5f * (p0[1] + p1[1]), 0.5f * (p0[2] + p1[2]) };
    mat_cols(x, y, z, rot);
    add_prim(m, part, c, rot, rw, rt, 0.5f * len * stretch, 0, 0.0f);
}

static void dir_az_el(float sgn, float az, float el, float out[3])
{
    out[0] = sgn * cosf(az) * cosf(el);
    out[1] = sinf(el);
    out[2] = sinf(az) * cosf(el);
}

/* One paddle: upper limb and the webbed hand, stroking with phase `psi`. az0 / el0 = where it points at rest, A / B = how far it sweeps and lifts. */
static void paddle(struct mturtle *m, float sgn, const float sh0[3], float l1, float l2, float az0, float el0, float A, float B, float psi, float amp, float w1, float w2, float pull)
{
    float d1[3], d2[3], e[3], w[3], sh[3] = { sh0[0], sh0[1], sh0[2] };
    float az1 = az0 + A * amp * sinf(psi), el1 = el0 + B * amp * cosf(psi);
    float az2 = az0 - 0.10f + 1.25f * A * amp * sinf(psi - 0.9f), el2 = el0 - 0.05f + 1.15f * B * amp * cosf(psi - 0.9f);
    if (pull < 0.0f) {                                                   /* stretched straight out behind it, flat */
        float k = -pull;
        az1 = mixf(az1, -1.42f, k); el1 = mixf(el1, -0.03f, k);
        az2 = mixf(az2, -1.32f, k); el2 = mixf(el2, -0.03f, k);
        l1 *= 1.0f + 0.15f * k; l2 *= 1.0f + 0.25f * k;
    } else if (pull > 0.0f) {                                            /* pulled in under the shell: a sliver at the rim is all that shows */
        sh[0] *= 1.0f - 0.32f * pull;
        l1 *= 1.0f - 0.85f * pull; l2 *= 1.0f - 0.86f * pull;
    }
    dir_az_el(sgn, az1, el1, d1);
    for (int i = 0; i < 3; i++)
        e[i] = sh[i] + d1[i] * l1;
    dir_az_el(sgn, az2, el2, d2);
    for (int i = 0; i < 3; i++)
        w[i] = e[i] + d2[i] * l2;
    static const float up[3] = { 0.0f, 1.0f, 0.0f };
    if (l1 > 0.012f)
        add_seg(m, P_SKIN, sh, e, up, w1, 0.5f * w1 + 0.004f, 1.15f);
    if (l2 > 0.012f)
        add_seg(m, P_PADDLE, e, w, up, w2, 0.012f, 1.12f);
}

void mturtle_pose(struct mturtle *m, float t, const uint8_t *const *glyph, int nglyph, const float ldir[3], const float lcol[3], float lstr)
{
    memset(m->prim, 0, sizeof(m->prim));
    m->n = 0;
    m->enabled = true;
    m->t = t;
    m->glyph = glyph;
    m->nglyph = nglyph;
    memcpy(m->ldir, ldir, sizeof(m->ldir));
    memcpy(m->lcol, lcol, sizeof(m->lcol));
    m->lstr = lstr;

    const float h = 0.4f;
    float pm[3], p0[3], pp[3];
    mturtle_where(t - h, pm);
    mturtle_where(t, p0);
    mturtle_where(t + h, pp);
    float v1[3] = { (p0[0] - pm[0]) / h, (p0[1] - pm[1]) / h, (p0[2] - pm[2]) / h };
    float v2[3] = { (pp[0] - p0[0]) / h, (pp[1] - p0[1]) / h, (pp[2] - p0[2]) / h };
    float v[3] = { 0.5f * (v1[0] + v2[0]), 0.5f * (v1[1] + v2[1]), 0.5f * (v1[2] + v2[2]) };
    float hs = sqrtf(v[0] * v[0] + v[2] * v[2]);
    m->speed = vlen(v);
    float yaw1 = atan2f(v1[0], v1[2]), yaw2 = atan2f(v2[0], v2[2]);
    float dyaw = fmodf(yaw2 - yaw1 + 9.4247780f, 6.2831853f) - 3.1415927f;
    float yaw_rate = dyaw / h;
    struct rest rs;
    rest_at(t, &rs);
    float down = rs.active ? sstep(0.7f, 1.0f, rs.r) : 0.0f;              /* how far it has settled on the floor: no swimming left in it */
    float yaw_swim = hs > 1e-3f ? atan2f(v[0], v[2]) : yaw2;
    if (down > 0.0f) {                                                   /* it turns to the way it lies, the short way round */
        float dy = fmodf(rs.yaw - yaw_swim + 9.4247780f, 6.2831853f) - 3.1415927f;
        yaw_swim += dy * down;
    }
    m->yaw = yaw_swim;
    m->pitch = clampf(0.9f * atan2f(v[1], fmaxf(hs, 0.05f)), -0.45f, 0.45f) * (1.0f - down);
    float swing = 0.045f * sinf(6.2831853f * t / STROKE_T) * (1.0f - down);    /* the body sways with the strokes */
    m->yaw += swing;
    m->roll = (clampf(1.4f * yaw_rate, -0.35f, 0.35f) + 0.04f * cosf(6.2831853f * t / STROKE_T)) * (1.0f - down);
    m->pos[0] = p0[0]; m->pos[1] = p0[1]; m->pos[2] = p0[2];
    m->stroke = (0.35f + 0.65f * sat((m->speed - 0.12f) / 0.25f)) * (1.0f - sstep(0.5f, 0.95f, rs.active ? rs.r : 0.0f));
    float head_in = 0.0f, fore_in = 0.0f, hind_in = 0.0f;
    if (rs.active && rs.r >= 1.0f - 1e-6f)
        rest_params(&rs, &head_in, &fore_in, &hind_in);
    else if (rs.active && rs.tau > 0.0f)                                  /* (rising: the params of the end of the rest, fading with the rise) */
        rest_params(&rs, &head_in, &fore_in, &hind_in);
    m->head_in = head_in; m->fore_in = fore_in; m->hind_in = hind_in;
    m->rest = rs.active ? rs.r : 0.0f;

    float f[3] = { sinf(m->yaw) * cosf(m->pitch), sinf(m->pitch), cosf(m->yaw) * cosf(m->pitch) };
    float r[3] = { cosf(m->yaw), 0.0f, -sinf(m->yaw) }, up[3];
    vcross(f, r, up);
    float cr = cosf(m->roll), sr = sinf(m->roll);
    float r2[3] = { r[0] * cr - up[0] * sr, r[1] * cr - up[1] * sr, r[2] * cr - up[2] * sr };
    float u2[3] = { up[0] * cr + r[0] * sr, up[1] * cr + r[1] * sr, up[2] * cr + r[2] * sr };
    mat_cols(r2, u2, f, m->m);

    float phi = 6.2831853f * t / STROKE_T, amp = m->stroke;
    const float Y[3] = { 0.0f, 1.0f, 0.0f };

    /* the shell: the dome above the rim plane, the plastron below it (the same equator, so it is closed) */
    float c0[3] = { 0.0f, 0.02f, 0.0f };
    add_prim(m, P_DOME, c0, k_id, SHELL_A, DOME_H, SHELL_B, 1, 0.0f);
    add_prim(m, P_PLASTRON, c0, k_id, SHELL_A, PLAST_H, SHELL_B, -1, 0.0f);
    for (int k = 0; k < 5; k++) {                                        /* the keel's knobs, along the curve of the dome */
        float z = 0.16f - 0.08f * (float)k, yd = 0.02f + DOME_H * sqrtf(fmaxf(0.0f, 1.0f - (z / SHELL_B) * (z / SHELL_B)));
        float c[3] = { 0.0f, yd + 0.002f, z };
        add_prim(m, P_KNOB, c, k_id, 0.022f, 0.020f, 0.032f, 0, 0.0f);
    }
    for (int k = -3; k <= 3; k++) {                                      /* the saw edge at the back */
        float b = 0.30f * (float)k, c[3] = { SHELL_A * 0.97f * sinf(b), 0.022f, -SHELL_B * 0.97f * cosf(b) };
        float ax[3] = { sinf(b), 0.0f, -cosf(b) }, x[3], y[3];
        vcross(Y, ax, x);
        vnorm(x);
        vcross(ax, x, y);
        float rot[9];
        mat_cols(x, y, ax, rot);
        add_prim(m, P_RIM, c, rot, 0.030f, 0.026f, 0.032f, 0, 0.0f);
    }

    /* the neck and the head: the head leads into a turn and looks about */
    float hy = (clampf(0.9f * yaw_rate, -0.5f, 0.5f) + 0.28f * sinf(0.37f * t + 1.0f)) * (1.0f - 0.7f * down) * (1.0f - head_in);
    float hp = (0.10f + 0.12f * sinf(0.53f * t + 2.0f)) * (1.0f - 0.6f * down) * (1.0f - head_in);
    float ext = 0.88f + 0.10f * sinf(0.29f * t);
    m->head_yaw = hy;
    m->head_pitch = hp;
    float hd[3] = { sinf(hy) * cosf(hp), sinf(hp), cosf(hy) * cosf(hp) }, hd1[3] = { sinf(0.5f * hy) * cosf(0.5f * hp + 0.04f), sinf(0.5f * hp + 0.04f), cosf(0.5f * hy) * cosf(0.5f * hp + 0.04f) };
    float n0[3] = { 0.0f, 0.03f, 0.21f }, n1[3], n2[3], hc[3];
    float off = mixf(0.24f * ext, -0.075f, head_in), neck = fmaxf(off - 0.04f, 0.0f), tail = fminf(off, 0.04f);   /* how far the head sits ahead of the neck's root: it goes back inside the shell */
    for (int i = 0; i < 3; i++) {
        n1[i] = n0[i] + hd1[i] * 0.5f * neck;
        n2[i] = n1[i] + hd[i] * 0.5f * neck;
        hc[i] = n2[i] + hd[i] * tail;
    }
    if (neck > 0.02f) {
        add_seg(m, P_SKIN, n0, n1, Y, 0.046f, 0.040f, 1.25f);
        add_seg(m, P_SKIN, n1, n2, Y, 0.040f, 0.036f, 1.25f);
    }
    float hx[3], hyv[3], hrot[9];
    vcross(Y, hd, hx);
    vnorm(hx);
    vcross(hd, hx, hyv);
    mat_cols(hx, hyv, hd, hrot);
    add_prim(m, P_SKULL, hc, hrot, 0.047f, 0.041f, 0.062f, 0, 0.0f);
    float sn[3] = { hc[0] + hd[0] * 0.060f - hyv[0] * 0.007f, hc[1] + hd[1] * 0.060f - hyv[1] * 0.007f, hc[2] + hd[2] * 0.060f - hyv[2] * 0.007f };
    add_prim(m, P_SKULL, sn, hrot, 0.029f, 0.024f, 0.040f, 0, 0.0f);

    /* the paddles: a front one and the hind one on the other side stroke together */
    for (int side = 0; side < 2; side++) {
        float sgn = side ? 1.0f : -1.0f, pf = side ? 3.1415927f : 0.0f, ph = side ? 0.0f : 3.1415927f;
        float shf[3] = { sgn * 0.170f, -0.006f, 0.150f }, shh[3] = { sgn * 0.140f, -0.012f, -0.180f };
        paddle(m, sgn, shf, 0.080f, 0.140f, -0.10f, -0.10f, 0.80f, 0.42f, phi + pf, amp, 0.038f, 0.040f, fore_in);
        paddle(m, sgn, shh, 0.070f, 0.125f, -1.00f, -0.05f, 0.55f, 0.36f, phi + ph, amp, 0.034f, 0.046f, hind_in);
    }
    float tb0[3] = { 0.0f, -0.004f, -SHELL_B + 0.01f }, tb1[3] = { 0.035f * sinf(phi), -0.012f, -SHELL_B - 0.09f };
    if (hind_in < 0.6f)
        add_seg(m, P_TAIL, tb0, tb1, Y, 0.030f, 0.026f, 1.3f);

    /* a sphere round it, and its bubbles */
    mat_vec(m->m, (float[3]){ 0.0f, 0.0f, 0.10f }, m->centre);
    for (int i = 0; i < 3; i++)
        m->centre[i] += m->pos[i];
    m->radius = 0.62f;

    m->nbub = 0;
    memset(m->bub, 0, sizeof(m->bub));
    int k1 = (int)floorf(t / BUBBLE_PERIOD);
    for (int k = k1 - 1; k <= k1; k++) {
        if (k < 0)
            continue;
        float tk = (float)k * BUBBLE_PERIOD + 2.0f * frand((uint32_t)k * 7919u + g_seed + 3u);
        int nb = 2 + (int)(h32((uint32_t)k * 31u + g_seed) % 3u);
        for (int i = 0; i < nb && m->nbub < MTURTLE_BUBBLES; i++) {
            float tb = tk + 0.35f * (float)i, age = t - tb;
            if (age < 0.0f || age > BUBBLE_LIFE)
                continue;
            struct rest rb;
            rest_at(tb, &rb);
            if (rb.active && rb.r > 0.3f)                                 /* (nothing to blow with the head inside the shell, down on the floor) */
                continue;
            float a0[3], a1[3];
            mturtle_where(tb, a0);
            mturtle_where(tb + 0.3f, a1);
            float fd[3] = { a1[0] - a0[0], 0.0f, a1[2] - a0[2] };
            vnorm(fd);
            struct mturtle_bubble *b = &m->bub[m->nbub];
            uint32_t hs2 = h32((uint32_t)k * 131u + (uint32_t)i * 17u + g_seed);
            float wob = 0.03f * sinf(2.6f * age + (float)(hs2 & 255) * 0.05f);
            b->c[0] = a0[0] + fd[0] * 0.52f + wob + 0.05f * ((float)((hs2 >> 8) & 255) / 255.0f - 0.5f);
            b->c[1] = a0[1] - 0.01f + 0.32f * age + 0.04f * age * age * 0.1f;
            b->c[2] = a0[2] + fd[2] * 0.52f + 0.03f * cosf(2.1f * age + (float)((hs2 >> 16) & 255) * 0.05f);
            b->r = (0.013f + 0.008f * (float)((hs2 >> 4) & 3)) * (1.0f + 0.12f * age);
            b->age = age;
            if (b->c[1] < 2.95f)
                m->nbub++;
        }
    }
}

/* ---- the ray ----------------------------------------------------------------------------------------------------------------- */

/* The shell's map: lines on the carapace at the point (x, z) of the turtle's frame (a cell's middle): 0 = plain scute, 1 = a seam between scutes, 2 = a ring of
 * growth inside one, 3 = the pale edge. */
static int shell_map(float x, float z)
{
    static const float cs[13][2] = {
        { 0.0f, 0.66f }, { 0.0f, 0.32f }, { 0.0f, -0.02f }, { 0.0f, -0.36f }, { 0.0f, -0.70f },
        { -0.58f, 0.52f }, { -0.60f, 0.14f }, { -0.58f, -0.26f }, { -0.50f, -0.62f },
        { 0.58f, 0.52f }, { 0.60f, 0.14f }, { 0.58f, -0.26f }, { 0.50f, -0.62f },
    };
    float u = x / SHELL_A, v = z / SHELL_B, rho = sqrtf(u * u + v * v);
    if (rho > 0.94f)
        return 3;
    if (rho > 0.80f) {                                                   /* the marginals: a ring of small scutes round the edge, 11 on a side */
        if (rho < 0.855f)
            return 1;
        float ang = atan2f(fabsf(u), -v) * (11.0f / 3.14159265f);
        return fabsf(ang - floorf(ang + 0.5f)) < 0.17f ? 1 : 0;
    }
    float d1 = 1e9f, d2 = 1e9f;
    for (int i = 0; i < 13; i++) {
        float dx = u - cs[i][0], dz = v - cs[i][1], d = sqrtf(dx * dx + dz * dz);
        if (d < d1) {
            d2 = d1;
            d1 = d;
        } else if (d < d2) {
            d2 = d;
        }
    }
    if (d2 - d1 < 0.060f)
        return 1;
    return fabsf(fmodf(d1, 0.24f) - 0.12f) < 0.030f && d1 > 0.12f ? 2 : 0;
}

/* The plastron's dark pattern (drawn from the exact point, so thin lines stay thin inside the letters), as a map turtle has it: the plastron is cream-yellow and the dark pigment follows the SEAMS between its shields - a thin line along
 * each seam (the midline, and the transverse ones between gulars / humerals / pectorals / abdominals / femorals / anals, which wander a little), with fingers and
 * smudges that thin out away from the line - and a dark spot at every seam of the small marginal shields at the rim. (x, z) is the point in the turtle's
 * frame. Returns 0 = cream, 1 = dark. */
static float hash01(uint32_t a, uint32_t b) { return (float)(h32(a * 2246822519u + b * 3266489917u + 0x9e3779b9u) & 1023u) / 1023.0f; }

static int plastron_dark(float x, float z)
{
    float u = x / SHELL_A, v = z / SHELL_B, rho = sqrtf(u * u + v * v), au = fabsf(u);
    if (rho > 0.97f)
        return 0;                                                        /* the very edge is pale */
    if (rho > 0.86f) {                                                   /* the marginals seen from below: a small dark spot at each seam between two of them */
        float ang = atan2f(au, -v) * (12.0f / 3.14159265f);
        return fabsf(ang - floorf(ang + 0.5f)) < 0.13f && rho > 0.88f && rho < 0.945f;
    }
    /* the midline between the left and the right shields: thin, wandering, with short fingers running out sideways */
    if (fabsf(v) < 0.80f) {
        float wander = 0.030f * sinf(v * 11.0f + 0.6f) + 0.018f * sinf(v * 29.0f + 1.0f);
        if (fabsf(u - wander) < 0.016f + 0.008f * sinf(v * 37.0f))
            return 1;
        float cell = floorf(v / 0.07f), c = (cell + 0.5f) * 0.07f;
        float len = (hash01((uint32_t)(int)cell, 7u) < 0.7f) ? 0.04f + 0.16f * hash01((uint32_t)(int)cell, 8u) : 0.0f;
        float du = fabsf(u - wander), slope = hash01((uint32_t)(int)cell, 9u) * 0.9f - 0.45f;
        if (du < len && fabsf(v - c - slope * du) < 0.012f * (1.0f - du / len) + 0.003f)      /* (a finger slants and tapers) */
            return 1;
    }
    /* the transverse seams: each one a thin zigzag across, with fingers running forwards and back off it */
    static const float zs[5] = { 0.60f, 0.36f, 0.02f, -0.34f, -0.60f };
    for (int i = 0; i < 5; i++) {
        float len = i == 2 ? 0.88f : 0.74f;
        if (au > len)
            continue;
        float vz = zs[i] + 0.06f * au + 0.022f * sinf(au * 23.0f + (float)i * 1.9f) + 0.012f * sinf(au * 51.0f + (float)i);
        if (fabsf(v - vz) < 0.014f + 0.008f * sinf(au * 41.0f + (float)i))
            return 1;
        float cell = floorf(au / 0.075f), c = (cell + 0.5f) * 0.075f;
        float r = hash01((uint32_t)(int)cell + 31u * (uint32_t)i, 3u);
        float fl = r < 0.65f ? 0.03f + 0.15f * hash01((uint32_t)(int)cell + 31u * (uint32_t)i, 4u) : 0.0f;
        float dir = hash01((uint32_t)(int)cell + 31u * (uint32_t)i, 5u) < 0.5f ? -1.0f : 1.0f;
        float dv = (v - vz) * dir, slope = hash01((uint32_t)(int)cell + 31u * (uint32_t)i, 6u) * 1.2f - 0.6f;
        if (dv > 0.0f && dv < fl && fabsf(au - c - slope * dv) < 0.012f * (1.0f - dv / fl) + 0.003f)
            return 1;
    }
    return 0;
}

/* The colour of a point of the turtle (letters, light): `part` it is on, qu the point on the unit sphere of its ellipsoid, nw the normal in the world, pt the point in
 * the turtle's frame, nt the normal in the turtle's frame, bt the distance from the camera. */
static void shade_hit(const struct mturtle *m, int part, int pi, const float qu[3], const float nw[3], const float pt[3], const float nt[3], float bt, float inv_f, const float dn[3], float rgb[3])
{
    (void)pi;
    float ndv = sat(-(nw[0] * dn[0] + nw[1] * dn[1] + nw[2] * dn[2]));
    int ci[3] = { (int)floorf(pt[0] / CELL), (int)floorf(pt[1] / CELL), (int)floorf(pt[2] / CELL) };
    uint32_t hc = h32((uint32_t)ci[0] * 73856093u ^ (uint32_t)ci[1] * 19349663u ^ (uint32_t)ci[2] * 83492791u ^ 0x7a31u);
    int epoch = (int)floorf(m->t * 1.2f + (float)((hc >> 12) & 255) / 256.0f * 4.0f);
    uint32_t hg = h32(hc + (uint32_t)epoch * 2246822519u);
    int ax = fabsf(nt[0]) >= fabsf(nt[1]) && fabsf(nt[0]) >= fabsf(nt[2]) ? 0 : fabsf(nt[1]) >= fabsf(nt[2]) ? 1 : 2;
    int au = ax == 0 ? 1 : 0, av = ax == 2 ? 1 : 2;
    float fu = pt[au] / CELL - floorf(pt[au] / CELL), fv = pt[av] / CELL - floorf(pt[av] / CELL);
    const uint8_t *g = m->nglyph > 0 && m->glyph ? m->glyph[hg % (uint32_t)m->nglyph] : NULL;
    int gx = (int)(fu * 9.0f), gy = (int)(fv * 16.0f);
    float bit = (g && gx < 8 && gy < 16 && ((g[gy] >> gx) & 1)) ? 1.0f : 0.0f;
    float foot = bt * inv_f, gp = CELL / 16.0f;
    float mix = sat((foot / gp - 0.9f) * 0.6f) * 0.6f;
    float cov = 0.28f + 0.72f * (bit * (1.0f - mix) + 0.3f * mix);
    float cellb = 0.66f + 0.34f * frand(hc >> 3);
    float sparkle = ((hg >> 8) & 31) == 0 ? 1.0f : 0.0f;

    float cx = ((float)ci[0] + 0.5f) * CELL, cz = ((float)ci[2] + 0.5f) * CELL;       /* the cell's middle: the map is made of whole letters */
    float col[3], gain = 1.0f;
    switch (part) {
    case P_DOME: {
        int mp = shell_map(cx, cz);
        if (mp == 1) { col[0] = 0.92f; col[1] = 0.88f; col[2] = 0.30f; gain = 0.75f; }
        else if (mp == 2) { col[0] = 0.70f; col[1] = 0.74f; col[2] = 0.24f; gain = 0.7f; }
        else if (mp == 3) { col[0] = 0.84f; col[1] = 0.78f; col[2] = 0.30f; gain = 0.75f; }
        else { col[0] = 0.30f; col[1] = 0.28f; col[2] = 0.09f; gain = 0.85f; }
        break;
    }
    case P_PLASTRON:
        if (plastron_dark(pt[0], pt[2])) { col[0] = 0.34f; col[1] = 0.22f; col[2] = 0.06f; gain = 0.95f; }
        else { col[0] = 0.98f; col[1] = 0.88f; col[2] = 0.48f; gain = 1.05f; }
        break;
    case P_KNOB:
        col[0] = mixf(0.18f, 0.62f, sat(qu[1] * 1.6f)); col[1] = mixf(0.14f, 0.52f, sat(qu[1] * 1.6f)); col[2] = 0.06f; gain = 0.8f;
        break;
    case P_RIM:
        col[0] = 0.60f; col[1] = 0.56f; col[2] = 0.22f;
        break;
    case P_SKULL: {
        float aq = atan2f(qu[1], fabsf(qu[0]));                         /* the stripes run along the head */
        bool stripe = fabsf(aq - 0.45f) < 0.10f || fabsf(aq - 1.05f) < 0.08f || fabsf(aq - 2.0f) < 0.10f || fabsf(aq - 2.7f) < 0.09f;
        col[0] = 0.40f; col[1] = 0.46f; col[2] = 0.14f;
        if (stripe && qu[2] > -0.5f) { col[0] = 0.95f; col[1] = 0.88f; col[2] = 0.32f; }
        float ex = fabsf(qu[0]) - 0.70f, ey = qu[1] - 0.46f, ez = qu[2] - 0.55f;         /* the eye (dark, a yellow ring) ... */
        float de = sqrtf(ex * ex + ey * ey + ez * ez);
        if (de < 0.15f) { col[0] = 0.04f; col[1] = 0.05f; col[2] = 0.03f; }
        else if (de < 0.22f) { col[0] = 0.95f; col[1] = 0.86f; col[2] = 0.30f; }
        else if (de > 0.34f && de < 0.50f && qu[2] < ez - 0.06f) { col[0] = 1.00f; col[1] = 0.92f; col[2] = 0.34f; gain = 1.1f; }   /* ... and the crescent behind it */
        if (qu[1] < -0.55f) { col[0] = 0.90f; col[1] = 0.86f; col[2] = 0.50f; }       /* the chin */
        break;
    }
    case P_PADDLE:
    case P_TAIL:
    default: {
        float aq = atan2f(qu[1], qu[0]);
        bool stripe = sinf(3.0f * aq) > 0.86f && part != P_TAIL;
        col[0] = 0.38f; col[1] = 0.44f; col[2] = 0.13f;
        if (stripe) { col[0] = 0.92f; col[1] = 0.85f; col[2] = 0.30f; }
        if (part == P_PADDLE && qu[1] < -0.2f) { col[0] = 0.80f; col[1] = 0.78f; col[2] = 0.40f; }
        break;
    }
    }

    float att = 1.0f / (1.0f + 0.018f * bt * bt);
    float L = 0.30f + 0.62f * ndv;
    float key = m->lstr > 0.0f ? sat(nw[0] * m->ldir[0] + nw[1] * m->ldir[1] + nw[2] * m->ldir[2]) * m->lstr : 0.0f;
    L += 0.55f * key;
    float rim = powf(1.0f - ndv, 3.0f);
    float lum = 2.25f * att * L * cellb * cov * gain;
    for (int i = 0; i < 3; i++)
        rgb[i] = 255.0f * (col[i] * lum + 0.18f * col[i] * rim) + m->lcol[i] * 0.16f * key * att * cov;
    rgb[0] += 70.0f * sparkle * bit * att; rgb[1] += 255.0f * sparkle * bit * att; rgb[2] += 110.0f * sparkle * bit * att;       /* (a letter of the rain) */
}

bool mturtle_shade(const struct mturtle *m, const float o[3], const float dn[3], float tmax, float inv_f, float rgb[3])
{
    if (!m->enabled)
        return false;
    bool touched = false;
    float bt = tmax;
    float wx = m->centre[0] - o[0], wy = m->centre[1] - o[1], wz = m->centre[2] - o[2];
    float tp = wx * dn[0] + wy * dn[1] + wz * dn[2];
    float near2 = wx * wx + wy * wy + wz * wz - tp * tp;
    if (tp >= 0.0f && near2 <= m->radius * m->radius) {
        int best = -1;
        float bq[3] = { 0 };
        for (int i = 0; i < m->n; i++) {
            const struct mturtle_prim *p = &m->prim[i];
            float ex = p->c[0] - o[0], ey = p->c[1] - o[1], ez = p->c[2] - o[2];
            float tq = ex * dn[0] + ey * dn[1] + ez * dn[2];
            if (tq < -p->rad || ex * ex + ey * ey + ez * ez - tq * tq > p->rad * p->rad)
                continue;
            float d0[3] = { o[0] - p->c[0], o[1] - p->c[1], o[2] - p->c[2] }, oo[3], dd[3];
            mat_vec(p->a, d0, oo);
            mat_vec(p->a, dn, dd);
            float a = dd[0] * dd[0] + dd[1] * dd[1] + dd[2] * dd[2], b = oo[0] * dd[0] + oo[1] * dd[1] + oo[2] * dd[2], cc = oo[0] * oo[0] + oo[1] * oo[1] + oo[2] * oo[2] - 1.0f;
            float disc = b * b - a * cc;
            if (disc <= 0.0f)
                continue;
            float t = (-b - sqrtf(disc)) / a;
            if (t <= 1e-3f || t >= bt)
                continue;
            float q[3] = { oo[0] + t * dd[0], oo[1] + t * dd[1], oo[2] + t * dd[2] };
            if (p->dir > 0 && q[1] < p->ycut)
                continue;
            if (p->dir < 0 && q[1] > p->ycut)
                continue;
            bt = t;
            best = i;
            memcpy(bq, q, sizeof(bq));
        }
        if (best >= 0) {
            const struct mturtle_prim *p = &m->prim[best];
            float nl[3] = { bq[0] * p->ir[0], bq[1] * p->ir[1], bq[2] * p->ir[2] }, nw[3];
            vnorm(nl);
            mat_vec(p->rw, nl, nw);
            float hw[3] = { o[0] + bt * dn[0] - m->pos[0], o[1] + bt * dn[1] - m->pos[1], o[2] + bt * dn[2] - m->pos[2] }, pt[3], nt[3];
            mat_tvec(m->m, hw, pt);
            mat_tvec(m->m, nw, nt);
            shade_hit(m, p->part, best, bq, nw, pt, nt, bt, inv_f, dn, rgb);
            touched = true;
        }
    }
    for (int i = 0; i < m->nbub; i++) {                                   /* the bubbles: nearer than the turtle's surface they lie over it */
        const struct mturtle_bubble *b = &m->bub[i];
        float ex = b->c[0] - o[0], ey = b->c[1] - o[1], ez = b->c[2] - o[2];
        float tq = ex * dn[0] + ey * dn[1] + ez * dn[2], pd2 = ex * ex + ey * ey + ez * ez - tq * tq, r2 = b->r * b->r;
        if (tq <= 0.0f || pd2 >= r2)
            continue;
        float half = sqrtf(r2 - pd2), t = tq - half;
        if (t <= 1e-3f || t >= bt)
            continue;
        float nx = (o[0] + t * dn[0] - b->c[0]) / b->r, ny = (o[1] + t * dn[1] - b->c[1]) / b->r, nz = (o[2] + t * dn[2] - b->c[2]) / b->r;
        float ndv = sat(-(nx * dn[0] + ny * dn[1] + nz * dn[2])), rim = 1.0f - ndv;
        float att = 1.0f / (1.0f + 0.018f * t * t), al = sat(0.22f + 0.70f * rim * rim);
        float hl = sat((nx * -0.5f + ny * 0.7f + nz * -0.5f) * 3.0f - 2.0f);   /* a bright spot where the room's glow hits it */
        al = fmaxf(al, hl);
        float k = al * (0.5f + 0.5f * att);
        rgb[0] = mixf(rgb[0], 190.0f + 60.0f * hl, k);
        rgb[1] = mixf(rgb[1], 255.0f, k);
        rgb[2] = mixf(rgb[2], 215.0f + 40.0f * hl, k);
        touched = true;
    }
    (void)sstep;
    return touched;
}

float mturtle_floor_shade(const struct mturtle *m, float x, float z)
{
    if (!m->enabled)
        return 1.0f;
    float h = fmaxf(m->pos[1] - 0.05f, 0.0f), dx = x - m->pos[0], dz = z - m->pos[2];
    float s = sinf(m->yaw), c = cosf(m->yaw), u = dx * c - dz * s, v = dx * s + dz * c;       /* across and along the turtle */
    float ra = 0.25f + 0.28f * h, rb = 0.31f + 0.28f * h, q = u * u / (ra * ra) + v * v / (rb * rb);
    if (q > 5.0f)
        return 1.0f;
    return 1.0f - 0.72f / (1.0f + 4.0f * h * h) * expf(-1.4f * q);
}

int mturtle_spheres(const struct mturtle *m, float out[][4], int max)
{
    int n = 0;
    if (!m->enabled)
        return 0;
    for (int i = 0; i < m->n && n < max; i++) {
        out[n][0] = m->prim[i].c[0]; out[n][1] = m->prim[i].c[1]; out[n][2] = m->prim[i].c[2];
        out[n][3] = m->prim[i].rad;
        n++;
    }
    for (int i = 0; i < m->nbub && n < max; i++) {
        out[n][0] = m->bub[i].c[0]; out[n][1] = m->bub[i].c[1]; out[n][2] = m->bub[i].c[2];
        out[n][3] = m->bub[i].r;
        n++;
    }
    return n;
}
