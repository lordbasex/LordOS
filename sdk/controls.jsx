// controls.jsx — shared building blocks for the layout of LordOS apps: toolbar buttons
// with tooltips, toolbars, a "more" menu button, a search field, empty states and grouped
// ("inset") forms. The spacing and type scale they follow is in docs/DESIGN.md.
import { View, Text, addListener } from '@lordos/runtime';
import { useEffect, useRef, useState } from 'react';
import { useTheme, popupBg } from './theme.js';
import { TextInput, useAppWindow } from './components.jsx';
import { wm } from './windows.js';
import { direction } from './i18n.js';
import { Lumo } from './mascot.js';
import { Overlay } from './layers.js';

// Design tokens (docs/DESIGN.md): an 8 px grid, desktop-standard type sizes and control heights.
export const spacing = { xxs: 2, xs: 4, s: 8, m: 12, l: 16, xl: 20, xxl: 24, xxxl: 32 };
export const typeScale = { caption: 11, body: 13, callout: 12, headline: 15, title: 17, largeTitle: 22 };
export const controlSize = { regular: 28, small: 22, toolbar: 44, radius: 7, radiusSmall: 6, groupRadius: 10 };

// Is a point on the trailing half of the window? Tooltips and menus open toward the inside.
function nearEnd(winId, node) {
  try {
    const [x, , w] = lord.getLayout(node.id);
    const f = wm.get(winId);
    const mid = f ? f.x + f.w / 2 : 640;
    const end = x + w / 2 > mid;
    return direction() === 'rtl' ? !end : end;
  } catch (e) { return false; }
}

// Hover tooltip (shown after a short delay), for icon-only toolbar items.
function useTooltip(title) {
  const [show, setShow] = useState(false);
  const timer = useRef(0);
  useEffect(() => () => clearTimeout(timer.current), []);
  const enter = () => { if (!title) return; clearTimeout(timer.current); timer.current = setTimeout(() => setShow(true), 550); };
  const leave = () => { clearTimeout(timer.current); setShow(false); };
  return [show, enter, leave];
}

function TooltipBubble({ text, end, top }) {
  const t = useTheme();
  return (
    <View style={{ position: 'absolute', top, [end ? 'right' : 'left']: 0, zIndex: 1000, pointerEvents: 'none',
      paddingHorizontal: 8, height: 22, borderRadius: Math.max(2, t.menuRadius - 2), justifyContent: 'center',
      backgroundColor: t.popupBg || (t.mode === 'dark' ? '#3a3a40' : '#fbfbfd'), border: `1px solid ${t.separator}`, boxShadow: '0 4px 12px rgba(0,0,0,0.25)' }}>
      <Text style={{ fontSize: 11, color: t.text, whiteSpace: 'nowrap' }}>{text}</Text>
    </View>
  );
}

// Toolbar item: an icon (a glyph, or an element such as a drawn shape) and/or a short label,
// borderless until hovered.
// title: tooltip text (required for icon-only buttons). active: a toggled-on state.
export function ToolbarButton({ icon, label, title, onClick, active, disabled, small, iconSize, color, style }) {
  const t = useTheme();
  const { id: winId } = useAppWindow();
  const ref = useRef(null);
  const [hover, setHover] = useState(false);
  const [tip, tipEnter, tipLeave] = useTooltip(title);
  const h = small ? controlSize.small : controlSize.regular;
  const fg = disabled ? t.textDim : active ? t.accent : color || t.text;
  return (
    <View ref={ref} onClick={() => { tipLeave(); if (!disabled) onClick?.(); }}
      onMouseEnter={() => { setHover(true); tipEnter(); }} onMouseLeave={() => { setHover(false); tipLeave(); }}
      style={[{ height: h, minWidth: h, paddingHorizontal: label ? 8 : 0, borderRadius: t.controlRadiusSmall, flexDirection: 'row', gap: 6,
        alignItems: 'center', justifyContent: 'center', opacity: disabled ? 0.45 : 1, zIndex: tip ? 1000 : 0,
        backgroundColor: active ? (t.mode === 'dark' ? 'rgba(255,255,255,0.12)' : 'rgba(0,0,0,0.08)') : hover && !disabled ? t.hover : 'transparent' }, style]}>
      {icon == null ? null : typeof icon === 'string' || typeof icon === 'number'
        ? <Text style={{ fontSize: iconSize || (small ? 13 : 15), color: fg, textAlign: 'center' }}>{icon}</Text>
        : icon /* an element: a drawn glyph */}
      {label ? <Text style={{ fontSize: small ? 12 : 13, fontWeight: 500, color: fg, whiteSpace: 'nowrap' }}>{label}</Text> : null}
      {/* in the tooltip layer (sdk/layers.js): zIndex only orders siblings, so a bubble drawn in place went
          under whatever the window draws after the toolbar (the picture in Photos, issue #72) */}
      {tip && title ? <Overlay anchor={ref} layer="tooltip"><TooltipBubble text={title} end={nearEnd(winId, ref.current)} top={h + 6} /></Overlay> : null}
    </View>
  );
}

