// Reminders — to-do lists kept by lord-maild (local, or synced with CalDAV as VTODO): see
// docs/MAIL.md. Smart lists (Today, Scheduled, All, Flagged, Completed) are filters over
// reminders.items(); their alarms ring with the app closed (sdk/calendar.js, startReminders).
import {
  View, Text, useTheme, useLocale, useKeyboard, useWindowState, usePim, reminders, calendars,
  Button, Toggle, Segmented, TextInput, ScrollView, Hairline, Toolbar, ToolbarButton, ToolbarSpacer, EmptyState, addListener,
  formatDate, formatTime, direction, t,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import {
  RED, COLORS, sod, addDays, ymd, parseYmd, minutesOf, atMinutes, Sheet, Field, Choice, TextArea, DateField, TimeField, ColorSwatches,
  Popover, MenuList, useLoad, useNow,
} from './ui.jsx';

// ---- icon: a white rounded square with three colored dots and lines ----

export function Icon({ size: s }) {
  const dots = ['#007aff', '#ff3b30', '#ff9500'];
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, backgroundColor: '#ffffff', border: '1px solid rgba(0,0,0,0.08)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, paddingHorizontal: s * 0.17, justifyContent: 'center', gap: s * 0.09 }}>
      {dots.map((c, i) => (
        <View key={c} style={{ flexDirection: 'row', alignItems: 'center', gap: s * 0.08 }}>
          <View style={{ width: s * 0.13, height: s * 0.13, borderRadius: s * 0.065, backgroundColor: c }} />
          <View style={{ width: s * (i === 2 ? 0.3 : 0.42), height: Math.max(1, s * 0.035), borderRadius: s * 0.02, backgroundColor: '#c7c7cc' }} />
        </View>
      ))}
    </View>
  );
}

const SMART = { today: '#007aff', scheduled: RED, all: '#5b5b60', flagged: '#ff9500', completed: '#8e8e93' };
const ORANGE = '#ff9500';

const dueDay = (it) => (it.due == null ? null : it.dueAllDay ? it.dueDate || ymd(new Date(it.due)) : ymd(new Date(it.due)));
const isOverdue = (it, now) => !it.done && it.due != null && (it.dueAllDay ? dueDay(it) < ymd(new Date(now)) : it.due < now);
const isToday = (it, now) => it.due != null && dueDay(it) <= ymd(new Date(now));
const prioMarks = (p) => (p === 1 ? '!!!' : p >= 2 && p <= 5 ? '!!' : p >= 6 ? '!' : '');

function matches(sel, it, now) {
  if (sel === 'today') return isToday(it, now);
  if (sel === 'scheduled') return it.due != null;
  if (sel === 'flagged') return !!it.flag;
  if (sel === 'completed') return !!it.done;
  if (sel === 'all') return true;
  return it.list === sel.slice(5);
}

function dueText(it) {
  if (it.due == null) return '';
  if (it.dueAllDay) { const d = parseYmd(it.dueDate) || new Date(it.due); return formatDate(d, 'short'); }
  const d = new Date(it.due);
  return t('reminders.dueAt', { date: formatDate(d, 'short'), time: formatTime(d) });
}

