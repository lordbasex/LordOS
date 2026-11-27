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
//   window {window}                  a window's frame node, box and sheets (a screenshot of it)
//   region {x, y, w, h}              whether a region shows some of an allowed window or a surface tied to one
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
//   <HandsCursor />   the agent's own cursor (docs/HANDS.md "The agent's cursor"): a violet arrow labelled with the name
//                     the client gave, "· reported", in a layer of its own below the system band (99.5), built once out
//                     of React's sight; lord-server moves it from C (server/src/hands/handscur.h). It also tells
//                     lord-server Settings → AI → Hands → Agent motion and reduce motion.
// Settings → AI → Hands turns it on for this session (lord.hands.serve; nothing on disk turns it on) and keeps the
// allowlist the person chose in settings.hands.allow.
import { createElement as h, Fragment, useEffect } from 'react';
import { View, Text, screen, hostRoots, instanceOf, overlayTree } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { t, appName } from './i18n.js';
import { settings } from './settings.js';
import { windows, wm, getApp, listApps, windowTitle, appContentNodes } from './windows.js';
import { textFocusBox } from './components.jsx';
import { openFileWithApps } from './sysopen.js';
import { launchApp, launcherAppsNow, linuxNamesIn, windowLinuxApp } from './apps.js';
import * as fs from './fs.js';
import {
  serializeTree, findNodes, makeRefs, own, allowed, appOfWindow, windowsList, matchApp, filterLog, SYSTEM_Z, blackoutNodes,
} from './handsmodel.js';
import { focusedNode } from './focus.js';
import { dialogStack } from './dialogkeys.js';
import { appDenied } from './handsdeny.js';

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
// Settings → AI → Hands → Agent motion: 'human' (the default on the live desktop) or 'machine'; kept with the person's
// settings (HandsCursor tells lord-server; a client's own choice, lordos-mcp's hands_mode, wins for that client)
export const handsMotionSaved = () => ((settings.get().hands || {}).motion === 'machine' ? 'machine' : 'human');
export function setHandsMotion(m) {
  const motion = m === 'machine' ? 'machine' : 'human';
  settings.set((s) => ({ ...s, hands: { ...(s.hands || {}), motion } }));
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
// the node, or one it is inside, is protected, denied, a secret field, or in the system band
const deniedOrProtected = (inst) => {
  for (let n = inst, d = 0; n && d < 4096; n = n.parent || n.host || null, d++) {
    if (own(n.props, 'protected') || own(n.props, 'handsDeny')) return true;
    const st = own(n.props, 'accessibilityState');
    if (st && own(st, 'secure') === true) return true;
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
      const out = windows.get().filter((w) => allowed(allow, appOfWindow(w)) && !w.minimized)
        .map((w) => ({ node: frameInst(w.id), z: 0, win: w.id, title: windowTitle(w) })).filter((r) => r.node);
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
  const roots = [{ node: f, z: 0, win: id, title: windowTitle(wm.get(id)) }];
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

// open_app's launches waiting for their window: token → { before: the window ids then, app, desktopId }
const launches = new Map();
let launchSeq = 0;

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
  // where a key would go: the text field with the keys and the node with the keyboard focus, each and every node it is
  // in (deniedOrProtected); their ids go back so lord-server checks them natively too (hands.c gate GATE_KEYS)
  focus(a) {
    const box = textFocusBox();
    let kf = null;
    try { kf = focusedNode(); } catch (e) { kf = null; }
    const f = frontWindow();
    const fi = f ? frameInst(f) : null;
    const held = [box, kf].filter((n) => n && !n.deleted);
    // a person-only app in front (sdk/handsdeny.js), or its frame in a denied place: no key, whatever holds the focus
    const frontDenied = !!(f && appDenied(appOfId(f))) || !!(fi && deniedOrProtected(fi));
    const secure = held.some(deniedOrProtected) || frontDenied || !!(f && wm.get(f)?.protected);
    const nodes = held.map((n) => n.id).filter((id) => typeof id === 'number' && id > 0);
    // a dialog that owns the keyboard (Enter answers it): one an agent never answers (handsDeny, protected, the system
    // band), or one whose root isn't known, refuses every key (fail closed)
    let dialog = false, dialogNode = 0;
    if (dialogStack.owner() !== null) {
      const dn = dialogStack.ownerNode();
      dialog = !dn || dn.deleted || deniedOrProtected(dn);
      dialogNode = dn && !dn.deleted && typeof dn.id === 'number' ? dn.id : 0;
    }
    // the field's box only (where the agent cursor rests and pulses while it types), never what is in it
    const fb = box && !box.deleted ? boxOf(box) : null;
    return { secure, nodes, dialog, dialogNode, box: fb ? fb.slice(0, 4) : null,
      allowed: !(Array.isArray(a.allow) && a.allow.length) || (!!f && allowed(a.allow, appOfId(f))) };
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
    // its sheets (a modal of its own: Settings → Wallpaper's sheet, a file dialog), painted over it as Screenshots does
    const sheets = (wm.sheetNodes ? wm.sheetNodes(id) : []).filter((n) => typeof n === 'number' && n > 0).slice(0, 8);
    return b ? { node, x: b[0], y: b[1], w: b[2], h: b[3], sheets } : { error: 'not-found' };
  },
  // with an allowlist: the region shows some of an allowed window, or of a menu, popover or sheet tied to one (handsWin);
  // everything else in it is painted black (blackout), as in a window's screenshot
  region(a) {
    const meets = (b) => !!b && a.x < b[0] + b[2] && a.x + a.w > b[0] && a.y < b[1] + b[3] && a.y + a.h > b[1];
    const shows = windows.get().some((w) => {
      if (w.minimized || !allowed(a.allow, appOfWindow(w))) return false;
      const node = wm.frameNode(w.id);
      return meets(node ? boxOf({ id: node }) : null);
    }) || tiedSurfaces(a.allow).some((n) => meets(boxOf(n)));
    return { shows };
  },
  // fail closed: everything is black but the allowed windows and the menus, popovers and sheets tied to one of them
  blackout(a) {
    const keep = new Set();
    for (const w of windows.get()) if (!w.minimized && allowed(a.allow, appOfWindow(w))) { const n = wm.frameNode(w.id); if (n) keep.add(n); }
    const kept = (n) => keep.has(n.id) || (typeof own(n.props, 'handsWin') === 'string' && allowed(a.allow, appOfId(own(n.props, 'handsWin'))));
    return { nodes: blackoutNodes(hostRoots().filter((r) => r.z < SYSTEM_Z), kept) };
  },
  open(a) {
    if (!a.app) return { error: 'not-found' };
    // LordOS's own apps first, then everything the launcher lists (Linux, Flatpak, web and guest apps), found as its
    // search finds them; a Linux app also by its .desktop Name in English (ws170 QA BUG-23)
    const own = listApps().map((x) => ({ id: x.id, name: appName(x) || x.name || x.id, plain: !x.package && !x.isolated }));
    const ownIds = new Set(own.map((x) => x.id));
    const en = new Map(linuxNamesIn('en').map((x) => [x.id, x.name]));
    const rest = launcherAppsNow().filter((x) => !ownIds.has(x.id))
      .map((x) => ({ id: x.id, name: x.name || x.id, desktopId: x.desktopId, aliases: en.has(x.id) ? [en.get(x.id)] : [], plain: false }));
    const app = matchApp([...own, ...rest], a.app);
    if (!app) return { error: 'not-found' };
    if (!allowed(a.allow, app.id)) return { error: 'allowlist' };
    if (appDenied(app.id)) return { error: 'hidden-from-agent' };   // a person-only app (sdk/handsdeny.js)
    if (app.plain) {
      const id = wm.open(app.id);
      return id ? { window: id, app: app.id } : { error: 'not-found' };
    }
    // an app that brings its own window (a Linux or Flatpak app, an isolated one): launched as the launcher does, and
    // its window is the one that appears next (hands.c waits for it through "ready", which names it; ws170 QA BUG-15)
    const before = new Set(windows.get().map((w) => w.id));
    if (!launchApp(app.id)) return { error: 'not-found' };
    const token = `launch-${++launchSeq}`;
    launches.set(token, { before, app: app.id, desktopId: app.desktopId || '' });
    if (launches.size > 8) launches.delete(launches.keys().next().value);
    return { window: token, app: app.id };
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
  // close_window and open_app wait for it to happen (hands.c PH_WAIT): the window is gone; how many nodes its tree has
  // now (0 before its frame and content are laid out). Only for the window the agent itself just closed or opened.
  gone(a) { return { gone: !wm.get(String(a.window || '')) }; },
  ready(a) {
    let id = String(a.window || '');
    const l = launches.get(id);
    if (l) {
      // the window the launch brought: a new one of that app (its .desktop's or Flatpak's, an isolated app's), else the
      // first new window at all; none yet: not ready
      const fresh = windows.get().filter((w) => !l.before.has(w.id));
      const ofApp = (w) => w.app === l.app || w.args?.appId === l.app || (w.args?.appId && windowLinuxApp(w.args.appId)?.id === l.app);
      const w = fresh.find(ofApp) || fresh[0];
      if (!w) return { n: 0 };
      id = w.id;
      const r = OPS.ready({ window: id });
      return { ...r, window: id };
    }
    const w = wm.get(id);
    if (!w) return { n: 0 };
    // the app's own content (AppHost's node), not the frame the theme draws around it before the app is there
    const cn = appContentNodes.get(id);
    const f = (cn && instanceOf(cn)) || null;
    let n = 0;
    const walk = (x, d) => {
      if (!x || d > 4096 || n > 4096) return;
      const b = x.type === 'raw' ? null : boxOf(x);
      if (b && b[2] > 0 && b[3] > 0) n++;
      for (let c = x.first; c; c = c.next) walk(c, d + 1);
    };
    if (f) for (let c = f.first; c; c = c.next) walk(c, 0);   // its children: the node itself is there with the frame
    // a Linux app's window is its surface: there once it was mapped with a size
    if (!n && w.args && w.args.surface && w.w > 0 && w.h > 0) n = 1;
    // an isolated app draws in its own process: its content node laid out is all there is to see here
    if (!n && f && getApp(appOfWindow(w))?.isolated) { const b = boxOf(f); if (b && b[2] > 0 && b[3] > 0) n = 1; }
    return { n };
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

// the shell surfaces tied to an allowed window (handsWin: its sheets, menus and popovers), below the system band
function tiedSurfaces(allow) {
  const out = [];
  const walk = (n, d) => {
    if (!n || d > 4096 || out.length > 64) return;
    const w = own(n.props, 'handsWin');
    if (typeof w === 'string' && allowed(allow, appOfId(w)) && n.id) { out.push(n); return; }
    for (let c = n.first; c; c = c.next) walk(c, d + 1);
  };
  for (const r of hostRoots()) if (r.z > 0 && r.z < SYSTEM_Z) walk(r.node, 0);
  return out;
}

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

// The agent's cursor: built once, out of React's sight, then only lord-server moves it. Violet arrow (a rounded tip at the
// point), a ripple ring and the client's name "· reported" (as the prompt says: what the program calls itself, not
// verified). The name comes from lord-server, cleaned there (printable, no bidi or control characters, 40 bytes at most)
// and is cut again here.
export const HANDS_CURSOR_Z = 99.5;                      // server/src/hands/handscur.h HANDSCUR_Z
const TIP = 24, VIOLET = '#8b5cf6';
let cursorNodes = null, cursorOk = false;
// the nodes once; lord.hands.cursor until lord-server takes them (false before the desktop's root is up, or if it ever
// refuses: HandsCursor tries again)
function buildCursor() {
  if (cursorOk) return cursorNodes;
  if (typeof lord === 'undefined' || !lord.hands || typeof lord.hands.cursor !== 'function') return null;
  if (!cursorNodes) {
    let ring = 0, text = 0;
    const root = overlayTree(HANDS_CURSOR_Z, { left: -1000, top: -1000, width: 240, height: 72, display: 'none', direction: 'ltr' }, (el, r) => {
      ring = el('view', { position: 'absolute', left: TIP - 5, top: TIP - 5, width: 10, height: 10, borderRadius: 5, border: `2px solid ${VIOLET}`, opacity: 0 }, r);
      // the arrow: a teardrop whose sharp corner is the point
      el('view', { position: 'absolute', left: TIP, top: TIP, width: 17, height: 17, backgroundColor: VIOLET, border: '2px solid #ffffff',
        borderTopLeftRadius: 1, borderTopRightRadius: 9, borderBottomRightRadius: 9, borderBottomLeftRadius: 9, boxShadow: '0 2px 6px rgba(0,0,0,0.45)' }, r);
      const pill = el('view', { position: 'absolute', left: TIP + 14, top: TIP + 19, height: 18, paddingHorizontal: 7, borderRadius: 9, justifyContent: 'center',
        backgroundColor: VIOLET, boxShadow: '0 2px 6px rgba(0,0,0,0.35)' }, r);
      const tx = el('text', { fontSize: 11, fontWeight: 600, color: '#ffffff', whiteSpace: 'nowrap' }, pill);
      text = lord.createNode('raw');
      lord.appendChild(tx, text);
    });
    if (!root) return null;
    cursorNodes = { root, ring, text, label: null };
  }
  try { cursorOk = lord.hands.cursor(cursorNodes.root, cursorNodes.ring) === true; } catch (e) { cursorOk = false; }
  return cursorNodes;
}
const RETRY_MS = 1000;
const cut = (str, n) => (str.length > n ? `${str.slice(0, n - 1)}…` : str);
export function HandsCursor() {
  const st = useStore(handsStatus);
  const motion = useStore(settings, (s) => `${(s.hands || {}).motion === 'machine' ? 'machine' : 'human'}|${!!s.reduceMotion}`);
  // the person's motion to lord-server; a change is refused while the agent owns the input (only the person's Settings
  // page changes it), so it is sent again until it is taken
  useEffect(() => {
    const a = api();
    if (!a || typeof a.motion !== 'function') return undefined;
    const [m, r] = motion.split('|');
    let timer = 0, live = true;
    const send = () => {
      try { a.motion(m, r === 'true'); } catch (e) { if (live) timer = setTimeout(send, RETRY_MS); }
    };
    send();
    return () => { live = false; if (timer) clearTimeout(timer); };
  }, [motion]);
  const granted = (st.clients || []).filter((c) => c.state === 'granted');
  const who = cut([...new Set(granted.map(clientName))].join(', '), 48);
  const label = t('hands.cursor.label', { client: who });
  useEffect(() => {
    let timer = 0, live = true;
    const apply = () => {
      const c = buildCursor();
      if (c && c.label !== label) {
        c.label = label;
        try { lord.setText(c.text, label); } catch (e) { /* gone */ }
      }
      // registration failed (the root not up yet, a refusal): again, until lord-server takes the nodes
      if (!cursorOk && live) timer = setTimeout(apply, RETRY_MS);
    };
    apply();
    return () => { live = false; if (timer) clearTimeout(timer); };
  }, [label, granted.length]);
  return null;
}

refreshHands();
