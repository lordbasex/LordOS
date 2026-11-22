// kit.jsx — Machines' own controls in the look of a native virtualization app on a Mac desktop (2026-11-20, the
// maintainer: sheets over the window instead of pages in it, glyphs beside the sections, pill buttons, grouped cards;
// the look of hypervisaur's screenshots, BSD-2-Clause, github.com/Zesty0wl/hypervisaur, reimplemented here: nothing of
// its code or images is used). The glyphs are LordOS's own (assets/machines/symbols).
import { View, Text, Image, addListener } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { usePal, hexOf } from './ui.jsx';

// a glyph of assets/machines/symbols, tinted (the theme's accent unless told)
export function Sym({ name, size = 20, color }) {
  const p = usePal();
  const hex = hexOf(color || p.accent);
  const px = Math.max(16, Math.min(256, Math.ceil(size * 2)));
  return <Image src={`svg:${px}:${hex}:asset:machines/symbols/${name}-symbolic.svg`} style={{ width: size, height: size }} />;
}

// A sheet: the window behind stays, dimmed, and the sheet comes to the front, as an alert does. Escape and a press on
// the dimmed part close it (unless `modal`).
export function Sheet({ width = 880, height = 640, onClose, modal, children }) {
  const p = usePal();
  const close = useRef(onClose);
  close.current = onClose;
  useEffect(() => addListener('keydown', (e) => { if (e.key === 'Escape' && !modal) close.current?.(); }), [modal]);
  return (
    <View onClick={modal ? undefined : () => close.current?.()} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0,
      backgroundColor: p.dark ? 'rgba(0,0,0,0.5)' : 'rgba(30,30,40,0.28)', alignItems: 'center', justifyContent: 'center', padding: 24 }}>
      <View onClick={(e) => e.stopPropagation()} style={{ width, height, maxWidth: '100%', maxHeight: '100%', borderRadius: 16, overflow: 'hidden',
        backgroundColor: p.dark ? '#1f1f2b' : '#f6f6f8', border: `1px solid ${p.dark ? 'rgba(255,255,255,0.12)' : 'rgba(0,0,0,0.12)'}`,
        boxShadow: '0 24px 64px rgba(0,0,0,0.55)' }}>
        {children}
      </View>
    </View>
  );
}

// a sheet's title bar: an optional back button, a title and a subtitle
export function SheetHeader({ title, subtitle, onBack, right }) {
  const p = usePal();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 18, height: 60,
      backgroundColor: p.dark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.03)', borderBottomWidth: 1, borderColor: p.line }}>
      {onBack ? <Round onClick={onBack}><Sym name="chevron-back" size={18} color={p.text} /></Round> : null}
      <View style={{ flex: 1, minWidth: 0 }}>
        <Text style={{ fontSize: 14, fontWeight: 700, color: p.text, textAlign: 'start' }}>{title}</Text>
        {subtitle ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{subtitle}</Text> : null}
      </View>
      {right || null}
    </View>
  );
}

// a round button (back, refresh)
export function Round({ onClick, children, size = 32, title }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  return (
    <View title={title} onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: size, height: size, borderRadius: size / 2, alignItems: 'center', justifyContent: 'center',
        backgroundColor: hover ? (p.dark ? 'rgba(255,255,255,0.16)' : 'rgba(0,0,0,0.1)') : (p.dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.05)') }}>
      {children}
    </View>
  );
}

// a pill button: Cancel, Done, Continue
export function Pill({ label, onClick, primary, disabled, danger, icon }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  const bg = primary ? (danger ? '#e5483f' : p.accent) : p.dark ? 'rgba(255,255,255,0.1)' : 'rgba(0,0,0,0.06)';
  return (
    <View onClick={disabled ? undefined : onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 36, paddingHorizontal: 20, borderRadius: 18, flexDirection: 'row', gap: 8, alignItems: 'center', justifyContent: 'center',
        backgroundColor: bg, opacity: disabled ? 0.45 : hover ? 0.88 : 1, border: primary ? 'none' : `1px solid ${p.dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.08)'}` }}>
      {icon ? <Sym name={icon} size={16} color={primary ? '#ffffff' : danger ? '#ff453a' : p.text} /> : null}
      <Text style={{ fontSize: 13, fontWeight: 600, color: primary ? '#fff' : danger ? '#ff453a' : p.text, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}

// a titled group of rows on a rounded card
export function Section({ title, children, style }) {
  const p = usePal();
  return (
    <View style={{ gap: 8, ...style }}>
      {title ? <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{title}</Text> : null}
      <View style={{ borderRadius: 14, backgroundColor: p.dark ? 'rgba(255,255,255,0.05)' : '#ffffff',
        border: `1px solid ${p.dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.08)'}` }}>{children}</View>
    </View>
  );
}

// a sidebar row: glyph, title and a one-line summary; the selected one filled with the accent
export function SideRow({ icon, title, detail, selected, onClick }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 10, paddingVertical: 7, borderRadius: 10,
        backgroundColor: selected ? p.accent : hover ? (p.dark ? 'rgba(255,255,255,0.06)' : 'rgba(0,0,0,0.04)') : 'transparent' }}>
      <Sym name={icon} size={22} color={selected ? '#ffffff' : p.accent} />
      <View style={{ flex: 1, minWidth: 0 }}>
        <Text style={{ fontSize: 13, fontWeight: 600, color: selected ? '#fff' : p.text, textAlign: 'start' }}>{title}</Text>
        {detail ? <Text style={{ fontSize: 11, color: selected ? 'rgba(255,255,255,0.82)' : p.dim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{detail}</Text> : null}
      </View>
    </View>
  );
}

// a pane's header: its glyph, title and summary
export function PaneHeader({ icon, title, detail }) {
  const p = usePal();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12 }}>
      <Sym name={icon} size={28} color={p.text} />
      <View style={{ flex: 1, minWidth: 0 }}>
        <Text style={{ fontSize: 20, fontWeight: 700, color: p.text, textAlign: 'start' }}>{title}</Text>
        {detail ? <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start' }}>{detail}</Text> : null}
      </View>
    </View>
  );
}

// a group of toolbar buttons on one rounded pill (the machine window's toolbar)
export function ToolPill({ children }) {
  const p = usePal();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 2, padding: 3, borderRadius: 20,
      backgroundColor: p.dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.05)', border: `1px solid ${p.dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.06)'}` }}>{children}</View>
  );
}
export function ToolGlyph({ icon, title, onClick, on, disabled, badge }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  return (
    <View title={title} onClick={disabled ? undefined : onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: 36, height: 30, borderRadius: 15, alignItems: 'center', justifyContent: 'center', opacity: disabled ? 0.4 : 1,
        backgroundColor: on ? p.accent : hover ? (p.dark ? 'rgba(255,255,255,0.12)' : 'rgba(0,0,0,0.08)') : 'transparent' }}>
      <Sym name={icon} size={18} color={on ? '#ffffff' : p.text} />
      {badge ? <View style={{ position: 'absolute', right: 6, bottom: 4, width: 8, height: 8, borderRadius: 4, backgroundColor: badge,
        border: `1.5px solid ${p.dark ? '#1f1f2b' : '#fff'}` }} /> : null}
    </View>
  );
}
