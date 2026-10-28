// @lordos/runtime — React renderer for lord-server (same model as React Native):
// the <View>, <Text> and <Image> components are native nodes drawn by the server in C.
import Reconciler from 'react-reconciler';
import { ConcurrentRoot, DefaultEventPriority, NoEventPriority } from 'react-reconciler/constants';
import { createContext, useEffect, useState } from 'react';
import { flatten, normalize, applyStyle } from './style.js';
import { register, unregister, currentPriority, addListener } from './events.js';

export { addListener };
export { setLayout, getLayout, layouts, modifiers } from './keymap.js';
export const View = 'view';
export const Text = 'text';
export const Image = 'image';
// <Terminal session={id} />: grid of a lord.term session drawn in C.
export const Terminal = 'terminal';
// <Surface surface={id} />: window of a Linux (Wayland) app composited by lord-server.
export const Surface = 'surface';
// <MediaView media={id} />: video, photo, capture or camera from lord.media (docs/MEDIA.md); drawn by C.
export const MediaView = 'media';
export const screen = { width: lord.screen.width, height: lord.screen.height, displays: [], mirror: false };
// Displays (docs/DISPLAYS.md): the desktop is one logical space (screen.width × height) and every
// display shows a rectangle of it: { id, name, x, y, w, h, scale, main }. The menu bar and the Dock
// go on the main one; windows can be on any display or span two.
function readDisplays() {
  const d = lord.screen.displays;
  screen.displays = Array.isArray(d) && d.length ? d.map((x) => ({ ...x }))
    : [{ id: 0, name: '', x: 0, y: 0, w: screen.width, h: screen.height, scale: 100, main: true }];
  screen.mirror = !!lord.screen.mirror;
}
readDisplays();
export const displays = () => screen.displays;
export const mainDisplay = () => screen.displays.find((d) => d.main) || screen.displays[0];
// The display containing a point (else the nearest one).
export function displayAt(x, y) {
  let best = null, dist = Infinity;
  for (const d of screen.displays) {
    const dx = Math.max(d.x - x, 0, x - (d.x + d.w - 1)), dy = Math.max(d.y - y, 0, y - (d.y + d.h - 1));
    const k = dx * dx + dy * dy;
    if (k < dist) { dist = k; best = d; }
  }
  return best || mainDisplay();
}
// System dialogs and panels go on the main display: its box as absolute-position style.
export function mainBox() { const m = mainDisplay(); return { left: m.x, top: m.y, width: m.w, height: m.h }; }
// The display with most of a rectangle (a window spanning two belongs to the one with more of it).
export function displayFor(r) {
  let best = null, area = 0;
  for (const d of screen.displays) {
    const w = Math.min(r.x + r.w, d.x + d.w) - Math.max(r.x, d.x), h = Math.min(r.y + r.h, d.y + d.h) - Math.max(r.y, d.y);
    if (w > 0 && h > 0 && w * h > area) { area = w * h; best = d; }
  }
  return best || displayAt(r.x + r.w / 2, r.y + r.h / 2);
}
// The screen can change size (session viewed from another monitor or from the browser):
// lord-server updates lord.screen and calls __lordScreen.
const screenListeners = new Set();
let rootNode = 0;
export function onScreenChange(fn) { screenListeners.add(fn); return () => screenListeners.delete(fn); }
globalThis.__lordScreen = () => {
  const old = { width: screen.width, height: screen.height, displays: screen.displays, mirror: screen.mirror };
  screen.width = lord.screen.width;
  screen.height = lord.screen.height;
  readDisplays();
  if (rootNode) { lord.setStyle(rootNode, 'width', screen.width); lord.setStyle(rootNode, 'height', screen.height); }
  for (const c of layerNodes.values()) { lord.setStyle(c.id, 'width', screen.width); lord.setStyle(c.id, 'height', screen.height); }
  for (const fn of screenListeners) { try { fn(screen, old); } catch (e) { console.error('pantalla:', e?.message || e); } }
};

// For each instance, a copy of the flattened style that produced inst.style, as [key, value, key,
// value, …] in its own order (a WeakMap, so the instance a ref sees is unchanged). It is kept only
// when every value is a primitive and nothing in inst.style is NaN (null otherwise). A render whose
// flattened style has the same keys, in the same order, with the same values (Object.is) then skips
// normalize and sends nothing: normalize is pure, so it would give a style equal to inst.style and
// applyStyle would send no call. It is a copy, not the props' object, so a style object changed in
// place is still seen; the NaN rule keeps the calls applyStyle repeats because NaN !== NaN (a bad
// '#zz' color).
function flatCopy(flat, style) {
  const out = [];
  for (const k in flat) {
    const v = flat[k];
    if (v !== null && (typeof v === 'object' || typeof v === 'function')) return null;
    out.push(k, v);
  }
  for (const k in style) {
    const v = style[k];
    if (v !== v || (Array.isArray(v) && v.some((x) => x !== x))) return null;
  }
  return out;
}
const flats = new WeakMap();
function sameFlat(copy, flat) {
  if (!copy) return false;
  let i = 0;
  for (const k in flat) {
    if (i >= copy.length || copy[i] !== k || !Object.is(copy[i + 1], flat[k])) return false;
    i += 2;
  }
  return i === copy.length;
}

