// themeapi.js — what a community theme's code gets (docs/THEMES-GUIDE.md, "Community themes"; AGENTS.md
// "Community themes"). It runs only inside LordOS's theme host (runtime/theme-host.js), in LordOS's realm.
//
// A community theme is source a person installed from a .zip. LordOS built it (lord-themec) and runs it in a second
// QuickJS realm with a restricted `lord` (server/src/ui/js_theme.c). Its imports resolve here:
//   react        → a React of its own: hooks, memo, context… and createElement, which never makes a host node
//   @lordos/sdk  → the allowlisted part of the SDK below, every component and callback wrapped
//   @lordos/apps → the native apps (their windows draw themselves; the theme only places them)
// What crosses from LordOS's realm into the theme's is sanitized: a node it gets (a ref, an event's target) is a frozen
// handle { id, type }, never the runtime's instance (whose props and links reach every other node); an event is a plain
// copy; stores are read-only views. The theme's own nodes are marked natively (lord.themeOwn): they are the only ones
// its restricted lord.setStyle may touch. React's host guard (runtime/index.js setHostGuard) refuses a host node a
// theme's function rendered itself, so an element it forges can't bring raw refs or handlers either.
//
// Security is not only here: lord-server refuses every native call this file's wrappers would make for the theme
// beyond its policy (js_theme.c), and lockdown() below freezes LordOS's realm before the theme's first line runs.
import * as React from 'react';
import * as SDK from './index.js';
import { apps as hostApps } from '@lordos/apps';
import { addListener as rtAddListener, keyboardLayer, setHostGuard, setRootErrorHandler, onCommit, THEME_OWNED } from '@lordos/runtime';
import { LAYERS } from './layers.js';
import { captureCornerAlways } from './capturestatus.jsx';

const { createElement: h } = React;

// ---- what is the theme's code ----
// lord.themeRealmFn: 2 the theme's own function, 1 anything that can't be told to be LordOS's code (a native of its
// realm, a bound function, a proxy), 0 LordOS's. Everything the theme hands LordOS's code to call later (a callback, a
// timer, an effect, a component that isn't plainly its own) is called through lord.themeCall: tainted, so every native
// it reaches is under the theme's policy even with no frame of the theme's on the stack (server/src/ui/js_theme.c).
const realmOf = (f) => (typeof f === 'function' ? lord.themeRealmFn(f) : 0);
const isThemeFn = (f) => realmOf(f) !== 0;
const themeCall = (fn, ...a) => lord.themeCall(fn, ...a);
// a callback of the theme's that throws (a refused native, a bug) is logged; LordOS's code that called it goes on
const themeCallSafe = (fn, ...a) => {
  try { return themeCall(fn, ...a); } catch (e) { console.error(`community theme: ${e?.message || e}`); return undefined; }
};

// ---- handles: what the theme gets instead of a node ----
const handles = new WeakMap();          // instance → handle
const instances = new WeakMap();        // handle → instance
const isInstance = (v) => v !== null && typeof v === 'object' && typeof v.id === 'number' && 'props' in v && 'style' in v && 'type' in v;
function handleOf(inst) {
  if (!inst) return null;
  let hd = handles.get(inst);
  if (!hd) {
    hd = Object.freeze({ id: inst.id, type: inst.type });
    handles.set(inst, hd);
    instances.set(hd, inst);
  }
  return hd;
}
// what the theme passes back to the SDK (an Overlay's anchor): the instance again
const fromTheme = (v) => (v && typeof v === 'object' && instances.has(v) ? instances.get(v) : v);
const readCurrent = (r) => r.current;
function refFromTheme(r) {
  if (!r || typeof r !== 'object') return fromTheme(r);
  // read later, as the theme's code (its getter, a Proxy's trap): tainted
  return { get current() { return fromTheme(lord.themeCall(readCurrent, r)); } };
}

// an event as a plain copy (no instance, no internal fields)
const EVENT_FIELDS = ['type', 'x', 'y', 'button', 'buttons', 'detail', 'key', 'code', 'chr', 'base', 'down', 'repeat', 'mods', 'ctrl', 'alt',
  'shift', 'meta', 'deltaX', 'deltaY', 'notches', 'precise', 'phase', 'momentum', 'source', 'scale', 'rotation', 'dx', 'dy',
  'fingers', 'system', 'clicks', 'timeStamp', 'pointerType', 'pressure', 'text'];
function eventToTheme(e) {
  const o = {};
  for (const k of EVENT_FIELDS) if (k in e && (typeof e[k] !== 'object' || e[k] === null)) o[k] = e[k];
  o.target = isInstance(e.target) ? handleOf(e.target) : null;
  o.currentTarget = isInstance(e.currentTarget) ? handleOf(e.currentTarget) : null;
  o.stopPropagation = () => { try { e.stopPropagation?.(); } catch (x) { /* none */ } };
  o.preventDefault = () => { try { e.preventDefault?.(); } catch (x) { /* none */ } };
  return Object.freeze(o);
}
const looksLikeEvent = (v) => v !== null && typeof v === 'object' && ('target' in v || 'currentTarget' in v) && !Array.isArray(v);
// LordOS's data as the theme sees it: plain objects and arrays as frozen copies (made once per object), so the theme
// can't change a window, a setting or a store's state in place (a window's `protected`, the theme setting); nodes as
// handles; functions, elements and other objects as they are
const snapshots = new WeakMap();
const originals = new WeakMap();      // a frozen copy → LordOS's object it was made from
// host code never reads a theme-owned object directly: every read of what may hold one runs through lord.themeCall
// (tainted: a getter, a Proxy's trap or a toStringTag accessor in it runs under the theme's policy)
const snapshot = (v) => (v === null || typeof v !== 'object' ? v : lord.themeCall(snapshotRaw, v));
function snapshotRaw(v, depth = 0) {
  try { return snapshotOne(v, depth); } catch (e) { return undefined; }
}
function snapshotOne(v, depth) {
  if (v === null || typeof v !== 'object') return v;
  if (isInstance(v)) return handleOf(v);
  if (isElement(v) || depth > 8) return v;
  const proto = Object.getPrototypeOf(v);
  const arr = Array.isArray(v);
  if (!arr && proto !== Object.prototype && proto !== null) return v;
  let c = snapshots.get(v);
  if (c) return c;
  c = arr ? v.map((x) => snapshotRaw(x, depth + 1)) : {};
  if (!arr) for (const k of Object.keys(v)) c[k] = snapshotRaw(v[k], depth + 1);
  Object.freeze(c);
  snapshots.set(v, c);
  originals.set(c, v);
  return c;
}
function toTheme(v) {
  if (isInstance(v)) return handleOf(v);
  if (looksLikeEvent(v)) return eventToTheme(v);
  return snapshot(v);
}

