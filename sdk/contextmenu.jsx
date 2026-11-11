// contextmenu.jsx — a right-click menu inside a window, with submenus (Files, Terminal, Editor, Notes):
//
//   <ContextMenu x={…} y={…} items={[…]} onClose={…} bounds={{ w, h }} />
//
// x, y and bounds are in the window's coordinates. A click outside closes it. Keyboard (sdk/menunav.js): ↑/↓ move
// and wrap, skipping separators, headers and disabled rows, Home/End jump, Enter/Space choose, → opens a submenu
// and ← closes it (mirrored in Arabic/Hebrew), Escape closes one level, and typing the first letters of a row goes
// to it. A submenu opens on hover after a short pause, stays open while the pointer crosses other rows on its way
// to it (the safe triangle) and opens on the side with room, the reading direction's first. The field that had
// the keyboard gets it back when the menu closes.
import { useEffect, useRef, useState } from 'react';
import { View, Text, addListener } from '@lordos/runtime';
import { useTheme } from './theme.js';
import { direction } from './i18n.js';
import { takeTextFocus, releaseTextFocus, focusToken, restoreFocus, textFocusOwner } from './components.jsx';
import { SUB_DELAY, SUB_GRACE, selectable, stepIndex, edgeIndex, menuAction, typeAhead, matchIndex, subPlace, aiming } from './menunav.js';

const inside = (inst, ref) => { for (let n = inst; n; n = n.parent) if (n === ref?.current) return true; return false; };
const ITEM_H = 24;
const rowH = (it) => (it === '-' ? 9 : it.header ? 20 : it.swatches ? ITEM_H + 4 : ITEM_H);
const heightOf = (items) => items.reduce((s, it) => s + rowH(it), 10);
const rowTop = (items, i) => 5 + items.slice(0, i).reduce((s, q) => s + rowH(q), 0);
// where a box of this size goes, inside the bounds
const boxAt = (items, x, y, width, bounds) => ({
  left: Math.max(4, Math.min(x, bounds.w - width - 4)),
  top: Math.max(4, Math.min(y, bounds.h - heightOf(items) - 4)),
});

function MenuBox({ items, left, top, width, active, onRow, onPick, onMove, onEnter, onDown, boxRef }) {
  const t = useTheme();
  return (
    <View ref={boxRef} onMouseDown={onDown} onMouseMove={onMove} onMouseEnter={onEnter} style={{ position: 'absolute', left, top, width, paddingVertical: 5, borderRadius: 8, backgroundColor: t.mode === 'dark' ? '#2c2c31' : '#ffffff',
      border: `1px solid ${t.separator}`, boxShadow: '0 10px 30px rgba(0,0,0,0.35)' }}>
      {items.map((it, i) => {
        if (it === '-') return <View key={i} style={{ height: 1, marginVertical: 4, marginHorizontal: 8, backgroundColor: t.separator }} />;
        if (it.header) return <Text key={i} style={{ height: 20, paddingHorizontal: 14, paddingTop: 4, fontSize: 11, fontWeight: 600, color: t.textDim }}>{it.header}</Text>;
        if (it.swatches) {
          return (
            <View key={i} onMouseEnter={() => onRow(i)} style={{ flexDirection: 'row', gap: 8, height: ITEM_H + 4, alignItems: 'center', paddingHorizontal: 14 }}>
              {it.swatches.map((s, j) => (
                <View key={s.color || s.hex || j} onClick={() => onPick(i, s)} style={{ width: 16, height: 16, borderRadius: 8, backgroundColor: s.hex || 'transparent',
                  border: s.on ? `2px solid ${t.text}` : s.hex ? '0 solid transparent' : `1.5px solid ${t.textDim}` }} />
              ))}
            </View>
          );
        }
        const on = active === i && !it.disabled;
        return (
          <View key={i} onMouseEnter={() => onRow(i)}
            onClick={() => { if (!it.disabled) onPick(i); }}
            style={{ height: ITEM_H, marginHorizontal: 5, paddingHorizontal: 9, borderRadius: 5, flexDirection: 'row', alignItems: 'center', backgroundColor: on ? t.accent : 'transparent' }}>
            <Text style={{ width: 16, fontSize: 11, color: on ? '#fff' : t.text }}>{it.checked ? '✓' : ''}</Text>
            {it.dot ? <View style={{ width: 14, alignItems: 'center' }}><View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: it.dot }} /></View> : null}
            <Text style={{ flex: 1, fontSize: 13, color: on ? '#fff' : it.disabled ? t.textDim : it.danger ? '#ff453a' : t.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{it.label}</Text>
            {it.hint ? <Text style={{ fontSize: 11, color: on ? '#fff' : t.textDim, marginHorizontal: 4 }}>{it.hint}</Text> : null}
            {it.submenu ? <Text style={{ fontSize: 12, color: on ? '#fff' : t.textDim }}>›</Text> : null}
          </View>
        );
      })}
    </View>
  );
}

