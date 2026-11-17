/* camfx - picture effects for mellagui-cam. See camfx.h. All integer arithmetic where it counts; floats only for the geometry of the distortions. */
#include "camfx.h"

#include "font.h"

#include <ctype.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CL(v) ((v) < 0 ? 0 : (v) > 255 ? 255 : (v))
#define CR(p) (((p) >> 16) & 255)
#define CG(p) (((p) >> 8) & 255)
#define CB(p) ((p) & 255)
#define PI_F 3.14159265f

static uint32_t hash2(int x, int y);
static inline int lerp_i(int a, int b, float t);

#define F_FOCUS 1                                       /* works round a point the user can move */
#define F_BACKDROP 2                                    /* wants the reference picture */
#define F_HIST 4                                        /* wants the pictures before */
#define F_FACE 8                                        /* wants the faces */
#define F_PIC 16                                        /* wants a backdrop picture */

static const struct { const char *id, *name; int flags; } k_fx[CAMFX_COUNT] = {
    [CAMFX_NORMAL] = { "normal", "Normal", 0 },       [CAMFX_BW] = { "bw", "Black & White", 0 },       [CAMFX_SEPIA] = { "sepia", "Sepia", 0 },
    [CAMFX_PENCIL] = { "pencil", "Pencil", 0 },       [CAMFX_COMIC] = { "comic", "Comic Book", 0 },    [CAMFX_POP] = { "pop", "Pop Art", 0 },
    [CAMFX_THERMAL] = { "thermal", "Thermal", 0 },    [CAMFX_XRAY] = { "xray", "X-Ray", 0 },           [CAMFX_GLOW] = { "glow", "Glow", 0 },
    [CAMFX_MIRROR] = { "mirror", "Mirror", F_FOCUS }, [CAMFX_KALEIDO] = { "kaleidoscope", "Kaleidoscope", F_FOCUS },
    [CAMFX_TUNNEL] = { "tunnel", "Light Tunnel", F_FOCUS },
    [CAMFX_SQUEEZE] = { "squeeze", "Squeeze", F_FOCUS }, [CAMFX_STRETCH] = { "stretch", "Stretch", F_FOCUS }, [CAMFX_BULGE] = { "bulge", "Bulge", F_FOCUS },
    [CAMFX_DENT] = { "dent", "Dent", F_FOCUS },       [CAMFX_TWIRL] = { "twirl", "Twirl", F_FOCUS },   [CAMFX_VINTAGE] = { "vintage", "Vintage", F_FOCUS },
    [CAMFX_NEGATIVE] = { "negative", "Negative", 0 }, [CAMFX_SOLARIZE] = { "solarize", "Solarize", 0 }, [CAMFX_DUOTONE] = { "duotone", "Duotone", 0 },
    [CAMFX_EMBOSS] = { "emboss", "Emboss", 0 },       [CAMFX_NIGHT] = { "night", "Night Vision", 0 },  [CAMFX_PIXEL] = { "pixel", "Pixels", 0 },
    [CAMFX_HALFTONE] = { "halftone", "Halftone", 0 }, [CAMFX_GLITCH] = { "glitch", "Glitch", 0 },      [CAMFX_ASCII] = { "ascii", "ASCII", 0 },
    [CAMFX_TRIPLE] = { "triple", "Triple", F_FOCUS }, [CAMFX_MANDALA] = { "mandala", "Mandala", F_FOCUS }, [CAMFX_FISHEYE] = { "fisheye", "Fisheye", F_FOCUS },
    [CAMFX_RIPPLE] = { "ripple", "Ripple", F_FOCUS }, [CAMFX_WATER] = { "water", "Water", F_FOCUS },   [CAMFX_TILTSHIFT] = { "tiltshift", "Tilt Shift", F_FOCUS },
    [CAMFX_TRAILS] = { "trails", "Light Trails", F_HIST }, [CAMFX_ECHO] = { "echo", "Echo", F_HIST },  [CAMFX_MOTION] = { "motion", "Motion", F_HIST },
    [CAMFX_SUNSET] = { "sunset", "Sunset", F_BACKDROP }, [CAMFX_SPACE] = { "space", "Space", F_BACKDROP }, [CAMFX_NEON] = { "neon", "Neon", F_BACKDROP },
    [CAMFX_GHOST] = { "ghost", "Ghost", F_BACKDROP }, [CAMFX_RAIN] = { "rain", "Rainy Window", F_BACKDROP }, [CAMFX_SKY] = { "sky", "Blue Sky", F_BACKDROP },
    [CAMFX_MATRIX] = { "matrix", "Matrix", F_BACKDROP }, [CAMFX_TIMEWARP] = { "timewarp", "Time Warp", F_HIST },
    [CAMFX_PICTURE] = { "picture", "Your Picture", F_BACKDROP | F_PIC }, [CAMFX_ROOM] = { "room", "Tom's Room", F_BACKDROP | F_PIC },
    [CAMFX_HAT] = { "hat", "Top Hat", F_FACE }, [CAMFX_SHADES] = { "shades", "Sunglasses", F_FACE }, [CAMFX_MOUSTACHE] = { "moustache", "Moustache", F_FACE },
    [CAMFX_CLOWN] = { "clown", "Clown", F_FACE },
};

