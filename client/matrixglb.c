/* matrixglb: the cat made of a rigged .glb model. See matrixglb.h. */
#pragma GCC optimize("O3", "fast-math")
#include "matrixglb.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "glb.h"

#define BODY_LENGTH 1.05f               /* the rest pose is made this long in metres, whatever the model's units are (a Blender armature: centimetres under a scale of 0.01) */
#define EYE_UNITS 5.67e-5f              /* the radius of an eye in the model's units (of the cat this was made for) */
#define CELLM 0.040f                    /* one letter of the fur in metres */

struct mglb {
    struct glb *g;
    float scale;                        /* model units -> metres */
    float origin[3];                    /* the model's point that stands on the floor at the cat's place: the middle of the rest pose, at its feet */
    float *jm;                          /* nj * 16 */
    float *sp, *sn;                     /* skinned in model space (3 a vertex) */
    float *wp, *wn;                     /* in the world: metres, unit normals */
    float *sx, *sy, *sz;                /* projected: pixels, pixels, the depth along the camera's forward axis */
    /* the pose */
    float t, x, z, yaw, cy, sy_, walk;
    float glow, look, eye_open;
    float eyew[2][3];                   /* the eyes in the world */
    float headdir[3];                   /* where the head points, in the world */
    float ldir[3], lcol[3], lstr;
    const uint8_t *const *glyph;
    int nglyph;
    struct mcat shadow;
    /* the picture */
    float co[3], cf[3];                 /* the camera: position and forward axis */
    int rx0, ry0, rx1, ry1;             /* the rectangle the cat is in */
    float *depth;
    int32_t *tri;
    float *b1, *b2;
    size_t cap;
};

struct mglb *mglb_load(const char *path, char *err, size_t errsz)
{
    struct glb *g = glb_load(path, err, errsz);
    if (!g)
        return NULL;
    struct mglb *m = calloc(1, sizeof(*m));
    if (!m) {
        glb_free(g);
        return NULL;
    }
    m->g = g;
    {
        float lx = g->bmax[0] - g->bmin[0], lz = g->bmax[2] - g->bmin[2], len = fmaxf(lx, lz);
        m->scale = len > 1e-12f ? BODY_LENGTH / len : 1.0f;
    }
    m->origin[0] = 0.5f * (g->bmin[0] + g->bmax[0]);
    m->origin[1] = g->bmin[1];
    m->origin[2] = 0.5f * (g->bmin[2] + g->bmax[2]);
    size_t nv = (size_t)g->nv;
    m->jm = malloc((size_t)(g->nj > 0 ? g->nj : 1) * 16 * sizeof(float));
    m->sp = malloc(nv * 3 * sizeof(float));
    m->sn = malloc(nv * 3 * sizeof(float));
    m->wp = malloc(nv * 3 * sizeof(float));
    m->wn = malloc(nv * 3 * sizeof(float));
    m->sx = malloc(nv * sizeof(float));
    m->sy = malloc(nv * sizeof(float));
    m->sz = malloc(nv * sizeof(float));
    if (!m->jm || !m->sp || !m->sn || !m->wp || !m->wn || !m->sx || !m->sy || !m->sz) {
        mglb_free(m);
        return NULL;
    }
    return m;
}

void mglb_free(struct mglb *m)
{
    if (!m)
        return;
    glb_free(m->g);
    free(m->jm); free(m->sp); free(m->sn); free(m->wp); free(m->wn); free(m->sx); free(m->sy); free(m->sz);
    free(m->depth); free(m->tri); free(m->b1); free(m->b2);
    free(m);
}

static float sat(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static float sstep(float a, float b, float v) { float t = sat((v - a) / (b - a)); return t * t * (3.0f - 2.0f * t); }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }
static uint32_t h32(uint32_t a)
{
    a ^= a >> 16; a *= 0x7feb352du; a ^= a >> 15; a *= 0x846ca68bu; a ^= a >> 16;
    return a;
}
static float frand(uint32_t a) { return (float)(h32(a) & 0xffffu) / 65535.0f; }

