// components.jsx — base controls for apps. They take colors from useTheme(), so they
// adapt to any theme.
import { View, Text, addListener, displayFor, onCommit, onScreenChange, capsLock } from '@lordos/runtime';
import { createContext, useContext, useEffect, useRef, useState } from 'react';
import { useTheme } from './theme.js';
import { getApp, windows, openedAt, wm, activeDesktop, onActiveDesktop } from './windows.js';
import { onDesktop } from './desktopmodel.js';
import { useStore } from './store.js';
import { textDirection, direction, t as tr } from './i18n.js';
import { SCROLLBAR, scrollbarMode, thumbGeometry, offsetForThumb, nearEdge, hitThumb, barShown, barOpacity, hideTime, barWidth } from './scrollbar.js';
import { ime, useIme, Preedit } from './ime.js';
import { noteTextField } from './textfocus.js';
import { caretAt, clampPos, editKey, replaceSel, selRange, selectedText, wordAt, newHistory, historyFor, record, undo, redo, canUndo, canRedo, historyKey, lineAt } from './textfieldmodel.js';
import { wrapRows, rowOf, posAt, areaEdit } from './textareamodel.js';
import { primaryStep, pasteAt } from './primarymodel.js';
import { revealAfter, eyeShown, maskedText, eyeLabel, capsAfter, capsHint, capsFits } from './passwordfieldmodel.js';
import { Layer, Overlay, shellLayers } from './layers.js';
import { ContextMenu } from './contextmenu.jsx';
import { dialogStack } from './dialogkeys.js';
import { stat as fsStat, isRemote as fsIsRemote } from './fs.js';
import { dropSorter, dropCount } from './filedrop.js';
import { setTextFocusBridge, useFocusArea, focusRingColor } from './focus.js';
import { rangeKey, choiceKey, scrollKey } from './focusmodel.js';
import { wheelValue } from './controlmodel.js';

// Push button. Optional (all additive): small (22 px high), plain (no fill until hover), disabled,
// destructive (red text) and icon (a glyph before the label). See docs/DESIGN.md.
// The shared controls are Tab stops (sdk/focus.js) unless disabled or focusable={false}: Space and Enter press a
// button or flip a Toggle, the arrows move a Slider, a Stepper or a Segmented control.
export function Button({ label, onClick, primary, style, small, plain, disabled, destructive, icon, focusable = true }) {
  const t = useTheme();
  const [hover, setHover] = useState(false);
  const fg = primary ? '#fff' : destructive ? '#ff453a' : t.text;
  const bg = primary ? t.accent : plain ? (hover && !disabled ? t.hover : 'transparent') : t.control;
  const border = t.buttonBorder && !plain ? { border: `1px solid ${primary ? t.accent : t.separator}` } : null;
  return (
    <View onClick={disabled ? undefined : onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      focusable={focusable && !disabled}
      style={[{ height: small ? 22 : 28, paddingHorizontal: small ? 10 : 14, borderRadius: small ? t.controlRadiusSmall : t.controlRadius, flexDirection: 'row', gap: 6,
        justifyContent: 'center', alignItems: 'center', backgroundColor: bg, ...border,
        opacity: disabled ? 0.4 : hover && !plain ? 0.85 : 1 }, style]}>
      {icon ? <Text style={{ fontSize: small ? 12 : 14, color: fg }}>{icon}</Text> : null}
      {label != null && label !== '' ? <Text style={{ fontSize: small ? 12 : 13, fontWeight: 500, color: fg, whiteSpace: 'nowrap' }}>{label}</Text> : null}
    </View>
  );
}

// small: the 36 × 20 switch of grouped forms (Settings rows) instead of 42 × 24. The knob moves
// physically (direction: 'ltr'), also in right-to-left languages.
export function Toggle({ on, onChange, small, focusable = true }) {
  const t = useTheme();
  const w = small ? 36 : 42, h = small ? 20 : 24, k = h - 4;
  return (
    <View onClick={() => onChange(!on)} focusable={focusable} style={{ width: w, height: h, borderRadius: h / 2, direction: 'ltr',
      backgroundColor: on ? t.accent : t.control }}>
      <View style={{ position: 'absolute', top: 2, left: on ? w - k - 2 : 2, width: k, height: k, borderRadius: k / 2,
        backgroundColor: '#fff', boxShadow: '0 1px 3px rgba(0,0,0,0.35)' }} />
    </View>
  );
}

// appearance: 'accent' (default: the selected segment is filled with the accent) or 'neutral'
// (view switcher: a raised neutral segment). small: 22 px high instead of 28. fill: the
// segments share the full width of the control.
// One Tab stop: ← → (Home, End) choose the segment, in the row's order (swapped in right to left).
export function Segmented({ options, value, onChange, appearance = 'accent', small, fill, style, focusable = true }) {
  const t = useTheme();
  const neutral = appearance === 'neutral';
  const raised = t.mode === 'dark' ? 'rgba(255,255,255,0.18)' : '#ffffff';
  const onKey = (e) => {
    const i = choiceKey(e, { index: options.findIndex(([v]) => v === value), count: options.length, rtl: direction() === 'rtl' });
    if (i == null) return false;
    if (options[i][0] !== value) onChange(options[i][0]);
    return true;
  };
  return (
    <View focusable={focusable && options.length > 0} onKeyDown={onKey} style={[{ flexDirection: 'row', backgroundColor: t.control, borderRadius: small ? t.controlRadiusSmall : t.controlRadius + 1, padding: 2 }, style]}>
      {options.map(([v, label]) => {
        const on = value === v;
        return (
          <View key={v} onClick={() => onChange(v)} style={{ paddingHorizontal: small ? 10 : 14, height: small ? 18 : 24, borderRadius: Math.max(2, (small ? t.controlRadiusSmall : t.controlRadius) - 1),
            justifyContent: 'center', alignItems: 'center', flexGrow: fill ? 1 : 0,
            backgroundColor: on ? (neutral ? raised : t.accent) : 'transparent',
            boxShadow: on && neutral ? '0 1px 2px rgba(0,0,0,0.18)' : 'none' }}>
            <Text style={{ fontSize: small ? 11 : 12, fontWeight: on && neutral ? 600 : neutral ? 500 : 600, whiteSpace: 'nowrap',
              color: on && !neutral ? '#fff' : t.text }}>{label}</Text>
          </View>
        );
      })}
    </View>
  );
}

// Source-list row. Optional: detail (trailing text, e.g. a count) and indent (nesting level).
export function SidebarItem({ label, selected, onClick, icon, detail, indent = 0 }) {
  const t = useTheme();
  const [hover, setHover] = useState(false);
  // selection 'indicator' (a theme token): a light fill and a short accent bar at the leading edge
  const ind = t.selection === 'indicator';
  const filled = selected && !ind;
  const sel = t.mode === 'dark' ? 'rgba(255,255,255,0.09)' : 'rgba(0,0,0,0.06)';
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 28, paddingLeft: 10 + indent * 14, paddingRight: 10, borderRadius: t.controlRadiusSmall, flexDirection: 'row', alignItems: 'center', gap: 8,
        backgroundColor: filled ? t.accent : selected ? sel : hover ? t.hover : 'transparent' }}>
      {selected && ind ? <View style={{ position: 'absolute', insetInlineStart: 0, top: 8, width: 3, height: 12, borderRadius: 2, backgroundColor: t.accent }} /> : null}
      {typeof icon === 'string' ? <Text style={{ width: 18, textAlign: 'center', fontSize: 14, color: filled ? '#fff' : t.accent }}>{icon}</Text> : icon}
      <Text style={{ flex: 1, fontSize: 13, fontWeight: selected && ind ? 600 : 500, color: filled ? '#fff' : t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{label}</Text>
      {detail != null ? <Text style={{ fontSize: 11, color: filled ? 'rgba(255,255,255,0.8)' : t.textDim }}>{detail}</Text> : null}
    </View>
  );
}

export function SectionTitle({ children }) {
  const t = useTheme();
  return <Text style={{ fontSize: 11, fontWeight: 600, color: t.textDim, marginLeft: 10, marginTop: 10, marginBottom: 4 }}>{children}</Text>;
}

