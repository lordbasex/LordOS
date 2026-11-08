// gpumodel.js — Monitor → GPU's model (docs/MONITOR.md, "GPU"): lord.monitor.gpu()'s raw counters (server/src/monitor/
// gpustat.c) turned into what the tab and the Overview show. Pure functions with no UI and no SDK, tested by
// server/tests/gpu_model_test.mjs. Nothing here makes up a number: what a driver doesn't report stays null.
//
//   const st = createGpuState();
//   const view = gpuStep(st, raw);   // { t, cards, apps, ai, vms, caps }
//
// Per card: utilisation (amdgpu's gpu_busy_percent; else the busiest engine summed over the user's apps, marked
// 'apps'), memory, temperatures, fan, power, clocks, and which APIs are installed for it. Per app: the DRM clients of
// its processes (deduplicated: a client shared by fds or processes counts once), each engine's share of the last
// second, its GPU memory and the APIs its mapped libraries tell.

export const GPU_HISTORY = 180;           // 3 minutes at one sample per second, like Cooling's history
export const GIB = 1024 ** 3;

const num = (v) => (typeof v === 'number' && Number.isFinite(v) ? v : null);

// ---- names and kinds ----

const VENDORS = { 1002: 'AMD', 8086: 'Intel', '10de': 'NVIDIA', '1af4': 'Virtio', '15ad': 'VMware', 1234: 'QEMU', '1b36': 'QXL', 1013: 'Cirrus', 1414: 'Hyper-V', '14e4': 'Broadcom', '5143': 'Qualcomm' };

// The card's name from pci.ids: "Ellesmere [Radeon RX 470/480/570/570X/580/580X/590]" → "AMD Radeon RX 470/…/590".
// null when there is no PCI name (a Raspberry Pi's V3D, a display-only device): the tab names those by their driver.
export function gpuName(card) {
  const dev = String(card?.deviceName || '').trim();
  if (!dev) return null;
  const br = /\[([^\]]+)\]\s*$/.exec(dev);
  const vendor = VENDORS[card.vendor] || String(card.vendorName || '').replace(/\s*\[.*\]\s*$/, '').replace(/,? (Inc|Corporation|Corp)\.?$/, '').trim();
  const model = br ? br[1] : dev;
  return vendor && !model.startsWith(vendor) ? `${vendor} ${model}` : model;
}

const DISPLAY_ONLY = ['simpledrm', 'efidrm', 'vkms', 'vc4'];
const VIRTUAL = ['virtio_gpu', 'virtio-gpu', 'vmwgfx', 'qxl', 'bochs', 'bochs-drm', 'cirrus', 'cirrus-qemu', 'hyperv_drm'];

// 'discrete' | 'integrated' | 'virtual' | 'display': what the drawing shows. An amdgpu card with a fan (or a lot of
// its own memory) is a graphics card; without, the graphics of an APU.
export function gpuKind(card) {
  const d = card?.driver || '';
  if (DISPLAY_ONLY.includes(d) && !card.render) return 'display';
  if (VIRTUAL.includes(d)) return 'virtual';
  if (d === 'amdgpu' || d === 'radeon') return card.fanMax != null || card.fanRpm != null || (card.vramTotal || 0) >= 3 * GIB ? 'discrete' : 'integrated';
  if (d === 'nvidia' || d === 'nouveau') return 'discrete';
  if (d === 'xe' || d === 'i915') return (card.vramTotal || 0) > 0 || (card.slot && !card.slot.startsWith('0000:00:')) ? 'discrete' : 'integrated';
  return 'integrated';
}

// The fan: amdgpu's pwm1 may read 0 in automatic mode while the fan turns (Polaris), so then its speed is the RPM's
// share of fan1_max. { rpm, pct, spinning } or null without a fan.
export function fanState(card, manual = null) {
  const rpm = num(card?.fanRpm), max = num(card?.fanMax);
  let pct = num(card?.fanPct);
  if (rpm == null && pct == null) return null;
  if ((pct == null || (pct === 0 && rpm > 0 && !manual)) && rpm != null && max > 0) pct = Math.min(100, Math.round((rpm / max) * 100));
  if (rpm === 0) pct = 0;
  return { rpm, max, pct, spinning: rpm == null ? pct > 0 : rpm > 0 };
}

export function hottest(card) {
  let best = null;
  for (const t of card?.temps || []) if (num(t.c) != null && (!best || t.c > best.c)) best = t;
  return best;
}

// ---- engines ----

