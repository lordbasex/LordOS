// wayland.js — ventanas de apps Linux (GTK, Qt, foot…) que se conectan al compositor Wayland
// de lord-server (server/src/wl). Para el tema son ventanas como las de las apps LordOS: entran
// en el gestor con la app interna 'linux' y su id es 'linux#<superficie>'.
//
// Those that ask for server-side decoration (Qt, foot) get the theme's frame; those that draw their
// own (GTK, Chromium and Firefox with their own bar) go "frameless": args.csd = true, and the theme
// draws only the content. The app may switch while open ('deco'). Moving, resizing and the window
// menu from the app's own bar arrive as requests from the compositor.
import { createElement as h, Fragment } from 'react';
import { View, Text, Surface, addListener, onShellKeyboard, shellHasKeyboard, displayFor, screen, onScreenChange } from '@lordos/runtime';
import { windows, wm, registerBuiltinApp, windowChrome, pointerWorkArea, openingArea, getApp, placeLinuxWindow, activeDesktop, overviewShowing, workAreaOf, isWindowLocked, windowLocks } from './windows.js';
import { followParents, overParent, lateParentFrame } from './dialogmodel.js';
import { tiledEdges } from './snap.js';
import { acceptsAppRequest, activationOutcome, keyboardSurface, focusedOn, surfaceState, coveredIds, isNoticeWindow, noticeFrame, NOTICE_MAX_W, NOTICE_MAX_H, ownLayer, layerName } from './desktopmodel.js';
import { desktopOfSurface } from './desktops.js';
import { useAppWindow } from './components.jsx';
import { locale, appName, direction } from './i18n.js';
import { linuxAppFor, windowLinuxApp, windowPixelIcons } from './apps.js';
import { shortcutsWho } from './kbinhibitmodel.js';
import { webOwnerOf } from './webapps.js';
import { osk } from './osk.js';
import { ime } from './ime.js';
import { rememberedFrame } from './windowmemory.js';
import { menusWithSpeech } from './voice.js';
import { settings } from './settings.js';
import { dockIdOfWindow } from './dock.js';
import { promptForServer } from './privacy.js';
import { fitFrame } from './sessionmodel.js';
import { chromeInset, dragFrameAt, tagKey, placeKeyOf, descriptionTitle, restoredFrame, bellAction, blurBoxes, BLUR_RADIUS } from './shellxmodel.js';
import { dndActive } from './notify.js';
import { sound } from './sound.js';

// the launcher entry a new window belongs to (as sdk/dock.js launcherIdOf), for its assigned display
const launcherKeyFor = (appId) => (!appId ? null : getApp(appId)?.package || getApp(appId)?.isolated ? appId : windowLinuxApp(appId)?.id || null);

const idOf = (sid) => `linux#${sid}`;
// What a Linux window's frame adds around its content (sdk/shellxmodel.js chromeInset): the theme's borders and title bar
// and, in a theme with a menu strip per window (Lord Classic, Lord Workspaces, Celeste), that strip when it shows one,
// as every Linux window with the Edit menu with Speech does. win: the window, or { id, args } of one about to open.
const frameInset = (win) => chromeInset(windowChrome(), !!win?.args?.csd, !win?.args?.csd && !!menusWithSpeech(win.id, undefined, win)?.length);
export const surfaceOf = (wid) => (String(wid).startsWith('linux#') ? Number(String(wid).slice(6)) || 0 : 0);

// ext-background-effect: blurred boxes right under the surface, where the app asked (server/src/wl/bgeffect.c; lord-server
// sends them only while blur can really be drawn). The surface keeps its key, so it stays the same node.
function LinuxWindow() {
  const { args } = useAppWindow();
  const blur = args?.blur?.length ? args.blur : null;
  const surface = h(Surface, { key: 's', surface: args?.surface || 0, style: { flex: 1 } });
  if (!blur) return surface;
  return h(Fragment, null, ...blur.map((b, i) => h(View, { key: `b${i}`, style: { position: 'absolute', left: b.x, top: b.y, width: b.w, height: b.h,
    backdropFilter: `blur(${BLUR_RADIUS}px)`, pointerEvents: 'none' } })), surface);
}

// Ícono genérico (los temas que dibujan app.Icon lo necesitan): un pingüino sobre gris.
function LinuxIcon({ size: s = 32 }) {
  return h(View, { style: { width: s, height: s, borderRadius: s * 0.225, alignItems: 'center', justifyContent: 'center',
    background: 'linear-gradient(180deg, #5b6573, #353c47)' } },
  h(Text, { style: { fontSize: s * 0.58, lineHeight: s } }, '🐧'));
}

