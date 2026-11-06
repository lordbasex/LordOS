// fileparts.jsx — what Files and the system file dialog (filepicker.jsx) draw the same way: the
// sidebar's rows, the list's rows, the crumbs of a path and the user's folders.
import { useEffect, useState } from 'react';
import { View, Text } from '@lordos/runtime';
import { direction, formatDate, formatTime } from './i18n.js';
import { typeScale, spacing } from './controls.jsx';
import { FolderGlyph } from './fileglyphs.jsx';
import * as fs from './fs.js';

// Source-list row: 28 px, accent-colored icon, a soft highlight when selected and the
// accent when something is dragged over it.
// accessory: always at the end of the row (Cloud's small usage ring); trail: only on hover or when selected (⏏)
export function SideItem({ label, icon, selected, onClick, t, drop, hot, dim, trail, onContext, onPress, accessory }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseDown={onPress} onContextMenu={onContext} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)} {...drop}
      style={{ height: 28, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 8, borderRadius: 6,
        backgroundColor: hot ? t.accent : selected ? (t.mode === 'dark' ? 'rgba(255,255,255,0.12)' : 'rgba(0,0,0,0.08)') : hover ? t.hover : 'transparent' }}>
      <View style={{ width: 18, alignItems: 'center', justifyContent: 'center' }}>{icon}</View>
      <Text style={{ flex: 1, fontSize: typeScale.body, color: hot ? '#fff' : dim ? t.textDim : t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{label}</Text>
      {accessory || null}
      {trail && (hover || selected) ? (
        <View onClick={(e) => { e.stopPropagation?.(); trail.onClick(); }} style={{ width: 18, alignItems: 'center' }}>
          <Text style={{ fontSize: 12, color: t.textDim }}>{trail.glyph}</Text>
        </View>
      ) : null}
    </View>
  );
}

export const SideTitle = ({ children, t, first }) => (
  <Text style={{ fontSize: typeScale.caption, fontWeight: 600, color: t.textDim, marginHorizontal: 8, marginTop: first ? 6 : 14, marginBottom: 2 }}>{children}</Text>
);
export const PlaceGlyph = ({ ch, t }) => <Text style={{ fontSize: 14, color: t.accent, textAlign: 'center' }}>{ch}</Text>;

// The list view: a row, its header's text and a cell after the name.
export const listRowStyle = (t, on, odd) => ({ flexDirection: 'row', alignItems: 'center', height: 24, paddingHorizontal: 10, borderRadius: 5,
  backgroundColor: on ? t.accent : odd ? (t.mode === 'dark' ? 'rgba(255,255,255,0.03)' : 'rgba(0,0,0,0.025)') : 'transparent' });
export const listHeaderCell = (t, on) => ({ fontSize: typeScale.caption, fontWeight: on ? 600 : 500, color: on ? t.text : t.textDim, whiteSpace: 'nowrap', overflow: 'hidden' });
export const listCell = (t, on, width, align) => ({ width, paddingHorizontal: 8, fontSize: typeScale.callout, color: on ? '#fff' : t.textDim, textAlign: align, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' });
export const when = (ms) => { if (!ms) return '—'; const d = new Date(ms); return `${formatDate(d, 'numeric')} ${formatTime(d)}`; };

// crumbs: [{ label, folder (shows the icon), drop (props for dragging onto it) }]; null is a gap
export function PathCrumbs({ crumbs, onGo, t }) {
  const arrow = direction() === 'rtl' ? '‹' : '›';
  return crumbs.map((c, i) => (
    <View key={i} style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.xs }}>
      {i > 0 && <Text style={{ fontSize: 11, color: t.textDim }}>{arrow}</Text>}
      {c ? (
        <View onClick={() => onGo(i)} {...c.drop} style={{ flexDirection: 'row', alignItems: 'center', gap: 5, height: 20, paddingHorizontal: 4, borderRadius: 4 }}>
          {c.folder ? <FolderGlyph size={13} /> : null}
          <Text style={{ fontSize: typeScale.caption, color: i === crumbs.length - 1 ? t.text : t.textDim, whiteSpace: 'nowrap' }}>{c.label}</Text>
        </View>
      ) : <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>…</Text>}
    </View>
  ));
}

// Width of a box after layout, re-measured while the window is resized.
export function useWidth(ref, initial) {
  const [w, setW] = useState(initial);
  useEffect(() => {
    const measure = () => { if (!ref.current) return; const lw = Math.round(lord.getLayout(ref.current.id)[2]); if (lw > 0) setW((x) => (Math.abs(x - lw) > 2 ? lw : x)); };
    measure();
    const id = setInterval(measure, 300);
    return () => clearInterval(id);
  }, []);
  return w;
}

// User folders (xdg-user-dirs already provides the names translated by the system).
const XDG = [['DESKTOP', 'desktop', ['Escritorio', 'Desktop', 'Área de Trabalho']], ['DOCUMENTS', 'documents', ['Documentos', 'Documents']],
  ['DOWNLOAD', 'downloads', ['Descargas', 'Downloads', 'Transferências']], ['PICTURES', 'pictures', ['Imágenes', 'Pictures', 'Imagens']],
  ['MUSIC', 'music', ['Música', 'Music']], ['VIDEOS', 'videos', ['Videos', 'Videos', 'Vídeos']]];
export function userDirs() {
  const home = fs.home();
  let conf = '';
  try { conf = fs.read('.config/user-dirs.dirs'); } catch (e) { /* no xdg-user-dirs */ }
  let existing;
  try { existing = new Set(lord.fs.list(home).filter((e) => e.dir).map((e) => e.name)); } catch (e) { existing = new Set(); }
  const out = [];
  for (const [key, label, names] of XDG) {
    const m = new RegExp(`^XDG_${key}_DIR="\\$HOME/([^"]+)"`, 'm').exec(conf);
    const dir = m ? m[1] : names.find((n) => existing.has(n));
    if (dir && existing.has(dir)) out.push({ label, path: `${home}/${dir}` });
  }
  return out;
}
