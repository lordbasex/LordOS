// clipfiles.js — what a paste in Files copies (pure; server/tests/files_clipfiles_test.mjs).
// inside: Files' own clipboard ({ mode: 'copy' | 'cut', paths }, apps/files/ops.js); outside: what the session
// clipboard holds (clipboard.files(): { paths, cut, own }, the files a Linux file manager copied, already checked by
// the compositor: this machine's absolute paths). The newest wins: when the session clipboard has files Files didn't
// put there, they are what was copied last. Another app's cut is pasted as a copy (only Files moves its own cut).

const MAX = 65536;
const usable = (p) => typeof p === 'string' && p.length > 1 && p[0] === '/' && !/[\0\r\n]/.test(p);

// → { paths, move, outside } or null when there is nothing to paste
export function pasteFrom(inside, outside) {
  if (outside && !outside.own && Array.isArray(outside.paths)) {
    const paths = outside.paths.filter(usable).slice(0, MAX);
    if (paths.length) return { paths, move: false, outside: true };
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
