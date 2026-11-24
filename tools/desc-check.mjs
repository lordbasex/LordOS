// desc-check.mjs — (0.8.9.6) every LordOS app and program says what it is, in English at least (docs/MONITOR.md,
// "Descriptions"): Monitor's Get Info, and anything else that asks sdk/describe.js, shows it.
//
//   node tools/desc-check.mjs           (also run by tools/i18n-check.mjs)
//
// Fails (exit 1) when an app in apps/<id>/manifest.json has no "description", or a LordOS program has no text in
// sdk/programdesc.json "lordos": every build/lord-* that server/Makefile or server/mk/*.mk builds (but the test builds)
// and every lord-* that tools/iso/mkdeb.sh ships. The texts are English; other languages come for free when the system
// has them (Debian's translations, Comment[lang]) or from "Translate with Lumo".
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
// builds that exist only for the tests (never installed)
const TEST_BUILD = /-(test|testpaths|pkgtest|abi5|norule|encfail|hwfall)$/;

export function checkDescriptions() {
  const problems = [];
  const desc = JSON.parse(fs.readFileSync(path.join(root, 'sdk/programdesc.json'), 'utf8'));
  for (const id of fs.readdirSync(path.join(root, 'apps')).sort()) {
    const mf = path.join(root, 'apps', id, 'manifest.json');
    if (!fs.existsSync(mf)) continue;
    const m = JSON.parse(fs.readFileSync(mf, 'utf8'));
    if (typeof m.description !== 'string' || m.description.trim().length < 8) problems.push(`apps/${id}/manifest.json has no English "description"`);
  }
  const programs = new Set();
  const mk = [path.join(root, 'server/Makefile'), ...fs.readdirSync(path.join(root, 'server/mk')).filter((f) => f.endsWith('.mk')).map((f) => path.join(root, 'server/mk', f))];
  for (const f of mk) for (const m of fs.readFileSync(f, 'utf8').matchAll(/^build\/(lord-[a-z0-9-]+):/gm)) if (!TEST_BUILD.test(m[1])) programs.add(m[1]);
  const deb = path.join(root, 'tools/iso/mkdeb.sh');
  if (fs.existsSync(deb)) for (const m of fs.readFileSync(deb, 'utf8').matchAll(/\b(lord-[a-z0-9-]+)\b/g)) if (!TEST_BUILD.test(m[1]) && !/\.(service|socket|timer)$/.test(m[1])) programs.add(m[1]);
  for (const p of [...programs].sort()) {
    const t = desc.lordos?.[p];
    if (typeof t !== 'string' || t.trim().length < 8) problems.push(`${p} (built or shipped) has no description in sdk/programdesc.json "lordos"`);
  }
  for (const [k, v] of Object.entries({ ...desc.kernel, ...desc.programs, ...desc.lordos })) {
    if (typeof v !== 'string' || !v.trim()) problems.push(`sdk/programdesc.json: "${k}" has no text`);
  }
  return { problems, programs: programs.size };
}

if (process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) {
  const { problems, programs } = checkDescriptions();
  for (const p of problems) console.log(`✗ ${p}`);
  console.log(problems.length ? `desc-check: ${problems.length} problems` : `desc-check: every app and ${programs} LordOS programs have a description`);
  process.exit(problems.length ? 1 : 0);
}
