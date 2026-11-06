/*
 * A small JavaScript interpreter for lumen-web (no libraries; tree-walking, ES5 plus the common ES6+ syntax).
 *
 * Supported: var/let/const, functions, closures, arrow functions, default/rest parameters, spread, destructuring,
 * template literals, classes (extends, super, static, fields, getters/setters), generators (function*, yield, yield*),
 * async functions / arrows / methods with await and for await (over sync iterables), for/for-in/for-of/while/do/switch,
 * labels, try/catch/finally, optional chaining, ??, **, ??=, typeof/instanceof/in/delete, `with`, getters/setters in
 * object literals, the Function constructor and (indirect) eval, Object/Function/Array/String/Number/Boolean/Math/JSON/
 * Date/Map/Set/Error/RegExp builtins (regexes: js_regex.c), and, written in JavaScript (js_poly.c), Promise, a Symbol-lite,
 * WeakMap/WeakSet, typed array constructors, Reflect, a little Intl, URL, URLSearchParams.
 * Also: async generators and for await over async iterators, tagged templates, Proxy (get/set/has/deleteProperty/ownKeys/apply/construct).
 * Not supported: real Symbols, modules, labels on blocks, \p{...} in regexes.
 * Strings are UTF-8 byte strings (length and indexes count bytes). Generators and async functions run their body as a
 * coroutine on its own C stack (js_coro.c).
 *
 * Memory is arena allocated and released all at once by js_free() (there is no garbage collector); a memory cap and an
 * instruction budget stop runaway scripts. The embedder (dom.c) adds `window`, `document`, timers ... as host objects.
 */
#ifndef LUMEN_JS_H
#define LUMEN_JS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct js js;
typedef struct jsobj jsobj;
typedef struct jsstr jsstr;

struct jsstr {
    uint32_t len;
    uint32_t hash;
    uint8_t interned;
    uint8_t kind;                   /* 0 not scanned yet, 1 ASCII only, 2 has non-ASCII */
    uint32_t len16, c16, cbyte;     /* UTF-16 length, and a cached (UTF-16 index, byte offset) pair for sequential access */
    char s[];                       /* NUL terminated */
};

enum jstype { JS_UNDEF, JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_OBJ };

typedef struct jsval {
    uint8_t t;
    union { double n; bool b; jsstr *s; jsobj *o; } u;
} jsval;

#define JS_UNDEFINED ((jsval){ .t = JS_UNDEF })
#define JS_NULLV ((jsval){ .t = JS_NULL })
static inline jsval js_bool(bool b) { jsval v = { .t = JS_BOOL }; v.u.b = b; return v; }
static inline jsval js_num(double n) { jsval v = { .t = JS_NUM }; v.u.n = n; return v; }
static inline jsval js_strv(jsstr *s) { jsval v = { .t = JS_STR }; v.u.s = s; return v; }
static inline jsval js_objv(jsobj *o) { jsval v = { .t = JS_OBJ }; v.u.o = o; return v; }
static inline bool js_is_obj(jsval v) { return v.t == JS_OBJ; }
static inline bool js_is_undef(jsval v) { return v.t == JS_UNDEF; }

/* native function: (context, this, argument count, arguments, the function object itself (js_fn_data)) */
typedef jsval (*js_native)(js *J, jsval thisv, int argc, jsval *argv, jsobj *callee);

/* host object class: `get` / `set` run before the ordinary property lookup (return true when handled);
 * `keys` appends enumerable own property names (for for-in / Object.keys) */
struct js_hostclass {
    const char *name;
    bool (*get)(js *J, jsobj *o, jsstr *key, jsval *out);
    bool (*set)(js *J, jsobj *o, jsstr *key, jsval v);
    void (*keys)(js *J, jsobj *o, jsobj *arr);
};

/* ---- context ---------------------------------------------------------------------------------------------- */

