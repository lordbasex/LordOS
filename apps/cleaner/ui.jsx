// Cleaner's look, "Noche": a night sky in the dark theme and its day version in the light one, with slowly
// twinkling stars, orbits around the hero, glass cards and cyan-to-violet accents. Colours follow the system's
// mode. Motion goes through one frame callback and lord.setStyle on nodes (no React render per frame); it runs only
// while the window is in front, and with Reduce motion nothing moves. Timing: entrances 320-500 ms with a decelerating
// curve, ambient loops on slow sines, staggers under 400 ms in all.
//
// The engine repaints one box around everything that changed in a frame, and a box that reaches a blurred surface
// grows to all of it. So things that move far apart never move in the same frame: each loop has its own frames in a
// cycle of six (SLOT), and what barely moves only sets a value when its whole pixel changes.
//
// A frame callback that changes nothing brings no flip, and with no flip the compositor sleeps until something else
// wakes it (it does so on a real display; a viewer's video keeps a headless session awake). Slots make such frames
// common, so a timer is kept pending while anything runs. It does nothing: firing is enough for the frame to follow,
// and with the screen off, when no frame runs, it fires once and stops.
import { View, Text, Image, useAppWindow, useStore, useTheme, settings, windows, wm, cleanerPalette, typeScale, spacing } from '@lordos/sdk';
import { createContext, useContext, useEffect, useMemo, useRef, useState } from 'react';

const Pal = createContext(null);
export const usePal = () => useContext(Pal);
export function PalProvider({ children }) {
  const th = useTheme();
  const s = useStore(settings);
  return <Pal.Provider value={cleanerPalette(s, th.mode)}>{children}</Pal.Provider>;
}

// bits of a cycle of six frames: at 60 Hz one bit is 10 updates a second
export const SLOT = { all: 0b010101, a: 0b001001, b: 0b010010, c: 0b000100, d: 0b100000, spin: 0b001010 };
const jobs = new Map();
let waiting = false, frame = 0, ran = 0, nudge = 0;
function ask() {
  waiting = true;
  requestAnimationFrame(tick);
  clearTimeout(nudge);
  nudge = setTimeout(() => {}, 17);
}
function tick() {
  waiting = false;
  const now = Date.now();
  if (now - ran >= 14) {                  // a faster display doesn't run them faster
    ran = now;
    const bit = 1 << (frame++ % 6);
    for (const [j, mask] of [...jobs]) if ((mask & bit) && j(now) === false) jobs.delete(j);
  }
  if (jobs.size) ask();
}
function run(fn, mask = SLOT.all) {
  jobs.set(fn, mask);
  if (!waiting) ask();
  return () => jobs.delete(fn);
}

const reduced = () => !!settings.get().reduceMotion;
const set = (id, k, v) => { try { lord.setStyle(id, k, v); } catch (e) { /* the node went away */ } };
const decel = (x) => 1 - (1 - x) ** 3;

export function tween(node, props, { ms = 320, delay = 0, done } = {}) {
  const id = node && node.id;
  if (id == null) return () => {};
  const end = () => { for (const k in props) set(id, k, props[k][1]); if (done) done(); };
  if (reduced()) { end(); return () => {}; }
  for (const k in props) set(id, k, props[k][0]);
  const start = Date.now() + delay;
  return run((now) => {
    if (now < start) return true;
    const x = Math.min(1, (now - start) / ms), e = decel(x);
    for (const k in props) set(id, k, props[k][0] + (props[k][1] - props[k][0]) * e);
    if (x >= 1) { end(); return false; }
    return true;
  });
}

// a number or a yes/no from the window's frame: a window that moves renders nothing unless this changes
export function useFrame(pick) {
  const { id } = useAppWindow() || {};
  return useStore(windows, (ws) => { const w = ws.find((x) => x.id === id); return w ? pick(wm.frame(w)) : null; });
}

export function useLive() {
  const w = useAppWindow() || {};
  const hidden = useStore(windows, (ws) => !!ws.find((x) => x.id === w.id)?.minimized);
  const still = useStore(settings, (s) => !!s.reduceMotion);
  return !still && !hidden && !!w.focused;
}

