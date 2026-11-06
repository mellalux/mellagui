/* JavaScript builtins, part 2: String, Date, Map, Set, URI functions. */
#include <ctype.h>
#include <time.h>

#include "js_lib.h"

/* ---- String --------------------------------------------------------------------------------------------------- */

NATIVE(String_ctor)
{
    jsstr *s = argc ? (ARG(0).t == JS_OBJ ? to_jsstr(J, ARG(0)) : to_jsstr(J, ARG(0))) : J->k_empty;
    THROW_IF(J);
    if (J->newtarget.t != JS_UNDEF && thisv.t == JS_OBJ) {
        thisv.u.o->cls = JC_STRING;
        thisv.u.o->prim = js_strv(s);
        return thisv;
    }
    return js_strv(s);
}
NATIVE(String_fromCharCode)
{
    struct sbuf sb = { 0 };
    for (int i = 0; i < argc; i++) {
        double d = to_number(J, argv[i]);
        THROW_IF(J);
        uint32_t cp = (uint32_t)to_int32(d) & 0xffff;
        char b[4];
        int n;
        if (cp < 0x80) { b[0] = (char)cp; n = 1; }
        else if (cp < 0x800) { b[0] = (char)(0xc0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); n = 2; }
        else { b[0] = (char)(0xe0 | cp >> 12); b[1] = (char)(0x80 | ((cp >> 6) & 63)); b[2] = (char)(0x80 | (cp & 63)); n = 3; }
        sb_add(&sb, b, (size_t)n);
    }
    return sb_to_val(J, &sb);
}
NATIVE(String_fromCodePoint)
{
    struct sbuf sb = { 0 };
    for (int i = 0; i < argc; i++) {
        double d = to_number(J, argv[i]);
        THROW_IF(J);
        uint32_t cp = (uint32_t)d;
        char b[4];
        int n;
        if (cp < 0x80) { b[0] = (char)cp; n = 1; }
        else if (cp < 0x800) { b[0] = (char)(0xc0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); n = 2; }
        else if (cp < 0x10000) { b[0] = (char)(0xe0 | cp >> 12); b[1] = (char)(0x80 | ((cp >> 6) & 63)); b[2] = (char)(0x80 | (cp & 63)); n = 3; }
        else { b[0] = (char)(0xf0 | cp >> 18); b[1] = (char)(0x80 | ((cp >> 12) & 63)); b[2] = (char)(0x80 | ((cp >> 6) & 63)); b[3] = (char)(0x80 | (cp & 63)); n = 4; }
        sb_add(&sb, b, (size_t)n);
    }
    return sb_to_val(J, &sb);
}

#define THIS_STR() jsstr *s = this_string(J, thisv); THROW_IF(J)

NATIVE(String_toString)
{
    if (thisv.t == JS_STR) return thisv;
    if (thisv.t == JS_OBJ && thisv.u.o->cls == JC_STRING) return thisv.u.o->prim;
    return throw_type(J, "String.prototype.toString requires a string");
}
/* substring by UTF-16 indexes [a, b) */
static jsval sub16(js *J, jsstr *s, double a, double b)
{
    uint32_t o1 = str_off(s, (uint32_t)a), o2 = str_off(s, (uint32_t)b);
    return o2 > o1 ? js_strn(J, s->s + o1, o2 - o1) : js_cstr(J, "");
}