int camfx_is_backdrop(int fx) { return fx >= 0 && fx < CAMFX_COUNT && (k_fx[fx].flags & F_BACKDROP) != 0; }
int camfx_has_focus(int fx) { return fx >= 0 && fx < CAMFX_COUNT && (k_fx[fx].flags & F_FOCUS) != 0; }
int camfx_needs_history(int fx) { return fx >= 0 && fx < CAMFX_COUNT && (k_fx[fx].flags & F_HIST) != 0; }
int camfx_needs_faces(int fx) { return fx >= 0 && fx < CAMFX_COUNT && (k_fx[fx].flags & F_FACE) != 0; }
int camfx_needs_picture(int fx) { return fx >= 0 && fx < CAMFX_COUNT && (k_fx[fx].flags & F_PIC) != 0; }

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
    uint8_t *g = calloc((size_t)n, 2);
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
    uint8_t *g = calloc((size_t)n, 2);
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

/* ---- more colour effects ------------------------------------------------------------------------------------------------------ */

static void fx_negative(const uint32_t *s, int n, uint32_t *d)
{
    for (int i = 0; i < n; i++)
        d[i] = rgb(255 - (int)CR(s[i]), 255 - (int)CG(s[i]), 255 - (int)CB(s[i]));
}

static void fx_solarize(const uint32_t *s, int n, uint32_t *d)
{
    for (int i = 0; i < n; i++) {
        int r = (int)CR(s[i]), g = (int)CG(s[i]), b = (int)CB(s[i]);
        r = (r < 128 ? r : 255 - r) * 2;                                 /* the tent: the bright half is folded back */
        g = (g < 128 ? g : 255 - g) * 2;
        b = (b < 128 ? b : 255 - b) * 2;
        d[i] = rgb(r, g, b);
    }
}

static void fx_duotone(const uint32_t *s, int n, uint32_t *d)             /* navy -> magenta -> peach by brightness */
{
    for (int i = 0; i < n; i++) {
        int l = lum(s[i]);
        if (l < 128) {
            float t = (float)l / 127.0f;
            d[i] = rgb(lerp_i(25, 225, t), lerp_i(12, 40, t), lerp_i(80, 140, t));
        } else {
            float t = (float)(l - 128) / 127.0f;
            d[i] = rgb(lerp_i(225, 255, t), lerp_i(40, 205, t), lerp_i(140, 120, t));
        }
    }
}

static void fx_emboss(const uint32_t *s, int w, int h, uint32_t *d)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int x0 = x > 0 ? x - 1 : 0, x1 = x + 1 < w ? x + 1 : w - 1, y0 = y > 0 ? y - 1 : 0, y1 = y + 1 < h ? y + 1 : h - 1;
            int v = 128 + (lum(s[(size_t)y1 * (size_t)w + (size_t)x1]) - lum(s[(size_t)y0 * (size_t)w + (size_t)x0])) * 2;
            v = CL(v);
            d[(size_t)y * (size_t)w + (size_t)x] = rgb(v, v, v);
        }
}

/* Night vision: brightened, green, noisy, with scan lines and a dark ring round the edge of the goggles. */
static void fx_night(const uint32_t *s, int w, int h, uint32_t *d)
{
    float cx = (float)w / 2.0f, cy = (float)h / 2.0f, rmax = sqrtf(cx * cx + cy * cy);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v = lum(s[(size_t)y * (size_t)w + (size_t)x]) * 3 / 2 + 40;
            v = CL(v + (int)(hash2(x, y) & 31) - 16);
            float rr = sqrtf(((float)x - cx) * ((float)x - cx) + ((float)y - cy) * ((float)y - cy)) / rmax;
            float t = rr < 0.55f ? 0.0f : (rr - 0.55f) / 0.45f;
            v = v * (256 - (int)(t * t * 230.0f)) / 256;
            if (y % 3 == 0)
                v = v * 80 / 100;
            d[(size_t)y * (size_t)w + (size_t)x] = rgb(v * 18 / 100, v, v * 30 / 100);
        }
}

/* Big blocks of one colour in an 8 bit palette (3 bits red, 3 green, 2 blue). */
static void fx_pixel(const uint32_t *s, int w, int h, uint32_t *d)
{
    int bs = w / 64 < 4 ? 4 : w / 64;
    for (int by = 0; by < h; by += bs)
        for (int bx = 0; bx < w; bx += bs) {
            int x1 = bx + bs < w ? bx + bs : w, y1 = by + bs < h ? by + bs : h, n = 0;
            long sr = 0, sg = 0, sb = 0;
            for (int y = by; y < y1; y++)
                for (int x = bx; x < x1; x++, n++) {
                    uint32_t p = s[(size_t)y * (size_t)w + (size_t)x];
                    sr += CR(p), sg += CG(p), sb += CB(p);
                }
            int r = (int)(sr / n) >> 5, g = (int)(sg / n) >> 5, b = (int)(sb / n) >> 6;
            uint32_t c = rgb(r * 255 / 7, g * 255 / 7, b * 255 / 3);
            for (int y = by; y < y1; y++)
                for (int x = bx; x < x1; x++)
                    d[(size_t)y * (size_t)w + (size_t)x] = c;
        }
}

/* Dots of ink on cream paper, on a grid turned 45 degrees; the dot is bigger where the picture is darker. */
static void fx_halftone(const uint32_t *s, int w, int h, uint32_t *d)
{
    float cs = (float)w / 80.0f < 5.0f ? 5.0f : (float)w / 80.0f, c = 0.70710678f, sn = 0.70710678f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float u = ((float)x * c + (float)y * sn) / cs, v = (-(float)x * sn + (float)y * c) / cs;
            float cu = floorf(u) + 0.5f, cv = floorf(v) + 0.5f;
            float px = (cu * c - cv * sn) * cs, py = (cu * sn + cv * c) * cs;                    /* the dot's centre, back in the picture */
            float l = (float)lum(sample(s, w, h, px, py)) / 255.0f, rad = sqrtf(1.0f - l) * 0.72f;
            float dist = sqrtf((u - cu) * (u - cu) + (v - cv) * (v - cv)), cov = (rad - dist) * cs + 0.5f;
            cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
            int k = (int)(cov * 256.0f);
            d[(size_t)y * (size_t)w + (size_t)x] = rgb((244 * (256 - k) + 28 * k) / 256, (238 * (256 - k) + 24 * k) / 256, (222 * (256 - k) + 30 * k) / 256);
        }
}

