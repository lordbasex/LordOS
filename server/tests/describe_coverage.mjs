// describe_coverage.mjs — (0.8.9.6) how many of this computer's running processes Get Info can describe, by source and
// language (a test tool, read-only, never part of `make test`): /proc for the processes, build/describe-cli for what
// lord-server would read (dpkg, Debian's translations, manual pages, units), sdk/describemodel.js for the order.
// From server/, after `make build/describe-cli`:   node tests/describe_coverage.mjs [lang]
import fs from 'node:fs';
import { spawnSync } from 'node:child_process';
import { createDescriber } from '../../sdk/describemodel.js';

const lang = process.argv[2] || 'es';
const read = (p) => { try { return fs.readFileSync(p, 'utf8'); } catch (e) { return ''; } };
const procs = [];
for (const d of fs.readdirSync('/proc')) {
  if (!/^\d+$/.test(d)) continue;
  const stat = read(`/proc/${d}/stat`);
  const r = stat.lastIndexOf(')');
  if (r < 0) continue;
  const name = stat.slice(stat.indexOf('(') + 1, r);
  const ppid = Number(stat.slice(r + 2).split(' ')[1]);
  let exe = '';
  try { exe = fs.readlinkSync(`/proc/${d}/exe`); } catch (e) { exe = ''; }
  const kernel = Number(d) === 2 || ppid === 2;
  const unit = (read(`/proc/${d}/cgroup`).trim().split('/').pop() || '');
  procs.push({ pid: Number(d), name, exe, prot: kernel ? 'kernel' : null, unit: /\.service$/.test(unit) && !/^user@/.test(unit) ? unit : null });
}
const answers = new Map();
const ask = (lines) => {
  const want = lines.filter((l) => !answers.has(l));
  if (!want.length) return;
  const r = spawnSync('./build/describe-cli', { input: `${want.join('\n')}\n`, encoding: 'utf8', maxBuffer: 64 << 20 });
  const out = r.stdout.split('\n');
  want.forEach((l, i) => answers.set(l, JSON.parse(out[i] || 'null')));
};
// the owners first
const exes = [...new Set(procs.map((p) => p.exe.replace(/ \(deleted\)$/, '')).filter(Boolean))];
ask(exes.map((e) => `owner ${e} en`));
for (const p of procs) if (p.exe) p.pkg = answers.get(`owner ${p.exe.replace(/ \(deleted\)$/, '')} en`) || null;
let asked = [];
const d = createDescriber({
  native: (kind, arg, l) => { const k = `${kind} ${arg} ${l}`; if (answers.has(k)) return answers.get(k) || false; asked.push(k); return null; },
  getApp: () => null, webAppEntry: () => null, readCache: () => '{}', writeCache: () => {},
});
for (let pass = 0; pass < 3; pass++) {
  asked = [];
  for (const p of procs) d.describeProgram(p, { lang });
  if (!asked.length) break;
  ask([...new Set(asked)]);
}
const by = {}, langs = {};
let known = 0;
const unknown = [];
for (const p of procs) {
  const r = d.describeProgram(p, { lang });
  const k = r.unknown ? 'unknown' : r.pending ? 'pending' : r.src;
  by[k] = (by[k] || 0) + 1;
  if (!r.unknown && !r.pending) { known++; langs[r.lang] = (langs[r.lang] || 0) + 1; } else unknown.push(`${p.name} ${p.exe}`);
}
console.log(JSON.stringify({ lang, processes: procs.length, described: known, bySource: by, byLanguage: langs, unknownSample: [...new Set(unknown)].slice(0, 25) }, null, 1));