NATIVE(String_charAt)
{
    THIS_STR();
    double i = arg_int(J, ARG(0), 0);
    if (i < 0 || i >= str_len16(s)) return js_cstr(J, "");
    jsval r = str_char_at(J, s, (uint32_t)i);
    return r.t == JS_UNDEF ? js_cstr(J, "") : r;
}
NATIVE(String_charCodeAt)
{
    THIS_STR();
    double i = arg_int(J, ARG(0), 0);
    if (i < 0 || i >= str_len16(s)) return js_num(NAN);
    return js_num((double)str_code_unit(s, (uint32_t)i));
}
NATIVE(String_codePointAt)
{
    THIS_STR();
    double i = arg_int(J, ARG(0), 0);
    if (i < 0 || i >= str_len16(s)) return JS_UNDEFINED;
    int32_t u = str_code_unit(s, (uint32_t)i);
    if (u >= 0xd800 && u < 0xdc00) {                              /* high half: the whole code point */
        uint32_t b = str_off(s, (uint32_t)i);
        const uint8_t *p = (const uint8_t *)s->s + b;
        if (b + 4 <= s->len)
            return js_num((double)(((uint32_t)(p[0] & 7) << 18) | ((uint32_t)(p[1] & 63) << 12) | ((uint32_t)(p[2] & 63) << 6) | (p[3] & 63)));
    }
    return js_num((double)u);
}
static const char *find_bytes(const char *h, size_t hl, const char *n, size_t nl, size_t from)
{
    if (nl == 0)
        return from <= hl ? h + from : NULL;
    if (hl < nl)
        return NULL;
    for (size_t i = from; i + nl <= hl; i++)
        if (h[i] == n[0] && !memcmp(h + i, n, nl))
            return h + i;
    return NULL;
}
NATIVE(String_indexOf)
{
    THIS_STR();
    jsstr *n = to_jsstr(J, ARG(0));
    THROW_IF(J);
    double from = fmin(fmax(arg_int(J, ARG(1), 0), 0), str_len16(s));
    const char *p = find_bytes(s->s, s->len, n->s, n->len, str_off(s, (uint32_t)from));
    return js_num(p ? (double)str_unit(s, (uint32_t)(p - s->s)) : -1);
}
NATIVE(String_lastIndexOf)
{
    THIS_STR();
    jsstr *n = to_jsstr(J, ARG(0));
    THROW_IF(J);
    if (n->len > s->len) return js_num(-1);
    for (size_t i = s->len - n->len + 1; i-- > 0;)
        if (!memcmp(s->s + i, n->s, n->len))
            return js_num((double)str_unit(s, (uint32_t)i));
    return js_num(-1);
}
NATIVE(String_includes)
{
    THIS_STR();
    jsstr *n = to_jsstr(J, ARG(0));
    THROW_IF(J);
    double from = fmin(fmax(arg_int(J, ARG(1), 0), 0), str_len16(s));
    return js_bool(find_bytes(s->s, s->len, n->s, n->len, str_off(s, (uint32_t)from)) != NULL);
}
NATIVE(String_startsWith)
{
    THIS_STR();
    jsstr *n = to_jsstr(J, ARG(0));
    THROW_IF(J);
    double from = fmin(fmax(arg_int(J, ARG(1), 0), 0), str_len16(s));
    size_t o = str_off(s, (uint32_t)from);
    return js_bool(o + n->len <= s->len && !memcmp(s->s + o, n->s, n->len));
}
NATIVE(String_endsWith)
{
    THIS_STR();
    jsstr *n = to_jsstr(J, ARG(0));
    THROW_IF(J);
    double end = fmin(fmax(arg_int(J, ARG(1), str_len16(s)), 0), str_len16(s));
    size_t o = str_off(s, (uint32_t)end);
    return js_bool(n->len <= o && !memcmp(s->s + o - n->len, n->s, n->len));
}
static double rel(js *J, jsval v, double len, double dflt)
{
    double d = arg_int(J, v, dflt);
    if (d < 0) d = fmax(len + d, 0);
    return fmin(d, len);
}
NATIVE(String_slice)
{
    THIS_STR();
    double L = str_len16(s), a = rel(J, ARG(0), L, 0), b = rel(J, ARG(1), L, L);
    if (a >= b) return js_cstr(J, "");
    return sub16(J, s, a, b);
}
NATIVE(String_substring)
{
    THIS_STR();
    double L = str_len16(s), a = fmin(fmax(arg_int(J, ARG(0), 0), 0), L), b = fmin(fmax(arg_int(J, ARG(1), L), 0), L);
    if (a > b) { double t = a; a = b; b = t; }
    return sub16(J, s, a, b);
}
NATIVE(String_substr)
{
    THIS_STR();
    double L = str_len16(s), a = rel(J, ARG(0), L, 0);
    double l = fmin(fmax(arg_int(J, ARG(1), L - a), 0), L - a);
    return sub16(J, s, a, a + l);
}
NATIVE(String_at)
{
    THIS_STR();
    double i = arg_int(J, ARG(0), 0), L = str_len16(s);
    if (i < 0) i += L;
    if (i < 0 || i >= L) return JS_UNDEFINED;
    return str_char_at(J, s, (uint32_t)i);
}
static jsval case_map(js *J, jsstr *s, bool up)
{
    jsstr *r = js_str_new(J, s->s, s->len);
    for (uint32_t i = 0; i < r->len; i++) {
        unsigned char c = (unsigned char)r->s[i];
        if (c < 0x80)
            r->s[i] = (char)(up ? toupper(c) : tolower(c));
        else if (c == 0xc3 && i + 1 < r->len) {                      /* Latin-1 supplement: ä ö ü õ é ... */
            unsigned char d = (unsigned char)r->s[i + 1];
            if (up && d >= 0xa0 && d <= 0xbe && d != 0xb7) r->s[i + 1] = (char)(d - 0x20);
            else if (!up && d >= 0x80 && d <= 0x9e && d != 0x97) r->s[i + 1] = (char)(d + 0x20);
            i++;
        }
    }
    return js_strv(r);
}
NATIVE(String_toUpperCase) { THIS_STR(); return case_map(J, s, true); }
NATIVE(String_toLowerCase) { THIS_STR(); return case_map(J, s, false); }
static bool is_ws(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
static jsval trim_impl(js *J, jsstr *s, bool l, bool r)
{
    size_t a = 0, b = s->len;
    while (l && a < b && is_ws((unsigned char)s->s[a])) a++;
    while (r && b > a && is_ws((unsigned char)s->s[b - 1])) b--;
    return js_strn(J, s->s + a, b - a);
}
NATIVE(String_trim) { THIS_STR(); return trim_impl(J, s, true, true); }
NATIVE(String_trimStart) { THIS_STR(); return trim_impl(J, s, true, false); }
NATIVE(String_trimEnd) { THIS_STR(); return trim_impl(J, s, false, true); }
NATIVE(String_concat)
{
    THIS_STR();
    jsstr *r = s;
    for (int i = 0; i < argc; i++) {
        jsstr *a = to_jsstr(J, argv[i]);
        THROW_IF(J);
        r = str_concat(J, r, a);
    }
    return js_strv(r);
}
NATIVE(String_repeat)
{
    THIS_STR();
    double n = arg_int(J, ARG(0), 0);
    if (n < 0 || !isfinite(n) || n * s->len > 100e6)
        return js_throw_error(J, "RangeError", "Invalid count value");
    struct sbuf sb = { 0 };
    for (double i = 0; i < n; i++)
        sb_add(&sb, s->s, s->len);
    return sb_to_val(J, &sb);
}
static jsval pad_impl(js *J, jsval thisv, int argc, jsval *argv, bool start)
{
    THIS_STR();
    double target = arg_int(J, ARG(0), 0);
    jsstr *fill = argc > 1 && argv[1].t != JS_UNDEF ? to_jsstr(J, argv[1]) : js_intern(J, " ", 1);
    THROW_IF(J);
    double cur = str_len16(s);
    if (target <= cur || !fill->len || target > 100e6)
        return js_strv(s);
    size_t need = (size_t)(target - cur);
    struct sbuf pad = { 0 };
    while (pad.n < need)
        sb_add(&pad, fill->s, fill->len);
    jsstr *p = js_str_new(J, pad.p, need);
    sb_free(&pad);
    return js_strv(start ? str_concat(J, p, s) : str_concat(J, s, p));
}
NATIVE(String_padStart) { return pad_impl(J, thisv, argc, argv, true); }
NATIVE(String_padEnd) { return pad_impl(J, thisv, argc, argv, false); }
NATIVE(String_split)
{
    THIS_STR();
    jsobj *r = js_array_new(J);
    double limit = ARG(1).t == JS_UNDEF ? 4294967295.0 : to_number(J, ARG(1));
    if (ARG(0).t == JS_UNDEF) {
        js_array_push(J, r, js_strv(s));
        return js_objv(r);
    }
    if (js_is_regexp(ARG(0)))
        return js_regex_split(J, s, ARG(0), limit);
    jsstr *sep = to_jsstr(J, ARG(0));
    THROW_IF(J);
    if (!sep->len) {
        for (uint32_t i = 0; i < s->len && r->len < limit;) {
            uint8_t c = (uint8_t)s->s[i];
            uint32_t l = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
            if (i + l > s->len) l = s->len - i;
            js_array_push(J, r, js_strn(J, s->s + i, l));
            i += l;
        }
        return js_objv(r);
    }
    size_t pos = 0;
    for (;;) {
        const char *p = find_bytes(s->s, s->len, sep->s, sep->len, pos);
        if (r->len >= limit)
            break;
        if (!p) {
            js_array_push(J, r, js_strn(J, s->s + pos, s->len - pos));
            break;
        }
        js_array_push(J, r, js_strn(J, s->s + pos, (size_t)(p - s->s) - pos));
        pos = (size_t)(p - s->s) + sep->len;
    }
    return js_objv(r);
}
static jsval replace_impl(js *J, jsval thisv, int argc, jsval *argv, bool all)
{
    THIS_STR();
    if (js_is_regexp(ARG(0)))
        return js_regex_replace(J, s, ARG(0), ARG(1), all);
    jsstr *pat = to_jsstr(J, ARG(0));
    THROW_IF(J);
    jsval rep = ARG(1);
    jsstr *reps = NULL;
    if (!js_is_function(rep)) {
        reps = to_jsstr(J, rep);
        THROW_IF(J);
    }
    struct sbuf sb = { 0 };
    size_t pos = 0;
    bool did = false;
    for (;;) {
        const char *p = (did && !all) ? NULL : find_bytes(s->s, s->len, pat->s, pat->len, pos);
        if (!p || (pat->len == 0 && pos > s->len)) {
            sb_add(&sb, s->s + pos, s->len - pos);
            break;
        }
        size_t at = (size_t)(p - s->s);
        sb_add(&sb, s->s + pos, at - pos);
        if (reps) {
            for (uint32_t i = 0; i < reps->len; i++) {
                if (reps->s[i] == '$' && i + 1 < reps->len) {
                    char c = reps->s[i + 1];
                    if (c == '$') { sb_putc(&sb, '$'); i++; continue; }
                    if (c == '&') { sb_add(&sb, pat->s, pat->len); i++; continue; }
                    if (c == '`') { sb_add(&sb, s->s, at); i++; continue; }
                    if (c == '\'') { sb_add(&sb, s->s + at + pat->len, s->len - at - pat->len); i++; continue; }
                }
                sb_putc(&sb, reps->s[i]);
            }
        } else {
            jsval args[3] = { js_strv(pat), js_num((double)at), js_strv(s) };
            jsval r = call_function(J, rep.u.o, JS_UNDEFINED, 3, args, JS_UNDEFINED);
            if (J->thrown) { sb_free(&sb); return JS_UNDEFINED; }
            jsstr *rs = to_jsstr(J, r);
            if (!rs) { sb_free(&sb); return JS_UNDEFINED; }
            sb_add(&sb, rs->s, rs->len);
        }
        did = true;
        pos = at + pat->len;
        if (pat->len == 0) {
            if (pos < s->len) sb_putc(&sb, s->s[pos]);
            pos++;
            if (pos > s->len) break;
        }
    }
    return sb_to_val(J, &sb);
}
NATIVE(String_replace) { return replace_impl(J, thisv, argc, argv, false); }
NATIVE(String_replaceAll) { return replace_impl(J, thisv, argc, argv, true); }
NATIVE(String_localeCompare)
{
    THIS_STR();
    jsstr *o = to_jsstr(J, ARG(0));
    THROW_IF(J);
    size_t l = s->len < o->len ? s->len : o->len;
    int c = strncasecmp(s->s, o->s, l);
    if (!c) c = memcmp(s->s, o->s, l);
    if (!c) c = s->len < o->len ? -1 : s->len > o->len ? 1 : 0;
    return js_num(c < 0 ? -1 : c > 0 ? 1 : 0);
}
NATIVE(String_normalize) { THIS_STR(); return js_strv(s); }
NATIVE(String_valueOf) { return String_toString(J, thisv, argc, argv, callee); }
NATIVE(String_match) { THIS_STR(); return js_regex_match(J, s, ARG(0)); }
NATIVE(String_matchAll) { THIS_STR(); return js_regex_match_all(J, s, ARG(0)); }
NATIVE(String_search) { THIS_STR(); return js_regex_search(J, s, ARG(0)); }

/* ---- Map / Set -------------------------------------------------------------------------------------------------- */

static jsobj *coll_this(js *J, jsval thisv, uint8_t cls)
{
    if (thisv.t == JS_OBJ && thisv.u.o->cls == cls)
        return thisv.u.o;
    throw_type(J, "method called on an incompatible receiver");
    return NULL;
}
static int64_t coll_find(jsobj *c, jsval k)
{
    for (uint32_t i = 0; i < c->len; i++)
        if (same_value_zero(c->items[i], k))
            return i;
    return -1;
}
static void coll_put(js *J, jsobj *c, jsval k, jsval v)
{
    int64_t i = coll_find(c, k);
    if (i >= 0) {
        if (c->cls == JC_MAP)
            c->items2[i] = v;
        return;
    }
    if (c->len == c->icap) {
        uint32_t nc = c->icap ? c->icap * 2 : 8;
        c->items = js_realloc_buf(J, c->items, c->icap * sizeof(jsval), nc * sizeof(jsval));
        if (c->cls == JC_MAP)
            c->items2 = js_realloc_buf(J, c->items2, c->icap * sizeof(jsval), nc * sizeof(jsval));
        c->icap = nc;
    }
    c->items[c->len] = k;
    if (c->cls == JC_MAP)
        c->items2[c->len] = v;
    c->len++;
}
static jsval coll_ctor(js *J, jsval thisv, int argc, jsval *argv, uint8_t cls)
{
    if (J->newtarget.t == JS_UNDEF)
        return throw_type(J, "Constructor %s requires 'new'", cls == JC_MAP ? "Map" : "Set");
    jsobj *c = thisv.u.o;
    c->cls = cls;
    if (argc > 0 && argv[0].t != JS_UNDEF && argv[0].t != JS_NULL) {
        jsval fn = argv[0];
        jsval arrv;
        if (fn.t == JS_OBJ && (fn.u.o->cls == JC_SET || fn.u.o->cls == JC_MAP)) {
            jsval from = js_get(J, js_objv(J->global), "Array");
            arrv = js_call(J, js_get(J, from, "from"), from, 1, &fn, NULL);
            THROW_IF(J);
        } else {
            arrv = fn;
        }
        jsobj *arr = this_array(J, arrv);
        THROW_IF(J);
        for (uint32_t i = 0; i < arr->len; i++) {
            if (cls == JC_MAP) {
                jsval k = js_getv(J, arr->items[i], js_num(0)), v = js_getv(J, arr->items[i], js_num(1));
                THROW_IF(J);
                coll_put(J, c, k, v);
            } else {
                coll_put(J, c, arr->items[i], JS_UNDEFINED);
            }
        }
    }
    return thisv;
}
NATIVE(Map_ctor) { return coll_ctor(J, thisv, argc, argv, JC_MAP); }
NATIVE(Set_ctor) { return coll_ctor(J, thisv, argc, argv, JC_SET); }
NATIVE(Map_get) { jsobj *c = coll_this(J, thisv, JC_MAP); THROW_IF(J); int64_t i = coll_find(c, ARG(0)); return i >= 0 ? c->items2[i] : JS_UNDEFINED; }
NATIVE(Map_set) { jsobj *c = coll_this(J, thisv, JC_MAP); THROW_IF(J); coll_put(J, c, ARG(0), ARG(1)); return thisv; }
NATIVE(Set_add) { jsobj *c = coll_this(J, thisv, JC_SET); THROW_IF(J); coll_put(J, c, ARG(0), JS_UNDEFINED); return thisv; }
static jsval coll_has(js *J, jsval thisv, jsval k, uint8_t cls)
{
    jsobj *c = coll_this(J, thisv, cls);
    THROW_IF(J);
    return js_bool(coll_find(c, k) >= 0);
}
NATIVE(Map_has) { return coll_has(J, thisv, ARG(0), JC_MAP); }
NATIVE(Set_has) { return coll_has(J, thisv, ARG(0), JC_SET); }
static jsval coll_delete(js *J, jsval thisv, jsval k, uint8_t cls)
{
    jsobj *c = coll_this(J, thisv, cls);
    THROW_IF(J);
    int64_t i = coll_find(c, k);
    if (i < 0)
        return js_bool(false);
    memmove(c->items + i, c->items + i + 1, (c->len - (uint32_t)i - 1) * sizeof(jsval));
    if (cls == JC_MAP)
        memmove(c->items2 + i, c->items2 + i + 1, (c->len - (uint32_t)i - 1) * sizeof(jsval));
    c->len--;
    return js_bool(true);
}
NATIVE(Map_delete) { return coll_delete(J, thisv, ARG(0), JC_MAP); }
NATIVE(Set_delete) { return coll_delete(J, thisv, ARG(0), JC_SET); }
NATIVE(Map_clear) { jsobj *c = coll_this(J, thisv, JC_MAP); THROW_IF(J); c->len = 0; return JS_UNDEFINED; }
NATIVE(Set_clear) { jsobj *c = coll_this(J, thisv, JC_SET); THROW_IF(J); c->len = 0; return JS_UNDEFINED; }
NATIVE(Map_size) { jsobj *c = coll_this(J, thisv, JC_MAP); THROW_IF(J); return js_num(c->len); }
NATIVE(Set_size) { jsobj *c = coll_this(J, thisv, JC_SET); THROW_IF(J); return js_num(c->len); }
static jsval coll_foreach(js *J, jsval thisv, int argc, jsval *argv, uint8_t cls)
{
    jsobj *c = coll_this(J, thisv, cls);
    THROW_IF(J);
    if (!js_is_function(ARG(0)))
        return throw_type(J, "callback is not a function");
    for (uint32_t i = 0; i < c->len; i++) {
        jsval args[3] = { cls == JC_MAP ? c->items2[i] : c->items[i], c->items[i], thisv };
        call_function(J, ARG(0).u.o, ARG(1), 3, args, JS_UNDEFINED);
        THROW_IF(J);
    }
    return JS_UNDEFINED;
}
NATIVE(Map_forEach) { return coll_foreach(J, thisv, argc, argv, JC_MAP); }
NATIVE(Set_forEach) { return coll_foreach(J, thisv, argc, argv, JC_SET); }
NATIVE(Map_keys) { jsobj *c = coll_this(J, thisv, JC_MAP); THROW_IF(J); jsobj *r = js_array_new(J); for (uint32_t i = 0; i < c->len; i++) js_array_push(J, r, c->items[i]); return js_objv(r); }
NATIVE(Map_values) { jsobj *c = coll_this(J, thisv, JC_MAP); THROW_IF(J); jsobj *r = js_array_new(J); for (uint32_t i = 0; i < c->len; i++) js_array_push(J, r, c->items2[i]); return js_objv(r); }
NATIVE(Map_entries)
{
    jsobj *c = coll_this(J, thisv, JC_MAP);
    THROW_IF(J);
    jsobj *r = js_array_new(J);
    for (uint32_t i = 0; i < c->len; i++) {
        jsobj *p = js_array_new(J);
        js_array_push(J, p, c->items[i]);
        js_array_push(J, p, c->items2[i]);
        js_array_push(J, r, js_objv(p));
    }
    return js_objv(r);
}
NATIVE(Set_values) { jsobj *c = coll_this(J, thisv, JC_SET); THROW_IF(J); jsobj *r = js_array_new(J); for (uint32_t i = 0; i < c->len; i++) js_array_push(J, r, c->items[i]); return js_objv(r); }
NATIVE(Set_entries)
{
    jsobj *c = coll_this(J, thisv, JC_SET);
    THROW_IF(J);
    jsobj *r = js_array_new(J);
    for (uint32_t i = 0; i < c->len; i++) {
        jsobj *p = js_array_new(J);
        js_array_push(J, p, c->items[i]);
        js_array_push(J, p, c->items[i]);
        js_array_push(J, r, js_objv(p));
    }
    return js_objv(r);
}

/* ---- Date ------------------------------------------------------------------------------------------------------- */

static double days_from_civil(double y, double m, double d)       /* m 1..12; proleptic Gregorian */
{
    y -= m <= 2;
    double era = floor(y / 400);
    double yoe = y - era * 400;
    double doy = floor((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5) + d - 1;
    double doe = yoe * 365 + floor(yoe / 4) - floor(yoe / 100) + doy;
    return era * 146097 + doe - 719468;
}
static void civil_from_days(double z, int *y, int *m, int *d)
{
    z += 719468;
    double era = floor(z / 146097);
    double doe = z - era * 146097;
    double yoe = floor((doe - floor(doe / 1460) + floor(doe / 36524) - floor(doe / 146096)) / 365);
    double yy = yoe + era * 400;
    double doy = doe - (365 * yoe + floor(yoe / 4) - floor(yoe / 100));
    double mp = floor((5 * doy + 2) / 153);
    *d = (int)(doy - floor((153 * mp + 2) / 5) + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

struct dfields { int y, mo, d, h, mi, s, ms, wd; };

static void split_ms(double ms, struct dfields *f)
{
    double days = floor(ms / 86400000.0);
    double rem = ms - days * 86400000.0;
    civil_from_days(days, &f->y, &f->mo, &f->d);
    f->mo--;
    f->h = (int)(rem / 3600000);
    f->mi = (int)fmod(rem / 60000, 60);
    f->s = (int)fmod(rem / 1000, 60);
    f->ms = (int)fmod(rem, 1000);
    f->wd = (int)fmod(fmod(days + 4, 7) + 7, 7);
}
static double join_ms(double y, double mo, double d, double h, double mi, double s, double ms)
{
    double ym = y + floor(mo / 12);
    double mn = fmod(mo, 12);
    if (mn < 0) mn += 12;
    double days = days_from_civil(ym, mn + 1, 1) + (d - 1);
    return days * 86400000.0 + h * 3600000.0 + mi * 60000.0 + s * 1000.0 + ms;
}
static double tz_offset_ms(double utc_ms)                          /* local = utc + offset */
{
    time_t t = (time_t)floor(utc_ms / 1000);
    struct tm tm;
    if (!localtime_r(&t, &tm))
        return 0;
    return (double)tm.tm_gmtoff * 1000.0;
}
static double local_to_utc(double local_ms)
{
    double off = tz_offset_ms(local_ms);
    off = tz_offset_ms(local_ms - off);
    return local_ms - off;
}

static double now_ms_epoch(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)(ts.tv_nsec / 1000000);
}

static double parse_date(const char *s)
{
    int y, mo = 1, d = 1, h = 0, mi = 0, sec = 0, n = 0;
    double ms = 0;
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (sscanf(p, "%d-%d-%d%n", &y, &mo, &d, &n) >= 1 && n > 0 && p[4] == '-') {
        p += n;
        bool has_time = false, utc = true;
        double off = 0;
        if (*p == 'T' || *p == ' ') {
            p++;
            int m2 = 0;
            if (sscanf(p, "%d:%d%n", &h, &mi, &m2) == 2) {
                p += m2;
                has_time = true;
                if (*p == ':') {
                    int m3 = 0;
                    if (sscanf(p + 1, "%d%n", &sec, &m3) == 1) p += 1 + m3;
                    if (*p == '.') {
                        p++;
                        double scale = 100;
                        while (isdigit((unsigned char)*p)) { ms += (*p - '0') * scale; scale /= 10; p++; }
                    }
                }
            }
        }
        if (*p == 'Z') {
            p++;
        } else if (*p == '+' || *p == '-') {
            int sg = *p == '-' ? -1 : 1, oh = 0, om = 0;
            if (sscanf(p + 1, "%d:%d", &oh, &om) >= 1)
                off = sg * (oh * 60 + om) * 60000.0;
            p += strlen(p);
        } else if (has_time) {
            utc = false;                                            /* date-time without offset: local time */
        }
        double t = join_ms(y, mo - 1, d, h, mi, sec, floor(ms));
        if (!utc)
            t = local_to_utc(t);
        return t - off;
    }
    /* "Nov 6, 2026", "6 Nov 2026 10:00:00", "Tue Nov 06 2026" */
    static const char *const mon[] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
    int month = -1, day = -1, year = -1;
    h = mi = sec = 0;
    char tok[40];
    while (*p) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        if (!*p) break;
        size_t l = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == ':') && l < sizeof(tok) - 1) tok[l++] = *p++;
        tok[l] = 0;
        if (strchr(tok, ':')) { sscanf(tok, "%d:%d:%d", &h, &mi, &sec); continue; }
        if (isalpha((unsigned char)tok[0])) {
            for (int i = 0; i < 12; i++)
                if (!strncasecmp(tok, mon[i], 3)) month = i;
            continue;
        }
        int v = atoi(tok);
        if (v > 31 || l >= 3) year = v;
        else if (day < 0) day = v;
    }
    if (month < 0 || year < 0)
        return NAN;
    return local_to_utc(join_ms(year, month, day < 0 ? 1 : day, h, mi, sec, 0));
}

