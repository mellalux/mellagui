/* camfx - picture effects for mellagui-cam. See camfx.h. All integer arithmetic where it counts; floats only for the geometry of the distortions. */
#include "camfx.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CL(v) ((v) < 0 ? 0 : (v) > 255 ? 255 : (v))
#define CR(p) (((p) >> 16) & 255)
#define CG(p) (((p) >> 8) & 255)
#define CB(p) ((p) & 255)
#define PI_F 3.14159265f

static const struct { const char *id, *name; } k_fx[CAMFX_COUNT] = {
    [CAMFX_NORMAL] = { "normal", "Normal" },       [CAMFX_BW] = { "bw", "Black & White" },       [CAMFX_SEPIA] = { "sepia", "Sepia" },
    [CAMFX_PENCIL] = { "pencil", "Pencil" },       [CAMFX_COMIC] = { "comic", "Comic Book" },    [CAMFX_POP] = { "pop", "Pop Art" },
    [CAMFX_THERMAL] = { "thermal", "Thermal" },    [CAMFX_XRAY] = { "xray", "X-Ray" },           [CAMFX_GLOW] = { "glow", "Glow" },
    [CAMFX_MIRROR] = { "mirror", "Mirror" },       [CAMFX_KALEIDO] = { "kaleidoscope", "Kaleidoscope" }, [CAMFX_TUNNEL] = { "tunnel", "Light Tunnel" },
    [CAMFX_SQUEEZE] = { "squeeze", "Squeeze" },    [CAMFX_STRETCH] = { "stretch", "Stretch" },   [CAMFX_BULGE] = { "bulge", "Bulge" },
    [CAMFX_DENT] = { "dent", "Dent" },             [CAMFX_TWIRL] = { "twirl", "Twirl" },         [CAMFX_VINTAGE] = { "vintage", "Vintage" },
    [CAMFX_SUNSET] = { "sunset", "Sunset" },       [CAMFX_SPACE] = { "space", "Space" },         [CAMFX_NEON] = { "neon", "Neon" },
    [CAMFX_GHOST] = { "ghost", "Ghost" },
};

int camfx_is_backdrop(int fx) { return fx >= CAMFX_SUNSET && fx <= CAMFX_GHOST; }

const char *camfx_name(int fx) { return fx >= 0 && fx < CAMFX_COUNT ? k_fx[fx].name : NULL; }
const char *camfx_id(int fx) { return fx >= 0 && fx < CAMFX_COUNT ? k_fx[fx].id : NULL; }

int camfx_by_id(const char *id)
{
    if (!id)
        return -1;
    for (int i = 0; i < CAMFX_COUNT; i++)
        if (!strcasecmp(id, k_fx[i].id))
            return i;
    return -1;
}

static inline uint32_t rgb(int r, int g, int b)
{
    return 0xff000000u | ((uint32_t)CL(r) << 16) | ((uint32_t)CL(g) << 8) | (uint32_t)CL(b);
}

static inline int lum(uint32_t p)
{
    return (int)((CR(p) * 77 + CG(p) * 150 + CB(p) * 29) >> 8);
}

/* Bilinear sample with the coordinates clamped to the picture. */
static uint32_t sample(const uint32_t *s, int w, int h, float x, float y)
{
    if (!(x > 0))
        x = 0;
    if (!(y > 0))
        y = 0;
    if (x > (float)(w - 1))
        x = (float)(w - 1);
    if (y > (float)(h - 1))
        y = (float)(h - 1);
    int x0 = (int)x, y0 = (int)y, x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
    int fx = (int)((x - (float)x0) * 256.0f), fy = (int)((y - (float)y0) * 256.0f);
    const uint32_t *r0 = s + (size_t)y0 * (size_t)w, *r1 = s + (size_t)y1 * (size_t)w;
    uint32_t out = 0xff000000u;
    for (int sh = 0; sh <= 16; sh += 8) {
        int a = (int)((r0[x0] >> sh) & 255), b = (int)((r0[x1] >> sh) & 255), c = (int)((r1[x0] >> sh) & 255), d = (int)((r1[x1] >> sh) & 255);
        int top = a * (256 - fx) + b * fx, bot = c * (256 - fx) + d * fx;
        out |= (uint32_t)((top * (256 - fy) + bot * fy) >> 16) << sh;
    }
    return out;
}