// Buttons that belong together (zoom −/+, markup tools): one rounded track.
export function ToolbarGroup({ children, style }) {
  const t = useTheme();
  return <View style={[{ flexDirection: 'row', alignItems: 'center', gap: 2, padding: 2, borderRadius: t.controlRadius + 1, backgroundColor: t.control }, style]}>{children}</View>;
}

// The app's toolbar row under the title bar: 44 px, 12 px side insets, 8 px between items and a
// hairline at the bottom.
export function Toolbar({ children, style, separator = true }) {
  const t = useTheme();
  return (
    <View style={[{ height: controlSize.toolbar, flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingHorizontal: spacing.m, zIndex: 10 }, style]}>
      {separator ? <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 1, backgroundColor: t.separator }} /> : null}
      {children}
    </View>
  );
}

export const ToolbarSpacer = () => <View style={{ flex: 1 }} />;

// 1 px hairline on one edge of its parent (absolutely positioned). edge: 'top' | 'bottom' |
// 'start' | 'end'; start and end follow the UI direction (the engine's left/right are physical).
// inset: space left at both ends of the line.
export function Hairline({ edge = 'bottom', inset = 0 }) {
  const t = useTheme();
  const rtl = direction() === 'rtl';
  const side = edge === 'start' ? (rtl ? 'right' : 'left') : edge === 'end' ? (rtl ? 'left' : 'right') : edge;
  const box = side === 'top' || side === 'bottom'
    ? { left: inset, right: inset, [side]: 0, height: 1 } : { top: inset, bottom: inset, [side]: 0, width: 1 };
  return <View style={{ position: 'absolute', backgroundColor: t.separator, pointerEvents: 'none', ...box }} />;
}

// Thin vertical divider between toolbar groups.
export function ToolbarDivider() {
  const t = useTheme();
  return <View style={{ width: 1, height: 18, marginHorizontal: 2, backgroundColor: t.separator }} />;
}

// "More" button that opens a small menu: items = [{ label, onClick, checked, disabled, destructive, detail } | '-'].
// Optional (0.8.8.4): detail (a second, dimmer line under an item's label), onOpenChange(open) (a card that shows the
// button only on hover keeps it while its menu is open) and trigger(open) (draws the button itself: a pop-up field in a
// form) with minWidth for the menu.
export function MenuButton({ icon = '⋯', label, title, items, small, onOpenChange, trigger, minWidth = 200 }) {
  const t = useTheme();
  const { id: winId } = useAppWindow();
  const [open, setOpen] = useState(false);
  const was = useRef(false);
  useEffect(() => { if (was.current !== open) { was.current = open; onOpenChange?.(open); } }, [open]);
  const btn = useRef(null), menu = useRef(null);
  useEffect(() => {
    if (!open) return undefined;
    return addListener('pointerdown', (e) => {
      const inside = (n) => { if (!n) return false; const [x, y, w, h] = lord.getLayout(n.id); return e.x >= x && e.x <= x + w && e.y >= y && e.y <= y + h; };
      if (!inside(menu.current) && !inside(btn.current)) setOpen(false);
    });
  }, [open]);
  const end = open && nearEnd(winId, btn.current);
  // the menu goes in the overlay layer (sdk/layers.js): above the rest of the window, whatever
  // comes after the button, and not clipped by a scrolling parent
  return (
    <View ref={btn}>
      {trigger ? <View onClick={() => setOpen((o) => !o)}>{trigger(open)}</View>
        : <ToolbarButton icon={icon} label={label} title={open ? null : title} small={small} active={open} onClick={() => setOpen((o) => !o)} />}
      {open && (
        <Overlay anchor={btn}>
          <View ref={menu} style={{ position: 'absolute', top: (trigger ? lord.getLayout(btn.current.id)[3] : small ? controlSize.small : controlSize.regular) + 4, [end ? 'right' : 'left']: 0,
            minWidth, paddingVertical: 5, paddingHorizontal: 5, borderRadius: t.menuRadius, backgroundColor: popupBg(t),
            border: `1px solid ${t.separator}`, boxShadow: '0 10px 30px rgba(0,0,0,0.35)' }}>
            {items.map((it, i) => (it === '-' ? <View key={`s${i}`} style={{ height: 1, marginVertical: 4, marginHorizontal: 6, backgroundColor: t.separator }} />
              : <MenuRow key={it.label} it={it} onDone={() => setOpen(false)} />))}
          </View>
        </Overlay>
      )}
    </View>
  );
}