static double *date_slot(js *J, jsval thisv)
{
    if (thisv.t == JS_OBJ && thisv.u.o->cls == JC_DATE)
        return &thisv.u.o->prim.u.n;
    throw_type(J, "this is not a Date object");
    return NULL;
}

NATIVE(Date_ctor)
{
    double t;
    if (argc == 0) t = now_ms_epoch();
    else if (argc == 1) {
        jsval v = argv[0];
        if (v.t == JS_OBJ && v.u.o->cls == JC_DATE) t = v.u.o->prim.u.n;
        else {
            jsval p = to_primitive(J, v, 0);
            THROW_IF(J);
            if (p.t == JS_STR) t = parse_date(p.u.s->s);
            else { t = to_number(J, p); THROW_IF(J); t = isfinite(t) ? trunc(t) : NAN; }
        }
    } else {
        double f[7] = { 0, 0, 1, 0, 0, 0, 0 };
        for (int i = 0; i < 7 && i < argc; i++) {
            f[i] = to_number(J, argv[i]);
            THROW_IF(J);
        }
        if (f[0] >= 0 && f[0] <= 99) f[0] += 1900;
        t = local_to_utc(join_ms(f[0], f[1], f[2], f[3], f[4], f[5], f[6]));
    }
    if (J->newtarget.t == JS_UNDEF) {                               /* Date() as a function: the current time as a string */
        jsval d = js_objv(obj_new(J, JC_DATE, J->date_proto));
        d.u.o->prim = js_num(now_ms_epoch());
        jsval f = get_prop(J, d, J->k_toString);
        return call_function(J, f.u.o, d, 0, NULL, JS_UNDEFINED);
    }
    jsobj *o = thisv.u.o;
    o->cls = JC_DATE;
    o->prim = js_num(t);
    return thisv;
}
NATIVE(Date_now) { return js_num(now_ms_epoch()); }
NATIVE(Date_parse) { jsstr *s = to_jsstr(J, ARG(0)); THROW_IF(J); return js_num(parse_date(s->s)); }
NATIVE(Date_UTC)
{
    double f[7] = { 0, 0, 1, 0, 0, 0, 0 };
    for (int i = 0; i < 7 && i < argc; i++) { f[i] = to_number(J, argv[i]); THROW_IF(J); }
    if (f[0] >= 0 && f[0] <= 99) f[0] += 1900;
    return js_num(join_ms(f[0], f[1], f[2], f[3], f[4], f[5], f[6]));
}
NATIVE(Date_getTime) { double *t = date_slot(J, thisv); THROW_IF(J); return js_num(*t); }

