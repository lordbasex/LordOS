// The pieces the Databases screens share (the approved mocks: Main, Structure, Content, Query, Table Info, Console):
// the palette from the theme's tokens, icons drawn from boxes, the mocks' buttons, check boxes, pop-up fields, form
// rows, boxes, sheets, the footer bar and its icon buttons.
import { View, Text, MenuButton, Overlay, addListener, useTheme, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';

// surfaces: the theme's tokens, with the mocks' darker bars in dark mode
export function usePal() {
  const t = useTheme();
  const dark = t.mode !== 'light';
  return {
    t, dark,
    bg: t.windowBg, side: t.sidebarBg, line: t.separator, input: t.inputBg, control: t.control, accent: t.accent,
    text: t.text, dim: t.textDim, hover: t.hover,
    bar: dark ? '#232328' : 'rgba(0,0,0,0.035)',
    foot: dark ? '#202024' : 'rgba(0,0,0,0.03)',
    head: dark ? '#2a2a30' : 'rgba(0,0,0,0.05)',
    tabs: dark ? '#19191c' : 'rgba(0,0,0,0.06)',
    zebra: dark ? 'rgba(255,255,255,0.028)' : 'rgba(0,0,0,0.025)',
    box: dark ? '#26262b' : 'rgba(0,0,0,0.03)',
    code: dark ? '#1a1a1d' : '#fafafc',
    gutter: dark ? '#1d1d21' : '#f2f2f5',
    nul: dark ? 'rgba(235,235,245,0.42)' : 'rgba(0,0,0,0.38)',
    sel: t.accent,
    ok: '#30d158', warn: '#ff9f0a', bad: '#ff453a', pk: '#ffd60a', mul: '#64d2ff', view: '#bf5af2', fx: '#ff9f0a',
    okBg: dark ? 'rgba(48,209,88,0.12)' : 'rgba(48,209,88,0.14)', badBg: dark ? 'rgba(255,69,58,0.12)' : 'rgba(255,69,58,0.1)',
    warnBg: dark ? 'rgba(255,159,10,0.14)' : 'rgba(255,159,10,0.16)',
  };
}

export const MONO = 'monospace';

// ---- icons, drawn from boxes (the engine has no SVG) ----
const B = (style) => <View style={{ position: 'absolute', ...style }} />;
export const Icons = {
  db: (c, s = 18) => (
    <View style={{ width: s, height: s }}>
      {B({ left: s * 0.18, top: s * 0.16, width: s * 0.64, height: s * 0.72, borderRadius: s * 0.16, border: `1.6px solid ${c}` })}
      {B({ left: s * 0.18, top: s * 0.16, width: s * 0.64, height: s * 0.26, borderRadius: s * 0.13, border: `1.6px solid ${c}` })}
      {B({ left: s * 0.18, top: s * 0.52, width: s * 0.64, height: 1.6, backgroundColor: c })}
    </View>
  ),
  table: (c, s = 18) => (
    <View style={{ width: s, height: s }}>
      {B({ left: s * 0.12, top: s * 0.18, width: s * 0.76, height: s * 0.64, borderRadius: 2, border: `1.5px solid ${c}` })}
      {B({ left: s * 0.12, top: s * 0.4, width: s * 0.76, height: 1.5, backgroundColor: c })}
      {B({ left: s * 0.38, top: s * 0.4, width: 1.5, height: s * 0.42, backgroundColor: c })}
    </View>
  ),
  grid: (c, s = 18) => (
    <View style={{ width: s, height: s }}>
      {B({ left: s * 0.12, top: s * 0.18, width: s * 0.76, height: s * 0.64, borderRadius: 2, border: `1.5px solid ${c}` })}
      {B({ left: s * 0.12, top: s * 0.4, width: s * 0.76, height: 1.5, backgroundColor: c })}
      {B({ left: s * 0.12, top: s * 0.6, width: s * 0.76, height: 1.5, backgroundColor: c })}
      {B({ left: s * 0.38, top: s * 0.18, width: 1.5, height: s * 0.64, backgroundColor: c })}
      {B({ left: s * 0.62, top: s * 0.18, width: 1.5, height: s * 0.64, backgroundColor: c })}
    </View>
  ),
  link: (c, s = 18) => (
    <View style={{ width: s, height: s }}>
      {B({ left: s * 0.08, top: s * 0.34, width: s * 0.5, height: s * 0.32, borderRadius: s * 0.16, border: `1.6px solid ${c}` })}
      {B({ left: s * 0.42, top: s * 0.34, width: s * 0.5, height: s * 0.32, borderRadius: s * 0.16, border: `1.6px solid ${c}` })}
    </View>
  ),
  bolt: (c, s = 18) => <View style={{ width: s, height: s, alignItems: 'center', justifyContent: 'center' }}><Text style={{ fontSize: s * 0.95, fontWeight: 700, color: c }}>ϟ</Text></View>,
  info: (c, s = 18) => (
    <View style={{ width: s, height: s, alignItems: 'center', justifyContent: 'center' }}>
      {B({ left: s * 0.1, top: s * 0.1, width: s * 0.8, height: s * 0.8, borderRadius: s * 0.4, border: `1.6px solid ${c}` })}
      <Text style={{ fontSize: s * 0.52, fontWeight: 700, color: c }}>i</Text>
    </View>
  ),
  doc: (c, s = 18) => (
    <View style={{ width: s, height: s }}>
      {B({ left: s * 0.2, top: s * 0.1, width: s * 0.6, height: s * 0.8, borderRadius: 2, border: `1.5px solid ${c}` })}
      {B({ left: s * 0.32, top: s * 0.45, width: s * 0.36, height: 1.5, backgroundColor: c })}
      {B({ left: s * 0.32, top: s * 0.63, width: s * 0.24, height: 1.5, backgroundColor: c })}
    </View>
  ),
  users: (c, s = 18) => (
    <View style={{ width: s, height: s }}>
      {B({ left: s * 0.24, top: s * 0.1, width: s * 0.34, height: s * 0.34, borderRadius: s * 0.17, border: `1.6px solid ${c}` })}
      {B({ left: s * 0.08, top: s * 0.56, width: s * 0.66, height: s * 0.36, borderTopLeftRadius: s * 0.3, borderTopRightRadius: s * 0.3, border: `1.6px solid ${c}`, borderBottomColor: 'transparent' })}
      {B({ left: s * 0.66, top: s * 0.14, width: s * 0.2, height: s * 0.32, borderTopRightRadius: s * 0.16, borderBottomRightRadius: s * 0.16, border: `1.6px solid ${c}`, borderLeftColor: 'transparent' })}
    </View>
  ),
  pulse: (c, s = 18) => (
    <View style={{ width: s, height: s, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: s * 0.08 }}>
      {[0.25, 0.7, 0.45, 0.9, 0.35].map((h, i) => <View key={i} style={{ width: s * 0.1, height: s * h, borderRadius: 1, backgroundColor: c }} />)}
    </View>
  ),
  console: (c, s = 18) => (
    <View style={{ width: s, height: s, alignItems: 'center', justifyContent: 'center' }}>
      {B({ left: s * 0.08, top: s * 0.16, width: s * 0.84, height: s * 0.68, borderRadius: 2, border: `1.5px solid ${c}` })}
      <Text style={{ fontSize: s * 0.42, fontWeight: 700, color: c, fontFamily: MONO }}>{'>_'}</Text>
    </View>
  ),
  eye: (c, s = 14) => (
    <View style={{ width: s, height: s }}>
      {B({ left: 0, top: s * 0.24, width: s, height: s * 0.52, borderRadius: s * 0.26, border: `1.5px solid ${c}` })}
      {B({ left: s * 0.36, top: s * 0.36, width: s * 0.28, height: s * 0.28, borderRadius: s * 0.14, backgroundColor: c })}
    </View>
  ),
  clock: (c, s = 14) => (
    <View style={{ width: s, height: s }}>
      {B({ left: 0, top: 0, width: s, height: s, borderRadius: s / 2, border: `1.5px solid ${c}` })}
      {B({ left: s * 0.47, top: s * 0.22, width: 1.5, height: s * 0.32, backgroundColor: c })}
      {B({ left: s * 0.47, top: s * 0.5, width: s * 0.24, height: 1.5, backgroundColor: c })}
    </View>
  ),
  folder: (c, s = 14) => (
    <View style={{ width: s, height: s }}>
      {B({ left: 0, top: s * 0.12, width: s * 0.45, height: s * 0.25, borderTopLeftRadius: 2, borderTopRightRadius: 2, backgroundColor: c })}
      {B({ left: 0, top: s * 0.24, width: s, height: s * 0.66, borderRadius: 2, border: `1.5px solid ${c}` })}
    </View>
  ),
  lock: (c, s = 14) => (
    <View style={{ width: s, height: s }}>
      {B({ left: s * 0.26, top: 0, width: s * 0.48, height: s * 0.6, borderRadius: s * 0.24, border: `1.5px solid ${c}` })}
      {B({ left: s * 0.12, top: s * 0.42, width: s * 0.76, height: s * 0.56, borderRadius: 2, backgroundColor: c })}
    </View>
  ),
  fx: (c) => <Text style={{ width: 14, fontSize: 11, fontFamily: MONO, color: c }}>fx</Text>,
};
export const Dot = ({ color, size = 8 }) => <View style={{ width: size, height: size, borderRadius: size / 2, backgroundColor: color || 'transparent', border: color ? 'none' : '1px solid rgba(128,128,128,0.6)' }} />;

// ---- buttons ----
// the mocks' buttons: gray, primary (accent) or a link
export function Btn({ label, onClick, primary, disabled, link, danger, small, icon, style, title }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  const bg = link ? 'transparent' : primary ? (danger ? '#e5483f' : p.accent) : p.control;
  return (
    <View onClick={disabled ? undefined : onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)} title={title}
      style={{ height: small ? 22 : 28, paddingHorizontal: link ? 2 : small ? 9 : 14, borderRadius: small ? 6 : 7, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 6,
        backgroundColor: bg, opacity: disabled ? 0.45 : hover && !link ? 0.86 : 1, ...style }}>
      {icon ? icon(primary ? '#fff' : p.text) : null}
      <Text style={{ fontSize: small ? 12 : 13, fontWeight: primary ? 600 : 500, whiteSpace: 'nowrap',
        color: link ? (hover ? '#7cbcff' : '#4da3ff') : primary ? '#fff' : danger ? p.bad : p.text }}>{label}</Text>
    </View>
  );
}

