// cooling.jsx — the session side of lord-fand (docs/FANS.md), shared by Settings → Cooling and
// Monitor → Cooling:
//
//   useFan()            status of lord-fand ({ temp, fans, profile, curve, step, … } or null), polled
//                       every 2 s only while something on screen uses it, plus a short history
//   fanHistory          store with the last FAN_HISTORY samples { t, temp, pct, rpm }
//   coolingVariant(s)   'argon' | 'cooler' | 'pc' | 'none': which illustration fits the hardware
//   CoolingArt          our own drawing of that hardware; the fan spins with the real duty and the
//                       heatsink warms from blue to orange with the temperature
//   FanCurveChart       the profile's steps (°C → %), the hysteresis band, the live point; the steps
//                       can be dragged when `onChange` is given
//
// The illustrations are drawn here with views (no product photos or renders). The animation runs only
// while the fan turns and the window is visible, at 20 frames per second, and never with
// Appearance → Reduce motion.
import { createElement as h, useEffect, useRef, useState } from 'react';
import { View, Text, addListener } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { settings } from './settings.js';
import { useTheme } from './theme.js';
import { t, formatNumber } from './i18n.js';
import { windows } from './windows.js';
import { useAppWindow } from './components.jsx';

const has = () => typeof lord !== 'undefined' && lord.fan && typeof lord.fan.status === 'function';
const call = (fn, fallback) => { try { return fn(); } catch (e) { return fallback; } };

export const FAN_HISTORY = 90;                  // 3 minutes at one sample every 2 s
export const fanStatus = createStore(undefined); // undefined: not asked yet; null: no service
export const fanHistory = createStore([]);
export const fanCase = createStore(undefined);

let users = 0;
let timer = null;
function poll() {
  const s = has() ? call(() => lord.fan.status(), null) : null;
  fanStatus.set(() => s);
  if (fanCase.get() === undefined) fanCase.set(() => (has() ? call(() => lord.fan.caseConfig(), null) : null));
  if (s && s.temp != null) {
    const f = (s.fans || [])[0];
    const sample = { t: Date.now(), temp: s.temp, pct: f && f.pct >= 0 ? f.pct : 0, rpm: f && f.rpm >= 0 ? f.rpm : -1 };
    fanHistory.set((l) => (l.length >= FAN_HISTORY ? [...l.slice(l.length - FAN_HISTORY + 1), sample] : [...l, sample]));
  }
  timer = users > 0 ? setTimeout(poll, 2000) : null;
}
// Ask again now (after a change from Settings).
export function refreshFan() { if (timer) clearTimeout(timer); poll(); }

export function useFan() {
  useEffect(() => {
    users++;
    if (!timer) poll();
    return () => { users--; };
  }, []);
  return useStore(fanStatus);
}

// ---- requests: '' when done, or an error code (errors.<code>) ----
const ask = (fn) => { const r = has() ? call(fn, 'fan.badRequest') : 'fan.unavailable'; refreshFan(); return r; };
export const fanControl = {
  setProfile: (p) => ask(() => lord.fan.setProfile(p)),
  setCurve: (pts) => ask(() => lord.fan.setCurve(pts)),
  force: (v) => ask(() => lord.fan.force(v)),
};

export function coolingVariant(s, caseId = fanCase.get()) {
  if (!s || !s.fans || !s.fans.length) return 'none';
  if (s.fans.some((f) => f.kind === 'board')) return caseId === 'argon-one-v5' ? 'argon' : 'cooler';
  return 'pc';
}

// ---- colors ----

const mix = (a, b, k) => a.map((x, i) => Math.round(x + (b[i] - x) * k));
// cool blue up to 40 °C, amber at 65 °C, orange-red from 80 °C
export function heatColor(temp, alpha = 1) {
  const c = temp == null ? [90, 160, 255]
    : temp <= 40 ? [90, 160, 255]
      : temp <= 65 ? mix([90, 160, 255], [255, 180, 64], (temp - 40) / 25)
        : mix([255, 180, 64], [255, 90, 54], Math.min(1, (temp - 65) / 15));
  return `rgba(${c[0]},${c[1]},${c[2]},${alpha})`;
}