// What an engine does, across drivers: amdgpu (gfx, compute, dec, enc, enc_1, jpeg, dma, vpe), i915 (render, copy,
// video, video-enhance), xe (rcs, bcs, vcs, vecs, ccs), v3d (bin, render, tfu, csd, cache_clean), others.
export function engineCategory(name) {
  const n = String(name || '').toLowerCase();
  if (/^(gfx|render|rcs|bin|3d|graphics)/.test(n)) return '3d';
  if (/^(compute|ccs|csd|comp)/.test(n)) return 'compute';
  if (/^(enc|vce|uvd_enc|nvenc)/.test(n)) return 'encode';
  if (/^(dec|uvd|vcn|jpeg|nvdec|video|vcs|vecs|vpe)/.test(n)) return 'video';
  if (/^(dma|sdma|copy|bcs|tfu|cache)/.test(n)) return 'copy';
  return 'other';
}

export const clientKey = (c) => (c && c.id != null ? `${c.pdev || ''}|${c.driver || ''}|${c.id}` : null);

// Each process's clients, a client listed only under the first process (lowest pid) that holds it.
export function dedupeClients(list) {
  const seen = new Set();
  return [...(list || [])].sort((a, b) => a.pid - b.pid).map((p) => {
    const clients = [];
    for (const c of p.clients || []) {
      const k = clientKey(c);
      if (k && seen.has(k)) continue;
      if (k) seen.add(k);
      clients.push(c);
    }
    return { ...p, clients };
  });
}

// One client's memory: { device, system } bytes. A region's size is drm-total (what it allocated), else the older
// drm-memory, else drm-resident. vram*/local* are the card's own memory; the rest (gtt, system, cpu, stolen) is RAM.
export function clientMem(c) {
  let device = 0, system = 0, any = false;
  for (const [name, m] of Object.entries(c?.mem || {})) {
    const v = [m.total, m.memory, m.resident].find((x) => num(x) != null && x >= 0);
    if (v == null) continue;
    any = true;
    if (/^(vram|local)/.test(name)) device += v; else system += v;
  }
  return any ? { device, system } : null;
}

// Each engine's busy share (0..1) since the previous sample: nanoseconds over the elapsed time × the engine's
// capacity, or cycles over total cycles (xe). prev/cur are a client's counters; dtNs the time between them.
export function engineShares(prev, cur, dtNs) {
  const out = {};
  if (!prev || !cur) return out;
  if (dtNs > 0) {
    for (const [e, ns] of Object.entries(cur.engines || {})) {
      const p = prev.engines?.[e];
      if (num(p) == null || ns < p) continue;
      const cap = cur.capacity?.[e] || 1;
      out[e] = Math.min(1, (ns - p) / (dtNs * cap));
    }
  }
  for (const [e, cy] of Object.entries(cur.cycles || {})) {
    const p = prev.cycles?.[e], tc = cur.totalCycles?.[e], tp = prev.totalCycles?.[e];
    if ([p, tc, tp].some((x) => num(x) == null) || cy < p || tc <= tp) continue;
    out[e] = Math.min(1, (cy - p) / (tc - tp));
  }
  return out;
}

// ---- APIs ----

const VK_DRIVERS = {
  libvulkan_radeon: 'RADV', libvulkan_intel: 'ANV', libvulkan_intel_hasvk: 'HasVK', libvulkan_nouveau: 'NVK', libvulkan_broadcom: 'V3DV',
  libvulkan_virtio: 'Venus', libvulkan_lvp: 'Lavapipe', libvulkan_gfxstream: 'gfxstream', libvulkan_freedreno: 'Turnip', libvulkan_panfrost: 'PanVK',
  libvulkan_asahi: 'Honeykrisp', libvulkan_powervr_mesa: 'PowerVR', libGLX_nvidia: 'NVIDIA', libnvidia: 'NVIDIA', amdvlk64: 'AMDVLK',
};
// the Mesa Vulkan driver each kernel driver uses ([preferred, …]); none: Mesa has no Vulkan driver for it
const VK_FOR = {
  amdgpu: ['libvulkan_radeon', 'amdvlk64'], i915: ['libvulkan_intel', 'libvulkan_intel_hasvk'], xe: ['libvulkan_intel'], nouveau: ['libvulkan_nouveau'],
  nvidia: ['libGLX_nvidia', 'libnvidia'], v3d: ['libvulkan_broadcom'], virtio_gpu: ['libvulkan_virtio'], msm: ['libvulkan_freedreno'],
  panfrost: ['libvulkan_panfrost'], panthor: ['libvulkan_panfrost'], asahi: ['libvulkan_asahi'],
};
const VK_NONE = ['radeon', 'vc4', 'simpledrm', 'efidrm', 'vkms', 'bochs', 'bochs-drm', 'cirrus', 'cirrus-qemu', 'qxl'];
// Mesa's OpenGL (gallium) driver for each kernel driver
const GL_FOR = {
  amdgpu: 'radeonsi', radeon: 'r600', i915: 'iris', xe: 'iris', nouveau: 'nouveau', v3d: 'v3d', vc4: 'vc4', virtio_gpu: 'virtio_gpu',
  vmwgfx: 'vmwgfx', msm: 'msm', panfrost: 'panfrost', panthor: 'panfrost', lima: 'lima', etnaviv: 'etnaviv', asahi: 'asahi',
};
const VA_FOR = { amdgpu: ['radeonsi'], radeon: ['r600'], i915: ['iHD', 'i965'], xe: ['iHD'], nouveau: ['nouveau'], nvidia: ['nvidia'], virtio_gpu: ['virtio_gpu'] };
const VDPAU_FOR = { amdgpu: 'radeonsi', radeon: 'r600', nouveau: 'nouveau', nvidia: 'nvidia' };

