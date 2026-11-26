// hands.jsx — "Hands" (Manos): an AI agent uses the desktop (docs/HANDS.md; approved by the maintainer on 2026-11-26).
//
// lord-server decides everything (server/src/hands/hands.c): whether a client may act, where a click or a key may land,
// the rate. The shell answers what only it knows, through __lordHands(op, json) → JSON text:
//   status, windows                  the displays; windows_context's list (sdk/handsmodel.js windowsList)
//   read {window, filter, depth}     read_window: the UI tree as text with ref_N ids (handsmodel.js serializeTree)
//   find {window, query}             find
//   resolve {ref}                    a ref's node and box, and whether it is a secret field
//   target {node}                    whether what is at that node belongs to an allowed app
//   focus                            whether the keys would go to a password field, a denied node, an allowed app
//   setvalue {ref, value}            form_input: the control's own setter (handsSetValue), never a password field
//   window {window}                  a window's frame node and box (a screenshot of it)
//   region {x, y, w, h}              whether a region lies inside one allowed window
//   blackout                         the nodes an allowlisted screenshot paints black (other apps' windows, the Dock…)
//   open {app}, openPath {path}, close {window}, log {app, pattern, max}
// Each op gets the allowlist ("allow") from lord-server and applies it. The tree never holds the system band, a
// handsDeny node or a password's value (handsmodel.js).
//
// What the person sees, always drawn by LordOS and never by a theme:
//   <HandsPrompt />   in the system layer: "A program running on your account asked to control your computer", the name
//                     and the program path it reports (not verified: same-user programs can't be told apart), This
//                     session / Deny. One prompt per connection, answered only by the person's real input.
//   <HandsBanner />   in the agent layer (105, above every layer a theme draws in), on every display while an agent is
//                     connected: "<client> is using your computer" and Stop. Ctrl+Alt+Esc stops every agent too.
// Settings → AI → Hands turns it on for this session (lord.hands.serve; nothing on disk turns it on) and keeps the
// allowlist the person chose in settings.hands.allow.
import { createElement as h, Fragment } from 'react';
import { View, Text, screen, hostRoots, instanceOf } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { t, appName } from './i18n.js';
import { settings } from './settings.js';
import { windows, wm, getApp, listApps, windowTitle } from './windows.js';
import { textFocusBox } from './components.jsx';
import { openFileWithApps } from './sysopen.js';
import * as fs from './fs.js';
import {
  serializeTree, findNodes, makeRefs, own, allowed, appOfWindow, windowsList, matchApp, filterLog, SYSTEM_Z,
} from './handsmodel.js';

const api = () => (typeof lord !== 'undefined' && lord.hands && typeof lord.hands.serve === 'function' ? lord.hands : null);
export const handsStatus = createStore({ on: false, qa: false, allow: [], clients: [], events: 0 });
export const handsStopped = createStore(null);          // why the agents were stopped last ('stop', 'panic', 'locked', 'off')

// ---- the person's choices ----