export function useLoop(fn, on = true, mask = SLOT.all) {
  const live = useLive();
  const f = useRef(fn);
  f.current = fn;
  useEffect(() => {
    if (!live || !on) return undefined;
    const t0 = Date.now();
    return run((now) => { f.current((now - t0) / 1000); return true; }, mask);
  }, [live, on, mask]);
}

export function Enter({ children, style, delay = 0, y = 0, x = 12, ms = 420, scale = 1 }) {
  const r = useRef(null);
  useEffect(() => tween(r.current, { opacity: [0, 1], translateX: [x, 0], translateY: [y, 0], ...(scale !== 1 ? { scaleX: [scale, 1], scaleY: [scale, 1] } : null) }, { delay, ms }), []);
  return <View ref={r} style={style}>{children}</View>;
}

export function Label({ children, size = typeScale.body, weight = 400, dim, mono, lines, style, align = 'start', tref }) {
  const p = usePal();
  return (
    <Text ref={tref} style={{ fontSize: size, fontWeight: weight, color: dim ? p.sub : p.text, textAlign: align,
      ...(mono ? { fontFamily: 'monospace' } : null), ...(lines ? { whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' } : null), ...style }}>{children}</Text>
  );
}

// A number that counts up to its real value, through the text node
export function CountUp({ value, format, size = 30, weight = 600, color }) {
  const p = usePal();
  const r = useRef(null);
  useEffect(() => {
    const id = r.current && r.current.id;
    if (id == null) return undefined;
    const put = (v) => { try { lord.setText(id, format(v)); } catch (e) { /* gone */ } };
    if (reduced() || !value) { put(value); return undefined; }
    const t0 = Date.now();
    return run((now) => {
      const x = Math.min(1, (now - t0) / 800);
      put(x >= 1 ? value : value * decel(x));
      return x < 1;
    });
  }, [value]);
  return <Label tref={r} size={size} weight={weight} style={{ color: color || p.text }}>{format(value)}</Label>;
}

export function Tile({ name, size = 28 }) {
  return <Image src={`asset:cleaner/tile-${name}.png`} style={{ width: size, height: size, flexShrink: 0 }} />;
}

export function Glass({ children, style, onClick, lift }) {
  const p = usePal();
  const r = useRef(null);
  const hover = (a, b) => () => { if (lift) tween(r.current, { translateX: [a, b] }, { ms: 180 }); };
  return (
    <View ref={r} onClick={onClick} onMouseEnter={hover(0, 3)} onMouseLeave={hover(3, 0)}
      style={{ backgroundColor: p.glass, borderRadius: 14, border: `1px solid ${p.line}`, ...style }}>{children}</View>
  );
}

export function Pill({ label, on, onClick }) {
  const p = usePal();
  return (
    <View onClick={onClick} style={{ height: 28, paddingHorizontal: 12, borderRadius: 14, justifyContent: 'center',
      background: on ? `linear-gradient(135deg, ${p.btnA}, ${p.btnB})` : undefined, backgroundColor: on ? undefined : p.glass, border: `1px solid ${on ? 'transparent' : p.line}` }}>
      <Text style={{ fontSize: typeScale.callout, fontWeight: 600, whiteSpace: 'nowrap', color: on ? p.btnText : p.text }}>{label}</Text>
    </View>
  );
}

export function GlassButton({ label, onClick, disabled, small, danger }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  return (
    <View onClick={disabled ? undefined : onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: small ? 28 : 40, paddingHorizontal: small ? 12 : 18, borderRadius: small ? 10 : 12, justifyContent: 'center', alignItems: 'center',
        backgroundColor: danger ? p.danger : hover && !disabled ? p.track : p.glass, border: danger ? 'none' : `1px solid ${p.line}`, opacity: disabled ? 0.5 : 1, flexShrink: 0 }}>
      <Text style={{ fontSize: small ? typeScale.callout : typeScale.body, fontWeight: 600, color: danger ? '#ffffff' : p.text, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}

export function Check({ on, onClick }) {
  const p = usePal();
  return (
    <View onClick={onClick} style={{ width: 22, height: 22, borderRadius: 7, alignItems: 'center', justifyContent: 'center', flexShrink: 0, backgroundColor: on ? p.accent : p.track }}>
      {on ? <Text style={{ fontSize: 14, lineHeight: 16, fontWeight: 700, color: p.btnText }}>✓</Text> : null}
    </View>
  );
}

export function Bar({ value, style, h = 6 }) {
  const p = usePal();
  return (
    <View style={{ height: h, borderRadius: h / 2, backgroundColor: p.track, overflow: 'hidden', direction: 'ltr', ...style }}>
      <View style={{ width: `${Math.round(Math.max(0, Math.min(1, value)) * 1000) / 10}%`, height: h, borderRadius: h / 2, background: `linear-gradient(90deg, ${p.barA}, ${p.barB})` }} />
    </View>
  );
}

// ---- the sky ----

const STARS = [[18, 22, 3], [42, 12, 2], [66, 32, 3], [84, 18, 2], [30, 72, 2], [72, 82, 3], [92, 60, 2], [52, 58, 2], [10, 48, 2], [60, 8, 2], [36, 40, 2], [78, 46, 2]];

export function Sky() {
  const p = usePal();
  const dots = useRef([]);
  // the stars breathe out of step, one star per update: a few pixels, and never in a frame with something else
  const next = useRef(0);
  useLoop((t) => {
    const i = next.current++ % STARS.length, d = dots.current[i];
    if (d) set(d.id, 'opacity', 0.35 + 0.65 * (0.5 + 0.5 * Math.sin(t * 1.85 + i * 2.1)));
  }, !!p.stars, SLOT.d);
  return (
    <>
      {p.sky ? <Image src={`asset:cleaner/${p.sky}.png`} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, width: '100%', height: '100%' }} />
        : <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, background: p.bg }} />}
      {p.stars && STARS.map(([x, y, s], i) => (
        <View key={i} ref={(r) => { dots.current[i] = r; }} style={{ position: 'absolute', left: `${x}%`, top: `${y}%`, width: s, height: s, borderRadius: s / 2, backgroundColor: p.star, opacity: 0.7, pointerEvents: 'none' }} />
      ))}
    </>
  );
}

// the window's colour for a section, crossfaded slowly
export function Tints({ section }) {
  const p = usePal();
  const refs = useRef({});
  const shown = useRef(section);
  useEffect(() => {
    const was = shown.current;
    if (was === section) return;
    shown.current = section;
    if (refs.current[was]) tween(refs.current[was], { opacity: [1, 0] }, { ms: 700 });
    if (refs.current[section]) tween(refs.current[section], { opacity: [0, 1] }, { ms: 700 });
  }, [section]);
  return Object.keys(p.tints).map((k) => (
    <View key={k} ref={(r) => { refs.current[k] = r; }} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: p.tints[k], opacity: k === section ? 1 : 0, pointerEvents: 'none' }} />
  ));
}

