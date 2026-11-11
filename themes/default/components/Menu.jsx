// Drop-down / context menu. Closes when clicking outside or with Escape. Keyboard (sdk/menunav.js): ↑/↓ move and
// wrap, skipping separators and disabled rows, Home/End jump, Enter/Space choose, → opens a submenu and ← closes it
// (mirrored in Arabic/Hebrew), Escape closes one level, and typing the first letters of a row goes to it. A menu
// opened from a bar steps to the bar's next or previous menu with → on a row without a submenu and ← in the top
// menu (onStep). A submenu opens on hover after a short pause and stays open while the pointer crosses other rows
// on its way to it (the safe triangle); it opens on the side with room, the reading direction's first. A menu
// taller than the screen scrolls (wheel or touchpad), and the keyboard keeps the highlighted item in view.
import { View, Text, addListener, useTheme, screen, direction, ScrollView, takeTextFocus, releaseTextFocus, focusToken, restoreFocus, textFocusOwner, Layer, displayAt, insideDisplay,
  SUB_DELAY, SUB_GRACE, selectable, stepIndex, edgeIndex, menuAction, typeAhead, matchIndex, subPlace, aiming } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';

const ROW = 24, SEP_H = 9, HEAD_H = 22;
const inside = (inst, ref) => {
  for (let n = inst; n; n = n.parent) if (n === ref?.current) return true;
  return false;
};
const rowH = (it) => (it === '-' ? SEP_H : it?.header ? HEAD_H : ROW);
// the menus holding the keyboard (token → key handler): their own Escape closes one level, not the whole menu
const keyTokens = new Map();
// A menu has the keyboard now: the shell's own Escape (closing its panels) leaves that one to the menu.
export const menuHasKeys = () => keyTokens.has(textFocusOwner());
// A press in a menu is not a press elsewhere: the menu keeps the keyboard (sdk/components.jsx drops it otherwise).
const keepKeys = () => { const o = textFocusOwner(); if (keyTokens.has(o)) takeTextFocus(o, keyTokens.get(o)); };

function Item({ item, onPick, active, onEnter }) {
  const t = useTheme();
  if (item === '-') return <View style={{ height: 1, marginVertical: 4, marginHorizontal: 8, backgroundColor: t.separator }} />;
  if (item.header) return <Text style={{ height: HEAD_H, paddingHorizontal: 15, paddingTop: 5, fontSize: 11, fontWeight: 600, color: t.textDim }}>{item.header}</Text>;
  const off = item.disabled;
  const lit = active && !off;
  return (
    <View onMouseEnter={onEnter} onClick={() => { if (!off) onPick(); }}
      style={{ height: ROW, paddingHorizontal: 10, marginHorizontal: 5, borderRadius: 5, flexDirection: 'row', alignItems: 'center',
        backgroundColor: lit ? t.accent : 'transparent' }}>
      <Text style={{ width: 16, fontSize: 12, color: lit ? '#fff' : t.text }}>{item.checked ? '✓' : ''}</Text>
      {item.dot ? <View style={{ width: 14, alignItems: 'center' }}><View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: item.dot }} /></View> : null}
      <Text style={{ flex: 1, fontSize: 13, color: lit ? '#fff' : off ? t.textDim : item.danger ? '#ff453a' : t.text, whiteSpace: 'nowrap' }}>{item.label}</Text>
      {item.hint ? <Text style={{ fontSize: 12, color: lit ? '#fff' : t.textDim }}>{item.hint}</Text> : null}
      {item.submenu ? <Text style={{ fontSize: 12, color: lit ? '#fff' : t.textDim }}>›</Text> : null}
    </View>
  );
}

// Width according to the longest text (translations change each item's length).
function fitWidth(items, min) {
  let w = min;
  for (const it of items) {
    if (it === '-' || !it?.label) continue;
    const need = 10 + 16 + lord.measureText(it.label, 13, 400) + (it.hint ? 18 + lord.measureText(it.hint, 12, 400) : 0) + 10 + 10 + (it.submenu ? 14 : 0);
    if (need > w) w = need;
  }
  return Math.min(Math.ceil(w), screen.width - 8);
}

const SUB_W = 160;

