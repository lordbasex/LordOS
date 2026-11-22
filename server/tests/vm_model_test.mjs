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
// the helper runs but the guest has no clipboard tool (lordos-guest's hello said "noclip": VAC_CLIP_NOTOOL 2048)
eq('linux guest without xclip', agentLines({ state: 'connected', caps: 1 | 4 | 8 | 2048, system: 'Ubuntu 26.04' }, { clipboard: 'both' }).find((l) => l[0] === 'clipboard')[1], 'needsXclip');
eq('without xclip but the clipboard off', agentLines({ state: 'connected', caps: 2048 }, { clipboard: 'off' }).find((l) => l[0] === 'clipboard')[1], 'off');
eq('clipboard works wins over noclip', M.clipState({ caps: 32 | 2048 }, { clipboard: 'both' }), 'works');
eq('clipNeedsTool: connected, on, no tool', M.clipNeedsTool({ state: 'connected', caps: 2048 }, { clipboard: 'to-guest' }), true);
eq('clipNeedsTool: not when it works', M.clipNeedsTool({ state: 'connected', caps: 32 }, { clipboard: 'both' }), false);
eq('clipNeedsTool: not while waiting', M.clipNeedsTool({ state: 'waiting', caps: 2048 }, { clipboard: 'both' }), false);
eq('clipNeedsTool: not with the clipboard off', M.clipNeedsTool({ state: 'connected', caps: 2048 }, {}), false);
eq('an old helper without noclip still says install', agentLines({ state: 'connected', caps: 1 }, { clipboard: 'both' }).find((l) => l[0] === 'clipboard')[1], 'needsHelper');
eq('linux guest resolution', agentLines({ state: 'connected', caps: 1, system: 'Ubuntu 26.04' }, {}).find((l) => l[0] === 'resolution')[1], 'works');
eq('windows guest: no journal or awake lines (systemd\'s)', agentLines({ state: 'connected', caps: 1 | 2 | 4 | 16 | 64, system: 'Windows 11 Pro' }, {}).map((l) => l[0]).filter((x) => x === 'logs' || x === 'awake'), []);
eq('linux guest keeps them', agentLines({ state: 'connected', caps: 16 | 64, system: 'Ubuntu 26.04' }, {}).filter((l) => l[0] === 'logs' || l[0] === 'awake').map((l) => l[1]), ['works', 'works']);
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
eq('lib view kept', M.libView({ view: 'list', sort: 'disk', show: 'linux' }), { view: 'list', sort: 'disk', show: 'linux' });
eq('lib view: the old Off filter reads as all', M.libView({ show: 'off' }).show, 'all');
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
// the sound's output and input (6.9, schema 8)
eq('devs of an older machine', M.soundDevs({}), { output: '', input: '' });
eq('devs from the list', M.soundDevs({ soundOut: 'alsa_output.usb', soundIn: 'alsa_input.cam' }), { output: 'alsa_output.usb', input: 'alsa_input.cam' });
eq('devs from a poll', M.soundDevs({ output: 'a', input: '' }), { output: 'a', input: '' });
eq('devs not text', M.soundDevs({ soundOut: 3, soundIn: null }), { output: '', input: '' });
const sinks = [{ name: 'alsa_output.pci', description: 'Built-in Audio Analog Stereo' }, { name: 'alsa_output.usb', description: 'USB Advanced Audio Device Analog Stereo', default: true }, { name: 'raw' }, { description: 'no name' }];
eq('choices: the default', M.soundDevChoices(sinks, ''), [
  { name: '', label: null, isDefault: true, current: 'USB Advanced Audio Device Analog Stereo', checked: true },
  { name: 'alsa_output.pci', label: 'Built-in Audio Analog Stereo', checked: false },
  { name: 'alsa_output.usb', label: 'USB Advanced Audio Device Analog Stereo', checked: false },
  { name: 'raw', label: 'raw', checked: false }]);
eq('choices: one chosen', M.soundDevChoices(sinks, 'alsa_output.pci').filter((x) => x.checked).map((x) => x.name), ['alsa_output.pci']);
eq('choices: unplugged stays, missing', M.soundDevChoices(sinks, 'alsa_output.gone').slice(-1)[0], { name: 'alsa_output.gone', label: 'alsa_output.gone', missing: true, checked: true });
eq('choices: unplugged, the default not checked', M.soundDevChoices(sinks, 'alsa_output.gone')[0].checked, false);
eq('choices: no list', M.soundDevChoices(undefined, 'x'), [{ name: '', label: null, isDefault: true, current: '', checked: false }, { name: 'x', label: 'x', missing: true, checked: true }]);
eq('chosen', [M.soundDevChosen(sinks, '').isDefault, M.soundDevChosen(sinks, 'alsa_output.usb').label, M.soundDevChosen([], 'y').missing, M.soundDevChosen(sinks, null).isDefault], [true, 'USB Advanced Audio Device Analog Stereo', true, true]);
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
eq('copy line by size', M.copyLine([{ id: 7, name: 'b', state: 'copying', pct: 0, size: 300 }, { id: 3, name: 'a', state: 'copying', pct: 50, size: 100 }]), { name: 'a', pct: 12, waiting: 1, fast: false });
eq('copy line empty files', M.copyLine([{ id: 1, name: 'z', state: 'copying', pct: 100, size: 0 }]), { name: 'z', pct: 99, waiting: 0, fast: false });

// a long name is shortened in the middle so the percentage after it stays in view (the ISO's name hid it)
eq('mid name short', M.midName('a.iso'), 'a.iso');
{ const n = M.midName('Windows11_Client_x64_es-mx_26300_9457.iso'); eq('mid name long', [Array.from(n).length <= 36, n.endsWith('9457.iso'), n.includes('…'), n.startsWith('Windows11_')], [true, true, true, true]); }
eq('mid name in the copy line', M.copyLine([{ id: 1, name: 'x'.repeat(60) + '.iso', state: 'copying', pct: 10, size: 1 }]).name.length <= 36, true);
eq('guest folder', [M.guestFolder('/home/ana/Descargas/x (2).txt'), M.guestFolder('C:\\Users\\Ana\\Downloads\\x.txt'), M.guestFolder('x.txt'), M.guestFolder('/x')], ['/home/ana/Descargas', 'C:\\Users\\Ana\\Downloads', '', '/']);
eq('follow stuck', [M.followStuck([1920, 1080], [1784, 830], 0, 5000), M.followStuck([1920, 1080], [1784, 830], 0, 3000), M.followStuck([1920, 1080], [1918, 1080], 0, 9000), M.followStuck(null, [1, 1], 0, 9000)], [true, false, false, false]);
eq('copy line through the transfer folder', M.copyLine([{ id: 2, name: 'iso', state: 'copying', pct: 40, size: 10, via: 'transfer' }]).fast, true);
// the transfer folder (6.7.1): what the window says of it, how a drop goes, WinFsp in the catalog
const T = 512, S = 1024;
const runT = (caps, extra = {}) => ({ state: 'running', agent: { state: 'connected', caps, ...extra } });
eq('transfer: off', M.transferState({ transfer: false }, runT(T)), 'off');
eq('transfer: no row', M.transferState(null, runT(T)), 'off');
eq('transfer: stopped', M.transferState({ transfer: true }, { state: 'stopped' }), 'waiting');
eq('transfer: agent waiting', M.transferState({ transfer: true }, { state: 'running', agent: { state: 'waiting', caps: 0 } }), 'waiting');
eq('transfer: mounted', M.transferState({ transfer: true }, runT(T | 12)), 'ready');
eq('transfer: paused, mounted', M.transferState({ transfer: true }, { state: 'paused', agent: { state: 'connected', caps: T } }), 'ready');
eq('transfer: needs WinFsp', M.transferState({ transfer: true }, runT(S | 12)), 'setup');
eq('transfer: setting up', M.transferState({ transfer: true }, runT(S, { transfer: 1 })), 'settingUp');
eq('transfer: not yet', M.transferState({ transfer: true }, runT(12)), 'waiting');
eq('drop way', [M.dropWay({ transfer: true }, runT(T)), M.dropWay({ transfer: true }, runT(S)), M.dropWay({ transfer: false }, runT(T)), M.dropWay({ transfer: true }, runT(0))], ['fast', 'agent', 'off', 'agent']);
eq('agent lines: the transfer folder counts as folders', agentLines({ state: 'connected', caps: T }, { transfer: true }).find((l) => l[0] === 'folders')[1], 'works');
eq('agent lines: not mounted', agentLines({ state: 'connected', caps: 0 }, { transfer: true }).find((l) => l[0] === 'folders')[1], 'none');
eq('auto setup', [M.autoTransferSetup({ transferSetup: true }, 'setup'), M.autoTransferSetup({ transferSetup: false }, 'setup'), M.autoTransferSetup({ transferSetup: true }, 'ready'), M.autoTransferSetup(null, 'setup')], [true, false, false, false]);
{
  const cat = JSON.parse((await import('node:fs')).readFileSync(path.join(here, '../../assets/machines/systems.json'), 'utf8'));
  const w = M.winfspFor(cat, 'amd64');
  eq('winfsp pinned', w && { id: w.id, name: w.name, size: w.size, sha: w.sha.length }, { id: 'winfsp', name: 'winfsp-2.1.25156.msi', size: 2191360, sha: 64 });
  eq('winfsp arm64: none yet', M.winfspFor(cat, 'arm64'), null);
}

