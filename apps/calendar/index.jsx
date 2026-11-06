// Calendar — day, week and month views of the calendars kept by lord-maild (local, or synced with
// CalDAV): see docs/MAIL.md. Data comes from the SDK (calendars, calendarEvents); every call is a
// promise and usePim() changes whenever something changes, so views re-query on it.
import {
  View, Text, useTheme, useLocale, useAppWindow, useKeyboard, useWindowState, usePim, calendars, calendarEvents,
  Button, Segmented, ScrollView, Hairline, Toolbar, ToolbarButton, ToolbarGroup, ToolbarSpacer, EmptyState, formatDate, formatTime, weekdayName, direction, textDirection, t, openDavSetup,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import {
  RED, sod, addDays, addMonths, sameDay, startOfWeek, ymd, parseYmd, minutesOf, monthYear, MiniMonth, ColorCheck, Layers, Popover, MenuList,
  useLoad, useNow,
} from './ui.jsx';
import { EventSheet, CalendarSheet } from './editor.jsx';

// ---- icon: a page with a red header strip and today's day number ----

export function Icon({ size: s }) {
  const day = new Date().getDate();
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, backgroundColor: '#ffffff', overflow: 'hidden',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, border: '1px solid rgba(0,0,0,0.08)' }}>
      <View style={{ height: s * 0.27, backgroundColor: '#f2463d', alignItems: 'center', justifyContent: 'center' }}>
        <View style={{ flexDirection: 'row', gap: s * 0.05 }}>
          {[0, 1, 2].map((i) => <View key={i} style={{ width: s * 0.07, height: s * 0.07, borderRadius: s * 0.035, backgroundColor: 'rgba(255,255,255,0.85)' }} />)}
        </View>
      </View>
      <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center' }}>
        <Text style={{ fontSize: s * 0.46, fontWeight: 300, color: '#1c1c1e' }}>{String(day)}</Text>
      </View>
    </View>
  );
}

const HOUR = 48;                 // px per hour in the day and week grids
const GUTTER = 56;

// days an event covers (all-day: sdate..edate exclusive; timed: every day it touches)
function eventDays(ev) {
  if (ev.allDay) {
    const s = parseYmd(ev.sdate) || sod(ev.start), e = parseYmd(ev.edate) || sod(ev.end);
    const out = [];
    for (let d = s; d < e || out.length === 0; d = addDays(d, 1)) { out.push(d); if (out.length > 366) break; }
    return out;
  }
  const out = [];
  const last = new Date(Math.max(ev.start, ev.end - 1));
  for (let d = sod(ev.start); d <= last; d = addDays(d, 1)) { out.push(d); if (out.length > 366) break; }
  return out;
}

// index events by 'YYYY-MM-DD'
function byDay(events) {
  const m = new Map();
  for (const ev of events) for (const d of eventDays(ev)) { const k = ymd(d); if (!m.has(k)) m.set(k, []); m.get(k).push(ev); }
  for (const l of m.values()) l.sort((a, b) => (b.allDay - a.allDay) || a.start - b.start || String(a.title).localeCompare(String(b.title)));
  return m;
}

// overlapping timed segments of one day go side by side: [{ ev, top, bottom, col, cols }]
function layoutDay(day, events) {
  const d0 = sod(day).getTime(), d1 = addDays(day, 1).getTime();
  const segs = events.filter((e) => !e.allDay).map((ev) => {
    const s = Math.max(ev.start, d0), e = Math.min(Math.max(ev.end, ev.start + 15 * 60000), d1);
    return { ev, top: (s - d0) / 60000, bottom: (e - d0) / 60000 };
  }).sort((a, b) => a.top - b.top || b.bottom - a.bottom);
  let group = [], colsEnd = [], groupEnd = -1;
  const flush = () => { for (const g of group) g.cols = colsEnd.length; group = []; colsEnd = []; };
  for (const s of segs) {
    if (s.top >= groupEnd) { flush(); groupEnd = -1; }
    let c = colsEnd.findIndex((end) => end <= s.top);
    if (c < 0) { c = colsEnd.length; colsEnd.push(0); }
    colsEnd[c] = Math.max(s.bottom, s.top + 20);
    s.col = c;
    group.push(s);
    groupEnd = Math.max(groupEnd, colsEnd[c]);
  }
  flush();
  return segs;
}

