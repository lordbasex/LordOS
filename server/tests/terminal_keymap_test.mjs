// terminal_keymap_test.mjs — (0.8.8.6) the Terminal's key mappings: iTerm2's numeric keypad entries are not
// imported onto the main keys (on a Latin American keyboard the "-" key typed "/"), "\n" in a text mapping is
// a line feed as in iTerm2 (Claude Code takes it as a new line; the shell as Enter), and settings migration 2
// drops the mappings that only repeat what a US keyboard types. Bundles the real modules with esbuild over a
// stub '@lordos/sdk'. Needs node and the repository's node_modules (esbuild).
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const nodePaths = [path.join(root, 'node_modules'), ...(process.env.NODE_PATH || '').split(':').filter(Boolean)];
let esbuild = null;
for (const p of nodePaths) { try { esbuild = createRequire(path.join(p, 'x.js'))('esbuild'); break; } catch (e) { /* next */ } }
if (!esbuild) { console.log('needs node_modules (esbuild)'); process.exit(1); }

const entry = `
import { itermCombo, itermKeyMap } from './apps/terminal/iterm.js';
import { mappingBytes, actionOf } from './apps/terminal/keys.js';
import { migrateSettings, usNoopMapping } from './sdk/settings.js';
globalThis.__out = { actionOf, itermCombo, itermKeyMap, mappingBytes, migrateSettings, usNoopMapping };`;
const sdkStub = {
  name: 'sdk-stub',
  setup(b) {
    b.onResolve({ filter: /^@lordos\/sdk$/ }, () => ({ path: 'sdk', namespace: 'stub' }));
    b.onLoad({ filter: /.*/, namespace: 'stub' }, () => ({
      contents: `export const t = (k) => k;
        export function createStore(v) { let s = v; return { get: () => s, set: (f) => { s = typeof f === 'function' ? f(s) : f; }, subscribe: () => () => {} }; }
        export const useStore = () => undefined;
        export const settings = createStore({});`,
      loader: 'js',
    }));
  },
};
globalThis.lord = new Proxy({ settings: { load: () => '{}', save: () => true } }, { get: (o, k) => (k in o ? o[k] : () => undefined) });
const r = await esbuild.build({ stdin: { contents: entry, resolveDir: root, loader: 'js' }, bundle: true, write: false, format: 'iife', plugins: [sdkStub], logLevel: 'silent' });
new Function(r.outputFiles[0].text)();
const { actionOf, itermCombo, itermKeyMap, mappingBytes, migrateSettings, usNoopMapping } = globalThis.__out;

