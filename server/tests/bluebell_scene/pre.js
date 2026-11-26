// pre.js — goes before the theme bundle in tests/app_scene_headless.sh bluebell_scene (a test tool, never shipped): keeps
// which text each node shows and where it hangs, so driver.js can find and click them.
(function () {
  const texts = new Map(), parent = new Map();
  const wrap = (name, fn) => { const real = lord[name]; lord[name] = (...a) => { fn(...a); return real(...a); }; };
  wrap('setText', (id, s) => texts.set(id, String(s)));
  wrap('appendChild', (p, c) => parent.set(c, p));
  wrap('insertBefore', (p, c) => parent.set(c, p));
  wrap('destroy', (id) => { texts.delete(id); parent.delete(id); });
  globalThis.__appScene = { texts, parent };
})();
// A demo identity and computer name (the lead's rule for screenshots that may go into docs or the site): the scene never
// shows the real account or host. The real ones are kept only so driver.js can check they appear nowhere on screen.
(function () {
  const sys = lord.system;
  const real = sys.info.bind(sys);
  try { const r = real(); globalThis.__sceneReal = { user: String(r.user || ''), fullName: String(r.fullName || ''), hostname: String(r.hostname || '') }; } catch (e) { globalThis.__sceneReal = {}; }
  const demo = () => ({ ...real(), user: 'demo', fullName: 'Alex Demo', hostname: 'demo-pc', cpu: 'Demo CPU @ 2.0GHz', machine: 'Demo PC' });
  try { sys.info = demo; } catch (e) { /* below */ }
  if (sys.info !== demo) { try { Object.defineProperty(sys, 'info', { value: demo, configurable: true, writable: true }); } catch (e) { /* stays real */ } }
  // the computer's disks (models, serial numbers, labels) are its own: the scenes show two demo disks, and the real
  // serials and labels are kept only for the driver's check
  try {
    const d = lord.disks;
    if (d && typeof d.list === 'function') {
      const realDisks = d.list() || [];
      globalThis.__sceneReal.disks = realDisks.flatMap((x) => [x.serial, x.model, x.wwn, ...(x.partitions || []).flatMap((p) => [p.label, p.uuid, p.partlabel])]).filter((v) => typeof v === 'string' && v.length > 3);
      const DEMO = [
        { name: 'sda', model: 'Demo SSD 512', serial: 'DEMO0001', size: 512e9, removable: false, system: true, transport: 'sata', table: 'gpt', rota: false, partitions: [
          { name: 'sda1', number: 1, label: 'EFI', size: 5e8, fstype: 'vfat', mounts: ['/boot/efi'] }, { name: 'sda2', number: 2, label: 'LordOS', size: 5.1e11, fstype: 'ext4', mounts: ['/'] }] },
        { name: 'sdb', model: 'Demo USB Stick', serial: 'DEMO0002', size: 32e9, removable: true, transport: 'usb', table: 'dos', partitions: [
          { name: 'sdb1', number: 1, label: 'STICK', size: 32e9, fstype: 'exfat', mounts: [] }] },
      ];
      const fake = new Proxy(d, { get: (o, k) => (k === 'list' ? () => DEMO.map((x) => ({ ...x, partitions: x.partitions.map((p) => ({ ...p })) })) : o[k]) });
      try { lord.disks = fake; } catch (e) { /* stays real */ }
    }
  } catch (e) { /* no disks */ }
  // the time zone says where someone lives: the Clock shows a neutral one
  try { if (lord.time) lord.time.zone = () => 'Etc/UTC'; } catch (e) { /* stays real */ }
})();
