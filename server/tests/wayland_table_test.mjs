// wayland_table_test.mjs — the Wayland protocol table (docs/wayland-protocols.json) against what lord-server really
// advertises: every wl_global_create(..., &X_interface, VERSION, ...) in server/src must be a LordOS global of the
// table with the same version, and every LordOS global of the table must be created somewhere. Then the generated
// outputs (docs/WAYLAND.md's section and web/content/wayland/protocols.json) must be up to date.
//
//   node tests/wayland_table_test.mjs        (from server/; tests/run_suites.sh runs it)
//
// When it fails after adding or bumping a global: edit that protocol's `lordos` entry in docs/wayland-protocols.json
// and run `node tools/gen-wayland-table.mjs`.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { load, validate, validateWeb, markdown, withSection, webData, loadConf, confMarkdown, withConfSection, DEPTHS, PROOFS, confDetail, validateDetail, WEBCONF, expectedHeader, TBHDR, OFFERED } from '../../tools/gen-wayland-table.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
let fails = 0;
const bad = (m) => { fails++; console.error('FAIL ' + m); };

// every .c under server/src, and the .h files that may hold a version macro
const files = [], headers = [];
(function walk(d) { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const p = path.join(d, e.name); if (e.isDirectory()) walk(p); else if (p.endsWith('.c')) files.push(p); else if (p.endsWith('.h')) headers.push(p); } })(path.join(REPO, 'server/src'));

// a version given as a simple macro: #define NAME 3 (or (3), 3u) in the same file first, then in any other source
export function defineOf(name, own, others) {
  const re = new RegExp(`^[ \\t]*#[ \\t]*define[ \\t]+${name}[ \\t]+\\(?[ \\t]*(\\d+)[uU]?[ \\t]*\\)?[ \\t]*(?:/[*/].*)?$`, 'm');
  for (const s of [own, ...others]) { const m = s.match(re); if (m) return +m[1]; }
  return null;
}

