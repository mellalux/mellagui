// ---- generators
function* counter(n) { for (var i = 0; i < n; i++) yield i; return 'end'; }
var g = counter(3); eq([g.next().value, g.next().value, g.next().value], [0, 1, 2], 'next'); var last = g.next(); eq([last.value, last.done], ['end', true], 'return value'); eq(g.next().done, true, 'stays done');
eq([...counter(4)], [0, 1, 2, 3], 'spread'); eq(Array.from(counter(2)), [0, 1], 'Array.from'); var got = []; for (var v of counter(3)) got.push(v); eq(got, [0, 1, 2], 'for-of');
function* echo() { var x = yield 'first'; var y = yield 'got ' + x; return 'done ' + y; } var e = echo(); eq([e.next().value, e.next('A').value, e.next('B').value], ['first', 'got A', 'done B'], 'sending values');
function* inf() { var i = 0; while (true) yield i++; } var firstFive = []; for (var q of inf()) { if (q >= 5) break; firstFive.push(q); } eq(firstFive, [0, 1, 2, 3, 4], 'break out of an infinite generator'); var [i0, i1, i2] = inf(); eq([i0, i1, i2], [0, 1, 2], 'destructure an infinite generator');
function* deleg() { yield 'a'; var r = yield* counter(2); yield r; yield* [10, 20]; yield* 'xy'; } eq([...deleg()], ['a', 0, 1, 'end', 10, 20, 'x', 'y'], 'yield*');
function* guarded() { try { yield 1; yield 2; } finally { log.push('cleanup'); } } var log = []; var gg = guarded(); gg.next(); var ret = gg.return('bye'); eq([ret.value, ret.done, log], ['bye', true, ['cleanup']], 'return() runs finally');
var tg = (function* () { try { yield 1; } catch (err) { yield 'caught ' + err; } })(); tg.next(); eq(tg.throw('oops').value, 'caught oops', 'throw() is catchable'); throws(function () { (function* () { yield 1; })().throw(new Error('direct')); }, 'direct', 'throw before start');
function* thrower() { yield 1; throw new Error('inside'); } var th = thrower(); th.next(); throws(function () { th.next(); }, 'inside', 'exception propagates'); eq(th.next().done, true, 'generator is done after an exception');
var obj = { *gen() { yield 'm1'; yield 'm2'; }, items: [1, 2], [Symbol.iterator]: function* () { yield* this.items; } }; eq([...obj.gen()], ['m1', 'm2'], 'generator method'); eq([...obj], [1, 2], 'custom iterable with a generator');
class Tree { constructor() { this.kids = ['a', 'b']; } *[Symbol.iterator]() { for (var k of this.kids) yield k.toUpperCase(); } static *range(n) { for (var i = 0; i < n; i++) yield i; } } eq([...new Tree()], ['A', 'B'], 'class iterator'); eq([...Tree.range(3)], [0, 1, 2], 'static generator');
var manual = { [Symbol.iterator]() { var i = 0; return { next() { return i < 3 ? { value: i++, done: false } : { value: undefined, done: true }; } }; } }; eq([...manual], [0, 1, 2], 'manual iterator'); eq(Array.from(manual, function (x) { return x * 2; }), [0, 2, 4], 'Array.from iterable + map');
eq(new Map(function* () { yield ['k', 1]; }()).get('k'), 1, 'Map from generator'); eq(new Set(counter(3)).size, 3, 'Set from generator'); eq(typeof counter(1).next, 'function', 'generator object'); eq(counter(1)[Symbol.iterator]() !== undefined, true, 'is iterable');
function* fib() { var a = 0, b = 1; while (true) { yield a; var t = a + b; a = b; b = t; } } var f10 = []; for (var x of fib()) { if (f10.length == 10) break; f10.push(x); } eq(f10, [0, 1, 1, 2, 3, 5, 8, 13, 21, 34], 'fibonacci');
throws(function () { new counter(1); }, 'not a constructor', 'generator is not a constructor');
// ---- async functions: everything is collected in `order` and checked after the microtasks have run
var order = [];
async function a1() { order.push('a1 start'); var v = await 5; order.push('a1 got ' + v); var w = await Promise.resolve(6); order.push('a1 got ' + w); return v + w; }
var p1 = a1(); order.push('after call'); p1.then(function (v) { order.push('a1 result ' + v); });
async function a2() { throw new Error('async boom'); } a2().catch(function (e) { order.push('a2 caught ' + e.message); });
async function a3() { try { await Promise.reject(new Error('rejected')); } catch (e) { order.push('a3 caught ' + e.message); return 'recovered'; } finally { order.push('a3 finally'); } } a3().then(function (v) { order.push('a3 ' + v); });
var arrow = async (x) => x * 2; arrow(21).then(function (v) { order.push('arrow ' + v); }); var arrow1 = async x => await x + 1; arrow1(1).then(function (v) { order.push('arrow1 ' + v); });
var objm = { async m(x) { return x + '!'; } }; objm.m('hi').then(function (v) { order.push('method ' + v); });
class K { constructor() { this.v = 7; } async get() { await null; return this.v; } static async make() { return new K(); } } K.make().then(function (k) { return k.get(); }).then(function (v) { order.push('class ' + v); });
async function sequential() { var out = []; for (var i = 0; i < 3; i++) out.push(await Promise.resolve(i * 10)); return out; } sequential().then(function (v) { order.push('seq ' + v.join()); });
async function parallel() { var r = await Promise.all([1, 2, 3].map(async function (x) { await null; return x * x; })); return r; } parallel().then(function (v) { order.push('par ' + v.join()); });
async function forAwait() { var s = 0; for await (var x of [Promise.resolve(1), 2, Promise.resolve(3)]) s += x; return s; } forAwait().then(function (v) { order.push('forawait ' + v); });
async function nested() { return await a1b(); } async function a1b() { return await 'nested ok'; } nested().then(function (v) { order.push(v); });
(async function () { var r = await new Promise(function (res) { setTimeout(function () { res('timer'); }, 0); }); order.push('iife ' + r); })();
async function deep(n) { return n == 0 ? 0 : 1 + await deep(n - 1); } deep(15).then(function (v) { order.push('deep ' + v); });
eq(typeof a1().then, 'function', 'async function returns a promise'); eq(a1() instanceof Promise, true, 'instanceof Promise'); eq(order.slice(0, 2), ['a1 start', 'after call'], 'runs synchronously until the first await');
throws(function () { new a1(); }, 'not a constructor', 'async function is not a constructor');
var done = false;
function checkLater(n) {
  if (n > 0) return Promise.resolve().then(function () { checkLater(n - 1); });
  ['a1 got 5', 'a1 got 6', 'a1 result 11', 'a2 caught async boom', 'a3 caught rejected', 'a3 finally', 'a3 recovered', 'arrow 42', 'arrow1 2', 'method hi!', 'class 7', 'seq 0,10,20', 'par 1,4,9', 'forawait 6', 'nested ok', 'deep 15'].forEach(function (x) { eq(order.indexOf(x) >= 0, true, 'async: ' + x); });
  eq(order.indexOf('a1 got 5') < order.indexOf('a1 got 6'), true, 'await order');
}
checkLater(40);
// ---- async generators and for await over async iterators
var aorder = [];
async function* agen() { aorder.push('start'); var x = await Promise.resolve(1); yield x; yield 2; try { yield 3; } finally { aorder.push('cleanup'); } return 'end'; }
(async function () {
  var seen = [];
  for await (var v of agen()) seen.push(v);
  aorder.push('forawait ' + seen.join());
  var g = agen(); var r1 = await g.next(); var r2 = await g.next(); var r3 = await g.next(); var r4 = await g.return('early');
  aorder.push('manual ' + [r1.value, r2.value, r3.value, r4.value, r4.done].join());
  var parallel = agen(); var ps = [parallel.next(), parallel.next(), parallel.next(), parallel.next()]; var rs = await Promise.all(ps);
  aorder.push('queued ' + rs.map(function (r) { return r.value + ':' + r.done; }).join());
  var custom = { [Symbol.asyncIterator]() { var i = 0; return { next() { return Promise.resolve(i < 3 ? { value: i++, done: false } : { done: true }); } }; } };
  var got = []; for await (var c of custom) got.push(c); aorder.push('custom ' + got.join());
  var thrower = (async function* () { yield 1; throw new Error('agen boom'); })(); await thrower.next(); try { await thrower.next(); } catch (e) { aorder.push('caught ' + e.message); }
  class Src { async *items() { yield 'a'; yield 'b'; } } var items = []; for await (var it of new Src().items()) items.push(it); aorder.push('class ' + items.join(''));
})();
function checkAgen(n) {
  if (n > 0) return Promise.resolve().then(function () { checkAgen(n - 1); });
  ['start', 'forawait 1,2,3', 'cleanup', 'manual 1,2,3,early,true', 'queued 1:false,2:false,3:false,end:true', 'custom 0,1,2', 'caught agen boom', 'class ab'].forEach(function (x) { eq(aorder.indexOf(x) >= 0, true, 'async generator: ' + x); });
}
checkAgen(80);
