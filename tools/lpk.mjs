// Packages and signs LordOS apps (.lpk = tar.gz; see docs/NATIVE-APPS.md and docs/SIGNING.md).
//
//   node tools/lpk.mjs <folder> [output.lpk] [--key key.json | --unsigned]
//       packages the app; signs it with --key, with $LPK_KEY or with ~/.config/axia/lpk-key.json
//       (if it exists). Without a key the result is «Unsigned» (still with contenido.json, to detect damage).
//   node tools/lpk.mjs keygen [--name "Name"] [--out file]
//       creates an Ed25519 key pair (default ~/.config/axia/lpk-key.json, mode 600).
//   node tools/lpk.mjs sign <package.lpk> --key key.json [output.lpk]
//       re-signs a package (with a new contenido.json).
//   node tools/lpk.mjs verify <package.lpk>
//       shows the signature, the key and whether the files match.
//   node tools/lpk.mjs catalog <source.json> --key official.json [folder]
//       writes signed catalog.json + catalog.sig (only valid with an official LordOS key).
//
// An app folder has manifest.json and index.jsx like the ones in apps/, and optionally
// icon.png, locales/<language>.json and assets/. React, @lordos/sdk and @lordos/runtime are NOT included.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import crypto from 'node:crypto';

const argv = process.argv.slice(2);
const flag = (name) => { const i = argv.indexOf(name); if (i < 0) return undefined; const v = argv[i + 1]; argv.splice(i, 2); return v; };
const bool = (name) => { const i = argv.indexOf(name); if (i < 0) return false; argv.splice(i, 1); return true; };
const die = (msg) => { console.error(`lpk: ${msg}`); process.exit(1); };
const DEFAULT_KEY = path.join(os.homedir(), '.config', 'axia', 'lpk-key.json');

// ---- Ed25519 keys (32-byte seed in hex, like pkgstate.c) ----
const PKCS8 = Buffer.from('302e020100300506032b657004220420', 'hex');
const privateKey = (seedHex) => crypto.createPrivateKey({ key: Buffer.concat([PKCS8, Buffer.from(seedHex, 'hex')]), format: 'der', type: 'pkcs8' });
const publicHex = (key) => crypto.createPublicKey(key).export({ format: 'der', type: 'spki' }).subarray(12).toString('hex');
function loadKey(file) {
  const k = JSON.parse(fs.readFileSync(file, 'utf8'));
  if (k.alg !== 'ed25519' || !/^[0-9a-f]{64}$/.test(k.secret || '')) die(`${file} is not an lpk.mjs key`);
  const priv = privateKey(k.secret);
  if (publicHex(priv) !== k.public) die(`${file}: the public key does not match the private one`);
  return { name: String(k.name || ''), public: k.public, priv };
}
const sha256 = (b) => crypto.createHash('sha256').update(b).digest('hex');

