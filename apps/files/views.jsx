// views.jsx — the four Files views: icons (cards), list (with expandable folders),
// columns (Miller, with a preview) and gallery. Their scroll views are no Tab stops (focusable={false}): the arrows
// move the selection (index.jsx), not the view.
import { View, Text, Image, TextInput, ScrollView, EmptyState, useTheme, addListener, direction, Disclosure, formatBytes, typeScale, t as tr, StatusGlyph, StatusBadge, bundleFor } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { EntryIcon, TagDots, tagTint } from './glyphs.jsx';
import { kindOf, isImage, isTextual, thumbSrc } from './kinds.js';
import { tagsOf, commentOf } from './ops.js';
import { renameSelection } from './names.js';
import { visible, resize, moveColumn, dropBefore, totalWidth, NAME_MIN } from './columns.js';
import { sortEntries } from './locations.js';
import { colWidth, clampCol, fitCol, colStrip, chainFolder } from './colview.js';
import { iconMetrics, listMetrics, iconColumns } from './viewprefs.js';
import { when, listRowStyle, listHeaderCell, listCell } from '../../sdk/fileparts.jsx';
import { DropHot } from './dnd.jsx';
import { useMarquee, MarqueeBand } from './marqueeview.jsx';

// folder of a search or Recents result (the last segment of the path)
const where = (p) => { const parts = String(p).split('/'); return parts[parts.length - 2] || '/'; };
export const kindLabel = (e) => (e.sub ? e.sub : e.app ? tr('files.kind.app') : bundleFor(e) ? bundleFor(e).kind() : tr(`files.kind.${kindOf(e)}`));

const FIELD_H = 22;
const textWidth = (s, size) => (typeof lord !== 'undefined' && lord.measureText ? lord.measureText(String(s), size, 400) : String(s).length * size * 0.55);

// Name, or the field for renaming in place. font and line follow the view's zoom (viewprefs.js); room: the width the
// name wraps in (when it may take more than one line).
function Name({ e, ctx, center, color, lines = 1, font, line, fieldW, room }) {
  const size = font || (center ? 12 : 13), lh = line || (size <= 13 ? 16 : Math.round(size * 1.34));
  const shown = ctx.shownName ? ctx.shownName(e) : e.name;
  if (ctx.renaming?.path === e.path) {
    // the field takes the name's place without changing the layout: its box is as high as the name's lines were (the
    // 22 px field is taller than a 16 px line, and the icon grid moved down by the difference), the field centred over it
    const rows = lines > 1 && room && textWidth(shown, size) > room ? lines : 1;
    const h = rows * lh;
    return (
      <View onMouseDown={(ev) => ev.stopPropagation()} style={{ width: fieldW || (center ? 110 : 240), height: h }}>
        <View style={{ position: 'absolute', left: 0, right: 0, top: Math.round((h - FIELD_H) / 2), height: FIELD_H }}>
          {/* Tab keeps the new name, as Enter does, instead of leaving the field open without the keys (sdk/focus.js) */}
          <TextInput autoFocus selectAll select={renameSelection(ctx.renaming.value, !!e.dir)} value={ctx.renaming.value} onChange={ctx.onRenameChange} onSubmit={ctx.onRenameCommit} onCancel={ctx.onRenameCancel}
            onKeyDown={(k) => { if (k.key !== 'Tab' || k.ctrl || k.alt || k.meta) return false; ctx.onRenameCommit(); return true; }}
            style={{ height: FIELD_H, paddingHorizontal: 4 }} />
        </View>
      </View>
    );
  }
  // a press on the name (not the icon) says so to the item's handler below it: a slow second click there renames
  // (apps/files/index.jsx, sdk/slowclick.js)
  return (
    <Text onMouseDown={(ev) => { ev.onName = true; }} style={{ fontSize: size, color, textAlign: center ? 'center' : 'start', whiteSpace: lines === 1 ? 'nowrap' : 'normal',
      overflow: 'hidden', maxHeight: lines * lh, lineHeight: lh, textOverflow: 'ellipsis', numberOfLines: lines }}>{shown}</Text>
  );
}

