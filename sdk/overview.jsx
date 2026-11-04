// overview.jsx — the Overview, the same in every theme (docs/DESKTOPS.md): the windows of the desktop
// in view in a grid and, along the top, a strip with every desktop. Click a window to go to it, drag it
// to a desktop in the strip (or to the add button) to move it, add and remove desktops there, drag a
// desktop along the strip to reorder and double-click its name (or right-click it) to rename it. With
// Ctrl+Down it shows only the windows of the app in front, with no strip.
//
// The windows are the real ones drawn smaller: WindowShell gives each one a transform (lord-server paints
// it from a cached raster, so no app redraws) and what you see is live. Other desktops in the strip are
// miniatures of their windows' frames with the app icon, over the wallpaper: their windows aren't drawn.
//
// A theme mounts <OverviewBackdrop /> below its windows and <OverviewChrome /> above them, inside its
// ThemeProvider. Nothing here runs unless it is open.
import { createElement as h, useEffect, useState } from 'react';
import { View, Text, Image, addListener, mainDisplay } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { t, direction } from './i18n.js';
import { useTheme, popupBg } from './theme.js';
import { Layer } from './layers.js';
import { settings } from './settings.js';
import { windows, wm, getApp, workAreaOf, windowTitle, activeDesktop, desktopList, desktopNames, desktopName, renameDesktop, moveDesktop, onActiveDesktop, overviewShowing,
  setActiveDesktop, createDesktop, closeDesktop } from './windows.js';
import { MAX_DESKTOPS, onDesktop, gridLayout, stripLayout, stripSlot, moveDesktopTo, hitRect, overviewPlaceholder, OVERVIEW_LABEL } from './desktopmodel.js';
import { launcherIdOf } from './dock.js';
import { windowIcon } from './apps.js';
import { findWallpaper, wallpaperThumb } from './wallpapers.js';
import { clearTextFocus, TextInput } from './components.jsx';

const DRAG_START = 6;
const own = (w) => !(getApp(w.id)?.builtin && !String(w.id).startsWith('linux#'));   // not the theme's panels

// null, or { app, list, places: Map id → { x, y, scale, w, h }, strip, area }
export const overview = createStore(null);
const hoverStore = createStore(null);      // the window under the pointer or the keyboard
const dragStore = createStore(null);       // { id, x, y, over } while a window is dragged
const cardHover = createStore(null);       // the card showing its remove button
const cardDrag = createStore(null);        // { id, x, slot, grab } while a desktop is dragged along the strip
const renaming = createStore(null);        // { id, value } the desktop whose name is being edited
const cardMenu = createStore(null);        // { id, x, y } a desktop's right-click menu
let press = null;                          // { id, x, y } the button went down on a window
let cardPress = null;                      // { id, x, y, grab } the button went down on a desktop
let cardDropAt = 0;                        // when a desktop was dropped: the click that follows is not a click
let renameKey = null;                      // the last key the name field took (not the Overview's)
let fieldBox = null, menuBox = null;       // the name field's and the menu's nodes

const stripFor = (list, area) => {
  const d = mainDisplay();
  return stripLayout(list, area, { aspect: d.w / Math.max(1, d.h), canAdd: list.length < MAX_DESKTOPS, rtl: direction() === 'rtl' });
};
function compute(app) {
  const list = windows.get().filter((w) => own(w) && !w.fullscreen && !w.pip && onActiveDesktop(w) && (!app || launcherIdOf(w) === app));
  const area = workAreaOf(mainDisplay());
  const strip = app ? null : stripFor(desktopList.get(), area);
  const top = strip ? strip.h : 14;
  return { app, list, strip, area, places: gridLayout(list, { x: area.x, y: area.y + top, w: area.w, h: area.h - top }, wm.frame) };
}
const refresh = () => {
  const o = overview.get();
  if (o) overview.set(compute(o.app));
};
windows.subscribe(refresh);
activeDesktop.subscribe(refresh);
desktopList.subscribe(refresh);
desktopNames.subscribe(refresh);

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
  cardPress = null;
  hoverStore.set(null);
  dragStore.set(null);
  cardHover.set(null);
  cardDrag.set(null);
  endRename(true);
  cardMenu.set(null);
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