#define DATE_GET(name, local, field) \
    NATIVE(Date_##name) { double *t = date_slot(J, thisv); THROW_IF(J); if (isnan(*t)) return js_num(NAN); \
        struct dfields f; split_ms(local ? *t + tz_offset_ms(*t) : *t, &f); return js_num(field); }
DATE_GET(getFullYear, 1, f.y) DATE_GET(getMonth, 1, f.mo) DATE_GET(getDate, 1, f.d) DATE_GET(getDay, 1, f.wd)
DATE_GET(getHours, 1, f.h) DATE_GET(getMinutes, 1, f.mi) DATE_GET(getSeconds, 1, f.s) DATE_GET(getMilliseconds, 1, f.ms)
DATE_GET(getUTCFullYear, 0, f.y) DATE_GET(getUTCMonth, 0, f.mo) DATE_GET(getUTCDate, 0, f.d) DATE_GET(getUTCDay, 0, f.wd)
DATE_GET(getUTCHours, 0, f.h) DATE_GET(getUTCMinutes, 0, f.mi) DATE_GET(getUTCSeconds, 0, f.s)
DATE_GET(getUTCMilliseconds, 0, f.ms)
NATIVE(Date_getTimezoneOffset) { double *t = date_slot(J, thisv); THROW_IF(J); return js_num(-tz_offset_ms(*t) / 60000.0); }

