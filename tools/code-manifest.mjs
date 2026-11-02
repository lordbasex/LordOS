// Writes code-manifest.json: the size and SHA-256 of every file LordOS runs as its own code (see
// docs/SIGNING.md, "Verified system code"). axia-server checks each file against it before loading
// it (server/src/sec/codesig.c); tools/release/sign-code.sh signs it with the LordOS release key.
//
//   node tools/code-manifest.mjs [--root <dir>] [--check]
//
// <dir> (default: the repository) holds themes/ and assets/ as installed (/usr/lib/lordos has the
// same layout). The output is deterministic: the same files give the same bytes, so a signature
// made on the maintainer's machine stays valid for the .deb, the ISO and the Raspberry Pi image.
// If the manifest changes, a code-manifest.sig next to it no longer matches and is removed.
// --check: only compare with the existing manifest (exit 1 if it would change).
//
// Listed: themes/*/theme.bundle.js (the shell, the SDK, the runtime and every native app),
// assets/app-host.bundle.js (React + SDK of isolated apps) and assets/sysapps/*/app.js (system apps
// that run as their own process). Not listed (data, not code): fonts, locales, icons, images,
// help articles, theme.json and the sysapps' manifest.json (window size only).
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import fs from 'node:fs';
import path from 'node:path';

const repo = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const args = process.argv.slice(2);
const at = args.indexOf('--root');
const root = at >= 0 ? path.resolve(args[at + 1]) : repo;
const check = args.includes('--check');

export function codeFiles(dir) {
  const out = [];
  const add = (rel) => { try { if (fs.lstatSync(path.join(dir, rel)).isFile()) out.push(rel); } catch { /* not built */ } };
  const list = (rel) => { try { return fs.readdirSync(path.join(dir, rel)).sort(); } catch { return []; } };
  for (const t of list('themes')) add(`themes/${t}/theme.bundle.js`);
  add('assets/app-host.bundle.js');
  for (const a of list('assets/sysapps')) add(`assets/sysapps/${a}/app.js`);
  // byte order, as axia-server compares paths with strcmp
  return out.sort((a, b) => (Buffer.compare(Buffer.from(a), Buffer.from(b))));
}

export function manifestText(dir, version) {
  const files = codeFiles(dir).map((rel) => {
    const data = fs.readFileSync(path.join(dir, rel));
    return `    { "path": ${JSON.stringify(rel)}, "size": ${data.length}, "sha256": "${createHash('sha256').update(data).digest('hex')}" }`;
  });
  return `{\n  "format": 1,\n  "kind": "lordos-system-code",\n  "version": ${JSON.stringify(version)},\n  "files": [\n${files.join(',\n')}\n  ]\n}\n`;
}

export function writeManifest(dir = root) {
  const version = JSON.parse(fs.readFileSync(path.join(repo, 'package.json'), 'utf8')).version;
  const text = manifestText(dir, version);
  const file = path.join(dir, 'code-manifest.json'), sig = path.join(dir, 'code-manifest.sig');
  const old = fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : null;
  if (old === text) return { changed: false, files: (text.match(/"path"/g) || []).length };
  if (check) return { changed: true };
  fs.writeFileSync(file, text);
  if (fs.existsSync(sig)) fs.rmSync(sig);           // signed a different manifest
  return { changed: true, files: (text.match(/"path"/g) || []).length };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const r = writeManifest(root);
  if (check) { console.log(r.changed ? 'code-manifest.json is out of date' : 'code-manifest.json is up to date'); process.exit(r.changed ? 1 : 0); }
  console.log(`code-manifest.json: ${r.files} files${r.changed ? '' : ' (unchanged)'}`);
}
