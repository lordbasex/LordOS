// Terminal — LordOS's native terminal.
//
// La emulación (xterm-256color, color directo, mouse, imágenes en línea, marcas del shell)
// y el dibujo están en C (server/src/term); esta app arma pestañas, paneles divididos,
// perfiles, buscador, barra de herramientas, atajos y la ventana desplegable.
import { View, Text, useAppWindow, useKeyboard, useStore, useTheme, wm, term, clipboard, addShortcuts, registerDockMenu, speechMenu, registerSelection, ContextMenu, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import {
  wins, meta, ensureWin, getWin, patchWin, activeTab, activeSid, addTab, closePane, closeTab, split, cycleTab,
  selectTab, focusPane, zoom, toggleZoom, toggleTimestamps, leaves, tabTitle, busyIn, toggleHotkeyWindow, toast,
  undoClose, paneToTab, equalize, moveTab, saveArrangement, restoreArrangement, toggleAlert, meta as metaStore,
} from './state.js';
import { PaneTree, paneNodes } from './Panes.jsx';
import {
  TabBar, TAB_COLORS, StatusBar, FindBar, Toolbelt, Overlays, ReplayBar, popupKey, openAutocomplete, openPasteHistory, openCommandHistory,
} from './Chrome.jsx';
import { Preferences } from './Preferences.jsx';
import { tmuxAction, isGateway, detach } from './tmux.js';
import { actionOf, hint, keyMappingOf, mappingBytes, keyCapture, comboOf, actionLabel } from './keys.js';
import { clipboardImage, pathsText, isClaude } from './paths.js';
import { getProfile, schemeOf, terminalSettings, profileName, profileMenu } from './profiles.js';

// Menú del Dock (clic derecho): nueva ventana, nueva pestaña y pestaña con un perfil.
registerDockMenu('terminal', (entry) => {
  const last = entry.windows[entry.windows.length - 1];
  const items = [{ label: actionLabel('newWindow'), onClick: () => (last ? run(last.id, 'newWindow') : wm.open('terminal', { new: true })) }];
  if (last) {
    items.push({ label: actionLabel('newTab'), onClick: () => { wm.focus(last.id); run(last.id, 'newTab'); } });
    const profiles = terminalSettings().profiles;
    if (profiles.length > 1) {
      items.push({ label: tr('terminal.menu.profiles'), submenu: profileMenu(profiles, (p) => ({ label: profileName(p), onClick: () => { wm.focus(last.id); addTab(last.id, p.id); } })) });
    }
  }
  return items;
});

export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(180deg, #2c2c31, #111113)',
      border: '1px solid rgba(255,255,255,0.15)', boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)` }}>
      <Text style={{ position: 'absolute', left: s * 0.17, top: s * 0.16, fontSize: s * 0.32, fontWeight: 700, color: '#4ade80' }}>{'>_'}</Text>
    </View>
  );
}

// ---- ventana desplegable: tecla global (F12 por defecto) ----

const isHotkey = (e) => {
  const hk = terminalSettings().hotkey;
  return (hk === 'F12' && e.code === 88 && !e.meta && !e.ctrl && !e.alt && !e.shift) || (hk === 'Super+`' && e.meta && e.code === 41);
};
// it goes before the theme's table: with ❖` chosen here, the key is this window's
addShortcuts([{ id: 'terminal.hotkey', keys: ['F12', 'Meta+`'], when: isHotkey, run: () => toggleHotkeyWindow() }]);

const MODIFIER_CODES = new Set([29, 97, 42, 54, 56, 100, 125, 126, 58]);

// ---- navegación entre paneles ----

function cyclePane(wid, d) {
  const tab = activeTab(getWin(wid));
  const ls = leaves(tab.root);
  const i = ls.indexOf(tab.focus);
  focusPane(wid, ls[(i + d + ls.length) % ls.length]);
}

