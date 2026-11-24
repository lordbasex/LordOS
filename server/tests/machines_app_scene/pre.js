// pre.js — goes before the theme bundle in tests/machines_app_headless.sh (a test tool, never shipped): the Machines app
// sees a made-up library (Debian and Fedora running, Debian's guest tools connected and its disk busy, Windows 11 off),
// a computer whose kernel has no Landlock, that is on battery and short of memory, and a budget that refuses Windows 11's
// start beside the two running. Nothing is started or changed. It also keeps which text each node shows and where it
// hangs, so driver.js can click them.
(function () {
  const L = '/home/scene/Machines';
  const base = { arch: 'amd64', accel: 'kvm', diskGb: 64, network: 'shared', reason: 'none', error: '', graphics: '2d', display: 'window', firmware: 'uefi', shares: [], ports: [] };
  const deb = { ...base, dir: `${L}/Debian 13.lordvm`, folder: 'Debian 13.lordvm', id: '11111111-2222-4333-8444-555555555555', name: 'Debian 13', vcpus: 4, memoryMb: 4096,
    state: 'running', agentState: 'connected', vmPid: 4242 };
  const fed = { ...base, dir: `${L}/Fedora.lordvm`, folder: 'Fedora.lordvm', id: '22222222-2222-4333-8444-555555555555', name: 'Fedora', vcpus: 2, memoryMb: 4096,
    state: 'running', agentState: 'off', vmPid: 4343 };
  const win = { ...base, dir: `${L}/Windows 11.lordvm`, folder: 'Windows 11.lordvm', id: '33333333-2222-4333-8444-555555555555', name: 'Windows 11', vcpus: 4, memoryMb: 8192,
    state: 'stopped', windowsDevices: true, firmware: 'uefi', tpm: true, secureBoot: true };
  const realHost = lord.vm && lord.vm.host;
  lord.vm.host = () => ({ ...((realHost && realHost()) || {}), arch: 'amd64', kvm: true, kvmDevice: true, cpus: 8, cores: 4, ramMb: 16384, memMaxMb: 14336, libFreeBytes: 420e9,
    libTotalBytes: 1000e9, missing: { amd64: '', arm64: '' }, ready: true, display: true, no3d: 'gpu', nested: true, landlock: 0, board: '' });
  lord.vm.list = () => [{ ...deb }, { ...fed }, { ...win }];
  lord.vm.groups = () => [];
  let io = 0, cpu = 0;
  lord.vm.stats = (dir) => {
    if (!dir.includes('Debian') && !dir.includes('Fedora')) return { diskBytes: 30e9, baseBytes: 0 };
    io += dir.includes('Debian') ? 4e6 : 0; cpu += 900000;
    return { diskBytes: 12e9, baseBytes: 0, cpuUsec: cpu, memBytes: 3.1e9, ioBytes: io, at: Date.now() };
  };
  lord.vm.budget = (dir) => (dir && String(dir).includes('Windows')
    ? { usedMb: 8960, maxMb: 14336, freeMb: 5376, needMb: 8576, running: 2, ok: false, pressure: 14.2 }
    : { usedMb: 8960, maxMb: 14336, freeMb: 5376, needMb: 0, running: 2, ok: true, pressure: 14.2 });
  lord.vm.sensors = () => ({ tempC: 67, fanRpm: 2400, underVoltage: -1 });
  lord.vm.snaps = () => ({ list: [], max: 32, warn: 16 });
  lord.vm.job = () => null;
  lord.vm.gpus = () => [];
  lord.vm.start = () => 0;
  lord.vm.startDetached = () => 0;
  lord.vm.stop = () => true;
  // on battery
  if (lord.power) {
    const realState = lord.power.state;
    lord.power.state = () => ({ ...((realState && realState()) || {}), present: true, discharging: true, percent: 64 });
  }
  const texts = new Map(), parent = new Map();
  const wrap = (name, fn) => { const real = lord[name]; lord[name] = (...a) => { fn(...a); return real(...a); }; };
  wrap('setText', (id, s) => texts.set(id, String(s)));
  wrap('appendChild', (p, c) => parent.set(c, p));
  wrap('insertBefore', (p, c) => parent.set(c, p));
  wrap('destroy', (id) => { texts.delete(id); parent.delete(id); });
  globalThis.__appScene = { texts, parent };
})();
