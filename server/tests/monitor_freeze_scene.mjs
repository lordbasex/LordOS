// monitor_freeze_scene.mjs — the scene of tests/menubar_test.mjs monitor_freeze_scene.mjs (not run by itself): (0.8.9.6)
// opening a Terminal group of 300 programs in Monitor → Processes (Grouped) froze the desktop for about a second (every
// row of the group drawn at once, then again on every redraw of the list). Through runtime/index.js over the stub lord:
// opening the group draws only the rows in sight (plus a margin), stays within a frame's work, the list redrawn with the
// group open stays within it too, scrolling brings the next rows, and the order of the rows stays the same.
// Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { monitorProcesses, settings } from '../../sdk/index.js';
import { enableShellLayers } from '../../sdk/layers.js';
import { ROW_BUDGET } from '../../sdk/lists.jsx';
import { AppWindowContext } from '../../sdk/components.jsx';
import { ProcessesTab, ProcessLayers, procSight } from '../../apps/monitor/Processes.jsx';
import monitorEn from '../../assets/locales/en/monitor.json';

const { parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 30; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return parents.has(id); };
const textIds = (s) => [...texts].filter(([id, x]) => (s instanceof RegExp ? s.test(String(x)) : x === s) && inTree(id)).map(([id]) => id);
const nodeOf = (s) => { const id = textIds(s)[0]; return id ? parents.get(id) ?? id : null; };
const click = async (s) => { const p = nodeOf(s); if (!p) return false; __lordEvent('down', p, 5, 5, 0); __lordEvent('up', p, 5, 5, 0); await flush(); return true; };
// the table draws a few new rows per pass (sdk/lists.jsx ROW_BUDGET): wait until every pass is done
const settle = async () => { for (let i = 0; i < 40; i++) { await flush(); await new Promise((res) => setTimeout(res, 2)); } };
const key = async (name) => { __lordKey(0, 1, name, '', name.toLowerCase(), 0); __lordKey(0, 0, name, '', '', 0); await flush(); };

lord.locale.catalog = (lang, ns) => (lang === 'en' && ns === 'monitor' ? JSON.stringify(monitorEn) : null);
lord.monitor = new Proxy({ exeOwners: (paths) => Object.fromEntries(paths.map((p) => [p, p.startsWith('/usr/') ? 'coreutils' : ''])) },
  { get: (o, k) => (k in o ? o[k] : () => null) });
lord.pkg = new Proxy({ info: (name) => ({ name, origin: 'Debian' }) }, { get: (o, k) => (k in o ? o[k] : () => null) });

const MB = 1024 * 1024, ME = 1000, N = 300;
let st = 9000;
const P = (pid, ppid, name, o = {}) => ({ pid, ppid, name, uid: ME, user: 'alex', cmd: name, exe: `/usr/bin/${name}`, cpu: 0, rss: 10 * MB, threads: 1, st: st += 10,
  start: st / 100, prot: null, self: false, ...o });
const NAMES = ['bash', 'node', 'claude', 'sh', 'make', 'cc1', 'python3', 'git', 'ssh', 'rg'];
const list = [P(500, 1, 'lord-server', { app: 'lordos', self: true, prot: 'session', exe: '/usr/lib/lordos/lord-server' })];
for (let i = 0; i < N / 10; i++) {
  const sh = 1000 + i * 20;
  list.push(P(sh, 500, 'bash', { app: 'lordos:terminal' }));
  for (let k = 1; k < 10; k++) list.push(P(sh + k, k < 4 ? sh + k - 1 : sh + 3, NAMES[k], { app: 'lordos:terminal', cpu: ((i * k) % 7) / 300, threads: 1 + (k % 4) }));
}
const publish = (tick = 0) => monitorProcesses.set(() => ({ t: Date.now(), total: list.length, hz: 100,
  list: list.map((p) => ({ ...p, cpu: p.cpu + ((p.pid + tick) % 3) / 1000 })) }));
publish();

let created = 0;
const realCreate = lord.createNode;
lord.createNode = (...a) => { created++; return realCreate(...a); };

enableShellLayers();
rt.render(h(AppWindowContext.Provider, { value: { id: 'monitor-1', focused: true, args: null } },
  h(rt.View, { style: { width: 1100, height: 700 } }, h(ProcessesTab), h(ProcessLayers, { active: true }))));
await flush();
await click('Grouped');
settings.set({ monitor: { ...settings.get().monitor, procSort: { key: 'pid', dir: 1 } } });
publish(1);
await flush();
procSight.set({ y: 0, h: 600 });
await flush();

const rowsShown = () => textIds(/^\d{4}$/).length;          // a process row shows its PID
await click(monitorEn['comp.terminal']);
created = 0;
let t0 = performance.now();
__lordKey(0, 1, 'ArrowRight', '', 'arrowright', 0); __lordKey(0, 0, 'ArrowRight', '', '', 0);
for (let i = 0; i < 20; i++) await Promise.resolve();        // the event's own render and commit, before any later pass
const firstMs = performance.now() - t0, firstRows = rowsShown();
check('the first pass after opening a group of 300 programs draws at most ROW_BUDGET rows', firstRows > 0 && firstRows <= ROW_BUDGET, `${firstRows} rows`);
check('and stays within a frame\'s budget (node: under 50 ms; it took about 500 ms before, a row at a time is about 2 ms)', firstMs < 50, `${firstMs.toFixed(1)} ms`);
await settle();
const shown = rowsShown();
check('then only the rows in sight and a margin are drawn', shown > 20 && shown <= 45, `${shown} rows`);
check('with a bounded number of nodes', created < 4000, `${created} nodes`);

created = 0;
t0 = performance.now();
publish(2);
await flush();
const redrawMs = performance.now() - t0;
check('the list redrawn with the group open stays within a frame\'s work (node: 60 ms)', redrawMs < 60, `${redrawMs.toFixed(1)} ms`);
check('a redraw makes few new nodes (rows whose values did not change are kept)', created < 600, `${created} nodes`);

// the order is stable: by PID, the rows drawn are the first ones, the same after a redraw
const ALL = list.filter((p) => p.app === 'lordos:terminal').map((p) => p.pid).sort((a, b) => a - b);
const drawn = () => textIds(/^\d{4}$/).map((id) => Number(texts.get(id))).sort((a, b) => a - b);
await settle();
const first = drawn();
check('the rows drawn are the first ones in the order', first.length > 20 && first.every((p, i) => p === ALL[i]), first.join(','));
publish(3);
await settle();
check('the same rows after a redraw', drawn().join(',') === first.join(','));

// scrolling brings the next rows into sight
procSight.set({ y: 26 * 285, h: 600 });
await settle();
const late = drawn();
check('scrolled to the end: the last rows show', late.includes(ALL[ALL.length - 1]) && late.every((p) => ALL.indexOf(p) >= 200), late.join(','));
check('and the first ones are no longer drawn', !late.includes(ALL[0]));
process.exit(0);