// a callback of the theme's, called by LordOS's code: its arguments sanitized (one wrapper per function, so its
// identity stays stable across renders)
const callbacks = new WeakMap();
function wrapCallback(fn) {
  if (typeof fn !== 'function') return fn;
  let w = callbacks.get(fn);
  if (!w) {
    w = (...a) => sanitizeIn(themeCallSafe(fn, ...a.map(toTheme)));
    callbacks.set(fn, w);
  }
  return w;
}
const refs = new WeakMap();
const writeCurrent = (r, v) => { r.current = v; };
function wrapRef(r) {
  if (r == null) return r;
  if (typeof r === 'function') return wrapCallback(r);
  if (typeof r !== 'object') return undefined;
  let w = refs.get(r);
  if (!w) {
    w = (v) => { lord.themeCall(writeCurrent, r, toTheme(v)); };   // the theme's ref object, written as its code
    refs.set(r, w);
  }
  return w;
}

// Elements the theme hands LordOS's code, in children or in any other prop (an icon, a label, a render result):
// a host element (a string type) is dropped wherever it hides; an element of a component that is neither the
// facade's nor plainly the theme's own (a forged one, with LordOS's raw component or an object type such as a context
// Provider, memo, forwardRef or Suspense) is made again with sanitized props, so nothing reaches a host node unwrapped.
const ELEMENT = Symbol.for('react.transitional.element');
const isElement = (v) => v !== null && typeof v === 'object' && v.$$typeof === ELEMENT;
// A node the theme hands LordOS (children, a label, a render result): an allowlist. Primitives stay; arrays and other
// iterables become arrays (bounded); an element is kept when it is the facade's or plainly the theme's own component,
// made again with sanitized props otherwise, and dropped when it is a host element; everything else (thenables, lazy
// objects, contexts, portals, plain objects) is dropped. The host guard (guardHost) is the backstop behind it.
const NODE_MAX = 5000;
const cleanNode = (c, depth = 0) => (c === null || typeof c !== 'object' ? cleanNodeRaw(c, depth) : lord.themeCall(cleanNodeRaw, c, depth));
function cleanNodeRaw(c, depth = 0, budget = { n: 0 }) {
  try { return cleanOne(c, depth, budget); } catch (e) { return null; }
}
function cleanOne(c, depth, budget) {
  if (++budget.n > NODE_MAX || depth > 32) return null;
  if (c === null || c === undefined || typeof c === 'string' || typeof c === 'number' || typeof c === 'boolean') return c;
  if (typeof c !== 'object') return null;
  if (isElement(c)) {
    const ty = c.type;
    if (typeof ty === 'string') {
      console.error('community theme: a host element was dropped (use View, Text and Image from @lordos/sdk)');
      return null;
    }
    if (typeof ty === 'function' && (facadeTypes.has(ty) || realmOf(ty) === 2)) return c;
    const props = { ...themePropsRaw(c.props, false, depth + 1) };
    if (c.key != null) props.key = String(c.key);
    return Object.freeze(h(typeof ty === 'function' ? typeFor(ty) : ty, props));
  }
  // real arrays only (never a theme's iterator), read index by index
  if (!Array.isArray(c)) return null;
  const n = Math.min(c.length >>> 0, NODE_MAX), out = [];
  for (let i = 0; i < n; i++) out.push(cleanNodeRaw(c[i], depth + 1, budget));
  return out;
}
const cleanChildren = (c) => cleanNode(c, 0);

// Any other value crossing from the theme's realm into LordOS's code (a prop, an argument, what a callback or a store
// returns): primitives stay; a function becomes a wrapper called through lord.themeCall; a handle becomes its node
// again; a ref object stays live; an element or an iterable goes through cleanNode; a plain object or array is copied
// key by key (bounded in depth and count); a date is copied; anything else is dropped.
const VALUE_MAX = 20000;
const sanitizeIn = (v) => (v === null || (typeof v !== 'object' && typeof v !== 'function') ? v : lord.themeCall(sanitizeRaw, v));
// a theme value that throws while it is read (a refused native in its getter or trap) is dropped, never LordOS's error
function sanitizeRaw(v, depth = 0, budget = { n: 0 }) {
  try { return sanitizeOne(v, depth, budget); } catch (e) { return undefined; }
}
function sanitizeOne(v, depth, budget) {
  if (++budget.n > VALUE_MAX || depth > 16) return undefined;
  if (v === null || v === undefined || typeof v === 'string' || typeof v === 'number' || typeof v === 'boolean' || typeof v === 'bigint') return v;
  if (typeof v === 'function') return wrapCallback(v);
  if (typeof v !== 'object') return undefined;
  if (instances.has(v)) return instances.get(v);
  if (isElement(v)) return cleanNodeRaw(v, depth);
  if (Array.isArray(v)) { const n = Math.min(v.length >>> 0, VALUE_MAX), out = []; for (let i = 0; i < n; i++) out.push(sanitizeRaw(v[i], depth + 1, budget)); return out; }
  let time = NaN;
  try { time = Date.prototype.getTime.call(v); } catch (e) { time = NaN; }
  if (time === time) return new Date(time);
  const proto = Object.getPrototypeOf(v);
  if (proto !== null && Object.getPrototypeOf(proto) !== null) return undefined;     // plain objects only (any realm's)
  if (originals.has(v)) return originals.get(v);    // a frozen copy of LordOS's data, given back as it was
  const keys = Object.keys(v);
  if (keys.length === 1 && keys[0] === 'current') return refFromTheme(v);
  const o = {};
  for (const k of keys) {
    if (k === '__proto__') continue;
    o[k] = sanitizeRaw(v[k], depth + 1, budget);
  }
  return o;
}

