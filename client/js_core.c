/* JavaScript interpreter core: memory, strings, objects, properties, conversions (see js_int.h). */
#include <ctype.h>
#include <math.h>

#include "js_int.h"

#define CHUNK_SIZE (1u << 20)

/* ---- memory ------------------------------------------------------------------------------------------------ */

static void mem_account(js *J, size_t n)
{
    J->mem += n;
    if (J->mem_cap && J->mem > J->mem_cap && !J->aborted) {
        J->aborted = true;
        J->thrown = true;
        J->exc = JS_UNDEFINED;
    }
}

void *js_alloc(js *J, size_t n)
{
    n = (n + 15) & ~(size_t)15;
    mem_account(J, n);
    if (n > CHUNK_SIZE / 4) {
        struct arena_chunk *c = calloc(1, sizeof(*c) + n + 16);
        if (!c) {
            fprintf(stderr, "lumen-web: out of memory\n");
            exit(1);
        }
        /* a big block gets its own chunk, linked behind the head so the bump chunk stays current */
        if (J->chunks) {
            c->next = J->chunks->next;
            J->chunks->next = c;
        } else {
            c->next = NULL;
            J->chunks = c;
        }
        return (char *)(((uintptr_t)(c + 1) + 15) & ~(uintptr_t)15);
    }
    if (J->bump + n > J->bump_end) {
        struct arena_chunk *c = calloc(1, sizeof(*c) + CHUNK_SIZE + 16);
        if (!c) {
            fprintf(stderr, "lumen-web: out of memory\n");
            exit(1);
        }
        c->next = J->chunks;
        J->chunks = c;
        J->bump = (char *)(((uintptr_t)(c + 1) + 15) & ~(uintptr_t)15);
        J->bump_end = (char *)(c + 1) + CHUNK_SIZE;
    }
    void *p = J->bump;
    J->bump += n;
    return p;
}

void *js_realloc_buf(js *J, void *p, size_t oldn, size_t newn)
{
    if (newn > oldn)
        mem_account(J, newn - oldn);
    void *q = realloc(p, newn);
    if (!q && newn) {
        fprintf(stderr, "lumen-web: out of memory\n");
        exit(1);
    }
    return q;
}

/* ---- strings ------------------------------------------------------------------------------------------------ */

static uint32_t hash_bytes(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h ? h : 1;
}

jsstr *js_str_new(js *J, const char *s, size_t len)
{
    jsstr *r = js_alloc(J, sizeof(jsstr) + len + 1);
    r->len = (uint32_t)len;
    if (len)
        memcpy(r->s, s, len);
    r->s[len] = 0;
    return r;
}

static void itab_grow(js *J)
{
    size_t nc = J->icap ? J->icap * 2 : 1024;
    jsstr **nt = calloc(nc, sizeof(*nt));
    if (!nt)
        exit(1);
    for (size_t i = 0; i < J->icap; i++) {
        jsstr *s = J->itab[i];
        if (!s)
            continue;
        size_t h = s->hash & (nc - 1);
        while (nt[h])
            h = (h + 1) & (nc - 1);
        nt[h] = s;
    }
    free(J->itab);
    J->itab = nt;
    J->icap = nc;
}

static jsstr *intern_bytes(js *J, const char *s, size_t len, jsstr *existing)
{
    if ((J->icount + 1) * 2 > J->icap)
        itab_grow(J);
    uint32_t hv = hash_bytes(s, len);
    size_t h = hv & (J->icap - 1);
    while (J->itab[h]) {
        jsstr *c = J->itab[h];
        if (c->hash == hv && c->len == len && !memcmp(c->s, s, len))
            return c;
        h = (h + 1) & (J->icap - 1);
    }
    jsstr *r = existing ? existing : js_str_new(J, s, len);
    r->hash = hv;
    r->interned = 1;
    J->itab[h] = r;
    J->icount++;
    return r;
}

jsstr *js_intern(js *J, const char *s, size_t len)
{
    return intern_bytes(J, s, len, NULL);
}

jsstr *js_intern_str(js *J, jsstr *s)
{
    if (s->interned)
        return s;
    return intern_bytes(J, s->s, s->len, s);
}

jsval js_cstr(js *J, const char *z)
{
    return js_strv(js_str_new(J, z, strlen(z)));
}

jsval js_strn(js *J, const char *s, size_t len)
{
    return js_strv(js_str_new(J, s, len));
}

jsstr *str_concat(js *J, jsstr *a, jsstr *b)
{
    if (!a->len)
        return b;
    if (!b->len)
        return a;
    jsstr *r = js_alloc(J, sizeof(jsstr) + (size_t)a->len + b->len + 1);
    r->len = a->len + b->len;
    memcpy(r->s, a->s, a->len);
    memcpy(r->s + a->len, b->s, b->len);
    r->s[r->len] = 0;
    return r;
}

/* ---- UTF-16 view ------------------------------------------------------------------------------------------------ */