// Container with mouse-wheel / touchpad scrolling and an overlay scroll bar (sdk/scrollbar.js,
// docs/DESIGN.md "Scroll bars"): a thin bar drawn over the content at the end edge (left in RTL),
// hidden until the view scrolls or the pointer comes near the edge, fading out a second after the
// last scroll. It never reserves a strip: give the content its normal padding (contentStyle) and
// the bar sits in it. Settings → Appearance → "Show scroll bars" (theme token scrollBars:
// 'auto' | 'scrolling' | 'always'); reduce motion shows and hides it without a fade.
// scrollRef (optional): { scrollTo(y), offset(), inner } to jump to a position (an index, a key
//   press); a jump that moves the view shows the bar like any other scroll.
// contentStyle (optional): style of the scrolled content box (its padding).
// gutter (0.8.1, optional): with "Always" only, keep a strip at the end for the bar.
// resetKey (0.8.8.3, optional): what the view is showing (a page, a section, a folder, a search).
//   When it changes the view goes back to the top, so the next page doesn't open where the last
//   one was scrolled to (or past its end). Leave it out where the same content just updates
//   (a growing log, a list that refreshes) and the position should stay.
// onWheel (0.8.8.4, optional): sees each wheel event first; if it calls e.stopPropagation() the view
//   doesn't scroll (Files zooms with Ctrl + wheel).
// focusable (0.8.8.8, optional): a Tab stop while its content doesn't fit (sdk/focus.js); focused, or with a control
//   inside it focused, ↓ ↑, Page Down and Page Up (Space and Shift+Space), Home and End scroll it. false where the
//   arrows belong to the view's own content (Files' icons and rows). A control Tab moves to comes into sight.
// horizontal (0.8.8.8, optional): scrolls along the row instead (its content is laid out in one row): with a
//   sideways swipe or tilt of the wheel, Shift and the wheel, and ← → (swapped in right to left, where it starts at
//   the right); the bar runs along the bottom edge.
export const SCROLL_GUTTER = 11;
// A TextInput asks the ScrollView it is in to show its caret: on focus, on typing, and when the view
// changes height, as a window does when it shrinks for the on-screen keyboard. A wheel scroll with a
// field focused doesn't ask.
const ScrollReveal = createContext(null);
export function ScrollView({ style, contentStyle, children, scrollRef, gutter, resetKey, onScroll, onWheel, focusable, horizontal = false }) {
  const t = useTheme();
  const mode = scrollbarMode(t.scrollBars);
  const reduce = !!t.reduceMotion;
  const rtl = direction() === 'rtl';
  const outer = useRef(null), inner = useRef(null), thumb = useRef(null);
  const [offset, setOffset] = useState(0);
  // new content (resetKey changed): start at the top; React renders again before committing
  const [shownKey, setShownKey] = useState(resetKey);
  if (shownKey !== resetKey) { setShownKey(resetKey); if (offset) setOffset(0); }
  const [metrics, setMetrics] = useState({ view: 0, content: 0 });
  const [ui, setUi] = useState({ near: false, hover: false, dragging: false, shown: mode === 'always' });
  const st = useRef(null);
  if (!st.current) st.current = { lastScroll: null, hideTimer: 0, fadeTimer: 0, grab: 0, ui };
  st.current.ui = ui; st.current.mode = mode; st.current.reduce = reduce; st.current.metrics = metrics;
  // the length along the axis it scrolls: the height of a layout box, or its width for a horizontal view
  const len = horizontal ? 2 : 3;
  // a point's distance from the view's start edge along that axis (the right edge of a horizontal view in RTL)
  const along = (x, y, box) => (horizontal ? (rtl ? box[0] + box[2] - x : x - box[0]) : y - box[1]);
  const measure = () => {
    if (!outer.current || !inner.current) return null;
    const view = lord.getLayout(outer.current.id)[len], content = lord.getLayout(inner.current.id)[len];
    const m = st.current.metrics;
    if (view !== m.view || content !== m.content) { st.current.metrics = { view, content }; setMetrics({ view, content }); }
    return { view, content };
  };
  useEffect(() => { const id = setTimeout(measure, 50); return () => clearTimeout(id); });
  const max = Math.max(0, metrics.content - metrics.view);
  const y = Math.min(offset, max);
  const geom = thumbGeometry(metrics.view, metrics.content, y);

  // ---- showing and hiding ----
  const setOpacity = (o) => { try { if (thumb.current) lord.setStyle(thumb.current.id, 'opacity', o); } catch (e) { /* unmounted */ } };
  const update = (patch) => setUi((u) => {
    const n = { ...u, ...patch };
    return n.near === u.near && n.hover === u.hover && n.dragging === u.dragging && n.shown === u.shown ? u : n;
  });
  const model = () => ({ mode: st.current.mode, lastScroll: st.current.lastScroll, ...st.current.ui });
  const stopFade = () => { clearInterval(st.current.fadeTimer); st.current.fadeTimer = 0; };
  const schedule = () => {
    const s = st.current;
    clearTimeout(s.hideTimer);
    const at = hideTime(model());
    if (at == null) return;
    s.hideTimer = setTimeout(() => {
      if (!s.ui.shown) return;
      if (barShown(model(), lord.now())) { schedule(); return; }
      if (s.reduce) { s.ui = { ...s.ui, shown: false }; update({ shown: false }); return; }
      const t0 = lord.now();
      stopFade();
      s.fadeTimer = setInterval(() => {
        const o = barOpacity(model(), lord.now(), t0, false);
        setOpacity(o);
        if (o <= 0) { stopFade(); s.ui = { ...s.ui, shown: false }; update({ shown: false }); }
      }, 33);
    }, Math.max(0, at - lord.now()));
  };
  const show = (patch) => {
    stopFade();
    setOpacity(1);
    st.current.ui = { ...st.current.ui, ...patch, shown: true };
    update({ ...patch, shown: true });
    schedule();
  };
  const scrolled = () => { st.current.lastScroll = lord.now(); show({}); };
  useEffect(() => () => { clearTimeout(st.current.hideTimer); stopFade(); }, []);
  useEffect(() => { if (mode === 'always') show({}); else schedule(); }, [mode]);

  // ---- dragging the thumb ----
  useEffect(() => {
    const a = addListener('pointermove', (e) => {
      const s = st.current;
      if (!s.ui.dragging || !outer.current) return;
      const m = s.metrics;
      setOffset(offsetForThumb(m.view, m.content, s.along(e.x, e.y, lord.getLayout(outer.current.id)) - s.grab));
      s.lastScroll = lord.now();
    });
    const b = addListener('pointerup', () => {
      const s = st.current;
      if (!s.ui.dragging) return;
      s.lastScroll = lord.now();
      s.ui = { ...s.ui, dragging: false };
      update({ dragging: false });
      schedule();
    });
    return () => { a(); b(); };
  }, []);
  st.current.along = along;

  const jump = (v) => {
    const m = measure() || metrics;
    const to = Math.max(0, Math.min(Math.max(0, m.content - m.view), v));
    if (Math.round(to) !== Math.round(y)) scrolled();
    setOffset(Math.max(0, v));
  };
  if (scrollRef) scrollRef.current = { scrollTo: jump, offset: () => y, inner };
  // onScroll(y, view): the offset after it changed (what is in sight, for lazy contents)
  const scrolledTo = useRef(onScroll);
  scrolledTo.current = onScroll;
  useEffect(() => { scrolledTo.current?.(y, metrics.view); }, [y, metrics.view]);
  // c: { x, y, w, h } on the screen (a caret has no width)
  const latestReveal = useRef(null);
  latestReveal.current = (c, always) => {
    if (!outer.current) return;
    const box = lord.getLayout(outer.current.id);
    const resized = st.current.fieldView !== box[len];
    st.current.fieldView = box[len];
    if (!always && !resized) return;
    // how far c starts and ends from the view's start edge along the axis
    const a = horizontal ? (rtl ? box[0] + box[2] - (c.x + (c.w || 0)) : c.x - box[0]) : c.y - box[1];
    const b = a + (horizontal ? c.w || 0 : c.h);
    const past = b + 12 - box[len], before = 4 - a;
    if (past > 0) jump(y + past); else if (before > 0) jump(y - before);
  };
  const reveal = useRef((c, always) => latestReveal.current?.(c, always)).current;

  // the keys of the view (focused, or a control inside it focused): sdk/focus.js
  const onKey = (e) => {
    const m = measure() || metrics;
    const v = scrollKey(e, { offset: y, view: m.view, content: m.content, horizontal, rtl });
    if (v == null) return false;
    jump(v);
    return true;
  };

  const onMove = (e) => {
    if (!outer.current) return;
    if (!geom) measure();                       // content that grew by itself has told nobody
    const box = lord.getLayout(outer.current.id);
    const [ox, oy, ow, oh] = box;
    // a vertical view's bar is at the end edge; a horizontal one's along the bottom edge
    const near = !!geom && (horizontal ? nearEdge(e.y - oy, oh, false) : nearEdge(e.x - ox, ow, rtl));
    const hover = !!geom && st.current.ui.shown && (horizontal ? hitThumb(e.y - oy, along(e.x, e.y, box), oh, geom, false)
      : hitThumb(e.x - ox, e.y - oy, ow, geom, rtl));
    const u = st.current.ui;
    if (near === u.near && hover === u.hover) return;
    if (mode === 'auto' && near && !u.near) show({ near, hover });
    else { st.current.ui = { ...u, near, hover }; update({ near, hover }); schedule(); }
  };
  const shown = mode === 'always' || ui.shown;
  const wide = ui.hover || ui.dragging;
  const thumbColor = wide ? (t.scrollThumbHover || (t.mode === 'dark' ? 'rgba(255,255,255,0.5)' : 'rgba(0,0,0,0.45)'))
    : (t.scrollThumb || (t.mode === 'dark' ? 'rgba(255,255,255,0.32)' : 'rgba(0,0,0,0.3)'));
  const w = barWidth(ui);
  const pad = gutter && mode === 'always' ? (horizontal ? { paddingBottom: SCROLL_GUTTER } : { paddingInlineEnd: SCROLL_GUTTER }) : null;
  // the bar's strip and thumb: at the end edge, or along the bottom edge from the start edge of a horizontal view
  const strip = horizontal ? { bottom: 0, [rtl ? 'right' : 'left']: geom?.top, height: SCROLLBAR.hit, width: geom?.size }
    : { end: 0, top: geom?.top, width: SCROLLBAR.hit, height: geom?.size };
  const bar = horizontal ? { bottom: SCROLLBAR.inset, left: 0, width: geom?.size, height: w } : { end: SCROLLBAR.inset, top: 0, height: geom?.size, width: w };
  return (
    <View ref={outer} onWheel={(e) => {
      if (onWheel) { onWheel(e); if (e.stopped) return; }
      // sideways: a swipe or a tilted wheel (deltaX), or Shift and the wheel; in RTL the view starts at the right
      const d = horizontal ? (e.deltaX ? (rtl ? -e.deltaX : e.deltaX) : e.shift ? e.deltaY || 0 : 0) : e.deltaY;
      const m = measure() || metrics;
      const mx = Math.max(0, m.content - m.view);
      if (mx > 0 && d) scrolled();
      setOffset((o) => Math.max(0, Math.min(mx, Math.min(o, max) + (d || 0))));
    }} onMouseMove={onMove} onMouseLeave={() => { st.current.ui = { ...st.current.ui, near: false, hover: false }; update({ near: false, hover: false }); schedule(); }}
      focusable={focusable ?? max > 0} onKeyDown={onKey} onRevealFocus={(r) => reveal(r, true)}
      style={[{ overflow: 'hidden', flex: 1 }, horizontal ? { flexDirection: 'row' } : null, style]}>
      <View ref={inner} style={[horizontal ? { translateX: rtl ? y : -y, flexShrink: 0, flexDirection: 'row' } : { translateY: -y }, pad, contentStyle]}>
        <ScrollReveal.Provider value={reveal}>{children}</ScrollReveal.Provider>
      </View>
      {geom && (
        <View onMouseDown={(e) => {
          if (!outer.current) return;
          e.stopPropagation();
          st.current.grab = along(e.x, e.y, lord.getLayout(outer.current.id)) - geom.top;
          st.current.ui = { ...st.current.ui, dragging: true };
          show({ dragging: true });
        }} style={{ position: 'absolute', ...strip, pointerEvents: shown || ui.near ? 'auto' : 'none' }}>
          <View ref={thumb} style={{ position: 'absolute', ...bar,
            borderRadius: t.scrollRadius != null ? Math.min(t.scrollRadius, w / 2) : w / 2, backgroundColor: thumbColor,
            ...(t.scrollThumbBorder ? { border: `1px solid ${t.scrollThumbBorder}` } : null), opacity: shown ? 1 : 0 }} />
        </View>
      )}
    </View>
  );
}