// props a theme never sets on a node (another app's terminal, window, video, pixels or a node's protection)
const BLOCKED_PROPS = new Set(['session', 'surface', 'media', 'crop', 'pixels', 'wave', 'dangerouslySetInnerHTML']);
// an image: the theme's own assets/ or LordOS's "asset:" pictures, never a path elsewhere
const srcOk = (s) => typeof s === 'string' && !s.includes('..') && !s.startsWith('/') && !s.includes('\0') &&
  (/^assets\/[A-Za-z0-9_][A-Za-z0-9._/-]*$/.test(s) || /^asset:[A-Za-z0-9_][A-Za-z0-9._/-]*$/.test(s));

const themeProps = (p, host, depth = 0) => lord.themeCall(themePropsRaw, p, host, depth);
function themePropsRaw(p, host, depth = 0) {
  const o = {};
  if (!p || typeof p !== 'object') return o;
  let keys = [];
  try { keys = Object.keys(p); } catch (e) { keys = []; }
  for (const k of keys) {
    if (BLOCKED_PROPS.has(k) || k === 'lordThemeOwned') continue;
    let v;
    try { v = p[k]; } catch (e) { continue; }
    if (k === 'ref') o.ref = wrapRef(v);
    else if (k === 'src' && host) { if (srcOk(v)) o.src = v; }
    else if (k === 'children') o.children = cleanNodeRaw(v, depth);
    else o[k] = sanitizeRaw(v, depth);
  }
  if (host) o.lordThemeOwned = THEME_OWNED;     // a prop React copies (it drops symbol keys); its value can't be forged
  return o;
}

// ---- components ----
const wrappedComponents = new WeakMap();
function wrapComponent(C, name) {
  if (typeof C === 'string') {
    const W = (p) => h(C, themeProps(p, true));
    W.displayName = name;
    return W;
  }
  let W = wrappedComponents.get(C);
  if (!W) {
    W = (p) => h(C, themeProps(p, false));
    W.displayName = name || C.displayName || C.name;
    wrappedComponents.set(C, W);
  }
  return W;
}
const facadeTypes = new WeakSet();

// a component the theme gives that isn't plainly its own function (a bound function, a native, a proxy): called tainted
const untrustedTypes = new WeakMap();
function typeFor(type) {
  if (typeof type !== 'function' || facadeTypes.has(type) || realmOf(type) !== 1) return type;
  let W = untrustedTypes.get(type);
  if (!W) { W = (p) => themeCall(type, p); untrustedTypes.set(type, W); }
  return W;
}

// React for the theme: createElement never makes a host node, and a component that isn't the theme's own or the
// facade's (an app's) gets sanitized props
function themeCreateElement(type, props, ...children) {
  if (typeof type === 'string') throw new TypeError('a community theme draws with View, Text and Image from @lordos/sdk');
  const kids = children.map((c) => cleanChildren(c));
  if (typeof type === 'function' && (facadeTypes.has(type) || realmOf(type) === 2)) return h(type, props, ...kids);
  // LordOS's own components (an app's), untrusted functions, and object or symbol types (a context's Provider,
  // memo, forwardRef, Fragment, Suspense): sanitized props
  return h(typeof type === 'function' ? typeFor(type) : type, themeProps(props, false), ...kids);
}
function themeCloneElement(el, props, ...children) {
  if (!el || typeof el.type === 'string') throw new TypeError('a community theme clones only its own elements');
  return themeCreateElement(el.type, { ...el.props, key: el.key, ...props }, ...(children.length ? children : [el.props?.children]));
}
// hooks: what React calls later runs tainted
const fnOr = (f) => (typeof f === 'function' ? () => themeCall(f) : f);
const effect = (useE) => (f, deps) => useE(() => { const c = themeCallSafe(f); return typeof c === 'function' ? () => { themeCallSafe(c); } : undefined; }, deps);
const setters = new WeakMap();
function setterFor(set) {
  let w = setters.get(set);
  if (!w) { w = (u) => set(typeof u === 'function' ? (p) => themeCall(u, p) : u); setters.set(set, w); }
  return w;
}
const themeReact = () => Object.freeze({
  createElement: (...a) => lord.themeCall(themeCreateElement, ...a), cloneElement: (...a) => lord.themeCall(themeCloneElement, ...a), Fragment: React.Fragment,
  isValidElement: React.isValidElement, Children: React.Children,
  memo: (C, cmp) => React.memo(realmOf(C) === 2 ? C : typeFor(C), typeof cmp === 'function' ? (a, b) => themeCall(cmp, a, b) : undefined),
  forwardRef: (render) => React.forwardRef((p, r) => themeCall(render, p, r)),
  createContext: React.createContext,
  useState: (init) => { const [v, set] = React.useState(fnOr(init)); return [v, setterFor(set)]; },
  useReducer: (red, init, initFn) => { const [v, d] = React.useReducer((st, a) => themeCall(red, st, a), init, typeof initFn === 'function' ? (x) => themeCall(initFn, x) : undefined); return [v, d]; },
  useEffect: effect(React.useEffect), useLayoutEffect: effect(React.useLayoutEffect),
  useMemo: (f, deps) => React.useMemo(() => themeCall(f), deps),
  useCallback: React.useCallback, useRef: React.useRef, useContext: React.useContext, useId: React.useId,
  useSyncExternalStore: (sub, get) => React.useSyncExternalStore((cb) => themeCall(sub, cb), () => themeCall(get)),
  useTransition: () => { const [p, start] = React.useTransition(); return [p, (f) => start(() => themeCall(f))]; },
  useDeferredValue: React.useDeferredValue, startTransition: (f) => React.startTransition(() => themeCall(f)),
  Suspense: React.Suspense, StrictMode: React.StrictMode,
});

