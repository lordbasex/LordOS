// pre.js — goes before the theme bundle in tests/gpu_headless.sh (a test tool, never shipped): Monitor → GPU shows
// made-up cards and processes instead of the machine's own (an RX 580 like the mockup's and an Intel UHD 630; a model
// runner, a screen recording, the browser, LordOS itself, a machine, the voice, Studio, Files; another user's ollama;
// Sentinela's engine), so the screenshots are the same everywhere. lord.monitor.gpu() returns what gpustat.c would,
// one second later on each call; the first 180 polls run 4 ms apart so the three minutes of history are full at once.
// Everything else is the real code.
(function () {
  const GIB = 1024 ** 3, MIB = 1024 ** 2;
  const realST = globalThis.setTimeout;
  let fast = 182;
  globalThis.setTimeout = (fn, ms, ...a) => realST(fn, ms === 1000 && typeof fn === 'function' && /\.gpu\(\)/.test(String(fn)) && fast-- > 0 ? 4 : ms, ...a);
  let k = 0;
  const wave = (base, amp, f, ph = 0) => Math.max(0, Math.min(1, base + amp * Math.sin(k * f + ph) + amp * 0.4 * Math.sin(k * f * 3.7 + ph)));
  const burst = (from, lvl) => (k >= from ? lvl : lvl * 0.04);
  // per process: [pid, name, app, appName, libs, model, shares { gfx, compute, dec, enc, sdma }, vram, gtt]
  const procs = () => [
    [48211, 'llama-server', 'lordos:terminal', 'Terminal', ['libvulkan.so.1', 'libvulkan_radeon.so', 'libggml-vulkan.so'], 'Qwen3-8B-Q4_K_M.gguf', { compute: burst(40, wave(0.7, 0.08, 0.21)), sdma: 0.01 }, 4.88 * GIB, 118 * MIB],
    [52007, 'lord-media', 'desktop:screen-recording', 'Screen recording', ['libva.so.2', 'radeonsi_drv_video.so', 'libEGL.so.1'], '', { gfx: 0.01, enc: burst(95, wave(0.14, 0.04, 0.4)) }, 96 * MIB, 42 * MIB],
    [2927, 'WPEWebProcess', 'desktop:web', 'Web', ['libEGL.so.1', 'radeonsi_dri.so', 'libva.so.2', 'radeonsi_drv_video.so'], '', { gfx: wave(0.09, 0.05, 0.3), dec: wave(0.12, 0.05, 0.17), sdma: 0.01 }, 288 * MIB, 67 * MIB],
    [1612, 'lord-server', 'lordos', '', ['libEGL.so.1', 'radeonsi_dri.so'], '', { gfx: wave(0.09, 0.03, 0.5), sdma: 0.01 }, 214 * MIB, 12 * MIB],
    [61190, 'qemu-system-x86_64', 'desktop:machines', 'Machines · Windows 11', ['libvulkan.so.1', 'libvulkan_radeon.so'], '', { gfx: wave(0.06, 0.03, 0.23) }, 512 * MIB, 24 * MIB],
    [57712, 'lord-studio', 'desktop:studio', 'LordOS Studio', ['libEGL.so.1', 'radeonsi_dri.so'], '', { gfx: 0.02 }, 38 * MIB, 21 * MIB],
    [3311, 'Files', 'desktop:files', 'Files', ['libEGL.so.1', 'radeonsi_dri.so'], '', { gfx: 0.002 }, 12 * MIB, 4 * MIB],
  ];
  const ns = new Map();
  const raw = () => {
    k++;
    const list = procs().map(([pid, name, app, appName, libs, model, sh, vram, gtt]) => {
      const prev = ns.get(pid) || {};
      const engines = {};
      for (const e of ['gfx', 'compute', 'dec', 'enc', 'sdma']) engines[e] = (prev[e] || 0) + Math.round((sh[e] || 0) * 1e9);
      ns.set(pid, engines);
      return { pid, ppid: 1, name, cmd: name, exe: `/usr/bin/${name}`, model, self: pid === 1612, app, appName, libs,
        clients: [{ driver: 'amdgpu', pdev: '0000:01:00.0', id: pid, engines, capacity: {}, cycles: {}, totalCycles: {},
          mem: { vram: { memory: null, total: vram, resident: vram, shared: 0 }, gtt: { memory: null, total: gtt, resident: gtt, shared: 0 } } }], kfd: [], nvDev: [] };
    });
    const ig = [[8221, 'chrome', 'desktop:google-chrome', 'Web browser', { render: 0.05 }, 388 * MIB], [9410, 'lord-media', 'desktop:video', 'Video player', { video: 0.03 }, 24 * MIB]];
    for (const [pid, name, app, appName, sh, sys] of ig) {
      const prev = ns.get(pid) || {};
      const engines = {};
      for (const e of ['render', 'video', 'copy']) engines[e] = (prev[e] || 0) + Math.round((sh[e] || 0) * (0.8 + 0.4 * Math.sin(k / 9)) * 1e9);
      ns.set(pid, engines);
      list.push({ pid, ppid: 1, name, cmd: name, exe: `/usr/bin/${name}`, model: '', self: false, app, appName, libs: ['libEGL.so.1', 'iris_dri.so'],
        clients: [{ driver: 'i915', pdev: '0000:00:02.0', id: pid, engines, capacity: {}, cycles: {}, totalCycles: {}, mem: { system0: { memory: null, total: sys, resident: sys, shared: 0 } } }], kfd: [], nvDev: [] });
    }
    const busy = Math.round(Math.min(100, 30 + 50 * wave(0.7, 0.1, 0.21)));
    return {
      t: Date.now(), mono: k * 1000, hz: 100,
      cards: [
        { card: 'card1', driver: 'amdgpu', slot: '0000:01:00.0', vendor: '1002', device: '67df', vendorName: 'Advanced Micro Devices, Inc. [AMD/ATI]',
          deviceName: 'Ellesmere [Radeon RX 580]', render: true, busy, vramUsed: Math.round((k < 40 ? 2.3 : 6.92) * GIB), vramTotal: 8 * GIB, visUsed: null, visTotal: null,
          gttUsed: Math.round(0.61 * GIB), gttTotal: Math.round(62.8 * GIB), temps: [{ label: 'edge', c: 71, crit: 94 }], fanRpm: 1840, fanMax: 3700, fanPct: 50, powerW: 118.4, powerCapW: 145,
          energyUj: null, sclk: 1340, sclkMax: 1340, mclk: 2000, mclkMax: 2000, curMhz: null, maxMhz: null, memBusy: 38, link: { gts: 8.0, width: 16 }, linkMax: { gts: 8.0, width: 16 },
          rp0Mhz: null, rc6Ms: null, gts: [], nvModel: '', nvVbios: '' },
        { card: 'card0', driver: 'i915', slot: '0000:00:02.0', vendor: '8086', device: '3e92', vendorName: 'Intel Corporation', deviceName: 'CoffeeLake-S GT2 [UHD Graphics 630]',
          render: true, busy: null, vramUsed: null, vramTotal: null, visUsed: null, visTotal: null, gttUsed: null, gttTotal: null, temps: [], fanRpm: null, fanMax: null, fanPct: null,
          powerW: null, powerCapW: null, energyUj: null, sclk: null, sclkMax: null, mclk: null, mclkMax: null, curMhz: 350, maxMhz: 1200, memBusy: null, link: null, linkMax: null,
          rp0Mhz: 1200, rc6Ms: k * 910, gts: [], nvModel: '', nvVbios: '' },
      ],
      list,
      hidden: [{ pid: 1044, name: 'ollama', kind: 'kfd' }],
      ai: [
        { pid: 48211, ppid: 3000, name: 'llama-server', state: 'R', own: true, ticks: k * 60, rss: 1.2 * GIB, model: 'Qwen3-8B-Q4_K_M.gguf' },
        { pid: 7001, ppid: 1612, name: 'llama-server', state: 'S', own: true, ticks: k * 186, rss: 3.1 * GIB, model: '3' },
        { pid: 58829, ppid: 1612, name: 'lord-say', state: 'S', own: true, ticks: k, rss: 8 * MIB, model: '' },
        { pid: 58830, ppid: 58829, name: 'lord-tts', state: 'S', own: true, ticks: k * 4, rss: 410 * MIB, model: '' },
        { pid: 57712, ppid: 1612, name: 'lord-studio', state: 'S', own: true, ticks: k * 22, rss: 240 * MIB, model: '' },
      ],
      say: [{ pid: 58829, state: 'S', backend: 'vulkan', quality: 'hd', lang: 'es', mode: 'speak' }],
      vms: [{ pid: 61190, ticks: k * 80, rss: 4 * GIB }],
      caps: { v: 1, dri: ['radeonsi_dri.so', 'iris_dri.so'], va: ['radeonsi_drv_video.so', 'iHD_drv_video.so'], vdpau: [], libs: ['libvulkan.so.1', 'libEGL.so.1', 'libGLX.so.0', 'libva.so.2', 'libgallium-25.0.7-2+deb13u1.so'],
        vulkan: [{ file: 'radeon_icd.json', lib: 'libvulkan_radeon.so', api: '1.4.305' }, { file: 'intel_icd.json', lib: 'libvulkan_intel.so', api: '1.4.305' }], opencl: [], kfd: true, nvidia: '' },
    };
  };
  // the voice's GPU use: lord-tts is sandboxed (its fds can't be read), but in the made-up list it shows like the mockup
  const withVoice = (r) => {
    const prev = ns.get(58830) || {};
    const compute = (prev.compute || 0) + Math.round(0.04 * 1e9);
    ns.set(58830, { compute });
    r.list.push({ pid: 58830, ppid: 58829, name: 'lord-tts', cmd: 'lord-tts', exe: '/usr/lib/lordos/lord-tts', model: '', self: false, app: 'desktop:voice', appName: 'LordOS Voice',
      libs: ['libvulkan.so.1', 'libvulkan_radeon.so', 'libggml-vulkan.so'],
      clients: [{ driver: 'amdgpu', pdev: '0000:01:00.0', id: 58830, engines: { compute }, capacity: {}, cycles: {}, totalCycles: {}, mem: { vram: { memory: null, total: 310 * MIB, resident: 310 * MIB, shared: 0 }, gtt: { memory: null, total: 8 * MIB, resident: 8 * MIB, shared: 0 } } }], kfd: [], nvDev: [] });
    return r;
  };
  lord.monitor.wantGpu = () => true;
  lord.monitor.gpu = () => withVoice(raw());
  lord.sentinela = { ...(lord.sentinela || {}), state: () => ({ state: 'ready', model: 'Qwen3-4B Q4_K_M', pid: 7001, idleMs: 1000, queued: 0 }) };
  const texts = new Map(), parent = new Map();
  const wrap = (name, fn) => { const real = lord[name]; lord[name] = (...a) => { fn(...a); return real(...a); }; };
  wrap('setText', (id, s) => texts.set(id, String(s)));
  wrap('appendChild', (p, c) => parent.set(c, p));
  wrap('insertBefore', (p, c) => parent.set(c, p));
  wrap('destroy', (id) => { texts.delete(id); parent.delete(id); });
  globalThis.__gpuScene = { texts, parent };
})();
