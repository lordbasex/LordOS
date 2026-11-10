// lists.jsx — a List and a Table whose rows are chosen with the pointer and the keyboard, as in the desktop's own
// lists (docs/DESIGN.md, "Keyboard focus"). The rules are in sdk/listmodel.js.
//
// The selection belongs to the app: selection is the array of the selected rows' keys and onSelectionChange(keys,
// { lead }) gets the new one. A press selects a row; with multiple, Shift and a press select a range from the last
// row pressed and Ctrl (⌘) and a press add a row or take it out. The list is one Tab stop (sdk/focus.js), and a press
// on a row gives it the keys too (without the ring): ↑ ↓ move,
// Home and End go to the ends and Page Up and Page Down a view at a time; with Shift they extend the range, Ctrl+A
// selects every row, Ctrl (⌘) and an arrow move only the outlined lead row and Ctrl+Space toggles it. Typing a name
// goes to the next row whose text starts with it (type-to-find). Enter and a double click call onActivate(row).
// The row the keys move to comes into sight inside a ScrollView. The columns of a Table and the rows' contents follow
// the reading direction (a row is laid out from the right in right to left).
import { View, Text } from '@lordos/runtime';
import { useEffect, useRef, useState } from 'react';
import { useTheme } from './theme.js';
import { useAppWindow } from './components.jsx';
import { wm } from './windows.js';
import { revealNode, focusRingColor, keyFocused, onFocusMove } from './focus.js';
import { typeAhead, TYPE_MS } from './menunav.js';
import { NO_SELECTION, clickSelect, keySelect, findSelect, findChar, keepShown } from './listmodel.js';

const shown = (v) => typeof v === 'string' || typeof v === 'number';

// The keys, the pointer and the anchor of one list. rows: the data; keyOf(row, i) its key; textOf(row) its text for
// type-to-find.
function useSelection({ rows, keyOf, textOf, selection, onSelectionChange, multiple, onActivate, rowHeight, box }) {
  const { id: winId } = useAppWindow();
  const keys = rows.map((r, i) => String(keyOf(r, i)));
  const sel = useRef(NO_SELECTION);
  // the app's selection, with the anchor and the lead kept here between renders
  const given = (selection || []).map(String);
  const kept = keepShown(sel.current, keys);
  if (given.join('\n') !== kept.selected.join('\n')) {
    sel.current = { selected: keys.filter((k) => given.includes(k)), anchor: given.includes(kept.anchor) ? kept.anchor : given[0] ?? null, lead: given.includes(kept.lead) ? kept.lead : given[given.length - 1] ?? null };
  } else sel.current = kept;
  const typed = useRef(null);
  const nodes = useRef(new Map());
  const [focused, setFocused] = useState(false);     // the keyboard focus is on it, with its ring
  useEffect(() => onFocusMove(() => setFocused(!!box.current && keyFocused() === box.current)), []);
  const latest = useRef(null);
  latest.current = { rows, keys, onSelectionChange, onActivate, multiple, textOf };
  const [, force] = useState(0);

  const apply = (next, reveal) => {
    if (!next || next === sel.current) return;
    const changed = next.selected.join('\n') !== sel.current.selected.join('\n');
    sel.current = next;
    if (changed) latest.current.onSelectionChange?.(next.selected, { lead: next.lead });
    else force((n) => n + 1);
    if (reveal) revealNode(nodes.current.get(next.lead));
  };

  // the rows that fit in the view (Page Up and Page Down): the list's own box, or the window's when the list is taller
  const page = () => {
    let h = 0;
    try { h = lord.getLayout(box.current.id)[3]; } catch (e) { /* not laid out */ }
    const f = wm.get(winId);
    if (f && f.h > 0) h = h > 0 ? Math.min(h, f.h) : f.h;
    return Math.max(1, Math.floor((h || rowHeight * 10) / rowHeight) - 1);
  };

  const onKeyDown = (e) => {
    const { keys: ks, rows: rs, textOf: tx } = latest.current;
    if (!ks.length) return false;
    const now = Date.now();
    const searching = !!typed.current && now - typed.current.at < TYPE_MS;
    const ch = findChar(e, searching);
    if (ch) {
      typed.current = typeAhead(searching ? typed.current : null, ch, now);
      const next = findSelect(sel.current, ks, rs.map((r) => (tx ? tx(r) : '')), typed.current.buf);
      if (next) apply(next, true);
      return true;
    }
    if (e.key === 'Enter' && !e.ctrl && !e.meta && !e.alt) {
      const i = ks.indexOf(sel.current.lead);
      if (i < 0 || !latest.current.onActivate) return false;
      latest.current.onActivate(rs[i]);
      return true;
    }
    const next = keySelect(e, sel.current, ks, { multiple: latest.current.multiple, page: page() });
    if (!next) return false;
    typed.current = null;
    apply(next, true);
    return true;
  };

  const rowProps = (row, i) => {
    const key = keys[i];
    return {
      ref: (n) => { if (n) nodes.current.set(key, n); else nodes.current.delete(key); },
      onMouseDown: (e) => {
        if (e.button) return;
        typed.current = null;
        apply(clickSelect(sel.current, latest.current.keys, key, { shift: e.shift, toggle: e.ctrl || e.meta, multiple: latest.current.multiple }), false);
      },
      onDoubleClick: () => latest.current.onActivate?.(row),
    };
  };
  const isSelected = (i) => sel.current.selected.includes(keys[i]);
  const isLead = (i) => focused && multiple && sel.current.lead === keys[i];
  return { onKeyDown, rowProps, isSelected, isLead };
}