const stem = (lib) => String(lib || '').replace(/\.so(\..*)?$/, '');

export const mesaVersion = (libs) => {
  for (const l of libs || []) { const m = /^libgallium-(\d+\.\d+(?:\.\d+)?)/.exec(l); if (m) return m[1]; }
  return null;
};

export function nvidiaVersion(line) {
  const m = /Kernel Module(?: for [^ ]+)?\s+(\d+(?:\.\d+)+)/.exec(String(line || ''));
  return m ? m[1] : null;
}

// What this card can use: [{ id, state: 'available' | 'missing' | 'unsupported' | 'none' | 'browser', detail }] —
// available only with the files that prove it; unsupported only where it can't be (CUDA without NVIDIA, a kernel driver
// Mesa has no Vulkan driver for); missing: this card has a driver for it that isn't installed; none: nothing found and
// no known driver to suggest.
export function gpuApis(card, caps) {
  if (!caps) return [];
  const d = card?.driver || '';
  const libs = caps.libs || [];
  const out = [];
  // Vulkan
  const icds = caps.vulkan || [];
  const want = VK_FOR[d] || [];
  const icd = want.map((w) => icds.find((i) => stem(i.lib) === w || stem(i.lib).startsWith(`${w}.`))).find(Boolean);
  const loader = libs.includes('libvulkan.so.1');
  if (icd && loader) out.push({ id: 'vulkan', state: 'available', detail: `${VK_DRIVERS[stem(icd.lib)] || stem(icd.lib)}${icd.api ? ` · ${icd.api}` : ''}` });
  else if (VK_NONE.includes(d)) out.push({ id: 'vulkan', state: 'unsupported', detail: '' });
  else out.push({ id: 'vulkan', state: want.length ? 'missing' : 'none', detail: '' });
  // OpenGL / OpenGL ES: Mesa's driver for the card, or NVIDIA's
  const mesa = mesaVersion(libs);
  const gl = GL_FOR[d];
  const egl = libs.includes('libEGL.so.1') || libs.includes('libGLX.so.0');
  if (d === 'nvidia') out.push({ id: 'opengl', state: icds.some((i) => stem(i.lib) === 'libGLX_nvidia') ? 'available' : 'missing', detail: 'NVIDIA' });
  else if (gl && egl && ((caps.dri || []).includes(`${gl}_dri.so`) || mesa)) out.push({ id: 'opengl', state: 'available', detail: mesa ? `Mesa ${mesa} · ${gl}` : `Mesa · ${gl}` });
  else if (!gl && egl && ((caps.dri || []).includes('swrast_dri.so') || mesa)) out.push({ id: 'opengl', state: 'available', detail: 'llvmpipe' });
  else out.push({ id: 'opengl', state: 'missing', detail: '' });
  // OpenCL: the installed platforms (they may run on the CPU: PoCL)
  const cl = (caps.opencl || []).map((v) => stem(v.lib).toLowerCase());
  const clName = (s) => (s.includes('rusticl') ? 'Rusticl' : s.includes('mesaopencl') ? 'Clover' : s.includes('pocl') ? 'PoCL' : s.includes('nvidia') ? 'NVIDIA'
    : s.includes('amdocl') ? 'ROCm' : s.includes('igdrcl') || s.includes('intelocl') ? 'Intel' : s.replace(/^lib/, ''));
  if (cl.length && libs.includes('libOpenCL.so.1')) out.push({ id: 'opencl', state: 'available', detail: [...new Set(cl.map(clName))].join(', ') });
  else out.push({ id: 'opencl', state: 'missing', detail: '' });
  // video decode and encode: VA-API and VDPAU drivers for the card
  const va = (VA_FOR[d] || []).find((n) => (caps.va || []).includes(`${n}_drv_video.so`));
  const vdp = VDPAU_FOR[d] && (caps.vdpau || []).some((n) => n.startsWith(`libvdpau_${VDPAU_FOR[d]}.`));
  if ((va && libs.includes('libva.so.2')) || (vdp && libs.includes('libvdpau.so.1'))) out.push({ id: 'video', state: 'available', detail: [va && libs.includes('libva.so.2') ? `VA-API · ${va}` : '', vdp ? 'VDPAU' : ''].filter(Boolean).join(', ') });
  else out.push({ id: 'video', state: VA_FOR[d] || VDPAU_FOR[d] ? 'missing' : 'none', detail: '' });
  // vendors' compute stacks
  if (d === 'nvidia' || d === 'nouveau') {
    const ver = nvidiaVersion(caps.nvidia);
    const prop = d === 'nvidia';
    out.push({ id: 'cuda', state: prop && libs.includes('libcuda.so.1') ? 'available' : prop ? 'missing' : 'unsupported', detail: prop && ver ? ver : '' });
    out.push({ id: 'nvenc', state: prop && libs.includes('libnvidia-encode.so.1') ? 'available' : prop ? 'missing' : 'unsupported', detail: '' });
    out.push({ id: 'nvdec', state: prop && libs.includes('libnvcuvid.so.1') ? 'available' : prop ? 'missing' : 'unsupported', detail: '' });
  }
  if (d === 'amdgpu') {
    const hip = libs.includes('libamdhip64.so');
    out.push({ id: 'rocm', state: caps.kfd && hip ? 'available' : 'missing', detail: caps.kfd && !hip ? 'kfd' : '' });
  }
  // WebGPU: browsers bring it, on top of Vulkan
  const vk = out[0].state === 'available';
  out.push({ id: 'webgpu', state: vk ? 'browser' : 'missing', detail: '' });
  return out;
}

