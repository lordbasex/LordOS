// hardware.jsx — the session side of Settings' hardware pages (docs/SETTINGS.md):
//
//   displays      store with lord.display.list() (outputs, modes, Night Light, backlight)
//   applyDisplay  change resolution/refresh/rotation/main display live, with the 15-second
//                 "Keep these display settings?" countdown (<DisplayConfirm />, drawn by the system)
//   night         Night Light scheduler: sunset to sunrise (from the time zone's coordinates, never
//                 from the network) or custom hours, with a 30-minute fade, plus "on until tomorrow"
//   battery       store with lord.power.state(); low-battery notice; battery in the menu bar
//   idle          screen off / lock / sleep after N minutes (deadlines live in lord-server)
//   bluetooth     store with lord.bt.state(); pairing requests (<BluetoothPrompt />); setBluetoothPower,
//                 remembered and applied again at login (btpowermodel.js)
//   polkit        a Linux app's request for administrator rights (<PolkitPrompt />)
//   portal open   a Linux app asks to open a link or a file (OpenURI): opened like everything else, and asked
//                 first when the app is not the one in front (<PortalOpenPrompt />, portalopenmodel.js)
//   portal keys   a Linux app asks for global shortcuts: the person gives each one a key or none
//                 (<GlobalShortcutsPrompt />, globalshortcuts.jsx)
//   web apps      a web app's page asks for the camera, the microphone, the screen, the location or notifications
//                 and its registry entry says "ask" (org.lordos.WebApps1.Ask, <WebAppAskPrompt />, webapps.js)
//   timedate      NTP state from timedated (read-only)
//   menuClock(d)  the menu bar clock with the format chosen in Settings → Date & Time
//
// Everything arrives as __lordHw(kind, data) from lord-server (ui/js_hw.c): no polling.
import { createElement as h, useEffect, useRef, useState } from 'react';
import { View, Text, mainBox, addListener } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { settings } from './settings.js';
import { useTheme } from './theme.js';
import { t, formatDate, formatTime, weekdayName } from './i18n.js';
import { notify, dismissNotification } from './notify.js';
import { linuxAppFor, shortAppName, windowAppName } from './apps.js';
import { admin } from './admin.js';
import { registerStatusItem } from './statusitems.js';
import { TextInput, Toggle } from './components.jsx';
import { dialogStack, dialogKey, keyOwner } from './dialogkeys.js';
import { webAsks, webAskEvent, answerWebAsk, webAppEntry } from './webapps.js';
import { profileOf } from './webappsmodel.js';
import { portalFile } from './filepicker.jsx';
import { lockBeforeSleep, lockAfterWake } from './idlemodel.js';
import { btPowerAction } from './btpowermodel.js';
import { openPlan, urlHost, splitPath } from './portalopenmodel.js';
import { openUrl, openFileWithApps, revealPath } from './sysopen.js';
import { wm } from './windows.js';
import { linuxPlayersChanged } from './media.js';
import { trayEvent } from './trayitems.jsx';
import { globalShortcutsEvent } from './globalshortcuts.jsx';
import { castEvent, castNames } from './screencast.jsx';

const has = (name) => typeof lord !== 'undefined' && lord[name] && typeof lord[name] === 'object';
const call = (fn, fallback) => { try { return fn(); } catch (e) { return fallback; } };

export const displays = createStore(has('display') ? call(() => lord.display.list(), null) : null);
export const battery = createStore(has('power') ? call(() => lord.power.state(), null) : null);
export const powerProfiles = createStore(has('power') ? call(() => lord.power.profiles(), null) : null);
export const bluetooth = createStore(has('bt') ? call(() => lord.bt.state(), null) : null);
export const timedate = createStore(null);
export const displayConfirm = createStore(null);   // { conn, old, deadline, left }
const btResults = new Set();
const ppdResults = new Set();

export const useDisplays = () => useStore(displays);
export const useBattery = () => useStore(battery);
export const useBluetooth = () => useStore(bluetooth);
export const usePowerProfiles = () => useStore(powerProfiles);
export const useTimedate = () => useStore(timedate);
export const onBluetoothResult = (fn) => { btResults.add(fn); return () => btResults.delete(fn); };
// The Bluetooth switch (Settings, Control Center, Quick Settings): the choice is kept for the next login,
// since BlueZ powers every adapter again when it starts (btpowermodel.js).
export function setBluetoothPower(on) {
  settings.set({ bluetoothOff: !on });
  return has('bt') && call(() => lord.bt.power(on), false);
}
export const onPowerProfileResult = (fn) => { ppdResults.add(fn); return () => ppdResults.delete(fn); };

// ---- defaults kept in settings.json ----

export const NIGHT_DEFAULTS = { schedule: 'off', from: 22 * 60, to: 7 * 60, temp: 3400, manual: null };
export const POWER_DEFAULTS = { offAC: 600, offBattery: 180, lockDelay: 0, sleepAC: 0, sleepBattery: 900, lowBattery: true, menuPercent: true };
export const CLOCK_DEFAULTS = { hour24: 'auto', date: 'short', seconds: false };
export const nightPrefs = (s = settings.get()) => ({ ...NIGHT_DEFAULTS, ...(s.night || {}) });
export const powerPrefs = (s = settings.get()) => ({ ...POWER_DEFAULTS, ...(s.power || {}) });
export const clockPrefs = (s = settings.get()) => ({ ...CLOCK_DEFAULTS, ...(s.clock || {}) });

