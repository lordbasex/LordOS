// sentinela_settings_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): Settings → AI → Sentinela
// (apps/settings/Sentinela.jsx) over a stand-in lord.sentinela and the package engine (admin.run('pkg-install')).
// Checks: off by default with only its switch; on, the models of the catalog with their sizes and a Download button
// when none is installed; Download asks pkg-install for the engine and the model only (signed packages), shows the
// download's progress, and when it ends the model is installed and chosen; the triggers with "coming soon" for the
// network one; the limits; an alert of the history opens with its facts and the closed list of actions; "It's mine"
// entries can be watched again; (0.8.9.6) the blocklist with Remove. Prints "ok <name>" or "FAIL <name> <detail>".
import './sentinela_scene_stubs.js';
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { settings } from '../../sdk/settings.js';
import { sentinelaAlerts, sentinelaPrefs } from '../../sdk/sentinela.js';
import { SentinelaSettings } from '../../apps/settings/Sentinela.jsx';

const { texts, parents, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

let models = [], engine = false;
lord.sentinela = {
  available: () => engine, models: () => models, state: () => ({ state: 'off', model: null }),
  facts: () => 0, analyze: () => 0, take: () => null, stop: () => true,
  // (0.8.9.6) the blocklist: one program on it; Remove asks lord-server, whose answer comes on the next take
  blocked: () => ({ version: 1, entries: blockedList }),
  unblock: (id) => { unblocks.push(id); blockedList = blockedList.filter((x) => x.id !== id); return 7; },
  blockTake: (id) => (id === 7 ? { ok: true } : null),
};
let blockedList = [{ id: 'a'.repeat(64), kind: 'binary', path: '/home/ana/.cache/x/miner', size: 1000, interp: null, pkg: null, name: 'miner', at: 1795000000000 }];
const unblocks = [];
lord.power = { state: () => ({ hasBattery: false }) };
lord.fs = { ...(lord.fs || {}), home: () => '/home/ana', read: () => { throw new Error('none'); }, list: () => [], mkdir: () => true, write: () => true };
const requests = [];
lord.admin = { available: () => true, request: (action, arg) => { requests.push([action, arg]); return requests.length; } };

enableShellLayers();
rt.render(h(rt.View, null, h(SentinelaSettings, {})));
await flush();
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const textNode = (s) => [...texts].find(([id, x]) => String(x) === s && inTree(id))?.[0];
const shows = (s) => !!textNode(s);
const has = (re) => [...texts].some(([id, x]) => re.test(String(x)) && inTree(id));
const click = async (id, x = 5, y = 5) => { __lordEvent('down', id, x, y, 0); __lordEvent('up', id, x, y, 0); await flush(); };
const button = (label) => parents.get(textNode(label));

check('off by default', sentinelaPrefs().on === false);
check('its switch is shown', shows('sentinela.settings.on'));
check('nothing else while off', !shows('sentinela.settings.model') && !shows('sentinela.settings.triggers'));

settings.set((s) => ({ ...s, sentinela: { on: true } }));
await flush();
check('on: the local model section', shows('sentinela.settings.model'));
check('both catalog models listed', shows('Qwen3 1.7B') && shows('Qwen3 4B'));
check('no model: says so', shows('sentinela.settings.noModel'));
check('a Download button per model', [...texts.values()].filter((x) => x === 'sentinela.settings.download').length === 2);
check('the triggers', shows('sentinela.trigger.cpu') && shows('sentinela.trigger.autostart'));
check('the network trigger is "coming soon"', shows('sentinela.trigger.network') && shows('sentinela.settings.soon'));
check('the limits', shows('sentinela.settings.maxValue') && shows('sentinela.settings.batteryValue') && shows('sentinela.settings.whenValue'));
check('the engine is not installed: says so', shows('sentinela.settings.engineMissing'));

// Download the first: the engine and the model, nothing else
const dl = [...texts].filter(([id, x]) => x === 'sentinela.settings.download' && inTree(id)).map(([id]) => id)[0];
await click(parents.get(dl));
check('Download asks pkg-install for the engine and the model only', JSON.stringify(requests[0]) === '["pkg-install","llama.cpp lordos-llm-qwen3-1.7b"]', JSON.stringify(requests));
globalThis.__lordAdmin(1, 'LOG', 'progress download 641219632 1282439264');
await flush();
check('the download\'s progress', shows('sentinela.settings.downloading'));
models = [{ id: 'qwen3-1.7b', bytes: 1282439264 }];
engine = true;
globalThis.__lordAdmin(1, 'OK', '');
await flush();
await flush();
check('installed: chosen for the analyses', sentinelaPrefs().model === 'qwen3-1.7b', JSON.stringify(sentinelaPrefs().model));
check('… and marked in use', has(/sentinela\.settings\.inUse/));
check('the other model can still be downloaded', [...texts].filter(([id, x]) => x === 'sentinela.settings.download' && inTree(id)).length === 1);

// an alert of the history: its facts and every closed action
sentinelaAlerts.set(() => [{ id: 'a1', time: Date.now(), rule: 'cpu', name: 'miner-helper', pid: 4242, st: 777, exe: '/home/ana/bin/miner-helper', pkg: null,
  place: 'home', verdict: 'dangerous', reason: 'Parece un minero.', suggested: 'end_block', analysis: 'done',
  ev: { rule: 'cpu', name: 'miner-helper', exe: '/home/ana/bin/miner-helper', cpuAvg: 0.98, cpuPeak: 0.99, minutes: 5 },
  facts: { ok: true, exe: '/home/ana/bin/miner-helper', pkg: null, chain: [{ name: 'sh' }, { name: 'cron' }] } }]);
await flush();
check('the alert listed', shows('sentinela.title.dangerous'));
await click(parents.get(textNode('sentinela.title.dangerous')));
check('opened: the model\'s reason', shows('Parece un minero.'));
check('… what Lumo analyzed', shows('sentinela.analyzed') && shows('sentinela.fact.programNoPkg') && shows('sentinela.fact.chain') && shows('sentinela.fact.cpu'));
check('… every closed action', ['end_block', 'pause', 'limit', 'monitor', 'ignore'].every((a) => shows(`sentinela.action.${a}`)));
check('… and "you decide"', shows('sentinela.youDecide'));

// the machines: their triggers (one always on), and a machine's alert with the machine's actions only
check('the machine triggers', ['vmAlways', 'vmUsage', 'vmCrash', 'vmAnon'].every((x) => shows(`sentinela.trigger.${x}`)) && shows('sentinela.settings.always'));
check('what it sees of machines', shows('sentinela.settings.seesMachines'));
sentinelaAlerts.set(() => [{ id: 'v1', time: Date.now(), rule: 'vmSandbox', kind: 'machine', name: 'Whonix-Gateway', pid: 0, st: null,
  machine: { id: 'aaaa-1', name: 'Whonix-Gateway', dir: '/home/ana/Machines/Whonix-Gateway.lordvm', role: 'gateway', anon: true }, analysis: 'rule',
  ev: { rule: 'vmSandbox', kind: 'machine', missing: ['landlockFs', 'seccomp'], machine: { id: 'aaaa-1', name: 'Whonix-Gateway', role: 'gateway' } } }]);
await flush();
check('the machine alert listed', shows('sentinela.title.vmSandbox'));
await click(parents.get(textNode('sentinela.title.vmSandbox')));
check('… why, with what is missing', shows('sentinela.why.vmSandbox') && shows('sentinela.fact.machine') && shows('sentinela.analysis.rule'));
check('… the machine\'s actions', ['vm_open', 'vm_shutdown', 'monitor', 'pause'].every((a) => shows(`sentinela.action.${a}`)));
check('… and none of a process\'s', !shows('sentinela.action.end_block') && !shows('sentinela.action.ignore'));

// "It's mine" entries
settings.set((s) => ({ ...s, sentinela: { ...s.sentinela, ignore: [{ exe: '/home/ana/bin/tool', pkg: null, name: 'tool' }] } }));
await flush();
check('a program marked as mine is listed', shows('tool'));
await click(button('sentinela.settings.unignore'));
check('… and can be watched again', sentinelaPrefs().ignore.length === 0);

// (0.8.9.6) the blocklist: listed with its path and hash, and Remove takes it off
check('the blocklist lists the blocked program', shows('sentinela.block.title') && shows('miner') && has(/\/home\/ana\/\.cache\/x\/miner/));
await click(button('sentinela.block.remove'));
await new Promise((res) => setTimeout(res, 300));
await flush();
check('… and Remove takes it off the list', unblocks[0] === 'a'.repeat(64) && !shows('miner') && shows('sentinela.block.empty'), JSON.stringify(unblocks));

settings.set((s) => ({ ...s, sentinela: { ...s.sentinela, on: false } }));
await flush();
check('off again: only the switch', !shows('sentinela.settings.model'));
check('off with an empty blocklist: no blocklist either', !shows('sentinela.block.title'));
process.exit(0);