// ---- apps mode (7.2) ----
eq('apps blocker', [M.appsBlocker(null, { agent: true }), M.appsBlocker({ state: 'connected', caps: 32 }, { agent: false }), M.appsBlocker({ state: 'connected', caps: 32 }, { agent: true }),
  M.appsBlocker({ state: 'connected', caps: 288 }, { agent: true })], ['needsAgent', 'needsAgent', 'needsTools', null]);
eq('agent lines say apps', agentLines({ state: 'connected', caps: 256 }, { agent: true }).find((x) => x[0] === 'apps')[1], 'works');
eq('apps guest size', [M.appsGuestSize({ w: 1920, h: 1080 }), M.appsGuestSize({ w: 1367, h: 769 }), M.appsGuestSize({ w: 100, h: 50 }), M.appsGuestSize({ w: 10000, h: 9000 })],
  [[1920, 1080], [1366, 768], [320, 200], [8192, 8192]]);
eq('guest scale', [M.guestScale(1920, 1080, { w: 1920, h: 1080 }), M.guestScale(3840, 2160, { w: 1920, h: 1080 }), M.guestScale(1024, 768, { w: 2048, h: 1080 }), M.guestScale(0, 0, { w: 1, h: 1 })], [1, 0.5, 1.40625, 1]);
const D0 = { x: 1920, y: 0, w: 1920, h: 1080 };
eq('to host rect', M.toHostRect({ x: 100, y: 50, w: 640, h: 480 }, 0.5, D0), { x: 1970, y: 25, w: 320, h: 240 });
eq('round trip rect', M.toGuestRect(M.toHostRect({ x: 100, y: 50, w: 640, h: 480 }, 1, D0), 1, D0), { x: 100, y: 50, w: 640, h: 480 });
eq('crop of', M.cropOf({ x: 10, y: 20, w: 30, h: 40 }), [10, 20, 30, 40]);
eq('crop point 1:1', M.cropPoint(110, 220, [100, 200, 300, 400], [500, 600, 300, 400], 1920, 1080), [510, 620]);
eq('crop point scaled', M.cropPoint(250, 400, [100, 200, 300, 400], [500, 600, 600, 800], 1920, 1080), [800, 1000]);
eq('crop point clamped to the screen', M.cropPoint(399, 599, [100, 200, 300, 400], [1800, 1000, 300, 400], 1920, 1080), [1919, 1079]);
eq('crop point outside', [M.cropPoint(99, 220, [100, 200, 300, 400], [0, 0, 300, 400], 1920, 1080), M.cropPoint(400, 220, [100, 200, 300, 400], [0, 0, 300, 400], 1920, 1080), M.cropPoint(1, 1, [0, 0, 0, 0], [0, 0, 1, 1], 9, 9)], [null, null, null]);
{
  // a guest window above the menu bar (Calculator at y = -10 on a 1:1 guest; 28 px menu bar, 32 px title bar): moved
  // below it, x kept; one that fits stays; the start side's bar too
  const work = { x: 0, y: 28, w: 1920, h: 1052 }, inset = { l: 1, t: 32, r: 1, b: 1 };
  eq('inside work: under the menu bar', M.insideWork({ x: 47, y: -10, w: 320, h: 500 }, inset, work), { x: 47, y: 60, w: 320, h: 500 });
  eq('inside work: fits', M.insideWork({ x: 47, y: 60, w: 320, h: 500 }, inset, work), null);
  eq('inside work: a left bar', M.insideWork({ x: 10, y: 200, w: 320, h: 500 }, { l: 0, t: 30 }, { x: 64, y: 0, w: 1856, h: 1080 }), { x: 64, y: 200, w: 320, h: 500 });
  eq('inside work: no inset, no area', [M.insideWork({ x: 0, y: 0, w: 5, h: 5 }, null, { x: 0, y: 0, w: 9, h: 9 }), M.insideWork({ x: 0, y: 0, w: 5, h: 5 }, null, null)], [null, null]);
  // the place asked of the guest is where the LordOS window shows: the crop stays 1:1 once the guest moves there
  const d = { x: 0, y: 0, w: 1920, h: 1080 }, k = M.guestScale(1920, 1080, d);
  const to = M.insideWork(M.toHostRect({ x: 47, y: -10, w: 320, h: 500 }, k, d), inset, work);
  eq('inside work: the guest asked', M.toGuestRect(to, k, d), { x: 47, y: 60, w: 320, h: 500 });
}
// the guest tools' install errors: lord-vm's words → a translated line, its text kept only as a detail
eq('install error: missing', M.installError("the LordOS guest tools for Windows aren't on this computer"), { key: 'missing', detail: '' });
eq('install error: busy', [M.installError('busy').key, M.installError('the guest agent is busy').key], ['busy', 'busy']);
eq('install error: no exec', M.installError("the guest agent can't write files or run commands").key, 'noExec');
eq('install error: timeout', M.installError("the guest's script took too long").key, 'timeout');
eq('install error: gone', [M.installError('qemu-guest-agent stopped answering in the guest').key, M.installError('the guest agent started again').key], ['gone', 'gone']);
eq('install error: script', M.installError("the guest's script failed (1): Access is denied"), { key: 'failed', detail: "the guest's script failed (1): Access is denied" });
eq('install error: other', M.installError('qemu-guest-agent: Guest agent command failed'), { key: 'other', detail: 'qemu-guest-agent: Guest agent command failed' });
eq('install error: empty', M.installError(''), { key: 'other', detail: '' });
// a bridged start: Starting while the admin helper makes the network, never the earlier run's Off; one Play at a time
{
  const off = { state: 'stopped', reason: 'guest-shutdown' };
  eq('start phase: a new window', M.startPhase(0, null, false), 'starting');
  eq('start phase: bridging over an earlier run', M.startPhase(0, off, true), 'starting');
  eq('start phase: off', M.startPhase(0, off, false), 'off');
  eq('start phase: running', M.startPhase(7, { state: 'starting' }, false), 'session');
  // a session whose first state hasn't come yet is still starting (the window read st.state of null and showed its error)
  eq('start phase: a session before its first state', [M.startPhase(7, null, true), M.startPhase(7, null, false)], ['starting', 'starting']);
  eq('play starts', [M.playStarts(0, false, false), M.playStarts(0, true, false), M.playStarts(5, false, false), M.playStarts(0, false, 'shutdown')], [true, false, false, false]);
}
eq('near rect', [M.nearRect({ x: 0, y: 0, w: 10, h: 10 }, { x: 2, y: -2, w: 11, h: 9 }), M.nearRect({ x: 0, y: 0, w: 10, h: 10 }, { x: 3, y: 0, w: 10, h: 10 }), M.nearRect(null, {})], [true, false, false]);
// the guest didn't take the place asked of its window (its minimum size): after a while the LordOS window goes back
{
  const asked = { x: 401, y: 321, w: 900, h: 379 }, got = { x: 401, y: 321, w: 900, h: 493 };
  eq('guest kept its size', [M.guestKept(asked, 1000, got, 1400), M.guestKept(asked, 1000, got, 2600), M.guestKept(asked, 1000, { ...asked, w: 901 }, 9000),
    M.guestKept(null, 0, got, 9000), M.guestKept(asked, 0, null, 9000)], [false, true, false, false, false]);
{
  // apps mode on several displays (a window dragged to monitor 2 closed: the guest was asked to move off its screen)
  const d = { x: 0, y: 0, w: 1920, h: 1080 }, k = M.guestScale(1920, 1080, d);
  const p2 = M.guestPlace({ x: 2500, y: 200, w: 800, h: 600 }, k, d, 1920, 1080);
  eq('monitor 2 (x 2500): the guest stays on its screen', p2.ask, { x: 1120, y: 200, w: 800, h: 600 });
  eq('monitor 2: the window keeps its place (offset)', [p2.off, M.offsetRect(M.toHostRect(p2.ask, k, d), p2.off)], [{ x: 1380, y: 0 }, { x: 2500, y: 200, w: 800, h: 600 }]);
  const left = M.guestPlace({ x: -1500, y: -40, w: 700, h: 500 }, k, d, 1920, 1080);
  eq('a monitor to the left (negative x, above)', [left.ask, left.off], [{ x: 0, y: 0, w: 700, h: 500 }, { x: -1500, y: -40 }]);
  const big = M.guestPlace({ x: 2000, y: 0, w: 2560, h: 1400 }, k, d, 1920, 1080);
  eq('larger than the guest screen: its size', big.ask, { x: 0, y: 0, w: 1920, h: 1080 });
  eq('on the guest screen: no offset', M.guestPlace({ x: 100, y: 100, w: 640, h: 480 }, k, d, 1920, 1080), { ask: { x: 100, y: 100, w: 640, h: 480 }, off: null });
  // a main display with a scale (the guest's screen half its logical size on a 2× display, k = 2)
  const d2 = { x: 0, y: 0, w: 1920, h: 1080 }, k2 = M.guestScale(960, 540, d2);
  const h2 = M.guestPlace({ x: 3000, y: 100, w: 400, h: 300 }, k2, d2, 960, 540);
  eq('scaled: inside 0..960', [h2.ask.x >= 0, h2.ask.x + h2.ask.w <= 960, h2.ask.w, h2.ask.h], [true, true, 200, 150]);
  for (let i = 0; i < 2000; i++) {
    const r = { x: Math.round(Math.sin(i) * 6000), y: Math.round(Math.cos(i * 3) * 3000), w: 50 + (i * 37) % 3000, h: 40 + (i * 53) % 2000 };
    const g = M.guestPlace(r, k, d, 1920, 1080).ask;
    if (!(g.x >= 0 && g.y >= 0 && g.x + g.w <= 1920 && g.y + g.h <= 1080)) { eq(`guest place inside its screen #${i}`, g, 'inside'); break; }
  }
  // files dropped on an apps mode window (7.2.1)
  const pb = M.placeBatch(['/h/a.txt', 'rel.txt', '/h/b.png'], 0x30140, [2500.4, -3], 7);
  eq('a drop: absolute paths, one batch, the point inside the guest screen', pb, { items: [
    { path: '/h/a.txt', place: { win: 0x30140, x: 2500, y: 0, batch: 7, index: 0, count: 2 } },
    { path: '/h/b.png', place: { win: 0x30140, x: 2500, y: 0, batch: 7, index: 1, count: 2 } }], left: 1 });
  const many = M.placeBatch(Array.from({ length: 11 }, (_, i) => `/f${i}`), 5, [1, 1]);
  eq('at most PLACE_MAX at once', [many.items.length, many.left, many.items[7].place.count, many.items[0].place.batch >= 1], [M.PLACE_MAX, 3, M.PLACE_MAX, true]);
  eq('offsetRect without offset', M.offsetRect({ x: 1, y: 2, w: 3, h: 4 }, null), { x: 1, y: 2, w: 3, h: 4 });
  eq('minimized by our move: restored', [M.minimizedByUs(1000, 2000, false), M.minimizedByUs(1000, 5000, false), M.minimizedByUs(1000, 2000, true), M.minimizedByUs(0, 2000, false)], [true, false, false, false]);
  // a window's Dock icon: asked for every window that has none yet, on each new list, not only when the icons change
  // (a window opened from a later list than its icon came with, as after a re-attach, kept a letter tile)
  {
    const a = { gid: 1, winId: 'machines#ga-m-1' }, b = { gid: 2, winId: 0 }, c = { gid: 3, winId: 'machines#ga-m-3', icon: true, iconSeq: 4 };
    eq('icons: windows without one, not those without a LordOS window', M.iconsToAsk([a, b, c], 4).map((x) => x.gid), [1]);
    eq('icons: all again when the guest\'s icons changed', M.iconsToAsk([a, b, c], 5).map((x) => x.gid), [1, 3]);
    a.icon = true; a.iconSeq = 4;
    eq('icons: nothing to ask once each has its own', M.iconsToAsk(new Map([[1, a], [3, c]]).values(), 4), []);
    eq('icons: no tracks', M.iconsToAsk(null, 1), []);
  }
}
}
eq('popup', [M.isPopup({ owner: 5, topmost: true }), M.isPopup({ owner: 5, topmost: false }), M.isPopup({ owner: 0, topmost: true })], [true, false, false]);
eq('instance', [M.guestInstance('ab12-cd', 4294967295), M.guestInstance('a/../b', 7)], ['ga-ab12-cd-4294967295', 'ga-ab-7']);
{
  const W = (id, x, extra = {}) => ({ id, x, y: 10, w: 300, h: 200, minimized: false, ...extra });
  const have = new Map([[1, { rect: W(1, 0), minimized: false }], [9, { rect: W(9, 0), minimized: false }], [2, { rect: W(2, 50), minimized: false, asked: W(2, 80) }], [3, { rect: W(3, 0), minimized: false }], [4, { rect: W(4, 0), minimized: true }]]);
  const p = M.planWindows([W(1, 20), W(9, 1), W(2, 80), W(4, 0), W(5, 0), W(6, 0, { minimized: true }), W(7, 0, { w: 4 }), W(8, 0, { minimized: true })], have);
  eq('plan open', p.open.map((w) => w.id), [5]);
  eq('plan close', p.close, [3]);
  eq('plan follow (not what this side asked, not a pixel of rounding)', p.follow.map((w) => w.id), [1]);
  eq('plan restore', p.restore.map((w) => w.id), [4]);
  const q = M.planWindows([W(1, 200, { minimized: true })], new Map([[1, { rect: W(1, 0), minimized: false }]]));
  eq('plan minimize, no follow', [q.minimize.map((w) => w.id), q.follow.length], [[1], 0]);
}
eq('close refusals: the guest\'s list as a slot per window', M.closeRefusedOf([{ gid: 70188, reason: 'elevated', at: 1795400000000 }, { gid: 5, reason: 'other', at: 1 }]),
  { 70188: { reason: 'elevated', at: 1795400000000 }, 5: { reason: 'other', at: 1 } });