// a small icon button of the footers (+ − duplicate refresh, pages)
export function IconBtn({ glyph, icon, title, onClick, disabled, active, size = 15 }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  return (
    <View onClick={disabled ? undefined : onClick} title={title} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: 28, height: 24, borderRadius: 6, alignItems: 'center', justifyContent: 'center', opacity: disabled ? 0.35 : 1,
        backgroundColor: active ? p.hover : hover && !disabled ? p.hover : 'transparent' }}>
      {icon ? icon(p.dim) : <Text style={{ fontSize: size, color: p.dim }}>{glyph}</Text>}
    </View>
  );
}

// an icon above its name: the mocks' main toolbar (Structure, Content, … Console)
export function ToolItem({ icon, label, onClick, on, disabled }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  const fg = on ? '#5aa9ff' : hover && !disabled ? p.text : p.dim;
  return (
    <View onClick={disabled ? undefined : onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ minWidth: 62, paddingHorizontal: 6, paddingVertical: 5, gap: 4, borderRadius: 7, alignItems: 'center', opacity: disabled ? 0.4 : 1,
        backgroundColor: on ? 'rgba(10,132,255,0.2)' : hover && !disabled ? p.hover : 'transparent' }}>
      {icon(fg, 20)}
      <Text style={{ fontSize: 11, color: fg, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}
export const Sep = ({ height = 34 }) => { const p = usePal(); return <View style={{ width: 1, height, marginHorizontal: 8, backgroundColor: p.line }} />; };

// ---- form ----
export function Check({ on, onChange, label, disabled }) {
  const p = usePal();
  return (
    <View onClick={disabled ? undefined : () => onChange(!on)} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, opacity: disabled ? 0.5 : 1 }}>
      <View style={{ width: 15, height: 15, borderRadius: 4, alignItems: 'center', justifyContent: 'center',
        backgroundColor: on ? p.accent : 'transparent', border: `1.5px solid ${on ? p.accent : p.dim}` }}>
        {on ? <Text style={{ fontSize: 10, fontWeight: 700, color: '#fff' }}>✓</Text> : null}
      </View>
      {label ? <Text style={{ fontSize: 13, color: p.text, textAlign: 'start' }}>{label}</Text> : null}
    </View>
  );
}
export function Radio({ on, onClick, label, disabled }) {
  const p = usePal();
  return (
    <View onClick={disabled ? undefined : onClick} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, opacity: disabled ? 0.5 : 1 }}>
      <View style={{ width: 15, height: 15, borderRadius: 8, backgroundColor: on ? '#fff' : 'transparent', border: on ? `5px solid ${p.accent}` : `1.5px solid ${p.dim}` }} />
      <Text style={{ fontSize: 13, color: p.text }}>{label}</Text>
    </View>
  );
}

