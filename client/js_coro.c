/*
 * Generators and async functions. Both run their body as a coroutine on a separate C stack (ucontext): `yield` / `await`
 * switch back to whoever resumed the coroutine. Generators are resumed by next()/throw()/return(); async functions are
 * driven by promise reactions (see async_step). A suspended coroutine that is never resumed is simply freed with the
 * interpreter. generator.return() unwinds the body with J->gen_unwind set (catch clauses ignore it, finally blocks run).
 */
#include <sys/mman.h>
#include <ucontext.h>

#include "js_lib.h"

#define CORO_STACK (512 * 1024)

struct coro {
    ucontext_t ctx, caller;
    char *stack;
    jsobj *fn;
    jsval thisv;
    jsval *args;
    int argc;
    int kind;                       /* 0 generator, 1 async */
    int state;                      /* 0 not started, 1 suspended, 2 running, 3 done */
    jsval transfer;                 /* value travelling in or out */
    int mode;                       /* resume mode: 0 next, 1 throw, 2 return */
    bool threw;
    jsval result;
    int depth;
    jsval resolve, reject;          /* async: the result promise */
    bool awaiting;                  /* suspended on `await` (as opposed to `yield`) */
    struct coro *next_all;
};

static const struct js_hostclass gen_class = { "Generator", NULL, NULL, NULL };

static struct coro *g_starting;     /* handed to coro_entry (makecontext only passes ints) */

bool co_active(js *J) { return J->cur_co != NULL; }

/* The interpreter pointer travels in a static too: coroutines never outlive their js. */
static js *g_js;

static void coro_main(void)
{
    struct coro *co = g_starting;
    js *J = g_js;
    jsval r = run_closure_body(J, co->fn, co->thisv, co->argc, co->args, JS_UNDEFINED);
    if (J->thrown && J->gen_unwind && !J->aborted) {            /* generator.return(v) finished unwinding */
        J->gen_unwind = false;
        r = J->exc;
        J->thrown = false;
        J->exc = JS_UNDEFINED;
    }
    co->result = r;
    co->threw = J->thrown;
    co->state = 3;
    swapcontext(&co->ctx, &co->caller);                         /* never resumed */
}

static void coro_free_stack(struct coro *co)
{
    if (co->stack) {
        munmap(co->stack, CORO_STACK);
        co->stack = NULL;
    }
}

static struct coro *coro_new(js *J, jsobj *f, jsval thisv, int argc, jsval *argv, int kind)
{
    struct coro *co = calloc(1, sizeof(*co));
    if (!co)
        exit(1);
    co->fn = f;
    co->thisv = thisv;
    co->argc = argc;
    co->args = malloc(sizeof(jsval) * (size_t)(argc + 1));
    if (argc)
        memcpy(co->args, argv, sizeof(jsval) * (size_t)argc);
    co->kind = kind;
    co->next_all = J->all_co;
    J->all_co = co;
    return co;
}

void js_coro_free_all(js *J)
{
    for (struct coro *c = J->all_co; c;) {
        struct coro *nx = c->next_all;
        coro_free_stack(c);
        free(c->args);
        free(c);
        c = nx;
    }
    J->all_co = NULL;
}

/* Resume `co` with a value. Afterwards co->state is 1 (suspended: co->transfer is the yielded value) or 3 (done:
 * co->result / co->threw, with J->thrown still set when it threw). */
