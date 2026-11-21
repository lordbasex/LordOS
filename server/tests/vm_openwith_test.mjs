// vm_openwith_test.mjs — apps/machines/openwith.js: a machine in Files' "Open with" (docs/VIRTUALIZATION.md 7.3).
// node server/tests/vm_openwith_test.mjs
import { opensOf, openMachines, windowsNeedsTools, openStep, withAsk, withoutAsks, askStale, OPEN_QUEUE, OPEN_WAIT_MS } from '../../apps/machines/openwith.js';
import { AGENT_CAPS } from '../../apps/machines/model.js';

let pass = 0, fail = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) pass++; else { fail++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
}
const C = AGENT_CAPS;
const win = (caps) => ({ state: 'connected', caps, system: 'Windows 11 Pro' });
eq('tools that open files, a Windows guest', opensOf(win(C.windows | C.open | C.clipboard)), 'windows');
eq('another guest whose helper opens files', opensOf({ state: 'connected', caps: C.clipboard | C.open, system: 'Debian' }), 'other');
eq('tools without "open" (older ones)', opensOf(win(C.windows | C.clipboard)), false);
eq('only qemu-guest-agent: nothing known', opensOf(win(C.files | C.exec)), undefined);
eq('not connected: nothing known', [opensOf({ state: 'waiting', caps: C.open }), opensOf(null)], [undefined, undefined]);

const list = [
  { dir: '/m/a.lordvm', id: 'a', name: 'Windows 11 Pro pepe', state: 'stopped', reason: 'suspended' },
  { dir: '/m/b.lordvm', id: 'b', name: 'Ubuntu', state: 'running' },
  { dir: '/m/c.lordvm', id: 'c', name: 'Win old', state: 'running' },
  { dir: '/m/d.lordvm', id: 'd', name: 'Newer', state: 'stopped', error: 'newer' },
  { dir: '/m/e.lordvm', id: 'e', name: 'Win E', state: 'running' },
];
const meta = { '/m/a.lordvm': { opens: 'windows' }, '/m/b.lordvm': {}, '/m/c.lordvm': { opens: false }, '/m/d.lordvm': { opens: 'windows' }, '/m/e.lordvm': { opens: 'windows' } };
eq('listed: tools that open files, readable', openMachines(list, (d) => meta[d]).map((m) => [m.id, m.opens, m.active]), [['a', 'windows', false], ['e', 'windows', true]]);
eq('a broken app.json lists nothing', openMachines(list, () => { throw new Error('x'); }), []);

const wl = [{ dir: '/w/a', state: 'stopped' }, { dir: '/w/b', state: 'running' }, { dir: '/w/c', state: 'stopped', error: 'newer' }];
eq('hint: a Windows machine with old tools', windowsNeedsTools(wl, (d) => ({ '/w/a': { os: 'windows', opens: false }, '/w/b': { os: 'debian' } })[d]), true);
eq('hint: a Windows machine whose tools never spoke', windowsNeedsTools(wl, (d) => ({ '/w/a': { os: 'windows' } })[d]), true);
eq('hint: none when one opens files', windowsNeedsTools(wl, (d) => ({ '/w/a': { os: 'windows' }, '/w/b': { os: 'windows', opens: 'windows' } })[d]), false);
eq('hint: none without a Windows machine', windowsNeedsTools(wl, (d) => ({ '/w/a': { os: 'debian', opens: false } })[d]), false);
eq('hint: an unreadable one doesn\'t count', windowsNeedsTools(wl, (d) => ({ '/w/c': { os: 'windows' } })[d]), false);
eq('hint: a broken app.json counts as none', windowsNeedsTools(wl, () => { throw new Error('x'); }), false);

eq('step: no session → start', [openStep(0, null), openStep(3, { ended: true, state: 'stopped' })], ['start', 'start']);
eq('step: paused → resume', openStep(3, { state: 'paused' }), 'resume');
eq('step: starting → wait', openStep(3, { state: 'starting' }), 'wait');
eq('step: running, tools not there yet → wait', openStep(3, { state: 'running', agent: win(C.files) }), 'wait');
eq('step: tools that open → send', openStep(3, { state: 'running', agent: win(C.windows | C.open) }), 'send');
eq('step: tools that can\'t → cant', openStep(3, { state: 'running', agent: win(C.windows) }), 'cant');

let asks = {};
asks = withAsk(asks, '/m/a', '/home/u/x.exe', 1, 100);
asks = withAsk(asks, '/m/a', '/home/u/x.exe', 2, 200);
eq('a path asked twice waits once', asks['/m/a'].map((a) => a.id), [1]);
for (let i = 0; i < OPEN_QUEUE + 3; i++) asks = withAsk(asks, '/m/a', `/f${i}`, 10 + i, 300);
eq('at most OPEN_QUEUE wait', asks['/m/a'].length, OPEN_QUEUE);
eq('taken', withoutAsks(asks, '/m/a'), {});
eq('nothing to take', withoutAsks({}, '/m/z'), {});
eq('stale after OPEN_WAIT_MS', [askStale({ at: 0 }, OPEN_WAIT_MS), askStale({ at: 0 }, OPEN_WAIT_MS + 1)], [false, true]);
console.log(`vm-openwith-test: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
