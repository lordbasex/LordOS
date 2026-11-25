// profiles.js — profiles, the starting values of a new machine (Machines settings → Profiles; docs/VIRTUALIZATION.md
// "Profiles and your own systems"): the five of LordOS and the person's own, the .lordprofile file they are shared
// as, and what each gives on this computer. Pure functions, tested by server/tests/vm_profiles_test.mjs.
//
// A profile is only values, in six parts: identity, system, resources and graphics, network, integration, keyboard and
// mouse. Reading one (a file, an import) keeps known fields only, checks every value (ranges, ports, enums, a shared
// folder under the person's home), lists what it ignored, and fits it to this computer: never more than half of its
// processors, never more memory than machines may use here, a graphics card only when one is set up for machines.
import { NET_MODES, PORTS_MAX, portRule, portProblems, usesPorts, CLOSE_CHOICES, GPU_CHOICES, LIMITS, vcpuDefault } from './model.js';
import { CARD_COLORS, SYSTEM_KINDS, ID_RE, cleanText, validateSystem, logoOf } from './systems.js';

export const PROFILE_FORMAT = 'lordos-machine-profile/1';
export const BUILTIN = ['productivity', 'development', 'games', 'design', 'light'];
export const DEFAULT_PROFILE = 'productivity';
export const PROFILE_FILE_MAX = 64 * 1024;
export const PROFILES_MAX = 64;
export const CLIPBOARDS = ['off', 'both', 'to-guest', 'from-guest'];
export const MOUSE = ['auto', 'capture'];
export const SHORTCUTS = ['ask', 'never'];
export const SUGGESTED_MAX = 8;
export const DESC_MAX = 280;
const SHARE_MAX = 512;

// LordOS's five (the approved mock "Settings"): what each asks for; 'half' is half of this computer's physical cores, six
// at most (model.js vcpuDefault)
const BUILT = {
  productivity: { color: 'violet', kind: 'linux', cpus: 4, memoryMb: [8192, 1 / 3], diskGb: 64, graphics: '3d', gpu: '', network: 'shared',
    clipboard: 'both', transfer: true, appsMode: false, onClose: 'ask', mouse: 'auto', captureFullscreen: false },
  development: { color: 'blue', kind: 'linux', cpus: 'half', memoryMb: [16384, 1 / 2], diskGb: 80, graphics: '2d', gpu: '', network: 'shared',
    clipboard: 'both', transfer: true, appsMode: false, onClose: 'ask', mouse: 'auto', captureFullscreen: false },
  games: { color: 'red', kind: 'windows', cpus: 'half', memoryMb: [16384, 1 / 2], diskGb: 128, graphics: '3d', gpu: 'intel-shared', network: 'shared',
    clipboard: 'both', transfer: false, appsMode: false, onClose: 'ask', mouse: 'capture', captureFullscreen: true },
  design: { color: 'orange', kind: 'linux', cpus: 'half', memoryMb: [32768, 1 / 2], diskGb: 80, graphics: '3d', gpu: '', network: 'shared',
    clipboard: 'both', transfer: true, appsMode: false, onClose: 'ask', mouse: 'auto', captureFullscreen: false },
  light: { color: 'green', kind: 'linux', cpus: 2, memoryMb: [4096, 1 / 2], diskGb: 40, graphics: '2d', gpu: '', network: 'shared',
    clipboard: 'off', transfer: false, appsMode: false, onClose: 'ask', mouse: 'auto', captureFullscreen: false },
};

// half of this computer's processors (at least one, never more than it recommends for one machine)
export const cpuCap = (host) => {
  const cpus = Math.max(1, host?.cpus | 0 || 2);
  const rec = Math.max(1, host?.vcpusRecommended | 0 || cpus);
  return Math.max(1, Math.min(rec, Math.floor(cpus / 2)));
};
// the memory machines may use here (MB, a multiple of 256)
export const memCap = (host) => Math.max(LIMITS.memMin, Math.floor((host?.memMaxMb || 2048) / 256) * 256);

