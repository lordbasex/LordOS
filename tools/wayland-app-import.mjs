#!/usr/bin/env node
// Refreshes the KDE (KWin) and GNOME (Mutter) columns of docs/wayland-protocols.json from wayland.app.
//
//   node tools/wayland-app-import.mjs [--bundle main.js] [--date YYYY-MM-DD]
//   node tools/gen-wayland-table.mjs            # then regenerate docs/WAYLAND.md's table and the site data
//
// wayland.app (https://wayland.app/protocols/, the Wayland Explorer) is a single-page app: every protocol page
// is drawn from data inside its one JavaScript bundle, which holds the protocol registry (id, name, source,
// stability, the protocol XML as JSON) and, per compositor, the globals that compositor advertised to an
// ordinary client (wayland-info) with their versions. This script reads that bundle (downloaded, or a saved copy
// with --bundle), takes the registry and the Mutter and KWin lists, and updates the JSON:
//   - every protocol of the core, wayland-protocols, wlr-protocols and kde-protocols, plus any other one that
//     LordOS, KWin or Mutter advertises or that LordOS keeps a status for, is listed;
//   - `kwin` and `mutter` are replaced with what wayland.app states ({global: version}, {} when not advertised);
//   - `lordos` (written by hand) is kept as it is; the rest of each row is wayland.app's.
// It never touches the LordOS columns: those come from server/src/wl/*.c (tests/wayland_table_test.mjs checks them).
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { validate, RE_GLOBAL, RE_CVER, isVersion } from './gen-wayland-table.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const FILE = path.join(REPO, 'docs/wayland-protocols.json');
const SITE = 'https://wayland.app/protocols/';
const KEEP_SOURCES = new Set(['core', 'wayland-protocols', 'wlr-protocols', 'kde-protocols']);
// interfaces wayland.app's dumps have that its registry doesn't (older protocol versions): their own rows
const EXTRA = {
  zxdg_exporter_v1: { id: 'xdg-foreign-unstable-v1', name: 'XDG foreign v1', source: 'wayland-protocols', stability: 'unstable', deprecated: true, summary: 'Protocol for exporting xdg surface handles (superseded by xdg-foreign-unstable-v2)', globals: ['zxdg_exporter_v1', 'zxdg_importer_v1'] },
  zwp_text_input_manager_v2: { id: 'text-input-unstable-v2', name: 'Text input v2', source: 'kde-protocols', stability: 'unstable', deprecated: true, summary: 'Protocol for composing text (KDE\'s v2, superseded by text-input-unstable-v3)', globals: ['zwp_text_input_manager_v2'] },
};

const arg = (k) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : null; };

async function bundle() {
  const f = arg('--bundle');
  if (f) return fs.readFileSync(f, 'utf8');
  const html = await (await fetch(SITE)).text();
  const m = html.match(/src="(\/protocols\/static\/js\/main\.[0-9a-f]+\.js)"/);
  if (!m) throw new Error('wayland.app: no main bundle in the index page');
  return (await fetch(new URL(m[1], SITE))).text();
}

// The bundle is untrusted text from a third-party site: nothing in it is ever run. Its registry and its
// JSON.parse('...') modules are read by the small literal reader below, which accepts only string, number and
// boolean literals, arrays, objects, enum members (X.y, from the bundle's own string enums) and module references
// (f(123)); anything else (a call, an operator, a template, a getter) is refused.
const ESC = { n: '\n', t: '\t', r: '\r', b: '\b', f: '\f', v: '\v', '0': '\0' };
// the string literal opening at s[i] ('"' or "'"): { value, end } (end is just past the closing quote)
export function readString(s, i) {
  const q = s[i];
  if (q !== '"' && q !== "'") throw new Error(`wayland.app: expected a string at ${i}`);
  let out = '';
  for (let k = i + 1; k < s.length; k++) {
    const c = s[k];
    if (c === q) return { value: out, end: k + 1 };
    if (c === '\n' || c === '\r') throw new Error('wayland.app: newline in a string literal');
    if (c !== '\\') { out += c; continue; }
    const e = s[++k];
    if (e === undefined) break;
    if (e in ESC && !(e === '0' && /[0-9]/.test(s[k + 1] || ''))) out += ESC[e];
    else if (e === 'x') { const h = s.slice(k + 1, k + 3); if (!/^[0-9a-fA-F]{2}$/.test(h)) throw new Error('wayland.app: bad \\x escape'); out += String.fromCharCode(parseInt(h, 16)); k += 2; }
    else if (e === 'u') {
      let h, n;
      if (s[k + 1] === '{') { const z = s.indexOf('}', k); h = s.slice(k + 2, z); n = z - k; if (!/^[0-9a-fA-F]{1,6}$/.test(h)) throw new Error('wayland.app: bad \\u escape'); }
      else { h = s.slice(k + 1, k + 5); n = 4; if (!/^[0-9a-fA-F]{4}$/.test(h)) throw new Error('wayland.app: bad \\u escape'); }
      const cp = parseInt(h, 16); if (cp > 0x10ffff) throw new Error('wayland.app: bad \\u escape');
      out += String.fromCodePoint(cp); k += n;
    } else if (e === '\r') { if (s[k + 1] === '\n') k++; }             // line continuation
    else if (e === '\n' || e === '\u2028' || e === '\u2029') { /* line continuation */ }
    else if (/[0-9]/.test(e)) throw new Error('wayland.app: octal escape in a string literal');
    else out += e;                                              // \' \" \\ and other identity escapes
  }
  throw new Error('wayland.app: unterminated string literal');
}

