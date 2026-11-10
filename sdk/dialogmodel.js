// dialogmodel.js — a Linux app's dialog and the window it belongs to (pure logic, no lord.*).
//
// lord-server says which window a dialog belongs to (xdg_toplevel.set_parent, or xdg-foreign's
// set_parent_of from another process) and whether it is modal (xdg-dialog-v1); sdk/wayland.js keeps
// that in the window's args.parent (its window id) and args.modal. A dialog opens centered over its
// parent, moves, minimizes and changes desktop with it, and a modal one stays right above it: the
// parent takes no clicks or keys while it is up (server/src/wl/wl.c), so the front window, the one
// with the keyboard, is the dialog. See docs/WAYLAND.md, "Dialogs".

export const ownerOf = (w) => w?.args?.parent || null;

// A w×h dialog centered over the parent's frame, inside the work area.
export function overParent(parent, w, h, area) {
  const ww = Math.min(w, area.w), hh = Math.min(h, area.h);
  const x = Math.round(parent.x + (parent.w - ww) / 2), y = Math.round(parent.y + (parent.h - hh) / 2);
  return {
    x: Math.max(area.x, Math.min(x, area.x + area.w - ww)),
    y: Math.max(area.y, Math.min(y, area.y + area.h - hh)),
    w: ww, h: hh,
  };
}

// The window list after a change (prev → next), with each dialog kept with its parent. Returns next
// itself when nothing had to change (the caller sets the list only then, so it settles).
export function followParents(prev, next) {
  const before = new Map(prev.map((w) => [w.id, w]));
  const byId = new Map(next.map((w) => [w.id, w]));
  let changed = false;
  // a modal dialog minimized or brought back on its own takes its parent along: the parent can't be
  // used without it
  const along = new Map(next.filter((w) => w.args?.modal && before.has(w.id) && !!w.minimized !== !!before.get(w.id).minimized)
    .map((w) => [ownerOf(w), !!w.minimized]));
  let list = next.map((w) => {
    let n = w;
    if (along.has(w.id) && !!w.minimized !== along.get(w.id)) n = { ...n, minimized: along.get(w.id) };
    const p = byId.get(ownerOf(w));
    if (!p) return n;
    const p0 = before.get(p.id), w0 = before.get(w.id);
    // the parent moved and the dialog didn't: it goes along by the same amount
    if (p0 && w0 && (p.x !== p0.x || p.y !== p0.y) && w.x === w0.x && w.y === w0.y && !w.maximized) {
      n = { ...n, x: w.x + p.x - p0.x, y: w.y + p.y - p0.y };
    }
    if (p0 && !!p.minimized !== !!p0.minimized && !!n.minimized !== !!p.minimized) n = { ...n, minimized: !!p.minimized };
    if (p.desktop !== undefined && n.desktop !== p.desktop) n = { ...n, desktop: p.desktop };
    return n;
  });
  if (list.some((w, i) => w !== next[i])) changed = true;
  // a modal dialog right above its parent (the parent was brought forward: the dialog comes too)
  for (let pass = 0; pass < list.length; pass++) {
    const at = new Map(list.map((w, i) => [w.id, i]));
    const i = list.findIndex((w) => w.args?.modal && at.has(ownerOf(w)) && at.get(ownerOf(w)) > at.get(w.id));
    if (i < 0) break;
    const w = list[i], rest = list.filter((_, k) => k !== i);
    const p = rest.findIndex((x) => x.id === ownerOf(w));
    list = [...rest.slice(0, p + 1), w, ...rest.slice(p + 1)];
    changed = true;
  }
  return changed ? list : next;
}
