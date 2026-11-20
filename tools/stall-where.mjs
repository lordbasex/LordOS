// stall-where.mjs — the stall detector's lines (lord-server: stall: …, docs/MAIN-THREAD.md) with the minified
// places of their JavaScript stacks turned into source files and lines, through the theme's source map.
//
//   LORD_SOURCEMAP=1 node tools/build-theme.mjs default     # the map, from the same sources as the running bundle
//   grep 'stall:' shell.log compositor.log | node tools/stall-where.mjs [themes/default/theme.bundle.js.map] [--skip N]
//
// --skip N: the bundle that ran had N lines before the theme's own (server/tests/perf_headless.sh adds some).
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repo = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const args = process.argv.slice(2);
const skipAt = args.indexOf('--skip');
const skip = skipAt >= 0 ? +args.splice(skipAt, 2)[1] : 0;
const mapFile = args[0] || path.join(repo, 'themes', 'default', 'theme.bundle.js.map');
const map = JSON.parse(fs.readFileSync(mapFile, 'utf8'));

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
const val = new Map([...B64].map((c, i) => [c, i]));
function vlq(s, i) {           // [value, next index]
  let res = 0, shift = 0, c;
  do { c = val.get(s[i++]); res += (c & 31) << shift; shift += 5; } while (c & 32);
  return [res & 1 ? -(res >> 1) : res >> 1, i];
}
// lines[generated line] = [[genCol, source, line, col], …] sorted by genCol
const lines = [];
{
  let src = 0, sl = 0, sc = 0;
  for (const text of map.mappings.split(';')) {
    const segs = [];
    let gc = 0;
    for (const seg of text ? text.split(',') : []) {
      let i = 0, v;
      [v, i] = vlq(seg, i); gc += v;
      if (i < seg.length) {
        [v, i] = vlq(seg, i); src += v;
        [v, i] = vlq(seg, i); sl += v;
        [v, i] = vlq(seg, i); sc += v;
        segs.push([gc, src, sl, sc]);
      }
    }
    lines.push(segs);
  }
}
function where(line, col) {    // 1-based line and column, as QuickJS prints them
  const segs = lines[line - 1 - skip];
  if (!segs) return null;
  let best = null;
  for (const s of segs) { if (s[0] <= col - 1) best = s; else break; }
  if (!best) return null;
  const file = map.sources[best[1]].replace(/^(\.\.\/)+/, '');
  return `${file}:${best[2] + 1}`;
}
const input = fs.readFileSync(0, 'utf8');
for (const l of input.split('\n')) {
  if (!l) continue;
  console.log(l.replace(/\(theme\.bundle\.js:(\d+):(\d+)\)/g, (m, a, b) => `(${where(+a, +b) || m})`));
}
