// The top band of the main display: the activity strip at the start (a pill per activity, its icon in its colour, the
// one in view filled with the accent; "+ New activity"; where you are: "Activity 2 of 4 · up to 9") and the status
// cluster at the end (privacy indicators, the status items such as the network, the volume, notifications and the
// clock). Each is one small surface of the theme's material, blurred like the Context Spine (the engine keeps a
// blurred backdrop while nothing behind it changes). Their controls look like a video player's: a pill per control
// that shows under the pointer, glyphs in the spine's colour, thin lines between groups (components/BarButton.jsx).
// An activity's icon keeps its own colour (Edit activity).
// A pill: click switches; double click renames in place (Enter keeps, Esc drops); right click: rename, icon and colour,
// delete (asked first, in the page). The clock re-renders only its own text, once a minute (sharedTicker).
import {
  View, Text, TextInput, useStore, desktopList, desktopNames, desktopName, activeDesktop, renameDesktop, switchDesktop, formatTime, formatDate,
  StatusItems, PrivacyIndicators, clockPrefs, weekdayName, useSound, defaultSink, t as tr,
} from '@lordos/sdk';
import { memo, useEffect, useRef, useState } from 'react';
import { tween, EASE } from '../motion.js';
import { Surface } from './Surface.jsx';
import { BarButton, BarSeparator, BAR_ITEM } from './BarButton.jsx';
import { Menu } from '../../classic/components/Menu.jsx';
import { ActivityIcon, PlusGlyph, SpeakerGlyph, BellGlyph } from '../glyphs.jsx';
import { activities, renaming, deleting, newActivity, noteLaunch } from '../state.js';
import { MAX_ACTIVITIES } from '../model/activities.js';

const H = 44;
// One timer for the clock, woken at each minute's turn (each second's only with Settings → Date & Time → Show seconds):
// the time and the day re-render then, and nothing else does. Runs only while the clock is on screen. The format is
// the menu bar clock's (24-hour, the date or the weekday, seconds; sdk/hardware.jsx clockPrefs).
function clockTicker(read) {
  let value = null, timer = 0;
  const subs = new Set();
  const arm = () => {
    const n = new Date(), ms = n.getSeconds() * 1000 + n.getMilliseconds();
    timer = setTimeout(tick, clockPrefs().seconds ? 1000 - n.getMilliseconds() + 5 : 60000 - ms + 20);
  };
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
const dayOf = (d) => { const c = clockPrefs(); return c.date === 'none' ? '' : c.date === 'weekday' ? weekdayName(d.getDay()) : formatDate(d, 'short'); };
export const clock = clockTicker((d) => `${dayOf(d)}\u0000${formatTime(d, { seconds: !!clockPrefs().seconds })}`);

const Clock = memo(function Clock({ t }) {
  const [day, time] = useStore(clock).split('\u0000');
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
      {day ? <Text style={{ fontSize: 11.5, color: t.textDim, whiteSpace: 'nowrap' }}>{day}</Text> : null}
      <Text style={{ fontSize: 14, fontWeight: 600, color: t.text, whiteSpace: 'nowrap' }}>{time}</Text>
    </View>
  );
});

const Pill = memo(function Pill({ t, id, active, glide, name, meta, onMenu, pillRef }) {
  const [hover, setHover] = useState(false);
  const editing = useStore(renaming) === id;
  const [value, setValue] = useState(name);
  const fg = active ? t.onAccent : t.text;
  if (editing) {
    return (
      <View style={{ width: 160, height: BAR_ITEM, justifyContent: 'center' }}>
        <TextInput value={value} autoFocus selectAll onChange={setValue} size={12.5}
          onSubmit={() => { renameDesktop(id, value.trim()); renaming.set(null); }} onCancel={() => renaming.set(null)} style={{ height: 30 }} />
      </View>
    );
  }
  return (
    <View ref={pillRef} onClick={() => switchDesktop(id)} onDoubleClick={() => { setValue(name); renaming.set(id); }} onContextMenu={(e) => onMenu(id, e)}
      onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)} title={name}
      style={{ height: BAR_ITEM, flexDirection: 'row', alignItems: 'center', gap: 7, paddingHorizontal: 12, borderRadius: BAR_ITEM / 2, maxWidth: 190,
        backgroundColor: active ? (glide ? 'transparent' : t.accent) : hover ? t.itemHover : 'transparent' }}>
      <ActivityIcon icon={meta.icon} color={active ? t.onAccent : meta.color} bg={active ? t.accent : t.solidBg} size={13} />
      <Text style={{ fontSize: 12.5, fontWeight: 600, color: fg, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{name}</Text>
    </View>
  );
});

// after the next frame's layout (a timer where there are no frames: the tests)
const nextFrame = (fn) => (typeof requestAnimationFrame === 'function' ? requestAnimationFrame(fn) : setTimeout(fn, 16));
const cancelFrame = (id) => (typeof cancelAnimationFrame === 'function' ? cancelAnimationFrame(id) : clearTimeout(id));

