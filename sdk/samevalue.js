// samevalue.js — pure, no imports (tested by server/tests/poll_same_test.mjs).
// Whether two polled values are the same data (plain JSON-like values: numbers, strings, arrays, objects). A poll
// that reads the same thing again keeps the old value, so React has nothing to render.
export function sameValue(a, b) {
  if (Object.is(a, b)) return true;
  if (!a || !b || typeof a !== 'object' || typeof b !== 'object' || Array.isArray(a) !== Array.isArray(b)) return false;
  const ka = Object.keys(a), kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  for (const k of ka) if (!Object.prototype.hasOwnProperty.call(b, k) || !sameValue(a[k], b[k])) return false;
  return true;
}
