// Drop-down / context menu. Closes when clicking outside or with Escape. Keyboard: ↑/↓ move,
// Home/End jump, Enter/Space choose, → opens a submenu and ← closes it. A menu taller than the
// screen scrolls (wheel or touchpad), and the keyboard keeps the highlighted item in view.
import { View, Text, addListener, useTheme, screen, direction, ScrollView, takeTextFocus, releaseTextFocus, focusToken, restoreFocus, Layer, displayAt, insideDisplay } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';

const ROW = 24, SEP_H = 9, HEAD_H = 22;
const inside = (inst, ref) => {
  for (let n = inst; n; n = n.parent) if (n === ref?.current) return true;
  return false;
};
const rowH = (it) => (it === '-' ? SEP_H : it?.header ? HEAD_H : ROW);
const selectable = (it) => it && it !== '-' && !it.header && !it.disabled;

function Item({ item, onClose, onSub, active, onHover }) {
  const t = useTheme();
  const self = useRef(null);
  if (item === '-') return <View style={{ height: 1, marginVertical: 4, marginHorizontal: 8, backgroundColor: t.separator }} />;
  if (item.header) return <Text style={{ height: HEAD_H, paddingHorizontal: 15, paddingTop: 5, fontSize: 11, fontWeight: 600, color: t.textDim }}>{item.header}</Text>;
  const off = item.disabled;
  const lit = active && !off;
  const enter = () => {
    onHover();
    if (item.submenu && self.current) { const [x, y, w] = lord.getLayout(self.current.id); onSub({ items: item.submenu, x: x + w - 4, y: y - 5 }); }
    else onSub(null);
  };
  return (
    <View ref={self} onMouseEnter={enter}
      onClick={() => { if (!off && !item.submenu) { onClose(); item.onClick?.(); } }}
      style={{ height: ROW, paddingHorizontal: 10, marginHorizontal: 5, borderRadius: 5, flexDirection: 'row', alignItems: 'center',
        backgroundColor: lit ? t.accent : 'transparent' }}>
      <Text style={{ width: 16, fontSize: 12, color: lit ? '#fff' : t.text }}>{item.checked ? '✓' : ''}</Text>
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

// above: y is the bottom edge of the menu (it opens upwards, as from the Dock).
// side: 'left' opens the menu to the left of x and 'right' to the right, regardless of the language
// (the side Dock; screen sides are physical). centered: y is the center of the menu.
// keys: this menu has the keyboard (a submenu takes it while it is open; onBack gives it back).
// Every menu is drawn in the shell's popover layer (sdk/layers.js), above the Dock, the menu bar and
// the windows wherever it is rendered from; its submenus go in the same layer, beside it.
export function Menu({ x, y, items, onClose, anchor, width: minWidth = 220, above = false, sub = false, nodeRef, side = null, centered = false, onBack }) {
  const t = useTheme();
  const own = useRef(null);
  const ref = nodeRef || own;
  const subRef = useRef(null);
  const scrollRef = useRef(null);
  const [subMenu, setSubMenu] = useState(null);      // { items, x, y, keys }
  const [active, setActive] = useState(-1);
  // the display it opens on: the one under the point it was asked at (a menu bar's, the Dock's, a click's)
  const disp = displayAt(x, y);
  const width = Math.min(fitWidth(items, minWidth), disp.w - 8);
  // In Arabic/Hebrew x is the right edge: the menu opens to the left.
  if (side ? side === 'left' : direction() === 'rtl') x -= width;
  // Always inside that display (not spilling onto the next one); taller than it: it scrolls.
  const full = items.reduce((h, it) => h + rowH(it), 10);
  const height = Math.min(full, disp.h - 8);
  if (above) y -= height;
  else if (centered) y -= height / 2;
  ({ x, y } = insideDisplay(x, y, width, height, disp));
  const scrolls = full > height;

  // the keyboard
  const st = useRef({});
  st.current = { items, active, subMenu, onClose, onBack };
  const rowTop = (i) => items.slice(0, i).reduce((h, it) => h + rowH(it), 5);
  const openSub = (i) => {
    const it = items[i];
    if (!it?.submenu) return;
    setSubMenu({ items: it.submenu, x: x + width - 4, y: y + rowTop(i) - 5 - (scrollRef.current?.offset?.() || 0), keys: true });
  };
  const reveal = (top) => {
    const sv = scrollRef.current;
    if (!sv) return;
    const o = sv.offset();
    if (top < o) sv.scrollTo(top - 5);
    else if (top + ROW > o + height) sv.scrollTo(top + ROW - height + 5);
  };
  const move = (dir) => {
    const { items: list, active: a } = st.current;
    const n = list.length;
    for (let k = 1; k <= n; k++) {
      const i = ((a < 0 ? (dir > 0 ? -1 : 0) : a) + dir * k + n * 2) % n;
      if (selectable(list[i])) { setActive(i); reveal(rowTop(i)); return; }
    }
  };
  useEffect(() => {
    if (subMenu?.keys) return undefined;              // the submenu has the keyboard
    const prev = focusToken();
    const tok = {};
    const onKey = (e) => {
      const { items: list, active: a, onClose: close, onBack: back } = st.current;
      const k = e.key;
      if (k === 'ArrowDown') move(1);
      else if (k === 'ArrowUp') move(-1);
      else if (k === 'Home') { setActive(-1); setTimeout(() => move(1), 0); }
      else if (k === 'End') { setActive(0); setTimeout(() => move(-1), 0); }
      else if (k === 'ArrowRight' && list[a]?.submenu) openSub(a);
      else if (k === 'ArrowLeft' && back) back();
      else if ((k === 'Enter' || k === ' ' || k === 'Space') && selectable(list[a])) {
        if (list[a].submenu) openSub(a);
        else { close(); list[a].onClick?.(); }
      }
    };
    takeTextFocus(tok, onKey);
    return () => { releaseTextFocus(tok); if (!sub) restoreFocus(prev); };
  }, [!!subMenu?.keys]);
  useEffect(() => {
    if (sub) return undefined;              // the submenu is closed by its parent menu
    const offPtr = addListener('pointerdown', (e) => { if (!inside(e.target, ref) && !inside(e.target, subRef) && !inside(e.target, anchor)) onClose(); });
    const offKey = addListener('keydown', (e) => { if (e.key === 'Escape') onClose(); });
    return () => { offPtr(); offKey(); };
  }, [onClose]);
  const rows = items.map((it, i) => (
    <Item key={i} item={it} onClose={onClose} active={i === active}
      onHover={() => setActive(selectable(it) ? i : -1)} onSub={(s) => setSubMenu(s ? { ...s, keys: false } : null)} />
  ));
  const body = (
    <>
    <View ref={ref} style={{
      position: 'absolute', left: x, top: y, width, height: scrolls ? height : undefined, paddingVertical: scrolls ? 0 : 5, borderRadius: 9,
      backgroundColor: t.panelBg, backdropFilter: `blur(${t.blur}px)`, border: `1px solid ${t.windowBorder}`,
      boxShadow: '0 12px 32px rgba(0,0,0,0.35)', overflow: 'hidden',
    }}>
      {scrolls
        ? <ScrollView scrollRef={scrollRef} style={{ height }}><View style={{ paddingVertical: 5 }}>{rows}</View></ScrollView>
        : rows}
    </View>
    {subMenu && <Menu sub nodeRef={subRef} x={subMenu.x} y={subMenu.y} items={subMenu.items} onClose={onClose} width={160}
      onBack={subMenu.keys ? () => setSubMenu(null) : undefined} />}
    </>
  );
  return sub ? body : <Layer name="popover">{body}</Layer>;
}
