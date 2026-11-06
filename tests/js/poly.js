// Promise: results are collected by microtasks, checked at the end by a final queued task
var log = [];
Promise.resolve(1).then(function (v) { log.push('a' + v); return v + 1; }).then(function (v) { log.push('b' + v); throw new Error('boom'); }).catch(function (e) { log.push('c' + e.message); return 'rec'; }).then(function (v) { log.push('d' + v); });
new Promise(function (res) { log.push('executor'); res('x'); }).finally(function () { log.push('fin'); });
Promise.all([1, Promise.resolve(2), new Promise(function (r) { r(3); })]).then(function (v) { log.push('all' + v.join('')); });
Promise.race([new Promise(function () {}), Promise.resolve('fast')]).then(function (v) { log.push('race' + v); });
Promise.allSettled([Promise.reject('no'), 1]).then(function (v) { log.push('settled' + v.map(function (r) { return r.status; }).join()); });
Promise.any([Promise.reject(1), Promise.resolve('any')]).then(function (v) { log.push('any' + v); });
Promise.reject('r').then(function () { log.push('never'); }, function (e) { log.push('rejected-' + e); });
var thenable = { then: function (res) { res('thenable'); } }; Promise.resolve(thenable).then(function (v) { log.push(v); });
log.push('sync');
queueMicrotask(function () { log.push('micro'); });
eq(log, ['executor', 'sync'], 'nothing runs before the script ends');
eq(typeof Promise.prototype.then, 'function', 'then'); eq(Object.keys(new Promise(function () {})), [], 'promise has no enumerable keys'); eq(new Promise(function () {}) instanceof Promise, true, 'instanceof');
throws(function () { Promise(function () {}); }, 'without', 'new required'); throws(function () { new Promise(); }, 'not a function', 'executor required');
// Symbol-lite
var s1 = Symbol('a'), s2 = Symbol('a'); eq(s1 === s2, false, 'unique symbols'); var so = {}; so[s1] = 1; so.x = 2; eq(Object.keys(so), ['x'], 'symbol keys are hidden'); eq(so[s1], 1, 'symbol key works'); eq(JSON.stringify(so), '{"x":2}', 'JSON hides symbols');
eq(Symbol.for('k') === Symbol.for('k'), true, 'Symbol.for'); eq(typeof Symbol.iterator, 'string', 'well-known symbols exist');
// WeakMap, typed arrays, Reflect, Intl
var wm = new WeakMap(), key = {}; wm.set(key, 5); eq([wm.get(key), wm.has({})], [5, false], 'WeakMap'); var ws = new WeakSet(); ws.add(key); eq(ws.has(key), true, 'WeakSet');
var u8 = new Uint8Array(3); eq([u8.length, u8[0]], [3, 0], 'Uint8Array'); eq(new Float32Array([1.5, 2]).length, 2, 'typed array from array'); eq(Uint8Array.BYTES_PER_ELEMENT, 1, 'BYTES_PER_ELEMENT');
eq([Reflect.has({ a: 1 }, 'a'), Reflect.ownKeys({ a: 1, b: 2 }), Reflect.getPrototypeOf([]) === Array.prototype, Reflect.apply(Math.max, null, [1, 3])], [true, ['a', 'b'], true, 3], 'Reflect');
eq(new Intl.NumberFormat('en-US').format(1234567.891), '1,234,567.891', 'Intl.NumberFormat'); eq(new Intl.NumberFormat('en-US', { maximumFractionDigits: 0 }).format(2.5), '3', 'Intl fraction digits');
// the log is checked after the whole microtask chain has run (js_run drains the queue before returning, so check via a late task)
function late(n, fn) { if (n == 0) return fn(); Promise.resolve().then(function () { late(n - 1, fn); }); }
late(10, function () {
  eq(log.indexOf('sync') < log.indexOf('a1'), true, 'then callbacks are asynchronous');
  eq(log.slice(0, 2), ['executor', 'sync'], 'synchronous part first');
  ['a1', 'b2', 'cboom', 'drec', 'fin', 'all123', 'racefast', 'settledrejected,fulfilled', 'anyany', 'rejected-r', 'thenable', 'micro'].forEach(function (x) { eq(log.indexOf(x) >= 0, true, 'ran ' + x); });
  eq(log.indexOf('never'), -1, 'rejected promise skips onFulfilled');
  eq(log.indexOf('a1') < log.indexOf('b2') && log.indexOf('b2') < log.indexOf('cboom') && log.indexOf('cboom') < log.indexOf('drec'), true, 'chain order');
});
// ---- URL, URLSearchParams
var u = new URL('https://user:pw@Example.com:8080/a/b/../c.html?x=1&y=two%20words#frag');
eq([u.protocol, u.hostname, u.port, u.host, u.origin, u.pathname.indexOf('/a/b'), u.search, u.hash, u.username], ['https:', 'example.com', '8080', 'example.com:8080', 'https://example.com:8080', 0, '?x=1&y=two%20words', '#frag', 'user'], 'URL parts');
eq(u.searchParams.get('y'), 'two words', 'searchParams.get'); u.searchParams.set('x', '2'); u.searchParams.append('z', 'a b'); eq(u.search, '?x=2&y=two+words&z=a+b', 'searchParams updates the URL');
eq(new URL('/p/q?r=1', 'http://h.org/a/b').href, 'http://h.org/p/q?r=1', 'relative to origin'); eq(new URL('c', 'http://h.org/a/b').href, 'http://h.org/a/c', 'relative to directory'); eq(new URL('../x', 'http://h.org/a/b/c').pathname, '/a/x', 'dot segments'); throws(function () { new URL('nope'); }, 'Invalid URL', 'invalid URL');
var sp = new URLSearchParams('a=1&b=2&a=3'); eq([sp.get('a'), sp.getAll('a'), sp.has('b'), sp.has('z'), sp.get('z')], ['1', ['1', '3'], true, false, null], 'URLSearchParams'); sp.delete('a'); eq(sp.toString(), 'b=2', 'delete'); eq(new URLSearchParams({ q: 'x y', n: 1 }).toString(), 'q=x+y&n=1', 'from object'); eq(String(new URLSearchParams('?k=v')), 'k=v', 'leading ?');
