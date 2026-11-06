/* JavaScript builtins, part 1: Object, Function, Array, Error, Boolean, Number, Math, JSON, console, globals. */
#include <ctype.h>
#include <time.h>

#include "js_lib.h"

/* ---- helpers ------------------------------------------------------------------------------------------------ */

void sb_add(struct sbuf *s, const char *d, size_t n)
{
    if (s->n + n + 1 > s->cap) {
        s->cap = s->cap * 2 > s->n + n + 1 ? s->cap * 2 : s->n + n + 64;
        s->p = realloc(s->p, s->cap);
        if (!s->p)
            exit(1);
    }
    memcpy(s->p + s->n, d, n);
    s->n += n;
    s->p[s->n] = 0;
}
void sb_addz(struct sbuf *s, const char *z) { sb_add(s, z, strlen(z)); }
void sb_putc(struct sbuf *s, char c) { sb_add(s, &c, 1); }
void sb_free(struct sbuf *s) { free(s->p); s->p = NULL; s->n = s->cap = 0; }
jsval sb_to_val(js *J, struct sbuf *s)
{
    jsval v = js_strn(J, s->p ? s->p : "", s->n);
    sb_free(s);
    return v;
}

jsobj *lib_ctor(js *J, const char *name, js_native fn, int nargs, jsobj *proto)
{
    jsobj *c = js_native_new(J, name, fn, nargs, NULL);
    c->fn->is_ctor_native = 1;
    if (proto) {
        obj_add_prop(J, c, J->k_prototype, js_objv(proto), 0);
        obj_add_prop(J, proto, J->k_constructor, js_objv(c), PF_WRITE);
    }
    obj_add_prop(J, J->global, js_intern(J, name, strlen(name)), js_objv(c), PF_WRITE);
    return c;
}

void lib_def(js *J, jsobj *o, const char *name, js_native fn, int nargs) { js_def_native(J, o, name, fn, nargs); }
void lib_const(js *J, jsobj *o, const char *name, jsval v) { obj_add_prop(J, o, js_intern(J, name, strlen(name)), v, 0); }

void lib_getter(js *J, jsobj *o, const char *name, js_native fn)
{
    jsstr *k = js_intern(J, name, strlen(name));
    struct prop *p = obj_add_prop(J, o, k, JS_UNDEFINED, PF_ACCESSOR);
    p->get = make_native(J, k, fn, 0, NULL);
}

double arg_int(js *J, jsval v, double dflt)
{
    if (v.t == JS_UNDEF)
        return dflt;
    double d = to_number(J, v);
    if (isnan(d))
        return 0;
    return trunc(d);
}

jsstr *this_string(js *J, jsval thisv)
{
    if (thisv.t == JS_STR)
        return thisv.u.s;
    if (thisv.t == JS_OBJ && thisv.u.o->cls == JC_STRING && thisv.u.o->prim.t == JS_STR)
        return thisv.u.o->prim.u.s;
    if (thisv.t == JS_UNDEF || thisv.t == JS_NULL) {
        throw_type(J, "String.prototype method called on null or undefined");
        return NULL;
    }
    return to_jsstr(J, thisv);
}

jsobj *this_array(js *J, jsval thisv)
{
    if (thisv.t == JS_OBJ && thisv.u.o->cls == JC_ARRAY)
        return thisv.u.o;
    jsobj *a = js_array_new(J);
    if (thisv.t == JS_STR) {
        for (uint32_t i = 0; i < thisv.u.s->len; i++)
            js_array_push(J, a, js_strn(J, thisv.u.s->s + i, 1));
    } else if (thisv.t == JS_OBJ && js_has_iterator(J, thisv)) {      /* generators and other iterables */
        jsval *items;
        int cnt;
        if (gen_collect(J, thisv, &items, &cnt, -1)) {
            for (int i = 0; i < cnt; i++)
                js_array_push(J, a, items[i]);
            free(items);
        }
    } else if (thisv.t == JS_OBJ) {
        jsval lv = get_prop(J, thisv, J->k_length);
        if (J->thrown)
            return a;
        uint32_t n = lv.t == JS_NUM && lv.u.n > 0 ? (uint32_t)lv.u.n : 0;
        for (uint32_t i = 0; i < n && !J->thrown; i++)
            js_array_push(J, a, get_prop(J, thisv, js_intern_str(J, num_to_str(J, (double)i))));
    } else if (thisv.t == JS_UNDEF || thisv.t == JS_NULL) {
        throw_type(J, "Array.prototype method called on null or undefined");
    }
    return a;
}

static jsobj *obj_arg(js *J, jsval v, const char *what)
{
    if (v.t != JS_OBJ) {
        throw_type(J, "%s called on non-object", what);
        return NULL;
    }
    return v.u.o;
}

/* ---- Object ---------------------------------------------------------------------------------------------------- */

NATIVE(Object_ctor)
{
    jsval v = ARG(0);
    if (v.t == JS_UNDEF || v.t == JS_NULL)
        return J->newtarget.t != JS_UNDEF && thisv.t == JS_OBJ ? thisv : js_objv(js_object_new(J));
    if (v.t == JS_OBJ)
        return v;
    jsobj *w = js_object_new(J);
    w->cls = v.t == JS_STR ? JC_STRING : v.t == JS_NUM ? JC_NUMBER : JC_BOOLEAN;
    w->prim = v;
    w->proto = v.t == JS_STR ? J->string_proto : v.t == JS_NUM ? J->number_proto : J->boolean_proto;
    return js_objv(w);
}

static jsobj *keys_of(js *J, jsval v, bool only_enum)
{
    jsobj *out = js_array_new(J);
    if (v.t == JS_STR) {
        for (uint32_t i = 0; i < v.u.s->len; i++)
            js_array_push(J, out, js_strv(num_to_str(J, (double)i)));
    } else if (v.t == JS_OBJ) {
        own_keys(J, v.u.o, out, only_enum);
    } else if (v.t == JS_UNDEF || v.t == JS_NULL) {
        throw_type(J, "Cannot convert undefined or null to object");
    }
    return out;
}