// A built-in profile as a full profile (the same shape as a person's), its memory a share of this computer's RAM
export function builtinProfile(id, host) {
  const b = BUILT[id] || BUILT[DEFAULT_PROFILE];
  const ram = host?.ramMb || 8192;
  const mem = Math.max(LIMITS.memMin, Math.floor(Math.min(b.memoryMb[0], ram * b.memoryMb[1]) / 256) * 256);
  return {
    id, builtin: true, name: '', author: '', description: '', color: b.color,
    system: { kind: b.kind, suggested: [], image: null },
    resources: { cpus: b.cpus, memoryMb: mem, diskGb: b.diskGb, graphics: b.graphics, gpu: b.gpu },
    network: { mode: b.network, ports: [] },
    integration: { clipboard: b.clipboard, share: null, transfer: b.transfer, appsMode: b.appsMode, onClose: b.onClose },
    input: { mouse: b.mouse, shortcuts: 'ask', captureFullscreen: b.captureFullscreen },
  };
}

// A new profile of the person's own, from another (Duplicate) or from the default values
export function blankProfile(from, id) {
  const base = from || builtinProfile(DEFAULT_PROFILE, null);
  return { ...JSON.parse(JSON.stringify({ ...base, builtin: undefined })), id, builtin: false };
}

// a shared folder's path as a profile keeps it: "~/" and a path under the person's home, no hidden folder, no '.' or
// '..' part, no control characters
export function sharePathOk(p) {
  if (typeof p !== 'string' || p.length > SHARE_MAX || !/^~\/[^/]/.test(p) || /[\u0000-\u001f\u007f]/.test(p)) return false;
  return !p.slice(2).replace(/\/+$/, '').split('/').some((x) => x === '' || x === '.' || x === '..' || x.startsWith('.'));
}
export const shareAbs = (p, home) => (sharePathOk(p) ? `${String(home || '').replace(/\/+$/, '')}/${p.slice(2).replace(/\/+$/, '')}` : '');
// a folder under the home as a profile keeps it ('' when it isn't one)
export function shareRel(abs, home) {
  const h = String(home || '').replace(/\/+$/, '');
  const a = String(abs || '');
  if (!h || !a.startsWith(`${h}/`)) return '';
  const rel = `~/${a.slice(h.length + 1).replace(/\/+$/, '')}`;
  return sharePathOk(rel) ? rel : '';
}

const KNOWN = {
  '': ['format', 'id', 'name', 'author', 'description', 'color', 'system', 'resources', 'network', 'integration', 'input', 'logo'],
  system: ['kind', 'suggested', 'image'],
  resources: ['cpus', 'memoryMb', 'diskGb', 'graphics', 'gpu'],
  network: ['mode', 'ports'],
  integration: ['clipboard', 'share', 'transfer', 'appsMode', 'onClose'],
  'integration.share': ['path', 'writable'],
  input: ['mouse', 'shortcuts', 'captureFullscreen'],
};
const isObj = (v) => !!v && typeof v === 'object' && !Array.isArray(v);

