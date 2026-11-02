// privacy.js — per-app permissions with system-drawn prompts, camera/microphone indicators and the frame
// of the Settings → Privacy page (see docs/PRIVACY.md).
//
// The permission state is kept by the server (lord.perms, server/src/sec/perms.c) in a
// file apps cannot write. Here we have:
//   - the queue of permission prompts, which THE SYSTEM DRAWS (PermissionPrompt), never the app;
//   - requestPermission(app, perm): returns a promise with true/false, asking only
//     once (afterwards the saved decision applies);
//   - the camera (green dot) and microphone (orange) indicators in the menu bar;
//   - registerPrivacySection(): a slot so other parts (e.g. the Monitor) can add their
//     own section to the Privacy page, without touching its code.
import { createElement as h, Fragment } from 'react';
import { View, Text, mainBox } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { t, locale, direction } from './i18n.js';
import { appName } from './i18n.js';
import { getApp } from './windows.js';
import { osk } from './osk.js';

const api = () => (typeof lord !== 'undefined' && lord.perms) || null;

export const PERMISSIONS = (() => { try { return api() ? lord.perms.names() : []; } catch (e) { return []; } })();

// ---- permission state ----
export const perms = {
  names: () => PERMISSIONS,
  get: (app, perm) => { try { return api() ? lord.perms.get(app, perm) : 'ask'; } catch (e) { return 'ask'; } },
  set(app, perm, state) { try { return api() ? lord.perms.set(app, perm, state) : false; } finally { permsRev.set((v) => v + 1); } },
  list: () => { try { return api() ? lord.perms.list() : []; } catch (e) { return []; } },
};
export const permsRev = createStore(0);   // goes up when something changes (to redraw Privacy)

// ---- queue of permission prompts (drawn by the system) ----
const queue = createStore([]);            // [{ id, app, perm, resolve }]
let seq = 1;

const inApp = () => typeof lord !== 'undefined' && lord.broker && typeof lord.broker.request === 'function';

// Requests a permission: if there is already a decision, it respects it; if not, it queues a system prompt.
// Inside an isolated app (lord-server --app), it goes through the broker: the prompt is drawn by
// lord-server, not the app.
export function requestPermission(app, perm) {
  if (inApp()) { try { return Promise.resolve(!!lord.broker.request(perm)); } catch (e) { return Promise.resolve(false); } }
  const st = perms.get(app, perm);
  if (st === 'allow') return Promise.resolve(true);
  if (st === 'deny') return Promise.resolve(false);
  return new Promise((resolve) => {
    queue.set((q) => [...q, { id: seq++, app, perm, resolve }]);
  });
}

// The server (compositor) receives prompts from isolated apps through the broker: it shows the
// system prompt and, with the answer, calls lord.broker.resolve. Indicators too.
if (typeof globalThis !== 'undefined') {
  globalThis.__lordBroker = (kind, data) => {
    try {
      if (kind === 'prompt') {
        // perm 'keyring': the app asks for an item another app saved (owner); see docs/PASSWORDS.md
        queue.set((q) => [...q, { id: seq++, app: data.app, perm: data.perm, owner: data.owner, broker: data.reqid }]);
      } else if (kind === 'textfocus') {
        osk.appField(data.app, !!data.on, data.y, data.h);      // only a hint; sdk/osk.js
      } else if (kind === 'menu') {
        if (typeof globalThis.__lordApplyAppMenus === 'function') globalThis.__lordApplyAppMenus(data.app);
      } else if (kind === 'pick') {
        if (typeof globalThis.__lordBrokerPick === 'function') globalThis.__lordBrokerPick(data.reqid, data.app, data.save, data.suggest);
      } else if (kind === 'mailopen' || kind === 'launch') {
        // an isolated system app asked the session to open an attachment or another app; lord-server
        // checked it (sec/brokermail.c), sdk/sysopen.js does it
        if (typeof globalThis.__lordSysOpen === 'function') globalThis.__lordSysOpen(kind, data);
      } else if (kind === 'device') {
        if (data.off) brokerDevicesByApp(data.app, null, false);
        else brokerDevicesByApp(data.app, data.kind, !!data.on);
      }
    } catch (e) { /* malformed prompt */ }
  };
}

