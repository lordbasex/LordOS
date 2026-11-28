// catalog.js — the systems the New Machine assistant downloads (assets/machines/systems.json; docs/VIRTUALIZATION.md
// 4.1 and 3.9, "The Linux download cards"): pure functions, tested by server/tests/vm_catalog_test.mjs. The admin
// helper fetches the makers' lists and the images (vm-image-list, vm-image-get) and checks again everything chosen
// here: the app only reads a list and names the newest file in it, with its SHA-256.
import { hostOf, redactUrl } from './systems.js';

// the cards for an architecture, in the catalog's order: { id, name, release, approxMb, kind }
// opts: { kvm, hostArch } (lord.vm.host()): a group's card shows only with KVM on the host's own architecture
export function systemsFor(cat, arch, opts) {
  const out = [];
  for (const s of Array.isArray(cat?.systems) ? cat.systems : []) {
    const a = s?.arch?.[arch];
    // Windows (4.3) has no folder of its own: the person's ISO from Microsoft's page, or the pinned evaluation
    const win = s?.kind === 'windows' && Array.isArray(a?.isos);
    // an ISO whose maker signs no list: the catalog pins its one file and hash itself (pinnedImage)
    const pinned = (s?.kind || 'iso') === 'iso' && !!pinnedImage(a);
    if (!a || s.hidden || typeof s.id !== 'string' || (typeof a.file !== 'string' && !win && !pinned)) continue;
    // a machine group (Whonix, docs/VIRTUALIZATION.md 19.8): its card needs KVM on this computer's own architecture
    const group = s.kind === 'group';
    if (group && (!opts || !opts.kvm || opts.hostArch !== arch)) continue;
    // family: what a profile's system kind suggests it for ('linux', 'windows', or "family" in its entry)
    const family = s.kind === 'windows' ? 'windows' : group ? 'group' : /^[a-z]+$/.test(s.family || '') ? s.family : 'linux';
    out.push({ id: s.id, name: String(s.name || s.id), release: String(s.release || ''), approxMb: Number(a.approxMb) || 0, kind: s.kind || 'iso', family,
      listed: !a.sums && !pinned, ...(pinned ? { pinned: true } : {}), ...(group ? { group: String(s.group || ''), members: groupMachines(s) } : {}) });
  }
  return out;
}

