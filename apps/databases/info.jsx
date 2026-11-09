// Table Info (the approved mock): the table's engine, row format, encoding, collation, dates and next auto-increment
// value; its size (rows, average row, data, indexes, free space, with a bar); its comment; its CREATE syntax, in color,
// with Copy.
import { View, Text, ScrollView, clipboard, t as tr } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { usePal, Btn, Note, CodeBlock, MONO } from './ui.jsx';
import { run1 } from './conn.js';
import { createSql, parseCreate, humanBytes } from './model.js';
import { errText, n } from './texts.js';
import { highlight } from './sqlview.js';

export function TableInfo({ s, db, table, view, status }) {
  const p = usePal();
  const [create, setCreate] = useState('');
  const [error, setError] = useState('');
  useEffect(() => {
    run1(s.key, createSql(db, table, view)).then((r) => setCreate(parseCreate(r))).catch((e) => setError(errText(e)));
  }, [db, table]);
  const st = status || {};
  const total = (st.data || 0) + (st.index || 0) + (st.free || 0);
  const frac = (x) => (total ? Math.max(0, Math.min(1, (x || 0) / total)) : 0);
  const kv = (rows) => rows.map(([k, val, mono]) => (
    <View key={k} style={{ flexDirection: 'row', gap: 12 }}>
      <Text style={{ width: 150, fontSize: 12.5, color: p.dim, textAlign: 'end' }}>{k}</Text>
      <Text style={{ flex: 1, fontSize: mono ? 12 : 12.5, fontFamily: mono ? MONO : undefined, color: p.text, textAlign: 'start' }}>{val === null || val === undefined || val === '' ? '—' : String(val)}</Text>
    </View>
  ));
  return (
    <ScrollView style={{ flex: 1 }} contentStyle={{ paddingHorizontal: 18, paddingVertical: 16, gap: 14 }}>
      {error ? <Note kind="bad">{error}</Note> : null}
      {!view ? (
        <View style={{ flexDirection: 'row', gap: 14 }}>
          <Card title={tr('databases.info.table')}>
            {kv([
              [tr('databases.info.engine'), st.engine], [tr('databases.info.rowFormat'), st.rowFormat], [tr('databases.info.encoding'), st.charset],
              [tr('databases.info.collation'), st.collation, true], [tr('databases.info.created'), st.created], [tr('databases.info.updated'), st.updated],
              [tr('databases.info.autoIncrement'), st.autoIncrement === null || st.autoIncrement === undefined ? null : n(st.autoIncrement), true],
            ])}
          </Card>
          <Card title={tr('databases.info.size')}>
            {kv([
              [tr('databases.info.rows'), st.rows === null || st.rows === undefined ? null : tr('databases.info.estimate', { count: n(st.rows) })],
              [tr('databases.info.avgRow'), st.avgRow === null || st.avgRow === undefined ? null : tr('databases.info.bytes', { count: n(st.avgRow) })],
              [tr('databases.info.data'), humanBytes(st.data)], [tr('databases.info.indexes'), humanBytes(st.index)], [tr('databases.info.free'), humanBytes(st.free)],
            ])}
            <View style={{ height: 8, borderRadius: 4, backgroundColor: p.dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.08)', overflow: 'hidden', flexDirection: 'row', marginTop: 12, direction: 'ltr' }}>
              <View style={{ width: `${Math.round(frac(st.data) * 1000) / 10}%`, backgroundColor: p.accent }} />
              <View style={{ width: `${Math.round(frac(st.index) * 1000) / 10}%`, backgroundColor: p.view }} />
            </View>
            <View style={{ flexDirection: 'row', gap: 14, marginTop: 8 }}>
              <Legend color={p.accent} label={tr('databases.info.data')} />
              <Legend color={p.view} label={tr('databases.info.indexes')} />
              <Legend label={tr('databases.info.free')} />
            </View>
          </Card>
        </View>
      ) : null}
      {!view ? (
        <Card title={tr('databases.info.comments')}>
          <View style={{ backgroundColor: p.input, border: `1px solid ${p.line}`, borderRadius: 7, paddingHorizontal: 10, paddingVertical: 7 }}>
            <Text style={{ fontSize: 12.5, color: st.comment ? p.text : p.dim, textAlign: 'start', lineHeight: 1.4 }}>{st.comment || tr('databases.info.noComment')}</Text>
          </View>
        </Card>
      ) : null}
      <Card title={tr('databases.info.create')} right={<Btn small label={tr('databases.copy')} disabled={!create} onClick={() => clipboard.set(create)} />}>
        {create ? <CodeBlock lines={highlight(create, p.dark)} /> : <Text style={{ fontSize: 12.5, color: p.dim }}>{tr('databases.loading')}</Text>}
      </Card>
    </ScrollView>
  );
}

function Card({ title, children, right }) {
  const p = usePal();
  return (
    <View style={{ flex: 1, minWidth: 0, backgroundColor: p.box, border: `1px solid ${p.line}`, borderRadius: 10, paddingHorizontal: 18, paddingVertical: 14, gap: 7 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', marginBottom: 3 }}>
        <Text style={{ flex: 1, fontSize: 11, fontWeight: 600, color: p.dim, letterSpacing: 0.5, textAlign: 'start' }}>{title}</Text>
        {right || null}
      </View>
      {children}
    </View>
  );
}
function Legend({ color, label }) {
  const p = usePal();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 5 }}>
      {color ? <View style={{ width: 8, height: 8, borderRadius: 2, backgroundColor: color }} /> : null}
      <Text style={{ fontSize: 11.5, color: p.dim }}>{label}</Text>
    </View>
  );
}