/* A broken signal: bands of rows slide sideways, the colours are pulled apart, now and then a band is inverted, scan lines. */
static void fx_glitch(const uint32_t *s, int w, int h, uint32_t *d)
{
    int bandh = h / 36 < 3 ? 3 : h / 36, split = w / 110 < 2 ? 2 : w / 110;
    for (int y = 0; y < h; y++) {
        uint32_t hb = hash2(y / bandh, 91);
        int shift = hb % 4 == 0 ? ((int)((hb >> 8) % 61) - 30) * w / 320 : 0, ss = split + shift / 4;
        bool inv = hb % 29 == 0;
        const uint32_t *row = s + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            int xr = x + shift + ss, xg = x + shift, xb = x + shift - ss;
            xr = xr < 0 ? 0 : xr > w - 1 ? w - 1 : xr;
            xg = xg < 0 ? 0 : xg > w - 1 ? w - 1 : xg;
            xb = xb < 0 ? 0 : xb > w - 1 ? w - 1 : xb;
            int r = (int)CR(row[xr]), g = (int)CG(row[xg]), b = (int)CB(row[xb]);
            if (inv)
                r = 255 - r, g = 255 - g, b = 255 - b;
            if (y & 1)
                r = r * 88 / 100, g = g * 88 / 100, b = b * 88 / 100;
            d[(size_t)y * (size_t)w + (size_t)x] = rgb(r, g, b);
        }
    }
}

/* The picture as letters of the built-in font: each cell shows a character chosen by its brightness, in green with a little of the picture's colour. */
static void fx_ascii(const uint32_t *s, int w, int h, uint32_t *d)
{
    static const char ramp[] = " .:-=+*#%@";
    int cw = w / 96 < 6 ? 6 : w / 96, ch = cw * 2;
    for (int cy = 0; cy < h; cy += ch)
        for (int cx = 0; cx < w; cx += cw) {
            long sr = 0, sg = 0, sb = 0;
            int n = 0;
            for (int j = 0; j < 3; j++)
                for (int i = 0; i < 3; i++, n++) {
                    int x = cx + (2 * i + 1) * cw / 6, y = cy + (2 * j + 1) * ch / 6;
                    uint32_t p = s[(size_t)(y < h ? y : h - 1) * (size_t)w + (size_t)(x < w ? x : w - 1)];
                    sr += CR(p), sg += CG(p), sb += CB(p);
                }
            int ar = (int)(sr / n), ag = (int)(sg / n), ab = (int)(sb / n);
            uint32_t avg = rgb(ar, ag, ab);
            int l = lum(avg);
            const uint8_t *gl = lm_font_glyph((uint32_t)(unsigned char)ramp[l * 9 / 255]);
            int bright = 70 + l * 185 / 255;
            uint32_t on = rgb(bright * (10 + ar * 40 / 255) / 100, bright, bright * (10 + ab * 40 / 255) / 100), off = rgb(0, l / 14, 0);
            for (int py = 0; py < ch && cy + py < h; py++)
                for (int px = 0; px < cw && cx + px < w; px++) {
                    int gx = px * 8 / cw, gy = py * 16 / ch;
                    d[(size_t)(cy + py) * (size_t)w + (size_t)(cx + px)] = ((gl[gy] >> (7 - gx)) & 1) ? on : off;
                }
        }
}

/* ---- more lenses (they work round the focus point) ---------------------------------------------------------------------------- */

/* A lens over the whole picture: the middle is magnified, the corners stay where they are. */
static void fx_fisheye(const uint32_t *s, int w, int h, uint32_t *d, float fcx, float fcy)
{
    float cx = fcx * (float)(w - 1), cy = fcy * (float)(h - 1);
    float mx = cx > (float)(w - 1) - cx ? cx : (float)(w - 1) - cx, my = cy > (float)(h - 1) - cy ? cy : (float)(h - 1) - cy, rmax = sqrtf(mx * mx + my * my);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float dx = (float)x - cx, dy = (float)y - cy, rr = sqrtf(dx * dx + dy * dy) / (rmax > 1 ? rmax : 1.0f), f = 0.55f + 0.45f * rr * rr;
            d[(size_t)y * (size_t)w + (size_t)x] = sample(s, w, h, cx + dx * f, cy + dy * f);
        }
}

/* Rings that run out from the point, like a stone in a pond. */
static void fx_ripple(const uint32_t *s, int w, int h, uint32_t *d, float fcx, float fcy)
{
    float cx = fcx * (float)(w - 1), cy = fcy * (float)(h - 1), amp0 = (float)h / 55.0f, lam = (float)h / 28.0f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float dx = (float)x - cx, dy = (float)y - cy, r = sqrtf(dx * dx + dy * dy), sx = (float)x, sy = (float)y;
            if (r > 0.5f) {
                float off = amp0 * expf(-r / ((float)h * 0.9f)) * sinf(r / lam * 1.5f);
                sx += dx / r * off;
                sy += dy / r * off;
            }
            d[(size_t)y * (size_t)w + (size_t)x] = sample(s, w, h, sx, sy);
        }
}