// globals in the code: interface name -> {version, where}
export function codeGlobals(srcs, extra = []) {
  const out = new Map();
  const all = [...srcs.map(([, s]) => s), ...extra];
  for (const [f, s] of srcs) {
    const rel = path.relative(REPO, f);
    for (const m of s.matchAll(/wl_global_create\s*\(\s*[^,]+,\s*&(\w+)\s*,\s*([^,]+),/g)) {
      const sym = m[1];
      // a hand-written interface table (static const struct wl_interface kde_mgr_interface = { "org_kde_…", 1, …)
      const own = s.match(new RegExp(`struct\\s+wl_interface\\s+${sym}\\s*=\\s*\\{\\s*"([^"]+)"`));
      const name = own ? own[1] : sym.replace(/_interface$/, '');
      let v = m[2].trim();
      const line = s.slice(0, m.index).split('\n').length;
      if (/^[A-Za-z_]\w*$/.test(v)) { const n = defineOf(v, s, all); if (n != null) v = String(n); }
      if (!/^\d+$/.test(v)) { bad(`${rel}:${line}: ${name}'s version "${v}" is not a number or a simple #define; teach this test to read it`); continue; }
      if (out.has(name) && out.get(name).version !== +v) bad(`${name} is created with two versions (${out.get(name).where}, ${rel}:${line})`);
      out.set(name, { version: +v, where: `${rel}:${line}` });
    }
    // wl_shm comes from libwayland
    if (/\bwl_display_init_shm\s*\(/.test(s)) out.set('wl_shm', { version: null, where: rel + ' (wl_display_init_shm)' });
  }
  return out;
}

const d = load();
for (const e of validate(d)) bad('docs/wayland-protocols.json: ' + e);

const code = codeGlobals(files.map((f) => [f, fs.readFileSync(f, 'utf8')]), headers.map((f) => fs.readFileSync(f, 'utf8')));
const table = new Map();
for (const p of d.protocols) for (const [g, v] of Object.entries(p.lordos.globals || {})) table.set(g, { version: v, id: p.id, libwayland: !!p.lordos.libwayland });

for (const [g, c] of code) {
  const t = table.get(g);
  if (!t) { bad(`${c.where} creates ${g}, but no protocol of docs/wayland-protocols.json lists it in lordos.globals`); continue; }
  if (c.version == null) { if (!t.libwayland) bad(`${g} comes from libwayland; mark ${t.id}'s lordos with "libwayland": true`); continue; }
  if (c.version !== t.version) bad(`${g}: ${c.where} creates version ${c.version}, docs/wayland-protocols.json (${t.id}) says ${t.version}`);
}
for (const [g, t] of table) if (!code.has(g)) bad(`docs/wayland-protocols.json (${t.id}) says lord-server offers ${g}, but nothing in server/src creates it`);
// the trusted-only list in secctx.c names interfaces lord-server must not offer to a sandbox; one it offers to everyone
// would be a contradiction worth seeing
{
  const sc = fs.readFileSync(path.join(REPO, 'server/src/wl/secctx.c'), 'utf8');
  const hidden = (sc.match(/hidden\[\]\s*=\s*\{([\s\S]*?)\};/) || [, ''])[1].match(/"(\w+)"/g) || [];
  for (const h of hidden.map((x) => x.slice(1, -1))) {
    const t = table.get(h);
    if (t && h !== 'wp_security_context_manager_v1') console.log(`note: ${h} (${t.id}) is offered, and hidden from sandboxed clients by secctx.c`);
  }
}

// the generated outputs
const C = loadConf();
const md = fs.readFileSync(path.join(REPO, 'docs/WAYLAND.md'), 'utf8');
if (withSection(md, markdown(d)) !== md) bad('docs/WAYLAND.md\'s generated protocol table is stale: run node tools/gen-wayland-table.mjs');
if (withConfSection(md, confMarkdown(d, C)) !== md) bad('docs/WAYLAND.md\'s generated conformance table is stale: run node tools/gen-wayland-table.mjs');
const webFile = path.join(REPO, 'web/content/wayland/protocols.json');
const web = fs.existsSync(webFile) ? fs.readFileSync(webFile, 'utf8') : '';
if (web !== JSON.stringify(webData(d, C), null, 1) + '\n') bad('web/content/wayland/protocols.json is stale: run node tools/gen-wayland-table.mjs');
// every check, for lordos.org/wayland-conformance/
const confWeb = fs.existsSync(WEBCONF) ? fs.readFileSync(WEBCONF, 'utf8') : '';
if (confWeb !== (C ? JSON.stringify(confDetail(C)) + '\n' : '')) bad('web/content/wayland/conformance.json is stale: run node tools/gen-wayland-table.mjs');
// lordos-testbench's expected globals (its --globals report) come from the same table
const hdr = fs.existsSync(TBHDR) ? fs.readFileSync(TBHDR, 'utf8') : '';
if (hdr !== expectedHeader(d)) bad('server/src/testbench/expected_globals.h is stale: run node tools/gen-wayland-table.mjs');
{
  const n = +(hdr.match(/#define TB_EXPECTED_N (\d+)/) || [, -1])[1];
  if (n !== table.size) bad(`server/src/testbench/expected_globals.h expects ${n} globals, the table offers ${table.size}`);
}
// a global offered only to some clients says so (lordos.offered), so the testbench doesn't call it missing
for (const [g, want] of [['xwayland_shell_v1', 'xwayland'], ['wp_drm_lease_device_v1', 'headset'], ['wp_linux_drm_syncobj_manager_v1', 'render-node']]) {
  const p = d.protocols.find((x) => x.lordos.globals && x.lordos.globals[g]);
  if (p && p.lordos.offered !== want) bad(`${p.id}: lordos.offered should be "${want}" (lord-server offers ${g} only then)`);
}
if (!validate({ ...d, protocols: [{ ...d.protocols.find((p) => p.id === 'xdg-shell'), lordos: { ...d.protocols.find((p) => p.id === 'xdg-shell').lordos, offered: '"><x' } }] }).length) bad('validate() accepted a hostile lordos.offered');
if (OFFERED.some((o) => !/^[a-z-]+$/.test(o))) bad('OFFERED values must be plain words (they go into a C header)');

// the conformance run (docs/wayland-conformance.json) against the table, docs/WAYLAND.md's own words and the screenshots
if (!C) bad('docs/wayland-conformance.json is missing: record a run (make -C server wl-conformance WLCONF_ARGS=--record)');
else {
  const W = webData(d, C).conformance;
  // every scored global is a LordOS global of the table at the same version, and every LordOS global was scored
  const scored = new Map(W.globals.map((g) => [g.global, g]));
  for (const g of W.globals) {
    const t = table.get(g.global);
    if (!t) bad(`docs/wayland-conformance.json scores ${g.global}, which no protocol of docs/wayland-protocols.json offers`);
    else if (g.version != null && g.version !== t.version) bad(`${g.global}: the conformance run saw version ${g.version}, docs/wayland-protocols.json says ${t.version}: record a new run`);
  }
  for (const g of table.keys()) if (!scored.has(g)) bad(`docs/wayland-protocols.json offers ${g}, but the conformance run didn't score it: record a new run`);
  // the summary agrees with the items
  const mean = (xs) => Math.round((10 * xs.reduce((a, x) => a + x.score, 0)) / (xs.length || 1)) / 10;
  if (C.summary.protocols !== W.globals.length) bad(`conformance summary: ${C.summary.protocols} protocols, ${W.globals.length} scored`);
  if (C.summary.complete !== W.globals.filter((g) => g.depth === 'complete').length) bad('conformance summary: the complete count disagrees with the items');
  if (Math.abs(C.summary.mean - mean(W.globals)) > 0.1) bad(`conformance summary: mean ${C.summary.mean}, the items give ${mean(W.globals)}`);
  if (C.summary.x11_mean != null && Math.abs(C.summary.x11_mean - mean(W.x11)) > 0.1) bad(`conformance summary: X11 mean ${C.summary.x11_mean}, the items give ${mean(W.x11)}`);
  for (const x of [...W.globals, ...W.x11]) if (!DEPTHS.includes(x.depth)) bad(`${x.global || x.id}: depth ${x.depth}`);
  // docs/WAYLAND.md's hand-written summary lines say the same numbers
  const wl = md.match(/\*\*Wayland \((\d{4}-\d{2}-\d{2})[^)]*\): (\d+) globals, mean ([\d.]+) %, (\d+) complete\.\*\*/);
  if (!wl) bad('docs/WAYLAND.md: the "**Wayland (date, …): N globals, mean M %, K complete.**" line is missing');
  else if (wl[1] !== C.date || +wl[2] !== W.summary.protocols || +wl[3] !== W.summary.mean || +wl[4] !== W.summary.complete) bad(`docs/WAYLAND.md says ${wl[0]}, the run says ${C.date}: ${W.summary.protocols} globals, mean ${W.summary.mean} %, ${W.summary.complete} complete`);
  const xl = md.match(/\*\*Xwayland \/ X11 \((\d{4}-\d{2}-\d{2})[^)]*\): (\d+) items, mean ([\d.]+) %\*\*/);
  if (!xl) bad('docs/WAYLAND.md: the "**Xwayland / X11 (date, …): N items, mean M %**" line is missing');
  else if (xl[1] !== C.date || +xl[2] !== W.summary.x11Items || +xl[3] !== W.summary.x11Mean) bad(`docs/WAYLAND.md says ${xl[0]}, the run says ${W.summary.x11Items} items, mean ${W.summary.x11Mean} %`);
  // the screenshots: each one named exists, and each one in the folder is shown
  const files = new Set(fs.readdirSync(PROOFS).filter((f) => f.endsWith('.png')));
  for (const p of W.proofs) if (!files.has(p.png)) bad(`proof ${p.id}: docs/wayland-proofs/${p.png} is missing`);
  const shown = new Set(W.proofs.map((p) => p.png));
  for (const f of files) if (!shown.has(f)) bad(`docs/wayland-proofs/${f} is in no proof (proofs.json, or x11-<item id>.png)`);
  for (const p of W.proofs.filter((x) => x.kind === 'x11')) if (!W.x11.some((i) => i.proofs.includes(p.id))) bad(`${p.png} proves no X11 item: name it x11-<item id>.png or add it to X11_PROOF_ITEM`);
}

// hostile rows (wayland.app is external input): each one must be refused by validate() (the JSON, the importer) and,
// in the site's copy, by validateWeb() (web/build.mjs refuses to build with it)
{
  const good = d.protocols.find((p) => p.id === 'xdg-shell');
  const web = webData(d, C);
  const goodWeb = web.protocols.find((p) => p.id === 'xdg-shell');
  const XSS = '"><script>alert(1)</script>';
  const cases = [
    ['source', (p) => { p.source = XSS; }],
    ['stability', (p) => { p.stability = XSS; }],
    ['id', (p) => { p.id = XSS; }],
    ['name', (p) => { p.name = XSS; }],
    ['summary', (p) => { p.summary = 'a <img src=x onerror=alert(1)>'; }],
    ['url', (p) => { p.url = 'javascript:alert(1)'; }],
    ['url host', (p) => { p.url = 'https://evil.example/protocols/x'; }],
    ['global', (p) => { p.globals = [XSS]; }],
    ['kwin version string', (p) => { p.kwin = { xdg_wm_base: '6"><script>' }; }],
    ['mutter version 0', (p) => { p.mutter = { xdg_wm_base: 0 }; }],
    ['kwin interface', (p) => { p.kwin = { [XSS]: 1 }; }],
    ['lordos status', (p) => { p.lordos.status = XSS; }],
    ['lordos version', (p) => { p.lordos.globals = { xdg_wm_base: 7.5 }; }],
    ['lordos note', (p) => { p.lordos.note = XSS; }],
    ['lordos since', (p) => { p.lordos.since = XSS; }],
  ];
  for (const [what, spoil] of cases) {
    const p = JSON.parse(JSON.stringify(good));
    spoil(p);
    if (!validate({ ...d, protocols: [p] }).length) bad(`validate() accepted a hostile ${what}`);
  }
  const webCases = [
    ['category', (p) => { p.cat = XSS; }],
    ['status', (p) => { p.lordos.status = XSS; }],
    ['kwin', (p) => { p.kwin = XSS; }],
    ['lordos version', (p) => { p.lordos.version = '7<b>'; }],
    ['url', (p) => { p.url = 'javascript:alert(1)'; }],
    ['bumped', (p) => { p.lordos.bumped = XSS; }],
  ];
  for (const [what, spoil] of webCases) {
    const p = JSON.parse(JSON.stringify(goodWeb));
    spoil(p);
    if (!validateWeb({ ...web, protocols: [p] }).length) bad(`validateWeb() accepted a hostile ${what}`);
  }
  if (validateWeb({ ...web, compared: { ...web.compared, kwin: { ...web.compared.kwin, version: XSS } } }).length === 0) bad('validateWeb() accepted a hostile KWin version');
  // the conformance fields (row, X11 item, proof, run)
  const rowCases = [
    ['conf score', (p) => { p.conf.score = '100"><b>'; }],
    ['conf score over 100', (p) => { p.conf.score = 101; }],
    ['conf depth', (p) => { p.conf.depth = XSS; }],
    ['conf points', (p) => { p.conf.points = p.conf.total + 1; }],
    ['conf global', (p) => { p.conf.globals = [XSS]; }],
    ['conf proof', (p) => { p.conf.proofs = [XSS]; }],
    ['conf string', (p) => { p.conf = XSS; }],
  ];
  for (const [what, spoil] of rowCases) {
    const p = JSON.parse(JSON.stringify(goodWeb));
    spoil(p);
    if (!validateWeb({ ...web, protocols: [p] }).length) bad(`validateWeb() accepted a hostile ${what}`);
  }
  const confCases = [
    ['commit', (c) => { c.commit = XSS; }],
    ['date', (c) => { c.date = XSS; }],
    ['tool', (c) => { c.tool = 'javascript:alert(1)'; }],
    ['xwayland version', (c) => { c.env.xwayland = XSS; }],
    ['mode', (c) => { c.env.mode = XSS; }],
    ['summary mean', (c) => { c.summary.mean = '100<'; }],
    ['x11 id', (c) => { c.x11[0].id = XSS; }],
    ['x11 title', (c) => { c.x11[0].title = XSS; }],
    ['x11 depth', (c) => { c.x11[0].depth = 'deep'; }],
    ['x11 score', (c) => { c.x11[0].score = -1; }],
    ['global row', (c) => { c.globals[0].row = 'no-such-row'; }],
    ['global name', (c) => { c.globals[0].global = XSS; }],
    ['proof png', (c) => { c.proofs[0].png = '../../etc/passwd.png'; }],
    ['proof png ext', (c) => { c.proofs[0].png = 'x.svg'; }],
    ['proof shows', (c) => { c.proofs[0].shows = XSS; }],
    ['proof kind', (c) => { c.proofs[0].kind = XSS; }],
    ['proof app', (c) => { c.proofs[0].app = XSS; }],
  ];
  for (const [what, spoil] of confCases) {
    const w2 = JSON.parse(JSON.stringify(web));
    spoil(w2.conformance);
    if (!validateWeb(w2).length) bad(`validateWeb() accepted a hostile conformance ${what}`);
  }
  if (validateWeb(web).length) bad('validateWeb() refused the real table: ' + validateWeb(web).join('; '));
  // the conformance page's copy (every check): hostile values and numbers that don't add up are refused
  if (C) {
    const det = confDetail(C);
    if (validateDetail(det, web.conformance).length) bad('validateDetail() refused the real run: ' + validateDetail(det, web.conformance).slice(0, 5).join('; '));
    const detCases = [
      ['commit', (x) => { x.commit = XSS; }],
      ['short commit', (x) => { x.commit = x.commit.slice(0, 12); }],
      ['note', (x) => { x.notes = ['a\u001b[31m']; }],
      ['global name', (x) => { x.globals[0].global = XSS; }],
      ['xml path', (x) => { x.globals[0].xml = '../../etc/passwd.xml'; }],
      ['check id', (x) => { x.globals[0].checks[0].id = XSS; }],
      ['check result', (x) => { x.globals[0].checks[0].result = 'pwned'; }],
      ['check kind', (x) => { x.globals[0].checks[0].kind = XSS; }],
      ['check where', (x) => { x.globals[0].checks[0].where = '../../../etc/shadow:1'; }],
      ['check where absolute', (x) => { x.globals[0].checks[0].where = '/etc/shadow:1'; }],
      ['check msg control', (x) => { x.globals[0].checks[0].msg = 'ok\nfake line'; }],
      ['check msg length', (x) => { x.globals[0].checks[0].msg = 'x'.repeat(5000); }],
      ['a pass turned into a fail', (x) => { const c = x.globals[0].checks.find((k) => k.result === 'pass'); c.result = 'fail'; }],
      ['a check dropped', (x) => { x.globals[0].checks.pop(); }],
      ['a global dropped', (x) => { x.globals.pop(); }],
      ['x11 id', (x) => { x.x11[0].id = XSS; }],
      ['x11 weight', (x) => { x.x11[0].checks[0].weight = 7; }],
      ['client bound', (x) => { if (x.clients[0]) x.clients[0].bound = [XSS]; else x.clients = [{ client: 'x', result: 'pass', msg: 'm', bound: [XSS] }]; }],
      ['client result', (x) => { x.clients = [{ client: 'x', result: XSS, msg: 'm', bound: [] }]; }],
    ];
    for (const [what, spoil] of detCases) {
      const x = JSON.parse(JSON.stringify(det));
      spoil(x);
      if (!validateDetail(x, web.conformance).length) bad(`validateDetail() accepted a hostile ${what}`);
    }
  }
}

// the parser itself, on a known snippet
{
  const t = codeGlobals([['x.c', 'static const struct wl_interface my_mgr_interface = {\n "org_x_mgr", 2, 0 };\nwl_global_create(dpy, &my_mgr_interface, 2, NULL, b);\n  wl_global_create(dpy, &xdg_wm_base_interface, 7, NULL, bind);\nwl_display_init_shm(dpy);\n#define FIX_VERSION 3   /* the protocol */\nwl_global_create(dpy, &wl_fixes_interface, FIX_VERSION, NULL, b);\nwl_global_create(dpy, &wp_x_v1_interface, HDR_V, NULL, b);']], ['#define HDR_V (4u)\n']);
  const want = JSON.stringify([['org_x_mgr', 2], ['xdg_wm_base', 7], ['wl_fixes', 3], ['wp_x_v1', 4], ['wl_shm', null]]);
  const got = JSON.stringify([...t].map(([k, v]) => [k, v.version]));
  if (got !== want) bad(`codeGlobals parser: ${got} != ${want}`);
}

if (fails) { console.error(`wayland_table_test: ${fails} failed`); process.exit(1); }
console.log(`wayland_table_test: ok (${code.size} globals in server/src match docs/wayland-protocols.json; generated outputs up to date)`);