export function midTrim(path, max = 64) {
  if (!path || path.length <= max) return path || '';
  const k = Math.floor((max - 1) / 2);
  return `${path.slice(0, k)}…${path.slice(-k)}`;
}

// ---- a section's opening ----

const ORBIT_A = 162, ORBIT_B = 120;
const onOrbit = (r, deg) => { const a = deg * Math.PI / 180; return [Math.sin(a) * r, -Math.cos(a) * r]; };
const INNER_START = 205;

export function Hero({ art, title, line, features = [], children }) {
  const p = usePal();
  const obj = useRef(null), shade = useRef(null), outer = useRef(null), inner = useRef(null), sheen = useRef(null), sparkA = useRef(null), sparkB = useRef(null);
  const whole = useRef(99);
  const [ix, iy] = onOrbit(ORBIT_B, INNER_START);
  const small = !!useFrame((g) => g.w < 1040 || g.h < 660);   // a small window: everything still has to fit above the button
  // the two dots and the object move in frames of their own; the rings don't turn, only their dots do
  useLoop((t) => {
    if (outer.current) { const [x, y] = onOrbit(ORBIT_A, t * 40); set(outer.current.id, 'translateX', x); set(outer.current.id, 'translateY', y + ORBIT_A); }
  }, !!p.orbit, SLOT.a);
  useLoop((t) => {
    if (inner.current) { const [x, y] = onOrbit(ORBIT_B, INNER_START - t * 25.7); set(inner.current.id, 'translateX', x - ix); set(inner.current.id, 'translateY', y - iy); }
    const float = Math.round(-7 + Math.sin(t * 1.05) * 7);
    if (float !== whole.current) {
      whole.current = float;
      if (obj.current) set(obj.current.id, 'translateY', float);
      if (shade.current) { set(shade.current.id, 'opacity', 0.85 + float * 0.04); set(shade.current.id, 'scaleX', 1 + float * 0.012); }
    }
    if (sparkA.current) set(sparkA.current.id, 'opacity', 0.15 + 0.85 * Math.max(0, Math.sin(t * 1.3)));
    if (sparkB.current) set(sparkB.current.id, 'opacity', 0.15 + 0.85 * Math.max(0, Math.sin(t * 1.3 + 2.4)));
    if (sheen.current) { const x = (t % 5) / 5; set(sheen.current.id, 'translateX', x < 0.7 ? 0 : ((x - 0.7) / 0.3) * 420); }
  }, true, SLOT.b);
  return (
    <View style={{ flex: 1, flexDirection: 'row', alignItems: 'center', gap: spacing.xxl, paddingBottom: ORB_SPACE, minHeight: 0 }}>
      <View style={{ width: 340, height: 340, flexShrink: 0, alignItems: 'center', justifyContent: 'center', direction: 'ltr', ...(small ? { transform: 'scale(0.62)', marginHorizontal: -64 } : null) }}>
        {p.orbit ? <Image src={`asset:cleaner/orbit-outer-${p.orbit}.png`} style={{ position: 'absolute', left: 0, top: 0, width: 340, height: 340 }} /> : null}
        {p.orbit ? <Image src={`asset:cleaner/orbit-inner-${p.orbit}.png`} style={{ position: 'absolute', left: 42, top: 42, width: 256, height: 256 }} /> : null}
        {p.pane ? <View style={{ position: 'absolute', left: 40, top: 40, right: 40, bottom: 40, borderRadius: 40, backgroundColor: 'rgba(255,255,255,0.5)', border: '1px solid rgba(255,255,255,0.9)', boxShadow: '0 30px 60px rgba(90,60,170,0.25)', transform: 'rotate(-6deg)', overflow: 'hidden' }}>
          <View ref={sheen} style={{ position: 'absolute', top: -60, bottom: -60, left: -90, width: 70, backgroundColor: 'rgba(255,255,255,0.75)', transform: 'rotate(18deg)' }} />
        </View> : null}
        <Image src={`asset:cleaner/glass-${art}.png`} style={{ position: 'absolute', left: 39, top: 45, width: 262, height: 250 }} />
        <Image ref={shade} src="asset:cleaner/shadow.png" style={{ position: 'absolute', left: 80, top: 244, width: 180, height: 36, opacity: 0.6 }} />
        <Image ref={obj} src={`asset:cleaner/object-${art}.png`} style={{ position: 'absolute', left: 39, top: 45, width: 262, height: 250 }} />
        {p.orbit ? <Image ref={outer} src={`asset:cleaner/orbit-dot-${p.orbit}.png`} style={{ position: 'absolute', left: 156, top: -6, width: 28, height: 28 }} /> : null}
        {p.orbit ? <View ref={inner} style={{ position: 'absolute', left: 165 + ix, top: 165 + iy, width: 10, height: 10, borderRadius: 5, backgroundColor: '#ffd36b' }} /> : null}
        <Image ref={sparkA} src="asset:cleaner/spark.png" style={{ position: 'absolute', left: 58, top: 62, width: 20, height: 20 }} />
        <Image ref={sparkB} src="asset:cleaner/spark.png" style={{ position: 'absolute', left: 262, top: 176, width: 14, height: 14 }} />
      </View>
      <Enter style={{ flex: 1, minWidth: 0, gap: spacing.s }} y={14} x={0} ms={500}>
        <Label size={small ? 26 : 40} weight={300}>{title}</Label>
        <Label size={small ? typeScale.callout : 15} dim>{line}</Label>
        <View style={{ gap: small ? spacing.xs : spacing.m, marginTop: small ? spacing.xs : spacing.m }}>
          {features.map(([tile, text]) => (
            <View key={text} style={{ flexDirection: 'row', gap: spacing.m, alignItems: 'center' }}>
              <Tile name={tile} size={small ? 24 : 30} />
              <View style={{ flex: 1, minWidth: 0 }}><Label size={small ? typeScale.callout : 15} weight={600}>{text}</Label></View>
            </View>
          ))}
        </View>
        {children}
      </Enter>
    </View>
  );
}

