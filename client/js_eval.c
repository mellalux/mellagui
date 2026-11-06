/* JavaScript tree-walking evaluator (see js_int.h). */
#include <math.h>

#include "js_int.h"

#define MAX_DEPTH 120

enum completion { C_NORMAL, C_BREAK, C_CONTINUE, C_RETURN };

static jsval eval(js *J, struct node *n, struct scope *sc);
static enum completion exec(js *J, struct node *n, struct scope *sc);
static jsval make_closure(js *J, struct node *fn, struct scope *sc, jsobj *home);
static jsval eval_class(js *J, struct node *n, struct scope *sc);
static bool bind_pattern(js *J, struct node *pat, jsval v, struct scope *sc, int mode);

/* binding modes for patterns */
#define BM_ASSIGN 0
#define BM_VAR 1
#define BM_LET 2
#define BM_CONST 3

static jsval abort_run(js *J, const char *why)
{
    (void)why;
    J->aborted = true;
    J->thrown = true;
    J->exc = JS_UNDEFINED;
    return JS_UNDEFINED;
}

#define STEP(J) do { if ((J)->budget && ++(J)->steps > (J)->budget) { abort_run(J, "budget"); return JS_UNDEFINED; } } while (0)

/* ---- scopes ------------------------------------------------------------------------------------------------ */

static struct scope *scope_new(js *J, struct scope *parent)
{
    struct scope *s = J->free_scopes;
    if (s) {
        J->free_scopes = s->next_free;
        memset(s, 0, sizeof(*s));
    } else {
        s = js_alloc(J, sizeof(*s));
    }
    s->pooled = 1;
    s->parent = parent;
    s->names = s->in_names;
    s->vals = s->in_vals;
    s->fl = s->in_fl;
    s->cap = SC_INLINE;
    return s;
}

static void scope_release(js *J, struct scope *s)
{
    if (s->captured)
        return;
    if (s->names != s->in_names) {
        /* grown buffers were arena allocated: leave them */
    }
    s->next_free = J->free_scopes;
    J->free_scopes = s;
}

static void scope_mark_captured(struct scope *s)
{
    for (; s && !s->captured; s = s->parent)
        s->captured = 1;
}

static void scope_grow(js *J, struct scope *s)
{
    uint32_t nc = s->cap * 2;
    jsstr **nn = js_alloc(J, nc * sizeof(*nn));
    jsval *nv = js_alloc(J, nc * sizeof(*nv));
    uint8_t *nf = js_alloc(J, nc);
    memcpy(nn, s->names, s->n * sizeof(*nn));
    memcpy(nv, s->vals, s->n * sizeof(*nv));
    memcpy(nf, s->fl, s->n);
    s->names = nn;
    s->vals = nv;
    s->fl = nf;
    s->cap = nc;
}

static int scope_find(const struct scope *s, jsstr *name)
{
    for (uint32_t i = 0; i < s->n; i++)
        if (s->names[i] == name)
            return (int)i;
    return -1;
}

static void scope_declare(js *J, struct scope *s, jsstr *name, jsval v, bool is_const)
{
    int i = scope_find(s, name);
    if (i >= 0) {
        s->vals[i] = v;
        s->fl[i] = is_const;
        return;
    }
    if (s->n == s->cap)
        scope_grow(J, s);
    s->names[s->n] = name;
    s->vals[s->n] = v;
    s->fl[s->n] = is_const;
    s->n++;
}

/* The scope that receives `var` and function declarations: the nearest function scope (or the global one). */
static struct scope *var_scope(struct scope *s)
{
    while (s->parent && !s->has_this && (!s->obj || s->is_with)) {
        if (s->func)
            return s;
        s = s->parent;
    }
    return s;
}

static jsval lookup(js *J, jsstr *name, struct scope *sc, bool *found)
{
    for (struct scope *s = sc; s; s = s->parent) {
        int i = scope_find(s, name);
        if (i >= 0) {
            *found = true;
            return s->vals[i];
        }
        if (s->obj && has_prop(J, s->obj, name)) {
            *found = true;
            return get_prop(J, js_objv(s->obj), name);
        }
    }
    *found = false;
    return JS_UNDEFINED;
}

static void assign_var(js *J, jsstr *name, jsval v, struct scope *sc)
{
    for (struct scope *s = sc; s; s = s->parent) {
        int i = scope_find(s, name);
        if (i >= 0) {
            if (s->fl[i]) {
                throw_type(J, "Assignment to constant variable '%s'", name->s);
                return;
            }
            s->vals[i] = v;
            return;
        }
        if (s->obj && has_prop(J, s->obj, name)) {
            set_prop(J, js_objv(s->obj), name, v);
            return;
        }
    }
    set_prop(J, js_objv(J->global), name, v);               /* sloppy mode: implicit global */
}

static jsval this_of(js *J, struct scope *sc)
{
    for (struct scope *s = sc; s; s = s->parent)
        if (s->has_this)
            return s->thisv;
    return js_objv(J->global);
}

static struct scope *func_scope_of(struct scope *sc)
{
    for (struct scope *s = sc; s; s = s->parent)
        if (s->has_this)
            return s;
    return NULL;
}

/* ---- operators ---------------------------------------------------------------------------------------------- */

static jsval binary_op(js *J, int op, jsval a, jsval b)
{
    switch (op) {
    case OP_ADD: {
        if (a.t == JS_NUM && b.t == JS_NUM)
            return js_num(a.u.n + b.u.n);
        jsval pa = to_primitive(J, a, 0);
        THROW_IF(J);
        jsval pb = to_primitive(J, b, 0);
        THROW_IF(J);
        if (pa.t == JS_STR || pb.t == JS_STR) {
            jsstr *sa = to_jsstr(J, pa);
            THROW_IF(J);
            jsstr *sb = to_jsstr(J, pb);
            THROW_IF(J);
            return js_strv(str_concat(J, sa, sb));
        }
        double x = to_number(J, pa), y = to_number(J, pb);
        THROW_IF(J);
        return js_num(x + y);
    }
    case OP_EQ: { bool r = loose_equals(J, a, b); THROW_IF(J); return js_bool(r); }
    case OP_NE: { bool r = loose_equals(J, a, b); THROW_IF(J); return js_bool(!r); }
    case OP_SEQ: return js_bool(js_strict_equals(a, b));
    case OP_SNE: return js_bool(!js_strict_equals(a, b));
    case OP_LT: case OP_GT: case OP_LE: case OP_GE: {
        if (a.t == JS_NUM && b.t == JS_NUM) {
            switch (op) {
            case OP_LT: return js_bool(a.u.n < b.u.n);
            case OP_GT: return js_bool(a.u.n > b.u.n);
            case OP_LE: return js_bool(a.u.n <= b.u.n);
            default: return js_bool(a.u.n >= b.u.n);
            }
        }
        jsval pa = to_primitive(J, a, 1);
        THROW_IF(J);
        jsval pb = to_primitive(J, b, 1);
        THROW_IF(J);
        if (pa.t == JS_STR && pb.t == JS_STR) {
            size_t la = pa.u.s->len, lb = pb.u.s->len;
            int c = memcmp(pa.u.s->s, pb.u.s->s, la < lb ? la : lb);
            if (!c)
                c = la < lb ? -1 : la > lb ? 1 : 0;
            switch (op) {
            case OP_LT: return js_bool(c < 0);
            case OP_GT: return js_bool(c > 0);
            case OP_LE: return js_bool(c <= 0);
            default: return js_bool(c >= 0);
            }
        }
        double x = to_number(J, pa), y = to_number(J, pb);
        THROW_IF(J);
        switch (op) {
        case OP_LT: return js_bool(x < y);
        case OP_GT: return js_bool(x > y);
        case OP_LE: return js_bool(x <= y);
        default: return js_bool(x >= y);
        }
    }
    case OP_IN: {
        if (b.t != JS_OBJ)
            return throw_type(J, "Cannot use 'in' operator to search for a key in a non-object");
        jsstr *k = key_of(J, a);
        THROW_IF(J);
        return js_bool(has_prop(J, b.u.o, k));
    }
    case OP_INSTANCEOF: {
        if (!js_is_function(b))
            return throw_type(J, "Right-hand side of 'instanceof' is not callable");
        if (a.t != JS_OBJ)
            return js_bool(false);
        jsval proto = get_prop(J, b, J->k_prototype);
        THROW_IF(J);
        for (jsobj *o = a.u.o->proto; o; o = o->proto)
            if (proto.t == JS_OBJ && o == proto.u.o)
                return js_bool(true);
        return js_bool(false);
    }
    default:
        break;
    }
    double x = to_number(J, a);
    THROW_IF(J);
    double y = to_number(J, b);
    THROW_IF(J);
    switch (op) {
    case OP_SUB: return js_num(x - y);
    case OP_MUL: return js_num(x * y);
    case OP_DIV: return js_num(x / y);
    case OP_MOD: return js_num(fmod(x, y));
    case OP_EXP: return js_num(isnan(y) ? NAN : (y == 0 ? 1 : pow(x, y)));
    case OP_SHL: return js_num((double)(int32_t)((uint32_t)to_int32(x) << (to_uint32(y) & 31)));
    case OP_SHR: return js_num((double)(to_int32(x) >> (to_uint32(y) & 31)));
    case OP_USHR: return js_num((double)(to_uint32(x) >> (to_uint32(y) & 31)));
    case OP_BAND: return js_num((double)(to_int32(x) & to_int32(y)));
    case OP_BOR: return js_num((double)(to_int32(x) | to_int32(y)));
    case OP_BXOR: return js_num((double)(to_int32(x) ^ to_int32(y)));
    }
    return JS_UNDEFINED;
}

/* ---- member access -------------------------------------------------------------------------------------------- */

static jsstr *member_key(js *J, struct node *m, struct scope *sc, jsval *numkey)
{
    numkey->t = JS_UNDEF;
    if (!(m->flag & 1))
        return m->b->s ? js_intern_str(J, m->b->s) : NULL;
    jsval k = eval(J, m->b, sc);
    if (J->thrown)
        return NULL;
    if (k.t == JS_NUM) {
        *numkey = k;
        return NULL;
    }
    return key_of(J, k);
}

