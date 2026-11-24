// download.js — Machines settings → Systems downloads a catalog system ahead of time with the same machinery as the New
// Machine assistant (assistant.jsx): the admin helper reads the maker's signed SHA-256 list (vm-image-list), the app
// names the newest image in it, and vm-image-get downloads it into /var/lib/lordos/machines/images/<sha256>.iso, kept
// only with that hash (docs/VIRTUALIZATION.md 3.9, 4.1). A Windows card downloads the pinned evaluation in the person's
// language. Nothing here takes an address: the catalog has them, root checks them.
import { admin, fs, t as tr } from '@lordos/sdk';
import CATALOG from '../../assets/machines/systems.json';
import { entryOf, listingNames, newest, pickImage, pinnedImage, progressOf, unattendedOf, downloadedImages, IMAGES_DIR } from './catalog.js';
import { windowsEntry, evalFor } from './windows.js';
import { userGetArg, sigVerdict } from './systems.js';

// the newest image of a catalog entry, from its maker's list (checked by the helper): { name, sha, size }
export async function resolveImage(id, arch) {
  const e = entryOf(CATALOG, id, arch);
  if (!e) throw new Error(tr('machines.dl.noList', { why: tr('machines.dl.noImage') }));
  // a pinned image: the signed catalog is its list
  const pin = pinnedImage(e);
  if (pin) return pin;
  let p = await admin.run('vm-image-list', `${id} ${arch}`, null, { quiet: true });
  if (!e.sums) {
    const list = newest(listingNames(fs.read(String(p)), e.sumsPattern), e.sumsPattern);
    if (!list) throw new Error(tr('machines.dl.noList', { why: tr('machines.dl.noImage') }));
    p = await admin.run('vm-image-list', `${id} ${arch} ${list}`, null, { quiet: true });
  }
  const img = pickImage(e, fs.read(String(p)));
  if (!img) throw new Error(tr('machines.dl.noList', { why: tr('machines.dl.noImage') }));
  return img;
}

// downloads what a catalog card installs from (its own image, or the one its unattended install boots); onProgress
// gets { done, total }. Resolves with the image's path.
export async function downloadSystem(id, arch, locale, onProgress) {
  const get = (eid, f) => admin.run('vm-image-get', `${eid} ${arch} ${f.name} ${f.sha}`, (l) => { const g = progressOf(l); if (g) onProgress?.(g); }, { quiet: true }).then(String);
  const s = (CATALOG.systems || []).find((x) => x?.id === id);
  if (s?.kind === 'windows') {
    const f = evalFor(windowsEntry(CATALOG, arch), locale);
    if (!f) throw new Error(tr('machines.dl.noImage'));
    return get('windows', f);
  }
  const u = unattendedOf(CATALOG, id, arch);
  const eid = u ? u.image : id;
  return get(eid, await resolveImage(eid, arch));
}
export const stopDownload = () => admin.run('vm-image-cancel', '', null, { quiet: true }).catch(() => {});

// A system the person added (Machines settings → Systems): the admin helper's vm-image-get-user downloads its image, and
// its signature files when it has a key, from the address the person gave (checked again in C: https, a public name,
// port 443, at most 16 GiB, room left on the store) into the same image store, kept only with its SHA-256. An
// administrator's action. onProgress gets { done, total }. Resolves with the image's path.
export async function downloadUserSystem(s, onProgress) {
  const arg = userGetArg(s);
  if (!arg) throw new Error(tr('machines.set.sys.err.url'));
  return String(await admin.run('vm-image-get-user', arg, (l) => { const g = progressOf(l); if (g) onProgress?.(g); }, { quiet: true }));
}

// Its signature, checked by lord-pgpv (lord.vm.sigCheck) with the system's key; then the image's SHA-256 must be in the
// checksum file that signature covers. Resolves with { valid, fingerprint } or { valid: false, reason }.
export function checkSignature(s) {
  const g = s?.signature;
  const job = g?.key && typeof lord.vm?.sigCheck === 'function' ? lord.vm.sigCheck(s.sha256, g.key, g.fingerprint) : 0;
  if (!job) return Promise.resolve({ valid: false, reason: 'noHelper' });
  return new Promise((resolve) => {
    const timer = setInterval(() => {
      const r = lord.vm.job(job);
      if (r && !r.done) return;
      clearInterval(timer);
      resolve(r ? sigVerdict(r.out) : { valid: false, reason: 'noHelper' });
    }, 150);
  });
}

// the catalog's ids with a whole image here (for "Downloaded"): an image of the card itself or of the one its unattended
// install boots
export function downloadedIds() {
  let entries = [];
  try { entries = lord.fs.list(IMAGES_DIR); } catch (e) { return new Set(); }
  const info = (sha) => JSON.parse(fs.read(`${IMAGES_DIR}/${sha}.json`));
  const ids = new Set(downloadedImages(CATALOG, entries, info, []).filter((x) => !x.partial).map((x) => x.id));
  for (const s of CATALOG.systems || []) {
    const img = s?.unattended?.image;
    if (img && ids.has(img)) ids.add(s.id);
  }
  return ids;
}
