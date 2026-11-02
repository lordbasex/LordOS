// sharemodel.js — the screen-sharing API's small pure rules for the shell (tests/share_model_test.mjs).

// "desktop:foo" or "lpk:foo" → the id the app list knows.
export const shareAppId = (id) => String(id || '').replace(/^(desktop|lpk):/, '');

// What a prompt asks, in order. Only names the server may send; anything else isn't asked.
export function shareAskList(e) {
  const known = ['screen-share', 'remote-input'];
  return known.filter((p) => Array.isArray(e?.ask) && e.ask.includes(p));
}

// Grants of apps that are no longer installed: removed by Remove, or from outside (dpkg, the Store's Linux side). A null
// list means it couldn't be read, so nothing of that kind is called stale.
export function staleGrants(grants, lpkIds, desktopIds) {
  const out = new Set();
  for (const g of grants || []) {
    const id = String(g.id || '');
    if (id.startsWith('lpk:') && lpkIds && !lpkIds.has(id.slice(4))) out.add(id);
    if (id.startsWith('desktop:') && desktopIds && !desktopIds.has(id.slice(8))) out.add(id);
  }
  return [...out];
}
