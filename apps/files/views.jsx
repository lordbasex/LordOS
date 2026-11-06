// views.jsx — the four Files views: icons (cards), list (with expandable folders),
// columns (Miller, with a preview) and gallery.
import { View, Text, Image, TextInput, ScrollView, EmptyState, useTheme, addListener, direction, formatBytes, typeScale, t as tr, StatusGlyph, StatusBadge } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { EntryIcon, TagDots } from './glyphs.jsx';
import { kindOf, isImage, isTextual } from './kinds.js';
import { tagsOf, commentOf } from './ops.js';
import { visible, resize, moveColumn, dropBefore, totalWidth, NAME_MIN } from './columns.js';
import { sortEntries } from './locations.js';
import { iconMetrics, listMetrics } from './viewprefs.js';
import { when, listRowStyle, listHeaderCell, listCell } from '../../sdk/fileparts.jsx';

// folder of a search or Recents result (the last segment of the path)
const where = (p) => { const parts = String(p).split('/'); return parts[parts.length - 2] || '/'; };
export const kindLabel = (e) => (e.sub ? e.sub : e.app ? tr('files.kind.app') : tr(`files.kind.${kindOf(e)}`));

// Name, or the field for renaming in place. font and line follow the view's zoom (viewprefs.js).
function Name({ e, ctx, center, color, lines = 1, font, line, fieldW }) {
  const size = font || (center ? 12 : 13), lh = line || (size <= 13 ? 16 : Math.round(size * 1.34));
  if (ctx.renaming?.path === e.path) {
    return (
      <View onMouseDown={(ev) => ev.stopPropagation()} style={{ width: fieldW || (center ? 110 : 240) }}>
        <TextInput autoFocus selectAll value={ctx.renaming.value} onChange={ctx.onRenameChange} onSubmit={ctx.onRenameCommit} onCancel={ctx.onRenameCancel}
          style={{ height: 22, paddingHorizontal: 4 }} />
      </View>
    );
  }
  return (
    <Text style={{ fontSize: size, color, textAlign: center ? 'center' : 'start', whiteSpace: lines === 1 ? 'nowrap' : 'normal',
      overflow: 'hidden', maxHeight: lines * lh, lineHeight: lh, textOverflow: 'ellipsis', numberOfLines: lines }}>{ctx.shownName ? ctx.shownName(e) : e.name}</Text>
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

// ---- icons ----

export function IconView({ groups, ctx }) {
  const t = useTheme();
  const m = iconMetrics(ctx.zoom);
  return (
    <ScrollView resetKey={ctx.scrollKey} onWheel={ctx.onZoomWheel} style={{ flex: 1 }}>
      <View style={{ padding: 12, paddingBottom: 32 }}>
        {groups.map((g) => (
          <View key={g.key}>
            <GroupTitle g={g} t={t} />
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8 }}>
              {g.items.map((e) => {
                const sel = ctx.selected.has(e.path);
                return (
                  <View key={e.path} {...itemHandlers(e, ctx)}
                    style={{ width: m.cell, paddingVertical: 6, alignItems: 'center', gap: 5, opacity: ctx.cut.has(e.path) ? 0.45 : 1 }}>
                    <View style={{ padding: 4, borderRadius: 7, backgroundColor: ctx.dropTarget === e.path ? t.accent : sel ? (t.mode === 'dark' ? 'rgba(255,255,255,0.12)' : 'rgba(0,0,0,0.08)') : 'transparent' }}>
                      <EntryIcon e={e} size={m.icon} dark={t.mode === 'dark'} thumb={!ctx.cloudOnly?.(e)} />
                      {ctx.cloud?.status[e.path] ? <View style={{ position: 'absolute', insetInlineEnd: -4, bottom: -4 }}><StatusBadge s={ctx.cloud.status[e.path]} size={Math.max(18, Math.round(m.icon * 0.32))} /></View> : null}
                    </View>
                    <View style={{ maxWidth: m.cell - 4, paddingHorizontal: 5, paddingVertical: 1, borderRadius: 5, backgroundColor: sel ? t.accent : 'transparent', alignItems: 'center' }}>
                      <Name e={e} ctx={ctx} center lines={2} font={m.font} line={m.line} fieldW={m.cell + 10} color={sel ? '#fff' : t.text} />
                    </View>
                    <TagDots tags={tagsOf(e.path)} />
                  </View>
                );
              })}
            </View>
          </View>
        ))}
        {groups.every((g) => !g.items.length) && <EmptyState title={ctx.emptyText} style={{ minHeight: 260 }} />}
      </View>
    </ScrollView>
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
      <View key={e.path} {...itemHandlers(e, ctx)}
        style={[listRowStyle(t, sel || ctx.dropTarget === e.path, odd), { height: m.row, opacity: ctx.cut.has(e.path) ? 0.45 : 1 }]}>
        <View style={{ width: ctx.nameW, flexDirection: 'row', alignItems: 'center', gap: 6, paddingLeft: depth * 16 }}>
          <View onMouseDown={(ev) => { ev.stopPropagation(); if (e.dir && !e.trash) ctx.onToggleExpand(e.path); }} style={{ width: 14, alignItems: 'center' }}>
            {e.dir && !e.trash ? <Text style={{ fontSize: 9, color: sel ? '#fff' : t.textDim }}>{open ? '▼' : '▶'}</Text> : null}
          </View>
          {/* thumbnails from 32 px up, as in the icon view */}
          <EntryIcon e={e} size={m.icon} dark={t.mode === 'dark'} thumb={m.icon >= 32 && !ctx.cloudOnly?.(e)} />
          <View style={{ flex: 1, minWidth: 0 }}><Name e={e} ctx={ctx} font={m.font} color={sel ? '#fff' : t.text} /></View>
          <TagDots tags={tagsOf(e.path)} size={7} />
        </View>
        {visible(ctx.columns).map((k) => (k === 'tags'
          ? <View key={k} style={{ width: ctx.columns.width.tags, paddingHorizontal: 8 }}><TagDots tags={tagsOf(e.path)} size={7} /></View>
          : <Text key={k} style={{ ...listCell(t, sel, ctx.columns.width[k], cellAlign(k)), fontSize: m.cell }}>{cellText(k, e, ctx)}</Text>))}
        {ctx.cloud?.on ? <View style={{ width: STATUS_W, alignItems: 'center', justifyContent: 'center' }}><StatusGlyph s={ctx.cloud.status[e.path]} size={18} /></View> : null}
      </View>,
    );
    if (open) {
      let kids = ctx.kidsOf(e) || [];
      if (!e.netHost) try { kids = sortEntries(lord.fs.list(e.path).filter((x) => ctx.hidden || !x.name.startsWith('.')).map((x) => ({ ...x, path: `${e.path}/${x.name}` })), sort, ctx.sortOpts); } catch (err) { /* no permission */ }
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
  const inner = { ...ctx, nameW };
  const thumb = over ? Math.max(40, room * (room / need)) : 0;
  const bar = useRef(null);
  useEffect(() => {
    const mv = addListener('pointermove', (e) => { const b = bar.current; if (b) setSx(Math.max(0, Math.min(b.over, b.x0 + (e.x - b.at) * b.k))); });
    const up = addListener('pointerup', () => { bar.current = null; });
    return () => { mv(); up(); };
  }, []);
  return (
    <View onWheel={(e) => { if (over && e.deltaX) setSx(Math.max(0, Math.min(over, x + e.deltaX))); }} style={{ flex: 1, overflow: 'hidden' }}>
      <View style={{ flex: 1, width: Math.max(room, need), [direction() === 'rtl' ? 'marginRight' : 'marginLeft']: -x }}>
        <Header sort={sort} onSort={onSort} t={t} ctx={ctx} columns={cols} onColumns={ctx.onColumns} nameW={nameW} />
        <ScrollView resetKey={ctx.scrollKey} onWheel={ctx.onZoomWheel} style={{ flex: 1 }}>
          <View style={{ paddingHorizontal: 8, paddingTop: 4, paddingBottom: 32 }}>
            {groups.map((g) => (
              <View key={g.key}>
                <GroupTitle g={g} t={t} />
                {ListRows({ items: g.items, depth: 0, ctx: inner, t, sort, alt })}
              </View>
            ))}
            {groups.every((g) => !g.items.length) && <EmptyState title={ctx.emptyText} style={{ minHeight: 240 }} />}
          </View>
        </ScrollView>
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
      {isImage(e) && !noRead ? <Image src={e.path} style={{ width: '100%', flex: 1, objectFit: 'contain', borderRadius: 6 }} />
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

export function ColumnView({ root, chain, setChain, entriesOf, ctx, sort }) {
  const t = useTheme();
  const dirs = [root, ...chain.filter((p) => ctx.isDir(p))];
  const last = chain[chain.length - 1];
  const file = last && !ctx.isDir(last) ? ctx.entryAt(last) : null;
  // the ones that fit: the last 3 folders
  const shown = dirs.slice(-3);
  const offset = dirs.length - shown.length;
  return (
    <View style={{ flex: 1, flexDirection: 'row', direction: 'ltr' }}>
      {shown.map((dir, ci) => {
        const items = sortEntries(entriesOf(dir), sort, ctx.sortOpts);
        const level = offset + ci;
        return (
          <View key={dir} dropPath={dir} style={{ width: 220, flexDirection: 'row' }}>
            <View style={{ flex: 1 }}>
            <ScrollView style={{ flex: 1 }}>
              <View style={{ paddingHorizontal: 6, paddingVertical: 6 }}>
                {items.map((e) => {
                  const inChain = chain[level] === e.path;
                  const sel = ctx.selected.has(e.path) || inChain;
                  return (
                    <View key={e.path} {...itemHandlers(e, ctx)}
                      onMouseDown={(ev) => { ev.stopPropagation(); setChain([...chain.slice(0, level), e.path]); ctx.onItemDown(e, ev); }}
                      style={{ flexDirection: 'row', alignItems: 'center', gap: 6, height: 24, paddingHorizontal: 8, borderRadius: 5,
                        backgroundColor: ctx.dropTarget === e.path || sel ? (ctx.selected.has(e.path) ? t.accent : t.hover) : 'transparent' }}>
                      <EntryIcon e={e} size={16} dark={t.mode === 'dark'} thumb={false} />
                      <View style={{ flex: 1, minWidth: 0 }}><Name e={e} ctx={ctx} color={ctx.selected.has(e.path) ? '#fff' : t.text} /></View>
                      {e.dir && <Text style={{ fontSize: 12, color: ctx.selected.has(e.path) ? '#fff' : t.textDim }}>›</Text>}
                    </View>
                  );
                })}
              </View>
            </ScrollView>
            </View>
            <View style={{ width: 1, backgroundColor: t.separator }} />
          </View>
        );
      })}
      <View style={{ flex: 1 }}>{file ? <Preview e={file} noRead={ctx.cloudOnly?.(file)} /> : null}</View>
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
      <View style={{ flex: 1, padding: 8 }}>{cur ? <Preview e={cur} big name={ctx.shownName?.(cur)} noRead={ctx.cloudOnly?.(cur)} /> : <EmptyState title={ctx.emptyText} />}</View>
      {cur && ctx.renaming?.path === cur.path ? <View style={{ alignItems: 'center', paddingBottom: 8 }}><Name e={cur} ctx={ctx} /></View> : null}
      <View style={{ height: 97, borderTopWidth: 1, borderTopColor: t.separator, flexDirection: 'row', gap: 8, paddingHorizontal: 12, alignItems: 'center', justifyContent: 'center', direction: 'ltr', overflow: 'hidden' }}>
        {entries.slice(from, from + 9).map((e) => {
          const sel = e === cur;
          return (
            <View key={e.path} {...itemHandlers(e, ctx)}
              style={{ width: 72, height: 72, alignItems: 'center', justifyContent: 'center', borderRadius: 8,
                border: `2px solid ${sel ? t.accent : 'transparent'}`, backgroundColor: sel ? t.hover : 'transparent' }}>
              <EntryIcon e={e} size={56} dark={t.mode === 'dark'} thumb={!ctx.cloudOnly?.(e)} />
              {ctx.cloud?.status[e.path] ? <View style={{ position: 'absolute', insetInlineEnd: 2, bottom: 2 }}><StatusBadge s={ctx.cloud.status[e.path]} size={18} /></View> : null}
            </View>
          );
        })}
      </View>
    </View>
  );
}
