/* Internals of the JavaScript interpreter (js.h): objects, AST, scopes. Only js_*.c include this. */
#ifndef LUMEN_JS_INT_H
#define LUMEN_JS_INT_H

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "js.h"

/* ---- objects -------------------------------------------------------------------------------------------------- */

enum jscls { JC_OBJECT, JC_ARRAY, JC_FUNCTION, JC_ERROR, JC_BOOLEAN, JC_NUMBER, JC_STRING, JC_DATE, JC_MAP, JC_SET, JC_HOST, JC_REGEXP };

#define PF_ENUM 1
#define PF_WRITE 2
#define PF_ACCESSOR 4

struct prop {
    jsstr *key;                     /* interned; NULL = deleted */
    jsval v;
    jsobj *get, *set;
    uint8_t flags;
};

struct node;
struct scope;
struct fninfo;

struct func {
    struct fninfo *fi;              /* closure */
    struct scope *scope;
    js_native native;
    void *data;
    jsobj *home;                    /* object whose prototype `super` refers to (methods) */
    jsobj *fields_cls;              /* unused */
    jsstr *name;
    uint8_t is_class, derived, is_ctor_native;
    jsobj *parent_ctor;             /* class constructors: the `extends` target */
    struct node *class_node;        /* class constructors: the class body for instance fields */
    struct scope *class_scope;
};

#define OF_FROZEN 1
#define OF_NOEXT 2

struct jsobj {
    uint8_t cls;
    uint8_t flags;
    jsobj *proto;
    struct prop *props;
    uint32_t nprops, pcap;
    int32_t *hidx;
    uint32_t hcap;
    jsval *items;                   /* array elements / Map and Set keys */
    jsval *items2;                  /* Map values */
    uint32_t len, icap;
    struct func *fn;
    void *host;
    const struct js_hostclass *hclass;
    jsval prim;                     /* wrapped primitive, Date time value */
    jsobj *all_next;
};

/* ---- AST ------------------------------------------------------------------------------------------------------- */

enum nodetype {
    N_NUM, N_STR, N_TEMPLATE, N_IDENT, N_THIS, N_LIT /* undefined/null/true/false in num */, N_ARRAY, N_OBJECT, N_FUNC,
    N_CLASS, N_UNARY, N_UPDATE, N_BINARY, N_LOGICAL, N_ASSIGN, N_COND, N_CALL, N_NEW, N_MEMBER, N_SEQ, N_SPREAD,
    N_SUPER, N_HOLE, N_REGEX, N_AWAIT, N_YIELD,
    /* statements */
    N_VAR, N_DECL, N_EXPR, N_BLOCK, N_IF, N_FOR, N_FORIN, N_FOROF, N_WHILE, N_DOWHILE, N_RETURN, N_BREAK,
    N_CONTINUE, N_THROW, N_TRY, N_WITH, N_SWITCH, N_CASE, N_EMPTY, N_LABEL, N_PROP, N_PROGRAM
};

/* operator codes (node->op) */
enum {
    OP_NONE, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_EXP, OP_SHL, OP_SHR, OP_USHR, OP_BAND, OP_BOR, OP_BXOR,
    OP_EQ, OP_NE, OP_SEQ, OP_SNE, OP_LT, OP_GT, OP_LE, OP_GE, OP_IN, OP_INSTANCEOF,
    OP_AND, OP_OR, OP_NULLISH,
    OP_NOT, OP_NEG, OP_POS, OP_BNOT, OP_TYPEOF, OP_VOID, OP_DELETE,
    OP_INC, OP_DEC,
    OP_ASSIGN
};

struct node {
    uint8_t t, op, flag, kind;
    int line;
    struct node *a, *b, *c, *d;
    struct node **list;
    int n;
    jsstr *s;
    double num;
    struct fninfo *fi;
};

/* node->kind for N_VAR / N_PROP / N_FUNC */
enum { VK_VAR, VK_LET, VK_CONST };
enum { PK_INIT, PK_GET, PK_SET, PK_SPREAD, PK_METHOD };

