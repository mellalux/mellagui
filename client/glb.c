/* glb: a reader for glTF binary files with a skinned mesh, and the skinning. See glb.h. */
#include "glb.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "ffpipe.h"
#include "img.h"
#include "json.h"

#define FILE_MAX (400u * 1024u * 1024u)
#define VERTS_MAX 4000000
#define NODES_MAX 4096
#define JOINTS_MAX 255

/* ---- small helpers -------------------------------------------------------------------------------------------------------------- */

static void fail(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void fail(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* ---- 4 x 4 matrices (column major: m[column * 4 + row]) --------------------------------------------------------------------------- */

static void mat_identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat_mul(const float a[16], const float b[16], float out[16])
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++)
            r[c * 4 + row] = a[row] * b[c * 4] + a[4 + row] * b[c * 4 + 1] + a[8 + row] * b[c * 4 + 2] + a[12 + row] * b[c * 4 + 3];
    memcpy(out, r, sizeof(r));
}

static void quat_normalize(float q[4])
{
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l < 1e-12f) {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
        return;
    }
    for (int i = 0; i < 4; i++)
        q[i] /= l;
}

static void trs_matrix(const float t[3], const float r[4], const float s[3], float m[16])
{
    float x = r[0], y = r[1], z = r[2], w = r[3];
    float r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
    float r10 = 2 * (x * y + z * w), r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
    float r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);
    m[0] = r00 * s[0]; m[1] = r10 * s[0]; m[2] = r20 * s[0]; m[3] = 0;
    m[4] = r01 * s[1]; m[5] = r11 * s[1]; m[6] = r21 * s[1]; m[7] = 0;
    m[8] = r02 * s[2]; m[9] = r12 * s[2]; m[10] = r22 * s[2]; m[11] = 0;
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2]; m[15] = 1;
}

/* A matrix (as a node's "matrix") as translation, rotation and scale. */
static void decompose(const float m[16], float t[3], float r[4], float s[3])
{
    t[0] = m[12]; t[1] = m[13]; t[2] = m[14];
    s[0] = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    s[1] = sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    s[2] = sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    float a[9];
    for (int c = 0; c < 3; c++)
        for (int row = 0; row < 3; row++)
            a[row * 3 + c] = m[c * 4 + row] / (s[c] > 1e-12f ? s[c] : 1.0f);
    float tr = a[0] + a[4] + a[8];
    if (tr > 0.0f) {
        float k = sqrtf(tr + 1.0f) * 2.0f;
        r[3] = 0.25f * k; r[0] = (a[7] - a[5]) / k; r[1] = (a[2] - a[6]) / k; r[2] = (a[3] - a[1]) / k;
    } else if (a[0] > a[4] && a[0] > a[8]) {
        float k = sqrtf(1.0f + a[0] - a[4] - a[8]) * 2.0f;
        r[3] = (a[7] - a[5]) / k; r[0] = 0.25f * k; r[1] = (a[1] + a[3]) / k; r[2] = (a[2] + a[6]) / k;
    } else if (a[4] > a[8]) {
        float k = sqrtf(1.0f + a[4] - a[0] - a[8]) * 2.0f;
        r[3] = (a[2] - a[6]) / k; r[0] = (a[1] + a[3]) / k; r[1] = 0.25f * k; r[2] = (a[5] + a[7]) / k;
    } else {
        float k = sqrtf(1.0f + a[8] - a[0] - a[4]) * 2.0f;
        r[3] = (a[3] - a[1]) / k; r[0] = (a[2] + a[6]) / k; r[1] = (a[5] + a[7]) / k; r[2] = 0.25f * k;
    }
    quat_normalize(r);
}

/* ---- the file ---------------------------------------------------------------------------------------------------------------------- */

struct ctx {
    const uint8_t *file;
    size_t flen;
    const uint8_t *bin;
    size_t blen;
    struct json *root;
};

static const struct json *arr_at(const struct json *arr, int i)
{
    if (!arr || arr->type != JSON_ARR || i < 0)
        return NULL;
    const struct json *c = arr->child;
    while (c && i-- > 0)
        c = c->next;
    return c;
}

static int arr_len(const struct json *arr)
{
    int n = 0;
    if (arr && arr->type == JSON_ARR)
        for (const struct json *c = arr->child; c; c = c->next)
            n++;
    return n;
}

/* A view of accessor `ai`: where its first element is, the distance between elements and what they are. */
struct view {
    const uint8_t *base;
    size_t stride;
    int count, ncomp, ctype;
    bool normalized;
};