// ---- the menu bar clock ----

export function menuClock(d) {
  const c = clockPrefs();
  const time = formatTime(d, { seconds: !!c.seconds });
  if (c.date === 'none') return time;
  if (c.date === 'weekday') return `${weekdayName(d.getDay())}   ${time}`;
  return `${formatDate(d, 'short')}   ${time}`;
}

// ---- Night Light ----

const NEUTRAL = 6500;
const minutesOf = (d) => d.getHours() * 60 + d.getMinutes() + d.getSeconds() / 60;

// Same as night_amount() in server/src/hw/night.c (tested there).
export function nightAmount(now, from, to, fade = 30) {
  const len = (((to - from) % 1440) + 1440) % 1440;
  if (!len) return 0;
  const d = (((now - from) % 1440) + 1440) % 1440;
  if (d >= len) return 0;
  if (fade <= 0) return 1;
  return Math.max(0, Math.min(1, d / fade, (len - d) / fade));
}

// Sunset and sunrise today, in local minutes, from the chosen time zone's coordinates.
export function sunTimes(day = new Date()) {
  if (!has('display') || !lord.time) return null;
  const zone = call(() => lord.time.zone(), '');
  const s = call(() => lord.display.sun(zone, day.getFullYear(), day.getMonth() + 1, day.getDate()), null);
  if (!s) return null;
  const local = (utcMin) => {
    const d = new Date(Date.UTC(day.getFullYear(), day.getMonth(), day.getDate()) + utcMin * 60000);
    return d.getHours() * 60 + d.getMinutes();
  };
  return { sunset: local(s.set), sunrise: local(s.rise) };
}

// The window (local minutes) when the schedule is on, or null.
export function nightWindow(p = nightPrefs()) {
  if (p.schedule === 'custom') return { from: p.from, to: p.to };
  if (p.schedule === 'sun') {
    const s = sunTimes();
    return s ? { from: s.sunset, to: s.sunrise } : { from: NIGHT_DEFAULTS.from, to: NIGHT_DEFAULTS.to };
  }
  return null;
}

function nightTarget(now = new Date()) {
  const p = nightPrefs();
  const w = nightWindow(p);
  let amount = w ? nightAmount(minutesOf(now), w.from, w.to) : 0;
  if (p.manual && now.getTime() < p.manual.until) amount = p.manual.on ? 1 : 0;
  return { kelvin: Math.round(NEUTRAL - (NEUTRAL - p.temp) * amount), amount, fading: amount > 0 && amount < 1 };
}

export const nightActive = () => nightTarget().amount > 0;

// "Turn on until tomorrow" / "off until the next night" (from Control Center): an override
// that lasts until the end of the current (or next) scheduled night, or 07:00 without a schedule.
export function setNightManual(on) {
  const now = new Date(), m = minutesOf(now), w = nightWindow();
  const scheduled = w ? nightAmount(m, w.from, w.to, 0) > 0 : false;
  const manual = on === scheduled ? null
    : { on: !!on, until: now.getTime() + ((((((w ? w.to : 7 * 60) - m) % 1440) + 1440) % 1440) || 1440) * 60000 };
  settings.set({ night: { ...nightPrefs(), manual } });
}

let nightTimer = 0;
export function nightTick() {
  clearTimeout(nightTimer);
  if (!has('display')) return;
  const { kelvin, fading } = nightTarget();
  if (call(() => lord.display.temperature(), NEUTRAL) !== kelvin) call(() => lord.display.setTemperature(kelvin));
  const p = nightPrefs();
  if (p.schedule === 'off' && !(p.manual && Date.now() < p.manual.until)) return;
  // next check: every 30 s while fading, else at most 5 minutes (survives clock and zone changes)
  nightTimer = setTimeout(nightTick, fading ? 30000 : 300000);
}

// Preview while dragging the warmth slider in Settings.
export function previewNight(kelvin) { if (has('display')) call(() => lord.display.setTemperature(kelvin)); }

// ---- displays ----

// Applies a mode live and starts the 15-second countdown; keepDisplay() saves it, revert brings back
// the previous one (also when the countdown runs out with nobody answering).
// scale is in percent (100–300, 25 % steps; docs/DISPLAYS.md).
export function applyDisplay(out, { w, h: hh, refresh = 0, rotation = 0, scale = 100, conn = out.id }) {
  if (!has('display')) return false;
  const d = displays.get();
  const cur = (d?.outputs || []).find((o) => o.id === conn) || out;
  const old = { conn: cur.id, w: cur.mode?.w || 0, h: cur.mode?.h || 0, refresh: cur.mode?.refresh || 0, rotation: cur.rotation || 0, scale: cur.scale || 100 };
  if (!lord.display.apply(conn, w, hh, refresh, rotation, scale)) return false;
  const prev = displayConfirm.get();
  displayConfirm.set({ conn, old: prev ? prev.old : old, deadline: Date.now() + 15000, left: 15 });
  scheduleConfirm();
  return true;
}

