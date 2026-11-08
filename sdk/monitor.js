// monitor.js — system metrics for the Monitor app (lord.monitor, see docs/MONITOR.md).
//
//   const samples = useMonitor({ procs: true, conns: false, geo: true });   // last 240 samples (60 s)
//   const procs = useStore(monitorProcesses);    // { t, total, list: [{ pid, name, cpu, rss, app… }] }
//   const conns = useStore(monitorConnections);  // { t, others, list: [{ id, proto, remote, cc, app… }] }
//   monitor.info()                               // { cpuModel, cores, geo, home: { lat, lon, cc } }
//
// lord-server samples in its own thread only while some useMonitor is mounted (and
// polls it): with the app closed it costs nothing. The process and connection lists are built
// only if some useMonitor asks for them, once per second.
//
// One process (Processes tab, docs/MONITOR.md "Process actions"):
//   monitor.watchProcess(p, { asAdmin })   Get Info: monitorDetail follows p once per second; returns stop()
//   await monitor.stopProcess(p, 'TERM' | 'KILL')   the user's own: lord-server signals it; another
//                                                  user's or root's: the root helper (administrators)
//
// Traffic history (0.8.2, docs/MONITOR.md "Traffic history"): lord-server records the machine's and
// each of the user's apps' bytes per minute (7 days) and per hour (30 days) in a file of the user's,
// while Settings → Privacy → Monitor → "Keep a traffic history" is on (settings.monitor.history).
//   monitor.history({ from, step, n, app })  → { res, oldest, now, rx: [n], tx: [n], top: [[app, rx, tx]] } | null
//   monitor.clearHistory()                   → true when it is gone
//
// Phase 2 (not implemented yet): monitor.deviceUsers() → [{ kind: 'camera' | 'microphone',
// app, name, pid }] and the monitorDevices store; and the firewall with per-app and per-domain rules,
// which rely on the same connection records (the `verdict` field, always 'allow' today).
import { useEffect, useRef, useState } from 'react';
import { createStore, useStore } from './store.js';
import { locale } from './i18n.js';
import { admin } from './admin.js';
import { codeError } from './errors.js';
import { settings } from './settings.js';

export const MONITOR_HISTORY = 240;             // 60 s at 4 samples per second
export const monitorSamples = createStore([]);
export const monitorProcesses = createStore(null);
export const monitorConnections = createStore(null);
export const monitorDevices = createStore([]);  // phase 2
export const monitorDetail = createStore(null); // Get Info of one process (procctl.c's JSON)
// Disk temperatures while the Monitor is open: { [disk]: { t: [ms], v: [°C] } }, one point per new
// reading (every 3 s; a resting spinning disk is not read), the last DISK_TEMP_HISTORY points.
export const DISK_TEMP_HISTORY = 600;           // 30 min at one reading every 3 s
export const diskTempHistory = createStore({});
const lastTempT = new Map();
function noteDiskTemps(s) {
  const add = [];
  for (const d of s?.disk?.disks || []) {
    if (d.temp == null || !d.tempT || lastTempT.get(d.name) === d.tempT) continue;
    lastTempT.set(d.name, d.tempT);
    add.push(d);
  }
  if (!add.length) return;
  diskTempHistory.set((h) => {
    const n = { ...h };
    for (const d of add) {
      const o = n[d.name] || { t: [], v: [] };
      const cut = o.t.length >= DISK_TEMP_HISTORY ? 1 : 0;
      n[d.name] = { t: [...o.t.slice(cut), d.tempT], v: [...o.v.slice(cut), d.temp] };
    }
    return n;
  });
}

const available = () => typeof lord.monitor !== 'undefined';
const wants = new Map();                        // mounted useMonitor → { procs, conns }
let timer = null;
let lastWant = '';

function wanted() {
  let procs = false, conns = false, geo = false;
  for (const w of wants.values()) { procs ||= w.procs; conns ||= w.conns; geo ||= w.conns && w.geo; }
  return { procs, conns, geo };
}

