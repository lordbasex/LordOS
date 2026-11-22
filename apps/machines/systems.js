// systems.js — the systems the person adds to New Machine themselves ("Added by you", Machines settings → Systems;
// docs/VIRTUALIZATION.md "Profiles and your own systems") and the optional official logos (PNG only): pure functions,
// tested by server/tests/vm_profiles_test.mjs. A system is only data: a name, a kind, a color, a default profile, how
// it installs, and its image, either an ISO the person picked on this computer or an https address with the image's
// SHA-256 (required), optionally with an OpenPGP signature over a checksum file and the project's public key. Nothing
// here downloads, opens or runs anything: the admin helper downloads (vm-image-get-user, which checks the address again
// in C: vmurl.c) and lord-pgpv checks the signature (vmsig.c). An image is usable only once its SHA-256 matched and, when
// the system has a key, its signature verified (verified), and the New Machine assistant hashes it again before a
// machine is made from it.

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
export const URL_MAX = 1000;                           // the admin helper's limit (vmurl.h VMURL_MAX)
export const IMAGE_MAX = 16 * 1024 * 1024 * 1024;       // 16 GiB: no installer is larger (vmurl.h VMURL_IMAGE_MAX)
export const KEY_MAX = 32 * 1024;                     // an armored public key (vmsig.h VMSIG_KEY_MAX)
// where the admin helper keeps downloaded images (one copy for every account, named by its SHA-256)
export const STORE_IMAGES = '/var/lib/lordos/machines/images';
export const ID_RE = /^[a-z][a-z0-9-]{0,31}$/;
const SHA_RE = /^[0-9a-f]{64}$/;
// an OpenPGP fingerprint: v4 (40 hex digits) or v6 (64), spaces allowed as people copy them
const FPR_RE = /^([0-9A-F]{40}|[0-9A-F]{64})$/;
const CTRL = /[\u0000-\u001f\u007f-\u009f\u2028\u2029\u202a-\u202e\u2066-\u2069]/;

const str = (v) => (typeof v === 'string' ? v : '');
export const cleanText = (v, max) => (typeof v === 'string' && !CTRL.test(v) ? v.trim().slice(0, max) : null);
export const shaOf = (v) => { const s = str(v).trim().toLowerCase(); return SHA_RE.test(s) ? s : ''; };
export const fprOf = (v) => { const s = str(v).replace(/\s+/g, '').toUpperCase(); return FPR_RE.test(s) ? s : ''; };

// suffixes that only mean something on a LAN or in a test (pkg/pubhost.c LOCAL_TLDS)
const LOCAL_TLDS = ['local', 'localhost', 'localdomain', 'internal', 'intranet', 'lan', 'home', 'corp', 'private', 'arpa', 'test', 'invalid', 'example', 'onion', 'alt'];
// a public DNS name, as the admin helper checks it (pubhost_name_ok): 2+ labels of letters, digits and '-', at most 253
// bytes, the last one letters only (or an IDN xn-- label), never an IP literal, localhost or a local suffix
export function publicHost(h) {
  const s = str(h).toLowerCase();
  if (s.length < 4 || s.length > 253) return false;
  const labels = s.split('.');
  if (labels.length < 2 || labels.length > 127) return false;
  if (!labels.every((l) => /^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$/.test(l))) return false;
  const tld = labels[labels.length - 1];
  if (!/^xn--[a-z0-9-]+$/.test(tld) && !/^[a-z]{2,63}$/.test(tld)) return false;
  return !LOCAL_TLDS.includes(tld);
}