let confirmTimer = 0;
function scheduleConfirm() {
  clearTimeout(confirmTimer);
  const c = displayConfirm.get();
  if (!c) return;
  const left = Math.ceil((c.deadline - Date.now()) / 1000);
  if (left <= 0) { revertDisplay(); return; }
  if (left !== c.left) displayConfirm.set({ ...c, left });
  confirmTimer = setTimeout(scheduleConfirm, 250);
}

export function keepDisplay() {
  const c = displayConfirm.get();
  clearTimeout(confirmTimer);
  displayConfirm.set(null);
  if (c && has('display')) lord.display.save(c.conn);
}

export function revertDisplay() {
  const c = displayConfirm.get();
  clearTimeout(confirmTimer);
  displayConfirm.set(null);
  if (c && has('display')) lord.display.apply(c.old.conn, c.old.w, c.old.h, c.old.refresh, c.old.rotation, c.old.scale || 100);
}

// Arrangement, mirror, main display, capture: applied and kept right away (nothing goes dark).
export function arrangeDisplay(conn, x, y, apply = true) {
  if (!has('display') || !lord.display.arrange) return null;
  const r = lord.display.arrange(conn, Math.round(x), Math.round(y), apply);
  if (r && apply) lord.display.save(conn);
  return r;
}
export function setDisplayMirror(on) {
  if (!has('display') || !lord.display.setMirror) return false;
  const ok = lord.display.setMirror(on);
  if (ok) setTimeout(() => lord.display.save(0), 0);
  return ok;
}
export function setMainDisplay(conn) {
  if (!has('display') || !lord.display.setMain) return false;
  const ok = lord.display.setMain(conn);
  if (ok) setTimeout(() => lord.display.save(conn), 0);
  return ok;
}
export function setCaptureAllDisplays(on) { return has('display') && lord.display.setCaptureAll ? lord.display.setCaptureAll(on) : false; }
// Settings → Displays, "Rotate automatically": the built-in panel follows the accelerometer (lord-server
// saves it in displays.json and sends the new list).
export function setAutoRotateDisplays(on) { return has('display') && lord.display.setAutoRotate ? lord.display.setAutoRotate(on) : false; }

// System dialogs are drawn outside the theme's provider: light or dark from the user's setting.
export function useSystemTokens() {
  const th = useTheme();
  const mode = useStore(settings, (s) => s.mode);
  return mode === 'light' ? { ...th, mode: 'light', windowBg: '#f6f6f8', text: '#1d1d1f', textDim: 'rgba(60,60,67,0.62)',
    separator: 'rgba(0,0,0,0.12)', control: '#e2e2e7' } : th;
}

// The "Keep these display settings?" alert, drawn like a standard system alert: a 260 px panel with the icon on
// top, a 13/700 title, an 11 px body with the countdown and two equal buttons (the default one in the
// accent color).
export function DisplayConfirm() {
  const c = useStore(displayConfirm);
  const th = useSystemTokens();
  if (!c) return null;
  const dark = th.mode !== 'light';
  return h(View, { style: { position: 'absolute', ...mainBox(), alignItems: 'center', justifyContent: 'center',
    backgroundColor: 'rgba(0,0,0,0.12)', zIndex: 100001 } },
  h(View, { style: { width: 260, borderRadius: 12, paddingTop: 20, paddingHorizontal: 16, paddingBottom: 16, alignItems: 'center',
    backgroundColor: dark ? '#2b2b2f' : '#ececee', border: `1px solid ${dark ? 'rgba(255,255,255,0.12)' : 'rgba(0,0,0,0.1)'}`,
    boxShadow: '0 18px 50px rgba(0,0,0,0.4)' } },
  h(DisplayGlyph, { size: 56, color: th.accent }),
  h(Text, { style: { width: 228, marginTop: 10, fontSize: 13, fontWeight: 700, color: th.text, textAlign: 'center', lineHeight: 1.3 } }, t('prefs.display.keepTitle')),
  h(Text, { style: { width: 228, marginTop: 6, fontSize: 11, color: th.text, opacity: 0.8, textAlign: 'center', lineHeight: 1.4 } }, t('prefs.display.keepBody', { count: c.left })),
  h(View, { style: { flexDirection: 'row', gap: 8, marginTop: 16, alignSelf: 'stretch' } },
    h(DialogButton, { label: t('prefs.display.revert'), onClick: revertDisplay, th, dark }),
    h(DialogButton, { label: t('prefs.display.keep'), onClick: keepDisplay, primary: true, th, dark }))));
}

export function DisplayGlyph({ size = 40, color = '#0a84ff' }) {
  const w = size, hh = size * 0.68;
  return h(View, { style: { width: w, height: size, alignItems: 'center' } },
    h(View, { style: { width: w, height: hh, borderRadius: size * 0.08, border: `${Math.max(2, size / 18)}px solid ${color}`, backgroundColor: 'rgba(10,132,255,0.12)' } }),
    h(View, { style: { width: size * 0.12, height: size * 0.14, backgroundColor: color } }),
    h(View, { style: { width: size * 0.42, height: Math.max(2, size / 18), borderRadius: 2, backgroundColor: color } }));
}

