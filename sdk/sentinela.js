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
// The virtual machines of Machines are watched too (sdk/sentinelavm.js, docs/SENTINELA.md "Machines"): every 10 s
// lord.vm.list(), lord.vm.stats(dir) of the running ones and their network counts (lord.monitor.machinesWatched). A QEMU
// out of its machine's scope, a sandbox missing a part or a Workstation with another way out is always shown, with no
// model; the other machine rules may ask the model like the process rules. Machine actions: open it, show it in Monitor,
// pause, resume, shut down, each from the person's click.
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
import { brainBackend, brainThreads } from './aistore.js';
import { vmAnyActive } from './vmlibrary.js';
import {
  SENTINELA_DEFAULTS, TRIGGER_DEFAULTS, createTracker, buildSummary, systemPrompt, parseVerdict, ignoreMatch, ruleAction, factLines,
  placeOf, clean, presentAlert, buildInvestigation, investigateSystemPrompt, parseInvestigation, evidenceOf, translateSystemPrompt,
  translateInput, parseTranslation, INVESTIGATE_ACTIONS,
} from './sentinelamodel.js';
import {
  createMachineTracker, machineRow, machineSummary, machineSystemPrompt, machineNoticeActions, vmSuggested, vmWhy, vmFactLines,
  VM_ALWAYS, VM_TRIGGER_OF, VM_ACTIONS,
} from './sentinelavm.js';

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

