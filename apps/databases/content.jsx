// Content (the approved mock): the table's rows, 1,000 to a page, with the filter bar (a column, an operator and a
// value; more rules with +), sorting by a click on a column's header, a cell edited in place (Enter writes it with an
// UPDATE keyed by the primary key, Esc leaves it), and rows added, duplicated and deleted. A table without a primary or
// unique key, a view and a locked connection only show their rows, and say why.
import { View, Text, TextInput, ContextMenu, ConfirmDialog, ScrollView, clipboard, useKeyboard, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { usePal, Btn, IconBtn, Popup, Field, Sheet, Foot, FootText, Note, Check } from './ui.jsx';
import { Grid } from './grid.jsx';
import { run, run1 } from './conn.js';
import { tableMeta, viewOf, patchView } from './viewstate.js';
import {
  pageSql, countSql, pageCount, nextSort, pageRange, rowKey, updateCellSql, selectRowSql, deleteRowsSql, insertRowSql, duplicateValues,
  columnWidths, isNumericColumn, cellText, cellInput, isHexValue, opNeedsValue, toInserts, PAGE_SIZE,
} from './model.js';
import { errText, opOptions, n } from './texts.js';

export function Content({ s, db, table, view, status }) {
  const p = usePal();
  const saved = viewOf(s.key).content?.[`${db}.${table}`] || {};
  const [meta, setMeta] = useState(null);
  const [rules, setRules] = useState(saved.rules || [{ column: '', op: '=', value: '', value2: '' }]);
  const [applied, setApplied] = useState(saved.applied || []);
  const [sort, setSort] = useState(saved.sort || null);
  const [page, setPage] = useState(saved.page || 0);
  const [data, setData] = useState({ columns: [], rows: [] });
  const [total, setTotal] = useState(null);
  const [loading, setLoading] = useState(false);
  const [sel, setSel] = useState(new Set());
  const anchor = useRef(-1);
  const [editing, setEditing] = useState(null);      // { row, col, text, nul }
  const [msg, setMsg] = useState(null);              // { kind, text }
  const [menu, setMenu] = useState(null);
  const [sheet, setSheet] = useState(null);
  const root = useRef(null);

  // remember the filter, sort and page per table while the tab lives
  useEffect(() => {
    const v = viewOf(s.key);
    patchView(s.key, { content: { ...(v.content || {}), [`${db}.${table}`]: { rules, applied, sort, page } } });
  }, [rules, applied, sort, page]);

  useEffect(() => { tableMeta(s.key, db, table, { view }).then(setMeta).catch((e) => setMsg({ kind: 'bad', text: errText(e) })); }, [db, table]);

  const load = async (opts = {}) => {
    const q = { db, table, rules: opts.rules ?? applied, sort: opts.sort !== undefined ? opts.sort : sort, page: opts.page ?? page };
    setLoading(true);
    try {
      const r = await run1(s.key, pageSql(q), { maxRows: PAGE_SIZE });
      setData({ columns: r.columns.map((c) => ({ ...c })), rows: r.rows });
      setSel(new Set()); setEditing(null);
      if (opts.count !== false) count(q.rules);
    } catch (e) { setMsg({ kind: 'bad', text: errText(e) }); } finally { setLoading(false); }
  };
  useEffect(() => { load(); }, [db, table]);
  // how many rows: COUNT(*) with a filter, or for a table that isn't huge; the server's estimate otherwise (a COUNT(*)
  // of millions of rows would hold the connection, and every other query waits behind it)
  const count = (r) => {
    if (r.length) { run1(s.key, countSql({ db, table, rules: r })).then((c) => setTotal(Number(c.rows[0]?.[0]) || 0)).catch(() => setTotal(null)); return; }
    if (!status || status.rows === null) { setTotal(null); return; }
    if (status.rows > 200000) { setTotal({ estimate: status.rows }); return; }
    run1(s.key, countSql({ db, table, rules: [] })).then((c) => setTotal(Number(c.rows[0]?.[0]) || 0)).catch(() => setTotal(null));
  };
  useEffect(() => { if (status && !applied.length) count([]); }, [status]);

  const key = meta ? rowKey(meta.fields, meta.indexes, { view }) : { error: 'nokey' };
  const columns = data.columns.map((c) => c.name);
  const why = view ? tr('databases.err.view') : s.locked ? tr('databases.err.readonly') : key.error ? tr('databases.err.nokey') : '';
  const editable = !why && !s.closed;
  const totalNum = total && typeof total === 'object' ? total.estimate : total;
  const pages = totalNum === null ? page + (data.rows.length === PAGE_SIZE ? 2 : 1) : pageCount(totalNum);
  const [from, to] = pageRange(page, data.rows.length);

  const applyFilter = () => {
    const r = rules.filter((x) => x.column && (!opNeedsValue(x.op) || x.value !== '' || x.op === '=' || x.op === '!='));
    setApplied(r); setPage(0); load({ rules: r, page: 0 });
  };
  const clearFilter = () => { setRules([{ column: '', op: '=', value: '', value2: '' }]); setApplied([]); setPage(0); load({ rules: [], page: 0 }); };
  const onHeader = (c) => { const ns = nextSort(sort, c.label); setSort(ns); setPage(0); load({ sort: ns, page: 0, count: false }); };
  const goPage = (pg) => { setPage(pg); load({ page: pg, count: false }); };

  const select = (i, e) => {
    if (editing && editing.row !== i) setEditing(null);
    setSel((cur) => {
      if (e?.shift && anchor.current >= 0) { const a = Math.min(anchor.current, i), b = Math.max(anchor.current, i); const out = new Set(); for (let k = a; k <= b; k++) out.add(k); return out; }
      if (e?.ctrl || e?.meta) { const out = new Set(cur); if (out.has(i)) out.delete(i); else out.add(i); anchor.current = i; return out; }
      anchor.current = i;
      return new Set([i]);
    });
  };
  const startEdit = (row, col) => {
    if (!editable) { setMsg({ kind: 'warn', text: why || tr('databases.err.closed') }); return; }
    const v = data.rows[row][col];
    setSel(new Set([row])); anchor.current = row;
    setEditing({ row, col, text: v === null ? '' : isHexValue(v) ? cellText(v, 100000).replace(/…$/, '') : String(v), nul: v === null });
  };
  const binaryCol = (col) => !!data.columns[col]?.flags?.binary && data.columns[col]?.charset === 63;
  const commit = async (value) => {
    const { row, col } = editing;
    const old = data.rows[row];
    const v = value === null ? null : cellInput(value, binaryCol(col));
    if (v === old[col]) { setEditing(null); return; }
    const u = updateCellSql({ db, table, key, columns, row: old, column: columns[col], value: v });
    if (u.error) { setMsg({ kind: 'bad', text: errText({ code: u.error }) }); return; }
    try {
      const r = await run(s.key, u.sql, { src: 'you' });
      const res = (r.results || [])[0] || {};
      const fresh = old.slice(); fresh[col] = v;
      setData((d) => ({ ...d, rows: d.rows.map((x, k) => (k === row ? fresh : x)) }));
      setEditing(null);
      setMsg(res.affected === 0 ? { kind: 'warn', text: tr('databases.content.noChange') } : null);
      // what the server kept (a trimmed value, a default, a trigger)
      const again = selectRowSql({ db, table, key, columns, row: fresh });
      if (!again.error) run1(s.key, again.sql).then((x) => { if (x.rows[0]) setData((d) => ({ ...d, rows: d.rows.map((y, k) => (k === row ? x.rows[0] : y)) })); }).catch(() => {});
    } catch (e) { setMsg({ kind: 'bad', text: errText(e) }); }
  };
  const remove = async () => {
    const rows = [...sel].sort((a, b) => a - b).map((i) => data.rows[i]);
    const d = deleteRowsSql({ db, table, key, columns, rows });
    setSheet(null);
    if (d.error) { setMsg({ kind: 'bad', text: errText({ code: d.error }) }); return; }
    try { await run(s.key, d.sql, { src: 'you' }); load(); } catch (e) { setMsg({ kind: 'bad', text: errText(e) }); }
  };
  const duplicate = async () => {
    const i = [...sel][0];
    if (i === undefined || !meta) return;
    const ins = insertRowSql({ db, table, values: duplicateValues(meta.fields, columns, data.rows[i]) });
    if (ins.error) { setMsg({ kind: 'bad', text: errText({ code: ins.error }) }); return; }
    try { await run(s.key, ins.sql, { src: 'you' }); load(); setMsg({ kind: 'ok', text: tr('databases.content.duplicated') }); } catch (e) { setMsg({ kind: 'bad', text: errText(e) }); }
  };
  const insert = async (values) => {
    const ins = insertRowSql({ db, table, values });
    if (ins.error) return errText({ code: ins.error });
    try { await run(s.key, ins.sql, { src: 'you' }); setSheet(null); load(); return ''; } catch (e) { return errText(e); }
  };

  useKeyboard((e) => {
    if (editing || sheet || menu) return;
    const cur = [...sel];
    const last = cur.length ? Math.max(...cur) : -1;
    if (e.key === 'ArrowDown' && data.rows.length) select(Math.min(data.rows.length - 1, last + 1), { shift: e.shift });
    else if (e.key === 'ArrowUp' && data.rows.length) select(Math.max(0, (cur.length ? Math.min(...cur) : 1) - 1), { shift: e.shift });
    else if ((e.key === 'Delete' || e.key === 'Backspace') && cur.length && editable) setSheet({ kind: 'delete' });
    else if (e.key === 'Enter' && cur.length === 1) startEdit(cur[0], 0);
    else if ((e.ctrl || e.meta) && (e.base || '').toLowerCase() === 'c' && cur.length) {
      clipboard.set(cur.sort((a, b) => a - b).map((i) => data.rows[i].map((v) => cellText(v, 100000)).join('\t')).join('\n'));
    }
  });

  const widths = columnWidths(columns, data.rows);
  const gcols = data.columns.map((c, i) => ({ key: `${i}`, index: i, label: c.name, width: widths[i], align: isNumericColumn(c) ? 'end' : 'start', mono: isNumericColumn(c) || /DATE|TIME|YEAR/.test(c.typeName || '') }));
  const cell = (r, c) => (r[c.index] === null || r[c.index] === undefined ? null : cellText(r[c.index]));
  const cols = columns.map((c) => [c, c]);
  const ctx = (i, k, e) => {
    select(i);
    const pt = root.current ? e.local(root.current) : { x: e.x, y: e.y };
    setMenu({ x: pt.x, y: pt.y, items: [
      { label: tr('databases.content.edit'), disabled: !editable, onClick: () => startEdit(i, k) },
      { label: tr('databases.content.setNull'), disabled: !editable || data.rows[i][k] === null, onClick: () => commitNull(i, k) },
      '-',
      { label: tr('databases.content.copyValue'), onClick: () => clipboard.set(cellText(data.rows[i][k], 1e7)) },
      { label: tr('databases.content.copyInsert'), onClick: () => clipboard.set(toInserts(table, columns, [data.rows[i]])) },
      '-',
      { label: tr('databases.content.deleteRow'), destructive: true, disabled: !editable, onClick: () => setSheet({ kind: 'delete' }) },
    ] });
  };
  const commitNull = (i, k) => {
    const old = data.rows[i];
    const u = updateCellSql({ db, table, key, columns, row: old, column: columns[k], value: null });
    setEditing(null);
    if (u.error) { setMsg({ kind: 'bad', text: errText({ code: u.error }) }); return; }
    run(s.key, u.sql, { src: 'you' }).then(() => setData((d) => ({ ...d, rows: d.rows.map((x, j) => (j === i ? x.map((y, m) => (m === k ? null : y)) : x)) })))
      .catch((e) => setMsg({ kind: 'bad', text: errText(e) }));
  };

  const editor = editing ? (
    <TextInput autoFocus value={editing.text} onChange={(t) => setEditing((x) => ({ ...x, text: t }))} onSubmit={(t) => commit(t)} onCancel={() => setEditing(null)}
      style={{ flex: 1, height: 25, borderRadius: 0, paddingHorizontal: 8 }} size={12.5} />
  ) : null;
  const sortLabel = sort ? `${sort.column} ${sort.dir === 'desc' ? '↓' : '↑'}` : tr('databases.content.sortNone');
  const totalText = total === null ? '' : typeof total === 'object' ? tr('databases.content.ofEstimate', { total: n(total.estimate) }) : tr('databases.content.of', { total: n(total) });
  const filteredFrom = applied.length && status && status.rows !== null ? tr('databases.content.filteredFrom', { total: n(status.rows) }) : '';
  const editingName = editing ? tr('databases.content.editing', { name: columns[editing.col] }) : '';

  return (
    <View ref={root} style={{ flex: 1, minHeight: 0 }}>
      {/* filter bar */}
      {rules.map((r, i) => (
        <View key={i} style={{ height: 42, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 12, backgroundColor: p.bar }}>
          <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 1, backgroundColor: p.line }} />
          <Text style={{ width: 52, fontSize: 12.5, color: p.dim, textAlign: 'start' }}>{i === 0 ? tr('databases.content.search') : tr('databases.content.and')}</Text>
          <Popup small value={r.column} options={cols} placeholder={tr('databases.content.column')} onChange={(v) => setRules((l) => l.map((x, k) => (k === i ? { ...x, column: v } : x)))} />
          <Popup small value={r.op} options={opOptions()} onChange={(v) => setRules((l) => l.map((x, k) => (k === i ? { ...x, op: v } : x)))} />
          {opNeedsValue(r.op) ? <TextInput value={r.value} onChange={(v) => setRules((l) => l.map((x, k) => (k === i ? { ...x, value: v } : x)))} onSubmit={applyFilter}
            placeholder={r.op === 'in' ? tr('databases.content.listHint') : tr('databases.content.value')} style={{ width: 200, height: 26 }} size={12.5} /> : null}
          {r.op === 'between' ? <Text style={{ fontSize: 12.5, color: p.dim }}>{tr('databases.content.betweenAnd')}</Text> : null}
          {r.op === 'between' ? <TextInput value={r.value2} onChange={(v) => setRules((l) => l.map((x, k) => (k === i ? { ...x, value2: v } : x)))} onSubmit={applyFilter} style={{ width: 120, height: 26 }} size={12.5} /> : null}
          {i === 0 ? <Btn small primary label={tr('databases.content.filter')} onClick={applyFilter} style={{ height: 26 }} /> : null}
          {i === rules.length - 1 ? <IconBtn glyph="+" size={16} title={tr('databases.content.addRule')} onClick={() => setRules((l) => [...l, { column: '', op: '=', value: '', value2: '' }])} /> : null}
          {i > 0 ? <IconBtn glyph="−" size={16} title={tr('databases.content.removeRule')} onClick={() => setRules((l) => l.filter((_, k) => k !== i))} /> : null}
          {i === 0 && applied.length ? <Btn small link label={tr('databases.content.clearFilter')} onClick={clearFilter} /> : null}
          <View style={{ flex: 1 }} />
          {i === 0 ? <Text style={{ fontSize: 12, color: p.dim, whiteSpace: 'nowrap' }}>{tr('databases.content.sort')}</Text> : null}
          {i === 0 ? <Text style={{ fontSize: 12, fontWeight: 600, color: p.text, whiteSpace: 'nowrap' }}>{sortLabel}</Text> : null}
        </View>
      ))}
      {msg ? <Note kind={msg.kind} style={{ borderRadius: 0 }} action={<Btn small label={tr('databases.dismiss')} onClick={() => setMsg(null)} />}>{msg.text}</Note> : null}
      <Grid columns={gcols} rows={data.rows} cell={cell} selected={sel} onRowClick={select} sort={sort} onHeader={onHeader}
        onDoubleClick={(i, k) => startEdit(i, k)} onContextMenu={ctx} editing={editing ? { row: editing.row, col: editing.col } : null} editor={editor}
        emptyText={loading ? tr('databases.loading') : applied.length ? tr('databases.content.noMatch') : tr('databases.content.empty')} />
      <Foot>
        <IconBtn glyph="+" size={17} title={tr('databases.content.addRow')} disabled={!editable || !meta} onClick={() => setSheet({ kind: 'add' })} />
        <IconBtn glyph="−" size={17} title={tr('databases.content.deleteRows')} disabled={!editable || !sel.size} onClick={() => setSheet({ kind: 'delete' })} />
        <IconBtn glyph="⧉" title={tr('databases.content.duplicateRow')} disabled={!editable || sel.size !== 1} onClick={duplicate} />
        <IconBtn glyph="↻" title={tr('databases.refresh')} onClick={() => load()} />
        <FootText style={{ marginLeft: 8, flex: 1 }}>
          {[data.rows.length ? tr('databases.content.rows', { from: n(from), to: n(to) }) + (totalText ? ` ${totalText}` : '') : '', filteredFrom,
            sel.size ? tr('databases.content.selected', { count: sel.size }) : '', editingName, why && !s.closed ? why : ''].filter(Boolean).join(' · ')}
        </FootText>
        <IconBtn glyph="‹" size={16} title={tr('databases.content.prev')} disabled={page <= 0 || loading} onClick={() => goPage(page - 1)} />
        <FootText color={p.text}>{tr('databases.content.page', { page: page + 1, pages: Math.max(pages, page + 1) })}</FootText>
        <IconBtn glyph="›" size={16} title={tr('databases.content.next')} disabled={loading || data.rows.length < PAGE_SIZE || (totalNum !== null && page + 1 >= pages)} onClick={() => goPage(page + 1)} />
      </Foot>
      {menu ? <ContextMenu x={menu.x} y={menu.y} items={menu.items} onClose={() => setMenu(null)} /> : null}
      {sheet?.kind === 'delete' ? (
        <ConfirmDialog title={tr('databases.content.deleteTitle', { count: sel.size })} detail={tr('databases.content.deleteDetail', { table })} destructive
          okLabel={tr('databases.content.deleteRows')} cancelLabel={tr('databases.cancel')} onOk={remove} onCancel={() => setSheet(null)} />
      ) : null}
      {sheet?.kind === 'add' && meta ? <AddRowSheet fields={meta.fields} onCancel={() => setSheet(null)} onSave={insert} /> : null}
    </View>
  );
}

