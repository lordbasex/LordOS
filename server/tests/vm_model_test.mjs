// vm_model_test.mjs — Machines' pure logic (apps/machines/model.js): state mapping, allowed actions, the New machine
// sheet's checks, letterbox and pointer scaling, the guest size on resize, the refresh rate and wheel steps.
//
//   node tests/vm_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const M = await import(path.join(here, '../../apps/machines/model.js'));
const D = await import(path.join(here, '../../apps/machines/diag.js'));
const { stateView, actionsFor, defaultsFor, validateDraft, createRequest, missingFor, letterbox, toPicture, guestSize, refreshRate, wheelSteps, windowTitle, folderOf, memLimit, budgetCheck, systemOf, versionOf, archOf, suggestName, profileSize, usage, summaryOf, doingOf, thumbSrc, closeChoice, snapName, snapSize, cloneMethod, storageRows, storageBar, profileOf, resolutions, agentLines, agentTone, clipTo, clipFrom, soundOf, audible, soundSummary, muteToggle, micLive, micAsk } = M;

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

// ---- states ----
eq('running', stateView('running', 'none'), { key: 'running', tone: 'run', active: true, why: null });
eq('paused', stateView('paused', 'user').key, 'paused');
eq('crashed keeps why', stateView('crashed', 'out-of-memory'), { key: 'crashed', tone: 'bad', active: false, why: 'out-of-memory' });
eq('crashed no why', stateView('crashed', 'none').why, 'qemu-failed');
eq('stopped clean', stateView('stopped', 'guest-shutdown'), { key: 'stopped', tone: 'off', active: false, why: null });
eq('stopped after forced', stateView('stopped', 'forced').why, 'forced');
eq('start failed', stateView('stopped', 'start-failed').why, 'start-failed');
eq('supervisor lost → recovered', stateView('stopped', 'supervisor-lost'), { key: 'recovered', tone: 'bad', active: false, why: 'supervisor-lost' });
eq('unknown → stopped', stateView(undefined).key, 'stopped');
eq('stopping busy', stateView('stopping').tone, 'busy');

// ---- actions ----
eq('stopped: start, delete', [actionsFor('stopped').start, actionsFor('stopped').remove, actionsFor('stopped').shutdown], [true, true, false]);
eq('running: no start, no delete', [actionsFor('running').start, actionsFor('running').remove, actionsFor('running').shutdown, actionsFor('running').pause], [false, false, true, true]);
eq('paused: resume, not pause', [actionsFor('paused').resume, actionsFor('paused').pause, actionsFor('paused').shutdown, actionsFor('paused').poweroff], [true, false, false, true]);
eq('crashed: start again', actionsFor('crashed').start, true);
eq('starting: force off only', [actionsFor('starting').poweroff, actionsFor('starting').shutdown, actionsFor('starting').remove], [true, false, false]);