function paneInDirection(wid, dx, dy) {
  const tab = activeTab(getWin(wid));
  const rect = (sid) => { const n = paneNodes.get(sid); return n ? lord.getLayout(n.id) : null; };
  const cur = rect(tab.focus);
  if (!cur) return;
  const cx = cur[0] + cur[2] / 2, cy = cur[1] + cur[3] / 2;
  let best = null, bestD = Infinity;
  for (const sid of leaves(tab.root)) {
    if (sid === tab.focus) continue;
    const r = rect(sid);
    if (!r) continue;
    const ox = r[0] + r[2] / 2 - cx, oy = r[1] + r[3] / 2 - cy;
    if ((dx && Math.sign(ox) !== dx) || (dy && Math.sign(oy) !== dy)) continue;
    const d = Math.abs(ox) + Math.abs(oy) * (dx ? 3 : 1) + Math.abs(ox) * (dy ? 3 : 0);
    if (d < bestD) { bestD = d; best = sid; }
  }
  if (best) focusPane(wid, best);
}

// ---- acciones (atajos y menús) ----

function sendTo(w, fn) {
  const tab = activeTab(w);
  for (const s of w.broadcast ? leaves(tab.root) : [tab.focus]) fn(s);
}

function run(wid, action) {
  const w = getWin(wid);
  if (!w) return;
  const tab = activeTab(w);
  const sid = tab?.focus;
  if (sid != null && tmuxAction(action, sid, tab)) return;     // panel de tmux -CC
  const m = /^tab(\d)$/.exec(action);
  if (m) { const tb = w.tabs[+m[1] - 1]; if (tb) selectTab(wid, tb.id); return; }
  switch (action) {
    case 'newTab': addTab(wid); break;
    case 'newWindow': {
      // tamaño en columnas × filas del perfil por defecto (Ventana → columnas/filas)
      const dp = getProfile(terminalSettings().defaultProfile);
      const info = term.info(sid), cur = wm.get(wid);
      const frame = dp.columns && dp.rows && info?.cellWidth && cur ? {
        x: cur.x + 30, y: cur.y + 30,
        w: Math.round(dp.columns * info.cellWidth + 2 * dp.padding + 4),
        h: Math.round(dp.rows * info.cellHeight + 2 * dp.padding + 64 + (dp.statusBar ? 24 : 0)),
      } : undefined;
      wm.open('terminal', { new: true, frame });
      break;
    }
    case 'closePane': closePane(wid, sid); break;
    case 'closeTab': closeTab(wid, tab.id); break;
    case 'splitRight': split(wid, 'row'); break;
    case 'splitDown': split(wid, 'column'); break;
    case 'nextPane': cyclePane(wid, 1); break;
    case 'prevPane': cyclePane(wid, -1); break;
    case 'paneLeft': paneInDirection(wid, -1, 0); break;
    case 'paneRight': paneInDirection(wid, 1, 0); break;
    case 'paneUp': paneInDirection(wid, 0, -1); break;
    case 'paneDown': paneInDirection(wid, 0, 1); break;
    case 'nextTab': cycleTab(wid, 1); break;
    case 'prevTab': cycleTab(wid, -1); break;
    case 'find': patchWin(wid, (x) => ({ find: x.find || { query: '', cs: false } })); break;
    case 'findNext': term.searchMove(sid, -1); break;
    case 'findPrev': term.searchMove(sid, 1); break;
    case 'copy': { const txt = term.copy(sid); if (txt) clipboard.remember(txt); break; }
    case 'paste': {
      // an image on the clipboard goes in as its file's path (paths.js): Claude Code attaches it
      const img = clipboardImage(lord.clipboard.state());
      if (img) { sendTo(w, (s) => term.paste(s, pathsText([img]))); break; }
      const txt = clipboard.get();
      if (txt) sendTo(w, (s) => term.paste(s, txt));
      break;
    }
    case 'pasteHistory': openPasteHistory(wid); break;
    case 'selectAll': term.select(sid, 'all'); break;
    case 'selectOutput':
      if (term.select(sid, 'lastOutput')) { const txt = term.copy(sid); clipboard.remember(txt); toast(wid, tr('terminal.toast.outputCopied')); }
      else toast(wid, tr('terminal.toast.noOutput'));
      break;
    case 'clearBuffer': term.clear(sid, 'buffer'); break;
    case 'zoomIn': zoom(wid, 1); break;
    case 'zoomOut': zoom(wid, -1); break;
    case 'zoomReset': zoom(wid, 0); break;
    case 'maximizePane': toggleZoom(wid); break;
    case 'fullscreen': wm.toggleMaximize(wid); break;
    case 'prevMark': term.mark(sid, 'prev'); break;
    case 'nextMark': term.mark(sid, 'next'); break;
    case 'setMark': term.mark(sid, 'set'); toast(wid, 'Marca agregada'); break;
    case 'scrollTop': term.scroll(sid, 'top'); break;
    case 'scrollBottom': term.scroll(sid, 'bottom'); break;
    case 'scrollPageUp': term.scroll(sid, 'pageUp'); break;
    case 'scrollPageDown': term.scroll(sid, 'pageDown'); break;
    case 'scrollLineUp': term.scroll(sid, 1); break;
    case 'scrollLineDown': term.scroll(sid, -1); break;
    case 'broadcast': patchWin(wid, (x) => ({ broadcast: !x.broadcast })); break;
    case 'autocomplete': openAutocomplete(wid, sid); break;
    case 'composer': patchWin(wid, (x) => ({ composer: !x.composer })); break;
    case 'openQuickly': patchWin(wid, () => ({ popup: { kind: 'quick' } })); break;
    case 'preferences': patchWin(wid, () => ({ prefs: true })); break;
    case 'toolbelt': patchWin(wid, (x) => ({ toolbelt: !x.toolbelt })); break;
    case 'timestamps': toggleTimestamps(wid); break;
    case 'editTitle': patchWin(wid, () => ({ popup: { kind: 'title' } })); break;
    case 'reset': term.clear(sid, 'reset'); break;
    case 'copyMode': term.copyMode(sid, true); patchWin(wid, () => ({ copyMode: sid })); break;
    case 'instantReplay': {
      const r = term.replay(sid, 'enter');
      if (r && r.count) patchWin(wid, () => ({ replay: { sid, ...r } }));
      else toast(wid, tr('terminal.toast.noReplay'));
      break;
    }
    case 'commandHistory': openCommandHistory(wid); break;
    case 'undoClose': undoClose(wid); break;
    case 'alertNextMark': toggleAlert(wid, sid); break;
    case 'equalize': equalize(wid); break;
    case 'paneToTab': paneToTab(wid); break;
    case 'moveTabLeft': moveTab(wid, tab.id, -1); break;
    case 'moveTabRight': moveTab(wid, tab.id, 1); break;
    case 'logSession': {
      const on = !term.info(sid)?.logging;
      const path = term.log(sid, on);
      toast(wid, on ? (path ? tr('terminal.toast.logging', { path: path.replace(lord.fs.home(), '~') }) : tr('terminal.toast.logFailed')) : tr('terminal.toast.logStopped'));
      break;
    }
    case 'exportText': {
      const path = term.export(sid);
      toast(wid, path ? tr('terminal.toast.saved', { path: path.replace(lord.fs.home(), '~') }) : tr('terminal.toast.saveFailed'));
      break;
    }
    case 'pasteOneLine': { const txt = clipboard.get().replace(/\s*\n\s*/g, ' ').trim(); if (txt) sendTo(w, (x) => term.paste(x, txt)); break; }
    case 'pasteEscaped': { const txt = clipboard.get(); if (txt) sendTo(w, (x) => term.paste(x, `'${txt.replace(/'/g, "'\\''")}'`)); break; }
    case 'saveArrangement': toast(wid, tr('terminal.toast.arrangementSaved', { count: saveArrangement() })); break;
    case 'restoreArrangement': if (!restoreArrangement()) toast(wid, tr('terminal.toast.noArrangement')); break;
    default: break;
  }
}

