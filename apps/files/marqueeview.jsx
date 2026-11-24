// marqueeview.jsx — Files' rubber-band selection in the icon and list views (GitHub #233); the rules are marquee.js.
//
// useMarquee(ctx) gives a view:
//   scrollRef        for its ScrollView (the content's origin, the viewport and scrolling by itself)
//   itemRef(path)    for each item's box, and for the empty boxes that stand in for items out of sight in a big
//                    folder: where the layout put it is what the rectangle is tested against (no hit test per node)
//   onDown(ev)       for the scrolled content's own press: an item's press never gets here (it stops it)
//   band             a small store with the rectangle, drawn by <MarqueeBand band={band} /> inside the content
//
// A press with no modifier lets the window's own handler clear the selection as before (a plain click on empty space
// still does only that); with Shift or Ctrl (⌘) it is kept, and a click without a drag clears it as a plain click
// would. The selection changes at most once per frame, and only when the set of items under the rectangle changes, so
// a drag across a big folder repaints only the cards and rows that change (and the rectangle's own small node).
import { View, createStore, useStore, addListener, useTheme } from '@lordos/sdk';
import { useEffect, useMemo, useRef } from 'react';
import { DRAG_MIN, rectOf, hitPaths, modeOf, combine, sameSet, autoscrollStep, frameGate } from './marquee.js';

export function useMarquee(ctx) {
  const scrollRef = useRef(null);
  const nodes = useRef(new Map());          // path → its node, while mounted
  const refs = useRef(new Map());           // path → its ref callback (one per path, so React doesn't call it on every render)
  const band = useMemo(() => createStore(null), []);
  const op = useRef(null);
  const ctxRef = useRef(ctx);
  ctxRef.current = ctx;

  const itemRef = (path) => {
    let fn = refs.current.get(path);
    if (!fn) {
      fn = (n) => { if (n) nodes.current.set(path, n); else nodes.current.delete(path); };
      refs.current.set(path, fn);
    }
    return fn;
  };

  const origin = () => {
    const inner = scrollRef.current?.inner?.current;
    if (!inner) return null;
    try { return lord.getLayout(inner.id); } catch (e) { return null; }
  };
  // the boxes of the items now laid out, in the content's space; kept for the whole drag, so an item that scrolled out
  // of sight (and became an empty box again) still counts
  const collect = (o, cache) => {
    for (const [path, n] of nodes.current) {
      let l = null;
      try { l = lord.getLayout(n.id); } catch (e) { l = null; }
      if (l && l[2] > 0 && l[3] > 0) cache.set(path, { path, x: l[0] - o[0], y: l[1] - o[1], w: l[2], h: l[3] });
    }
  };

  const frame = () => {
    const p = op.current;
    if (!p) return;
    const o = origin();
    if (!o) return;
    collect(o, p.cache);
    const r = rectOf(p.start, { x: p.px - o[0], y: p.py - o[1] });
    band.set(r);
    const next = combine(p.base, hitPaths(r, [...p.cache.values()]), p.mode);
    if (!sameSet(next, p.last)) { p.last = next; ctxRef.current.onMarquee?.(next); }
    // near the top or bottom edge: the view scrolls and the rectangle follows, frame after frame while the pointer stays
    const sc = scrollRef.current;
    const outer = sc?.outer?.current;
    if (!outer) return;
    let v = null;
    try { v = lord.getLayout(outer.id); } catch (e) { v = null; }
    const step = v ? autoscrollStep(p.py, v[1], v[3]) : 0;
    if (step) {
      const at = sc.offset();
      sc.scrollTo(at + step);
      p.gate.push(true);
    }
  };

  useEffect(() => {
    const mv = addListener('pointermove', (e) => {
      const p = op.current;
      if (!p) return;
      p.px = e.x; p.py = e.y;
      if (!p.moved) {
        if (Math.abs(e.x - p.sx) < DRAG_MIN && Math.abs(e.y - p.sy) < DRAG_MIN) return;
        p.moved = true;
      }
      p.gate.push(true);
    });
    const up = addListener('pointerup', () => {
      const p = op.current;
      if (!p) return;
      if (p.moved) p.gate.flush();
      p.gate.cancel();
      op.current = null;
      band.set(null);
      // a click with Shift or Ctrl on empty space: what a plain click does (the window's handler didn't get it)
      if (!p.moved && p.mode !== 'replace') ctxRef.current.onMarquee?.(new Set());
    });
    return () => { mv(); up(); const p = op.current; if (p) p.gate.cancel(); op.current = null; };
  }, []);

  const onDown = (ev) => {
    if (ev.button) return;
    const c = ctxRef.current;
    if (!c.onMarquee) return;
    const o = origin();
    if (!o) return;
    const mode = modeOf(ev);
    if (mode !== 'replace') {
      ev.stopPropagation();
      if (c.renaming) c.onRenameCommit?.();
    }
    const base = new Set(c.selected);
    op.current = {
      mode, base, last: mode === 'replace' ? new Set() : base, moved: false, cache: new Map(),
      start: { x: ev.x - o[0], y: ev.y - o[1] }, sx: ev.x, sy: ev.y, px: ev.x, py: ev.y, gate: frameGate(frame),
    };
  };

  return { scrollRef, itemRef, onDown, band };
}

// The rectangle: a translucent box in the accent colour, inside the scrolled content (it scrolls with it), physical
// left and top in every language. Its own component and store: moving it draws only this node.
export function MarqueeBand({ band }) {
  const r = useStore(band);
  const t = useTheme();
  if (!r || (r.w < 1 && r.h < 1)) return null;
  return (
    <View style={{ position: 'absolute', left: r.x, top: r.y, width: Math.max(1, r.w), height: Math.max(1, r.h), pointerEvents: 'none',
      backgroundColor: t.mode === 'dark' ? 'rgba(10,132,255,0.18)' : 'rgba(0,122,255,0.14)', border: `1px solid ${t.accent || '#0a84ff'}`, borderRadius: 2 }} />
  );
}