// ---- keyboard focus ----

let focused = null;          // token of the active TextInput
let focusedNow = false;      // it got focus during the mouse event in progress
const handlers = new Map();
// A form: Tab and Shift+Tab go round its text fields, in the order they mounted. Without one, Tab goes on to the next
// control of the window (sdk/focus.js).
const FieldOrder = createContext(null);
export function FormFields({ children }) {
  const order = useRef([]).current;
  return <FieldOrder.Provider value={order}>{children}</FieldOrder.Provider>;
}
const focusSubs = new Set();
const carets = new Map();    // token → where its caret is (also for password fields, off the input method)
const setFocus = (tok) => {
  focused = tok;
  for (const fn of focusSubs) fn();
  noteTextField('input', tok !== null ? () => carets.get(tok)?.() || null : null);
};

addListener('pointerdown', () => { if (!focusedNow) setFocus(null); focusedNow = false; });
addListener('keydown', (e) => { if (focused && handlers.has(focused)) handlers.get(focused)(e); });

export const hasTextFocus = () => focused !== null;

// The box of each TextInput and TextArea: Tab goes on from the one with the keys to the next control (sdk/focus.js).
// A field of an app's own (takeTextFocus: Notes, Mail's body) has none and keeps Tab.
const boxes = new Map();     // token → its box (a ref)
setTextFocusBridge({ owner: () => (focused === null ? null : boxes.get(focused)?.current || false), blur: () => clearTextFocus() });

// A field out of sight must not keep the keys: its window stays mounted (desktopmodel.js), so when the
// window that was in front is minimized or leaves the desktop in view, the text focus goes with it.
export const clearTextFocus = () => { if (focused !== null) setFocus(null); };
let frontWas = null;
function checkFront() {
  const w = frontWas && wm.get(frontWas);
  if (w && (w.minimized || !onActiveDesktop(w))) clearTextFocus();
  frontWas = wm.focused();
}
windows.subscribe(checkFront);
activeDesktop.subscribe(checkFront);

// For a custom text field (for example, a note edited on the desktop): while it has
// focus it receives the keys, and useKeyboard/the apps don't (as with a TextInput). Call from the
// field's onMouseDown; it is released by a click elsewhere or with releaseTextFocus.
export function takeTextFocus(token, handler, onChange) {
  handlers.set(token, handler);
  if (onChange) focusSubs.add(onChange);
  focusedNow = true;
  setFocus(token);
}
export function releaseTextFocus(token, onChange) {
  if (onChange) focusSubs.delete(onChange);
  handlers.delete(token);
  if (focused === token) setFocus(null);
}
export const textFocusOwner = () => focused;
// For panels that take the keyboard for a moment (Search, clipboard): remember the focused
// field, give focus back on close and paste the clipboard into it as if it were Ctrl+V.
export const focusToken = () => focused;
export function restoreFocus(tok) { if (tok && handlers.has(tok)) setFocus(tok); }
export function pasteIntoFocused() {
  if (!focused || !handlers.has(focused)) return false;
  handlers.get(focused)({ ctrl: true, base: 'v', key: 'v', char: '' });
  return true;
}

// ---- what TextInput and TextArea share: undo and redo, and the right-click menu

// the primary selection (primarymodel.js): lord.primary is lord-server's (an isolated app has none: nothing is published
// and a middle click pastes nothing); get() gives text only while a middle-button press is handled, with middle-click
// paste on (Settings → Mouse & Trackpad)
const hasPrimary = () => typeof lord !== 'undefined' && lord.primary && typeof lord.primary.set === 'function';
const publishPrimary = (text) => { if (text && hasPrimary()) { try { lord.primary.set(text); } catch (e) { /* too long */ } } };
const readPrimary = () => { try { return (hasPrimary() && lord.primary.get()) || ''; } catch (e) { return ''; } };

// the step of the history (textfieldmodel.js) a key that edits makes: one character deleted joins the step in progress,
// a selection deleted or a word (Ctrl or ⌥) is a step of its own
const stepOf = (cur, e) => (selRange(cur) || e.ctrl || e.alt || e.meta ? 'clear' : e.key === 'Enter' ? 'type' : 'delete');

// Undo, Redo, Cut, Copy, Paste, Delete and Select All: each sends the field's own key (send is its key handler), so the
// menu does exactly what the keys do. A password field cuts and copies nothing.
function fieldMenuItems(send, { hist, sel, secure, value }) {
  const key = (base, more) => () => send({ ctrl: true, key: base, base, char: '', fromMenu: true, ...more });
  return [
    { label: tr('shell.textMenu.undo'), disabled: !canUndo(hist), onClick: key('z') },
    { label: tr('shell.textMenu.redo'), disabled: !canRedo(hist), onClick: key('z', { shift: true }) },
    '-',
    { label: tr('shell.textMenu.cut'), disabled: !sel || secure, onClick: key('x') },
    { label: tr('shell.textMenu.copy'), disabled: !sel || secure, onClick: key('c') },
    { label: tr('shell.textMenu.paste'), onClick: key('v') },
    { label: tr('shell.textMenu.delete'), disabled: !sel, onClick: () => send({ key: 'Delete', char: '', fromMenu: true }) },
    '-',
    { label: tr('shell.textMenu.selectAll'), disabled: !value, onClick: key('a') },
  ];
}

// The menu at { x, y } of the field's box (anchor), above the window (layers.js). keepFocus: choosing an item leaves the
// field focused. It stays inside the display (or, in an isolated app, the window) the field is on.
function FieldMenu({ anchor, at, items, onClose }) {
  const win = useContext(AppWindowContext);
  let bounds;
  try {
    const r = lord.getLayout(anchor.current.id);
    const host = !shellLayers() && win?.overlayHost ? lord.getLayout(win.overlayHost.id) : null;
    const d = host ? { x: host[0], y: host[1], w: host[2], h: host[3] } : displayFor({ x: r[0], y: r[1], w: r[2], h: r[3] });
    bounds = { w: d.x + d.w - r[0], h: d.y + d.h - r[1] };
  } catch (e) { bounds = undefined; }
  return (
    <Overlay anchor={anchor}>
      <View keepFocus style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, pointerEvents: 'box-none' }}>
        <ContextMenu x={at.x} y={at.y} items={items} onClose={onClose} bounds={bounds} />
      </View>
    </Overlay>
  );
}