static jsval get_member(js *J, jsval base, jsstr *key, jsval numkey)
{
    if (numkey.t == JS_NUM) {
        double d = numkey.u.n;
        if (d >= 0 && d == floor(d) && d < 4294967295.0) {
            uint32_t i = (uint32_t)d;
            if (base.t == JS_OBJ && base.u.o->cls == JC_ARRAY)
                return i < base.u.o->len ? base.u.o->items[i] : JS_UNDEFINED;
            if (base.t == JS_STR)
                return str_char_at(J, base.u.s, i);
        }
        key = num_to_str(J, d);
        key = js_intern_str(J, key);
    }
    return get_prop(J, base, key);
}

static void put_member(js *J, jsval base, jsstr *key, jsval numkey, jsval v)
{
    if (numkey.t == JS_NUM) {
        double d = numkey.u.n;
        if (base.t == JS_OBJ && base.u.o->cls == JC_ARRAY && !(base.u.o->flags & OF_FROZEN) && d >= 0 &&
            d == floor(d) && d < 4294967295.0) {
            array_put(J, base.u.o, (uint32_t)d, v);
            return;
        }
        key = js_intern_str(J, num_to_str(J, d));
    }
    set_prop(J, base, key, v);
}

static const char *callee_name(struct node *n, char *buf, size_t bn)
{
    if (n->t == N_IDENT) {
        snprintf(buf, bn, "%s", n->s->s);
    } else if (n->t == N_MEMBER) {
        char inner[100];
        callee_name(n->a, inner, sizeof(inner));
        if (n->flag & 1)
            snprintf(buf, bn, "%s[...]", inner);
        else
            snprintf(buf, bn, "%s.%s", inner, n->b->s->s);
    } else if (n->t == N_THIS) {
        snprintf(buf, bn, "this");
    } else if (n->t == N_CALL) {
        char inner[100];
        callee_name(n->a, inner, sizeof(inner));
        snprintf(buf, bn, "%s(...)", inner);
    } else {
        snprintf(buf, bn, "expression");
    }
    return buf;
}

/* ---- functions ------------------------------------------------------------------------------------------------ */

static jsval make_closure(js *J, struct node *fn, struct scope *sc, jsobj *home)
{
    struct fninfo *fi = fn->fi;
    jsobj *f = obj_new(J, JC_FUNCTION, J->function_proto);
    f->fn = js_alloc(J, sizeof(struct func));
    f->fn->fi = fi;
    f->fn->scope = sc;
    f->fn->home = home;
    f->fn->name = fi->name;
    scope_mark_captured(sc);
    obj_add_prop(J, f, J->k_length, js_num(fi->nformal), 0);
    obj_add_prop(J, f, J->k_name, js_strv(fi->name ? fi->name : J->k_empty), 0);
    if (fi->is_gen) {
        jsobj *proto = obj_new(J, JC_OBJECT, J->gen_proto);
        obj_add_prop(J, f, J->k_prototype, js_objv(proto), PF_WRITE);
    } else if (!fi->is_arrow && !fi->is_method && !fi->is_async) {
        jsobj *proto = js_object_new(J);
        obj_add_prop(J, proto, J->k_constructor, js_objv(f), PF_WRITE);
        obj_add_prop(J, f, J->k_prototype, js_objv(proto), PF_WRITE);
    }
    return js_objv(f);
}

static jsval eval_args(js *J, struct node *call, struct scope *sc, jsval **out, int *outn)
{
    int n = call->n;
    int cap = n + 4, cnt = 0;
    jsval *v = malloc(sizeof(jsval) * (size_t)cap);
    if (!v)
        exit(1);
    for (int i = 0; i < n; i++) {
        struct node *a = call->list[i];
        if (a->t == N_SPREAD) {
            jsval s = eval(J, a->a, sc);
            if (J->thrown) { free(v); return JS_UNDEFINED; }
            if (s.t == JS_OBJ && s.u.o->cls == JC_ARRAY) {
                for (uint32_t k = 0; k < s.u.o->len; k++) {
                    if (cnt == cap) { cap *= 2; v = realloc(v, sizeof(jsval) * (size_t)cap); }
                    v[cnt++] = s.u.o->items[k];
                }
            } else if (s.t == JS_STR) {
                for (uint32_t k = 0; k < s.u.s->len; k++) {
                    if (cnt == cap) { cap *= 2; v = realloc(v, sizeof(jsval) * (size_t)cap); }
                    v[cnt++] = js_strn(J, s.u.s->s + k, 1);
                }
            } else if (s.t == JS_OBJ && (s.u.o->cls == JC_SET || s.u.o->cls == JC_MAP)) {
                for (uint32_t k = 0; k < s.u.o->len; k++) {
                    if (cnt == cap) { cap *= 2; v = realloc(v, sizeof(jsval) * (size_t)cap); }
                    v[cnt++] = s.u.o->items[k];
                }
            } else {
                free(v);
                return throw_type(J, "spread of a non-iterable value");
            }
            continue;
        }
        jsval x = eval(J, a, sc);
        if (J->thrown) { free(v); return JS_UNDEFINED; }
        if (cnt == cap) { cap *= 2; v = realloc(v, sizeof(jsval) * (size_t)cap); }
        v[cnt++] = x;
    }
    *out = v;
    *outn = cnt;
    return JS_UNDEFINED;
}

static void run_field_inits(js *J, jsobj *ctor, jsval thisv);

jsval call_function(js *J, jsobj *f, jsval thisv, int argc, jsval *argv, jsval newtarget)
{
    if (f->cls == JC_PROXY) {                                   /* callable proxy: apply / construct traps */
        bool is_new = newtarget.t != JS_UNDEF;
        jsval trap = get_prop(J, js_objv((jsobj *)f->host), js_intern(J, is_new ? "construct" : "apply", is_new ? 9 : 5));
        THROW_IF(J);
        jsobj *arr = js_array_new(J);
        for (int i = 0; i < argc; i++)
            js_array_push(J, arr, argv[i]);
        if (js_is_function(trap)) {
            jsval args[3] = { f->prim, is_new ? js_objv(arr) : thisv, is_new ? js_objv(arr) : js_objv(arr) };
            if (is_new) {
                args[1] = js_objv(arr);
                return call_function(J, trap.u.o, js_objv((jsobj *)f->host), 2, args, JS_UNDEFINED);
            }
            return call_function(J, trap.u.o, js_objv((jsobj *)f->host), 3, args, JS_UNDEFINED);
        }
        if (is_new)
            return construct(J, f->prim, argc, argv);
        return call_function(J, f->prim.u.o, thisv, argc, argv, JS_UNDEFINED);
    }
    struct func *fn = f->fn;
    if (!fn)
        return throw_type(J, "not a function");
    if (fn->native) {
        if (J->depth > MAX_DEPTH)
            return js_throw_error(J, "RangeError", "Maximum call stack size exceeded");
        J->depth++;
        jsval saved_nt = J->newtarget;
        J->newtarget = newtarget;
        jsval r = fn->native(J, thisv, argc, argv, f);
        J->newtarget = saved_nt;
        J->depth--;
        return r;
    }
    struct fninfo *fi = fn->fi;
    if (fn->is_class && newtarget.t == JS_UNDEF)
        return throw_type(J, "Class constructor %s cannot be invoked without 'new'", fn->name ? fn->name->s : "");
    if (fi->is_gen)
        return make_generator(J, f, thisv, argc, argv);
    if (fi->is_async)
        return start_async(J, f, thisv, argc, argv);
    return run_closure_body(J, f, thisv, argc, argv, newtarget);
}

jsval run_closure_body(js *J, jsobj *f, jsval thisv, int argc, jsval *argv, jsval newtarget)
{
    struct func *fn = f->fn;
    struct fninfo *fi = fn->fi;
    if (J->depth > (J->cur_co ? 40 : MAX_DEPTH))
        return js_throw_error(J, "RangeError", "Maximum call stack size exceeded");
    J->depth++;
    struct scope *sc = scope_new(J, fn->scope);
    sc->func = f;
    if (!fi->is_arrow) {
        sc->has_this = 1;
        /* sloppy mode: an undefined `this` is the global object (class code is strict and keeps it) */
        sc->thisv = (thisv.t == JS_UNDEF || thisv.t == JS_NULL) && !fn->home ? js_objv(J->global) : thisv;
    }
    if (fi->uses_args && !fi->is_arrow) {
        jsobj *a = js_array_new(J);
        for (int i = 0; i < argc; i++)
            js_array_push(J, a, argv[i]);
        sc->args_obj = a;
        scope_declare(J, sc, J->k_arguments, js_objv(a), false);
    }
    jsval result = JS_UNDEFINED;
    /* parameters */
    for (int i = 0; i < fi->nparams && !J->thrown; i++) {
        struct node *p = fi->params[i];
        if (p->t == N_SPREAD) {
            jsobj *rest = js_array_new(J);
            for (int k = i; k < argc; k++)
                js_array_push(J, rest, argv[k]);
            bind_pattern(J, p->a, js_objv(rest), sc, BM_LET);
            break;
        }
        bind_pattern(J, p, i < argc ? argv[i] : JS_UNDEFINED, sc, BM_LET);
    }
    if (!J->thrown) {
        /* hoisting */
        for (int i = 0; i < fi->nvars; i++)
            if (scope_find(sc, fi->vars[i]) < 0)
                scope_declare(J, sc, fi->vars[i], JS_UNDEFINED, false);
        for (int i = 0; i < fi->nfuncs; i++) {
            jsval c = make_closure(J, fi->funcs[i], sc, NULL);
            scope_declare(J, sc, fi->funcs[i]->s, c, false);
        }
        if (fn->is_class && !fn->derived && fn->class_node)
            run_field_inits(J, f, thisv);
        if (J->thrown) {
            /* fall through */
        } else if (fi->expr_body) {
            result = eval(J, fi->body, sc);
        } else {
            enum completion c = exec(J, fi->body, sc);
            if (c == C_RETURN) {
                result = J->retval;
                J->retval = JS_UNDEFINED;
            }
        }
    }
    J->depth--;
    scope_release(J, sc);
    if (J->thrown)
        return JS_UNDEFINED;
    if (newtarget.t != JS_UNDEF && result.t != JS_OBJ)
        return thisv;                                   /* constructor call: `this` unless an object was returned */
    return result;
}