export function DialogButton({ label, onClick, primary, th, dark = th.mode !== 'light' }) {
  const [hover, setHover] = useState(false);
  return h(View, { onClick, onMouseEnter: () => setHover(true), onMouseLeave: () => setHover(false),
    style: { flex: 1, height: 28, borderRadius: 7, alignItems: 'center', justifyContent: 'center',
      backgroundColor: primary ? th.accent : dark ? 'rgba(255,255,255,0.14)' : '#ffffff',
      boxShadow: primary ? 'none' : '0 0.5px 1.5px rgba(0,0,0,0.25)', opacity: hover ? 0.88 : 1 } },
  h(Text, { style: { fontSize: 13, fontWeight: 500, color: primary ? '#fff' : th.text } }, label));
}

// ---- idle: screen off, lock, sleep ----

let idleLocks = false;                  // "require password" is on (a lock deadline was set)
export function applyIdle() {
  if (!has('display')) return;
  const p = powerPrefs();
  const ac = battery.get()?.onAC !== false;
  const off = ac ? p.offAC : p.offBattery;
  const lock = p.lockDelay < 0 || !off ? 0 : off + p.lockDelay;
  idleLocks = lock > 0;
  call(() => lord.display.setIdle(off || 0, lock, (ac ? p.sleepAC : p.sleepBattery) || 0));
}

const managed = () => typeof lord.session !== 'undefined' && lord.session.managed();
const machinesAwake = () => { try { return typeof lord.vm !== 'undefined' ? lord.vm.awake() : 0; } catch (e) { return 0; } };
// the sleep deadline came while a machine ran: looked at again every 30 s, and the computer sleeps once none runs
// (unless someone came back first: the "wake" stage)
let sleepHeld = null;
function onIdle(data) {
  const stage = data?.stage;
  if (stage === 'wake' && sleepHeld) { clearInterval(sleepHeld); sleepHeld = null; }
  // the lock deadline, or logind's Lock (loginctl lock-session, lock-sessions: hw/logindlock.c, from:'logind')
  if (stage === 'lock' && managed()) lord.session.lock();
  // the computer is going to sleep (the lid, the power button, a sleep timer) or woke up: the display was
  // off, so "Require password after the display turns off" applies (issue #36, sdk/idlemodel.js).
  // lord-server holds the sleep until the screen is taken (hw/sleeplock.c).
  if (stage === 'suspend' || stage === 'resume') {
    if (typeof lord.greeter !== 'undefined' || !managed()) return;
    const { lockDelay } = powerPrefs();
    if (stage === 'suspend' ? lockBeforeSleep(lockDelay) : lockAfterWake(lockDelay, data?.slept)) lord.session.lock();
    return;
  }
  if (stage !== 'sleep') return;
  // a machine that runs keeps this computer awake (Machines, "Keep this computer awake", on unless turned off there)
  // (lord-vm also holds logind's idle inhibitor, which the login screen's sleep deadline obeys: lord-sessiond)
  const n = machinesAwake();
  if (n > 0) {
    console.log(`idle: sleep held off: ${n} running machine(s) keep this computer awake`);
    if (!sleepHeld) sleepHeld = setInterval(() => {
      if (machinesAwake() > 0) return;
      clearInterval(sleepHeld);
      sleepHeld = null;
      console.log('idle: no running machine holds sleep off any more: going to sleep');
      onIdle({ stage: 'sleep' });
    }, 30000);
    return;
  }
  // the login screen of a locked session has no admin helper: lord-sessiond suspends (docs/SESSIONS.md)
  if (typeof lord.greeter !== 'undefined') { lord.greeter.send(JSON.stringify({ t: 'power', a: 'suspend' })); return; }
  // sleep came before the lock deadline (a short sleep time): lock first, so waking up asks for the password
  if (idleLocks && managed()) lord.session.lock();
  admin.run('power-suspend', '', undefined, { quiet: true }).catch((e) => console.log(`suspender: ${e.message}`));
}

// ---- battery: low-battery notice and the menu bar item ----

let lowNotified = 100;
function checkLow(b) {
  if (!b || !b.hasBattery) return;
  if (b.charging || b.onAC) { lowNotified = 100; return; }
  if (!powerPrefs().lowBattery || b.percent < 0) return;
  const level = b.percent <= 5 ? 5 : b.percent <= 10 ? 10 : b.percent <= 20 ? 20 : 100;
  if (level >= lowNotified || level === 100) return;
  lowNotified = level;
  notify({ app: 'settings', title: t(level === 5 ? 'prefs.power.criticalTitle' : 'prefs.power.lowTitle'),
    body: t('prefs.power.lowBody', { pct: b.percent }), sticky: level === 5 });
}