/* ---- colour effects ------------------------------------------------------------------------------------------------------ */

static void fx_bw(const uint32_t *s, int n, uint32_t *d)
{
    for (int i = 0; i < n; i++) {
        int l = (lum(s[i]) - 128) * 9 / 8 + 128;                    /* a little more contrast than a plain grey */
        d[i] = rgb(l, l, l);
    }
}

static void fx_sepia(const uint32_t *s, int n, uint32_t *d)
{
    for (int i = 0; i < n; i++) {
        int r = (int)CR(s[i]), g = (int)CG(s[i]), b = (int)CB(s[i]);
        d[i] = rgb((r * 393 + g * 769 + b * 189) / 1000, (r * 349 + g * 686 + b * 168) / 1000, (r * 272 + g * 534 + b * 131) / 1000);
    }
}

/* 3 x 3 Sobel of a grey picture, |gx| + |gy| / 4, 0..255. The border repeats its edge. */
static void sobel(const uint8_t *g, int w, int h, uint8_t *m)
{
    for (int y = 0; y < h; y++) {
        int ym = y > 0 ? y - 1 : y, yp = y < h - 1 ? y + 1 : y;
        for (int x = 0; x < w; x++) {
            int xm = x > 0 ? x - 1 : x, xp = x < w - 1 ? x + 1 : x;
            int a = g[ym * w + xm], b = g[ym * w + x], c = g[ym * w + xp], d = g[y * w + xm], f = g[y * w + xp];
            int e = g[yp * w + xm], k = g[yp * w + x], i = g[yp * w + xp];
            int gx = (c + 2 * f + i) - (a + 2 * d + e), gy = (e + 2 * k + i) - (a + 2 * b + c);
            int v = (abs(gx) + abs(gy)) / 4;
            m[y * w + x] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
}

static void fx_pencil(const uint32_t *s, int w, int h, uint32_t *d)
{
    int n = w * h;
    uint8_t *g = malloc((size_t)n * 2);
    if (!g) {
        memcpy(d, s, (size_t)n * 4);
        return;
    }
    uint8_t *m = g + n;
    for (int i = 0; i < n; i++)
        g[i] = (uint8_t)lum(s[i]);
    sobel(g, w, h, m);
    for (int i = 0; i < n; i++) {
        int v = 255 - CL(m[i] * 2) - (255 - g[i]) / 4;               /* the line, and a little shading where the picture is dark */
        v = CL(v);
        d[i] = rgb(v, v - v / 40, v - v / 16);                        /* warm paper */
    }
    free(g);
}

static int post4(int c)                                             /* 0..255 -> one of four levels */
{
    return (c >> 6) * 85;
}

static void fx_comic(const uint32_t *s, int w, int h, uint32_t *d)
{
    int n = w * h;
    uint8_t *g = malloc((size_t)n * 2);
    if (!g) {
        memcpy(d, s, (size_t)n * 4);
        return;
    }
    uint8_t *m = g + n;
    for (int i = 0; i < n; i++)
        g[i] = (uint8_t)lum(s[i]);
    sobel(g, w, h, m);
    for (int i = 0; i < n; i++) {
        if (m[i] > 42) {
            d[i] = 0xff000000u;
            continue;
        }
        int l = g[i], r = (int)CR(s[i]), gg = (int)CG(s[i]), b = (int)CB(s[i]);
        r = l + (r - l) * 14 / 10;                                   /* saturated, then four levels a channel */
        gg = l + (gg - l) * 14 / 10;
        b = l + (b - l) * 14 / 10;
        d[i] = rgb(post4(CL(r)) + 20, post4(CL(gg)) + 20, post4(CL(b)) + 20);
    }
    free(g);
}

static void fx_pop(const uint32_t *s, int w, int h, uint32_t *d)
{
    static const uint8_t pal[4][4][3] = {
        { { 20, 20, 70 }, { 220, 30, 50 }, { 250, 210, 40 }, { 250, 240, 210 } },
        { { 10, 50, 60 }, { 0, 150, 160 }, { 250, 120, 30 }, { 250, 240, 200 } },
        { { 60, 10, 80 }, { 230, 40, 140 }, { 60, 200, 230 }, { 255, 250, 220 } },
        { { 20, 60, 20 }, { 50, 170, 60 }, { 250, 230, 60 }, { 250, 250, 230 } },
    };
    for (int y = 0; y < h; y++) {
        int q0 = y >= h / 2 ? 2 : 0, sy = (y * 2) % h;
        for (int x = 0; x < w; x++) {
            int q = q0 + (x >= w / 2 ? 1 : 0);
            int l = lum(s[(size_t)sy * (size_t)w + (size_t)((x * 2) % w)]) >> 6;
            d[(size_t)y * (size_t)w + (size_t)x] = rgb(pal[q][l][0], pal[q][l][1], pal[q][l][2]);
        }
    }
}

static void fx_thermal(const uint32_t *s, int n, uint32_t *d)
{
    static const uint8_t stop[7][3] = { { 0, 0, 60 }, { 40, 0, 200 }, { 0, 200, 220 }, { 80, 230, 60 }, { 255, 230, 0 }, { 255, 60, 0 }, { 255, 255, 255 } };
    uint32_t lut[256];
    for (int v = 0; v < 256; v++) {
        int seg = v * 6 / 256, f = v * 6 - seg * 256;                   /* the stretch 0..5 of the gradient and 0..255 inside it */
        lut[v] = rgb(stop[seg][0] + (stop[seg + 1][0] - stop[seg][0]) * f / 256, stop[seg][1] + (stop[seg + 1][1] - stop[seg][1]) * f / 256,
                     stop[seg][2] + (stop[seg + 1][2] - stop[seg][2]) * f / 256);
    }
    for (int i = 0; i < n; i++)
        d[i] = lut[lum(s[i])];
}

static void fx_xray(const uint32_t *s, int n, uint32_t *d)
{
    for (int i = 0; i < n; i++) {
        int v = (255 - lum(s[i]) - 128) * 13 / 10 + 128;
        v = CL(v);
        d[i] = rgb(v * 50 / 100, v * 82 / 100, v * 115 / 100 + 10);
    }
}

/* Separable box blur of an XRGB picture, radius r (edges repeat). `tmp` is as big as the picture. */
static void box_blur(uint32_t *a, uint32_t *tmp, int w, int h, int r)
{
    int span = 2 * r + 1;
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t *in = pass ? tmp : a;
        uint32_t *out = pass ? a : tmp;
        int len = pass ? h : w, lines = pass ? w : h;
        size_t step = pass ? (size_t)w : 1, stride = pass ? 1 : (size_t)w;      /* along the line, and from line to line */
        for (int l = 0; l < lines; l++) {
            const uint32_t *p = in + (size_t)l * stride;
            uint32_t *q = out + (size_t)l * stride;
            for (int i = 0; i < len; i++) {
                int sr = 0, sg = 0, sb = 0;
                for (int k = -r; k <= r; k++) {
                    int j = i + k < 0 ? 0 : i + k >= len ? len - 1 : i + k;
                    uint32_t c = p[(size_t)j * step];
                    sr += (int)CR(c);
                    sg += (int)CG(c);
                    sb += (int)CB(c);
                }
                q[(size_t)i * step] = rgb(sr / span, sg / span, sb / span);
            }
        }
    }
}

static void fx_glow(const uint32_t *s, int w, int h, uint32_t *d)
{
    int sw = (w + 3) / 4, sh = (h + 3) / 4;
    uint32_t *small = malloc((size_t)sw * (size_t)sh * 8);
    if (!small) {
        memcpy(d, s, (size_t)w * (size_t)h * 4);
        return;
    }
    uint32_t *tmp = small + (size_t)sw * (size_t)sh;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            int r = 0, g = 0, b = 0, c = 0;
            for (int yy = y * 4; yy < y * 4 + 4 && yy < h; yy++)
                for (int xx = x * 4; xx < x * 4 + 4 && xx < w; xx++) {
                    uint32_t p = s[(size_t)yy * (size_t)w + (size_t)xx];
                    r += (int)CR(p);
                    g += (int)CG(p);
                    b += (int)CB(p);
                    c++;
                }
            small[(size_t)y * (size_t)sw + (size_t)x] = rgb(r / c, g / c, b / c);
        }
    box_blur(small, tmp, sw, sh, 3);
    box_blur(small, tmp, sw, sh, 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t a = s[(size_t)y * (size_t)w + (size_t)x];
            uint32_t b = sample(small, sw, sh, ((float)x + 0.5f) / 4.0f - 0.5f, ((float)y + 0.5f) / 4.0f - 0.5f);
            int r = (int)CR(b) * 95 / 100, g = (int)CG(b) * 95 / 100, bl = (int)CB(b) * 95 / 100;            /* screen: 1 - (1 - a)(1 - b) */
            d[(size_t)y * (size_t)w + (size_t)x] = rgb(255 - (255 - (int)CR(a)) * (255 - CL(r)) / 255, 255 - (255 - (int)CG(a)) * (255 - CL(g)) / 255,
                                                        255 - (255 - (int)CB(a)) * (255 - CL(bl)) / 255);
        }
    free(small);
}

