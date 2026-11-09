// Query (the approved mock): an editor with line numbers and SQL colors (the Editor's own view, apps/editor), completion
// of table and column names and keywords (as you type, or Ctrl+Space), Run Current (Ctrl+Enter: the statement under the
// cursor, or the selection) and Run All (Ctrl+Shift+Enter), Explain, the history of this favorite, results in tabs with
// a Messages tab, rows and milliseconds, Copy as CSV, JSON or INSERT. Safe mode asks before an UPDATE or DELETE without
// WHERE; a locked connection runs only what reads.
import { View, Text, MenuButton, ConfirmDialog, addListener, useAppWindow, hasTextFocus, ime, clipboard, t as tr } from '@lordos/sdk';
import { useEffect, useReducer, useRef, useState } from 'react';
import { EditorView } from '../editor/EditorView.jsx';
import { Doc, head, hasSel, selStart, selEnd } from '../editor/buffer.js';
import { languageById, paletteFor } from '../editor/syntax.js';
import { usePal, Btn, Foot, FootText, Note, MONO } from './ui.jsx';
import { Grid } from './grid.jsx';
import { run, run1, patchSession, prefs, setPrefs } from './conn.js';
import { viewOf, patchView } from './viewstate.js';
import {
  splitStatements, statementAt, offsetOf, isDangerous, isReadOnlySql, firstWord, wordBefore, completions, allColumnsSql, columnsByTable,
  pushHistory, columnWidths, isNumericColumn, cellText, fmtMs, toCsv, toJson, toInserts, stripComments,
} from './model.js';
import { errText, ago } from './texts.js';

const SQL = languageById('sql');
const MAX_ROWS = 5000;