export default function RemindersApp() {
  const th = useTheme();
  useLocale();
  const rev = usePim();
  const now = useNow(60000);
  const [sel, setSel] = useWindowState('sel', 'today');
  const [showDone, setShowDone] = useWindowState('showDone', false);
  const [sheet, setSheet] = useState(null);           // { newList } | { list }
  const listsQ = useLoad(() => reminders.lists(), [rev]);
  const lists = listsQ.data || [];
  const ids = lists.map((l) => l.id).join('\n');
  const itemsQ = useLoad(() => (lists.length ? reminders.items(lists.map((l) => l.id)) : Promise.resolve([])), [rev, ids]);
  const items = itemsQ.data || [];
  const listOf = (id) => lists.find((l) => l.id === id);
  const curList = sel.startsWith('list:') ? listOf(sel.slice(5)) : null;
  useEffect(() => { if (sel.startsWith('list:') && listsQ.data && !curList) setSel('today'); }, [listsQ.data]);

  const counts = useMemo(() => {
    const c = { today: 0, scheduled: 0, all: 0, flagged: 0, completed: 0, lists: {} };
    for (const it of items) {
      if (it.done) { c.completed++; continue; }
      c.all++;
      if (it.due != null) c.scheduled++;
      if (isToday(it, now)) c.today++;
      if (it.flag) c.flagged++;
      c.lists[it.list] = (c.lists[it.list] || 0) + 1;
    }
    return c;
  }, [items, now]);

  const newRow = useRef(null);
  useKeyboard((e) => {
    if (sheet) { if (e.key === 'Escape') setSheet(null); return; }
    const k = String(e.base || e.key || '').toLowerCase();
    if ((e.meta || e.ctrl) && k === 'n') newRow.current?.();
  });

  const unavailable = listsQ.error && !listsQ.data;
  const title = curList ? curList.name : t(`reminders.smart.${sel}`);
  const color = curList ? curList.color || '#007aff' : SMART[sel] || th.text;

  return (
    <View style={{ flex: 1, flexDirection: 'row', backgroundColor: th.windowBg }}>
      <Sidebar sel={sel} onSel={setSel} lists={lists} counts={counts} now={now} onAdd={() => setSheet({ newList: true })} onEdit={(l) => setSheet({ list: l })} />
      <View style={{ flex: 1 }}>
        <Toolbar separator={false}>
          <ToolbarSpacer />
          {sel !== 'completed' ? (
            <ToolbarButton label={showDone ? t('reminders.hideCompleted') : t('reminders.showCompleted')} small onClick={() => setShowDone(!showDone)} active={showDone} />
          ) : null}
          <ToolbarButton icon="+" iconSize={20} title={t('reminders.newReminder')} disabled={unavailable || sel === 'completed'} onClick={() => newRow.current?.()} />
        </Toolbar>
        {unavailable ? (
          <EmptyState mascot="confused" title={t('reminders.unavailable')} hint={t('reminders.unavailableHint')} />
        ) : (
          <ListView key={sel} sel={sel} title={title} color={color} items={items} lists={lists} now={now} showDone={showDone || sel === 'completed'}
            newRowRef={newRow} loading={!itemsQ.data} />
        )}
      </View>
      {sheet ? <ListSheet list={sheet.list} onClose={() => setSheet(null)} onCreated={(id) => setSel(`list:${id}`)} /> : null}
    </View>
  );
}

// ---- sidebar: smart tiles and my lists ----

function Sidebar({ sel, onSel, lists, counts, now, onAdd, onEdit }) {
  const th = useTheme();
  const tiles = ['today', 'scheduled', 'all', 'flagged', 'completed'];
  const glyph = { today: String(new Date(now).getDate()), scheduled: '▦', all: '☰', flagged: '⚑', completed: '✓' };
  return (
    <View style={{ width: 236, backgroundColor: th.sidebarBg }}>
      <Hairline edge="end" />
      <ScrollView style={{ flex: 1 }}>
        <View style={{ paddingHorizontal: 10, paddingTop: 12, paddingBottom: 10, gap: 2 }}>
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8 }}>
            {tiles.map((k) => (
              <Tile key={k} label={t(`reminders.smart.${k}`)} color={SMART[k]} glyph={glyph[k]} count={k === 'completed' ? null : counts[k]}
                selected={sel === k} onClick={() => onSel(k)} />
            ))}
          </View>
          <Text style={{ fontSize: 11, fontWeight: 600, color: th.textDim, marginHorizontal: 8, marginTop: 18, marginBottom: 4 }}>{t('reminders.myLists')}</Text>
          {lists.map((l) => <ListRow key={l.id} list={l} count={counts.lists[l.id] || 0} selected={sel === `list:${l.id}`} onClick={() => onSel(`list:${l.id}`)} onEdit={onEdit} />)}
          {!lists.length ? <Text style={{ fontSize: 12, color: th.textDim, marginHorizontal: 8 }}>{t('reminders.noLists')}</Text> : null}
        </View>
      </ScrollView>
      <View style={{ height: 40, paddingHorizontal: 10, justifyContent: 'center' }}>
        <Hairline edge="top" />
        <Button small plain icon="⊕" label={t('reminders.addList')} onClick={onAdd} style={{ alignSelf: 'flex-start' }} />
      </View>
    </View>
  );
}