// a pop-up field: options [[value, label]]
export function Popup({ value, options, onChange, width, small, extra = [], placeholder }) {
  const p = usePal();
  const cur = options.find((o) => o[0] === value);
  const items = [...options.map(([v, label]) => ({ label, checked: v === value, onClick: () => onChange(v) })), ...(extra.length ? ['-', ...extra] : [])];
  return (
    <MenuButton items={items} minWidth={Math.max(160, width || 0)} trigger={(open) => (
      <View style={{ width, height: small ? 26 : 28, paddingHorizontal: 10, borderRadius: small ? 6 : 7, flexDirection: 'row', alignItems: 'center', gap: 8,
        backgroundColor: p.control, border: `1px solid ${open ? p.accent : 'transparent'}` }}>
        <Text style={{ flex: width ? 1 : 0, fontSize: small ? 12.5 : 13, color: cur ? p.text : p.dim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{cur ? cur[1] : placeholder || ''}</Text>
        <Text style={{ fontSize: 9, color: p.dim }}>▼</Text>
      </View>
    )} />
  );
}

// a label column (right-aligned, as in the mocks) and a field
export function Field({ label, children, labelWidth = 120, top }) {
  const p = usePal();
  return (
    <View style={{ flexDirection: 'row', alignItems: top ? 'flex-start' : 'center', gap: 12, minHeight: 28 }}>
      <Text style={{ width: labelWidth, fontSize: 13, color: p.dim, textAlign: 'end', marginTop: top ? 6 : 0 }}>{label}</Text>
      <View style={{ flex: 1, minWidth: 0 }}>{children}</View>
    </View>
  );
}

// a rounded box with a small caps title (DATABASE SERVER, SSH TUNNEL, …)
export function Box({ title, children, style, right }) {
  const p = usePal();
  return (
    <View style={{ backgroundColor: p.box, border: `1px solid ${p.line}`, borderRadius: 10, paddingHorizontal: 18, paddingVertical: 14, gap: 9, ...style }}>
      {title ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', marginBottom: 2 }}>
          <Text style={{ flex: 1, fontSize: 11, fontWeight: 600, color: p.dim, letterSpacing: 0.5, textAlign: 'start' }}>{title}</Text>
          {right || null}
        </View>
      ) : null}
      {children}
    </View>
  );
}
export const Caps = ({ children, style }) => { const p = usePal(); return <Text style={{ fontSize: 11, fontWeight: 600, color: p.dim, letterSpacing: 0.5, textAlign: 'start', ...style }}>{children}</Text>; };

// a note under a form: info, ok, warn or bad
export function Note({ kind = 'info', children, style, action }) {
  const p = usePal();
  const [bg, fg] = kind === 'ok' ? [p.okBg, p.ok] : kind === 'bad' ? [p.badBg, p.bad] : kind === 'warn' ? [p.warnBg, p.warn] : [p.box, p.dim];
  return (
    <View style={{ flexDirection: 'row', gap: 10, alignItems: 'center', backgroundColor: bg, border: `1px solid ${kind === 'info' ? p.line : fg}`, borderRadius: 8, paddingHorizontal: 12, paddingVertical: 9, ...style }}>
      <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: fg }} />
      <Text style={{ flex: 1, fontSize: 12, color: p.text, lineHeight: 1.4, textAlign: 'start' }}>{children}</Text>
      {action || null}
    </View>
  );
}

