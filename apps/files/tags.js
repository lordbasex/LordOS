// tags.js — the tag and comment maps Files keeps (path → [colors], path → text), as pure functions.
// Each returns a new map. The stores take it whole (store.set(() => map)): an object passed to set() is
// merged into the old one, which brought back a path whose last tag or comment was removed (GitHub #181).

// Toggles one color on the paths: off when every path has it, on otherwise. A path left with no tags goes.
export function toggledTags(all, paths, color) {
  const next = { ...all };
  const on = paths.every((p) => (next[p] || []).includes(color));
  for (const p of paths) {
    const cur = new Set(next[p] || []);
    if (on) cur.delete(color); else cur.add(color);
    if (cur.size) next[p] = [...cur]; else delete next[p];
  }
  return next;
}

// Sets one path's comment; an empty one removes it.
export function withComment(all, path, text) {
  const next = { ...all };
  if (String(text).trim()) next[path] = String(text).slice(0, 500); else delete next[path];
  return next;
}

// A moved or renamed path keeps its entry under the new path.
export function movedKey(all, from, to) {
  if (!(from in all) || from === to) return all;
  const next = { ...all, [to]: all[from] };
  delete next[from];
  return next;
}