static void fx_vintage(const uint32_t *s, int w, int h, uint32_t *d, float fcx, float fcy)
{
    float vcx = fcx * (float)(w - 1), vcy = fcy * (float)(h - 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t p = s[(size_t)y * (size_t)w + (size_t)x];
            int r = (int)CR(p), g = (int)CG(p), b = (int)CB(p), l = lum(p);
            int sr = (r * 393 + g * 769 + b * 189) / 1000, sg = (r * 349 + g * 686 + b * 168) / 1000, sb = (r * 272 + g * 534 + b * 131) / 1000;
            r = (sr * 7 + l * 3) / 10;                                  /* mostly sepia, a bit of the picture's own grey */
            g = (sg * 7 + l * 3) / 10;
            b = (sb * 7 + l * 3) / 10;
            float nx = ((float)x - vcx) / ((float)w / 2.0f), ny = ((float)y - vcy) / ((float)h / 2.0f);
            float t = (sqrtf(nx * nx + ny * ny) - 0.45f) / 0.75f;      /* vignette: nothing in the middle, dark at the corners */
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            int k = 256 - (int)(t * t * 150.0f);
            uint32_t hs = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
            hs = (hs ^ (hs >> 13)) * 1274126177u;
            int grain = (int)((hs ^ (hs >> 16)) & 15) - 8;
            r = (r * 85 / 100 + 28) * k / 256 + grain;
            g = (g * 85 / 100 + 24) * k / 256 + grain;
            b = (b * 85 / 100 + 18) * k / 256 + grain;
            d[(size_t)y * (size_t)w + (size_t)x] = rgb(r, g, b);
        }
}

