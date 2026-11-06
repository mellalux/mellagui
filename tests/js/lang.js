// ---- operators and conversions
eq(1 + 2 * 3, 7, 'precedence');
eq(7 % 3, 1, 'mod'); eq(-7 % 3, -1, 'neg mod'); eq(2 ** 10, 1024, 'exp'); eq(2 ** 3 ** 2, 512, 'exp right assoc');
eq('5' * '2', 10, 'str mul'); eq('5' + 2, '52', 'str add'); eq(5 + '2', '52', 'num+str'); eq(1 + null, 1, 'null');
eq(1 + undefined, null, 'NaN'); eq(true + true, 2, 'bool'); eq([] + [], '', 'arr+arr'); eq([1,2] + '', '1,2', 'arr str');
eq({} + '', '[object Object]', 'obj str');
eq(1 / 0, null, 'inf json'); eq(isFinite(1 / 0), false, 'isFinite'); eq(0.1 + 0.2, 0.30000000000000004, 'float');
eq(String(0.1 + 0.2), '0.30000000000000004', 'float str'); eq(String(1e21), '1e+21', '1e21'); eq(String(1e-7), '1e-7', '1e-7');
eq(String(123456789012345680000), '123456789012345680000', 'big'); eq(String(0.000001), '0.000001', 'small'); eq(String(-0), '0', '-0');
eq(String(1.5e300), '1.5e+300', '1.5e300'); eq(String(100), '100', '100'); eq(String(2.5), '2.5', '2.5');
eq(5 / 2, 2.5, 'div'); eq(5 >> 1, 2, 'shr'); eq(-5 >>> 0, 4294967291, 'ushr'); eq(1 << 31, -2147483648, 'shl');
eq(5 & 3, 1, 'and'); eq(5 | 3, 7, 'or'); eq(5 ^ 3, 6, 'xor'); eq(~5, -6, 'not');
eq(null == undefined, true, 'null==undef'); eq(null === undefined, false, 'null===undef'); eq(0 == '', true, '0==""');
eq('1' == 1, true, '1==1'); eq(NaN == NaN, false, 'nan'); eq([1] == 1, true, '[1]==1'); eq('b' > 'a', true, 'str cmp');
eq('10' < '9', true, 'str lt'); eq(10 < '9', false, 'num str lt'); eq(null >= 0, true, 'null>=0');
eq(typeof 1, 'number', 'typeof'); eq(typeof 'a', 'string', 'typeof s'); eq(typeof null, 'object', 'typeof null');
eq(typeof function () {}, 'function', 'typeof fn'); eq(typeof {}, 'object', 'typeof obj'); eq(typeof undefined, 'undefined', 'tu');
eq(!!'' , false, 'falsy'); eq(!!'0', true, 'truthy 0 str'); eq(!![] , true, 'truthy arr'); eq(!!NaN, false, 'NaN falsy');
eq(null ?? 'd', 'd', 'nullish'); eq(0 ?? 'd', 0, 'nullish 0'); eq(0 || 'd', 'd', 'or'); eq(1 && 2, 2, 'and');
eq(void 0, undefined, 'void'); eq((1, 2, 3), 3, 'comma');
var a = 1; a += 2; a *= 3; a -= 1; a /= 2; a %= 3; eq(a, 1, 'compound'); a **= 3; eq(a, 1, 'exp assign');
var b; b ||= 5; eq(b, 5, '||='); b &&= 6; eq(b, 6, '&&='); var c = null; c ??= 7; eq(c, 7, '??=');
var i = 0; eq(i++, 0, 'post'); eq(++i, 2, 'pre'); eq(i--, 2, 'post dec'); eq(--i, 0, 'pre dec');
eq(0.1 * 3, 0.30000000000000004, 'f'); eq(parseInt('42px'), 42, 'parseInt'); eq(parseInt('0x1f'), 31, 'hex'); eq(parseInt('z', 36), 35, 'radix');
eq(parseFloat('3.14abc'), 3.14, 'parseFloat'); eq(Number('  12 '), 12, 'Number trim'); eq(Number(''), 0, 'Number empty'); eq(Number('1x'), null, 'NaN');
eq(Number(null), 0, 'Number null'); eq(Number([5]), 5, 'Number arr'); eq(+true, 1, 'unary plus');
// ---- strings, escapes, templates
eq('a\tb\n'.length, 4, 'escapes'); eq('A\x42', 'AB', 'unicode esc'); eq("it's", 'it\'s', 'quotes');
var name = 'World', n = 3; eq(`Hello ${name}! ${n * 2} ${'x' + `y${1}`}`, 'Hello World! 6 xy1', 'template');
eq(`a
b`, 'a\nb', 'multiline template'); eq(`\${}`, '${}', 'escaped dollar'); eq(`${{a: 1}.a}`, '1', 'obj in template');
// ---- control flow
var s = 0; for (var k = 0; k < 10; k++) { if (k == 3) continue; if (k == 7) break; s += k; } eq(s, 0 + 1 + 2 + 4 + 5 + 6, 'for');
var w = 0; while (w < 5) w++; eq(w, 5, 'while'); var dw = 0; do { dw++; } while (dw < 3); eq(dw, 3, 'do');
outer: for (var x = 0; x < 3; x++) { for (var y = 0; y < 3; y++) { if (y == 1) continue outer; if (x == 2) break outer; s += 100; } } eq(s, 18 + 200, 'labels');
function sw(v) { switch (v) { case 1: return 'one'; case 2: case 3: return 'two-three'; default: return 'other'; } }
eq([sw(1), sw(2), sw(3), sw(9)], ['one', 'two-three', 'two-three', 'other'], 'switch');
function sw2(v) { var r = ''; switch (v) { case 1: r += 'a'; case 2: r += 'b'; break; case 3: r += 'c'; } return r; } eq([sw2(1), sw2(2), sw2(3)], ['ab', 'b', 'c'], 'fallthrough');
var forin = []; for (var key in { p: 1, q: 2 }) forin.push(key); eq(forin, ['p', 'q'], 'for in');
var arrk = []; for (var idx in ['x', 'y']) arrk.push(idx); eq(arrk, ['0', '1'], 'for in array');
var forof = []; for (var v of [1, 2, 3]) forof.push(v * 2); eq(forof, [2, 4, 6], 'for of');
var chars = []; for (var ch of 'hi') chars.push(ch); eq(chars, ['h', 'i'], 'for of string');
eq((function () { try { return 1; } finally { s = 'fin'; } })(), 1, 'finally return'); eq(s, 'fin', 'finally ran');
eq((function () { for (var i = 0; i < 3; i++) { try { if (i == 1) continue; } finally { s += i; } } return s; })(), 'fin012', 'finally continue');
eq((function () { try { throw new Error('x'); } catch (e) { return e.message; } })(), 'x', 'catch');
eq((function () { try { null.x; } catch (e) { return e instanceof TypeError; } })(), true, 'TypeError');
eq((function () { try { undefinedVar; } catch (e) { return e.name; } })(), 'ReferenceError', 'ReferenceError');
eq((function () { try { try { throw 1; } finally { s = 'inner'; } } catch (e) { return e + s; } })(), '1inner', 'nested try');
eq((function () { try { throw { code: 5 }; } catch ({ code }) { return code; } })(), 5, 'catch destructure');
eq((function () { try { throw 1; } catch { return 'no binding'; } })(), 'no binding', 'optional catch binding');
// ---- functions and closures
function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); } eq(fact(10), 3628800, 'recursion');
function counter() { var c = 0; return { inc: function () { return ++c; }, get: () => c }; } var ctr = counter(); ctr.inc(); ctr.inc(); eq(ctr.get(), 2, 'closure');
var fs = []; for (var q = 0; q < 3; q++) fs.push(function () { return q; }); eq(fs.map(f => f()), [3, 3, 3], 'var closure');
var gs = []; for (let q = 0; q < 3; q++) gs.push(() => q); eq(gs.map(f => f()), [0, 1, 2], 'let closure');
var hs = []; for (const z of [7, 8]) hs.push(() => z); eq(hs.map(f => f()), [7, 8], 'const of closure');
eq(hoisted(), 'ok', 'function hoisting'); function hoisted() { return 'ok'; }
eq(typeof hv, 'undefined', 'var hoisting'); var hv = 1;
function dflt(a, b = a + 1, ...rest) { return [a, b, rest]; } eq(dflt(1), [1, 2, []], 'default'); eq(dflt(1, 5, 6, 7), [1, 5, [6, 7]], 'rest');
eq(dflt.length, 1, 'fn length'); eq(dflt.name, 'dflt', 'fn name');
function args() { return arguments.length + ':' + Array.prototype.slice.call(arguments).join(','); } eq(args(1, 2, 3), '3:1,2,3', 'arguments');
eq(Math.max(...[1, 5, 3]), 5, 'spread call'); eq([...'ab', ...[1, 2]], ['a', 'b', 1, 2], 'spread array');
eq({ ...{ a: 1, b: 2 }, b: 3 }, { a: 1, b: 3 }, 'spread object');
var addf = (a, b) => a + b; eq(addf(1, 2), 3, 'arrow'); var sq = x => x * x; eq(sq(4), 16, 'arrow single'); eq((() => ({ a: 1 }))(), { a: 1 }, 'arrow obj');
var obj = { vv: 42, reg: function () { return this.vv; }, arr() { return (() => this.vv)(); } }; eq(obj.reg(), 42, 'this'); eq(obj.arr(), 42, 'arrow this');
var detached = obj.reg; eq(typeof detached(), 'undefined', 'lost this');
eq(obj.reg.call({ vv: 7 }), 7, 'call'); eq(obj.reg.apply({ vv: 8 }, []), 8, 'apply'); eq(obj.reg.bind({ vv: 9 })(), 9, 'bind');
eq(function (a, b) { return a + b; }.bind(null, 1)(2), 3, 'bind partial');
eq((function f(n) { return n ? n + f(n - 1) : 0; })(4), 10, 'named fn expr'); eq((function () { return typeof f2; var f2 = 1; })(), 'undefined', 'inner var');
eq(((a, b) => a + b).toString(), '(a, b) => a + b', 'toString');
throws(function r() { r(); }, 'call stack', 'stack overflow');
throws(function () { nonexistent.y; }, 'not defined', 'undef var'); throws(function () { var u; u.p; }, 'undefined', 'undef prop');
throws(function () { (void 0)(); }, 'not a function', 'call non fn'); throws(function () { const c = 1; c = 2; }, 'constant', 'const assign');
throws(function () { new (function () {}.bind())(); return 1; } && function () { null(); }, 'not a function', 'null call');
// ---- objects
var o2 = { a: 1, 'b c': 2, 3: 'three', [`k${1}`]: 'computed', get g() { return this.a + 1; }, set g(v) { this.a = v; } };
eq(o2.a, 1, 'prop'); eq(o2['b c'], 2, 'string key'); eq(o2[3], 'three', 'num key'); eq(o2.k1, 'computed', 'computed'); eq(o2.g, 2, 'getter'); o2.g = 10; eq(o2.a, 10, 'setter');
eq(Object.keys({ b: 1, a: 2, 1: 'x', 0: 'y' }), ['0', '1', 'b', 'a'], 'key order'); eq(Object.entries({ a: 1 }), [['a', 1]], 'entries'); eq(Object.values({ a: 1, b: 2 }), [1, 2], 'values');
eq('a' in { a: 1 }, true, 'in'); eq(delete o2.a, true, 'delete'); eq('a' in o2, false, 'deleted'); eq(o2.hasOwnProperty('g'), true, 'hasOwn');
var { a: da, b: db = 5, ...rest2 } = { a: 1, c: 3, d: 4 }; eq([da, db, rest2], [1, 5, { c: 3, d: 4 }], 'object destructuring');
var [e1, , e3 = 9, ...e4] = [1, 2, undefined, 4, 5]; eq([e1, e3, e4], [1, 9, [4, 5]], 'array destructuring');
var sw1 = 1, sw3 = 2; [sw1, sw3] = [sw3, sw1]; eq([sw1, sw3], [2, 1], 'swap'); var nested = { p: { q: [10, 20] } }; var { p: { q: [, second] } } = nested; eq(second, 20, 'nested destructure');
function dpar({ x, y = 2 }, [z]) { return x + y + z; } eq(dpar({ x: 1 }, [3]), 6, 'param destructure');
eq(o2?.zz?.yy, undefined, 'optional chain'); eq(o2?.['k1'], 'computed', 'optional computed'); eq(o2.nope?.(), undefined, 'optional call'); eq(null?.a.b.c, undefined, 'chain short circuit');
eq(Object.getPrototypeOf([]) === Array.prototype, true, 'proto'); eq({}.toString(), '[object Object]', 'obj toString'); eq(Object.prototype.toString.call([]), '[object Array]', 'toString array');
var frozen = Object.freeze({ f: 1 }); frozen.f = 2; eq(frozen.f, 1, 'freeze'); eq(Object.isFrozen(frozen), true, 'isFrozen');
var dp = {}; Object.defineProperty(dp, 'hid', { value: 1, enumerable: false }); eq(Object.keys(dp), [], 'non-enumerable'); eq(dp.hid, 1, 'defined');
Object.defineProperty(dp, 'acc', { get: function () { return 'got'; }, enumerable: true }); eq(dp.acc, 'got', 'defineProperty getter'); eq(Object.keys(dp), ['acc'], 'enumerable getter');
eq(JSON.stringify(Object.assign({}, { a: 1 }, { b: 2 })), '{"a":1,"b":2}', 'assign'); eq(Object.fromEntries([['a', 1], ['b', 2]]), { a: 1, b: 2 }, 'fromEntries');
var proto = { hello() { return 'hi ' + this.n; } }; var child = Object.create(proto); child.n = 'c'; eq(child.hello(), 'hi c', 'Object.create'); eq(proto.isPrototypeOf(child), true, 'isPrototypeOf');
// ---- prototypes and classes
function Animal(name) { this.name = name; } Animal.prototype.speak = function () { return this.name + ' makes a sound'; };
function Dog(name) { Animal.call(this, name); } Dog.prototype = Object.create(Animal.prototype); Dog.prototype.constructor = Dog; Dog.prototype.speak = function () { return Animal.prototype.speak.call(this) + ' (woof)'; };
var d = new Dog('Rex'); eq(d.speak(), 'Rex makes a sound (woof)', 'prototype chain'); eq(d instanceof Animal, true, 'instanceof'); eq(d.constructor === Dog, true, 'constructor');
class Shape { constructor(n) { this.n = n; } area() { return 0; } toString() { return `${this.n}:${this.area()}`; } static make(n) { return new this(n); } get label() { return 'L' + this.n; } }
class Circle extends Shape { constructor(r) { super('circle'); this.r = r; } area() { return Math.round(Math.PI * this.r * this.r); } static unit() { return new Circle(1); } }
var ci = new Circle(2); eq(String(ci), 'circle:13', 'class'); eq(Circle.unit().area(), 3, 'static'); eq(ci instanceof Shape, true, 'class instanceof'); eq(ci.label, 'Lcircle', 'class getter');
eq(Shape.make('s').n, 's', 'static this'); eq(typeof Shape, 'function', 'class typeof'); throws(function () { Shape(); }, "without 'new'", 'class call');
class Counter { count = 0; static total = 5; inc() { this.count++; return this; } } eq(new Counter().inc().inc().count, 2, 'class field'); eq(Counter.total, 5, 'static field');
class Sub extends Counter { count = 10; inc() { super.inc(); return this; } } eq(new Sub().inc().count, 11, 'super method + field override');
class MyErr extends Error { constructor(m) { super(m); this.name = 'MyErr'; } } var me = new MyErr('boom'); eq([me.message, me.name, me instanceof Error, me instanceof MyErr, String(me)], ['boom', 'MyErr', true, true, 'MyErr: boom'], 'extends Error');
class Stack2 extends Array { top() { return this[this.length - 1]; } } var st = new Stack2(); st.push(1, 2); eq(st.top(), 2, 'extends Array'); eq(st.length, 2, 'extends Array length');
// ---- misc
eq(typeof globalThis, 'object', 'globalThis'); eq(isNaN('abc'), true, 'isNaN'); eq(void 0 === undefined, true, 'undefined');
var cnt = 0; var lz = { get x() { cnt++; return 1; } }; lz.x; lz.x; eq(cnt, 2, 'getter calls');
eq([1, 2, 3].indexOf(2), 1, 'indexOf'); eq(new Date(0).getTime(), 0, 'Date');
eq((function () { 'use strict'; return typeof this; })(), 'undefined' === typeof undefined ? typeof this : '', 'strict this is ignored');

