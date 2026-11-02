// MapView.jsx — world map of the Network tab, as in per-app network monitors: vector land
// (dibujada por axia-server en otro hilo: lord.monitor.map / mapFrame, server/src/monitor/map.c),
// un arco desde "Este equipo" a cada país con conexiones, un punto por país y etiquetas en
// píldora. Rueda: zoom hacia el cursor; arrastrar: mover; doble clic: acercar; botones +, − y
// "ver todo". Clic en un punto o en un arco: elige ese lugar.
//
// Mientras llega el cuadro nuevo se muestra el anterior corrido (y escalado, si cambió el zoom),
// así que mover y acercar no esperan al dibujo. Las etiquetas son texto del motor: siempre nítidas.
import { View, Text, Image, useTheme, useLocale, addListener, formatNumber, tOptional, useAppWindow, useStore, useWindowHidden, settings } from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { parseColor, canvas, dot } from './draw.js';
import { COUNTRY_LL } from './countries.js';
import { C, useCardStyle } from './index.jsx';

// ---- traffic direction on the arcs ----
// Dots travel along each arc while bytes move: sent (orange) from "This computer" to the place,
// received (green) back. More dots and faster the higher the rate; an idle arc has none and is
// drawn dimmed (map.c). The dots are engine views moved with lord.setStyle (no React render per
// frame) at FLOW_FPS with the window in front and FLOW_FPS_BEHIND behind it, only for arcs on
// screen and at most FLOW_ARCS of them; the timer stops with the window minimized, while the map
// moves and when nothing moves. Each dot is a small ready-made image, and each frame repaints only
// the small boxes the dots left and entered (the engine's several damaged rectangles, 0.8.2; the
// cost is in docs/MONITOR.md). With Reduce motion each busy arc gets two still arrows instead.
const FLOW_FPS = 12, FLOW_FPS_BEHIND = 5, FLOW_ARCS = 12, FLOW_MIN = 1;   // FLOW_MIN: bytes per second that count as "moving"
// any traffic moves (one slow dot up to 64 B/s), 1 from about 4 MB/s
const flowLevel = (rate) => (rate >= FLOW_MIN ? Math.max(0, Math.min(1, Math.log10(Math.max(1, rate / 64)) / 4.8)) : -1);
const bez = (a, k) => { const u = 1 - k; return [u * u * a.x0 + 2 * u * k * a.cx + k * k * a.x1, u * u * a.y0 + 2 * u * k * a.cy + k * k * a.y1]; };

// One moving dot as a ready-made 9×9 image (a ring and the direction's color): the engine only
// blits it, instead of filling and stroking a rounded box with antialiasing on every frame (0.8.2).
function dotSprite(fill, ring) {
  const cv = canvas(9, 9), [, , , ra] = parseColor(ring);
  dot(cv, 4.5, 4.5, 4.5, ring, ra);
  dot(cv, 4.5, 4.5, 2.5, fill, 1);
  return cv;
}

