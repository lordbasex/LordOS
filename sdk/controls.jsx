// controls.jsx — shared building blocks for the layout of LordOS apps: toolbar buttons
// with tooltips, toolbars, a "more" menu button, a search field, empty states and grouped
// ("inset") forms; and (0.8.8.8) the choice controls with their keys built in: Dropdown, Checkbox,
// RadioGroup, Tabs, Popover, Tooltip and ProgressBar (their rules are in sdk/controlmodel.js; List
// and Table are in sdk/lists.jsx). The spacing and type scale they follow is in docs/DESIGN.md.
import { View, Text, addListener } from '@lordos/runtime';
import { useEffect, useRef, useState } from 'react';
import { useTheme, popupBg } from './theme.js';
import { TextInput, ScrollView, useAppWindow, hasTextFocus, clearTextFocus } from './components.jsx';
import { wm } from './windows.js';
import { useStore } from './store.js';
import { dialogStack, dialogKey, defaultButton, keyOwner } from './dialogkeys.js';
import { direction, t as tr } from './i18n.js';
import { Lumo } from './mascot.js';
import { Overlay, shellLayers } from './layers.js';
import { menuStep, within } from './focusmodel.js';
import { focusRingColor, useFocusScope, keyFocused, onFocusMove } from './focus.js';
import { groupKey, dropdownKey, tipSide, progressSpan } from './controlmodel.js';
import { typeAhead, matchIndex, TYPE_MS } from './menunav.js';
import { findChar } from './listmodel.js';

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

// A MenuButton's menu opens upward when it wouldn't fit between the button and the window's bottom edge (a button in a
// footer, near the bottom of the window) and there is more room above: the height is the rows' own (24, 40 with a
// detail line, 9 a separator) plus the menu's padding and border.
export const menuHeight = (items) => (items || []).reduce((h, it) => h + (it === '-' ? 9 : it && it.detail ? 40 : 24), 12);
export function opensUp(btn, win, height) {
  if (!btn || !win || !(win.h > 0)) return false;
  const below = win.y + win.h - (btn.y + btn.h + 4), above = btn.y - win.y - 4;
  return below < height && above > below;
}
function nearBottom(winId, node, items) {
  try {
    const [x, y, w, h] = lord.getLayout(node.id);
    return opensUp({ x, y, w, h }, wm.get(winId), menuHeight(items));
  } catch (e) { return false; }
}

// Hover tooltip (shown after a short delay), for icon-only toolbar items; the keyboard focus shows it too.
function useTooltip(title) {
  const [show, setShow] = useState(false);
  const timer = useRef(0);
  useEffect(() => () => clearTimeout(timer.current), []);
  const enter = () => { if (!title) return; clearTimeout(timer.current); timer.current = setTimeout(() => setShow(true), 550); };
  const leave = () => { clearTimeout(timer.current); setShow(false); };
  return [show, enter, leave];
}

// top (below the anchor) or bottom (above it): the distance from the anchor's top or bottom edge
function TooltipBubble({ text, end, top, bottom }) {
  const t = useTheme();
  return (
    <View style={{ position: 'absolute', ...(bottom != null ? { bottom } : { top }), [end ? 'right' : 'left']: 0, zIndex: 1000, pointerEvents: 'none',
      paddingHorizontal: 8, height: 22, borderRadius: Math.max(2, t.menuRadius - 2), justifyContent: 'center',
      backgroundColor: t.popupBg || (t.mode === 'dark' ? '#3a3a40' : '#fbfbfd'), border: `1px solid ${t.separator}`, boxShadow: '0 4px 12px rgba(0,0,0,0.25)' }}>
      <Text style={{ fontSize: 11, color: t.text, whiteSpace: 'nowrap' }}>{text}</Text>
    </View>
  );
}