// the footer bar of a pane
export function Foot({ children, style }) {
  const p = usePal();
  return (
    <View style={{ height: 32, flexDirection: 'row', alignItems: 'center', gap: 2, paddingHorizontal: 6, backgroundColor: p.foot, ...style }}>
      <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 1, backgroundColor: p.line }} />
      {children}
    </View>
  );
}
export const FootText = ({ children, color, mono, style }) => {
  const p = usePal();
  return <Text style={{ fontSize: mono ? 11.5 : 12, color: color || p.dim, fontFamily: mono ? MONO : undefined, whiteSpace: 'nowrap', overflow: 'hidden', ...style }}>{children}</Text>;
};

// ---- sheets ----
// a panel over the window: title, body, actions at the end. Esc cancels, Enter does the primary action (when given).
export function Sheet({ title, children, actions, width = 520, onCancel, onEnter }) {
  const p = usePal();
  useEffect(() => addListener('keydown', (e) => {
    if (e.key === 'Escape') onCancel?.();
    else if (e.key === 'Enter' && !e.ctrl && onEnter) onEnter();
  }), [onCancel, onEnter]);
  return (
    <View onClick={(e) => e.stopPropagation?.()} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.38)', zIndex: 50 }}>
      <View style={{ width, maxWidth: '94%', maxHeight: '94%', paddingHorizontal: 22, paddingVertical: 20, gap: 12, borderRadius: 14, backgroundColor: p.bg,
        border: `1px solid ${p.line}`, boxShadow: '0 18px 48px rgba(0,0,0,0.5)' }}>
        {title ? <Text style={{ fontSize: 15, fontWeight: 700, color: p.text, textAlign: 'start' }}>{title}</Text> : null}
        {children}
        {actions?.length ? <View style={{ flexDirection: 'row', justifyContent: 'flex-end', alignItems: 'center', gap: 8, marginTop: 4 }}>{actions}</View> : null}
      </View>
    </View>
  );
}