registerBuiltinApp({ id: 'linux', name: 'Linux', builtin: true, size: [800, 600], minSize: [160, 100], Component: LinuxWindow, Icon: LinuxIcon });

/* arrastre pedido por la app (xdg_toplevel.move/resize): sigue al puntero hasta soltar */
let drag = null;
addListener('pointermove', (e) => {
  if (!drag) return;
  const w = wm.get(drag.id);
  if (!w || isWindowLocked(drag.id)) { drag = null; return; }
  if (!drag.start || drag.start.x == null) { drag.start = { x: e.x, y: e.y, f: wm.frame(w) }; return; }
  const dx = e.x - drag.start.x, dy = e.y - drag.start.y, f = drag.start.f;
  if (drag.edges == null && w.maximized) {
    // out of the maximized place only once the pointer really moved (a click's jitter keeps it), at
    // its own size and under the pointer where it was grabbed
    if (Math.abs(dx) + Math.abs(dy) < 6) return;
    const rel = f.w ? Math.min(1, Math.max(0, (drag.start.x - f.x) / f.w)) : 0.5;
    drag.start = { x: e.x, y: e.y, f: { ...f, x: Math.round(e.x - rel * w.w), y: f.y + dy, w: w.w, h: w.h } };
    wm.move(drag.id, drag.start.f.x, drag.start.f.y);
    return;
  }
  if (drag.edges == null) { wm.move(drag.id, f.x + dx, f.y + dy); return; }
  const E = drag.edges;      // 1 arriba, 2 abajo, 4 izquierda, 8 derecha
  let { x, y, w: ww, h: hh } = f;
  if (E & 8) ww = Math.max(160, f.w + dx);
  if (E & 2) hh = Math.max(100, f.h + dy);
  if (E & 4) { ww = Math.max(160, f.w - dx); x = f.x + f.w - ww; }
  if (E & 1) { hh = Math.max(100, f.h - dy); y = f.y + f.h - hh; }
  wm.setFrame(drag.id, { x, y, w: ww, h: hh });
});
addListener('pointerup', () => { drag = null; });

// xdg-toplevel-drag (server/src/wl/tldrag.c): a window that follows a drag, a browser tab torn off. Its geometry's
// (ox, oy) point stays under the pointer; lord-server skips its frame when choosing where the drag goes.
let tdrag = null;
function followDrag(px, py) {
  const w = tdrag && wm.get(tdrag.id);
  if (!w || isWindowLocked(tdrag.id)) { tdrag = null; return; }
  let g = null;
  try { g = lord.wayland.toplevelDrag(tdrag.sid, wm.frameNode(tdrag.id) || 0); } catch (e) { g = null; }
  if (!g) { tdrag = null; return; }
  if (w.maximized) wm.toggleMaximize(tdrag.id);
  const f = wm.frame(wm.get(tdrag.id));
  const at = dragFrameAt(f, g, g.ox, g.oy, px, py, frameInset(w));
  if (at.x !== f.x || at.y !== f.y) wm.move(tdrag.id, at.x, at.y);
}
addListener('pointermove', (e) => { if (tdrag) followDrag(e.x, e.y); });

