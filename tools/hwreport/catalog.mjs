// catalog.mjs — the tested-hardware catalogue (0.8.7, docs/BENCHMARK.md "0.8.7 — Community hardware reports").
// From a folder of accepted reports it builds:
//   (a) the model of lordos.org/hardware (web/build.mjs renders it, en/es/pt, with renderSections below), and
//   (b) the compact snapshot System Information's Compatibility panel reads (assets/hwreport/snapshot.json,
//       shipped in the lordos package and signed on the maintainer's machine with tools/release/sign-hwsnapshot.sh;
//       axia-server verifies it in C, server/src/sysreport/hwsnap.c, before JS sees it).
//
//   node tools/hwreport/catalog.mjs [--reports <dir>] [--snapshot <out.json>] [--check]
//
// --check builds the snapshot and compares it with the file instead of writing it (exit 1 if they differ).
// Every report is validated against the schema first; one that doesn't pass stops the build. The output
// depends only on the reports (sorted keys, "generated" is the newest report's date), so the same folder
// always gives the same bytes and the same signature.
//
// The folder: one <name>.json per accepted report, and an optional index.json
//   { "reports": { "<name>.json": { "issue": 123, "fixedIn": { "<component>": "0.8.8" } } } }
// with what the maintainers add when they accept one (the issue it came from, the version that fixed a
// problem it reported). The project's own reference machines have "source": "reference" and no issue.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { validate } from './jsonschema.mjs';
import { deviceKeys, SNAPSHOT_FORMAT, COMPONENTS } from '../../apps/sysinfo/community.js';
import { reportKind, machineLabel, gpuVendor, mainGpu } from '../../apps/sysinfo/share.js';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
export const SCHEMA_FILE = path.join(REPO, 'apps/sysinfo/hwreport.schema.json');
export const REPORTS_DIR = path.join(REPO, 'tools/hwreport/reports');
export const SNAPSHOT_FILE = path.join(REPO, 'assets/hwreport/snapshot.json');
export const loadSchema = () => JSON.parse(fs.readFileSync(SCHEMA_FILE, 'utf8'));

const base = (v) => { const m = /^(\d+\.\d+\.\d+(?:\.\d+)?)/.exec(String(v || '')); return m ? m[1] : ''; };
const cmpVer = (a, b) => { const x = a.split('.').map(Number), y = b.split('.').map(Number); for (let i = 0; i < 4; i++) { const d = (x[i] || 0) - (y[i] || 0); if (d) return d; } return 0; };
const cpuShort = (s) => String(s || '').replace(/\(R\)|\(TM\)|\bCPU\b|\bProcessor\b/g, '').replace(/\s+/g, ' ').trim();
const VERSION_RE = /^\d+\.\d+\.\d+(\.\d+)?$/;

// Reads and validates every report of a folder: [{ file, report, issue, fixedIn }], sorted by file name.
export function readReports(dir, schema = loadSchema()) {
  const index = fs.existsSync(path.join(dir, 'index.json')) ? JSON.parse(fs.readFileSync(path.join(dir, 'index.json'), 'utf8')) : {};
  const meta = (index && index.reports) || {};
  const out = [];
  for (const file of fs.readdirSync(dir).filter((f) => f.endsWith('.json') && f !== 'index.json').sort()) {
    const report = JSON.parse(fs.readFileSync(path.join(dir, file), 'utf8'));
    const errors = validate(schema, report);
    if (errors.length) throw new Error(`${file}: not a valid report: ${errors.slice(0, 5).map((e) => `${e.path} ${e.message}`).join('; ')}`);
    const m = meta[file] || {};
    const issue = Number.isInteger(m.issue) && m.issue > 0 ? m.issue : null;
    if (report.source === 'community' && !issue) throw new Error(`${file}: a community report needs its issue number in index.json`);
    const fixedIn = {};
    for (const [c, v] of Object.entries(m.fixedIn || {})) if (COMPONENTS.includes(c) && VERSION_RE.test(v)) fixedIn[c] = v;
    out.push({ file, report, issue, fixedIn });
  }
  return out;
}