// ---- the spinning fan ----

// Rotation that follows the duty: up to 1.2 turns per second at 100 %, eased; stops (no timer) at
// 0 %, while the window is minimized and with Reduce motion. The rotors are turned with
// lord.setStyle on their nodes (no React render per frame): lord-server repaints a cached raster.
function useSpin(pct) {
  const { id } = useAppWindow() || {};
  const hidden = useStore(windows, (ws) => !!ws.find((w) => w.id === id)?.minimized);
  const motion = useStore(settings, (s) => !s.reduceMotion);
  const st = useRef(null);
  if (!st.current) st.current = { v: 0, a: 0, rotors: new Map() };
  const s = st.current;
  const target = motion && !hidden ? Math.max(0, Math.min(100, pct || 0)) / 100 * 1.2 : 0;
  useEffect(() => {
    if (!motion || hidden || (target === 0 && s.v < 0.01)) { s.v = target; return undefined; }
    const FPS = 20;
    let tm = null;
    const step = () => {
      s.v += (target - s.v) * 0.12;
      if (target === 0 && s.v < 0.01) { s.v = 0; return; }
      s.a = (s.a + s.v * 360 / FPS) % 360;
      for (const [node, k] of s.rotors) call(() => lord.setStyle(node, 'rotate', +(s.a * k).toFixed(1)));
      tm = setTimeout(step, 1000 / FPS);
    };
    tm = setTimeout(step, 1000 / FPS);
    return () => clearTimeout(tm);
  }, [target, motion, hidden]);
  return s;
}

function Fan({ size, frame, blade, hub, spin, k = 1 }) {
  const b = Math.round(size * 0.42), bh = Math.round(size * 0.2);
  const rotor = useRef(null);
  useEffect(() => {
    const n = rotor.current?.id;
    if (!spin || n == null) return undefined;
    spin.rotors.set(n, k);
    return () => spin.rotors.delete(n);
  }, [spin, k]);
  return h(View, { style: { width: size, height: size, borderRadius: size / 2, backgroundColor: frame, alignItems: 'center', justifyContent: 'center' } },
    h(View, { ref: rotor, style: { position: 'absolute', left: 0, top: 0, width: size, height: size, transform: `rotate(${((spin?.a || 0) * k).toFixed(1)}deg)` } },
      [0, 1, 2, 3, 4].map((i) => h(View, { key: i, style: { position: 'absolute', left: 0, top: 0, width: size, height: size, transform: `rotate(${i * 72}deg)` } },
        h(View, { style: { position: 'absolute', left: size / 2 - 1, top: size / 2 - bh, width: b, height: bh, borderTopRightRadius: bh, borderBottomRightRadius: bh / 3,
          borderTopLeftRadius: 2, backgroundColor: blade } })))),
    h(View, { style: { width: size * 0.26, height: size * 0.26, borderRadius: size * 0.13, backgroundColor: hub, boxShadow: '0 1px 2px rgba(0,0,0,0.35)' } }));
}

// Fins: n thin bars spread over the box (len = their length, span = the box across them), tinted by
// the temperature glow.
function Fins({ n, w, len, span, color, glow, vertical }) {
  const gap = Math.max(1, (span - n * w) / Math.max(1, n - 1));
  return h(View, { style: { flexDirection: vertical ? 'row' : 'column', gap } },
    Array.from({ length: n }, (_, i) => h(View, { key: i, style: { width: vertical ? w : len, height: vertical ? len : w, borderRadius: 1.5,
      background: `linear-gradient(${vertical ? 180 : 90}deg, ${color}, ${glow})` } })));
}

// ---- illustrations (our own, stylized) ----