// The large round button: it sits high enough for its pulse never to reach the Dock's blur (a box that does repaints
// the whole Dock), its glow is still and only its inside breathes, and busy it is circled by two dots.
const ORB = 96, ORB_R = ORB / 2 + 6, ORB_BOTTOM = 40;
export const ORB_SPACE = 148;                // what a section leaves free for it
export function Orb({ label, onClick, disabled, busy }) {
  const p = usePal();
  const glow = useRef(null), ring = useRef(null), press = useRef(null), dot = useRef(null), tail = useRef(null);
  useLoop((t) => {
    if (busy) {
      for (const [d, back] of [[dot, 0], [tail, 28]]) if (d.current) { const [x, y] = onOrbit(ORB_R, t * 260 - back); set(d.current.id, 'translateX', x); set(d.current.id, 'translateY', y); }
      return;
    }
    if (glow.current) set(glow.current.id, 'opacity', 0.16 * (0.5 - Math.cos((t / 3.6) * Math.PI * 2) * 0.5));
    if (ring.current) { const x = (t % 2.6) / 2.6, e = 1 - (1 - x) ** 2, s = 1 + e * 0.22; set(ring.current.id, 'scaleX', s); set(ring.current.id, 'scaleY', s); set(ring.current.id, 'opacity', 0.7 * (1 - x)); }
  }, true, busy ? SLOT.spin : SLOT.c);
  const hit = () => { if (press.current) tween(press.current, { scaleX: [0.95, 1], scaleY: [0.95, 1] }, { ms: 240 }); if (onClick) onClick(); };
  return (
    <View style={{ position: 'absolute', left: '50%', marginLeft: -80, bottom: ORB_BOTTOM, width: 160, height: 112, alignItems: 'center', justifyContent: 'center', direction: 'ltr', opacity: disabled ? 0.55 : 1 }}>
      {busy ? [[dot, 1], [tail, 0.5]].map(([d, o], i) => <View key={i} ref={d} style={{ position: 'absolute', left: 75, top: 51, width: 10, height: 10, borderRadius: 5, backgroundColor: p.ringLine, opacity: o }} />)
        : <View ref={ring} style={{ position: 'absolute', left: 32, top: 8, width: ORB, height: ORB, borderRadius: ORB / 2, border: `2px solid ${p.ringLine}`, opacity: 0 }} />}
      <View ref={press} onClick={disabled ? undefined : hit} style={{ width: ORB, height: ORB, borderRadius: ORB / 2, alignItems: 'center', justifyContent: 'center', overflow: 'hidden',
        border: `3px solid ${p.ringLine}`, background: `linear-gradient(160deg, ${p.btnA}, ${p.btnB})`, boxShadow: `0 0 30px 6px ${p.glow}` }}>
        <View ref={glow} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: '#ffffff', opacity: 0, pointerEvents: 'none' }} />
        <Text style={{ fontSize: label && label.length > 9 ? typeScale.body : 15, fontWeight: 600, color: p.btnText, textAlign: 'center', paddingHorizontal: 6 }}>{label}</Text>
      </View>
    </View>
  );
}