jsval construct(js *J, jsval fnv, int argc, jsval *argv)
{
    if (!js_is_function(fnv))
        return throw_type(J, "not a constructor");
    jsobj *f = fnv.u.o;
    if (f->cls == JC_PROXY)
        return call_function(J, f, JS_UNDEFINED, argc, argv, fnv);
    if (f->fn->fi && (f->fn->fi->is_arrow || f->fn->fi->is_method || f->fn->fi->is_gen || f->fn->fi->is_async) && !f->fn->is_class)
        return throw_type(J, "%s is not a constructor", f->fn->name ? f->fn->name->s : "function");
    jsval protov = get_prop(J, fnv, J->k_prototype);
    THROW_IF(J);
    if (f->fn->native && f->fn->is_ctor_native) {
        /* native constructors create their own object; `this` carries the prototype to use */
        jsobj *o = obj_new(J, JC_OBJECT, protov.t == JS_OBJ ? protov.u.o : J->object_proto);
        return call_function(J, f, js_objv(o), argc, argv, fnv);
    }
    if (f->fn->native)
        return throw_type(J, "%s is not a constructor", f->fn->name ? f->fn->name->s : "function");
    jsobj *o = obj_new(J, JC_OBJECT, protov.t == JS_OBJ ? protov.u.o : J->object_proto);
    jsval r = call_function(J, f, js_objv(o), argc, argv, fnv);
    return r;
}

jsval js_construct(js *J, jsval fn, int argc, jsval *argv, bool *threw)
{
    jsval r = construct(J, fn, argc, argv);
    if (threw)
        *threw = J->thrown;
    return r;
}

jsval js_call(js *J, jsval fn, jsval thisv, int argc, jsval *argv, bool *threw)
{
    jsval r;
    if (js_memory_exhausted(J)) {
        if (threw)
            *threw = false;
        return JS_UNDEFINED;
    }
    J->entries++;
    if (!js_is_function(fn))
        r = throw_type(J, "not a function");
    else
        r = call_function(J, fn.u.o, thisv, argc, argv, JS_UNDEFINED);
    J->entries--;
    if (threw)
        *threw = J->thrown;
    if (!J->thrown)
        js_run_microtasks(J);
    return r;
}

jsval fn_to_string(js *J, jsobj *f)
{
    struct func *fn = f->fn;
    if (fn && fn->fi && fn->fi->src)
        return js_strn(J, fn->fi->src, fn->fi->src_len);
    char b[200];
    snprintf(b, sizeof(b), "function %s() { [native code] }", fn && fn->name ? fn->name->s : "");
    return js_cstr(J, b);
}

/* ---- classes ----------------------------------------------------------------------------------------------- */

static jsval class_ctor_native(js *J, jsval thisv, int argc, jsval *argv, jsobj *callee)
{
    /* default constructor of a derived class: pass everything to the parent */
    struct func *fn = callee->fn;
    if (fn->parent_ctor) {
        jsval r = call_function(J, fn->parent_ctor, thisv, argc, argv, js_objv(callee));
        THROW_IF(J);
        if (r.t == JS_OBJ && r.u.o != thisv.u.o)
            return r;
    }
    run_field_inits(J, callee, thisv);
    return thisv;
}

static void run_field_inits(js *J, jsobj *ctor, jsval thisv)
{
    struct func *fn = ctor->fn;
    struct node *cn = fn->class_node;
    if (!cn)
        return;
    struct scope *fs = scope_new(J, fn->class_scope);
    fs->has_this = 1;
    fs->thisv = thisv;
    fs->func = ctor;
    for (int i = 0; i < cn->n && !J->thrown; i++) {
        struct node *m = cn->list[i];
        if (m->kind != PK_INIT || (m->flag & 2))
            continue;
        jsstr *key;
        if (m->flag & 1) {
            jsval k = eval(J, m->a, fn->class_scope);
            if (J->thrown) break;
            key = key_of(J, k);
        } else {
            key = m->a->t == N_NUM ? num_to_str(J, m->a->num) : m->a->s;
            if (key) key = js_intern_str(J, key);
        }
        if (J->thrown || !key)
            break;
        jsval v = m->b ? eval(J, m->b, fs) : JS_UNDEFINED;
        if (J->thrown)
            break;
        if (thisv.t == JS_OBJ) {
            struct prop *p = obj_find_own(thisv.u.o, key);
            if (p)
                p->v = v;
            else
                obj_add_prop(J, thisv.u.o, key, v, PF_ENUM | PF_WRITE);
        }
    }
    scope_release(J, fs);
}

static jsval eval_class(js *J, struct node *n, struct scope *sc)
{
    jsval parent = JS_UNDEFINED;
    jsobj *parent_proto = J->object_proto;
    bool derived = false;
    if (n->a) {
        parent = eval(J, n->a, sc);
        THROW_IF(J);
        if (parent.t == JS_NULL) {
            parent_proto = NULL;
        } else {
            if (!js_is_function(parent))
                return throw_type(J, "Class extends value is not a constructor or null");
            jsval pp = get_prop(J, parent, J->k_prototype);
            THROW_IF(J);
            parent_proto = pp.t == JS_OBJ ? pp.u.o : NULL;
            derived = true;
        }
    }
    struct scope *cs = scope_new(J, sc);                    /* class scope: the class name is visible inside */
    cs->captured = 1;
    scope_mark_captured(sc);
    jsobj *proto = obj_new(J, JC_OBJECT, parent_proto);
    struct node *ctor_node = NULL;
    for (int i = 0; i < n->n; i++) {
        struct node *m = n->list[i];
        if (!(m->flag & 2) && m->kind == PK_METHOD && !(m->flag & 1) && m->a->t == N_STR && !strcmp(m->a->s->s, "constructor"))
            ctor_node = m->b;
    }
    jsobj *ctor;
    if (ctor_node) {
        jsval cv = make_closure(J, ctor_node, cs, proto);
        ctor = cv.u.o;
        ctor->fn->fi->is_method = 0;
    } else {
        ctor = make_native(J, n->s ? n->s : J->k_empty, class_ctor_native, 0, NULL);
        ctor->fn->is_ctor_native = 1;
        obj_add_prop(J, ctor, J->k_prototype, js_objv(proto), 0);
    }
    ctor->fn->is_class = 1;
    ctor->fn->derived = derived;
    ctor->fn->home = proto;
    ctor->fn->parent_ctor = derived ? parent.u.o : NULL;
    ctor->fn->class_node = n;
    ctor->fn->class_scope = cs;
    ctor->fn->name = n->s;
    {
        struct prop *np = obj_find_own(ctor, J->k_name);
        if (np)
            np->v = js_strv(n->s ? n->s : J->k_empty);
    }
    if (derived)
        ctor->proto = parent.u.o;
    struct prop *pp = obj_find_own(ctor, J->k_prototype);
    if (pp)
        pp->v = js_objv(proto);
    else
        obj_add_prop(J, ctor, J->k_prototype, js_objv(proto), 0);
    struct prop *cp = obj_find_own(proto, J->k_constructor);
    if (cp)
        cp->v = js_objv(ctor);
    else
        obj_add_prop(J, proto, J->k_constructor, js_objv(ctor), PF_WRITE);
    if (n->s)
        scope_declare(J, cs, n->s, js_objv(ctor), false);
    for (int i = 0; i < n->n && !J->thrown; i++) {
        struct node *m = n->list[i];
        bool is_static = m->flag & 2;
        if (m->kind == PK_METHOD && m->b == ctor_node && !is_static && !(m->flag & 1) && m->a->t == N_STR &&
            !strcmp(m->a->s->s, "constructor"))
            continue;
        jsobj *target = is_static ? ctor : proto;
        jsstr *key;
        if (m->flag & 1) {
            jsval k = eval(J, m->a, cs);
            THROW_IF(J);
            key = key_of(J, k);
        } else {
            key = m->a->t == N_NUM ? num_to_str(J, m->a->num) : m->a->s;
            key = key ? js_intern_str(J, key) : NULL;
        }
        THROW_IF(J);
        if (!key)
            continue;
        if (m->kind == PK_INIT) {
            if (is_static) {
                struct scope *fs = scope_new(J, cs);
                fs->has_this = 1;
                fs->thisv = js_objv(ctor);
                fs->func = ctor;
                jsval v = m->b ? eval(J, m->b, fs) : JS_UNDEFINED;
                scope_release(J, fs);
                THROW_IF(J);
                obj_add_prop(J, ctor, key, v, PF_ENUM | PF_WRITE);
            }
            continue;
        }
        jsval f = make_closure(J, m->b, cs, target);
        struct prop *ex = obj_find_own(target, key);
        if (m->kind == PK_GET || m->kind == PK_SET) {
            if (!ex)
                ex = obj_add_prop(J, target, key, JS_UNDEFINED, PF_ACCESSOR);
            ex->flags = PF_ACCESSOR;
            if (m->kind == PK_GET)
                ex->get = f.u.o;
            else
                ex->set = f.u.o;
        } else if (ex) {
            ex->v = f;
            ex->flags = PF_WRITE;
            ex->get = ex->set = NULL;
        } else {
            obj_add_prop(J, target, key, f, PF_WRITE);
        }
    }
    THROW_IF(J);
    return js_objv(ctor);
}

/* ---- patterns ------------------------------------------------------------------------------------------------- */

