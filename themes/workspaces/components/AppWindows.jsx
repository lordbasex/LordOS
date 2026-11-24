// The windows of an activity on one display, in its frame header (the display's bar), and an app's windows from its
// icon there.
//
// WindowsTray: one icon per window of the activity on this display, open and minimized alike, in the order they
// opened (the store's order changes with every focus, so the icons would jump about). A minimized one has a short line
// under its icon. A click brings an open window forward and a minimized one back into its slot of the layout (the
// tiler gives a window brought back the slot it kept, tiler.js). No label: the icons say it.
//
// An app's icon (a window's in the tray, or a pinned app's): resting the pointer on it lists that app's windows in this
// activity when it has more than one (two Google Chrome profiles open on the same screen); right click shows the same
// list as a menu, with "New window" for an app the launcher knows (opened on this display, tiler.js aimLaunch). The
// rows are the Dock's own (sdk/dock.js appWindowItems): each window's title as the app set it, cleaned to one plain
// line (sdk/windowlistmodel.js: no control or bidi-override characters, cut to a length), the window in front checked,
// "–" for a minimized one. The title is the app's: Chrome puts the profile's name there in some setups, and a window
// that names itself after another app still only brings forward itself.
import { View, Text, Layer, createStore, useStore, windows, wm, windowTitle, windowIcon, launcherIdOf, launchApp, appWindowItems, cleanWindowLabel, activeDesktop, t as tr } from '@lordos/sdk';
import { memo, useRef, useState } from 'react';
import { Menu } from '../../classic/components/Menu.jsx';
import { aimLaunch } from '../tiler.js';
import { groupHidden } from '../state.js';
import { Surface } from './Surface.jsx';

// the windows an app has in an activity (its own windows: no dialog, popup, notice or window kept out of the taskbar)
export const listed = (w, desk, hid) => w.desktop === desk && !w.pip && !w.args?.notice && !w.args?.parent && !w.args?.dialog && !w.args?.skipTaskbar && !hid.has(w.id);
export function appWindowsIn(all, desk, key, hid = new Set()) {
  return all.filter((w) => listed(w, desk, hid) && launcherIdOf(w) === key);
}

// the order windows first showed in a tray (window id → n), never the stacking order
const firstSeen = new Map();
let seenN = 0;
export function trayOrder(id) {
  if (!firstSeen.has(id)) {
    if (firstSeen.size > 512) firstSeen.clear();
    firstSeen.set(id, seenN++);
  }
  return firstSeen.get(id);
}

// the hover list: after a short rest on the icon, under it; it stays while the pointer is on it
const hover = { timer: 0 };
const HOVER_MS = 450;
export const appPeek = createStore(null);
function peekSoon(node, key, desk, d) {
  clearTimeout(hover.timer);
  hover.timer = setTimeout(() => {
    let box = null;
    try { box = lord.getLayout(node.id); } catch (e) { return; }
    if (box && box[2] > 0) appPeek.set({ key, desk, d, x: box[0], y: box[1] + box[3] + 6 });
  }, appPeek.get() ? 0 : HOVER_MS);
}
function peekEnd(ms = 180) { clearTimeout(hover.timer); hover.timer = setTimeout(() => appPeek.set(null), ms); }

// the menu of an app's icon: its windows here, then New window (an app the launcher knows). after: run once a row is
// chosen (the launcher closes).
export function appMenuItems(key, desk, d, after = null, launchable = true) {
  const wins = appWindowsIn(windows.get(), desk, key, groupHidden.get());
  const done = (fn) => () => { fn(); if (after) after(); };
  const items = wins.length ? appWindowItems(wins, '').map((it) => ({ ...it, onClick: done(it.onClick) })) : [];
  if (launchable) {
    if (items.length) items.push('-');
    items.push({ label: tr('shell.dock.newWindow'), onClick: done(() => { aimLaunch(d, 1, key); launchApp(key); }) });
  }
  return items;
}

