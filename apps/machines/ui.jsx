// Pieces the Machines windows share: the state dot, the system marks, the three lights, bars, a context menu and the
// surfaces of the Control Center (docs/VIRTUALIZATION.md, "The app"; the approved mocks, Main and Toolbar).
import { View, Text, Overlay, screen, addListener, useTheme, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';

export const TONE = { run: '#30d158', pause: '#ff9f0a', off: '#8e8e93', busy: '#0a84ff', bad: '#ff453a' };
export const has = () => typeof lord !== 'undefined' && !!lord.vm;
export const gb = (mb) => (mb >= 1024 ? `${Math.round((mb / 1024) * 10) / 10} GB` : `${mb} MB`);
export const bytes = (b) => (b >= 1e12 ? `${Math.round(b / 1e11) / 10} TB` : b >= 1e9 ? `${Math.round(b / 1e8) / 10} GB` : `${Math.round(b / 1e6)} MB`);
export const gib = (b) => `${Math.round((b / 1073741824) * 10) / 10}`;

// surfaces: dark like the mocks, light from the theme
export function usePal() {
  const t = useTheme();
  const dark = t.mode === 'dark';
  return {
    t, dark,
    bg: dark ? '#1f1f23' : t.windowBg,
    card: dark ? '#2b2b31' : '#ffffff',
    raised: dark ? '#26262b' : 'rgba(0,0,0,0.035)',
    track: dark ? '#44444c' : 'rgba(0,0,0,0.12)',
    line: dark ? '#3b3b43' : t.separator,
    btn: dark ? '#3b3b43' : 'rgba(0,0,0,0.07)',
    text: t.text, dim: t.textDim,
    accent: '#0a84ff', cpu: '#6cb6ff', mem: '#c79bff', disk: '#ffb340', ok: '#30d158', warn: '#ffc56e', bad: '#ff9a9a',
    warnBg: dark ? '#4a3512' : '#fff1d6', badBg: dark ? '#4a2028' : '#ffe1e4',
  };
}

export function StateDot({ tone, size = 8 }) { return <View style={{ width: size, height: size, borderRadius: size / 2, backgroundColor: TONE[tone] || TONE.off }} />; }

// Our own marks for the systems we recognize: a colored rounded tile with a letter, never a project's logo.
const MARKS = {
  lordos: ['#5aa9ff', '#1f5fbf', 'L'], debian: ['#e0668c', '#8c2146', 'D'], ubuntu: ['#f28a55', '#a83a1a', 'U'], fedora: ['#6a8fd8', '#2b4478', 'F'],
  mint: ['#8bd17c', '#3c7a32', 'M'], arch: ['#5fb4e0', '#1d6a91', 'A'], alpine: ['#4fa3c7', '#1a4f6a', 'A'], opensuse: ['#7fcf6a', '#2f6b25', 'S'],
  kali: ['#5b7fd6', '#22305a', 'K'], rocky: ['#4fc6a0', '#1f6a52', 'R'], alma: ['#e2b04a', '#8a5a14', 'A'], centos: ['#b07ad6', '#5a2d7a', 'C'],
  freebsd: ['#e05a5a', '#7a1f1f', 'B'], raspios: ['#d6567a', '#7a1f3a', 'P'], other: ['#8a93a6', '#3b4252', '▣'],
};
export function OsMark({ id, size = 40 }) {
  const [a, b, ch] = MARKS[id] || MARKS.other;
  return (
    <View style={{ width: size, height: size, borderRadius: size * 0.22, alignItems: 'center', justifyContent: 'center', background: `linear-gradient(180deg, ${a}, ${b})` }}>
      <Text style={{ fontSize: size * 0.5, fontWeight: 700, color: '#fff' }}>{ch}</Text>
    </View>
  );
}
// the card picture's colors when there's no picture yet
export const markColors = (id) => (MARKS[id] || MARKS.other).slice(0, 2);

// power, agent, disk activity
export function Lights({ power, agent, disk }) {
  const dot = (on, color, label) => <View key={label} title={label} style={{ width: 9, height: 9, borderRadius: 5, backgroundColor: on ? color : '#5a5a63' }} />;
  return (
    <View style={{ flexDirection: 'row', gap: 6, paddingHorizontal: 8, height: 20, alignItems: 'center', borderRadius: 10, backgroundColor: 'rgba(0,0,0,0.62)' }}>
      {dot(power, '#30d158', tr(power ? 'machines.light.powerOn' : 'machines.light.powerOff'))}
      {dot(agent, '#6cb6ff', tr(agent ? 'machines.light.agentOn' : 'machines.light.agentOff'))}
      {dot(disk, '#ffb340', tr(disk ? 'machines.light.diskActive' : 'machines.light.diskIdle'))}
    </View>
  );
}

export function Bar({ frac, color, height = 6 }) {
  const p = usePal();
  const f = Math.max(0, Math.min(1, frac || 0));
  return (
    <View style={{ height, borderRadius: height / 2, backgroundColor: p.track, overflow: 'hidden' }}>
      <View style={{ width: `${Math.round(f * 1000) / 10}%`, height, borderRadius: height / 2, backgroundColor: color }} />
    </View>
  );
}

// A button in the mocks' style: filled (accent), plain (gray) or a text link
export function Btn({ label, onClick, primary, disabled, big, link, danger, style }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  const bg = link ? 'transparent' : primary ? p.accent : p.btn;
  return (
    <View onClick={disabled ? undefined : onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: big ? 44 : 32, paddingHorizontal: big ? 18 : 12, borderRadius: big ? 12 : 8, alignItems: 'center', justifyContent: 'center',
        backgroundColor: bg, opacity: disabled ? 0.45 : hover && !link ? 0.88 : 1, ...style }}>
      <Text style={{ fontSize: big ? 14 : 13, fontWeight: primary || big ? 700 : 600, whiteSpace: 'nowrap',
        color: link ? '#6cb6ff' : primary ? '#fff' : danger ? '#ff453a' : p.text }}>{label}</Text>
    </View>
  );
}