static void str_scan(jsstr *s)
{
    if (s->kind)
        return;
    bool ascii = true;
    uint32_t units = 0;
    for (uint32_t i = 0; i < s->len;) {
        uint8_t c = (uint8_t)s->s[i];
        if (c < 0x80) { i++; units++; continue; }
        ascii = false;
        uint32_t l = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        if (i + l > s->len) l = s->len - i;
        units += l == 4 ? 2 : 1;
        i += l;
    }
    s->kind = ascii ? 1 : 2;
    s->len16 = units;
}

uint32_t str_len16(jsstr *s)
{
    str_scan(s);
    return s->kind == 1 ? s->len : s->len16;
}

static uint32_t seq_len(const jsstr *s, uint32_t i)
{
    uint8_t c = (uint8_t)s->s[i];
    uint32_t l = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
    return i + l > s->len ? s->len - i : l;
}

uint32_t str_off(jsstr *s, uint32_t unit)
{
    str_scan(s);
    if (s->kind == 1)
        return unit < s->len ? unit : s->len;
    uint32_t u = 0, b = 0;
    if (s->c16 <= unit && s->cbyte <= s->len) { u = s->c16; b = s->cbyte; }
    while (b < s->len && u < unit) {
        uint32_t l = seq_len(s, b);
        uint32_t w = l == 4 ? 2 : 1;
        if (u + w > unit)                                      /* inside a surrogate pair: the character's start */
            break;
        u += w;
        b += l;
    }
    s->c16 = u;
    s->cbyte = b;
    return b;
}

uint32_t str_unit(jsstr *s, uint32_t byte)
{
    str_scan(s);
    if (s->kind == 1)
        return byte < s->len ? byte : s->len;
    uint32_t u = 0, b = 0;
    if (s->cbyte <= byte && s->cbyte <= s->len) { u = s->c16; b = s->cbyte; }
    while (b < s->len && b < byte) {
        uint32_t l = seq_len(s, b);
        u += l == 4 ? 2 : 1;
        b += l;
    }
    s->c16 = u;
    s->cbyte = b;
    return u;
}

static uint32_t cp_at(const jsstr *s, uint32_t b, uint32_t *l)
{
    uint8_t c = (uint8_t)s->s[b];
    *l = seq_len(s, b);
    if (*l == 1) return c;
    uint32_t cp = c & (0xffu >> (*l + 1));
    for (uint32_t k = 1; k < *l; k++)
        cp = (cp << 6) | ((uint8_t)s->s[b + k] & 63);
    return cp;
}

int32_t str_code_unit(jsstr *s, uint32_t unit)
{
    str_scan(s);
    if (s->kind == 1)
        return unit < s->len ? (uint8_t)s->s[unit] : -1;
    if (unit >= s->len16)
        return -1;
    uint32_t b = str_off(s, unit), l;
    uint32_t cp = cp_at(s, b, &l);
    if (cp >= 0x10000) {
        uint32_t start_unit = str_unit(s, b);                   /* (resets the cache position: fine) */
        cp -= 0x10000;
        return start_unit == unit ? (int32_t)(0xd800 + (cp >> 10)) : (int32_t)(0xdc00 + (cp & 0x3ff));
    }
    return (int32_t)cp;
}

jsval str_char_at(js *J, jsstr *s, uint32_t unit)
{
    str_scan(s);
    if (s->kind == 1)
        return unit < s->len ? js_strn(J, s->s + unit, 1) : JS_UNDEFINED;
    if (unit >= s->len16)
        return JS_UNDEFINED;
    uint32_t b = str_off(s, unit), l;
    cp_at(s, b, &l);
    if (l == 4 && str_unit(s, b) != unit)
        return js_strn(J, "", 0);                               /* low half of a pair: the character was returned for the high half */
    return js_strn(J, s->s + b, l);
}

/* ---- objects ------------------------------------------------------------------------------------------------- */

jsobj *obj_new(js *J, uint8_t cls, jsobj *proto)
{
    jsobj *o = js_alloc(J, sizeof(*o));
    o->cls = cls;
    o->proto = proto;
    o->all_next = J->all_objs;
    J->all_objs = o;
    return o;
}

jsobj *js_object_new(js *J) { return obj_new(J, JC_OBJECT, J->object_proto); }
jsobj *js_array_new(js *J) { return obj_new(J, JC_ARRAY, J->array_proto); }
jsobj *js_proto_of(jsobj *o) { return o->proto; }
void js_set_proto(jsobj *o, jsobj *proto) { o->proto = proto; }
jsobj *js_object_proto(js *J) { return J->object_proto; }
jsobj *js_function_proto(js *J) { return J->function_proto; }
jsobj *js_global(js *J) { return J->global; }