// Reads a profile (a parsed .lordprofile, a stored one) → { ok, error, profile, adjusted, ignored, image, imageErrors }.
//   ctx: { host, gpuReady (a graphics card is set up for machines here), systems: [known system ids], home, id,
//          stored (true: the person's own file, its id kept), imported (a file from elsewhere: a port open to the
//          network becomes this computer only, 'lan') }
//   adjusted: [{ field, from, to, why }] — why: 'lan' (a port that was open to the network, now this computer only), 'half' (more than half the processors), 'memory' (more than machines may
//             use here), 'range', 'gpu' (none set up here), 'invalid' (a wrong value: the default kept), 'port' (a port
//             rule dropped), 'system' (a suggested system that isn't here)
//   ignored: the unknown fields, by path ('script', 'resources.turbo')
//   image: a system the profile brings (only an https image with its SHA-256), or null; imageErrors: why one was refused
export function readProfile(input, ctx = {}) {
  const fail = (error) => ({ ok: false, error, profile: null, adjusted: [], ignored: [], image: null, imageErrors: [] });
  let o = input;
  if (typeof input === 'string') {
    if (input.length > PROFILE_FILE_MAX) return fail('tooBig');
    try { o = JSON.parse(input); } catch (e) { return fail('json'); }
  }
  if (!isObj(o)) return fail('json');
  if (o.format !== PROFILE_FORMAT) return fail('format');
  const name = cleanText(o.name, 64);
  if (!name) return fail('name');
  const adjusted = [], ignored = [];
  const note = (field, from, to, why) => adjusted.push({ field, from, to, why });
  const walk = (obj, path) => {
    if (!isObj(obj)) return;
    for (const k of Object.keys(obj)) {
      const p = path ? `${path}.${k}` : k;
      if (!(KNOWN[path] || []).includes(k)) { ignored.push(p); continue; }
      if (KNOWN[p] && isObj(obj[k])) walk(obj[k], p);
    }
  };
  walk(o, '');
  const pick = (field, v, list, def) => {
    if (v === undefined) return def;
    if (list.includes(v)) return v;
    note(field, v, def, 'invalid');
    return def;
  };
  const bool = (field, v, def) => {
    if (v === undefined) return def;
    if (typeof v === 'boolean') return v;
    note(field, v, def, 'invalid');
    return def;
  };
  const host = ctx.host || null;
  const base = builtinProfile(DEFAULT_PROFILE, host);
  const sys = isObj(o.system) ? o.system : {};
  const res = isObj(o.resources) ? o.resources : {};
  const net = isObj(o.network) ? o.network : {};
  const integ = isObj(o.integration) ? o.integration : {};
  const inp = isObj(o.input) ? o.input : {};
  const p = {
    id: ctx.stored && ID_RE.test(String(o.id || '')) && !/^(productivity|development|games|design|light|testing)$/.test(o.id) ? o.id : ctx.id || '',
    builtin: false,
    name,
    author: cleanText(o.author, 64) || '',
    description: cleanText(o.description, DESC_MAX) || '',
    color: pick('color', o.color, CARD_COLORS, 'violet'),
    system: { kind: pick('system.kind', sys.kind, SYSTEM_KINDS, 'linux'), suggested: [], image: null },
    resources: { ...base.resources, gpu: '' },
    network: { mode: 'shared', ports: [] },
    integration: { ...base.integration, share: null },
    input: { ...base.input },
  };
  // the image a profile brings: only an https address and its SHA-256 (never a file of another computer)
  let image = null, imageErrors = [];
  if (sys.image !== undefined && sys.image !== null) {
    const r = validateSystem(sys.image, { fromProfile: true, newId: ctx.imageId || 's-new', home: ctx.home, profiles: null });
    if (r.ok) { image = { ...r.system, profile: ctx.id || r.system.profile }; ignored.push(...r.ignored.map((k) => `system.image.${k}`)); } else imageErrors = r.errors;
  }
  // suggested systems: ids of systems this computer knows (the catalog, the person's own, the one this profile brings)
  if (sys.suggested !== undefined) {
    if (!Array.isArray(sys.suggested)) note('system.suggested', sys.suggested, [], 'invalid');
    else {
      const known = ctx.systems || null;
      for (const id of sys.suggested.slice(0, SUGGESTED_MAX)) {
        const ok = typeof id === 'string' && ID_RE.test(id) && (!known || known.includes(id) || (image && id === sys.image?.id));
        if (!ok) { note('system.suggested', id, null, 'system'); continue; }
        const real = image && id === sys.image?.id ? image.id : id;
        if (!p.system.suggested.includes(real)) p.system.suggested.push(real);
      }
      if (sys.suggested.length > SUGGESTED_MAX) note('system.suggested', sys.suggested.length, SUGGESTED_MAX, 'range');
    }
  }
  if (image && !p.system.suggested.includes(image.id)) p.system.suggested.unshift(image.id);
  // resources
  if (res.cpus !== undefined) {
    if (res.cpus === 'half') p.resources.cpus = 'half';
    else if (Number.isInteger(res.cpus) && res.cpus >= 1 && res.cpus <= LIMITS.vcpusMax) p.resources.cpus = res.cpus;
    else note('resources.cpus', res.cpus, p.resources.cpus, Number.isInteger(res.cpus) ? 'range' : 'invalid');
  }
  if (host && Number.isInteger(p.resources.cpus) && p.resources.cpus > cpuCap(host)) { note('resources.cpus', p.resources.cpus, cpuCap(host), 'half'); p.resources.cpus = cpuCap(host); }
  if (res.memoryMb !== undefined) {
    if (Number.isInteger(res.memoryMb) && res.memoryMb >= LIMITS.memMin && res.memoryMb <= LIMITS.memMax) p.resources.memoryMb = Math.floor(res.memoryMb / 256) * 256 || LIMITS.memMin;
    else note('resources.memoryMb', res.memoryMb, p.resources.memoryMb, Number.isInteger(res.memoryMb) ? 'range' : 'invalid');
  }
  if (host && p.resources.memoryMb > memCap(host)) { note('resources.memoryMb', p.resources.memoryMb, memCap(host), 'memory'); p.resources.memoryMb = memCap(host); }
  if (res.diskGb !== undefined) {
    if (Number.isInteger(res.diskGb) && res.diskGb >= LIMITS.diskMin && res.diskGb <= LIMITS.diskMax) p.resources.diskGb = res.diskGb;
    else {
      const to = Number.isFinite(res.diskGb) ? Math.max(LIMITS.diskMin, Math.min(LIMITS.diskMax, Math.round(res.diskGb))) : p.resources.diskGb;
      note('resources.diskGb', res.diskGb, to, Number.isFinite(res.diskGb) ? 'range' : 'invalid');
      p.resources.diskGb = to;
    }
  }
  p.resources.graphics = pick('resources.graphics', res.graphics, ['2d', '3d'], '2d');
  const gpu = pick('resources.gpu', res.gpu, GPU_CHOICES, '');
  if (gpu && ctx.gpuReady === false) note('resources.gpu', gpu, '', 'gpu');
  else p.resources.gpu = gpu;
  // network
  p.network.mode = pick('network.mode', net.mode, NET_MODES, 'shared');
  if (net.ports !== undefined) {
    if (!Array.isArray(net.ports)) note('network.ports', net.ports, [], 'invalid');
    else {
      const rules = [];
      for (const r of net.ports.slice(0, PORTS_MAX)) {
        const ok = isObj(r) && (r.proto === undefined || r.proto === 'tcp' || r.proto === 'udp') && Number.isInteger(r.host) && Number.isInteger(r.guest)
          && (r.open === undefined || r.open === 'lan' || r.open === 'this-computer');
        const rule = ok ? portRule(r) : null;
        if (!rule || portProblems([...rules, rule], p.network.mode).length) { note('network.ports', isObj(r) ? `${r.proto || 'tcp'} ${r.host} → ${r.guest}` : r, null, 'port'); continue; }
        // an imported profile never opens a port to the network by itself: this computer only, and said
        if (ctx.imported && rule.open === 'lan') { rule.open = 'this-computer'; note('network.ports', `${rule.proto.toUpperCase()} ${rule.host} → ${rule.guest}`, 'this-computer', 'lan'); }
        rules.push(rule);
      }
      if (net.ports.length > PORTS_MAX) note('network.ports', net.ports.length, PORTS_MAX, 'range');
      p.network.ports = usesPorts(p.network.mode) ? rules : [];
      if (!usesPorts(p.network.mode) && rules.length) note('network.ports', rules.length, 0, 'port');
    }
  }
  // integration
  p.integration.clipboard = pick('integration.clipboard', integ.clipboard, CLIPBOARDS, p.integration.clipboard);
  p.integration.transfer = bool('integration.transfer', integ.transfer, p.integration.transfer);
  p.integration.appsMode = bool('integration.appsMode', integ.appsMode, p.integration.appsMode);
  p.integration.onClose = pick('integration.onClose', integ.onClose, CLOSE_CHOICES, 'ask');
  if (integ.share !== undefined && integ.share !== null) {
    const sh = integ.share;
    if (isObj(sh) && sharePathOk(sh.path) && (sh.writable === undefined || typeof sh.writable === 'boolean')) p.integration.share = { path: sh.path.replace(/\/+$/, ''), writable: !!sh.writable };
    else note('integration.share', isObj(sh) ? sh.path : sh, null, 'invalid');
  }
  // keyboard and mouse
  p.input.mouse = pick('input.mouse', inp.mouse, MOUSE, 'auto');
  p.input.shortcuts = pick('input.shortcuts', inp.shortcuts, SHORTCUTS, 'ask');
  p.input.captureFullscreen = bool('input.captureFullscreen', inp.captureFullscreen, false);
  // its picture's logo (PNG only; downloaded only with the switch on)
  if (o.logo !== undefined && o.logo !== null) {
    const le = {};
    const l = logoOf(o.logo, le);
    if (l) p.logo = l; else note('logo', o.logo?.url || o.logo, null, 'invalid');
  }
  return { ok: true, error: '', profile: p, adjusted, ignored, image, imageErrors };
}

