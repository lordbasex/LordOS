// store.js — where Machines keeps the person's profiles, systems and downloaded logos (docs/VIRTUALIZATION.md
// "Profiles and your own systems", Storage): ~/.config/lordos/machines/profiles/<id>.json (one profile each, the same
// JSON as a .lordprofile plus its id), ~/.config/lordos/machines/systems.json (the systems added by the person) and
// ~/.cache/lordos/machines/logos/<sha256>.png (logos, only with the switch on). Written with lord.fs as the person:
// none of them launches anything or holds a permission, so they aren't in fsops_protected; what they hold is checked
// again each time it is read (profiles.js readProfile, systems.js readSystems), and an image is hashed again before a
// machine is made from it.
import { fs, settings, createStore, t as tr } from '@lordos/sdk';
import { readProfile, writeProfile, PROFILES_MAX } from './profiles.js';
import { readSystems, writeSystems, LOGOS_DIR, logoFile, shaOf } from './systems.js';

const home = () => fs.home();
export const machinesConf = () => `${home()}/.config/lordos/machines`;
export const profilesDir = () => `${machinesConf()}/profiles`;
export const systemsFile = () => `${machinesConf()}/systems.json`;
export const logosDir = () => `${home()}/${LOGOS_DIR}`;

// what changed last: windows of the app read again when it moves
export const storeRev = createStore(0);
const bump = () => storeRev.set((n) => n + 1);

function ensure(dir) {
  const parts = dir.slice(home().length + 1).split('/');
  let p = home();
  for (const x of parts) {
    p = `${p}/${x}`;
    try { fs.stat(p); } catch (e) { fs.mkdir(p); }
  }
}

export function loadProfiles() {
  let entries = [];
  try { entries = fs.list(profilesDir()) || []; } catch (e) { return []; }
  const out = [];
  for (const e of entries) {
    if (out.length >= PROFILES_MAX) break;
    if (e.dir || !/^u-[0-9a-f]{8}\.json$/.test(e.name || '') || (e.size || 0) > 65536) continue;
    try {
      const r = readProfile(fs.read(`${profilesDir()}/${e.name}`), { stored: true });
      if (r.ok && `${r.profile.id}.json` === e.name) out.push(r.profile);
    } catch (x) { /* unreadable: left out */ }
  }
  return out;
}
export function saveProfile(p) {
  ensure(profilesDir());
  fs.write(`${profilesDir()}/${p.id}.json`, writeProfile(p, [], { withId: true }));
  bump();
}
// moved to the Trash and taken out of it at once: gone, as a delete
function remove(path) {
  fs.trash(path);
  try { const t = (fs.trashList() || []).find((x) => x.original === path); if (t) fs.removeFromTrash(t.id); } catch (e) { /* stays in the Trash */ }
}
export function deleteProfile(id) {
  if (!/^u-[0-9a-f]{8}$/.test(id)) return;
  remove(`${profilesDir()}/${id}.json`);
  if (settings.get().machinesDefaults?.profile === id) setDefaults({ profile: null });
  bump();
}

export function loadSystems() {
  try { return readSystems(fs.read(systemsFile()), { home: home() }); } catch (e) { return []; }
}
export function saveSystems(list) {
  ensure(machinesConf());
  fs.write(systemsFile(), writeSystems(list));
  bump();
}

// settings.machinesDefaults (0.8.9): { profile, bridge, logos, toolsAuto } — the default profile, the card bridged machines join,
// the official logos switch (off unless the person turned it on) and the LordOS tools updated by themselves (on unless
// turned off; a machine's Configure → Agent can say otherwise, model.js toolsAutoOn)
export const defaultsOf = (v) => {
  const o = v && typeof v === 'object' ? v : {};
  return { profile: typeof o.profile === 'string' ? o.profile : null, bridge: typeof o.bridge === 'string' ? o.bridge : '', logos: o.logos === true,
    toolsAuto: o.toolsAuto !== false };
};
export function setDefaults(patch) {
  settings.set({ machinesDefaults: { ...defaultsOf(settings.get().machinesDefaults), ...patch } });
}

// a logo already kept here (its PNG, re-encoded), or ''
export function logoPath(sha) {
  const f = logoFile(home(), sha);
  if (!f) return '';
  try { return fs.stat(f) ? f : ''; } catch (e) { return ''; }
}
// Remove downloaded logos: every PNG of the folder
export function removeLogos() {
  let entries = [];
  try { entries = fs.list(logosDir()) || []; } catch (e) { return 0; }
  let n = 0;
  for (const e of entries) {
    if (e.dir || !/^[0-9a-f]{64}\.png$/.test(e.name || '')) continue;
    try { remove(`${logosDir()}/${e.name}`); n++; } catch (x) { /* left */ }
  }
  bump();
  return n;
}
export const ensureLogos = () => ensure(logosDir());
export { shaOf };

// a profile's name: LordOS's in the person's language, the person's own as they wrote it
export const profileTitle = (pr) => (pr.builtin ? tr(`machines.as.profile.${pr.id}`) : pr.name);
