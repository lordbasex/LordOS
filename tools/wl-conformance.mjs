#!/usr/bin/env node
// tools/wl-conformance.mjs — checks lord-server's Wayland compositor (server/src/wl/*) and its Xwayland integration
// point by point and gives each protocol, and each X11 area, a score from 0 to 100 % (docs/WAYLAND.md, "Conformance").
//
//   node tools/wl-conformance.mjs                 static + dynamic (a headless lord-server) + X11 + real clients
//   make -C server wl-conformance                 the same (builds lord-server and the theme first)
//   options: --static-only  --no-x11  --no-smoke  --only <global,...>  --json <file>  --record (docs/wayland-conformance.json)
//            --claims <file> (default docs/wayland-protocols.json)  --xml-dirs <dir:dir>  --allow <file>  --verbose
//
// Inputs: the protocol XML files (the build's wayland-protocols, /usr/share/wayland, vendored copies in the
// repository), the globals lord-server advertises (wl_global_create in the source, and what a client really sees),
// and the claims file when it exists. For each advertised global and every interface reachable from it, up to the
// advertised version:
//   static   every request has a handler that is not a stub (a NULL one makes libwayland abort lord-server), a
//            destructor really destroys, every event is sent somewhere, every protocol error is raised somewhere;
//   dynamic  build/wl-conformance-client (server/tests/wl_conformance.c) against a headless lord-server: binding
//            at each version and the destructor (generic, any protocol), then the protocol's own flows and errors;
//   X11      build/x11-conformance-client (server/tests/x11_conformance.c) against Xwayland on that server:
//            ICCCM, EWMH, selections (INCR both ways), plus patterns in xwm.c and the unit tests;
//   clients  real programs that are installed (weston-simple-*, foot, vkcube, GTK and Qt demos, ...), each pass or
//            fail with the globals it bound (reported, not scored).
// Weights: a request 3, an event 2, an error 1, a protocol-specific dynamic check 3, a generic bind/destroy 1,
// an X11 dynamic check 3, an X11 source pattern or unit test 1. Allowed exceptions (tools/wl-conformance/allow.json,
// each with its reason) count as passed and are listed. Skipped checks don't count, but an item with skips, without
// a protocol-specific flow or without its XML is never called complete.
// Never point it at a live session: it starts its own lord-server in a private runtime folder and home.
import { execFileSync, spawn, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const SERVER = path.join(ROOT, 'server');
const BUILD = path.join(SERVER, 'build');
const GEN = path.join(BUILD, 'gen', 'conf');

// ---------------------------------------------------------------- options
const argv = process.argv.slice(2);
const opt = (name, def) => { const i = argv.indexOf(name); return i >= 0 && i + 1 < argv.length ? argv[i + 1] : def; };
const flag = (name) => argv.includes(name);
const OPTS = {
  staticOnly: flag('--static-only'),
  x11: !flag('--no-x11'),
  smoke: !flag('--no-smoke'),
  only: opt('--only', '') ? opt('--only', '').split(',') : null,
  json: opt('--json', path.join(BUILD, 'wl-conformance.json')),
  record: flag('--record'),
  claims: opt('--claims', path.join(ROOT, 'docs', 'wayland-protocols.json')),
  allow: opt('--allow', path.join(ROOT, 'tools', 'wl-conformance', 'allow.json')),
  xwaylandSpec: path.join(ROOT, 'tools', 'wl-conformance', 'xwayland.json'),
  xmlDirs: (opt('--xml-dirs', '') || '').split(':').filter(Boolean),
  verbose: flag('--verbose'),
  date: opt('--date', new Date().toISOString().slice(0, 10)),
};
const W = { request: 3, event: 2, error: 1, flow: 3, generic: 1, claim: 2, x11dyn: 3, x11static: 1, unit: 1 };
const log = (...a) => { if (OPTS.verbose) console.error(...a); };

// ---------------------------------------------------------------- protocol XML
function parseXml(text, file) {
  // a self-closing <description summary="…"/> goes first: the lazy match of a full one would otherwise run from it to the
  // next </description> and swallow the requests, events and error entries between them (color-management-v1 has 7)
  text = text.replace(/<!--[\s\S]*?-->/g, '').replace(/<description[^>]*\/>/g, '').replace(/<description[\s\S]*?<\/description>/g, '');
  const attrs = (s) => { const o = {}; for (const m of s.matchAll(/([\w-]+)="([^"]*)"/g)) o[m[1]] = m[2]; return o; };
  const proto = { name: '', file, interfaces: [] };
  let iface = null, msg = null, en = null;
  for (const m of text.matchAll(/<(\/?)([\w-]+)([^>]*?)(\/?)>/g)) {
    const [, close, tag, rest, selfClose] = m;
    const a = attrs(rest);
    if (close) {
      if (tag === 'interface') iface = null;
      else if (tag === 'request' || tag === 'event') msg = null;
      else if (tag === 'enum') en = null;
      continue;
    }
    if (tag === 'protocol') proto.name = a.name;
    else if (tag === 'interface') { iface = { name: a.name, version: +a.version || 1, requests: [], events: [], enums: [], protocol: proto }; proto.interfaces.push(iface); }
    else if ((tag === 'request' || tag === 'event') && iface) {
      msg = { name: a.name, type: a.type || '', since: +a.since || 1, deprecatedSince: +a['deprecated-since'] || 0, args: [] };
      (tag === 'request' ? iface.requests : iface.events).push(msg);
      if (selfClose) msg = null;
    } else if (tag === 'arg' && msg) msg.args.push({ name: a.name, type: a.type, iface: a.interface || '' });
    else if (tag === 'enum' && iface) { en = { name: a.name, entries: [] }; iface.enums.push(en); if (selfClose) en = null; }
    else if (tag === 'entry' && en) en.entries.push({ name: a.name, value: a.value, since: +a.since || 1 });
  }
  return proto;
}

function walk(dir, pred, out = []) {
  let ents = [];
  try { ents = fs.readdirSync(dir, { withFileTypes: true }); } catch { return out; }
  for (const e of ents) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) { if (!['node_modules', '.git', 'build', 'dist'].includes(e.name)) walk(p, pred, out); }
    else if (pred(p)) out.push(p);
  }
  return out;
}

// the build's protocol list (server/Makefile and server/mk/*.mk): files named there win over any other copy
function buildProtocolNames() {
  const names = new Set();
  for (const f of [path.join(SERVER, 'Makefile'), ...walk(path.join(SERVER, 'mk'), (p) => p.endsWith('.mk'))]) {
    const t = fs.readFileSync(f, 'utf8');
    for (const m of t.matchAll(/(?:stable|staging|unstable|experimental)\/[\w-]+\/([\w.-]+?)(?:\.xml)?\b/g)) names.add(m[1].replace(/\.xml$/, ''));
    for (const m of t.matchAll(/([\w-]+)\.xml/g)) names.add(m[1]);
  }
  return names;
}

function loadProtocols() {
  const dirs = [...OPTS.xmlDirs, '/usr/share/wayland', '/usr/share/wayland-protocols', '/usr/share/plasma-wayland-protocols'];
  const vendored = walk(ROOT, (p) => p.endsWith('.xml') && !p.includes(`${path.sep}web${path.sep}`) && !p.includes('node_modules'))
    .filter((p) => { try { return /<protocol\s+name=/.test(fs.readFileSync(p, 'utf8').slice(0, 4000)); } catch { return false; } });
  const files = [...vendored, ...dirs.flatMap((d) => walk(d, (p) => p.endsWith('.xml')))];
  const inBuild = buildProtocolNames();
  const rank = (f) => {
    const base = path.basename(f, '.xml');
    let r = 0;
    if (inBuild.has(base)) r += 100;
    if (f.startsWith(ROOT)) r += 50;
    if (f.includes('/stable/')) r += 3; else if (f.includes('/staging/')) r += 2; else if (f.includes('/unstable/')) r += 1;
    if (f.endsWith('/wayland.xml')) r += 200;
    return r;
  };
  const byIface = new Map();
  const protos = [];
  for (const f of files.sort((a, b) => rank(b) - rank(a))) {
    let p;
    try { p = parseXml(fs.readFileSync(f, 'utf8'), f); } catch { continue; }
    if (!p.interfaces.length) continue;
    p.rank = rank(f);
    p.base = path.basename(f, '.xml');
    protos.push(p);
    for (const i of p.interfaces) if (!byIface.has(i.name)) byIface.set(i.name, i);
  }
  return { byIface, protos };
}

