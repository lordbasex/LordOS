// A running machine's own disk and network numbers on its row in Monitor → Processes (apps/monitor/vmiomodel.js;
// docs/VIRTUALIZATION.md 6.14).   node tests/monitor_vmio_test.mjs
import { diskStep, netOf, machineIdOf, machineIds, sweep } from '../../apps/monitor/vmiomodel.js';

let pass = 0, fail = 0;
const t = (name, ok, extra) => { if (ok) pass++; else { fail++; console.log(`FAIL ${name}${extra === undefined ? '' : ': ' + JSON.stringify(extra)}`); } };
const ID = '0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0';

// disk: two counter reads over their time
let s = diskStep(null, { at: 1000, ioBytes: 5e6 });
t('first read: no rate yet', s.disk === null && s.io === 5e6 && s.at === 1000, s);
s = diskStep(s, { at: 3000, ioBytes: 9e6 });
t('2 MB/s over 2 s', s.disk === 2e6, s);
t('a counter that went back (the machine started again): no rate', diskStep(s, { at: 4000, ioBytes: 1e3 }).disk === null);
t('time that did not move: no rate', diskStep(s, { at: 3000, ioBytes: 9.5e6 }).disk === null);
t('no counter (not in its cgroup, or off): nothing', diskStep(s, { diskBytes: 123 }).disk === null && diskStep(s, null).at === null);
t('a counter that is not a number: nothing', diskStep(s, { at: 'x', ioBytes: 1 }).disk === null && diskStep(s, { at: 5000, ioBytes: -1 }).disk === null);
t('a read after nothing: no rate', diskStep({ at: null, io: null, disk: null }, { at: 5000, ioBytes: 1 }).disk === null);

// network: lord-netperm's counts for the machine's scope
const counts = { [`lordos:vm-${ID}`]: [3, 0, 1e6, 2e5, 1500, 250, []], 'desktop:org.x': [1, 0, 9, 9, 99, 99] };
t('the machine\'s rates', JSON.stringify(netOf(counts, ID)) === JSON.stringify({ rx: 1500, tx: 250 }), netOf(counts, ID));
t('not counted: null', netOf(counts, 'aaaaaaaa-0000-0000-0000-000000000000') === null && netOf(null, ID) === null && netOf(counts, '') === null);
t('counts without rates: null', netOf({ [`lordos:vm-${ID}`]: [1, 0, 5, 5] }, ID) === null);
t('a bad rate is 0 beside a good one', JSON.stringify(netOf({ [`lordos:vm-${ID}`]: [1, 0, 5, 5, 10, 'x'] }, ID)) === JSON.stringify({ rx: 10, tx: 0 }));
t('another app\'s counts are never the machine\'s', netOf({ 'lordos:vm-': [0, 0, 0, 0, 5, 5] }, '') === null);

// which rows are machines
t('a machine\'s scope', machineIdOf(`desktop:lordos-vm-${ID}`) === ID);
t('a Tor gateway\'s scope', machineIdOf(`desktop:lordos-vm-tor-${ID}`) === ID);
t('not a machine', machineIdOf('desktop:org.gnome.Calculator') === null && machineIdOf('desktop:lordos-vm-../x') === null && machineIdOf(null) === null);
const groups = [
  { kind: 'app', item: { app: `desktop:lordos-vm-${ID}` } },
  { kind: 'app', item: { app: `desktop:lordos-vm-${ID}` } },
  { kind: 'app', gone: true, item: { app: 'desktop:lordos-vm-aaaaaaaa-0000-0000-0000-000000000000' } },
  { kind: 'tree', item: { app: `desktop:lordos-vm-${ID.replace('0', '1')}` } },
  { kind: 'app', item: { app: 'desktop:org.x' } },
];
t('the machines shown, once each, not an ended or a tree row', JSON.stringify(machineIds(groups)) === JSON.stringify([ID]), machineIds(groups));

// a sweep: only machines with a folder, each from its own previous read; a throwing read is no number
let calls = [];
const stats = (dir) => { calls.push(dir); if (dir === '/bad') throw new Error('x'); return { at: 2000 + calls.length * 1000, ioBytes: calls.length * 1e6 }; };
const dirs = { [ID]: '/m/a.lordvm', b: '/bad', c: null };
let m = sweep(null, [ID, 'b', 'c'], (id) => dirs[id], stats);
t('a sweep reads each machine with a folder', calls.length === 2 && m.has(ID) && m.has('b') && !m.has('c'), calls);
t('a read that threw is no number', m.get('b').disk === null && m.get('b').at === null);
m = sweep(m, [ID], (id) => dirs[id], stats);
t('the next sweep gives a rate and drops what is no longer shown', m.get(ID).disk !== null && m.get(ID).disk > 0 && !m.has('b'), [...m]);

console.log(`== monitor vmio: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