// onKeyDown(e) optional: return true if the key was already handled (e.g. Shift+Enter).
// secure: password field (shows •, does not go through the input method and cannot be copied). An eye at its end shows the
// text until the field loses focus (passwordfieldmodel.js); it stays a password field meanwhile: no history, no input
// method, nothing copied. eye: false leaves the eye out (an app with its own Show). While it has focus with Caps Lock
// on, ⇪ and "Caps Lock is on" (only ⇪ when the words do not fit) sit before the eye.
// selectAll: starts with all the text selected (the first key replaces it), as when renaming in Files.
// Editing as in any text field (textfieldmodel.js): the caret moves with ←, →, Home, End and a click; a selection is made
// with Shift and those keys, by dragging, with a double click (a word) or Ctrl/⌘+A, and typing, a paste or Backspace
// replace it; Ctrl/⌘+C and X copy and cut it (never from a password field); Ctrl or ⌥ with ← → and Backspace go by word.
// A triple click selects it all. Ctrl/⌘+Z undoes and Ctrl/⌘+Shift+Z (or Ctrl+Y) redoes: typing is one step until a pause,
// a new word or a caret moved elsewhere; a paste, a cut, a deleted selection and a value changed from outside are steps
// of their own. A right click opens Undo, Redo, Cut, Copy, Paste, Delete and Select All, and moves no caret.
// What is selected becomes the primary selection (never in a password field), and a middle click pastes the primary
// selection where it lands, as in any Linux app (primarymodel.js; Settings → Mouse & Trackpad can turn that off).
// The text is drawn in one piece with the caret and the selection over it, so placing the caret moves no letter; a
// value longer than the field scrolls to keep the caret in sight. A value changed from outside puts the caret at the end.
export function TextInput({ value, onChange, placeholder, onSubmit, onCancel, onKeyDown, autoFocus, style, secure, eye, size = 13, selectAll = false }) {
  const t = useTheme();
  const token = useRef({}).current;
  const [isFocused, setIsFocused] = useState(false);
  const [blink, setBlink] = useState(true);
  const latest = useRef();
  const caret = useRef(null);
  const textBox = useRef(null);
  const len = (v) => [...(v || '')].length;
  const at = useRef({ value, pos: len(value), anchor: selectAll && value ? 0 : null });    // caret and selection, for the value they were made in
  const hist = useRef(newHistory(secure));  // undo and redo (textfieldmodel.js); none for a password field
  hist.current = historyFor(hist.current, secure);   // a password field keeps no history; a change of secure drops it
  const prim = useRef(null);                // the selection last published as the primary selection (primarymodel.js)
  // a value changed from outside: the caret goes to its end, and Undo brings the old one back
  const adopt = (v) => {
    if (at.current.value === v) return;
    const next = { value: v, pos: len(v), anchor: null };
    hist.current = record(hist.current, at.current, next, 'external', lord.now());
    at.current = next;
  };
  adopt(value);
  const box = useRef(null);
  const [menu, setMenu] = useState(null);   // the right-click menu: { x, y } in the field's box
  const menuOpen = useRef(false);
  menuOpen.current = !!menu;
  const scroll = useRef(0);                 // how far the text is moved toward its start edge, to keep the caret in sight
  const pressing = useRef(false);           // a drag that selects
  const [, redraw] = useState(0);
  const secureRef = useRef(secure);
  secureRef.current = secure;
  const [revealed, setRevealed] = useState(false);     // the eye: the password shown for now
  const shownRef = useRef(false);
  shownRef.current = !!secure && revealed;
  const [caps, setCaps] = useState(capsLock);          // Caps Lock, for a password field's hint
  const [eyeTip, setEyeTip] = useState(false);
  const eyeRef = useRef(null);
  latest.current = { value, onChange, onSubmit, onCancel, onKeyDown };
  // what is selected becomes the primary selection (never a password field's)
  const publish = () => { const r = primaryStep(prim.current, at.current, secureRef.current); prim.current = r.last; publishPrimary(r.text); };
  const order = useContext(FieldOrder);
  const measure = (str) => lord.measureText(str, size, 400);
  const rtlOf = (v) => !secureRef.current && (v ? textDirection(v) === 'rtl' : direction() === 'rtl');

  // the position under a pointer x (screen coordinates), from the text's start edge and with the scroll
  const posAtX = (x) => {
    const v = latest.current.value || '';
    if (!textBox.current || !v) return 0;
    let lx;
    try { const [bx, , bw] = lord.getLayout(textBox.current.id); lx = rtlOf(v) ? bx + bw - x : x - bx; } catch (e) { return at.current.pos; }
    const shown = secureRef.current ? maskedText(v, shownRef.current) : v;
    return clampPos(v, caretAt(shown, lx + scroll.current, measure));
  };

  useEffect(() => {
    // with focus, keys go through the input method (Korean, Japanese, Chinese)
    const caretRect = () => { if (!caret.current) return null; const [x, y, , h] = lord.getLayout(caret.current.id); return { x, y, h }; };
    const sync = () => {
      setIsFocused(focused === token);
      if (focused !== token) setRevealed((r) => revealAfter(r, 'blur'));    // the eye closes with the focus
      if (focused === token && !secureRef.current) ime.attach(token, caretRect, { inline: true }); else ime.detach(token);
    };
    focusSubs.add(sync);
    carets.set(token, caretRect);
    handlers.set(token, (e) => {
      const p = latest.current;
      if (e.key === 'Escape' && menuOpen.current) return;           // it closes the menu (contextmenu.jsx)
      // a key the app took (Tab completing an address) moves no focus
      if (!e.fromMenu && p.onKeyDown && p.onKeyDown(e)) { e.preventDefault?.(); setBlink(true); return; }
      if (e.key === 'Tab' && order && order.length > 1 && !e.ctrl && !e.meta && !e.alt) {
        const i = order.indexOf(token);
        e.preventDefault?.();
        setFocus(order[(i + (e.shift ? order.length - 1 : 1)) % order.length]);
        return;
      }
      adopt(p.value);
      // a value and its caret, drawn at once (a caret that only moved changes no value)
      const show = (r) => { at.current = r; if (r.value !== p.value) p.onChange(r.value); publish(); redraw((n) => n + 1); };
      const put = (r, step) => { hist.current = record(hist.current, at.current, r, step, lord.now()); show(r); };
      const cur = at.current;
      const mod = (e.ctrl || e.meta) && !e.alt;
      const h = historyKey(e);
      if (h) { const r = (h === 'undo' ? undo : redo)(hist.current, cur); if (r) { hist.current = r.h; show(r.s); } setBlink(true); return; }
      if (mod && e.base === 'v') { put(replaceSel(cur, lord.clipboard.get().replace(/\r?\n/g, ' ')), 'paste'); setBlink(true); return; }
      if (mod && (e.base === 'c' || e.base === 'x')) {
        const txt = secureRef.current ? '' : selectedText(cur);
        if (txt) { lord.clipboard.set(txt, { origin: 'lordos' }); if (e.base === 'x') put(replaceSel(cur, ''), 'cut'); }
        return;
      }
      const k = editKey(cur, e, rtlOf(p.value));
      if (k) put(k, stepOf(cur, e));
      else if (e.key === 'Enter') { setRevealed((r) => revealAfter(r, 'submit')); p.onSubmit?.(p.value); }
      else if (e.key === 'Escape') p.onCancel?.();
      else if (e.char) put(replaceSel(cur, e.char), 'type');
      setBlink(true);
    });
    order?.push(token);
    boxes.set(token, box);
    if (autoFocus) setFocus(token);
    // a drag that started in the field selects, also past its edges
    const offMove = addListener('pointermove', (e) => {
      if (!pressing.current || focused !== token) return;
      const pos = posAtX(e.x);
      if (pos !== at.current.pos) { at.current = { ...at.current, pos }; redraw((n) => n + 1); }
    });
    const offUp = addListener('pointerup', () => { if (pressing.current) { pressing.current = false; publish(); } });
    return () => { offMove(); offUp(); if (order) order.splice(order.indexOf(token), 1); focusSubs.delete(sync); handlers.delete(token); carets.delete(token); boxes.delete(token); ime.detach(token); if (focused === token) setFocus(null); };
  }, []);
  // Caps Lock comes with every key (runtime/keymap.js); a password field with focus follows it for its hint
  useEffect(() => {
    if (!secure || !isFocused) return undefined;
    setCaps(capsLock());
    const follow = (e) => setCaps((c) => capsAfter(c, e));
    const offDown = addListener('keydown', follow), offUp = addListener('keyup', follow);
    return () => { offDown(); offUp(); };
  }, [secure, isFocused]);
  useEffect(() => { if (!secure) setRevealed((r) => revealAfter(r, 'unsecure')); }, [secure]);
  const imeSt = useIme();
  const preedit = isFocused ? imeSt.preedit : '';
  const reveal = useContext(ScrollReveal);
  useEffect(() => {
    if (!isFocused || !reveal) return undefined;
    const look = (always) => { const c = carets.get(token)?.(); if (c) reveal(c, always); };
    let later = 0;                                       // layout runs after the commit
    const moved = () => { later = later || setTimeout(() => { later = 0; look(false); }, 60); };
    const id = setTimeout(() => look(true), 30);         // the caret is drawn after this render
    const off = onCommit(moved), offScreen = onScreenChange(moved);
    return () => { clearTimeout(id); clearTimeout(later); off(); offScreen(); };
  }, [isFocused, value]);

  useEffect(() => {
    if (!isFocused) return undefined;
    const id = setInterval(() => setBlink((b) => !b), 530);
    return () => clearInterval(id);
  }, [isFocused]);

  const onDown = (ev) => {
    focusedNow = true;
    setFocus(token);
    if (!ev) return;
    adopt(value);
    // a middle click pastes the primary selection where it lands; without one (or with middle-click paste off), and on a
    // right click, the caret and the selection stay
    const pasted = ev.button === 1 ? readPrimary() : '';
    if (pasted) {
      const cur = at.current, r = pasteAt(cur, posAtX(ev.x), pasted, true);
      hist.current = record(hist.current, cur, r, 'paste', lord.now());
      at.current = r;
      if (r.value !== value) latest.current.onChange(r.value);
      redraw((n) => n + 1);
      setBlink(true);
      return;
    }
    if (ev.button) { redraw((n) => n + 1); return; }
    const pos = posAtX(ev.x);
    const cur = at.current;
    at.current = ev.shift ? { value, pos, anchor: cur.anchor != null ? cur.anchor : cur.pos } : { value, pos, anchor: pos };
    hist.current = record(hist.current, cur, at.current, 'move');
    pressing.current = true;
    redraw((n) => n + 1);
    setBlink(true);
  };
  // Tab came to the field: the keys are its own, with all of its text selected (the first key replaces it)
  const onKeyFocus = () => {
    adopt(value);
    at.current = { value, pos: len(value), anchor: value ? 0 : null };
    setFocus(token);
    redraw((n) => n + 1);
    setBlink(true);
  };
  const onDouble = () => {
    if (secure || !value) return;
    const [a, b] = wordAt(value, at.current.pos);
    at.current = { value, pos: b, anchor: a };
    pressing.current = false;
    publish();
    redraw((n) => n + 1);
  };
  const onClick = (ev) => {
    if (!ev || ev.detail !== 3 || !value) return;                 // a triple click: the whole value (one line)
    at.current = { value, pos: len(value), anchor: 0 };
    pressing.current = false;
    publish();
    redraw((n) => n + 1);
  };
  const onMenu = (ev) => {
    ev.stopPropagation();                                          // the field's menu, not the one of the view it is in
    try { const [bx, by] = lord.getLayout(box.current.id); setMenu({ x: ev.x - bx, y: ev.y - by }); } catch (e) { /* not laid out yet */ }
  };

  const shown = secure ? maskedText(value, shownRef.current) : value || '';
  const chars = [...shown];
  const rtl = rtlOf(value);
  const edge = rtl ? 'right' : 'left';
  const pos = isFocused ? clampPos(value, at.current.pos) : chars.length;
  const sel = isFocused ? selRange(at.current) : null;
  const caretX = measure(chars.slice(0, pos).join(''));
  const fullW = shown ? measure(shown) : 0;
  let innerW = 0;
  try { innerW = textBox.current ? lord.getLayout(textBox.current.id)[2] : 0; } catch (e) { innerW = 0; }
  if (!isFocused || innerW <= 0) scroll.current = 0;
  else {
    let sx = scroll.current;
    if (caretX - sx > innerW - 2) sx = caretX - innerW + 2;
    if (caretX - sx < 0) sx = caretX;
    scroll.current = Math.max(0, Math.min(sx, Math.max(0, fullW - innerW + 2)));
  }
  const sx = scroll.current;
  const lineH = size + 6;
  // as wide as its text (or placeholder) when nothing gives it a width, as it was when the text was in the row
  const basis = Math.ceil(Math.max(fullW, placeholder && !value ? measure(placeholder) : 0)) + 3;
  const hasEye = eyeShown({ secure, eye });
  const hint = capsHint({ secure, focused: isFocused, caps });
  const hintText = hint ? tr('shell.password.capsLock') : '';
  let room = 0;
  if (hint) try { room = lord.getLayout(box.current.id)[2] - 20 - (hasEye ? 24 : 0) - 16; } catch (e) { room = 0; }
  const hintWords = hint && capsFits(room, fullW, lord.measureText(hintText, 11, 400));
  // a press on the eye keeps the field's focus and its caret; a click flips it
  const eyeDown = (ev) => { ev?.stopPropagation(); focusedNow = true; if (focused !== token) setFocus(token); };
  const eyeClick = (ev) => { ev?.stopPropagation(); setEyeTip(false); setRevealed((r) => revealAfter(r, 'toggle')); setBlink(true); };
  // a password field is laid out left to right (the caret's sums), so in a right-to-left UI the eye and the hint go first
  const startSide = secure && direction() === 'rtl';
  const capsGlyph = <Text key="g" style={{ fontSize: size, fontWeight: 700, color: t.textDim }}>⇪</Text>;
  const capsWords = hintWords ? <Text key="w" style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap' }}>{hintText}</Text> : null;
  const capsBit = hint ? (
    <View key="caps" style={{ flexDirection: 'row', alignItems: 'center', gap: 3, pointerEvents: 'none' }}>
      {startSide ? [capsWords, capsGlyph] : [capsGlyph, capsWords]}
    </View>
  ) : null;
  const eyeBit = hasEye ? (
    <View key="eye" ref={eyeRef} onMouseDown={eyeDown} onClick={eyeClick} onDoubleClick={(ev) => ev?.stopPropagation()} onContextMenu={(ev) => ev?.stopPropagation()}
      onMouseEnter={() => setEyeTip(true)} onMouseLeave={() => setEyeTip(false)}
      style={{ width: 20, height: 20, borderRadius: 4, alignItems: 'center', justifyContent: 'center', backgroundColor: eyeTip ? t.hover : 'transparent' }}>
      <EyeGlyph off={shownRef.current} color={t.textDim} />
      {eyeTip && shellLayers() ? <Overlay anchor={eyeRef} layer="tooltip"><FieldTip text={tr(eyeLabel(shownRef.current))} /></Overlay> : null}
    </View>
  ) : null;
  const tail = hint || hasEye ? (
    <View style={{ flexDirection: 'row', alignItems: 'center', flexShrink: 0, gap: 4,
      [startSide ? 'marginRight' : 'marginLeft']: 6, [startSide ? 'marginLeft' : 'marginRight']: hasEye ? -4 : 0 }}>
      {startSide ? [eyeBit, capsBit] : [capsBit, eyeBit]}
    </View>
  ) : null;
  return (
    <View ref={box} onMouseDown={onDown} onDoubleClick={onDouble} onClick={onClick} onContextMenu={onMenu}
      focusable focusRing={false} onFocus={onKeyFocus}
      style={[{ height: Math.max(28, size + 15), paddingHorizontal: 10, borderRadius: t.controlRadius, flexDirection: 'row', alignItems: 'center',
        // the cursor goes to the logical end: on the left if the text is Arabic or Hebrew
        direction: secure ? 'ltr' : value ? textDirection(value) : 'inherit',
        backgroundColor: t.inputBg, border: `1px solid ${isFocused && t.inputFocus !== 'underline' ? t.accent : t.separator}`, overflow: 'hidden' }, style]}>
      {/* inputFocus 'underline' (a theme token): an accent line along the bottom edge */}
      {isFocused && t.inputFocus === 'underline' ? <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 2, backgroundColor: t.accent, pointerEvents: 'none' }} /> : null}
      {startSide ? tail : null}
      <View ref={textBox} style={{ flexGrow: 1, flexShrink: 1, flexBasis: basis, minWidth: 0, height: lineH, overflow: 'hidden' }}>
        {sel ? <View style={{ position: 'absolute', top: 1, height: lineH - 2, [edge]: measure(chars.slice(0, sel[0]).join('')) - sx,
          width: Math.max(1, measure(chars.slice(sel[0], sel[1]).join(''))), backgroundColor: t.accent, opacity: 0.38, borderRadius: 2 }} /> : null}
        {preedit && !secure ? (
          // while composing (Korean, Japanese, Chinese) the composition sits between the two halves, as before
          <View style={{ position: 'absolute', top: 0, height: lineH, [edge]: -sx, flexDirection: 'row', alignItems: 'center' }}>
            <Text style={{ fontSize: size, color: t.text, whiteSpace: 'nowrap' }}>{chars.slice(0, pos).join('')}</Text>
            <Preedit st={imeSt} size={size} color={t.text} accent={t.accent} />
            <View ref={caret} style={{ width: 1.5, height: size + 3, marginLeft: 1, backgroundColor: t.accent }} />
            <Text style={{ fontSize: size, color: t.text, whiteSpace: 'nowrap' }}>{chars.slice(pos).join('')}</Text>
          </View>
        ) : (
          <>
            {shown ? <Text style={{ position: 'absolute', top: 0, [edge]: -sx, fontSize: size, lineHeight: lineH, color: t.text, whiteSpace: 'nowrap' }}>{shown}</Text>
              : <Text style={{ position: 'absolute', top: 0, [edge]: 0, fontSize: size, lineHeight: lineH, color: t.textDim, whiteSpace: 'nowrap' }}>{placeholder || ''}</Text>}
            {isFocused && <View ref={caret} style={{ position: 'absolute', top: 1.5, height: size + 3, width: 1.5, [edge]: caretX - sx,
              backgroundColor: blink && !sel ? t.accent : 'transparent' }} />}
          </>
        )}
      </View>
      {startSide ? null : tail}
      {menu ? <FieldMenu anchor={box} at={menu} onClose={() => setMenu(null)}
        items={fieldMenuItems((e) => handlers.get(token)?.(e), { hist: hist.current, sel: !!selRange(at.current), secure, value })} /> : null}
    </View>
  );
}