// ---- defaults and validation ----
const pi = { arch: 'arm64', cpus: 4, vcpusRecommended: 3, memMaxMb: 14336, kvm: true };
const pc = { arch: 'amd64', cpus: 4, vcpusRecommended: 3, memMaxMb: 1024, kvm: true };
eq('Pi defaults', defaultsFor(pi), { name: '', arch: 'arm64', vcpus: 3, memoryMb: 2048, diskGb: 20, source: 'import', path: '', rootPassword: '' });
eq('small host memory', defaultsFor(pc).memoryMb, 1024);
eq('tiny host memory', defaultsFor({ arch: 'amd64', memMaxMb: 700 }).memoryMb, 700);
const good = { name: 'Debian 13', arch: 'amd64', vcpus: 2, memoryMb: 1024, diskGb: 20, source: 'import', path: '/home/a/debian.qcow2' };
eq('good draft', validateDraft(good, pc), { ok: true, errors: {}, warnings: [], emulated: false });
eq('empty name', validateDraft({ ...good, name: '  ' }, pc).errors.name, 'nameEmpty');
eq('long name (bytes)', validateDraft({ ...good, name: 'é'.repeat(33) }, pc).errors.name, 'nameBad');
eq('control char', validateDraft({ ...good, name: 'a\tb' }, pc).errors.name, 'nameBad');
eq('taken name', validateDraft(good, pc, ['debian 13']).errors.name, 'nameTaken');
eq('too many vCPUs', validateDraft({ ...good, vcpus: 5 }, pc).errors.vcpus, 'vcpus');
eq('above recommended warns', validateDraft({ ...good, vcpus: 4 }, pc).warnings, ['vcpus']);
eq('zero vCPUs', validateDraft({ ...good, vcpus: 0 }, pc).errors.vcpus, 'vcpus');
eq('memory over host', validateDraft({ ...good, memoryMb: 2048 }, pc).errors.memoryMb, 'memory');
eq('memory under min', validateDraft({ ...good, memoryMb: 128 }, pc).errors.memoryMb, 'memory');
eq('disk 0', validateDraft({ ...good, diskGb: 0 }, pc).errors.diskGb, 'disk');
eq('no image', validateDraft({ ...good, path: '' }, pc).errors.path, 'imageMissing');
eq('iso missing', validateDraft({ ...good, source: 'iso', path: '' }, pc).errors.path, 'isoMissing');
eq('wrong image kind', validateDraft({ ...good, path: '/a/b.vmdk' }, pc).errors.path, 'imageKind');
eq('iso kind', validateDraft({ ...good, source: 'iso', path: '/a/b.qcow2' }, pc).errors.path, 'isoKind');
eq('iso ok', validateDraft({ ...good, source: 'iso', path: '/a/debian.ISO' }, pc).ok, true);
eq('emulated warns', validateDraft({ ...good, arch: 'arm64' }, pc), { ok: true, errors: {}, warnings: ['emulated'], emulated: true });
eq('no kvm warns', validateDraft(good, { ...pc, kvm: false }).warnings, ['noKvm']);
eq('bad arch', validateDraft({ ...good, arch: 'riscv64' }, pc).errors.arch, 'arch');
eq('request import', createRequest(good), { name: 'Debian 13', arch: 'amd64', vcpus: 2, memoryMb: 1024, diskGb: 20, network: 'shared', import: '/home/a/debian.qcow2' });
eq('request root password', createRequest({ ...good, rootPassword: 'x' }).rootPassword, 'x');
eq('no root password for an ISO', createRequest({ ...good, source: 'iso', path: '/a.iso', rootPassword: 'x' }).rootPassword, undefined);
eq('request iso', createRequest({ ...good, name: ' X ', source: 'iso', path: '/a.iso' }).iso, '/a.iso');
eq('missing packages', missingFor({ missing: { amd64: 'qemu-system-x86 ovmf qemu-utils', arm64: 'qemu-system-arm qemu-utils' } }, ['amd64', 'arm64']),
  ['qemu-system-x86', 'ovmf', 'qemu-utils', 'qemu-system-arm']);
eq('nothing missing', missingFor({ missing: { amd64: '' } }, ['amd64']), []);
eq('Windows by its labels and file names', [systemOf('CCCOMA_X64FRE_ES-ES_DV9').id, systemOf('CENA_X64FREE_ES-ES_DV9').id, systemOf('', 'Win11_26H2_Spanish_x64.iso').id,
  systemOf('', 'windows-notes.iso').id], ['windows', 'windows', 'windows', 'other']);
// the TPM (swtpm) only for Windows machines and the requirements page
eq('swtpm only when a Windows machine asks', [missingFor({ missing: { amd64: 'swtpm passt' } }, ['amd64']), missingFor({ missing: { amd64: 'swtpm passt' } }, ['amd64'], { windows: true })],
  [['passt'], ['swtpm', 'passt']]);