/* model space -> world: the model's origin on the floor at the cat's place, turned by the yaw, in metres */
static void to_world(const struct mglb *m, const float p[3], float out[3])
{
    float x = (p[0] - m->origin[0]) * m->scale, y = (p[1] - m->origin[1]) * m->scale, z = (p[2] - m->origin[2]) * m->scale;
    out[0] = m->x + x * m->cy + z * m->sy_;
    out[1] = y;
    out[2] = m->z - x * m->sy_ + z * m->cy;
}

void mglb_pose(struct mglb *m, float t, const float cam[3], const uint8_t *const *glyph, int nglyph, const float ldir[3], const float lcol[3], float lstr)
{
    const struct glb *g = m->g;
    struct mcat_state st;
    mcat_state(t, &st);
    m->t = t;
    m->x = st.x; m->z = st.z; m->yaw = st.yaw;
    m->cy = cosf(st.yaw);
    m->sy_ = sinf(st.yaw);
    m->walk = st.walk;
    m->glyph = glyph;
    m->nglyph = nglyph;
    memcpy(m->ldir, ldir, sizeof(m->ldir));
    memcpy(m->lcol, lcol, sizeof(m->lcol));
    m->lstr = lstr;

    /* the walk: the animation's cycle follows the distance walked, so the paws do not slide */
    float stride_m = g->stride > 1e-9f ? g->stride * m->scale : 0.25f;
    float anim_t = g->anim_len * (st.dist / stride_m);

    /* the head: turned by the script about the head joint; towards the camera when the script says so */
    float head_rot[9];
    bool turn = g->head_joint >= 0 && g->has_eyes;
    if (turn) {
        float mid[3] = { 0.5f * (g->eye[0][0] + g->eye[1][0]), 0.5f * (g->eye[0][1] + g->eye[1][1]), 0.5f * (g->eye[0][2] + g->eye[1][2]) };
        float wx = cam[0] - m->x, wy = cam[1], wz = cam[2] - m->z;                       /* the camera in the body's frame (metres, then model units) */
        float bx = (wx * m->cy - wz * m->sy_) / m->scale, by = wy / m->scale, bz = (wx * m->sy_ + wz * m->cy) / m->scale;
        float vx = bx - (mid[0] - m->origin[0]), vy = by - (mid[1] - m->origin[1]), vz = bz - (mid[2] - m->origin[2]);
        float cam_yaw = atan2f(vx, vz), cam_pitch = -atan2f(vy, sqrtf(vx * vx + vz * vz));
        float hy, hp, hr;
        mcat_head_script(t, cam_yaw, cam_pitch, &hy, &hp, &hr);
        float cyw = cosf(hy), syw = sinf(hy), cpt = cosf(hp), spt = sinf(hp), crl = cosf(hr), srl = sinf(hr);
        float ry[9] = { cyw, 0, syw, 0, 1, 0, -syw, 0, cyw }, rx[9] = { 1, 0, 0, 0, cpt, -spt, 0, spt, cpt }, rz[9] = { crl, -srl, 0, srl, crl, 0, 0, 0, 1 }, tmp[9];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                tmp[i * 3 + j] = ry[i * 3] * rx[j] + ry[i * 3 + 1] * rx[3 + j] + ry[i * 3 + 2] * rx[6 + j];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                head_rot[i * 3 + j] = tmp[i * 3] * rz[j] + tmp[i * 3 + 1] * rz[3 + j] + tmp[i * 3 + 2] * rz[6 + j];
    }
    glb_pose(g, anim_t, st.walk, turn ? head_rot : NULL, m->jm);

    /* the eyes and how straight the head looks at the camera */
    m->glow = 0.0f;
    m->eye_open = mcat_eye_open(t);
    if (g->has_eyes) {
        const float *jh = m->jm + (size_t)0;                                          /* (the head's joint matrix: found by node) */
        int hj = -1;
        for (int j = 0; j < g->nj; j++)
            if (g->joint[j] == g->head_joint)
                hj = j;
        if (hj >= 0) {
            jh = m->jm + (size_t)hj * 16;
            for (int e = 0; e < 2; e++) {
                const float *p = g->eye[e];
                float q[3] = { jh[0] * p[0] + jh[4] * p[1] + jh[8] * p[2] + jh[12], jh[1] * p[0] + jh[5] * p[1] + jh[9] * p[2] + jh[13],
                               jh[2] * p[0] + jh[6] * p[1] + jh[10] * p[2] + jh[14] };
                to_world(m, q, m->eyew[e]);
            }
            float fm[3] = { jh[8], jh[9], jh[10] };                                    /* the rest pose's forward (+z) turned like the head */
            float fl = sqrtf(fm[0] * fm[0] + fm[1] * fm[1] + fm[2] * fm[2]);
            if (fl > 1e-9f) {
                fm[0] /= fl; fm[1] /= fl; fm[2] /= fl;
            }
            m->headdir[0] = fm[0] * m->cy + fm[2] * m->sy_;
            m->headdir[1] = fm[1];
            m->headdir[2] = -fm[0] * m->sy_ + fm[2] * m->cy;
            float hc[3] = { 0.5f * (m->eyew[0][0] + m->eyew[1][0]), 0.5f * (m->eyew[0][1] + m->eyew[1][1]), 0.5f * (m->eyew[0][2] + m->eyew[1][2]) };
            float dx = cam[0] - hc[0], dy = cam[1] - hc[1], dz = cam[2] - hc[2], dl = sqrtf(dx * dx + dy * dy + dz * dz);
            m->look = dl > 1e-3f ? (m->headdir[0] * dx + m->headdir[1] * dy + m->headdir[2] * dz) / dl : 0.0f;
            m->glow = sstep(0.90f, 0.985f, m->look) * (0.88f + 0.12f * sinf(t * 4.1f));
        }
    }

    /* the shadow */
    struct mcat *sh = &m->shadow;
    memset(sh, 0, sizeof(*sh));
    sh->enabled = true;
    sh->pos[0] = m->x; sh->pos[2] = m->z;
    sh->cy = m->cy; sh->sy = m->sy_;
    sh->scale = 1.0f;
    sh->sh_w = 0.18f; sh->sh_l = 0.48f; sh->sh_h = 0.36f;
    memcpy(sh->ldir, ldir, sizeof(sh->ldir));
    sh->lstr = lstr;
    sh->lie = 0.0f;
}