eq('close refusals: anything else left out', M.closeRefusedOf([null, { gid: 0, reason: 'elevated', at: 1 }, { gid: 3, reason: 'admin', at: 1 }, { gid: 4, reason: 'other' }, { gid: '7', reason: 'other', at: 1 }]), {});
eq('close refusals: the last per window', M.closeRefusedOf([{ gid: 9, reason: 'other', at: 1 }, { gid: 9, reason: 'elevated', at: 2 }]), { 9: { reason: 'elevated', at: 2 } });
eq('close refusals: not a list', [M.closeRefusedOf(undefined), M.closeRefusedOf({})], [{}, {}]);
eq('guest apps menu', M.guestAppItems([{ name: 'Zeta', token: 'z' }, { name: 'alpha', token: 'a' }, { name: 'dup', token: 'a' }, { name: '', token: 'x' }, null]).map((a) => a.token), ['a', 'z']);
{
  // apps mode's Running list: no popups, no owned windows, no slivers; minimized kept; sorted by app, title, id
  const list = [
    { id: 7, title: 'Report.docx', app: 'Word', w: 800, h: 600, focused: true },
    { id: 3, title: '', app: 'Notepad', w: 400, h: 300, minimized: true },
    { id: 9, title: 'Menu', app: 'Word', owner: 7, topmost: true, w: 200, h: 300 },
    { id: 10, title: 'Save As', app: 'Word', owner: 7, w: 500, h: 400 },
    { id: 11, title: 'tiny', app: 'X', w: 4, h: 4 },
    { id: 5, title: 'Budget.docx', app: 'Word', w: 800, h: 600 },
    null, { title: 'no id', w: 100, h: 100 },
  ];
  const rows = M.runningWindows(list);
  eq('running windows', rows.map((r) => r.id), [3, 5, 7]);
  eq('running row (title falls back to the app)', rows[0], { id: 3, title: 'Notepad', app: 'Notepad', minimized: true, focused: false });
  eq('running none', [M.runningWindows(null), M.runningWindows([])], [[], []]);
  eq('running key changes with a title, not with a move', [M.runningKey(rows) === M.runningKey(M.runningWindows(list.map((w) => (w ? { ...w, x: 99 } : w)))),
    M.runningKey(rows) === M.runningKey(M.runningWindows(list.map((w) => (w?.id === 5 ? { ...w, title: 'Other' } : w))))], [true, false]);
  const apps = M.guestAppItems([{ name: 'Calculator', token: 'c' }, { name: 'Microsoft Edge', token: 'e' }, { name: 'Paint', token: 'p' }, { name: 'Edge Dev Tools', token: 'd' }, { name: 'Configuración', token: 's' }]);
  eq('apps search empty keeps all', M.appsMatching(apps, '  ').map((a) => a.token), ['c', 's', 'd', 'e', 'p']);
  eq('apps search: name start, then a word start, then inside', M.appsMatching(apps, 'ed').map((a) => a.token), ['d', 'e']);
  eq('apps search: accents and case off', M.appsMatching(apps, 'CONFIGURACION').map((a) => a.token), ['s']);
  eq('apps search: every word', [M.appsMatching(apps, 'micro edge').map((a) => a.token), M.appsMatching(apps, 'micro paint')], [['e'], []]);
  eq('apps search: inside a word last', M.appsMatching(apps, 'a').map((a) => a.token)[0], 'c');
  eq('app initial', [M.appInitial(' paint'), M.appInitial(''), M.appInitial('écran')], ['P', '?', 'É']);
  // the launcher's apps (status item, controller): the search, and an icon per token when the guest sent one
  const px = { w: 16, h: 16 };
  eq('launcher apps: icons by token, none otherwise', M.launcherApps(apps, 'ed', { d: px }).map((a) => [a.token, a.icon]), [['d', px], ['e', null]]);
  eq('launcher apps: no icons at all', M.launcherApps(apps, '', undefined).every((a) => a.icon === null), true);
  eq('launcher apps: keeps name and token', M.launcherApps([{ name: 'Paint', token: 'p' }], 'pa', {})[0], { name: 'Paint', token: 'p', icon: null });
  // the status items by the clock: one per machine with a session, in name order
  const gs = { '/m/win': { sid: 3, name: 'Windows 11' }, '/m/deb': { sid: 2, name: 'Debian' }, '/m/off': { name: 'Off' }, '/m/x': null };
  eq('apps status list', M.appsStatusList(gs), [{ dir: '/m/deb', name: 'Debian' }, { dir: '/m/win', name: 'Windows 11' }]);
  eq('apps status list: no name → the folder', M.appsStatusList({ '/a/b/vm1': { sid: 1 } }), [{ dir: '/a/b/vm1', name: 'vm1' }]);
  eq('apps status list: none', [M.appsStatusList(null), M.appsStatusList({})], [[], []]);
  eq('apps status diff: added and gone', M.appsStatusDiff(['/m/deb', '/m/old'], gs), { add: ['/m/win'], remove: ['/m/old'] });
  eq('apps status diff: nothing to do', M.appsStatusDiff(['/m/win', '/m/deb'], gs), { add: [], remove: [] });
  eq('apps status diff: apps mode ended everywhere', M.appsStatusDiff(['/m/deb'], {}), { add: [], remove: ['/m/deb'] });
  // launch feedback: one launch while it waits for a window of that app (5 s at most)
  eq('window of app: program name holds a word', [M.windowOfApp('Notepad', { app: 'notepad.exe' }), M.windowOfApp('Microsoft Edge', { app: 'msedge.exe' }),
    M.windowOfApp('Word', { app: 'WINWORD.EXE' }), M.windowOfApp('Calculadora', { app: 'ApplicationFrameHost.exe', title: 'Calculadora' })], [true, true, true, true]);
  eq('window of app: not another app', [M.windowOfApp('Notepad', { app: 'mspaint.exe', title: 'Untitled - Paint' }), M.windowOfApp('', { app: 'x.exe' }), M.windowOfApp('Go', { app: 'go.exe' })], [false, false, false]);
  const lst0 = [{ id: 1, app: 'explorer.exe', title: 'Files' }];
  const np = { name: 'Notepad', token: 'n' };
  const L1 = M.launchStart({}, np, lst0, 1000);
  eq('launch start: what was listed', L1.n, { at: 1000, name: 'Notepad', seen: [1] });
  eq('launch pending: waits', M.launchPending(L1.n, lst0, 1000 + 4999), true);
  eq('launch pending: 5 s passed', M.launchPending(L1.n, lst0, 1000 + M.LAUNCH_WAIT_MS), false);
  const lst1 = [...lst0, { id: 7, app: 'notepad.exe', title: 'Untitled - Notepad' }];
  eq('launch answered by a new window of the app', [M.launchAnswered(L1.n, lst1), M.launchPending(L1.n, lst1, 1500)], [7, false]);
  eq('launch: a window that was already there does not answer', M.launchAnswered(M.launchStart({}, np, lst1, 0).n, lst1), 0);
  eq('launch: an owned window does not answer', M.launchAnswered(L1.n, [...lst0, { id: 8, app: 'notepad.exe', owner: 7 }]), 0);
  eq('launch: none', [M.launchPending(undefined, lst0, 0), M.launchAnswered(null, lst1)], [false, 0]);
  eq('launch prune keeps the waiting ones', Object.keys(M.launchPrune({ ...L1, p: { at: 0, name: 'Paint', seen: [] } }, lst0, 5500)), ['n']);
  // close feedback: Closing…, then a note when it stays or the guest refused
  const C1 = M.closeStart({}, 7, 1000);
  eq('close start', C1, { 7: { gid: 7, at: 1000 } });
  eq('close: closing', M.closeView(C1[7], lst1, null, 2000), { state: 'closing' });
  eq('close: still there after 3 s', M.closeView(C1[7], lst1, null, 1000 + M.CLOSE_WAIT_MS), { state: 'refused', reason: '' });
  eq('close: gone', M.closeView(C1[7], lst0, null, 9000), null);
  eq('close: refused as administrator', M.closeView(C1[7], lst1, { reason: 'elevated', at: 1200 }, 1300), { state: 'refused', reason: 'elevated' });
  eq('close: another reason is generic', M.closeView(C1[7], lst1, { reason: 'busy', at: 1200 }, 1300), { state: 'refused', reason: '' });
  eq('close: a refusal from before the click does not count', M.closeView(C1[7], lst1, { reason: 'elevated', at: 900 }, 1300), { state: 'closing' });
  eq('close: none', M.closeView(null, lst1, null, 0), null);
  eq('close prune drops closed windows', M.closePrune({ ...C1, 9: { gid: 9, at: 0 } }, lst1), C1);
  eq('machine os: kept, then guessed', [M.machineOsId({ os: 'fedora' }, 'Debian'), M.machineOsId({ osName: 'Ubuntu 24.04' }, 'Work'), M.machineOsId({}, 'Debian 13'), M.machineOsId(null, 'zzz')], ['fedora', 'ubuntu', 'debian', 'other']);
}
// the boot order (Configure → Boot)
eq('boot list auto', M.bootList('auto'), ['disk', 'cd', 'network']);
eq('boot list chosen', M.bootList('network,disk'), ['network', 'disk', 'cd']);
eq('boot list junk', [M.bootList(undefined), M.bootList('floppy,cd,cd')], [['disk', 'cd', 'network'], ['cd', 'disk', 'network']]);
eq('boot move', [M.bootMove(['disk', 'cd', 'network'], 2, -1), M.bootMove(['disk', 'cd', 'network'], 0, 1), M.bootMove(['disk', 'cd', 'network'], 0, -1), M.bootMove(['disk', 'cd', 'network'], 2, 1)],
  [['disk', 'network', 'cd'], ['cd', 'disk', 'network'], ['disk', 'cd', 'network'], ['disk', 'cd', 'network']]);
