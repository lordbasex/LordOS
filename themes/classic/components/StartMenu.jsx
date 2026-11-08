// The Classic start menu: a search field, the pinned apps (a grid the user arranges), recently used
// apps, every app A–Z, and at the bottom the user and the power button (Lock, Switch User, Log Out,
// Restart…, Shut Down…: the session's own power sheet, sdk/power.jsx). Typing searches like the
// system search (apps, settings, files, notes, calculator, Help…; sdk/spotlight.js searchEverything).
// Right click on an app: pin or unpin it in the start menu or the taskbar; drag it onto the taskbar to
// pin it there. Keyboard: type to search, ↑/↓ and Enter, Escape closes.
import {
  View, Text, ScrollView, SearchField, addListener, t as tr, tOptional, useSettings, settings, useStore,
  useLauncherApps, launchApp, dockIdOf, dock, beginDockDrag, dockDragJustEnded, session, wm, useSystemInfo, searchEverything,
  mainDisplay, direction,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { Menu } from './Menu.jsx';
import { PowerGlyph, Chevron, FolderGlyph, PageGlyph, GearGlyph, SearchGlyph } from '../glyphs.jsx';
import { BAR_H, BAR_W } from './Taskbar.jsx';

export const START_DEFAULT = ['files', 'terminal', 'editor', 'calculator', 'notes', 'photos', 'player', 'mail', 'calendar',
  'contacts', 'store', 'settings', 'monitor', 'clock', 'help', 'about'];
const COLS = 6;

// The pinned list: the user's (settings.startPinned) or the defaults that are installed.
export function startPins(s, list) {
  const have = new Set(list.map((a) => a.id));
  return (Array.isArray(s.startPinned) ? s.startPinned : START_DEFAULT).filter((id) => have.has(id));
}
export function pinToStart(id, on) {
  const list = settings.get().startPinned;
  const cur = Array.isArray(list) ? list : START_DEFAULT;
  settings.set({ startPinned: on ? [...cur.filter((x) => x !== id), id] : cur.filter((x) => x !== id) });
}

function Tile({ t, a, onOpen, onMenu, onDrag }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={() => { if (!dockDragJustEnded()) onOpen(); }} onContextMenu={onMenu} onMouseDown={onDrag}
      onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: 92, height: 82, alignItems: 'center', justifyContent: 'center', gap: 6, borderRadius: 6, paddingHorizontal: 4,
        backgroundColor: hover ? t.hover : 'transparent' }}>
      <a.Icon size={32} />
      <Text style={{ fontSize: 12, color: t.text, textAlign: 'center', numberOfLines: 2, maxWidth: 84 }}>{a.name}</Text>
    </View>
  );
}

function Row({ t, Icon, glyph, title, subtitle, selected, onClick, onMenu, onDrag, onHover }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onContextMenu={onMenu} onMouseDown={onDrag}
      onMouseEnter={() => { setHover(true); onHover?.(); }} onMouseLeave={() => setHover(false)}
      style={{ height: 44, flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 10, borderRadius: 6,
        backgroundColor: selected ? t.hover : hover ? t.hover : 'transparent' }}>
      {selected ? <View style={{ position: 'absolute', insetInlineStart: 0, top: 14, width: 3, height: 16, borderRadius: 2, backgroundColor: t.accent }} /> : null}
      <View style={{ width: 28, height: 28, alignItems: 'center', justifyContent: 'center' }}>{Icon ? <Icon size={28} /> : glyph}</View>
      <View style={{ flex: 1, minWidth: 0 }}>
        <Text style={{ fontSize: 13, color: t.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{title}</Text>
        {subtitle ? <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{subtitle}</Text> : null}
      </View>
    </View>
  );
}

function Header({ t, title, action, onAction, back }) {
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', height: 32, paddingHorizontal: 8 }}>
      <Text style={{ flex: 1, fontSize: 13, fontWeight: 600, color: t.text, textAlign: 'start' }}>{title}</Text>
      {action ? (
        <View onClick={onAction} style={{ flexDirection: 'row', alignItems: 'center', gap: 4, height: 26, paddingHorizontal: 10, borderRadius: 4,
          backgroundColor: t.control, border: `1px solid ${t.tileBorder}` }}>
          {back ? <Chevron color={t.text} size={5} dir={direction() === 'rtl' ? 'right' : 'left'} /> : null}
          <Text style={{ fontSize: 12, color: t.text }}>{action}</Text>
          {back ? null : <Chevron color={t.text} size={5} dir={direction() === 'rtl' ? 'left' : 'right'} />}
        </View>
      ) : null}
    </View>
  );
}

