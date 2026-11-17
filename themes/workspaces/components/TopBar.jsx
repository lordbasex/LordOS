// The top band of the main display: the activity strip at the start (a pill per activity, its icon in its colour, the
// one in view filled with the accent; "+ New activity"; where you are: "Activity 2 of 4 · up to 9") and the status
// cluster at the end (privacy indicators, the status items such as the network, the volume, notifications and the
// clock). Each is one small surface of the theme's material, blurred like the Context Spine (the engine keeps a
// blurred backdrop while nothing behind it changes). Their controls look like a video player's: a pill per control
// that shows under the pointer, glyphs in the spine's colour, thin lines between groups (components/BarButton.jsx).
// An activity's icon keeps its own colour (Edit activity).
// A pill: click switches; double click renames in place (Enter keeps, Esc drops); right click: rename, icon and colour,
// delete (asked first, in the page). The other activities are summed up (the Focus Surface): each pill counts its
// windows, and resting the pointer on one shows what it holds (its windows by app and title, or its pinned apps when
// it has none); a click on a window there goes to it. The clock re-renders only its own text, once a minute (sharedTicker).
import {
  View, Text, TextInput, useStore, desktopList, desktopNames, desktopName, activeDesktop, renameDesktop, switchDesktop, formatTime, formatDate,
  StatusItems, PrivacyIndicators, clockPrefs, weekdayName, useSound, defaultSink, t as tr, createStore, Layer, windows, windowTitle, windowIcon,
  getApp, appName, wm, useLauncherApps,
} from '@lordos/sdk';
import { activitySummary } from '../model/activities.js';
import { Fragment, memo, useEffect, useRef, useState } from 'react';
import { tween, EASE } from '../motion.js';
import { Surface } from './Surface.jsx';
import { BarButton, BarSeparator, BAR_ITEM, FocusRing } from './BarButton.jsx';
import { Menu } from '../../classic/components/Menu.jsx';
import { ActivityIcon, PlusGlyph, SpeakerGlyph, BellGlyph } from '../glyphs.jsx';
import { activities, renaming, deleting, newActivity, noteLaunch, showGroup, addGroup, renameGroup, removeGroup } from '../state.js';
import { MAX_ACTIVITIES, MAX_GROUPS } from '../model/activities.js';

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

// The Focus Surface's summary of another activity: { id, x, y } while the pointer rests on its pill (or on the card).
const peek = createStore(null);
let peekTimer = 0;
const PEEK_MS = 450;
function peekSoon(id, pill) {
  clearTimeout(peekTimer);
  peekTimer = setTimeout(() => {
    let box = null;
    try { box = lord.getLayout(pill.id); } catch (e) { return; }
    if (box && box[2] > 0) peek.set({ id, x: box[0], y: box[1] + box[3] + 8 });
  }, peek.get() ? 0 : PEEK_MS);
}
function peekEnd(ms = 160) { clearTimeout(peekTimer); peekTimer = setTimeout(() => peek.set(null), ms); }
const own = (w) => !(getApp(w.id)?.builtin && !String(w.id).startsWith('linux#'));
// how many windows each activity has, as one string (the strip re-renders when a count changes, not on a title)
const countsOf = (list) => { const c = {}; for (const w of list) if (own(w) && !w.pip) c[w.desktop] = (c[w.desktop] || 0) + 1; return JSON.stringify(c); };

function PeekRow({ t, w, onPick }) {
  const [hover, setHover] = useState(false);
  const Icon = windowIcon(w, getApp);
  return (
    <View onClick={() => onPick(w)} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 8, height: 30, borderRadius: 8, backgroundColor: hover ? t.itemHover : 'transparent' }}>
      {Icon ? <Icon size={18} /> : <View style={{ width: 18, height: 18 }} />}
      <Text style={{ flex: 1, fontSize: 12, color: t.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{windowTitle(w) || appName(getApp(w.id))}</Text>
      {w.minimized ? <Text style={{ fontSize: 10.5, color: t.textDim }}>{tr('shell.ws.peek.minimized')}</Text> : null}
    </View>
  );
}

function PinnedLine({ t, pins }) {
  const apps = useLauncherApps();
  const names = pins.map((k) => apps.find((a) => a.id === k)?.name).filter(Boolean);
  return (
    <Text style={{ fontSize: 11.5, color: t.textDim, paddingHorizontal: 8, paddingVertical: 4 }}>
      {names.length ? tr('shell.ws.peek.pinned', { apps: names.join(', ') }) : tr('shell.ws.peek.empty')}
    </Text>
  );
}