static bool acc_view(const struct ctx *c, int ai, struct view *v, char *err, size_t errsz)
{
    const struct json *a = arr_at(json_get(c->root, "accessors"), ai);
    if (!a || a->type != JSON_OBJ) {
        fail(err, errsz, "accessor %d is missing", ai);
        return false;
    }
    if (json_get(a, "sparse")) {
        fail(err, errsz, "sparse accessors are not supported");
        return false;
    }
    int count = (int)json_num(a, "count", -1), ctype = (int)json_num(a, "componentType", 0), bvi = (int)json_num(a, "bufferView", -1);
    const char *type = json_str(a, "type", "");
    int ncomp = !strcmp(type, "SCALAR") ? 1 : !strcmp(type, "VEC2") ? 2 : !strcmp(type, "VEC3") ? 3 : !strcmp(type, "VEC4") ? 4 : !strcmp(type, "MAT4") ? 16 : 0;
    size_t csz = ctype == 5120 || ctype == 5121 ? 1 : ctype == 5122 || ctype == 5123 ? 2 : ctype == 5125 || ctype == 5126 ? 4 : 0;
    if (count < 0 || count > VERTS_MAX * 4 || !ncomp || !csz || bvi < 0) {
        fail(err, errsz, "accessor %d is not usable", ai);
        return false;
    }
    const struct json *b = arr_at(json_get(c->root, "bufferViews"), bvi);
    if (!b || (int)json_num(b, "buffer", 0) != 0) {
        fail(err, errsz, "bufferView %d is missing (or not in the first buffer)", bvi);
        return false;
    }
    double boff = json_num(b, "byteOffset", 0), blen = json_num(b, "byteLength", -1), aoff = json_num(a, "byteOffset", 0), stride = json_num(b, "byteStride", 0);
    size_t esz = csz * (size_t)ncomp;
    if (stride == 0)
        stride = (double)esz;
    if (boff < 0 || blen < 0 || aoff < 0 || stride < (double)esz || boff + blen > (double)c->blen)
        goto bad;
    if (count > 0 && aoff + (double)(count - 1) * stride + (double)esz > blen)
        goto bad;
    v->base = c->bin + (size_t)boff + (size_t)aoff;
    v->stride = (size_t)stride;
    v->count = count;
    v->ncomp = ncomp;
    v->ctype = ctype;
    v->normalized = json_bool(a, "normalized", false);
    return true;
bad:
    fail(err, errsz, "accessor %d reaches outside the file's data", ai);
    return false;
}

static float read_comp(const uint8_t *p, int ctype, bool norm)
{
    switch (ctype) {
    case 5126: { float f; memcpy(&f, p, 4); return f; }
    case 5121: return norm ? (float)*p / 255.0f : (float)*p;
    case 5120: { int8_t x = (int8_t)*p; return norm ? fmaxf((float)x / 127.0f, -1.0f) : (float)x; }
    case 5123: { uint16_t x; memcpy(&x, p, 2); return norm ? (float)x / 65535.0f : (float)x; }
    case 5122: { int16_t x; memcpy(&x, p, 2); return norm ? fmaxf((float)x / 32767.0f, -1.0f) : (float)x; }
    case 5125: { uint32_t x; memcpy(&x, p, 4); return (float)x; }
    }
    return 0.0f;
}

static size_t comp_size(int ctype)
{
    return ctype == 5120 || ctype == 5121 ? 1 : ctype == 5122 || ctype == 5123 ? 2 : 4;
}

/* All the elements of accessor `ai` as floats (count * ncomp), expecting `want` components. */
static float *acc_floats(const struct ctx *c, int ai, int want, int *count, char *err, size_t errsz)
{
    struct view v;
    if (!acc_view(c, ai, &v, err, errsz))
        return NULL;
    if (v.ncomp != want) {
        fail(err, errsz, "accessor %d has %d components, %d wanted", ai, v.ncomp, want);
        return NULL;
    }
    float *out = malloc(((size_t)v.count * (size_t)want + 1) * sizeof(float));
    if (!out) {
        fail(err, errsz, "out of memory");
        return NULL;
    }
    size_t cs = comp_size(v.ctype);
    for (int i = 0; i < v.count; i++)
        for (int k = 0; k < want; k++)
            out[(size_t)i * (size_t)want + (size_t)k] = read_comp(v.base + (size_t)i * v.stride + (size_t)k * cs, v.ctype, v.normalized);
    if (count)
        *count = v.count;
    return out;
}

/* ---- the texture ------------------------------------------------------------------------------------------------------------------ */

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static void mkdir_p(char *path)
{
    for (char *p = path + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(path, 0700);
            *p = '/';
        }
    mkdir(path, 0700);
}

