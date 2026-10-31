// overview.jsx — the Overview, the same in every theme (docs/DESKTOPS.md): the windows of the desktop
// in view in a grid and, along the top, a strip with every desktop. Click a window to go to it, drag it
// to a desktop in the strip (or to the add button) to move it, add and remove desktops there. With
// Ctrl+Down it shows only the windows of the app in front, with no strip.
//
// The windows are the real ones drawn smaller: WindowShell gives each one a transform (lord-server paints
// it from a cached raster, so no app redraws) and what you see is live. Other desktops in the strip are
// miniatures of their windows' frames with the app icon, over the wallpaper: their windows aren't drawn.
//
// A theme mounts <OverviewBackdrop /> below its windows and <OverviewChrome /> above them, inside its
// ThemeProvider. Nothing here runs unless it is open.
import { createElement as h, useEffect } from 'react';
import { View, Text, Image, addListener, mainDisplay } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { t, direction } from './i18n.js';
import { useTheme } from './theme.js';
import { Layer } from './layers.js';
import { settings } from './settings.js';
import { windows, wm, getApp, workAreaOf, windowTitle, activeDesktop, desktopList, onActiveDesktop, overviewShowing, setActiveDesktop, createDesktop, closeDesktop } from './windows.js';
import { MAX_DESKTOPS, onDesktop, gridLayout, stripLayout, hitRect, OVERVIEW_LABEL } from './desktopmodel.js';
import { launcherIdOf } from './dock.js';
import { windowIcon } from './apps.js';
import { findWallpaper, wallpaperThumb } from './wallpapers.js';
import { clearTextFocus } from './components.jsx';

const DRAG_START = 6;
const own = (w) => !(getApp(w.id)?.builtin && !String(w.id).startsWith('linux#'));   // not the theme's panels

// null, or { app, list, places: Map id → { x, y, scale, w, h }, strip, area }
export const overview = createStore(null);
const hoverStore = createStore(null);      // the window under the pointer or the keyboard
const dragStore = createStore(null);       // { id, x, y, over } while a window is dragged
const cardHover = createStore(null);       // the card showing its remove button
let press = null;                          // { id, x, y } the button went down on a window

function compute(app) {
  const list = windows.get().filter((w) => own(w) && !w.fullscreen && !w.pip && onActiveDesktop(w) && (!app || launcherIdOf(w) === app));
  const area = workAreaOf(mainDisplay());
  const d = mainDisplay();
  const strip = app ? null : stripLayout(desktopList.get(), area, { aspect: d.w / Math.max(1, d.h), canAdd: desktopList.get().length < MAX_DESKTOPS, rtl: direction() === 'rtl' });
  const top = strip ? strip.h : 14;
  return { app, list, strip, area, places: gridLayout(list, { x: area.x, y: area.y + top, w: area.w, h: area.h - top }) };
}
const refresh = () => {
  const o = overview.get();
  if (o) overview.set(compute(o.app));
};
windows.subscribe(refresh);
activeDesktop.subscribe(refresh);
desktopList.subscribe(refresh);

// A fullscreen Linux app is presented above every layer: the overview wouldn't be seen.
const blocked = () => typeof lord !== 'undefined' && !!lord.wayland?.fullscreen?.();

export function openOverview({ app = null } = {}) {
  if (blocked()) return;
  clearTextFocus();                       // its keys are the overview's
  overviewShowing.set(true);
  overview.set(compute(app));
}
export function closeOverview() {
  if (!overview.get()) return;
  overview.set(null);
  overviewShowing.set(false);
  press = null;
  hoverStore.set(null);
  dragStore.set(null);
  cardHover.set(null);
}
export function toggleOverview() { if (overview.get()) closeOverview(); else openOverview(); }
export const overviewOpen = () => !!overview.get();
// Only the windows of the app in front.
export function toggleAppWindows() {
  if (overview.get()) { closeOverview(); return; }
  const w = wm.get(wm.focused());
  if (w) openOverview({ app: launcherIdOf(w) });
}

function pick(id) {
  closeOverview();
  wm.focus(id);
}

// What the pointer is over while dragging a window: a desktop, or 'add'.
function dropAt(x, y) {
  const s = overview.get()?.strip;
  if (!s) return null;
  if (hitRect(s.add, x, y)) return 'add';
  return s.cards.find((c) => hitRect(c, x, y))?.id ?? null;
}
function drop(id, over) {
  const d = over === 'add' ? createDesktop() : over;
  if (d != null) wm.moveToDesktop(id, d);
}