// a Linux app's drag of files released over LordOS: who claimed it at the release (wayland.takeDrop) gets the
// paths once lord-server has read them ('filesdrop' on window 0)
let dropTaker = null;
// when each Linux window showed and where: a dialog told its parent late goes over it (case 'parent')
const mappedAt = new Map();
// The icon a Linux window set for itself (xdg-toplevel-icon), re-encoded by lord-server: kept per window, shown only when
// its app has no installed icon (sdk/apps.js windowIcon: the installed app's icon always wins)
function windowOwnIcon(sid, id) {
  let px = null;
  try { px = typeof lord.wayland.icon === 'function' ? lord.wayland.icon(sid) : null; } catch (e) { px = null; }
  windowPixelIcons.set((m) => {
    if (!px && !m[id]) return m;
    const next = { ...m };
    if (px) next[id] = px; else delete next[id];
    return next;
  });
}
globalThis.__lordWayland = (sid, type, d = {}) => {
  if (sid === 0 && type === 'bell') { ringBell(null); return; }   // xdg-system-bell with no window
  if (sid === 0 && type === 'filesdrop') {
    const fn = dropTaker;
    dropTaker = null;
    const paths = typeof lord.wayland.droppedFiles === 'function' ? lord.wayland.droppedFiles() : [];
    if (fn && paths.length) { try { fn(paths); } catch (e) { console.warn('files drop:', e); } }
    return;
  }
  const id = idOf(sid);
  const win = wm.get(id);
  switch (type) {
    case 'textinput':
      // a text field of the app took or lost focus (zwp_text_input_v3)
      osk.linuxField(id, !!d.on, d.h > 0 ? d.y : -1, d.h);
      linuxIme(id, !!d.on);
      break;
    case 'map': {
      // X11 (Xwayland): the WM_CLASS class, or its instance when only that names a .desktop
      if (d.x11 && d.appAlt && !linuxAppFor(d.appId) && linuxAppFor(d.appAlt)) d = { ...d, appId: d.appAlt };
      // a Flatpak app's window: lord-server tells the app from the client's root-owned scope; its own app_id may be
      // anything ("tuxocide" for an SDL game), so the Flatpak id names and draws it (docs/FLATPAK.md)
      if (d.flatpak && !linuxAppFor(d.appId)) d = { ...d, appId: d.flatpak };
      const area = openingArea(launcherKeyFor(d.appId));   // its assigned display, or under the pointer (docs/DISPLAYS.md)
      // a browser's own notification popup (no app id, title or parent): floating in the top corner, never
      // the front window, so it doesn't take the keyboard (sdk/desktopmodel.js isNoticeWindow)
      if (isNoticeWindow(d)) {
        const others = windows.get().filter((x) => x.args?.notice && x.pip).map((x) => x.pip);
        const pip = noticeFrame(area, others, d.width, d.height, direction() === 'rtl');
        wm.open('linux', { instance: String(sid), frame: { ...pip }, pip, args: { surface: sid, csd: true, appId: '', notice: true } });
        wm.setCloseGuard(id, () => { lord.wayland.close(sid); return false; });
        break;
      }
      const args = { surface: sid, csd: !!d.csd, appId: d.appId || '', x11: !!d.x11, pkg: !!(d.appId && (getApp(d.appId)?.package || getApp(d.appId)?.isolated)) };
      // a web app's window (lordos-web.<app>.<profile>): lord-server says whether its process is that lord-web
      const web = d.x11 ? null : webOwnerOf(sid, d.appId);
      if (web) args.web = web;
      // the size it asked for, with the theme's frame (and menu strip) around it
      const c = frameInset({ id, args });
      const w = Math.min(area.w, d.width + c.w), hh = Math.min(area.h, d.height + c.h);
      const n = windows.get().length;
      // cascaded, but always inside the work area (never under the Dock)
      const x = Math.round(area.x + (area.w - w) / 2 + (n % 5) * 24), y = Math.round(area.y + Math.max(8, (area.h - hh) / 2 - 30) + (n % 5) * 24);
      // a dialog over the window it belongs to (sdk/dialogmodel.js), on that window's desktop
      const owner = d.parent ? wm.get(idOf(d.parent)) : null;
      const saved = owner ? null : placeLinuxWindow(dockIdOfWindow({ id, args }));   // session restore: the place it had
      // xdg-toplevel-tag: the kind of window it is keys its remembered place; its description names it while untitled
      const tg = linuxTag(sid);
      if (tagKey(tg?.tag)) args.tag = tagKey(tg.tag);
      if (descriptionTitle(tg?.description)) args.tagDescription = descriptionTitle(tg.description);
      // xdg-session-management: the app restored this window by name: its own place comes first (given once)
      const restored = linuxRestored(sid, c);
      // a dialog goes where its app asks and isn't remembered; a fixed window keeps its size
      if (d.dialog) args.dialog = true;
      if (d.fixed) args.fixedLinux = true;
      if (d.skipTaskbar) args.skipTaskbar = true;      // X11 _NET_WM_STATE_SKIP_TASKBAR: no Dock entry of its own
      if (d.skipPager) args.skipPager = true;          // X11 _NET_WM_STATE_SKIP_PAGER: not in the Overview
      if (d.parent) { args.parent = idOf(d.parent); args.modal = !!d.modal; }
      const kept = restored || saved || d.dialog ? null : rememberedFrame(placeKeyOf(dockIdOfWindow({ id, args }), args.tag), { fixed: !!d.fixed, size: [w, hh] });
      wm.open('linux', {
        instance: String(sid),
        frame: owner ? overParent(wm.frame(owner), w, hh, workAreaOf(displayFor(owner)))
          : restored?.frame || saved?.frame || (kept ? { x: kept.x, y: kept.y, w: kept.w, h: kept.h } : { w, h: hh, x: Math.max(area.x, Math.min(x, area.x + area.w - w)), y: Math.max(area.y, Math.min(y, area.y + area.h - hh)) }),
        args,
        desktop: owner ? owner.desktop : saved?.desktop ?? desktopOfSurface(sid),    // restored, or still open across a theme switch
      });
      // when it showed and the size it asked for (not a remembered frame of its app's): a dialog told its parent late
      mappedAt.set(id, { t: Date.now(), frame: { w, h: hh } });
      if (mappedAt.size > 256) mappedAt.delete(mappedAt.keys().next().value);
      if (restored) { if (restored.max) wm.toggleMaximize(id); } else saved?.then(id);
      // always on top or kept below: what an X11 app asked for (_NET_WM_STATE_ABOVE / _BELOW), or the layer the
      // window had before a theme switch
      if (d.layer) wm.setLayer(id, layerName(d.layer));
      if (kept?.maximized) wm.toggleMaximize(id);
      // app .lpk aislada: su nombre (del manifest) en vez del id
      { const pkg = d.appId ? getApp(d.appId) : null; wm.setTitle(id, pkg && (pkg.package || pkg.isolated) && (!d.title || d.title === d.appId) ? (pkg.isolated ? appName(pkg) : pkg.name) : (d.title || args.tagDescription || d.appId || null)); }
      // a built-in isolated app that asks for it (manifest "protected": Passwords) is the protected fill in captures
      if (d.appId && getApp(d.appId)?.isolated && getApp(d.appId)?.protected) wm.setProtected(id, true);
      wm.setCloseGuard(id, () => { lord.wayland.close(sid); return false; });   // la app decide cuándo cierra
      windowOwnIcon(sid, id);
      if (d.appId && (getApp(d.appId)?.package || getApp(d.appId)?.isolated)) setTimeout(() => applyAppMenus(d.appId), 0);   // menús de la app aislada (ya mapeada)
      break;
    }
    case 'unmap': mappedAt.delete(id); if (win) wm.close(id, true); if (windowPixelIcons.get()[id]) windowOwnIcon(-1, id); break;
    case 'icon': windowOwnIcon(sid, id); break;       // xdg_toplevel_icon_manager_v1.set_icon, committed (X11: _NET_WM_ICON)
    case 'taskbar': if (win) wm.setArgs(id, { skipTaskbar: !!d.skip }); break;   // X11 _NET_WM_STATE_SKIP_TASKBAR changed
    case 'pager': if (win) wm.setArgs(id, { skipPager: !!d.skip }); break;       // X11 _NET_WM_STATE_SKIP_PAGER changed
    // X11 _NET_WM_STATE_ABOVE / _BELOW changed: its layer (it never takes the keyboard by it, desktopmodel.js stackOrder)
    case 'layer': if (win) wm.setLayer(id, layerName(d.layer)); break;
    // the window it belongs to changed, or it became modal or not (xdg-foreign, xdg-dialog-v1)
    case 'parent': {
      if (!win) break;
      wm.setArgs(id, { parent: d.parent ? idOf(d.parent) : null, modal: !!d.parent && !!d.modal, dialog: win.args?.dialog || !!d.parent });
      // told only after it showed (X11's WM_TRANSIENT_FOR read late, a GTK dialog's xdg-foreign parent): over its parent
      // now, on the parent's display and desktop, unless the person already moved it (dialogmodel.js lateParentFrame)
      const owner = d.parent ? wm.get(idOf(d.parent)) : null;
      const to = owner ? lateParentFrame(wm.get(id), wm.frame(owner), mappedAt.get(id), Date.now(), workAreaOf(displayFor(owner))) : null;
      if (to) { if (owner.desktop !== undefined && wm.get(id)?.desktop !== owner.desktop) wm.moveToDesktop(id, owner.desktop); wm.setFrame(id, to); }
      break;
    }
    case 'title': {                      // an isolated app keeps its name when the title is just its id
      if (!win) break;
      // a window taken for a notification popup that names itself is a normal window after all
      if (win.args?.notice && d.title) { wm.setArgs(id, { notice: false }); wm.setPip(id, null); }
      const pkg = win.args?.pkg ? getApp(win.args.appId) : null;
      wm.setTitle(id, pkg && (!d.title || d.title === win.args.appId) ? (pkg.isolated ? appName(pkg) : pkg.name) : (d.title || null));
      break;
    }
    case 'geometry': {                     // la app cambió su tamaño por su cuenta
      if (!win || win.maximized) break;
      // a notice popup grew: never past a notification's size (sdk/desktopmodel.js NOTICE_MAX_*), so it can't come to cover
      // the screen; what doesn't fit is cut off
      if (win.args?.notice && win.pip) { wm.setPip(id, { ...win.pip, w: Math.max(1, Math.min(Math.round(d.width) || 1, NOTICE_MAX_W)), h: Math.max(1, Math.min(Math.round(d.height) || 1, NOTICE_MAX_H)) }); break; }
      const c = frameInset(win);
      if (Math.abs(win.w - (d.width + c.w)) > 1 || Math.abs(win.h - (d.height + c.h)) > 1) wm.resize(id, d.width + c.w, d.height + c.h);
      break;
    }
    // desde donde se apretó el botón (la app pide mover recién después de un umbral)
    // lo sigue lord-server en C (wm.beginMove/beginResize, docs/WINDOWS.md); si no se puede (el tema no
    // marcó el nodo de la ventana), con los eventos del puntero como antes
    // A hidden desktop's window never gets to bring itself forward: these requests come from the app.
    case 'move': if (acceptsAppRequest(win, activeDesktop.get())) { wm.focus(id); if (!wm.beginMove(id)) drag = { id, edges: null, start: { x: d.x, y: d.y, f: wm.frame(wm.get(id)) } }; } break;
    case 'resize': if (acceptsAppRequest(win, activeDesktop.get())) { wm.focus(id); if (!wm.beginResize(id, d.edges || 10)) drag = { id, edges: d.edges || 10, start: { x: d.x, y: d.y, f: wm.frame(wm.get(id)) } }; } break;
    case 'maximize': if (win && !win.maximized) wm.toggleMaximize(id); break;
    case 'unmaximize': if (win && win.maximized) wm.toggleMaximize(id); break;
    case 'minimize':        // a move still under way ends with it: a minimized window never follows the pointer (#189)
      if (drag && drag.id === id) drag = null;
      wm.cancelGrab(id);
      if (win) wm.minimize(id);
      break;
    // The app switched who draws the frame while open (Chromium's «Use system title bar and borders»,
    // Firefox's «Title Bar»): same window, place, size, maximized state, focus and Dock entry; only the
    // theme's frame comes or goes (the content area grows or shrinks by it, and the app is configured).
    case 'deco': if (win && !!win.args?.csd !== !!d.csd) { if (win.windowMenu) wm.closeWindowMenu(id); wm.setArgs(id, { csd: !!d.csd }); } break;
    // right click on the app's own bar (xdg_toplevel.show_window_menu): the theme's window menu
    case 'menu': if (acceptsAppRequest(win, activeDesktop.get())) { wm.focus(id); wm.showWindowMenu(id, d.x, d.y); } break;
    // xdg_activation_v1.activate: 'activate' with a token LordOS issued or the app in front handed over
    // (a link opened in the browser already running), 'attention' with any other (desktopmodel.js)
    // a VM, remote desktop or game asks for ❖, ⌥Tab and the rest while it's in front (keyboard-shortcuts-inhibit,
    // server/src/wl/kbinhibit.h): asked once per app, lord-server keeps the answer as its "shortcuts" permission
    // The app is who lord-server checked it is (a Flatpak app's id or the program it runs), never the app_id or
    // title it set; d.name is the installed app's whose .desktop entry runs that program, or ''.
    case 'shortcuts': {
      const who = shortcutsWho(d.app);
      if (!who || who.kind === 'lord') break;
      promptForServer({ app: d.app, perm: 'shortcuts', name: String(d.name || '') || who.short, done: (allow) => lord.wayland.shortcutsAnswer(d.app, allow) });
      break;
    }
    // an app asks to be told when the person is away from the computer (ext-idle-notify, server/src/wl/idlenotify.h):
    // asked once per app, lord-server keeps the answer as its "idle" permission. d.app is an isolated LordOS app's id
    // (d.lpk) or who lord-server checked a Linux app is; d.name an installed app's name for it, or ''.
    case 'idlenotify': {
      const who = shortcutsWho(d.app);
      if (!who || (who.kind === 'lord') !== !!d.lpk) break;
      promptForServer({ app: d.app, perm: 'idle', name: String(d.name || '') || (who.kind === 'lord' ? '' : who.short), done: (allow) => lord.wayland.idleAnswer(d.app, allow) });
      break;
    }
    // xdg-toplevel-drag: this window follows the drag from now (d.on), or stops where it was let go
    case 'tldrag':
      if (d.on && win) { tdrag = { id, sid }; wm.focus(id); followDrag(d.x, d.y); } else if (tdrag?.id === id) tdrag = null;
      break;
    // xdg-toplevel-tag changed while open: a new kind of window remembers its own place from now on
    case 'tag':
      if (win) wm.setArgs(id, { tag: tagKey(d.tag) || undefined, tagDescription: descriptionTitle(d.description) || undefined });
      break;
    case 'bell': ringBell(win); break;      // xdg-system-bell (rate-limited in lord-server, sysbell.c)
    case 'blur': if (win) wm.setArgs(id, { blur: blurBoxes(d.rects) }); break;   // ext-background-effect (bgeffect.c)
    // an app asks to lease a VR headset's connector (drm-lease-v1, server/src/wl/drmlease.c): lord-server offers only
    // connectors the kernel marks non-desktop and asks every time until the person says "always" or "no" (its "vr"
    // permission). d.app is who lord-server checked the app is (never an isolated or sandboxed app), d.headset the
    // headset's name and connector, d.name an installed app's name for it, or ''.
    case 'drmlease': {
      const who = shortcutsWho(d.app);
      if (!who || who.kind === 'lord') break;
      promptForServer({ app: d.app, perm: 'vr', name: String(d.name || '') || who.short, headset: String(d.headset || ''), once: true,
        done: (how) => lord.wayland.leaseAnswer(d.app, how) });
      break;
    }
    case 'activate': case 'attention': {
      const how = activationOutcome(win, wm.focused(), type === 'activate');
      if (how === 'focus') wm.focus(id); else if (how === 'attention' && !win.attention) wm.setAttention(id, true);
      break;
    }
    default: break;
  }
};

