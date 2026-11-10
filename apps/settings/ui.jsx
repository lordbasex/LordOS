// ui.jsx — the building blocks of every Settings page, on top of the SDK's GroupBox (docs/DESIGN.md,
// "Settings"): a page column, titled sections (inset grouped boxes with hairlines between rows),
// rows with the control or the value at the trailing edge, form rows, a pop-up button, a page header, badges and notes.
import { View, Text, useTheme, addListener, GroupBox as InsetBox, spacing, typeScale, createStore, useStore, screen, ScrollView, Overlay, menuStep } from '@lordos/sdk';
import { createElement as h, useEffect, useRef, useState } from 'react';


// Status colors used by the pages (standard system colors).
export const tint = { ok: '#30d158', warn: '#ff9f0a', error: '#ff453a', off: '#8e8e93' };

// A row of a GroupBox: label (and an optional hint below it) at the start, the control at the end.
// Rows are 28 px of content with 4 px above and below (36 px between hairlines), taller with a hint.
// stack: the control goes under the label instead of at the trailing edge (wide segmented controls).
export function SettingRow({ label, hint, children, icon, disabled, onClick, minHeight = 36, labelStyle, stack }) {
  const t = useTheme();
  return (
    <View onClick={disabled ? undefined : onClick} style={{ minHeight, flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: hint ? 7 : 4,
      opacity: disabled ? 0.45 : 1 }}>
      {icon || null}
      <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
        <Text style={[{ fontSize: typeScale.body, color: t.text, textAlign: 'start' }, labelStyle]}>{label}</Text>
        {hint ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.35, textAlign: 'start' }}>{hint}</Text> : null}
        {stack && children != null && children !== false ? (
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: spacing.s, marginTop: spacing.xs }}>{children}</View>
        ) : null}
      </View>
      {!stack && children != null && children !== false ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>{children}</View>
      ) : null}
    </View>
  );
}

// A key/value row: the label at the start, the value right-aligned in the secondary color.
export function ValueRow({ label, value, children, mono }) {
  const t = useTheme();
  return (
    <View style={{ minHeight: 36, flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: 4 }}>
      <Text style={{ fontSize: typeScale.body, color: t.text, whiteSpace: 'nowrap', textAlign: 'start' }}>{label}</Text>
      <View style={{ flex: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', justifyContent: 'flex-end', gap: spacing.s }}>
        {value != null ? (
          <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.body, color: t.textDim, textAlign: 'end', whiteSpace: 'nowrap', overflow: 'hidden',
            ...(mono ? { fontFamily: 'monospace', fontSize: 12, direction: 'ltr' } : {}) }}>{String(value)}</Text>
        ) : null}
        {children}
      </View>
    </View>
  );
}

// A form row: the label at the start and a field of a fixed width at the trailing edge (forms
// inside a grouped box). The field shrinks in narrow windows.
export function FieldRow({ label, children, hint, width = 240 }) {
  const t = useTheme();
  return (
    <View style={{ minHeight: 36, flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: 4 }}>
      <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
        <Text style={{ fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{label}</Text>
        {hint ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{hint}</Text> : null}
      </View>
      <View style={{ width, flexShrink: 1, minWidth: 120, flexDirection: 'row', alignItems: 'center', gap: spacing.s, justifyContent: 'flex-end' }}>{children}</View>
    </View>
  );
}

// Buttons at the trailing edge, under a group or as the last row of one.
export function Actions({ children, start, style }) {
  return (
    <View style={[{ flexDirection: 'row', alignItems: 'center', flexWrap: 'wrap', gap: spacing.s, justifyContent: start ? 'flex-start' : 'flex-end' }, style]}>
      {children}
    </View>
  );
}

// A titled section: a 13/600 header, the inset box and an optional footer.
// A section without rows (children) shows only its header and footer.
export function Section({ title, footer, children, accessory, icon, style }) {
  const t = useTheme();
  const rows = (Array.isArray(children) ? children.flat() : [children]).filter((c) => c != null && c !== false && c !== '');
  return (
    <View style={[{ gap: 6 }, style]}>
      {title || accessory ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, minHeight: 22, paddingHorizontal: spacing.xs }}>
          {icon || null}
          <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{title || ''}</Text>
          {accessory || null}
        </View>
      ) : null}
      {rows.length ? <InsetBox>{rows}</InsetBox> : null}
      {footer ? <Note>{footer}</Note> : null}
    </View>
  );
}

