/* Shared helpers of the builtin libraries (js_lib*.c). */
#ifndef LUMEN_JS_LIB_H
#define LUMEN_JS_LIB_H

#include <math.h>

#include "js_int.h"

#define NATIVE(name) static jsval name(js *J __attribute__((unused)), jsval thisv __attribute__((unused)), \
                                       int argc __attribute__((unused)), jsval *argv __attribute__((unused)), \
                                       jsobj *callee __attribute__((unused)))
#define ARG(i) ((i) < argc ? argv[i] : JS_UNDEFINED)

struct sbuf { char *p; size_t n, cap; };
void sb_add(struct sbuf *s, const char *d, size_t n);
void sb_addz(struct sbuf *s, const char *z);
void sb_putc(struct sbuf *s, char c);
void sb_free(struct sbuf *s);
jsval sb_to_val(js *J, struct sbuf *s);

jsobj *lib_ctor(js *J, const char *name, js_native fn, int nargs, jsobj *proto);
void lib_def(js *J, jsobj *o, const char *name, js_native fn, int nargs);
void lib_const(js *J, jsobj *o, const char *name, jsval v);
void lib_getter(js *J, jsobj *o, const char *name, js_native fn);
double arg_int(js *J, jsval v, double dflt);                 /* ToIntegerOrInfinity, default when undefined */
jsstr *this_string(js *J, jsval thisv);                       /* ToString(this) for String.prototype methods */
jsobj *this_array(js *J, jsval thisv);                        /* real array, or a snapshot of an array-like */

void js_init_lib2(js *J);                                     /* String, Date, Map/Set, URI (js_lib2.c) */

#endif
