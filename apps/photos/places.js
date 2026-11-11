// places.js — what Photos lists besides the pictures folder, no screen (GitHub #89): the cloud folders kept on this
// computer. A kept folder is fully downloaded, so its pictures open without the network; folders only in the cloud are
// left out, so browsing Photos never waits for a provider.

// accounts: cloudState's accounts → [{ path, account, label }], one per kept folder, by account and then path
import { compareNames } from '../../sdk/collate.js';
export function keptPlaces(accounts) {
  const out = [];
  for (const a of accounts || []) {
    if (!a || a.disabled || a.state === 'off' || typeof a.mount !== 'string' || !a.mount || !Array.isArray(a.kept)) continue;   // turned off: not mounted
    const name = String(a.name || a.id || '');
    const kept = a.kept.filter((p) => typeof p === 'string' && (p === a.mount || p.startsWith(`${a.mount}/`)));
    // a folder inside another kept one is already listed with it
    const top = kept.filter((p) => !kept.some((q) => q !== p && p.startsWith(`${q}/`))).sort((x, y) => x.localeCompare(y));
    for (const p of top) out.push({ path: p, account: name, label: p === a.mount ? name : `${name} · ${p.slice(a.mount.length + 1)}` });
  }
  return out.sort((x, y) => compareNames(x.account, y.account) || x.path.localeCompare(y.path));
}

// a path inside one of the places
export const inPlaces = (places, p) => places.some((k) => p === k.path || String(p).startsWith(`${k.path}/`));
