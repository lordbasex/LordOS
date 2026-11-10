// state.js — estado de la Terminal fuera de React: ventanas → pestañas → árbol de paneles
// → sesiones. Vive a nivel de módulo para que minimizar una ventana (que desmonta su
// contenido) no pierda pestañas ni shells; al cerrar la ventana se cierran sus sesiones.
import { createStore, wm, term, clipboard, windows as wmWindows, workArea, t as T } from '@lordos/sdk';
import { getProfile, terminalSettings, saveTerminalSettings, toConfig, profileName } from './profiles.js';

export const meta = createStore({});      // sid → { profile, title, cwd, process, busy, badge, bell, activity, flash, lastExit }
export const wins = createStore({});      // wid → { tabs, active, ...paneles y diálogos }
export const recentDirs = createStore([]);
export const notes = createStore([]);
export const commandHistory = createStore([]);   // [{ cmd, time, sid }] más reciente primero
export const captured = createStore([]);         // líneas capturadas por disparadores
const recentlyClosed = [];                         // para deshacer el cierre de un panel

let seq = 1;
export const nextId = (p) => `${p}${seq++}`;

// ---- árbol de paneles: { type: 'leaf', sid } | { type: 'split', id, dir: 'row'|'column', ratio, a, b } ----

export const leaves = (n) => (!n ? [] : n.type === 'leaf' ? [n.sid] : [...leaves(n.a), ...leaves(n.b)]);

function replaceLeaf(n, sid, fn) {
  if (n.type === 'leaf') return n.sid === sid ? fn(n) : n;
  return { ...n, a: replaceLeaf(n.a, sid, fn), b: replaceLeaf(n.b, sid, fn) };
}

function removeLeaf(n, sid) {
  if (n.type === 'leaf') return n.sid === sid ? null : n;
  const a = removeLeaf(n.a, sid), b = removeLeaf(n.b, sid);
  if (!a) return b;
  if (!b) return a;
  return { ...n, a, b };
}

function setRatioIn(n, id, ratio) {
  if (n.type === 'leaf') return n;
  if (n.id === id) return { ...n, ratio };
  return { ...n, a: setRatioIn(n.a, id, ratio), b: setRatioIn(n.b, id, ratio) };
}

// ---- utilidades ----

export const patchMeta = (sid, p) => meta.set((m) => ({ ...m, [sid]: { ...(m[sid] || {}), ...p } }));
export const patchWin = (wid, fn) => wins.set((w) => (w[wid] ? { ...w, [wid]: { ...w[wid], ...fn(w[wid]) } } : w));
export const getWin = (wid) => wins.get()[wid];
const basename = (p) => (p && p !== '/' ? p.replace(/\/+$/, '').split('/').pop() : p || '');

export function findSession(sid) {
  for (const [wid, w] of Object.entries(wins.get()))
    for (const tab of w.tabs) if (leaves(tab.root).includes(sid)) return { wid, w, tab };
  return null;
}

export const activeTab = (w) => w?.tabs.find((t) => t.id === w.active) || w?.tabs[0] || null;
export const activeSid = (w) => activeTab(w)?.focus ?? null;

export function sessionTitle(sid) {
  const m = meta.get()[sid] || {};
  const base = m.title || (m.cwd ? basename(m.cwd.replace(/^\/home\/[^/]+/, '~')) || '~' : 'Terminal');   // nombre propio, igual en todos los idiomas
  return m.busy && m.process ? `${base} — ${m.process}` : base;
}

export function tabTitle(tab) {
  return tab.title || sessionTitle(tab.focus);
}

// ---- sesiones ----

export function configureSession(sid) {
  const m = meta.get()[sid];
  const found = findSession(sid);
  const p = getProfile(m?.profile);
  term.configure(sid, toConfig(p, { timestamps: found?.w.timestamps, fontDelta: found?.w.fontDelta || 0 }));
}

function startDir(p, reuseFrom) {
  if (p.workingDir === 'home') return undefined;
  if (p.workingDir === 'custom' && p.customDir) return p.customDir.replace(/^~/, lord.fs.home());
  if (reuseFrom == null) return undefined;
  const info = term.info(reuseFrom);
  return info?.cwd || undefined;
}

