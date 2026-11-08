// A small menu at a point of the screen, in the popover layer (docs/LAYERS.md): the taskbar's jump
// lists, the Trash, the desktop, a window's title bar and the start menu's right click.
// items: [{ label, onClick, disabled, checked, destructive, hint } | '-']; submenus are flattened.
// Keyboard: ↑/↓, Enter, Escape. `above`: the menu ends at y (a bottom taskbar) instead of starting there.
// `boxRef`: receives the menu's node. The menu is a portal into the popover layer, so its rows are not
// inside the panel that opened it: a panel that closes on a click outside itself must also treat a
// click in its menu as inside (the start menu), or it unmounts the menu before the row's click lands.
// The menu stays inside the display of the point it opens at (a jump list near the edge of one taskbar must not
// spill onto the next display).
import { View, Text, Layer, ScrollView, addListener, takeTextFocus, releaseTextFocus, direction, displayAt } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';

const ROW = 30, SEP = 9;

export function flatten(items) {
  const out = [];
  for (const it of items || []) {
    if (it && it !== '-' && it.submenu) { if (out.length && out[out.length - 1] !== '-') out.push('-'); out.push(...it.submenu); }
    else if (it) out.push(it);
  }
  while (out[0] === '-') out.shift();
  while (out[out.length - 1] === '-') out.pop();
  return out.filter((it, i) => !(it === '-' && out[i - 1] === '-'));
}

export function Menu({ t, items: raw, x, y, above, onClose, width = 240, boxRef }) {
  const items = flatten(raw);
  const ref = useRef(null);
  const [active, setActive] = useState(-1);
  const st = useRef({});
  st.current = { items, active };
  const ok = (it) => it && it !== '-' && !it.disabled;
  useEffect(() => {
    const offPtr = addListener('pointerdown', (e) => {
      for (let n = e.target; n; n = n.parent) if (n === ref.current) return;
      onClose();
    });
    const tok = {};
    takeTextFocus(tok, (e) => {
      const { items: list, active: a } = st.current;
      const step = (d) => { for (let k = 1; k <= list.length; k++) { const i = ((a < 0 && d < 0 ? 0 : a) + d * k + list.length * 2) % list.length; if (ok(list[i])) { setActive(i); return; } } };
      if (e.key === 'Escape') onClose();
      else if (e.key === 'ArrowDown') step(1);
      else if (e.key === 'ArrowUp') step(-1);
      else if ((e.key === 'Enter' || e.key === ' ') && ok(list[a])) { onClose(); list[a].onClick?.(); }
    });
    return () => { offPtr(); releaseTextFocus(tok); };
  }, []);
  const H = items.reduce((h, it) => h + (it === '-' ? SEP : ROW), 10);
  const d = displayAt(x, y);
  const maxH = Math.max(120, d.h - 24);
  const h = Math.min(H, maxH);
  // in right-to-left languages the menu opens toward the left of the point
  const rtl = direction() === 'rtl';
  let left = rtl ? x - width : x;
  left = Math.max(d.x + 6, Math.min(left, d.x + d.w - width - 6));
  let top = above ? y - h : y;
  top = Math.max(d.y + 6, Math.min(top, d.y + d.h - h - 6));
  const rows = items.map((it, i) => (it === '-'
    ? <View key={i} style={{ height: 1, backgroundColor: t.separator, marginVertical: 4, marginHorizontal: 6 }} />
    : (
      <View key={i} onMouseEnter={() => setActive(ok(it) ? i : -1)} onClick={() => { if (it.disabled) return; onClose(); it.onClick?.(); }}
        style={{ flexDirection: 'row', alignItems: 'center', height: ROW, paddingHorizontal: 10, gap: 8, borderRadius: t.menuItemRadius,
          backgroundColor: i === active ? t.hover : 'transparent', opacity: it.disabled ? 0.45 : 1 }}>
        <Text style={{ width: 14, fontSize: 12, color: t.accent }}>{it.checked ? '✓' : ''}</Text>
        <Text style={{ flex: 1, fontSize: 13, color: it.destructive ? t.closeHover : t.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{it.label}</Text>
        {it.hint ? <Text style={{ fontSize: 11, color: t.textDim }}>{it.hint}</Text> : null}
      </View>
    )));
  return (
    <Layer name="popover">
      <View ref={(n) => { ref.current = n; if (boxRef) boxRef.current = n; }} style={{ position: 'absolute', left, top, width, padding: 5, borderRadius: t.menuRadius,
        backgroundColor: t.panelBg, backdropFilter: t.blur ? `blur(${t.blur}px)` : undefined, border: `1px solid ${t.windowBorder}`, boxShadow: t.shadow }}>
        {H > maxH ? <ScrollView style={{ height: maxH - 10 }}>{rows}</ScrollView> : rows}
      </View>
    </Layer>
  );
}
