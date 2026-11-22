// wayland_import_test.mjs — tools/wayland-app-import.mjs reads wayland.app's bundle (untrusted third-party text) as
// literals and never runs it: string escapes decode as JavaScript does, the registry's enum members and module
// references resolve, and anything that would be code (a call, an operator, a string breakout) is refused.
import { readString, readLiteral, moduleJson, parse } from '../../tools/wayland-app-import.mjs';

let ok = 0, bad = 0;
const eq = (name, a, b) => { const p = JSON.stringify(a) === JSON.stringify(b); p ? ok++ : (bad++, console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`)); };
const throws = (name, f) => { try { f(); bad++; console.log(`FAIL ${name}: no error`); } catch (e) { ok++; } };
globalThis.__pwned = false;

eq('plain string', readString(`'abc'`, 0).value, 'abc');
eq('escapes', readString(String.raw`"a\n\t\"\\\x41\u0042\u{1F600}"`, 0).value, 'a\n\t"\\AB\u{1F600}');
eq('escaped quote stays inside', readString(String.raw`'it\'s'`, 0).value, "it's");
// F2: an escaped backslash before the quote ends the string; what follows is never run
const brk = String.raw`'x\\'+(globalThis.__pwned=true)//'`;
eq('escaped backslash ends the string', readString(brk, 0), { value: 'x\\', end: 5 });
throws('octal escape', () => readString(String.raw`'\1'`, 0));
throws('unterminated', () => readString(`'abc`, 0));

const enums = { a: { CORE: 'core' }, b: { STABLE: 'stable' } };
eq('registry literal', readLiteral(`[{id:"wayland",name:"Wayland",source:a.CORE,stability:b.STABLE,protocol:n(123),deprecated:[],x:!0,y:!1,z:void 0,w:-1.5e2}]`, 0, enums).value,
  [{ id: 'wayland', name: 'Wayland', source: 'core', stability: 'stable', protocol: { mod: 123 }, deprecated: [], x: true, y: false, w: -150 }]);
// F1: code inside the registry is refused, never run
throws('call in a value', () => readLiteral(`[{name:process.getBuiltinModule("child_process")}]`, 0, enums));
throws('expression in a value', () => readLiteral(`[{name:"a"+(globalThis.__pwned=true)}]`, 0, enums));
throws('unknown enum', () => readLiteral(`[{source:a.EVIL}]`, 0, enums));
throws('template', () => readLiteral('[`x`]', 0, enums));
throws('call with code', () => readLiteral(`[n(globalThis.__pwned=true)]`, 0, enums));
throws('__proto__ key', () => readLiteral(`{__proto__:{polluted:1}}`, 0, enums));
throws('deep nesting', () => readLiteral('['.repeat(100) + ']'.repeat(100), 0, enums));

// a small bundle shaped like wayland.app's: the registry, an enum, modules; and the same with a breakout module
const mod = (id, json) => `${id}:function(e){"use strict";e.exports=JSON.parse('${json}')},`;
const kw = JSON.stringify({ version: '6.7.0', generationTimestamp: 1, globals: [{ interface: 'wl_compositor', version: 6 }] });
const mu = JSON.stringify({ version: '51.0', generationTimestamp: 1, globals: [{ interface: 'wl_compositor', version: 6 }] });
const xml = JSON.stringify({ description: { summary: 'core' }, interfaces: [{ name: 'wl_display' }, { name: 'wl_compositor' }] });
const enumSrc = 'function(e){e.CORE="core",e.WP="wayland-protocols"}(a||(a={}));function(e){e.STABLE="stable"}(b||(b={}));';
const comps = '{id:"mutter",name:"Mutter",icon:"m.svg",info:r(2)},{id:"kwin",name:"KWin",icon:"k.svg",info:r(3)}';
const body = (m1) => `${enumSrc}var x=[{id:"wayland",name:"Wayland",source:a.CORE,stability:b.STABLE,protocol:n(1)}];${comps};${m1}${mod(2, mu)}${mod(3, kw)}`;
const r = parse(body(mod(1, xml)));
eq('parse: one protocol', r.protocols.map((p) => [p.id, p.source, p.stability, p.roots]), [['wayland', 'core', 'stable', ['wl_compositor']]]);
eq('parse: kwin', r.comps.kwin.version, '6.7.0');
// F2 in a module: the string ends at the escaped backslash, the rest is not JSON and is refused, nothing runs
throws('module breakout refused', () => moduleJson(`${mod(9, String.raw`x\\'+(globalThis.__pwned=true)//`)}`, 9));
throws('registry with code refused', () => parse(body(mod(1, xml)).replace('name:"Wayland"', 'name:(globalThis.__pwned=true)')));
eq('nothing ran', globalThis.__pwned, false);

console.log(`wayland-import: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
