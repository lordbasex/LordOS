// sentinela.js — Sentinela, the session's process guard (docs/SENTINELA.md). startShell calls startSentinela(); it does
// nothing until the person turns it on in Settings → AI → Sentinela (settings.sentinela.on, off by default).
//
// While on: the Monitor's thread builds the process list every 10 s whether the Monitor is open or not
// (lord.monitor.watchProcs), the rules of sentinelamodel.js read it, and every 5 minutes the person's autostart folder
// is compared with what was there before. When a rule fires:
//   1. lord.sentinela.facts reads the process's origin in lord-server's engine thread (its program, the package that
//      installed it, where it lives, who started it);
//   2. with a model chosen and installed, the engine installed and the computer not on battery, the local model reads a
//      data-only summary (buildSummary) and its answer is checked (parseVerdict): a verdict, a short reason, and one
//      action of LordOS's closed list as a suggestion;
//   3. a desktop notification (sdk/notify.js: banner, sound, Do Not Disturb and the history like any other) with that
//      explanation, the facts and up to three of the closed actions; clicking it opens the alert in Settings.
// Nothing runs without the person's click. The alerts are kept in ~/.local/share/lordos/sentinela/alerts.json (the last
// 50), "It's mine" in settings.sentinela.ignore (settings.json, which no app can write).
//
//   sentinelaAlerts (store)        the alerts, newest first
//   sentinelaEngine (store)        { available, models: [{ id, bytes }], state } refreshed by refreshSentinelaEngine()
//   runSentinelaAction(alert, id)  one of the closed actions (end_block, pause, resume, limit, monitor, ignore)
//   sentinelaOnBattery()           alerts without analysis while on battery
import { createStore } from './store.js';
import { settings } from './settings.js';
import { notify } from './notify.js';
import { t, locale } from './i18n.js';
import { wm } from './windows.js';
import { firewall } from './monitor.js';
import * as fs from './fs.js';
import { LANGUAGE_NAMES } from './langnames.js';
import { ShieldGlyph } from './sentinelaglyph.js';
import { admin } from './admin.js';
import { llmPackageOk } from './llmcatalog.js';
import {
  SENTINELA_DEFAULTS, TRIGGER_DEFAULTS, createTracker, buildSummary, systemPrompt, parseVerdict, ignoreMatch, ruleAction, factLines,
  placeOf, clean,
} from './sentinelamodel.js';

const TICK_MS = 10 * 1000, AUTOSTART_MS = 5 * 60 * 1000, ANSWER_WAIT_MS = 25 * 1000, ANSWER_MAX_MS = 200 * 1000;
const HISTORY = 50;
const ALERTS = '.local/share/lordos/sentinela/alerts.json';

export const sentinelaAlerts = createStore([]);
export const sentinelaEngine = createStore({ available: false, models: [], state: null });

const api = () => (typeof lord !== 'undefined' && lord.sentinela) || null;
export const sentinelaPrefs = (s = settings.get()) => {
  const p = { ...SENTINELA_DEFAULTS, ...(s.sentinela || {}) };
  return { ...p, triggers: { ...TRIGGER_DEFAULTS, ...(p.triggers || {}) }, ignore: Array.isArray(p.ignore) ? p.ignore : [] };
};
export function setSentinelaPrefs(patch) {
  settings.set((s) => ({ ...s, sentinela: { ...(s.sentinela || {}), ...patch } }));
}

export function sentinelaOnBattery() {
  try { const b = lord.power.state(); return !!(b && b.hasBattery && b.onAC === false); } catch (e) { return false; }
}

export function refreshSentinelaEngine() {
  const a = api();
  let v = { available: false, models: [], state: null };
  if (a) { try { v = { available: !!a.available(), models: a.models() || [], state: a.state() }; } catch (e) { /* older server */ } }
  sentinelaEngine.set(() => v);
  return v;
}

// ---- the engine and the models: signed packages (pkg-install, an administrator), as the voices ----