export function createSession(profileId, reuseFrom, cwd) {
  const p = getProfile(profileId);
  const sid = term.create({ cwd: cwd || startDir(p, reuseFrom), scrollback: p.scrollback });
  patchMeta(sid, { profile: p.id, title: '', cwd: '', badge: p.badge || '', bell: false, activity: false,
    pendingText: p.initialText || '' });
  return sid;
}

function newTab(profileId, reuseFrom, sid = null) {
  const s = sid ?? createSession(profileId, reuseFrom);
  return { id: nextId('t'), root: { type: 'leaf', sid: s }, focus: s, title: null, zoom: false };
}

const pendingLayouts = new Map();      // wid → pestañas a reconstruir (restaurar disposición)
const pendingTabs = new Map();         // wid → pestañas ya armadas con sesiones vivas (recuperadas)

function buildTree(n) {
  if (n.type === 'leaf') {
    const sid = createSession(n.profile, null, n.cwd);
    return { type: 'leaf', sid };
  }
  return { type: 'split', id: nextId('s'), dir: n.dir, ratio: n.ratio, a: buildTree(n.a), b: buildTree(n.b) };
}

// args from another app: { cwd } starts the shell there, { typed } leaves a line typed and not run
export function ensureWin(wid, args) {
  if (getWin(wid)) return;
  let tabs;
  if (pendingLayouts.has(wid)) {
    tabs = pendingLayouts.get(wid).map((tb) => {
      const root = buildTree(tb.root);
      return { id: nextId('t'), root, focus: leaves(root)[0], title: tb.title || null, zoom: false };
    });
    pendingLayouts.delete(wid);
  } else if (pendingTabs.has(wid)) {
    tabs = pendingTabs.get(wid);
    pendingTabs.delete(wid);
  } else {
    const sid = args?.cwd || args?.typed ? createSession(null, null, typeof args.cwd === 'string' ? args.cwd : undefined) : null;
    if (sid != null && /^(ssh|sftp)( -[pP] \d{1,5})? [A-Za-z0-9_][A-Za-z0-9._@-]{0,300}$/.test(String(args.typed || ''))) patchMeta(sid, { pendingText: args.typed });
    tabs = [newTab(null, null, sid)];
  }
  wins.set((w) => ({ ...w, [wid]: {
    tabs, active: tabs[0].id, find: null, toolbelt: false, popup: null, dialog: null,
    broadcast: false, timestamps: false, fontDelta: 0, toasts: [], composer: false, copyMode: null, replay: null,
  } }));
  for (const t of tabs) for (const sid of leaves(t.root)) configureSession(sid);
}

// Al cerrarse una ventana de Terminal en el gestor, se cierran sus sesiones.
wmWindows.subscribe(() => {
  const ids = new Set(wmWindows.get().map((w) => w.id));
  for (const [wid, w] of Object.entries(wins.get())) {
    if (ids.has(wid)) continue;
    for (const tab of w.tabs) for (const sid of leaves(tab.root)) term.close(sid);
    wins.set((all) => { const n = { ...all }; delete n[wid]; return n; });
  }
});

// ---- acciones ----

export function toast(wid, text) {
  if (!wid || !getWin(wid)) return;
  const id = nextId('n');
  patchWin(wid, (w) => ({ toasts: [...w.toasts, { id, text }].slice(-4) }));
  setTimeout(() => patchWin(wid, (w) => ({ toasts: w.toasts.filter((t) => t.id !== id) })), 4000);
}

export function addTab(wid, profileId) {
  const w = getWin(wid);
  const cur = activeSid(w);
  const prof = profileId || terminalSettings().defaultProfile;
  const tab = newTab(prof, cur);
  const idx = w.tabs.findIndex((t) => t.id === w.active);
  patchWin(wid, (x) => ({ tabs: [...x.tabs.slice(0, idx + 1), tab, ...x.tabs.slice(idx + 1)], active: tab.id }));
  configureSession(tab.focus);
}

