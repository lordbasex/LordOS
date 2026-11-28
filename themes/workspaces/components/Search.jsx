// Search: Workspaces' panel over the desktop (the overlay layer, so a Linux app doesn't keep the keyboard), on the
// display under the pointer. ❖Space, Ctrl+Space or ❖ alone open it; typing starts at the first character; ↑/↓ choose,
// Enter opens, Esc closes. It is LordOS's own search (sdk/spotlight.js searchEverything: apps, files, Settings, notes,
// the calculator, Help…) plus two sections this theme registers: Activities and Open windows (index.jsx). With nothing
// typed (or opened from the spine's Apps) it shows the activities and every app.
// It drops in from a little above and settles (motion.js, 240 ms; nothing with Reduce motion).
import {
  View, Text, Image, ScrollView, SearchField, addListener, useStore, useLauncherApps, launchApp, searchEverything, tOptional, desktopList, desktopName,
  activeDesktop, switchDesktop, compareNames, t as tr,
} from '@lordos/sdk';
import { aimLaunch, launchOn } from '../tiler.js';
import { appMenuItems } from './AppWindows.jsx';
import { Menu } from '../../classic/components/Menu.jsx';
import { memo, useEffect, useMemo, useRef, useState } from 'react';
import { Surface } from './Surface.jsx';
import { ActivityIcon, FolderGlyph, PageGlyph, SearchGlyph } from '../glyphs.jsx';
import { activities, noteLaunch } from '../state.js';
import { tween, EASE } from '../motion.js';

const W = 680;
const ROW = 44;

function resultGlyph(t, it, sec) {
  if (it.Icon) return <it.Icon size={26} />;
  if (it.image) return <Image src={it.image} style={{ width: 26, height: 26 }} />;
  if (sec === 'files') return it.glyph === '📁' ? <FolderGlyph color={t.accent} size={22} /> : <PageGlyph color={t.textDim} size={22} />;
  if (sec === 'notes') return <PageGlyph color={t.accent} size={22} />;
  const g = it.glyph && /^[=⇄⏎⚙]$/.test(it.glyph) ? it.glyph : '';
  return g ? <Text style={{ fontSize: 17, color: t.accent }}>{g}</Text> : <SearchGlyph color={t.textDim} size={16} />;
}

