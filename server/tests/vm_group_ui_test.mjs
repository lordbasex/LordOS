// vm_group_ui_test.mjs — a machine group in the Machines app (docs/VIRTUALIZATION.md 19.8): apps/machines/group.js (one
// library card per group, its menu, Destroy and start fresh's plan, the group's memory budget, the chips and search) and
// the group card of apps/machines/catalog.js (the Whonix card only with KVM on the host's architecture, the release
// folders, SHA-512 lists, the archive in Storage). node server/tests/vm_group_ui_test.mjs
import { libraryItems, groupMenu, freshPlan, groupBudget, sessionCapMb, groupShows, groupSpec, LOCKED_ROWS, groupDamage, archiveMatches, gatewayBar, gatewayOf, renameProblemKey } from '../../apps/machines/group.js';
import { systemsFor, releaseNames, newestRelease, releaseCmp, pickImage, entryOf, groupMachines, downloadedImages } from '../../apps/machines/catalog.js';
import { machineProtected, groupState, groupPlan } from '../../apps/machines/model.js';
import { readFileSync } from 'node:fs';

let pass = 0, fail = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) pass++; else { fail++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
}
const G = '/home/a/Machines/Whonix.lordgroup';
const group = { dir: G, id: 'g1', name: 'Whonix', kind: 'whonix', error: '', disposable: false, freshGateway: false,
  members: [{ role: 'gateway', dir: `${G}/Gateway.lordvm`, start: 1, stop: 2, primary: false },
            { role: 'workstation', dir: `${G}/Workstation.lordvm`, start: 2, stop: 1, primary: true }] };
const mem = (role, state, extra = {}) => ({ dir: `${G}/${role === 'gateway' ? 'Gateway' : 'Workstation'}.lordvm`, name: `Whonix ${role}`, groupDir: G, groupKind: 'whonix',
  profileLock: 'anonymity', role, primary: role === 'workstation', groupOk: true, error: '', state, vcpus: 1, memoryMb: 2048, accel: 'kvm', ...extra });
const deb = { dir: '/home/a/Machines/Debian.lordvm', name: 'Debian', state: 'stopped', error: '', vcpus: 4, memoryMb: 4096, accel: 'kvm' };
const list = (gw, ws, gx, wx) => [mem('gateway', gw, gx), mem('workstation', ws, wx), deb];

// one card per group; its members aren't cards of their own
const it = libraryItems(list('stopped', 'stopped'), [group]);
eq('the members are no cards', it.machines.map((m) => m.name), ['Debian']);
eq('one group card', it.groups.map((g) => [g.group.name, g.state, !!g.gw, !!g.ws]), [['Whonix', 'off', true, true]]);
const unr = libraryItems(list('stopped', 'stopped'), [{ dir: `${G}x.lordgroup`, name: 'Broken.lordgroup', error: 'invalid' }]);
eq('a group.json that can\'t be read: a damaged card named by its folder', unr.groups.map((g) => [g.group.name, g.state, !!g.unreadable]), [['Broken', 'damaged', true]]);
eq('a group from a newer LordOS shows no card', libraryItems([], [{ dir: G, name: 'N.lordgroup', error: 'newer' }]).groups.length, 0);
eq('the card\'s numbers', groupSpec(it.groups[0].gw, it.groups[0].ws), { vcpus: 2, memoryMb: 4096 });
// the members are protected machines (their windows and apps-mode windows: a neutral fill in every capture)
eq('a member is protected', [machineProtected(mem('gateway', 'stopped')), machineProtected(mem('workstation', 'stopped', { profileLock: '' }))], [true, true]);
eq('another machine isn\'t', machineProtected(deb), false);