// ---------------------------------------------------------------- the C source
function stripComments(t) {
  // comments become spaces (newlines kept, so offsets and line numbers stay right); strings are left alone
  let out = '', i = 0;
  while (i < t.length) {
    const c = t[i], n = t[i + 1];
    if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < t.length && t[j] !== c) { if (t[j] === '\\') j++; j++; }
      out += t.slice(i, j + 1); i = j + 1;
    } else if (c === '/' && n === '*') {
      const j = t.indexOf('*/', i + 2); const end = j < 0 ? t.length : j + 2;
      out += t.slice(i, end).replace(/[^\n]/g, ' '); i = end;
    } else if (c === '/' && n === '/') {
      const j = t.indexOf('\n', i); const end = j < 0 ? t.length : j;
      out += ' '.repeat(end - i); i = end;
    } else { out += c; i++; }
  }
  return out;
}
function loadSources() {
  const files = walk(path.join(SERVER, 'src'), (p) => /\.[ch]$/.test(p)).map((f) => {
    const raw = fs.readFileSync(f, 'utf8');
    const code = stripComments(raw);
    const nl = []; for (let i = 0; i < raw.length; i++) if (raw[i] === '\n') nl.push(i);
    return { file: path.relative(ROOT, f), raw, code, nl };
  });
  const lineOf = (F, off) => { let lo = 0, hi = F.nl.length; while (lo < hi) { const m = (lo + hi) >> 1; if (F.nl[m] < off) lo = m + 1; else hi = m; } return lo + 1; };
  const where = (F, off) => `${F.file}:${lineOf(F, off)}`;
  // implementation tables: static const struct <iface>_interface <name> = { ... };
  const impls = new Map();
  for (const F of files) for (const m of F.code.matchAll(/\bconst\s+struct\s+(\w+)_interface\s+(\w+)\s*=\s*\{([\s\S]*?)\}\s*;/g)) {
    const iface = m[1];
    if (iface === 'wl') continue;                   // a hand-written struct wl_interface (the KDE decoration)
    const body = m[3];
    const fields = {}, positional = [];
    if (/\.\w+\s*=/.test(body)) for (const f of body.matchAll(/\.(\w+)\s*=\s*([\w&]+)/g)) fields[f[1]] = f[2];
    else for (const v of body.split(',').map((s) => s.trim()).filter(Boolean)) positional.push(v);
    if (!impls.has(iface)) impls.set(iface, []);
    impls.get(iface).push({ name: m[2], fields, positional, where: where(F, m.index) });
  }
  // function definitions (handlers return void)
  const fns = new Map();
  for (const F of files) for (const m of F.code.matchAll(/\b(?:static\s+)?(?:inline\s+)?void\s+(\w+)\s*\(([^;{)]*)\)\s*\{/g)) {
    const start = m.index + m[0].length - 1;
    let depth = 0, j = start;
    for (; j < F.code.length; j++) { if (F.code[j] === '{') depth++; else if (F.code[j] === '}' && --depth === 0) break; }
    const body = F.code.slice(start + 1, j), rawBody = F.raw.slice(start + 1, j);
    if (!fns.has(m[1])) fns.set(m[1], { body, rawBody, where: where(F, m.index) });
  }
  const all = files.map((F) => F.code).join('\n');
  const find = (re) => { for (const F of files) { const m = F.code.match(re); if (m) return where(F, m.index); } return null; };
  // a pattern in one file, or in any file under a directory ("server/src/"), comments excluded
  const findRaw = (file, re) => {
    for (const F of files.filter((x) => (file.endsWith('/') ? x.file.startsWith(file) : x.file === file))) { const m = F.code.match(re); if (m) return where(F, m.index); }
    return null;
  };
  return { files, impls, fns, all, find, findRaw, where };
}

// advertised globals from the source: wl_global_create(dpy, &<iface>_interface, <version>, ...)
function staticGlobals(src, P) {
  const out = [];
  for (const F of src.files) for (const m of F.code.matchAll(/wl_global_create\s*\(\s*[^,]+,\s*&(\w+)\s*,\s*([^,]+),/g)) {
    let iface = m[1].replace(/_interface$/, '');
    if (!m[1].endsWith('_interface') || /kde_mgr/.test(m[1])) {
      const d = F.code.match(new RegExp(`struct\\s+wl_interface\\s+${m[1]}\\s*=\\s*\\{\\s*"([\\w]+)"`));
      if (d) iface = d[1];
    }
    const v = m[2].trim();
    let version = /^\d+$/.test(v) ? +v : null;
    if (version === null) { const d = F.code.match(new RegExp(`#define\\s+${v}\\s+(\\d+)`)); if (d) version = +d[1]; }
    out.push({ iface, version, where: src.where(F, m.index) });
  }
  if (/wl_display_init_shm\s*\(/.test(src.all)) out.push({ iface: 'wl_shm', version: null, where: src.find(/wl_display_init_shm\s*\(/), libwayland: true });
  for (const g of out) if (g.version === null && g.libwayland) g.version = P.byIface.get(g.iface)?.version ?? null;   // what libwayland-server's own XML says
  return out;
}

// what the claims file says (docs/wayland-protocols.json, written by hand; any shape with interface + version)
function readClaims(file) {
  let j;
  try { j = JSON.parse(fs.readFileSync(file, 'utf8')); } catch { return null; }
  const out = new Map();
  const visit = (o) => {
    if (Array.isArray(o)) { o.forEach(visit); return; }
    if (!o || typeof o !== 'object') return;
    const name = [o.interface, o.global, o.name].find((x) => typeof x === 'string' && /^[a-z][a-z0-9_]+$/.test(x));
    const ver = [o.version, o.advertised, o.versionAdvertised].find((x) => Number.isInteger(x));
    if (name && ver !== undefined) out.set(name, { version: ver, status: o.status || o.state || '' });
    for (const v of Object.values(o)) if (v && typeof v === 'object') visit(v);
  };
  visit(j);
  return out;
}

// ---------------------------------------------------------------- static analysis
const LIBWAYLAND = new Set(['wl_shm', 'wl_shm_pool', 'wl_display', 'wl_registry']);
const up = (s) => s.toUpperCase();

function reachable(P, root, version) {
  const seen = new Map();
  const todo = [[root, version]];
  while (todo.length) {
    const [name, v] = todo.pop();
    const I = P.byIface.get(name);
    if (!I || seen.has(name) || ['wl_display', 'wl_registry'].includes(name)) continue;
    const ver = Math.min(v, I.version);
    seen.set(name, ver);
    for (const m of [...I.requests, ...I.events]) if (m.since <= ver) for (const a of m.args) if (a.type === 'new_id' && a.iface) todo.push([a.iface, ver]);
  }
  return seen;
}

function stubKind(fn, src) {
  const raw = fn.rawBody;
  if (/not\s+(yet\s+)?implemented|unimplemented|\bTODO\b|\bFIXME\b|\bstub\b/i.test(raw)) return 'marked not implemented';
  const b = fn.body.replace(/\(\s*void\s*\)\s*\(?\s*\w+\s*\)?\s*;/g, '').replace(/\breturn\s*;/g, '').replace(/\s+/g, '');
  if (!b) return 'empty (only (void) casts)';
  return '';
}
function destroys(fn, src, depth = 0) {
  if (/wl_resource_destroy\s*\(/.test(fn.body)) return true;
  if (depth > 1) return false;
  for (const m of fn.body.matchAll(/\b(\w+)\s*\(/g)) { const g = src.fns.get(m[1]); if (g && g !== fn && destroys(g, src, depth + 1)) return true; }
  return false;
}

function staticChecks(P, src, allow, global, version) {
  const checks = [];
  const add = (c) => checks.push({ kind: 'static', ...c });
  const ifaces = reachable(P, global, version);
  for (const [name, ver] of ifaces) {
    const I = P.byIface.get(name);
    const lib = LIBWAYLAND.has(name);
    const impls = src.impls.get(name) || [];
    for (const r of I.requests) {
      if (r.since > ver) continue;
      const id = `${name}.${r.name}`;
      const base = { id: `request.${id}`, what: `request ${id}`, weight: W.request, hint: id };
      if (lib) { add({ ...base, result: 'pass', msg: 'implemented by libwayland-server', where: null }); continue; }
      if (allow.requests?.[id]) { add({ ...base, result: 'allowed', msg: allow.requests[id], where: impls[0]?.where || null }); continue; }
      if (!impls.length) { add({ ...base, result: 'fail', msg: `no ${name}_interface implementation table in server/src` }); continue; }
      const idx = I.requests.indexOf(r);
      const problems = [], excused = [];
      let at = impls[0].where;
      for (const im of impls) {
        if (allow.requests?.[`${id}@${im.name}`]) { excused.push(`${im.name}: ${allow.requests[`${id}@${im.name}`]}`); continue; }
        const fnName = im.fields[r.name] ?? im.positional[idx];
        if (!fnName || fnName === 'NULL' || fnName === '0') { problems.push(`${im.name} (${im.where}) has no handler: a client sending it makes libwayland abort lord-server`); continue; }
        const fn = src.fns.get(fnName.replace(/^&/, ''));
        if (!fn) continue;                         // defined somewhere we can't parse (a macro): counted as present
        at = fn.where;
        const stub = stubKind(fn, src);
        if (stub && r.type !== 'destructor') problems.push(`${fnName} (${fn.where}) is a stub: ${stub}`);
        if (r.type === 'destructor' && !destroys(fn, src)) problems.push(`${fnName} (${fn.where}) is a destructor that never calls wl_resource_destroy`);
      }
      const result = problems.length ? 'fail' : excused.length ? 'allowed' : 'pass';
      add({ ...base, result, msg: problems.join('; ') || (excused.length ? `handled; allowed: ${excused.join('; ')}` : `handled (${impls.map((i) => i.name).join(', ')})`), where: at });
    }
    for (const e of I.events) {
      if (e.since > ver) continue;
      const id = `${name}.${e.name}`;
      const base = { id: `event.${id}`, what: `event ${id}`, weight: W.event, hint: id };
      if (lib) { add({ ...base, result: 'pass', msg: 'sent by libwayland-server', where: null }); continue; }
      if (allow.events?.[id]) { add({ ...base, result: 'allowed', msg: allow.events[id], where: null }); continue; }
      // called, posted by opcode, or handed to a helper that calls it (wlcolor.c's info_prim(i, …, <iface>_send_primaries))
      const at = src.find(new RegExp(`\\b${name}_send_${e.name}\\s*\\(`)) || src.find(new RegExp(`(?:post|queue)_event\\s*\\([^;]*\\b${up(name)}_${up(e.name)}\\b`)) ||
        src.find(new RegExp(`[(,]\\s*${name}_send_${e.name}\\s*[,)]`));
      add({ ...base, result: at ? 'pass' : 'fail', msg: at ? 'sent' : `${name}_send_${e.name}() is never called`, where: at });
    }
    const errEnum = I.enums.find((x) => x.name === 'error');
    for (const en of errEnum?.entries || []) {
      if (en.since > ver) continue;
      const id = `${name}.error.${en.name}`;
      const base = { id: `error.${id}`, what: `error ${id}`, weight: W.error, hint: id };
      if (lib) { add({ ...base, result: 'pass', msg: 'raised by libwayland-server', where: null }); continue; }
      if (allow.errors?.[id]) { add({ ...base, result: 'allowed', msg: allow.errors[id], where: null }); continue; }
      const macro = `${up(name)}_ERROR_${up(en.name)}`;
      const at = src.find(new RegExp(`\\b${macro}\\b`));
      add({ ...base, result: at ? 'pass' : 'fail', msg: at ? 'raised' : `${macro} is never raised`, where: at });
    }
  }
  return { checks, ifaces: [...ifaces.keys()] };
}

// ---------------------------------------------------------------- the headless server
function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }

const THEME_HOOK = `
;(() => {
  const orig = globalThis.__lordWayland;
  const titles = {};
  let leaseAsks = 0;
  globalThis.__lordWayland = (id, type, d) => {
    try {
      if (type === 'map' || type === 'title') titles[id] = d && d.title;
      const t = titles[id] || '';
      if ((String(t).startsWith('wlconf') || type === 'map' || type === 'unmap' || type === 'title') && type !== 'bell') {
        let j = ''; try { j = JSON.stringify(d); } catch (e) { j = '?'; }
        console.log('WLCONF ' + JSON.stringify({ id: String(id), type, title: t, d: j.length > 600 ? j.slice(0, 600) : j }));
      }
      if (type === 'map' && String(d && d.title).startsWith('wlconf-close')) setTimeout(() => { try { lord.wayland.close(id); } catch (e) { console.log('WLCONF close-failed ' + e); } }, 800);
      // permission prompts (idle notifications, shortcuts inhibitors) are answered here, without the theme's
      // dialog: an open prompt holds the keyboard and later checks need it
      if (type === 'idlenotify') { setTimeout(() => { try { lord.wayland.idleAnswer(d.app, true); } catch (e) {} }, 100); return undefined; }
      if (type === 'shortcuts') { setTimeout(() => { try { lord.wayland.shortcutsAnswer(d.app, true); } catch (e) {} }, 100); return undefined; }
      // drm-lease-v1's "vr" prompt (the test hooks' fake headset): the first is answered "allow once", the next ones
      // "don't allow" (saved, so a later request is refused with no prompt)
      if (type === 'drmlease') {
        leaseAsks++;
        const how = leaseAsks === 1 ? 'once' : 'deny';
        console.log('WLCONF ' + JSON.stringify({ id: '0', type: 'leasePrompt', title: '', d: JSON.stringify({ app: d && d.app, headset: d && d.headset, answer: how }) }));
        setTimeout(() => { try { lord.wayland.leaseAnswer(d.app, how); } catch (e) {} }, 100);
        return undefined;
      }
      if (type === 'bell') console.log('WLCONF ' + JSON.stringify({ id: String(id), type, title: titles[id] || '', d: '{}' }));
      // xdg-toplevel-tag: what the shell reads with the window when it maps
      if (type === 'map' && String(d && d.title).startsWith('wlconf')) {
        try { const tg = lord.wayland.tag(id); if (tg) console.log('WLCONF ' + JSON.stringify({ id: String(id), type: 'tagAtMap', title: d.title, d: JSON.stringify(tg) })); } catch (e) {}
      }
    } catch (e) {}
    return orig(id, type, d);
  };
})();
`;

async function startServer() {
  const theme = path.join(ROOT, 'themes', 'default', 'theme.bundle.js');
  const bin = path.join(BUILD, 'lord-server');
  if (!fs.existsSync(bin) || !fs.existsSync(theme)) throw new Error(`needs ${path.relative(ROOT, bin)} and ${path.relative(ROOT, theme)} (make -C server; npm run build)`);
  const D = fs.mkdtempSync(path.join(os.tmpdir(), 'lwconf.'));
  fs.chmodSync(D, 0o700);
  fs.mkdirSync(path.join(D, 'home', '.config', 'lordos'), { recursive: true });
  fs.writeFileSync(path.join(D, 'home', '.config', 'lordos', 'settings.json'), '{"schema": 9, "desktopChosen": true, "language": "en"}\n');
  fs.writeFileSync(path.join(D, 'theme.js'), fs.readFileSync(theme, 'utf8') + THEME_HOOK);
  const env = { ...process.env, XDG_RUNTIME_DIR: D, HOME: path.join(D, 'home'), LORD_GPU: '0', LANG: 'en_US.UTF-8', LORD_HEADLESS_DRAW: '1' };
  for (const k of ['WAYLAND_DISPLAY', 'WAYLAND_SOCKET', 'DISPLAY', 'XAUTHORITY', 'LORD_SESSIOND', 'LORDOS_SESSIOND', 'DBUS_SESSION_BUS_ADDRESS']) delete env[k];
  const out = fs.openSync(path.join(D, 'server.log'), 'w');
  // --user: the session's services, among them the X socket (Xwayland starts on the first X11 connection)
  // --test-hooks: the pointer, a graphics tablet, a VR headset and an Xwayland-like connection on demand (src/testhooks.h)
  const P = spawn(bin, ['--headless', '1280x800', '--theme', path.join(D, 'theme.js'), '--timeout', '900', '--user', os.userInfo().username, '--test-hooks'],
    { cwd: SERVER, env, stdio: ['ignore', out, out], detached: true });
  let sock = '';
  for (let i = 0; i < 120 && !sock; i++) { await sleep(250); sock = fs.readdirSync(D).find((n) => /^wayland-\d+$/.test(n)) || ''; }
  if (!sock) { try { process.kill(-P.pid, 'SIGTERM'); } catch {} throw new Error(`lord-server made no Wayland socket (log: ${D}/server.log)`); }
  await sleep(2500);                               // the theme is up
  const logs = () => ['server.log', 'home/.cache/lordos/shell.log', 'home/.cache/lordos/compositor.log'].map((f) => { try { return fs.readFileSync(path.join(D, f), 'utf8'); } catch { return ''; } }).join('\n');
  const display = () => { const m = logs().match(/xwayland: DISPLAY=(:\d+)/); return m ? m[1] : ''; };
  const clientEnv = { ...env, WAYLAND_DISPLAY: sock };
  // SIGTERM, then SIGKILL after 3 s: a lord-server stuck in a loop ignores SIGTERM (its group: Xwayland too)
  const stop = () => {
    try { process.kill(-P.pid, 'SIGTERM'); } catch { return; }
    const wait = new Int32Array(new SharedArrayBuffer(4));
    for (let i = 0; i < 30; i++) { try { process.kill(P.pid, 0); } catch { return; } Atomics.wait(wait, 0, 0, 100); }
    try { process.kill(-P.pid, 'SIGKILL'); } catch {}
  };
  return { D, sock, env: clientEnv, logs, display, stop, alive: () => { try { process.kill(P.pid, 0); return true; } catch { return false; } }, pid: P.pid };
}

function themeEvents(S) {
  const out = [];
  for (const line of S.logs().split('\n')) {
    const i = line.indexOf('WLCONF {');
    if (i < 0) continue;
    try { const e = JSON.parse(line.slice(i + 7)); e.d = e.d && e.d !== '?' ? JSON.parse(e.d) : e.d; out.push(e); } catch {}
  }
  return out;
}

// What the theme got from the flows' windows (the THEME_HOOK's log): the protocols whose effect is on the shell's side.
// Each becomes a flow check of the global it is about (hint: the request that made it).
function themeFlowChecks(ev) {
  const out = [];
  const add = (item, id, ok, hint, msg) => out.push({ item, id, result: ok ? 'pass' : 'fail', hint, msg, kind: 'flow' });
  const of = (type, title) => ev.filter((e) => e.type === type && (title === undefined || e.title === title));
  const bell = of('bell', 'wlconf-bell');
  add('xdg_system_bell_v1', 'bell.reaches_theme', bell.length >= 1, 'xdg_system_bell_v1.ring', `the shell was told "bell" on the ringing window ${bell.length} time(s)`);
  add('xdg_system_bell_v1', 'bell.rate_limited', bell.length >= 1 && bell.length <= 6, 'xdg_system_bell_v1.ring', `31 rings on the window in a second reach the shell ${bell.length} time(s), not 31 (bellgate.h)`);
  const blur = of('blur', 'wlconf-blur').map((e) => e.d && e.d.rects);
  const two = blur.some((r) => Array.isArray(r) && r.length === 2);
  add('ext_background_effect_manager_v1', 'blur.reaches_theme', two, 'ext_background_effect_surface_v1.set_blur_region', `the shell got the two rectangles to blur behind the window (${JSON.stringify(blur)})`);
  const last = blur[blur.length - 1];
  add('ext_background_effect_manager_v1', 'blur.removed_reaches_theme', two && Array.isArray(last) && last.length === 0, 'ext_background_effect_surface_v1.set_blur_region',
    `a null region, then the object destroyed: the shell's last rectangles are none (${JSON.stringify(last)})`);
  const tm = of('tagAtMap', 'wlconf-tag')[0];
  add('xdg_toplevel_tag_manager_v1', 'tag.at_map', !!tm && tm.d.tag === 'conformance window' && tm.d.description === 'Conformance window', 'xdg_toplevel_tag_manager_v1.set_toplevel_tag',
    `set before the first frame, the shell reads them with the window: ${tm ? JSON.stringify(tm.d) : 'nothing'}`);
  const tg = of('tag', 'wlconf-tag').map((e) => e.d);
  const changed = tg.filter((d) => d && d.tag === 'conformance window, changed').pop();
  add('xdg_toplevel_tag_manager_v1', 'tag.change_reaches_theme', !!changed && !/[\u0000-\u001f]/.test(changed.description || '') && /^Ventana ñ/.test(changed.description || ''),
    'xdg_toplevel_tag_manager_v1.set_toplevel_description', `changed while mapped: a "tag" event, the control character out (${changed ? JSON.stringify(changed) : 'none'})`);
  const long = tg.find((d) => d && /^x+$/.test(d.tag || ''));
  add('xdg_toplevel_tag_manager_v1', 'tag.cut', !!long && long.tag.length === 127, 'xdg_toplevel_tag_manager_v1.set_toplevel_tag', `a 399-byte tag reaches the shell cut to ${long ? long.tag.length : '?'} bytes (127)`);
  const td = of('tldrag', 'wlconf-tldrag').map((e) => e.d);
  const on = td.find((d) => d && d.on === true);
  add('xdg_toplevel_drag_manager_v1', 'tldrag.reaches_theme', !!on && on.ox === 20 && on.oy === 10, 'xdg_toplevel_drag_v1.attach',
    `the attached window follows the pointer: the shell was told ${on ? JSON.stringify(on) : 'nothing'} (offset 20, 10)`);
  add('xdg_toplevel_drag_manager_v1', 'tldrag.ends_reaches_theme', td.some((d) => d && d.on === false), 'xdg_toplevel_drag_v1.attach', `when the drag ends the shell is told it no longer follows (${JSON.stringify(td)})`);
  const lp = ev.filter((e) => e.type === 'leasePrompt').map((e) => e.d);
  add('wp_drm_lease_device_v1', 'lease.prompt', lp.length === 2 && lp.every((d) => /^program:\//.test(d.app || '') && /LordOS test headset/.test(d.headset || '')), 'wp_drm_lease_request_v1.submit',
    `the system's "vr" prompt, keyed by the program lord-server checked, naming the headset: ${lp.length} prompt(s) (${lp.map((d) => `${d.answer}: ${d.headset}`).join('; ')}); none after "don't allow" was saved`);
  return out;
}

// ---------------------------------------------------------------- building the test clients
function sh(cmd, args, o = {}) { return spawnSync(cmd, args, { encoding: 'utf8', maxBuffer: 64 << 20, ...o }); }

function buildWlClient(P, wantedIfaces) {
  fs.mkdirSync(GEN, { recursive: true });
  // the protocols with a flow in wl_conformance.c (always generated when the XML is there) and the advertised ones
  const flowBases = ['xdg-shell', 'xdg-decoration-unstable-v1', 'viewporter', 'linux-dmabuf-v1', 'presentation-time', 'fractional-scale-v1',
    'xdg-output-unstable-v1', 'primary-selection-unstable-v1', 'xdg-foreign-unstable-v2', 'xdg-dialog-v1', 'xdg-toplevel-icon-v1', 'ext-idle-notify-v1',
    'single-pixel-buffer-v1', 'alpha-modifier-v1', 'relative-pointer-unstable-v1', 'pointer-constraints-unstable-v1', 'cursor-shape-v1',
    'idle-inhibit-unstable-v1', 'keyboard-shortcuts-inhibit-unstable-v1', 'xdg-activation-v1', 'pointer-gestures-unstable-v1', 'text-input-unstable-v3',
    'security-context-v1', 'wl-fixes', 'ext-background-effect-v1', 'fifo-v1', 'commit-timing-v1', 'tearing-control-v1', 'content-type-v1',
    'pointer-warp-v1', 'xdg-system-bell-v1', 'xdg-toplevel-drag-v1', 'xdg-toplevel-tag-v1', 'xdg-session-management-v1', 'tablet-v2', 'color-management-v1',
    'color-representation-v1', 'linux-drm-syncobj-v1', 'drm-lease-v1', 'xwayland-shell-v1', 'server-decoration'];
  const chosen = new Map();                         // base → proto (the best ranked copy of each file name)
  const pick = (p) => { if (p && p.name !== 'wayland' && !chosen.has(p.base)) chosen.set(p.base, p); };
  for (const b of flowBases) pick(P.protos.find((p) => p.base === b));
  for (const i of wantedIfaces) pick(P.byIface.get(i)?.protocol);
  // interfaces they refer to (cursor-shape needs tablet's tool, ...): the protocols defining them, until closed
  for (let changed = true; changed;) {
    changed = false;
    const defined = new Set([...chosen.values()].flatMap((p) => p.interfaces.map((i) => i.name)));
    for (const p of [...chosen.values()]) for (const i of p.interfaces) for (const m of [...i.requests, ...i.events]) for (const a of m.args) {
      if (!a.iface || defined.has(a.iface) || P.byIface.get(a.iface)?.protocol.name === 'wayland') continue;
      const dep = P.byIface.get(a.iface)?.protocol;
      if (dep && !chosen.has(dep.base)) { chosen.set(dep.base, dep); changed = true; }
    }
  }
  // one definition per interface: drop a file whose interfaces another chosen file already defines
  const owner = new Map();
  for (const p of [...chosen.values()].sort((a, b) => b.rank - a.rank)) {
    if (p.interfaces.some((i) => owner.has(i.name))) { chosen.delete(p.base); continue; }
    for (const i of p.interfaces) owner.set(i.name, p.base);
  }
  const cfiles = [];
  let have = '/* generated by tools/wl-conformance.mjs */\n';
  for (const [base, p] of chosen) {
    const h = path.join(GEN, `${base}-client-protocol.h`), c = path.join(GEN, `${base}-protocol.c`);
    execFileSync('wayland-scanner', ['client-header', p.file, h]);
    execFileSync('wayland-scanner', ['private-code', p.file, c]);
    cfiles.push(c);
    have += `#define CONF_HAVE_${up(base).replace(/[^A-Z0-9]/g, '_')} 1\n`;
  }
  fs.writeFileSync(path.join(GEN, 'conf_have.h'), have);
  const core = P.protos.find((p) => p.name === 'wayland');
  const table = [...(core?.interfaces || []), ...[...chosen.values()].flatMap((p) => p.interfaces)].map((i) => i.name);
  fs.writeFileSync(path.join(GEN, 'conf_table.c'), '/* generated by tools/wl-conformance.mjs */\n#include <wayland-client.h>\n' +
    table.map((n) => `extern const struct wl_interface ${n}_interface;`).join('\n') +
    '\nstruct conf_iface { const char *name; const struct wl_interface *iface; };\nconst struct conf_iface conf_ifaces[] = {\n' +
    table.map((n) => `    { "${n}", &${n}_interface },`).join('\n') + '\n    { 0, 0 },\n};\n');
  const bin = path.join(BUILD, 'wl-conformance-client');
  const r = sh('cc', ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-I', GEN, '-o', bin, path.join(SERVER, 'tests', 'wl_conformance.c'), path.join(GEN, 'conf_table.c'), ...cfiles, '-lwayland-client']);
  if (r.status !== 0) throw new Error(`building wl-conformance-client:\n${r.stderr}`);
  if (r.stderr.trim()) console.error(r.stderr.trim());
  return { bin, protocols: [...chosen.keys()] };
}

function buildX11Client() {
  const bin = path.join(BUILD, 'x11-conformance-client');
  const r = sh('cc', ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-o', bin, path.join(SERVER, 'tests', 'x11_conformance.c'), '-lxcb']);
  if (r.status !== 0) return { error: r.stderr.split('\n').slice(0, 3).join(' ') };
  if (r.stderr.trim()) console.error(r.stderr.trim());
  return { bin };
}

function parseChecks(text) {
  const out = [];
  for (const line of (text || '').split('\n')) {
    const m = line.match(/^CHECK (\S+) (\S+) (PASS|FAIL|SKIP) (\S+) ?(.*)$/);
    if (m) out.push({ item: m[1], id: m[2], result: m[3].toLowerCase(), hint: m[4], msg: m[5] });
  }
  return out;
}

function runClient(bin, args, env, timeout = 120000) {
  const r = sh(bin, args, { env, timeout, cwd: SERVER });
  if (r.error || (r.status !== 0 && r.status !== 1)) log(`${path.basename(bin)} ${args.join(' ')}: status ${r.status} ${r.error || ''}\n${r.stderr}`);
  return { out: r.stdout || '', err: r.stderr || '', status: r.status, signal: r.signal };
}

function runBg(bin, args, env) {
  const P = spawn(bin, args, { env, cwd: SERVER, stdio: ['ignore', 'pipe', 'pipe'] });
  let out = '';
  P.stdout.on('data', (d) => { out += d; });
  P.stderr.on('data', (d) => { log(String(d)); });
  const done = new Promise((r) => P.on('exit', (code) => r(code)));
  return { P, out: () => out, done, waitFor: async (re, ms) => { const end = Date.now() + ms; while (Date.now() < end) { const m = out.match(re); if (m) return m; await sleep(50); } return null; } };
}

// ---------------------------------------------------------------- real clients (reported, not scored)
const SMOKE = [
  { name: 'wayland-info', cmd: ['wayland-info'], exits: true },
  { name: 'weston-simple-shm', cmd: ['weston-simple-shm'] },
  { name: 'weston-simple-egl', cmd: ['weston-simple-egl'] },
  { name: 'weston-simple-dmabuf-feedback', cmd: ['weston-simple-dmabuf-feedback'] },
  { name: 'foot', cmd: ['foot', '--title', 'wlconf-foot', 'sh', '-c', 'sleep 30'] },
  { name: 'vkcube', cmd: ['vkcube', '--wsi', 'wayland'] },
  { name: 'gtk3-demo', cmd: ['gtk3-demo'] },
  { name: 'gtk4-demo', cmd: ['gtk4-demo'] },
  { name: 'qt6 (qml)', cmd: ['qml6', '-'], skip: true },
  { name: 'firefox', cmd: ['firefox', '--new-instance', '--headless'], skip: true },
  { name: 'xterm (X11)', cmd: ['xterm', '-T', 'wlconf-xterm', '-e', 'sleep 30'], x11: true },
];
const which = (c) => { const r = sh('sh', ['-c', `command -v ${c}`]); return r.status === 0 ? r.stdout.trim() : ''; };

async function smoke(S) {
  const res = [];
  for (const t of SMOKE) {
    if (t.skip) continue;
    const exe = which(t.cmd[0]);
    if (!exe) { res.push({ client: t.name, result: 'skip', msg: 'not installed' }); continue; }
    const env = { ...S.env, WAYLAND_DEBUG: '1', HOME: path.join(S.D, 'home'), XDG_CONFIG_HOME: path.join(S.D, 'home', '.config') };
    if (t.x11) { env.DISPLAY = S.display(); env.XAUTHORITY = path.join(S.D, `lord-xauth.${S.display().slice(1)}`); }
    else delete env.DISPLAY;
    const before = themeEvents(S).length;
    const P = spawn(exe, t.cmd.slice(1), { env, cwd: path.join(S.D, 'home'), stdio: ['ignore', 'pipe', 'pipe'], detached: true });
    let err = '';
    P.stderr.on('data', (d) => { if (err.length < (4 << 20)) err += d; });
    P.stdout.on('data', () => {});
    let code = null;
    P.on('exit', (c, sig) => { code = c ?? sig; });
    const end = Date.now() + 6000;
    while (Date.now() < end && code === null) await sleep(100);
    const maps = themeEvents(S).slice(before).filter((e) => e.type === 'map');
    const bound = [...new Set([...err.matchAll(/wl_registry#\d+\.bind\(\d+, "(\w+)", (\d+)/g)].map((m) => `${m[1]} ${m[2]}`))];
    const proto = (err.match(/(?:protocol error|error \d+:|wl_display.*error)[^\n]*/i) || [''])[0];
    const alive = code === null;
    const ok = t.exits ? code === 0 : (alive && maps.length > 0);
    res.push({ client: t.name, result: ok ? 'pass' : 'fail', msg: ok ? (t.exits ? 'ran to the end' : `mapped ${maps.length} window(s), still running after 6 s`) : (alive ? 'running but no window mapped in 6 s' : `exited (${code})${proto ? ': ' + proto : ''}`), bound });
    try { process.kill(-P.pid, 'SIGTERM'); } catch {}
    await sleep(300);
  }
  return res;
}

// ---------------------------------------------------------------- X11
async function x11Section(S, src, spec) {
  const items = new Map(spec.items.map((it) => [it.id, { ...it, checks: [] }]));
  const add = (item, c) => { if (!items.has(item)) items.set(item, { id: item, title: item, checks: [] }); items.get(item).checks.push(c); };
  // source patterns
  for (const it of spec.items) for (const p of it.static || []) {
    const at = src.findRaw(p.file, new RegExp(p.pattern));
    const ok = p.absent ? !at : !!at;
    add(it.id, { kind: 'static', id: `src.${p.id}`, what: p.what, weight: W.x11static, result: ok ? 'pass' : 'fail', msg: ok ? (at ? `found at ${at}` : 'absent, as it should be') : `${p.what}: not in ${p.file}`, where: at || p.file });
  }
  // unit tests
  for (const it of spec.items) for (const u of it.unit || []) {
    const mk = sh('make', ['-C', SERVER, `build/${u}`]);
    const r = mk.status === 0 ? sh(path.join(BUILD, u), [], { cwd: SERVER, timeout: 60000 }) : null;
    const ok = r && r.status === 0;
    add(it.id, { kind: 'unit', id: `unit.${u}`, what: `build/${u}`, weight: W.unit, result: r ? (ok ? 'pass' : 'fail') : 'skip', msg: r ? (ok ? 'passes' : `fails: ${(r.stdout + r.stderr).trim().split('\n').slice(-2).join(' ')}`) : 'could not build it', where: `server/tests/${u.replace(/-test$/, '_test')}.c` });
  }
  if (!S) return { items: [...items.values()], dynamic: 'not run (--static-only)' };
  const disp = S.display();
  if (!disp) return { items: [...items.values()], dynamic: 'not run: lord-server opened no X socket (is the xwayland package installed?)' };
  const xb = buildX11Client();
  if (xb.error) return { items: [...items.values()], dynamic: `not run: the xcb client doesn't build (${xb.error}; needs libxcb1-dev)` };
  const xenv = { ...S.env, DISPLAY: disp, XAUTHORITY: path.join(S.D, `lord-xauth.${disp.slice(1)}`) };
  delete xenv.WAYLAND_DISPLAY;
  const r = runClient(xb.bin, ['wm'], xenv, 90000);
  for (const c of parseChecks(r.out)) add(c.item, { kind: 'dynamic', id: c.id, what: c.hint, weight: W.x11dyn, result: c.result, msg: c.msg, hint: c.hint });
  // what the theme got for those windows
  await sleep(500);
  const ev = themeEvents(S);
  const mapOf = (title) => ev.find((e) => e.type === 'map' && e.d && e.d.title === title);
  const m1 = mapOf('wlconf-x11');
  add('xwayland.startup', { kind: 'dynamic', id: 'surface_association', what: 'WL_SURFACE_ID / xwayland-shell-v1', weight: W.x11dyn, result: m1 ? 'pass' : 'fail', msg: m1 ? `the X window reached the theme as a window (${m1.d.width}x${m1.d.height})` : 'the X window never reached the theme (map)' });
  add('icccm.wm_name', { kind: 'dynamic', id: 'title_on_map', what: '_NET_WM_NAME', weight: W.x11dyn, result: m1 ? 'pass' : 'fail', msg: m1 ? 'the theme got the title "wlconf-x11"' : 'no window with that title' });
  const renamed = ev.find((e) => e.type === 'title' && JSON.stringify(e.d).includes('wlconf-x11-renamed'));
  add('icccm.wm_name', { kind: 'dynamic', id: 'title_change', what: '_NET_WM_NAME', weight: W.x11dyn, result: renamed ? 'pass' : 'fail', msg: renamed ? 'a later _NET_WM_NAME change reached the theme' : 'the new title never reached the theme' });
  const clsSeen = ev.some((e) => e.type === 'map' && String(e.d?.title || '').startsWith('wlconf-x11') && /wlconf-x11|Wlconf-X11/.test(JSON.stringify(e.d).replace(/"title":"[^"]*"/, '')));
  add('icccm.wm_class', { kind: 'dynamic', id: 'class_on_map', what: 'WM_CLASS', weight: W.x11dyn, result: clsSeen ? 'pass' : 'fail', msg: clsSeen ? 'the theme got the WM_CLASS (app id)' : 'no WM_CLASS in what the theme got (map)' });
  const pidSeen = ev.some((e) => String(e.d?.title || '').startsWith('wlconf-x11') && /"pid":\s*[1-9]/.test(JSON.stringify(e.d)));
  add('ewmh.wm_pid', { kind: 'dynamic', id: 'pid_on_map', what: '_NET_WM_PID', weight: W.x11dyn, result: pidSeen ? 'pass' : 'skip', msg: pidSeen ? 'the theme got the pid' : 'the theme isn\'t told a pid (lord-server may use it internally; not checkable from here)' });
  const dlg = mapOf('wlconf-x11-dialog');
  const parent = dlg && (/"parent":\s*"?[^n"]/.test(JSON.stringify(dlg.d)) || ev.some((e) => e.type === 'parent' && e.id === dlg.id));
  add('icccm.transient_for', { kind: 'dynamic', id: 'parent_known', what: 'WM_TRANSIENT_FOR', weight: W.x11dyn, result: parent ? 'pass' : 'fail', msg: parent ? 'the theme knows the dialog\'s parent' : (dlg ? 'the dialog mapped with no parent' : 'the dialog never mapped') });
  const mot = mapOf('wlconf-x11-motif');
  const noFrame = mot && /"csd":\s*true|"deco":\s*"?(client|none)/.test(JSON.stringify(mot.d));
  add('motif.wm_hints', { kind: 'dynamic', id: 'no_decorations', what: '_MOTIF_WM_HINTS', weight: W.x11dyn, result: noFrame ? 'pass' : 'fail', msg: noFrame ? 'decorations 0: the theme draws no frame' : (mot ? `decorations 0 but the theme frames it (${JSON.stringify(mot.d).slice(0, 120)})` : 'the window never mapped') });
  const icon = ev.some((e) => e.type === 'icon' && e.title === 'wlconf-x11-motif');
  // always on top and skip pager reach the theme's window model (sdk/wayland.js: w.layer, args.skipPager)
  const ab = mapOf('wlconf-x11-above');
  add('ewmh.wm_state', { kind: 'dynamic', id: 'above_reaches_theme', what: '_NET_WM_STATE_ABOVE', weight: W.x11dyn, result: ab && ab.d.layer === 1 ? 'pass' : 'fail', msg: ab ? `the window mapped with layer ${ab.d.layer} (1: always on top)` : 'the always-on-top window never mapped' });
  const layerEv = ev.filter((e) => e.type === 'layer' && e.title === 'wlconf-x11-above').map((e) => e.d && e.d.layer);
  const toggled = layerEv.includes(0) && layerEv.includes(1) && layerEv.includes(-1);
  add('ewmh.wm_state', { kind: 'dynamic', id: 'layer_changes_reach_theme', what: '_NET_WM_STATE_ABOVE', weight: W.x11dyn, result: toggled ? 'pass' : 'fail', msg: `the theme was told the layers ${JSON.stringify(layerEv)} (off, on, below expected)` });
  const pager = ev.some((e) => e.type === 'pager' && String(e.title).startsWith('wlconf-x11') && e.d && e.d.skip === true);
  add('ewmh.wm_state', { kind: 'dynamic', id: 'skip_pager_reaches_theme', what: '_NET_WM_STATE_SKIP_PAGER', weight: W.x11dyn, result: pager ? 'pass' : 'fail', msg: pager ? 'the theme was told to leave it out of the Overview' : 'no pager event reached the theme' });
  add('ewmh.wm_icon', { kind: 'dynamic', id: 'icon_reaches_theme', what: '_NET_WM_ICON', weight: W.x11dyn, result: icon ? 'pass' : 'fail', msg: icon ? 'the theme got the icon' : 'a _NET_WM_ICON never reached the theme' });
  // selections, both ways
  const wlb = path.join(BUILD, 'wl-conformance-client');
  const sel = async (id, what, ownArgs, readArgs, xOwns) => {
    const owner = xOwns ? runBg(xb.bin, ['clip-own', ...ownArgs], xenv) : runBg(wlb, ['clip-own', ...ownArgs], S.env);
    const ready = await owner.waitFor(xOwns ? /READY (\S+) (\w+)/ : /OWNED (\w+)/, 8000);
    let result = 'fail', msg;
    if (!ready) msg = 'the owner never got the selection (no keyboard focus?)';
    else {
      await sleep(800);
      const sum = xOwns ? ready[2] : ready[1];
      const rr = xOwns ? runClient(wlb, ['clip-read', ...readArgs], S.env, 30000) : runClient(xb.bin, ['clip-read', ...readArgs], xenv, 30000);
      const m = rr.out.match(/CLIP (-?\d+) (\w+)/);
      const want = +(xOwns ? readArgs[1] : readArgs[0]);
      result = m && +m[1] === want && m[2] === sum ? 'pass' : 'fail';
      msg = m ? `read ${m[1]} of ${want} bytes, checksum ${m[2] === sum ? 'matches' : `${m[2]} ≠ ${sum}`}${rr.out.includes('incr=1') ? ' (INCR)' : ''}` : `the reader got nothing (${rr.out.trim() || rr.err.trim().slice(0, 120)})`;
    }
    try { owner.P.kill('SIGTERM'); } catch {}
    await owner.done.catch(() => {});
    add('selections', { kind: 'dynamic', id, what, weight: W.x11dyn, result, msg });
  };
  await sel('x11_to_wayland_clipboard', 'CLIPBOARD X11 → Wayland', ['4000', '0'], ['text/plain;charset=utf-8', '4000'], true);
  await sel('x11_to_wayland_primary', 'PRIMARY X11 → Wayland', ['3000', '0', 'primary'], ['text/plain;charset=utf-8', '3000', 'primary'], true);
  await sel('x11_to_wayland_incr', 'CLIPBOARD X11 → Wayland, INCR (1 MiB)', ['1048576', '1'], ['text/plain;charset=utf-8', '1048576'], true);
  await sel('wayland_to_x11_clipboard', 'CLIPBOARD Wayland → X11', ['5000'], ['5000'], false);
  await sel('wayland_to_x11_incr', 'CLIPBOARD Wayland → X11, INCR (2 MiB)', ['2097152'], ['2097152'], false);
  return { items: [...items.values()], dynamic: 'run' };
}

// ---------------------------------------------------------------- scoring and output
function score(checks) {
  let tot = 0, got = 0, skips = 0;
  for (const c of checks) {
    if (c.result === 'skip') { skips++; continue; }
    tot += c.weight;
    if (c.result === 'pass' || c.result === 'allowed') got += c.weight;
  }
  return { score: tot ? Math.floor((got / tot) * 1000) / 10 : 0, points: got, total: tot, skipped: skips };
}

// Which of the requests, events and errors of the interfaces reachable from a global a protocol-specific flow check
// names in its hint (comma-separated: "wp_fifo_v1.set_barrier,wp_fifo_v1.wait_barrier"). Allowed exceptions don't count.
// Reported beside the score: the static checks cover every message; this says which ones a flow also exercised.
function coverage(P, allow, ifaces, version, checks) {
  const named = new Set();
  for (const c of checks) if (c.kind === 'flow' && (c.result === 'pass' || c.result === 'allowed')) for (const h of String(c.what || '').split(',')) named.add(h.trim());
  const want = [];
  for (const name of ifaces) {
    const I = P.byIface.get(name);
    if (!I || LIBWAYLAND.has(name)) continue;
    const ver = Math.min(version || 1, I.version);
    for (const r of I.requests) if (r.since <= ver && !allow.requests?.[`${name}.${r.name}`]) want.push(`${name}.${r.name}`);
    for (const e of I.events) if (e.since <= ver && !allow.events?.[`${name}.${e.name}`]) want.push(`${name}.${e.name}`);
    for (const en of I.enums.find((x) => x.name === 'error')?.entries || []) if (en.since <= ver && !allow.errors?.[`${name}.error.${en.name}`]) want.push(`${name}.error.${en.name}`);
  }
  const missing = want.filter((m) => !named.has(m));
  return { covered: want.length - missing.length, total: want.length, missing };
}

function hintWhere(hint, src) {
  // "xdg_surface.error.unconfigured_buffer" → where it's raised; "wl_surface.set_buffer_scale" → its handler
  if (!hint || hint === '-') return null;
  hint = hint.split(',')[0];
  const e = hint.match(/^(\w+)\.error\.(\w+)$/);
  if (e) return src.find(new RegExp(`\\b${up(e[1])}_ERROR_${up(e[2])}\\b`));
  const r = hint.match(/^(\w+)\.(\w+)$/);
  if (r) {
    for (const im of src.impls.get(r[1]) || []) { const fn = im.fields[r[2]]; if (fn && src.fns.get(fn)) return src.fns.get(fn).where; }
    return src.find(new RegExp(`\\b${r[1]}_send_${r[2]}\\s*\\(`));
  }
  return null;
}

async function main() {
  const t0 = Date.now();
  const P = loadProtocols();
  const src = loadSources();
  let allow = {};
  try { allow = JSON.parse(fs.readFileSync(OPTS.allow, 'utf8')); } catch {}
  const xspec = JSON.parse(fs.readFileSync(OPTS.xwaylandSpec, 'utf8'));
  const claims = readClaims(OPTS.claims);
  const sGlobals = staticGlobals(src, P);
  let commit = '';
  try { commit = execFileSync('git', ['-C', ROOT, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim(); } catch {}
  let dirty = false;
  try { dirty = execFileSync('git', ['-C', ROOT, 'status', '--porcelain', '--', 'server/src'], { encoding: 'utf8' }).trim() !== ''; } catch {}

  let S = null, runtime = null, dynamic = [], smokeRes = [], built = null, notes = [];
  if (!OPTS.staticOnly) {
    S = await startServer();
    console.error(`lord-server (headless, CPU) on ${S.sock} in ${S.D}`);
    const g = sh(path.join(BUILD, 'wl-globals'), [], { env: S.env });
    if (g.status !== 0) { sh('make', ['-C', SERVER, 'build/wl-globals']); }
    const g2 = g.status === 0 ? g : sh(path.join(BUILD, 'wl-globals'), [], { env: S.env });
    runtime = new Map(g2.stdout.trim().split('\n').filter(Boolean).map((l) => { const [i, v] = l.split(' '); return [i, +v]; }));
  }
  // the globals: what a client sees, else the source
  const globals = new Map();
  for (const g of sGlobals) if (!globals.has(g.iface)) globals.set(g.iface, { iface: g.iface, version: g.version, where: g.where, libwayland: !!g.libwayland });
  if (runtime) {
    for (const [i, v] of runtime) { const g = globals.get(i) || { iface: i, where: null }; g.version = v; g.advertised = true; globals.set(i, g); }
    for (const g of globals.values()) if (!runtime.has(g.iface)) { g.advertised = false; g.note = 'created in the source but not advertised to a normal client here (conditional, trusted-only or needs hardware)'; }
  }
  if (claims) for (const [i, c] of claims) if (!globals.has(i)) globals.set(i, { iface: i, version: null, claimed: c.version, advertised: false, where: null, note: 'claimed but not advertised' });
  const list = [...globals.values()].filter((g) => !OPTS.only || OPTS.only.includes(g.iface)).sort((a, b) => a.iface.localeCompare(b.iface));

  if (S) {
    built = buildWlClient(P, list.map((g) => g.iface));
    const gargs = list.filter((g) => g.advertised !== false && g.version).map((g) => {
      const I = P.byIface.get(g.iface);
      const d = I ? I.requests.findIndex((r) => r.type === 'destructor') : -1;
      return `${g.iface}:${g.version}:${d}:${d >= 0 ? I.requests[d].since : 1}`;
    });
    const gen = runClient(built.bin, ['generic', ...gargs], S.env, 300000);
    dynamic.push(...parseChecks(gen.out).map((c) => ({ ...c, kind: 'generic' })));
    const fl = runClient(built.bin, ['flows', ...(OPTS.only || [])], S.env, 600000);
    if (fl.signal || (fl.status !== 0 && fl.status !== null)) notes.push(`wl-conformance-client flows ended with ${fl.signal || fl.status}`);
    dynamic.push(...parseChecks(fl.out).map((c) => ({ ...c, kind: 'flow' })));
    await sleep(500);
    dynamic.push(...themeFlowChecks(themeEvents(S)));
    // the close request from the theme (xdg_toplevel.close): checked in the X11 run for X; for Wayland via the hook
    if (!S.alive()) notes.push('lord-server died during the dynamic checks');
  }

  // per item
  const items = [];
  for (const g of list) {
    const I = P.byIface.get(g.iface);
    const checks = [];
    let ifaces = [];
    if (I && g.version) {
      const st = staticChecks(P, src, allow, g.iface, g.version);
      checks.push(...st.checks);
      ifaces = st.ifaces;
    }
    if (claims) {
      const c = claims.get(g.iface);
      if (c) checks.push({ kind: 'claim', id: 'claim.version', what: 'claimed version', weight: W.claim, result: c.version === g.version ? 'pass' : 'fail', msg: `claimed v${c.version}, advertised v${g.version ?? 'none'}` });
    }
    for (const d of dynamic.filter((d) => d.item === g.iface)) {
      const where = d.result === 'fail' ? hintWhere(d.hint, src) : null;
      const allowed = d.result === 'fail' && allow.dynamic?.[`${g.iface}/${d.id}`];
      checks.push({ kind: d.kind, id: `${d.kind}.${d.id}`, what: d.hint, weight: d.kind === 'generic' ? W.generic : W.flow, result: allowed ? 'allowed' : d.result, msg: allowed ? `${d.msg} — allowed: ${allowed}` : d.msg, where });
    }
    const s = score(checks);
    const hasFlow = checks.some((c) => c.kind === 'flow' && c.result !== 'skip');
    // a global a normal client never sees (drm-lease without a headset, xwayland-shell for Xwayland only) counts as
    // complete when its own flow ran it, through lord-server's --test-hooks (a fake headset, an Xwayland-like connection)
    const complete = s.score === 100 && !s.skipped && !!I && hasFlow;
    items.push({
      global: g.iface, protocol: I ? I.protocol.name : null, xml: I ? path.relative(ROOT, I.protocol.file).startsWith('..') ? I.protocol.file : path.relative(ROOT, I.protocol.file) : null,
      version: g.version ?? null, advertised: g.advertised ?? null, libwayland: g.libwayland || undefined, where: g.where, note: g.note,
      interfaces: ifaces, ...s, complete, depth: hasFlow ? (g.advertised === false ? 'protocol-specific (test hooks)' : 'protocol-specific') : (checks.some((c) => c.kind === 'generic') ? 'generic only' : 'static only'),
      coverage: coverage(P, allow, ifaces, g.version, checks),
      checks,
    });
  }

  // Xwayland
  let x11 = null;
  if (OPTS.x11 && (!OPTS.only || OPTS.only.some((o) => o.startsWith('x')))) {
    const xs = await x11Section(S, src, xspec);
    x11 = { dynamic: xs.dynamic, items: xs.items.map((it) => ({ id: it.id, title: it.title, ...score(it.checks), complete: false, checks: it.checks })) };
    for (const it of x11.items) it.complete = it.score === 100 && !it.skipped && it.checks.some((c) => c.kind === 'dynamic');
  }
  if (S && OPTS.smoke && !OPTS.only) smokeRes = await smoke(S);
  if (S) { if (!S.alive()) notes.push('lord-server was not alive at the end'); S.stop(); if (!OPTS.verbose) fs.rmSync(S.D, { recursive: true, force: true }); else console.error(`kept ${S.D}`); }

  const result = {
    schema: 1,
    date: OPTS.date,
    commit, dirty,
    tool: 'tools/wl-conformance.mjs',
    method: 'docs/WAYLAND.md#conformance',
    weights: W,
    environment: {
      wayland_protocols: (sh('pkg-config', ['--modversion', 'wayland-protocols']).stdout || '').trim() || null,
      libwayland: (sh('pkg-config', ['--modversion', 'wayland-server']).stdout || '').trim() || null,
      xwayland: (sh('dpkg-query', ['-W', '-f=${Version}', 'xwayland']).stdout || '').trim() || null,
      mode: OPTS.staticOnly ? 'static' : 'static+dynamic (headless, CPU path)',
      claims: claims ? path.relative(ROOT, OPTS.claims) : `${path.relative(ROOT, OPTS.claims)} (not found: globals read from the source and the running server)`,
      generated_protocols: built?.protocols || [],
    },
    summary: {
      protocols: items.length,
      complete: items.filter((i) => i.complete).length,
      mean: items.length ? Math.round(items.reduce((a, i) => a + i.score, 0) / items.length * 10) / 10 : 0,
      x11_mean: x11?.items.length ? Math.round(x11.items.reduce((a, i) => a + i.score, 0) / x11.items.length * 10) / 10 : null,
    },
    notes,
    protocols: items,
    xwayland: x11,
    clients: smokeRes,
    seconds: Math.round((Date.now() - t0) / 1000),
  };
  fs.mkdirSync(path.dirname(OPTS.json), { recursive: true });
  fs.writeFileSync(OPTS.json, JSON.stringify(result, null, 1) + '\n');
  if (OPTS.record) fs.writeFileSync(path.join(ROOT, 'docs', 'wayland-conformance.json'), JSON.stringify(result, null, 1) + '\n');
  printReport(result);
}

function printReport(r) {
  const pad = (s, n) => String(s).padEnd(n), lpad = (s, n) => String(s).padStart(n);
  const bar = (p) => { const n = Math.round(p / 10); return '█'.repeat(n) + '·'.repeat(10 - n); };
  console.log(`\nWayland conformance — ${r.date}, commit ${r.commit.slice(0, 10)}${r.dirty ? ' (+local changes)' : ''}, ${r.environment.mode}`);
  console.log(`${pad('global', 46)} ${pad('ver', 4)} ${lpad('score', 6)}  ${pad('', 10)}  ${pad('fails', 5)} ${pad('depth', 18)}`);
  for (const i of r.protocols) {
    const fails = i.checks.filter((c) => c.result === 'fail').length;
    console.log(`${pad(i.global, 46)} ${pad(i.version ?? '-', 4)} ${lpad(i.score.toFixed(1), 5)}%  ${bar(i.score)}  ${lpad(fails, 5)} ${pad(i.depth, 18)}${i.complete ? ' complete' : ''}${i.advertised === false ? ' (not advertised)' : ''}`);
  }
  if (r.xwayland) {
    console.log(`\nXwayland / X11 (${r.xwayland.dynamic})`);
    for (const i of r.xwayland.items) {
      const fails = i.checks.filter((c) => c.result === 'fail').length;
      console.log(`${pad(i.id, 46)} ${pad('', 4)} ${lpad(i.score.toFixed(1), 5)}%  ${bar(i.score)}  ${lpad(fails, 5)}${i.complete ? ' complete' : ''}`);
    }
  }
  if (r.clients.length) {
    console.log('\nReal clients (not scored)');
    for (const c of r.clients) console.log(`  ${pad(c.client, 30)} ${pad(c.result, 5)} ${c.msg}${c.bound?.length ? ` — bound ${c.bound.length} globals` : ''}`);
  }
  console.log('\nFailing checks');
  for (const i of [...r.protocols, ...(r.xwayland?.items || [])]) for (const c of i.checks) if (c.result === 'fail') console.log(`  ${i.global || i.id}: ${c.id} — ${c.msg}${c.where ? `  [${c.where}]` : ''}`);
  console.log(`\n${r.summary.protocols} protocols, mean ${r.summary.mean} %, ${r.summary.complete} complete${r.summary.x11_mean !== null ? `; X11 mean ${r.summary.x11_mean} %` : ''}; ${r.seconds} s`);
  for (const n of r.notes) console.log(`note: ${n}`);
}

main().catch((e) => { console.error(`wl-conformance: ${e.stack || e}`); process.exit(1); });
