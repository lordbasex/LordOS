// monitor_describe_scene.mjs — the scene of tests/menubar_test.mjs monitor_describe_scene.mjs (not run by itself):
// (0.8.9.6) Monitor → Processes → Get Info's Description and "Investigate with Lumo" (apps/monitor/Describe.jsx) over a
// stub lord: a package's description in the person's language, an English one marked with "Translate with Lumo", an
// unknown program with Sentinela off (a note how to turn it on) and on (the button), the investigation sent for that
// process only on the click, its answer with the risk and the closed actions, and Blocklist asking first, then asking
// lord-server to block it and ending it. Prints "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { monitorProcesses, monitorDetail, settings } from '../../sdk/index.js';
import { enableShellLayers } from '../../sdk/layers.js';
import { AppWindowContext } from '../../sdk/components.jsx';
import { DescriptionPanel } from '../../apps/monitor/Describe.jsx';
import monitorEn from '../../assets/locales/en/monitor.json';

const { parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const wait = (ms) => new Promise((res) => setTimeout(res, ms));
const flush = async () => { for (let k = 0; k < 4; k++) { for (let i = 0; i < 30; i++) await new Promise((res) => setImmediate(res)); await wait(3); } };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return parents.has(id); };
const textIds = (s) => [...texts].filter(([id, x]) => (s instanceof RegExp ? s.test(String(x)) : x === s) && inTree(id)).map(([id]) => id);
const shows = (s) => textIds(s).length > 0;
const click = async (s) => { const id = textIds(s).pop(); const p = id ? parents.get(id) ?? id : null; if (!p) return false; __lordEvent('down', p, 5, 5, 0); __lordEvent('up', p, 5, 5, 0); await flush(); return true; };
const allText = () => [...texts].filter(([id]) => inTree(id)).map(([, x]) => String(x));

lord.locale.catalog = (lang, ns) => (lang === 'en' && ns === 'monitor' ? JSON.stringify(monitorEn) : null);
// lord-server's answers: bash's package has a description; nothing for the home-folder program
const describeCalls = [];
lord.monitor = new Proxy({
  exeOwners: (paths) => Object.fromEntries(paths.map((p) => [p, p === '/usr/bin/tool' ? 'tooling' : p.startsWith('/home/') ? '' : 'coreutils'])),
  describe: (kind, arg, lang) => { describeCalls.push(`${kind}:${arg}:${lang}`); return kind === 'pkg' && arg === 'tooling' ? { text: 'Made-up tools for tests', long: '', lang: 'en' } : false; },
  signal: (pid, st, kind) => { signals.push(`${pid}:${kind}`); return null; },
}, { get: (o, k) => (k in o ? o[k] : () => null) });
const signals = [], investigations = [], blocks = [];
let engine = false;
lord.sentinela = {
  available: () => engine, models: () => (engine ? [{ id: 'qwen3-1.7b', bytes: 1 }] : []), state: () => ({ state: 'off' }), stop: () => true,
  facts: () => 1,
  investigate: (model, system, user, pid, st) => { investigations.push({ model, user, pid, st }); return 2; },
  take: (id) => (id === 1 ? { ok: true, exe: '/home/alex/.cache/x/miner', pkg: null, place: 'home', chain: [] }
    : id === 2 ? { ok: true, ms: 900, content: '{"what":"It mines a cryptocurrency.","why":"A cron task started it.","risk":"high","action":"blocklist"}',
      evidence: { kind: 'script', interp: 'shell', bytes: 2300, sent: 2300, sha256: 'b'.repeat(64) } } : null),
  block: (pid, st) => { blocks.push(`${pid}:${st}`); return 3; },
  blockTake: (id) => (id === 3 ? { ok: true, entry: { id: 'b'.repeat(64), kind: 'script' } } : null),
  blocked: () => ({ version: 1, entries: [] }),
};

const ME = 1000;
const tool = { pid: 700, st: 5, name: 'tool', exe: '/usr/bin/tool', uid: ME, user: 'alex', cpu: 0, rss: 1, threads: 1, app: null };
const miner = { pid: 800, st: 9, name: 'miner', exe: '/home/alex/.cache/x/miner', uid: ME, user: 'alex', cpu: 1.9, rss: 1, threads: 4, app: null };
monitorProcesses.set(() => ({ t: 1, total: 2, hz: 100, list: [tool, miner] }));
monitorDetail.set(() => ({ pid: 800, cgroup: '/user.slice/user-1000.slice/session-2.scope', fds: [] }));
let which = tool;
function Scene() { return h(DescriptionPanel, { p: which, d: { cgroup: '/user.slice/x.scope', fds: [] }, own: true, key: which.pid }); }
enableShellLayers();
const render = () => rt.render(h(AppWindowContext.Provider, { value: { id: 'monitor-1', focused: true, args: null } }, h(rt.View, { style: { width: 600 } }, h(Scene))));
render();
await flush();
await wait(300);
await flush();
check('a package\'s description', shows('Made-up tools for tests'), JSON.stringify(allText()));
check('in English for an English interface: no translate button', !shows(monitorEn['desc.translate']));
check('nothing is investigated without a click', investigations.length === 0 && !describeCalls.some((c) => c.startsWith('investigate')));

// the unknown program, Sentinela off: a note how to turn it on
which = miner;
render();
await flush();
await wait(300);
await flush();
check('nothing known: Unknown', shows(monitorEn['desc.unknown']), JSON.stringify(allText()));
check('Sentinela off: how to turn it on, no button', shows(monitorEn['desc.lumoOff']) && !shows(monitorEn['desc.investigate']));

// Sentinela on with a local model: the button; the click asks for that process
engine = true;
settings.set((s) => ({ ...s, sentinela: { on: true, model: 'qwen3-1.7b' } }));
await flush();
check('Sentinela on: Investigate with Lumo', shows(monitorEn['desc.investigate']));
await click(monitorEn['desc.investigate']);
await wait(1500);
await flush();
check('the click investigates that process (PID and start time) with the local model', investigations.length === 1 && investigations[0].pid === 800 &&
  investigations[0].st === 9 && investigations[0].model === 'qwen3-1.7b', JSON.stringify(investigations));
check('the facts only: no command line arguments', investigations[0] && !/--/.test(investigations[0].user));
check('Lumo\'s answer, its risk and what it read', shows('It mines a cryptocurrency.') && shows(monitorEn['desc.risk.high']) &&
  shows('Lumo read the shell script (2 KB).'), JSON.stringify(allText()));
check('the closed actions', ['end', 'pause', 'block_network', 'blocklist'].every((a) => shows(monitorEn[`desc.action.${a}`])));
check('nothing is done by itself', signals.length === 0 && blocks.length === 0);
await click(monitorEn['desc.action.blocklist']);
check('Blocklist asks first', shows(/^Add “miner” to the blocklist\?/) && blocks.length === 0, JSON.stringify(allText()));
await click(monitorEn['desc.action.blocklist']);
await wait(600);
await flush();
check('then blocks that process\'s program and ends it', blocks[0] === '800:9' && signals.includes('800:KILL'), JSON.stringify({ blocks, signals }));
check('and says so', shows(monitorEn['desc.done.blocklist']));
process.exit(0);