eq('boot summary', [M.bootSummary('auto'), M.bootSummary(undefined), M.bootSummary('network,disk'), M.bootSummary('cd')], [null, null, 'machines.cfg.bootFirst.network', 'machines.cfg.bootFirst.cd']);

// ---- the clipboard's gate and its notices (6.8) ----
{
  const { clipInUse, clipBlocked, CLIP_BURST_MS, CLIP_RENOTIFY_MS, AGENT_CAPS } = M;
  eq('in use: window in front', clipInUse(3, true, false), true);
  eq('in use: an apps mode window in front', clipInUse(3, false, true), true);
  eq('not in use: background', clipInUse(3, false, false), false);
  eq('not in use: no session', clipInUse(0, true, true), false);
  eq('lazy cap bit', AGENT_CAPS.clipLazy, 4096);
  let r = clipBlocked({}, 2, 0, 1000);
  eq('first look: nothing told (old counts)', r.notify, []);
  r = clipBlocked(r.seen, 3, 0, 2000);
  eq('a read blocked: told', r.notify, ['read']);
  r = clipBlocked(r.seen, 9, 0, 5000);
  eq('more in the same burst: quiet', r.notify, []);
  r = clipBlocked(r.seen, 10, 0, 5000 + CLIP_BURST_MS - 1);
  eq('still the burst (each one extends it)', r.notify, []);
  r = clipBlocked(r.seen, 11, 0, 5000 + 2 * CLIP_BURST_MS);
  eq('after a quiet spell: told again', r.notify, ['read']);
  r = clipBlocked(r.seen, 11, 1, 5000 + 2 * CLIP_BURST_MS + 10);
  eq('a write is its own burst', r.notify, ['write']);
  let s = r.seen, t = 5000 + 2 * CLIP_BURST_MS, told = 0;
  for (let k = 1; t < 5000 + 2 * CLIP_BURST_MS + CLIP_RENOTIFY_MS + 20000; k++) { t += 10000; const x = clipBlocked(s, 11 + k, 1, t); s = x.seen; told += x.notify.length; }
  eq('a steady trickle: told again after the renotify time', told, 1);
  eq('a new session (counts drop) is a new baseline', clipBlocked({ read: 50, write: 3 }, 0, 0, 1).notify, []);
  eq('unchanged: quiet', clipBlocked({ read: 1, write: 1 }, 1, 1, 9e9).notify, []);
}

