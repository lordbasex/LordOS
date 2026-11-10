// clipfiles.js — what a paste in Files copies (pure; server/tests/files_clipfiles_test.mjs).
// inside: Files' own clipboard ({ mode: 'copy' | 'cut', paths }, apps/files/ops.js); outside: what the session
// clipboard holds (clipboard.files(): { paths, cut, own }, the files a Linux file manager copied, already checked by
// the compositor: this machine's absolute paths). The newest wins: when the session clipboard has files Files didn't
// put there, they are what was copied last. Another app's cut is pasted as a copy (only Files moves its own cut).
// The compositor keeps another app's files only from an app outside a sandbox whose clipboard text named those
// same paths (server/src/wl/urilist.c, uril_accept); even so Files asks before copying them (pasteAsk).

const MAX = 65536;
const usable = (p) => typeof p === 'string' && p.length > 1 && p[0] === '/' && !/[\0\r\n]/.test(p);

// → { paths, move, outside, from? } or null when there is nothing to paste (from: the app that copied them, '' if unknown)
export function pasteFrom(inside, outside) {
  if (outside && !outside.own && Array.isArray(outside.paths)) {
    const paths = outside.paths.filter(usable).slice(0, MAX);
    if (paths.length) return { paths, move: false, outside: true, from: typeof outside.from === 'string' ? outside.from : '' };
  }
  if (inside && Array.isArray(inside.paths) && inside.paths.length) return { paths: inside.paths, move: inside.mode === 'cut', outside: false };
  return null;
}

// What Files offers the session clipboard when it copies or cuts: only real absolute paths (a Cloud or network
// place that isn't mounted has none); null when there is nothing Linux apps could use.
export function offerable(paths) {
  const list = (paths || []).filter(usable);
  return list.length && list.length === paths.length && list.length <= MAX ? list : null;
}

// What Files asks before pasting files copied in another app (null: a paste of its own, no question): how many,
// the first SHOWN names and how many more. from: the app's name as given (its app_id or window title), on one
// line and short, '' if there is none (the question then says "another app").
const SHOWN = 5;
const baseName = (p) => p.slice(p.lastIndexOf('/') + 1) || p;
export function pasteAsk(c) {
  if (!c || !c.outside || !Array.isArray(c.paths) || !c.paths.length) return null;
  const from = [...String(c.from || '').replace(/[\s\u2028\u2029]+/g, ' ').trim()];   // by code point
  return {
    count: c.paths.length,
    names: c.paths.slice(0, SHOWN).map(baseName),
    more: Math.max(0, c.paths.length - SHOWN),
    from: from.length > 60 ? `${from.slice(0, 59).join('')}…` : from.join(''),
  };
}
