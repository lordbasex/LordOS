// notify.js — session notifications (like GNOME notifications).
//
//   const id = notify({ app: 'backup', title: 'Backup done', body: '1234 files',
//                       actions: [{ id: 'open', label: 'Open', run: () => wm.open('backup') }],
//                       sound: true, sticky: false, timeout: 6000 })
//   dismissNotification(id)
//   useNotifications() → the banners on screen; notifyHistory (store) → this session's notifications
//
// <NotificationBanner /> is mounted by startShell, so it shows with any theme: at the top, on the
// end side of the line (right; left in Arabic or Hebrew), below the theme's bar (workArea). A
// notification goes away after `timeout` ms, except `sticky` ones (alarms), which wait until an
// action or the × is pressed. Clicking the banner runs `defaultAction` ({ id, run }: the click on the notification
// itself, never shown as a button) when there is one, else the first action.
// The app passes texts already translated (t()). The history lives in memory only.
import { createElement as h, useEffect, useState } from 'react';
import { View, Text } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { useTheme } from './theme.js';
import { direction, formatTime, appName } from './i18n.js';
import { getApp, workArea } from './windows.js';
import { sound } from './sound.js';
import { toneOf } from './notifytones.js';
import { settings } from './settings.js';
import { perms } from './privacy.js';
import { screen } from '@lordos/runtime';

const MAX_SHOWN = 4, HISTORY = 50;
export const notifications = createStore([]);        // on screen, newest first
export const notifyHistory = createStore([]);
let seq = 1;
const timers = new Map();

// ---- Settings → Notifications (docs/SETTINGS.md): per-app style, Do Not Disturb, grouping ----
//
// settings.notifications = { dnd: false | until (ms), schedule: { on, from, to } (local minutes),
//   grouping: 'app' | 'off', apps: { [app]: { allow, style: 'banner' | 'alert' | 'none', sound, lock } },
//   (sound: true the default sound, false none, or one of notifytones.js NOTIFY_TONES)
//   seen: [app ids that have notified] }
// app is an app id ('backup'), a package id, 'lordos-web.<app>' for a web app (lord-server checked it is that
// app's lord-web; its sound also follows Settings → Web apps), or 'linux:<desktop id or name>' for Linux apps
// (org.freedesktop.Notifications, server/src/hw/notifyd.c). For .lpk packages "allow" is the
// sandbox's "notifications" permission (the same switch as in Privacy & Security).
export const NOTIFY_DEFAULTS = { dnd: false, schedule: { on: false, from: 22 * 60, to: 7 * 60 }, grouping: 'app', apps: {}, seen: [] };
export const notifyPrefs = (s = settings.get()) => ({ ...NOTIFY_DEFAULTS, ...(s.notifications || {}) });
const isPackage = (app) => { try { return !!getApp(app)?.package; } catch (e) { return false; } };
export function appNotifyPrefs(app, s = settings.get()) {
  const p = { allow: true, style: 'banner', sound: true, lock: true, ...(notifyPrefs(s).apps[app] || {}) };
  if (isPackage(app)) p.allow = perms.get(app, 'notifications') !== 'deny';
  return p;
}
export function setAppNotifyPrefs(app, patch) {
  if (isPackage(app) && 'allow' in patch) perms.set(app, 'notifications', patch.allow ? 'allow' : 'deny');
  settings.set((s) => {
    const p = notifyPrefs(s);
    return { ...s, notifications: { ...p, apps: { ...p.apps, [app]: { ...(p.apps[app] || {}), ...patch } } } };
  });
}
export function dndActive(now = new Date(), s = settings.get()) {
  const p = notifyPrefs(s);
  if (p.dnd === true || (typeof p.dnd === 'number' && now.getTime() < p.dnd)) return true;
  if (!p.schedule?.on) return false;
  const m = now.getHours() * 60 + now.getMinutes(), len = (((p.schedule.to - p.schedule.from) % 1440) + 1440) % 1440;
  return len > 0 && (((m - p.schedule.from) % 1440) + 1440) % 1440 < len;
}
function remember(app) {
  const p = notifyPrefs();
  if (!app || p.seen.includes(app)) return;
  settings.set((s) => ({ ...s, notifications: { ...notifyPrefs(s), seen: [app, ...notifyPrefs(s).seen].slice(0, 64) } }));
}

