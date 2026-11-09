// Structure (the approved mock): the table's fields (type, length, unsigned, zerofill, binary, null, key, default, extra,
// collation) and its indexes. A field or index added, changed or removed waits in the footer as ALTER TABLE …; Apply
// shows the whole statement before it runs. A view's structure is read only.
import { View, Text, TextInput, ConfirmDialog, t as tr } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { usePal, Btn, IconBtn, Check, Popup, Field, Sheet, Foot, FootText, CodeBlock, Note, MONO } from './ui.jsx';
import { Grid } from './grid.jsx';
import { run } from './conn.js';
import { tableMeta, forgetMeta } from './viewstate.js';
import { alterSql, alterClause, validateField, validateIndex, FIELD_TYPES, freeName, lengthRule } from './model.js';
import { errText, defaultOptions, indexKindOptions, fieldProblems, n } from './texts.js';
import { highlight } from './sqlview.js';

export function Structure({ s, db, table, view, onChanged }) {
  const p = usePal();
  const [meta, setMeta] = useState(null);
  const [error, setError] = useState('');
  const [fields, setFields] = useState([]);       // as shown: with the pending changes applied
  const [indexes, setIndexes] = useState([]);
  const [changes, setChanges] = useState([]);
  const [selF, setSelF] = useState(-1);
  const [selI, setSelI] = useState(-1);
  const [sheet, setSheet] = useState(null);       // { kind: 'field', field, index } | { kind: 'index' } | { kind: 'apply' } | { kind: 'dropField' } | …
  const [applying, setApplying] = useState(false);

  const load = (fresh) => {
    setError('');
    tableMeta(s.key, db, table, { fresh, view }).then((m) => { setMeta(m); setFields(m.fields); setIndexes(m.indexes); setChanges([]); }).catch((e) => setError(errText(e)));
  };
  useEffect(() => { load(false); }, [db, table]);
  const editable = !view && !s.locked && !s.closed;

  const pending = (c, nextFields, nextIndexes) => { setChanges((list) => [...list, c]); if (nextFields) setFields(nextFields); if (nextIndexes) setIndexes(nextIndexes); };
  const saveField = (f, i) => {
    if (i === null) {
      const after = selF >= 0 ? fields[selF].name : fields.length ? fields[fields.length - 1].name : null;
      const at = selF >= 0 ? selF + 1 : fields.length;
      pending({ op: 'add', field: f, after }, [...fields.slice(0, at), { ...f, key: '' }, ...fields.slice(at)]);
      setSelF(at);
    } else {
      const from = fields[i].name;
      pending({ op: 'change', from, field: f }, fields.map((x, k) => (k === i ? { ...x, ...f } : x)));
    }
    setSheet(null);
  };
  const dropField = () => {
    if (selF < 0) return;
    pending({ op: 'drop', name: fields[selF].name }, fields.filter((_, k) => k !== selF));
    setSelF(-1); setSheet(null);
  };
  const dupField = () => {
    if (selF < 0) return;
    const f = fields[selF];
    setSheet({ kind: 'field', index: null, field: { ...f, name: freeName(`${f.name}_copy`, fields.map((x) => x.name)), key: '', autoIncrement: false, extra: '' } });
  };
  const saveIndex = (x) => { pending({ op: 'addIndex', index: x }, null, [...indexes, { name: x.kind === 'PRIMARY' ? 'PRIMARY' : x.name, unique: x.kind === 'UNIQUE' || x.kind === 'PRIMARY', type: x.kind === 'FULLTEXT' ? 'FULLTEXT' : 'BTREE', cardinality: null, comment: '', columns: x.columns }]); setSheet(null); };
  const dropIndex = () => { if (selI < 0) return; pending({ op: 'dropIndex', name: indexes[selI].name }, null, indexes.filter((_, k) => k !== selI)); setSelI(-1); };
  const apply = async () => {
    setApplying(true);
    try {
      await run(s.key, alterSql(db, table, changes), { src: 'you' });
      forgetMeta(s.key, db, table);
      setSheet(null);
      load(true);
      onChanged?.();
    } catch (e) { setSheet({ kind: 'apply', error: errText(e) }); } finally { setApplying(false); }
  };

  if (error && !meta) return <View style={{ padding: 16 }}><Note kind="bad">{error}</Note></View>;
  const mark = (on) => <View style={{ width: 14, height: 14, borderRadius: 4, alignItems: 'center', justifyContent: 'center', backgroundColor: on ? p.accent : 'transparent', border: `1.5px solid ${on ? p.accent : p.dim}` }}>{on ? <Text style={{ fontSize: 9, fontWeight: 700, color: '#fff' }}>✓</Text> : null}</View>;
  const keyPill = (k) => (k ? (
    <View style={{ paddingHorizontal: 5, paddingVertical: 1, borderRadius: 4, backgroundColor: k === 'PRI' ? 'rgba(255,214,10,0.18)' : 'rgba(100,210,255,0.16)' }}>
      <Text style={{ fontSize: 10.5, fontWeight: 700, color: k === 'PRI' ? p.pk : p.mul }}>{k}</Text>
    </View>
  ) : null);
  const defText = (f) => (f.def.kind === 'null' ? 'NULL' : f.def.kind === 'none' ? '' : f.def.value);
  const fcols = [
    { key: 'name', label: tr('databases.st.field'), width: 150, mono: true, render: (v, f) => <Text style={{ fontSize: 12, fontFamily: MONO, color: p.text, whiteSpace: 'nowrap' }}>{f.name}</Text> },
    { key: 'type', label: tr('databases.st.type'), width: 110, render: (v, f) => <Text style={{ fontSize: 12.5, color: p.text }}>{f.type}</Text> },
    { key: 'len', label: tr('databases.st.length'), width: 110, render: (v, f) => <Text style={{ fontSize: 12, fontFamily: MONO, color: p.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{f.length}</Text> },
    { key: 'un', label: 'UN', width: 44, align: 'center', render: (v, f) => mark(f.unsigned) },
    { key: 'zf', label: 'ZF', width: 44, align: 'center', render: (v, f) => mark(f.zerofill) },
    { key: 'bin', label: 'BIN', width: 44, align: 'center', render: (v, f) => mark(f.binary) },
    { key: 'null', label: tr('databases.st.null'), width: 52, align: 'center', render: (v, f) => mark(f.nullable) },
    { key: 'key', label: tr('databases.st.key'), width: 56, align: 'center', render: (v, f) => keyPill(f.key) },
    { key: 'def', label: tr('databases.st.default'), width: 150, render: (v, f) => <Text style={{ fontSize: 12.5, color: f.def.kind === 'null' ? p.nul : p.text, fontStyle: f.def.kind === 'null' ? 'italic' : 'normal', whiteSpace: 'nowrap', overflow: 'hidden' }}>{defText(f)}</Text> },
    { key: 'extra', label: tr('databases.st.extra'), width: 170, render: (v, f) => <Text style={{ fontSize: 12.5, color: p.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{f.extra}</Text> },
    { key: 'coll', label: tr('databases.st.collation'), width: 170, render: (v, f) => <Text style={{ fontSize: 11.5, fontFamily: MONO, color: p.text, whiteSpace: 'nowrap' }}>{f.collation || ''}</Text> },
    { key: 'comment', label: tr('databases.st.comment'), width: 220, render: (v, f) => <Text style={{ fontSize: 12.5, color: p.dim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{f.comment}</Text> },
  ];
  const icols = [
    { key: 'u', label: tr('databases.st.unique'), width: 70, align: 'center', render: (v, x) => mark(x.unique) },
    { key: 'name', label: tr('databases.st.keyName'), width: 200, render: (v, x) => <Text style={{ fontSize: 12, fontFamily: MONO, color: p.text }}>{x.name}</Text> },
    { key: 'cols', label: tr('databases.st.columns'), width: 260, render: (v, x) => <Text style={{ fontSize: 12, fontFamily: MONO, color: p.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{x.columns.map((c) => (c.sub ? `${c.name}(${c.sub})` : c.name)).join(', ')}</Text> },
    { key: 'type', label: tr('databases.st.indexType'), width: 100, render: (v, x) => <Text style={{ fontSize: 12.5, color: p.text }}>{x.type}</Text> },
    { key: 'card', label: tr('databases.st.cardinality'), width: 110, render: (v, x) => <Text style={{ fontSize: 12.5, color: p.text }}>{x.cardinality === null ? '—' : n(x.cardinality)}</Text> },
    { key: 'comment', label: tr('databases.st.comment'), width: 200, render: (v, x) => <Text style={{ fontSize: 12.5, color: p.dim }}>{x.comment}</Text> },
  ];
  const alter = alterSql(db, table, changes);
  const shortAlter = changes.length ? `ALTER TABLE \`${table}\` ${alterClause(changes[0]).slice(0, 48)}${changes.length > 1 || alterClause(changes[0]).length > 48 ? ' …' : ''}` : '';

  return (
    <View style={{ flex: 1, minHeight: 0 }}>
      <View style={{ flex: 3, minHeight: 120 }}>
        <Grid columns={fcols} rows={fields} cell={() => null} selected={new Set(selF >= 0 ? [selF] : [])}
          onRowClick={(i) => setSelF(i)} onDoubleClick={(i) => editable && setSheet({ kind: 'field', index: i, field: fields[i] })} />
      </View>
      <Foot>
        <IconBtn glyph="+" size={17} title={tr('databases.st.addField')} disabled={!editable} onClick={() => setSheet({ kind: 'field', index: null, field: { name: freeName('field', fields.map((x) => x.name)), type: 'VARCHAR', length: '255', unsigned: false, zerofill: false, binary: false, nullable: true, def: { kind: 'null', value: '' }, extra: '', autoIncrement: false, onUpdate: '', collation: '', comment: '', key: '' } })} />
        <IconBtn glyph="−" size={17} title={tr('databases.st.removeField')} disabled={!editable || selF < 0} onClick={() => setSheet({ kind: 'dropField' })} />
        <IconBtn glyph="⧉" title={tr('databases.st.duplicateField')} disabled={!editable || selF < 0} onClick={dupField} />
        <IconBtn glyph="↻" title={tr('databases.refresh')} onClick={() => load(true)} />
        <FootText style={{ marginLeft: 8 }}>{selF >= 0 ? tr('databases.st.countSel', { count: fields.length, name: fields[selF]?.name || '' }) : tr('databases.st.count', { count: fields.length })}</FootText>
        <View style={{ flex: 1 }} />
        {changes.length ? <FootText mono style={{ marginRight: 8, maxWidth: 380 }}>{tr('databases.st.ready', { sql: shortAlter })}</FootText> : view ? <FootText style={{ marginRight: 8 }}>{tr('databases.st.viewReadOnly')}</FootText> : null}
        {changes.length ? <Btn small label={tr('databases.discard')} onClick={() => load(true)} style={{ marginRight: 6 }} /> : null}
        {changes.length ? <Btn small primary label={tr('databases.apply')} onClick={() => setSheet({ kind: 'apply' })} /> : null}
      </Foot>
      <View style={{ height: 30, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 12, backgroundColor: p.bar }}>
        <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 1, backgroundColor: p.line }} />
        <Text style={{ fontSize: 11, fontWeight: 600, color: p.dim, letterSpacing: 0.5 }}>{tr('databases.st.indexes')}</Text>
      </View>
      <View style={{ flex: 1, minHeight: 110 }}>
        <Grid columns={icols} rows={indexes} cell={() => null} selected={new Set(selI >= 0 ? [selI] : [])} onRowClick={(i) => setSelI(i)}
          emptyText={view ? tr('databases.st.viewNoIndexes') : tr('databases.st.noIndexes')} />
      </View>
      <Foot>
        <IconBtn glyph="+" size={17} title={tr('databases.st.addIndex')} disabled={!editable} onClick={() => setSheet({ kind: 'index' })} />
        <IconBtn glyph="−" size={17} title={tr('databases.st.removeIndex')} disabled={!editable || selI < 0} onClick={dropIndex} />
        <FootText style={{ marginLeft: 8 }}>{tr('databases.st.indexCount', { count: indexes.length })}</FootText>
        {s.locked && !view ? <FootText style={{ marginLeft: 12 }} color={p.warn}>{tr('databases.lock.footer')}</FootText> : null}
      </Foot>

      {sheet?.kind === 'field' ? <FieldSheet field={sheet.field} isNew={sheet.index === null} others={fields.filter((_, k) => k !== sheet.index).map((f) => f.name)}
        onCancel={() => setSheet(null)} onSave={(f) => saveField(f, sheet.index)} /> : null}
      {sheet?.kind === 'index' ? <IndexSheet fields={fields} onCancel={() => setSheet(null)} onSave={saveIndex} /> : null}
      {sheet?.kind === 'dropField' && selF >= 0 ? (
        <ConfirmDialog title={tr('databases.st.dropTitle', { name: fields[selF].name })} detail={tr('databases.st.dropDetail')} destructive
          okLabel={tr('databases.st.removeField')} cancelLabel={tr('databases.cancel')} onOk={dropField} onCancel={() => setSheet(null)} />
      ) : null}
      {sheet?.kind === 'apply' ? (
        <Sheet title={tr('databases.st.applyTitle', { table })} width={640} onCancel={() => setSheet(null)}
          actions={[<Btn key="c" label={tr('databases.cancel')} onClick={() => setSheet(null)} />, <Btn key="a" primary disabled={applying} label={applying ? tr('databases.applying') : tr('databases.apply')} onClick={apply} />]}>
          <Text style={{ fontSize: 12.5, color: p.dim, lineHeight: 1.4, textAlign: 'start' }}>{tr('databases.st.applyDetail')}</Text>
          <CodeBlock lines={highlight(alter, p.dark)} maxLines={16} />
          {sheet.error ? <Note kind="bad">{sheet.error}</Note> : null}
        </Sheet>
      ) : null}
    </View>
  );
}

function FieldSheet({ field, isNew, others, onCancel, onSave }) {
  const p = usePal();
  const [f, setF] = useState(() => ({ ...field, def: { ...field.def }, onUpdate: field.onUpdate || '' }));
  const [problem, setProblem] = useState('');
  const set = (patch) => { setF((x) => ({ ...x, ...patch })); setProblem(''); };
  const save = () => {
    const probs = validateField(f);
    if (others.includes(f.name)) probs.push({ field: 'name', code: 'nameTaken' });
    if (probs.length) { setProblem(fieldProblems(probs)); return; }
    onSave({ ...f, extra: [f.autoIncrement ? 'auto_increment' : '', f.onUpdate ? `on update ${f.onUpdate}` : ''].filter(Boolean).join(' ') });
  };
  const type = String(f.type).toUpperCase();
  const timeType = type === 'DATETIME' || type === 'TIMESTAMP';
  return (
    <Sheet title={isNew ? tr('databases.st.addField') : tr('databases.st.editField', { name: field.name })} width={560} onCancel={onCancel}
      actions={[<Btn key="c" label={tr('databases.cancel')} onClick={onCancel} />, <Btn key="s" primary label={tr('databases.st.keepChange')} onClick={save} />]}>
      <Field label={tr('databases.st.field')}><TextInput autoFocus value={f.name} onChange={(v) => set({ name: v })} /></Field>
      <Field label={tr('databases.st.type')}><Popup width={220} value={type} options={FIELD_TYPES.map((x) => [x, x])} onChange={(v) => set({ type: v, length: lengthRule(v) === 'none' ? '' : f.length })} /></Field>
      <Field label={tr('databases.st.length')}><TextInput value={f.length} onChange={(v) => set({ length: v })} placeholder={lengthRule(type) === 'required' ? tr('databases.st.lengthNeeded') : lengthRule(type) === 'none' ? '—' : tr('databases.f.optional')} /></Field>
      <Field label="">
        <View style={{ flexDirection: 'row', gap: 16, flexWrap: 'wrap' }}>
          <Check on={!!f.unsigned} onChange={(v) => set({ unsigned: v })} label={tr('databases.st.unsigned')} />
          <Check on={!!f.zerofill} onChange={(v) => set({ zerofill: v })} label={tr('databases.st.zerofill')} />
          <Check on={!!f.binary} onChange={(v) => set({ binary: v })} label={tr('databases.st.binary')} />
          <Check on={!!f.nullable} onChange={(v) => set({ nullable: v, def: !v && f.def.kind === 'null' ? { kind: 'none', value: '' } : f.def })} label={tr('databases.st.allowNull')} />
          <Check on={!!f.autoIncrement} onChange={(v) => set({ autoIncrement: v })} label={tr('databases.st.autoIncrement')} />
        </View>
      </Field>
      <Field label={tr('databases.st.default')}>
        <View style={{ flexDirection: 'row', gap: 8 }}>
          <Popup width={170} value={f.def.kind} options={defaultOptions()} onChange={(k) => set({ def: { kind: k, value: k === 'expr' ? 'CURRENT_TIMESTAMP' : k === 'value' ? f.def.value : '' } })} />
          {f.def.kind === 'value' || f.def.kind === 'expr' ? <TextInput value={f.def.value} onChange={(v) => set({ def: { ...f.def, value: v } })} style={{ flex: 1 }} /> : null}
        </View>
      </Field>
      {timeType ? <Field label=""><Check on={!!f.onUpdate} onChange={(v) => set({ onUpdate: v ? 'CURRENT_TIMESTAMP' : '' })} label={tr('databases.st.onUpdate')} /></Field> : null}
      <Field label={tr('databases.st.collation')}><TextInput value={f.collation || ''} onChange={(v) => set({ collation: v.trim() })} placeholder={tr('databases.st.collationHint')} /></Field>
      <Field label={tr('databases.st.comment')}><TextInput value={f.comment || ''} onChange={(v) => set({ comment: v })} /></Field>
      <Text style={{ fontSize: 12, color: p.dim, lineHeight: 1.4, textAlign: 'start' }}>{tr('databases.st.notYet')}</Text>
      {problem ? <Note kind="bad">{problem}</Note> : null}
    </Sheet>
  );
}

function IndexSheet({ fields, onCancel, onSave }) {
  const p = usePal();
  const [kind, setKind] = useState('INDEX');
  const [name, setName] = useState('');
  const [cols, setCols] = useState([]);
  const [problem, setProblem] = useState('');
  const toggle = (c) => setCols((l) => (l.includes(c) ? l.filter((x) => x !== c) : [...l, c]));
  const save = () => {
    const x = { kind, name: kind === 'PRIMARY' ? '' : name.trim() || `idx_${cols.join('_')}`.slice(0, 64), columns: cols.map((c) => ({ name: c, sub: null })) };
    const probs = validateIndex(x, fields);
    if (probs.length) { setProblem(fieldProblems(probs)); return; }
    onSave(x);
  };
  return (
    <Sheet title={tr('databases.st.addIndex')} width={480} onCancel={onCancel}
      actions={[<Btn key="c" label={tr('databases.cancel')} onClick={onCancel} />, <Btn key="s" primary label={tr('databases.st.keepChange')} onClick={save} />]}>
      <Field label={tr('databases.st.indexType')} labelWidth={100}><Popup width={200} value={kind} options={indexKindOptions()} onChange={setKind} /></Field>
      {kind !== 'PRIMARY' ? <Field label={tr('databases.st.keyName')} labelWidth={100}><TextInput value={name} onChange={setName} placeholder={cols.length ? `idx_${cols.join('_')}` : ''} /></Field> : null}
      <Field label={tr('databases.st.columns')} labelWidth={100} top>
        <View style={{ gap: 6, maxHeight: 220, overflow: 'hidden' }}>
          {fields.map((f) => (
            <View key={f.name} style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
              <Check on={cols.includes(f.name)} onChange={() => toggle(f.name)} label={f.name} />
              {cols.includes(f.name) ? <Text style={{ fontSize: 11, color: p.dim }}>{cols.indexOf(f.name) + 1}</Text> : null}
            </View>
          ))}
        </View>
      </Field>
      {problem ? <Note kind="bad">{problem}</Note> : null}
    </Sheet>
  );
}
