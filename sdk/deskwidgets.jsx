// deskwidgets.jsx — the desktop widgets of Lord Workspaces (0.8.8.10): a catalog, a grid per display, sizes and each
// widget's body. widgets.js draws the layer; widgetgrid.js has the grid's rules.
//
//   registerWidgetKind({ type, nameKey, descKey, sizes?, add?(x, y) })   an entry of the catalog. Built-in kinds have
//                          sizes (s 1×1, m 2×1, l 2×2, xl 4×2); an app's kind without sizes adds its own widget
//                          (the Notes app's sticky note, which it keeps itself)
//   widgetKinds()          the catalog, in order (a kind may say it isn't available here: Battery with no battery)
//   addDeskWidget(type, size, display)   a widget on the first free block of that display's grid, in the activity in
//                          view; null when there is no room
//   removeDeskWidget(id) / setDeskWidget(id, patch)
//   startDeskWidgets()     Lord Workspaces publishes them (the other themes never show them)
//
// A widget belongs to one activity (desktop) and one display (its connector): it shows only there, on a block of cells
// of that display's grid that no other widget may take. Kept in settings.deskWidgets as
// [{ id, type, size, desk, dk, col, row, opts }]. Moved by dragging it anywhere (a glass block shows where it will land:
// the nearest free block); resized, set up and removed from its right-click menu.
//
// The look follows TangoOS's widgets (the maintainer's own design): 22 px corners, the Crystal blur, 12 px headings,
// large figures. Nothing on them runs per second: clocks move once a minute; System, Network, Temperatures and Claude
// Code read only while shown.
import { useEffect, useRef, useState } from 'react';
import { View, Text, addListener, screen, onScreenChange, displays as screenDisplays, mainDisplay } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { settings } from './settings.js';
import { registerWidgetType, setWidgets } from './widgets.js';
import { useTheme } from './theme.js';
import { Layer } from './layers.js';
import { ContextMenu } from './contextmenu.jsx';
import { workAreaOf, activeDesktop, wm } from './windows.js';
import { t, formatTime, formatDate, monthName, weekdayName } from './i18n.js';
import { useCpuUsage, useSystemInfo, formatBytes } from './system.js';
import { memUsedKB } from './memfigure.js';
import { NowPlayingTile } from './media.js';
import { speak, stopSpeaking, speakAvailable } from './voice.js';
import { events, reminders, usePimQuery } from './calendar.js';
import { useFan } from './cooling.jsx';
import { battery } from './hardware.jsx';
import { clipboard, clipboardHistory } from './clipboard.js';
import { wifiBars } from './menubarmodel.js';
import { gridOf, rectOf, firstFree, nearestFree, cellAt, fits, occupancy, SIZE_ORDER } from './widgetgrid.js';

// ---- the catalog ----
const kinds = [];
export const widgetCatalog = createStore(0);
export function registerWidgetKind(k) {
  if (!k || !k.type || kinds.some((x) => x.type === k.type)) return;
  kinds.push(k);
  widgetCatalog.set(widgetCatalog.get() + 1);
}
export const widgetKinds = () => kinds.filter((k) => !k.available || k.available());

// ---- where they are ----
const MAX_WIDGETS = 48;
const all = () => (Array.isArray(settings.get().deskWidgets) ? settings.get().deskWidgets.filter((w) => w && BODIES[w.type]) : []);
const dkOf = (d) => (d && typeof d.connector === 'string' && d.connector ? d.connector : `#${d?.id ?? 0}`);
const shown = () => (screenDisplays() || []).filter((d) => !screen.mirror || d.main);
const gridFor = (d) => gridOf(workAreaOf(d));
const sizeOf = (w) => (SIZE_ORDER.includes(w.size) && KIND_SIZES[w.type]?.includes(w.size) ? w.size : KIND_SIZES[w.type]?.[0] || 's');
const deskOf = (w) => (w.desk == null ? activeDesktop.get() : w.desk);

// The blocks of one activity on one display: kept ones where they fit, the others (no block yet, a display gone, a
// smaller screen) on the nearest free block. Pure but for the reads of the settings and the displays.
function placed(desk, d, list = all()) {
  const g = gridFor(d), dk = dkOf(d), main = mainDisplay();
  const here = list.filter((w) => deskOf(w) === desk && (w.dk === dk || (!shown().some((x) => dkOf(x) === w.dk) && d.id === main?.id)));
  const out = [];
  for (const w of here) {
    const size = sizeOf(w);
    const occ = occupancy(out);
    const spot = w.dk === dk && fits(g, occ, w.col, w.row, size) ? { col: w.col, row: w.row } : nearestFree(g, out, size, w.col || 0, w.row || 0);
    if (spot) out.push({ ...w, size, ...spot });
  }
  return out;
}

export function addDeskWidget(type, size, display) {
  if (!BODIES[type]) return null;
  const d = display || mainDisplay();
  const desk = activeDesktop.get();
  const sz = KIND_SIZES[type].includes(size) ? size : KIND_SIZES[type][0];
  const spot = firstFree(gridFor(d), placed(desk, d), sz);
  if (!spot) return null;
  const id = `${type}-${Date.now().toString(36)}-${Math.floor(Math.random() * 1e4)}`;
  settings.set({ deskWidgets: [...all(), { id, type, size: sz, desk, dk: dkOf(d), ...spot, opts: {} }].slice(-MAX_WIDGETS) });
  return id;
}
export const removeDeskWidget = (id) => settings.set({ deskWidgets: all().filter((w) => w.id !== id) });
export const setDeskWidget = (id, patch) => settings.set({ deskWidgets: all().map((w) => (w.id === id ? { ...w, ...patch, opts: { ...(w.opts || {}), ...(patch.opts || {}) } } : w)) });
// a new size where it fits: its own block if there is room there, else the nearest free one; false with no room
export function resizeDeskWidget(id, size) {
  const w = all().find((x) => x.id === id);
  const d = shown().find((x) => dkOf(x) === w?.dk) || mainDisplay();
  if (!w || !d) return false;
  const g = gridFor(d), others = placed(deskOf(w), d).filter((x) => x.id !== id);
  const spot = fits(g, occupancy(others), w.col, w.row, size) ? { col: w.col, row: w.row } : nearestFree(g, others, size, w.col, w.row);
  if (!spot) return false;
  setDeskWidget(id, { size, ...spot });
  return true;
}
export const deskWidgetSize = (size) => { const r = rectOf({ x: 0, y: 0, cell: 170, gap: 16 }, 0, 0, size); return [r.w, r.h]; };