// (b) the snapshot: one entry per device key, component and LordOS version, with the states reported.
export function buildSnapshot(entries) {
  const map = new Map();
  let generated = '';
  for (const { report } of entries) {
    if (report.createdAt > generated) generated = report.createdAt;
    const v = base(report.lordos);
    if (!v) continue;
    const states = Object.fromEntries((report.compatibility || []).map((c) => [c.id, c.state]));
    for (const d of deviceKeys(report.system)) {
      const st = states[d.component];
      if (!['works', 'limits', 'no'].includes(st)) continue;
      const id = `${d.key}\u0000${d.component}\u0000${v}`;
      let e = map.get(id);
      if (!e) map.set(id, (e = { key: d.key, component: d.component, name: d.name, lordos: v, works: 0, limits: 0, no: 0, reports: 0, references: 0 }));
      e[st]++;
      e.reports++;
      if (report.source === 'reference') e.references++;
    }
  }
  const devices = [...map.values()].sort((a, b) => (a.key < b.key ? -1 : a.key > b.key ? 1 : a.component < b.component ? -1 : a.component > b.component ? 1 : cmpVer(a.lordos, b.lordos)));
  return { format: SNAPSHOT_FORMAT, version: 1, generated, reports: entries.length, devices };
}
export const snapshotText = (snap) => `${JSON.stringify(snap)}\n`;

// (a) the page's model.
const PERF = [['cpu', 'cpu.matmul.n', 'GFLOP/s'], ['memory', 'mem.copy', 'GB/s'], ['gpu', 'gpu.scene', 'fps'], ['storage', 'disk.seqread', 'MB/s']];
export function buildModel(entries) {
  const machines = [], gpus = new Map(), problems = [];
  for (const { file, report, issue, fixedIn } of entries) {
    const s = report.system;
    const g = mainGpu(s);
    const compat = Object.fromEntries((report.compatibility || []).map((c) => [c.id, { state: c.state, note: c.note || null }]));
    const res = (report.bench && report.bench.results) || {};
    const desk = (report.desktop && report.desktop.results) || {};
    const perf = PERF.filter(([, id]) => res[id]).map(([cat, id, unit]) => ({ cat, id, unit, value: res[id].value }));
    if (desk['desk.fps']) perf.push({ cat: 'desktop', id: 'desk.fps', unit: 'fps', value: desk['desk.fps'].value });
    const m = {
      id: file.replace(/\.json$/, ''), source: report.source, issue, lordos: report.lordos, live: report.live,
      name: machineLabel(report), kind: reportKind(report), arch: (s.cpu && s.cpu.arch) || '',
      cpu: cpuShort(s.cpu && s.cpu.model), threads: (s.cpu && s.cpu.threads) || 0,
      memGB: s.memory && s.memory.totalKB ? Math.round(s.memory.totalKB / 1048576) : null,
      gpu: g ? [g.vendor, g.device].filter(Boolean).join(' ') || g.driver : '', driver: g ? g.driver : '',
      wifi: (s.network || []).filter((n) => n.kind === 'wifi').map((n) => n.driver).filter(Boolean).join(', '),
      battery: (s.power || []).some((p) => p.kind === 'battery'),
      compat, perf, fixedIn,
    };
    machines.push(m);
    if (g) {
      const key = g.vendorId && g.deviceId ? `pci:${g.vendorId}:${g.deviceId}` : `gpu:${g.driver}`;
      const e = gpus.get(key) || { key, name: m.gpu, driver: g.driver, vendor: gpuVendor(report), works: 0, limits: 0, no: 0, versions: [] };
      const st = compat.gpu && compat.gpu.state;
      if (e[st] != null) e[st]++;
      if (!e.versions.includes(report.lordos)) e.versions.push(report.lordos);
      gpus.set(key, e);
    }
    for (const [id, c] of Object.entries(compat)) {
      if (c.state === 'limits' || c.state === 'no') problems.push({ machine: m.name, id, state: c.state, note: c.note, lordos: report.lordos, fixedIn: fixedIn[id] || null, source: report.source, issue });
    }
  }
  const order = { reference: 0, community: 1 };
  machines.sort((a, b) => order[a.source] - order[b.source] || a.name.localeCompare(b.name));
  return { machines, gpus: [...gpus.values()].sort((a, b) => a.name.localeCompare(b.name)), problems };
}