// a block of code (SQL), selectable by copying: the CREATE syntax, an ALTER before it runs
export function CodeBlock({ lines, style, maxLines }) {
  const p = usePal();
  const shown = maxLines && lines.length > maxLines ? [...lines.slice(0, maxLines), [['…', 'comment']]] : lines;
  return (
    <View style={{ backgroundColor: p.code, border: `1px solid ${p.line}`, borderRadius: 8, paddingHorizontal: 14, paddingVertical: 10, overflow: 'hidden', direction: 'ltr', ...style }}>
      {shown.map((toks, i) => (
        <View key={i} style={{ flexDirection: 'row', height: 19 }}>
          {toks.map(([text, color], j) => <Text key={j} style={{ fontFamily: MONO, fontSize: 12, color: color || p.text, whiteSpace: 'pre' }}>{text}</Text>)}
        </View>
      ))}
    </View>
  );
}

// a menu from a small button, in the overlay layer
export function MenuGlyph({ glyph = '⋯', title, items }) {
  return <MenuButton icon={glyph} title={title} small items={items} />;
}

// the "coming in a later version" pane (phase 2 screens)
export function Later({ label }) {
  const p = usePal();
  return (
    <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 8, padding: 30 }}>
      <Text style={{ fontSize: 15, fontWeight: 600, color: p.text }}>{label}</Text>
      <Text style={{ fontSize: 12.5, color: p.dim, textAlign: 'center', maxWidth: 380, lineHeight: 1.4 }}>{tr('databases.later')}</Text>
    </View>
  );
}

// the window's size, read after each render and every half second (there are no resize events)
export function useSize(ref, initial = { w: 800, h: 500 }) {
  const [size, setSize] = useState(initial);
  const last = useRef(initial);
  useEffect(() => {
    const read = () => {
      if (!ref.current) return;
      try {
        const [, , w, h] = lord.getLayout(ref.current.id);
        if (Math.abs(w - last.current.w) > 0.5 || Math.abs(h - last.current.h) > 0.5) { last.current = { w, h }; setSize({ w, h }); }
      } catch (e) { /* not laid out yet */ }
    };
    read();
    const id = setInterval(read, 400);
    return () => clearInterval(id);
  }, []);
  return size;
}

export { Overlay };
