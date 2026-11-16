// The top band of the main display: the activity strip at the start (a pill per activity, its icon in its colour, the
// one in view filled with the accent; "+ New activity"; where you are: "Activity 2 of 4 · up to 9") and the status
// cluster at the end (privacy indicators, the status items such as the network, the volume, notifications and the
// clock). Each is one small surface, so at most two blurred boxes here.
// A pill: click switches; double click renames in place (Enter keeps, Esc drops); right click: rename, icon and colour,
// delete (asked first, in the page). The clock re-renders only its own text, once a minute (sharedTicker).
import {
  View, Text, TextInput, useStore, desktopList, desktopNames, desktopName, activeDesktop, renameDesktop, switchDesktop, formatTime, formatDate,
  StatusItems, PrivacyIndicators, useSound, defaultSink, t as tr,
} from '@lordos/sdk';
import { memo, useState } from 'react';
import { Surface } from './Surface.jsx';
import { Menu } from '../../classic/components/Menu.jsx';
import { ActivityIcon, PlusGlyph, SpeakerGlyph, BellGlyph } from '../glyphs.jsx';
import { activities, renaming, deleting, newActivity, noteLaunch } from '../state.js';
import { MAX_ACTIVITIES } from '../model/activities.js';

const H = 44;
// One timer for the clock, woken at each minute's turn (not every second): the time and the day re-render then, and
// nothing else does. Runs only while the clock is on screen.
function minuteTicker(read) {
  let value = null, timer = 0;
  const subs = new Set();
  const arm = () => { const n = new Date(); timer = setTimeout(tick, 60000 - (n.getSeconds() * 1000 + n.getMilliseconds()) + 20); };
  const tick = () => { const v = read(new Date()); if (v !== value) { value = v; for (const fn of [...subs]) fn(); } arm(); };
  return {
    get: () => (value == null ? (value = read(new Date())) : value),
    subscribe(fn) {
      if (!subs.size) { value = read(new Date()); arm(); }
      subs.add(fn);
      return () => { if (subs.delete(fn) && !subs.size) { clearTimeout(timer); timer = 0; } };
    },
  };
}
export const clock = minuteTicker((d) => `${formatDate(d, 'short')}\u0000${formatTime(d)}`);

const Clock = memo(function Clock({ t }) {
  const [day, time] = useStore(clock).split('\u0000');
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
      <Text style={{ fontSize: 11.5, color: t.textDim, whiteSpace: 'nowrap' }}>{day}</Text>
      <Text style={{ fontSize: 14, fontWeight: 600, color: t.text, whiteSpace: 'nowrap' }}>{time}</Text>
    </View>
  );
});

const Pill = memo(function Pill({ t, id, active, name, meta, onMenu }) {
  const [hover, setHover] = useState(false);
  const editing = useStore(renaming) === id;
  const [value, setValue] = useState(name);
  const fg = active ? t.onAccent : t.text;
  if (editing) {
    return (
      <View style={{ width: 160, height: 32, justifyContent: 'center' }}>
        <TextInput value={value} autoFocus selectAll onChange={setValue} size={12.5}
          onSubmit={() => { renameDesktop(id, value.trim()); renaming.set(null); }} onCancel={() => renaming.set(null)} style={{ height: 30 }} />
      </View>
    );
  }
  return (
    <View onClick={() => switchDesktop(id)} onDoubleClick={() => { setValue(name); renaming.set(id); }} onContextMenu={(e) => onMenu(id, e)}
      onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)} title={name}
      style={{ height: 32, flexDirection: 'row', alignItems: 'center', gap: 7, paddingHorizontal: 12, borderRadius: 16, maxWidth: 190,
        backgroundColor: active ? t.accent : hover ? t.hover : 'transparent' }}>
      <ActivityIcon icon={meta.icon} color={active ? t.onAccent : meta.color} bg={active ? t.accent : t.solidBg} size={13} />
      <Text style={{ fontSize: 12.5, fontWeight: 600, color: fg, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{name}</Text>
    </View>
  );
});