/* ---- mirrors and distortions: every output pixel looks up a place in the source ---------------------------------------------- */

/* The left of the axis is reflected on the right of it (the axis at fcx of the width: the middle unless the user moved it). */
static void fx_mirror(const uint32_t *s, int w, int h, uint32_t *d, float fcx)
{
    float ax = fcx * (float)(w - 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int xs = (float)x <= ax ? x : (int)(2.0f * ax - (float)x + 0.5f);
            xs = xs < 0 ? 0 : xs > w - 1 ? w - 1 : xs;
            d[(size_t)y * (size_t)w + (size_t)x] = s[(size_t)y * (size_t)w + (size_t)xs];
        }
}

static void fx_kaleido(const uint32_t *s, int w, int h, uint32_t *d, float fcx, float fcy)
{
    float cx = fcx * (float)(w - 1), cy = fcy * (float)(h - 1), seg = 2.0f * PI_F / 6.0f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float dx = (float)x - cx, dy = (float)y - cy, r = sqrtf(dx * dx + dy * dy) * 0.8f;
            float a = atan2f(dy, dx) + PI_F;
            a = fmodf(a, seg);
            if (a > seg / 2.0f)
                a = seg - a;                                            /* fold the sector onto its mirror image: the pattern is symmetric */
            a += 0.15f;
            d[(size_t)y * (size_t)w + (size_t)x] = sample(s, w, h, cx + r * cosf(a), cy + r * sinf(a));
        }
}

