// motion.js — Workspaces' transitions. Each one changes a node's native transform or opacity with lord.setStyle
// (scaleX, scaleY, translateX, translateY, opacity): lord-server paints the subtree once into a cached raster and only
// composites it, and React doesn't render per frame. One shared frame loop runs while a tween is alive and stops with
// the last one; with Reduce motion (Settings → Appearance or the theme's Control Center) every tween jumps to its end.
//
//   tween(node, { scaleX: [0.96, 1], opacity: [0, 1] }, { ms, delay, ease, clear, done })
//   EASE.out (cubic), EASE.back (a small overshoot: "drop and settle"), EASE.spring (an underdamped spring)
//
// Durations stay between 120 and 260 ms (docs/WORKSPACES.md, "Motion").
import { settings } from '@lordos/sdk';

export const EASE = {
  out: (p) => 1 - (1 - p) ** 3,
  inOut: (p) => (p < 0.5 ? 4 * p * p * p : 1 - (-2 * p + 2) ** 3 / 2),
  back: (p) => { const c1 = 1.4, c3 = c1 + 1; return 1 + c3 * (p - 1) ** 3 + c1 * (p - 1) ** 2; },
  // x(t) of a spring released from 0 towards 1, damping ζ ≈ 0.55, settled by t = 1
  spring: (p) => (p >= 1 ? 1 : 1 - Math.exp(-6 * p) * Math.cos(9.5 * p)),
};

export const reduceMotion = () => !!settings.get().reduceMotion;

const live = new Set();
let ticking = false;
const now = () => Date.now();
const nextFrame = (fn) => (typeof requestAnimationFrame === 'function' ? requestAnimationFrame(fn) : setTimeout(fn, 16));

function set(node, key, v) {
  try { lord.setStyle(node, key, v); } catch (e) { /* the node is gone */ }
}

function frame() {
  const t = now();
  for (const tw of [...live]) {
    const p = Math.min(1, Math.max(0, (t - tw.t0) / tw.ms));
    if (t < tw.t0) continue;
    const e = tw.ease(p);
    for (const [k, [a, b]] of tw.props) set(tw.node, k, +(a + (b - a) * e).toFixed(4));
    if (p >= 1) finish(tw);
  }
  if (live.size) nextFrame(frame); else ticking = false;
}

function finish(tw) {
  live.delete(tw);
  for (const [k, [, b]] of tw.props) set(tw.node, k, tw.clear ? undefined : b);
  tw.done?.();
}

// node: a native node id (ref.current.id). Returns a cancel function (it leaves the node at its end values).
export function tween(node, props, { ms = 200, delay = 0, ease = EASE.out, clear = true, done } = {}) {
  const entries = Object.entries(props);
  if (!node || typeof lord === 'undefined') { done?.(); return () => {}; }
  const tw = { node, props: entries, ms: Math.max(1, ms), t0: now() + delay, ease, clear, done };
  if (reduceMotion()) { finish(tw); return () => {}; }
  for (const x of live) if (x.node === node && x.props.some(([k]) => props[k])) live.delete(x);   // the newer one wins
  for (const [k, [a]] of entries) set(node, k, a);
  live.add(tw);
  if (!ticking) { ticking = true; nextFrame(frame); }
  return () => { if (live.has(tw)) finish(tw); };
}

export const tweening = () => live.size;