struct fninfo {
    jsstr *name;
    struct node **params;           /* binding patterns (identifier, array/object pattern, N_ASSIGN default, N_SPREAD rest) */
    int nparams;
    struct node *body;              /* N_BLOCK, or an expression when expr_body */
    uint8_t is_arrow, expr_body, uses_args, is_method, is_ctor, has_lexical, is_getter, is_setter, is_gen, is_async;
    jsstr **vars;                   /* hoisted `var` names */
    int nvars;
    struct node **funcs;            /* hoisted function declarations (N_FUNC with .s = name) */
    int nfuncs;
    const char *src;                /* source text, for Function.prototype.toString */
    uint32_t src_len;
    int nformal;                    /* function.length */
};

/* ---- scopes ---------------------------------------------------------------------------------------------------- */

#define SC_INLINE 6

struct scope {
    struct scope *parent;
    jsstr **names;
    jsval *vals;
    uint8_t *fl;                    /* 1 = const */
    uint32_t n, cap;
    jsobj *obj;                     /* the global scope also resolves names through this object */
    jsval thisv;
    uint8_t has_this, captured, pooled, is_with;
    jsobj *func;                    /* function being executed (for super / home object) */
    jsobj *args_obj;
    struct scope *next_free;
    jsstr *in_names[SC_INLINE];
    jsval in_vals[SC_INLINE];
    uint8_t in_fl[SC_INLINE];
};

/* ---- context --------------------------------------------------------------------------------------------------- */

struct arena_chunk { struct arena_chunk *next; };

struct js {
    struct arena_chunk *chunks;
    char *bump, *bump_end;
    size_t mem, mem_cap;
    jsstr **itab;
    size_t icap, icount;
    jsobj *all_objs;
    struct scope *free_scopes;

    uint64_t steps, budget;
    int depth;
    bool thrown, aborted, short_circuit;
    jsval exc;
    jsval retval;
    struct coro *cur_co, *all_co;   /* running coroutine (generator / async function body), all of them for cleanup */
    int entries;                    /* nesting of host entry points (js_run / js_call): microtasks run when it returns to 0 */
    bool draining;
    bool gen_unwind;                /* generator.return(): unwinding with `exc` as the return value (catch clauses ignore it) */
    jsobj *gen_proto;
    jsval *mq;                      /* queueMicrotask() queue */
    size_t mq_n, mq_cap, mq_head;
    jsval newtarget;                // set while a native runs: the constructor when called with `new`
    jsstr *label;                   /* pending break/continue label */

    jsobj *global;
    struct scope *gscope;
    jsobj *object_proto, *function_proto, *array_proto, *string_proto, *number_proto, *boolean_proto, *error_proto,
          *date_proto, *map_proto, *set_proto, *regexp_proto;
    jsobj *err_protos[8];           /* by error type index, see js_lib.c */

    void (*console)(void *data, const char *level, const char *msg);
    void *console_data;

    /* well known interned names */
    jsstr *k_length, *k_prototype, *k_constructor, *k_name, *k_message, *k_toString, *k_valueOf, *k_arguments,
          *k_undefined, *k_stack, *k_get, *k_set, *k_value, *k_writable, *k_enumerable, *k_configurable, *k_empty,
          *k_this, *k_caller, *k_toJSON, *k_next, *k_done, *k_index;
    jsstr *k_digits[64];
};

