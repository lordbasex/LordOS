// screencastmodel.js — the chooser a Linux app's request to share the screen shows (org.freedesktop.portal.ScreenCast,
// server/src/ui/portal.c; drawn by sdk/screencast.jsx). lord-server says what the app asked for: screens, windows,
// one or several. Nothing is shared until the person picks, and only what was picked.
//
//   castChoices(req, displays, windows)    { screens: [{ key, display, name, index }], windows: [{ key, id, title, app }] }:
//                                          the displays when screens were asked for; the windows on screen (not
//                                          minimized, not a full-screen overlay of the shell) when windows were, the
//                                          one in front first
//   castToggle(picked, key, multiple)      the keys picked after a click: a new one replaces the last unless multiple
//                                          (at most CAST_MAX of them); a click on a picked one unpicks it
//   castSources(picked, choices, frameOf)  what lord.portal.cast gets, in the order picked: { display } for a screen,
//                                          { node, also } for a window (frameOf(id) → { node, also }); a window whose
//                                          frame is gone meanwhile is left out, so it can't share something else

export const CAST_MAX = 4;              // CAST_MAX_STREAMS in server/src/cast/castproto.h

export function castChoices(req, displays, windows) {
  const screens = !req?.monitor ? [] : (displays || []).map((d, index) => ({ key: `d:${d.id}`, display: d.id, name: d.name || '', index }));
  const list = !req?.window ? [] : (windows || []).filter((w) => w && w.id && !w.minimized && !String(w.instance || '').includes('overlay'));
  return { screens, windows: list.slice().reverse().map((w) => ({ key: `w:${w.id}`, id: w.id, title: w.title || '', app: w.app || '' })) };
}

export function castToggle(picked, key, multiple) {
  const now = picked || [];
  if (now.includes(key)) return now.filter((k) => k !== key);
  if (!multiple) return [key];
  return now.length >= CAST_MAX ? now : [...now, key];
}

export function castSources(picked, choices, frameOf) {
  const out = [];
  for (const key of picked || []) {
    const s = choices.screens.find((c) => c.key === key);
    if (s) { out.push({ display: s.display }); continue; }
    const w = choices.windows.find((c) => c.key === key);
    const f = w && frameOf ? frameOf(w.id) : null;
    if (f && f.node > 0) out.push({ node: f.node, also: Array.isArray(f.also) ? f.also.slice(0, 8) : [] });
  }
  return out.slice(0, CAST_MAX);
}
