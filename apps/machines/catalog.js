// catalog.js — the systems the New Machine assistant downloads (assets/machines/systems.json; docs/VIRTUALIZATION.md
// 4.1 and 3.9, "The Linux download cards"): pure functions, tested by server/tests/vm_catalog_test.mjs. The admin
// helper fetches the makers' lists and the images (vm-image-list, vm-image-get) and checks again everything chosen
// here: the app only reads a list and names the newest file in it, with its SHA-256.

// the cards for an architecture, in the catalog's order: { id, name, release, approxMb, kind }
export function systemsFor(cat, arch) {
  const out = [];
  for (const s of Array.isArray(cat?.systems) ? cat.systems : []) {
    const a = s?.arch?.[arch];
    if (!a || s.hidden || typeof s.id !== 'string' || typeof a.file !== 'string') continue;
    out.push({ id: s.id, name: String(s.name || s.id), release: String(s.release || ''), approxMb: Number(a.approxMb) || 0, kind: s.kind || 'iso', listed: !a.sums });
  }
  return out;
}

export function entryOf(cat, id, arch) {
  const s = (cat?.systems || []).find((x) => x?.id === id);
  return s?.arch?.[arch] ? { ...s.arch[arch], id, name: s.name, release: s.release } : null;
}

const SHA = /^[0-9a-fA-F]{64}$/;
const NAME = /^[A-Za-z0-9_+~][A-Za-z0-9._+~-]{0,126}$/;

// A maker's SHA-256 list → [{ name, sha, size }]. GNU lines ("<hash>  <name>", "<hash> *<name>"), BSD lines
// ("SHA256 (<name>) = <hash>"); a BSD list's "# <name>: <n> bytes" comment gives the size. Anything else is skipped
// (OpenPGP armour, other hashes, comments).
export function parseSums(text) {
  const out = [], sizes = new Map();
  for (const raw of String(text || '').split('\n')) {
    const l = raw.replace(/\r$/, '');
    let m = /^([0-9a-fA-F]{64}) [ *]([^\s]+)$/.exec(l) || null;
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
  const files = parseSums(sumsText);
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
export function downloadedImages(cat, entries, infoOf, machines, runningOf = (m) => m.state === 'running' || m.state === 'paused') {
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
    const file = typeof info?.file === 'string' && NAME.test(info.file) ? info.file : '';
    const version = sys && file ? versionIn(file, sys.release) : '';
    const users = [], dirs = [], running = [];
    if (!partial) for (const x of Array.isArray(machines) ? machines : []) {
      if (x?.iso !== path) continue;
      users.push(x.name); dirs.push(x.dir);
      if (runningOf(x)) running.push(x.name);
    }
    out.set(m[1], { sha: m[1], path, partial, size: Number(e.size) || 0, id: sys ? sys.id : 'other', arch: info?.arch === 'arm64' || info?.arch === 'amd64' ? info.arch : '',
      file, version, label: sys ? [sys.name, version].filter(Boolean).join(' ') : '', users, dirs, running });
  }
  return [...out.values()].sort((a, b) => b.size - a.size || a.sha.localeCompare(b.sha));
}

// ---- unattended installs (vmunattend.h) ----
// how a card installs without questions on this architecture: { kind, image (the entry whose image it boots: the card's
// own, or a hidden one), kernel, initrd, append } or null
export function unattendedOf(cat, id, arch) {
  const s = (cat?.systems || []).find((x) => x?.id === id);
  const u = s?.unattended;
  const a = u?.arch?.[arch];
  if (!u || !a || !['preseed', 'autoinstall', 'kickstart'].includes(u.kind) || typeof a.kernel !== 'string' || typeof a.initrd !== 'string') return null;
  const image = typeof u.image === 'string' ? u.image : id;
  if (!entryOf(cat, image, arch)) return null;
  return { kind: u.kind, image, kernel: a.kernel, initrd: a.initrd, append: String(u.append || ''), release: String(s.release || '') };
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

// an account name from the person's own: "Federico Pereira" or "lordbasex" → "lordbasex"
export function userFor(home) {
  const u = String(home || '').split('/').filter(Boolean).pop() || '';
  return /^[a-z_][a-z0-9_-]{0,31}$/.test(u) && !RESERVED.includes(u) ? u : 'user';
}