// Context menu: items [{ label, onClick, hint, checked, dot (a color), disabled, danger, submenu } | '-' | { header } | { swatches }]
export function ContextMenu({ x, y, items, onClose, bounds = { w: 4000, h: 4000 } }) {
  const ref = useRef(null), subRef = useRef(null);
  const [sub, setSub] = useState(null);           // { items, left, top, width, side, row, keys }
  const [active, setActive] = useState(-1);
  const [subActive, setSubActive] = useState(-1);
  items = items.filter(Boolean);
  const width = (list) => Math.min(320, Math.max(190, ...list.filter((i) => i && i.label).map((i) => 46 + lord.measureText(i.label, 13, 400) + (i.hint ? 16 + lord.measureText(i.hint, 11, 400) : 0))));
  const w = width(items);
  const { left, top } = boxAt(items, x, y, w, bounds);
  const st = useRef({});
  st.current = { items, active, sub, subActive, onClose };

  // beside the menu, on the side with room (sdk/menunav.js subPlace), level with its row
  const openSub = (i, keys) => {
    const it = st.current.items[i];
    if (!it?.submenu || it.disabled) return;
    const sw = width(it.submenu);
    const place = subPlace({ x: left, w }, sw, { x: 0, w: bounds.w }, direction() === 'rtl');
    const at = boxAt(it.submenu, place.x, top + rowTop(items, i) - 5, sw, bounds);
    setSub({ items: it.submenu, left: at.left, top: at.top, width: sw, side: place.side, row: i, keys });
    setSubActive(keys ? edgeIndex(it.submenu) : -1);
  };
  const closeSub = () => { setSub(null); setSubActive(-1); };
  const pick = (list, i, swatch) => {
    const it = list[i];
    if (swatch) { onClose(); swatch.onClick(); return; }
    if (!selectable(it)) return;
    if (it.submenu) openSub(i, false);
    else { onClose(); it.onClick?.(); }
  };

  // the pointer: a submenu opens (or the open one closes) after a pause on a row, longer while the pointer heads
  // for the open submenu across other rows
  const timer = useRef(null), hover = useRef(-1), trail = useRef({ from: null, to: null, at: 0 });
  const later = (fn, ms) => { clearTimeout(timer.current); timer.current = setTimeout(fn, ms); };
  const headingToSub = () => {
    const s = st.current.sub, p = trail.current;
    if (!s || !subRef.current || Date.now() - p.at > 100) return false;
    try { const [bx, by, bw, bh] = lord.getLayout(subRef.current.id); return aiming(p.from, p.to, { x: bx, y: by, w: bw, h: bh }, s.side); } catch (e) { return false; }
  };
  const settle = () => {
    if (headingToSub()) { later(settle, SUB_DELAY); return; }
    const i = hover.current, s = st.current.sub;
    if (i < 0 || (s && s.row === i)) return;
    if (st.current.items[i]?.submenu) openSub(i, false);
    else if (s) closeSub();
  };
  const enterRow = (i) => {
    hover.current = i;
    setActive(selectable(items[i]) ? i : -1);
    const s = st.current.sub;
    if ((s && s.row === i) || (!s && !items[i]?.submenu)) { clearTimeout(timer.current); return; }
    later(settle, s && headingToSub() ? SUB_GRACE : SUB_DELAY);
  };
  const moved = (e) => { trail.current = { from: trail.current.to, to: { x: e.x, y: e.y }, at: Date.now() }; };
  const enterSub = () => { clearTimeout(timer.current); const s = st.current.sub; if (s) { hover.current = s.row; setActive(s.row); } };

  // the keyboard: the open submenu's level when it was opened from the keyboard, else the menu's
  const typed = useRef(null);
  const holder = useRef(null);                     // { tok, onKey }
  // a press in the menu is not a press elsewhere: the menu keeps the keyboard (sdk/components.jsx drops it otherwise)
  const keep = () => { const k = holder.current; if (k && textFocusOwner() === k.tok) takeTextFocus(k.tok, k.onKey); };
  useEffect(() => {
    const prev = focusToken();
    const tok = {};
    const onKey = (e) => {
      const { items: top0, active: a0, sub: s, subActive: a1, onClose: close } = st.current;
      const inSub = !!(s && s.keys);
      const list = inSub ? s.items : top0, a = inSub ? a1 : a0, set = inSub ? setSubActive : setActive;
      const act = menuAction(e, direction() === 'rtl');
      if (act !== 'type') typed.current = null;
      const go = (i) => { if (i >= 0) set(i); };
      if (act === 'next' || act === 'prev') { if (s && !inSub) closeSub(); go(stepIndex(list, a, act === 'next' ? 1 : -1)); }
      else if (act === 'first' || act === 'last') { if (s && !inSub) closeSub(); go(edgeIndex(list, act === 'last')); }
      else if (act === 'activate') { if (selectable(list[a])) { if (!inSub && list[a].submenu) openSub(a, true); else { close(); list[a].onClick?.(); } } }
      else if (act === 'open') { if (!inSub && selectable(list[a]) && list[a].submenu) openSub(a, true); }
      else if (act === 'back') { if (s) closeSub(); }
      else if (act === 'close') { if (s) closeSub(); else close(); }
      else if (act === 'type') { typed.current = typeAhead(typed.current, e.char, Date.now()); go(matchIndex(list, a, typed.current.buf)); }
    };
    holder.current = { tok, onKey };
    takeTextFocus(tok, onKey);
    const a = addListener('pointerdown', (e) => { if (!inside(e.target, ref) && !inside(e.target, subRef)) st.current.onClose(); });
    return () => {
      a();
      clearTimeout(timer.current);
      const mine = textFocusOwner() === tok;
      releaseTextFocus(tok);
      if (mine) restoreFocus(prev);               // not when the click that closed it went to another field
    };
  }, []);
  return (
    <>
      <MenuBox items={items} left={left} top={top} width={w} active={active} boxRef={ref} onMove={moved} onDown={keep}
        onRow={enterRow} onPick={(i, swatch) => { clearTimeout(timer.current); pick(items, i, swatch); }} />
      {sub && <MenuBox items={sub.items} left={sub.left} top={sub.top} width={sub.width} active={subActive} boxRef={subRef} onEnter={enterSub} onDown={keep}
        onRow={(i) => setSubActive(selectable(sub.items[i]) ? i : -1)} onPick={(i, swatch) => { if (swatch) { onClose(); swatch.onClick(); } else if (selectable(sub.items[i])) { onClose(); sub.items[i].onClick?.(); } }} />}
    </>
  );
}