NATIVE(Object_keys) { return js_objv(keys_of(J, ARG(0), true)); }
NATIVE(Object_getOwnPropertyNames)
{
    jsobj *k = keys_of(J, ARG(0), false);
    if (ARG(0).t == JS_OBJ && ARG(0).u.o->cls == JC_ARRAY)
        js_array_push(J, k, js_strv(J->k_length));
    return js_objv(k);
}
NATIVE(Object_values)
{
    jsobj *k = keys_of(J, ARG(0), true);
    THROW_IF(J);
    jsobj *out = js_array_new(J);
    for (uint32_t i = 0; i < k->len; i++)
        js_array_push(J, out, get_prop(J, ARG(0), js_intern_str(J, k->items[i].u.s)));
    return js_objv(out);
}
NATIVE(Object_entries)
{
    jsobj *k = keys_of(J, ARG(0), true);
    THROW_IF(J);
    jsobj *out = js_array_new(J);
    for (uint32_t i = 0; i < k->len; i++) {
        jsobj *pair = js_array_new(J);
        js_array_push(J, pair, k->items[i]);
        js_array_push(J, pair, get_prop(J, ARG(0), js_intern_str(J, k->items[i].u.s)));
        js_array_push(J, out, js_objv(pair));
    }
    return js_objv(out);
}
NATIVE(Object_assign)
{
    jsval target = ARG(0);
    if (target.t != JS_OBJ)
        return throw_type(J, "Cannot convert undefined or null to object");
    for (int i = 1; i < argc; i++) {
        if (argv[i].t != JS_OBJ && argv[i].t != JS_STR)
            continue;
        jsobj *k = keys_of(J, argv[i], true);
        for (uint32_t j = 0; j < k->len && !J->thrown; j++) {
            jsstr *key = js_intern_str(J, k->items[j].u.s);
            jsval v = get_prop(J, argv[i], key);
            THROW_IF(J);
            set_prop(J, target, key, v);
        }
    }
    return target;
}
NATIVE(Object_create)
{
    jsval p = ARG(0);
    if (p.t != JS_OBJ && p.t != JS_NULL)
        return throw_type(J, "Object prototype may only be an Object or null");
    jsobj *o = obj_new(J, JC_OBJECT, p.t == JS_OBJ ? p.u.o : NULL);
    if (ARG(1).t == JS_OBJ) {
        jsval dp = js_get(J, js_objv(J->global), "Object");
        jsval defprops = get_prop(J, dp, js_intern(J, "defineProperties", 16));
        jsval args[2] = { js_objv(o), ARG(1) };
        call_function(J, defprops.u.o, dp, 2, args, JS_UNDEFINED);
    }
    return js_objv(o);
}
NATIVE(Object_getPrototypeOf)
{
    jsval v = ARG(0);
    if (v.t == JS_STR) return js_objv(J->string_proto);
    if (v.t == JS_NUM) return js_objv(J->number_proto);
    if (v.t == JS_BOOL) return js_objv(J->boolean_proto);
    if (v.t != JS_OBJ)
        return throw_type(J, "Cannot convert undefined or null to object");
    return v.u.o->proto ? js_objv(v.u.o->proto) : JS_NULLV;
}
NATIVE(Object_setPrototypeOf)
{
    jsobj *o = obj_arg(J, ARG(0), "Object.setPrototypeOf");
    THROW_IF(J);
    o->proto = ARG(1).t == JS_OBJ ? ARG(1).u.o : NULL;
    return ARG(0);
}
static void define_prop(js *J, jsobj *o, jsstr *key, jsval desc)
{
    if (desc.t != JS_OBJ) {
        throw_type(J, "Property description must be an object");
        return;
    }
    jsval g = get_prop(J, desc, J->k_get), s = get_prop(J, desc, J->k_set), v = get_prop(J, desc, J->k_value);
    jsval w = get_prop(J, desc, J->k_writable), e = get_prop(J, desc, J->k_enumerable);
    if (J->thrown)
        return;
    uint32_t idx;
    if (o->cls == JC_ARRAY && (key == J->k_length || is_index_key(key, &idx))) {
        set_prop(J, js_objv(o), key, v);
        return;
    }
    struct prop *p = obj_find_own(o, key);
    bool is_new = !p;
    if (!p)
        p = obj_add_prop(J, o, key, JS_UNDEFINED, 0);
    if (js_is_function(g) || js_is_function(s)) {
        p->flags = (uint8_t)((p->flags & PF_ENUM) | PF_ACCESSOR);
        if (js_is_function(g)) p->get = g.u.o;
        if (js_is_function(s)) p->set = s.u.o;
    } else {
        if (v.t != JS_UNDEF || is_new || (p->flags & PF_ACCESSOR))
            p->v = v;
        if (p->flags & PF_ACCESSOR) {
            p->flags &= (uint8_t)~PF_ACCESSOR;
            p->get = p->set = NULL;
        }
    }
    if (e.t != JS_UNDEF)
        p->flags = (uint8_t)(js_truthy(e) ? (p->flags | PF_ENUM) : (p->flags & ~PF_ENUM));
    if (w.t != JS_UNDEF && !(p->flags & PF_ACCESSOR))
        p->flags = (uint8_t)(js_truthy(w) ? (p->flags | PF_WRITE) : (p->flags & ~PF_WRITE));
}
NATIVE(Object_defineProperty)
{
    jsobj *o = obj_arg(J, ARG(0), "Object.defineProperty");
    THROW_IF(J);
    jsstr *k = key_of(J, ARG(1));
    THROW_IF(J);
    define_prop(J, o, k, ARG(2));
    return ARG(0);
}
NATIVE(Object_defineProperties)
{
    jsobj *o = obj_arg(J, ARG(0), "Object.defineProperties");
    THROW_IF(J);
    jsobj *ks = keys_of(J, ARG(1), true);
    for (uint32_t i = 0; i < ks->len && !J->thrown; i++) {
        jsstr *k = js_intern_str(J, ks->items[i].u.s);
        define_prop(J, o, k, get_prop(J, ARG(1), k));
    }
    return ARG(0);
}
NATIVE(Object_getOwnPropertyDescriptor)
{
    if (ARG(0).t != JS_OBJ)
        return JS_UNDEFINED;
    jsstr *k = key_of(J, ARG(1));
    THROW_IF(J);
    jsobj *o = ARG(0).u.o;
    uint32_t idx;
    jsobj *d = js_object_new(J);
    if (o->cls == JC_ARRAY && (k == J->k_length || (is_index_key(k, &idx) && idx < o->len))) {
        set_prop(J, js_objv(d), J->k_value, get_prop(J, ARG(0), k));
        set_prop(J, js_objv(d), J->k_writable, js_bool(true));
        set_prop(J, js_objv(d), J->k_enumerable, js_bool(k != J->k_length));
        set_prop(J, js_objv(d), J->k_configurable, js_bool(k != J->k_length));
        return js_objv(d);
    }
    struct prop *p = obj_find_own(o, k);
    if (!p)
        return JS_UNDEFINED;
    if (p->flags & PF_ACCESSOR) {
        set_prop(J, js_objv(d), J->k_get, p->get ? js_objv(p->get) : JS_UNDEFINED);
        set_prop(J, js_objv(d), J->k_set, p->set ? js_objv(p->set) : JS_UNDEFINED);
    } else {
        set_prop(J, js_objv(d), J->k_value, p->v);
        set_prop(J, js_objv(d), J->k_writable, js_bool(p->flags & PF_WRITE && !(o->flags & OF_FROZEN)));
    }
    set_prop(J, js_objv(d), J->k_enumerable, js_bool(p->flags & PF_ENUM));
    set_prop(J, js_objv(d), J->k_configurable, js_bool(!(o->flags & OF_FROZEN)));
    return js_objv(d);
}
NATIVE(Object_freeze) { if (ARG(0).t == JS_OBJ) ARG(0).u.o->flags |= OF_FROZEN | OF_NOEXT; return ARG(0); }
NATIVE(Object_isFrozen) { return js_bool(ARG(0).t != JS_OBJ || (ARG(0).u.o->flags & OF_FROZEN)); }
NATIVE(Object_seal) { if (ARG(0).t == JS_OBJ) ARG(0).u.o->flags |= OF_NOEXT; return ARG(0); }
NATIVE(Object_preventExtensions) { if (ARG(0).t == JS_OBJ) ARG(0).u.o->flags |= OF_NOEXT; return ARG(0); }
NATIVE(Object_isExtensible) { return js_bool(ARG(0).t == JS_OBJ && !(ARG(0).u.o->flags & OF_NOEXT)); }
NATIVE(Object_fromEntries)
{
    jsobj *o = js_object_new(J);
    jsval *items;
    jsobj *arr = this_array(J, ARG(0));
    THROW_IF(J);
    items = arr->items;
    for (uint32_t i = 0; i < arr->len && !J->thrown; i++) {
        jsval pair = items[i];
        jsval k = js_getv(J, pair, js_num(0)), v = js_getv(J, pair, js_num(1));
        THROW_IF(J);
        jsstr *key = key_of(J, k);
        THROW_IF(J);
        set_prop(J, js_objv(o), key, v);
    }
    return js_objv(o);
}
NATIVE(Object_is)
{
    jsval a = ARG(0), b = ARG(1);
    if (a.t == JS_NUM && b.t == JS_NUM) {
        if (isnan(a.u.n) && isnan(b.u.n))
            return js_bool(true);
        if (a.u.n == 0 && b.u.n == 0)
            return js_bool(signbit(a.u.n) == signbit(b.u.n));
    }
    return js_bool(js_strict_equals(a, b));
}
NATIVE(Object_hasOwnProperty)
{
    if (thisv.t == JS_UNDEF || thisv.t == JS_NULL)
        return throw_type(J, "Cannot convert undefined or null to object");
    jsstr *k = key_of(J, ARG(0));
    THROW_IF(J);
    uint32_t idx;
    if (thisv.t == JS_STR)
        return js_bool(k == J->k_length || (is_index_key(k, &idx) && idx < thisv.u.s->len));
    if (thisv.t != JS_OBJ)
        return js_bool(false);
    jsobj *o = thisv.u.o;
    if (o->cls == JC_ARRAY && (k == J->k_length || (is_index_key(k, &idx) && idx < o->len)))
        return js_bool(true);
    if (o->cls == JC_HOST && o->hclass && o->hclass->get) {
        jsval out;
        if (o->hclass->get(J, o, k, &out))
            return js_bool(true);
    }
    return js_bool(obj_find_own(o, k) != NULL);
}
NATIVE(Object_propertyIsEnumerable)
{
    jsstr *k = key_of(J, ARG(0));
    THROW_IF(J);
    if (thisv.t != JS_OBJ)
        return js_bool(false);
    struct prop *p = obj_find_own(thisv.u.o, k);
    uint32_t idx;
    return js_bool((p && (p->flags & PF_ENUM)) || (thisv.u.o->cls == JC_ARRAY && is_index_key(k, &idx) && idx < thisv.u.o->len));
}
NATIVE(Object_isPrototypeOf)
{
    if (thisv.t != JS_OBJ || ARG(0).t != JS_OBJ)
        return js_bool(false);
    for (jsobj *o = ARG(0).u.o->proto; o; o = o->proto)
        if (o == thisv.u.o)
            return js_bool(true);
    return js_bool(false);
}
NATIVE(Object_toString)
{
    const char *t = "Object";
    if (thisv.t == JS_UNDEF) return js_cstr(J, "[object Undefined]");
    if (thisv.t == JS_NULL) return js_cstr(J, "[object Null]");
    if (thisv.t == JS_STR) t = "String";
    else if (thisv.t == JS_NUM) t = "Number";
    else if (thisv.t == JS_BOOL) t = "Boolean";
    else switch (thisv.u.o->cls) {
    case JC_ARRAY: t = "Array"; break;
    case JC_FUNCTION: t = "Function"; break;
    case JC_ERROR: t = "Error"; break;
    case JC_DATE: t = "Date"; break;
    case JC_NUMBER: t = "Number"; break;
    case JC_STRING: t = "String"; break;
    case JC_BOOLEAN: t = "Boolean"; break;
    case JC_MAP: t = "Map"; break;
    case JC_SET: t = "Set"; break;
    case JC_REGEXP: t = "RegExp"; break;
    case JC_HOST: t = thisv.u.o->hclass && thisv.u.o->hclass->name ? thisv.u.o->hclass->name : "Object"; break;
    default: break;
    }
    char b[64];
    snprintf(b, sizeof(b), "[object %s]", t);
    return js_cstr(J, b);
}
NATIVE(Object_valueOf) { return thisv; }
NATIVE(Object_get_proto)
{
    if (thisv.t != JS_OBJ)
        return JS_NULLV;
    return thisv.u.o->proto ? js_objv(thisv.u.o->proto) : JS_NULLV;
}
NATIVE(Object_set_proto)
{
    if (thisv.t == JS_OBJ && (ARG(0).t == JS_OBJ || ARG(0).t == JS_NULL))
        thisv.u.o->proto = ARG(0).t == JS_OBJ ? ARG(0).u.o : NULL;
    return JS_UNDEFINED;
}

/* ---- Function ---------------------------------------------------------------------------------------------------- */

NATIVE(Function_ctor)
{
    /* new Function("a", "b", "return a + b"): compiled in the global scope */
    struct sbuf sb = { 0 };
    sb_addz(&sb, "(function anonymous(");
    for (int i = 0; i + 1 < argc; i++) {
        jsstr *p = to_jsstr(J, argv[i]);
        if (!p) { sb_free(&sb); return JS_UNDEFINED; }
        if (i) sb_putc(&sb, ',');
        sb_add(&sb, p->s, p->len);
    }
    sb_addz(&sb, "\n) {\n");
    if (argc > 0) {
        jsstr *body = to_jsstr(J, argv[argc - 1]);
        if (!body) { sb_free(&sb); return JS_UNDEFINED; }
        sb_add(&sb, body->s, body->len);
    }
    sb_addz(&sb, "\n})");
    jsval fn;
    char err[300];
    bool ok = js_eval_value(J, sb.p, sb.n, &fn, err, sizeof(err));
    sb_free(&sb);
    if (!ok)
        return js_throw_error(J, "SyntaxError", "%s", err);
    return fn;
}
NATIVE(global_eval)
{
    if (ARG(0).t != JS_STR)
        return ARG(0);
    jsval v;
    char err[300];
    if (!js_eval_value(J, ARG(0).u.s->s, ARG(0).u.s->len, &v, err, sizeof(err)))
        return js_throw_error(J, strncmp(err, "SyntaxError", 11) ? "Error" : "SyntaxError", "%s", err);
    return v;
}
NATIVE(Function_call)
{
    if (!js_is_function(thisv))
        return throw_type(J, "Function.prototype.call called on non-function");
    return call_function(J, thisv.u.o, ARG(0), argc > 1 ? argc - 1 : 0, argv + 1, JS_UNDEFINED);
}
static jsval apply_args(js *J, jsval arr, jsval **out, int *n)
{
    *out = NULL;
    *n = 0;
    if (arr.t == JS_UNDEF || arr.t == JS_NULL)
        return JS_UNDEFINED;
    jsobj *a = this_array(J, arr);
    THROW_IF(J);
    *out = a->items;
    *n = (int)a->len;
    return JS_UNDEFINED;
}
NATIVE(Function_apply)
{
    if (!js_is_function(thisv))
        return throw_type(J, "Function.prototype.apply called on non-function");
    jsval *args;
    int n;
    apply_args(J, ARG(1), &args, &n);
    THROW_IF(J);
    jsval *copy = malloc(sizeof(jsval) * (size_t)(n + 1));
    if (n > 0)
        memcpy(copy, args, sizeof(jsval) * (size_t)n);
    jsval r = call_function(J, thisv.u.o, ARG(0), n, copy, JS_UNDEFINED);
    free(copy);
    return r;
}
struct bound { jsobj *fn; jsval this_; int n; jsval args[]; };
NATIVE(bound_call)
{
    struct bound *b = js_fn_data(callee);
    int total = b->n + argc;
    jsval *all = malloc(sizeof(jsval) * (size_t)(total + 1));
    if (b->n > 0)
        memcpy(all, b->args, sizeof(jsval) * (size_t)b->n);
    if (argc > 0)
        memcpy(all + b->n, argv, sizeof(jsval) * (size_t)argc);
    jsval r;
    if (J->newtarget.t != JS_UNDEF)
        r = construct(J, js_objv(b->fn), total, all);
    else
        r = call_function(J, b->fn, b->this_, total, all, JS_UNDEFINED);
    free(all);
    return r;
}
NATIVE(Function_bind)
{
    if (!js_is_function(thisv))
        return throw_type(J, "Bind must be called on a function");
    int n = argc > 1 ? argc - 1 : 0;
    struct bound *b = js_alloc(J, sizeof(*b) + sizeof(jsval) * (size_t)(n + 1));
    b->fn = thisv.u.o;
    b->this_ = ARG(0);
    b->n = n;
    for (int i = 0; i < n; i++)
        b->args[i] = argv[i + 1];
    char nm[100];
    jsval on = get_prop(J, thisv, J->k_name);
    snprintf(nm, sizeof(nm), "bound %s", on.t == JS_STR ? on.u.s->s : "");
    jsobj *f = make_native(J, js_intern(J, nm, strlen(nm)), bound_call, 0, b);
    f->fn->is_ctor_native = 1;
    return js_objv(f);
}
NATIVE(Function_toString)
{
    if (!js_is_function(thisv))
        return throw_type(J, "Function.prototype.toString requires a function");
    return fn_to_string(J, thisv.u.o);
}

/* ---- Array -------------------------------------------------------------------------------------------------------- */

static jsobj *self_array(js *J, jsval thisv)
{
    if (thisv.t == JS_OBJ && thisv.u.o->cls == JC_ARRAY)
        return thisv.u.o;
    throw_type(J, "Array method called on a non-array");
    return NULL;
}

