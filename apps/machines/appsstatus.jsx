// Apps mode's status item (docs/VIRTUALIZATION.md 7.2): while a machine is in apps mode, an item by the clock with the
// machine's mark (like a Unity mode's menu in the menu bar). Its menu works like the guest's Start menu: a search, the
// guest's windows (Running) and apps to open (appslist.jsx, the same lists as the controller's panel), then the machine's
// name, Show the Machine's Window and Leave Apps Mode. One item per machine, through the SDK's status items, so every
// theme that draws <StatusItems /> shows it on each of its bars (Lord Flow's menu bar, the Classic taskbar, Workspaces'
// top bar); it goes when apps mode ends (the machine leaves `guests`).
import { View, Text, ScrollView, Overlay, addListener, useStore, wm, registerStatusItem, statusPanelAt, statusPanelRoom, t as tr } from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { guests } from './appsmode.jsx';
import { ListHead, AppsSearch, RunningList, LaunchList, launchFirst } from './appslist.jsx';
import { usePal, OsMark, menus } from './ui.jsx';
import { readMeta } from './meta.js';
import { launcherApps, launchAnswered, appsStatusDiff, machineOsId } from './model.js';

const ORDER = 7.5;             // after the Linux apps' tray icons (7), before the isolated apps' items (8)
const items = new Map();       // dir → unregister

// a row at the menu's foot
function FootRow({ label, onClick }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 28, justifyContent: 'center', paddingHorizontal: 8, borderRadius: 7, backgroundColor: hover ? p.hover : 'transparent' }}>
      <Text style={{ fontSize: 13, color: p.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{label}</Text>
    </View>
  );
}

function Menu({ g, os, room, close }) {
  const p = usePal();
  const [query, setQuery] = useState('');
  const apps = launcherApps(g.apps || [], query, g.appIcons);
  // an app opened from here: the menu stays, its row saying "Opening…", until the app's window is listed (then it's in
  // front and the menu goes, so the keys reach it)
  const [opened, setOpened] = useState(null);
  const launched = (a) => setOpened(a.token);
  useEffect(() => { if (opened && launchAnswered(g.launches?.[opened], g.list)) close(); }, [opened, g.list]);
  const edge = p.dark ? 'rgba(255,255,255,0.14)' : 'rgba(0,0,0,0.12)';
  // the head (search) and the foot always show; the lists scroll between them
  const lists = room ? Math.max(120, room - 150) : 420;
  return (
    <View style={{ width: 320, padding: 8, gap: 6, borderRadius: 12, backgroundColor: p.card, border: `1px solid ${edge}`, boxShadow: '0 12px 32px rgba(0,0,0,0.4)' }}>
      <AppsSearch value={query} onChange={setQuery} autoFocus onSubmit={() => launchFirst(g, apps, launched)} onCancel={close} />
      <ScrollView style={{ maxHeight: lists }} contentStyle={{ gap: 4 }}>
        {/* while searching, only the apps (as a Start menu does) */}
        {query.trim() ? null : <ListHead label={tr('machines.apps.running')} />}
        {query.trim() ? null : <RunningList g={g} after={close} />}
        <ListHead label={tr('machines.apps.launch')} />
        <LaunchList g={g} apps={apps} onLaunched={launched} />
      </ScrollView>
      <View style={{ height: 1, backgroundColor: p.line }} />
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 8, paddingVertical: 2 }}>
        <OsMark id={os} size={18} />
        <Text style={{ flex: 1, minWidth: 0, fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{g.name}</Text>
      </View>
      <FootRow label={tr('machines.apps.showWindow')} onClick={() => { close(); if (g.home) wm.focus(g.home); }} />
      <FootRow label={tr('machines.apps.leave')} onClick={() => { close(); if (g.leave) g.leave(); }} />
    </View>
  );
}

// the item itself: one copy per bar; each copy opens its own menu, toward the middle of its display
function MachineItem({ dir, height }) {
  const g = useStore(guests, (x) => x[dir] || null);
  const [open, setOpen] = useState(false);
  const btn = useRef(null), panel = useRef(null);
  const os = useMemo(() => machineOsId(readMeta(dir), g?.name), [dir, g?.name]);
  useEffect(() => {
    if (!open) return undefined;
    // the guest window in front doesn't take the keys while the menu's search does (appsmode.jsx GuestWindow)
    menus.open++;
    const inside = (n, e) => { if (!n) return false; const [x, y, w, hh] = lord.getLayout(n.id); return e.x >= x && e.x <= x + w && e.y >= y && e.y <= y + hh; };
    const a = addListener('pointerdown', (e) => { if (!inside(panel.current, e) && !inside(btn.current, e)) setOpen(false); });
    const b = addListener('keydown', (e) => { if (e.key === 'Escape') setOpen(false); });
    return () => { menus.open--; a(); b(); };
  }, [open]);
  useEffect(() => { if (!g) setOpen(false); }, [!g]);
  if (!g) return null;
  return (
    <View ref={btn} title={tr('machines.apps.statusTip', { name: g.name || '' })} onClick={() => setOpen((o) => !o)}
      style={{ height, width: 26, alignItems: 'center', justifyContent: 'center' }}>
      <OsMark id={os} size={16} />
      {open ? (
        <Overlay anchor={btn}>
          <View ref={panel} style={statusPanelAt(btn)}>
            <Menu g={g} os={os} room={statusPanelRoom(btn)} close={() => setOpen(false)} />
          </View>
        </Overlay>
      ) : null}
    </View>
  );
}

function sync() {
  const { add, remove } = appsStatusDiff([...items.keys()], guests.get());
  for (const dir of remove) { items.get(dir)(); items.delete(dir); }
  for (const dir of add) items.set(dir, registerStatusItem(`machines:${dir}`, (p) => <MachineItem {...p} dir={dir} />, ORDER));
}

// once, when the app's code loads (index.jsx): the items follow `guests` from then on
let started = false;
export function startAppsStatus() {
  if (started) return;
  started = true;
  guests.subscribe(sync);
  sync();
}
