// desktops.js — Desktops, the part with a runtime (docs/DESKTOPS.md). The rules are in desktopmodel.js
// and the stores in windows.js; this module switches, keeps the count, draws the brief indicator and
// runs the switch animation.
//
//   nextDesktopBy(step), addDesktopAndShow(), closeActiveDesktop(), desktopName(id)
//   flowDesktopKeys, classicDesktopKeys     the theme tables' entries (sdk/shortcuts.js)
//   desktopSwipe(dir)                       a registerGestureActions action
//
// The animation: the windows arriving slide a little and fade in, on their cached raster, by setting
// the transform of each one's node (lord.setStyle: no React render per frame). A short timer loop runs
// only during a switch, and none at all with Reduce motion.
import { createElement as h } from 'react';
import { View, Text, mainDisplay } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { t, direction } from './i18n.js';
import { settings } from './settings.js';
import { useTheme } from './theme.js';
import { Layer } from './layers.js';
import { windows, wm, activeDesktop, desktopList, setActiveDesktop, setDesktops, createDesktop, closeDesktop, fxNodes, overviewShowing } from './windows.js';
import { MAX_DESKTOPS, visibleOn, neighbor, arrowStep, swipeStep, cleanCount, cleanIndex } from './desktopmodel.js';
import { cycleWindows } from './trackpad.js';
import { toggleOverview, toggleAppWindows } from './overview.jsx';

const FX_MS = 170, FX_SLIDE = 56, HUD_MS = 800;

// ---- names and positions ----

const positionOf = (id) => desktopList.get().indexOf(id);
export const desktopName = (id) => t('shell.desktops.name', { n: positionOf(id) + 1 });
// " · Desktop 2" for a window on another desktop, in lists that show every desktop.
export function desktopSuffix(w) {
  const list = desktopList.get();
  if (list.length < 2 || !w || w.pip || w.desktop === activeDesktop.get()) return '';
  return ` · ${desktopName(w.desktop)}`;
}

// ---- switching ----

export function switchDesktop(id) { setActiveDesktop(id); }
export function nextDesktopBy(step) {
  const id = neighbor(desktopList.get(), activeDesktop.get(), step);
  if (id != null) switchDesktop(id);
  else showHud();                                     // the end of the strip: at least say where we are
}
const stepArrow = (dir) => nextDesktopBy(arrowStep(dir, direction() === 'rtl'));

export function addDesktopAndShow() {
  const id = createDesktop();
  if (id != null) switchDesktop(id);
  return id;
}
export const canAddDesktop = () => desktopList.get().length < MAX_DESKTOPS;
export function removeDesktopById(id) { return closeDesktop(id); }
export const closeActiveDesktop = () => closeDesktop(activeDesktop.get());

// The window menu's Move to Desktop.
export function moveToDesktopItems(id) {
  const w = wm.get(id);
  const items = desktopList.get().filter((d) => d !== w?.desktop)
    .map((d) => ({ label: desktopName(d), onClick: () => wm.moveToDesktop(id, d) }));
  if (items.length) items.push('-');
  items.push({ label: t('shell.desktops.new'), disabled: !canAddDesktop(), onClick: () => { const d = createDesktop(); if (d != null) wm.moveToDesktop(id, d); } });
  return items;
}

// ---- shortcuts ----

export const ctrlArrowsOn = () => settings.get().desktopArrows !== false;
const arrowDir = (e) => (e.code === 105 ? 'left' : 'right');
const onStep = (e) => stepArrow(arrowDir(e));

// Lord Flow. ⌃ arrows are word movement in a Linux app, so Settings → Keyboard can turn them off
// (then the 'flow' key table keeps them from nobody); ❖⌃ arrows always work.
export const flowDesktopKeys = [
  { id: 'overview', keys: ['Ctrl+ArrowUp'], run: () => toggleOverview(), when: ctrlArrowsOn, listed: ctrlArrowsOn, label: 'shell.shortcuts.overview', group: 'desktops' },
  { id: 'appWindows', keys: ['Ctrl+ArrowDown'], run: () => toggleAppWindows(), when: ctrlArrowsOn, listed: ctrlArrowsOn, label: 'shell.shortcuts.appWindows', group: 'desktops' },
  { id: 'switchDesktopCtrl', keys: ['Ctrl+ArrowLeft', 'Ctrl+ArrowRight'], show: ['⌃←→'], run: onStep, when: ctrlArrowsOn, listed: ctrlArrowsOn, label: 'shell.shortcuts.switchDesktop', group: 'desktops' },
  { id: 'switchDesktopMeta', keys: ['Meta+Ctrl+ArrowLeft', 'Meta+Ctrl+ArrowRight'], show: ['❖⌃←→'], run: onStep, label: 'shell.shortcuts.switchDesktop', group: 'desktops' },
];
export const flowGrabs = () => (ctrlArrowsOn() ? 'flow-desktops' : 'flow');

// Lord Classic.
export const classicDesktopKeys = [
  { id: 'overview', keys: ['Meta+Tab'], run: () => toggleOverview(), label: 'shell.shortcuts.overview', group: 'desktops' },
  { id: 'newDesktop', keys: ['Meta+Ctrl+D'], run: addDesktopAndShow, label: 'shell.shortcuts.newDesktop', group: 'desktops' },
  { id: 'switchDesktop', keys: ['Meta+Ctrl+ArrowLeft', 'Meta+Ctrl+ArrowRight'], show: ['❖⌃←→'], run: onStep, label: 'shell.shortcuts.switchDesktop', group: 'desktops' },
  { id: 'closeDesktop', keys: ['Meta+Ctrl+F4'], run: closeActiveDesktop, label: 'shell.shortcuts.closeDesktop', group: 'desktops' },
];