// Firewall restriction "no notifications" (Settings → Privacy → Apps' network, docs/FIREWALL.md):
// checked in lord-server's rule store before anything is shown, above the per-app setting. A
// built-in app is "lordos", an isolated one (.lpk, Passwords) "lpk:<id>". Settings' own system
// warnings (battery) are exempt; Linux apps ('linux:…') are checked where their D-Bus call arrives.
function fwNotifyBlocked(app) {
  try {
    if (typeof lord === 'undefined' || !lord.fw || typeof lord.fw.blocks !== 'function') return false;
    // Linux apps and web apps ('lordos-web.…', from lord-web) are checked where their D-Bus call arrives
    if (!app || app === 'settings' || app.startsWith('linux:') || app.startsWith('lordos-web.')) return false;
    const a = getApp(app);
    return !!lord.fw.blocks(a && (a.package || a.isolated) ? `lpk:${app}` : 'lordos', 'no-notifications');
  } catch (e) { return false; }
}

export function notify({ app = '', title = '', body = '', actions = [], defaultAction = null, sound: withSound = true, sticky = false, timeout = 6000, onClose, label = '', icon = null } = {}) {
  app = String(app);
  const ap = appNotifyPrefs(app);
  remember(app);
  if (!ap.allow || fwNotifyBlocked(app)) return 0;          // not delivered at all
  const id = seq++;
  const n = { id, app, label: String(label), Icon: typeof icon === 'function' ? icon : null, title: String(title), body: String(body), actions: Array.isArray(actions) ? actions.slice(0, 3) : [],
    defaultAction: defaultAction && typeof defaultAction.run === 'function' ? defaultAction : null,
    sticky: !!sticky || ap.style === 'alert', time: Date.now(), onClose };
  notifyHistory.set((l) => [{ id, app: n.app, label: n.label, title: n.title, body: n.body, time: n.time, actions: n.actions }, ...l].slice(0, HISTORY));
  // Do Not Disturb or "no banner": kept in the history only, without a sound
  if (dndActive() || ap.style === 'none') return id;
  notifications.set((l) => [n, ...l].slice(0, MAX_SHOWN));
  const tone = toneOf(ap.sound);
  if (withSound && tone) sound.play(tone);
  if (!n.sticky) timers.set(id, setTimeout(() => dismissNotification(id), Math.max(1500, timeout)));
  return id;
}

export function dismissNotification(id, reason = 'closed') {
  clearTimeout(timers.get(id));
  timers.delete(id);
  const n = notifications.get().find((x) => x.id === id);
  if (!n) return;
  notifications.set((l) => l.filter((x) => x.id !== id));
  try { n.onClose?.(reason); } catch (e) { console.error('notify onClose:', e); }
}

export const useNotifications = () => useStore(notifications);

// Where the banners go: 'top' (default: below a menu bar) or 'bottom' (above a taskbar, the Classic
// theme). Always on the end side of the line.
let placement = 'top';
export function setBannerPlacement(p) { placement = p === 'bottom' ? 'bottom' : 'top'; }
export const clearNotifyHistory = () => notifyHistory.set([]);

function runAction(n, a) {
  dismissNotification(n.id, a.id || 'action');
  try { a.run?.(); } catch (e) { console.error('notify:', e); }
}