// ---- read-only stores ----
const views = new WeakMap();
function readOnly(store, map = (x) => snapshot(x)) {
  if (!store || typeof store.get !== 'function') return store;
  let v = views.get(store);
  if (!v) {
    v = Object.freeze({ get: () => map(store.get()), subscribe: (fn) => store.subscribe(wrapCallback(fn)) });
    views.set(store, v);
    sdkStores.add(v);
  }
  return v;
}
// a store the theme reads: one of LordOS's (a read-only view, or one the facade's createStore made) as it is; any
// other object is the theme's own, so its get and subscribe run through lord.themeCall (tainted), and the callback
// React gives subscribe reaches the theme only as a fresh wrapper
const sdkStores = new WeakSet();
const themeStores = new WeakMap();
const callGet = (s) => s.get();
const callSubscribe = (s, fn) => s.subscribe(fn);
function storeFromTheme(s) {
  if (!s || typeof s !== 'object') return s;
  if (sdkStores.has(s)) return s;
  let w = themeStores.get(s);
  if (!w) {
    w = Object.freeze({
      get: () => sanitizeIn(themeCallSafe(callGet, s)),
      subscribe: (fn) => { const off = themeCallSafe(callSubscribe, s, () => { fn(); }); return () => { if (typeof off === 'function') themeCallSafe(off); }; },
    });
    themeStores.set(s, w);
  }
  return w;
}
const themeUseStore = (s, sel) => snapshot(SDK.useStore(storeFromTheme(s), sel ? wrapCallback(sel) : undefined));
function themeCreateStore(initial) {
  const st = SDK.createStore(initial);
  const v = Object.freeze({
    get: () => snapshot(st.get()),
    set: (p) => st.set(typeof p === 'function' ? (x) => themeCall(p, x) : p),
    subscribe: (fn) => st.subscribe(wrapCallback(fn)),
  });
  sdkStores.add(v);
  return v;
}

// notifications in a notification centre: plain data, never their actions
const noteData = (n) => (n && typeof n === 'object' ? Object.freeze({
  id: n.id, app: n.app, title: n.title, body: n.body, time: n.time, icon: typeof n.icon === 'string' ? n.icon : undefined,
  urgency: n.urgency, sticky: !!n.sticky, read: !!n.read,
}) : n);
const notesData = (l) => (Array.isArray(l) ? Object.freeze(l.map(noteData)) : []);

// ---- settings: read everything, write only the theme's own options and a few appearance keys ----
const WRITABLE = /^(mode|accent|effects|reduceTransparency|dock[A-Z][A-Za-z]*|menuBar[A-Za-z]*|taskbar[A-Za-z]*)$/;
function settingsPatch(id, patch) {
  const cur = SDK.settings.get();
  const p = sanitizeIn(typeof patch === 'function' ? themeCall(patch, snapshot(cur)) : patch);
  if (!p || typeof p !== 'object') return;
  const out = {};
  for (const k of Object.keys(p)) {
    const v = p[k];
    if (k === 'themeOptions' && v && typeof v === 'object') {
      const mine = v[id];
      if (mine && typeof mine === 'object') out.themeOptions = { ...(cur.themeOptions || {}), [id]: JSON.parse(JSON.stringify(mine)) };
    } else if (WRITABLE.test(k) && (v === null || ['string', 'number', 'boolean'].includes(typeof v))) out[k] = v;
  }
  if (Object.keys(out).length) SDK.settings.set(out);
}

// ---- the keyboard: a theme's own listeners hear keys only while the keyboard is the shell's ----
let themeHolds = 0;
function themeHoldKeyboard() {
  const release = SDK.holdKeyboard();
  themeHolds++;
  let on = true;
  return () => { if (on) { on = false; themeHolds--; release(); } };
}
function keysForTheme() {
  const top = keyboardLayer();
  if (top && top.layer >= LAYERS.system) return false;          // a system prompt has the keyboard
  if (themeHolds > 0) return true;                                // its own menu or launcher holds it (holdKeyboard)
  if (top) return false;                                          // LordOS's search, a sheet: not the theme's
  return SDK.wm.focused() == null;                                // no window has the keyboard
}
const POINTER_TYPES = new Set(['pointerdown', 'pointerup', 'pointermove']);
function themeAddListener(type, fn) {
  if (typeof fn !== 'function') return () => {};
  if (type === 'keydown' || type === 'keyup') return rtAddListener(type, (e) => { if (keysForTheme()) themeCallSafe(fn, eventToTheme(e)); });
  if (POINTER_TYPES.has(type)) return rtAddListener(type, (e) => themeCallSafe(fn, Object.freeze({ type: e.type, x: e.x, y: e.y, button: e.button })));
  return () => {};
}
// a theme's shortcuts: only combinations with ❖, ⌃ or ⌥ (or Esc and F1–F24), so a table can't read what is typed
const comboOk = (c) => typeof c === 'string' && (/(^|\+)(Meta|Ctrl|Alt)\+/.test(c) || /^(Escape|F([1-9]|1[0-9]|2[0-4]))$/.test(c));
function themeUseShortcuts(raw = {}) {
  const o = sanitizeIn(raw) || {};
  const table = Array.isArray(o.table) ? o.table.filter((e) => e && comboOk(e.combo) && typeof e.run === 'function')
    .map((e) => ({ id: typeof e.id === 'string' ? e.id : undefined, combo: e.combo, run: wrapCallback(e.run), when: e.when ? wrapCallback(e.when) : undefined, repeat: !!e.repeat })) : [];
  return SDK.useShortcuts({ grabs: typeof o.grabs === 'string' ? o.grabs : '', table, onSuperTap: o.onSuperTap ? wrapCallback(o.onSuperTap) : null });
}