// one stable ref callback per pill
const refFns = new Map();
function pillRefFor(map, id) {
  if (!refFns.has(id)) refFns.set(id, (n) => { if (n) map.set(id, n); else map.delete(id); });
  return refFns.get(id);
}

export const ActivityStrip = memo(function ActivityStrip({ t, rect, onEdit }) {
  const list = useStore(desktopList);
  const active = useStore(activeDesktop);
  const meta = useStore(activities);
  const names = useStore(desktopNames);
  const [menu, setMenu] = useState(null);
  // the accent under the pill in view: it glides to the next one on a switch (motion.js, left and width only, inside
  // this small surface), the pill's own fill until it has been measured once
  const strip = useRef(null);
  const glider = useRef(null);
  const pills = useRef(new Map());
  const [pos, setPos] = useState(null);
  const last = useRef(null);
  useEffect(() => {
    const raf = nextFrame(() => {
      const p = pills.current.get(active), sn = strip.current, g = glider.current;
      if (!p || !sn) return;
      let box, sbox;
      try { box = lord.getLayout(p.id); sbox = lord.getLayout(sn.id); } catch (e) { return; }
      if (!box || !sbox || box[2] <= 0) return;
      const next = { x: box[0] - sbox[0], w: box[2] };
      const was = last.current;
      last.current = next;
      setPos(next);
      // drawn at its new place at once, then moved there from the old one: a translate and a horizontal scale from the
      // left edge (a cached raster, the cheap path), never left/width (those lay the strip out again on every frame)
      if (was && g && (was.x !== next.x || was.w !== next.w)) {
        // after the windows' slide (sdk/desktops.js, 170 ms): in the same frames the two changes would be repainted as one
        // box spanning both (the engine's single damage box), several times the pixels of each alone
        tween(g.id, { translateX: [was.x - next.x, 0], scaleX: [was.w / next.w, 1] }, { ms: 180, delay: 150, ease: EASE.back });
      }
    });
    return () => cancelFrame(raf);
  }, [active, list.length, names]);
  const at = list.indexOf(active) + 1;
  const full = list.length >= MAX_ACTIVITIES;
  return (
    <Surface ref={strip} t={t} radius={H / 2} style={{ position: 'absolute', left: rect.x, top: rect.y, height: H, maxWidth: rect.w, flexDirection: 'row', alignItems: 'center', gap: 2, paddingHorizontal: 4 }}>
      {pos ? <View ref={glider} style={{ position: 'absolute', left: pos.x, top: (H - BAR_ITEM) / 2 - 1, width: pos.w, height: BAR_ITEM, borderRadius: BAR_ITEM / 2, backgroundColor: t.accent, pointerEvents: 'none', transformOrigin: 'left center' }} /> : null}
      {list.map((id) => <Pill key={id} t={t} id={id} active={id === active} glide={!!pos} name={desktopName(id)} meta={meta[id] || { icon: 'spark', color: t.accent }}
        pillRef={pillRefFor(pills.current, id)} onMenu={(d, e) => setMenu({ id: d, x: e.x, y: e.y })} />)}
      <BarSeparator t={t} />
      <BarButton t={t} disabled={full} title={tr('shell.ws.activity.new')} onClick={(e) => { noteLaunch(e.x, e.y); newActivity(); }}
        glyph={(c) => <PlusGlyph color={c} size={11} />}>
        {(c) => <Text style={{ fontSize: 12.5, fontWeight: 500, color: c, whiteSpace: 'nowrap' }}>{tr('shell.ws.activity.newShort')}</Text>}
      </BarButton>
      <BarSeparator t={t} />
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
    <BarButton t={t} onClick={onClick} title={tr('shell.cc.sound')}
      glyph={(c) => <SpeakerGlyph color={c} size={15} level={pct === 0 ? 0 : pct < 50 ? 1 : 2} muted={out.mute} />} />
  );
}

// rect: the top band; screenW: the whole screen's width (the cluster is placed by its end edge, its width is its content)
export const StatusCluster = memo(function StatusCluster({ t, rect, rtl, screenW, unread, onControl, onNotif, notifRef }) {
  return (
    <Surface t={t} radius={H / 2} style={{ position: 'absolute', top: rect.y, height: H, ...(rtl ? { left: rect.x } : { right: screenW - (rect.x + rect.w) }),
      flexDirection: 'row', alignItems: 'center', gap: 2, paddingHorizontal: 4 }}>
      <PrivacyIndicators />
      <View style={{ paddingHorizontal: 4 }}><StatusItems color={t.iconFg} height={BAR_ITEM} /></View>
      <BarSeparator t={t} />
      <Volume t={t} onClick={onControl} />
      <BarButton ref={notifRef} t={t} onClick={onNotif} title={tr('shell.ws.notif.title')}
        glyph={(c) => <BellGlyph color={c} size={15} dot={unread ? t.accent : null} />} />
      <BarSeparator t={t} />
      <BarButton t={t} onClick={onNotif}><Clock t={t} /></BarButton>
    </Surface>
  );
});

// room the activity strip leaves for the status cluster
export const STATUS_W = 330;