static void rebuild_index(jsobj *o)
{
    uint32_t cap = 16;
    while (cap < o->nprops * 2 + 2)
        cap *= 2;
    int32_t *h = realloc(o->hidx, cap * sizeof(int32_t));
    if (!h)
        exit(1);
    memset(h, 0xff, cap * sizeof(int32_t));
    for (uint32_t i = 0; i < o->nprops; i++) {
        if (!o->props[i].key)
            continue;
        uint32_t k = o->props[i].key->hash & (cap - 1);
        while (h[k] >= 0)
            k = (k + 1) & (cap - 1);
        h[k] = (int32_t)i;
    }
    o->hidx = h;
    o->hcap = cap;
}

struct prop *obj_find_own(jsobj *o, jsstr *key)
{
    if (o->nprops > 8) {
        if (!o->hidx)
            rebuild_index(o);                           /* (built lazily; deletions drop it again) */
        uint32_t k = key->hash & (o->hcap - 1);
        while (o->hidx[k] >= 0) {
            struct prop *p = &o->props[o->hidx[k]];
            if (p->key == key)
                return p;
            k = (k + 1) & (o->hcap - 1);
        }
        return NULL;
    }
    for (uint32_t i = 0; i < o->nprops; i++)
        if (o->props[i].key == key)
            return &o->props[i];
    return NULL;
}

static struct prop *find_own_js(js *J, jsobj *o, jsstr *key)
{
    (void)J;
    return obj_find_own(o, key);
}

struct prop *obj_add_prop(js *J, jsobj *o, jsstr *key, jsval v, uint8_t flags)
{
    if (o->nprops == o->pcap) {
        uint32_t nc = o->pcap ? o->pcap * 2 : 4;
        o->props = js_realloc_buf(J, o->props, o->pcap * sizeof(struct prop), nc * sizeof(struct prop));
        o->pcap = nc;
    }
    struct prop *p = &o->props[o->nprops];
    memset(p, 0, sizeof(*p));
    p->key = key;
    p->v = v;
    p->flags = flags;
    o->nprops++;
    if (o->hidx) {
        if (o->nprops * 2 + 2 > o->hcap) {
            rebuild_index(o);
        } else {
            uint32_t k = key->hash & (o->hcap - 1);
            while (o->hidx[k] >= 0)
                k = (k + 1) & (o->hcap - 1);
            o->hidx[k] = (int32_t)(o->nprops - 1);
        }
    }
    return p;
}

bool obj_delete(jsobj *o, jsstr *key)
{
    for (uint32_t i = 0; i < o->nprops; i++)
        if (o->props[i].key == key) {
            o->props[i].key = NULL;
            o->props[i].v = JS_UNDEFINED;
            free(o->hidx);
            o->hidx = NULL;
            o->hcap = 0;
            return true;
        }
    return false;
}

bool is_index_key(jsstr *k, uint32_t *idx)
{
    if (!k->len || k->len > 10)
        return false;
    if (k->s[0] == '0' && k->len > 1)
        return false;
    uint64_t v = 0;
    for (uint32_t i = 0; i < k->len; i++) {
        if (k->s[i] < '0' || k->s[i] > '9')
            return false;
        v = v * 10 + (uint64_t)(k->s[i] - '0');
    }
    if (v >= 0xffffffffu)
        return false;
    *idx = (uint32_t)v;
    return true;
}

/* ---- arrays ------------------------------------------------------------------------------------------------- */

static void array_reserve(js *J, jsobj *a, uint32_t n)
{
    if (n <= a->icap)
        return;
    uint32_t nc = a->icap ? a->icap : 4;
    while (nc < n)
        nc *= 2;
    a->items = js_realloc_buf(J, a->items, a->icap * sizeof(jsval), nc * sizeof(jsval));
    for (uint32_t i = a->icap; i < nc; i++)
        a->items[i] = JS_UNDEFINED;
    a->icap = nc;
}

void array_set_len(js *J, jsobj *a, uint32_t n)
{
    if (n > 50u * 1000 * 1000) {
        js_throw_error(J, "RangeError", "Invalid array length");
        return;
    }
    if (n > a->len) {
        array_reserve(J, a, n);
        for (uint32_t i = a->len; i < n; i++)
            a->items[i] = JS_UNDEFINED;
    }
    a->len = n;
}

void array_put(js *J, jsobj *a, uint32_t i, jsval v)
{
    if (i >= a->len) {
        if (i > a->len + 10u * 1000 * 1000) {
            js_throw_error(J, "RangeError", "array index too large");
            return;
        }
        array_set_len(J, a, i + 1);
        if (J->thrown)
            return;
    }
    a->items[i] = v;
}

void js_array_push(js *J, jsobj *arr, jsval v)
{
    array_put(J, arr, arr->len, v);
}

uint32_t js_array_len(const jsobj *arr) { return arr->len; }
jsval js_array_get(const jsobj *arr, uint32_t i) { return i < arr->len ? arr->items[i] : JS_UNDEFINED; }
bool js_is_array(jsval v) { return v.t == JS_OBJ && v.u.o->cls == JC_ARRAY; }
bool js_is_function(jsval v) { return v.t == JS_OBJ && v.u.o->cls == JC_FUNCTION; }

