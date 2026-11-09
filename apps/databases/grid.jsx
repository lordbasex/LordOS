// The table of rows the Content, Query and Structure panes share (the mocks' grid): a header that sorts on a click, rows
// of 25 px drawn only where they show (a page holds 1,000 rows), zebra stripes, a selection, a cell edited in place,
// the wheel (with Shift, sideways) and a scroll bar that can be dragged.
import { View, Text, addListener, useAppWindow } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { usePal, useSize, MONO } from './ui.jsx';

export const ROW_H = 25;
const HEAD_H = 28;

// columns: [{ key, label, width, align: 'start' | 'end' | 'center', mono, render(value, row, i) }]
// rows: arrays (or anything render reads); cell(row, col) → the value for the default renderer
// selected: Set of row indexes; onRowClick(i, e); onDoubleClick(i, colIndex, e); onContextMenu(i, colIndex, e)
// sort: { column, dir }; onHeader(col); editing: { row, col } and editor (an element drawn in that cell)
export function Grid({ columns, rows, cell = (r, c) => r[c.index], selected, onRowClick, onDoubleClick, onContextMenu, sort, onHeader, editing, editor,
  emptyText, scrollTo, rowStyle }) {
  const p = usePal();
  const { focused } = useAppWindow();
  const box = useRef(null);
  const size = useSize(box, { w: 600, h: 300 });
  const [scroll, setScroll] = useState({ top: 0, left: 0 });
  const drag = useRef(null);
  const total = columns.reduce((n, c) => n + c.width, 0);
  const bodyH = Math.max(0, size.h - HEAD_H);
  const maxTop = Math.max(0, rows.length * ROW_H - bodyH);
  const maxLeft = Math.max(0, total - size.w);
  const top = Math.min(scroll.top, maxTop), left = Math.min(scroll.left, maxLeft);

  // keep a row in view (keyboard moves, a new row)
  useEffect(() => {
    if (scrollTo == null || scrollTo < 0) return;
    setScroll((s) => {
      const y = scrollTo * ROW_H;
      if (y < s.top) return { ...s, top: y };
      if (y + ROW_H > s.top + bodyH) return { ...s, top: y + ROW_H - bodyH };
      return s;
    });
  }, [scrollTo]);
  // back to the top for other rows (another page, table or result)
  const firstRef = useRef(rows);
  useEffect(() => { if (firstRef.current !== rows && rows.length < (firstRef.current?.length || 0)) setScroll({ top: 0, left: 0 }); firstRef.current = rows; }, [rows]);

  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      const d = drag.current;
      if (!d) return;
      if (d.axis === 'y') setScroll((s) => ({ ...s, top: Math.max(0, Math.min(maxTop, d.start + (e.y - d.y) * d.ratio)) }));
      else setScroll((s) => ({ ...s, left: Math.max(0, Math.min(maxLeft, d.start + (e.x - d.x) * d.ratio)) }));
    });
    const offUp = addListener('pointerup', () => { drag.current = null; });
    return () => { offMove(); offUp(); };
  });

  const onWheel = (e) => {
    const dy = e.deltaY || 0, dx = e.deltaX || 0;
    setScroll((s) => (e.shift || Math.abs(dx) > Math.abs(dy)
      ? { ...s, left: Math.max(0, Math.min(maxLeft, s.left + (dx || dy))) }
      : { ...s, top: Math.max(0, Math.min(maxTop, s.top + dy)) }));
  };

  const first = Math.max(0, Math.floor(top / ROW_H) - 2);
  const last = Math.min(rows.length - 1, Math.ceil((top + bodyH) / ROW_H) + 2);
  const vis = [];
  for (let i = first; i <= last; i++) vis.push(i);
  const xs = [];
  columns.reduce((x, c) => { xs.push(x); return x + c.width; }, 0);
  const firstCol = Math.max(0, xs.findIndex((x, i) => x + columns[i].width >= left));
  let lastCol = columns.length - 1;
  for (let i = firstCol; i < columns.length; i++) if (xs[i] > left + size.w) { lastCol = i; break; }
  const colRange = columns.slice(firstCol, lastCol + 1).map((c, k) => [c, firstCol + k]);

  const textFor = (c, v) => {
    if (v === null || v === undefined) return { s: 'NULL', nul: true };
    return { s: String(v), nul: false };
  };
  const thumbH = rows.length * ROW_H > bodyH ? Math.max(24, (bodyH * bodyH) / (rows.length * ROW_H)) : 0;
  const thumbY = thumbH && maxTop ? (top / maxTop) * (bodyH - thumbH) : 0;
  const thumbW = total > size.w ? Math.max(30, (size.w * size.w) / total) : 0;
  const thumbX = thumbW && maxLeft ? (left / maxLeft) * (size.w - thumbW) : 0;

  return (
    <View ref={box} onWheel={onWheel} style={{ flex: 1, minHeight: 0, overflow: 'hidden', direction: 'ltr' }}>
      {/* header */}
      <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: HEAD_H, backgroundColor: p.head }}>
        {colRange.map(([c, i]) => {
          const sorted = sort && sort.column === (c.sortKey ?? c.label);
          return (
            <View key={c.key} onClick={onHeader ? () => onHeader(c) : undefined}
              style={{ position: 'absolute', top: 0, height: HEAD_H, left: xs[i] - left, width: c.width, paddingHorizontal: 9, flexDirection: 'row', alignItems: 'center', gap: 4,
                justifyContent: c.align === 'end' ? 'flex-end' : c.align === 'center' ? 'center' : 'flex-start' }}>
              <View style={{ position: 'absolute', right: 0, top: 0, bottom: 0, width: 1, backgroundColor: p.line }} />
              <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{c.label}</Text>
              {sorted ? <Text style={{ fontSize: 11, color: p.text }}>{sort.dir === 'desc' ? '↓' : '↑'}</Text> : null}
            </View>
          );
        })}
        <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 1, backgroundColor: p.line }} />
      </View>
      {/* rows */}
      <View style={{ position: 'absolute', left: 0, right: 0, top: HEAD_H, bottom: 0, overflow: 'hidden' }}>
        {vis.map((i) => {
          const r = rows[i];
          const sel = selected?.has(i);
          const extra = rowStyle ? rowStyle(r, i) : null;
          return (
            <View key={i} onMouseDown={(e) => onRowClick?.(i, e)}
              style={{ position: 'absolute', left: 0, right: 0, top: i * ROW_H - top, height: ROW_H, backgroundColor: sel ? (focused ? p.sel : p.control) : i % 2 ? p.zebra : 'transparent', ...extra }}>
              {colRange.map(([c, k]) => {
                const isEdit = editing && editing.row === i && editing.col === k;
                const v = cell(r, c, i);
                const custom = c.render ? c.render(v, r, i, sel) : null;
                const { s, nul } = custom ? { s: '', nul: false } : textFor(c, v);
                return (
                  <View key={c.key} onDoubleClick={onDoubleClick ? (e) => onDoubleClick(i, k, e) : undefined} onContextMenu={onContextMenu ? (e) => onContextMenu(i, k, e) : undefined}
                    style={{ position: 'absolute', top: 0, height: ROW_H, left: xs[k] - left, width: c.width, paddingHorizontal: isEdit ? 0 : 9, flexDirection: 'row', alignItems: 'center',
                      justifyContent: c.align === 'end' ? 'flex-end' : c.align === 'center' ? 'center' : 'flex-start', overflow: 'hidden' }}>
                    <View style={{ position: 'absolute', right: 0, top: 0, bottom: 0, width: 1, backgroundColor: p.line }} />
                    {isEdit ? editor : custom || (
                      <Text style={{ fontSize: c.mono ? 12 : 12.5, fontFamily: c.mono ? MONO : undefined, whiteSpace: 'nowrap', overflow: 'hidden',
                        color: sel && focused ? (nul ? 'rgba(255,255,255,0.75)' : '#fff') : nul ? p.nul : p.text, fontStyle: nul ? 'italic' : 'normal' }}>{s}</Text>
                    )}
                  </View>
                );
              })}
            </View>
          );
        })}
        {!rows.length && emptyText ? (
          <View style={{ position: 'absolute', left: 0, right: 0, top: 30, alignItems: 'center' }}><Text style={{ fontSize: 12.5, color: p.dim }}>{emptyText}</Text></View>
        ) : null}
      </View>
      {/* scroll bars */}
      {thumbH ? (
        <View onMouseDown={(e) => { drag.current = { axis: 'y', y: e.y, start: top, ratio: maxTop / Math.max(1, bodyH - thumbH) }; }}
          style={{ position: 'absolute', right: 2, top: HEAD_H + thumbY, width: 7, height: thumbH, borderRadius: 4, backgroundColor: p.dark ? 'rgba(255,255,255,0.28)' : 'rgba(0,0,0,0.28)' }} />
      ) : null}
      {thumbW ? (
        <View onMouseDown={(e) => { drag.current = { axis: 'x', x: e.x, start: left, ratio: maxLeft / Math.max(1, size.w - thumbW) }; }}
          style={{ position: 'absolute', bottom: 2, left: thumbX, height: 7, width: thumbW, borderRadius: 4, backgroundColor: p.dark ? 'rgba(255,255,255,0.28)' : 'rgba(0,0,0,0.28)' }} />
      ) : null}
    </View>
  );
}