// The shell extras' reads (lord.wayland, server/src/ui/js_sys.c): a window's tag, and the place its app restored it to.
function linuxTag(sid) {
  try { return typeof lord.wayland.tag === 'function' ? lord.wayland.tag(sid) : null; } catch (e) { return null; }
}
function linuxRestored(sid, inset) {
  let r = null;
  try { r = typeof lord.wayland.sessionFrame === 'function' ? lord.wayland.sessionFrame(sid) : null; } catch (e) { r = null; }
  const f = restoredFrame(r, inset);
  if (!f) return null;
  const main = Math.max(0, (screen.displays || []).findIndex((x) => x.main));
  return { frame: fitFrame(f, screen.displays, (x) => workAreaOf(x), main), max: !!r.max };
}
// xdg-system-bell: LordOS's alert sound, unless Do Not Disturb is on; a window not in front wants attention.
function ringBell(win) {
  const what = bellAction({ window: !!win, focused: !!win && wm.focused() === win.id, dnd: dndActive() });
  if (what.sound) sound.play('error');
  if (what.attention && !win.attention) wm.setAttention(win.id, true);
}
// Settings → transparency with blur: blur behind Linux windows is offered to apps only with it (bgeffect.c)
let effectsSent = null;
function applyEffects() {
  const on = settings.get().effects !== false;
  if (on === effectsSent || typeof lord.wayland === 'undefined' || typeof lord.wayland.effects !== 'function') return;
  effectsSent = on;
  lord.wayland.effects(on);
}
applyEffects();
settings.subscribe(applyEffects);

