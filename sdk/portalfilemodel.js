// portalfilemodel.js — the FileChooser portal's dialog for a Linux app (sdk/filepicker.jsx PortalFilePrompt), as plain
// functions tested with node (server/tests/portal_file_model_test.mjs).
//
// portalFilter: the app's glob patterns (a(sa(us)) filters, kind 0) as a matcher { test(name) }, or null for every file.
// The spec's glob: * any run, ? one character, [abc] / [a-z] / [!abc] a class; everything else is literal, and case
// doesn't count. Chromium writes extensions as "*.[pP][dD][fF]" (each letter in both cases), so classes must work or
// nothing would match. A pattern is the app's, so it never becomes a RegExp (a "*a*a*a*…b" one would backtrack for
// ages over a long name): it is compiled to tokens and matched by the usual star-backtracking walk, at most
// name × pattern steps; MAX_GLOB characters a pattern, MAX_GLOBS patterns.
export const MAX_GLOB = 128;
export const MAX_GLOBS = 64;
const low = (c) => c.toLowerCase();
export function compileGlob(p) {
  const cs = [...String(p).slice(0, MAX_GLOB)];
  const out = [];
  for (let i = 0; i < cs.length; i++) {
    const c = cs[i];
    if (c === '*') { if (out[out.length - 1]?.k !== '*') out.push({ k: '*' }); continue; }
    if (c === '?') { out.push({ k: '?' }); continue; }
    if (c === '[') {
      const end = cs.indexOf(']', i + 2);          // "[]…]" starts a class with "]"; a class has one character at least
      if (end > i) {
        let body = cs.slice(i + 1, end);
        let neg = false;
        if (body[0] === '!' || body[0] === '^') { neg = true; body = body.slice(1); }
        const ranges = [];
        for (let k = 0; k < body.length; k++) {
          if (body[k + 1] === '-' && k + 2 < body.length) { ranges.push([low(body[k]), low(body[k + 2])]); k += 2; } else ranges.push([low(body[k]), low(body[k])]);
        }
        if (ranges.length) { out.push({ k: '[', neg, ranges }); i = end; continue; }
      }
    }
    out.push({ k: 'c', c: low(c) });
  }
  return out;
}
const one = (t, c) => (t.k === '?' ? true : t.k === 'c' ? t.c === c
  : t.ranges.some(([a, b]) => c >= a && c <= b) !== t.neg);
export function globMatch(toks, name) {
  const s = [...low(String(name))];
  let i = 0, j = 0, star = -1, back = 0;
  while (i < s.length) {
    if (j < toks.length && toks[j].k !== '*' && one(toks[j], s[i])) { i++; j++; }
    else if (j < toks.length && toks[j].k === '*') { star = j++; back = i; }
    else if (star >= 0) { j = star + 1; i = ++back; }
    else return false;
  }
  while (j < toks.length && toks[j].k === '*') j++;
  return j === toks.length;
}
export function portalFilter(patterns) {
  if (!Array.isArray(patterns) || !patterns.length || patterns.includes('*')) return null;
  const list = patterns.filter((p) => typeof p === 'string' && p).slice(0, MAX_GLOBS).map(compileGlob);
  if (!list.length) return null;
  return { test: (name) => list.some((t) => globMatch(t, name)) };
}

// Where the dialog goes: over the app's window when it said which one (lord-server resolved it: a Wayland window the
// app exported, or the app's own X11 window), else centered on the display of the window in front (the app that asked
// is almost always it), else on the main display; never straddling two displays. box: { x, y, w, h } in screen
// coordinates. ownerFrame/area: the parent's frame and its display's work area; frontArea: the front window's.
export function portalBox(ownerFrame, ownerArea, frontArea, mainArea, overParent, size = [920, 620]) {
  if (ownerFrame && ownerArea) return overParent(ownerFrame, Math.round(Math.min(size[0], ownerArea.w * 0.96)), Math.round(Math.min(size[1], ownerArea.h * 0.94)), ownerArea);
  const a = frontArea || mainArea;
  if (!a) return null;
  const w = Math.round(Math.min(size[0], a.w * 0.96)), h = Math.round(Math.min(size[1], a.h * 0.94));
  return { x: Math.round(a.x + (a.w - w) / 2), y: Math.round(a.y + (a.h - h) / 2), w, h };
}
