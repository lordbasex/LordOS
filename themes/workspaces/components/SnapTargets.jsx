// Where a window will go, Lord Workspaces only (the other themes keep the SDK's own preview):
//   GlassTarget     the glass rectangle of a target: the Crystal material's blur over LordOS's own surfaces (a tint over
//                   Linux apps, whose pixels the renderer doesn't blur on the GPU yet), a 1 px inner light, the accent edge;
//                   it grows from the pointer in 150 ms, with no motion under Reduce motion
//   DropTarget      while a window is dragged over a tiled display: the slot it will land in (tiler.js slotPreview); the
//                   drop then takes that slot (dropHint), whatever the window's centre
//   SnapFlyout      the layouts under a window's maximize button after a short hover: halves, thirds, main and side,
//                   quarters; a click puts the window in that zone (Free: the SDK's snap zones, the magnet fills the other
//                   half) or in that slot of that layout (a tiled display: the layout is set there)
import { View, Text, Layer, addListener, useStore, createStore, moveGrab, displayAt, activeDesktop, wm, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { tween, EASE } from '../motion.js';
import { slotPreview, dropHint, layoutOf, keyOfDisplay, placeInSlot, windowLocked } from '../tiler.js';

const POLL_MS = 90;          // the SDK's own snap preview reads the pointer as often
export const FLYOUT_DELAY = 450;

// { id, dk, rect } while a dragged window would land in a tile, else null
export const dropTarget = createStore(null);
// { id, x, y } while the layout flyout is open for window id, anchored under its maximize button
export const flyout = createStore(null);

export function GlassTarget({ t, rect, from }) {
  const ref = useRef(null);
  const key = `${rect.x},${rect.y},${rect.w},${rect.h}`;
  useEffect(() => {
    const n = ref.current?.id;
    if (!n) return;
    if (from) {
      try { lord.setStyle(n, 'transformOriginX', Math.max(0, Math.min(rect.w, from.x - rect.x))); lord.setStyle(n, 'transformOriginY', Math.max(0, Math.min(rect.h, from.y - rect.y))); } catch (e) { /* gone */ }
    }
    tween(n, { scaleX: [0.94, 1], scaleY: [0.94, 1], opacity: [0, 1] }, { ms: 150, ease: EASE.out });
  }, [key]);
  const blur = t.mat.blur;
  return (
    <View ref={ref} style={{ position: 'absolute', left: rect.x + 6, top: rect.y + 6, width: rect.w - 12, height: rect.h - 12, borderRadius: 14, pointerEvents: 'none',
      backgroundColor: t.mode === 'dark' ? 'rgba(124,92,255,0.16)' : 'rgba(91,63,224,0.12)', border: `2px solid ${t.accent}`,
      ...(blur ? { backdropFilter: `blur(${Math.min(blur, 18)}px)` } : null), overflow: 'hidden' }}>
      <View style={{ position: 'absolute', left: 1, right: 1, top: 1, bottom: 1, borderRadius: 12, border: `1px solid ${t.mode === 'dark' ? 'rgba(255,255,255,0.28)' : 'rgba(255,255,255,0.7)'}` }} />
      {t.mat.sheen ? <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: '40%', background: t.mat.sheen }} /> : null}
    </View>
  );
}

// Follows a move grab (sdk moveGrab) a few times a second; one render per change of target, none per pointer move.
export function useDropTarget() {
  const grab = useStore(moveGrab);
  useEffect(() => {
    if (!grab || grab.edges || typeof lord === 'undefined' || !lord.wm?.pointer) return undefined;
    const id = grab.id;
    let last = null;
    const tick = () => {
      const p = lord.wm.pointer();
      const w = wm.get(id);
      if (!p.active || !w || windowLocked(w)) return;
      const d = displayAt(p.x, p.y);
      const r = d ? slotPreview(activeDesktop.get(), d, id, p.x, p.y) : null;
      last = r ? { id, dk: r.dk, rect: r.rect, x: p.x, y: p.y } : null;
      const cur = dropTarget.get();
      const same = cur && last && cur.dk === last.dk && cur.rect.x === last.rect.x && cur.rect.y === last.rect.y && cur.rect.w === last.rect.w;
      if (!same && (cur || last)) dropTarget.set(last);
    };
    tick();
    const timer = setInterval(tick, POLL_MS);
    return () => {
      clearInterval(timer);
      if (last) dropHint(id, last.dk, last.x, last.y);
      dropTarget.set(null);
    };
  }, [grab]);
}