// LordOS's input method composes into the Linux app's field (server/src/wl/wl_textinput.c): the app
// draws the composition itself (inline), the candidate window goes at its caret, read from the
// compositor each time (the window can move). A window that closes with the field takes it along.
let imeWindow = null;
const linuxCaret = () => { try { return lord.wayland.textCaret(); } catch (e) { return null; } };
function linuxIme(id, on) {
  if (on) { imeWindow = id; ime.attach('linux', linuxCaret, { inline: true }); return; }
  if (!imeWindow) return;
  imeWindow = null;
  ime.detach('linux');
}
windows.subscribe(() => { if (imeWindow && !wm.get(imeWindow)) linuxIme(null, false); });

// A terminal emulator, from its .desktop: there ❖C reaches it as Ctrl+Shift+C (Ctrl+C is the signal).
const isTerminalEmulator = (w) => String(linuxAppFor(w.args?.appId || '')?.categories || '').split(';').includes('TerminalEmulator');

// Dialogs stay with their window: moved, minimized and sent to another desktop with it, a modal one
// right above it (sdk/dialogmodel.js). Each change is compared with the list before it, until it settles;
// it runs before sync below, which then sees the settled list.
let seen = windows.get();
windows.subscribe(() => {
  const prev = seen, next = windows.get();
  seen = next;
  const kept = followParents(prev, next);
  if (kept !== next) windows.set(kept);
});