function applyProps(inst, prev, next) {
  const flat = flatten(next.style);
  if (!sameFlat(flats.get(inst), flat)) {
    const style = normalize(flat);
    applyStyle(inst.id, inst.style, style);
    inst.style = style;
    flats.set(inst, flatCopy(flat, style));
  }
  if (prev.src !== next.src) lord.setProp(inst.id, 'src', next.src ?? null);
  if (prev.session !== next.session) lord.setProp(inst.id, 'session', next.session ?? 0);
  if (prev.surface !== next.surface) lord.setProp(inst.id, 'surface', next.surface ?? 0);
  if (prev.media !== next.media) lord.setProp(inst.id, 'media', next.media ?? 0);
  // protected: the node (and what's inside) comes out black in screenshots, recordings and remote desktop
  if (!!prev.protected !== !!next.protected) lord.setProp(inst.id, 'protected', !!next.protected);
  // <Image pixels={{ width, height, data }} />: RGBA canvas (a new object for each frame)
  if (prev.pixels !== next.pixels && next.pixels) lord.setProp(inst.id, 'pixels', next.pixels);
}

function attach(parent, child) {
  child.parent = parent.isContainer ? null : parent;
}

// Priority of the update in progress (React 19 sets it and queries it here).
let updatePriority = NoEventPriority;

// react-reconciler 0.34 host interface (React 19), mutation mode like React Native.
const host = {
  rendererPackageName: '@lordos/runtime',
  rendererVersion: '0.8.4',
  supportsMutation: true,
  supportsPersistence: false,
  supportsHydration: false,
  isPrimaryRenderer: true,
  noTimeout: -1,
  scheduleTimeout: (fn, ms) => setTimeout(fn, ms),
  cancelTimeout: (id) => clearTimeout(id),
  supportsMicrotasks: true,
  scheduleMicrotask: (fn) => queueMicrotask(fn),

  createInstance(type, props) {
    const inst = { id: lord.createNode(type), type, props, style: {}, parent: null };
    register(inst);
    applyProps(inst, {}, props);
    return inst;
  },
  createTextInstance(text) {
    const inst = { id: lord.createNode('raw'), type: 'raw', props: {}, style: {}, parent: null };
    lord.setText(inst.id, text);
    return inst;
  },
  appendInitialChild(parent, child) { lord.appendChild(parent.id, child.id); attach(parent, child); },
  appendChild(parent, child) { lord.appendChild(parent.id, child.id); attach(parent, child); },
  appendChildToContainer(container, child) { lord.appendChild(container.id, child.id); attach(container, child); layerCount(container, 1); },
  insertBefore(parent, child, before) { lord.insertBefore(parent.id, child.id, before.id); attach(parent, child); },
  insertInContainerBefore(container, child, before) { lord.insertBefore(container.id, child.id, before.id); attach(container, child); layerCount(container, 1); },
  removeChild(parent, child) { lord.removeChild(parent.id, child.id); },
  removeChildFromContainer(container, child) { lord.removeChild(container.id, child.id); layerCount(container, -1); },
  detachDeletedInstance(inst) { unregister(inst); lord.destroy(inst.id); },
  clearContainer() {},

  finalizeInitialChildren: () => false,
  // React 19: no prepareUpdate; commitUpdate receives the type and the old and new props
  commitUpdate(inst, _type, prev, next) { applyProps(inst, prev, next); inst.props = next; },
  commitTextUpdate(inst, _old, text) { lord.setText(inst.id, text); },
  resetTextContent() {},
  shouldSetTextContent: () => false,
  hideInstance(inst) { lord.setStyle(inst.id, 'display', 'none'); },
  unhideInstance(inst) { lord.setStyle(inst.id, 'display', inst.style.display ?? 'flex'); },
  hideTextInstance(inst) { lord.setText(inst.id, ''); },
  unhideTextInstance(inst, text) { lord.setText(inst.id, text); },

  getRootHostContext: () => ({}),
  getChildHostContext: (ctx) => ctx,
  getPublicInstance: (inst) => inst,
  prepareForCommit: () => null,
  resetAfterCommit() { layersCommitted(); },
  preparePortalMount() {},
  setCurrentUpdatePriority(p) { updatePriority = p; },
  getCurrentUpdatePriority: () => updatePriority,
  resolveUpdatePriority: () => updatePriority || currentPriority() || DefaultEventPriority,
  resolveEventType: () => null,
  resolveEventTimeStamp: () => -1.1,
  trackSchedulerEvent() {},
  shouldAttemptEagerTransition: () => false,
  requestPostPaintCallback() {},
  // nothing suspends the commit (no stylesheets or images to wait for: they load in C)
  maySuspendCommit: () => false,
  maySuspendCommitOnUpdate: () => false,
  maySuspendCommitInSyncRender: () => false,
  preloadInstance: () => true,
  startSuspendingCommit: () => null,
  suspendInstance() {},
  suspendOnActiveViewTransition() {},
  waitForCommitToBeReady: () => null,
  getSuspendedCommitReason: () => null,
  // forms and <form> transitions (there is no DOM)
  NotPendingTransition: null,
  HostTransitionContext: createContext(null),
  resetFormInstance() {},
  // Fragment refs: an empty object (unused)
  createFragmentInstance: () => ({}),
  updateFragmentInstanceFiber() {},
  commitNewChildToFragmentInstance() {},
  deleteChildFromFragmentInstance() {},
  getInstanceFromNode: () => null,
  beforeActiveInstanceBlur() {},
  afterActiveInstanceBlur() {},
  prepareScopeUpdate() {},
  getInstanceFromScope: () => null,
};