// ---- picture ----
const r2 = (o) => Object.fromEntries(Object.entries(o).map(([k, v]) => [k, Math.round(v * 100) / 100]));
eq('letterbox wide box', r2(letterbox([0, 0, 1000, 500], 800, 600)), { x: 166.67, y: 0, w: 666.67, h: 500, k: 0.83 });
eq('letterbox tall box', letterbox([10, 20, 400, 600], 800, 600), { x: 10, y: 170, w: 400, h: 300, k: 0.5 });
eq('letterbox exact', letterbox([0, 0, 1024, 768], 1024, 768).k, 1);
eq('letterbox empty', letterbox([0, 0, 100, 100], 0, 0).k, 0);
eq('point center', toPicture(210, 320, [10, 20, 400, 600], 800, 600), [800 / 2, 600 / 2]);
eq('point in band clamps', toPicture(100, 25, [10, 20, 400, 600], 800, 600), [180, 0]);
eq('point past end', toPicture(9999, 9999, [10, 20, 400, 600], 800, 600), [799, 599]);
eq('point no picture', toPicture(1, 1, [0, 0, 10, 10], 0, 0), null);
eq('guest size even', guestSize([0, 0, 1023.6, 701.2], null), [1022, 700]);
eq('guest size min', guestSize([0, 0, 200, 100], null), [320, 200]);
eq('guest size unchanged', guestSize([0, 0, 1024, 700], [1022, 700]), null);
eq('guest size changed', guestSize([0, 0, 1280, 700], [1022, 700]), [1280, 700]);
eq('rate Hz', refreshRate([{ mode: { refresh: 60 } }, { mode: { refresh: 144 } }]), 144);
eq('rate mHz', refreshRate([{ mode: { refresh: 59950 } }]), 60);
eq('rate none', refreshRate(null), 60);
eq('rate absurd', refreshRate([{ mode: { refresh: 500 } }]), 60);
eq('wheel notch up', wheelSteps(0, -120, false), [1, 0]);
eq('wheel notch down', wheelSteps(0, 1, false), [-1, 0]);
eq('wheel precise adds up', wheelSteps(0, -30, true), [0, 30]);
eq('wheel precise step', wheelSteps(30, -30, true), [1, 20]);
eq('wheel precise clamp', wheelSteps(0, 100000, true)[0], -32);
eq('title', windowTitle('Debian', 'Running'), 'Debian — Running');
eq('folder', folderOf('/home/a/Machines/Debian 13.lordvm'), 'Debian 13');
// the memory budget at start: QEMU's overhead (384 MB with KVM, 640 MB emulated) counts for every machine
const pi8 = { memMaxMb: 6144 };
eq('limit kvm', memLimit({ memoryMb: 1024, accel: 'kvm' }), 1408);
eq('limit tcg', memLimit({ memoryMb: 1024, accel: 'tcg' }), 1664);
const run = (dir, memoryMb, accel) => ({ dir, memoryMb, accel, state: 'running' });
eq('alone always', budgetCheck([], { dir: 'a', memoryMb: 6144, accel: 'tcg' }, pi8).ok, true);
eq('beside one fits', budgetCheck([run('b', 2048, 'kvm')], { dir: 'a', memoryMb: 2048, accel: 'tcg' }, pi8), { ok: true, need: 2688, free: 3712 });
eq('beside one, too big emulated', budgetCheck([run('b', 2048, 'kvm')], { dir: 'a', memoryMb: 3200, accel: 'tcg' }, pi8).ok, false);
eq('same with kvm fits', budgetCheck([run('b', 2048, 'kvm')], { dir: 'a', memoryMb: 3200, accel: 'kvm' }, pi8).ok, true);
eq('stopped ones do not count', budgetCheck([{ dir: 'b', memoryMb: 4096, accel: 'kvm', state: 'stopped' }], { dir: 'a', memoryMb: 4096, accel: 'kvm' }, pi8).ok, true);
eq('itself does not count', budgetCheck([run('a', 4096, 'kvm')], { dir: 'a', memoryMb: 4096, accel: 'kvm' }, pi8).ok, true);

// the Control Center and the assistant
eq('system from label', systemOf('Debian 13.1.0 amd64 n').id, 'debian');
eq('system from file', systemOf('', 'ubuntu-24.04-desktop-arm64.iso'), { id: 'ubuntu', name: 'Ubuntu' });
eq('system unknown', systemOf('MYDISK').id, 'other');
eq('version', versionOf('debian-13.1.0-amd64-netinst.iso'), '13.1');
eq('version none', versionOf('netinst.iso'), '');
eq('arch arm', archOf('debian-13-nocloud-arm64.qcow2'), 'arm64');
eq('arch x86', archOf('Fedora-Server-dvd-x86_64-41.iso'), 'amd64');
eq('arch unknown', archOf('mysystem.iso'), '');
eq('name', suggestName({ name: 'Debian' }, '13', ['Debian 13']), 'Debian 13 2');
// 5.6 on a Pi 5 (4 cores, 16 GB) and on a small VM (4 cores, 3.8 GB)
const pi5 = { cpus: 4, vcpusRecommended: 3, ramMb: 16219, memMaxMb: 14171 }, small = { cpus: 4, vcpusRecommended: 3, ramMb: 3946, memMaxMb: 2922 };
eq('profile productivity pi', profileSize('productivity', pi5), { vcpus: 2, memoryMb: 5376, diskGb: 32, network: 'shared', gfx: '2d' });
eq('profile development pi', profileSize('development', pi5), { vcpus: 3, memoryMb: 7936, diskGb: 64, network: 'shared', gfx: '2d' });
eq('profile testing isolated', profileSize('testing', pi5), { vcpus: 2, memoryMb: 4096, diskGb: 32, network: 'isolated', gfx: '2d' });
eq('profile design 3d', profileSize('design', pi5).gfx, '3d');
eq('profile light pi', profileSize('light', pi5), { vcpus: 2, memoryMb: 3072, diskGb: 24, network: 'shared', gfx: '2d' });
eq('profile dev small host', profileSize('development', small), { vcpus: 3, memoryMb: 1792, diskGb: 64, network: 'shared', gfx: '2d' });
eq('profile testing small host', profileSize('testing', small).memoryMb, 2816);
eq('profile of', profileOf({ vcpus: 2, memoryMb: 4096, network: 'isolated' }, pi5, 'testing'), 'testing');
eq('profile changed by hand', profileOf({ vcpus: 3, memoryMb: 4096, network: 'isolated' }, pi5, 'testing'), '');
eq('resolutions fit', resolutions(1280, 800, [1022, 598]), [[1022, 598], [1024, 768], [1280, 720], [1280, 800]]);
eq('resolutions big', resolutions(1920, 1080, [1920, 1080]).length, 8);
eq('usage first sample', usage(null, { at: 1000, cpuUsec: 5, memBytes: 9, ioBytes: 1 }, 2), { memBytes: 9, cpu: null, disk: false });
eq('usage 50 %', usage({ at: 1000, cpuUsec: 0, ioBytes: 5 }, { at: 2000, cpuUsec: 1000000, memBytes: 1, ioBytes: 9 }, 2), { memBytes: 1, cpu: 50, disk: true });
eq('usage off', usage(null, { diskBytes: 5 }, 2), null);
const sl = [{ dir: 'a', state: 'running' }, { dir: 'b', state: 'stopped' }];
eq('summary', summaryOf(sl, { a: { memBytes: 1073741824 } }, { ramMb: 4096, libFreeBytes: 50, libTotalBytes: 100 }),
  { running: { a: 1, b: 2, frac: 0.5 }, memory: { a: 1073741824, b: 4294967296, frac: 0.25 }, free: { a: 50, b: 100, frac: 0.5 } });