// The eye of a password field, drawn: an outline with a pupil, struck through while the text is shown (a click hides it)
function EyeGlyph({ off, color, s = 14 }) {
  const w = 1.5;
  return (
    <View style={{ width: s, height: s, alignItems: 'center', justifyContent: 'center', pointerEvents: 'none' }}>
      <View style={{ width: s, height: s * 0.62, borderRadius: s * 0.31, border: `${w}px solid ${color}`, alignItems: 'center', justifyContent: 'center' }}>
        <View style={{ width: s * 0.28, height: s * 0.28, borderRadius: s * 0.14, backgroundColor: color }} />
      </View>
      {off ? <View style={{ position: 'absolute', width: s * 1.05, height: w, backgroundColor: color, transform: 'rotate(-40deg)' }} /> : null}
    </View>
  );
}

// What the eye does, under it after a hover (the tooltip layer)
function FieldTip({ text }) {
  const t = useTheme();
  return (
    <View style={{ position: 'absolute', top: 26, end: 0, paddingHorizontal: 8, height: 22, justifyContent: 'center', pointerEvents: 'none',
      borderRadius: Math.max(2, t.menuRadius - 2), backgroundColor: t.popupBg || (t.mode === 'dark' ? '#3a3a40' : '#fbfbfd'),
      border: `1px solid ${t.separator}`, boxShadow: '0 4px 12px rgba(0,0,0,0.25)' }}>
      <Text style={{ fontSize: 11, color: t.text, whiteSpace: 'nowrap' }}>{text}</Text>
    </View>
  );
}

