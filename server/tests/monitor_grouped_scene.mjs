// monitor_grouped_scene.mjs — the scene of tests/menubar_test.mjs monitor_grouped_scene.mjs (not run by itself): Monitor →
// Processes (apps/monitor/Processes.jsx) over a made-up process list, through runtime/index.js and the server's own
// events. Grouped turns on and is remembered; Chrome is one row with its totals; hovering it shows the summary (count,
// busiest and largest child); the origin badges stand on the owners lord-server reports; the Terminal's programs show
// who started them; Quit All lists the processes it had when chosen and skips a PID reused meanwhile; a child row's
// Quit signals only that process; while the pointer is over the list the order is held and an exited process stays
// greyed. Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { monitorProcesses, settings } from '../../sdk/index.js';
import { enableShellLayers } from '../../sdk/layers.js';
import { AppWindowContext } from '../../sdk/components.jsx';
import { ProcessesTab, ProcessLayers } from '../../apps/monitor/Processes.jsx';
import monitorEn from '../../assets/locales/en/monitor.json';

const { parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
// (0.8.9.6) the table draws a few new rows per pass (sdk/lists.jsx ROW_BUDGET), a moment apart: flush waits for them
const flush = async () => { for (let k = 0; k < 4; k++) { for (let i = 0; i < 30; i++) await new Promise((res) => setImmediate(res)); await new Promise((res) => setTimeout(res, 3)); } };
const wait = (ms) => new Promise((res) => setTimeout(res, ms));
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return parents.has(id); };
const textIds = (s) => [...texts].filter(([id, x]) => (s instanceof RegExp ? s.test(String(x)) : x === s) && inTree(id)).map(([id]) => id);
const shows = (s) => textIds(s).length > 0;
const nodeOf = (s, last = false) => { const ids = textIds(s); const id = last ? ids[ids.length - 1] : ids[0]; return id ? parents.get(id) ?? id : null; };
// last: the newest node with that text (a sheet's button over the row's own)
const click = async (s, last = false) => {
  const p = nodeOf(s, last);
  if (!p) return false;
  __lordEvent('down', p, 5, 5, 0); __lordEvent('up', p, 5, 5, 0);
  await flush();
  return true;
};
const hover = async (s) => { const p = nodeOf(s); if (p) __lordEvent('move', p, 5, 5, 0); await flush(); return !!p; };
const key = async (name, flags = 0) => { __lordKey(0, 1, name, '', name.toLowerCase(), flags); __lordKey(0, 0, name, '', '', flags); await flush(); };

// ---- the stub's lord: the strings, the owners, the signals ----
const signals = [];
lord.locale.catalog = (lang, ns) => (lang === 'en' && ns === 'monitor' ? JSON.stringify(monitorEn) : null);
lord.monitor = new Proxy({
  signal: (pid, st, kind) => { signals.push(`${pid}:${st}:${kind}`); return null; },
  exeOwners: (paths) => Object.fromEntries(paths.map((p) => [p, { '/usr/lib/lordos/lord-server': 'lordos', '/opt/google/chrome/chrome': 'google-chrome-stable',
    '/opt/google/chrome/chrome_crashpad_handler': 'google-chrome-stable', '/usr/bin/bash': 'bash' }[p] ?? ''])),
}, { get: (o, k) => (k in o ? o[k] : () => null) });
lord.pkg = new Proxy({ info: (name) => ({ name, origin: name === 'google-chrome-stable' ? 'Google LLC' : 'Debian' }) }, { get: (o, k) => (k in o ? o[k] : () => null) });

const MB = 1024 * 1024, ME = 1000;
let st = 9000;
const P = (pid, ppid, name, o = {}) => ({ pid, ppid, name, uid: ME, user: 'alex', cmd: name, exe: `/usr/bin/${name}`, cpu: 0, rss: 10 * MB, threads: 1, st: st += 10,
  start: st / 100, prot: null, self: false, ...o });
const CHROME = { app: 'desktop:google-chrome', appName: 'Google Chrome', exe: '/opt/google/chrome/chrome' };
let list = [
  P(500, 1, 'lord-server', { app: 'lordos', self: true, prot: 'session', exe: '/usr/lib/lordos/lord-server', rss: 300 * MB, cpu: 0.02 }),
  P(600, 500, 'chrome', { ...CHROME, rss: 400 * MB, cpu: 0.10, threads: 40 }),
  P(601, 600, 'chrome', { ...CHROME, rss: 50 * MB }),
  P(603, 601, 'chrome', { ...CHROME, rss: 200 * MB, cpu: 0.30, threads: 20 }),
  P(604, 601, 'chrome', { ...CHROME, rss: 250 * MB, cpu: 0.05 }),
  P(605, 600, 'chrome', { ...CHROME, rss: 120 * MB, cpu: 0.02 }),
  P(606, 1, 'chrome_crashpad', { ...CHROME, exe: '/opt/google/chrome/chrome_crashpad_handler', rss: 5 * MB }),
  P(900, 500, 'bash', { app: 'lordos:terminal', exe: '/usr/bin/bash', rss: 4 * MB }),
  P(901, 900, 'claude', { app: 'lordos:terminal', exe: '/home/alex/.local/bin/claude', rss: 90 * MB, cpu: 0.01 }),
  P(902, 901, 'sh', { app: 'lordos:terminal', exe: '/usr/bin/sh', rss: 1 * MB }),
  P(903, 902, 'sleep', { app: 'lordos:terminal', exe: '/usr/bin/sleep', rss: 1 * MB }),
  P(950, 1, 'tool', { exe: '/home/alex/bin/tool', rss: 3 * MB, cpu: 0.6 }),
];
const publish = () => monitorProcesses.set(() => ({ t: Date.now(), total: list.length, hz: 100, list: list.map((p) => ({ ...p })) }));
publish();