// ---- "ask before each paste" (6.8, machine.json clipboard_ask) ----
{
  const { clipAskEmpty, clipAskRequest, clipAskPush, clipAskCopied, clipAskAnswer, clipAskTick, clipAskShown, clipPreviewLine, CLIP_ASK_IN_MS, CLIP_ASK_OUT_MS } = M;
  let st = clipAskEmpty();
  eq('ask: nothing shown', clipAskShown(st), null);
  let r = clipAskRequest(st, 4, 100); st = r.state;
  eq('ask: a request shows the bar', [clipAskShown(st), r.answer], ['in', undefined]);
  r = clipAskRequest(st, 4, 200);
  eq('ask: the same offer again (another form) waits for the same answer', [r.state === st, r.answer], [true, undefined]);
  r = clipAskAnswer(st, true); st = r.state;
  eq('ask: yes answers that offer', [r.answer, clipAskShown(st)], [{ gen: 4, yes: true }, null]);
  r = clipAskRequest(st, 4, 300);
  eq('ask: a later form of the same offer is answered at once', r.answer, { gen: 4, yes: true });
  r = clipAskRequest(st, 5, 400); st = r.state;
  eq('ask: a new offer asks again', [clipAskShown(st), r.answer], ['in', undefined]);
  r = clipAskTick(st, 400 + CLIP_ASK_IN_MS - 1);
  eq('ask: not yet timed out', r.answer, undefined);
  r = clipAskTick(st, 400 + CLIP_ASK_IN_MS); st = r.state;
  eq('ask: no answer in time is a no', [r.answer, clipAskShown(st)], [{ gen: 5, yes: false }, null]);
  eq('ask: and that offer stays a no', clipAskRequest(st, 5, 1e6).answer, { gen: 5, yes: false });
  // out: what the machine copied
  r = clipAskCopied(st, 9, 1000); st = r.state;
  eq('ask: a copy out shows the bar', clipAskShown(st), 'out');
  r = clipAskRequest(st, 6, 1100); st = r.state;
  eq('ask: a paste in goes first', clipAskShown(st), 'in');
  r = clipAskAnswer(st, false); st = r.state;
  eq('ask: no to the paste, the copy out shows next', [r.answer, clipAskShown(st)], [{ gen: 6, yes: false }, 'out']);
  r = clipAskCopied(st, 10, 1200); st = r.state;
  eq('ask: a newer copy replaces the older one', st.out.seq, 10);
  r = clipAskTick(st, 1200 + CLIP_ASK_OUT_MS); st = r.state;
  eq('ask: a copy out is ignored after a minute', [r.answer, clipAskShown(st)], [{ seq: 10, yes: false }, null]);
  r = clipAskCopied(st, 11, 2000); st = r.state;
  r = clipAskAnswer(st, true); st = r.state;
  eq('ask: bring', r.answer, { seq: 11, yes: true });
  // push (a helper that can't ask): keyed by the clipboard's hash
  r = clipAskPush(st, 'abc', 3000); st = r.state;
  eq('push: asks', clipAskShown(st), 'push');
  r = clipAskAnswer(st, true); st = r.state;
  eq('push: yes', r.answer, { key: 'abc', yes: true });
  eq('push: the same clipboard again is answered at once', clipAskPush(st, 'abc', 4000).answer, { key: 'abc', yes: true });
  r = clipAskPush(st, 'def', 5000); st = r.state;
  eq('push: a new one asks', clipAskShown(st), 'push');
  r = clipAskTick(st, 5000 + CLIP_ASK_IN_MS);
  eq('push: no answer is a no', r.answer, { key: 'def', yes: false });
  eq('push: waits behind a request', clipAskPush(clipAskRequest(clipAskEmpty(), 3, 0).state, 'x', 1).state.in, { gen: 3, at: 0 });
  eq('ask: a request of generation 0 is nothing', clipAskRequest(clipAskEmpty(), 0, 1).state.in, null);
  // the preview line
  eq('preview: one line', clipPreviewLine({ kind: 'text', text: 'a\n b\t\tc ' }), { kind: 'text', text: 'a b c' });
  eq('preview: cut at 80 characters', [...clipPreviewLine({ kind: 'text', text: 'é'.repeat(100) }).text].length, 80);
  eq('preview: a picture', clipPreviewLine({ kind: 'image', w: 640, h: 480 }), { kind: 'image', w: 640, h: 480 });
  eq('preview: rich text keeps its kind', clipPreviewLine({ kind: 'html', text: 'Hi' }).kind, 'html');
  eq('preview: none', clipPreviewLine(null), { kind: '', text: '' });
}