static void declare_name(js *J, struct scope *sc, jsstr *name, jsval v, int mode)
{
    if (mode == BM_VAR) {
        struct scope *vs = var_scope(sc);
        for (struct scope *s = sc; s; s = s->parent) {         /* `var x = 1` in a nested block assigns the hoisted x */
            int i = scope_find(s, name);
            if (i >= 0) {
                s->vals[i] = v;
                return;
            }
            if (s == vs)
                break;
        }
        if (vs->obj)
            set_prop(J, js_objv(vs->obj), name, v);
        else
            scope_declare(J, vs, name, v, false);
    } else {
        scope_declare(J, sc, name, v, mode == BM_CONST);
    }
}

static bool iterate_values(js *J, jsval it, jsval **out, int *outn, int max);

static bool bind_pattern(js *J, struct node *pat, jsval v, struct scope *sc, int mode)
{
    switch (pat->t) {
    case N_IDENT:
        if (mode == BM_ASSIGN)
            assign_var(J, pat->s, v, sc);
        else
            declare_name(J, sc, pat->s, v, mode);
        return !J->thrown;
    case N_MEMBER: {
        jsval base = eval(J, pat->a, sc);
        if (J->thrown)
            return false;
        jsval nk;
        jsstr *k = member_key(J, pat, sc, &nk);
        if (J->thrown)
            return false;
        put_member(J, base, k, nk, v);
        return !J->thrown;
    }
    case N_ASSIGN: {
        if (v.t == JS_UNDEF) {
            v = eval(J, pat->b, sc);
            if (J->thrown)
                return false;
            if (pat->a->t == N_IDENT && v.t == JS_OBJ && v.u.o->cls == JC_FUNCTION && v.u.o->fn->fi && !v.u.o->fn->fi->name) {
                struct prop *np = obj_find_own(v.u.o, J->k_name);
                if (np) np->v = js_strv(pat->a->s);
            }
        }
        return bind_pattern(J, pat->a, v, sc, mode);
    }
    case N_ARRAY: {
        jsval *items = NULL;
        int cnt = 0;
        if (v.t == JS_UNDEF || v.t == JS_NULL) {
            throw_type(J, "%s is not iterable", v.t == JS_UNDEF ? "undefined" : "null");
            return false;
        }
        int need = pat->n;                                       /* an infinite generator must not be drained */
        for (int i = 0; i < pat->n; i++)
            if (pat->list[i]->t == N_SPREAD)
                need = -1;
        if (!iterate_values(J, v, &items, &cnt, need))
            return false;
        for (int i = 0; i < pat->n; i++) {
            struct node *e = pat->list[i];
            if (e->t == N_HOLE)
                continue;
            if (e->t == N_SPREAD) {
                jsobj *rest = js_array_new(J);
                for (int k = i; k < cnt; k++)
                    js_array_push(J, rest, items[k]);
                bool ok = bind_pattern(J, e->a, js_objv(rest), sc, mode);
                free(items);
                return ok;
            }
            if (!bind_pattern(J, e, i < cnt ? items[i] : JS_UNDEFINED, sc, mode)) {
                free(items);
                return false;
            }
        }
        free(items);
        return true;
    }
    case N_OBJECT: {
        if (v.t == JS_UNDEF || v.t == JS_NULL) {
            throw_type(J, "Cannot destructure '%s' as it is %s", v.t == JS_UNDEF ? "undefined" : "null",
                       v.t == JS_UNDEF ? "undefined" : "null");
            return false;
        }
        jsobj *used = js_array_new(J);
        for (int i = 0; i < pat->n; i++) {
            struct node *p = pat->list[i];
            if (p->kind == PK_SPREAD) {
                jsobj *rest = js_object_new(J);
                if (v.t == JS_OBJ) {
                    jsobj *ks = js_array_new(J);
                    own_keys(J, v.u.o, ks, true);
                    for (uint32_t k = 0; k < ks->len; k++) {
                        bool skip = false;
                        for (uint32_t u = 0; u < used->len; u++)
                            if (js_strict_equals(used->items[u], ks->items[k]))
                                skip = true;
                        if (!skip) {
                            jsstr *kk = js_intern_str(J, ks->items[k].u.s);
                            set_prop(J, js_objv(rest), kk, get_prop(J, v, kk));
                        }
                    }
                }
                return bind_pattern(J, p->b, js_objv(rest), sc, mode);
            }
            jsstr *key;
            if (p->flag & 1) {
                jsval kv = eval(J, p->a, sc);
                if (J->thrown)
                    return false;
                key = key_of(J, kv);
            } else {
                key = p->a->t == N_NUM ? num_to_str(J, p->a->num) : p->a->s;
                if (key)
                    key = js_intern_str(J, key);
            }
            if (J->thrown || !key)
                return false;
            js_array_push(J, used, js_strv(key));
            jsval pv = get_prop(J, v, key);
            if (J->thrown)
                return false;
            if (!bind_pattern(J, p->b, pv, sc, mode))
                return false;
        }
        return true;
    }
    default:
        throw_type(J, "invalid destructuring target");
        return false;
    }
}

/* Collect the values of an iterable (array, string, Set, Map entries). */
static bool iterate_values(js *J, jsval it, jsval **out, int *outn, int max)
{
    
    jsval *v = NULL;
    int n = 0;
    if (it.t == JS_STR) {
        v = malloc(sizeof(jsval) * (it.u.s->len + 1));
        /* iterate by code point */
        for (uint32_t i = 0; i < it.u.s->len;) {
            uint8_t c = (uint8_t)it.u.s->s[i];
            uint32_t l = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
            if (i + l > it.u.s->len)
                l = it.u.s->len - i;
            v[n++] = js_strn(J, it.u.s->s + i, l);
            i += l;
        }
    } else if (it.t == JS_OBJ && (it.u.o->cls == JC_ARRAY || it.u.o->cls == JC_SET)) {
        v = malloc(sizeof(jsval) * (it.u.o->len + 1));
        for (uint32_t i = 0; i < it.u.o->len; i++)
            v[n++] = it.u.o->items[i];
    } else if (it.t == JS_OBJ && it.u.o->cls == JC_MAP) {
        v = malloc(sizeof(jsval) * (it.u.o->len + 1));
        for (uint32_t i = 0; i < it.u.o->len; i++) {
            jsobj *pair = js_array_new(J);
            js_array_push(J, pair, it.u.o->items[i]);
            js_array_push(J, pair, it.u.o->items2[i]);
            v[n++] = js_objv(pair);
        }
    } else if (it.t == JS_OBJ && js_has_iterator(J, it)) {
        return gen_collect(J, it, out, outn, max);
    } else if (it.t == JS_OBJ) {
        /* array-likes with a numeric length (arguments, NodeLists from the DOM) */
        jsval lv = get_prop(J, it, J->k_length);
        if (J->thrown)
            return false;
        if (lv.t != JS_NUM) {
            throw_type(J, "object is not iterable");
            return false;
        }
        uint32_t len = (uint32_t)lv.u.n;
        v = malloc(sizeof(jsval) * (len + 1));
        for (uint32_t i = 0; i < len; i++) {
            v[n++] = get_prop(J, it, js_intern_str(J, num_to_str(J, (double)i)));
            if (J->thrown) {
                free(v);
                return false;
            }
        }
    } else {
        throw_type(J, "value is not iterable");
        return false;
    }
    *out = v;
    *outn = n;
    return true;
}

/* ---- expressions ---------------------------------------------------------------------------------------------- */

static jsval eval_template(js *J, struct node *n, struct scope *sc)
{
    jsstr *acc = n->list[0]->s;
    for (int i = 1; i < n->n; i += 2) {
        jsval e = eval(J, n->list[i], sc);
        THROW_IF(J);
        jsstr *s = to_jsstr(J, e);
        THROW_IF(J);
        acc = str_concat(J, str_concat(J, acc, s), n->list[i + 1]->s);
    }
    return js_strv(acc);
}

static jsval eval_object(js *J, struct node *n, struct scope *sc)
{
    jsobj *o = js_object_new(J);
    for (int i = 0; i < n->n; i++) {
        struct node *p = n->list[i];
        if (p->kind == PK_SPREAD) {
            jsval src = eval(J, p->b, sc);
            THROW_IF(J);
            if (src.t == JS_OBJ) {
                jsobj *ks = js_array_new(J);
                own_keys(J, src.u.o, ks, true);
                for (uint32_t k = 0; k < ks->len; k++) {
                    jsstr *kk = js_intern_str(J, ks->items[k].u.s);
                    jsval v = get_prop(J, src, kk);
                    THROW_IF(J);
                    set_prop(J, js_objv(o), kk, v);
                }
            } else if (src.t == JS_STR) {
                for (uint32_t k = 0; k < src.u.s->len; k++)
                    set_prop(J, js_objv(o), js_intern_str(J, num_to_str(J, (double)k)), js_strn(J, src.u.s->s + k, 1));
            }
            continue;
        }
        jsstr *key;
        if (p->flag & 1) {
            jsval kv = eval(J, p->a, sc);
            THROW_IF(J);
            key = key_of(J, kv);
        } else {
            key = p->a->t == N_NUM ? num_to_str(J, p->a->num) : p->a->s;
            key = key ? js_intern_str(J, key) : NULL;
        }
        THROW_IF(J);
        if (!key)
            continue;
        if (p->kind == PK_GET || p->kind == PK_SET) {
            jsval f = make_closure(J, p->b, sc, o);
            struct prop *ex = obj_find_own(o, key);
            if (!ex)
                ex = obj_add_prop(J, o, key, JS_UNDEFINED, PF_ACCESSOR | PF_ENUM);
            ex->flags = PF_ACCESSOR | PF_ENUM;
            if (p->kind == PK_GET)
                ex->get = f.u.o;
            else
                ex->set = f.u.o;
            continue;
        }
        jsval v;
        if (p->kind == PK_METHOD) {
            v = make_closure(J, p->b, sc, o);
        } else {
            v = eval(J, p->b, sc);
            THROW_IF(J);
            if (v.t == JS_OBJ && v.u.o->cls == JC_FUNCTION && v.u.o->fn->fi && !v.u.o->fn->fi->name &&
                (p->b->t == N_FUNC || p->b->t == N_CLASS)) {
                struct prop *np = obj_find_own(v.u.o, J->k_name);
                if (np && np->v.t == JS_STR && !np->v.u.s->len)
                    np->v = js_strv(key);
            }
        }
        struct prop *ex = obj_find_own(o, key);
        if (ex) {
            ex->v = v;
            ex->flags = PF_ENUM | PF_WRITE;
            ex->get = ex->set = NULL;
        } else if (o->cls == JC_OBJECT) {
            obj_add_prop(J, o, key, v, PF_ENUM | PF_WRITE);
        }
    }
    return js_objv(o);
}