// arcs: [{ cc, x0, y0, cx, cy, x1, y1, tx, rx }] in the map's pixels.
function FlowLayer({ arcs, w, h, paused }) {
  const t = useTheme();
  const { id, focused } = useAppWindow() || {};
  const fps = focused === false ? FLOW_FPS_BEHIND : FLOW_FPS;
  const hidden = useWindowHidden(id);
  const motion = useStore(settings, (st) => !st.reduceMotion);
  // each moving dot: arc index, direction (1 sent, -1 received), phase offset, seconds per trip
  const dots = [];
  arcs.forEach((a, i) => {
    for (const [dir, rate] of [[1, a.tx], [-1, a.rx]]) {
      const lv = flowLevel(rate);
      if (lv < 0) continue;
      const n = 1 + Math.round(lv * 3), trip = 4 - 2.6 * lv;
      for (let k = 0; k < n; k++) dots.push({ key: `${a.cc}${dir}${k}`, arc: i, dir, off: k / n + (dir < 0 ? 0.5 / n : 0), trip });
    }
  });
  const nodes = useRef(new Map());
  const place = (d, now) => {
    const a = arcs[d.arc];
    let k = ((now / 1000) / d.trip + d.off) % 1;
    if (d.dir < 0) k = 1 - k;
    const [x, y] = bez(a, k);
    return [x - 4.5, y - 4.5, x > -6 && y > -6 && x < w + 6 && y < h + 6];
  };
  const animate = motion && !hidden && !paused && dots.length > 0;
  const sig = dots.map((d) => `${d.key}:${d.trip.toFixed(2)}`).join(',') + arcs.map((a) => `${a.x0 | 0},${a.y0 | 0},${a.x1 | 0},${a.y1 | 0}`).join(';');
  useEffect(() => {
    if (!animate) return undefined;
    let tm = null;
    const step = () => {
      const now = lord.now();
      for (const d of dots) {
        const node = nodes.current.get(d.key);
        if (node == null) continue;
        const [x, y, on] = place(d, now);
        try {
          lord.setStyle(node, 'left', on ? x : -20);
          lord.setStyle(node, 'top', on ? y : -20);
        } catch (e) { /* the node went away */ }
      }
      tm = setTimeout(step, 1000 / fps);
    };
    tm = setTimeout(step, 1000 / fps);
    return () => clearTimeout(tm);
  }, [animate, sig, w, h, fps]);
  const ring = t.mode === 'dark' ? 'rgba(20,21,24,0.85)' : 'rgba(255,255,255,0.9)';
  const sprites = useMemo(() => ({ tx: dotSprite(C.tx, ring), rx: dotSprite(C.rx, ring) }), [ring]);
  if (paused || hidden) return null;
  if (!motion) {
    // still markers at 38 % (received) and 62 % (sent) of each busy arc: a short comet of three
    // dots, the biggest in front, so it reads as pointing the way the bytes go (no rotation needed)
    const out = [];
    arcs.forEach((a) => {
      for (const [dir, rate, k] of [[1, a.tx, 0.62], [-1, a.rx, 0.38]]) {
        if (flowLevel(rate) < 0) continue;
        const [x, y] = bez(a, k), [x2, y2] = bez(a, k + 0.01 * dir);
        const len = Math.hypot(x2 - x, y2 - y) || 1, ux = (x2 - x) / len, uy = (y2 - y) / len;
        if (x < -8 || y < -8 || x > w + 8 || y > h + 8) continue;
        [[0, 7], [-6, 5], [-11, 3]].forEach(([off, size], i) => {
          const cx = x + ux * off, cy = y + uy * off;
          out.push(<View key={`${a.cc}${dir}${i}`} style={{ position: 'absolute', left: cx - size / 2, top: cy - size / 2, width: size, height: size, borderRadius: size / 2,
            backgroundColor: dir > 0 ? C.tx : C.rx, opacity: 1 - i * 0.25, pointerEvents: 'none' }} />);
        });
      }
    });
    return <>{out}</>;
  }
  const now = lord.now();
  return (
    <>
      {dots.map((d) => {
        const [x, y, on] = place(d, now);
        return (
          <Image key={d.key} ref={(r) => { if (r) nodes.current.set(d.key, r.id); else nodes.current.delete(d.key); }} pixels={d.dir > 0 ? sprites.tx : sprites.rx}
            style={{ position: 'absolute', left: on ? x : -20, top: on ? y : -20, width: 9, height: 9, pointerEvents: 'none' }} />
        );
      })}
    </>
  );
}

const LAT_TOP = 84, LAT_BOTTOM = -56;
const countryName = (cc) => (cc ? tOptional(`countries.${cc}`) || cc : '');
const rgba = (c, alpha) => { const [r, g, b, a] = parseColor(c); return ((r << 24) | (g << 16) | (b << 8) | Math.round((alpha ?? a) * 255)) >>> 0; };

// Vista: centro (lon, lat) y píxeles por grado. La mínima muestra el mundo entero (84° N a 56° S).
function clampView(v, w, h) {
  const minPpd = Math.min(w / 360, h / (LAT_TOP - LAT_BOTTOM));
  const ppd = Math.max(minPpd, Math.min(minPpd * 60, v.ppd));
  // el centro puede ir a cualquier punto del mundo: así el zoom sigue siempre al cursor
  const lon = Math.max(-180, Math.min(180, v.lon));
  const lat = Math.max(LAT_BOTTOM, Math.min(LAT_TOP, v.lat));
  return { lon, lat, ppd };
}