static void fx_tunnel(const uint32_t *s, int w, int h, uint32_t *d, float fcx, float fcy)
{
    float cx = fcx * (float)(w - 1), cy = fcy * (float)(h - 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float dx = (float)x - cx, dy = (float)y - cy, r = sqrtf(dx * dx + dy * dy) / ((float)h / 2.0f);
            float u = fabsf(atan2f(dy, dx)) / PI_F * (float)(w - 1);   /* the angle, mirrored so there is no seam */
            float t = 0.22f / (r + 0.05f);                              /* the depth: the picture repeats towards the middle */
            d[(size_t)y * (size_t)w + (size_t)x] = sample(s, w, h, u, (t - floorf(t)) * (float)(h - 1));
        }
}

/* The lens effects work inside the ellipse that touches the four edges (the twirl in a circle); `mode` picks the lookup. */
static void fx_lens(int fx, const uint32_t *s, int w, int h, uint32_t *d, float fcx, float fcy)
{
    float cx = fcx * (float)(w - 1), cy = fcy * (float)(h - 1), hw = (float)w / 2.0f, hh = (float)h / 2.0f, rad = (hw < hh ? hw : hh) * 0.95f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float dx = (float)x - cx, dy = (float)y - cy, nx = dx / hw, ny = dy / hh, rho = sqrtf(nx * nx + ny * ny);
            float sx = (float)x, sy = (float)y;
            if (rho < 1.0f) {
                float wgt = (1.0f - rho) * (1.0f - rho);
                switch (fx) {
                case CAMFX_SQUEEZE: sx = cx + dx * (1.0f + 1.2f * wgt); break;               /* the middle gets narrower */
                case CAMFX_STRETCH: sy = cy + dy * (1.0f - 0.55f * wgt); break;              /* the middle gets taller */
                case CAMFX_BULGE:
                    if (rho > 0) {
                        float f = powf(rho, 0.8f);                                            /* source radius = rho^1.8: the middle is magnified */
                        sx = cx + dx * f;
                        sy = cy + dy * f;
                    }
                    break;
                case CAMFX_DENT:
                    if (rho > 0) {
                        float f = powf(rho, -0.444f);                                         /* source radius = rho^0.556: the middle is pinched */
                        sx = cx + dx * f;
                        sy = cy + dy * f;
                    }
                    break;
                }
            }
            if (fx == CAMFX_TWIRL) {
                float r = sqrtf(dx * dx + dy * dy);
                if (r < rad) {
                    float t = 1.0f - r / rad, ang = 2.4f * t * t, c = cosf(ang), sn = sinf(ang);
                    sx = cx + dx * c - dy * sn;
                    sy = cy + dx * sn + dy * c;
                }
            }
            d[(size_t)y * (size_t)w + (size_t)x] = sample(s, w, h, sx, sy);
        }
}


/* ---- backdrops: what differs from the empty scene stays, the rest is replaced -------------------------------------------------------------- */