/* ---- number formatting / parsing --------------------------------------------------------------------------- */

jsstr *num_to_str(js *J, double d)
{
    char buf[64];
    if (isnan(d))
        return js_intern(J, "NaN", 3);
    if (isinf(d))
        return d > 0 ? js_intern(J, "Infinity", 8) : js_intern(J, "-Infinity", 9);
    if (d == 0)
        return J->k_digits[0];
    if (d >= 0 && d < 64 && d == floor(d))
        return J->k_digits[(int)d];
    if (fabs(d) < 1e15 && d == floor(d)) {
        int n = snprintf(buf, sizeof(buf), "%lld", (long long)d);
        return js_str_new(J, buf, (size_t)n);
    }
    char e[48];
    int prec;
    for (prec = 1; prec <= 17; prec++) {
        snprintf(e, sizeof(e), "%.*e", prec - 1, d);
        if (strtod(e, NULL) == d)
            break;
    }
    if (prec > 17)
        prec = 17;
    snprintf(e, sizeof(e), "%.*e", prec - 1, fabs(d));
    char digits[32];
    int k = 0;
    const char *p = e;
    while (*p && *p != 'e') {
        if (isdigit((unsigned char)*p))
            digits[k++] = *p;
        p++;
    }
    int ex = atoi(p + 1);
    while (k > 1 && digits[k - 1] == '0')
        k--;
    digits[k] = 0;
    int n = ex + 1;
    char out[96];
    int o = 0;
    if (d < 0)
        out[o++] = '-';
    if (k <= n && n <= 21) {
        memcpy(out + o, digits, (size_t)k);
        o += k;
        for (int i = 0; i < n - k; i++)
            out[o++] = '0';
    } else if (0 < n && n <= 21) {
        memcpy(out + o, digits, (size_t)n);
        o += n;
        out[o++] = '.';
        memcpy(out + o, digits + n, (size_t)(k - n));
        o += k - n;
    } else if (-6 < n && n <= 0) {
        out[o++] = '0';
        out[o++] = '.';
        for (int i = 0; i < -n; i++)
            out[o++] = '0';
        memcpy(out + o, digits, (size_t)k);
        o += k;
    } else {
        out[o++] = digits[0];
        if (k > 1) {
            out[o++] = '.';
            memcpy(out + o, digits + 1, (size_t)(k - 1));
            o += k - 1;
        }
        o += snprintf(out + o, sizeof(out) - (size_t)o, "e%c%d", n - 1 < 0 ? '-' : '+', abs(n - 1));
    }
    return js_str_new(J, out, (size_t)o);
}

double str_to_number(const char *s, size_t len)
{
    while (len && isspace((unsigned char)*s)) { s++; len--; }
    while (len && isspace((unsigned char)s[len - 1])) len--;
    if (!len)
        return 0;
    char buf[128];
    if (len >= sizeof(buf))
        return NAN;
    memcpy(buf, s, len);
    buf[len] = 0;
    const char *p = buf;
    double sign = 1;
    if (!strcmp(buf, "Infinity") || !strcmp(buf, "+Infinity"))
        return INFINITY;
    if (!strcmp(buf, "-Infinity"))
        return -INFINITY;
    if (buf[0] == '0' && len > 2 && strchr("xXoObB", buf[1])) {
        int base = (buf[1] == 'x' || buf[1] == 'X') ? 16 : (buf[1] == 'o' || buf[1] == 'O') ? 8 : 2;
        double v = 0;
        for (p = buf + 2; *p; p++) {
            int dg = isdigit((unsigned char)*p) ? *p - '0' : isalpha((unsigned char)*p) ? tolower(*p) - 'a' + 10 : 99;
            if (dg >= base)
                return NAN;
            v = v * base + dg;
        }
        return v;
    }
    if (*p == '-' || *p == '+') {
        sign = *p == '-' ? -1 : 1;
        p++;
    }
    for (const char *q = p; *q; q++)
        if (!(isdigit((unsigned char)*q) || *q == '.' || *q == 'e' || *q == 'E' || *q == '+' || *q == '-'))
            return NAN;
    char *e;
    double v = strtod(p, &e);
    if (e == p || *e)
        return NAN;
    return sign * v;
}

int32_t to_int32(double d)
{
    if (isnan(d) || isinf(d))
        return 0;
    double m = fmod(trunc(d), 4294967296.0);
    if (m < 0)
        m += 4294967296.0;
    return (int32_t)(uint32_t)m;
}

uint32_t to_uint32(double d) { return (uint32_t)to_int32(d); }

/* ---- errors --------------------------------------------------------------------------------------------------- */

static const char *const ERR_NAMES[] = { "Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", "EvalError",
                                          "URIError", NULL };