// The cover WindowShell puts over a window in the overview.
export function OverviewCover({ id, scale, radius }) {
  const th = useTheme();
  const hovered = useStore(hoverStore) === id;
  return h(View, {
    onMouseDown: (e) => { if (!e.button) press = { id, x: e.x, y: e.y }; },
    onMouseEnter: () => hoverStore.set(id),
    onMouseLeave: () => hoverStore.set((v) => (v === id ? null : v)),
    style: { position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, cursor: 'pointer', borderRadius: radius,
      border: hovered ? `${Math.round(3 / scale)}px solid ${th.accent}` : '0 solid transparent' },
  });
}
// The window's place in the overview, or null. WindowShell reads it.
export function useOverviewPlace(id) {
  return useStore(overview, (o) => o?.places.get(id) || null);
}

export function OverviewBackdrop() {
  const o = useStore(overview);
  if (!o) return null;
  const { area } = o;
  return h(View, { onClick: closeOverview, style: { position: 'absolute', left: area.x, top: area.y, width: area.w, height: area.h,
    backgroundColor: 'rgba(8,10,16,0.45)', alignItems: 'center', justifyContent: 'center' } },
  o.list.length ? null : h(Text, { style: { fontSize: 15, color: '#fff' } }, t('shell.desktops.empty')));
}