export function DropTarget({ t }) {
  const d = useStore(dropTarget);
  return d ? <GlassTarget t={t} rect={d.rect} /> : null;
}

// ---- the layout flyout ----
// each group: its cells as fractions [x, y, w, h], the SDK snap zone of each cell (Free) and the tiling layout (tiled)
const GROUPS = [
  { key: 'halves', layout: 'halves', cells: [[0, 0, 0.5, 1], [0.5, 0, 0.5, 1]], zones: ['left', 'right'] },
  { key: 'thirds', layout: 'three', cells: [[0, 0, 1 / 3, 1], [1 / 3, 0, 1 / 3, 1], [2 / 3, 0, 1 / 3, 1]], zones: ['left-third', 'center-third', 'right-third'] },
  { key: 'split', layout: 'split', cells: [[0, 0, 2 / 3, 1], [2 / 3, 0, 1 / 3, 1]], zones: ['left-two-thirds', 'right-third'] },
  { key: 'quarters', layout: 'grid', cells: [[0, 0, 0.5, 0.5], [0.5, 0, 0.5, 0.5], [0, 0.5, 0.5, 0.5], [0.5, 0.5, 0.5, 0.5]], zones: ['top-left', 'top-right', 'bottom-left', 'bottom-right'] },
];
const MINI_W = 76, MINI_H = 46, GAP = 3;

export function applyZone(id, g, i) {
  const w = wm.get(id);
  if (!w || windowLocked(w)) return;
  const desk = activeDesktop.get();
  const d = displayAt(w.x + w.w / 2, w.y + w.h / 2);
  if (!d) return;
  const dk = keyOfDisplay(d);
  if (layoutOf(desk, dk).layout === 'free') { wm.snap(id, g.zones[i]); return; }
  placeInSlot(desk, dk, id, g.layout, i);
}

function Cell({ t, x, y, w, h, onPick }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onPick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ position: 'absolute', left: x, top: y, width: w, height: h, borderRadius: 5,
        backgroundColor: hover ? t.accent : t.mode === 'dark' ? 'rgba(255,255,255,0.10)' : 'rgba(20,16,60,0.08)',
        border: `1px solid ${hover ? t.accent : t.separator}` }} />
  );
}

export function SnapFlyout({ t }) {
  const f = useStore(flyout);
  const ref = useRef(null);
  useEffect(() => {
    if (!f) return undefined;
    const offDown = addListener('pointerdown', (e) => { for (let n = e.target; n; n = n.parent) if (n === ref.current) return; flyout.set(null); });
    const offKey = addListener('keydown', (e) => { if (e.key === 'Escape') flyout.set(null); });
    return () => { offDown(); offKey(); };
  }, [f]);
  if (!f) return null;
  const pad = 10;
  const width = GROUPS.length * MINI_W + (GROUPS.length - 1) * 10 + pad * 2;
  return (
    <Layer name="popover">
      <View ref={ref} onMouseLeave={() => flyout.set(null)}
        style={{ position: 'absolute', left: Math.max(4, f.x - width + 40), top: f.y, width, padding: pad, gap: 8, borderRadius: 14,
          backgroundColor: t.mat.blur ? t.mat.bg : t.mat.bgStill, border: `1px solid ${t.mat.border}`, boxShadow: t.mat.shadow,
          ...(t.mat.blur ? { backdropFilter: `blur(${t.mat.blur}px)` } : null) }}>
        <Text style={{ fontSize: 11, fontWeight: 600, color: t.textDim, textAlign: 'start' }}>{tr('shell.ws.snapfly.title')}</Text>
        <View style={{ flexDirection: 'row', gap: 10, direction: 'ltr' }}>
          {GROUPS.map((g) => (
            <View key={g.key} title={tr(`shell.ws.snapfly.${g.key}`)} style={{ width: MINI_W, height: MINI_H }}>
              {g.cells.map(([cx, cy, cw, ch], i) => (
                <Cell key={i} t={t} x={cx * MINI_W + (cx ? GAP / 2 : 0)} y={cy * MINI_H + (cy ? GAP / 2 : 0)}
                  w={cw * MINI_W - (cx ? GAP / 2 : 0) - (cx + cw < 1 ? GAP / 2 : 0)} h={ch * MINI_H - (cy ? GAP / 2 : 0) - (cy + ch < 1 ? GAP / 2 : 0)}
                  onPick={() => { flyout.set(null); applyZone(f.id, g, i); }} />
              ))}
            </View>
          ))}
        </View>
      </View>
    </Layer>
  );
}