// Several lines of text (textareamodel.js): Enter starts a new line, long lines wrap at the field's width, the caret moves
// with the arrows (↑ ↓ keep the column), Home, End and a click, and it selects and edits as a TextInput does (Shift with
// the keys, a drag, a double click, Ctrl/⌘+A; typing or a paste replaces the selection; Ctrl/⌘+C, X). Each row is drawn
// in one piece with the caret and the selection over it. A paste keeps its line breaks. Grows with its rows. A triple
// click selects the line (up to its line breaks, not the row); Undo, Redo, the right-click menu and the primary selection
// (select to copy, middle click to paste) work as in a TextInput.
export function TextArea({ value, onChange, placeholder, onCancel, style, size = 13, minRows = 3 }) {
  const t = useTheme();
  const token = useRef({}).current;
  const [isFocused, setIsFocused] = useState(false);
  const [blink, setBlink] = useState(true);
  const [width, setWidth] = useState(0);
  const box = useRef(null);
  const caret = useRef(null);
  const latest = useRef();
  const len = (v) => [...(v || '')].length;
  const at = useRef({ value, pos: len(value), anchor: null });
  const hist = useRef(newHistory());
  const prim = useRef(null);
  const publish = () => { const r = primaryStep(prim.current, at.current); prim.current = r.last; publishPrimary(r.text); };
  const adopt = (v) => {
    if (at.current.value === v) return;
    const next = { value: v, pos: len(v), anchor: null };
    hist.current = record(hist.current, at.current, next, 'external', lord.now());
    at.current = next;
  };
  adopt(value);
  const [menu, setMenu] = useState(null);
  const menuOpen = useRef(false);
  menuOpen.current = !!menu;
  const pressing = useRef(false);
  const [, redraw] = useState(0);
  const order = useContext(FieldOrder);
  const measure = (str) => lord.measureText(str, size, 400);
  const lineH = Math.round(size * 1.5);
  const pad = 7;
  const rows = wrapRows(value || '', width - 2 * pad - 4, measure);
  latest.current = { value: value || '', onChange, onCancel, rows };

  // the position under a pointer (screen coordinates): its row, then the nearest place in it
  const posAtXY = (x, y) => {
    const p = latest.current;
    if (!box.current || !p.value) return 0;
    try {
      const [bx, by] = lord.getLayout(box.current.id);
      return posAt(p.value, p.rows, x - bx - pad, Math.max(0, Math.floor((y - by - pad) / lineH)), measure);
    } catch (e) { return at.current.pos; }
  };

  useEffect(() => {
    const caretRect = () => { if (!caret.current) return null; const [x, y, , h] = lord.getLayout(caret.current.id); return { x, y, h }; };
    const sync = () => {
      setIsFocused(focused === token);
      if (focused === token) ime.attach(token, caretRect, { inline: true }); else ime.detach(token);
    };
    focusSubs.add(sync);
    carets.set(token, caretRect);
    handlers.set(token, (e) => {
      const p = latest.current;
      if (e.key === 'Escape' && menuOpen.current) return;
      if (e.key === 'Tab' && order && order.length > 1 && !e.ctrl && !e.meta && !e.alt) {
        const i = order.indexOf(token);
        e.preventDefault?.();
        setFocus(order[(i + (e.shift ? order.length - 1 : 1)) % order.length]);
        return;
      }
      adopt(p.value);
      const show = (r) => { at.current = r; if (r.value !== p.value) p.onChange(r.value); publish(); redraw((n) => n + 1); };
      const put = (r, step) => { hist.current = record(hist.current, at.current, r, step, lord.now()); show(r); };
      const cur = at.current;
      const mod = (e.ctrl || e.meta) && !e.alt;
      const h = historyKey(e);
      if (h) { const r = (h === 'undo' ? undo : redo)(hist.current, cur); if (r) { hist.current = r.h; show(r.s); } setBlink(true); return; }
      if (mod && e.base === 'v') { put(replaceSel(cur, lord.clipboard.get().replace(/\r\n?/g, '\n')), 'paste'); setBlink(true); return; }
      if (mod && (e.base === 'c' || e.base === 'x')) {
        const txt = selectedText(cur);
        if (txt) { lord.clipboard.set(txt, { origin: 'lordos' }); if (e.base === 'x') put(replaceSel(cur, ''), 'cut'); }
        return;
      }
      const r = areaEdit(cur, e, p.rows, measure);
      if (r) put(r, stepOf(cur, e));
      else if (e.key === 'Escape') { setFocus(null); p.onCancel?.(); }
      else if (e.char && !e.ctrl && !e.meta) put(replaceSel(cur, e.char), 'type');
      setBlink(true);
    });
    order?.push(token);
    boxes.set(token, box);
    const offMove = addListener('pointermove', (e) => {
      if (!pressing.current || focused !== token) return;
      const pos = posAtXY(e.x, e.y);
      if (pos !== at.current.pos) { at.current = { ...at.current, pos }; redraw((n) => n + 1); }
    });
    const offUp = addListener('pointerup', () => { if (pressing.current) { pressing.current = false; publish(); } });
    return () => { offMove(); offUp(); if (order) order.splice(order.indexOf(token), 1); focusSubs.delete(sync); handlers.delete(token); carets.delete(token); boxes.delete(token); ime.detach(token); if (focused === token) setFocus(null); };
  }, []);
  // the width the rows wrap at, once the layout has given the field one
  useEffect(() => {
    const look = () => { try { const w = box.current ? lord.getLayout(box.current.id)[2] : 0; if (w && Math.abs(w - width) > 0.5) setWidth(w); } catch (e) { /* not laid out yet */ } };
    const id = setTimeout(look, 0);
    const off = onCommit(look), offScreen = onScreenChange(look);
    return () => { clearTimeout(id); off(); offScreen(); };
  }, [width]);
  useEffect(() => {
    if (!isFocused) return undefined;
    const id = setInterval(() => setBlink((b) => !b), 530);
    return () => clearInterval(id);
  }, [isFocused]);
  const imeSt = useIme();
  const preedit = isFocused ? imeSt.preedit : '';

  const onDown = (ev) => {
    focusedNow = true;
    setFocus(token);
    if (!ev) return;
    adopt(value);
    const pasted = ev.button === 1 ? readPrimary() : '';           // a middle click pastes the primary selection there
    if (pasted) {
      const cur = at.current, r = pasteAt(cur, posAtXY(ev.x, ev.y), pasted);
      hist.current = record(hist.current, cur, r, 'paste', lord.now());
      at.current = r;
      if (r.value !== (value || '')) latest.current.onChange(r.value);
      redraw((n) => n + 1);
      setBlink(true);
      return;
    }
    if (ev.button) { redraw((n) => n + 1); return; }               // a right click keeps the caret and the selection
    const pos = posAtXY(ev.x, ev.y);
    const cur = at.current;
    at.current = ev.shift ? { value, pos, anchor: cur.anchor != null ? cur.anchor : cur.pos } : { value, pos, anchor: pos };
    hist.current = record(hist.current, cur, at.current, 'move');
    pressing.current = true;
    redraw((n) => n + 1);
    setBlink(true);
  };
  const onKeyFocus = () => { setFocus(token); setBlink(true); };   // Tab came to the field: the caret where it was
  const onDouble = () => {
    if (!value) return;
    const [a, b] = wordAt(value, at.current.pos);
    at.current = { value, pos: b, anchor: a };
    pressing.current = false;
    publish();
    redraw((n) => n + 1);
  };
  const onClick = (ev) => {
    if (!ev || ev.detail !== 3 || !value) return;                 // a triple click: the line the caret is in
    const [a, b] = lineAt(value, at.current.pos);
    at.current = { value, pos: b, anchor: a };
    pressing.current = false;
    publish();
    redraw((n) => n + 1);
  };
  const onMenu = (ev) => {
    ev.stopPropagation();                                          // the field's menu, not the one of the view it is in
    try { const [bx, by] = lord.getLayout(box.current.id); setMenu({ x: ev.x - bx, y: ev.y - by }); } catch (e) { /* not laid out yet */ }
  };

  const chars = [...(value || '')];
  const pos = isFocused ? clampPos(value, at.current.pos) : chars.length;
  const sel = isFocused ? selRange(at.current) : null;
  const caretRow = rowOf(rows, pos);
  const textStyle = { position: 'absolute', left: 0, top: 0, fontSize: size, color: t.text, whiteSpace: 'nowrap', lineHeight: lineH };
  return (
    <View ref={box} onMouseDown={onDown} onDoubleClick={onDouble} onClick={onClick} onContextMenu={onMenu}
      focusable focusRing={false} onFocus={onKeyFocus}
      style={[{ minHeight: minRows * lineH + 2 * pad, padding: pad, borderRadius: t.controlRadius, direction: 'ltr',
        backgroundColor: t.inputBg, border: `1px solid ${isFocused && t.inputFocus !== 'underline' ? t.accent : t.separator}`, overflow: 'hidden' }, style]}>
      {isFocused && t.inputFocus === 'underline' ? <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 2, backgroundColor: t.accent, pointerEvents: 'none' }} /> : null}
      {!value && !preedit ? (
        <View style={{ height: lineH }}>
          <Text style={{ ...textStyle, color: t.textDim }}>{placeholder || ''}</Text>
          {isFocused && <View ref={caret} style={{ position: 'absolute', left: 0, top: (lineH - size - 3) / 2, width: 1.5, height: size + 3, backgroundColor: blink ? t.accent : 'transparent' }} />}
        </View>
      ) : rows.map((r, i) => {
        const mine = isFocused && i === caretRow;
        const text = chars.slice(r.start, r.end).join('');
        if (mine && preedit) {
          // while composing, the composition sits in the caret's row between its two halves
          return (
            <View key={i} style={{ flexDirection: 'row', height: lineH, alignItems: 'center' }}>
              <Text style={{ fontSize: size, color: t.text, whiteSpace: 'nowrap' }}>{chars.slice(r.start, pos).join('')}</Text>
              <Preedit st={imeSt} size={size} color={t.text} accent={t.accent} />
              <View ref={caret} style={{ width: 1.5, height: size + 3, marginLeft: 1, backgroundColor: t.accent }} />
              <Text style={{ fontSize: size, color: t.text, whiteSpace: 'nowrap' }}>{chars.slice(pos, r.end).join('')}</Text>
            </View>
          );
        }
        // the part of the selection in this row (a selected line break shows as a little room after the row's text)
        const a = sel ? Math.max(sel[0], r.start) : 0, b = sel ? Math.min(sel[1], r.end) : 0;
        const broke = sel && sel[0] <= r.end && sel[1] > r.end;
        const selX = sel && (b > a || broke) ? measure(chars.slice(r.start, a).join('')) : null;
        const selW = selX != null ? measure(chars.slice(a, b).join('')) + (broke ? 6 : 0) : 0;
        return (
          <View key={i} style={{ height: lineH }}>
            {selX != null ? <View style={{ position: 'absolute', top: 1, left: selX, width: Math.max(1, selW), height: lineH - 2, backgroundColor: t.accent, opacity: 0.38, borderRadius: 2 }} /> : null}
            {text ? <Text style={textStyle}>{text}</Text> : null}
            {mine && <View ref={caret} style={{ position: 'absolute', top: (lineH - size - 3) / 2, left: measure(chars.slice(r.start, pos).join('')), width: 1.5, height: size + 3,
              backgroundColor: blink && !sel ? t.accent : 'transparent' }} />}
          </View>
        );
      })}
      {menu ? <FieldMenu anchor={box} at={menu} onClose={() => setMenu(null)}
        items={fieldMenuItems((e) => handlers.get(token)?.(e), { hist: hist.current, sel: !!selRange(at.current), secure: false, value })} /> : null}
    </View>
  );
}