let n = 0, bad = 0;
const eq = (what, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${what}\n    got  ${g}\n    want ${w}`); }
};

// iTerm2's keypad: "-" (0x2d), "/" (0x2f), "*" (0x2a), keypad Enter (0x03), with NSNumericPadKeyMask
eq('keypad "-" is not imported', itermCombo('0x2d-0x200000'), null);
eq('keypad "/" is not imported', itermCombo('0x2f-0x200000'), null);
eq('keypad "*" is not imported', itermCombo('0x2a-0x200000'), null);
eq('keypad Enter is not imported', itermCombo('0x3-0x200000'), null);
eq('the arrows carry the flag and stay', itermCombo('0xf700-0x220000'), 'Shift+ArrowUp');
eq('the main "-" stays', itermCombo('0x2d-0x40000'), 'Ctrl+-');
const warnings = [];
eq('a map with keypad entries', itermKeyMap({
  '0x2d-0x200000': { Action: 12, Text: '-' }, '0xf702-0x280000': { Action: 10, Text: 'b' }, '0xd-0x20000': { Action: 12, Text: '\\n' },
}, warnings, 'p').map((m) => m.combo), ['Alt+ArrowLeft', 'Shift+Enter']);

eq('"\\n" is a line feed', mappingBytes({ action: 'text', param: 'a\\nb' }), 'a\nb');
eq('"\\r" is a carriage return', mappingBytes({ action: 'text', param: 'x\\r' }), 'x\r');

eq('"-" sending "-" repeats the US key', usNoopMapping({ combo: '-', action: 'text', param: '-' }), true);
eq('Shift+8 sending "*" repeats the US key', usNoopMapping({ combo: 'Shift+8', action: 'text', param: '*' }), true);
eq('Shift+Enter sending a new line is kept', usNoopMapping({ combo: 'Shift+Enter', action: 'text', param: '\\n' }), false);
eq('"-" sending something else is kept', usNoopMapping({ combo: '-', action: 'text', param: '_' }), false);
eq('a hex mapping is kept', usNoopMapping({ combo: 'Ctrl+2', action: 'hex', param: '0x00' }), false);

const before = {
  schema: 1,
  terminal: {
    profiles: [{ id: 'a', keyMap: [{ combo: '-', action: 'text', param: '-' }, { combo: '/', action: 'text', param: '/' }, { combo: 'Ctrl+2', action: 'hex', param: '0x00' }] }, { id: 'b' }],
    keyMap: [{ combo: 'Shift+Enter', action: 'text', param: '\\n' }, { combo: '5', action: 'text', param: '5' }],
  },
};
const after = migrateSettings(before);
eq('migration 2: the profile keeps only its real mappings', after.terminal.profiles[0].keyMap.map((m) => m.combo), ['Ctrl+2']);
eq('migration 2: a profile without mappings is untouched', after.terminal.profiles[1], { id: 'b' });
eq('migration 2: the global mappings too', after.terminal.keyMap.map((m) => m.combo), ['Shift+Enter']);
eq('migrations: the schema', after.schema, 3);
eq('migrations: settings without a terminal', migrateSettings({ schema: 1, x: 1 }), { schema: 3, x: 1 });

// migration 3: a default profile that types a command on open
const ssh = { id: 'p1', name: 'TURN NAT', initialText: 'ssh ec2-user@203.0.113.7' };
let m3 = migrateSettings({ schema: 2, terminal: { profiles: [ssh], defaultProfile: 'p1' } }).terminal;
eq('migration 3: a plain Default profile is added and made the default', [m3.defaultProfile, m3.profiles.map((p) => p.id)], ['default', ['default', 'p1']]);
m3 = migrateSettings({ schema: 2, terminal: { profiles: [ssh, { id: 'work', name: 'Work' }], defaultProfile: 'p1' } }).terminal;
eq('migration 3: or an existing plain profile becomes the default', [m3.defaultProfile, m3.profiles.length], ['work', 2]);
m3 = migrateSettings({ schema: 2, terminal: { profiles: [{ id: 'default' }, ssh], defaultProfile: 'default' } }).terminal;
eq('migration 3: a plain default is left alone', m3.defaultProfile, 'default');

// zoom by the character (evdev codes: 12 is US "-", 53 US "/", 27 US "]", 13 US "=")
eq('❖ and the Latin American "-" key (US "/")', actionOf({ meta: true, key: '-', code: 53 }), 'zoomOut');
eq('❖ and the Latin American "+" key (US "]")', actionOf({ meta: true, key: '+', code: 27 }), 'zoomIn');
eq('❖ and the US "=" key', actionOf({ meta: true, key: '=', code: 13 }), 'zoomIn');
eq('❖ and the US "-" key', actionOf({ meta: true, key: '-', code: 12 }), 'zoomOut');
eq('Ctrl+Shift and "+"', actionOf({ ctrl: true, shift: true, key: '+', code: 13 }), 'zoomIn');
eq('❖] is still the next pane on a US keyboard', actionOf({ meta: true, key: ']', code: 27 }), 'nextPane');
eq('Ctrl+- alone is not zoom (it is ^_ for the shell)', actionOf({ ctrl: true, key: '-', code: 12 }), null);

console.log(`terminal_keymap_test: ${n - bad}/${n} checks`);
process.exit(bad ? 1 : 0);