// A section scanning: its object large and centred, the verb, and the real path going by.
export function SectionScan({ art, verb, path, counter, found }) {
  const img = useRef(null);
  useLoop((t) => { if (img.current) { const s = 1 + Math.sin(t * 2.4) * 0.03; set(img.current.id, 'scaleX', s); set(img.current.id, 'scaleY', s); } });
  return (
    <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: spacing.s, paddingBottom: ORB_SPACE }}>
      <Image ref={img} src={`asset:cleaner/hero-${art}.png`} style={{ width: 300, height: 286 }} />
      <Label size={28} weight={300}>{verb}</Label>
      <Label size={typeScale.callout} mono dim lines={1}>{midTrim(path)}</Label>
      <Label size={typeScale.callout} dim>{counter}</Label>
      {found}
    </View>
  );
}

// The ring while working: a conic bar in an Image (the engine has no conic gradient), only with a known total,
// and a fast spinner. With no total, only the spinner turns and the middle shows real counters.
const RING = 240;
const angles = (() => {
  let a = null;
  return () => {
    if (a) return a;
    a = new Float32Array(RING * RING);
    for (let y = 0; y < RING; y++) for (let x = 0; x < RING; x++) {
      const dx = x + 0.5 - RING / 2, dy = y + 0.5 - RING / 2, r = Math.hypot(dx, dy);
      a[y * RING + x] = r > RING / 2 || r < RING / 2 - 10 ? -1 : ((Math.atan2(dx, -dy) / (Math.PI * 2) + 1) % 1);
    }
    return a;
  };
})();
const hex = (s) => { const n = parseInt(s.slice(1), 16); return [n >> 16 & 255, n >> 8 & 255, n & 255]; };