function itemHandlers(e, ctx) {
  return {
    onMouseDown: (ev) => { ev.stopPropagation(); ctx.onItemDown(e, ev); },
    onDoubleClick: (ev) => { ev.stopPropagation(); ctx.onOpen(e); },
    onContextMenu: (ev) => { ev.stopPropagation(); ctx.onContext(e, ev); },
    ...(e.dir && !e.trash && !e.arc && !e.netHost ? { dropPath: e.path } : {}),
  };
}

function GroupTitle({ g, t }) {
  if (!g.label) return null;
  return (
    <View style={{ paddingHorizontal: 8, paddingTop: 14, paddingBottom: 6, gap: 6 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text }}>{g.label}</Text>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{g.items.length}</Text>
      </View>
      <View style={{ height: 1, backgroundColor: t.separator }} />
    </View>
  );
}

// ---- what is in sight (big folders) ----
// A folder of thousands of files drew every card or row at once: /usr/share/doc (2220 entries) made 18 000 nodes in
// one run of 1.4 s with the pointer stopped, and 70 MB of heap. Past LIVE_ALL entries, only what is within two
// screens of the visible part is drawn; the rest are empty boxes of the same size, so the scroll bar and the
// positions don't change, and they are drawn as they come near. The icon view's boxes are a card's tallest (a
// two-line name); the list's rows all have the same height.
const LIVE_ALL = 300;
const SIGHT_STEP = 120;                   // the offset is followed in steps: a wheel notch doesn't draw anything again
const GROUP_TITLE_H = 44;                 // GroupTitle: 14 + 6 padding, a line of text, 6 gap and the rule
function useSight() {
  const [sight, setSight] = useState({ y: 0, h: 1200 });
  const onScroll = (y, h) => {
    const q = Math.floor(y / SIGHT_STEP) * SIGHT_STEP;
    setSight((v) => (v.y === q && v.h === h ? v : { y: q, h: h || v.h }));
  };
  return [sight, onScroll];
}
const inSight = (sight, top, height) => top + height >= sight.y - 2 * sight.h && top <= sight.y + 3 * sight.h;

// ---- icons ----

export function IconView({ groups, ctx }) {
  const t = useTheme();
  const m = iconMetrics(ctx.zoom);
  const [sight, onScroll] = useSight();
  const total = groups.reduce((n, g) => n + g.items.length, 0);
  const windowed = total > LIVE_ALL;
  // the cards per row: from the grid's own width once it is laid out (a scroll bar's gutter changes it), the window's before
  const grid = useRef(null);
  const [gridW, setGridW] = useState(0);
  useEffect(() => {
    if (!windowed) return undefined;
    const look = () => { try { const w = grid.current ? lord.getLayout(grid.current.id)[2] : 0; if (w > 0 && Math.abs(w - gridW) > 1) setGridW(w); } catch (e) { /* not laid out */ } };
    const id = setTimeout(look, 50);
    return () => clearTimeout(id);
  });
  const cols = gridW ? Math.max(1, Math.floor((gridW + 8) / (m.cell + 8))) : iconColumns(ctx.contentW || 800, ctx.zoom);
  const cardH = 12 + m.icon + 8 + 5 + 2 + 2 * m.line;          // paddings, the icon's box, the gap, the name's two lines
  const rowH = cardH + 8;
  let top = 12;
  // a drag on the empty space selects what its rectangle touches (GitHub #233, marqueeview.jsx)
  const mq = useMarquee(ctx);
  return (
    <View style={{ flex: 1 }} onMouseDown={mq.onDown}>
    <ScrollView scrollRef={mq.scrollRef} resetKey={ctx.scrollKey} onWheel={ctx.onZoomWheel} onScroll={windowed ? onScroll : undefined} focusable={false} style={{ flex: 1 }}>
      <View style={{ padding: 12, paddingBottom: 32 }}>
        {groups.map((g) => {
          if (g.label) top += GROUP_TITLE_H;
          const at = top;
          top += Math.ceil(g.items.length / cols) * rowH;
          return (
          <View key={g.key}>
            <GroupTitle g={g} t={t} />
            <View ref={grid} style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8 }}>
              {g.items.map((e, i) => {
                if (windowed && !inSight(sight, at + Math.floor(i / cols) * rowH, rowH)) return <View key={e.path} ref={mq.itemRef(e.path)} style={{ width: m.cell, height: cardH }} />;
                const sel = ctx.selected.has(e.path);
                return (
                  <View key={e.path} ref={mq.itemRef(e.path)} {...itemHandlers(e, ctx)}
                    style={{ width: m.cell, paddingVertical: 6, alignItems: 'center', gap: 5, opacity: ctx.cut.has(e.path) ? 0.45 : 1 }}>
                    <DropHot dnd={ctx.dnd} path={e.path}>{(hot) => (
                    <View style={{ padding: 4, borderRadius: 7, backgroundColor: hot ? t.accent : sel ? (t.mode === 'dark' ? 'rgba(255,255,255,0.12)' : 'rgba(0,0,0,0.08)') : 'transparent' }}>
                      <EntryIcon e={e} size={m.icon} dark={t.mode === 'dark'} thumb={!ctx.cloudOnly?.(e)} tint={tagTint(tagsOf(e.path))} />
                      {ctx.cloud?.status[e.path] ? <View style={{ position: 'absolute', insetInlineEnd: -4, bottom: -4 }}><StatusBadge s={ctx.cloud.status[e.path]} size={Math.max(20, Math.round(m.icon * 0.36))} /></View> : null}
                    </View>
                    )}</DropHot>
                    <View style={{ maxWidth: m.cell - 4, paddingHorizontal: 5, paddingVertical: 1, borderRadius: 5, backgroundColor: sel ? t.accent : 'transparent', alignItems: 'center' }}>
                      <Name e={e} ctx={ctx} center lines={2} font={m.font} line={m.line} fieldW={m.cell + 10} room={m.cell - 14} color={sel ? '#fff' : t.text} />
                    </View>
                    <TagDots tags={tagsOf(e.path)} />
                  </View>
                );
              })}
            </View>
          </View>
          );
        })}
        {groups.every((g) => !g.items.length) && <EmptyState title={ctx.emptyText} hint={ctx.emptyHint} action={ctx.emptyAction} style={{ minHeight: 260 }} />}
        <MarqueeBand band={mq.band} />
      </View>
    </ScrollView>
    </View>
  );
}

