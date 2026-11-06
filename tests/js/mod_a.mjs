export let counter = 0;
export function inc() { counter++; return counter; }
export const PI2 = 6.28;
export default class Greeter { hi(n) { return 'hi ' + n; } }
export { PI2 as TAU };
export function hoisted() { return 'h'; }
