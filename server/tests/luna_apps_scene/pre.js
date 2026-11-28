// pre.js — goes before the theme bundle in tests/app_scene_headless.sh luna_apps_scene (a test tool, never shipped): keeps
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
  const demo = () => ({ ...real(), user: 'demo', fullName: 'Alex Demo', hostname: 'demo-pc', cpu: 'Demo CPU @ 2.0GHz', machine: 'Demo PC', memTotalKB: 16 * 1024 * 1024, memAvailableKB: 9 * 1024 * 1024 });
  // System Information's report: the machine, the processor, the graphics, the memory and the drives are demo ones (the
  // real names, serials and addresses are kept only for the driver's check)
  try {
    const si = lord.sysinfo;
    if (si && typeof si.report === 'function') {
      const rr = si.report() || {};
      const keep = [];
      const walk = (o, d) => { if (!o || d > 5) return; if (typeof o === 'string') { if (o.length > 5) keep.push(o); return; } if (typeof o === 'object') for (const v of Object.values(o)) walk(v, d + 1); };
      // what identifies this one computer: serial numbers, UUIDs, hardware (MAC) addresses, the machine's own product name
      const pick = (o, keys) => (o ? keys.map((k) => o[k]) : []);
      walk([...pick(rr.machine, ['serial', 'uuid', 'product', 'boardSerial', 'chassisSerial']), ...(rr.storage || []).flatMap((x) => pick(x, ['serial', 'wwn', 'uuid'])),
        ...(rr.network || []).flatMap((x) => pick(x, ['mac', 'address', 'permAddress'])), ...(rr.gpus || []).flatMap((x) => pick(x, ['serial', 'uuid']))], 0);
      globalThis.__sceneReal.hardware = keep.filter((v) => !/^(true|false|unknown|desktop|laptop|connected|disconnected|amdgpu|i915|nvidia|nouveau|radeon|ext4|vfat|btrfs|ethernet|wifi|loopback)$/i.test(v));
      const demoReport = () => {
        const r = si.report() || {};
        return { ...r, machine: { ...(r.machine || {}), vendor: 'Demo', product: 'Demo PC', version: '', serial: '', board: 'Demo board', bios: 'Demo BIOS' },
          cpu: { ...(r.cpu || {}), model: 'Demo CPU @ 2.0GHz' },
          gpus: (r.gpus || []).slice(0, 1).map((g) => ({ ...g, name: 'Demo Graphics', vendor: 'Demo', product: 'Demo Graphics', device: 'Demo Graphics', model: 'Demo Graphics', pciId: '', serial: '' })),
          memory: { ...(r.memory || {}), totalKB: 16 * 1024 * 1024, modules: [] },
          storage: [{ name: 'sda', model: 'Demo SSD 512', size: 512e9, bytes: 512e9, kind: 'ssd', transport: 'sata', serial: 'DEMO0001' }],
          displays: (r.displays || []).map((d, i) => ({ ...d, name: `Demo Display ${i + 1}`, serial: '', edidSerial: '', vendor: 'Demo', product: '' })),
          network: [], usb: [], pci: [], peripherals: [] };
      };
      try { lord.sysinfo = new Proxy(si, { get: (o, k) => (k === 'report' ? demoReport : o[k]) }); } catch (e) { /* stays real */ }
    }
  } catch (e) { /* no report */ }
  try { sys.info = demo; } catch (e) { /* below */ }
  if (sys.info !== demo) { try { Object.defineProperty(sys, 'info', { value: demo, configurable: true, writable: true }); } catch (e) { /* stays real */ } }
  // the computer's disks (models, serial numbers, labels) are its own: the scenes show two demo disks, and the real
  // serials and labels are kept only for the driver's check
  try {
    const d = lord.disks;
    if (d && typeof d.list === 'function') {
      const realDisks = d.list() || [];
      // serial numbers, WWNs and UUIDs identify the machine; a label counts unless it is a word every system has
      const COMMON = /^(lordos|efi|esp|boot|root|home|swap|system|data|recovery|windows|linux|ubuntu|debian|basic data partition|microsoft reserved partition|biosboot)$/i;
      globalThis.__sceneReal.disks = realDisks.flatMap((x) => [x.serial, x.wwn, ...(x.partitions || []).flatMap((p) => [p.uuid, ...[p.label, p.partlabel].filter((l) => l && !COMMON.test(l))])])
        .filter((v) => typeof v === 'string' && v.length > 3);
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
  // the system journal and LordOS's log files hold user names, paths and commands: the scenes show none
  try {
    const L = lord.logs;
    if (L) lord.logs = new Proxy(L, { get: (o, k) => (k === 'files' ? () => [] : k === 'take' ? () => [] : k === 'tail' ? () => '' : o[k]) });
  } catch (e) { /* stays real */ }
  // Monitor's processor name and the home location its map would use
  try {
    const M = lord.monitor;
    if (M && typeof M.info === 'function') lord.monitor = new Proxy(M, { get: (o, k) => (k === 'info' ? () => ({ ...(o.info() || {}), cpuModel: 'Demo CPU @ 2.0GHz', geo: null, home: null }) : o[k]) });
  } catch (e) { /* stays real */ }
  // the time zone says where someone lives: the Clock shows a neutral one
  try { if (lord.time) lord.time.zone = () => 'Etc/UTC'; } catch (e) { /* stays real */ }
})();