/* A WebP picture as a PNG file, through ffmpeg, kept in the cache folder. Returns 0 and the PNG's path, or -1 with the reason. */
static int webp_to_png(const uint8_t *data, size_t n, char *png, size_t pngsz, char *why, size_t whysz)
{
    const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    char dir[400];
    if (cache && *cache)
        snprintf(dir, sizeof(dir), "%s/mellagui/glb", cache);
    else if (home && *home)
        snprintf(dir, sizeof(dir), "%s/.cache/mellagui/glb", home);
    else {
        fail(why, whysz, "no cache folder for the WebP texture");
        return -1;
    }
    char tmpdir[420];
    snprintf(tmpdir, sizeof(tmpdir), "%s/", dir);
    mkdir_p(tmpdir);
    snprintf(png, pngsz, "%s/%016llx.png", dir, (unsigned long long)fnv(data, n));
    if (access(png, R_OK) == 0)
        return 0;
    const char *ff = ffpipe_ffmpeg_path();
    if (!ff) {
        fail(why, whysz, "the texture is WebP and there is no ffmpeg to turn it into a PNG");
        return -1;
    }
    char webp[460], tmp[460];
    snprintf(webp, sizeof(webp), "%s/%016llx.webp", dir, (unsigned long long)fnv(data, n));
    snprintf(tmp, sizeof(tmp), "%s/%016llx.tmp.png", dir, (unsigned long long)fnv(data, n));
    FILE *f = fopen(webp, "wb");
    if (!f || fwrite(data, 1, n, f) != n) {
        if (f)
            fclose(f);
        fail(why, whysz, "cannot write %s", webp);
        return -1;
    }
    fclose(f);
    pid_t pid = fork();
    if (pid < 0) {
        unlink(webp);
        fail(why, whysz, "fork failed");
        return -1;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
        }
        execl(ff, ff, "-loglevel", "error", "-y", "-i", webp, "-frames:v", "1", "-f", "image2", tmp, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    bool done = false;
    for (int i = 0; i < 1500 && !done; i++) {                           /* up to 30 s */
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            done = true;
        else
            usleep(20000);
    }
    if (!done) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    unlink(webp);
    if (!done || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(tmp);
        fail(why, whysz, "ffmpeg could not turn the WebP texture into a PNG");
        return -1;
    }
    if (rename(tmp, png) < 0) {
        unlink(tmp);
        fail(why, whysz, "cannot keep %s", png);
        return -1;
    }
    return 0;
}

static void load_texture(struct glb *g, const struct ctx *c)
{
    g->tex_note[0] = 0;
    const struct json *mats = json_get(c->root, "materials"), *mat = NULL;
    const struct json *meshes = json_get(c->root, "meshes");
    const struct json *prim = arr_at(json_get(arr_at(meshes, 0), "primitives"), 0);
    mat = arr_at(mats, (int)json_num(prim, "material", 0));
    const struct json *bct = json_get(json_get(mat, "pbrMetallicRoughness"), "baseColorTexture");
    if (!bct) {
        snprintf(g->tex_note, sizeof(g->tex_note), "the material has no base colour texture");
        return;
    }
    const struct json *tex = arr_at(json_get(c->root, "textures"), (int)json_num(bct, "index", -1));
    if (!tex) {
        snprintf(g->tex_note, sizeof(g->tex_note), "the texture is missing");
        return;
    }
    const struct json *webp = json_get(json_get(tex, "extensions"), "EXT_texture_webp");
    int src = webp ? (int)json_num(webp, "source", -1) : (int)json_num(tex, "source", -1);
    const struct json *im = arr_at(json_get(c->root, "images"), src);
    if (!im || !json_get(im, "bufferView")) {
        snprintf(g->tex_note, sizeof(g->tex_note), "the image is not inside the file");
        return;
    }
    const struct json *b = arr_at(json_get(c->root, "bufferViews"), (int)json_num(im, "bufferView", -1));
    double boff = json_num(b, "byteOffset", 0), blen = json_num(b, "byteLength", -1);
    if (!b || boff < 0 || blen <= 0 || boff + blen > (double)c->blen) {
        snprintf(g->tex_note, sizeof(g->tex_note), "the image reaches outside the file's data");
        return;
    }
    const uint8_t *data = c->bin + (size_t)boff;
    size_t n = (size_t)blen;
    struct lm_image img = { 0, 0, NULL };
    char why[160] = "";
    if (n > 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4)) {
        char png[500];
        if (webp_to_png(data, n, png, sizeof(png), why, sizeof(why)) < 0 || lm_img_load(png, &img, why, sizeof(why)) < 0) {
            snprintf(g->tex_note, sizeof(g->tex_note), "%s", why);
            return;
        }
        snprintf(g->tex_note, sizeof(g->tex_note), "WebP through ffmpeg");
    } else {
        if (lm_img_decode(data, n, &img, why, sizeof(why)) < 0) {
            snprintf(g->tex_note, sizeof(g->tex_note), "%s", why);
            return;
        }
        snprintf(g->tex_note, sizeof(g->tex_note), "PNG / JPEG");
    }
    g->tex = img.px;
    g->tex_w = img.w;
    g->tex_h = img.h;
}

/* ---- loading ----------------------------------------------------------------------------------------------------------------------- */

void glb_free(struct glb *g)
{
    if (!g)
        return;
    free(g->pos); free(g->nrm); free(g->uv); free(g->jidx); free(g->jwt); free(g->idx);
    free(g->node); free(g->order); free(g->joint); free(g->ibm);
    for (int i = 0; i < g->nc; i++) {
        free(g->chan[i].time);
        free(g->chan[i].val);
    }
    free(g->chan);
    free(g->tex);
    free(g);
}

/* Fills g->order (parents before children); false on a loop. */
static bool order_nodes(struct glb *g)
{
    g->order = malloc((size_t)g->nn * sizeof(int));
    unsigned char *seen = calloc((size_t)g->nn, 1);
    if (!g->order || !seen) {
        free(seen);
        return false;
    }
    int n = 0;
    for (int pass = 0; pass < g->nn && n < g->nn; pass++) {                 /* (a simple relaxation: at most nn passes) */
        bool any = false;
        for (int i = 0; i < g->nn; i++) {
            if (seen[i])
                continue;
            int p = g->node[i].parent;
            if (p < 0 || seen[p]) {
                seen[i] = 1;
                g->order[n++] = i;
                any = true;
            }
        }
        if (!any)
            break;
    }
    free(seen);
    return n == g->nn;
}