static void resume(js *J, struct coro *co, int mode, jsval v)
{
    if (co->state == 2) {
        throw_type(J, "Generator is already running");
        return;
    }
    co->mode = mode;
    co->transfer = v;
    if (co->state == 0) {
        co->stack = mmap(NULL, CORO_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (co->stack == MAP_FAILED) {
            co->stack = NULL;
            js_throw_error(J, "RangeError", "too many suspended generators / async calls");
            return;
        }
        getcontext(&co->ctx);
        co->ctx.uc_stack.ss_sp = co->stack;
        co->ctx.uc_stack.ss_size = CORO_STACK;
        co->ctx.uc_link = NULL;
        makecontext(&co->ctx, coro_main, 0);
        co->depth = 0;
    }
    struct coro *prev = J->cur_co;
    int saved_depth = J->depth;
    g_starting = co;
    g_js = J;
    J->cur_co = co;
    J->depth = co->depth;
    co->state = 2;
    swapcontext(&co->caller, &co->ctx);
    J->cur_co = prev;
    J->depth = saved_depth;
    if (co->state == 3)
        coro_free_stack(co);
}

/* Called inside a coroutine: hand `v` to the resumer and wait to be resumed. */
static jsval co_suspend(js *J, jsval v, bool awaiting)
{
    struct coro *co = J->cur_co;
    if (!co)
        return js_throw_error(J, "SyntaxError", "yield/await outside a generator or async function");
    co->awaiting = awaiting;
    co->transfer = v;
    co->state = 1;
    co->depth = J->depth;
    swapcontext(&co->ctx, &co->caller);
    /* resumed */
    g_js = J;
    co->state = 2;
    J->depth = co->depth;
    if (co->mode == 1) {
        js_throw_value(J, co->transfer);
        return JS_UNDEFINED;
    }
    if (co->mode == 2) {
        J->gen_unwind = true;
        J->thrown = true;
        J->exc = co->transfer;
        return JS_UNDEFINED;
    }
    return co->transfer;
}

jsval co_yield(js *J, jsval v) { return co_suspend(J, v, false); }
struct tla { bool done, ok; jsval val; };
NATIVE(tla_ok) { struct tla *t = js_fn_data(callee); t->done = t->ok = true; t->val = ARG(0); return JS_UNDEFINED; }
NATIVE(tla_err) { struct tla *t = js_fn_data(callee); t->done = true; t->ok = false; t->val = ARG(0); return JS_UNDEFINED; }

/* await. Inside a coroutine it suspends; at the top level of a module (no coroutine) it runs the microtask queue until the
 * promise settles (the page's own fetch/XHR are synchronous, so this covers the usual `await fetch(...)`) */
jsval co_await(js *J, jsval v)
{
    if (J->cur_co)
        return co_suspend(J, v, true);
    jsval P = js_get(J, js_objv(J->global), "Promise");
    jsval p = js_call(J, js_get(J, P, "resolve"), P, 1, &v, NULL);
    THROW_IF(J);
    struct tla *t = js_alloc(J, sizeof(*t));
    jsval cbs[2] = { js_objv(js_native_new(J, "", tla_ok, 1, t)), js_objv(js_native_new(J, "", tla_err, 1, t)) };
    js_call(J, js_get(J, p, "then"), p, 2, cbs, NULL);
    THROW_IF(J);
    int saved = J->entries;                                    /* we are inside js_run_module: allow draining here */
    J->entries = 0;
    js_run_microtasks(J);
    J->entries = saved;
    if (!t->done)
        return js_throw_error(J, "Error", "top-level await: the promise did not settle");
    if (!t->ok) {
        J->thrown = true;
        J->exc = t->val;
        return JS_UNDEFINED;
    }
    return t->val;
}

/* ---- generators ------------------------------------------------------------------------------------------------- */

jsval make_generator(js *J, jsobj *f, jsval thisv, int argc, jsval *argv)
{
    jsval pv = get_prop(J, js_objv(f), J->k_prototype);
    THROW_IF(J);
    jsobj *g = js_host_new(J, &gen_class, NULL, pv.t == JS_OBJ ? pv.u.o : J->gen_proto);
    g->host = coro_new(J, f, thisv, argc, argv, 0);
    return js_objv(g);
}

static jsval iter_result(js *J, jsval value, bool done)
{
    jsobj *r = js_object_new(J);
    set_prop(J, js_objv(r), J->k_value, value);
    set_prop(J, js_objv(r), J->k_done, js_bool(done));
    return js_objv(r);
}

static jsval gen_resume(js *J, jsval thisv, int mode, jsval v)
{
    struct coro *co = js_host_ptr(thisv, &gen_class);
    if (!co)
        return throw_type(J, "next method called on an incompatible receiver");
    if (co->state == 3 || (co->state == 0 && mode != 0)) {
        if (co->state == 0)
            co->state = 3;
        if (mode == 1)
            return js_throw_value(J, v);
        return iter_result(J, mode == 2 ? v : JS_UNDEFINED, true);
    }
    resume(J, co, mode, v);
    THROW_IF(J);
    if (co->state == 3) {
        if (co->threw)
            return JS_UNDEFINED;                                /* the exception propagates to the caller */
        return iter_result(J, co->result, true);
    }
    return iter_result(J, co->transfer, false);
}

NATIVE(Gen_next) { return gen_resume(J, thisv, 0, ARG(0)); }
NATIVE(Gen_throw) { return gen_resume(J, thisv, 1, ARG(0)); }
NATIVE(Gen_return) { return gen_resume(J, thisv, 2, ARG(0)); }
NATIVE(Gen_iterator) { return thisv; }

/* Collect the values of an object following the iterator protocol (an object with @@iterator, or an iterator itself). */
bool js_has_iterator(js *J, jsval v)
{
    if (v.t != JS_OBJ)
        return false;
    jsval f = get_prop(J, v, js_intern(J, "@@iterator", 10));
    if (J->thrown) {
        js_exception(J);
        return false;
    }
    return js_is_function(f);
}

bool gen_collect(js *J, jsval obj, jsval **out, int *outn, int max)
{
    jsval itf = get_prop(J, obj, js_intern(J, "@@iterator", 10));
    if (J->thrown)
        return false;
    jsval it = obj;
    if (js_is_function(itf)) {
        it = call_function(J, itf.u.o, obj, 0, NULL, JS_UNDEFINED);
        if (J->thrown)
            return false;
    }
    jsval next = get_prop(J, it, J->k_next);
    if (J->thrown)
        return false;
    if (!js_is_function(next)) {
        throw_type(J, "iterator.next is not a function");
        return false;
    }
    int cap = 16, n = 0;
    jsval *v = malloc(sizeof(jsval) * (size_t)cap);
    while (max < 0 || n < max) {
        jsval r = call_function(J, next.u.o, it, 0, NULL, JS_UNDEFINED);
        if (J->thrown) { free(v); return false; }
        if (r.t != JS_OBJ) { free(v); throw_type(J, "iterator result is not an object"); return false; }
        jsval done = get_prop(J, r, J->k_done);
        if (J->thrown) { free(v); return false; }
        if (js_truthy(done))
            break;
        jsval val = get_prop(J, r, J->k_value);
        if (J->thrown) { free(v); return false; }
        if (n == cap) { cap *= 2; v = realloc(v, sizeof(jsval) * (size_t)cap); }
        v[n++] = val;
        if (J->budget && ++J->steps > J->budget) {
            free(v);
            J->aborted = true;
            J->thrown = true;
            return false;
        }
    }
    if (max >= 0 && n >= max) {                                 /* stopped early: close the iterator */
        jsval ret = get_prop(J, it, js_intern(J, "return", 6));
        if (!J->thrown && js_is_function(ret))
            call_function(J, ret.u.o, it, 0, NULL, JS_UNDEFINED);
    }
    *out = v;
    *outn = n;
    return true;
}

/* ---- async functions ------------------------------------------------------------------------------------------- */

static void async_step(js *J, struct coro *co, int mode, jsval v);

NATIVE(async_on_fulfilled)
{
    struct coro *co = js_fn_data(callee);
    async_step(J, co, 0, ARG(0));
    return JS_UNDEFINED;
}
NATIVE(async_on_rejected)
{
    struct coro *co = js_fn_data(callee);
    async_step(J, co, 1, ARG(0));
    return JS_UNDEFINED;
}

static void async_step(js *J, struct coro *co, int mode, jsval v)
{
    resume(J, co, mode, v);
    if (J->aborted)
        return;
    if (J->thrown && co->state != 3) {                          /* could not even start */
        jsval ex = js_exception(J);
        call_function(J, co->reject.u.o, JS_UNDEFINED, 1, &ex, JS_UNDEFINED);
        return;
    }
    if (co->state == 3) {
        if (co->threw) {
            jsval ex = js_exception(J);
            call_function(J, co->reject.u.o, JS_UNDEFINED, 1, &ex, JS_UNDEFINED);
        } else {
            jsval r = co->result;
            call_function(J, co->resolve.u.o, JS_UNDEFINED, 1, &r, JS_UNDEFINED);
        }
        return;
    }
    /* suspended at an await: continue when the awaited value settles */
    jsval P = js_get(J, js_objv(J->global), "Promise");
    jsval p = js_call(J, js_get(J, P, "resolve"), P, 1, &co->transfer, NULL);
    if (J->thrown)
        return;
    jsval cbs[2] = { js_objv(js_native_new(J, "", async_on_fulfilled, 1, co)), js_objv(js_native_new(J, "", async_on_rejected, 1, co)) };
    js_call(J, js_get(J, p, "then"), p, 2, cbs, NULL);
}

NATIVE(async_executor)
{
    struct coro *co = js_fn_data(callee);
    co->resolve = ARG(0);
    co->reject = ARG(1);
    return JS_UNDEFINED;
}

jsval start_async(js *J, jsobj *f, jsval thisv, int argc, jsval *argv)
{
    struct coro *co = coro_new(J, f, thisv, argc, argv, 1);
    jsval P = js_get(J, js_objv(J->global), "Promise");
    jsval ex = js_objv(js_native_new(J, "", async_executor, 2, co));
    jsval promise = construct(J, P, 1, &ex);
    THROW_IF(J);
    async_step(J, co, 0, JS_UNDEFINED);
    return promise;
}

/* ---- async generators ---------------------------------------------------------------------------------------- */

struct agreq { int mode; jsval value, resolve, reject; };

struct agen {
    struct coro *co;
    struct agreq *q;
    int qn, qcap;
    bool running;
};

static const struct js_hostclass agen_class = { "AsyncGenerator", NULL, NULL, NULL };

struct pcap { jsval res, rej; };

NATIVE(pcap_executor)
{
    struct pcap *c = js_fn_data(callee);
    c->res = ARG(0);
    c->rej = ARG(1);
    return JS_UNDEFINED;
}

static jsval new_promise(js *J, jsval *res, jsval *rej)
{
    struct pcap *c = js_alloc(J, sizeof(*c));
    jsval P = js_get(J, js_objv(J->global), "Promise");
    jsval ex = js_objv(js_native_new(J, "", pcap_executor, 2, c));
    jsval p = construct(J, P, 1, &ex);
    *res = c->res;
    *rej = c->rej;
    return p;
}

static void agen_drive(js *J, struct agen *ag);

static void agen_settle(js *J, struct agen *ag, bool ok, jsval v, bool done)
{
    struct agreq req = ag->q[0];
    memmove(ag->q, ag->q + 1, sizeof(*ag->q) * (size_t)(--ag->qn));
    if (ok) {
        jsval r = iter_result(J, v, done);
        call_function(J, req.resolve.u.o, JS_UNDEFINED, 1, &r, JS_UNDEFINED);
    } else {
        call_function(J, req.reject.u.o, JS_UNDEFINED, 1, &v, JS_UNDEFINED);
    }
}

static void agen_step(js *J, struct agen *ag, int mode, jsval v);

NATIVE(agen_on_ok) { agen_step(J, js_fn_data(callee), 0, ARG(0)); return JS_UNDEFINED; }
NATIVE(agen_on_err) { agen_step(J, js_fn_data(callee), 1, ARG(0)); return JS_UNDEFINED; }

static void agen_step(js *J, struct agen *ag, int mode, jsval v)
{
    struct coro *co = ag->co;
    resume(J, co, mode, v);
    if (J->aborted)
        return;
    if (co->state == 3 || (J->thrown && co->state != 3)) {
        if (co->threw || J->thrown) {
            jsval ex = js_exception(J);
            ag->running = false;
            agen_settle(J, ag, false, ex, true);
        } else {
            ag->running = false;
            agen_settle(J, ag, true, co->result, true);
        }
        agen_drive(J, ag);
        return;
    }
    if (co->awaiting) {                                         /* continue when the awaited value settles */
        jsval P = js_get(J, js_objv(J->global), "Promise");
        jsval p = js_call(J, js_get(J, P, "resolve"), P, 1, &co->transfer, NULL);
        if (J->thrown)
            return;
        jsval cbs[2] = { js_objv(js_native_new(J, "", agen_on_ok, 1, ag)), js_objv(js_native_new(J, "", agen_on_err, 1, ag)) };
        js_call(J, js_get(J, p, "then"), p, 2, cbs, NULL);
        return;
    }
    ag->running = false;                                        /* a yield: answer the pending next() */
    agen_settle(J, ag, true, co->transfer, false);
    agen_drive(J, ag);
}

static void agen_drive(js *J, struct agen *ag)
{
    while (!ag->running && ag->qn > 0 && !J->aborted) {
        struct agreq *req = &ag->q[0];
        struct coro *co = ag->co;
        if (co->state == 0 && req->mode != 0)
            co->state = 3;
        if (co->state == 3) {
            if (req->mode == 1) agen_settle(J, ag, false, req->value, true);
            else agen_settle(J, ag, true, req->mode == 2 ? req->value : JS_UNDEFINED, true);
            continue;
        }
        ag->running = true;
        agen_step(J, ag, req->mode, req->value);
    }
}

static jsval agen_request(js *J, jsval thisv, int mode, jsval v)
{
    struct agen *ag = js_host_ptr(thisv, &agen_class);
    jsval res, rej;
    jsval p = new_promise(J, &res, &rej);
    THROW_IF(J);
    if (!ag) {
        jsval e = make_error(J, ERR_TYPE, "next method called on an incompatible receiver");
        call_function(J, rej.u.o, JS_UNDEFINED, 1, &e, JS_UNDEFINED);
        return p;
    }
    if (ag->qn == ag->qcap) {                                   /* (arena memory: freed with the interpreter) */
        int ncap = ag->qcap ? ag->qcap * 2 : 4;
        struct agreq *nq = js_alloc(J, sizeof(*nq) * (size_t)ncap);
        if (ag->qn)
            memcpy(nq, ag->q, sizeof(*nq) * (size_t)ag->qn);
        ag->q = nq;
        ag->qcap = ncap;
    }
    ag->q[ag->qn++] = (struct agreq){ mode, v, res, rej };
    agen_drive(J, ag);
    return p;
}

NATIVE(AGen_next) { return agen_request(J, thisv, 0, ARG(0)); }
NATIVE(AGen_throw) { return agen_request(J, thisv, 1, ARG(0)); }
NATIVE(AGen_return) { return agen_request(J, thisv, 2, ARG(0)); }

jsval make_async_generator(js *J, jsobj *f, jsval thisv, int argc, jsval *argv)
{
    jsval pv = get_prop(J, js_objv(f), J->k_prototype);
    THROW_IF(J);
    jsobj *g = js_host_new(J, &agen_class, NULL, pv.t == JS_OBJ ? pv.u.o : J->agen_proto);
    struct agen *ag = js_alloc(J, sizeof(*ag));
    ag->co = coro_new(J, f, thisv, argc, argv, 0);
    g->host = ag;
    return js_objv(g);
}

bool js_has_async_iterator(js *J, jsval v)
{
    if (v.t != JS_OBJ)
        return false;
    jsval f = get_prop(J, v, js_intern(J, "@@asyncIterator", 15));
    if (J->thrown) {
        js_exception(J);
        return false;
    }
    return js_is_function(f);
}

void js_init_coro(js *J)
{
    J->agen_proto = obj_new(J, JC_OBJECT, J->object_proto);
    lib_def(J, J->agen_proto, "next", AGen_next, 1);
    lib_def(J, J->agen_proto, "throw", AGen_throw, 1);
    lib_def(J, J->agen_proto, "return", AGen_return, 1);
    lib_def(J, J->agen_proto, "@@asyncIterator", Gen_iterator, 0);
    J->gen_proto = obj_new(J, JC_OBJECT, J->object_proto);
    lib_def(J, J->gen_proto, "next", Gen_next, 1);
    lib_def(J, J->gen_proto, "throw", Gen_throw, 1);
    lib_def(J, J->gen_proto, "return", Gen_return, 1);
    lib_def(J, J->gen_proto, "@@iterator", Gen_iterator, 0);
}