/* The picture above the horizon (at the point's height) is as it is, below it is its wavy, blue reflection. */
static void fx_water(const uint32_t *s, int w, int h, uint32_t *d, float fcy)
{
    float y0 = fcy * (float)(h - 1), span = (float)h - y0 + 1.0f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            if ((float)y <= y0) {
                d[i] = s[i];
                continue;
            }
            float dd = (float)y - y0, amp = 1.5f + dd * 0.04f, sx = (float)x + amp * sinf(dd * 0.45f + (float)x * 0.02f);
            uint32_t p = sample(s, w, h, sx, y0 - dd);
            int k = (int)((0.15f + 0.35f * dd / span) * 256.0f);
            d[i] = rgb(((int)CR(p) * (256 - k) + 20 * k) / 256, ((int)CG(p) * (256 - k) + 60 * k) / 256, ((int)CB(p) * (256 - k) + 110 * k) / 256);
        }
}

/* The picture is sharp in a band at the point's height and blurred above and below, a little more colourful: a miniature. */
static void fx_tiltshift(const uint32_t *s, int w, int h, uint32_t *d, float fcy)
{
    int sw = (w + 3) / 4, sh = (h + 3) / 4;                                 /* the blur is made on a quarter-size copy (a big radius on a big picture is slow) */
    uint32_t *sm = malloc((size_t)sw * (size_t)sh * 8);
    if (!sm) {
        memcpy(d, s, (size_t)w * (size_t)h * 4);
        return;
    }
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            int r = 0, g = 0, b = 0, n = 0;
            for (int j = 0; j < 4 && y * 4 + j < h; j++)
                for (int i = 0; i < 4 && x * 4 + i < w; i++, n++) {
                    uint32_t p = s[(size_t)(y * 4 + j) * (size_t)w + (size_t)(x * 4 + i)];
                    r += (int)CR(p), g += (int)CG(p), b += (int)CB(p);
                }
            sm[(size_t)y * (size_t)sw + (size_t)x] = rgb(r / n, g / n, b / n);
        }
    int rad = sh / 40 < 2 ? 2 : sh / 40;
    box_blur(sm, sm + (size_t)sw * (size_t)sh, sw, sh, rad);
    box_blur(sm, sm + (size_t)sw * (size_t)sh, sw, sh, rad);
    float cy = fcy * (float)(h - 1), band = (float)h * 0.12f, ramp = (float)h * 0.28f;
    for (int y = 0; y < h; y++) {
        float t = (fabsf((float)y - cy) - band) / ramp;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        t = t * t * (3.0f - 2.0f * t);
        int k = (int)(t * 256.0f);
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            uint32_t bl = k ? sample(sm, sw, sh, ((float)x - 1.5f) / 4.0f, ((float)y - 1.5f) / 4.0f) : s[i];
            int r0 = ((int)CR(s[i]) * (256 - k) + (int)CR(bl) * k) / 256, g0 = ((int)CG(s[i]) * (256 - k) + (int)CG(bl) * k) / 256;
            int b0 = ((int)CB(s[i]) * (256 - k) + (int)CB(bl) * k) / 256, l = (r0 * 77 + g0 * 150 + b0 * 29) >> 8;
            d[i] = rgb(l + (r0 - l) * 5 / 4, l + (g0 - l) * 5 / 4, l + (b0 - l) * 5 / 4);
        }
    }
    free(sm);
}

/* ---- effects that use the pictures before ------------------------------------------------------------------------------------- */

/* Bright things leave a trail that fades: each pixel is the brighter of the new picture and the last result, a little dimmer. */
static void fx_trails(const uint32_t *s, int n, uint32_t *d, const struct camfx_hist *hs)
{
    const uint32_t *po = hs ? hs->prev_out : NULL;
    for (int i = 0; i < n; i++) {
        if (!po) {
            d[i] = s[i];
            continue;
        }
        int r = (int)CR(po[i]) * 237 / 256, g = (int)CG(po[i]) * 237 / 256, b = (int)CB(po[i]) * 237 / 256;
        d[i] = rgb((int)CR(s[i]) > r ? (int)CR(s[i]) : r, (int)CG(s[i]) > g ? (int)CG(s[i]) : g, (int)CB(s[i]) > b ? (int)CB(s[i]) : b);
    }
}

/* A slow average: what moves is smeared into a ghost, what stands still stays sharp. */
static void fx_echo(const uint32_t *s, int n, uint32_t *d, const struct camfx_hist *hs)
{
    const uint32_t *po = hs ? hs->prev_out : NULL;
    for (int i = 0; i < n; i++) {
        if (!po) {
            d[i] = s[i];
            continue;
        }
        int c[3];
        for (int k = 0; k < 3; k++) {
            int sh = 16 - 8 * k, pv = (int)((po[i] >> sh) & 255), sv = (int)((s[i] >> sh) & 255), df = sv - pv;
            c[k] = pv + (df * 77 + (df > 0 ? 255 : -255)) / 256;
        }
        d[i] = rgb(c[0], c[1], c[2]);
    }
}

/* What changed since the picture before on a dark grey picture: cyan where it got brighter, magenta where it got darker. */
static void fx_motion(const uint32_t *s, int n, uint32_t *d, const struct camfx_hist *hs)
{
    const uint32_t *ps = hs ? hs->prev_src : NULL;
    for (int i = 0; i < n; i++) {
        int base = lum(s[i]) / 4, a = 0, up = 1;
        if (ps) {
            int dl = lum(s[i]) - lum(ps[i]);
            up = dl > 0;
            a = dl < 0 ? -dl : dl;
            a = a < 6 ? 0 : CL((a - 5) * 5);
        }
        d[i] = up ? rgb(base, base + a, base + a) : rgb(base + a, base, base + a * 4 / 5);
    }
}

