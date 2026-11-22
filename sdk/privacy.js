// privacy.js — per-app permissions with system-drawn prompts, camera/microphone indicators and the frame
// of the Settings → Privacy page (see docs/PRIVACY.md).
//
// The permission state is kept by the server (lord.perms, server/src/sec/perms.c) in a
// file apps cannot write. Here we have:
//   - the queue of permission prompts, which THE SYSTEM DRAWS (PermissionPrompt), never the app;
//   - requestPermission(app, perm): returns a promise with true/false, asking only
//     once (afterwards the saved decision applies);
//   - what camera and microphone are in use, for the capture indicator by the clock (sdk/capturestatus.jsx);
//   - registerPrivacySection(): a slot so other parts (e.g. the Monitor) can add their
//     own section to the Privacy page, without touching its code.
import { appStatusEvent } from './appstatus.jsx';
import { createElement as h, Fragment } from 'react';
import { View, Text, mainBox } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { t, locale, direction, regionData } from './i18n.js';
import { appName } from './i18n.js';
import { getApp } from './windows.js';
import { osk } from './osk.js';
import { shortcutsWho } from './kbinhibitmodel.js';

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
    // the same question already up (a window that asks each time it comes to the front): one answer for both
    const up = queue.get().find((x) => x.app === app && x.perm === perm && x.resolve);
    if (up) { const was = up.resolve; up.resolve = (v) => { was(v); resolve(v); }; return; }
    queue.set((q) => [...q, { id: seq++, app, perm, resolve }]);
  });
}

// What lord-server checked a Linux app is, for the "shortcuts" prompt and Settings → Privacy: "Flatpak app
// org.example.Viewer" or "Program /usr/bin/viewer"; '' for anything else (sdk/kbinhibitmodel.js shortcutsWho).
export function shortcutsWhoText(key) {
  const who = shortcutsWho(key);
  if (who?.kind === 'flatpak') return t('privacy.who.flatpak', { id: who.id });
  if (who?.kind === 'program') return t('privacy.who.program', { path: who.path });
  return '';
}

// A prompt whose decision lord-server keeps itself: a Linux app that asks for the desktop's shortcuts
// (sdk/wayland.js, server/src/wl/kbinhibit.h). name: what the person reads; done(allow) reports the answer.
// once: three answers (Don't allow, Allow once, Always allow), and done gets 'deny', 'once' or 'always' (the VR
// headset's lease, server/src/wl/drmlease.c); headset: the device it names.
export function promptForServer({ app, perm, name, done, once = false, headset = '' }) {
  if (queue.get().some((x) => x.app === app && x.perm === perm)) return;
  queue.set((q) => [...q, { id: seq++, app, perm, name, done, once, headset }]);
}

