// gpu_model_test.mjs — Monitor → GPU's model (apps/monitor/gpumodel.js): names and kinds, the fan, engine shares from
// ns and cycles, deduplicated clients, memory, grouping into apps, the APIs installed and in use (maps fixtures), the AI
// engines, virtual machines, history and the Overview's pick. Run by tests/run_suites.sh (needs node).
import {
  gpuName, gpuKind, fanState, engineCategory, dedupeClients, clientMem, engineShares, gpuApis, apiTags, mesaVersion, nvidiaVersion,
  aiEngines, createGpuState, gpuStep, pushHistory, mainCard, topApp, GIB, nvSlot, mergeNv, nvEngines, categoryShares, appType, memSplit, pcieGen,
  aiRows, rowsThatFit,
} from '../../apps/monitor/gpumodel.js';

let n = 0, bad = 0;
const eq = (what, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${what}\n    got  ${g}\n    want ${w}`); }
};
const near = (what, got, want, eps = 1e-6) => { n++; if (!(Math.abs(got - want) <= eps)) { bad++; console.log(`✗ ${what}: got ${got}, want ${want}`); } };

// ---- names and kinds (the values read on an RX 580) ----
const rx580 = { card: 'card0', driver: 'amdgpu', slot: '0000:01:00.0', vendor: '1002', device: '67df', vendorName: 'Advanced Micro Devices, Inc. [AMD/ATI]',
  deviceName: 'Ellesmere [Radeon RX 470/480/570/570X/580/580X/590]', render: true, busy: 1, vramUsed: 360460288, vramTotal: 8 * GIB, gttUsed: 37634048,
  gttTotal: 67468935168, temps: [{ label: 'edge', c: 47 }], fanRpm: 756, fanMax: 3700, fanPct: 0, powerW: 22.13, powerCapW: 145, energyUj: null,
  sclk: 1329, sclkMax: 1340, mclk: 300, mclkMax: 2000, curMhz: null, maxMhz: null };
eq('amd name', gpuName(rx580), 'AMD Radeon RX 470/480/570/570X/580/580X/590');
eq('intel name', gpuName({ vendor: '8086', vendorName: 'Intel Corporation', deviceName: 'CometLake-S GT2 [UHD Graphics 630]' }), 'Intel UHD Graphics 630');
eq('nvidia name', gpuName({ vendor: '10de', vendorName: 'NVIDIA Corporation', deviceName: 'GA104 [GeForce RTX 3070]' }), 'NVIDIA GeForce RTX 3070');
eq('virtio name', gpuName({ vendor: '1af4', vendorName: 'Red Hat, Inc.', deviceName: 'Virtio 1.0 GPU' }), 'Virtio 1.0 GPU');
eq('no PCI name', gpuName({ driver: 'v3d', deviceName: '' }), null);
eq('kinds', [gpuKind(rx580), gpuKind({ driver: 'amdgpu', vramTotal: 512 << 20 }), gpuKind({ driver: 'i915', slot: '0000:00:02.0' }),
  gpuKind({ driver: 'xe', slot: '0000:03:00.0', vramTotal: 8 * GIB }), gpuKind({ driver: 'v3d', render: true }), gpuKind({ driver: 'vc4', render: false }),
  gpuKind({ driver: 'virtio_gpu' }), gpuKind({ driver: 'nvidia' })], ['discrete', 'integrated', 'integrated', 'discrete', 'integrated', 'display', 'virtual', 'discrete']);

// ---- the fan: Polaris reads pwm1 0 in automatic while it turns ----
eq('fan from rpm', fanState(rx580), { rpm: 756, max: 3700, pct: 20, spinning: true });
eq('fan by hand', fanState({ ...rx580, fanPct: 45 }), { rpm: 756, max: 3700, pct: 45, spinning: true });
eq('zero-rpm mode', fanState({ ...rx580, fanRpm: 0, fanPct: 33 }), { rpm: 0, max: 3700, pct: 0, spinning: false });
eq('no fan', fanState({ fanRpm: null, fanPct: null }), null);

// ---- engines ----
eq('categories', ['gfx', 'compute', 'dec', 'enc_1', 'jpeg', 'dma', 'render', 'video-enhance', 'copy', 'rcs', 'ccs', 'bcs', 'vcs', 'bin', 'csd', 'tfu', 'cache_clean', 'xyz'].map(engineCategory),
  ['3d', 'compute', 'video', 'encode', 'video', 'copy', '3d', 'video', 'copy', '3d', 'compute', 'copy', 'video', '3d', 'compute', 'copy', 'copy', 'other']);
const a0 = { engines: { gfx: 1e9, dec: 0 }, capacity: {} }, a1 = { engines: { gfx: 1.25e9, dec: 0.1e9 }, capacity: {} };
const sh = engineShares(a0, a1, 1e9);
near('gfx 25 %', sh.gfx, 0.25);
near('dec 10 %', sh.dec, 0.1);
near('capacity 2', engineShares({ engines: { video: 0 } }, { engines: { video: 1e9 }, capacity: { video: 2 } }, 1e9).video, 0.5);
near('xe cycles', engineShares({ cycles: { rcs: 100 }, totalCycles: { rcs: 1000 } }, { cycles: { rcs: 400 }, totalCycles: { rcs: 2000 } }, 1e9).rcs, 0.3);
eq('a counter that went back is skipped', engineShares({ engines: { gfx: 5 } }, { engines: { gfx: 1 } }, 1e9), {});
eq('never over 100 %', engineShares({ engines: { gfx: 0 } }, { engines: { gfx: 5e9 } }, 1e9), { gfx: 1 });
eq('first sample: nothing yet', engineShares(undefined, a1, 1e9), {});

// ---- clients ----
const cl = (id, extra = {}) => ({ driver: 'amdgpu', pdev: '0000:01:00.0', id, engines: {}, capacity: {}, cycles: {}, totalCycles: {}, mem: {}, ...extra });
const dd = dedupeClients([{ pid: 20, clients: [cl(5), cl(6)] }, { pid: 10, clients: [cl(5)] }, { pid: 30, clients: [cl(null), cl(null)] }]);
eq('a shared client counts once, under the first process', dd.map((p) => [p.pid, p.clients.map((c) => c.id)]), [[10, [5]], [20, [6]], [30, [null, null]]]);
eq('amdgpu memory (drm-memory)', clientMem(cl(1, { mem: { vram: { memory: 1000, total: -1, resident: -1, shared: 0 }, gtt: { memory: 200, total: -1, resident: -1, shared: 0 }, cpu: { memory: 0, total: -1, resident: -1, shared: 0 } } })), { device: 1000, system: 200 });
eq('i915 memory (drm-total)', clientMem(cl(1, { mem: { system0: { memory: -1, total: 4096, resident: 2048, shared: 0 }, 'stolen-system0': { memory: -1, total: 0, resident: 0, shared: 0 } } })), { device: 0, system: 4096 });
eq('xe local memory', clientMem(cl(1, { mem: { vram0: { memory: -1, total: 12 << 20, resident: 12 << 20, shared: -1 } } })), { device: 12 << 20, system: 0 });
eq('no memory keys', clientMem(cl(1)), null);

// ---- APIs installed (the RX 580 machine; NVIDIA and a Pi from fixtures) ----
const caps = { v: 1, dri: ['radeonsi_dri.so', 'swrast_dri.so', 'iris_dri.so'], va: ['radeonsi_drv_video.so'], vdpau: ['libvdpau_trace.so.1'],
  libs: ['libvulkan.so.1', 'libEGL.so.1', 'libGLESv2.so.2', 'libOpenCL.so.1', 'libvdpau.so.1', 'libgallium-25.0.7-2+deb13u1.so', 'libGLX.so.0', 'libva.so.2'],
  vulkan: [{ file: 'intel_icd.json', lib: 'libvulkan_intel.so', api: '1.4.305' }, { file: 'radeon_icd.json', lib: 'libvulkan_radeon.so', api: '1.4.305' },
    { file: 'lvp_icd.json', lib: 'libvulkan_lvp.so', api: '1.4.305' }], opencl: [], kfd: true, nvidia: '' };
const byId = (l) => Object.fromEntries(l.map((x) => [x.id, [x.state, x.detail]]));
eq('mesa version', mesaVersion(caps.libs), '25.0.7');
eq('RX 580 APIs', byId(gpuApis(rx580, caps)), {
  vulkan: ['available', 'RADV · 1.4.305'], opengl: ['available', 'Mesa 25.0.7 · radeonsi'], opencl: ['missing', ''], video: ['available', 'VA-API · radeonsi'],
  rocm: ['missing', 'kfd'], webgpu: ['browser', ''] });
eq('without the Vulkan loader', gpuApis(rx580, { ...caps, libs: caps.libs.filter((l) => l !== 'libvulkan.so.1') })[0].state, 'missing');
eq('Rusticl', byId(gpuApis(rx580, { ...caps, opencl: [{ file: 'rusticl.icd', lib: 'libRusticlOpenCL.so.1' }, { file: 'pocl.icd', lib: 'libpocl.so.2' }] })).opencl, ['available', 'Rusticl, PoCL']);
eq('ROCm', byId(gpuApis(rx580, { ...caps, libs: [...caps.libs, 'libamdhip64.so'] })).rocm, ['available', '']);
const nvCaps = { v: 1, dri: [], va: [], vdpau: ['libvdpau_nvidia.so.1'], libs: ['libvulkan.so.1', 'libEGL.so.1', 'libGLX.so.0', 'libcuda.so.1', 'libnvidia-encode.so.1', 'libvdpau.so.1'],
  vulkan: [{ file: 'nvidia_icd.json', lib: 'libGLX_nvidia.so.0', api: '1.3.277' }], opencl: [{ file: 'nvidia.icd', lib: 'libnvidia-opencl.so.1' }], kfd: false,
  nvidia: 'NVRM version: NVIDIA UNIX x86_64 Kernel Module  550.54.14  Thu Feb 22 01:44:30 UTC 2024' };
eq('nvidia version', nvidiaVersion(nvCaps.nvidia), '550.54.14');
eq('nvidia version (open)', nvidiaVersion('NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  560.35.03  Release Build'), '560.35.03');
eq('NVIDIA APIs', byId(gpuApis({ driver: 'nvidia' }, nvCaps)), {
  vulkan: ['available', 'NVIDIA · 1.3.277'], opengl: ['available', 'NVIDIA'], opencl: ['missing', ''], video: ['available', 'VDPAU'],
  cuda: ['available', '550.54.14'], nvenc: ['available', ''], nvdec: ['missing', ''], webgpu: ['browser', ''] });
eq('nouveau: no CUDA', byId(gpuApis({ driver: 'nouveau' }, caps)).cuda, ['unsupported', '']);
eq('Pi 5 V3D', byId(gpuApis({ driver: 'v3d' }, { ...caps, vulkan: [{ file: 'broadcom_icd.json', lib: 'libvulkan_broadcom.so', api: '1.3.305' }], dri: ['v3d_dri.so'] })), {
  vulkan: ['available', 'V3DV · 1.3.305'], opengl: ['available', 'Mesa 25.0.7 · v3d'], opencl: ['missing', ''], video: ['none', ''], webgpu: ['browser', ''] });
eq('old radeon: no Vulkan', gpuApis({ driver: 'radeon' }, caps)[0].state, 'unsupported');
eq('a VM without a GPU (simpledrm): software GL', byId(gpuApis({ driver: 'simpledrm' }, caps)).opengl, ['available', 'llvmpipe']);
eq('no caps yet', gpuApis(rx580, null), []);

// ---- APIs in use, from maps lines (gpustat.c keeps the base names) ----
const ids = (l) => l.map((x) => (x.detail ? `${x.id}:${x.detail}` : x.id));
eq('lord-server (GLES through EGL)', ids(apiTags(['libEGL.so.1.1.0', 'libEGL_mesa.so.0.0.0', 'libGLESv2.so.2.1.0', 'libgallium-25.0.7-2+deb13u1.so'], 'amdgpu')), ['opengles:radeonsi']);
eq('a browser (the RX 580 GPU process)', ids(apiTags(['libva.so.2.2200.0', 'libEGL_mesa.so.0.0.0', 'libgallium-25.0.7-2+deb13u1.so', 'libva-drm.so.2.2200.0', 'libEGL.so.1.1.0'], 'amdgpu')),
  ['opengl:radeonsi', 'video']);
eq('a Vulkan game', ids(apiTags(['libvulkan.so.1.4.309', 'libvulkan_radeon.so'])), ['vulkan:RADV']);
eq('a GL game with a DRI driver', ids(apiTags(['libGLX.so.0', 'libGLX_mesa.so.0', 'iris_dri.so', 'libva.so.2', 'iHD_drv_video.so'])), ['opengl:iris', 'video:iHD']);
eq('CUDA and NVENC', ids(apiTags(['libcuda.so.550.54.14', 'libnvidia-encode.so.550.54.14', 'libnvcuvid.so.1', 'libGLX_nvidia.so.0', 'libnvidia-glcore.so.550'])),
  ['opengl:NVIDIA', 'cuda', 'video:NVDEC', 'encode:NVENC']);
eq('OpenCL and HIP', ids(apiTags(['libOpenCL.so.1', 'libamdhip64.so.6'])), ['opencl', 'hip']);
eq('ggml on Vulkan', ids(apiTags(['libggml-vulkan.so'])), ['vulkan']);
eq('nothing known', apiTags([]), []);

// ---- AI engines ----
const rawAi = { ai: [{ pid: 400, ppid: 1, name: 'lord-say', state: 'S', own: true }, { pid: 401, ppid: 400, name: 'lord-tts', state: 'R', own: false }],
  say: [{ pid: 400, state: 'S', backend: 'vulkan', quality: 'hd', lang: 'es', mode: 'speak' }] };
eq('voice on the GPU', aiEngines(rawAi), [{ id: 'voice', pid: 401, backend: 'gpu', api: 'vulkan', quality: 'hd', lang: 'es', state: 'speaking', mem: null, hidden: true }]);
eq('paused', aiEngines({ ...rawAi, ai: [{ ...rawAi.ai[0], state: 'T' }, rawAi.ai[1]] })[0].state, 'paused');
eq('saving on the CPU', aiEngines({ ...rawAi, say: [{ ...rawAi.say[0], backend: 'cpu', mode: 'save', quality: 'standard' }] }).map((e) => [e.backend, e.api, e.state, e.quality]), [['cpu', null, 'saving', 'standard']]);
eq('no note yet', aiEngines({ ai: rawAi.ai, say: [] })[0].backend, null);
eq('OCR: CPU', aiEngines({ ai: [{ pid: 9, ppid: 1, name: 'lord-ocr', state: 'R', own: false }] }).map((e) => [e.id, e.backend]), [['ocr', 'cpu']]);
eq('lord-say alone (a preview): no engine', aiEngines({ ai: [rawAi.ai[0]], say: [] }), []);
eq('a dead helper', aiEngines({ ai: [{ ...rawAi.ai[1], state: 'Z' }] }), []);
eq('its memory when readable', aiEngines({ ...rawAi, ai: [rawAi.ai[0], { ...rawAi.ai[1], own: true }] }, [{ pids: [401], mem: 300 << 20 }])[0].mem, 300 << 20);

// ---- the step: two samples one second apart ----
const proc = (pid, gfx, extra = {}) => ({ pid, ppid: 1, name: 'game', cmd: 'game', exe: '/usr/games/game', self: false, libs: ['libvulkan.so.1', 'libvulkan_radeon.so'],
  clients: [cl(217, { engines: { gfx, dec: 0 }, mem: { vram: { memory: 100 << 20, total: -1, resident: -1, shared: 0 }, gtt: { memory: 8 << 20, total: -1, resident: -1, shared: 0 } } })], ...extra });
const raw1 = { t: 1000, mono: 5000, hz: 100, cards: [rx580], list: [proc(10, 1e9), { ...proc(11, 0), clients: [cl(217, { engines: { gfx: 1e9 } })] }],
  ai: [], say: [], vms: [{ pid: 50, ticks: 1000, rss: 2 * GIB }], caps };
const raw2 = { ...raw1, t: 2000, mono: 6000, cards: [{ ...rx580, busy: 42 }], list: [proc(10, 1.5e9)], vms: [{ pid: 50, ticks: 1150, rss: 2 * GIB }] };
const st = createGpuState();
const v1 = gpuStep(st, raw1);
eq('first sample: no share yet', [v1.apps.length, v1.apps[0].total, v1.apps[0].count], [1, 0, 1]);
eq('memory of the app', [v1.apps[0].mem, v1.apps[0].sysMem], [100 << 20, 8 << 20]);
const v2 = gpuStep(st, raw2);
near('gfx 50 %', v2.apps[0].engines.gfx, 0.5);
eq('category', v2.apps[0].cats['3d'], 0.5);
eq('apis of the app', v2.apps[0].apis, [{ id: 'vulkan', detail: 'RADV' }]);
eq('card', [v2.cards[0].busy, v2.cards[0].busySource, v2.cards[0].name, v2.cards[0].kind, v2.cards[0].temp, v2.cards[0].fan.pct, v2.cards[0].apps], [0.42, 'driver', 'AMD Radeon RX 470/480/570/570X/580/580X/590', 'discrete', 47, 20, ['name:game']]);
eq('vms', [v2.vms.count, v2.vms.cpu, v2.vms.rss], [1, 1.5, 2 * GIB]);
// a driver without gpu_busy_percent: the busiest engine summed over the apps
const st2 = createGpuState();
const intel = { ...rx580, driver: 'i915', slot: '0000:00:02.0', busy: null, vramTotal: null, vramUsed: null, fanRpm: null, fanMax: null, fanPct: null, powerW: null, energyUj: 1000000 };
gpuStep(st2, { ...raw1, cards: [intel], list: [{ ...proc(10, 0), clients: [cl(1, { driver: 'i915', pdev: '0000:00:02.0', engines: { render: 0, video: 0 } })] }] });
const vi = gpuStep(st2, { ...raw1, mono: 6000, cards: [{ ...intel, energyUj: 13000000 }], list: [{ ...proc(10, 0), clients: [cl(1, { driver: 'i915', pdev: '0000:00:02.0', engines: { render: 3e8, video: 6e8 } })] }] });
eq('estimated from apps', [vi.cards[0].busy, vi.cards[0].busySource, vi.cards[0].kind, vi.cards[0].shared], [0.6, 'apps', 'integrated', true]);
near('power from energy', vi.cards[0].powerW, 12);
eq('nothing reported', gpuStep(createGpuState(), { ...raw1, cards: [{ ...intel, energyUj: null }], list: [] }).cards[0].busy, null);
eq('not an answer', gpuStep(createGpuState(), null), null);
// lord-server itself is LordOS
eq('self', gpuStep(createGpuState(), { ...raw1, list: [{ ...proc(10, 0), self: true }] }).apps[0].key, 'app:lordos');

// ---- history and the Overview ----
let h = {};
for (let i = 0; i < 5; i++) h = pushHistory(h, { t: i, cards: [{ key: 'k', busy: 0.123, temp: 50, fan: { pct: 20, rpm: 700 }, vram: { used: 1 }, powerW: 20, sclk: 300 }] }, 3);
eq('history capped', h.k.map((p) => p.t), [2, 3, 4]);
eq('a point', h.k[0], { t: 2, busy: 12.3, temp: 50, fan: 20, rpm: 700, vram: 1, power: 20, sclk: 300, eng: { '3d': 0, compute: 0, video: 0, encode: 0, copy: 0 } });
const two = { cards: [{ key: 'a', kind: 'integrated', render: true, apps: [] }, { key: 'b', kind: 'discrete', render: true, apps: ['x'] }, { key: 'c', kind: 'display', apps: [] }], apps: [{ key: 'x', total: 0.3, mem: 1 }] };
eq('main card', [mainCard(two).main.key, mainCard(two).more], ['b', 1]);
eq('top app', topApp(two, two.cards[1]).key, 'x');
eq('no cards', mainCard({ cards: [] }), { main: null, more: 0 });

// ---- the new view: engines by category, types, memory split, link, idle, NVIDIA, AI ----
eq('categories', categoryShares({ gfx: 0.2, compute: 0.5, comp_1: 0.6, dec: 0.1, enc: 0.15, sdma: 0.03, weird: 1 }), { '3d': 0.2, compute: 1, video: 0.1, encode: 0.15, copy: 0.03 });
eq('types', [appType({ cats: { compute: 0.7, '3d': 0.1 } }), appType({ cats: { video: 0.1, encode: 0.05, '3d': 0.02 } }), appType({ cats: { '3d': 0.09 } }),
  appType({ cats: {}, apis: [{ id: 'cuda' }] }), appType({ cats: {}, apis: [{ id: 'cuda' }, { id: 'opengl' }] }), appType({ cats: {}, apis: [{ id: 'video' }] })],
  ['compute', 'video', 'graphics', 'compute', 'graphics', 'video']);
eq('split', memSplit([{ type: 'compute', mem: 5 }, { type: 'graphics', mem: 2, sysMem: 9 }, { type: 'video', mem: 1 }, { mem: 3 }], 's', false), { compute: 5, graphics: 5, video: 1 });
eq('split shared', memSplit([{ type: 'graphics', mem: 0, sysMem: 9 }], 's', true), { compute: 0, graphics: 9, video: 0 });
eq('pcie gens', [pcieGen(2.5), pcieGen(8), pcieGen(16), pcieGen(7), pcieGen(null)], [1, 3, 4, null, null]);
eq('nv slots', [nvSlot('00000000:01:00.0'), nvSlot('0000:0A:00.0'), nvSlot('../../x'), nvSlot('00000000:01:00.9'), nvSlot(null)], ['0000:01:00.0', '0000:0a:00.0', null, null, null]);
// lord-nvgpu's canned and hostile numbers
const nvCard = { ...rx580, driver: 'nvidia', vendor: '10de', slot: '0000:01:00.0', busy: null, vramTotal: null, vramUsed: null, temps: [], fanRpm: null, fanMax: null, fanPct: null, powerW: null, powerCapW: null, sclk: null, mclk: null, sclkMax: null, mclkMax: null };
const nvGood = { bus: '00000000:01:00.0', util: { sm: 71, mem: 38, enc: 14, dec: 12 }, temp: 66, fan: 45, power: 180, powerLimit: 220, clk: { core: 1800, mem: 7000 },
  vram: { used: 5 * GIB, total: 12 * GIB }, cuda: '12.4', driver: '550.54.14', computeMode: 'default', ecc: 0,
  procs: [{ pid: 10, mem: 4 * GIB, sm: 70, enc: 0, dec: 0 }, { pid: 77, mem: GIB, sm: 1, enc: 14, dec: 12 }] };
const mg = mergeNv(nvCard, nvGood);
eq('merged', [mg.busy, mg.temps[0].c, mg.fanPct, mg.powerW, mg.powerCapW, mg.sclk, mg.mclk, mg.vramUsed, mg.vramTotal, mg.memBusy], [71, 66, 45, 180, 220, 1800, 7000, 5 * GIB, 12 * GIB, 38]);
const hostile = { bus: '00000000:01:00.0', util: { sm: 500, mem: -3, enc: 'x', dec: 1e9 }, temp: 9999, fan: -1, power: 1e12, powerLimit: 0, clk: { core: NaN, mem: Infinity },
  vram: { used: 13 * GIB, total: 12 * GIB }, procs: [{ pid: -1, mem: 1, sm: 1 }, { pid: 1.5, sm: 2 }, { pid: '10' }, null, { pid: 10, mem: -5, sm: 900, enc: 50 }] };
const mh = mergeNv(nvCard, hostile);
eq('hostile merged', [mh.busy, mh.temps.length, mh.fanPct, mh.powerW, mh.powerCapW, mh.sclk, mh.mclk, mh.vramUsed, mh.vramTotal, mh.memBusy], [null, 0, null, null, null, null, null, null, 12 * GIB, null]);
eq('nv engines', [nvEngines(nvGood), nvEngines(hostile)], [{ gfx: 0.71, nvenc: 0.14, nvdec: 0.12 }, {}]);
const stn = createGpuState();
const vn = gpuStep(stn, { ...raw1, cards: [nvCard], list: [{ ...proc(10, 0), libs: ['libcuda.so.1'], clients: [] }], nv: [nvGood] });
const nvApp = vn.apps.find((a) => a.pids.includes(10));
eq('nv process', [nvApp.type, Math.round(nvApp.total * 100), nvApp.mem, nvApp.apis.map((a) => a.id), vn.cards[0].busy, vn.cards[0].apps.length], ['compute', 70, 4 * GIB, ['cuda'], 0.71, 2]);
eq('nv video process', vn.apps.find((a) => a.pids.includes(77)).type, 'video');
eq('nv card cats', vn.cards[0].cats, { '3d': 0.71, compute: 0, video: 0.12, encode: 0.14, copy: 0 });
const vh = gpuStep(createGpuState(), { ...raw1, cards: [nvCard], list: [], nv: [hostile, { bus: 'garbage', procs: [{ pid: 5, sm: 50 }] }, null, 7] });
eq('hostile nv', [vh.apps.map((a) => a.pids), Math.round(vh.apps[0].total * 100), vh.apps[0].mem, vh.cards[0].busy], [[[10]], 50, null, null]);
eq('no nv', gpuStep(createGpuState(), { ...raw1, nv: 'x' }).cards.length, 1);
// link, idle (RC6), hidden rows, memory busy
const sti = createGpuState();
const ic = { ...intel, slot: '0000:00:02.0', link: { gts: 8, width: 16 }, linkMax: { gts: 16, width: 16 }, rc6Ms: 1000, memBusy: 38, temps: [{ label: 'edge', c: 70, crit: 94 }] };
gpuStep(sti, { ...raw1, mono: 1000, cards: [ic], list: [] });
const vi2 = gpuStep(sti, { ...raw1, mono: 2000, cards: [{ ...ic, rc6Ms: 1910 }], list: [], hidden: [{ pid: 1044, name: 'ollama', kind: 'kfd' }, { pid: 'x' }] });
eq('link idle hidden', [vi2.cards[0].link, vi2.cards[0].linkMax, vi2.cards[0].idle, vi2.cards[0].memBusy, vi2.cards[0].tempCrit, vi2.cards[0].hidden],
  [{ gen: 3, width: 16 }, { gen: 4, width: 16 }, 0.91, 0.38, 94, [{ pid: 1044, name: 'ollama', kind: 'kfd' }]]);
// AI rows: a runner with its model (from argv), Sentinela by its pid, the voice with lord-say's note, an app mapping libcuda
const sta = createGpuState();
const rawAi2 = (mono, t) => ({ ...raw1, mono, hz: 100, cards: [nvCard], nv: [nvGood],
  list: [{ ...proc(10, 0), name: 'llama-server', model: 'Qwen3-8B-Q4_K_M.gguf', libs: ['libggml-vulkan.so'], clients: [] }, { ...proc(77, 0), name: 'python3', model: '', libs: ['libcuda.so.1'], clients: [] }],
  ai: [{ pid: 10, ppid: 1, name: 'llama-server', state: 'R', own: true, ticks: 100 + t, rss: GIB, model: 'Qwen3-8B-Q4_K_M.gguf' },
    { pid: 20, ppid: 1, name: 'llama-server', state: 'S', own: true, ticks: 50 + 2 * t, rss: 3 * GIB, model: '3' },
    { pid: 30, ppid: 31, name: 'lord-tts', state: 'S', own: true, ticks: 5, rss: 1, model: '' }, { pid: 31, ppid: 1, name: 'lord-say', state: 'S', own: true, ticks: 1, rss: 1, model: '' },
    { pid: 40, ppid: 1, name: 'lord-studio', state: 'S', own: true, ticks: 9 + t / 5, rss: 1, model: '' }, { pid: 50, ppid: 1, name: 'bash', state: 'S', own: true, ticks: 1, rss: 1 },
    { pid: 60, ppid: 1, name: 'ollama', state: 'Z', own: false, ticks: 1, rss: 1 }],
  say: [{ pid: 31, state: 'S', backend: 'vulkan', quality: 'hd', lang: 'es', mode: 'speak' }] });
let va = gpuStep(sta, rawAi2(1000, 0));
aiRows(sta, va, rawAi2(1000, 0), { pid: 20, model: 'qwen3-4b' });
const r2 = rawAi2(2000, 100);
va = gpuStep(sta, r2);
const rows = aiRows(sta, va, r2, { pid: 20, model: 'qwen3-4b' });
eq('ai kinds', rows.map((r) => `${r.kind}:${r.pid}:${r.where}`), ['runner:10:gpu', 'app:77:gpu', 'voice:30:gpu', 'sentinela:20:cpu', 'studio:40:cpu']);
eq('ai runner', [rows[0].model, Math.round(rows[0].compute * 100), rows[0].vram, rows[0].cpu], ['Qwen3-8B-Q4_K_M.gguf', 70, 4 * GIB, 1]);
eq('ai sentinela', [rows[3].model, rows[3].cpu, rows[3].rss], ['qwen3-4b', 2, 3 * GIB]);
eq('ai voice', [rows[2].lang, rows[2].quality, rows[2].apis], ['es', 'hd', ['vulkan']]);
eq('no view', aiRows(createGpuState(), null, null), []);
eq('rows that fit', [rowsThatFit(300, 23, 30), rowsThatFit(10, 23, 30), rowsThatFit(5000, 23, 0, 1, 12), rowsThatFit(NaN, 23)], [11, 1, 12, 1]);

console.log(`gpu-model-test: ${n} checks, ${bad} failed`);
process.exit(bad ? 1 : 0);