export function selectTab(wid, tabId) {
  patchWin(wid, () => ({ active: tabId }));
  const tab = getWin(wid).tabs.find((t) => t.id === tabId);
  if (tab) clearAttention(tab.focus);
}

export function cycleTab(wid, d) {
  const w = getWin(wid);
  const i = w.tabs.findIndex((t) => t.id === w.active);
  const n = w.tabs.length;
  selectTab(wid, w.tabs[(i + d + n) % n].id);
}

export function moveTab(wid, tabId, d) {
  patchWin(wid, (w) => {
    const i = w.tabs.findIndex((t) => t.id === tabId), j = i + d;
    if (i < 0 || j < 0 || j >= w.tabs.length) return {};
    const tabs = [...w.tabs];
    [tabs[i], tabs[j]] = [tabs[j], tabs[i]];
    return { tabs };
  });
}

function clearAttention(sid) {
  const m = meta.get()[sid];
  if (m && (m.bell || m.activity)) patchMeta(sid, { bell: false, activity: false });
}

export function focusPane(wid, sid) {
  const f = findSession(sid);
  if (!f) return;
  patchWin(f.wid, (w) => ({ active: f.tab.id, tabs: w.tabs.map((t) => (t.id === f.tab.id ? { ...t, focus: sid } : t)) }));
  clearAttention(sid);
  if (wid && wid !== f.wid) wm.focus(f.wid);
}

export function split(wid, dir, profileId) {
  const w = getWin(wid);
  const tab = activeTab(w);
  if (!tab) return;
  const cur = tab.focus;
  const sid = createSession(profileId || meta.get()[cur]?.profile, cur);
  const root = replaceLeaf(tab.root, cur, (leaf) => ({ type: 'split', id: nextId('s'), dir, ratio: 0.5, a: leaf, b: { type: 'leaf', sid } }));
  patchWin(wid, (x) => ({ tabs: x.tabs.map((t) => (t.id === tab.id ? { ...t, root, focus: sid, zoom: false } : t)) }));
  configureSession(sid);
}

export function setRatio(wid, tabId, splitId, ratio) {
  const r = Math.max(0.1, Math.min(0.9, ratio));
  patchWin(wid, (w) => ({ tabs: w.tabs.map((t) => (t.id === tabId ? { ...t, root: setRatioIn(t.root, splitId, r) } : t)) }));
}

// Saca la sesión de su pestaña (y la pestaña o la ventana si quedan vacías).
export function removeSession(sid) {
  const f = findSession(sid);
  term.close(sid);
  meta.set((m) => { const n = { ...m }; delete n[sid]; return n; });
  if (!f) return;
  const root = removeLeaf(f.tab.root, sid);
  if (!root) {
    const rest = f.w.tabs.filter((t) => t.id !== f.tab.id);
    if (!rest.length) { wm.close(f.wid, true); return; }
    const idx = f.w.tabs.findIndex((t) => t.id === f.tab.id);
    patchWin(f.wid, () => ({ tabs: rest, active: f.w.active === f.tab.id ? rest[Math.max(0, idx - 1)].id : f.w.active }));
    return;
  }
  const remaining = leaves(root);
  patchWin(f.wid, (w) => ({ tabs: w.tabs.map((t) => (t.id === f.tab.id
    ? { ...t, root, focus: remaining.includes(t.focus) ? t.focus : remaining[remaining.length - 1], zoom: false } : t)) }));
}

// Procesos en ejecución (distintos del shell) en un conjunto de sesiones, para confirmar.
export function busyIn(sids) {
  const out = [];
  for (const sid of sids) {
    const info = term.info(sid);
    const p = getProfile(meta.get()[sid]?.profile);
    if (info?.alive && info.busy && p.confirmClose) out.push(info.process);
  }
  return out;
}

export function askThen(wid, sids, what, fn) {
  const busy = busyIn(sids);
  if (!busy.length) { fn(); return; }
  patchWin(wid, () => ({ dialog: { text: T(`terminal.dialog.close.${what}`), detail: T('terminal.dialog.busy', { list: [...new Set(busy)].join(', ') }), onConfirm: fn } }));
}