// a system's signature as a profile carries it: the addresses, the fingerprint and the key; never this computer's pin
const exportSignature = (g) => ({ url: g.url, sums: g.sums, fingerprint: g.fingerprint, ...(g.key ? { key: g.key } : {}) });

// A profile as its file (.lordprofile, or the person's own under ~/.config/lordos/machines/profiles): values only, and
// a system it suggests that the person added by an address goes with it (address and SHA-256, never the image)
export function writeProfile(p, systems = [], { withId = false } = {}) {
  const image = (systems || []).find((s) => s.source === 'url' && p.system?.suggested?.includes(s.id)) || null;
  const out = {
    format: PROFILE_FORMAT, ...(withId ? { id: p.id } : {}),
    name: p.name, author: p.author || '', description: p.description || '', color: p.color,
    system: { kind: p.system.kind, suggested: [...(p.system.suggested || [])],
      ...(image && !withId ? { image: { id: image.id, name: image.name, kind: image.kind, color: image.color, install: image.install, url: image.url, sha256: image.sha256,
        ...(image.size ? { size: image.size } : {}), ...(image.signature ? { signature: exportSignature(image.signature) } : {}), ...(image.logo ? { logo: image.logo } : {}) } } : {}) },
    resources: { ...p.resources },
    network: { mode: p.network.mode, ports: (p.network.ports || []).map(portRule) },
    integration: { ...p.integration, share: p.integration.share ? { ...p.integration.share } : null },
    input: { ...p.input },
    ...(p.logo ? { logo: p.logo } : {}),
  };
  return JSON.stringify(out, null, 2);
}