function Banner({ n, th }) {
  const [hover, setHover] = useState(false);
  const app = n.app ? getApp(n.app) : null;
  const first = n.defaultAction || n.actions[0];
  return h(View, {
    onMouseEnter: () => setHover(true), onMouseLeave: () => setHover(false),
    onClick: () => (first ? runAction(n, first) : dismissNotification(n.id)),
    style: { width: 340, padding: 12, gap: 8, borderRadius: 14, backgroundColor: th.windowBg, border: `1px solid ${th.separator}`,
      boxShadow: '0 10px 30px rgba(0,0,0,0.4)' },
  },
  h(View, { style: { flexDirection: 'row', gap: 10, alignItems: 'flex-start' } },
    app?.Icon ? h(app.Icon, { size: 34 }) : n.Icon ? h(n.Icon, { size: 34 }) : h(View, { style: { width: 34, height: 34, borderRadius: 8, backgroundColor: th.accent } }),
    h(View, { style: { flex: 1, gap: 2 } },
      h(View, { style: { flexDirection: 'row', gap: 6 } },
        h(Text, { style: { flex: 1, fontSize: 11, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' } },
          `${app ? appName(app) : n.label}${n.more ? ` · +${n.more}` : ''}`),
        h(Text, { style: { fontSize: 11, color: th.textDim } }, formatTime(new Date(n.time)))),
      h(Text, { style: { fontSize: 13, fontWeight: 700, color: th.text, textAlign: 'start' } }, n.title),
      n.body ? h(Text, { style: { fontSize: 12, color: th.text, lineHeight: 1.35, textAlign: 'start' } }, n.body) : null),
    h(View, {
      onClick: (e) => { e.stopPropagation(); dismissNotification(n.id); },
      style: { width: 18, height: 18, borderRadius: 9, alignItems: 'center', justifyContent: 'center', backgroundColor: hover ? th.control : 'transparent' },
    }, h(Text, { style: { fontSize: 11, color: th.textDim } }, '×'))),
  n.actions.length ? h(View, { style: { flexDirection: 'row', gap: 8, justifyContent: 'flex-end' } },
    ...n.actions.map((a, i) => h(View, {
      key: a.id || i, onClick: (e) => { e.stopPropagation(); runAction(n, a); },
      style: { height: 26, paddingHorizontal: 12, borderRadius: 7, justifyContent: 'center', backgroundColor: a.primary ? th.accent : th.control },
    }, h(Text, { style: { fontSize: 12, fontWeight: 600, color: a.primary ? '#fff' : th.text } }, a.label)))) : null);
}

export function NotificationBanner() {
  const list = useNotifications();
  // drawn outside the theme's provider: follow the user's light/dark setting
  const base = useTheme(), mode = useStore(settings, (x) => x.mode);
  const th = mode === 'light' ? { ...base, mode: 'light', windowBg: '#f6f6f8', text: '#1d1d1f', textDim: 'rgba(60,60,67,0.62)',
    separator: 'rgba(0,0,0,0.12)', control: '#e2e2e7' } : base;
  const [, force] = useState(0);
  useEffect(() => { if (list.length) force((x) => x + 1); }, [list.length]);
  if (!list.length) return null;
  const area = workArea() || { x: 0, y: 0, w: screen.width, h: screen.height };
  const rtl = direction() === 'rtl';
  const left = rtl ? (area.x || 0) + 12 : (area.x || 0) + (area.w || screen.width) - 352;
  const at = placement === 'bottom'
    ? { bottom: Math.max(0, screen.height - ((area.y || 0) + (area.h || screen.height))) + 12 }
    : { top: (area.y || 0) + 10 };
  const shown = grouped(list);
  if (placement === 'bottom') shown.reverse();          // the newest one next to the taskbar
  return h(View, { style: { position: 'absolute', left, width: 340, gap: 10, ...at } },
    ...shown.map((n) => h(Banner, { key: n.id, n, th })));
}

// Grouping by app (Settings → Notifications): the newest banner of each app, with "+N".
function grouped(list) {
  if (notifyPrefs().grouping === 'off') return list;
  const out = [], seen = new Map();
  for (const n of list) {
    const k = n.app || n.label;
    if (seen.has(k)) { seen.get(k).more++; continue; }
    const g = { ...n, more: 0 };
    seen.set(k, g);
    out.push(g);
  }
  return out;
}