// ---- list ----

const SIDE_PAD = 36;              // the rows' and the header's own padding
const STATUS_W = 70;              // Cloud's Status column, last, in a cloud account's folders (docs/CLOUD.md 7.1)
const cellAlign = (k) => (k === 'size' ? 'end' : 'start');
const longDate = (st, k) => when(st?.[k]);

function cellText(k, e, ctx) {
  const st = ['created', 'opened', 'changed', 'perms', 'owner'].includes(k) ? ctx.statOf(e) : null;
  switch (k) {
    case 'date': return e.netHost || e.netWay ? '—' : e.trash ? (e.deleted || '').replace('T', ' ') : when(e.mtime);
    case 'created': return longDate(st, 'btime');
    case 'opened': return longDate(st, 'atime');
    case 'changed': return longDate(st, 'ctime');
    case 'size': return e.netHost || e.netWay ? '—' : e.dir && !e.trash ? '—' : e.app ? '' : formatBytes(e.size);
    case 'kind': return ctx.showFolder ? where(e.path) : kindLabel(e);
    case 'comments': return commentOf(e.path);
    case 'perms': return st?.mode || '—';
    case 'owner': return st?.user || '—';
    default: return '';
  }
}

// Name stays first. A column moves by dragging its title, widens by dragging its edge, and its menu is a right click.
function Header({ sort, onSort, t, ctx, columns, onColumns, nameW }) {
  const row = useRef(null);
  const op = useRef(null);
  const [live, setLive] = useState(null);       // a resize or a move under way
  const cols = live?.columns || columns;
  useEffect(() => {
    const mv = addListener('pointermove', (e) => {
      const o = op.current;
      if (!o || (o.k === 'name' && !o.grip)) return;       // Name stays first
      if (!o.moved && Math.abs(e.x - o.x) < 5) return;
      o.moved = true;
      const dx = (direction() === 'rtl' ? -1 : 1) * (e.x - o.x);
      if (o.grip) setLive({ columns: resize(o.columns, o.k, o.w + dx) });
      else {
        const [rx, , rw] = lord.getLayout(row.current.id);
        const from = direction() === 'rtl' ? rx + rw - e.x : e.x - rx;
        const before = dropBefore(o.columns, from - nameW - SIDE_PAD / 2);
        setLive({ columns: moveColumn(o.columns, o.k, before), moving: o.k });
      }
    });
    const up = () => {
      const o = op.current;
      op.current = null;
      if (!o) return;
      if (o.moved) setLive((l) => { if (l) onColumns(l.columns); return null; });
      else if (!o.grip) onSort(o.k);
    };
    const a = addListener('pointerup', up);
    return () => { mv(); a(); };
  }, [nameW, columns]);
  const start = (ev, k, grip) => { ev.stopPropagation(); if (ev.button) return; op.current = { k, grip, x: ev.x, w: k === 'name' ? nameW : cols.width[k], columns: cols }; };
  const title = (k) => tr(`files.col.${k === 'kind' && ctx.showFolder ? 'where' : k}`);
  const label = (k) => (
    <>
      <Text style={listHeaderCell(t, sort.by === k)}>{title(k)}</Text>
      {sort.by === k && <Text style={{ fontSize: 8, color: t.textDim }}>{sort.desc ? '▼' : '▲'}</Text>}
    </>
  );
  return (
    <View ref={row} onContextMenu={(ev) => { ev.stopPropagation(); ctx.onHeaderMenu(ev); }} style={{ flexDirection: 'row', height: 27, alignItems: 'center', paddingHorizontal: 18, borderBottomWidth: 1, borderBottomColor: t.separator }}>
      <View onMouseDown={(ev) => start(ev, 'name', false)} style={{ width: live?.columns?.nameWidth ?? nameW, height: 26, flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 20 }}>
        {label('name')}
        <View onMouseDown={(ev) => start(ev, 'name', true)} style={{ position: 'absolute', top: 0, bottom: 0, insetInlineEnd: 0, width: 8, cursor: 'ew-resize' }} />
      </View>
      {visible(cols).map((k) => (
        <View key={k} onMouseDown={(ev) => start(ev, k, false)} style={{ width: cols.width[k], height: 26, flexDirection: 'row', alignItems: 'center', gap: 4, overflow: 'hidden',
          justifyContent: k === 'size' ? 'flex-end' : 'flex-start', paddingHorizontal: 8, opacity: live?.moving === k ? 0.5 : 1 }}>
          <View style={{ position: 'absolute', top: 6, bottom: 6, insetInlineStart: 0, width: 1, backgroundColor: t.separator }} />
          {label(k)}
          <View onMouseDown={(ev) => start(ev, k, true)} style={{ position: 'absolute', top: 0, bottom: 0, insetInlineEnd: 0, width: 8, cursor: 'ew-resize' }} />
        </View>
      ))}
      {ctx.cloud?.on ? (
        <View style={{ width: STATUS_W, height: 26, alignItems: 'center', justifyContent: 'center' }}>
          <View style={{ position: 'absolute', top: 6, bottom: 6, insetInlineStart: 0, width: 1, backgroundColor: t.separator }} />
          <Text style={listHeaderCell(t, false)}>{tr('cloud.col.status')}</Text>
        </View>
      ) : null}
    </View>
  );
}