// ---- a Windows Clone's computer name; a machine whose description can't be read; a linked clone's disk ----
{
  const { winNameValid, cloneWinName, isWindowsMachine, unreadableOf, actionsOf, WIN_NAME_MAX } = M;
  eq('win name: the default fits', cloneWinName('Windows 11 Pro prueba pepe'), 'windows-1-clone');
  eq('win name: default valid', winNameValid(cloneWinName('Windows 11 Pro prueba pepe')), true);
  eq('win name: a short name', cloneWinName('Win'), 'win-clone');
  eq('win name: digits only name', winNameValid(cloneWinName('12345')), true);
  eq('win name: max', WIN_NAME_MAX, 15);
  eq('win name: 15 ok, 16 not', [winNameValid('abcdefghijklmno'), winNameValid('abcdefghijklmnop')], [true, false]);
  eq('win name: only digits refused', winNameValid('12345'), false);
  eq('win name: dash first/last refused', [winNameValid('-pc'), winNameValid('pc-')], [false, false]);
  eq('win name: other characters refused', [winNameValid('pc_1'), winNameValid('pc 1'), winNameValid('')], [false, false, false]);
  eq('win name: letters, digits, dashes', winNameValid('win-11-a'), true);
  eq('windows machine', [isWindowsMachine({ windowsDevices: true }), isWindowsMachine({ devices: 'windows' }), isWindowsMachine({ devices: 'virtio' }), isWindowsMachine(null)], [true, true, false, false]);
  eq('unreadable', [unreadableOf({ error: 'newer' }), unreadableOf({ error: 'invalid' }), unreadableOf({ error: '' }), unreadableOf(undefined)], ['newer', 'invalid', null, null]);
  eq('unreadable: no start', actionsOf({ state: 'stopped', error: 'newer' }).start, false);
  eq('readable: start', actionsOf({ state: 'stopped', error: '' }).start, M.actionsFor('stopped').start);
}

// ---- the Requirements page: every row it can show has its title and its line in English ----
{
  const fsm = await import('node:fs');
  const en = JSON.parse(fsm.readFileSync(path.join(here, '../../assets/locales/en/machines.json'), 'utf8'));
  const host = { kvm: false, kvmDevice: true };
  const ids = new Set(M.reqRows(host, ['amd64', 'arm64'], ['swtpm', 'ovmf']).map(([k]) => k.split('.')[0]));
  eq('requirements: every id is known', [...ids].sort(), [...M.REQ_IDS].sort());
  for (const id of M.REQ_IDS) eq(`requirements: ${id} has req.row and req.use`, [typeof en[`req.row.${id}`], typeof en[`req.use.${id}`]], ['string', 'string']);
  eq('requirements: tpm only with amd64', M.reqRows(host, ['arm64'], []).some(([k]) => k === 'tpm'), false);
  eq('requirements: missing swtpm', M.reqRows(host, ['amd64'], ['swtpm']).find(([k]) => k === 'tpm')[1], 'missing');
  eq('library menu key', typeof en.libraryMenu, 'string');
}