export function BatteryGlyph({ percent, charging, color, width = 24 }) {
  const hh = Math.round(width / 2);
  const fill = Math.max(0, Math.min(100, percent < 0 ? 0 : percent));
  const low = !charging && percent >= 0 && percent <= 20;
  return h(View, { style: { flexDirection: 'row', alignItems: 'center', direction: 'ltr' } },
    h(View, { style: { width, height: hh, borderRadius: 3, border: `1.2px solid ${color}`, padding: 1.5, opacity: 0.95 } },
      h(View, { style: { width: `${fill}%`, height: '100%', borderRadius: 1.5, backgroundColor: low ? '#ff453a' : charging ? '#30d158' : color } })),
    h(View, { style: { width: 2, height: hh * 0.4, borderRadius: 1, backgroundColor: color, opacity: 0.7, marginLeft: 1 } }),
    charging ? h(Text, { style: { position: 'absolute', left: width / 2 - 4, top: -2, fontSize: 10, color: '#fff' } }, '⚡') : null);
}

function BatteryStatus({ color, height }) {
  const b = useBattery();
  const s = useStore(settings);
  if (!b || !b.hasBattery) return null;
  const p = powerPrefs(s);
  return h(View, { style: { height, paddingHorizontal: 6, flexDirection: 'row', alignItems: 'center', gap: 5 } },
    p.menuPercent && b.percent >= 0 ? h(Text, { style: { fontSize: 12, fontWeight: 500, color } }, `${b.percent}%`) : null,
    h(BatteryGlyph, { percent: b.percent, charging: b.charging, color }));
}

// ---- Bluetooth pairing requests (drawn by the system) ----

export function BluetoothPrompt() {
  const st = useBluetooth();
  const th = useSystemTokens();
  const [value, setValue] = useState('');
  const p = st?.prompt;
  useEffect(() => setValue(''), [p?.id]);
  if (!p) return null;
  const typed = p.kind === 'pin' || p.kind === 'passkey';
  const shows = p.kind === 'confirm' || p.kind === 'display-pin' || p.kind === 'display-passkey';
  const answer = (ok) => lord.bt.answer(p.id, ok, typed ? value : null);
  const valid = !typed || (p.kind === 'pin' ? /^[A-Za-z0-9]{1,16}$/.test(value) : /^\d{1,6}$/.test(value));
  return h(View, { style: { position: 'absolute', ...mainBox(), alignItems: 'center', justifyContent: 'flex-start',
    backgroundColor: 'rgba(0,0,0,0.3)', zIndex: 100000 } },
  h(View, { style: { marginTop: 90, width: 340, borderRadius: 14, padding: 22, gap: 10, alignItems: 'center', backgroundColor: th.windowBg,
    border: `1px solid ${th.separator}`, boxShadow: '0 20px 60px rgba(0,0,0,0.45)' } },
  h(View, { style: { width: 46, height: 46, borderRadius: 23, backgroundColor: '#0a84ff', alignItems: 'center', justifyContent: 'center' } },
    h(Text, { style: { fontSize: 24, fontWeight: 700, color: '#fff' } }, 'ᛒ')),
  h(Text, { style: { fontSize: 14, fontWeight: 700, color: th.text, textAlign: 'center' } }, t(`prefs.bt.prompt.${p.kind}.title`, { name: p.name })),
  h(Text, { style: { fontSize: 12, color: th.textDim, textAlign: 'center', lineHeight: 1.4 } }, t(`prefs.bt.prompt.${p.kind}.body`, { name: p.name })),
  shows && p.value ? h(Text, { style: { fontSize: 28, fontWeight: 700, color: th.text, letterSpacing: 4, marginVertical: 4, direction: 'ltr' } }, p.value) : null,
  typed ? h(View, { style: { alignSelf: 'stretch' } }, h(TextInput, { value, onChange: setValue, autoFocus: true, placeholder: t(`prefs.bt.prompt.${p.kind}.placeholder`),
    onSubmit: () => valid && answer(true), onCancel: () => answer(false) })) : null,
  h(View, { style: { flexDirection: 'row', gap: 10, marginTop: 8, alignSelf: 'stretch' } },
    p.kind.startsWith('display-')
      ? h(DialogButton, { label: t('prefs.bt.prompt.cancel'), onClick: () => answer(false), th })
      : [h(DialogButton, { key: 'n', label: t(p.kind === 'service' || p.kind === 'authorize' ? 'prefs.bt.prompt.deny' : 'prefs.bt.prompt.cancel'), onClick: () => answer(false), th }),
        h(DialogButton, { key: 'y', label: t(p.kind === 'service' || p.kind === 'authorize' ? 'prefs.bt.prompt.allow' : 'prefs.bt.prompt.pair'), primary: true, th,
          onClick: () => valid && answer(true) })])));
}

// ---- A Linux app asks for administrator rights (polkit, server/src/hw/pkagent.c) ----
//
// lord-server owns the password field: keys of this computer go straight to it, never through here.
// This only draws what lord-server reports, a character count included, and can only cancel: the
// keyboard's Enter is the only way to submit, so nothing injected can.

export const polkitPrompt = createStore(null);

function asker(p) {
  const id = p.who?.startsWith('desktop:') ? p.who.slice(8) : '';
  const app = id ? linuxAppFor(id) : null;
  return app ? shortAppName(app) : id || p.comm || t('passwords.polkit.unknown');
}