/* Sliding box blur of a grey picture, radius r (edges repeat); `tmp` is as big. */
static void blur_u8(uint8_t *a, uint8_t *tmp, int w, int h, int r)
{
    int span = 2 * r + 1;
    for (int pass = 0; pass < 2; pass++) {
        const uint8_t *in = pass ? tmp : a;
        uint8_t *out = pass ? a : tmp;
        int len = pass ? h : w, lines = pass ? w : h;
        size_t step = pass ? (size_t)w : 1, stride = pass ? 1 : (size_t)w;
        for (int l = 0; l < lines; l++) {
            const uint8_t *p = in + (size_t)l * stride;
            uint8_t *q = out + (size_t)l * stride;
            int sum = 0;
            for (int k = -r; k <= r; k++)
                sum += p[(size_t)(k < 0 ? 0 : k >= len ? len - 1 : k) * step];
            q[0] = (uint8_t)(sum / span);
            for (int i = 1; i < len; i++) {
                int add = i + r >= len ? len - 1 : i + r, sub = i - 1 - r < 0 ? 0 : i - 1 - r;
                sum += p[(size_t)add * step] - p[(size_t)sub * step];
                q[(size_t)i * step] = (uint8_t)(sum / span);
            }
        }
    }
}

/* alpha 0..255 of the person: from the difference to the reference, or an oval when there is none. */
static void make_mask(const uint32_t *src, const uint32_t *ref, int w, int h, uint8_t *m, uint8_t *tmp)
{
    int n = w * h;
    if (!ref) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float nx = ((float)x - 0.5f * (float)w) / (0.30f * (float)w), ny = ((float)y - 0.56f * (float)h) / (0.46f * (float)h);
                float a = (1.0f - sqrtf(nx * nx + ny * ny)) / 0.14f;
                m[y * w + x] = (uint8_t)(a <= 0 ? 0 : a >= 1 ? 255 : (int)(a * 255.0f));
            }
        return;
    }
    for (int i = 0; i < n; i++) {
        int d = abs((int)CR(src[i]) - (int)CR(ref[i])) + abs((int)CG(src[i]) - (int)CG(ref[i])) + abs((int)CB(src[i]) - (int)CB(ref[i]));
        m[i] = d > 72 ? 255 : 0;
    }
    int small = (w < h ? w : h);
    blur_u8(m, tmp, w, h, small / 80 + 1);                         /* specks go, small holes fill ... */
    for (int i = 0; i < n; i++)
        m[i] = m[i] > 110 ? 255 : 0;
    blur_u8(m, tmp, w, h, small / 160 + 1);                        /* ... and the edge is soft */
}

static uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static inline int lerp_i(int a, int b, float t) { return a + (int)((float)(b - a) * t); }

static uint32_t bd_sunset(int x, int y, int w, int h)
{
    float u = (float)x / (float)w, v = (float)y / (float)h, hor = 0.62f;
    float sx = 0.68f, sy = 0.50f, sr = 0.11f;
    float dx = (u - sx) * (float)w / (float)h, dy = v - sy, d = sqrtf(dx * dx + dy * dy);
    int r, g, b;
    if (v < hor) {
        float t = v / hor;
        if (t < 0.6f) {
            float k = t / 0.6f;
            r = lerp_i(40, 200, k), g = lerp_i(20, 60, k), b = lerp_i(90, 110, k);
        } else {
            float k = (t - 0.6f) / 0.4f;
            r = lerp_i(200, 255, k), g = lerp_i(60, 170, k), b = lerp_i(110, 60, k);
        }
        float glow = expf(-(d / 0.3f) * (d / 0.3f)) * 0.55f;
        r += (int)(glow * 255), g += (int)(glow * 150), b += (int)(glow * 40);
        if (d < sr)
            r = 255, g = 235, b = 150;
    } else {
        float k = (v - hor) / (1.0f - hor);
        r = lerp_i(110, 25, k), g = lerp_i(40, 15, k), b = lerp_i(90, 60, k);
        float refl = expf(-((u - sx) / 0.07f) * ((u - sx) / 0.07f)) * (0.5f + 0.5f * sinf((float)y * 0.8f + (float)x * 0.05f)) * (1.0f - k) * 0.9f;
        r += (int)(refl * 255), g += (int)(refl * 150), b += (int)(refl * 50);
    }
    float hill = hor - 0.06f - 0.04f * sinf(u * 7.0f + 1.0f) - 0.025f * sinf(u * 17.0f);
    if (v > hill && v < hor + 0.015f)
        r = 25, g = 10, b = 40;
    return rgb(r, g, b);
}