struct glb *glb_load(const char *path, char *err, size_t errsz)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fail(err, errsz, "cannot open %s: %s", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen < 20 || (unsigned long)flen > FILE_MAX) {
        fclose(f);
        fail(err, errsz, "%s is not a usable size for a glb file", path);
        return NULL;
    }
    uint8_t *file = malloc((size_t)flen);
    if (!file || fread(file, 1, (size_t)flen, f) != (size_t)flen) {
        free(file);
        fclose(f);
        fail(err, errsz, "cannot read %s", path);
        return NULL;
    }
    fclose(f);

    struct glb *g = calloc(1, sizeof(*g));
    struct ctx c = { file, (size_t)flen, NULL, 0, NULL };
    char *jtext = NULL;
    float *pos = NULL, *nrm = NULL, *uv = NULL, *jw = NULL, *jf = NULL, *ibm = NULL;
    if (!g)
        goto oom;
    g->head_joint = g->foot_joint = g->hips_joint = -1;
    uint32_t magic, ver, total;
    memcpy(&magic, file, 4); memcpy(&ver, file + 4, 4); memcpy(&total, file + 8, 4);
    if (magic != 0x46546c67u || ver != 2 || total > (uint32_t)flen) {
        fail(err, errsz, "not a glTF 2 binary file (.glb)");
        goto bad;
    }
    size_t off = 12;
    while (off + 8 <= (size_t)total) {
        uint32_t clen, ctype;
        memcpy(&clen, file + off, 4); memcpy(&ctype, file + off + 4, 4);
        if ((size_t)clen > (size_t)total - off - 8) {
            fail(err, errsz, "a chunk reaches past the end of the file");
            goto bad;
        }
        if (ctype == 0x4e4f534au && !jtext) {                               /* JSON */
            jtext = malloc((size_t)clen + 1);
            if (!jtext)
                goto oom;
            memcpy(jtext, file + off + 8, clen);
            jtext[clen] = 0;
        } else if (ctype == 0x004e4942u && !c.bin) {                        /* BIN */
            c.bin = file + off + 8;
            c.blen = clen;
        }
        off += 8 + (size_t)clen;
        off = (off + 3) & ~(size_t)3;
    }
    if (!jtext || !c.bin) {
        fail(err, errsz, "the file has no JSON or no binary chunk");
        goto bad;
    }
    c.root = json_parse(jtext);
    if (!c.root || c.root->type != JSON_OBJ) {
        fail(err, errsz, "the JSON part is damaged");
        goto bad;
    }
    const struct json *req = json_get(c.root, "extensionsRequired");
    for (const struct json *e = req ? req->child : NULL; e; e = e->next)
        if (e->type == JSON_STR && strcmp(e->str, "EXT_texture_webp") && strcmp(e->str, "KHR_materials_ior") && strcmp(e->str, "KHR_materials_specular")
            && strcmp(e->str, "KHR_texture_transform") && strcmp(e->str, "KHR_materials_emissive_strength")) {
            fail(err, errsz, "the file requires the extension %s, which is not supported", e->str);
            goto bad;
        }

    /* the nodes */
    const struct json *nodes = json_get(c.root, "nodes");
    g->nn = arr_len(nodes);
    if (g->nn < 1 || g->nn > NODES_MAX) {
        fail(err, errsz, "the file has %d nodes", g->nn);
        goto bad;
    }
    g->node = calloc((size_t)g->nn, sizeof(*g->node));
    if (!g->node)
        goto oom;
    for (int i = 0; i < g->nn; i++)
        g->node[i].parent = -1;
    int ni = 0;
    for (const struct json *n = nodes->child; n; n = n->next, ni++) {
        struct glb_node *nd = &g->node[ni];
        snprintf(nd->name, sizeof(nd->name), "%s", json_str(n, "name", ""));
        nd->r[3] = 1.0f;
        nd->s[0] = nd->s[1] = nd->s[2] = 1.0f;
        const struct json *m = json_get(n, "matrix"), *t = json_get(n, "translation"), *r = json_get(n, "rotation"), *s = json_get(n, "scale");
        if (m && arr_len(m) == 16) {
            float mm[16];
            int k = 0;
            for (const struct json *e = m->child; e; e = e->next)
                mm[k++] = (float)e->num;
            decompose(mm, nd->t, nd->r, nd->s);
        } else {
            if (t && arr_len(t) == 3) { int k = 0; for (const struct json *e = t->child; e; e = e->next) nd->t[k++] = (float)e->num; }
            if (r && arr_len(r) == 4) { int k = 0; for (const struct json *e = r->child; e; e = e->next) nd->r[k++] = (float)e->num; quat_normalize(nd->r); }
            if (s && arr_len(s) == 3) { int k = 0; for (const struct json *e = s->child; e; e = e->next) nd->s[k++] = (float)e->num; }
        }
    }
    ni = 0;
    for (const struct json *n = nodes->child; n; n = n->next, ni++)
        for (const struct json *ch = json_get(n, "children") ? json_get(n, "children")->child : NULL; ch; ch = ch->next) {
            int ci = (int)ch->num;
            if (ci < 0 || ci >= g->nn || ci == ni || g->node[ci].parent >= 0) {
                fail(err, errsz, "the node tree is damaged");
                goto bad;
            }
            g->node[ci].parent = ni;
        }
    if (!order_nodes(g)) {
        fail(err, errsz, "the node tree has a loop");
        goto bad;
    }
    for (int i = 0; i < g->nn; i++) {
        if (!strcmp(g->node[i].name, "head")) g->head_joint = i;
        if (!strcmp(g->node[i].name, "frontleg2")) g->foot_joint = i;
        if (!strcmp(g->node[i].name, "Hips")) g->hips_joint = i;
    }

    /* the mesh: the first primitive of the first mesh, and its skin */
    const struct json *mesh = arr_at(json_get(c.root, "meshes"), 0);
    const struct json *prim = arr_at(json_get(mesh, "primitives"), 0);
    const struct json *attr = json_get(prim, "attributes");
    if (!prim || !attr) {
        fail(err, errsz, "the file has no mesh");
        goto bad;
    }
    if ((int)json_num(prim, "mode", 4) != 4) {
        fail(err, errsz, "only triangle meshes are supported");
        goto bad;
    }
    int nv = 0, nuv = 0, nnr = 0, njw = 0, njj = 0, nibm = 0;
    pos = acc_floats(&c, (int)json_num(attr, "POSITION", -1), 3, &nv, err, errsz);
    if (!pos)
        goto bad;
    if (nv < 3 || nv > VERTS_MAX) {
        fail(err, errsz, "the mesh has %d vertices", nv);
        goto bad;
    }
    nrm = json_get(attr, "NORMAL") ? acc_floats(&c, (int)json_num(attr, "NORMAL", -1), 3, &nnr, err, errsz) : NULL;
    uv = json_get(attr, "TEXCOORD_0") ? acc_floats(&c, (int)json_num(attr, "TEXCOORD_0", -1), 2, &nuv, err, errsz) : NULL;
    if ((json_get(attr, "NORMAL") && !nrm) || (json_get(attr, "TEXCOORD_0") && !uv))
        goto bad;
    if ((nrm && nnr != nv) || (uv && nuv != nv)) {
        fail(err, errsz, "the vertex attributes disagree on the vertex count");
        goto bad;
    }
    g->nv = nv;
    g->pos = pos; pos = NULL;
    g->nrm = nrm ? nrm : calloc((size_t)nv * 3, sizeof(float)); nrm = NULL;
    g->uv = uv ? uv : calloc((size_t)nv * 2, sizeof(float)); uv = NULL;
    g->jidx = calloc((size_t)nv * 4, 1);
    g->jwt = calloc((size_t)nv * 4, sizeof(float));
    if (!g->nrm || !g->uv || !g->jidx || !g->jwt)
        goto oom;
    if (!nrm && !json_get(attr, "NORMAL"))
        for (int i = 0; i < nv; i++)
            g->nrm[i * 3 + 1] = 1.0f;

    /* the skin */
    int skin_i = -1;
    for (const struct json *n = nodes->child; n; n = n->next)
        if (json_get(n, "mesh") && json_get(n, "skin"))
            skin_i = (int)json_num(n, "skin", -1);
    const struct json *skin = skin_i >= 0 ? arr_at(json_get(c.root, "skins"), skin_i) : NULL;
    if (skin) {
        const struct json *js = json_get(skin, "joints");
        g->nj = arr_len(js);
        if (g->nj < 1 || g->nj > JOINTS_MAX) {
            fail(err, errsz, "the skin has %d joints", g->nj);
            goto bad;
        }
        g->joint = malloc((size_t)g->nj * sizeof(int));
        if (!g->joint)
            goto oom;
        int k = 0;
        for (const struct json *e = js->child; e; e = e->next, k++) {
            int nd = (int)e->num;
            if (nd < 0 || nd >= g->nn) {
                fail(err, errsz, "a joint is not a node");
                goto bad;
            }
            g->joint[k] = nd;
        }
        if (json_get(skin, "inverseBindMatrices")) {
            ibm = acc_floats(&c, (int)json_num(skin, "inverseBindMatrices", -1), 16, &nibm, err, errsz);
            if (!ibm)
                goto bad;
            if (nibm < g->nj) {
                fail(err, errsz, "there are fewer inverse bind matrices than joints");
                goto bad;
            }
        }
        g->ibm = calloc((size_t)g->nj * 16, sizeof(float));
        if (!g->ibm)
            goto oom;
        for (int j = 0; j < g->nj; j++) {
            if (ibm)
                memcpy(g->ibm + j * 16, ibm + (size_t)j * 16, 16 * sizeof(float));
            else
                mat_identity(g->ibm + j * 16);
        }
        free(ibm);
        ibm = NULL;
        jf = json_get(attr, "JOINTS_0") ? acc_floats(&c, (int)json_num(attr, "JOINTS_0", -1), 4, &njj, err, errsz) : NULL;
        jw = json_get(attr, "WEIGHTS_0") ? acc_floats(&c, (int)json_num(attr, "WEIGHTS_0", -1), 4, &njw, err, errsz) : NULL;
        if (!jf || !jw || njj != nv || njw != nv) {
            if (!err || !err[0])
                fail(err, errsz, "the skinned mesh has no joints and weights");
            goto bad;
        }
        for (int i = 0; i < nv; i++) {
            float sum = 0.0f;
            for (int k2 = 0; k2 < 4; k2++) {
                float w = jw[i * 4 + k2], jn = jf[i * 4 + k2];
                if (!(w > 0.0f) || jn < 0.0f || jn >= (float)g->nj)
                    w = 0.0f, jn = 0.0f;
                g->jidx[i * 4 + k2] = (uint8_t)jn;
                g->jwt[i * 4 + k2] = w;
                sum += w;
            }
            if (sum > 1e-6f)
                for (int k2 = 0; k2 < 4; k2++)
                    g->jwt[i * 4 + k2] /= sum;
            else
                g->jwt[i * 4] = 1.0f;
        }
        free(jf); free(jw);
        jf = jw = NULL;
    } else {
        for (int i = 0; i < nv; i++)
            g->jwt[i * 4] = 1.0f;
    }

    /* the triangles */
    if (json_get(prim, "indices")) {
        struct view v;
        if (!acc_view(&c, (int)json_num(prim, "indices", -1), &v, err, errsz))
            goto bad;
        if (v.ncomp != 1 || v.ctype == 5126 || v.ctype == 5120 || v.ctype == 5122 || v.count < 3 || v.count > VERTS_MAX * 6) {
            fail(err, errsz, "the index list is not usable");
            goto bad;
        }
        g->nt = v.count / 3;
        g->idx = malloc((size_t)g->nt * 3 * sizeof(uint32_t));
        if (!g->idx)
            goto oom;
        size_t cs = comp_size(v.ctype);
        for (int i = 0; i < g->nt * 3; i++) {
            uint32_t x = 0;
            if (cs == 1) x = v.base[(size_t)i * v.stride];
            else if (cs == 2) { uint16_t h; memcpy(&h, v.base + (size_t)i * v.stride, 2); x = h; }
            else memcpy(&x, v.base + (size_t)i * v.stride, 4);
            if (x >= (uint32_t)nv) {
                fail(err, errsz, "a triangle uses a vertex that does not exist");
                goto bad;
            }
            g->idx[i] = x;
        }
    } else {
        g->nt = nv / 3;
        g->idx = malloc((size_t)g->nt * 3 * sizeof(uint32_t));
        if (!g->idx)
            goto oom;
        for (int i = 0; i < g->nt * 3; i++)
            g->idx[i] = (uint32_t)i;
    }

    /* the first animation */
    const struct json *anim = arr_at(json_get(c.root, "animations"), 0);
    if (anim) {
        const struct json *chs = json_get(anim, "channels"), *sams = json_get(anim, "samplers");
        int nch = arr_len(chs);
        g->chan = calloc((size_t)(nch > 0 ? nch : 1), sizeof(*g->chan));
        if (!g->chan)
            goto oom;
        for (const struct json *ch = chs ? chs->child : NULL; ch; ch = ch->next) {
            const struct json *tg = json_get(ch, "target"), *sm = arr_at(sams, (int)json_num(ch, "sampler", -1));
            const char *pth = json_str(tg, "path", "");
            int nd = (int)json_num(tg, "node", -1), path = !strcmp(pth, "translation") ? 0 : !strcmp(pth, "rotation") ? 1 : !strcmp(pth, "scale") ? 2 : -1;
            if (!sm || nd < 0 || nd >= g->nn || path < 0)
                continue;                                                   /* (weights and the like are not played) */
            const char *interp = json_str(sm, "interpolation", "LINEAR");
            bool cubic = !strcmp(interp, "CUBICSPLINE");
            int nk = 0, nvv = 0, comp = path == 1 ? 4 : 3;
            float *tm = acc_floats(&c, (int)json_num(sm, "input", -1), 1, &nk, err, errsz);
            if (!tm)
                goto bad;
            float *vv = acc_floats(&c, (int)json_num(sm, "output", -1), comp, &nvv, err, errsz);
            if (!vv) {
                free(tm);
                goto bad;
            }
            if (nk < 1 || nvv < (cubic ? nk * 3 : nk)) {
                free(tm);
                free(vv);
                fail(err, errsz, "an animation channel has too few values");
                goto bad;
            }
            struct glb_chan *gc = &g->chan[g->nc++];
            gc->node = nd; gc->path = path; gc->n = nk; gc->step = !strcmp(interp, "STEP");
            gc->time = tm;
            gc->val = malloc((size_t)nk * (size_t)comp * sizeof(float));
            if (!gc->val) {
                free(vv);
                goto oom;
            }
            for (int k = 0; k < nk; k++)
                for (int q = 0; q < comp; q++)
                    gc->val[k * comp + q] = vv[(cubic ? k * 3 + 1 : k) * comp + q];
            free(vv);
            if (nk > 0 && tm[nk - 1] > g->anim_len)
                g->anim_len = tm[nk - 1];
        }
    }

    load_texture(g, &c);

    /* the rest pose's bounds, the cat's eyes, how far a stride goes */
    {
        float *jm = malloc((size_t)(g->nj > 0 ? g->nj : 1) * 16 * sizeof(float)), *sp = malloc((size_t)nv * 3 * sizeof(float)), *sn = malloc((size_t)nv * 3 * sizeof(float));
        if (!jm || !sp || !sn) {
            free(jm); free(sp); free(sn);
            goto oom;
        }
        glb_pose(g, 0.0f, 0.0f, NULL, jm);
        glb_skin(g, jm, 0, nv, sp, sn);
        for (int a = 0; a < 3; a++) {
            g->bmin[a] = 1e30f;
            g->bmax[a] = -1e30f;
        }
        for (int i = 0; i < nv; i++)
            for (int a = 0; a < 3; a++) {
                g->bmin[a] = fminf(g->bmin[a], sp[i * 3 + a]);
                g->bmax[a] = fmaxf(g->bmax[a], sp[i * 3 + a]);
            }
        free(jm); free(sp); free(sn);
    }
    if (g->nv == 82659 && g->head_joint >= 0) {                              /* the cat this was made for: its eyes, measured on the face (they are painted on the texture) */
        g->has_eyes = true;
        g->eye[0][0] = -0.000165f; g->eye[0][1] = 0.00235f; g->eye[0][2] = 0.00252f;
        g->eye[1][0] = 0.000165f;  g->eye[1][1] = 0.00235f; g->eye[1][2] = 0.00252f;
    }
    if (g->anim_len > 0.0f && g->foot_joint >= 0 && g->hips_joint >= 0 && g->nj > 0) {      /* the paw's travel relative to the hips over a cycle: the stance is ~60 % of it */
        float zmin = 1e30f, zmax = -1e30f, G[NODES_MAX][16];
        if (g->nn <= NODES_MAX) {
            for (int s = 0; s < 32; s++) {
                float tt = g->anim_len * (float)s / 32.0f;
                for (int oi = 0; oi < g->nn; oi++) {
                    int i = g->order[oi];
                    float t3[3] = { g->node[i].t[0], g->node[i].t[1], g->node[i].t[2] }, r4[4] = { g->node[i].r[0], g->node[i].r[1], g->node[i].r[2], g->node[i].r[3] }, s3[3] = { g->node[i].s[0], g->node[i].s[1], g->node[i].s[2] };
                    for (int ci = 0; ci < g->nc; ci++) {
                        const struct glb_chan *ch = &g->chan[ci];
                        if (ch->node != i || ch->path != 0 || ch->n < 1)
                            continue;
                        int k = 0;
                        while (k + 1 < ch->n && ch->time[k + 1] <= tt)
                            k++;
                        for (int q = 0; q < 3; q++)
                            t3[q] = ch->val[k * 3 + q];
                    }
                    for (int ci = 0; ci < g->nc; ci++) {
                        const struct glb_chan *ch = &g->chan[ci];
                        if (ch->node != i || ch->path != 1 || ch->n < 1)
                            continue;
                        int k = 0;
                        while (k + 1 < ch->n && ch->time[k + 1] <= tt)
                            k++;
                        for (int q = 0; q < 4; q++)
                            r4[q] = ch->val[k * 4 + q];
                    }
                    float lm[16];
                    trs_matrix(t3, r4, s3, lm);
                    if (g->node[i].parent >= 0)
                        mat_mul(G[g->node[i].parent], lm, G[i]);
                    else
                        memcpy(G[i], lm, sizeof(lm));
                }
                float z = G[g->foot_joint][14] - G[g->hips_joint][14];
                zmin = fminf(zmin, z);
                zmax = fmaxf(zmax, z);
            }
            g->stride = (zmax - zmin) / 0.6f;
        }
    }

    json_free(c.root);
    free(jtext);
    free(file);
    return g;