// What a profile gives on this computer (the "On this computer" line and the New Machine assistant's starting values)
//   → { vcpus, memoryMb, diskGb, network, graphics, gpu, clipboard, transfer, appsMode, onClose, share, ports, input }
// opts: { gpuReady, no3d (why 3D can't be had: '' when it can) }
export function onHost(p, host, opts = {}) {
  const r = p?.resources || {};
  const cap = cpuCap(host);
  const vcpus = r.cpus === 'half' ? vcpuDefault(host, cap) : Math.max(1, Math.min(cap, r.cpus | 0 || 1));
  const memoryMb = Math.max(LIMITS.memMin, Math.min(memCap(host), r.memoryMb | 0 || 2048));
  return {
    vcpus, memoryMb, diskGb: Math.max(LIMITS.diskMin, Math.min(LIMITS.diskMax, r.diskGb | 0 || 32)),
    network: NET_MODES.includes(p?.network?.mode) ? p.network.mode : 'shared',
    graphics: r.graphics === '3d' && !opts.no3d ? '3d' : '2d',
    gpu: r.gpu && opts.gpuReady ? r.gpu : '',
    clipboard: CLIPBOARDS.includes(p?.integration?.clipboard) ? p.integration.clipboard : 'off',
    transfer: !!p?.integration?.transfer, appsMode: !!p?.integration?.appsMode,
    onClose: CLOSE_CHOICES.includes(p?.integration?.onClose) ? p.integration.onClose : 'ask',
    share: p?.integration?.share || null,
    ports: usesPorts(p?.network?.mode) ? (p?.network?.ports || []).map(portRule) : [],
    input: { mouse: p?.input?.mouse === 'capture' ? 'capture' : 'auto', shortcuts: p?.input?.shortcuts === 'never' ? 'never' : 'ask', captureFullscreen: !!p?.input?.captureFullscreen },
  };
}