// The wheel over a Slider or a Stepper (sdk/controlmodel.js wheelValue): a notch up adds a step and down takes one; a
// touchpad's swipe only while the control has the keyboard focus. focused tells it (onFocus and onBlur of its node).
function useWheelStep() {
  const focused = useRef(false);
  return {
    focused, onFocus: () => { focused.current = true; }, onBlur: () => { focused.current = false; },
    wheel: (e, o, set) => {
      const v = wheelValue(e, { ...o, focused: focused.current });
      if (v == null) return;
      e.stopPropagation();               // the wheel moves the control, not the page it is in
      if (v !== o.value) set(v);
    },
  };
}

// Number with − and + buttons (e.g. font size). One Tab stop: ↑ and → (← in right to left) add a step, ↓ and ← take one,
// Page Up and Page Down ten, Home and End go to min and max. The wheel over it adds or takes a step.
export function Stepper({ value, onChange, min = -Infinity, max = Infinity, step = 1, format = (v) => String(v), width = 56, focusable = true }) {
  const t = useTheme();
  const set = (v) => onChange(Math.max(min, Math.min(max, Math.round(v / step) * step)));
  const w = useWheelStep();
  const onKey = (e) => {
    const v = rangeKey(e, { value, min, max, step, rtl: direction() === 'rtl' });
    if (v == null) return false;
    if (v !== value) set(v);
    return true;
  };
  const Btn = ({ label, d }) => (
    <View onClick={() => set(value + d)} style={{ width: 26, height: 26, alignItems: 'center', justifyContent: 'center' }}>
      <Text style={{ fontSize: 15, fontWeight: 600, color: t.text }}>{label}</Text>
    </View>
  );
  return (
    <View focusable={focusable} onKeyDown={onKey} onFocus={w.onFocus} onBlur={w.onBlur} onWheel={(e) => w.wheel(e, { value, min, max, step }, set)}
      style={{ flexDirection: 'row', alignItems: 'center', backgroundColor: t.control, borderRadius: t.controlRadius, height: 26 }}>
      <Btn label="−" d={-step} />
      <Text style={{ width, textAlign: 'center', fontSize: 12, fontWeight: 600, color: t.text }}>{format(value)}</Text>
      <Btn label="+" d={step} />
    </View>
  );
}

// Horizontal slider (drag or click on the bar). It runs from the start edge: min is on the right in right to left.
// A Tab stop: ← → (swapped in right to left) and ↓ ↑ move it by step (a twentieth of the range unless given), Page Down
// and Page Up by a tenth, Home and End to the ends. The wheel over it moves it by step too.
export function Slider({ value, onChange, min = 0, max = 1, width = 160, step, focusable = true }) {
  const t = useTheme();
  const rtl = direction() === 'rtl';
  const w = useWheelStep();
  const ref = useRef(null);
  const drag = useRef(false);
  const latest = useRef(null);
  latest.current = { onChange, rtl };
  const at = (x) => {
    const [lx, , w] = lord.getLayout(ref.current.id);
    const f = Math.max(0, Math.min(1, (x - lx) / w));
    latest.current.onChange(min + (latest.current.rtl ? 1 - f : f) * (max - min));
  };
  const onKey = (e) => {
    const range = max - min;
    const v = rangeKey(e, { value, min, max, step: step || range / 20, page: Math.max(step || 0, range / 10), rtl });
    if (v == null) return false;
    if (v !== value) onChange(v);
    return true;
  };
  useEffect(() => {
    const a = addListener('pointermove', (e) => { if (drag.current) at(e.x); });
    const b = addListener('pointerup', () => { drag.current = false; });
    return () => { a(); b(); };
  }, []);
  const f = (value - min) / (max - min || 1);
  return (
    <View ref={ref} onMouseDown={(e) => { drag.current = true; at(e.x); }} focusable={focusable} onKeyDown={onKey}
      onFocus={w.onFocus} onBlur={w.onBlur} onWheel={(e) => w.wheel(e, { value, min, max, step: step || (max - min) / 20 }, onChange)}
      style={{ width, height: 20, borderRadius: 10, justifyContent: 'center', direction: 'ltr' }}>
      <View style={{ height: 4, borderRadius: 2, backgroundColor: t.control }}>
        <View style={{ width: Math.max(0, f * width), height: 4, borderRadius: 2, backgroundColor: t.accent, ...(rtl ? { marginLeft: Math.max(0, (1 - f) * width) } : null) }} />
      </View>
      <View style={{ position: 'absolute', left: (rtl ? 1 - f : f) * (width - 16), top: 2, width: 16, height: 16, borderRadius: 8,
        backgroundColor: '#fff', boxShadow: '0 1px 3px rgba(0,0,0,0.4)' }} />
    </View>
  );
}