// Cerrar un panel: la sesión queda 5 s en espera por si se deshace (❖Z).
export function closePane(wid, sid) {
  askThen(wid, [sid], 'pane', () => {
    const f = findSession(sid);
    if (!f) return;
    const item = { sid, wid, profile: meta.get()[sid]?.profile };
    recentlyClosed.push(item);
    detachSession(sid);
    toast(wid, T('terminal.toast.paneClosed'));
    setTimeout(() => {
      const i = recentlyClosed.indexOf(item);
      if (i >= 0) { recentlyClosed.splice(i, 1); term.close(sid); meta.set((m) => { const n = { ...m }; delete n[sid]; return n; }); }
    }, 5000);
  });
}

export function undoClose(wid) {
  const item = recentlyClosed.pop();
  if (!item) { toast(wid, T('terminal.toast.nothingToUndo')); return; }
  const target = getWin(item.wid) ? item.wid : wid;
  const tab = { id: nextId('t'), root: { type: 'leaf', sid: item.sid }, focus: item.sid, title: null, zoom: false };
  patchWin(target, (w) => ({ tabs: [...w.tabs, tab], active: tab.id }));
  wm.focus(target);
}

// Saca la sesión del árbol sin cerrarla (deshacer, mover a otra pestaña).
function detachSession(sid) {
  const f = findSession(sid);
  if (!f) return null;
  const root = removeLeaf(f.tab.root, sid);
  if (!root) {
    const rest = f.w.tabs.filter((t) => t.id !== f.tab.id);
    if (!rest.length) { patchWin(f.wid, () => ({ tabs: [] })); wm.close(f.wid, true); return f; }
    const idx = f.w.tabs.findIndex((t) => t.id === f.tab.id);
    patchWin(f.wid, () => ({ tabs: rest, active: f.w.active === f.tab.id ? rest[Math.max(0, idx - 1)].id : f.w.active }));
    return f;
  }
  const remaining = leaves(root);
  patchWin(f.wid, (w) => ({ tabs: w.tabs.map((t) => (t.id === f.tab.id
    ? { ...t, root, focus: remaining.includes(t.focus) ? t.focus : remaining[remaining.length - 1], zoom: false } : t)) }));
  return f;
}

export function paneToTab(wid) {
  const tab = activeTab(getWin(wid));
  if (!tab || leaves(tab.root).length < 2) return;
  const sid = tab.focus;
  detachSession(sid);
  const nt = { id: nextId('t'), root: { type: 'leaf', sid }, focus: sid, title: null, zoom: false };
  patchWin(wid, (w) => ({ tabs: [...w.tabs, nt], active: nt.id }));
}

export function equalize(wid) {
  const even = (n) => (n.type === 'leaf' ? n : { ...n, ratio: 0.5, a: even(n.a), b: even(n.b) });
  patchWin(wid, (w) => ({ tabs: w.tabs.map((t) => (t.id === w.active ? { ...t, root: even(t.root) } : t)) }));
}

// ---- window arrangements ----

export function saveArrangement() {
  const out = [];
  for (const [wid, w] of Object.entries(wins.get())) {
    const win = wm.get(wid);
    if (!win) continue;
    const tree = (n) => (n.type === 'leaf'
      ? { type: 'leaf', cwd: term.info(n.sid)?.cwd || '', profile: meta.get()[n.sid]?.profile }
      : { type: 'split', dir: n.dir, ratio: n.ratio, a: tree(n.a), b: tree(n.b) });
    out.push({ frame: { x: win.x, y: win.y, w: win.w, h: win.h }, tabs: w.tabs.map((t) => ({ title: t.title, root: tree(t.root) })) });
  }
  saveTerminalSettings({ arrangement: out });
  return out.length;
}

export function restoreArrangement() {
  const arr = terminalSettings().arrangement || [];
  for (const w of arr) {
    const id = `terminal#r${seq++}`;
    pendingLayouts.set(id, w.tabs);
    wm.open('terminal', { instance: id.split('#')[1], frame: w.frame });
  }
  return arr.length;
}