function poll() {
  const w = wanted();
  const key = `${w.procs}|${w.conns}|${w.geo}`;
  if (key !== lastWant) { lastWant = key; lord.monitor.want(w.procs, w.conns, w.geo); }
  const s = lord.monitor.sample();
  if (s) {
    monitorSamples.set((list) => (list.length >= MONITOR_HISTORY ? [...list.slice(1 - MONITOR_HISTORY), s] : [...list, s]));
    noteDiskTemps(s);
  }
  const lang = locale.get().lang;
  if (w.procs) { const p = lord.monitor.processes(lang); if (p) monitorProcesses.set(() => p); }
  if (watched && !watched.viaAdmin) { const d = lord.monitor.detailInfo(); if (d) monitorDetail.set(() => d); }
  if (w.conns) {
    const c = lord.monitor.connections(lang);
    if (c) {
      for (const x of c.list) x.verdict = 'allow';           // a socket that exists already passed the firewall
      monitorConnections.set(() => c);
      fwPoll();
    }
  }
}

function subscribe(token, w) {
  wants.set(token, w);
  if (timer || !available()) return;
  lord.monitor.start();
  lastWant = '';
  poll();
  timer = setInterval(poll, 250);
}

function unsubscribe(token) {
  wants.delete(token);
  if (wants.size || !timer) return;
  clearInterval(timer);
  timer = null;
  if (watched) stopWatching(watched);
  lord.monitor.want(false, false, false);
  lord.monitor.stop();
  monitorSamples.set([]);                       // the history starts over when reopened
  monitorProcesses.set(() => null);
  monitorConnections.set(() => null);
}

// geo: look up each connection's country in the local database (assets/monitor, no network).
// everyMs: how often the caller is redrawn (sampling stays every 250 ms; with
// the window in the background 1000 is better: the history loses no points and draws less).
// everyMs = 0: only requests the data, without redrawing (returns null).
export function useMonitor({ procs = false, conns = false, geo = true, everyMs = 250 } = {}) {
  const token = useRef(null);
  if (!token.current) token.current = {};
  useEffect(() => () => unsubscribe(token.current), []);
  useEffect(() => { subscribe(token.current, { procs, conns, geo }); }, [procs, conns, geo]);
  const [list, setList] = useState(() => monitorSamples.get());
  useEffect(() => {
    if (!everyMs) return undefined;
    let last = 0, timer = null;
    const push = () => { timer = null; last = Date.now(); setList(monitorSamples.get()); };
    push();
    const off = monitorSamples.subscribe(() => {
      if (timer) return;
      const wait = everyMs - (Date.now() - last);
      if (wait <= 20) push(); else timer = setTimeout(push, wait);
    });
    return () => { off(); if (timer) clearTimeout(timer); };
  }, [everyMs]);
  return everyMs ? list : null;
}

