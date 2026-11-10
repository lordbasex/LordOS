// ui.jsx — small widgets shared by Calendar, Contacts and Reminders (each app keeps its own copy,
// since apps only import from '@lordos/sdk'): sheets, drop-down choices, a multi-line field, date
// and time fields with a month picker, and color swatches. Reminders' popovers are the SDK's (0.8.8.8).
import {
  View, Text, useTheme, direction, ScrollView, Button, Hairline, takeTextFocus, releaseTextFocus, Popover,
  weekdayName, formatTime, formatDate, t,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';

export const RED = '#ff3b30';
export const COLORS = ['#ff3b30', '#ff9500', '#ffcc00', '#34c759', '#30b0c7', '#007aff', '#af52de', '#a2845e'];
const pad = (n) => String(n).padStart(2, '0');

// ---- dates ----

export const DAY = 86400000;
export const sod = (d) => { const x = new Date(d); x.setHours(0, 0, 0, 0); return x; };
export const addDays = (d, n) => { const x = new Date(d); x.setDate(x.getDate() + n); return x; };
export const addMonths = (d, n) => new Date(d.getFullYear(), d.getMonth() + n, 1);
export const sameDay = (a, b) => a.getFullYear() === b.getFullYear() && a.getMonth() === b.getMonth() && a.getDate() === b.getDate();
export const startOfWeek = (d) => { const x = sod(d); return addDays(x, -((x.getDay() + 6) % 7)); };   // weeks start on Monday
export const ymd = (d) => `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
export const parseYmd = (s) => { const m = /^(\d{4})-(\d{2})-(\d{2})/.exec(s || ''); return m ? new Date(+m[1], +m[2] - 1, +m[3]) : null; };
export const minutesOf = (d) => d.getHours() * 60 + d.getMinutes();
export const atMinutes = (day, min) => { const x = sod(day); x.setHours(Math.floor(min / 60), min % 60, 0, 0); return x; };
export const monthName = (m) => t(`calendar.month.${m}`);
export const monthYear = (d) => t('calendar.monthYear', { month: monthName(d.getMonth()), year: d.getFullYear() });

// ---- popovers: the SDK's (in the popover layer above the window, with the keys built in: the focus moves in
// when it opens from the keyboard, Escape closes it and the focus goes back) ----

export { Popover };

// A menu inside a popover: items = [{ label, onClick, checked, destructive, color, disabled } | '-']
export function MenuList({ items, onDone }) {
  return items.map((it, i) => (it === '-' ? <Separator key={`s${i}`} /> : <MenuItem key={`${i}${it.label}`} it={it} onDone={onDone} />));
}

function Separator() {
  const th = useTheme();
  return <View style={{ height: 1, marginVertical: 4, marginHorizontal: 6, backgroundColor: th.separator }} />;
}

function MenuItem({ it, onDone }) {
  const th = useTheme();
  const [hover, setHover] = useState(false);
  const on = hover && !it.disabled;
  return (
    <View onClick={() => { if (it.disabled) return; onDone?.(); it.onClick?.(); }} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 26, paddingHorizontal: 8, borderRadius: 5, flexDirection: 'row', alignItems: 'center', gap: 6,
        backgroundColor: on ? th.accent : 'transparent', opacity: it.disabled ? 0.4 : 1 }}>
      <Text style={{ width: 12, fontSize: 11, color: on ? '#fff' : th.text }}>{it.checked ? '✓' : ''}</Text>
      {it.color ? <View style={{ width: 10, height: 10, borderRadius: 5, backgroundColor: it.color }} /> : null}
      <Text style={{ flex: 1, fontSize: 13, whiteSpace: 'nowrap', overflow: 'hidden', color: on ? '#fff' : it.destructive ? RED : th.text }}>{it.label}</Text>
    </View>
  );
}

// ---- sheet: a modal box centered on the window ----

export function Sheet({ title, onClose, children, footer, width = 440 }) {
  const th = useTheme();
  return (
    <View onMouseDown={(e) => e.stopPropagation?.()} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, zIndex: 100,
      alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.3)' }}>
      <View style={{ width, maxWidth: '94%', maxHeight: '94%', borderRadius: 12, backgroundColor: th.windowBg, border: `1px solid ${th.separator}`,
        boxShadow: '0 18px 50px rgba(0,0,0,0.45)', overflow: 'hidden' }}>
        {title ? (
          <View style={{ paddingHorizontal: 20, paddingTop: 18, paddingBottom: 4 }}>
            <Text style={{ fontSize: 15, fontWeight: 600, color: th.text }}>{title}</Text>
          </View>
        ) : null}
        <View style={{ paddingHorizontal: 20, paddingVertical: 12, gap: 10, flexShrink: 1 }}>{children}</View>
        {footer ? (
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 20, paddingVertical: 14 }}>
            <Hairline edge="top" />
            {footer}
          </View>
        ) : null}
      </View>
    </View>
  );
}

// label | control row for sheets and inspectors
export function Field({ label, children, width = 86, top }) {
  const th = useTheme();
  return (
    <View style={{ flexDirection: 'row', alignItems: top ? 'flex-start' : 'center', gap: 10, minHeight: 28 }}>
      <Text style={{ width, fontSize: 12, color: th.textDim, textAlign: 'end', marginTop: top ? 6 : 0 }}>{label}</Text>
      <View style={{ flex: 1, flexDirection: 'row', alignItems: 'center', gap: 8, flexWrap: 'wrap' }}>{children}</View>
    </View>
  );
}

// ---- drop-down choice: options = [[value, label, color?]] ----

export function Choice({ value, options, onChange, width = 180, height }) {
  const th = useTheme();
  const ref = useRef(null);
  const scroll = useRef(null);
  const [open, setOpen] = useState(false);
  const cur = options.find((o) => o[0] === value);
  const long = height || options.length > 12;
  useEffect(() => {
    if (!open || !long) return undefined;
    const i = options.findIndex((o) => o[0] === value);
    const id = setTimeout(() => scroll.current?.scrollTo(Math.max(0, i * 26 - 100)), 40);
    return () => clearTimeout(id);
  }, [open]);
  const list = options.map(([v, label, color]) => ({ label, color, checked: v === value, onClick: () => onChange(v) }));
  return (
    <View ref={ref} style={{ width }}>
      <View onClick={() => setOpen(!open)} style={{ height: 26, paddingHorizontal: 9, borderRadius: 6, flexDirection: 'row', alignItems: 'center', gap: 6,
        backgroundColor: th.control }}>
        {cur && cur[2] ? <View style={{ width: 10, height: 10, borderRadius: 5, backgroundColor: cur[2] }} /> : null}
        <Text style={{ flex: 1, fontSize: 13, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{cur ? cur[1] : ''}</Text>
        <Text style={{ fontSize: 9, color: th.textDim }}>▼</Text>
      </View>
      {open && (
        <Popover anchor={ref} width={Math.max(width, 170)} onClose={() => setOpen(false)}>
          {long ? (
            <View style={{ height: height || 260 }}>
              <ScrollView scrollRef={scroll} style={{ flex: 1 }}><MenuList items={list} onDone={() => setOpen(false)} /></ScrollView>
            </View>
          ) : <MenuList items={list} onDone={() => setOpen(false)} />}
        </Popover>
      )}
    </View>
  );
}

// ---- multi-line text field ----

export function TextArea({ value, onChange, placeholder, minHeight = 64 }) {
  const th = useTheme();
  const token = useRef({}).current;
  const [focus, setFocus] = useState(false);
  const [blink, setBlink] = useState(true);
  const latest = useRef();
  latest.current = { value: value || '', onChange };
  useEffect(() => () => releaseTextFocus(token), []);
  useEffect(() => {
    if (!focus) return undefined;
    const id = setInterval(() => setBlink((b) => !b), 530);
    return () => clearInterval(id);
  }, [focus]);
  const onKey = (e) => {
    const { value: v, onChange: set } = latest.current;
    if ((e.ctrl || e.meta) && e.base === 'v') { try { set(v + lord.clipboard.get()); } catch (err) { /* no clipboard */ } return; }
    if (e.key === 'Backspace') set([...v].slice(0, -1).join(''));
    else if (e.key === 'Enter') set(`${v}\n`);
    else if (e.key === 'Escape') { releaseTextFocus(token); setFocus(false); }
    else if (e.char && !e.ctrl && !e.meta) set(v + e.char);
    setBlink(true);
  };
  const caret = focus && blink ? '▏' : '';
  return (
    <View onMouseDown={() => { takeTextFocus(token, onKey, () => setFocus(false)); setFocus(true); }}
      style={{ flex: 1, minHeight, padding: 7, borderRadius: 7, backgroundColor: th.inputBg, border: `1px solid ${focus ? th.accent : th.separator}` }}>
      {value ? <Text style={{ fontSize: 13, color: th.text, lineHeight: 18 }}>{`${value}${caret}`}</Text>
        : <Text style={{ fontSize: 13, color: th.textDim }}>{focus ? caret || ' ' : placeholder || ''}</Text>}
    </View>
  );
}

// ---- month grid picker ----

export function MiniMonth({ value, onPick, marks, month: ctlMonth, onMonth, cell = 26, today = new Date() }) {
  const th = useTheme();
  const [own, setOwn] = useState(() => new Date((value || today).getFullYear(), (value || today).getMonth(), 1));
  const month = ctlMonth || own;
  const setMonth = (m) => (onMonth ? onMonth(m) : setOwn(m));
  const rtl = direction() === 'rtl';
  const first = startOfWeek(month);
  const rows = [];
  for (let r = 0; r < 6; r++) {
    const cells = [];
    for (let c = 0; c < 7; c++) {
      const d = addDays(first, r * 7 + c);
      const sel = value && sameDay(d, value), isToday = sameDay(d, today), inMonth = d.getMonth() === month.getMonth();
      cells.push(
        <View key={c} onClick={() => onPick(d)} style={{ width: cell, height: cell, alignItems: 'center', justifyContent: 'center' }}>
          <View style={{ width: cell - 4, height: cell - 4, borderRadius: (cell - 4) / 2, alignItems: 'center', justifyContent: 'center',
            backgroundColor: sel ? (isToday ? RED : th.accent) : 'transparent' }}>
            <Text style={{ fontSize: 11, fontWeight: isToday || sel ? 600 : 400,
              color: sel ? '#fff' : isToday ? RED : inMonth ? th.text : th.textDim }}>{String(d.getDate())}</Text>
          </View>
          {marks && marks.has(ymd(d)) && !sel ? <View style={{ position: 'absolute', bottom: 1, width: 3, height: 3, borderRadius: 2, backgroundColor: th.textDim }} /> : null}
        </View>,
      );
    }
    rows.push(<View key={r} style={{ flexDirection: 'row' }}>{cells}</View>);
  }
  return (
    <View style={{ width: cell * 7, gap: 2 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', height: 24 }}>
        <Text style={{ flex: 1, fontSize: 12, fontWeight: 600, color: th.text, marginHorizontal: 4, whiteSpace: 'nowrap', overflow: 'hidden' }}>{monthYear(month)}</Text>
        <NavArrow glyph={rtl ? '›' : '‹'} onClick={() => setMonth(addMonths(month, -1))} />
        <NavArrow glyph="●" small onClick={() => { setMonth(new Date(today.getFullYear(), today.getMonth(), 1)); }} />
        <NavArrow glyph={rtl ? '‹' : '›'} onClick={() => setMonth(addMonths(month, 1))} />
      </View>
      <View style={{ flexDirection: 'row' }}>
        {[1, 2, 3, 4, 5, 6, 0].map((wd) => (
          <Text key={wd} style={{ width: cell, textAlign: 'center', fontSize: 10, fontWeight: 600, color: th.textDim }}>{[...weekdayName(wd)][0] || ''}</Text>
        ))}
      </View>
      {rows}
    </View>
  );
}

function NavArrow({ glyph, onClick, small }) {
  const th = useTheme();
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: 20, height: 20, borderRadius: 5, alignItems: 'center', justifyContent: 'center', backgroundColor: hover ? th.hover : 'transparent' }}>
      <Text style={{ fontSize: small ? 7 : 15, color: th.textDim }}>{glyph}</Text>
    </View>
  );
}

// ---- date and time fields ----

export function DateField({ value, onChange, width = 120 }) {
  const th = useTheme();
  const ref = useRef(null);
  const [open, setOpen] = useState(false);
  return (
    <View ref={ref}>
      <View onClick={() => setOpen(!open)} style={{ width, height: 26, paddingHorizontal: 9, borderRadius: 6, justifyContent: 'center', backgroundColor: th.control }}>
        <Text style={{ fontSize: 13, color: th.text, whiteSpace: 'nowrap' }}>{value ? formatDate(value, 'numeric') : ''}</Text>
      </View>
      {open && (
        <Popover anchor={ref} width={7 * 28 + 16} padding={8} onClose={() => setOpen(false)}>
          <MiniMonth value={value} cell={28} onPick={(d) => { setOpen(false); onChange(d); }} />
        </Popover>
      )}
    </View>
  );
}

// minutes since midnight, every 15 minutes (and the current value if it is not on the grid)
export function TimeField({ value, onChange, width = 100 }) {
  const opts = [];
  for (let m = 0; m < 1440; m += 15) opts.push(m);
  if (!opts.includes(value)) { opts.push(value); opts.sort((a, b) => a - b); }
  return <Choice width={width} value={value} onChange={onChange} options={opts.map((m) => [m, formatTime(atMinutes(new Date(2000, 0, 3), m))])} />;
}

// ---- colors ----

export function ColorSwatches({ value, onChange, colors = COLORS }) {
  const th = useTheme();
  return (
    <View style={{ flexDirection: 'row', gap: 8, flexWrap: 'wrap' }}>
      {colors.map((c) => (
        <View key={c} onClick={() => onChange(c)} style={{ width: 24, height: 24, borderRadius: 12, alignItems: 'center', justifyContent: 'center',
          border: `2px solid ${value === c ? th.text : 'transparent'}` }}>
          <View style={{ width: 18, height: 18, borderRadius: 9, backgroundColor: c }} />
        </View>
      ))}
    </View>
  );
}

// square check box filled with a color when on
export function ColorCheck({ on, color, onClick, size = 14 }) {
  return (
    <View onClick={onClick} style={{ width: size, height: size, borderRadius: 4, alignItems: 'center', justifyContent: 'center',
      border: `1.5px solid ${color}`, backgroundColor: on ? color : 'transparent' }}>
      {on ? <Text style={{ fontSize: size - 4, fontWeight: 700, color: '#fff' }}>✓</Text> : null}
    </View>
  );
}

// a promise-returning loader that re-runs when deps change: { data, error }
export function useLoad(fn, deps) {
  const [st, setSt] = useState({ data: null, error: null });
  useEffect(() => {
    let live = true;
    Promise.resolve().then(fn).then((data) => { if (live) setSt({ data, error: null }); }).catch((error) => { if (live) setSt((s) => ({ data: s.data, error })); });
    return () => { live = false; };
  }, deps);
  return st;
}

export function useNow(ms = 30000) {
  const [now, setNow] = useState(Date.now());
  useEffect(() => { const id = setInterval(() => setNow(Date.now()), ms); return () => clearInterval(id); }, [ms]);
  return now;
}