const pkgArg = (list) => {
  if (!Array.isArray(list) || !list.length || !list.every(llmPackageOk)) throw new Error('not a model package');
  return list.join(' ');
};
// onProgress({ done, total }) from the package engine's "progress download" lines (docs/PACKAGES.md)
export const installLlm = (list, onProgress) => admin.run('pkg-install', pkgArg(list), (line) => {
  const m = /^progress download (\d+) (\d+)/.exec(String(line || ''));
  if (m && onProgress) onProgress({ done: Number(m[1]), total: Number(m[2]) });
});
export const removeLlm = (list) => admin.run('pkg-remove', pkgArg(list));

// a job of lord.sentinela: polled until its result comes, or null after ms
function job(id, ms) {
  return new Promise((resolve) => {
    const a = api();
    if (!a || !(id > 0)) { resolve(null); return; }
    const until = Date.now() + ms;
    const poll = () => {
      let r = null;
      try { r = a.take(id); } catch (e) { resolve(null); return; }
      if (r) resolve(r); else if (Date.now() > until) resolve(null); else setTimeout(poll, 400);
    };
    setTimeout(poll, 200);
  });
}

// ---- the alerts kept ----

function loadAlerts() {
  try {
    const j = JSON.parse(fs.read(ALERTS));
    if (Array.isArray(j)) sentinelaAlerts.set(() => j.filter((a) => a && typeof a.id === 'string').slice(0, HISTORY));
  } catch (e) { /* none yet */ }
}
function saveAlerts() {
  try {
    for (const d of ['.local', '.local/share', '.local/share/lordos', '.local/share/lordos/sentinela']) { try { fs.mkdir(d); } catch (e) { /* there */ } }
    fs.write(ALERTS, `${JSON.stringify(sentinelaAlerts.get().slice(0, HISTORY))}\n`);
  } catch (e) { /* a full disk: the alerts stay in memory */ }
}
function putAlert(a) {
  sentinelaAlerts.set((l) => [a, ...l.filter((x) => x.id !== a.id)].slice(0, HISTORY));
  saveAlerts();
}
function patchAlert(id, patch) {
  sentinelaAlerts.set((l) => l.map((x) => (x.id === id ? { ...x, ...patch } : x)));
  saveAlerts();
}
export function clearSentinelaAlerts() { sentinelaAlerts.set(() => []); saveAlerts(); }

// ---- actions: a closed list, each one run only from the person's click ----

const PROC_ACTIONS = new Set(['end_block', 'pause', 'resume', 'limit']);
export const actionLabel = (id) => t(`sentinela.action.${id}`);

export async function runSentinelaAction(alert, id) {
  if (!alert || !['end_block', 'pause', 'resume', 'limit', 'monitor', 'ignore'].includes(id)) return false;
  if (id === 'monitor') { wm.open('monitor', { args: { tab: 'processes', nonce: Date.now() } }); return true; }
  if (id === 'ignore') {
    const p = sentinelaPrefs();
    if (alert.exe && !ignoreMatch(p.ignore, alert.exe, alert.pkg)) {
      setSentinelaPrefs({ ignore: [{ exe: alert.exe, pkg: alert.pkg || null, name: alert.name, at: Date.now() }, ...p.ignore].slice(0, 200) });
    }
    patchAlert(alert.id, { done: 'ignore' });
    return true;
  }
  if (!PROC_ACTIONS.has(id) || !alert.pid || alert.st == null) return false;
  let err = null;
  if (id === 'end_block' && alert.app && firewall.enforceable(alert.app)) {
    // its network first, so it can't send anything while it ends
    try {
      const st = firewall.state();
      const cur = (st && st.restricted && st.restricted[alert.app]) || [];
      firewall.setRestricted(alert.app, [...new Set([...cur, 'no-network'])]);
    } catch (e) { err = 'sentinela.blockFailed'; }
  }
  const kind = { end_block: 'TERM', pause: 'STOP', resume: 'CONT', limit: 'LOWER' }[id];
  try { const r = lord.monitor.signal(alert.pid, alert.st, kind); if (r) err = r; } catch (e) { err = e.code || 'proc.failed'; }
  patchAlert(alert.id, { done: err ? null : id, error: err });
  if (err) notify({ label: t('sentinela.name'), icon: ShieldGlyph, title: t('sentinela.actionFailed', { name: alert.name }), body: t(`errors.${err}`), sound: false });
  return !err;
}

