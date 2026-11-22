// systems.js — the systems the person adds to New Machine themselves ("Added by you", Machines settings → Systems;
// docs/VIRTUALIZATION.md "Profiles and your own systems") and the optional official logos (PNG only): pure functions,
// tested by server/tests/vm_profiles_test.mjs. A system is only data: a name, a kind, a color, a default profile, how
// it installs, and its image, either an ISO the person picked on this computer or an https address with the image's
// SHA-256 (required). Nothing here downloads, opens or runs anything; an image is usable only once its SHA-256 matched
// (verified), and the New Machine assistant hashes it again before a machine is made from it.

export const SYSTEMS_FORMAT = 'lordos-machine-systems/1';
export const SYSTEM_KINDS = ['linux', 'windows', 'other'];
export const CARD_COLORS = ['blue', 'green', 'red', 'orange', 'violet', 'gray'];
// what each color paints (a card's picture before it has one, a profile's tile)
export const COLOR_HEX = { blue: ['#1f6aa5', '#7cc4e8'], green: ['#2f6b2c', '#a3b547'], red: ['#a33a2a', '#f0a060'],
  orange: ['#b0601a', '#f2b45a'], violet: ['#7a3b9c', '#e07fa8'], gray: ['#4a5060', '#9aa2b4'] };
// manual (the system's own installer) or one of the ways lord-vm installs without questions (vmunattend.h): the
// catalog's entry of that way gives the installer's kernel and initrd for the architecture (customUnattended)
export const INSTALLS = ['manual', 'preseed', 'autoinstall', 'kickstart'];
export const SYSTEMS_MAX = 64;
export const NAME_MAX = 64;
export const URL_MAX = 2048;
export const IMAGE_MAX = 64 * 1024 * 1024 * 1024;       // 64 GiB: no installer is larger
export const ID_RE = /^[a-z][a-z0-9-]{0,31}$/;
const SHA_RE = /^[0-9a-f]{64}$/;
// an OpenPGP fingerprint: v4 (40 hex digits) or v6 (64), spaces allowed as people copy them
const FPR_RE = /^([0-9A-F]{40}|[0-9A-F]{64})$/;
const CTRL = /[\u0000-\u001f\u007f-\u009f\u2028\u2029\u202a-\u202e\u2066-\u2069]/;

const str = (v) => (typeof v === 'string' ? v : '');
export const cleanText = (v, max) => (typeof v === 'string' && !CTRL.test(v) ? v.trim().slice(0, max) : null);
export const shaOf = (v) => { const s = str(v).trim().toLowerCase(); return SHA_RE.test(s) ? s : ''; };
export const fprOf = (v) => { const s = str(v).replace(/\s+/g, '').toUpperCase(); return FPR_RE.test(s) ? s : ''; };