let started = false;
export function startDeskWidgets() {
  if (started) return;
  started = true;
  for (const type of Object.keys(BODIES)) registerWidgetType(`ws-${type}`, WidgetCard);
  let last = '';
  const publish = (force) => {
    const desk = activeDesktop.get();
    const key = `${desk}|${JSON.stringify(settings.get().deskWidgets || [])}|${shown().map((d) => `${dkOf(d)}:${d.x},${d.y},${d.w}x${d.h}`).join(';')}`;
    if (!force && key === last) return;
    last = key;
    const byType = {};
    for (const d of shown()) {
      const g = gridFor(d);
      for (const w of placed(desk, d)) {
        const r = rectOf(g, w.col, w.row, w.size);
        (byType[w.type] ||= []).push({ id: w.id, x: r.x, y: r.y, w: r.w, h: r.h, size: w.size, kind: w.type, opts: w.opts || {}, dk: dkOf(d) });
      }
    }
    for (const type of Object.keys(BODIES)) setWidgets(`ws-${type}`, byType[type] || []);
  };
  settings.subscribe(() => publish());
  activeDesktop.subscribe(() => publish());
  onScreenChange(() => setTimeout(() => publish(true), 0));
  publish(true);
}

// ---- the look (TangoOS's widget tokens) ----
function look(th) {
  const dark = th.mode !== 'light';
  return {
    dark,
    bg: dark ? 'rgba(36,36,42,0.55)' : 'rgba(250,250,252,0.62)',
    solid: dark ? 'rgba(36,36,42,0.92)' : 'rgba(250,250,252,0.94)',
    text: dark ? '#f4f4f6' : '#1c1c1f',
    dim: dark ? 'rgba(244,244,246,0.55)' : 'rgba(28,28,31,0.55)',
    faint: dark ? 'rgba(244,244,246,0.36)' : 'rgba(28,28,31,0.38)',
    track: dark ? 'rgba(255,255,255,0.10)' : 'rgba(0,0,0,0.08)',
    face: dark ? '#1e1e22' : '#ffffff',
    sep: dark ? 'rgba(255,255,255,0.16)' : 'rgba(0,0,0,0.16)',
    accent: th.accent || '#7C5CFF',
    ok: dark ? '#3dd68c' : '#1fa463', warn: dark ? '#f5b43c' : '#d98a00', crit: dark ? '#ff6b6e' : '#e5484d', info: dark ? '#5b9bff' : '#2f7cf6',
    blur: th.mat?.blur ? 30 : 0,
  };
}
const toneOf = (L, v) => (v > 0.85 ? L.crit : v > 0.6 ? L.warn : L.accent);

// ---- the card: drag to a free block, right-click for its menu ----
export const widgetDrag = createStore(null);   // { id, rect } while a widget is dragged: where it will land
function WidgetCard({ widget }) {
  const th = useTheme() || {};
  const L = look(th);
  const [live, setLive] = useState(null);
  const [menu, setMenu] = useState(null);
  const drag = useRef(null);
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      const dr = drag.current;
      if (!dr) return;
      if (!dr.moving && Math.abs(e.x - dr.mx) + Math.abs(e.y - dr.my) < 6) return;
      dr.moving = true;
      const x = dr.x + e.x - dr.mx, y = dr.y + e.y - dr.my;
      setLive({ x, y });
      const d = shown().find((s) => e.x >= s.x && e.x < s.x + s.w && e.y >= s.y && e.y < s.y + s.h) || shown().find((s) => dkOf(s) === widget.dk);
      if (!d) return;
      const g = gridFor(d), c = cellAt(g, x, y);
      const others = placed(activeDesktop.get(), d).filter((o) => o.id !== widget.id);
      const spot = nearestFree(g, others, widget.size, c.col, c.row);
      dr.target = spot ? { dk: dkOf(d), ...spot } : null;
      widgetDrag.set(spot ? { id: widget.id, rect: rectOf(g, spot.col, spot.row, widget.size) } : null);
    });
    const offUp = addListener('pointerup', () => {
      const dr = drag.current;
      drag.current = null;
      if (!dr) return;
      widgetDrag.set(null);
      setLive(null);
      if (dr.moving && dr.target) setDeskWidget(widget.id, { dk: dr.target.dk, col: dr.target.col, row: dr.target.row });
    });
    return () => { offMove(); offUp(); };
  }, [widget.id, widget.size, widget.dk]);
  const g = live ? { ...widget, ...live } : widget;
  const Body = BODIES[widget.kind];
  const sizes = KIND_SIZES[widget.kind] || ['s'];
  const items = [
    ...(sizes.length > 1 ? [{ label: t('shell.widgets.size'), submenu: sizes.map((sz) => ({ label: t(`shell.widgets.size.${sz}`), checked: sz === widget.size, onClick: () => resizeDeskWidget(widget.id, sz) })) }] : []),
    ...(KIND_MENU[widget.kind] ? KIND_MENU[widget.kind](widget) : []),
    '-',
    { label: t('shell.widgets.remove'), danger: true, onClick: () => removeDeskWidget(widget.id) },
  ];
  return (
    <>
      <View onMouseDown={(e) => { if (!e.button) drag.current = { mx: e.x, my: e.y, x: widget.x, y: widget.y, moving: false }; }}
        onContextMenu={(e) => setMenu({ x: e.x, y: e.y })}
        style={{ position: 'absolute', left: g.x, top: g.y, width: widget.w, height: widget.h, borderRadius: 22, overflow: 'hidden',
          backgroundColor: L.blur ? L.bg : L.solid, ...(L.blur ? { backdropFilter: `blur(${L.blur}px)` } : null),
          border: `0.5px solid ${L.dark ? 'rgba(255,255,255,0.2)' : 'rgba(0,0,0,0.12)'}`,
          boxShadow: live ? '0 24px 60px rgba(0,0,0,0.32)' : '0 8px 28px rgba(0,0,0,0.16)', transform: live ? 'scale(1.03)' : undefined }}>
        <View style={{ flex: 1, paddingTop: 13, paddingHorizontal: 15, paddingBottom: 14 }}>
          {Body ? <Body w={widget} L={L} /> : null}
        </View>
      </View>
      {menu ? <Layer name="popover"><ContextMenu x={menu.x} y={menu.y} items={items} onClose={() => setMenu(null)} /></Layer> : null}
    </>
  );
}
// where a dragged widget will land, in glass (drawn by the theme under the widgets)
export function WidgetDropTarget() {
  const d = useStore(widgetDrag);
  const th = useTheme() || {};
  if (!d) return null;
  const r = d.rect;
  return <View style={{ position: 'absolute', left: r.x, top: r.y, width: r.w, height: r.h, borderRadius: 22, pointerEvents: 'none',
    backgroundColor: th.mode === 'light' ? 'rgba(91,63,224,0.10)' : 'rgba(124,92,255,0.14)', border: `2px dashed ${th.accent || '#7C5CFF'}` }} />;
}