// The server (compositor) receives prompts from isolated apps through the broker: it shows the
// system prompt and, with the answer, calls lord.broker.resolve. Indicators too.
if (typeof globalThis !== 'undefined') {
  globalThis.__lordBroker = (kind, data) => {
    try {
      if (kind === 'prompt') {
        // perm 'keyring': the app asks for an item another app saved (owner); see docs/PASSWORDS.md
        queue.set((q) => [...q, { id: seq++, app: data.app, perm: data.perm, owner: data.owner, broker: data.reqid }]);
      } else if (kind === 'status') {
        appStatusEvent(data);                                   // an app's item by the clock; sdk/appstatus.jsx
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

function answer(item, allow, how) {
  queue.set((q) => q.filter((x) => x.id !== item.id));
  if (item.once && item.done) {
    // three answers, lord-server saves "always" and "no" itself (a VR headset's lease)
    try { item.done(how || (allow ? 'once' : 'deny')); } finally { permsRev.set((v) => v + 1); }
  } else if (item.broker != null) {
    // prompt from an isolated app: lord-server saves the decision and answers the app
    try { lord.broker.resolve(item.broker, allow); } catch (e) { /* server without broker */ }
  } else if (item.done) {
    // lord-server saves it (a Linux app's): Privacy redraws with the new decision
    try { item.done(allow); } finally { permsRev.set((v) => v + 1); }
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
  const name = item.name || appName(getApp(item.app)) || item.app;
  const owner = item.owner ? appName(getApp(item.owner)) || item.owner : '';
  const msg = t(`privacy.ask.${item.perm}`, { app: name, owner, headset: item.headset || '' });
  const note = item.perm === 'keyring' ? 'privacy.ask.keyringNote' : item.perm === 'shortcuts' ? 'privacy.ask.shortcutsNote'
    : item.perm === 'idle' ? 'privacy.ask.idleNote' : item.perm === 'vr' ? 'privacy.ask.vrNote' : 'privacy.ask.where';
  // a Linux app's "shortcuts" or "idle": who lord-server checked it is, under its name (the name alone could be anyone's)
  const who = item.done && (item.perm === 'shortcuts' || item.perm === 'idle' || item.perm === 'vr') ? shortcutsWhoText(item.app) : '';
  return h(View, { style: { position: 'absolute', ...mainBox(), alignItems: 'center', justifyContent: 'flex-start',
    backgroundColor: 'rgba(0,0,0,0.35)', zIndex: 100000 } },
    h(View, { style: { marginTop: 80, width: 320, borderRadius: 14, padding: 20, gap: 14, backgroundColor: '#2b2b30',
      border: '1px solid rgba(255,255,255,0.12)', boxShadow: '0 20px 60px rgba(0,0,0,0.5)', alignItems: 'center' } },
      h(View, { style: { width: 44, height: 44, borderRadius: 10, backgroundColor: item.perm === 'camera' ? '#30d158' : item.perm === 'microphone' ? '#ff9f0a' : '#0a84ff',
        alignItems: 'center', justifyContent: 'center' } }, h(Text, { style: { fontSize: 22 } }, glyph(item.perm))),
      h(Text, { style: { alignSelf: 'stretch', fontSize: 14, fontWeight: 700, color: '#f5f5f7', textAlign: 'center', lineHeight: 1.4 } }, msg),
      who ? h(Text, { style: { alignSelf: 'stretch', fontSize: 11, color: 'rgba(235,235,245,0.75)', textAlign: 'center' } }, who) : null,
      h(Text, { style: { alignSelf: 'stretch', fontSize: 11, color: 'rgba(235,235,245,0.55)', textAlign: 'center' } }, t(note)),
      item.once
        ? h(View, { style: { flexDirection: 'row', flexWrap: 'wrap', justifyContent: 'center', gap: 10, marginTop: 4 } },
          h(Btn, { label: t('privacy.deny'), onClick: () => answer(item, false, 'deny') }),
          h(Btn, { label: t('privacy.allowOnce'), onClick: () => answer(item, true, 'once') }),
          h(Btn, { primary: true, label: t('privacy.allowAlways'), onClick: () => answer(item, true, 'always') }))
        : h(View, { style: { flexDirection: 'row', gap: 10, marginTop: 4 } },
          h(Btn, { label: t('privacy.deny'), onClick: () => answer(item, false) }),
          h(Btn, { primary: true, label: t('privacy.allow'), onClick: () => answer(item, true) }))));
}

function Btn({ label, primary, onClick }) {
  return h(View, { onClick, style: { paddingVertical: 7, paddingHorizontal: 16, borderRadius: 8,
    backgroundColor: primary ? '#0a84ff' : 'rgba(255,255,255,0.1)' } },
    h(Text, { style: { fontSize: 13, fontWeight: 600, color: '#fff' } }, label));
}

const glyph = (p) => (p === 'camera' ? '📷' : p === 'microphone' ? '🎙' : p === 'screen' ? '▦' : p === 'network' ? '⇅' : p === 'keyring' ? '🔑' : p === 'shortcuts' ? '❖' : p === 'idle' ? '☾' : p === 'vr' ? '◎' : '●');

// ---- camera/microphone in use ----
// LordOS's own: native apps (lord.media, sdk/media.js) and isolated .lpk apps (the broker reports it, brokerDevicesByApp);
// and Linux apps' through PipeWire, as lord-server's PipeWire guard sees it (hw event "capture-use", sdk/hardware.jsx).
// The capture indicator by the clock draws them (sdk/capturestatus.jsx); Settings → Privacy lists them (useIndicators).
export const brokerDevices = createStore([]);    // [{ app, kind: 'camera'|'microphone', media? }]
export const linuxCapture = createStore([]);     // [{ app, name, mic, camera, screen }]: app '' an unknown program
// the Linux apps reading LordOS Studio's camera device (hw event "studio-use", server/src/media/mstudio.c):
// { apps: [id], camera, effects } (sdk/studiomodel.js studioUseOf)
export const studioUse = createStore({ apps: [], camera: '', effects: false });

// media: a native app's camera source (lord.media), so the indicator can show what it sees
export function markDeviceUse(app, kind, on, media = null) {
  // Inside an isolated app, it is reported through the broker (lord-server shows the indicator).
  if (typeof lord !== 'undefined' && lord.broker && typeof lord.broker.device === 'function') {
    try { lord.broker.device(kind, !!on); } catch (e) { /* */ }
    return;
  }
  brokerDevices.set((list) => {
    const rest = list.filter((d) => !(d.app === app && d.kind === kind));
    return on ? [...rest, { app, kind, ...(media ? { media } : null) }] : rest;
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

// [{ kind, apps: [name] }] for each device in use.
export function useIndicators() {
  const own = useStore(brokerDevices);
  const ext = useStore(linuxCapture);
  const byKind = {};
  const add = (kind, name) => { const l = (byKind[kind] = byKind[kind] || []); if (!l.includes(name)) l.push(name); };
  for (const d of own) add(d.kind, appName(getApp(d.app)) || d.app);
  for (const d of ext) {
    const name = d.name || t('shell.cast.unknownApp');
    if (d.camera) add('camera', name);
    if (d.mic) add('microphone', name);
  }
  return Object.entries(byKind).map(([kind, apps]) => ({ kind, apps }));
}


// ---- Privacy page slots (e.g. the Monitor adds its own) ----
export const privacySections = createStore([]);   // [{ id, order, Component }]
export function registerPrivacySection(section) {
  if (!section || !section.id || typeof section.Component !== 'function') return;
  privacySections.set((list) => [...list.filter((s) => s.id !== section.id), section].sort((a, b) => (a.order || 50) - (b.order || 50)));
}

// Language, region (with its formats) and direction (RTL) also go live to isolated apps.
function publishLocale() {
  try {
    if (typeof lord === 'undefined' || !lord.broker || typeof lord.broker.publish !== 'function') return;
    const l = locale.get();
    // rf: glibc's formats for the region, which the app can't read itself (sdk/i18n.js regionData)
    lord.broker.publish('locale', JSON.stringify({ lang: l.lang, region: l.region || '', dir: direction(), rf: regionData(l.region || '') }));
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