export const ActivityStrip = memo(function ActivityStrip({ t, rect, onEdit }) {
  const list = useStore(desktopList);
  const active = useStore(activeDesktop);
  useStore(desktopNames);
  const meta = useStore(activities);
  const [menu, setMenu] = useState(null);
  const at = list.indexOf(active) + 1;
  const full = list.length >= MAX_ACTIVITIES;
  return (
    <Surface t={t} radius={H / 2} style={{ position: 'absolute', left: rect.x, top: rect.y, height: H, maxWidth: rect.w, flexDirection: 'row', alignItems: 'center', gap: 2, paddingHorizontal: 6 }}>
      {list.map((id) => <Pill key={id} t={t} id={id} active={id === active} name={desktopName(id)} meta={meta[id] || { icon: 'spark', color: t.accent }}
        onMenu={(d, e) => setMenu({ id: d, x: e.x, y: e.y })} />)}
      <View onClick={(e) => { if (!full) { noteLaunch(e.x, e.y); newActivity(); } }} title={tr('shell.ws.activity.new')}
        style={{ height: 32, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 12, borderRadius: 16, opacity: full ? 0.45 : 1 }}>
        <PlusGlyph color={t.text} size={11} />
        <Text style={{ fontSize: 12.5, fontWeight: 500, color: t.text, whiteSpace: 'nowrap' }}>{tr('shell.ws.activity.newShort')}</Text>
      </View>
      <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap', paddingHorizontal: 8 }}>{tr('shell.ws.activity.position', { n: at, total: list.length, max: MAX_ACTIVITIES })}</Text>
      {menu && <Menu t={t} x={menu.x} y={menu.y} onClose={() => setMenu(null)} items={[
        { label: tr('shell.ws.activity.rename'), onClick: () => renaming.set(menu.id) },
        { label: tr('shell.ws.activity.edit'), onClick: () => onEdit(menu.id, menu) },
        '-',
        { label: tr('shell.ws.activity.delete'), disabled: list.length < 2, destructive: true, onClick: () => deleting.set(menu.id) },
      ]} />}
    </Surface>
  );
});

function Volume({ t, onClick }) {
  const st = useSound();
  const out = defaultSink(st);
  if (!st.connected || !out) return null;
  const pct = Math.round(out.level * 100);
  return (
    <View onClick={onClick} title={tr('shell.cc.sound')} style={{ width: 28, height: 28, borderRadius: 14, alignItems: 'center', justifyContent: 'center' }}>
      <SpeakerGlyph color={t.text} size={15} level={pct === 0 ? 0 : pct < 50 ? 1 : 2} muted={out.mute} />
    </View>
  );
}

// rect: the top band; screenW: the whole screen's width (the cluster is placed by its end edge, its width is its content)
export const StatusCluster = memo(function StatusCluster({ t, rect, rtl, screenW, unread, onControl, onNotif, notifRef }) {
  return (
    <Surface t={t} radius={H / 2} style={{ position: 'absolute', top: rect.y, height: H, ...(rtl ? { left: rect.x } : { right: screenW - (rect.x + rect.w) }),
      flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 12 }}>
      <PrivacyIndicators />
      <StatusItems color={t.text} height={28} />
      <Volume t={t} onClick={onControl} />
      <View ref={notifRef} onClick={onNotif} title={tr('shell.ws.notif.title')} style={{ width: 28, height: 28, borderRadius: 14, alignItems: 'center', justifyContent: 'center' }}>
        <BellGlyph color={t.text} size={15} dot={unread ? t.accent : null} />
      </View>
      <View onClick={onNotif} style={{ paddingStart: 4 }}><Clock t={t} /></View>
    </Surface>
  );
});

// room the activity strip leaves for the status cluster
export const STATUS_W = 330;