// ---- small parts ----
function Head({ L, title, right }) {
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', height: 16, gap: 5, marginBottom: 6 }}>
      <Text style={{ flex: 1, fontSize: 12, fontWeight: 600, color: L.dim, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{title}</Text>
      {right ? <Text style={{ fontSize: 12, fontWeight: 500, color: L.dim, whiteSpace: 'nowrap' }}>{right}</Text> : null}
    </View>
  );
}
const Big = ({ L, children, size = 30, color }) => <Text style={{ fontSize: size, fontWeight: 600, color: color || L.text, textAlign: 'start' }}>{children}</Text>;
const Sub = ({ L, children, center }) => <Text style={{ fontSize: 11.5, color: L.dim, textAlign: center ? 'center' : 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{children}</Text>;
function Bar({ L, value, color }) {
  return (
    <View style={{ height: 5, borderRadius: 3, backgroundColor: L.track, direction: 'ltr' }}>
      <View style={{ width: `${Math.round(Math.max(0.02, Math.min(1, value || 0)) * 100)}%`, height: 5, borderRadius: 3, backgroundColor: color || L.accent }} />
    </View>
  );
}
function Line({ L, label, value, color }) {
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, minHeight: 20 }}>
      <Text style={{ flex: 1, fontSize: 12, color: L.dim, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{label}</Text>
      <Text style={{ fontSize: 12.5, fontWeight: 600, color: color || L.text, whiteSpace: 'nowrap' }}>{value}</Text>
    </View>
  );
}
const Quiet = ({ L, text }) => <Text style={{ fontSize: 12, color: L.dim, textAlign: 'start', paddingTop: 4 }}>{text}</Text>;
// a ring of short ticks (no SVG in the renderer): the filled ones are the value
function TickRing({ L, size, value, color, ticks = 40, children }) {
  const c = size / 2, r = c - 4, n = Math.round(Math.max(0, Math.min(1, value || 0)) * ticks);
  return (
    <View style={{ width: size, height: size, alignItems: 'center', justifyContent: 'center', direction: 'ltr' }}>
      {Array.from({ length: ticks }, (_, i) => {
        const a = (i / ticks) * Math.PI * 2, x = c + Math.sin(a) * r, y = c - Math.cos(a) * r;
        return <View key={i} style={{ position: 'absolute', left: x - 1.5, top: y - 4, width: 3, height: 8, borderRadius: 1.5, backgroundColor: i < n ? color || L.accent : L.track, transform: `rotate(${(i / ticks) * 360}deg)` }} />;
      })}
      {children}
    </View>
  );
}
// a timer only while mounted
function useEvery(fn, ms, deps = []) {
  const [v, setV] = useState(() => { try { return fn(); } catch (e) { return null; } });
  useEffect(() => { const id = setInterval(() => { try { setV(fn()); } catch (e) { setV(null); } }, ms); return () => clearInterval(id); }, deps);
  return v;
}
function useMinute() {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    let iv = 0;
    const first = setTimeout(() => { setNow(Date.now()); iv = setInterval(() => setNow(Date.now()), 60000); }, 60000 - (Date.now() % 60000) + 50);
    return () => { clearTimeout(first); clearInterval(iv); };
  }, []);
  return now;
}

