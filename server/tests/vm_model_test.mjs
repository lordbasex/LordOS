// vm_model_test.mjs — Machines' pure logic (apps/machines/model.js): state mapping, allowed actions, the New machine
// sheet's checks, letterbox and pointer scaling, the guest size on resize, the refresh rate and wheel steps.
//
//   node tests/vm_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const M = await import(path.join(here, '../../apps/machines/model.js'));
const { stateView, actionsFor, defaultsFor, validateDraft, createRequest, missingFor, letterbox, toPicture, guestSize, refreshRate, wheelSteps, windowTitle, folderOf, memLimit, budgetCheck, systemOf, versionOf, archOf, suggestName, profileSize, usage, summaryOf, doingOf, thumbSrc, closeChoice, snapName, snapSize, cloneMethod, storageRows, storageBar } = M;

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
eq('request import', createRequest(good), { name: 'Debian 13', arch: 'amd64', vcpus: 2, memoryMb: 1024, diskGb: 20, import: '/home/a/debian.qcow2' });
eq('request root password', createRequest({ ...good, rootPassword: 'x' }).rootPassword, 'x');
eq('no root password for an ISO', createRequest({ ...good, source: 'iso', path: '/a.iso', rootPassword: 'x' }).rootPassword, undefined);
eq('request iso', createRequest({ ...good, name: ' X ', source: 'iso', path: '/a.iso' }).iso, '/a.iso');
eq('missing packages', missingFor({ missing: { amd64: 'qemu-system-x86 ovmf qemu-utils', arm64: 'qemu-system-arm qemu-utils' } }, ['amd64', 'arm64']),
  ['qemu-system-x86', 'ovmf', 'qemu-utils', 'qemu-system-arm']);
eq('nothing missing', missingFor({ missing: { amd64: '' } }, ['amd64']), []);

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
eq('profile light', profileSize('light', { vcpusRecommended: 3, memMaxMb: 14160 }), { vcpus: 1, memoryMb: 1024, diskGb: 16 });
eq('profile dev small host', profileSize('development', { vcpusRecommended: 3, memMaxMb: 2897 }), { vcpus: 3, memoryMb: 2816, diskGb: 64 });
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
eq('storage bar', storageBar(st).map((b) => [b.key, b.bytes]), [['machines', 630], ['isos', 50], ['trash', 20], ['other', 0]]);
eq('storage bar no total', storageBar({}), []);

console.log(`vm-model: ${n} checks, ${bad} failed`);
process.exit(bad ? 1 : 0);