oom:
    fail(err, errsz, "out of memory");
bad:
    free(pos); free(nrm); free(uv); free(jw); free(jf); free(ibm);
    if (c.root)
        json_free(c.root);
    free(jtext);
    free(file);
    glb_free(g);
    return NULL;
}

/* ---- posing and skinning ---------------------------------------------------------------------------------------------------------- */

static float sample_key(const struct glb_chan *ch, float t, int comp, float out[4])
{
    int k = 0;
    while (k + 1 < ch->n && ch->time[k + 1] <= t)
        k++;
    if (t <= ch->time[0] || k + 1 >= ch->n || ch->step) {
        for (int q = 0; q < comp; q++)
            out[q] = ch->val[(t >= ch->time[ch->n - 1] ? ch->n - 1 : k) * comp + q];
        return 0.0f;
    }
    float span = ch->time[k + 1] - ch->time[k], f = span > 1e-9f ? (t - ch->time[k]) / span : 0.0f;
    if (comp == 4) {                                                        /* nlerp along the shorter way */
        float dot = 0.0f;
        for (int q = 0; q < 4; q++)
            dot += ch->val[k * 4 + q] * ch->val[(k + 1) * 4 + q];
        float sgn = dot < 0.0f ? -1.0f : 1.0f;
        for (int q = 0; q < 4; q++)
            out[q] = ch->val[k * 4 + q] * (1.0f - f) + sgn * ch->val[(k + 1) * 4 + q] * f;
        quat_normalize(out);
    } else {
        for (int q = 0; q < comp; q++)
            out[q] = ch->val[k * comp + q] * (1.0f - f) + ch->val[(k + 1) * comp + q] * f;
    }
    return f;
}