NATIVE(Array_ctor)
{
    jsobj *a;
    if (J->newtarget.t != JS_UNDEF && thisv.t == JS_OBJ) {
        a = thisv.u.o;
        a->cls = JC_ARRAY;
    } else {
        a = js_array_new(J);
    }
    if (argc == 1 && argv[0].t == JS_NUM) {
        double n = argv[0].u.n;
        if (n < 0 || n != floor(n) || n > 4294967295.0)
            return js_throw_error(J, "RangeError", "Invalid array length");
        array_set_len(J, a, (uint32_t)n);
        THROW_IF(J);
    } else {
        for (int i = 0; i < argc; i++)
            js_array_push(J, a, argv[i]);
    }
    return js_objv(a);
}
NATIVE(Array_isArray) { return js_bool(js_is_array(ARG(0))); }
NATIVE(Array_of)
{
    jsobj *a = js_array_new(J);
    for (int i = 0; i < argc; i++)
        js_array_push(J, a, argv[i]);
    return js_objv(a);
}
NATIVE(Array_from)
{
    jsval src = ARG(0), fn = ARG(1);
    jsobj *a = js_array_new(J);
    if (src.t == JS_UNDEF || src.t == JS_NULL)
        return throw_type(J, "undefined is not iterable");
    jsobj *base;
    if (src.t == JS_OBJ && (src.u.o->cls == JC_SET)) {
        base = js_array_new(J);
        for (uint32_t i = 0; i < src.u.o->len; i++)
            js_array_push(J, base, src.u.o->items[i]);
    } else if (src.t == JS_OBJ && src.u.o->cls == JC_MAP) {
        base = js_array_new(J);
        for (uint32_t i = 0; i < src.u.o->len; i++) {
            jsobj *pair = js_array_new(J);
            js_array_push(J, pair, src.u.o->items[i]);
            js_array_push(J, pair, src.u.o->items2[i]);
            js_array_push(J, base, js_objv(pair));
        }
    } else if (src.t == JS_STR) {
        base = js_array_new(J);
        for (uint32_t i = 0; i < src.u.s->len;) {
            uint8_t c = (uint8_t)src.u.s->s[i];
            uint32_t l = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
            if (i + l > src.u.s->len) l = src.u.s->len - i;
            js_array_push(J, base, js_strn(J, src.u.s->s + i, l));
            i += l;
        }
    } else if (src.t == JS_OBJ && src.u.o->cls != JC_ARRAY && !js_has_iterator(J, src) && get_prop(J, src, J->k_length).t != JS_NUM) {
        /* {length: n} style objects and plain objects without length: empty */
        base = js_array_new(J);
    } else {
        base = this_array(J, src);
    }
    THROW_IF(J);
    for (uint32_t i = 0; i < base->len && !J->thrown; i++) {
        jsval v = base->items[i];
        if (js_is_function(fn)) {
            jsval args[2] = { v, js_num(i) };
            v = call_function(J, fn.u.o, JS_UNDEFINED, 2, args, JS_UNDEFINED);
            THROW_IF(J);
        }
        js_array_push(J, a, v);
    }
    return js_objv(a);
}
NATIVE(Array_push_impl)
{
    jsobj *a = self_array(J, thisv);
    THROW_IF(J);
    if (a->flags & OF_FROZEN)
        return throw_type(J, "Cannot add property, object is not extensible");
    for (int i = 0; i < argc; i++)
        js_array_push(J, a, argv[i]);
    return js_num(a->len);
}
NATIVE(Array_pop_impl)
{
    jsobj *a = self_array(J, thisv);
    THROW_IF(J);
    if (!a->len || (a->flags & OF_FROZEN))
        return JS_UNDEFINED;
    return a->items[--a->len];
}
NATIVE(Array_shift_impl)
{
    jsobj *a = self_array(J, thisv);
    THROW_IF(J);
    if (!a->len || (a->flags & OF_FROZEN))
        return JS_UNDEFINED;
    jsval r = a->items[0];
    memmove(a->items, a->items + 1, (a->len - 1) * sizeof(jsval));
    a->len--;
    return r;
}
NATIVE(Array_unshift_impl)
{
    jsobj *a = self_array(J, thisv);
    THROW_IF(J);
    if (a->flags & OF_FROZEN)
        return throw_type(J, "Cannot add property, object is not extensible");
    uint32_t old = a->len;
    array_set_len(J, a, old + (uint32_t)argc);
    THROW_IF(J);
    memmove(a->items + argc, a->items, old * sizeof(jsval));
    for (int i = 0; i < argc; i++)
        a->items[i] = argv[i];
    return js_num(a->len);
}
static double rel_index(js *J, jsval v, double len, double dflt)
{
    double d = arg_int(J, v, dflt);
    if (d < 0)
        d = fmax(len + d, 0);
    return fmin(d, len);
}
NATIVE(Array_slice)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    double s = rel_index(J, ARG(0), a->len, 0), e = rel_index(J, ARG(1), a->len, a->len);
    jsobj *r = js_array_new(J);
    for (double i = s; i < e; i++)
        js_array_push(J, r, a->items[(uint32_t)i]);
    return js_objv(r);
}
NATIVE(Array_splice_impl)
{
    jsobj *a = self_array(J, thisv);
    THROW_IF(J);
    if (a->flags & OF_FROZEN)
        return throw_type(J, "Cannot modify a frozen array");
    double len = a->len;
    double start = rel_index(J, ARG(0), len, 0);
    double del = argc == 0 ? 0 : argc == 1 ? len - start : fmin(fmax(arg_int(J, ARG(1), 0), 0), len - start);
    int ins = argc > 2 ? argc - 2 : 0;
    jsobj *removed = js_array_new(J);
    for (uint32_t i = 0; i < (uint32_t)del; i++)
        js_array_push(J, removed, a->items[(uint32_t)start + i]);
    uint32_t tail = (uint32_t)(len - start - del);
    uint32_t nlen = (uint32_t)(len - del) + (uint32_t)ins;
    if (nlen > a->len)
        array_set_len(J, a, nlen);
    THROW_IF(J);
    memmove(a->items + (uint32_t)start + ins, a->items + (uint32_t)(start + del), tail * sizeof(jsval));
    for (int i = 0; i < ins; i++)
        a->items[(uint32_t)start + (uint32_t)i] = argv[2 + i];
    a->len = nlen;
    return js_objv(removed);
}
NATIVE(Array_concat)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    jsobj *r = js_array_new(J);
    for (uint32_t i = 0; i < a->len; i++)
        js_array_push(J, r, a->items[i]);
    for (int k = 0; k < argc; k++) {
        if (js_is_array(argv[k])) {
            for (uint32_t i = 0; i < argv[k].u.o->len; i++)
                js_array_push(J, r, argv[k].u.o->items[i]);
        } else {
            js_array_push(J, r, argv[k]);
        }
    }
    return js_objv(r);
}
static jsval join_array(js *J, jsobj *a, const char *sep, size_t seplen, int depth)
{
    struct sbuf sb = { 0 };
    if (depth > 20)
        return js_cstr(J, "");
    for (uint32_t i = 0; i < a->len; i++) {
        if (i)
            sb_add(&sb, sep, seplen);
        jsval v = a->items[i];
        if (v.t == JS_UNDEF || v.t == JS_NULL)
            continue;
        jsstr *s = to_jsstr(J, v);
        if (!s) {
            sb_free(&sb);
            return JS_UNDEFINED;
        }
        sb_add(&sb, s->s, s->len);
    }
    return sb_to_val(J, &sb);
}
NATIVE(Array_join)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    if (ARG(0).t == JS_UNDEF)
        return join_array(J, a, ",", 1, 0);
    jsstr *sep = to_jsstr(J, ARG(0));
    THROW_IF(J);
    return join_array(J, a, sep->s, sep->len, 0);
}
NATIVE(Array_toString)
{
    if (thisv.t != JS_OBJ || thisv.u.o->cls != JC_ARRAY)
        return js_cstr(J, "[object Object]");
    return join_array(J, thisv.u.o, ",", 1, 0);
}
NATIVE(Array_reverse_impl)
{
    jsobj *a = self_array(J, thisv);
    THROW_IF(J);
    for (uint32_t i = 0, j = a->len; i + 1 < j; i++, j--) {
        jsval t = a->items[i];
        a->items[i] = a->items[j - 1];
        a->items[j - 1] = t;
    }
    return thisv;
}
NATIVE(Array_indexOf)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    double from = rel_index(J, ARG(1), a->len, 0);
    for (uint32_t i = (uint32_t)from; i < a->len; i++)
        if (js_strict_equals(a->items[i], ARG(0)))
            return js_num(i);
    return js_num(-1);
}
NATIVE(Array_lastIndexOf)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    for (uint32_t i = a->len; i-- > 0;)
        if (js_strict_equals(a->items[i], ARG(0)))
            return js_num(i);
    return js_num(-1);
}
NATIVE(Array_includes)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    for (uint32_t i = 0; i < a->len; i++)
        if (same_value_zero(a->items[i], ARG(0)))
            return js_bool(true);
    return js_bool(false);
}

/* iteration helpers: callback(value, index, array) */
static jsval cb_call(js *J, jsval fn, jsval thisarg, jsval v, uint32_t i, jsval arr)
{
    jsval args[3] = { v, js_num(i), arr };
    return call_function(J, fn.u.o, thisarg, 3, args, JS_UNDEFINED);
}
#define NEED_FN() do { if (!js_is_function(ARG(0))) return throw_type(J, "callback is not a function"); } while (0)