export function MapView({ places, home, selected, onSelect, geoOn, fitKey }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const box = useRef(null);
  const [size, setSize] = useState([0, 0]);
  const [view, setView] = useState(null);
  const [frame, setFrame] = useState(null);
  const viewRef = useRef(null);
  viewRef.current = view;
  const [w, h] = size;
  // tamaño real de la caja (llena la columna)
  useEffect(() => {
    if (!box.current) return;
    const l = lord.getLayout(box.current.id);
    const nw = Math.round(l[2]), nh = Math.round(l[3]);
    if (nw > 0 && nh > 0 && (Math.abs(nw - w) > 1 || Math.abs(nh - h) > 1)) setSize([nw, nh]);
  });
  const fit = (pts) => {
    if (!w || !h) return null;
    if (!pts.length) return clampView({ lon: 10, lat: 15, ppd: 0 }, w, h);
    let x0 = 180, x1 = -180, y0 = 90, y1 = -90;
    for (const [lon, lat] of pts) { x0 = Math.min(x0, lon); x1 = Math.max(x1, lon); y0 = Math.min(y0, lat); y1 = Math.max(y1, lat); }
    const ppd = Math.min(w / (x1 - x0 + 30), h / (y1 - y0 + 24));
    return clampView({ lon: (x0 + x1) / 2, lat: (y0 + y1) / 2 + 3, ppd }, w, h);
  };
  const allPts = () => [...places.map((p) => COUNTRY_LL[p.cc]), ...(home ? [[home.lon, home.lat]] : [])].filter(Boolean);
  useEffect(() => { if (w && h) setView((v) => (v ? clampView(v, w, h) : fit([]))); }, [w, h]);
  // primera vez con datos: encuadrar todas las conexiones (hasta que el usuario mueva el mapa)
  const touched = useRef(false);
  const placesKey = places.map((p) => p.cc).join(',');
  useEffect(() => { if (w && h && places.length && !touched.current) setView(fit(allPts())); }, [w, h, placesKey]);
  // al elegir otra app (o volver a todas): encuadrar sus lugares
  const firstFit = useRef(true);
  useEffect(() => {
    if (firstFit.current) { firstFit.current = false; return; }
    if (w && h) setView(fit(allPts()));
  }, [fitKey]);
  const v = view && w ? view : null;
  // dónde va el cuadro fr (pedido con su lon/lat/ppd) en la vista vv: corrido y escalado
  const imgBox = (vv, fr) => {
    const k = vv.ppd / fr.ppd, l0 = vv.lon - w / 2 / vv.ppd, t0 = vv.lat + h / 2 / vv.ppd;
    return { left: (fr.lon - l0) * vv.ppd, top: (t0 - fr.lat) * vv.ppd, width: fr.width * k, height: fr.height * k };
  };
  const lon0 = v ? v.lon - w / 2 / v.ppd : 0, lat0 = v ? v.lat + h / 2 / v.ppd : 0;
  const toXY = (lon, lat) => [(lon - lon0) * v.ppd, (lat0 - lat) * v.ppd];

  // puntos (radio según conexiones)
  const dots = places.filter((p) => COUNTRY_LL[p.cc]).map((p) => ({
    cc: p.cc, lon: COUNTRY_LL[p.cc][0], lat: COUNTRY_LL[p.cc][1], r: Math.min(11, 3 + 1.6 * Math.sqrt(Math.max(1, p.count))), sel: selected === p.cc, count: p.count,
    tx: p.rateTx || 0, rx: p.rateRx || 0, idle: (p.rateTx || 0) + (p.rateRx || 0) < FLOW_MIN,
  }));
  // one arc per app and place, in the app's color; the apps that reach the same place get arcs apart
  // (the busiest one lowest)
  const arcs = [];
  for (const p of places) {
    const ll = COUNTRY_LL[p.cc];
    if (!ll || !p.links) continue;
    [...p.links.values()].sort((a, b) => b.bytes - a.bytes).forEach((l, k) => {
      arcs.push({ cc: p.cc, app: l.app, lon: ll[0], lat: ll[1], lift: Math.min(0.62, 0.22 + 0.09 * k), color: l.color,
        sel: selected === p.cc, tx: l.tx || 0, rx: l.rx || 0, idle: (l.tx || 0) + (l.rx || 0) < FLOW_MIN });
    });
  }
  const dark = t.mode === 'dark';
  const card = useCardStyle();
  const colors = {
    land: rgba(dark ? '#8a93a8' : '#9aa3b5', dark ? 0.30 : 0.42),
    border: rgba(dark ? '#cfd6e6' : '#ffffff', dark ? (v && v.ppd > 6 ? 0.42 : 0.25) : 0.9),
    accent: rgba(t.accent, 1), homeColor: rgba('#30d158', 1), selColor: rgba('#ffd60a', 1),
  };
  const dataKey = JSON.stringify([dots.map((d) => [d.cc, d.r, d.sel, d.idle]), arcs.map((a) => [a.cc, a.lift, a.color, a.sel, a.idle]), home, colors]);
  // pedir un cuadro cuando cambia la vista, el tamaño o los datos; retirarlo cuando esté listo
  const pending = useRef(0);
  // Mientras se mueve (arrastrar, dos dedos con inercia, pellizco): a lo sumo un cuadro cada 100 ms
  // (se ve el anterior corrido, sin reescalar); con zoom, a media resolución; al quedar quieto, el bueno.
  const movingAt = useRef(0), lastReq = useRef(0);
  const [settle, setSettle] = useState(0);
  const [moving, setMoving] = useState(false);       // sin etiquetas mientras se mueve
  const markMoving = () => { movingAt.current = lord.now(); if (!moving) setMoving(true); };
  const latest = useRef(null);
  latest.current = { w, h, colors, home, dots, arcs };
  const frameRef = useRef(null);
  frameRef.current = frame;
  const pollT = useRef(0);
  // pide un cuadro de la vista vv (q: resolución) y lo va a buscar cuando esté listo
  const requestMap = (vv, q) => {
    const { w: ww, h: hh, colors: cc, home: hm, dots: dd, arcs: aa } = latest.current;
    lastReq.current = lord.now();
    pending.current = lord.monitor.map({ width: Math.round(ww * q), height: Math.round(hh * q), lon: vv.lon - ww / 2 / vv.ppd,
      lat: vv.lat + hh / 2 / vv.ppd, ppd: vv.ppd * q, ...cc, home: hm || null, dots: dd.map((d) => ({ lon: d.lon, lat: d.lat, r: d.r, sel: d.sel, idle: d.idle })),
      arcs: aa.map((a) => ({ lon: a.lon, lat: a.lat, lift: a.lift, color: rgba(a.color, 1), sel: a.sel, idle: a.idle })) });
    clearTimeout(pollT.current);
    const poll = () => {
      const f = lord.monitor.mapFrame();
      if (f) setFrame({ ...f, pixels: { width: f.width, height: f.height, data: f.data } });
      if (!f || f.seq < pending.current) pollT.current = setTimeout(poll, 12);
    };
    pollT.current = setTimeout(poll, 4);
  };
  useEffect(() => () => clearTimeout(pollT.current), []);
  useEffect(() => {
    if (!v || !w || !h || typeof lord.monitor?.map !== 'function') return undefined;
    const now = lord.now();
    const busy = now - movingAt.current < 150;
    const settleTimer = busy ? setTimeout(() => { setSettle((n) => n + 1); setMoving(false); }, 170) : null;
    if (busy && now - lastReq.current < 100) return () => clearTimeout(settleTimer);
    requestMap(v, busy && frame && Math.abs(frame.ppd - v.ppd) > 1e-9 ? 0.5 : 1);
    return () => clearTimeout(settleTimer);
  }, [w, h, v?.lon, v?.lat, v?.ppd, dataKey, settle]);

  // ---- interacción ----
  const zoomAt = (factor, sx, sy) => { touched.current = true; markMoving();
    const base = live.current;               // venía desplazándose con dos dedos: se sigue desde ahí
    live.current = null;
    setView((c0) => {
    const cur = base || c0;
    if (!cur) return cur;
    const l0 = cur.lon - w / 2 / cur.ppd, t0 = cur.lat + h / 2 / cur.ppd;
    const lon = l0 + sx / cur.ppd, lat = t0 - sy / cur.ppd;                 // punto bajo el cursor
    const ppd = cur.ppd * factor;
    return clampView({ lon: lon - (sx - w / 2) / ppd, lat: lat + (sy - h / 2) / ppd, ppd }, w, h);
  }); };
  const hit = (sx, sy) => {
    let best = null;
    for (const d of dots) {
      const [x, y] = toXY(d.lon, d.lat);
      const dist = Math.hypot(sx - x, sy - y);
      if (dist <= d.r + 5 && (!best || dist < best.dist)) best = { cc: d.cc, dist };
    }
    if (best || !home) return best?.cc || null;
    const [hx, hy] = toXY(home.lon, home.lat);
    for (const d of arcs) {                                                // arcos
      const [x, y] = toXY(d.lon, d.lat);
      const len = Math.hypot(x - hx, y - hy), cx = (hx + x) / 2, cy = (hy + y) / 2 - len * d.lift;
      for (let i = 0; i <= 48; i++) {
        const k = i / 48, u = 1 - k;
        const px = u * u * hx + 2 * u * k * cx + k * k * x, py = u * u * hy + 2 * u * k * cy + k * k * y;
        if (Math.hypot(sx - px, sy - py) < 5) return d.cc;
      }
    }
    return null;
  };
  const onMouseDown = (e) => {
    if (e.button && e.button !== 1 && e.button !== 0) return;
    const start = e.local(e.currentTarget), origin = viewRef.current;
    if (!origin) return;
    let moved = false;
    const offMove = addListener('pointermove', (m) => {
      const dx = m.x - e.x, dy = m.y - e.y;
      if (!moved && Math.hypot(dx, dy) < 3) return;
      moved = true;
      touched.current = true;
      markMoving();
      setView(clampView({ lon: origin.lon - dx / origin.ppd, lat: origin.lat + dy / origin.ppd, ppd: origin.ppd }, w, h));
    });
    const offUp = addListener('pointerup', () => {
      offMove();
      offUp();
      if (!moved) onSelect(hit(start.x, start.y));
    });
  };
  // trackpad: dos dedos mueven el mapa (like a maps app); ctrl + dos dedos o una rueda, zoom
  const onWheel = (e) => {
    if (e.precise && !e.ctrl) {
      const cur = viewRef.current;
      if (!cur || (!e.deltaX && !e.deltaY)) return;
      touched.current = true;
      markMoving();
      // se juntan los movimientos y la vista cambia una vez por cuadro (el touchpad manda 80 por segundo)
      const pp = panPending.current;
      pp.dx += e.deltaX;
      pp.dy += e.deltaY;
      // Sin pasar por React: se corre la imagen del último cuadro (lord.setStyle) y cada 100 ms se pide
      // otro; la vista se confirma en React cuando el movimiento se detiene (etiquetas de vuelta).
      // a lo sumo ~30 veces por segundo (cada cuadro cuesta pintar toda la tarjeta del mapa)
      if (!pp.raf) pp.raf = setTimeout(() => requestAnimationFrame(() => {
        pp.at = lord.now();
        const { dx, dy } = pp;
        pp.dx = pp.dy = 0;
        pp.raf = 0;
        const c = live.current || viewRef.current;
        if (!c) return;
        const nv = clampView({ lon: c.lon + dx / c.ppd, lat: c.lat - dy / c.ppd, ppd: c.ppd }, w, h);
        live.current = nv;
        movingAt.current = lord.now();
        const fr = frameRef.current;
        if (fr && imgRef.current) {
          const b = imgBox(nv, fr);
          lord.setStyle(imgRef.current.id, 'left', b.left);
          lord.setStyle(imgRef.current.id, 'top', b.top);
        }
        if (typeof lord.monitor?.map === 'function' && lord.now() - lastReq.current >= 100) requestMap(nv, 1);
        clearTimeout(pp.settle);
        pp.settle = setTimeout(() => { const f = live.current; live.current = null; if (f) setView(f); setMoving(false); }, 170);
      }), Math.max(0, 33 - (lord.now() - (pp.at || 0))));
      return;
    }
    if (e.precise && !e.notches) return;
    const p = e.local(e.currentTarget);
    zoomAt(Math.pow(1.25, -Math.sign(e.deltaY) * Math.min(3, Math.abs(e.notches || 1))), p.x, p.y);
  };
  const onDoubleClick = (e) => { const p = e.local(e.currentTarget); zoomAt(2, p.x, p.y); };
  // pellizcar: zoom hacia el centro del gesto (scale es relativa al comienzo: se aplica el cambio)
  const pinchScale = useRef(1);
  const panPending = useRef({ dx: 0, dy: 0, raf: 0, settle: 0, at: 0 });
  const live = useRef(null);                 // vista mientras se desplaza con dos dedos (fuera de React)
  const imgRef = useRef(null);
  useEffect(() => () => clearTimeout(panPending.current.settle), []);
  const onPinch = (e) => {
    if (e.phase === 'begin') { pinchScale.current = 1; return; }
    const k = e.scale / (pinchScale.current || 1);
    pinchScale.current = e.scale;
    if (!(k > 0) || Math.abs(k - 1) < 0.002) return;
    touched.current = true;
    const p = e.local(e.currentTarget);
    zoomAt(k, p.x, p.y);
  };

  // ---- imagen: el último cuadro, corrido/escalado a la vista actual ----
  const vNow = live.current || v;
  const img = frame && vNow ? imgBox(vNow, frame) : null;

  // ---- arcs with traffic, for the moving dots (the busiest ones, in screen pixels) ----
  const flowArcs = [];
  if (v && home) {
    const [hx, hy] = toXY(home.lon, home.lat);
    for (const d of arcs.filter((x) => !x.idle).sort((a, b) => b.tx + b.rx - (a.tx + a.rx)).slice(0, FLOW_ARCS)) {
      const [x, y] = toXY(d.lon, d.lat);
      const len = Math.hypot(x - hx, y - hy);
      if (len < 6) continue;
      const cx = (hx + x) / 2, cy = (hy + y) / 2 - len * d.lift;
      if (Math.max(hx, x, cx) < 0 || Math.min(hx, x, cx) > w || Math.max(hy, y, cy) < 0 || Math.min(hy, y, cy) > h) continue;   // off screen
      flowArcs.push({ cc: `${d.cc}${d.app}`, x0: hx, y0: hy, cx, cy, x1: x, y1: y, tx: d.tx, rx: d.rx });
    }
  }

  // ---- etiquetas ----
  const labels = useMemo(() => {
    if (!v || moving) return [];
    const out = [], boxes = [];
    const fits = (r) => r.x >= 2 && r.x + r.w <= w - 2 && r.y >= 2 && r.y + r.h <= h - 2 && !boxes.some((b) => r.x < b.x + b.w && b.x < r.x + r.w && r.y < b.y + b.h && b.y < r.y + r.h);
    if (home) {
      const [x, y] = toXY(home.lon, home.lat), text = tr('monitor.net.thisComputer');
      const r = { x: x + 9, y: y - 9, w: text.length * 5.6 + 16, h: 18 };
      if (fits(r)) { boxes.push(r); out.push({ key: 'home', text, kind: 'home', ...r }); }
    }
    for (const d of [...dots].sort((a, b) => b.sel - a.sel || b.count - a.count)) {
      const [x, y] = toXY(d.lon, d.lat);
      const text = `${countryName(d.cc)} · ${formatNumber(d.count, 0)}`, lw = text.length * 5.6 + 16;
      for (const [lx, ly] of [[x + d.r + 5, y - 9], [x - d.r - 5 - lw, y - 9], [x - lw / 2, y - d.r - 22], [x - lw / 2, y + d.r + 4]]) {
        const r = { x: lx, y: ly, w: lw, h: 18 };
        if (fits(r)) { boxes.push(r); out.push({ key: d.cc, text, kind: d.sel ? 'sel' : 'place', cc: d.cc, ...r }); break; }
      }
    }
    // más cerca: nombres de los demás países a la vista (sin píldora)
    if (v.ppd >= Math.min(w / 360, h / (LAT_TOP - LAT_BOTTOM)) * 3) {
      const have = new Set(dots.map((d) => d.cc));
      for (const [cc, [lon, lat]] of Object.entries(COUNTRY_LL)) {
        if (have.has(cc)) continue;
        const [x, y] = toXY(lon, lat);
        if (x < 0 || y < 0 || x > w || y > h) continue;
        const text = countryName(cc), lw = text.length * 5 + 4;
        const r = { x: x - lw / 2, y: y - 6, w: lw, h: 12 };
        if (fits(r)) { boxes.push(r); out.push({ key: cc, text, kind: 'name', ...r }); }
        if (out.length > 90) break;
      }
    }
    return out;
  }, [v?.lon, v?.lat, v?.ppd, w, h, dataKey, tr, moving]);

  const btn = { width: 26, height: 26, borderRadius: 7, alignItems: 'center', justifyContent: 'center',
    backgroundColor: dark ? 'rgba(40,42,48,0.92)' : 'rgba(255,255,255,0.95)', border: `1px solid ${t.separator}` };
  const btnText = { fontSize: 15, fontWeight: 600, color: t.text, lineHeight: 1 };
  return (
    // the card's own color, opaque: a repaint of the moving dots starts here instead of at the card
    <View ref={box} style={{ flex: 1, minHeight: 120, overflow: 'hidden', borderRadius: 8, direction: 'ltr', backgroundColor: card.bg }}
      onMouseDown={onMouseDown} onWheel={onWheel} onPinch={onPinch} onDoubleClick={onDoubleClick}>
      {img && <Image ref={imgRef} pixels={frame.pixels} style={{ position: 'absolute', left: img.left, top: img.top, width: img.width, height: img.height, pointerEvents: 'none' }} />}
      {v && <FlowLayer arcs={flowArcs} w={w} h={h} paused={moving || !!live.current} />}
      {labels.map((l) => (l.kind === 'name' ? (
        <Text key={`n${l.key}`} style={{ position: 'absolute', left: l.x, top: l.y, width: l.w, fontSize: 9, color: t.textDim, textAlign: 'center', whiteSpace: 'nowrap', pointerEvents: 'none' }}>{l.text}</Text>
      ) : (
        <View key={`p${l.key}`} style={{ position: 'absolute', left: l.x, top: l.y, height: 18, paddingHorizontal: 8, borderRadius: 9, justifyContent: 'center', pointerEvents: 'none',
          backgroundColor: l.kind === 'home' ? 'rgba(48,209,88,0.92)' : l.kind === 'sel' ? 'rgba(255,214,10,0.95)' : dark ? 'rgba(28,29,33,0.88)' : 'rgba(255,255,255,0.94)',
          border: l.kind === 'place' ? `1px solid ${t.separator}` : '0 solid transparent' }}>
          <Text style={{ fontSize: 10, fontWeight: 600, whiteSpace: 'nowrap', color: l.kind === 'home' ? '#fff' : l.kind === 'sel' ? '#1c1c1e' : t.text }}>{l.text}</Text>
        </View>
      )))}
      <View style={{ position: 'absolute', right: 8, top: 8, gap: 6 }}>
        <View style={btn} onMouseDown={(e) => e.stopPropagation()} onClick={() => zoomAt(1.6, w / 2, h / 2)}><Text style={btnText}>+</Text></View>
        <View style={btn} onMouseDown={(e) => e.stopPropagation()} onClick={() => zoomAt(1 / 1.6, w / 2, h / 2)}><Text style={btnText}>−</Text></View>
        <View style={btn} onMouseDown={(e) => e.stopPropagation()} onClick={() => setView(fit(allPts()))}>
          <View style={{ width: 12, height: 12, borderRadius: 2, border: `1.5px solid ${t.text}` }} />
        </View>
      </View>
      {!geoOn && (
        <View style={{ position: 'absolute', left: 10, bottom: 8, paddingHorizontal: 8, paddingVertical: 4, borderRadius: 6, backgroundColor: dark ? 'rgba(28,29,33,0.88)' : 'rgba(255,255,255,0.94)' }}>
          <Text style={{ fontSize: 10, color: t.textDim }}>{tr('monitor.net.geoOff')}</Text>
        </View>
      )}
    </View>
  );
}
