// logos.js — the systems' official logos, opt-in (approved by the maintainer 2026-11-22; docs/TRADEMARKS.md and
// docs/VIRTUALIZATION.md "Official logos"). LordOS ships no third-party logo: the catalog holds, for the systems whose
// owners' policies let a logo identify their software, only an https address of a PNG and its SHA-256 (an entry of kind
// "logo" in assets/machines/systems.json). With "Official logos of the systems" on (Machines settings → General, off by
// default), the admin helper downloads it like any pinned file (vm-image-get: the address from the catalog, never from
// here, kept only with that hash); the app checks it is a PNG of at most 512 KB and 1024 × 1024 (systems.js
// pngHeader), has lord-media re-encode it in its sandbox (MW_EDIT, given fds, never paths) into
// ~/.cache/lordos/machines/logos/<sha256>.png, and keeps only that. Turned off, no logo shows; Remove downloaded logos
// deletes them. On any failure the system keeps LordOS's own drawn mark.
import { admin, fs, media, settings } from '@lordos/sdk';
import CATALOG from '../../assets/machines/systems.json';
import { logoEntry } from './catalog.js';
import { pngHeader, LOGO_MAX_BYTES } from './systems.js';
import { defaultsOf, logoPath, logosDir, ensureLogos, storeRev } from './store.js';

export const logosOn = () => defaultsOf(settings.get().machinesDefaults).logos;

// a system's logo kept here (switch on), or ''
export function logoFor(sysId, arch) {
  if (!logosOn()) return '';
  const e = logoEntry(CATALOG, sysId, arch || 'amd64') || logoEntry(CATALOG, sysId, 'arm64');
  return e ? logoPath(e.sha) : '';
}

const busy = new Set();
const gone = (path) => {
  try { fs.trash(path); const t = (fs.trashList() || []).find((x) => x.original === path); if (t) fs.removeFromTrash(t.id); } catch (e) { /* not there */ }
};

// Downloads and re-encodes one system's logo; resolves with its PNG or '' (no logo listed, switch off, any failure)
export async function fetchLogo(sysId, arch) {
  if (!logosOn()) return '';
  const e = logoEntry(CATALOG, sysId, arch);
  if (!e || e.bytes > LOGO_MAX_BYTES) return '';
  const have = logoPath(e.sha);
  if (have || busy.has(e.sha)) return have;
  busy.add(e.sha);
  const tmp = `${logosDir()}/.${e.sha}.download.png`;
  try {
    const src = String(await admin.run('vm-image-get', `${e.id} ${arch} ${e.file} ${e.sha}`, null, { quiet: true }));
    const bytes = fs.read(src, 'binary');
    if (!bytes || bytes.byteLength !== e.bytes || !pngHeader(bytes)) return '';
    ensureLogos();
    gone(tmp);
    fs.write(tmp, bytes);
    const out = `${logosDir()}/${e.sha}.png`;
    await media.edit('machines', tmp, out, { format: 'png' });
    storeRev.set((n) => n + 1);
    return logoPath(e.sha);
  } catch (err) {
    return '';
  } finally {
    gone(tmp);
    busy.delete(e.sha);
  }
}

// every listed system's logo, for this architecture (the switch just turned on)
export async function fetchAllLogos(arch) {
  const ids = (CATALOG.systems || []).filter((s) => s?.kind === 'logo' && typeof s.for === 'string').map((s) => s.for);
  for (const id of ids) await fetchLogo(id, arch);
}
// the systems whose owners' policies let their logo be downloaded (Settings → General lists them)
export const logoSystems = () => (CATALOG.systems || []).filter((s) => s?.kind === 'logo' && typeof s.for === 'string' && s.for !== 'windows')
  .map((s) => ({ id: s.for, name: String(s.name || s.for) }));
