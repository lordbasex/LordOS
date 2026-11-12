// trash.js — Move to Trash in Photos (GitHub #119), no screen: which pictures go where, the library's selection and
// what the viewer shows once its picture is gone.
//
// It works as Files does (apps/files/index.jsx toTrash): a picture on this computer goes to the Trash without asking;
// one on a slow folder (a network share) goes off the compositor's thread; one in a cloud account is deleted there,
// after asking, since it goes to the provider's own trash and Restore isn't offered (docs/CLOUD.md 17.2).
import { splitTrash } from '../../sdk/cloudmodel.js';

// paths, the cloud accounts and fs.isRemote → { local, slow, cloud: [{ acc, paths }] }
export function trashPlan(paths, accounts, isRemote = () => false) {
  const s = splitTrash(paths, accounts);
  const slow = s.local.filter((p) => isRemote(p));
  return { local: s.local.filter((p) => !slow.includes(p)), slow, cloud: s.cloud };
}

// A click on a thumbnail. order: the paths as the grid shows them; mods: { shift, ctrl, meta }.
// Ctrl (⌘) adds or takes one away, Shift selects from the anchor to it, a plain click selects only it.
// → { sel: Set, anchor }
export function clickSelect(sel, order, anchor, path, mods = {}) {
  if (mods.ctrl || mods.meta) {
    const next = new Set(sel);
    if (next.has(path)) next.delete(path); else next.add(path);
    return { sel: next, anchor: path };
  }
  if (mods.shift && anchor && order.includes(anchor) && order.includes(path)) {
    const a = order.indexOf(anchor), b = order.indexOf(path);
    return { sel: new Set(order.slice(Math.min(a, b), Math.max(a, b) + 1)), anchor };
  }
  return { sel: new Set([path]), anchor: path };
}

// a right click on a thumbnail: on one already selected the menu acts on the whole selection, otherwise on it alone
export const menuTargets = (sel, path) => (sel.has(path) ? [...sel] : [path]);

// the selection without the pictures that are no longer listed
export function keepListed(sel, items) {
  const there = new Set(items.map((e) => e.path));
  const next = new Set([...sel].filter((p) => there.has(p)));
  return next.size === sel.size ? sel : next;
}

// The viewer after its pictures in `gone` left: the same list without them, at the picture that came after the one
// shown (or the one before it, when it was the last). null when nothing is left, and the viewer goes back to the library.
export function afterRemove(list, index, gone) {
  const out = new Set(gone);
  const rest = list.filter((e) => !out.has(e.path));
  if (!rest.length) return null;
  let i = 0;
  for (let k = 0; k < Math.min(index, list.length); k++) if (!out.has(list[k].path)) i++;
  return { list: rest, index: Math.min(i, rest.length - 1) };
}