export function ProgressRing({ progress, big, small, under }) {
  const p = usePal();
  const spin = useRef(null);
  useLoop((t) => { if (spin.current) set(spin.current.id, 'rotate', (t / 1.4 * 360) % 360); });
  const pixels = useMemo(() => {
    const a = angles(), data = new Uint8Array(RING * RING * 4), c = hex(p.accent);
    const known = progress != null;
    for (let i = 0; i < a.length; i++) {
      if (a[i] < 0) continue;
      const on = known && a[i] <= progress, o = i * 4;
      data[o] = c[0]; data[o + 1] = c[1]; data[o + 2] = c[2]; data[o + 3] = on ? 255 : 46;
    }
    return { width: RING, height: RING, data };
  }, [progress == null ? -1 : Math.round(progress * 200), p]);
  return (
    <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: spacing.m, paddingBottom: ORB_SPACE }}>
      <View style={{ width: RING, height: RING, alignItems: 'center', justifyContent: 'center', direction: 'ltr' }}>
        <Image pixels={pixels} style={{ position: 'absolute', left: 0, top: 0, width: RING, height: RING }} />
        <Image ref={spin} src="asset:cleaner/ring.png" style={{ position: 'absolute', left: 24, top: 24, width: RING - 48, height: RING - 48 }} />
        <Label size={36} weight={300}>{big}</Label>
        <Label size={typeScale.callout} dim>{small}</Label>
      </View>
      {under}
    </View>
  );
}

// A long list drawn a window at a time: fetch(offset, limit) → { items, total }; a few dozen views at most.
export function PagedList({ fetch, rowH = 34, render, reset, style }) {
  const p = usePal();
  const [y, setY] = useState(0);
  const st = useRef({ pages: new Map(), total: -1 });
  const PAGE = 100;
  const box = useRef(null);
  const [h, setH] = useState(400);          // the box's real height, once laid out
  useEffect(() => {
    const id = setTimeout(() => { if (box.current) { const v = lord.getLayout(box.current.id)[3]; if (v > 0 && Math.abs(v - h) > 1) setH(v); } }, 30);
    return () => clearTimeout(id);
  });
  const load = (n) => {
    if (st.current.pages.has(n)) return st.current.pages.get(n);
    const r = fetch(n * PAGE, PAGE);
    st.current.pages.set(n, r.items);
    st.current.total = r.total;
    return r.items;
  };
  const seen = useRef(reset);
  if (seen.current !== reset) { seen.current = reset; st.current = { pages: new Map(), total: -1 }; }
  useEffect(() => { setY(0); }, [reset]);
  if (st.current.total < 0) load(0);
  const total = Math.max(0, st.current.total), full = total * rowH, max = Math.max(0, full - h), top = Math.min(y, max);
  const first = Math.max(0, Math.floor(top / rowH) - 1), last = Math.min(total, Math.ceil((top + h) / rowH) + 1);
  const rows = [];
  for (let i = first; i < last; i++) {
    const it = load(Math.floor(i / PAGE))[i % PAGE];
    if (it) rows.push(render(it, i));
  }
  const bar = full > h ? Math.max(28, h * (h / full)) : 0;
  return (
    <View ref={box} onWheel={(e) => setY(Math.max(0, Math.min(max, top + e.deltaY)))} style={{ flex: 1, minHeight: 0, overflow: 'hidden', ...style }}>
      <View style={{ position: 'absolute', left: 0, right: bar ? 10 : 0, top: first * rowH - top }}>{rows}</View>
      {bar ? <View style={{ position: 'absolute', end: 2, width: 5, borderRadius: 3, height: bar, top: (h - bar) * (top / (max || 1)), backgroundColor: p.sub, opacity: 0.6 }} /> : null}
    </View>
  );
}
