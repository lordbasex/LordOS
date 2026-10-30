// Cleaner's small building blocks, plain SDK and theme colours. The app's final look waits for the maintainer's
// choice of design, so nothing here animates.
import { View, Text, Button, useTheme, typeScale, spacing } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';

export function Enter({ children, style }) { return <View style={style}>{children}</View>; }

export function Label({ children, size = typeScale.body, weight = 400, dim, mono, lines, style, align = 'start' }) {
  const th = useTheme();
  return (
    <Text style={{ fontSize: size, fontWeight: weight, color: dim ? th.textDim : th.text, textAlign: align,
      ...(mono ? { fontFamily: 'monospace' } : null), ...(lines ? { whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' } : null), ...style }}>{children}</Text>
  );
}

export function CountUp({ value, format }) { return <Label size={typeScale.title} weight={600}>{format(value)}</Label>; }

export function Glass({ children, style, onClick }) {
  const th = useTheme();
  return <View onClick={onClick} style={{ backgroundColor: th.control, borderRadius: th.groupRadius, ...style }}>{children}</View>;
}

export function Pill({ label, on, onClick }) { return <Button small primary={on} label={label} onClick={onClick} />; }
export function GlassButton({ label, onClick, disabled, small }) { return <Button small={small} label={label} onClick={onClick} disabled={disabled} />; }

export function Check({ on, onClick }) {
  const th = useTheme();
  return (
    <View onClick={onClick} style={{ width: 16, height: 16, borderRadius: 4, alignItems: 'center', justifyContent: 'center', flexShrink: 0,
      backgroundColor: on ? th.accent : 'transparent', border: `1.5px solid ${on ? th.accent : th.textDim}` }}>
      {on ? <Text style={{ fontSize: 11, lineHeight: 12, color: '#fff' }}>✓</Text> : null}
    </View>
  );
}

export function Bar({ value, style }) {
  const th = useTheme();
  return (
    <View style={{ height: 6, borderRadius: 3, backgroundColor: th.separator, overflow: 'hidden', direction: 'ltr', ...style }}>
      <View style={{ width: `${Math.round(Math.max(0, Math.min(1, value)) * 1000) / 10}%`, height: '100%', backgroundColor: th.accent }} />
    </View>
  );
}

export function Hero({ title, line, children }) {
  return (
    <View style={{ gap: spacing.s }}>
      <Label size={typeScale.title} weight={600}>{title}</Label>
      <Label dim>{line}</Label>
      {children}
    </View>
  );
}

// A long list drawn a window at a time: fetch(offset, limit) → { items, total }; a few dozen views at most.
export function PagedList({ fetch, rowH = 34, render, reset, style }) {
  const th = useTheme();
  const box = useRef(null);
  const [h, setH] = useState(320);
  const [y, setY] = useState(0);
  const st = useRef({ pages: new Map(), total: -1 });
  const PAGE = 100;
  const load = (p) => {
    if (st.current.pages.has(p)) return st.current.pages.get(p);
    const r = fetch(p * PAGE, PAGE);
    st.current.pages.set(p, r.items);
    st.current.total = r.total;
    return r.items;
  };
  const seen = useRef(reset);
  if (seen.current !== reset) { seen.current = reset; st.current = { pages: new Map(), total: -1 }; }
  useEffect(() => { setY(0); }, [reset]);
  useEffect(() => {
    const id = setTimeout(() => { if (box.current) { const v = lord.getLayout(box.current.id)[3]; if (v > 0 && Math.abs(v - h) > 1) setH(v); } }, 20);
    return () => clearTimeout(id);
  });
  if (st.current.total < 0) load(0);
  const total = Math.max(0, st.current.total), full = total * rowH, max = Math.max(0, full - h), top = Math.min(y, max);
  const first = Math.max(0, Math.floor(top / rowH) - 1), last = Math.min(total, Math.ceil((top + h) / rowH) + 1);
  const rows = [];
  for (let i = first; i < last; i++) {
    const it = load(Math.floor(i / PAGE))[i % PAGE];
    if (it) rows.push(render(it, i));
  }
  const bar = full > h ? Math.max(28, (h * h) / full) : 0;
  return (
    <View ref={box} onWheel={(e) => setY(Math.max(0, Math.min(max, top + e.deltaY)))} style={{ flex: 1, minHeight: 0, overflow: 'hidden', ...style }}>
      <View style={{ position: 'absolute', left: 0, right: bar ? 10 : 0, top: first * rowH - top }}>{rows}</View>
      {bar ? <View style={{ position: 'absolute', end: 2, width: 6, borderRadius: 3, height: bar, top: (h - bar) * (top / (max || 1)), backgroundColor: th.textDim }} /> : null}
    </View>
  );
}