// ---- layers: a theme's surfaces go below the system's (notifications, prompts, the input method, Screenshots) ----
const THEME_LAYERS = new Set(['dock', 'menubar', 'overlay', 'popover', 'modal', 'tooltip', 'drag']);
function ThemeLayer(p) {
  if (!THEME_LAYERS.has(p.name)) throw new Error(`a community theme can't draw in the ${typeof p.name === 'string' ? p.name : '?'} layer`);
  return h(SDK.Layer, { name: p.name }, cleanChildren(p.children));
}
function ThemeOverlay(p) {
  const layer = THEME_LAYERS.has(p.layer) ? p.layer : 'popover';
  return h(SDK.Overlay, { anchor: refFromTheme(p.anchor), onClose: p.onClose ? wrapCallback(p.onClose) : undefined, layer }, cleanChildren(p.children));
}
// a window's frame: the window as the window manager has it (its protection can't be dropped by the theme)
function ThemeWindowShell(props) {
  const p = { ...props, win: sanitizeIn(props.win) };
  const real = p.win && SDK.wm.get ? SDK.wm.get(p.win.id) : null;
  return h(SDK.WindowShell, { ...themeProps(p, false), win: real || p.win });
}

// ---- wm: an allowlist of what a frame, a Dock and a window menu need ----
// Window ids and geometry only: nothing that sets a window's arguments, protection, menus, close guard, layer,
// picture-in-picture, full screen or script moves (setArgs, setProtected, setMenus, setCloseGuard, setLayer, setPip,
// setFullscreen, scriptMove are not here). open takes an app id and at most { new, frame } with numbers.
const num = (v) => (typeof v === 'number' && Number.isFinite(v) ? v : 0);
const idOf = (v) => (typeof v === 'string' ? v : '');
const rect = (raw) => { const r = sanitizeIn(raw); return r && typeof r === 'object' ? { x: num(r.x), y: num(r.y), w: num(r.w), h: num(r.h) } : undefined; };
const plain = (v) => (v == null || typeof v !== 'object' ? v : JSON.parse(JSON.stringify(v)));
function themeWm() {
  const W = SDK.wm;
  const id1 = (k) => (id) => plain(W[k](idOf(id)));
  return Object.freeze({
    focused: () => W.focused(), get: (id) => plain(W.get(idOf(id))), frame: (w) => plain(W.frame(w && typeof w === 'object' ? W.get(idOf(w.id)) || {} : {})),
    focus: id1('focus'), raise: id1('raise'), minimize: id1('minimize'), toggleMaximize: id1('toggleMaximize'), center: id1('center'),
    toggleAbove: id1('toggleAbove'), isAbove: id1('isAbove'), hasKeyboard: id1('hasKeyboard'), closeWindowMenu: id1('closeWindowMenu'),
    beginMove: id1('beginMove'), cancelGrab: () => W.cancelGrab?.(), cornerRadius: id1('cornerRadius'),
    close: (id) => W.close(idOf(id)),
    beginResize: (id, edge) => W.beginResize(idOf(id), typeof edge === 'string' ? edge : ''),
    snap: (id, where) => W.snap(idOf(id), typeof where === 'string' ? where : ''),
    snapKey: (id, key) => W.snapKey(idOf(id), typeof key === 'string' ? key : ''),
    move: (id, x, y) => W.move(idOf(id), num(x), num(y)),
    resize: (id, w, hh) => W.resize(idOf(id), num(w), num(hh)),
    setFrame: (id, r) => W.setFrame(idOf(id), rect(r)),
    liftForKeyboard: (id, r) => W.liftForKeyboard?.(idOf(id), rect(r)),
    dropFromKeyboard: id1('dropFromKeyboard'),
    moveToDisplay: (id, d) => W.moveToDisplay(idOf(id), num(d)),
    moveToDesktop: (id, d) => W.moveToDesktop(idOf(id), num(d)),
    arrange: (how) => W.arrange(typeof how === 'string' ? how : ''),
    showWindowMenu: (id, x, y) => W.showWindowMenu?.(idOf(id), num(x), num(y)),
    open: (app, raw) => { const opts = sanitizeIn(raw); return W.open(idOf(app), opts && typeof opts === 'object' ? { new: !!opts.new, frame: rect(opts.frame) } : undefined); },
  });
}

// ---- configureWindows: LordOS's apps (the ones the theme lists, by id), and only numbers and the theme's functions ----
function themeConfigureWindows(raw = {}) {
  const o = sanitizeIn(raw) || {};
  const ids = Array.isArray(o.apps) ? new Set(o.apps.map((a) => (a && typeof a === 'object' ? a.id : a)).filter((x) => typeof x === 'string')) : null;
  const apps = ids ? hostApps.filter((a) => ids.has(a.id)) : hostApps;
  const areaFn = typeof o.workArea === 'function' ? wrapCallback(o.workArea) : null;
  const cfg = { apps };
  if (areaFn) cfg.workArea = (d) => rect(areaFn({ ...d })) || { x: d.x, y: d.y, w: d.w, h: d.h };
  else if (o.workArea && typeof o.workArea === 'object') cfg.workArea = rect(o.workArea);
  if (o.chrome && typeof o.chrome === 'object') cfg.chrome = { w: num(o.chrome.w), h: num(o.chrome.h) };
  if (o.snap != null) cfg.snap = !!o.snap;
  if (o.snapThirds != null) cfg.snapThirds = !!o.snapThirds;
  if (typeof o.locked === 'function') { const f = wrapCallback(o.locked); cfg.locked = (w) => !!f(plain(w)); }
  SDK.configureWindows(cfg);
}