// The hardware pages call it GroupBox (same props as the SDK's: title, footer, rows).
export const GroupBox = Section;

// Page header: a big icon, the title and one line of explanation (the top of a settings pane).
export function PageHeader({ icon, title, subtitle, children }) {
  const t = useTheme();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.l, padding: spacing.l, borderRadius: t.groupRadius,
      backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.025)', border: `1px solid ${t.separator}` }}>
      {icon}
      <View style={{ flex: 1, minWidth: 0, gap: 3 }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{title}</Text>
        {subtitle ? <Text style={{ fontSize: typeScale.callout, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{subtitle}</Text> : null}
      </View>
      {children}
    </View>
  );
}

// "#rrggbb" a bit lighter (f > 1) or darker (f < 1), for the badges' gradient.
export function shade(hex, f) {
  if (!/^#[0-9a-f]{6}$/i.test(hex)) return hex;
  const n = parseInt(hex.slice(1), 16);
  const c = (v) => Math.max(0, Math.min(255, Math.round(f > 1 ? v + (255 - v) * (f - 1) : v * f)));
  return `#${[n >> 16, (n >> 8) & 255, n & 255].map((v) => c(v).toString(16).padStart(2, '0')).join('')}`;
}

// A rounded square with a glyph and a soft top-to-bottom gradient, as in the sidebar (also used in rows).
export function Badge({ glyph, color, size = 26, children }) {
  return (
    <View style={{ width: size, height: size, borderRadius: Math.round(size * 0.24), alignItems: 'center', justifyContent: 'center',
      background: `linear-gradient(180deg, ${shade(color, 1.18)}, ${color})` }}>
      {children || <Text style={{ fontSize: Math.round(size * 0.55), fontWeight: 600, color: '#fff', lineHeight: size, textAlign: 'center' }}>{glyph}</Text>}
    </View>
  );
}

// Informational text under a group, or a warning (tone 'warn') or an error/success message
// (tone 'error' / 'ok').
export function Note({ children, tone, style }) {
  const t = useTheme();
  const color = tone === 'warn' ? tint.warn : tone === 'error' ? tint.error : tone === 'ok' ? tint.ok : t.textDim;
  return <Text style={[{ fontSize: typeScale.caption, color, lineHeight: 1.4, marginHorizontal: spacing.xs, textAlign: 'start' }, style]}>{children}</Text>;
}

// A result message ({ ok, text }) under a group.
export const Message = ({ msg }) => (msg ? <Note tone={msg.ok ? 'ok' : 'error'}>{msg.text}</Note> : null);

// A short status line with a colored dot (connected, off…).
export function StatusDot({ color, size = 8 }) {
  return <View style={{ width: size, height: size, borderRadius: size / 2, backgroundColor: color }} />;
}

// The page: a column of sections 20 px apart. It also hosts the open pop-up menu (PopUpLayer), which
// draws itself in the overlay layer (sdk/layers.js).
export function Stack({ children, gap = 20 }) {
  return (
    <View style={{ gap, paddingBottom: 24 }}>
      {children}
      <PopUpLayer />
    </View>
  );
}

const popup = createStore(null);       // { node, options, value, onChange, width, lit, keys }

// Pop-up button: shows the current value with ▲▼ and opens the list of options below it (or above,
// near the bottom of the screen). options = [[value, label]]; a value of '-' is a separator.
// A Tab stop (sdk/focus.js): Space, Enter, ↓ or ↑ open the list on the current value; while it is open ↓ and ↑ go
// through the options (lit), Enter or Space chooses one, Escape and Tab close it.
export function PopUp({ value, options, onChange, width = 190, disabled }) {
  const t = useTheme();
  const [hover, setHover] = useState(false);
  const btn = useRef(null);
  const open = useStore(popup, (p) => !!p && p.node === btn.current);
  useEffect(() => () => { if (popup.get()?.node === btn.current) popup.set(null); }, []);
  const cur = options.find(([v]) => v === value);
  const keys = useRef(null);
  const show = () => popup.set({ node: btn.current, options, value, onChange, width, lit: options.findIndex(([v]) => v === value), keys });
  const toggle = () => { if (disabled) return; if (open) popup.set(null); else show(); };
  keys.current = (e) => {
    if (e.ctrl || e.meta || e.alt) return false;
    const p = popup.get();
    const skip = (i) => options[i][0] === '-';
    if (!p || p.node !== btn.current) {
      if (disabled || (e.key !== 'ArrowDown' && e.key !== 'ArrowUp')) return false;
      show();
      return true;
    }
    if (e.key === 'Escape') { popup.set(null); return true; }
    if (e.key === 'Tab') { popup.set(null); return false; }
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { popup.set({ lit: menuStep(options.length, p.lit, e.key === 'ArrowDown' ? 1 : -1, skip) }); return true; }
    if (e.key === 'Enter' || e.key === ' ') {
      const o = p.lit >= 0 && !skip(p.lit) ? options[p.lit] : null;
      popup.set(null);
      if (o && o[0] !== value) onChange(o[0]);
      return true;
    }
    return false;
  };
  return (
    <View ref={btn} onClick={toggle} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      focusable={!disabled} onKeyDown={(e) => keys.current(e)}
      style={{ width, height: 24, paddingHorizontal: 9, borderRadius: t.controlRadiusSmall, flexDirection: 'row', alignItems: 'center', gap: 6,
        backgroundColor: t.control, opacity: disabled ? 0.45 : hover || open ? 0.88 : 1, boxShadow: '0 1px 1px rgba(0,0,0,0.12)',
        ...(t.buttonBorder ? { border: `1px solid ${t.separator}` } : null) }}>
      {cur && cur[2] ? h(cur[2], { size: 16 }) : null}
      <Text style={{ flex: 1, fontSize: 12, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{cur ? cur[1] : ''}</Text>
      {t.menuHighlight === 'subtle'
        // a drawn chevron (the themes with subtle menus, such as Classic)
        ? <View style={{ width: 16, height: 18, alignItems: 'center', justifyContent: 'center' }}>
          <View style={{ width: 6, height: 6, marginTop: -3, borderRightWidth: 1.5, borderBottomWidth: 1.5, borderColor: t.textDim, transform: 'rotate(45deg)' }} />
        </View>
        : <View style={{ width: 16, height: 18, borderRadius: 4, backgroundColor: t.accent, alignItems: 'center', justifyContent: 'center' }}>
          <Text style={{ fontSize: 8, color: '#fff', lineHeight: 8 }}>▲</Text>
          <Text style={{ fontSize: 8, color: '#fff', lineHeight: 8 }}>▼</Text>
        </View>}
    </View>
  );
}

function PopUpLayer() {
  const t = useTheme();
  const p = useStore(popup);
  const menu = useRef(null);
  useEffect(() => {
    if (!p) return undefined;
    return addListener('pointerdown', (e) => {
      const inside = (n) => { if (!n) return false; const [x, y, w, h] = lord.getLayout(n.id); return e.x >= x && e.x <= x + w && e.y >= y && e.y <= y + h; };
      if (!inside(menu.current) && !inside(p.node)) popup.set(null);
    });
  }, [p]);
  // opened with the pointer, the keys reach the list as a listener (with the keyboard focus on the button they come first)
  useEffect(() => {
    if (!p) return undefined;
    return addListener('keydown', (e) => { if (p.keys?.current(e)) e.preventDefault?.(); });
  }, [p]);
  if (!p || !p.node) return null;
  const [, by, bw, bh] = lord.getLayout(p.node.id);
  const rows = p.options.length;
  const mh = rows * 24 + 12;
  const below = by + bh + 4 + Math.min(mh, 320) < screen.height - 70;
  const height = Math.min(mh, 320);
  // in the overlay layer (sdk/layers.js), placed against the button: above the rows below it and
  // not clipped by the page's scroll view
  return (
    <Overlay anchor={p.node}>
      <View ref={menu} style={{ position: 'absolute', left: 0, top: below ? bh + 4 : -4 - height, width: Math.max(bw, p.width), height, padding: 5, borderRadius: t.menuRadius, overflow: 'hidden',
        backgroundColor: t.popupBg || (t.mode === 'dark' ? '#2c2c31' : '#ffffff'), border: `1px solid ${t.separator}`, boxShadow: '0 10px 30px rgba(0,0,0,0.35)' }}>
        <ScrollView style={{ flex: 1 }}>
          {p.options.map(([v, label], i) => (v === '-' ? <View key={`s${i}`} style={{ height: 1, marginVertical: 4, marginHorizontal: 6, backgroundColor: t.separator }} />
            : <PopRow key={String(v)} label={label} Icon={p.options[i][2]} checked={v === p.value} lit={i === p.lit} onHover={() => { if (p.lit !== -1) popup.set({ lit: -1 }); }}
              onClick={() => { popup.set(null); if (v !== p.value) p.onChange(v); }} />))}
        </ScrollView>
      </View>
    </Overlay>
  );
}

// keyed: the keyboard is on the row (as a hover); onHover: the pointer came onto it
function PopRow({ label, checked, onClick, Icon, lit: keyed, onHover }) {
  const t = useTheme();
  const [over, setHover] = useState(false);
  const lit = over || keyed;
  const hover = lit && t.menuHighlight !== 'subtle';
  return (
    <View onClick={onClick} onMouseEnter={() => { setHover(true); onHover?.(); }} onMouseLeave={() => setHover(false)}
      style={{ height: 24, paddingHorizontal: 6, borderRadius: t.menuItemRadius, flexDirection: 'row', alignItems: 'center', gap: 6,
        backgroundColor: hover ? t.accent : lit ? t.hover : 'transparent' }}>
      <Text style={{ width: 14, fontSize: 11, color: hover ? '#fff' : checked && !hover && t.menuHighlight === 'subtle' ? t.accent : t.text }}>{checked ? '✓' : ''}</Text>
      {Icon ? <Icon size={16} /> : null}
      <Text style={{ flex: 1, fontSize: 12, whiteSpace: 'nowrap', color: hover ? '#fff' : t.text, textAlign: 'start' }}>{label}</Text>
    </View>
  );
}

// "HH:MM" ↔ minutes after midnight, and a compact time field made of two steppers.
export const hhmm = (m) => `${String(Math.floor(m / 60) % 24).padStart(2, '0')}:${String(m % 60).padStart(2, '0')}`;

export function TimeField({ value, onChange, format }) {
  const t = useTheme();
  const step = (d) => onChange((((value + d) % 1440) + 1440) % 1440);
  const Arrow = ({ glyph, d }) => (
    <View onClick={() => step(d)} style={{ width: 16, height: 11, alignItems: 'center', justifyContent: 'center' }}>
      <Text style={{ fontSize: 7, color: t.textDim }}>{glyph}</Text>
    </View>
  );
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 2, direction: 'ltr' }}>
      <View style={{ height: 24, minWidth: 66, paddingHorizontal: 8, borderRadius: 6, backgroundColor: t.control, justifyContent: 'center' }}>
        <Text style={{ fontSize: 12, color: t.text, textAlign: 'center' }}>{format ? format(value) : hhmm(value)}</Text>
      </View>
      <View>
        <Arrow glyph="▲" d={15} />
        <Arrow glyph="▼" d={-15} />
      </View>
    </View>
  );
}