const reconciler = Reconciler(host);

// ---- what is open in the layers that take the keyboard ----
// The search, the app launcher, shell sheets and system prompts (keyring, permissions, the file
// chooser) live in the overlay, modal and system layers (sdk/layers.js). While any of them shows
// something, the keyboard is LordOS's: no Linux app (Wayland or X11) gets the keys typed into it
// (sdk/wayland.js asks the compositor; docs/PRIVACY.md).
const KEYBOARD_LAYERS = new Set([40, 60, 100]);   // overlay, modal, system
let keyboardBusy = false;
const keyboardWatchers = new Set();
function layerCount(container, d) {
  if (!container.layer) return;
  container.count = Math.max(0, (container.count || 0) + d);
}
function layersCommitted() {
  let busy = false;
  for (const [z, c] of layerNodes) if (KEYBOARD_LAYERS.has(z) && c.count > 0) { busy = true; break; }
  if (busy === keyboardBusy) return;
  keyboardBusy = busy;
  for (const fn of keyboardWatchers) { try { fn(busy); } catch (e) { console.error(e); } }
}
export const shellHasKeyboard = () => keyboardBusy;
export function onShellKeyboard(fn) { keyboardWatchers.add(fn); return () => keyboardWatchers.delete(fn); }

// ---- layers: one stacking order for the whole screen (sdk/layers.js, docs/LAYERS.md) ----
// Each layer is a root node of the screen's size, above the React tree (zIndex > 0) and
// pointerEvents 'box-none' (lord-server never hits the layer itself, only what is in it), so paint
// order and hit testing follow the same order. Content goes in with createPortal.
const layerNodes = new Map();                 // zIndex → container
export function layerContainer(z) {
  if (!rootNode || !(z > 0)) return null;
  let c = layerNodes.get(z);
  if (!c) {
    const id = lord.createNode('view');
    for (const [k, v] of [['position', 'absolute'], ['left', 0], ['top', 0], ['width', screen.width], ['height', screen.height],
      ['pointerEvents', 'box-none'], ['zIndex', z]]) lord.setStyle(id, k, v);
    lord.appendChild(rootNode, id);
    c = { id, isContainer: true, layer: z, count: 0 };
    layerNodes.set(z, c);
  }
  return c;
}
// A portal into a host node: a layer container (above), or a View instance (the window's overlay
// host: events then bubble from the portal's content to that View and up to the window).
export function createPortal(children, container, key = null) {
  return reconciler.createPortal(children, container, null, key);
}

export function render(element) {
  const rootId = lord.createNode('view');
  rootNode = rootId;
  lord.setStyle(rootId, 'width', screen.width);
  lord.setStyle(rootId, 'height', screen.height);
  lord.setRoot(rootId);
  const container = { id: rootId, isContainer: true };
  const report = (kind) => (e) => console.error(`react (${kind}):`, e?.message ? `${e.message}\n${e.stack || ''}` : e);
  // React 19: uncaught errors, errors caught by a boundary, and recoverable ones, separately
  const root = reconciler.createContainer(container, ConcurrentRoot, null, false, null, 'lord',
    report('error'), report('capturado'), report('recuperable'), null);
  reconciler.updateContainer(element, root, null, null);
  return root;
}

// ---- Minimal SDK for themes (grows in M3) ----

// Formatted clock: React only re-renders when the text changes (e.g. once per minute).
export function useClock(format = (d) => d.toISOString(), intervalMs = 1000) {
  const [text, setText] = useState(() => format(new Date()));
  useEffect(() => {
    const id = setInterval(() => setText(format(new Date())), intervalMs);
    return () => clearInterval(id);
  }, [intervalMs]);
  return text;
}

export function useStats(intervalMs = 2000) {
  const [stats, setStats] = useState(() => lord.stats());
  useEffect(() => {
    const id = setInterval(() => setStats(lord.stats()), intervalMs);
    return () => clearInterval(id);
  }, [intervalMs]);
  return stats;
}

export const system = { version: lord.version };
