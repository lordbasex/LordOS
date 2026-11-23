#!/usr/bin/env node
// mkdeb-llm.mjs — builds Sentinela's model packages (docs/SENTINELA.md, "Models") for the signed APT repository
// (tools/release/apt-repo.sh), the way tools/voice/mkdeb-voice.mjs builds the voices. One package per model of
// sdk/llmcatalog.js, Architecture: all:
//
//   lordos-llm-<id>    /usr/share/lordos/llm/<id>.gguf (0644, root), the only folder lord-server takes models from
//                      (server/src/llm/engine.h); Recommends: llama.cpp (the engine, LordOS's own package)
//
//   node tools/llm/mkdeb-llm.mjs <source folder> <output folder> <version: YYYY.MM.DD[.N]>
//
// The source folder is what tools/llm/fetch-models.sh downloaded. Every file is checked again against the SHA-256
// pinned in sdk/llmcatalog.js before it is packaged: a package is never built from a file that doesn't match. The
// packages are reproducible (SOURCE_DATE_EPOCH from the version's date, files owned by root) and stored uncompressed:
// the weights don't compress.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { LLM_MODELS, ENGINE_PACKAGE } = await import(path.join(here, '../../sdk/llmcatalog.js'));

const [src, out, version] = process.argv.slice(2);
if (!src || !out || !/^\d{4}\.\d{2}\.\d{2}(\.\d{1,2})?$/.test(version || '')) {
  console.error('usage: mkdeb-llm.mjs <source folder> <output folder> <version: YYYY.MM.DD[.N]>');
  process.exit(2);
}
fs.mkdirSync(out, { recursive: true });
const [y, m, d] = version.split('.').map(Number);
const epoch = Math.floor(Date.UTC(y, m - 1, d) / 1000);

function sha256(file) {
  const h = crypto.createHash('sha256');
  const fd = fs.openSync(file, 'r');
  const buf = Buffer.alloc(1 << 22);
  let n;
  while ((n = fs.readSync(fd, buf, 0, buf.length, null)) > 0) h.update(buf.subarray(0, n));
  fs.closeSync(fd);
  return h.digest('hex');
}

function* walk(dir) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) yield* walk(p);
    yield p;
  }
}

const copyright = (mo) => `Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: ${mo.name}
Source: https://huggingface.co/${mo.source}
Comment: Packaged by LordOS (https://lordos.org) for Sentinela, unchanged: ${mo.file}, SHA-256 ${mo.sha256}.

Files: *
Copyright: Alibaba Cloud (the Qwen team)
License: ${mo.license}
 The model is licensed under the Apache License, Version 2.0. On Debian systems the full text is in
 /usr/share/common-licenses/Apache-2.0.
`;

const made = [];
// ONLY=<id>: just that model (a test build)
for (const mo of LLM_MODELS.filter((x) => !process.env.ONLY || x.id === process.env.ONLY)) {
  const file = path.join(src, mo.file);
  if (!fs.existsSync(file) || fs.statSync(file).size !== mo.bytes) throw new Error(`${file}: missing or not ${mo.bytes} bytes`);
  const got = sha256(file);
  if (got !== mo.sha256) throw new Error(`${file}: SHA-256 ${got}, sdk/llmcatalog.js pins ${mo.sha256}`);
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'lordos-llm-'));
  try {
    const dst = path.join(root, 'usr/share/lordos/llm', `${mo.id}.gguf`);
    fs.mkdirSync(path.dirname(dst), { recursive: true });
    try { fs.linkSync(file, dst); } catch (e) { if (e.code === 'EXDEV') fs.copyFileSync(file, dst); else throw e; }
    fs.chmodSync(dst, 0o644);
    const doc = path.join(root, 'usr/share/doc', mo.package);
    fs.mkdirSync(doc, { recursive: true });
    fs.writeFileSync(path.join(doc, 'copyright'), copyright(mo));
    fs.mkdirSync(path.join(root, 'DEBIAN'));
    fs.writeFileSync(path.join(root, 'DEBIAN/control'), [
      `Package: ${mo.package}`, `Version: ${version}`, 'Architecture: all', 'Section: misc', 'Priority: optional',
      'Maintainer: LordOS <lordos@lordos.org>', `Installed-Size: ${Math.ceil(mo.bytes / 1024)}`, `Recommends: ${ENGINE_PACKAGE}`,
      'Homepage: https://lordos.org', `Description: ${mo.name} local language model for Sentinela`,
      ` ${mo.name} (${mo.quant}, GGUF) from ${mo.source}, unchanged, for LordOS's Sentinela: it explains`,
      ' its alerts on this computer, without network, through llama.cpp in its sandbox. About',
      ` ${Math.round(mo.ramMb / 100) / 10} GB of memory while it analyzes; nothing runs while Sentinela is off.`,
    ].join('\n') + '\n');
    for (const p of walk(root)) { if (fs.statSync(p).isDirectory()) fs.chmodSync(p, 0o755); fs.utimesSync(p, epoch, epoch); }
    fs.chmodSync(root, 0o755);
    fs.utimesSync(root, epoch, epoch);
    const deb = path.join(out, `${mo.package}_${version}_all.deb`);
    execFileSync('dpkg-deb', ['--root-owner-group', '-Znone', '--build', root, deb], {
      env: { ...process.env, SOURCE_DATE_EPOCH: String(epoch) }, stdio: ['ignore', 'ignore', 'inherit'] });
    made.push(deb);
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
}
for (const f of made) console.log(f);