// The keyboard goes to the Linux app in front of the showing desktop, never to a window of a hidden
// one (a security rule: desktopmodel.js keyboardSurface, docs/DESKTOPS.md). Parking comes first: it
// takes the keyboard and fullscreen from a window that just left the desktop in view, so the focus
// below lands on a compositor that agrees.
let lastFocus = -1;
const lastState = new Map();
// Each display's work area (menu bar and Dock out): a new Linux window is told it as the most it can fill
// (xdg_toplevel.configure_bounds). Sent when it changes.
let sentAreas = '';
function sendWorkAreas() {
  if (typeof lord.wayland?.workAreas !== 'function') return;
  const list = (screen.displays || []).map((d) => { const a = workAreaOf(d); return { x: Math.round(a.x), y: Math.round(a.y), w: Math.round(a.w), h: Math.round(a.h) }; });
  const key = JSON.stringify(list);
  if (key !== sentAreas) { sentAreas = key; lord.wayland.workAreas(list); }
}
function sync() {
  if (typeof lord.wayland === 'undefined') return;
  const list = windows.get(), active = activeDesktop.get();
  // wholly under an opaque window: lord-server paces it once a second (desktopmodel.js coveredIds)
  const covered = coveredIds(list, active, overviewShowing.get(), (w) => wm.frame(w),
    (w) => !!w.args?.web || (!w.args?.surface && !getApp(w.id)?.transparent));
  for (const w of list) {
    const s = surfaceOf(w.id);
    if (!s) continue;
    const st = surfaceState(w, active);
    const cov = covered.has(w.id);
    // locked (the theme's padlock, isWindowLocked): the app is told it can't maximize (minimize stays), and its own buttons go
    const fixed = isWindowLocked(w.id);
    // snapped to a half, quarter or third: the edges against the work area (the app may square its corners there)
    const edges = st.maximized ? 0 : tiledEdges(w.snapped);
    // always on top (1) or kept below (-1): an X11 app sees it in _NET_WM_STATE (xwm.c), and it survives a theme switch
    const layer = ownLayer(w);
    const key = (st.maximized ? 1 : 0) | (st.iconic ? 2 : 0) | (st.parked ? 4 : 0) | (cov ? 8 : 0) | (fixed ? 16 : 0) | (edges << 5) | ((layer + 1) << 9);
    if (lastState.get(s) !== key) { lastState.set(s, key); lord.wayland.state(s, st.maximized, false, st.iconic, st.parked, cov, fixed, edges, layer); }
  }
  sendWorkAreas();
  // search, the launcher, a sheet or a system prompt is open: the keys are LordOS's, not the app's
  const f = keyboardSurface(list, active, shellHasKeyboard(), surfaceOf);
  if (f !== lastFocus) { lastFocus = f; lord.wayland.focus(f, !!f && isTerminalEmulator(focusedOn(list, active))); }
  // the window in front no longer wants attention, however it got there (the one above it closed)
  const top = focusedOn(list, active);
  if (top?.attention) setTimeout(() => wm.setAttention(top.id, false), 0);
}
windows.subscribe(sync);
windowLocks.subscribe(sync);
// (after the theme set its work area, configureWindows; again when the displays change)
setTimeout(sendWorkAreas, 0);
onScreenChange(() => setTimeout(sendWorkAreas, 0));
activeDesktop.subscribe(sync);
overviewShowing.subscribe(sync);
if (typeof onShellKeyboard === 'function') onShellKeyboard(() => sync());

