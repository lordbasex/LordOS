// handsmodel.js — Hands' reader, the pure part (sdk/hands.jsx, docs/HANDS.md; tested by server/tests/hands_model_test.mjs).
//
// An AI agent (Claude Code, Lumo) reads a window as Claude in Chrome reads a page: LordOS's UI tree as a compact text
// tree, one line per thing a person sees or uses, with a role, a name, its state and its box, and a stable `ref_N` the
// agent can click or set. The tree is the React host tree the runtime keeps (runtime/index.js): every node has its type,
// its props, its children (first/next) and, for a text, its text.
//
//   serializeTree(roots, opts) → { text, count, truncated }
//       roots: [{ node, z }] (a window, or the layers back to front); opts: { box(node) → [x, y, w, h] | null,
//       refs (makeRefs()), filter: 'all' | 'interactive', depth, maxChars, focused(node) → bool }
//   findNodes(roots, query, opts) → { text, count }     the nodes whose role, name or value match the words of query
//   makeRefs() → { refOf(node), nodeOf('ref_N'), prune() }   ref_N stay the same for a node as long as it lives
//
// What never comes out, whatever the tree holds (the same lines as for community themes, docs/HANDS.md "Never"):
//   - a layer of the system band (zIndex 100 and above: permission, keyring, password and polkit prompts, the file
//     chooser of Linux apps, Hands' own banner): the caller never passes them, and serializeTree drops them again;
//   - a node marked handsDeny (Settings → AI → Hands, the theme-trust sheet) and everything inside it;
//   - a protected node (a password field, a protected window: props.protected) shows as `protected` with its label and
//     never its value, text or children; a field marked secure has no value either.
// A role comes from accessibilityRole (the SDK's controls set it), else from what the node does: a View with a click
// handler or focusable is a button, a Text is text, an Image an image, a Surface a Linux app's window (no tree: use a
// screenshot), a Terminal a terminal.

export const SYSTEM_Z = 100;
export const ROLES = new Set([
  'window', 'button', 'link', 'textfield', 'textarea', 'searchfield', 'checkbox', 'switch', 'radio', 'radiogroup', 'tab',
  'tablist', 'menubar', 'menu', 'menuitem', 'list', 'row', 'listitem', 'combobox', 'option', 'slider', 'spinbutton', 'dialog',
  'toolbar', 'heading', 'image', 'text', 'group', 'progressbar', 'terminal', 'app', 'media', 'table', 'cell', 'columnheader',
  'clickable', 'protected',
]);
export const INTERACTIVE = new Set([
  'button', 'link', 'textfield', 'textarea', 'searchfield', 'checkbox', 'switch', 'radio', 'tab', 'menuitem', 'row', 'listitem',
  'combobox', 'option', 'slider', 'spinbutton', 'clickable', 'terminal', 'app',
]);
const VALUE_ROLES = new Set(['textfield', 'textarea', 'searchfield', 'combobox', 'slider', 'spinbutton', 'progressbar']);
const LEAF_ROLES = new Set(['textfield', 'textarea', 'searchfield', 'checkbox', 'switch', 'radio', 'slider', 'spinbutton',
  'combobox', 'image', 'text', 'progressbar', 'terminal', 'app', 'media', 'protected', 'button', 'menuitem', 'tab', 'link', 'option']);
const NAME_MAX = 80, VALUE_MAX = 200, TEXT_MAX = 160;

// a prop's own value, never a getter's (a community theme's props are plain copies, and stay unread otherwise)
export function own(props, key) {
  if (!props || typeof props !== 'object') return undefined;
  let d;
  try { d = Object.getOwnPropertyDescriptor(props, key); } catch (e) { return undefined; }
  return d && 'value' in d ? d.value : undefined;
}
const str = (v) => (typeof v === 'string' ? v : typeof v === 'number' && Number.isFinite(v) ? String(v) : '');
// one line of plain text: no controls, bidi marks or line breaks, at most max characters
export function clean(s, max = NAME_MAX) {
  const t = str(s).replace(/[\u0000-\u001f\u007f-\u009f​-‏‪-‮⁠-⁯﻿]/g, ' ').replace(/\s+/g, ' ').trim();
  const chars = [...t];
  return chars.length > max ? `${chars.slice(0, max - 1).join('')}…` : t;
}
const quote = (s) => `"${s.replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"`;

const isFn = (v) => typeof v === 'function';
const handlers = ['onClick', 'onMouseDown', 'onMouseUp', 'onDoubleClick', 'onContextMenu', 'onPress'];
function hidden(n) {
  if (!n || n.deleted || n.hidden) return true;
  const st = n.style;
  return !!(st && st.display === 'none');
}
const denied = (n) => !!own(n.props, 'handsDeny');
const isProtected = (n) => !!own(n.props, 'protected');