export const ActivityPeek = memo(function ActivityPeek({ t }) {
  const p = useStore(peek);
  const all = useStore(windows);
  const meta = useStore(activities);
  if (!p || p.id === activeDesktop.get()) return null;
  const m = meta[p.id] || { icon: 'spark', color: t.accent, pins: [] };
  const sum = activitySummary(all.filter((w) => own(w) && !w.pip), p.id, 6);
  const pick = (w) => { peek.set(null); switchDesktop(p.id); wm.focus(w.id); };
  return (
    <Layer name="popover">
      <View onMouseEnter={() => clearTimeout(peekTimer)} onMouseLeave={() => peekEnd()}
        style={{ position: 'absolute', left: p.x, top: p.y, width: 280, padding: 8, gap: 2, borderRadius: 14, backgroundColor: t.popupBg,
          border: `1px solid ${t.mat.border}`, boxShadow: t.shadowSoft }}>
        <View onClick={() => { peek.set(null); switchDesktop(p.id); }} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 8, paddingVertical: 6 }}>
          <View style={{ width: 22, height: 22, borderRadius: 7, backgroundColor: m.color, alignItems: 'center', justifyContent: 'center' }}>
            <ActivityIcon icon={m.icon} color="#ffffff" bg={m.color} size={12} />
          </View>
          <Text style={{ flex: 1, fontSize: 13, fontWeight: 700, color: t.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{desktopName(p.id)}</Text>
          <Text style={{ fontSize: 11, color: t.textDim }}>{tr('shell.ws.peek.windows', { n: sum.total })}</Text>
        </View>
        {sum.shown.map((w) => <PeekRow key={w.id} t={t} w={w} onPick={pick} />)}
        {sum.more ? <Text style={{ fontSize: 11, color: t.textDim, paddingHorizontal: 8, paddingVertical: 4 }}>{tr('shell.ws.peek.more', { n: sum.more })}</Text> : null}
        {!sum.total ? <PinnedLine t={t} pins={m.pins} /> : null}
      </View>
    </Layer>
  );
});