static jsval eval_array(js *J, struct node *n, struct scope *sc)
{
    jsobj *a = js_array_new(J);
    for (int i = 0; i < n->n; i++) {
        struct node *e = n->list[i];
        if (e->t == N_HOLE) {
            js_array_push(J, a, JS_UNDEFINED);
            continue;
        }
        if (e->t == N_SPREAD) {
            jsval s = eval(J, e->a, sc);
            THROW_IF(J);
            jsval *items;
            int cnt;
            if (!iterate_values(J, s, &items, &cnt, -1))
                return JS_UNDEFINED;
            for (int k = 0; k < cnt; k++)
                js_array_push(J, a, items[k]);
            free(items);
            continue;
        }
        jsval v = eval(J, e, sc);
        THROW_IF(J);
        js_array_push(J, a, v);
    }
    return js_objv(a);
}

static jsval eval_super_call(js *J, struct node *n, struct scope *sc)
{
    struct scope *fs = func_scope_of(sc);
    jsobj *ctor = NULL;
    for (struct scope *s = sc; s; s = s->parent)
        if (s->func && s->has_this) { ctor = s->func; break; }
    if (!fs || !ctor || !ctor->fn->is_class || !ctor->fn->parent_ctor)
        return js_throw_error(J, "SyntaxError", "'super' keyword unexpected here");
    jsval *args;
    int argc;
    eval_args(J, n, sc, &args, &argc);
    THROW_IF(J);
    jsobj *parent = ctor->fn->parent_ctor;
    jsval r;
    if (parent->fn->native && !parent->fn->is_ctor_native) {
        r = call_function(J, parent, fs->thisv, argc, args, js_objv(ctor));
    } else {
        r = call_function(J, parent, fs->thisv, argc, args, js_objv(ctor));
    }
    free(args);
    THROW_IF(J);
    if (r.t == JS_OBJ && fs->thisv.t == JS_OBJ && r.u.o != fs->thisv.u.o) {
        /* the parent constructor produced its own object (native parents such as Error/Array): adopt it */
        r.u.o->proto = fs->thisv.u.o->proto;
        fs->thisv = r;
    }
    run_field_inits(J, ctor, fs->thisv);
    return JS_UNDEFINED;
}

static jsobj *super_base(js *J, struct scope *sc)
{
    for (struct scope *s = sc; s; s = s->parent)
        if (s->func && s->func->fn->home)
            return s->func->fn->home->proto;
    (void)J;
    return NULL;
}

static jsval eval_call(js *J, struct node *n, struct scope *sc)
{
    struct node *c = n->a;
    jsval thisv = JS_UNDEFINED, fnv;
    if (c->t == N_SUPER)
        return eval_super_call(J, n, sc);
    if (c->t == N_MEMBER) {
        jsval base;
        jsobj *sup = NULL;
        if (c->a->t == N_SUPER) {
            sup = super_base(J, sc);
            base = this_of(J, sc);
        } else {
            base = eval(J, c->a, sc);
            THROW_IF(J);
        }
        THROW_IF(J);
        if (J->short_circuit) {
            if (n->kind & 8)
                J->short_circuit = false;
            return JS_UNDEFINED;
        }
        if ((c->flag & 2) && (base.t == JS_UNDEF || base.t == JS_NULL)) {
            if (!(n->kind & 8))
                J->short_circuit = true;
            return JS_UNDEFINED;
        }
        jsval nk;
        jsstr *k = member_key(J, c, sc, &nk);
        THROW_IF(J);
        if (sup) {
            jsstr *kk = k ? k : js_intern_str(J, num_to_str(J, nk.u.n));
            fnv = JS_UNDEFINED;
            for (jsobj *o = sup; o; o = o->proto) {
                struct prop *p = obj_find_own(o, kk);
                if (p) {
                    fnv = (p->flags & PF_ACCESSOR) ? (p->get ? call_function(J, p->get, base, 0, NULL, JS_UNDEFINED) : JS_UNDEFINED) : p->v;
                    break;
                }
            }
        } else {
            fnv = get_member(J, base, k, nk);
        }
        THROW_IF(J);
        thisv = base;
    } else {
        fnv = eval(J, c, sc);
        THROW_IF(J);
        if (J->short_circuit) {
            if (n->kind & 8)
                J->short_circuit = false;
            return JS_UNDEFINED;
        }
    }
    if ((n->flag & 2) && (fnv.t == JS_UNDEF || fnv.t == JS_NULL)) {
        if (!(n->kind & 8))
            J->short_circuit = true;
        return JS_UNDEFINED;
    }
    if (!js_is_function(fnv)) {
        char nm[160];
        callee_name(c, nm, sizeof(nm));
        return throw_type(J, "%s is not a function", nm);
    }
    jsval *args;
    int argc;
    eval_args(J, n, sc, &args, &argc);
    THROW_IF(J);
    jsval r = call_function(J, fnv.u.o, thisv, argc, args, JS_UNDEFINED);
    free(args);
    return r;
}

static jsval eval_new(js *J, struct node *n, struct scope *sc)
{
    jsval f = eval(J, n->a, sc);
    THROW_IF(J);
    if (!js_is_function(f)) {
        char nm[160];
        callee_name(n->a, nm, sizeof(nm));
        return throw_type(J, "%s is not a constructor", nm);
    }
    jsval *args;
    int argc;
    eval_args(J, n, sc, &args, &argc);
    THROW_IF(J);
    jsval r = construct(J, f, argc, args);
    free(args);
    return r;
}

static jsval eval_update(js *J, struct node *n, struct scope *sc)
{
    struct node *t = n->a;
    double delta = n->op == OP_INC ? 1 : -1;
    if (t->t == N_IDENT) {
        bool found;
        jsval old = lookup(J, t->s, sc, &found);
        THROW_IF(J);
        if (!found)
            return js_throw_error(J, "ReferenceError", "%s is not defined", t->s->s);
        double x = to_number(J, old);
        THROW_IF(J);
        assign_var(J, t->s, js_num(x + delta), sc);
        THROW_IF(J);
        return js_num(n->flag ? x + delta : x);
    }
    if (t->t == N_MEMBER) {
        jsval base = eval(J, t->a, sc);
        THROW_IF(J);
        jsval nk;
        jsstr *k = member_key(J, t, sc, &nk);
        THROW_IF(J);
        jsval old = get_member(J, base, k, nk);
        THROW_IF(J);
        double x = to_number(J, old);
        THROW_IF(J);
        put_member(J, base, k, nk, js_num(x + delta));
        THROW_IF(J);
        return js_num(n->flag ? x + delta : x);
    }
    return js_throw_error(J, "SyntaxError", "Invalid left-hand side expression in postfix operation");
}

static jsval eval_assign(js *J, struct node *n, struct scope *sc)
{
    struct node *t = n->a;
    int op = n->op;
    if (op == OP_ASSIGN) {
        if (t->t == N_IDENT) {
            jsval v = eval(J, n->b, sc);
            THROW_IF(J);
            if (v.t == JS_OBJ && v.u.o->cls == JC_FUNCTION && v.u.o->fn->fi && !v.u.o->fn->fi->name &&
                (n->b->t == N_FUNC || n->b->t == N_CLASS)) {
                struct prop *np = obj_find_own(v.u.o, J->k_name);
                if (np && np->v.t == JS_STR && !np->v.u.s->len)
                    np->v = js_strv(t->s);
            }
            assign_var(J, t->s, v, sc);
            THROW_IF(J);
            return v;
        }
        if (t->t == N_MEMBER) {
            jsval base;
            if (t->a->t == N_SUPER)
                base = this_of(J, sc);
            else
                base = eval(J, t->a, sc);
            THROW_IF(J);
            jsval nk;
            jsstr *k = member_key(J, t, sc, &nk);
            THROW_IF(J);
            jsval v = eval(J, n->b, sc);
            THROW_IF(J);
            put_member(J, base, k, nk, v);
            THROW_IF(J);
            return v;
        }
        jsval v = eval(J, n->b, sc);
        THROW_IF(J);
        bind_pattern(J, t, v, sc, BM_ASSIGN);
        THROW_IF(J);
        return v;
    }
    /* compound / logical assignment */
    jsval base = JS_UNDEFINED, nk = JS_UNDEFINED, cur;
    jsstr *k = NULL;
    if (t->t == N_IDENT) {
        bool found;
        cur = lookup(J, t->s, sc, &found);
        THROW_IF(J);
        if (!found && !(op == OP_AND || op == OP_OR || op == OP_NULLISH))
            return js_throw_error(J, "ReferenceError", "%s is not defined", t->s->s);
    } else {
        base = eval(J, t->a, sc);
        THROW_IF(J);
        k = member_key(J, t, sc, &nk);
        THROW_IF(J);
        cur = get_member(J, base, k, nk);
        THROW_IF(J);
    }
    jsval result;
    if (op == OP_AND || op == OP_OR || op == OP_NULLISH) {
        bool assign = op == OP_AND ? js_truthy(cur) : op == OP_OR ? !js_truthy(cur) : (cur.t == JS_UNDEF || cur.t == JS_NULL);
        if (!assign)
            return cur;
        result = eval(J, n->b, sc);
        THROW_IF(J);
    } else {
        jsval rhs = eval(J, n->b, sc);
        THROW_IF(J);
        result = binary_op(J, op, cur, rhs);
        THROW_IF(J);
    }
    if (t->t == N_IDENT)
        assign_var(J, t->s, result, sc);
    else
        put_member(J, base, k, nk, result);
    THROW_IF(J);
    return result;
}