export const handsAllowSaved = () => {
  const a = (settings.get().hands || {}).allow;
  return Array.isArray(a) ? a.filter((x) => typeof x === 'string' && /^[A-Za-z0-9._:#@+-]{1,128}$/.test(x)).slice(0, 16) : [];
};
// on for this session only: lord-server forgets it at the next login
export function setHandsOn(on, allow = handsAllowSaved()) {
  const a = api();
  if (!a) return;
  try { a.serve(!!on, allow); } catch (e) { /* an older lord-server */ }
  refreshHands();
}
export function setHandsAllow(list) {
  const allow = (list || []).slice(0, 16);
  settings.set((s) => ({ ...s, hands: { ...(s.hands || {}), allow } }));
  const st = handsStatus.get();
  if (st.on) setHandsOn(true, allow);
}
export function stopHands() { const a = api(); if (a) { try { a.stop(); } catch (e) { /* gone */ } } refreshHands(); }
export function answerHands(bus, yes) { const a = api(); if (a) { try { a.answer(bus, !!yes); } catch (e) { /* gone */ } } refreshHands(); }
export function refreshHands() {
  const a = api();
  if (!a) return;
  try { const st = a.status(); if (st) handsStatus.set(st); } catch (e) { /* older lord-server */ }
}

globalThis.__lordHandsEvent = (json) => {
  let e;
  try { e = JSON.parse(json); } catch (err) { return; }
  if (e.t === 'clients' && e.status) handsStatus.set(e.status);
  else if (e.t === 'stopped') handsStopped.set(typeof e.why === 'string' ? e.why : 'stop');
  else if (e.t === 'log' && typeof e.line === 'string' && /^hands: [a-z_ -]+$/.test(e.line)) console.log(e.line);
};

// ---- the tree ----

const refs = makeRefs();
const boxOf = (n) => { try { const b = lord.getLayout(n.id); return Array.isArray(b) && b.length >= 4 ? b : null; } catch (e) { return null; } };
// the window a host instance is in (its WindowShell's lordWin), and the layer it hangs from
function placeOf(inst) {
  let win = null, z = 0;
  for (let n = inst, d = 0; n && d < 4096; d++) {
    if (!win && own(n.props, 'lordWin')) win = own(n.props, 'lordWin');
    const up = n.parent || n.host || null;
    if (!up) break;
    if (up.isContainer) { z = up.layer || 0; break; }
    n = up;
  }
  return { win, z };
}
const appOfId = (id) => appOfWindow(wm.get(id) || { id });
const frontWindow = () => { try { return wm.focused(); } catch (e) { return null; } };
// what is at a host instance belongs to an allowed app: its window's, or for the menu bar, menus and sheets the window
// in front's; never the Dock, the launcher, the desktop or a notification under an allowlist
function instAllowed(inst, allow) {
  if (!Array.isArray(allow) || !allow.length) return true;
  const { win, z } = placeOf(inst);
  if (win) return allowed(allow, appOfId(win));
  if (z === 20 || z === 50 || z === 60) { const f = frontWindow(); return !!f && allowed(allow, appOfId(f)); }
  return false;
}
const deniedOrProtected = (inst) => {
  for (let n = inst, d = 0; n && d < 4096; n = n.parent || n.host || null, d++) {
    if (own(n.props, 'protected') || own(n.props, 'handsDeny')) return true;
    if (n.isContainer) return (n.layer || 0) >= SYSTEM_Z;
  }
  return false;
};
const frameInst = (id) => { const node = wm.frameNode(id); return node ? instanceOf(node) : null; };
const layer = (z) => hostRoots().find((r) => r.z === z);

// the roots to read: "desktop" every layer below the system band; a window (or "" the one in front) with the menu
// bar, the menus and the sheets when it is in front
function rootsFor(window, allow) {
  if (window === 'desktop') {
    if (Array.isArray(allow) && allow.length) {
      const out = windows.get().filter((w) => allowed(allow, appOfWindow(w)) && !w.minimized).map((w) => ({ node: frameInst(w.id), z: 0 })).filter((r) => r.node);
      if (allowed(allow, appOfId(frontWindow()))) for (const z of [20, 50, 60]) { const l = layer(z); if (l) out.push(l); }
      return { roots: out };
    }
    return { roots: hostRoots().filter((r) => r.z < SYSTEM_Z) };
  }
  const id = window || frontWindow();
  if (!id || !wm.get(id)) return { error: 'not-found' };
  if (!allowed(allow, appOfId(id))) return { error: 'allowlist' };
  const f = frameInst(id);
  if (!f) return { error: 'not-found' };
  const roots = [{ node: f, z: 0 }];
  if (id === frontWindow()) for (const z of [20, 50, 60]) { const l = layer(z); if (l && l.node.first) roots.push(l); }
  return { roots };
}

function displayOf(w) {
  const f = w.maximized ? (wm.frame ? wm.frame(w) : w) : w;
  const cx = (f.x || 0) + (f.w || 0) / 2, cy = (f.y || 0) + (f.h || 0) / 2;
  const d = screen.displays.find((x) => cx >= x.x && cx < x.x + x.w && cy >= x.y && cy < x.y + x.h) || screen.displays[0];
  return d ? d.id : 0;
}
const kindOf = (w) => (w.args && w.args.surface ? 'linux' : getApp(appOfWindow(w))?.package ? 'package' : 'native');

const OPS = {
  status(a) {
    return { displays: screen.displays.map((d) => ({ id: d.id, name: d.name || '', x: d.x, y: d.y, w: d.w, h: d.h, scale: d.scale || 100, main: !!d.main })),
      width: screen.width, height: screen.height, focused: frontWindow(), allow: a.allow || [] };
  },
  windows(a) {
    const list = windows.get().map((w) => ({ ...w, frame: wm.frame ? wm.frame(w) : w }));
    return { windows: windowsList(list, { focusedId: frontWindow(), displayOf, titleOf: (w) => windowTitle(w), allow: a.allow, kindOf }) };
  },
  read(a) {
    refs.prune();
    const r = rootsFor(a.window || '', a.allow);
    if (r.error) return r;
    const f = textFocusBox();
    return { text: serializeTree(r.roots, { box: boxOf, refs, filter: a.filter, depth: a.depth, focused: (n) => n === f }).text };
  },
  find(a) {
    const r = rootsFor(a.window || '', a.allow);
    if (r.error) return r;
    return { text: findNodes(r.roots, a.query, { box: boxOf, refs }).text || '' };
  },
  resolve(a) {
    const n = refs.nodeOf(a.ref);
    if (!n || n.type === 'raw') return { error: 'not-found' };
    if (!instAllowed(n, a.allow)) return { error: 'allowlist' };
    const b = boxOf(n);
    if (!b) return { error: 'not-found' };
    const st = own(n.props, 'accessibilityState');
    const secure = deniedOrProtected(n) || !!(st && own(st, 'secure') === true);
    return { node: n.id, x: b[0], y: b[1], w: b[2], h: b[3], secure };
  },
  target(a) {
    const inst = a.node ? instanceOf(a.node) : null;
    return { allowed: inst ? instAllowed(inst, a.allow) : !(Array.isArray(a.allow) && a.allow.length) };
  },
  focus(a) {
    const box = textFocusBox();
    const f = frontWindow();
    const fi = f ? frameInst(f) : null;
    const secure = !!(box && deniedOrProtected(box)) || !!(fi && own(fi.props, 'protected')) || !!(f && wm.get(f)?.protected);
    const denied = !!(box && own(box.props, 'handsDeny'));
    return { secure, denied, allowed: !(Array.isArray(a.allow) && a.allow.length) || (!!f && allowed(a.allow, appOfId(f))) };
  },
  setvalue(a) {
    const n = refs.nodeOf(a.ref);
    if (!n) return { error: 'not-found' };
    if (!instAllowed(n, a.allow)) return { error: 'allowlist' };
    const st = own(n.props, 'accessibilityState');
    if (deniedOrProtected(n) || (st && own(st, 'secure') === true)) return { error: 'password' };
    const set = own(n.props, 'handsSetValue');
    if (typeof set !== 'function') return { error: 'not-editable' };
    try { if (set(String(a.value)) === false) return { error: 'not-editable' }; } catch (e) { return { error: 'not-editable' }; }
    return { ok: true };
  },
  window(a) {
    const id = a.window || frontWindow();
    const w = id ? wm.get(id) : null;
    if (!w || w.minimized) return { error: 'not-found' };
    if (!allowed(a.allow, appOfWindow(w))) return { error: 'allowlist' };
    const node = wm.frameNode(id);
    const b = node ? boxOf({ id: node }) : null;
    return b ? { node, x: b[0], y: b[1], w: b[2], h: b[3] } : { error: 'not-found' };
  },
  region(a) {
    const inside = windows.get().some((w) => {
      if (w.minimized || !allowed(a.allow, appOfWindow(w))) return false;
      const node = wm.frameNode(w.id);
      const b = node ? boxOf({ id: node }) : null;
      return !!b && a.x >= b[0] && a.y >= b[1] && a.x + a.w <= b[0] + b[2] && a.y + a.h <= b[1] + b[3];
    });
    return { inside };
  },
  blackout(a) {
    const nodes = [];
    for (const w of windows.get()) if (!allowed(a.allow, appOfWindow(w))) { const n = wm.frameNode(w.id); if (n) nodes.push(n); }
    // the Dock, the launcher and search, the notifications: no app's window, so none of an allowed one
    for (const z of [10, 30, 40, 95]) { const l = layer(z); for (let c = l && l.node.first; c; c = c.next) nodes.push(c.id); }
    // the desktop itself (the wallpaper's widgets): everything of the theme's tree but the windows
    return { nodes };
  },
  open(a) {
    if (!a.app) return { error: 'not-found' };
    const apps = listApps().map((x) => ({ id: x.id, name: appName(x) || x.name || x.id }));
    const app = matchApp(apps, a.app);
    if (!app) return { error: 'not-found' };
    if (!allowed(a.allow, app.id)) return { error: 'allowlist' };
    const id = wm.open(app.id);
    return id ? { window: id, app: app.id } : { error: 'not-found' };
  },
  openPath(a) {
    if (Array.isArray(a.allow) && a.allow.length) return { error: 'allowlist' };   // which app opens it isn't known before
    const home = fs.home();
    const p = String(a.path || '');
    if (!(p === home || p.startsWith(`${home}/`) || p.startsWith(`/media/`))) return { error: 'outside' };
    let st = null;
    try { st = fs.stat(p); } catch (e) { st = null; }
    if (!st) return { error: 'not-found' };
    if (st.dir) { wm.open('files', { new: true, args: { path: p } }); return { opened: true, folder: true }; }
    const ok = openFileWithApps(p, p.split('/').pop(), { untrusted: true });   // a file that would run is only shown in Files
    return { opened: !!ok };
  },
  close(a) {
    const w = wm.get(a.window);
    if (!w) return { error: 'not-found' };
    if (!allowed(a.allow, appOfWindow(w))) return { error: 'allowlist' };
    return { closed: wm.close(a.window) !== false };
  },
  log(a) {
    if (!allowed(a.allow, a.app)) return { error: 'allowlist' };
    if (!lord.logs || typeof lord.logs.files !== 'function') return { error: 'not-found' };
    const id = String(a.app || '');
    const files = lord.logs.files().map((f) => f.name).filter((n) => typeof n === 'string');
    // the app's own files (an isolated app's or a Linux app's, a web app's), else its lines in shell.log
    const own_ = files.filter((n) => n === `apps/${id}.log` || (n.startsWith(`web/${id}-`) && n.endsWith('.log')));
    let text = '';
    if (own_.length) for (const n of own_) { try { text += `${lord.logs.tail(n, 262144) || ''}\n`; } catch (e) { /* unreadable */ } }
    else if (files.includes('shell.log')) {
      try { text = String(lord.logs.tail('shell.log', 524288) || '').split('\n').filter((l) => l.includes(id)).join('\n'); } catch (e) { text = ''; }
    }
    return { text: filterLog(text, a.pattern, a.max || 100) };
  },
};

globalThis.__lordHands = (op, json) => {
  let a;
  try { a = JSON.parse(json); } catch (e) { return JSON.stringify({ error: 'failed' }); }
  const fn = Object.prototype.hasOwnProperty.call(OPS, op) ? OPS[op] : null;
  if (!fn) return JSON.stringify({ error: 'failed' });
  try { return JSON.stringify(fn(a || {})); } catch (e) { console.error('hands:', e?.message || e); return JSON.stringify({ error: 'failed' }); }
};

// ---- what the person sees ----

const clientName = (c) => (c && c.name) || t('hands.client.unknown');

export function HandsPrompt() {
  const st = useStore(handsStatus);
  const c = (st.clients || []).find((x) => x.state === 'asking');
  if (!c) return null;
  const m = screen.displays.find((d) => d.main) || screen.displays[0] || { x: 0, y: 0, w: screen.width, h: screen.height };
  return h(View, { handsDeny: true, style: { position: 'absolute', left: m.x, top: m.y, width: m.w, height: m.h, alignItems: 'center', backgroundColor: 'rgba(0,0,0,0.35)' } },
    h(View, { style: { marginTop: 90, width: 400, borderRadius: 14, padding: 20, gap: 10, backgroundColor: '#2b2b30', border: '1px solid rgba(255,255,255,0.12)',
      boxShadow: '0 20px 60px rgba(0,0,0,0.5)' } },
      // honest about what is known (docs/HANDS.md "Threat model"): a program of this account asked; its name and path are
      // what it reports, never verified. The person's own click here is the control.
      h(Text, { style: { fontSize: 15, fontWeight: 700, color: '#f5f5f7', textAlign: 'start', lineHeight: 1.35 } }, t('hands.prompt.title')),
      h(Text, { style: { fontSize: 12, color: 'rgba(235,235,245,0.75)', textAlign: 'start', lineHeight: 1.4 } }, t('hands.prompt.what')),
      h(Text, { style: { fontSize: 12, color: '#f5f5f7', textAlign: 'start' } }, t('hands.prompt.name', { client: clientName(c) })),
      h(Text, { style: { fontSize: 11, color: 'rgba(235,235,245,0.8)', textAlign: 'start' } }, t('hands.prompt.program', { program: c.exe || t('hands.prompt.unknownProgram') })),
      h(Text, { style: { fontSize: 11, color: 'rgba(235,235,245,0.55)', textAlign: 'start' } }, t('hands.prompt.reported')),
      h(Text, { style: { fontSize: 11, color: '#ffd60a', textAlign: 'start', lineHeight: 1.4 } }, t('hands.prompt.never')),
      h(View, { style: { flexDirection: 'row', justifyContent: 'flex-end', gap: 10, marginTop: 6 } },
        h(PromptBtn, { label: t('hands.prompt.deny'), onClick: () => answerHands(c.bus, false) }),
        h(PromptBtn, { primary: true, label: t('hands.prompt.allow'), onClick: () => answerHands(c.bus, true) }))));
}

function PromptBtn({ label, primary, onClick }) {
  return h(View, { onClick, style: { paddingVertical: 7, paddingHorizontal: 16, borderRadius: 8, backgroundColor: primary ? '#0a84ff' : 'rgba(255,255,255,0.1)' } },
    h(Text, { style: { fontSize: 13, fontWeight: 600, color: '#fff' } }, label));
}

// one pill at the top of every display while an agent is connected and granted
export function HandsBanner() {
  const st = useStore(handsStatus);
  const active = (st.clients || []).filter((c) => c.state === 'granted');
  if (!active.length) return null;
  const who = [...new Set(active.map(clientName))].join(', ');
  const ds = screen.displays.length ? screen.displays : [{ id: 0, x: 0, y: 0, w: screen.width, h: screen.height }];
  return h(Fragment, null, ds.map((d) => h(View, {
    key: d.id, handsDeny: true,
    style: { position: 'absolute', top: d.y + 6, left: d.x + Math.round(d.w / 2) - 210, width: 420, height: 34, flexDirection: 'row', alignItems: 'center',
      gap: 10, paddingHorizontal: 14, borderRadius: 17, backgroundColor: 'rgba(191,90,242,0.95)', boxShadow: '0 6px 18px rgba(0,0,0,0.35)' },
  },
  h(View, { style: { width: 8, height: 8, borderRadius: 4, backgroundColor: '#fff' } }),
  h(Text, { style: { flex: 1, fontSize: 12, fontWeight: 600, color: '#fff', whiteSpace: 'nowrap', textOverflow: 'ellipsis', overflow: 'hidden', textAlign: 'start' } },
    t('hands.banner.using', { client: who })),
  h(Text, { style: { fontSize: 11, color: 'rgba(255,255,255,0.8)', whiteSpace: 'nowrap' } }, t('hands.banner.panic')),
  h(View, { onClick: stopHands, style: { paddingHorizontal: 12, height: 24, borderRadius: 12, justifyContent: 'center', backgroundColor: '#fff' } },
    h(Text, { style: { fontSize: 12, fontWeight: 700, color: '#6b21a8' } }, t('hands.banner.stop'))))));
}

refreshHands();
