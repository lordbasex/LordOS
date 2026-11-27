// names.js — the free name Files gives a copy, a duplicate, a new folder or an archive, kept within what a file system
// takes for one name: NAME_MAX, 255 bytes of UTF-8 (fewer letters when they have accents or are not Latin). A long name
// copied next to itself became "<name> 2.<ext>", past that limit, and the copy failed (GitHub #102); now the stem is
// shortened, never the extension or the number. Pure (no lord.*). tests: server/tests/files_names_test.mjs.

export const NAME_MAX = 255;

// UTF-8 bytes of a string (a lone surrogate counts as the 3 bytes of U+FFFD)
export function nameBytes(s) {
  let n = 0;
  for (const ch of String(s)) {
    const c = ch.codePointAt(0);
    n += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
  }
  return n;
}

// stem cut, whole characters only, so that stem + tail fits max bytes; trailing spaces and dots of the cut go
function fit(stem, tail, max) {
  const room = max - nameBytes(tail);
  if (nameBytes(stem) <= room) return stem + tail;
  let out = '', n = 0;
  for (const ch of stem) {
    const b = nameBytes(ch);
    if (n + b > room) break;
    out += ch; n += b;
  }
  out = out.replace(/[ .]+$/, '');
  return (out || stem.slice(0, 1)) + tail;
}

// A name not in taken (a Set of the folder's names): name itself, else "<stem> 2<ext>", "<stem> 3<ext>"… Every one
// fits max bytes.
export function freeNameOf(taken, name, max = NAME_MAX) {
  const m = /^(.*?)(\.[^.]+)?$/.exec(name);
  let stem = m[1], ext = m[2] || '';
  if (!stem || nameBytes(ext) > max / 2) { stem = name; ext = ''; }       // ".bashrc", or an "extension" that is most of it
  if (!taken.has(name) && nameBytes(name) <= max) return name;
  if (!taken.has(name)) {
    const short = fit(stem, ext, max);
    if (!taken.has(short)) return short;
  }
  const base = stem.replace(/ \d+$/, '');
  for (let i = 2; i < 10000; i++) {
    const n = fit(base, ` ${i}${ext}`, max);
    if (!taken.has(n)) return n;
  }
  return fit(base, ` ${Date.now()}${ext}`, max);
}

// What renaming selects first (F2, a slow click; MEJORA-1 of the ws170 QA): the name up to its last dot, so typing keeps
// the extension, as macOS and Nautilus do; a folder, a name without a dot, or one whose only dot leads it (".bashrc")
// selects it whole. [start, end] in code points (the text field counts characters so).
export function renameSelection(name, dir = false) {
  const chars = [...String(name || '')];
  const dot = dir ? -1 : chars.lastIndexOf('.');
  return [0, dot > 0 ? dot : chars.length];
}
