// The Classic taskbar: the start button, search and task view, the pinned and open apps (one button
// per app: its windows are grouped), the Trash last, and the tray at the end (privacy dots, status
// items, keyboard layout, quick settings, the clock with the notification bell and a "show desktop"
// sliver). The keyboard layout and the bell can be turned off in Settings → Desktop & Dock → Taskbar. Centered by default (Settings → Desktop & Dock → alignment); bottom, top, left or right.
//
// Pinning follows the same rules and SDK calls as the default theme's Dock (sdk/dock.js, docs/DOCK.md):
// Files and Settings can't be removed (but move), the Trash is always last; drag a
// button to reorder it (a line shows where it lands), drag it off the bar and hold to unpin it, drop
// apps from the start menu. Click: opens the app or brings its window (if it was already in front,
// minimizes it); right click: its jump list.
//
// One taskbar per display (0.8.8.6; Settings → Desktop & Dock → Taskbar → "Show the taskbar on every display", on by
// default, the same settings.menuBarAllDisplays as Lord Flow's menu bar): each one is whole (start, pinned and
// running apps, Trash, tray, clock) and every bar shows every app, as there is no per-display window list. `area`
// is the display it is drawn on; the panels it opens go on that display (themes/classic/index.jsx). The bars share
// one clock timer and one network poll, and each registers its own drop target for pins.
import {
  View, Text, Layer, useSettings, useWindows, wm, windowTitle, formatTime, formatDate, t as tr, settings, sharedTicker,
  useDockEntries, dockActivate, dockMenu, useStore, dockDrag, beginDockDrag, registerDockTarget, dockDragJustEnded,
  useTrash, openTrash, refreshTrash, trashMenu, StatusItems, PrivacyIndicators, keyboard, useSound, defaultSink, LordOSLogo,
  mainDisplay, toggleShowDesktop, menuBarPrefs,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { Menu } from './Menu.jsx';
import { SearchGlyph, TaskViewGlyph, NetworkGlyph, SpeakerGlyph, BellGlyph, TrashGlyph } from '../glyphs.jsx';

export const BAR_H = 48;      // bar at the top or bottom
export const BAR_W = 56;      // bar on the left or right (icons only)
export const barSize = (pos) => (pos === 'left' || pos === 'right' ? BAR_W : BAR_H);

// The taskbar's default pins (Files first; the user's own order lives in settings.dockPinned).
export const TASKBAR_DEFAULT = ['files', 'terminal', 'store', 'settings'];

// The network as the tray shows it: polled every 5 s (no per-frame work), once for every taskbar and the quick
// settings (a shared ticker: it polls only while one of them is shown, and they re-render only when it changes).
const VPN = (i) => ['wireguard', 'tun', 'xfrm', 'ppp'].includes(i.type) || /^(wg|ovpn|ips|ppp|oc|fvpn)-/.test(i.name);
export function readNet() {
  if (typeof lord === 'undefined' || !lord.net) return { online: false, wifi: false, name: '', ip: '', vpn: 0 };
  let ifs = [];
  try { ifs = lord.net.interfaces(); } catch (e) { ifs = []; }
  const main = ifs.find((i) => !VPN(i) && i.type !== 'loopback' && i.addresses.some((a) => a.scope === 'global' && a.family === 4));
  return {
    online: !!main, wifi: main?.type === 'wifi', name: main?.name || '', vpn: ifs.filter((i) => VPN(i) && i.up).length,
    ip: main?.addresses.find((a) => a.family === 4 && a.scope === 'global')?.address || '',
  };
}
export const netTicker = sharedTicker(() => JSON.stringify(readNet()), 5000);
export function useNet() {
  const key = useStore(netTicker);
  return useMemo(() => JSON.parse(key), [key]);
}

// What the bar shows, in order: ❖1..❖9 open them (themes/classic/index.jsx).
let barEntries = [];
export const taskbarEntries = () => barEntries;

function BarButton({ t, vert, active, onClick, onContextMenu, onMouseDown, onEnter, onLeave, children, refFn, width, style }) {
  const [hover, setHover] = useState(false);
  return (
    <View ref={refFn} onClick={onClick} onContextMenu={onContextMenu} onMouseDown={onMouseDown}
      onMouseEnter={() => { setHover(true); onEnter?.(); }} onMouseLeave={() => { setHover(false); onLeave?.(); }}
      style={{ width: width || (vert ? 44 : 40), height: vert ? 40 : 40, borderRadius: 6, alignItems: 'center', justifyContent: 'center',
        backgroundColor: active ? (t.mode === 'dark' ? 'rgba(255,255,255,0.10)' : 'rgba(255,255,255,0.7)') : hover ? t.hover : 'transparent',
        border: `1px solid ${active ? t.tileBorder : 'transparent'}`, ...style }}>
      {children}
    </View>
  );
}

// area: the display the bar is on (global coordinates). start / quick / notif: that panel is open from this bar.
export function Taskbar({ t, pos, align, area, start, onStart, onSearch, taskView, onTaskView, quick, onQuick, notif, onNotif, unread, onEmptyTrash }) {
  const entries = useDockEntries(TASKBAR_DEFAULT);
  barEntries = entries;
  const { focused, windows } = useWindows();
  const [s] = useSettings();
  const bar = menuBarPrefs(s);      // what the tray shows (Settings → Desktop & Dock → Taskbar, issue #58)
  const [jump, setJump] = useState(null);
  const [trashAt, setTrashAt] = useState(null);
  const [tip, setTip] = useState(null);
  const tipTimer = useRef(0);
  const trash = useTrash();
  const drag = useStore(dockDrag);
  const snd = useSound();
  const out = defaultSink(snd);
  const net = useNet();
  const vert = pos === 'left' || pos === 'right';
  const refs = useRef(new Map());
  const barRef = useRef(null);
  const trashRef = useRef(null);
  const center = align !== 'start';
  const m = area || mainDisplay();
  const barId = m.id;
  useEffect(() => () => clearTimeout(tipTimer.current), []);

  const click = (e) => {
    if (dockDragJustEnded()) return;
    const top = e.windows.find((w) => w.id === focused);
    if (top && e.windows.length === 1) wm.minimize(top.id); else dockActivate(e);
  };
  // where the buttons are (global coordinates), for the drop position
  const centerOf = (id) => {
    const n = refs.current.get(id);
    if (!n) return null;
    const [x, y, w, h] = lord.getLayout(n.id);
    return vert ? y + h / 2 : x + w / 2;
  };
  const geo = useRef({});
  geo.current = { entries, vert, drag, barId };
  useEffect(() => registerDockTarget({
    hit(x, y) {
      const g = geo.current;
      if (!barRef.current) return null;
      const [bx, by, bw, bh] = lord.getLayout(barRef.current.id);
      if (!(x >= bx - 16 && x < bx + bw + 16 && y >= by - 16 && y < by + bh + 16)) return null;
      if (trashRef.current) {
        const [tx, ty, tw, th] = lord.getLayout(trashRef.current.id);
        if (x >= tx && x < tx + tw && y >= ty && y < ty + th) return { trash: true, bar: g.barId };
      }
      const a = g.vert ? y : x;
      const own = g.drag?.source === 'dock' ? g.drag.id : null;
      const cs = g.entries.filter((e) => e.pinned && !e.recent && e.id !== own).map((e) => centerOf(e.id)).filter((c) => c != null);
      return { index: cs.filter((c) => c < a).length, bar: g.barId };
    },
    near(x, y) {
      if (!barRef.current) return false;
      const [bx, by, bw, bh] = lord.getLayout(barRef.current.id);
      const dx = x < bx ? bx - x : x > bx + bw ? x - bx - bw : 0, dy = y < by ? by - y : y > by + bh ? y - by - bh : 0;
      return Math.hypot(dx, dy) <= 60;
    },
  }), []);

  // the drop line: before the pinned button at that index (after the last one at the end), on the bar under the pointer
  let dropLine = null;
  const overHere = !!drag && !!drag.over && drag.over.bar === barId;
  if (overHere && drag.over.index != null && barRef.current) {
    const own = drag.source === 'dock' ? drag.id : null;
    const pinned = entries.filter((e) => e.pinned && !e.recent && e.id !== own);
    const k = Math.max(0, Math.min(pinned.length, drag.over.index));
    const ref = refs.current.get((pinned[k] || pinned[pinned.length - 1])?.id);
    if (ref) {
      const [bx, by] = lord.getLayout(barRef.current.id);
      const [x, y, w, h] = lord.getLayout(ref.id);
      const at = pinned[k] ? (vert ? y - by - 3 : x - bx - 3) : (vert ? y - by + h + 1 : x - bx + w + 1);
      dropLine = vert ? { top: at, left: 8, right: 8, height: 2 } : { left: at, top: 8, bottom: 8, width: 2 };
    }
  }

  // a name above (or beside) the hovered button, after a short wait
  const showTip = (text, node) => {
    clearTimeout(tipTimer.current);
    tipTimer.current = setTimeout(() => {
      if (!node) return;
      const [x, y, w, h] = lord.getLayout(node.id);
      setTip({ text, x: x + w / 2, y, w, h });
    }, 600);
  };
  const hideTip = () => { clearTimeout(tipTimer.current); setTip(null); };
  // a menu next to the bar, at the point along it
  const menuAt = (ev) => (pos === 'bottom' ? { x: ev.x - 20, y: m.y + m.h - BAR_H - 6, above: true }
    : pos === 'top' ? { x: ev.x - 20, y: m.y + BAR_H + 6 }
      : pos === 'left' ? { x: m.x + BAR_W + 6, y: ev.y - 20 } : { x: m.x + m.w - BAR_W - 246, y: ev.y - 20 });

  const indicator = (e) => {
    if (!e.running) return null;
    const on = e.focused || e.attention;     // a window that asked for attention (xdg-activation): long, in the accent
    const len = on ? 16 : 6;
    const at = pos === 'top' ? { top: 2, left: 20 - len / 2, width: len, height: 3 }
      : pos === 'left' ? { left: 2, top: 20 - len / 2, width: 3, height: len }
        : pos === 'right' ? { right: 2, top: 20 - len / 2, width: 3, height: len } : { bottom: 2, left: 20 - len / 2, width: len, height: 3 };
    return <View style={{ position: 'absolute', ...at, borderRadius: 2, backgroundColor: on ? t.accent : t.textDim }} />;
  };

  const appButton = (e) => {
    const dragged = drag && drag.source === 'dock' && drag.id === e.id;
    const title = e.windows.length > 1 ? `${e.name} · ${e.windows.length}` : e.windows.length ? windowTitle(e.windows[e.windows.length - 1]) : e.name;
    return (
      <BarButton key={e.id} t={t} vert={vert} active={e.focused} refFn={(n) => { if (n) refs.current.set(e.id, n); else refs.current.delete(e.id); }}
        onClick={() => { hideTip(); click(e); }} onContextMenu={(ev) => { hideTip(); setJump({ entry: e, at: menuAt(ev) }); }}
        onMouseDown={(ev) => { if (!ev.button) beginDockDrag({ id: e.id, Icon: e.Icon, x: ev.x, y: ev.y, source: 'dock' }); }}
        onEnter={() => showTip(title, refs.current.get(e.id))} onLeave={hideTip} style={{ opacity: dragged ? 0.35 : 1 }}>
        <e.Icon size={26} />
        {indicator(e)}
        {/* a web app's unread count (SetBadge): a red badge, "99+" past 99 (the window count moves down then) */}
        {e.badge ? <View style={{ position: 'absolute', right: 1, top: 1, minWidth: 14, height: 14, paddingHorizontal: 3, borderRadius: 7,
          backgroundColor: '#ff3b30', alignItems: 'center', justifyContent: 'center', pointerEvents: 'none' }}>
          <Text style={{ fontSize: 9, fontWeight: 700, color: '#ffffff', whiteSpace: 'nowrap' }}>{e.badge}</Text></View> : null}
        {e.windows.length > 1 ? <View style={{ position: 'absolute', right: 3, [e.badge ? 'bottom' : 'top']: 3, minWidth: 12, height: 12, paddingHorizontal: 2, borderRadius: 6,
          backgroundColor: t.control, border: `1px solid ${t.tileBorder}`, alignItems: 'center', justifyContent: 'center' }}>
          <Text style={{ fontSize: 8, fontWeight: 700, color: t.text }}>{e.windows.length}</Text></View> : null}
      </BarButton>
    );
  };

  const startRef = useRef(null), searchRef = useRef(null), tvRef = useRef(null);
  const apps = (
    <View style={{ flexDirection: vert ? 'column' : 'row', alignItems: 'center', gap: 2, direction: 'ltr' }}>
      <BarButton t={t} vert={vert} active={start} refFn={startRef} onClick={() => { hideTip(); onStart(); }}
        onEnter={() => showTip(tr('shell.classic.start'), startRef.current)} onLeave={hideTip}>
        <LordOSLogo size={26} />
      </BarButton>
      <BarButton t={t} vert={vert} refFn={searchRef} onClick={() => { hideTip(); onSearch(); }}
        onEnter={() => showTip(tr('shell.classic.search'), searchRef.current)} onLeave={hideTip}>
        <SearchGlyph color={t.text} size={17} />
      </BarButton>
      <BarButton t={t} vert={vert} active={taskView} refFn={tvRef} onClick={() => { hideTip(); onTaskView(); }}
        onEnter={() => showTip(tr('shell.desktops.overview'), tvRef.current)} onLeave={hideTip}>
        <TaskViewGlyph color={t.text} bg={t.mode === 'dark' ? '#2a2a30' : '#eceef2'} size={19} />
      </BarButton>
      {entries.map(appButton)}
      <View style={vert ? { width: 24, height: 1, marginVertical: 3, backgroundColor: t.separator } : { width: 1, height: 24, marginHorizontal: 3, backgroundColor: t.separator }} />
      <View ref={trashRef} dropTrash onMouseUp={() => setTimeout(refreshTrash, 60)}>
        <BarButton t={t} vert={vert} active={overHere && !!drag.over.trash} onClick={() => { if (!dockDragJustEnded()) openTrash(); }}
          onContextMenu={(ev) => setTrashAt(menuAt(ev))} onEnter={() => showTip(tr('shell.classic.trash'), trashRef.current)} onLeave={hideTip}>
          <TrashGlyph size={24} full={trash.full} dark={t.mode === 'dark'} />
        </BarButton>
      </View>
    </View>
  );

  const level = out ? (out.mute || out.level === 0 ? 0 : out.level < 0.5 ? 1 : 2) : 2;
  const qsGlyphs = (
    <View style={{ flexDirection: vert ? 'column' : 'row', alignItems: 'center', gap: vert ? 6 : 8, direction: 'ltr' }}>
      <NetworkGlyph color={t.text} size={15} off={!net.online} />
      {out ? <SpeakerGlyph color={t.text} size={16} level={level} muted={out.mute} /> : null}
    </View>
  );
  const tray = (
    <View style={{ flexDirection: vert ? 'column' : 'row', alignItems: 'center', gap: 2 }}>
      <PrivacyIndicators />
      {/* as tall as the bar's buttons, so a status item's panel opens clear of the bar (sdk/statusplace.js) */}
      {vert ? null : <StatusItems color={t.text} height={40} />}
      {!vert && bar.keyboard && keyboard.configured().length > 1 ? (
        <BarButton t={t} width={36} onClick={() => keyboard.next()}>
          <Text style={{ fontSize: 11, fontWeight: 600, color: t.text }}>{keyboard.label(keyboard.active())}</Text>
        </BarButton>
      ) : null}
      <BarButton t={t} vert={vert} active={quick} width={vert ? 44 : 64} onClick={onQuick}>{qsGlyphs}</BarButton>
      <BarButton t={t} vert={vert} active={notif} width={vert ? 48 : undefined} onClick={onNotif}
        style={vert ? { height: 52 } : { paddingHorizontal: 8, width: undefined, flexDirection: 'row', gap: 8 }}>
        <TrayClock t={t} vert={vert} />
        {bar.bell ? <BellGlyph color={t.text} size={14} dot={unread ? t.accent : null} /> : null}
      </BarButton>
      {vert ? null : <View onClick={toggleShowDesktop} style={{ width: 8, height: BAR_H, marginStart: 2, borderStartWidth: 1, borderColor: t.separator }} />}
    </View>
  );

  const edge = vert
    ? { top: 0, bottom: 0, width: BAR_W, [pos]: 0, flexDirection: 'column', paddingVertical: 6,
      [pos === 'left' ? 'borderRightWidth' : 'borderLeftWidth']: 1 }
    : { left: 0, right: 0, height: BAR_H, [pos]: 0, flexDirection: 'row', paddingHorizontal: 6,
      [pos === 'top' ? 'borderBottomWidth' : 'borderTopWidth']: 1 };
  return (
    <View ref={barRef} style={{ position: 'absolute', ...edge, alignItems: 'center', borderColor: t.barBorder,
      backgroundColor: t.barBg, backdropFilter: t.blur ? `blur(${t.blur}px)` : undefined }}>
      {center ? <View style={{ flex: 1 }} /> : null}
      {apps}
      <View style={{ flex: 1, flexDirection: vert ? 'column' : 'row', justifyContent: 'flex-end', alignItems: 'center' }}>{tray}</View>
      {dropLine && <View style={{ position: 'absolute', ...dropLine, backgroundColor: t.accent, pointerEvents: 'none' }} />}
      {jump && <Menu t={t} {...jump.at} onClose={() => setJump(null)} items={jumpItems(entries.find((x) => x.id === jump.entry.id) || jump.entry)} />}
      {trashAt && <Menu t={t} {...trashAt} onClose={() => setTrashAt(null)} items={trashMenu({ onEmpty: onEmptyTrash })} />}
      {tip && (
        <Layer name="tooltip">
          <TipBubble t={t} tip={tip} pos={pos} area={m} />
        </Layer>
      )}
    </View>
  );
}

// Time over the date; its own component, so only it renders when the minute (or second) changes. One timer for every
// taskbar's clock (sdk/ticker.js); it reads Settings → Date & Time on each tick, so a change shows within a second.
const clockTicker = sharedTicker((d) => `${formatTime(d, { seconds: !!settings.get().clock?.seconds })}\n${formatDate(d, 'numeric')}`, 1000);
function TrayClock({ t, vert }) {
  const txt = useStore(clockTicker);
  const [time, date] = txt.split('\n');
  return (
    <View style={{ alignItems: vert ? 'center' : 'flex-end' }}>
      <Text style={{ fontSize: vert ? 10 : 12, color: t.text, whiteSpace: 'nowrap' }}>{time}</Text>
      {vert ? null : <Text style={{ fontSize: 11, color: t.text, whiteSpace: 'nowrap' }}>{date}</Text>}
    </View>
  );
}

// The SDK's Dock items (dockMenu), with taskbar wording: pin/unpin, close all windows.
function jumpItems(entry) {
  const items = [];
  for (const it of dockMenu(entry, { keepLabel: tr('shell.dock.pin'), removeLabel: tr('shell.dock.unpin') })) {
    if (it !== '-' && it.label === tr('shell.dock.quit')) items.push({ ...it, label: tr('shell.dock.closeAll') });
    else if (it !== '-' && it.label === tr('shell.dock.hide')) continue;
    else items.push(it);
  }
  return items;
}

function TipBubble({ t, tip, pos, area }) {
  const w = Math.min(320, Math.max(40, tip.text.length * 7 + 20));
  const at = pos === 'bottom' ? { left: tip.x - w / 2, top: tip.y - 34 } : pos === 'top' ? { left: tip.x - w / 2, top: tip.y + tip.h + 8 }
    : pos === 'left' ? { left: tip.x + tip.w / 2 + 8, top: tip.y + tip.h / 2 - 13 } : { left: tip.x - tip.w / 2 - w - 8, top: tip.y + tip.h / 2 - 13 };
  return (
    <View style={{ position: 'absolute', ...at, left: Math.max(area.x + 4, Math.min(at.left, area.x + area.w - w - 4)), width: w, height: 26, borderRadius: 4,
      alignItems: 'center', justifyContent: 'center', backgroundColor: t.popupBg, border: `1px solid ${t.windowBorder}`, boxShadow: '0 4px 12px rgba(0,0,0,0.25)', pointerEvents: 'none' }}>
      <Text style={{ fontSize: 12, color: t.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{tip.text}</Text>
    </View>
  );
}
