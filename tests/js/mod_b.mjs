import Greeter, { counter, inc as increment, TAU } from './mod_a.mjs';
import * as A from './mod_a.mjs';
export { hoisted } from './mod_a.mjs';
export * from './mod_a.mjs';
export * as ns from './mod_a.mjs';
export default function () { return 'anon default'; }
export const value = new Greeter().hi('b') + counter;
export { counter as c2, A };
export { increment };
export const meta = typeof import.meta.url;