// ---- the allowlist ----
// Components (wrapped), functions (arguments and results sanitized), and values. Anything not here is not a theme's:
// the file system, the terminal, packages, administrator actions, permissions, the keyring, the clipboard, search
// across files, Trash emptying, the network's settings, theme switching.
const COMPONENTS = ['View', 'Text', 'Image', 'ScrollView', 'TextInput', 'SearchField', 'Toggle', 'Slider', 'Segmented', 'Button',
  'AppHost', 'SeamCover', 'DesktopWidgets', 'WidgetDropTarget', 'WidgetPicker', 'MotionWallpaper', 'PowerDialog', 'ConfirmDialog',
  'AppSwitcher', 'DesktopHud', 'LevelHud', 'OverviewBackdrop', 'OverviewChrome', 'StatusItems', 'PrivacyIndicators', 'LordOSLogo',
  'MoonGlyph', 'SunGlyph', 'SignalGlyph'];
const FUNCTIONS = ['t', 'tOptional', 'appName', 'formatDate', 'formatTime', 'formatNumber', 'formatBytes', 'weekdayName', 'monthName',
  'monthWeeks', 'weekStartFor', 'direction', 'textDirection', 'useLocale', 'useWindows', 'getApp', 'windowTitle',
  'windowIcon', 'windowAccent', 'windowMenuItems', 'windowMenuTiling', 'alwaysOnTopItem', 'moveToDesktopItems', 'appMenuItems',
  'useAppMenus', 'useMenusWithSpeech', 'appGroup', 'appSettingsItem', 'hideApp', 'hideOthers', 'cycleAppWindows', 'launchApp',
  'useLauncherApps', 'refreshApps', 'appCategory', 'compareNames', 'groupApps', 'groupKey', 'groupRows', 'useDockEntries',
  'dockActivate', 'dockMenu', 'dockPrefs', 'dockIdOf', 'beginDockDrag', 'registerDockTarget', 'trashMenu', 'useTrash', 'openTrash',
  'refreshTrash', 'screenDisplays', 'mainDisplay', 'displayAt', 'displayFor', 'mainBox', 'pointerDisplay', 'workAreaOf',
  'pointerWorkArea', 'keepWindowsInWorkAreas', 'titleBarPx', 'useWindowButtons', 'solidOver', 'useClock', 'sharedTicker',
  'findWallpaper', 'wallpaperSrc', 'wallpaperIdFor', 'themeOptionValue', 'themeOptionPatch', 'menuBarEveryDisplay', 'menuBarPrefs',
  'barDisplays', 'hasBar', 'barIndex', 'edgeIndex', 'stepIndex', 'matchIndex', 'typeAhead', 'selectable', 'keepSelection',
  'openSwitcher', 'switcherGrabs', 'switcherHold', 'switcherKeys', 'flowGrabs', 'flowDesktopKeys', 'classicDesktopKeys',
  'addMenuBarKeys', 'registerGestureActions', 'desktopSwipe', 'activeDesktop', 'desktopList', 'desktopName', 'switchDesktop',
  'openOverview', 'closeOverview', 'toggleOverview', 'toggleShowDesktop', 'snapZoneAt', 'startDeskWidgets', 'widgetPicker',
  'statusItems', 'useIndicators', 'useThumb', 'openCalendarApp', 'webHelpItems', 'menuAction', 'startView', 'guestAppsMode',
  'takeTextFocus', 'releaseTextFocus', 'restoreFocus', 'textFocusOwner', 'focusToken', 'setBannerPlacement', 'useSystemInfo',
  'useCpuUsage', 'memUsedKB', 'mediaFormatTime', 'lordosWallpapers', 'nightActive', 'dndActive', 'useThemeFont'];
// read access to a system model, by the capability theme.json declares; undeclared, a neutral value
const CAPABILITY = {
  wifi: ['wifiTile', 'wifiTileView', 'wifiBars'],
  bluetooth: ['useBluetooth', 'BluetoothGlyph'],
  sound: ['useSound', 'defaultSink', 'useSoundOutputs'],
  battery: ['useBattery', 'BatteryGlyph'],
  brightness: ['useBacklight'],
  media: ['useNowPlaying', 'NowPlayingTile', 'useLinuxPlayers'],
};
const NEUTRAL = { useBluetooth: () => null, useSound: () => ({ sinks: [], sources: [], apps: [] }), defaultSink: () => null,
  useSoundOutputs: () => [], useBattery: () => null, useBacklight: () => null, useNowPlaying: () => null, useLinuxPlayers: () => [],
  wifiTile: () => null, wifiTileView: () => null, wifiBars: () => 0 };

function exportValue(name, v) {
  if (typeof v === 'function' && /^[A-Z]/.test(name)) { const W = wrapComponent(v, name); facadeTypes.add(W); return W; }
  if (typeof v === 'function') return (...a) => toTheme(v(...a.map((x) => sanitizeIn(x))));
  return v;
}

