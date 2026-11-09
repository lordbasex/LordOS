#!/usr/bin/env node
// app-versions.mjs — every native app carries its own version (apps/README.md), shown in the Store; it must change
// when the app changes. This refuses a release where an app's files differ from the previous release but its
// "version" doesn't (GitHub #91: 0.8.8.6 changed Files, Monitor and Settings and still showed their old versions).
//
//   node tools/app-versions.mjs [REF]      REF: the previous release (default: the newest v* tag before HEAD)
//   node tools/app-versions.mjs --bump [REF]   raises the patch number of each app that needs it
//
// An app that didn't exist at REF is new and passes with any version.
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const git = (...a) => execFileSync('git', a, { cwd: root, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim();

const args = process.argv.slice(2);
const bump = args.includes('--bump');
let ref = args.find((a) => !a.startsWith('--'));
if (!ref) {
  try { ref = git('describe', '--tags', '--abbrev=0', '--match', 'v*', 'HEAD'); } catch (e) { console.log('app-versions: no release tag, nothing to compare'); process.exit(0); }
}

// 0.8.5 → 0.8.6, 0.8.9-alpha.1 → 0.8.9-alpha.2
export function nextVersion(v) {
  const pre = /^(\d+\.\d+\.\d+[-~][0-9A-Za-z.]*?)(\d+)$/.exec(v);
  if (pre) return `${pre[1]}${Number(pre[2]) + 1}`;
  const m = /^(\d+)\.(\d+)\.(\d+)$/.exec(v);
  if (!m) throw new Error(`not a version: ${v}`);
  return `${m[1]}.${m[2]}.${Number(m[3]) + 1}`;
}

const dir = path.join(root, 'apps');
const ids = fs.readdirSync(dir).filter((d) => fs.existsSync(path.join(dir, d, 'manifest.json'))).sort();
const stale = [];
for (const id of ids) {
  let old;
  try { old = JSON.parse(git('show', `${ref}:apps/${id}/manifest.json`)).version; } catch (e) { continue; }   // new app
  const file = path.join(dir, id, 'manifest.json');
  const now = JSON.parse(fs.readFileSync(file, 'utf8')).version;
  if (now !== old) continue;
  // committed changes since REF, and changes not committed yet
  const changed = git('diff', '--name-only', ref, '--', `apps/${id}`);
  if (!changed) continue;
  stale.push({ id, version: now, files: changed.split('\n').length, file });
}

if (!stale.length) { console.log(`app-versions: every app that changed since ${ref} has a new version`); process.exit(0); }
if (bump) {
  for (const s of stale) {
    const text = fs.readFileSync(s.file, 'utf8');
    const next = nextVersion(s.version);
    fs.writeFileSync(s.file, text.replace(`"version": "${s.version}"`, `"version": "${next}"`));
    console.log(`${s.id}: ${s.version} → ${next}`);
  }
  process.exit(0);
}
for (const s of stale) console.log(`apps/${s.id}: ${s.files} file(s) changed since ${ref}, still version ${s.version}`);
console.log('app-versions: raise those versions (node tools/app-versions.mjs --bump)');
process.exit(1);
