// App launcher ("Apps"): all apps in full screen, with keyboard search.
// An app is pinned to the Dock by dragging it onto the Dock (the launcher steps aside as soon as the
// drag starts, and the app lands where it is dropped) or with a right click → Keep in Dock. It works
// the same for LordOS apps, .lpk packages and Linux apps (sdk/dock.js).
import { View, Text, TextInput, ScrollView, screen, t as tr, appName, refreshApps, APP_CATEGORIES, appCategory, dock, dockIdOf, useStore, settings,
  beginDockDrag, dockDragJustEnded } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { Menu } from './Menu.jsx';

const COLS = 6, CELL = 130, ICON = 72;

// area: the display it covers (the main one; global coordinates)
export function Launchpad({ t, apps, onOpen, onClose, area = { x: 0, y: 0, w: screen.width, h: screen.height } }) {
  useEffect(refreshApps, []);
  const A = useRef(area);
  A.current = area;
  const [query, setQuery] = useState('');
  const [cat, setCat] = useState(null);                      // null: all
  const q = query.trim().toLowerCase();
  const match = (a) => [appName(a), a.name, a.id, a.genericName, a.keywords, a.comment].some((v) => v && String(v).toLowerCase().includes(q));
  const present = APP_CATEGORIES.filter((c) => apps.some((a) => appCategory(a) === c));
  const list = apps.filter((a) => (!q || match(a)) && (q || !cat || appCategory(a) === cat));
  const rows = [];
  for (let i = 0; i < list.length; i += COLS) rows.push(list.slice(i, i + COLS));
  const open = (id) => { onClose(); onOpen(id); };
  useStore(settings);                                         // «Keep in Dock» changes immediately
  const [ctx, setCtx] = useState(null);                       // { app, x, y }

  return (
    <View onClick={onClose} style={{
      position: 'absolute', left: area.x, top: area.y, width: area.w, height: area.h, alignItems: 'center',
      backgroundColor: t.effects ? 'rgba(10,10,16,0.35)' : 'rgba(18,18,24,0.94)', backdropFilter: `blur(${t.effects ? 32 : 0}px)`,
    }}>
      <View onClick={(e) => e.stopPropagation()} style={{ marginTop: 48, width: 260 }}>
        <TextInput value={query} onChange={setQuery} placeholder={tr('shell.launchpad.search')} autoFocus
          onSubmit={() => list[0] && open(list[0].id)} onCancel={onClose}
          style={{ backgroundColor: 'rgba(255,255,255,0.14)', border: '1px solid rgba(255,255,255,0.22)' }} />
      </View>
      {!q && (
        <View onClick={(e) => e.stopPropagation()} style={{ marginTop: 22, flexDirection: 'row', gap: 6 }}>
          {[null, ...present].map((c) => (
            <View key={c || 'all'} onClick={() => setCat(c)} style={{ paddingHorizontal: 12, paddingVertical: 5, borderRadius: 14,
              backgroundColor: c === cat ? 'rgba(255,255,255,0.32)' : 'rgba(255,255,255,0.1)' }}>
              <Text style={{ fontSize: 12, fontWeight: 600, color: '#fff' }}>{c ? tr(`shell.category.${c}`) : tr('shell.category.all')}</Text>
            </View>
          ))}
        </View>
      )}
      <ScrollView onClick={(e) => e.stopPropagation()} style={{ marginTop: q ? 40 : 14, height: area.h - 190, width: Math.min(COLS * CELL, area.w - 20) }}>
      <View style={{ gap: 26, paddingVertical: 16 }}>
        {rows.map((row, r) => (
          <View key={r} style={{ flexDirection: 'row' }}>
            {row.map((a) => (
              <View key={a.id} onClick={(e) => { e.stopPropagation(); if (!dockDragJustEnded()) open(a.id); }}
                onMouseDown={(e) => { if (!e.button && dockIdOf(a)) beginDockDrag({ id: dockIdOf(a), Icon: a.Icon, x: e.x, y: e.y, source: 'launcher', onStart: onClose }); }}
                onContextMenu={(e) => { e.stopPropagation(); setCtx({ app: a, x: e.x, y: e.y }); }}
                style={{ width: CELL, alignItems: 'center', gap: 10 }}>
                <a.Icon size={ICON} />
                <Text style={{ fontSize: 13, fontWeight: 500, color: '#fff', whiteSpace: 'nowrap', maxWidth: CELL - 10, overflow: 'hidden' }}>{appName(a)}</Text>
              </View>
            ))}
          </View>
        ))}
        {list.length === 0 && <Text style={{ fontSize: 14, color: 'rgba(255,255,255,0.7)', textAlign: 'center' }}>{tr('shell.launchpad.empty')}</Text>}
      </View>
      </ScrollView>
      {ctx && (
        <Menu x={ctx.x} y={ctx.y} onClose={() => setCtx(null)} items={[
          { label: tr('shell.dock.open'), onClick: () => open(ctx.app.id) },
          ...(dockIdOf(ctx.app) ? [{ label: tr('shell.dock.keep'), checked: dock.isPinned(ctx.app), disabled: dock.isPinned(ctx.app) && !dock.canRemove(ctx.app),
            onClick: () => (dock.isPinned(ctx.app) ? dock.unpin(ctx.app) : dock.pin(ctx.app)) }] : []),
        ]} />
      )}
    </View>
  );
}
