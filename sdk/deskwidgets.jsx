// deskwidgets.jsx — the desktop widgets of Workspaces (0.8.8.10): a catalog, a grid per display, sizes and each
// widget's body. widgets.js draws the layer; widgetgrid.js has the grid's rules.
//
//   registerWidgetKind({ type, nameKey, descKey, sizes?, add?(x, y) })   an entry of the catalog. Built-in kinds have
//                          sizes (s 1×1, m 2×1, l 2×2, w 4×1, xl 4×2); an app's kind without sizes adds its own widget
//                          (the Notes app's sticky note, which it keeps itself). Such a kind may also give a Chooser
//                          component, shown in the catalog after its card is clicked (sdk/widgetpicker.jsx): Notes
//                          offers a new note or one already made there
//   widgetKinds()          the catalog, in order (a kind may say it isn't available here: Battery with no battery)
//   addDeskWidget(type, size, display)   a widget on the first free block of that display's grid, in the activity in
//                          view; null when there is no room
//   removeDeskWidget(id) / setDeskWidget(id, patch)
//   startDeskWidgets()     Workspaces publishes them (the other themes never show them)
//
// A widget belongs to one activity (desktop) and one display (its connector): it shows only there, on a block of cells
// of that display's grid that no other widget may take. Kept in settings.deskWidgets as
// [{ id, type, size, desk, dk, col, row, opts }]. Moved by dragging it anywhere (a glass block shows where it will land:
// the nearest free block); resized, set up and removed from its right-click menu. Any widget can be pinned instead
// (opts.pin, widgetgrid.js PINS): 'bottom-center' keeps it centred along the bottom of its display, off the grid;
// dragging it puts it back on the grid. opts.frame 'none' draws it without its card (the sound wave).
//
// The look follows TangoOS's widgets (the maintainer's own design): 22 px corners, the Crystal blur, 12 px headings,
// large figures. Nothing on them runs per second but the analog clock's second hand when the person picks it (its own
// node, the only one that changes each second); otherwise clocks move once a minute. System, Network, Temperatures and
// Claude Code read only while shown.
import { memo, useEffect, useRef, useState } from 'react';
import { View, Text, SoundWave, addListener, screen, onScreenChange, displays as screenDisplays, mainDisplay, displayAt } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { settings } from './settings.js';
import { registerWidgetType, setWidgets } from './widgets.js';
import { useTheme } from './theme.js';
import { Layer } from './layers.js';
import { SearchField } from './controls.jsx';
import { ScrollView } from './components.jsx';
import { ContextMenu } from './contextmenu.jsx';
import { workAreaOf, activeDesktop, wm } from './windows.js';
import { t, formatTime, formatDate, monthName, weekdayName } from './i18n.js';
import { formatBytes } from './system.js';
import { useFeed, WidgetIdContext, setWidgetRects } from './widgetfeed.js';
import { memUsedKB } from './memfigure.js';
import { NowPlayingTile } from './media.js';
import { speak, stopSpeaking, speakAvailable } from './voice.js';
import { events, reminders, usePimQuery } from './calendar.js';
import { useFan } from './cooling.jsx';
import { battery, bluetooth } from './hardware.jsx';
import { fractionOf, toneOf as deviceTone } from './devicesmodel.js';
import { useDevices, deviceShort, deviceValue, deviceVia } from './devices.js';
import { clipboard, clipboardHistory } from './clipboard.js';
import { wifiBars, monthWeeks, weekStartFor } from './menubarmodel.js';
import { gridOf, rectOf, firstFree, nearestFree, cellAt, fits, occupancy, SIZE_ORDER, PINS, pinnedRect, pinBlocks } from './widgetgrid.js';
import { useSound, useSpectrumSource } from './sound.js';
import { sourceOf, appsPlaying, colorsOf, gainOf, PALETTE_ORDER } from './soundwavemodel.js';
import { kindOf, unitFor, temp as tempIn, skyOf, cityOk, widgetCity, placeOf } from './weathermodel.js';
import { vpnState, rates as vpnRates, targetOf, eventOf } from './vpnmodel.js';
import { configDirs, accountOf, leftText, alertOf, summarize } from './claudewidgetmodel.js';
import { admin } from './admin.js';
import { clockTimer, timerStart, timerPause, timerResume, timerReset, timerLeft, stopwatch, swStart, swStop, swReset, swElapsed } from './alarms.js';
import { timerLayout, idleSeconds, PRESETS, PRESET_DROP } from './timerwidgetmodel.js';
import { recentCached as fsRecent, recentFiles, basename } from './fs.js';
import { openFileWithApps } from './sysopen.js';
import { lordosUpdate, restartNeeded } from './updates.js';
import { storeUpdates } from './appcontrol.js';
import { useLauncherApps, launchApp } from './apps.js';
import { VideoBody, videoSetup } from './videowidget.jsx';
import { dock, dockIdOf } from './dock.js';
import { locale } from './i18n.js';
import { regionCountry } from './localename.js';

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
// a size the kind no longer has: its nearest one (the clock's 4×2 row of cities became the 4×1 wide one), else its first
const RENAMED = { xl: 'w' };
const sizeOf = (w) => {
  const ok = KIND_SIZES[w.type] || [];
  if (SIZE_ORDER.includes(w.size) && ok.includes(w.size)) return w.size;
  return ok.includes(RENAMED[w.size]) ? RENAMED[w.size] : ok[0] || 's';
};
const deskOf = (w) => (w.desk == null ? activeDesktop.get() : w.desk);
const pinOf = (w) => (PINS.includes(w?.opts?.pin) ? w.opts.pin : null);
// the pinned widgets of one activity on one display (its own, or the main display's when its display is gone)
function pinnedOn(desk, d, list = all()) {
  const dk = dkOf(d), main = mainDisplay();
  return list.filter((w) => deskOf(w) === desk && pinOf(w) && (w.dk === dk || (!shown().some((x) => dkOf(x) === w.dk) && d.id === main?.id)))
    .map((w) => ({ ...w, size: sizeOf(w) }));
}

// The blocks of one activity on one display: kept ones where they fit, the others (no block yet, a display gone, a
// smaller screen) on the nearest free block. Pure but for the reads of the settings and the displays.
function placed(desk, d, list = all()) {
  const g = gridFor(d), dk = dkOf(d), main = mainDisplay();
  const here = list.filter((w) => deskOf(w) === desk && !pinOf(w) && (w.dk === dk || (!shown().some((x) => dkOf(x) === w.dk) && d.id === main?.id)));
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
  const opts = { ...(NEW_OPTS[type] || {}) };
  // a new weather widget starts on the last city chosen and keeps it (#193: each widget has its own)
  if (type === 'weather') opts.city = cityOk(settings.get().weather?.city);
  // a pinned one needs no free block; it still gets one to go back to when it is dragged
  // never under a pinned one either (the sound wave along the bottom): its cells count as taken
  const spot = firstFree(gridFor(d), [...placed(desk, d), ...pinBlocks(gridFor(d), workAreaOf(d), pinnedOn(desk, d))], sz) || (opts.pin ? { col: 0, row: 0 } : null);
  if (!spot) return null;
  const id = `${type}-${Date.now().toString(36)}-${Math.floor(Math.random() * 1e4)}`;
  settings.set({ deskWidgets: [...all(), { id, type, size: sz, desk, dk: dkOf(d), ...spot, opts }].slice(-MAX_WIDGETS) });
  return id;
}
export const removeDeskWidget = (id) => settings.set({ deskWidgets: all().filter((w) => w.id !== id) });
export const setDeskWidget = (id, patch) => settings.set({ deskWidgets: all().map((w) => (w.id === id ? { ...w, ...patch, opts: { ...(w.opts || {}), ...(patch.opts || {}) } } : w)) });
// a new size where it fits: its own block if there is room there, else the nearest free one; false with no room
export function resizeDeskWidget(id, size) {
  const w = all().find((x) => x.id === id);
  const d = shown().find((x) => dkOf(x) === w?.dk) || mainDisplay();
  if (!w || !d) return false;
  if (pinOf(w)) { setDeskWidget(id, { size }); return true; }
  const g = gridFor(d), others = [...placed(deskOf(w), d).filter((x) => x.id !== id), ...pinBlocks(g, workAreaOf(d), pinnedOn(deskOf(w), d))];
  const spot = fits(g, occupancy(others), w.col, w.row, size) ? { col: w.col, row: w.row } : nearestFree(g, others, size, w.col, w.row);
  if (!spot) return false;
  setDeskWidget(id, { size, ...spot });
  return true;
}
export const deskWidgetSize = (size) => { const r = rectOf({ x: 0, y: 0, cell: 170, gap: 16 }, 0, 0, size); return [r.w, r.h]; };