// the ScreenCast chooser and its pill (sdk/screencast.jsx) name only the app lord-server verified (its launcher's
// scope), never the name a process gives itself: with none they say it is an unknown program
function castAsker(p) {
  const who = typeof p?.who === 'string' ? p.who : '';
  if (who.startsWith('desktop:')) {
    const app = linuxAppFor(who.slice(8));
    return app ? shortAppName(app) : who.slice(8);
  }
  return who.startsWith('lpk:') ? who.slice(4) : who;
}
castNames({ app: castAsker, window: windowAppName });

export function PolkitPrompt() {
  const p = useStore(polkitPrompt);
  const th = useSystemTokens();
  const asking = !!p && p.state === 'ask' && p.local;
  const [blink, setBlink] = useState(true);
  // a caret like every text field's, and it shows the keys go here
  useEffect(() => {
    if (!asking) return undefined;
    const id = setInterval(() => setBlink((v) => !v), 530);
    return () => clearInterval(id);
  }, [asking]);
  if (!p) return null;
  const row = (label, value) => value ? h(View, { style: { flexDirection: 'row', gap: 8, alignSelf: 'stretch' } },
    h(Text, { style: { width: 86, fontSize: 12, color: th.textDim } }, label),
    h(Text, { style: { flex: 1, fontSize: 12, color: th.text, direction: 'ltr' } }, value)) : null;
  const note = p.state === 'check' ? t('passwords.polkit.checking')
    : p.wrong ? t('passwords.polkit.wrong')
      : p.local ? t('passwords.polkit.typeHere') : t('passwords.polkit.noKeyboard');
  return h(View, { style: { position: 'absolute', ...mainBox(), alignItems: 'center', justifyContent: 'center',
    backgroundColor: 'rgba(0,0,0,0.35)', zIndex: 100002 } },
  h(View, { style: { width: 420, padding: 18, gap: 10, borderRadius: 14, backgroundColor: th.windowBg, border: `1px solid ${th.separator}`,
    boxShadow: '0 20px 60px rgba(0,0,0,0.5)' } },
  h(Text, { style: { fontSize: 15, fontWeight: 700, color: th.text } }, t('passwords.polkit.title', { app: asker(p) })),
  h(Text, { style: { fontSize: 12, color: th.textDim, lineHeight: 1.4 } }, p.message),
  row(t('passwords.polkit.action'), p.action),
  row(t('passwords.polkit.account'), p.account),
  // sunken into the dialog, and lit while the keys go here
  h(View, { style: { height: 32, borderRadius: 8, paddingHorizontal: 10, flexDirection: 'row', alignItems: 'center',
    backgroundColor: th.mode === 'light' ? 'rgba(0,0,0,0.05)' : 'rgba(0,0,0,0.32)', border: `1px solid ${asking ? th.accent : th.separator}`,
    boxShadow: asking ? `0 0 0 3px ${th.accent}40` : 'none', direction: 'ltr' } },
  h(Text, { style: { fontSize: 13, color: th.text } }, '•'.repeat(Math.min(p.count, 40))),
  asking ? h(View, { style: { width: 1.5, height: 16, marginLeft: 1, backgroundColor: blink ? th.accent : 'transparent' } }) : null),
  h(Text, { style: { fontSize: 12, color: p.wrong ? '#ff453a' : th.textDim } }, note),
  h(View, { style: { flexDirection: 'row', justifyContent: 'flex-end' } },
    h(View, { style: { width: 110 } }, h(DialogButton, { label: t('passwords.cancel'), onClick: () => lord.polkit.cancel(p.id), th })))));
}

// ---- A Linux app asks to open a link or a file (org.freedesktop.portal.OpenURI, server/src/ui/portal.c) ----
//
// lord-server already checked a local file and says whether the app is the one in front. It opens the way
// everything else here does (sysopen.js); an app in the background waits for the person, one question at a
// time, with the host or the file's name first so a long address can't hide where it goes.

export const portalOpens = createStore([]);

// A file is opened by its name, so lord-server checks right before that it is still the file it checked
// when the app asked (not swapped while the question was up); otherwise it is refused.
function runOpen(id, plan) {
  try {
    if (plan.action === 'url') return openUrl(plan.target) ? 0 : 2;
    if (!lord.portal.still || lord.portal.still(id) !== true) return 2;
    if (plan.action === 'file') openFileWithApps(plan.target);
    else if (plan.action === 'folder') wm.open('files', { new: true, args: { path: plan.target, nonce: Date.now() } });
    else if (plan.action === 'reveal') revealPath(plan.target);
    else return 2;
    return 0;
  } catch (e) { return 2; }
}

function portalOpen(req) {
  if (!req || !has('portal') || !lord.portal.opened) return;
  const plan = openPlan(req);
  if (plan.action === 'refuse') lord.portal.opened(req.id, 2);
  else if (!plan.ask) lord.portal.opened(req.id, runOpen(req.id, plan));
  else portalOpens.set([...portalOpens.get(), { ...req, plan }]);
}