// An https address a download may come from, the rules of the admin helper's vm-image-get-user (vmurl.h): https only,
// a public DNS name (no IP address, localhost or local network name), no user name or password, port 443 only, a path
// of plain URL characters with %XX escapes (never %00) and no "." or ".." part, no fragment, at most URL_MAX characters.
// → the address (host in lower case, without :443), or '' with why in err.code ('http', 'host', 'url').
export function httpsUrl(v, err = {}) {
  const s = str(v).trim();
  err.code = '';
  if (!s || s.length > URL_MAX || /[^\x21-\x7e]/.test(s) || /[<>"\\^`{|}#]/.test(s)) { err.code = 'url'; return ''; }
  if (/^http:/i.test(s)) { err.code = 'http'; return ''; }
  const m = /^https:\/\/([^/?#]+)(\/[^?#]*)(\?[^#]*)?$/i.exec(s);
  if (!m) { err.code = 'url'; return ''; }
  const auth = m[1];
  if (auth.includes('@')) { err.code = 'url'; return ''; }
  const hp = /^([^:]+)(:443)?$/.exec(auth);
  if (!hp) { err.code = 'url'; return ''; }
  if (!publicHost(hp[1])) { err.code = 'host'; return ''; }
  const pchar = /^(?:[A-Za-z0-9\-._~!$&'()*+,;=:@]|%(?!00)[0-9A-Fa-f]{2})*$/;
  const segs = m[2].slice(1).split('/');
  if (!segs[segs.length - 1] || segs.some((x) => !pchar.test(x) || /^(?:\.|%2e){1,2}$/i.test(x))) { err.code = 'url'; return ''; }
  if (m[3] && !/^\?(?:[A-Za-z0-9\-._~!$&'()*+,;=:@/?]|%(?!00)[0-9A-Fa-f]{2})*$/.test(m[3])) { err.code = 'url'; return ''; }
  return `https://${hp[1].toLowerCase()}${m[2]}${m[3] || ''}`;
}
// an address for a log or a message: never its query (it may hold a token), nor a user name or password
export function redactUrl(url) {
  const m = /^([a-z][a-z0-9+.-]*:\/\/)?(?:[^/@?#]*@)?([^?#]*)([?#][^]*)?$/i.exec(str(url));
  return m ? `${m[1] || ''}${m[2].replace(/[\u0000-\u001f\u007f]/g, '?')}${m[3] ? '?…' : ''}` : '';
}
export const hostOf = (url) => (/^https:\/\/([^/:?#]+)/.exec(str(url)) || ['', ''])[1];
// the file an address names (its last path part, decoded), '' when it isn't a plain name
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

// An armored OpenPGP public key block, as the person pastes it from the project's page: the armor's first and last
// lines and only base64 between them, at most KEY_MAX characters. → the text, or ''.
export function keyOf(v) {
  const s = str(v).replace(/\r\n?/g, '\n').trim();
  if (!s || s.length > KEY_MAX) return '';
  const m = /^-----BEGIN PGP PUBLIC KEY BLOCK-----\n((?:[A-Za-z][\w-]*: [^\n]*\n)*)\n?([A-Za-z0-9+/=\n]+)-----END PGP PUBLIC KEY BLOCK-----$/.exec(s);
  return m ? `${s}\n` : '';
}

// An optional signature: a detached OpenPGP signature over a checksum file, the project's key fingerprint and, to check
// it, the project's public key (armored). The admin helper downloads the two files with the image (vm-image-get-user);
// lord-pgpv checks them with that key, of which only the primary key with that fingerprint counts (v4, 40 hex: a v6 one
// is stored but can't be checked); the image's SHA-256 must then be a line of the checksum file. pinned: the fingerprint
// the first good check used, kept for that system (never from a profile): a later key with another fingerprint isn't
// used until the person trusts it.
export function signatureOf(o, err = {}, opts = {}) {
  err.code = '';
  if (o == null) return null;
  if (typeof o !== 'object' || Array.isArray(o)) { err.code = 'signature'; return null; }
  const e = {};
  const url = httpsUrl(o.url, e), sums = httpsUrl(o.sums, e), fpr = fprOf(o.fingerprint);
  if (!url || !sums || !fpr) { err.code = 'signature'; return null; }
  const out = { url, sums, fingerprint: fpr };
  if (o.key != null && o.key !== '') {
    const key = keyOf(o.key);
    if (!key) { err.code = 'key'; return null; }
    out.key = key;
  }
  const pin = fprOf(o.pinned);
  if (pin && !opts.fromProfile) out.pinned = pin;
  return out;
}
// what a signature allows now: 'none' (no signature), 'noKey' (stored, not checked: no key), 'unsupported' (a v6
// fingerprint), 'keyChanged' (the key isn't the one pinned for this system), 'check'
export function signatureState(sig) {
  if (!sig) return 'none';
  if (!sig.key) return 'noKey';
  if (sig.fingerprint.length !== 40) return 'unsupported';
  if (sig.pinned && sig.pinned !== sig.fingerprint) return 'keyChanged';
  return 'check';
}
// the admin helper's argument for a system's download: "<url> <sha256> <size|-> [<signature url> <checksum file url>]"
export function userGetArg(s) {
  const e = {};
  const url = httpsUrl(s?.url, e), sha = shaOf(s?.sha256);
  if (!url || !sha) return '';
  const size = Number.isSafeInteger(s.size) && s.size > 0 && s.size <= IMAGE_MAX ? String(s.size) : '-';
  const sig = s.signature && signatureState(s.signature) === 'check' ? ` ${httpsUrl(s.signature.url)} ${httpsUrl(s.signature.sums)}` : '';
  return `${url} ${sha} ${size}${sig}`;
}
// lord.vm.job(id).out of a signature check (lord-server builds it: ui/js_vm.c sig_verdict) → { valid, fingerprint } or
// { valid: false, reason }
export function sigVerdict(out) {
  let o = null;
  try { o = JSON.parse(str(out)); } catch (e) { o = null; }
  if (o && o.valid === true && fprOf(o.fingerprint).length === 40) return { valid: true, fingerprint: fprOf(o.fingerprint) };
  const reason = o && typeof o.reason === 'string' && /^[a-zA-Z]{1,24}$/.test(o.reason) ? o.reason : 'badSignature';
  return { valid: false, reason };
}

const SYSTEM_KEYS = ['id', 'name', 'kind', 'color', 'profile', 'install', 'source', 'path', 'url', 'sha256', 'size', 'signature', 'logo', 'added', 'verified', 'sigOk'];

// A system as the person or a profile gives it → { ok, system, errors: [code], ignored: [field] }. Codes: 'name',
// 'kind', 'install', 'source', 'path', 'url', 'http', 'sha', 'size', 'signature', 'logoUrl', 'logoSha', 'id'. Unknown
// fields are ignored and listed. opts: { home, profiles: [ids], fromProfile (only an https image), newId }. Codes also
// 'host' (not a public name) and 'key' (not an armored public key).
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
  s.signature = signatureOf(o.signature, se, { fromProfile: opts.fromProfile });
  if (se.code) errors.push(se.code);
  const le = {};
  s.logo = logoOf(o.logo, le);
  if (le.code) errors.push(le.code);
  if (typeof o.added === 'string' && /^\d{4}-\d{2}-\d{2}T[0-9:.]+Z$/.test(o.added) && !opts.fromProfile) s.added = o.added;
  // verified: where the image is that matched its SHA-256, and when (a local file's own path, or the admin helper's
  // image store for a download: images/<its sha256>.iso)
  const v = o.verified;
  const vpath = v && typeof v === 'object' ? str(v.path) : '';
  if (!opts.fromProfile && vpath && (isoPathOk(vpath, opts.home) || (s.sha256 && vpath === `${STORE_IMAGES}/${s.sha256}.iso`))
    && typeof v.at === 'string' && v.at.length <= 40) s.verified = { path: vpath, at: v.at };
  // sigOk: the signature that verified the downloaded image, by which key
  const g = o.sigOk;
  if (!opts.fromProfile && s.verified && s.signature && g && typeof g === 'object' && fprOf(g.fingerprint) === s.signature.fingerprint
    && typeof g.at === 'string' && g.at.length <= 40) s.sigOk = { fingerprint: s.signature.fingerprint, at: g.at };
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