const Row = memo(function Row({ t, it, sec, selected, onPick, onHover }) {
  return (
    <View onClick={() => onPick(it)} onMouseEnter={onHover}
      style={{ height: ROW, flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 10, borderRadius: 10, backgroundColor: selected ? t.accent : 'transparent' }}>
      <View style={{ width: 28, height: 28, alignItems: 'center', justifyContent: 'center' }}>{resultGlyph(t, it, sec)}</View>
      <View style={{ flex: 1, minWidth: 0 }}>
        <Text style={{ fontSize: 13, fontWeight: 500, color: selected ? t.onAccent : t.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{it.title}</Text>
        {it.subtitle ? <Text style={{ fontSize: 11, color: selected ? 'rgba(255,255,255,0.82)' : t.textDim, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{it.subtitle}</Text> : null}
      </View>
    </View>
  );
});

const AppTile = memo(function AppTile({ t, a, selected, onPick, onMenu }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={(e) => onPick(a, e)} onContextMenu={(e) => onMenu(a, e)} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: 98, height: 86, alignItems: 'center', justifyContent: 'center', gap: 6, borderRadius: 12, paddingHorizontal: 4,
        backgroundColor: selected ? t.accent : hover ? t.hover : 'transparent' }}>
      <a.Icon size={36} />
      <Text style={{ fontSize: 11.5, color: selected ? t.onAccent : t.text, textAlign: 'center', numberOfLines: 2, maxWidth: 90 }}>{a.name}</Text>
    </View>
  );
});

// display: where it opens; isOnChrome(x, y): a click on the spine or the top band toggles it there instead of closing
export function Search({ t, display, onClose, isOnChrome }) {
  const box = useRef(null);
  const apps = useLauncherApps();
  const [query, setQuery] = useState('');
  const [sel, setSel] = useState(0);
  useStore(searchEverything.version());
  const list = useStore(desktopList);
  const active = useStore(activeDesktop);
  const meta = useStore(activities);
  // the file index (sdk/spotlight.js) only once something is typed: the Apps view never needs it
  const typed = query.trim() !== '';
  useEffect(() => { if (typed) searchEverything.prepare(); }, [typed]);
  useEffect(() => {
    const n = box.current?.id;
    if (n) { tween(n, { translateY: [-18, 0] }, { ms: 240, ease: EASE.back }); tween(n, { opacity: [0, 1] }, { ms: 140 }); }
  }, []);
  // right click on an app: its windows in this activity and New window, opened on this display (AppWindows.jsx)
  const [menu, setMenu] = useState(null);
  const menuBox = useRef(null);
  const openMenu = (a, e) => setMenu({ x: e.x, y: e.y, items: appMenuItems(a.id, active, display, () => { onClose(); }) });
  useEffect(() => addListener('pointerdown', (e) => {
    for (let n = e.target; n; n = n.parent) if (n === box.current || (menuBox.current && n === menuBox.current)) return;
    if (!isOnChrome(e.x, e.y)) onClose();
  }), []);
  useEffect(() => setSel(0), [query]);
  const sorted = useMemo(() => [...apps].sort((a, b) => compareNames(a.name, b.name)), [apps]);
  const sections = useMemo(() => (query.trim() ? searchEverything.results(query, apps) : []), [query, apps]);
  const flat = query.trim() ? sections.flatMap((s) => s.items.map((it) => ({ it, sec: s.id }))) : sorted.map((a) => ({ app: a }));
  const pick = (entry, e) => {
    if (!entry) return;
    if (e) noteLaunch(e.x, e.y);
    else noteLaunch(display.x + display.w / 2, display.y + display.h * 0.3);
    onClose();
    // an app opened from here shows on this display (the one the panel opened on), wherever there is room
    if (entry.app) launchOn(display, entry.app.id);
    else {
      if (String(entry.it?.key || '').startsWith('app:')) aimLaunch(display, 1, String(entry.it.key).slice(4));
      searchEverything.run(entry.it);
    }
  };
  const cols = 6;
  const onKey = (e) => {
    const step = query.trim() ? 1 : cols;
    if (e.key === 'ArrowDown') { setSel((v) => Math.min(flat.length - 1, v + step)); return true; }
    if (e.key === 'ArrowUp') { setSel((v) => Math.max(0, v - step)); return true; }
    if (!query.trim() && e.key === 'ArrowRight') { setSel((v) => Math.min(flat.length - 1, v + 1)); return true; }
    if (!query.trim() && e.key === 'ArrowLeft') { setSel((v) => Math.max(0, v - 1)); return true; }
    if (e.key === 'Enter') { pick(flat[Math.min(sel, flat.length - 1)]); return true; }
    return false;
  };
  const height = Math.min(560, display.h - 120);
  let k = 0;
  return (
    <Surface ref={box} t={t} radius={20} style={{ position: 'absolute', left: display.x + Math.round((display.w - W) / 2), top: display.y + Math.round(display.h * 0.12),
      width: W, height, padding: 14, gap: 10, backgroundColor: t.panelBg, ...(t.panelBlur ? { backdropFilter: `blur(${t.panelBlur}px)` } : null) }}>
      <SearchField value={query} onChange={setQuery} onKeyDown={onKey} autoFocus placeholder={tr('shell.ws.search.placeholder')} onCancel={onClose} style={{ height: 38 }} />
      {!query.trim() && list.length > 1 ? (
        <View style={{ flexDirection: 'row', gap: 6, flexWrap: 'wrap' }}>
          {list.map((id) => (
            <View key={id} onClick={() => { switchDesktop(id); onClose(); }}
              style={{ height: 30, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 10, borderRadius: 15,
                backgroundColor: id === active ? t.hover : 'transparent', border: `1px solid ${t.separator}` }}>
              <ActivityIcon icon={meta[id]?.icon} color={meta[id]?.color || t.accent} bg={t.solidBg} size={12} />
              <Text style={{ fontSize: 12, color: t.text, whiteSpace: 'nowrap' }}>{desktopName(id)}</Text>
            </View>
          ))}
        </View>
      ) : null}
      <ScrollView style={{ flex: 1 }}>
        {query.trim() ? (flat.length ? sections.map((s) => (
          <View key={s.id} style={{ marginBottom: 6 }}>
            <Text style={{ fontSize: 11, fontWeight: 600, color: t.textDim, marginHorizontal: 10, marginTop: 6, marginBottom: 2, textAlign: 'start' }}>
              {tOptional(`shell.spotlight.section.${s.id}`) || s.id}</Text>
            {s.items.map((it) => { const i = k++; return <Row key={it.key} t={t} it={it} sec={s.id} selected={i === sel} onPick={() => pick({ it })} onHover={() => setSel(i)} />; })}
          </View>
        )) : (
          <View style={{ height: 160, alignItems: 'center', justifyContent: 'center', gap: 8 }}>
            <SearchGlyph color={t.textDim} size={28} />
            <Text style={{ fontSize: 13, color: t.textDim }}>{tr('shell.ws.search.noResults', { query: query.trim() })}</Text>
          </View>
        )) : (
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 4 }}>
            {sorted.map((a, i) => <AppTile key={a.id} t={t} a={a} selected={i === sel} onPick={(x, e) => pick({ app: x }, e)} onMenu={openMenu} />)}
          </View>
        )}
      </ScrollView>
      {menu && <Menu t={t} x={menu.x} y={menu.y} width={280} boxRef={menuBox} onClose={() => setMenu(null)} items={menu.items} />}
    </Surface>
  );
}