eq('summary without numbers', summaryOf([], {}, {}).memory, null);
eq('doing shut down', doingOf({ state: 'stopped', reason: 'guest-shutdown' }), 'shutDown');
eq('doing failed', doingOf({ state: 'crashed', reason: 'start-failed' }), 'crashed');
eq('thumb', thumbSrc('/h/M/a.lordvm', 1700000000.5), '/h/M/a.lordvm/thumb.png#1700000000');
eq('no thumb', thumbSrc('/h/M/a.lordvm', 0), '');

// ---- Beta: suspend, Keep Running, closing ----
eq('suspended state', stateView('stopped', 'suspended'), { key: 'suspended', tone: 'pause', active: false, why: null });
eq('resume failed', stateView('crashed', 'resume-failed').why, 'resume-failed');
eq('suspend allowed running', actionsFor('running').suspend, true);
eq('suspend allowed paused', actionsFor('paused').suspend, true);
eq('suspend not off', actionsFor('stopped').suspend, false);
eq('doing suspended', doingOf({ state: 'stopped', reason: 'poweroff', suspended: true }), 'suspended');
eq('doing no window', doingOf({ state: 'running' }, false), 'noWindow');
eq('doing windowed', doingOf({ state: 'running' }, true), 'running');
eq('close default', closeChoice({}), 'ask');
eq('close keep', closeChoice({ onClose: 'keep' }), 'keep');
eq('close bad', closeChoice({ onClose: 'reboot' }), 'ask');

eq('snap name first', snapName([], 'Snapshot'), 'Snapshot 1');
eq('snap name next free', snapName([{ name: 'Snapshot 2' }], 'Snapshot'), 'Snapshot 3');
eq('snap name taken', snapName([{ name: 'snapshot 2' }, { name: 'x' }], 'Snapshot'), 'Snapshot 3');
eq('snap size memory', snapSize({ memory: true, vmstate: 5 }), 5);
eq('snap size disk', snapSize({ memory: false, vmstate: 0 }), 0);

eq('clone reflink', cloneMethod({ reflink: true }, 'clone'), 'reflink');
eq('clone linked on ext4', cloneMethod({ reflink: false }, 'clone'), 'linked');
eq('clone with snapshots copies', cloneMethod({ snapshots: 2 }, 'clone'), 'copy');
eq('clone of a linked clone copies', cloneMethod({ linked: true }, 'clone'), 'copy');
eq('clone suspended blocked', cloneMethod({ suspended: true }, 'clone'), 'blocked');
eq('duplicate suspended ok', cloneMethod({ suspended: true }, 'duplicate'), 'duplicate');
eq('from snapshot copies', cloneMethod({ reflink: true }, 'clone', true), 'copy');
const st = { total: 1000, free: 400, isos: 50, trash: 20, machines: [
  { dir: '/a', id: 'A', name: 'Debian Dev', total: 100, frees: 10, base: '/l/.shared/b.qcow2' },
  { dir: '/b', id: 'B', name: 'Debian Dev clone', total: 30, frees: 30, base: '/l/.shared/b.qcow2', meta: { cloneOf: 'A' } },
  { dir: '/c', id: 'C', name: 'Alpha', total: 200, frees: 200, base: '' },
], bases: [{ path: '/l/.shared/b.qcow2', bytes: 300, users: 2, trash: 0 }] };
eq('storage rows order', storageRows(st).map((r) => [r.name || 'base', r.level]), [['Alpha', 0], ['Debian Dev', 0], ['Debian Dev clone', 1], ['base', 0]]);
eq('storage row base users', storageRows(st)[1].baseUsers, 2);
eq('storage bar', storageBar(st).map((b) => [b.key, b.bytes]), [['machines', 630], ['isos', 50], ['trash', 20]]);
eq('storage bar: empty slices hidden', storageBar({ total: 1000, free: 400, machines: [{ total: 600 }], bases: [], isos: 0, trash: 0 }).map((b) => b.key), ['machines']);
eq('storage bar no total', storageBar({}), []);

