// terminal_config_test.mjs — (0.8.8.7) the Terminal's own configuration file (apps/terminal/config.js): the move out
// of settings.json (every field kept, the section removed only once the file is written), a chosen file that isn't
// there (the last good copy, read-only, nothing written), choosing a place (move or use what is there), and files
// that are refused (not JSON, a newer schema). lord.termconf is a fake over a map of files; the C side has its own
// test (tests/termconf_test.c). Bundles the real modules with esbuild over a stub '@lordos/sdk'.
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
import * as config from './apps/terminal/config.js';
import { terminalSettings, saveTerminalSettings } from './apps/terminal/profiles.js';
import { settings } from '@lordos/sdk';
globalThis.__out = { ...config, terminalSettings, saveTerminalSettings, settings };`;
const sdkStub = {
  name: 'sdk-stub',
  setup(b) {
    b.onResolve({ filter: /^@lordos\/sdk$/ }, () => ({ path: 'sdk', namespace: 'stub' }));
    b.onLoad({ filter: /.*/, namespace: 'stub' }, () => ({
      contents: `export const t = (k) => k;
        export function createStore(v) { let s = v; return { get: () => s, set: (f) => { s = typeof f === 'function' ? f(s) : { ...s, ...f }; }, subscribe: () => () => {} }; }
        export const settings = createStore(globalThis.__settings || {});`,
      loader: 'js',
    }));
  },
};
const built = (await esbuild.build({ stdin: { contents: entry, resolveDir: root, loader: 'js' }, bundle: true, write: false, format: 'iife', plugins: [sdkStub], logLevel: 'silent' })).outputFiles[0].text;
globalThis.setInterval = () => 0;      // the watcher: not in a test

const DEF = '/home/u/.config/lordos/terminal.json';
// a fresh copy of the modules over a fake lord.termconf: files { path: text }, away: folders that aren't there
function load(settingsJson, files = {}, away = []) {
  const fs = { ...files };
  let custom = '';
  const err = (code) => Object.assign(new Error(code), { code });
  const isAway = (p) => away.some((d) => p.startsWith(`${d}/`));
  const active = () => custom || DEF;
  const calls = { migrate: 0, write: 0 };
  globalThis.lord = { termconf: {
    setPath: (p) => { custom = p === DEF ? '' : p; return active(); },
    where: () => ({ path: active(), cache: DEF, custom: !!custom }),
    read: (which) => {
      const p = which === 'cache' ? DEF : active();
      if (isAway(p)) throw err('termconf.unavailable');
      if (!(p in fs)) throw err('termconf.missing');
      return { text: fs[p], mtime: 1 };
    },
    write: (text) => { calls.write++; if (isAway(active())) throw err('termconf.unavailable'); fs[active()] = text; if (custom) fs[DEF] = text; return 2; },
    writeCache: (text) => { if (custom) fs[DEF] = text; return true; },
    mtime: () => (active() in fs ? 1 : null),
    migrate: (text) => { calls.migrate++; if (DEF in fs) throw err('termconf.exists'); fs[DEF] = text; return true; },
  } };
  globalThis.__settings = settingsJson;
  new Function(built)();
  return { ...globalThis.__out, fs, calls };
}

let n = 0, bad = 0;
const eq = (what, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${what}\n    got  ${g}\n    want ${w}`); }
};

// 1. the move out of settings.json: every profile and field, the section gone afterwards
{
  const terminal = { profiles: [{ id: 'default', name: '' }, { id: 'p1', name: 'Server', itermGuid: 'ABC-123', initialText: 'ssh host' }],
    defaultProfile: 'default', hotkey: 'Super+`', schemes: { mine: { name: 'Mine' } }, shortcuts: { copy: ['Ctrl+C'] }, keyMap: [{ combo: 'Ctrl+K', action: 'text', param: 'x' }], live: [{ x: 1 }] };
  const m = load({ language: 'en', terminal });
  const t = m.terminalSettings();
  eq('migrated: profiles', t.profiles.map((p) => [p.id, p.name, p.itermGuid]), [['default', '', ''], ['p1', 'Server', 'ABC-123']]);
  eq('migrated: hotkey, schemes, shortcuts, keyMap, live', [t.hotkey, t.schemes.mine.name, t.shortcuts.copy, t.keyMap.length, t.live.length], ['Super+`', 'Mine', ['Ctrl+C'], 1, 1]);
  const file = JSON.parse(m.fs[DEF]);
  eq('terminal.json: schema 1 and the itermGuid', [file.schema, file.profiles[1].itermGuid, file.profiles[1].initialText], [1, 'ABC-123', 'ssh host']);
  eq('settings.json: the section is gone, the rest stays', [m.settings.get().terminal, m.settings.get().language], [undefined, 'en']);
  m.saveTerminalSettings({ hotkey: 'F12' });
  eq('a change goes to terminal.json', JSON.parse(m.fs[DEF]).hotkey, 'F12');
  eq('and not back to settings.json', m.settings.get().terminal, undefined);
}