function MenuRow({ it, onDone }) {
  const t = useTheme();
  const [hover, setHover] = useState(false);
  const lit = hover && !it.disabled;
  const on = lit && t.menuHighlight !== 'subtle';     // 'subtle' (a theme token): a light fill, the text keeps its color
  return (
    <View onClick={() => { if (it.disabled) return; onDone(); it.onClick?.(); }} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: it.detail ? 40 : 24, paddingHorizontal: 8, borderRadius: t.menuItemRadius, flexDirection: 'row', alignItems: 'center', gap: 6,
        backgroundColor: on ? t.accent : lit ? t.hover : 'transparent', opacity: it.disabled ? 0.4 : 1 }}>
      <Text style={{ width: 12, fontSize: 11, color: on ? '#fff' : t.text }}>{it.checked ? '✓' : ''}</Text>
      {it.detail ? (
        <View style={{ flex: 1, gap: 1 }}>
          <Text style={{ fontSize: 13, whiteSpace: 'nowrap', color: on ? '#fff' : t.text }}>{it.label}</Text>
          <Text style={{ fontSize: 11, whiteSpace: 'nowrap', color: on ? 'rgba(255,255,255,0.8)' : t.textDim }}>{it.detail}</Text>
        </View>
      ) : <Text style={{ flex: 1, fontSize: 13, whiteSpace: 'nowrap', color: on ? '#fff' : it.destructive ? '#ff453a' : t.text }}>{it.label}</Text>}
    </View>
  );
}

// Search field: a TextInput with a magnifier and a clear button.
export function SearchField({ value, onChange, placeholder, onSubmit, onCancel, onKeyDown, autoFocus, width, style }) {
  const t = useTheme();
  // the magnifier sits at the leading edge and the clear button at the trailing one
  const rtl = direction() === 'rtl';
  const lead = rtl ? 'right' : 'left', trail = rtl ? 'left' : 'right';
  return (
    <View style={[{ width, flexDirection: 'row', alignItems: 'center' }, style]}>
      <TextInput value={value} onChange={onChange} placeholder={placeholder} onSubmit={onSubmit} onKeyDown={onKeyDown} autoFocus={autoFocus}
        onCancel={() => { if (value) onChange(''); onCancel?.(); }}
        style={{ flex: 1, [rtl ? 'paddingRight' : 'paddingLeft']: 26, [rtl ? 'paddingLeft' : 'paddingRight']: value ? 26 : 10 }} />
      <Text style={{ position: 'absolute', [lead]: 9, top: 5, fontSize: 13, color: t.textDim, pointerEvents: 'none' }}>⌕</Text>
      {value ? (
        <View onClick={() => onChange('')} style={{ position: 'absolute', [trail]: 6, top: 6, width: 16, height: 16, borderRadius: 8,
          alignItems: 'center', justifyContent: 'center', backgroundColor: t.textDim }}>
          <Text style={{ fontSize: 10, fontWeight: 700, color: t.mode === 'dark' ? '#1f1f23' : '#fff' }}>✕</Text>
        </View>
      ) : null}
    </View>
  );
}

// Empty state: an icon (a glyph or an element), a short title and one hint line, centered.
// mascot: a Lumo pose ('confused' for errors and unavailable services, 'celebrate' for "all done",
// 'shield' for security, 'wave' for welcome) drawn instead of the icon. Use it only where the state
// is an outcome worth a little warmth, not for "nothing selected" panes (docs/DESIGN.md).
export function EmptyState({ icon, mascot, title, hint, action, style }) {
  const t = useTheme();
  return (
    <View style={[{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: spacing.s, padding: spacing.xxl }, style]}>
      {mascot ? <Lumo pose={mascot} size={112} style={{ marginBottom: spacing.xs }} />
        : typeof icon === 'string' ? <Text style={{ fontSize: 40, color: t.textDim, marginBottom: spacing.xs }}>{icon}</Text> : icon}
      {title ? <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'center' }}>{title}</Text> : null}
      {hint ? <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'center', maxWidth: 360, lineHeight: 1.4 }}>{hint}</Text> : null}
      {action ? <View style={{ marginTop: spacing.s }}>{action}</View> : null}
    </View>
  );
}

