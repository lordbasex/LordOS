// themes-sync.mjs — the official themes come from their own repository, LordOS-themes (github.com/lordbasex/LordOS-themes),
// the way the wallpapers come from LordOS-wallpapers. LordOS keeps a vendored snapshot of it in themes/<id>/, so
// `npm run build`, the .deb, the ISO and the Raspberry Pi image build offline from this tree, exactly as before:
//
//   themes/upstream.json   the pin: the repository and the commit (and tag, when there is one) the snapshot is from,
//                          and the SHA-256 of that commit's catalog.json
//   themes/catalog.json    a copy of that commit's catalog.json (as assets/wallpapers/motion.json is of the wallpapers'):
//                          each theme's id, folder, name, author, email, organization, version, minLordOS, verified
//                          star, and the size and SHA-256 of every file
//   themes/<id>/           the theme's source as the catalog lists it (folder flow/ is the id default; the others keep
//                          their names). README, LICENSE, provenance and screenshots stay in the repository.
//
// Usage (from anywhere; paths are the repository's):
//   node tools/themes-sync.mjs                      check (offline): the pin names this catalog, and every themes/<id>/
//                                                   file is the catalog's, by size and SHA-256, with nothing extra.
//                                                   Exit 1 otherwise. Run by server/tests/run_suites.sh.
//   node tools/themes-sync.mjs --from <clone> [--ref <commit|tag>]
//                                                   copy LordOS-themes@<ref> (default: the pinned commit) from a local
//                                                   clone into themes/<id>/, checking every file's size and SHA-256
//                                                   against that commit's catalog.json first (nothing is written if
//                                                   one differs, or the commit has a file the catalog doesn't list);
//                                                   then writes themes/catalog.json and moves the pin to that commit.
//   node tools/themes-sync.mjs --export <clone>     the other way, while developing a theme in this tree: copy each
//                                                   themes/<id>/ source (git's tracked files) into <clone>/<folder>/
//                                                   for a commit there (run its `node tools/catalog.mjs` next).
//
// Only git is used to read the clone (`git cat-file`, `git ls-tree`): nothing is fetched, nothing is run from it. The
// built bundles (themes/<id>/theme.bundle.js) are signed by the code manifest as before (tools/code-manifest.mjs).
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import fs from 'node:fs';
import path from 'node:path';

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const THEMES = path.join(root, 'themes');
const PIN = path.join(THEMES, 'upstream.json');
const CATALOG = path.join(THEMES, 'catalog.json');
// what the build leaves in themes/<id>/ (ignored by git): never part of the snapshot
const BUILT = [/^theme\.bundle\.js(\.map)?$/, /\.vm-e2e\./];
const ID = /^[a-z][a-z0-9-]{0,31}$/;
const FILE = /^(?!\/)(?!.*\/\/)(?!.*(^|\/)\.)[A-Za-z0-9_./-]{1,200}$/;   // relative, no "..", no hidden file, no "//"
const HEX = /^[0-9a-f]{64}$/;

const sha = (b) => createHash('sha256').update(b).digest('hex');
const fail = (msg) => { console.error(`themes-sync: ${msg}`); process.exit(1); };

// The catalog's shape, checked before any of it is used: ids and folders are names, files relative paths.
export function checkCatalog(cat) {
  const bad = [];
  if (!cat || cat.format !== 1 || !Array.isArray(cat.themes)) return ['not a format-1 catalog'];
  const ids = new Set(), folders = new Set();
  for (const t of cat.themes) {
    if (!t || !ID.test(t.id) || !ID.test(t.folder)) { bad.push(`bad id or folder ${JSON.stringify(t && t.id)}`); continue; }
    if (ids.has(t.id) || folders.has(t.folder)) bad.push(`${t.id}: listed twice`);
    if (t.id === 'greeter' || t.id === 'zscene') bad.push(`${t.id}: reserved id`);
    ids.add(t.id); folders.add(t.folder);
    for (const key of ['files', 'docs', 'screenshots']) {
      if (!Array.isArray(t[key])) { bad.push(`${t.id}: no ${key}`); continue; }
      for (const f of t[key]) {
        if (!f || !FILE.test(f.path) || f.path.split('/').includes('..') || !Number.isInteger(f.size) || f.size < 0 || !HEX.test(f.sha256)) bad.push(`${t.id}: bad entry ${JSON.stringify(f && f.path)}`);
      }
    }
    if (!t.files.some((f) => f.path === 'index.jsx') || !t.files.some((f) => f.path === 'theme.json')) bad.push(`${t.id}: needs index.jsx and theme.json`);
  }
  return bad;
}