// (0.8.9.6.2) where the engine computes (settings.brain.backend: 'auto' | 'cpu' | 'gpu'; Settings → AI, "Where they
// run"), told to lord-server whenever it changes; the engine restarts there at its next job (engine.h llm_set_backend)
let brainMode = null;
function syncBrainBackend() {
  const a = api();
  if (!a || typeof a.backend !== 'function') return;
  const m = brainBackend(settings.get());
  if (m === brainMode) return;
  try { a.backend(m); brainMode = m; } catch (e) { /* an older server: the processor only */ }
}
// (0.8.9.6.2) the Brain's processor cores (settings.brain.cpuThreads, 0 automatic; Settings → AI), told to lord-server
// once the slider rests (each new count restarts an idle engine: engine.h llm_set_threads); at start at once
let brainThreadsSent = null;
let threadsTimer = null;
function syncBrainThreads() {
  const a = api();
  if (!a || typeof a.threads !== 'function') return;
  if (brainThreads(settings.get()) === brainThreadsSent) return;
  if (threadsTimer) clearTimeout(threadsTimer);
  threadsTimer = setTimeout(() => {
    threadsTimer = null;
    const n = brainThreads(settings.get());
    if (n === brainThreadsSent) return;
    try { a.threads(n); brainThreadsSent = n; } catch (e) { /* an older server: two threads */ }
    refreshSentinelaEngine();
  }, brainThreadsSent === null ? 0 : 700);
}
export function setBrainThreads(n) {
  const v = Number.isFinite(n) && n > 0 ? Math.round(n) : 0;
  settings.set((s) => ({ ...s, brain: { ...(s.brain || {}), cpuThreads: v } }));
  syncBrainThreads();
}
export function setBrainBackend(mode) {
  settings.set((s) => ({ ...s, brain: { ...(s.brain || {}), backend: mode } }));
  syncBrainBackend();
  refreshSentinelaEngine();
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
const VM_LABEL = { vm_monitor: 'monitor', vm_pause: 'pause', vm_resume: 'resume' };
export const actionLabel = (id) => t(`sentinela.action.${VM_LABEL[id] || id}`);

// a machine's actions: by its id (a renamed machine moves its folder), only on the person's click
const vmApi = () => (typeof lord !== 'undefined' && lord.vm && typeof lord.vm.list === 'function' ? lord.vm : null);
function vmNow(alert) {
  const v = vmApi();
  const id = alert && alert.machine && alert.machine.id;
  if (!v || !id) return null;
  try { return (v.list() || []).find((m) => m && m.id === id) || null; } catch (e) { return null; }
}
function runVmAction(alert, id) {
  const m = vmNow(alert);
  if (id === 'vm_monitor') { wm.open('monitor', { args: { tab: 'processes', nonce: Date.now() } }); return true; }
  if (id === 'vm_open') {
    const live = m && ['running', 'paused', 'starting', 'stopping'].includes(m.state);
    // a running machine's own window; otherwise the library (opening its window would start it again)
    if (live) wm.open('machines', { instance: `vm-${m.id}`, args: { machine: m.dir, name: m.name } });
    else wm.open('machines');
    return true;
  }
  const how = { vm_pause: 'pause', vm_resume: 'resume', vm_shutdown: 'shutdown' }[id];
  let err = null;
  if (!m) err = 'sentinela.machineGone';
  else { try { if (!vmApi().stop(m.dir, how)) err = 'sentinela.machineFailed'; } catch (e) { err = 'sentinela.machineFailed'; } }
  patchAlert(alert.id, { done: err ? null : id, error: err });
  if (err) notify({ label: t('sentinela.name'), icon: ShieldGlyph, title: t('sentinela.actionFailed', { name: alert.name }), body: t(`errors.${err}`), sound: false });
  return !err;
}

export async function runSentinelaAction(alert, id) {
  if (alert && alert.kind === 'machine') return [...VM_ACTIONS, 'vm_resume'].includes(id) ? runVmAction(alert, id) : false;
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
  if (alert && alert.kind === 'machine') return machineNoticeActions(alert);
  const first = alert.suggested && !['monitor', 'ignore'].includes(alert.suggested) ? alert.suggested : ruleAction(alert.rule, alert.place);
  const ids = alert.rule === 'autostart' ? ['monitor', 'ignore'] : [first, 'monitor', 'ignore'];
  return ids.filter((x, i) => ids.indexOf(x) === i);
}

// ---- one alert ----

const langName = (lang) => (LANGUAGE_NAMES.find((x) => x[0] === lang || x[0] === String(lang).split('-')[0]) || [0, 0, 'English'])[2];

function openAlert(a) { wm.open('settings', { args: { section: 'ai', page: 'sentinela', alert: a.id, nonce: Date.now() } }); }

function alertText(a) {
  const vars = { name: clean(a.name, 40) };
  const p = presentAlert(a);
  if (a.kind === 'machine') {
    const why = p.reason || vmWhy(a.ev || a, t);
    return { title: t(p.title, vars), body: [why, ...(p.note ? [t(p.note)] : []), ...vmFactLines(a.ev || a, t)].join('\n'), sticky: p.sticky || VM_ALWAYS.has(a.rule) };
  }
  const lines = factLines(a.ev || a, a.facts, { home: home(), user: user() }).map((l) => t(l.key, l.vars));
  const why = p.reason || t(`sentinela.why.${a.rule}`, { ...vars, place: t(`sentinela.place.${a.place || 'none'}`) });
  return { title: t(p.title, vars), body: [why, ...(p.note ? [t(p.note)] : []), ...lines].join('\n'), sticky: p.sticky };
}

function show(a) {
  const { title, body, sticky } = alertText(a);
  return notify({
    label: t('sentinela.name'), icon: ShieldGlyph, title, body, sticky,
    actions: noticeActions(a).map((id, i) => ({ id, label: actionLabel(id), primary: i === 0 && id !== 'monitor', run: () => runSentinelaAction(a, id) })),
    defaultAction: { id: 'open', run: () => openAlert(a) },
  });
}

let home = () => { try { return fs.home(); } catch (e) { return ''; } };
let user = () => { const h = home(); return h.split('/').filter(Boolean).pop() || ''; };

// a machine's alert: the "always" rules with no model; the others with the same engine and the same limits as a process's
async function handleMachine(ev, prefs) {
  const a = api();
  const alert = {
    id: `${ev.at}-${ev.key}-${ev.rule}`, time: ev.at, rule: ev.rule, kind: 'machine', name: ev.name, machine: ev.machine, pid: 0, st: null,
    ev, verdict: null, reason: null, suggested: null, analysis: 'none', done: null,
  };
  let pending = null;
  if (VM_ALWAYS.has(ev.rule)) alert.analysis = 'rule';
  else {
    const eng = refreshSentinelaEngine();
    const model = prefs.model && eng.models.some((m) => m.id === prefs.model) ? prefs.model : null;
    if (!model || !eng.available || !a) alert.analysis = 'noModel';
    else if (sentinelaOnBattery()) alert.analysis = 'battery';
    else {
      try {
        const language = langName(locale.get().lang);
        const id = a.analyze(model, machineSystemPrompt(language), machineSummary(ev, { language }));
        alert.analysis = 'running';
        pending = job(id, ANSWER_MAX_MS);
      } catch (e) { alert.analysis = 'failed'; }
    }
  }
  const apply = (r) => {
    const v = r && r.ok ? parseVerdict(r.content) : null;
    if (!v) return { analysis: r && r.ok ? 'rejected' : 'failed' };
    return { analysis: 'done', verdict: v.verdict, reason: v.reason, suggested: vmSuggested(v.action), ms: r.ms };
  };
  if (pending) {
    const first = await Promise.race([pending, new Promise((res) => setTimeout(() => res(undefined), ANSWER_WAIT_MS))]);
    if (first !== undefined) Object.assign(alert, apply(first));
    else pending.then((r) => patchAlert(alert.id, apply(r)));
  }
  // a rule raised it: always shown, whatever the model said (presentAlert)
  putAlert(alert);
  show(alert);
  return alert;
}

async function handle(ev, prefs) {
  if (ev.kind === 'machine') return handleMachine(ev, prefs);
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
  // a rule raised it: always shown, whatever the model said (presentAlert)
  putAlert(alert);
  show(alert);
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

let started = false, watching = false, tracker = null, uid = null, lastAutostart = 0, busy = 0, vmTracker = null;

// the machines now: lord.vm.list(), lord.vm.stats(dir) of the running ones and their network counts (null: no new
// sample this tick, so no network sample is taken)
// Read only while lord-server says a machine is active (vmlibrary.js), and once more after the last one stopped, so an
// end is seen: no lord.vm call at all while nothing runs (docs/VIRTUALIZATION.md, "The shell and the machines")
const VM_ACTIVE = new Set(['starting', 'running', 'paused', 'stopping']);
let vmWasActive = false;
function machineRows() {
  const v = vmApi();
  if (!v) return null;
  const now = vmAnyActive();
  if (!now && !vmWasActive) return null;
  vmWasActive = now;
  let list = [];
  try { list = v.list() || []; } catch (e) { return null; }
  let net = null;
  try {
    const w = typeof lord.monitor.machinesWatched === 'function' ? lord.monitor.machinesWatched() : null;
    if (w && w.ok && w.machines && typeof w.machines === 'object') net = w.machines;
  } catch (e) { net = null; }
  const zero = { socks: 0, listen: 0, peers: 0, bytes: 0 };
  return list.filter((m) => m && typeof m.id === 'string' && m.id && !m.error).map((m) => {
    const live = VM_ACTIVE.has(m.state);
    let st = null;
    if (live) { try { st = v.stats(m.dir); } catch (e) { st = null; } }
    return machineRow(m, st, live && net ? (net[m.id] || zero) : null);
  });
}

export function sentinelaTick(now = Date.now()) {
  const prefs = sentinelaPrefs();
  const a = api();
  if (!prefs.on || !a || !lord.monitor?.watchProcs) {
    if (watching) {
      watching = false;
      tracker = null;
      vmTracker = null;
      try { lord.monitor.watchProcs(false); } catch (e) { /* older server */ }
      try { lord.monitor.watchMachines?.(false); } catch (e) { /* older server */ }
      try { a?.stop(); } catch (e) { /* nothing running */ }
    }
    return [];
  }
  if (!watching) {
    watching = true; tracker = null; vmTracker = null; lastAutostart = 0;
    try { lord.monitor.watchProcs(true); } catch (e) { return []; }
    try { lord.monitor.watchMachines?.(true); } catch (e) { /* older server */ }
  }
  const events = [];
  let list = null;
  try { list = lord.monitor.procsWatched(locale.get().lang); } catch (e) { list = null; }
  if (list && Array.isArray(list.list)) {
    if (uid == null) { const me = list.list.find((p) => p.self); uid = me ? me.uid : null; }
    if (!tracker) tracker = createTracker({ uid, home: home() });
    events.push(...tracker.feed(list.list, now).filter((ev) => prefs.triggers[ev.rule]));
  }
  if (prefs.triggers.autostart && now - lastAutostart >= AUTOSTART_MS) { lastAutostart = now; events.push(...checkAutostart(prefs, now)); }
  // the machines: the "always" rules whatever the toggles say
  const rows = machineRows();
  if (rows) {
    if (!vmTracker) vmTracker = createMachineTracker();
    const vmEvents = vmTracker.feed(rows, now).filter((ev) => VM_ALWAYS.has(ev.rule) || prefs.triggers[VM_TRIGGER_OF[ev.rule]] !== false);
    // first in line: a machine out of its limits doesn't wait for a busy process's analysis
    events.unshift(...vmEvents.filter((ev) => VM_ALWAYS.has(ev.rule)));
    events.push(...vmEvents.filter((ev) => !VM_ALWAYS.has(ev.rule)));
  }
  // at most two analyses at a time; the rest wait for the next tick's events (a rule fires once an hour per process)
  for (const ev of events.slice(0, 4 + events.filter((x) => VM_ALWAYS.has(x.rule)).length)) {
    busy++;
    handle(ev, prefs).finally(() => { busy--; });
  }
  return events;
}

export function startSentinela() {
  if (started || typeof lord === 'undefined' || !lord.sentinela || typeof lord.greeter !== 'undefined') return;
  started = true;
  syncBrainBackend();
  settings.subscribe(syncBrainBackend);
  syncBrainThreads();
  settings.subscribe(syncBrainThreads);
  loadAlerts();
  refreshBlocked();
  sentinelaTick();
  setInterval(() => { try { sentinelaTick(); } catch (e) { console.error('sentinela:', e); } }, TICK_MS);
  setInterval(() => { try { blockNotices(); } catch (e) { console.error('sentinela:', e); } }, BLOCK_NOTICE_MS);
}

// ---- (0.8.9.6) "Investigate with Lumo" (Monitor → Get Info, the person's click only; docs/SENTINELA.md "Investigate") ----

// Sentinela on, a model chosen and installed, the engine there: what Lumo needs
export function lumoReady() {
  const p = sentinelaPrefs();
  if (!p.on || !api()) return { ok: false, why: p.on ? 'noEngine' : 'off' };
  const eng = refreshSentinelaEngine();
  const model = p.model && eng.models.some((m) => m.id === p.model) ? p.model : null;
  if (!eng.available) return { ok: false, why: 'noEngine' };
  if (!model) return { ok: false, why: 'noModel' };
  return { ok: true, model };
}

// p: the process (Monitor's list); extra: { started, conns } (Get Info's cgroup and connections). Resolves
// { ok: true, result: { what, why, risk, action }, evidence, ms } or { ok: false, error }.
export async function investigateProcess(p, { started = '', conns = null } = {}) {
  const ready = lumoReady();
  if (!ready.ok) return { ok: false, error: `sentinela.lumo.${ready.why}` };
  const a = api();
  let facts = null;
  try { facts = await job(a.facts(p.pid, p.st), 15000); } catch (e) { facts = null; }
  if (facts && facts.ok === false && facts.error === 'proc.gone') return { ok: false, error: 'proc.gone' };
  const language = langName(locale.get().lang);
  const user_ = buildInvestigation(p, facts, { home: home(), user: user(), started, conns, own: true, language });
  let r = null;
  try { r = await job(a.investigate(ready.model, investigateSystemPrompt(language), user_, p.pid, p.st), ANSWER_MAX_MS); } catch (e) { return { ok: false, error: e.code || 'llm.failed' }; }
  if (!r) return { ok: false, error: 'llm.timeout' };
  if (!r.ok) return { ok: false, error: r.error || 'llm.failed' };
  const v = parseInvestigation(r.content);
  if (!v) return { ok: false, error: 'sentinela.lumo.rejected', evidence: evidenceOf(r.evidence) };
  return { ok: true, result: v, evidence: evidenceOf(r.evidence), ms: r.ms || 0, facts: facts && facts.ok ? facts : null };
}

// "Translate with Lumo": an English description into the person's language (kept per language by sdk/describe.js)
// model: try this installed model (Settings → AI's sheet) whenever a function that uses the model is on (Sentinela, or
// Writing unless turned off), not only Sentinela's own
export async function translateWithLumo(text, { model = null } = {}) {
  let ready = lumoReady();
  if (model) {
    const eng = refreshSentinelaEngine();
    const writingOff = (settings.get().writing || {}).on === false;
    const fnOn = sentinelaPrefs().on || !writingOff;
    ready = !api() || !eng.available ? { ok: false, why: 'noEngine' } : !eng.models.some((m) => m.id === model) ? { ok: false, why: 'noModel' }
      : !fnOn ? { ok: false, why: 'off' } : { ok: true, model };
  }
  if (!ready.ok) return { ok: false, error: `sentinela.lumo.${ready.why}` };
  const nonce = Array.from({ length: 16 }, () => '0123456789abcdef'[Math.floor(Math.random() * 16)]).join('');
  const language = langName(locale.get().lang);
  let r = null;
  try { r = await job(api().translate(ready.model, translateSystemPrompt(language), translateInput(text, nonce)), ANSWER_MAX_MS); } catch (e) { return { ok: false, error: e.code || 'llm.failed' }; }
  const out = r && r.ok ? parseTranslation(r.content) : null;
  return out ? { ok: true, text: out } : { ok: false, error: r && !r.ok ? r.error || 'llm.failed' : 'sentinela.lumo.rejected' };
}

// ---- (0.8.9.6) the blocklist (docs/SENTINELA.md "Blocklist"): ended whenever it starts, kept by lord-server ----

export const sentinelaBlocked = createStore([]);
export function refreshBlocked() {
  let l = [];
  try { const b = api()?.blocked?.(); l = b && Array.isArray(b.entries) ? b.entries : []; } catch (e) { l = []; }
  sentinelaBlocked.set(() => l);
  return l;
}
const blockJob = (id) => new Promise((resolve) => {
  const until = Date.now() + 60000;
  const poll = () => {
    let r = null;
    try { r = api().blockTake(id); } catch (e) { resolve({ ok: false, error: 'sys.failed' }); return; }
    if (r) resolve(r); else if (Date.now() > until) resolve({ ok: false, error: 'llm.timeout' }); else setTimeout(poll, 150);
  };
  setTimeout(poll, 100);
});
// p: the process; withPkg: every program of its package too
export async function blockProgram(p, { withPkg = false } = {}) {
  const a = api();
  if (!a || typeof a.block !== 'function') return { ok: false, error: 'sys.unsupported' };
  let id;
  try { id = a.block(p.pid, p.st, !!withPkg); } catch (e) { return { ok: false, error: e.code || 'sys.failed' }; }
  const r = await blockJob(id);
  refreshBlocked();
  return r;
}
export async function unblockProgram(entryId) {
  const a = api();
  if (!a || typeof a.unblock !== 'function') return { ok: false, error: 'sys.unsupported' };
  let id;
  try { id = a.unblock(entryId); } catch (e) { return { ok: false, error: e.code || 'sys.failed' }; }
  const r = await blockJob(id);
  refreshBlocked();
  return r;
}

// an app's network off (the firewall's existing no-network restriction), when the process has an app scope it can apply to
function cutNetwork(app) {
  if (!app || !firewall.enforceable(app)) return false;
  const st = firewall.state();
  const cur = (st && st.restricted && st.restricted[app]) || [];
  firewall.setRestricted(app, [...new Set([...cur, 'no-network'])]);
  return true;
}

// An investigation's action, from the person's click only: end, pause, block_network (the app's firewall) or blocklist
// (on the list, ended now, the app's network off). Resolves { ok, error?, network? }.
export async function runInvestigateAction(p, action) {
  if (!p || !INVESTIGATE_ACTIONS.includes(action) || action === 'none') return { ok: false, error: 'sys.badArgument' };
  const sig = (kind) => { try { const r = lord.monitor.signal(p.pid, p.st, kind); return r ? { ok: false, error: r } : { ok: true }; } catch (e) { return { ok: false, error: e.code || 'proc.failed' }; } };
  if (action === 'end') return sig('TERM');
  if (action === 'pause') return sig('STOP');
  if (action === 'block_network') {
    try { return cutNetwork(p.app) ? { ok: true, network: true } : { ok: false, error: 'sentinela.noAppNetwork' }; } catch (e) { return { ok: false, error: 'sentinela.blockFailed' }; }
  }
  // blocklist: its network first, then on the list (which ends it, and every later start of it)
  let network = false;
  try { network = cutNetwork(p.app); } catch (e) { network = false; }
  const r = await blockProgram(p);
  if (!r.ok) return r;
  sig('KILL');
  return { ok: true, network, entry: r.entry };
}

// the blocklist's notices: what it ended, at most one notice per program every 10 minutes
const BLOCK_NOTICE_MS = 3000, BLOCK_QUIET_MS = 10 * 60 * 1000;
const lastNotice = new Map();
function blockNotices(now = Date.now()) {
  const a = api();
  if (!a || typeof a.blockEvents !== 'function' || !sentinelaBlocked.get().length) return;
  let evs = [];
  try { evs = a.blockEvents() || []; } catch (e) { evs = []; }
  for (const ev of evs) {
    if (!ev || typeof ev.id !== 'string') continue;
    if (now - (lastNotice.get(ev.id) || 0) < BLOCK_QUIET_MS) continue;
    lastNotice.set(ev.id, now);
    notify({ label: t('sentinela.name'), icon: ShieldGlyph, title: t('sentinela.block.ended', { name: clean(ev.name, 40) }), body: t('sentinela.block.endedBody'), sound: false,
      defaultAction: { id: 'open', run: () => wm.open('settings', { args: { section: 'ai', page: 'sentinela', nonce: Date.now() } }) } });
  }
}

// tests: the clock's helpers
export const __sentinelaTest = { handle, show, alertText, machineRows, setHome: (fn) => { home = fn; user = () => home().split('/').filter(Boolean).pop() || ''; }, busy: () => busy };
