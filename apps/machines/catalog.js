// catalog.js — the systems the New Machine assistant downloads (assets/machines/systems.json; docs/VIRTUALIZATION.md
// 4.1 and 3.9, "The Linux download cards"): pure functions, tested by server/tests/vm_catalog_test.mjs. The admin
// helper fetches the makers' lists and the images (vm-image-list, vm-image-get) and checks again everything chosen
// here: the app only reads a list and names the newest file in it, with its SHA-256.

// the cards for an architecture, in the catalog's order: { id, name, release, approxMb, kind }
export function systemsFor(cat, arch) {
  const out = [];
  for (const s of Array.isArray(cat?.systems) ? cat.systems : []) {
    const a = s?.arch?.[arch];
    if (!a || typeof s.id !== 'string' || typeof a.file !== 'string') continue;
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

// the size on a card: the list's when it says, else the catalog's estimate
export const sizeMb = (approxMb, bytes) => Math.round(bytes > 0 ? bytes / 1048576 : approxMb);