// The glyph of a search result that has no app icon (the system search uses emoji there).
function resultGlyph(t, it, sectionId) {
  if (it.image) return null;
  if (sectionId === 'files') return it.subtitle && it.glyph === '📁' ? <FolderGlyph color={t.accent} size={22} /> : <PageGlyph color={t.textDim} size={22} />;
  if (sectionId === 'settings') return <GearGlyph color={t.textDim} bg={t.panelBg.startsWith('rgba') ? (t.mode === 'dark' ? '#26262b' : '#f6f6f8') : t.panelBg} size={18} />;
  if (sectionId === 'notes') return <PageGlyph color={t.accent} size={22} />;
  const g = it.glyph && /^[=⇄⏎]$/.test(it.glyph) ? it.glyph : '';
  return g ? <Text style={{ fontSize: 18, color: t.accent }}>{g}</Text> : <SearchGlyph color={t.textDim} size={16} />;
}

// box: the panel's place ({ left, top, width, height }, global coordinates)
export function StartMenu({ t, box, onClose, isOnBar }) {
  const ref = useRef(null);
  const menuRef = useRef(null);                        // the open context or power menu (a portal, outside ref)
  const list = useLauncherApps();
  const [s] = useSettings();
  const info = useSystemInfo(60000);
  const [query, setQuery] = useState('');
  const [view, setView] = useState('home');           // 'home' | 'all'
  const [ctx, setCtx] = useState(null);                // { app, x, y }
  const [power, setPower] = useState(null);
  const [sel, setSel] = useState(0);
  useStore(searchEverything.version());
  useEffect(() => { searchEverything.prepare(); }, []);
  useEffect(() => addListener('pointerdown', (e) => {
    for (let n = e.target; n; n = n.parent) if (n === ref.current || (menuRef.current && n === menuRef.current)) return;
    if (!isOnBar(e.x, e.y)) onClose();
  }), []);
  useEffect(() => setSel(0), [query]);

  const byId = new Map(list.map((a) => [a.id, a]));
  const pins = startPins(s, list).map((id) => byId.get(id)).filter(Boolean);
  const recent = (s.dockRecentApps || []).map((x) => list.find((a) => dockIdOf(a) === dockIdOf(x))).filter(Boolean)
    .filter((a, i, arr) => arr.indexOf(a) === i).slice(0, 6);
  const sorted = useMemo(() => [...list].sort((a, b) => a.name.localeCompare(b.name)), [list]);
  const sections = useMemo(() => (query.trim() ? searchEverything.results(query, list) : []), [query, list]);
  const flat = sections.flatMap((x) => x.items.map((it) => ({ it, sec: x.id })));

  const open = (a) => { onClose(); launchApp(a.id); };
  const drag = (a) => (e) => { if (!e.button && dockIdOf(a)) beginDockDrag({ id: dockIdOf(a), Icon: a.Icon, x: e.x, y: e.y, source: 'launcher', onStart: onClose }); };
  const menu = (a) => (e) => setCtx({ app: a, x: e.x, y: e.y });
  const run = (it) => { if (!it) return; onClose(); searchEverything.run(it); };
  const onKey = (e) => {
    if (!query.trim()) return false;
    if (e.key === 'ArrowDown') { setSel((v) => Math.min(flat.length - 1, v + 1)); return true; }
    if (e.key === 'ArrowUp') { setSel((v) => Math.max(0, v - 1)); return true; }
    if (e.key === 'Enter') { run(flat[Math.min(sel, flat.length - 1)]?.it); return true; }
    return false;
  };

  const name = info.fullName || info.user || '';
  const initials = name.split(/\s+/).filter(Boolean).slice(0, 2).map((w) => w[0].toUpperCase()).join('') || '?';
  const bodyH = box.height - 72 - 64;
  let body;
  if (query.trim()) {
    let k = 0;
    body = (
      <ScrollView style={{ height: bodyH }}>
        {flat.length ? sections.map((sec) => (
          <View key={sec.id} style={{ marginBottom: 6 }}>
            <Text style={{ fontSize: 11, fontWeight: 600, color: t.textDim, marginHorizontal: 10, marginTop: 6, marginBottom: 2, textAlign: 'start' }}>
              {tOptional(`shell.spotlight.section.${sec.id}`) || sec.id}</Text>
            {sec.items.map((it) => {
              const i = k++;
              return <Row key={it.key} t={t} Icon={it.Icon} glyph={resultGlyph(t, it, sec.id)} title={it.title} subtitle={it.subtitle}
                selected={i === sel} onHover={() => setSel(i)} onClick={() => run(it)}
                onMenu={it.preview?.kind === 'app' ? menu(it.preview.app) : undefined} />;
            })}
          </View>
        )) : (
          <View style={{ height: bodyH - 40, alignItems: 'center', justifyContent: 'center', gap: 8 }}>
            <SearchGlyph color={t.textDim} size={32} />
            <Text style={{ fontSize: 13, color: t.textDim }}>{tr('shell.classic.noResults', { query: query.trim() })}</Text>
          </View>
        )}
      </ScrollView>
    );
  } else if (view === 'all') {
    let letter = '';
    body = (
      <View style={{ height: bodyH }}>
        <Header t={t} title={tr('shell.classic.allApps')} action={tr('shell.classic.back')} back onAction={() => setView('home')} />
        <ScrollView style={{ flex: 1 }}>
          {sorted.map((a) => {
            const l = (a.name[0] || '#').toUpperCase();
            const head = l !== letter ? (letter = l) : null;
            return (
              <View key={a.id}>
                {head ? <Text style={{ fontSize: 12, fontWeight: 600, color: t.accent, marginHorizontal: 12, marginTop: 8, marginBottom: 2, textAlign: 'start' }}>{head}</Text> : null}
                <Row t={t} Icon={a.Icon} title={a.name} onClick={() => { if (!dockDragJustEnded()) open(a); }} onMenu={menu(a)} onDrag={drag(a)} />
              </View>
            );
          })}
        </ScrollView>
      </View>
    );
  } else {
    const rows = Math.ceil(pins.length / COLS);
    const gridH = Math.min(rows, recent.length ? 3 : 5) * 82;
    body = (
      <View style={{ height: bodyH, gap: 4 }}>
        <Header t={t} title={tr('shell.classic.pinned')} action={tr('shell.classic.allApps')} onAction={() => setView('all')} />
        {pins.length ? (
          <ScrollView style={{ height: gridH, flex: 0 }}>
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 0, paddingHorizontal: 2 }}>
              {pins.map((a) => <Tile key={a.id} t={t} a={a} onOpen={() => open(a)} onMenu={menu(a)} onDrag={drag(a)} />)}
            </View>
          </ScrollView>
        ) : <Text style={{ fontSize: 12, color: t.textDim, margin: 12, textAlign: 'start' }}>{tr('shell.classic.noPins')}</Text>}
        {recent.length ? (
          <View style={{ marginTop: 10 }}>
            <Header t={t} title={tr('shell.classic.recent')} />
            <View style={{ flexDirection: 'row', flexWrap: 'wrap' }}>
              {recent.map((a) => (
                <View key={a.id} style={{ width: '50%' }}>
                  <Row t={t} Icon={a.Icon} title={a.name} subtitle={tr('shell.classic.recentHint')} onClick={() => { if (!dockDragJustEnded()) open(a); }} onMenu={menu(a)} onDrag={drag(a)} />
                </View>
              ))}
            </View>
          </View>
        ) : null}
      </View>
    );
  }

  const ctxItems = ctx ? [
    { label: tr('shell.dock.open'), onClick: () => open(ctx.app) },
    '-',
    pins.includes(ctx.app)
      ? { label: tr('shell.classic.unpinStart'), onClick: () => pinToStart(ctx.app.id, false) }
      : { label: tr('shell.classic.pinStart'), onClick: () => pinToStart(ctx.app.id, true) },
    ...(dockIdOf(ctx.app) ? [dock.isPinned(ctx.app)
      ? { label: tr('shell.dock.unpin'), disabled: !dock.canRemove(ctx.app), onClick: () => dock.unpin(ctx.app) }
      : { label: tr('shell.dock.pin'), onClick: () => dock.pin(ctx.app) }] : []),
  ] : [];
  const powerItems = [
    ...(session.managed() ? [{ label: tr('shell.menu.lock'), onClick: () => session.lock() }, { label: tr('shell.menu.switchUser'), onClick: () => session.switchUser() }] : []),
    { label: tr('shell.menu.logout'), onClick: () => session.quit() },
    ...(session.canPower() ? ['-', { label: tr('shell.menu.restart'), onClick: () => session.restart() }, { label: tr('shell.menu.shutdown'), onClick: () => session.shutdown() }] : []),
  ];
  // the power glyph cuts its gap with the color behind it (opaque approximations of the footer)
  const footBg = t.mode === 'dark' ? '#232327' : '#efeff2';
  const hoverBg = t.mode === 'dark' ? '#2e2e33' : '#e6e6ea';
  return (
    <View ref={ref} style={{ position: 'absolute', ...box, borderRadius: 10, overflow: 'hidden',
      backgroundColor: t.panelBg, backdropFilter: t.blur ? `blur(${t.blur}px)` : undefined, border: `1px solid ${t.windowBorder}`, boxShadow: t.shadow }}>
      <View style={{ height: 72, paddingHorizontal: 28, justifyContent: 'center' }}>
        <SearchField value={query} onChange={setQuery} onKeyDown={onKey} autoFocus placeholder={tr('shell.classic.searchHint')}
          onCancel={() => { if (!query) onClose(); }} style={{ height: 34 }} />
      </View>
      <View style={{ flex: 1, paddingHorizontal: 22 }}>{body}</View>
      <View style={{ height: 64, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 28, gap: 10, backgroundColor: t.panelFoot,
        borderTopWidth: 1, borderColor: t.separator }}>
        <View onClick={() => { onClose(); wm.open('settings', { args: { section: 'users' } }); }} style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 8, height: 40, borderRadius: 6 }}>
          <View style={{ width: 32, height: 32, borderRadius: 16, alignItems: 'center', justifyContent: 'center', backgroundColor: t.accent }}>
            <Text style={{ fontSize: 13, fontWeight: 600, color: '#fff' }}>{initials}</Text>
          </View>
          <Text style={{ fontSize: 13, color: t.text, whiteSpace: 'nowrap' }}>{name}</Text>
        </View>
        <View style={{ flex: 1 }} />
        <View onClick={(e) => setPower({ x: e.x - 200, y: e.y - 12 })} style={{ width: 40, height: 40, borderRadius: 6, alignItems: 'center', justifyContent: 'center',
          backgroundColor: power ? t.hover : 'transparent' }}>
          <PowerGlyph color={t.text} bg={power ? hoverBg : footBg} size={16} />
        </View>
      </View>
      {ctx && <Menu t={t} boxRef={menuRef} x={ctx.x} y={ctx.y} onClose={() => setCtx(null)} items={ctxItems} />}
      {power && <Menu t={t} boxRef={menuRef} x={power.x} y={power.y} above width={220} onClose={() => setPower(null)} items={powerItems.map((it) => (it === '-' ? it : { ...it, onClick: () => { onClose(); it.onClick(); } }))} />}
    </View>
  );
}

// Where the start menu goes for a taskbar position and alignment (global coordinates).
export function startBox(pos, align, m = mainDisplay()) {
  const width = Math.min(640, m.w - 24);
  const height = Math.min(700, m.h - (pos === 'top' || pos === 'bottom' ? BAR_H : 0) - 24);
  const vert = pos === 'left' || pos === 'right';
  const left = vert ? (pos === 'left' ? m.x + BAR_W + 12 : m.x + m.w - BAR_W - 12 - width)
    : align === 'start' ? (direction() === 'rtl' ? m.x + m.w - 12 - width : m.x + 12) : m.x + Math.round((m.w - width) / 2);
  const top = pos === 'top' ? m.y + BAR_H + 12 : pos === 'bottom' ? m.y + m.h - BAR_H - 12 - height : m.y + 12;
  return { left, top, width, height };
}