export function closeTab(wid, tabId) {
  const tab = getWin(wid)?.tabs.find((t) => t.id === tabId);
  if (!tab) return;
  askThen(wid, leaves(tab.root), 'tab', () => { for (const sid of leaves(tab.root)) removeSession(sid); });
}

export function toggleZoom(wid) {
  const tab = activeTab(getWin(wid));
  if (!tab || leaves(tab.root).length < 2) return;
  patchWin(wid, (w) => ({ tabs: w.tabs.map((t) => (t.id === tab.id ? { ...t, zoom: !t.zoom } : t)) }));
}

export function setTabTitle(wid, tabId, title) {
  patchWin(wid, (w) => ({ tabs: w.tabs.map((t) => (t.id === tabId ? { ...t, title: title || null } : t)) }));
}

// Cambia el tamaño de letra de todas las sesiones de la ventana.
export function zoom(wid, d) {
  patchWin(wid, (w) => ({ fontDelta: d === 0 ? 0 : Math.max(-6, Math.min(30, w.fontDelta + d)) }));
  for (const tab of getWin(wid).tabs) for (const sid of leaves(tab.root)) configureSession(sid);
}

export function toggleTimestamps(wid) {
  patchWin(wid, (w) => ({ timestamps: !w.timestamps }));
  for (const tab of getWin(wid).tabs) for (const sid of leaves(tab.root)) configureSession(sid);
}

export function reconfigureAll() {
  for (const w of Object.values(wins.get())) for (const tab of w.tabs) for (const sid of leaves(tab.root)) configureSession(sid);
}

export function setSessionProfile(sid, profileId) {
  // la insignia del perfil, o la que haya puesto el shell (OSC 1337 SetBadgeFormat)
  patchMeta(sid, { profile: profileId, badge: getProfile(profileId).badge || meta.get()[sid]?.oscBadge || '' });
  configureSession(sid);
}

// ---- eventos de las sesiones ----

const triggerSeen = new Map();     // sid → Set de líneas ya evaluadas

