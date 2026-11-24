// pre.js — goes before the theme bundle in tests/machines_group_headless.sh (a test tool, never shipped): the Machines
// app sees a made-up library with one Whonix group (its two members, off) and a Debian machine, this computer with KVM,
// and the admin helper's answers for Whonix's release folder, that release's listing and its signed SHA-512 list (the
// files are made up here; nothing is downloaded or verified). Everything else is the real code. It also keeps which text
// each node shows and where it hangs, so driver.js can click them.
(function () {
  const G = '/home/scene/Machines/Whonix.lordgroup';
  const mem = (role) => ({ dir: `${G}/${role === 'gateway' ? 'Gateway' : 'Workstation'}.lordvm`, folder: role === 'gateway' ? 'Gateway.lordvm' : 'Workstation.lordvm',
    id: role === 'gateway' ? '0f8e2a6c-1b2d-4c3e-9a7b-112233445566' : '5a1b2c3d-4e5f-4a6b-8c7d-998877665544', name: `Whonix ${role === 'gateway' ? 'Gateway' : 'Workstation'}`,
    arch: 'amd64', accel: 'kvm', vcpus: 1, memoryMb: 2048, diskGb: 100, network: role === 'gateway' ? 'shared' : 'link', state: 'stopped', reason: 'none', error: '',
    groupDir: G, groupId: 'b7c1d2e3-f4a5-4b6c-8d7e-0f1a2b3c4d5e', groupName: 'Whonix', groupKind: 'whonix', profileLock: 'anonymity', role, primary: role === 'workstation',
    groupOk: true, disposable: false, agent: false, clipboard: 'off', shares: [], ports: [], graphics: '2d', display: 'window', firmware: 'bios' });
  const deb = { dir: '/home/scene/Machines/Debian 13.lordvm', folder: 'Debian 13.lordvm', id: '11111111-2222-4333-8444-555555555555', name: 'Debian 13', arch: 'amd64', accel: 'kvm',
    vcpus: 4, memoryMb: 4096, diskGb: 64, network: 'shared', state: 'stopped', reason: 'none', error: '', graphics: '3d', display: 'window', firmware: 'uefi', shares: [], ports: [] };
  const group = { dir: G, id: 'b7c1d2e3-f4a5-4b6c-8d7e-0f1a2b3c4d5e', name: 'Whonix', kind: 'whonix', release: '18.2.4.3', disposable: false, freshGateway: false, error: '',
    archiveSha512: 'e5'.repeat(64), basesThere: true,
    members: [{ role: 'gateway', dir: `${G}/Gateway.lordvm`, start: 1, stop: 2, primary: false }, { role: 'workstation', dir: `${G}/Workstation.lordvm`, start: 2, stop: 1, primary: true }] };
  const realHost = lord.vm && lord.vm.host;
  lord.vm.host = () => ({ ...((realHost && realHost()) || {}), arch: 'amd64', kvm: true, kvmDevice: true, cpus: 8, ramMb: 16384, memMaxMb: 12288, libFreeBytes: 420e9, libTotalBytes: 1000e9,
    missing: [], qemu: { amd64: true, arm64: true }, edk2: { amd64: true, arm64: true } });
  // a second group, damaged (19.9): its Workstation's folder and its Gateway's base are missing; Repair, Rename and
  // Delete's dialogs are shown on it and on Whonix (nothing runs: every lord-vm job is refused here)
  const G2 = '/home/scene/Machines/Tor Work.lordgroup';
  const group2 = { ...group, dir: G2, id: 'c8d2e3f4-a5b6-4c7d-8e9f-0a1b2c3d4e5f', name: 'Tor Work', basesThere: false, basesMissing: ['gateway'], archiveThere: false,
    members: [{ role: 'gateway', dir: `${G2}/Gateway.lordvm`, start: 1, stop: 2, primary: false }, { role: 'workstation', dir: `${G2}/Workstation.lordvm`, start: 2, stop: 1, primary: true }] };
  const gw2 = { ...mem('gateway'), dir: `${G2}/Gateway.lordvm`, id: '0f8e2a6c-1b2d-4c3e-9a7b-112233445577', name: 'Tor Work Gateway', groupDir: G2, groupId: group2.id, groupName: 'Tor Work' };
  lord.vm.list = () => [mem('gateway'), mem('workstation'), { ...deb }, gw2];
  lord.vm.groups = () => [JSON.parse(JSON.stringify(group)), JSON.parse(JSON.stringify(group2))];
  lord.vm.groupRenamePlan = (dir, name) => ({ problem: name.toLowerCase() === 'tor work' ? 'name-taken' : '', moves: true, folder: `${name}.lordgroup`, other: 'Tor Work' });
  lord.vm.stats = () => ({ diskBytes: 3.6e9, baseBytes: 3.1e9 });
  lord.vm.snaps = (dir) => ({ list: dir.includes('Workstation') ? [{ tag: 'a', name: 'Before banking' }] : [], max: 32, warn: 16 });   // js_vm.c f_snaps' shape
  lord.vm.job = () => null;
  lord.vm.gpus = () => [];
  // the admin helper: Whonix's releases folder, the release's listing and its SHA-512 list, as paths to files
  const FILES = {
    '/scene/releases': '<a href="18.2.1.9/">18.2.1.9/</a><a href="18.2.4.3/">18.2.4.3/</a>',
    '/scene/index': '<a href="Whonix-LXQt-18.2.4.3.Intel_AMD64.qcow2.libvirt.xz">x</a><a href="Whonix-LXQt-18.2.4.3.Intel_AMD64.qcow2.libvirt.xz.sha512sums">s</a>',
    '/scene/list': `${'e5'.repeat(64)}  Whonix-LXQt-18.2.4.3.Intel_AMD64.qcow2.libvirt.xz\n`,
  };
  const realRead = lord.fs.read;
  lord.fs.read = (p, ...a) => (Object.prototype.hasOwnProperty.call(FILES, p) ? FILES[p] : realRead(p, ...a));
  let aid = 9000;
  lord.admin.available = () => true;
  lord.admin.request = (action, arg) => {
    const id = ++aid;
    const parts = String(arg).split(' ');
    const out = action === 'vm-image-list' ? (parts.length === 2 ? '/scene/releases' : parts.length === 3 ? '/scene/index' : '/scene/list') : '';
    setTimeout(() => globalThis.__lordAdmin(id, out ? 'OK' : 'ERR', out || 'admin.unavailable'), 150);
    return id;
  };
  const texts = new Map(), parent = new Map();
  const wrap = (name, fn) => { const real = lord[name]; lord[name] = (...a) => { fn(...a); return real(...a); }; };
  wrap('setText', (id, s) => texts.set(id, String(s)));
  wrap('appendChild', (p, c) => parent.set(c, p));
  wrap('insertBefore', (p, c) => parent.set(c, p));
  wrap('destroy', (id) => { texts.delete(id); parent.delete(id); });
  globalThis.__groupScene = { texts, parent };
})();
