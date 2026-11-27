// themec_test.mjs — LordOS's theme compiler (tools/themec/themec.js; docs/THEMES-GUIDE.md "Community themes"), the
// same file lord-themec runs in its sandbox: the build (JSX, imports and exports, the module table) run against a fake
// host, and what the checks refuse. Run from server/: node tests/themec_test.mjs
import assert from 'node:assert/strict';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadThemec, readTheme } from './community_theme_install.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const C = loadThemec();
let n = 0;
const ok = (what) => { n++; console.log(`ok   ${what}`); };
const manifest = JSON.stringify({ name: 'T', description: 'A test.', community: true, author: 'Tests', license: 'MIT', version: '1.0.0', capabilities: ['battery'] });
const theme = (files, id = 'tt') => C.compile({ id, files: Object.entries({ 'theme.json': manifest, ...files }).map(([name, text]) => ({ name, size: text.length, text })) });

// a fake host: createElement records its calls; startShell keeps the element
function run(code) {
  const calls = [];
  let shell = null;
  const react = { createElement: (type, props, ...kids) => ({ type, props, kids }), Fragment: 'FRAG', useState: (x) => [x, () => {}] };
  const sdk = { startShell: (el) => { shell = el; }, View: 'View', Text: 'Text', t: (k) => `T:${k}` };
  const ctx = { __lordThemeSdk: { react, sdk, apps: [{ id: 'files' }] }, calls };
  vm.createContext(ctx);
  vm.runInContext(code, ctx);
  return shell;
}

// ---- the build ----
{
  const r = theme({
    'index.jsx': `import { startShell, View, Text, t } from '@lordos/sdk';
import { apps } from '@lordos/apps';
import React, { useState } from 'react';
import Bar, { size, double as twice } from './parts/bar.jsx';
import * as data from './data.json';
const n = apps.length;
function App({ label }) {
  const [x] = useState(2);
  return <View style={{ gap: twice(x) }} a-b="c" {...{ spread: 1 }}>
    {/* a comment */}
    <Bar size={size} />
    <Text>{t('theme-tt.hello')}</Text>
    <>{label}{n}{data.default.k}</>
  </View>;
}
startShell(<App label="x" />);`,
    'parts/bar.jsx': `import { View } from '@lordos/sdk';
export const size = 3;
export function double(v) { return v * 2; }
export default function Bar(p) { return <View width={p.size} disabled />; }`,
    'data.json': '{"k": 7}',
    'locales/en.json': '{"hello": "Hello"}',
  });
  assert.equal(r.report.errors, 0, JSON.stringify(r.report.findings));
  const el = run(r.code);
  assert.equal(typeof el.type, 'function');
  assert.equal(el.props.label, 'x');
  const out = el.type(el.props);
  assert.equal(out.type, 'View');
  assert.equal(out.props.style.gap, 4);
  assert.equal(out.props['a-b'], 'c');
  assert.equal(out.props.spread, 1);
  assert.equal(out.kids.length, 3, 'the comment child is dropped');
  assert.equal(out.kids[0].props.size, 3);
  assert.equal(JSON.stringify(out.kids[2].kids), JSON.stringify(['x', 1, 7]));
  assert.equal(out.kids[2].type, 'FRAG');
  const bar = out.kids[0].type(out.kids[0].props);
  assert.equal(bar.props.width, 3);
  assert.equal(bar.props.disabled, true);
  assert.equal(JSON.stringify(r.keep.sort()), JSON.stringify(['locales/en.json', 'theme.json']));
  assert.equal(JSON.stringify(r.report.meta.capabilities), JSON.stringify(['battery']));
  ok('JSX, fragments, spreads, comments, default/named/renamed/namespace imports, JSON and the module table');
}
{
  const r = theme({ 'index.jsx': `import { startShell, Text } from '@lordos/sdk';
const el = <Text>
  one
  two   &amp; three
</Text>;
startShell(el);` });
  assert.equal(r.report.errors, 1, 'visible text is refused by the checks');
  const u = C.compile({ id: 'tt', files: [{ name: 'theme.json', size: 2, text: manifest }, { name: 'index.jsx', size: 1, text: `import { startShell, Text } from '@lordos/sdk';\nstartShell(<Text>\n  one\n  two   &amp; three\n</Text>);` }] }, { checks: false });
  const el = run(u.code);
  assert.equal(JSON.stringify(el.kids), JSON.stringify(['one two   & three']));
  ok('JSX text: lines trimmed and joined, entities decoded; untranslated text refused by the checks');
}

