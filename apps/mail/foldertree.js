// foldertree.js — the sidebar's folder tree from the flat IMAP list (no imports: tested with node,
// server/tests/mail_foldertree_test.mjs). Each folder comes from lord-maild as { name, display, special,
// delim, noselect } with the special folders first, in their order.
//
// - Special folders (Inbox, Drafts, Sent, Archive, Junk, Trash…) are roots, in the server's order,
//   wherever they live ("INBOX.Sent", "[Gmail]/Sent Mail").
// - Every other folder hangs under its nearest listed parent, by the hierarchy delimiter.
// - Servers that keep everything under INBOX ("INBOX.Work", Dovecot/Courier style namespaces)
//   show those folders at the top level, as if the INBOX prefix were not there.
// - A \Noselect container is shown (not selectable) only when something visible hangs under it.

const ci = (s) => s.toUpperCase();

function inboxPrefixed(f) {
  const d = f.delim;
  return !!d && f.name.length > 6 && ci(f.name.slice(0, 5)) === 'INBOX' && f.name[5] === d;
}

// Path segments of a folder; null when it has no hierarchy.
function segments(f, strip) {
  if (ci(f.name) === 'INBOX') return ['INBOX'];
  if (!f.delim) return [f.name];
  let parts = f.name.split(f.delim);
  if (ci(parts[0]) === 'INBOX') parts[0] = 'INBOX';
  if (strip && parts[0] === 'INBOX' && parts.length > 1) parts = parts.slice(1);
  return parts;
}

// Returns [{ f, depth }] in display order.
export function folderTree(folders, maxDepth = 8) {
  const list = (folders || []).filter((f) => f && typeof f.name === 'string' && f.name);
  // the "INBOX." personal namespace: every folder but INBOX itself (and the special ones, which
  // are roots anyway) lives under it
  const others = list.filter((f) => ci(f.name) !== 'INBOX' && !f.special);
  const strip = others.length > 0 && others.every(inboxPrefixed);

  const nodes = new Map();
  for (const f of list) {
    const segs = segments(f, strip);
    const key = segs.join('\u0000');
    if (nodes.has(key)) continue;                 // same path twice: keep the first
    nodes.set(key, { f, segs, kids: [] });
  }
  const roots = [];
  for (const n of nodes.values()) {
    let parent = null;
    if (!n.f.special) {
      for (let k = n.segs.length - 1; k > 0 && !parent; k--) parent = nodes.get(n.segs.slice(0, k).join('\u0000')) || null;
    }
    (parent ? parent.kids : roots).push(n);
  }
  const byName = (a, b) => (a.f.display || a.f.name).localeCompare(b.f.display || b.f.name, undefined, { sensitivity: 'base', numeric: true });
  // visible: selectable, or a container with something visible under it
  const visible = (n) => !n.f.noselect || n.kids.some(visible);
  const out = [];
  const walk = (n, depth) => {
    if (!visible(n)) return;
    out.push({ f: n.f, depth: Math.min(depth, maxDepth) });
    for (const k of [...n.kids].sort(byName)) walk(k, depth + 1);
  };
  const special = roots.filter((n) => n.f.special);       // already in their order
  const plain = roots.filter((n) => !n.f.special).sort(byName);
  for (const n of special) walk(n, 0);
  for (const n of plain) walk(n, 0);
  return out;
}