// the actions a notification shows (at most three): the suggested one first, then Monitor and "It's mine"
export function noticeActions(alert) {
  const first = alert.suggested && !['monitor', 'ignore'].includes(alert.suggested) ? alert.suggested : ruleAction(alert.rule, alert.place);
  const ids = alert.rule === 'autostart' ? ['monitor', 'ignore'] : [first, 'monitor', 'ignore'];
  return ids.filter((x, i) => ids.indexOf(x) === i);
}

// ---- one alert ----

const langName = (lang) => (LANGUAGE_NAMES.find((x) => x[0] === lang || x[0] === String(lang).split('-')[0]) || [0, 0, 'English'])[2];

function openAlert(a) { wm.open('settings', { args: { section: 'ai', page: 'sentinela', alert: a.id, nonce: Date.now() } }); }

function alertText(a) {
  const vars = { name: clean(a.name, 40) };
  const title = a.verdict === 'dangerous' || a.verdict === 'suspicious' ? t(`sentinela.title.${a.verdict}`, vars) : t(`sentinela.title.${a.rule}`, vars);
  const lines = factLines(a.ev || a, a.facts, { home: home(), user: user() }).map((l) => t(l.key, l.vars));
  const why = a.reason || t(`sentinela.why.${a.rule}`, { ...vars, place: t(`sentinela.place.${a.place || 'none'}`) });
  return { title, body: [why, ...lines].join('\n') };
}

function show(a) {
  const { title, body } = alertText(a);
  return notify({
    label: t('sentinela.name'), icon: ShieldGlyph, title, body, sticky: a.verdict === 'dangerous',
    actions: noticeActions(a).map((id, i) => ({ id, label: actionLabel(id), primary: i === 0 && id !== 'monitor', run: () => runSentinelaAction(a, id) })),
    defaultAction: { id: 'open', run: () => openAlert(a) },
  });
}

let home = () => { try { return fs.home(); } catch (e) { return ''; } };
let user = () => { const h = home(); return h.split('/').filter(Boolean).pop() || ''; };

async function handle(ev, prefs) {
  const a = api();
  let facts = null;
  if (a && ev.pid) { try { facts = await job(a.facts(ev.pid, ev.st), 15000); } catch (e) { facts = null; } }
  const exe = facts && facts.ok ? facts.exe : ev.exe;
  const pkg = facts && facts.ok ? facts.pkg : null;
  if (ignoreMatch(prefs.ignore, exe, pkg)) return null;
  const place = facts && facts.ok ? facts.place : placeOf(exe, home());
  const alert = {
    id: `${ev.at}-${ev.key || ev.file || ev.name}`, time: ev.at, rule: ev.rule, name: ev.name, pid: ev.pid || 0, st: ev.st ?? null,
    exe, pkg, place, app: ev.app || null, facts: facts && facts.ok ? facts : null, ev, verdict: null, reason: null, suggested: null,
    analysis: 'none', done: null,
  };
  // the analysis: a model chosen and installed, the engine there, not on battery
  const eng = refreshSentinelaEngine();
  const model = prefs.model && eng.models.some((m) => m.id === prefs.model) ? prefs.model : null;
  let pending = null;
  if (!model || !eng.available) alert.analysis = 'noModel';
  else if (sentinelaOnBattery()) alert.analysis = 'battery';
  else {
    try {
      const language = langName(locale.get().lang);
      const summary = buildSummary(ev, alert.facts, { home: home(), user: user(), cores: cores(), language });
      const id = a.analyze(model, systemPrompt(language), summary);
      alert.analysis = 'running';
      pending = job(id, ANSWER_MAX_MS);
    } catch (e) { alert.analysis = 'failed'; }
  }
  const apply = (r) => {
    const v = r && r.ok ? parseVerdict(r.content) : null;
    if (!v) return { analysis: r && r.ok ? 'rejected' : 'failed' };
    return { analysis: 'done', verdict: v.verdict, reason: v.reason, suggested: v.action, ms: r.ms };
  };
  if (pending) {
    const first = await Promise.race([pending, new Promise((res) => setTimeout(() => res(undefined), ANSWER_WAIT_MS))]);
    if (first !== undefined) Object.assign(alert, apply(first));
    else pending.then((r) => patchAlert(alert.id, apply(r)));      // too slow for the notice: it lands in the history
  }
  // the model found it normal: kept in the history, no notice
  putAlert(alert);
  if (alert.verdict !== 'normal') show(alert);
  return alert;
}