// the menu: the approved mock's items, enabled by the members' states
const keys = (m) => m.filter((x) => x !== '-').map((x) => `${x.k}${x.disabled ? '-' : '+'}`).join(' ');
eq('menu, off', keys(groupMenu(it.groups[0])), 'start+ showWorkstation- showGateway- shutdown- poweroff- snapshot+ configure+ disposable+ logs+ rename+ showInFiles+ fresh+ delete+');
const on = libraryItems(list('running', 'running'), [group]).groups[0];
eq('menu, running', keys(groupMenu(on)), 'start- showWorkstation+ showGateway+ shutdown+ poweroff+ snapshot- configure+ disposable+ logs+ rename- showInFiles+ fresh- delete-');
const part = libraryItems(list('running', 'stopped'), [group]).groups[0];
eq('menu, the gateway alone (the workstation failed): Stop Gateway', keys(groupMenu(part)), 'start+ showWorkstation- showGateway+ stopGateway+ shutdown+ poweroff+ snapshot- configure+ disposable+ logs+ rename- showInFiles+ fresh- delete-');
eq('force off, fresh and delete are destructive', groupMenu(on).filter((x) => x.destructive).map((x) => x.k), ['poweroff', 'fresh', 'delete']);
eq('the disposable item is checked with the group', groupMenu({ ...it.groups[0], group: { ...group, disposable: true } }).find((x) => x.k === 'disposable').checked, true);
const dmg = libraryItems([mem('gateway', 'stopped'), deb], [group]).groups[0];
eq('a damaged group: Show in Files, Repair, Delete', keys(groupMenu(dmg)).split(' ').filter((x) => x.endsWith('+')), ['showInFiles+', 'repair+', 'delete+']);
eq('an unreadable group: Show in Files and Delete', keys(groupMenu(unr.groups[0])).split(' ').filter((x) => x.endsWith('+')), ['showInFiles+', 'delete+']);
const dmgOn = libraryItems([mem('gateway', 'running'), deb], [group]).groups[0];
eq('a damaged group with a member running: no Repair, no Delete', keys(groupMenu(dmgOn)).split(' ').filter((x) => /^(repair|delete)/.test(x)), ['repair-', 'delete-']);

// what is damaged (19.9): the Repair dialog's lines
eq('damage: the workstation missing', groupDamage(dmg), { members: ['workstation'], bases: [], unreadable: false, fetch: false });
const noBase = libraryItems(list('stopped', 'stopped'), [{ ...group, basesMissing: ['gateway'], archiveThere: false }]).groups[0];
eq('a base missing makes the group damaged', noBase.state, 'damaged');
eq('damage: a base missing, the download to fetch again', groupDamage(noBase), { members: [], bases: ['gateway'], unreadable: false, fetch: true });
eq('damage: a base missing, the download here', groupDamage({ ...noBase, group: { ...noBase.group, archiveThere: true } }).fetch, false);
eq('damage: unreadable', groupDamage(unr.groups[0]), { members: [], bases: [], unreadable: true, fetch: false });
eq('a running group with its base gone isn\'t damaged (QEMU keeps the base open)', groupState({ ...group, basesMissing: ['gateway'] }, list('running', 'running')), 'running');
eq('a base missing: the plan doesn\'t start the group', groupPlan({ ...group, basesMissing: ['workstation'] }, list('stopped', 'stopped'), 'start'), { fail: 'damaged' });
eq('a base missing: the plan still stops it', groupPlan({ ...group, basesMissing: ['workstation'] }, list('running', 'running'), 'stop').verb, 'shutdown');

// the archive fetched again must be the one group.json recorded
eq('archive: the recorded one (any case)', archiveMatches({ sha: 'AB12' }, { archiveSha512: 'ab12' }), true);
eq('archive: another one is refused', archiveMatches({ sha: 'ab13' }, { archiveSha512: 'ab12' }), false);
eq('archive: nothing recorded', archiveMatches({ sha: 'ab12' }, {}), false);

