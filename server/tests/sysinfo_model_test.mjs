// sysinfo_model_test.mjs — System Information's model (apps/sysinfo/model.js, 0.8.5): the GPU layers,
// the reasons for CPU composition, the Compatibility panel and the Hardware sections. Run by
// tests/run_suites.sh (needs node).
import { evaluate, evaluateProbe, evaluateBench, LIMITS } from '../../apps/sysinfo/checks.js';
import { compositionReason, cardLayers, sessionLayers, compatibility, hardwareSections, piBoard, throttleFlags, piStatus, machineName, reportFileName, reportJson, parseMemoryLines, parseSerialLines, peripherals, firmwareDate } from '../../apps/sysinfo/model.js';

let n = 0, bad = 0;
const eq = (what, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${what}\n    got  ${g}\n    want ${w}`); }
};
const states = (rows) => Object.fromEntries(rows.map((r) => [r.id, r.state]));
const notes = (rows) => Object.fromEntries(rows.map((r) => [r.id, r.note]));

// ---- why the CPU composes ----
eq('gpu: no reason', compositionReason({ composition: 'gpu', gpuInit: 'on' }), null);
eq('session reasons win', ['headless', 'no-display', 'rotated', 'capture'].map((c) => compositionReason({ composition: c, gpuInit: 'on' })),
  ['headless', 'no-display', 'rotated', 'capture']);
eq('gpu-off: the init outcome', compositionReason({ composition: 'gpu-off', gpuInit: 'software' }), 'software');
eq('nothing known', compositionReason({}), 'unknown');

// ---- layers ----
const amd = { card: 'card0', bus: 'pci', vendorId: '1002', deviceId: '67df', vendor: 'AMD', device: 'RX 580', driver: 'amdgpu', renderNode: true, connectors: 2, connected: 1 };
eq('a display card', states(cardLayers(amd)), { device: 'yes', driver: 'yes', kms: 'yes', render: 'yes' });
eq('device name', cardLayers(amd)[0].vars.name, 'AMD RX 580');
const v3d = { card: 'card0', bus: 'platform', driver: 'v3d', renderNode: true, connectors: 0, connected: 0 };
eq('a render-only card (Pi 5 v3d)', [states(cardLayers(v3d)).kms, cardLayers(v3d)[2].note, cardLayers(v3d)[0].vars.name], ['no', 'renderOnly', '?:?']);
eq('no driver', states(cardLayers({ ...amd, driver: '' })).driver, 'no');

const on = { composition: 'gpu', gpuInit: 'on', renderer: 'AMD Radeon RX 580', renderNode: '/dev/dri/renderD128', dmabuf: true, scanouts: 12 };
eq('session on the GPU', states(sessionLayers(on)), { egl: 'yes', composition: 'yes', scanout: 'yes', dmabuf: 'yes', decode: 'unchecked', encode: 'unchecked', vulkan: 'unchecked' });
eq('scanout notes', [sessionLayers(on)[2].note, sessionLayers({ ...on, scanouts: 0 })[2].note], ['scanoutSeen', 'scanoutNone']);
eq('forced llvmpipe is marked software', sessionLayers({ ...on, renderer: 'llvmpipe (LLVM 19.1.7, 256 bits)' })[0].note, 'software');
const soft = { composition: 'gpu-off', gpuInit: 'software', renderer: 'llvmpipe (LLVM 19.1.7, 256 bits)', dmabuf: false, scanouts: 0 };
eq('software refused', [states(sessionLayers(soft)).egl, sessionLayers(soft)[0].note, sessionLayers(soft)[1].note], ['no', 'softwareRefused', 'software']);
eq('headless', notes(sessionLayers({ composition: 'headless', gpuInit: '' })).composition, 'headless');

// ---- compatibility ----
const vm = {
  machine: { vendor: 'QEMU', product: 'Standard PC', virtualization: 'kvm' },
  gpus: [{ card: 'card0', driver: 'virtio-pci', connectors: 1, connected: 1 }],
  displays: [{ connector: 'Virtual-1', nativeWidth: 1280, nativeHeight: 800 }],
  storage: [{ name: 'sda', bytes: 34e9 }],
  network: [{ kind: 'ethernet', driver: 'virtio_net', up: true }],
  audio: ['HDA Intel'], bluetooth: [], power: [],
  software: { landlockAbi: 6, bpfLsm: true, cgroup2: true },
  session: { composition: 'gpu-off', gpuInit: 'no-3d' },
};
const c = compatibility(vm);
eq('a VM', states(c), { gpu: 'limits', display: 'works', storage: 'works', network: 'works', wifi: 'absent', bluetooth: 'absent', audio: 'works', power: 'absent', isolation: 'works', suspend: 'unchecked' });
eq('the GPU note names the reason', notes(c).gpu, 'cpu.no-3d');
eq('counts', [c.find((r) => r.id === 'display').vars.count, c.find((r) => r.id === 'storage').vars.count], [1, 1]);

const laptop = {
  ...vm,
  gpus: [{ card: 'card0', driver: 'i915', connectors: 1, connected: 1, renderNode: true }],
  displays: [{ connector: 'eDP-1', nativeWidth: 0, mode: '1920x1200' }],
  network: [{ kind: 'wifi', driver: 'iwlwifi', up: false }, { kind: 'ethernet', driver: 'r8152', up: false }],
  bluetooth: [{ driver: 'btusb' }],
  power: [{ kind: 'ac', online: 0 }, { kind: 'battery', capacity: 76, health: 64 }],
  software: { landlockAbi: 0, bpfLsm: false, cgroup2: true },
  session: { composition: 'gpu', renderer: 'Mesa Intel(R) Xe Graphics' },
};
const l = compatibility(laptop);
eq('a laptop', states(l), { gpu: 'works', display: 'limits', storage: 'works', network: 'limits', wifi: 'works', bluetooth: 'works', audio: 'works', power: 'limits', isolation: 'limits', suspend: 'unchecked' });
eq('laptop notes', [notes(l).display, notes(l).network, notes(l).power, notes(l).isolation], ['noEdid', 'notConnected', 'batteryWorn', 'missing.landlock']);
eq('gpu note carries the renderer', l[0].vars.renderer, 'Mesa Intel(R) Xe Graphics');
eq('healthy battery', compatibility({ ...laptop, power: [{ kind: 'battery', health: 92 }] }).find((r) => r.id === 'power'), { id: 'power', state: 'works', note: 'batteryHealth', vars: { health: 92 } });
eq('Landlock unknown (a fixture) with the rest present', compatibility({ ...vm, software: { landlockAbi: null, bpfLsm: true, cgroup2: true } }).find((r) => r.id === 'isolation').state, 'works');
eq('a Wi-Fi card without a driver', compatibility({ ...vm, network: [{ kind: 'wifi', driver: '', up: false }] }).find((r) => r.id === 'wifi').note, 'noDriver');
eq('headless: displays not checked', compatibility({ ...vm, displays: [], session: { composition: 'headless' } }).find((r) => r.id === 'display'),
  { id: 'display', state: 'unchecked', note: 'headless', vars: {} });
eq('no GPU at all', notes(compatibility({ ...vm, gpus: [] })).gpu, 'noGpu');
eq('an empty report does not throw', compatibility({}).length, 10);

// ---- sections, Pi flags, names ----
eq('sections of a VM', hardwareSections({ ...vm, pci: [{}], usb: [] }), ['computer', 'cpu', 'memory', 'graphics', 'displays', 'storage', 'network', 'audio', 'peripherals', 'pci', 'software']);
// ---- serial numbers: only what the action's format allows ----
eq('serial lines', parseSerialLines(['serial system PF1ABC', 'serial board M1 2.3/x', 'serial device 10000000abcdef01']),
  [{ kind: 'system', value: 'PF1ABC' }, { kind: 'board', value: 'M1 2.3/x' }, { kind: 'device', value: '10000000abcdef01' }]);
eq('serial: other lines dropped', parseSerialLines(['mem 1|DDR4|1|1|a|b|c', 'serial chassis X1', 'serial system', 'serial system a|b', 'serial system a\nb', null]), []);
eq('serial: one per kind', parseSerialLines(['serial system A1', 'serial system B2']), [{ kind: 'system', value: 'A1' }]);
eq('serial: not a list', parseSerialLines(undefined), []);

// ---- peripherals: on screen only, no address ----
const btState = { devices: [
  { path: '/org/bluez/hci0/dev_AA_BB_CC_DD_EE_01', address: 'AA:BB:CC:DD:EE:01', name: 'MX Master 3', icon: 'input-mouse', paired: true, connected: false, battery: -1 },
  { path: '/org/bluez/hci0/dev_AA_BB_CC_DD_EE_02', address: 'AA:BB:CC:DD:EE:02', name: 'MX Keys', icon: 'input-keyboard', paired: true, connected: true, battery: 80 },
  { path: '/org/bluez/hci0/dev_AA_BB_CC_DD_EE_03', address: 'AA:BB:CC:DD:EE:03', name: 'Nearby phone', icon: 'phone', paired: false, connected: false, battery: -1 },
  { path: '/org/bluez/hci0/dev_AA_BB_CC_DD_EE_04', address: 'AA:BB:CC:DD:EE:04', name: 'AA-BB-CC-DD-EE-04', icon: '', paired: true, connected: false, battery: -1 },
  { path: '/org/bluez/hci0/dev_AA_BB_CC_DD_EE_05', address: 'AA:BB:CC:DD:EE:05', name: 'AA:BB:CC:DD:EE:05', icon: 'audio-headset', paired: false, connected: true, battery: 40 },
] };
const usbList = [{ name: 'Logitech USB Receiver', kind: 'receiver', connection: 'usb', battery: -1 }, { name: 'Wireless Mouse', kind: 'mouse', connection: 'receiver', battery: 64 }];
const merged = peripherals(usbList, btState);
eq('peripherals: names, kinds and order', merged.map((p) => [p.name, p.kind, p.connection, p.connected, p.battery]), [
  ['MX Keys', 'keyboard', 'bluetooth', true, 80], ['Wireless Mouse', 'mouse', 'receiver', true, 64], ['MX Master 3', 'mouse', 'bluetooth', false, null],
  ['Logitech USB Receiver', 'receiver', 'usb', true, null], ['', 'audio', 'bluetooth', true, 40], ['', 'other', 'bluetooth', false, null]]);
eq('peripherals: no address or path survives', /AA[:_-]BB|org\/bluez/.test(JSON.stringify(merged)), false);
eq('peripherals: nothing known', peripherals(null, null), []);
eq('peripherals: only the fields it builds', Object.keys(peripherals([{ name: 'x', kind: 'mouse', connection: 'usb', battery: -1, serial: 'S', address: 'A' }], null)[0]), ['name', 'kind', 'connection', 'battery', 'connected']);

// ---- firmware dates ----
eq('DMI date', firmwareDate('11/23/2023').toDateString(), new Date(2023, 10, 23).toDateString());
eq('bootloader date', firmwareDate('2026-09-25').toDateString(), new Date(2026, 8, 25).toDateString());
eq('not a date', [firmwareDate(''), firmwareDate('13/40/2023'), firmwareDate('2023'), firmwareDate(undefined)], [null, null, null, null]);

eq('a Pi adds its section', hardwareSections({ raspberryPi: { model: 'Raspberry Pi 5' } }).includes('raspberryPi'), true);
eq('throttle 0x50000', throttleFlags(0x50000), { underVoltageNow: false, cappedNow: false, throttledNow: false, softLimitNow: false,
  underVoltage: true, capped: false, throttled: true, softLimit: false });
eq('throttle unknown', [throttleFlags(null), throttleFlags(-1)], [null, null]);
// the Pi's rows: the LordOS kernel only has rpi_volt's alarm (no throttling row), older ones get_throttled
eq('pi: LordOS kernel, no alarm', piStatus({ underVoltageAlarm: false }), { power: 'powerOkRecent', throttling: null });
eq('pi: LordOS kernel, alarm', piStatus({ underVoltageAlarm: true }), { power: 'underVoltageRecent', throttling: null });
eq('pi: nothing readable', piStatus({ model: 'Raspberry Pi 5' }), { power: null, throttling: null });
eq('pi: no section', piStatus(null), { power: null, throttling: null });
eq('pi: old kernel, since boot', piStatus({ throttled: 0x50000 }), { power: 'underVoltageSeen', throttling: 'throttledSeen' });
eq('pi: old kernel, clear', piStatus({ throttled: 0 }), { power: 'powerOk', throttling: 'throttleOk' });
eq('pi: old kernel, now', piStatus({ throttled: 0x9 }), { power: 'underVoltageNow', throttling: 'throttledNow' });
eq('pi: both, sticky bits are recent', piStatus({ throttled: 0x20000, underVoltageAlarm: false }), { power: 'powerOkRecent', throttling: 'throttledRecent' });
eq('names', [machineName(vm), machineName({ machine: { model: 'Raspberry Pi 5 Model B Rev 1.0' } }), machineName({})], ['QEMU Standard PC', 'Raspberry Pi 5 Model B Rev 1.0', '']);

// ---- the Report tab ----
const taken = new Set(['lordos-report-2026-10-26.md', 'lordos-report-2026-10-26-2.md']);
eq('file names', [reportFileName('2026-10-26', 'md', (n) => taken.has(n)), reportFileName('2026-10-26', 'json', (n) => taken.has(n))],
  ['lordos-report-2026-10-26-3.md', 'lordos-report-2026-10-26.json']);
eq('a strange date never reaches the name', reportFileName('../../x', 'md', () => false), 'lordos-report-today.md');
eq('all taken', reportFileName('2026-10-26', 'md', () => true), null);
eq('json text', reportJson({ schema: 1, cpu: { cores: 8 } }), '{\n  "schema": 1,\n  "cpu": {\n    "cores": 8\n  }\n}\n');

// ---- memory modules ----
eq('module lines', parseMemoryLines(['mem 16384|DDR4|3200|2666|DIMM_A1|BANK 0|Kingston', 'mem 0||0|0|DIMM_B1||', 'mem 4096|RAM|0|0|DIMM 0||QEMU']), [
  { sizeMB: 16384, type: 'DDR4', speed: 3200, configured: 2666, slot: 'DIMM_A1', bank: 'BANK 0', maker: 'Kingston' },
  { sizeMB: 0, type: '', speed: 0, configured: 0, slot: 'DIMM_B1', bank: '', maker: '' },
  { sizeMB: 4096, type: 'RAM', speed: 0, configured: 0, slot: 'DIMM 0', bank: '', maker: 'QEMU' }]);
eq('bad lines dropped', parseMemoryLines(['fw x y', 'mem 1|DDR4|1|1|a|b', 'mem x|DDR4|1|1|a|b|c', 'mem 1|DDR4|1|1|a|b|c|d', `mem 1|DDR4|1|1|${'a'.repeat(40)}|b|c`, null]), []);
eq('not a list', parseMemoryLines(undefined), []);

// ---- checks (part 1) ----
const st = (rows) => Object.fromEntries(rows.map((r) => [r.id, r.state]));
const good = {
  report: { software: { landlockAbi: 6, bpfLsm: true, kernelConfig: { CONFIG_CGROUP_BPF: 'y' }, services: { 'lord-netperm': true } },
    session: { composition: 'gpu', renderer: 'AMD Radeon RX 580' } },
  facts: { uid: 1000, euid: 1000, cgroup: '/system.slice/lordos.service/lordos-session-1000/server', leafRoot: true, parentRoot: true, appScopes: 3, remoteListening: false },
  code: { state: 'verified', files: 9, refused: [] }, remote: { enabled: false }, stats: { rssKB: 71680 },
};
eq('a healthy system', st(evaluate(good)), { serverUser: 'pass', landlock: 'pass', netperm: 'pass', code: 'pass', remoteClosed: 'pass', cgroups: 'pass', gpu: 'pass', memory: 'info' });
eq('levels', evaluate(good).map((r) => r.level), ['configured', 'configured', 'configured', 'effective', 'effective', 'configured', 'configured', 'measured']);
const broken = {
  report: { software: { landlockAbi: 3, bpfLsm: false, kernelConfig: {}, services: { 'lord-netperm': false } }, session: { composition: 'gpu-off', gpuInit: 'no-3d' } },
  facts: { uid: 0, euid: 0, cgroup: '/system.slice/lordos.service/lordos-session-0/server', leafRoot: false, parentRoot: true, appScopes: 0, remoteListening: true },
  code: { state: 'verified', files: 9, refused: [{ path: 'x' }] }, remote: { enabled: false },
};
eq('a broken system', st(evaluate(broken)), { serverUser: 'fail', landlock: 'warn', netperm: 'fail', code: 'fail', remoteClosed: 'fail', cgroups: 'fail', gpu: 'warn' });
eq('broken notes', evaluate(broken).map((r) => r.note), ['serverUserRoot', 'landlockOld', 'netpermOff', 'codeRefused', 'remoteOffListening', 'cgroupsUser', 'gpuNot.no-3d']);
eq('remote on by choice is information', evaluate({ ...good, remote: { enabled: true }, facts: { ...good.facts, remoteListening: true } }).find((r) => r.id === 'remoteClosed'),
  { id: 'remoteClosed', paper: '§11.4', level: 'effective', state: 'info', note: 'remoteOnListening', vars: { port: LIMITS.remotePort } });
eq('no BPF LSM is a warning', evaluate({ ...good, report: { ...good.report, software: { ...good.report.software, bpfLsm: false } } }).find((r) => r.id === 'netperm').note, 'netpermNoLsm');
eq('headless is information', evaluate({ ...good, report: { ...good.report, session: { composition: 'headless' } } }).find((r) => r.id === 'gpu').state, 'info');
eq('development code is a warning', evaluate({ ...good, code: { state: 'development' } }).find((r) => r.id === 'code').state, 'warn');
eq('outside LordOS\'s session tree it is only information', evaluate({ ...good, facts: { ...good.facts, cgroup: '/user.slice/user-1000.slice/session-3.scope' } }).find((r) => r.id === 'cgroups'),
  { id: 'cgroups', paper: '§11.4', level: 'configured', state: 'info', note: 'cgroupsNotSession', vars: {} });
eq('nothing known', st(evaluate({})), { serverUser: 'unknown', landlock: 'unknown', netperm: 'unknown', code: 'unknown', remoteClosed: 'unknown', cgroups: 'unknown', gpu: 'warn' });

// ---- the Sandbox Probe ----
const tr_ = (ids, denied = true) => ids.map((id) => ({ id, denied, errno: denied ? 13 : 0 }));
const KER = ['fsHome', 'fsSystem', 'cgroupEscape', 'netInet', 'netUnix', 'ptrace', 'userns', 'mount', 'exec'];
const HEL = ['helperSocket', 'helperExec', 'helperFork', 'helperData'];
const BRO = ['brokerFsEscape', 'brokerClipboard', 'brokerFido', 'brokerMail'];
const okProbe = { probe: { v: 1, sandbox: 63, tries: [...tr_(KER), ...tr_(['ownData'], false), ...tr_(HEL), ...tr_(BRO)] },
  proc: { noNewPrivs: '1', seccomp: '2', capEff: '0000000000000000', cgroup: '/system.slice/lordos.service/lordos-session-1000/app-lordos-lpk-sysprobe.scope', memoryMax: '536870912' }, at: 1 };
eq('a tight sandbox', st(evaluateProbe(okProbe)), { appSandbox: 'pass', helperSandbox: 'pass', broker: 'pass', appScope: 'pass' });
eq('the cap in MB', evaluateProbe(okProbe).find((r) => r.id === 'appScope').vars.mb, 512);
const leaky = { ...okProbe, probe: { ...okProbe.probe, tries: okProbe.probe.tries.map((x) => (x.id === 'netInet' || x.id === 'helperExec' || x.id === 'brokerFido' ? { ...x, denied: false } : x)) } };
eq('leaks are named', evaluateProbe(leaky).map((r) => [r.id, r.state, r.vars.list || '']),
  [['appSandbox', 'fail', 'netInet'], ['helperSandbox', 'fail', 'helperExec'], ['broker', 'fail', 'brokerFido'], ['appScope', 'pass', '']]);
const missing = { ...okProbe, probe: { ...okProbe.probe, tries: okProbe.probe.tries.filter((x) => x.id !== 'ptrace') } };
eq('a try that never ran counts as got through', evaluateProbe(missing)[0].vars.list, 'ptrace');
eq('a broken probe is not a pass', evaluateProbe({ ...okProbe, probe: { ...okProbe.probe, tries: okProbe.probe.tries.map((x) => (x.id === 'ownData' ? { ...x, denied: true } : x)) } })[0].state, 'unknown');
eq('no seccomp seen from outside', evaluateProbe({ ...okProbe, proc: { ...okProbe.proc, seccomp: '0' } })[0].state, 'warn');
eq('no scope of its own', evaluateProbe({ ...okProbe, proc: { ...okProbe.proc, cgroup: '/user.slice/x.scope', memoryMax: 'max' } }).find((r) => r.id === 'appScope').note, 'scopeMissing');
eq('helper never ran', evaluateProbe({ ...okProbe, probe: { ...okProbe.probe, tries: [...tr_(KER), ...tr_(['ownData'], false), ...tr_(['helperRan'])] } }).find((r) => r.id === 'helperSandbox').state, 'unknown');
eq('garbage does not throw', evaluateProbe({ probe: { tries: [null, 3, { id: 5 }] } }).map((r) => r.state), ['unknown', 'fail', 'fail', 'unknown']);

// ---- the Test Bench's report ----
const bench = { tool: 'lordos-testbench', at: 1, fps: 59.9, presentation: true, presented: 600, discarded: 0, refreshHz: 60,
  latency: { n: 4, avgMs: 21.25 }, gl: { renderer: 'AMD Radeon RX 580', kind: 'HARDWARE GPU', drawMs: 0.4 },
  checks: [{ id: 'a', state: 'pass' }, { id: 'b', state: 'pass' }, { id: 'c', state: 'info' }] };
eq('a good bench', st(evaluateBench(bench)), { wayland: 'pass', renderer3d: 'pass', presented: 'info', latency: 'info' });
eq('a failing check is named', evaluateBench({ ...bench, checks: [...bench.checks, { id: 'clipboard round trip', state: 'fail' }] })[0].vars.list, 'clipboard round trip');
eq('software renderer warns, headless presents nothing', st(evaluateBench({ ...bench, presented: 0, discarded: 9, gl: { renderer: 'llvmpipe', kind: 'SOFTWARE (CPU)' }, latency: {} })),
  { wayland: 'pass', renderer3d: 'warn', presented: 'warn' });
eq('not a bench report', [evaluateBench(null), evaluateBench({ tool: 'x' }), evaluateBench({ tool: 'lordos-testbench' })], [null, null, null]);

// ---- a Raspberry Pi's revision code ----
eq('a Pi 5, 16 GB, from Sony UK', piBoard('e04171'), { product: 'Raspberry Pi 5 Model B', chip: 'BCM2712', maker: 'Sony UK', memoryGB: 16, version: '1.1' });
eq('an old-style or unreadable code says nothing', [piBoard('a02082x'.slice(0, 0)), piBoard('000e'), piBoard(null)], [null, null, null]);

console.log(`sysinfo-model-test: ${n - bad} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
