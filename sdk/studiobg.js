// studiobg.js — LordOS Studio's background catalog on disk (docs/CAMERA-STUDIO-PLAN.md 6.2; the twin of sdk/motion.jsx's
// downloads). The catalog (assets/studio/backgrounds.json) ships in the package with its thumbnails and one picture per
// category; the other pictures and every loop are fetched on demand by the closed admin action
// studio-bg-get "<id> <variant>" (server/src/hw/studio_bg_admin.c): JS names an entry, never a URL or a path, and
// lord-server keeps only a file with the catalog's hash, in /var/lib/lordos/studio. Removing one is studio-bg-remove,
// for an administrator. The plain logic (which variant, which path, credits) is in studiomodel.js.
import catalog from '../assets/studio/backgrounds.json';
import { settings } from './settings.js';
import { createStore } from './store.js';
import { admin } from './admin.js';
import { normalizeStudio, bgVariant, bgSrc, bgReady } from './studiomodel.js';

export const studioCatalog = catalog;
export const bgDownloads = createStore({});          // id → percent while downloading

const setStudio = (fn) => settings.set((s) => ({ ...s, studio: fn(normalizeStudio(s.studio)) }));

// The item's file, downloaded first when it isn't there yet; resolves to the path lord.studio.setBackground takes
export function fetchBackground(it, { big = false } = {}) {
  const files = normalizeStudio(settings.get().studio).bgFiles;
  if (bgReady(it, files)) return Promise.resolve(bgSrc(it, files[it.id] || (it.variants?.[0]?.name || '')));
  const v = bgVariant(it, { big });
  if (!v) return Promise.reject(new Error('studio.downloadFailed'));
  bgDownloads.set((d) => ({ ...d, [it.id]: 0 }));
  const done = () => bgDownloads.set((d) => { const x = { ...d }; delete x[it.id]; return x; });
  return admin.run('studio-bg-get', `${it.id} ${v.name}`, (line) => {
    const m = /^PROGRESS (\d+)/.exec(line);
    if (m) bgDownloads.set((d) => ({ ...d, [it.id]: Number(m[1]) }));
  }, { quiet: true }).then(() => {
    done();
    setStudio((c) => ({ ...c, bgFiles: { ...c.bgFiles, [it.id]: v.name } }));
    return bgSrc(it, v.name);
  }, (e) => { done(); throw e; });
}

// A downloaded item removed (an administrator's action); a background showing it goes back to the blur
export async function removeBackground(it) {
  const c = normalizeStudio(settings.get().studio);
  const name = c.bgFiles[it.id];
  if (!name) return;
  const path = bgSrc(it, name);
  await admin.run('studio-bg-remove', `${it.id} ${name}`, null, { quiet: true });
  setStudio((x) => {
    const files = { ...x.bgFiles };
    delete files[it.id];
    const n = { ...x, bgFiles: files };
    if (x.background === path) n.background = null;
    if (x.video === path) n.video = null;
    return normalizeStudio(n);
  });
}