// ---- the guest agent ----
eq('agent off lines', agentLines(null, {}).find((l) => l[0] === 'clipboard')[1], 'off');
eq('agent connected', agentLines({ state: 'connected', caps: 1 | 2 | 16 | 32 }, { clipboard: 'both' }).filter((l) => l[1] === 'works').map((l) => l[0]), ['shell', 'logs', 'shutdown', 'time', 'clipboard', 'resolution']);
eq('agent helper missing', agentLines({ state: 'connected', caps: 1 }, { clipboard: 'to-guest' }).find((l) => l[0] === 'clipboard')[1], 'needsHelper');
eq('windows guest without the tools', agentLines({ state: 'connected', caps: 1 | 4 | 8, system: 'Windows 11 Pro' }, { clipboard: 'both' }).filter((l) => ['clipboard', 'resolution'].includes(l[0])).map((l) => l[1]), ['needsTools', 'needsTools']);
eq('windows guest with the tools', agentLines({ state: 'connected', caps: 1 | 4 | 8 | 32 | 128 | 256, system: 'Windows 11 Pro' }, { clipboard: 'both' }).filter((l) => ['clipboard', 'resolution'].includes(l[0])).map((l) => l[1]), ['works', 'works']);
eq('linux guest resolution', agentLines({ state: 'connected', caps: 1, system: 'Ubuntu 26.04' }, {}).find((l) => l[0] === 'resolution')[1], 'works');
eq('agent tone', [agentTone(null, false), agentTone({ state: 'waiting' }, true), agentTone({ state: 'connected' }, true)], ['off', 'pause', 'run']);
eq('clip ways', [clipTo('both'), clipTo('from-guest'), clipFrom('from-guest'), clipFrom('off')], [true, false, true, false]);

// ---- unified diagnostics (diag.js) ----
const LOG = ['2026-11-05T06:00:00.000Z lord-vm 1: machine old', '2026-11-05T06:00:01.000Z qemu: SIGSEGV failed',
  '2026-11-05T06:11:31.745Z lord-vm 9: machine 3bf "x" (amd64, kvm, 1 vCPU, 512 MB, pc-q35-10.0)',
  '2026-11-05T06:11:31.811Z recovery: an interrupted suspend was rolled back',
  '2026-11-05T06:11:32.000Z cgroup: in app-lordos-lordos-vm-3bf.scope (memory.max 1024 MB, cpu 150%)',
  '2026-11-05T06:11:32.100Z qemu: pid 5; sandbox: landlock-fs=1 landlock-net=1',
  '2026-11-05T06:11:32.200Z qemu: version 10.0.13 Debian',
  '2026-11-05T06:11:40.000Z note: the machine\'s network stopped: passt ended (signal 11)',
  '2026-11-05T06:12:00.000Z qemu: event VSERPORT_CHANGE'].join('\n');
const lg = D.readLog(LOG);
eq('diag last run only', lg.problems.length, 0);
eq('diag recovery', lg.recovery.map((r) => r.text), ['an interrupted suspend was rolled back']);
eq('diag fields', [lg.version, lg.sandbox, lg.cgroup.slice(0, 6), lg.started], ['10.0.13 Debian', 'landlock-fs=1 landlock-net=1', 'in app', '2026-11-05 06:11:31']);
eq('diag network', lg.network.length, 1);
eq('diag report', D.reportText({ name: 'X', id: 'i', arch: 'amd64', accel: 'kvm', vcpus: 1, memoryMb: 512, diskGb: 20, firmware: 'uefi', network: 'shared', state: 'running', reason: 'none', suspended: false }, lg, { snapshots: 2 }).split('\n').length, 10);

// ---- a Clone's name inside the guest (6.3), as lord-vm checks it ----
eq('hostname ok', [M.hostnameValid('debian-13-clone'), M.hostnameValid('a'), M.hostnameValid('-a'), M.hostnameValid('a-'), M.hostnameValid('A'), M.hostnameValid('a.b'), M.hostnameValid('x'.repeat(64))],
  [true, true, false, false, false, false, false]);
eq('hostname from name', [M.hostnameFrom('Debian 13 (test) '), M.hostnameFrom('Ñandú'), M.hostnameFrom('--')], ['debian-13-test', 'and', 'machine']);
eq('clone hostname', [M.cloneHostname('Agent Test'), M.cloneHostname('x'.repeat(80)).length, M.hostnameValid(M.cloneHostname('x'.repeat(80)))], ['agent-test-clone', 63, true]);
// ---- the network healing itself, in the Overview (6.5) ----
const NET = ['2026-11-05T06:11:31.745Z lord-vm 9: machine 3bf "x" (amd64, kvm, 1 vCPU, 512 MB, pc-q35-10.0)',
  '2026-11-05T06:12:00.000Z network: passt ended (signal 9): reconnecting',
  '2026-11-05T06:12:00.100Z network: passt 77 started (backend pst1)',
  '2026-11-05T06:12:00.200Z network: the network is back (passt restarted, 412 ms after it ended)',
  '2026-11-05T06:13:00.000Z network: passt ended (status 0) while the machine stops'].join('\n');