// Grouped ("inset") section, as in Settings: an optional caption above a rounded box whose rows
// are separated by hairlines. Children are FormRow (or any element).
export function GroupBox({ title, children, footer, style }) {
  const t = useTheme();
  const rows = (Array.isArray(children) ? children.flat() : [children]).filter(Boolean);
  const box = t.mode === 'dark' ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.025)';
  return (
    <View style={[{ gap: 6 }, style]}>
      {title ? <Text style={{ fontSize: typeScale.caption, fontWeight: 600, color: t.textDim, marginLeft: spacing.s }}>{title}</Text> : null}
      <View style={{ borderRadius: t.groupRadius, backgroundColor: box, border: `1px solid ${t.separator}`, paddingHorizontal: spacing.m }}>
        {rows.map((r, i) => (
          <View key={i}>
            {i > 0 ? <View style={{ height: 1, backgroundColor: t.separator }} /> : null}
            {r}
          </View>
        ))}
      </View>
      {footer ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, marginLeft: spacing.s }}>{footer}</Text> : null}
    </View>
  );
}

// A row of a GroupBox: label at the start, value or control at the end (labelWidth: a right-aligned
// label column, as in classic desktop forms).
export function FormRow({ label, children, labelWidth, minHeight = 36 }) {
  const t = useTheme();
  const value = typeof children === 'string' || typeof children === 'number'
    ? <Text style={{ fontSize: typeScale.body, color: t.text }}>{String(children)}</Text> : children;
  return (
    <View style={{ minHeight, flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: 6 }}>
      <Text style={labelWidth ? { width: labelWidth, fontSize: typeScale.body, color: t.textDim, textAlign: 'end' }
        : { fontSize: typeScale.body, color: t.text }}>{label}</Text>
      <View style={{ flex: 1, alignItems: labelWidth ? 'flex-start' : 'flex-end' }}>{value}</View>
    </View>
  );
}

// A confirmation in the shell (the Dock's «Empty Trash»): a dimmed screen with a small panel,
// never alert(). Escape cancels and Enter confirms. destructive: the button is red.
export function ConfirmDialog({ title, detail, okLabel, cancelLabel, onOk, onCancel, destructive, area }) {
  const t = useTheme();
  useEffect(() => addListener('keydown', (e) => {
    if (e.key === 'Escape') onCancel();
    else if (e.key === 'Enter') onOk();
  }), [onOk, onCancel]);
  const a = area || { x: 0, y: 0, w: undefined, h: undefined };
  return (
    <View onClick={onCancel} style={{ position: 'absolute', left: a.x, top: a.y, ...(a.w ? { width: a.w, height: a.h } : { right: 0, bottom: 0 }),
      backgroundColor: 'rgba(0,0,0,0.3)', alignItems: 'center', justifyContent: 'center' }}>
      <View onClick={(e) => e.stopPropagation()} style={{ width: 360, padding: spacing.xl, gap: spacing.s, borderRadius: t.groupRadius + 2, backgroundColor: t.windowBg,
        border: `1px solid ${t.separator}`, boxShadow: '0 14px 36px rgba(0,0,0,0.45)' }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{title}</Text>
        {detail ? <Text style={{ fontSize: typeScale.body, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{detail}</Text> : null}
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s, marginTop: spacing.m }}>
          <DialogButton t={t} label={cancelLabel} onClick={onCancel} />
          <DialogButton t={t} label={okLabel} onClick={onOk} primary destructive={destructive} />
        </View>
      </View>
    </View>
  );
}
function DialogButton({ t, label, onClick, primary, destructive }) {
  const [hover, setHover] = useState(false);
  const bg = primary ? (destructive ? '#e5483f' : t.accent) : t.control;
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: controlSize.regular, paddingHorizontal: 14, borderRadius: t.controlRadius, justifyContent: 'center', alignItems: 'center',
        backgroundColor: bg, opacity: hover ? 0.85 : 1, ...(t.buttonBorder && !primary ? { border: `1px solid ${t.separator}` } : null) }}>
      <Text style={{ fontSize: typeScale.body, fontWeight: 500, color: primary ? '#fff' : t.text, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}
