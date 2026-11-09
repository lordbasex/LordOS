// config.js — where the Terminal's configuration lives (docs/TERMINAL.md, "Configuration file"): profiles, schemes,
// key mappings, shortcuts, the hotkey and the windows to restore, in their own file instead of settings.json.
// lord.termconf (server/src/termconf.c) reads and writes it: atomically, 0600, with five rotating backups, and
// protected from lord.fs wherever it is. settings.json keeps only the chosen place ("terminalConfigPath").
//
//   termConfig            the store: { data, path, cache, custom, readOnly, problem, reloaded }
//   loadTermConfig()      at the first use: the chosen place, the migration out of settings.json, the read
//   saveTermConfig(data)  the whole configuration (in memory only while read-only)
//   chooseLocation(path, how)  'move' (write the current configuration there) | 'use' (read the one that is there)
//   retryTermConfig()     read the chosen file again (a disk mounted, a folder put back)
import { settings, createStore } from '@lordos/sdk';

export const SCHEMA = 1;

export const termConfig = createStore({
  data: null,                // everything that used to be settings.json → "terminal"
  path: '', cache: '', custom: false,
  readOnly: false,           // the chosen file isn't there: the last good copy (the cache), and nothing is written
  problem: null,             // { code } of what went wrong (termconf.unavailable, termconf.corrupt…)
  reloaded: 0,               // goes up when the file was changed from outside and read again
});

const api = () => (typeof lord !== 'undefined' && lord.termconf && typeof lord.termconf.setPath === 'function' ? lord.termconf : null);
const fail = (code) => Object.assign(new Error(code), { code });
let mtime = 0, timer = null, loaded = false;

// the file's text → the configuration (a newer schema than this version knows is refused, never half-read)
export function parseConfig(text) {
  let o;
  try { o = JSON.parse(text); } catch (e) { throw fail('termconf.corrupt'); }
  if (!o || typeof o !== 'object' || Array.isArray(o)) throw fail('termconf.corrupt');
  const { schema, ...data } = o;
  if (typeof schema === 'number' && schema > SCHEMA) throw fail('termconf.newer');
  if (data.profiles !== undefined && !Array.isArray(data.profiles)) throw fail('termconf.corrupt');
  return data;
}
export const serializeConfig = (data) => `${JSON.stringify({ schema: SCHEMA, ...data }, null, 1)}\n`;

const chosenPath = () => { const p = settings.get().terminalConfigPath; return typeof p === 'string' ? p : ''; };

function where(patch = {}) {
  const w = api().where();
  termConfig.set((s) => ({ ...s, path: w.path, cache: w.cache, custom: w.custom, ...patch }));
}

// the chosen file isn't usable: the cache, read-only; an empty configuration only when there is no cache either
function fallBack(problem) {
  let data = {};
  try { data = parseConfig(api().read('cache').text); } catch (e) { /* nothing to fall back on */ }
  where({ data, readOnly: true, problem: { code: problem.code || 'termconf.unavailable' } });
}

// fromOutside: changed by something else (a restore, a git pull): the copy in the default place follows it, and the
// chosen file itself is left exactly as it is
function readActive(fromOutside = false) {
  const a = api();
  const r = a.read('active');
  const data = parseConfig(r.text);
  mtime = r.mtime;
  where({ data, readOnly: false, problem: null });
  if (fromOutside && a.where().custom) { try { a.writeCache(serializeConfig(data)); } catch (e) { /* the copy waits */ } }
}

export function loadTermConfig() {
  if (loaded) return;
  loaded = true;
  const a = api();
  if (!a) {                                         // no lord.termconf (an old server): as before
    termConfig.set((s) => ({ ...s, data: settings.get().terminal || {} }));
    return;
  }
  const s = settings.get();
  try { a.setPath(chosenPath()); } catch (e) { a.setPath(''); fallBack(e); watch(); return; }
  // 0.8.8.7: settings.json → "terminal" moves to terminal.json once (a backup of settings.json first, the new file
  // read back before the section leaves settings.json)
  if (s.terminal && typeof s.terminal === 'object' && !a.where().custom) {
    try {
      a.migrate(serializeConfig(s.terminal));
      settings.set({ terminal: undefined });
    } catch (e) {
      if (e.code !== 'termconf.exists') {        // not written: keep using settings.json's copy, and say so
        where({ data: s.terminal, readOnly: false, problem: { code: e.code || 'termconf.writeFailed' } });
        watch();
        return;
      }
    }
  }
  try { readActive(); } catch (e) {
    // a fresh install (no file yet) starts with the defaults; anything else falls back on the cache
    if (e.code === 'termconf.missing' && !a.where().custom) where({ data: {}, readOnly: false, problem: null });
    else fallBack(e);
  }
  watch();
}

// Changed from outside (a backup restored, a git pull): read again. While read-only, the chosen place is tried
// again now and then and used as soon as it is back.
function watch() {
  if (timer || !api()) return;
  timer = setInterval(() => {
    const st = termConfig.get();
    const a = api();
    if (st.readOnly) { try { readActive(true); termConfig.set((x) => ({ ...x, reloaded: x.reloaded + 1 })); } catch (e) { /* still away */ } return; }
    const m = a.mtime();
    if (m != null && m === mtime) return;
    if (m == null) {
      // gone: the default file is written again from memory; a chosen one's place is away (a folder renamed, a disk
      // unmounted), so the last good copy is used read-only until it's back
      if (!a.where().custom) { if (st.data) saveTermConfig(st.data); return; }
      try { readActive(); } catch (e) { fallBack(e); termConfig.set((x) => ({ ...x, data: st.data || x.data })); }
      return;
    }
    try { readActive(true); termConfig.set((x) => ({ ...x, reloaded: x.reloaded + 1 })); } catch (e) { fallBack(e); termConfig.set((x) => ({ ...x, data: st.data || x.data })); }
  }, 2000);
}

export function saveTermConfig(data) {
  loadTermConfig();
  const st = termConfig.get();
  termConfig.set((s) => ({ ...s, data }));
  const a = api();
  if (!a) { settings.set({ terminal: data }); return true; }
  if (st.readOnly) return false;                     // the chosen file is away: changes stay in memory
  try {
    mtime = a.write(serializeConfig(data));
    if (st.problem) termConfig.set((s) => ({ ...s, problem: null }));
    return true;
  } catch (e) {
    fallBack(e);
    termConfig.set((s) => ({ ...s, data }));          // what was just changed stays on screen
    return false;
  }
}

export function retryTermConfig() {
  const a = api();
  if (!a) return false;
  try { a.setPath(chosenPath()); readActive(); return true; } catch (e) { fallBack(e); return false; }
}

// path: '' (back to the default) or an absolute .json file. 'move': the configuration in memory is written there;
// 'use': the file already there is read (and must be valid). settings.json records the place only once it worked.
export function chooseLocation(path, how) {
  const a = api();
  if (!a) throw fail('termconf.unavailable');
  const was = chosenPath();
  const data = termConfig.get().data || {};
  try {
    a.setPath(path);
    if (how === 'use') readActive();
    else { mtime = a.write(serializeConfig(data)); where({ data, readOnly: false, problem: null }); }
  } catch (e) {
    try { a.setPath(was); } catch (x) { a.setPath(''); }
    throw e;
  }
  settings.set({ terminalConfigPath: path || undefined });
  // a file that was there: the copy in the default place follows it
  if (path && how === 'use') { try { mtime = a.write(serializeConfig(termConfig.get().data || {})); } catch (e) { /* read fine, the copy waits */ } }
}
