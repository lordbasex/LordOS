// displaysui.jsx — "Identify" for Settings → Displays (docs/DISPLAYS.md, "Numbers, identify and snapping"):
//
//   identifyDisplays(ids?)   shows each display's number (1, 2, 3…; sdk/displaylayout.js displayNumbers),
//                            big, centered on that real display for 2 s, the screens slightly dimmed. `ids` are
//                            lord.display.list() output ids (one or an array); none = every active display.
//                            Returns false when there is nothing to show (no display API, mirror mode).
//   <DisplayIdentify />      the badges; startShell mounts it in the `notification` layer (sdk/index.js,
//                            docs/LAYERS.md), so every theme has it. It never takes clicks.
//
// The desktop is one logical space across displays: each badge is placed at its display's global
// rectangle (lord.screen.displays, matched by connector; the list's own rectangle otherwise).
import { createElement as h } from 'react';
import { View, Text, screen as desk, displays as screenDisplays } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { displays } from './hardware.jsx';
import { displayNumbers } from './displaylayout.js';

export const IDENTIFY_MS = 2000;

export const identifying = createStore(null);    // { seq, items: [{ id, n, x, y, w, h }] }
let seq = 0, timer = 0;

export function identifyDisplays(ids) {
  const d = displays.get();
  if (!d || d.mirror) return false;
  const outs = (d.outputs || []).filter((o) => o.active && o.w);
  const nums = displayNumbers(outs);
  let screen = [];
  try { screen = screenDisplays() || []; } catch (e) { screen = []; }
  const want = ids == null ? null : new Set([].concat(ids));
  const items = outs.filter((o) => !want || want.has(o.id)).map((o) => {
    const s = screen.find((x) => x.connector && x.connector === o.connector);
    const r = s && s.w ? s : o;
    return { id: o.id, n: nums[o.id], x: r.x, y: r.y, w: r.w, h: r.h };
  }).filter((it) => it.n && it.w > 0 && it.h > 0);
  if (!items.length) return false;
  clearTimeout(timer);
  identifying.set({ seq: ++seq, items });
  timer = setTimeout(() => identifying.set(null), IDENTIFY_MS);
  return true;
}

// One big rounded badge with the number, centered on its display.
function Badge({ it }) {
  const size = Math.round(Math.max(120, Math.min(240, Math.min(it.w, it.h) * 0.3)));
  return h(View, { style: { position: 'absolute', left: it.x, top: it.y, width: it.w, height: it.h, alignItems: 'center', justifyContent: 'center', pointerEvents: 'none' } },
    h(View, { style: { width: size, height: size, borderRadius: Math.round(size * 0.24), alignItems: 'center', justifyContent: 'center',
      backgroundColor: 'rgba(28,28,32,0.78)', border: '1px solid rgba(255,255,255,0.18)', boxShadow: '0 18px 50px rgba(0,0,0,0.45)', pointerEvents: 'none' } },
    h(Text, { style: { fontSize: Math.round(size * 0.6), fontWeight: 700, color: '#ffffff', textAlign: 'center', pointerEvents: 'none' } }, String(it.n))));
}

export function DisplayIdentify() {
  // shown at once and gone after IDENTIFY_MS (identifyDisplays clears the store): no per-frame fade,
  // which also respects Reduce motion
  const c = useStore(identifying);
  if (!c) return null;
  // the whole desktop's size, slightly dimmed: the dim makes the numbers stand out, and a root with a
  // background of its own is what gets the area repainted when the badges appear (a transparent root's
  // new children were left unpainted until something else changed)
  return h(View, { key: c.seq, style: { position: 'absolute', left: 0, top: 0, width: desk.width, height: desk.height, pointerEvents: 'none',
    backgroundColor: 'rgba(0,0,0,0.18)' } },
    ...c.items.map((it) => h(Badge, { key: it.id, it })));
}