// ---- per-app firewall (lord.fw, docs/FIREWALL.md) ----
// lord-netperm (root) applies the rules in the kernel and counts each app's new connections;
// lord-server passes them to JS. firewallState: { status, counts: { app: [allowed, denied, rx, tx,
// rxRate, txRate, [[addr, rx, tx, rxRate, txRate, blocked], ...], blocked] } (the bytes when
// status.bytes; blocked: bytes of open connections dropped because a rule now denies them),
// events: [the last 200 new connections], rev }. rev goes up when a rule changes.
const fwApi = () => (typeof lord !== 'undefined' && lord.fw) || null;
export const firewallState = createStore({ status: null, counts: {}, events: [], rev: 0 });
let fwLast = 0;
function fwPoll(force) {
  const api = fwApi();
  if (!api) return;
  const now = Date.now();
  if (!force && now - fwLast < 1000) return;
  fwLast = now;
  try {
    const p = api.poll();
    const status = api.status();
    const at = Date.now();
    firewallState.set((s) => ({ ...s, status, counts: p.counts || {}, events: p.events.length ? [...s.events, ...p.events.map((e) => ({ ...e, at }))].slice(-200) : s.events }));
  } catch (e) { /* no lord.fw */ }
}
const fwBump = (r) => { firewallState.set((s) => ({ ...s, rev: s.rev + 1 })); fwPoll(true); return r; };
const fwCall = (fn, dflt) => { try { const api = fwApi(); return api ? fn(api) : dflt; } catch (e) { return dflt; } };
// A rule can only be applied to an app with its own scope: LordOS, a .desktop app, an .lpk or (0.8.5)
// one of LordOS's own components with its own scope (lordos:mail, lordos:store, lordos:print,
// lordos:terminal, lordos:lists). The essential system services (system:dns, system:time,
// system:network) are shown but never take a rule.
const fwEnforceable = (app) => app === 'lordos' || /^(desktop|lpk|lordos):/.test(app || '');
const fwCallThrow = (fn) => { const api = fwApi(); if (!api) throw codeError('fw.list.unavailable'); return fn(api); };
export const firewall = {
  available: () => !!fwApi(),
  enforceable: fwEnforceable,
  status: () => fwCall((f) => f.status(), null),
  state: () => fwCall((f) => f.state(), { mode: 'silent', trusted: [], restricted: {} }),
  appRule: (app) => (fwEnforceable(app) ? fwCall((f) => f.appRule(app), null) : null),
  setAppRule: (app, v) => fwBump(fwCall((f) => f.setAppRule(app, v), false)),
  setTrusted: (app, on) => fwBump(fwCall((f) => f.setTrusted(app, !!on), false)),
  setRestricted: (app, flags) => fwBump(fwCall((f) => f.setRestricted(app, flags), false)),
  setMode: (m) => fwBump(fwCall((f) => f.setMode(m), false)),
  blocks: (app, flag) => fwCall((f) => f.blocks(app, flag), false),
  rules: (app) => fwCall((f) => f.rules(app), []),
  addRule: (r) => fwBump(fwCall((f) => f.addRule(r), false)),
  removeRule: (id) => fwBump(fwCall((f) => f.removeRule(id), false)),
  updateRule: (id, r) => fwBump(fwCall((f) => f.updateRule(id, r), false)),
  ruleApps: () => fwCall((f) => (f.ruleApps ? f.ruleApps() : []), []),
  // (0.8.5) subscribed blocklists (docs/FIREWALL.md, "Blocklists"): lists() → { lists: [{ id, url, format,
  // custom, on, updated, tried, domains, nets, truncated, error }], busy, domains, nets } | null
  lists: () => fwCall((f) => (f.lists ? f.lists() : null), null),
  setList: (id, on) => fwBump(fwCallThrow((f) => f.setList(id, !!on))),
  addList: (url, format) => fwBump(fwCallThrow((f) => f.addList(url, format))),
  removeList: (id) => fwBump(fwCallThrow((f) => f.removeList(id))),
  updateLists: (force) => fwCall((f) => (f.updateLists ? f.updateLists(!!force) : 1), 1),
  listMatch: (names, addr) => fwCall((f) => (f.listMatch ? f.listMatch(names || [], addr || '') : null), null),
  // the app whose rules Settings → Privacy shows open (the Monitor's "Rules for this app")
  focus: createStore(null),
  refresh: () => fwPoll(true),
  RESTRICTIONS: ['no-internet', 'no-lan', 'no-network', 'no-incoming', 'no-camera', 'no-mic', 'no-screen', 'no-usb', 'no-bluetooth', 'no-files', 'no-clipboard', 'no-notifications'],
};

// ---- one process: Get Info and stop ----
let watched = null;                             // { pid, st, viaAdmin, timer }

// Get Info as root (another user's process, administrators only): the helper's proc-info sends the
// JSON in LOG lines that are joined here. If it fails, the session's partial details are used.
function adminDetail(w) {
  let text = '';
  admin.run('proc-info', `${w.pid} ${w.st}`, (line) => { text += line; }, { quiet: true }).then(() => {
    if (watched !== w) return;
    try { monitorDetail.set(() => JSON.parse(text)); } catch (e) { /* cut or broken: keep the last one */ }
  }).catch(() => {
    if (watched !== w) return;
    w.viaAdmin = false;                         // not allowed after all: what the session can read
    clearInterval(w.timer);
    lord.monitor.detail(w.pid, w.st);
  });
}

