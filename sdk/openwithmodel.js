// openwithmodel.js — "Open with" providers, the pure part (sdk/openwith.js keeps the registry and the person's choices;
// docs/FILES.md, "Open with"; docs/VIRTUALIZATION.md 7.3). Tested by server/tests/openwith_model_test.mjs.
//
// A provider is something other than an installed app that can open a file: today a machine in Machines whose guest
// tools open files ("Windows 11 Pro (…)"); later LordBridge or a Wine runner, through the same shape:
//   { id, label, icon?, accepts(file), suggests?(file), available?(), needsStart?(), open(file) }
// or a hint, which opens nothing: { id, hint(file) } → why no provider opens this file yet, and what to do ("No machine
// can open it yet: update the LordOS tools…"), '' when it has nothing to say. Files shows it, disabled, under Open with.
// file is Files' entry ({ name, path, dir, … }). accepts: it can open this file (listed in Open with); suggests: it is
// the natural handler when no app here opens the file (a program for another system); available: it can be used now
// at all (a machine that still exists); needsStart: using it starts something first (an off machine), so a double
// click asks before. open never runs anything here: a provider copies the file into its own world and opens it there.
//
// The person's "Always open .exe files with …" is kept per extension (settings openWithDefaults: { exe: id }), apart
// from ~/.config/mimeapps.list, which other desktops read and where a provider's id would name no .desktop file.

export const OPEN_DEFAULTS_MAX = 64;

// the extension a choice is kept under: lower case, letters and digits, at most 16; '' when the name has none
export function extKey(name) {
  const n = String(name || '');
  const i = n.lastIndexOf('.');
  if (i <= 0 || i === n.length - 1) return '';
  const e = n.slice(i + 1).toLowerCase();
  return /^[a-z0-9]{1,16}$/.test(e) ? e : '';
}

const call = (f, ...a) => { try { return typeof f === 'function' ? f(...a) : undefined; } catch (e) { return undefined; } };
const usable = (p) => !!p && typeof p.id === 'string' && p.id && typeof p.open === 'function' && typeof p.label === 'string' && p.label;

// the providers that open this file now, in the order given, each id once: [provider]
export function providersFor(list, file) {
  if (!file || file.dir || file.app || file.trash) return [];
  const seen = new Set(), out = [];
  for (const p of list || []) {
    if (!usable(p) || seen.has(p.id)) continue;
    if (p.available && call(p.available) === false) continue;
    if (call(p.accepts, file) !== true) continue;
    seen.add(p.id);
    out.push(p);
  }
  return out;
}

// the one that suggests itself for a file no app here opens (the first), or null
export function suggestedFor(list, file) {
  return providersFor(list, file).find((p) => call(p.suggests, file) === true) || null;
}

// what to say when no provider opens this file yet: [text], each hint's id once; [] when one opens it
export function hintsFor(list, file) {
  if (!file || file.dir || file.app || file.trash || providersFor(list, file).length) return [];
  const seen = new Set(), out = [];
  for (const p of list || []) {
    if (!p || typeof p.id !== 'string' || !p.id || typeof p.hint !== 'function' || seen.has(p.id)) continue;
    const s = call(p.hint, file);
    if (typeof s !== 'string' || !s) continue;
    seen.add(p.id);
    out.push(s);
  }
  return out;
}

// the person's choice for this file's extension, when that provider still opens it; else null
export function defaultFor(defaults, list, file) {
  const k = extKey(file?.name);
  const id = k && defaults && typeof defaults === 'object' ? defaults[k] : null;
  if (typeof id !== 'string' || !id) return null;
  return providersFor(list, file).find((p) => p.id === id) || null;
}

// the choices with this extension's set (id) or forgotten (null); at most OPEN_DEFAULTS_MAX kept (the newest)
export function withDefault(defaults, ext, id) {
  const k = extKey(`x.${ext}`);
  const base = defaults && typeof defaults === 'object' && !Array.isArray(defaults) ? defaults : {};
  const out = {};
  for (const [e, v] of Object.entries(base)) if (e !== k && extKey(`x.${e}`) === e && typeof v === 'string' && v) out[e] = v;
  if (!k) return out;
  if (typeof id === 'string' && id) out[k] = id;
  const keys = Object.keys(out);
  if (keys.length > OPEN_DEFAULTS_MAX) for (const e of keys.slice(0, keys.length - OPEN_DEFAULTS_MAX)) delete out[e];
  return out;
}