// the Workstation window's bar: no network while the Gateway is down, with Start Gateway
const L2 = (gw, ws) => list(gw, ws);
const wsOf = (l) => l.find((m) => m.role === 'workstation'), gwOfL = (l) => gatewayOf(wsOf(l), l);
eq('bar: the gateway off', gatewayBar(wsOf(L2('stopped', 'running')), gwOfL(L2('stopped', 'running'))), 'off');
eq('bar: the gateway crashed', gatewayBar(wsOf(L2('crashed', 'running')), gwOfL(L2('crashed', 'running'))), 'off');
eq('bar: the gateway starting', gatewayBar(wsOf(L2('starting', 'running')), gwOfL(L2('starting', 'running'))), 'starting');
eq('bar: both running', gatewayBar(wsOf(L2('running', 'running')), gwOfL(L2('running', 'running'))), null);
eq('bar: the workstation off', gatewayBar(wsOf(L2('stopped', 'stopped')), gwOfL(L2('stopped', 'stopped'))), null);
eq('bar: the gateway shutting down', gatewayBar(wsOf(L2('stopping', 'running')), gwOfL(L2('stopping', 'running'))), 'off');
eq('bar: a machine of no group', gatewayBar(deb, null), null);
eq('bar: the gateway is the same group\'s', gatewayOf(wsOf(L2('running', 'running')), [...L2('running', 'running').filter((m) => m.role !== 'gateway'), { ...mem('gateway', 'running'), groupDir: '/x.lordgroup' }]), null);

// the rename dialog's line
eq('rename: a free name', renameProblemKey({ problem: '' }, 'Tor', 'Whonix'), '');
eq('rename: taken', renameProblemKey({ problem: 'name-taken' }, 'Tor', 'Whonix'), 'machines.group.rename.taken');
eq('rename: folder taken', renameProblemKey({ problem: 'folder-taken' }, 'Tor', 'Whonix'), 'machines.group.rename.folderTaken');
eq('rename: a bad name', renameProblemKey({ problem: 'bad-name' }, '.x', 'Whonix'), 'machines.new.err.nameBad');
eq('rename: the same name says nothing', renameProblemKey({ problem: 'name-taken' }, 'Whonix', 'Whonix'), '');

// Destroy and start fresh: the workstation by default, the gateway with the box (Tor's guards)
const snaps = (m) => (m?.role === 'gateway' ? 2 : 1);
eq('fresh: the workstation only', freshPlan(it.groups[0], false, snaps), { members: ['workstation'], snapshots: 1, guards: false });
eq('fresh: also the gateway', freshPlan(it.groups[0], true, snaps), { members: ['workstation', 'gateway'], snapshots: 3, guards: true });

// the group's budget: both members with QEMU's overhead, the disposable session's cap with the workstation
const host = { memMaxMb: 6000 };
eq('the session cap: the workstation\'s memory, at least 1 GB', [sessionCapMb({ memoryMb: 2048 }), sessionCapMb({ memoryMb: 512 })], [2048, 1024]);
eq('budget: both members', groupBudget(group, list('stopped', 'stopped'), host), { ok: true, need: 2 * (2048 + 384), free: 6000 });
eq('budget: with a disposable session', groupBudget({ ...group, disposable: true }, list('stopped', 'stopped'), host), { ok: false, need: 2 * (2048 + 384) + 2048, free: 6000 });
eq('budget: another machine running counts', groupBudget(group, [mem('gateway', 'stopped'), mem('workstation', 'stopped'), { ...deb, state: 'running' }], host).free, 6000 - 4096 - 384);
eq('budget: the gateway already running needs only the workstation', groupBudget(group, list('running', 'stopped'), host).need, 2048 + 384);
eq('budget: a running disposable session counts its cap', groupBudget(group, list('stopped', 'running', {}, { disposable: true }), host).free, 6000 - (2048 + 384) - 2048);

// chips and search
eq('chips', [groupShows(it.groups[0], { show: 'all' }), groupShows(it.groups[0], { show: 'running' }), groupShows(on, { show: 'running' }), groupShows(it.groups[0], { show: 'linux' }), groupShows(it.groups[0], { show: 'windows' })],
  [true, false, true, true, false]);