// 2. terminal.json already there: it wins, the migration doesn't overwrite it
{
  const m = load({ terminal: { profiles: [{ id: 'old' }] } }, { [DEF]: JSON.stringify({ schema: 1, profiles: [{ id: 'kept' }] }) });
  eq('existing file wins', m.terminalSettings().profiles.map((p) => p.id), ['kept']);
  eq('migration was refused, nothing overwritten', JSON.parse(m.fs[DEF]).profiles[0].id, 'kept');
}

// 3. a fresh install: the defaults, nothing written until something changes
{
  const m = load({});
  eq('fresh: the default profile', m.terminalSettings().profiles.map((p) => p.id), ['default']);
  eq('fresh: nothing written', m.calls.write, 0);
}

// 4. the chosen file's folder is away: the last good copy, read-only, nothing written there or anywhere
{
  const chosen = '/home/u/Backups/LordOS-Terminal/terminal.json';
  const m = load({ terminalConfigPath: chosen }, { [DEF]: JSON.stringify({ schema: 1, profiles: [{ id: 'cached', name: 'From the cache' }] }) },
    ['/home/u/Backups/LordOS-Terminal']);
  eq('away: profiles from the cache', m.terminalSettings().profiles.map((p) => p.name), ['From the cache']);
  eq('away: read-only, with the reason', [m.termConfig.get().readOnly, m.termConfig.get().problem.code], [true, 'termconf.unavailable']);
  eq('away: a change is not saved', m.saveTerminalSettings({ hotkey: 'off' }), false);
  eq('away: nothing written', [m.calls.write, m.fs[chosen]], [0, undefined]);
  eq('away: the change is still on screen', m.terminalSettings().hotkey, 'off');
  eq('away: the chosen place is still the setting', m.settings.get().terminalConfigPath, chosen);
}

// 5. choosing a place: move there (and the setting follows), use what is there, back to the default
{
  const there = '/home/u/Backups/other.json';
  const m = load({}, { [DEF]: JSON.stringify({ schema: 1, profiles: [{ id: 'a' }] }), [there]: JSON.stringify({ schema: 1, profiles: [{ id: 'theirs' }] }) });
  m.terminalSettings();
  const moved = '/home/u/Backups/moved.json';
  m.chooseLocation(moved, 'move');
  eq('move: written there', JSON.parse(m.fs[moved]).profiles[0].id, 'a');
  eq('move: the setting', m.settings.get().terminalConfigPath, moved);
  m.chooseLocation(there, 'use');
  eq('use: the file that is there', m.terminalSettings().profiles.map((p) => p.id), ['theirs']);
  eq('use: the cache follows it', JSON.parse(m.fs[DEF]).profiles[0].id, 'theirs');
  m.chooseLocation('', 'move');
  eq('back to the default: setting removed', m.settings.get().terminalConfigPath, undefined);
  eq('back to the default: the configuration kept', JSON.parse(m.fs[DEF]).profiles[0].id, 'theirs');
}

// 6. using a file that isn't valid: refused, and the place doesn't change
{
  const bad1 = '/home/u/bad.json', newer = '/home/u/newer.json';
  const m = load({}, { [bad1]: 'not json', [newer]: JSON.stringify({ schema: 99, profiles: [] }) });
  m.terminalSettings();
  let code = null;
  try { m.chooseLocation(bad1, 'use'); } catch (e) { code = e.code; }
  eq('not JSON refused', [code, m.settings.get().terminalConfigPath], ['termconf.corrupt', undefined]);
  try { m.chooseLocation(newer, 'use'); } catch (e) { code = e.code; }
  eq('newer schema refused', code, 'termconf.newer');
  eq('parse: profiles must be a list', (() => { try { m.parseConfig('{"profiles":{}}'); return null; } catch (e) { return e.code; } })(), 'termconf.corrupt');
}

console.log(`terminal_config_test: ${n - bad}/${n} checks`);
process.exit(bad ? 1 : 0);