function palette(dark) {
  return dark
    ? { stage: 'linear-gradient(160deg, #23252c, #17181d)', alu: 'linear-gradient(170deg, #6b7079, #3b3f47)', aluEdge: '#2a2d33', fin: '#8b9099',
      slot: '#1b1d22', pcb: 'linear-gradient(160deg, #1f5a3d, #123826)', port: '#a7acb5', text: '#c9ccd2', shadow: 'rgba(0,0,0,0.5)', frame: '#2c2f36',
      blade: '#a9b0bb', hub: '#d9dde3', tower: 'linear-gradient(170deg, #34373e, #1f2126)', glass: 'rgba(120,140,170,0.10)' }
    : { stage: 'linear-gradient(160deg, #eef1f6, #dde2ea)', alu: 'linear-gradient(170deg, #e4e7ec, #aab0ba)', aluEdge: '#8d939d', fin: '#c3c8d0',
      slot: '#3a3d44', pcb: 'linear-gradient(160deg, #2f7a54, #1d5a3c)', port: '#7d838d', text: '#4a4f58', shadow: 'rgba(30,40,60,0.28)', frame: '#5b616b',
      blade: '#eef1f5', hub: '#ffffff', tower: 'linear-gradient(170deg, #50545c, #33363c)', glass: 'rgba(150,180,220,0.18)' };
}

function ArgonArt({ p, pct, temp, spin, W, H }) {
  const cw = Math.round(W * 0.78), ch = Math.round(H * 0.6), x = (W - cw) / 2, y = Math.round(H * 0.16);
  const glow = heatColor(temp, 0.55);
  return h(View, { style: { position: 'absolute', left: x, top: y, width: cw, height: ch } },
    // shadow and body: aluminium top over a darker base
    h(View, { style: { position: 'absolute', left: 6, right: 6, top: ch - 8, height: 16, borderRadius: 10, backgroundColor: p.shadow } }),
    h(View, { style: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, borderRadius: 14, background: p.alu, border: `1px solid ${p.aluEdge}`,
      boxShadow: `0 10px 24px ${p.shadow}`, overflow: 'hidden' } },
      // fins across the top, warming with the temperature
      h(View, { style: { position: 'absolute', left: 18, top: 16, width: cw * 0.52, height: ch * 0.62, justifyContent: 'space-between' } },
        h(Fins, { n: 7, w: 3, len: cw * 0.52, span: ch * 0.62, color: p.fin, glow, vertical: false })),
      // round blower window with the fan
      h(View, { style: { position: 'absolute', right: 22, top: (ch - ch * 0.62) / 2 - 4, width: ch * 0.62, height: ch * 0.62, borderRadius: ch * 0.31,
        backgroundColor: p.slot, alignItems: 'center', justifyContent: 'center', border: `2px solid ${p.aluEdge}` } },
        h(Fan, { size: ch * 0.54, frame: p.slot, blade: p.blade, hub: p.hub, spin })),
      // front edge: two USB, the audio jacks and a light
      h(View, { style: { position: 'absolute', left: 18, bottom: 9, flexDirection: 'row', alignItems: 'center', gap: 7 } },
        h(View, { style: { width: 18, height: 7, borderRadius: 1.5, backgroundColor: p.slot, border: `1px solid ${p.port}` } }),
        h(View, { style: { width: 18, height: 7, borderRadius: 1.5, backgroundColor: p.slot, border: `1px solid ${p.port}` } }),
        h(View, { style: { width: 7, height: 7, borderRadius: 3.5, backgroundColor: p.slot, border: `1px solid ${p.port}` } }),
        h(View, { style: { width: 7, height: 7, borderRadius: 3.5, backgroundColor: p.slot, border: `1px solid ${p.port}` } }),
        h(View, { style: { width: 4, height: 4, borderRadius: 2, marginLeft: 6, backgroundColor: pct > 0 ? '#30d158' : p.port } }))));
}