eq('search by name or Tor', [groupShows(it.groups[0], { query: 'whon' }), groupShows(it.groups[0], { query: 'tor' }), groupShows(it.groups[0], { query: 'debian' })], [true, true, false]);
eq('the locked rows Configure shows', LOCKED_ROWS.length >= 8 && LOCKED_ROWS.includes('clipboard') && LOCKED_ROWS.includes('agent'), true);

// the catalog's group card
const CAT = JSON.parse(readFileSync(new URL('../../assets/machines/systems.json', import.meta.url)));
const card = (o) => systemsFor(CAT, 'amd64', o).find((s) => s.id === 'whonix');
eq('the Whonix card with KVM on amd64', [card({ kvm: true, hostArch: 'amd64' })?.kind, card({ kvm: true, hostArch: 'amd64' })?.members.map((m) => m.role)], ['group', ['gateway', 'workstation']]);
eq('no Whonix card without KVM', card({ kvm: false, hostArch: 'amd64' }), undefined);
eq('no Whonix card emulated on another architecture', card({ kvm: true, hostArch: 'arm64' }), undefined);
eq('no Whonix card on arm64', systemsFor(CAT, 'arm64', { kvm: true, hostArch: 'arm64' }).some((s) => s.id === 'whonix'), false);
eq('the members\' machines', groupMachines(CAT.systems.find((s) => s.id === 'whonix')), [{ role: 'gateway', vcpus: 1, memoryMb: 2048 }, { role: 'workstation', vcpus: 1, memoryMb: 2048 }]);
// the releases' folder listing (download.whonix.org/libvirt/): names only, the newest not under the floor
const html = '<a href="../">../</a><a href="18.2.1.9/">18.2.1.9/</a><a href="18.2.4.3/">18.2.4.3/</a><a href="18.2.10.1/">18.2.10.1/</a><a href="evil/">evil/</a><a href="18.2.4.3.tar">x</a>';
const pat = '^[0-9]+(\\.[0-9]+){3}$';
eq('release folders', releaseNames(html, pat), ['18.2.1.9', '18.2.4.3', '18.2.10.1']);
eq('newest by numbers', newestRelease(releaseNames(html, pat), '18.2.4.3'), '18.2.10.1');
eq('never below the floor', newestRelease(['18.2.1.9'], '18.2.4.3'), null);
eq('release order', [releaseCmp('18.2.4.3', '18.2.10.1') < 0, releaseCmp('18.2.4.3', '18.2.4.3')], [true, 0]);
// a SHA-512 list: only 128-digit lines for a SHA-512 entry
const e = entryOf(CAT, 'whonix', 'amd64');
const h512 = 'ab'.repeat(64), h256 = 'cd'.repeat(32);
const sums = `${h512}  Whonix-LXQt-18.2.4.3.Intel_AMD64.qcow2.libvirt.xz\n${h256}  Whonix-LXQt-18.2.4.4.Intel_AMD64.qcow2.libvirt.xz\n`;
eq('the entry says SHA-512', e.hash, 'sha512');
eq('pick: the 128-digit line', pickImage(e, sums), { name: 'Whonix-LXQt-18.2.4.3.Intel_AMD64.qcow2.libvirt.xz', sha: h512, size: 0 });
eq('a SHA-256 entry takes only the 64-digit line', pickImage({ ...entryOf(CAT, 'debian', 'amd64'), file: '^Whonix' }, sums)?.sha, h256);
// the archive in Storage: its groups, and whether it can go (both bases there)
const sha = 'ef'.repeat(64);
const imgs = downloadedImages(CAT, [{ name: `${sha}.tar.xz`, size: 1957016760 }, { name: `${'01'.repeat(64)}.tar.xz.dl.part`, size: 5 }], () => ({ id: 'whonix', file: 'Whonix-LXQt-18.2.4.3.Intel_AMD64.qcow2.libvirt.xz' }), [], undefined,
  [{ name: 'Whonix', archiveSha512: sha, basesThere: true }]);
eq('the archive is listed with its group', imgs.map((x) => [x.archive, x.partial, x.users, x.basesThere]), [[true, false, ['Whonix'], true], [true, true, [], false]]);

console.log(`vm_group_ui_test: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