// Every file under dir (relative, sorted), without the build's outputs.
function walk(dir, rel = '') {
  const out = [];
  let ents = [];
  try { ents = fs.readdirSync(path.join(dir, rel), { withFileTypes: true }); } catch { return out; }
  for (const e of ents) {
    const r = rel ? `${rel}/${e.name}` : e.name;
    if (e.isDirectory()) out.push(...walk(dir, r));
    else if (!(rel === '' && BUILT.some((re) => re.test(e.name)))) out.push(r);
  }
  return out.sort();
}

function readJson(p) {
  try { return JSON.parse(fs.readFileSync(p, 'utf8')); } catch (e) { return fail(`can't read ${path.relative(root, p)}: ${e.message}`); }
}

// --check: the snapshot in this tree is exactly the pinned catalog's.
export function checkTree() {
  const problems = [];
  const pin = readJson(PIN);
  const text = fs.readFileSync(CATALOG);
  if (!/^[0-9a-f]{40}$/.test(pin.commit || '')) problems.push('upstream.json: no commit');
  if (pin.catalogSha256 !== sha(text)) problems.push('themes/catalog.json is not the pinned commit\'s (catalogSha256 differs)');
  const cat = JSON.parse(text);
  problems.push(...checkCatalog(cat));
  if (problems.length) return problems;
  for (const t of cat.themes) {
    const dir = path.join(THEMES, t.id);
    const want = new Map(t.files.map((f) => [f.path, f]));
    for (const rel of walk(dir)) if (!want.has(rel)) problems.push(`themes/${t.id}/${rel}: not in the catalog (change it in LordOS-themes, then sync)`);
    for (const f of t.files) {
      let b;
      try { b = fs.readFileSync(path.join(dir, f.path)); } catch { problems.push(`themes/${t.id}/${f.path}: missing`); continue; }
      if (b.length !== f.size || sha(b) !== f.sha256) problems.push(`themes/${t.id}/${f.path}: differs from LordOS-themes@${pin.commit.slice(0, 12)}`);
    }
  }
  return problems;
}

const git = (clone, args, enc = 'utf8') => execFileSync('git', ['-C', clone, ...args], { encoding: enc, maxBuffer: 256 << 20, stdio: ['ignore', 'pipe', 'pipe'] });