// The APIs a process uses, from the libraries it maps (gpustat.c keeps only the ones that tell):
// [{ id: 'vulkan' | 'opengl' | 'opengles' | 'opencl' | 'cuda' | 'hip' | 'video' | 'encode', detail }]
export function apiTags(libs, driver = '') {
  const L = (libs || []).map(String);
  const has = (re) => L.some((l) => re.test(l));
  const out = [];
  const vkIcd = L.map(stem).find((s) => VK_DRIVERS[s] && s !== 'libGLX_nvidia');
  if (has(/^libvulkan[._]/) || has(/^libggml-vulkan/)) out.push({ id: 'vulkan', detail: vkIcd ? VK_DRIVERS[vkIcd] : '' });
  const dri = L.find((l) => /_dri\.so/.test(l));
  const glDriver = dri ? dri.replace(/_dri\.so.*$/, '') : has(/^libgallium-/) ? GL_FOR[driver] || '' : has(/^libnvidia-(glcore|eglcore)/) ? 'NVIDIA' : '';
  const desktopGl = has(/^libGLX/) || has(/^libGL\.so/) || has(/^libOpenGL/);
  const gles = has(/^libGLESv2/);
  if (desktopGl) out.push({ id: 'opengl', detail: glDriver });
  else if (gles) out.push({ id: 'opengles', detail: glDriver });
  else if (has(/^libEGL/) || dri || has(/^libgallium-/)) out.push({ id: 'opengl', detail: glDriver });
  if (has(/^lib(OpenCL|RusticlOpenCL|MesaOpenCL)/)) out.push({ id: 'opencl', detail: '' });
  if (has(/^libcuda\.so/) || has(/^libggml-cuda/)) out.push({ id: 'cuda', detail: '' });
  if (has(/^libamdhip64/) || has(/^libggml-hip/)) out.push({ id: 'hip', detail: '' });
  const vaDrv = L.find((l) => /_drv_video\.so/.test(l));
  if (has(/^libva(\.|-)/) || vaDrv || has(/^libvdpau/) || has(/^libnvcuvid/)) out.push({ id: 'video', detail: vaDrv ? vaDrv.replace(/_drv_video\.so.*$/, '') : has(/^libvdpau/) ? 'VDPAU' : has(/^libnvcuvid/) ? 'NVDEC' : '' });
  if (has(/^libnvidia-encode/)) out.push({ id: 'encode', detail: 'NVENC' });
  return out;
}