// X11 apps on HiDPI displays (Settings → Displays, docs/WAYLAND.md "X11 apps on HiDPI displays"): 'self', the
// default, gives them X pixels that are device pixels and Xft.dpi to draw larger (sharp); 'lordos' enlarges
// them (the right size, blurry). lord-server applies it the next time Xwayland starts.
export const x11SelfScale = (s = settings.get()) => s.x11Scaling !== 'lordos';
let x11Sent = null;
function applyX11Scaling() {
  const on = x11SelfScale();
  if (on === x11Sent || typeof lord.wayland === 'undefined' || typeof lord.wayland.x11SelfScale !== 'function') return;
  x11Sent = on;
  lord.wayland.x11SelfScale(on);
}
applyX11Scaling();
settings.subscribe(applyX11Scaling);
export const setX11Scaling = (mode) => settings.set((s) => ({ ...s, x11Scaling: mode === 'lordos' ? 'lordos' : 'self' }));

export const wayland = {
  info: () => (typeof lord.wayland !== 'undefined' ? lord.wayland.info() : { socket: '', windows: 0 }),
  isLinuxWindow: (id) => surfaceOf(id) > 0,
  // Files' drag over a Linux window (docs/WAYLAND.md, "Drag and drop"): lord-server offers the paths as
  // text/uri-list while the button is down; dragTaken says what the window under the pointer agreed to
  // ('copy', 'move' or ''), endDrag cancels one still running. Only the paths of the drag, checked in C.
  dragFiles: (paths) => typeof lord.wayland !== 'undefined' && typeof lord.wayland.dragFiles === 'function' && lord.wayland.dragFiles(paths),
  dragTaken: () => (typeof lord.wayland !== 'undefined' && typeof lord.wayland.dragTaken === 'function' ? lord.wayland.dragTaken() : ''),
  endDrag: () => { if (typeof lord.wayland !== 'undefined' && typeof lord.wayland.dragFiles === 'function') lord.wayland.dragFiles(null); },
  // the other way: called on the release, true if a Linux app's drag of files was just dropped here and this
  // caller is the first to take it; fn(paths) runs once they are read (a drop nobody takes is cancelled)
  takeDrop: (fn) => {
    if (typeof lord.wayland === 'undefined' || typeof lord.wayland.takeDrop !== 'function' || !lord.wayland.takeDrop()) return false;
    dropTaker = fn;
    return true;
  },
};