// how many widgets each display shows in the activity in view, by its key (widgetDisplayKey): a display with widgets
// isn't empty, so the theme leaves its empty state out
export const deskWidgetsShown = createStore({});
export const widgetDisplayKey = (d) => dkOf(d);

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
      for (const w of pinnedOn(desk, d)) {
        const r = pinnedRect(workAreaOf(d), w.size, pinOf(w));
        if (r) (byType[w.type] ||= []).push({ id: w.id, x: r.x, y: r.y, w: r.w, h: r.h, size: w.size, kind: w.type, opts: w.opts || {}, dk: dkOf(d) });
      }
    }
    for (const type of PAINT_ORDER) setWidgets(`ws-${type}`, byType[type] || []);
    setWidgetRects(Object.values(byType).flat());
    const per = {};
    for (const list of Object.values(byType)) for (const w of list) per[w.dk] = (per[w.dk] || 0) + 1;
    // replaced, not merged (a store merges an object into an object): a display left with none drops out
    if (JSON.stringify(per) !== JSON.stringify(deskWidgetsShown.get())) deskWidgetsShown.set(() => per);
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
      if (dr.moving && dr.target) setDeskWidget(widget.id, { dk: dr.target.dk, col: dr.target.col, row: dr.target.row, opts: { pin: null } });
    });
    return () => { offMove(); offUp(); };
  }, [widget.id, widget.size, widget.dk]);
  const g = live ? { ...widget, ...live } : widget;
  const Body = BODIES[widget.kind];
  const bare = widget.opts?.frame === 'none' && FRAMELESS.has(widget.kind);
  const sizes = KIND_SIZES[widget.kind] || ['s'];
  const items = [
    ...(sizes.length > 1 ? [{ label: t('shell.widgets.size'), submenu: sizes.map((sz) => ({ label: t(`shell.widgets.size.${sz}`), checked: sz === widget.size, onClick: () => resizeDeskWidget(widget.id, sz) })) }] : []),
    ...(KIND_MENU[widget.kind] ? KIND_MENU[widget.kind](widget) : []),
    { label: t('shell.widgets.pin.bottomCenter'), checked: widget.opts?.pin === 'bottom-center',
      onClick: () => setDeskWidget(widget.id, { opts: { pin: widget.opts?.pin === 'bottom-center' ? null : 'bottom-center' } }) },
    '-',
    { label: t('shell.widgets.remove'), danger: true, onClick: () => removeDeskWidget(widget.id) },
  ];
  return (
    <>
      <View onMouseDown={(e) => { if (!e.button) drag.current = { mx: e.x, my: e.y, x: widget.x, y: widget.y, moving: false }; }}
        onContextMenu={(e) => {
          const d = shown().find((x) => e.x >= x.x && e.x < x.x + x.w && e.y >= x.y && e.y < x.y + x.h) || shown().find((x) => dkOf(x) === widget.dk) || { x: 0, y: 0, w: 4000, h: 4000 };
          setMenu({ x: e.x, y: e.y, d: { x: d.x, y: d.y, w: d.w, h: d.h } });
        }}
        style={bare ? { position: 'absolute', left: g.x, top: g.y, width: widget.w, height: widget.h, borderRadius: 22, overflow: 'hidden',
          // no card: only while it is dragged a faint outline shows what is being moved
          ...(live ? { border: `1px dashed ${L.dark ? 'rgba(255,255,255,0.35)' : 'rgba(0,0,0,0.25)'}` } : null) }
        : { position: 'absolute', left: g.x, top: g.y, width: widget.w, height: widget.h, borderRadius: 22, overflow: 'hidden',
          backgroundColor: L.blur ? L.bg : L.solid, ...(L.blur ? { backdropFilter: `blur(${L.blur}px)` } : null),
          border: `0.5px solid ${L.dark ? 'rgba(255,255,255,0.2)' : 'rgba(0,0,0,0.12)'}`,
          boxShadow: live ? '0 24px 60px rgba(0,0,0,0.32)' : '0 8px 28px rgba(0,0,0,0.16)', transform: live ? 'scale(1.03)' : undefined }}>
        <View style={bare ? { flex: 1 } : { flex: 1, paddingTop: 13, paddingHorizontal: 15, paddingBottom: 14 }}>
          {Body ? <WidgetIdContext.Provider value={widget.id}><Body w={widget} L={L} /></WidgetIdContext.Provider> : null}
        </View>
      </View>
      {menu ? (
        // inside the widget's own display: near an edge it opens inwards, never onto the next monitor
        <Layer name="popover">
          <View style={{ position: 'absolute', left: menu.d.x, top: menu.d.y, width: menu.d.w, height: menu.d.h, pointerEvents: 'box-none' }}>
            <ContextMenu x={menu.x - menu.d.x} y={menu.y - menu.d.y} items={items} bounds={{ w: menu.d.w, h: menu.d.h }} onClose={() => setMenu(null)} />
          </View>
        </Layer>
      ) : null}
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
// a ring of short ticks (no SVG in the renderer): the filled ones are the value. The ticks are their own memoized
// component with plain props, so a ring whose filled count and colours are unchanged (most refreshes: the value
// moves by less than a tick) costs nothing in React; only the text in the middle is updated.
const TickMarks = memo(function TickMarks({ size, ticks, n, on, off }) {
  const c = size / 2, r = c - 4;
  return Array.from({ length: ticks }, (_, i) => {
    const a = (i / ticks) * Math.PI * 2, x = c + Math.sin(a) * r, y = c - Math.cos(a) * r;
    return <View key={i} style={{ position: 'absolute', left: x - 1.5, top: y - 4, width: 3, height: 8, borderRadius: 1.5, backgroundColor: i < n ? on : off, transform: `rotate(${(i / ticks) * 360}deg)` }} />;
  });
});
function TickRing({ L, size, value, color, ticks = 40, children }) {
  const n = Math.round(Math.max(0, Math.min(1, value || 0)) * ticks);
  return (
    <View style={{ width: size, height: size, alignItems: 'center', justifyContent: 'center', direction: 'ltr' }}>
      <TickMarks size={size} ticks={ticks} n={n} on={color || L.accent} off={L.track} />
      {children}
    </View>
  );
}
// This widget's own data, read every ms while it can be seen (sdk/widgetfeed.js: aligned with every other widget's
// reads, none while it is behind a window or nobody sees the screen)
function useEvery(fn, ms, deps = []) {
  return useFeed(null, () => { try { return fn(); } catch (e) { return null; } }, ms, deps);
}
// The shared sources: one read for every widget that shows them.
const minuteNow = () => Math.floor(Date.now() / 60000) * 60000;
const useMinute = () => useFeed('minute', minuteNow, 60000);
const secondNow = () => Math.floor(Date.now() / 1000) * 1000;
// CPU usage (0..1, in whole percents) between two reads of /proc/stat; null on the first
function cpuRead(st) {
  const [t, i] = lord.system.cpuTimes();
  const p = st.p;
  st.p = [t, i];
  if (!p || t <= p[0]) return null;
  return Math.round(Math.max(0, Math.min(1, 1 - (i - p[1]) / (t - p[0]))) * 100) / 100;
}
const sysInfoRead = () => lord.system.info();
// the network's rates (bytes/s) and the last 30 of them; null on the first read. After a time unseen the first rate
// is the average over that time, and the history carries on from there (it has a gap, nothing made up).
function netRead(st) {
  const s = lord.monitor?.netTotals ? lord.monitor.netTotals() : null;
  const p = st.p;
  st.p = s;
  if (!s || !p || s.t <= p.t) return st.v || null;
  const dt = (s.t - p.t) / 1000, v = { rx: (s.rx - p.rx) / dt, tx: (s.tx - p.tx) / dt };
  st.hist = [...(st.hist || []).slice(-29), v];
  st.v = { ...v, hist: st.hist };
  return st.v;
}
const netIfsRead = () => (lord.net?.interfaces ? lord.net.interfaces() : []);

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
// a hand of a face of radius c: deg from 12 o'clock, len past the centre, tail behind it
function hand(c, deg, len, tail, w, color) {
  const a = (deg * Math.PI) / 180, d = (len - tail) / 2;
  return <View style={{ position: 'absolute', left: c + Math.sin(a) * d - w / 2, top: c - Math.cos(a) * d - (len + tail) / 2, width: w, height: len + tail, borderRadius: w / 2, backgroundColor: color, transform: `rotate(${deg}deg)` }} />;
}
// The second hand (opts.style 'analog-seconds'), in the accent: its own component on a shared one-second feed, so
// each second re-renders this one node and repaints only the square it sweeps, never the face. Seconds are the same
// in every time zone, so the world cities' faces share it.
function SecondHand({ L, c, k }) {
  const s = new Date(useFeed('second', secondNow, 1000)).getSeconds();
  return hand(c, s * 6, 44 * k, 12 * k, 1.2 * k, L.accent);
}
function AnalogFace({ L, size, date, seconds }) {
  const k = size / 112, c = size / 2;
  const h = date.getHours(), m = date.getMinutes();
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
      {hand(c, ((h % 12) + m / 60) * 30, 26 * k, 6 * k, 3.6 * k, L.text)}
      {hand(c, m * 6, 40 * k, 8 * k, 2.6 * k, L.text)}
      {seconds ? <SecondHand L={L} c={c} k={k} /> : null}
      <View style={{ position: 'absolute', left: c - 3 * k, top: c - 3 * k, width: 6 * k, height: 6 * k, borderRadius: 3 * k, backgroundColor: seconds ? L.accent : '#F97316' }} />
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
  const digital = w.opts?.style === 'digital', seconds = w.opts?.style === 'analog-seconds';
  const here = cityName(localZone()) || t('shell.widgets.clock.name');
  if (w.size === 's') {
    return (
      <View style={{ flex: 1, alignItems: 'center', gap: 4 }}>
        {digital ? <View style={{ flex: 1, justifyContent: 'center' }}><Big L={L} size={44}>{formatTime(date)}</Big></View> : <AnalogFace L={L} size={104} date={date} seconds={seconds} />}
        <Text style={{ fontSize: 12, fontWeight: 650, color: L.text, textAlign: 'center' }}>{here}</Text>
        <Sub L={L} center>{formatDate(date, 'short')}</Sub>
      </View>
    );
  }
  if (w.size === 'm') {
    return (
      <View style={{ flex: 1, flexDirection: 'row', gap: 16 }}>
        <View style={{ alignItems: 'center', justifyContent: 'center', gap: 4, width: 128 }}>
          {digital ? <Big L={L} size={40}>{formatTime(date)}</Big> : <AnalogFace L={L} size={116} date={date} seconds={seconds} />}
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
          {digital ? <Big L={L} size={64}>{formatTime(date)}</Big> : <AnalogFace L={L} size={170} date={date} seconds={seconds} />}
          <Text style={{ fontSize: 12.5, fontWeight: 650, color: L.text }}>{here} · {formatDate(date, 'short')}</Text>
        </View>
        <View style={{ gap: 2 }}>{cities(4).map((z) => <CityRow key={z} L={L} zone={z} now={now} />)}</View>
      </View>
    );
  }
  // w (4×1): four cities in a row, a face each
  const list = [localZone(), ...cities(3)];
  return (
    <View style={{ flex: 1, flexDirection: 'row', justifyContent: 'space-around', alignItems: 'center' }}>
      {list.map((z) => {
        const d = zoneDate(z, now) || date;
        return (
          <View key={z || 'here'} style={{ alignItems: 'center', gap: 3, width: 160 }}>
            {digital ? <View style={{ height: 100, justifyContent: 'center' }}><Big L={L} size={36}>{formatTime(d)}</Big></View> : <AnalogFace L={L} size={100} date={d} seconds={seconds} />}
            <Text style={{ fontSize: 12, fontWeight: 650, color: L.text, textAlign: 'center', whiteSpace: 'nowrap', textOverflow: 'ellipsis', maxWidth: 160 }}>{cityName(z) || here}</Text>
            <Sub L={L} center>{formatTime(d)}</Sub>
          </View>
        );
      })}
    </View>
  );
}

// ---- system ----
function SystemBody({ w, L }) {
  const cpu = useFeed('cpu', cpuRead, 2000) || 0;
  const info = useFeed('sysinfo', sysInfoRead, 4000) || {};
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
// Styles (the widget's menu, w.opts.style), as in the approved mockups: small 'day' (the default: the day and what is
// next), 'month' or 'weather' (the day with the forecast); medium 'daymonth' (the default: the day and its events
// beside the month), 'agenda' (today and the next days) or 'busy' (the month shaded by how many events each day has);
// large 'agendamonth' (the default: the day and its events over the month) or 'month' (the month with today's events
// under it). The space is always used: with no events it says so and shows what comes next.
const CAL_RED = '#E5484D';
const CAL_STYLES = { s: ['day', 'month', 'weather'], m: ['daymonth', 'agenda', 'busy'], l: ['agendamonth', 'month'] };
const calStyle = (w) => { const ok = CAL_STYLES[w.size] || CAL_STYLES.s; return ok.includes(w.opts?.style) ? w.opts.style : ok[0]; };
const ymd = (d) => `${d.getFullYear()}-${d.getMonth()}-${d.getDate()}`;
const dayStart = (t) => { const d = new Date(t); d.setHours(0, 0, 0, 0); return d.getTime(); };
// the events from today on, for days ahead; asked again every 5 minutes and at once when the calendars change
// (usePimQuery follows them)
function useEventsAhead(now, days) {
  return usePimQuery(async () => {
    try {
      const from = dayStart(now);
      const occ = await events.range(from, from + days * 86400000);
      return (occ || []).filter((o) => (o.end || o.start) >= now - 60000).sort((a, b) => a.start - b.start);
    } catch (e) { return []; }
  }, [Math.floor(now / 300000), days]) || [];
}
const evKey = (e) => `${e.uid}${e.occ || ''}`;
const evTime = (e) => (e.allDay ? t('shell.widgets.calendar.allDay') : `${formatTime(new Date(e.start))} – ${formatTime(new Date(e.end || e.start))}`);
const todayOf = (ev, now) => { const a = dayStart(now), b = a + 86400000; return ev.filter((e) => e.start < b && (e.end || e.start) >= a); };
// "Tomorrow: Dentist, 09:00", "Thursday: …", or nothing in the week
function nextLine(ev, now) {
  const tomorrow = dayStart(now) + 86400000;
  const n = ev.find((e) => e.start >= tomorrow);
  if (!n || n.start >= tomorrow + 6 * 86400000) return t('shell.widgets.calendar.nothingWeek');
  const d = new Date(n.start);
  const when = n.start < tomorrow + 86400000 ? t('shell.widgets.calendar.tomorrow') : weekdayName(d.getDay(), false);
  return t('shell.widgets.calendar.next', { when, title: n.title, time: n.allDay ? t('shell.widgets.calendar.allDay') : formatTime(d) });
}
function CalTag({ children, color = CAL_RED, size = 11 }) {
  return <Text style={{ fontSize: size, fontWeight: 700, color, textAlign: 'start', whiteSpace: 'nowrap' }}>{String(children).toLocaleUpperCase()}</Text>;
}
function DayBlock({ L, d, size = 46 }) {
  return (
    <View>
      <CalTag>{weekdayName(d.getDay(), false)}</CalTag>
      <Text style={{ fontSize: size, fontWeight: 400, color: L.text, textAlign: 'start', lineHeight: size * 1.08 }}>{String(d.getDate())}</Text>
    </View>
  );
}
function EventRow({ L, e, compact }) {
  return (
    <View style={{ flexDirection: 'row', gap: 7, alignItems: 'center', minHeight: compact ? 26 : 30 }}>
      <View style={{ width: 3, alignSelf: 'stretch', borderRadius: 2, backgroundColor: e.color || L.accent }} />
      <View style={{ flex: 1, minWidth: 0 }}>
        <Text style={{ fontSize: compact ? 12 : 12.5, fontWeight: 600, color: L.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{e.title}</Text>
        <Sub L={L}>{evTime(e)}</Sub>
      </View>
    </View>
  );
}
// nothing today: says so, and what comes next
function NoneToday({ L, ev, now }) {
  return (
    <View style={{ flexDirection: 'row', gap: 7, alignItems: 'center', minHeight: 30 }}>
      <View style={{ width: 3, alignSelf: 'stretch', borderRadius: 2, backgroundColor: L.track }} />
      <View style={{ flex: 1, minWidth: 0 }}>
        <Text style={{ fontSize: 12.5, fontWeight: 600, color: L.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{t('shell.widgets.calendar.noToday')}</Text>
        <Sub L={L}>{nextLine(ev, now)}</Sub>
      </View>
    </View>
  );
}
// today's events, at most max; the last line says how many more there are
function TodayList({ L, ev, now, max, compact }) {
  const today = todayOf(ev, now);
  if (!today.length) return <NoneToday L={L} ev={ev} now={now} />;
  const more = today.length - max;
  const list = more > 0 ? today.slice(0, max - 1) : today;
  return (
    <View style={{ gap: compact ? 4 : 6 }}>
      {list.map((e) => <EventRow key={evKey(e)} L={L} e={e} compact={compact} />)}
      {more > 0 ? <Sub L={L}>{t('shell.widgets.calendar.more', { count: more + 1 })}</Sub> : null}
    </View>
  );
}
// The month: weeks from the language's first weekday, weekends dimmed, today in a red circle, a dot on a day with
// events (or, with heat, the day shaded by how many). fill: the weeks spread over the height there is.
function MonthGrid({ L, now, busy, font = 11, heat, fill }) {
  const d0 = new Date(now), y = d0.getFullYear(), m = d0.getMonth();
  const first = weekStartFor(locale.get().lang);
  const weeks = monthWeeks(y, m, first);
  const wds = [0, 1, 2, 3, 4, 5, 6].map((i) => (i + first) % 7);
  const dot = font * 1.95;
  return (
    <View style={{ direction: 'ltr', flex: fill ? 1 : undefined, justifyContent: fill ? 'space-between' : 'flex-start', gap: fill ? 0 : 1 }}>
      <View style={{ flexDirection: 'row' }}>
        {wds.map((wd) => <Text key={wd} style={{ flex: 1, fontSize: Math.max(9, font - 2), fontWeight: 700, textAlign: 'center', color: L.faint }}>{weekdayName(wd).slice(0, 1).toLocaleUpperCase()}</Text>)}
      </View>
      {weeks.map((wk, r) => (
        <View key={r} style={{ flexDirection: 'row' }}>
          {wk.map((d, i) => {
            const wd = wds[i], today = d === d0.getDate(), n = d ? busy.get(ymd(new Date(y, m, d))) || 0 : 0;
            const shade = heat && n && !today ? `rgba(229,72,77,${n >= 3 ? 0.55 : n === 2 ? 0.4 : 0.25})` : 'transparent';
            return (
              <View key={i} style={{ flex: 1, height: dot + 1, alignItems: 'center', justifyContent: 'center' }}>
                {d ? (
                  <View style={{ width: heat ? dot + 4 : dot, height: dot, borderRadius: today ? dot / 2 : 6, alignItems: 'center', justifyContent: 'center', backgroundColor: today ? CAL_RED : shade }}>
                    <Text style={{ fontSize: font, fontWeight: today ? 700 : 600, color: today ? '#ffffff' : wd === 0 || wd === 6 ? L.dim : L.text }}>{String(d)}</Text>
                    {n && !today && !heat ? <View style={{ position: 'absolute', bottom: 0, width: 3.5, height: 3.5, borderRadius: 2, backgroundColor: L.accent }} /> : null}
                  </View>
                ) : null}
              </View>
            );
          })}
        </View>
      ))}
    </View>
  );
}
const Rule = ({ L }) => <View style={{ height: 1, backgroundColor: L.track, marginVertical: 8 }} />;
function CalendarBody({ w, L }) {
  const now = useMinute();
  const d = new Date(now);
  const style = calStyle(w);
  const ev = useEventsAhead(now, style === 'busy' || style === 'month' || style === 'agendamonth' ? 42 : 8);
  const busy = new Map();
  for (const o of ev) { const k = ymd(new Date(o.start)); busy.set(k, (busy.get(k) || 0) + 1); }
  const monthTitle = (withYear) => <CalTag>{withYear ? `${monthName(d.getMonth())} ${d.getFullYear()}` : monthName(d.getMonth())}</CalTag>;
  if (style === 'month' && w.size === 's') {
    return <View style={{ flex: 1, gap: 4 }}>{monthTitle(false)}<MonthGrid L={L} now={now} busy={busy} font={10} fill /></View>;
  }
  if (style === 'weather') return <CalendarWeather L={L} d={d} />;
  if (w.size === 's') {
    const today = todayOf(ev, now);
    return (
      <View style={{ flex: 1, gap: 6 }}>
        <DayBlock L={L} d={d} size={48} />
        {today[0] ? <EventRow L={L} e={today[0]} compact /> : <Quiet L={L} text={t('shell.widgets.calendar.noEvents')} />}
        <Sub L={L}>{today.length > 1 ? t('shell.widgets.calendar.more', { count: today.length - 1 }) : nextLine(ev, now)}</Sub>
      </View>
    );
  }
  if (style === 'agenda') {
    const tomorrow = dayStart(now) + 86400000;
    const later = ev.filter((e) => e.start >= tomorrow && e.start < tomorrow + 86400000).slice(0, 1);
    return (
      <View style={{ flex: 1, gap: 4 }}>
        <CalTag size={10}>{`${t('shell.widgets.calendar.today')} · ${weekdayName(d.getDay(), false)} ${d.getDate()}`}</CalTag>
        <TodayList L={L} ev={ev} now={now} max={2} compact />
        {later.length ? <View style={{ gap: 4, marginTop: 2 }}><CalTag size={10} color={L.dim}>{t('shell.widgets.calendar.tomorrow')}</CalTag>{later.map((e) => <EventRow key={evKey(e)} L={L} e={e} compact />)}</View> : null}
      </View>
    );
  }
  if (style === 'busy') {
    return (
      <View style={{ flex: 1, flexDirection: 'row', gap: 14 }}>
        <View style={{ width: 104 }}>{monthTitle(false)}<Text style={{ fontSize: 14, fontWeight: 600, color: L.text, textAlign: 'start', marginTop: 2 }}>{weekdayName(d.getDay(), false)}</Text>
          <Text style={{ fontSize: 46, fontWeight: 400, color: L.text, textAlign: 'start', lineHeight: 50 }}>{String(d.getDate())}</Text></View>
        <View style={{ flex: 1, minWidth: 0 }}><MonthGrid L={L} now={now} busy={busy} font={9.5} heat fill /></View>
      </View>
    );
  }
  if (w.size === 'm') {
    return (
      <View style={{ flex: 1, flexDirection: 'row', gap: 14 }}>
        <View style={{ width: 132, gap: 6 }}><DayBlock L={L} d={d} size={42} /><TodayList L={L} ev={ev} now={now} max={2} compact /></View>
        <View style={{ flex: 1, minWidth: 0, gap: 2 }}>{monthTitle(false)}<MonthGrid L={L} now={now} busy={busy} font={9.5} fill /></View>
      </View>
    );
  }
  if (style === 'month') {
    return (
      <View style={{ flex: 1 }}>
        {monthTitle(true)}
        <View style={{ height: 4 }} />
        <MonthGrid L={L} now={now} busy={busy} font={12.5} fill />
        <Rule L={L} />
        {todayOf(ev, now).length ? <CalTag size={10} color={L.dim}>{t('shell.widgets.calendar.today')}</CalTag> : null}
        <View style={{ height: 4 }} />
        <TodayList L={L} ev={ev} now={now} max={2} compact />
      </View>
    );
  }
  return (
    <View style={{ flex: 1 }}>
      <View style={{ flexDirection: 'row', gap: 14, alignItems: todayOf(ev, now).length ? 'flex-start' : 'center' }}>
        <DayBlock L={L} d={d} size={46} />
        <View style={{ flex: 1, minWidth: 0, paddingTop: 2 }}><TodayList L={L} ev={ev} now={now} max={3} compact /></View>
      </View>
      <Rule L={L} />
      {monthTitle(true)}
      <View style={{ height: 4 }} />
      <MonthGrid L={L} now={now} busy={busy} font={11.5} fill />
    </View>
  );
}
// small, with the weather: the month, the weekday, the day and today's forecast where the person is (when the
// weather widget's forecasts are turned on; without them, only the day)
function CalendarWeather({ L, d }) {
  const on = wxOn();
  const here = wxHere();
  const data = useWeather(here ? [here] : [], on);
  const cur = data[here];
  return (
    <View style={{ flex: 1, gap: 2 }}>
      <CalTag>{monthName(d.getMonth())}</CalTag>
      <Text style={{ fontSize: 15, fontWeight: 600, color: L.text, textAlign: 'start' }}>{weekdayName(d.getDay(), false)}</Text>
      <View style={{ flexDirection: 'row', alignItems: 'flex-end', gap: 10 }}>
        <Text style={{ fontSize: 56, fontWeight: 400, color: L.text, textAlign: 'start', lineHeight: 60 }}>{String(d.getDate())}</Text>
        {cur && cur.c != null ? <WxGlyph code={cur.c} day={cur.d} size={32} /> : null}
      </View>
      {cur && cur.t != null ? <Sub L={L}>{`${tempIn(cur.t, wxUnit())}° · ${wxHereName() || ''}`}</Sub> : null}
    </View>
  );
}
function calendarMenu(w) {
  const cur = calStyle(w);
  return [{ label: t('shell.widgets.calendar.style'), submenu: (CAL_STYLES[w.size] || CAL_STYLES.s).map((v) => ({
    label: t(`shell.widgets.calendar.style.${v}`), checked: v === cur, onClick: () => setDeskWidget(w.id, { opts: { style: v } }) })) }];
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
  const speed = useFeed('net', netRead, 2000);
  const hist = { current: speed?.hist || [] };
  const ifs = useFeed('net.ifs', netIfsRead, 8000) || [];
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

// ---- devices: the battery of the computer, mice, keyboards, headsets (sdk/devicesmodel.js) ----
// No timer: lord.power and lord.bt send their changes (a power_supply uevent, a BlueZ signal), so the list is as
// fresh as the kernel's and costs nothing at rest. lord-hidpp's exact Logitech percentages come through the shared
// feed once a minute while a Devices widget is in view.
// a small drawing of each kind, in plain views (no SVG in the renderer)
function DeviceGlyph({ kind, size, color }) {
  const u = size / 24, line = Math.max(1.5, 1.8 * u);
  const b = (st) => <View style={{ position: 'absolute', ...st }} />;
  const box = { width: size, height: size, direction: 'ltr' };
  if (kind === 'laptop') return <View style={box}>{b({ left: 5 * u, top: 6 * u, width: 14 * u, height: 9 * u, borderRadius: 1.5 * u, border: `${line}px solid ${color}` })}{b({ left: 3 * u, top: 17 * u, width: 18 * u, height: line, borderRadius: line, backgroundColor: color })}</View>;
  if (kind === 'mouse') return <View style={box}>{b({ left: 7 * u, top: 3.5 * u, width: 10 * u, height: 17 * u, borderRadius: 5 * u, border: `${line}px solid ${color}` })}{b({ left: 12 * u - line / 2, top: 6.5 * u, width: line, height: 3.5 * u, borderRadius: line, backgroundColor: color })}</View>;
  if (kind === 'keyboard') return <View style={box}>{b({ left: 3 * u, top: 7 * u, width: 18 * u, height: 10 * u, borderRadius: 2 * u, border: `${line}px solid ${color}` })}{[6.5, 10, 13.5, 17].map((x) => <View key={x} style={{ position: 'absolute', left: x * u - 0.5 * u, top: 10 * u, width: 1.6 * u, height: 1.6 * u, borderRadius: u, backgroundColor: color }} />)}{b({ left: 7 * u, top: 13.6 * u, width: 10 * u, height: line, borderRadius: line, backgroundColor: color })}</View>;
  if (kind === 'headset') return <View style={box}>{b({ left: 5 * u, top: 4 * u, width: 14 * u, height: 14 * u, borderRadius: 7 * u, border: `${line}px solid ${color}`, borderBottomColor: 'transparent' })}{b({ left: 4 * u, top: 13 * u, width: 4 * u, height: 7 * u, borderRadius: 1.5 * u, backgroundColor: color })}{b({ left: 16 * u, top: 13 * u, width: 4 * u, height: 7 * u, borderRadius: 1.5 * u, backgroundColor: color })}</View>;
  if (kind === 'gamepad') return <View style={box}>{b({ left: 3 * u, top: 7 * u, width: 18 * u, height: 11 * u, borderRadius: 5 * u, border: `${line}px solid ${color}` })}{b({ left: 7 * u, top: 11.5 * u, width: 4 * u, height: line, backgroundColor: color })}{b({ left: 9 * u - line / 2, top: 9.5 * u, width: line, height: 4 * u, backgroundColor: color })}</View>;
  if (kind === 'pen') return <View style={box}>{b({ left: 11 * u, top: 2 * u, width: 2.6 * u, height: 18 * u, borderRadius: 1.3 * u, backgroundColor: color, transform: 'rotate(35deg)' })}</View>;
  if (kind === 'phone') return <View style={box}>{b({ left: 7 * u, top: 3 * u, width: 10 * u, height: 18 * u, borderRadius: 2.5 * u, border: `${line}px solid ${color}` })}</View>;
  return <View style={box}>{b({ left: 4 * u, top: 8 * u, width: 15 * u, height: 8 * u, borderRadius: 2 * u, border: `${line}px solid ${color}` })}{b({ left: 19.5 * u, top: 10.5 * u, width: 1.8 * u, height: 3 * u, borderRadius: u, backgroundColor: color })}</View>;
}
const toneColor = (L, d) => ({ ok: L.ok, warn: L.warn, crit: L.crit }[deviceTone(d)]);
function DeviceRing({ L, d, size, ticks = 32 }) {
  return (
    <TickRing L={L} size={size} value={fractionOf(d)} color={toneColor(L, d)} ticks={ticks}>
      <DeviceGlyph kind={d.kind} size={size * 0.38} color={L.text} />
    </TickRing>
  );
}
const ValueText = ({ L, d, size }) => (
  <Text style={{ fontSize: size, fontWeight: 650, color: deviceTone(d) === 'ok' ? L.text : toneColor(L, d), whiteSpace: 'nowrap' }}>{deviceValue(d)}</Text>
);
// a device as a row: its ring, then its name and value (and how it connects, when there is room)
function DeviceRow({ L, d, ring, nameSize, via }) {
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, minWidth: 0 }}>
      <DeviceRing L={L} d={d} size={ring} ticks={ring >= 56 ? 32 : 24} />
      <View style={{ flex: 1, minWidth: 0, gap: 1 }}>
        <Text style={{ fontSize: nameSize, fontWeight: 600, color: L.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{deviceShort(d)}</Text>
        <ValueText L={L} d={d} size={nameSize + 1} />
        {via ? <Sub L={L}>{deviceVia(d)}</Sub> : null}
      </View>
    </View>
  );
}
function DevicesBody({ w, L }) {
  const list = useDevices();
  if (!list.length) {
    return <View style={{ flex: 1 }}><Head L={L} title={t('shell.widgets.devices.name')} /><Quiet L={L} text={t('shell.widgets.devices.none')} /></View>;
  }
  if (w.size === 's') {
    // one: a large ring with its name; two: a row each; three or four: a 2×2 of rings with their values
    if (list.length === 1) {
      const d = list[0];
      return (
        <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 6 }}>
          <DeviceRing L={L} d={d} size={88} ticks={36} />
          <Text style={{ fontSize: 12, fontWeight: 600, color: L.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{deviceShort(d)}</Text>
          <ValueText L={L} d={d} size={13} />
        </View>
      );
    }
    if (list.length === 2) {
      return <View style={{ flex: 1, justifyContent: 'space-evenly' }}>{list.map((d) => <DeviceRow key={d.id} L={L} d={d} ring={50} nameSize={12} />)}</View>;
    }
    return (
      <View style={{ flex: 1, flexDirection: 'row', flexWrap: 'wrap', alignContent: 'space-between' }}>
        {list.slice(0, 4).map((d) => (
          <View key={d.id} style={{ width: '50%', alignItems: 'center', gap: 3 }}>
            <DeviceRing L={L} d={d} size={46} ticks={24} />
            <ValueText L={L} d={d} size={10.5} />
          </View>
        ))}
      </View>
    );
  }
  if (w.size === 'm') {
    // one running low and not charging takes the card: the warning, with its name
    const low = list.find((d) => deviceTone(d) === 'crit');
    if (low) {
      return (
        <View style={{ flex: 1, flexDirection: 'row', alignItems: 'center', gap: 16 }}>
          <DeviceRing L={L} d={low} size={96} ticks={36} />
          <View style={{ flex: 1, minWidth: 0, gap: 3 }}>
            <Text style={{ fontSize: 11, fontWeight: 700, color: L.crit, textAlign: 'start' }}>{t('shell.widgets.devices.low').toLocaleUpperCase()}</Text>
            <Text style={{ fontSize: 17, fontWeight: 650, color: L.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{deviceShort(low)}</Text>
            <Sub L={L}>{`${deviceValue(low)} · ${deviceVia(low)}`}</Sub>
            {list.length > 1 ? <Sub L={L}>{list.filter((d) => d !== low).slice(0, 2).map((d) => `${deviceShort(d)} ${deviceValue(d)}`).join(' · ')}</Sub> : null}
          </View>
        </View>
      );
    }
    // up to two: side by side as rows, with their names; three or four: rings with name and value under
    if (list.length <= 2) {
      return (
        <View style={{ flex: 1, flexDirection: 'row', alignItems: 'center', gap: 14 }}>
          {list.map((d) => <View key={d.id} style={{ flex: 1, minWidth: 0 }}><DeviceRow L={L} d={d} ring={64} nameSize={13} via /></View>)}
        </View>
      );
    }
    return (
      <View style={{ flex: 1, flexDirection: 'row', justifyContent: 'space-around', alignItems: 'center' }}>
        {list.slice(0, 4).map((d) => (
          <View key={d.id} style={{ alignItems: 'center', gap: 4, width: 74 }}>
            <DeviceRing L={L} d={d} size={58} />
            <ValueText L={L} d={d} size={12.5} />
            <Text style={{ fontSize: 10.5, color: L.dim, whiteSpace: 'nowrap', textOverflow: 'ellipsis', maxWidth: 74 }}>{deviceShort(d)}</Text>
          </View>
        ))}
      </View>
    );
  }
  // large: a list with names, connection and bars, spread over the card
  const rows = list.slice(0, 5);
  const big = rows.length <= 3;
  return (
    <View style={{ flex: 1, gap: 6 }}>
      <Head L={L} title={t('shell.widgets.devices.name')} />
      <View style={{ flex: 1, justifyContent: rows.length >= 3 ? 'space-evenly' : 'flex-start', gap: rows.length >= 3 ? 0 : 18, paddingTop: rows.length >= 3 ? 0 : 6 }}>
        {rows.map((d) => (
          <View key={d.id} style={{ flexDirection: 'row', alignItems: 'center', gap: 12 }}>
            <DeviceRing L={L} d={d} size={big ? 54 : 44} ticks={28} />
            <View style={{ flex: 1, minWidth: 0, gap: 4 }}>
              <View style={{ flexDirection: 'row', alignItems: 'baseline', gap: 8 }}>
                <Text style={{ flex: 1, fontSize: big ? 14.5 : 13.5, fontWeight: 600, color: L.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{deviceShort(d)}</Text>
                <ValueText L={L} d={d} size={big ? 16 : 14} />
              </View>
              <Sub L={L}>{deviceVia(d)}</Sub>
              <Bar L={L} value={fractionOf(d)} color={toneColor(L, d)} />
            </View>
          </View>
        ))}
      </View>
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

// ---- Claude Code: every session of every account (~/.claude and ~/.claude-* folders), its context and the plan's limits ----
const readJson = (path) => { try { const v = JSON.parse(lord.fs.read(path)); return v && typeof v === 'object' ? v : null; } catch (e) { return null; } };
const pctOf = (v) => (v == null || v === '' || !Number.isFinite(+v) ? null : Math.max(0, Math.min(100, Math.round(+v))));
function limitOf(x, now) {
  const p = pctOf(x?.used_percentage);
  if (p == null) return null;
  const at = Number.isFinite(+x.resets_at) && +x.resets_at > 0 ? +x.resets_at * 1000 : null;
  return at != null && at <= now ? null : { pct: p, resetsAt: at };
}
// the accounts' names and plans: their .claude.json, read once a minute
let claudeAccounts = { at: 0, map: {} };
function accountsFor(home, dirs) {
  if (Date.now() - claudeAccounts.at < 60000) return claudeAccounts.map;
  const map = {};
  for (const d of dirs) {
    let text = null;
    try { text = lord.fs.read(d === '.claude' ? `${home}/.claude.json` : `${home}/${d}/.claude.json`); } catch (e) { text = null; }
    const a = text ? accountOf(text) : null;
    if (a) map[d] = a;
  }
  claudeAccounts = { at: Date.now(), map };
  return map;
}
function claudeData() {
  const home = lord.fs.home(), now = Date.now();
  let dirs = ['.claude'];
  try { dirs = configDirs(lord.fs.list(home).filter((e) => e.dir).map((e) => e.name)); } catch (e) { /* the default only */ }
  const out = [];
  for (const dir of dirs) {
    let names = [];
    try { names = lord.fs.list(`${home}/${dir}/sessions`).map((e) => e.name).filter((n) => /^\d{1,9}\.json$/.test(n)).slice(0, 16); } catch (e) { continue; }
    for (const n of names) {
      const d = readJson(`${home}/${dir}/sessions/${n}`);
      const id = d && /^[A-Za-z0-9-]{1,64}$/.test(String(d.sessionId || '')) ? d.sessionId : null;
      if (!id) continue;
      const st = readJson(`${home}/.cache/lordos/claude/${id}.json`);
      const mine = st && st.session_id === id;
      const cwd = mine && typeof st.workspace?.current_dir === 'string' ? st.workspace.current_dir : typeof d.cwd === 'string' ? d.cwd : '';
      out.push({ id, dir, state: d.status === 'busy' ? 'working' : d.status === 'idle' ? 'waiting' : '', ctx: mine ? pctOf(st.context_window?.used_percentage) : null,
        model: mine && typeof st.model?.display_name === 'string' ? st.model.display_name.slice(0, 24) : '', cwd: cwd.split('/').filter(Boolean).pop() || '',
        five: mine ? limitOf(st.rate_limits?.five_hour, now) : null, week: mine ? limitOf(st.rate_limits?.seven_day, now) : null,
        at: +d.statusUpdatedAt || +d.updatedAt || 0 });
    }
  }
  return summarize(out, accountsFor(home, dirs));
}
// a slow glow while a limit is near: a timer only then (900 ms), none otherwise
function AlertGlow({ level }) {
  const [on, setOn] = useState(true);
  useEffect(() => { if (!level) return undefined; const id = setInterval(() => setOn((x) => !x), 900); return () => clearInterval(id); }, [level]);
  if (!level) return null;
  const c = level === 'high' ? '#ff453a' : '#ffb020';
  return <View style={{ position: 'absolute', left: -15, top: -13, right: -15, bottom: -14, borderRadius: 22, border: `2px solid ${c}`,
    boxShadow: on ? `0 0 22px ${c}` : `0 0 6px ${c}`, opacity: on ? 1 : 0.55, pointerEvents: 'none' }} />;
}
// inner: a caption under the figure, inside the ring (the small Claude Code widget's "Context")
function Gauge({ L, size, pct, label, sub, inner }) {
  const v = (pct ?? 0) / 100;
  return (
    <View style={{ alignItems: 'center', gap: 2, minWidth: 0 }}>
      <TickRing L={L} size={size} value={v} color={toneOf(L, v)} ticks={size >= 90 ? 48 : 36}>
        <Text style={{ fontSize: size >= 90 ? 19 : size >= 70 ? 15 : 13, fontWeight: 650, color: L.text }}>{pct != null ? `${pct}%` : '–'}</Text>
        {inner ? <Text style={{ fontSize: 9.5, fontWeight: 600, color: L.dim, whiteSpace: 'nowrap', textOverflow: 'ellipsis', maxWidth: size * 0.62, textAlign: 'center' }}>{inner}</Text> : null}
      </TickRing>
      {label ? <Text style={{ fontSize: 11, fontWeight: 600, color: L.text, whiteSpace: 'nowrap' }}>{label}</Text> : null}
      {sub ? <Text style={{ fontSize: 10, color: L.dim, whiteSpace: 'nowrap', textOverflow: 'ellipsis', maxWidth: size + 30 }}>{sub}</Text> : null}
    </View>
  );
}
function ClaudeBody({ w, L }) {
  const data = useFeed('claude', claudeData, 60000) || { sessions: [], accounts: [] };
  const list = data.sessions;
  const dot = (s) => (s === 'working' ? L.warn : s === 'waiting' ? L.ok : L.faint);
  const stateText = (s) => t(s === 'working' ? 'shell.widgets.claude.working' : s === 'waiting' ? 'shell.widgets.claude.waiting' : 'shell.widgets.claude.idle');
  if (!list.length) return <View style={{ flex: 1 }}><Head L={L} title="Claude Code" /><Quiet L={L} text={t('shell.widgets.claude.none')} /></View>;
  const c = list[0];
  const acc = data.accounts.find((a) => a.dir === c.dir) || {};
  const five = acc.five || c.five, week = acc.week || c.week, now = Date.now();
  const alert = alertOf({ ctx: c.ctx, five, week });
  const left = (l) => (l?.resetsAt ? t('shell.widgets.claude.resets', { t: leftText(l.resetsAt - now, t) }) : '');
  const who = acc.account ? `${acc.account.name}${acc.account.plan ? ` · ${acc.account.plan}` : ''}` : c.dir === '.claude' ? '' : c.dir.replace(/^\.claude-/, '');
  const stateRow = (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 5, minWidth: 0 }}>
      <View style={{ width: 7, height: 7, borderRadius: 4, backgroundColor: dot(c.state) }} />
      <Text style={{ fontSize: 11, color: L.dim, whiteSpace: 'nowrap', textOverflow: 'ellipsis', minWidth: 0 }}>{[stateText(c.state), c.model].filter(Boolean).join(' · ')}</Text>
    </View>
  );
  if (w.size === 's') {
    const mini = (lab, l) => (
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 5, alignSelf: 'stretch' }}>
        <Text style={{ width: 28, fontSize: 10, fontWeight: 600, color: L.dim, textAlign: 'start' }}>{lab}</Text>
        <View style={{ flex: 1 }}><Bar L={L} value={(l?.pct ?? 0) / 100} color={toneOf(L, (l?.pct ?? 0) / 100)} /></View>
        <Text style={{ width: 30, fontSize: 10.5, fontWeight: 650, color: L.text, textAlign: 'end' }}>{l?.pct != null ? `${l.pct}%` : '–'}</Text>
      </View>
    );
    // the context ring as large as the card allows, the text along the bottom (as the weather's)
    return (
      <View style={{ flex: 1, gap: 3 }}>
        <AlertGlow level={alert} />
        <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center' }}>
          <Gauge L={L} size={90} pct={c.ctx} inner={t('shell.widgets.claude.ctx')} />
        </View>
        {stateRow}
        {mini(t('shell.widgets.claude.fiveShort'), five)}
        {mini(t('shell.widgets.claude.weekShort'), week)}
      </View>
    );
  }
  const big = w.size === 'l' ? 88 : 66;
  const gauges = (
    <View style={{ flexDirection: 'row', justifyContent: 'space-around', alignItems: 'flex-start' }}>
      <Gauge L={L} size={big} pct={c.ctx} label={t('shell.widgets.claude.ctx')} sub={c.cwd} />
      <Gauge L={L} size={big} pct={five?.pct ?? null} label={t('shell.widgets.claude.five')} sub={left(five)} />
      <Gauge L={L} size={big} pct={week?.pct ?? null} label={t('shell.widgets.claude.week')} sub={left(week)} />
    </View>
  );
  return (
    <View style={{ flex: 1, gap: 6 }}>
      <AlertGlow level={alert} />
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        <Text style={{ fontSize: 12, fontWeight: 650, color: L.dim }}>Claude Code</Text>
        <View style={{ flex: 1, minWidth: 0 }}>{stateRow}</View>
        {who ? <Text style={{ fontSize: 11, color: L.dim, whiteSpace: 'nowrap', textOverflow: 'ellipsis', maxWidth: 130 }}>{who}</Text> : null}
      </View>
      {alert ? <Text style={{ fontSize: 11, fontWeight: 650, color: alert === 'high' ? L.crit : L.warn, textAlign: 'start' }}>{t('shell.widgets.claude.near')}</Text> : null}
      {gauges}
      {w.size === 'l' ? (
        <View style={{ gap: 4, marginTop: 4 }}>
          {list.slice(0, 4).map((x) => (
            <View key={x.id} style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
              <View style={{ width: 7, height: 7, borderRadius: 4, backgroundColor: dot(x.state) }} />
              <Text style={{ flex: 1, fontSize: 11.5, color: L.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{x.cwd || x.model || 'Claude'}</Text>
              {data.accounts.length > 1 ? <Text style={{ fontSize: 10.5, color: L.dim }}>{(data.accounts.find((a) => a.dir === x.dir)?.account?.name) || x.dir.replace(/^\.claude-?/, '') || '·'}</Text> : null}
              <Text style={{ width: 36, fontSize: 11, fontWeight: 650, color: x.ctx != null ? toneOf(L, x.ctx / 100) : L.dim, textAlign: 'end' }}>{x.ctx != null ? `${x.ctx}%` : '–'}</Text>
            </View>
          ))}
          {data.accounts.length > 1 ? data.accounts.map((a) => (
            <Text key={a.dir} style={{ fontSize: 10.5, color: L.dim, textAlign: 'start' }}>{`${a.account?.name || a.dir} · ${t('shell.widgets.claude.weekShort')} ${a.week?.pct != null ? `${a.week.pct}%` : '–'} · ${t('shell.widgets.claude.fiveShort')} ${a.five?.pct != null ? `${a.five.pct}%` : '–'}`}</Text>
          )) : null}
        </View>
      ) : null}
    </View>
  );
}

// ---- weather (Open-Meteo through lord-fetch, by the time zone's coordinates: server/src/fetch/wxsvc.h) ----
// Off until the person turns it on from the widget (settings.weather.on): nothing is asked of the internet before.
const wxOn = () => !!settings.get().weather?.on;
// where the forecast is for: a city the person picked from the offline list (asked as "geonames/<id>": lord-server
// takes its coordinates from assets/geo/cities.tsv), else the time zone's. Each widget has its own (opts.city, 0.8.9.1,
// #193); settings.weather.city is the default: the last one chosen, which a new widget starts on and the calendar's
// weather style shows (weathermodel.js widgetCity). Two widgets on the same city share one forecast: lord-server keeps
// one per place (wxsvc.c), and a widget asks again only when its 10-minute turn comes.
const wxCity = (w) => widgetCity(w?.opts, settings.get().weather?.city);
const wxHere = (w) => placeOf(wxCity(w), localZone());
const wxHereName = (w) => wxCity(w)?.name || cityName(localZone());
const wxSetCity = (w, c) => {
  const city = cityOk(c);
  settings.set({ weather: { ...(settings.get().weather || {}), city } });
  if (w) setDeskWidget(w.id, { opts: { city } });
};
// the city picker: which widget asked (null: closed)
const wxPick = createStore(null);
const wxUnit = () => settings.get().weather?.unit || unitFor(regionCountry(locale.get().region));
function useWeather(zones, on) {
  const now = useMinute();
  const key = zones.join('|');
  const [tick, setTick] = useState(0);
  useEffect(() => {
    if (!on) return undefined;
    for (const z of zones) { try { lord.weather?.request(z); } catch (e) { /* no lord.weather (tests, older server) */ } }
    // the first answer takes a few seconds: look again shortly, then once a minute (useMinute)
    const id = setTimeout(() => setTick((x) => x + 1), 4000);
    return () => clearTimeout(id);
  }, [key, on, Math.floor(now / 600000)]);
  void tick;
  const out = {};
  for (const z of zones) { try { out[z] = on ? lord.weather?.get(z) || null : null; } catch (e) { out[z] = null; } }
  return out;
}
// drawn, no emoji: a sun (or moon), a cloud, and what falls from it
function WxGlyph({ code, day = 1, size = 40 }) {
  const k = kindOf(code), s = size / 40;
  const sun = (x, y, r) => (
    <View style={{ position: 'absolute', left: x - r, top: y - r, width: 2 * r, height: 2 * r }}>
      {Array.from({ length: 8 }, (_, i) => {
        const a = (i / 8) * Math.PI * 2, d = r * 0.98;
        return <View key={i} style={{ position: 'absolute', left: r + Math.sin(a) * d - 1.2 * s, top: r - Math.cos(a) * d - 3 * s, width: 2.4 * s, height: 6 * s, borderRadius: 1.2 * s, backgroundColor: '#FFC531', transform: `rotate(${i * 45}deg)` }} />;
      })}
      <View style={{ position: 'absolute', left: r * 0.42, top: r * 0.42, width: r * 1.16, height: r * 1.16, borderRadius: r * 0.58, backgroundColor: '#FFC531' }} />
    </View>
  );
  const moon = (x, y, r) => (
    <View style={{ position: 'absolute', left: x - r, top: y - r, width: 2 * r, height: 2 * r, borderRadius: r, backgroundColor: '#F2F0E6', overflow: 'hidden' }}>
      <View style={{ position: 'absolute', left: r * 0.7, top: -r * 0.35, width: 2 * r, height: 2 * r, borderRadius: r, backgroundColor: 'rgba(16,24,48,0.92)' }} />
    </View>
  );
  const cloud = (x, y, w, color = '#F4F6FA') => (
    <View style={{ position: 'absolute', left: x, top: y, width: w, height: w * 0.62 }}>
      <View style={{ position: 'absolute', left: w * 0.06, top: w * 0.26, width: w * 0.88, height: w * 0.36, borderRadius: w * 0.18, backgroundColor: color }} />
      <View style={{ position: 'absolute', left: w * 0.14, top: w * 0.14, width: w * 0.36, height: w * 0.36, borderRadius: w * 0.18, backgroundColor: color }} />
      <View style={{ position: 'absolute', left: w * 0.36, top: 0, width: w * 0.46, height: w * 0.46, borderRadius: w * 0.23, backgroundColor: color }} />
    </View>
  );
  const drops = (n, color, len, round) => Array.from({ length: n }, (_, i) => (
    <View key={i} style={{ position: 'absolute', left: (10 + i * (22 / Math.max(1, n - 1))) * s, top: 29 * s, width: (round ? 4 : 2.4) * s, height: (round ? 4 : len) * s, borderRadius: 2 * s, backgroundColor: color, transform: round ? undefined : 'rotate(18deg)' }} />
  ));
  const parts = [];
  if (k === 'clear') parts.push(<View key="c" style={{ position: 'absolute', left: 0, top: 0, width: size, height: size }}>{day ? sun(20 * s, 20 * s, 15 * s) : moon(20 * s, 20 * s, 13 * s)}</View>);
  else {
    if (k === 'partly') parts.push(<View key="p" style={{ position: 'absolute', left: 0, top: 0, width: size, height: size }}>{day ? sun(25 * s, 14 * s, 11 * s) : moon(26 * s, 13 * s, 9 * s)}</View>);
    parts.push(<View key="cl" style={{ position: 'absolute', left: 0, top: 0, width: size, height: size }}>{cloud(2 * s, (k === 'partly' ? 12 : 6) * s, 36 * s, k === 'storm' || k === 'rain' ? '#C9D1DC' : '#F4F6FA')}</View>);
    if (k === 'rain' || k === 'showers') parts.push(...drops(4, '#5AB0FF', 8, false));
    if (k === 'drizzle') parts.push(...drops(3, '#7CC0FF', 5, false));
    if (k === 'snow') parts.push(...drops(4, '#FFFFFF', 4, true));
    if (k === 'fog') parts.push(...[0, 1].map((i) => <View key={`f${i}`} style={{ position: 'absolute', left: (5 + i * 6) * s, top: (31 + i * 5) * s, width: 26 * s, height: 2.6 * s, borderRadius: 1.3 * s, backgroundColor: '#E6EAF0' }} />));
    if (k === 'storm') parts.push(<View key="b" style={{ position: 'absolute', left: 18 * s, top: 27 * s, width: 4 * s, height: 12 * s, backgroundColor: '#FFC531', borderRadius: 1 * s, transform: 'rotate(20deg)' }} />);
  }
  return <View style={{ width: size, height: size, direction: 'ltr' }}>{parts}</View>;
}
// Weather → Change city…: a search over the offline list of cities (lord.weather.cities: nothing asked of the network),
// or back to the time zone's, for the widget that asked (w). Centred on the widget's display, in the person's theme.
const foldQuery = (s) => String(s || '').normalize('NFD').replace(/[\u0300-\u036f]/g, '').toLowerCase().replace(/[^a-z0-9 '.-]/g, '').slice(0, 40);
function CityPicker({ w, onClose }) {
  const th = useTheme() || {};
  const [q, setQ] = useState('');
  const fq = foldQuery(q).trim();
  let list = [];
  try { list = fq.length >= 2 && lord.weather?.cities ? lord.weather.cities(fq) || [] : []; } catch (e) { list = []; }
  const cur = wxCity(w);
  const pick = (c) => { wxSetCity(w, c); onClose(); };
  useEffect(() => addListener('keydown', (e) => { if (e.key === 'Escape') onClose(); }), []);
  // on the widget's display (it opened on the main one; BUG-27 of the ws170 QA)
  const on = Number.isFinite(w.x) && Number.isFinite(w.y) ? displayAt(w.x + (w.w || 0) / 2, w.y + (w.h || 0) / 2) : null;
  const d = on || mainDisplay() || { x: 0, y: 0, w: 1280, h: 800 };
  const W = 420, H = 460;
  const row = (key, label, sub, on, onClick) => (
    <View key={key} onClick={onClick} style={{ paddingVertical: 8, paddingHorizontal: 12, borderRadius: 10, backgroundColor: on ? th.hover : 'transparent' }}>
      <Text style={{ fontSize: 13, fontWeight: 600, color: th.text, textAlign: 'start' }}>{label}</Text>
      {sub ? <Text style={{ fontSize: 11, color: th.textDim, textAlign: 'start' }}>{sub}</Text> : null}
    </View>
  );
  return (
    <Layer name="overlay">
      <View style={{ position: 'absolute', left: d.x + Math.round((d.w - W) / 2), top: d.y + Math.round((d.h - H) / 2), width: W, height: H, padding: 16, gap: 10,
        borderRadius: 18, backgroundColor: th.windowBg || '#1e1b26', border: `1px solid ${th.separator || 'rgba(255,255,255,0.1)'}`, boxShadow: '0 24px 70px rgba(0,0,0,0.45)' }}>
        <Text style={{ fontSize: 16, fontWeight: 700, color: th.text, textAlign: 'start' }}>{t('shell.widgets.weather.locationTitle')}</Text>
        <SearchField value={q} onChange={setQ} autoFocus placeholder={t('shell.widgets.weather.locationSearch')} onCancel={() => { if (!q) onClose(); }} style={{ height: 34 }} />
        <ScrollView style={{ flex: 1 }} contentStyle={{ gap: 2 }}>
          {row('auto', t('shell.widgets.weather.locationAuto'), cityName(localZone()), !cur, () => pick(null))}
          {list.map((c) => row(c.id, c.name, [c.cc, c.adm1].filter(Boolean).join(' · '), cur?.id === c.id, () => pick({ id: c.id, name: c.name, cc: c.cc })))}
          {fq.length >= 2 && !list.length ? <Text style={{ fontSize: 12, color: th.textDim, padding: 12 }}>{t('shell.widgets.weather.locationNone')}</Text> : null}
        </ScrollView>
        <Text style={{ fontSize: 10.5, color: th.textDim, textAlign: 'start' }}>{t('shell.widgets.weather.locationCredit')}</Text>
      </View>
    </Layer>
  );
}
const WX_TEXT = '#ffffff', WX_DIM = 'rgba(255,255,255,0.78)';
function WeatherBody({ w }) {
  const on = wxOn();
  useStore(settings, (s) => `${!!s.weather?.on}|${s.weather?.unit || ''}|${s.weather?.city?.id || ''}|${JSON.stringify(s.clock?.cities || [])}`);
  const picking = useStore(wxPick) === w.id;
  const here = wxHere(w);
  const zones = [here, ...(w.size === 'l' ? cities(3) : [])].filter(Boolean);
  const data = useWeather(zones, on);
  const cur = data[here], unit = wxUnit();
  const deg = (c) => `${tempIn(c, unit)}°`;
  const sky = skyOf(cur?.c != null ? kindOf(cur.c) : 'partly', cur ? cur.d : 1);
  const bleed = { position: 'absolute', left: -15, top: -13, right: -15, bottom: -14, background: `linear-gradient(170deg, ${sky[0]}, ${sky[1]})` };
  const city = wxHereName(w) || t('shell.widgets.weather.name');
  const picker = picking ? <CityPicker w={w} onClose={() => wxPick.set(null)} /> : null;
  if (!on) {
    return (
      <View style={{ flex: 1, gap: 6, justifyContent: 'center' }}>
        <View style={bleed} />
        {picker}
        <WxGlyph code={2} size={w.size === 's' ? 34 : 40} />
        <Text style={{ fontSize: 13, fontWeight: 650, color: WX_TEXT, textAlign: 'start' }}>{t('shell.widgets.weather.name')}</Text>
        {w.size !== 's' ? <Text style={{ fontSize: 11, color: WX_DIM, textAlign: 'start' }}>{t('shell.widgets.weather.consent')}</Text> : null}
        <View onClick={() => settings.set({ weather: { ...(settings.get().weather || {}), on: true } })}
          style={{ alignSelf: 'flex-start', height: 26, paddingHorizontal: 12, borderRadius: 13, justifyContent: 'center', backgroundColor: 'rgba(255,255,255,0.22)' }}>
          <Text style={{ fontSize: 12, fontWeight: 600, color: WX_TEXT }}>{t('shell.widgets.weather.turnOn')}</Text>
        </View>
      </View>
    );
  }
  if (!cur || cur.t == null) {
    return (
      <View style={{ flex: 1, gap: 4 }}>
        <View style={bleed} />
        {picker}
        <Text style={{ fontSize: 13, fontWeight: 650, color: WX_TEXT, textAlign: 'start' }}>{city}</Text>
        <Text style={{ fontSize: 11.5, color: WX_DIM, textAlign: 'start' }}>{t(cur?.error ? 'shell.widgets.weather.offline' : 'shell.widgets.weather.loading')}</Text>
      </View>
    );
  }
  const today = cur.days?.[0];
  const head = (
    <View style={{ gap: 1 }}>
      <Text style={{ fontSize: 13, fontWeight: 650, color: WX_TEXT, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{city}</Text>
      <Text style={{ fontSize: w.size === 's' ? 40 : 46, fontWeight: 300, color: WX_TEXT, textAlign: 'start' }}>{deg(cur.t)}</Text>
    </View>
  );
  const foot = (
    <View style={{ gap: 1 }}>
      <WxGlyph code={cur.c} day={cur.d} size={20} />
      <Text style={{ fontSize: 11.5, fontWeight: 600, color: WX_TEXT, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{t(`shell.widgets.weather.kind.${kindOf(cur.c)}`)}</Text>
      {today ? <Text style={{ fontSize: 11.5, color: WX_DIM, textAlign: 'start' }}>{t('shell.widgets.weather.hilo', { hi: deg(today[1]), lo: deg(today[2]) })}</Text> : null}
    </View>
  );
  if (w.size === 's') return <View style={{ flex: 1, justifyContent: 'space-between' }}><View style={bleed} />{picker}{head}{foot}</View>;
  const dayName = (i) => (i === 0 ? t('shell.widgets.weather.today') : weekdayName(new Date(Date.now() + i * 86400000).getDay(), true));
  if (w.size === 'm') {
    return (
      <View style={{ flex: 1, flexDirection: 'row', gap: 10 }}>
        <View style={bleed} />
        {picker}
        <View style={{ width: 120, justifyContent: 'space-between' }}>{head}{foot}</View>
        <View style={{ flex: 1, flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' }}>
          {(cur.days || []).slice(1, 6).map((d, i) => (
            <View key={i} style={{ alignItems: 'center', gap: 5 }}>
              <Text style={{ fontSize: 11, fontWeight: 600, color: WX_DIM }}>{dayName(i + 1)}</Text>
              <WxGlyph code={d[0]} size={24} />
              <Text style={{ fontSize: 12, fontWeight: 600, color: WX_TEXT }}>{deg(d[1])}</Text>
              <Text style={{ fontSize: 11, color: WX_DIM }}>{deg(d[2])}</Text>
            </View>
          ))}
        </View>
      </View>
    );
  }
  // l: today, the week as rows with a range bar, and three other cities
  const all = (cur.days || []).flatMap((d) => [d[1], d[2]]);
  const lo = Math.min(...all), hi = Math.max(...all), span = Math.max(1, hi - lo);
  return (
    <View style={{ flex: 1, gap: 8 }}>
      <View style={bleed} />
      {picker}
      <View style={{ flexDirection: 'row', justifyContent: 'space-between' }}>{head}<View style={{ alignItems: 'flex-end' }}>{foot}</View></View>
      <View style={{ gap: 3 }}>
        {(cur.days || []).slice(0, 5).map((d, i) => (
          <View key={i} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, height: 20, direction: 'ltr' }}>
            <Text style={{ width: 52, fontSize: 12, fontWeight: 600, color: WX_TEXT, textAlign: 'start' }}>{dayName(i)}</Text>
            <WxGlyph code={d[0]} size={18} />
            <Text style={{ width: 32, fontSize: 12, color: WX_DIM, textAlign: 'end' }}>{deg(d[2])}</Text>
            <View style={{ flex: 1, height: 4, borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.18)' }}>
              <View style={{ position: 'absolute', left: `${((d[2] - lo) / span) * 100}%`, width: `${Math.max(6, ((d[1] - d[2]) / span) * 100)}%`, height: 4, borderRadius: 2, background: 'linear-gradient(90deg, #7ED1FF, #FFC531)' }} />
            </View>
            <Text style={{ width: 32, fontSize: 12, fontWeight: 600, color: WX_TEXT, textAlign: 'end' }}>{deg(d[1])}</Text>
          </View>
        ))}
      </View>
      <View style={{ flexDirection: 'row', justifyContent: 'space-between', marginTop: 'auto' }}>
        {zones.slice(1).map((z) => {
          const o = data[z];
          return (
            <View key={z} style={{ alignItems: 'center', gap: 2, minWidth: 0, flex: 1 }}>
              <Text style={{ fontSize: 11, color: WX_DIM, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{cityName(z)}</Text>
              {o?.t != null ? <View style={{ flexDirection: 'row', alignItems: 'center', gap: 4 }}><WxGlyph code={o.c} day={o.d} size={16} /><Text style={{ fontSize: 12.5, fontWeight: 600, color: WX_TEXT }}>{deg(o.t)}</Text></View> : <Text style={{ fontSize: 12, color: WX_DIM }}>…</Text>}
            </View>
          );
        })}
      </View>
    </View>
  );
}

// ---- VPN: the ones set up in Settings → Network → VPN, connected with the same closed actions Settings uses ----
const vpnList = () => { try { return lord.net?.vpnList ? lord.net.vpnList() || [] : []; } catch (e) { return []; } };
const vpnSettings = () => wm.open('settings', { args: { section: 'network', page: 'vpn' } });
// what each VPN is doing while this desktop asked: name → { what: 'up'|'down'|'settings'|'error', at, text }
const vpnBusy = createStore({});
let vpnEventsOn = false;
function vpnEvents() {
  if (vpnEventsOn) return;
  vpnEventsOn = true;
  admin.onEvent((text) => {
    const e = eventOf(text);
    if (!e) return;
    const cur = vpnBusy.get()[e.name];
    if (e.what === 'connected' || e.what === 'disconnected') vpnBusy.set((b) => { const n = { ...b }; delete n[e.name]; return n; });
    else if (cur && (e.what === 'need-auth' || e.what === 'sso' || e.what === 'untrusted-cert')) vpnBusy.set((b) => ({ ...b, [e.name]: { what: 'settings', at: Date.now() } }));
    else if (cur && (e.what === 'auth-failed' || e.what === 'error')) vpnBusy.set((b) => ({ ...b, [e.name]: { what: 'error', at: Date.now() } }));
  });
}
function vpnToggle(name, on) {
  vpnEvents();
  vpnBusy.set((b) => ({ ...b, [name]: { what: on ? 'up' : 'down', at: Date.now() } }));
  admin.run(on ? 'net-vpn-up' : 'net-vpn-down', name, undefined, { quiet: true }).then(
    () => { if (!on) vpnBusy.set((b) => { const n = { ...b }; delete n[name]; return n; }); },
    () => vpnBusy.set((b) => ({ ...b, [name]: { what: 'error', at: Date.now() } })));
}
// the VPNs, their interfaces (every 3 s while shown, no root) and this VPN's rates
function useVpn(target) {
  const list = useFeed('vpn.list', vpnList, 10000) || [];
  const busy = useStore(vpnBusy);
  const prev = useRef(null);
  const sample = useEvery(() => {
    const ifs = lord.net?.interfaces ? lord.net.interfaces() : [];
    const v = list.find((x) => x.name === target);
    const st = vpnState(v, ifs), now = Date.now(), p = prev.current;
    prev.current = { ...st, t: now };
    return { ifs, st, rate: p && st.up ? vpnRates(p, st, (now - p.t) / 1000) : null };
  }, 3000, [target, list.length]) || { ifs: [], st: vpnState(null), rate: null };
  useEffect(() => { vpnEvents(); }, []);
  // a request that got no answer in 40 s gives way to Settings (it may be asking for a password there)
  const b = busy[target];
  const stuck = b && (b.what === 'up' || b.what === 'down') && Date.now() - b.at > 40000;
  const upNow = !!(b && b.what === 'up' && sample.st.up);
  useEffect(() => { if (upNow) vpnBusy.set((x) => { const n = { ...x }; delete n[target]; return n; }); }, [upNow, target]);
  return { list, ifs: sample.ifs, st: sample.st, rate: sample.rate, busy: stuck ? { what: 'settings' } : b };
}
// an on/off switch with relief, like a real one: a sunken track, a raised knob with a dimple, ON/OFF on the free side
function ReliefSwitch({ on, busy, onClick, w = 120, h = 52 }) {
  const pad = Math.round(h * 0.09), track = h - 2 * pad, knob = track - 2 * Math.round(h * 0.06);
  const inner = track - knob;
  const left = on ? w - 2 * pad - knob - inner / 2 : inner / 2;
  return (
    <View onClick={busy ? undefined : onClick} style={{ width: w, height: h, borderRadius: h / 2, padding: pad, direction: 'ltr', opacity: busy ? 0.75 : 1,
      background: 'linear-gradient(180deg, rgba(0,0,0,0.22), rgba(255,255,255,0.55))', boxShadow: '0 1px 2px rgba(255,255,255,0.4)' }}>
      <View style={{ flex: 1, borderRadius: track / 2, overflow: 'hidden',
        background: on ? 'linear-gradient(180deg, #23874b, #3fbf6e)' : 'linear-gradient(180deg, #6f7787, #959dac)' }}>
        <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: Math.round(track * 0.35), background: 'linear-gradient(180deg, rgba(0,0,0,0.22), rgba(0,0,0,0))' }} />
        <Text style={{ position: 'absolute', top: 0, bottom: 0, ...(on ? { left: Math.round(track * 0.38) } : { right: Math.round(track * 0.34) }), lineHeight: track,
          fontSize: Math.round(track * 0.42), fontWeight: 500, color: 'rgba(255,255,255,0.88)' }}>{t(on ? 'shell.widgets.vpn.on' : 'shell.widgets.vpn.off')}</Text>
        <View style={{ position: 'absolute', left, top: inner / 2, width: knob, height: knob, borderRadius: knob / 2, alignItems: 'center', justifyContent: 'center',
          background: 'linear-gradient(160deg, #f3f6fb, #c3cad6)', boxShadow: '0 4px 10px rgba(0,0,0,0.35)' }}>
          <View style={{ width: knob * 0.62, height: knob * 0.62, borderRadius: knob * 0.31, border: '1px solid rgba(255,255,255,0.7)',
            background: busy ? 'linear-gradient(160deg, #c3cad6, #f3f6fb)' : 'linear-gradient(340deg, #f3f6fb, #cfd5df)' }} />
        </View>
      </View>
    </View>
  );
}
function VpnStatus({ L, st, busy, center }) {
  const key = busy?.what === 'up' ? 'connecting' : busy?.what === 'down' ? 'disconnecting' : busy?.what === 'error' ? 'failed' : st.up ? 'connected' : 'disconnected';
  const color = key === 'connected' ? L.ok : key === 'failed' ? L.crit : L.dim;
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 5, justifyContent: center ? 'center' : 'flex-start' }}>
      <View style={{ width: 7, height: 7, borderRadius: 4, backgroundColor: color }} />
      <Text style={{ fontSize: 11.5, color: key === 'connected' ? L.text : L.dim }}>{t(`shell.widgets.vpn.${key}`)}</Text>
    </View>
  );
}
function VpnSettingsLink({ L, busy }) {
  if (busy?.what !== 'settings' && busy?.what !== 'error') return null;
  return (
    <View onClick={vpnSettings} style={{ alignSelf: 'flex-start', height: 22, paddingHorizontal: 9, borderRadius: 11, justifyContent: 'center', backgroundColor: L.track }}>
      <Text style={{ fontSize: 11, fontWeight: 600, color: L.text }}>{t('shell.widgets.vpn.settings')}</Text>
    </View>
  );
}
function VpnBody({ w, L }) {
  const list0 = useFeed('vpn.list', vpnList, 10000) || [];
  const target = targetOf(w.opts, list0);
  const { list, st, rate: r, busy } = useVpn(target);
  const v = list.find((x) => x.name === target);
  if (!list.length) {
    return (
      <View style={{ flex: 1, gap: 6 }}>
        <Head L={L} title="VPN" />
        <Quiet L={L} text={t('shell.widgets.vpn.none')} />
        <View onClick={vpnSettings} style={{ alignSelf: 'flex-start', height: 24, paddingHorizontal: 10, borderRadius: 12, justifyContent: 'center', backgroundColor: L.accent, marginTop: 'auto' }}>
          <Text style={{ fontSize: 11.5, fontWeight: 600, color: '#fff' }}>{t('shell.widgets.vpn.add')}</Text>
        </View>
      </View>
    );
  }
  // several VPNs and none chosen for this widget (or a large one): a row per VPN, each with its own switch
  const row = (x) => <VpnRow key={x.name} L={L} vpn={x} />;
  if (w.size === 'l' || !v) {
    return (
      <View style={{ flex: 1, gap: 8 }}>
        <Head L={L} title="VPN" right={!v && w.size !== 'l' ? t('shell.widgets.vpn.choose') : ''} />
        <View style={{ gap: 6 }}>{list.slice(0, w.size === 'l' ? 6 : 2).map(row)}</View>
      </View>
    );
  }
  const on = busy?.what === 'up' ? true : busy?.what === 'down' ? false : st.up;
  const sw = (sz) => <ReliefSwitch on={on} busy={busy?.what === 'up' || busy?.what === 'down'} onClick={() => vpnToggle(v.name, !st.up)} {...sz} />;
  if (w.size === 's') {
    return (
      <View style={{ flex: 1, alignItems: 'center', gap: 7 }}>
        <Text style={{ fontSize: 12.5, fontWeight: 650, color: L.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis', maxWidth: 140 }}>{v.name}</Text>
        {sw({ w: 112, h: 48 })}
        <VpnStatus L={L} st={st} busy={busy} center />
        <Sub L={L} center>{st.up ? st.ip : String(v.type || '').toUpperCase()}</Sub>
      </View>
    );
  }
  return (
    <View style={{ flex: 1, flexDirection: 'row', gap: 14 }}>
      <View style={{ flex: 1, gap: 5, minWidth: 0 }}>
        <Text style={{ fontSize: 13, fontWeight: 650, color: L.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{v.name}</Text>
        <VpnStatus L={L} st={st} busy={busy} />
        {st.up ? <Line L={L} label="IP" value={st.ip || '…'} /> : <Sub L={L}>{String(v.type || '').toUpperCase()}</Sub>}
        {st.up ? (
          <View style={{ flexDirection: 'row', gap: 12 }}>
            <View><Sub L={L}>↓ {t('shell.widgets.network.down')}</Sub><Text style={{ fontSize: 14, fontWeight: 600, color: L.accent }}>{r ? rate(r.rx) : '…'}</Text></View>
            <View><Sub L={L}>↑ {t('shell.widgets.network.up')}</Sub><Text style={{ fontSize: 14, fontWeight: 600, color: L.ok }}>{r ? rate(r.tx) : '…'}</Text></View>
          </View>
        ) : <VpnSettingsLink L={L} busy={busy} />}
      </View>
      <View style={{ justifyContent: 'center' }}>{sw({ w: 120, h: 52 })}</View>
    </View>
  );
}
function VpnRow({ L, vpn }) {
  const { st, busy } = useVpn(vpn.name);
  const on = busy?.what === 'up' ? true : busy?.what === 'down' ? false : st.up;
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, minHeight: 40 }}>
      <View style={{ flex: 1, minWidth: 0, gap: 1 }}>
        <Text style={{ fontSize: 12.5, fontWeight: 650, color: L.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{vpn.name}</Text>
        <Text style={{ fontSize: 11, color: st.up ? L.ok : L.dim, textAlign: 'start' }}>{st.up ? st.ip || t('shell.widgets.vpn.connected') : t(busy?.what === 'up' ? 'shell.widgets.vpn.connecting' : busy?.what === 'error' ? 'shell.widgets.vpn.failed' : 'shell.widgets.vpn.disconnected')}</Text>
      </View>
      <ReliefSwitch on={on} busy={busy?.what === 'up' || busy?.what === 'down'} onClick={() => vpnToggle(vpn.name, !st.up)} w={78} h={34} />
    </View>
  );
}

// ---- timer and stopwatch (the Clock app's own, sdk/alarms.js: they keep running and ring with the window closed) ----
const mmss = (ms) => {
  const s = Math.ceil(Math.max(0, ms) / 1000), h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), x = s % 60;
  return h ? `${h}:${String(m).padStart(2, '0')}:${String(x).padStart(2, '0')}` : `${m}:${String(x).padStart(2, '0')}`;
};
// a tick every second only while something runs (idle: no timer at all)
function useRunningTick(running) {
  const [, set] = useState(0);
  useEffect(() => { if (!running) return undefined; const id = setInterval(() => set((x) => x + 1), 1000); return () => clearInterval(id); }, [running]);
}
// The layout comes from sdk/timerwidgetmodel.js: the ring, the fonts and every pill's width are picked to fit the card
// in every language (no row ever wraps; a label too long for the room is cut with an ellipsis).
const textWidth = (s, size, weight) => (typeof lord !== 'undefined' && lord.measureText ? lord.measureText(String(s), size, weight) : String(s).length * size * 0.62);
function FitPills({ L, row, items }) {
  return (
    <View style={{ flexDirection: 'row', gap: row.style.gap, justifyContent: 'center' }}>
      {row.keep.map((i, k) => {
        const it = items[i];
        return (
          <View key={it.key} onClick={it.onClick} style={{ width: row.widths[k], height: row.style.h, borderRadius: row.style.h / 2, paddingHorizontal: row.style.pad, justifyContent: 'center', backgroundColor: it.primary ? L.accent : L.track }}>
            <Text style={{ fontSize: row.style.font, fontWeight: 600, color: it.primary ? '#ffffff' : L.text, textAlign: 'center', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{it.label}</Text>
          </View>
        );
      })}
    </View>
  );
}
function TimerBody({ w, L }) {
  const tm = useStore(clockTimer), sw = useStore(stopwatch);
  useRunningTick(tm.state === 'running' || sw.running);
  const two = w.size !== 's';
  const left = tm.state === 'idle' ? idleSeconds(tm) * 1000 : timerLeft(tm), frac = tm.state !== 'idle' && tm.duration ? 1 - left / tm.duration : 0;
  const tItems = tm.state === 'idle'
    ? PRESETS.map((n) => ({ key: `p${n}`, label: t('clock.timer.preset', { n }), onClick: () => timerStart(n * 60) }))
    : [{ key: 'pause', primary: true, label: t(tm.state === 'running' ? 'clock.timer.pause' : 'clock.timer.resume'), onClick: tm.state === 'running' ? timerPause : timerResume },
      { key: 'cancel', label: t('clock.cancel'), onClick: timerReset }];
  const sItems = [{ key: 'run', primary: true, label: t(sw.running ? 'clock.timer.pause' : 'clock.timer.start'), onClick: sw.running ? swStop : swStart },
    { key: 'reset', label: t('shell.widgets.timer.reset'), onClick: swReset }];
  const swText = mmss(swElapsed(sw)), swTitle = t('shell.widgets.timer.stopwatch');
  const lay = timerLayout({ size: w.size, w: w.w || (two ? 356 : 170), h: w.h || 170, measure: textWidth,
    timer: { text: mmss(left), labels: tItems.map((i) => i.label), drop: tm.state === 'idle' ? PRESET_DROP : [] },
    stopwatch: { title: swTitle, text: swText, labels: sItems.map((i) => i.label) } });
  // tap the ring: start the last duration (idle), pause, resume
  const ringTap = tm.state === 'idle' ? () => timerStart(idleSeconds(tm)) : tm.state === 'running' ? timerPause : timerResume;
  const timer = (
    <View style={{ width: lay.cols[0].w, alignItems: 'center', justifyContent: 'center', gap: lay.timer.gap }}>
      <View onClick={ringTap}>
        <TickRing L={L} size={lay.timer.ring} value={frac} color={tm.state === 'paused' ? L.warn : L.accent}>
          <Text style={{ fontSize: lay.timer.ringFont, fontWeight: 600, color: tm.state === 'idle' ? L.dim : L.text, whiteSpace: 'nowrap' }}>{mmss(left)}</Text>
        </TickRing>
      </View>
      <FitPills L={L} row={lay.timer.row} items={tItems} />
    </View>
  );
  if (!two) return <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center' }}>{timer}</View>;
  return (
    <View style={{ flex: 1, flexDirection: 'row', gap: lay.colGap }}>
      {timer}
      <View style={{ width: 1, alignSelf: 'stretch', backgroundColor: L.sep }} />
      <View style={{ width: lay.cols[1].w, alignItems: 'center', justifyContent: 'center', gap: lay.stopwatch.gap }}>
        <Sub L={L} center>{swTitle}</Sub>
        <Text onClick={sw.running ? swStop : swStart} style={{ fontSize: lay.stopwatch.figure, fontWeight: 600, color: L.text, whiteSpace: 'nowrap' }}>{swText}</Text>
        <FitPills L={L} row={lay.stopwatch.row} items={sItems} />
      </View>
    </View>
  );
}

// ---- recent files (lord.fs.recent: the ones opened lately, as Files' Recents) ----
const extOf = (n) => { const m = /\.([A-Za-z0-9]{1,5})$/.exec(String(n || '')); return m ? m[1].toUpperCase() : ''; };
function RecentBody({ w, L }) {
  const n = w.size === 'l' ? 8 : 3;
  useStore(recentFiles);                            // a new walk's answer (it runs on lord-server's workers) draws again
  useEvery(() => { fsRecent(n, 30); return 0; }, 60000, [n]);   // a new walk asked for each minute
  let list = [];
  try { list = fsRecent(n, 30) || []; } catch (e) { list = []; }
  return (
    <View style={{ flex: 1, gap: 6 }}>
      <Head L={L} title={t('shell.widgets.recent.name')} />
      {!list.length ? <Quiet L={L} text={t('shell.recent.none')} /> : list.slice(0, n).map((f) => {
        const name = f.name || basename(f.path);
        return (
          <View key={f.path} onClick={() => openFileWithApps(f.path, name)} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, minHeight: 24 }}>
            <View style={{ width: 30, height: 20, borderRadius: 5, backgroundColor: L.track, alignItems: 'center', justifyContent: 'center' }}>
              <Text style={{ fontSize: 8.5, fontWeight: 700, color: L.dim }}>{extOf(name) || '•'}</Text>
            </View>
            <Text style={{ flex: 1, fontSize: 12.5, color: L.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{name}</Text>
          </View>
        );
      })}
    </View>
  );
}

// ---- updates: LordOS's own package and the apps' (the signed lists' last check; nothing is fetched here) ----
function UpdatesBody({ w, L }) {
  const st = useEvery(() => {
    let os = null, apps = 0, restart = null;
    try { os = lordosUpdate(); } catch (e) { os = null; }
    try { apps = storeUpdates(); } catch (e) { apps = 0; }
    try { restart = restartNeeded(); } catch (e) { restart = null; }
    return { os, apps, restart };
  }, 600000) || {};
  const ok = !st.os?.available && !st.apps && !st.restart;
  const open = () => wm.open('store', { args: { page: 'updates' } });
  return (
    <View style={{ flex: 1, gap: 6 }}>
      <Head L={L} title={t('shell.widgets.updates.name')} right={st.os?.installed ? `LordOS ${String(st.os.installed).split('~')[0]}` : ''} />
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        <View style={{ width: 26, height: 26, borderRadius: 13, alignItems: 'center', justifyContent: 'center', backgroundColor: ok ? L.ok : L.warn }}>
          <Text style={{ fontSize: 14, fontWeight: 700, color: '#fff' }}>{ok ? '✓' : '↑'}</Text>
        </View>
        <Text style={{ flex: 1, fontSize: 13, fontWeight: 600, color: L.text, textAlign: 'start' }}>
          {st.restart ? t('shell.widgets.updates.restart') : st.os?.available ? t('shell.widgets.updates.available', { version: String(st.os.version).split('~')[0] }) : t('shell.widgets.updates.upToDate')}
        </Text>
      </View>
      {st.apps ? <Sub L={L}>{t('shell.widgets.updates.apps', { n: st.apps })}</Sub> : null}
      {!ok || w.size !== 's' ? <View style={{ marginTop: 'auto' }}><Pill L={L} primary={!ok} label={t('settings.updates.open')} onClick={open} /></View> : null}
    </View>
  );
}

// ---- app launcher: 4 or 8 apps (the widget's own choice, else the Dock's pins) ----
let launcherApps = [];    // the last list a launcher drew: its right-click menu offers these
function LauncherBody({ w, L }) {
  const all = useLauncherApps();
  launcherApps = all;
  const n = w.size === 'l' ? 8 : 4;
  const chosen = Array.isArray(w.opts?.apps) ? w.opts.apps : null;
  let pins = [];
  try { pins = dock.pinned() || []; } catch (e) { pins = []; }
  const byId = (id) => all.find((a) => a.id === id) || all.find((a) => dockIdOf(a.id) === id);
  const list = (chosen || pins).map(byId).filter(Boolean).slice(0, n);
  const size = w.size === 's' ? 40 : 46;
  return (
    <View style={{ flex: 1, flexDirection: 'row', flexWrap: 'wrap', alignContent: 'center', justifyContent: 'space-around', rowGap: 10 }}>
      {list.map((a) => (
        <View key={a.id} onClick={() => launchApp(a.id)} style={{ width: w.size === 's' ? '45%' : '24%', alignItems: 'center', gap: 4 }}>
          {a.Icon ? <a.Icon size={size} /> : null}
          {w.size !== 's' ? <Text style={{ fontSize: 11, color: L.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis', maxWidth: 76, textAlign: 'center' }}>{a.name}</Text> : null}
        </View>
      ))}
      {!list.length ? <Quiet L={L} text={t('shell.widgets.launcher.empty')} /> : null}
    </View>
  );
}

// ---- video: cameras, streams and TV channels (sdk/videowidget.jsx, lord-stream) ----
// ---- the sound wave: lines that follow the desktop's sound, one app's or the microphone's (drawn in C) ----
const FRAMELESS = new Set(['soundwave']);
// a new sound wave starts centred along the bottom of its display, without a card
const NEW_OPTS = { soundwave: { pin: 'bottom-center' } };
const waveFrame = (w) => (w.opts?.frame === 'card' ? 'card' : 'none');   // no card by default: part of the wallpaper
function SoundWaveBody({ w, L }) {
  const th = useTheme() || {};
  const src = sourceOf(w.opts);
  useSpectrumSource(src);
  const look = { lines: w.opts?.lines === 'few' ? 8 : w.opts?.lines === 'many' ? 22 : 16,
    colors: colorsOf(w.opts?.palette || 'neon', th.accent), gain: gainOf(w.opts?.sensitivity), hideIdle: w.opts?.idle !== 'line' };
  const wave = <SoundWave wave={look} style={{ flex: 1 }} />;
  if (waveFrame(w) === 'none') return wave;
  const name = src.kind === 'mic' ? t('shell.widgets.soundwave.mic') : src.kind === 'app' ? src.label : t('shell.widgets.soundwave.desktop');
  return <View style={{ flex: 1 }}><Head L={L} title={t('shell.widgets.soundwave.name')} right={name} />{wave}</View>;
}
function soundWaveMenu(w) {
  const set = (p) => setDeskWidget(w.id, { opts: p });
  const src = sourceOf(w.opts);
  const apps = appsPlaying(lord.sound?.state ? lord.sound.state() : null);
  if (src.kind === 'app' && !apps.some((a) => a.key === src.app)) apps.unshift({ key: src.app, label: src.label });
  const pick = (v, cur, key) => ({ checked: v === cur, onClick: () => set({ [key]: v }) });
  return [
    { label: t('shell.widgets.soundwave.source'), submenu: [
      { label: t('shell.widgets.soundwave.desktop'), checked: src.kind === 'desktop', onClick: () => set({ source: { kind: 'desktop' } }) },
      { label: t('shell.widgets.soundwave.mic'), checked: src.kind === 'mic', onClick: () => set({ source: { kind: 'mic' } }) },
      ...(apps.length ? ['-', ...apps.map((a) => ({ label: a.label, checked: src.kind === 'app' && src.app === a.key,
        onClick: () => set({ source: { kind: 'app', app: a.key, label: a.label } }) }))] : [{ label: t('shell.widgets.soundwave.noApps'), disabled: true }]),
    ] },
    { label: t('shell.widgets.soundwave.style'), submenu: [
      { label: t('shell.widgets.soundwave.frameNone'), ...pick('none', waveFrame(w), 'frame') },
      { label: t('shell.widgets.soundwave.frameCard'), ...pick('card', waveFrame(w), 'frame') },
    ] },
    { label: t('shell.widgets.soundwave.colors'), submenu: PALETTE_ORDER.map((p) => ({ label: t(`shell.widgets.soundwave.palette.${p}`), ...pick(p, w.opts?.palette || 'neon', 'palette') })) },
    { label: t('shell.widgets.soundwave.lines'), submenu: ['few', 'normal', 'many'].map((v) => ({ label: t(`shell.widgets.soundwave.lines.${v}`), ...pick(v, w.opts?.lines || 'normal', 'lines') })) },
    { label: t('shell.widgets.soundwave.idle'), submenu: ['hide', 'line'].map((v) => ({ label: t(`shell.widgets.soundwave.idle.${v}`), ...pick(v, w.opts?.idle || 'hide', 'idle') })) },
    { label: t('shell.widgets.soundwave.sensitivity'), submenu: ['low', 'normal', 'high'].map((v) => ({ label: t(`shell.widgets.soundwave.sensitivity.${v}`), ...pick(v, w.opts?.sensitivity || 'normal', 'sensitivity') })) },
  ];
}

const VideoKind = ({ w }) => <VideoBody w={w} setOpts={(p) => setDeskWidget(w.id, { opts: p })} />;

// ---- the kinds: their sizes (the first is the default), bodies and menu entries ----
const BODIES = { video: VideoKind, timer: TimerBody, recent: RecentBody, updates: UpdatesBody, launcher: LauncherBody, vpn: VpnBody, weather: WeatherBody, clock: ClockBody, calendar: CalendarBody, system: SystemBody, network: NetworkBody, disks: DisksBody, temps: TempsBody,
  battery: BatteryBody, devices: DevicesBody, reminders: RemindersBody, clipboard: ClipboardBody, claude: ClaudeBody, nowplaying: NowPlayingBody, readaloud: ReadAloudBody,
  soundwave: SoundWaveBody };
// The order the kinds are painted in (widgets.js draws the types in the order they were first published): the ones
// that never change on their own first, the ones that do (a clock, a reading every few seconds, a video, the sound wave)
// last, the busiest at the end. Each card's frosted glass blurs what is painted under it within ~45 px (cards sit 16 px
// apart): a number changing in a card painted above its neighbours never makes them blur again (paint.c's blur cache),
// so changes stay inside their own card (0.8.8.10). Kinds not listed go between the two groups.
const LIVE_KINDS = ['clock', 'timer', 'vpn', 'battery', 'temps', 'disks', 'system', 'network', 'claude', 'nowplaying', 'video', 'soundwave'];
const PAINT_ORDER = [...Object.keys(BODIES).filter((k) => !LIVE_KINDS.includes(k)), ...LIVE_KINDS.filter((k) => k in BODIES)];
const KIND_SIZES = { video: ['m', 's', 'l', 'xl'], timer: ['s', 'm'], recent: ['m', 'l'], updates: ['s', 'm'], launcher: ['s', 'm', 'l'], vpn: ['s', 'm', 'l'], weather: ['s', 'm', 'l'], clock: ['s', 'm', 'l', 'w'], calendar: ['s', 'm', 'l'], system: ['s', 'm', 'l'], network: ['s', 'm'], disks: ['s', 'm', 'l'],
  temps: ['s', 'm', 'l'], battery: ['s', 'm'], devices: ['m', 's', 'l'], reminders: ['s', 'm', 'l'], clipboard: ['m', 'l'], claude: ['s', 'm', 'l'], nowplaying: ['m'], readaloud: ['s', 'm'],
  soundwave: ['w', 'm', 'l', 'xl'] };
export const kindSizes = (type) => KIND_SIZES[type] || null;
const KIND_MENU = {
  soundwave: soundWaveMenu,
  calendar: calendarMenu,
  video: (w) => [
    { label: t('shell.widgets.video.setup'), onClick: () => videoSetup.set({ widget: w, set: (p) => setDeskWidget(w.id, { opts: p }) }) },
    { label: t(w.opts?.muted === false ? 'shell.widgets.video.mute' : 'shell.widgets.video.unmute'), onClick: () => setDeskWidget(w.id, { opts: { muted: w.opts?.muted !== false ? false : true } }) },
    { label: t(w.opts?.paused ? 'shell.widgets.video.play' : 'shell.widgets.video.pause'), onClick: () => setDeskWidget(w.id, { opts: { paused: !w.opts?.paused } }) },
  ],
  timer: () => [{ label: t('shell.widgets.timer.openClock'), onClick: () => wm.open('clock') }],
  recent: () => [{ label: t('apps.files'), onClick: () => wm.open('files') }],
  // which apps: a check per app (the Dock's pins until chosen), at most 8
  launcher: (w) => {
    const apps = launcherApps;
    const cur = Array.isArray(w.opts?.apps) ? w.opts.apps : null;
    return [{ label: t('shell.widgets.launcher.choose'), submenu: apps.slice(0, 40).map((a) => {
      const on = cur ? cur.includes(a.id) : false;
      return { label: a.name, checked: on, onClick: () => setDeskWidget(w.id, { opts: { apps: on ? cur.filter((x) => x !== a.id) : [...(cur || []), a.id].slice(-8) } }) };
    }) }];
  },
  // which VPN this widget drives: one widget per VPN, as many as wanted
  vpn: (w) => {
    const list = vpnList(), cur = targetOf(w.opts, list);
    return [
      ...(list.length > 1 ? [{ label: t('shell.widgets.vpn.choose'), submenu: list.map((v) => ({ label: v.name, checked: v.name === cur, onClick: () => setDeskWidget(w.id, { opts: { vpn: v.name } }) })) }] : []),
      { label: t('shell.widgets.vpn.settings'), onClick: vpnSettings },
    ];
  },
  weather: (w) => [
    { label: t('shell.widgets.weather.changeCity'), onClick: () => wxPick.set(w.id) },
    { label: '°C', checked: wxUnit() === 'c', onClick: () => settings.set({ weather: { ...(settings.get().weather || {}), unit: 'c' } }) },
    { label: '°F', checked: wxUnit() === 'f', onClick: () => settings.set({ weather: { ...(settings.get().weather || {}), unit: 'f' } }) },
    // the large one's row of other cities is the Clock's
    ...(w.size === 'l' ? [{ label: t('shell.widgets.weather.otherCities'), onClick: () => wm.open('clock') }] : []),
    ...(wxOn() ? [{ label: t('shell.widgets.weather.turnOff'), onClick: () => settings.set({ weather: { ...(settings.get().weather || {}), on: false } }) }] : []),
  ],
  clock: (w) => [
    { label: t('shell.widgets.clock.analog'), checked: w.opts?.style !== 'digital' && w.opts?.style !== 'analog-seconds', onClick: () => setDeskWidget(w.id, { opts: { style: 'analog' } }) },
    { label: t('shell.widgets.clock.analogSeconds'), checked: w.opts?.style === 'analog-seconds', onClick: () => setDeskWidget(w.id, { opts: { style: 'analog-seconds' } }) },
    { label: t('shell.widgets.clock.digital'), checked: w.opts?.style === 'digital', onClick: () => setDeskWidget(w.id, { opts: { style: 'digital' } }) },
    { label: t('shell.widgets.clock.cities'), onClick: () => wm.open('clock') },
  ],
};
for (const type of Object.keys(BODIES)) {
  registerWidgetKind({ type, nameKey: `shell.widgets.${type}.name`, descKey: `shell.widgets.${type}.desc`, sizes: KIND_SIZES[type],
    ...(type === 'battery' ? { available: () => !!battery.get()?.hasBattery } : null) });
}
