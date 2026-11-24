// probe.js — test-only, never in a release: the first module of the Machines e2e bundle (tools/vm/e2e/build-e2e.mjs). It
// keeps a copy of lord-server's node tree on the JS side, by wrapping the lord.* node calls before the runtime makes its
// first node, so a test can find what is on screen by its text and click it where lord-server laid it out. No new native
// API and no change to the runtime: the wrapped calls do exactly what they did, then note what they did.
//
// From a driver command (tools/vm/e2e/driver.js, `mh` in scope; README.md here):
//   mh.find(pattern, { within, all, visible, covered })   nodes whose text matches (a string: exact after trimming; a RegExp:
//                                                test), shown and on top (not under a sheet or a menu) unless asked
//   mh.findImage(pattern, { within, all })      images by their source (an icon button: 'machines/symbols/agent-')
//   mh.texts({ within })                         every visible text, top to bottom, left to right
//   mh.rect(id)                                  [x, y, w, h] of a node on screen (lord.getLayout)
//   mh.textOf(id)                                the text under a node
//   mh.stats()                                   how many nodes it tracks
// `within` is a rectangle [x, y, w, h] (a window's, from wm.windows()) or a node id.
const N = new Map();            // id → { id, type, text, parent, kids, style }
let root = 0;

function node(id) {
  let n = N.get(id);
  if (!n) { n = { id, type: '?', text: '', parent: 0, kids: [], style: {} }; N.set(id, n); }
  return n;
}
function detach(n) {
  if (!n.parent) return;
  const p = N.get(n.parent);
  if (p) { const i = p.kids.indexOf(n.id); if (i >= 0) p.kids.splice(i, 1); }
  n.parent = 0;
}
function drop(id) {
  const n = N.get(id);
  if (!n) return;
  detach(n);
  for (const k of n.kids.slice()) drop(k);
  N.delete(id);
}

function wrap() {
  if (typeof lord === 'undefined' || typeof lord.createNode !== 'function' || lord.__mhProbe) return;
  const o = {
    createNode: lord.createNode, setText: lord.setText, setStyle: lord.setStyle, setProp: lord.setProp, appendChild: lord.appendChild,
    insertBefore: lord.insertBefore, removeChild: lord.removeChild, destroy: lord.destroy, setRoot: lord.setRoot,
  };
  lord.createNode = (type) => { const id = o.createNode(type); const n = node(id); n.type = type; return id; };
  lord.setText = (id, text) => { const r = o.setText(id, text); node(id).text = String(text ?? ''); return r; };
  lord.setStyle = (id, k, v) => {
    const r = o.setStyle(id, k, v);
    if (k === 'display' || k === 'opacity' || k === 'overflow' || k === 'visibility' || k === 'pointerEvents' || k === 'zIndex') node(id).style[k] = v;
    else if (k === 'backgroundColor' || k === 'background') node(id).style.bg = !!v && v !== 'transparent' && !/^rgba\([^)]*,\s*0\)$/.test(String(v));
    return r;
  };
  // an Image's source: icon buttons have no text, they are found by their picture (mh.findImage)
  lord.setProp = (id, k, v) => { const r = o.setProp(id, k, v); if (k === 'src') node(id).src = typeof v === 'string' ? v : ''; return r; };
  lord.appendChild = (p, c) => {
    const r = o.appendChild(p, c);
    const cn = node(c); detach(cn); cn.parent = p; node(p).kids.push(c);
    return r;
  };
  lord.insertBefore = (p, c, b) => {
    const r = o.insertBefore(p, c, b);
    const cn = node(c); detach(cn); cn.parent = p;
    const pk = node(p).kids; const i = pk.indexOf(b);
    if (i >= 0) pk.splice(i, 0, c); else pk.push(c);
    return r;
  };
  lord.removeChild = (p, c) => { const r = o.removeChild(p, c); detach(node(c)); return r; };
  lord.destroy = (id) => { const r = o.destroy(id); drop(id); return r; };
  lord.setRoot = (id) => { root = id; return o.setRoot(id); };
  lord.__mhProbe = true;
}
wrap();