/* A slit scan: the top row is the picture now, the rows below are older and older pictures (up to CAMFX_RING of them). */
static void fx_timewarp(const uint32_t *s, int w, int h, uint32_t *d, const struct camfx_hist *hs)
{
    int nr = hs && hs->ring ? hs->nring : 0;
    for (int y = 0; y < h; y++) {
        int k = nr > 0 ? y * (nr + 1) / h : 0;
        const uint32_t *row = (k == 0 || !hs->ring[k - 1] ? s : hs->ring[k - 1]) + (size_t)y * (size_t)w;
        memcpy(d + (size_t)y * (size_t)w, row, (size_t)w * 4);
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

static void fx_kaleido(const uint32_t *s, int w, int h, uint32_t *d, float fcx, float fcy, int nseg)
{
    float cx = fcx * (float)(w - 1), cy = fcy * (float)(h - 1), seg = 2.0f * PI_F / (float)nseg;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float dx = (float)x - cx, dy = (float)y - cy, r = sqrtf(dx * dx + dy * dy) * (nseg > 6 ? 0.55f : 0.8f);
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

static inline int lerp_i(int a, int b, float t)
{
    return a + (int)((float)(b - a) * t);
}

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

/* No picture chosen yet: a soft grey-blue wall. */
static uint32_t bd_plain(int x, int y, int w, int h)
{
    (void)x;
    (void)w;
    float v = (float)y / (float)h;
    return rgb(lerp_i(70, 30, v), lerp_i(80, 38, v), lerp_i(100, 55, v));
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

static float smooth01(float t)
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3.0f - 2.0f * t);
}

static float vnoise(float x, float y)                                                  /* value noise, 0..1 */
{
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = smooth01(x - (float)xi), fy = smooth01(y - (float)yi);
    float a = (float)(hash2(xi, yi) & 0xffff), b = (float)(hash2(xi + 1, yi) & 0xffff), c = (float)(hash2(xi, yi + 1) & 0xffff), e = (float)(hash2(xi + 1, yi + 1) & 0xffff);
    return ((a + (b - a) * fx) * (1.0f - fy) + (c + (e - c) * fx) * fy) / 65535.0f;
}

static float fbm(float x, float y)
{
    float v = 0, amp = 0.5f;
    for (int o = 0; o < 4; o++, amp *= 0.5f, x *= 2.03f, y *= 2.03f)
        v += amp * vnoise(x, y);
    return v;
}

/* A wet window on a city at night: soft discs of coloured light, streaks and beads of rain. The same picture at any size. */
static uint32_t bd_rain(int x, int y, int w, int h)
{
    float fx = (float)x * 640.0f / (float)w, fy = (float)y * 480.0f / (float)h, v = fy / 480.0f;
    float r = 10.0f + 14.0f * v, g = 14.0f + 8.0f * v, b = 30.0f + 16.0f * v;
    static const float pal[4][3] = { { 255, 190, 90 }, { 255, 90, 90 }, { 110, 170, 255 }, { 255, 120, 200 } };
    int ci0 = (int)floorf(fx / 48.0f), cj0 = (int)floorf(fy / 48.0f);
    for (int cj = cj0 - 1; cj <= cj0 + 1; cj++)
        for (int ci = ci0 - 1; ci <= ci0 + 1; ci++) {
            uint32_t hs = hash2(ci, cj);
            if ((hs & 7) >= 4)
                continue;
            float px = (float)ci * 48.0f + (float)((hs >> 3) % 48), py = (float)cj * 48.0f + (float)((hs >> 9) % 48), rad = 10.0f + (float)((hs >> 15) % 18);
            float dd = sqrtf((fx - px) * (fx - px) + (fy - py) * (fy - py));
            if (dd >= rad)
                continue;
            float al = 0.14f + 0.16f * smooth01((dd - rad * 0.55f) / (rad * 0.45f)) * (1.0f - smooth01((dd - rad * 0.95f) / (rad * 0.05f)));
            const float *c = pal[(hs >> 20) % 4];
            r += c[0] * al, g += c[1] * al, b += c[2] * al;
        }
    float u = fx - fy * 0.06f;                                                          /* streaks run down a little slanted */
    int col = (int)floorf(u / 6.0f);
    uint32_t hc = hash2(col, 77);
    if (fabsf(u - ((float)col * 6.0f + (float)(hc % 6))) < 0.8f) {
        float period = 230.0f + (float)(hc % 140), ph = fmodf(fy + (float)((hc >> 8) % 400), period), len = 30.0f + (float)((hc >> 16) % 60);
        if (ph < len) {
            float k = 0.25f + 0.5f * (1.0f - ph / len);
            r += 190 * k, g += 210 * k, b += 255 * k;
        }
    }
    int bi = (int)floorf(fx / 40.0f), bj = (int)floorf(fy / 40.0f);                      /* a bead of water in some cells, lit at its upper left */
    uint32_t hb = hash2(bi, bj + 5000);
    if ((hb & 3) == 0) {
        float px = (float)bi * 40.0f + 8.0f + (float)((hb >> 4) % 24), py = (float)bj * 40.0f + 8.0f + (float)((hb >> 10) % 24), rad = 2.5f + (float)((hb >> 16) % 3);
        float dd = sqrtf((fx - px) * (fx - px) + (fy - py) * (fy - py));
        if (dd < rad) {
            float hl = 1.0f - sqrtf((fx - (px - rad * 0.35f)) * (fx - (px - rad * 0.35f)) + (fy - (py - rad * 0.35f)) * (fy - (py - rad * 0.35f))) / rad;
            hl = hl < 0 ? 0 : hl;
            r += 40 + 150 * hl, g += 50 + 150 * hl, b += 70 + 150 * hl;
        }
    }
    return rgb((int)r, (int)g, (int)b);
}

/* A blue sky with clouds over a meadow. */
static uint32_t bd_sky(int x, int y, int w, int h)
{
    float fx = (float)x * 640.0f / (float)w, fy = (float)y * 480.0f / (float)h, u = fx / 640.0f, v = fy / 480.0f;
    float hor = 0.78f + 0.025f * sinf(u * 5.0f + 1.0f) + 0.012f * sinf(u * 13.0f);
    if (v < hor) {
        float t = v / 0.78f;
        float r = (float)lerp_i(40, 180, t), g = (float)lerp_i(110, 215, t), b = (float)lerp_i(215, 248, t);
        float n = fbm(fx / 150.0f + 3.7f, fy / 75.0f + 1.3f), c = smooth01((n - 0.47f) / 0.28f);
        if (c > 0) {                                                                    /* (the sky between the clouds costs one noise, not two) */
            float sh = fbm(fx / 150.0f + 3.7f, fy / 75.0f + 1.55f), shade = c * smooth01((n - sh) * 6.0f + 0.2f) * 38.0f;
            r += (250.0f - r) * c - shade, g += (252.0f - g) * c - shade, b += (255.0f - b) * c - shade * 0.6f;
        }
        return rgb((int)r, (int)g, (int)b);
    }
    float k = (v - hor) / (1.0f - hor + 0.001f), stripe = 0.93f + 0.07f * sinf(k * 40.0f + sinf(u * 6.0f) * 2.0f);
    return rgb((int)((float)lerp_i(100, 45, k) * stripe), (int)((float)lerp_i(175, 120, k) * stripe), (int)((float)lerp_i(65, 35, k) * stripe));
}

/* Letters falling down green columns, the head white. A cell is 10 x 16 of a 640 x 480 picture. */
static uint32_t bd_matrix(int x, int y, int w, int h)
{
    float fx = (float)x * 640.0f / (float)w, fy = (float)y * 480.0f / (float)h;
    int col = (int)(fx / 10.0f), row = (int)(fy / 16.0f);
    float b = 0;
    bool head = false;
    for (int st = 0; st < 2; st++) {
        uint32_t hs = hash2(col, 1 + st);
        int len = 10 + (int)((hs >> 12) % 14), hd = (int)(hs % 54) - 8, dist = hd - row;
        if (st == 1)
            hd = (int)((hs >> 4) % 54) - 8, dist = hd - row;
        if (dist >= 0 && dist < len) {
            float k = 1.0f - (float)dist / (float)len;
            if (k > b)
                b = k;
            if (dist == 0)
                head = true;
        }
    }
    if (b <= 0)
        return rgb(0, 5, 2);
    const uint8_t *gl = lm_font_glyph(33 + hash2(col, row + 300) % 94);
    int px = (int)(fmodf(fx, 10.0f) * 0.8f), py = (int)fmodf(fy, 16.0f);
    bool on = (gl[py] >> (7 - px)) & 1;
    if (!on)
        return rgb(0, (int)(8 + b * 26), (int)(3 + b * 8));
    if (head)
        return rgb(200, 255, 210);
    return rgb((int)(b * 40), (int)(70 + b * 185), (int)(30 + b * 60));
}

/* ---- effects drawn on the faces the program found ----------------------------------------------------------------------------- */

/* Blends the colour (col0 at the top of the shape, col1 at the bottom) over a rounded box or an ellipse, turned by `rot`, with a soft edge.
 * (cx, cy) the middle, (hw, hh) the half sizes, rad the corner radius (an ellipse: rad < 0), alpha 0..1. */
static void paint_shape(uint32_t *d, int w, int h, float cx, float cy, float hw, float hh, float rad, float rot, uint32_t col0, uint32_t col1, float alpha)
{
    float R = (hw > hh ? hw : hh) + 2.0f, cs = cosf(rot), sn = sinf(rot);
    int x0 = (int)floorf(cx - R), x1 = (int)ceilf(cx + R), y0 = (int)floorf(cy - R), y1 = (int)ceilf(cy + R);
    x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > w - 1 ? w - 1 : x1, y1 = y1 > h - 1 ? h - 1 : y1;
    if (hw < 0.5f || hh < 0.5f)
        return;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy, u = dx * cs + dy * sn, v = -dx * sn + dy * cs, dist;
            if (rad < 0) {                                                       /* an ellipse: the distance estimated by the gradient of its equation */
                float k0 = sqrtf(u * u / (hw * hw) + v * v / (hh * hh)), k1 = sqrtf(u * u / (hw * hw * hw * hw) + v * v / (hh * hh * hh * hh));
                dist = k1 > 1e-9f ? k0 * (k0 - 1.0f) / k1 : -(hw < hh ? hw : hh);
            } else {                                                             /* a rounded box */
                float qx = fabsf(u) - (hw - rad), qy = fabsf(v) - (hh - rad);
                float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
                dist = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - rad;
            }
            float cov = 0.5f - dist;
            if (cov <= 0)
                continue;
            cov = (cov > 1 ? 1 : cov) * alpha;
            float t = (v + hh) / (2.0f * hh);
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            int r = lerp_i((int)CR(col0), (int)CR(col1), t), g = lerp_i((int)CG(col0), (int)CG(col1), t), b = lerp_i((int)CB(col0), (int)CB(col1), t);
            uint32_t *p = &d[(size_t)y * (size_t)w + (size_t)x];
            int k = (int)(cov * 256.0f);
            *p = rgb(((int)CR(*p) * (256 - k) + r * k) / 256, ((int)CG(*p) * (256 - k) + g * k) / 256, ((int)CB(*p) * (256 - k) + b * k) / 256);
        }
}

static void face_hat(uint32_t *d, int w, int h, const struct camfx_face *f)
{
    float cx = f->x + f->w / 2, by = f->y + 0.02f * f->h;                       /* the brim sits on the forehead, its middle at by */
    paint_shape(d, w, h, cx, by - 0.30f * f->h, 0.37f * f->w, 0.33f * f->h, 0.03f * f->h, 0, 0x2a2a30, 0x0e0e12, 1.0f);                 /* the crown */
    paint_shape(d, w, h, cx - 0.22f * f->w, by - 0.30f * f->h, 0.025f * f->w, 0.28f * f->h, 0.02f * f->w, 0, 0x6a6a74, 0x30303a, 0.55f); /* a light stripe */
    paint_shape(d, w, h, cx, by - 0.07f * f->h, 0.375f * f->w, 0.055f * f->h, 0.01f * f->h, 0, 0xb01a2c, 0x7a0f1d, 1.0f);              /* the red band */
    paint_shape(d, w, h, cx, by + 0.012f * f->h, 0.64f * f->w, 0.072f * f->h, -1, 0, 0x08080a, 0x08080a, 0.45f);                        /* the brim's shadow */
    paint_shape(d, w, h, cx, by, 0.64f * f->w, 0.07f * f->h, -1, 0, 0x2c2c32, 0x0a0a0d, 1.0f);                                          /* the brim */
}

static void face_shades(uint32_t *d, int w, int h, const struct camfx_face *f)
{
    float cx = f->x + f->w / 2, ey = f->y + 0.37f * f->h, dx = 0.215f * f->w, lw = 0.175f * f->w, lh = 0.105f * f->h;
    paint_shape(d, w, h, cx, ey, 0.07f * f->w, 0.016f * f->h, 0.008f * f->h, 0, 0x101012, 0x101012, 1.0f);                              /* the bridge */
    for (int k = -1; k <= 1; k += 2) {
        float ex = cx + (float)k * dx;
        paint_shape(d, w, h, cx + (float)k * 0.43f * f->w, ey - 0.005f * f->h, 0.075f * f->w, 0.013f * f->h, 0.006f * f->h, 0, 0x101012, 0x101012, 1.0f);  /* the temple */
        paint_shape(d, w, h, ex, ey, lw + 0.012f * f->w, lh + 0.012f * f->h, 0.06f * f->h, 0, 0x0c0c0e, 0x0c0c0e, 1.0f);                /* the frame */
        paint_shape(d, w, h, ex, ey, lw, lh, 0.055f * f->h, 0, 0x34343c, 0x040406, 0.94f);                                              /* the lens */
        paint_shape(d, w, h, ex - 0.06f * f->w, ey - 0.03f * f->h, 0.03f * f->w, 0.012f * f->h, 0.01f * f->h, -0.5f, 0xffffff, 0xffffff, 0.35f);   /* a glare */
    }
}

static void face_moustache(uint32_t *d, int w, int h, const struct camfx_face *f)
{
    float cx = f->x + f->w / 2, my = f->y + 0.745f * f->h;
    for (int k = -1; k <= 1; k += 2) {
        float s = (float)k;
        paint_shape(d, w, h, cx + s * 0.135f * f->w, my, 0.17f * f->w, 0.052f * f->h, -1, s * 0.22f, 0x4a2c1a, 0x1c0e06, 1.0f);       /* the lobe, drooping outwards */
        paint_shape(d, w, h, cx + s * 0.30f * f->w, my - 0.03f * f->h, 0.05f * f->w, 0.042f * f->h, -1, -s * 0.5f, 0x3a2012, 0x1c0e06, 1.0f);   /* the curl */
    }
    paint_shape(d, w, h, cx, my - 0.012f * f->h, 0.08f * f->w, 0.04f * f->h, -1, 0, 0x4a2c1a, 0x28160a, 1.0f);
}

static void face_clown(uint32_t *d, int w, int h, const struct camfx_face *f)
{
    static const uint32_t wig[6] = { 0xff7a14, 0xffd21e, 0x32be3c, 0x1ec8e6, 0xc83cc8, 0xe62828 };
    float cx = f->x + f->w / 2, cy = f->y + 0.40f * f->h;
    for (int i = 0; i < 11; i++) {                                              /* an arc of curls over the head and down the sides */
        float a = 3.14159265f * (0.88f + 1.24f * (float)i / 10.0f);              /* from the left side over the top to the right side */
        float px = cx + cosf(a) * 0.64f * f->w, py = cy + sinf(a) * 0.56f * f->h;
        paint_shape(d, w, h, px, py, 0.21f * f->w, 0.21f * f->w, -1, 0, wig[i % 6], wig[i % 6] - ((wig[i % 6] >> 2) & 0x3f3f3f), 1.0f);
    }
    float nx = cx, ny = f->y + 0.62f * f->h, r = 0.085f * f->w;
    paint_shape(d, w, h, nx, ny, r, r, -1, 0, 0xff4a52, 0xb80f1c, 1.0f);                                               /* the nose */
    paint_shape(d, w, h, nx - r * 0.35f, ny - r * 0.35f, r * 0.28f, r * 0.2f, -1, -0.6f, 0xffffff, 0xffffff, 0.55f);   /* its shine */
}

static void fx_faces(int fx, const uint32_t *s, int w, int h, uint32_t *d, const struct camfx_hist *hs)
{
    memcpy(d, s, (size_t)w * (size_t)h * 4);
    for (int i = 0; hs && hs->faces && i < hs->nfaces; i++) {
        const struct camfx_face *f = &hs->faces[i];
        if (!(f->w >= 8.0f && f->h >= 8.0f))
            continue;
        switch (fx) {
        case CAMFX_HAT: face_hat(d, w, h, f); break;
        case CAMFX_SHADES: face_shades(d, w, h, f); break;
        case CAMFX_MOUSTACHE: face_moustache(d, w, h, f); break;
        default: face_clown(d, w, h, f); break;
        }
    }
}

static void fx_backdrop(int fx, const uint32_t *src, const uint32_t *ref, const struct camfx_hist *hs, int w, int h, uint32_t *d)
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
                switch (fx) {
                case CAMFX_SUNSET: bg = bd_sunset(x, y, w, h); break;
                case CAMFX_SPACE: bg = bd_space(x, y, w, h); break;
                case CAMFX_RAIN: bg = bd_rain(x, y, w, h); break;
                case CAMFX_SKY: bg = bd_sky(x, y, w, h); break;
                case CAMFX_MATRIX: bg = bd_matrix(x, y, w, h); break;
                case CAMFX_PICTURE: bg = hs && hs->pic ? (hs->pic[i] | 0xff000000u) : bd_plain(x, y, w, h); break;
                case CAMFX_ROOM: bg = hs && hs->pic ? (hs->pic[i] | 0xff000000u) : bd_matrix(x, y, w, h); break;
                default: bg = bd_neon(x, y, w, h); break;
                }
            }
            d[i] = rgb(((int)CR(fg) * a + (int)CR(bg) * (255 - a)) / 255, ((int)CG(fg) * a + (int)CG(bg) * (255 - a)) / 255,
                       ((int)CB(fg) * a + (int)CB(bg) * (255 - a)) / 255);
        }
    free(m);
}