// The title above each window, with a close button.
function Labels({ o, th }) {
  const hover = useStore(hoverStore);
  return o.list.map((w) => {
    const p = o.places.get(w.id);
    if (!p) return null;
    const Icon = windowIcon(w, getApp);
    const on = hover === w.id;
    return h(View, { key: w.id, onMouseEnter: () => hoverStore.set(w.id),
      style: { position: 'absolute', left: p.x, top: p.y - OVERVIEW_LABEL, width: p.w, height: OVERVIEW_LABEL - 4, flexDirection: 'row', alignItems: 'center', gap: 8,
        paddingHorizontal: 6, borderRadius: 6, backgroundColor: on ? th.windowBg : 'transparent' } },
    Icon ? h(Icon, { size: 16 }) : null,
    h(Text, { style: { flex: 1, fontSize: 12, color: on ? th.text : '#ffffff', whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' } }, windowTitle(w)),
    on ? h(View, { onClick: () => wm.close(w.id), style: { width: 22, height: 22, borderRadius: 4, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(196,43,28,0.9)' } },
      h(Text, { style: { fontSize: 13, color: '#fff', lineHeight: 22 } }, '×')) : null);
  });
}

// One desktop in the strip: the wallpaper and its windows as small frames.
function Card({ c, index, th, active, all, wallpaper, over, canRemove }) {
  const hovered = useStore(cardHover) === c.id;
  const d = mainDisplay();
  const k = c.w / Math.max(1, d.w);
  const mine = all.filter((w) => !w.minimized && !w.pip && !w.fullscreen && own(w) && onDesktop(w, c.id));
  return h(View, { key: c.id, onClick: () => { if (active) closeOverview(); else setActiveDesktop(c.id); },
    onMouseEnter: () => cardHover.set(c.id), onMouseLeave: () => cardHover.set((v) => (v === c.id ? null : v)),
    style: { position: 'absolute', left: c.x, top: c.y, width: c.w, height: c.h + 24, alignItems: 'center' } },
  h(View, { style: { width: c.w, height: c.h, borderRadius: 8, overflow: 'hidden', backgroundColor: th.windowBg,
    border: `2px solid ${over ? th.accent : active ? '#ffffff' : 'rgba(255,255,255,0.25)'}` } },
  wallpaper ? h(Image, { src: wallpaper, style: { position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, objectFit: 'cover' } }) : null,
  mine.map((w) => {
    const r = wm.frame(w), Icon = windowIcon(w, getApp);
    const mw = Math.max(10, r.w * k), mh = Math.max(8, r.h * k);
    return h(View, { key: w.id, style: { position: 'absolute', left: (r.x - d.x) * k, top: (r.y - d.y) * k, width: mw, height: mh, borderRadius: 3, alignItems: 'center', justifyContent: 'center',
      backgroundColor: th.windowBg, border: `1px solid ${th.separator}` } },
    Icon && mw >= 22 && mh >= 22 ? h(Icon, { size: Math.min(20, mh - 6) }) : null);
  })),
  h(Text, { style: { marginTop: 4, fontSize: 12, fontWeight: active ? 600 : 400, color: '#ffffff', whiteSpace: 'nowrap' } }, t('shell.desktops.name', { n: index + 1 })),
  hovered && canRemove ? h(View, { onClick: (e) => { e.stopPropagation?.(); closeDesktop(c.id); }, style: { position: 'absolute', top: 4, [direction() === 'rtl' ? 'left' : 'right']: 4, width: 20, height: 20, borderRadius: 10,
    alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.65)' } },
  h(Text, { style: { fontSize: 13, color: '#fff', lineHeight: 20 } }, '×')) : null);
}

function Strip({ o, th }) {
  const drag = useStore(dragStore);
  const all = useStore(windows);
  const active = useStore(activeDesktop);
  const s = settings.get();
  const wall = wallpaperThumb(findWallpaper(th.wallpapers, s.wallpaper));
  const { strip } = o;
  return h(View, { style: { position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, pointerEvents: 'box-none' } },
    strip.cards.map((c, i) => h(Card, { key: c.id, c, index: i, th, all, active: c.id === active, wallpaper: wall, over: drag?.over === c.id, canRemove: strip.cards.length > 1 })),
    strip.add ? h(View, { onClick: () => createDesktop(), style: { position: 'absolute', left: strip.add.x, top: strip.add.y, width: strip.add.w, height: strip.add.h, borderRadius: 8,
      alignItems: 'center', justifyContent: 'center', backgroundColor: drag?.over === 'add' ? th.accent : 'rgba(255,255,255,0.14)', border: '1px solid rgba(255,255,255,0.25)' } },
    h(Text, { style: { fontSize: 22, color: '#fff' } }, '+')) : null);
}

// What is being dragged: the app's icon and title, under the pointer.
function Ghost({ th }) {
  const d = useStore(dragStore);
  const w = d && wm.get(d.id);
  if (!w) return null;
  const Icon = windowIcon(w, getApp);
  return h(View, { style: { position: 'absolute', left: d.x + 10, top: d.y + 10, maxWidth: 260, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 10, height: 34,
    borderRadius: 8, pointerEvents: 'none', backgroundColor: th.windowBg, border: `1px solid ${th.separator}`, boxShadow: '0 10px 28px rgba(0,0,0,0.4)' } },
  Icon ? h(Icon, { size: 18 }) : null,
  h(Text, { style: { fontSize: 12, color: th.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis' } }, windowTitle(w)));
}

export function OverviewChrome() {
  const o = useStore(overview);
  const th = useTheme();
  const open = !!o;
  useEffect(() => {
    if (!open) return undefined;
    const offKey = addListener('keydown', (e) => {
      if (e.key === 'Escape') { closeOverview(); return; }
      const list = overview.get()?.list || [];
      if (!list.length) return;
      const i = Math.max(0, list.findIndex((w) => w.id === hoverStore.get()));
      if (e.key === 'ArrowRight' || e.key === 'Tab') hoverStore.set(list[(i + 1) % list.length].id);
      else if (e.key === 'ArrowLeft') hoverStore.set(list[(i - 1 + list.length) % list.length].id);
      else if (e.key === 'Enter' && hoverStore.get()) pick(hoverStore.get());
    });
    const offMove = addListener('pointermove', (e) => {
      if (!press) return;
      if (!dragStore.get() && Math.abs(e.x - press.x) + Math.abs(e.y - press.y) < DRAG_START) return;
      dragStore.set({ id: press.id, x: e.x, y: e.y, over: dropAt(e.x, e.y) });
    });
    const offUp = addListener('pointerup', (e) => {
      const p = press, d = dragStore.get();
      press = null;
      if (d) { dragStore.set(null); drop(d.id, dropAt(e.x, e.y)); } else if (p) pick(p.id);
    });
    return () => { offKey(); offMove(); offUp(); };
  }, [open]);
  if (!o) return null;
  return [
    h(Layer, { key: 'chrome', name: 'overlay' },
      h(View, { style: { position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, pointerEvents: 'box-none' } },
        o.strip ? h(Strip, { o, th }) : null,
        h(Labels, { o, th }))),
    h(Layer, { key: 'drag', name: 'drag' }, h(Ghost, { th })),
  ];
}