// the literal starting at s[i] (see above); enums: { name: { member: string } }. Returns { value, end }.
export function readLiteral(s, i, enums = {}) {
  const ws = (k) => { while (k < s.length && /\s/.test(s[k])) k++; return k; };
  const ident = (k) => { const m = /^[A-Za-z_$][\w$]*/.exec(s.slice(k, k + 256)); return m ? m[0] : null; };
  let depth = 0;
  const value = (k) => {
    k = ws(k);
    const c = s[k];
    if (c === '"' || c === "'") return readString(s, k);
    if (c === '[' || c === '{') {
      if (++depth > 64) throw new Error('wayland.app: literal nested too deep');
      const close = c === '[' ? ']' : '}';
      const out = c === '[' ? [] : Object.create(null);
      k = ws(k + 1);
      while (s[k] !== close) {
        if (c === '[') { const v = value(k); out.push(v.value); k = ws(v.end); }
        else {
          let key;
          if (s[k] === '"' || s[k] === "'") { const r = readString(s, k); key = r.value; k = r.end; }
          else if (/[0-9]/.test(s[k])) { const m = /^\d+/.exec(s.slice(k)); key = m[0]; k += key.length; }
          else { key = ident(k); if (!key) throw new Error(`wayland.app: bad key at ${k}`); k += key.length; }
          if (key === '__proto__' || key === 'constructor' || key === 'prototype') throw new Error('wayland.app: refusing key ' + key);
          k = ws(k);
          if (s[k] !== ':') throw new Error(`wayland.app: expected ':' at ${k}`);
          const v = value(k + 1); out[key] = v.value; k = ws(v.end);
        }
        if (s[k] === ',') k = ws(k + 1);
        else if (s[k] !== close) throw new Error(`wayland.app: expected ',' or '${close}' at ${k}`);
      }
      depth--;
      return { value: c === '{' ? Object.assign({}, out) : out, end: k + 1 };
    }
    if (c === '!' && (s[k + 1] === '0' || s[k + 1] === '1') && !/[\w$.]/.test(s[k + 2] || '')) return { value: s[k + 1] === '0', end: k + 2 };
    const num = /^-?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?/.exec(s.slice(k, k + 64));
    if (num) return { value: Number(num[0]), end: k + num[0].length };
    const id = ident(k);
    if (!id) throw new Error(`wayland.app: unexpected '${c}' at ${k}`);
    let e = k + id.length;
    if (id === 'true' || id === 'false') return { value: id === 'true', end: e };
    if (id === 'null') return { value: null, end: e };
    if (id === 'void' && s.slice(ws(e), ws(e) + 1) === '0') return { value: undefined, end: ws(e) + 1 };
    if (s[e] === '.') {                                          // an enum member: X.y
      const m = ident(e + 1);
      if (!m || !Object.hasOwn(enums, id) || !Object.hasOwn(enums[id], m)) throw new Error(`wayland.app: unknown enum ${id}.${m}`);
      return { value: enums[id][m], end: e + 1 + m.length };
    }
    if (s[e] === '(') {                                          // a module reference: f(123)
      const m = /^\((\d{1,9})\)/.exec(s.slice(e, e + 16));
      if (!m) throw new Error(`wayland.app: refusing call ${id}(...) at ${k}`);
      return { value: { mod: Number(m[1]) }, end: e + m[0].length };
    }
    throw new Error(`wayland.app: refusing identifier ${id} at ${k}`);
  };
  return value(i);
}