// what the active pane has selected (Edit → Speech, ❖Esc)
const selectionOf = (wid) => { const s = activeSid(getWin(wid)); return s ? term.text(s, 'selection') : ''; };

function menus(wid, w) {
  const item = (action, extra = {}) => ({ label: actionLabel(action), hint: hint(action), onClick: () => run(wid, action), ...extra });
  const tab = activeTab(w);
  const multi = tab && leaves(tab.root).length > 1;
  const profiles = terminalSettings().profiles;
  return [
    { label: tr('terminal.menu.shell'), items: [
      item('newTab'), item('newWindow'), '-',
      item('splitRight'), item('splitDown'), '-',
      item('broadcast', { checked: w.broadcast }),
      item('composer', { checked: w.composer }),
      item('alertNextMark', { checked: !!metaStore.get()[tab?.focus]?.alert }), '-',
      item('logSession'), item('exportText'), '-',
      item('saveArrangement'), item('restoreArrangement'), '-',
      item('closePane'), item('closeTab'), item('undoClose'),
    ] },
    { label: tr('terminal.menu.edit'), items: [
      item('copy'), item('paste'), item('pasteHistory'),
      item('pasteOneLine'), item('pasteEscaped'), '-',
      item('copyMode'), item('commandHistory'), '-',
      item('selectAll'), item('selectOutput'), '-',
      speechMenu(() => selectionOf(wid)), '-',
      item('find'), item('findNext'), item('findPrev'), '-',
      item('autocomplete'), item('clearBuffer'), item('reset'),
    ] },
    { label: tr('terminal.menu.view'), items: [
      item('zoomIn'), item('zoomOut'), item('zoomReset'), '-',
      item('maximizePane', { checked: tab?.zoom, disabled: !multi }), item('fullscreen'),
      item('equalize', { disabled: !multi }), item('paneToTab', { disabled: !multi }), '-',
      item('instantReplay'),
      item('prevMark'), item('nextMark'), item('setMark'), '-',
      item('timestamps', { checked: w.timestamps }), item('toolbelt', { checked: w.toolbelt }),
      item('openQuickly'), item('editTitle'),
    ] },
    { label: tr('terminal.menu.profiles'), items: [
      ...profileMenu(profiles, (p) => ({ label: tr('terminal.menu.newTabWith', { name: profileName(p) }), onClick: () => addTab(wid, p.id) })), '-',
      item('preferences'),
    ] },
  ];
}