function ListRows({ items, depth, ctx, t, sort, alt }) {
  const m = listMetrics(ctx.zoom);
  const rows = [];
  for (const e of items) {
    const sel = ctx.selected.has(e.path);
    const open = ctx.expanded.has(e.path);
    const odd = (alt.n++) % 2 === 1;
    rows.push(
      <DropHot key={e.path} dnd={ctx.dnd} path={e.path}>{(hot) => (
      <View ref={ctx.itemRef?.(e.path)} {...itemHandlers(e, ctx)}
        style={[listRowStyle(t, sel || hot, odd), { height: m.row, opacity: ctx.cut.has(e.path) ? 0.45 : 1 }]}>
        <View style={{ width: ctx.nameW, flexDirection: 'row', alignItems: 'center', gap: 6, paddingLeft: depth * 16 }}>
          <Disclosure state={e.dir && !e.trash ? (open ? 'open' : 'closed') : 'none'} color={sel ? '#fff' : t.textDim} onDown height={m.row}
            onToggle={() => ctx.onToggleExpand(e.path)} />
          {/* thumbnails from 32 px up, as in the icon view */}
          <EntryIcon e={e} size={m.icon} dark={t.mode === 'dark'} thumb={m.icon >= 32 && !ctx.cloudOnly?.(e)} tint={tagTint(tagsOf(e.path))} />
          {/* the tag dots right after the name (not at the column's end); the name keeps the room when renamed */}
          <View style={{ flexShrink: 1, minWidth: 0, ...(ctx.renaming?.path === e.path || !tagsOf(e.path).length ? { flex: 1 } : null) }}><Name e={e} ctx={ctx} font={m.font} color={sel ? '#fff' : t.text} /></View>
          <TagDots tags={tagsOf(e.path)} size={8} />
          {tagsOf(e.path).length && ctx.renaming?.path !== e.path ? <View style={{ flex: 1 }} /> : null}
        </View>
        {visible(ctx.columns).map((k) => (k === 'tags'
          ? <View key={k} style={{ width: ctx.columns.width.tags, paddingHorizontal: 8 }}><TagDots tags={tagsOf(e.path)} size={7} /></View>
          : <Text key={k} style={{ ...listCell(t, sel, ctx.columns.width[k], cellAlign(k)), fontSize: m.cell }}>{cellText(k, e, ctx)}</Text>))}
        {ctx.cloud?.on ? <View style={{ width: STATUS_W, alignItems: 'center', justifyContent: 'center' }}><StatusGlyph s={ctx.cloud.status[e.path]} size={18} /></View> : null}
      </View>
      )}</DropHot>,
    );
    if (open) {
      let kids = ctx.kidsOf(e) || [];
      if (!e.netHost) try { kids = sortEntries((ctx.listOf ? ctx.listOf(e.path) : lord.fs.list(e.path)).filter((x) => ctx.hidden || !x.name.startsWith('.')).map((x) => ({ ...x, path: `${e.path}/${x.name}` })), sort, ctx.sortOpts); } catch (err) { /* no permission */ }
      rows.push(...ListRows({ items: kids, depth: depth + 1, ctx, t, sort, alt }));
    }
  }
  return rows;
}