// Toolbar item: an icon (a glyph, or an element such as a drawn shape) and/or a short label,
// borderless until hovered.
// title: tooltip text (required for icon-only buttons). active: a toggled-on state.
// A Tab stop unless disabled (sdk/focus.js): Space and Enter press it. onKeyDown (optional) sees its keys first.
export function ToolbarButton({ icon, label, title, onClick, active, disabled, small, iconSize, color, style, onKeyDown, focusable = true }) {
  const t = useTheme();
  const { id: winId } = useAppWindow();
  const ref = useRef(null);
  const [hover, setHover] = useState(false);
  const [tip, tipEnter, tipLeave] = useTooltip(title);
  const h = small ? controlSize.small : controlSize.regular;
  const fg = disabled ? t.textDim : active ? t.accent : color || t.text;
  return (
    <View ref={ref} onClick={() => { tipLeave(); if (!disabled) onClick?.(); }} focusable={focusable && !disabled} onKeyDown={onKeyDown}
      onFocus={tipEnter} onBlur={tipLeave} onMouseEnter={() => { setHover(true); tipEnter(); }} onMouseLeave={() => { setHover(false); tipLeave(); }}
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
// From the keyboard (sdk/focus.js): Space, Enter or ↓ on the button opens it; while it is open ↓ and ↑ go through its
// items, Enter or Space chooses one, Escape and Tab close it.
export function MenuButton({ icon = '⋯', label, title, items, small, onOpenChange, trigger, minWidth = 200 }) {
  const t = useTheme();
  const { id: winId } = useAppWindow();
  const [open, setOpen] = useState(false);
  const [lit, setLit] = useState(-1);            // the item the keyboard is on
  useEffect(() => { if (!open) setLit(-1); }, [open]);
  const skip = (i) => !items[i] || items[i] === '-' || items[i].disabled;
  const keys = useRef(null);
  keys.current = (e) => {
    if (e.ctrl || e.meta || e.alt) return false;
    if (!open) {
      if (e.key !== 'ArrowDown') return false;
      setOpen(true); setLit(menuStep(items.length, -1, 1, skip));
      return true;
    }
    if (e.key === 'Escape') { setOpen(false); return true; }
    if (e.key === 'Tab') { setOpen(false); return false; }
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { setLit((i) => menuStep(items.length, i, e.key === 'ArrowDown' ? 1 : -1, skip)); return true; }
    if (e.key === 'Enter' || e.key === ' ') {
      const it = lit >= 0 && !skip(lit) ? items[lit] : null;
      setOpen(false);
      it?.onClick?.();
      return true;
    }
    return false;
  };
  const onKey = (e) => keys.current(e);
  // opened with the pointer: the keys reach it as a listener (with the keyboard focus on the button they come first)
  useEffect(() => {
    if (!open) return undefined;
    return addListener('keydown', (e) => { if (keys.current(e)) e.preventDefault?.(); });
  }, [open]);
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
  const up = open && nearBottom(winId, btn.current, items);
  // the menu goes in the overlay layer (sdk/layers.js): above the rest of the window, whatever
  // comes after the button, and not clipped by a scrolling parent
  return (
    <View ref={btn}>
      {trigger ? <View onClick={() => setOpen((o) => !o)} focusable onKeyDown={onKey}>{trigger(open)}</View>
        : <ToolbarButton icon={icon} label={label} title={open ? null : title} small={small} active={open} onClick={() => setOpen((o) => !o)} onKeyDown={onKey} />}
      {open && (
        <Overlay anchor={btn}>
          <View ref={menu} style={{ position: 'absolute', [up ? 'bottom' : 'top']: (trigger ? lord.getLayout(btn.current.id)[3] : small ? controlSize.small : controlSize.regular) + 4, [end ? 'right' : 'left']: 0,
            minWidth, paddingVertical: 5, paddingHorizontal: 5, borderRadius: t.menuRadius, backgroundColor: popupBg(t),
            border: `1px solid ${t.separator}`, boxShadow: '0 10px 30px rgba(0,0,0,0.35)' }}>
            {items.map((it, i) => (it === '-' ? <View key={`s${i}`} style={{ height: 1, marginVertical: 4, marginHorizontal: 6, backgroundColor: t.separator }} />
              : <MenuRow key={it.label} it={it} lit={lit === i} onHover={() => setLit(-1)} onDone={() => setOpen(false)} />))}
          </View>
        </Overlay>
      )}
    </View>
  );
}

// lit: the keyboard is on it (as a hover); onHover: the pointer came onto it
function MenuRow({ it, onDone, lit: keyed, onHover }) {
  const t = useTheme();
  const [hover, setHover] = useState(false);
  const lit = (hover || keyed) && !it.disabled;
  const on = lit && t.menuHighlight !== 'subtle';     // 'subtle' (a theme token): a light fill, the text keeps its color
  return (
    <View onClick={() => { if (it.disabled) return; onDone(); it.onClick?.(); }} onMouseEnter={() => { setHover(true); onHover?.(); }} onMouseLeave={() => setHover(false)}
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

// A confirmation in the shell (the Dock's «Empty Trash») or in a window: a dimmed area with a small
// panel, never alert(). It takes the keys only while it is in front, that is in the focused window
// (or the desktop's own) and the newest dialog open (sdk/dialogkeys.js): Escape cancels and Enter
// does the default. destructive: the button is red and the default is Cancel, as in libadwaita, so
// the action needs a click, or Tab to it and then Space. cautious: the default is Cancel too, with a button
// that isn't red (an action that only needs a second look, such as pasting files another app copied).
// children: an option shown under the detail (Settings → Web apps' "Delete its data" toggle).
export function ConfirmDialog({ title, detail, okLabel, cancelLabel, onOk, onCancel, destructive, cautious, area, children }) {
  const t = useTheme();
  const { id: win, focused } = useAppWindow();
  const token = useRef({}).current;
  const safe = !!(destructive || cautious);         // Enter presses Cancel
  const dflt = defaultButton(safe);
  const [focus, setFocus] = useState(dflt);         // the button Space presses; Tab moves it
  const owner = useStore(dialogStack, (list) => keyOwner(list) === token);
  const latest = useRef(null);
  latest.current = { onOk, onCancel, destructive: safe, focus };
  useEffect(() => {
    dialogStack.open(token, win, focused);
    if (dialogStack.owner() === token) clearTextFocus();   // a field under it must not go on typing
    return () => dialogStack.close(token);
  }, []);
  useEffect(() => { dialogStack.setFocused(token, focused); }, [focused]);
  useEffect(() => addListener('keydown', (e) => {
    if (dialogStack.owner() !== token || hasTextFocus()) return;
    const p = latest.current;
    const r = dialogKey(e, p);
    if (r) dialogStack.take(e);
    if (r?.focus) setFocus(r.focus);
    else if (r?.press === 'ok') p.onOk();
    else if (r?.press === 'cancel') p.onCancel();
  }), []);
  const a = area || { x: 0, y: 0, w: undefined, h: undefined };
  return (
    <View onClick={onCancel} style={{ position: 'absolute', left: a.x, top: a.y, ...(a.w ? { width: a.w, height: a.h } : { right: 0, bottom: 0 }),
      backgroundColor: 'rgba(0,0,0,0.3)', alignItems: 'center', justifyContent: 'center' }}>
      <View onClick={(e) => e.stopPropagation()} style={{ width: 360, padding: spacing.xl, gap: spacing.s, borderRadius: t.groupRadius + 2, backgroundColor: t.windowBg,
        border: `1px solid ${t.separator}`, boxShadow: '0 14px 36px rgba(0,0,0,0.45)' }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{title}</Text>
        {detail ? <Text style={{ fontSize: typeScale.body, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{detail}</Text> : null}
        {children || null}
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s, marginTop: spacing.m }}>
          <DialogButton t={t} label={cancelLabel} onClick={onCancel} isDefault={dflt === 'cancel'} ring={owner && focus === 'cancel'} />
          <DialogButton t={t} label={okLabel} onClick={onOk} primary destructive={destructive} isDefault={dflt === 'ok'} ring={owner && focus === 'ok'} />
        </View>
      </View>
    </View>
  );
}
// isDefault: the button Enter presses, with an accent edge and a heavier label (a filled accent
// button already reads as the default); ring: the keyboard focus, drawn while the dialog has the keys, as the
// focus ring of the other controls (sdk/focus.js: 2 px, 1 px outside the button, the theme's focusRing color)
function DialogButton({ t, label, onClick, primary, destructive, isDefault, ring }) {
  const [hover, setHover] = useState(false);
  const bg = primary ? (destructive ? '#e5483f' : t.accent) : t.control;
  const edge = primary ? null : isDefault ? { border: `1px solid ${t.accent}` } : t.buttonBorder ? { border: `1px solid ${t.separator}` } : null;
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: controlSize.regular, paddingHorizontal: 14, borderRadius: t.controlRadius, justifyContent: 'center', alignItems: 'center',
        backgroundColor: bg, opacity: hover ? 0.85 : 1, ...edge }}>
      <Text style={{ fontSize: typeScale.body, fontWeight: isDefault ? 600 : 500, color: primary ? '#fff' : t.text, whiteSpace: 'nowrap' }}>{label}</Text>
      {ring ? <View style={{ position: 'absolute', left: -3, top: -3, right: -3, bottom: -3, borderRadius: t.controlRadius + 3, pointerEvents: 'none',
        border: `2px solid ${focusRingColor(t)}` }} /> : null}
    </View>
  );
}

// ---- choice controls with their keys built in (0.8.8.8, docs/DESKTOP-PARITY.md) ----

const boxOf = (n) => { try { const [x, y, w, h] = lord.getLayout(n.id); return { x, y, w, h }; } catch (e) { return null; } };
const refNode = (r) => (r && typeof r === 'object' && 'current' in r ? r.current : r);
// The area a popup stays inside: the window's frame (the window's own surface in an isolated app's process), or the
// screen when there is none.
function areaOf(win) {
  const f = shellLayers() ? wm.get(win?.id) : win?.overlayHost ? boxOf(win.overlayHost) : null;
  if (f && f.w > 0 && f.h > 0) return { x: f.x, y: f.y, w: f.w, h: f.h };
  const sc = typeof lord !== 'undefined' && lord.screen ? lord.screen : { width: 1280, height: 800 };
  return { x: 0, y: 0, w: sc.width, h: sc.height };
}

// The pointer's last place on the screen, for a tooltip shown by the keyboard focus (it must not cover it).
let pointerAt = null;
let pointerWatch = null;
const watchPointer = () => { if (!pointerWatch) pointerWatch = addListener('pointermove', (e) => { pointerAt = { x: e.x, y: e.y }; }); };

// The popups open (a Popover, a Dropdown's list), oldest first. A press outside one closes it, unless it lands in one
// opened after it (a Dropdown or a date picker inside a Popover) or on that one's anchor; Escape closes only the
// newest, so a list open inside a popover closes first.
const popups = [];
const hitIn = (n, e) => { const r = n && boxOf(n); return !!r && e.x >= r.x && e.x < r.x + r.w && e.y >= r.y && e.y < r.y + r.h; };
function usePopup(open, box, anchor, onClose) {
  const close = useRef(onClose);
  close.current = onClose;
  useEffect(() => {
    if (!open) return undefined;
    const me = { box, anchor };
    popups.push(me);
    const offPtr = addListener('pointerdown', (e) => {
      const i = popups.indexOf(me);
      if (i < 0) return;
      for (const p of popups.slice(i)) if (hitIn(p.box.current, e) || hitIn(refNode(p.anchor), e)) return;
      close.current?.();
    });
    const offKey = addListener('keydown', (e) => { if (e.key === 'Escape' && popups[popups.length - 1] === me) close.current?.(); });
    return () => { offPtr(); offKey(); const i = popups.indexOf(me); if (i >= 0) popups.splice(i, 1); };
  }, [open]);
}

// A popover opened from a control: anchor (a ref to it), onClose (a press outside it, or Escape), width (240),
// padding (6). Below the anchor at its start edge, above it when there is no room below in the window, and moved
// sideways to stay inside the window; drawn in the popover layer (sdk/layers.js) above the whole window. A focus
// scope (sdk/focus.js): opened from the keyboard the focus moves to its first control and Tab goes round its own;
// Escape closes it and the focus goes back to the control it was opened from.
export function Popover({ anchor, onClose, width = 240, padding = 6, children, style }) {
  const t = useTheme();
  const win = useAppWindow();
  const box = useRef(null);
  const [up, setUp] = useState(false);
  const [, force] = useState(0);
  useFocusScope(box);
  usePopup(true, box, anchor, onClose);
  const a = boxOf(refNode(anchor));
  const area = areaOf(win);
  useEffect(() => {
    if (!a) { const id = setTimeout(() => force((v) => v + 1), 16); return () => clearTimeout(id); }
    const b = box.current && boxOf(box.current);
    if (b) { const want = opensUp(a, area, b.h + 4); if (want !== up) setUp(want); }
    return undefined;
  });
  if (!a) return null;
  const w = Math.max(0, Math.min(width, area.w - 16));
  const rtl = direction() === 'rtl';
  const x = Math.max(area.x + 8, Math.min(area.x + area.w - w - 8, rtl ? a.x + a.w - w : a.x));
  return (
    <Overlay anchor={anchor}>
      <View ref={box} style={[{ position: 'absolute', left: x - a.x, [up ? 'bottom' : 'top']: a.h + 4, width: w, padding,
        borderRadius: t.menuRadius + 2, backgroundColor: popupBg(t), border: `1px solid ${t.separator}`, boxShadow: '0 12px 32px rgba(0,0,0,0.32)' }, style]}>
        {children}
      </View>
    </Overlay>
  );
}

// A tooltip for any control: text, shown after delay ms while the pointer is over children or the keyboard focus is
// on a control inside them. It never covers the pointer: below the control, or above it near the window's bottom
// edge or where the pointer is (sdk/controlmodel.js tipSide). In the tooltip layer, it takes no clicks.
export function Tooltip({ text, children, delay = 550, style }) {
  const win = useAppWindow();
  const ref = useRef(null);
  const timer = useRef(0);
  const why = useRef({ hover: false, focus: false });
  const [show, setShow] = useState(false);
  const update = () => {
    clearTimeout(timer.current);
    if (!text || !(why.current.hover || why.current.focus)) { setShow(false); return; }
    timer.current = setTimeout(() => setShow(true), delay);
  };
  useEffect(() => {
    watchPointer();
    const off = onFocusMove(() => {
      const n = keyFocused();
      const f = !!n && !!ref.current && within(n, ref.current);
      if (f !== why.current.focus) { why.current.focus = f; update(); }
    });
    return () => { off(); clearTimeout(timer.current); };
  }, []);
  useEffect(() => { if (!text) setShow(false); }, [text]);
  let place = null;
  if (show && ref.current) {
    const a = boxOf(ref.current);
    if (a) {
      const size = { w: (typeof lord !== 'undefined' && lord.measureText ? lord.measureText(String(text), 11, 400) : String(text).length * 6) + 18, h: 22 };
      place = { side: tipSide(a, size, areaOf(win), why.current.hover ? null : pointerAt), end: nearEnd(win.id, ref.current), h: a.h };
    }
  }
  return (
    <View ref={ref} style={style} onMouseEnter={() => { why.current.hover = true; update(); }}
      onMouseLeave={() => { why.current.hover = false; update(); }} onMouseDown={() => { why.current.hover = false; update(); }}>
      {children}
      {place ? (
        <Overlay anchor={ref} layer="tooltip">
          <TooltipBubble text={text} end={place.end} {...(place.side === 'above' ? { bottom: place.h + 6 } : { top: place.h + 6 })} />
        </Overlay>
      ) : null}
    </View>
  );
}

// The rows of a Dropdown's list: 24 px each, up to 12 before it scrolls.
const DROP_ROW = 24, DROP_ROWS = 12;

// A drop-down choice (a pop-up button): value, options = [[value, label]], onChange(value). width (180), placeholder
// (shown when no option has the value), small, disabled. A Tab stop: Space, Enter, ↓ and ↑ open it on the current
// choice; then ↓ ↑, Home and End move, Enter or Space chooses, Escape closes it and Tab closes it and goes on.
// Typing a name (closed or open) goes to the next choice that starts with it.
export function Dropdown({ value, options, onChange, width = 180, placeholder, small, disabled, focusable = true }) {
  const t = useTheme();
  const { id: winId } = useAppWindow();
  const btn = useRef(null), list = useRef(null), scroll = useRef(null);
  const [open, setOpen] = useState(false);
  const [lit, setLit] = useState(-1);
  const typed = useRef(null);
  const current = options.findIndex((o) => o[0] === value);
  const latest = useRef(null);
  latest.current = { open, lit, current, options, onChange, value };
  const choose = (i) => {
    const L = latest.current, o = L.options[i];
    setOpen(false);
    if (o && o[0] !== L.value) L.onChange(o[0]);
  };
  const keys = (e) => {
    const L = latest.current;
    const now = Date.now();
    const searching = !!typed.current && now - typed.current.at < TYPE_MS;
    const ch = findChar(e, searching);
    if (ch) {
      typed.current = typeAhead(searching ? typed.current : null, ch, now);
      const i = matchIndex(L.options.map((o) => ({ label: String(o[1] ?? '') })), L.open ? L.lit : L.current, typed.current.buf);
      if (i >= 0) { if (L.open) setLit(i); else choose(i); }
      return true;
    }
    const r = dropdownKey(e, { open: L.open, lit: L.lit, current: L.current, count: L.options.length });
    if (!r) return false;
    typed.current = null;
    if (r.choose != null) { choose(r.choose); return true; }
    setOpen(r.open);
    if (r.lit != null) setLit(r.lit);
    return !r.pass;
  };
  // opened with the pointer: the keys reach it as a listener (with the keyboard focus on it they come first)
  useEffect(() => {
    if (!open) { setLit(-1); return undefined; }
    return addListener('keydown', (e) => { if (keys(e)) e.preventDefault?.(); });
  }, [open]);
  usePopup(open, list, btn, () => setOpen(false));
  // the lit row comes into sight in a long list
  useEffect(() => {
    if (open && lit >= 0 && options.length > DROP_ROWS) scroll.current?.scrollTo(Math.max(0, (lit - DROP_ROWS + 3) * DROP_ROW));
  }, [open, lit]);
  const h = small ? controlSize.small : controlSize.regular - 2;
  const cur = current >= 0 ? options[current][1] : placeholder ?? tr('shell.control.choose');
  const end = open && nearEnd(winId, btn.current);
  const rows = options.map(([v, label], i) => {
    const on = lit === i;
    const fill = on && t.menuHighlight !== 'subtle';
    return (
      <View key={String(v)} onClick={() => choose(i)} onMouseEnter={() => setLit(i)}
        style={{ height: DROP_ROW, paddingHorizontal: 8, borderRadius: t.menuItemRadius, flexDirection: 'row', alignItems: 'center', gap: 6,
          backgroundColor: fill ? t.accent : on ? t.hover : 'transparent' }}>
        <Text style={{ width: 12, fontSize: 11, color: fill ? '#fff' : t.text }}>{v === value ? '✓' : ''}</Text>
        <Text style={{ flex: 1, fontSize: 13, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', color: fill ? '#fff' : t.text, textAlign: 'start' }}>{label}</Text>
      </View>
    );
  });
  return (
    <View ref={btn} style={{ width }}>
      <View onClick={disabled ? undefined : () => setOpen((o) => !o)} focusable={focusable && !disabled} onKeyDown={keys}
        style={{ height: h, paddingHorizontal: 9, borderRadius: small ? t.controlRadiusSmall : t.controlRadius, flexDirection: 'row', alignItems: 'center', gap: 6,
          backgroundColor: t.control, opacity: disabled ? 0.45 : 1, ...(t.buttonBorder ? { border: `1px solid ${t.separator}` } : null) }}>
        <Text style={{ flex: 1, fontSize: small ? 12 : 13, color: current >= 0 ? t.text : t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{cur}</Text>
        <Text style={{ fontSize: 9, color: t.textDim }}>▼</Text>
      </View>
      {open && (
        <Overlay anchor={btn}>
          <View ref={list} style={{ position: 'absolute', top: h + 4, [end ? 'right' : 'left']: 0, minWidth: Math.max(width, 160), padding: 5, borderRadius: t.menuRadius,
            backgroundColor: popupBg(t), border: `1px solid ${t.separator}`, boxShadow: '0 10px 30px rgba(0,0,0,0.35)' }}>
            {options.length > DROP_ROWS
              ? <View style={{ height: DROP_ROWS * DROP_ROW }}><ScrollView scrollRef={scroll} focusable={false} style={{ flex: 1 }}>{rows}</ScrollView></View>
              : rows}
          </View>
        </Overlay>
      )}
    </View>
  );
}

// A check box with its label: checked, onChange(checked), label, disabled, mixed (some of what it stands for: a dash).
// A Tab stop: Space (and Enter) flips it, as a click on the box or the label.
export function Checkbox({ checked, onChange, label, disabled, mixed, focusable = true }) {
  const t = useTheme();
  const on = !!checked || !!mixed;
  return (
    <View onClick={disabled ? undefined : () => onChange?.(!checked)} focusable={focusable && !disabled}
      style={{ flexDirection: 'row', alignItems: 'center', gap: 6, opacity: disabled ? 0.45 : 1, borderRadius: 4 }}>
      <View style={{ width: 14, height: 14, borderRadius: 4, alignItems: 'center', justifyContent: 'center', backgroundColor: on ? t.accent : t.control,
        border: `1px solid ${on ? t.accent : t.separator}` }}>
        {on ? <Text style={{ fontSize: 10, fontWeight: 700, color: '#fff', marginTop: -1 }}>{mixed ? '–' : '✓'}</Text> : null}
      </View>
      {label != null && label !== '' ? <Text style={{ fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{label}</Text> : null}
    </View>
  );
}

// A group of radio buttons: value, options = [[value, label, { disabled }?]], onChange(value); row: side by side
// instead of one under the other. One Tab stop: the arrows (← → swapped in right to left) choose the next or the
// previous one, going round and skipping the disabled ones, Home and End the first and the last; Space chooses the
// first when none is chosen yet.
export function RadioGroup({ value, options, onChange, row = false, disabled, focusable = true, style }) {
  const t = useTheme();
  const skip = (i) => !!disabled || !!options[i]?.[2]?.disabled;
  const index = options.findIndex((o) => o[0] === value);
  const onKey = (e) => {
    if (e.key === ' ' && !e.ctrl && !e.meta && !e.alt) {
      if (index < 0) { const i = options.findIndex((o, k) => !skip(k)); if (i >= 0) onChange(options[i][0]); }
      return true;
    }
    const i = groupKey(e, { index, count: options.length, rtl: direction() === 'rtl', vertical: true, skip });
    if (i == null) return false;
    if (i !== index) onChange(options[i][0]);
    return true;
  };
  return (
    <View focusable={focusable && !disabled && options.length > 0} onKeyDown={onKey}
      style={[{ flexDirection: row ? 'row' : 'column', gap: row ? spacing.l : spacing.s, borderRadius: 4 }, style]}>
      {options.map(([v, label], i) => {
        const on = v === value, off = skip(i);
        return (
          <View key={String(v)} onClick={off ? undefined : () => onChange(v)} style={{ flexDirection: 'row', alignItems: 'center', gap: 6, opacity: off ? 0.45 : 1 }}>
            <View style={{ width: 14, height: 14, borderRadius: 7, alignItems: 'center', justifyContent: 'center', backgroundColor: on ? t.accent : t.control,
              border: `1px solid ${on ? t.accent : t.separator}` }}>
              {on ? <View style={{ width: 6, height: 6, borderRadius: 3, backgroundColor: '#fff' }} /> : null}
            </View>
            <Text style={{ fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{label}</Text>
          </View>
        );
      })}
    </View>
  );
}

// A row of tabs over a pane: value, tabs = [[value, label, { disabled }?]], onChange(value). The chosen one is
// underlined in the accent. One Tab stop: ← → choose the next or the previous tab (swapped in right to left, where
// the first one is on the right), going round; Home and End the first and the last.
export function Tabs({ value, tabs, onChange, focusable = true, style }) {
  const t = useTheme();
  const skip = (i) => !!tabs[i]?.[2]?.disabled;
  const index = tabs.findIndex((o) => o[0] === value);
  const onKey = (e) => {
    const i = groupKey(e, { index, count: tabs.length, rtl: direction() === 'rtl', skip });
    if (i == null) return false;
    if (i !== index) onChange(tabs[i][0]);
    return true;
  };
  return (
    <View focusable={focusable && tabs.length > 0} onKeyDown={onKey}
      style={[{ flexDirection: 'row', gap: spacing.xs, borderRadius: t.controlRadiusSmall }, style]}>
      <Hairline edge="bottom" />
      {tabs.map(([v, label], i) => {
        const on = v === value, off = skip(i);
        return (
          <View key={String(v)} onClick={off ? undefined : () => onChange(v)}
            style={{ height: 30, paddingHorizontal: spacing.m, justifyContent: 'center', opacity: off ? 0.45 : 1 }}>
            <Text style={{ fontSize: typeScale.body, fontWeight: on ? 600 : 500, color: on ? t.text : t.textDim, whiteSpace: 'nowrap' }}>{label}</Text>
            {on ? <View style={{ position: 'absolute', left: spacing.s, right: spacing.s, bottom: 0, height: 2, borderRadius: 1, backgroundColor: t.accent }} /> : null}
          </View>
        );
      })}
    </View>
  );
}

// A progress bar: value from 0 to 1, or null (left out) while how much is left is not known, when a band slides
// across it (still, over the whole bar, with reduced motion). It fills from the start edge (the right in right to
// left). width (a number or a percentage; the parent's by default), height (6).
export function ProgressBar({ value, width, height = 6, style }) {
  const t = useTheme();
  const known = value != null && Number.isFinite(value);
  const [ms, setMs] = useState(0);
  useEffect(() => {
    if (known || t.reduceMotion) return undefined;
    const t0 = Date.now();
    const id = setInterval(() => setMs(Date.now() - t0), 40);
    return () => clearInterval(id);
  }, [known, !!t.reduceMotion]);
  const span = progressSpan(known ? value : null, ms, { reduceMotion: !!t.reduceMotion });
  const part = (f) => ({ flexGrow: f, flexShrink: 1, flexBasis: 0 });
  return (
    <View style={[{ width, height, borderRadius: height / 2, backgroundColor: t.control, overflow: 'hidden', flexDirection: 'row' }, style]}>
      <View style={part(span.from)} />
      <View style={{ ...part(span.to - span.from), borderRadius: height / 2, backgroundColor: t.accent, opacity: span.still ? 0.45 : 1 }} />
      <View style={part(1 - span.to)} />
    </View>
  );
}