// The changes lord.vm.set makes after lord.vm.create (which takes only CPU, memory, disk, shared or isolated network,
// the clipboard both ways and 3D): the network mode and its ports, the bridged card, the clipboard's way, the transfer
// folder, a shared folder (already a full path the person confirmed), the graphics card, and "Capture automatically in
// full screen" (machine.json capture_fullscreen is its one home, the field Configure changes; app.json keeps only the
// mouse and shortcuts choices)
export function setPatch(v, { bridge = '', shareAbsPath = '' } = {}) {
  const out = {};
  if (v.network === 'bridged' || v.network === 'hostonly') out.network = v.network;
  if (v.network === 'bridged' && bridge) out.bridge = bridge;
  if (usesPorts(v.network) && v.ports.length) out.ports = v.ports;
  if (v.clipboard !== 'off' && v.clipboard !== 'both') out.clipboard = v.clipboard;
  if (v.transfer) out.transfer = true;
  if (v.gpu) out.gpu = v.gpu;
  if (v.input?.captureFullscreen) out.captureFullscreen = true;
  if (shareAbsPath && v.share) out.shares = [{ tag: 'share', path: shareAbsPath, writable: !!v.share.writable }];
  return out;
}

// the profiles shown, in order: LordOS's five, then the person's own by name
export function allProfiles(own, host) {
  return [...BUILTIN.map((id) => builtinProfile(id, host)), ...[...(own || [])].sort((a, b) => a.name.localeCompare(b.name))];
}
export const profileById = (id, own, host) => (BUILTIN.includes(id) ? builtinProfile(id, host) : (own || []).find((p) => p.id === id) || null);
export const defaultProfileId = (v, own) => (BUILTIN.includes(v) || (own || []).some((p) => p.id === v) ? v : DEFAULT_PROFILE);
// a file name for an exported profile: its name in plain letters
export const exportName = (p) => `${String(p?.name || 'profile').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 48) || 'profile'}.lordprofile`;

// What an imported profile would open beyond the machine itself, shown in the import summary before it is kept (none of
// it happens silently): its network bridged (the person picks the card), the clipboard both ways or from the machine,
// a writable shared folder (the person picks the folder when creating). → ['bridged', 'clipBoth', 'shareWrite', …]
export function exposureOf(p) {
  const out = [];
  if (p?.network?.mode === 'bridged') out.push('bridged');
  if (p?.integration?.clipboard === 'both' || p?.integration?.clipboard === 'from-guest') out.push('clipBoth');
  if (p?.integration?.share?.writable) out.push('shareWrite');
  return out;
}