static uint32_t bd_space(int x, int y, int w, int h)
{
    float fx = (float)x * 640.0f / (float)w, fy = (float)y * 480.0f / (float)h;            /* the same picture at any size */
    float v = (float)y / (float)h;
    int r = lerp_i(5, 15, v), g = lerp_i(5, 10, v), b = lerp_i(20, 40, v);
    float n1 = 0.5f + 0.5f * sinf(fx * 0.011f + sinf(fy * 0.017f) * 2.0f), n2 = 0.5f + 0.5f * sinf(fy * 0.013f + sinf(fx * 0.009f) * 2.0f);
    float n = n1 * n2;
    r += (int)(90 * n * 0.5f + 30 * (1 - n) * 0.2f), g += (int)(40 * n * 0.5f + 70 * (1 - n) * 0.2f), b += (int)(140 * n * 0.5f + 160 * (1 - n) * 0.2f);
    uint32_t hs = hash2(x, y);
    if ((hs & 1023) < 3) {
        int s = 120 + (int)((hs >> 12) & 127);
        r = g = b = s;
        if (hs & 0x100000)
            r = s, b = s / 2 + 60;
    }
    float pdx = ((float)x - 0.2f * (float)w), pdy = ((float)y - 0.72f * (float)h), pr = 0.2f * (float)h, pd = sqrtf(pdx * pdx + pdy * pdy);
    if (pd < pr) {
        float zx = pdx / pr, zy = pdy / pr, zz = sqrtf(1.0f - zx * zx - zy * zy);
        float lit = zx * 0.5f - zy * 0.5f + zz * 0.7f;
        lit = lit < 0.08f ? 0.08f : lit > 1 ? 1 : lit;
        float band = 0.85f + 0.15f * sinf(zy * 14.0f);
        r = (int)(210.0f * lit * band), g = (int)(120.0f * lit * band), b = (int)(70.0f * lit);
    }
    return rgb(r, g, b);
}

static uint32_t bd_neon(int x, int y, int w, int h)
{
    float u = (float)x / (float)w, v = (float)y / (float)h, hor = 0.55f;
    int r, g, b;
    if (v < hor) {
        float t = v / hor;
        r = lerp_i(20, 255, t * t), g = lerp_i(0, 40, t), b = lerp_i(50, 150, t);
        float dx = (u - 0.5f) * (float)w / (float)h, dy = v - 0.38f, d = sqrtf(dx * dx + dy * dy);
        if (d < 0.2f) {
            float k = (dy + 0.2f) / 0.4f;                                                  /* the sun: yellow on top, magenta below, cut by stripes */
            if (!(k > 0.5f && fmodf(k * 14.0f, 1.0f) < (k - 0.45f) * 1.4f))
                r = 255, g = lerp_i(230, 40, k), b = lerp_i(60, 160, k);
        }
    } else {
        float t = v - hor, z = 1.0f / (t + 0.001f);
        r = 15, g = 0, b = 30;
        float horiz = fmodf(z * 0.05f, 1.0f), xx = (u - 0.5f) * z * 0.9f, vert = fabsf(xx - floorf(xx + 0.5f));
        float line = 0;
        if (horiz < 0.08f * (1.0f + t))
            line = 1;
        if (vert < 0.03f + 0.02f * t)
            line = 1;
        float fade = t < 0.04f ? t / 0.04f : 1.0f;
        r += (int)(line * fade * 60), g += (int)(line * fade * 230), b += (int)(line * fade * 255);
    }
    return rgb(r, g, b);
}

