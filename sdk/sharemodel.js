// sharemodel.js — the screen-sharing API's small pure rules for the shell (tests/share_model_test.mjs).

// "desktop:foo" or "lpk:foo" → the id the app list knows.
export const shareAppId = (id) => String(id || '').replace(/^(desktop|lpk):/, '');

// What a prompt asks, in order. Only names the server may send; anything else isn't asked.
export function shareAskList(e) {
  const known = ['screen-share', 'remote-input'];
  return known.filter((p) => Array.isArray(e?.ask) && e.ask.includes(p));
}