// Trackpad (registerGestureActions): with one desktop a sideways swipe still switches windows.
export const desktopSwipe = (dir) => () => {
  if (desktopList.get().length > 1) nextDesktopBy(swipeStep(dir, direction() === 'rtl'));
  else cycleWindows(dir);
};

// ---- the count, across sessions and theme switches ----

let surfaceDesktops = null;       // theme switch: Linux surface id → position in the list

// A position in the list, created if it isn't there yet (a restored window comes before its desktop).
export function desktopAt(index) {
  const i = cleanIndex(index);
  while (desktopList.get().length <= i) if (createDesktop() == null) break;
  const list = desktopList.get();
  return list[Math.min(i, list.length - 1)];
}
export const activeIndex = () => Math.max(0, positionOf(activeDesktop.get()));
export const indexOfDesktop = (id) => Math.max(0, positionOf(id));

// A Linux window that stays connected across a theme switch comes back on its desktop.
export function desktopOfSurface(sid) {
  const i = surfaceDesktops?.[sid];
  return Number.isInteger(i) ? desktopAt(i) : undefined;
}
// themes.use: what the next JS context needs, by position (the ids are this context's).
export function desktopsForThemeSwitch() {
  const surfaces = {};
  for (const w of windows.get()) if (w.args?.surface) surfaces[w.args.surface] = indexOfDesktop(w.desktop);
  return { active: activeIndex(), surfaces };
}

let started = false;
// startShell, once, before anything opens a window.
export function startDesktops() {
  const s = settings.get();
  const list = Array.from({ length: cleanCount(s.desktopCount) }, (_, i) => i + 1);
  const was = s.themeDesktops;
  if (was) { surfaceDesktops = was.surfaces || null; settings.set({ themeDesktops: null }); }
  setDesktops(list, list[Math.min(cleanIndex(was?.active), list.length - 1)]);
  started = true;
  shown = activeDesktop.get();
}
desktopList.subscribe(() => {
  if (started && settings.get().desktopCount !== desktopList.get().length) settings.set({ desktopCount: desktopList.get().length });
});

// ---- the brief indicator and the animation ----

const hudState = createStore(false);
let hudTimer = 0;
function showHud() {
  if (desktopList.get().length < 2 || overviewShowing.get()) return;
  hudState.set(true);
  clearTimeout(hudTimer);
  hudTimer = setTimeout(() => hudState.set(false), HUD_MS);
}

// While it runs: the windows' start offset, read once by WindowShell. Null when idle.
export const desktopFx = createStore(null);
let fxTimer = 0;
const style = (node, k, v) => { try { lord.setStyle(node, k, v); } catch (e) { /* closed meanwhile */ } };

function endFx(nodes) {
  clearTimeout(fxTimer);
  fxTimer = 0;
  for (const n of nodes) { style(n, 'transform', undefined); style(n, 'opacity', undefined); }
  desktopFx.set(null);
}
function animate(step) {
  endFx([]);
  if (settings.get().reduceMotion || overviewShowing.get() || typeof lord === 'undefined') return;
  const dx = step * FX_SLIDE * (direction() === 'rtl' ? -1 : 1);
  const t0 = Date.now();
  desktopFx.set({ transform: `translate(${dx}px, 0px)`, opacity: 0 });
  const tick = () => {
    const nodes = visibleOn(windows.get(), activeDesktop.get()).map((w) => fxNodes.get(w.id)).filter(Boolean);
    const p = Math.min(1, (Date.now() - t0) / FX_MS);
    if (p >= 1) { endFx(nodes); return; }
    const e = 1 - (1 - p) * (1 - p) * (1 - p);        // ease-out
    for (const n of nodes) {
      style(n, 'transform', `translate(${(dx * (1 - e)).toFixed(1)}px, 0px)`);
      style(n, 'opacity', +e.toFixed(3));
    }
    fxTimer = setTimeout(tick, 16);
  };
  fxTimer = setTimeout(tick, 16);
}

let shown = activeDesktop.get();
activeDesktop.subscribe(() => {
  const to = activeDesktop.get(), from = shown;
  shown = to;
  if (!started || to === from) return;
  const list = desktopList.get();
  animate(list.indexOf(to) >= list.indexOf(from) ? 1 : -1);
  showHud();
});

export function DesktopHud() {
  const on = useStore(hudState);
  const active = useStore(activeDesktop);
  const list = useStore(desktopList);
  const th = useTheme();
  if (!on) return null;
  const d = mainDisplay();
  return h(Layer, { name: 'tooltip' },
    h(View, { style: { position: 'absolute', left: d.x, top: d.y + Math.round(d.h * 0.14), width: d.w, alignItems: 'center', pointerEvents: 'none' } },
      h(View, { style: { paddingHorizontal: 16, paddingVertical: 10, gap: 8, alignItems: 'center', borderRadius: (th.groupRadius ?? 10) + 4,
        backgroundColor: th.mode === 'dark' ? 'rgba(30,30,34,0.92)' : 'rgba(245,245,247,0.94)', border: `1px solid ${th.separator}`, boxShadow: '0 10px 28px rgba(0,0,0,0.35)' } },
      h(Text, { style: { fontSize: 13, fontWeight: 600, color: th.text, whiteSpace: 'nowrap' } }, desktopName(active)),
      h(View, { style: { flexDirection: 'row', gap: 6 } }, list.map((id) => h(View, { key: id,
        style: { width: 18, height: 6, borderRadius: 3, backgroundColor: id === active ? th.accent : th.separator } }))))));
}
