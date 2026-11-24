// pre.js — goes before the theme bundle in tests/monitor_headless.sh (a test tool, never shipped): Monitor → Processes
// shows a made-up process list (a browser with its helpers, a web app, a Flatpak app, a LordOS app, the Terminal's
// programs, a program from the home folder, one updated on disk, a machine) instead of the machine's own, with the
// owners and repositories fixed, so the screenshots are the same everywhere. lord.monitor.signal is a no-op: the scene
// never signals a real process. Everything else is the real code. It also keeps which text each node shows and where
// it hangs, so driver.js can click them.
(function () {
  const MB = 1024 * 1024;
  let st = 1200000;
  const P = (pid, ppid, name, uid, o = {}) => ({ pid, ppid, name, uid, user: o.user || 'alex', cmd: name, exe: `/usr/bin/${name}`, state: 'S', threads: 1, cpu: 0,
    rss: 10 * MB, start: (st += 37) / 100, st, self: false, prot: null, ...o });
  const CHROME = { app: 'desktop:google-chrome', appName: 'Google Chrome', exe: '/opt/google/chrome/chrome' };
  const fake = (me) => [
    P(1, 0, 'systemd', 0, { user: 'root', prot: 'init', exe: '/usr/lib/systemd/systemd' }),
    P(410, 1, 'lord-sessiond', 0, { user: 'root', prot: 'system', exe: '/usr/lib/lordos/lord-sessiond', rss: 9 * MB }),
    P(520, 410, 'lord-server', me, { app: 'lordos', self: true, prot: 'session', exe: '/usr/lib/lordos/lord-server', rss: 412 * MB, cpu: 0.031, threads: 23 }),
    P(531, 520, 'lord-media', me, { app: 'lordos', prot: 'system', exe: '/usr/lib/lordos/lord-media', rss: 38 * MB }),
    P(540, 520, 'lord-server', me, { app: 'lpk:mail', exe: '/usr/lib/lordos/lord-server', rss: 96 * MB, cpu: 0.004, threads: 6 }),
    P(2100, 410, 'chrome', me, { ...CHROME, rss: 512 * MB, pss: 380 * MB, cpu: 0.064, threads: 41 }),
    P(2106, 2100, 'chrome', me, { ...CHROME, cmd: 'chrome --type=zygote', rss: 61 * MB, threads: 3 }),
    P(2107, 2106, 'chrome', me, { ...CHROME, cmd: 'chrome --type=zygote', rss: 22 * MB, threads: 2 }),
    P(2131, 2107, 'chrome', me, { ...CHROME, cmd: 'chrome --type=renderer', rss: 310 * MB, pss: 240 * MB, cpu: 0.211, threads: 19 }),
    P(2133, 2107, 'chrome', me, { ...CHROME, cmd: 'chrome --type=renderer', rss: 188 * MB, pss: 130 * MB, cpu: 0.027, threads: 17 }),
    P(2135, 2107, 'chrome', me, { ...CHROME, cmd: 'chrome --type=renderer', rss: 96 * MB, cpu: 0.003, threads: 14 }),
    P(2110, 2100, 'chrome', me, { ...CHROME, cmd: 'chrome --type=gpu-process', rss: 141 * MB, cpu: 0.018, threads: 11 }),
    P(2104, 1, 'chrome_crashpad', me, { ...CHROME, exe: '/opt/google/chrome/chrome_crashpad_handler', rss: 4 * MB, threads: 3 }),
    P(2400, 410, 'chrome', me, { app: 'desktop:chrome-hnpfjngllnobngcgfapefoaidbinmjnm-Default', appName: 'WhatsApp Web', exe: '/opt/google/chrome/chrome', rss: 220 * MB, cpu: 0.012, threads: 22 }),
    P(2600, 410, 'bwrap', me, { app: 'flatpak:org.gimp.GIMP', appName: 'GNU Image Manipulation Program', exe: '/usr/bin/bwrap', rss: 2 * MB }),
    P(2601, 2600, 'gimp-2.10', me, { app: 'flatpak:org.gimp.GIMP', appName: 'GNU Image Manipulation Program', exe: '/app/bin/gimp-2.10', rss: 182 * MB, cpu: 0.009, threads: 12 }),
    P(2700, 520, 'lord-web', me, { app: 'desktop:lordos-web-calendar', exe: '/usr/lib/lordos/lord-web', rss: 58 * MB, threads: 9 }),
    P(2701, 2700, 'WebKitWebProces', me, { app: 'desktop:lordos-web-calendar', exe: '/usr/lib/x86_64-linux-gnu/webkitgtk-6.0/WebKitWebProcess', rss: 151 * MB, cpu: 0.015, threads: 16 }),
    P(3000, 520, 'bash', me, { app: 'lordos:terminal', exe: '/usr/bin/bash', rss: 5 * MB }),
    P(3011, 3000, 'claude', me, { app: 'lordos:terminal', exe: '/home/alex/.local/share/claude/versions/2.1/claude', rss: 268 * MB, cpu: 0.042, threads: 18 }),
    P(3020, 3011, 'sh', me, { app: 'lordos:terminal', exe: '/usr/bin/dash', rss: 1 * MB }),
    P(3021, 3020, 'sleep', me, { app: 'lordos:terminal', exe: '/usr/bin/sleep', rss: 1 * MB }),
    P(3100, 520, 'bash', me, { app: 'lordos:terminal', exe: '/usr/bin/bash', rss: 5 * MB }),
    P(3101, 3100, 'make', me, { app: 'lordos:terminal', exe: '/usr/bin/make', rss: 9 * MB, cpu: 0.02 }),
    P(3102, 3101, 'cc1', me, { app: 'lordos:terminal', exe: '/usr/libexec/gcc/x86_64-linux-gnu/14/cc1', rss: 120 * MB, cpu: 0.93 }),
    P(3300, 1, 'syncd', me, { exe: '/home/alex/bin/syncd', rss: 14 * MB, cpu: 0.006, threads: 4 }),
    P(3400, 1, 'pipewire', me, { prot: 'system', exe: '/usr/bin/pipewire (deleted)', rss: 18 * MB, cpu: 0.008, threads: 3 }),
    P(3500, 410, 'qemu-system-x86', me, { app: 'desktop:lordos-vm-7f3a', exe: '/usr/bin/qemu-system-x86_64', rss: 2140 * MB, cpu: 0.12, threads: 9 }),
    ...big(me),
  ];
  // MONITOR_BIG (tests/monitor_freeze_headless.sh): a Terminal with 240 programs (shells, node, a build), the size of
  // the group whose opening froze the desktop (0.8.9.6)
  function big(me) {
    if (!globalThis.MONITOR_BIG) return [];
    const out = [];
    const names = ['bash', 'node', 'claude', 'sh', 'make', 'cc1', 'python3', 'git', 'ssh', 'rg'];
    for (let i = 0; i < 24; i++) {
      const sh = 5000 + i * 20;
      out.push(P(sh, 520, 'bash', me, { app: 'lordos:terminal', exe: '/usr/bin/bash', rss: 5 * MB }));
      for (let k = 1; k < 10; k++) {
        const n = names[k];
        out.push(P(sh + k, k < 4 ? sh + k - 1 : sh + 3, n, me, { app: 'lordos:terminal', exe: `/usr/bin/${n}`, rss: k * 7 * MB, cpu: ((i * k) % 7) / 300, threads: 1 + (k % 4) }));
      }
    }
    return out;
  }
  const OWN = {
    '/usr/lib/lordos/lord-sessiond': 'lordos', '/usr/lib/lordos/lord-server': 'lordos', '/usr/lib/lordos/lord-media': 'lordos', '/usr/lib/lordos/lord-web': 'lordos-web',
    '/opt/google/chrome/chrome': 'google-chrome-stable', '/opt/google/chrome/chrome_crashpad_handler': 'google-chrome-stable', '/usr/bin/bwrap': 'bubblewrap',
    '/usr/lib/x86_64-linux-gnu/webkitgtk-6.0/WebKitWebProcess': 'libwebkitgtk-6.0-4', '/usr/bin/bash': 'bash', '/usr/bin/dash': 'dash', '/usr/bin/sleep': 'coreutils',
    '/usr/bin/make': 'make', '/usr/libexec/gcc/x86_64-linux-gnu/14/cc1': 'cpp-14-x86-64-linux-gnu', '/usr/bin/pipewire': 'pipewire-bin',
    '/usr/bin/qemu-system-x86_64': 'qemu-system-x86', '/usr/lib/systemd/systemd': 'systemd',
  };
  // made once: the same processes (PID and start time) in every sample
  const realProcs = lord.monitor.processes;
  let made = null;
  lord.monitor.processes = (lang) => {
    const r = realProcs(lang);
    if (!r) return null;
    if (!made) made = fake((r.list.find((p) => p.self) || {}).uid ?? 1000);
    const list = made.map((p) => ({ ...p }));
    return { ...r, total: list.length, list };
  };
  lord.monitor.signal = () => null;
  // Get Info of a made-up process: what procctl.c's JSON says, made from the list
  let watch = null;
  lord.monitor.detail = (pid, st) => { watch = pid ? { pid, st } : null; return true; };
  lord.monitor.detailInfo = () => {
    const p = watch && made && made.find((x) => x.pid === watch.pid);
    if (!p) return null;
    const scope = p.app === 'lordos:terminal' ? 'lordos-net-terminal.scope' : /^desktop:(.+)$/.test(p.app || '') ? `app-lordos-${p.app.slice(8)}.scope` : 'session.scope';
    return { pid: p.pid, gone: false, ppid: p.ppid, pgid: p.pid, sid: 520, uid: p.uid, euid: p.uid, cmdline: [p.exe, ...(p.cmd.split(' ').slice(1))], exe: p.exe,
      rss: p.rss, vsize: p.rss * 4, shared: p.rss / 4, private: (p.rss * 3) / 4, pss: p.rss * 0.8, swap: 0, rssAnon: p.rss * 0.7, rssFile: p.rss * 0.25, rssShmem: p.rss * 0.05,
      utime: 1240, stime: 310, hz: 100, threads: p.threads, vctx: 18200, nvctx: 940, minflt: 51200, majflt: 12, startMs: Date.now() - 3 * 3600000, nice: 0, prio: 20,
      cgroup: `/user.slice/user-1000.slice/${scope}`, isolated: false, seccomp: 0, nnp: 0, fds: null, fdTotal: 0 };
  };
  lord.monitor.exeOwners = (paths) => Object.fromEntries(paths.map((p) => [p, OWN[p] || '']));
  const realInfo = lord.pkg && lord.pkg.info;
  if (lord.pkg) lord.pkg.info = (name, o) => (name === 'google-chrome-stable' ? { name, origin: 'Google LLC' } : OWN && Object.values(OWN).includes(name) ? { name, origin: 'Debian' } : realInfo(name, o));
  const realVm = lord.vm && lord.vm.list;
  if (lord.vm) lord.vm.list = () => [{ id: '7f3a', name: 'Debian 13' }, ...((realVm && realVm()) || [])];
  if (lord.webapps && typeof lord.webapps.list === 'function') {
    const realWeb = lord.webapps.list;
    lord.webapps.list = () => [{ id: 'calendar', name: 'Calendar web', origin: 'https://calendar.example.org', profiles: [{ id: 'default', name: 'Default' }] },
      ...((realWeb() || []).filter((e) => e.id !== 'calendar'))];
  }
  const texts = new Map(), parent = new Map();
  const wrap = (name, fn) => { const real = lord[name]; lord[name] = (...a) => { fn(...a); return real(...a); }; };
  wrap('setText', (id, s) => texts.set(id, String(s)));
  wrap('appendChild', (p, c) => parent.set(c, p));
  wrap('insertBefore', (p, c) => parent.set(c, p));
  wrap('destroy', (id) => { texts.delete(id); parent.delete(id); });
  globalThis.__monitorScene = { texts, parent };
})();