// ---- the clock: analog (TangoOS's face), digital, world cities ----
const localOffset = (ms) => -new Date(ms).getTimezoneOffset() * 60;
function zoneDate(zone, now) {
  if (!zone) return new Date(now);
  let z = null;
  try { z = lord.time.zoneOffset(zone, Math.floor(now / 1000)); } catch (e) { z = null; }
  return z ? new Date(now + (z.offset - localOffset(now)) * 1000) : null;
}
const cityName = (zone) => (String(zone || '').split('/').pop() || '').replace(/_/g, ' ');
const localZone = () => { try { return lord.time.zone(); } catch (e) { return ''; } };
const DEFAULT_CITIES = ['America/New_York', 'Europe/Madrid', 'Asia/Tokyo', 'Australia/Sydney', 'Europe/London', 'America/Mexico_City'];
function cities(n) {
  const own = localZone();
  const list = (Array.isArray(settings.get().clock?.cities) && settings.get().clock.cities.length ? settings.get().clock.cities : DEFAULT_CITIES).filter((z) => z && z !== own);
  return list.slice(0, n);
}
function AnalogFace({ L, size, date }) {
  const k = size / 112, c = size / 2;
  const h = date.getHours(), m = date.getMinutes();
  const hand = (deg, len, tail, w, color) => {
    const a = (deg * Math.PI) / 180, d = (len - tail) / 2;
    return <View style={{ position: 'absolute', left: c + Math.sin(a) * d - w / 2, top: c - Math.cos(a) * d - (len + tail) / 2, width: w, height: len + tail, borderRadius: w / 2, backgroundColor: color, transform: `rotate(${deg}deg)` }} />;
  };
  return (
    <View style={{ width: size, height: size, borderRadius: c, backgroundColor: L.face, border: `0.5px solid ${L.sep}`, direction: 'ltr' }}>
      {Array.from({ length: 60 }, (_, i) => {
        const major = i % 5 === 0, a = (i / 60) * Math.PI * 2;
        const len = (major ? 4.5 : 2) * k, w = (major ? 1.6 : 0.8) * k, r = c - 5 * k - len / 2;
        return <View key={i} style={{ position: 'absolute', left: c + Math.sin(a) * r - w / 2, top: c - Math.cos(a) * r - len / 2, width: w, height: len, borderRadius: w / 2, backgroundColor: major ? L.text : L.faint, transform: `rotate(${i * 6}deg)` }} />;
      })}
      {size >= 100 ? Array.from({ length: 12 }, (_, i) => {
        const n = i + 1, a = (n / 12) * Math.PI * 2, r = c - 19 * k, fs = 11.5 * k;
        return <Text key={n} style={{ position: 'absolute', left: c + Math.sin(a) * r - fs, top: c - Math.cos(a) * r - fs * 0.62, width: fs * 2, fontSize: fs, fontWeight: 600, color: L.text, textAlign: 'center' }}>{String(n)}</Text>;
      }) : null}
      {hand(((h % 12) + m / 60) * 30, 26 * k, 6 * k, 3.6 * k, L.text)}
      {hand(m * 6, 40 * k, 8 * k, 2.6 * k, L.text)}
      <View style={{ position: 'absolute', left: c - 3 * k, top: c - 3 * k, width: 6 * k, height: 6 * k, borderRadius: 3 * k, backgroundColor: '#F97316' }} />
    </View>
  );
}
const dayDiff = (a, b) => Math.round((new Date(a.getFullYear(), a.getMonth(), a.getDate()) - new Date(b.getFullYear(), b.getMonth(), b.getDate())) / 86400000);
function CityRow({ L, zone, now }) {
  const d = zoneDate(zone, now);
  if (!d) return null;
  const diff = dayDiff(d, new Date(now));
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, minHeight: 22 }}>
      <Text style={{ flex: 1, fontSize: 12.5, fontWeight: 600, color: L.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{cityName(zone)}</Text>
      {diff ? <Text style={{ fontSize: 10.5, color: L.dim }}>{t(diff > 0 ? 'shell.widgets.clock.tomorrow' : 'shell.widgets.clock.yesterday')}</Text> : null}
      <Text style={{ fontSize: 15, fontWeight: 600, color: L.text }}>{formatTime(d)}</Text>
    </View>
  );
}
function ClockBody({ w, L }) {
  const now = useMinute();
  const date = new Date(now);
  const digital = w.opts?.style === 'digital';
  const here = cityName(localZone()) || t('shell.widgets.clock.name');
  if (w.size === 's') {
    return (
      <View style={{ flex: 1, alignItems: 'center', gap: 4 }}>
        {digital ? <View style={{ flex: 1, justifyContent: 'center' }}><Big L={L} size={44}>{formatTime(date)}</Big></View> : <AnalogFace L={L} size={104} date={date} />}
        <Text style={{ fontSize: 12, fontWeight: 650, color: L.text, textAlign: 'center' }}>{here}</Text>
        <Sub L={L} center>{formatDate(date, 'short')}</Sub>
      </View>
    );
  }
  if (w.size === 'm') {
    return (
      <View style={{ flex: 1, flexDirection: 'row', gap: 16 }}>
        <View style={{ alignItems: 'center', justifyContent: 'center', gap: 4, width: 128 }}>
          {digital ? <Big L={L} size={40}>{formatTime(date)}</Big> : <AnalogFace L={L} size={116} date={date} />}
          <Sub L={L} center>{here}</Sub>
        </View>
        <View style={{ flex: 1, justifyContent: 'center', gap: 4, minWidth: 0 }}>
          {cities(4).map((z) => <CityRow key={z} L={L} zone={z} now={now} />)}
        </View>
      </View>
    );
  }
  if (w.size === 'l') {
    return (
      <View style={{ flex: 1, gap: 10 }}>
        <View style={{ alignItems: 'center', gap: 4 }}>
          {digital ? <Big L={L} size={64}>{formatTime(date)}</Big> : <AnalogFace L={L} size={170} date={date} />}
          <Text style={{ fontSize: 12.5, fontWeight: 650, color: L.text }}>{here} · {formatDate(date, 'short')}</Text>
        </View>
        <View style={{ gap: 2 }}>{cities(4).map((z) => <CityRow key={z} L={L} zone={z} now={now} />)}</View>
      </View>
    );
  }
  // xl: four cities, a face each
  const list = [localZone(), ...cities(3)];
  return (
    <View style={{ flex: 1, flexDirection: 'row', justifyContent: 'space-around', alignItems: 'center' }}>
      {list.map((z) => {
        const d = zoneDate(z, now) || date;
        return (
          <View key={z || 'here'} style={{ alignItems: 'center', gap: 6 }}>
            {digital ? <Big L={L} size={40}>{formatTime(d)}</Big> : <AnalogFace L={L} size={140} date={d} />}
            <Text style={{ fontSize: 13, fontWeight: 650, color: L.text }}>{cityName(z) || here}</Text>
            <Sub L={L} center>{formatTime(d)}</Sub>
          </View>
        );
      })}
    </View>
  );
}