export function Query({ s, db, tables }) {
  const p = usePal();
  const { focused } = useAppWindow();
  const v = viewOf(s.key);
  if (!v.query) v.query = { doc: new Doc(''), results: [], active: 0, messages: [], summary: null };
  const q = v.query;
  const doc = q.doc;
  const [, bump] = useReducer((x) => x + 1, 0);
  const api = useRef(null);
  const box = useRef(null);
  const [edH, setEdH] = useState(q.height || 300);
  const drag = useRef(null);
  const [running, setRunning] = useState(false);
  const [confirm, setConfirm] = useState(null);     // { list, statements }
  const [comp, setComp] = useState(null);           // { items, i, x, y, from, l }
  const compRef = useRef(null);
  compRef.current = comp;
  const [schema, setSchema] = useState(() => v.schema?.[db] || null);
  const safe = prefs().safeMode !== false;

  // a query handed over from the table list (Query the table)
  useEffect(() => {
    if (v.queryInsert) {
      const text = v.queryInsert;
      patchView(s.key, { queryInsert: null });
      const last = doc.lineCount - 1;
      api.current?.goto({ l: last, c: doc.line(last).length });
      api.current?.insertText(`${doc.text().trim() ? '\n' : ''}${text}`);
    }
  }, []);
  // the schema for completion, once per database
  useEffect(() => {
    if (!db || v.schema?.[db]) return;
    run1(s.key, allColumnsSql(db)).then((r) => {
      const cols = columnsByTable(r);
      v.schema = { ...(v.schema || {}), [db]: cols };
      setSchema(cols);
    }).catch(() => {});
  }, [db]);

  // ---- completion ----
  const compFor = (manual) => {
    if (doc.cursors.length !== 1 || hasSel(doc.cursors[0])) return null;
    const k = head(doc.cursors[0]);
    const line = doc.line(k.l);
    const w = wordBefore(line, k.c);
    if (!manual && !w.qualifier && w.word.length < 2) return null;
    if (!manual && w.qualifier && !w.word && line[k.c - 1] !== '.') return null;
    const sch = { tables, columns: schema || {} };
    let items = completions(w.word, sch, { qualifier: w.qualifier, text: doc.lines.join('\n') });
    if (manual && !w.word && !w.qualifier) items = tables.slice(0, 12).map((x) => ({ label: x, kind: 'table', detail: '' }));
    if (!items.length) return null;
    const c = ime.caret();
    let x = 60, y = 40;
    if (c && box.current) { const [bx, by] = lord.getLayout(box.current.id); x = c.x - bx; y = c.y - by + c.h + 2; }
    return { items, i: 0, x, y, from: w.from, l: k.l };
  };
  const accept = (it) => {
    const c = compRef.current;
    if (!c) return;
    const k = head(doc.cursors[0]);
    doc.cursors = [{ l: k.l, c: k.c, al: k.l, ac: c.from, goal: -1 }];
    api.current?.insertText(it.label);
    setComp(null);
  };
  const onChange = () => {
    bump();
    setTimeout(() => {
      if (!typed.current) { if (compRef.current) setComp(null); return; }
      typed.current = false;
      setComp(compFor(false));
    }, 0);
  };
  const typed = useRef(false);
  const onCommand = (b) => { if (b === ' ') setComp(compFor(true)); };

  // ---- keys: Ctrl+Enter, Ctrl+Shift+Enter, the completion list ----
  const keys = useRef(null);
  useEffect(() => {
    if (!focused) return undefined;
    return addListener('keydown', (e) => keys.current?.(e));
  }, [focused]);
  keys.current = (e) => {
      if (e.down === false || hasTextFocus() || confirm) return;
      const c = compRef.current;
      if ((e.ctrl || e.meta) && e.key === 'Enter') { setComp(null); if (e.shift) runAll(); else runCurrent(); return; }
      if (c) {
        if (e.key === 'ArrowDown') { setComp({ ...c, i: (c.i + 1) % c.items.length }); return; }
        if (e.key === 'ArrowUp') { setComp({ ...c, i: (c.i + c.items.length - 1) % c.items.length }); return; }
        if (e.key === 'Enter' || e.key === 'Tab') { accept(c.items[c.i]); return; }
        if (e.key === 'Escape') { setComp(null); return; }
        if (e.key === 'Backspace') {
          const k = head(doc.cursors[0]);
          if (k.c > 0) { doc.cursors = [{ l: k.l, c: k.c, al: k.l, ac: k.c - 1, goal: -1 }]; api.current?.insertText(''); }
          setComp(compFor(false));
          return;
        }
        if (e.char && !e.ctrl && !e.meta && e.char >= ' ') {
          api.current?.insertText(e.char);
          setComp(/[\w$.]/.test(e.char) ? compFor(false) : null);
          return;
        }
        setComp(null);
        return;
      }
      if (e.char && !e.ctrl && !e.meta && /[\w$.]/.test(e.char)) typed.current = true;
  };

  // ---- the splitter between the editor and the results ----
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      const d = drag.current;
      if (!d) return;
      const h = Math.max(80, Math.min(900, d.h + e.y - d.y));
      setEdH(h); q.height = h;
    });
    const offUp = addListener('pointerup', () => { drag.current = null; });
    return () => { offMove(); offUp(); };
  }, []);

  // ---- running ----
  const textOf = () => doc.lines.join('\n');
  const runCurrent = () => {
    const k = doc.cursors[doc.cursors.length - 1];
    if (hasSel(k)) { const text = doc.textIn(selStart(k), selEnd(k)); start(splitStatements(text).map((x) => x.sql)); return; }
    const st = statementAt(textOf(), offsetOf(doc.lines, head(k)));
    if (st) start([st.sql]);
  };
  const runAll = () => start(splitStatements(textOf()).map((x) => x.sql));
  const explain = () => {
    const k = doc.cursors[doc.cursors.length - 1];
    const st = statementAt(textOf(), offsetOf(doc.lines, head(k)));
    if (st) start([`EXPLAIN ${st.sql}`]);
  };
  const start = (list) => {
    if (!list.length || running) return;
    if (s.closed) { q.messages = [{ error: errText({ code: 'closed' }) }]; q.active = 'messages'; bump(); return; }
    if (s.locked && list.some((x) => !isReadOnlySql(x))) { q.summary = { error: true, text: errText({ code: 'readonly' }) }; q.messages = [{ sql: list.find((x) => !isReadOnlySql(x)), error: errText({ code: 'readonly' }) }]; q.active = 'messages'; bump(); return; }
    const risky = list.filter(isDangerous);
    if (safe && risky.length) { setConfirm({ list, risky }); return; }
    runList(list);
  };
  const runList = async (list) => {
    setConfirm(null);
    setRunning(true);
    const results = [], messages = [];
    let totalMs = 0, failed = false;
    for (const sql of list) {
      try {
        const r = await run(s.key, sql, { src: 'you', maxRows: MAX_ROWS });
        totalMs += r.ms || 0;
        const res = r.results || [];
        res.forEach((x, i) => {
          results.push({ ...x, sql, ms: i === 0 ? r.ms : null });
          messages.push({ sql, ms: i === 0 ? r.ms : null, text: x.columns.length ? tr('databases.q.rowsMsg', { count: x.rows.length }) + (x.truncated ? ` ${tr('databases.q.truncated')}` : '')
            : tr('databases.q.affectedMsg', { count: x.affected ?? 0 }) + (x.insertId ? ` · ${tr('databases.q.insertId', { id: x.insertId })}` : '') + (x.info ? ` · ${x.info}` : ''),
          warnings: x.warnings || 0 });
        });
        if (firstWord(sql) === 'USE') {
          const m = /^\s*USE\s+(`(?:[^`]|``)+`|[^\s;]+)/i.exec(stripComments(sql));
          if (m) patchSession(s.key, { db: m[1].replace(/^`|`$/g, '').replace(/``/g, '`') });
        }
      } catch (e) {
        messages.push({ sql, error: errText(e) });
        failed = true;
        break;
      }
    }
    q.results = results.slice(0, 50);
    q.messages = messages;
    const rowsShown = results.filter((x) => x.columns.length);
    const last = results[results.length - 1];
    q.summary = failed ? { error: true, text: messages[messages.length - 1].error }
      : { error: false, text: last && last.columns.length ? tr('databases.q.rowsIn', { count: last.rows.length, ms: fmtMs(totalMs) }) : tr('databases.q.doneIn', { count: results.length, ms: fmtMs(totalMs) }) };
    q.active = failed ? 'messages' : rowsShown.length ? results.indexOf(rowsShown[0]) : results.length ? 0 : 'messages';
    const fav = s.fav.id;
    const h = prefs().history || {};
    setPrefs({ history: { ...h, [fav]: pushHistory(h[fav], list.join(';\n')) } });
    setRunning(false);
    bump();
  };

  const history = (prefs().history || {})[s.fav.id] || [];
  const tokens = p.dark
    ? { editorBg: p.code, gutterBg: p.gutter, gutterText: '#5c5c66', gutterActive: '#d4d4d8', lineHighlight: 'rgba(10,132,255,0.09)', caret: '#5aa9ff', selection: '#3b4f7a', selectionBlur: '#33333b', match: '#a1803066', matchCurrent: '#d6a12e' }
    : { editorBg: '#ffffff', gutterBg: p.gutter, gutterText: '#a1a1aa', gutterActive: '#3f3f46', lineHighlight: '#f1f5ff', caret: '#2563eb', selection: '#b4d7ff', selectionBlur: '#e4e4e7', match: '#fde68a99', matchCurrent: '#f59e0b' };
  const res = typeof q.active === 'number' ? q.results[q.active] : null;
  const widths = res ? columnWidths(res.columns.map((c) => c.name), res.rows) : [];
  const gcols = res ? res.columns.map((c, i) => ({ key: `${i}`, index: i, label: c.name, width: widths[i], align: isNumericColumn(c) ? 'end' : 'start', mono: isNumericColumn(c) })) : [];
  const cols = res ? res.columns.map((c) => c.name) : [];
  const tabLabel = (r, i) => (r.columns.length ? tr('databases.q.tabRows', { n: i + 1, count: r.rows.length }) : tr('databases.q.tabAffected', { n: i + 1, count: r.affected ?? 0 }));

  return (
    <View style={{ flex: 1, minHeight: 0 }}>
      <View ref={box} style={{ height: edH, flexDirection: 'row' }}>
        <EditorView doc={doc} lang={SQL} t={tokens} palette={paletteFor(p.dark ? 'dark' : 'light')} settings={{ tabSize: 2, useTabs: false }} matches={null}
          onChange={onChange} onCommand={onCommand} apiRef={api} overlay={!!comp || !!confirm} />
        {comp ? (
          <View style={{ position: 'absolute', left: Math.max(4, comp.x), top: comp.y, width: 300, padding: 4, borderRadius: 8, backgroundColor: p.dark ? '#2c2c31' : '#ffffff',
            border: `1px solid ${p.line}`, boxShadow: '0 10px 30px rgba(0,0,0,0.45)', zIndex: 30 }}>
            {comp.items.map((it, i) => (
              <View key={`${it.kind}${it.label}`} onMouseDown={() => accept(it)}
                style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 8, height: 24, borderRadius: 5, backgroundColor: i === comp.i ? p.accent : 'transparent' }}>
                <Text style={{ flex: 1, fontSize: 12, fontFamily: MONO, color: i === comp.i ? '#fff' : p.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{it.label}</Text>
                <Text style={{ fontSize: 11.5, color: i === comp.i ? 'rgba(255,255,255,0.8)' : p.dim, whiteSpace: 'nowrap' }}>{it.detail || tr(`databases.q.kind.${it.kind}`)}</Text>
              </View>
            ))}
          </View>
        ) : null}
      </View>
      <View onMouseDown={(e) => { drag.current = { y: e.y, h: edH }; }} style={{ height: 38, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 10, backgroundColor: p.bar }}>
        <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 1, backgroundColor: p.line }} />
        <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 1, backgroundColor: p.line }} />
        <Btn small primary label={tr('databases.q.runCurrent')} disabled={running} onClick={runCurrent} title="Ctrl+Enter" style={{ height: 26 }} />
        <Text style={{ fontSize: 11, color: p.dim }}>Ctrl+Enter</Text>
        <Btn small label={tr('databases.q.runAll')} disabled={running} onClick={runAll} title="Ctrl+Shift+Enter" style={{ height: 26 }} />
        <Btn small label={tr('databases.q.explain')} disabled={running} onClick={explain} style={{ height: 26 }} />
        <View style={{ width: 1, height: 20, backgroundColor: p.line, marginHorizontal: 2 }} />
        <MenuButton label={tr('databases.q.favorites')} small items={[{ label: tr('databases.later'), disabled: true }]} />
        <MenuButton label={tr('databases.q.history')} small minWidth={360} items={history.length ? history.slice(0, 20).map((h) => ({
          label: h.sql.replace(/\s+/g, ' ').slice(0, 60), detail: ago(h.at), onClick: () => api.current?.insertText(h.sql),
        })) : [{ label: tr('databases.q.noHistory'), disabled: true }]} />
        <View style={{ flex: 1 }} />
        <View onClick={() => setPrefs({ safeMode: !safe })} title={tr('databases.q.safeToggle')}>
          <Text style={{ fontSize: 12, color: safe ? p.dim : p.warn }}>{safe ? tr('databases.q.safeOn') : tr('databases.q.safeOff')}</Text>
        </View>
      </View>
      {/* results tabs */}
      <View style={{ height: 34, flexDirection: 'row', alignItems: 'center', gap: 2, paddingHorizontal: 6, backgroundColor: p.foot }}>
        <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 1, backgroundColor: p.line }} />
        {q.results.slice(0, 12).map((r, i) => <ResTab key={i} label={tabLabel(r, i)} on={q.active === i} onClick={() => { q.active = i; bump(); }} />)}
        <ResTab label={tr('databases.q.messages')} on={q.active === 'messages'} onClick={() => { q.active = 'messages'; bump(); }} />
        {running ? <Text style={{ fontSize: 12, color: p.dim, marginLeft: 8 }}>{tr('databases.q.running')}</Text> : null}
      </View>
      {q.active === 'messages' ? (
        <View style={{ flex: 1, minHeight: 0, paddingHorizontal: 12, paddingVertical: 8, gap: 6 }}>
          {!q.messages.length ? <Text style={{ fontSize: 12.5, color: p.dim, textAlign: 'start' }}>{tr('databases.q.noMessages')}</Text> : null}
          {q.messages.slice(-200).map((m, i) => (
            <View key={i} style={{ gap: 2 }}>
              {m.sql ? <Text style={{ fontSize: 11.5, fontFamily: MONO, color: p.dim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{m.sql.replace(/\s+/g, ' ').slice(0, 160)}</Text> : null}
              <Text style={{ fontSize: 12.5, color: m.error ? p.bad : p.text, textAlign: 'start' }}>
                {m.error || [m.text, m.warnings ? tr('databases.q.warnings', { count: m.warnings }) : '', m.ms != null ? fmtMs(m.ms) : ''].filter(Boolean).join(' · ')}
              </Text>
            </View>
          ))}
        </View>
      ) : res && res.columns.length ? (
        <Grid columns={gcols} rows={res.rows} cell={(r, c) => (r[c.index] === null ? null : cellText(r[c.index]))} emptyText={tr('databases.q.noRows')} />
      ) : res ? (
        <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center' }}>
          <Text style={{ fontSize: 13, color: p.dim }}>{tr('databases.q.affectedMsg', { count: res.affected ?? 0 })}{res.insertId ? ` · ${tr('databases.q.insertId', { id: res.insertId })}` : ''}</Text>
        </View>
      ) : (
        <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center' }}><Text style={{ fontSize: 13, color: p.dim }}>{tr('databases.q.hint')}</Text></View>
      )}
      <Foot style={{ paddingHorizontal: 12 }}>
        {q.summary ? <View style={{ width: 10, height: 10, borderRadius: 5, backgroundColor: q.summary.error ? p.bad : p.ok, marginRight: 6 }} /> : null}
        <FootText style={{ flex: 1 }}>{q.summary ? q.summary.text : s.locked ? tr('databases.lock.footer') : ''}{res?.truncated ? ` · ${tr('databases.q.truncated')}` : ''}</FootText>
        {res && res.columns.length ? (
          <>
            <FootText>{tr('databases.q.copyAs')}</FootText>
            <Btn small label="CSV" style={{ marginLeft: 6 }} onClick={() => clipboard.set(toCsv(cols, res.rows))} />
            <Btn small label="JSON" style={{ marginLeft: 4 }} onClick={() => clipboard.set(toJson(cols, res.rows))} />
            <Btn small label="INSERT" style={{ marginLeft: 4 }} onClick={() => clipboard.set(toInserts(res.columns[0]?.orgTable || res.columns[0]?.table || 'result', cols, res.rows))} />
          </>
        ) : null}
      </Foot>
      {confirm ? (
        <ConfirmDialog title={tr('databases.q.safeTitle')} destructive okLabel={tr('databases.q.runAnyway')} cancelLabel={tr('databases.cancel')}
          detail={`${tr('databases.q.safeDetail', { count: confirm.risky.length })}\n\n${confirm.risky[0].replace(/\s+/g, ' ').slice(0, 200)}`}
          onOk={() => runList(confirm.list)} onCancel={() => setConfirm(null)} />
      ) : null}
    </View>
  );
}

function ResTab({ label, on, onClick }) {
  const p = usePal();
  return (
    <View onClick={onClick} style={{ paddingHorizontal: 10, height: 24, justifyContent: 'center', borderRadius: 6, backgroundColor: on ? p.control : 'transparent' }}>
      <Text style={{ fontSize: 12, color: on ? p.text : p.dim, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}