// ---- LordOS's AI engines ----

// The helpers LordOS starts for AI work. gpu: whether the engine can use the GPU at all.
export const AI_ENGINES = [
  { id: 'voice', bins: ['lord-tts'], front: 'lord-say', gpu: true },
  { id: 'ocr', bins: ['lord-ocr'], gpu: false },
];

// [{ id, pid, backend: 'gpu' | 'cpu' | null, api: 'vulkan' | null, quality, lang, state, mem, hidden }]: the engines
// running now. The voice's backend, quality and language come from lord-say's note (its helper's /proc is closed: it
// runs sandboxed and not dumpable, so its GPU memory is hidden unless the kernel lets us read it).
export function aiEngines(raw, apps = []) {
  const procs = raw?.ai || [];
  const out = [];
  for (const e of AI_ENGINES) {
    const helpers = procs.filter((p) => e.bins.includes(p.name) && p.state !== 'Z');
    if (!helpers.length) continue;
    const h = helpers[0];
    const say = e.front ? (raw.say || []).find((s) => s.pid === h.ppid) || null : null;
    const front = e.front ? procs.find((p) => p.pid === h.ppid && p.name === e.front) : null;
    let mem = null;
    for (const a of apps) if (a.pids.includes(h.pid)) mem = a.mem;
    const backend = say ? (say.backend === 'vulkan' ? 'gpu' : 'cpu') : e.gpu ? null : 'cpu';
    const state = front?.state === 'T' || h.state === 'T' ? 'paused' : say?.mode === 'save' ? 'saving' : say ? 'speaking' : 'working';
    out.push({ id: e.id, pid: h.pid, backend, api: backend === 'gpu' ? 'vulkan' : null, quality: say?.quality || null, lang: say?.lang || null,
      state, mem, hidden: mem == null && !h.own });
  }
  return out;
}

// ---- the step ----

export function createGpuState() { return { clients: new Map(), energy: new Map(), vms: new Map(), mono: 0 }; }

const appKeyOf = (p) => (p.self ? 'app:lordos' : p.app ? `app:${p.app}` : `name:${p.name || p.pid}`);