// ---- what the checks and the build refuse ----
const refused = [
  ['eval', 'eval("1")', 'code.eval'],
  ['Function', 'new Function("return 1")', 'code.function'],
  ['globalThis', 'globalThis.lord', 'code.globalThis'],
  ['a name starting with __', 'const __lordThemeSdk = 1', 'code.internal'],
  ['constructor', '(() => {}).constructor', 'code.prototype'],
  ['Reflect', 'Reflect.get({}, "a")', 'code.patch'],
  ['the network', 'fetch("x")', 'code.network'],
  ['lord.term', 'lord.term.create()', 'lord.forbidden'],
  ['lord.fs', 'lord.fs.write("/tmp/x", "y")', 'lord.forbidden'],
  ['lord.admin', 'lord.admin.request("x")', 'lord.forbidden'],
  ['lord.perms', 'lord.perms.set("a", "camera", "allow")', 'lord.forbidden'],
  ['lord[...]', 'lord["fs"]', 'lord.dynamic'],
  ['Symbol.for', 'Symbol.for("react.transitional.element")', 'code.symbol'],
  ['Proxy', 'new Proxy({}, {})', 'code.proxy'],
  ['a remote URL', 'const u = "https://example.com/x"', 'code.url'],
  ['a host element', 'const e = <view />', 'build'],
  ['another package', 'import x from "left-pad"', 'build'],
  ['a file outside the theme', 'import x from "../../etc/x.js"', 'build'],
  ['an image outside the theme', 'const i = <Image src="../x.png" />', 'code.path'],
  ['dynamic import()', 'import("./x.js")', 'code.dynamicImport'],
  ['export *', 'export * from "./x.js"', 'build'],
];
for (const [what, line, rule] of refused) {
  const r = theme({ 'index.jsx': `import { startShell } from '@lordos/sdk';\n${line};\nstartShell(null);\n` });
  assert.ok(!r.code && r.report.findings.some((f) => f.rule === rule), `${what}: ${JSON.stringify(r.report.findings)}`);
}
ok(`${refused.length} refusals (eval, Function, globalThis, __ names, constructor, Reflect, network, lord.term/fs/admin/perms, lord[…], Symbol.for, Proxy, URLs, host elements, other packages, outside files, import(), export *)`);
{
  const bad = [
    [{ 'index.jsx': 'x', 'theme.json': '{"name":"T"}' }, 'manifest.community'],
    [{ 'index.jsx': 'x', 'theme.json': JSON.stringify({ ...JSON.parse(manifest), capabilities: ['terminal'] }) }, 'manifest.capabilities'],
    [{ 'index.jsx': 'x', 'evil.sh': 'rm -rf ~' }, 'files.type'],
    [{ 'index.jsx': 'x', 'a/../../b.js': 'x' }, 'files.name'],
  ];
  for (const [files, rule] of bad) {
    const r = C.compile({ id: 'tt', files: Object.entries({ 'theme.json': manifest, ...files }).map(([name, text]) => ({ name, size: text.length, text })) });
    assert.ok(!r.code && r.report.findings.some((f) => f.rule === rule), `${rule}: ${JSON.stringify(r.report.findings)}`);
  }
  assert.ok(C.compile({ id: '../x', files: [] }).report.findings.some((f) => f.rule === 'id'));
  ok('the manifest (community, capabilities), file types, names with "..", the id');
}
{
  // (Meridian review, 0.8.9.6.2) a parent import that stays inside the theme is the kit's documented layout: allowed
  const r = theme({
    'index.jsx': `import { startShell } from '@lordos/sdk';\nimport Bar from './components/Bar.jsx';\nstartShell(<Bar />);\n`,
    'components/Bar.jsx': `import { View } from '@lordos/sdk';\nimport { k } from '../tokens.js';\nexport default function Bar() { return <View style={{ height: k }} />; }\n`,
    'tokens.js': 'export const k = 3;\n',
  });
  assert.equal(r.report.errors, 0, JSON.stringify(r.report.findings));
  const bad = theme({
    'index.jsx': `import { startShell } from '@lordos/sdk';\nimport Bar from './components/Bar.jsx';\nstartShell(<Bar />);\n`,
    'components/Bar.jsx': `import { k } from '../../tokens.js';\nexport default function Bar() { return k; }\n`,
  });
  assert.ok(!bad.code && bad.report.findings.some((f) => /outside the theme/.test(f.message)), JSON.stringify(bad.report.findings));
  ok('a parent import inside the theme builds; one that leaves it is refused');
}
{
  const r = C.compile({ id: 'okminimal', files: readTheme(path.join(here, 'community_themes', 'okminimal')) });
  assert.equal(r.report.errors, 0, JSON.stringify(r.report.findings));
  assert.ok(r.code.length > 1000);
  ok('the okminimal fixture builds');
  const p = C.compile({ id: 'probe', files: readTheme(path.join(here, 'community_themes', 'probe')) });
  assert.ok(!p.code && p.report.errors > 5, 'the probe fixture is refused by the checks');
  ok(`the probe fixture is refused by the checks (${p.report.errors} errors)`);
}
console.log(`themec: ${n} groups ok`);