const struct mcat *mglb_shadow(const struct mglb *m) { return &m->shadow; }
float mglb_glow(const struct mglb *m) { return m->glow; }
void mglb_position(const struct mglb *m, float *x, float *z) { *x = m->x; *z = m->z; }
void mglb_rect(const struct mglb *m, int *x0, int *y0, int *x1, int *y1) { *x0 = m->rx0; *y0 = m->ry0; *x1 = m->rx1; *y1 = m->ry1; }

/* ---- skinning, projecting and rasterising (on threads) ------------------------------------------------------------------------------- */

struct pjob {
    struct mglb *m;
    int part, parts;
    float o[3], fwd[3], right[3], up[3];
    int w, h;
    float f;
    float minx, maxx, miny, maxy;       /* of the projected vertices of this part */
    int y0, y1;                         /* the rows of this part's band */
};

static void phase_skin(struct pjob *j)
{
    struct mglb *m = j->m;
    const struct glb *g = m->g;
    int v0 = (int)((int64_t)g->nv * j->part / j->parts), v1 = (int)((int64_t)g->nv * (j->part + 1) / j->parts);
    glb_skin(g, m->jm, v0, v1, m->sp, m->sn);
    j->minx = j->miny = 1e30f;
    j->maxx = j->maxy = -1e30f;
    float cxp = (float)j->w * 0.5f, cyp = (float)j->h * 0.5f;
    for (int i = v0; i < v1; i++) {
        float w3[3], n3[3];
        to_world(m, m->sp + (size_t)i * 3, w3);
        const float *n = m->sn + (size_t)i * 3;
        float nx = n[0], ny = n[1], nz = n[2], nl = sqrtf(nx * nx + ny * ny + nz * nz);
        if (nl > 1e-9f) {
            nx /= nl; ny /= nl; nz /= nl;
        }
        n3[0] = nx * m->cy + nz * m->sy_;
        n3[1] = ny;
        n3[2] = -nx * m->sy_ + nz * m->cy;
        memcpy(m->wp + (size_t)i * 3, w3, sizeof(w3));
        memcpy(m->wn + (size_t)i * 3, n3, sizeof(n3));
        float vx = w3[0] - j->o[0], vy = w3[1] - j->o[1], vz = w3[2] - j->o[2];
        float zc = vx * j->fwd[0] + vy * j->fwd[1] + vz * j->fwd[2];
        float xc = vx * j->right[0] + vy * j->right[1] + vz * j->right[2], yc = vx * j->up[0] + vy * j->up[1] + vz * j->up[2];
        m->sz[i] = zc;
        if (zc > 0.05f) {
            float sx = cxp + j->f * xc / zc, sy = cyp - j->f * yc / zc;
            m->sx[i] = sx;
            m->sy[i] = sy;
            j->minx = fminf(j->minx, sx); j->maxx = fmaxf(j->maxx, sx);
            j->miny = fminf(j->miny, sy); j->maxy = fmaxf(j->maxy, sy);
        } else {
            m->sx[i] = m->sy[i] = 0.0f;
        }
    }
}