// ---- objects with many properties (hash index)
var big = {}; for (var bi = 0; bi < 40; bi++) big['k' + bi] = bi;
eq([big.k0, big.k39, 'k20' in big, big.hasOwnProperty('k21'), big.hasOwnProperty('nope'), Object.keys(big).length], [0, 39, true, true, false, 40], 'big object');
delete big.k5; eq([big.k5, 'k5' in big, big.k6, Object.keys(big).length], [undefined, false, 6, 39], 'big object delete'); big.k5 = 'again'; eq(Object.keys(big)[39], 'k5', 'key order after re-add');
var lit = { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, i: 9, j: 10, k: 11, l: 12 }; eq(Object.prototype.hasOwnProperty.call(lit, 'l'), true, 'hasOwnProperty on a 12 property literal');
throws(function () { Object.prototype.hasOwnProperty.call(undefined, 'x'); }, 'undefined or null', 'hasOwnProperty on undefined');
// ---- with, eval, Function
var wo = { wa: 1 }; with (wo) { wa = 5; var wv = wa + 1; } eq([wo.wa, wv], [5, 6], 'with'); eq(new Function('a', 'b', 'return a * b')(6, 7), 42, 'Function ctor'); eq(eval('1 + 2'), 3, 'eval'); eq(Function('return 5')(), 5, 'Function call');