NATIVE(Array_forEach)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    NEED_FN();
    for (uint32_t i = 0; i < a->len; i++) {
        cb_call(J, ARG(0), ARG(1), a->items[i], i, js_objv(a));
        THROW_IF(J);
    }
    return JS_UNDEFINED;
}
NATIVE(Array_map)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    NEED_FN();
    jsobj *r = js_array_new(J);
    for (uint32_t i = 0; i < a->len; i++) {
        jsval v = cb_call(J, ARG(0), ARG(1), a->items[i], i, js_objv(a));
        THROW_IF(J);
        js_array_push(J, r, v);
    }
    return js_objv(r);
}
NATIVE(Array_filter)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    NEED_FN();
    jsobj *r = js_array_new(J);
    for (uint32_t i = 0; i < a->len; i++) {
        jsval item = a->items[i];
        jsval v = cb_call(J, ARG(0), ARG(1), item, i, js_objv(a));
        THROW_IF(J);
        if (js_truthy(v))
            js_array_push(J, r, item);
    }
    return js_objv(r);
}
static jsval reduce_impl(js *J, jsval thisv, int argc, jsval *argv, bool right)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    NEED_FN();
    int64_t n = a->len, i = right ? n - 1 : 0, step = right ? -1 : 1;
    jsval acc;
    if (argc >= 2) {
        acc = argv[1];
    } else {
        if (n == 0)
            return throw_type(J, "Reduce of empty array with no initial value");
        acc = a->items[i];
        i += step;
    }
    for (; i >= 0 && i < (int64_t)a->len; i += step) {
        jsval args[4] = { acc, a->items[i], js_num((double)i), js_objv(a) };
        acc = call_function(J, ARG(0).u.o, JS_UNDEFINED, 4, args, JS_UNDEFINED);
        THROW_IF(J);
    }
    return acc;
}
NATIVE(Array_reduce) { return reduce_impl(J, thisv, argc, argv, false); }
NATIVE(Array_reduceRight) { return reduce_impl(J, thisv, argc, argv, true); }
NATIVE(Array_some)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    NEED_FN();
    for (uint32_t i = 0; i < a->len; i++) {
        jsval v = cb_call(J, ARG(0), ARG(1), a->items[i], i, js_objv(a));
        THROW_IF(J);
        if (js_truthy(v))
            return js_bool(true);
    }
    return js_bool(false);
}
NATIVE(Array_every)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    NEED_FN();
    for (uint32_t i = 0; i < a->len; i++) {
        jsval v = cb_call(J, ARG(0), ARG(1), a->items[i], i, js_objv(a));
        THROW_IF(J);
        if (!js_truthy(v))
            return js_bool(false);
    }
    return js_bool(true);
}
static jsval find_impl(js *J, jsval thisv, int argc, jsval *argv, bool last, bool want_index)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    NEED_FN();
    for (int64_t k = 0; k < (int64_t)a->len; k++) {
        int64_t i = last ? (int64_t)a->len - 1 - k : k;
        if (i < 0 || i >= (int64_t)a->len)
            break;
        jsval item = a->items[i];
        jsval v = cb_call(J, ARG(0), ARG(1), item, (uint32_t)i, js_objv(a));
        THROW_IF(J);
        if (js_truthy(v))
            return want_index ? js_num((double)i) : item;
    }
    return want_index ? js_num(-1) : JS_UNDEFINED;
}
NATIVE(Array_find) { return find_impl(J, thisv, argc, argv, false, false); }
NATIVE(Array_findIndex) { return find_impl(J, thisv, argc, argv, false, true); }
NATIVE(Array_findLast) { return find_impl(J, thisv, argc, argv, true, false); }
NATIVE(Array_findLastIndex) { return find_impl(J, thisv, argc, argv, true, true); }
NATIVE(Array_fill_impl)
{
    jsobj *a = self_array(J, thisv);
    THROW_IF(J);
    double s = rel_index(J, ARG(1), a->len, 0), e = rel_index(J, ARG(2), a->len, a->len);
    for (double i = s; i < e; i++)
        a->items[(uint32_t)i] = ARG(0);
    return thisv;
}
NATIVE(Array_at)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    double i = arg_int(J, ARG(0), 0);
    if (i < 0)
        i += a->len;
    return i >= 0 && i < a->len ? a->items[(uint32_t)i] : JS_UNDEFINED;
}
static void flatten(js *J, jsobj *out, jsobj *a, double depth)
{
    for (uint32_t i = 0; i < a->len; i++) {
        if (js_is_array(a->items[i]) && depth >= 1)
            flatten(J, out, a->items[i].u.o, depth - 1);
        else
            js_array_push(J, out, a->items[i]);
    }
}
NATIVE(Array_flat)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    jsobj *r = js_array_new(J);
    flatten(J, r, a, arg_int(J, ARG(0), 1));
    return js_objv(r);
}
NATIVE(Array_flatMap)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    NEED_FN();
    jsobj *r = js_array_new(J);
    for (uint32_t i = 0; i < a->len; i++) {
        jsval v = cb_call(J, ARG(0), ARG(1), a->items[i], i, js_objv(a));
        THROW_IF(J);
        if (js_is_array(v))
            flatten(J, r, v.u.o, 0);
        else
            js_array_push(J, r, v);
    }
    return js_objv(r);
}
NATIVE(Array_keys)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    jsobj *r = js_array_new(J);
    for (uint32_t i = 0; i < a->len; i++)
        js_array_push(J, r, js_num(i));
    return js_objv(r);
}
NATIVE(Array_values)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    jsobj *r = js_array_new(J);
    for (uint32_t i = 0; i < a->len; i++)
        js_array_push(J, r, a->items[i]);
    return js_objv(r);
}
NATIVE(Array_entries)
{
    jsobj *a = this_array(J, thisv);
    THROW_IF(J);
    jsobj *r = js_array_new(J);
    for (uint32_t i = 0; i < a->len; i++) {
        jsobj *pair = js_array_new(J);
        js_array_push(J, pair, js_num(i));
        js_array_push(J, pair, a->items[i]);
        js_array_push(J, r, js_objv(pair));
    }
    return js_objv(r);
}

struct sortctx { js *J; jsval cmp; bool failed; };
static int sort_cmp(struct sortctx *c, jsval a, jsval b)
{
    js *J = c->J;
    if (c->failed)
        return 0;
    if (a.t == JS_UNDEF || b.t == JS_UNDEF)
        return a.t == JS_UNDEF ? (b.t == JS_UNDEF ? 0 : 1) : -1;
    if (js_is_function(c->cmp)) {
        jsval args[2] = { a, b };
        jsval r = call_function(J, c->cmp.u.o, JS_UNDEFINED, 2, args, JS_UNDEFINED);
        if (J->thrown) { c->failed = true; return 0; }
        double d = to_number(J, r);
        return d < 0 ? -1 : d > 0 ? 1 : 0;
    }
    jsstr *sa = to_jsstr(J, a), *sb = to_jsstr(J, b);
    if (!sa || !sb) { c->failed = true; return 0; }
    size_t l = sa->len < sb->len ? sa->len : sb->len;
    int r = memcmp(sa->s, sb->s, l);
    return r ? r : (sa->len < sb->len ? -1 : sa->len > sb->len ? 1 : 0);
}
static void merge_sort(struct sortctx *c, jsval *v, jsval *tmp, uint32_t n)
{
    if (n < 2 || c->failed)
        return;
    uint32_t m = n / 2;
    merge_sort(c, v, tmp, m);
    merge_sort(c, v + m, tmp, n - m);
    uint32_t i = 0, j = m, k = 0;
    while (i < m && j < n) {
        if (sort_cmp(c, v[j], v[i]) < 0)
            tmp[k++] = v[j++];
        else
            tmp[k++] = v[i++];
    }
    while (i < m) tmp[k++] = v[i++];
    while (j < n) tmp[k++] = v[j++];
    memcpy(v, tmp, n * sizeof(jsval));
}
NATIVE(Array_sort_impl)
{
    jsobj *a = self_array(J, thisv);
    THROW_IF(J);
    if (ARG(0).t != JS_UNDEF && !js_is_function(ARG(0)))
        return throw_type(J, "The comparison function must be either a function or undefined");
    if (a->flags & OF_FROZEN)
        return throw_type(J, "Cannot sort a frozen array");
    struct sortctx c = { J, ARG(0), false };
    jsval *tmp = malloc(sizeof(jsval) * (a->len + 1));
    merge_sort(&c, a->items, tmp, a->len);
    free(tmp);
    THROW_IF(J);
    return thisv;
}