// above: y is the bottom edge of the menu (it opens upwards, as from the Dock).
// side: 'left' opens the menu to the left of x and 'right' to the right, regardless of the language
// (the side Dock; screen sides are physical). centered: y is the center of the menu.
// keys: opened from the keyboard, so its first row starts highlighted.
// onStep(dir): → on a row without a submenu (+1) or ← in the top menu (-1), for the bar that opened it.
// A submenu opened from the keyboard takes it (onBack gives it back); one opened by the pointer leaves it with its
// parent, whose ← or Escape closes it. onEnter: the pointer came into this submenu (its parent keeps it open).
// Every menu is drawn in the shell's popover layer (sdk/layers.js), above the Dock, the menu bar and
// the windows wherever it is rendered from; its submenus go in the same layer, beside it.
export function Menu({ x, y, items, onClose, anchor, width: minWidth = 220, above = false, sub = false, nodeRef, side = null, centered = false, onBack, keys = false, onStep, onEnter }) {
  const t = useTheme();
  const own = useRef(null);
  const ref = nodeRef || own;
  const subRef = useRef(null);
  const scrollRef = useRef(null);
  const [subMenu, setSubMenu] = useState(null);      // { items, x, y, side, row, keys }
  const [active, setActive] = useState(() => (keys ? edgeIndex(items) : -1));
  const rtl = direction() === 'rtl';
  // the display it opens on: the one under the point it was asked at (a menu bar's, the Dock's, a click's)
  const disp = displayAt(x, y);
  const width = Math.min(fitWidth(items, minWidth), disp.w - 8);
  // In Arabic/Hebrew x is the right edge: the menu opens to the left.
  if (side ? side === 'left' : rtl) x -= width;
  // Always inside that display (not spilling onto the next one); taller than it: it scrolls.
  const full = items.reduce((h, it) => h + rowH(it), 10);
  const height = Math.min(full, disp.h - 8);
  if (above) y -= height;
  else if (centered) y -= height / 2;
  ({ x, y } = insideDisplay(x, y, width, height, disp));
  const scrolls = full > height;

  const st = useRef({});
  st.current = { items, active, subMenu, onClose, onBack, onStep, rtl };
  const rowTop = (i) => items.slice(0, i).reduce((h, it) => h + rowH(it), 5);
  // beside this menu, on the side with room (sdk/menunav.js subPlace), level with its row
  const openSub = (i, withKeys) => {
    const it = st.current.items[i];
    if (!it?.submenu || it.disabled) return;
    const place = subPlace({ x, w: width }, Math.min(fitWidth(it.submenu, SUB_W), disp.w - 8), disp, st.current.rtl);
    setSubMenu({ items: it.submenu, x: place.x, side: place.side, y: y + rowTop(i) - 5 - (scrollRef.current?.offset?.() || 0), row: i, keys: withKeys });
  };
  const reveal = (i) => {
    const sv = scrollRef.current;
    if (!sv || i < 0) return;
    const top = rowTop(i), o = sv.offset();
    if (top < o) sv.scrollTo(top - 5);
    else if (top + ROW > o + height) sv.scrollTo(top + ROW - height + 5);
  };
  const highlight = (i) => { if (i >= 0) { setActive(i); reveal(i); } };

  // the pointer: a submenu opens (or the open one closes) after a pause on a row, longer while the pointer heads
  // for the open submenu across other rows
  const timer = useRef(null);
  const hover = useRef(-1);
  const trail = useRef({ from: null, to: null, at: 0 });
  const later = (fn, ms) => { clearTimeout(timer.current); timer.current = setTimeout(fn, ms); };
  const headingToSub = () => {
    const s = st.current.subMenu, p = trail.current;
    if (!s || !subRef.current || Date.now() - p.at > 100) return false;
    try { const [bx, by, bw, bh] = lord.getLayout(subRef.current.id); return aiming(p.from, p.to, { x: bx, y: by, w: bw, h: bh }, s.side); } catch (e) { return false; }
  };
  const settle = () => {
    if (headingToSub()) { later(settle, SUB_DELAY); return; }
    const i = hover.current, s = st.current.subMenu;
    if (i < 0 || (s && s.row === i)) return;
    if (st.current.items[i]?.submenu) openSub(i, false);
    else if (s) setSubMenu(null);
  };
  const enterRow = (i) => {
    hover.current = i;
    setActive(selectable(items[i]) ? i : -1);
    const s = st.current.subMenu;
    if (s && s.row === i) { clearTimeout(timer.current); return; }
    if (!s && !items[i]?.submenu) { clearTimeout(timer.current); return; }
    later(settle, s && headingToSub() ? SUB_GRACE : SUB_DELAY);
  };
  const moved = (e) => { trail.current = { from: trail.current.to, to: { x: e.x, y: e.y }, at: Date.now() }; };
  const enterSub = () => {
    clearTimeout(timer.current);
    const s = st.current.subMenu;
    if (s) { hover.current = s.row; setActive(s.row); }
  };
  useEffect(() => () => clearTimeout(timer.current), []);

  // the keyboard: the field that had it before the menu opened gets it back when the menu closes (not when a
  // submenu takes it), unless the click that closed the menu gave it to another one
  const typed = useRef(null);
  const [prev] = useState(focusToken);
  useEffect(() => {
    if (subMenu?.keys) return undefined;              // the submenu has the keyboard
    if (sub && !keys) return undefined;               // opened by the pointer: the parent keeps it
    const tok = {};
    const onKey = (e) => {
      const { items: list, active: a, subMenu: open, onClose: close, onBack: back, onStep: step, rtl: mirrored } = st.current;
      const act = menuAction(e, mirrored);
      if (act !== 'type') typed.current = null;
      if (act === 'next' || act === 'prev') { if (open) setSubMenu(null); highlight(stepIndex(list, a, act === 'next' ? 1 : -1)); }
      else if (act === 'first' || act === 'last') { if (open) setSubMenu(null); highlight(edgeIndex(list, act === 'last')); }
      else if (act === 'activate') {
        if (!selectable(list[a])) return;
        if (list[a].submenu) openSub(a, true);
        else { close(); list[a].onClick?.(); }
      } else if (act === 'open') {
        if (selectable(list[a]) && list[a].submenu) openSub(a, true);
        else if (step) step(1);
      } else if (act === 'back') {
        if (open) setSubMenu(null);
        else if (back) back();
        else if (step) step(-1);
      } else if (act === 'close') {
        if (open) setSubMenu(null);
        else if (back) back();
        else close();
      } else if (act === 'type') {
        typed.current = typeAhead(typed.current, e.char, Date.now());
        highlight(matchIndex(list, a, typed.current.buf));
      }
    };
    keyTokens.set(tok, onKey);
    takeTextFocus(tok, onKey);
    return () => { keyTokens.delete(tok); releaseTextFocus(tok); };
  }, [!!subMenu?.keys, keys]);
  // after the effect above in the list, so this menu's keys are released first; a submenu still holding them lets
  // them go after this (and leaves the field's focus as it is)
  useEffect(() => () => { const o = textFocusOwner(); if (!sub && (o === null || keyTokens.has(o))) restoreFocus(prev); }, []);
  useEffect(() => {
    if (sub) return undefined;              // the submenu is closed by its parent menu
    const offPtr = addListener('pointerdown', (e) => { if (!inside(e.target, ref) && !inside(e.target, subRef) && !inside(e.target, anchor)) onClose(); });
    // the menu holding the keyboard answers Escape itself (one level); this is for when something else holds it
    const offKey = addListener('keydown', (e) => { if (e.key === 'Escape' && !keyTokens.has(textFocusOwner())) onClose(); });
    return () => { offPtr(); offKey(); };
  }, [onClose]);
  const rows = items.map((it, i) => (
    <Item key={i} item={it} active={i === active} onEnter={() => enterRow(i)}
      onPick={() => { if (it.submenu) { clearTimeout(timer.current); openSub(i, false); } else { onClose(); it.onClick?.(); } }} />
  ));
  const body = (
    <>
    <View ref={ref} onMouseDown={keepKeys} onMouseMove={moved} onMouseEnter={onEnter} style={{
      position: 'absolute', left: x, top: y, width, height: scrolls ? height : undefined, paddingVertical: scrolls ? 0 : 5, borderRadius: 9,
      backgroundColor: t.panelBg, backdropFilter: `blur(${t.blur}px)`, border: `1px solid ${t.windowBorder}`,
      boxShadow: '0 12px 32px rgba(0,0,0,0.35)', overflow: 'hidden',
    }}>
      {scrolls
        ? <ScrollView scrollRef={scrollRef} style={{ height }}><View style={{ paddingVertical: 5 }}>{rows}</View></ScrollView>
        : rows}
    </View>
    {subMenu && <Menu key={subMenu.row} sub nodeRef={subRef} x={subMenu.x} y={subMenu.y} side="right" items={subMenu.items} onClose={onClose} width={SUB_W}
      keys={subMenu.keys} onStep={onStep} onEnter={enterSub} onBack={subMenu.keys ? () => setSubMenu(null) : undefined} />}
    </>
  );
  return sub ? body : <Layer name="popover">{body}</Layer>;
}
