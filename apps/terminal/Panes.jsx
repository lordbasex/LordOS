// Panes.jsx — un panel de terminal y el árbol de paneles divididos.
import { View, Text, Terminal, addListener, term, clipboard, useStore, wm, fs, ime, openUrl, t as T } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { meta, setRatio, focusPane, toast } from './state.js';
import { tmuxFocus } from './tmux.js';
import { getProfile } from './profiles.js';
import { SecretBar } from './SecretBar.jsx';
import { SaveBar } from './SaveBar.jsx';
import { ClaudeBar } from './ClaudeBar.jsx';
import { pathsText } from './paths.js';
import { dropUpload, downloadName, dropTarget } from './dropupload.js';
import { nameAt } from './lsnames.js';

export const paneNodes = new Map();     // sid → instancia del nodo (para navegar entre paneles)

// Ctrl/❖+clic: carpetas y archivos de la carpeta personal se abren en Archivos ("semantic
// history"); web links are copied (LordOS has no browser yet).
function openLink(wid, sid, url) {
  const web = /^https?:\/\//i.test(url) ? url : url.startsWith('www.') ? `https://${url}` : '';
  if (web && openUrl(web)) return;
  const home = fs.home();
  let path = url.startsWith('file://') ? decodeURIComponent(url.replace(/^file:\/\/[^/]*/, '')) : url;
  if (path.startsWith('~')) path = home + path.slice(1);
  else if (path.startsWith('./')) path = `${term.info(sid)?.cwd || home}/${path.slice(2)}`;
  if (path.startsWith('/') && (path === home || path.startsWith(`${home}/`))) {
    const rel = path.slice(home.length + 1);
    let dir = rel;
    try { lord.fs.list(rel || '.'); } catch (e) { dir = fs.parent(rel); }
    wm.open('files', { args: { path: dir ? `${home}/${dir}` : home } });
    return;
  }
  clipboard.set(url);
  toast(wid, T('terminal.toast.linkCopied', { url: url.length > 60 ? `${url.slice(0, 57)}…` : url }));
}