function Tile({ label, color, glyph, count, selected, onClick }) {
  const th = useTheme();
  const dark = th.mode === 'dark';
  return (
    <View onClick={onClick} style={{ width: 104, height: 64, borderRadius: 10, paddingHorizontal: 9, paddingTop: 8, paddingBottom: 7, justifyContent: 'space-between',
      backgroundColor: selected ? color : dark ? 'rgba(255,255,255,0.07)' : '#ffffff',
      border: `1px solid ${selected ? 'transparent' : dark ? 'rgba(255,255,255,0.06)' : 'rgba(0,0,0,0.06)'}`, boxShadow: dark || selected ? 'none' : '0 1px 1px rgba(0,0,0,0.06)' }}>
      <View style={{ flexDirection: 'row', alignItems: 'center' }}>
        <View style={{ width: 26, height: 26, borderRadius: 13, alignItems: 'center', justifyContent: 'center', backgroundColor: selected ? '#ffffff' : color }}>
          <Text style={{ fontSize: 12, fontWeight: 600, color: selected ? color : '#ffffff' }}>{glyph}</Text>
        </View>
        <View style={{ flex: 1 }} />
        {count != null ? <Text style={{ fontSize: 20, fontWeight: 700, color: selected ? '#fff' : th.text }}>{String(count)}</Text> : null}
      </View>
      <Text style={{ fontSize: 12, fontWeight: 600, color: selected ? '#fff' : th.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{label}</Text>
    </View>
  );
}

function ListRow({ list, count, selected, onClick, onEdit }) {
  const th = useTheme();
  const ref = useRef(null);
  const [menu, setMenu] = useState(false);
  const [hover, setHover] = useState(false);
  return (
    <View ref={ref} onClick={onClick} onContextMenu={() => setMenu(true)} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 32, paddingHorizontal: 8, borderRadius: 6, flexDirection: 'row', alignItems: 'center', gap: 8,
        backgroundColor: selected ? (th.mode === 'dark' ? 'rgba(255,255,255,0.14)' : 'rgba(0,0,0,0.09)') : hover ? th.hover : 'transparent' }}>
      <View style={{ width: 22, height: 22, borderRadius: 11, alignItems: 'center', justifyContent: 'center', backgroundColor: list.color || '#007aff' }}>
        <Text style={{ fontSize: 11, color: '#fff' }}>☰</Text>
      </View>
      <Text style={{ flex: 1, fontSize: 13, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{list.name}</Text>
      <Text style={{ fontSize: 12, color: th.textDim }}>{String(count)}</Text>
      {menu && (
        <Popover anchor={ref} width={200} onClose={() => setMenu(false)}>
          <MenuList onDone={() => setMenu(false)} items={[
            { label: t('reminders.editList'), onClick: () => onEdit(list) },
            '-',
            { label: t('reminders.deleteList'), destructive: true, disabled: list.kind !== 'local', onClick: () => calendars.remove(list.id).catch(() => {}) },
          ]} />
        </Popover>
      )}
    </View>
  );
}

// ---- the list of reminders ----

function sortItems(sel, list) {
  const open = list.filter((x) => !x.done), done = list.filter((x) => x.done);
  if (!sel.startsWith('list:')) open.sort((a, b) => (a.due ?? Infinity) - (b.due ?? Infinity));
  done.sort((a, b) => (b.doneAt || 0) - (a.doneAt || 0));
  // subtasks right after their parent
  const out = [], byParent = new Map(), uids = new Set(open.map((x) => x.uid));
  for (const x of open) if (x.parent && uids.has(x.parent)) { if (!byParent.has(x.parent)) byParent.set(x.parent, []); byParent.get(x.parent).push(x); }
  for (const x of open) {
    if (x.parent && uids.has(x.parent)) continue;
    out.push({ it: x, depth: 0 });
    for (const c of byParent.get(x.uid) || []) out.push({ it: c, depth: 1 });
  }
  for (const x of done) out.push({ it: x, depth: 0 });
  return out;
}

function ListView({ sel, title, color, items, lists, now, showDone, newRowRef, loading }) {
  const th = useTheme();
  const shown = items.filter((it) => matches(sel, it, now) && (showDone || !it.done));
  const openCount = shown.filter((x) => !x.done).length;
  const smart = !sel.startsWith('list:');
  const listOf = (id) => lists.find((l) => l.id === id);
  // sections: All → by list; Scheduled → by day; others → one section
  let sections;
  if (sel === 'all') sections = lists.map((l) => ({ key: l.id, title: l.name, color: l.color, rows: sortItems(sel, shown.filter((x) => x.list === l.id)) })).filter((s) => s.rows.length);
  else if (sel === 'scheduled') {
    const m = new Map();
    for (const x of shown) { const k = dueDay(x); if (!m.has(k)) m.set(k, []); m.get(k).push(x); }
    const today = ymd(new Date(now));
    sections = [...m.keys()].sort().map((k) => ({ key: k, title: k < today ? t('reminders.overdue') : k === today ? t('reminders.smart.today') : formatDate(parseYmd(k), 'long'),
      color: k < today ? RED : th.text, rows: sortItems(sel, m.get(k)) }));
    // merge the overdue days into one section
    const od = sections.filter((s) => s.key < today);
    if (od.length > 1) sections = [{ key: 'overdue', title: od[0].title, color: RED, rows: od.flatMap((s) => s.rows) }, ...sections.filter((s) => s.key >= today)];
  } else sections = [{ key: 'one', title: null, rows: sortItems(sel, shown) }];

  const target = sel.startsWith('list:') ? listOf(sel.slice(5)) : lists[0];
  const canAdd = sel !== 'completed';
  return (
    <View style={{ flex: 1 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 20, paddingBottom: 10 }}>
        <Text style={{ flex: 1, fontSize: 26, fontWeight: 700, color, whiteSpace: 'nowrap', overflow: 'hidden' }}>{title}</Text>
        <Text style={{ fontSize: 26, fontWeight: 600, color }}>{sel === 'completed' ? String(shown.length) : String(openCount)}</Text>
      </View>
      <ScrollView style={{ flex: 1 }}>
        <View style={{ paddingHorizontal: 12, paddingBottom: 20 }}>
          {sections.map((s) => (
            <View key={s.key} style={{ marginBottom: s.title ? 14 : 0 }}>
              {s.title ? <Text style={{ fontSize: 15, fontWeight: 600, color: s.color || th.text, marginHorizontal: 8, marginTop: 6, marginBottom: 2 }}>{s.title}</Text> : null}
              {s.rows.map(({ it, depth }) => (
                <ReminderRow key={`${it.list}/${it.uid}`} item={it} depth={depth} lists={lists} now={now} color={listOf(it.list)?.color || it.color || '#007aff'}
                  listName={smart && sel !== 'all' ? listOf(it.list)?.name : ''} />
              ))}
            </View>
          ))}
          {!shown.length && !loading && !canAdd ? <Text style={{ fontSize: 13, color: th.textDim, marginHorizontal: 8, marginVertical: 10 }}>{t('reminders.nothingCompleted')}</Text> : null}
          {!shown.length && !loading && canAdd && sel !== 'all' && !sel.startsWith('list:') ? (
            <Text style={{ fontSize: 13, color: th.textDim, marginHorizontal: 8, marginVertical: 8 }}>{t('reminders.empty')}</Text>
          ) : null}
          {canAdd ? <NewRow sel={sel} target={target} color={target?.color || color} registerFocus={newRowRef} /> : null}
        </View>
      </ScrollView>
    </View>
  );
}

function Circle({ done, color, onClick, size = 18 }) {
  return (
    <View onClick={onClick} style={{ width: size, height: size, borderRadius: size / 2, alignItems: 'center', justifyContent: 'center',
      border: `1.5px solid ${done ? color : '#a1a1a6'}` }}>
      {done ? <View style={{ width: size - 5, height: size - 5, borderRadius: (size - 5) / 2, backgroundColor: color }} /> : null}
    </View>
  );
}

function ReminderRow({ item, depth, color, lists, now, listName }) {
  const th = useTheme();
  const row = useRef(null), info = useRef(null);
  const [done, setDone] = useState(item.done);
  const [editing, setEditing] = useState(null);       // the title being typed
  const [hover, setHover] = useState(false);
  const [inspect, setInspect] = useState(false);
  useEffect(() => setDone(item.done), [item.done]);
  const latest = useRef();
  latest.current = { editing, item };
  const commit = () => {
    const { editing: v, item: it } = latest.current;
    setEditing(null);
    if (v != null && v.trim() && v !== it.title) reminders.save(it.list, { ...it, title: v.trim() }).catch(() => {});
  };
  // a click outside the row ends editing
  useEffect(() => {
    if (editing == null) return undefined;
    return addListener('pointerdown', (e) => {
      if (!row.current) return;
      const [x, y, w, h] = lord.getLayout(row.current.id);
      if (e.x < x || e.x > x + w || e.y < y || e.y > y + h) commit();
    });
  }, [editing != null]);
  const toggle = () => { setDone(!done); reminders.toggle(item).catch(() => setDone(item.done)); };
  const overdue = isOverdue(item, now);
  const marks = prioMarks(item.prio);
  const due = dueText(item);
  const sub = [due, listName].filter(Boolean);
  const rtl = direction() === 'rtl';
  return (
    <View ref={row} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flexDirection: 'row', alignItems: 'flex-start', gap: 10, paddingVertical: 8, paddingHorizontal: 8, marginHorizontal: depth ? 28 : 0 }}>
      <View style={{ paddingTop: 1 }}><Circle done={done} color={color} onClick={toggle} /></View>
      <View style={{ flex: 1, gap: 2 }}>
        {editing != null ? (
          <TextInput value={editing} onChange={setEditing} autoFocus onSubmit={commit} onCancel={() => setEditing(null)} style={{ height: 24 }} />
        ) : (
          <View onClick={() => setEditing(item.title || '')} style={{ flexDirection: 'row', gap: 4, minHeight: 20, alignItems: 'center' }}>
            {marks ? <Text style={{ fontSize: 13, fontWeight: 700, color }}>{marks}</Text> : null}
            <Text style={{ flex: 1, fontSize: 13, color: done ? th.textDim : th.text }}>{item.title || t('reminders.untitled')}</Text>
          </View>
        )}
        {item.notes ? <Text style={{ fontSize: 12, color: th.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{item.notes.split('\n')[0]}</Text> : null}
        {sub.length ? (
          <View style={{ flexDirection: 'row', gap: 6 }}>
            {due ? <Text style={{ fontSize: 12, color: overdue ? RED : th.textDim }}>{due}</Text> : null}
            {listName ? <Text style={{ fontSize: 12, color: th.textDim }}>{listName}</Text> : null}
          </View>
        ) : null}
      </View>
      {item.flag ? <Text style={{ fontSize: 13, color: ORANGE, paddingTop: 1 }}>⚑</Text> : null}
      <View ref={info} onClick={() => setInspect(true)} style={{ width: 20, height: 20, borderRadius: 10, alignItems: 'center', justifyContent: 'center',
        border: `1.2px solid ${hover || inspect ? th.accent : 'transparent'}` }}>
        <Text style={{ fontSize: 11, fontWeight: 700, color: hover || inspect ? th.accent : 'transparent' }}>i</Text>
      </View>
      {/* the separator starts under the text, as in Reminders */}
      <View style={{ position: 'absolute', bottom: 0, height: 1, [rtl ? 'right' : 'left']: 36, [rtl ? 'left' : 'right']: 0, backgroundColor: th.separator, pointerEvents: 'none' }} />
      {inspect ? <Inspector item={item} lists={lists} anchor={info} onClose={() => setInspect(false)} /> : null}
    </View>
  );
}

// ---- details popover ----

function Inspector({ item, lists, anchor, onClose }) {
  const th = useTheme();
  const initDay = item.due != null ? (item.dueAllDay ? parseYmd(item.dueDate) || sod(item.due) : sod(item.due)) : sod(Date.now());
  const [d, setD] = useState({
    title: item.title || '', notes: item.notes || '', prio: item.prio || 0, flag: !!item.flag, list: item.list,
    mode: item.due == null ? 'none' : item.dueAllDay ? 'date' : 'time', day: initDay,
    min: item.due != null && !item.dueAllDay ? minutesOf(new Date(item.due)) : 9 * 60,
  });
  const set = (p) => setD((x) => ({ ...x, ...p }));
  const save = async () => {
    const todo = { ...item, title: d.title.trim() || item.title, notes: d.notes, prio: d.prio, flag: d.flag };
    if (d.mode === 'none') { todo.due = null; todo.dueAllDay = false; todo.alarms = []; }
    else if (d.mode === 'date') { todo.due = sod(d.day).getTime(); todo.dueAllDay = true; todo.alarms = []; }
    else { todo.due = atMinutes(d.day, d.min).getTime(); todo.dueAllDay = false; todo.alarms = item.alarms && item.alarms.length ? item.alarms : [0]; }
    delete todo.dueDate;
    onClose();
    try { await reminders.save(d.list, todo, d.list !== item.list ? { from: item.list } : {}); } catch (e) { /* stays as it was */ }
  };
  const del = () => { onClose(); reminders.remove(item.list, item.uid).catch(() => {}); };
  return (
    <Popover anchor={anchor} width={340} padding={12} onClose={save}>
      <View style={{ gap: 9 }}>
        <TextInput value={d.title} onChange={(v) => set({ title: v })} size={15} onSubmit={save} />
        <TextArea value={d.notes} onChange={(v) => set({ notes: v })} placeholder={t('reminders.notes')} minHeight={48} />
        <Field width={70} label={t('reminders.due')}>
          <Segmented small value={d.mode} onChange={(v) => set({ mode: v })}
            options={[['none', t('reminders.dueNone')], ['date', t('reminders.dueDate')], ['time', t('reminders.dueTime')]]} />
        </Field>
        {d.mode !== 'none' ? (
          <Field width={70} label="">
            <DateField value={d.day} onChange={(v) => set({ day: sod(v) })} width={110} />
            {d.mode === 'time' ? <TimeField value={d.min} onChange={(v) => set({ min: v })} width={96} /> : null}
          </Field>
        ) : null}
        <Field width={70} label={t('reminders.priority')}>
          <Choice width={150} value={d.prio >= 6 ? 9 : d.prio >= 2 ? 5 : d.prio} onChange={(v) => set({ prio: v })}
            options={[[0, t('reminders.prio.none')], [9, t('reminders.prio.low')], [5, t('reminders.prio.medium')], [1, t('reminders.prio.high')]]} />
        </Field>
        <Field width={70} label={t('reminders.flagged')}><Toggle on={d.flag} onChange={(v) => set({ flag: v })} /></Field>
        <Field width={70} label={t('reminders.list')}>
          <Choice width={180} value={d.list} onChange={(v) => set({ list: v })} options={lists.map((l) => [l.id, l.name, l.color])} />
        </Field>
        <View style={{ flexDirection: 'row', gap: 8, marginTop: 4 }}>
          <Button small destructive label={t('reminders.delete')} onClick={del} />
          <View style={{ flex: 1 }} />
          <Button small primary label={t('reminders.done')} onClick={save} />
        </View>
        <Text style={{ fontSize: 10, color: th.textDim }}>{item.doneAt ? t('reminders.completedOn', { date: formatDate(new Date(item.doneAt), 'short') }) : ''}</Text>
      </View>
    </Popover>
  );
}

// ---- "New Reminder" row ----

function NewRow({ sel, target, color, registerFocus }) {
  const th = useTheme();
  const [text, setText] = useState('');
  const [active, setActive] = useState(false);
  const [error, setError] = useState('');
  registerFocus.current = () => setActive(true);
  const add = async (v) => {
    const title = String(v || '').trim();
    if (!title) { setActive(false); return; }
    setText('');
    try {
      let list = target ? target.id : null;
      if (!list) list = (await calendars.create({ name: t('reminders.defaultList'), color: COLORS[5], todos: true, events: false })).uid;
      const todo = { title, notes: '', prio: 0, flag: sel === 'flagged', done: false, alarms: [], due: null, dueAllDay: false };
      if (sel === 'today' || sel === 'scheduled') { todo.due = sod(Date.now()).getTime(); todo.dueAllDay = true; }
      await reminders.save(list, todo);
      setError('');
    } catch (e) { setError(t('reminders.saveFailed')); }
  };
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, minHeight: 36, paddingVertical: 6, paddingHorizontal: 8 }}>
      <View onClick={() => setActive(true)} style={{ width: 18, height: 18, borderRadius: 9, alignItems: 'center', justifyContent: 'center', backgroundColor: active ? 'transparent' : color }}>
        {active ? <View style={{ width: 18, height: 18, borderRadius: 9, border: '1.5px solid #a1a1a6' }} /> : <Text style={{ fontSize: 13, fontWeight: 600, color: '#fff' }}>+</Text>}
      </View>
      {active ? (
        <View style={{ flex: 1 }}>
          <TextInput value={text} onChange={setText} autoFocus placeholder={t('reminders.newReminder')} onSubmit={add} onCancel={() => { setText(''); setActive(false); }} style={{ height: 26 }} />
        </View>
      ) : (
        <Text onClick={() => setActive(true)} style={{ flex: 1, fontSize: 13, color: th.textDim }}>{t('reminders.newReminder')}</Text>
      )}
      {error ? <Text style={{ fontSize: 12, color: RED }}>{error}</Text> : null}
    </View>
  );
}