jsval make_error(js *J, int type, const char *msg)
{
    jsobj *e = obj_new(J, JC_ERROR, J->err_protos[type]);
    obj_add_prop(J, e, J->k_message, js_cstr(J, msg), PF_WRITE);
    char st[300];
    snprintf(st, sizeof(st), "%s: %s\n    at <anonymous>", ERR_NAMES[type], msg);
    obj_add_prop(J, e, J->k_stack, js_cstr(J, st), PF_WRITE);
    return js_objv(e);
}

jsval js_throw_value(js *J, jsval v)
{
    if (!J->aborted) {
        J->thrown = true;
        J->exc = v;
    }
    return JS_UNDEFINED;
}

jsval js_throw_error(js *J, const char *type, const char *fmt, ...)
{
    char m[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    int t = 0;
    for (int i = 0; ERR_NAMES[i]; i++)
        if (!strcmp(ERR_NAMES[i], type))
            t = i;
    return js_throw_value(J, make_error(J, t, m));
}

jsval throw_type(js *J, const char *fmt, ...)
{
    char m[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    return js_throw_value(J, make_error(J, ERR_TYPE, m));
}

bool js_thrown(js *J) { return J->thrown; }

jsval js_exception(js *J)
{
    jsval e = J->exc;
    J->thrown = false;
    J->exc = JS_UNDEFINED;
    return e;
}

void js_describe_exception(js *J, jsval ex, char *out, size_t n)
{
    if (J->aborted) {
        snprintf(out, n, "script stopped: %s", J->mem_cap && J->mem > J->mem_cap ? "out of memory" : "ran too long");
        return;
    }
    if (ex.t == JS_OBJ && ex.u.o->cls == JC_ERROR) {
        jsval nm = get_prop(J, ex, J->k_name), msg = get_prop(J, ex, J->k_message);
        bool saved = J->thrown;
        J->thrown = false;
        jsstr *a = nm.t == JS_STR ? nm.u.s : NULL, *b = msg.t == JS_STR ? msg.u.s : NULL;
        snprintf(out, n, "%s: %s", a ? a->s : "Error", b ? b->s : "");
        J->thrown = saved;
        return;
    }
    bool saved = J->thrown;
    jsval se = J->exc;
    J->thrown = false;
    jsstr *s = to_jsstr(J, ex);
    snprintf(out, n, "Uncaught %s", s ? s->s : "(exception)");
    J->thrown = saved;
    J->exc = se;
}

/* ---- conversions ---------------------------------------------------------------------------------------------- */

const char *type_of(jsval v)
{
    switch (v.t) {
    case JS_UNDEF: return "undefined";
    case JS_NULL: return "object";
    case JS_BOOL: return "boolean";
    case JS_NUM: return "number";
    case JS_STR: return "string";
    default: return v.u.o->cls == JC_FUNCTION ? "function" : "object";
    }
}

bool js_truthy(jsval v)
{
    switch (v.t) {
    case JS_UNDEF: case JS_NULL: return false;
    case JS_BOOL: return v.u.b;
    case JS_NUM: return v.u.n != 0 && !isnan(v.u.n);
    case JS_STR: return v.u.s->len > 0;
    default: return true;
    }
}

jsval to_primitive(js *J, jsval v, int hint)
{
    if (v.t != JS_OBJ)
        return v;
    jsobj *o = v.u.o;
    if (hint == 0 && o->cls == JC_DATE)
        hint = 2;
    jsstr *order[2];
    order[0] = hint == 2 ? J->k_toString : J->k_valueOf;
    order[1] = hint == 2 ? J->k_valueOf : J->k_toString;
    for (int i = 0; i < 2; i++) {
        jsval f = get_prop(J, v, order[i]);
        THROW_IF(J);
        if (js_is_function(f)) {
            jsval r = call_function(J, f.u.o, v, 0, NULL, JS_UNDEFINED);
            THROW_IF(J);
            if (r.t != JS_OBJ)
                return r;
        }
    }
    return throw_type(J, "Cannot convert object to primitive value");
}

jsstr *to_jsstr(js *J, jsval v)
{
    switch (v.t) {
    case JS_UNDEF: return J->k_undefined;
    case JS_NULL: return js_intern(J, "null", 4);
    case JS_BOOL: return v.u.b ? js_intern(J, "true", 4) : js_intern(J, "false", 5);
    case JS_NUM: return num_to_str(J, v.u.n);
    case JS_STR: return v.u.s;
    default: {
        jsval p = to_primitive(J, v, 2);
        if (J->thrown)
            return NULL;
        return to_jsstr(J, p);
    }
    }
}

double to_number(js *J, jsval v)
{
    switch (v.t) {
    case JS_UNDEF: return NAN;
    case JS_NULL: return 0;
    case JS_BOOL: return v.u.b ? 1 : 0;
    case JS_NUM: return v.u.n;
    case JS_STR: return str_to_number(v.u.s->s, v.u.s->len);
    default: {
        jsval p = to_primitive(J, v, 1);
        if (J->thrown)
            return NAN;
        return to_number(J, p);
    }
    }
}

const char *js_tostr(js *J, jsval v, size_t *len)
{
    jsstr *s = to_jsstr(J, v);
    if (!s)
        return NULL;
    if (len)
        *len = s->len;
    return s->s;
}

double js_tonum(js *J, jsval v) { return to_number(J, v); }

jsstr *key_of(js *J, jsval v)
{
    if (v.t == JS_STR)
        return js_intern_str(J, v.u.s);
    jsstr *s = to_jsstr(J, v);
    return s ? js_intern_str(J, s) : NULL;
}

bool js_strict_equals(jsval a, jsval b)
{
    if (a.t != b.t)
        return false;
    switch (a.t) {
    case JS_UNDEF: case JS_NULL: return true;
    case JS_BOOL: return a.u.b == b.u.b;
    case JS_NUM: return a.u.n == b.u.n;
    case JS_STR: return a.u.s == b.u.s || (a.u.s->len == b.u.s->len && !memcmp(a.u.s->s, b.u.s->s, a.u.s->len));
    default: return a.u.o == b.u.o;
    }
}

bool same_value_zero(jsval a, jsval b)
{
    if (a.t == JS_NUM && b.t == JS_NUM && isnan(a.u.n) && isnan(b.u.n))
        return true;
    return js_strict_equals(a, b);
}

bool loose_equals(js *J, jsval a, jsval b)
{
    if (a.t == b.t)
        return js_strict_equals(a, b);
    if ((a.t == JS_UNDEF || a.t == JS_NULL) && (b.t == JS_UNDEF || b.t == JS_NULL))
        return true;
    if (a.t == JS_UNDEF || a.t == JS_NULL || b.t == JS_UNDEF || b.t == JS_NULL)
        return false;
    if (a.t == JS_OBJ) {
        jsval p = to_primitive(J, a, 0);
        return J->thrown ? false : loose_equals(J, p, b);
    }
    if (b.t == JS_OBJ) {
        jsval p = to_primitive(J, b, 0);
        return J->thrown ? false : loose_equals(J, a, p);
    }
    return to_number(J, a) == to_number(J, b);
}

/* ---- property access -------------------------------------------------------------------------------------------- */

static jsval obj_get(js *J, jsobj *o, jsstr *key, jsval recv)
{
    uint32_t idx;
    for (jsobj *c = o; c; c = c->proto) {
        if (c->cls == JC_HOST && c->hclass && c->hclass->get) {
            jsval out;
            if (c->hclass->get(J, c, key, &out))
                return out;
            if (J->thrown)
                return JS_UNDEFINED;
        }
        if (c->cls == JC_ARRAY) {
            if (key == J->k_length)
                return js_num((double)c->len);
            if (is_index_key(key, &idx))
                return idx < c->len ? c->items[idx] : JS_UNDEFINED;
        } else if (c->cls == JC_STRING && c->prim.t == JS_STR) {
            if (key == J->k_length)
                return js_num((double)c->prim.u.s->len);
            if (is_index_key(key, &idx) && idx < c->prim.u.s->len)
                return js_strn(J, c->prim.u.s->s + idx, 1);
        }
        struct prop *p = find_own_js(J, c, key);
        if (p) {
            if (p->flags & PF_ACCESSOR) {
                if (!p->get)
                    return JS_UNDEFINED;
                return call_function(J, p->get, recv, 0, NULL, JS_UNDEFINED);
            }
            return p->v;
        }
    }
    return JS_UNDEFINED;
}

jsval get_prop(js *J, jsval base, jsstr *key)
{
    switch (base.t) {
    case JS_UNDEF:
    case JS_NULL:
        return throw_type(J, "Cannot read properties of %s (reading '%s')", base.t == JS_UNDEF ? "undefined" : "null", key->s);
    case JS_STR: {
        uint32_t idx;
        if (key == J->k_length)
            return js_num((double)str_len16(base.u.s));
        if (is_index_key(key, &idx))
            return str_char_at(J, base.u.s, idx);
        return obj_get(J, J->string_proto, key, base);
    }
    case JS_NUM: return obj_get(J, J->number_proto, key, base);
    case JS_BOOL: return obj_get(J, J->boolean_proto, key, base);
    default: return obj_get(J, base.u.o, key, base);
    }
}

bool has_prop(js *J, jsobj *o, jsstr *key)
{
    uint32_t idx;
    for (jsobj *c = o; c; c = c->proto) {
        if (c->cls == JC_HOST && c->hclass && c->hclass->get) {
            jsval out;
            if (c->hclass->get(J, c, key, &out))
                return true;
        }
        if (c->cls == JC_ARRAY && (key == J->k_length || (is_index_key(key, &idx) && idx < c->len)))
            return true;
        if (c->cls == JC_STRING && c->prim.t == JS_STR && (key == J->k_length || (is_index_key(key, &idx) && idx < str_len16(c->prim.u.s))))
            return true;
        if (find_own_js(J, c, key))
            return true;
    }
    return false;
}

bool set_prop(js *J, jsval base, jsstr *key, jsval v)
{
    if (base.t == JS_UNDEF || base.t == JS_NULL) {
        throw_type(J, "Cannot set properties of %s (setting '%s')", base.t == JS_UNDEF ? "undefined" : "null", key->s);
        return false;
    }
    if (base.t != JS_OBJ)
        return false;
    jsobj *o = base.u.o;
    if (o->cls == JC_HOST && o->hclass && o->hclass->set) {
        if (o->hclass->set(J, o, key, v))
            return true;
        if (J->thrown)
            return false;
    }
    uint32_t idx;
    if (o->cls == JC_ARRAY) {
        if (o->flags & OF_FROZEN)
            return false;
        if (key == J->k_length) {
            double n = to_number(J, v);
            if (J->thrown)
                return false;
            if (n < 0 || n != floor(n) || n > 4294967295.0) {
                js_throw_error(J, "RangeError", "Invalid array length");
                return false;
            }
            array_set_len(J, o, (uint32_t)n);
            return true;
        }
        if (is_index_key(key, &idx)) {
            array_put(J, o, idx, v);
            return true;
        }
    }
    struct prop *p = find_own_js(J, o, key);
    if (p) {
        if (p->flags & PF_ACCESSOR) {
            if (p->set)
                call_function(J, p->set, base, 1, &v, JS_UNDEFINED);
            return !J->thrown;
        }
        if (!(p->flags & PF_WRITE) || (o->flags & OF_FROZEN))
            return false;
        p->v = v;
        return true;
    }
    for (jsobj *c = o->proto; c; c = c->proto) {           /* inherited setter? */
        struct prop *ip = find_own_js(J, c, key);
        if (ip) {
            if (ip->flags & PF_ACCESSOR) {
                if (ip->set)
                    call_function(J, ip->set, base, 1, &v, JS_UNDEFINED);
                return !J->thrown;
            }
            break;
        }
    }
    if (o->flags & (OF_FROZEN | OF_NOEXT))
        return false;
    obj_add_prop(J, o, key, v, PF_ENUM | PF_WRITE);
    return true;
}

jsval js_get(js *J, jsval base, const char *key)
{
    return get_prop(J, base, js_intern(J, key, strlen(key)));
}

void js_set(js *J, jsval base, const char *key, jsval v)
{
    set_prop(J, base, js_intern(J, key, strlen(key)), v);
}

jsval js_getv(js *J, jsval base, jsval key)
{
    jsstr *k = key_of(J, key);
    if (!k)
        return JS_UNDEFINED;
    return get_prop(J, base, k);
}

void js_def_value(js *J, jsobj *o, const char *name, jsval v)
{
    jsstr *k = js_intern(J, name, strlen(name));
    struct prop *p = find_own_js(J, o, k);
    if (p) {
        p->v = v;
        return;
    }
    obj_add_prop(J, o, k, v, PF_WRITE);
}

void own_keys(js *J, jsobj *o, jsobj *out, bool only_enum)
{
    if (o->cls == JC_ARRAY)
        for (uint32_t i = 0; i < o->len; i++)
            js_array_push(J, out, js_strv(num_to_str(J, (double)i)));
    if (o->cls == JC_STRING && o->prim.t == JS_STR)
        for (uint32_t i = 0; i < o->prim.u.s->len; i++)
            js_array_push(J, out, js_strv(num_to_str(J, (double)i)));
    if (o->cls == JC_HOST && o->hclass && o->hclass->keys)
        o->hclass->keys(J, o, out);
    /* integer-like keys first (ascending), then the rest in insertion order */
    uint32_t idx;
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < o->nprops; i++)
        if (o->props[i].key && (!only_enum || (o->props[i].flags & PF_ENUM)) && is_index_key(o->props[i].key, &idx))
            cnt++;
    if (cnt) {
        uint32_t *ids = malloc(cnt * sizeof(uint32_t));
        uint32_t k = 0;
        for (uint32_t i = 0; i < o->nprops; i++)
            if (o->props[i].key && (!only_enum || (o->props[i].flags & PF_ENUM)) && is_index_key(o->props[i].key, &idx))
                ids[k++] = idx;
        for (uint32_t a = 1; a < cnt; a++) {                   /* insertion sort: few keys */
            uint32_t x = ids[a], b = a;
            while (b > 0 && ids[b - 1] > x) { ids[b] = ids[b - 1]; b--; }
            ids[b] = x;
        }
        for (uint32_t i = 0; i < cnt; i++)
            js_array_push(J, out, js_strv(num_to_str(J, (double)ids[i])));
        free(ids);
    }
    for (uint32_t i = 0; i < o->nprops; i++)
        if (o->props[i].key && (!only_enum || (o->props[i].flags & PF_ENUM)) && !is_index_key(o->props[i].key, &idx) &&
            !(o->props[i].key->s[0] == '@' && o->props[i].key->s[1] == '@'))      /* Symbol-lite keys stay hidden */
            js_array_push(J, out, js_strv(o->props[i].key));
}

/* ---- host objects / natives -------------------------------------------------------------------------------- */

jsobj *make_native(js *J, jsstr *name, js_native fn, int nargs, void *data)
{
    jsobj *f = obj_new(J, JC_FUNCTION, J->function_proto);
    f->fn = js_alloc(J, sizeof(struct func));
    f->fn->native = fn;
    f->fn->data = data;
    f->fn->name = name;
    obj_add_prop(J, f, J->k_length, js_num(nargs), 0);
    obj_add_prop(J, f, J->k_name, js_strv(name), 0);
    return f;
}

jsobj *js_native_new(js *J, const char *name, js_native fn, int nargs, void *data)
{
    return make_native(J, js_intern(J, name, strlen(name)), fn, nargs, data);
}

void *js_fn_data(jsobj *callee) { return callee->fn ? callee->fn->data : NULL; }

void js_def_native(js *J, jsobj *o, const char *name, js_native fn, int nargs)
{
    jsstr *k = js_intern(J, name, strlen(name));
    jsobj *f = make_native(J, k, fn, nargs, NULL);
    struct prop *p = find_own_js(J, o, k);
    if (p) {
        p->v = js_objv(f);
        return;
    }
    obj_add_prop(J, o, k, js_objv(f), PF_WRITE);
}

jsobj *js_host_new(js *J, const struct js_hostclass *hc, void *ptr, jsobj *proto)
{
    jsobj *o = obj_new(J, JC_HOST, proto ? proto : J->object_proto);
    o->hclass = hc;
    o->host = ptr;
    return o;
}

void *js_host_ptr(jsval v, const struct js_hostclass *hc)
{
    if (v.t != JS_OBJ || v.u.o->cls != JC_HOST || v.u.o->hclass != hc)
        return NULL;
    return v.u.o->host;
}

void js_host_set_ptr(jsobj *o, void *ptr) { o->host = ptr; }
void *js_host_data(jsobj *o) { return o->host; }
void js_fn_mark_ctor(jsobj *f) { if (f->fn) f->fn->is_ctor_native = 1; }

/* ---- context ----------------------------------------------------------------------------------------------- */

void js_set_budget(js *J, uint64_t steps)
{
    J->steps = 0;
    J->budget = steps;
    if (J->aborted && steps) {                       /* a new run after an aborted one starts clean (memory permitting) */
        if (!J->mem_cap || J->mem <= J->mem_cap) {
            J->aborted = false;
            J->thrown = false;
        }
    }
}

void js_set_memory_cap(js *J, size_t bytes) { J->mem_cap = bytes; }
size_t js_memory_used(js *J) { return J->mem; }
bool js_memory_exhausted(js *J) { return J->mem_cap && J->mem > J->mem_cap; }
uint64_t js_steps_used(js *J) { return J->steps; }

void js_set_console(js *J, void (*fn)(void *, const char *, const char *), void *data)
{
    J->console = fn;
    J->console_data = data;
}

js *js_new(void)
{
    js *J = calloc(1, sizeof(*J));
    if (!J)
        return NULL;
    J->mem_cap = 384u << 20;
    for (int i = 0; i < 64; i++) {
        char b[4];
        int n = snprintf(b, sizeof(b), "%d", i);
        J->k_digits[i] = js_intern(J, b, (size_t)n);
    }
#define K(f, s) J->f = js_intern(J, s, strlen(s))
    K(k_length, "length"); K(k_prototype, "prototype"); K(k_constructor, "constructor"); K(k_name, "name");
    K(k_message, "message"); K(k_toString, "toString"); K(k_valueOf, "valueOf"); K(k_arguments, "arguments");
    K(k_undefined, "undefined"); K(k_stack, "stack"); K(k_get, "get"); K(k_set, "set"); K(k_value, "value");
    K(k_writable, "writable"); K(k_enumerable, "enumerable"); K(k_configurable, "configurable"); K(k_empty, "");
    K(k_this, "this"); K(k_caller, "caller"); K(k_toJSON, "toJSON"); K(k_next, "next"); K(k_done, "done");
    K(k_index, "index");
#undef K
    js_init_builtins(J);
    return J;
}

void js_free(js *J)
{
    if (!J)
        return;
    for (jsobj *o = J->all_objs; o; o = o->all_next) {
        js_regex_free_host(o);
        free(o->props);
        free(o->hidx);
        free(o->items);
        free(o->items2);
    }
    struct arena_chunk *c = J->chunks;
    while (c) {
        struct arena_chunk *n = c->next;
        free(c);
        c = n;
    }
    free(J->itab);
    free(J->mq);
    js_coro_free_all(J);
    free(J);
}
