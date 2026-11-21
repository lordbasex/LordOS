// openwith.js — "Open with" providers (sdk/openwithmodel.js has the shape and the rules; docs/FILES.md, "Open with").
// Files lists them after the apps that open a file; an app or a system part registers them:
//
//   registerOpenProvider(provider)          one provider → unregister()
//   registerOpenProviders(source, list)     a set that changes (Machines: one per machine whose guest tools open
//                                           files); list() is asked each time a menu is built → unregister()
//   openProvidersFor(file)                  [provider] that open this file now
//   openHintsFor(file)                      [text]: why none opens it yet (a hint provider: { id, hint(file) })
//   suggestedProviderFor(file)              the one to offer when no app here opens it (a program for another system)
//   defaultProviderFor(file)                the person's "Always open .ext files with …", or null
//   setDefaultProvider(file, id | null)     kept in settings (openWithDefaults, per extension)
//
// Only the session's own code registers providers (themes and native apps): an isolated app has no way in.
import { settings } from './settings.js';
import { providersFor, hintsFor, suggestedFor, defaultFor, withDefault, extKey } from './openwithmodel.js';

const sources = new Map();          // source id → () => [provider]

export function registerOpenProviders(source, list) {
  const key = `set:${source}`;
  sources.set(key, list);
  return () => { if (sources.get(key) === list) sources.delete(key); };
}
export function registerOpenProvider(p) {
  const key = `one:${p?.id}`;
  const f = () => [p];
  sources.set(key, f);
  return () => { if (sources.get(key) === f) sources.delete(key); };
}

function all() {
  const out = [];
  for (const f of sources.values()) {
    let l = [];
    try { l = f() || []; } catch (e) { l = []; }
    if (Array.isArray(l)) out.push(...l);
  }
  return out;
}

const choices = () => settings.get().openWithDefaults || {};
export const openProvidersFor = (file) => providersFor(all(), file);
export const openHintsFor = (file) => hintsFor(all(), file);
export const suggestedProviderFor = (file) => suggestedFor(all(), file);
export const defaultProviderFor = (file) => defaultFor(choices(), all(), file);
export function setDefaultProvider(file, id) {
  const k = extKey(file?.name);
  if (!k) return false;
  settings.set({ openWithDefaults: withDefault(choices(), k, id) });
  return true;
}
export { extKey as openExtOf };