function CoolerArt({ p, temp, spin, W, H }) {
  const bw = Math.round(W * 0.66), bh = Math.round(H * 0.68), x = (W - bw) / 2, y = Math.round(H * 0.14);
  const glow = heatColor(temp, 0.6);
  const hs = Math.round(bh * 0.66);
  return h(View, { style: { position: 'absolute', left: x, top: y, width: bw, height: bh } },
    h(View, { style: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, borderRadius: 10, background: p.pcb, boxShadow: `0 10px 22px ${p.shadow}` } },
      // header pins along the top edge
      h(View, { style: { position: 'absolute', left: 14, top: 7, flexDirection: 'row', gap: 3 } },
        Array.from({ length: 16 }, (_, i) => h(View, { key: i, style: { width: 4, height: 9, borderRadius: 1, backgroundColor: '#d6c27a', opacity: 0.85 } }))),
      // ports on the right edge
      h(View, { style: { position: 'absolute', right: -6, top: 18, gap: 6 } },
        [0, 1, 2].map((i) => h(View, { key: i, style: { width: 20, height: i === 2 ? 22 : 16, borderRadius: 2, background: 'linear-gradient(90deg, #c9cdd3, #8e939b)' } }))),
      // mounting holes
      ...[[8, bh - 16], [bw - 30, bh - 16]].map(([l, t2], i) => h(View, { key: `h${i}`, style: { position: 'absolute', left: l, top: t2, width: 8, height: 8, borderRadius: 4, backgroundColor: 'rgba(0,0,0,0.35)' } })),
      // heatsink with the fan on top
      h(View, { style: { position: 'absolute', left: Math.round(bw * 0.2), top: Math.round((bh - hs) / 2) + 4, width: Math.round(hs * 1.25), height: hs, borderRadius: 8,
        background: p.alu, border: `1px solid ${p.aluEdge}`, boxShadow: `0 6px 14px ${p.shadow}`, alignItems: 'center', justifyContent: 'center' } },
        h(View, { style: { position: 'absolute', left: 6, right: 6, top: 6, bottom: 6, flexDirection: 'row', justifyContent: 'space-between' } },
          h(Fins, { n: 9, w: 3, len: hs - 12, span: hs * 1.25 - 12, color: p.fin, glow, vertical: true })),
        h(View, { style: { width: hs * 0.8, height: hs * 0.8, borderRadius: hs * 0.4, backgroundColor: p.frame, alignItems: 'center', justifyContent: 'center', border: `2px solid ${p.aluEdge}` } },
          h(Fan, { size: hs * 0.72, frame: p.frame, blade: p.blade, hub: p.hub, spin })))));
}