let coresN = 0;
const cores = () => { if (!coresN) { try { coresN = lord.monitor.info().cores || 0; } catch (e) { coresN = 0; } } return coresN; };

// ---- autostart: a new entry in ~/.config/autostart ----

function autostartEntries() {
  const dir = `${home()}/.config/autostart`;
  let list = [];
  try { list = fs.list(dir, { hidden: false }).filter((e) => !e.dir && e.name.endsWith('.desktop')); } catch (e) { return []; }
  return list.slice(0, 200).map((e) => {
    let exec = '';
    try { exec = (/^Exec=(.*)$/m.exec(String(fs.read(`${dir}/${e.name}`)).slice(0, 65536)) || [])[1] || ''; } catch (x) { /* unreadable */ }
    // only the program: never the arguments (they can hold anything)
    const prog = exec.trim().replace(/^env\s+(\S+=\S*\s+)*/, '').split(/\s+/)[0] || '';
    return { file: e.name, exe: prog.replace(/^"|"$/g, '') };
  });
}

function checkAutostart(prefs, now) {
  const cur = autostartEntries();
  const names = cur.map((x) => x.file).sort();
  if (!Array.isArray(prefs.autostartSeen)) { setSentinelaPrefs({ autostartSeen: names }); return []; }     // the first look: what is there is known
  const fresh = cur.filter((x) => !prefs.autostartSeen.includes(x.file));
  if (fresh.length || names.length !== prefs.autostartSeen.length) setSentinelaPrefs({ autostartSeen: names });
  return fresh.map((x) => ({ rule: 'autostart', name: x.file.replace(/\.desktop$/, ''), exe: x.exe, file: x.file, key: `autostart:${x.file}`, at: now }));
}

// ---- the service ----

let started = false, watching = false, tracker = null, uid = null, lastAutostart = 0, busy = 0;

export function sentinelaTick(now = Date.now()) {
  const prefs = sentinelaPrefs();
  const a = api();
  if (!prefs.on || !a || !lord.monitor?.watchProcs) {
    if (watching) {
      watching = false;
      tracker = null;
      try { lord.monitor.watchProcs(false); } catch (e) { /* older server */ }
      try { a?.stop(); } catch (e) { /* nothing running */ }
    }
    return [];
  }
  if (!watching) { watching = true; tracker = null; lastAutostart = 0; try { lord.monitor.watchProcs(true); } catch (e) { return []; } }
  const events = [];
  let list = null;
  try { list = lord.monitor.procsWatched(locale.get().lang); } catch (e) { list = null; }
  if (list && Array.isArray(list.list)) {
    if (uid == null) { const me = list.list.find((p) => p.self); uid = me ? me.uid : null; }
    if (!tracker) tracker = createTracker({ uid, home: home() });
    events.push(...tracker.feed(list.list, now).filter((ev) => prefs.triggers[ev.rule]));
  }
  if (prefs.triggers.autostart && now - lastAutostart >= AUTOSTART_MS) { lastAutostart = now; events.push(...checkAutostart(prefs, now)); }
  // at most two analyses at a time; the rest wait for the next tick's events (a rule fires once an hour per process)
  for (const ev of events.slice(0, 4)) {
    busy++;
    handle(ev, prefs).finally(() => { busy--; });
  }
  return events;
}

export function startSentinela() {
  if (started || typeof lord === 'undefined' || !lord.sentinela || typeof lord.greeter !== 'undefined') return;
  started = true;
  loadAlerts();
  sentinelaTick();
  setInterval(() => { try { sentinelaTick(); } catch (e) { console.error('sentinela:', e); } }, TICK_MS);
}

// tests: the clock's helpers
export const __sentinelaTest = { handle, show, alertText, setHome: (fn) => { home = fn; user = () => home().split('/').filter(Boolean).pop() || ''; }, busy: () => busy };