function answerOpen(p, yes) {
  portalOpens.set(portalOpens.get().filter((x) => x.id !== p.id));
  lord.portal.opened(p.id, yes ? runOpen(p.id, p.plan) : 1);
}

export function PortalOpenPrompt() {
  const list = useStore(portalOpens);
  const th = useSystemTokens();
  const p = list[0];
  if (!p) return null;
  const { action, target } = p.plan;
  const web = action === 'url';
  const file = splitPath(target);
  const app = asker(p);
  const title = web ? t('shell.portalOpen.titleUrl', { app }) : action === 'file' ? t('shell.portalOpen.titleFile', { app })
    : action === 'folder' ? t('shell.portalOpen.titleFolder', { app }) : t('shell.portalOpen.titleReveal', { app });
  const line = { alignSelf: 'stretch', direction: 'ltr', textAlign: 'center', overflow: 'hidden', textOverflow: 'ellipsis' };
  return h(View, { style: { position: 'absolute', ...mainBox(), alignItems: 'center', justifyContent: 'flex-start',
    backgroundColor: 'rgba(0,0,0,0.3)', zIndex: 100000 } },
  h(View, { style: { marginTop: 90, width: 380, borderRadius: 14, padding: 22, gap: 10, alignItems: 'center', backgroundColor: th.windowBg,
    border: `1px solid ${th.separator}`, boxShadow: '0 20px 60px rgba(0,0,0,0.45)' } },
  h(Text, { style: { fontSize: 14, fontWeight: 700, color: th.text, textAlign: 'center' } }, title),
  h(Text, { style: { ...line, fontSize: 13, fontWeight: 600, color: th.text, whiteSpace: 'nowrap' } }, web ? urlHost(target) : file.name),
  h(Text, { style: { ...line, fontSize: 11, color: th.textDim, numberOfLines: 2 } }, (web ? target : file.dir).slice(0, 300)),
  h(Text, { style: { fontSize: 12, color: th.textDim, textAlign: 'center', lineHeight: 1.4 } }, t('shell.portalOpen.body')),
  h(View, { style: { flexDirection: 'row', gap: 10, marginTop: 8, alignSelf: 'stretch' } },
    h(DialogButton, { label: t('shell.portalOpen.cancel'), onClick: () => answerOpen(p, false), th }),
    h(DialogButton, { label: action === 'reveal' ? t('shell.portalOpen.show') : t('shell.portalOpen.open'), primary: true, th,
      onClick: () => answerOpen(p, true) }))));
}

// ---- Linux apps' notifications (org.freedesktop.Notifications, server/src/hw/notifyd.c) ----

const fromLinux = new Map();            // D-Bus id → our notification id
function linuxNotify(n) {
  if (!n || !n.id) return;
  const app = linuxAppFor(n.desktop || n.app);
  const key = `linux:${app?.desktopId || n.desktop || n.app || 'app'}`;
  const prev = fromLinux.get(n.id);
  if (prev) dismissNotification(prev, 'replaced');
  const reason = { closed: 2, expired: 1, action: 2, replaced: 0, remote: 0 };
  const id = notify({
    app: key, label: shortAppName(app) || n.app, icon: app?.Icon || null, title: n.title, body: n.body, sticky: n.urgency === 2,
    timeout: n.timeout > 0 ? n.timeout : 6000,
    actions: (n.actions || []).filter(([k]) => k !== 'default').map(([k, label]) => ({ id: k, label, run: () => lord.notifyd.action(n.id, k) })),
    onClose: (why) => { if (fromLinux.get(n.id) === id) fromLinux.delete(n.id); if (reason[why]) lord.notifyd.closed(n.id, reason[why]); },
  });
  if (id) fromLinux.set(n.id, id);
}