function PcArt({ p, temp, spin, W, H }) {
  const tw = Math.round(H * 1.08), th = Math.round(H * 0.84), x = (W - tw) / 2, y = Math.round(H * 0.08);
  const glow = heatColor(temp, 0.6);
  return h(View, { style: { position: 'absolute', left: x, top: y, width: tw, height: th } },
    h(View, { style: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, borderRadius: 12, background: p.tower, boxShadow: `0 10px 24px ${p.shadow}`, border: `1px solid ${p.aluEdge}` } },
      // glass side panel with the board inside
      h(View, { style: { position: 'absolute', left: 10, right: 10, top: 10, bottom: 10, borderRadius: 8, backgroundColor: p.glass, border: `1px solid rgba(255,255,255,0.12)` } },
        h(View, { style: { position: 'absolute', left: 12, top: 12, width: tw * 0.5, height: th * 0.66, borderRadius: 4, background: 'linear-gradient(160deg, #2b3340, #1b2029)' } },
          // CPU tower cooler
          h(View, { style: { position: 'absolute', left: tw * 0.06, top: th * 0.08, width: tw * 0.26, height: tw * 0.26, borderRadius: 4, background: p.alu, alignItems: 'center', justifyContent: 'center' } },
            h(View, { style: { position: 'absolute', left: 4, right: 4, top: 4, bottom: 4, flexDirection: 'row', justifyContent: 'space-between' } },
              h(Fins, { n: 7, w: 2, len: tw * 0.26 - 8, span: tw * 0.26 - 8, color: p.fin, glow, vertical: true })),
            h(Fan, { size: tw * 0.21, frame: p.frame, blade: p.blade, hub: p.hub, spin })),
          // memory and a card
          h(View, { style: { position: 'absolute', left: tw * 0.35, top: th * 0.08, width: 5, height: th * 0.3, borderRadius: 1, backgroundColor: '#5e6878' } }),
          h(View, { style: { position: 'absolute', left: tw * 0.38, top: th * 0.08, width: 5, height: th * 0.3, borderRadius: 1, backgroundColor: '#5e6878' } }),
          h(View, { style: { position: 'absolute', left: tw * 0.06, top: th * 0.5, width: tw * 0.36, height: 9, borderRadius: 2, backgroundColor: '#3c4554' } })),
        // rear and front case fans
        h(View, { style: { position: 'absolute', right: 12, top: 12, width: tw * 0.26, height: tw * 0.26, borderRadius: 6, backgroundColor: p.frame, alignItems: 'center', justifyContent: 'center' } },
          h(Fan, { size: tw * 0.24, frame: p.frame, blade: p.blade, hub: p.hub, spin, k: -1 })),
        h(View, { style: { position: 'absolute', right: 12, bottom: 12, width: tw * 0.26, height: tw * 0.26, borderRadius: 6, backgroundColor: p.frame, alignItems: 'center', justifyContent: 'center' } },
          h(Fan, { size: tw * 0.24, frame: p.frame, blade: p.blade, hub: p.hub, spin, k: 0.9 })))));
}

function PassiveArt({ p, temp, W, H }) {
  const cw = Math.round(W * 0.34), x = (W - cw) / 2, y = Math.round(H * 0.3);
  const glow = heatColor(temp, 0.45);
  return h(View, { style: { position: 'absolute', left: 0, top: 0, width: W, height: H } },
    // calm air lines
    ...[0, 1, 2].map((i) => h(View, { key: `a${i}`, style: { position: 'absolute', left: x - 30 + i * 18, top: y - 34 - i * 6, width: cw * 0.7, height: 3, borderRadius: 2,
      backgroundColor: p.fin, opacity: 0.35 - i * 0.08 } })),
    h(View, { style: { position: 'absolute', left: x, top: y, width: cw, height: H * 0.42, borderRadius: 8, background: p.alu, border: `1px solid ${p.aluEdge}`,
      boxShadow: `0 10px 22px ${p.shadow}`, paddingHorizontal: 8, paddingVertical: 8, flexDirection: 'row', justifyContent: 'space-between' } },
      h(Fins, { n: 8, w: 3, len: H * 0.42 - 16, span: cw - 16, color: p.fin, glow, vertical: true })),
    h(View, { style: { position: 'absolute', left: x - 12, top: y + H * 0.42 - 2, width: cw + 24, height: 10, borderRadius: 3, background: p.pcb } }));
}

// The illustration in a stage of the given size. variant: coolingVariant(status).
export function CoolingArt({ variant, pct = 0, temp = null, width = 320, height = 168 }) {
  const th = useTheme();
  const p = palette(th.mode === 'dark');
  const spin = useSpin(variant === 'none' ? 0 : pct);
  const W = width, H = height;
  const props = { p, pct, temp, spin, W, H };
  const Art = variant === 'argon' ? ArgonArt : variant === 'cooler' ? CoolerArt : variant === 'pc' ? PcArt : PassiveArt;
  return h(View, { style: { width: W, height: H, borderRadius: 14, background: p.stage, overflow: 'hidden', direction: 'ltr' } },
    h(View, { style: { position: 'absolute', left: W * 0.15, right: W * 0.15, top: H * 0.1, bottom: H * 0.1, borderRadius: H, backgroundColor: heatColor(temp, th.mode === 'dark' ? 0.08 : 0.1) } }),
    h(Art, props));
}

