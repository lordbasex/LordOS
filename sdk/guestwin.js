// guestwin.js — apps mode's guest side (docs/VIRTUALIZATION.md 7.2): when this LordOS runs inside a Machines guest,
// the host shows each of its windows as a window of the host's own desktop. lord-server's endpoint
// (server/src/ui/js_guestwin.c) is there only inside a guest; lordos-guest (tools/guest) relays it to the host.
//
// Every 100 ms while the endpoint exists: what the host asked (through lordos-guest) is applied — apps mode on or off,
// an action on a window this side listed, opening an app from the launcher's list — and the windows are published:
// id (a number kept per window), title, app name, the content's area in framebuffer pixels (without the theme's title
// bar), focused/minimized, owner (a Linux dialog's parent window). lord-server sends only what changed. Apps are
// opened only through launchApp, with an id from the list this side published (lord-server checks it too).
// While apps mode is on, `guestAppsMode` is true: the themes leave out the wallpaper, the Dock and the bars, so the
// host's picture of a window is the window alone; off (or lordos-guest gone) they come back.
import { createStore } from './store.js';
import { windows, wm, listApps, appContentNodes, windowTitle, onActiveDesktop } from './windows.js';
import { launchApp, windowAppName, windowLinuxApp } from './apps.js';
import { appName, locale } from './i18n.js';
import { mainDisplay } from '@lordos/runtime';

export const guestAppsMode = createStore(false);

const TICK_MS = 100;
const APPS_MS = 5000;
const ids = new Map();          // window id → number on the wire
const back = new Map();         // and back
let nextId = 1;
const numOf = (wid) => {
  let n = ids.get(wid);
  if (!n) { n = nextId++; ids.set(wid, n); back.set(n, wid); }
  return n;
};

const scaleOf = () => { const d = mainDisplay(); return d && d.scale ? d.scale / 100 : 1; };

// the content's area in logical pixels (the frame when the content isn't laid out)
function contentOf(w) {
  const n = appContentNodes.get(w.id);
  const l = n ? lord.getLayout(n) : null;
  if (l && l.length === 4 && l[2] > 0 && l[3] > 0) return { x: l[0], y: l[1], w: l[2], h: l[3] };
  const f = wm.frame(w);
  return { x: f.x, y: f.y, w: f.w, h: f.h };
}

export function guestWindowList() {
  const ws = windows.get();
  const k = scaleOf();
  const focused = wm.focused();
  const out = [];
  for (const w of [...ws].reverse()) {          // top to bottom
    const r = contentOf(w);
    const hidden = w.minimized || !onActiveDesktop(w);
    const a = w.args || {};
    const icon = String(w.id).startsWith('linux#') ? (windowLinuxApp(a.appId || '')?.icon || '') : '';
    out.push({
      id: numOf(w.id), title: windowTitle(w) || '', app: windowAppName(w) || '',
      x: Math.round(r.x * k), y: Math.round(r.y * k), w: Math.max(1, Math.round(r.w * k)), h: Math.max(1, Math.round(r.h * k)),
      focused: w.id === focused, minimized: !!hidden, topmost: !!(w.pip || w.fullscreen),
      owner: a.parent && ids.has(a.parent) ? ids.get(a.parent) : (a.parent ? numOf(a.parent) : 0),
      pid: 0, icon: typeof icon === 'string' && /^\/.*\.(png|svg)$/.test(icon) ? icon : '',
    });
  }
  // forget the numbers of windows that are gone
  const live = new Set(ws.map((w) => w.id));
  for (const [wid, n] of ids) if (!live.has(wid)) { ids.delete(wid); back.delete(n); }
  return out;
}

// the launcher's apps: LordOS apps by id, Linux apps as desktop:<id>, Flatpak apps as flatpak:<id> (launchApp's ids)
export function guestAppList() {
  const lang = locale.get().lang;
  const out = listApps().filter((a) => !a.builtin && !a.hidden).map((a) => ({ name: appName(a) || a.id, token: a.id }));
  try { for (const a of lord.apps.linux(lang) || []) if (!a.terminal && !a.noDisplay) out.push({ name: a.name || a.id, token: `desktop:${a.id}` }); } catch (e) { /* not in a session */ }
  try { if (typeof lord.apps.flatpak === 'function') for (const a of lord.apps.flatpak(lang) || []) out.push({ name: a.name || a.id, token: `flatpak:${a.id}` }); } catch (e) { /* none */ }
  return out.filter((a) => a.token && a.name).sort((a, b) => a.name.localeCompare(b.name)).slice(0, 256);
}

// one action the host asked for, on a window this side listed
export function applyGuestAction(ev) {
  const wid = back.get(ev.id);
  const w = wid ? wm.get(wid) : null;
  if (!w) return false;
  switch (ev.action) {
    case 'focus': case 'restore': wm.focus(wid); return true;
    case 'close': wm.close(wid); return true;
    case 'minimize': wm.minimize(wid); return true;
    case 'move': {
      const k = scaleOf(), c = contentOf(w), f = wm.frame(w);
      // the frame around the content keeps its margins (title bar, borders): only the content's area is the guest's
      const left = c.x - f.x, top = c.y - f.y, right = f.x + f.w - (c.x + c.w), bottom = f.y + f.h - (c.y + c.h);
      const x = ev.x / k, y = ev.y / k, cw = ev.w / k, ch = ev.h / k;
      wm.setFrame(wid, { x: Math.round(x - left), y: Math.round(y - top), w: Math.round(cw + left + right), h: Math.round(ch + top + bottom) });
      return true;
    }
    default: return false;
  }
}

let started = false;
export function startGuestWin() {
  if (started || typeof lord === 'undefined' || !lord.guestwin || !lord.guestwin.active()) return;
  started = true;
  let appsAt = -APPS_MS;
  let was = false;
  setInterval(() => {
    let evs = [];
    try { evs = lord.guestwin.poll() || []; } catch (e) { evs = []; }
    for (const ev of evs) {
      if (ev.type === 'apps') guestAppsMode.set(!!ev.on);
      else if (ev.type === 'act') applyGuestAction(ev);
      else if (ev.type === 'launch' && typeof ev.token === 'string' && guestAppList().some((a) => a.token === ev.token)) launchApp(ev.token);
    }
    const on = lord.guestwin.connected();
    if (!on) { if (was) { was = false; guestAppsMode.set(false); } return; }
    const now = Date.now();
    if (!was || now - appsAt > APPS_MS) { appsAt = now; lord.guestwin.apps(guestAppList()); }
    was = true;
    lord.guestwin.publish(guestWindowList());
  }, TICK_MS);
}