/* Mutating methods on array-likes (jQuery objects, `arguments`-like objects): run on a snapshot, then write it back. */
static jsval generic_mutator(js *J, jsval thisv, int argc, jsval *argv, jsobj *callee, js_native impl)
{
    if (thisv.t != JS_OBJ)
        return throw_type(J, "Array method called on a non-object");
    jsobj *snap = this_array(J, thisv);
    THROW_IF(J);
    uint32_t old_len = snap->len;
    jsobj *copy = js_array_new(J);
    for (uint32_t i = 0; i < snap->len; i++)
        js_array_push(J, copy, snap->items[i]);
    jsval r = impl(J, js_objv(copy), argc, argv, callee);
    THROW_IF(J);
    for (uint32_t i = 0; i < copy->len; i++) {
        set_prop(J, thisv, js_intern_str(J, num_to_str(J, (double)i)), copy->items[i]);
        THROW_IF(J);
    }
    for (uint32_t i = copy->len; i < old_len; i++)
        obj_delete(thisv.u.o, js_intern_str(J, num_to_str(J, (double)i)));
    set_prop(J, thisv, J->k_length, js_num(copy->len));
    if (r.t == JS_OBJ && r.u.o == copy)
        return thisv;
    return r;
}
#define GENERIC(name) NATIVE(Array_##name) { \
    if (thisv.t == JS_OBJ && thisv.u.o->cls == JC_ARRAY) return Array_##name##_impl(J, thisv, argc, argv, callee); \
    return generic_mutator(J, thisv, argc, argv, callee, Array_##name##_impl); }
GENERIC(push) GENERIC(pop) GENERIC(shift) GENERIC(unshift) GENERIC(splice) GENERIC(reverse) GENERIC(fill) GENERIC(sort)

/* ---- Error --------------------------------------------------------------------------------------------------- */

static jsval error_ctor_impl(js *J, jsval thisv, int argc, jsval *argv, jsobj *callee, int type)
{
    jsobj *e;
    if (J->newtarget.t != JS_UNDEF && thisv.t == JS_OBJ) {
        e = thisv.u.o;
        e->cls = JC_ERROR;
    } else {
        e = obj_new(J, JC_ERROR, J->err_protos[type]);
    }
    (void)callee;
    if (argc > 0 && argv[0].t != JS_UNDEF) {
        jsstr *m = to_jsstr(J, argv[0]);
        THROW_IF(J);
        obj_add_prop(J, e, J->k_message, js_strv(m), PF_WRITE);
    }
    if (argc > 1 && argv[1].t == JS_OBJ) {
        jsval c = get_prop(J, argv[1], js_intern(J, "cause", 5));
        if (c.t != JS_UNDEF)
            obj_add_prop(J, e, js_intern(J, "cause", 5), c, PF_WRITE);
    }
    jsval nm = get_prop(J, js_objv(e), J->k_name), msg = get_prop(J, js_objv(e), J->k_message);
    char st[400];
    snprintf(st, sizeof(st), "%s%s%s\n    at <anonymous>", nm.t == JS_STR ? nm.u.s->s : "Error", msg.t == JS_STR && msg.u.s->len ? ": " : "",
             msg.t == JS_STR ? msg.u.s->s : "");
    obj_add_prop(J, e, J->k_stack, js_cstr(J, st), PF_WRITE);
    return js_objv(e);
}
#define ERRCTOR(fn, type) NATIVE(fn) { return error_ctor_impl(J, thisv, argc, argv, callee, type); }
ERRCTOR(Error_ctor, ERR_ERROR) ERRCTOR(TypeError_ctor, ERR_TYPE) ERRCTOR(RangeError_ctor, ERR_RANGE)
ERRCTOR(SyntaxError_ctor, ERR_SYNTAX) ERRCTOR(ReferenceError_ctor, ERR_REFERENCE) ERRCTOR(EvalError_ctor, ERR_EVAL)
ERRCTOR(URIError_ctor, ERR_URI)
NATIVE(Error_toString)
{
    if (thisv.t != JS_OBJ)
        return throw_type(J, "Error.prototype.toString called on non-object");
    jsval n = get_prop(J, thisv, J->k_name), m = get_prop(J, thisv, J->k_message);
    THROW_IF(J);
    jsstr *ns = n.t == JS_UNDEF ? js_intern(J, "Error", 5) : to_jsstr(J, n);
    jsstr *ms = m.t == JS_UNDEF ? J->k_empty : to_jsstr(J, m);
    THROW_IF(J);
    if (!ms->len)
        return js_strv(ns);
    if (!ns->len)
        return js_strv(ms);
    jsstr *r = str_concat(J, str_concat(J, ns, js_intern(J, ": ", 2)), ms);
    return js_strv(r);
}
NATIVE(Error_captureStackTrace) { return JS_UNDEFINED; }

/* ---- Boolean / Number ------------------------------------------------------------------------------------------ */

NATIVE(Boolean_ctor)
{
    jsval b = js_bool(js_truthy(ARG(0)));
    if (J->newtarget.t != JS_UNDEF && thisv.t == JS_OBJ) {
        thisv.u.o->cls = JC_BOOLEAN;
        thisv.u.o->prim = b;
        return thisv;
    }
    return b;
}
static jsval this_prim(js *J, jsval thisv, int t, uint8_t cls, const char *what)
{
    if (thisv.t == t)
        return thisv;
    if (thisv.t == JS_OBJ && thisv.u.o->cls == cls)
        return thisv.u.o->prim;
    return throw_type(J, "%s requires a %s", what, what);
}
NATIVE(Boolean_toString)
{
    jsval b = this_prim(J, thisv, JS_BOOL, JC_BOOLEAN, "Boolean.prototype.toString");
    THROW_IF(J);
    return js_cstr(J, b.u.b ? "true" : "false");
}
NATIVE(Boolean_valueOf) { return this_prim(J, thisv, JS_BOOL, JC_BOOLEAN, "Boolean.prototype.valueOf"); }

NATIVE(Number_ctor)
{
    double d = argc ? to_number(J, argv[0]) : 0;
    THROW_IF(J);
    if (J->newtarget.t != JS_UNDEF && thisv.t == JS_OBJ) {
        thisv.u.o->cls = JC_NUMBER;
        thisv.u.o->prim = js_num(d);
        return thisv;
    }
    return js_num(d);
}
NATIVE(Number_isInteger) { return js_bool(ARG(0).t == JS_NUM && isfinite(ARG(0).u.n) && ARG(0).u.n == floor(ARG(0).u.n)); }
NATIVE(Number_isSafeInteger)
{
    return js_bool(ARG(0).t == JS_NUM && isfinite(ARG(0).u.n) && ARG(0).u.n == floor(ARG(0).u.n) &&
                   fabs(ARG(0).u.n) <= 9007199254740991.0);
}
NATIVE(Number_isFinite) { return js_bool(ARG(0).t == JS_NUM && isfinite(ARG(0).u.n)); }
NATIVE(Number_isNaN) { return js_bool(ARG(0).t == JS_NUM && isnan(ARG(0).u.n)); }
NATIVE(global_isNaN) { double d = to_number(J, ARG(0)); THROW_IF(J); return js_bool(isnan(d)); }
NATIVE(global_isFinite) { double d = to_number(J, ARG(0)); THROW_IF(J); return js_bool(isfinite(d)); }
NATIVE(global_parseFloat)
{
    jsstr *s = to_jsstr(J, ARG(0));
    THROW_IF(J);
    const char *p = s->s;
    while (isspace((unsigned char)*p)) p++;
    if (!strncmp(p, "Infinity", 8) || !strncmp(p, "+Infinity", 9))
        return js_num(INFINITY);
    if (!strncmp(p, "-Infinity", 9))
        return js_num(-INFINITY);
    const char *q = p;
    if (*q == '+' || *q == '-') q++;
    if (!isdigit((unsigned char)*q) && !(*q == '.' && isdigit((unsigned char)q[1])))
        return js_num(NAN);
    char *e;
    double d = strtod(p, &e);
    /* strtod accepts hex floats and "inf"; the checks above keep the JS meaning */
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        return js_num(0);
    return js_num(d);
}
NATIVE(global_parseInt)
{
    jsstr *s = to_jsstr(J, ARG(0));
    THROW_IF(J);
    double radix = ARG(1).t == JS_UNDEF ? 0 : to_number(J, ARG(1));
    int base = (int)to_int32(radix);
    const char *p = s->s;
    while (isspace((unsigned char)*p)) p++;
    int sign = 1;
    if (*p == '-' || *p == '+') {
        sign = *p == '-' ? -1 : 1;
        p++;
    }
    if (base == 0 || base == 16) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
            base = 16;
        }
    }
    if (base == 0)
        base = 10;
    if (base < 2 || base > 36)
        return js_num(NAN);
    double v = 0;
    int digits = 0;
    for (; *p; p++) {
        int d = isdigit((unsigned char)*p) ? *p - '0' : isalpha((unsigned char)*p) ? tolower(*p) - 'a' + 10 : 99;
        if (d >= base)
            break;
        v = v * base + d;
        digits++;
    }
    if (!digits)
        return js_num(NAN);
    return js_num(sign * v);
}
static double this_number(js *J, jsval thisv)
{
    jsval v = this_prim(J, thisv, JS_NUM, JC_NUMBER, "Number.prototype method");
    return J->thrown ? NAN : v.u.n;
}
NATIVE(Number_toString)
{
    double d = this_number(J, thisv);
    THROW_IF(J);
    double radix = arg_int(J, ARG(0), 10);
    if (radix < 2 || radix > 36)
        return js_throw_error(J, "RangeError", "toString() radix must be between 2 and 36");
    if (radix == 10 || !isfinite(d))
        return js_strv(num_to_str(J, d));
    char buf[1100];
    int o = sizeof(buf) - 1;
    buf[o] = 0;
    bool neg = d < 0;
    double ip = floor(fabs(d)), fp = fabs(d) - ip;
    if (ip == 0)
        buf[--o] = '0';
    while (ip > 0 && o > 40) {
        int dg = (int)fmod(ip, radix);
        buf[--o] = (char)(dg < 10 ? '0' + dg : 'a' + dg - 10);
        ip = floor(ip / radix);
    }
    if (neg)
        buf[--o] = '-';
    struct sbuf sb = { 0 };
    sb_addz(&sb, buf + o);
    if (fp > 0) {
        sb_putc(&sb, '.');
        for (int i = 0; i < 20 && fp > 0; i++) {
            fp *= radix;
            int dg = (int)fp;
            sb_putc(&sb, (char)(dg < 10 ? '0' + dg : 'a' + dg - 10));
            fp -= dg;
        }
    }
    return sb_to_val(J, &sb);
}
NATIVE(Number_toFixed)
{
    double d = this_number(J, thisv);
    THROW_IF(J);
    double f = arg_int(J, ARG(0), 0);
    if (f < 0 || f > 100)
        return js_throw_error(J, "RangeError", "toFixed() digits argument must be between 0 and 100");
    if (!isfinite(d) || fabs(d) >= 1e21)
        return js_strv(num_to_str(J, d));
    char buf[200];
    {
        /* JS rounds an exact tie away from zero (printf rounds to even): detect it from the exact decimal expansion */
        char exact[1200];
        snprintf(exact, sizeof(exact), "%.*f", (int)f + 30, d);
        char *dot = strchr(exact, '.');
        if (dot) {
            char *tail = dot + 1 + (int)f;
            if (*tail == '5' && strspn(tail + 1, "0") == strlen(tail + 1))
                d = nextafter(d, d < 0 ? -INFINITY : INFINITY);
        }
    }
    snprintf(buf, sizeof(buf), "%.*f", (int)f, d);
    if (d == 0 || (atof(buf) == 0 && buf[0] == '-'))
        { char *m = strchr(buf, '-'); if (m) memmove(m, m + 1, strlen(m)); }
    return js_cstr(J, buf);
}
NATIVE(Number_toPrecision)
{
    double d = this_number(J, thisv);
    THROW_IF(J);
    if (ARG(0).t == JS_UNDEF || !isfinite(d))
        return js_strv(num_to_str(J, d));
    double p = arg_int(J, ARG(0), 0);
    if (p < 1 || p > 100)
        return js_throw_error(J, "RangeError", "toPrecision() argument must be between 1 and 100");
    char buf[200];
    double ex = d == 0 ? 0 : floor(log10(fabs(d)));
    if (ex < -6 || ex >= p) {
        snprintf(buf, sizeof(buf), "%.*e", (int)p - 1, d);
        char *e = strchr(buf, 'e');
        if (e) {                                            /* e+05 -> e+5 */
            int x = atoi(e + 1);
            snprintf(e, sizeof(buf) - (size_t)(e - buf), "e%c%d", x < 0 ? '-' : '+', abs(x));
        }
    } else {
        snprintf(buf, sizeof(buf), "%.*f", (int)(p - 1 - ex < 0 ? 0 : p - 1 - ex), d);
    }
    return js_cstr(J, buf);
}
NATIVE(Number_toExponential)
{
    double d = this_number(J, thisv);
    THROW_IF(J);
    if (!isfinite(d))
        return js_strv(num_to_str(J, d));
    char buf[200];
    if (ARG(0).t == JS_UNDEF) {
        jsstr *s = num_to_str(J, fabs(d));
        int digits = 0;
        for (uint32_t i = 0; i < s->len && s->s[i] != 'e'; i++)
            if (isdigit((unsigned char)s->s[i])) digits++;
        snprintf(buf, sizeof(buf), "%.*e", digits > 0 ? digits - 1 : 0, d);
    } else {
        snprintf(buf, sizeof(buf), "%.*e", (int)arg_int(J, ARG(0), 0), d);
    }
    char *e = strchr(buf, 'e');
    if (e) {
        int x = atoi(e + 1);
        snprintf(e, sizeof(buf) - (size_t)(e - buf), "e%c%d", x < 0 ? '-' : '+', abs(x));
    }
    return js_cstr(J, buf);
}
NATIVE(Number_valueOf)
{
    double d = this_number(J, thisv);
    THROW_IF(J);
    return js_num(d);
}
NATIVE(Number_toLocaleString)
{
    double d = this_number(J, thisv);
    THROW_IF(J);
    if (!isfinite(d))
        return js_strv(num_to_str(J, d));
    char buf[80];
    snprintf(buf, sizeof(buf), "%.3f", fabs(d));
    char *dot = strchr(buf, '.');
    size_t il = (size_t)(dot - buf);
    char frac[8] = "";
    if (dot) {
        size_t fl = strlen(dot + 1);
        while (fl > 0 && dot[fl] == '0') fl--;
        if (fl > 0) { snprintf(frac, sizeof(frac), ".%.*s", (int)fl, dot + 1); }
    }
    struct sbuf sb = { 0 };
    if (d < 0 && (il > 1 || buf[0] != '0' || frac[0]))
        sb_putc(&sb, '-');
    for (size_t i = 0; i < il; i++) {
        sb_putc(&sb, buf[i]);
        if ((il - i - 1) % 3 == 0 && i + 1 < il)
            sb_putc(&sb, ',');
    }
    sb_addz(&sb, frac);
    return sb_to_val(J, &sb);
}

/* ---- Math ---------------------------------------------------------------------------------------------------- */

#define MATH1(name, expr) NATIVE(Math_##name) { double x = to_number(J, ARG(0)); THROW_IF(J); return js_num(expr); }
MATH1(abs, fabs(x)) MATH1(ceil, ceil(x)) MATH1(floor, floor(x)) MATH1(trunc, trunc(x)) MATH1(sqrt, sqrt(x))
MATH1(cbrt, (isfinite(x) && round(cbrt(x)) * round(cbrt(x)) * round(cbrt(x)) == x) ? round(cbrt(x)) : cbrt(x)) MATH1(exp, exp(x)) MATH1(log, log(x)) MATH1(log2, log2(x)) MATH1(log10, log10(x))
MATH1(sin, sin(x)) MATH1(cos, cos(x)) MATH1(tan, tan(x)) MATH1(asin, asin(x)) MATH1(acos, acos(x))
MATH1(atan, atan(x)) MATH1(sinh, sinh(x)) MATH1(cosh, cosh(x)) MATH1(tanh, tanh(x)) MATH1(log1p, log1p(x))
MATH1(expm1, expm1(x)) MATH1(fround, (double)(float)x)
MATH1(sign, isnan(x) ? NAN : x > 0 ? 1 : x < 0 ? -1 : x)
MATH1(round, isfinite(x) ? floor(x + 0.5) : x)
MATH1(clz32, (double)(to_uint32(x) == 0 ? 32 : __builtin_clz(to_uint32(x))))
NATIVE(Math_atan2) { double y = to_number(J, ARG(0)), x = to_number(J, ARG(1)); THROW_IF(J); return js_num(atan2(y, x)); }
NATIVE(Math_pow) { double x = to_number(J, ARG(0)), y = to_number(J, ARG(1)); THROW_IF(J); return js_num(isnan(y) ? NAN : y == 0 ? 1 : pow(x, y)); }
NATIVE(Math_min)
{
    double r = INFINITY;
    for (int i = 0; i < argc; i++) {
        double d = to_number(J, argv[i]);
        THROW_IF(J);
        if (isnan(d)) return js_num(NAN);
        if (d < r || (d == 0 && r == 0 && signbit(d))) r = d;
    }
    return js_num(r);
}
NATIVE(Math_max)
{
    double r = -INFINITY;
    for (int i = 0; i < argc; i++) {
        double d = to_number(J, argv[i]);
        THROW_IF(J);
        if (isnan(d)) return js_num(NAN);
        if (d > r || (d == 0 && r == 0 && !signbit(d))) r = d;
    }
    return js_num(r);
}
NATIVE(Math_hypot)
{
    double s = 0;
    for (int i = 0; i < argc; i++) {
        double d = to_number(J, argv[i]);
        THROW_IF(J);
        s += d * d;
    }
    return js_num(sqrt(s));
}
static uint64_t rng_state;
NATIVE(Math_random)
{
    if (!rng_state)
        rng_state = (uint64_t)time(NULL) * 6364136223846793005ULL ^ (uint64_t)(uintptr_t)J ^ 0x9e3779b97f4a7c15ULL;
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return js_num((double)(rng_state >> 11) / 9007199254740992.0);
}