// An https address a download may come from: https only, a host name (no user name or password in it, no spaces, no
// control characters), at most URL_MAX characters. → the address, or '' with why in err.code ('http', 'url').
export function httpsUrl(v, err = {}) {
  const s = str(v).trim();
  err.code = '';
  if (!s || s.length > URL_MAX || /[\s\u0000-\u001f\u007f<>"\\^`{|}]/.test(s)) { err.code = 'url'; return ''; }
  if (/^http:/i.test(s)) { err.code = 'http'; return ''; }
  const m = /^https:\/\/([^/?#]+)(\/[^?#]*)?(\?[^#]*)?$/i.exec(s);
  if (!m) { err.code = 'url'; return ''; }
  const host = m[1];
  if (host.includes('@') || !/^([a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z][a-z0-9-]{0,62}(:[0-9]{1,5})?$/i.test(host)) { err.code = 'url'; return ''; }
  const port = /:([0-9]+)$/.exec(host);
  if (port && (Number(port[1]) < 1 || Number(port[1]) > 65535)) { err.code = 'url'; return ''; }
  return `https://${host.toLowerCase()}${m[2] || '/'}${m[3] || ''}`;
}
export const hostOf = (url) => (/^https:\/\/([^/:?#]+)/.exec(str(url)) || ['', ''])[1];
// the file an address names (its last path part, decoded): what a browser saves it as in Downloads, '' when it isn't a
// plain name
export function fileOfUrl(url) {
  const m = /^https:\/\/[^/]+\/(?:[^?#]*\/)?([^/?#]+)$/.exec(str(url).replace(/\?.*$/, ''));
  if (!m) return '';
  let f = '';
  try { f = decodeURIComponent(m[1]); } catch (e) { return ''; }
  return /^[A-Za-z0-9_+~][A-Za-z0-9._+~-]{0,126}$/.test(f) ? f : '';
}

// a local ISO's path: absolute, under the person's home, a .iso, no '..' or '.' parts, no control characters
export function isoPathOk(path, home) {
  const p = str(path);
  const h = str(home).replace(/\/+$/, '');
  if (!h.startsWith('/') || !p.startsWith(`${h}/`) || CTRL.test(p) || p.length > 4096 || !/\.iso$/i.test(p)) return false;
  return !p.slice(h.length + 1).split('/').some((x) => x === '' || x === '.' || x === '..');
}

// A logo (PNG only, maintainer 2026-11-22): { url: https to a .png, sha256 } or null with why in err.code ('logoUrl',
// 'logoSha'). Shown only when the person turned on "Official logos of the systems"; downloaded only then.
export function logoOf(o, err = {}) {
  err.code = '';
  if (o == null) return null;
  if (typeof o !== 'object' || Array.isArray(o)) { err.code = 'logoUrl'; return null; }
  const e = {};
  const url = httpsUrl(o.url, e);
  if (!url || !/\.png$/i.test(url.replace(/\?.*$/, ''))) { err.code = e.code === 'http' ? 'http' : 'logoUrl'; return null; }
  const sha = shaOf(o.sha256);
  if (!sha) { err.code = 'logoSha'; return null; }
  return { url, sha256: sha };
}

// An optional signature: a detached OpenPGP signature over a checksum file, and the project's key fingerprint. This
// version stores it and says "not checked" (no OpenPGP verifier usable for a key the person gives: the admin helper's
// checks only the catalog's pinned keys; docs/VIRTUALIZATION.md "A signature on your own system").
export function signatureOf(o, err = {}) {
  err.code = '';
  if (o == null) return null;
  if (typeof o !== 'object' || Array.isArray(o)) { err.code = 'signature'; return null; }
  const e = {};
  const url = httpsUrl(o.url, e), sums = httpsUrl(o.sums, e), fpr = fprOf(o.fingerprint);
  if (!url || !sums || !fpr) { err.code = 'signature'; return null; }
  return { url, sums, fingerprint: fpr };
}

const SYSTEM_KEYS = ['id', 'name', 'kind', 'color', 'profile', 'install', 'source', 'path', 'url', 'sha256', 'size', 'signature', 'logo', 'added', 'verified'];

// A system as the person or a profile gives it → { ok, system, errors: [code], ignored: [field] }. Codes: 'name',
// 'kind', 'install', 'source', 'path', 'url', 'http', 'sha', 'size', 'signature', 'logoUrl', 'logoSha', 'id'. Unknown
// fields are ignored and listed. opts: { home, profiles: [ids], fromProfile (only an https image), newId }.
export function validateSystem(o, opts = {}) {
  const errors = [], ignored = [];
  if (!o || typeof o !== 'object' || Array.isArray(o)) return { ok: false, system: null, errors: ['invalid'], ignored };
  for (const k of Object.keys(o)) if (!SYSTEM_KEYS.includes(k)) ignored.push(k);
  const s = {};
  s.id = ID_RE.test(str(o.id)) && !opts.fromProfile ? o.id : opts.newId || '';
  if (!s.id) errors.push('id');
  const name = cleanText(o.name, NAME_MAX);
  if (!name) errors.push('name'); else s.name = name;
  s.kind = SYSTEM_KINDS.includes(o.kind) ? o.kind : (errors.push('kind'), 'other');
  s.color = CARD_COLORS.includes(o.color) ? o.color : 'gray';
  s.profile = ID_RE.test(str(o.profile)) && (!opts.profiles || opts.profiles.includes(o.profile)) ? o.profile : 'productivity';
  if (o.install != null && !INSTALLS.includes(o.install)) errors.push('install');
  s.install = INSTALLS.includes(o.install) ? o.install : 'manual';
  // a Windows image installs with its own installer here (no answer file for an image that isn't Microsoft's listed one)
  if (s.kind !== 'linux' && s.install !== 'manual') { errors.push('install'); s.install = 'manual'; }
  const source = opts.fromProfile ? 'url' : o.source;
  if (source !== 'file' && source !== 'url') errors.push('source');
  s.source = source === 'file' ? 'file' : 'url';
  if (s.source === 'file') {
    if (!isoPathOk(o.path, opts.home)) errors.push('path'); else s.path = o.path;
  } else {
    const e = {};
    const url = httpsUrl(o.url, e);
    if (!url) errors.push(e.code); else if (!fileOfUrl(url)) errors.push('url'); else s.url = url;
  }
  const sha = shaOf(o.sha256);
  if (!sha) errors.push('sha'); else s.sha256 = sha;
  if (o.size != null) {
    if (!Number.isSafeInteger(o.size) || o.size <= 0 || o.size > IMAGE_MAX) errors.push('size'); else s.size = o.size;
  }
  const se = {};
  s.signature = signatureOf(o.signature, se);
  if (se.code) errors.push(se.code);
  const le = {};
  s.logo = logoOf(o.logo, le);
  if (le.code) errors.push(le.code);
  if (typeof o.added === 'string' && /^\d{4}-\d{2}-\d{2}T[0-9:.]+Z$/.test(o.added) && !opts.fromProfile) s.added = o.added;
  // verified: where the image is that matched its SHA-256, and when (a local file's own path; a download in Downloads)
  if (!opts.fromProfile && o.verified && typeof o.verified === 'object' && isoPathOk(o.verified.path, opts.home)
    && typeof o.verified.at === 'string' && o.verified.at.length <= 40) s.verified = { path: o.verified.path, at: o.verified.at };
  return { ok: errors.length === 0, system: errors.length ? null : s, errors: [...new Set(errors)], ignored };
}

// systems.json → the valid systems (a broken entry is left out, never repaired into something else), at most SYSTEMS_MAX
export function readSystems(text, opts = {}) {
  let j = null;
  try { j = typeof text === 'string' ? JSON.parse(text) : text; } catch (e) { return []; }
  if (!j || j.format !== SYSTEMS_FORMAT || !Array.isArray(j.systems)) return [];
  const out = [], seen = new Set();
  for (const o of j.systems.slice(0, SYSTEMS_MAX)) {
    const r = validateSystem(o, opts);
    if (r.ok && !seen.has(r.system.id)) { seen.add(r.system.id); out.push(r.system); }
  }
  return out;
}
export const writeSystems = (list) => JSON.stringify({ format: SYSTEMS_FORMAT, systems: (list || []).slice(0, SYSTEMS_MAX) }, null, 2);

// a new id for a system ('s-' and 8 hex digits) that isn't taken
export function newId(prefix, taken = [], rnd = Math.random) {
  for (let i = 0; i < 100; i++) {
    const id = `${prefix}-${Math.floor(rnd() * 0xffffffff).toString(16).padStart(8, '0')}`;
    if (!taken.includes(id)) return id;
  }
  return `${prefix}-${Date.now().toString(16).slice(-8)}`;
}

// a system's state in Settings → Systems and New Machine: 'ready' (its SHA-256 matched), 'pick' (a local file not
// checked yet), 'download' (an address not downloaded yet)
export const systemState = (s) => (s?.verified ? 'ready' : s?.source === 'file' ? 'pick' : 'download');

// how a system installs without questions on an architecture: the catalog's entry of that way gives the kernel and the
// initrd (preseed → Debian's, autoinstall → Ubuntu's, kickstart → Fedora's) — lord-vm checks it all again. null: manual
export function customUnattended(s, cat, arch) {
  if (!s || s.kind !== 'linux' || !INSTALLS.includes(s.install) || s.install === 'manual') return null;
  for (const e of Array.isArray(cat?.systems) ? cat.systems : []) {
    const u = e?.unattended;
    const a = u?.arch?.[arch];
    if (u?.kind === s.install && a && typeof a.kernel === 'string' && typeof a.initrd === 'string') {
      return { kind: u.kind, kernel: a.kernel, initrd: a.initrd, append: String(u.append || ''), release: '' };
    }
  }
  return null;
}

// A downloaded file in Downloads that may be a system's image: the address's own name first, then a browser's renamed
// copy ("name (1).iso"); only regular files. entries: fs.list of Downloads.
export function downloadCandidates(s, entries) {
  const f = fileOfUrl(s?.url);
  if (!f) return [];
  const stem = f.replace(/\.iso$/i, '');
  const re = new RegExp(`^${stem.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}( ?\\(\\d{1,3}\\)|-\\d{1,3})?\\.iso$`, 'i');
  return (Array.isArray(entries) ? entries : []).filter((e) => e && !e.dir && !e.link && typeof e.name === 'string' && re.test(e.name)
    && (!s.size || !e.size || e.size === s.size)).sort((a, b) => (a.name === f ? -1 : b.name === f ? 1 : (b.mtime || 0) - (a.mtime || 0)));
}

// ---- official logos (PNG only; opt-in: settings machinesLogos) ----

export const LOGO_MAX_BYTES = 512 * 1024;
export const LOGO_MAX_SIDE = 1024;
// A file's first bytes as a PNG: the signature, then IHDR as the first chunk with a sane width and height. → { w, h }
// or null. Anything else (SVG, JPEG, an HTML error page, a PNG larger than LOGO_MAX_SIDE) is refused before the
// sandboxed re-encode ever sees it.
export function pngHeader(bytes) {
  const b = bytes instanceof Uint8Array ? bytes : bytes instanceof ArrayBuffer ? new Uint8Array(bytes) : null;
  if (!b || b.length < 33 || b.length > LOGO_MAX_BYTES) return null;
  const sig = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];
  for (let i = 0; i < 8; i++) if (b[i] !== sig[i]) return null;
  const u32 = (o) => ((b[o] << 24) >>> 0) + (b[o + 1] << 16) + (b[o + 2] << 8) + b[o + 3];
  if (u32(8) !== 13 || String.fromCharCode(b[12], b[13], b[14], b[15]) !== 'IHDR') return null;
  const w = u32(16), h = u32(20), depth = b[24], color = b[25];
  if (w < 1 || h < 1 || w > LOGO_MAX_SIDE || h > LOGO_MAX_SIDE) return null;
  if (![1, 2, 4, 8, 16].includes(depth) || ![0, 2, 3, 4, 6].includes(color) || b[26] !== 0 || b[27] !== 0 || b[28] > 1) return null;
  return { w, h };
}
// where a logo is kept on this computer once re-encoded: the person's cache, by its SHA-256
export const LOGOS_DIR = '.cache/lordos/machines/logos';
export const logoFile = (home, sha) => (shaOf(sha) ? `${str(home).replace(/\/+$/, '')}/${LOGOS_DIR}/${shaOf(sha)}.png` : '');