enableShellLayers();
// a focused window: the tab's keys (Delete, →) listen only then
rt.render(h(AppWindowContext.Provider, { value: { id: 'monitor-1', focused: true, args: null } },
  h(rt.View, { style: { width: 1100, height: 700 } }, h(ProcessesTab), h(ProcessLayers, { active: true }))));
await flush();
check('the flat list first: each process a row', textIds('chrome').length >= 5, String(textIds('chrome').length));

await click('Grouped');
check('Grouped turns on and is remembered in the settings', settings.get()?.monitor?.procGrouped === true, JSON.stringify(settings.get()?.monitor));
check('Chrome is one row, named after the app', shows('Google Chrome') && textIds('chrome').length === 0);
check('with its process count and total memory (1025 MB)', shows('6') && shows(/^1[.,]0\d? GB$/), JSON.stringify([...texts.values()].filter((x) => /B$/.test(String(x)))));
check('the Terminal\'s programs are one group, named after the component', shows(monitorEn['comp.terminal']));
check('a program in the home folder no package owns: Unpackaged', shows(monitorEn['badge.unpackaged']));
publish();                                              // the owners come back on the next redraw
await flush();
check('a third-party package shows its repository', shows('Google LLC'));

// the hover summary: after a moment, with what the group had when it opened
await hover('Google Chrome');
check('the order is held while the pointer is over the list', shows(monitorEn['proc.paused']));
await wait(800);
await flush();
check('the summary: the number of processes', shows('6 processes'));
check('the summary: the busiest child', shows(/^Most CPU: chrome \(PID 603\) · 30[.,]0%$/), JSON.stringify([...texts.values()].filter((x) => /Most/.test(String(x)))));
check('the summary: the largest child', shows(/^Most memory: chrome \(PID 600\) · 400/));
check('the summary: the group\'s rolling statistics of the last minute', shows(/^CPU, last minute: average 47[.,]0% · peak 47[.,]0% · p95 47[.,]0%$/),
  JSON.stringify([...texts.values()].filter((x) => /last minute/.test(String(x)))));

// held: an exited process stays, greyed, until the order is released
list = list.filter((p) => p.pid !== 950);
publish();
await flush();
check('held: the exited program stays, marked ended', shows('tool') && shows(monitorEn['proc.ended']));

// Quit All on Chrome: the processes it had when chosen
await click('Google Chrome');
await key('Delete');
check('Quit All asks first, naming the app', shows('Quit all of “Google Chrome”?'));
check('and lists the processes it had then', shows('6 processes') && shows('PID 604 · alex'));
const reusedSt = list.find((p) => p.pid === 603).st + 5;
list = list.map((p) => (p.pid === 603 ? { ...p, st: reusedSt } : p)).filter((p) => p.pid !== 605);
publish();
await flush();
await click(monitorEn['group.quitAll'], true);
await flush();
const sent = signals.splice(0);
check('Quit All signals the same processes only (PID and start time)', sent.length === 4 && sent.every((s) => s.endsWith(':TERM')) && !sent.some((s) => s.startsWith('603:') || s.startsWith('605:')),
  JSON.stringify(sent));
list = list.filter((p) => !(p.app === 'desktop:google-chrome' && p.st !== reusedSt));
publish();
await flush();
check('then it says the reused PID was skipped', shows('Skipped, their PID now belongs to another process: 1'));
await click(monitorEn['proc.close'], true);

// a child's Quit: only that process
await hover(monitorEn['comp.terminal']);
await click(monitorEn['comp.terminal']);
await key('ArrowRight');
check('→ opens the group: its processes show', shows('claude') && shows('sleep'));
check('a child shows who started it: Terminal › bash › claude › sh', textIds('claude').length >= 2 && textIds('sh').length >= 2 && textIds(monitorEn['comp.terminal']).length >= 2);
await click('903');
await key('Delete');
check('a child\'s Quit asks for that process', shows('Are you sure you want to quit “sleep”?'));
await click(monitorEn['proc.quit'], true);
check('and signals only it', signals.length === 1 && signals[0].startsWith('903:') && signals[0].endsWith(':TERM'), JSON.stringify(signals));

// the statistics' columns: average and peak over the chosen window, sortable
settings.set({ monitor: { ...settings.get().monitor, procStats: 5, procSort: { key: 'cpuPeak', dir: -1 } } });
publish();
await flush();
check('the statistics\' columns show and sort', shows(monitorEn['stats.cpuAvg']) && shows(monitorEn['stats.cpuPeak']) && shows(' ▼') && shows(monitorEn['stats.memPeak']),
  JSON.stringify([...texts.values()].filter((x) => /CPU|Mem/.test(String(x)))));

process.exit(0);