// ---- context of each app's window ----

export const AppWindowContext = createContext({ id: null, focused: false, args: null });
export const useAppWindow = () => useContext(AppWindowContext);

// State of a window that survives the app being remounted (going fullscreen
// or picture-in-picture moves it in the tree). Cleared when the window closes.
const winStates = new Map();          // window id → Map(key → value)
windows.subscribe(() => {
  const ids = new Set(windows.get().map((w) => w.id));
  for (const k of [...winStates.keys()]) if (!ids.has(k)) winStates.delete(k);
});
export function useWindowState(key, initial) {
  const { id } = useAppWindow();
  if (!winStates.has(id)) winStates.set(id, new Map());
  const m = winStates.get(id);
  const [v, setV] = useState(() => (m.has(key) ? m.get(key) : typeof initial === 'function' ? initial() : initial));
  m.set(key, v);
  const set = useRef(null);
  if (!set.current) set.current = (x) => setV((prev) => { const n = typeof x === 'function' ? x(prev) : x; m.set(key, n); return n; });
  return [v, set.current];
}

// Files dragged from Files onto an app's window (apps/files: a View with onFilesDrop and onFilesOver). accepts(name)
// and folders say what the app takes (sdk/filedrop.js); onDrop({ files, folders }, how) gets only those, how.alt
// with ⌥ held. Spread `props` on the window's root View and draw <FilesDropHint> last with `over`, the sort of what is
// over the window now ({ files, folders, skipped, alt }) or null.
export function useFilesDrop({ accepts, folders = false, onDrop }) {
  const [over, setOver] = useState(null);
  // a folder on a network mount is not asked about (its stat can wait on the provider): it counts as a file
  const sorter = useRef(null);
  if (!sorter.current) sorter.current = dropSorter({ accepts, folders, isDir: (p) => !fsIsRemote(p) && !!fsStat(p)?.dir });
  const props = {
    onFilesOver: (paths, how) => {
      if (!paths) { setOver(null); return; }
      const s = sorter.current(paths), alt = !!how?.alt;
      setOver((o) => (o && o.sorted === s && o.alt === alt ? o : { ...s, sorted: s, alt }));
    },
    onFilesDrop: (paths, how) => {
      setOver(null);
      const s = sorter.current(paths);
      if (dropCount(s)) onDrop({ files: s.files, folders: s.folders }, how || {});
    },
  };
  return { over, props };
}

// The outline over the whole window while files are dragged over it: what letting go does (title) and, below it, what
// is left out (note). Drawn last, so it is above the panes.
export function FilesDropHint({ over, title, note }) {
  const t = useTheme();
  if (!over) return null;
  return (
    <View style={{ position: 'absolute', left: 8, top: 8, right: 8, bottom: 8, borderRadius: 10, pointerEvents: 'none', alignItems: 'center', justifyContent: 'center',
      gap: 6, paddingHorizontal: 16, border: `2px dashed ${t.accent}`, backgroundColor: 'rgba(10,132,255,0.10)' }}>
      <Text style={{ fontSize: 15, fontWeight: 700, color: t.text, textAlign: 'center' }}>{title}</Text>
      {note ? <Text style={{ fontSize: 12, color: t.textDim, textAlign: 'center' }}>{note}</Text> : null}
    </View>
  );
}

// The theme uses <AppHost id focused /> inside its window frame.
export function AppHost({ id, focused }) {
  const app = getApp(id);
  const args = useStore(windows, (ws) => ws.find((w) => w.id === id)?.args || null);
  const prot = useStore(windows, (ws) => !!ws.find((w) => w.id === id)?.protected);
  const mode = useStore(windows, (ws) => { const w = ws.find((x) => x.id === id); return w?.fullscreen ? 'fullscreen' : w?.pip ? 'pip' : 'window'; });
  // the window's overlay host (sdk/layers.js): lists and popovers of an isolated app's process go
  // here, above all of the window's content; in the desktop's process they go to the shell layers
  const [overlayHost, setOverlayHost] = useState(null);
  // the window's content is the area Tab goes round while it is focused (sdk/focus.js)
  const area = useFocusArea(id, focused, focusRingColor(useTheme()));
  useEffect(() => {                       // developer timing: open → first commit of this window
    const t0 = openedAt.get(id);
    if (t0 != null && app && !app.isolated) { openedAt.delete(id); console.log(`timing: ${app.id} window committed ${(lord.now() - t0).toFixed(1)} ms after wm.open`); }
  }, []);
  if (!app) return null;
  const C = app.Component;
  // the content goes in its own node: `protected` paints it black in screenshots (wm.setProtected)
  return (
    <AppWindowContext.Provider value={{ id, focused, args, mode, protected: prot, overlayHost }}>
      <View ref={area} protected={prot} style={{ flex: 1, alignSelf: 'stretch' }}>
        <C />
        <View ref={setOverlayHost} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, pointerEvents: 'box-none' }} />
      </View>
    </AppWindowContext.Provider>
  );
}

// Fullscreen and picture-in-picture windows (wm.setFullscreen / wm.setPip): above the
// theme, without its frame. startShell places them; the theme doesn't have to do anything.
// Fullscreen belongs to its desktop: on a hidden one it stays mounted and display: none, so the app
// keeps its state. Picture-in-picture is on all of them.
export function FloatingWindows() {
  const list = useStore(windows);
  const desktop = useStore(activeDesktop);
  const fs = list.filter((w) => w.fullscreen && !w.minimized);
  const pips = list.filter((w) => w.pip && !w.minimized && !w.fullscreen);
  const drag = useRef(null);
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      const d = drag.current;
      if (!d) return;
      const f = d.resize ? { ...d.frame, w: Math.max(160, d.frame.w + e.x - d.x), h: Math.max(90, d.frame.h + e.y - d.y) }
        : { ...d.frame, x: d.frame.x + e.x - d.x, y: d.frame.y + e.y - d.y };
      windows.set((ws) => ws.map((w) => (w.id === d.id ? { ...w, pip: f } : w)));
    });
    const offUp = addListener('pointerup', () => { drag.current = null; });
    const offKey = addListener('keydown', (e) => {
      if (e.key !== 'Escape' || hasTextFocus()) return;
      const top = windows.get().filter((w) => w.fullscreen && !w.capture && onActiveDesktop(w)).pop();
      if (top) windows.set((ws) => ws.map((w) => (w.id === top.id ? { ...w, fullscreen: false } : w)));
    });
    return () => { offMove(); offUp(); offKey(); };
  }, []);
  if (!fs.length && !pips.length) return null;
  return (
    <>
      {pips.map((w) => (
        <View key={w.id} style={{ position: 'absolute', left: w.pip.x, top: w.pip.y, width: w.pip.w, height: w.pip.h, borderRadius: 10,
          overflow: 'hidden', backgroundColor: '#000', boxShadow: '0 18px 48px rgba(0,0,0,0.55)', pointerEvents: 'auto' }}
          onMouseDown={(e) => { drag.current = { id: w.id, x: e.x, y: e.y, frame: w.pip }; }}>
          <AppHost id={w.id} focused={false} />
          <View onMouseDown={(e) => { e.stopPropagation?.(); drag.current = { id: w.id, x: e.x, y: e.y, frame: w.pip, resize: true }; }}
            style={{ position: 'absolute', right: 0, bottom: 0, width: 18, height: 18 }} />
        </View>
      ))}
      {fs.map((w) => {
        const d = displayFor(w);              // the display the window was on (docs/DISPLAYS.md)
        const here = onDesktop(w, desktop);
        // a capture window (Screenshots' ❖⇧4) goes above every layer. keepFocus: a press on it is not a
        // click elsewhere, so the menu, sheet or text field under the still stays as it was
        const win = (
        <View key={w.id} keepFocus={w.capture || undefined}
          style={{ position: 'absolute', left: d.x, top: d.y, width: d.w, height: d.h, backgroundColor: '#000', display: here ? 'flex' : 'none' }}>
          <AppHost id={w.id} focused={here} />
        </View>
        );
        return w.capture ? <Layer key={w.id} name="capture">{win}</Layer> : win;
      })}
    </>
  );
}

// Keyboard for the app whose window is in front (and with no active TextInput). While a
// ConfirmDialog has the keys (in this window or the desktop's own) the app gets none.
export function useKeyboard(handler) {
  const { id, focused: active } = useAppWindow();
  const ref = useRef(handler);
  ref.current = handler;
  useEffect(() => {
    if (!active) return undefined;
    return addListener('keydown', (e) => { if (!hasTextFocus() && !dialogStack.blocks(id, e)) ref.current(e); });
  }, [active, id]);
}