/* core (js_core.c) */
void *js_alloc(js *J, size_t n);
void *js_realloc_buf(js *J, void *p, size_t oldn, size_t newn);   /* malloc'd growable buffers, accounted */
jsstr *js_intern_str(js *J, jsstr *s);
jsobj *obj_new(js *J, uint8_t cls, jsobj *proto);
struct prop *obj_find_own(jsobj *o, jsstr *key);
struct prop *obj_add_prop(js *J, jsobj *o, jsstr *key, jsval v, uint8_t flags);
bool obj_delete(jsobj *o, jsstr *key);
jsval get_prop(js *J, jsval base, jsstr *key);
bool set_prop(js *J, jsval base, jsstr *key, jsval v);
bool has_prop(js *J, jsobj *o, jsstr *key);
bool is_index_key(jsstr *k, uint32_t *idx);
jsstr *key_of(js *J, jsval v);                      /* ToPropertyKey (interned); NULL if toString threw */
/* UTF-16 view of a UTF-8 string: `length` and indexes count UTF-16 code units (a 4-byte character is two) */
uint32_t str_len16(jsstr *s);
uint32_t str_off(jsstr *s, uint32_t unit);              /* byte offset of a UTF-16 index (clamped to the end) */
uint32_t str_unit(jsstr *s, uint32_t byte);             /* UTF-16 index of a byte offset */
jsval str_char_at(js *J, jsstr *s, uint32_t unit);      /* s[unit] as a string ("" for the low half of a surrogate pair) */
int32_t str_code_unit(jsstr *s, uint32_t unit);         /* charCodeAt (-1 out of range) */
jsstr *num_to_str(js *J, double d);
jsstr *str_concat(js *J, jsstr *a, jsstr *b);
jsstr *to_jsstr(js *J, jsval v);                    /* ToString; NULL if it threw */
double to_number(js *J, jsval v);
jsval to_primitive(js *J, jsval v, int hint);       /* hint: 0 default, 1 number, 2 string */
const char *type_of(jsval v);
bool loose_equals(js *J, jsval a, jsval b);
bool same_value_zero(jsval a, jsval b);
int32_t to_int32(double d);
uint32_t to_uint32(double d);
bool js_call_budget(js *J);
void array_set_len(js *J, jsobj *a, uint32_t n);
void array_put(js *J, jsobj *a, uint32_t i, jsval v);
jsval throw_type(js *J, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void own_keys(js *J, jsobj *o, jsobj *out, bool only_enum);   /* appends string keys (indexes first) */
jsval make_error(js *J, int type, const char *msg);
double str_to_number(const char *s, size_t len);

/* parser (js_parse.c) */
struct node *js_parse_program(js *J, const char *src, size_t len, char *err, size_t errn);

/* evaluator (js_eval.c) */
jsval run_program(js *J, struct node *prog);
struct scope *eval_global_scope(js *J);
jsval call_function(js *J, jsobj *f, jsval thisv, int argc, jsval *argv, jsval newtarget);
jsval construct(js *J, jsval fnv, int argc, jsval *argv);
jsval fn_to_string(js *J, jsobj *f);
jsobj *make_native(js *J, jsstr *name, js_native fn, int nargs, void *data);

/* coroutines: generators and async functions (js_coro.c) */
struct coro;
jsval make_generator(js *J, jsobj *f, jsval thisv, int argc, jsval *argv);
jsval start_async(js *J, jsobj *f, jsval thisv, int argc, jsval *argv);
jsval co_yield(js *J, jsval v);                      /* yield / await inside a running coroutine */
bool co_active(js *J);
void js_init_coro(js *J);
void js_coro_free_all(js *J);
jsval run_closure_body(js *J, jsobj *f, jsval thisv, int argc, jsval *argv, jsval newtarget);
bool gen_collect(js *J, jsval it, jsval **out, int *outn, int max);   /* iterate an iterator-protocol object; false on exception */
bool js_has_iterator(js *J, jsval v);

/* builtins (js_lib.c) */
void js_init_builtins(js *J);
void js_init_polyfills(js *J);                      /* js_poly.c: Promise, Symbol-lite, Reflect ... (after the builtins) */
jsstr *js_json_quote(js *J, jsstr *s);

/* regular expressions (js_regex.c) */
void js_init_regex(js *J);
void js_regex_free_host(jsobj *o);
jsval js_regexp_literal(js *J, jsstr *pattern, jsstr *flags);
bool js_is_regexp(jsval v);
jsval js_regex_match(js *J, jsstr *s, jsval pat);
jsval js_regex_match_all(js *J, jsstr *s, jsval pat);
jsval js_regex_search(js *J, jsstr *s, jsval pat);
jsval js_regex_replace(js *J, jsstr *s, jsval rx, jsval repl, bool all);
jsval js_regex_split(js *J, jsstr *s, jsval rx, double limit);

#define ERR_ERROR 0
#define ERR_TYPE 1
#define ERR_RANGE 2
#define ERR_SYNTAX 3
#define ERR_REFERENCE 4
#define ERR_EVAL 5
#define ERR_URI 6

#define THROW_IF(J) do { if ((J)->thrown) return JS_UNDEFINED; } while (0)

#endif