// the role of a host node, or '' for a plain container
export function roleOf(n) {
  const p = n.props;
  const r = own(p, 'accessibilityRole');
  if (typeof r === 'string' && ROLES.has(r)) return r;
  if (own(p, 'lordWin')) return 'window';
  if (n.type === 'text') return 'text';
  if (n.type === 'image') return own(p, 'accessibilityLabel') ? 'image' : '';
  if (n.type === 'surface') return 'app';
  if (n.type === 'terminal') return 'terminal';
  if (n.type === 'media') return 'media';
  if (n.type === 'view' && (handlers.some((k) => isFn(own(p, k))) || own(p, 'focusable') === true)) return 'clickable';
  return '';
}

// the text a node shows (its texts, depth first), never inside a protected or denied node
export function gatherText(n, max = TEXT_MAX) {
  const out = [];
  let len = 0;
  const walk = (k, d) => {
    if (len >= max || d > 40 || hidden(k) || denied(k) || isProtected(k)) return;
    if (k.type === 'raw') { const t = str(k.text); if (t) { out.push(t); len += t.length; } return; }
    for (let c = k.first; c; c = c.next) walk(c, d + 1);
  };
  walk(n, 0);
  return clean(out.join(' '), max);
}

function nameOf(n, role) {
  const p = n.props;
  const label = clean(own(p, 'accessibilityLabel'));
  if (label) return label;
  if (role === 'window') return clean(own(p, 'lordTitle')) || '';
  if (VALUE_ROLES.has(role) || role === 'protected') return clean(own(p, 'placeholder'));
  const text = gatherText(n, NAME_MAX);
  return text || clean(own(p, 'title')) || clean(own(p, 'placeholder'));
}

function statesOf(n, role) {
  const p = n.props;
  const st = own(p, 'accessibilityState');
  const s = st && typeof st === 'object' ? st : {};
  const out = [];
  const flag = (k) => own(s, k) === true || (k === 'disabled' && own(p, 'disabled') === true);
  for (const k of ['disabled', 'checked', 'selected', 'expanded', 'busy']) if (flag(k)) out.push(k);
  if (own(s, 'checked') === 'mixed') out.push('mixed');
  if (role === 'switch' || role === 'checkbox' || role === 'radio') if (!out.includes('checked') && own(s, 'checked') === false) out.push('unchecked');
  return out;
}

// a field's value: never a password's or a protected node's
function valueOf(n, role) {
  if (!VALUE_ROLES.has(role) || isProtected(n)) return null;
  const st = own(n.props, 'accessibilityState');
  if (st && own(st, 'secure') === true) return null;
  const v = own(n.props, 'accessibilityValue');
  if (v === undefined || v === null) return null;
  return clean(typeof v === 'object' ? own(v, 'text') ?? own(v, 'now') : v, VALUE_MAX);
}

export function makeRefs() {
  let next = 1;
  const byNode = new WeakMap(), byRef = new Map();
  return {
    refOf(n) {
      let r = byNode.get(n);
      if (!r) { r = next++; byNode.set(n, r); byRef.set(r, n); }
      return `ref_${r}`;
    },
    nodeOf(ref) {
      const m = /^ref_(\d{1,9})$/.exec(String(ref || ''));
      const n = m ? byRef.get(Number(m[1])) : null;
      return n && !n.deleted ? n : null;
    },
    prune() { for (const [r, n] of byRef) if (n.deleted) byRef.delete(r); },
    get size() { return byRef.size; },
  };
}

const boxText = (b) => (b ? ` @${Math.round(b[0])},${Math.round(b[1])} ${Math.round(b[2])}x${Math.round(b[3])}` : '');
const intersect = (a, b) => (a && b ? [Math.max(a[0], b[0]), Math.max(a[1], b[1]),
  Math.min(a[0] + a[2], b[0] + b[2]) - Math.max(a[0], b[0]), Math.min(a[1] + a[3], b[1] + b[3]) - Math.max(a[1], b[1])] : a || b);