export function gpuStep(state, raw) {
  if (!raw || !Array.isArray(raw.cards)) return null;
  const mono = num(raw.mono) || 0;
  const dtNs = state.mono && mono > state.mono ? (mono - state.mono) * 1e6 : 0;
  const caps = raw.caps || null;
  // clients: shares since the previous sample
  const procs = dedupeClients(raw.list);
  const nextClients = new Map();
  const perCard = new Map();                     // pdev → { category: share } summed over the apps (estimate)
  const groups = new Map();
  for (const p of procs) {
    if (!p.clients.length) continue;
    const key = appKeyOf(p);
    let g = groups.get(key);
    if (!g) groups.set(key, (g = { key, item: p, count: 0, pids: [], gpus: new Set(), engines: {}, cats: {}, total: 0, mem: null, sysMem: null, apis: [] }));
    g.count++;
    g.pids.push(p.pid);
    if (p.self) g.item = p;
    for (const c of p.clients) {
      const k = clientKey(c);
      const shares = k ? engineShares(state.clients.get(k), c, dtNs) : {};
      if (k) nextClients.set(k, { engines: c.engines, cycles: c.cycles, totalCycles: c.totalCycles });
      if (c.pdev) g.gpus.add(c.pdev);
      for (const [e, v] of Object.entries(shares)) {
        g.engines[e] = Math.min(1, (g.engines[e] || 0) + v);
        const cat = engineCategory(e);
        g.cats[cat] = Math.min(1, (g.cats[cat] || 0) + v);
        const pc = perCard.get(c.pdev || '') || {};
        pc[e] = Math.min(1, (pc[e] || 0) + v);
        perCard.set(c.pdev || '', pc);
      }
      const m = clientMem(c);
      if (m) { g.mem = (g.mem || 0) + m.device; g.sysMem = (g.sysMem || 0) + m.system; }
    }
    for (const a of apiTags(p.libs, p.clients[0]?.driver)) if (!g.apis.some((x) => x.id === a.id)) g.apis.push(a);
  }
  state.clients = nextClients;
  const apps = [...groups.values()].map((g) => ({ ...g, gpus: [...g.gpus], total: Math.max(0, ...Object.values(g.engines)) }))
    .sort((a, b) => b.total - a.total || (b.mem || 0) + (b.sysMem || 0) - ((a.mem || 0) + (a.sysMem || 0)) || a.key.localeCompare(b.key));
  // cards
  const cards = raw.cards.map((c) => {
    const key = c.slot || c.card;
    let powerW = num(c.powerW);
    if (powerW == null && num(c.energyUj) != null) {
      const e = state.energy.get(key);
      if (e && mono > e.mono && c.energyUj >= e.uj) powerW = (c.energyUj - e.uj) / ((mono - e.mono) * 1e3);
      state.energy.set(key, { mono, uj: c.energyUj });
    }
    const est = perCard.get(c.slot) || (raw.cards.length === 1 ? perCard.get('') : null) || null;
    const busy = num(c.busy) != null ? c.busy / 100 : est && Object.keys(est).length ? Math.max(...Object.values(est)) : null;
    const t = hottest(c);
    const kind = gpuKind(c);
    return {
      key, card: c.card, driver: c.driver, slot: c.slot, name: gpuName(c), kind, render: !!c.render,
      busy, busySource: num(c.busy) != null ? 'driver' : busy != null ? 'apps' : null, engines: est || {},
      vram: num(c.vramTotal) > 0 ? { used: num(c.vramUsed), total: c.vramTotal } : null,
      gtt: num(c.gttTotal) > 0 ? { used: num(c.gttUsed), total: c.gttTotal } : null,
      shared: kind === 'integrated' && !(num(c.vramTotal) > 0),
      temp: t ? t.c : null, tempLabel: t ? t.label : null, temps: c.temps || [],
      fan: fanState(c), powerW, powerCapW: num(c.powerCapW),
      sclk: num(c.sclk) ?? num(c.curMhz), sclkMax: num(c.sclkMax) ?? num(c.maxMhz), mclk: num(c.mclk), mclkMax: num(c.mclkMax),
      apis: gpuApis(c, caps),
      apps: apps.filter((a) => a.gpus.includes(c.slot) || (raw.cards.length === 1 && a.gpus.length)).map((a) => a.key),
    };
  });
  // virtual machines: their CPU (share of one core) and memory
  const hz = num(raw.hz) || 100;
  const vmsNext = new Map();
  let vcpu = 0, vrss = 0;
  for (const v of raw.vms || []) {
    const p = state.vms.get(v.pid);
    if (p && dtNs > 0 && v.ticks >= p) vcpu += (v.ticks - p) / hz / (dtNs / 1e9);
    vmsNext.set(v.pid, v.ticks);
    vrss += v.rss || 0;
  }
  state.vms = vmsNext;
  state.mono = mono;
  return { t: raw.t, cards, apps, ai: aiEngines(raw, apps), vms: { count: (raw.vms || []).length, cpu: dtNs > 0 ? vcpu : null, rss: vrss }, caps };
}

// ---- history and the Overview ----

// One point per card per sample: { t, busy (0..100), temp, fan (0..100), rpm, vram, power, sclk }
export function pushHistory(hist, view, cap = GPU_HISTORY) {
  const next = { ...hist };
  for (const c of view?.cards || []) {
    const pt = { t: view.t, busy: c.busy == null ? null : Math.round(c.busy * 1000) / 10, temp: c.temp, fan: c.fan?.pct ?? null, rpm: c.fan?.rpm ?? null,
      vram: c.vram?.used ?? null, power: c.powerW, sclk: c.sclk };
    const old = next[c.key] || [];
    next[c.key] = old.length >= cap ? [...old.slice(old.length - cap + 1), pt] : [...old, pt];
  }
  return next;
}

// The card the Overview shows: the first graphics card, else the first that renders; `more` how many others.
export function mainCard(view) {
  const cards = (view?.cards || []).filter((c) => c.kind !== 'display');
  const main = cards.find((c) => c.kind === 'discrete') || cards.find((c) => c.render) || cards[0] || null;
  return { main, more: Math.max(0, cards.length - (main ? 1 : 0)) };
}

// The app using a card the most (its busiest engine, else its memory), or null.
export function topApp(view, card) {
  if (!card) return null;
  return (view?.apps || []).find((a) => card.apps.includes(a.key) && (a.total > 0.005 || (a.mem || 0) > 0)) || null;
}
