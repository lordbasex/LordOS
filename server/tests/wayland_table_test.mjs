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
import { load, validate, validateWeb, markdown, withSection, webData } from '../../tools/gen-wayland-table.mjs';

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
const md = fs.readFileSync(path.join(REPO, 'docs/WAYLAND.md'), 'utf8');
if (withSection(md, markdown(d)) !== md) bad('docs/WAYLAND.md\'s generated protocol table is stale: run node tools/gen-wayland-table.mjs');
const webFile = path.join(REPO, 'web/content/wayland/protocols.json');
const web = fs.existsSync(webFile) ? fs.readFileSync(webFile, 'utf8') : '';
if (web !== JSON.stringify(webData(d), null, 1) + '\n') bad('web/content/wayland/protocols.json is stale: run node tools/gen-wayland-table.mjs');

// hostile rows (wayland.app is external input): each one must be refused by validate() (the JSON, the importer) and,
// in the site's copy, by validateWeb() (web/build.mjs refuses to build with it)
{
  const good = d.protocols.find((p) => p.id === 'xdg-shell');
  const web = webData(d);
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
  if (validateWeb(web).length) bad('validateWeb() refused the real table: ' + validateWeb(web).join('; '));
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