export function ListView({ groups, ctx, sort, onSort }) {
  const t = useTheme();
  const alt = { n: 0 };
  const [sx, setSx] = useState(0);
  const cols = ctx.columns;
  const extra = ctx.cloud?.on ? STATUS_W : 0;
  const need = totalWidth(cols) + extra + NAME_MIN + SIDE_PAD;
  const room = ctx.contentW;
  const nameW = cols.nameWidth || Math.max(NAME_MIN, room - totalWidth(cols) - extra - SIDE_PAD);
  const over = Math.max(0, need - room);
  const x = Math.min(sx, over);
  // a drag on the empty space selects the rows its rectangle touches (GitHub #233, marqueeview.jsx)
  const mq = useMarquee(ctx);
  const inner = { ...ctx, nameW, itemRef: mq.itemRef };
  const thumb = over ? Math.max(40, room * (room / need)) : 0;
  const bar = useRef(null);
  const [sight, onScroll] = useSight();
  const rowH = listMetrics(ctx.zoom).row;
  const windowed = groups.reduce((n, g) => n + g.items.length, 0) > LIVE_ALL;
  let top = 4;
  // a group's rows (its expanded folders' too), with the ones out of sight as one empty box above and one below
  const rowsOf = (g) => {
    const rows = ListRows({ items: g.items, depth: 0, ctx: inner, t, sort, alt });
    if (g.label) top += GROUP_TITLE_H;
    const at = top;
    top += rows.length * rowH;
    if (!windowed) return rows;
    let from = 0, to = rows.length;
    while (from < to && !inSight(sight, at + from * rowH, rowH)) from++;
    while (to > from && !inSight(sight, at + (to - 1) * rowH, rowH)) to--;
    return [from ? <View key="above" style={{ height: from * rowH }} /> : null, ...rows.slice(from, to),
      to < rows.length ? <View key="below" style={{ height: (rows.length - to) * rowH }} /> : null];
  };
  useEffect(() => {
    const mv = addListener('pointermove', (e) => { const b = bar.current; if (b) setSx(Math.max(0, Math.min(b.over, b.x0 + (e.x - b.at) * b.k))); });
    const up = addListener('pointerup', () => { bar.current = null; });
    return () => { mv(); up(); };
  }, []);
  return (
    <View onWheel={(e) => { if (over && e.deltaX) setSx(Math.max(0, Math.min(over, x + e.deltaX))); }} style={{ flex: 1, overflow: 'hidden' }}>
      <View style={{ flex: 1, width: Math.max(room, need), [direction() === 'rtl' ? 'marginRight' : 'marginLeft']: -x }}>
        <Header sort={sort} onSort={onSort} t={t} ctx={ctx} columns={cols} onColumns={ctx.onColumns} nameW={nameW} />
        <View style={{ flex: 1 }} onMouseDown={mq.onDown}>
        <ScrollView scrollRef={mq.scrollRef} resetKey={ctx.scrollKey} onWheel={ctx.onZoomWheel} onScroll={windowed ? onScroll : undefined} focusable={false} style={{ flex: 1 }}>
          <View style={{ paddingHorizontal: 8, paddingTop: 4, paddingBottom: 32 }}>
            {groups.map((g) => (
              <View key={g.key}>
                <GroupTitle g={g} t={t} />
                {rowsOf(g)}
              </View>
            ))}
            {groups.every((g) => !g.items.length) && <EmptyState title={ctx.emptyText} hint={ctx.emptyHint} action={ctx.emptyAction} style={{ minHeight: 240 }} />}
            <MarqueeBand band={mq.band} />
          </View>
        </ScrollView>
        </View>
      </View>
      {over ? (
        <View style={{ height: 10, justifyContent: 'center', paddingHorizontal: 2 }}>
          <View onMouseDown={(ev) => { ev.stopPropagation(); bar.current = { at: ev.x, x0: x, over, k: over / Math.max(1, room - thumb) }; }}
            style={{ width: thumb, height: 6, borderRadius: 3, backgroundColor: t.textDim, opacity: 0.5, [direction() === 'rtl' ? 'marginRight' : 'marginLeft']: (x / over) * (room - thumb - 4) }} />
        </View>
      ) : null}
    </View>
  );
}