/* setters: which = 0 year,1 month,2 date,3 hours,4 minutes,5 seconds,6 ms */
static jsval date_set(js *J, jsval thisv, int argc, jsval *argv, int which, bool local)
{
    double *t = date_slot(J, thisv);
    THROW_IF(J);
    double base = isnan(*t) ? 0 : *t;
    struct dfields f;
    split_ms(local ? base + tz_offset_ms(base) : base, &f);
    double v[7] = { f.y, f.mo, f.d, f.h, f.mi, f.s, f.ms };
    for (int i = 0; i < argc && which + i < 7; i++) {
        v[which + i] = to_number(J, argv[i]);
        THROW_IF(J);
    }
    double r = join_ms(v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
    *t = argc == 0 ? NAN : (local ? local_to_utc(r) : r);
    return js_num(*t);
}
#define DATE_SET(name, which, local) NATIVE(Date_##name) { return date_set(J, thisv, argc, argv, which, local); }
DATE_SET(setFullYear, 0, 1) DATE_SET(setMonth, 1, 1) DATE_SET(setDate, 2, 1) DATE_SET(setHours, 3, 1)
DATE_SET(setMinutes, 4, 1) DATE_SET(setSeconds, 5, 1) DATE_SET(setMilliseconds, 6, 1)
DATE_SET(setUTCFullYear, 0, 0) DATE_SET(setUTCMonth, 1, 0) DATE_SET(setUTCDate, 2, 0) DATE_SET(setUTCHours, 3, 0)
DATE_SET(setUTCMinutes, 4, 0) DATE_SET(setUTCSeconds, 5, 0) DATE_SET(setUTCMilliseconds, 6, 0)
NATIVE(Date_setTime) { double *t = date_slot(J, thisv); THROW_IF(J); double d = to_number(J, ARG(0)); THROW_IF(J); *t = d; return js_num(d); }

static const char *const DAYS[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

NATIVE(Date_toISOString)
{
    double *t = date_slot(J, thisv);
    THROW_IF(J);
    if (isnan(*t))
        return js_throw_error(J, "RangeError", "Invalid time value");
    struct dfields f;
    split_ms(*t, &f);
    char b[48];
    snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", f.y, f.mo + 1, f.d, f.h, f.mi, f.s, f.ms);
    return js_cstr(J, b);
}
NATIVE(Date_toJSON)
{
    double *t = date_slot(J, thisv);
    THROW_IF(J);
    if (isnan(*t))
        return JS_NULLV;
    return Date_toISOString(J, thisv, 0, NULL, callee);
}
NATIVE(Date_toString)
{
    double *t = date_slot(J, thisv);
    THROW_IF(J);
    if (isnan(*t))
        return js_cstr(J, "Invalid Date");
    double off = tz_offset_ms(*t);
    struct dfields f;
    split_ms(*t + off, &f);
    int om = (int)(off / 60000);
    char b[96];
    snprintf(b, sizeof(b), "%s %s %02d %04d %02d:%02d:%02d GMT%c%02d%02d", DAYS[f.wd], MONTHS[f.mo], f.d, f.y, f.h, f.mi, f.s,
             om < 0 ? '-' : '+', abs(om) / 60, abs(om) % 60);
    return js_cstr(J, b);
}
NATIVE(Date_toUTCString)
{
    double *t = date_slot(J, thisv);
    THROW_IF(J);
    if (isnan(*t))
        return js_cstr(J, "Invalid Date");
    struct dfields f;
    split_ms(*t, &f);
    char b[64];
    snprintf(b, sizeof(b), "%s, %02d %s %04d %02d:%02d:%02d GMT", DAYS[f.wd], f.d, MONTHS[f.mo], f.y, f.h, f.mi, f.s);
    return js_cstr(J, b);
}
NATIVE(Date_toDateString)
{
    double *t = date_slot(J, thisv);
    THROW_IF(J);
    if (isnan(*t))
        return js_cstr(J, "Invalid Date");
    struct dfields f;
    split_ms(*t + tz_offset_ms(*t), &f);
    char b[48];
    snprintf(b, sizeof(b), "%s %s %02d %04d", DAYS[f.wd], MONTHS[f.mo], f.d, f.y);
    return js_cstr(J, b);
}
static jsval locale_fmt(js *J, jsval thisv, bool date, bool time_)
{
    double *t = date_slot(J, thisv);
    THROW_IF(J);
    if (isnan(*t))
        return js_cstr(J, "Invalid Date");
    struct dfields f;
    split_ms(*t + tz_offset_ms(*t), &f);
    char b[80], d[32] = "", tm[32] = "";
    if (date)
        snprintf(d, sizeof(d), "%d/%d/%d", f.mo + 1, f.d, f.y);
    if (time_)
        snprintf(tm, sizeof(tm), "%d:%02d:%02d %s", f.h % 12 == 0 ? 12 : f.h % 12, f.mi, f.s, f.h < 12 ? "AM" : "PM");
    snprintf(b, sizeof(b), "%s%s%s", d, date && time_ ? ", " : "", tm);
    return js_cstr(J, b);
}
NATIVE(Date_toLocaleDateString) { return locale_fmt(J, thisv, true, false); }
NATIVE(Date_toLocaleTimeString) { return locale_fmt(J, thisv, false, true); }
NATIVE(Date_toLocaleString) { return locale_fmt(J, thisv, true, true); }
NATIVE(Date_valueOf) { return Date_getTime(J, thisv, argc, argv, callee); }

/* ---- URI -------------------------------------------------------------------------------------------------------- */

static jsval uri_encode(js *J, jsval v, const char *keep)
{
    jsstr *s = to_jsstr(J, v);
    THROW_IF(J);
    struct sbuf sb = { 0 };
    for (uint32_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->s[i];
        if (isalnum(c) || strchr(keep, c) ) {
            sb_putc(&sb, (char)c);
        } else {
            char b[4];
            snprintf(b, sizeof(b), "%%%02X", c);
            sb_addz(&sb, b);
        }
    }
    return sb_to_val(J, &sb);
}
static jsval uri_decode(js *J, jsval v, const char *reserved)
{
    jsstr *s = to_jsstr(J, v);
    THROW_IF(J);
    struct sbuf sb = { 0 };
    for (uint32_t i = 0; i < s->len; i++) {
        if (s->s[i] == '%') {
            if (i + 2 < s->len && isxdigit((unsigned char)s->s[i + 1]) && isxdigit((unsigned char)s->s[i + 2])) {
                char h[3] = { s->s[i + 1], s->s[i + 2], 0 };
                char c = (char)strtol(h, NULL, 16);
                if (c && strchr(reserved, c))
                    sb_add(&sb, s->s + i, 3);
                else
                    sb_putc(&sb, c);
                i += 2;
                continue;
            }
            sb_free(&sb);
            return js_throw_error(J, "URIError", "URI malformed");
        }
        sb_putc(&sb, s->s[i]);
    }
    return sb_to_val(J, &sb);
}
NATIVE(global_encodeURIComponent) { return uri_encode(J, ARG(0), "-_.!~*'()"); }
NATIVE(global_encodeURI) { return uri_encode(J, ARG(0), "-_.!~*'();/?:@&=+$,#"); }
NATIVE(global_decodeURIComponent) { return uri_decode(J, ARG(0), ""); }
NATIVE(global_decodeURI) { return uri_decode(J, ARG(0), ";/?:@&=+$,#"); }

/* ---- setup ------------------------------------------------------------------------------------------------------- */

void js_init_lib2(js *J)
{
    jsobj *S = lib_ctor(J, "String", String_ctor, 1, J->string_proto);
    lib_def(J, S, "fromCharCode", String_fromCharCode, 1);
    lib_def(J, S, "fromCodePoint", String_fromCodePoint, 1);
    jsobj *SP = J->string_proto;
    lib_def(J, SP, "toString", String_toString, 0); lib_def(J, SP, "valueOf", String_valueOf, 0);
    lib_def(J, SP, "charAt", String_charAt, 1); lib_def(J, SP, "charCodeAt", String_charCodeAt, 1);
    lib_def(J, SP, "codePointAt", String_codePointAt, 1); lib_def(J, SP, "indexOf", String_indexOf, 1);
    lib_def(J, SP, "lastIndexOf", String_lastIndexOf, 1); lib_def(J, SP, "includes", String_includes, 1);
    lib_def(J, SP, "startsWith", String_startsWith, 1); lib_def(J, SP, "endsWith", String_endsWith, 1);
    lib_def(J, SP, "slice", String_slice, 2); lib_def(J, SP, "substring", String_substring, 2);
    lib_def(J, SP, "substr", String_substr, 2); lib_def(J, SP, "at", String_at, 1);
    lib_def(J, SP, "toUpperCase", String_toUpperCase, 0); lib_def(J, SP, "toLowerCase", String_toLowerCase, 0);
    lib_def(J, SP, "toLocaleUpperCase", String_toUpperCase, 0); lib_def(J, SP, "toLocaleLowerCase", String_toLowerCase, 0);
    lib_def(J, SP, "trim", String_trim, 0); lib_def(J, SP, "trimStart", String_trimStart, 0);
    lib_def(J, SP, "trimEnd", String_trimEnd, 0); lib_def(J, SP, "trimLeft", String_trimStart, 0);
    lib_def(J, SP, "trimRight", String_trimEnd, 0); lib_def(J, SP, "concat", String_concat, 1);
    lib_def(J, SP, "repeat", String_repeat, 1); lib_def(J, SP, "padStart", String_padStart, 2);
    lib_def(J, SP, "padEnd", String_padEnd, 2); lib_def(J, SP, "split", String_split, 2);
    lib_def(J, SP, "replace", String_replace, 2); lib_def(J, SP, "replaceAll", String_replaceAll, 2);
    lib_def(J, SP, "localeCompare", String_localeCompare, 1); lib_def(J, SP, "normalize", String_normalize, 0);
    lib_def(J, SP, "match", String_match, 1); lib_def(J, SP, "search", String_search, 1);
    lib_def(J, SP, "matchAll", String_matchAll, 1);

    jsobj *M = lib_ctor(J, "Map", Map_ctor, 0, J->map_proto);
    (void)M;
    jsobj *MP = J->map_proto;
    lib_def(J, MP, "get", Map_get, 1); lib_def(J, MP, "set", Map_set, 2); lib_def(J, MP, "has", Map_has, 1);
    lib_def(J, MP, "delete", Map_delete, 1); lib_def(J, MP, "clear", Map_clear, 0); lib_def(J, MP, "forEach", Map_forEach, 1);
    lib_def(J, MP, "keys", Map_keys, 0); lib_def(J, MP, "values", Map_values, 0); lib_def(J, MP, "entries", Map_entries, 0);
    lib_getter(J, MP, "size", Map_size);
    jsobj *St = lib_ctor(J, "Set", Set_ctor, 0, J->set_proto);
    (void)St;
    jsobj *StP = J->set_proto;
    lib_def(J, StP, "add", Set_add, 1); lib_def(J, StP, "has", Set_has, 1); lib_def(J, StP, "delete", Set_delete, 1);
    lib_def(J, StP, "clear", Set_clear, 0); lib_def(J, StP, "forEach", Set_forEach, 1); lib_def(J, StP, "values", Set_values, 0);
    lib_def(J, StP, "keys", Set_values, 0); lib_def(J, StP, "entries", Set_entries, 0);
    lib_getter(J, StP, "size", Set_size);

    jsobj *D = lib_ctor(J, "Date", Date_ctor, 7, J->date_proto);
    lib_def(J, D, "now", Date_now, 0); lib_def(J, D, "parse", Date_parse, 1); lib_def(J, D, "UTC", Date_UTC, 7);
    jsobj *DP = J->date_proto;
#define D(n, a) lib_def(J, DP, #n, Date_##n, a)
    D(getTime, 0); D(valueOf, 0); D(getFullYear, 0); D(getMonth, 0); D(getDate, 0); D(getDay, 0); D(getHours, 0);
    D(getMinutes, 0); D(getSeconds, 0); D(getMilliseconds, 0); D(getUTCFullYear, 0); D(getUTCMonth, 0); D(getUTCDate, 0);
    D(getUTCDay, 0); D(getUTCHours, 0); D(getUTCMinutes, 0); D(getUTCSeconds, 0); D(getUTCMilliseconds, 0);
    D(getTimezoneOffset, 0); D(setFullYear, 3); D(setMonth, 2); D(setDate, 1); D(setHours, 4); D(setMinutes, 3);
    D(setSeconds, 2); D(setMilliseconds, 1); D(setUTCFullYear, 3); D(setUTCMonth, 2); D(setUTCDate, 1); D(setUTCHours, 4);
    D(setUTCMinutes, 3); D(setUTCSeconds, 2); D(setUTCMilliseconds, 1); D(setTime, 1); D(toISOString, 0); D(toJSON, 1);
    D(toString, 0); D(toUTCString, 0); D(toDateString, 0); D(toLocaleDateString, 0); D(toLocaleTimeString, 0);
    D(toLocaleString, 0);
#undef D

    lib_def(J, J->global, "encodeURIComponent", global_encodeURIComponent, 1);
    lib_def(J, J->global, "encodeURI", global_encodeURI, 1);
    lib_def(J, J->global, "decodeURIComponent", global_decodeURIComponent, 1);
    lib_def(J, J->global, "decodeURI", global_decodeURI, 1);
}
