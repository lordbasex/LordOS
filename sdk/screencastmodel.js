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
//   castAskerName(who, { linux, flatpak }) the asking app's name from lord-server's verified identity
//   castWindowOf(choice, windows)          the live window of a choice (for its app's name)

export const CAST_MAX = 4;              // CAST_MAX_STREAMS in server/src/cast/castproto.h

// The app the chooser and the indicator name: only lord-server's verified identity (`who`), "desktop:<id>" (its
// .desktop file), "flatpak:<id>" (its root-owned scope, docs/FLATPAK.md) or "lpk:<id>"; linux(id) and flatpak(id)
// give the installed app's name ('' when it isn't known), else the id itself. A Flatpak app's name is its own metadata
// (org.evil.App may call itself "Firefox"), so it is never shown alone: "Firefox · org.mozilla.firefox", the verified
// id always there; flatpak(id) must look the app up by that exact id only. Before 2026-11-22 a Flatpak app was named
// "flatpak:org.chromium.Chromium".
export function castAskerName(who, { linux = () => '', flatpak = () => '' } = {}) {
  const w = typeof who === 'string' ? who : '';
  if (w.startsWith('desktop:')) return linux(w.slice(8)) || w.slice(8);
  if (w.startsWith('flatpak:')) {
    const id = w.slice(8), name = String(flatpak(id) || '').trim();
    return name && name !== id ? `${name} \u00b7 ${id}` : id;
  }
  if (w.startsWith('lpk:')) return w.slice(4);
  return w;
}

// The live window a choice stands for (its app's name needs the window's args), or the choice when it is gone.
// Before 2026-11-22 every Linux window was labelled "Linux".
export const castWindowOf = (choice, list) => (Array.isArray(list) && list.find((x) => x && x.id === choice?.id)) || choice;

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
