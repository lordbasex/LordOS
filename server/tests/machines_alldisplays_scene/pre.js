// pre.js — goes before the theme bundle in tests/machines_alldisplays_headless.sh (a test tool, never shipped): All
// Displays (docs/VIRTUALIZATION.md 5.4) in a headless lord-server. The one headless frame is told it is two displays side
// by side (each half of it), and a made-up Debian machine with two outputs runs: lord.vm.start, attach, poll and
// allDisplays are stand-ins that record what the app asks (globalThis.__adScene) and give both outputs the same picture
// (a still the driver takes of the desktop). Nothing is started or changed.
(function () {
  const W = lord.screen.width, H = lord.screen.height, half = Math.floor(W / 2);
  lord.screen.displays = [
    { id: 11, name: 'Left', x: 0, y: 0, w: half, h: H, scale: 100, main: true },
    { id: 12, name: 'Right', x: half, y: 0, w: W - half, h: H, scale: 100, main: false },
  ];
  const L = '/home/scene/Machines';
  const deb = { arch: 'amd64', accel: 'kvm', diskGb: 64, network: 'shared', reason: 'none', error: '', graphics: '2d', display: 'window', firmware: 'uefi', shares: [], ports: [],
    dir: `${L}/Debian 13.lordvm`, folder: 'Debian 13.lordvm', id: '11111111-2222-4333-8444-555555555555', name: 'Debian 13', vcpus: 4, memoryMb: 4096,
    state: 'running', agentState: 'off', vmPid: 4242, heads: 2, gpu: '', agent: false };
  const S = { picture: 0, calls: [], attached: [], closed: [] };
  globalThis.__adScene = S;
  lord.vm.list = () => [{ ...deb }];
  lord.vm.groups = () => [];
  lord.vm.budget = () => ({ usedMb: 4096, maxMb: 14336, freeMb: 10240, needMb: 0, running: 1, ok: true, pressure: 0 });
  lord.vm.stats = () => ({ diskBytes: 12e9, baseBytes: 0, cpuUsec: 1, memBytes: 1e9, ioBytes: 1, at: Date.now() });
  lord.vm.job = () => null;
  lord.vm.gpus = () => [];
  const sess = new Map();
  let next = 1;
  const poll = (s) => ({ state: 'running', reason: 'none', display: S.picture ? 'up' : 'connecting', media: S.picture || 0, w: s.head ? 800 : 1024, h: 600,
    vcpus: 4, memoryMb: 4096, sandbox: 1, cgroup: 1, agent: { state: 'off' }, ended: false, frames: 1, filesSeq: 0 });
  lord.vm.start = () => { const id = next++; sess.set(id, { head: 0 }); return id; };
  lord.vm.attach = (dir, o) => { const id = next++; const head = o && o.head ? o.head : 0; sess.set(id, { head }); S.attached.push(head); return id; };
  lord.vm.poll = (id) => (sess.has(id) ? poll(sess.get(id)) : null);
  lord.vm.allDisplays = (id, sizes) => { S.calls.push(sizes ? sizes.map((x) => x.join('x')).join(',') : 'off'); return true; };
  lord.vm.close = (id) => { if (sess.has(id)) S.closed.push(sess.get(id).head); sess.delete(id); };
  for (const f of ['rate', 'shown', 'resize', 'motion', 'button', 'wheel', 'key', 'release', 'protect', 'snapshot', 'cmd']) lord.vm[f] = () => true;
  const texts = new Map(), parent = new Map();
  const wrap = (name, fn) => { const real = lord[name]; lord[name] = (...a) => { fn(...a); return real(...a); }; };
  wrap('setText', (id, s) => texts.set(id, String(s)));
  wrap('appendChild', (p, c) => parent.set(c, p));
  wrap('insertBefore', (p, c) => parent.set(c, p));
  wrap('destroy', (id) => { texts.delete(id); parent.delete(id); });
  globalThis.__appScene = { texts, parent };
})();
