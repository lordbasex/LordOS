// contextmenu.jsx — a right-click menu inside a window, with submenus (Files, Terminal, Editor, Notes):
//
//   <ContextMenu x={…} y={…} items={[…]} onClose={…} bounds={{ w, h }} />
//
// x, y and bounds are in the window's coordinates. A click outside or Esc closes it.
import { useEffect, useRef, useState } from 'react';
import { View, Text, addListener } from '@lordos/runtime';
import { useTheme } from './theme.js';

const inside = (inst, ref) => { for (let n = inst; n; n = n.parent) if (n === ref?.current) return true; return false; };
const ITEM_H = 24;

function MenuBox({ items, x, y, width, onClose, onSub, boxRef, bounds }) {
  const t = useTheme();
  const [hover, setHover] = useState(-1);
  const rowH = (it) => (it === '-' ? 9 : it.header ? 20 : it.swatches ? ITEM_H + 4 : ITEM_H);
  const h = items.reduce((s, it) => s + rowH(it), 10);
  const left = Math.max(4, Math.min(x, bounds.w - width - 4));
  const top = Math.max(4, Math.min(y, bounds.h - h - 4));
  return (
    <View ref={boxRef} style={{ position: 'absolute', left, top, width, paddingVertical: 5, borderRadius: 8, backgroundColor: t.mode === 'dark' ? '#2c2c31' : '#ffffff',
      border: `1px solid ${t.separator}`, boxShadow: '0 10px 30px rgba(0,0,0,0.35)' }}>
      {items.map((it, i) => {
        if (it === '-') return <View key={i} style={{ height: 1, marginVertical: 4, marginHorizontal: 8, backgroundColor: t.separator }} />;
        if (it.header) return <Text key={i} style={{ height: 20, paddingHorizontal: 14, paddingTop: 4, fontSize: 11, fontWeight: 600, color: t.textDim }}>{it.header}</Text>;
        if (it.swatches) {
          return (
            <View key={i} style={{ flexDirection: 'row', gap: 8, height: ITEM_H + 4, alignItems: 'center', paddingHorizontal: 14 }}>
              {it.swatches.map((s, j) => (
                <View key={s.color || s.hex || j} onClick={() => { onClose(); s.onClick(); }} style={{ width: 16, height: 16, borderRadius: 8, backgroundColor: s.hex || 'transparent',
                  border: s.on ? `2px solid ${t.text}` : s.hex ? '0 solid transparent' : `1.5px solid ${t.textDim}` }} />
              ))}
            </View>
          );
        }
        const on = hover === i && !it.disabled;
        return (
          <View key={i} onMouseEnter={(ev) => { setHover(i); onSub(it.submenu ? { items: it.submenu, x: left + width - 6, y: top + 5 + items.slice(0, i).reduce((s, q) => s + rowH(q), 0) - 5 } : null); void ev; }}
            onMouseLeave={() => setHover(-1)}
            onClick={() => { if (!it.disabled && !it.submenu) { onClose(); it.onClick?.(); } }}
            style={{ height: ITEM_H, marginHorizontal: 5, paddingHorizontal: 9, borderRadius: 5, flexDirection: 'row', alignItems: 'center', backgroundColor: on ? t.accent : 'transparent' }}>
            <Text style={{ width: 16, fontSize: 11, color: on ? '#fff' : t.text }}>{it.checked ? '✓' : ''}</Text>
            <Text style={{ flex: 1, fontSize: 13, color: on ? '#fff' : it.disabled ? t.textDim : it.danger ? '#ff453a' : t.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{it.label}</Text>
            {it.hint ? <Text style={{ fontSize: 11, color: on ? '#fff' : t.textDim, marginHorizontal: 4 }}>{it.hint}</Text> : null}
            {it.submenu ? <Text style={{ fontSize: 12, color: on ? '#fff' : t.textDim }}>›</Text> : null}
          </View>
        );
      })}
    </View>
  );
}

// Context menu: items [{ label, onClick, hint, checked, disabled, danger, submenu } | '-' | { header } | { swatches }]
export function ContextMenu({ x, y, items, onClose, bounds = { w: 4000, h: 4000 } }) {
  const ref = useRef(null), subRef = useRef(null);
  const [sub, setSub] = useState(null);
  useEffect(() => {
    const a = addListener('pointerdown', (e) => { if (!inside(e.target, ref) && !inside(e.target, subRef)) onClose(); });
    const b = addListener('keydown', (e) => { if (e.key === 'Escape') onClose(); });
    return () => { a(); b(); };
  }, []);
  items = items.filter(Boolean);
  const width = (list) => Math.min(320, Math.max(190, ...list.filter((i) => i && i.label).map((i) => 46 + lord.measureText(i.label, 13, 400) + (i.hint ? 16 + lord.measureText(i.hint, 11, 400) : 0))));
  return (
    <>
      <MenuBox items={items} x={x} y={y} width={width(items)} onClose={onClose} onSub={setSub} boxRef={ref} bounds={bounds} />
      {sub && <MenuBox items={sub.items} x={sub.x} y={sub.y} width={width(sub.items)} onClose={onClose} onSub={() => {}} boxRef={subRef} bounds={bounds} />}
    </>
  );
}