function syncFrom(clone, ref) {
  const pin = fs.existsSync(PIN) ? readJson(PIN) : {};
  const want = ref || pin.commit;
  if (!want) fail('no --ref and no pinned commit');
  let commit;
  try { commit = git(clone, ['rev-parse', '--verify', `${want}^{commit}`]).trim(); } catch { fail(`${clone}: no commit ${want}`); }
  const blob = (p) => git(clone, ['cat-file', 'blob', `${commit}:${p}`], 'buffer');
  const catText = blob('catalog.json');
  const cat = JSON.parse(catText.toString('utf8'));
  const bad = checkCatalog(cat);
  if (bad.length) fail(`catalog.json@${commit.slice(0, 12)}: ${bad.join('; ')}`);
  // read and check everything first; write only when all of it holds
  const staged = [];
  for (const t of cat.themes) {
    const listed = new Set([...t.files, ...t.docs, ...t.screenshots].map((f) => f.path));
    const tree = git(clone, ['ls-tree', '-r', '--name-only', commit, '--', `${t.folder}/`]).split('\n').filter(Boolean).map((p) => p.slice(t.folder.length + 1));
    for (const p of tree) if (!listed.has(p)) fail(`${t.folder}/${p}@${commit.slice(0, 12)}: not in catalog.json`);
    for (const f of [...t.files, ...t.docs, ...t.screenshots]) {
      const b = blob(`${t.folder}/${f.path}`);
      if (b.length !== f.size || sha(b) !== f.sha256) fail(`${t.folder}/${f.path}@${commit.slice(0, 12)}: size or SHA-256 isn't the catalog's`);
    }
    staged.push({ t, files: t.files.map((f) => [f.path, blob(`${t.folder}/${f.path}`)]) });
  }
  for (const { t, files } of staged) {
    const dir = path.join(THEMES, t.id);
    for (const rel of walk(dir)) fs.rmSync(path.join(dir, rel));
    for (const [rel, b] of files) { fs.mkdirSync(path.dirname(path.join(dir, rel)), { recursive: true }); fs.writeFileSync(path.join(dir, rel), b); }
    // folders left empty
    for (const d of fs.readdirSync(dir, { recursive: true }).map(String).sort().reverse()) {
      const abs = path.join(dir, d);
      try { if (fs.statSync(abs).isDirectory() && !fs.readdirSync(abs).length) fs.rmdirSync(abs); } catch { /* gone */ }
    }
    console.log(`themes/${t.id} ← ${t.folder}/ (${files.length} files)`);
  }
  let tag = '';
  try { tag = git(clone, ['describe', '--tags', '--exact-match', commit]).trim(); } catch { /* no tag */ }
  fs.writeFileSync(CATALOG, catText);
  const out = { format: 1, repository: cat.repository || 'lordbasex/LordOS-themes', url: `https://github.com/${cat.repository || 'lordbasex/LordOS-themes'}`,
    commit, tag, catalogSha256: sha(catText) };
  fs.writeFileSync(PIN, `${JSON.stringify(out, null, 2)}\n`);
  console.log(`pinned LordOS-themes@${commit.slice(0, 12)}${tag ? ` (${tag})` : ''}; run npm run build`);
}

function exportTo(clone) {
  const cat = readJson(CATALOG);
  for (const t of cat.themes) {
    const tracked = execFileSync('git', ['-C', root, 'ls-files', '--', `themes/${t.id}/`], { encoding: 'utf8' }).split('\n').filter(Boolean);
    const dest = path.join(clone, t.folder);
    const keep = new Set([...t.docs, ...t.screenshots].map((f) => f.path));
    for (const rel of walk(dest)) if (!keep.has(rel) && !rel.startsWith('screenshots/')) fs.rmSync(path.join(dest, rel));
    for (const p of tracked) {
      const rel = p.slice(`themes/${t.id}/`.length);
      fs.mkdirSync(path.dirname(path.join(dest, rel)), { recursive: true });
      fs.copyFileSync(path.join(root, p), path.join(dest, rel));
    }
    console.log(`${t.folder}/ ← themes/${t.id} (${tracked.length} files)`);
  }
  console.log('now, in the clone: node tools/catalog.mjs, review, commit -s; then here: --from <clone> --ref <commit>');
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const a = process.argv.slice(2);
  const opt = (k) => { const i = a.indexOf(k); return i >= 0 ? a[i + 1] : undefined; };
  if (a.includes('--export')) exportTo(path.resolve(opt('--export')));
  else if (a.includes('--from')) syncFrom(path.resolve(opt('--from')), opt('--ref'));
  else {
    const problems = checkTree();
    for (const p of problems) console.log(`✗ ${p}`);
    const pin = readJson(PIN);
    console.log(`themes-sync: ${problems.length ? `${problems.length} problem(s)` : `ok (LordOS-themes@${String(pin.commit).slice(0, 12)}${pin.tag ? ` ${pin.tag}` : ''})`}`);
    process.exit(problems.length ? 1 : 0);
  }
}