static void fx_backdrop(int fx, const uint32_t *src, const uint32_t *ref, int w, int h, uint32_t *d)
{
    int n = w * h;
    uint8_t *m = malloc((size_t)n * 2);
    if (!m) {
        memcpy(d, src, (size_t)n * 4);
        return;
    }
    make_mask(src, ref, w, h, m, m + n);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int i = y * w + x, a = m[i];
            uint32_t fg = src[i], bg;
            if (fx == CAMFX_GHOST) {
                int l = lum(src[i]);
                bg = ref ? ref[i] : rgb(l / 3, l / 3, l / 3 + 12);                         /* the empty room (no reference: the picture, dark) */
                fg = rgb((int)CR(fg) * 3 / 4 + 55, (int)CG(fg) * 3 / 4 + 60, (int)CB(fg) * 3 / 4 + 64);   /* a pale, cold figure */
                a = a * 6 / 10;
            } else {
                bg = fx == CAMFX_SUNSET ? bd_sunset(x, y, w, h) : fx == CAMFX_SPACE ? bd_space(x, y, w, h) : bd_neon(x, y, w, h);
            }
            d[i] = rgb(((int)CR(fg) * a + (int)CR(bg) * (255 - a)) / 255, ((int)CG(fg) * a + (int)CG(bg) * (255 - a)) / 255,
                       ((int)CB(fg) * a + (int)CB(bg) * (255 - a)) / 255);
        }
    free(m);
}

void camfx_apply(int fx, const uint32_t *src, int w, int h, uint32_t *dst)
{
    camfx_apply_ref(fx, src, NULL, w, h, dst);
}

int camfx_has_focus(int fx) { return fx >= CAMFX_MIRROR && fx <= CAMFX_VINTAGE; }

void camfx_apply_ref(int fx, const uint32_t *src, const uint32_t *ref, int w, int h, uint32_t *dst)
{
    camfx_apply_at(fx, src, ref, w, h, dst, 0.5f, 0.5f);
}

void camfx_apply_at(int fx, const uint32_t *src, const uint32_t *ref, int w, int h, uint32_t *dst, float fcx, float fcy)
{
    fcx = fcx < 0 ? 0 : fcx > 1 ? 1 : fcx;
    fcy = fcy < 0 ? 0 : fcy > 1 ? 1 : fcy;
    if (!src || !dst || w <= 0 || h <= 0)
        return;
    int n = w * h;
    if (w < 2 || h < 2 || fx <= CAMFX_NORMAL || fx >= CAMFX_COUNT) {
        memcpy(dst, src, (size_t)n * 4);
        return;
    }
    switch (fx) {
    case CAMFX_BW: fx_bw(src, n, dst); break;
    case CAMFX_SEPIA: fx_sepia(src, n, dst); break;
    case CAMFX_PENCIL: fx_pencil(src, w, h, dst); break;
    case CAMFX_COMIC: fx_comic(src, w, h, dst); break;
    case CAMFX_POP: fx_pop(src, w, h, dst); break;
    case CAMFX_THERMAL: fx_thermal(src, n, dst); break;
    case CAMFX_XRAY: fx_xray(src, n, dst); break;
    case CAMFX_GLOW: fx_glow(src, w, h, dst); break;
    case CAMFX_VINTAGE: fx_vintage(src, w, h, dst, fcx, fcy); break;
    case CAMFX_MIRROR: fx_mirror(src, w, h, dst, fcx); break;
    case CAMFX_KALEIDO: fx_kaleido(src, w, h, dst, fcx, fcy); break;
    case CAMFX_TUNNEL: fx_tunnel(src, w, h, dst, fcx, fcy); break;
    case CAMFX_SUNSET: case CAMFX_SPACE: case CAMFX_NEON: case CAMFX_GHOST: fx_backdrop(fx, src, ref, w, h, dst); break;
    default: fx_lens(fx, src, w, h, dst, fcx, fcy); break;
    }
}