// Renaming a desktop: the field replaces its name under the card. Enter or a click elsewhere keeps what
// was typed (empty: back to the default name), Esc leaves the name as it was.
function startRename(id) {
  cardMenu.set(null);
  renaming.set({ id, value: desktopNames.get()[id] || '' });
}
function endRename(keep) {
  const r = renaming.get();
  if (!r) return;
  renaming.set(null);
  if (keep) renameDesktop(r.id, r.value);
}
const inside = (node, e) => {
  if (!node || typeof lord === 'undefined') return false;
  try { const [x, y, w, hh] = lord.getLayout(node.id); return e.x >= x && e.x < x + w && e.y >= y && e.y < y + hh; } catch (err) { return false; }
};

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
// A minimized window's body when it has nothing else to show: its app icon and title. Inside the
// window's scaled node, so the sizes are divided by the scale.
export function OverviewPlaceholder({ win, scale }) {
  const th = useTheme();
  if (!overviewPlaceholder(win, true)) return null;
  const Icon = windowIcon(win, getApp);
  return h(View, { style: { position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', gap: Math.round(10 / scale),
    padding: Math.round(12 / scale), backgroundColor: th.windowBg, borderRadius: win.maximized ? 0 : 8, overflow: 'hidden', pointerEvents: 'none' } },
  Icon ? h(Icon, { size: Math.round(48 / scale) }) : null,
  h(Text, { style: { maxWidth: '100%', fontSize: Math.round(13 / scale), color: th.textDim, whiteSpace: 'nowrap', textOverflow: 'ellipsis' } }, windowTitle(win)));
}

// The window's place in the overview, or null. WindowShell reads it.
export function useOverviewPlace(id) {
  return useStore(overview, (o) => o?.places.get(id) || null);
}