void glb_pose(const struct glb *g, float anim_time, float amount, const float *head_rot, float *jm)
{
    float (*T)[3] = malloc((size_t)g->nn * sizeof(*T)), (*R)[4] = malloc((size_t)g->nn * sizeof(*R)), (*S)[3] = malloc((size_t)g->nn * sizeof(*S));
    float (*G)[16] = malloc((size_t)g->nn * sizeof(*G));
    if (!T || !R || !S || !G) {
        free(T); free(R); free(S); free(G);
        for (int j = 0; j < g->nj; j++)
            mat_identity(jm + j * 16);
        return;
    }
    for (int i = 0; i < g->nn; i++) {
        memcpy(T[i], g->node[i].t, sizeof(T[i]));
        memcpy(R[i], g->node[i].r, sizeof(R[i]));
        memcpy(S[i], g->node[i].s, sizeof(S[i]));
    }
    amount = amount < 0.0f ? 0.0f : amount > 1.0f ? 1.0f : amount;
    if (amount > 0.0f && g->anim_len > 0.0f) {
        float tt = fmodf(anim_time, g->anim_len);
        if (tt < 0.0f)
            tt += g->anim_len;
        for (int ci = 0; ci < g->nc; ci++) {
            const struct glb_chan *ch = &g->chan[ci];
            float v[4];
            int comp = ch->path == 1 ? 4 : 3, i = ch->node;
            sample_key(ch, tt, comp, v);
            if (ch->path == 0)
                for (int q = 0; q < 3; q++) T[i][q] = T[i][q] + (v[q] - T[i][q]) * amount;
            else if (ch->path == 2)
                for (int q = 0; q < 3; q++) S[i][q] = S[i][q] + (v[q] - S[i][q]) * amount;
            else {
                float dot = 0.0f, o[4];
                for (int q = 0; q < 4; q++)
                    dot += R[i][q] * v[q];
                float sgn = dot < 0.0f ? -1.0f : 1.0f;
                for (int q = 0; q < 4; q++)
                    o[q] = R[i][q] * (1.0f - amount) + sgn * v[q] * amount;
                quat_normalize(o);
                memcpy(R[i], o, sizeof(o));
            }
        }
    }
    for (int oi = 0; oi < g->nn; oi++) {
        int i = g->order[oi];
        float lm[16];
        trs_matrix(T[i], R[i], S[i], lm);
        if (g->node[i].parent >= 0)
            mat_mul(G[g->node[i].parent], lm, G[i]);
        else
            memcpy(G[i], lm, sizeof(lm));
    }
    if (head_rot && g->head_joint >= 0) {                                   /* turn the head about its own place; what hangs on it follows */
        int h = g->head_joint;
        float p[3] = { G[h][12], G[h][13], G[h][14] }, A[16];
        mat_identity(A);
        for (int c = 0; c < 3; c++)
            for (int r = 0; r < 3; r++)
                A[c * 4 + r] = head_rot[r * 3 + c];
        for (int r = 0; r < 3; r++)
            A[12 + r] = p[r] - (head_rot[r * 3] * p[0] + head_rot[r * 3 + 1] * p[1] + head_rot[r * 3 + 2] * p[2]);
        float orig[16];
        memcpy(orig, G[h], sizeof(orig));
        mat_mul(A, orig, G[h]);
        bool *below = calloc((size_t)g->nn, sizeof(bool));
        if (below) {
            below[h] = true;
            for (int oi = 0; oi < g->nn; oi++) {
                int i = g->order[oi];
                if (i == h)
                    continue;
                int p2 = g->node[i].parent;
                if (p2 >= 0 && below[p2]) {
                    float lm[16];
                    trs_matrix(T[i], R[i], S[i], lm);
                    mat_mul(G[p2], lm, G[i]);
                    below[i] = true;
                }
            }
            free(below);
        }
    }
    for (int j = 0; j < g->nj; j++)
        mat_mul(G[g->joint[j]], g->ibm + j * 16, jm + j * 16);
    free(T); free(R); free(S); free(G);
}