function answer(item, allow) {
  queue.set((q) => q.filter((x) => x.id !== item.id));
  if (item.broker != null) {
    // prompt from an isolated app: lord-server saves the decision and answers the app
    try { lord.broker.resolve(item.broker, allow); } catch (e) { /* server without broker */ }
  } else {
    perms.set(item.app, item.perm, allow ? 'allow' : 'deny');
    if (item.resolve) item.resolve(allow);
  }
}

// System prompt: ««App» wants to use the microphone — Allow / Don't Allow».
export function PermissionPrompt() {
  const q = useStore(queue);
  if (!q.length) return null;
  const item = q[0];
  const name = appName(getApp(item.app)) || item.app;
  const owner = item.owner ? appName(getApp(item.owner)) || item.owner : '';
  const msg = t(`privacy.ask.${item.perm}`, { app: name, owner });
  return h(View, { style: { position: 'absolute', ...mainBox(), alignItems: 'center', justifyContent: 'flex-start',
    backgroundColor: 'rgba(0,0,0,0.35)', zIndex: 100000 } },
    h(View, { style: { marginTop: 80, width: 320, borderRadius: 14, padding: 20, gap: 14, backgroundColor: '#2b2b30',
      border: '1px solid rgba(255,255,255,0.12)', boxShadow: '0 20px 60px rgba(0,0,0,0.5)', alignItems: 'center' } },
      h(View, { style: { width: 44, height: 44, borderRadius: 10, backgroundColor: item.perm === 'camera' ? '#30d158' : item.perm === 'microphone' ? '#ff9f0a' : '#0a84ff',
        alignItems: 'center', justifyContent: 'center' } }, h(Text, { style: { fontSize: 22 } }, glyph(item.perm))),
      h(Text, { style: { fontSize: 14, fontWeight: 700, color: '#f5f5f7', textAlign: 'center', lineHeight: 1.4 } }, msg),
      h(Text, { style: { fontSize: 11, color: 'rgba(235,235,245,0.55)', textAlign: 'center' } }, t(item.perm === 'keyring' ? 'privacy.ask.keyringNote' : 'privacy.ask.where')),
      h(View, { style: { flexDirection: 'row', gap: 10, marginTop: 4 } },
        h(Btn, { label: t('privacy.deny'), onClick: () => answer(item, false) }),
        h(Btn, { primary: true, label: t('privacy.allow'), onClick: () => answer(item, true) }))));
}

function Btn({ label, primary, onClick }) {
  return h(View, { onClick, style: { paddingVertical: 7, paddingHorizontal: 16, borderRadius: 8,
    backgroundColor: primary ? '#0a84ff' : 'rgba(255,255,255,0.1)' } },
    h(Text, { style: { fontSize: 13, fontWeight: 600, color: '#fff' } }, label));
}

const glyph = (p) => (p === 'camera' ? '📷' : p === 'microphone' ? '🎙' : p === 'screen' ? '▦' : p === 'network' ? '⇅' : p === 'keyring' ? '🔑' : '●');

// ---- camera/microphone indicators ----
// Own source (isolated .lpk apps: the broker knows and reflects it here with markDeviceUse) +
// the Monitor's source (Linux apps, via PipeWire: globalThis.__lordos.monitorDevices, phase 2).
const brokerDevices = createStore([]);    // [{ app, kind: 'camera'|'microphone' }]

export function markDeviceUse(app, kind, on) {
  // Inside an isolated app, it is reported through the broker (lord-server shows the indicator).
  if (typeof lord !== 'undefined' && lord.broker && typeof lord.broker.device === 'function') {
    try { lord.broker.device(kind, !!on); } catch (e) { /* */ }
    return;
  }
  brokerDevices.set((list) => {
    const rest = list.filter((d) => !(d.app === app && d.kind === kind));
    return on ? [...rest, { app, kind }] : rest;
  });
}

// From the server (compositor): the broker reported that an app is using (or stopped using) a
// device. 'off' with kind null turns off all of that app's devices (the app closed).
function brokerDevicesByApp(app, kind, on) {
  brokerDevices.set((list) => {
    const rest = list.filter((d) => d.app !== app || (kind && d.kind !== kind));
    return on && kind ? [...rest, { app, kind }] : rest;
  });
}