// the JSON.parse('...') module with this webpack id
export function moduleJson(s, id) {
  const i = s.indexOf(`${id}:function(e){"use strict";e.exports=JSON.parse('`);
  if (i < 0) return null;
  return JSON.parse(readString(s, s.indexOf("'", i)).value);
}

// the balanced [...] that starts at `start`
function balanced(s, start) {
  let d = 0, q = null;
  for (let j = start; j < s.length; j++) {
    const c = s[j];
    if (q) { if (c === '\\') j++; else if (c === q) q = null; continue; }
    if (c === '"' || c === "'" || c === '`') q = c;
    else if ('[{('.includes(c)) d++;
    else if (']})'.includes(c) && --d === 0) return s.slice(start, j + 1);
  }
  throw new Error('unbalanced');
}

function enumOf(s, v) {
  const i = s.indexOf(`}(${v}||(${v}={}))`);
  const seg = s.slice(s.lastIndexOf('function(e){', i), i);
  return Object.fromEntries([...seg.matchAll(/e\.(\w+)="([^"]+)"/g)].map((m) => [m[1], m[2]]));
}

export function parse(s) {
  const at = s.search(/\w=\[\{id:"wayland",name:/);
  if (at < 0) throw new Error('wayland.app: protocol registry not found');
  const arr = balanced(s, s.indexOf('[', at));
  // the registry is written as code: source/stability enum members and f(<module>) for each protocol's XML; it is
  // read as a literal (readLiteral), never run
  const vars = [...new Set([...arr.matchAll(/source:(\w+)\.\w+,stability:(\w+)\.\w+/g)].flatMap((m) => [m[1], m[2]]))];
  const enums = Object.fromEntries(vars.map((v) => [v, enumOf(s, v)]));
  const reg = readLiteral(arr, 0, enums).value;
  if (!Array.isArray(reg)) throw new Error('wayland.app: the registry is not an array');
  const comps = {};
  for (const m of s.matchAll(/\{id:"(mutter|kwin)",name:"[^"]+",icon:"[^"]+",info:\w+\((\d+)\)\}/g)) comps[m[1]] = moduleJson(s, m[2]);
  if (!comps.mutter || !comps.kwin) throw new Error('wayland.app: Mutter or KWin data not found');
  const protocols = reg.map((p) => {
    const x = moduleJson(s, p.protocol.mod) || { interfaces: [] };
    const ifs = x.interfaces || [];
    const made = new Set(ifs.flatMap((i) => [...(i.requests || []), ...(i.events || [])]).flatMap((m) => (m.args || []).filter((a) => a.argType === 'new_id' && a.interface).map((a) => a.interface)));
    return { id: p.id, name: p.name, source: p.source, stability: p.stability, deprecated: !!(p.deprecated && p.deprecated.length) || /deprecated/i.test(p.name),
      summary: (x.description && x.description.summary) || '', ifs: ifs.map((i) => i.name), ifSummary: Object.fromEntries(ifs.map((i) => [i.name, (i.description && i.description.summary) || ''])), roots: ifs.map((i) => i.name).filter((n) => !made.has(n) && n !== 'wl_display') };
  });
  return { protocols, comps };
}

const cap = (s) => s.charAt(0).toUpperCase() + s.slice(1);

async function main() {
  const { protocols, comps } = parse(await bundle());
  const old = fs.existsSync(FILE) ? JSON.parse(fs.readFileSync(FILE, 'utf8')) : { protocols: [] };
  const byId = new Map(old.protocols.map((p) => [p.id, p]));
  // wayland.app is external input: every advertised global must be a plain interface name with a positive integer version
  for (const c of ['kwin', 'mutter']) {
    if (!RE_CVER.test(String(comps[c].version)) || !Number.isFinite(comps[c].generationTimestamp)) throw new Error(`wayland.app: ${c}: bad version or date`);
    for (const g of comps[c].globals) if (!RE_GLOBAL.test(g.interface) || !isVersion(g.version)) throw new Error(`wayland.app: ${c}: refusing global ${JSON.stringify(g)}`);
  }
  const adv = (c) => new Map(comps[c].globals.map((g) => [g.interface, g.version]));
  const K = adv('kwin'), M = adv('mutter');
  const out = [];
  const row = (base, globals) => {
    const prev = byId.get(base.id) || {};
    const pick = (map) => Object.fromEntries(globals.filter((g) => map.has(g)).map((g) => [g, map.get(g)]));
    const lordos = prev.lordos || { status: 'missing', globals: {} };
    const r = { id: base.id, name: base.name, source: base.source, stability: base.stability, deprecated: base.deprecated || undefined,
      summary: cap(base.summary || base.name), url: base.url, onWaylandApp: base.onWaylandApp === false ? false : undefined,
      globals, kwin: pick(K), mutter: pick(M), lordos };
    out.push(r);
    return r;
  };
  const known = new Set();
  for (const p of protocols) {
    if (p.id === 'wayland') {   // the core protocol: one row per global
      for (const g of p.roots) {
        if (g === 'wl_registry') continue;
        row({ id: `wayland-${g}`, name: g, source: 'core', stability: 'stable', deprecated: g === 'wl_shell', summary: p.ifSummary[g] || g, url: `${SITE}wayland#${g}` }, [g]);
      }
      p.ifs.forEach((i) => known.add(i));
      continue;
    }
    // the advertised ones first; a protocol nobody advertises shows its likely manager
    let globals = p.roots.filter((g) => K.has(g) || M.has(g) || (byId.get(p.id) && byId.get(p.id).globals && byId.get(p.id).globals.includes(g)));
    if (!globals.length) { const mg = p.roots.filter((g) => /manager|factory|registry|exporter|importer|notifier|_shell|_base|list_v/.test(g)); globals = mg.length ? mg : p.roots.slice(0, 1); }
    p.ifs.forEach((i) => known.add(i));
    const used = KEEP_SOURCES.has(p.source) || globals.some((g) => K.has(g) || M.has(g)) || (byId.get(p.id) && byId.get(p.id).lordos && byId.get(p.id).lordos.status !== 'missing');
    if (!used) continue;
    row({ ...p, url: SITE + p.id }, globals);
  }
  for (const [g, x] of Object.entries(EXTRA)) {
    if (known.has(g)) continue;
    row({ ...x, url: SITE, onWaylandApp: false }, x.globals);
  }
  // rows kept by hand that wayland.app no longer lists stay, marked
  for (const p of old.protocols) if (!out.some((r) => r.id === p.id)) out.push({ ...p, onWaylandApp: false });
  const ts = (c) => new Date(comps[c].generationTimestamp).toISOString().slice(0, 10);
  const doc = {
    $comment: old.$comment || 'The Wayland protocol table: one source of truth for docs/WAYLAND.md\'s generated table and lordos.org/wayland-protocols-table. Edit the lordos fields by hand; tools/wayland-app-import.mjs refreshes kwin and mutter; tools/gen-wayland-table.mjs writes the outputs; server/tests/wayland_table_test.mjs checks them against server/src/wl.',
    compared: {
      date: arg('--date') || new Date().toISOString().slice(0, 10),
      source: SITE,
      kwin: { name: 'KWin', desktop: 'KDE Plasma', version: comps.kwin.version, dataDate: ts('kwin') },
      mutter: { name: 'Mutter', desktop: 'GNOME', version: comps.mutter.version, dataDate: ts('mutter') },
      ...(old.compared && old.compared.lordos ? { lordos: old.compared.lordos } : {}),
    },
    ...(old.statuses ? { statuses: old.statuses } : {}),
    protocols: out,
  };
  // nothing outside the strict shapes is written (gen-wayland-table.mjs's validate: ids, interface names, versions, sources, URLs, plain text)
  const errs = validate(doc);
  if (errs.length) throw new Error('refusing to write docs/wayland-protocols.json:\n  ' + errs.slice(0, 30).join('\n  '));
  fs.writeFileSync(FILE, JSON.stringify(doc, null, 2) + '\n');
  console.log(`wayland.app: ${protocols.length} protocols, KWin ${comps.kwin.version} (${comps.kwin.globals.length} globals, ${ts('kwin')}), Mutter ${comps.mutter.version} (${comps.mutter.globals.length} globals, ${ts('mutter')}); ${out.length} rows in ${path.relative(REPO, FILE)}`);
  const lost = [...K.keys(), ...M.keys()].filter((g) => !out.some((r) => r.globals.includes(g)));
  if (lost.length) console.warn('advertised but in no row:', [...new Set(lost)].join(', '));
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) main().catch((e) => { console.error(e.message); process.exit(1); });