static void raster_band(struct pjob *j)
{
    struct mglb *m = j->m;
    const struct glb *g = m->g;
    int rw = m->rx1 - m->rx0;
    for (int y = j->y0; y < j->y1; y++)                                   /* clear the band */
        for (int x = 0; x < rw; x++) {
            size_t k = (size_t)(y - m->ry0) * (size_t)rw + (size_t)x;
            m->depth[k] = 1e30f;
            m->tri[k] = -1;
        }
    for (int t = 0; t < g->nt; t++) {
        uint32_t i0 = g->idx[t * 3], i1 = g->idx[t * 3 + 1], i2 = g->idx[t * 3 + 2];
        float z0 = m->sz[i0], z1 = m->sz[i1], z2 = m->sz[i2];
        if (z0 <= 0.05f || z1 <= 0.05f || z2 <= 0.05f)
            continue;
        float x0 = m->sx[i0], y0 = m->sy[i0], x1 = m->sx[i1], y1 = m->sy[i1], x2 = m->sx[i2], y2 = m->sy[i2];
        float miny = fminf(y0, fminf(y1, y2)), maxy = fmaxf(y0, fmaxf(y1, y2));
        int iy0 = (int)floorf(miny - 0.5f), iy1 = (int)ceilf(maxy + 0.5f);
        if (iy0 < j->y0) iy0 = j->y0;
        if (iy1 >= j->y1) iy1 = j->y1 - 1;
        if (iy0 > iy1)
            continue;
        float minx = fminf(x0, fminf(x1, x2)), maxx = fmaxf(x0, fmaxf(x1, x2));
        int ix0 = (int)floorf(minx - 0.5f), ix1 = (int)ceilf(maxx + 0.5f);
        if (ix0 < m->rx0) ix0 = m->rx0;
        if (ix1 >= m->rx1) ix1 = m->rx1 - 1;
        if (ix0 > ix1)
            continue;
        float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (fabsf(area) < 1e-10f)
            continue;
        float ia = 1.0f / area, w0 = 1.0f / z0, w1 = 1.0f / z1, w2 = 1.0f / z2;
        for (int y = iy0; y <= iy1; y++) {
            float py = (float)y + 0.5f;
            for (int x = ix0; x <= ix1; x++) {
                float px = (float)x + 0.5f;
                float l1 = ((px - x0) * (y2 - y0) - (x2 - x0) * (py - y0)) * ia, l2 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0)) * ia, l0 = 1.0f - l1 - l2;
                if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f)
                    continue;
                float iw = l0 * w0 + l1 * w1 + l2 * w2, z = 1.0f / iw;
                size_t k = (size_t)(y - m->ry0) * (size_t)rw + (size_t)(x - m->rx0);
                if (z < m->depth[k]) {
                    m->depth[k] = z;
                    m->tri[k] = t;
                    m->b1[k] = l1 * w1 * z;
                    m->b2[k] = l2 * w2 * z;
                }
            }
        }
    }
}

static void *pjob_skin(void *a) { phase_skin(a); return NULL; }
static void *pjob_raster(void *a) { raster_band(a); return NULL; }