export function TermPane({ wid, sid, active, windowFocused, multi, onContextMenu }) {
  const ref = useRef(null);
  const dragging = useRef(false);
  const m = useStore(meta, (x) => x[sid]) || {};
  const p = getProfile(m.profile);
  const [flash, setFlash] = useState(false);
  // files dragged from Files over the pane: what letting go does, shown before it happens
  const [over, setOver] = useState(null);       // { count, upload, dest }
  const onFilesOver = (paths, how) => {
    if (!paths) { setOver(null); return; }
    const i = term.info(sid);
    const upload = !how?.alt && !!(i && i.busy && i.process === 'ssh');
    setOver({ count: paths.length, upload, dest: upload ? dropTarget(sid) : '' });
  };

  useEffect(() => { term.focus(sid, active && windowFocused); }, [sid, active, windowFocused]);

  // Método de entrada (coreano, japonés, chino): la composición y los candidatos se muestran
  // sobre el cursor de la terminal; el texto confirmado llega como cualquier tecla.
  useEffect(() => {
    if (!active || !windowFocused) return undefined;
    const token = {};
    ime.attach(token, () => {
      if (!ref.current) return null;
      const info = term.info(sid);
      if (!info) return null;
      const [x, y] = lord.getLayout(ref.current.id);
      return { x: x + info.padding + info.cursorX * info.cellWidth, y: y + info.padding + info.cursorY * info.cellHeight, h: info.cellHeight };
    });
    return () => ime.detach(token);
  }, [sid, active, windowFocused]);

  useEffect(() => {
    if (!m.flash) return undefined;
    setFlash(true);
    const id = setTimeout(() => setFlash(false), 140);
    return () => clearTimeout(id);
  }, [m.flash]);

  useEffect(() => {
    const local = (e) => { const [x, y] = lord.getLayout(ref.current.id); return { x: e.x - x, y: e.y - y }; };
    const offMove = addListener('pointermove', (e) => {
      if (!dragging.current || !ref.current) return;
      const l = local(e);
      term.mouse(sid, 'move', l.x, l.y, 0, e.mods);
    });
    const offUp = addListener('pointerup', (e) => {
      if (!dragging.current || !ref.current) return;
      dragging.current = false;
      const l = local(e);
      if (term.mouse(sid, 'up', l.x, l.y, e.button, e.mods) === 3) clipboard.remember(lord.clipboard.get());
    });
    return () => { offMove(); offUp(); };
  }, [sid]);

  const onMouseDown = (e) => {
    focusPane(wid, sid);
    if (m.gateway) tmuxFocus(sid);
    const l = e.local(ref.current);
    // a file name of `ls` output in a pane that runs ssh (lsnames.js): ❖ + click downloads it, the menu offers it
    const remoteName = () => { const i = term.info(sid); if (!i || !i.busy || i.process !== 'ssh') return null; const a = term.lineAt(sid, l.x, l.y); const n = a ? nameAt(a.before, a.after) : null; return n ? { ...n, line: a.before + a.after } : null; };
    if (e.button === 2) { onContextMenu?.(e, sid, remoteName()); return; }
    if (e.button === 0 && (e.meta || e.ctrl)) {
      const url = term.urlAt(sid, l.x, l.y);
      if (url) { openLink(wid, sid, url); return; }
      const n = remoteName();
      if (n && downloadName(wid, sid, n.name, n.line)) return;
    }
    // a middle click the program in the pane doesn't take pastes the primary selection (lord.primary gives it only now,
    // and nothing with middle-click paste off in Settings → Mouse & Trackpad)
    if (term.mouse(sid, 'down', l.x, l.y, e.button, e.mods) === 4) {
      const text = lord.primary ? lord.primary.get() : '';
      if (text) term.paste(sid, text);
      return;
    }
    dragging.current = true;
  };

  const badge = m.badge;
  const transparent = (p.transparency || 0) > 0;
  return (
    <View onFilesOver={onFilesOver} onFilesDrop={(paths, how) => { setOver(null); focusPane(wid, sid); if (!how?.alt && dropUpload(wid, sid, paths)) return; term.paste(sid, pathsText(paths)); }}
      style={{ flex: 1, overflow: 'hidden', backdropFilter: transparent && p.blur ? 'blur(14px)' : undefined }}>
      <Terminal ref={(inst) => { ref.current = inst; if (inst) paneNodes.set(sid, inst); else paneNodes.delete(sid); }}
        session={sid} onMouseDown={onMouseDown}
        onMouseMove={(e) => { if (!dragging.current) { const l = e.local(ref.current); term.mouse(sid, 'move', l.x, l.y, 0, e.mods); } }}
        onWheel={(e) => { if (!e.notches) return; const l = e.local(ref.current); term.wheel(sid, e.notches * 3, l.x, l.y, e.mods); }}
        style={{ flex: 1 }} />
      {badge ? (
        <Text style={{ position: 'absolute', right: 18, top: 10, fontSize: 30, fontWeight: 700, color: 'rgba(255,255,255,0.13)',
          pointerEvents: 'none', whiteSpace: 'nowrap' }}>{badge}</Text>
      ) : null}
      {m.tmux ? (
        <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.72)',
          alignItems: 'center', justifyContent: 'center', gap: 6 }}>
          <Text style={{ fontSize: 15, fontWeight: 700, color: '#fff' }}>{T('terminal.tmux.connected')}</Text>
          <Text style={{ fontSize: 12, color: 'rgba(255,255,255,0.7)' }}>{T('terminal.tmux.gateway')}</Text>
          <Text style={{ fontSize: 12, color: 'rgba(255,255,255,0.7)' }}>{T('terminal.tmux.escape')}</Text>
        </View>
      ) : null}
      {multi && !active && p.dimInactive && (
        <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.28)', pointerEvents: 'none' }} />
      )}
      {flash && <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(255,255,255,0.14)', pointerEvents: 'none' }} />}
      {over ? (
        <View style={{ position: 'absolute', left: 8, top: 8, right: 8, bottom: 8, borderRadius: 10, pointerEvents: 'none', alignItems: 'center', justifyContent: 'center', gap: 6,
          border: `2px dashed ${over.upload ? '#ff9f0a' : 'rgba(255,255,255,0.45)'}`, backgroundColor: over.upload ? 'rgba(255,159,10,0.12)' : 'rgba(255,255,255,0.06)' }}>
          <Text style={{ fontSize: 15, fontWeight: 700, color: '#fff', textAlign: 'center' }}>{over.upload ? T('terminal.drop.overUpload', { count: over.count }) : T('terminal.drop.overPaste', { count: over.count })}</Text>
          {over.upload ? <Text style={{ fontSize: 12.5, color: 'rgba(255,255,255,0.85)', textAlign: 'center', direction: 'ltr' }}>{over.dest}</Text> : null}
          {over.upload ? <Text style={{ fontSize: 11, color: 'rgba(255,255,255,0.6)', textAlign: 'center' }}>{T('terminal.drop.overAlt')}</Text> : null}
        </View>
      ) : null}
      {m.secret && m.secretDismissed !== m.secret ? <SecretBar sid={sid} wid={wid} prompt={m.secret} /> : null}
      {m.saveOffer && !m.secret ? <SaveBar sid={sid} wid={wid} offer={m.saveOffer} /> : null}
      {m.claudeConnect && !m.secret && !m.saveOffer ? <ClaudeBar sid={sid} wid={wid} /> : null}
    </View>
  );
}

