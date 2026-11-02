// components.jsx — base controls for apps. They take colors from useTheme(), so they
// adapt to any theme.
import { View, Text, addListener, displayFor, onCommit, onScreenChange } from '@lordos/runtime';
import { createContext, useContext, useEffect, useRef, useState } from 'react';
import { useTheme } from './theme.js';
import { getApp, windows, openedAt, wm, activeDesktop, onActiveDesktop } from './windows.js';
import { onDesktop } from './desktopmodel.js';
import { useStore } from './store.js';
import { textDirection, direction } from './i18n.js';
import { SCROLLBAR, scrollbarMode, thumbGeometry, offsetForThumb, nearEdge, hitThumb, barShown, barOpacity, hideTime, barWidth } from './scrollbar.js';
import { ime, useIme, Preedit } from './ime.js';
import { noteTextField } from './textfocus.js';

// Push button. Optional (all additive): small (22 px high), plain (no fill until hover), disabled,
// destructive (red text) and icon (a glyph before the label). See docs/DESIGN.md.
export function Button({ label, onClick, primary, style, small, plain, disabled, destructive, icon }) {
  const t = useTheme();
  const [hover, setHover] = useState(false);
  const fg = primary ? '#fff' : destructive ? '#ff453a' : t.text;
  const bg = primary ? t.accent : plain ? (hover && !disabled ? t.hover : 'transparent') : t.control;
  const border = t.buttonBorder && !plain ? { border: `1px solid ${primary ? t.accent : t.separator}` } : null;
  return (
    <View onClick={disabled ? undefined : onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
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
export function Toggle({ on, onChange, small }) {
  const t = useTheme();
  const w = small ? 36 : 42, h = small ? 20 : 24, k = h - 4;
  return (
    <View onClick={() => onChange(!on)} style={{ width: w, height: h, borderRadius: h / 2, direction: 'ltr',
      backgroundColor: on ? t.accent : t.control }}>
      <View style={{ position: 'absolute', top: 2, left: on ? w - k - 2 : 2, width: k, height: k, borderRadius: k / 2,
        backgroundColor: '#fff', boxShadow: '0 1px 3px rgba(0,0,0,0.35)' }} />
    </View>
  );
}

// appearance: 'accent' (default: the selected segment is filled with the accent) or 'neutral'
// (view switcher: a raised neutral segment). small: 22 px high instead of 28. fill: the
// segments share the full width of the control.
export function Segmented({ options, value, onChange, appearance = 'accent', small, fill, style }) {
  const t = useTheme();
  const neutral = appearance === 'neutral';
  const raised = t.mode === 'dark' ? 'rgba(255,255,255,0.18)' : '#ffffff';
  return (
    <View style={[{ flexDirection: 'row', backgroundColor: t.control, borderRadius: small ? t.controlRadiusSmall : t.controlRadius + 1, padding: 2 }, style]}>
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
      <Text style={{ flex: 1, fontSize: 13, fontWeight: selected && ind ? 600 : 500, color: filled ? '#fff' : t.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{label}</Text>
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
export const SCROLL_GUTTER = 11;
// A TextInput asks the ScrollView it is in to show its caret: on focus, on typing, and when the view
// changes height, as a window does when it shrinks for the on-screen keyboard. A wheel scroll with a
// field focused doesn't ask.
const ScrollReveal = createContext(null);
export function ScrollView({ style, contentStyle, children, scrollRef, gutter }) {
  const t = useTheme();
  const mode = scrollbarMode(t.scrollBars);
  const reduce = !!t.reduceMotion;
  const rtl = direction() === 'rtl';
  const outer = useRef(null), inner = useRef(null), thumb = useRef(null);
  const [offset, setOffset] = useState(0);
  const [metrics, setMetrics] = useState({ view: 0, content: 0 });
  const [ui, setUi] = useState({ near: false, hover: false, dragging: false, shown: mode === 'always' });
  const st = useRef(null);
  if (!st.current) st.current = { lastScroll: null, hideTimer: 0, fadeTimer: 0, grab: 0, ui };
  st.current.ui = ui; st.current.mode = mode; st.current.reduce = reduce; st.current.metrics = metrics;
  const measure = () => {
    if (!outer.current || !inner.current) return null;
    const view = lord.getLayout(outer.current.id)[3], content = lord.getLayout(inner.current.id)[3];
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
      const [, oy] = lord.getLayout(outer.current.id);
      const m = s.metrics;
      setOffset(offsetForThumb(m.view, m.content, e.y - oy - s.grab));
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

  const jump = (v) => {
    const m = measure() || metrics;
    const to = Math.max(0, Math.min(Math.max(0, m.content - m.view), v));
    if (Math.round(to) !== Math.round(y)) scrolled();
    setOffset(Math.max(0, v));
  };
  if (scrollRef) scrollRef.current = { scrollTo: jump, offset: () => y, inner };
  const latestReveal = useRef(null);
  latestReveal.current = (c, always) => {
    if (!outer.current) return;
    const [, oy, , h] = lord.getLayout(outer.current.id);
    const resized = st.current.fieldView !== h;
    st.current.fieldView = h;
    if (!always && !resized) return;
    const below = c.y + c.h + 12 - (oy + h), above = oy + 4 - c.y;
    if (below > 0) jump(y + below); else if (above > 0) jump(y - above);
  };
  const reveal = useRef((c, always) => latestReveal.current?.(c, always)).current;

  const onMove = (e) => {
    if (!outer.current) return;
    if (!geom) measure();                       // content that grew by itself has told nobody
    const [ox, oy, ow] = lord.getLayout(outer.current.id);
    const near = !!geom && nearEdge(e.x - ox, ow, rtl);
    const hover = !!geom && st.current.ui.shown && hitThumb(e.x - ox, e.y - oy, ow, geom, rtl);
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
  const pad = gutter && mode === 'always' ? { paddingInlineEnd: SCROLL_GUTTER } : null;
  return (
    <View ref={outer} onWheel={(e) => {
      const m = measure() || metrics;
      const mx = Math.max(0, m.content - m.view);
      if (mx > 0 && e.deltaY) scrolled();
      setOffset((o) => Math.max(0, Math.min(mx, Math.min(o, max) + e.deltaY)));
    }} onMouseMove={onMove} onMouseLeave={() => { st.current.ui = { ...st.current.ui, near: false, hover: false }; update({ near: false, hover: false }); schedule(); }}
      style={[{ overflow: 'hidden', flex: 1 }, style]}>
      <View ref={inner} style={[{ translateY: -y }, pad, contentStyle]}><ScrollReveal.Provider value={reveal}>{children}</ScrollReveal.Provider></View>
      {geom && (
        <View onMouseDown={(e) => {
          if (!outer.current) return;
          const [, oy] = lord.getLayout(outer.current.id);
          e.stopPropagation();
          st.current.grab = e.y - oy - geom.top;
          st.current.ui = { ...st.current.ui, dragging: true };
          show({ dragging: true });
        }} style={{ position: 'absolute', end: 0, top: geom.top, width: SCROLLBAR.hit, height: geom.size,
          pointerEvents: shown || ui.near ? 'auto' : 'none' }}>
          <View ref={thumb} style={{ position: 'absolute', end: SCROLLBAR.inset, top: 0, height: geom.size, width: w,
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
// A form: Tab and Shift+Tab go round its text fields, in the order they mounted. Without one, Tab does nothing.
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

// onKeyDown(e) optional: return true if the key was already handled (e.g. Shift+Enter).
// secure: password field (shows •, does not go through the input method and cannot be copied).
// selectAll: starts with all the text selected (the first key replaces it; the arrows
// deselect it), as when renaming in Files.
export function TextInput({ value, onChange, placeholder, onSubmit, onCancel, onKeyDown, autoFocus, style, secure, size = 13, selectAll = false }) {
  const t = useTheme();
  const token = useRef({}).current;
  const [isFocused, setIsFocused] = useState(false);
  const [blink, setBlink] = useState(true);
  const latest = useRef();
  const caret = useRef(null);
  const secureRef = useRef(secure);
  secureRef.current = secure;
  const [allSel, setAllSel] = useState(selectAll);
  const allRef = useRef(selectAll);
  allRef.current = allSel;
  latest.current = { value, onChange, onSubmit, onCancel, onKeyDown };
  const order = useContext(FieldOrder);

  useEffect(() => {
    // with focus, keys go through the input method (Korean, Japanese, Chinese)
    const caretRect = () => { if (!caret.current) return null; const [x, y, , h] = lord.getLayout(caret.current.id); return { x, y, h }; };
    const sync = () => {
      setIsFocused(focused === token);
      if (focused === token && !secureRef.current) ime.attach(token, caretRect, { inline: true }); else ime.detach(token);
    };
    focusSubs.add(sync);
    carets.set(token, caretRect);
    handlers.set(token, (e) => {
      const p = latest.current;
      if (p.onKeyDown && p.onKeyDown(e)) { setBlink(true); return; }
      if (e.key === 'Tab' && order && order.length > 1 && !e.ctrl && !e.meta && !e.alt) {
        const i = order.indexOf(token);
        setFocus(order[(i + (e.shift ? order.length - 1 : 1)) % order.length]);
        return;
      }
      const all = allRef.current;
      if (all && (e.char || e.key === 'Backspace' || e.key === 'Delete' || ((e.ctrl || e.meta) && e.base === 'v'))) { setAllSel(false); allRef.current = false; p.value = ''; }
      else if (all && /^Arrow|^Home$|^End$/.test(e.key || '')) { setAllSel(false); allRef.current = false; return; }
      if ((e.ctrl || e.meta) && e.base === 'v') { p.onChange(p.value + lord.clipboard.get().replace(/\n/g, ' ')); return; }
      if (e.key === 'Backspace' || e.key === 'Delete') p.onChange(all ? '' : e.key === 'Delete' ? p.value : [...p.value].slice(0, -1).join(''));
      else if (e.key === 'Enter') p.onSubmit?.(p.value);
      else if (e.key === 'Escape') p.onCancel?.();
      else if (e.char) p.onChange(p.value + e.char);
      setBlink(true);
    });
    order?.push(token);
    if (autoFocus) setFocus(token);
    return () => { if (order) order.splice(order.indexOf(token), 1); focusSubs.delete(sync); handlers.delete(token); carets.delete(token); ime.detach(token); if (focused === token) setFocus(null); };
  }, []);
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

  return (
    <View onMouseDown={() => { focusedNow = true; setFocus(token); }}
      style={[{ height: Math.max(28, size + 15), paddingHorizontal: 10, borderRadius: t.controlRadius, flexDirection: 'row', alignItems: 'center',
        // the cursor goes to the logical end: on the left if the text is Arabic or Hebrew
        direction: secure ? 'ltr' : value ? textDirection(value) : 'inherit',
        backgroundColor: t.inputBg, border: `1px solid ${isFocused && t.inputFocus !== 'underline' ? t.accent : t.separator}`, overflow: 'hidden' }, style]}>
      {/* inputFocus 'underline' (a theme token): an accent line along the bottom edge */}
      {isFocused && t.inputFocus === 'underline' ? <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 2, backgroundColor: t.accent, pointerEvents: 'none' }} /> : null}
      {value ? <Text style={{ fontSize: size, color: allSel && isFocused ? '#fff' : t.text, whiteSpace: 'nowrap',
        backgroundColor: allSel && isFocused ? t.accent : 'transparent' }}>{secure ? '•'.repeat([...value].length) : value}</Text> : null}
      {preedit && !secure ? <Preedit st={imeSt} size={size} color={t.text} accent={t.accent} /> : null}
      {isFocused && <View ref={caret} style={{ width: 1.5, height: size + 3, marginLeft: 1, backgroundColor: blink ? t.accent : 'transparent' }} />}
      {!value && !preedit && <Text style={{ fontSize: size, color: t.textDim, whiteSpace: 'nowrap' }}>{placeholder || ''}</Text>}
    </View>
  );
}

// Number with − and + buttons (e.g. font size).
export function Stepper({ value, onChange, min = -Infinity, max = Infinity, step = 1, format = (v) => String(v), width = 56 }) {
  const t = useTheme();
  const set = (v) => onChange(Math.max(min, Math.min(max, Math.round(v / step) * step)));
  const Btn = ({ label, d }) => (
    <View onClick={() => set(value + d)} style={{ width: 26, height: 26, alignItems: 'center', justifyContent: 'center' }}>
      <Text style={{ fontSize: 15, fontWeight: 600, color: t.text }}>{label}</Text>
    </View>
  );
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', backgroundColor: t.control, borderRadius: t.controlRadius, height: 26 }}>
      <Btn label="−" d={-step} />
      <Text style={{ width, textAlign: 'center', fontSize: 12, fontWeight: 600, color: t.text }}>{format(value)}</Text>
      <Btn label="+" d={step} />
    </View>
  );
}

// Horizontal slider (drag or click on the bar).
export function Slider({ value, onChange, min = 0, max = 1, width = 160 }) {
  const t = useTheme();
  const ref = useRef(null);
  const drag = useRef(false);
  const latest = useRef(onChange);
  latest.current = onChange;
  const at = (x) => {
    const [lx, , w] = lord.getLayout(ref.current.id);
    const f = Math.max(0, Math.min(1, (x - lx) / w));
    latest.current(min + f * (max - min));
  };
  useEffect(() => {
    const a = addListener('pointermove', (e) => { if (drag.current) at(e.x); });
    const b = addListener('pointerup', () => { drag.current = false; });
    return () => { a(); b(); };
  }, []);
  const f = (value - min) / (max - min || 1);
  return (
    <View ref={ref} onMouseDown={(e) => { drag.current = true; at(e.x); }} style={{ width, height: 20, justifyContent: 'center', direction: 'ltr' }}>
      <View style={{ height: 4, borderRadius: 2, backgroundColor: t.control }}>
        <View style={{ width: Math.max(0, f * width), height: 4, borderRadius: 2, backgroundColor: t.accent }} />
      </View>
      <View style={{ position: 'absolute', left: f * (width - 16), top: 2, width: 16, height: 16, borderRadius: 8,
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

// The theme uses <AppHost id focused /> inside its window frame.
export function AppHost({ id, focused }) {
  const app = getApp(id);
  const args = useStore(windows, (ws) => ws.find((w) => w.id === id)?.args || null);
  const prot = useStore(windows, (ws) => !!ws.find((w) => w.id === id)?.protected);
  const mode = useStore(windows, (ws) => { const w = ws.find((x) => x.id === id); return w?.fullscreen ? 'fullscreen' : w?.pip ? 'pip' : 'window'; });
  // the window's overlay host (sdk/layers.js): lists and popovers of an isolated app's process go
  // here, above all of the window's content; in the desktop's process they go to the shell layers
  const [overlayHost, setOverlayHost] = useState(null);
  useEffect(() => {                       // developer timing: open → first commit of this window
    const t0 = openedAt.get(id);
    if (t0 != null && app && !app.isolated) { openedAt.delete(id); console.log(`timing: ${app.id} window committed ${(lord.now() - t0).toFixed(1)} ms after wm.open`); }
  }, []);
  if (!app) return null;
  const C = app.Component;
  // the content goes in its own node: `protected` paints it black in screenshots (wm.setProtected)
  return (
    <AppWindowContext.Provider value={{ id, focused, args, mode, protected: prot, overlayHost }}>
      <View protected={prot} style={{ flex: 1, alignSelf: 'stretch' }}>
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
      const top = windows.get().filter((w) => w.fullscreen && onActiveDesktop(w)).pop();
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
        return (
        <View key={w.id} style={{ position: 'absolute', left: d.x, top: d.y, width: d.w, height: d.h, backgroundColor: '#000', display: here ? 'flex' : 'none' }}>
          <AppHost id={w.id} focused={here} />
        </View>
        );
      })}
    </>
  );
}

// Keyboard for the app whose window is in front (and with no active TextInput).
export function useKeyboard(handler) {
  const { focused: active } = useAppWindow();
  const ref = useRef(handler);
  ref.current = handler;
  useEffect(() => {
    if (!active) return undefined;
    return addListener('keydown', (e) => { if (!hasTextFocus()) ref.current(e); });
  }, [active]);
}
