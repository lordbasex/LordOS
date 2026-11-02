// releases.js — the LordOS releases, from the list GitHub publishes for lordbasex/LordOS
// (https://api.github.com/repos/lordbasex/LordOS/releases, the same list the Debian installer reads).
// Shared by the native and desktop apps. Only names and sizes are taken from it: what is written is
// trusted only after SHA256SUMS.sig verifies with the release key (docs/IMAGER.md).
import { deviceById } from './catalog.js';

export const REPO = 'lordbasex/LordOS';
export const RELEASES_URL = `https://api.github.com/repos/${REPO}/releases?per_page=10`;
export const downloadUrl = (tag, name) => `https://github.com/${REPO}/releases/download/${tag}/${name}`;
// The fingerprint docs/SIGNING.md publishes (SHA-256 of the DER public key)
export const RELEASE_KEY_FINGERPRINT = 'c7155b8e6bbbb180f907381994d06af76d333016cd6f3facf67cadd053ea108e';

// v0.8.3-beta, and point updates with a fourth number: v0.8.4.1-beta
const TAG = /^v[0-9]{1,4}\.[0-9]{1,4}\.[0-9]{1,4}(\.[0-9]{1,4})?(-[a-z0-9.]+)?$/;
const NAME = /^[A-Za-z0-9][A-Za-z0-9._+~-]{0,127}$/;

// [{ tag, version, date, prerelease, image: {name,size}, sums: {name,size}, sig: {name,size} }],
// newest first, only releases that have an image for the device and both signature files.
export function parseReleases(json, deviceId) {
  let list;
  try { list = typeof json === 'string' ? JSON.parse(json) : json; } catch { return []; }
  if (!Array.isArray(list)) return [];
  const dev = deviceById(deviceId);
  if (!dev || !dev.image) return [];
  const out = [];
  for (const r of list.slice(0, 50)) {
    if (!r || typeof r.tag_name !== 'string' || !TAG.test(r.tag_name) || r.draft) continue;
    const assets = Array.isArray(r.assets) ? r.assets : [];
    const find = (test) => {
      const a = assets.find((x) => x && typeof x.name === 'string' && NAME.test(x.name) && test(x.name) && Number.isSafeInteger(x.size) && x.size > 0);
      return a ? { name: a.name, size: a.size } : null;
    };
    const image = find((n) => dev.image.test(n));
    const sums = find((n) => n === 'SHA256SUMS');
    const sig = find((n) => n === 'SHA256SUMS.sig');
    if (!image || !sums || !sig || sums.size > 65536 || sig.size > 512) continue;
    const t = Date.parse(r.published_at || '');
    out.push({ tag: r.tag_name, version: r.tag_name.slice(1).replace(/-.*$/, ''), date: Number.isFinite(t) ? t : 0, prerelease: !!r.prerelease, image, sums, sig });
  }
  return out.sort((a, b) => b.date - a.date);
}

// The size check before writing: the uncompressed image must fit (a card's real size is a bit
// below its label, so 5 % of slack is shown as a warning, not refused)
export function fits(rawSize, diskSize) {
  if (!rawSize || !diskSize) return 'unknown';
  if (rawSize > diskSize) return 'no';
  return diskSize - rawSize < diskSize * 0.05 ? 'tight' : 'yes';
}