function Divider({ dir, onDrag, color, bg }) {
  const [hover, setHover] = useState(false);
  const row = dir === 'row';
  return (
    <View onMouseDown={onDrag} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ [row ? 'width' : 'height']: 5, alignItems: 'center', justifyContent: 'center', flexDirection: row ? 'row' : 'column',
        backgroundColor: bg }}>
      <View style={{ [row ? 'width' : 'height']: hover ? 3 : 1, [row ? 'height' : 'width']: '100%', backgroundColor: hover ? '#0a84ff' : color }} />
    </View>
  );
}

function Split({ wid, tab, node, render, color, bg }) {
  const ref = useRef(null);
  const drag = useRef(false);
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      if (!drag.current || !ref.current) return;
      const [x, y, w, h] = lord.getLayout(ref.current.id);
      setRatio(wid, tab.id, node.id, node.dir === 'row' ? (e.x - x) / w : (e.y - y) / h);
    });
    const offUp = addListener('pointerup', () => { drag.current = false; });
    return () => { offMove(); offUp(); };
  }, [wid, tab.id, node.id, node.dir]);
  return (
    // los paneles van siempre de izquierda a derecha (como el contenido de la terminal), también
    // con la interfaz en árabe o hebreo; así el arrastre del divisor usa la x tal cual
    <View ref={ref} style={{ flex: 1, flexDirection: node.dir, direction: 'ltr' }}>
      <View style={{ flex: node.ratio, flexDirection: 'row' }}>{render(node.a)}</View>
      <Divider dir={node.dir} color={color} bg={bg} onDrag={(e) => { e.stopPropagation(); drag.current = true; }} />
      <View style={{ flex: 1 - node.ratio, flexDirection: 'row' }}>{render(node.b)}</View>
    </View>
  );
}

export function PaneTree({ wid, tab, windowFocused, divider, bg, onContextMenu }) {
  const multi = tab.root.type === 'split';
  const leaf = (sid) => (
    <TermPane key={sid} wid={wid} sid={sid} active={tab.focus === sid} windowFocused={windowFocused}
      multi={multi && !tab.zoom} onContextMenu={onContextMenu} />
  );
  if (tab.zoom) return leaf(tab.focus);
  const render = (n) => (n.type === 'leaf' ? leaf(n.sid)
    : <Split key={n.id} wid={wid} tab={tab} node={n} render={render} color={divider} bg={bg} />);
  return render(tab.root);
}