eq('diag network healed', D.readLog(NET).network.map((x) => x.text.slice(0, 20)), ['network: passt ended', 'network: the network']);
eq('diag network not on stop', D.readLog(NET).problems.length, 0);
// ---- the import plan of another app's machine (6.22, migrate.jsx) ----
const PLAN = '2026-11-05T10:00:00.000Z import: something\n{"kind":"vmx","name":"Work","blockers":[{"code":"saved-state"}],"notes":[{"code":"license"},{"code":"tpm"},{"code":"snapshots-flattened"},{"code":"zzz"}]}';
const pl = M.parsePlan(PLAN);
eq('plan parsed', [pl.kind, pl.name], ['vmx', 'Work']);
eq('plan none', M.parsePlan('lord-vm: nope\n{"progress":1}'), null);
eq('plan notes in order, unknown ones dropped', M.planNotes(pl), ['snapshots-flattened', 'tpm']);
eq('plan blockers', M.planBlockers(pl), [{ code: 'saved-state', text: '' }]);
const pr = M.importProgress('{"progress":12.5,"disk":1,"disks":2}\n2026 import: disk 2\n{"progress":61,"disk":2,"disks":2}\n{"progr');
eq('import progress', [pr.pct, pr.disk, pr.disks, pr.done], [61, 2, 2, null]);
eq('import done', M.importProgress('{"progress":100,"disk":1,"disks":1}\n{"imported":true,"dir":"/m/x.lordvm"}').done.dir, '/m/x.lordvm');
eq('unique names', [M.uniqueName('Work', []), M.uniqueName('Work', ['Work']), M.uniqueName('Work', ['Work', 'Work 2']), M.uniqueName('  ', [])], ['Work', 'Work 2', 'Work 3', 'Imported machine']);

// the library's toolbar: view, order, Show and search (0.8.8.7)
eq('lib view defaults', M.libView(null), { view: 'grid', sort: 'name', show: 'all' });
eq('lib view kept', M.libView({ view: 'list', sort: 'disk', show: 'off' }), { view: 'list', sort: 'disk', show: 'off' });
eq('lib view junk', M.libView({ view: 'x', sort: 1, show: 'running' }), { view: 'grid', sort: 'name', show: 'running' });
const lib = [
  { name: 'Zeta', state: 'stopped', thumbTime: 50, diskBytes: 9, arch: 'amd64' },
  { name: 'Alpha', state: 'running', thumbTime: 10, diskBytes: 1, arch: 'amd64', meta: { osName: 'Ubuntu' } },
  { name: 'Mid', state: 'stopped', suspended: true, thumbTime: 99, diskBytes: 5, arch: 'arm64' },
  { name: 'Beta', state: 'paused', thumbTime: 0, arch: 'amd64' },
];
const names = (l) => l.map((m) => m.name);
eq('sort name', names(M.sortMachines(lib, 'name')), ['Alpha', 'Beta', 'Mid', 'Zeta']);
eq('sort state', names(M.sortMachines(lib, 'state')), ['Alpha', 'Beta', 'Mid', 'Zeta']);
eq('sort last', names(M.sortMachines(lib, 'last')), ['Mid', 'Zeta', 'Alpha', 'Beta']);
eq('sort disk', names(M.sortMachines(lib, 'disk')), ['Zeta', 'Mid', 'Alpha', 'Beta']);
eq('sort keeps input', names(lib)[0], 'Zeta');
eq('show running', names(M.filterMachines(lib, { show: 'running' })), ['Alpha', 'Beta']);
eq('show off', names(M.filterMachines(lib, { show: 'off' })), ['Zeta', 'Mid']);
eq('search system', names(M.filterMachines(lib, { query: ' ubun ' })), ['Alpha']);
eq('search arch', names(M.filterMachines(lib, { query: 'ARM64' })), ['Mid']);
eq('search none', M.filterMachines(lib, { query: 'nothing' }).length, 0);

// shared folders' tags
eq('share tag', [M.shareTag('Proyectos 2026'), M.shareTag('Música'), M.shareTag('...'), M.shareTag('Work', ['work', 'Work-2'])], ['Proyectos-2026', 'Musica', 'share', 'Work-3']);
eq('share tag length', M.shareTag('a'.repeat(60)).length, 28);
eq('mount command', M.mountCommand('Work'), 'sudo mkdir -p /mnt/Work && sudo mount -t virtiofs Work /mnt/Work');

