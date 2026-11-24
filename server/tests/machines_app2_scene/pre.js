// pre.js — goes before the theme bundle in tests/machines_app2_headless.sh (a test tool, never shipped): Machines sees a
// made-up library in the scene's home (its folders and app.json files are real, made by the script): "Ubuntu 24.04" off
// with tags and a color of its own, "Base Debian" off and kept as a template, "Fedora" running. Nothing is started or
// changed; lord.vm.isoDetect and lord.vm.job stay real (the Detect step runs lord-vm --iso-detect on the scene's disc).
// It also keeps which text each node shows and where it hangs, so driver.js can click them.
(function () {
  const L = `${lord.fs.home()}/Machines`;
  const base = { arch: 'amd64', accel: 'kvm', diskGb: 64, network: 'shared', reason: 'none', error: '', graphics: '2d', display: 'window', firmware: 'uefi', shares: [], ports: [],
    mac: '52:54:00:12:34:56', cpuModel: 'host', dataDisks: 0 };
  const ubu = { ...base, dir: `${L}/Ubuntu 24.04.lordvm`, folder: 'Ubuntu 24.04.lordvm', id: '44444444-2222-4333-8444-555555555555', name: 'Ubuntu 24.04', vcpus: 4, memoryMb: 4096, state: 'stopped' };
  const deb = { ...base, dir: `${L}/Base Debian.lordvm`, folder: 'Base Debian.lordvm', id: '55555555-2222-4333-8444-555555555555', name: 'Base Debian', vcpus: 2, memoryMb: 2048, state: 'stopped' };
  const fed = { ...base, dir: `${L}/Fedora.lordvm`, folder: 'Fedora.lordvm', id: '66666666-2222-4333-8444-555555555555', name: 'Fedora', vcpus: 2, memoryMb: 4096,
    state: 'running', agentState: 'off', vmPid: 4343 };
  const realHost = lord.vm && lord.vm.host;
  lord.vm.host = () => ({ ...((realHost && realHost()) || {}), arch: 'amd64', kvm: true, kvmDevice: true, cpus: 8, cores: 4, ramMb: 16384, memMaxMb: 14336, libFreeBytes: 420e9,
    libTotalBytes: 1000e9, missing: { amd64: '', arm64: '' }, ready: true, display: true, no3d: 'gpu', nested: true, landlock: 6, board: '' });
  lord.vm.list = () => [{ ...ubu }, { ...deb }, { ...fed }];
  lord.vm.groups = () => [];
  lord.vm.stats = () => ({ diskBytes: 12e9, baseBytes: 0, cpuUsec: 0, memBytes: 1e9, ioBytes: 0, at: Date.now() });
  lord.vm.budget = () => ({ usedMb: 4480, maxMb: 14336, freeMb: 9856, needMb: 0, running: 1, ok: true, pressure: 0 });
  lord.vm.sensors = () => ({ tempC: 50, fanRpm: -1, underVoltage: -1 });
  lord.vm.snaps = () => ({ list: [], max: 32, warn: 16 });
  lord.vm.gpus = () => [];
  lord.vm.start = () => 0;
  lord.vm.startDetached = () => 0;
  lord.vm.stop = () => true;
  lord.vm.set = () => 0;
  const texts = new Map(), parent = new Map();
  const wrap = (name, fn) => { const real = lord[name]; lord[name] = (...a) => { fn(...a); return real(...a); }; };
  wrap('setText', (id, s) => texts.set(id, String(s)));
  wrap('appendChild', (p, c) => parent.set(c, p));
  wrap('insertBefore', (p, c) => parent.set(c, p));
  wrap('destroy', (id) => { texts.delete(id); parent.delete(id); });
  globalThis.__appScene = { texts, parent };
})();
