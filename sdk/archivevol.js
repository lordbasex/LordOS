// archivevol.js — split archives (GitHub #103): which files are volumes of one archive and in which order.
// Pure logic (no lord.*), tested by server/tests/archive_vol_test.mjs.
//
//   splitVolume(name) → { base, kind, index } | null
//       'num': byte splits made by 7-Zip and similar ("docs.zip.001", "docs.7z.002", "backup.tar.gz.003";
//              a lone ".001" is a whole archive that fit in one volume)
//       'z':   Info-ZIP split zips ("docs.z01", "docs.z02" … and "docs.zip" as the last volume; index 0
//              stands for that last ".zip", which is a volume only when "docs.z01" exists next to it)
//   volumeSet(path, exists) → { paths, missing } | null
//       every volume in reading order, found by asking exists(path) for the names next to it; missing
//       is the name of the first volume that is not there (the set cannot be read), or null.
//       null: path is not a volume (a plain archive).

export const MAX_VOLUMES = 255;           // lord-doc takes up to 255 archive fds (DOC_INIT_FDS - 1)

const NUM = /^(.+\.(?:zip|7z|tar|tgz|tbz2?|txz|tzst|tar\.(?:gz|xz|zst|bz2|lz|lzma)|iso|cab|cpio))\.(\d{3})$/i;
const ZNN = /^(.+)\.z(\d{2,3})$/i;
const ZIP = /^(.+)\.zip$/i;

export function splitVolume(name) {
  const s = String(name);
  let m = NUM.exec(s);
  if (m) return Number(m[2]) > 0 ? { base: m[1], kind: 'num', index: Number(m[2]), digits: m[2].length } : null;
  m = ZNN.exec(s);
  if (m) return Number(m[2]) > 0 ? { base: m[1], kind: 'z', index: Number(m[2]) } : null;
  m = ZIP.exec(s);
  if (m) return { base: m[1], kind: 'z', index: 0 };
  return null;
}

const pad = (n, d) => String(n).padStart(d, '0');
const zName = (base, i) => `${base}.z${pad(i, 2)}`;

export function volumeSet(path, exists) {
  const p = String(path);
  const cut = p.lastIndexOf('/');
  const dir = cut < 0 ? '' : p.slice(0, cut + 1);
  const v = splitVolume(p.slice(cut + 1));
  if (!v) return null;
  const at = (name) => exists(dir + name);
  const names = [];
  let missing = null;
  if (v.kind === 'num') {
    const name = (i) => `${v.base}.${pad(i, v.digits)}`;
    let i = 1;
    while (i <= MAX_VOLUMES && at(name(i))) { names.push(name(i)); i++; }
    // a hole: a later volume (or the one that was opened) is there but this one is not
    if (i <= MAX_VOLUMES && (v.index > i || at(name(i + 1)) || at(name(i + 2)))) missing = name(i);
    if (!names.length && !missing) return null;
  } else {
    // ".zip" without ".z01" next to it is a plain zip
    if (v.index === 0 && !at(zName(v.base, 1))) return null;
    let i = 1;
    while (i < MAX_VOLUMES && at(zName(v.base, i))) { names.push(zName(v.base, i)); i++; }
    if (i < MAX_VOLUMES && (v.index > i || at(zName(v.base, i + 1)))) missing = zName(v.base, i);
    else if (!at(`${v.base}.zip`)) missing = `${v.base}.zip`;
    else names.push(`${v.base}.zip`);
  }
  return { paths: names.map((n) => dir + n), missing };
}

// The archive's display name without the volume suffix: "docs.zip.001" → "docs.zip", "docs.z03" → "docs.zip".
export function setName(name) {
  const v = splitVolume(name);
  if (!v) return String(name);
  return v.kind === 'num' ? v.base : `${v.base}.zip`;
}
