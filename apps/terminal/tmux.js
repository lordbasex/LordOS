// tmux.js — tmux control-mode integration (`tmux -CC`).
//
// Al correr `tmux -CC` (o `tmux -CC attach`) en una pestaña, esa sesión pasa a ser la
// "puerta": tmux habla un protocolo de líneas (%begin/%end, %output, %layout-change…).
// El servidor (server/src/term/term.c) manda la salida de cada panel directo a una sesión
// virtual; acá se arma el resto: cada ventana de tmux es una pestaña de una ventana de
// LordOS y cada panel de tmux un panel nativo. Dividir, cerrar o abrir pestañas en esa
// ventana se traduce a comandos de tmux, y el tamaño de la ventana se le informa con
// refresh-client. Esc en la puerta desconecta (detach); la sesión de tmux sigue viva.
import { term, wm, windows as wmWindows, t as T } from '@lordos/sdk';
import {
  meta, patchMeta, patchWin, getWin, leaves, nextId, configureSession, removeSession, toast, openWindowWithTabs, findSession,
} from './state.js';

const ctls = new Map();      // puerta → controlador

function send(c, cmd, cb = () => {}) {
  c.queue.push(cb);
  if (!term.tmuxCommand(c.gw, cmd)) c.queue.pop();
}

// ---- layout: "b25d,160x48,0,0{80x48,0,0,1,79x48,81,0,2}" ----

function parseLayout(str) {
  let i = str.indexOf(',') + 1;       // saltea el checksum
  const num = () => { const m = /^\d+/.exec(str.slice(i)); i += m ? m[0].length : 0; return m ? Number(m[0]) : 0; };
  const node = () => {
    const w = num(); i++; const h = num(); i++; num(); i++; num();
    if (str[i] === ',') { i++; return { w, h, pane: num() }; }
    const open = str[i++], close = open === '{' ? '}' : ']';
    const kids = [node()];
    while (str[i] === ',') { i++; kids.push(node()); }
    if (str[i] === close) i++;
    return { w, h, dir: open === '{' ? 'row' : 'column', kids };
  };
  return node();
}

const panesOf = (n) => (n.kids ? n.kids.flatMap(panesOf) : [n.pane]);

// n hijos de tmux → árbol binario con proporciones según el tamaño de cada uno.
function toTree(c, n) {
  if (!n.kids) return { type: 'leaf', sid: paneSid(c, n.pane) };
  const build = (kids) => {
    if (kids.length === 1) return toTree(c, kids[0]);
    const size = (k) => (n.dir === 'row' ? k.w : k.h);
    const total = kids.reduce((a, k) => a + size(k), 0) || 1;
    return { type: 'split', id: nextId('s'), dir: n.dir, ratio: Math.max(0.1, Math.min(0.9, size(kids[0]) / total)), a: toTree(c, kids[0]), b: build(kids.slice(1)) };
  };
  return build(n.kids);
}

// ---- paneles ----

function paneSid(c, num) {
  if (c.panes.has(num)) return c.panes.get(num);
  const sid = term.tmuxPane(c.gw, num);
  if (sid == null) return null;
  c.panes.set(num, sid);
  patchMeta(sid, { profile: meta.get()[c.gw]?.profile, title: '', cwd: '', badge: '', bell: false, activity: false, tmuxPane: num, gateway: c.gw });
  configureSession(sid);
  // contenido actual (con historia y colores) y después la posición del cursor
  send(c, `capture-pane -p -e -J -t %${num} -S -2000`, (lines) => {
    // no se recortan las líneas vacías del final: la última capturada es la última fila del panel
    send(c, `display-message -p -t %${num} "#{cursor_x} #{cursor_y} #{pane_height}"`, ([l = '']) => {
      const [x, y, ph] = l.split(' ').map(Number);
      term.feed(sid, `\x1b[H\x1b[2J${lines.join('\r\n')}`);
      const rows = term.info(sid)?.rows || 24;
      if ([x, y, ph].every(Number.isFinite)) {
        // la fila del cursor se cuenta desde abajo: el panel de LordOS puede tener otra altura
        const last = Math.min(lines.length, rows) - 1;
        const row = Math.max(0, Math.min(rows - 1, last - (ph - 1) + y));
        term.feed(sid, `\x1b[${row + 1};${x + 1}H`);
      }
      term.tmuxReady(sid);
    });
  });
  return sid;
}