// 3D graphics (6.10): Linux machines where the computer can give them; the request names them only when chosen
eq('3D asked', createRequest({ ...good, graphics: '3d' }).graphics, '3d');
eq('2D not named', createRequest({ ...good, graphics: '2d' }).graphics, undefined);
eq('3D for Linux here', M.gfxFor('3d', { no3d: '' }, false), '3d');
eq('never for the windows devices', [M.gfxFor('3d', { no3d: '' }, true), M.no3dReason({ no3d: '' }, true)], ['2d', 'windows']);
eq('2D without a GPU path', [M.gfxFor('3d', { no3d: 'gpu' }, false), M.no3dReason({ no3d: 'gpu' }, false)], ['2d', 'gpu']);
eq('2D profile stays 2D', M.gfxFor('2d', { no3d: '' }, false), '2d');
eq('arm64 machines stay 2D', [M.gfxFor('3d', { no3d: '' }, false, 'arm64'), M.no3dReason({ no3d: '' }, false, 'arm64')], ['2d', 'arch']);
eq('Design profile asks for 3D', profileSize('design', { cpus: 8, ramMb: 16384, memMaxMb: 14336, vcpusRecommended: 7 }).gfx, '3d');
eq('a running 3D machine', [M.runs3d({ sandbox: 511 | 512 }), M.runs3d({ sandbox: 511 }), M.runs3d(null)], [true, false, false]);

// ---- the network (6.5): modes, forwarded ports, the bridged card ----
eq('net modes', M.NET_MODES, ['shared', 'bridged', 'hostonly', 'isolated']);
eq('ports in shared and host-only only', ['shared', 'bridged', 'hostonly', 'isolated'].map(M.usesPorts), [true, false, true, false]);
const ssh = { proto: 'tcp', host: 2222, guest: 22, open: 'this-computer' };
eq('a good rule', M.portProblems([ssh], 'shared'), []);
eq('lan in shared', M.portProblems([{ ...ssh, open: 'lan' }], 'shared'), []);
eq('lan in host-only', M.portProblems([{ ...ssh, open: 'lan' }], 'hostonly'), [{ i: 0, code: 'lanHostOnly' }]);
eq('low host port', M.portProblems([{ ...ssh, host: 80 }, { ...ssh, host: 1023 }, { ...ssh, host: 1024 }], 'shared'), [{ i: 0, code: 'host' }, { i: 1, code: 'host' }]);
eq('host port range', M.portProblems([{ ...ssh, host: 65536 }, { ...ssh, host: 0 }], 'shared').map((x) => x.code), ['host', 'host']);
eq('guest port range', M.portProblems([{ ...ssh, guest: 0 }, { ...ssh, host: 2223, guest: 65536 }], 'shared').map((x) => x.code), ['guest', 'guest']);
eq('duplicate per protocol', M.portProblems([ssh, { ...ssh, guest: 23 }, { ...ssh, proto: 'udp' }], 'shared'), [{ i: 1, code: 'duplicate' }]);
eq('too many', M.portProblems(Array.from({ length: 33 }, (_, i) => ({ ...ssh, host: 3000 + i })), 'shared'), [{ i: -1, code: 'tooMany' }]);
eq('32 is fine', M.portProblems(Array.from({ length: 32 }, (_, i) => ({ ...ssh, host: 3000 + i })), 'shared'), []);
eq('a rule as set takes it', M.portRule({ proto: 'x', host: '8080', guest: 80, open: 'world' }), { proto: 'tcp', host: 8080, guest: 80, open: 'this-computer' });
eq('new rule: ssh on 2222', M.newPortRule([]), ssh);
eq('new rule: next free', M.newPortRule([ssh, { ...ssh, host: 2223 }, { ...ssh, proto: 'udp', host: 2224 }]).host, 2224);
eq('host-only closes lan', M.portsForMode([{ ...ssh, open: 'lan' }], 'hostonly'), [ssh]);
eq('shared keeps lan', M.portsForMode([{ ...ssh, open: 'lan' }], 'shared')[0].open, 'lan');
eq('port input', [M.portInput('22a2'), M.portInput(''), M.portInput('1234567'), M.portInput('-80')], [222, 0, 12345, 80]);
const cards = [{ name: 'wlan0', kind: 'wifi', up: true }, { name: 'enp4s0', kind: 'wired', up: false }, { name: 'enp3s0', kind: 'wired', up: true }];
eq('card: the one it had', M.defaultCard(cards, 'enp4s0'), 'enp4s0');
eq('card: first wired up', M.defaultCard(cards, 'wlan0'), 'enp3s0');
eq('card: none', M.defaultCard([{ name: 'wlan0', kind: 'wifi', up: true }], ''), '');
eq('bridge arg', M.bridgeArg({ id: '0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b', bridge: 'enp3s0', mac: '52:54:00:12:ab:cd' }), '0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b enp3s0 52:54:00:12:ab:cd');
eq('bridge arg without a card', M.bridgeArg({ id: 'x', mac: 'y' }), '');
eq('summary', [M.netSummary({ network: 'bridged', bridge: 'enp3s0' }), M.netSummary({ network: 'shared', ports: [{ ...ssh, open: 'lan' }] }), M.netSummary({ network: 'hostonly', ports: [{ ...ssh, open: 'lan' }] }), M.netSummary({})],
  [{ mode: 'bridged', card: 'enp3s0', ports: 0, lan: false }, { mode: 'shared', card: '', ports: 1, lan: true }, { mode: 'hostonly', card: '', ports: 1, lan: false }, { mode: 'shared', card: '', ports: 0, lan: false }]);