const Pill = memo(function Pill({ t, id, active, glide, name, meta, count, onMenu, pillRef }) {
  const [hover, setHover] = useState(false);
  const self = useRef(null);
  const ref = (n) => { self.current = n; pillRef(n); };
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
    <View ref={ref} onClick={() => { peek.set(null); switchDesktop(id); }} onDoubleClick={() => { setValue(name); renaming.set(id); }} onContextMenu={(e) => { peek.set(null); onMenu(id, e); }}
      onMouseEnter={() => { setHover(true); if (!active && self.current) peekSoon(id, self.current); }} onMouseLeave={() => { setHover(false); peekEnd(); }}
      title={active ? name : undefined}
      style={{ height: BAR_ITEM, flexDirection: 'row', alignItems: 'center', gap: 7, paddingHorizontal: 12, borderRadius: BAR_ITEM / 2, maxWidth: 190,
        backgroundColor: active ? (glide ? 'transparent' : t.accent) : hover ? t.itemHover : 'transparent' }}>
      <ActivityIcon icon={meta.icon} color={active ? t.onAccent : meta.color} bg={active ? t.accent : t.solidBg} size={13} />
      <Text style={{ fontSize: 12.5, fontWeight: 600, color: fg, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{name}</Text>
      {/* kept drawn and hidden on the pill in view: a switch changes its display, not its nodes */}
      <View style={{ display: !active && count ? 'flex' : 'none', minWidth: 16, height: 16, paddingHorizontal: 4, borderRadius: 8, backgroundColor: t.itemHover, alignItems: 'center', justifyContent: 'center' }}>
        <Text style={{ fontSize: 10, fontWeight: 700, color: t.textDim }}>{count || ''}</Text>
      </View>
      <FocusRing t={t} fid={`pill:${id}`} radius={BAR_ITEM / 2} />
    </View>
  );
});

// The groups of the activity in view (more activities on the same desktop, state.js): the activity's own first (its
// icon), then each group (icon and name); the one shown has the accent's outline. Click shows it; double click renames
// a group; right click: rename, remove (its windows join the activity's own). "+" adds one, up to MAX_GROUPS.
const groupRenaming = createStore(null);
const GroupChip = memo(function GroupChip({ t, desk, g, on, label, icon, color, onMenu }) {
  const [hover, setHover] = useState(false);
  const editing = useStore(groupRenaming) === `${desk}:${g}`;
  const [value, setValue] = useState(label);
  if (editing) {
    return (
      <View style={{ width: 130, height: BAR_ITEM, justifyContent: 'center' }}>
        <TextInput value={value} autoFocus selectAll onChange={setValue} size={12}
          onSubmit={() => { renameGroup(desk, g, value); groupRenaming.set(null); }} onCancel={() => groupRenaming.set(null)} style={{ height: 28 }} />
      </View>
    );
  }
  return (
    <View onClick={() => showGroup(desk, g)} onDoubleClick={() => { if (g) { setValue(label); groupRenaming.set(`${desk}:${g}`); } }}
      onContextMenu={(e) => { if (g) onMenu(g, e); }} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)} title={label}
      style={{ height: 28, flexDirection: 'row', alignItems: 'center', gap: 5, paddingHorizontal: g ? 8 : 6, borderRadius: 14, maxWidth: 130,
        border: `1.5px solid ${on ? t.accent : 'transparent'}`, backgroundColor: hover ? t.itemHover : 'transparent' }}>
      <View style={{ width: 16, height: 16, borderRadius: 5, backgroundColor: color, alignItems: 'center', justifyContent: 'center' }}>
        <ActivityIcon icon={icon} color="#ffffff" bg={color} size={9} />
      </View>
      {g ? <Text style={{ fontSize: 11.5, fontWeight: on ? 700 : 500, color: t.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{label}</Text> : null}
    </View>
  );
});

const Groups = memo(function Groups({ t, desk, meta, name }) {
  const [menu, setMenu] = useState(null);
  const groups = meta.groups || [];
  if (!groups.length) return null;
  const inView = meta.group ?? null;
  const labelOf = (g, i) => g.name || tr('shell.ws.group.name', { n: i + 1 });
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 1, paddingHorizontal: 2 }}>
      <GroupChip t={t} desk={desk} g={null} on={inView == null} label={name} icon={meta.icon} color={meta.color} />
      {groups.map((g, i) => <GroupChip key={g.id} t={t} desk={desk} g={g.id} on={inView === g.id} label={labelOf(g, i)} icon={g.icon} color={g.color}
        onMenu={(id, e) => setMenu({ id, x: e.x, y: e.y })} />)}
      {groups.length < MAX_GROUPS ? (
        <BarButton t={t} title={tr('shell.ws.group.add')} onClick={() => addGroup(desk)} style={{ height: 28, minWidth: 28 }}
          glyph={(c) => <PlusGlyph color={c} size={9} />} />
      ) : null}
      {menu && <Menu t={t} x={menu.x} y={menu.y} onClose={() => setMenu(null)} items={[
        { label: tr('shell.ws.activity.rename'), onClick: () => groupRenaming.set(`${desk}:${menu.id}`) },
        '-',
        { label: tr('shell.ws.group.remove'), destructive: true, onClick: () => removeGroup(desk, menu.id) },
      ]} />}
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
  const counts = JSON.parse(useStore(windows, countsOf));
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
      {list.map((id) => (
        <Fragment key={id}>
          <Pill t={t} id={id} active={id === active} glide={!!pos} name={desktopName(id)} meta={meta[id] || { icon: 'spark', color: t.accent }} count={counts[id] || 0}
            pillRef={pillRefFor(pills.current, id)} onMenu={(d, e) => setMenu({ id: d, x: e.x, y: e.y })} />
          {id === active && meta[id]?.groups?.length ? <Groups t={t} desk={id} meta={meta[id]} name={desktopName(id)} /> : null}
        </Fragment>
      ))}
      <BarSeparator t={t} />
      <BarButton t={t} fid="new" disabled={full} title={tr('shell.ws.activity.new')} onClick={(e) => { noteLaunch(e.x, e.y); newActivity(); }}
        glyph={(c) => <PlusGlyph color={c} size={11} />}>
        {(c) => <Text style={{ fontSize: 12.5, fontWeight: 500, color: c, whiteSpace: 'nowrap' }}>{tr('shell.ws.activity.newShort')}</Text>}
      </BarButton>
      <BarSeparator t={t} />
      <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap', paddingHorizontal: 8 }}>{tr('shell.ws.activity.position', { n: at, total: list.length, max: MAX_ACTIVITIES })}</Text>
      {menu && <Menu t={t} x={menu.x} y={menu.y} onClose={() => setMenu(null)} items={[
        { label: tr('shell.ws.activity.rename'), onClick: () => renaming.set(menu.id) },
        { label: tr('shell.ws.activity.edit'), onClick: () => onEdit(menu.id, menu) },
        { label: tr('shell.ws.group.add'), disabled: (meta[menu.id]?.groups || []).length >= MAX_GROUPS, onClick: () => addGroup(menu.id) },
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