// ---- minimal ustar tar: regular files only ----
function header(name, size) {
  const h = Buffer.alloc(512);
  let prefix = '';
  if (name.length > 100) { const i = name.lastIndexOf('/', 155); prefix = name.slice(0, i); name = name.slice(i + 1); }
  h.write(name, 0, 100);
  h.write('0000644\0', 100); h.write('0000000\0', 108); h.write('0000000\0', 116);
  h.write(size.toString(8).padStart(11, '0') + '\0', 124);
  h.write(Math.floor(Date.now() / 1000).toString(8).padStart(11, '0') + '\0', 136);
  h.write('        ', 148); h.write('0', 156);
  h.write('ustar\0', 257); h.write('00', 263);
  h.write(prefix, 345, 155);
  let sum = 0;
  for (const b of h) sum += b;
  h.write(sum.toString(8).padStart(6, '0') + '\0 ', 148);
  return h;
}
function tar(files) {
  const parts = [];
  for (const [name, data] of files) {
    parts.push(header(name, data.length), data);
    if (data.length % 512) parts.push(Buffer.alloc(512 - (data.length % 512)));
  }
  parts.push(Buffer.alloc(1024));
  return Buffer.concat(parts);
}
function untar(buf) {
  if (buf[0] === 0x1f && buf[1] === 0x8b) buf = zlib.gunzipSync(buf);
  const out = [];
  for (let off = 0; off + 512 <= buf.length;) {
    const h = buf.subarray(off, off + 512);
    if (h.every((b) => b === 0)) break;
    const str = (a, n) => h.subarray(a, a + n).toString('latin1').replace(/\0.*$/s, '');
    const size = parseInt(str(124, 12).trim() || '0', 8);
    const prefix = str(257, 6) === 'ustar' ? str(345, 155) : '';
    const name = (prefix ? `${prefix}/` : '') + str(0, 100);
    const type = str(156, 1);
    if (type === '0' || type === '') out.push([name.replace(/^\.\//, ''), Buffer.from(buf.subarray(off + 512, off + 512 + size))]);
    off += 512 + Math.ceil(size / 512) * 512;
  }
  return out;
}

// contenido.json + firma.json (see server/src/sec/sign.c): the signature is over the bytes of contenido.json
function seal(files, manifest, key) {
  const list = files.filter(([p]) => p !== 'contenido.json' && p !== 'firma.json');
  const contents = {
    format: 1, id: manifest.id, version: String(manifest.version),
    ...(key ? { signer: { name: key.name, key: key.public } } : {}),
    files: list.map(([p, d]) => ({ path: p, size: d.length, sha256: sha256(d) })),
  };
  const text = Buffer.from(JSON.stringify(contents, null, 2) + '\n');
  const out = [...list, ['contenido.json', text]];
  if (key) {
    const sig = crypto.sign(null, text, key.priv).toString('hex');
    out.push(['firma.json', Buffer.from(JSON.stringify({ format: 1, alg: 'ed25519', key: key.public, sig }, null, 2) + '\n')]);
  }
  return out;
}

function pickKey() {
  if (bool('--unsigned')) return null;
  const file = flag('--key') || process.env.LPK_KEY || (fs.existsSync(DEFAULT_KEY) ? DEFAULT_KEY : null);
  return file ? loadKey(file) : null;
}

const write = (out, files) => fs.writeFileSync(out, zlib.gzipSync(tar(files), { level: 9 }));
const trustLine = (key) => (key ? `signed by «${key.name}» (${key.public.slice(0, 16)}…)` : 'unsigned');

async function pack() {
  const key = pickKey();
  const [dir, outArg] = argv;
  if (!dir) die('usage: node tools/lpk.mjs <app folder> [output.lpk] [--key key.json | --unsigned]');
  const { build } = await import('esbuild');
  const manifest = JSON.parse(fs.readFileSync(path.join(dir, 'manifest.json'), 'utf8'));
  if (!/^[a-z][a-z0-9-]{1,39}$/.test(manifest.id || '')) die('invalid manifest.id: a-z, 0-9 and -, 2 to 40 long');
  if (!/^[0-9][0-9A-Za-z.+~-]{0,31}$/.test(String(manifest.version || ''))) die('invalid manifest.version');
  if (!manifest.name) die('falta manifest.name');
  if (!manifest.icon || !/\.png$/.test(manifest.icon) || !fs.existsSync(path.join(dir, manifest.icon))) die('missing own icon: manifest.icon must point to a .png in the app (128×128 or larger recommended)');
  const KNOWN = ['files', 'clipboard', 'notifications', 'network', 'camera', 'microphone', 'screen', 'listen', 'usb', 'bluetooth', 'apps'];
  for (const p of manifest.permissions || []) if (!KNOWN.includes(p)) die(`unknown permission in manifest.permissions: ${p} (valid: ${KNOWN.join(', ')})`);

  // Shared modules are taken from the theme (or from the app's process) at run time.
  const sharedPlugin = {
    name: 'lordos-shared',
    setup(b) {
      b.onResolve({ filter: /^(react|react\/jsx-runtime|@lordos\/sdk|@lordos\/runtime)$/ }, (a) => ({ path: a.path, namespace: 'lordos-shared' }));
      b.onLoad({ filter: /.*/, namespace: 'lordos-shared' }, (a) => ({
        contents: `module.exports = globalThis.__lordos.require(${JSON.stringify(a.path)});`, loader: 'js',
      }));
    },
  };
  const result = await build({
    entryPoints: [path.join(dir, 'index.jsx')],
    bundle: true, write: false, format: 'iife', globalName: '__lordosApp', target: 'es2020', minify: true,
    jsx: 'automatic', plugins: [sharedPlugin], define: { 'process.env.NODE_ENV': '"production"' }, logLevel: 'warning',
  });
  const appJs = result.outputFiles[0].contents;
  const files = [['manifest.json', Buffer.from(JSON.stringify(manifest, null, 2) + '\n')], ['app.js', Buffer.from(appJs)]];
  files.push([manifest.icon, fs.readFileSync(path.join(dir, manifest.icon))]);
  const walk = (base, rel) => {
    for (const e of fs.readdirSync(path.join(base, rel), { withFileTypes: true })) {
      if (e.name.startsWith('.')) continue;
      const r = rel ? `${rel}/${e.name}` : e.name;
      if (e.isDirectory()) walk(base, r);
      else if (e.isFile()) files.push([r, fs.readFileSync(path.join(base, r))]);
    }
  };
  for (const sub of ['locales', 'assets']) if (fs.existsSync(path.join(dir, sub))) walk(dir, sub);
  for (const [p] of files) if (!/^([A-Za-z0-9_-][A-Za-z0-9._-]*\/)*[A-Za-z0-9_-][A-Za-z0-9._-]*$/.test(p)) die(`invalid path in the package: ${p}`);
  const out = outArg || `${manifest.id}-${manifest.version}.lpk`;
  const sealed = seal(files, manifest, key);
  write(out, sealed);
  const kb = (fs.statSync(out).size / 1024).toFixed(1);
  console.log(`${out}: ${sealed.length} files, ${kb} KB (app.js ${(appJs.length / 1024).toFixed(1)} KB), ${trustLine(key)}`);
}

function keygen() {
  const name = flag('--name') || os.userInfo().username;
  const out = flag('--out') || DEFAULT_KEY;
  if (fs.existsSync(out)) die(`${out} already exists (a key is never overwritten)`);
  const { privateKey: priv, publicKey: pub } = crypto.generateKeyPairSync('ed25519');
  const secret = priv.export({ format: 'der', type: 'pkcs8' }).subarray(16).toString('hex');
  const pubHex = pub.export({ format: 'der', type: 'spki' }).subarray(12).toString('hex');
  fs.mkdirSync(path.dirname(out), { recursive: true, mode: 0o700 });
  fs.writeFileSync(out, JSON.stringify({ format: 1, alg: 'ed25519', name, public: pubHex, secret }, null, 2) + '\n', { mode: 0o600, flag: 'wx' });
  console.log(`${out}: key of «${name}»\npublic: ${pubHex}\n(the private key is never shared or committed to the repository)`);
}

function sign() {
  const key = pickKey();
  const [inp, outArg] = argv.slice(1);
  if (!inp || !key) die('usage: node tools/lpk.mjs sign <package.lpk> --key key.json [output.lpk]');
  const files = untar(fs.readFileSync(inp));
  const m = files.find(([p]) => p === 'manifest.json');
  if (!m) die('not a LordOS package');
  const out = outArg || inp;
  write(out, seal(files, JSON.parse(m[1].toString('utf8')), key));
  console.log(`${out}: ${trustLine(key)}`);
}

function verify() {
  const files = untar(fs.readFileSync(argv[1] || die('usage: node tools/lpk.mjs verify <package.lpk>')));
  const get = (p) => files.find(([n]) => n === p)?.[1];
  const c = get('contenido.json'), s = get('firma.json');
  if (!c) { console.log('no contenido.json: «Unsigned» and no hashes'); return; }
  const contents = JSON.parse(c.toString('utf8'));
  let ok = true;
  for (const f of contents.files) {
    const d = get(f.path);
    if (!d || d.length !== f.size || sha256(d) !== f.sha256) { console.log(`ALTERED: ${f.path}`); ok = false; }
  }
  const extra = files.filter(([p]) => p !== 'contenido.json' && p !== 'firma.json' && !contents.files.some((f) => f.path === p));
  for (const [p] of extra) { console.log(`EXTRA: ${p}`); ok = false; }
  if (s) {
    const sig = JSON.parse(s.toString('utf8'));
    const pub = crypto.createPublicKey({ key: Buffer.concat([Buffer.from('302a300506032b6570032100', 'hex'), Buffer.from(sig.key, 'hex')]), format: 'der', type: 'spki' });
    const good = crypto.verify(null, c, pub, Buffer.from(sig.sig, 'hex'));
    console.log(`signature ${good ? 'valid' : 'INVALID'} from «${contents.signer?.name || '?'}», key ${sig.key}`);
    ok = ok && good;
  } else console.log('unsigned (hashes only)');
  console.log(ok ? 'the files match' : 'the package does NOT match its contenido.json');
  process.exit(ok ? 0 : 1);
}

function catalog() {
  const key = pickKey();
  const [src, outDir = '.'] = argv.slice(1);
  if (!src || !key) die('usage: node tools/lpk.mjs catalog <source.json> --key official.json [folder]');
  const cat = JSON.parse(fs.readFileSync(src, 'utf8'));
  cat.format = 1;
  cat.date = cat.date || new Date().toISOString().slice(0, 10);
  // size and hash of each package (if its file is given with "file")
  for (const p of cat.packages || []) {
    if (!p.file) continue;
    const b = fs.readFileSync(path.resolve(path.dirname(src), p.file));
    p.size = b.length;
    p.sha256 = sha256(b);
    delete p.file;
  }
  const text = Buffer.from(JSON.stringify(cat, null, 2) + '\n');
  const sig = crypto.sign(null, text, key.priv).toString('hex');
  fs.mkdirSync(outDir, { recursive: true });
  fs.writeFileSync(path.join(outDir, 'catalog.json'), text);
  fs.writeFileSync(path.join(outDir, 'catalog.sig'), JSON.stringify({ format: 1, alg: 'ed25519', key: key.public, sig }, null, 2) + '\n');
  console.log(`${outDir}/catalog.json (serial ${cat.serial}, ${(cat.packages || []).length} packages, ${(cat.authors || []).length} authors, ${(cat.revoked?.keys || []).length + (cat.revoked?.packages || []).length} revoked), signed by «${key.name}»`);
}

const cmd = argv[0];
if (cmd === 'keygen') keygen();
else if (cmd === 'sign') sign();
else if (cmd === 'verify') verify();
else if (cmd === 'catalog') catalog();
else await pack();