// a new row: a field per column; left empty, a column takes its default
function AddRowSheet({ fields, onCancel, onSave }) {
  const p = usePal();
  const [vals, setVals] = useState({});
  const [nulls, setNulls] = useState({});
  const [error, setError] = useState('');
  const save = async () => {
    const values = {};
    for (const f of fields) {
      if (nulls[f.name]) values[f.name] = null;
      else if (vals[f.name] !== undefined && vals[f.name] !== '') values[f.name] = cellInput(vals[f.name], /BINARY|BLOB/.test(f.type));
    }
    setError(await onSave(values));
  };
  return (
    <Sheet title={tr('databases.content.addRow')} width={560} onCancel={onCancel}
      actions={[<Btn key="c" label={tr('databases.cancel')} onClick={onCancel} />, <Btn key="s" primary label={tr('databases.content.insert')} onClick={save} />]}>
      <Text style={{ fontSize: 12, color: p.dim, lineHeight: 1.4, textAlign: 'start' }}>{tr('databases.content.addHint')}</Text>
      <ScrollView style={{ maxHeight: 380 }}>
        <View style={{ gap: 8, paddingRight: 8 }}>
          {fields.map((f) => (
            <Field key={f.name} label={f.name} labelWidth={140}>
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
                <TextInput value={vals[f.name] || ''} onChange={(v) => setVals((o) => ({ ...o, [f.name]: v }))} style={{ flex: 1 }}
                  placeholder={f.autoIncrement ? tr('databases.content.auto') : f.def.kind === 'value' || f.def.kind === 'expr' ? f.def.value : f.def.kind === 'null' ? 'NULL' : f.type} />
                {f.nullable ? <Check on={!!nulls[f.name]} onChange={(on) => setNulls((o) => ({ ...o, [f.name]: on }))} label="NULL" /> : null}
              </View>
            </Field>
          ))}
        </View>
      </ScrollView>
      {error ? <Note kind="bad">{error}</Note> : null}
    </Sheet>
  );
}