export function makeThemeSdk({ id, caps }) {
  const has = new Set(caps);
  const sdk = {};
  for (const n of COMPONENTS) if (SDK[n] !== undefined) sdk[n] = exportValue(n, SDK[n]);
  for (const n of FUNCTIONS) if (SDK[n] !== undefined) sdk[n] = exportValue(n, SDK[n]);
  for (const [cap, names] of Object.entries(CAPABILITY)) {
    for (const n of names) {
      if (SDK[n] === undefined) continue;
      sdk[n] = has.has(cap) ? exportValue(n, SDK[n]) : (/^[A-Z]/.test(n) ? () => null : NEUTRAL[n] || (() => null));
    }
  }
  // the host components a theme draws with: marked as its own nodes (THEME_OWNED, lord.themeOwn)
  for (const [n, type] of [['View', 'view'], ['Text', 'text'], ['Image', 'image']]) { sdk[n] = wrapComponent(type, n); facadeTypes.add(sdk[n]); }
  for (const [n, C] of [['Layer', ThemeLayer], ['Overlay', ThemeOverlay], ['WindowShell', ThemeWindowShell]]) { facadeTypes.add(C); sdk[n] = C; }
  // objects
  sdk.wm = themeWm();
  sdk.configureWindows = themeConfigureWindows;
  sdk.screen = Object.freeze({ get width() { return SDK.screen.width; }, get height() { return SDK.screen.height; },
    get displays() { return SDK.screen.displays.map((d) => ({ ...d })); }, get mirror() { return SDK.screen.mirror; } });
  sdk.screenDisplays = () => SDK.screenDisplays().map((d) => ({ ...d }));
  // logging out: marked the theme's (its crash count stays, main.c), then done by LordOS's own code in a job of its own,
  // so saving the session before it goes isn't under the theme's policy
  const quitForTheme = () => SDK.session.quit();
  sdk.session = Object.freeze({ lock: () => SDK.session.lock(), quit: () => { lord.themes.quitByTheme(); Promise.resolve().then(quitForTheme); }, canPower: () => SDK.session.canPower?.(),
    // a countdown the theme starts never confirms by itself: only the person's real click or key does (powermodel.js)
    restart: () => SDK.session.restart({ byTheme: true }), shutdown: () => SDK.session.shutdown({ byTheme: true }), managed: () => SDK.session.managed() });
  sdk.spotlight = Object.freeze({ toggle: (d) => SDK.spotlight.toggle(sanitizeIn(d)), open: (d) => SDK.spotlight.open?.(sanitizeIn(d)), close: () => SDK.spotlight.close?.() });
  sdk.keyboard = Object.freeze({ label: (x) => SDK.keyboard.label(sanitizeIn(x)), active: () => SDK.keyboard.active(), next: () => SDK.keyboard.next() });
  sdk.settings = Object.freeze({ get: () => snapshot(SDK.settings.get()), subscribe: (fn) => SDK.settings.subscribe(wrapCallback(fn)), set: (p) => settingsPatch(id, p) });
  const setSettings = (p) => settingsPatch(id, p);
  sdk.useSettings = () => [SDK.useStore(SDK.settings, snapshot), setSettings];
  sdk.useTrackedSettings = sdk.useSettings;
  sdk.useStore = themeUseStore;
  sdk.createStore = themeCreateStore;
  sdk.locale = readOnly(SDK.locale);
  sdk.snapPreview = readOnly(SDK.snapPreview);
  sdk.dockDrag = readOnly(SDK.dockDrag);
  sdk.dockPoof = readOnly(SDK.dockPoof);
  sdk.dockDragJustEnded = SDK.dockDragJustEnded ? exportValue('dockDragJustEnded', SDK.dockDragJustEnded) : undefined;
  sdk.overview = readOnly(SDK.overview);
  sdk.notifyPrefs = readOnly(SDK.notifyPrefs);
  sdk.dock = SDK.dock ? Object.freeze({ pin: (x, i) => SDK.dock.pin(sanitizeIn(x), i === undefined ? undefined : num(i)), move: (x, i) => SDK.dock.move(sanitizeIn(x), num(i)), unpin: (x) => SDK.dock.unpin(sanitizeIn(x)) }) : undefined;
  sdk.sound = has.has('sound') ? Object.freeze({ available: () => SDK.sound.available?.() }) : Object.freeze({ available: () => false });
  // a style pack (sdk/appskins.jsx): its data only, pointer colours, tunes, an installed font and icons as image files of
  // its own; component icons and app skins are not a community theme's (they would draw inside LordOS's apps)
  sdk.registerStylePack = (raw) => {
    const p = sanitizeIn(raw) || {};
    const imgs = (o) => Object.fromEntries(Object.entries(o && typeof o === 'object' ? o : {}).filter(([, v]) => typeof v === 'string'));
    const ic = p.icons && typeof p.icons === 'object' ? p.icons : {};
    SDK.registerStylePack({ cursors: p.cursors, sounds: p.sounds, fonts: p.fonts, icons: { apps: imgs(ic.apps), files: imgs(ic.files), toolbar: imgs(ic.toolbar) } });
  };
  sdk.addListener = themeAddListener;
  sdk.holdKeyboard = themeHoldKeyboard;
  sdk.useShortcuts = themeUseShortcuts;
  sdk.registerGestureActions = (a) => SDK.registerGestureActions(sanitizeIn(a) || {});
  // notifications: showing one is allowed; a notification centre reads the history as plain, read-only data (with the
// capability): no actions, no dismissing, no clearing
  sdk.notify = (raw) => { const n = sanitizeIn(raw); const str = (v) => (typeof v === 'string' || typeof v === 'number' ? String(v) : ''); return SDK.notify({ title: str(n?.title), body: str(n?.body), app: 'theme' }); };
  if (has.has('notifications')) {
    sdk.notifyHistory = readOnly(SDK.notifyHistory, notesData);
    sdk.useNotifications = () => notesData(SDK.useNotifications());
    sdk.clearNotifyHistory = () => {};          // read-only: the history is the person's (Settings, LordOS's own centre)
  } else {
    sdk.notifyHistory = Object.freeze({ get: () => [], subscribe: () => () => {} });
    sdk.useNotifications = () => [];
    sdk.clearNotifyHistory = () => {};
  }
  sdk.startShell = (el) => startThemeShell(el);
  sdk.ThemeProvider = facadeComponent(SDK.ThemeProvider, 'ThemeProvider');
  sdk.useTheme = () => SDK.useTheme();
  sdk.themeId = id;
  for (const k of Object.keys(sdk)) if (sdk[k] === undefined) delete sdk[k];
  return Object.freeze(sdk);
}
function facadeComponent(C, name) { const W = wrapComponent(C, name); facadeTypes.add(W); return W; }

// ---- the shell ----
// startShell only keeps the theme's element: LordOS's code starts the shell (its services: alarms, mail, the network
// item…) after the theme's code has run, from lord-server (js.c start_community → __lordThemeHost.run), never on the
// theme's behalf, so none of that runs under the theme's policy.
let pending = null;
function startThemeShell(el) {
  if (pending === null) pending = el;
}
export function runThemeShell() {
  if (pending === null) return false;
  const el = pending;
  pending = undefined;
  // the first commit with the theme's tree on screen tells the watchdog it drew (main.c community_watch)
  const off = onCommit(() => { off(); try { lord.themes.started(); } catch (e) { /* older server */ } });
  SDK.startShell(el);
  return true;
}