// ---- system ----
function SystemBody({ w, L }) {
  const cpu = useCpuUsage(1500);
  const info = useSystemInfo(5000) || {};
  const used = memUsedKB(info), total = info.memTotalKB || 0, mem = total ? used / total : 0;
  const ring = (label, v, size) => (
    <View style={{ alignItems: 'center', gap: 4 }}>
      <TickRing L={L} size={size} value={v} color={toneOf(L, v)}>
        <Text style={{ fontSize: size > 80 ? 22 : 17, fontWeight: 650, color: L.text }}>{`${Math.round(v * 100)}%`}</Text>
      </TickRing>
      <Sub L={L} center>{label}</Sub>
    </View>
  );
  if (w.size === 's') {
    return (
      <View style={{ flex: 1 }}>
        <Head L={L} title="CPU" />
        <View style={{ alignItems: 'center' }}>{ring(`${t('shell.cc.memory')} ${Math.round(mem * 100)}%`, cpu, 86)}</View>
      </View>
    );
  }
  return (
    <View style={{ flex: 1 }}>
      <Head L={L} title={t('shell.widgets.system.name')} right={total ? `${formatBytes(used * 1024)} / ${formatBytes(total * 1024)}` : ''} />
      <View style={{ flex: 1, flexDirection: 'row', justifyContent: 'space-around', alignItems: 'center' }}>
        {ring('CPU', cpu, w.size === 'l' ? 120 : 92)}
        {ring(t('shell.cc.memory'), mem, w.size === 'l' ? 120 : 92)}
      </View>
      {w.size === 'l' ? <View style={{ gap: 6 }}><Line L={L} label={t('shell.widgets.disks.system')} value={info.diskTotal ? t('shell.widgets.disks.free', { free: formatBytes(info.diskFree) }) : ''} /><Bar L={L} value={info.diskTotal ? 1 - info.diskFree / info.diskTotal : 0} /></View> : null}
    </View>
  );
}