// ---- ventanas de tmux = pestañas ----

function applyWindow(c, id, layout, name) {
  let tree;
  try { tree = parseLayout(layout); } catch (e) { return; }
  const live = new Set(panesOf(tree));
  for (const [num, sid] of [...c.panes]) {     // paneles que ya no existen en esta ventana
    if (c.paneWin.get(num) === id && !live.has(num)) { c.panes.delete(num); c.paneWin.delete(num); removeSession(sid); }
  }
  for (const num of live) c.paneWin.set(num, id);
  const root = toTree(c, tree);
  const w = c.wins.get(id);
  if (w && c.wid && getWin(c.wid)) {
    patchWin(c.wid, (x) => ({ tabs: x.tabs.map((t) => (t.id === w.tab ? {
      ...t, root, focus: leaves(root).includes(t.focus) ? t.focus : leaves(root)[0], title: name ?? t.title } : t)) }));
    return;
  }
  const tab = { id: nextId('t'), root, focus: leaves(root)[0], title: name || null, zoom: false, tmuxWindow: id };
  c.wins.set(id, { tab: tab.id });
  if (c.wid && getWin(c.wid)) patchWin(c.wid, (x) => ({ tabs: [...x.tabs, tab], active: tab.id }));
  else c.newTabs.push(tab);
}

function closeWindow(c, id) {
  const w = c.wins.get(id);
  if (!w) return;
  c.wins.delete(id);
  for (const [num, sid] of [...c.panes]) if (c.paneWin.get(num) === id) { c.panes.delete(num); c.paneWin.delete(num); removeSession(sid); }
}

function syncWindows(c, lines) {
  const seen = new Set();
  for (const l of lines) {
    const [id, layout, name] = l.split('\t');
    if (!id || !layout) continue;
    seen.add(id);
    applyWindow(c, id, layout, name);
  }
  for (const id of [...c.wins.keys()]) if (!seen.has(id)) closeWindow(c, id);
  if (c.newTabs.length) {
    const tabs = c.newTabs;
    c.newTabs = [];
    const from = findSession(c.gw);
    const f = from ? wm.get(from.wid) : null;
    c.wid = openWindowWithTabs(tabs, f ? { x: f.x + 40, y: f.y + 40, w: f.w, h: f.h } : undefined);
    setTimeout(() => toast(c.wid, T('terminal.tmux.opened')), 0);
  }
}

const listWindows = (c) => send(c, 'list-windows -F "#{window_id}\t#{window_layout}\t#{window_name}"', (lines) => syncWindows(c, lines));

// ---- protocolo ----

function onLines(c, text) {
  for (const line of text.split('\n')) {
    if (c.block) {
      if (/^%(end|error) /.test(line)) {
        const cb = c.queue.shift();
        const { lines } = c.block;
        c.block = null;
        if (cb && !line.startsWith('%error')) { try { cb(lines); } catch (e) { console.error('tmux:', e); } }
        else if (line.startsWith('%error')) toast(findSession(c.gw)?.wid, `tmux: ${lines.join(' ') || 'error'}`);
      } else c.block.lines.push(line);
      continue;
    }
    if (!line) continue;
    const [kind, a, ...rest] = line.split(' ');
    switch (kind) {
      case '%begin': c.block = { lines: [] }; break;
      case '%layout-change': applyWindow(c, a, rest[0]); break;
      case '%window-add': case '%unlinked-window-add': case '%sessions-changed': case '%session-changed': listWindows(c); break;
      case '%window-close': case '%unlinked-window-close': closeWindow(c, a); listWindows(c); break;
      case '%window-renamed': {
        const w = c.wins.get(a);
        if (w && c.wid) patchWin(c.wid, (x) => ({ tabs: x.tabs.map((t) => (t.id === w.tab ? { ...t, title: rest.join(' ') } : t)) }));
        break;
      }
      case '%window-pane-changed': {
        const sid = c.panes.get(Number(String(rest[0] || '').slice(1)));
        const w = c.wins.get(a);
        if (sid != null && w && c.wid) patchWin(c.wid, (x) => ({ tabs: x.tabs.map((t) => (t.id === w.tab ? { ...t, focus: sid } : t)) }));
        break;
      }
      case '%exit': teardown(c, rest.length ? `${a} ${rest.join(' ')}` : a); break;
      default: break;
    }
  }
}