/* ---- JSON -------------------------------------------------------------------------------------------------------- */

jsstr *js_json_quote(js *J, jsstr *s)
{
    struct sbuf sb = { 0 };
    sb_putc(&sb, '"');
    for (uint32_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->s[i];
        switch (c) {
        case '"': sb_addz(&sb, "\\\""); break;
        case '\\': sb_addz(&sb, "\\\\"); break;
        case '\n': sb_addz(&sb, "\\n"); break;
        case '\r': sb_addz(&sb, "\\r"); break;
        case '\t': sb_addz(&sb, "\\t"); break;
        case '\b': sb_addz(&sb, "\\b"); break;
        case '\f': sb_addz(&sb, "\\f"); break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", c);
                sb_addz(&sb, b);
            } else {
                sb_putc(&sb, (char)c);
            }
        }
    }
    sb_putc(&sb, '"');
    jsstr *r = js_str_new(J, sb.p, sb.n);
    sb_free(&sb);
    return r;
}

struct jstate { js *J; struct sbuf sb; char indent[32]; int depth; jsval replacer; jsobj *allow; jsobj *stack[64]; int sp; };

static bool json_value(struct jstate *S, jsval holder, jsstr *key, jsval v, int level);

static void json_nl(struct jstate *S, int level)
{
    if (!S->indent[0])
        return;
    sb_putc(&S->sb, '\n');
    for (int i = 0; i < level; i++)
        sb_addz(&S->sb, S->indent);
}

static bool json_value(struct jstate *S, jsval holder, jsstr *key, jsval v, int level)
{
    js *J = S->J;
    if (v.t == JS_OBJ) {
        jsval tj = get_prop(J, v, J->k_toJSON);
        if (J->thrown) return false;
        if (js_is_function(tj)) {
            jsval a[1] = { js_strv(key) };
            v = call_function(J, tj.u.o, v, 1, a, JS_UNDEFINED);
            if (J->thrown) return false;
        }
    }
    if (js_is_function(S->replacer)) {
        jsval a[2] = { js_strv(key), v };
        v = call_function(J, S->replacer.u.o, holder, 2, a, JS_UNDEFINED);
        if (J->thrown) return false;
    }
    if (v.t == JS_OBJ && (v.u.o->cls == JC_NUMBER || v.u.o->cls == JC_STRING || v.u.o->cls == JC_BOOLEAN))
        v = v.u.o->prim;
    switch (v.t) {
    case JS_NULL: sb_addz(&S->sb, "null"); return true;
    case JS_BOOL: sb_addz(&S->sb, v.u.b ? "true" : "false"); return true;
    case JS_NUM:
        if (!isfinite(v.u.n)) sb_addz(&S->sb, "null");
        else sb_addz(&S->sb, num_to_str(J, v.u.n)->s);
        return true;
    case JS_STR: {
        jsstr *q = js_json_quote(J, v.u.s);
        sb_add(&S->sb, q->s, q->len);
        return true;
    }
    case JS_UNDEF:
        return false;
    default:
        break;
    }
    jsobj *o = v.u.o;
    if (o->cls == JC_FUNCTION)
        return false;
    for (int i = 0; i < S->sp; i++)
        if (S->stack[i] == o) {
            throw_type(J, "Converting circular structure to JSON");
            return false;
        }
    if (S->sp >= 60) {
        js_throw_error(J, "RangeError", "JSON nesting too deep");
        return false;
    }
    S->stack[S->sp++] = o;
    if (o->cls == JC_ARRAY) {
        sb_putc(&S->sb, '[');
        for (uint32_t i = 0; i < o->len; i++) {
            if (i) sb_putc(&S->sb, ',');
            json_nl(S, level + 1);
            if (!json_value(S, v, js_intern_str(J, num_to_str(J, (double)i)), o->items[i], level + 1)) {
                if (J->thrown) return false;
                sb_addz(&S->sb, "null");
            }
        }
        if (o->len) json_nl(S, level);
        sb_putc(&S->sb, ']');
    } else {
        sb_putc(&S->sb, '{');
        jsobj *ks = js_array_new(J);
        own_keys(J, o, ks, true);
        bool first = true;
        for (uint32_t i = 0; i < ks->len; i++) {
            jsstr *k = js_intern_str(J, ks->items[i].u.s);
            if (S->allow) {
                bool ok = false;
                for (uint32_t a = 0; a < S->allow->len; a++)
                    if (S->allow->items[a].t == JS_STR && js_intern_str(J, S->allow->items[a].u.s) == k) ok = true;
                if (!ok) continue;
            }
            jsval pv = get_prop(J, v, k);
            if (J->thrown) return false;
            size_t mark = S->sb.n;
            if (!first) sb_putc(&S->sb, ',');
            json_nl(S, level + 1);
            jsstr *q = js_json_quote(J, k);
            sb_add(&S->sb, q->s, q->len);
            sb_putc(&S->sb, ':');
            if (S->indent[0]) sb_putc(&S->sb, ' ');
            if (!json_value(S, v, k, pv, level + 1)) {
                if (J->thrown) return false;
                S->sb.n = mark;                              /* undefined / function: skip the member */
                S->sb.p[mark] = 0;
                continue;
            }
            first = false;
        }
        if (!first) json_nl(S, level);
        sb_putc(&S->sb, '}');
    }
    S->sp--;
    return true;
}

NATIVE(JSON_stringify)
{
    struct jstate S = { .J = J };
    S.replacer = ARG(1);
    if (js_is_array(ARG(1))) {
        S.allow = ARG(1).u.o;
        S.replacer = JS_UNDEFINED;
    }
    jsval sp = ARG(2);
    if (sp.t == JS_OBJ && (sp.u.o->cls == JC_NUMBER || sp.u.o->cls == JC_STRING))
        sp = sp.u.o->prim;
    if (sp.t == JS_NUM) {
        int n = (int)fmin(fmax(sp.u.n, 0), 10);
        for (int i = 0; i < n; i++)
            S.indent[i] = ' ';
    } else if (sp.t == JS_STR) {
        snprintf(S.indent, sizeof(S.indent), "%.10s", sp.u.s->s);
    }
    jsobj *holder = js_object_new(J);
    set_prop(J, js_objv(holder), J->k_empty, ARG(0));
    bool ok = json_value(&S, js_objv(holder), J->k_empty, ARG(0), 0);
    if (J->thrown) {
        sb_free(&S.sb);
        return JS_UNDEFINED;
    }
    if (!ok) {
        sb_free(&S.sb);
        return JS_UNDEFINED;
    }
    return sb_to_val(J, &S.sb);
}

struct jparse { js *J; const char *p, *end; int depth; };
static jsval jp_value(struct jparse *P);
static void jp_ws(struct jparse *P) { while (P->p < P->end && (*P->p == ' ' || *P->p == '\t' || *P->p == '\n' || *P->p == '\r')) P->p++; }
static jsval jp_err(struct jparse *P)
{
    if (P->p >= P->end)
        return js_throw_error(P->J, "SyntaxError", "Unexpected end of JSON input");
    return js_throw_error(P->J, "SyntaxError", "Unexpected token '%c' in JSON", *P->p);
}
static bool jp_string(struct jparse *P, jsstr **out)
{
    P->p++;
    struct sbuf sb = { 0 };
    while (P->p < P->end && *P->p != '"') {
        if ((unsigned char)*P->p < 0x20) { sb_free(&sb); jp_err(P); return false; }
        if (*P->p == '\\') {
            P->p++;
            if (P->p >= P->end) { sb_free(&sb); jp_err(P); return false; }
            char c = *P->p++;
            switch (c) {
            case 'n': sb_putc(&sb, '\n'); break;
            case 't': sb_putc(&sb, '\t'); break;
            case 'r': sb_putc(&sb, '\r'); break;
            case 'b': sb_putc(&sb, '\b'); break;
            case 'f': sb_putc(&sb, '\f'); break;
            case '/': case '\\': case '"': sb_putc(&sb, c); break;
            case 'u': {
                uint32_t cp = 0;
                for (int i = 0; i < 4; i++) {
                    if (P->p >= P->end || !isxdigit((unsigned char)*P->p)) { sb_free(&sb); jp_err(P); return false; }
                    cp = cp * 16 + (uint32_t)(isdigit((unsigned char)*P->p) ? *P->p - '0' : tolower(*P->p) - 'a' + 10);
                    P->p++;
                }
                if (cp >= 0xd800 && cp < 0xdc00 && P->p + 6 <= P->end && P->p[0] == '\\' && P->p[1] == 'u') {
                    uint32_t lo = 0;
                    bool ok = true;
                    for (int i = 0; i < 4; i++) {
                        if (!isxdigit((unsigned char)P->p[2 + i])) { ok = false; break; }
                        lo = lo * 16 + (uint32_t)(isdigit((unsigned char)P->p[2 + i]) ? P->p[2 + i] - '0' : tolower(P->p[2 + i]) - 'a' + 10);
                    }
                    if (ok && lo >= 0xdc00 && lo < 0xe000) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                        P->p += 6;
                    }
                }
                char b[4];
                int n;
                if (cp < 0x80) { b[0] = (char)cp; n = 1; }
                else if (cp < 0x800) { b[0] = (char)(0xc0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); n = 2; }
                else if (cp < 0x10000) { b[0] = (char)(0xe0 | cp >> 12); b[1] = (char)(0x80 | ((cp >> 6) & 63)); b[2] = (char)(0x80 | (cp & 63)); n = 3; }
                else { b[0] = (char)(0xf0 | cp >> 18); b[1] = (char)(0x80 | ((cp >> 12) & 63)); b[2] = (char)(0x80 | ((cp >> 6) & 63)); b[3] = (char)(0x80 | (cp & 63)); n = 4; }
                sb_add(&sb, b, (size_t)n);
                break;
            }
            default: sb_free(&sb); P->p--; jp_err(P); return false;
            }
        } else {
            sb_putc(&sb, *P->p++);
        }
    }
    if (P->p >= P->end) { sb_free(&sb); jp_err(P); return false; }
    P->p++;
    *out = js_str_new(P->J, sb.p ? sb.p : "", sb.n);
    sb_free(&sb);
    return true;
}
static jsval jp_value(struct jparse *P)
{
    js *J = P->J;
    jp_ws(P);
    if (P->p >= P->end)
        return jp_err(P);
    if (++P->depth > 100)
        return js_throw_error(J, "RangeError", "JSON nesting too deep");
    jsval r = JS_UNDEFINED;
    char c = *P->p;
    if (c == '{') {
        P->p++;
        jsobj *o = js_object_new(J);
        jp_ws(P);
        if (P->p < P->end && *P->p == '}') {
            P->p++;
        } else {
            for (;;) {
                jp_ws(P);
                if (P->p >= P->end || *P->p != '"') { jp_err(P); P->depth--; return JS_UNDEFINED; }
                jsstr *k;
                if (!jp_string(P, &k)) { P->depth--; return JS_UNDEFINED; }
                jp_ws(P);
                if (P->p >= P->end || *P->p != ':') { jp_err(P); P->depth--; return JS_UNDEFINED; }
                P->p++;
                jsval v = jp_value(P);
                if (J->thrown) { P->depth--; return JS_UNDEFINED; }
                jsstr *ik = js_intern_str(J, k);
                struct prop *ex = obj_find_own(o, ik);
                if (ex) ex->v = v; else obj_add_prop(J, o, ik, v, PF_ENUM | PF_WRITE);
                jp_ws(P);
                if (P->p < P->end && *P->p == ',') { P->p++; continue; }
                if (P->p < P->end && *P->p == '}') { P->p++; break; }
                jp_err(P);
                P->depth--;
                return JS_UNDEFINED;
            }
        }
        r = js_objv(o);
    } else if (c == '[') {
        P->p++;
        jsobj *a = js_array_new(J);
        jp_ws(P);
        if (P->p < P->end && *P->p == ']') {
            P->p++;
        } else {
            for (;;) {
                jsval v = jp_value(P);
                if (J->thrown) { P->depth--; return JS_UNDEFINED; }
                js_array_push(J, a, v);
                jp_ws(P);
                if (P->p < P->end && *P->p == ',') { P->p++; continue; }
                if (P->p < P->end && *P->p == ']') { P->p++; break; }
                jp_err(P);
                P->depth--;
                return JS_UNDEFINED;
            }
        }
        r = js_objv(a);
    } else if (c == '"') {
        jsstr *s;
        if (!jp_string(P, &s)) { P->depth--; return JS_UNDEFINED; }
        r = js_strv(s);
    } else if (!strncmp(P->p, "true", 4) && P->end - P->p >= 4) { P->p += 4; r = js_bool(true); }
    else if (!strncmp(P->p, "false", 5) && P->end - P->p >= 5) { P->p += 5; r = js_bool(false); }
    else if (!strncmp(P->p, "null", 4) && P->end - P->p >= 4) { P->p += 4; r = JS_NULLV; }
    else if (c == '-' || isdigit((unsigned char)c)) {
        const char *s = P->p;
        if (*P->p == '-') P->p++;
        if (P->p >= P->end || !isdigit((unsigned char)*P->p)) { jp_err(P); P->depth--; return JS_UNDEFINED; }
        while (P->p < P->end && isdigit((unsigned char)*P->p)) P->p++;
        if (P->p < P->end && *P->p == '.') { P->p++; while (P->p < P->end && isdigit((unsigned char)*P->p)) P->p++; }
        if (P->p < P->end && (*P->p == 'e' || *P->p == 'E')) {
            P->p++;
            if (P->p < P->end && (*P->p == '+' || *P->p == '-')) P->p++;
            while (P->p < P->end && isdigit((unsigned char)*P->p)) P->p++;
        }
        char buf[64];
        size_t l = (size_t)(P->p - s);
        if (l >= sizeof(buf)) l = sizeof(buf) - 1;
        memcpy(buf, s, l);
        buf[l] = 0;
        r = js_num(strtod(buf, NULL));
    } else {
        jp_err(P);
        P->depth--;
        return JS_UNDEFINED;
    }
    P->depth--;
    return r;
}
static jsval revive(js *J, jsval holder, jsstr *key, jsval fn)
{
    jsval v = get_prop(J, holder, key);
    THROW_IF(J);
    if (v.t == JS_OBJ) {
        jsobj *ks = js_array_new(J);
        own_keys(J, v.u.o, ks, true);
        for (uint32_t i = 0; i < ks->len; i++) {
            jsstr *k = js_intern_str(J, ks->items[i].u.s);
            jsval nv = revive(J, v, k, fn);
            THROW_IF(J);
            if (nv.t == JS_UNDEF) {
                if (v.u.o->cls == JC_ARRAY) { uint32_t idx; if (is_index_key(k, &idx)) v.u.o->items[idx] = JS_UNDEFINED; }
                else obj_delete(v.u.o, k);
            } else {
                set_prop(J, v, k, nv);
            }
        }
    }
    jsval a[2] = { js_strv(key), v };
    return call_function(J, fn.u.o, holder, 2, a, JS_UNDEFINED);
}
NATIVE(JSON_parse)
{
    jsstr *s = to_jsstr(J, ARG(0));
    THROW_IF(J);
    struct jparse P = { J, s->s, s->s + s->len, 0 };
    jsval v = jp_value(&P);
    THROW_IF(J);
    jp_ws(&P);
    if (P.p < P.end)
        return js_throw_error(J, "SyntaxError", "Unexpected token in JSON after the value");
    if (js_is_function(ARG(1))) {
        jsobj *h = js_object_new(J);
        set_prop(J, js_objv(h), J->k_empty, v);
        return revive(J, js_objv(h), J->k_empty, ARG(1));
    }
    return v;
}