// The page's sections as HTML, with the site's escaping and strings (web/i18n/pages.json "hardware").
// `t`: those strings; `esc`: the site's HTML escape; `issueUrl(n)`: a link to issue n.
export function renderSections(model, t, esc, issueUrl) {
  const st = (s) => `<span class="hw-st hw-${esc(s)}">${esc((t.state && t.state[s]) || s)}</span>`;
  const comp = (id) => esc((t.component && t.component[id]) || id);
  const src = (m) => (m.source === 'reference'
    ? `<span class="hw-ref">${esc(t.reference)}</span>`
    : m.issue ? `<a href="${esc(issueUrl(m.issue))}" rel="noopener">#${m.issue}</a>` : '');
  const fmt = (v) => (v >= 100 ? Math.round(v).toLocaleString('en-US') : String(Math.round(v * 10) / 10));
  const machines = model.machines.map((m) => `<article class="hw-card">
<h3>${esc(m.name)}</h3>
<p class="hw-meta">${esc((t.kind && t.kind[m.kind]) || m.kind)} · ${esc(m.arch)} · LordOS ${esc(m.lordos)}${m.live ? ` · ${esc(t.live)}` : ''} · ${src(m)}</p>
<dl class="hw-dl">
<dt>${esc(t.cpu)}</dt><dd>${esc(m.cpu)}${m.threads ? ` (${m.threads} ${esc(t.threads)})` : ''}</dd>
<dt>${esc(t.gpu)}</dt><dd>${esc(m.gpu)}${m.driver ? ` <code>${esc(m.driver)}</code>` : ''}</dd>
${m.memGB != null ? `<dt>${esc(t.memory)}</dt><dd>${m.memGB} GB</dd>` : ''}
${m.wifi ? `<dt>${esc(t.wifi)}</dt><dd><code>${esc(m.wifi)}</code></dd>` : ''}
</dl>
<ul class="hw-compat">${Object.entries(m.compat).filter(([, c]) => c.state !== 'absent').map(([id, c]) => `<li>${comp(id)} ${st(c.state)}</li>`).join('')}</ul>
${m.perf.length ? `<p class="hw-perf">${m.perf.map((p) => `<span>${esc((t.perf && t.perf[p.cat]) || p.cat)} <b>${fmt(p.value)}</b> ${esc(p.unit)}</span>`).join(' · ')}</p>` : ''}
</article>`).join('\n');
  const gpuRows = model.gpus.map((g) => `<tr><td>${esc(g.name)}</td><td><code>${esc(g.driver)}</code></td><td>${g.works ? st('works') : g.limits ? st('limits') : st('no')}</td><td>${esc(g.versions.join(', '))}</td></tr>`).join('\n');
  const note = (k) => (k && t.notes && t.notes[k] ? ` ${esc(t.notes[k])}` : '');
  const probRows = model.problems.map((p) => `<tr><td>${esc(p.machine)}</td><td>${comp(p.id)}</td><td>${st(p.state)}${note(p.note)}</td><td>${esc(p.lordos)}</td><td>${p.fixedIn ? esc(p.fixedIn) : '—'}</td></tr>`).join('\n');
  return {
    machines,
    gpus: `<div class="hw-table"><table><thead><tr><th>${esc(t.gpu)}</th><th>${esc(t.driver)}</th><th>${esc(t.stateHead)}</th><th>${esc(t.versions)}</th></tr></thead><tbody>${gpuRows}</tbody></table></div>`,
    problems: probRows
      ? `<div class="hw-table"><table><thead><tr><th>${esc(t.machine)}</th><th>${esc(t.componentHead)}</th><th>${esc(t.stateHead)}</th><th>${esc(t.reportedIn)}</th><th>${esc(t.fixedIn)}</th></tr></thead><tbody>${probRows}</tbody></table></div>`
      : `<p class="hw-none">${esc(t.noProblems)}</p>`,
  };
}

// ---- command line ----
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const arg = (name, def) => { const i = process.argv.indexOf(name); return i > 0 && process.argv[i + 1] ? path.resolve(process.argv[i + 1]) : def; };
  const dir = arg('--reports', REPORTS_DIR), out = arg('--snapshot', SNAPSHOT_FILE);
  try {
    const entries = readReports(dir);
    const text = snapshotText(buildSnapshot(entries));
    if (process.argv.includes('--check')) {
      const have = fs.existsSync(out) ? fs.readFileSync(out, 'utf8') : '';
      if (have !== text) { console.error(`${path.relative(REPO, out)} is not the snapshot of ${path.relative(REPO, dir)}: run node tools/hwreport/catalog.mjs`); process.exit(1); }
      console.log(`snapshot up to date (${entries.length} reports)`);
    } else if (fs.existsSync(out) && fs.readFileSync(out, 'utf8') === text) {
      console.log(`${path.relative(REPO, out)} unchanged (${entries.length} reports)`);
    } else {
      fs.mkdirSync(path.dirname(out), { recursive: true });
      fs.writeFileSync(out, text);
      const sig = `${out}.sig`;
      if (fs.existsSync(sig)) { fs.unlinkSync(sig); console.log(`removed the old ${path.basename(sig)}: sign again with tools/release/sign-hwsnapshot.sh`); }
      console.log(`wrote ${path.relative(REPO, out)}: ${entries.length} reports, ${JSON.parse(text).devices.length} device entries`);
    }
  } catch (e) { console.error(String(e.message || e)); process.exit(1); }
}