function monitorDeviceStore() {
  try { return globalThis.__lordos && globalThis.__lordos.monitorDevices; } catch (e) { return null; }
}

// [{ kind, apps: [name] }] for each device in use.
export function useIndicators() {
  const own = useStore(brokerDevices);
  const md = monitorDeviceStore();
  const ext = md ? useStore(md) : [];
  const all = [...own, ...(Array.isArray(ext) ? ext : [])];
  const byKind = {};
  for (const d of all) {
    const name = d.name || appName(getApp(d.app)) || d.app;
    (byKind[d.kind] = byKind[d.kind] || []).push(name);
  }
  return Object.entries(byKind).map(([kind, apps]) => ({ kind, apps }));
}

// Component for the menu bar (both themes include it). A green dot for the camera and
// an orange one for the microphone; tapping it shows which app is using it.
export function PrivacyIndicators({ size = 8 }) {
  const list = useIndicators();
  const [open, setOpen] = useStoreLocal();
  if (!list.length) return null;
  return h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 6 }, onClick: () => setOpen(!open) },
    list.map((d) => h(View, { key: d.kind, style: { width: size, height: size, borderRadius: size / 2,
      backgroundColor: d.kind === 'camera' ? '#30d158' : '#ff9f0a' } })),
    open ? h(View, { style: { position: 'absolute', top: 22, right: 0, minWidth: 180, borderRadius: 10, padding: 10, gap: 6,
      backgroundColor: '#2b2b30', border: '1px solid rgba(255,255,255,0.12)', boxShadow: '0 12px 40px rgba(0,0,0,0.45)', zIndex: 99999 } },
      list.map((d) => h(View, { key: d.kind, style: { gap: 2 } },
        h(Text, { style: { fontSize: 11, fontWeight: 700, color: d.kind === 'camera' ? '#30d158' : '#ff9f0a' } },
          t(`privacy.inUse.${d.kind}`)),
        d.apps.map((a, i) => h(Text, { key: i, style: { fontSize: 12, color: '#f5f5f7' } }, a))))) : null);
}

// local mini-state without React hooks (the theme's component may not have useState available
// in its import); we use a module store.
const openStore = createStore(false);
function useStoreLocal() { return [useStore(openStore), openStore.set]; }

// ---- Privacy page slots (e.g. the Monitor adds its own) ----
export const privacySections = createStore([]);   // [{ id, order, Component }]
export function registerPrivacySection(section) {
  if (!section || !section.id || typeof section.Component !== 'function') return;
  privacySections.set((list) => [...list.filter((s) => s.id !== section.id), section].sort((a, b) => (a.order || 50) - (b.order || 50)));
}

// Language, region and direction (RTL) also go live to isolated apps.
function publishLocale() {
  try {
    if (typeof lord === 'undefined' || !lord.broker || typeof lord.broker.publish !== 'function') return;
    const l = locale.get();
    lord.broker.publish('locale', JSON.stringify({ lang: l.lang, region: l.region || '', dir: direction() }));
  } catch (e) { /* no broker */ }
}
publishLocale();
locale.subscribe(publishLocale);

// Isolated apps can find out that the user revoked a permission from them (to release the
// camera or the microphone). In-process nothing happens (revocation is immediate in the broker).
export function onPermissionRevoked(fn) {
  try { return typeof globalThis.__lordOnRevoke === 'function' ? globalThis.__lordOnRevoke(fn) : () => {}; } catch (e) { return () => {}; }
}

// Microphone for an isolated app: lord-server owns the PipeWire stream and passes the app
// a READ-ONLY ring (see docs/PRIVACY.md). micLevel() gives the level (RMS 0..1).
export function micStart() { try { return !!(lord.broker && lord.broker.micStart && lord.broker.micStart()); } catch (e) { return false; } }
export function micStop() { try { lord.broker && lord.broker.micStop && lord.broker.micStop(); } catch (e) { /* */ } }
export function micLevel() { try { return (lord.broker && lord.broker.micLevel) ? lord.broker.micLevel() : 0; } catch (e) { return 0; } }