// every node the agent may know of, in paint order, with its line's parts; visit(entry) → false stops the walk
function walkTree(roots, opts, visit) {
  const box = opts.box || (() => null);
  const maxDepth = Math.min(Math.max(1, opts.depth || 30), 60);
  let stop = false;
  const walk = (n, depth, clip, inWin) => {
    if (stop || hidden(n) || denied(n)) return;
    let b = null;
    if (n.type !== 'raw' && n.id) { try { b = box(n); } catch (e) { b = null; } }
    if (b && (b[2] <= 0 || b[3] <= 0)) return;
    const vis = b ? intersect(b, clip) : null;
    if (b && clip && (!vis || vis[2] <= 0 || vis[3] <= 0)) return;      // scrolled or clipped out of sight
    const role = isProtected(n) ? 'protected' : roleOf(n);
    const st = n.style || {};
    const nextClip = b && st.overflow === 'hidden' ? (clip ? intersect(clip, b) : b) : clip;
    if (role && n.type !== 'raw') {
      const entry = { node: n, role, name: nameOf(n, role), states: statesOf(n, role), value: valueOf(n, role), box: vis || b, depth, win: inWin };
      if (role === 'window') entry.win = own(n.props, 'lordWin');
      if (visit(entry) === false) { stop = true; return; }
      if (LEAF_ROLES.has(role) || depth >= maxDepth) return;
      for (let c = n.first; c; c = c.next) walk(c, depth + 1, nextClip, entry.win || inWin);
      return;
    }
    for (let c = n.first; c; c = c.next) walk(c, depth, nextClip, inWin);
  };
  for (const r of roots || []) {
    if (!r || !r.node || (r.z || 0) >= SYSTEM_Z) continue;
    if (r.win) {
      // a window's frame (whatever node the theme made it): its line first, its inside below it
      if (hidden(r.node) || denied(r.node)) continue;
      let b = null;
      try { b = box(r.node); } catch (e) { b = null; }
      if (visit({ node: r.node, role: 'window', name: clean(r.title || ''), states: [], value: null, box: b, depth: 0, win: r.win }) === false) break;
      if (isProtected(r.node)) continue;
      for (let c = r.node.first; c; c = c.next) walk(c, 1, b && (r.node.style || {}).overflow === 'hidden' ? b : null, r.win);
    } else walk(r.node, 0, null, null);
    if (stop) break;
  }
}

export function serializeTree(roots, opts = {}) {
  const refs = opts.refs || makeRefs();
  const interactive = opts.filter === 'interactive';
  const maxChars = opts.maxChars || 40000;
  const lines = [];
  let size = 0, count = 0, truncated = false;
  walkTree(roots, opts, (e) => {
    if (interactive && !INTERACTIVE.has(e.role) && e.role !== 'window' && e.role !== 'protected') return true;
    if (e.role === 'text' && !e.name) return true;
    const ref = e.role === 'text' ? '' : ` [${refs.refOf(e.node)}]`;
    let line = `${'  '.repeat(interactive ? (e.role === 'window' ? 0 : 1) : e.depth)}${e.role}${e.name ? ` ${quote(e.name)}` : ''}`;
    if (e.role === 'window' && e.win) line += ` id=${e.win}`;
    line += ref;
    if (e.value !== null && e.value !== undefined) line += ` value=${quote(e.value)}`;
    if (e.states.length) line += ` {${e.states.join(',')}}`;
    if (opts.focused && opts.focused(e.node)) line += ' {focused}';
    line += boxText(e.box);
    if (size + line.length + 1 > maxChars) { truncated = true; return false; }
    lines.push(line);
    size += line.length + 1;
    count++;
    return true;
  });
  if (truncated) lines.push('… (cut: read a window, a smaller depth or filter "interactive")');
  return { text: lines.join('\n'), count, truncated };
}

// words of a query: lower case, accents folded
const fold = (s) => str(s).normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();
const SYNONYMS = {
  field: ['textfield', 'textarea', 'searchfield'], input: ['textfield', 'textarea', 'searchfield'], box: ['textfield', 'checkbox'],
  search: ['searchfield'], toggle: ['switch', 'checkbox'], menu: ['menuitem', 'menu', 'menubar'], item: ['menuitem', 'row', 'listitem', 'option'],
  dropdown: ['combobox'], select: ['combobox'], popup: ['combobox'], tab: ['tab'], button: ['button', 'clickable'], link: ['link'],
  row: ['row', 'listitem'], check: ['checkbox'], slider: ['slider'], text: ['text'], window: ['window'],
};
const STOP = new Set(['the', 'a', 'an', 'to', 'of', 'in', 'on', 'for', 'with', 'and', 'or', 'el', 'la', 'los', 'las', 'de', 'del', 'un', 'una']);