const rect = (id) => { const l = lord.getLayout(id); return l && l.length === 4 ? l.map((v) => Math.round(v)) : null; };
function textOf(id) {
  const n = N.get(id);
  if (!n) return '';
  if (n.type === 'raw') return n.text;
  let s = n.text || '';
  for (const k of n.kids) s += textOf(k);
  return s;
}
// shown: attached to the root, no ancestor with display none or opacity 0, a size, and inside every ancestor that clips
function shown(id) {
  let r = rect(id);
  if (!r || r[2] <= 0 || r[3] <= 0) return null;
  let n = N.get(id);
  let x0 = r[0], y0 = r[1], x1 = r[0] + r[2], y1 = r[1] + r[3];
  while (n) {
    if (n.style.display === 'none' || n.style.opacity === 0 || n.style.visibility === 'hidden') return null;
    if (n.id === root) break;
    const p = N.get(n.parent);
    if (!p) return null;                                 // not on screen (taken out, or never put in)
    if (p.style.overflow === 'hidden' || p.style.overflow === 'scroll') {
      const pr = rect(p.id);
      if (pr) { x0 = Math.max(x0, pr[0]); y0 = Math.max(y0, pr[1]); x1 = Math.min(x1, pr[0] + pr[2]); y1 = Math.min(y1, pr[1] + pr[3]); }
      if (x1 <= x0 || y1 <= y0) return null;
    }
    n = p;
  }
  return [x0, y0, x1 - x0, y1 - y0];
}
// in paint order (children after their parent, siblings by zIndex, then later ones over earlier ones; the layers are the
// root's children with their zIndex): a node with a background that comes later and covers the point hides it (a sheet
// over the library, a menu or a question over a window)
function paintOrder() {
  const out = [];
  const z = (id) => Number(N.get(id)?.style.zIndex) || 0;
  const walk = (id) => {
    const n = N.get(id);
    if (!n || n.style.display === 'none') return;
    out.push(id);
    const ks = n.kids.map((k, i) => [k, i]).sort((a, b) => z(a[0]) - z(b[0]) || a[1] - b[1]);
    for (const [k] of ks) walk(k);
  };
  walk(root);
  return out;
}
function covered(id, r, order) { return coveredBy(id, r, order) !== 0; }
// the node painted over this one's centre (0 when none): for a test that wonders why a text isn't found
function coveredBy(id, r, order) {
  const cx = r[0] + r[2] / 2, cy = r[1] + r[3] / 2;
  const anc = new Set();
  for (let n = N.get(id); n; n = N.get(n.parent)) anc.add(n.id);
  const i = order.indexOf(id);
  if (i < 0) return -1;
  // the first node after this one's subtree
  let j = i + 1;
  const under = (x) => { for (let n = N.get(x); n; n = N.get(n.parent)) if (n.id === id) return true; return false; };
  while (j < order.length && under(order[j])) j++;
  for (; j < order.length; j++) {
    const o = N.get(order[j]);
    if (!o.style.bg || anc.has(o.id)) continue;
    const q = shown(o.id);
    if (q && cx >= q[0] && cx < q[0] + q[2] && cy >= q[1] && cy < q[1] + q[3]) return o.id;
  }
  return 0;
}
const inside = (r, w) => r[0] + r[2] / 2 >= w[0] && r[0] + r[2] / 2 < w[0] + w[2] && r[1] + r[3] / 2 >= w[1] && r[1] + r[3] / 2 < w[1] + w[3];
const area = (within) => (Array.isArray(within) ? within : typeof within === 'number' ? rect(within) : null);
// the nodes that hold text: a 'text' node (its raw children joined), or a raw one hung elsewhere
function textNodes() {
  const out = [];
  for (const n of N.values()) {
    if (n.type === 'text') out.push(n);
    else if (n.type === 'raw' && N.get(n.parent)?.type !== 'text') out.push(n);
  }
  return out;
}
function find(pattern, opt = {}) {
  const w = area(opt.within);
  const test = pattern instanceof RegExp ? (s) => pattern.test(s) : (s) => s.trim() === String(pattern);
  const out = [];
  let order = null;
  for (const n of textNodes()) {
    const s = textOf(n.id);
    if (!s || !test(s)) continue;
    const r = opt.visible === false ? rect(n.id) : shown(n.id);
    if (!r || (w && !inside(r, w))) continue;
    // on top: nothing painted over it (the default for what is shown; { covered: true } keeps the hidden ones too)
    if (opt.visible !== false && !opt.covered) { order = order || paintOrder(); if (covered(n.id, r, order)) continue; }
    out.push({ id: n.id, text: s, rect: r });
  }
  // the topmost first when windows overlap is not known here: the caller narrows with `within`; reading order otherwise
  out.sort((a, b) => a.rect[1] - b.rect[1] || a.rect[0] - b.rect[0]);
  return opt.all ? out : out.slice(0, 1);
}
// images whose source matches (Machines' symbols: "svg:40:…:asset:machines/symbols/agent-symbolic.svg"), shown and on top
function findImage(pattern, opt = {}) {
  const w = area(opt.within);
  const test = pattern instanceof RegExp ? (s) => pattern.test(s) : (s) => s.includes(String(pattern));
  const out = [];
  let order = null;
  for (const n of N.values()) {
    if (n.type !== 'image' || !n.src || !test(n.src)) continue;
    const r = shown(n.id);
    if (!r || (w && !inside(r, w))) continue;
    if (!opt.covered) { order = order || paintOrder(); if (covered(n.id, r, order)) continue; }
    out.push({ id: n.id, src: n.src, rect: r });
  }
  out.sort((a, b) => a.rect[1] - b.rect[1] || a.rect[0] - b.rect[0]);
  return opt.all ? out : out.slice(0, 1);
}
function texts(opt = {}) {
  return find(/\S/, { ...opt, all: true }).map((t) => t.text.trim());
}

globalThis.mh = {
  find, findImage, texts, rect, textOf, shown, stats: () => ({ nodes: N.size, root }),
  coveredBy: (id) => { const r = shown(id); return r ? coveredBy(id, r, paintOrder()) : -1; },
  info: (id) => { const n = N.get(id); return n ? { id, type: n.type, style: n.style, parent: n.parent, kids: n.kids.slice(0, 20), text: textOf(id).slice(0, 80) } : null; },
};