void mglb_prepare(struct mglb *m, const float o[3], const float fwd[3], const float right[3], const float up[3], int w, int h, float f, int threads)
{
    threads = threads < 1 ? 1 : threads > 8 ? 8 : threads;
    struct pjob jobs[8];
    pthread_t th[8];
    bool started[8] = { false };
    memcpy(m->co, o, sizeof(m->co));
    memcpy(m->cf, fwd, sizeof(m->cf));
    for (int i = 0; i < threads; i++) {
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].m = m; jobs[i].part = i; jobs[i].parts = threads; jobs[i].w = w; jobs[i].h = h; jobs[i].f = f;
        memcpy(jobs[i].o, o, sizeof(jobs[i].o)); memcpy(jobs[i].fwd, fwd, sizeof(jobs[i].fwd));
        memcpy(jobs[i].right, right, sizeof(jobs[i].right)); memcpy(jobs[i].up, up, sizeof(jobs[i].up));
        if (i > 0)
            started[i] = pthread_create(&th[i], NULL, pjob_skin, &jobs[i]) == 0;
    }
    phase_skin(&jobs[0]);
    for (int i = 1; i < threads; i++) {
        if (started[i])
            pthread_join(th[i], NULL);
        else
            phase_skin(&jobs[i]);
    }
    float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
    for (int i = 0; i < threads; i++) {
        minx = fminf(minx, jobs[i].minx); maxx = fmaxf(maxx, jobs[i].maxx);
        miny = fminf(miny, jobs[i].miny); maxy = fmaxf(maxy, jobs[i].maxy);
    }
    m->rx0 = m->ry0 = m->rx1 = m->ry1 = 0;
    if (maxx < minx)
        return;
    m->rx0 = (int)fmaxf(floorf(minx) - 24.0f, 0.0f);                       /* (a margin for the glow of the eyes) */
    m->ry0 = (int)fmaxf(floorf(miny) - 24.0f, 0.0f);
    m->rx1 = (int)fminf(ceilf(maxx) + 24.0f, (float)w);
    m->ry1 = (int)fminf(ceilf(maxy) + 24.0f, (float)h);
    if (m->rx1 <= m->rx0 || m->ry1 <= m->ry0) {
        m->rx1 = m->rx0;
        m->ry1 = m->ry0;
        return;
    }
    size_t need = (size_t)(m->rx1 - m->rx0) * (size_t)(m->ry1 - m->ry0);
    if (need > m->cap) {
        free(m->depth); free(m->tri); free(m->b1); free(m->b2);
        m->depth = malloc(need * sizeof(float));
        m->tri = malloc(need * sizeof(int32_t));
        m->b1 = malloc(need * sizeof(float));
        m->b2 = malloc(need * sizeof(float));
        m->cap = (m->depth && m->tri && m->b1 && m->b2) ? need : 0;
        if (!m->cap) {
            m->rx1 = m->rx0;
            m->ry1 = m->ry0;
            return;
        }
    }
    int rh = m->ry1 - m->ry0;
    for (int i = 0; i < threads; i++) {
        jobs[i].y0 = m->ry0 + rh * i / threads;
        jobs[i].y1 = m->ry0 + rh * (i + 1) / threads;
        started[i] = false;
        if (i > 0)
            started[i] = pthread_create(&th[i], NULL, pjob_raster, &jobs[i]) == 0;
    }
    raster_band(&jobs[0]);
    for (int i = 1; i < threads; i++) {
        if (started[i])
            pthread_join(th[i], NULL);
        else
            raster_band(&jobs[i]);
    }
}

/* ---- colouring a pixel ----------------------------------------------------------------------------------------------------------------- */

