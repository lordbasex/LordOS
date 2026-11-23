#!/usr/bin/env node
// compare.mjs — two reports of tools/wl-conformance.mjs side by side: your run against the one LordOS recorded
// (docs/wayland-conformance.json, published on lordos.org/wayland-conformance/), check by check.
//
//   node tools/wl-conformance/compare.mjs                                   build/wl-conformance.json against docs/wayland-conformance.json
//   node tools/wl-conformance/compare.mjs <yours.json> [<recorded.json>]
//
// Prints both runs (date, commit, versions), every global and X11 item whose score differs, and every check whose
// result differs or that only one run has. Exit status: 0 when every check has the same result in both, 1 when
// something differs, 2 when a file can't be read. It only reads the two files.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const [mineArg, oursArg] = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const files = { mine: mineArg || path.join(ROOT, 'server', 'build', 'wl-conformance.json'), ours: oursArg || path.join(ROOT, 'docs', 'wayland-conformance.json') };
const read = (f) => {
  try {
    const r = JSON.parse(fs.readFileSync(f, 'utf8'));
    if (!r || !Array.isArray(r.protocols)) throw new Error('not a wl-conformance report');
    return r;
  } catch (e) { console.error(`compare: ${f}: ${e.message}`); process.exit(2); }
};
const A = read(files.mine), B = read(files.ours);

const head = (name, r, f) => {
  const E = r.environment || {}, S = r.summary || {};
  console.log(`${name}: ${path.relative(process.cwd(), f) || f}`);
  console.log(`  ${r.date}, commit ${String(r.commit || '?').slice(0, 10)}${r.dirty ? ' (+local changes)' : ''}; ${E.mode || '?'}`);
  console.log(`  wayland-protocols ${E.wayland_protocols || '?'}, libwayland ${E.libwayland || '?'}, Xwayland ${E.xwayland || 'not installed'}`);
  console.log(`  ${S.protocols} globals, mean ${S.mean} %, ${S.complete} complete${S.x11_mean != null ? `; X11 mean ${S.x11_mean} %` : '; X11 not run'}`);
};
head('yours', A, files.mine);
head('recorded', B, files.ours);

// items keyed by global (Wayland) or "x11:<id>"
const items = (r) => new Map([...r.protocols.map((p) => [p.global, p]), ...((r.xwayland && r.xwayland.items) || []).map((i) => [`x11:${i.id}`, i])]);
const IA = items(A), IB = items(B);
let differ = 0;
const out = [], notRun = [];
for (const key of [...new Set([...IB.keys(), ...IA.keys()])].sort()) {
  const a = IA.get(key), b = IB.get(key);
  // a run with --only, --no-x11 or --static-only leaves items out: listed, not counted as a difference
  if (!a) { notRun.push(key); continue; }
  if (!b) { differ++; out.push(`${key}: only in yours (${a.score} %), not in the recorded run`); continue; }
  const lines = [];
  const ca = new Map(a.checks.map((c) => [c.id, c])), cb = new Map(b.checks.map((c) => [c.id, c]));
  for (const id of new Set([...cb.keys(), ...ca.keys()])) {
    const x = ca.get(id), y = cb.get(id);
    if (!x) lines.push(`  - ${id}: only in the recorded run (${y.result})`);
    else if (!y) lines.push(`  + ${id}: only in yours (${x.result}: ${x.msg})`);
    else if (x.result !== y.result) lines.push(`  ! ${id}: yours ${x.result}, recorded ${y.result}\n      yours:    ${x.msg}${x.where ? `  [${x.where}]` : ''}\n      recorded: ${y.msg}`);
  }
  if (lines.length || a.score !== b.score || a.version !== b.version) {
    differ += lines.length || 1;
    out.push(`${key}: yours ${a.score} %${a.version != null ? ` (v${a.version})` : ''}, recorded ${b.score} %${b.version != null ? ` (v${b.version})` : ''}${a.complete !== b.complete ? `; complete: yours ${a.complete}, recorded ${b.complete}` : ''}`, ...lines);
  }
}
console.log('');
if (notRun.length) console.log(`Not in your run (left out by --only, --no-x11 or --static-only, or not run here): ${notRun.length} of ${IB.size} recorded items${notRun.length <= 8 ? `: ${notRun.join(', ')}` : ''}.`);
if (!differ) console.log(`Same result for every check your run has (${[...IA.values()].reduce((n, i) => n + i.checks.length, 0)} checks in ${IA.size} items).`);
else { console.log(`Differences (${differ}):`); for (const l of out) console.log(l); }
process.exit(differ ? 1 : 0);