export function OverviewBackdrop() {
  const o = useStore(overview);
  if (!o) return null;
  const { area } = o;
  // a click on the wallpaper with a menu or a name field open only closes that
  const click = () => { if (cardMenu.get() || renaming.get()) { cardMenu.set(null); endRename(true); } else closeOverview(); };
  return h(View, { onClick: click, style: { position: 'absolute', left: area.x, top: area.y, width: area.w, height: area.h,
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
function Card({ c, th, active, all, wallpaper, over, canRemove, lifted }) {
  const hovered = useStore(cardHover) === c.id;
  const edit = useStore(renaming, (r) => (r?.id === c.id ? r : null));
  const d = mainDisplay();
  const k = c.w / Math.max(1, d.w);
  const mine = all.filter((w) => !w.minimized && !w.pip && !w.fullscreen && own(w) && onDesktop(w, c.id));
  const show = () => { if (Date.now() - cardDropAt < 300) return; if (active) closeOverview(); else setActiveDesktop(c.id); };
  return h(View, { key: c.id, onClick: show,
    onMouseDown: (e) => { if (!e.button && !edit) cardPress = { id: c.id, x: e.x, y: e.y, grab: e.x - c.x }; },
    onContextMenu: (e) => { endRename(true); cardMenu.set({ id: c.id, x: e.x, y: e.y }); },
    onMouseEnter: () => cardHover.set(c.id), onMouseLeave: () => cardHover.set((v) => (v === c.id ? null : v)),
    style: { position: 'absolute', left: c.x, top: c.y, width: c.w, height: c.h + 24, alignItems: 'center', opacity: lifted ? 0.9 : 1 } },
  h(View, { style: { width: c.w, height: c.h, borderRadius: 8, overflow: 'hidden', backgroundColor: th.windowBg,
    border: `2px solid ${over || lifted ? th.accent : active ? '#ffffff' : 'rgba(255,255,255,0.25)'}`, boxShadow: lifted ? '0 10px 28px rgba(0,0,0,0.45)' : 'none' } },
  wallpaper ? h(Image, { src: wallpaper, style: { position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, objectFit: 'cover' } }) : null,
  mine.map((w) => {
    const r = wm.frame(w), Icon = windowIcon(w, getApp);
    const mw = Math.max(10, r.w * k), mh = Math.max(8, r.h * k);
    return h(View, { key: w.id, style: { position: 'absolute', left: (r.x - d.x) * k, top: (r.y - d.y) * k, width: mw, height: mh, borderRadius: 3, alignItems: 'center', justifyContent: 'center',
      backgroundColor: th.windowBg, border: `1px solid ${th.separator}` } },
    Icon && mw >= 22 && mh >= 22 ? h(Icon, { size: Math.min(20, mh - 6) }) : null);
  })),
  edit ? h(View, { ref: (n) => { fieldBox = n; }, onClick: (e) => e.stopPropagation?.(), style: { marginTop: 2, width: Math.max(c.w, 120) } },
    h(TextInput, { value: edit.value, autoFocus: true, selectAll: !!edit.value, size: 12, placeholder: t('shell.desktops.name', { n: desktopList.get().indexOf(c.id) + 1 }),
      style: { height: 22, paddingHorizontal: 6 },
      onKeyDown: (e) => { renameKey = e; return false; },
      onChange: (v) => renaming.set((r) => (r ? { ...r, value: v } : r)),
      onSubmit: () => endRename(true), onCancel: () => endRename(false) }))
    // the name: a click shows that desktop without leaving the Overview, a double-click renames it
    : h(View, { onClick: (e) => { e.stopPropagation?.(); if (Date.now() - cardDropAt >= 300 && !active) setActiveDesktop(c.id); },
      onDoubleClick: (e) => { e.stopPropagation?.(); startRename(c.id); }, style: { marginTop: 4, maxWidth: c.w, paddingHorizontal: 4 } },
    h(Text, { style: { fontSize: 12, fontWeight: active ? 600 : 400, color: '#ffffff', whiteSpace: 'nowrap', textOverflow: 'ellipsis' } }, desktopName(c.id))),
  hovered && canRemove && !lifted ? h(View, { onClick: (e) => { e.stopPropagation?.(); closeDesktop(c.id); }, style: { position: 'absolute', top: 4, [direction() === 'rtl' ? 'left' : 'right']: 4, width: 20, height: 20, borderRadius: 10,
    alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.65)' } },
  h(Text, { style: { fontSize: 13, color: '#fff', lineHeight: 20 } }, '×')) : null);
}

function Strip({ o, th }) {
  const drag = useStore(dragStore);
  const lift = useStore(cardDrag);
  const all = useStore(windows);
  const active = useStore(activeDesktop);
  useStore(desktopNames);
  const s = settings.get();
  const wall = wallpaperThumb(findWallpaper(th.wallpapers, s.wallpaper));
  const { strip } = o;
  // while a desktop is dragged the others make room where it would go, and it follows the pointer
  let cards = strip.cards;
  if (lift) {
    const xs = strip.cards.map((c) => c.x), lo = Math.min(...xs), hi = Math.max(...xs);
    const placed = stripFor(moveDesktopTo(desktopList.get(), lift.id, lift.slot), o.area).cards
      .map((c) => (c.id === lift.id ? { ...c, x: Math.max(lo, Math.min(lift.x - lift.grab, hi)) } : c));
    cards = [...placed.filter((c) => c.id !== lift.id), ...placed.filter((c) => c.id === lift.id)];   // on top of the others
  }
  return h(View, { style: { position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, pointerEvents: 'box-none' } },
    cards.map((c) => h(Card, { key: c.id, c, th, all, active: c.id === active, wallpaper: wall, over: drag?.over === c.id, canRemove: strip.cards.length > 1, lifted: lift?.id === c.id })),
    strip.add ? h(View, { onClick: () => createDesktop(), style: { position: 'absolute', left: strip.add.x, top: strip.add.y, width: strip.add.w, height: strip.add.h, borderRadius: 8,
      alignItems: 'center', justifyContent: 'center', backgroundColor: drag?.over === 'add' ? th.accent : 'rgba(255,255,255,0.14)', border: '1px solid rgba(255,255,255,0.25)' } },
    h(Text, { style: { fontSize: 22, color: '#fff' } }, '+')) : null);
}

// A desktop's right-click menu: rename, move along the strip, close.
function CardMenu({ th }) {
  const m = useStore(cardMenu);
  const list = useStore(desktopList);
  if (!m || !list.includes(m.id)) return null;
  const i = list.indexOf(m.id), rtl = direction() === 'rtl';
  const left = rtl ? i + 1 : i - 1, right = rtl ? i - 1 : i + 1;    // the strip is mirrored in right-to-left languages
  const items = [
    { label: t('shell.desktops.rename'), run: () => startRename(m.id) },
    '-',
    { label: t('shell.desktops.moveLeft'), disabled: left < 0 || left >= list.length, run: () => moveDesktop(m.id, left) },
    { label: t('shell.desktops.moveRight'), disabled: right < 0 || right >= list.length, run: () => moveDesktop(m.id, right) },
    '-',
    { label: t('shell.desktops.close'), disabled: list.length < 2, run: () => closeDesktop(m.id) },
  ];
  const d = mainDisplay(), w = 200;
  return h(View, { ref: (n) => { menuBox = n; }, style: { position: 'absolute', left: Math.max(d.x, Math.min(m.x, d.x + d.w - w - 4)), top: m.y, width: w, paddingVertical: 5, paddingHorizontal: 5,
    borderRadius: th.menuRadius ?? 8, backgroundColor: popupBg(th), border: `1px solid ${th.separator}`, boxShadow: '0 10px 30px rgba(0,0,0,0.35)' } },
  items.map((it, k) => (it === '-' ? h(View, { key: `s${k}`, style: { height: 1, marginVertical: 4, marginHorizontal: 6, backgroundColor: th.separator } })
    : h(MenuItem, { key: it.label, it, th }))));
}
function MenuItem({ it, th }) {
  const [hover, setHover] = useState(false);
  const lit = hover && !it.disabled;
  return h(View, { onClick: () => { if (it.disabled) return; cardMenu.set(null); it.run(); },
    onMouseEnter: () => setHover(true), onMouseLeave: () => setHover(false),
    style: { height: 24, paddingHorizontal: 10, borderRadius: th.menuItemRadius ?? 4, justifyContent: 'center', backgroundColor: lit ? th.accent : 'transparent', opacity: it.disabled ? 0.4 : 1 } },
  h(Text, { style: { fontSize: 13, whiteSpace: 'nowrap', textAlign: 'start', color: lit ? '#fff' : th.text } }, it.label));
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
      if (renaming.get() || e === renameKey) return;             // the name field's keys
      if (cardMenu.get()) { if (e.key === 'Escape') cardMenu.set(null); return; }
      if (e.key === 'Escape') { closeOverview(); return; }
      const list = overview.get()?.list || [];
      if (!list.length) return;
      const i = Math.max(0, list.findIndex((w) => w.id === hoverStore.get()));
      if (e.key === 'ArrowRight' || e.key === 'Tab') hoverStore.set(list[(i + 1) % list.length].id);
      else if (e.key === 'ArrowLeft') hoverStore.set(list[(i - 1 + list.length) % list.length].id);
      else if (e.key === 'Enter' && hoverStore.get()) pick(hoverStore.get());
    });
    const offDown = addListener('pointerdown', (e) => {
      if (cardMenu.get() && !inside(menuBox, e)) cardMenu.set(null);
      if (renaming.get() && !inside(fieldBox, e)) endRename(true);
    });
    const offMove = addListener('pointermove', (e) => {
      if (cardPress) {
        if (!cardDrag.get() && Math.abs(e.x - cardPress.x) < DRAG_START) return;
        const cards = overview.get()?.strip?.cards;
        if (!cards || cards.length < 2) return;
        cardDrag.set({ id: cardPress.id, x: e.x, grab: cardPress.grab, slot: stripSlot(cards, e.x - cardPress.grab + cards[0].w / 2) });
        return;
      }
      if (!press) return;
      if (!dragStore.get() && Math.abs(e.x - press.x) + Math.abs(e.y - press.y) < DRAG_START) return;
      dragStore.set({ id: press.id, x: e.x, y: e.y, over: dropAt(e.x, e.y) });
    });
    const offUp = addListener('pointerup', (e) => {
      if (cardPress) {
        const lift = cardDrag.get();
        cardPress = null;
        if (lift) { cardDrag.set(null); cardDropAt = Date.now(); moveDesktop(lift.id, lift.slot); }
        return;
      }
      const p = press, d = dragStore.get();
      press = null;
      if (d) { dragStore.set(null); drop(d.id, dropAt(e.x, e.y)); } else if (p) pick(p.id);
    });
    return () => { offKey(); offDown(); offMove(); offUp(); };
  }, [open]);
  if (!o) return null;
  return [
    h(Layer, { key: 'chrome', name: 'overlay' },
      h(View, { style: { position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, pointerEvents: 'box-none' } },
        o.strip ? h(Strip, { o, th }) : null,
        h(Labels, { o, th }),
        h(CardMenu, { th }))),
    h(Layer, { key: 'drag', name: 'drag' }, h(Ghost, { th })),
  ];
}