// A toolbar button with a glyph and its name as the tooltip (the mock "Session": agent, shell, logs, screenshot,
// record, pause, power)
export function IconBtn({ glyph, label, onClick, disabled, on, danger }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  return (
    <View onClick={disabled ? undefined : onClick} title={label} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: 32, height: 30, borderRadius: 8, alignItems: 'center', justifyContent: 'center', opacity: disabled ? 0.4 : 1,
        backgroundColor: danger ? '#8f1f2c' : on ? p.accent : hover && !disabled ? p.btn : 'transparent' }}>
      <Text style={{ fontSize: 14, fontWeight: 700, color: on || danger ? '#fff' : p.text }}>{glyph}</Text>
    </View>
  );
}

// A menu at a point of the window (right-click on a card) or under a button; items as MenuButton's
// How many menus are open: while one is, a machine's window keeps the keyboard for the menu (Esc closes it and never
// reaches the guest).
export const menus = { open: 0 };
export function PopMenu({ x, y, items, onClose, width = 220 }) {
  const p = usePal();
  const ref = useRef(null);
  useEffect(() => { menus.open++; return () => { menus.open--; }; }, []);
  useEffect(() => {
    const a = addListener('pointerdown', (e) => {
      const n = ref.current;
      if (!n) return;
      const [lx, ly, w, h] = lord.getLayout(n.id);
      if (e.x < lx || e.x > lx + w || e.y < ly || e.y > ly + h) onClose();
    });
    const b = addListener('keydown', (e) => { if (e.key === 'Escape') onClose(); });
    return () => { a(); b(); };
  }, []);
  return (
    <View ref={ref} style={{ position: 'absolute', left: x, top: y, width, padding: 5, borderRadius: 10, backgroundColor: p.dark ? '#2b2b31' : '#ffffff',
      border: `1px solid ${p.line}`, boxShadow: '0 12px 34px rgba(0,0,0,0.5)' }}>
      {items.filter(Boolean).map((it, i) => (it === '-' ? <View key={`s${i}`} style={{ height: 1, marginVertical: 4, marginHorizontal: 6, backgroundColor: p.line }} />
        : <MenuItem key={`${i}${it.label}`} it={it} onClose={onClose} />))}
    </View>
  );
}
function MenuItem({ it, onClose }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  const lit = hover && !it.disabled;
  return (
    <View onClick={() => { if (it.disabled) return; onClose(); it.onClick?.(); }} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ minHeight: 28, paddingHorizontal: 8, borderRadius: 6, flexDirection: 'row', alignItems: 'center', gap: 6, backgroundColor: lit ? p.accent : 'transparent', opacity: it.disabled ? 0.4 : 1 }}>
      <Text style={{ width: 12, fontSize: 11, color: lit ? '#fff' : p.text }}>{it.checked ? '✓' : ''}</Text>
      <Text style={{ flex: 1, fontSize: 13, whiteSpace: 'nowrap', color: lit ? '#fff' : it.destructive ? '#ff453a' : p.text }}>{it.label}</Text>
      {it.hint ? <Text style={{ fontSize: 11, color: lit ? '#e6f1ff' : p.dim }}>{it.hint}</Text> : null}
    </View>
  );
}

// a toolbar button that opens a PopMenu under itself (in the window's overlay layer, so nothing after it covers it)
export function MenuBtn({ label, items, primary, disabled }) {
  const [open, setOpen] = useState(false);
  const [end, setEnd] = useState(false);
  const ref = useRef(null);
  const show = () => {
    if (!ref.current) return;
    const [x] = lord.getLayout(ref.current.id);
    setEnd(x > screen.width / 2);
    setOpen(true);
  };
  return (
    <View ref={ref}>
      <Btn label={`${label} ▾`} primary={primary} disabled={disabled} onClick={() => (open ? setOpen(false) : show())} style={{ height: 30 }} />
      {open ? (
        <Overlay anchor={ref}>
          <View style={{ position: 'absolute', top: 34, width: 240, ...(end ? { right: 0 } : { left: 0 }) }}>
            <PopMenu x={0} y={0} width={240} items={items} onClose={() => setOpen(false)} />
          </View>
        </Overlay>
      ) : null}
    </View>
  );
}

// the window's content root, for a menu at a pointer position (PopMenu takes coordinates inside it)
export const overlayRoot = { current: null };

// A dialog over the window (the mocks' Clone and the snapshot sheets): title, body, actions on the right. Esc cancels.
export function Sheet({ title, children, actions, width = 560, onCancel }) {
  const p = usePal();
  useEffect(() => addListener('keydown', (e) => { if (e.key === 'Escape') onCancel?.(); }), [onCancel]);
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.45)' }}>
      <View style={{ width, padding: 22, gap: 14, borderRadius: 18, backgroundColor: p.dark ? '#1f1f23' : '#ffffff', boxShadow: '0 24px 60px rgba(0,0,0,0.5)' }}>
        <Text style={{ fontSize: 18, fontWeight: 700, color: p.text, textAlign: 'start' }}>{title}</Text>
        {children}
        {actions?.length ? <View style={{ flexDirection: 'row', justifyContent: 'flex-end', alignItems: 'center', gap: 8 }}>{actions}</View> : null}
      </View>
    </View>
  );
}