// ---- preview (columns, gallery, Quick Preview) ----

export function Preview({ e, big = false, name, noRead = false }) {
  const t = useTheme();
  const [text, setText] = useState(null);
  useEffect(() => {
    setText(null);
    if (!e || e.dir || e.app || noRead || !isTextual(e) || e.size > 8 * 1024 * 1024) return;
    try { setText(lord.fs.read(e.path, 'text').slice(0, 65536)); } catch (err) { setText(null); }
  }, [e?.path, noRead]);
  if (!e) return null;
  return (
    <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 6, padding: 16 }}>
      {isImage(e) && !noRead ? <Image src={thumbSrc(e.path, big ? 1024 : 512, e)} style={{ width: '100%', flex: 1, objectFit: 'contain', borderRadius: 6 }} />
        : text != null ? (
          <View style={{ alignSelf: 'stretch', flex: 1, borderRadius: 8, backgroundColor: t.mode === 'dark' ? '#16161a' : '#fbfbfd', border: `1px solid ${t.separator}`, padding: 12, overflow: 'hidden', direction: 'ltr' }}>
            <Text style={{ fontFamily: 'monospace', fontSize: big ? 12 : 10, color: t.text, lineHeight: big ? 17 : 14 }}>{text.split('\n').slice(0, big ? 200 : 60).join('\n')}</Text>
          </View>
        ) : <EntryIcon e={e} size={big ? 160 : 96} dark={t.mode === 'dark'} thumb={!noRead} />}
      <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'center', marginTop: 6 }}>{name || e.name}</Text>
      <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'center' }}>{`${kindLabel(e)}${e.dir || e.app ? '' : ` · ${formatBytes(e.size)}`}${e.mtime ? ` · ${when(e.mtime)}` : ''}`}</Text>
    </View>
  );
}

// ---- columns (Miller) ----

