// vm_group_model_test.mjs — apps/machines/model.js's machine groups (docs/VIRTUALIZATION.md 19.9): the members found in
// lord.vm.list(), the group's state, groupPlan's start and stop order and its failures, when the link is made again, and
// what a `machines` request with a group folder does. node server/tests/vm_group_model_test.mjs
import { groupMembers, groupState, groupPlan, groupLinkKey, groupCliPlan, groupStartFailed, CLI } from '../../apps/machines/model.js';

let pass = 0, fail = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) pass++; else { fail++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
}
const G = '/home/a/Machines/Whonix.lordgroup';
const group = { dir: G, id: 'g1', name: 'Whonix', kind: 'whonix', error: '',
  members: [{ role: 'gateway', dir: `${G}/Gateway.lordvm`, start: 1, stop: 2, primary: false },
            { role: 'workstation', dir: `${G}/Workstation.lordvm`, start: 2, stop: 1, primary: true }] };
const mem = (role, state, extra = {}) => ({ dir: `${G}/${role === 'gateway' ? 'Gateway' : 'Workstation'}.lordvm`, name: `Whonix ${role}`, groupDir: G, role,
  primary: role === 'workstation', groupOk: true, error: '', state, ...extra });
const list = (gw, ws, gx = {}, wx = {}) => [mem('gateway', gw, gx), mem('workstation', ws, wx), { dir: '/home/a/Machines/Debian.lordvm', name: 'Debian', state: 'running', error: '' }];
const step = (p) => (p.member ? `${p.verb} ${p.role}` : p.wait ? `wait ${p.wait.role}` : p.fail ? `fail ${p.fail}` : p.done ? `done${p.link ? '+link' : ''}` : '?');

// members and state
eq('members by role', Object.keys(groupMembers(group, list('stopped', 'stopped'))).sort(), ['gateway', 'workstation']);
eq('another library machine is not a member', Object.values(groupMembers(group, list('stopped', 'stopped'))).every((m) => m.groupDir === G), true);
eq('off', groupState(group, list('stopped', 'stopped')), 'off');
eq('running', groupState(group, list('running', 'running')), 'running');
eq('partial: gateway only', groupState(group, list('running', 'stopped')), 'partial');
eq('partial: starting', groupState(group, list('running', 'starting')), 'partial');
eq('damaged: a member missing', groupState(group, [mem('gateway', 'stopped')]), 'damaged');
eq('damaged: not its own', groupState(group, list('stopped', 'stopped', {}, { groupOk: false })), 'damaged');
eq('damaged: unreadable', groupState(group, list('stopped', 'stopped', { error: 'invalid' })), 'damaged');

// start: the gateway first, the workstation once the gateway runs, then the link
eq('start: the gateway first', step(groupPlan(group, list('stopped', 'stopped'), 'start')), 'start gateway');
eq('start: wait for the gateway', step(groupPlan(group, list('starting', 'stopped'), 'start')), 'wait gateway');
eq('start: then the workstation', step(groupPlan(group, list('running', 'stopped'), 'start')), 'start workstation');
eq('start: a paused gateway counts as up', step(groupPlan(group, list('paused', 'stopped'), 'start')), 'start workstation');
eq('start: wait for the workstation', step(groupPlan(group, list('running', 'starting'), 'start')), 'wait workstation');
eq('start: both up → link', step(groupPlan(group, list('running', 'running'), 'start')), 'done+link');
eq('start: a crashed gateway is started again first', step(groupPlan(group, list('crashed', 'stopped'), 'start')), 'start gateway');
eq('start: the gateway shutting down: wait', step(groupPlan(group, list('stopping', 'stopped'), 'start')), 'wait gateway');
eq('start: only the workstation is off', step(groupPlan(group, list('running', 'crashed'), 'start')), 'start workstation');
eq('start: the order comes from group.json', step(groupPlan({ ...group, members: [{ ...group.members[0], start: 2 }, { ...group.members[1], start: 1 }] }, list('stopped', 'stopped'), 'start')), 'start workstation');
eq('start: damaged', step(groupPlan(group, [mem('gateway', 'stopped')], 'start')), 'fail damaged');
eq('the gateway failing stops the start', groupStartFailed('gateway'), 'gatewayFailed');
eq('the workstation failing leaves the gateway', groupStartFailed('workstation'), 'workstationFailed');

// stop: the workstation first, then the gateway
eq('stop: the workstation first', step(groupPlan(group, list('running', 'running'), 'stop')), 'shutdown workstation');
eq('stop: wait for it', step(groupPlan(group, list('running', 'stopping'), 'stop')), 'wait workstation');
eq('stop: then the gateway', step(groupPlan(group, list('running', 'stopped'), 'stop')), 'shutdown gateway');
eq('stop: a crashed workstation is off', step(groupPlan(group, list('running', 'crashed'), 'stop')), 'shutdown gateway');
eq('stop: a starting member: wait', step(groupPlan(group, list('starting', 'stopped'), 'stop')), 'wait gateway');
eq('stop: all off', step(groupPlan(group, list('stopped', 'stopped'), 'stop')), 'done');

// the link: both run under lord-vms not linked yet
eq('no link while one is off', groupLinkKey(group, list('running', 'stopped', { vmPid: 10 })), '');
eq('no link without the supervisors', groupLinkKey(group, list('running', 'running')), '');
const k1 = groupLinkKey(group, list('running', 'running', { vmPid: 10 }, { vmPid: 11 }));
eq('a key for both', k1, 'gateway:10,workstation:11');
eq('the same pair: the same key', groupLinkKey(group, list('running', 'paused', { vmPid: 10 }, { vmPid: 11 })), k1);
eq('the gateway started anew: a new key', groupLinkKey(group, list('running', 'running', { vmPid: 12 }, { vmPid: 11 })) !== k1, true);

// the session channel with a group folder
const groups = [group];
const cp = (verb, l, g = groups, dir = G) => { const p = groupCliPlan({ verb, dir }, g, l); return { code: p.code, key: p.key, group: p.group?.name, window: p.window, open: p.open?.role }; };
eq('a group not in the library', cp('start', list('stopped', 'stopped'), groups, '/home/b/Machines/W.lordgroup'), { code: CLI.NO_MACHINE, key: 'notInLibrary' });
eq('a group.json that failed', cp('start', list('stopped', 'stopped'), [{ ...group, error: 'invalid' }]), { code: CLI.NO_MACHINE, key: 'notInLibrary' });
eq('start in its window', cp('start', list('stopped', 'stopped')), { group: 'Whonix', window: true });
eq('start without a window', cp('start-detached', list('stopped', 'stopped')), { group: 'Whonix', window: false });
eq('start a partial group: the rest', cp('start-detached', list('running', 'stopped')), { group: 'Whonix', window: false });
eq('start a running group', cp('start', list('running', 'running')), { code: CLI.OK, key: 'alreadyRunning' });
eq('start a damaged group', cp('start', [mem('gateway', 'stopped')]), { code: CLI.FAILED, key: 'groupDamaged' });
eq('open: the workstation', cp('open', list('running', 'running')), { code: CLI.OK, key: 'opened', open: 'workstation' });
eq('open while off', cp('open', list('running', 'stopped')), { code: CLI.NOT_RUNNING, key: 'notRunning' });
eq('release', cp('release', list('stopped', 'stopped')), { code: CLI.OK, key: '' });

console.log(`vm_group_model_test: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