const hexColor = (h) => (/^#?[0-9a-fA-F]{6}$/.test(h || '') ? parseInt(h.replace('#', ''), 16) : -1);

function runTriggers(sid, wid) {
  const p = getProfile(meta.get()[sid]?.profile);
  if (!p.triggers?.length) return;
  const text = term.text(sid, 'screen') || '';
  const seen = triggerSeen.get(sid) || new Set();
  const now = new Set(text.split('\n').filter(Boolean));
  for (const line of now) {
    if (seen.has(line)) continue;
    for (const tr of p.triggers) {
      if (tr.enabled === false) continue;
      let re;
      try { re = new RegExp(tr.regex); } catch (e) { continue; }
      const m = line.match(re);
      if (!m) continue;
      const param = (tr.param || '').replace(/\\(\d)/g, (_, i) => m[+i] ?? '');
      if (tr.action === 'notify') toast(wid, param || line.trim());
      else if (tr.action === 'bell') patchMeta(sid, { flash: Date.now(), bell: true });
      else if (tr.action === 'attention') { toast(wid, param || line.trim()); wm.focus(wid); }
      else if (tr.action === 'send') term.write(sid, param.replace(/\\n/g, '\r'));
      else if (tr.action === 'capture') captured.set((c) => [{ text: line.trim(), sid, time: Date.now() }, ...c].slice(0, 100));
      else if (tr.action === 'highlight') {
        // "#texto,#fondo" o el formato "{#texto,#fondo}"; por defecto, fondo amarillo
        const [fg, bg] = (tr.param || '').replace(/[{}\s]/g, '').split(',');
        term.highlight(sid, m[0], hexColor(fg), tr.param ? hexColor(bg) : 0xffd60a);
      }
      else if (tr.action === 'mark') term.mark(sid, 'set');
      else if (tr.action === 'title') patchMeta(sid, { title: param || m[0] });
      else if (tr.action === 'stop') break;
    }
  }
  triggerSeen.set(sid, now);
}

term.onAny((sid, type, data) => {
  const f = findSession(sid);
  const wid = f?.wid;
  const isActive = f && f.w.active === f.tab.id && f.tab.focus === sid && wm.focused() === wid;
  switch (type) {
    case 'title': patchMeta(sid, { title: data }); break;
    case 'cwd': {
      // while ssh runs, the directory is the server's (its shell reports OSC 7): kept apart, for uploads and Files mode
      const i = term.info(sid);
      if (i && i.busy && i.process === 'ssh') { patchMeta(sid, { remoteCwd: data }); break; }
      patchMeta(sid, { cwd: data });
      if (data) recentDirs.set((d) => [data, ...d.filter((x) => x !== data)].slice(0, 25));
      if (data) autoProfile(sid, data, wid);
      break;
    }
    case 'run':
      patchMeta(sid, { lastCmd: data });
      commandHistory.set((h) => [{ cmd: data, time: Date.now(), sid }, ...h.filter((x) => x.cmd !== data)].slice(0, 200));
      break;
    case 'bell':
      if (getProfile(meta.get()[sid]?.profile).bell !== 'none') patchMeta(sid, { flash: Date.now(), bell: !isActive });
      break;
    case 'badge': patchMeta(sid, { badge: data, oscBadge: data }); break;
    case 'notify': toast(wid, data); break;
    case 'attention': toast(wid, T('terminal.toast.attention')); if (wid) wm.focus(wid); break;
    case 'profile': {
      const p = terminalSettings().profiles.find((x) => x.name === data || x.id === data);
      if (p) setSessionProfile(sid, p.id);
      break;
    }
    case 'copy': clipboard.remember(lord.clipboard.get()); toast(wid, T('terminal.toast.copied')); break;
    case 'command': {
      const m = meta.get()[sid] || {};
      patchMeta(sid, { lastExit: Number(data) });
      if (m.alert) {
        patchMeta(sid, { alert: false, flash: Date.now(), bell: !isActive });
        toast(wid, m.lastCmd ? T('terminal.toast.finishedCmd', { cmd: m.lastCmd, code: data }) : T('terminal.toast.finished', { code: data }));
      }
      break;
    }
    case 'output': {
      const m = meta.get()[sid] || {};
      if (m.pendingText) {
        // "Enviar texto al abrir": cuando el shell ya mostró algo (su prompt)
        patchMeta(sid, { pendingText: '' });
        term.write(sid, m.pendingText.replace(/\\n/g, '\r').replace(/\n/g, '\r'));
      }
      if (!isActive) patchMeta(sid, { activity: true });
      runTriggers(sid, wid);
      break;
    }
    case 'host':
      patchMeta(sid, { host: data });
      autoProfile(sid, meta.get()[sid]?.cwd, wid);
      break;
    // Passwords in the Terminal (SecretBar.jsx): the program's password prompt, "" when it stops asking
    case 'secret': patchMeta(sid, { secret: data || '' }); break;
    case 'fill':
      if (data === 'ok') patchMeta(sid, { secretDismissed: meta.get()[sid]?.secret || '' });
      else toast(wid, T(data === 'not-asking' ? 'terminal.secret.notAsking' : data === 'multiline' ? 'terminal.secret.multiline' : 'terminal.secret.failed'));
      break;
    // Save to Passwords (SaveBar.jsx): a typed password was accepted; only its program, host and user come here
    case 'save': {
      let o = null;
      try { o = JSON.parse(data); } catch (e) { /* not JSON: nothing to offer */ }
      if (o && Number(o.token) > 0) patchMeta(sid, { saveOffer: { ...o, token: Number(o.token) } });
      break;
    }
    case 'saved':
      patchMeta(sid, { saveOffer: null });
      toast(wid, T(data === 'ok' ? 'terminal.save.saved' : data === 'locked' ? 'terminal.save.locked' : 'terminal.save.failed'));
      break;
    case 'exit': triggerSeen.delete(sid); removeSession(sid); break;
    default: break;
  }
});

// Automatic profile switching. Cada línea de
// perfil.rules es una regla: ruta ("~/prod/*", "/srv/**"), "usuario@host", "host",
// "usuario@" o "usuario@host:/ruta", con * como comodín. Gana la regla más específica.
let localInfo = null;
function autoProfile(sid, cwd, wid) {
  const m = meta.get()[sid] || {};
  const home = lord.fs.home();
  if (!localInfo) localInfo = lord.system.info();
  const localHost = localInfo.hostname;
  const [hu, hh] = (m.host || '').includes('@') ? m.host.split('@') : [null, m.host || ''];
  const user = hu || (!hh || hh === localHost ? localInfo.user : '');
  const host = hh || localHost;
  const glob = (pat, slash) => new RegExp(`^${pat.replace(/[.+^${}()|[\]\\]/g, '\\$&').replace(/\*\*/g, '\u0000')
    .replace(/\*/g, slash ? '[^/]*' : '.*').replace(/\u0000/g, '.*')}$`);
  const score = (rule) => {
    const r = rule.trim();
    if (!r) return 0;
    try {
      let u = null, h = null, path = null;
      if (r.startsWith('/') || r.startsWith('~')) path = r;
      else {
        const [who, p] = r.split(':');
        path = p || null;
        if (who.includes('@')) [u, h] = who.split('@'); else h = who;
      }
      let s = 0;
      if (u) { if (!glob(u).test(user)) return 0; s += 2; }
      if (h) { if (!glob(h).test(host)) return 0; s += 1; }
      if (path) {
        if (!cwd || !glob(path.replace(/^~(?=\/|$)/, home), true).test(cwd)) return 0;
        s += 4;
      }
      return s;
    } catch (e) { return 0; }
  };
  let match = null, best = 0;
  for (const p of terminalSettings().profiles)
    for (const r of (p.rules || '').split('\n')) { const s = score(r); if (s > best) { best = s; match = p; } }
  if (match && match.id !== m.profile) {
    patchMeta(sid, { autoFrom: m.autoFrom || m.profile });
    setSessionProfile(sid, match.id);
    toast(wid, T('terminal.toast.autoProfile', { name: profileName(match) }));
  } else if (!match && m.autoFrom) {
    const back = m.autoFrom;
    patchMeta(sid, { autoFrom: null });
    setSessionProfile(sid, back);
  }
}

export function toggleAlert(wid, sid) {
  const on = !meta.get()[sid]?.alert;
  patchMeta(sid, { alert: on });
  toast(wid, on ? T('terminal.toast.alertOn') : T('terminal.toast.alertOff'));
}

// ---- drop-down hotkey window ----

export function toggleHotkeyWindow() {
  const id = 'terminal#hotkey';
  const w = wm.get(id);
  if (w && !w.minimized && wm.focused() === id) { wm.minimize(id); return; }
  const a = workArea();
  if (w) wm.moveToDesktop(id, wm.desktop());      // it comes to the desktop in view, not the other way round
  wm.open('terminal', { instance: 'hotkey', frame: { x: a.x + 40, y: a.y, w: a.w - 80, h: Math.round(a.h * 0.45) } });
  wm.focus(id);
}

// ---- sesiones que sobreviven (cambio de tema o reinicio de lord-server) ----
//
// La disposición viva (ventanas → pestañas → paneles, con la clave del guardián de cada
// sesión) se guarda en terminal.live. Cuando el módulo arranca y ya hay sesiones abiertas
// —porque se cambió de tema o porque lord-server se reinició y las recuperó de sus
// guardianes (server/src/term/hold.c)— se vuelven a abrir las mismas ventanas.

let lastLive = '';
function saveLive() {
  const tree = (n) => (n.type === 'leaf'
    ? { type: 'leaf', key: term.info(n.sid)?.key || '', profile: meta.get()[n.sid]?.profile || '' }
    : { type: 'split', dir: n.dir, ratio: n.ratio, a: tree(n.a), b: tree(n.b) });
  const out = [];
  for (const [wid, w] of Object.entries(wins.get())) {
    const win = wm.get(wid);
    if (!win || !w.tabs.length) continue;
    out.push({ frame: { x: win.x, y: win.y, w: win.w, h: win.h }, active: Math.max(0, w.tabs.findIndex((t) => t.id === w.active)),
      tabs: w.tabs.map((t) => ({ title: t.title, root: tree(t.root) })) });
  }
  const json = JSON.stringify(out);
  if (json !== lastLive) { lastLive = json; saveTerminalSettings({ live: out }); }
}
let liveTimer = null;
const scheduleLive = () => { clearTimeout(liveTimer); liveTimer = setTimeout(saveLive, 1500); };
wins.subscribe(scheduleLive);
wmWindows.subscribe(scheduleLive);

function restoreLive() {
  const sids = term.list();
  if (!sids.length) return;
  const byKey = new Map(sids.map((sid) => [term.info(sid)?.key, sid]));
  const used = new Set();
  const adopt = (sid, profile) => {
    const p = getProfile(profile);
    const info = term.info(sid) || {};
    patchMeta(sid, { profile: p.id, title: info.title || '', cwd: info.cwd || '', badge: info.badge || p.badge || '', bell: false, activity: false, restored: !!info.restored });
  };
  const mapTree = (n) => {
    if (!n) return null;
    if (n.type === 'leaf') {
      const sid = byKey.get(n.key);
      if (!n.key || sid == null || used.has(sid)) return null;
      used.add(sid);
      adopt(sid, n.profile);
      return { type: 'leaf', sid };
    }
    const a = mapTree(n.a), b = mapTree(n.b);
    if (!a || !b) return a || b;
    return { type: 'split', id: nextId('s'), dir: n.dir, ratio: n.ratio, a, b };
  };
  const out = [];
  for (const w of terminalSettings().live || []) {
    const tabs = (w.tabs || []).map((t) => {
      const root = mapTree(t.root);
      return root && { id: nextId('t'), root, focus: leaves(root)[0], title: t.title || null, zoom: false };
    }).filter(Boolean);
    if (tabs.length) out.push({ frame: w.frame, tabs, active: tabs[Math.min(w.active || 0, tabs.length - 1)].id });
  }
  // las que no estaban en la disposición guardada: una pestaña cada una
  const rest = sids.filter((sid) => !used.has(sid)).map((sid) => { adopt(sid, null); return newTab(null, null, sid); });
  if (rest.length) { if (out.length) out[0].tabs.push(...rest); else out.push({ frame: null, tabs: rest, active: rest[0].id }); }
  const restored = sids.some((sid) => term.info(sid)?.restored);
  // el gestor de ventanas recién está listo cuando el tema terminó de cargar
  setTimeout(() => out.forEach((w, i) => {
    const id = i === 0 ? 'terminal' : `terminal#r${seq++}`;
    pendingTabs.set(id, w.tabs);
    wm.open('terminal', { ...(i ? { instance: id.split('#')[1] } : {}), frame: w.frame || undefined });
    setTimeout(() => {
      patchWin(id, () => ({ active: w.active }));
      if (restored && i === 0) { toast(id, T('terminal.toast.restored', { count: sids.length })); term.ackRestored(); }
    }, 0);
  }), 0);
}
restoreLive();

// The terminal window used last (the Profiles window opens there); the first one left when it's gone.
let lastTerm = null;
export const noteTermFocus = (wid) => { lastTerm = wid; };
export const lastTermWin = () => (lastTerm && getWin(lastTerm) ? lastTerm : Object.keys(wins.get())[0] || null);

// A new terminal window with one tab of that profile. Returns its id.
export const openWindowWithProfile = (profileId) => openWindowWithTabs([newTab(profileId)]);

// Abre una ventana de Terminal con pestañas ya armadas (sesiones existentes). Devuelve su id.
export function openWindowWithTabs(tabs, frame) {
  const id = `terminal#w${seq++}`;
  pendingTabs.set(id, tabs);
  wm.open('terminal', { instance: id.split('#')[1], frame });
  return id;
}