// Each column's divider widens it by dragging (as the list's column edges do) and fits it to its longest name with a
// double click; the width is kept per folder (colview.js, GitHub #99). Every column is shown: when they don't fit
// they scroll sideways (a sideways swipe or tilt of the wheel, or the bar along the bottom), and opening a deeper
// folder scrolls to it, as in Finder (GitHub #133). The view is laid out left to right in every language, so the
// pointer's x needs no flip.
export function ColumnView({ root, chain, setChain, entriesOf, ctx, sort, widths, onWidth }) {
  const t = useTheme();
  const box = useRef(null);
  const op = useRef(null);
  const [live, setLive] = useState(null);       // { dir, w } while a divider is dragged
  const dirs = [root, ...chain.filter((p) => ctx.isDir(p))];
  const last = chain[chain.length - 1];
  const file = last && !ctx.isDir(last) ? ctx.entryAt(last) : null;
  const widthOf = (dir) => (live && live.dir === dir ? live.w : colWidth(widths, dir));
  useEffect(() => {
    const mv = addListener('pointermove', (e) => {
      const o = op.current;
      if (!o || (!o.moved && Math.abs(e.x - o.x) < 3)) return;
      o.moved = true;
      setLive({ dir: o.dir, w: clampCol(o.w + e.x - o.x) });
    });
    const up = () => {
      const o = op.current;
      op.current = null;
      if (o?.moved) setLive((l) => { if (l) onWidth?.(l.dir, l.w); return null; });
    };
    const a = addListener('pointerup', up);
    return () => { mv(); a(); };
  }, [onWidth]);
  const startDrag = (ev, dir) => { ev.stopPropagation(); if (ev.button) return; op.current = { dir, x: ev.x, w: widthOf(dir) }; };
  const fit = (ev, dir, items) => {
    ev.stopPropagation();
    op.current = null;
    const names = items.map((e) => lord.measureText(String(ctx.shownName ? ctx.shownName(e) : e.name), 13, 400));
    onWidth?.(dir, fitCol(names, items.map((e) => tagsOf(e.path).length * 9)));
  };
  // every column, side by side; what doesn't fit scrolls (the preview takes what is left, at least PREVIEW_MIN)
  const avail = box.current ? lord.getLayout(box.current.id)[2] : 0;
  const strip = colStrip(dirs.map(widthOf), avail);
  const [sx, setSx] = useState(0);
  const x = Math.min(sx, strip.over);
  // a deeper folder opened (or another one picked): scrolled to the end, so it and the preview are in sight; widening
  // a column keeps the offset, so the first columns stay where they are
  const deepest = chainFolder(chain, ctx.isDir);
  const overNow = useRef(0);
  overNow.current = strip.over;
  useEffect(() => { setSx(overNow.current); }, [deepest, dirs.length]);
  const bar = useRef(null);
  useEffect(() => {
    const mv = addListener('pointermove', (e) => { const b = bar.current; if (b) setSx(Math.max(0, Math.min(b.over, b.x0 + (e.x - b.at) * b.k))); });
    const up = addListener('pointerup', () => { bar.current = null; });
    return () => { mv(); up(); };
  }, []);
  const thumb = strip.over ? Math.max(40, avail * (avail / strip.total)) : 0;
  return (
    <View ref={box} onWheel={(e) => { if (strip.over && e.deltaX) setSx(Math.max(0, Math.min(strip.over, x + e.deltaX))); }}
      style={{ flex: 1, overflow: 'hidden', direction: 'ltr' }}>
    <View style={{ flex: 1, flexDirection: 'row', width: Math.max(avail, strip.total), marginLeft: -x }}>
      {dirs.map((dir, level) => {
        const items = sortEntries(entriesOf(dir), sort, ctx.sortOpts);
        return (
          <View key={dir} dropPath={dir} style={{ width: widthOf(dir), flexDirection: 'row' }}>
            <View style={{ flex: 1, minWidth: 0 }}>
            <ScrollView focusable={false} style={{ flex: 1 }}>
              <View style={{ paddingHorizontal: 6, paddingVertical: 6 }}>
                {items.map((e) => {
                  const inChain = chain[level] === e.path;
                  const sel = ctx.selected.has(e.path) || inChain;
                  return (
                    <DropHot key={e.path} dnd={ctx.dnd} path={e.path}>{(hot) => (
                    <View {...itemHandlers(e, ctx)}
                      onMouseDown={(ev) => { ev.stopPropagation(); setChain([...chain.slice(0, level), e.path]); ctx.onItemDown(e, ev); }}
                      style={{ flexDirection: 'row', alignItems: 'center', gap: 6, height: 24, paddingHorizontal: 8, borderRadius: 5,
                        backgroundColor: hot || sel ? (ctx.selected.has(e.path) ? t.accent : t.hover) : 'transparent' }}>
                      <EntryIcon e={e} size={16} dark={t.mode === 'dark'} thumb={false} tint={tagTint(tagsOf(e.path))} />
                      {/* the tag dots right after the name, as in the list view (the tint shows only on folders, GitHub #99) */}
                      <View style={{ flexShrink: 1, minWidth: 0, ...(ctx.renaming?.path === e.path || !tagsOf(e.path).length ? { flex: 1 } : null) }}><Name e={e} ctx={ctx} color={ctx.selected.has(e.path) ? '#fff' : t.text} /></View>
                      <TagDots tags={tagsOf(e.path)} size={7} />
                      {tagsOf(e.path).length && ctx.renaming?.path !== e.path ? <View style={{ flex: 1 }} /> : null}
                      {e.dir && <Text style={{ fontSize: 12, color: ctx.selected.has(e.path) ? '#fff' : t.textDim }}>›</Text>}
                    </View>
                    )}</DropHot>
                  );
                })}
              </View>
            </ScrollView>
            </View>
            {/* the divider: drag to widen, double click to fit the longest name */}
            <View onMouseDown={(ev) => startDrag(ev, dir)} onDoubleClick={(ev) => fit(ev, dir, items)}
              style={{ width: 5, alignItems: 'center', cursor: 'ew-resize' }}>
              <View style={{ width: 1, flex: 1, backgroundColor: live?.dir === dir ? t.accent : t.separator }} />
            </View>
          </View>
        );
      })}
      <View style={{ width: strip.preview }}>{file ? <Preview e={file} noRead={ctx.cloudOnly?.(file)} /> : null}</View>
    </View>
    {strip.over ? (
      <View style={{ height: 10, justifyContent: 'center', paddingHorizontal: 2 }}>
        <View onMouseDown={(ev) => { ev.stopPropagation(); bar.current = { at: ev.x, x0: x, over: strip.over, k: strip.over / Math.max(1, avail - thumb) }; }}
          style={{ width: thumb, height: 6, borderRadius: 3, backgroundColor: t.textDim, opacity: 0.5, marginLeft: (x / strip.over) * (avail - thumb - 4) }} />
      </View>
    ) : null}
    </View>
  );
}