// Apps .lpk aisladas: su título es el nombre del paquete en el idioma actual; al cambiar de idioma
// se actualiza (refreshPackages recalcula los nombres en el mismo cambio).
locale.subscribe(() => setTimeout(() => {
  for (const w of windows.get()) {
    if (!w.args?.pkg) continue;
    const pkg = getApp(w.args.appId);
    if (pkg && pkg.name) wm.setTitle(w.id, pkg.isolated ? appName(pkg) : pkg.name);
  }
}, 0));

// Compositor: una app .lpk aislada publicó sus menús por el broker. Se ponen en la ventana de esa
// app (linux#<superficie> con app_id = su id) para que el tema los muestre en su barra de menú;
// los clics vuelven a la app por lord.broker.menuClick. Ver docs/PRIVACY.md.
function applyAppMenus(app) {
  if (typeof lord === 'undefined' || !lord.broker || typeof lord.broker.menus !== 'function') return;
  const wins = windows.get().filter((w) => surfaceOf(w.id) > 0 && w.args?.appId === app);   // every window of the app
  if (!wins.length) return;
  let raw;
  try { raw = lord.broker.menus(app); } catch (e) { return; }
  const groups = (Array.isArray(raw) ? raw : []).map((g) => ({
    label: g.label,
    items: (g.items || []).map((it) => {
      if (it === '-' || it == null) return '-';
      const row = { label: it.label, hint: it.hint, disabled: !!it.disabled, checked: !!it.checked };
      if (it.id) row.onClick = () => { try { lord.broker.menuClick(app, it.id); } catch (e) { /* */ } };
      if (it.submenu) row.submenu = it.submenu.map((s) => (s === '-' ? '-' : { label: s.label, hint: s.hint, disabled: !!s.disabled, checked: !!s.checked, onClick: s.id ? () => lord.broker.menuClick(app, s.id) : undefined }));
      return row;
    }),
  }));
  for (const win of wins) wm.setMenus(win.id, groups);
}
if (typeof globalThis !== 'undefined') globalThis.__lordApplyAppMenus = applyAppMenus;
