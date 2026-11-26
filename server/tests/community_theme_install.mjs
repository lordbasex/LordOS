// community_theme_install.mjs — puts a community theme into a test home the way lord-server's install does, without
// lord-themec: compiled by tools/themec/themec.js (the same compiler) into <home>/.local/share/lordos/themes/<id>/
// with its install record (the SHA-256 of theme.js). A test tool (community_theme_headless.sh).
//   node tests/community_theme_install.mjs <theme folder> <home> [--unchecked] [--untrusted] [--failed why]
// --unchecked builds code the checks would refuse (to test what lord-server does with it anyway).
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
export function loadThemec() {
  const ctx = {};
  vm.createContext(ctx);
  vm.runInContext(fs.readFileSync(path.join(here, '..', '..', 'tools', 'themec', 'themec.js'), 'utf8'), ctx);
  return ctx.LordThemec;
}
export function readTheme(dir) {
  const files = [];
  const walk = (r) => {
    for (const n of fs.readdirSync(path.join(dir, r)).sort()) {
      const rel = r ? `${r}/${n}` : n;
      const st = fs.lstatSync(path.join(dir, rel));
      if (st.isDirectory()) walk(rel);
      else files.push({ name: rel, size: st.size, text: /\.(js|jsx|json|md|txt)$|^(LICENSE|NOTICE)$/.test(n) ? fs.readFileSync(path.join(dir, rel), 'utf8') : undefined });
    }
  };
  walk('');
  return files;
}

function main(argv) {
  const [src, home] = argv.filter((a) => !a.startsWith('--') && argv[argv.indexOf(a) - 1] !== '--failed');
  const id = path.basename(path.resolve(src));
  const C = loadThemec();
  const r = C.compile({ id, files: readTheme(src) }, { checks: !argv.includes('--unchecked') });
  if (!r.code) { console.error(JSON.stringify(r.report.findings.filter((f) => f.level === 'error').slice(0, 10))); return 1; }
  const dir = path.join(home, '.local', 'share', 'lordos', 'themes', id);
  fs.mkdirSync(dir, { recursive: true, mode: 0o700 });
  for (let p = dir; p !== home; p = path.dirname(p)) fs.chmodSync(p, 0o700);
  for (const k of r.keep) { fs.mkdirSync(path.dirname(path.join(dir, k)), { recursive: true }); fs.copyFileSync(path.join(src, k), path.join(dir, k)); }
  if (!fs.existsSync(path.join(dir, 'theme.json'))) fs.copyFileSync(path.join(src, 'theme.json'), path.join(dir, 'theme.json'));
  fs.writeFileSync(path.join(dir, 'theme.js'), r.code);
  const meta = JSON.parse(fs.readFileSync(path.join(src, 'theme.json'), 'utf8'));
  const fi = argv.indexOf('--failed');
  fs.writeFileSync(path.join(dir, 'install.json'), `${JSON.stringify({
    v: 1, id, sha256: crypto.createHash('sha256').update(r.code).digest('hex'), author: meta.author || '', version: meta.version || '',
    caps: meta.capabilities || [], trusted: !argv.includes('--untrusted'), crashes: 0, failed: fi > 0 ? argv[fi + 1] : '', installed: 1,
  })}\n`);
  console.log(`installed ${id} into ${dir}`);
  return 0;
}
if (import.meta.url === `file://${process.argv[1]}`) process.exit(main(process.argv.slice(2)));