// The colors of a row: the accent with white text when selected (a light fill with the 'indicator' selection token),
// a faint stripe on every other row.
function rowColors(t, on, i, striped) {
  const light = t.selection === 'indicator';
  const bg = on ? (light ? (t.mode === 'dark' ? 'rgba(255,255,255,0.12)' : 'rgba(0,0,0,0.08)') : t.accent) : striped && i % 2 ? t.hover : 'transparent';
  return { bg, fg: on && !light ? '#fff' : t.text, dim: on && !light ? 'rgba(255,255,255,0.82)' : t.textDim };
}

// The lead row of a multiple selection, outlined in the focus ring's color while the list has the keyboard focus.
const leadEdge = (t, on) => (on ? { border: `1px solid ${focusRingColor(t)}` } : null);

// A list of rows. items: the data; itemKey(item, i) its key (the index if left out); renderItem(item, { selected,
// lead, color, dim }) draws one (the text of textOf(item) if left out); textOf(item): its text for type-to-find.
// rowHeight: 26 by default; striped: a stripe on every other row. onContextMenu(item, e) on a right click (the row is
// selected first, unless it already is). focusable (default true): a Tab stop.
export function List({ items, itemKey = (it, i) => i, renderItem, textOf, selection, onSelectionChange, multiple = false, onActivate,
  onContextMenu, rowHeight = 26, striped = false, focusable = true, style }) {
  const t = useTheme();
  const box = useRef(null);
  const text = textOf || ((it) => (shown(it) ? String(it) : ''));
  const s = useSelection({ rows: items, keyOf: itemKey, textOf: text, selection, onSelectionChange, multiple, onActivate, rowHeight, box });
  return (
    <View ref={box} focusable={focusable && items.length > 0} focusOnPress onKeyDown={s.onKeyDown}
      style={[{ borderRadius: t.controlRadiusSmall }, style]}>
      {items.map((it, i) => {
        const on = s.isSelected(i);
        const c = rowColors(t, on, i, striped);
        const rp = s.rowProps(it, i);
        return (
          <View key={String(itemKey(it, i))} {...rp} onContextMenu={onContextMenu ? (e) => { if (!on) rp.onMouseDown({ button: 0 }); onContextMenu(it, e); } : undefined}
            style={{ height: rowHeight, paddingHorizontal: 8, flexDirection: 'row', alignItems: 'center', gap: 6, borderRadius: t.controlRadiusSmall,
              backgroundColor: c.bg, ...leadEdge(t, s.isLead(i)) }}>
            {renderItem ? renderItem(it, { selected: on, lead: s.isLead(i), color: c.fg, dim: c.dim })
              : <Text style={{ flex: 1, fontSize: 13, color: c.fg, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{text(it)}</Text>}
          </View>
        );
      })}
    </View>
  );
}

// A column's box: the flexible one stretches; fixed-width ones keep their width.
const colBox = (c) => (c.flex ? { flex: c.flex, minWidth: 0 } : { width: c.width, flexShrink: 0 });

// A table: a header row and the rows. columns: [{ key, label, flex | width, num (a number: at the end edge), sort
// (the header sorts it), dim (dimmer text) }]; rows: the data; rowKey(row, i) its key; renderCell(row, column,
// { selected, color, dim }) draws one cell: a string or a number (drawn as text), or an element. sort ({ key, dir })
// and onSort(key): the header of a sortable column sorts by it. textOf(row): its text for type-to-find (the first
// column's text if left out). The rest as a List. The columns follow the reading direction.
export function Table({ columns, rows, rowKey = (r, i) => i, renderCell, textOf, sort, onSort, selection, onSelectionChange, multiple = false,
  onActivate, onContextMenu, rowHeight = 26, striped = true, focusable = true, style }) {
  const t = useTheme();
  const box = useRef(null);
  const first = columns[0];
  const text = textOf || ((r) => { const v = first ? renderCell(r, first, {}) : ''; return shown(v) ? String(v) : ''; });
  const s = useSelection({ rows, keyOf: rowKey, textOf: text, selection, onSelectionChange, multiple, onActivate, rowHeight, box });
  const head = t.mode === 'dark' ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.03)';
  const arrow = (c) => (sort?.key === c.key ? (sort.dir > 0 ? ' ▲' : ' ▼') : '');
  return (
    <View ref={box} focusable={focusable && rows.length > 0} focusOnPress onKeyDown={s.onKeyDown}
      style={[{ borderRadius: t.groupRadius, border: `1px solid ${t.separator}`, overflow: 'hidden', backgroundColor: t.windowBg }, style]}>
      <View style={{ flexDirection: 'row', height: 28, alignItems: 'center', paddingHorizontal: 10, backgroundColor: head }}>
        {columns.map((c) => (
          <View key={c.key} onClick={onSort && c.sort ? () => onSort(c.key) : undefined}
            style={{ ...colBox(c), flexDirection: 'row', justifyContent: c.num ? 'flex-end' : 'flex-start', paddingHorizontal: 6 }}>
            <Text style={{ fontSize: 11, fontWeight: 600, color: sort?.key === c.key ? t.text : t.textDim, whiteSpace: 'nowrap' }}>{c.label}{arrow(c)}</Text>
          </View>
        ))}
      </View>
      {rows.map((r, i) => {
        const on = s.isSelected(i);
        const col = rowColors(t, on, i, striped);
        const rp = s.rowProps(r, i);
        return (
          <View key={String(rowKey(r, i))} {...rp} onContextMenu={onContextMenu ? (e) => { if (!on) rp.onMouseDown({ button: 0 }); onContextMenu(r, e); } : undefined}
            style={{ flexDirection: 'row', height: rowHeight, alignItems: 'center', paddingHorizontal: 10, backgroundColor: col.bg, ...leadEdge(t, s.isLead(i)) }}>
            {columns.map((c) => {
              const v = renderCell(r, c, { selected: on, color: col.fg, dim: col.dim });
              return (
                <View key={c.key} style={{ ...colBox(c), flexDirection: 'row', alignItems: 'center', gap: 6, justifyContent: c.num ? 'flex-end' : 'flex-start',
                  paddingHorizontal: 6, overflow: 'hidden' }}>
                  {shown(v) ? <Text style={{ fontSize: 12, color: c.dim ? col.dim : col.fg, whiteSpace: 'nowrap', textAlign: c.num ? 'end' : 'start' }}>{String(v)}</Text> : v}
                </View>
              );
            })}
          </View>
        );
      })}
    </View>
  );
}
