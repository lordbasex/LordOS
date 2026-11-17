// A component used as a JSX tag must be declared in its file (imported, defined or a parameter).
// GitHub #174: Disks used <Button> without importing it, so pressing Mount on a disk whose file system needed a
// package threw "Button is not defined" while rendering and (before the per-window error boundary) blanked the whole
// desktop. esbuild bundles such a file without a word, so this check reads every .jsx of the apps, themes and SDK: a
// capitalised tag name that appears nowhere else in its file (outside comments) can only be undeclared.
// Run: node server/tests/jsx_undeclared_test.mjs (from the repository root or from server/)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
let fails = 0;
const fail = (m) => { fails++; console.error('FAIL', m); };

// Comments out, strings kept (a name inside a string is no declaration, but counting it errs on the quiet side).
export function stripComments(src) {
  return src.replace(/\/\*[\s\S]*?\*\//g, ' ').replace(/(^|[^:'"`\\])\/\/[^\n]*/g, '$1');
}

// The capitalised JSX tags of a file that are never declared in it.
export function undeclaredTags(src) {
  const code = stripComments(src);
  const tags = new Set();
  for (const m of code.matchAll(/<\/?([A-Z][A-Za-z0-9_$]*)(?=[\s/>.])/g)) tags.add(m[1]);
  const out = [];
  for (const name of tags) {
    const any = code.match(new RegExp(`(?<![A-Za-z0-9_$])${name.replace(/\$/g, '\\$')}(?![A-Za-z0-9_$])`, 'g')) || [];
    const asTag = code.match(new RegExp(`</?${name.replace(/\$/g, '\\$')}(?![A-Za-z0-9_$])`, 'g')) || [];
    if (any.length === asTag.length) out.push(name);
  }
  return out.sort();
}

// the checker itself
const bad = `import { View, Text } from '@lordos/sdk';
// <Button> in a comment is not a use
export function A() { return <View><Text>x</Text>{ok ? <Button small /> : null}</View>; }`;
const good = `import { View, Button as Btn } from '@lordos/sdk';
function Row({ icon: Icon }) { return <View><Icon /><Btn /></View>; }
const Ctx = makeCtx(); export const B = () => <Ctx.Provider value={1}><Row /></Ctx.Provider>;`;
if (JSON.stringify(undeclaredTags(bad)) !== '["Button"]') fail(`checker: expected ["Button"], got ${JSON.stringify(undeclaredTags(bad))}`);
if (undeclaredTags(good).length) fail(`checker: false positive ${JSON.stringify(undeclaredTags(good))}`);
// the 0.8.8.9 Disks import line, without Button, is caught
const disks0889 = `import {
  partMountable, View, Text, Toolbar as Bar, GroupBox,
} from '@lordos/sdk';
export function Detail({ msg }) { return <View>{msg?.formats ? <View><Button small label="x" /></View> : null}<GroupBox /><Bar /></View>; }`;
if (JSON.stringify(undeclaredTags(disks0889)) !== '["Button"]') fail('checker misses the Disks case of #174');

// every .jsx the themes bundle
function walk(dir, acc) {
  let ents = [];
  try { ents = fs.readdirSync(dir, { withFileTypes: true }); } catch { return acc; }
  for (const e of ents) {
    if (e.name === 'node_modules' || e.name.startsWith('.')) continue;
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p, acc);
    else if (e.name.endsWith('.jsx')) acc.push(p);
  }
  return acc;
}
const files = ['apps', 'themes', 'sdk', 'runtime'].flatMap((d) => walk(path.join(root, d), []));
if (files.length < 50) fail(`only ${files.length} .jsx files found under ${root}`);
for (const f of files) {
  const names = undeclaredTags(fs.readFileSync(f, 'utf8'));
  if (names.length) fail(`${path.relative(root, f)}: <${names.join('>, <')}> used but never imported or declared`);
}

if (fails) { console.error(`jsx-undeclared: ${fails} failure(s)`); process.exit(1); }
console.log(`jsx-undeclared: ${files.length} .jsx files, every JSX component is declared`);
