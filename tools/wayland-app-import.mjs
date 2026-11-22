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

// the JSON.parse('...') module with this webpack id
function moduleJson(s, id) {
  const i = s.indexOf(`${id}:function(e){"use strict";e.exports=JSON.parse('`);
  if (i < 0) return null;
  const st = s.indexOf("'", i) + 1;
  let k = st;
  while (!(s[k] === "'" && s[k - 1] !== '\\')) k++;
  return JSON.parse(Function(`return '${s.slice(st, k)}'`)());
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

function parse(s) {
  const at = s.search(/\w=\[\{id:"wayland",name:/);
  if (at < 0) throw new Error('wayland.app: protocol registry not found');
  const arr = balanced(s, s.indexOf('[', at));
  // the registry is code: source/stability enums and n(<module>) for each protocol's XML
  const vars = [...new Set([...arr.matchAll(/source:(\w+)\.\w+,stability:(\w+)\.\w+/g)].flatMap((m) => [m[1], m[2]]))];
  const enums = vars.map((v) => enumOf(s, v));
  const reg = Function(...vars, 'n', `return ${arr}`)(...enums, (id) => ({ mod: id }));
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
  fs.writeFileSync(FILE, JSON.stringify(doc, null, 2) + '\n');
  console.log(`wayland.app: ${protocols.length} protocols, KWin ${comps.kwin.version} (${comps.kwin.globals.length} globals, ${ts('kwin')}), Mutter ${comps.mutter.version} (${comps.mutter.globals.length} globals, ${ts('mutter')}); ${out.length} rows in ${path.relative(REPO, FILE)}`);
  const lost = [...K.keys(), ...M.keys()].filter((g) => !out.some((r) => r.globals.includes(g)));
  if (lost.length) console.warn('advertised but in no row:', [...new Set(lost)].join(', '));
}

main().catch((e) => { console.error(e.message); process.exit(1); });