void glb_skin(const struct glb *g, const float *jm, int v0, int v1, float *outpos, float *outnrm)
{
    for (int i = v0; i < v1; i++) {
        const float *p = g->pos + (size_t)i * 3, *n = g->nrm + (size_t)i * 3;
        float ax = 0, ay = 0, az = 0, nx = 0, ny = 0, nz = 0;
        if (g->nj == 0) {
            ax = p[0]; ay = p[1]; az = p[2]; nx = n[0]; ny = n[1]; nz = n[2];
        } else {
            for (int k = 0; k < 4; k++) {
                float w = g->jwt[(size_t)i * 4 + (size_t)k];
                if (w <= 0.0f)
                    continue;
                const float *m = jm + (size_t)g->jidx[(size_t)i * 4 + (size_t)k] * 16;
                ax += w * (m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]);
                ay += w * (m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]);
                az += w * (m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]);
                nx += w * (m[0] * n[0] + m[4] * n[1] + m[8] * n[2]);
                ny += w * (m[1] * n[0] + m[5] * n[1] + m[9] * n[2]);
                nz += w * (m[2] * n[0] + m[6] * n[1] + m[10] * n[2]);
            }
        }
        outpos[(size_t)i * 3] = ax; outpos[(size_t)i * 3 + 1] = ay; outpos[(size_t)i * 3 + 2] = az;
        outnrm[(size_t)i * 3] = nx; outnrm[(size_t)i * 3 + 1] = ny; outnrm[(size_t)i * 3 + 2] = nz;
    }
}