function watchProcess(p, { asAdmin = false } = {}) {
  if (!available() || !p) return () => {};
  if (watched) stopWatching(watched);
  const w = { pid: p.pid, st: p.st, viaAdmin: !!asAdmin && admin.available(), timer: null };
  watched = w;
  monitorDetail.set(() => null);
  if (w.viaAdmin) {
    lord.monitor.detail(0, 0);
    adminDetail(w);
    w.timer = setInterval(() => adminDetail(w), 1000);
  } else lord.monitor.detail(p.pid, p.st);
  return () => stopWatching(w);
}

function stopWatching(w) {
  if (watched !== w) return;
  clearInterval(w.timer);
  watched = null;
  lord.monitor.detail(0, 0);
  monitorDetail.set(() => null);
}

// own: the process runs as this user (p.uid === the session's uid); otherwise the root helper.
async function stopProcess(p, kind, { own } = {}) {
  if (kind !== 'TERM' && kind !== 'KILL') throw codeError('sys.badArgument');
  if (own) {
    const err = lord.monitor.signal(p.pid, p.st, kind);
    if (err) throw codeError(err);
    return;
  }
  await admin.run('proc-signal', `${p.pid} ${p.st} ${kind}`, null, { quiet: true });
}

// The history records in lord-server, with the window closed too; the switch is the user's setting
// (lord-server ignores it on the login screen and in isolated apps).
let historyOn = null;
function syncHistory() {
  const on = settings.get()?.monitor?.history !== false;
  if (on === historyOn || !available() || typeof lord.monitor.historyEnable !== 'function') return;
  historyOn = on;
  try { lord.monitor.historyEnable(on); } catch (e) { /* an older lord-server */ }
}
if (typeof lord !== 'undefined') { syncHistory(); settings.subscribe(syncHistory); }

function history({ from, step, n, app = null }) {
  if (!available() || typeof lord.monitor.history !== 'function') return null;
  try { return lord.monitor.history({ from: Math.floor(from), step: Math.floor(step), n: Math.floor(n), app }); } catch (e) { return null; }
}
function clearHistory() {
  if (!available() || typeof lord.monitor.historyClear !== 'function') return false;
  try { return !!lord.monitor.historyClear(); } catch (e) { return false; }
}

// (0.8.8.6) The machines' names by id ({ id: name }), for Monitor → Network's Machines row (a running
// machine's connections carry its id, "cgSub"). Read again at most every 2 s, and only when an id is new.
let vmNames = { at: 0, map: {} };
function machineNames(ids) {
  const want = [...(ids || [])];
  if (!want.length || want.every((id) => id in vmNames.map) || Date.now() - vmNames.at < 2000) return vmNames.map;
  vmNames.at = Date.now();
  try {
    if (typeof lord === 'undefined' || !lord.vm || typeof lord.vm.list !== 'function') return vmNames.map;
    const map = {};
    for (const m of lord.vm.list() || []) if (m && typeof m.id === 'string' && typeof m.name === 'string') map[m.id] = m.name;
    vmNames.map = map;
  } catch (e) { /* keeps the last names */ }
  return vmNames.map;
}

let infoCache = null;
export const monitor = {
  machineNames,
  available,
  info: () => (infoCache ||= available() ? lord.monitor.info() : null),
  deviceUsers: () => [],                        // phase 2
  watchProcess,
  stopProcess,
  history,
  clearHistory,
  // Monitor → Voltage (docs/MONITOR.md, "Voltage"): only while that view is on screen
  wantVoltage: (on) => { if (available()) lord.monitor.wantVoltage(!!on); },
  voltage: () => (available() ? lord.monitor.voltage() : null),
  // (0.8.8.6) Monitor → GPU and the Overview's GPU card (docs/MONITOR.md, "GPU"): only while one is on screen
  wantGpu: (on) => { if (available() && typeof lord.monitor.wantGpu === 'function') lord.monitor.wantGpu(!!on); },
  gpu: () => (available() && typeof lord.monitor.gpu === 'function' ? lord.monitor.gpu(locale.get().lang) : null),
};
