// The clock's panel (issue #58): this session's notifications (sdk/notify.js notifyHistory) grouped by app, with Do
// Not Disturb and "Clear all", and a month calendar below it, as in Lord Classic. The calendar can be turned off in
// Settings → Desktop & Dock → Menu Bar; a click on a day opens the calendar app chosen in Settings → Default Apps
// (LordOS Calendar unless the user chose another one, sdk/sysopen.js openCalendarApp).
import {
  View, Text, ScrollView, Toggle, useStore, notifyHistory, clearNotifyHistory, settings, notifyPrefs, dndActive, useSettings,
  getApp, appName, formatTime, formatDate, monthName, weekdayName, locale, addListener, t as tr, menuBarPrefs, monthCells, weekStartFor,
  openCalendarApp,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';

const W = 340;

// A chevron from two borders, pointing left or right (left to right always: months go forward to the right).
function Arrow({ color, dir }) {
  return (
    <View style={{ width: 5, height: 5, borderRightWidth: 1.5, borderBottomWidth: 1.5, borderColor: color, transform: `rotate(${dir === 'left' ? 135 : -45}deg)` }} />
  );
}

function Item({ t, n, onRemove }) {
  const [hover, setHover] = useState(false);
  return (
    <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ padding: 10, paddingEnd: 28, gap: 2, borderRadius: 10, backgroundColor: t.hover, border: `1px solid ${t.separator}` }}>
      <View style={{ flexDirection: 'row', gap: 8 }}>
        <Text style={{ flex: 1, fontSize: 12, fontWeight: 600, color: t.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{n.title}</Text>
        <Text style={{ fontSize: 11, color: t.textDim }}>{formatTime(new Date(n.time))}</Text>
      </View>
      {n.body ? <Text style={{ fontSize: 12, color: t.textDim, lineHeight: 1.35, textAlign: 'start', numberOfLines: 3 }}>{n.body}</Text> : null}
      {n.actions?.length ? (
        <View style={{ flexDirection: 'row', gap: 6, marginTop: 6 }}>
          {n.actions.map((a) => (
            <View key={a.id} onClick={() => { onRemove(); try { a.run?.(); } catch (e) { console.error('notify:', e); } }}
              style={{ height: 24, paddingHorizontal: 10, borderRadius: 6, justifyContent: 'center', backgroundColor: t.control }}>
              <Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{a.label}</Text>
            </View>
          ))}
        </View>
      ) : null}
      {hover ? (
        <View onClick={onRemove} style={{ position: 'absolute', top: 8, insetInlineEnd: 6, width: 18, height: 18, borderRadius: 9, alignItems: 'center', justifyContent: 'center',
          backgroundColor: t.control }}>
          <Text style={{ fontSize: 10, color: t.textDim }}>✕</Text>
        </View>
      ) : null}
    </View>
  );
}

function MonthCalendar({ t, onClose }) {
  const today = new Date();
  const [cur, setCur] = useState(() => new Date(today.getFullYear(), today.getMonth(), 1));
  const first = weekStartFor(locale.get().lang);
  const y = cur.getFullYear(), m = cur.getMonth();
  const cells = monthCells(y, m, first);
  const isToday = (d) => d && y === today.getFullYear() && m === today.getMonth() && d === today.getDate();
  const cell = (W - 24) / 7;
  const nav = (dir) => (
    <View onClick={() => setCur(new Date(y, m + (dir === 'left' ? -1 : 1), 1))}
      style={{ width: 24, height: 24, borderRadius: 6, alignItems: 'center', justifyContent: 'center' }}>
      <Arrow color={t.text} dir={dir} />
    </View>
  );
  return (
    <View style={{ gap: 4 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center' }}>
        <Text style={{ flex: 1, fontSize: 13, fontWeight: 600, color: t.text, textAlign: 'start' }}>{`${monthName(m)} ${y}`}</Text>
        <View style={{ flexDirection: 'row', gap: 2, direction: 'ltr' }}>{nav('left')}{nav('right')}</View>
      </View>
      <View style={{ flexDirection: 'row' }}>
        {Array.from({ length: 7 }, (_, i) => (
          <Text key={i} style={{ width: cell, fontSize: 11, color: t.textDim, textAlign: 'center' }}>{weekdayName((i + first) % 7).slice(0, 2)}</Text>
        ))}
      </View>
      <View style={{ flexDirection: 'row', flexWrap: 'wrap' }}>
        {cells.map((d, i) => (
          <View key={i} onClick={d ? () => { onClose(); openCalendarApp(new Date(y, m, d, 9).getTime()); } : undefined}
            style={{ width: cell, height: 30, alignItems: 'center', justifyContent: 'center' }}>
            {d ? (
              <View style={{ width: 26, height: 26, borderRadius: 13, alignItems: 'center', justifyContent: 'center', backgroundColor: isToday(d) ? t.accent : 'transparent' }}>
                <Text style={{ fontSize: 12, color: isToday(d) ? '#fff' : t.text, fontWeight: isToday(d) ? 600 : 400 }}>{d}</Text>
              </View>
            ) : null}
          </View>
        ))}
      </View>
    </View>
  );
}

const inside = (inst, ref) => { for (let n = inst; n; n = n.parent) if (n === ref.current) return true; return false; };

// area: the display of the menu bar it opened from (global coordinates); top: the menu bar's height
export function NotificationCenter({ t, top, onClose, area }) {
  const ref = useRef(null);
  const list = useStore(notifyHistory);
  const [s] = useSettings();
  const p = menuBarPrefs(s);
  // a click on the menu bar is the clock's own (it toggles the panel); anywhere else below it closes the panel
  useEffect(() => addListener('pointerdown', (e) => { if (!inside(e.target, ref) && e.y > area.y + top) onClose(); }), [onClose, area.y]);
  const dnd = dndActive();
  const setDnd = (on) => settings.set((x) => ({ ...x, notifications: { ...notifyPrefs(x), dnd: !!on } }));
  const groups = [];
  const byKey = new Map();
  for (const n of list) {
    const k = n.app || n.label || '';
    if (!byKey.has(k)) { const g = { key: k, items: [] }; byKey.set(k, g); groups.push(g); }
    byKey.get(k).items.push(n);
  }
  const remove = (id) => notifyHistory.set((l) => l.filter((x) => x.id !== id));
  const calH = p.calendar ? 290 : 0;
  const listH = Math.max(140, Math.min(460, area.h - top - calH - 40));
  const panel = (children, extra) => (
    <View style={{ width: W, borderRadius: 16, overflow: 'hidden', backgroundColor: t.panelBg, backdropFilter: `blur(${t.blur}px)`,
      border: `1px solid ${t.windowBorder}`, boxShadow: '0 16px 40px rgba(0,0,0,0.4)', ...extra }}>{children}</View>
  );
  return (
    <View ref={ref} style={{ position: 'absolute', left: area.x + area.w - W - 10, top: area.y + top + 6, width: W, gap: 10 }}>
      {panel(
        <View style={{ flex: 1 }}>
          <View style={{ height: 44, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 14, gap: 8 }}>
            <Text style={{ flex: 1, fontSize: 13, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('shell.classic.notifications')}</Text>
            <Text style={{ fontSize: 11, color: t.textDim }}>{tr('shell.classic.dnd')}</Text>
            <Toggle small on={dnd} onChange={setDnd} />
            {list.length ? (
              <View onClick={clearNotifyHistory} style={{ height: 24, paddingHorizontal: 10, borderRadius: 6, justifyContent: 'center', backgroundColor: t.control }}>
                <Text style={{ fontSize: 12, color: t.text }}>{tr('shell.classic.clearAll')}</Text>
              </View>
            ) : null}
          </View>
          {groups.length ? (
            <ScrollView style={{ height: listH - 44 }}>
              <View style={{ paddingHorizontal: 10, paddingBottom: 10, gap: 10 }}>
                {groups.map((g) => {
                  const app = g.key ? getApp(g.key) : null;
                  return (
                    <View key={g.key || '-'} style={{ gap: 6 }}>
                      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 4 }}>
                        {app?.Icon ? <app.Icon size={16} /> : null}
                        <Text style={{ fontSize: 12, fontWeight: 600, color: t.textDim, textAlign: 'start' }}>{app ? appName(app) : g.items[0].label || ''}</Text>
                      </View>
                      {g.items.slice(0, 6).map((n) => <Item key={n.id} t={t} n={n} onRemove={() => remove(n.id)} />)}
                    </View>
                  );
                })}
              </View>
            </ScrollView>
          ) : (
            <View style={{ height: 96, alignItems: 'center', justifyContent: 'center' }}>
              <Text style={{ fontSize: 12, color: t.textDim }}>{tr('shell.classic.noNotifications')}</Text>
            </View>
          )}
        </View>)}
      {p.calendar ? panel(
        <View style={{ padding: 12, gap: 6 }}>
          <Text style={{ fontSize: 12, color: t.textDim, textAlign: 'start' }}>{formatDate(new Date(), 'long')}</Text>
          <MonthCalendar t={t} onClose={onClose} />
        </View>) : null}
    </View>
  );
}