/* ---- describing -------------------------------------------------------------------------------------------------------------------- */

void glb_describe(const struct glb *g, FILE *out)
{
    fprintf(out, "mesh: %d vertices, %d triangles\n", g->nv, g->nt);
    fprintf(out, "rest pose bounds: x %.5f .. %.5f, y %.5f .. %.5f, z %.5f .. %.5f (size %.5f x %.5f x %.5f)\n", g->bmin[0], g->bmax[0], g->bmin[1], g->bmax[1],
            g->bmin[2], g->bmax[2], g->bmax[0] - g->bmin[0], g->bmax[1] - g->bmin[1], g->bmax[2] - g->bmin[2]);
    fprintf(out, "nodes: %d, joints: %d\n", g->nn, g->nj);
    for (int j = 0; j < g->nj; j++)
        fprintf(out, "  joint %d: %s (parent %s)\n", j, g->node[g->joint[j]].name,
                g->node[g->joint[j]].parent >= 0 ? g->node[g->node[g->joint[j]].parent].name : "-");
    fprintf(out, "animation: %d channels, %.3f s\n", g->nc, g->anim_len);
    fprintf(out, "texture: %s", g->tex ? "" : "none");
    if (g->tex)
        fprintf(out, "%d x %d", g->tex_w, g->tex_h);
    fprintf(out, " (%s)\n", g->tex_note);
    fprintf(out, "head joint: %s, eyes: %s, stride: %.5f\n", g->head_joint >= 0 ? "yes" : "no", g->has_eyes ? "known" : "not known", g->stride);
}
