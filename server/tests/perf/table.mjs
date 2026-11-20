// table.mjs — perf_headless.sh's PERF lines (stdin, one JSON object per line) as a table per scenario.
import fs from 'node:fs';
const lines = fs.readFileSync(0, 'utf8').split('\n').filter(Boolean).map((l) => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean);
const heaps = new Map();
let cur = null;
const rows = [];
for (const o of lines) {
  if (o.begin) { cur = { name: o.begin, heap0: o.heap, steps: [] }; continue; }
  if (o.end && cur) { cur.heap1 = o.heap; rows.push(cur); cur = null; continue; }
  if (o.step && cur) cur.steps.push(o);
  if (o.done) console.log(`stalls past the limit: ${o.stalls}, worst ${o.worst?.toFixed?.(1)} ms`);
  if (o.error) console.log('error:', o.step, o.error);
}
const f = (n) => (n == null ? '-' : n.toFixed(1));
for (const r of rows) {
  const n = r.steps.length || 1;
  const sum = (k) => r.steps.reduce((a, s) => a + (s[k] || 0), 0);
  const max = Math.max(0, ...r.steps.map((s) => s.max || 0));
  const worst = r.steps.reduce((a, s) => (!a || s.max > a.max ? s : a), null);
  const dh = r.heap1 && r.heap0 ? r.heap1.usedKB - r.heap0.usedKB : null;
  console.log(`${r.name}: ${r.steps.length} steps; per step js ${f(sum('js') / n)} layout ${f(sum('layout') / n)} paint ${f(sum('paint') / n)} ms; ` +
    `longest run ${f(max)} ms${worst ? ` (${worst.step})` : ''}; heap ${r.heap0?.usedKB} → ${r.heap1?.usedKB} KB (${dh >= 0 ? '+' : ''}${dh}), objects ${r.heap0?.objects} → ${r.heap1?.objects}, nodes ${r.heap1?.nodes}`);
  if (process.env.PERF_STEPS) for (const s of r.steps) console.log(`    ${s.step}: js ${s.js} layout ${s.layout} paint ${s.paint} max ${s.max}${s.ok === false ? ' (not found)' : ''}`);
}
