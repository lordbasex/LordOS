// runtime_props_scene.mjs — the scene of tests/runtime_props_test.mjs (not run by itself): fuzzed React
// trees rendered through runtime/index.js with a recording `lord` (the mock comes in as the bundle's
// banner, so it exists before the runtime loads). Prints every native call, then each node's final
// style. Usage, after the test bundles it: node <bundle> SEED STEPS
import React from 'react';
import * as rt from '@lordos/runtime';

const { log, fmt, state } = globalThis.__lordTest;
const seed0 = +(process.argv[2] || 1), STEPS = +(process.argv[3] || 300);
const h = React.createElement;

// xorshift32, so both bundles see the same trees
let rnd = seed0 >>> 0 || 1;
const r = () => { rnd ^= rnd << 13; rnd >>>= 0; rnd ^= rnd >>> 17; rnd ^= rnd << 5; rnd >>>= 0; return rnd / 4294967296; };
const pick = (a) => a[Math.floor(r() * a.length)];
// values that normalize expands, overrides (margin / marginTop), NaN ('#zz', a gradient without an
// angle, NaN itself), -0, the values normalize drops (undefined, null, false) and objects (transform)
const VALUES = {
  width: [10, 20, '50%', 'auto', '12px', NaN, -0, 0, undefined],
  height: [10, 30, '100%', null, false],
  backgroundColor: ['#fff', '#zz', 'red', 'rgba(0,0,0,.5)', 0xff00ff00, '#12345678', undefined],
  color: ['#000', 'white', 'rgb(1 2 3)', '#abc'],
  margin: [0, 4, '4px 8px', '1 2 3', '1 2 3 4'],
  marginTop: [0, 8, -0, '3px'],
  padding: [2, '2px 4px'],
  flex: [1, 0, -1, '2'],
  borderRadius: [4, 8, '6px'],
  boxShadow: ['0 2px 8px rgba(0,0,0,.3)', 'none', '0 24px 56px #000'],
  background: ['linear-gradient(to right, red, blue)', 'linear-gradient(deg, red, blue)', 'linear-gradient(45deg, #fff, #000)', '#333'],
  transform: [[{ rotate: '45deg' }], [{ scale: 2 }, { translateX: 3 }], 'rotate(0.5turn)', 'none'],
  transformOrigin: ['center', 'top left', '10px 20%'],
  fontWeight: [400, 'bold', 'semibold', '700'],
  fontFamily: ['mono', 'Inter', 'brand'],
  opacity: [1, 0.5, '0.25', NaN],
  display: ['flex', 'none'],
  textAlign: ['start', 'end'],
  border: ['1px solid #ccc', '2 solid red'],
  marginInline: ['4 8', 3],
  lineClamp: [1, 2],
  zIndex: [1, 2, 3],
};
const KEYS = Object.keys(VALUES);
function randStyle() {
  const o = {};
  const n = Math.floor(r() * 6);
  for (let i = 0; i < n; i++) { const k = pick(KEYS); o[k] = pick(VALUES[k]); }
  return o;
}
function shuffled(o) {
  const ks = Object.keys(o);
  for (let i = ks.length - 1; i > 0; i--) { const j = Math.floor(r() * (i + 1)); [ks[i], ks[j]] = [ks[j], ks[i]]; }
  const out = {};
  for (const k of ks) out[k] = o[k];
  return out;
}
// The style a node gets on the next render, from its previous one: a new random style, the same object,
// an equal copy, the same object changed in place (a key set or deleted), the same values in another
// order, a copy with one key changed, or one key set to undefined.
function evolve(prev) {
  const x = r();
  if (prev === undefined || x < 0.08) return r() < 0.3 ? [randStyle(), r() < 0.5 && randStyle(), null] : randStyle();
  if (x < 0.35) return prev;                                      // same object
  if (x < 0.65) return Array.isArray(prev) ? prev.map((s) => (s ? { ...s } : s)) : { ...prev };  // equal copy
  if (x < 0.72) { if (!Array.isArray(prev)) { const k = pick(KEYS); prev[k] = pick(VALUES[k]); } return prev; }  // changed in place
  if (x < 0.76) { if (!Array.isArray(prev)) { const k = pick(KEYS); delete prev[k]; } return prev; }
  if (x < 0.84) return Array.isArray(prev) ? prev : shuffled(prev);  // same values, other order
  if (x < 0.92) { const c = Array.isArray(prev) ? [...prev] : { ...prev }; if (!Array.isArray(c)) { const k = pick(KEYS); c[k] = pick(VALUES[k]); } return c; }
  return Array.isArray(prev) ? prev.map((s) => (s ? { ...s } : s)) : { ...prev, [pick(KEYS)]: undefined };
}

const N = 24;
let styles = [], texts = [], hidden = false, srcs = [], prot = [], order = [];
for (let i = 0; i < N; i++) { styles[i] = evolve(undefined); texts[i] = 't' + i; srcs[i] = undefined; prot[i] = false; order[i] = i; }
let bump = null;
function Tree() {
  const [, set] = React.useState(0);
  bump = () => set((x) => x + 1);
  const kids = order.map((i) => h(i % 5 === 4 ? rt.Image : i % 3 === 0 ? rt.Text : rt.View,
    { key: i, style: styles[i], src: i % 5 === 4 ? srcs[i] : undefined, protected: prot[i] }, i % 3 === 0 ? texts[i] : null));
  const inner = h(rt.View, { style: styles[0] }, kids.slice(0, N / 2));
  const act = React.Activity ? h(React.Activity, { mode: hidden ? 'hidden' : 'visible' }, h(rt.View, { style: styles[1] }, kids.slice(N / 2))) : kids.slice(N / 2);
  return h(rt.View, { style: { flex: 1 } }, inner, act);
}
// React schedules with setImmediate in node: let every update commit
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
rt.render(h(Tree));
await flush();
for (let step = 0; step < STEPS; step++) {
  for (let i = 0; i < N; i++) {
    styles[i] = evolve(styles[i]);
    if (r() < 0.1) texts[i] = 't' + Math.floor(r() * 4);
    if (r() < 0.05) srcs[i] = pick([undefined, 'a.png', 'b.png']);
    if (r() < 0.03) prot[i] = !prot[i];
  }
  if (r() < 0.05) hidden = !hidden;
  if (r() < 0.05) order = order.slice().sort(() => r() - 0.5);
  if (r() < 0.03) order = order.filter((x) => x !== pick(order));
  bump();
  await flush();
  log.push(`-- step ${step}`);
}
for (const [id, s] of state) log.push(`final ${id} ${Object.keys(s).sort().map((k) => k + '=' + fmt(s[k])).join(' ')}`);
process.stdout.write(log.join('\n') + '\n');