function start(gw, resumed) {
  if (ctls.has(gw)) return;
  // la primera respuesta (%begin…%end) es la del comando con que se lanzó tmux
  const c = { gw, queue: resumed ? [] : [() => {}], block: null, wins: new Map(), panes: new Map(), paneWin: new Map(),
    newTabs: [], wid: null, size: '' };
  ctls.set(gw, c);
  patchMeta(gw, { tmux: true });
  listWindows(c);
}

function teardown(c, reason) {
  if (!ctls.has(c.gw)) return;
  ctls.delete(c.gw);
  for (const sid of c.panes.values()) removeSession(sid);
  c.panes.clear();
  patchMeta(c.gw, { tmux: false });
  if (reason) toast(findSession(c.gw)?.wid, T('terminal.tmux.detached', { reason }));
}

// Tamaño de la ventana de tmux = el de la pestaña, sumando paneles y separadores.
function sizeOf(n) {
  if (n.type === 'leaf') { const i = term.info(n.sid); return i ? [i.cols, i.rows] : [80, 24]; }
  const [aw, ah] = sizeOf(n.a), [bw, bh] = sizeOf(n.b);
  return n.dir === 'row' ? [aw + 1 + bw, Math.max(ah, bh)] : [Math.max(aw, bw), ah + 1 + bh];
}

let resizeTimer = null;
function resized(sid) {
  const c = ctls.get(meta.get()[sid]?.gateway);
  if (!c) return;
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => {
    const f = findSession(sid);
    if (!f) return;
    const [w, h] = sizeOf(f.tab.root);
    const size = `${w}x${h}`;
    if (size !== c.size) { c.size = size; send(c, `refresh-client -C ${size}`); }
  }, 150);
}

term.onAny((sid, type, data) => {
  if (type === 'tmux-start') start(sid, false);
  else if (type === 'tmux') { const c = ctls.get(sid); if (c) onLines(c, data); }
  else if (type === 'tmux-end') { const c = ctls.get(sid); if (c) teardown(c, ''); }
  else if (type === 'resize') resized(sid);
});

// Si se cierra la ventana de LordOS que muestra tmux, se desconecta (tmux sigue vivo).
wmWindows.subscribe(() => {
  const ids = new Set(wmWindows.get().map((w) => w.id));
  for (const c of ctls.values()) if (c.wid && !ids.has(c.wid) && !getWin(c.wid)) { term.tmuxCommand(c.gw, 'detach-client'); c.wid = null; }
});

// ---- para la app ----

export const isGateway = (sid) => ctls.has(sid);
export const detach = (sid) => term.tmuxCommand(sid, 'detach-client');

// Acciones de la app sobre un panel de tmux → comandos de tmux. Devuelve true si lo manejó.
export function tmuxAction(action, sid, tab) {
  const m = meta.get()[sid];
  const c = m && ctls.get(m.gateway);
  if (!c) return false;
  const p = `%${m.tmuxPane}`;
  switch (action) {
    case 'newTab': send(c, 'new-window'); return true;
    case 'splitRight': send(c, `split-window -h -t ${p}`); return true;
    case 'splitDown': send(c, `split-window -v -t ${p}`); return true;
    case 'closePane': send(c, `kill-pane -t ${p}`); return true;
    case 'closeTab': if (tab?.tmuxWindow) send(c, `kill-window -t ${tab.tmuxWindow}`); return true;
    case 'equalize': send(c, `select-layout -t ${p} tiled`); return true;
    case 'maximizePane': send(c, `resize-pane -Z -t ${p}`); return true;
    default: return false;
  }
}

export function tmuxFocus(sid) {
  const m = meta.get()[sid];
  const c = m && ctls.get(m.gateway);
  if (c) send(c, `select-pane -t %${m.tmuxPane}`);
}

// Sesiones recuperadas tras reiniciar axia-server que seguían en modo control.
setTimeout(() => { for (const sid of term.list()) if (term.info(sid)?.tmux) start(sid, true); }, 0);