// ---- calendar ----
const ymd = (d) => `${d.getFullYear()}-${d.getMonth()}-${d.getDate()}`;
function useEventsAhead(now, days) {
  return usePimQuery(async () => {
    try {
      const from = new Date(now); from.setHours(0, 0, 0, 0);
      const occ = await events.range(from.getTime(), from.getTime() + days * 86400000);
      return (occ || []).filter((o) => (o.end || o.start) >= now - 60000).sort((a, b) => a.start - b.start);
    } catch (e) { return []; }
  }, [Math.floor(now / 3600000), days]) || [];
}
function MonthGrid({ L, now, busy, cell }) {
  const d0 = new Date(now), y = d0.getFullYear(), m = d0.getMonth();
  const first = new Date(y, m, 1), days = new Date(y, m + 1, 0).getDate();
  const cells = [];
  for (let i = 0; i < first.getDay(); i++) cells.push(null);
  for (let d = 1; d <= days; d++) cells.push(d);
  return (
    <View style={{ direction: 'ltr' }}>
      <View style={{ flexDirection: 'row' }}>{[0, 1, 2, 3, 4, 5, 6].map((wd) => <Text key={wd} style={{ width: cell, fontSize: 10, fontWeight: 600, textAlign: 'center', color: L.faint }}>{weekdayName(wd).slice(0, 1).toUpperCase()}</Text>)}</View>
      <View style={{ flexDirection: 'row', flexWrap: 'wrap' }}>
        {cells.map((d, i) => {
          const today = d === d0.getDate(), has = d && busy.has(ymd(new Date(y, m, d)));
          return (
            <View key={i} style={{ width: cell, height: cell - 4, alignItems: 'center', justifyContent: 'center' }}>
              {d ? <View style={{ width: cell - 8, height: cell - 8, borderRadius: (cell - 8) / 2, alignItems: 'center', justifyContent: 'center', backgroundColor: today ? '#E5484D' : 'transparent' }}>
                <Text style={{ fontSize: 11.5, fontWeight: today ? 700 : 500, color: today ? '#ffffff' : L.text }}>{String(d)}</Text>
                {has && !today ? <View style={{ position: 'absolute', bottom: 0, width: 4, height: 4, borderRadius: 2, backgroundColor: L.accent }} /> : null}
              </View> : null}
            </View>
          );
        })}
      </View>
    </View>
  );
}
function EventRow({ L, e }) {
  return (
    <View style={{ flexDirection: 'row', gap: 7, alignItems: 'center', minHeight: 30 }}>
      <View style={{ width: 3, alignSelf: 'stretch', borderRadius: 2, backgroundColor: e.color || L.accent }} />
      <View style={{ flex: 1, minWidth: 0 }}>
        <Text style={{ fontSize: 12.5, fontWeight: 600, color: L.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{e.title}</Text>
        <Sub L={L}>{e.allDay ? t('shell.widgets.calendar.allDay') : `${formatTime(new Date(e.start))} – ${formatTime(new Date(e.end || e.start))}`}</Sub>
      </View>
    </View>
  );
}
function CalendarBody({ w, L }) {
  const now = useMinute();
  const d = new Date(now);
  const ev = useEventsAhead(now, w.size === 'l' ? 31 : 2);
  const busy = new Set(ev.map((o) => ymd(new Date(o.start))));
  const dayBlock = (
    <View style={{ gap: 0 }}>
      <Text style={{ fontSize: 12, fontWeight: 700, color: '#E5484D', textAlign: 'start' }}>{weekdayName(d.getDay(), false).toUpperCase()}</Text>
      <Big L={L} size={44}>{String(d.getDate())}</Big>
    </View>
  );
  if (w.size === 's') {
    return (
      <View style={{ flex: 1, gap: 4 }}>
        {dayBlock}
        {ev[0] ? <EventRow L={L} e={ev[0]} /> : <Quiet L={L} text={t('shell.widgets.calendar.noEvents')} />}
      </View>
    );
  }
  if (w.size === 'm') {
    return (
      <View style={{ flex: 1, flexDirection: 'row', gap: 14 }}>
        <View style={{ width: 96 }}>{dayBlock}<Sub L={L}>{monthName(d.getMonth())}</Sub></View>
        <View style={{ flex: 1, gap: 2, minWidth: 0 }}>{ev.length ? ev.slice(0, 3).map((e) => <EventRow key={`${e.uid}${e.occ || ''}`} L={L} e={e} />) : <Quiet L={L} text={t('shell.widgets.calendar.noEvents')} />}</View>
      </View>
    );
  }
  return (
    <View style={{ flex: 1, gap: 6 }}>
      <Head L={L} title={`${monthName(d.getMonth())} ${d.getFullYear()}`} />
      <MonthGrid L={L} now={now} busy={busy} cell={44} />
      {ev.slice(0, 2).map((e) => <EventRow key={`${e.uid}${e.occ || ''}`} L={L} e={e} />)}
    </View>
  );
}

// ---- now playing, read aloud ----
function NowPlayingBody({ L }) {
  return (
    <View style={{ flex: 1 }}>
      <Head L={L} title={t('shell.widgets.nowplaying.name')} />
      <NowPlayingTile style={{ backgroundColor: 'transparent', padding: 0 }} />
    </View>
  );
}
function Pill({ L, label, onClick, primary, disabled }) {
  return (
    <View onClick={disabled ? undefined : onClick} style={{ height: 30, paddingHorizontal: 14, borderRadius: 15, justifyContent: 'center', opacity: disabled ? 0.45 : 1, backgroundColor: primary ? L.accent : L.track }}>
      <Text style={{ fontSize: 13, fontWeight: 600, color: primary ? '#ffffff' : L.text }}>{label}</Text>
    </View>
  );
}
function ReadAloudBody({ w, L }) {
  const [note, setNote] = useState('');
  const can = speakAvailable();
  const read = () => {
    let text = '';
    try { text = lord.clipboard.get() || ''; } catch (e) { text = ''; }
    if (!text.trim()) { setNote(t('shell.widgets.readaloud.empty')); return; }
    setNote('');
    speak(text.slice(0, 20000), undefined, t('shell.widgets.readaloud.name'));
  };
  return (
    <View style={{ flex: 1, gap: 8 }}>
      <Head L={L} title={t('shell.widgets.readaloud.name')} />
      <Text style={{ fontSize: 12, color: L.dim, textAlign: 'start' }}>{can ? note || t('shell.widgets.readaloud.hint') : t('shell.widgets.readaloud.noVoice')}</Text>
      <View style={{ flex: 1 }} />
      <View style={{ flexDirection: w.size === 's' ? 'column' : 'row', gap: 8 }}>
        <Pill L={L} primary disabled={!can} label={t('shell.widgets.readaloud.read')} onClick={read} />
        <Pill L={L} disabled={!can} label={t('shell.widgets.readaloud.stop')} onClick={() => stopSpeaking()} />
      </View>
    </View>
  );
}

// ---- network ----
const rate = (n) => `${formatBytes(Math.max(0, n))}/s`;
function NetworkBody({ w, L }) {
  const prev = useRef(null);
  const hist = useRef([]);
  const speed = useEvery(() => {
    const s = lord.monitor?.netTotals ? lord.monitor.netTotals() : null;
    const p = prev.current;
    prev.current = s;
    if (!s || !p || s.t <= p.t) return null;
    const dt = (s.t - p.t) / 1000, v = { rx: (s.rx - p.rx) / dt, tx: (s.tx - p.tx) / dt };
    hist.current = [...hist.current.slice(-29), v];
    return v;
  }, 2000);
  const ifs = useEvery(() => (lord.net?.interfaces ? lord.net.interfaces() : []), 8000) || [];
  const up = ifs.filter((i) => i.up && i.carrier !== false && i.type !== 'loopback');
  const main = up.find((i) => i.type === 'wifi' || i.type === 'ethernet') || up[0];
  const ip = main?.addresses?.find((a) => a.scope === 'global' && a.family === 4)?.address || '';
  const head = !main ? t('shell.widgets.network.offline') : main.type === 'wifi' ? `${t('shell.widgets.network.wifi')} ${'▮'.repeat(wifiBars(main.signal))}${'▯'.repeat(4 - wifiBars(main.signal))}` : `${main.name}`;
  const rates = (
    <View style={{ gap: 6 }}>
      <View><Sub L={L}>↓ {t('shell.widgets.network.down')}</Sub><Big L={L} size={20} color={L.accent}>{speed ? rate(speed.rx) : '…'}</Big></View>
      <View><Sub L={L}>↑ {t('shell.widgets.network.up')}</Sub><Big L={L} size={20} color={L.ok}>{speed ? rate(speed.tx) : '…'}</Big></View>
    </View>
  );
  if (w.size === 's') return <View style={{ flex: 1 }}><Head L={L} title={head} />{rates}</View>;
  const h = hist.current, top = Math.max(1, ...h.map((x) => Math.max(x.rx, x.tx)));
  return (
    <View style={{ flex: 1 }}>
      <Head L={L} title={head} right={ip} />
      <View style={{ flex: 1, flexDirection: 'row', gap: 14 }}>
        <View style={{ width: 112 }}>{rates}</View>
        <View style={{ flex: 1, flexDirection: 'row', alignItems: 'flex-end', gap: 2, direction: 'ltr' }}>
          {Array.from({ length: 30 }, (_, i) => {
            const x = h[i - (30 - h.length)];
            return (
              <View key={i} style={{ flex: 1, height: '100%', justifyContent: 'flex-end', gap: 1 }}>
                <View style={{ height: x ? Math.max(1, (x.rx / top) * 70) : 1, borderRadius: 1, backgroundColor: L.accent, opacity: 0.85 }} />
                <View style={{ height: x ? Math.max(1, (x.tx / top) * 30) : 1, borderRadius: 1, backgroundColor: L.ok, opacity: 0.8 }} />
              </View>
            );
          })}
        </View>
      </View>
    </View>
  );
}

// ---- disks ----
function DisksBody({ w, L }) {
  const list = useEvery(() => {
    const out = [];
    const info = lord.system.info();
    if (info && info.diskTotal) out.push({ name: t('shell.widgets.disks.system'), total: info.diskTotal, free: info.diskFree });
    try { for (const v of (lord.fs.volumes ? lord.fs.volumes() : []).slice(0, 4)) if (v.total) out.push({ name: v.name || v.path, total: v.total, free: v.free }); } catch (e) { /* none */ }
    return out;
  }, 30000) || [];
  const first = list[0];
  if (w.size === 's') {
    const v = first ? 1 - first.free / first.total : 0;
    return (
      <View style={{ flex: 1 }}>
        <Head L={L} title={t('shell.widgets.disks.name')} />
        <View style={{ alignItems: 'center', gap: 4 }}>
          <TickRing L={L} size={86} value={v} color={toneOf(L, v)}><Text style={{ fontSize: 17, fontWeight: 650, color: L.text }}>{`${Math.round(v * 100)}%`}</Text></TickRing>
          <Sub L={L} center>{first ? t('shell.widgets.disks.free', { free: formatBytes(first.free) }) : '…'}</Sub>
        </View>
      </View>
    );
  }
  return (
    <View style={{ flex: 1, gap: 8 }}>
      <Head L={L} title={t('shell.widgets.disks.name')} />
      {list.slice(0, w.size === 'l' ? 5 : 2).map((d) => {
        const v = d.total ? 1 - d.free / d.total : 0;
        return (
          <View key={d.name} style={{ gap: 4 }}>
            <Line L={L} label={d.name} value={t('shell.widgets.disks.free', { free: formatBytes(d.free) })} />
            <Bar L={L} value={v} color={toneOf(L, v)} />
          </View>
        );
      })}
    </View>
  );
}

// ---- temperatures ----
const heat = (L, c) => (c >= 75 ? L.crit : c >= 65 ? L.warn : L.ok);
function TempsBody({ w, L }) {
  const f = useFan();
  if (f === null) return <View style={{ flex: 1 }}><Head L={L} title={t('shell.widgets.temps.name')} /><Quiet L={L} text={t('shell.widgets.temps.none')} /></View>;
  const fans = f ? [...(f.fans || []), ...(f.firmwareFans || []).map((x, i) => ({ id: `fw${i}`, rpm: x.rpm }))].filter((x) => x.rpm >= 0) : [];
  const gpu = f ? (f.gpuFans || []).find((g) => g.temp != null) : null;
  const cpuT = f?.temp != null ? Math.round(f.temp) : null;
  if (w.size === 's') {
    return (
      <View style={{ flex: 1 }}>
        <Head L={L} title="CPU" />
        <Big L={L} size={40} color={cpuT != null ? heat(L, cpuT) : L.text}>{cpuT != null ? `${cpuT}°` : '…'}</Big>
        <View style={{ flex: 1 }} />
        {gpu ? <Line L={L} label="GPU" value={`${Math.round(gpu.temp)}°`} color={heat(L, gpu.temp)} /> : null}
        {fans[0] ? <Line L={L} label={t('shell.widgets.temps.fan', { n: 1 })} value={`${fans[0].rpm} rpm`} /> : null}
      </View>
    );
  }
  return (
    <View style={{ flex: 1 }}>
      <Head L={L} title={t('shell.widgets.temps.name')} />
      <View style={{ flexDirection: 'row', gap: 18 }}>
        <View><Sub L={L}>CPU</Sub><Big L={L} size={30} color={cpuT != null ? heat(L, cpuT) : L.text}>{cpuT != null ? `${cpuT}°` : '…'}</Big></View>
        {gpu ? <View><Sub L={L}>GPU</Sub><Big L={L} size={30} color={heat(L, gpu.temp)}>{`${Math.round(gpu.temp)}°`}</Big></View> : null}
      </View>
      <View style={{ gap: 0, marginTop: 6 }}>{fans.slice(0, w.size === 'l' ? 6 : 2).map((x, i) => <Line key={x.id || i} L={L} label={t('shell.widgets.temps.fan', { n: i + 1 })} value={`${x.rpm} rpm`} />)}</View>
    </View>
  );
}

// ---- battery ----
function BatteryBody({ w, L }) {
  const b = useStore(battery);
  if (!b || !b.hasBattery) return <View style={{ flex: 1 }}><Head L={L} title={t('shell.widgets.battery.name')} /><Quiet L={L} text={t('shell.widgets.battery.none')} /></View>;
  const pct = b.percent >= 0 ? b.percent : 0;
  const state = b.charging ? t('shell.widgets.battery.charging') : b.onAC ? t('shell.widgets.battery.plugged')
    : b.minutesLeft > 0 ? t('shell.widgets.battery.left', { h: Math.floor(b.minutesLeft / 60), m: b.minutesLeft % 60 }) : '';
  const color = pct <= 20 && !b.charging ? L.crit : L.ok;
  return (
    <View style={{ flex: 1, flexDirection: w.size === 's' ? 'column' : 'row', alignItems: 'center', gap: 12 }}>
      <TickRing L={L} size={w.size === 's' ? 96 : 110} value={pct / 100} color={color}><Text style={{ fontSize: 20, fontWeight: 650, color: L.text }}>{`${pct}%`}</Text></TickRing>
      <View style={{ gap: 2, alignItems: w.size === 's' ? 'center' : 'flex-start' }}><Sub L={L} center={w.size === 's'}>{state}</Sub></View>
    </View>
  );
}

// ---- reminders ----
function RemindersBody({ w, L }) {
  const items = usePimQuery(async () => {
    const list = await reminders.items();
    return (list || []).filter((it) => !it.done).sort((a, b) => (a.due ?? Infinity) - (b.due ?? Infinity));
  }, []) || [];
  const n = w.size === 's' ? 2 : w.size === 'm' ? 3 : 8;
  return (
    <View style={{ flex: 1 }}>
      <Head L={L} title={t('shell.widgets.reminders.name')} right={items.length ? String(items.length) : ''} />
      {!items.length ? <Quiet L={L} text={t('shell.widgets.reminders.none')} /> : items.slice(0, n).map((it) => (
        <View key={it.uid} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, minHeight: 26 }}>
          <View onClick={() => reminders.toggle(it)} onMouseDown={(e) => e.stopPropagation?.()} title={t('shell.widgets.reminders.done')}
            style={{ width: 16, height: 16, borderRadius: 8, border: `1.5px solid ${it.color || L.accent}` }} />
          <Text style={{ flex: 1, fontSize: 12.5, color: L.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{it.title}</Text>
          {it.due && w.size !== 's' ? <Text style={{ fontSize: 11, color: it.due < Date.now() ? L.crit : L.dim }}>{formatDate(new Date(it.due))}</Text> : null}
        </View>
      ))}
    </View>
  );
}

// ---- clipboard ----
function ClipboardBody({ w, L }) {
  const hist = useStore(clipboardHistory) || [];
  const [copied, setCopied] = useState(null);
  const list = hist.filter((x) => x.text != null || x.image).slice(0, w.size === 'l' ? 8 : 3);
  return (
    <View style={{ flex: 1, gap: 3 }}>
      <Head L={L} title={t('shell.widgets.clipboard.name')} />
      {!list.length ? <Quiet L={L} text={t('shell.widgets.clipboard.none')} /> : list.map((x) => (
        <View key={x.id} onMouseDown={(e) => e.stopPropagation?.()} onClick={() => { clipboard.use(x); setCopied(x.id); setTimeout(() => setCopied(null), 1200); }} title={t('shell.widgets.clipboard.copy')}
          style={{ height: 30, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 10, borderRadius: 9, backgroundColor: copied === x.id ? L.accent : L.track }}>
          <Text style={{ flex: 1, fontSize: 12.5, color: copied === x.id ? '#ffffff' : L.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>
            {copied === x.id ? t('shell.widgets.clipboard.copied') : x.image ? t('shell.widgets.clipboard.image') : String(x.text).replace(/\s+/g, ' ').slice(0, 90)}
          </Text>
        </View>
      ))}
    </View>
  );
}

// ---- Claude Code ----
const readJson = (path) => { try { const v = JSON.parse(lord.fs.read(path)); return v && typeof v === 'object' ? v : null; } catch (e) { return null; } };
function claudeSessions() {
  const home = lord.fs.home();
  let names = [];
  try { names = lord.fs.list(`${home}/.claude/sessions`).map((e) => e.name).filter((n) => /^\d{1,9}\.json$/.test(n)).slice(0, 16); } catch (e) { return []; }
  return names.map((n) => {
    const d = readJson(`${home}/.claude/sessions/${n}`);
    const id = d && /^[A-Za-z0-9-]{1,64}$/.test(String(d.sessionId || '')) ? d.sessionId : null;
    if (!id) return null;
    const st = readJson(`${home}/.cache/lordos/claude/${id}.json`);
    const ctx = st && st.session_id === id ? Math.round(+st.context_window?.used_percentage) : NaN;
    const dir = st && typeof st.workspace?.current_dir === 'string' ? st.workspace.current_dir : typeof d.cwd === 'string' ? d.cwd : '';
    return { id, state: d.status === 'busy' ? 'working' : d.status === 'idle' ? 'waiting' : '', ctx: Number.isFinite(ctx) ? ctx : null,
      model: typeof st?.model?.display_name === 'string' ? st.model.display_name.slice(0, 24) : '', dir: dir.split('/').filter(Boolean).pop() || '' };
  }).filter(Boolean);
}
function ClaudeBody({ w, L }) {
  const list = useEvery(claudeSessions, 3000) || [];
  const dot = (s) => (s === 'working' ? L.warn : s === 'waiting' ? L.ok : L.faint);
  const stateText = (s) => t(s === 'working' ? 'shell.widgets.claude.working' : s === 'waiting' ? 'shell.widgets.claude.waiting' : 'shell.widgets.claude.idle');
  if (!list.length) return <View style={{ flex: 1 }}><Head L={L} title="Claude Code" /><Quiet L={L} text={t('shell.widgets.claude.none')} /></View>;
  if (w.size === 's') {
    const c = list[0];
    return (
      <View style={{ flex: 1, alignItems: 'center' }}>
        <Head L={L} title="Claude Code" right={list.length > 1 ? `+${list.length - 1}` : ''} />
        <TickRing L={L} size={80} value={(c.ctx || 0) / 100} color={toneOf(L, (c.ctx || 0) / 100)}><Text style={{ fontSize: 16, fontWeight: 650, color: L.text }}>{c.ctx != null ? `${c.ctx}%` : '–'}</Text></TickRing>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 5, marginTop: 4 }}><View style={{ width: 7, height: 7, borderRadius: 4, backgroundColor: dot(c.state) }} /><Sub L={L}>{stateText(c.state)}</Sub></View>
      </View>
    );
  }
  return (
    <View style={{ flex: 1, gap: 6 }}>
      <Head L={L} title="Claude Code" />
      {list.slice(0, w.size === 'l' ? 5 : 2).map((c) => (
        <View key={c.id} style={{ gap: 3 }}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
            <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: dot(c.state) }} />
            <Text style={{ flex: 1, fontSize: 12.5, fontWeight: 600, color: L.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{c.dir || c.model || 'Claude'}</Text>
            <Text style={{ fontSize: 11.5, color: L.dim }}>{stateText(c.state)}</Text>
          </View>
          {c.ctx != null ? <Bar L={L} value={c.ctx / 100} color={toneOf(L, c.ctx / 100)} /> : null}
        </View>
      ))}
    </View>
  );
}

// ---- the kinds: their sizes (the first is the default), bodies and menu entries ----
const BODIES = { clock: ClockBody, calendar: CalendarBody, system: SystemBody, network: NetworkBody, disks: DisksBody, temps: TempsBody,
  battery: BatteryBody, reminders: RemindersBody, clipboard: ClipboardBody, claude: ClaudeBody, nowplaying: NowPlayingBody, readaloud: ReadAloudBody };
const KIND_SIZES = { clock: ['s', 'm', 'l', 'xl'], calendar: ['s', 'm', 'l'], system: ['s', 'm', 'l'], network: ['s', 'm'], disks: ['s', 'm', 'l'],
  temps: ['s', 'm', 'l'], battery: ['s', 'm'], reminders: ['s', 'm', 'l'], clipboard: ['m', 'l'], claude: ['s', 'm', 'l'], nowplaying: ['m'], readaloud: ['s', 'm'] };
export const kindSizes = (type) => KIND_SIZES[type] || null;
const KIND_MENU = {
  clock: (w) => [
    { label: t('shell.widgets.clock.analog'), checked: w.opts?.style !== 'digital', onClick: () => setDeskWidget(w.id, { opts: { style: 'analog' } }) },
    { label: t('shell.widgets.clock.digital'), checked: w.opts?.style === 'digital', onClick: () => setDeskWidget(w.id, { opts: { style: 'digital' } }) },
    { label: t('shell.widgets.clock.cities'), onClick: () => wm.open('clock') },
  ],
};
for (const type of Object.keys(BODIES)) {
  registerWidgetKind({ type, nameKey: `shell.widgets.${type}.name`, descKey: `shell.widgets.${type}.desc`, sizes: KIND_SIZES[type],
    ...(type === 'battery' ? { available: () => !!battery.get()?.hasBattery } : null) });
}