void camfx_apply(int fx, const uint32_t *src, int w, int h, uint32_t *dst)
{
    camfx_apply_h(fx, src, NULL, NULL, w, h, dst, 0.5f, 0.5f);
}

void camfx_apply_ref(int fx, const uint32_t *src, const uint32_t *ref, int w, int h, uint32_t *dst)
{
    camfx_apply_h(fx, src, ref, NULL, w, h, dst, 0.5f, 0.5f);
}

void camfx_apply_at(int fx, const uint32_t *src, const uint32_t *ref, int w, int h, uint32_t *dst, float fcx, float fcy)
{
    camfx_apply_h(fx, src, ref, NULL, w, h, dst, fcx, fcy);
}

void camfx_apply_h(int fx, const uint32_t *src, const uint32_t *ref, const struct camfx_hist *hist, int w, int h, uint32_t *dst, float fcx, float fcy)
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
    case CAMFX_KALEIDO: fx_kaleido(src, w, h, dst, fcx, fcy, 6); break;
    case CAMFX_TRIPLE: fx_kaleido(src, w, h, dst, fcx, fcy, 3); break;
    case CAMFX_MANDALA: fx_kaleido(src, w, h, dst, fcx, fcy, 10); break;
    case CAMFX_TUNNEL: fx_tunnel(src, w, h, dst, fcx, fcy); break;
    case CAMFX_NEGATIVE: fx_negative(src, n, dst); break;
    case CAMFX_SOLARIZE: fx_solarize(src, n, dst); break;
    case CAMFX_DUOTONE: fx_duotone(src, n, dst); break;
    case CAMFX_EMBOSS: fx_emboss(src, w, h, dst); break;
    case CAMFX_NIGHT: fx_night(src, w, h, dst); break;
    case CAMFX_PIXEL: fx_pixel(src, w, h, dst); break;
    case CAMFX_HALFTONE: fx_halftone(src, w, h, dst); break;
    case CAMFX_GLITCH: fx_glitch(src, w, h, dst); break;
    case CAMFX_ASCII: fx_ascii(src, w, h, dst); break;
    case CAMFX_FISHEYE: fx_fisheye(src, w, h, dst, fcx, fcy); break;
    case CAMFX_RIPPLE: fx_ripple(src, w, h, dst, fcx, fcy); break;
    case CAMFX_WATER: fx_water(src, w, h, dst, fcy); break;
    case CAMFX_TILTSHIFT: fx_tiltshift(src, w, h, dst, fcy); break;
    case CAMFX_TRAILS: fx_trails(src, n, dst, hist); break;
    case CAMFX_ECHO: fx_echo(src, n, dst, hist); break;
    case CAMFX_MOTION: fx_motion(src, n, dst, hist); break;
    case CAMFX_TIMEWARP: fx_timewarp(src, w, h, dst, hist); break;
    case CAMFX_SUNSET: case CAMFX_SPACE: case CAMFX_NEON: case CAMFX_GHOST: case CAMFX_RAIN: case CAMFX_SKY: case CAMFX_MATRIX: case CAMFX_PICTURE: case CAMFX_ROOM:
        fx_backdrop(fx, src, ref, hist, w, h, dst);
        break;
    case CAMFX_HAT: case CAMFX_SHADES: case CAMFX_MOUSTACHE: case CAMFX_CLOWN: fx_faces(fx, src, w, h, dst, hist); break;
    default: fx_lens(fx, src, w, h, dst, fcx, fcy); break;
    }
}