// ---- the curve chart ----

const TMIN = 30, TMAX = 90;

function useBoxWidth(ref, fallback) {
  const [w, setW] = useState(fallback);
  useEffect(() => {
    const m = () => {
      const n = ref.current;
      if (!n) return;
      const x = Math.round(lord.getLayout(n.id)[2]);
      if (x > 0) setW((o) => (Math.abs(x - o) > 1 ? x : o));
    };
    m();
    const id = setTimeout(m, 60);
    return () => clearTimeout(id);
  });
  return w;
}

// curve: [[°C, %], …]; floor: % below the first step; step: active step (-1 below the first);
// temp/pct: the live point; onChange(points) makes the steps draggable (custom profile).
export function FanCurveChart({ curve = [], floor = 0, hyst = 3, step = -1, temp = null, pct = null, height = 180, onChange }) {
  const th = useTheme();
  const dark = th.mode === 'dark';
  const box = useRef(null);
  const W = useBoxWidth(box, 480);
  const L = 34, R = 10, T = 10, B = 22;
  const pw = Math.max(40, W - L - R), ph = height - T - B;
  const X = (c) => L + ((Math.max(TMIN, Math.min(TMAX, c)) - TMIN) / (TMAX - TMIN)) * pw;
  const Y = (v) => T + (1 - Math.max(0, Math.min(100, v)) / 100) * ph;
  const [drag, setDrag] = useState(null);          // { i, pts } while dragging
  const st = useRef(null);
  const pts = drag ? drag.pts : curve;
  useEffect(() => {
    if (!onChange) return undefined;
    const offMove = addListener('pointermove', (e) => {
      const d = st.current;
      if (!d) return;
      const n = d.start.map((q) => [...q]);
      const i = d.i;
      const lo = i > 0 ? n[i - 1][0] + 1 : TMIN, hi = i < n.length - 1 ? n[i + 1][0] - 1 : TMAX;
      const plo = i > 0 ? n[i - 1][1] : 0, phi = i < n.length - 1 ? n[i + 1][1] : 100;
      n[i][0] = Math.max(lo, Math.min(hi, Math.round(d.start[i][0] + (e.x - d.x) * (TMAX - TMIN) / pw)));
      n[i][1] = Math.max(plo, Math.min(phi, Math.round((d.start[i][1] - (e.y - d.y) * 100 / ph) / 5) * 5));
      d.pts = n;
      setDrag({ i, pts: n });
    });
    const offUp = addListener('pointerup', () => {
      const d = st.current;
      st.current = null;
      if (!d) return;
      setDrag(null);
      if (d.pts && JSON.stringify(d.pts) !== JSON.stringify(d.start)) onChange(d.pts);
    });
    return () => { offMove(); offUp(); };
  }, [onChange, pw, ph]);

  const accent = th.accent;
  const grid = th.separator;
  const kids = [];
  // grid and labels
  for (const v of [0, 25, 50, 75, 100]) {
    kids.push(h(View, { key: `gy${v}`, style: { position: 'absolute', left: L, width: pw, top: Y(v), height: 1, backgroundColor: grid, opacity: v === 0 ? 1 : 0.6 } }));
    kids.push(h(Text, { key: `ly${v}`, style: { position: 'absolute', left: 0, width: L - 6, top: Y(v) - 7, fontSize: 10, color: th.textDim, textAlign: 'right' } }, `${v}%`));
  }
  for (let c = 30; c <= 90; c += 10) {
    kids.push(h(View, { key: `gx${c}`, style: { position: 'absolute', top: T, height: ph, left: X(c), width: 1, backgroundColor: grid, opacity: 0.45 } }));
    kids.push(h(Text, { key: `lx${c}`, style: { position: 'absolute', top: T + ph + 5, left: X(c) - 16, width: 32, fontSize: 10, color: th.textDim, textAlign: 'center' } }, `${c}°`));
  }
  // the steps: filled area and the line
  const segs = [];
  if (pts.length) segs.push([TMIN, pts[0][0], floor, -1]);
  pts.forEach((q, i) => segs.push([q[0], i + 1 < pts.length ? pts[i + 1][0] : TMAX, q[1], i]));
  segs.forEach(([a, b, v, i]) => {
    const on = i === step;
    kids.push(h(View, { key: `f${i}`, style: { position: 'absolute', left: X(a), width: Math.max(0, X(b) - X(a)), top: Y(v), height: Math.max(0, Y(0) - Y(v)),
      backgroundColor: accent, opacity: on ? 0.22 : 0.1 } }));
    kids.push(h(View, { key: `s${i}`, style: { position: 'absolute', left: X(a), width: Math.max(2, X(b) - X(a)), top: Y(v) - 1, height: 2.5, borderRadius: 1, backgroundColor: accent } }));
  });
  for (let i = 0; i < segs.length - 1; i++) {
    const v0 = segs[i][2], v1 = segs[i + 1][2], x = X(segs[i + 1][0]);
    kids.push(h(View, { key: `v${i}`, style: { position: 'absolute', left: x - 1, width: 2.5, top: Y(Math.max(v0, v1)), height: Math.abs(Y(v0) - Y(v1)) + 1, backgroundColor: accent } }));
  }
  // hysteresis bands: a step is left only hyst °C below it
  pts.forEach((q, i) => {
    kids.push(h(View, { key: `hy${i}`, style: { position: 'absolute', left: X(q[0] - hyst), width: X(q[0]) - X(q[0] - hyst), top: T, height: ph,
      backgroundColor: '#ff9f0a', opacity: i === step ? 0.16 : 0.06 } }));
  });
  // step handles
  pts.forEach((q, i) => {
    const r = onChange ? 7 : 5;
    kids.push(h(View, { key: `p${i}`,
      onMouseDown: onChange ? (e) => { st.current = { i, x: e.x, y: e.y, start: pts.map((z) => [...z]), pts: null }; } : undefined,
      style: { position: 'absolute', left: X(q[0]) - r, top: Y(q[1]) - r, width: r * 2, height: r * 2, borderRadius: r, cursor: onChange ? 'grab' : 'default',
        backgroundColor: i === step ? accent : (dark ? '#1c1d22' : '#ffffff'), border: `2px solid ${accent}` } }));
    if (drag && drag.i === i) {
      kids.push(h(Text, { key: `dl${i}`, style: { position: 'absolute', left: X(q[0]) - 40, width: 80, top: Y(q[1]) - 24, fontSize: 11, fontWeight: 600, color: th.text, textAlign: 'center' } },
        `${q[0]} °C · ${q[1]} %`));
    }
  });
  // the live point
  if (temp != null && pct != null && pct >= 0) {
    const c = heatColor(temp);
    kids.push(h(View, { key: 'halo', style: { position: 'absolute', left: X(temp) - 11, top: Y(pct) - 11, width: 22, height: 22, borderRadius: 11, backgroundColor: c, opacity: 0.25 } }));
    kids.push(h(View, { key: 'live', style: { position: 'absolute', left: X(temp) - 5, top: Y(pct) - 5, width: 10, height: 10, borderRadius: 5, backgroundColor: c, border: '2px solid #ffffff' } }));
    kids.push(h(View, { key: 'livex', style: { position: 'absolute', left: X(temp), top: Y(pct) + 6, width: 1, height: Math.max(0, Y(0) - Y(pct) - 6), backgroundColor: c, opacity: 0.6 } }));
  }
  return h(View, { ref: box, style: { height, direction: 'ltr' } }, kids);
}

// "52 °C" / "—"
export const formatTemp = (v) => (v == null ? '—' : t('cooling.temp', { v: formatNumber(Math.round(v), 0) }));