/* ---- console / globals ---------------------------------------------------------------------------------------------- */

static void inspect(js *J, jsval v, int depth, struct sbuf *sb, bool top, jsobj **seen, int nseen)
{
    switch (v.t) {
    case JS_STR:
        if (top) { sb_add(sb, v.u.s->s, v.u.s->len); }
        else { sb_putc(sb, '\''); sb_add(sb, v.u.s->s, v.u.s->len); sb_putc(sb, '\''); }
        return;
    case JS_OBJ: break;
    default: {
        jsstr *s = to_jsstr(J, v);
        if (s) sb_add(sb, s->s, s->len);
        return;
    }
    }
    jsobj *o = v.u.o;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == o) { sb_addz(sb, "[Circular]"); return; }
    if (o->cls == JC_FUNCTION) {
        jsval n = get_prop(J, v, J->k_name);
        char b[160];
        snprintf(b, sizeof(b), "[Function%s%s]", n.t == JS_STR && n.u.s->len ? ": " : " (anonymous)", n.t == JS_STR ? n.u.s->s : "");
        sb_addz(sb, b);
        return;
    }
    if (o->cls == JC_ERROR) {
        jsval st = get_prop(J, v, J->k_stack);
        if (st.t == JS_STR) { sb_add(sb, st.u.s->s, st.u.s->len); return; }
    }
    if (o->cls == JC_DATE) {
        jsval f = get_prop(J, v, js_intern(J, "toISOString", 11));
        if (js_is_function(f)) {
            jsval r = call_function(J, f.u.o, v, 0, NULL, JS_UNDEFINED);
            if (!J->thrown && r.t == JS_STR) { sb_add(sb, r.u.s->s, r.u.s->len); return; }
            J->thrown = false;
        }
    }
    if (depth > 2) { sb_addz(sb, o->cls == JC_ARRAY ? "[Array]" : "[Object]"); return; }
    jsobj *stack[16];
    int ns = nseen < 15 ? nseen : 15;
    memcpy(stack, seen, sizeof(jsobj *) * (size_t)ns);
    stack[ns++] = o;
    if (o->cls == JC_ARRAY || o->cls == JC_SET) {
        sb_addz(sb, o->cls == JC_SET ? "Set(" : "");
        char n[16];
        if (o->cls == JC_SET) { snprintf(n, sizeof(n), "%u) ", o->len); sb_addz(sb, n); }
        sb_addz(sb, o->len ? "[ " : "[");
        for (uint32_t i = 0; i < o->len && i < 100; i++) {
            if (i) sb_addz(sb, ", ");
            inspect(J, o->items[i], depth + 1, sb, false, stack, ns);
        }
        if (o->len > 100) sb_addz(sb, ", ...");
        sb_addz(sb, o->len ? " ]" : "]");
        return;
    }
    if (o->cls == JC_MAP) {
        char n[24];
        snprintf(n, sizeof(n), "Map(%u) {", o->len);
        sb_addz(sb, n);
        for (uint32_t i = 0; i < o->len; i++) {
            sb_addz(sb, i ? ", " : " ");
            inspect(J, o->items[i], depth + 1, sb, false, stack, ns);
            sb_addz(sb, " => ");
            inspect(J, o->items2[i], depth + 1, sb, false, stack, ns);
        }
        sb_addz(sb, o->len ? " }" : "}");
        return;
    }
    if (o->cls == JC_HOST && o->hclass && o->hclass->name) {
        sb_addz(sb, o->hclass->name);
        return;
    }
    jsobj *ks = js_array_new(J);
    own_keys(J, o, ks, true);
    if (!ks->len) { sb_addz(sb, "{}"); return; }
    sb_addz(sb, "{ ");
    for (uint32_t i = 0; i < ks->len && i < 50; i++) {
        if (i) sb_addz(sb, ", ");
        sb_add(sb, ks->items[i].u.s->s, ks->items[i].u.s->len);
        sb_addz(sb, ": ");
        jsval pv = get_prop(J, v, js_intern_str(J, ks->items[i].u.s));
        if (J->thrown) { J->thrown = false; pv = JS_UNDEFINED; }
        inspect(J, pv, depth + 1, sb, false, stack, ns);
    }
    sb_addz(sb, " }");
}

static jsval console_emit(js *J, const char *level, int argc, jsval *argv)
{
    struct sbuf sb = { 0 };
    for (int i = 0; i < argc; i++) {
        if (i) sb_putc(&sb, ' ');
        inspect(J, argv[i], 0, &sb, true, NULL, 0);
    }
    if (J->console)
        J->console(J->console_data, level, sb.p ? sb.p : "");
    else
        fprintf(stderr, "[js %s] %s\n", level, sb.p ? sb.p : "");
    sb_free(&sb);
    return JS_UNDEFINED;
}
NATIVE(console_log) { return console_emit(J, "log", argc, argv); }
NATIVE(console_info) { return console_emit(J, "info", argc, argv); }
NATIVE(console_warn) { return console_emit(J, "warn", argc, argv); }
NATIVE(console_error) { return console_emit(J, "error", argc, argv); }
NATIVE(console_debug) { return console_emit(J, "debug", argc, argv); }
NATIVE(console_noop) { return JS_UNDEFINED; }
NATIVE(console_assert)
{
    if (!js_truthy(ARG(0)))
        console_emit(J, "error", argc > 1 ? argc - 1 : 0, argv + 1);
    return JS_UNDEFINED;
}

NATIVE(global_queueMicrotask)
{
    if (!js_is_function(ARG(0)))
        return throw_type(J, "queueMicrotask needs a function");
    if (J->mq_n == J->mq_cap) {
        J->mq_cap = J->mq_cap ? J->mq_cap * 2 : 32;
        J->mq = realloc(J->mq, sizeof(jsval) * J->mq_cap);
        if (!J->mq)
            exit(1);
    }
    J->mq[J->mq_n++] = ARG(0);
    return JS_UNDEFINED;
}

void js_run_microtasks(js *J)
{
    if (J->entries > 0 || J->draining)
        return;
    J->draining = true;
    while (J->mq_head < J->mq_n && !J->aborted) {
        jsval fn = J->mq[J->mq_head++];
        call_function(J, fn.u.o, JS_UNDEFINED, 0, NULL, JS_UNDEFINED);
        if (J->thrown) {
            jsval ex = js_exception(J);
            char b[400];
            js_describe_exception(J, ex, b, sizeof(b));
            if (J->console) J->console(J->console_data, "error", b);
            else fprintf(stderr, "[js error] %s\n", b);
        }
    }
    J->mq_head = J->mq_n = 0;
    J->draining = false;
}

NATIVE(global_structuredClone)
{
    jsval js_ = js_get(J, js_objv(J->global), "JSON");
    jsval s = js_call(J, js_get(J, js_, "stringify"), js_, 1, argv, NULL);
    THROW_IF(J);
    if (s.t != JS_STR)
        return s;
    return js_call(J, js_get(J, js_, "parse"), js_, 1, &s, NULL);
}

/* ---- setup ------------------------------------------------------------------------------------------------------- */

