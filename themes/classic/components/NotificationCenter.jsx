// The notification panel, from the clock: this session's notifications (sdk/notify.js
// notifyHistory) grouped by app, with Do Not Disturb and "Clear all", and a month calendar below.
import {
  View, Text, ScrollView, Toggle, useStore, notifyHistory, clearNotifyHistory, settings, notifyPrefs, dndActive, useSettings,
  getApp, appName, formatTime, formatDate, monthName, weekdayName, locale, addListener, wm, t as tr,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { BellGlyph, CloseGlyph, Chevron } from '../glyphs.jsx';

const W = 380;

function Item({ t, n, onRemove }) {
  const [hover, setHover] = useState(false);
  return (
    <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ padding: 10, paddingEnd: 28, gap: 2, borderRadius: 6, backgroundColor: t.tile, border: `1px solid ${t.tileBorder}` }}>
      <View style={{ flexDirection: 'row', gap: 8 }}>
        <Text style={{ flex: 1, fontSize: 13, fontWeight: 600, color: t.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{n.title}</Text>
        <Text style={{ fontSize: 11, color: t.textDim }}>{formatTime(new Date(n.time))}</Text>
      </View>
      {n.body ? <Text style={{ fontSize: 12, color: t.textDim, lineHeight: 1.35, textAlign: 'start', numberOfLines: 3 }}>{n.body}</Text> : null}
      {n.actions?.length ? (
        <View style={{ flexDirection: 'row', gap: 6, marginTop: 6 }}>
          {n.actions.map((a) => (
            <View key={a.id} onClick={() => { onRemove(); try { a.run?.(); } catch (e) { console.error('notify:', e); } }}
              style={{ height: 26, paddingHorizontal: 10, borderRadius: 4, justifyContent: 'center', backgroundColor: t.hover }}>
              <Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{a.label}</Text>
            </View>
          ))}
        </View>
      ) : null}
      {hover ? (
        <View onClick={onRemove} style={{ position: 'absolute', top: 8, insetInlineEnd: 6, width: 18, height: 18, borderRadius: 4, alignItems: 'center', justifyContent: 'center',
          backgroundColor: t.hover }}>
          <CloseGlyph color={t.textDim} size={9} />
        </View>
      ) : null}
    </View>
  );
}

function MonthCalendar({ t }) {
  const today = new Date();
  const [cur, setCur] = useState(() => new Date(today.getFullYear(), today.getMonth(), 1));
  const first = (locale.get().lang || 'en').startsWith('en') ? 0 : 1;      // the week starts on Sunday in English
  const y = cur.getFullYear(), m = cur.getMonth();
  const lead = (new Date(y, m, 1).getDay() - first + 7) % 7;
  const days = new Date(y, m + 1, 0).getDate();
  const cells = [...Array(lead).fill(null), ...Array.from({ length: days }, (_, i) => i + 1)];
  while (cells.length % 7) cells.push(null);
  const isToday = (d) => d && y === today.getFullYear() && m === today.getMonth() && d === today.getDate();
  const cell = (W - 32) / 7;
  return (
    <View style={{ gap: 6 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center' }}>
        <Text style={{ flex: 1, fontSize: 13, fontWeight: 600, color: t.text, textAlign: 'start' }}>{`${monthName(m)} ${y}`}</Text>
        <View style={{ flexDirection: 'row', gap: 2, direction: 'ltr' }}>
          <View onClick={() => setCur(new Date(y, m - 1, 1))} style={{ width: 28, height: 28, borderRadius: 4, alignItems: 'center', justifyContent: 'center' }}>
            <Chevron color={t.text} size={6} dir="up" />
          </View>
          <View onClick={() => setCur(new Date(y, m + 1, 1))} style={{ width: 28, height: 28, borderRadius: 4, alignItems: 'center', justifyContent: 'center' }}>
            <Chevron color={t.text} size={6} dir="down" />
          </View>
        </View>
      </View>
      <View style={{ flexDirection: 'row' }}>
        {Array.from({ length: 7 }, (_, i) => (
          <Text key={i} style={{ width: cell, fontSize: 11, color: t.textDim, textAlign: 'center' }}>{weekdayName((i + first) % 7).slice(0, 2)}</Text>
        ))}
      </View>
      <View style={{ flexDirection: 'row', flexWrap: 'wrap' }}>
        {cells.map((d, i) => (
          <View key={i} onClick={d ? () => wm.open('calendar', { args: { at: new Date(y, m, d, 9).getTime() } }) : undefined}
            style={{ width: cell, height: 32, alignItems: 'center', justifyContent: 'center' }}>
            {d ? (
              <View style={{ width: 28, height: 28, borderRadius: 14, alignItems: 'center', justifyContent: 'center', backgroundColor: isToday(d) ? t.accent : 'transparent' }}>
                <Text style={{ fontSize: 12, color: isToday(d) ? '#fff' : t.text, fontWeight: isToday(d) ? 600 : 400 }}>{d}</Text>
              </View>
            ) : null}
          </View>
        ))}
      </View>
    </View>
  );
}

export function NotificationCenter({ t, at, onClose, isOnBar }) {
  const ref = useRef(null);
  const list = useStore(notifyHistory);
  useSettings();
  useEffect(() => addListener('pointerdown', (e) => {
    for (let n = e.target; n; n = n.parent) if (n === ref.current) return;
    if (!isOnBar(e.x, e.y)) onClose();
  }), []);
  useEffect(() => addListener('keydown', (e) => { if (e.key === 'Escape') onClose(); }), []);
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
  const calH = 300;
  const listH = Math.max(140, at.height - calH - 12);
  const panel = (children, h, extra) => (
    <View style={{ width: W, height: h, borderRadius: 10, overflow: 'hidden', backgroundColor: t.panelBg,
      backdropFilter: t.blur ? `blur(${t.blur}px)` : undefined, border: `1px solid ${t.windowBorder}`, boxShadow: t.shadow, ...extra }}>{children}</View>
  );
  return (
    <View ref={ref} style={{ position: 'absolute', left: at.left, top: at.top, width: W, gap: 12 }}>
      {panel(
        <View style={{ flex: 1 }}>
          <View style={{ height: 48, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 16, gap: 10 }}>
            <Text style={{ flex: 1, fontSize: 14, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('shell.classic.notifications')}</Text>
            <Text style={{ fontSize: 11, color: t.textDim }}>{tr('shell.classic.dnd')}</Text>
            <Toggle small on={dnd} onChange={setDnd} />
            {list.length ? (
              <View onClick={clearNotifyHistory} style={{ height: 26, paddingHorizontal: 10, borderRadius: 4, justifyContent: 'center', backgroundColor: t.control, border: `1px solid ${t.tileBorder}` }}>
                <Text style={{ fontSize: 12, color: t.text }}>{tr('shell.classic.clearAll')}</Text>
              </View>
            ) : null}
          </View>
          {groups.length ? (
            <ScrollView style={{ height: listH - 48 }}>
              <View style={{ paddingHorizontal: 12, paddingBottom: 12, gap: 12 }}>
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
            <View style={{ height: listH - 48, alignItems: 'center', justifyContent: 'center', gap: 10 }}>
              <BellGlyph color={t.textDim} size={28} />
              <Text style={{ fontSize: 13, color: t.textDim }}>{tr('shell.classic.noNotifications')}</Text>
            </View>
          )}
        </View>, listH)}
      {panel(
        <View style={{ padding: 16, gap: 8 }}>
          <Text style={{ fontSize: 13, color: t.text, textAlign: 'start' }}>{formatDate(new Date(), 'long')}</Text>
          <MonthCalendar t={t} />
        </View>, calH)}
    </View>
  );
}
