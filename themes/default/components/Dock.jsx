// Dock with magnification by mouse position, tooltip and open-app dot. It can go at the bottom,
// on the left or on the right (Settings → Desktop & Dock), hide (always, or only while a window
// covers it) and show recent apps. Left and right are physical sides of the screen, also in Arabic or Hebrew (as
// on popular desktop systems): the Dock is laid out with direction: 'ltr' and only the bottom row reverses its order in RTL.
//
// Customizing (docs/DOCK.md): an icon is dragged to another place (its neighbours slide aside in
// 150 ms), dragged out and held to remove it (a «poof»), or dropped from the app launcher at a
// precise position. The logic is the SDK's (sdk/dock.js: beginDockDrag, registerDockTarget, dock.*);
// this file only draws it and says where things are. The Trash is always the last item.
import {
  View, Text, screen, appName, useLocale, addListener, t as tr, useStore, settings, dockDrag, dockPoof, beginDockDrag,
  registerDockTarget, dockDragJustEnded, refreshTrash, Layer, useDockCovered,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';

const GAP = 8, PAD = 9, MARGIN = 6;
const SEP = GAP + 1;          // separator before the recent apps and before the Trash
const SLIDE_MS = 180;
const REORDER_MS = 150;       // neighbours sliding aside while dragging
const POOF_MS = 320;
const NEAR = 60;              // dragged farther than this from the Dock: the remove cue (after a hold)
const TRASH = '__trash';

// What the Dock takes up from its screen edge (for the work area).
export const dockThickness = (size) => size + PAD * 2 + MARGIN + 4;

export function LaunchpadIcon({ size: s }) {
  const colors = ['#ff5f57', '#febc2e', '#28c840', '#0a84ff', '#bf5af2', '#ff9f0a', '#30d158', '#ff375f', '#64d2ff'];
  const d = s * 0.14, g = s * 0.08, o = (s - 3 * d - 2 * g) / 2;
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(180deg, #5b5b66, #2b2b33)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)` }}>
      {colors.map((c, i) => (
        <View key={i} style={{ position: 'absolute', left: o + (i % 3) * (d + g), top: o + Math.floor(i / 3) * (d + g),
          width: d, height: d, borderRadius: d * 0.3, backgroundColor: c }} />
      ))}
    </View>
  );
}

// The Trash: a wire basket, empty or with crumpled papers. Light and dark versions.
export function TrashIcon({ size: s, full, dark }) {
  const body = dark ? 'linear-gradient(180deg, rgba(235,238,245,0.30), rgba(200,205,215,0.16))' : 'linear-gradient(180deg, rgba(255,255,255,0.85), rgba(215,220,230,0.65))';
  const line = dark ? 'rgba(255,255,255,0.55)' : 'rgba(60,66,80,0.45)';
  const w = s * 0.62, hh = s * 0.64, x = (s - w) / 2, y = s * 0.3;
  return (
    <View style={{ width: s, height: s }}>
      {full && (
        <>
          <View style={{ position: 'absolute', left: x + w * 0.02, top: y - s * 0.24, width: w * 0.52, height: s * 0.32, borderRadius: s * 0.13,
            backgroundColor: '#ffffff', border: '1px solid rgba(60,66,80,0.35)', transform: 'rotate(-14deg)' }} />
          <View style={{ position: 'absolute', left: x + w * 0.4, top: y - s * 0.28, width: w * 0.56, height: s * 0.34, borderRadius: s * 0.08,
            backgroundColor: '#e9f1ff', border: '1px solid rgba(60,66,80,0.35)', transform: 'rotate(12deg)' }}>
            {[0.3, 0.5, 0.7].map((k) => <View key={k} style={{ position: 'absolute', left: w * 0.08, right: w * 0.1, top: s * 0.34 * k, height: 1, backgroundColor: 'rgba(60,90,160,0.45)' }} />)}
          </View>
        </>
      )}
      <View style={{ position: 'absolute', left: x, top: y, width: w, height: hh, borderBottomLeftRadius: s * 0.1, borderBottomRightRadius: s * 0.1,
        borderTopLeftRadius: s * 0.04, borderTopRightRadius: s * 0.04, background: body, border: `1px solid ${line}`, overflow: 'hidden',
        boxShadow: `0 ${s * 0.03}px ${s * 0.1}px rgba(0,0,0,0.25)` }}>
        {[0.2, 0.4, 0.6, 0.8].map((k) => (
          <View key={k} style={{ position: 'absolute', top: 0, bottom: 0, left: w * k - 0.5, width: 1, backgroundColor: line }} />
        ))}
        <View style={{ position: 'absolute', left: 0, right: 0, top: hh * 0.5, height: 1, backgroundColor: line }} />
      </View>
      <View style={{ position: 'absolute', left: x - s * 0.03, top: y - 1, width: w + s * 0.06, height: s * 0.05, borderRadius: s * 0.03,
        backgroundColor: dark ? 'rgba(235,238,245,0.6)' : 'rgba(120,126,140,0.7)' }} />
    </View>
  );
}

// Auto-hide: shows when the pointer touches the edge (after a short delay) and hides
// when it leaves the Dock. State only changes when crossing those boundaries: moving the mouse around the rest of the
// screen doesn't redraw anything. The animation belongs to this component, not the desktop.
// force: shown right away (an app being dragged from the app launcher).
// mode (sdk/dockmodel.js, issue #46): 'never' hides nothing; 'always' as above; 'overlap' hides only
// while `covered` (a window is where the Dock is) and comes back as soon as nothing covers it; the
// pointer at the edge still brings it over the windows, and it goes again when the pointer leaves.
const COVER_MS = 300;         // a window just covered the Dock: it slides away after this
function useAutohide(mode, position, rectRef, keepOpen, areaRef, force, covered) {
  const enabled = mode !== 'never';
  const may = enabled && (mode === 'always' || !!covered);          // may it hide now
  const [p, setP] = useState(may ? 0 : 1);     // 0 hidden … 1 in view
  const want = useRef(!may);
  const keep = useRef(keepOpen);
  keep.current = keepOpen;
  const mayHide = useRef(may);
  mayHide.current = may;
  const api = useRef(null);
  useEffect(() => {
    if (!enabled) { want.current = true; setP(1); return undefined; }
    want.current = !mayHide.current;
    setP(want.current ? 1 : 0);
    let timer = 0, raf = 0, from = 0, t0 = 0, px = -1, py = -1;
    const animate = (to) => {
      cancelAnimationFrame(raf);
      t0 = performance.now();
      const step = (now) => {
        const k = Math.min(1, (now - t0) / SLIDE_MS), e = 1 - (1 - k) * (1 - k);
        setP(from + (to - from) * e);
        if (k < 1) raf = requestAnimationFrame(step);
        else from = to;
      };
      raf = requestAnimationFrame(step);
    };
    const set = (on) => {
      if (want.current === on) return;
      want.current = on;
      from = on ? 0 : 1;
      animate(on ? 1 : 0);
    };
    const stop = () => { if (timer) { clearTimeout(timer); timer = 0; } };
    const isOver = () => {
      const r = rectRef.current;
      return !!r && px >= r.x0 - 8 && px < r.x1 + 8 && py >= r.y0 - 8 && py < r.y1 + 8;
    };
    // hides after ms unless, by then, the pointer is on it, a menu of it is open or nothing covers it
    const hideSoon = (ms) => {
      if (!timer) timer = setTimeout(() => { timer = 0; if (!keep.current && mayHide.current && !isOver()) set(false); }, ms);
    };
    api.current = {
      show: () => { stop(); set(true); },
      // 'overlap': a window started or stopped covering the Dock ('always' ↔ 'overlap' too)
      cover: (on) => {
        stop();
        if (!on) set(true);
        else if (want.current && !keep.current && !isOver()) hideSoon(COVER_MS);
      },
    };
    const off = addListener('pointermove', (e) => {
      px = e.x; py = e.y;
      // the edge of the display the Dock is on (the main one: docs/DISPLAYS.md)
      const A = areaRef.current, inX = e.x >= A.x && e.x < A.x + A.w, inY = e.y >= A.y && e.y < A.y + A.h;
      const edge = position === 'bottom' ? inX && e.y >= A.y + A.h - 2 : position === 'left' ? inY && e.x <= A.x + 1 : inY && e.x >= A.x + A.w - 2;
      if (!want.current) {
        if (edge && !timer) timer = setTimeout(() => { timer = 0; set(true); }, 120);
        else if (!edge) stop();
      } else if (!isOver() && !keep.current && mayHide.current) hideSoon(500);
      else stop();
    });
    return () => { off(); stop(); cancelAnimationFrame(raf); api.current = null; };
  }, [enabled, position]);
  useEffect(() => { if (force && enabled) api.current?.show(); }, [force, enabled]);
  useEffect(() => { if (enabled) api.current?.cover(may); }, [may, enabled]);
  return p;
}

// Positions that glide to their new value in `ms` (ease-out) while `active`; otherwise they jump.
// Values are numbers by key; a key seen for the first time starts where `origin(key)` says, or at
// its target.
function useGlide(targets, active, ms, origin) {
  const cur = useRef({});
  const anim = useRef(null);                 // { from, to, t0, sig }
  const running = useRef(false);
  const [, tick] = useState(0);
  useEffect(() => {
    if (!running.current) return undefined;
    const raf = requestAnimationFrame(() => tick((n) => n + 1));
    return () => cancelAnimationFrame(raf);
  });
  const sig = Object.keys(targets).map((k) => `${k}:${Math.round(targets[k])}`).join('|');
  const now = performance.now();
  const valueAt = (a, k) => {
    const p = Math.min(1, (now - a.t0) / ms), e = 1 - (1 - p) ** 3;
    const f = a.from[k] ?? a.to[k];
    return f + (a.to[k] - f) * e;
  };
  if (!active || ms <= 0) { anim.current = null; running.current = false; cur.current = { ...targets }; return targets; }
  if (!anim.current || anim.current.sig !== sig) {
    const from = {};
    for (const k of Object.keys(targets)) {
      const was = anim.current && k in anim.current.to ? valueAt(anim.current, k) : cur.current[k];
      from[k] = was ?? origin?.(k) ?? targets[k];
    }
    anim.current = { from, to: { ...targets }, t0: now, sig };
  }
  const out = {};
  for (const k of Object.keys(targets)) out[k] = valueAt(anim.current, k);
  cur.current = out;
  running.current = now - anim.current.t0 < ms;
  return out;
}

// The «poof» after an icon is removed: the icon shrinks and fades and five puffs spread out.
function Poof({ poof, size, reduce }) {
  const [, tick] = useState(0);
  const p = poof ? Math.min(1, (Date.now() - poof.t0) / POOF_MS) : 1;
  useEffect(() => {
    if (!poof || p >= 1 || reduce) return undefined;
    const raf = requestAnimationFrame(() => tick((n) => n + 1));
    return () => cancelAnimationFrame(raf);
  });
  if (!poof || p >= 1 || reduce) return null;
  const s = size;
  return (
    <View style={{ position: 'absolute', left: poof.x - s, top: poof.y - s, width: s * 2, height: s * 2, pointerEvents: 'none' }}>
      {(() => {
        // the icon shrinks (drawn smaller, not scaled: a scaled raster would show its box) and fades
        const k = Math.round(s * (1 - 0.6 * p));
        return (
          <View style={{ position: 'absolute', left: s - k / 2, top: s - k / 2, width: k, height: k, opacity: 1 - p }}>
            {poof.Icon ? <poof.Icon size={k} /> : null}
          </View>
        );
      })()}
      {[0, 72, 144, 216, 288].map((a) => {
        const r = s * (0.25 + 0.75 * p), d = s * (0.22 + 0.2 * p);
        const cx = s + Math.cos((a * Math.PI) / 180) * r, cy = s + Math.sin((a * Math.PI) / 180) * r;
        return <View key={a} style={{ position: 'absolute', left: cx - d / 2, top: cy - d / 2, width: d, height: d, borderRadius: d / 2,
          backgroundColor: 'rgba(235,238,245,0.9)', opacity: 0.85 * (1 - p) }} />;
      })}
    </View>
  );
}

export function Dock({ t, items: list, running, onOpen, onContext, onTrash, onTrashContext, trashFull = false, position = 'bottom', size = 54,
  magnify = true, maxSize = 86, hideMode = 'never', top = 0, keepOpen = false, area = { x: 0, y: 0, w: screen.width, h: screen.height } }) {
  // area: the display it is on (global coordinates); everything below is placed inside it
  const vert = position !== 'bottom';
  const [m, setM] = useState(null);             // mouse position along the Dock's axis
  const { dir } = useLocale();
  const drag = useStore(dockDrag);
  const poof = useStore(dockPoof);
  const reduce = !!useStore(settings).reduceMotion;
  // The magnifier computes positions with the mouse coordinate: in Arabic/Hebrew the bottom row reverses
  // the list (the visual result users expect); on the side the order is top to bottom in all of them.
  const flip = !vert && dir === 'rtl';
  const trashItem = { id: TRASH, trash: true };
  const dragging = !!drag;
  const own = drag && drag.source === 'dock' ? drag.id : null;          // an icon of this Dock is being dragged
  // tokens in reading order: items, the gap where a drop would land, separators
  // Tokens on screen (reading order, mirrored for a bottom Dock in RTL): the icons without the one
  // being dragged, the gap where a drop would land, the separators and the Trash, always last.
  const tokensFor = (skip, gapAt) => {
    const out = [];
    let pi = 0, sepDone = false, gapDone = gapAt < 0;
    for (const it of list) {
      if (it.id === skip) continue;
      const pinned = it.pinned && !it.recent;
      if (!gapDone && (!pinned || pi === gapAt)) { out.push({ gap: true }); gapDone = true; }
      if (pinned) pi++;
      if (it.recent && !sepDone) { out.push({ sep: true }); sepDone = true; }
      out.push({ item: it });
    }
    if (!gapDone) out.push({ gap: true });
    out.push({ sep: true }, { item: trashItem });
    return flip ? out.reverse() : out;
  };
  const pinnedCount = list.filter((it) => it.pinned && !it.recent && it.id !== own).length;
  const gapAt = drag && drag.over && drag.over.index != null ? Math.max(0, Math.min(pinnedCount, drag.over.index)) : -1;
  const vis = tokensFor(own, gapAt);
  const items = vis.filter((x) => x.item).map((x) => x.item);
  const n = items.length;
  const seps = vis.filter((x) => x.sep).length;
  const gaps = vis.filter((x) => x.gap).length;
  const origin = vert ? area.y + top : area.x, avail = vert ? area.h - top : area.w;
  const AX = area.x, AY = area.y, AR = area.x + area.w, AB = area.y + area.h;
  // with many apps the icons shrink so the Dock fits on screen
  // (it accounts for magnifier growth: about 2.6 extra icons in total)
  const grow = magnify ? 2.6 * Math.max(0, maxSize / size - 1) : 0;
  const fit = Math.floor((avail - 2 * PAD - 16 - (n + gaps - 1) * GAP - seps * SEP) / (Math.max(1, n + gaps) + grow));
  const BASE = Math.max(20, Math.min(size, fit));
  const MAX = magnify && !dragging ? Math.max(BASE, Math.round(maxSize * BASE / size)) : BASE, RANGE = BASE * 3;
  // base layout (every icon at BASE): for the magnifier, the tooltip and the drop positions
  const layout = (sizeOf, toks = vis) => {
    let x = PAD;
    const c = [];
    for (const tk of toks) {
      if (tk.sep) x += SEP;
      else if (tk.gap) x += BASE + GAP;
      else { const s = sizeOf(c.length); c.push(x + s / 2); x += s + GAP; }
    }
    return { c, len: x - GAP + PAD };
  };
  const base = layout(() => BASE);
  const baseStart = origin + (avail - base.len) / 2;
  const centers = base.c.map((c) => baseStart + c);
  const sizes = centers.map((c) => {
    if (m == null || MAX === BASE) return BASE;
    const d = Math.abs(m - c) / RANGE;
    return d < 1 ? Math.round(BASE + (MAX - BASE) * Math.cos((d * Math.PI) / 2)) : BASE;
  });
  const hovered = m == null || dragging ? -1 : centers.findIndex((c) => Math.abs(m - c) <= (BASE + GAP) / 2);
  const real = layout((i) => sizes[i]);
  const len = real.len;
  const startT = Math.max(origin, origin + (avail - len) / 2);
  const bar = BASE + PAD * 2;

  // glide: every icon's place on screen, the bar's start and length
  const settle = useRef(0);
  const wasDragging = useRef(false);
  const lastGhost = useRef(null);
  if (wasDragging.current && !dragging) settle.current = Date.now() + REORDER_MS + 40;
  wasDragging.current = dragging;
  if (drag) lastGhost.current = { id: drag.id, at: vert ? drag.y : drag.x };
  const targets = { __start: startT, __len: len };
  items.forEach((it, i) => { targets[it.id] = startT + real.c[i]; });
  const glide = useGlide(targets, dragging || Date.now() < settle.current, reduce ? 0 : REORDER_MS,
    (k) => (lastGhost.current && lastGhost.current.id === k ? lastGhost.current.at : undefined));
  const start = glide.__start, glen = glide.__len;
  const iconC = items.map((it) => glide[it.id] - start);

  // where the bar is on screen (for auto-hide, drops) and how far it moves when hiding
  const rect = useRef(null);
  rect.current = position === 'bottom' ? { x0: startT, x1: startT + len, y0: AB - MARGIN - bar, y1: AB }
    : position === 'left' ? { x0: AX, x1: AX + MARGIN + bar, y0: startT, y1: startT + len }
      : { x0: AR - MARGIN - bar, x1: AR, y0: startT, y1: startT + len };
  const areaRef = useRef(area);
  areaRef.current = area;
  const external = !!drag && drag.source !== 'dock';
  // 'overlap' (issue #46): is a window where the Dock rests (its size without magnification)?
  const restStart = Math.max(origin, origin + (avail - base.len) / 2);
  const restRect = position === 'bottom' ? { x0: restStart, x1: restStart + base.len, y0: AB - MARGIN - bar, y1: AB }
    : position === 'left' ? { x0: AX, x1: AX + MARGIN + bar, y0: restStart, y1: restStart + base.len }
      : { x0: AR - MARGIN - bar, x1: AR, y0: restStart, y1: restStart + base.len };
  const covered = useDockCovered(restRect, hideMode === 'overlap');
  const shown = useAutohide(hideMode, position, rect, keepOpen || hovered >= 0 || dragging, areaRef, external, covered);
  const hide = Math.round((1 - shown) * (bar + MARGIN + 2));

  // Drop target (sdk/dock.js): the index among the shown pinned items, or the Trash. Computed from
  // the layout without the dragged icon and without the gap, so it doesn't move under the pointer.
  const geo = useRef(null);
  {
    const toks = tokensFor(own, -1);
    const r = layout(() => BASE, toks);
    const rs = origin + (avail - r.len) / 2;
    const restItems = toks.filter((x) => x.item).map((x) => x.item);
    const rest = restItems.map((it, i) => (it.pinned && !it.recent ? rs + r.c[i] : null)).filter((c) => c != null);
    const ti = items.findIndex((it) => it.trash);
    geo.current = { rest, flip, vert, rect: rect.current, trashC: ti >= 0 ? startT + real.c[ti] : null, BASE };
  }
  useEffect(() => registerDockTarget({
    hit(x, y) {
      const g = geo.current, r = g.rect, slack = 24;
      if (!(x >= r.x0 - slack && x < r.x1 + slack && y >= r.y0 - slack && y < r.y1 + slack)) return null;
      const a = g.vert ? y : x;
      if (g.trashC != null && Math.abs(a - g.trashC) <= g.BASE / 2 + 2) return { trash: true };
      const before = g.rest.filter((c) => c < a).length;
      return { index: g.flip ? g.rest.length - before : before };
    },
    near(x, y) {
      const r = geo.current.rect;
      const dx = x < r.x0 ? r.x0 - x : x > r.x1 ? x - r.x1 : 0, dy = y < r.y0 ? r.y0 - y : y > r.y1 ? y - r.y1 : 0;
      return Math.hypot(dx, dy) <= NEAR;
    },
  }), []);

  // a Files drag (or any held button) over the Trash lights it up
  const [trashHover, setTrashHover] = useState(false);
  const buttonDown = useRef(false);
  useEffect(() => {
    const a = addListener('pointerdown', () => { buttonDown.current = true; });
    const b = addListener('pointerup', () => { buttonDown.current = false; });
    return () => { a(); b(); };
  }, []);

  if (hideMode !== 'never' && shown <= 0 && !dragging) return <Layer name="drag"><Poof poof={poof} size={BASE} reduce={reduce} /></Layer>;

  // The hit box covers only the bar until the pointer is on it; then it grows to fit the magnified
  // icons and the tooltip. A full-height box at rest would swallow clicks on windows just above the Dock.
  const BH = m != null ? MAX + PAD * 2 + 34 : bar, BW = m != null ? MAX + PAD * 2 : bar;
  const box = position === 'bottom' ? { left: start, top: AB - (MARGIN - hide) - BH, width: glen, height: BH }
    : position === 'left' ? { left: AX + MARGIN - hide, top: start, width: BW, height: glen }
      : { left: AR - (MARGIN - hide) - BW, top: start, width: BW, height: glen };
  const back = position === 'bottom' ? { left: 0, right: 0, bottom: 0, height: bar }
    : position === 'left' ? { left: 0, top: 0, bottom: 0, width: bar } : { right: 0, top: 0, bottom: 0, width: bar };
  const place = (i) => (position === 'bottom' ? { left: iconC[i] - sizes[i] / 2, bottom: PAD }
    : position === 'left' ? { top: iconC[i] - sizes[i] / 2, left: PAD } : { top: iconC[i] - sizes[i] / 2, right: PAD });
  const dot = (s, d = 4) => (position === 'bottom' ? { left: s / 2 - d / 2, bottom: -5 - d / 2 } : position === 'left' ? { top: s / 2 - d / 2, left: -5 - d / 2 } : { top: s / 2 - d / 2, right: -5 - d / 2 });
  const dotColor = t.mode === 'dark' ? 'rgba(255,255,255,0.85)' : 'rgba(0,0,0,0.7)';
  // where to open the right-click menu and the tooltip: beside the icon, on the inner side
  const anchor = (i) => (position === 'bottom' ? { x: start + iconC[i], y: AB - MARGIN - bar - 6 }
    : position === 'left' ? { x: AX + MARGIN + bar + 8, y: start + iconC[i] } : { x: AR - MARGIN - bar - 8, y: start + iconC[i] });
  const hit = hovered >= 0 ? items[hovered] : null;
  const showTip = hovered >= 0 && !keepOpen;
  const label = !hit ? '' : hit.trash ? tr('shell.trash.name') : hit.launcher ? tr('shell.menu.apps') : appName(hit);
  const tip = (
    <View style={{ paddingHorizontal: 10, paddingVertical: 4, borderRadius: 7, backgroundColor: t.panelBg,
      backdropFilter: `blur(${t.blur}px)`, border: `1px solid ${t.windowBorder}` }}>
      <Text style={{ fontSize: 12, fontWeight: 500, color: t.text, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
  // separators: halfway between the icons around them
  const sepLines = [];
  {
    let k = 0;
    for (let j = 0; j < vis.length; j++) {
      if (vis[j].item) { k++; continue; }
      if (!vis[j].sep || k === 0 || k >= n) continue;
      const mid = (iconC[k - 1] + sizes[k - 1] / 2 + iconC[k] - sizes[k] / 2) / 2;
      sepLines.push(mid);
    }
  }
  const trashLit = (drag && drag.over && drag.over.trash) || (trashHover && buttonDown.current && !drag);
  const ghost = drag ? (
    <View style={{ position: 'absolute', left: drag.x - BASE / 2, top: drag.y - BASE / 2, width: BASE, height: BASE, pointerEvents: 'none',
      opacity: drag.removing ? 0.55 : 0.92 }}>
      {drag.Icon ? <drag.Icon size={BASE} /> : null}
      {drag.removing && (
        <View style={{ position: 'absolute', left: BASE / 2 - 50, width: 100, top: BASE + 6, alignItems: 'center' }}>
          <View style={{ paddingHorizontal: 8, paddingVertical: 3, borderRadius: 6, backgroundColor: t.panelBg, border: `1px solid ${t.windowBorder}` }}>
            <Text style={{ fontSize: 11, fontWeight: 600, color: t.text, whiteSpace: 'nowrap' }}>{tr('shell.dock.removeCue')}</Text>
          </View>
        </View>
      )}
      {drag.removing && (
        <View style={{ position: 'absolute', right: -6, top: -6, width: 18, height: 18, borderRadius: 9, backgroundColor: 'rgba(235,238,245,0.95)',
          alignItems: 'center', justifyContent: 'center', boxShadow: '0 1px 4px rgba(0,0,0,0.35)' }}>
          <Text style={{ fontSize: 12, fontWeight: 700, color: '#4a4f5c' }}>×</Text>
        </View>
      )}
    </View>
  ) : null;

  return (
    <>
      <View onMouseMove={(e) => setM(vert ? e.y : e.x)} onMouseLeave={() => setM(null)}
        style={{ position: 'absolute', ...box, direction: 'ltr' }}>
        <View style={{ position: 'absolute', ...back, borderRadius: 20,
          backgroundColor: t.dockBg, backdropFilter: `blur(${t.blur}px)`, border: `1px solid ${t.dockBorder}` }} />
        {sepLines.map((c, i) => (
          <View key={`sep${i}`} style={vert ? { position: 'absolute', top: c, height: 1, width: BASE * 0.7, [position]: PAD + BASE * 0.15, backgroundColor: t.dockBorder }
            : { position: 'absolute', left: c, width: 1, height: BASE * 0.7, bottom: PAD + BASE * 0.15, backgroundColor: t.dockBorder }} />
        ))}
        {items.map((app, i) => (app.trash ? (
          <View key={TRASH} dropTrash onClick={() => { if (!dockDragJustEnded()) onTrash?.(); }}
            onContextMenu={() => onTrashContext?.(anchor(i))}
            onMouseEnter={() => setTrashHover(true)} onMouseLeave={() => setTrashHover(false)}
            onMouseUp={() => setTimeout(refreshTrash, 60)}
            style={{ position: 'absolute', ...place(i), width: sizes[i], height: sizes[i], borderRadius: sizes[i] * 0.22,
              backgroundColor: trashLit ? 'rgba(255,255,255,0.22)' : 'transparent' }}>
            <TrashIcon size={sizes[i]} full={trashFull} dark={t.mode === 'dark'} />
          </View>
        ) : (
          <View key={app.id} onClick={() => { if (!dockDragJustEnded()) onOpen(app.id); }} onContextMenu={() => onContext?.(app.id, anchor(i))}
            onMouseDown={(e) => { if (!e.button) beginDockDrag({ id: app.id, Icon: app.Icon, x: e.x, y: e.y, source: 'dock' }); }}
            style={{ position: 'absolute', ...place(i), width: sizes[i], height: sizes[i] }}>
            <app.Icon size={sizes[i]} />
            {/* a web app's unread count (SetBadge, docs/WEBAPPS.md): a red badge, "99+" past 99 */}
            {app.badge ? (
              <View style={{ position: 'absolute', right: -Math.round(sizes[i] * 0.08), top: -Math.round(sizes[i] * 0.06), minWidth: 18, height: 18, paddingHorizontal: 5,
                borderRadius: 9, backgroundColor: '#ff3b30', alignItems: 'center', justifyContent: 'center', boxShadow: '0 1px 3px rgba(0,0,0,0.35)', pointerEvents: 'none' }}>
                <Text style={{ fontSize: 11, fontWeight: 700, color: '#ffffff', whiteSpace: 'nowrap' }}>{app.badge}</Text>
              </View>
            ) : null}
            {/* a window that asked for attention (xdg-activation): a larger dot in the accent */}
            {running.includes(app.id) && (app.attention
              ? <View style={{ position: 'absolute', ...dot(sizes[i], 6), width: 6, height: 6, borderRadius: 3, backgroundColor: t.accent }} />
              : <View style={{ position: 'absolute', ...dot(sizes[i]), width: 4, height: 4, borderRadius: 2, backgroundColor: dotColor }} />
            )}
          </View>
        )))}
      </View>
      {/* the name tooltip (not while the icon's menu is open: the menu takes that place) */}
      <Layer name="tooltip">
        {showTip && position === 'bottom' && (
          <View style={{ position: 'absolute', left: start + iconC[hovered] - 90, width: 180, bottom: screen.height - (AB - MARGIN + hide) + sizes[hovered] + PAD + 12,
            alignItems: 'center', pointerEvents: 'none', direction: 'ltr' }}>{tip}</View>
        )}
        {showTip && vert && (
          <View style={{ position: 'absolute', top: start + iconC[hovered] - 13, pointerEvents: 'none', direction: 'ltr',
            ...(position === 'left' ? { left: AX + MARGIN - hide + PAD + sizes[hovered] + 14 } : { right: screen.width - AR + MARGIN - hide + PAD + sizes[hovered] + 14 }) }}>{tip}</View>
        )}
      </Layer>
      <Layer name="drag">
        {ghost}
        <Poof poof={poof} size={BASE} reduce={reduce} />
      </Layer>
    </>
  );
}