// An app's icon in the frame header: a window's (the tray) or a pinned app's. key: its launcher id.
export const AppIcon = memo(function AppIcon({ t, d, desk, appKey, Icon, title, minimized, onClick, launchable }) {
  const ref = useRef(null);
  const [menu, setMenu] = useState(null);
  const [over, setOver] = useState(false);
  return (
    <View ref={ref} title={title} onClick={(e) => { appPeek.set(null); onClick(e); }}
      onContextMenu={(e) => { appPeek.set(null); const items = appMenuItems(appKey, desk, d, null, launchable); if (items.length) setMenu({ x: e.x, y: e.y, items }); }}
      onMouseEnter={() => { setOver(true); if (ref.current && appWindowsIn(windows.get(), desk, appKey, groupHidden.get()).length > 1) peekSoon(ref.current, appKey, desk, d); }}
      onMouseLeave={() => { setOver(false); peekEnd(); }}
      style={{ width: 28, height: 28, borderRadius: 7, alignItems: 'center', justifyContent: 'center', backgroundColor: over ? t.hover : 'transparent' }}
    >
      {Icon ? <Icon size={18} /> : <View style={{ width: 14, height: 14, borderRadius: 4, backgroundColor: t.textDim }} />}
      {/* a minimized window: a short line under its icon */}
      <View style={{ position: 'absolute', bottom: 1, width: 12, height: 2.5, borderRadius: 1.5, backgroundColor: t.frameText, opacity: 0.85, display: minimized ? 'flex' : 'none', pointerEvents: 'none' }} />
      {menu && <Menu t={t} x={menu.x} y={menu.y} width={280} onClose={() => setMenu(null)} items={menu.items} />}
    </View>
  );
});

// The windows of this activity on this display. ids: [{ id, min }] in the order they first showed.
export const trayNodes = new Map();      // display id → the tray's node, for the minimize effect (index.jsx)
export function WindowsTray({ t, d, desk, ids }) {
  const ref = useRef(null);
  const set = (n) => { ref.current = n; if (n) trayNodes.set(d.id, n.id); else if (trayNodes.get(d.id) != null && !n) trayNodes.delete(d.id); };
  const list = ids.map(({ id, min }) => ({ w: wm.get(id), min })).filter((x) => x.w);
  const shown = list.slice(0, 10);
  return (
    <View ref={set} title={tr('shell.ws.windows.title')} style={{ height: 32, flexDirection: 'row', alignItems: 'center', gap: 1, paddingHorizontal: 3, borderRadius: 12,
      backgroundColor: t.chipBg, display: list.length ? 'flex' : 'none' }}>
      {shown.map(({ w, min }) => (
        <AppIcon key={w.id} t={t} d={d} desk={desk} appKey={launcherIdOf(w)} Icon={windowIcon(w)} minimized={min}
          title={min ? `${cleanWindowLabel(windowTitle(w))} · ${tr('shell.ws.peek.minimized')}` : cleanWindowLabel(windowTitle(w))}
          onClick={() => wm.focus(w.id)} launchable={false} />
      ))}
      {list.length > shown.length ? <Text style={{ fontSize: 11, color: t.textDim, paddingHorizontal: 4 }}>{`+${list.length - shown.length}`}</Text> : null}
    </View>
  );
}

// The hover list of an app's windows (in the popover layer), drawn once by the theme.
function PeekRow({ t, it }) {
  const [h, setH] = useState(false);
  const w = wm.get(it.id);
  const Icon = w ? windowIcon(w) : null;
  return (
    <View onClick={() => { appPeek.set(null); it.onClick(); }} onMouseEnter={() => setH(true)} onMouseLeave={() => setH(false)}
      style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 8, height: 30, borderRadius: 8, backgroundColor: h ? t.itemHover : 'transparent' }}>
      {Icon ? <Icon size={16} /> : <View style={{ width: 16, height: 16 }} />}
      {it.dot ? <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: it.dot }} /> : null}
      <Text style={{ flex: 1, fontSize: 12, fontWeight: it.checked ? 700 : 400, color: t.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{it.label}</Text>
      {it.minimized ? <Text style={{ fontSize: 10.5, color: t.textDim }}>{tr('shell.ws.peek.minimized')}</Text> : null}
    </View>
  );
}
export const AppWindowsPeek = memo(function AppWindowsPeek({ t }) {
  const p = useStore(appPeek);
  useStore(windows);
  if (!p || p.desk !== activeDesktop.get()) return null;
  const wins = appWindowsIn(windows.get(), p.desk, p.key, groupHidden.get());
  if (wins.length < 2) return null;
  const items = appWindowItems(wins, '');
  const left = Math.max(p.d.x + 8, Math.min(p.x, p.d.x + p.d.w - 308));
  return (
    <Layer name="popover">
      <Surface t={t} radius={14} onMouseEnter={() => clearTimeout(hover.timer)} onMouseLeave={() => peekEnd()}
        style={{ position: 'absolute', left, top: p.y, width: 300, padding: 6, gap: 1 }}>
        {items.map((it) => <PeekRow key={it.id} t={t} it={it} />)}
      </Surface>
    </Layer>
  );
});