// the one image an entry pins (vmcatalog.h: no maker's list; the admin helper keeps the download only with this hash):
// { name, sha, size, pinned: true } or null
const PIN_ISO = /^[A-Za-z0-9_+~][A-Za-z0-9._+~-]{0,90}\.iso$/;
export function pinnedImage(a) {
  const f = Array.isArray(a?.pinned) && a.pinned.length === 1 ? a.pinned[0] : null;
  if (!f || !PIN_ISO.test(f.name || '') || !/^[0-9a-f]{64}$/.test(f.sha256 || '') || !/^https:\/\/[^/\s"<>\\]+\/\S+$/.test(f.url || '') ||
      !Number.isInteger(f.bytes) || f.bytes < 1) return null;
  return { name: f.name, sha: f.sha256, size: f.bytes, pinned: true };
}

export function entryOf(cat, id, arch) {
  const s = (cat?.systems || []).find((x) => x?.id === id);
  return s?.arch?.[arch] ? { ...s.arch[arch], id, name: s.name, release: s.release, hash: s.hash === 'sha512' ? 'sha512' : 'sha256', kind: s.kind || 'iso' } : null;
}

// ---- machine groups (docs/VIRTUALIZATION.md 19.6, 19.8) ----
// a group entry's members as the catalog sets them: [{ role, vcpus, memoryMb }], the gateway first
export function groupMachines(s) {
  const m = s?.machine || {};
  return ['gateway', 'workstation'].filter((r) => m[r]).map((r) => ({ role: r, vcpus: Number(m[r].vcpus) || 1, memoryMb: Number(m[r].memoryMb) || 1024 }));
}
// the release folders a "releases" listing links to (href="18.2.4.3/"), matching the entry's pattern
export function releaseNames(html, pattern) {
  const r = new RegExp(pattern || '^$');
  const out = [];
  for (const m of String(html || '').matchAll(/href="([0-9][0-9.]{0,30})\/"/g)) if (r.test(m[1]) && !out.includes(m[1])) out.push(m[1]);
  return out;
}
// "18.2.4.3" against "18.2.10.1", by numbers
export function releaseCmp(a, b) {
  const x = String(a).split('.').map(Number), y = String(b).split('.').map(Number);
  for (let i = 0; i < Math.max(x.length, y.length); i++) { const d = (x[i] || 0) - (y[i] || 0); if (d) return d; }
  return 0;
}
// the newest release, never older than the floor (the admin helper checks the floor and its high-water mark again)
export function newestRelease(names, floor) {
  return (names || []).filter((n) => !floor || releaseCmp(n, floor) >= 0).sort(releaseCmp).pop() || null;
}

const SHA = /^([0-9a-fA-F]{64}|[0-9a-fA-F]{128})$/;
const NAME = /^[A-Za-z0-9_+~][A-Za-z0-9._+~-]{0,126}$/;

// A maker's SHA-256 list → [{ name, sha, size }]. GNU lines ("<hash>  <name>", "<hash> *<name>"), BSD lines
// ("SHA256 (<name>) = <hash>"); a BSD list's "# <name>: <n> bytes" comment gives the size. Anything else is skipped
// (OpenPGP armour, other hashes, comments).
export function parseSums(text) {
  const out = [], sizes = new Map();
  for (const raw of String(text || '').split('\n')) {
    const l = raw.replace(/\r$/, '');
    let m = /^([0-9a-fA-F]{64}|[0-9a-fA-F]{128}) [ *]([^\s]+)$/.exec(l) || null;
    let name, sha;
    if (m) { sha = m[1]; name = m[2]; } else if ((m = /^SHA256 \(([^\s()]+)\) = ([0-9a-fA-F]{64})$/.exec(l))) { name = m[1]; sha = m[2]; }
    else if ((m = /^# ([^\s:]+): (\d+) bytes$/.exec(l))) { sizes.set(m[1], Number(m[2])); continue; } else continue;
    if (NAME.test(name) && SHA.test(sha)) out.push({ name, sha: sha.toLowerCase(), size: 0 });
  }
  for (const f of out) f.size = sizes.get(f.name) || 0;
  return out;
}

// the file names a folder's HTML listing links to (href="name"), that match re
export function listingNames(html, re) {
  const r = new RegExp(re);
  const out = [];
  for (const m of String(html || '').matchAll(/href="([^"/?#]+)"/g)) if (NAME.test(m[1]) && r.test(m[1]) && !out.includes(m[1])) out.push(m[1]);
  return out;
}

// the first run of numbers, for comparing versions: "ubuntu-26.04.1-desktop-amd64" → [26, 4, 1] (not the 64 of the
// architecture), "Fedora-…-45-1.10-x86_64-…" → [45, 1, 10]
const numbers = (s) => ((/\d+(?:[.-]\d+)*/.exec(String(s)) || [''])[0].match(/\d+/g) || []).map(Number);
export function compareNames(a, b) {
  const x = numbers(a), y = numbers(b);
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    const d = (x[i] ?? -1) - (y[i] ?? -1);
    if (d) return d;
  }
  return a < b ? -1 : a > b ? 1 : 0;
}

// the newest of names that match re
export function newest(names, re) {
  const r = new RegExp(re);
  return names.filter((n) => NAME.test(n) && r.test(n)).sort(compareNames).pop() || null;
}

// the newest image of an entry in its list: { name, sha, size } or null
export function pickImage(entry, sumsText) {
  const pin = pinnedImage(entry);
  if (pin) return pin;
  // a SHA-512 entry (Whonix) takes only 128-digit lines, every other only 64-digit ones
  const len = entry?.hash === 'sha512' ? 128 : 64;
  const files = parseSums(sumsText).filter((f) => f.sha.length === len);
  const name = newest(files.map((f) => f.name), entry.file);
  return name ? files.find((f) => f.name === name) : null;
}

// the version a file name carries, from the release on: "debian-13.7.0-amd64…" (13) → "13.7",
// "Fedora-Workstation-Live-45-1.4…" (45) → "45", "ubuntu-26.04.1-…" (26.04) → "26.04.1"
export function versionIn(name, release) {
  const s = String(name || ''), r = String(release || '');
  const i = r ? s.indexOf(r) : -1;
  if (i < 0) return r;
  const m = /^[0-9]+(\.[0-9]+)*/.exec(s.slice(i));
  return m ? m[0].replace(/(\.\d+)\.0$/, '$1') : r;
}

// "progress download <done> <total>" from vm-image-get: { done, total } or null
export function progressOf(line) {
  const m = /^progress download (\d+) (\d+)/.exec(String(line || ''));
  return m ? { done: Number(m[1]), total: Number(m[2]) } : null;
}

// "vm.downloadResumed from=<bytes>" from vm-image-get (a stopped download continued): the bytes kept, or 0
export function resumedOf(line) {
  const m = /^vm\.downloadResumed from=(\d+)/.exec(String(line || ''));
  return m ? Number(m[1]) : 0;
}

// the size on a card: the list's when it says, else the catalog's estimate
export const sizeMb = (approxMb, bytes) => Math.round(bytes > 0 ? bytes / 1048576 : approxMb);

// ---- downloaded images (the Storage window, 6.16) ----
// /var/lib/lordos/machines/images: <sha256>.iso (checked), <sha256>.iso.part (a stopped download, continued on the next
// try) and <sha256>.json (what it is: { id, arch, file, size, at }, written by the admin helper). entries: fs.list of
// that folder; infoOf(sha): the parsed .json or null; machines: lord.vm.list() (this account's: { name, iso, state }).
// → [{ sha, path, partial, size, id, arch, file, version, label, users: [names], dirs, running: [names] }], biggest first
export const IMAGES_DIR = '/var/lib/lordos/machines/images';
export function downloadedImages(cat, entries, infoOf, machines, runningOf = (m) => m.state === 'running' || m.state === 'paused', groups = []) {
  const out = new Map();
  for (const e of Array.isArray(entries) ? entries : []) {
    const m = /^([0-9a-f]{64})\.iso(\.part)?$/.exec(String(e?.name || ''));
    if (!m || e.dir || e.link) continue;
    const partial = !!m[2];
    const prev = out.get(m[1]);
    if (prev && !prev.partial) continue;                  // the checked image wins over a leftover part
    const path = `${IMAGES_DIR}/${m[1]}.iso`;
    let info = null;
    try { info = infoOf(m[1]); } catch (x) { info = null; }
    const sys = (cat?.systems || []).find((s) => s?.id === info?.id);
    // an official logo (opt-in, logos.js) is no system image: the Storage window doesn't list it
    if (sys?.kind === 'logo' || /^logo-/.test(String(info?.id || ''))) continue;
    const file = typeof info?.file === 'string' && NAME.test(info.file) ? info.file : '';
    const version = sys && file ? versionIn(file, sys.release) : '';
    const users = [], dirs = [], running = [];
    if (!partial) for (const x of Array.isArray(machines) ? machines : []) {
      if (x?.iso !== path) continue;
      users.push(x.name); dirs.push(x.dir);
      if (runningOf(x)) running.push(x.name);
    }
    // a system the person added (vm-image-get-user): its file and the site it came from (the helper wrote the address
    // without its query; redacted again here all the same)
    const from = info?.id === 'user' && typeof info.from === 'string' ? hostOf(redactUrl(info.from)) : '';
    out.set(m[1], { sha: m[1], path, partial, size: Number(e.size) || 0, id: sys ? sys.id : 'other', arch: info?.arch === 'arm64' || info?.arch === 'amd64' ? info.arch : '',
      file, version, label: sys ? [sys.name, version].filter(Boolean).join(' ') : file && from ? `${file} · ${from}` : '', users, dirs, running });
  }
  // a machine group's verified archive (Whonix, 19.6): <sha512>.tar.xz; its users are the groups made from it, and it
  // may go once their bases are there (Start Fresh needs only the bases; a missing base is made again from a new download)
  for (const e of Array.isArray(entries) ? entries : []) {
    const m = /^([0-9a-f]{128})\.tar\.xz(\.dl\.part|\.dl)?$/.exec(String(e?.name || ''));
    if (!m || e.dir || e.link) continue;
    const partial = !!m[2];
    if (out.get(m[1]) && !out.get(m[1]).partial) continue;
    let info = null;
    try { info = infoOf(m[1]); } catch (x) { info = null; }
    const sys = (cat?.systems || []).find((s) => s?.id === info?.id && s?.kind === 'group');
    const file = typeof info?.file === 'string' && NAME.test(info.file) ? info.file : '';
    const gs = partial ? [] : (Array.isArray(groups) ? groups : []).filter((g) => g?.archiveSha512 === m[1]);
    out.set(m[1], { sha: m[1], path: `${IMAGES_DIR}/${m[1]}.tar.xz`, partial, size: Number(e.size) || 0, id: sys ? sys.id : 'other', arch: 'amd64', file,
      version: '', label: sys ? `${sys.name} (${file || 'tar.xz'})` : file, users: gs.map((g) => g.name), dirs: [], running: [], archive: true,
      basesThere: gs.length > 0 && gs.every((g) => g.basesThere) });
  }
  return [...out.values()].sort((a, b) => b.size - a.size || a.sha.localeCompare(b.sha));
}

// ---- unattended installs (vmunattend.h) ----
// the answer files lord-vm makes: Debian's preseed (and Kali's, the same with its archive and tasks), Ubuntu's
// autoinstall, Fedora's kickstart and LordOS's own
// lordos-auto.conf (the live ISO's unattended install, docs/UNATTENDED.md)
export const UNATTENDED_KINDS = ['preseed', 'kali', 'autoinstall', 'kickstart', 'lordos'];
// how a card installs without questions on this architecture: { kind, image (the entry whose image it boots: the card's
// own, or a hidden one), kernel, initrd, append } or null
export function unattendedOf(cat, id, arch) {
  const s = (cat?.systems || []).find((x) => x?.id === id);
  const u = s?.unattended;
  const a = u?.arch?.[arch];
  if (!u || !a || !UNATTENDED_KINDS.includes(u.kind) || typeof a.kernel !== 'string' || typeof a.initrd !== 'string') return null;
  const image = typeof u.image === 'string' ? u.image : id;
  if (!entryOf(cat, image, arch)) return null;
  // an architecture's own command line when it has one (arm64: the screen as the console, 4.1)
  const append = typeof a.append === 'string' ? a.append : u.append;
  return { kind: u.kind, image, kernel: a.kernel, initrd: a.initrd, append: String(append || ''), release: String(s.release || '') };
}

// LordOS's interface language for its unattended install: this LordOS's own, as a locale id ("es", "zh-Hans"), or "en"
export function lordosLang(lang) {
  const l = String(lang || '');
  return /^[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8})?$/.test(l) ? l : 'en';
}

// A card's machine over what the profile gives (size: { vcpus, memoryMb, diskGb, gfx, ... }); host: { cpus, memMaxMb }.
// "machine": what the card asks whatever the profile, fitted to this computer (never more processors
// than it has, never more memory than machines may use here). "defaults" (LordOS: 4 processors, 4 GB, 32 GB, 3D):
// the card's least size over the profile's, the processors never more than this computer has.
export function cardSize(cat, id, size, host) {
  const s = (cat?.systems || []).find((x) => x?.id === id);
  const m = s?.machine, d = s?.defaults;
  if (m && typeof m === 'object') {
    const out = { ...size };
    if (Number.isInteger(m.vcpus) && m.vcpus > 0) out.vcpus = Math.min(m.vcpus, Math.max(1, host?.cpus | 0 || m.vcpus));
    if (Number.isInteger(m.memoryMb) && m.memoryMb >= 256) out.memoryMb = Math.min(m.memoryMb, Math.max(256, Math.floor((host?.memMaxMb || m.memoryMb) / 256) * 256));
    if (Number.isInteger(m.diskGb) && m.diskGb > 0) out.diskGb = m.diskGb;
    if (m.graphics === '3d' || m.graphics === '2d') out.gfx = m.graphics;
    return out;
  }
  if (!d || typeof d !== 'object') return size;
  const n = (v) => (Number.isInteger(v) && v > 0 ? v : 0);
  const want = n(d.vcpus);
  const vcpus = Math.max(size.vcpus, Math.min(want, Math.max(1, host?.cpus || want)));
  return { ...size, vcpus, memoryMb: Math.max(size.memoryMb, n(d.memoryMb)), diskGb: Math.max(size.diskGb, n(d.diskGb)),
    gfx: d.graphics === '3d' ? '3d' : size.gfx };
}

// what the person types for an unattended install, checked as lord-vm checks it: { ok, errors: { user, password, hostname } }
const RESERVED = ['root', 'daemon', 'bin', 'sys', 'sync', 'games', 'man', 'lp', 'mail', 'news', 'uucp', 'proxy', 'www-data', 'backup', 'list',
  'irc', 'nobody', 'systemd-network', 'messagebus', 'admin', 'sudo', 'wheel', 'adm'];
export function checkSetup({ user, password, hostname }) {
  const errors = {};
  if (!/^[a-z_][a-z0-9_-]{0,31}$/.test(user || '')) errors.user = user ? 'user' : 'userEmpty';
  else if (RESERVED.includes(user)) errors.user = 'userReserved';
  if (!password) errors.password = 'passwordEmpty';
  else if (password.length > 255) errors.password = 'passwordLong';
  if (!/^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$/.test(hostname || '')) errors.hostname = 'hostname';
  return { ok: !Object.keys(errors).length, errors };
}

// a machine's name as a computer name: "Debian 13.7" → "debian-13-7"
export function hostnameFor(name) {
  const h = String(name || '').toLowerCase().normalize('NFD').replace(/[\u0300-\u036f]/g, '').replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 63).replace(/-+$/, '');
  return h || 'machine';
}

// the computer's name the Set up step shows for a machine called name: the one made from the name while the field is
// empty or still holds the one made for an earlier name (auto: the last one filled in by itself, as going back from
// Debian to LordOS); a name the person typed stays
export function hostnameNext(current, auto, name) {
  const cur = String(current || '');
  return !cur || (auto && cur === auto) ? hostnameFor(name) : cur;
}

// an account name from the person's own: "Federico Pereira" or "lordbasex" → "lordbasex"
export function userFor(home) {
  const u = String(home || '').split('/').filter(Boolean).pop() || '';
  return /^[a-z_][a-z0-9_-]{0,31}$/.test(u) && !RESERVED.includes(u) ? u : 'user';
}

// ---- official logos (opt-in; logos.js) ----
// the catalog's logo of a system (an entry of kind "logo" whose "for" names it), pinned like any download: { id, file,
// sha, bytes } or null. Only systems whose owners' policies allow it to identify them are listed (docs/TRADEMARKS.md);
// a system of the other kind never has one.
export function logoEntry(cat, sysId, arch) {
  if (!sysId || sysId === 'windows') return null;
  const s = (cat?.systems || []).find((x) => x?.kind === 'logo' && x.for === sysId && typeof x.id === 'string' && /^logo-[a-z0-9-]+$/.test(x.id));
  const pin = s?.arch?.[arch]?.pinned?.[0];
  if (!pin || !/^[0-9a-f]{64}$/.test(pin.sha256 || '') || !/\.png$/.test(pin.name || '') || !/^https:\/\//.test(pin.url || '')) return null;
  return { id: s.id, file: pin.name, sha: pin.sha256, bytes: pin.bytes | 0 };
}
