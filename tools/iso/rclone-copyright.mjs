#!/usr/bin/env node
// rclone-copyright.mjs — writes packaging/rclone/copyright-v<version> for tools/iso/mkdeb-rclone.sh: rclone's own license,
// then the license of every Go module linked into the official release binary, in the binary's own order.
//
//   node tools/iso/rclone-copyright.mjs <the release's rclone binary> > packaging/rclone/copyright-v<version>
//
// The modules and their exact versions come from the binary itself (`go version -m`: what is linked, not what go.mod
// lists); each module's license file comes from the Go module cache (`go mod download -json`, checked against go.sum by
// the go command), so it needs Go and, for modules not cached yet, proxy.golang.org. A module without a license file
// points to its page on pkg.go.dev. Run it on the previous version's binary to check the output is unchanged.
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const bin = process.argv[2];
if (!bin || !fs.existsSync(bin)) { console.error('usage: rclone-copyright.mjs <rclone binary>'); process.exit(2); }
const go = process.env.GO || 'go';
const run = (args) => execFileSync(go, args, { encoding: 'utf8', maxBuffer: 64 << 20, env: { ...process.env, GOFLAGS: '-mod=mod', GO111MODULE: 'on' } });

// the binary's modules: "dep path version sum", each maybe followed by "=> replacement version sum"
const lines = run(['version', '-m', bin]).split('\n');
let main = null;
const deps = [];
for (const l of lines) {
  const f = l.trim().split(/\t/);
  if (f[0] === 'mod') main = { path: f[1], version: f[2].replace(/\+dirty$/, '') };
  else if (f[0] === 'dep') deps.push({ path: f[1], version: f[2], from: { path: f[1], version: f[2] } });
  else if (f[0] === '=>' && deps.length) deps[deps.length - 1].from = { path: f[1], version: f[2] };
}
if (!main) { console.error(`${bin}: no module information (not a Go binary?)`); process.exit(1); }

const LICENSE = /^(LICEN[CS]E|COPYING|COPYRIGHT)([-._].*)?$/i;
function licenseOf(mod) {
  let dir;
  try { dir = JSON.parse(run(['mod', 'download', '-json', `${mod.path}@${mod.version}`])).Dir; } catch (e) { return null; }
  if (!dir) return null;
  // LICENSE first (a module may also ship a COPYING.md about something else), then LICENCE, COPYING, COPYRIGHT
  const rank = (n) => ['LICENSE', 'LICENCE', 'COPYING', 'COPYRIGHT'].findIndex((p) => n.toUpperCase().startsWith(p));
  const files = fs.readdirSync(dir).filter((n) => LICENSE.test(n) && fs.statSync(path.join(dir, n)).isFile())
    // a module under two licenses (LICENSE-APACHE and LICENSE-MIT) is taken under MIT, as rclone itself is
    .sort((a, b) => rank(a) - rank(b) || (/MIT/i.test(b) - /MIT/i.test(a)) || (a < b ? -1 : a > b ? 1 : 0));
  return files.length ? fs.readFileSync(path.join(dir, files[0]), 'utf8') : null;
}
// a license text as a copyright file's field: every line indented by one space, an empty line as " ." (its bytes otherwise
// as the module ships them); the file's last newline ends the field with an empty line, as the 1.75.2 file was made
function field(text) {
  const parts = text.split('\n');
  const last = parts.pop();
  const out = parts.map((l) => (l.replace(/\r$/, '') === '' ? ' .' : ` ${l}`));
  out.push(last === '' ? '' : ` ${last}`);
  return out.join('\n');
}

const own = licenseOf(main);
if (!own) { console.error(`no license file for ${main.path}@${main.version}`); process.exit(1); }
const out = ['Files: *', 'Copyright: Nick Craig-Wood and the rclone contributors', 'License: Expat', field(own)];
for (const d of deps) {
  const text = licenseOf(d.from);
  out.push(`Files: (Go module) ${d.path} ${d.version}`);
  out.push(text ? `License: see below\n${field(text)}` : `License: see https://pkg.go.dev/${d.path}?tab=licenses\n`);
}
process.stdout.write(out.join('\n'));