// sound and microphone (6.9)
eq('sound of an older machine', soundOf({}), { sound: true, volume: 100, microphone: false });
eq('sound of a muted one', soundOf({ sound: false, volume: 30, microphone: true }), { sound: false, volume: 30, microphone: true });
eq('sound volume clamped', [soundOf({ volume: 140 }).volume, soundOf({ volume: -3 }).volume, soundOf({ volume: 33.6 }).volume, soundOf({ volume: 'x' }).volume], [100, 0, 34, 100]);
eq('audible', [audible(soundOf({})), audible(soundOf({ sound: false })), audible(soundOf({ volume: 0 })), audible(null)], [true, false, false, false]);
eq('summary on', soundSummary({ volume: 70, microphone: true }), { key: 'machines.cfg.sum.soundOn', volume: 70, mic: 'machines.cfg.sum.micOn' });
eq('summary off', soundSummary({ sound: false }).key, 'machines.cfg.sum.soundOff');
eq('summary zero', soundSummary({ volume: 0 }).key, 'machines.cfg.sum.soundOff');
eq('mute toggle', [muteToggle({}), muteToggle({ sound: false, volume: 40 }), muteToggle({ volume: 0 }), muteToggle({ sound: false, volume: 0 })],
  [{ sound: false }, { sound: true }, { sound: true, volume: 50 }, { sound: true, volume: 50 }]);
eq('mic live', [micLive({ mic: 'live' }), micLive({ mic: 'wait' }), micLive(undefined)], [true, false, false]);
eq('mic ask', [micAsk({ mic: 'wait' }, false), micAsk({ mic: 'wait' }, true), micAsk({ mic: 'off' }, false), micAsk(null, false)], [true, false, false, false]);
// files dropped on the window (the guest agent copies them in)
const ag = (caps, state = 'connected') => ({ state, caps });
eq('drop: off', M.dropBlocker({ state: 'stopped' }, true), 'off');
eq('drop: no state', M.dropBlocker(null, true), 'off');
eq('drop: paused', M.dropBlocker({ state: 'paused', agent: ag(12) }, true), 'paused');
eq('drop: agent off in Configure', M.dropBlocker({ state: 'running', agent: ag(12) }, false), 'agentOff');
eq('drop: agent waiting', M.dropBlocker({ state: 'running', agent: ag(0, 'waiting') }, true), 'agentWaiting');
eq('drop: no agent yet', M.dropBlocker({ state: 'running' }, true), 'agentWaiting');
eq('drop: no file writes', M.dropBlocker({ state: 'running', agent: ag(4) }, true), 'agentCant');
eq('drop: no exec', M.dropBlocker({ state: 'running', agent: ag(8) }, true), 'agentCant');
eq('drop: ok', M.dropBlocker({ state: 'running', agent: ag(1 | 4 | 8) }, true), null);
eq('copy line none', M.copyLine([{ id: 1, state: 'done', pct: 100, size: 5 }]), null);
eq('copy line by size', M.copyLine([{ id: 7, name: 'b', state: 'copying', pct: 0, size: 300 }, { id: 3, name: 'a', state: 'copying', pct: 50, size: 100 }]), { name: 'a', pct: 12, waiting: 1 });
eq('copy line empty files', M.copyLine([{ id: 1, name: 'z', state: 'copying', pct: 100, size: 0 }]), { name: 'z', pct: 99, waiting: 0 });
eq('guest folder', [M.guestFolder('/home/ana/Descargas/x (2).txt'), M.guestFolder('C:\\Users\\Ana\\Downloads\\x.txt'), M.guestFolder('x.txt'), M.guestFolder('/x')], ['/home/ana/Descargas', 'C:\\Users\\Ana\\Downloads', '', '/']);
eq('follow stuck', [M.followStuck([1920, 1080], [1784, 830], 0, 5000), M.followStuck([1920, 1080], [1784, 830], 0, 3000), M.followStuck([1920, 1080], [1918, 1080], 0, 9000), M.followStuck(null, [1, 1], 0, 9000)], [true, false, false, false]);

console.log(`vm-model: ${n} checks, ${bad} failed`);
process.exit(bad ? 1 : 0);