// React's host guard: a host node whose nearest component is the theme's own function was forged by the theme (it
// renders View, Text and Image from the SDK, never a host element itself)
const readRender = (t) => (typeof t.render === 'function' ? t.render : t.type);
function guardHost(_type, props, fiber) {
  // the backstop: every host node's element was made by LordOS's own React (runtime/react-made.cjs records them)
  if (!React.lordMadeProps || !React.lordMadeProps(props)) throw new TypeError('a community theme draws with View, Text and Image from @lordos/sdk');
  for (let f = fiber && fiber.return; f; f = f.return) {
    const t = f.type;
    const fn = typeof t === 'function' ? t : t && typeof t === 'object' ? lord.themeCall(readRender, t) : null;
    if (typeof fn === 'function') {
      if (isThemeFn(fn)) throw new TypeError('a community theme draws with View, Text and Image from @lordos/sdk');
      return;
    }
  }
}

// ---- lockdown: LordOS's realm before the theme's first line runs ----
// No code from text (eval and every Function constructor throw), and every built-in frozen, so nothing the theme can
// reach through a prototype changes how LordOS's code behaves.
function freezeDeep(o, seen) {
  if (o === null || (typeof o !== 'object' && typeof o !== 'function') || seen.has(o)) return;
  seen.add(o);
  for (const k of Reflect.ownKeys(o)) {
    const d = Object.getOwnPropertyDescriptor(o, k);
    if (!d) continue;
    if ('value' in d) freezeDeep(d.value, seen);
    else { freezeDeep(d.get, seen); freezeDeep(d.set, seen); }
  }
  freezeDeep(Object.getPrototypeOf(o), seen);
  Object.freeze(o);
}
const INTRINSICS = ['Object', 'Function', 'Array', 'Number', 'parseFloat', 'parseInt', 'Boolean', 'String', 'Symbol', 'Date', 'Promise',
  'RegExp', 'Error', 'AggregateError', 'EvalError', 'RangeError', 'ReferenceError', 'SyntaxError', 'TypeError', 'URIError', 'JSON',
  'Math', 'Reflect', 'Map', 'Set', 'WeakMap', 'WeakSet', 'WeakRef', 'FinalizationRegistry', 'ArrayBuffer', 'SharedArrayBuffer',
  'DataView', 'Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array', 'Uint16Array', 'Int32Array', 'Uint32Array', 'Float32Array',
  'Float64Array', 'BigInt64Array', 'BigUint64Array', 'BigInt', 'Proxy', 'Atomics', 'escape', 'unescape', 'isFinite', 'isNaN',
  'encodeURI', 'decodeURI', 'encodeURIComponent', 'decodeURIComponent', 'Iterator', 'console', 'performance'];
let locked = false;
export function lockdown() {
  if (locked) return;
  locked = true;
  const g = globalThis;
  const refuse = function () { throw new TypeError('code from text is not allowed while a community theme runs'); };
  const ctors = [Function, Object.getPrototypeOf(async function () {}).constructor, Object.getPrototypeOf(function* () {}).constructor,
    Object.getPrototypeOf(async function* () {}).constructor];
  for (const C of ctors) Object.defineProperty(C.prototype, 'constructor', { value: refuse, writable: false, configurable: false });
  Object.defineProperty(g, 'Function', { value: refuse, writable: false, configurable: false });
  Object.defineProperty(g, 'eval', { value: refuse, writable: false, configurable: false });
  const seen = new Set();
  for (const n of INTRINSICS) if (g[n] !== undefined) freezeDeep(g[n], seen);
  for (const C of ctors) freezeDeep(C, seen);
  // the iterator, generator and async prototypes only reachable through instances
  for (const x of [[][Symbol.iterator](), new Map()[Symbol.iterator](), new Set()[Symbol.iterator](), ''[Symbol.iterator](),
    (function* () {})(), (async function* () {})(), /x/[Symbol.matchAll]?.('x')]) if (x) freezeDeep(Object.getPrototypeOf(x), seen);
  freezeDeep(refuse, seen);
}

// the globals the theme's realm gets: its own wrappers, never LordOS's objects (which it could change)
export function themeGlobals() {
  const log = (k) => (...a) => console[k](...a.map((x) => (typeof x === 'string' ? x : (() => { try { return JSON.stringify(x); } catch (e) { return String(x); } })())));
  const timer = (f) => (fn, ms, ...a) => f(typeof fn === 'function' ? () => themeCallSafe(fn, ...a) : () => {}, ms);
  return {
    console: Object.freeze({ log: log('log'), info: log('info'), debug: log('debug'), warn: log('warn'), error: log('error') }),
    setTimeout: timer(setTimeout), setInterval: timer(setInterval), clearTimeout: (i) => clearTimeout(i), clearInterval: (i) => clearInterval(i),
    requestAnimationFrame: (fn) => requestAnimationFrame((t) => themeCallSafe(fn, t)), cancelAnimationFrame: (i) => cancelAnimationFrame(i),
    queueMicrotask: (fn) => queueMicrotask(() => themeCallSafe(fn)),
    performance: Object.freeze({ now: () => performance.now() }),
  };
}

export function prepareThemeHost(id, caps) {
  setHostGuard(guardHost);
  captureCornerAlways();               // the privacy pill is LordOS's, above the theme, whatever the theme draws
  const sdk = makeThemeSdk({ id, caps: Array.isArray(caps) ? caps : [] });
  // the theme's tree dying (React's uncaught error) takes it off (the watchdog, main.c)
  setRootErrorHandler((e) => { try { lord.themes.fail(String(e?.message || e).slice(0, 200)); } catch (x) { /* none */ } });
  return {
    // the apps as copies: the theme passes them to configureWindows, and can't change LordOS's records
    sdk: Object.freeze({ react: themeReact(), sdk, apps: Object.freeze(hostApps.map((a) => Object.freeze({ ...a }))) }),
    globals: themeGlobals(),
  };
}
