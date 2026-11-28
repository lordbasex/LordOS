// windowlistmodel.js — a window's title as a one-line label in a window list (the Dock's menu, Workspaces' bar).
// Pure: tested with node (server/tests/windowlist_model_test.mjs).
//
// The title is whatever the app set (a Linux app's xdg_toplevel.set_title or WM_NAME), so it is untrusted: a label never
// carries control characters (a line break would draw a second line over the next row), nor the bidi controls that
// reorder what follows them (U+202A–U+202E, U+2066–U+2069; a title could otherwise show its end first and pass for
// another app's), nor zero-width marks used to make two titles look the same; runs of spaces become one; it is cut
// to WINDOW_LABEL_MAX characters (by code point, never inside a surrogate pair) with an ellipsis.
export const WINDOW_LABEL_MAX = 80;
// eslint-disable-next-line no-control-regex
const DROP = /[\u0000-\u001f\u007f-\u009f​-‏‪-‮⁠-⁩﻿￹-￻]/g;
export function cleanWindowLabel(title, max = WINDOW_LABEL_MAX) {
  if (typeof title !== 'string' || !title) return '';
  const s = title.replace(DROP, ' ').replace(/\s+/g, ' ').trim();
  const cps = [...s];
  return cps.length > max ? `${cps.slice(0, Math.max(1, max - 1)).join('').trimEnd()}…` : s;
}