static jsval eval_unary(js *J, struct node *n, struct scope *sc)
{
    if (n->op == OP_TYPEOF && n->a->t == N_IDENT) {
        bool found;
        jsval v = lookup(J, n->a->s, sc, &found);
        THROW_IF(J);
        return js_cstr(J, found ? type_of(v) : "undefined");
    }
    if (n->op == OP_DELETE) {
        struct node *t = n->a;
        if (t->t != N_MEMBER)
            return js_bool(true);
        jsval base = eval(J, t->a, sc);
        THROW_IF(J);
        jsval nk;
        jsstr *k = member_key(J, t, sc, &nk);
        THROW_IF(J);
        if (!k)
            k = js_intern_str(J, num_to_str(J, nk.u.n));
        if (base.t != JS_OBJ)
            return js_bool(true);
        jsobj *o = base.u.o;
        uint32_t idx;
        if (o->cls == JC_PROXY) {
            jsval trap = get_prop(J, js_objv((jsobj *)o->host), js_intern(J, "deleteProperty", 14));
            THROW_IF(J);
            if (js_is_function(trap)) {
                jsval args[2] = { o->prim, js_strv(k) };
                jsval r = call_function(J, trap.u.o, js_objv((jsobj *)o->host), 2, args, JS_UNDEFINED);
                return js_bool(!J->thrown && js_truthy(r));
            }
            o = o->prim.t == JS_OBJ ? o->prim.u.o : o;
        }
        if (o->flags & OF_FROZEN)
            return js_bool(false);
        if (o->cls == JC_ARRAY && is_index_key(k, &idx)) {
            if (idx < o->len)
                o->items[idx] = JS_UNDEFINED;
            return js_bool(true);
        }
        obj_delete(o, k);
        return js_bool(true);
    }
    jsval v = eval(J, n->a, sc);
    THROW_IF(J);
    switch (n->op) {
    case OP_NOT: return js_bool(!js_truthy(v));
    case OP_NEG: { double d = to_number(J, v); THROW_IF(J); return js_num(-d); }
    case OP_POS: { double d = to_number(J, v); THROW_IF(J); return js_num(d); }
    case OP_BNOT: { double d = to_number(J, v); THROW_IF(J); return js_num((double)(~to_int32(d))); }
    case OP_TYPEOF: return js_cstr(J, type_of(v));
    case OP_VOID: return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

static jsval eval(js *J, struct node *n, struct scope *sc)
{
    STEP(J);
    switch (n->t) {
    case N_NUM: return js_num(n->num);
    case N_STR: return js_strv(n->s);
    case N_TEMPLATE: return eval_template(J, n, sc);
    case N_REGEX: return js_regexp_literal(J, n->s, n->a->s);
    case N_TAGGED: {
        struct node *c = n->a;
        jsval thisv = JS_UNDEFINED, fnv;
        if (c->t == N_MEMBER && c->a->t != N_SUPER) {
            jsval base = eval(J, c->a, sc);
            THROW_IF(J);
            jsval nk;
            jsstr *k = member_key(J, c, sc, &nk);
            THROW_IF(J);
            fnv = get_member(J, base, k, nk);
            thisv = base;
        } else {
            fnv = eval(J, c, sc);
        }
        THROW_IF(J);
        if (!js_is_function(fnv))
            return throw_type(J, "tag is not a function");
        struct node *tp = n->b;
        int nvals = tp->n / 2;
        jsobj *strs = js_array_new(J), *raws = js_array_new(J);
        for (int i = 0; i < tp->n; i += 2) {
            js_array_push(J, strs, js_strv(tp->list[i]->s));
            js_array_push(J, raws, js_strv(tp->list[i]->s));
        }
        obj_add_prop(J, strs, js_intern(J, "raw", 3), js_objv(raws), 0);
        jsval *args = malloc(sizeof(jsval) * (size_t)(nvals + 1));
        args[0] = js_objv(strs);
        for (int i = 0; i < nvals; i++) {
            args[i + 1] = eval(J, tp->list[2 * i + 1], sc);
            if (J->thrown) { free(args); return JS_UNDEFINED; }
        }
        jsval r = call_function(J, fnv.u.o, thisv, nvals + 1, args, JS_UNDEFINED);
        free(args);
        return r;
    }
    case N_AWAIT: {
        jsval v = eval(J, n->a, sc);
        THROW_IF(J);
        return co_yield(J, v);
    }
    case N_YIELD: {
        if (n->flag) {                                         /* yield* iterable */
            jsval src = eval(J, n->a, sc);
            THROW_IF(J);
            jsval *items;
            int cnt;
            if (js_has_iterator(J, src)) {
                jsval itf = get_prop(J, src, js_intern(J, "@@iterator", 10));
                jsval it = call_function(J, itf.u.o, src, 0, NULL, JS_UNDEFINED);
                THROW_IF(J);
                jsval next = get_prop(J, it, J->k_next);
                THROW_IF(J);
                jsval sent = JS_UNDEFINED;
                for (;;) {
                    jsval r = js_is_function(next) ? call_function(J, next.u.o, it, 1, &sent, JS_UNDEFINED) : JS_UNDEFINED;
                    THROW_IF(J);
                    if (r.t != JS_OBJ)
                        return throw_type(J, "iterator result is not an object");
                    jsval done = get_prop(J, r, J->k_done), val = get_prop(J, r, J->k_value);
                    THROW_IF(J);
                    if (js_truthy(done))
                        return val;
                    sent = co_yield(J, val);
                    THROW_IF(J);
                }
            }
            if (!iterate_values(J, src, &items, &cnt, -1))
                return JS_UNDEFINED;
            for (int i = 0; i < cnt; i++) {
                co_yield(J, items[i]);
                if (J->thrown) {
                    free(items);
                    return JS_UNDEFINED;
                }
            }
            free(items);
            return JS_UNDEFINED;
        }
        jsval v = n->a ? eval(J, n->a, sc) : JS_UNDEFINED;
        THROW_IF(J);
        return co_yield(J, v);
    }
    case N_IDENT: {
        bool found;
        jsval v = lookup(J, n->s, sc, &found);
        THROW_IF(J);
        if (!found) {
            if (n->s == J->k_undefined)
                return JS_UNDEFINED;
            return js_throw_error(J, "ReferenceError", "%s is not defined", n->s->s);
        }
        return v;
    }
    case N_THIS: return this_of(J, sc);
    case N_LIT: return n->kind == 1 ? JS_NULLV : js_bool(n->kind == 2);
    case N_ARRAY: return eval_array(J, n, sc);
    case N_OBJECT: return eval_object(J, n, sc);
    case N_FUNC: {
        if (n->fi->name && !n->fi->is_arrow && !n->fi->is_method) {
            /* a named function expression sees its own name */
            struct scope *fs = scope_new(J, sc);
            fs->captured = 1;
            scope_mark_captured(sc);
            jsval f = make_closure(J, n, fs, NULL);
            scope_declare(J, fs, n->fi->name, f, false);
            return f;
        }
        return make_closure(J, n, sc, NULL);
    }
    case N_CLASS: return eval_class(J, n, sc);
    case N_UNARY: return eval_unary(J, n, sc);
    case N_UPDATE: return eval_update(J, n, sc);
    case N_BINARY: {
        jsval a = eval(J, n->a, sc);
        THROW_IF(J);
        jsval b = eval(J, n->b, sc);
        THROW_IF(J);
        return binary_op(J, n->op, a, b);
    }
    case N_LOGICAL: {
        jsval a = eval(J, n->a, sc);
        THROW_IF(J);
        if (n->op == OP_AND)
            return js_truthy(a) ? eval(J, n->b, sc) : a;
        if (n->op == OP_OR)
            return js_truthy(a) ? a : eval(J, n->b, sc);
        return (a.t == JS_UNDEF || a.t == JS_NULL) ? eval(J, n->b, sc) : a;
    }
    case N_ASSIGN: return eval_assign(J, n, sc);
    case N_COND: {
        jsval c = eval(J, n->a, sc);
        THROW_IF(J);
        return eval(J, js_truthy(c) ? n->b : n->c, sc);
    }
    case N_CALL: return eval_call(J, n, sc);
    case N_NEW: return eval_new(J, n, sc);
    case N_MEMBER: {
        jsval base;
        jsobj *sup = NULL;
        if (n->a->t == N_SUPER) {
            sup = super_base(J, sc);
            base = this_of(J, sc);
        } else {
            base = eval(J, n->a, sc);
        }
        THROW_IF(J);
        if (J->short_circuit) {
            if (n->kind & 8)
                J->short_circuit = false;
            return JS_UNDEFINED;
        }
        if ((n->flag & 2) && (base.t == JS_UNDEF || base.t == JS_NULL)) {
            if (!(n->kind & 8))
                J->short_circuit = true;
            return JS_UNDEFINED;
        }
        jsval nk;
        jsstr *k = member_key(J, n, sc, &nk);
        THROW_IF(J);
        if (sup) {
            jsstr *kk = k ? k : js_intern_str(J, num_to_str(J, nk.u.n));
            for (jsobj *o = sup; o; o = o->proto) {
                struct prop *p = obj_find_own(o, kk);
                if (p)
                    return (p->flags & PF_ACCESSOR) ? (p->get ? call_function(J, p->get, base, 0, NULL, JS_UNDEFINED) : JS_UNDEFINED) : p->v;
            }
            return JS_UNDEFINED;
        }
        return get_member(J, base, k, nk);
    }
    case N_SEQ: {
        jsval v = JS_UNDEFINED;
        for (int i = 0; i < n->n; i++) {
            v = eval(J, n->list[i], sc);
            THROW_IF(J);
        }
        return v;
    }
    case N_SPREAD:
        return js_throw_error(J, "SyntaxError", "unexpected spread");
    case N_SUPER:
        return js_throw_error(J, "SyntaxError", "'super' keyword unexpected here");
    default:
        return js_throw_error(J, "SyntaxError", "cannot evaluate node %d", n->t);
    }
}

/* ---- statements ----------------------------------------------------------------------------------------------- */

static enum completion exec_list(js *J, struct node **l, int n, struct scope *sc)
{
    for (int i = 0; i < n; i++) {
        enum completion c = exec(J, l[i], sc);
        if (J->thrown || c != C_NORMAL)
            return c;
    }
    return C_NORMAL;
}

/* A loop body finished with completion c: should the loop stop? Sets *out to the completion to propagate. */
static bool loop_ctl(js *J, enum completion c, jsstr *own_label, enum completion *out)
{
    *out = C_NORMAL;
    if (c == C_BREAK) {
        if (!J->label || J->label == own_label) {
            J->label = NULL;
            return true;
        }
        *out = C_BREAK;
        return true;
    }
    if (c == C_CONTINUE) {
        if (!J->label || J->label == own_label) {
            J->label = NULL;
            return false;
        }
        *out = C_CONTINUE;
        return true;
    }
    if (c == C_RETURN) {
        *out = C_RETURN;
        return true;
    }
    return false;
}

static struct scope *copy_scope(js *J, struct scope *s)
{
    struct scope *c = scope_new(J, s->parent);
    for (uint32_t i = 0; i < s->n; i++)
        scope_declare(J, c, s->names[i], s->vals[i], s->fl[i]);
    c->captured = 0;
    return c;
}

static enum completion exec_loop_body(js *J, struct node *body, struct scope *sc)
{
    return exec(J, body, sc);
}

static enum completion exec_for(js *J, struct node *n, struct scope *sc, jsstr *label)
{
    struct scope *ls = sc;
    bool per_iter = n->flag;
    if (per_iter) {
        ls = scope_new(J, sc);
    }
    enum completion res = C_NORMAL;
    if (n->a) {
        if (n->a->t == N_VAR)
            exec(J, n->a, ls);
        else
            eval(J, n->a, ls);
    }
    if (J->thrown)
        goto done;
    for (;;) {
        if (n->b) {
            jsval t = eval(J, n->b, ls);
            if (J->thrown)
                goto done;
            if (!js_truthy(t))
                break;
        }
        if (J->budget && ++J->steps > J->budget) {
            abort_run(J, "budget");
            goto done;
        }
        enum completion c = exec_loop_body(J, n->d, ls);
        if (J->thrown)
            goto done;
        enum completion out;
        if (loop_ctl(J, c, label, &out)) {
            res = out;
            break;
        }
        if (per_iter) {                                 /* fresh bindings for closures created in the body */
            struct scope *next = copy_scope(J, ls);
            if (!ls->captured)
                scope_release(J, ls);
            ls = next;
        }
        if (n->c) {
            eval(J, n->c, ls);
            if (J->thrown)
                goto done;
        }
    }
done:
    if (per_iter && !ls->captured)
        scope_release(J, ls);
    return res;
}

/* for-of over an object that follows the iterator protocol (generators, custom iterables): one value at a time, so that
 * `break` stops infinite generators; the iterator is closed on an early exit. */
static enum completion exec_forof_iter(js *J, struct node *n, struct scope *sc, jsstr *label, jsval coll)
{
    jsval itf = get_prop(J, coll, js_intern(J, "@@iterator", 10));
    if (J->thrown)
        return C_NORMAL;
    jsval it = call_function(J, itf.u.o, coll, 0, NULL, JS_UNDEFINED);
    if (J->thrown)
        return C_NORMAL;
    jsval next = get_prop(J, it, J->k_next);
    if (J->thrown)
        return C_NORMAL;
    if (!js_is_function(next)) {
        throw_type(J, "iterator.next is not a function");
        return C_NORMAL;
    }
    struct node *decl = n->a;
    bool lexical = decl->t == N_VAR && decl->kind != VK_VAR;
    enum completion res = C_NORMAL;
    for (;;) {
        jsval r = call_function(J, next.u.o, it, 0, NULL, JS_UNDEFINED);
        if (J->thrown)
            return C_NORMAL;
        if (r.t != JS_OBJ) {
            throw_type(J, "iterator result is not an object");
            return C_NORMAL;
        }
        jsval done = get_prop(J, r, J->k_done);
        if (J->thrown)
            return C_NORMAL;
        if (js_truthy(done))
            break;
        jsval val = get_prop(J, r, J->k_value);
        if (J->thrown)
            return C_NORMAL;
        if (n->kind == 1) {                                      /* for await: await each value */
            val = co_yield(J, val);
            if (J->thrown)
                return C_NORMAL;
        }
        struct scope *is = lexical ? scope_new(J, sc) : sc;
        if (decl->t == N_VAR)
            bind_pattern(J, decl->list[0]->a, val, is, decl->kind == VK_VAR ? BM_VAR : decl->kind == VK_LET ? BM_LET : BM_CONST);
        else
            bind_pattern(J, decl, val, is, BM_ASSIGN);
        enum completion c = J->thrown ? C_NORMAL : exec_loop_body(J, n->c, is);
        if (lexical && !is->captured)
            scope_release(J, is);
        enum completion out;
        bool stop = J->thrown || loop_ctl(J, c, label, &out);
        if (stop) {
            if (!J->thrown)
                res = out;
            /* close the iterator (errors from return() are ignored when unwinding from an exception) */
            bool had = J->thrown;
            jsval saved_exc = J->exc;
            jsval saved_ret = J->retval;
            jsstr *saved_label = J->label;
            bool unwind = J->gen_unwind;
            J->thrown = false;
            J->gen_unwind = false;
            jsval ret = get_prop(J, it, js_intern(J, "return", 6));
            if (!J->thrown && js_is_function(ret))
                call_function(J, ret.u.o, it, 0, NULL, JS_UNDEFINED);
            if (had) {
                J->thrown = true;
                J->exc = saved_exc;
                J->gen_unwind = unwind;
            } else if (J->thrown) {
                /* an exception from return() propagates */
            } else {
                J->retval = saved_ret;
                J->label = saved_label;
            }
            return res;
        }
        if (J->budget && ++J->steps > J->budget) {
            abort_run(J, "budget");
            return C_NORMAL;
        }
    }
    return res;
}

static enum completion exec_forin_of(js *J, struct node *n, struct scope *sc, jsstr *label)
{
    bool is_of = n->t == N_FOROF;
    jsval coll = eval(J, n->b, sc);
    if (J->thrown)
        return C_NORMAL;
    if (is_of && coll.t == JS_OBJ && coll.u.o->cls != JC_ARRAY && coll.u.o->cls != JC_SET && coll.u.o->cls != JC_MAP &&
        js_has_iterator(J, coll))
        return exec_forof_iter(J, n, sc, label, coll);
    jsval *items = NULL;
    int cnt = 0;
    if (is_of) {
        if (coll.t == JS_UNDEF || coll.t == JS_NULL) {
            throw_type(J, "%s is not iterable", coll.t == JS_UNDEF ? "undefined" : "null");
            return C_NORMAL;
        }
        if (!iterate_values(J, coll, &items, &cnt, -1))
            return C_NORMAL;
    } else {
        if (coll.t == JS_UNDEF || coll.t == JS_NULL)
            return C_NORMAL;
        jsobj *keys = js_array_new(J);
        if (coll.t == JS_OBJ) {
            for (jsobj *o = coll.u.o; o; o = o->proto) {
                jsobj *ks = js_array_new(J);
                own_keys(J, o, ks, true);
                for (uint32_t i = 0; i < ks->len; i++) {
                    bool dup = false;
                    for (uint32_t k = 0; k < keys->len && !dup; k++)
                        dup = js_strict_equals(keys->items[k], ks->items[i]);
                    if (!dup)
                        js_array_push(J, keys, ks->items[i]);
                }
            }
        } else if (coll.t == JS_STR) {
            for (uint32_t i = 0; i < coll.u.s->len; i++)
                js_array_push(J, keys, js_strv(num_to_str(J, (double)i)));
        }
        items = malloc(sizeof(jsval) * (keys->len + 1));
        cnt = (int)keys->len;
        if (keys->len)
            memcpy(items, keys->items, sizeof(jsval) * keys->len);
    }
    enum completion res = C_NORMAL;
    struct node *decl = n->a;
    bool lexical = decl->t == N_VAR && decl->kind != VK_VAR;
    for (int i = 0; i < cnt; i++) {
        if (is_of && n->kind == 1) {                              /* for await over a sync iterable: await each value */
            items[i] = co_yield(J, items[i]);
            if (J->thrown)
                break;
        }
        struct scope *is = sc;
        if (lexical)
            is = scope_new(J, sc);
        if (decl->t == N_VAR)
            bind_pattern(J, decl->list[0]->a, items[i], is, decl->kind == VK_VAR ? BM_VAR : decl->kind == VK_LET ? BM_LET : BM_CONST);
        else
            bind_pattern(J, decl, items[i], is, BM_ASSIGN);
        enum completion c = J->thrown ? C_NORMAL : exec_loop_body(J, n->c, is);
        if (lexical && !is->captured)
            scope_release(J, is);
        if (J->thrown)
            break;
        enum completion out;
        if (loop_ctl(J, c, label, &out)) {
            res = out;
            break;
        }
        if (J->budget && ++J->steps > J->budget) {
            abort_run(J, "budget");
            break;
        }
    }
    free(items);
    return res;
}

static enum completion exec_switch(js *J, struct node *n, struct scope *sc)
{
    jsval d = eval(J, n->a, sc);
    if (J->thrown)
        return C_NORMAL;
    struct scope *bs = n->flag ? scope_new(J, sc) : sc;
    int start = -1;
    for (int i = 0; i < n->n && start < 0; i++) {
        struct node *c = n->list[i];
        if (!c->a)
            continue;
        jsval t = eval(J, c->a, bs);
        if (J->thrown)
            goto out;
        if (js_strict_equals(d, t))
            start = i;
    }
    if (start < 0)
        for (int i = 0; i < n->n; i++)
            if (!n->list[i]->a)
                start = i;
    enum completion res = C_NORMAL;
    if (start >= 0) {
        for (int i = start; i < n->n; i++) {
            enum completion c = exec_list(J, n->list[i]->list, n->list[i]->n, bs);
            if (J->thrown)
                goto out;
            if (c == C_BREAK && !J->label)
                break;
            if (c != C_NORMAL) {
                res = c;
                break;
            }
        }
    }
    if (n->flag && !bs->captured)
        scope_release(J, bs);
    return res;
out:
    return C_NORMAL;
}

static enum completion exec_var(js *J, struct node *n, struct scope *sc)
{
    int mode = n->kind == VK_VAR ? BM_VAR : n->kind == VK_LET ? BM_LET : BM_CONST;
    for (int i = 0; i < n->n; i++) {
        struct node *d = n->list[i];
        if (!d->b) {
            if (mode == BM_VAR)
                continue;                               /* `var x;` keeps the hoisted value */
            bind_pattern(J, d->a, JS_UNDEFINED, sc, mode);
        } else {
            jsval v = eval(J, d->b, sc);
            if (J->thrown)
                return C_NORMAL;
            if (d->a->t == N_IDENT && v.t == JS_OBJ && v.u.o->cls == JC_FUNCTION && v.u.o->fn->fi && !v.u.o->fn->fi->name &&
                (d->b->t == N_FUNC || d->b->t == N_CLASS)) {
                struct prop *np = obj_find_own(v.u.o, J->k_name);
                if (np && np->v.t == JS_STR && !np->v.u.s->len)
                    np->v = js_strv(d->a->s);
            }
            bind_pattern(J, d->a, v, sc, mode);
        }
        if (J->thrown)
            return C_NORMAL;
    }
    return C_NORMAL;
}

static enum completion exec_try(js *J, struct node *n, struct scope *sc)
{
    enum completion res = exec(J, n->a, sc);
    if (J->thrown && !J->aborted && !J->gen_unwind && n->flag) {
        jsval ex = js_exception(J);
        struct scope *cs = scope_new(J, sc);
        if (n->b)
            bind_pattern(J, n->b, ex, cs, BM_LET);
        if (!J->thrown)
            res = exec(J, n->c, cs);
        if (!cs->captured)
            scope_release(J, cs);
    }
    if (n->d && !J->aborted) {
        bool had = J->thrown;
        bool unwind = J->gen_unwind;
        jsval saved_exc = J->exc, saved_ret = J->retval;
        jsstr *saved_label = J->label;
        J->thrown = false;
        J->gen_unwind = false;
        enum completion fc = exec(J, n->d, sc);
        if (J->thrown)
            return C_NORMAL;                            /* an exception in finally replaces everything */
        if (fc != C_NORMAL)
            return fc;                                  /* break/continue/return in finally overrides */
        if (had) {
            J->thrown = true;
            J->gen_unwind = unwind;
            J->exc = saved_exc;
        } else {
            J->retval = saved_ret;
            J->label = saved_label;
        }
    }
    return res;
}

static enum completion exec_labeled(js *J, struct node *n, struct scope *sc)
{
    struct node *s = n->a;
    enum completion c;
    switch (s->t) {
    case N_FOR: c = exec_for(J, s, sc, n->s); break;
    case N_FORIN: case N_FOROF: c = exec_forin_of(J, s, sc, n->s); break;
    default: c = exec(J, s, sc);
    }
    if (c == C_BREAK && J->label == n->s) {
        J->label = NULL;
        return C_NORMAL;
    }
    return c;
}

static enum completion exec(js *J, struct node *n, struct scope *sc)
{
    if (J->budget && ++J->steps > J->budget) {
        abort_run(J, "budget");
        return C_NORMAL;
    }
    switch (n->t) {
    case N_EMPTY: return C_NORMAL;
    case N_EXPR: eval(J, n->a, sc); return C_NORMAL;
    case N_VAR: return exec_var(J, n, sc);
    case N_BLOCK: {
        if (!n->flag)
            return exec_list(J, n->list, n->n, sc);
        struct scope *bs = scope_new(J, sc);
        enum completion c = exec_list(J, n->list, n->n, bs);
        if (!bs->captured)
            scope_release(J, bs);
        return c;
    }
    case N_IF: {
        jsval c = eval(J, n->a, sc);
        if (J->thrown)
            return C_NORMAL;
        if (js_truthy(c))
            return exec(J, n->b, sc);
        return n->c ? exec(J, n->c, sc) : C_NORMAL;
    }
    case N_FOR: return exec_for(J, n, sc, NULL);
    case N_FORIN: case N_FOROF: return exec_forin_of(J, n, sc, NULL);
    case N_WHILE:
        for (;;) {
            jsval c = eval(J, n->a, sc);
            if (J->thrown)
                return C_NORMAL;
            if (!js_truthy(c))
                return C_NORMAL;
            enum completion r = exec(J, n->b, sc), out;
            if (J->thrown)
                return C_NORMAL;
            if (loop_ctl(J, r, NULL, &out))
                return out;
        }
    case N_DOWHILE:
        for (;;) {
            enum completion r = exec(J, n->b, sc), out;
            if (J->thrown)
                return C_NORMAL;
            if (loop_ctl(J, r, NULL, &out))
                return out;
            jsval c = eval(J, n->a, sc);
            if (J->thrown || !js_truthy(c))
                return C_NORMAL;
        }
    case N_RETURN:
        J->retval = n->a ? eval(J, n->a, sc) : JS_UNDEFINED;
        return J->thrown ? C_NORMAL : C_RETURN;
    case N_BREAK: J->label = n->s; return C_BREAK;
    case N_CONTINUE: J->label = n->s; return C_CONTINUE;
    case N_THROW: {
        jsval v = eval(J, n->a, sc);
        if (J->thrown)
            return C_NORMAL;
        js_throw_value(J, v);
        return C_NORMAL;
    }
    case N_TRY: return exec_try(J, n, sc);
    case N_SWITCH: return exec_switch(J, n, sc);
    case N_LABEL: return exec_labeled(J, n, sc);
    case N_WITH: {
        jsval o = eval(J, n->a, sc);
        if (J->thrown)
            return C_NORMAL;
        if (o.t != JS_OBJ) {
            throw_type(J, "Cannot convert undefined or null to object");
            return C_NORMAL;
        }
        struct scope *ws = scope_new(J, sc);
        ws->captured = 1;
        scope_mark_captured(sc);
        ws->obj = o.u.o;
        ws->is_with = 1;
        return exec(J, n->b, ws);
    }
    default:
        js_throw_error(J, "SyntaxError", "cannot execute node %d", n->t);
        return C_NORMAL;
    }
}

/* ---- programs ------------------------------------------------------------------------------------------------- */

jsval run_program(js *J, struct node *prog)
{
    struct scope *g = J->gscope;
    struct fninfo *fi = prog->fi;
    for (int i = 0; i < fi->nvars; i++)
        if (!has_prop(J, J->global, fi->vars[i]))
            set_prop(J, js_objv(J->global), fi->vars[i], JS_UNDEFINED);
    for (int i = 0; i < fi->nfuncs; i++) {
        jsval c = make_closure(J, fi->funcs[i], g, NULL);
        set_prop(J, js_objv(J->global), fi->funcs[i]->s, c);
    }
    J->depth = 0;
    J->label = NULL;
    /* the completion value of the last expression statement (for js_eval_string) */
    jsval last = JS_UNDEFINED;
    for (int i = 0; i < prog->n && !J->thrown; i++) {
        struct node *st = prog->list[i];
        if (st->t == N_EXPR) {
            last = eval(J, st->a, g);
            continue;
        }
        enum completion c = exec(J, st, g);
        if (c == C_RETURN)
            break;
    }
    if (J->thrown)
        return JS_UNDEFINED;
    return last;
}

struct scope *eval_global_scope(js *J)
{
    struct scope *g = scope_new(J, NULL);
    g->captured = 1;
    g->obj = J->global;
    g->has_this = 1;
    g->thisv = js_objv(J->global);
    return g;
}

bool js_run(js *J, const char *src, size_t len, const char *name, char *err, size_t errn)
{
    (void)name;
    if (js_memory_exhausted(J)) {
        snprintf(err, errn, "script not run: the JavaScript memory limit of this page was reached");
        return false;
    }
    struct node *prog = js_parse_program(J, src, len, err, errn);
    if (!prog)
        return false;
    J->thrown = false;
    J->short_circuit = false;
    J->entries++;
    run_program(J, prog);
    J->entries--;
    if (J->thrown) {
        jsval ex = js_exception(J);
        js_describe_exception(J, ex, err, errn);          /* (`aborted` stays set; js_set_budget clears it) */
        return false;
    }
    js_run_microtasks(J);
    return true;
}

char *js_eval_string(js *J, const char *src, char *err, size_t errn)
{
    struct node *prog = js_parse_program(J, src, strlen(src), err, errn);
    if (!prog)
        return NULL;
    J->thrown = false;
    jsval v = run_program(J, prog);
    if (!J->thrown) {
        jsstr *s = to_jsstr(J, v);
        if (s)
            return strdup(s->s);
    }
    jsval ex = js_exception(J);
    js_describe_exception(J, ex, err, errn);
    return NULL;
}

bool js_eval_value(js *J, const char *src, size_t len, jsval *out, char *err, size_t errn)
{
    struct node *prog = js_parse_program(J, src, len, err, errn);
    if (!prog)
        return false;
    J->thrown = false;
    J->short_circuit = false;
    jsval v = run_program(J, prog);
    if (J->thrown) {
        jsval ex = js_exception(J);
        js_describe_exception(J, ex, err, errn);
        return false;
    }
    *out = v;
    return true;
}