void js_init_builtins(js *J)
{
    J->object_proto = obj_new(J, JC_OBJECT, NULL);
    J->function_proto = obj_new(J, JC_OBJECT, J->object_proto);
    J->array_proto = obj_new(J, JC_OBJECT, J->object_proto);
    J->string_proto = obj_new(J, JC_OBJECT, J->object_proto);
    J->number_proto = obj_new(J, JC_OBJECT, J->object_proto);
    J->boolean_proto = obj_new(J, JC_OBJECT, J->object_proto);
    J->error_proto = obj_new(J, JC_OBJECT, J->object_proto);
    J->date_proto = obj_new(J, JC_OBJECT, J->object_proto);
    J->map_proto = obj_new(J, JC_OBJECT, J->object_proto);
    J->set_proto = obj_new(J, JC_OBJECT, J->object_proto);
    J->global = obj_new(J, JC_OBJECT, J->object_proto);
    J->gscope = eval_global_scope(J);

    lib_const(J, J->global, "globalThis", js_objv(J->global));
    obj_add_prop(J, J->global, J->k_undefined, JS_UNDEFINED, 0);
    lib_const(J, J->global, "NaN", js_num(NAN));
    lib_const(J, J->global, "Infinity", js_num(INFINITY));

    /* Object */
    jsobj *O = lib_ctor(J, "Object", Object_ctor, 1, J->object_proto);
    lib_def(J, O, "keys", Object_keys, 1); lib_def(J, O, "values", Object_values, 1);
    lib_def(J, O, "entries", Object_entries, 1); lib_def(J, O, "assign", Object_assign, 2);
    lib_def(J, O, "create", Object_create, 2); lib_def(J, O, "getPrototypeOf", Object_getPrototypeOf, 1);
    lib_def(J, O, "setPrototypeOf", Object_setPrototypeOf, 2); lib_def(J, O, "defineProperty", Object_defineProperty, 3);
    lib_def(J, O, "defineProperties", Object_defineProperties, 2);
    lib_def(J, O, "getOwnPropertyNames", Object_getOwnPropertyNames, 1);
    lib_def(J, O, "getOwnPropertyDescriptor", Object_getOwnPropertyDescriptor, 2);
    lib_def(J, O, "freeze", Object_freeze, 1); lib_def(J, O, "isFrozen", Object_isFrozen, 1);
    lib_def(J, O, "seal", Object_seal, 1); lib_def(J, O, "isSealed", Object_isFrozen, 1);
    lib_def(J, O, "preventExtensions", Object_preventExtensions, 1); lib_def(J, O, "isExtensible", Object_isExtensible, 1);
    lib_def(J, O, "fromEntries", Object_fromEntries, 1); lib_def(J, O, "is", Object_is, 2);
    jsobj *OP = J->object_proto;
    lib_def(J, OP, "hasOwnProperty", Object_hasOwnProperty, 1); lib_def(J, OP, "isPrototypeOf", Object_isPrototypeOf, 1);
    lib_def(J, OP, "propertyIsEnumerable", Object_propertyIsEnumerable, 1); lib_def(J, OP, "toString", Object_toString, 0);
    lib_def(J, OP, "toLocaleString", Object_toString, 0); lib_def(J, OP, "valueOf", Object_valueOf, 0);
    {
        jsstr *k = js_intern(J, "__proto__", 9);
        struct prop *p = obj_add_prop(J, OP, k, JS_UNDEFINED, PF_ACCESSOR);
        p->get = make_native(J, k, Object_get_proto, 0, NULL);
        p->set = make_native(J, k, Object_set_proto, 1, NULL);
    }

    /* Function */
    jsobj *F = lib_ctor(J, "Function", Function_ctor, 1, J->function_proto);
    (void)F;
    lib_def(J, J->function_proto, "call", Function_call, 1); lib_def(J, J->function_proto, "apply", Function_apply, 2);
    lib_def(J, J->function_proto, "bind", Function_bind, 1); lib_def(J, J->function_proto, "toString", Function_toString, 0);

    /* Array */
    jsobj *A = lib_ctor(J, "Array", Array_ctor, 1, J->array_proto);
    lib_def(J, A, "isArray", Array_isArray, 1); lib_def(J, A, "of", Array_of, 0); lib_def(J, A, "from", Array_from, 1);
    jsobj *AP = J->array_proto;
    lib_def(J, AP, "push", Array_push, 1); lib_def(J, AP, "pop", Array_pop, 0); lib_def(J, AP, "shift", Array_shift, 0);
    lib_def(J, AP, "unshift", Array_unshift, 1); lib_def(J, AP, "slice", Array_slice, 2);
    lib_def(J, AP, "splice", Array_splice, 2); lib_def(J, AP, "concat", Array_concat, 1);
    lib_def(J, AP, "join", Array_join, 1); lib_def(J, AP, "toString", Array_toString, 0);
    lib_def(J, AP, "reverse", Array_reverse, 0); lib_def(J, AP, "indexOf", Array_indexOf, 1);
    lib_def(J, AP, "lastIndexOf", Array_lastIndexOf, 1); lib_def(J, AP, "includes", Array_includes, 1);
    lib_def(J, AP, "forEach", Array_forEach, 1); lib_def(J, AP, "map", Array_map, 1);
    lib_def(J, AP, "filter", Array_filter, 1); lib_def(J, AP, "reduce", Array_reduce, 1);
    lib_def(J, AP, "reduceRight", Array_reduceRight, 1); lib_def(J, AP, "some", Array_some, 1);
    lib_def(J, AP, "every", Array_every, 1); lib_def(J, AP, "find", Array_find, 1);
    lib_def(J, AP, "findIndex", Array_findIndex, 1); lib_def(J, AP, "findLast", Array_findLast, 1);
    lib_def(J, AP, "findLastIndex", Array_findLastIndex, 1); lib_def(J, AP, "fill", Array_fill, 1);
    lib_def(J, AP, "at", Array_at, 1); lib_def(J, AP, "flat", Array_flat, 0); lib_def(J, AP, "flatMap", Array_flatMap, 1);
    lib_def(J, AP, "keys", Array_keys, 0); lib_def(J, AP, "values", Array_values, 0);
    lib_def(J, AP, "entries", Array_entries, 0); lib_def(J, AP, "sort", Array_sort, 1);

    /* Errors */
    static const char *const names[] = { "Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", "EvalError", "URIError" };
    static const js_native ctors[] = { Error_ctor, TypeError_ctor, RangeError_ctor, SyntaxError_ctor, ReferenceError_ctor,
                                       EvalError_ctor, URIError_ctor };
    jsobj *base_ctor = NULL;
    for (int i = 0; i < 7; i++) {
        jsobj *proto = i == 0 ? J->error_proto : obj_new(J, JC_OBJECT, J->error_proto);
        J->err_protos[i] = proto;
        jsobj *c = lib_ctor(J, names[i], ctors[i], 1, proto);
        obj_add_prop(J, proto, J->k_name, js_cstr(J, names[i]), PF_WRITE);
        obj_add_prop(J, proto, J->k_message, js_cstr(J, ""), PF_WRITE);
        if (i == 0) {
            base_ctor = c;
            lib_def(J, proto, "toString", Error_toString, 0);
            lib_def(J, c, "captureStackTrace", Error_captureStackTrace, 1);
        } else {
            c->proto = base_ctor;
        }
    }

    /* Boolean / Number */
    jsobj *B = lib_ctor(J, "Boolean", Boolean_ctor, 1, J->boolean_proto);
    (void)B;
    lib_def(J, J->boolean_proto, "toString", Boolean_toString, 0); lib_def(J, J->boolean_proto, "valueOf", Boolean_valueOf, 0);
    jsobj *N = lib_ctor(J, "Number", Number_ctor, 1, J->number_proto);
    lib_def(J, N, "isInteger", Number_isInteger, 1); lib_def(J, N, "isSafeInteger", Number_isSafeInteger, 1);
    lib_def(J, N, "isFinite", Number_isFinite, 1); lib_def(J, N, "isNaN", Number_isNaN, 1);
    lib_def(J, N, "parseFloat", global_parseFloat, 1); lib_def(J, N, "parseInt", global_parseInt, 2);
    lib_const(J, N, "MAX_SAFE_INTEGER", js_num(9007199254740991.0)); lib_const(J, N, "MIN_SAFE_INTEGER", js_num(-9007199254740991.0));
    lib_const(J, N, "EPSILON", js_num(2.220446049250313e-16)); lib_const(J, N, "MAX_VALUE", js_num(1.7976931348623157e308));
    lib_const(J, N, "MIN_VALUE", js_num(5e-324)); lib_const(J, N, "POSITIVE_INFINITY", js_num(INFINITY));
    lib_const(J, N, "NEGATIVE_INFINITY", js_num(-INFINITY)); lib_const(J, N, "NaN", js_num(NAN));
    jsobj *NP = J->number_proto;
    lib_def(J, NP, "toString", Number_toString, 1); lib_def(J, NP, "toFixed", Number_toFixed, 1);
    lib_def(J, NP, "toPrecision", Number_toPrecision, 1); lib_def(J, NP, "toExponential", Number_toExponential, 1);
    lib_def(J, NP, "valueOf", Number_valueOf, 0); lib_def(J, NP, "toLocaleString", Number_toLocaleString, 0);
    lib_def(J, J->global, "parseFloat", global_parseFloat, 1); lib_def(J, J->global, "parseInt", global_parseInt, 2);
    lib_def(J, J->global, "eval", global_eval, 1);
    lib_def(J, J->global, "isNaN", global_isNaN, 1); lib_def(J, J->global, "isFinite", global_isFinite, 1);
    lib_def(J, J->global, "structuredClone", global_structuredClone, 1);
    lib_def(J, J->global, "queueMicrotask", global_queueMicrotask, 1);

    /* Math */
    jsobj *M = js_object_new(J);
    lib_const(J, J->global, "Math", js_objv(M));
    lib_def(J, M, "abs", Math_abs, 1); lib_def(J, M, "ceil", Math_ceil, 1); lib_def(J, M, "floor", Math_floor, 1);
    lib_def(J, M, "trunc", Math_trunc, 1); lib_def(J, M, "sqrt", Math_sqrt, 1); lib_def(J, M, "cbrt", Math_cbrt, 1);
    lib_def(J, M, "exp", Math_exp, 1); lib_def(J, M, "log", Math_log, 1); lib_def(J, M, "log2", Math_log2, 1);
    lib_def(J, M, "log10", Math_log10, 1); lib_def(J, M, "sin", Math_sin, 1); lib_def(J, M, "cos", Math_cos, 1);
    lib_def(J, M, "tan", Math_tan, 1); lib_def(J, M, "asin", Math_asin, 1); lib_def(J, M, "acos", Math_acos, 1);
    lib_def(J, M, "atan", Math_atan, 1); lib_def(J, M, "sinh", Math_sinh, 1); lib_def(J, M, "cosh", Math_cosh, 1);
    lib_def(J, M, "tanh", Math_tanh, 1); lib_def(J, M, "log1p", Math_log1p, 1); lib_def(J, M, "expm1", Math_expm1, 1);
    lib_def(J, M, "fround", Math_fround, 1); lib_def(J, M, "sign", Math_sign, 1); lib_def(J, M, "round", Math_round, 1);
    lib_def(J, M, "clz32", Math_clz32, 1); lib_def(J, M, "atan2", Math_atan2, 2); lib_def(J, M, "pow", Math_pow, 2);
    lib_def(J, M, "min", Math_min, 2); lib_def(J, M, "max", Math_max, 2); lib_def(J, M, "hypot", Math_hypot, 2);
    lib_def(J, M, "random", Math_random, 0);
    lib_const(J, M, "PI", js_num(M_PI)); lib_const(J, M, "E", js_num(M_E)); lib_const(J, M, "LN2", js_num(M_LN2));
    lib_const(J, M, "LN10", js_num(M_LN10)); lib_const(J, M, "LOG2E", js_num(M_LOG2E));
    lib_const(J, M, "LOG10E", js_num(M_LOG10E)); lib_const(J, M, "SQRT2", js_num(M_SQRT2));
    lib_const(J, M, "SQRT1_2", js_num(M_SQRT1_2));

    /* JSON, console */
    jsobj *JS = js_object_new(J);
    lib_const(J, J->global, "JSON", js_objv(JS));
    lib_def(J, JS, "stringify", JSON_stringify, 3); lib_def(J, JS, "parse", JSON_parse, 2);
    jsobj *C = js_object_new(J);
    lib_const(J, J->global, "console", js_objv(C));
    lib_def(J, C, "log", console_log, 0); lib_def(J, C, "info", console_info, 0); lib_def(J, C, "warn", console_warn, 0);
    lib_def(J, C, "error", console_error, 0); lib_def(J, C, "debug", console_debug, 0); lib_def(J, C, "trace", console_debug, 0);
    lib_def(J, C, "assert", console_assert, 0);
    static const char *const noops[] = { "group", "groupEnd", "groupCollapsed", "time", "timeEnd", "table", "dir", "clear", "count" };
    for (size_t i = 0; i < sizeof(noops) / sizeof(noops[0]); i++)
        lib_def(J, C, noops[i], console_noop, 0);

    js_init_lib2(J);
    js_init_regex(J);
    js_init_coro(J);
    js_init_polyfills(J);
}