// ---- ventana ----

export default function TerminalApp() {
  const t = useTheme();
  const { id: wid, focused, args } = useAppWindow();
  if (!getWin(wid)) ensureWin(wid, args);
  const w = useStore(wins, (x) => x[wid]);
  const allMeta = useStore(meta);
  const root = useRef(null);
  const [ctx, setCtx] = useState(null);
  const tab = w ? activeTab(w) : null;
  const sid = tab?.focus ?? null;
  const profile = getProfile(allMeta[sid]?.profile);

  // título de la ventana = el de la pestaña activa
  useEffect(() => { if (tab) wm.setTitle(wid, tabTitle(tab)); });

  // menús en la barra del tema
  useEffect(() => { if (w) wm.setMenus(wid, menus(wid, w)); }, [w]);

  // confirmar si hay procesos corriendo al cerrar la ventana
  useEffect(() => wm.setCloseGuard(wid, () => {
    const cur = getWin(wid);
    if (!cur) return true;
    const sids = cur.tabs.flatMap((x) => leaves(x.root));
    if (!busyIn(sids).length) return true;
    patchWin(wid, () => ({ dialog: { text: tr('terminal.dialog.closeWindow'), detail: tr('terminal.dialog.busy', { list: [...new Set(busyIn(sids))].join(', ') }),
      onConfirm: () => wm.close(wid, true) } }));
    return false;
  }), [wid]);

  // ❖Esc reads the active pane's selection aloud (sdk/voice.js)
  useEffect(() => registerSelection(wid, () => selectionOf(wid)), [wid]);

  // proceso en primer plano de cada sesión (títulos de pestañas)
  useEffect(() => {
    const poll = () => {
      const cur = getWin(wid);
      if (!cur) return;
      for (const s of cur.tabs.flatMap((x) => leaves(x.root))) {
        const info = term.info(s);
        const m = meta.get()[s];
        if (info && m && (m.process !== info.process || m.busy !== info.busy))
          meta.set((x) => ({ ...x, [s]: { ...x[s], process: info.process, busy: info.busy } }));
      }
    };
    poll();
    const id = setInterval(poll, 1500);
    return () => clearInterval(id);
  }, [wid]);

  useKeyboard((e) => {
    const cur = getWin(wid);
    if (!cur) return;
    const s = activeSid(cur);
    const cap = keyCapture.get();
    if (cap) {
      if (MODIFIER_CODES.has(e.code)) return;
      keyCapture.set(null);
      if (e.key !== 'Escape' || e.meta || e.ctrl || e.alt || e.shift) { const c = comboOf(e); if (c) cap.onCombo(c); }
      return;
    }
    if (cur.prefs) { if (e.key === 'Escape') patchWin(wid, () => ({ prefs: false })); return; }
    if (cur.dialog) { if (e.key === 'Escape') patchWin(wid, () => ({ dialog: null })); if (e.key === 'Enter') { const fn = cur.dialog.onConfirm; patchWin(wid, () => ({ dialog: null })); fn(); } return; }
    if (popupKey(wid, cur, s, e)) return;
    if (cur.copyMode != null) {
      const k = e.char || (e.ctrl ? e.base : '') || e.key;
      if (!k || MODIFIER_CODES.has(e.code)) return;
      const r = term.copyKey(cur.copyMode, k, e.ctrl);
      if (r.status === 2 && r.text) { clipboard.remember(r.text); toast(wid, tr('terminal.toast.copied')); }
      if (r.status !== 0) patchWin(wid, () => ({ copyMode: null }));
      return;
    }
    if (cur.replay) {
      const r = cur.replay;
      if (e.key === 'Escape' || e.key === 'Enter') { term.replay(r.sid, 'exit'); patchWin(wid, () => ({ replay: null })); }
      else if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
        const n = term.replay(r.sid, Math.max(0, r.index + (e.key === 'ArrowLeft' ? -1 : 1)));
        patchWin(wid, () => ({ replay: { sid: r.sid, ...n } }));
      }
      return;
    }
    if (isHotkey(e)) return;
    // la puerta de tmux -CC no recibe teclas (es el canal de comandos); Esc desconecta
    if (s != null && isGateway(s)) { if (e.key === 'Escape') detach(s); else if (actionOf(e)) run(wid, actionOf(e)); return; }
    const mapping = MODIFIER_CODES.has(e.code) ? null : keyMappingOf(e, getProfile(meta.get()[s]?.profile));
    if (mapping) {
      const bytes = mappingBytes(mapping);
      if (bytes != null) sendTo(cur, (x) => term.write(x, bytes));
      else if (mapping.action !== 'ignore') run(wid, mapping.action);
      return;
    }
    const action = actionOf(e);
    if (action) { run(wid, action); return; }
    if (MODIFIER_CODES.has(e.code) || e.meta || s == null) return;
    // Ctrl+V in Claude Code with an image on the clipboard: its path goes in. Claude Code looks for the image
    // with xclip or wl-paste, which don't reach LordOS's clipboard; it attaches a pasted image path the same way.
    if (e.ctrl && !e.alt && !e.shift && e.code === 47 && isClaude(term.info(s)?.process)) {
      const img = clipboardImage(lord.clipboard.state());
      if (img) { sendTo(cur, (x) => term.paste(x, pathsText([img]))); return; }
    }
    const key = e.key && e.key.length > 1 ? e.key : '';
    const base = e.ctrl || e.alt ? e.base || '' : '';
    if (!key && !e.char && !base) return;
    sendTo(cur, (x) => term.key(x, key, e.char || '', base, e.mods & 7));
  });

  if (!w || !tab) return <View style={{ flex: 1, backgroundColor: '#000' }} />;
  const scheme = schemeOf(profile);
  const paneBg = scheme.bg;

  // a tab's menu: its color (over its profile's, as in other terminals), title and closing
  const onTabMenu = (e, tb) => {
    const [rx, ry] = lord.getLayout(root.current.id);
    const setColor = (hex) => patchWin(wid, (x) => ({ tabs: x.tabs.map((o) => (o.id === tb.id ? { ...o, color: hex } : o)) }));
    setCtx({ x: e.x - rx, y: e.y - ry, items: [
      { label: tr('terminal.prefs.tabColor'), disabled: true, onClick: () => {} },
      { swatches: [null, ...TAB_COLORS].map((hex) => ({ hex, on: (tb.color || null) === hex, onClick: () => setColor(hex) })) },
      '-',
      { label: actionLabel('editTitle'), onClick: () => { selectTab(wid, tb.id); run(wid, 'editTitle'); } },
      { label: actionLabel('closeTab'), onClick: () => closeTab(wid, tb.id) },
    ] });
  };

  const onContextMenu = (e, s) => {
    const [rx, ry] = lord.getLayout(root.current.id);
    const info = term.info(s);
    setCtx({ x: e.x - rx, y: e.y - ry, items: [
      { label: actionLabel('copy'), hint: hint('copy'), disabled: !info?.hasSelection, onClick: () => run(wid, 'copy') },
      { label: actionLabel('paste'), hint: hint('paste'), onClick: () => run(wid, 'paste') },
      speechMenu(() => term.text(s, 'selection')),
      '-',
      { label: actionLabel('splitRight'), hint: hint('splitRight'), onClick: () => run(wid, 'splitRight') },
      { label: actionLabel('splitDown'), hint: hint('splitDown'), onClick: () => run(wid, 'splitDown') },
      '-',
      { label: actionLabel('selectAll'), onClick: () => run(wid, 'selectAll') },
      { label: actionLabel('clearBuffer'), onClick: () => run(wid, 'clearBuffer') },
      { label: actionLabel('find'), onClick: () => run(wid, 'find') },
      '-',
      { label: actionLabel('preferences'), hint: '❖,', onClick: () => run(wid, 'preferences') },
      { label: actionLabel('closePane'), onClick: () => closePane(wid, s) },
    ] });
  };

  return (
    <View ref={root} style={{ flex: 1 }}>
      <TabBar t={t} wid={wid} w={w} onTabMenu={onTabMenu} />
      <View style={{ flex: 1, flexDirection: 'row' }}>
        <View style={{ flex: 1, flexDirection: 'row' }}>
          <PaneTree wid={wid} tab={tab} windowFocused={focused} divider={t.separator} bg={paneBg} onContextMenu={onContextMenu} />
          {w.find && sid != null && <FindBar key={sid} t={t} wid={wid} sid={sid} find={w.find} />}
        </View>
        {w.toolbelt && <Toolbelt t={t} wid={wid} w={w} sid={sid} />}
      </View>
      {w.replay && <ReplayBar t={t} wid={wid} replay={w.replay} />}
      {w.copyMode != null && (
        <View style={{ height: 24, justifyContent: 'center', paddingHorizontal: 12, backgroundColor: '#ff9f0a' }}>
          <Text style={{ fontSize: 12, fontWeight: 700, color: '#000' }}>{tr('terminal.copyModeBar')}</Text>
        </View>
      )}
      {profile.statusBar && sid != null && <StatusBar t={t} sid={sid} wid={wid} />}
      <Overlays t={t} wid={wid} w={w} sid={sid} tab={tab} />
      {ctx && <ContextMenu x={ctx.x} y={ctx.y} items={ctx.items} onClose={() => setCtx(null)} bounds={{ w: lord.getLayout(root.current.id)[2], h: lord.getLayout(root.current.id)[3] }} />}
      {w.prefs && <Preferences wid={wid} onClose={() => patchWin(wid, () => ({ prefs: false }))} />}
    </View>
  );
}
