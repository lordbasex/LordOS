// pre.js — goes before the theme bundle in tests/machines_sheets_headless.sh (a test tool, never shipped): Machines sees a
// made-up library in the scene's home (its folders and app.json files are real, made by the script): "Ubuntu 24.04" off,
// "Base Debian" off and kept as a template, and a Tor group ("Whonix": its gateway and workstation, both off). Nothing is
// started or changed: every lord.vm call that would do something answers as if refused. It also keeps which text each node
// shows and where it hangs, so driver.js can find and click them.
(function () {
  const L = `${lord.fs.home()}/Machines`;
  const base = { arch: 'amd64', accel: 'kvm', diskGb: 64, network: 'shared', reason: 'none', error: '', graphics: '2d', display: 'window', firmware: 'uefi', shares: [], ports: [],
    mac: '52:54:00:12:34:56', cpuModel: 'host', dataDisks: 1, state: 'stopped', agent: true };
  const G = `${L}/Whonix.lordgroup`;
  const machines = [
    { ...base, dir: `${L}/Ubuntu 24.04.lordvm`, folder: 'Ubuntu 24.04.lordvm', id: '44444444-2222-4333-8444-555555555555', name: 'Ubuntu 24.04', vcpus: 4, memoryMb: 4096 },
    { ...base, dir: `${L}/Base Debian.lordvm`, folder: 'Base Debian.lordvm', id: '55555555-2222-4333-8444-555555555555', name: 'Base Debian', vcpus: 2, memoryMb: 2048 },
    { ...base, dir: `${G}/Gateway.lordvm`, folder: 'Gateway.lordvm', id: '77777777-2222-4333-8444-555555555555', name: 'Whonix Gateway', vcpus: 1, memoryMb: 1024,
      groupDir: G, role: 'gateway', groupOk: true, network: 'shared' },
    { ...base, dir: `${G}/Workstation.lordvm`, folder: 'Workstation.lordvm', id: '88888888-2222-4333-8444-555555555555', name: 'Whonix Workstation', vcpus: 2, memoryMb: 2048,
      groupDir: G, role: 'workstation', groupOk: true, network: 'link' },
  ];
  const group = { dir: G, name: 'Whonix', kind: 'whonix', members: [{ role: 'gateway', start: 0, stop: 1 }, { role: 'workstation', start: 1, stop: 0 }],
    basesMissing: [], basesThere: true, archiveThere: true, freshGateway: false, disposable: false };
  const realHost = lord.vm && lord.vm.host;
  lord.vm.host = () => ({ ...((realHost && realHost()) || {}), arch: 'amd64', kvm: true, kvmDevice: true, cpus: 8, cores: 4, ramMb: 16384, memMaxMb: 14336, libFreeBytes: 420e9,
    libTotalBytes: 1000e9, missing: { amd64: '', arm64: '' }, ready: true, display: true, no3d: 'gpu', nested: true, landlock: 6, board: '' });
  lord.vm.list = () => machines.map((m) => ({ ...m }));
  lord.vm.groups = () => [{ ...group }];
  lord.vm.stats = () => ({ diskBytes: 12e9, baseBytes: 0, cpuUsec: 0, memBytes: 1e9, ioBytes: 0, at: Date.now() });
  lord.vm.budget = () => ({ usedMb: 0, maxMb: 14336, freeMb: 14336, needMb: 0, running: 0, ok: true, pressure: 0 });
  lord.vm.sensors = () => ({ tempC: 50, fanRpm: -1, underVoltage: -1 });
  lord.vm.snaps = () => ({ list: [{ tag: 's1', name: 'Before the update', date: 1763900000, vmState: false }], max: 32, warn: 16 });
  lord.vm.cloneInfo = () => ({ fs: 'ext4', reflink: false, linked: true });
  lord.vm.gpus = () => [];
  for (const k of ['start', 'startDetached', 'set', 'clone', 'export', 'importExport', 'groupFresh', 'groupSet']) lord.vm[k] = () => 0;
  lord.vm.stop = () => false;
  lord.vm.exportInfo = () => null;
  const texts = new Map(), parent = new Map();
  const wrap = (name, fn) => { const real = lord[name]; lord[name] = (...a) => { fn(...a); return real(...a); }; };
  wrap('setText', (id, s) => texts.set(id, String(s)));
  wrap('appendChild', (p, c) => parent.set(c, p));
  wrap('insertBefore', (p, c) => parent.set(c, p));
  wrap('destroy', (id) => { texts.delete(id); parent.delete(id); });
  globalThis.__appScene = { texts, parent };
})();