export function findNodes(roots, query, opts = {}) {
  const refs = opts.refs || makeRefs();
  const words = fold(query).split(/[^\p{L}\p{N}_.-]+/u).filter((w) => w && !STOP.has(w));
  const found = [];
  if (!words.length) return { text: '', count: 0 };
  walkTree(roots, opts, (e) => {
    if (e.role === 'window' || (e.role === 'text' && !e.name)) return true;
    const name = fold(e.name), value = fold(e.value || '');
    let score = 0;
    for (const w of words) {
      if (name === w) score += 6;
      else if (name.split(/\s+/).includes(w)) score += 4;
      else if (name.includes(w)) score += 3;
      if (value.includes(w)) score += 1;
      if (e.role === w || (SYNONYMS[w] || []).includes(e.role)) score += 2;
    }
    if (fold(e.name) === fold(query).trim()) score += 4;
    if (INTERACTIVE.has(e.role)) score += 0.5;
    if (score >= 2) found.push({ e, score });
    return true;
  });
  found.sort((a, b) => b.score - a.score);
  const top = found.slice(0, opts.max || 20);
  const text = top.map(({ e }) => {
    const ref = e.role === 'text' ? '' : ` [${refs.refOf(e.node)}]`;
    return `${e.role}${e.name ? ` ${quote(e.name)}` : ''}${ref}${e.value !== null && e.value !== undefined ? ` value=${quote(e.value)}` : ''}` +
      `${e.states.length ? ` {${e.states.join(',')}}` : ''}${e.win ? ` in=${e.win}` : ''}${boxText(e.box)}`;
  }).join('\n');
  return { text, count: top.length };
}

// ---- an allowlisted screenshot: everything but the allowed windows is black (fail closed) ----
// roots: [{ node, z }] below the system band (the theme's tree and every layer); kept(node) → true for an allowed window's
// frame or a surface tied to one (a menu, popover or sheet with handsWin of an allowed window). The answer is the ids of
// the largest subtrees holding nothing kept: the wallpaper, the desktop's widgets, other windows, the Dock, the menu bar,
// panels, tooltips, notifications. A node above a kept one is not blacked (its children are, one by one); a kept one and
// everything inside it stays. Raw text belongs to its Text, which is blacked whole.
export function blackoutNodes(roots, kept) {
  const out = [];
  const has = new Map();
  const holds = (n, d) => {
    if (has.has(n)) return has.get(n);
    let r = false;
    if (d > 4096) r = true;
    else if (kept(n)) r = true;
    else for (let c = n.first; c && !r; c = c.next) if (holds(c, d + 1)) r = true;
    has.set(n, r);
    return r;
  };
  const walk = (n, d) => {
    if (kept(n)) return;
    for (let c = n.first; c; c = c.next) {
      if (c.type === 'raw') continue;
      if (holds(c, d + 1)) walk(c, d + 1);
      else if (c.id) out.push(c.id);
    }
  };
  for (const r of roots || []) {
    if (!r || !r.node || (r.z || 0) >= SYSTEM_Z) continue;
    if (!holds(r.node, 0) && r.node.id && r.z) { for (let c = r.node.first; c; c = c.next) if (c.id && c.type !== 'raw') out.push(c.id); continue; }
    walk(r.node, 0);
  }
  return out;
}

// ---- the allowlist and the windows ----

// a window's app id ('files#2' → 'files'); a Linux window's is its own id
export const appOfWindow = (w) => String((w && (w.app || w.id)) || '').split('#')[0];
export const allowed = (allow, app) => !Array.isArray(allow) || !allow.length || allow.includes(app);

// windows_context's list: id, app, title, display, focused, minimized, kind, frame
export function windowsList(list, { focusedId, displayOf, titleOf, allow, kindOf } = {}) {
  return (list || []).filter((w) => w && allowed(allow, appOfWindow(w))).map((w) => {
    const f = w.frame || w;
    return {
      id: w.id, app: appOfWindow(w), title: clean(titleOf ? titleOf(w) : w.title || ''), display: displayOf ? displayOf(w) : null,
      focused: w.id === focusedId, minimized: !!w.minimized, kind: kindOf ? kindOf(w) : 'native',
      x: Math.round(f.x || 0), y: Math.round(f.y || 0), w: Math.round(f.w || 0), h: Math.round(f.h || 0),
    };
  });
}

// an app named by the agent: its id, or a shown name ("Notes", "notas"), case and accents ignored
export function matchApp(apps, wanted) {
  const q = fold(wanted).trim();
  if (!q) return null;
  return (apps || []).find((a) => fold(a.id) === q) || (apps || []).find((a) => fold(a.name) === q) ||
    (apps || []).find((a) => fold(a.name).startsWith(q)) || null;
}

// AppLog: the lines of an app's log that hold the pattern (plain text, case ignored), the newest last, at most max
export function filterLog(text, pattern, max = 100) {
  const p = fold(pattern || '');
  const lines = String(text || '').split('\n').filter((l) => l && (!p || fold(l).includes(p)));
  return lines.slice(-Math.max(1, Math.min(max, 400))).map((l) => clean(l, 400)).join('\n');
}