// ---- A web app asks for a device or for notifications (org.lordos.WebApps1.Ask, server/src/ui/webask.c) ----
//
// lord-server already checked that the question comes from the lord-web it started for that app and profile. One
// question at a time; Don't allow is the default (Enter and Escape), as in every dialog that lets something in
// (dialogkeys.js): Allow needs a click, or Tab to it and then Space. "Remember" keeps the answer in the registry
// (Settings → Web apps changes it later); off, it counts for this time only.
export function WebAppAskPrompt() {
  const list = useStore(webAsks);
  const th = useSystemTokens();
  const [remember, setRemember] = useState(true);
  const [focus, setFocus] = useState('cancel');
  const token = useRef({}).current;
  const q = list[0];
  const owner = useStore(dialogStack, (l) => keyOwner(l) === token);
  const latest = useRef(null);
  latest.current = { q, remember, focus };
  useEffect(() => {
    if (!q) return undefined;
    setFocus('cancel');
    dialogStack.open(token, null, true);
    return () => dialogStack.close(token);
  }, [q?.id]);
  useEffect(() => addListener('keydown', (e) => {
    const cur = latest.current;
    if (!cur.q || dialogStack.owner() !== token) return;
    const r = dialogKey(e, { destructive: true, focus: cur.focus });
    if (r) dialogStack.take(e);
    if (r?.focus) setFocus(r.focus);
    else if (r?.press) answerWebAsk(cur.q, r.press === 'ok', cur.remember);
  }), []);
  if (!q) return null;
  const entry = webAppEntry(q.app);
  const prof = profileOf(entry, q.profile);
  const app = entry ? (entry.profiles.length > 1 && prof ? t('webapps.windowName', { app: entry.name, profile: prof.name }) : entry.name) : q.app;
  const ring = (which) => (owner && focus === which ? { boxShadow: `0 0 0 2px ${th.accent}` } : null);
  return h(View, { style: { position: 'absolute', ...mainBox(), alignItems: 'center', justifyContent: 'flex-start',
    backgroundColor: 'rgba(0,0,0,0.3)', zIndex: 100000 } },
  h(View, { style: { marginTop: 90, width: 380, borderRadius: 14, padding: 22, gap: 10, alignItems: 'center', backgroundColor: th.windowBg,
    border: `1px solid ${th.separator}`, boxShadow: '0 20px 60px rgba(0,0,0,0.45)' } },
  h(Text, { style: { fontSize: 14, fontWeight: 700, color: th.text, textAlign: 'center', lineHeight: 1.4 } }, t(`webapps.ask.${q.kind}`, { app })),
  entry ? h(Text, { style: { alignSelf: 'stretch', fontSize: 11, color: th.textDim, textAlign: 'center', direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' } }, entry.origin) : null,
  h(Text, { style: { fontSize: 12, color: th.textDim, textAlign: 'center', lineHeight: 1.4 } }, t('webapps.ask.where')),
  h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: 8, alignSelf: 'stretch' } },
    h(Text, { style: { flex: 1, fontSize: 12, color: th.text, textAlign: 'start' } }, t('webapps.ask.remember')),
    h(Toggle, { small: true, on: remember, onChange: setRemember })),
  h(View, { style: { flexDirection: 'row', gap: 10, marginTop: 8, alignSelf: 'stretch' } },
    h(View, { style: { flex: 1, height: 28, borderRadius: 7, ...ring('cancel') } }, h(DialogButton, { label: t('webapps.ask.deny'), onClick: () => answerWebAsk(q, false, remember), th })),
    h(View, { style: { flex: 1, height: 28, borderRadius: 7, ...ring('ok') } }, h(DialogButton, { label: t('webapps.ask.allow'), primary: true, th, onClick: () => answerWebAsk(q, true, remember) })))));
}

// ---- events from lord-server ----

globalThis.__lordHw = (kind, data) => {
  switch (kind) {
    case 'display': displays.set(data); break;
    case 'idle': onIdle(data); break;
    case 'power': {
      const wasAC = battery.get()?.onAC;
      battery.set(data);
      checkLow(data);
      if (data?.onAC !== wasAC) applyIdle();
      break;
    }
    case 'bt': case 'bt-prompt': bluetooth.set(data); break;
    case 'bt-result': for (const fn of btResults) { try { fn(data); } catch (e) { console.log(`bt: ${e}`); } } break;
    case 'ppd': powerProfiles.set(data); break;
    case 'ppd-result': for (const fn of ppdResults) { try { fn(data); } catch (e) { console.log(`ppd: ${e}`); } } break;
    case 'timedate': timedate.set(data); break;
    case 'polkit': polkitPrompt.set(data); break;
    case 'portal-file': portalFile.set(data); break;
    case 'portal-open': portalOpen(data); break;
    case 'portal-open-gone': portalOpens.set(portalOpens.get().filter((x) => x.id !== data?.id)); break;
    case 'portal-shortcuts': case 'portal-shortcuts-gone': case 'portal-shortcuts-changed': globalShortcutsEvent(kind, data); break;
    case 'portal-cast': case 'portal-cast-gone': case 'portal-casts': castEvent(kind, data); break;   // ScreenCast (screencast.jsx)
    case 'webapps-ask': case 'webapps-ask-gone': webAskEvent(kind, data); break;
    case 'notify': linuxNotify(data); break;
    case 'sni': case 'sni-gone': case 'sni-menu': trayEvent(kind, data); break;   // Linux apps' tray icons (trayitems.jsx)
    case 'notify-close': { const own = fromLinux.get(data?.id); if (own) dismissNotification(own, 'remote'); break; }
    case 'mpris': linuxPlayersChanged(data); break;
    default: break;
  }
};

// Called once by startShell: Night Light, idle timers and the battery item.
export function startHardware() {
  if (has('bt')) {
    let btDone = null;
    const btApply = () => {
      const r = btPowerAction(bluetooth.get(), settings.get().bluetoothOff, btDone);
      btDone = r.done;
      if (r.off) call(() => lord.bt.power(false));
    };
    bluetooth.subscribe(btApply);
    btApply();
  }
  if (!has('display')) return;
  nightTick();
  applyIdle();
  let last = '';
  settings.subscribe(() => {
    const s = settings.get();
    const key = JSON.stringify([s.night, s.power]);
    if (key === last) return;
    last = key;
    nightTick();
    applyIdle();
  });
  registerStatusItem('battery', BatteryStatus, 5);
  checkLow(battery.get());
}