bool mglb_pixel(const struct mglb *m, int x, int y, const float o[3], const float dn[3], float tmax, float inv_f, float rgb[3])
{
    if (x < m->rx0 || x >= m->rx1 || y < m->ry0 || y >= m->ry1)
        return false;
    const struct glb *g = m->g;
    const float eye_r = EYE_UNITS * m->scale;
    bool touched = false;
    size_t k = (size_t)(y - m->ry0) * (size_t)(m->rx1 - m->rx0) + (size_t)(x - m->rx0);
    int32_t tri = m->tri[k];
    float fdot = dn[0] * m->cf[0] + dn[1] * m->cf[1] + dn[2] * m->cf[2];
    float bt = tri >= 0 && fdot > 1e-4f ? m->depth[k] / fdot : 1e30f;                     /* (the depth is along the camera's axis, the ray's distance is longer) */
    if (tri >= 0 && bt < tmax) {
        uint32_t i0 = g->idx[tri * 3], i1 = g->idx[tri * 3 + 1], i2 = g->idx[tri * 3 + 2];
        float b1 = m->b1[k], b2 = m->b2[k], b0 = 1.0f - b1 - b2;
        float q[3], nr[3], nw[3], pw[3], u = 0.0f, v = 0.0f;
        for (int a = 0; a < 3; a++) {
            q[a] = (b0 * g->pos[i0 * 3 + a] + b1 * g->pos[i1 * 3 + a] + b2 * g->pos[i2 * 3 + a]) * m->scale;      /* the rest pose in metres */
            nr[a] = b0 * g->nrm[i0 * 3 + a] + b1 * g->nrm[i1 * 3 + a] + b2 * g->nrm[i2 * 3 + a];
            nw[a] = b0 * m->wn[i0 * 3 + a] + b1 * m->wn[i1 * 3 + a] + b2 * m->wn[i2 * 3 + a];
            pw[a] = b0 * m->wp[i0 * 3 + a] + b1 * m->wp[i1 * 3 + a] + b2 * m->wp[i2 * 3 + a];
        }
        u = b0 * g->uv[i0 * 2] + b1 * g->uv[i1 * 2] + b2 * g->uv[i2 * 2];
        v = b0 * g->uv[i0 * 2 + 1] + b1 * g->uv[i1 * 2 + 1] + b2 * g->uv[i2 * 2 + 1];
        float nl = sqrtf(nw[0] * nw[0] + nw[1] * nw[1] + nw[2] * nw[2]);
        if (nl > 1e-9f) {
            nw[0] /= nl; nw[1] /= nl; nw[2] /= nl;
        }
        float ndv = -(nw[0] * dn[0] + nw[1] * dn[1] + nw[2] * dn[2]);
        if (ndv < 0.0f) {                                                              /* (a double sided surface seen from behind) */
            ndv = -ndv;
            nw[0] = -nw[0]; nw[1] = -nw[1]; nw[2] = -nw[2];
        }
        /* the colour of the model there */
        float tc[3] = { 0.80f, 0.65f, 0.40f };
        if (g->tex) {
            float fu = u - floorf(u), fv = v - floorf(v);
            int tx = (int)(fu * (float)g->tex_w), ty = (int)(fv * (float)g->tex_h);
            if (tx >= g->tex_w) tx = g->tex_w - 1;
            if (ty >= g->tex_h) ty = g->tex_h - 1;
            uint32_t px = g->tex[(size_t)ty * (size_t)g->tex_w + (size_t)tx];
            tc[0] = (float)((px >> 16) & 255) / 255.0f; tc[1] = (float)((px >> 8) & 255) / 255.0f; tc[2] = (float)(px & 255) / 255.0f;
        }
        /* the letters of the fur: a cell of the 3D grid in the rest pose, so they stay on the fur as it moves */
        int ci[3] = { (int)floorf(q[0] / CELLM), (int)floorf(q[1] / CELLM), (int)floorf(q[2] / CELLM) };
        uint32_t hc = h32((uint32_t)ci[0] * 73856093u ^ (uint32_t)ci[1] * 19349663u ^ (uint32_t)ci[2] * 83492791u);
        int epoch = (int)floorf(m->t * 1.3f + (float)((hc >> 12) & 255) / 256.0f * 4.0f);
        uint32_t hg = h32(hc + (uint32_t)epoch * 2246822519u);
        int ax = fabsf(nr[0]) >= fabsf(nr[1]) && fabsf(nr[0]) >= fabsf(nr[2]) ? 0 : fabsf(nr[1]) >= fabsf(nr[2]) ? 1 : 2;
        int au = ax == 0 ? 1 : 0, av = ax == 2 ? 1 : 2;
        float fu2 = q[au] / CELLM - floorf(q[au] / CELLM), fv2 = q[av] / CELLM - floorf(q[av] / CELLM);
        const uint8_t *gl = m->nglyph > 0 && m->glyph ? m->glyph[hg % (uint32_t)m->nglyph] : NULL;
        int gx = (int)(fu2 * 9.0f), gy = (int)(fv2 * 16.0f);
        float bit = (gl && gx < 8 && gy < 16 && ((gl[gy] >> gx) & 1)) ? 1.0f : 0.0f;
        float foot = bt * inv_f, gp = CELLM / 16.0f;
        float mix = sat((foot / gp - 0.9f) * 0.6f) * 0.6f;                              /* far: part of it stays the letter's own shape, so it still reads as text */
        float cov = 0.28f + 0.72f * (bit * (1.0f - mix) + 0.3f * mix);
        float cellb = 0.62f + 0.38f * frand(hc >> 3);
        float sparkle = ((hg >> 8) & 31) == 0 ? 1.0f : 0.0f;

        float att = 1.0f / (1.0f + 0.018f * bt * bt);
        float L = 0.30f + 0.62f * sat(ndv);
        float key = m->lstr > 0.0f ? sat(nw[0] * m->ldir[0] + nw[1] * m->ldir[1] + nw[2] * m->ldir[2]) * m->lstr : 0.0f;
        L += 0.55f * key;
        L *= 0.55f + 0.45f * sat(pw[1] / 0.15f);                                        /* darker down by the floor */
        float rim = powf(1.0f - sat(ndv), 3.0f);
        float lum = 3.4f * att * L * cellb * cov;
        float fur[3] = { 255.0f * (tc[0] * lum + 0.30f * rim), 255.0f * (tc[1] * lum + 0.26f * rim), 255.0f * (tc[2] * lum + 0.08f * rim) };
        for (int a = 0; a < 3; a++)
            fur[a] += m->lcol[a] * 0.18f * key * att * cov;
        fur[0] += 70.0f * sparkle * bit * att; fur[1] += 255.0f * sparkle * bit * att; fur[2] += 110.0f * sparkle * bit * att;
        /* the eyes: dark with a slit, or glowing */
        if (g->has_eyes)
            for (int e = 0; e < 2; e++) {
                float dx = pw[0] - m->eyew[e][0], dy = pw[1] - m->eyew[e][1], dz = pw[2] - m->eyew[e][2];
                float d2 = (dx * dx + dy * dy + dz * dz) / (eye_r * eye_r);
                if (d2 < 1.6f) {
                    float edge = 1.0f - sstep(0.7f, 1.2f, d2), gl2 = m->glow;
                    float side = (dx * m->cy - dz * m->sy_) / eye_r;                    /* across the eye (in the body's frame): the slit is a narrow band along it */
                    float slit = mixf(0.30f, 0.13f, gl2) * (0.4f + 0.6f * m->eye_open);
                    float kk = fabsf(side) < slit ? mixf(0.05f, 0.30f, gl2) : 1.0f;
                    float br = kk * (0.55f + 0.7f * gl2) * (m->eye_open > 0.3f ? 1.0f : 0.2f);
                    fur[0] = mixf(fur[0], mixf(130.0f, 205.0f, gl2) * br, edge);
                    fur[1] = mixf(fur[1], mixf(135.0f, 255.0f, gl2) * br, edge);
                    fur[2] = mixf(fur[2], mixf(20.0f, 120.0f, gl2) * br, edge);
                }
            }
        rgb[0] = fur[0]; rgb[1] = fur[1]; rgb[2] = fur[2];
        touched = true;
    }
    if (m->glow > 0.02f && g->has_eyes) {                                              /* the glow round the eyes (in front of what lies behind them) */
        float hr = 0.0215f;
        for (int e = 0; e < 2; e++) {
            float ex = m->eyew[e][0] - o[0], ey = m->eyew[e][1] - o[1], ez = m->eyew[e][2] - o[2];
            float te = ex * dn[0] + ey * dn[1] + ez * dn[2];
            if (te <= 0.05f || te > tmax + 0.05f)
                continue;
            float pd2 = ex * ex + ey * ey + ez * ez - te * te;
            if (pd2 > 9.0f * hr * hr)
                continue;
            float kk = m->glow * m->eye_open * expf(-pd2 / (hr * hr));
            if (kk < 0.01f)
                continue;
            touched = true;
            rgb[0] += 170.0f * kk; rgb[1] += 255.0f * kk; rgb[2] += 110.0f * kk;
        }
    }
    return touched;
}
