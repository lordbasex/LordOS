// lsnames.js — the file name under the pointer in `ls` or `ls -l` output (pure; server/tests/terminal_lsnames_test.mjs).
// The Terminal gives the line split at the pointer's cell (term.lineAt: { before, after }); ❖ + click on a name in an
// ssh pane downloads it (dropupload.js). Returns { name, dir } or null when the pointer isn't on a name.

// ls -l: type and permissions, links, owner, group, size (or "major, minor"), the date (three fields, or two with
// --time-style=long-iso/iso), then the name, "name -> target" for a link
const LONG = /^([-bcdlpsDw?])[rwxsStTl-]{9}[.+@]?\s+\d+\s+\S+\s+\S+\s+(?:\d+,\s*)?\d[\d.,]*[KMGTPE]?\s+(?:\d{4}-\d\d-\d\d\s+\d\d:\d\d(?::\d\d(?:\.\d+)?)?(?:\s+[+-]\d{4})?|\S+\s+\d{1,2}\s+(?:\d{1,2}:\d\d|\d{4}))\s/;

// ls quotes names with spaces or odd characters: 'a b', "it's", or $'tab\t'
function unquote(s) {
  if (s.length > 1 && s[0] === "'" && s.endsWith("'")) return s.slice(1, -1).replace(/'\\''/g, "'");
  if (s.length > 1 && s[0] === '"' && s.endsWith('"')) return s.slice(1, -1).replace(/\\(["\\$`])/g, '$1');
  return s;
}

// a name ls quoted keeps its spaces; otherwise names in columns are cut where two spaces start
function columnAt(line, col) {
  const parts = [];
  const re = /'(?:[^']|'\\'')*'|"(?:[^"\\]|\\.)*"|\S+(?: \S+)*/g;
  let m;
  while ((m = re.exec(line))) parts.push({ from: m.index, to: m.index + m[0].length, text: m[0] });
  const hit = parts.find((p) => col >= p.from && col < p.to);
  return hit ? unquote(hit.text) : null;
}

export function nameAt(before, after) {
  const line = String(before || '') + String(after || '');
  const col = String(before || '').length;
  if (!line.trim() || col >= line.length) return null;
  const long = LONG.exec(line);
  if (long) {
    const start = long[0].length;
    if (col < start) return null;                               // on the permissions, owner, size or date
    let name = line.slice(start).replace(/\s+$/, '');
    if (long[1] === 'l') name = name.split(' -> ')[0];
    name = unquote(name);
    return goodName(name) ? { name, dir: long[1] === 'd' } : null;
  }
  if (/^total \d/.test(line)) return null;
  const name = columnAt(line, col);
  return name && goodName(name) ? { name, dir: false } : null;
}

const goodName = (n) => !!n && n.length <= 255 && n !== '.' && n !== '..' && !/[/\x00-\x1f\x7f]/.test(n);