js *js_new(void);
void js_free(js *J);
jsobj *js_global(js *J);
void js_set_budget(js *J, uint64_t steps);          /* instructions allowed before the next js_set_budget (0 = unlimited) */
void js_set_memory_cap(js *J, size_t bytes);
size_t js_memory_used(js *J);
bool js_memory_exhausted(js *J);                    /* the memory cap was exceeded: no more scripts should run */
uint64_t js_steps_used(js *J);
void js_set_console(js *J, void (*fn)(void *data, const char *level, const char *msg), void *data);

/* Run a script at global level. False + "Name: message (line N)" in err on a syntax error or an uncaught exception. */
bool js_run(js *J, const char *src, size_t len, const char *name, char *err, size_t errn);
/* Run a script and return the value of its last expression statement (e.g. "(function(event){...})"). */
bool js_eval_value(js *J, const char *src, size_t len, jsval *out, char *err, size_t errn);
/* Evaluate an expression/script and return its value as a string (selftests). NULL on error (message in err). */
char *js_eval_string(js *J, const char *src, char *err, size_t errn);

/* ---- values ------------------------------------------------------------------------------------------------ */

jsstr *js_str_new(js *J, const char *s, size_t len);     /* a fresh (not interned) string */
jsstr *js_intern(js *J, const char *s, size_t len);
jsval js_cstr(js *J, const char *z);                     /* string value from a C string */
jsval js_strn(js *J, const char *s, size_t len);
const char *js_tostr(js *J, jsval v, size_t *len);       /* ToString; may call user code; NULL if that threw */
double js_tonum(js *J, jsval v);
bool js_truthy(jsval v);
bool js_strict_equals(jsval a, jsval b);

jsobj *js_object_new(js *J);
jsobj *js_array_new(js *J);
void js_array_push(js *J, jsobj *arr, jsval v);
uint32_t js_array_len(const jsobj *arr);
jsval js_array_get(const jsobj *arr, uint32_t i);
bool js_is_array(jsval v);
bool js_is_function(jsval v);

jsobj *js_native_new(js *J, const char *name, js_native fn, int nargs, void *data);
void *js_fn_data(jsobj *callee);
jsobj *js_host_new(js *J, const struct js_hostclass *hc, void *ptr, jsobj *proto);
void *js_host_ptr(jsval v, const struct js_hostclass *hc);   /* NULL unless v is a host object of that class */
void js_host_set_ptr(jsobj *o, void *ptr);
void *js_host_data(jsobj *o);                            /* the pointer of a host object (inside get/set hooks) */
void js_fn_mark_ctor(jsobj *f);                          /* a native that may be called with `new` (it then gets the fresh object as this) */

jsval js_get(js *J, jsval base, const char *key);
void js_set(js *J, jsval base, const char *key, jsval v);
jsval js_getv(js *J, jsval base, jsval key);
/* define a non-enumerable method / data property */
void js_def_native(js *J, jsobj *o, const char *name, js_native fn, int nargs);
void js_def_value(js *J, jsobj *o, const char *name, jsval v);
jsobj *js_proto_of(jsobj *o);
void js_set_proto(jsobj *o, jsobj *proto);
jsobj *js_object_proto(js *J);                           /* Object.prototype */
jsobj *js_function_proto(js *J);

/* Run the queued microtasks (queueMicrotask, Promise reactions). js_call and js_run do this by themselves at depth 0. */
void js_run_microtasks(js *J);

/* Call a function value. *threw set when it threw (the exception is then in js_exception()). */
jsval js_call(js *J, jsval fn, jsval thisv, int argc, jsval *argv, bool *threw);
jsval js_construct(js *J, jsval fn, int argc, jsval *argv, bool *threw);

/* errors from natives: throw a TypeError etc. and return undefined */
jsval js_throw_error(js *J, const char *type, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
jsval js_throw_value(js *J, jsval v);
bool js_thrown(js *J);
jsval js_exception(js *J);                               /* and clears it */
/* "Type: message" for an exception value, for logs */
void js_describe_exception(js *J, jsval ex, char *out, size_t n);

#endif
