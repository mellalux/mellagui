// ---- Array
var a = [3, 1, 2];
eq(a.length, 3, 'length'); a.push(4, 5); eq(a, [3, 1, 2, 4, 5], 'push'); eq(a.pop(), 5, 'pop'); eq(a.shift(), 3, 'shift'); a.unshift(0, 9); eq(a, [0, 9, 1, 2, 4], 'unshift');
eq(a.slice(1, 3), [9, 1], 'slice'); eq(a.slice(-2), [2, 4], 'slice neg'); eq(a.splice(1, 2, 'x', 'y', 'z'), [9, 1], 'splice ret'); eq(a, [0, 'x', 'y', 'z', 2, 4], 'splice');
eq([1, 2].concat([3], 4, [[5]]), [1, 2, 3, 4, [5]], 'concat'); eq([1, 2, 3].reverse(), [3, 2, 1], 'reverse'); eq([1, [2, [3, [4]]]].flat(2), [1, 2, 3, [4]], 'flat');
eq([1, 2, 3].join(), '1,2,3', 'join'); eq([1, null, undefined, 2].join('-'), '1---2', 'join null'); eq([[1, 2], [3]].toString(), '1,2,3', 'nested toString');
eq([5, 1, 10].sort(), [1, 10, 5], 'default sort'); eq([5, 1, 10].sort((x, y) => x - y), [1, 5, 10], 'numeric sort'); eq(['b', 'a'].sort(), ['a', 'b'], 'str sort');
var stable = [{ k: 1, v: 'a' }, { k: 0, v: 'b' }, { k: 1, v: 'c' }, { k: 0, v: 'd' }].sort((x, y) => x.k - y.k).map(o => o.v).join(''); eq(stable, 'bdac', 'stable sort');
eq([1, 2, 3].indexOf(4), -1, 'indexOf miss'); eq([1, 2, 1].lastIndexOf(1), 2, 'lastIndexOf'); eq([NaN].includes(NaN), true, 'includes NaN'); eq([1, 2, 3].includes(2), true, 'includes');
eq([1, 2, 3, 4].filter(x => x % 2 == 0), [2, 4], 'filter'); eq([1, 2, 3].reduce((s, x) => s + x, 10), 16, 'reduce'); eq([1, 2, 3].reduceRight((s, x) => s + x), 6, 'reduceRight');
throws(function () { [].reduce((a, b) => a); }, 'empty array', 'reduce empty'); eq([1, 2, 3].some(x => x > 2), true, 'some'); eq([1, 2, 3].every(x => x > 2), false, 'every');
eq([1, 2, 3].find(x => x > 1), 2, 'find'); eq([1, 2, 3].findIndex(x => x > 1), 1, 'findIndex'); eq([1, 2, 3].findLast(x => x < 3), 2, 'findLast'); eq([1, 2, 3].at(-1), 3, 'at');
eq(new Array(3).length, 3, 'new Array(n)'); eq(new Array(1, 2), [1, 2], 'new Array(items)'); eq(Array(2, 3), [2, 3], 'Array()'); eq(Array.from('abc'), ['a', 'b', 'c'], 'Array.from str');
eq(Array.from([1, 2], x => x * 2), [2, 4], 'Array.from map'); eq(Array.from(new Set([1, 1, 2])), [1, 2], 'Array.from set'); eq(Array.of(7), [7], 'Array.of'); eq(Array.isArray([]), true, 'isArray');
eq(new Array(3).fill(0), [0, 0, 0], 'fill'); eq([1, 2, 3].map((x, i) => x * i), [0, 2, 6], 'map index'); eq([[1, 2], [3, 4]].map(([x, y]) => x + y), [3, 7], 'map destructure');
eq([1, 2, 3].flatMap(x => [x, x]), [1, 1, 2, 2, 3, 3], 'flatMap'); eq(Array.from([1, 2].entries()), [[0, 1], [1, 2]], 'entries'); eq([..."ab"].keys().length, 2, 'keys');
var arr = []; arr[5] = 1; eq(arr.length, 6, 'sparse length'); arr.length = 2; eq(arr, [null, null], 'truncate'); var sum = 0; [1, 2, 3].forEach(function (x) { sum += x * this.m; }, { m: 2 }); eq(sum, 12, 'forEach thisArg');
eq(Array.prototype.slice.call({ 0: 'a', 1: 'b', length: 2 }), ['a', 'b'], 'array-like');
// ---- String
var s = 'Hello, World';
eq(s.length, 12, 'length'); eq(s[0], 'H', 'index'); eq(s.charAt(4), 'o', 'charAt'); eq(s.charCodeAt(0), 72, 'charCodeAt'); eq(s.indexOf('o'), 4, 'indexOf'); eq(s.indexOf('o', 5), 8, 'indexOf from');
eq(s.lastIndexOf('o'), 8, 'lastIndexOf'); eq(s.slice(7), 'World', 'slice'); eq(s.slice(-5, -1), 'Worl', 'slice neg'); eq(s.substring(7, 5), ', ', 'substring swap'); eq(s.substr(-5, 2), 'Wo', 'substr');
eq(s.toUpperCase(), 'HELLO, WORLD', 'upper'); eq(s.toLowerCase(), 'hello, world', 'lower'); eq(s.includes('World'), true, 'includes'); eq(s.startsWith('Hello'), true, 'startsWith'); eq(s.endsWith('World'), true, 'endsWith');
eq('  x '.trim(), 'x', 'trim'); eq('  x '.trimStart(), 'x ', 'trimStart'); eq('ab'.repeat(3), 'ababab', 'repeat'); eq('5'.padStart(3, '0'), '005', 'padStart'); eq('5'.padEnd(3, 'ab'), '5ab', 'padEnd');
eq('a,b,,c'.split(','), ['a', 'b', '', 'c'], 'split'); eq('abc'.split(''), ['a', 'b', 'c'], 'split empty'); eq('a b c'.split(' ', 2), ['a', 'b'], 'split limit'); eq('abc'.split(), ['abc'], 'split none');
eq('aXbXc'.replace('X', '-'), 'a-bXc', 'replace first'); eq('aXbXc'.replaceAll('X', '-'), 'a-b-c', 'replaceAll'); eq('abc'.replace('b', m => m.toUpperCase()), 'aBc', 'replace fn'); eq('abc'.replace('b', '[$&]'), 'a[b]c', 'replace $&');
eq('abc'.concat('d', 1), 'abcd1', 'concat'); eq('a'.localeCompare('b'), -1, 'localeCompare'); eq('b' < 'a', false, 'compare'); eq('abc'.at(-1), 'c', 'at'); eq(String.fromCharCode(72, 105), 'Hi', 'fromCharCode');
eq(String(null), 'null', 'String(null)'); eq(String([1, 2]), '1,2', 'String(arr)'); eq(String({}), '[object Object]', 'String(obj)'); eq(new String('x').length, 1, 'String object'); eq(typeof new String('x'), 'object', 'typeof String obj');
eq('äö'.toUpperCase(), 'ÄÖ', 'latin upper'); eq('x'.codePointAt(0), 120, 'codePointAt'); eq(encodeURIComponent('a b&c/d'), 'a%20b%26c%2Fd', 'encodeURIComponent'); eq(decodeURIComponent('a%20b%26'), 'a b&', 'decodeURIComponent');
eq(encodeURI('http://x/a b?q=1'), 'http://x/a%20b?q=1', 'encodeURI'); eq('abc'.match('b')[0], 'b', 'match with a string pattern');
// ---- Number / Math
eq((255).toString(16), 'ff', 'toString radix'); eq((255).toString(2), '11111111', 'toString 2'); eq((0.5).toString(2), '0.1', 'frac radix'); eq((3.14159).toFixed(2), '3.14', 'toFixed'); eq((2.5).toFixed(0), '3', 'toFixed round');
eq((1234.5678).toFixed(1), '1234.6', 'toFixed 1'); eq((0).toFixed(2), '0.00', 'toFixed 0'); eq((1e21).toFixed(2), '1e+21', 'toFixed big'); eq((123.456).toPrecision(4), '123.5', 'toPrecision'); eq((1234567).toLocaleString(), '1,234,567', 'toLocaleString');
eq(Number.isInteger(5), true, 'isInteger'); eq(Number.isInteger(5.5), false, 'isInteger frac'); eq(Number.isNaN('a'), false, 'Number.isNaN'); eq(Number.MAX_SAFE_INTEGER, 9007199254740991, 'MAX_SAFE_INTEGER'); eq(Number.EPSILON > 0, true, 'EPSILON');
eq(Math.max(1, 3, 2), 3, 'max'); eq(Math.min(), null, 'min empty inf'); eq(Math.max(), null, 'max empty'); eq(Math.floor(-1.5), -2, 'floor'); eq(Math.ceil(1.2), 2, 'ceil'); eq(Math.round(2.5), 3, 'round'); eq(Math.round(-2.5), -2, 'round neg');
eq(Math.trunc(-1.9), -1, 'trunc'); eq(Math.sign(-3), -1, 'sign'); eq(Math.abs(-2), 2, 'abs'); eq(Math.sqrt(16), 4, 'sqrt'); eq(Math.pow(2, 8), 256, 'pow'); eq(Math.hypot(3, 4), 5, 'hypot'); eq(Math.cbrt(27), 3, 'cbrt');
eq(Math.PI.toFixed(5), '3.14159', 'PI'); eq(Math.atan2(1, 1).toFixed(4), '0.7854', 'atan2'); eq(Math.log10(1000), 3, 'log10'); eq(Math.random() < 1, true, 'random'); eq(Math.max.apply(null, [1, 9, 4]), 9, 'apply max');
eq(0.1 + 0.7, 0.7999999999999999, 'float add'); eq(9007199254740993, 9007199254740992, 'precision'); eq(5e-324 > 0, true, 'min value'); eq(1.7976931348623157e308 * 10, null, 'overflow');
// ---- JSON
eq(JSON.stringify({ a: [1, 'x', null, true], b: { c: undefined, d: function () {} } }), '{"a":[1,"x",null,true],"b":{}}', 'stringify');
eq(JSON.stringify('he said "hi"\n'), '"he said \\"hi\\"\\n"', 'stringify escapes'); eq(JSON.stringify([undefined, function () {}]), '[null,null]', 'stringify arr undefined');
eq(JSON.stringify({ a: 1, b: [1, 2] }, null, 2), '{\n  "a": 1,\n  "b": [\n    1,\n    2\n  ]\n}', 'indent'); eq(JSON.stringify({ a: 1, b: 2 }, ['b']), '{"b":2}', 'allowlist');
eq(JSON.stringify({ a: 1, b: 2 }, (k, v) => typeof v == 'number' ? v * 2 : v), '{"a":2,"b":4}', 'replacer'); eq(JSON.stringify({ toJSON() { return 'custom'; } }), '"custom"', 'toJSON');
eq(JSON.stringify(undefined), undefined, 'stringify undefined'); eq(JSON.stringify(NaN), 'null', 'stringify NaN'); eq(JSON.stringify(new Date(0)), '"1970-01-01T00:00:00.000Z"', 'stringify date');
eq(JSON.parse('{"a":[1,2,{"b":null}],"c":"d\\u0041\\n","e":-1.5e2,"f":true}'), { a: [1, 2, { b: null }], c: 'dA\n', e: -150, f: true }, 'parse');
eq(JSON.parse('[]'), [], 'parse empty arr'); eq(JSON.parse(' 5 '), 5, 'parse number'); eq(JSON.parse('"x"'), 'x', 'parse string'); eq(JSON.parse('{"a":1}', (k, v) => typeof v == 'number' ? v + 1 : v), { a: 2 }, 'reviver');
throws(function () { JSON.parse('{a:1}'); }, 'Unexpected', 'parse error'); throws(function () { JSON.parse(''); }, 'end of JSON', 'parse empty'); throws(function () { JSON.parse('[1,]'); }, '', 'trailing comma');
var circ = {}; circ.self = circ; throws(function () { JSON.stringify(circ); }, 'circular', 'circular');
eq(JSON.parse(JSON.stringify({ n: { m: [1, { o: 2 }] } })).n.m[1].o, 2, 'roundtrip'); eq(structuredClone({ a: [1] }), { a: [1] }, 'structuredClone');
// ---- Map / Set
var m = new Map([['a', 1], ['b', 2]]); m.set('c', 3).set('a', 10);
eq([m.size, m.get('a'), m.has('b'), m.has('z'), m.get('z')], [3, 10, true, false, undefined], 'Map'); eq(Array.from(m.keys()), ['a', 'b', 'c'], 'Map keys'); eq(Array.from(m.values()), [10, 2, 3], 'Map values');
eq([...m], [['a', 10], ['b', 2], ['c', 3]], 'Map spread'); m.delete('b'); eq(m.size, 2, 'Map delete'); var mk = []; m.forEach((v, k) => mk.push(k + v)); eq(mk, ['a10', 'c3'], 'Map forEach');
var obj = {}, mo = new Map(); mo.set(obj, 'o').set(NaN, 'nan'); eq([mo.get(obj), mo.get(NaN), mo.get({})], ['o', 'nan', undefined], 'Map keys by identity');
for (var [k, v] of m) mk.push(k + '=' + v); eq(mk.slice(2), ['a=10', 'c=3'], 'Map for-of');
var st = new Set([1, 2, 2, 3]); st.add(4).add(1); eq([st.size, st.has(2), st.has(9)], [4, true, false], 'Set'); st.delete(1); eq([...st], [2, 3, 4], 'Set spread'); eq([...new Set('hello')].join(''), 'helo', 'Set dedupe');
st.clear(); eq(st.size, 0, 'Set clear'); eq(typeof new Map().set, 'function', 'Map methods');
// ---- Date
var d = new Date(Date.UTC(2024, 1, 29, 12, 30, 45, 123));
eq(d.getUTCFullYear(), 2024, 'getUTCFullYear'); eq(d.getUTCMonth(), 1, 'getUTCMonth'); eq(d.getUTCDate(), 29, 'getUTCDate'); eq(d.getUTCDay(), 4, 'getUTCDay'); eq(d.getUTCHours(), 12, 'hours'); eq(d.getUTCMilliseconds(), 123, 'ms');
eq(d.toISOString(), '2024-02-29T12:30:45.123Z', 'toISOString'); eq(new Date('2024-03-01T00:00:00Z').getTime() - d.getTime() > 0, true, 'parse ISO'); eq(new Date('2024-02-29').toISOString(), '2024-02-29T00:00:00.000Z', 'parse date only');
eq(new Date(2020, 0, 31).getMonth(), 0, 'local ctor'); var d2 = new Date(2020, 0, 31); d2.setMonth(1); eq(d2.getMonth(), 2, 'setMonth overflow'); d2.setDate(0); eq(d2.getDate(), 29, 'setDate 0 (leap)');
eq(new Date('garbage').getTime(), null, 'invalid date'); eq(isNaN(new Date('x')), true, 'invalid isNaN'); eq(typeof Date.now(), 'number', 'Date.now'); eq(new Date(86400000).getUTCDate(), 2, 'epoch day');
eq(new Date(2024, 0, 15, 10, 5).getHours(), 10, 'local hours'); eq(new Date(0) < new Date(1), true, 'date compare'); eq(new Date(5) - new Date(2), 3, 'date subtract'); eq(typeof String(new Date()), 'string', 'date string');
eq(new Date('Nov 6, 2026 10:00:00').getFullYear(), 2026, 'parse text'); eq(new Date(2024, 11, 25).toLocaleDateString(), '12/25/2024', 'toLocaleDateString');
// ---- errors
var e = new Error('msg'); eq([e.message, e.name, e instanceof Error, String(e)], ['msg', 'Error', true, 'Error: msg'], 'Error');
eq(new TypeError('t') instanceof Error, true, 'TypeError instanceof Error'); eq(new RangeError('r').name, 'RangeError', 'RangeError'); eq(typeof e.stack, 'string', 'stack');
throws(function () { new Array(-1); }, 'Invalid array length', 'RangeError'); throws(function () { null.x; }, 'null', 'null prop'); throws(function () { undefined.x = 1; }, 'undefined', 'set undefined prop');
throws(function () { (1).toFixed(200); }, 'between', 'toFixed range'); throws(function () { ({}).x.y; }, 'undefined', 'chained undefined');
eq((function () { try { throw 'str'; } catch (x) { return typeof x; } })(), 'string', 'throw string'); eq(Object.prototype.toString.call(null), '[object Null]', 'toString null');
// ---- UTF-16 semantics of strings (UTF-8 inside)
var est = 'õäöü'; eq([est.length, est[0], est.charAt(2), est.charCodeAt(1), est.slice(1, 3), est.substring(2), est.indexOf('ö'), est.lastIndexOf('ü')], [4, 'õ', 'ö', 228, 'äö', 'öü', 2, 3], 'BMP non-ASCII');
var emo = 'a\u{1F600}b'; eq([emo.length, emo.charCodeAt(1), emo.charCodeAt(2), emo.codePointAt(1), emo.indexOf('b'), emo.slice(0, 1) + emo.slice(1, 3) + emo.slice(3)], [4, 55357, 56832, 128512, 3, emo], 'surrogate pairs');
eq([...emo].length, 3, 'spread iterates code points'); eq(emo.split('').length, 3, 'split by code point'); eq('é'.padStart(3, '-'), '--é', 'padStart counts units'); eq('añb'.at(-1), 'b', 'at');
eq('abäcd'.match(/c/).index, 3, 'regex index in UTF-16 units'); eq('ä1'.replace(/\d/, function (m, off) { return off; }), 'ä1'.replace('1', '1'), 'replace offset'); var big = ''; for (var bi = 0; bi < 3000; bi++) big += 'ä'; var cnt = 0; for (var bj = 0; bj < big.length; bj++) if (big[bj] === 'ä') cnt++; eq(cnt, 3000, 'sequential indexing of a long non-ASCII string');