// ---- gallery ----

export function GalleryView({ entries, ctx }) {
  const t = useTheme();
  const cur = entries.find((e) => ctx.selected.has(e.path)) || entries[0];
  const idx = Math.max(0, entries.indexOf(cur));
  const from = Math.max(0, idx - 4);
  return (
    <View style={{ flex: 1 }}>
      <View style={{ flex: 1, padding: 8 }}>{cur ? <Preview e={cur} big name={ctx.shownName?.(cur)} noRead={ctx.cloudOnly?.(cur)} /> : <EmptyState title={ctx.emptyText} hint={ctx.emptyHint} action={ctx.emptyAction} />}</View>
      {cur && ctx.renaming?.path === cur.path ? <View style={{ alignItems: 'center', paddingBottom: 8 }}><Name e={cur} ctx={ctx} /></View> : null}
      <View style={{ height: 97, borderTopWidth: 1, borderTopColor: t.separator, flexDirection: 'row', gap: 8, paddingHorizontal: 12, alignItems: 'center', justifyContent: 'center', direction: 'ltr', overflow: 'hidden' }}>
        {entries.slice(from, from + 9).map((e) => {
          const sel = e === cur;
          return (
            <View key={e.path} {...itemHandlers(e, ctx)}
              style={{ width: 72, height: 72, alignItems: 'center', justifyContent: 'center', borderRadius: 8,
                border: `2px solid ${sel ? t.accent : 'transparent'}`, backgroundColor: sel ? t.hover : 'transparent' }}>
              <EntryIcon e={e} size={56} dark={t.mode === 'dark'} thumb={!ctx.cloudOnly?.(e)} tint={tagTint(tagsOf(e.path))} />
              {ctx.cloud?.status[e.path] ? <View style={{ position: 'absolute', insetInlineEnd: 2, bottom: 2 }}><StatusBadge s={ctx.cloud.status[e.path]} size={18} /></View> : null}
            </View>
          );
        })}
      </View>
    </View>
  );
}