const tint = (c, a) => (/^#[0-9a-f]{6}$/i.test(c || '') ? `${c}${a}` : `rgba(0,122,255,0.2)`);
const colorOf = (ev, cals) => ev.color || cals?.find((c) => c.id === ev.cal)?.color || '#007aff';

// ---- the app ----

export default function CalendarApp() {
  const th = useTheme();
  useLocale();
  const rev = usePim();
  const win = useAppWindow();
  const [view, setView] = useWindowState('view', 'week');
  const [cursorMs, setCursor] = useWindowState('cursor', () => sod(new Date()).getTime());
  const [sidebar, setSidebar] = useWindowState('sidebar', true);
  // narrow windows hide the sidebar unless it is asked for, as Calendar does
  const [narrowSidebar, setNarrowSidebar] = useWindowState('narrowSidebar', false);
  const rootRef = useRef(null);
  const rootW = useWidth(rootRef, 960);
  const narrow = rootW < 760;
  const showSidebar = narrow ? narrowSidebar : sidebar;
  const toggleSidebar = () => (narrow ? setNarrowSidebar(!narrowSidebar) : setSidebar(!sidebar));
  const [sheet, setSheet] = useState(null);        // { ev, isNew } | { cal } | { newCal: true }
  const now = useNow(30000);
  const cursor = new Date(cursorMs);
  const rangeFrom = view === 'day' ? sod(cursor) : view === 'week' ? startOfWeek(cursor) : startOfWeek(new Date(cursor.getFullYear(), cursor.getMonth(), 1));
  const rangeTo = addDays(rangeFrom, view === 'day' ? 1 : view === 'week' ? 7 : 42);
  const calsQ = useLoad(() => calendars.list(), [rev]);
  const evQ = useLoad(() => calendarEvents.range(rangeFrom.getTime(), rangeTo.getTime()), [rev, rangeFrom.getTime(), rangeTo.getTime()]);
  const cals = calsQ.data || [];
  const eventCals = cals.filter((c) => c.events);
  const events = evQ.data || [];
  const days = useMemo(() => byDay(events), [events]);

  // window args from an alarm's "Open": { at, uid, cal, nonce }
  const pending = useRef(null);
  useEffect(() => {
    const a = win.args;
    if (!a || !a.at) return;
    setCursor(sod(a.at).getTime());
    if (view === 'month') setView('day');
    pending.current = a;
  }, [win.args?.nonce, win.args?.at]);
  useEffect(() => {
    const p = pending.current;
    if (!p || !p.uid || !evQ.data) return;
    const ev = events.find((e) => e.uid === p.uid && (!p.cal || e.cal === p.cal) && (e.start === p.at || !e.occ));
    if (ev) { pending.current = null; setSheet({ ev }); }
  }, [evQ.data]);

  const go = (dir) => {
    if (view === 'day') setCursor(addDays(cursor, dir).getTime());
    else if (view === 'week') setCursor(addDays(cursor, dir * 7).getTime());
    else setCursor(addMonths(cursor, dir).getTime());
  };
  const today = () => setCursor(sod(new Date()).getTime());
  const defaultCal = () => eventCals.find((c) => !c.hidden) || eventCals[0];
  const newEvent = (day, min = null) => {
    const base = min == null ? (() => { const n = new Date(); return Math.min(23 * 60, (n.getHours() + 1) * 60); })() : min;
    const start = sod(day);
    start.setHours(Math.floor(base / 60), base % 60, 0, 0);
    const cal = defaultCal();
    setSheet({ isNew: true, ev: { title: '', start: start.getTime(), end: start.getTime() + 3600000, allDay: false, alarms: [], cal: cal ? cal.id : '' } });
  };

  useKeyboard((e) => {
    if (sheet) { if (e.key === 'Escape') setSheet(null); return; }
    const k = String(e.base || e.key || '').toLowerCase();
    const rtl = direction() === 'rtl';
    if ((e.meta || e.ctrl) && k === 't') today();
    else if ((e.meta || e.ctrl) && k === 'n') newEvent(cursor);
    else if ((e.meta || e.ctrl) && k === '1') setView('day');
    else if ((e.meta || e.ctrl) && k === '2') setView('week');
    else if ((e.meta || e.ctrl) && k === '3') setView('month');
    else if (e.key === 'ArrowLeft' && !e.meta) go(rtl ? 1 : -1);
    else if (e.key === 'ArrowRight' && !e.meta) go(rtl ? -1 : 1);
  });

  const unavailable = calsQ.error && !calsQ.data;
  let title;
  if (view === 'day') title = formatDate(cursor, 'long');
  else if (view === 'week') title = monthYear(addDays(startOfWeek(cursor), 3));
  else title = monthYear(cursor);
  const rtl = direction() === 'rtl';

  return (
    <View ref={rootRef} style={{ flex: 1, flexDirection: 'row', backgroundColor: th.windowBg }}>
      {showSidebar && (
        <Sidebar cals={eventCals} cursor={cursor} days={days} onPick={(d) => setCursor(sod(d).getTime())}
          onNew={() => setSheet({ newCal: true })} onEdit={(c) => setSheet({ cal: c })} />
      )}
      <View style={{ flex: 1 }}>
        {/* toolbar: sidebar and new event at the start, the view switch centered */}
        <Toolbar separator={false}>
          <ToolbarButton icon="▤" title={t('calendar.toggleSidebar')} onClick={toggleSidebar} active={showSidebar} />
          <ToolbarButton icon="+" iconSize={20} title={t('calendar.newEvent')} disabled={unavailable} onClick={() => newEvent(cursor)} />
          <ToolbarSpacer />
          <Segmented small appearance="neutral" value={view} onChange={setView}
            options={[['day', t('calendar.view.day')], ['week', t('calendar.view.week')], ['month', t('calendar.view.month')]]} />
          <ToolbarSpacer />
          <View style={{ width: 64 }} />
        </Toolbar>
        {/* the period title, with previous / today / next at the trailing edge */}
        <View style={{ height: narrow ? 42 : 50, flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 16 }}>
          <Text style={{ flex: 1, fontSize: narrow ? 17 : 22, fontWeight: 700, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{title}</Text>
          <ToolbarGroup>
            <ToolbarButton icon={rtl ? '›' : '‹'} iconSize={18} small title={t('calendar.previous')} onClick={() => go(-1)} />
            <ToolbarButton label={t('calendar.today')} small onClick={today} />
            <ToolbarButton icon={rtl ? '‹' : '›'} iconSize={18} small title={t('calendar.next')} onClick={() => go(1)} />
          </ToolbarGroup>
        </View>
        {unavailable ? (
          <EmptyState mascot="confused" title={t('calendar.unavailable')} hint={t('calendar.unavailableHint')} />
        ) : view === 'month' ? (
          <MonthGrid cursor={cursor} days={days} cals={cals} now={now} onOpen={(ev) => setSheet({ ev })}
            onSlot={(d) => newEvent(d, 9 * 60)} onDay={(d) => { setCursor(sod(d).getTime()); setView('day'); }} />
        ) : (
          <TimeGrid key={view} dates={view === 'day' ? [sod(cursor)] : [0, 1, 2, 3, 4, 5, 6].map((i) => addDays(startOfWeek(cursor), i))}
            days={days} cals={cals} now={now} onOpen={(ev) => setSheet({ ev })} onSlot={newEvent}
            onDay={(d) => { setCursor(sod(d).getTime()); setView('day'); }} />
        )}
      </View>
      {sheet && sheet.ev ? <EventSheet ev={sheet.ev} isNew={sheet.isNew} cals={eventCals} onClose={() => setSheet(null)} /> : null}
      {sheet && (sheet.newCal || sheet.cal) ? <CalendarSheet cal={sheet.cal} onClose={() => setSheet(null)} /> : null}
      <Layers />
    </View>
  );
}

// ---- sidebar: calendars by account, and a month to jump around ----

function Sidebar({ cals, cursor, days, onPick, onNew, onEdit }) {
  const th = useTheme();
  const groups = [];
  for (const c of cals) {
    const key = c.acct || '';
    let g = groups.find((x) => x.key === key);
    if (!g) { g = { key, name: key ? c.acctName || key : t('calendar.onThisComputer'), list: [] }; groups.push(g); }
    g.list.push(c);
  }
  groups.sort((a, b) => (a.key === '' ? -1 : b.key === '' ? 1 : a.name.localeCompare(b.name)));
  const marks = useMemo(() => new Set(days.keys()), [days]);
  return (
    <View style={{ width: 216, backgroundColor: th.sidebarBg }}>
      <Hairline edge="end" />
      <ScrollView style={{ flex: 1 }}>
        <View style={{ paddingHorizontal: 10, paddingTop: 12, paddingBottom: 10, gap: 2 }}>
          {groups.map((g) => (
            <View key={g.key} style={{ gap: 2, marginBottom: 14 }}>
              <Text style={{ fontSize: 11, fontWeight: 600, color: th.textDim, marginHorizontal: 8, marginBottom: 2, whiteSpace: 'nowrap', overflow: 'hidden' }}>{g.name}</Text>
              {g.list.map((c) => <CalRow key={c.id} cal={c} onEdit={onEdit} />)}
            </View>
          ))}
          {!cals.length ? <Text style={{ fontSize: 12, color: th.textDim, margin: 6 }}>{t('calendar.noCalendars')}</Text> : null}
          {/* synced calendars (CalDAV, a provider): Mail's Add Account sheet, in Calendars & Contacts mode */}
          <Button small plain icon="⊕" label={t('calendar.addAccount')} onClick={() => openDavSetup()} style={{ alignSelf: 'flex-start' }} />
        </View>
      </ScrollView>
      <View style={{ paddingHorizontal: 10, paddingTop: 12, gap: 6, alignItems: 'center' }}>
        <Hairline edge="top" />
        <MiniMonth key={`${cursor.getFullYear()}-${cursor.getMonth()}`} value={cursor} onPick={onPick} marks={marks} cell={26} />
      </View>
      <View style={{ height: 40, paddingHorizontal: 10, justifyContent: 'center' }}>
        <Hairline edge="top" />
        <Button small plain icon="⊕" label={t('calendar.newCalendar')} onClick={onNew} style={{ alignSelf: 'flex-start' }} />
      </View>
    </View>
  );
}

function CalRow({ cal, onEdit }) {
  const th = useTheme();
  const ref = useRef(null);
  const [menu, setMenu] = useState(false);
  const [hover, setHover] = useState(false);
  const toggle = () => calendars.update(cal.id, { hidden: !cal.hidden }).catch(() => {});
  return (
    <View ref={ref} onClick={toggle} onContextMenu={() => setMenu(true)} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 28, paddingHorizontal: 8, borderRadius: 6, flexDirection: 'row', alignItems: 'center', gap: 8, backgroundColor: hover ? th.hover : 'transparent' }}>
      <ColorCheck on={!cal.hidden} color={cal.color || '#007aff'} />
      <Text style={{ flex: 1, fontSize: 13, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{cal.name}</Text>
      {menu && (
        <Popover anchor={ref} width={200} onClose={() => setMenu(false)}>
          <MenuList onDone={() => setMenu(false)} items={[
            { label: t('calendar.editCalendar'), onClick: () => onEdit(cal) },
            { label: cal.hidden ? t('calendar.show') : t('calendar.hide'), onClick: toggle },
            '-',
            { label: t('calendar.deleteCalendar'), destructive: true, disabled: cal.kind !== 'local', onClick: () => calendars.remove(cal.id).catch(() => {}) },
          ]} />
        </Popover>
      )}
    </View>
  );
}

// ---- day and week: the hour grid ----

function useWidth(ref, initial = 100) {
  const [w, setW] = useState(initial);
  useEffect(() => {
    const id = setTimeout(() => { if (ref.current) { const nw = lord.getLayout(ref.current.id)[2]; if (nw && Math.abs(nw - w) > 0.5) setW(nw); } }, 20);
    return () => clearTimeout(id);
  });
  return w;
}

function TimeGrid({ dates, days, cals, now, onOpen, onSlot, onDay }) {
  const th = useTheme();
  const scroll = useRef(null);
  const colRef = useRef(null);
  const colW = useWidth(colRef, 120);
  const rtl = direction() === 'rtl';
  const nowD = new Date(now);
  useEffect(() => { const id = setTimeout(() => scroll.current?.scrollTo(8 * HOUR - 16), 60); return () => clearTimeout(id); }, []);
  const sep = (key) => <View key={key} style={{ position: 'absolute', top: 0, bottom: 0, [rtl ? 'right' : 'left']: 0, width: 1, backgroundColor: th.separator }} />;
  const allDay = dates.map((d) => (days.get(ymd(d)) || []).filter((e) => e.allDay));
  const maxAll = Math.max(1, ...allDay.map((l) => l.length));
  const single = dates.length === 1;
  const stacked = !single && colW < 80;          // narrow columns: weekday above the day number
  return (
    <View style={{ flex: 1 }}>
      {/* day headers */}
      <View style={{ flexDirection: 'row', height: single ? 32 : stacked ? 46 : 36 }}>
        <Hairline edge="bottom" />
        <View style={{ width: GUTTER }} />
        {dates.map((d) => {
          const today = sameDay(d, nowD);
          return (
            <View key={ymd(d)} onClick={() => !single && onDay(d)}
              style={{ flex: 1, flexDirection: stacked ? 'column' : 'row', alignItems: 'center', justifyContent: 'center', gap: stacked ? 1 : 5, overflow: 'hidden' }}>
              <Text style={{ fontSize: stacked ? 10 : 12, fontWeight: stacked ? 600 : 400, color: today ? RED : th.textDim, whiteSpace: 'nowrap' }}>{weekdayName(d.getDay())}</Text>
              <View style={{ minWidth: 24, height: 24, paddingHorizontal: 4, borderRadius: 12, alignItems: 'center', justifyContent: 'center', backgroundColor: today ? RED : 'transparent' }}>
                <Text style={{ fontSize: 15, fontWeight: today ? 600 : 500, color: today ? '#fff' : th.text }}>{String(d.getDate())}</Text>
              </View>
            </View>
          );
        })}
      </View>
      {/* all-day row */}
      <View style={{ flexDirection: 'row', minHeight: 30, maxHeight: 30 + 20 * Math.min(maxAll, 4) }}>
        <Hairline edge="bottom" />
        <View style={{ width: GUTTER, justifyContent: 'center', paddingVertical: 3 }}>
          <Text style={{ fontSize: 10, lineHeight: 12, color: th.textDim, textAlign: 'end', marginHorizontal: 8 }}>{t('calendar.allDay')}</Text>
        </View>
        {dates.map((d, i) => (
          <View key={ymd(d)} onClick={() => onSlot(d, null)} style={{ flex: 1, padding: 2, gap: 2, overflow: 'hidden' }}>
            {sep('s')}
            {allDay[i].slice(0, 4).map((ev) => <AllDayChip key={`${ev.cal}${ev.uid}${ev.start}`} ev={ev} color={colorOf(ev, cals)} onOpen={onOpen} />)}
            {allDay[i].length > 4 ? <Text style={{ fontSize: 10, color: th.textDim, marginHorizontal: 4 }}>{t('calendar.more', { count: allDay[i].length - 4 })}</Text> : null}
          </View>
        ))}
      </View>
      {/* hours */}
      <ScrollView scrollRef={scroll} style={{ flex: 1 }}>
        <View style={{ flexDirection: 'row', height: 24 * HOUR + 8 }}>
          <View style={{ width: GUTTER }}>
            {Array.from({ length: 23 }, (_, i) => i + 1).map((h) => (
              <Text key={h} style={{ position: 'absolute', top: h * HOUR - 7, left: 0, right: 0, paddingHorizontal: 8, fontSize: 10, color: th.textDim, textAlign: 'end', whiteSpace: 'nowrap' }}>
                {formatTime(new Date(2000, 0, 3, h, 0))}
              </Text>
            ))}
          </View>
          <View style={{ flex: 1, flexDirection: 'row' }}>
            <View style={{ position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, pointerEvents: 'none' }}>
              {Array.from({ length: 24 }, (_, h) => (
                <View key={h} style={{ position: 'absolute', left: 0, right: 0, top: h * HOUR, height: 1, backgroundColor: th.separator }} />
              ))}
            </View>
            {dates.map((d, i) => {
              const segs = layoutDay(d, days.get(ymd(d)) || []);
              const today = sameDay(d, nowD);
              return (
                <View key={ymd(d)} ref={i === 0 ? colRef : undefined} style={{ flex: 1 }}
                  onClick={(e) => { const y = e.local(e.currentTarget).y; onSlot(d, Math.max(0, Math.min(23 * 60 + 30, Math.floor(y / (HOUR / 2)) * 30))); }}>
                  {sep('s')}
                  {segs.map((s) => {
                    const w = (colW - 4) / s.cols;
                    const left = rtl ? colW - 2 - (s.col + 1) * w : 2 + s.col * w;
                    return <EventBlock key={`${s.ev.cal}${s.ev.uid}${s.ev.start}`} seg={s} left={left} width={w - 1} color={colorOf(s.ev, cals)} onOpen={onOpen} />;
                  })}
                  {today ? (
                    <View style={{ position: 'absolute', left: 0, right: 0, top: (minutesOf(nowD) * HOUR) / 60 - 1, height: 2, backgroundColor: RED, pointerEvents: 'none' }}>
                      <View style={{ position: 'absolute', [rtl ? 'right' : 'left']: -4, top: -3, width: 8, height: 8, borderRadius: 4, backgroundColor: RED }} />
                    </View>
                  ) : null}
                </View>
              );
            })}
          </View>
        </View>
      </ScrollView>
    </View>
  );
}

function EventBlock({ seg, left, width, color, onOpen }) {
  const th = useTheme();
  const ev = seg.ev;
  const h = Math.max(18, ((seg.bottom - seg.top) * HOUR) / 60 - 2);
  const rtl = direction() === 'rtl';
  const dark = th.mode === 'dark';
  const oneLine = h < 34 || width < 72;           // no broken words in narrow columns
  return (
    <View onClick={(e) => { e.stopPropagation(); onOpen(ev); }}
      style={{ position: 'absolute', top: (seg.top * HOUR) / 60 + 1, left, width: Math.max(8, width), height: h, borderRadius: 4, overflow: 'hidden',
        backgroundColor: tint(color, dark ? '4d' : '2e'), paddingVertical: 2, [rtl ? 'paddingRight' : 'paddingLeft']: 7, [rtl ? 'paddingLeft' : 'paddingRight']: 4 }}>
      <View style={{ position: 'absolute', top: 0, bottom: 0, [rtl ? 'right' : 'left']: 0, width: 3, backgroundColor: color }} />
      {/* the event's own text keeps its direction, so a clipped title loses its end, not its start */}
      <Text style={{ fontSize: 11, fontWeight: 600, color: th.text, whiteSpace: oneLine ? 'nowrap' : undefined, overflow: 'hidden', direction: textDirection(ev.title || '') }}>
        {ev.title || t('calendar.untitled')}
      </Text>
      {h >= 34 ? <Text style={{ fontSize: 10, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden', direction: textDirection(ev.loc || ev.title || '') }}>{ev.loc ? `${formatTime(new Date(ev.start))} · ${ev.loc}` : formatTime(new Date(ev.start))}</Text> : null}
    </View>
  );
}

function AllDayChip({ ev, color, onOpen }) {
  return (
    <View onClick={(e) => { e.stopPropagation(); onOpen(ev); }} style={{ height: 18, borderRadius: 4, paddingHorizontal: 6, justifyContent: 'center', backgroundColor: color }}>
      <Text style={{ fontSize: 11, fontWeight: 600, color: '#fff', whiteSpace: 'nowrap', overflow: 'hidden', direction: textDirection(ev.title || '') }}>{ev.title || t('calendar.untitled')}</Text>
    </View>
  );
}

// ---- month: 6 × 7 days ----

function MonthGrid({ cursor, days, cals, now, onOpen, onSlot, onDay }) {
  const th = useTheme();
  const box = useRef(null);
  const [cellH, setCellH] = useState(90);
  useEffect(() => {
    const id = setTimeout(() => { if (box.current) { const h = lord.getLayout(box.current.id)[3] / 6; if (h && Math.abs(h - cellH) > 0.5) setCellH(h); } }, 20);
    return () => clearTimeout(id);
  });
  const nowD = new Date(now);
  const first = startOfWeek(new Date(cursor.getFullYear(), cursor.getMonth(), 1));
  const fit = Math.max(1, Math.floor((cellH - 28) / 18));
  const rtl = direction() === 'rtl';
  const rows = [];
  for (let r = 0; r < 6; r++) {
    const cells = [];
    for (let c = 0; c < 7; c++) {
      const d = addDays(first, r * 7 + c);
      const list = days.get(ymd(d)) || [];
      const today = sameDay(d, nowD), inMonth = d.getMonth() === cursor.getMonth();
      const shown = list.length > fit ? list.slice(0, fit - 1) : list;
      cells.push(
        <View key={c} onClick={() => onSlot(d)} style={{ flex: 1, padding: 3, gap: 2, overflow: 'hidden',
          backgroundColor: inMonth ? 'transparent' : th.mode === 'dark' ? 'rgba(255,255,255,0.02)' : 'rgba(0,0,0,0.025)' }}>
          <Hairline edge="top" />
          {c > 0 ? <View style={{ position: 'absolute', top: 0, bottom: 0, [rtl ? 'right' : 'left']: 0, width: 1, backgroundColor: th.separator }} /> : null}
          <View style={{ flexDirection: 'row', justifyContent: 'flex-end' }}>
            <View onClick={(e) => { e.stopPropagation(); onDay(d); }} style={{ minWidth: 22, height: 22, paddingHorizontal: 4, borderRadius: 11, alignItems: 'center', justifyContent: 'center',
              backgroundColor: today ? RED : 'transparent' }}>
              <Text style={{ fontSize: 12, fontWeight: today ? 700 : 500, color: today ? '#fff' : inMonth ? th.text : th.textDim }}>
                {String(d.getDate())}
              </Text>
            </View>
          </View>
          {shown.map((ev) => <MonthChip key={`${ev.cal}${ev.uid}${ev.start}`} ev={ev} color={colorOf(ev, cals)} onOpen={onOpen} />)}
          {list.length > shown.length ? (
            <Text onClick={(e) => { e.stopPropagation(); onDay(d); }} style={{ fontSize: 10, fontWeight: 600, color: th.textDim, marginHorizontal: 4 }}>
              {t('calendar.more', { count: list.length - shown.length })}
            </Text>
          ) : null}
        </View>,
      );
    }
    rows.push(<View key={r} style={{ flex: 1, flexDirection: 'row' }}>{cells}</View>);
  }
  return (
    <View style={{ flex: 1 }}>
      <View style={{ flexDirection: 'row', height: 26, alignItems: 'center' }}>
        {[1, 2, 3, 4, 5, 6, 0].map((wd) => (
          <Text key={wd} style={{ flex: 1, fontSize: 11, fontWeight: 600, color: th.textDim, textAlign: 'end', paddingHorizontal: 8 }}>{weekdayName(wd)}</Text>
        ))}
      </View>
      <View ref={box} style={{ flex: 1 }}>{rows}</View>
    </View>
  );
}

function MonthChip({ ev, color, onOpen }) {
  const th = useTheme();
  if (ev.allDay) return <AllDayChip ev={ev} color={color} onOpen={onOpen} />;
  return (
    <View onClick={(e) => { e.stopPropagation(); onOpen(ev); }} style={{ height: 16, flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 3 }}>
      <View style={{ width: 7, height: 7, borderRadius: 4, backgroundColor: color }} />
      <Text style={{ flex: 1, fontSize: 11, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden', direction: textDirection(ev.title || '') }}>{ev.title || t('calendar.untitled')}</Text>
      <Text style={{ fontSize: 10, color: th.textDim, whiteSpace: 'nowrap' }}>{formatTime(new Date(ev.start))}</Text>
    </View>
  );
}