// ---- new list / edit list ----

function ListSheet({ list, onClose, onCreated }) {
  const th = useTheme();
  const [name, setName] = useState(list ? list.name : '');
  const [color, setColor] = useState(list ? list.color : COLORS[5]);
  const [error, setError] = useState('');
  const save = async () => {
    if (!name.trim()) return;
    try {
      if (list) await calendars.update(list.id, { name: name.trim(), color });
      else { const r = await calendars.create({ name: name.trim(), color, todos: true, events: false }); if (r && r.uid) onCreated(r.uid); }
      onClose();
    } catch (e) { setError(t('reminders.saveFailed')); }
  };
  return (
    <Sheet width={380} title={list ? t('reminders.editList') : t('reminders.newList')} footer={[
      <Text key="e" style={{ flex: 1, fontSize: 12, color: RED }}>{error}</Text>,
      <Button key="c" label={t('reminders.cancel')} onClick={onClose} />,
      <Button key="s" primary disabled={!name.trim()} label={list ? t('reminders.save') : t('reminders.create')} onClick={save} />,
    ]}>
      <View style={{ alignItems: 'center', marginVertical: 4 }}>
        <View style={{ width: 56, height: 56, borderRadius: 28, alignItems: 'center', justifyContent: 'center', backgroundColor: color }}>
          <Text style={{ fontSize: 26, color: '#fff' }}>☰</Text>
        </View>
      </View>
      <TextInput value={name} onChange={setName} placeholder={t('reminders.listName')} autoFocus onSubmit={save} onCancel={onClose} />
      <Text style={{ fontSize: 11, fontWeight: 600, color: th.textDim }}>{t('reminders.color')}</Text>
      <ColorSwatches value={color} onChange={setColor} />
    </Sheet>
  );
}