// ---- the LordOS tools: older than the ones shipped (the popover's Update, the bar's dot) ----
{
  const fsm = await import('node:fs');
  const verIn = (f) => (/#define VERSION "([^"]+)"/.exec(fsm.readFileSync(path.join(here, '../..', f), 'utf8')) || [])[1];
  eq('shipped Windows tools = lordos-guest-win.c VERSION', M.TOOLS_SHIPPED.windows, verIn('tools/guest-win/lordos-guest-win.c'));
  eq('shipped Linux helper = lordos-guest.c VERSION', M.TOOLS_SHIPPED.linux, verIn('tools/guest/lordos-guest.c'));
  const on = (tools) => ({ state: 'connected', caps: 0, tools });
  const S = { windows: 'w4', linux: '5' };
  eq('Windows tools w1 < w4: outdated', M.toolsOutdated(on('w1'), S), true);
  eq('Windows tools w3 < w4: outdated', M.toolsOutdated(on('w3'), S), true);
  eq('Windows tools w4: current', M.toolsOutdated(on('w4'), S), false);
  eq('Windows tools w10 (newer): not outdated', M.toolsOutdated(on('w10'), S), false);
  eq('Linux helper 4 < 5: outdated', M.toolsOutdated(on('4'), S), true);
  eq('Linux helper 5: current', M.toolsOutdated(on('5'), S), false);
  eq('no hello yet: nothing to say', M.toolsOutdated(on(''), S), false);
  eq('unknown format: nothing to say', [M.toolsOutdated(on('x7'), S), M.toolsOutdated(on('w4.1'), S), M.toolsOutdated(on('w'), S)], [false, false, false]);
  eq('not connected: nothing to say', [M.toolsOutdated({ state: 'waiting', tools: 'w1' }, S), M.toolsOutdated(null, S)], [false, false]);
  eq('the default is what ships', M.toolsOutdated(on('w1')), true);
  // the popover's line, apart from qemu-guest-agent's version
  eq('tools line: older', M.toolsLine(on('w7'), { windows: 'w9', linux: '5' }), { key: 'old', have: 'w7', want: 'w9' });
  eq('tools line: current', M.toolsLine(on('w9'), { windows: 'w9', linux: '5' }), { key: 'current', have: 'w9' });
  eq('tools line: newer is current', M.toolsLine(on('w12'), { windows: 'w9', linux: '5' }), { key: 'current', have: 'w12' });
  eq('tools line: Linux older', M.toolsLine(on('4'), S), { key: 'old', have: '4', want: '5' });
  eq('tools line: unknown format', M.toolsLine(on('x1'), S), { key: 'other', have: 'x1' });
  eq('tools line: none', [M.toolsLine(on(''), S), M.toolsLine({ state: 'waiting', tools: 'w1' }, S), M.toolsLine(null, S)], [null, null, null]);
  eq('tools want', [M.toolsWant(on('w1'), S), M.toolsWant(on('w4'), S), M.toolsWant(on('3'), S)], ['w4', '', '5']);
}

// ---- the LordOS tools updated by themselves: when, once per run and version, backoff, settings, the notification ----
{
  const S = { windows: 'w9', linux: '5' };
  const ag = (tools, install = 0, state = 'connected') => ({ state, caps: 0, tools, install });
  const step = (s, agent, now, o = {}) => M.toolsAutoStep(s, { agent, on: true, busy: false, now, ...o }, S);
  const T0 = 1000000, SET = M.TOOLS_SETTLE_MS;
  // settings: the machine's own choice, else the default (on)
  eq('auto: on by default', M.toolsAutoOn({}, {}), true);
  eq('auto: the global default off', M.toolsAutoOn({}, { toolsAuto: false }), false);
  eq('auto: the machine says on over the default', M.toolsAutoOn({ toolsAuto: true }, { toolsAuto: false }), true);
  eq('auto: the machine says off', M.toolsAutoOn({ toolsAuto: false }, {}), false);
  eq('auto: nothing known', M.toolsAutoOn(undefined, undefined), true);
  // waits for the agent to settle, then installs once
  let r = step(null, ag('w7'), T0);
  eq('auto: just connected: waits', [r.act, r.s.since, r.s.want], [null, T0, 'w9']);
  r = step(r.s, ag('w7'), T0 + SET - 1);
  eq('auto: still settling', r.act, null);
  r = step(r.s, ag('w7'), T0 + SET);
  eq('auto: settled: install', [r.act, r.s.tries], ['install', 1]);
  const sent = r.s;
  r = step(sent, ag('w7', 1), T0 + SET + 20);
  eq('auto: on its way', [r.act, r.s.pending], [null, T0 + SET]);
  eq('auto: a new session (install 0) waits for the answer', step(sent, ag('w7', 0), T0 + SET + 60000).s.pending, T0 + SET);
  eq('auto: refused at once counts', step(sent, ag('w7', 3), T0 + SET + 5).result, 'failed');
  let ok = step(r.s, ag('w7', 2), T0 + SET + 30);
  eq('auto: done', [ok.act, ok.result, ok.s.pending, ok.s.done], [null, 'ok', 0, T0 + SET + 30]);
  eq('auto: the tools say w9: nothing more', step(ok.s, ag('w9', 2), T0 + SET + 40).act, null);
  // installed but the tools still say w7 a while later: one notification, never another install in this run
  r = step(ok.s, ag('w7', 2), T0 + SET + 40);
  eq('auto: installed, still old, too soon to say', r.act, null);
  r = step(r.s, ag('w7', 2), T0 + SET + 30 + SET * 3);
  eq('auto: installed, still old: notify once', [r.act, r.why], ['notify', 'failed']);
  r = step(r.s, ag('w7', 2), T0 + SET * 50);
  eq('auto: and only once', r.act, null);
  // failures: backoff, then given up with one notification
  r = step(sent, ag('w7', 1), T0 + SET + 20);
  r = step(r.s, ag('w7', 3), T0 + SET + 30);
  eq('auto: failed: backoff', [r.act, r.result, r.s.next], [null, 'failed', T0 + SET + 30 + M.TOOLS_BACKOFF_MS[0]]);
  let t = T0 + SET + 30;
  eq('auto: not before the backoff', step(r.s, ag('w7', 3), t + M.TOOLS_BACKOFF_MS[0] - 1).act, null);
  r = step(r.s, ag('w7', 3), t + M.TOOLS_BACKOFF_MS[0]);
  eq('auto: second try', [r.act, r.s.tries], ['install', 2]);
  t += M.TOOLS_BACKOFF_MS[0];
  // no answer at all: counted as failed after TOOLS_WAIT_MS
  eq('auto: waiting for its answer', step(r.s, ag('w7', 0), t + M.TOOLS_WAIT_MS).act, null);
  r = step(r.s, ag('w7', 0), t + M.TOOLS_WAIT_MS + 1);
  eq('auto: no answer: failed', [r.result, r.s.next], ['failed', t + M.TOOLS_WAIT_MS + 1 + M.TOOLS_BACKOFF_MS[1]]);
  t = r.s.next;
  r = step(r.s, ag('w7', 3), t);
  eq('auto: third try', [r.act, r.s.tries], ['install', 3]);
  r = step(r.s, ag('w7', 1), t + 5);
  r = step(r.s, ag('w7', 3), t + 10);
  eq('auto: the last failed: notify', [r.act, r.why, r.result], ['notify', 'failed', 'failed']);
  let loop = 0, s = r.s;
  for (let k = 1; k < 200; k++) { const x = step(s, ag('w7', 3), t + k * 3600000); s = x.s; if (x.act) loop++; }
  eq('auto: never a loop after giving up', loop, 0);
  // never while something is on its way, nor during an install the person asked for
  r = step(null, ag('w7'), T0);
  eq('auto: busy: deferred', step(r.s, ag('w7'), T0 + SET, { busy: true }).act, null);
  eq('auto: the person\'s install on its way: deferred', step(r.s, ag('w7', 1), T0 + SET).act, null);
  eq('auto: idle again: install', step(r.s, ag('w7'), T0 + SET + 5).act, 'install');
  // off: one notification per run and version, with no install
  r = step(null, ag('w7'), T0, { on: false });
  r = step(r.s, ag('w7'), T0 + SET, { on: false });
  eq('auto off: notify', [r.act, r.why, r.s.tries], ['notify', 'off', 0]);
  eq('auto off: once', step(r.s, ag('w7'), T0 + SET * 9, { on: false }).act, null);
  // the agent going away restarts the settling, nothing else
  r = step(r.s, ag('w7', 0, 'waiting'), T0 + SET * 10, { on: false });
  eq('auto: agent gone: settle again', r.s.since, 0);
  eq('auto: back, still told', step(r.s, ag('w7'), T0 + SET * 12, { on: false }).act, null);
  // nothing to do: current tools, a Linux helper current, a guest without the tools, not connected
  eq('auto: current tools', step(null, ag('w9'), T0 + SET * 2).act, null);
  eq('auto: no tools: never installs them by itself', step({ since: T0 }, ag(''), T0 + SET * 2).act, null);
  eq('auto: Linux helper older', step({ since: T0, want: '5' }, ag('4'), T0 + SET).act, 'install');
  // the shipped version changed (the host was upgraded): a fresh count
  r = M.toolsAutoStep({ ...M.TOOLS_AUTO_IDLE, want: 'w8', tries: 3, told: 'w8', since: T0 }, { agent: ag('w7'), on: true, busy: false, now: T0 + SET }, S);
  eq('auto: a newer version shipped: tried again', [r.act, r.s.want, r.s.tries], ['install', 'w9', 1]);
  eq('auto: failed backoff stops after the last', M.toolsAutoFailed({ tries: M.TOOLS_TRIES }, 5).next, Infinity);
}


// ---- a graphics card for a machine (Mode A, Intel GVT-g) ----
{
  const T = (name, highMb, maxW, maxH, weight, available) => ({ name, lowMb: 64, highMb, maxW, maxH, weight, available });
  const types = [T('i915-GVTg_V5_1', 2048, 1920, 1200, 16, 1), T('i915-GVTg_V5_2', 1024, 1920, 1200, 8, 1), T('i915-GVTg_V5_4', 512, 1920, 1200, 4, 2),
    T('i915-GVTg_V5_8', 256, 1024, 768, 2, 4)];
  const igpu = { address: '0000:00:02.0', vendor: 0x8086, device: 0x9bc5, desktop: false, gvtCapable: true, gvtOn: true, types };
  const amd = { address: '0000:01:00.0', vendor: 0x1002, device: 0x67df, desktop: true, gvtCapable: false, gvtOn: false, types: [] };
  eq('gvtGpu: the spare Intel iGPU with GVT-g on', M.gvtGpu({ gpus: [amd, igpu] })?.address, '0000:00:02.0');
  eq('gvtGpu: not the desktop\'s', M.gvtGpu({ gpus: [{ ...igpu, desktop: true }] }), null);
  eq('gvtGpu: not when GVT-g is off', [M.gvtGpu({ gpus: [{ ...igpu, gvtOn: false }] }), M.gvtGpu(null), M.gvtGpu({})], [null, null, null]);
  eq('gvtSpare: capable but off', M.gvtSpare({ gpus: [amd, { ...igpu, gvtOn: false }] })?.address, '0000:00:02.0');
  eq('gvtSpare: none when on', M.gvtSpare({ gpus: [amd, igpu] }), null);
  eq('gpuIds', M.gpuIds(igpu), '8086:9bc5');
  eq('whole: the largest', M.gvtType(igpu, 'intel-whole', 'follow')?.name, 'i915-GVTg_V5_1');
  eq('whole: largest by memory, then weight', M.gvtType({ types: [T('a', 512, 1920, 1200, 4, 1), T('b', 512, 1920, 1200, 8, 1)] }, 'intel-whole', 'follow')?.name, 'b');
  eq('shared, follows the window: smallest showing 1920 × 1080', M.gvtType(igpu, 'intel-shared', 'follow')?.name, 'i915-GVTg_V5_4');
  eq('shared, 1024 × 768: the smallest', M.gvtType(igpu, 'intel-shared', '1024x768')?.name, 'i915-GVTg_V5_8');
  eq('shared, 2560 × 1440: none fits, the largest', M.gvtType(igpu, 'intel-shared', '2560x1440')?.name, 'i915-GVTg_V5_1');
  eq('none, or no types', [M.gvtType(igpu, '', 'follow'), M.gvtType({ types: [] }, 'intel-whole', 'follow'), M.gvtType(null, 'intel-shared', 'follow')], [null, null, null]);
  const busy = { ...igpu, types: types.map((t) => ({ ...t, available: t.name === 'i915-GVTg_V5_2' ? 1 : 0 })) };
  eq('shared, free only: the next one up that has an instance', M.gvtType(busy, 'intel-shared', 'follow', true)?.name, 'i915-GVTg_V5_2');
  eq('whole, free only: the largest or nothing', M.gvtType(busy, 'intel-whole', 'follow', true), null);
  eq('shared, free only: none free', M.gvtType({ ...igpu, types: types.map((t) => ({ ...t, available: 0 })) }, 'intel-shared', 'follow', true), null);
  eq('gvt arg', M.gvtArg({ id: '0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b' }, types[2]), '0f8e2c1a-3b4d-4e5f-8a6b-7c8d9e0f1a2b i915-GVTg_V5_4');
  eq('gvt arg without a type', [M.gvtArg({ id: 'x' }, null), M.gvtArg({}, types[0])], ['', '']);
  eq('memlock need: memory + 512', [M.memlockNeed({ memoryMb: 8192 }), M.memlockNeed({}), M.memlockNeed(null)], [8704, 0, 0]);
  const ml = { capMb: 12288, defaultMb: 12288, totalMb: 16384, minMb: 2048, maxMb: 14336, custom: false };
  eq('memlock fits', [M.memlockFits({ memoryMb: 8192 }, ml), M.memlockFits({ memoryMb: 11776 }, ml), M.memlockFits({ memoryMb: 12032 }, ml)], [true, true, false]);
  eq('memlock fits without a cap', [M.memlockFits({ memoryMb: 999999 }, null), M.memlockFits({ memoryMb: 999999 }, { capMb: 0 })], [true, true]);
  eq('memlock max memory', [M.memlockMaxMemory(ml), M.memlockMaxMemory({ capMb: 12000 }), M.memlockMaxMemory(null)], [11776, 11264, Infinity]);
  eq('no suspend: a graphics card, or 3D running', [M.noSuspend(null, 'intel-shared'), M.noSuspend({ sandbox: 512 }, ''), M.noSuspend({ sandbox: 1 }, '')], [true, true, false]);
}

// ---- a card's ··· menu on the screen, never cut by the window ----
{
  const items = ['a', 'b', '-', 'c'].map((x) => (x === '-' ? x : { label: x }));
  eq('menu height', M.popMenuHeight(items), 14 + 30 * 3 + 11);
  eq('menu height: no items, nulls skipped', [M.popMenuHeight(null), M.popMenuHeight([null, { label: 'a' }])], [14, 44]);
  eq('menu below when it fits', M.menuSpot({ x: 100, y: 200, above: 160 }, 220, 300, { x: 0, y: 0, w: 1920, h: 1080 }), { left: 100, top: 200 });
  eq('menu too tall both ways: as low as fits', M.menuSpot({ x: 1150, y: 420, above: 380 }, 220, 450, { x: 0, y: 0, w: 1920, h: 860 }), { left: 1150, top: 402 });
  eq('menu above the button', M.menuSpot({ x: 10, y: 800, above: 760 }, 220, 450, { x: 0, y: 0, w: 1920, h: 1080 }), { left: 10, top: 310 });
  eq('menu with room neither way: as low as fits', M.menuSpot({ x: 10, y: 300, above: 260 }, 220, 900, { x: 0, y: 0, w: 1920, h: 1000 }), { left: 10, top: 92 });
  eq('menu kept off the right edge', M.menuSpot({ x: 1800, y: 10 }, 220, 100, { x: 0, y: 0, w: 1920, h: 1080 }).left, 1920 - 220 - 8);
  eq('menu kept off the left and top edges', M.menuSpot({ x: -20, y: -5 }, 220, 100, { x: 0, y: 0, w: 1920, h: 1080 }), { left: 8, top: 8 });
  const two = [{ x: 0, y: 0, w: 1920, h: 1080, main: true }, { x: 1920, y: 0, w: 1280, h: 720 }];
  eq('the display under a point', [M.displayUnder(two, 2000, 100).x, M.displayUnder(two, 2000, 900).x, M.displayUnder([], 1, 1)], [1920, 0, null]);
  eq('menu on the second, shorter display opens up there', M.menuSpot({ x: 3100, y: 600, above: 560 }, 220, 300, two[1]), { left: 1920 + 1280 - 220 - 8, top: 260 });
  eq('a pointer menu opens up from the pointer', M.menuSpot({ x: 500, y: 1000 }, 220, 300, { x: 0, y: 0, w: 1920, h: 1080 }), { left: 500, top: 700 });
}

// ---- the Keys panel (codes from linux/input-event-codes.h) ----
{
  const K = M.KEY;
  eq('evdev codes', [K.ctrl, K.alt, K.shift, K.meta, K.del, K.p, K.left, K.right, K.up, K.down, K.prtsc, K.esc, K.tab, K.f1, K.f10, K.f11, K.f12, K.t, K.s],
    [29, 56, 42, 125, 111, 25, 105, 106, 103, 108, 99, 1, 15, 59, 68, 87, 88, 20, 31]);
  eq('editing keys', [K.ins, K.home, K.end, K.pgup, K.pgdn, K.d, K.e, K.r, K.l], [110, 102, 107, 104, 109, 32, 18, 19, 38]);
  eq('Ctrl+Alt+Del is the same as the Power menu\'s', M.keyCombos(true)[0].codes, M.CTRL_ALT_DEL);
  const win = Object.fromEntries(M.keyCombos(true).map((c) => [c.id, c.codes]));
  eq('Windows: Win+P', win.project, [125, 25]);
  eq('Windows: Win+Shift+← / →', [win.moveLeft, win.moveRight], [[125, 42, 105], [125, 42, 106]]);
  eq('Windows: Ctrl+Shift+Esc, Win+Shift+S, PrintScreen', [win.taskMgr, win.snip, win.screenshot], [[29, 42, 1], [125, 42, 31], [99]]);
  eq('Windows: the whole list', Object.keys(win), ['cad', 'start', 'project', 'desktop', 'explorer', 'run', 'lock', 'taskView', 'switchApp', 'closeWin', 'taskMgr', 'moveLeft', 'moveRight', 'maximize', 'minimize', 'screenshot', 'snip']);
  const lin = Object.fromEntries(M.keyCombos(false).map((c) => [c.id, c.codes]));
  eq('Linux: the list', Object.keys(lin), ['cadLinux', 'activities', 'switchApp', 'closeWin', 'terminal', 'screenshot']);
  eq('Linux: Ctrl+Alt+T, Super', [lin.terminal, lin.activities], [[29, 56, 20], [125]]);
  eq('consoles: Ctrl+Alt+F1 … F7', [M.CONSOLE_KEYS.length, M.CONSOLE_KEYS[0].map((k) => K[k]), M.CONSOLE_KEYS[6].map((k) => K[k])], [7, [29, 56, 59], [29, 56, 65]]);
  eq('F keys and single keys all have codes', [...M.F_KEYS, ...M.SINGLE_KEYS, ...M.HELD_KEYS].every((k) => K[k] > 0), true);
  eq('labels', [M.comboLabel(['meta', 'shift', 'left'], true), M.comboLabel(['meta'], false), M.comboLabel(['ctrl', 'alt', 'f3'], false), M.comboLabel(['meta', 'p'], true, (x) => x.toUpperCase())],
    ['win+shift+←', 'super', 'ctrl+alt+F3', 'WIN+P']);
  // held keys
  let l = M.latchToggle([], 125);
  eq('latch: down at once', l, { latched: [125], send: [[125, 1]] });
  l = M.latchToggle(l.latched, 42);
  eq('latch: a second one', l, { latched: [125, 42], send: [[42, 1]] });
  eq('latch: again lets it go', M.latchToggle(l.latched, 42), { latched: [125], send: [[42, 0]] });
  eq('a modifier typed keeps them', M.latchKeyUp([29], 42), { latched: [29], send: [] });
  eq('the next key ends them, in reverse', M.latchKeyUp([125, 42], 25), { latched: [], send: [[42, 0], [125, 0]] });
  eq('a latched key the person let go of is up', M.latchKeyUp([29, 56], 29), { latched: [56], send: [] });
  eq('nothing latched: nothing to send', M.latchKeyUp([], 25), { latched: [], send: [] });
  eq('a combination: in order, reverse, then the held ones', M.comboSend([125, 25], [29]), { latched: [], send: [[125, 1], [25, 1], [25, 0], [125, 0], [29, 0]] });
  eq('a combination with a key already held doesn\'t press it twice', M.comboSend([125, 42, 105], [42]), { latched: [], send: [[125, 1], [105, 1], [105, 0], [125, 0], [42, 0]] });
  eq('a single key', M.comboSend([1]), { latched: [], send: [[1, 1], [1, 0]] });
  eq('release', M.latchRelease([29, 56, 42]), { latched: [], send: [[42, 0], [56, 0], [29, 0]] });
  eq('modifiers', [29, 97, 42, 54, 56, 100, 125, 126, 25, 1].map(M.isModifier), [true, true, true, true, true, true, true, true, false, false]);
}

console.log(`vm-model: ${n} checks, ${bad} failed`);
process.exit(bad ? 1 : 0);
