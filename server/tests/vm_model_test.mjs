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
  eq('offsetRect without offset', M.offsetRect({ x: 1, y: 2, w: 3, h: 4 }, null), { x: 1, y: 2, w: 3, h: 4 });
  eq('minimized by our move: restored', [M.minimizedByUs(1000, 2000, false), M.minimizedByUs(1000, 5000, false), M.minimizedByUs(1000, 2000, true), M.minimizedByUs(0, 2000, false)], [true, false, false, false]);
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
eq('guest apps menu', M.guestAppItems([{ name: 'Zeta', token: 'z' }, { name: 'alpha', token: 'a' }, { name: 'dup', token: 'a' }, { name: '', token: 'x' }, null]).map((a) => a.token), ['a', 'z']);
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

console.log(`vm-model: ${n} checks, ${bad} failed`);
process.exit(bad ? 1 : 0);
